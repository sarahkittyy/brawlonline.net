//! Database operations for accounts. Used by the HTTP handlers and the admin CLI.

use chrono::{DateTime, Duration, Utc};
use common::codes::{candidate_numbers, ConnectCode};
use common::playkey::{hash_token, random_token};
use common::rooms::RoomKind;
use rand::Rng;
use sqlx::{PgPool, Postgres, Transaction};
use uuid::Uuid;

#[derive(Debug, Clone, sqlx::FromRow)]
pub struct UserRow {
    pub uid: Uuid,
    pub email: String,
    pub email_verified_at: Option<DateTime<Utc>>,
    pub pw_hash: String,
    pub display_name: String,
    pub connect_code: Option<String>,
    pub play_key_version: i32,
    pub rules_version: i32,
    pub role: String,
    pub banned_until: Option<DateTime<Utc>>,
    pub ban_reason: Option<String>,
    pub created_at: DateTime<Utc>,
    /// Chat shadowban (`pp-admin mute`): its chat messages and DMs go nowhere, unknown to it.
    pub chat_muted_at: Option<DateTime<Utc>>,
    /// Rename lock (`pp-admin user lock-name`): `/v1/me/rename` refuses.
    pub rename_locked_at: Option<DateTime<Utc>>,
}

impl UserRow {
    pub fn is_banned(&self) -> bool {
        self.banned_until.is_some_and(|t| t > Utc::now())
    }
    pub fn email_verified(&self) -> bool {
        self.email_verified_at.is_some()
    }
}

const USER_COLS: &str = "uid, email, email_verified_at, pw_hash, display_name, connect_code, \
                         play_key_version, rules_version, role, banned_until, ban_reason, created_at,                          chat_muted_at, rename_locked_at";

/// "Banned forever" is stored as a far-future timestamp.
pub fn permanent_ban() -> DateTime<Utc> {
    DateTime::parse_from_rfc3339("9999-12-31T00:00:00Z").unwrap().with_timezone(&Utc)
}

/// Trims and lowercases an email and applies a basic shape check.
pub fn normalize_email(input: &str) -> Result<String, &'static str> {
    let e = input.trim().to_lowercase();
    if e.len() > 254 || e.chars().any(|c| c.is_whitespace() || c.is_control()) {
        return Err("Invalid email address");
    }
    let (local, domain) = e.split_once('@').ok_or("Invalid email address")?;
    if local.is_empty()
        || domain.contains('@')
        || !domain.contains('.')
        || domain.starts_with('.')
        || domain.ends_with('.')
    {
        return Err("Invalid email address");
    }
    Ok(e)
}

