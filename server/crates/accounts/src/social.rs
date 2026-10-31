//! The launcher's social features (`docs/social-protocol.md`): who is online, end-to-end
//! encrypted direct messages and room invites.
//!
//! A logged-in launcher holds one `GET /v1/social/stream` open (server-sent events). The stream
//! is the launcher's presence: while it is open the player is listed as online, and every event
//! for the player arrives on it. Sending is ordinary requests (`POST /v1/social/dm`,
//! `POST /v1/social/invite`). One stream per account: a second one (another launcher logged in
//! to the same account) replaces the first, which gets `replaced` and ends.
//!
//! The hub lives in this process's memory, like mm's rooms: accounts runs as one process. A DM is
//! a box sealed to the recipient's session key (the chat crypto of `docs/chat-protocol.md` §4),
//! so accounts relays bytes it can't read. Invites name the inviter's room, which only mm knows:
//! accounts reads where every room player is from mm's status snapshot (`MmStatus::members`), and
//! [`room_watcher`] tells each launcher when its own player's room changes.
//!
//! Events are queued per stream, bounded: a launcher that stops reading is dropped (its stream
//! ends, it reconnects) instead of the queue growing.

use std::collections::HashMap;
use std::convert::Infallible;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll};
use std::time::{Duration, Instant};

use axum::extract::State;
use axum::http::{HeaderMap, HeaderName, HeaderValue, StatusCode};
use axum::response::sse::{Event, KeepAlive, Sse};
use axum::response::IntoResponse;
use axum::Json;
use common::chat::{decode_hex, decode_hex_exact, is_uid, parse_identity_key, verify_kx, MAX_BOX_BYTES, MIN_BOX_BYTES};
use common::ratelimit::{RateLimiter, Window};
use common::rooms::RoomMember;
use serde::{Deserialize, Serialize};
use serde_json::json;
use tokio::sync::mpsc;

use crate::api::{current_user, me};
use crate::error::{ApiError, ApiResult};
use crate::AppState;

/// Events waiting for one launcher. A launcher reads them as they come; one that falls this far
/// behind is dropped.
pub const QUEUE: usize = 256;
/// Launchers connected at once.
pub const MAX_CLIENTS: usize = 5000;
/// A comment line keeps idle streams open through nginx (`proxy_read_timeout 30s`) and
/// Cloudflare (100 s).
pub const KEEP_ALIVE: Duration = Duration::from_secs(15);
/// How often [`room_watcher`] reads mm's status while launchers are connected.
pub const ROOM_POLL: Duration = Duration::from_secs(1);

/// Stream (re)connects per account: a launcher retries with a backoff, so this only stops loops.
const CONNECT_WINDOWS: [Window; 2] =
    [Window::new(10, Duration::from_secs(60)), Window::new(60, Duration::from_secs(3600))];
/// DMs per account, as mm's chat (`docs/chat-protocol.md` §3).
const DM_WINDOWS: [Window; 2] = [Window::new(5, Duration::from_secs(5)), Window::new(30, Duration::from_secs(60))];
/// Invites per account.
const INVITE_WINDOWS: [Window; 2] =
    [Window::new(10, Duration::from_secs(60)), Window::new(60, Duration::from_secs(3600))];
/// How long an invite can be answered.
pub const INVITE_TTL: Duration = Duration::from_secs(120);
/// Invites from one account to another.
const INVITE_PAIR_WINDOWS: [Window; 1] = [Window::new(1, Duration::from_secs(5))];

pub const HEADER_CHAT_KEY: &str = "x-chat-key";
pub const HEADER_CHAT_KX: &str = "x-chat-kx";
pub const HEADER_CHAT_KX_SIG: &str = "x-chat-kx-sig";

/// The group id a launcher signs its session key under (`docs/social-protocol.md` §2).
pub fn session_group(uid: &str) -> String {
    format!("dm-{uid}")
}

/// The group id of a DM conversation: both uids in byte order.
pub fn dm_group(a: &str, b: &str) -> String {
    let (lo, hi) = if a <= b { (a, b) } else { (b, a) };
    format!("dm-{lo}-{hi}")
}

/// A player online, as every launcher sees them.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Member {
    pub uid: String,
    pub display_name: String,
    pub connect_code: String,
    /// The install's Ed25519 identity key (hex).
    pub id_key: String,
    /// This launcher session's X25519 key (hex), signed by `id_key` under [`session_group`].
    pub kx: String,
    pub kx_sig: String,
}

/// The inviter's room, as the invite shows it.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OwnRoom {
    pub code: String,
    pub players: usize,
    pub open_slots: usize,
    pub mode: common::rooms::RoomMode,
    pub status: common::rooms::RoomStatus,
    pub joinable: bool,
}

