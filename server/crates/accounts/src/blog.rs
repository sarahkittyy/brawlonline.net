//! The blog: `brawlonline.net/blog` (the index: the latest post up top, older ones below) and
//! `/blog/<slug>` (a post), rendered here from `blog_posts` for people, search engines and link
//! previews alike, plus the editor's API (`/v1/blog/...`, admins only: `users.role = 'admin'`) and
//! the uploaded images and videos (`/blog/media/<name>`). The editor itself is a static page on
//! the website (`website/blog/write/`), which logs in with the same accounts as the launcher
//! (`website/auth/auth.js`). Design and the reasons: docs/design/blog.md.
//!
//! Posts are Markdown (CommonMark with GitHub's tables, strikethrough and task lists). Raw HTML in
//! a post is shown as text (HTML comments are dropped, `<br>` is kept); an image whose address
//! ends in .mp4/.webm/.mov is a video; an image alone in its paragraph is a figure with its alt
//! text as the caption. The editor's preview is this same renderer (`POST /v1/blog/render`).

use std::net::SocketAddr;
use std::path::Path as FsPath;

use axum::body::Body;
use axum::extract::{ConnectInfo, Path, Query, Request, State};
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Redirect, Response};
use axum::Json;
use chrono::{DateTime, Utc};
use common::net::ip_key;
use common::playkey::random_token;
use pulldown_cmark::{Event, HeadingLevel, Options, Parser, Tag, TagEnd};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tokio::io::AsyncWriteExt;

use crate::api::{client_ip, current_user, limit};
use crate::error::{ApiError, ApiResult};
use crate::pages::esc;
use crate::profile::json_ld;
use crate::store::UserRow;
use crate::AppState;

/// The largest post (its Markdown, title and the rest together) the editor may save.
pub const MAX_POST_BYTES: usize = 1024 * 1024;
const MAX_TITLE: usize = 200;
const MAX_SUMMARY: usize = 400;
const MAX_AUTHOR: usize = 80;
const MAX_SLUG: usize = 80;
/// Addresses under /blog/ that are not posts.
const RESERVED_SLUGS: [&str; 2] = ["write", "media"];
/// The cover when a post has no image: the home page's video still.
const DEFAULT_COVER: &str = "/assets/bg-poster.jpg";

// ---------------------------------------------------------------- the posts

#[derive(Debug, Clone, sqlx::FromRow)]
struct PostRow {
    id: i64,
    slug: String,
    title: String,
    summary: String,
    author: String,
    body_md: String,
    status: String,
    published_at: Option<DateTime<Utc>>,
    preview_token: String,
    created_at: DateTime<Utc>,
    updated_at: DateTime<Utc>,
}

const POST_COLUMNS: &str =
    "id, slug, title, summary, author, body_md, status, published_at, preview_token, created_at, updated_at";

/// A post as the editor sees it.
#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Post {
    pub id: i64,
    pub slug: String,
    pub title: String,
    pub summary: String,
    pub author: String,
    pub body: String,
    /// `draft` or `published`.
    pub status: String,
    pub published_at: Option<DateTime<Utc>>,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
    /// Where to look at it: the post for a published one, the preview link for a draft.
    pub view_url: String,
}

impl From<PostRow> for Post {
    fn from(r: PostRow) -> Self {
        let view_url = if r.status == "published" {
            format!("/blog/{}", r.slug)
        } else {
            format!("/blog/{}?preview={}", r.slug, r.preview_token)
        };
        Post {
            id: r.id,
            slug: r.slug,
            title: r.title,
            summary: r.summary,
            author: r.author,
            body: r.body_md,
            status: r.status,
            published_at: r.published_at,
            created_at: r.created_at,
            updated_at: r.updated_at,
            view_url,
        }
    }
}

/// A row of the editor's list of posts.
#[derive(Debug, Serialize, Deserialize, sqlx::FromRow)]
#[serde(rename_all = "camelCase")]
pub struct PostListItem {
    pub id: i64,
    pub slug: String,
    pub title: String,
    pub status: String,
    pub published_at: Option<DateTime<Utc>>,
    pub updated_at: DateTime<Utc>,
}

/// The account, if it may write the blog: logged in (Bearer session) with `role = 'admin'`.
async fn require_admin(state: &AppState, headers: &HeaderMap) -> ApiResult<UserRow> {
    let user = current_user(state, headers).await?;
    if user.role != "admin" {
        return Err(ApiError::forbidden("not_admin", "This account can't write the blog."));
    }
    Ok(user)
}

/// A post's address: lowercase letters, digits and single dashes, not at either end.
pub fn check_slug(slug: &str) -> Result<(), String> {
    if slug.is_empty() {
        return Err("The address can't be empty.".into());
    }
    if slug.len() > MAX_SLUG {
        return Err(format!("The address is at most {MAX_SLUG} characters."));
    }
    let ok = slug.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
        && !slug.starts_with('-')
        && !slug.ends_with('-')
        && !slug.contains("--");
    if !ok {
        return Err("Use lowercase letters, digits and dashes (like progress-report-1).".into());
    }
    if RESERVED_SLUGS.contains(&slug) {
        return Err(format!("\"{slug}\" is taken by the blog itself."));
    }
    Ok(())
}

/// An address made from a title: "Progress Report #1!" -> "progress-report-1".
pub fn slugify(title: &str) -> String {
    let mut out = String::new();
    for c in title.chars().flat_map(char::to_lowercase) {
        if c.is_ascii_alphanumeric() {
            out.push(c);
        } else if !out.is_empty() && !out.ends_with('-') {
            out.push('-');
        }
        if out.len() >= MAX_SLUG {
            break;
        }
    }
    let out = out.trim_end_matches('-').to_string();
    if out.is_empty() || RESERVED_SLUGS.contains(&out.as_str()) {
        "post".into()
    } else {
        out
    }
}

/// `base`, or `base-2`, `base-3`... whichever no post has yet.
async fn free_slug(state: &AppState, base: &str) -> ApiResult<String> {
    let taken: Vec<String> = sqlx::query_scalar("SELECT slug FROM blog_posts WHERE slug = $1 OR slug LIKE $1 || '-%'")
        .bind(base)
        .fetch_all(&state.pool)
        .await?;
    if !taken.iter().any(|s| s == base) {
        return Ok(base.to_string());
    }
    let n = (2..).find(|n| !taken.iter().any(|s| *s == format!("{base}-{n}"))).unwrap_or(2);
    Ok(format!("{base}-{n}"))
}

fn too_long(what: &str, max: usize) -> ApiError {
    ApiError::bad_request("too_long", format!("The {what} is at most {max} characters."))
}