#[derive(Debug, thiserror::Error)]
pub enum CreateUserError {
    #[error("An account with this email already exists")]
    EmailTaken,
    #[error(transparent)]
    Db(#[from] sqlx::Error),
}

fn is_unique_violation(e: &sqlx::Error, constraint: &str) -> bool {
    matches!(e, sqlx::Error::Database(db) if db.code().as_deref() == Some("23505")
        && db.constraint().is_some_and(|c| c.contains(constraint)))
}

/// Creates an account.
pub async fn create_user(
    pool: &PgPool,
    email: &str,
    pw_hash: &str,
    display_name: &str,
) -> Result<UserRow, CreateUserError> {
    let row = sqlx::query_as::<_, UserRow>(&format!(
        "INSERT INTO users (uid, email, pw_hash, display_name)
         VALUES ($1, $2, $3, $4) RETURNING {USER_COLS}"
    ))
    .bind(Uuid::new_v4())
    .bind(email)
    .bind(pw_hash)
    .bind(display_name)
    .fetch_one(pool)
    .await
    .map_err(|e| if is_unique_violation(&e, "email") { CreateUserError::EmailTaken } else { e.into() })?;
    Ok(row)
}

pub async fn user_by_uid(pool: &PgPool, uid: Uuid) -> sqlx::Result<Option<UserRow>> {
    sqlx::query_as(&format!("SELECT {USER_COLS} FROM users WHERE uid = $1")).bind(uid).fetch_optional(pool).await
}

pub async fn user_by_email(pool: &PgPool, email: &str) -> sqlx::Result<Option<UserRow>> {
    sqlx::query_as(&format!("SELECT {USER_COLS} FROM users WHERE email = $1")).bind(email).fetch_optional(pool).await
}

/// Finds a user by uid, email or connect code (admin CLI).
pub async fn find_user(pool: &PgPool, ident: &str) -> sqlx::Result<Option<UserRow>> {
    if let Ok(uid) = Uuid::parse_str(ident) {
        return user_by_uid(pool, uid).await;
    }
    if ident.contains('@') {
        return match normalize_email(ident) {
            Ok(e) => user_by_email(pool, &e).await,
            Err(_) => Ok(None),
        };
    }
    match common::codes::parse_connect_code(ident) {
        Ok(code) => {
            sqlx::query_as(&format!("SELECT {USER_COLS} FROM users WHERE connect_code = $1"))
                .bind(code.to_string())
                .fetch_optional(pool)
                .await
        }
        Err(_) => Ok(None),
    }
}

pub async fn list_users(pool: &PgPool) -> sqlx::Result<Vec<UserRow>> {
    sqlx::query_as(&format!("SELECT {USER_COLS} FROM users ORDER BY created_at")).fetch_all(pool).await
}

// ---------------------------------------------------------------- sessions

pub async fn create_session(pool: &PgPool, uid: Uuid, days: i64, user_agent: Option<&str>) -> sqlx::Result<String> {
    let token = random_token();
    sqlx::query("INSERT INTO sessions (token_hash, uid, expires_at, user_agent) VALUES ($1, $2, $3, $4)")
        .bind(hash_token(&token))
        .bind(uid)
        .bind(Utc::now() + Duration::days(days))
        .bind(user_agent.map(|u| u.chars().take(200).collect::<String>()))
        .execute(pool)
        .await?;
    Ok(token)
}

/// Resolves a session token to its user and slides the expiry forward
/// (at most once a minute, to keep writes down).
pub async fn session_user(pool: &PgPool, token: &str, days: i64) -> sqlx::Result<Option<UserRow>> {
    let h = hash_token(token);
    let uid: Option<Uuid> = sqlx::query_scalar(
        "UPDATE sessions SET last_seen = now(), expires_at = now() + make_interval(days => $2)
          WHERE token_hash = $1 AND expires_at > now()
          RETURNING uid",
    )
    .bind(&h)
    .bind(days as i32)
    .fetch_optional(pool)
    .await?;
    match uid {
        Some(uid) => user_by_uid(pool, uid).await,
        None => Ok(None),
    }
}

pub async fn delete_session(pool: &PgPool, token: &str) -> sqlx::Result<()> {
    sqlx::query("DELETE FROM sessions WHERE token_hash = $1").bind(hash_token(token)).execute(pool).await?;
    Ok(())
}

pub async fn delete_sessions_for(pool: &PgPool, uid: Uuid) -> sqlx::Result<u64> {
    Ok(sqlx::query("DELETE FROM sessions WHERE uid = $1").bind(uid).execute(pool).await?.rows_affected())
}

// ---------------------------------------------------------------- one-time tokens

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TokenPurpose {
    VerifyEmail,
    ResetPassword,
}

impl TokenPurpose {
    fn as_str(self) -> &'static str {
        match self {
            TokenPurpose::VerifyEmail => "verify_email",
            TokenPurpose::ResetPassword => "reset_password",
        }
    }
}

/// Issues a one-time token. A new reset token invalidates the user's older unused
/// ones, so only the latest reset link works. Verification links all stay valid
/// until they expire: Gmail held some for longer than players waited before asking
/// for another (2026-10-14), and the late ones then said "invalid". Any of them
/// proves the same address (a new address voids them: [`change_unverified_email`]).
pub async fn create_email_token(
    pool: &PgPool,
    uid: Uuid,
    purpose: TokenPurpose,
    ttl: Duration,
) -> sqlx::Result<String> {
    let token = random_token();
    let mut tx = pool.begin().await?;
    if purpose == TokenPurpose::ResetPassword {
        sqlx::query("UPDATE email_tokens SET used_at = now() WHERE uid = $1 AND purpose = $2 AND used_at IS NULL")
            .bind(uid)
            .bind(purpose.as_str())
            .execute(&mut *tx)
            .await?;
    }
    sqlx::query("INSERT INTO email_tokens (token_hash, uid, purpose, expires_at) VALUES ($1, $2, $3, $4)")
        .bind(hash_token(&token))
        .bind(uid)
        .bind(purpose.as_str())
        .bind(Utc::now() + ttl)
        .execute(&mut *tx)
        .await?;
    tx.commit().await?;
    Ok(token)
}

/// Checks a token without using it (to render the reset form).
pub async fn peek_email_token(pool: &PgPool, token: &str, purpose: TokenPurpose) -> sqlx::Result<Option<Uuid>> {
    sqlx::query_scalar(
        "SELECT uid FROM email_tokens WHERE token_hash = $1 AND purpose = $2 AND used_at IS NULL AND expires_at > now()",
    )
    .bind(hash_token(token))
    .bind(purpose.as_str())
    .fetch_optional(pool)
    .await
}

