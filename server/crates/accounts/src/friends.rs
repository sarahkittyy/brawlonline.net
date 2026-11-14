//! Friends and the ignore list (`docs/design/friends.md`).
//!
//! A friend request is a row in `friend_requests` until the other player accepts it (the two
//! become friends: a row in `friendships`) or the sender cancels it. It never expires and can be
//! sent while the other player is offline. Declining is silent: the row stays, marked declined,
//! so the sender still sees "Request sent" and sending again doesn't ask again. Two players
//! asking each other become friends at once. A request from someone the recipient ignores is
//! kept but never shown to them, and nobody is told; ignoring a friend ends the friendship and
//! drops the requests between the two.
//!
//! The ignore list (the launcher's and the game's chat) lives on the account since 2026-11-14.
//! The launcher uploads its install's `chat-hidden.json` once per account
//! (`POST /v1/me/ignores/import`) and writes that file from this list for the game.
//!
//! Each change tells the launchers concerned on their social stream: `friends {change, player?}`,
//! `change` being `request` (someone asked you), `accepted` (someone accepted your request) or
//! `changed` (read `GET /v1/friends` again).
//!
//! accounts keeps each launcher's friends in memory ([`FriendCache`]) for `GET /v1/rooms`, which
//! lists a friends-only room to the friends of its players (`rooms.rs`).

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};

use axum::extract::{Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::Json;
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use serde_json::json;
use sqlx::PgPool;
use uuid::Uuid;

use crate::api::{current_user, limit};
use crate::error::{ApiError, ApiResult};
use crate::profile::{name_tags, Ident};
use crate::store::UserRow;
use crate::AppState;

/// Friends per account.
pub const MAX_FRIENDS: i64 = 500;
/// Requests an account has waiting for an answer.
pub const MAX_OUTGOING: i64 = 100;
/// Players an account ignores (as many as the game's `chat-hidden.json` holds).
pub const MAX_IGNORED: i64 = 500;

/// A player in the lists: who they are and what they show beside their name.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Person {
    pub uid: String,
    pub display_name: String,
    pub connect_code: String,
    #[serde(flatten)]
    pub tag: crate::profile::NameTag,
    /// RFC 3339: when they became friends, or when the request was sent.
    pub since: String,
}

/// `GET /v1/friends`.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FriendsView {
    /// By name.
    pub friends: Vec<Person>,
    /// Requests to the caller, newest first (not declined, not from players they ignore).
    pub incoming: Vec<Person>,
    /// The caller's own requests waiting for an answer, newest first (declined ones too: the
    /// sender isn't told).
    pub outgoing: Vec<Person>,
}

/// Each account's friends, read from the database the first time they are needed and dropped when
/// they change. Only `GET /v1/rooms` reads it, for every launcher every few seconds.
#[derive(Default)]
pub struct FriendCache {
    map: Mutex<HashMap<Uuid, Arc<HashSet<Uuid>>>>,
}

impl FriendCache {
    pub async fn get(&self, pool: &PgPool, uid: Uuid) -> sqlx::Result<Arc<HashSet<Uuid>>> {
        if let Some(set) = self.map.lock().unwrap().get(&uid) {
            return Ok(set.clone());
        }
        let set = Arc::new(friend_uids(pool, uid).await?);
        self.map.lock().unwrap().insert(uid, set.clone());
        Ok(set)
    }

    pub fn forget(&self, uids: &[Uuid]) {
        let mut map = self.map.lock().unwrap();
        for uid in uids {
            map.remove(uid);
        }
    }
}

fn pair(a: Uuid, b: Uuid) -> (Uuid, Uuid) {
    if a < b {
        (a, b)
    } else {
        (b, a)
    }
}

async fn friend_uids(pool: &PgPool, uid: Uuid) -> sqlx::Result<HashSet<Uuid>> {
    let rows: Vec<(Uuid,)> =
        sqlx::query_as("SELECT CASE WHEN lo = $1 THEN hi ELSE lo END FROM friendships WHERE lo = $1 OR hi = $1")
            .bind(uid)
            .fetch_all(pool)
            .await?;
    Ok(rows.into_iter().map(|(u,)| u).collect())
}

