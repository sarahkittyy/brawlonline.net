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
//! Every changelog stays in the table as the history; only the newest is served for now.

use std::path::Path;

use axum::extract::State;
use axum::http::{header, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::Json;
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use sqlx::PgPool;

use crate::blog::{render, sniff_media, valid_media_name};
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
        sqlx::query("DELETE FROM changelogs WHERE every_start").execute(&mut *tx).await?;
    }
    let (id, published_at): (i64, DateTime<Utc>) =
        sqlx::query_as("INSERT INTO changelogs (body_md, every_start) VALUES ($1, $2) RETURNING id, published_at")
            .bind(&md)
            .bind(every_start)
            .fetch_one(&mut *tx)
            .await?;
    tx.commit().await?;
    Ok(Published { id, published_at, written })
}

/// Removes the staging-only changelog (the board's "show on staging" turned off): the count removed.
pub async fn clear_every_start(pool: &PgPool) -> sqlx::Result<u64> {
    Ok(sqlx::query("DELETE FROM changelogs WHERE every_start").execute(pool).await?.rows_affected())
}

#[derive(Debug, sqlx::FromRow)]
pub struct ChangelogRow {
    pub id: i64,
    pub body_md: String,
    pub published_at: DateTime<Utc>,
    pub every_start: bool,
}

/// Every changelog, newest first.
pub async fn list(pool: &PgPool) -> sqlx::Result<Vec<ChangelogRow>> {
    sqlx::query_as("SELECT id, body_md, published_at, every_start FROM changelogs ORDER BY published_at DESC, id DESC")
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
    /// A launcher shows only one published after the last it showed, and never one published
    /// after the launcher was built (it is about a release still to come).
    pub published_at: DateTime<Utc>,
    /// Staging's "show on staging": shown at every start, whatever the launcher showed before.
    pub every_start: bool,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct LatestResp {
    pub changelog: Option<LatestChangelog>,
}

/// `GET /v1/changelogs/latest` (public): the staging-only changelog if there is one, else the
/// newest; `null` without any. Launchers ask once after an update (staging ones at every start).
pub async fn latest(State(state): State<AppState>) -> Response {
    let row = sqlx::query_as::<_, ChangelogRow>(
        "SELECT id, body_md, published_at, every_start FROM changelogs \
         ORDER BY every_start DESC, published_at DESC, id DESC LIMIT 1",
    )
    .fetch_optional(&state.pool)
    .await;
    match row {
        Ok(row) => {
            let changelog = row.map(|r| LatestChangelog {
                id: r.id,
                html: render(&r.body_md).html,
                published_at: r.published_at,
                every_start: r.every_start,
            });
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

    #[test]
    fn media_names_in_markdown() {
        let md = "![a](/blog/media/0123456789abcdef0123.webp)\n\n![b](/blog/media/0123456789abcdef0123.webp) \
                  [clip](/blog/media/abcdefabcdefabcdef01.mp4). /blog/media/nope.png";
        assert_eq!(media_names(md), vec!["0123456789abcdef0123.webp", "abcdefabcdefabcdef01.mp4"]);
    }
}
