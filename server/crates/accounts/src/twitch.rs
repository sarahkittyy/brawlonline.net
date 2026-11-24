//! Twitch (docs/design/twitch-live.md): players connect their Twitch account through Twitch's own
//! sign-in, and a connected player who streams while they play Brawl Online shows as live around the
//! launcher (the online list, the hover card, the profile, Spectate's Twitch row).
//!
//! **Connecting** is OAuth's authorization code flow without any scope: the launcher asks
//! `POST /v1/me/twitch/connect` for an address and opens it in the browser; Twitch sends the player
//! back to `GET /v1/twitch/callback` with a code and the `state` that names the account; accounts
//! trades the code for a token, reads who signed in (`/helix/users`), revokes the token and keeps
//! the Twitch user id and login ([`store_link`]). Nothing of the player's Twitch account is kept but
//! that. The launcher sees it in `GET /v1/me/twitch`. A Twitch handle typed into the profile is a
//! link only: anyone can type anyone's.
//!
//! **Live** is Twitch's and ours together: [`poller`] notes every few seconds who is in a room, a
//! queue, a match or a live game (mm's status), and once a minute asks Twitch (`/helix/streams`,
//! with an app token) which of the connected accounts seen playing in the last
//! [`ACTIVE_WINDOW`] are live. Twitch's category is not looked at: people stream Brawl Online under
//! Brawl, Melee, Project+ or anything else. The result, most viewers first, is
//! `GET /v1/twitch/streams`.
//!
//! Without `TWITCH_CLIENT_ID` and `TWITCH_CLIENT_SECRET` it is all off (no streams; connecting
//! answers 503), except on a staging server with `STAGING_FAKE_STATE`: there the file's fake streams
//! are listed and connecting links the account's typed handle without Twitch
//! ([`crate::staging_fake`]).

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex, RwLock};
use std::time::{Duration, Instant};

use axum::extract::{Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::Html;
use axum::Json;
use rand::RngCore;
use serde::{Deserialize, Serialize};
use serde_json::json;
use sqlx::PgPool;
use uuid::Uuid;

use crate::api::{current_user, limit};
use crate::error::{ApiError, ApiResult};
use crate::AppState;

/// How long after a player was last seen playing a live stream of theirs still counts (between
/// sets, in the menus, looking for the next room).
pub const ACTIVE_WINDOW: Duration = Duration::from_secs(10 * 60);
/// How often mm's status is read for who is playing.
const ACTIVITY_TICK: Duration = Duration::from_secs(10);
/// How long a connect's `state` waits for Twitch to send the player back.
const CONNECT_TTL: Duration = Duration::from_secs(15 * 60);
/// Pending connects kept at most (each is a few dozen bytes; old ones go first).
const MAX_CONNECTS: usize = 10_000;
/// One Twitch call's time limit.
const HTTP_TIMEOUT: Duration = Duration::from_secs(10);
/// Helix takes at most this many `user_id`s per `/streams` call.
const IDS_PER_CALL: usize = 100;
/// The thumbnail size asked of Twitch's template (16:9, twice the launcher's largest card).
const THUMB_W: u32 = 440;
const THUMB_H: u32 = 248;

/// A live stream as the launcher gets it (`GET /v1/twitch/streams`).
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LiveStream {
    /// The player's account.
    pub uid: String,
    pub display_name: String,
    pub connect_code: Option<String>,
    /// Their primary main, when they show it beside their name.
    pub main: Option<String>,
    /// twitch.tv/<login>.
    pub login: String,
    /// Their Twitch display name.
    pub twitch_name: String,
    pub title: String,
    /// Twitch's category ("Super Smash Bros. Brawl").
    pub game: String,
    pub viewers: u32,
    /// The stream's picture (an https address; Twitch refreshes it every few minutes).
    pub thumbnail: String,
    /// RFC 3339.
    pub started_at: String,
}

/// A stream Twitch (or the staging file) says is live, before it is matched to its player.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct HelixStream {
    #[serde(default)]
    pub user_id: String,
    #[serde(default)]
    pub user_login: String,
    #[serde(default)]
    pub user_name: String,
    #[serde(default)]
    pub game_name: String,
    #[serde(default)]
    pub title: String,
    #[serde(default)]
    pub viewer_count: u32,
    #[serde(default)]
    pub started_at: String,
    #[serde(default)]
    pub thumbnail_url: String,
}

#[derive(Debug, Deserialize)]
struct HelixList<T> {
    #[serde(default = "Vec::new")]
    data: Vec<T>,
}