/// The caller's lists.
pub async fn view(pool: &PgPool, uid: Uuid) -> sqlx::Result<FriendsView> {
    type Row = (Uuid, String, Option<String>, DateTime<Utc>);
    let not_banned = "(u.banned_until IS NULL OR u.banned_until <= now())";
    let friends: Vec<Row> = sqlx::query_as(&format!(
        "SELECT u.uid, u.display_name, u.connect_code, f.since FROM friendships f
           JOIN users u ON u.uid = CASE WHEN f.lo = $1 THEN f.hi ELSE f.lo END
          WHERE (f.lo = $1 OR f.hi = $1) AND {not_banned}
          ORDER BY lower(u.display_name), u.uid"
    ))
    .bind(uid)
    .fetch_all(pool)
    .await?;
    let incoming: Vec<Row> = sqlx::query_as(&format!(
        "SELECT u.uid, u.display_name, u.connect_code, r.created_at FROM friend_requests r
           JOIN users u ON u.uid = r.from_uid
          WHERE r.to_uid = $1 AND NOT r.declined AND {not_banned}
            AND NOT EXISTS (SELECT 1 FROM ignores i WHERE i.uid = $1 AND i.ignored = r.from_uid)
          ORDER BY r.created_at DESC"
    ))
    .bind(uid)
    .fetch_all(pool)
    .await?;
    let outgoing: Vec<Row> = sqlx::query_as(&format!(
        "SELECT u.uid, u.display_name, u.connect_code, r.created_at FROM friend_requests r
           JOIN users u ON u.uid = r.to_uid
          WHERE r.from_uid = $1 AND {not_banned}
          ORDER BY r.created_at DESC"
    ))
    .bind(uid)
    .fetch_all(pool)
    .await?;
    let mut uids: Vec<Uuid> = friends.iter().chain(&incoming).chain(&outgoing).map(|r| r.0).collect();
    uids.sort();
    uids.dedup();
    let tags = name_tags(pool, &uids).await?;
    let people = |rows: Vec<Row>| -> Vec<Person> {
        rows.into_iter()
            .map(|(uid, name, code, at)| Person {
                uid: uid.to_string(),
                display_name: name,
                connect_code: code.unwrap_or_default(),
                tag: tags.get(&uid).cloned().unwrap_or_default(),
                since: at.to_rfc3339_opts(chrono::SecondsFormat::Secs, true),
            })
            .collect()
    };
    Ok(FriendsView { friends: people(friends), incoming: people(incoming), outgoing: people(outgoing) })
}

/// One player as the lists show them (for the events).
async fn person(pool: &PgPool, u: &UserRow) -> sqlx::Result<Person> {
    let tags = name_tags(pool, &[u.uid]).await?;
    Ok(Person {
        uid: u.uid.to_string(),
        display_name: u.display_name.clone(),
        connect_code: u.connect_code.clone().unwrap_or_default(),
        tag: tags.get(&u.uid).cloned().unwrap_or_default(),
        since: Utc::now().to_rfc3339_opts(chrono::SecondsFormat::Secs, true),
    })
}

async fn ignores(pool: &PgPool, who: Uuid, whom: Uuid) -> sqlx::Result<bool> {
    let row: Option<(i32,)> = sqlx::query_as("SELECT 1 FROM ignores WHERE uid = $1 AND ignored = $2")
        .bind(who)
        .bind(whom)
        .fetch_optional(pool)
        .await?;
    Ok(row.is_some())
}

async fn are_friends(pool: &PgPool, a: Uuid, b: Uuid) -> sqlx::Result<bool> {
    let (lo, hi) = pair(a, b);
    let row: Option<(i32,)> = sqlx::query_as("SELECT 1 FROM friendships WHERE lo = $1 AND hi = $2")
        .bind(lo)
        .bind(hi)
        .fetch_optional(pool)
        .await?;
    Ok(row.is_some())
}

