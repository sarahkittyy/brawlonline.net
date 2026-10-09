//! The ranked leaderboard (`GET /v1/ranked/leaderboard`) and a player's position on it
//! (`rank.position` of `GET /user/{uid}`).
//!
//! The leaderboard lists every player with at least one rated set, by rating (highest first),
//! ties by uid. Pages are keyset-paginated: the cursor is the last row's (rating, uid), so a
//! deep page costs the same index range scan as the first and no row is skipped or repeated
//! while ratings change between requests. Positions are counted (rows ahead of the cursor) in
//! the same snapshot as the page, so they are right on every page; the count is one pass over
//! the rated players, which is cheap at this scale.

use std::net::SocketAddr;

use axum::extract::{ConnectInfo, Query, State};
use axum::http::HeaderMap;
use axum::Json;
use common::net::ip_key;
use common::ranked::Standing;
use serde::{Deserialize, Serialize};
use sqlx::PgPool;
use uuid::Uuid;

use crate::api::{client_ip, limit};
use crate::cursor;
use crate::error::{ApiError, ApiResult};
use crate::AppState;

pub const DEFAULT_LIMIT: u32 = 50;
pub const MAX_LIMIT: u32 = 100;

/// Where a page starts: after this row.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Key {
    pub rating: f64,
    pub uid: Uuid,
}

impl Key {
    /// The opaque cursor. The rating goes in as its exact bits, so the next page starts
    /// exactly after this row even between two ratings that print the same.
    pub fn cursor(&self) -> String {
        cursor::encode("l1", &[&format!("{:016x}", self.rating.to_bits()), &self.uid.to_string()])
    }