async fn consume_email_token_tx(
    tx: &mut Transaction<'_, Postgres>,
    token: &str,
    purpose: TokenPurpose,
) -> sqlx::Result<Option<Uuid>> {
    sqlx::query_scalar(
        "UPDATE email_tokens SET used_at = now()
          WHERE token_hash = $1 AND purpose = $2 AND used_at IS NULL AND expires_at > now()
          RETURNING uid",
    )
    .bind(hash_token(token))
    .bind(purpose.as_str())
    .fetch_optional(&mut **tx)
    .await
}

/// Uses a verification token. Returns the verified uid.
pub async fn verify_email(pool: &PgPool, token: &str) -> sqlx::Result<Option<Uuid>> {
    let mut tx = pool.begin().await?;
    let Some(uid) = consume_email_token_tx(&mut tx, token, TokenPurpose::VerifyEmail).await? else {
        return Ok(None);
    };
    sqlx::query(
        "UPDATE users SET email_verified_at = COALESCE(email_verified_at, now()), updated_at = now() WHERE uid = $1",
    )
    .bind(uid)
    .execute(&mut *tx)
    .await?;
    tx.commit().await?;
    Ok(Some(uid))
}

/// Gives an account whose email is not verified a new email (a typo at sign-up), and
/// voids every link mailed to the old one: a verification or reset link would
/// otherwise verify the new address from the old one's inbox. `Ok(None)`: the email
/// is verified (also when that happened meanwhile). The tokens are locked before the
/// user, in [`verify_email`]'s order.
pub async fn change_unverified_email(
    pool: &PgPool,
    uid: Uuid,
    email: &str,
) -> Result<Option<UserRow>, CreateUserError> {
    let mut tx = pool.begin().await?;
    sqlx::query("UPDATE email_tokens SET used_at = now() WHERE uid = $1 AND used_at IS NULL")
        .bind(uid)
        .execute(&mut *tx)
        .await?;
    let row = sqlx::query_as::<_, UserRow>(&format!(
        "UPDATE users SET email = $2, updated_at = now() WHERE uid = $1 AND email_verified_at IS NULL
         RETURNING {USER_COLS}"
    ))
    .bind(uid)
    .bind(email)
    .fetch_optional(&mut *tx)
    .await
    .map_err(|e| if is_unique_violation(&e, "email") { CreateUserError::EmailTaken } else { e.into() })?;
    if row.is_some() {
        tx.commit().await?;
    }
    Ok(row)
}

/// Uses a reset token and sets the new password hash. Following a reset link
/// also proves the address, so it marks the email verified.
pub async fn reset_password_with_token(pool: &PgPool, token: &str, pw_hash: &str) -> sqlx::Result<Option<Uuid>> {
    let mut tx = pool.begin().await?;
    let Some(uid) = consume_email_token_tx(&mut tx, token, TokenPurpose::ResetPassword).await? else {
        return Ok(None);
    };
    set_password_tx(&mut tx, uid, pw_hash).await?;
    sqlx::query("UPDATE users SET email_verified_at = COALESCE(email_verified_at, now()) WHERE uid = $1")
        .bind(uid)
        .execute(&mut *tx)
        .await?;
    tx.commit().await?;
    Ok(Some(uid))
}

async fn set_password_tx(tx: &mut Transaction<'_, Postgres>, uid: Uuid, pw_hash: &str) -> sqlx::Result<()> {
    // A password change rotates the play key and ends every session (design 2.4).
    sqlx::query(
        "UPDATE users SET pw_hash = $2, play_key_version = play_key_version + 1, updated_at = now() WHERE uid = $1",
    )
    .bind(uid)
    .bind(pw_hash)
    .execute(&mut **tx)
    .await?;
    sqlx::query("DELETE FROM sessions WHERE uid = $1").bind(uid).execute(&mut **tx).await?;
    Ok(())
}

pub async fn set_password(pool: &PgPool, uid: Uuid, pw_hash: &str) -> sqlx::Result<()> {
    let mut tx = pool.begin().await?;
    set_password_tx(&mut tx, uid, pw_hash).await?;
    tx.commit().await
}

// ---------------------------------------------------------------- profile

