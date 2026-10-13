//! Rulesets (`docs/design/room-rules.md` §4, API: `docs/rulesets-protocol.md`): publishing,
//! looking one up by code, the public list with its search, the account's drafts and published
//! ones, the remembered ruleset and the recent list.
//!
//! A published version never changes and is never deleted (the database refuses it too,
//! `migrations/0008_rulesets.sql`); editing one publishes the next version with a new code. Every
//! published ruleset is public (the user's decision, 2026-10-12): the public list shows each
//! ruleset's newest version that is not hidden. The owner can hide one, which takes it off the
//! public list and their own; its code and link keep working.

use std::net::SocketAddr;

use axum::extract::{ConnectInfo, Path, Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::Html;
use axum::Json;
use chrono::{DateTime, Utc};
use common::db::{self, PublishedRuleset};
use common::net::ip_key;
use common::rules::{parse_ruleset_code, random_ruleset_code, Ruleset};
use serde::{Deserialize, Serialize};
use uuid::Uuid;

use crate::api::{client_ip, current_user, limit};
use crate::cursor;
use crate::error::{ApiError, ApiResult};
use crate::store::UserRow;
use crate::AppState;

/// Drafts an account can keep.
pub const MAX_DRAFTS: i64 = 50;
/// Tries for a free random code before giving up (64 M codes: a clash is rare).
const CODE_TRIES: usize = 10;

// ---------------------------------------------------------------- views

/// A newer version of a ruleset: the newest one that is not hidden.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct NewerVersion {
    pub code: String,
    pub version: u32,
}

/// A published ruleset as the API shows it.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct RulesetView {
    pub code: String,
    /// With `version` and `by` (the publisher's display name) filled.
    pub ruleset: Ruleset,
    /// SHA-256 of the canonical JSON (`Ruleset::hash`).
    pub hash: String,
    pub created_at: DateTime<Utc>,
    /// The newest later version that is not hidden, if any.
    pub newer: Option<NewerVersion>,
    /// `<PUBLIC_BASE_URL>/r/<code>`: the page to share.
    pub link: String,
    /// Only in the owner's own lists.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub hidden: Option<bool>,
}

async fn view(state: &AppState, p: PublishedRuleset, own: bool) -> ApiResult<RulesetView> {
    let newer = db::newer_ruleset_version(&state.pool, p.lineage, p.ruleset.version)
        .await?
        .map(|(code, version)| NewerVersion { code, version: version as u32 });
    Ok(RulesetView {
        link: format!("{}/r/{}", state.cfg.public_base_url.trim_end_matches('/'), p.code),
        code: p.code,
        ruleset: p.ruleset,
        hash: p.hash,
        created_at: p.created_at,
        newer,
        hidden: own.then_some(p.hidden),
    })
}

/// A draft: the owner's unpublished ruleset. It may not validate yet (`problem` says why).
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct DraftView {
    pub id: Uuid,
    /// The published version it edits (publishing makes that ruleset's next version if it is the
    /// owner's).
    pub based_on: Option<String>,
    pub ruleset: Ruleset,
    /// Why it can't be published as it is (`Ruleset::validate` after `normalize`), or null.
    pub problem: Option<String>,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
}

#[derive(sqlx::FromRow)]
struct DraftRow {
    id: Uuid,
    based_on: Option<String>,
    json: serde_json::Value,
    created_at: DateTime<Utc>,
    updated_at: DateTime<Utc>,
}

impl DraftRow {
    fn view(self) -> ApiResult<DraftView> {
        let ruleset: Ruleset = serde_json::from_value(self.json).map_err(ApiError::internal)?;
        Ok(DraftView {
            id: self.id,
            based_on: self.based_on,
            problem: problem(&ruleset),
            ruleset,
            created_at: self.created_at,
            updated_at: self.updated_at,
        })
    }
}

/// Why `r` can't be published, or `None`.
fn problem(r: &Ruleset) -> Option<String> {
    let mut n = r.clone();
    n.normalize();
    n.validate().err()
}

// ---------------------------------------------------------------- helpers

fn edit_limit(state: &AppState, uid: Uuid) -> ApiResult<()> {
    limit(state, |l, now| l.ruleset_edit.check(&uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })
}

