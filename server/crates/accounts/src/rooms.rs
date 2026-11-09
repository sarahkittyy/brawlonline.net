//! `GET /v1/rooms`: the players online and the public rooms, for the launcher's Home page.
//!
//! Rooms live in mm's memory, so accounts asks mm: mm publishes a snapshot on a local HTTP
//! listener (`MM_STATUS_LISTEN`, `GET /status`) and accounts reads it from `MM_STATUS_URL`. One
//! read serves every launcher for [`CACHE_FOR`], so launchers polling every few seconds cost mm
//! at most one request a second. Launchers only ever talk to accounts (nginx proxies `/v1/` there
//! already), and mm's listener stays on loopback.

use std::time::{Duration, Instant};

use axum::extract::{Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::Json;
use common::rooms::{clean_room_name, MmStatus, RoomKind, YourRoom};
use serde::{Deserialize, Serialize};
use tokio::sync::Mutex;

use crate::api::{current_user, limit};
use crate::error::{ApiError, ApiResult};
use crate::slur_filter;
use crate::store;
use crate::AppState;

/// How long one snapshot from mm is served.
pub const CACHE_FOR: Duration = Duration::from_secs(1);
/// mm answers from memory; anything slower means it is not there.
const FETCH_TIMEOUT: Duration = Duration::from_secs(2);

/// Reads mm's status, with the cache, and opens its live streams (`GET /live/{stream}` beside
/// `/status`).
pub struct MmStatusSource {
    url: String,
    http: reqwest::Client,
    /// For live streams: no overall timeout (they last as long as the game).
    live_http: reqwest::Client,
    cache: Mutex<Option<(Instant, MmStatus)>>,
    /// The latest snapshot with the players' flags filled in ([`MmStatusSource::get_with_flags`]),
    /// by its `updated_at`: one profiles query per snapshot, not per launcher.
    flagged: Mutex<Option<(String, MmStatus)>>,
}

impl MmStatusSource {
    pub fn new(url: &str) -> Self {
        let http = reqwest::Client::builder().timeout(FETCH_TIMEOUT).build().expect("reqwest client builds");
        let live_http =
            reqwest::Client::builder().connect_timeout(FETCH_TIMEOUT).build().expect("reqwest client builds");
        MmStatusSource { url: url.to_string(), http, live_http, cache: Mutex::new(None), flagged: Mutex::new(None) }
    }

    /// Opens mm's stream of a live game from game `game` (1-based) chunk `chunk` on. The caller
    /// checks the status (404: no such stream).
    pub async fn open_live(
        &self,
        id: &str,
        game: Option<u32>,
        chunk: Option<u32>,
    ) -> anyhow::Result<reqwest::Response> {
        let base = self.url.strip_suffix("/status").unwrap_or(&self.url);
        let mut url = format!("{base}/live/{id}?game={}", game.unwrap_or(1));
        if let Some(c) = chunk {
            url.push_str(&format!("&chunk={c}"));
        }
        // Only the answer's headers are timed: the body lasts as long as the game.
        Ok(tokio::time::timeout(FETCH_TIMEOUT, self.live_http.get(url).send()).await??)
    }

    pub async fn get(&self) -> anyhow::Result<MmStatus> {
        let mut cache = self.cache.lock().await;
        if let Some((at, s)) = cache.as_ref() {
            if at.elapsed() < CACHE_FOR {
                return Ok(s.clone());
            }
        }
        let status: MmStatus = self.http.get(&self.url).send().await?.error_for_status()?.json().await?;
        *cache = Some((Instant::now(), status.clone()));
        Ok(status)
    }
}

impl MmStatusSource {
    /// [`MmStatusSource::get`] with each room player's flag (`countries`, by slot) and each Brawl
    /// Mods room host's (`hostCountry`), for those who show it beside their name.
    pub async fn get_with_flags(&self, pool: &sqlx::PgPool) -> anyhow::Result<MmStatus> {
        let status = self.get().await?;
        let mut flagged = self.flagged.lock().await;
        if let Some((at, s)) = flagged.as_ref() {
            if *at == status.updated_at {
                return Ok(s.clone());
            }
        }
        let s = with_flags(pool, status).await?;
        *flagged = Some((s.updated_at.clone(), s.clone()));
        Ok(s)
    }
}

/// Fills in the room players' and Brawl Mods hosts' flags.
async fn with_flags(pool: &sqlx::PgPool, mut s: MmStatus) -> sqlx::Result<MmStatus> {
    let parse = |u: &String| uuid::Uuid::parse_str(u).ok();
    let mut uids: Vec<uuid::Uuid> = s.rooms.iter().flat_map(|r| r.uids.iter().filter_map(parse)).collect();
    uids.extend(s.mod_rooms.iter().filter_map(|r| parse(&r.host_uid)));
    uids.sort();
    uids.dedup();
    let tags = crate::profile::name_tags(pool, &uids).await?;
    let flag = |u: &String| parse(u).and_then(|u| tags.get(&u)).and_then(|t| t.country.clone());
    for r in &mut s.rooms {
        r.countries = r.uids.iter().map(flag).collect();
    }
    for r in &mut s.mod_rooms {
        r.host_country = flag(&r.host_uid);
    }
    Ok(s)
}

/// `GET /v1/rooms`: `{online, rooms: [{code, host, players, openSlots, mode, status, names,
/// joinable, name}], updatedAt, yourRoom}`. Session auth (the list names players); limited per
/// account ([`common::ratelimit::ROOMS_WINDOWS`]). 503 `mm_unavailable` when mm does not answer.
/// `yourRoom`: the room the caller is in, P+ or Brawl Mods, private ones too (their launcher pins
/// it and lets its host name it).
pub async fn rooms(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<MmStatus>> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.rooms.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    match state.mm_status.get_with_flags(&state.pool).await {
        // Where each player is stays between mm and accounts (it names private rooms), as does
        // the index of every Brawl Mods room.
        Ok(s) => {
            let your_room = your_room(&s, &user.uid.to_string());
            Ok(Json(MmStatus {
                members: Vec::new(),
                mod_room_index: Vec::new(),
                mod_members: Vec::new(),
                your_room,
                ..s
            }))
        }
        Err(e) => {
            tracing::warn!("mm status unavailable: {e}");
            Err(ApiError::new(
                StatusCode::SERVICE_UNAVAILABLE,
                "mm_unavailable",
                "The matchmaking server is not answering. Try again later.",
            ))
        }
    }
}

/// `GET /v1/mod-rooms/{code}`: the Brawl Mods room with this exact code, private ones too
/// (`{code, build: {hash, name}, hostName, players, open}`), so a launcher joining by code starts
/// the room's game; 404 `unknown_room`. Session auth, the same limit as `GET /v1/rooms`. Only a
/// player who has the code learns the room's game.
pub async fn mod_room(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(code): Path<String>,
) -> ApiResult<Json<common::mods::PublicModRoom>> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.rooms.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let code = code.trim().to_ascii_uppercase();
    let status = state.mm_status.get().await.map_err(|e| {
        tracing::warn!("mm status unavailable: {e}");
        ApiError::new(
            StatusCode::SERVICE_UNAVAILABLE,
            "mm_unavailable",
            "The matchmaking server is not answering. Try again later.",
        )
    })?;
    status
        .mod_room_index
        .into_iter()
        .find(|r| r.code == code)
        .map(Json)
        .ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "unknown_room", "Room not found."))
}

