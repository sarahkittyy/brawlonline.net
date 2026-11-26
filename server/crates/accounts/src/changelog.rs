//! The launcher's changelogs (docs/design/changelog.md): what's new in a launcher release. Sarah
//! writes one in her agent board (Markdown, pictures pasted in), and the board publishes it with
//! `pp-admin changelog publish` (`publish` here) just before it pushes main, so the release built
//! from that push is the first to show it. A launcher shows the newest once after it updates
//! (`GET /v1/changelogs/latest`). The Markdown is the blog's (`blog::render`), and so are the
//! pictures: files in BLOG_MEDIA_DIR served at `/blog/media/<name>`.
//!
//! Staging only: the board's "show on staging" publishes its draft as an `every_start` changelog,
//! which staging launchers show at every start.
//!
//! Every changelog stays in the table as the history. The launcher pages through it as a feed
//! (`GET /v1/changelogs`); `GET /v1/changelogs/latest` is the newest alone, for older launchers.
//!
//! EmissaryRecomp's patch notes are changelogs too (`product` `emissary`, its release's `version`):
//! the board publishes them once that release is live, and only launchers that play with it ask for
//! them (`?emissary=1`), in the same feed.

use std::path::Path;

use axum::extract::{Query, State};
use axum::http::{header, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::Json;
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use sqlx::PgPool;

use crate::blog::{render, sniff_media, valid_media_name};
use crate::cursor;
use crate::error::{ApiError, ApiResult};
use crate::AppState;

/// The longest changelog's Markdown.
pub const MAX_CHANGELOG_BYTES: usize = 1024 * 1024;
/// The largest picture or video in one (as the blog's uploads).
pub const MAX_MEDIA_BYTES: usize = 95 * 1024 * 1024;
/// How much of the first line `list` shows.
const TITLE_CHARS: usize = 80;

/// The first line with words in it, without a heading's `#`s; empty for a changelog with none.
/// Pictures and comments are not words.
pub fn title_of(md: &str) -> String {
    md.lines()
        .map(|l| l.trim().trim_start_matches('#').trim())
        .find(|l| !l.is_empty() && !l.starts_with("<!--") && !l.starts_with("!["))
        .map(|l| l.chars().take(TITLE_CHARS).collect())
        .unwrap_or_default()
}

/// The media names (`<20 hex>.<ext>`) the Markdown points at as `/blog/media/<name>`.
pub fn media_names(md: &str) -> Vec<String> {
    let mut names = Vec::new();
    for (i, _) in md.match_indices("/blog/media/") {
        let rest = &md[i + "/blog/media/".len()..];
        let end = rest.find(|c: char| !(c.is_ascii_alphanumeric() || c == '.')).unwrap_or(rest.len());
        let name = rest[..end].trim_end_matches('.');
        if valid_media_name(name) && !names.iter().any(|n| n == name) {
            names.push(name.to_string());
        }
    }
    names
}

/// A picture or video sent with a changelog: the name the Markdown uses for it, and its bytes.
#[derive(Debug, Clone)]
pub struct MediaFile {
    pub name: String,
    pub data: Vec<u8>,
}

#[derive(Debug, Serialize)]
pub struct Published {
    pub id: i64,
    pub published_at: DateTime<Utc>,
    /// Media files written (those already there are left as they are).
    pub written: Vec<String>,
}

/// Writes the media into `media_dir` under their content's names (as the blog's uploads: the first
/// 20 hex of the SHA-256 and the type's extension, sniffed from the bytes), points the Markdown at
/// those names, checks every picture it names is there, and adds the changelog. With
/// `every_start`, it replaces the staging-only one instead (`every_start` rows).
pub async fn publish(
    pool: &PgPool,
    media_dir: &Path,
    markdown: &str,
    media: &[MediaFile],
    every_start: bool,
) -> anyhow::Result<Published> {
    publish_at(pool, media_dir, markdown, media, every_start, None).await
}

/// How far back [`publish_at`] may date a changelog.
pub const MAX_BACKDATE: chrono::Duration = chrono::Duration::days(2);

/// [`publish`], dated `at` instead of now (at most [`MAX_BACKDATE`] ago, never ahead): for the
/// first push that brings patch notes, which prod's pp-admin can publish only once that push has
/// deployed it, dated just before the push so the launchers built from it show them
/// (docs/design/changelog.md, "The first push").
pub async fn publish_at(
    pool: &PgPool,
    media_dir: &Path,
    markdown: &str,
    media: &[MediaFile],
    every_start: bool,
    at: Option<DateTime<Utc>>,
) -> anyhow::Result<Published> {
    publish_for(pool, media_dir, markdown, media, every_start, at, None).await
}

/// An EmissaryRecomp release's version as its patch notes name it: `v` and three numbers.
pub fn valid_emissary_version(v: &str) -> bool {
    let Some(rest) = v.strip_prefix('v') else { return false };
    let parts: Vec<&str> = rest.split('.').collect();
    parts.len() == 3 && parts.iter().all(|p| !p.is_empty() && p.len() <= 9 && p.bytes().all(|b| b.is_ascii_digit()))
}

/// [`publish_at`] for the launcher's (`emissary` None) or an EmissaryRecomp release's patch notes
/// (`emissary` its version, `v0.7.2`). The every-start one is replaced per product.
pub async fn publish_for(
    pool: &PgPool,
    media_dir: &Path,
    markdown: &str,
    media: &[MediaFile],
    every_start: bool,
    at: Option<DateTime<Utc>>,
    emissary: Option<&str>,
) -> anyhow::Result<Published> {
    if let Some(v) = emissary {
        anyhow::ensure!(valid_emissary_version(v), "--emissary {v:?}: not a release version like v0.7.2");
    }
    let product = if emissary.is_some() { PRODUCT_EMISSARY } else { PRODUCT_LAUNCHER };
    if let Some(at) = at {
        let now = Utc::now();
        anyhow::ensure!(at <= now + chrono::Duration::minutes(1), "--published-at {at} is in the future");
        anyhow::ensure!(at >= now - MAX_BACKDATE, "--published-at {at} is more than 2 days ago");
    }
    anyhow::ensure!(!markdown.trim().is_empty(), "the changelog is empty");
    anyhow::ensure!(markdown.len() <= MAX_CHANGELOG_BYTES, "a changelog is at most 1 MB of text");
    let mut md = markdown.to_string();
    let mut written = Vec::new();
    for f in media {
        anyhow::ensure!(f.data.len() <= MAX_MEDIA_BYTES, "{}: a file is at most 95 MB", f.name);
        let (kind, ext) = sniff_media(&f.data[..f.data.len().min(16)])
            .ok_or_else(|| anyhow::anyhow!("{}: not a PNG, JPEG, GIF, WebP, AVIF, MP4, WebM or MOV", f.name))?;
        let name = format!("{}.{ext}", &hex::encode(Sha256::digest(&f.data))[..20]);
        if name != f.name {
            md = md.replace(&format!("/blog/media/{}", f.name), &format!("/blog/media/{name}"));
        }
        let dest = media_dir.join(&name);
        if !dest.exists() {
            std::fs::create_dir_all(media_dir)?;
            let partial = media_dir.join(format!("{name}.partial"));
            std::fs::write(&partial, &f.data)?;
            // pp-admin runs as root on prod; accounts (ppserver) reads the file.
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                std::fs::set_permissions(&partial, std::fs::Permissions::from_mode(0o644))?;
            }
            std::fs::rename(&partial, &dest)?;
            written.push(name.clone());
        }
        sqlx::query(
            "INSERT INTO blog_media (name, kind, bytes, original_name) VALUES ($1, $2, $3, 'changelog')
             ON CONFLICT (name) DO NOTHING",
        )
        .bind(&name)
        .bind(if kind == crate::blog::MediaKind::Video { "video" } else { "image" })
        .bind(f.data.len() as i64)
        .execute(pool)
        .await?;
    }
    let missing: Vec<String> = media_names(&md).into_iter().filter(|n| !media_dir.join(n).exists()).collect();
    anyhow::ensure!(
        missing.is_empty(),
        "pictures the changelog shows that aren't here or sent with it: {}",
        missing.join(", ")
    );

    let mut tx = pool.begin().await?;
    if every_start {
        sqlx::query("DELETE FROM changelogs WHERE every_start AND product = $1").bind(product).execute(&mut *tx).await?;
    }
    let (id, published_at): (i64, DateTime<Utc>) = sqlx::query_as(
        "INSERT INTO changelogs (body_md, every_start, published_at, product, version)
             VALUES ($1, $2, COALESCE($3, now()), $4, $5)
             RETURNING id, published_at",
    )
    .bind(&md)
    .bind(every_start)
    .bind(at)
    .bind(product)
    .bind(emissary)
    .fetch_one(&mut *tx)
    .await?;
    tx.commit().await?;
    Ok(Published { id, published_at, written })
}