/// Logged in, with the edit limit counted.
async fn editor(state: &AppState, headers: &HeaderMap) -> ApiResult<UserRow> {
    let user = current_user(state, headers).await?;
    edit_limit(state, user.uid)?;
    Ok(user)
}

/// A ruleset code from a path or body, or 404 (a malformed code names no ruleset).
fn code_or_404(input: &str) -> ApiResult<String> {
    parse_ruleset_code(input).ok_or_else(unknown_ruleset)
}

fn unknown_ruleset() -> ApiError {
    ApiError::new(StatusCode::NOT_FOUND, "unknown_ruleset", "No ruleset has this code")
}

fn invalid(msg: String) -> ApiError {
    ApiError::bad_request("invalid_ruleset", msg)
}

fn is_unique_violation(e: &sqlx::Error, constraint: &str) -> bool {
    matches!(e, sqlx::Error::Database(d) if d.code().as_deref() == Some("23505")
        && d.constraint().is_some_and(|c| c.contains(constraint)))
}

// ---------------------------------------------------------------- publish

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PublishReq {
    /// Publish this draft (it is deleted once published)...
    pub draft_id: Option<Uuid>,
    /// ...or this ruleset (an imported file, or an edit not saved as a draft).
    pub ruleset: Option<Ruleset>,
    /// With `ruleset`: the published version it edits.
    pub based_on: Option<String>,
}

/// `POST /v1/rulesets`: publishes a draft or a ruleset as a new, immutable version with a new
/// code. Based on one of the account's own rulesets it is that ruleset's next version (409
/// `unchanged` if it plays the same as the newest one); otherwise version 1 of a new ruleset.
/// Needs an account that can play online (verified email when required, a connect code); limited
/// per account ([`common::ratelimit::RULESET_PUBLISH_WINDOWS`]). 201 with the [`RulesetView`].
pub async fn publish(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<PublishReq>,
) -> ApiResult<(StatusCode, Json<RulesetView>)> {
    let user = current_user(&state, &headers).await?;
    if state.cfg.require_email_verification && !user.email_verified() {
        return Err(ApiError::forbidden("email_not_verified", "Verify your email before publishing rulesets"));
    }
    if user.connect_code.is_none() {
        return Err(ApiError::forbidden("not_activated", "Pick a connect code before publishing rulesets"));
    }

    let (mut ruleset, based_on, draft) = match (req.draft_id, req.ruleset) {
        (Some(id), None) => {
            if req.based_on.is_some() {
                return Err(ApiError::bad_request("bad_request", "A draft keeps its own basedOn"));
            }
            let row: DraftRow = sqlx::query_as(
                "SELECT id, based_on, json, created_at, updated_at FROM ruleset_drafts WHERE id = $1 AND owner = $2",
            )
            .bind(id)
            .bind(user.uid)
            .fetch_optional(&state.pool)
            .await?
            .ok_or_else(ApiError::not_found)?;
            let d = row.view()?;
            (d.ruleset, d.based_on, Some(id))
        }
        (None, Some(r)) => {
            let based_on = match req.based_on.as_deref() {
                Some(c) => Some(parse_ruleset_code(c).ok_or_else(unknown_ruleset)?),
                None => None,
            };
            (r, based_on, None)
        }
        _ => return Err(ApiError::bad_request("bad_request", "Send either draftId or ruleset")),
    };
    ruleset.normalize();
    ruleset.validate().map_err(invalid)?;

    // The ruleset it edits: the next version of it when it is the account's own.
    let base = match &based_on {
        Some(code) => Some(db::fetch_ruleset(&state.pool, code).await?.ok_or_else(unknown_ruleset)?),
        None => None,
    };
    let lineage = match &base {
        Some(b) if b.owner == Some(user.uid) => b.lineage,
        _ => Uuid::new_v4(),
    };
    let latest: Option<(String, i32, String)> =
        sqlx::query_as("SELECT code, version, json FROM rulesets WHERE lineage = $1 ORDER BY version DESC LIMIT 1")
            .bind(lineage)
            .fetch_optional(&state.pool)
            .await?;
    if let Some((code, version, json)) = &latest {
        let newest: Ruleset = serde_json::from_str(json).map_err(ApiError::internal)?;
        if newest.same_content(&ruleset) {
            return Err(ApiError::conflict("unchanged", format!("Nothing changed since version {version} ({code})")));
        }
    }

    limit(&state, |l, now| l.ruleset_publish.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait).with_message(format!(
            "You published a lot of rulesets lately. Try again in {} min.",
            wait.as_secs().div_ceil(60).max(1)
        ))
    })?;

    ruleset.version = latest.map_or(1, |(_, v, _)| v as u32 + 1);
    ruleset.by = String::new();
    let json = ruleset.canonical_json();
    let hash = ruleset.hash();
    let mut code = None;
    for _ in 0..CODE_TRIES {
        let candidate = random_ruleset_code(&mut rand::thread_rng());
        let mut tx = state.pool.begin().await?;
        let inserted = sqlx::query(
            "INSERT INTO rulesets (code, owner, lineage, version, name, publisher_name, json, hash)
             VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
        )
        .bind(&candidate)
        .bind(user.uid)
        .bind(lineage)
        .bind(ruleset.version as i32)
        .bind(&ruleset.name)
        .bind(&user.display_name)
        .bind(&json)
        .bind(&hash)
        .execute(&mut *tx)
        .await;
        match inserted {
            Ok(_) => {}
            Err(e) if is_unique_violation(&e, "rulesets_pkey") => continue,
            Err(e) if is_unique_violation(&e, "lineage_version") => {
                return Err(ApiError::conflict("conflict", "Another version was just published. Try again."));
            }
            Err(e) => return Err(e.into()),
        }
        if let Some(id) = draft {
            sqlx::query("DELETE FROM ruleset_drafts WHERE id = $1 AND owner = $2")
                .bind(id)
                .bind(user.uid)
                .execute(&mut *tx)
                .await?;
        }
        tx.commit().await?;
        code = Some(candidate);
        break;
    }
    let code = code.ok_or_else(|| ApiError::internal("no free ruleset code"))?;
    tracing::info!(uid = %user.uid, %code, version = ruleset.version, "ruleset published");
    let p = db::fetch_ruleset(&state.pool, &code).await?.ok_or_else(ApiError::not_found)?;
    Ok((StatusCode::CREATED, Json(view(&state, p, true).await?)))
}