/// The room `uid` is in, from mm's internal member lists.
fn your_room(s: &MmStatus, uid: &str) -> Option<YourRoom> {
    if let Some(m) = s.members.iter().find(|m| m.uid == uid) {
        // A 2v2 party (2026-11-02): the launcher's envelope invites to it, and it pins no room.
        let kind = if m.twos { RoomKind::Twos } else { RoomKind::Pplus };
        return Some(YourRoom { kind, code: m.code.clone(), host: m.host, name: m.name.clone() });
    }
    s.mod_members.iter().find(|m| m.uid == uid).map(|m| YourRoom {
        kind: RoomKind::Mods,
        code: m.code.clone(),
        host: m.host,
        name: m.name.clone(),
    })
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SetRoomNameReq {
    /// P+ rooms or Brawl Mods rooms: each has its own name.
    pub kind: RoomKind,
    /// As typed; empty or null: no name.
    pub name: Option<String>,
}

#[derive(Debug, Serialize)]
pub struct RoomNameView {
    pub kind: RoomKind,
    /// What was stored: the name cleaned up, or a kind one if it was a slur.
    pub name: Option<String>,
}

/// `PUT /v1/me/room-name` `{kind, name}`: the name of the rooms the account hosts
/// (`docs/design/rooms.md` §1.4), kept for its next rooms of that kind. mm renames the room it
/// hosts now within a few seconds (its account poll). A slur is replaced by a kind name at
/// once (the answer says what was stored). 400 `invalid_room_name`; limited per account
/// ([`common::ratelimit::ROOM_NAME_WINDOWS`]).
pub async fn set_room_name(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<SetRoomNameReq>,
) -> ApiResult<Json<RoomNameView>> {
    let user = current_user(&state, &headers).await?;
    if req.kind == RoomKind::Twos {
        return Err(ApiError::bad_request("invalid_room_name", "2v2 parties have no name."));
    }
    let name = clean_room_name(req.name.as_deref().unwrap_or_default())
        .map_err(|e| ApiError::bad_request("invalid_room_name", e))?;
    limit(&state, |l, now| l.room_name.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many name changes. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let typed = name.clone();
    let (name, recheck) = match name {
        Some(n) => {
            let (n, recheck) = slur_filter::screen_room_name(&state, &n).await;
            (Some(n), recheck)
        }
        None => (None, false),
    };
    store::set_room_name(&state.pool, user.uid, req.kind, name.as_deref()).await?;
    if let (Some(from), Some(to)) = (&typed, &name) {
        if from != to {
            slur_filter::audit_screened(&state, "user.room-name", user.uid, from, to).await;
        }
    }
    if let (true, Some(n)) = (recheck, &name) {
        slur_filter::recheck_room_name(&state, user.uid, req.kind, n.clone());
    }
    Ok(Json(RoomNameView { kind: req.kind, name }))
}