#[derive(Debug, Deserialize)]
struct HelixUser {
    id: String,
    login: String,
    display_name: String,
}

#[derive(Debug, Deserialize)]
struct TokenAnswer {
    access_token: String,
    #[serde(default)]
    expires_in: u64,
}

/// The Twitch state of one accounts process.
pub struct TwitchHub {
    http: reqwest::Client,
    /// A connect's `state` -> the account and when it was asked.
    connects: Mutex<HashMap<String, (Uuid, Instant)>>,
    /// The app access token for `/helix/streams` and when it runs out.
    app_token: tokio::sync::Mutex<Option<(String, Instant)>>,
    /// The latest live streams, most viewers first.
    streams: RwLock<Arc<Vec<LiveStream>>>,
    /// Who was last seen playing when (uid -> time).
    active: Mutex<HashMap<String, Instant>>,
}

impl Default for TwitchHub {
    fn default() -> Self {
        TwitchHub {
            http: reqwest::Client::builder().timeout(HTTP_TIMEOUT).build().unwrap_or_default(),
            connects: Mutex::new(HashMap::new()),
            app_token: tokio::sync::Mutex::new(None),
            streams: RwLock::new(Arc::new(Vec::new())),
            active: Mutex::new(HashMap::new()),
        }
    }
}

impl TwitchHub {
    pub fn streams(&self) -> Arc<Vec<LiveStream>> {
        self.streams.read().unwrap().clone()
    }

    /// Notes `uids` as playing now and forgets whoever hasn't been for [`ACTIVE_WINDOW`].
    pub fn saw_playing(&self, uids: impl IntoIterator<Item = String>, now: Instant) {
        let mut active = self.active.lock().unwrap();
        for uid in uids {
            active.insert(uid, now);
        }
        active.retain(|_, at| now.duration_since(*at) < ACTIVE_WINDOW);
    }

    /// The accounts seen playing in the last [`ACTIVE_WINDOW`].
    pub fn recently_playing(&self, now: Instant) -> HashSet<String> {
        let active = self.active.lock().unwrap();
        active.iter().filter(|(_, at)| now.duration_since(**at) < ACTIVE_WINDOW).map(|(u, _)| u.clone()).collect()
    }

    /// A new connect for `uid`: its `state`.
    fn start_connect(&self, uid: Uuid, now: Instant) -> String {
        let mut bytes = [0u8; 24];
        rand::thread_rng().fill_bytes(&mut bytes);
        let state = hex::encode(bytes);
        let mut connects = self.connects.lock().unwrap();
        connects.retain(|_, (_, at)| now.duration_since(*at) < CONNECT_TTL);
        if connects.len() >= MAX_CONNECTS {
            if let Some(oldest) = connects.iter().min_by_key(|(_, (_, at))| *at).map(|(k, _)| k.clone()) {
                connects.remove(&oldest);
            }
        }
        connects.insert(state.clone(), (uid, now));
        state
    }

    /// The account a callback's `state` belongs to (once: the state is used up).
    fn finish_connect(&self, state: &str, now: Instant) -> Option<Uuid> {
        let (uid, at) = self.connects.lock().unwrap().remove(state)?;
        (now.duration_since(at) < CONNECT_TTL).then_some(uid)
    }
}

/// Whether this server talks to Twitch (both keys set).
pub fn enabled(state: &AppState) -> bool {
    let t = &state.cfg.twitch;
    t.twitch_client_id.as_deref().is_some_and(|s| !s.is_empty())
        && t.twitch_client_secret.as_ref().is_some_and(|s| !s.is_empty())
}

/// Connecting without Twitch: only a staging server with its fake state, never production.
fn fake_connects(state: &AppState) -> bool {
    !enabled(state) && crate::staging_fake::active()
}

fn redirect_uri(state: &AppState) -> String {
    format!("{}/v1/twitch/callback", state.cfg.public_base_url.trim_end_matches('/'))
}