impl From<&RoomMember> for OwnRoom {
    fn from(m: &RoomMember) -> Self {
        OwnRoom {
            code: m.code.clone(),
            players: m.players,
            open_slots: m.open_slots,
            mode: m.mode,
            status: m.status,
            joinable: m.joinable,
        }
    }
}

struct Client {
    conn: u64,
    member: Member,
    tx: mpsc::Sender<String>,
    room: Option<OwnRoom>,
}

struct Hub {
    clients: HashMap<String, Client>,
    next_conn: u64,
    connects: RateLimiter<String>,
    dms: RateLimiter<String>,
    invites: RateLimiter<String>,
    invite_pairs: RateLimiter<(String, String)>,
    /// The last snapshot's rooms by uid (for a new stream's `welcome`).
    rooms: HashMap<String, OwnRoom>,
    /// Invites waiting for an answer, by (inviter, invitee).
    pending: HashMap<(String, String), Pending>,
}

/// What an invite asks the invitee to join.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum InviteKind {
    /// The inviter's room (the invitee joins it by code).
    Room,
    /// A Direct 1v1: both games search each other's connect code once the invitee accepts.
    Direct,
}

impl InviteKind {
    pub fn as_str(&self) -> &'static str {
        match self {
            InviteKind::Room => "room",
            InviteKind::Direct => "direct",
        }
    }
}

struct Pending {
    kind: InviteKind,
    expires: Instant,
}

/// Why a request was refused.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SocialError {
    Full,
    TooManyConnects(Duration),
    NotConnected,
    Offline,
    ToSelf,
    TooFast(Duration),
    RoomFull,
    AlreadyInRoom,
    NoInvite,
}

impl From<SocialError> for ApiError {
    fn from(e: SocialError) -> Self {
        let rate = |wait: Duration, msg: &str| ApiError::rate_limited(wait).with_message(msg.to_string());
        match e {
            SocialError::Full => {
                ApiError::new(StatusCode::SERVICE_UNAVAILABLE, "social_full", "Too many players online. Try later.")
            }
            SocialError::TooManyConnects(w) => rate(w, "Reconnecting too often. Wait a moment."),
            SocialError::NotConnected => ApiError::conflict("not_connected", "You're not connected. Try again."),
            SocialError::Offline => ApiError::new(StatusCode::NOT_FOUND, "offline", "That player is offline."),
            SocialError::ToSelf => ApiError::bad_request("invalid", "That's you."),
            SocialError::TooFast(w) => rate(w, "You're sending messages too fast."),
            SocialError::RoomFull => ApiError::conflict("room_full", "Your room has no open slot."),
            SocialError::AlreadyInRoom => ApiError::conflict("already_in_room", "That player is already in your room."),
            SocialError::NoInvite => ApiError::new(StatusCode::NOT_FOUND, "no_invite", "That invite has expired."),
        }
    }
}

/// Who is online and their event queues.
pub struct SocialHub {
    hub: Mutex<Hub>,
    limits: bool,
}

impl SocialHub {
    /// `limits: false` turns the rate limits off (tests, `DISABLE_RATE_LIMITS`).
    pub fn new(limits: bool) -> Self {
        SocialHub {
            hub: Mutex::new(Hub {
                clients: HashMap::new(),
                next_conn: 1,
                connects: RateLimiter::new(&CONNECT_WINDOWS),
                dms: RateLimiter::new(&DM_WINDOWS),
                invites: RateLimiter::new(&INVITE_WINDOWS),
                invite_pairs: RateLimiter::new(&INVITE_PAIR_WINDOWS),
                rooms: HashMap::new(),
                pending: HashMap::new(),
            }),
            limits,
        }
    }

    pub fn online_count(&self) -> usize {
        self.hub.lock().unwrap().clients.len()
    }

    pub fn is_empty(&self) -> bool {
        self.online_count() == 0
    }

    /// Registers a launcher: replaces the account's earlier stream, queues `welcome` for this
    /// one and tells everyone else. Returns the stream's id and its queue.
    pub fn connect(&self, member: Member, now: Instant) -> Result<(u64, mpsc::Receiver<String>), SocialError> {
        let mut hub = self.hub.lock().unwrap();
        if self.limits {
            hub.connects.check(&member.uid, now).map_err(SocialError::TooManyConnects)?;
        }
        if let Some(old) = hub.clients.remove(&member.uid) {
            // Its stream ends once the event is read (the sender goes with `old`).
            let _ = old.tx.try_send(json!({"type": "replaced"}).to_string());
        } else if hub.clients.len() >= MAX_CLIENTS {
            return Err(SocialError::Full);
        }
        let conn = hub.next_conn;
        hub.next_conn += 1;
        let (tx, rx) = mpsc::channel(QUEUE);
        let room = hub.rooms.get(&member.uid).cloned();
        let mut online: Vec<&Member> = hub.clients.values().map(|c| &c.member).collect();
        online.sort_by(|a, b| a.display_name.to_lowercase().cmp(&b.display_name.to_lowercase()));
        let welcome = json!({"type": "welcome", "you": member.uid, "online": online, "room": room}).to_string();
        tx.try_send(welcome).expect("a new queue has room");
        let joined = json!({"type": "online", "member": member}).to_string();
        let uid = member.uid.clone();
        Self::broadcast(&mut hub, &uid, &joined);
        hub.clients.insert(uid, Client { conn, member, tx, room });
        Ok((conn, rx))
    }

