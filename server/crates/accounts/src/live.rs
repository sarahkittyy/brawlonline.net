//! Spectating, the launcher's side (`docs/spectate-protocol.md` §3): the live games, a watch
//! stream per spectator, and spectator chat.
//!
//! The games live in mm, which keeps the chunks players' games upload. accounts reads the list
//! from mm's status snapshot (like the public rooms) and, for each spectator, opens mm's loopback
//! `GET /live/{stream}` and passes its lines on as server-sent events, together with the stream's
//! chat. Launchers only ever talk to accounts.
//!
//! **Spectator chat is not end-to-end encrypted** (`docs/design/spectating.md` §6.4): anyone
//! logged in can watch and read it, and pairwise boxes don't scale to a crowd. accounts sees the
//! text, cleans it, rate-limits it and keeps the last [`HISTORY`] lines per stream in memory for
//! late joiners and reports. It is never sent to a game's online connection: **the players of a
//! game never see its spectator chat**, and they may not watch their own game.

use std::collections::{HashMap, VecDeque};
use std::convert::Infallible;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll};
use std::time::{Duration, Instant};

use axum::extract::{Path, Query, State};
use axum::http::{HeaderMap, HeaderName, HeaderValue, StatusCode};
use axum::response::sse::{Event, KeepAlive, Sse};
use axum::response::IntoResponse;
use axum::Json;
use common::ratelimit::{RateLimiter, Window};
use common::spectate::LiveEntry;
use serde::{Deserialize, Serialize};
use serde_json::json;
use tokio::sync::mpsc;
use uuid::Uuid;

use crate::api::{current_user, limit, me};
use crate::error::{ApiError, ApiResult};
use crate::store::UserRow;
use crate::AppState;

/// Events waiting for one spectator. One that falls this far behind is dropped (it reconnects
/// from its newest chunk).
pub const QUEUE: usize = 512;
/// Spectators watching at once, over all streams.
pub const MAX_WATCHERS: usize = 20_000;
/// Chat lines kept per stream.
pub const HISTORY: usize = 200;
/// Chat lines a new spectator gets.
pub const WELCOME_LINES: usize = 20;
/// Lines delivered per stream per second, at most.
pub const DELIVER_PER_SECOND: usize = 5;
/// Context sent with a report: the reported line and this many before it.
pub const REPORT_CONTEXT: usize = 20;
/// How often spectator counts are sent to the spectators (when they changed).
pub const COUNT_EVERY: Duration = Duration::from_secs(5);
/// Spectators named in `welcome` and `spectators` events (the count says how many watch in all).
pub const NAMES_LISTED: usize = 100;
/// A stream's chat is forgotten this long after its last spectator left.
pub const FORGET_AFTER: Duration = Duration::from_secs(600);
/// mm sends an empty line every 15 s; nothing for this long means it is gone.
const MM_SILENCE: Duration = Duration::from_secs(45);

const CHAT_WINDOWS: [Window; 2] = [Window::new(5, Duration::from_secs(5)), Window::new(30, Duration::from_secs(60))];
const REPORT_WINDOWS: [Window; 1] = [Window::new(5, Duration::from_secs(3600))];
const WATCH_WINDOWS: [Window; 1] = [Window::new(30, Duration::from_secs(60))];

/// Slow mode: one line per this long per account, by spectator count.
pub fn slow_mode(spectators: usize) -> Option<Duration> {
    match spectators {
        0..=50 => None,
        51..=200 => Some(Duration::from_secs(3)),
        _ => Some(Duration::from_secs(10)),
    }
}

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ChatLine {
    pub id: u64,
    pub uid: String,
    pub display_name: String,
    pub connect_code: String,
    pub text: String,
    pub at: String,
}

/// Who writes a line: their account's name and code.
#[derive(Debug, Clone)]
pub struct Author {
    pub uid: String,
    pub display_name: String,
    pub connect_code: String,
}

struct Watcher {
    who: Author,
    tx: mpsc::Sender<String>,
}

/// One spectator in the list of who watches (the chat window's eye, `docs/design/spectating.md` §6.4).
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SpectatorName {
    pub uid: String,
    pub display_name: String,
    pub connect_code: String,
}

#[derive(Default)]
struct Room {
    watchers: HashMap<u64, Watcher>,
    lines: VecDeque<ChatLine>,
    next_line: u64,
    /// When the last second's lines went out.
    delivered: VecDeque<Instant>,
    /// Slow mode: each account's last line.
    last_by: HashMap<String, Instant>,
    /// The count last sent to the spectators.
    count_sent: usize,
    /// Who watches changed since the last `spectators` event (someone came or went).
    names_changed: bool,
    empty_since: Option<Instant>,
}

