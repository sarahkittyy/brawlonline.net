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
    /// Chat shadowban (`pp-admin mute`): mm drops the account's chat messages without a word.
    pub chat_muted: bool,
}

impl MmUser {
    pub fn is_banned(&self, now: DateTime<Utc>) -> bool {
        self.banned_until.is_some_and(|t| t > now)
    }
}

const MM_USER_SELECT: &str = "SELECT u.uid, u.display_name, u.connect_code, u.play_key_version, u.banned_until,
        u.email_verified_at IS NOT NULL AS email_verified,
        COALESCE(r.rating, $2) AS rating, COALESCE(r.sets_played, 0) AS ranked_sets,
        u.chat_muted_at IS NOT NULL AS chat_muted
   FROM users u LEFT JOIN ratings r ON r.uid = u.uid";

pub async fn fetch_mm_user(pool: &sqlx::PgPool, uid: Uuid) -> sqlx::Result<Option<MmUser>> {
    sqlx::query_as::<_, MmUser>(&format!("{MM_USER_SELECT} WHERE u.uid = $1"))
        .bind(uid)
        .bind(crate::ranked::DEFAULT_RATING)
        .fetch_optional(pool)
        .await
}

/// How often mm and accounts look for changed accounts ([`changed_users`]).
pub const ACCOUNT_POLL: std::time::Duration = std::time::Duration::from_secs(3);
/// How far back each look reaches before the previous one's time: a change whose transaction
/// began before that look but committed after it still has an older `updated_at`. Changes are
/// seen more than once; the services act only on what differs from what they hold.
pub const ACCOUNT_POLL_OVERLAP: chrono::Duration = chrono::Duration::seconds(30);

/// The accounts whose row changed (`users.updated_at`) after `since`, and the database's time
/// now (the next look's `since`, less [`ACCOUNT_POLL_OVERLAP`]). A ban, a rename or a mute is
/// carried to connected players this way (`pp-admin` writes only the database).
pub async fn changed_users(pool: &sqlx::PgPool, since: DateTime<Utc>) -> sqlx::Result<(Vec<MmUser>, DateTime<Utc>)> {
    let now: DateTime<Utc> = sqlx::query_scalar("SELECT now()").fetch_one(pool).await?;
    let users = sqlx::query_as::<_, MmUser>(&format!("{MM_USER_SELECT} WHERE u.updated_at > $1"))
        .bind(since)
        .bind(crate::ranked::DEFAULT_RATING)
        .fetch_all(pool)
        .await?;
    Ok((users, now))
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
    /// The published ruleset it played (`migrations/0009_match_rulesets.sql`); `None` for P+'s
    /// default and custom rules.
    pub ruleset: Option<MatchRuleset>,
}

/// A published ruleset as a room's match records it.
#[derive(Debug, Clone, PartialEq)]
pub struct MatchRuleset {
    pub code: String,
    pub name: String,
    pub version: i32,
    /// `Ruleset::hash`.
    pub hash: String,
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
        "INSERT INTO mm_matches (match_id, mode, players, is_host, stages, region, room_code, room_mode, ports, teams,
                                 ruleset_code, ruleset_name, ruleset_version, ruleset_hash)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)
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
    .bind(room.and_then(|r| r.ruleset.as_ref()).map(|s| s.code.as_str()))
    .bind(room.and_then(|r| r.ruleset.as_ref()).map(|s| s.name.as_str()))
    .bind(room.and_then(|r| r.ruleset.as_ref()).map(|s| s.version))
    .bind(room.and_then(|r| r.ruleset.as_ref()).map(|s| s.hash.as_str()))
    .execute(pool)
    .await
    .map(|_| ())
}

/// A match mm made, as mm reads it back when it starts ([`recent_matches`]).
#[derive(Debug, Clone, PartialEq, sqlx::FromRow)]
pub struct RecentMatch {
    pub match_id: String,
    /// `Mode::as_u8`.
    pub mode: i16,
    pub players: Vec<Uuid>,
    pub is_host: Uuid,
    /// A room's game: the room.
    pub room_code: Option<String>,
    /// How long ago it was made, by the database's clock.
    pub age_ms: i64,
}

/// The matches made in the last `within` (oldest first): mm keeps its matches in memory, so after
/// a restart it reads back the ones whose sets may still be going on (`migrations/0020`).
pub async fn recent_matches(pool: &sqlx::PgPool, within: std::time::Duration) -> sqlx::Result<Vec<RecentMatch>> {
    sqlx::query_as(
        "SELECT match_id, mode, players, is_host, room_code,
                (extract(epoch FROM now() - created_at) * 1000)::bigint AS age_ms
           FROM mm_matches WHERE created_at > now() - make_interval(secs => $1)
          ORDER BY created_at",
    )
    .bind(within.as_secs_f64())
    .fetch_all(pool)
    .await
}