    /// A stream ended: forgets the launcher unless a newer stream replaced it.
    pub fn disconnect(&self, uid: &str, conn: u64) {
        let mut hub = self.hub.lock().unwrap();
        if hub.clients.get(uid).is_some_and(|c| c.conn == conn) {
            hub.clients.remove(uid);
            Self::broadcast(&mut hub, uid, &json!({"type": "offline", "uid": uid}).to_string());
        }
    }

    /// Queues `event` for everyone but `except`; drops launchers whose queue is full (each drop
    /// is told to the rest in turn).
    fn broadcast(hub: &mut Hub, except: &str, event: &str) {
        let mut dropped = Vec::new();
        for (uid, c) in &hub.clients {
            if uid != except && c.tx.try_send(event.to_string()).is_err() {
                dropped.push(uid.clone());
            }
        }
        for uid in dropped {
            Self::drop_client(hub, &uid);
        }
    }

    fn drop_client(hub: &mut Hub, uid: &str) {
        if hub.clients.remove(uid).is_some() {
            tracing::info!(%uid, "social stream dropped: queue full");
            Self::broadcast(hub, uid, &json!({"type": "offline", "uid": uid}).to_string());
        }
    }

    fn send_to(hub: &mut Hub, uid: &str, event: String) -> bool {
        let Some(c) = hub.clients.get(uid) else { return false };
        if c.tx.try_send(event).is_err() {
            Self::drop_client(hub, uid);
            return false;
        }
        true
    }

    /// Relays a DM box from `from` (connected) to `to` (online): `dm {from, kx, box}`, with the
    /// sender's session key so the recipient can tell which key it was sealed with. A `muted`
    /// sender (`pp-admin mute`) is checked and answered as anyone, and its box goes nowhere.
    pub fn dm(&self, from: &str, to: &str, sealed: &[u8], muted: bool, now: Instant) -> Result<(), SocialError> {
        let mut hub = self.hub.lock().unwrap();
        let kx = hub.clients.get(from).ok_or(SocialError::NotConnected)?.member.kx.clone();
        if from == to {
            return Err(SocialError::ToSelf);
        }
        if !hub.clients.contains_key(to) {
            return Err(SocialError::Offline);
        }
        if self.limits {
            hub.dms.check(&from.to_string(), now).map_err(SocialError::TooFast)?;
        }
        if muted {
            tracing::debug!(%from, "muted sender: DM dropped");
            return Ok(());
        }
        let event = json!({"type": "dm", "from": from, "kx": kx, "box": hex_lower(sealed)}).to_string();
        if Self::send_to(&mut hub, to, event) {
            Ok(())
        } else {
            Err(SocialError::Offline)
        }
    }

    /// Sends an invite from a connected player to an online one. `room` is the inviter's room from
    /// mm's latest snapshot: in a room, the invite is to that room (it needs an empty open slot);
    /// in none, it is a Direct invite naming the inviter's connect code. `to_room` is the
    /// invitee's room code, if any.
    pub fn invite(
        &self,
        from: &str,
        to: &str,
        room: Option<&RoomMember>,
        to_room: Option<&str>,
        now: Instant,
    ) -> Result<InviteKind, SocialError> {
        let mut hub = self.hub.lock().unwrap();
        let inviter = hub.clients.get(from).ok_or(SocialError::NotConnected)?.member.clone();
        if from == to {
            return Err(SocialError::ToSelf);
        }
        if !hub.clients.contains_key(to) {
            return Err(SocialError::Offline);
        }
        if let Some(room) = room {
            if to_room == Some(room.code.as_str()) {
                return Err(SocialError::AlreadyInRoom);
            }
            if !room.joinable {
                return Err(SocialError::RoomFull);
            }
        }
        if self.limits {
            hub.invite_pairs.check(&(from.to_string(), to.to_string()), now).map_err(SocialError::TooFast)?;
            hub.invites.check(&from.to_string(), now).map_err(SocialError::TooFast)?;
        }
        let (kind, event) = match room {
            Some(room) => {
                (InviteKind::Room, json!({"type": "invite", "from": from, "kind": "room", "room": OwnRoom::from(room)}))
            }
            None => (
                InviteKind::Direct,
                json!({"type": "invite", "from": from, "kind": "direct", "connectCode": inviter.connect_code}),
            ),
        };
        hub.pending.retain(|_, p| p.expires > now);
        hub.pending
            .insert((from.to_string(), to.to_string()), Pending { kind: kind.clone(), expires: now + INVITE_TTL });
        if Self::send_to(&mut hub, to, event.to_string()) {
            Ok(kind)
        } else {
            Err(SocialError::Offline)
        }
    }