// ---------------------------------------------------------------- lookups

/// `GET /v1/rulesets/<code>`: a published ruleset, for anyone (the website's `/r/<code>` page, the
/// launcher's code box). Hidden ones too (hiding only takes them off the public list). Limited per
/// client IP ([`common::ratelimit::RULESET_LOOKUP_IP_WINDOWS`]).
pub async fn lookup(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Path(code): Path<String>,
) -> ApiResult<Json<RulesetView>> {
    let ip = ip_key(client_ip(&state, &headers, peer));
    limit(&state, |l, now| l.ruleset_lookup_ip.check(&ip, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let code = code_or_404(&code)?;
    let p = db::fetch_ruleset(&state.pool, &code).await?.ok_or_else(unknown_ruleset)?;
    Ok(Json(view(&state, p, false).await?))
}

/// `GET /v1/rulesets/<code>/preview`: the ruleset page as link previews (Discord, Slack, ...) see
/// it. They don't run the page's script, so the static `/r/<code>` page can't name the ruleset;
/// nginx sends them here instead (`deploy/sarahvps2/nginx-brawl-online.conf`, `$brawl_unfurler`).
/// The page's title is "<name> v<version> by <publisher>" and there is no description, so the
/// embed shows the name alone (the user's, 2026-10-12). Limited as the lookup is, together with it.
pub async fn preview(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Path(code): Path<String>,
) -> ApiResult<Html<String>> {
    let ip = ip_key(client_ip(&state, &headers, peer));
    limit(&state, |l, now| l.ruleset_lookup_ip.check(&ip, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let code = code_or_404(&code)?;
    let p = db::fetch_ruleset(&state.pool, &code).await?.ok_or_else(unknown_ruleset)?;
    Ok(Html(preview_page(&p.ruleset)))
}

fn preview_page(r: &Ruleset) -> String {
    let title = crate::pages::esc(&format!("{} v{} by {}", r.name, r.version, r.by));
    // theme-color is the embed's side bar, as on the static page.
    format!(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">         <meta name=\"theme-color\" content=\"#08080b\"><title>{title}</title>         <meta property=\"og:title\" content=\"{title}\"></head><body><h1>{title}</h1></body></html>"
    )
}

/// Public list page sizes.
pub const BROWSE_DEFAULT_LIMIT: u32 = 30;
pub const BROWSE_MAX_LIMIT: u32 = 100;
/// Longest search text taken (longer is cut).
pub const MAX_QUERY_CHARS: usize = 64;

#[derive(Debug, Deserialize)]
pub struct BrowseQuery {
    pub q: Option<String>,
    pub limit: Option<u32>,
    pub after: Option<String>,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BrowsePage {
    /// Newest first.
    pub rulesets: Vec<RulesetView>,
    /// Pass as `after` for the next page; null on the last.
    pub next: Option<String>,
}

fn browse_cursor(p: &PublishedRuleset) -> String {
    cursor::encode("r1", &[&p.created_at.timestamp_micros().to_string(), &p.code])
}

fn parse_browse_cursor(c: &str) -> Option<(DateTime<Utc>, String)> {
    let f = cursor::decode("r1", c, 2)?;
    let at = DateTime::from_timestamp_micros(f[0].parse().ok()?)?;
    Some((at, parse_ruleset_code(&f[1])?))
}

/// `GET /v1/rulesets?q=&limit=&after=`: the public list (the user's decision, 2026-10-12: every
/// published ruleset is public). Of each ruleset its newest version that is not hidden, newest
/// first, in pages (`next`). `q` matches the name, the publisher's name and the code,
/// case-insensitively. For anyone; limited per client IP together with the code lookup
/// ([`common::ratelimit::RULESET_LOOKUP_IP_WINDOWS`]). 400 `bad_cursor` for a cursor it did not
/// give out.
pub async fn browse(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Query(q): Query<BrowseQuery>,
) -> ApiResult<Json<BrowsePage>> {
    let ip = ip_key(client_ip(&state, &headers, peer));
    limit(&state, |l, now| l.ruleset_lookup_ip.check(&ip, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let n = cursor::page_limit(q.limit, BROWSE_DEFAULT_LIMIT, BROWSE_MAX_LIMIT);
    let after = match q.after.as_deref() {
        Some(c) => Some(parse_browse_cursor(c).ok_or_else(|| ApiError::bad_request("bad_cursor", "Bad cursor"))?),
        None => None,
    };
    let text: Option<String> =
        q.q.as_deref().map(|t| t.trim().chars().take(MAX_QUERY_CHARS).collect::<String>()).filter(|t| !t.is_empty());
    let mut rows = db::browse_rulesets(
        &state.pool,
        text.as_deref(),
        after.as_ref().map(|(at, code)| (*at, code.as_str())),
        n as i64 + 1,
    )
    .await?;
    let next = if rows.len() > n as usize {
        rows.truncate(n as usize);
        rows.last().map(browse_cursor)
    } else {
        None
    };
    let base = state.cfg.public_base_url.trim_end_matches('/').to_string();
    // Each is its ruleset's newest visible version: no newer one to link.
    let rulesets = rows
        .into_iter()
        .map(|p| RulesetView {
            link: format!("{base}/r/{}", p.code),
            code: p.code,
            ruleset: p.ruleset,
            hash: p.hash,
            created_at: p.created_at,
            newer: None,
            hidden: None,
        })
        .collect();
    Ok(Json(BrowsePage { rulesets, next }))
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MyRulesets {
    /// Newest edit first.
    pub drafts: Vec<DraftView>,
    /// Newest first, hidden ones included (`hidden: true`).
    pub published: Vec<RulesetView>,
}

/// `GET /v1/me/rulesets`: the account's drafts and published rulesets.
pub async fn mine(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<MyRulesets>> {
    let user = editor(&state, &headers).await?;
    let rows: Vec<DraftRow> = sqlx::query_as(
        "SELECT id, based_on, json, created_at, updated_at FROM ruleset_drafts WHERE owner = $1
          ORDER BY updated_at DESC",
    )
    .bind(user.uid)
    .fetch_all(&state.pool)
    .await?;
    let drafts = rows.into_iter().map(DraftRow::view).collect::<ApiResult<Vec<_>>>()?;
    let mut published = Vec::new();
    for p in db::fetch_rulesets_where(&state.pool, "WHERE r.owner = $1 ORDER BY r.created_at DESC", user.uid).await? {
        published.push(view(&state, p, true).await?);
    }
    Ok(Json(MyRulesets { drafts, published }))
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecentRuleset {
    #[serde(flatten)]
    pub view: RulesetView,
    pub used_at: DateTime<Utc>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct RecentRulesets {
    pub rulesets: Vec<RecentRuleset>,
}

/// `GET /v1/me/rulesets/recent`: the rulesets the account made its remembered one, newest first
/// (at most [`common::db::RECENT_RULESETS_KEPT`]).
pub async fn recent(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<RecentRulesets>> {
    let user = editor(&state, &headers).await?;
    let mut rulesets = Vec::new();
    for (p, used_at) in db::recent_rulesets(&state.pool, user.uid).await? {
        let own = p.owner == Some(user.uid);
        rulesets.push(RecentRuleset { view: view(&state, p, own).await?, used_at });
    }
    Ok(Json(RecentRulesets { rulesets }))
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HiddenReq {
    pub hidden: bool,
}

/// `PUT /v1/me/rulesets/<code>/hidden` `{hidden}`: takes one of the account's rulesets off the
/// public list (and "newer version" links), or puts it back. The owner's own list still shows it.
/// 204; 404 unless it is the account's.
pub async fn set_hidden(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(code): Path<String>,
    Json(req): Json<HiddenReq>,
) -> ApiResult<StatusCode> {
    let user = editor(&state, &headers).await?;
    let code = code_or_404(&code)?;
    let n = sqlx::query("UPDATE rulesets SET hidden = $1 WHERE code = $2 AND owner = $3")
        .bind(req.hidden)
        .bind(&code)
        .bind(user.uid)
        .execute(&state.pool)
        .await?
        .rows_affected();
    if n == 0 {
        return Err(unknown_ruleset());
    }
    Ok(StatusCode::NO_CONTENT)
}

// ---------------------------------------------------------------- drafts

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DraftReq {
    pub ruleset: Ruleset,
    /// The published version this draft edits.
    #[serde(default)]
    pub based_on: Option<String>,
}

async fn draft_based_on(state: &AppState, based_on: Option<&str>) -> ApiResult<Option<String>> {
    match based_on {
        None => Ok(None),
        Some(c) => {
            let code = code_or_404(c)?;
            db::fetch_ruleset(&state.pool, &code).await?.ok_or_else(unknown_ruleset)?;
            Ok(Some(code))
        }
    }
}

/// The draft as stored: its values as sent (it may not validate yet), the version and publisher
/// cleared.
fn draft_json(mut r: Ruleset) -> ApiResult<(String, serde_json::Value)> {
    r.version = 0;
    r.by = String::new();
    // The name is a column for lists; any text up to a sane size.
    if r.name.chars().count() > 200 {
        return Err(invalid("The name is too long".into()));
    }
    let name = r.name.clone();
    Ok((name, serde_json::to_value(&r).map_err(ApiError::internal)?))
}

/// `POST /v1/me/ruleset-drafts` `{ruleset, basedOn?}`: saves a new draft. 201 with the
/// [`DraftView`]; 409 `too_many_drafts` at [`MAX_DRAFTS`].
pub async fn create_draft(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<DraftReq>,
) -> ApiResult<(StatusCode, Json<DraftView>)> {
    let user = editor(&state, &headers).await?;
    let based_on = draft_based_on(&state, req.based_on.as_deref()).await?;
    let (name, json) = draft_json(req.ruleset)?;
    let count: i64 = sqlx::query_scalar("SELECT count(*) FROM ruleset_drafts WHERE owner = $1")
        .bind(user.uid)
        .fetch_one(&state.pool)
        .await?;
    if count >= MAX_DRAFTS {
        return Err(ApiError::conflict(
            "too_many_drafts",
            format!("You have {MAX_DRAFTS} drafts. Publish or delete some first."),
        ));
    }
    let row: DraftRow = sqlx::query_as(
        "INSERT INTO ruleset_drafts (id, owner, based_on, name, json) VALUES ($1, $2, $3, $4, $5)
         RETURNING id, based_on, json, created_at, updated_at",
    )
    .bind(Uuid::new_v4())
    .bind(user.uid)
    .bind(&based_on)
    .bind(&name)
    .bind(&json)
    .fetch_one(&state.pool)
    .await?;
    Ok((StatusCode::CREATED, Json(row.view()?)))
}

/// `GET /v1/me/ruleset-drafts/<id>`.
pub async fn get_draft(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
) -> ApiResult<Json<DraftView>> {
    let user = editor(&state, &headers).await?;
    let row: DraftRow = sqlx::query_as(
        "SELECT id, based_on, json, created_at, updated_at FROM ruleset_drafts WHERE id = $1 AND owner = $2",
    )
    .bind(id)
    .bind(user.uid)
    .fetch_optional(&state.pool)
    .await?
    .ok_or_else(ApiError::not_found)?;
    Ok(Json(row.view()?))
}

/// `PUT /v1/me/ruleset-drafts/<id>` `{ruleset, basedOn?}`: replaces a draft.
pub async fn update_draft(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Json(req): Json<DraftReq>,
) -> ApiResult<Json<DraftView>> {
    let user = editor(&state, &headers).await?;
    let based_on = draft_based_on(&state, req.based_on.as_deref()).await?;
    let (name, json) = draft_json(req.ruleset)?;
    let row: DraftRow = sqlx::query_as(
        "UPDATE ruleset_drafts SET based_on = $3, name = $4, json = $5, updated_at = now()
          WHERE id = $1 AND owner = $2
         RETURNING id, based_on, json, created_at, updated_at",
    )
    .bind(id)
    .bind(user.uid)
    .bind(&based_on)
    .bind(&name)
    .bind(&json)
    .fetch_optional(&state.pool)
    .await?
    .ok_or_else(ApiError::not_found)?;
    Ok(Json(row.view()?))
}

/// `DELETE /v1/me/ruleset-drafts/<id>`: 204.
pub async fn delete_draft(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
) -> ApiResult<StatusCode> {
    let user = editor(&state, &headers).await?;
    let n = sqlx::query("DELETE FROM ruleset_drafts WHERE id = $1 AND owner = $2")
        .bind(id)
        .bind(user.uid)
        .execute(&state.pool)
        .await?
        .rows_affected();
    if n == 0 {
        return Err(ApiError::not_found());
    }
    Ok(StatusCode::NO_CONTENT)
}

// ---------------------------------------------------------------- the remembered ruleset

#[derive(Debug, Serialize, Deserialize)]
pub struct Remembered {
    /// Null: P+ default.
    pub ruleset: Option<RulesetView>,
}

async fn remembered_view(state: &AppState, uid: Uuid) -> ApiResult<Remembered> {
    let ruleset = match db::remembered_ruleset(&state.pool, uid).await? {
        Some(p) => {
            let own = p.owner == Some(uid);
            Some(view(state, p, own).await?)
        }
        None => None,
    };
    Ok(Remembered { ruleset })
}

/// `GET /v1/me/ruleset`: the account's remembered ruleset (decision 11: the launcher's box and
/// in-game Create Room start a room with it).
pub async fn remembered(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<Remembered>> {
    let user = editor(&state, &headers).await?;
    Ok(Json(remembered_view(&state, user.uid).await?))
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SetRememberedReq {
    /// A ruleset code, or null for P+ default.
    pub code: Option<String>,
}

/// `PUT /v1/me/ruleset` `{code}`: sets the remembered ruleset (null: P+ default). A code also goes
/// to the top of the recent list. 404 `unknown_ruleset` for a code nobody published.
pub async fn set_remembered(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<SetRememberedReq>,
) -> ApiResult<Json<Remembered>> {
    let user = editor(&state, &headers).await?;
    let code = match req.code.as_deref() {
        Some(c) => {
            let code = code_or_404(c)?;
            db::fetch_ruleset(&state.pool, &code).await?.ok_or_else(unknown_ruleset)?;
            Some(code)
        }
        None => None,
    };
    db::set_remembered_ruleset(&state.pool, user.uid, code.as_deref()).await?;
    Ok(Json(remembered_view(&state, user.uid).await?))
}