/// Identity keys kept per account: older ones are deleted when a new one is recorded.
pub const CHAT_KEYS_KEPT: i64 = 20;

/// Records that `uid` used the chat identity key `key` (`hello.chatKey`): a new key is added, a
/// known one gets a new `last_seen`, and only the [`CHAT_KEYS_KEPT`] most recent stay.
/// Whether a Ranked set's player's game is connected to mm (`migrations/0016_ranked_seen.sql`) and,
/// if it stopped running while connected, since when (`stalled_since`, 0021): an older `at` than
/// the stored one changes nothing.
pub async fn record_ranked_presence(
    pool: &sqlx::PgPool,
    match_id: &str,
    uid: Uuid,
    online: bool,
    stalled_since: Option<DateTime<Utc>>,
    at: DateTime<Utc>,
) -> sqlx::Result<()> {
    sqlx::query(
        "INSERT INTO ranked_presence (match_id, uid, online, stalled_since, changed_at) VALUES ($1, $2, $3, $4, $5)
         ON CONFLICT (match_id, uid) DO UPDATE
           SET online = EXCLUDED.online, stalled_since = EXCLUDED.stalled_since, changed_at = EXCLUDED.changed_at
          WHERE ranked_presence.changed_at < EXCLUDED.changed_at",
    )
    .bind(match_id)
    .bind(uid)
    .bind(online)
    .bind(stalled_since)
    .bind(at)
    .execute(pool)
    .await?;
    Ok(())
}

/// How a Ranked set's game ended as one player's own game said it (`ranked_game_results`): a later
/// word on the same game replaces it.
pub async fn record_ranked_game_result(
    pool: &sqlx::PgPool,
    match_id: &str,
    game_index: u32,
    uid: Uuid,
    winner: Option<Uuid>,
    end_method: &str,
) -> sqlx::Result<()> {
    sqlx::query(
        "INSERT INTO ranked_game_results (match_id, game_index, uid, winner, end_method, created_at)
         VALUES ($1, $2, $3, $4, $5, now())
         ON CONFLICT (match_id, game_index, uid)
         DO UPDATE SET winner = EXCLUDED.winner, end_method = EXCLUDED.end_method, created_at = EXCLUDED.created_at",
    )
    .bind(match_id)
    .bind(game_index as i32)
    .bind(uid)
    .bind(winner)
    .bind(end_method)
    .execute(pool)
    .await?;
    Ok(())
}

/// One player's game's word on its speed over an online game (`speed_reports`,
/// `crate::speed`). A second word on the same game from the same player is ignored.
#[derive(Debug, Clone, PartialEq)]
pub struct SpeedReportRecord {
    pub uid: Uuid,
    pub match_id: String,
    /// The match's mode (`ranked`, `unranked`, `direct`, ...), `room` for a room's game.
    pub mode: String,
    pub game_uid: String,
    pub set_game: Option<u32>,
    pub end_method: String,
    pub perf: crate::speed::GamePerf,
    pub app_version: String,
    pub platform: String,
}

pub async fn record_speed_report(pool: &sqlx::PgPool, r: &SpeedReportRecord) -> sqlx::Result<()> {
    let d = r.perf.delay.as_ref();
    sqlx::query(
        "INSERT INTO speed_reports (uid, match_id, mode, game_uid, set_game, end_method, frames, fps, own_late,
                                    followed, peer_fps, slow, peer_slow, app_version, platform,
                                    delay, delay_profile, delay_asked, delay_fixed, rtt_start, ping, jitter,
                                    rollbacks, depth, rollback_ms, late_after_rollback, hitches, waits, wait_ms,
                                    resim_ms)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15,
                 $16, $17, $18, $19, $20, $21, $22, $23, $24, $25, $26, $27, $28, $29, $30)
         ON CONFLICT (game_uid, uid) DO NOTHING",
    )
    .bind(r.uid)
    .bind(&r.match_id)
    .bind(&r.mode)
    .bind(&r.game_uid)
    .bind(r.set_game.map(|g| g as i32))
    .bind(&r.end_method)
    .bind(r.perf.frames as i32)
    .bind(r.perf.fps as f32)
    .bind(r.perf.own_late as f32)
    .bind(r.perf.followed as f32)
    .bind(r.perf.peer_fps.map(|f| f as f32))
    .bind(r.perf.slow)
    .bind(r.perf.peer_slow)
    .bind(&r.app_version)
    .bind(&r.platform)
    .bind(d.map(|d| d.delay as i16))
    .bind(d.map(|d| d.delay_profile.as_str()))
    .bind(d.map(|d| d.delay_asked.as_str()))
    .bind(d.map(|d| d.delay_fixed))
    .bind(d.map(|d| d.rtt_start as f32))
    .bind(d.map(|d| d.ping as f32))
    .bind(d.map(|d| d.jitter as f32))
    .bind(d.map(|d| d.rollbacks as f32))
    .bind(d.map(|d| d.depth as f32))
    .bind(d.map(|d| d.rollback_ms as f32))
    .bind(d.map(|d| d.late_after_rollback as f32))
    .bind(d.map(|d| d.hitches as f32))
    .bind(d.map(|d| d.waits as i32))
    .bind(d.map(|d| d.wait_ms as f32))
    .bind(d.map(|d| d.resim_ms as f32))
    .execute(pool)
    .await?;
    Ok(())
}