/// `GET /v1/me/twitch`: the account's connected Twitch account, its profile's typed Twitch handle
/// (the launcher's hint to connect: a handle, no connection), and whether this server can connect.
pub async fn me(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<serde_json::Value>> {
    let user = current_user(&state, &headers).await?;
    let linked: Option<(String, String)> =
        sqlx::query_as("SELECT login, display_name FROM twitch_links WHERE uid = $1")
            .bind(user.uid)
            .fetch_optional(&state.pool)
            .await?;
    let handle: Option<String> = sqlx::query_scalar("SELECT twitch FROM profiles WHERE uid = $1")
        .bind(user.uid)
        .fetch_optional(&state.pool)
        .await?
        .flatten();
    Ok(Json(json!({
        "available": enabled(&state) || fake_connects(&state),
        "linked": linked.map(|(login, name)| json!({"login": login, "displayName": name})),
        "handle": handle,
    })))
}

/// `POST /v1/me/twitch/connect`: the Twitch sign-in address for the launcher to open.
pub async fn connect(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<serde_json::Value>> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.profile_edit.check(&user.uid, now)).map_err(ApiError::rate_limited)?;
    let now = Instant::now();
    if fake_connects(&state) {
        let s = state.twitch.start_connect(user.uid, now);
        let url = reqwest::Url::parse_with_params(&redirect_uri(&state), &[("code", "staging"), ("state", &s)])
            .map_err(ApiError::internal)?;
        return Ok(Json(json!({"url": url.as_str()})));
    }
    if !enabled(&state) {
        return Err(ApiError::new(
            StatusCode::SERVICE_UNAVAILABLE,
            "twitch_off",
            "Twitch isn't set up on this server.",
        ));
    }
    let s = state.twitch.start_connect(user.uid, now);
    let t = &state.cfg.twitch;
    let url = reqwest::Url::parse_with_params(
        &format!("{}/authorize", t.twitch_auth_url.trim_end_matches('/')),
        &[
            ("client_id", t.twitch_client_id.as_deref().unwrap_or("")),
            ("redirect_uri", &redirect_uri(&state)),
            ("response_type", "code"),
            ("scope", ""),
            ("state", &s),
            // The player picks the account, rather than whichever one the browser is signed in to.
            ("force_verify", "true"),
        ],
    )
    .map_err(ApiError::internal)?;
    Ok(Json(json!({"url": url.as_str()})))
}

/// `DELETE /v1/me/twitch`: the account's Twitch account is no longer connected (the profile's
/// typed handle stays).
pub async fn disconnect(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<StatusCode> {
    let user = current_user(&state, &headers).await?;
    sqlx::query("DELETE FROM twitch_links WHERE uid = $1").bind(user.uid).execute(&state.pool).await?;
    let uid = user.uid.to_string();
    let mut streams = state.twitch.streams.write().unwrap();
    if streams.iter().any(|s| s.uid == uid) {
        *streams = Arc::new(streams.iter().filter(|s| s.uid != uid).cloned().collect());
    }
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Debug, Deserialize)]
pub struct CallbackQuery {
    #[serde(default)]
    code: String,
    #[serde(default)]
    state: String,
    #[serde(default)]
    error: String,
}

/// `GET /v1/twitch/callback`: where Twitch sends the player back after its sign-in. A plain page;
/// the launcher notices the connection by itself.
pub async fn callback(State(state): State<AppState>, Query(q): Query<CallbackQuery>) -> (StatusCode, Html<String>) {
    const TITLE: &str = "Twitch";
    let failed = |status: StatusCode| (status, crate::pages::page(TITLE, "<p>Not connected.</p>"));
    let Some(uid) = state.twitch.finish_connect(&q.state, Instant::now()) else {
        return failed(StatusCode::BAD_REQUEST);
    };
    if !q.error.is_empty() || q.code.is_empty() {
        return failed(StatusCode::OK);
    }
    let who =
        if fake_connects(&state) { fake_user(&state.pool, uid).await } else { sign_in_user(&state, &q.code).await };
    let who = match who {
        Ok(w) => w,
        Err(e) => {
            tracing::warn!(%uid, "twitch connect failed: {e:#}");
            return failed(StatusCode::BAD_GATEWAY);
        }
    };
    if let Err(e) = store_link(&state.pool, uid, &who).await {
        tracing::warn!(%uid, "storing the twitch link failed: {e}");
        return failed(StatusCode::INTERNAL_SERVER_ERROR);
    }
    tracing::info!(%uid, twitch = %who.login, "twitch connected");
    (StatusCode::OK, crate::pages::page(TITLE, "<p>You're connected!</p>"))
}