async fn count(pool: &PgPool, sql: &str, uid: Uuid) -> sqlx::Result<i64> {
    let (n,): (i64,) = sqlx::query_as(sql).bind(uid).fetch_one(pool).await?;
    Ok(n)
}

async fn friend_count(pool: &PgPool, uid: Uuid) -> sqlx::Result<i64> {
    count(pool, "SELECT count(*) FROM friendships WHERE lo = $1 OR hi = $1", uid).await
}

/// Makes `a` and `b` friends and drops the requests between them.
async fn befriend(pool: &PgPool, a: Uuid, b: Uuid) -> sqlx::Result<()> {
    let (lo, hi) = pair(a, b);
    let mut tx = pool.begin().await?;
    sqlx::query("INSERT INTO friendships (lo, hi) VALUES ($1, $2) ON CONFLICT DO NOTHING")
        .bind(lo)
        .bind(hi)
        .execute(&mut *tx)
        .await?;
    sqlx::query("DELETE FROM friend_requests WHERE (from_uid = $1 AND to_uid = $2) OR (from_uid = $2 AND to_uid = $1)")
        .bind(a)
        .bind(b)
        .execute(&mut *tx)
        .await?;
    tx.commit().await
}

/// Ends a friendship and the requests between `a` and `b`. Whether they were friends.
async fn unfriend(pool: &PgPool, a: Uuid, b: Uuid) -> sqlx::Result<bool> {
    let (lo, hi) = pair(a, b);
    let mut tx = pool.begin().await?;
    let gone = sqlx::query("DELETE FROM friendships WHERE lo = $1 AND hi = $2")
        .bind(lo)
        .bind(hi)
        .execute(&mut *tx)
        .await?
        .rows_affected()
        > 0;
    sqlx::query("DELETE FROM friend_requests WHERE (from_uid = $1 AND to_uid = $2) OR (from_uid = $2 AND to_uid = $1)")
        .bind(a)
        .bind(b)
        .execute(&mut *tx)
        .await?;
    tx.commit().await?;
    Ok(gone)
}

fn tell(state: &AppState, uid: Uuid, change: &str, player: Option<&Person>) {
    let mut ev = json!({"type": "friends", "change": change});
    if let Some(p) = player {
        ev["player"] = json!(p);
    }
    state.social.notify(&uid.to_string(), ev.to_string());
}

fn parse_uid(s: &str) -> ApiResult<Uuid> {
    Uuid::parse_str(s).map_err(|_| ApiError::bad_request("invalid", "Not a player."))
}

/// The other player, by uid: an account that plays online and isn't banned.
async fn other(pool: &PgPool, uid: Uuid) -> ApiResult<UserRow> {
    crate::store::user_by_uid(pool, uid)
        .await?
        .filter(|u| u.connect_code.is_some() && !u.is_banned())
        .ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "no_player", "That player doesn't exist."))
}