/// A waiting-room speed check (`speed_checks`, `crate::speed`).
#[derive(Debug, Clone, PartialEq)]
pub struct SpeedCheckRecord {
    pub uid: Uuid,
    pub check: crate::speed::SpeedCheck,
    pub app_version: String,
    pub platform: String,
}

pub async fn record_speed_check(pool: &sqlx::PgPool, r: &SpeedCheckRecord) -> sqlx::Result<()> {
    let windows: Vec<f32> = r.check.windows.iter().map(|w| *w as f32).collect();
    sqlx::query(
        "INSERT INTO speed_checks (uid, mode, windows, gaps, backend, app_version, platform)
         VALUES ($1, $2, $3, $4, $5, $6, $7)",
    )
    .bind(r.uid)
    .bind(&r.check.mode)
    .bind(&windows)
    .bind(r.check.gaps as i32)
    .bind(&r.check.backend)
    .bind(&r.app_version)
    .bind(&r.platform)
    .execute(pool)
    .await?;
    Ok(())
}

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
    /// Games played with this version (`ruleset_plays`).
    pub games: i64,
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
    games: i64,
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
            games: self.games,
        })
    }
}

/// The columns of [`RulesetRow`], for `FROM rulesets r LEFT JOIN users u ON u.uid = r.owner`.
pub const RULESET_COLS: &str = "r.code, r.owner, r.lineage, r.version, \
     COALESCE(u.display_name, r.publisher_name) AS by, r.json, r.hash, r.hidden, r.created_at, \
     COALESCE((SELECT p.games FROM ruleset_plays p WHERE p.code = r.code), 0) AS games";

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

/// Escapes `%`, `_` and `\` for a `LIKE` pattern (with the default `\` escape).
fn like_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        if matches!(c, '%' | '_' | '\\') {
            out.push('\\');
        }
        out.push(c);
    }
    out
}

/// The public list of rulesets (`GET /v1/rulesets`): of every ruleset (lineage) its newest version
/// that is not hidden, the most played first (that version's own games), then newest (`created_at`,
/// then `code`, both descending). `query` matches the name, the publisher's name and the code,
/// case-insensitively, anywhere in them. `after` is the last row of the previous page (its games,
/// `created_at` and code); at most `limit` rows.
pub async fn browse_rulesets(
    pool: &sqlx::PgPool,
    query: Option<&str>,
    after: Option<(i64, DateTime<Utc>, &str)>,
    limit: i64,
) -> sqlx::Result<Vec<PublishedRuleset>> {
    let pattern = query.map(|q| format!("%{}%", like_escape(q)));
    let rows: Vec<RulesetRow> = sqlx::query_as(&format!(
        "WITH latest AS (
             SELECT DISTINCT ON (lineage) code FROM rulesets WHERE NOT hidden ORDER BY lineage, version DESC),
         listed AS (
             SELECT {RULESET_COLS} FROM latest l
               JOIN rulesets r ON r.code = l.code LEFT JOIN users u ON u.uid = r.owner
              WHERE ($1::text IS NULL OR r.name ILIKE $1 OR COALESCE(u.display_name, r.publisher_name) ILIKE $1
                     OR r.code ILIKE $1))
         SELECT * FROM listed
          WHERE $2::bigint IS NULL OR (games, created_at, code) < ($2, $3, $4)
          ORDER BY games DESC, created_at DESC, code DESC
          LIMIT $5"
    ))
    .bind(pattern)
    .bind(after.map(|a| a.0))
    .bind(after.map(|a| a.1))
    .bind(after.map(|a| a.2.to_string()).unwrap_or_default())
    .bind(limit)
    .fetch_all(pool)
    .await?;
    rows.into_iter().map(RulesetRow::into_published).collect()
}

/// One more game played with the published ruleset `code` (mm, best effort): a version deleted
/// since (pp-admin) has no row and counts nothing.
pub async fn count_ruleset_game(pool: &sqlx::PgPool, code: &str) -> sqlx::Result<()> {
    sqlx::query("UPDATE ruleset_plays SET games = games + 1 WHERE code = $1").bind(code).execute(pool).await.map(|_| ())
}