    pub fn parse(c: &str) -> Option<Key> {
        let f = cursor::decode("l1", c, 2)?;
        if f[0].len() != 16 {
            return None;
        }
        let rating = f64::from_bits(u64::from_str_radix(&f[0], 16).ok()?);
        if !rating.is_finite() {
            return None;
        }
        Some(Key { rating, uid: Uuid::parse_str(&f[1]).ok()? })
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Entry {
    /// 1-based. Ties (same rating) are ordered by uid and still get their own positions.
    pub position: u64,
    pub uid: Uuid,
    pub display_name: String,
    pub connect_code: String,
    pub rating: f64,
    pub sets_played: u32,
    /// Sets won and lost.
    pub wins: u32,
    pub losses: u32,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Page {
    pub entries: Vec<Entry>,
    /// The cursor for the next page (`after`), or null on the last page.
    pub next: Option<String>,
    /// Players with at least one rated set.
    pub total: u64,
}

#[derive(Debug, sqlx::FromRow)]
struct Row {
    uid: Uuid,
    display_name: String,
    connect_code: Option<String>,
    rating: f64,
    sets_played: i32,
    wins: i32,
    losses: i32,
}

const PAGE_SQL: &str = "SELECT r.uid, u.display_name, u.connect_code, r.rating, r.sets_played, r.wins, r.losses
                          FROM ratings r JOIN users u ON u.uid = r.uid
                         WHERE r.sets_played > 0";

/// One page of `limit` rows after `after` (from the top without it).
pub async fn page(pool: &PgPool, after: Option<Key>, limit: u32) -> sqlx::Result<Page> {
    let limit = i64::from(limit);
    let mut tx = pool.begin().await?;
    // The page and the counts from one snapshot, so the positions match the rows.
    sqlx::query("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY").execute(&mut *tx).await?;
    let mut rows: Vec<Row> = match after {
        None => {
            sqlx::query_as(&format!("{PAGE_SQL} ORDER BY r.rating DESC, r.uid LIMIT $1"))
                .bind(limit + 1)
                .fetch_all(&mut *tx)
                .await?
        }
        Some(k) => {
            sqlx::query_as(&format!(
            "{PAGE_SQL} AND (r.rating < $2 OR (r.rating = $2 AND r.uid > $3)) ORDER BY r.rating DESC, r.uid LIMIT $1"
        ))
            .bind(limit + 1)
            .bind(k.rating)
            .bind(k.uid)
            .fetch_all(&mut *tx)
            .await?
        }
    };
    // Rows up to and including the cursor's (none without one), and everyone.
    let (ahead, total): (i64, i64) = sqlx::query_as(
        "SELECT count(*) FILTER (WHERE rating > $1 OR (rating = $1 AND uid <= $2)), count(*)
           FROM ratings WHERE sets_played > 0",
    )
    .bind(after.map(|k| k.rating))
    .bind(after.map(|k| k.uid))
    .fetch_one(&mut *tx)
    .await?;
    tx.commit().await?;

    let more = rows.len() as i64 > limit;
    rows.truncate(limit as usize);
    let next = if more { rows.last().map(|r| Key { rating: r.rating, uid: r.uid }.cursor()) } else { None };
    let entries = rows
        .into_iter()
        .enumerate()
        .map(|(i, r)| Entry {
            position: ahead.max(0) as u64 + 1 + i as u64,
            uid: r.uid,
            display_name: r.display_name,
            connect_code: r.connect_code.unwrap_or_default(),
            rating: r.rating,
            sets_played: r.sets_played.max(0) as u32,
            wins: r.wins.max(0) as u32,
            losses: r.losses.max(0) as u32,
        })
        .collect();
    Ok(Page { entries, next, total: total.max(0) as u64 })
}

/// A player's leaderboard position (`None` before their first rated set) and the number of
/// players on the leaderboard.
pub async fn placement(pool: &PgPool, uid: Uuid, standing: Standing) -> sqlx::Result<(Option<u64>, u64)> {
    let (ahead, total): (i64, i64) = sqlx::query_as(
        "SELECT count(*) FILTER (WHERE rating > $1 OR (rating = $1 AND uid < $2)), count(*)
           FROM ratings WHERE sets_played > 0",
    )
    .bind(standing.rating)
    .bind(uid)
    .fetch_one(pool)
    .await?;
    let position = (standing.sets_played > 0).then_some(ahead.max(0) as u64 + 1);
    Ok((position, total.max(0) as u64))
}

#[derive(Debug, Deserialize)]
pub struct LeaderboardQuery {
    pub limit: Option<u32>,
    pub after: Option<String>,
}

/// `GET /v1/ranked/leaderboard?limit=&after=`. Public; limited per client IP
/// ([`common::ratelimit::LEADERBOARD_IP_WINDOWS`]).
pub async fn leaderboard(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Query(q): Query<LeaderboardQuery>,
) -> ApiResult<Json<Page>> {
    let ip = ip_key(client_ip(&state, &headers, peer));
    limit(&state, |l, now| l.leaderboard_ip.check(&ip, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let after = match q.after.as_deref().filter(|a| !a.is_empty()) {
        None => None,
        Some(a) => Some(Key::parse(a).ok_or_else(|| ApiError::bad_request("invalid_cursor", "Invalid cursor"))?),
    };
    let n = cursor::page_limit(q.limit, DEFAULT_LIMIT, MAX_LIMIT);
    Ok(Json(page(&state.pool, after, n).await?))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cursors_keep_the_exact_rating() {
        let k = Key { rating: 1400.0 + 1e-10, uid: Uuid::from_bytes([7; 16]) };
        let back = Key::parse(&k.cursor()).unwrap();
        assert_eq!(back, k);
        assert_eq!(back.rating.to_bits(), k.rating.to_bits());
        assert!(Key::parse(&cursor::encode("l1", &["7ff0000000000000", &k.uid.to_string()])).is_none(), "inf");
        assert!(Key::parse(&cursor::encode("l1", &["40", &k.uid.to_string()])).is_none());
        assert!(Key::parse(&cursor::encode("l1", &["4095e00000000000", "nope"])).is_none());
        assert!(Key::parse(&cursor::encode("h1", &["4095e00000000000", &k.uid.to_string()])).is_none());
    }
}
