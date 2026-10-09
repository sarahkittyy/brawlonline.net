//! Database access shared between services: the embedded migrations and the
//! read the mm server does to validate a ticket.

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