    /// The invitee's answer to `from`'s invite: `invite-answer {by, kind, accept, connectCode}` to
    /// the inviter (for a Direct invite, its game then searches `connectCode`). An invite is
    /// answered once, within [`INVITE_TTL`].
    pub fn answer(&self, by: &str, from: &str, accept: bool, now: Instant) -> Result<InviteKind, SocialError> {
        let mut hub = self.hub.lock().unwrap();
        let code = hub.clients.get(by).ok_or(SocialError::NotConnected)?.member.connect_code.clone();
        let pending = hub.pending.remove(&(from.to_string(), by.to_string())).ok_or(SocialError::NoInvite)?;
        if pending.expires <= now {
            return Err(SocialError::NoInvite);
        }
        let event = json!({
            "type": "invite-answer", "by": by, "kind": pending.kind.as_str(), "accept": accept, "connectCode": code,
        });
        if Self::send_to(&mut hub, from, event.to_string()) {
            Ok(pending.kind)
        } else {
            Err(SocialError::Offline)
        }
    }

    /// Accounts changed in the database (`common::db::changed_users`). A banned one's stream gets
    /// `banned` and ends, and the rest see it go offline. A renamed one (or one with a new connect code,
    /// the slur filter's) is listed again with the new name (`online`), and its own launcher gets `account` to read `/v1/me` again (its name,
    /// and the `user.json` its game reads). A mute changes nothing here: [`Self::dm`] reads it from
    /// the sender's account with each DM, and the muted player must not be able to tell.
    pub fn accounts_changed(&self, users: &[common::db::MmUser], wall: chrono::DateTime<chrono::Utc>) {
        let mut hub = self.hub.lock().unwrap();
        for u in users {
            let uid = u.uid.to_string();
            if u.is_banned(wall) {
                let Some(old) = hub.clients.remove(&uid) else { continue };
                tracing::info!(%uid, "account banned: social stream ended");
                // Its stream ends once the event is read (the sender goes with `old`).
                let _ = old.tx.try_send(json!({"type": "banned"}).to_string());
                hub.pending.retain(|(from, to), _| *from != uid && *to != uid);
                Self::broadcast(&mut hub, &uid, &json!({"type": "offline", "uid": uid}).to_string());
                continue;
            }
            let code = u.connect_code.clone().unwrap_or_default();
            let Some(c) = hub
                .clients
                .get_mut(&uid)
                .filter(|c| c.member.display_name != u.display_name || c.member.connect_code != code)
            else {
                continue;
            };
            c.member.display_name = u.display_name.clone();
            c.member.connect_code = code;
            let listed = json!({"type": "online", "member": c.member}).to_string();
            Self::broadcast(&mut hub, &uid, &listed);
            Self::send_to(&mut hub, &uid, json!({"type": "account"}).to_string());
        }
    }

    /// Takes mm's latest rooms by uid and sends `room {room}` to every launcher whose player's
    /// room changed (joined, left, or the room's slots or status changed).
    pub fn update_rooms(&self, members: &[RoomMember]) {
        let mut hub = self.hub.lock().unwrap();
        hub.rooms = members.iter().map(|m| (m.uid.clone(), OwnRoom::from(m))).collect();
        let mut changed = Vec::new();
        for (uid, c) in hub.clients.iter() {
            let now = hub.rooms.get(uid);
            if c.room.as_ref() != now {
                changed.push((uid.clone(), now.cloned()));
            }
        }
        for (uid, room) in changed {
            if let Some(c) = hub.clients.get_mut(&uid) {
                c.room = room.clone();
            }
            Self::send_to(&mut hub, &uid, json!({"type": "room", "room": room}).to_string());
        }
    }
}

fn hex_lower(b: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push(DIGITS[(x >> 4) as usize] as char);
        s.push(DIGITS[(x & 15) as usize] as char);
    }
    s
}

/// A stream's events; forgets the launcher when the response is dropped (the client went away
/// or the queue's sender was dropped).
struct SocialStream {
    rx: mpsc::Receiver<String>,
    hub: Arc<SocialHub>,
    uid: String,
    conn: u64,
}

impl futures_core::Stream for SocialStream {
    type Item = Result<Event, Infallible>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.rx.poll_recv(cx).map(|ev| ev.map(|data| Ok(Event::default().data(data))))
    }
}

impl Drop for SocialStream {
    fn drop(&mut self) {
        self.hub.disconnect(&self.uid, self.conn);
    }
}

fn header<'a>(headers: &'a HeaderMap, name: &str) -> Option<&'a str> {
    headers.get(name).and_then(|v| v.to_str().ok()).map(str::trim)
}

