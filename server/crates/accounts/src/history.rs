//! A player's match history (`GET /v1/me/matches`): their Ranked, Unranked and Direct matches,
//! newest first, with the score from the game reports and, for ranked sets, the rating change.
//!
//! mm records every match it makes (`mm_matches`), including the many that never start (a
//! failed P2P connect requeues with a new match). The history shows a match once something
//! was played or decided in it: it has a game report, or it is a ranked set that was settled
//! and either changed a rating or has a leave report (an abandoned set with no game). A ranked
//! set that went silent without any report (`ORPHANED`, nobody rated) is left out.
//!
//! Keyset pagination on (`created_at`, `match_id`), newest first; the cursor is the last row's.

use std::collections::HashMap;

use axum::extract::{Query, State};
use axum::http::HeaderMap;
use axum::Json;
use chrono::{DateTime, Utc};
use common::ranked::{self, GameReport, Timing};
use serde::{Deserialize, Serialize};
use sqlx::PgPool;
use uuid::Uuid;

use crate::api::{current_user, limit};
use crate::cursor;
use crate::error::{ApiError, ApiResult};
use crate::ranked::MODE_RANKED;
use crate::AppState;

pub const DEFAULT_LIMIT: u32 = 20;
pub const MAX_LIMIT: u32 = 50;

/// The `mode` query parameter.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModeFilter {
    All,
    Ranked,
    /// Every mode but Ranked: Unranked and Direct.
    Unranked,
}

impl ModeFilter {
    pub fn parse(s: Option<&str>) -> Option<ModeFilter> {
        match s.map(str::trim).unwrap_or("") {
            "" | "all" => Some(ModeFilter::All),
            "ranked" => Some(ModeFilter::Ranked),
            "unranked" => Some(ModeFilter::Unranked),
            _ => None,
        }
    }

    fn sql(self) -> &'static str {
        match self {
            ModeFilter::All => "",
            ModeFilter::Ranked => " AND m.mode = 0",
            ModeFilter::Unranked => " AND m.mode <> 0",
        }
    }
}

/// `mm_matches.mode` as the API names it (Slippi's mode numbers: 0 Ranked, 1 Unranked,
/// 2 Direct).
pub fn mode_name(mode: i16) -> &'static str {
    match mode {
        0 => "ranked",
        2 => "direct",
        _ => "unranked",
    }
}

/// Games won per player, in the order of `players`: [`ranked::resolve`]'s count for a ranked
/// set (the rules that settle it), [`ranked::count_wins`] for the other modes.
pub fn wins(
    mode: i16,
    players: &[Uuid],
    created: DateTime<Utc>,
    games: &[GameReport],
    now: DateTime<Utc>,
    timing: Timing,
) -> Vec<u32> {
    if mode == MODE_RANKED {
        if let Ok(p) = <[Uuid; 2]>::try_from(players) {
            return ranked::resolve(p, created, games, &[], now, timing).wins().to_vec();
        }
    }
    ranked::count_wins(players, games)
}

/// Where a page starts: after (older than) this match.
#[derive(Debug, Clone, PartialEq)]
pub struct Key {
    pub created_at: DateTime<Utc>,
    pub match_id: String,
}

impl Key {
    pub fn cursor(&self) -> String {
        cursor::encode("h1", &[&self.created_at.timestamp_micros().to_string(), &self.match_id])
    }