/// The Twitch account that signed in: the code traded for a token, `/helix/users`, the token
/// revoked (it is never kept).
async fn sign_in_user(state: &AppState, code: &str) -> anyhow::Result<HelixUser> {
    let t = &state.cfg.twitch;
    let client_id = t.twitch_client_id.as_deref().unwrap_or("");
    let auth = t.twitch_auth_url.trim_end_matches('/');
    let http = &state.twitch.http;
    let token: TokenAnswer = http
        .post(format!("{auth}/token"))
        .form(&[
            ("client_id", client_id),
            ("client_secret", t.twitch_client_secret.as_ref().map(|s| s.expose()).unwrap_or("")),
            ("code", code),
            ("grant_type", "authorization_code"),
            ("redirect_uri", &redirect_uri(state)),
        ])
        .send()
        .await?
        .error_for_status()?
        .json()
        .await?;
    let users: Result<HelixList<HelixUser>, reqwest::Error> = async {
        http.get(format!("{}/users", t.twitch_api_url.trim_end_matches('/')))
            .header("Client-Id", client_id)
            .bearer_auth(&token.access_token)
            .send()
            .await?
            .error_for_status()?
            .json()
            .await
    }
    .await;
    let _ = http
        .post(format!("{auth}/revoke"))
        .form(&[("client_id", client_id), ("token", &token.access_token)])
        .send()
        .await;
    users?.data.into_iter().next().ok_or_else(|| anyhow::anyhow!("twitch answered no user"))
}

/// Staging's stand-in for Twitch's sign-in: the profile's typed handle (or the display name) as
/// the Twitch account.
async fn fake_user(pool: &PgPool, uid: Uuid) -> anyhow::Result<HelixUser> {
    let (name, handle): (String, Option<String>) = sqlx::query_as(
        "SELECT u.display_name, p.twitch FROM users u LEFT JOIN profiles p ON p.uid = u.uid WHERE u.uid = $1",
    )
    .bind(uid)
    .fetch_one(pool)
    .await?;
    let login = handle
        .unwrap_or_else(|| name.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '_').collect())
        .to_lowercase();
    Ok(HelixUser { id: format!("staging-{uid}"), login: login.clone(), display_name: login })
}

/// Keeps `uid`'s Twitch account (moved from another Brawl Online account that had it) and puts its
/// login in the profile's Twitch handle, so the profile links the account that was signed in to.
async fn store_link(pool: &PgPool, uid: Uuid, who: &HelixUser) -> sqlx::Result<()> {
    let mut tx = pool.begin().await?;
    sqlx::query("DELETE FROM twitch_links WHERE twitch_id = $1 AND uid <> $2")
        .bind(&who.id)
        .bind(uid)
        .execute(&mut *tx)
        .await?;
    sqlx::query(
        "INSERT INTO twitch_links (uid, twitch_id, login, display_name) VALUES ($1, $2, $3, $4)
         ON CONFLICT (uid) DO UPDATE SET twitch_id = $2, login = $3, display_name = $4, linked_at = now()",
    )
    .bind(uid)
    .bind(&who.id)
    .bind(&who.login)
    .bind(&who.display_name)
    .execute(&mut *tx)
    .await?;
    sqlx::query(
        "INSERT INTO profiles (uid, twitch) VALUES ($1, $2)
         ON CONFLICT (uid) DO UPDATE SET twitch = $2, updated_at = now()",
    )
    .bind(uid)
    .bind(&who.login)
    .execute(&mut *tx)
    .await?;
    tx.commit().await
}

/// `GET /v1/twitch/streams`: the live streams of players playing Brawl Online, most viewers first.
pub async fn streams(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<serde_json::Value>> {
    current_user(&state, &headers).await?;
    Ok(Json(json!({"streams": *state.twitch.streams()})))
}

/// Who mm says is playing: in a room (P+ or Brawl Mods), searching, in a match, in a live game.
fn playing(s: &common::rooms::MmStatus) -> impl Iterator<Item = String> + '_ {
    s.members
        .iter()
        .map(|m| m.uid.clone())
        .chain(s.mod_members.iter().map(|m| m.uid.clone()))
        .chain(s.searching.iter().map(|q| q.uid.clone()))
        .chain(s.matched.iter().flat_map(|m| m.players.iter().map(|p| p.player.uid.clone())))
        .chain(s.live.iter().flat_map(|g| g.players.iter().map(|p| p.uid.clone())))
}