/// Removes the staging-only changelogs (the board's "show on staging" turned off): the count removed.
pub async fn clear_every_start(pool: &PgPool) -> sqlx::Result<u64> {
    Ok(sqlx::query("DELETE FROM changelogs WHERE every_start").execute(pool).await?.rows_affected())
}

/// `changelogs.product`.
pub const PRODUCT_LAUNCHER: &str = "launcher";
pub const PRODUCT_EMISSARY: &str = "emissary";

#[derive(Debug, sqlx::FromRow)]
pub struct ChangelogRow {
    pub id: i64,
    pub body_md: String,
    pub published_at: DateTime<Utc>,
    pub every_start: bool,
    /// [`PRODUCT_LAUNCHER`] or [`PRODUCT_EMISSARY`].
    pub product: String,
    /// An Emissary one's release (`v0.7.2`).
    pub version: Option<String>,
}

/// Every changelog, newest first.
pub async fn list(pool: &PgPool) -> sqlx::Result<Vec<ChangelogRow>> {
    sqlx::query_as("SELECT id, body_md, published_at, every_start, product, version FROM changelogs ORDER BY published_at DESC, id DESC")
        .fetch_all(pool)
        .await
}

/// The newest changelog, as a launcher shows it.
#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LatestChangelog {
    pub id: i64,
    /// `blog::render`'s HTML. Its pictures and videos are on this server (`/blog/media/...`, relative).
    pub html: String,
    /// The Markdown it was written in, for the Discord bot, which posts each release's to #updates
    /// (Discord's Markdown, not HTML). Launchers ignore it.
    pub markdown: String,
    /// A launcher shows only one published after the last it showed, and never one published
    /// after the launcher was built (it is about a release still to come).
    pub published_at: DateTime<Utc>,
    /// Staging's "show on staging": shown at every start, whatever the launcher showed before.
    pub every_start: bool,
    /// `launcher` or `emissary` (EmissaryRecomp's patch notes; only with `?emissary=1`).
    #[serde(default = "launcher_product")]
    pub product: String,
    /// An Emissary one's release (`v0.7.2`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub version: Option<String>,
}