/// `GET /v1/social/stream`: session auth, an account that can play online, and the launcher's
/// keys in `X-Chat-Key` (identity), `X-Chat-Kx` (session) and `X-Chat-Kx-Sig`. Server-sent
/// events, one JSON object per `data:` line (`docs/social-protocol.md` §3).
pub async fn stream(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<impl IntoResponse> {
    let user = current_user(&state, &headers).await?;
    let profile = me(&state, &user);
    let Some(code) = profile.connect_code.filter(|_| profile.user_json.is_some()) else {
        return Err(ApiError::forbidden("online_required", "Set up online play first."));
    };
    let uid = user.uid.to_string();
    let bad = || ApiError::bad_request("invalid_keys", "Invalid chat keys. Update the launcher.");
    let id_key = header(&headers, HEADER_CHAT_KEY).and_then(parse_identity_key).ok_or_else(bad)?;
    let kx = header(&headers, HEADER_CHAT_KX).and_then(decode_hex_exact::<32>).ok_or_else(bad)?;
    let kx_sig = header(&headers, HEADER_CHAT_KX_SIG).and_then(decode_hex_exact::<64>).ok_or_else(bad)?;
    if !verify_kx(&id_key, &session_group(&uid), &kx, &kx_sig) {
        return Err(bad());
    }
    let member = Member {
        uid: uid.clone(),
        display_name: user.display_name.clone(),
        connect_code: code,
        id_key: hex_lower(&id_key),
        kx: hex_lower(&kx),
        kx_sig: hex_lower(&kx_sig),
    };
    let (conn, rx) = state.social.connect(member, Instant::now())?;
    let pool = state.pool.clone();
    let account = user.uid;
    tokio::spawn(async move {
        if let Err(e) = common::db::record_chat_key(&pool, account, &id_key).await {
            tracing::error!(uid = %account, "recording chat key failed: {e}");
        }
    });
    let events = SocialStream { rx, hub: state.social.clone(), uid, conn };
    let sse = Sse::new(events).keep_alive(KeepAlive::new().interval(KEEP_ALIVE).text("ping"));
    // nginx passes the events on as they come instead of buffering the response.
    Ok(([(HeaderName::from_static("x-accel-buffering"), HeaderValue::from_static("no"))], sse))
}

#[derive(Debug, Deserialize)]
pub struct DmBody {
    pub to: String,
    #[serde(rename = "box")]
    pub sealed: String,
}

/// `POST /v1/social/dm {to, box}` → 204. The box is `docs/chat-protocol.md` §4's, 114 to 512
/// bytes as hex, sealed to the recipient's session key.
pub async fn dm(State(state): State<AppState>, headers: HeaderMap, Json(body): Json<DmBody>) -> ApiResult<StatusCode> {
    let user = current_user(&state, &headers).await?;
    if !is_uid(&body.to) {
        return Err(ApiError::bad_request("invalid", "Invalid message."));
    }
    let sealed = decode_hex(&body.sealed, MAX_BOX_BYTES)
        .filter(|b| b.len() >= MIN_BOX_BYTES)
        .ok_or_else(|| ApiError::bad_request("invalid", "Invalid message."))?;
    state.social.dm(&user.uid.to_string(), &body.to, &sealed, user.chat_muted_at.is_some(), Instant::now())?;
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Debug, Deserialize)]
pub struct InviteBody {
    pub to: String,
}

/// `POST /v1/social/invite {to}` → `{kind}`: invites an online player to the room the sender is
/// in (`room`, it needs an empty open slot) or, when the sender is in no room, to a Direct 1v1
/// (`direct`).
pub async fn invite(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(body): Json<InviteBody>,
) -> ApiResult<Json<serde_json::Value>> {
    let user = current_user(&state, &headers).await?;
    if !is_uid(&body.to) {
        return Err(ApiError::bad_request("invalid", "Invalid player."));
    }
    let status = state.mm_status.get().await.map_err(|e| {
        tracing::warn!("mm status unavailable: {e}");
        ApiError::new(
            StatusCode::SERVICE_UNAVAILABLE,
            "mm_unavailable",
            "The matchmaking server is not answering. Try again later.",
        )
    })?;
    let from = user.uid.to_string();
    let room = status.members.iter().find(|m| m.uid == from);
    let to_room = status.members.iter().find(|m| m.uid == body.to).map(|m| m.code.as_str());
    let kind = state.social.invite(&from, &body.to, room, to_room, Instant::now())?;
    Ok(Json(json!({"kind": kind.as_str()})))
}

#[derive(Debug, Deserialize)]
pub struct AnswerBody {
    /// The inviter.
    pub from: String,
    pub accept: bool,
}