/// Notes who plays every [`ACTIVITY_TICK`] and asks Twitch who streams every `TWITCH_POLL_SECS`.
pub async fn poller(state: AppState) {
    let poll = Duration::from_secs(state.cfg.twitch.twitch_poll_secs.max(10));
    let mut last_poll: Option<Instant> = None;
    let mut failing = false;
    loop {
        if let Ok(s) = state.mm_status.get().await {
            state.twitch.saw_playing(playing(&s), Instant::now());
        }
        if last_poll.is_none_or(|t| t.elapsed() >= poll) {
            last_poll = Some(Instant::now());
            match refresh(&state).await {
                Ok(()) => failing = false,
                Err(e) => {
                    if !failing {
                        tracing::warn!("twitch streams: {e:#}");
                    }
                    failing = true;
                }
            }
        }
        tokio::time::sleep(ACTIVITY_TICK).await;
    }
}

/// A connected account: the player and their Twitch account.
#[derive(Debug, sqlx::FromRow)]
struct LinkRow {
    uid: Uuid,
    twitch_id: String,
    login: String,
    display_name: String,
    name: String,
    connect_code: Option<String>,
    main: Option<String>,
}

const LINK_SELECT: &str = "SELECT l.uid, l.twitch_id, l.login, l.display_name, u.display_name AS name, u.connect_code,
        CASE WHEN COALESCE(p.show_main, true) THEN p.mains[1] END AS main
    FROM twitch_links l JOIN users u ON u.uid = l.uid LEFT JOIN profiles p ON p.uid = l.uid
    WHERE l.uid = ANY($1) AND (u.banned_until IS NULL OR u.banned_until < now())";

/// The streams now: Twitch's for the connected players seen playing, and staging's fakes.
async fn refresh(state: &AppState) -> anyhow::Result<()> {
    let mut out = Vec::new();
    let fakes = crate::staging_fake::twitch_streams();
    if !fakes.is_empty() {
        let uids: Vec<Uuid> = fakes.iter().filter_map(|(u, _)| u.parse().ok()).collect();
        let players: Vec<(Uuid, String, Option<String>, Option<String>)> = sqlx::query_as(
            "SELECT u.uid, u.display_name, u.connect_code, CASE WHEN COALESCE(p.show_main, true) THEN p.mains[1] END
             FROM users u LEFT JOIN profiles p ON p.uid = u.uid WHERE u.uid = ANY($1)",
        )
        .bind(&uids)
        .fetch_all(&state.pool)
        .await?;
        for (uid, s) in &fakes {
            if let Some((_, name, code, main)) = players.iter().find(|p| p.0.to_string() == *uid) {
                out.push(live_stream(uid.clone(), name.clone(), code.clone(), main.clone(), s));
            }
        }
    }
    if enabled(state) {
        let playing: Vec<Uuid> =
            state.twitch.recently_playing(Instant::now()).iter().filter_map(|u| u.parse().ok()).collect();
        let links: Vec<LinkRow> = if playing.is_empty() {
            Vec::new()
        } else {
            sqlx::query_as(LINK_SELECT).bind(&playing).fetch_all(&state.pool).await?
        };
        if !links.is_empty() {
            let ids: Vec<&str> = links.iter().map(|l| l.twitch_id.as_str()).collect();
            let live = live_streams(state, &ids).await?;
            for s in &live {
                let Some(l) = links.iter().find(|l| l.twitch_id == s.user_id) else { continue };
                if l.login != s.user_login || l.display_name != s.user_name {
                    let _ = sqlx::query("UPDATE twitch_links SET login = $2, display_name = $3 WHERE uid = $1")
                        .bind(l.uid)
                        .bind(&s.user_login)
                        .bind(&s.user_name)
                        .execute(&state.pool)
                        .await;
                }
                out.push(live_stream(l.uid.to_string(), l.name.clone(), l.connect_code.clone(), l.main.clone(), s));
            }
        }
    }
    out.sort_by(|a, b| b.viewers.cmp(&a.viewers).then_with(|| a.display_name.cmp(&b.display_name)));
    *state.twitch.streams.write().unwrap() = Arc::new(out);
    Ok(())
}

fn live_stream(uid: String, name: String, code: Option<String>, main: Option<String>, s: &HelixStream) -> LiveStream {
    // Twitch refreshes the picture every few minutes at the same address: a changing query string
    // gets the new one past the launcher's image cache.
    let bucket = chrono::Utc::now().timestamp() / 300;
    let thumbnail = if s.thumbnail_url.contains("{width}") {
        format!(
            "{}?t={bucket}",
            s.thumbnail_url.replace("{width}", &THUMB_W.to_string()).replace("{height}", &THUMB_H.to_string())
        )
    } else {
        s.thumbnail_url.clone()
    };
    LiveStream {
        uid,
        display_name: name,
        connect_code: code,
        main,
        login: s.user_login.clone(),
        twitch_name: if s.user_name.is_empty() { s.user_login.clone() } else { s.user_name.clone() },
        title: s.title.clone(),
        game: s.game_name.clone(),
        viewers: s.viewer_count,
        thumbnail,
        started_at: s.started_at.clone(),
    }
}