fn launcher_product() -> String {
    PRODUCT_LAUNCHER.into()
}

#[derive(Debug, Serialize, Deserialize)]
pub struct LatestResp {
    pub changelog: Option<LatestChangelog>,
}

/// `GET /v1/changelogs/latest` (public): the staging-only changelog if there is one, else the
/// newest; `null` without any. Launchers ask once after an update (staging ones at every start).
/// The launcher's only (older launchers know nothing of EmissaryRecomp's).
pub async fn latest(State(state): State<AppState>) -> Response {
    let row = sqlx::query_as::<_, ChangelogRow>(
        "SELECT id, body_md, published_at, every_start, product, version FROM changelogs \
         WHERE product = 'launcher' ORDER BY every_start DESC, published_at DESC, id DESC LIMIT 1",
    )
    .fetch_optional(&state.pool)
    .await;
    match row {
        Ok(row) => {
            let changelog = row.map(shown);
            ([(header::CACHE_CONTROL, HeaderValue::from_static("public, max-age=60"))], Json(LatestResp { changelog }))
                .into_response()
        }
        Err(e) => {
            tracing::error!("changelog latest: {e}");
            StatusCode::INTERNAL_SERVER_ERROR.into_response()
        }
    }
}

/// A page of the feed by default, and at most.
const PAGE_DEFAULT_LIMIT: u32 = 2;
const PAGE_MAX_LIMIT: u32 = 5;

