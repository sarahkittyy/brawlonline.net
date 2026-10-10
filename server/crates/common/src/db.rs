//! Database access shared between services: the embedded migrations, the read the mm server
//! does to validate a ticket, and what mm writes (matches, chat identity keys, chat reports).

use chrono::{DateTime, Utc};
use uuid::Uuid;

/// All migrations in `server/migrations`, embedded at build time.
pub static MIGRATOR: sqlx::migrate::Migrator = sqlx::migrate!("../../migrations");

/// What the mm server needs to know about a ticket's account.
#[derive(Debug, Clone, PartialEq, sqlx::FromRow)]
pub struct MmUser {
    pub uid: Uuid,
    pub display_name: String,
    pub connect_code: Option<String>,
    pub play_key_version: i32,
    pub banned_until: Option<DateTime<Utc>>,
    pub email_verified: bool,
    /// Elo rating ([`crate::ranked::DEFAULT_RATING`] before the first ranked set).
    pub rating: f64,
    /// Rated ranked sets played.
    pub ranked_sets: i32,
}

impl MmUser {
    pub fn is_banned(&self, now: DateTime<Utc>) -> bool {
        self.banned_until.is_some_and(|t| t > now)
    }
}

pub async fn fetch_mm_user(pool: &sqlx::PgPool, uid: Uuid) -> sqlx::Result<Option<MmUser>> {
    sqlx::query_as::<_, MmUser>(
        "SELECT u.uid, u.display_name, u.connect_code, u.play_key_version, u.banned_until,
                u.email_verified_at IS NOT NULL AS email_verified,
                COALESCE(r.rating, $2) AS rating, COALESCE(r.sets_played, 0) AS ranked_sets
           FROM users u LEFT JOIN ratings r ON r.uid = u.uid
          WHERE u.uid = $1",
    )
    .bind(uid)
    .bind(crate::ranked::DEFAULT_RATING)
    .fetch_optional(pool)
    .await
}

/// Records a pairing (best effort; mm does not wait for it).
#[allow(clippy::too_many_arguments)]
pub async fn insert_match(
    pool: &sqlx::PgPool,
    match_id: &str,
    mode: i16,
    players: &[Uuid],
    is_host: Uuid,
    stages: &[i16],
    region: &str,
) -> sqlx::Result<()> {
    sqlx::query(
        "INSERT INTO mm_matches (match_id, mode, players, is_host, stages, region) VALUES ($1, $2, $3, $4, $5, $6)
         ON CONFLICT (match_id) DO NOTHING",
    )
    .bind(match_id)
    .bind(mode)
    .bind(players)
    .bind(is_host)
    .bind(stages)
    .bind(region)
    .execute(pool)
    .await
    .map(|_| ())
}

/// Identity keys kept per account: older ones are deleted when a new one is recorded.
pub const CHAT_KEYS_KEPT: i64 = 20;

/// Records that `uid` used the chat identity key `key` (`hello.chatKey`): a new key is added, a
/// known one gets a new `last_seen`, and only the [`CHAT_KEYS_KEPT`] most recent stay.
pub async fn record_chat_key(pool: &sqlx::PgPool, uid: Uuid, key: &[u8; 32]) -> sqlx::Result<()> {
    sqlx::query(
        "INSERT INTO chat_identity_keys (uid, public_key) VALUES ($1, $2)
         ON CONFLICT (uid, public_key) DO UPDATE SET last_seen = now()",
    )
    .bind(uid)
    .bind(&key[..])
    .execute(pool)
    .await?;
    sqlx::query(
        "DELETE FROM chat_identity_keys WHERE uid = $1 AND public_key NOT IN (
             SELECT public_key FROM chat_identity_keys WHERE uid = $1
              ORDER BY last_seen DESC, first_seen DESC LIMIT $2)",
    )
    .bind(uid)
    .bind(CHAT_KEYS_KEPT)
    .execute(pool)
    .await
    .map(|_| ())
}

/// The chat identity keys recorded for `uid`, most recent first.
pub async fn fetch_chat_keys(pool: &sqlx::PgPool, uid: Uuid) -> sqlx::Result<Vec<[u8; 32]>> {
    let rows: Vec<Vec<u8>> =
        sqlx::query_scalar("SELECT public_key FROM chat_identity_keys WHERE uid = $1 ORDER BY last_seen DESC")
            .bind(uid)
            .fetch_all(pool)
            .await?;
    Ok(rows.into_iter().filter_map(|k| k.try_into().ok()).collect())
}

/// A `chat-report` as mm stores it.
#[derive(Debug, Clone, PartialEq)]
pub struct NewChatReport {
    pub reporter: Uuid,
    pub reported: Uuid,
    pub group_id: String,
    /// Cleaned (`chat::clean_text`).
    pub reason: String,
    /// `[{seq, text, raw, sig, verified}]` (`migrations/0006_chat.sql`).
    pub messages: serde_json::Value,
    pub verified: i32,
    pub total: i32,
}

/// Stores a report. A reporter or reported uid that is no account (any more) is stored as NULL
/// (the uid of a report comes from a peer).
pub async fn insert_chat_report(pool: &sqlx::PgPool, r: &NewChatReport) -> sqlx::Result<()> {
    sqlx::query(
        "INSERT INTO chat_reports (reporter, reported, group_id, reason, messages, verified, total)
         VALUES ((SELECT uid FROM users WHERE uid = $1), (SELECT uid FROM users WHERE uid = $2), $3, $4, $5, $6, $7)",
    )
    .bind(r.reporter)
    .bind(r.reported)
    .bind(&r.group_id)
    .bind(&r.reason)
    .bind(&r.messages)
    .bind(r.verified)
    .bind(r.total)
    .execute(pool)
    .await
    .map(|_| ())
}