#[derive(Debug, thiserror::Error)]
pub enum AssignCodeError {
    #[error("This account already has a connect code")]
    AlreadyAssigned,
    #[error("No codes are left with this prefix. Pick another one.")]
    Exhausted,
    #[error(transparent)]
    Db(#[from] sqlx::Error),
}

/// Assigns `PREFIX#N` (design 1.3): the lowest free number from a random start
/// in 1..=999, then 1000..=9999 if the prefix leaves room. Retries if another
/// sign-up takes the same code concurrently.
pub async fn assign_connect_code(pool: &PgPool, uid: Uuid, prefix: &str) -> Result<String, AssignCodeError> {
    assign_code_where(pool, uid, prefix, None).await
}

/// Gives an account that still has `current` a new code with `prefix` (the slur filter's
/// replacement, `crate::slur_filter`). [`AssignCodeError::AlreadyAssigned`] if its code is no
/// longer `current`.
pub async fn reassign_connect_code(
    pool: &PgPool,
    uid: Uuid,
    current: &str,
    prefix: &str,
) -> Result<String, AssignCodeError> {
    assign_code_where(pool, uid, prefix, Some(current)).await
}

/// [`assign_connect_code`] when the account's code is `current` (NULL: none yet).
async fn assign_code_where(
    pool: &PgPool,
    uid: Uuid,
    prefix: &str,
    current: Option<&str>,
) -> Result<String, AssignCodeError> {
    for _attempt in 0..8 {
        let taken: Vec<String> = sqlx::query_scalar("SELECT connect_code FROM users WHERE connect_code LIKE $1")
            .bind(format!("{prefix}#%"))
            .fetch_all(pool)
            .await?;
        let taken: std::collections::HashSet<u16> =
            taken.iter().filter_map(|c| c.split_once('#').and_then(|(_, n)| n.parse().ok())).collect();
        let start = rand::thread_rng().gen_range(1..=999u16);
        let Some(number) = candidate_numbers(prefix.len(), start).find(|n| !taken.contains(n)) else {
            return Err(AssignCodeError::Exhausted);
        };
        let code = ConnectCode::new(prefix, number).map_err(|_| AssignCodeError::Exhausted)?.to_string();
        let res = sqlx::query_scalar::<_, String>(
            "UPDATE users SET connect_code = $2, updated_at = now()
              WHERE uid = $1 AND connect_code IS NOT DISTINCT FROM $3 RETURNING connect_code",
        )
        .bind(uid)
        .bind(&code)
        .bind(current)
        .fetch_optional(pool)
        .await;
        match res {
            Ok(Some(c)) => return Ok(c),
            Ok(None) => return Err(AssignCodeError::AlreadyAssigned),
            Err(e) if is_unique_violation(&e, "connect_code") => continue,
            Err(e) => return Err(e.into()),
        }
    }
    Err(AssignCodeError::Exhausted)
}

/// Admin-only: sets a specific code (the "one admin-approved change").
pub async fn admin_set_connect_code(pool: &PgPool, uid: Uuid, code: &str) -> Result<(), sqlx::Error> {
    sqlx::query("UPDATE users SET connect_code = $2, updated_at = now() WHERE uid = $1")
        .bind(uid)
        .bind(code)
        .execute(pool)
        .await
        .map(|_| ())
}

pub async fn rename(pool: &PgPool, uid: Uuid, name: &str) -> sqlx::Result<()> {
    sqlx::query("UPDATE users SET display_name = $2, updated_at = now() WHERE uid = $1")
        .bind(uid)
        .bind(name)
        .execute(pool)
        .await
        .map(|_| ())
}

/// Renames the account if its name is still `from` (the slur filter's late replacement, which
/// must not undo a rename made since). Whether it did.
pub async fn rename_if(pool: &PgPool, uid: Uuid, from: &str, to: &str) -> sqlx::Result<bool> {
    sqlx::query("UPDATE users SET display_name = $3, updated_at = now() WHERE uid = $1 AND display_name = $2")
        .bind(uid)
        .bind(from)
        .bind(to)
        .execute(pool)
        .await
        .map(|r| r.rows_affected() == 1)
}

/// The column of the account's name for that kind of room.
fn room_name_column(kind: RoomKind) -> &'static str {
    match kind {
        RoomKind::Pplus => "room_name",
        RoomKind::Mods => "mods_room_name",
    }
}

/// Sets the name the account gives its rooms of that kind (`None`: no name). mm sees the change
/// (`updated_at`) and renames the room the account hosts.
pub async fn set_room_name(pool: &PgPool, uid: Uuid, kind: RoomKind, name: Option<&str>) -> sqlx::Result<()> {
    let col = room_name_column(kind);
    sqlx::query(&format!("UPDATE users SET {col} = $2, updated_at = now() WHERE uid = $1"))
        .bind(uid)
        .bind(name)
        .execute(pool)
        .await
        .map(|_| ())
}