#[derive(Debug, Deserialize)]
pub struct PageQuery {
    pub limit: Option<u32>,
    /// Only changelogs published before this time (RFC 3339): a launcher's build time, as one
    /// written for a release still to come waits for that release. Ignored with `after`.
    pub before: Option<String>,
    /// The `next` of the page before.
    pub after: Option<String>,
    /// `1`: EmissaryRecomp's patch notes too (a launcher whose Play uses it). `before` does not
    /// hold them back: they are published once their release is out.
    pub emissary: Option<String>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct ChangelogPage {
    /// Newest first; on the first page, the staging-only one ahead of them.
    pub changelogs: Vec<LatestChangelog>,
    /// Pass as `after` for the next page; null on the last.
    pub next: Option<String>,
}

fn page_cursor(r: &ChangelogRow) -> String {
    cursor::encode("c1", &[&r.published_at.timestamp_micros().to_string(), &r.id.to_string()])
}

fn parse_page_cursor(c: &str) -> Option<(DateTime<Utc>, i64)> {
    let f = cursor::decode("c1", c, 2)?;
    Some((DateTime::from_timestamp_micros(f[0].parse().ok()?)?, f[1].parse().ok()?))
}

fn shown(r: ChangelogRow) -> LatestChangelog {
    LatestChangelog {
        id: r.id,
        html: render(&r.body_md).html,
        markdown: r.body_md,
        published_at: r.published_at,
        every_start: r.every_start,
        product: r.product,
        version: r.version,
    }
}

/// `GET /v1/changelogs?limit=&before=&after=` (public): every changelog, newest first, in pages
/// (`next`), for the launcher's feed of patch notes. The first page (no `after`) starts with the
/// staging-only one if there is one, whatever `before` says. 400 `bad_cursor` / `bad_before`.
pub async fn page(State(state): State<AppState>, Query(q): Query<PageQuery>) -> ApiResult<Response> {
    let n = cursor::page_limit(q.limit, PAGE_DEFAULT_LIMIT, PAGE_MAX_LIMIT) as i64;
    let after = match q.after.as_deref() {
        Some(c) => Some(parse_page_cursor(c).ok_or_else(|| ApiError::bad_request("bad_cursor", "Bad cursor"))?),
        None => None,
    };
    let before = match q.before.as_deref().filter(|_| after.is_none()) {
        Some(t) => Some(
            DateTime::parse_from_rfc3339(t)
                .map_err(|_| ApiError::bad_request("bad_before", "Bad time"))?
                .with_timezone(&Utc),
        ),
        None => None,
    };
    let emissary = matches!(q.emissary.as_deref(), Some("1" | "true"));
    // Emissary ones are left out of `before`: published once their release is out, whatever
    // launcher reads them.
    let mut rows = match after {
        Some((at, id)) => {
            sqlx::query_as::<_, ChangelogRow>(
                "SELECT id, body_md, published_at, every_start, product, version FROM changelogs \
                 WHERE NOT every_start AND (published_at, id) < ($1, $2) \
                   AND (product = 'launcher' OR ($4 AND product = 'emissary')) \
                 ORDER BY published_at DESC, id DESC LIMIT $3",
            )
            .bind(at)
            .bind(id)
            .bind(n + 1)
            .bind(emissary)
            .fetch_all(&state.pool)
            .await
        }
        None => {
            sqlx::query_as::<_, ChangelogRow>(
                "SELECT id, body_md, published_at, every_start, product, version FROM changelogs \
                 WHERE NOT every_start \
                   AND ((product = 'launcher' AND ($1::timestamptz IS NULL OR published_at < $1)) \
                        OR ($3 AND product = 'emissary')) \
                 ORDER BY published_at DESC, id DESC LIMIT $2",
            )
            .bind(before)
            .bind(n + 1)
            .bind(emissary)
            .fetch_all(&state.pool)
            .await
        }
    }
    .map_err(ApiError::internal)?;
    let next = if rows.len() > n as usize {
        rows.truncate(n as usize);
        rows.last().map(page_cursor)
    } else {
        None
    };
    let mut changelogs = Vec::with_capacity(rows.len() + 1);
    if after.is_none() {
        // Staging's every-start ones: the launcher's, and with `emissary` the recomp's.
        let every: Vec<ChangelogRow> = sqlx::query_as(
            "SELECT id, body_md, published_at, every_start, product, version FROM changelogs \
             WHERE every_start AND (product = 'launcher' OR ($1 AND product = 'emissary')) \
             ORDER BY published_at DESC, id DESC LIMIT 2",
        )
        .bind(emissary)
        .fetch_all(&state.pool)
        .await
        .map_err(ApiError::internal)?;
        changelogs.extend(every.into_iter().map(shown));
    }
    changelogs.extend(rows.into_iter().map(shown));
    Ok((
        [(header::CACHE_CONTROL, HeaderValue::from_static("public, max-age=60"))],
        Json(ChangelogPage { changelogs, next }),
    )
        .into_response())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn emissary_versions() {
        assert!(valid_emissary_version("v0.7.2"));
        for bad in ["0.7.2", "v0.7", "v0.7.2-rc.1", "v0.x.2", ""] {
            assert!(!valid_emissary_version(bad), "{bad}");
        }
    }

    #[test]
    fn titles() {
        assert_eq!(title_of("# Launcher 0.2.9\n\nStuff"), "Launcher 0.2.9");
        assert_eq!(title_of("\n\n![](/blog/media/a.webp)\n\n<!-- Uploading x (1)… -->\n- Fixed it"), "- Fixed it");
        assert_eq!(title_of("   \n"), "");
        assert_eq!(title_of(&"x".repeat(200)).chars().count(), TITLE_CHARS);
    }

    #[test]
    fn media_names_in_markdown() {
        let md = "![a](/blog/media/0123456789abcdef0123.webp)\n\n![b](/blog/media/0123456789abcdef0123.webp) \
                  [clip](/blog/media/abcdefabcdefabcdef01.mp4). /blog/media/nope.png";
        assert_eq!(media_names(md), vec!["0123456789abcdef0123.webp", "abcdefabcdefabcdef01.mp4"]);
    }
}
