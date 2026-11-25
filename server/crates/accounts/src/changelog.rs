//! The launcher's changelogs (docs/design/changelog.md): what's new in a launcher release, written in
//! Markdown in the web editor (`website/changelog/write/`, admins only) before the push that ships
//! it, and shown once by every launcher that was just updated (`GET /v1/changelogs/latest`). The
//! Markdown is the blog's (`blog::render`: raw HTML as text, no outside images, videos by
//! extension), and so are the images (`/v1/blog/media`).
//!
//! Every changelog stays in the table as the history; only the newest published one is served for
//! now.

use axum::extract::{Path, State};
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::Json;
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};

use crate::admin::require_admin;
use crate::blog::render;
use crate::error::{ApiError, ApiResult};
use crate::AppState;

/// The longest changelog's Markdown (its images are links to uploads, so this is a lot of text).
/// Under prod nginx's 64 KiB request body cap, which `/v1/changelogs` shares with the rest of the API.
pub const MAX_CHANGELOG_BYTES: usize = 48 * 1024;
/// The request body limit of the editor's calls: a changelog and the JSON around it.
pub const BODY_LIMIT: usize = MAX_CHANGELOG_BYTES + 8 * 1024;
/// How much of the first line the editor's list shows.
const TITLE_CHARS: usize = 80;

#[derive(Debug, Clone, sqlx::FromRow)]
struct ChangelogRow {
    id: i64,
    body_md: String,
    status: String,
    published_at: Option<DateTime<Utc>>,
    created_at: DateTime<Utc>,
    updated_at: DateTime<Utc>,
}

const COLUMNS: &str = "id, body_md, status, published_at, created_at, updated_at";

/// A changelog as the editor sees it.
#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Changelog {
    pub id: i64,
    /// Its first line (a heading's text), for the editor's list.
    pub title: String,
    pub body: String,
    /// `draft` or `published`.
    pub status: String,
    pub published_at: Option<DateTime<Utc>>,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
}

impl From<ChangelogRow> for Changelog {
    fn from(r: ChangelogRow) -> Self {
        Changelog {
            id: r.id,
            title: title_of(&r.body_md),
            body: r.body_md,
            status: r.status,
            published_at: r.published_at,
            created_at: r.created_at,
            updated_at: r.updated_at,
        }
    }
}

/// The first line with words in it, without a heading's `#`s; empty for a changelog with none.
/// Images and comments (an upload's "Uploading" note) are not words.
pub fn title_of(md: &str) -> String {
    md.lines()
        .map(|l| l.trim().trim_start_matches('#').trim())
        .find(|l| !l.is_empty() && !l.starts_with("<!--") && !l.starts_with("!["))
        .map(|l| l.chars().take(TITLE_CHARS).collect())
        .unwrap_or_default()
}

async fn by_id(state: &AppState, id: i64) -> ApiResult<ChangelogRow> {
    sqlx::query_as::<_, ChangelogRow>(&format!("SELECT {COLUMNS} FROM changelogs WHERE id = $1"))
        .bind(id)
        .fetch_optional(&state.pool)
        .await?
        .ok_or_else(ApiError::not_found)
}

fn check_size(body: &str) -> ApiResult<()> {
    if body.len() > MAX_CHANGELOG_BYTES {
        return Err(ApiError::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "too_large",
            format!("A changelog is at most {} KB of text.", MAX_CHANGELOG_BYTES / 1024),
        ));
    }
    Ok(())
}