/// `POST /v1/social/invite/answer {from, accept}` → 204: answers an invite (tells the inviter).
pub async fn answer(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(body): Json<AnswerBody>,
) -> ApiResult<StatusCode> {
    let user = current_user(&state, &headers).await?;
    if !is_uid(&body.from) {
        return Err(ApiError::bad_request("invalid", "Invalid player."));
    }
    state.social.answer(&user.uid.to_string(), &body.from, body.accept, Instant::now())?;
    Ok(StatusCode::NO_CONTENT)
}

/// Reads mm's status every [`ROOM_POLL`] while launchers are connected and passes the rooms to
/// the hub. An unreachable mm changes nothing (the last rooms stay).
/// Reads the accounts that changed every [`common::db::ACCOUNT_POLL`] for
/// [`SocialHub::accounts_changed`] (`pp-admin` bans and renames write only the database).
pub async fn account_watcher(state: AppState) {
    let mut since = chrono::Utc::now() - common::db::ACCOUNT_POLL_OVERLAP;
    loop {
        tokio::time::sleep(common::db::ACCOUNT_POLL).await;
        let read = common::db::changed_users(&state.pool, since);
        match tokio::time::timeout(Duration::from_secs(10), read).await {
            Ok(Ok((users, now))) => {
                since = now - common::db::ACCOUNT_POLL_OVERLAP;
                if !users.is_empty() {
                    state.social.accounts_changed(&users, chrono::Utc::now());
                }
            }
            Ok(Err(e)) => tracing::warn!("reading changed accounts failed: {e}"),
            Err(_) => tracing::warn!("reading changed accounts timed out"),
        }
    }
}