/// [`set_room_name`] if the name is still `from` (the slur filter's late replacement). Whether it
/// did.
pub async fn set_room_name_if(pool: &PgPool, uid: Uuid, kind: RoomKind, from: &str, to: &str) -> sqlx::Result<bool> {
    let col = room_name_column(kind);
    sqlx::query(&format!("UPDATE users SET {col} = $3, updated_at = now() WHERE uid = $1 AND {col} = $2"))
        .bind(uid)
        .bind(from)
        .bind(to)
        .execute(pool)
        .await
        .map(|r| r.rows_affected() == 1)
}

/// Locks (`pp-admin user lock-name`) or unlocks the account's display name.
pub async fn set_rename_locked(pool: &PgPool, uid: Uuid, locked: bool) -> sqlx::Result<()> {
    sqlx::query(
        "UPDATE users SET rename_locked_at = CASE WHEN $2 THEN COALESCE(rename_locked_at, now()) END WHERE uid = $1",
    )
    .bind(uid)
    .bind(locked)
    .execute(pool)
    .await
    .map(|_| ())
}

pub async fn accept_rules(pool: &PgPool, uid: Uuid, version: i32) -> sqlx::Result<()> {
    sqlx::query("UPDATE users SET rules_version = GREATEST(rules_version, $2), updated_at = now() WHERE uid = $1")
        .bind(uid)
        .bind(version)
        .execute(pool)
        .await
        .map(|_| ())
}

// ---------------------------------------------------------------- admin

/// Deletes an account. Its sessions and email tokens go with it (`ON DELETE CASCADE`);
/// `mm_matches` rows keep the uid. The connect code is free again at once.
pub async fn delete_user(pool: &PgPool, uid: Uuid) -> sqlx::Result<bool> {
    Ok(sqlx::query("DELETE FROM users WHERE uid = $1").bind(uid).execute(pool).await?.rows_affected() == 1)
}

/// Bans until `until` (or lifts the ban with `None`). A ban also ends all
/// sessions and rotates the play key, so an existing user.json stops working.
pub async fn set_ban(pool: &PgPool, uid: Uuid, until: Option<DateTime<Utc>>, reason: Option<&str>) -> sqlx::Result<()> {
    let mut tx = pool.begin().await?;
    if until.is_some() {
        sqlx::query(
            "UPDATE users SET banned_until = $2, ban_reason = $3, play_key_version = play_key_version + 1, updated_at = now() WHERE uid = $1",
        )
        .bind(uid)
        .bind(until)
        .bind(reason)
        .execute(&mut *tx)
        .await?;
        sqlx::query("DELETE FROM sessions WHERE uid = $1").bind(uid).execute(&mut *tx).await?;
    } else {
        sqlx::query("UPDATE users SET banned_until = NULL, ban_reason = NULL, updated_at = now() WHERE uid = $1")
            .bind(uid)
            .execute(&mut *tx)
            .await?;
    }
    tx.commit().await
}

/// Mutes (a shadowban, `pp-admin mute`) or unmutes the account's chat. mm and the social hub see
/// it with their next look at the changed accounts (`common::db::changed_users`).
pub async fn set_muted(pool: &PgPool, uid: Uuid, muted: bool) -> sqlx::Result<()> {
    sqlx::query(
        "UPDATE users SET chat_muted_at = CASE WHEN $2 THEN COALESCE(chat_muted_at, now()) END, updated_at = now()
          WHERE uid = $1",
    )
    .bind(uid)
    .bind(muted)
    .execute(pool)
    .await
    .map(|_| ())
}

pub async fn rotate_play_key(pool: &PgPool, uid: Uuid) -> sqlx::Result<()> {
    sqlx::query("UPDATE users SET play_key_version = play_key_version + 1, updated_at = now() WHERE uid = $1")
        .bind(uid)
        .execute(pool)
        .await
        .map(|_| ())
}

pub async fn audit(
    pool: &PgPool,
    actor: &str,
    action: &str,
    target: Option<&str>,
    detail: serde_json::Value,
) -> sqlx::Result<()> {
    sqlx::query("INSERT INTO audit_log (actor, action, target, detail) VALUES ($1, $2, $3, $4)")
        .bind(actor)
        .bind(action)
        .bind(target)
        .bind(sqlx::types::Json(detail))
        .execute(pool)
        .await
        .map(|_| ())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn email_normalization() {
        assert_eq!(normalize_email("  Sarah@Example.COM ").unwrap(), "sarah@example.com");
        for bad in ["", "a", "a@", "@b.c", "a@b", "a@@b.c", "a b@c.d", "a@.b", "a@b.", "a@b@c.de"] {
            assert!(normalize_email(bad).is_err(), "{bad}");
        }
    }
}