/// `GET /v1/changelogs`: every changelog, drafts first (newest change first), then the published
/// ones (newest first).
pub async fn list(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<Vec<Changelog>>> {
    require_admin(&state, &headers).await?;
    let rows = sqlx::query_as::<_, ChangelogRow>(&format!(
        "SELECT {COLUMNS} FROM changelogs
          ORDER BY (status = 'draft') DESC, COALESCE(published_at, updated_at) DESC, id DESC"
    ))
    .fetch_all(&state.pool)
    .await?;
    Ok(Json(rows.into_iter().map(Changelog::from).collect()))
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct CreateReq {
    pub body: String,
}

/// `POST /v1/changelogs`: a new draft (an empty one with `{}`).
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<CreateReq>,
) -> ApiResult<(StatusCode, Json<Changelog>)> {
    let user = require_admin(&state, &headers).await?;
    check_size(&req.body)?;
    let row = sqlx::query_as::<_, ChangelogRow>(&format!(
        "INSERT INTO changelogs (body_md, created_by) VALUES ($1, $2) RETURNING {COLUMNS}"
    ))
    .bind(&req.body)
    .bind(user.uid)
    .fetch_one(&state.pool)
    .await?;
    tracing::info!(id = row.id, by = %user.uid, "changelog created");
    Ok((StatusCode::CREATED, Json(row.into())))
}

/// `GET /v1/changelogs/<id>`.
pub async fn get_one(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<i64>,
) -> ApiResult<Json<Changelog>> {
    require_admin(&state, &headers).await?;
    Ok(Json(by_id(&state, id).await?.into()))
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateReq {
    pub body: String,
    /// The `updatedAt` the editor last saw: a save over a newer change (another tab) is refused
    /// with 409 `changed_elsewhere` instead of overwriting it. Without it, the save always goes.
    #[serde(default)]
    pub expected_updated_at: Option<DateTime<Utc>>,
}

/// `PUT /v1/changelogs/<id>`: saves a changelog. A published one's change is what launchers get
/// from then on (those that showed it already don't show it again).
pub async fn update(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<i64>,
    Json(req): Json<UpdateReq>,
) -> ApiResult<Json<Changelog>> {
    require_admin(&state, &headers).await?;
    check_size(&req.body)?;
    let row = sqlx::query_as::<_, ChangelogRow>(&format!(
        "UPDATE changelogs SET body_md = $2, updated_at = now()
          WHERE id = $1 AND ($3::timestamptz IS NULL OR updated_at = $3)
          RETURNING {COLUMNS}"
    ))
    .bind(id)
    .bind(&req.body)
    .bind(req.expected_updated_at)
    .fetch_optional(&state.pool)
    .await?;
    match row {
        Some(row) => Ok(Json(row.into())),
        None => {
            // Gone, or changed since the editor loaded it.
            by_id(&state, id).await?;
            Err(ApiError::conflict(
                "changed_elsewhere",
                "This changelog was changed somewhere else (another tab?). Reload it to see that version.",
            ))
        }
    }
}

/// `POST /v1/changelogs/<id>/publish`: the one updated launchers show, if it is the newest. The
/// first publish sets its date; publishing again after an unpublish keeps it.
pub async fn publish(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<i64>,
) -> ApiResult<Json<Changelog>> {
    let user = require_admin(&state, &headers).await?;
    let row = by_id(&state, id).await?;
    if row.body_md.trim().is_empty() {
        return Err(ApiError::bad_request("empty", "Write something first."));
    }
    let row = sqlx::query_as::<_, ChangelogRow>(&format!(
        "UPDATE changelogs SET status = 'published', published_at = COALESCE(published_at, now()), updated_at = now()
          WHERE id = $1 RETURNING {COLUMNS}"
    ))
    .bind(id)
    .fetch_one(&state.pool)
    .await?;
    tracing::info!(id, by = %user.uid, "changelog published");
    Ok(Json(row.into()))
}

/// `POST /v1/changelogs/<id>/unpublish`: a draft again (launchers no longer get it).
pub async fn unpublish(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<i64>,
) -> ApiResult<Json<Changelog>> {
    let user = require_admin(&state, &headers).await?;
    let row = sqlx::query_as::<_, ChangelogRow>(&format!(
        "UPDATE changelogs SET status = 'draft', updated_at = now() WHERE id = $1 RETURNING {COLUMNS}"
    ))
    .bind(id)
    .fetch_optional(&state.pool)
    .await?
    .ok_or_else(ApiError::not_found)?;
    tracing::info!(id, by = %user.uid, "changelog unpublished");
    Ok(Json(row.into()))
}

/// `DELETE /v1/changelogs/<id>`. Its images stay (a blog post may use them).
pub async fn delete(State(state): State<AppState>, headers: HeaderMap, Path(id): Path<i64>) -> ApiResult<StatusCode> {
    let user = require_admin(&state, &headers).await?;
    let gone = sqlx::query("DELETE FROM changelogs WHERE id = $1").bind(id).execute(&state.pool).await?;
    if gone.rows_affected() == 0 {
        return Err(ApiError::not_found());
    }
    tracing::info!(id, by = %user.uid, "changelog deleted");
    Ok(StatusCode::NO_CONTENT)
}

/// The newest published changelog, as a launcher shows it.
#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LatestChangelog {
    pub id: i64,
    /// `blog::render`'s HTML. Its images and videos are on this server (`/blog/media/...`, relative).
    pub html: String,
    /// A launcher shows only one published after the last it showed, and never one published
    /// after the launcher was built (it is about a release still to come).
    pub published_at: DateTime<Utc>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct LatestResp {
    pub changelog: Option<LatestChangelog>,
}

/// `GET /v1/changelogs/latest` (public): the newest published changelog, or `null`. Launchers ask
/// once after an update.
pub async fn latest(State(state): State<AppState>) -> Response {
    let row = sqlx::query_as::<_, (i64, String, Option<DateTime<Utc>>)>(
        "SELECT id, body_md, published_at FROM changelogs WHERE status = 'published' \
         ORDER BY published_at DESC, id DESC LIMIT 1",
    )
    .fetch_optional(&state.pool)
    .await;
    match row {
        Ok(row) => {
            let changelog = row
                .and_then(|(id, body, at)| Some(LatestChangelog { id, html: render(&body).html, published_at: at? }));
            ([(header::CACHE_CONTROL, HeaderValue::from_static("public, max-age=60"))], Json(LatestResp { changelog }))
                .into_response()
        }
        Err(e) => {
            tracing::error!("changelog latest: {e}");
            StatusCode::INTERNAL_SERVER_ERROR.into_response()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn titles() {
        assert_eq!(title_of("# Launcher 0.2.9\n\nStuff"), "Launcher 0.2.9");
        assert_eq!(title_of("\n\n![](/blog/media/a.webp)\n\n<!-- Uploading x (1)… -->\n- Fixed it"), "- Fixed it");
        assert_eq!(title_of("   \n"), "");
        assert_eq!(title_of(&"x".repeat(200)).chars().count(), TITLE_CHARS);
    }
}
