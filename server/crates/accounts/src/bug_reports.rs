//! Bug reports from the launcher's bug button (`migrations/0011_bug_reports.sql`).
//!
//! Two calls, so the text is kept even when the zip cannot be sent:
//! - `POST /v1/bug-reports` `{description, launcherVersion, platform}`: stores the text (with the
//!   account when a session token comes along; reports work logged out too, since logging in may
//!   be what is broken) and answers `{id, uploadToken}`.
//! - `PUT /v1/bug-reports/{id}/logs` with `X-Upload-Token` and the zip as the body: writes
//!   `bug-<id>.zip` in `BUG_REPORTS_DIR`. One zip per report, within an hour of the text.
//!
//! The admin CLI lists, resolves and deletes them (`admin bug ...`).

use std::net::SocketAddr;
use std::path::{Path, PathBuf};

use axum::body::Body;
use axum::extract::{ConnectInfo, Path as UrlPath, State};
use axum::http::{HeaderMap, StatusCode};
use axum::Json;
use common::net::ip_key;
use common::playkey::{hash_token, random_token};
use futures_core::Stream;
use serde::{Deserialize, Serialize};
use tokio::io::AsyncWriteExt;

use crate::api::{client_ip, current_user, limit};
use crate::error::{ApiError, ApiResult};
use crate::AppState;

