//! The launcher's admin tools (`/v1/admin/...`): the same changes `pp-admin` makes (mute, rename,
//! connect code, name lock, ban, room names), for an account with `users.role = 'admin'` only. Every
//! change is audit-logged with the admin as the actor (`launcher:<their uid>`). mm and the social hub
//! see them as they see `pp-admin`'s: with their next look at the changed accounts.

use axum::extract::{Path, Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::Json;
use common::codes::{parse_connect_code, validate_display_name};
use common::rooms::{clean_room_name, parse_room_code, RoomKind};
use serde::{Deserialize, Serialize};
use uuid::Uuid;

use crate::api::current_user;
use crate::error::{ApiError, ApiResult};
use crate::store::{self, UserRow};
use crate::AppState;

/// Players listed without a search: the most recently seen.
pub const RECENT_PLAYERS: i64 = 500;
/// Players a search finds at most.
pub const SEARCH_PLAYERS: i64 = 100;

/// The account, if it is an admin's: logged in (Bearer session) with `role = 'admin'`. 403
/// `not_admin` for anyone else.
pub async fn require_admin(state: &AppState, headers: &HeaderMap) -> ApiResult<UserRow> {
    let user = current_user(state, headers).await?;
    if user.role != "admin" {
        return Err(ApiError::forbidden("not_admin", "This account is not an admin."));
    }
    Ok(user)
}

fn actor(admin: &UserRow) -> String {
    format!("launcher:{}", admin.uid)
}

/// A player in the admin's list (the online list's offline part).
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AdminPlayer {
    pub uid: Uuid,
    pub display_name: String,
    pub connect_code: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub main: Option<String>,
    pub banned: bool,
}

#[derive(Debug, Serialize)]
pub struct AdminPlayers {
    pub players: Vec<AdminPlayer>,
}

#[derive(Debug, Deserialize)]
pub struct PlayersQuery {
    /// Part of a name or connect code; none: the most recently seen players.
    pub q: Option<String>,
}

/// `LIKE` text matching `s` literally.
fn like_escape(s: &str) -> String {
    s.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_")
}

/// `GET /v1/admin/players?q=`: accounts with a connect code, banned ones too (`banned`). Without
/// `q` the [`RECENT_PLAYERS`] most recently seen (their latest session's use), with it up to
/// [`SEARCH_PLAYERS`] whose name or code contains it (any case), by name. Each with the flag and
/// main the player shows beside their name, as the online list has them.
pub async fn players(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(query): Query<PlayersQuery>,
) -> ApiResult<Json<AdminPlayers>> {
    require_admin(&state, &headers).await?;
    let needle = query.q.as_deref().map(str::trim).unwrap_or_default();
    if needle.chars().count() > 64 {
        return Err(ApiError::bad_request("invalid", "The search is too long."));
    }
    let rows: Vec<(Uuid, String, String, bool)> = if needle.is_empty() {
        sqlx::query_as(
            "SELECT u.uid, u.display_name, u.connect_code, COALESCE(u.banned_until > now(), false)
               FROM users u
               LEFT JOIN LATERAL (SELECT max(s.last_seen) AS seen FROM sessions s WHERE s.uid = u.uid) s ON true
              WHERE u.connect_code IS NOT NULL
              ORDER BY s.seen DESC NULLS LAST, u.created_at DESC
              LIMIT $1",
        )
        .bind(RECENT_PLAYERS)
        .fetch_all(&state.pool)
        .await?
    } else {
        let pattern = format!("%{}%", like_escape(needle));
        sqlx::query_as(
            "SELECT uid, display_name, connect_code, COALESCE(banned_until > now(), false)
               FROM users
              WHERE connect_code IS NOT NULL AND (display_name ILIKE $1 OR connect_code ILIKE $1)
              ORDER BY lower(display_name), connect_code
              LIMIT $2",
        )
        .bind(pattern)
        .bind(SEARCH_PLAYERS)
        .fetch_all(&state.pool)
        .await?
    };
    let uids: Vec<Uuid> = rows.iter().map(|r| r.0).collect();
    let tags = crate::profile::name_tags(&state.pool, &uids).await?;
    let players = rows
        .into_iter()
        .map(|(uid, display_name, connect_code, banned)| {
            let tag = tags.get(&uid);
            AdminPlayer {
                uid,
                display_name,
                connect_code,
                country: tag.and_then(|t| t.country.clone()),
                main: tag.and_then(|t| t.main.clone()),
                banned,
            }
        })
        .collect();
    Ok(Json(AdminPlayers { players }))
}

/// What the admin's player menu and rename box need to know about an account.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AdminUser {
    pub uid: Uuid,
    pub display_name: String,
    /// Empty before online play is set up.
    pub connect_code: String,
    pub muted: bool,
    pub name_locked: bool,
    pub banned: bool,
    pub admin: bool,
}