/// `GET /v1/friends`: [`FriendsView`]. Session auth.
pub async fn list(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<FriendsView>> {
    let user = current_user(&state, &headers).await?;
    Ok(Json(view(&state.pool, user.uid).await?))
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RequestReq {
    /// A connect code (`ABCD#123`), a profile address (`abcd-123`, or the whole link) or a uid.
    pub to: String,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RequestAnswer {
    /// `sent`: the request waits for them; `friends`: they had asked too (or already were).
    pub status: &'static str,
    pub player: Person,
    #[serde(flatten)]
    pub view: FriendsView,
}

/// `POST /v1/friends/requests {to}`: asks a player to be friends ([`RequestAnswer`]). 404
/// `no_player` "No player has the code …", 400 `invalid` (yourself), 409 `ignoring` (you ignore
/// them), 409 `friends_full` / `too_many_requests` ([`MAX_FRIENDS`], [`MAX_OUTGOING`]), 429 over
/// [`common::ratelimit::FRIEND_REQUEST_WINDOWS`].
pub async fn request(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<RequestReq>,
) -> ApiResult<Json<RequestAnswer>> {
    let user = current_user(&state, &headers).await?;
    let typed = req.to.trim();
    // A pasted profile link: its last part is the address.
    let addr = typed.trim_end_matches('/').rsplit('/').next().unwrap_or(typed);
    let not_found = || {
        let shown: String = typed.chars().take(40).collect();
        ApiError::new(StatusCode::NOT_FOUND, "no_player", format!("No player has the code {shown}."))
    };
    let ident = Ident::parse(addr).ok_or_else(not_found)?;
    let target = match &ident {
        Ident::Uid(uid) => crate::store::user_by_uid(&state.pool, *uid).await?,
        Ident::Code(code) => crate::store::find_user(&state.pool, &code.to_string()).await?,
    }
    .filter(|u| u.connect_code.is_some() && !u.is_banned())
    .ok_or_else(not_found)?;
    if target.uid == user.uid {
        return Err(ApiError::bad_request("invalid", "That's your own code."));
    }
    let pool = &state.pool;
    if are_friends(pool, user.uid, target.uid).await? {
        let player = person(pool, &target).await?;
        return Ok(Json(RequestAnswer { status: "friends", player, view: view(pool, user.uid).await? }));
    }
    if ignores(pool, user.uid, target.uid).await? {
        return Err(ApiError::conflict("ignoring", "You're ignoring this player. Unignore them first."));
    }
    limit(&state, |l, now| l.friend_requests.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many friend requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    // They asked first: friends now.
    let theirs: Option<(bool,)> =
        sqlx::query_as("SELECT declined FROM friend_requests WHERE from_uid = $1 AND to_uid = $2")
            .bind(target.uid)
            .bind(user.uid)
            .fetch_optional(pool)
            .await?;
    if theirs.is_some() {
        accept_from(&state, &user, &target).await?;
        let player = person(pool, &target).await?;
        return Ok(Json(RequestAnswer { status: "friends", player, view: view(pool, user.uid).await? }));
    }
    if friend_count(pool, user.uid).await? >= MAX_FRIENDS {
        return Err(ApiError::conflict("friends_full", format!("Your friends list is full ({MAX_FRIENDS}).")));
    }
    let waiting = count(pool, "SELECT count(*) FROM friend_requests WHERE from_uid = $1", user.uid).await?;
    if waiting >= MAX_OUTGOING {
        return Err(ApiError::conflict(
            "too_many_requests",
            format!("You have {MAX_OUTGOING} requests waiting. Cancel some first."),
        ));
    }
    // Sent again: the first one stands (declined stays declined).
    let fresh = sqlx::query(
        "INSERT INTO friend_requests (from_uid, to_uid) VALUES ($1, $2) ON CONFLICT (from_uid, to_uid) DO NOTHING",
    )
    .bind(user.uid)
    .bind(target.uid)
    .execute(pool)
    .await?
    .rows_affected()
        > 0;
    if fresh && !ignores(pool, target.uid, user.uid).await? {
        tell(&state, target.uid, "request", Some(&person(pool, &user).await?));
    }
    let player = person(pool, &target).await?;
    Ok(Json(RequestAnswer { status: "sent", player, view: view(pool, user.uid).await? }))
}

/// `user` accepts `from`'s request: friends, and `from` is told.
async fn accept_from(state: &AppState, user: &UserRow, from: &UserRow) -> ApiResult<()> {
    let pool = &state.pool;
    for (who, msg) in [(user.uid, "Your friends list is full."), (from.uid, "Their friends list is full.")] {
        if friend_count(pool, who).await? >= MAX_FRIENDS {
            return Err(ApiError::conflict("friends_full", msg));
        }
    }
    befriend(pool, user.uid, from.uid).await?;
    state.friend_cache.forget(&[user.uid, from.uid]);
    tell(state, from.uid, "accepted", Some(&person(pool, user).await?));
    Ok(())
}

/// `POST /v1/friends/requests/{uid}/accept`: friends with the player who asked; 404 `no_request`.
pub async fn accept(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
) -> ApiResult<Json<FriendsView>> {
    let user = current_user(&state, &headers).await?;
    let from = parse_uid(&uid)?;
    let asked: Option<(bool,)> =
        sqlx::query_as("SELECT declined FROM friend_requests WHERE from_uid = $1 AND to_uid = $2")
            .bind(from)
            .bind(user.uid)
            .fetch_optional(&state.pool)
            .await?;
    if asked.is_none() {
        return Err(ApiError::new(StatusCode::NOT_FOUND, "no_request", "That request is gone."));
    }
    let from = other(&state.pool, from).await?;
    accept_from(&state, &user, &from).await?;
    Ok(Json(view(&state.pool, user.uid).await?))
}

/// `POST /v1/friends/requests/{uid}/decline`: hides the request; the sender isn't told.
pub async fn decline(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
) -> ApiResult<Json<FriendsView>> {
    let user = current_user(&state, &headers).await?;
    let from = parse_uid(&uid)?;
    sqlx::query("UPDATE friend_requests SET declined = true WHERE from_uid = $1 AND to_uid = $2")
        .bind(from)
        .bind(user.uid)
        .execute(&state.pool)
        .await?;
    Ok(Json(view(&state.pool, user.uid).await?))
}

/// `DELETE /v1/friends/requests/{uid}`: takes back the caller's own request.
pub async fn cancel(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
) -> ApiResult<Json<FriendsView>> {
    let user = current_user(&state, &headers).await?;
    let to = parse_uid(&uid)?;
    let shown: Option<(bool,)> =
        sqlx::query_as("DELETE FROM friend_requests WHERE from_uid = $1 AND to_uid = $2 RETURNING declined")
            .bind(user.uid)
            .bind(to)
            .fetch_optional(&state.pool)
            .await?;
    // Gone from their requests (unless they never saw it).
    if shown.is_some_and(|(declined,)| !declined) && !ignores(&state.pool, to, user.uid).await? {
        tell(&state, to, "changed", None);
    }
    Ok(Json(view(&state.pool, user.uid).await?))
}

/// `DELETE /v1/friends/{uid}`: no longer friends (both lists change).
pub async fn remove(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
) -> ApiResult<Json<FriendsView>> {
    let user = current_user(&state, &headers).await?;
    let other = parse_uid(&uid)?;
    if unfriend(&state.pool, user.uid, other).await? {
        state.friend_cache.forget(&[user.uid, other]);
        tell(&state, other, "changed", None);
    }
    Ok(Json(view(&state.pool, user.uid).await?))
}

// ---------------------------------------------------------------- the ignore list

/// `GET /v1/me/ignores`.
#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct IgnoresView {
    /// Uids, the oldest first.
    pub ignored: Vec<String>,
    /// The launcher has uploaded its install's list for this account.
    pub imported: bool,
}

async fn ignores_view(pool: &PgPool, uid: Uuid) -> sqlx::Result<IgnoresView> {
    let rows: Vec<(Uuid,)> = sqlx::query_as("SELECT ignored FROM ignores WHERE uid = $1 ORDER BY created_at, ignored")
        .bind(uid)
        .fetch_all(pool)
        .await?;
    let (imported,): (bool,) =
        sqlx::query_as("SELECT ignores_imported FROM users WHERE uid = $1").bind(uid).fetch_one(pool).await?;
    Ok(IgnoresView { ignored: rows.into_iter().map(|(u,)| u.to_string()).collect(), imported })
}

/// Adds `whom` to `uid`'s ignores (an account that exists, not themselves) and ends anything
/// between them. Whether it was added.
async fn add_ignore(state: &AppState, uid: Uuid, whom: Uuid) -> ApiResult<bool> {
    let pool = &state.pool;
    if whom == uid {
        return Ok(false);
    }
    let added = sqlx::query(
        "INSERT INTO ignores (uid, ignored) SELECT $1, $2 WHERE EXISTS (SELECT 1 FROM users WHERE uid = $2)
         ON CONFLICT DO NOTHING",
    )
    .bind(uid)
    .bind(whom)
    .execute(pool)
    .await?
    .rows_affected()
        > 0;
    let had: Option<(bool,)> =
        sqlx::query_as("SELECT declined FROM friend_requests WHERE from_uid = $1 AND to_uid = $2")
            .bind(uid)
            .bind(whom)
            .fetch_optional(pool)
            .await?;
    let was_friend = unfriend(pool, uid, whom).await?;
    if was_friend {
        state.friend_cache.forget(&[uid, whom]);
    }
    if was_friend || had.is_some_and(|(declined,)| !declined) {
        tell(state, whom, "changed", None);
    }
    Ok(added)
}

/// `GET /v1/me/ignores`: [`IgnoresView`].
pub async fn ignores_list(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<IgnoresView>> {
    let user = current_user(&state, &headers).await?;
    Ok(Json(ignores_view(&state.pool, user.uid).await?))
}

/// `PUT /v1/me/ignores/{uid}`: ignores a player; a friend no longer is one, and the requests
/// between the two are gone. 409 `ignores_full` over [`MAX_IGNORED`].
pub async fn ignore(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
) -> ApiResult<Json<IgnoresView>> {
    let user = current_user(&state, &headers).await?;
    let whom = parse_uid(&uid)?;
    if count(&state.pool, "SELECT count(*) FROM ignores WHERE uid = $1", user.uid).await? >= MAX_IGNORED {
        return Err(ApiError::conflict("ignores_full", "You're ignoring too many players."));
    }
    add_ignore(&state, user.uid, whom).await?;
    Ok(Json(ignores_view(&state.pool, user.uid).await?))
}

/// `DELETE /v1/me/ignores/{uid}`.
pub async fn unignore(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(uid): Path<String>,
) -> ApiResult<Json<IgnoresView>> {
    let user = current_user(&state, &headers).await?;
    let whom = parse_uid(&uid)?;
    sqlx::query("DELETE FROM ignores WHERE uid = $1 AND ignored = $2")
        .bind(user.uid)
        .bind(whom)
        .execute(&state.pool)
        .await?;
    Ok(Json(ignores_view(&state.pool, user.uid).await?))
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ImportReq {
    /// The install's `chat-hidden.json`: uids (others are skipped).
    pub uids: Vec<String>,
}

/// `POST /v1/me/ignores/import {uids}`: the launcher's one upload of its install's list for this
/// account. Once done (`imported`), later calls change nothing. Unknown uids are skipped; at most
/// [`MAX_IGNORED`].
pub async fn import(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<ImportReq>,
) -> ApiResult<Json<IgnoresView>> {
    let user = current_user(&state, &headers).await?;
    let done: (bool,) = sqlx::query_as("SELECT ignores_imported FROM users WHERE uid = $1")
        .bind(user.uid)
        .fetch_one(&state.pool)
        .await?;
    if !done.0 {
        let mut have = count(&state.pool, "SELECT count(*) FROM ignores WHERE uid = $1", user.uid).await?;
        for uid in req.uids.iter().filter_map(|u| Uuid::parse_str(u).ok()).take(MAX_IGNORED as usize) {
            if have >= MAX_IGNORED {
                break;
            }
            if add_ignore(&state, user.uid, uid).await? {
                have += 1;
            }
        }
        sqlx::query("UPDATE users SET ignores_imported = true WHERE uid = $1")
            .bind(user.uid)
            .execute(&state.pool)
            .await?;
    }
    Ok(Json(ignores_view(&state.pool, user.uid).await?))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pairs_are_ordered() {
        let a = Uuid::from_u128(1);
        let b = Uuid::from_u128(2);
        assert_eq!(pair(a, b), (a, b));
        assert_eq!(pair(b, a), (a, b));
    }
}