/// `/helix/streams` for these Twitch user ids (live ones only), with the app token (fetched again
/// once if Twitch refuses it).
async fn live_streams(state: &AppState, ids: &[&str]) -> anyhow::Result<Vec<HelixStream>> {
    let mut out = Vec::new();
    for chunk in ids.chunks(IDS_PER_CALL) {
        let mut retried = false;
        loop {
            let token = app_token(state).await?;
            let t = &state.cfg.twitch;
            let mut query: Vec<(&str, &str)> = chunk.iter().map(|id| ("user_id", *id)).collect();
            query.push(("type", "live"));
            query.push(("first", "100"));
            let resp = state
                .twitch
                .http
                .get(format!("{}/streams", t.twitch_api_url.trim_end_matches('/')))
                .query(&query)
                .header("Client-Id", t.twitch_client_id.as_deref().unwrap_or(""))
                .bearer_auth(&token)
                .send()
                .await?;
            if resp.status() == StatusCode::UNAUTHORIZED && !retried {
                *state.twitch.app_token.lock().await = None;
                retried = true;
                continue;
            }
            let list: HelixList<HelixStream> = resp.error_for_status()?.json().await?;
            out.extend(list.data);
            break;
        }
    }
    Ok(out)
}

/// The app access token (client credentials), kept until a minute before it runs out.
async fn app_token(state: &AppState) -> anyhow::Result<String> {
    let mut cached = state.twitch.app_token.lock().await;
    if let Some((token, until)) = cached.as_ref() {
        if Instant::now() < *until {
            return Ok(token.clone());
        }
    }
    let t = &state.cfg.twitch;
    let answer: TokenAnswer = state
        .twitch
        .http
        .post(format!("{}/token", t.twitch_auth_url.trim_end_matches('/')))
        .form(&[
            ("client_id", t.twitch_client_id.as_deref().unwrap_or("")),
            ("client_secret", t.twitch_client_secret.as_ref().map(|s| s.expose()).unwrap_or("")),
            ("grant_type", "client_credentials"),
        ])
        .send()
        .await?
        .error_for_status()?
        .json()
        .await?;
    let life = Duration::from_secs(answer.expires_in.max(120) - 60);
    *cached = Some((answer.access_token.clone(), Instant::now() + life));
    Ok(answer.access_token)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_connect_state_is_used_once_and_runs_out() {
        let hub = TwitchHub::default();
        let uid = Uuid::new_v4();
        let now = Instant::now();
        let s = hub.start_connect(uid, now);
        assert_eq!(s.len(), 48);
        assert_eq!(hub.finish_connect(&s, now), Some(uid));
        assert_eq!(hub.finish_connect(&s, now), None, "used up");
        let s = hub.start_connect(uid, now);
        assert_eq!(hub.finish_connect(&s, now + CONNECT_TTL), None, "too late");
        assert_eq!(hub.finish_connect("nope", now), None);
    }

    #[test]
    fn playing_is_remembered_for_the_window() {
        let hub = TwitchHub::default();
        let now = Instant::now();
        hub.saw_playing(["a".to_string()], now);
        hub.saw_playing(["b".to_string()], now + Duration::from_secs(60));
        let later = now + ACTIVE_WINDOW + Duration::from_secs(1);
        assert_eq!(hub.recently_playing(now + Duration::from_secs(61)).len(), 2);
        assert_eq!(hub.recently_playing(later), HashSet::from(["b".to_string()]));
    }

    #[test]
    fn the_thumbnail_template_is_filled() {
        let s = HelixStream {
            user_id: "1".into(),
            user_login: "knux".into(),
            thumbnail_url: "https://static-cdn.jtvnw.net/previews-ttv/live_user_knux-{width}x{height}.jpg".into(),
            ..Default::default()
        };
        let l = live_stream("u".into(), "Knuckles".into(), None, None, &s);
        assert!(l.thumbnail.starts_with("https://static-cdn.jtvnw.net/previews-ttv/live_user_knux-440x248.jpg?t="));
        assert_eq!(l.twitch_name, "knux", "the login without a display name");
    }
}