/// `GET /v1/blog/posts`: every post, drafts first (newest change first), then the published ones
/// (newest first).
pub async fn list_posts(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<Vec<PostListItem>>> {
    require_admin(&state, &headers).await?;
    let posts = sqlx::query_as::<_, PostListItem>(
        "SELECT id, slug, title, status, published_at, updated_at FROM blog_posts
          ORDER BY (status = 'draft') DESC, COALESCE(published_at, updated_at) DESC, id DESC",
    )
    .fetch_all(&state.pool)
    .await?;
    Ok(Json(posts))
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct CreateReq {
    pub title: String,
    pub body: String,
}

/// `POST /v1/blog/posts`: a new draft (an empty one with `{}`).
pub async fn create_post(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<CreateReq>,
) -> ApiResult<(StatusCode, Json<Post>)> {
    let user = require_admin(&state, &headers).await?;
    if req.title.chars().count() > MAX_TITLE {
        return Err(too_long("title", MAX_TITLE));
    }
    let base = if req.title.trim().is_empty() { "untitled".to_string() } else { slugify(&req.title) };
    let slug = free_slug(&state, &base).await?;
    let row = sqlx::query_as::<_, PostRow>(&format!(
        "INSERT INTO blog_posts (slug, title, body_md, preview_token, created_by) VALUES ($1, $2, $3, $4, $5)
         RETURNING {POST_COLUMNS}"
    ))
    .bind(&slug)
    .bind(req.title.trim())
    .bind(&req.body)
    .bind(random_token())
    .bind(user.uid)
    .fetch_one(&state.pool)
    .await?;
    tracing::info!(id = row.id, slug = %row.slug, by = %user.uid, "blog post created");
    Ok((StatusCode::CREATED, Json(row.into())))
}

async fn post_by_id(state: &AppState, id: i64) -> ApiResult<PostRow> {
    sqlx::query_as::<_, PostRow>(&format!("SELECT {POST_COLUMNS} FROM blog_posts WHERE id = $1"))
        .bind(id)
        .fetch_optional(&state.pool)
        .await?
        .ok_or_else(ApiError::not_found)
}

/// `GET /v1/blog/posts/<id>`.
pub async fn get_post(State(state): State<AppState>, headers: HeaderMap, Path(id): Path<i64>) -> ApiResult<Json<Post>> {
    require_admin(&state, &headers).await?;
    Ok(Json(post_by_id(&state, id).await?.into()))
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateReq {
    pub slug: String,
    pub title: String,
    #[serde(default)]
    pub summary: String,
    #[serde(default)]
    pub author: String,
    pub body: String,
    /// The `updatedAt` the editor last saw: a save over a newer change (another tab) is refused
    /// with 409 `changed_elsewhere` instead of overwriting it. Without it, the save always goes.
    #[serde(default)]
    pub expected_updated_at: Option<DateTime<Utc>>,
}

/// `PUT /v1/blog/posts/<id>`: saves a post. A published post's change is live at once.
pub async fn update_post(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<i64>,
    Json(req): Json<UpdateReq>,
) -> ApiResult<Json<Post>> {
    require_admin(&state, &headers).await?;
    let slug = req.slug.trim();
    check_slug(slug).map_err(|m| ApiError::bad_request("bad_slug", m))?;
    if req.title.chars().count() > MAX_TITLE {
        return Err(too_long("title", MAX_TITLE));
    }
    if req.summary.chars().count() > MAX_SUMMARY {
        return Err(too_long("summary", MAX_SUMMARY));
    }
    if req.author.chars().count() > MAX_AUTHOR {
        return Err(too_long("author", MAX_AUTHOR));
    }
    if req.body.len() > MAX_POST_BYTES {
        return Err(ApiError::new(StatusCode::PAYLOAD_TOO_LARGE, "too_large", "A post is at most 1 MB of text."));
    }
    let taken: Option<i64> = sqlx::query_scalar("SELECT id FROM blog_posts WHERE slug = $1 AND id <> $2")
        .bind(slug)
        .bind(id)
        .fetch_optional(&state.pool)
        .await?;
    if taken.is_some() {
        return Err(ApiError::conflict("slug_taken", "Another post has this address."));
    }
    let row = sqlx::query_as::<_, PostRow>(&format!(
        "UPDATE blog_posts SET slug = $2, title = $3, summary = $4, author = $5, body_md = $6, updated_at = now()
          WHERE id = $1 AND ($7::timestamptz IS NULL OR updated_at = $7)
          RETURNING {POST_COLUMNS}"
    ))
    .bind(id)
    .bind(slug)
    .bind(req.title.trim())
    .bind(req.summary.trim())
    .bind(req.author.trim())
    .bind(&req.body)
    .bind(req.expected_updated_at)
    .fetch_optional(&state.pool)
    .await;
    let row = match row {
        Ok(Some(row)) => row,
        Ok(None) => {
            // Gone, or changed since the editor loaded it.
            post_by_id(&state, id).await?;
            return Err(ApiError::conflict(
                "changed_elsewhere",
                "This post was changed somewhere else (another tab?). Reload it to see that version.",
            ));
        }
        Err(sqlx::Error::Database(e)) if e.is_unique_violation() => {
            return Err(ApiError::conflict("slug_taken", "Another post has this address."));
        }
        Err(e) => return Err(e.into()),
    };
    Ok(Json(row.into()))
}

/// `POST /v1/blog/posts/<id>/publish`: on the blog from now on. The first publish sets the date
/// the pages show; publishing again after an unpublish keeps it.
pub async fn publish_post(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<i64>,
) -> ApiResult<Json<Post>> {
    let user = require_admin(&state, &headers).await?;
    let post = post_by_id(&state, id).await?;
    if post.title.trim().is_empty() || post.body_md.trim().is_empty() {
        return Err(ApiError::bad_request("empty", "Give the post a title and some text first."));
    }
    let row = sqlx::query_as::<_, PostRow>(&format!(
        "UPDATE blog_posts SET status = 'published', published_at = COALESCE(published_at, now()), updated_at = now()
          WHERE id = $1 RETURNING {POST_COLUMNS}"
    ))
    .bind(id)
    .fetch_one(&state.pool)
    .await?;
    tracing::info!(id, slug = %row.slug, by = %user.uid, "blog post published");
    Ok(Json(row.into()))
}

/// `POST /v1/blog/posts/<id>/unpublish`: back to a draft (its page 404s, its preview link works).
pub async fn unpublish_post(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<i64>,
) -> ApiResult<Json<Post>> {
    let user = require_admin(&state, &headers).await?;
    let row = sqlx::query_as::<_, PostRow>(&format!(
        "UPDATE blog_posts SET status = 'draft', updated_at = now() WHERE id = $1 RETURNING {POST_COLUMNS}"
    ))
    .bind(id)
    .fetch_optional(&state.pool)
    .await?
    .ok_or_else(ApiError::not_found)?;
    tracing::info!(id, slug = %row.slug, by = %user.uid, "blog post unpublished");
    Ok(Json(row.into()))
}

/// `DELETE /v1/blog/posts/<id>`. Its images and videos stay (another post may use them).
pub async fn delete_post(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<i64>,
) -> ApiResult<StatusCode> {
    let user = require_admin(&state, &headers).await?;
    let gone = sqlx::query("DELETE FROM blog_posts WHERE id = $1").bind(id).execute(&state.pool).await?;
    if gone.rows_affected() == 0 {
        return Err(ApiError::not_found());
    }
    tracing::info!(id, by = %user.uid, "blog post deleted");
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Debug, Deserialize)]
pub struct RenderReq {
    pub markdown: String,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RenderResp {
    pub html: String,
    pub reading_minutes: usize,
    /// `Rendered::blocks`: the source line of each top-level element of `html`.
    pub blocks: Vec<usize>,
}

/// `POST /v1/blog/render`: the editor's preview, from the same renderer as the pages.
pub async fn render_preview(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<RenderReq>,
) -> ApiResult<Json<RenderResp>> {
    require_admin(&state, &headers).await?;
    let r = render(&req.markdown);
    Ok(Json(RenderResp { reading_minutes: reading_minutes(r.words), html: r.html, blocks: r.blocks }))
}

// ---------------------------------------------------------------- images and videos

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MediaKind {
    Image,
    Video,
}

impl MediaKind {
    fn as_str(self) -> &'static str {
        match self {
            MediaKind::Image => "image",
            MediaKind::Video => "video",
        }
    }
}

const IMAGE_EXTS: [&str; 5] = ["png", "jpg", "gif", "webp", "avif"];
const VIDEO_EXTS: [&str; 3] = ["mp4", "webm", "mov"];

/// What an upload is, from its first bytes (never its name or Content-Type): the extension it is
/// stored with. SVG and anything else is refused (an SVG can carry script).
pub fn sniff_media(head: &[u8]) -> Option<(MediaKind, &'static str)> {
    let starts = |magic: &[u8]| head.starts_with(magic);
    if starts(b"\x89PNG\r\n\x1a\n") {
        return Some((MediaKind::Image, "png"));
    }
    if starts(b"\xff\xd8\xff") {
        return Some((MediaKind::Image, "jpg"));
    }
    if starts(b"GIF87a") || starts(b"GIF89a") {
        return Some((MediaKind::Image, "gif"));
    }
    if head.len() >= 12 && &head[0..4] == b"RIFF" && &head[8..12] == b"WEBP" {
        return Some((MediaKind::Image, "webp"));
    }
    if starts(b"\x1a\x45\xdf\xa3") {
        return Some((MediaKind::Video, "webm"));
    }
    if head.len() >= 12 && &head[4..8] == b"ftyp" {
        return Some(match &head[8..12] {
            b"avif" | b"avis" => (MediaKind::Image, "avif"),
            b"qt  " => (MediaKind::Video, "mov"),
            _ => (MediaKind::Video, "mp4"),
        });
    }
    None
}

/// The movie timescale uploads get (ticks per second): what ffmpeg writes by default.
const MOVIE_TIMESCALE: u32 = 1000;

/// The child boxes of an MP4 box's payload: (type, start, header length, size). Stops at the first
/// box that doesn't fit.
fn mp4_boxes(buf: &[u8]) -> Vec<([u8; 4], usize, usize, usize)> {
    let mut out = Vec::new();
    let mut i = 0;
    while i + 8 <= buf.len() {
        let size32 = u32::from_be_bytes(buf[i..i + 4].try_into().unwrap());
        let typ: [u8; 4] = buf[i + 4..i + 8].try_into().unwrap();
        let (size, hdr) = match size32 {
            0 => ((buf.len() - i) as u64, 8),
            1 if i + 16 <= buf.len() => (u64::from_be_bytes(buf[i + 8..i + 16].try_into().unwrap()), 16),
            1 => break,
            n => (n as u64, 8),
        };
        if size < hdr as u64 || size > (buf.len() - i) as u64 {
            break;
        }
        out.push((typ, i, hdr, size as usize));
        i += size as usize;
    }
    out
}

/// Rescales the u32 (or, `long`, u64) duration at `buf[at..]` from `from` ticks a second to
/// `MOVIE_TIMESCALE`. All ones means "unknown" and stays. False when it doesn't fit in `buf`.
fn rescale_field(buf: &mut [u8], at: usize, long: bool, from: u32) -> bool {
    let scale = |v: u64| ((v as u128 * MOVIE_TIMESCALE as u128 + from as u128 / 2) / from as u128) as u64;
    if long {
        let Some(f) = buf.get_mut(at..at + 8) else { return false };
        let v = u64::from_be_bytes(f.try_into().unwrap());
        if v != u64::MAX {
            f.copy_from_slice(&scale(v).to_be_bytes());
        }
    } else {
        let Some(f) = buf.get_mut(at..at + 4) else { return false };
        let v = u32::from_be_bytes(f.try_into().unwrap());
        if v != u32::MAX {
            f.copy_from_slice(&(scale(v as u64) as u32).to_be_bytes());
        }
    }
    true
}

/// Sets an MP4/MOV's movie timescale to `MOVIE_TIMESCALE` when it is finer, rescaling what is
/// counted in it (mvhd's duration, each tkhd's, each edit's, mehd's), as an ffmpeg remux does.
/// `moov` is the moov box's payload; sizes don't change. Returns whether it changed anything.
///
/// ffmpeg 8 writes the tracks' common multiple as the movie timescale (47952000 for 59.94 fps
/// video with 48 kHz audio), and Firefox then shows a video's first frame only while its time and
/// sound run on (Sarah's board, 2026-11-12: "i also see the video frozen ... it's stuck on one
/// frame"). The tracks' own timescales (the samples' times) are untouched.
pub fn normalize_movie_timescale(moov: &mut [u8]) -> bool {
    let kids = mp4_boxes(moov);
    let Some(&(_, at, hdr, size)) = kids.iter().find(|b| &b.0 == b"mvhd") else { return false };
    let mvhd = &mut moov[at + hdr..at + size];
    // version, flags, creation and modification times, then the timescale and the duration.
    let (ts_at, long) = match mvhd.first() {
        Some(0) => (12, false),
        Some(1) => (20, true),
        _ => return false,
    };
    let Some(ts) = mvhd.get(ts_at..ts_at + 4) else { return false };
    let from = u32::from_be_bytes(ts.try_into().unwrap());
    if from <= MOVIE_TIMESCALE || mvhd.len() < ts_at + 4 + if long { 8 } else { 4 } {
        return false;
    }
    // Check every field fits before changing any, so a cut-off box changes nothing.
    let mut fields: Vec<(usize, bool)> = vec![(at + hdr + ts_at + 4, long)];
    for &(typ, t_at, t_hdr, t_size) in &kids {
        let body = t_at + t_hdr..t_at + t_size;
        match &typ {
            b"trak" => {
                for (k, k_at, k_hdr, k_size) in mp4_boxes(&moov[body.clone()]) {
                    let k_at = body.start + k_at;
                    match &k {
                        // version, flags, times, track id, reserved, then the duration.
                        b"tkhd" => match moov.get(k_at + k_hdr) {
                            Some(0) => fields.push((k_at + k_hdr + 20, false)),
                            Some(1) => fields.push((k_at + k_hdr + 28, true)),
                            _ => return false,
                        },
                        b"edts" => {
                            let edts = k_at + k_hdr..k_at + k_size;
                            for (e, e_at, e_hdr, e_size) in mp4_boxes(&moov[edts.clone()]) {
                                if &e != b"elst" {
                                    continue;
                                }
                                let e_body = edts.start + e_at + e_hdr;
                                let Some(head) = moov.get(e_body..e_body + 8) else { return false };
                                let long = head[0] == 1;
                                let n = u32::from_be_bytes(head[4..8].try_into().unwrap()) as usize;
                                let entry = if long { 20 } else { 12 };
                                if 8 + n * entry > e_size - e_hdr {
                                    return false;
                                }
                                // Each edit: its duration (movie timescale), then where it starts
                                // in the track (the track's own timescale) and its rate.
                                fields.extend((0..n).map(|j| (e_body + 8 + j * entry, long)));
                            }
                        }
                        _ => {}
                    }
                }
            }
            b"mvex" => {
                for (k, k_at, k_hdr, _) in mp4_boxes(&moov[body.clone()]) {
                    let k_body = body.start + k_at + k_hdr;
                    if &k == b"mehd" {
                        fields.push((k_body + 4, moov.get(k_body) == Some(&1)));
                    }
                }
            }
            _ => {}
        }
    }
    if fields.iter().any(|&(f, long)| f + if long { 8 } else { 4 } > moov.len()) {
        return false;
    }
    for (f, long) in fields {
        rescale_field(moov, f, long, from);
    }
    moov[at + hdr + ts_at..at + hdr + ts_at + 4].copy_from_slice(&MOVIE_TIMESCALE.to_be_bytes());
    true
}

/// `normalize_movie_timescale` on an MP4/MOV file, in place (the moov box keeps its size and
/// place). Returns whether the file changed.
fn normalize_video_file(path: &FsPath) -> std::io::Result<bool> {
    use std::io::{Read, Seek, SeekFrom, Write};
    let mut f = std::fs::OpenOptions::new().read(true).write(true).open(path)?;
    let len = f.metadata()?.len();
    let mut pos = 0u64;
    while pos + 8 <= len {
        let mut h = [0u8; 8];
        f.seek(SeekFrom::Start(pos))?;
        f.read_exact(&mut h)?;
        let (size, hdr) = match u32::from_be_bytes(h[..4].try_into().unwrap()) {
            0 => (len - pos, 8),
            1 => {
                let mut l = [0u8; 8];
                f.read_exact(&mut l)?;
                (u64::from_be_bytes(l), 16)
            }
            n => (n as u64, 8),
        };
        if size < hdr || pos + size > len {
            return Ok(false);
        }
        if &h[4..8] == b"moov" {
            if size > 64 << 20 {
                return Ok(false);
            }
            let mut moov = vec![0u8; (size - hdr) as usize];
            f.read_exact(&mut moov)?;
            if !normalize_movie_timescale(&mut moov) {
                return Ok(false);
            }
            f.seek(SeekFrom::Start(pos + hdr))?;
            f.write_all(&moov)?;
            f.flush()?;
            return Ok(true);
        }
        pos += size;
    }
    Ok(false)
}

fn sha256_file(path: &FsPath) -> std::io::Result<[u8; 32]> {
    let mut hasher = Sha256::new();
    std::io::copy(&mut std::fs::File::open(path)?, &mut hasher)?;
    Ok(hasher.finalize().into())
}

/// A stored file's name: 20 hex digits of the content's SHA-256 and a known extension.
pub fn valid_media_name(name: &str) -> bool {
    let Some((stem, ext)) = name.split_once('.') else { return false };
    stem.len() == 20
        && stem.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        && (IMAGE_EXTS.contains(&ext) || VIDEO_EXTS.contains(&ext))
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MediaResp {
    /// Where the post links it: `/blog/media/<name>`.
    pub url: String,
    pub name: String,
    /// `image` or `video`.
    pub kind: String,
    pub bytes: u64,
}

fn media_too_large(max: u64) -> ApiError {
    ApiError::new(StatusCode::PAYLOAD_TOO_LARGE, "too_large", format!("A file is at most {} MB.", max / (1024 * 1024)))
}

fn media_storage(e: impl std::fmt::Display) -> ApiError {
    tracing::error!("blog media storage: {e}");
    ApiError::new(StatusCode::SERVICE_UNAVAILABLE, "storage_unavailable", "The server could not store the file.")
}

/// `PUT /v1/blog/media`: an image or video, the raw file as the body (`X-File-Name`: its name, URI
/// encoded, kept for the record). Stored once per content; answers where the post links it.
pub async fn upload_media(State(state): State<AppState>, headers: HeaderMap, body: Body) -> ApiResult<Json<MediaResp>> {
    let user = require_admin(&state, &headers).await?;
    let max = state.cfg.blog_media_max_bytes;
    let declared =
        headers.get(header::CONTENT_LENGTH).and_then(|v| v.to_str().ok()).and_then(|v| v.parse::<u64>().ok());
    if declared.is_some_and(|n| n > max) {
        return Err(media_too_large(max));
    }
    let stored: i64 =
        sqlx::query_scalar("SELECT COALESCE(sum(bytes), 0)::bigint FROM blog_media").fetch_one(&state.pool).await?;
    if stored as u64 + declared.unwrap_or(0) > state.cfg.blog_media_max_total_bytes {
        tracing::warn!(stored, "blog upload refused: BLOG_MEDIA_MAX_TOTAL_BYTES reached");
        return Err(ApiError::new(
            StatusCode::INSUFFICIENT_STORAGE,
            "storage_full",
            "The blog's storage is full (BLOG_MEDIA_MAX_TOTAL_BYTES).",
        ));
    }
    let original_name: String = headers
        .get("x-file-name")
        .and_then(|v| v.to_str().ok())
        .map(|v| v.chars().take(200).collect())
        .unwrap_or_default();

    let dir = &state.cfg.blog_media_dir;
    tokio::fs::create_dir_all(dir).await.map_err(|e| media_storage(format!("{}: {e}", dir.display())))?;
    let partial = dir.join(format!(".upload-{}.partial", &random_token()[..12]));
    let written = write_media(body, &partial, max).await;
    let (size, mut digest, head) = match written {
        Ok(w) => w,
        Err(e) => {
            let _ = tokio::fs::remove_file(&partial).await;
            return Err(e);
        }
    };
    let Some((kind, ext)) = sniff_media(&head) else {
        let _ = tokio::fs::remove_file(&partial).await;
        return Err(ApiError::new(
            StatusCode::UNSUPPORTED_MEDIA_TYPE,
            "unsupported_type",
            "Only PNG, JPEG, GIF, WebP and AVIF images and MP4, WebM and MOV videos.",
        ));
    };
    if kind == MediaKind::Video && ext != "webm" {
        // A movie timescale Firefox can't play (normalize_movie_timescale): fixed before it is named,
        // so the same upload again gets the same name.
        let p = partial.clone();
        let fixed = tokio::task::spawn_blocking(move || -> std::io::Result<Option<[u8; 32]>> {
            Ok(if normalize_video_file(&p)? { Some(sha256_file(&p)?) } else { None })
        })
        .await
        .map_err(std::io::Error::other)
        .and_then(|r| r);
        match fixed {
            Ok(Some(d)) => {
                tracing::info!(original_name, "blog upload: movie timescale set to {MOVIE_TIMESCALE}");
                digest = d;
            }
            Ok(None) => {}
            Err(e) => {
                let _ = tokio::fs::remove_file(&partial).await;
                return Err(media_storage(format!("{}: {e}", partial.display())));
            }
        }
    }
    let name = format!("{}.{ext}", &hex::encode(digest)[..20]);
    let dest = dir.join(&name);
    if tokio::fs::try_exists(&dest).await.unwrap_or(false) {
        let _ = tokio::fs::remove_file(&partial).await;
    } else if let Err(e) = tokio::fs::rename(&partial, &dest).await {
        let _ = tokio::fs::remove_file(&partial).await;
        return Err(media_storage(format!("{}: {e}", dest.display())));
    }
    sqlx::query(
        "INSERT INTO blog_media (name, kind, bytes, original_name, uploaded_by) VALUES ($1, $2, $3, $4, $5)
         ON CONFLICT (name) DO NOTHING",
    )
    .bind(&name)
    .bind(kind.as_str())
    .bind(size as i64)
    .bind(&original_name)
    .bind(user.uid)
    .execute(&state.pool)
    .await?;
    tracing::info!(%name, size, by = %user.uid, "blog media uploaded");
    Ok(Json(MediaResp { url: format!("/blog/media/{name}"), name, kind: kind.as_str().into(), bytes: size }))
}

/// Streams `body` to `path` (at most `max` bytes): its size, SHA-256 and first 16 bytes.
async fn write_media(body: Body, path: &FsPath, max: u64) -> ApiResult<(u64, [u8; 32], Vec<u8>)> {
    let mut file =
        tokio::fs::File::create(path).await.map_err(|e| media_storage(format!("{}: {e}", path.display())))?;
    let mut stream = std::pin::pin!(body.into_data_stream());
    let mut size = 0u64;
    let mut head = Vec::with_capacity(16);
    let mut hasher = Sha256::new();
    while let Some(chunk) = std::future::poll_fn(|cx| futures_core::Stream::poll_next(stream.as_mut(), cx)).await {
        // The body limit layer ends the stream with an error past its limit.
        let chunk = chunk.map_err(|_| media_too_large(max))?;
        size += chunk.len() as u64;
        if size > max {
            return Err(media_too_large(max));
        }
        if head.len() < 16 {
            head.extend(chunk.iter().take(16 - head.len()));
        }
        hasher.update(&chunk);
        file.write_all(&chunk).await.map_err(|e| media_storage(format!("{}: {e}", path.display())))?;
    }
    file.flush().await.map_err(|e| media_storage(format!("{}: {e}", path.display())))?;
    if size == 0 {
        return Err(ApiError::bad_request("empty", "The file is empty."));
    }
    Ok((size, hasher.finalize().into(), head))
}

/// `GET /blog/media/<name>`: an uploaded file, with byte ranges (video seeking). Names are content
/// hashes, so it is cached for good.
pub async fn media_file(State(state): State<AppState>, Path(name): Path<String>, req: Request) -> Response {
    if !valid_media_name(&name) {
        return StatusCode::NOT_FOUND.into_response();
    }
    let path = state.cfg.blog_media_dir.join(&name);
    match tower_http::services::ServeFile::new(path).try_call(req).await {
        Ok(resp) => {
            let ok = resp.status().is_success();
            let mut resp = resp.map(Body::new);
            if ok {
                resp.headers_mut()
                    .insert(header::CACHE_CONTROL, HeaderValue::from_static("public, max-age=31536000, immutable"));
            }
            resp.headers_mut().insert(header::X_CONTENT_TYPE_OPTIONS, HeaderValue::from_static("nosniff"));
            resp
        }
        Err(e) => {
            tracing::error!("blog media {name}: {e}");
            StatusCode::INTERNAL_SERVER_ERROR.into_response()
        }
    }
}

// ---------------------------------------------------------------- Markdown

/// A post's Markdown, rendered.
#[derive(Debug, Default)]
pub struct Rendered {
    pub html: String,
    /// The first image on the site (`/...`): the index's cover and the link preview's image.
    pub first_image: Option<String>,
    /// The first paragraph's text: the summary when the post has none.
    pub first_paragraph: String,
    pub words: usize,
    /// The source line (0-based) where each top-level element of `html` starts: the editor keeps
    /// its preview level with the Markdown by these.
    pub blocks: Vec<usize>,
}

#[derive(Debug, PartialEq, Eq)]
enum UrlKind {
    /// On this site: `/x`, `#x`, `x/y`.
    Local,
    /// http(s), protocol-relative or mailto.
    External,
    /// Any other scheme (javascript:, data:, ...): not linked.
    Bad,
}

fn url_kind(url: &str) -> UrlKind {
    let u = url.trim();
    if u.starts_with("//") {
        return UrlKind::External;
    }
    let scheme_end = u.find(':');
    let path_start = u.find(['/', '?', '#']);
    match scheme_end {
        Some(i) if path_start.is_none_or(|p| i < p) => {
            let scheme = u[..i].to_ascii_lowercase();
            if matches!(scheme.as_str(), "http" | "https" | "mailto") {
                UrlKind::External
            } else {
                UrlKind::Bad
            }
        }
        _ => UrlKind::Local,
    }
}

fn is_video_url(url: &str) -> bool {
    let path = url.split(['?', '#']).next().unwrap_or("").to_ascii_lowercase();
    VIDEO_EXTS.iter().any(|ext| path.ends_with(&format!(".{ext}")))
}

/// The `id` of a heading: its text in lowercase, dashes between words.
fn heading_id(text: &str) -> String {
    let mut out = String::new();
    for c in text.chars().flat_map(char::to_lowercase) {
        if c.is_alphanumeric() {
            out.push(c);
        } else if !out.is_empty() && !out.ends_with('-') {
            out.push('-');
        }
    }
    let out = out.trim_end_matches('-');
    if out.is_empty() {
        "section".into()
    } else {
        out.into()
    }
}

fn plain_text(events: &[Event]) -> String {
    let mut s = String::new();
    for e in events {
        match e {
            Event::Text(t) | Event::Code(t) => s.push_str(t),
            Event::SoftBreak | Event::HardBreak => s.push(' '),
            _ => {}
        }
    }
    s
}

/// `<img>` or `<video>` (or, for an image elsewhere on the web, a link to it: the pages load
/// nothing from other sites).
fn media_html(url: &str, alt: &str) -> String {
    match url_kind(url) {
        UrlKind::Bad => esc(alt),
        UrlKind::External => {
            let text = if alt.is_empty() { url } else { alt };
            format!("<a href=\"{}\" rel=\"noopener\">{}</a>", esc(url), esc(text))
        }
        UrlKind::Local if is_video_url(url) => {
            let label = if alt.is_empty() { String::new() } else { format!(" aria-label=\"{}\"", esc(alt)) };
            format!("<video src=\"{}\" controls preload=\"metadata\" playsinline{label}></video>", esc(url))
        }
        UrlKind::Local => format!("<img src=\"{}\" alt=\"{}\" loading=\"lazy\">", esc(url), esc(alt)),
    }
}

/// Where the HTML block starting at `events[i]` ends, when it is a comment (written as nothing).
fn comment_block(events: &[Event], i: usize) -> Option<usize> {
    if !matches!(events[i], Event::Start(Tag::HtmlBlock)) {
        return None;
    }
    let mut j = i + 1;
    let mut text = String::new();
    while j < events.len() && !matches!(events[j], Event::End(TagEnd::HtmlBlock)) {
        if let Event::Html(s) = &events[j] {
            text.push_str(s);
        }
        j += 1;
    }
    text.trim_start().starts_with("<!--").then_some(j)
}

/// The source line of each top-level element `render` writes: every top-level block but a
/// comment. An image alone in its paragraph stays one element (the figure).
fn block_lines(md: &str, events: &[Event], starts: &[usize]) -> Vec<usize> {
    let mut lines = Vec::new();
    let mut depth = 0usize;
    let (mut pos, mut line) = (0usize, 0usize);
    for (i, e) in events.iter().enumerate() {
        let top = match e {
            Event::Start(_) => {
                depth += 1;
                depth == 1 && comment_block(events, i).is_none()
            }
            Event::End(_) => {
                depth = depth.saturating_sub(1);
                false
            }
            Event::Rule => depth == 0,
            _ => false,
        };
        if top {
            let at = starts[i].clamp(pos, md.len());
            line += md.as_bytes()[pos..at].iter().filter(|&&b| b == b'\n').count();
            pos = at;
            lines.push(line);
        }
    }
    lines
}

pub fn render(md: &str) -> Rendered {
    let opts =
        Options::ENABLE_TABLES | Options::ENABLE_STRIKETHROUGH | Options::ENABLE_TASKLISTS | Options::ENABLE_FOOTNOTES;
    let (events, starts): (Vec<Event>, Vec<usize>) =
        Parser::new_ext(md, opts).into_offset_iter().map(|(e, range)| (e, range.start)).unzip();
    let mut out: Vec<Event> = Vec::with_capacity(events.len());
    let mut r = Rendered { blocks: block_lines(md, &events, &starts), ..Rendered::default() };
    let mut ids: Vec<String> = Vec::new();
    // Whether each open link was written (a link to a bad URL is only its text).
    let mut links: Vec<bool> = Vec::new();
    let mut i = 0;
    while i < events.len() {
        match &events[i] {
            Event::Text(t) => {
                r.words += t.split_whitespace().count();
                out.push(events[i].clone());
            }
            // Raw HTML shows as text; comments are dropped, line breaks kept.
            Event::Html(s) | Event::InlineHtml(s) => {
                let t = s.trim();
                if t.starts_with("<!--") {
                    // An inline comment (a block one is skipped whole below).
                } else if matches!(t.to_ascii_lowercase().as_str(), "<br>" | "<br/>" | "<br />") {
                    out.push(Event::Html("<br>".into()));
                } else {
                    out.push(Event::Text(s.clone()));
                }
            }
            Event::Start(Tag::HtmlBlock) => {
                // A block comment: skip the whole block.
                if let Some(j) = comment_block(&events, i) {
                    i = j + 1;
                    continue;
                }
                out.push(Event::Html("<p>".into()));
            }
            Event::End(TagEnd::HtmlBlock) => {
                out.push(Event::Html("</p>\n".into()));
            }
            Event::Start(Tag::Paragraph) if r.first_paragraph.is_empty() => {
                let end = (i..events.len()).find(|&j| matches!(events[j], Event::End(TagEnd::Paragraph)));
                if let Some(end) = end {
                    r.first_paragraph = plain_text(&events[i + 1..end]).trim().to_string();
                }
                out.push(events[i].clone());
            }
            // `#` in the text is an h2 like `##`: the post's title is the page's only h1.
            Event::Start(Tag::Heading { level, .. })
                if matches!(level, HeadingLevel::H1 | HeadingLevel::H2 | HeadingLevel::H3) =>
            {
                let end = (i..events.len()).find(|&j| matches!(events[j], Event::End(TagEnd::Heading(_))));
                let text = end.map(|end| plain_text(&events[i + 1..end])).unwrap_or_default();
                let base = heading_id(&text);
                let mut id = base.clone();
                let mut n = 2;
                while ids.contains(&id) {
                    id = format!("{base}-{n}");
                    n += 1;
                }
                ids.push(id.clone());
                let tag = if *level == HeadingLevel::H3 { "h3" } else { "h2" };
                out.push(Event::Html(format!("<{tag} id=\"{}\">", esc(&id)).into()));
            }
            Event::Start(Tag::Link { dest_url, title, .. }) => {
                let kind = url_kind(dest_url);
                if kind == UrlKind::Bad {
                    links.push(false);
                } else {
                    links.push(true);
                    let title = if title.is_empty() { String::new() } else { format!(" title=\"{}\"", esc(title)) };
                    let rel = if kind == UrlKind::External { " rel=\"noopener\"" } else { "" };
                    out.push(Event::Html(format!("<a href=\"{}\"{title}{rel}>", esc(dest_url)).into()));
                }
            }
            Event::End(TagEnd::Heading(HeadingLevel::H1)) => out.push(Event::Html("</h2>\n".into())),
            Event::End(TagEnd::Link) => {
                if links.pop().unwrap_or(false) {
                    out.push(Event::Html("</a>".into()));
                }
            }
            Event::Start(Tag::Image { dest_url, .. }) => {
                let mut depth = 1;
                let mut j = i + 1;
                while j < events.len() {
                    match events[j] {
                        Event::Start(Tag::Image { .. }) => depth += 1,
                        Event::End(TagEnd::Image) => {
                            depth -= 1;
                            if depth == 0 {
                                break;
                            }
                        }
                        _ => {}
                    }
                    j += 1;
                }
                let alt = plain_text(&events[i + 1..j.min(events.len())]);
                let url: &str = dest_url;
                if r.first_image.is_none() && url_kind(url) == UrlKind::Local && !is_video_url(url) {
                    r.first_image = Some(url.to_string());
                }
                // Alone in its paragraph: a figure, its alt text the caption.
                let alone = matches!(out.last(), Some(Event::Start(Tag::Paragraph)))
                    && matches!(events.get(j + 1), Some(Event::End(TagEnd::Paragraph)));
                if alone && url_kind(url) == UrlKind::Local {
                    out.pop();
                    let caption = if alt.trim().is_empty() {
                        String::new()
                    } else {
                        format!("<figcaption>{}</figcaption>", esc(alt.trim()))
                    };
                    out.push(Event::Html(format!("<figure>{}{caption}</figure>\n", media_html(url, &alt)).into()));
                    i = j + 2;
                    continue;
                }
                out.push(Event::Html(media_html(url, &alt).into()));
                i = j + 1;
                continue;
            }
            _ => out.push(events[i].clone()),
        }
        i += 1;
    }
    pulldown_cmark::html::push_html(&mut r.html, out.into_iter());
    r
}

fn reading_minutes(words: usize) -> usize {
    words.div_ceil(230).max(1)
}

/// The summary a post shows: its own, else its first paragraph cut at a word near 200 characters.
fn summary_of(post: &PostRow, rendered: &Rendered) -> String {
    if !post.summary.trim().is_empty() {
        return post.summary.trim().to_string();
    }
    let p = rendered.first_paragraph.trim();
    if p.chars().count() <= 200 {
        return p.to_string();
    }
    let cut: String = p.chars().take(200).collect();
    let cut = cut.rsplit_once(' ').map_or(cut.as_str(), |(head, _)| head).trim_end_matches([',', ';', ':', '-']);
    format!("{cut}…")
}

// ---------------------------------------------------------------- the pages

/// The 429 answer once the client has asked for too many pages ([`common::ratelimit::BLOG_IP_WINDOWS`]).
fn page_limited(state: &AppState, headers: &HeaderMap, peer: SocketAddr) -> Option<Response> {
    let ip = ip_key(client_ip(state, headers, peer));
    limit(state, |l, now| l.blog_ip.check(&ip, now)).err().map(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
            .into_response()
    })
}

fn date_text(t: Option<DateTime<Utc>>) -> String {
    t.map(|t| t.format("%b %-d, %Y").to_string()).unwrap_or_default()
}

fn absolute(base: &str, url: &str) -> String {
    if url.starts_with('/') {
        format!("{base}{url}")
    } else {
        url.to_string()
    }
}

fn page_response(status: StatusCode, body: String, cache: &'static str) -> Response {
    let mut resp = (status, body).into_response();
    let h = resp.headers_mut();
    h.insert(header::CONTENT_TYPE, HeaderValue::from_static("text/html; charset=utf-8"));
    // No scripts at all; styles, fonts, images and videos from the site itself.
    h.insert(
        header::CONTENT_SECURITY_POLICY,
        HeaderValue::from_static(
            "default-src 'none'; style-src 'self'; font-src 'self'; img-src 'self'; media-src 'self'; \
             base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
        ),
    );
    h.insert(header::REFERRER_POLICY, HeaderValue::from_static("no-referrer"));
    h.insert(header::CACHE_CONTROL, HeaderValue::from_static(cache));
    resp
}

struct Head<'a> {
    title: &'a str,
    description: &'a str,
    url: &'a str,
    image: &'a str,
    og_type: &'a str,
    extra: String,
    noindex: bool,
    /// The index: the logo and "Blog" are the page's h1 (a post's h1 is its title).
    brand_h1: bool,
}

/// The page around the content: the top-left buttons (as on the home page, Blog lit), the small
/// logo with "Blog" beside it, the dim gameplay still behind everything.
fn shell(head: &Head, content: &str) -> String {
    let robots = if head.noindex { "<meta name=\"robots\" content=\"noindex\">\n" } else { "" };
    let (h1, h1_end) = if head.brand_h1 { ("<h1 class=\"brand-h1\">", "</h1>") } else { ("", "") };
    format!(
        "<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">
<meta name=\"color-scheme\" content=\"dark\">
<meta name=\"theme-color\" content=\"#08080b\">
{robots}<title>{title}</title>
<meta name=\"description\" content=\"{description}\">
<link rel=\"canonical\" href=\"{url}\">
<meta property=\"og:site_name\" content=\"Brawl Online\">
<meta property=\"og:type\" content=\"{og_type}\">
<meta property=\"og:title\" content=\"{title}\">
<meta property=\"og:description\" content=\"{description}\">
<meta property=\"og:url\" content=\"{url}\">
<meta property=\"og:image\" content=\"{image}\">
<meta name=\"twitter:card\" content=\"summary_large_image\">
{extra}<link rel=\"icon\" href=\"/favicon.ico\" sizes=\"16x16 32x32 48x48\">
<link rel=\"icon\" href=\"/assets/icon-512.png\" type=\"image/png\" sizes=\"512x512\">
<link rel=\"preload\" href=\"/assets/fonts/barlow-condensed-900-italic.woff2\" as=\"font\" type=\"font/woff2\" crossorigin>
<link rel=\"stylesheet\" href=\"/blog/blog.css\">
</head>
<body class=\"blog\">
<div class=\"backdrop\" aria-hidden=\"true\"></div>
<nav class=\"top-nav\">
  <a class=\"pill\" href=\"/#faq\">FAQ</a>
  <a class=\"pill\" href=\"/#credits\">Credits</a>
  <a class=\"pill on\" href=\"/blog\" aria-current=\"page\">Blog</a>
</nav>
<main class=\"wrap\">
  {h1}<a class=\"brand\" href=\"/blog\">
    <span class=\"logo plate\"><span class=\"wordmark\"><span class=\"w1\">Brawl</span> <span class=\"w2\">Online</span></span></span>
    <span class=\"section\">Blog</span>
  </a>{h1_end}
{content}
  <p class=\"foot\">Wii/GCN games with rollback netcode. <a href=\"/\">Download Brawl Online</a></p>
</main>
</body>
</html>
",
        title = esc(head.title),
        description = esc(head.description),
        url = esc(head.url),
        image = esc(head.image),
        og_type = head.og_type,
        extra = head.extra,
    )
}

/// The index: the latest post as a card with its cover image and a Read button, older ones as rows
/// below.
fn index_html(posts: &[PostRow], base: &str) -> String {
    let mut content = String::new();
    match posts.split_first() {
        None => content.push_str("  <section class=\"panel empty\"><p>No posts yet.</p></section>\n"),
        Some((latest, older)) => {
            let r = render(&latest.body_md);
            let cover = r.first_image.as_deref().unwrap_or(DEFAULT_COVER);
            content.push_str(&format!(
                "  <a class=\"hero\" href=\"/blog/{slug}\">
    <span class=\"cover\"><img src=\"{cover}\" alt=\"\"></span>
    <span class=\"hero-body\">
      <span class=\"meta\"><span class=\"kicker\">Latest</span><span class=\"date\">{date}</span></span>
      <span class=\"ptitle\">{title}</span>
      <span class=\"summary\">{summary}</span>
      <span class=\"read plate\"><span class=\"label\">Read</span></span>
    </span>
  </a>
",
                slug = esc(&latest.slug),
                cover = esc(cover),
                date = esc(&date_text(latest.published_at)),
                title = esc(&latest.title),
                summary = esc(&summary_of(latest, &r)),
            ));
            if !older.is_empty() {
                content.push_str(
                    "  <section class=\"older\">\n    <h2>Older posts</h2>\n    <div class=\"panel\">\n      <ul class=\"rows\">\n",
                );
                for p in older {
                    let r = render(&p.body_md);
                    content.push_str(&format!(
                        "        <li><a href=\"/blog/{slug}\"><span class=\"date\">{date}</span><span class=\"t\">{title}<span class=\"s\">{summary}</span></span></a></li>\n",
                        slug = esc(&p.slug),
                        date = esc(&date_text(p.published_at)),
                        title = esc(&p.title),
                        summary = esc(&summary_of(p, &r)),
                    ));
                }
                content.push_str("      </ul>\n    </div>\n  </section>\n");
            }
        }
    }
    let url = format!("{base}/blog");
    let image = absolute(base, DEFAULT_COVER);
    shell(
        &Head {
            title: "Blog · Brawl Online",
            description:
                "News and progress reports from Brawl Online: rollback netplay for Project+ and every Wii game.",
            url: &url,
            image: &image,
            og_type: "website",
            extra: blog_ld(posts, base),
            noindex: false,
            brand_h1: true,
        },
        &content,
    )
}

/// The index's JSON-LD: a `Blog` with its posts.
fn blog_ld(posts: &[PostRow], base: &str) -> String {
    let items: Vec<_> = posts
        .iter()
        .map(|p| {
            serde_json::json!({
                "@type": "BlogPosting",
                "headline": p.title,
                "url": format!("{base}/blog/{}", p.slug),
                "datePublished": p.published_at.map(|t| t.to_rfc3339()),
            })
        })
        .collect();
    let ld = serde_json::json!({
        "@context": "https://schema.org",
        "@type": "Blog",
        "name": "Brawl Online Blog",
        "url": format!("{base}/blog"),
        "publisher": {"@type": "Organization", "name": "Brawl Online", "url": format!("{base}/")},
        "blogPost": items,
    });
    format!("<script type=\"application/ld+json\">{}</script>\n", json_ld(&ld))
}

/// The blog's sitemap (robots.txt names it): the index and every published post, each with its
/// last change.
fn sitemap_xml(posts: &[PostRow], base: &str) -> String {
    let mut out = String::from(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n",
    );
    let mut url = |loc: String, lastmod: Option<DateTime<Utc>>| {
        out.push_str(&format!("  <url>\n    <loc>{}</loc>\n", esc(&loc)));
        if let Some(t) = lastmod {
            out.push_str(&format!("    <lastmod>{}</lastmod>\n", t.format("%Y-%m-%dT%H:%M:%SZ")));
        }
        out.push_str("  </url>\n");
    };
    url(format!("{base}/blog"), posts.iter().map(|p| p.updated_at).max());
    for p in posts {
        url(format!("{base}/blog/{}", p.slug), Some(p.updated_at));
    }
    out.push_str("</urlset>\n");
    out
}

async fn published(state: &AppState) -> sqlx::Result<Vec<PostRow>> {
    sqlx::query_as::<_, PostRow>(&format!(
        "SELECT {POST_COLUMNS} FROM blog_posts WHERE status = 'published' ORDER BY published_at DESC, id DESC"
    ))
    .fetch_all(&state.pool)
    .await
}

/// `GET /blog/sitemap.xml`.
pub async fn sitemap(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
) -> Response {
    if let Some(resp) = page_limited(&state, &headers, peer) {
        return resp;
    }
    match published(&state).await {
        Ok(posts) => (
            [
                (header::CONTENT_TYPE, HeaderValue::from_static("application/xml; charset=utf-8")),
                (header::CACHE_CONTROL, HeaderValue::from_static("public, max-age=600")),
            ],
            sitemap_xml(&posts, &base_url(&state)),
        )
            .into_response(),
        Err(e) => {
            tracing::error!("blog sitemap: {e}");
            StatusCode::INTERNAL_SERVER_ERROR.into_response()
        }
    }
}

fn post_html(post: &PostRow, base: &str, preview: bool) -> String {
    let r = render(&post.body_md);
    let summary = summary_of(post, &r);
    let url = format!("{base}/blog/{}", post.slug);
    let image = absolute(base, r.first_image.as_deref().unwrap_or(DEFAULT_COVER));
    let mut byline = vec![];
    if post.published_at.is_some() {
        byline.push(esc(&date_text(post.published_at)));
    }
    if !post.author.is_empty() {
        byline.push(format!("by {}", esc(&post.author)));
    }
    byline.push(format!("{} min read", reading_minutes(r.words)));
    let mut extra = String::new();
    if let Some(t) = post.published_at {
        extra.push_str(&format!("<meta property=\"article:published_time\" content=\"{}\">\n", t.to_rfc3339()));
        let mut ld = serde_json::json!({
            "@context": "https://schema.org",
            "@type": "BlogPosting",
            "headline": post.title,
            "description": summary,
            "url": url,
            "image": image,
            "datePublished": t.to_rfc3339(),
            "dateModified": post.updated_at.to_rfc3339(),
            "mainEntityOfPage": url,
            "publisher": {"@type": "Organization", "name": "Brawl Online", "url": format!("{base}/")},
        });
        if !post.author.is_empty() {
            ld["author"] = serde_json::json!({"@type": "Person", "name": post.author});
        }
        extra.push_str(&format!("<script type=\"application/ld+json\">{}</script>\n", json_ld(&ld)));
    }
    let banner = if preview {
        "  <p class=\"draft-banner\">Draft preview: only people with this link can see it.</p>\n"
    } else {
        ""
    };
    let content = format!(
        "{banner}  <article class=\"article\">
    <h1 class=\"ptitle\">{title}</h1>
    <p class=\"byline\">{byline}</p>
    <div class=\"prose\">
{html}    </div>
  </article>
  <a class=\"pill back\" href=\"/blog\">&larr; All posts</a>
",
        title = esc(&post.title),
        byline = byline.join(" · "),
        html = r.html,
    );
    let title = format!("{} · Brawl Online", post.title);
    shell(
        &Head {
            title: &title,
            description: &summary,
            url: &url,
            image: &image,
            og_type: "article",
            extra,
            noindex: preview,
            brand_h1: false,
        },
        &content,
    )
}

fn message_html(title: &str, text: &str, base: &str) -> String {
    let url = format!("{base}/blog");
    let image = absolute(base, DEFAULT_COVER);
    shell(
        &Head {
            title: &format!("{title} · Brawl Online"),
            description: text,
            url: &url,
            image: &image,
            og_type: "website",
            extra: String::new(),
            noindex: true,
            brand_h1: false,
        },
        &format!(
            "  <section class=\"panel empty\"><h1>{}</h1><p>{}</p></section>\n  <a class=\"pill back\" href=\"/blog\">&larr; All posts</a>\n",
            esc(title),
            esc(text)
        ),
    )
}

fn base_url(state: &AppState) -> String {
    state.cfg.public_base_url.trim_end_matches('/').to_string()
}

/// `GET /blog`.
pub async fn index_page(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
) -> Response {
    if let Some(resp) = page_limited(&state, &headers, peer) {
        return resp;
    }
    let base = base_url(&state);
    match published(&state).await {
        Ok(posts) => {
            let mut resp = page_response(StatusCode::OK, index_html(&posts, &base), "public, max-age=60");
            if let Some(cookie) = posts.first().and_then(|p| seen_cookie(p.published_at, &base)) {
                resp.headers_mut().insert(header::SET_COOKIE, cookie);
            }
            resp
        }
        Err(e) => {
            tracing::error!("blog index: {e}");
            page_response(
                StatusCode::INTERNAL_SERVER_ERROR,
                message_html("Something went wrong", "Try again in a minute.", &base),
                "no-store",
            )
        }
    }
}

/// The cookie the index leaves: when its newest post was published (Unix seconds). The home
/// page's Blog button shows its "new post" badge only for a post published after it
/// (`website/script.js`, `GET /v1/blog/latest`). The same for everyone, so a cached index setting
/// it is still right. Not HttpOnly: the home page reads it.
pub const SEEN_COOKIE: &str = "bo_blog_seen";

fn seen_cookie(published_at: Option<DateTime<Utc>>, base: &str) -> Option<HeaderValue> {
    let secs = published_at?.timestamp();
    let secure = if base.starts_with("https://") { "; Secure" } else { "" };
    // 400 days, the most browsers keep a cookie.
    HeaderValue::from_str(&format!("{SEEN_COOKIE}={secs}; Path=/; Max-Age=34560000; SameSite=Lax{secure}")).ok()
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LatestPost {
    pub slug: String,
    pub title: String,
    /// Unix seconds, as in the index's cookie.
    pub published_at: i64,
}

#[derive(Debug, Serialize)]
pub struct LatestResp {
    pub latest: Option<LatestPost>,
}

/// `GET /v1/blog/latest`: the newest published post, for the home page's "new post" badge.
pub async fn latest_post(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
) -> Response {
    if let Some(resp) = page_limited(&state, &headers, peer) {
        return resp;
    }
    let row = sqlx::query_as::<_, (String, String, Option<DateTime<Utc>>)>(
        "SELECT slug, title, published_at FROM blog_posts WHERE status = 'published' \
         ORDER BY published_at DESC, id DESC LIMIT 1",
    )
    .fetch_optional(&state.pool)
    .await;
    match row {
        Ok(row) => {
            let latest =
                row.and_then(|(slug, title, at)| Some(LatestPost { slug, title, published_at: at?.timestamp() }));
            ([(header::CACHE_CONTROL, HeaderValue::from_static("public, max-age=60"))], Json(LatestResp { latest }))
                .into_response()
        }
        Err(e) => {
            tracing::error!("blog latest: {e}");
            StatusCode::INTERNAL_SERVER_ERROR.into_response()
        }
    }
}

/// `GET /blog/` -> `/blog`.
pub async fn index_redirect() -> Redirect {
    Redirect::permanent("/blog")
}

/// `GET /login` -> `/login/` (nginx does this itself in production).
pub async fn login_redirect(uri: axum::http::Uri) -> Redirect {
    match uri.query() {
        Some(q) => Redirect::permanent(&format!("/login/?{q}")),
        None => Redirect::permanent("/login/"),
    }
}

#[derive(Debug, Deserialize)]
pub struct PageQuery {
    pub preview: Option<String>,
}

/// `GET /blog/<slug>`: a published post, or a draft with its preview link.
pub async fn post_page(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Path(slug): Path<String>,
    Query(q): Query<PageQuery>,
) -> Response {
    if slug == "write" {
        return Redirect::permanent("/blog/write/").into_response();
    }
    if let Some(resp) = page_limited(&state, &headers, peer) {
        return resp;
    }
    let base = base_url(&state);
    let not_found = || {
        page_response(
            StatusCode::NOT_FOUND,
            message_html("No such post", "It may have moved or been taken down.", &base),
            "no-store",
        )
    };
    if check_slug(&slug).is_err() {
        return not_found();
    }
    let post = sqlx::query_as::<_, PostRow>(&format!("SELECT {POST_COLUMNS} FROM blog_posts WHERE slug = $1"))
        .bind(&slug)
        .fetch_optional(&state.pool)
        .await;
    match post {
        Ok(Some(p)) if p.status == "published" => {
            page_response(StatusCode::OK, post_html(&p, &base, false), "public, max-age=60")
        }
        Ok(Some(p)) if q.preview.as_deref() == Some(p.preview_token.as_str()) => {
            page_response(StatusCode::OK, post_html(&p, &base, true), "no-store")
        }
        Ok(_) => not_found(),
        Err(e) => {
            tracing::error!("blog post {slug}: {e}");
            page_response(
                StatusCode::INTERNAL_SERVER_ERROR,
                message_html("Something went wrong", "Try again in a minute.", &base),
                "no-store",
            )
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn slugs() {
        assert!(check_slug("progress-report-1").is_ok());
        assert!(check_slug("").is_err());
        assert!(check_slug("Progress").is_err());
        assert!(check_slug("-a").is_err());
        assert!(check_slug("a-").is_err());
        assert!(check_slug("a--b").is_err());
        assert!(check_slug("write").is_err());
        assert!(check_slug("media").is_err());
        assert!(check_slug("blog.css").is_err());
        assert!(check_slug(&"a".repeat(81)).is_err());
        assert_eq!(
            slugify("Brawl Online Progress Report #1: Celebrating 100,000+ Matches of Project+!"),
            "brawl-online-progress-report-1-celebrating-100-000-matches-of-project"
        );
        assert_eq!(slugify("  ?? "), "post");
        assert_eq!(slugify("Write"), "post");
        assert!(slugify(&"word ".repeat(40)).len() <= MAX_SLUG);
        assert!(check_slug(&slugify(&"word ".repeat(40))).is_ok());
    }

    fn mp4_box(typ: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let mut b = ((body.len() + 8) as u32).to_be_bytes().to_vec();
        b.extend_from_slice(typ);
        b.extend_from_slice(body);
        b
    }

    /// The moov of the upload Firefox froze on (ffmpeg 8: Lavc63.1.101 libx264, 59.94 fps + 48 kHz).
    fn ffmpeg8_moov(elst_entries: u32) -> Vec<u8> {
        let mut mvhd = vec![0u8; 12];
        mvhd.extend(47_952_000u32.to_be_bytes());
        mvhd.extend(869_600_000u32.to_be_bytes());
        mvhd.extend([0u8; 80]);
        let mut tkhd = vec![0u8, 0, 0, 3];
        tkhd.extend([0u8; 16]);
        tkhd.extend(869_600_000u32.to_be_bytes());
        tkhd.extend([0u8; 60]);
        let mut elst = vec![0u8; 4];
        elst.extend(elst_entries.to_be_bytes());
        for (dur, start) in [(800_000u32, -1i32), (868_800_000, 400)] {
            elst.extend(dur.to_be_bytes());
            elst.extend(start.to_be_bytes());
            elst.extend([0, 1, 0, 0]);
        }
        let trak =
            [mp4_box(b"tkhd", &tkhd), mp4_box(b"edts", &mp4_box(b"elst", &elst)), mp4_box(b"mdia", &[0; 12])].concat();
        [mp4_box(b"mvhd", &mvhd), mp4_box(b"trak", &trak)].concat()
    }

    #[test]
    fn movie_timescale() {
        let be32 = |b: &[u8], at: usize| u32::from_be_bytes(b[at..at + 4].try_into().unwrap());
        let mut moov = ffmpeg8_moov(2);
        assert!(normalize_movie_timescale(&mut moov));
        // mvhd: timescale and duration.
        assert_eq!((be32(&moov, 8 + 12), be32(&moov, 8 + 16)), (1000, 18135));
        let tkhd = 108 + 8; // after mvhd and trak's header
        assert_eq!(be32(&moov, tkhd + 8 + 20), 18135, "tkhd duration");
        // Each edit's duration in milliseconds; where it starts in the track is the track's own time.
        let elst = tkhd + 92 + 8 + 8 + 8; // after tkhd, edts and elst's headers, version and count
        assert_eq!((be32(&moov, elst), be32(&moov, elst + 4) as i32), (17, -1));
        assert_eq!((be32(&moov, elst + 12), be32(&moov, elst + 16)), (18118, 400));
        // Already 1000: nothing to do.
        let again = moov.clone();
        assert!(!normalize_movie_timescale(&mut moov));
        assert_eq!(moov, again);
        // An edit list longer than its box: nothing changes at all.
        let mut bad = ffmpeg8_moov(9);
        let before = bad.clone();
        assert!(!normalize_movie_timescale(&mut bad));
        assert_eq!(bad, before);
        assert!(!normalize_movie_timescale(&mut []));
    }

    #[test]
    fn sniffing() {
        assert_eq!(sniff_media(b"\x89PNG\r\n\x1a\n...."), Some((MediaKind::Image, "png")));
        assert_eq!(sniff_media(b"\xff\xd8\xff\xe0"), Some((MediaKind::Image, "jpg")));
        assert_eq!(sniff_media(b"GIF89a.."), Some((MediaKind::Image, "gif")));
        assert_eq!(sniff_media(b"RIFF\0\0\0\0WEBPVP8 "), Some((MediaKind::Image, "webp")));
        assert_eq!(sniff_media(b"\0\0\0\x20ftypisom\0\0"), Some((MediaKind::Video, "mp4")));
        assert_eq!(sniff_media(b"\0\0\0\x20ftypqt  \0\0"), Some((MediaKind::Video, "mov")));
        assert_eq!(sniff_media(b"\0\0\0\x20ftypavif\0\0"), Some((MediaKind::Image, "avif")));
        assert_eq!(sniff_media(b"\x1a\x45\xdf\xa3\x01"), Some((MediaKind::Video, "webm")));
        assert_eq!(sniff_media(b"<svg xmlns="), None);
        assert_eq!(sniff_media(b"<html>"), None);
        assert_eq!(sniff_media(b""), None);
        assert!(valid_media_name("0123456789abcdef0123.webp"));
        assert!(!valid_media_name("0123456789abcdef012.webp"));
        assert!(!valid_media_name("0123456789abcdef0123.svg"));
        assert!(!valid_media_name("0123456789ABCDEF0123.png"));
        assert!(!valid_media_name("../../etc/passwd.png"));
    }

    #[test]
    fn urls() {
        assert_eq!(url_kind("/blog/media/x.png"), UrlKind::Local);
        assert_eq!(url_kind("#section"), UrlKind::Local);
        assert_eq!(url_kind("other-post"), UrlKind::Local);
        assert_eq!(url_kind("https://discord.gg/x"), UrlKind::External);
        assert_eq!(url_kind("HTTP://x"), UrlKind::External);
        assert_eq!(url_kind("//evil.example/x.png"), UrlKind::External);
        assert_eq!(url_kind("mailto:a@b.c"), UrlKind::External);
        assert_eq!(url_kind("javascript:alert(1)"), UrlKind::Bad);
        assert_eq!(url_kind(" JavaScript:alert(1)"), UrlKind::Bad);
        assert_eq!(url_kind("data:text/html,x"), UrlKind::Bad);
        assert_eq!(url_kind("/path?a=b:c"), UrlKind::Local);
    }

    #[test]
    fn markdown_basics() {
        let r = render("# Title\n\nHello **world**, see [Discord](https://discord.gg/x).\n\n## New Ways\n\n### Sub\n\n## New Ways\n\n- a\n- b\n");
        assert!(r.html.contains("<strong>world</strong>"), "{}", r.html);
        assert!(r.html.contains("<a href=\"https://discord.gg/x\" rel=\"noopener\">Discord</a>"), "{}", r.html);
        assert!(r.html.contains("<h2 id=\"new-ways\">New Ways</h2>"), "{}", r.html);
        assert!(r.html.contains("<h2 id=\"new-ways-2\">"), "{}", r.html);
        assert!(r.html.contains("<h3 id=\"sub\">"), "{}", r.html);
        // `# Title` in the text: an h2, the page's only h1 is the post's title.
        assert!(r.html.contains("<h2 id=\"title\">Title</h2>"), "{}", r.html);
        assert!(!r.html.contains("<h1"), "{}", r.html);
        assert!(r.html.contains("<li>a</li>"), "{}", r.html);
        assert_eq!(r.first_paragraph, "Hello world, see Discord.");
        // A list right under a line, as in the progress report (CommonMark allows it).
        let r = render("Queue:\n- **Solo:** jump in\n- **Duo:** team up\n");
        assert!(r.html.contains("<ul>"), "{}", r.html);
    }

    #[test]
    fn markdown_html_is_text() {
        let r = render("Hi <script>alert(1)</script> there<br>\n\n<div onclick=\"x()\">block</div>\n\n<!-- a note\nfor me -->\n\nend");
        assert!(!r.html.contains("<script"), "{}", r.html);
        assert!(r.html.contains("&lt;script&gt;"), "{}", r.html);
        assert!(!r.html.contains("<div"), "{}", r.html);
        assert!(r.html.contains("&lt;div onclick"), "{}", r.html);
        assert!(r.html.contains("<br>"), "{}", r.html);
        assert!(!r.html.contains("a note"), "{}", r.html);
        assert!(r.html.contains("<p>end</p>"), "{}", r.html);
        let r = render("[x](javascript:alert(1)) ![y](javascript:alert(2)) [z](\"onmouseover=\"a)");
        assert!(!r.html.contains("javascript:"), "{}", r.html);
        assert!(!r.html.contains("href=\"\"onmouseover"), "{}", r.html);
    }

    #[test]
    fn markdown_media() {
        let r = render("Intro.\n\n![The waiting room](/blog/media/0123456789abcdef0123.webp)\n\n![](/blog/media/0123456789abcdef0124.mp4)\n\nInline ![i](/blog/media/0123456789abcdef0125.png) here.\n\n![ext](https://imgur.com/a.png)\n");
        assert!(
            r.html.contains("<figure><img src=\"/blog/media/0123456789abcdef0123.webp\" alt=\"The waiting room\" loading=\"lazy\"><figcaption>The waiting room</figcaption></figure>"),
            "{}",
            r.html
        );
        assert!(
            r.html.contains("<figure><video src=\"/blog/media/0123456789abcdef0124.mp4\" controls preload=\"metadata\" playsinline></video></figure>"),
            "{}",
            r.html
        );
        assert!(
            r.html.contains(
                "<p>Inline <img src=\"/blog/media/0123456789abcdef0125.png\" alt=\"i\" loading=\"lazy\"> here.</p>"
            ),
            "{}",
            r.html
        );
        assert!(r.html.contains("<a href=\"https://imgur.com/a.png\" rel=\"noopener\">ext</a>"), "{}", r.html);
        assert!(!r.html.contains("<p><figure>"), "{}", r.html);
        assert_eq!(r.first_image.as_deref(), Some("/blog/media/0123456789abcdef0123.webp"));
    }

    #[test]
    fn summaries() {
        let post = |summary: &str| PostRow {
            id: 1,
            slug: "a".into(),
            title: "A".into(),
            summary: summary.into(),
            author: String::new(),
            body_md: String::new(),
            status: "published".into(),
            published_at: None,
            preview_token: "t".into(),
            created_at: Utc::now(),
            updated_at: Utc::now(),
        };
        let long = render(&"word ".repeat(100));
        let s = summary_of(&post(""), &long);
        assert!(s.ends_with('…') && s.chars().count() <= 201, "{s}");
        assert_eq!(summary_of(&post(" Mine "), &long), "Mine");
        assert_eq!(reading_minutes(0), 1);
        assert_eq!(reading_minutes(2300), 10);
    }

    /// The top-level elements of rendered HTML (a crude tag walk, enough for the renderer's output).
    fn top_level_elements(html: &str) -> usize {
        let (mut depth, mut n) = (0usize, 0usize);
        let mut rest = html;
        while let Some(at) = rest.find('<') {
            rest = &rest[at + 1..];
            let end = rest.find('>').unwrap();
            let tag = &rest[..end];
            rest = &rest[end + 1..];
            if tag.starts_with('/') {
                depth -= 1;
                continue;
            }
            let name: String = tag.chars().take_while(|c| c.is_ascii_alphanumeric()).collect();
            if depth == 0 {
                n += 1;
            }
            if !matches!(name.as_str(), "img" | "br" | "hr" | "input") && !tag.ends_with('/') {
                depth += 1;
            }
        }
        n
    }

    #[test]
    fn block_lines_follow_the_elements() {
        let md = "# Title\n\nPara one\ncontinues\n\n<!-- Uploading x (1)… -->\n\n![cap](/blog/media/0123456789abcdef0123.webp)\n\n- a\n- b\n\n---\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\nFootnote[^1].\n\n[^1]: The note.\n\n> quote\n\n```\ncode\n```\n<b>raw</b>\n";
        let r = render(md);
        assert_eq!(r.blocks, vec![0, 2, 7, 9, 12, 14, 18, 20, 22, 24, 27], "{}", r.html);
        assert_eq!(top_level_elements(&r.html), r.blocks.len(), "{}", r.html);
        assert_eq!(render("").blocks, Vec::<usize>::new());
        // CRLF counts as one line each.
        assert_eq!(render("a\r\n\r\nb\r\n").blocks, vec![0, 2]);
    }

    #[test]
    fn pages_escape() {
        let mut p = PostRow {
            id: 1,
            slug: "hello".into(),
            title: "<b>Hi</b> & \"you\"".into(),
            summary: String::new(),
            author: "Me <i>".into(),
            body_md: "Text.".into(),
            status: "published".into(),
            published_at: Some(Utc::now()),
            preview_token: "t".into(),
            created_at: Utc::now(),
            updated_at: Utc::now(),
        };
        let html = post_html(&p, "https://brawlonline.net", false);
        assert!(!html.contains("<b>Hi"), "{html}");
        assert!(html.contains("&lt;b&gt;Hi&lt;/b&gt; &amp; &quot;you&quot;"));
        assert!(html.contains("by Me &lt;i&gt;"));
        assert!(html.contains("og:image\" content=\"https://brawlonline.net/assets/bg-poster.jpg"));
        assert!(!html.contains("noindex"));
        p.status = "draft".into();
        assert!(post_html(&p, "https://brawlonline.net", true).contains("noindex"));
        let index = index_html(&[p.clone(), p.clone()], "https://brawlonline.net");
        assert!(index.contains("class=\"hero\""));
        assert!(index.contains("Older posts"));
        assert!(!index_html(&[p], "https://brawlonline.net").contains("Older posts"));
        assert!(index_html(&[], "https://brawlonline.net").contains("No posts yet."));
    }
}
