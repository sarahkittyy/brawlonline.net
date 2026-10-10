//! `GET /v1/rooms`: the players online and the public rooms, for the launcher's Home page.
//!
//! Rooms live in mm's memory, so accounts asks mm: mm publishes a snapshot on a local HTTP
//! listener (`MM_STATUS_LISTEN`, `GET /status`) and accounts reads it from `MM_STATUS_URL`. One
//! read serves every launcher for [`CACHE_FOR`], so launchers polling every few seconds cost mm
//! at most one request a second. Launchers only ever talk to accounts (nginx proxies `/v1/` there
//! already), and mm's listener stays on loopback.

use std::time::{Duration, Instant};

use axum::extract::State;
use axum::http::{HeaderMap, StatusCode};
use axum::Json;
use common::rooms::MmStatus;
use tokio::sync::Mutex;

use crate::api::{current_user, limit};
use crate::error::{ApiError, ApiResult};
use crate::AppState;

/// How long one snapshot from mm is served.
pub const CACHE_FOR: Duration = Duration::from_secs(1);
/// mm answers from memory; anything slower means it is not there.
const FETCH_TIMEOUT: Duration = Duration::from_secs(2);

/// Reads mm's status, with the cache.
pub struct MmStatusSource {
    url: String,
    http: reqwest::Client,
    cache: Mutex<Option<(Instant, MmStatus)>>,
}

impl MmStatusSource {
    pub fn new(url: &str) -> Self {
        let http = reqwest::Client::builder().timeout(FETCH_TIMEOUT).build().expect("reqwest client builds");
        MmStatusSource { url: url.to_string(), http, cache: Mutex::new(None) }
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

/// `GET /v1/rooms`: `{online, rooms: [{code, host, players, openSlots, mode, status, names,
/// joinable}], updatedAt}`. Session auth (the list names players); limited per account
/// ([`common::ratelimit::ROOMS_WINDOWS`]). 503 `mm_unavailable` when mm does not answer.
pub async fn rooms(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<MmStatus>> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.rooms.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    match state.mm_status.get().await {
        Ok(s) => Ok(Json(s)),
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