pub async fn room_watcher(state: AppState) {
    let mut failing = false;
    loop {
        tokio::time::sleep(ROOM_POLL).await;
        if state.social.is_empty() {
            continue;
        }
        match state.mm_status.get().await {
            Ok(s) => {
                failing = false;
                state.social.update_rooms(&s.members);
            }
            Err(e) => {
                if !failing {
                    tracing::warn!("mm status unavailable for room updates: {e}");
                }
                failing = true;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use common::rooms::{RoomMode, RoomStatus};

    fn member(n: u8) -> Member {
        let uid = format!(
            "{0}{0}{0}{0}{0}{0}{0}{0}-{0}{0}{0}{0}-4{0}{0}{0}-8{0}{0}{0}-{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}",
            n
        );
        Member {
            uid,
            display_name: format!("p{n}"),
            connect_code: format!("P#{n}"),
            id_key: "00".repeat(32),
            kx: format!("{n:02x}").repeat(32),
            kx_sig: "00".repeat(64),
        }
    }

    fn next(rx: &mut mpsc::Receiver<String>) -> serde_json::Value {
        serde_json::from_str(&rx.try_recv().expect("an event")).unwrap()
    }

    fn account(m: &Member, name: &str, banned: bool) -> common::db::MmUser {
        common::db::MmUser {
            uid: m.uid.parse().unwrap(),
            display_name: name.into(),
            connect_code: Some(m.connect_code.clone()),
            play_key_version: 1,
            banned_until: banned.then(|| chrono::Utc::now() + chrono::Duration::days(1)),
            email_verified: true,
            rating: 0.0,
            ranked_sets: 0,
            unranked_rating: 0.0,
            chat_muted: false,
            room_name: None,
            mods_room_name: None,
            ranked_cooldown_until: None,
        }
    }

    /// A rename is listed to the others and tells the renamed launcher to read its account again;
    /// a ban ends the banned launcher's stream and takes it off the list.
    #[test]
    fn renames_and_bans_reach_the_launchers() {
        let hub = SocialHub::new(true);
        let now = Instant::now();
        let (a, b) = (member(1), member(2));
        let (_, mut ra) = hub.connect(a.clone(), now).unwrap();
        let (_, mut rb) = hub.connect(b.clone(), now).unwrap();
        while ra.try_recv().is_ok() {}
        while rb.try_recv().is_ok() {}
        hub.accounts_changed(&[account(&b, "Bee", false), account(&a, &a.display_name, false)], chrono::Utc::now());
        let ev = next(&mut ra);
        assert_eq!((ev["type"].as_str(), ev["member"]["displayName"].as_str()), (Some("online"), Some("Bee")));
        assert!(ra.try_recv().is_err(), "nothing for an unchanged account");
        assert_eq!(next(&mut rb), json!({"type": "account"}));
        // Seen again by the next look: nothing.
        hub.accounts_changed(&[account(&b, "Bee", false)], chrono::Utc::now());
        assert!(ra.try_recv().is_err() && rb.try_recv().is_err());
        hub.accounts_changed(&[account(&b, "Bee", true)], chrono::Utc::now());
        assert_eq!(next(&mut rb), json!({"type": "banned"}));
        assert!(rb.try_recv().is_err(), "its stream is over");
        assert_eq!(next(&mut ra), json!({"type": "offline", "uid": b.uid}));
        assert_eq!(hub.online_count(), 1);
    }

    /// A muted sender's DM is answered as sent and goes nowhere.
    #[test]
    fn a_muted_senders_dms_go_nowhere() {
        let hub = SocialHub::new(true);
        let now = Instant::now();
        let (a, b) = (member(1), member(2));
        let (_, _ra) = hub.connect(a.clone(), now).unwrap();
        let (_, mut rb) = hub.connect(b.clone(), now).unwrap();
        while rb.try_recv().is_ok() {}
        let sealed = vec![7u8; MIN_BOX_BYTES];
        hub.dm(&a.uid, &b.uid, &sealed, true, now).unwrap();
        assert!(rb.try_recv().is_err());
        assert_eq!(hub.dm(&a.uid, &member(3).uid, &sealed, true, now), Err(SocialError::Offline));
    }

    fn room(uid: &str, code: &str, joinable: bool) -> RoomMember {
        RoomMember {
            uid: uid.into(),
            code: code.into(),
            players: 1,
            open_slots: 2,
            mode: RoomMode::OneVsOne,
            status: RoomStatus::Waiting,
            joinable,
            host: false,
            name: None,
        }
    }

    #[test]
    fn presence_dm_and_replace() {
        let hub = SocialHub::new(true);
        let now = Instant::now();
        let (a, b) = (member(1), member(2));
        let (_, mut ra) = hub.connect(a.clone(), now).unwrap();
        let w = next(&mut ra);
        assert_eq!(w["type"], "welcome");
        assert_eq!(w["online"].as_array().unwrap().len(), 0);
        let (cb, mut rb) = hub.connect(b.clone(), now).unwrap();
        assert_eq!(next(&mut rb)["online"][0]["uid"], a.uid.as_str());
        let ev = next(&mut ra);
        assert_eq!((ev["type"].as_str(), ev["member"]["kx"].as_str()), (Some("online"), Some(b.kx.as_str())));

        let sealed = vec![7u8; MIN_BOX_BYTES];
        hub.dm(&a.uid, &b.uid, &sealed, false, now).unwrap();
        let ev = next(&mut rb);
        assert_eq!(ev["type"], "dm");
        assert_eq!(ev["from"], a.uid.as_str());
        assert_eq!(ev["kx"], a.kx.as_str());
        assert_eq!(ev["box"], "07".repeat(MIN_BOX_BYTES));
        assert_eq!(hub.dm(&a.uid, &a.uid, &sealed, false, now), Err(SocialError::ToSelf));
        assert_eq!(hub.dm(&a.uid, &member(3).uid, &sealed, false, now), Err(SocialError::Offline));
        assert_eq!(hub.dm(&member(3).uid, &a.uid, &sealed, false, now), Err(SocialError::NotConnected));
        for _ in 0..4 {
            hub.dm(&a.uid, &b.uid, &sealed, false, now).unwrap();
        }
        assert!(matches!(hub.dm(&a.uid, &b.uid, &sealed, false, now), Err(SocialError::TooFast(_))));

        // B logs in elsewhere: the old stream gets `replaced` and ends; A sees B's new key only.
        let mut b2 = b.clone();
        b2.kx = "ee".repeat(32);
        let (cb2, mut rb2) = hub.connect(b2, now).unwrap();
        while let Ok(ev) = rb.try_recv() {
            let ev: serde_json::Value = serde_json::from_str(&ev).unwrap();
            if ev["type"] == "replaced" {
                break;
            }
            assert_eq!(ev["type"], "dm");
        }
        assert!(rb.try_recv().is_err(), "the old stream's sender is gone");
        assert_eq!(next(&mut rb2)["type"], "welcome");
        // The old stream's end does not take the new one offline.
        hub.disconnect(&b.uid, cb);
        assert_eq!(hub.online_count(), 2);
        let ev = next(&mut ra);
        assert_eq!(ev["member"]["kx"], "ee".repeat(32));
        assert!(ra.try_recv().is_err());
        hub.disconnect(&b.uid, cb2);
        assert_eq!(next(&mut ra), json!({"type": "offline", "uid": b.uid}));
        assert_eq!(hub.online_count(), 1);
    }

    #[test]
    fn invites_need_a_joinable_room() {
        let hub = SocialHub::new(true);
        let now = Instant::now();
        let (a, b) = (member(1), member(2));
        let (_, mut ra) = hub.connect(a.clone(), now).unwrap();
        let (_, mut rb) = hub.connect(b.clone(), now).unwrap();
        next(&mut ra);
        next(&mut ra);
        next(&mut rb);
        let full = room(&a.uid, "KFQB", false);
        assert_eq!(hub.invite(&a.uid, &b.uid, Some(&full), None, now), Err(SocialError::RoomFull));
        let open = room(&a.uid, "KFQB", true);
        assert_eq!(hub.invite(&a.uid, &b.uid, Some(&open), Some("KFQB"), now), Err(SocialError::AlreadyInRoom));
        assert_eq!(hub.invite(&a.uid, &a.uid, Some(&open), None, now), Err(SocialError::ToSelf));
        assert_eq!(hub.invite(&a.uid, &b.uid, Some(&open), Some("BCDF"), now), Ok(InviteKind::Room));
        let ev = next(&mut rb);
        assert_eq!(ev["type"], "invite");
        assert_eq!(ev["kind"], "room");
        assert_eq!(ev["from"], a.uid.as_str());
        assert_eq!(ev["room"]["code"], "KFQB");
        assert_eq!(ev["room"]["openSlots"], 2);
        assert!(matches!(hub.invite(&a.uid, &b.uid, Some(&open), None, now), Err(SocialError::TooFast(_))));
        hub.invite(&a.uid, &b.uid, Some(&open), None, now + Duration::from_secs(6)).unwrap();
        next(&mut rb);
        // Answered once: B declines, A hears it; a second answer finds nothing.
        hub.answer(&b.uid, &a.uid, false, now).unwrap();
        let ev = next(&mut ra);
        assert_eq!(
            (ev["type"].as_str(), ev["accept"].as_bool(), ev["kind"].as_str()),
            (Some("invite-answer"), Some(false), Some("room"))
        );
        assert_eq!(hub.answer(&b.uid, &a.uid, true, now), Err(SocialError::NoInvite));
    }

    #[test]
    fn direct_invites_outside_rooms() {
        let hub = SocialHub::new(true);
        let now = Instant::now();
        let (a, b) = (member(1), member(2));
        let (_, mut ra) = hub.connect(a.clone(), now).unwrap();
        let (_, mut rb) = hub.connect(b.clone(), now).unwrap();
        next(&mut ra);
        next(&mut ra);
        next(&mut rb);
        assert_eq!(hub.invite(&a.uid, &b.uid, None, Some("KFQB"), now), Ok(InviteKind::Direct));
        let ev = next(&mut rb);
        assert_eq!(ev, json!({"type": "invite", "from": a.uid, "kind": "direct", "connectCode": a.connect_code}));
        assert_eq!(hub.answer(&a.uid, &b.uid, true, now), Err(SocialError::NoInvite), "only the invitee answers");
        hub.answer(&b.uid, &a.uid, true, now).unwrap();
        let ev = next(&mut ra);
        assert_eq!(
            ev,
            json!({"type": "invite-answer", "by": b.uid, "kind": "direct", "accept": true, "connectCode": b.connect_code})
        );
        // Expired invites can't be answered.
        hub.invite(&a.uid, &b.uid, None, None, now + Duration::from_secs(6)).unwrap();
        let late = now + INVITE_TTL + Duration::from_secs(7);
        assert_eq!(hub.answer(&b.uid, &a.uid, true, late), Err(SocialError::NoInvite));
    }

    #[test]
    fn room_changes_reach_their_player() {
        let hub = SocialHub::new(true);
        let now = Instant::now();
        let a = member(1);
        hub.update_rooms(&[room(&a.uid, "KFQB", true)]);
        let (_, mut ra) = hub.connect(a.clone(), now).unwrap();
        assert_eq!(next(&mut ra)["room"]["code"], "KFQB");
        hub.update_rooms(&[room(&a.uid, "KFQB", true)]);
        assert!(ra.try_recv().is_err(), "nothing changed");
        hub.update_rooms(&[room(&a.uid, "KFQB", false)]);
        assert_eq!(next(&mut ra)["room"]["joinable"], false);
        hub.update_rooms(&[]);
        assert_eq!(next(&mut ra), json!({"type": "room", "room": null}));
    }

    #[test]
    fn a_launcher_that_stops_reading_is_dropped() {
        let hub = SocialHub::new(false);
        let now = Instant::now();
        let (a, b) = (member(1), member(2));
        let (_, _ra) = hub.connect(a.clone(), now).unwrap();
        let (_, mut rb) = hub.connect(b.clone(), now).unwrap();
        let sealed = vec![1u8; MIN_BOX_BYTES];
        // A's queue holds `welcome`, `online` and then fills up.
        let mut sent = 0;
        while hub.dm(&b.uid, &a.uid, &sealed, false, now).is_ok() {
            sent += 1;
            assert!(sent <= QUEUE);
        }
        assert_eq!(sent, QUEUE - 2);
        assert_eq!(hub.online_count(), 1);
        next(&mut rb);
        assert_eq!(next(&mut rb), json!({"type": "offline", "uid": a.uid}));
    }

    #[test]
    fn group_ids() {
        let (a, b) = (member(1).uid, member(2).uid);
        assert_eq!(dm_group(&a, &b), dm_group(&b, &a));
        assert!(common::chat::is_group_id(&dm_group(&a, &b)));
        assert_eq!(session_group(&a), format!("dm-{a}"));
    }
}