impl AdminUser {
    fn of(u: &UserRow) -> Self {
        AdminUser {
            uid: u.uid,
            display_name: u.display_name.clone(),
            connect_code: u.connect_code.clone().unwrap_or_default(),
            muted: u.chat_muted_at.is_some(),
            name_locked: u.rename_locked_at.is_some(),
            banned: u.is_banned(),
            admin: u.role == "admin",
        }
    }
}

async fn target(state: &AppState, uid: &str) -> ApiResult<UserRow> {
    let uid = Uuid::parse_str(uid).map_err(|_| ApiError::not_found())?;
    store::user_by_uid(&state.pool, uid).await?.ok_or_else(ApiError::not_found)
}

/// An admin account can't be muted or banned from here (nor can the admin themselves).
fn not_an_admin(u: &UserRow) -> ApiResult<()> {
    if u.role == "admin" {
        return Err(ApiError::forbidden("target_admin", "Admin accounts can't be muted or banned."));
    }
    Ok(())
}

/// `GET /v1/admin/users/{uid}`: the account as [`AdminUser`]. 404 for no such account.
pub async fn user(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
) -> ApiResult<Json<AdminUser>> {
    require_admin(&state, &headers).await?;
    Ok(Json(AdminUser::of(&target(&state, &uid).await?)))
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MutedReq {
    pub muted: bool,
}

/// `PUT /v1/admin/users/{uid}/muted` `{muted}`: mutes (`pp-admin mute`, a shadowban) or unmutes the
/// account's chat. 403 `target_admin` for an admin account.
pub async fn set_muted(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
    Json(req): Json<MutedReq>,
) -> ApiResult<Json<AdminUser>> {
    let admin = require_admin(&state, &headers).await?;
    let u = target(&state, &uid).await?;
    if req.muted {
        not_an_admin(&u)?;
    }
    store::set_muted(&state.pool, u.uid, req.muted).await?;
    let action = if req.muted { "user.mute" } else { "user.unmute" };
    store::audit(&state.pool, &actor(&admin), action, Some(&u.uid.to_string()), serde_json::json!({})).await?;
    Ok(Json(AdminUser::of(&target(&state, &uid).await?)))
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BannedReq {
    pub banned: bool,
}

/// `PUT /v1/admin/users/{uid}/banned` `{banned}`: bans the account for good (`pp-admin user ban`
/// without `--days`: its sessions end, its play key changes, its launcher is told) or lifts a ban.
/// 403 `target_admin` for an admin account.
pub async fn set_banned(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
    Json(req): Json<BannedReq>,
) -> ApiResult<Json<AdminUser>> {
    let admin = require_admin(&state, &headers).await?;
    let u = target(&state, &uid).await?;
    if req.banned {
        not_an_admin(&u)?;
        let until = store::permanent_ban();
        store::set_ban(&state.pool, u.uid, Some(until), None).await?;
        store::audit(
            &state.pool,
            &actor(&admin),
            "user.ban",
            Some(&u.uid.to_string()),
            serde_json::json!({"until": until, "reason": null}),
        )
        .await?;
    } else {
        store::set_ban(&state.pool, u.uid, None, None).await?;
        store::audit(&state.pool, &actor(&admin), "user.unban", Some(&u.uid.to_string()), serde_json::json!({}))
            .await?;
    }
    Ok(Json(AdminUser::of(&target(&state, &uid).await?)))
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct UpdateUserReq {
    pub display_name: Option<String>,
    /// `ABCD#123`; another account's code is refused (409 `code_taken`).
    pub connect_code: Option<String>,
    /// Whether the player may rename themselves (`pp-admin user lock-name` / `unlock-name`).
    pub name_locked: Option<bool>,
}

/// `PUT /v1/admin/users/{uid}` `{displayName?, connectCode?, nameLocked?}`: renames the account,
/// gives it another connect code and locks or unlocks its name, all at once or none (`pp-admin user
/// rename`, `set-code`, `lock-name`). A field left out stays. 400 `invalid_display_name` /
/// `invalid_connect_code`, 409 `code_taken` (with who has it).
pub async fn update_user(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
    Json(req): Json<UpdateUserReq>,
) -> ApiResult<Json<AdminUser>> {
    let admin = require_admin(&state, &headers).await?;
    let u = target(&state, &uid).await?;
    let name = req.display_name.filter(|n| *n != u.display_name);
    if let Some(n) = &name {
        validate_display_name(n).map_err(|e| ApiError::bad_request("invalid_display_name", e.to_string()))?;
    }
    let code = match &req.connect_code {
        Some(c) => Some(
            parse_connect_code(c)
                .map_err(|e| ApiError::bad_request("invalid_connect_code", e.to_string()))?
                .to_string(),
        ),
        None => None,
    }
    .filter(|c| Some(c) != u.connect_code.as_ref());
    if let Some(c) = &code {
        if let Some(other) = store::find_user(&state.pool, c).await?.filter(|o| o.uid != u.uid) {
            return Err(code_taken(c, &other));
        }
    }
    let lock = req.name_locked.filter(|l| *l != u.rename_locked_at.is_some());

    let mut tx = state.pool.begin().await?;
    if let Some(n) = &name {
        sqlx::query("UPDATE users SET display_name = $2, updated_at = now() WHERE uid = $1")
            .bind(u.uid)
            .bind(n)
            .execute(&mut *tx)
            .await?;
    }
    if let Some(c) = &code {
        let res = sqlx::query("UPDATE users SET connect_code = $2, updated_at = now() WHERE uid = $1")
            .bind(u.uid)
            .bind(c)
            .execute(&mut *tx)
            .await;
        if let Err(e) = res {
            // Taken between the check and now.
            if store::is_unique_violation(&e, "connect_code") {
                return Err(ApiError::conflict("code_taken", format!("{c} is taken.")));
            }
            return Err(e.into());
        }
    }
    if let Some(l) = lock {
        sqlx::query(
            "UPDATE users SET rename_locked_at = CASE WHEN $2 THEN COALESCE(rename_locked_at, now()) END
              WHERE uid = $1",
        )
        .bind(u.uid)
        .bind(l)
        .execute(&mut *tx)
        .await?;
    }
    let actor = actor(&admin);
    let target_uid = u.uid.to_string();
    let audit = |action: &'static str, detail: serde_json::Value| {
        sqlx::query("INSERT INTO audit_log (actor, action, target, detail) VALUES ($1, $2, $3, $4)")
            .bind(actor.clone())
            .bind(action)
            .bind(target_uid.clone())
            .bind(sqlx::types::Json(detail))
    };
    if let Some(n) = &name {
        audit("user.rename", serde_json::json!({"from": u.display_name, "to": n})).execute(&mut *tx).await?;
    }
    if let Some(c) = &code {
        audit("user.set_code", serde_json::json!({"from": u.connect_code, "to": c})).execute(&mut *tx).await?;
    }
    match lock {
        Some(true) => {
            audit(
                "user.lock_name",
                serde_json::json!({"name": name.as_ref().unwrap_or(&u.display_name), "reason": null}),
            )
            .execute(&mut *tx)
            .await?;
        }
        Some(false) => {
            audit("user.unlock_name", serde_json::json!({})).execute(&mut *tx).await?;
        }
        None => {}
    }
    tx.commit().await?;
    Ok(Json(AdminUser::of(&target(&state, &uid).await?)))
}

fn code_taken(code: &str, other: &UserRow) -> ApiError {
    ApiError::conflict("code_taken", format!("{code} is taken by {}.", other.display_name))
}

#[derive(Debug, Deserialize)]
pub struct CodeQuery {
    pub code: String,
}

/// Who has a connect code (the rename box says so before Apply).
#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CodeOwner {
    /// The code as stored (`abcd#0123` -> `ABCD#123`).
    pub code: String,
    /// null: nobody has it.
    pub uid: Option<Uuid>,
    pub display_name: Option<String>,
}

/// `GET /v1/admin/codes?code=ABCD%23123`: who has the code ([`CodeOwner`]). 400
/// `invalid_connect_code`.
pub async fn code_owner(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(query): Query<CodeQuery>,
) -> ApiResult<Json<CodeOwner>> {
    require_admin(&state, &headers).await?;
    let code = parse_connect_code(&query.code)
        .map_err(|e| ApiError::bad_request("invalid_connect_code", e.to_string()))?
        .to_string();
    let owner = store::find_user(&state.pool, &code).await?;
    Ok(Json(CodeOwner { code, uid: owner.as_ref().map(|o| o.uid), display_name: owner.map(|o| o.display_name) }))
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoomNameReq {
    /// Empty or null: no name ("alice's room").
    pub name: Option<String>,
}

#[derive(Debug, Serialize)]
pub struct RoomNameView {
    pub kind: RoomKind,
    pub name: Option<String>,
}

/// `PUT /v1/admin/rooms/{code}/name` `{name}`: renames the open room (P+ or Brawl Mods) as its host
/// would (`pp-admin room rename`): the name is stored on the host's account for their rooms of that
/// kind, and mm renames the room within a few seconds. 404 `unknown_room` when no such room is open,
/// 503 `mm_unavailable`.
pub async fn rename_room(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(code): Path<String>,
    Json(req): Json<RoomNameReq>,
) -> ApiResult<Json<RoomNameView>> {
    let admin = require_admin(&state, &headers).await?;
    let name = clean_room_name(req.name.as_deref().unwrap_or_default())
        .map_err(|e| ApiError::bad_request("invalid_room_name", e))?;
    let code = parse_room_code(&code).ok_or_else(|| ApiError::bad_request("invalid", "Invalid room code."))?;
    let s = state.mm_status.get().await.map_err(|e| {
        tracing::warn!("mm status unavailable: {e}");
        ApiError::new(
            StatusCode::SERVICE_UNAVAILABLE,
            "mm_unavailable",
            "The matchmaking server is not answering. Try again later.",
        )
    })?;
    let pplus = s.members.iter().find(|m| m.host && !m.twos && m.code == code).map(|m| (&m.uid, RoomKind::Pplus));
    let modded = s.mod_members.iter().find(|m| m.host && m.code == code).map(|m| (&m.uid, RoomKind::Mods));
    let (host, kind) =
        pplus.or(modded).ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "unknown_room", "Room not found."))?;
    let host = Uuid::parse_str(host).map_err(ApiError::internal)?;
    let (from_pplus, from_mods): (Option<String>, Option<String>) =
        sqlx::query_as("SELECT room_name, mods_room_name FROM users WHERE uid = $1")
            .bind(host)
            .fetch_optional(&state.pool)
            .await?
            .ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "unknown_room", "Room not found."))?;
    let from = if kind == RoomKind::Mods { from_mods } else { from_pplus };
    store::set_room_name(&state.pool, host, kind, name.as_deref()).await?;
    store::audit(
        &state.pool,
        &actor(&admin),
        "user.room_name",
        Some(&host.to_string()),
        serde_json::json!({
            "kind": if kind == RoomKind::Mods { "mods" } else { "pplus" },
            "room": code,
            "from": from,
            "to": name,
        }),
    )
    .await?;
    Ok(Json(RoomNameView { kind, name }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn like_text_is_literal() {
        assert_eq!(like_escape("a_b%c\\d"), "a\\_b\\%c\\\\d");
        assert_eq!(like_escape("SARA#694"), "SARA#694");
    }
}