struct Hub {
    rooms: HashMap<String, Room>,
    next_conn: u64,
    watching: usize,
    chat: RateLimiter<String>,
    reports: RateLimiter<String>,
    watches: RateLimiter<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LiveError {
    Full,
    TooManyWatches(Duration),
    NotWatching,
    TooFast(Duration),
    Busy,
    Invalid,
    NoLine,
    TooManyReports(Duration),
}

impl From<LiveError> for ApiError {
    fn from(e: LiveError) -> Self {
        let rate = |wait: Duration, msg: &str| ApiError::rate_limited(wait).with_message(msg.to_string());
        match e {
            LiveError::Full => {
                ApiError::new(StatusCode::SERVICE_UNAVAILABLE, "live_full", "Too many spectators right now. Try later.")
            }
            LiveError::TooManyWatches(w) => rate(w, "Reconnecting too often. Wait a moment."),
            LiveError::NotWatching => ApiError::forbidden("not_watching", "Watch the game to chat."),
            LiveError::TooFast(w) => {
                let mut e = rate(w, "You're sending messages too fast.");
                e.code = "too_fast";
                e
            }
            LiveError::Busy => {
                let mut e = rate(Duration::from_secs(1), "Chat is busy. Try again in a moment.");
                e.code = "busy";
                e
            }
            LiveError::Invalid => ApiError::bad_request("invalid", "Invalid message."),
            LiveError::NoLine => ApiError::new(StatusCode::NOT_FOUND, "no_line", "That message is gone."),
            LiveError::TooManyReports(w) => rate(w, "Too many reports. Try later."),
        }
    }
}

/// A spectator just added by [`LiveHub::watch`].
pub struct Watching {
    pub conn: u64,
    /// For the first event (`welcome`) and the relay's lines.
    pub tx: mpsc::Sender<String>,
    pub rx: mpsc::Receiver<String>,
    /// The stream's last chat lines.
    pub chat: Vec<ChatLine>,
    /// How many watch the stream, this spectator included.
    pub count: usize,
    /// Who watches (this spectator included), at most [`NAMES_LISTED`].
    pub names: Vec<SpectatorName>,
}

/// Every stream's spectators and chat.
pub struct LiveHub {
    hub: Mutex<Hub>,
    limits: bool,
}

/// The stream's spectators in the order they came, each account once, at most [`NAMES_LISTED`].
fn names_of(room: &Room) -> Vec<SpectatorName> {
    let mut conns: Vec<(&u64, &Watcher)> = room.watchers.iter().collect();
    conns.sort_unstable_by_key(|(conn, _)| **conn);
    let mut seen = std::collections::HashSet::new();
    conns
        .into_iter()
        .filter(|(_, w)| seen.insert(w.who.uid.as_str()))
        .take(NAMES_LISTED)
        .map(|(_, w)| SpectatorName {
            uid: w.who.uid.clone(),
            display_name: w.who.display_name.clone(),
            connect_code: w.who.connect_code.clone(),
        })
        .collect()
}

fn now_rfc3339() -> String {
    chrono::Utc::now().to_rfc3339_opts(chrono::SecondsFormat::Millis, true)
}

impl LiveHub {
    /// `limits: false` turns the per-account limits off (tests, `DISABLE_RATE_LIMITS`). The
    /// per-stream delivery cap and slow mode always apply.
    pub fn new(limits: bool) -> Self {
        LiveHub {
            hub: Mutex::new(Hub {
                rooms: HashMap::new(),
                next_conn: 1,
                watching: 0,
                chat: RateLimiter::new(&CHAT_WINDOWS),
                reports: RateLimiter::new(&REPORT_WINDOWS),
                watches: RateLimiter::new(&WATCH_WINDOWS),
            }),
            limits,
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Hub> {
        self.hub.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Adds a spectator: its id, its event queue, the stream's last chat lines, how many watch and who.
    pub fn watch(&self, stream: &str, who: &Author, now: Instant) -> Result<Watching, LiveError> {
        let mut hub = self.lock();
        if self.limits {
            hub.watches.check(&who.uid, now).map_err(LiveError::TooManyWatches)?;
        }
        if hub.watching >= MAX_WATCHERS {
            return Err(LiveError::Full);
        }
        let conn = hub.next_conn;
        hub.next_conn += 1;
        hub.watching += 1;
        let (tx, rx) = mpsc::channel(QUEUE);
        let room = hub.rooms.entry(stream.to_string()).or_default();
        room.watchers.insert(conn, Watcher { who: who.clone(), tx: tx.clone() });
        room.empty_since = None;
        room.names_changed = true;
        let skip = room.lines.len().saturating_sub(WELCOME_LINES);
        let lines = room.lines.iter().skip(skip).cloned().collect();
        Ok(Watching { conn, tx, rx, chat: lines, count: room.watchers.len(), names: names_of(room) })
    }

    /// A spectator's stream ended (both its pump and its response call this; the second does
    /// nothing).
    pub fn unwatch(&self, stream: &str, conn: u64, now: Instant) {
        let mut hub = self.lock();
        let gone = hub.rooms.get_mut(stream).and_then(|r| {
            let w = r.watchers.remove(&conn);
            if w.is_some() {
                r.names_changed = true;
            }
            if r.watchers.is_empty() {
                r.empty_since = Some(now);
            }
            w
        });
        if gone.is_some() {
            hub.watching -= 1;
        }
    }

    pub fn counts(&self) -> HashMap<String, usize> {
        self.lock().rooms.iter().map(|(id, r)| (id.clone(), r.watchers.len())).collect()
    }

    fn deliver(room: &mut Room, event: &str) {
        let mut slow = Vec::new();
        for (conn, w) in &room.watchers {
            if w.tx.try_send(event.to_string()).is_err() {
                slow.push(*conn);
            }
        }
        // A spectator that stopped reading is dropped; its stream ends and it reconnects.
        for conn in slow {
            room.watchers.remove(&conn);
            room.names_changed = true;
        }
    }

    /// A spectator's chat line: checked, cleaned, rate-limited, then sent to every spectator of
    /// the stream (and kept for late joiners and reports). A `muted` author's line (`pp-admin
    /// mute`, a shadowban) goes only to the author's own streams and is not kept.
    pub fn chat(
        &self,
        stream: &str,
        author: &Author,
        text: &str,
        muted: bool,
        now: Instant,
    ) -> Result<ChatLine, LiveError> {
        let text = common::chat::clean_text(text.as_bytes(), 100).ok_or(LiveError::Invalid)?;
        let mut hub = self.lock();
        let Hub { rooms, chat, .. } = &mut *hub;
        let room = rooms.get_mut(stream).ok_or(LiveError::NotWatching)?;
        if !room.watchers.values().any(|w| w.who.uid == author.uid) {
            return Err(LiveError::NotWatching);
        }
        if let Some(gap) = slow_mode(room.watchers.len()) {
            if let Some(last) = room.last_by.get(&author.uid) {
                let since = now.duration_since(*last);
                if since < gap {
                    return Err(LiveError::TooFast(gap - since));
                }
            }
        }
        while room.delivered.front().is_some_and(|t| now.duration_since(*t) >= Duration::from_secs(1)) {
            room.delivered.pop_front();
        }
        if room.delivered.len() >= DELIVER_PER_SECOND {
            return Err(LiveError::Busy);
        }
        if self.limits {
            chat.check(&author.uid, now).map_err(LiveError::TooFast)?;
        }
        room.delivered.push_back(now);
        room.last_by.insert(author.uid.clone(), now);
        room.next_line += 1;
        let line = ChatLine {
            id: room.next_line,
            uid: author.uid.clone(),
            display_name: author.display_name.clone(),
            connect_code: author.connect_code.clone(),
            text,
            at: now_rfc3339(),
        };
        let event = json!({"type": "chat", "line": line}).to_string();
        if muted {
            for w in room.watchers.values().filter(|w| w.who.uid == author.uid) {
                let _ = w.tx.try_send(event.clone());
            }
            return Ok(line);
        }
        if room.lines.len() >= HISTORY {
            room.lines.pop_front();
        }
        room.lines.push_back(line.clone());
        Self::deliver(room, &event);
        Ok(line)
    }

    /// A report of line `id`: the line's author and the line with up to [`REPORT_CONTEXT`] lines
    /// before it (oldest first). Nobody reports their own line.
    pub fn report_lines(
        &self,
        stream: &str,
        id: u64,
        reporter: &str,
        now: Instant,
    ) -> Result<(String, Vec<ChatLine>), LiveError> {
        let mut hub = self.lock();
        let Hub { rooms, reports, .. } = &mut *hub;
        let room = rooms.get(stream).ok_or(LiveError::NoLine)?;
        let at = room.lines.iter().position(|l| l.id == id).ok_or(LiveError::NoLine)?;
        let author = room.lines[at].uid.clone();
        if author == reporter {
            return Err(LiveError::Invalid);
        }
        if self.limits {
            reports.check(&reporter.to_string(), now).map_err(LiveError::TooManyReports)?;
        }
        let from = at.saturating_sub(REPORT_CONTEXT);
        Ok((author, room.lines.iter().skip(from).take(at - from + 1).cloned().collect()))
    }

    /// Sends each stream's spectator count and who watches to its spectators when they changed,
    /// and forgets the chat of streams nobody watched for [`FORGET_AFTER`].
    pub fn tick(&self, now: Instant) {
        let mut hub = self.lock();
        hub.rooms.retain(|_, r| !r.empty_since.is_some_and(|t| now.duration_since(t) >= FORGET_AFTER));
        for room in hub.rooms.values_mut() {
            let n = room.watchers.len();
            if (n != room.count_sent || room.names_changed) && n > 0 {
                room.count_sent = n;
                room.names_changed = false;
                let names = names_of(room);
                Self::deliver(room, &json!({"type": "spectators", "count": n, "names": names}).to_string());
            }
        }
        let watching = hub.rooms.values().map(|r| r.watchers.len()).sum();
        hub.watching = watching;
    }
}

/// One of mm's relay lines as a launcher event: `{"t": "chunk", ...}` → `{"type": "chunk", ...}`.
/// `None` for an empty (keep-alive) line or anything that isn't a JSON object with `t`.
pub fn relay_event(line: &str) -> Option<(String, bool)> {
    let line = line.trim();
    if line.is_empty() {
        return None;
    }
    let mut v: serde_json::Map<String, serde_json::Value> = serde_json::from_str(line).ok()?;
    let t = v.remove("t")?;
    let over = t == "over";
    v.insert("type".into(), t);
    Some((serde_json::Value::Object(v).to_string(), over))
}

fn valid_stream_id(id: &str) -> bool {
    id.len() == 12 && id.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn mm_unavailable(e: impl std::fmt::Display) -> ApiError {
    tracing::warn!("mm unavailable for spectating: {e}");
    ApiError::new(
        StatusCode::SERVICE_UNAVAILABLE,
        "mm_unavailable",
        "The matchmaking server is not answering. Try again later.",
    )
}

fn ended() -> ApiError {
    ApiError::new(StatusCode::NOT_FOUND, "not_found", "This game has ended.")
}

async fn entry(state: &AppState, id: &str) -> ApiResult<LiveEntry> {
    if !valid_stream_id(id) {
        return Err(ended());
    }
    let status = state.mm_status.get().await.map_err(mm_unavailable)?;
    status.live.into_iter().find(|e| e.id == id).ok_or_else(ended)
}

/// `GET /v1/live`: the live games, newest first, each with its spectator count.
pub async fn list(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<serde_json::Value>> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.live.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let status = state.mm_status.get().await.map_err(mm_unavailable)?;
    let counts = state.live.counts();
    let mut games: Vec<LiveEntry> = status
        .live
        .into_iter()
        .map(|mut e| {
            e.spectators = Some(counts.get(&e.id).copied().unwrap_or(0) as u32);
            e
        })
        .collect();
    crate::staging_fake::spectators(&mut games);
    // The list still goes out without them if the ratings can't be read.
    match placed_ratings(&state.pool, &games).await {
        Ok(ratings) => fill_ratings(&mut games, &ratings),
        Err(e) => tracing::warn!("reading the live players' ratings failed: {e}"),
    }
    Ok(Json(json!({ "games": games })))
}

/// A placed player's Ranked rating and rank.
#[derive(Debug, Clone, PartialEq)]
pub struct Placed {
    pub rating: f64,
    pub rank: Option<common::spectate::LiveRank>,
}

/// The Ranked ratings and ranks of the live games' placed players (a player still placing has
/// neither, as on the leaderboard).
async fn placed_ratings(pool: &sqlx::PgPool, games: &[LiveEntry]) -> sqlx::Result<HashMap<Uuid, Placed>> {
    let mut uids: Vec<Uuid> =
        games.iter().flat_map(|g| &g.players).filter_map(|p| Uuid::parse_str(&p.uid).ok()).collect();
    uids.sort_unstable();
    uids.dedup();
    if uids.is_empty() {
        return Ok(HashMap::new());
    }
    let rows: Vec<(Uuid, f64, Option<i16>, Option<f64>)> =
        sqlx::query_as("SELECT uid, rating, rank, rd FROM ratings WHERE uid = ANY($1) AND sets_played >= $2")
            .bind(&uids)
            .bind(common::ranked::PLACEMENT_SETS as i32)
            .fetch_all(pool)
            .await?;
    Ok(rows
        .into_iter()
        .map(|(uid, rating, rank, rd)| {
            let rank = crate::ranked::RankView::of(crate::ranked::stored_tier(rank, rd).rank)
                .map(|r| common::spectate::LiveRank { tier: r.tier.to_string(), division: r.division });
            (uid, Placed { rating, rank })
        })
        .collect())
}

/// Every player gets their rating (none while placing) and their rank (null while placing).
fn fill_ratings(games: &mut [LiveEntry], ratings: &HashMap<Uuid, Placed>) {
    for p in games.iter_mut().flat_map(|g| &mut g.players) {
        let placed = Uuid::parse_str(&p.uid).ok().and_then(|uid| ratings.get(&uid));
        p.rating = placed.map(|r| r.rating);
        p.rank = Some(placed.and_then(|r| r.rank.clone()));
    }
}

#[derive(Debug, Deserialize)]
pub struct WatchQuery {
    pub game: Option<u32>,
    pub chunk: Option<u32>,
}

/// A spectator's events; leaves the stream when the response is dropped.
struct WatchStream {
    rx: mpsc::Receiver<String>,
    hub: Arc<LiveHub>,
    stream: String,
    conn: u64,
}

impl futures_core::Stream for WatchStream {
    type Item = Result<Event, Infallible>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.rx.poll_recv(cx).map(|ev| ev.map(|data| Ok(Event::default().data(data))))
    }
}

impl Drop for WatchStream {
    fn drop(&mut self) {
        self.hub.unwatch(&self.stream, self.conn, Instant::now());
    }
}

/// `GET /v1/live/{id}/watch?game=&chunk=`: server-sent events (`welcome`, mm's relay lines,
/// `chat`, `spectators`). A player of the game may not watch it.
pub async fn watch(
    State(state): State<AppState>,
    Path(id): Path<String>,
    Query(q): Query<WatchQuery>,
    headers: HeaderMap,
) -> ApiResult<impl IntoResponse> {
    let user = current_user(&state, &headers).await?;
    let uid = user.uid.to_string();
    let who = author_of(&state, &user);
    let mut entry = entry(&state, &id).await?;
    if entry.players.iter().any(|p| p.uid == uid) {
        return Err(ApiError::forbidden("playing", "You're playing in this game."));
    }
    let resp = state.mm_status.open_live(&id, q.game, q.chunk).await.map_err(mm_unavailable)?;
    if resp.status() == reqwest::StatusCode::NOT_FOUND {
        return Err(ended());
    }
    if !resp.status().is_success() {
        return Err(mm_unavailable(resp.status()));
    }
    let Watching { conn, tx, rx, chat, count, names } = state.live.watch(&id, &who, Instant::now())?;
    entry.spectators = Some(count as u32);
    let welcome =
        json!({"type": "welcome", "stream": entry, "you": uid, "chat": chat, "spectators": names}).to_string();
    let _ = tx.try_send(welcome);
    tokio::spawn(pump(resp, tx, state.live.clone(), id.clone(), conn));
    let events = WatchStream { rx, hub: state.live.clone(), stream: id, conn };
    let sse = Sse::new(events).keep_alive(KeepAlive::new().interval(crate::social::KEEP_ALIVE).text("ping"));
    Ok(([(HeaderName::from_static("x-accel-buffering"), HeaderValue::from_static("no"))], sse))
}

/// Passes mm's relay lines to one spectator until the stream is over, mm goes away, or the
/// spectator does.
async fn pump(mut resp: reqwest::Response, tx: mpsc::Sender<String>, hub: Arc<LiveHub>, stream: String, conn: u64) {
    let mut buf: Vec<u8> = Vec::new();
    loop {
        let next = match tokio::time::timeout(MM_SILENCE, resp.chunk()).await {
            Ok(Ok(Some(bytes))) => bytes,
            Ok(Ok(None)) | Ok(Err(_)) | Err(_) => break,
        };
        buf.extend_from_slice(&next);
        // A line is at most a chunk's base64 (64 KiB → ~88 KiB): anything far longer is not mm.
        if buf.len() > 1 << 20 {
            tracing::warn!(%stream, "mm relay line too long");
            break;
        }
        let mut over = false;
        while let Some(nl) = buf.iter().position(|b| *b == b'\n') {
            let line: Vec<u8> = buf.drain(..=nl).collect();
            let Ok(text) = std::str::from_utf8(&line) else { continue };
            if let Some((event, is_over)) = relay_event(text) {
                if tx.send(event).await.is_err() {
                    return hub.unwatch(&stream, conn, Instant::now());
                }
                over |= is_over;
            }
        }
        if over {
            break;
        }
    }
    hub.unwatch(&stream, conn, Instant::now());
}

#[derive(Debug, Deserialize)]
pub struct ChatBody {
    pub text: String,
}

/// An account as spectator chat names it.
fn author_of(state: &AppState, user: &UserRow) -> Author {
    let profile = me(state, user);
    Author {
        uid: user.uid.to_string(),
        display_name: user.display_name.clone(),
        connect_code: profile.connect_code.unwrap_or_default(),
    }
}

/// `POST /v1/live/{id}/chat`: a line to the stream's spectators (the sender must be watching).
/// The answer is the line as sent, `{"line": ...}`: the same one the watch streams get.
pub async fn chat(
    State(state): State<AppState>,
    Path(id): Path<String>,
    headers: HeaderMap,
    Json(body): Json<ChatBody>,
) -> ApiResult<Json<serde_json::Value>> {
    let user = current_user(&state, &headers).await?;
    if body.text.len() > 4096 {
        return Err(LiveError::Invalid.into());
    }
    let author = author_of(&state, &user);
    let line = state.live.chat(&id, &author, &body.text, user.chat_muted_at.is_some(), Instant::now())?;
    Ok(Json(json!({ "line": line })))
}

#[derive(Debug, Deserialize)]
pub struct ReportBody {
    pub line: u64,
    #[serde(default)]
    pub reason: String,
}

/// `POST /v1/live/{id}/report`: stores the line and its context from accounts' own copy, so no
/// signatures are needed (`verified` counts every line).
pub async fn report(
    State(state): State<AppState>,
    Path(id): Path<String>,
    headers: HeaderMap,
    Json(body): Json<ReportBody>,
) -> ApiResult<StatusCode> {
    let user = current_user(&state, &headers).await?;
    let (author, lines) = state.live.report_lines(&id, body.line, &user.uid.to_string(), Instant::now())?;
    let reported = uuid::Uuid::parse_str(&author).map_err(|_| LiveError::NoLine)?;
    let messages: Vec<serde_json::Value> = lines
        .iter()
        .map(|l| {
            json!({
                "seq": l.id,
                "text": format!("{} ({}): {}", l.display_name, l.connect_code, l.text),
                "raw": l.text.bytes().map(|b| format!("{b:02x}")).collect::<String>(),
                "sig": "",
                "verified": true,
            })
        })
        .collect();
    let n = messages.len() as i32;
    let row = common::db::NewChatReport {
        reporter: user.uid,
        reported,
        group_id: format!("spectate-{id}"),
        reason: common::chat::clean_text(body.reason.as_bytes(), 200).unwrap_or_default(),
        messages: serde_json::Value::Array(messages),
        verified: n,
        total: n,
    };
    common::db::insert_chat_report(&state.pool, &row).await.map_err(|e| {
        tracing::error!("storing a spectator chat report failed: {e}");
        ApiError::new(StatusCode::SERVICE_UNAVAILABLE, "unavailable", "Couldn't send the report. Try again later.")
    })?;
    tracing::info!(reporter = %user.uid, %reported, stream = %id, "spectator chat report stored");
    Ok(StatusCode::NO_CONTENT)
}

/// Sends spectator counts and forgets old chats, every [`COUNT_EVERY`].
pub async fn ticker(hub: Arc<LiveHub>) {
    loop {
        tokio::time::sleep(COUNT_EVERY).await;
        hub.tick(Instant::now());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn author(n: u8) -> Author {
        Author { uid: format!("u{n}"), display_name: format!("name{n}"), connect_code: format!("C#{n}") }
    }

    fn events(rx: &mut mpsc::Receiver<String>) -> Vec<serde_json::Value> {
        let mut v = Vec::new();
        while let Ok(e) = rx.try_recv() {
            v.push(serde_json::from_str(&e).unwrap());
        }
        v
    }

    #[test]
    fn chat_reaches_every_spectator_of_the_stream_only() {
        let hub = LiveHub::new(true);
        let now = Instant::now();
        let mut a = hub.watch("s1", &author(1), now).unwrap().rx;
        let w = hub.watch("s1", &author(2), now).unwrap();
        let (mut b, n) = (w.rx, w.count);
        let mut other = hub.watch("s2", &author(3), now).unwrap().rx;
        assert_eq!(n, 2);
        hub.chat("s1", &author(1), "  hi\u{200b} there ", false, now).unwrap();
        for rx in [&mut a, &mut b] {
            let ev = events(rx);
            assert_eq!(ev.len(), 1);
            assert_eq!(ev[0]["type"], "chat");
            assert_eq!(ev[0]["line"]["text"], "hi there");
            assert_eq!(ev[0]["line"]["displayName"], "name1");
        }
        assert!(events(&mut other).is_empty());
        // Not watching that stream: refused.
        assert_eq!(hub.chat("s1", &author(3), "x", false, now), Err(LiveError::NotWatching));
        assert_eq!(hub.chat("s1", &author(1), "\u{200b} ", false, now), Err(LiveError::Invalid));
    }

    /// A muted spectator (`pp-admin mute`) sees its own line as sent; nobody else gets it, and
    /// late joiners don't either.
    #[test]
    fn a_muted_spectators_lines_reach_only_itself() {
        let hub = LiveHub::new(true);
        let now = Instant::now();
        let mut a = hub.watch("s1", &author(1), now).unwrap().rx;
        let mut b = hub.watch("s1", &author(2), now).unwrap().rx;
        events(&mut a);
        events(&mut b);
        hub.chat("s1", &author(1), "psst", true, now).unwrap();
        let ev = events(&mut a);
        assert_eq!((ev.len(), ev[0]["line"]["text"].as_str()), (1, Some("psst")));
        assert!(events(&mut b).is_empty());
        let mut c = hub.watch("s1", &author(3), now).unwrap().rx;
        assert!(events(&mut c).iter().all(|e| e["type"] != "chat"));
    }

    #[test]
    fn a_late_joiner_gets_the_last_lines() {
        let hub = LiveHub::new(false);
        let mut now = Instant::now();
        let _w = hub.watch("s", &author(1), now).unwrap();
        for i in 0..30 {
            now += Duration::from_secs(1);
            hub.chat("s", &author(1), &format!("line {i}"), false, now).unwrap();
        }
        let lines = hub.watch("s", &author(2), now).unwrap().chat;
        assert_eq!(lines.len(), WELCOME_LINES);
        assert_eq!(lines.last().unwrap().text, "line 29");
        assert_eq!(lines[0].text, "line 10");
    }

    #[test]
    fn the_stream_cap_and_account_limits() {
        let hub = LiveHub::new(true);
        let now = Instant::now();
        let _w: Vec<_> = (1..=7u8).map(|n| hub.watch("s", &author(n), now).unwrap()).collect();
        for n in 1..=5u8 {
            hub.chat("s", &author(n), "hello", false, now).unwrap();
        }
        assert_eq!(hub.chat("s", &author(6), "hello", false, now), Err(LiveError::Busy));
        let later = now + Duration::from_secs(1);
        hub.chat("s", &author(6), "hello", false, later).unwrap();
        // One account: 5 in 5 s.
        let mut t = later;
        for _ in 0..4 {
            t += Duration::from_millis(250);
            hub.chat("s", &author(7), "x", false, t).unwrap();
        }
        t += Duration::from_millis(250);
        hub.chat("s", &author(7), "x", false, t).unwrap();
        t += Duration::from_millis(250);
        assert!(matches!(hub.chat("s", &author(7), "x", false, t), Err(LiveError::TooFast(_))));
    }

    #[test]
    fn slow_mode_grows_with_the_crowd() {
        assert_eq!(slow_mode(50), None);
        assert_eq!(slow_mode(51), Some(Duration::from_secs(3)));
        assert_eq!(slow_mode(201), Some(Duration::from_secs(10)));
        let hub = LiveHub::new(false);
        let now = Instant::now();
        let mut keep = Vec::new();
        for n in 0..60 {
            keep.push(hub.watch("s", &author(n), now).unwrap());
        }
        let a = Author { uid: "u1".into(), display_name: "a".into(), connect_code: "A#1".into() };
        hub.chat("s", &a, "one", false, now).unwrap();
        assert!(matches!(hub.chat("s", &a, "two", false, now + Duration::from_secs(2)), Err(LiveError::TooFast(_))));
        hub.chat("s", &a, "two", false, now + Duration::from_secs(3)).unwrap();
    }

    #[test]
    fn reports_take_the_line_and_its_context() {
        let hub = LiveHub::new(true);
        let mut now = Instant::now();
        let _w1 = hub.watch("s", &author(1), now).unwrap();
        let _w2 = hub.watch("s", &author(2), now).unwrap();
        let mut last = 0;
        for i in 0..25 {
            now += Duration::from_secs(2);
            last = hub.chat("s", &author(if i % 2 == 0 { 1 } else { 2 }), &format!("m{i}"), false, now).unwrap().id;
        }
        let (who, lines) = hub.report_lines("s", last, "u2", now).unwrap();
        assert_eq!(who, "u1");
        assert_eq!(lines.len(), REPORT_CONTEXT + 1);
        assert_eq!(lines.last().unwrap().text, "m24");
        assert_eq!(hub.report_lines("s", last, "u1", now), Err(LiveError::Invalid), "not your own line");
        assert_eq!(hub.report_lines("s", 999, "u2", now), Err(LiveError::NoLine));
    }

    #[test]
    fn counts_go_out_when_they_change_and_old_chats_are_forgotten() {
        let hub = LiveHub::new(false);
        let now = Instant::now();
        let w1 = hub.watch("s", &author(1), now).unwrap();
        let (c1, mut a) = (w1.conn, w1.rx);
        let w2 = hub.watch("s", &author(2), now).unwrap();
        let c2 = w2.conn;
        hub.tick(now);
        let names = json!([
            {"uid": "u1", "displayName": "name1", "connectCode": "C#1"},
            {"uid": "u2", "displayName": "name2", "connectCode": "C#2"},
        ]);
        assert_eq!(events(&mut a), vec![json!({"type": "spectators", "count": 2, "names": names})]);
        hub.tick(now);
        assert!(events(&mut a).is_empty());
        hub.unwatch("s", c2, now);
        hub.unwatch("s", c2, now);
        assert_eq!(hub.counts()["s"], 1);
        // Someone else came as u2 went: the same count, other names, so it goes out.
        let _w3 = hub.watch("s", &author(3), now).unwrap();
        hub.tick(now);
        let ev = events(&mut a);
        assert_eq!((ev.len(), ev[0]["count"].as_u64()), (1, Some(2)));
        assert_eq!(ev[0]["names"][1]["uid"], "u3");
        hub.unwatch("s", _w3.conn, now);
        hub.unwatch("s", c1, now);
        hub.tick(now + FORGET_AFTER);
        assert!(hub.counts().is_empty());
    }

    /// Who watches: in the order they came, an account with two streams open once, at most
    /// [`NAMES_LISTED`]; the welcome has them too.
    #[test]
    fn spectators_are_named_once_each() {
        let hub = LiveHub::new(false);
        let now = Instant::now();
        let first = hub.watch("s", &author(7), now).unwrap();
        assert_eq!(first.names.len(), 1);
        assert_eq!((first.names[0].uid.as_str(), first.names[0].display_name.as_str()), ("u7", "name7"));
        let again = hub.watch("s", &author(7), now).unwrap();
        assert_eq!((again.count, again.names.len()), (2, 1));
        let mut keep = vec![first, again];
        for n in 0..=200u8 {
            keep.push(hub.watch("s", &author(n), now).unwrap());
        }
        let last = keep.last().unwrap();
        assert_eq!(last.count, 203);
        assert_eq!(last.names.len(), NAMES_LISTED);
        assert_eq!(last.names[0].uid, "u7");
        assert_eq!(last.names[1].uid, "u0");
        // Another stream's spectators are not named here.
        let other = hub.watch("t", &author(9), now).unwrap();
        assert_eq!(other.names.len(), 1);
    }

    #[test]
    fn relay_lines_become_events() {
        assert_eq!(relay_event(""), None);
        assert_eq!(relay_event("\n"), None);
        assert_eq!(relay_event("not json"), None);
        let (e, over) = relay_event(r#"{"t":"chunk","game":1,"i":0,"b":"AA=="}"#).unwrap();
        let v: serde_json::Value = serde_json::from_str(&e).unwrap();
        assert_eq!((v["type"].as_str(), v["i"].as_u64(), over), (Some("chunk"), Some(0), false));
        assert!(relay_event(r#"{"t":"over"}"#).unwrap().1);
        assert!(valid_stream_id("0123456789ab"));
        assert!(!valid_stream_id("0123456789aB"));
        assert!(!valid_stream_id("../status"));
    }

    #[test]
    fn placed_players_get_their_rating_and_the_others_none() {
        use common::spectate::LivePlayer;
        let placed = Uuid::from_bytes([1; 16]);
        let placing = Uuid::from_bytes([2; 16]);
        let player = |uid: String| LivePlayer { uid, rating: Some(1.0), ..Default::default() };
        let mut games = vec![LiveEntry {
            players: vec![player(placed.to_string()), player(placing.to_string()), player("not-a-uid".into())],
            ..Default::default()
        }];
        let gold = common::spectate::LiveRank { tier: "gold".into(), division: Some(2) };
        fill_ratings(&mut games, &HashMap::from([(placed, Placed { rating: 1523.5, rank: Some(gold) })]));
        let ratings: Vec<_> = games[0].players.iter().map(|p| p.rating).collect();
        assert_eq!(ratings, [Some(1523.5), None, None]);
        let json = serde_json::to_value(&games[0].players).unwrap();
        assert_eq!(json[0]["rating"], 1523.5);
        assert_eq!(json[0]["rank"], json!({"tier": "gold", "division": 2}));
        assert!(json[1].get("rating").is_none(), "no rating, no key: {json}");
        assert!(json[1]["rank"].is_null() && json[1].get("rank").is_some(), "placing: rank null: {json}");
        assert!(json[2]["rank"].is_null());
        // mm's entries have no rank key at all.
        let from_mm = serde_json::to_value(LivePlayer::default()).unwrap();
        assert!(from_mm.get("rank").is_none(), "{from_mm}");
    }
}