/// The longest description, in characters.
pub const MAX_DESCRIPTION_CHARS: usize = 5000;
/// How long after the text its zip may still come.
pub const UPLOAD_WINDOW_MINUTES: i64 = 60;

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NewBugReport {
    #[serde(default)]
    pub description: String,
    #[serde(default)]
    pub launcher_version: String,
    #[serde(default)]
    pub platform: String,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BugReportCreated {
    pub id: i64,
    /// For `PUT /v1/bug-reports/{id}/logs` (`X-Upload-Token`).
    pub upload_token: String,
}

/// The zip of report `id` in `dir`.
pub fn logs_path(dir: &Path, id: i64) -> PathBuf {
    dir.join(format!("bug-{id}.zip"))
}

/// `s` without control characters (newlines and tabs stay), cut to `max` characters.
fn clean(s: &str, max: usize, keep_newlines: bool) -> String {
    s.chars()
        .filter(|&c| !c.is_control() || (keep_newlines && (c == '\n' || c == '\t')))
        .take(max)
        .collect::<String>()
        .trim()
        .to_string()
}

pub async fn create(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Json(req): Json<NewBugReport>,
) -> ApiResult<(StatusCode, Json<BugReportCreated>)> {
    let ip = ip_key(client_ip(&state, &headers, peer));
    limit(&state, |l, now| l.bug_report_ip.check(&ip, now)).map_err(|wait| {
        ApiError::rate_limited(wait).with_message("You have sent a lot of bug reports. Please try again later.")
    })?;
    // A logged-out or expired session still reports, just without an account.
    let uid = if headers.contains_key("authorization") {
        current_user(&state, &headers).await.ok().map(|u| u.uid)
    } else {
        None
    };
    let token = random_token();
    let id: i64 = sqlx::query_scalar(
        "INSERT INTO bug_reports (uid, description, launcher_version, platform, upload_token_hash)
         VALUES ($1, $2, $3, $4, $5) RETURNING id",
    )
    .bind(uid)
    .bind(clean(&req.description, MAX_DESCRIPTION_CHARS, true))
    .bind(clean(&req.launcher_version, 64, false))
    .bind(clean(&req.platform, 200, false))
    .bind(hash_token(&token))
    .fetch_one(&state.pool)
    .await?;
    tracing::info!(id, ?uid, "bug report");
    Ok((StatusCode::CREATED, Json(BugReportCreated { id, upload_token: token })))
}

fn too_large(max: u64) -> ApiError {
    let msg = format!("The logs are larger than {} MB.", max / (1024 * 1024));
    ApiError::new(StatusCode::PAYLOAD_TOO_LARGE, "too_large", msg)
}

fn storage_unavailable(e: impl std::fmt::Display) -> ApiError {
    tracing::error!("bug report logs: {e}");
    ApiError::new(StatusCode::SERVICE_UNAVAILABLE, "storage_unavailable", "The server can't store logs right now.")
}

pub async fn upload_logs(
    State(state): State<AppState>,
    UrlPath(id): UrlPath<i64>,
    headers: HeaderMap,
    body: Body,
) -> ApiResult<StatusCode> {
    let token = headers.get("x-upload-token").and_then(|v| v.to_str().ok()).ok_or_else(ApiError::unauthorized)?;
    let max = state.cfg.bug_report_max_logs_bytes;
    let declared: Option<u64> =
        headers.get("content-length").and_then(|v| v.to_str().ok()).and_then(|v| v.parse().ok());
    if declared.is_some_and(|n| n > max) {
        return Err(too_large(max));
    }

    // The report must be waiting for this zip: right token, none sent yet, within the hour.
    let waiting: Option<i64> = sqlx::query_scalar(
        "SELECT id FROM bug_reports
          WHERE id = $1 AND upload_token_hash = $2 AND logs_bytes IS NULL
            AND created_at > now() - make_interval(mins => $3)",
    )
    .bind(id)
    .bind(hash_token(token))
    .bind(UPLOAD_WINDOW_MINUTES as i32)
    .fetch_optional(&state.pool)
    .await?;
    if waiting.is_none() {
        return Err(ApiError::not_found());
    }
    let stored: i64 = sqlx::query_scalar("SELECT COALESCE(sum(logs_bytes), 0)::bigint FROM bug_reports")
        .fetch_one(&state.pool)
        .await?;
    if stored as u64 + declared.unwrap_or(max) > state.cfg.bug_reports_max_total_bytes {
        tracing::warn!(id, stored, "bug report logs refused: BUG_REPORTS_MAX_TOTAL_BYTES reached");
        return Err(ApiError::new(
            StatusCode::INSUFFICIENT_STORAGE,
            "storage_full",
            "The server has no room for more logs right now. Your report's text was sent.",
        ));
    }

    let dir = &state.cfg.bug_reports_dir;
    tokio::fs::create_dir_all(dir).await.map_err(|e| storage_unavailable(format!("{}: {e}", dir.display())))?;
    let dest = logs_path(dir, id);
    let partial = dir.join(format!(".bug-{id}.zip.{}.partial", &random_token()[..8]));
    let written = write_body(body, &partial, max).await;
    let size = match written {
        Ok(size) => size,
        Err(e) => {
            let _ = tokio::fs::remove_file(&partial).await;
            return Err(e);
        }
    };
    // Only the first upload counts; a second one racing it loses here.
    let claimed = sqlx::query(
        "UPDATE bug_reports SET logs_bytes = $3, upload_token_hash = NULL
          WHERE id = $1 AND upload_token_hash = $2 AND logs_bytes IS NULL",
    )
    .bind(id)
    .bind(hash_token(token))
    .bind(size as i64)
    .execute(&state.pool)
    .await;
    match claimed {
        Ok(r) if r.rows_affected() == 1 => {}
        Ok(_) => {
            let _ = tokio::fs::remove_file(&partial).await;
            return Err(ApiError::not_found());
        }
        Err(e) => {
            let _ = tokio::fs::remove_file(&partial).await;
            return Err(e.into());
        }
    }
    if let Err(e) = tokio::fs::rename(&partial, &dest).await {
        let _ = tokio::fs::remove_file(&partial).await;
        // Let the launcher try again: the report waits for its zip once more.
        let _ = sqlx::query("UPDATE bug_reports SET logs_bytes = NULL, upload_token_hash = $2 WHERE id = $1")
            .bind(id)
            .bind(hash_token(token))
            .execute(&state.pool)
            .await;
        return Err(storage_unavailable(format!("{}: {e}", dest.display())));
    }
    tracing::info!(id, size, "bug report logs");
    Ok(StatusCode::NO_CONTENT)
}

/// Streams `body` to `path`, at most `max` bytes, and checks that it starts like a zip.
async fn write_body(body: Body, path: &Path, max: u64) -> ApiResult<u64> {
    let mut file =
        tokio::fs::File::create(path).await.map_err(|e| storage_unavailable(format!("{}: {e}", path.display())))?;
    let mut stream = std::pin::pin!(body.into_data_stream());
    let mut size = 0u64;
    let mut head: Vec<u8> = Vec::with_capacity(4);
    while let Some(chunk) = std::future::poll_fn(|cx| stream.as_mut().poll_next(cx)).await {
        // The body limit layer ends the stream with an error past its limit.
        let chunk = chunk.map_err(|_| too_large(max))?;
        size += chunk.len() as u64;
        if size > max {
            return Err(too_large(max));
        }
        if head.len() < 4 {
            head.extend(chunk.iter().take(4 - head.len()));
        }
        file.write_all(&chunk).await.map_err(|e| storage_unavailable(format!("{}: {e}", path.display())))?;
    }
    // A zip starts with a local file header, or is an empty archive (end of central directory).
    if head != b"PK\x03\x04" && head != b"PK\x05\x06" {
        return Err(ApiError::bad_request("not_a_zip", "The logs must be a zip file."));
    }
    file.flush().await.map_err(|e| storage_unavailable(format!("{}: {e}", path.display())))?;
    file.sync_all().await.map_err(|e| storage_unavailable(format!("{}: {e}", path.display())))?;
    Ok(size)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clean_keeps_text_and_cuts() {
        assert_eq!(clean("  hi\r\nthere\u{0}\t!  ", 100, true), "hi\nthere\t!");
        assert_eq!(clean("a\nb", 100, false), "ab");
        assert_eq!(clean("ééééé", 3, true), "ééé");
    }
}