    pub fn parse(c: &str) -> Option<Key> {
        let f = cursor::decode("h1", c, 2)?;
        let created_at = DateTime::from_timestamp_micros(f[0].parse().ok()?)?;
        Some(Key { created_at, match_id: f[1].clone() })
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MatchPlayer {
    pub uid: Uuid,
    /// Empty for a deleted account.
    pub display_name: String,
    pub connect_code: String,
    /// Games won in this match.
    pub wins: u32,
    /// The rating change this set caused; null for Unranked and Direct, and for a set that
    /// did not change this player's rating (undecided, void, or abandoned by the opponent
    /// before the first game).
    pub rating_before: Option<f64>,
    pub rating_after: Option<f64>,
    pub rating_change: Option<f64>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MatchItem {
    pub match_id: String,
    /// `ranked`, `unranked` or `direct`.
    pub mode: &'static str,
    pub created_at: DateTime<Utc>,
    /// The `mm_matches` status: `ASSIGNED` while a ranked set is undecided (and always for
    /// Unranked and Direct), then `COMPLETE`, `ABANDONED`, `TERMINATED`, `ERROR` or `ORPHANED`.
    pub status: String,
    pub ranked: bool,
    pub players: Vec<MatchPlayer>,
    /// The set's winner (ranked only).
    pub winner: Option<Uuid>,
    pub end_reason: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MatchPage {
    pub matches: Vec<MatchItem>,
    /// The cursor for the next (older) page (`before`), or null on the last page.
    pub next: Option<String>,
}

/// A `game_reports` row: match, game, reporter, winner, time.
type ReportRow = (String, i32, Uuid, Option<Uuid>, DateTime<Utc>);

#[derive(Debug, sqlx::FromRow)]
struct Row {
    match_id: String,
    mode: i16,
    created_at: DateTime<Utc>,
    status: String,
    players: Vec<Uuid>,
    winner: Option<Uuid>,
    end_reason: Option<String>,
}

/// One page of `uid`'s matches older than `before` (newest first).
pub async fn matches(
    pool: &PgPool,
    uid: Uuid,
    mode: ModeFilter,
    before: Option<&Key>,
    limit: u32,
    now: DateTime<Utc>,
    timing: Timing,
) -> sqlx::Result<MatchPage> {
    let limit = i64::from(limit);
    let mut tx = pool.begin().await?;
    sqlx::query("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY").execute(&mut *tx).await?;
    let sql = format!(
        "SELECT m.match_id, m.mode, m.created_at, m.status, m.players, m.winner, m.end_reason
           FROM mm_matches m
          WHERE m.players @> ARRAY[$1]::uuid[]{}
            AND ($2::timestamptz IS NULL OR (m.created_at, m.match_id) < ($2, $3))
            AND (EXISTS (SELECT 1 FROM game_reports g WHERE g.match_id = m.match_id)
                 OR (m.mode = {MODE_RANKED} AND m.status <> 'ASSIGNED'
                     AND (EXISTS (SELECT 1 FROM rating_events e WHERE e.match_id = m.match_id)
                          OR EXISTS (SELECT 1 FROM leave_reports l WHERE l.match_id = m.match_id))))
          ORDER BY m.created_at DESC, m.match_id DESC
          LIMIT $4",
        mode.sql()
    );
    let mut rows: Vec<Row> = sqlx::query_as(&sql)
        .bind(uid)
        .bind(before.map(|k| k.created_at))
        .bind(before.map(|k| k.match_id.clone()))
        .bind(limit + 1)
        .fetch_all(&mut *tx)
        .await?;
    let more = rows.len() as i64 > limit;
    rows.truncate(limit as usize);
    let next = if more {
        rows.last().map(|r| Key { created_at: r.created_at, match_id: r.match_id.clone() }.cursor())
    } else {
        None
    };

    let ids: Vec<String> = rows.iter().map(|r| r.match_id.clone()).collect();
    let reports: Vec<ReportRow> = sqlx::query_as(
        "SELECT match_id, game_index, reporter, winner, created_at FROM game_reports WHERE match_id = ANY($1)",
    )
    .bind(&ids)
    .fetch_all(&mut *tx)
    .await?;
    let events: Vec<(String, Uuid, f64, f64)> =
        sqlx::query_as("SELECT match_id, uid, rating_before, rating_after FROM rating_events WHERE match_id = ANY($1)")
            .bind(&ids)
            .fetch_all(&mut *tx)
            .await?;
    let mut uids: Vec<Uuid> = rows.iter().flat_map(|r| r.players.iter().copied()).collect();
    uids.sort();
    uids.dedup();
    let users: Vec<(Uuid, String, Option<String>)> =
        sqlx::query_as("SELECT uid, display_name, connect_code FROM users WHERE uid = ANY($1)")
            .bind(&uids)
            .fetch_all(&mut *tx)
            .await?;
    tx.commit().await?;

    let mut games: HashMap<String, Vec<GameReport>> = HashMap::new();
    for (id, g, reporter, winner, at) in reports {
        games.entry(id).or_default().push(GameReport { game_index: g.max(0) as u32, reporter, winner, at });
    }
    let events: HashMap<(String, Uuid), (f64, f64)> =
        events.into_iter().map(|(id, u, before, after)| ((id, u), (before, after))).collect();
    let users: HashMap<Uuid, (String, Option<String>)> =
        users.into_iter().map(|(u, name, code)| (u, (name, code))).collect();

    let matches = rows
        .into_iter()
        .map(|r| {
            let won =
                wins(r.mode, &r.players, r.created_at, games.get(&r.match_id).map_or(&[], Vec::as_slice), now, timing);
            let players = r
                .players
                .iter()
                .zip(won)
                .map(|(p, wins)| {
                    let (name, code) = users.get(p).cloned().unwrap_or_default();
                    let event = events.get(&(r.match_id.clone(), *p)).copied();
                    MatchPlayer {
                        uid: *p,
                        display_name: name,
                        connect_code: code.unwrap_or_default(),
                        wins,
                        rating_before: event.map(|e| e.0),
                        rating_after: event.map(|e| e.1),
                        rating_change: event.map(|(before, after)| after - before),
                    }
                })
                .collect();
            MatchItem {
                mode: mode_name(r.mode),
                ranked: r.mode == MODE_RANKED,
                match_id: r.match_id,
                created_at: r.created_at,
                status: r.status,
                players,
                winner: r.winner,
                end_reason: r.end_reason,
            }
        })
        .collect();
    Ok(MatchPage { matches, next })
}

#[derive(Debug, Deserialize)]
pub struct MatchesQuery {
    pub mode: Option<String>,
    pub limit: Option<u32>,
    pub before: Option<String>,
}

/// `GET /v1/me/matches?mode=all|ranked|unranked&limit=&before=`. Session auth; limited per
/// account ([`common::ratelimit::HISTORY_WINDOWS`]).
pub async fn my_matches(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(q): Query<MatchesQuery>,
) -> ApiResult<Json<MatchPage>> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.history.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let mode = ModeFilter::parse(q.mode.as_deref())
        .ok_or_else(|| ApiError::bad_request("invalid_mode", "mode must be all, ranked or unranked"))?;
    let before = match q.before.as_deref().filter(|b| !b.is_empty()) {
        None => None,
        Some(b) => Some(Key::parse(b).ok_or_else(|| ApiError::bad_request("invalid_cursor", "Invalid cursor"))?),
    };
    let n = cursor::page_limit(q.limit, DEFAULT_LIMIT, MAX_LIMIT);
    let page = matches(&state.pool, user.uid, mode, before.as_ref(), n, Utc::now(), state.cfg.ranked_timing()).await?;
    Ok(Json(page))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn modes() {
        assert_eq!(ModeFilter::parse(None), Some(ModeFilter::All));
        assert_eq!(ModeFilter::parse(Some("all")), Some(ModeFilter::All));
        assert_eq!(ModeFilter::parse(Some("ranked")), Some(ModeFilter::Ranked));
        assert_eq!(ModeFilter::parse(Some("unranked")), Some(ModeFilter::Unranked));
        assert_eq!(ModeFilter::parse(Some("direct")), None);
        assert_eq!((mode_name(0), mode_name(1), mode_name(2)), ("ranked", "unranked", "direct"));
    }

    #[test]
    fn cursors_keep_the_microseconds() {
        let t = DateTime::from_timestamp_micros(1_791_460_800_123_456).unwrap();
        let k = Key { created_at: t, match_id: "mode.ranked-1791460800-7".into() };
        assert_eq!(Key::parse(&k.cursor()).unwrap(), k);
        assert!(Key::parse(&cursor::encode("h1", &["soon", "x"])).is_none());
        assert!(Key::parse(&cursor::encode("l1", &["1", "x"])).is_none());
    }

    #[test]
    fn ranked_wins_wait_for_both_reports_and_others_do_not() {
        let p = [Uuid::from_bytes([1; 16]), Uuid::from_bytes([2; 16])];
        let t0 = DateTime::from_timestamp(1_791_460_800, 0).unwrap();
        let lone = [GameReport { game_index: 1, reporter: p[0], winner: Some(p[0]), at: t0 }];
        let timing = Timing::default();
        // Ranked: a lone report waits for the grace period.
        assert_eq!(wins(0, &p, t0, &lone, t0, timing), vec![0, 0]);
        assert_eq!(wins(0, &p, t0, &lone, t0 + timing.report_grace, timing), vec![1, 0]);
        // Unranked and Direct count it at once.
        assert_eq!(wins(1, &p, t0, &lone, t0, timing), vec![1, 0]);
        assert_eq!(wins(2, &p, t0, &lone, t0, timing), vec![1, 0]);
    }
}
