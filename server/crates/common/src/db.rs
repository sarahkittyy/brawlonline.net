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

/// A room's game (`migrations/0007_room_matches.sql`): the room and, in the order of the
/// match's players, each one's slot and team.
#[derive(Debug, Clone, PartialEq)]
pub struct MatchRoom {
    pub code: String,
    /// `1v1`, `ffa` or `teams`.
    pub mode: String,
    /// 1-4.
    pub ports: Vec<i16>,
    /// 0 red, 1 blue, 2 green; `None` unless the room played a team battle.
    pub teams: Option<Vec<i16>>,
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
    room: Option<&MatchRoom>,
) -> sqlx::Result<()> {
    sqlx::query(
        "INSERT INTO mm_matches (match_id, mode, players, is_host, stages, region, room_code, room_mode, ports, teams)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)
         ON CONFLICT (match_id) DO NOTHING",
    )
    .bind(match_id)
    .bind(mode)
    .bind(players)
    .bind(is_host)
    .bind(stages)
    .bind(region)
    .bind(room.map(|r| r.code.as_str()))
    .bind(room.map(|r| r.mode.as_str()))
    .bind(room.map(|r| r.ports.as_slice()))
    .bind(room.and_then(|r| r.teams.as_deref()))
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

// ---------------------------------------------------------------- rulesets

/// Rulesets kept on an account's recent list (`ruleset_recent`, `migrations/0008_rulesets.sql`).
pub const RECENT_RULESETS_KEPT: i64 = 10;

/// A published ruleset (`rulesets`): the ruleset with `version` and `by` filled (`by` is the
/// publisher's current display name, or the name they had when publishing once the account is
/// gone).
#[derive(Debug, Clone, PartialEq)]
pub struct PublishedRuleset {
    pub code: String,
    pub owner: Option<Uuid>,
    pub lineage: Uuid,
    pub ruleset: crate::rules::Ruleset,
    pub hash: String,
    pub hidden: bool,
    pub created_at: DateTime<Utc>,
}

#[derive(sqlx::FromRow)]
struct RulesetRow {
    code: String,
    owner: Option<Uuid>,
    lineage: Uuid,
    version: i32,
    by: String,
    json: String,
    hash: String,
    hidden: bool,
    created_at: DateTime<Utc>,
}

impl RulesetRow {
    fn into_published(self) -> sqlx::Result<PublishedRuleset> {
        let mut ruleset: crate::rules::Ruleset =
            serde_json::from_str(&self.json).map_err(|e| sqlx::Error::Decode(Box::new(e)))?;
        ruleset.version = self.version as u32;
        ruleset.by = self.by;
        Ok(PublishedRuleset {
            code: self.code,
            owner: self.owner,
            lineage: self.lineage,
            ruleset,
            hash: self.hash,
            hidden: self.hidden,
            created_at: self.created_at,
        })
    }
}

/// The columns of [`RulesetRow`], for `FROM rulesets r LEFT JOIN users u ON u.uid = r.owner`.
pub const RULESET_COLS: &str = "r.code, r.owner, r.lineage, r.version, \
     COALESCE(u.display_name, r.publisher_name) AS by, r.json, r.hash, r.hidden, r.created_at";

/// A published ruleset by its code (already normalized: [`crate::rules::parse_ruleset_code`]).
pub async fn fetch_ruleset(pool: &sqlx::PgPool, code: &str) -> sqlx::Result<Option<PublishedRuleset>> {
    let row: Option<RulesetRow> = sqlx::query_as(&format!(
        "SELECT {RULESET_COLS} FROM rulesets r LEFT JOIN users u ON u.uid = r.owner WHERE r.code = $1"
    ))
    .bind(code)
    .fetch_optional(pool)
    .await?;
    row.map(RulesetRow::into_published).transpose()
}

/// Rows of a `SELECT {RULESET_COLS} FROM rulesets r LEFT JOIN users u ...` query.
pub async fn fetch_rulesets_where(pool: &sqlx::PgPool, tail: &str, uid: Uuid) -> sqlx::Result<Vec<PublishedRuleset>> {
    let rows: Vec<RulesetRow> =
        sqlx::query_as(&format!("SELECT {RULESET_COLS} FROM rulesets r LEFT JOIN users u ON u.uid = r.owner {tail}"))
            .bind(uid)
            .fetch_all(pool)
            .await?;
    rows.into_iter().map(RulesetRow::into_published).collect()
}

/// The newest version of `lineage` after `version` that is not hidden: (code, version).
pub async fn newer_ruleset_version(
    pool: &sqlx::PgPool,
    lineage: Uuid,
    version: u32,
) -> sqlx::Result<Option<(String, i32)>> {
    sqlx::query_as(
        "SELECT code, version FROM rulesets WHERE lineage = $1 AND version > $2 AND NOT hidden
          ORDER BY version DESC LIMIT 1",
    )
    .bind(lineage)
    .bind(version as i32)
    .fetch_optional(pool)
    .await
}

/// The account's remembered ruleset (decision 11), `None` for P+ default.
pub async fn remembered_ruleset(pool: &sqlx::PgPool, uid: Uuid) -> sqlx::Result<Option<PublishedRuleset>> {
    let code: Option<String> = sqlx::query_scalar("SELECT code FROM remembered_rulesets WHERE uid = $1")
        .bind(uid)
        .fetch_optional(pool)
        .await?;
    match code {
        Some(code) => fetch_ruleset(pool, &code).await,
        None => Ok(None),
    }
}

/// Sets the remembered ruleset (`None`: P+ default). A code also goes to the top of the recent
/// list, which keeps the newest [`RECENT_RULESETS_KEPT`]. The code must exist (foreign key).
pub async fn set_remembered_ruleset(pool: &sqlx::PgPool, uid: Uuid, code: Option<&str>) -> sqlx::Result<()> {
    let mut tx = pool.begin().await?;
    match code {
        None => {
            sqlx::query("DELETE FROM remembered_rulesets WHERE uid = $1").bind(uid).execute(&mut *tx).await?;
        }
        Some(code) => {
            sqlx::query(
                "INSERT INTO remembered_rulesets (uid, code) VALUES ($1, $2)
                 ON CONFLICT (uid) DO UPDATE SET code = EXCLUDED.code, updated_at = now()",
            )
            .bind(uid)
            .bind(code)
            .execute(&mut *tx)
            .await?;
            sqlx::query(
                "INSERT INTO ruleset_recent (uid, code) VALUES ($1, $2)
                 ON CONFLICT (uid, code) DO UPDATE SET used_at = clock_timestamp()",
            )
            .bind(uid)
            .bind(code)
            .execute(&mut *tx)
            .await?;
            sqlx::query(
                "DELETE FROM ruleset_recent WHERE uid = $1 AND code NOT IN (
                     SELECT code FROM ruleset_recent WHERE uid = $1 ORDER BY used_at DESC LIMIT $2)",
            )
            .bind(uid)
            .bind(RECENT_RULESETS_KEPT)
            .execute(&mut *tx)
            .await?;
        }
    }
    tx.commit().await
}

/// The account's recently used rulesets, newest first, with when each was last used.
pub async fn recent_rulesets(pool: &sqlx::PgPool, uid: Uuid) -> sqlx::Result<Vec<(PublishedRuleset, DateTime<Utc>)>> {
    #[derive(sqlx::FromRow)]
    struct Row {
        #[sqlx(flatten)]
        r: RulesetRow,
        used_at: DateTime<Utc>,
    }
    let rows: Vec<Row> = sqlx::query_as(&format!(
        "SELECT {RULESET_COLS}, x.used_at FROM ruleset_recent x
           JOIN rulesets r ON r.code = x.code LEFT JOIN users u ON u.uid = r.owner
          WHERE x.uid = $1 ORDER BY x.used_at DESC"
    ))
    .bind(uid)
    .fetch_all(pool)
    .await?;
    rows.into_iter().map(|row| Ok((row.r.into_published()?, row.used_at))).collect()
}
