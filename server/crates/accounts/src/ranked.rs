//! Ranked sets: the clients' reports, settling a set and the rating change, and what the game
//! and launcher read back. The rules are in [`common::ranked`].
//!
//! Dolphin reports every ranked game from both sides (`POST /v1/ranked/report-game`, Slippi's
//! `reportOnlineGame`) and a set that ends early (`POST /v1/ranked/report-leave`, Slippi's
//! `reportOnlineMatchStatus` with `abandoned`). Both authenticate with the play key, as the mm
//! ticket does. After every report the set is settled: [`common::ranked::resolve`] decides it
//! and, once decided, both ratings change in the same transaction that closes the
//! `mm_matches` row, so a set is rated exactly once. Sets that wait on a grace period or go
//! silent are settled by [`sweep`], which runs in the background.
//!
//! `GET /v1/ranked/result?matchId=&uid=` (Slippi's `getRankedMatchPersonalResult`) and
//! `GET /user/{uid}` (`rank`) give the rating back.

use std::collections::HashMap;

use axum::extract::{Query, State};
use axum::Json;
use chrono::{DateTime, Utc};
use common::ranked::{self, GameReport, Leave, LeaveReport, Outcome, Standing, Timing, MAX_GAME_INDEX};
use serde::{Deserialize, Serialize};
use sqlx::{PgPool, Postgres, Transaction};
use uuid::Uuid;

use crate::error::{ApiError, ApiResult};
use crate::store;
use crate::AppState;

/// `mm_matches.mode` of a ranked set (Slippi's mode numbers).
const MODE_RANKED: i16 = 0;

// ---------------------------------------------------------------- store

/// A player's standing, or the default for one who never played ranked.
pub async fn standing(pool: &PgPool, uid: Uuid) -> sqlx::Result<Standing> {
    let row: Option<(f64, i32)> =
        sqlx::query_as("SELECT rating, sets_played FROM ratings WHERE uid = $1").bind(uid).fetch_optional(pool).await?;
    Ok(row.map(|(rating, n)| Standing { rating, sets_played: n.max(0) as u32 }).unwrap_or_default())
}

#[derive(Debug, Clone, sqlx::FromRow)]
struct MatchRow {
    mode: i16,
    players: Vec<Uuid>,
    created_at: DateTime<Utc>,
    status: String,
    winner: Option<Uuid>,
    end_reason: Option<String>,
}

async fn match_row(pool: &PgPool, match_id: &str) -> sqlx::Result<Option<MatchRow>> {
    sqlx::query_as("SELECT mode, players, created_at, status, winner, end_reason FROM mm_matches WHERE match_id = $1")
        .bind(match_id)
        .fetch_optional(pool)
        .await
}

async fn reports(
    tx: &mut Transaction<'_, Postgres>,
    match_id: &str,
) -> sqlx::Result<(Vec<GameReport>, Vec<LeaveReport>)> {
    let games: Vec<(i32, Uuid, Option<Uuid>, DateTime<Utc>)> = sqlx::query_as(
        "SELECT game_index, reporter, winner, created_at FROM game_reports WHERE match_id = $1 ORDER BY game_index",
    )
    .bind(match_id)
    .fetch_all(&mut **tx)
    .await?;
    let leaves: Vec<(Uuid, String, DateTime<Utc>)> =
        sqlx::query_as("SELECT reporter, kind, created_at FROM leave_reports WHERE match_id = $1")
            .bind(match_id)
            .fetch_all(&mut **tx)
            .await?;
    Ok((
        games
            .into_iter()
            .map(|(g, reporter, winner, at)| GameReport { game_index: g.max(0) as u32, reporter, winner, at })
            .collect(),
        leaves
            .into_iter()
            .filter_map(|(reporter, kind, at)| Some(LeaveReport { reporter, kind: Leave::parse(&kind)?, at }))
            .collect(),
    ))
}

/// The rating change of one player in one set.
#[derive(Debug, Clone, Copy, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RatingChange {
    pub before: f64,
    pub after: f64,
    pub change: f64,
    /// Rated sets after this one (Slippi's `ratingUpdateCount`).
    pub sets_played: u32,
}

/// Settles a ranked set if its reports decide it: closes the `mm_matches` row and applies the
/// rating changes, all in one transaction under the row's lock. Returns the outcome (still
/// `InProgress` if undecided), or `None` for a set that was already closed or is not ranked.
pub async fn settle(
    pool: &PgPool,
    match_id: &str,
    now: DateTime<Utc>,
    timing: Timing,
) -> sqlx::Result<Option<Outcome>> {
    let mut tx = pool.begin().await?;
    let row: Option<MatchRow> = sqlx::query_as(
        "SELECT mode, players, created_at, status, winner, end_reason FROM mm_matches WHERE match_id = $1 FOR UPDATE",
    )
    .bind(match_id)
    .fetch_optional(&mut *tx)
    .await?;
    let Some(row) = row else { return Ok(None) };
    if row.mode != MODE_RANKED || row.status != "ASSIGNED" {
        return Ok(None);
    }
    let Ok(players) = <[Uuid; 2]>::try_from(row.players.clone()) else {
        tracing::error!(match_id, "ranked match without exactly two players");
        return Ok(None);
    };
    let (games, leaves) = reports(&mut tx, match_id).await?;
    let outcome = ranked::resolve(players, row.created_at, &games, &leaves, now, timing);
    if matches!(outcome, Outcome::InProgress { .. }) {
        tx.commit().await?;
        return Ok(Some(outcome));
    }

    let scores = ranked::scores(players, &outcome);
    if scores.iter().any(Option::is_some) {
        // Rows for first-timers, then both rows locked in uid order (no deadlock between two
        // sets that share a player).
        let mut sorted = players;
        sorted.sort();
        sqlx::query(
            "INSERT INTO ratings (uid, rating) SELECT u, $2 FROM unnest($1::uuid[]) AS u ON CONFLICT (uid) DO NOTHING",
        )
        .bind(&sorted[..])
        .bind(ranked::DEFAULT_RATING)
        .execute(&mut *tx)
        .await?;
        let rows: Vec<(Uuid, f64, i32)> =
            sqlx::query_as("SELECT uid, rating, sets_played FROM ratings WHERE uid = ANY($1) ORDER BY uid FOR UPDATE")
                .bind(&sorted[..])
                .fetch_all(&mut *tx)
                .await?;
        let before: HashMap<Uuid, Standing> =
            rows.into_iter().map(|(u, rating, n)| (u, Standing { rating, sets_played: n.max(0) as u32 })).collect();
        let st = players.map(|p| before.get(&p).copied().unwrap_or_default());
        for i in 0..2 {
            let Some(score) = scores[i] else { continue };
            let (me, opp) = (st[i], st[1 - i]);
            let k = ranked::k_factor(me.sets_played);
            let after = ranked::updated(me.rating, me.sets_played, opp.rating, score);
            sqlx::query(
                "UPDATE ratings SET rating = $2, sets_played = sets_played + 1,
                        wins = wins + $3, losses = losses + $4, updated_at = $5
                  WHERE uid = $1",
            )
            .bind(players[i])
            .bind(after)
            .bind(i32::from(score > 0.5))
            .bind(i32::from(score < 0.5))
            .bind(now)
            .execute(&mut *tx)
            .await?;
            sqlx::query(
                "INSERT INTO rating_events (uid, match_id, opponent, score, rating_before, rating_after,
                                            opponent_rating, k, sets_before, created_at)
                 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
            )
            .bind(players[i])
            .bind(match_id)
            .bind(players[1 - i])
            .bind(score)
            .bind(me.rating)
            .bind(after)
            .bind(opp.rating)
            .bind(k)
            .bind(me.sets_played as i32)
            .bind(now)
            .execute(&mut *tx)
            .await?;
        }
    }
    let (winner, reason) = match &outcome {
        Outcome::Complete { winner, .. } => (Some(*winner), None),
        Outcome::Abandoned { leaver, other, credit_other, .. } => {
            (credit_other.then_some(*other), Some(format!("abandoned by {leaver}")))
        }
        Outcome::Void { reason, .. } => (None, Some(reason.clone())),
        Outcome::InProgress { .. } => unreachable!(),
    };
    sqlx::query("UPDATE mm_matches SET status = $2, winner = $3, ended_at = $4, end_reason = $5 WHERE match_id = $1")
        .bind(match_id)
        .bind(outcome.status())
        .bind(winner)
        .bind(now)
        .bind(&reason)
        .execute(&mut *tx)
        .await?;
    tx.commit().await?;
    tracing::info!(match_id, status = outcome.status(), wins = ?outcome.wins(), reason = ?reason, "ranked set settled");
    Ok(Some(outcome))
}

/// Settles every open ranked set whose waits have run out. Returns how many it closed.
pub async fn sweep(pool: &PgPool, now: DateTime<Utc>, timing: Timing) -> sqlx::Result<usize> {
    let open: Vec<String> = sqlx::query_scalar(
        "SELECT match_id FROM mm_matches WHERE mode = $1 AND status = 'ASSIGNED' ORDER BY created_at",
    )
    .bind(MODE_RANKED)
    .fetch_all(pool)
    .await?;
    let mut closed = 0;
    for id in open {
        if matches!(settle(pool, &id, now, timing).await?, Some(o) if !matches!(o, Outcome::InProgress { .. })) {
            closed += 1;
        }
    }
    Ok(closed)
}

/// Runs [`sweep`] every `every` until the process ends.
pub async fn sweeper(pool: PgPool, timing: Timing, every: std::time::Duration) {
    let mut tick = tokio::time::interval(every.max(std::time::Duration::from_secs(1)));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    loop {
        tick.tick().await;
        if let Err(e) = sweep(&pool, Utc::now(), timing).await {
            tracing::error!("ranked sweep failed: {e}");
        }
    }
}

// ---------------------------------------------------------------- API

/// A set as the game and launcher see it, for one player.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetState {
    pub match_id: String,
    /// `ASSIGNED` while undecided, then `COMPLETE`, `ABANDONED`, `TERMINATED`, `ERROR` or
    /// `ORPHANED` (Slippi's set states).
    pub status: String,
    pub players: Vec<Uuid>,
    /// Games won, in the order of `players`.
    pub wins: [u32; 2],
    pub winner: Option<Uuid>,
    pub reason: Option<String>,
    /// The asking player's rating change, once the set changed it.
    pub rating: Option<RatingChange>,
}

async fn set_state(pool: &PgPool, match_id: &str, uid: Uuid, timing: Timing) -> ApiResult<SetState> {
    let row = match_row(pool, match_id).await?.ok_or_else(ApiError::not_found)?;
    if row.mode != MODE_RANKED {
        return Err(ApiError::not_found());
    }
    // Wins from the reports as they stand (the same rules that settled it).
    let mut tx = pool.begin().await?;
    let (games, leaves) = reports(&mut tx, match_id).await?;
    tx.commit().await?;
    let wins = match <[Uuid; 2]>::try_from(row.players.clone()) {
        Ok(p) => ranked::resolve(p, row.created_at, &games, &leaves, Utc::now(), timing).wins(),
        Err(_) => [0, 0],
    };
    let event: Option<(f64, f64, i32)> = sqlx::query_as(
        "SELECT rating_before, rating_after, sets_before FROM rating_events WHERE match_id = $1 AND uid = $2",
    )
    .bind(match_id)
    .bind(uid)
    .fetch_optional(pool)
    .await?;
    Ok(SetState {
        match_id: match_id.to_string(),
        status: row.status,
        players: row.players,
        wins,
        winner: row.winner,
        reason: row.end_reason,
        rating: event.map(|(before, after, n)| RatingChange {
            before,
            after,
            change: after - before,
            sets_played: n.max(0) as u32 + 1,
        }),
    })
}

/// The play key check of the report endpoints: the account exists, the key is its current one,
/// and it is not banned.
async fn reporter(state: &AppState, uid: &str, play_key: &str) -> ApiResult<Uuid> {
    let uid = Uuid::parse_str(uid.trim()).map_err(|_| ApiError::unauthorized())?;
    let user = store::user_by_uid(&state.pool, uid).await?.ok_or_else(ApiError::unauthorized)?;
    if play_key.is_empty() || play_key.len() > 128 || !state.secret.verify(uid, user.play_key_version, play_key) {
        return Err(ApiError::unauthorized());
    }
    if user.is_banned() {
        return Err(ApiError::forbidden("banned", "This account is banned"));
    }
    Ok(uid)
}

/// The ranked set `match_id` with `uid` as one of its players.
async fn players_match(pool: &PgPool, match_id: &str, uid: Uuid) -> ApiResult<MatchRow> {
    let row = match_row(pool, match_id).await?.ok_or_else(ApiError::not_found)?;
    if row.mode != MODE_RANKED || !row.players.contains(&uid) {
        return Err(ApiError::not_found());
    }
    Ok(row)
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReportGameReq {
    pub uid: String,
    pub play_key: String,
    pub match_id: String,
    /// 1-based, counting drawn games.
    pub game_index: u32,
    /// The winner's uid; null for a draw.
    pub winner: Option<String>,
    #[serde(default)]
    pub stage_id: Option<i32>,
    #[serde(default)]
    pub duration_frames: Option<i32>,
    /// Per player: `{uid, characterId, stocksRemaining, damage}` (kept for review).
    #[serde(default)]
    pub players: serde_json::Value,
}

/// `reportOnlineGame` for a ranked game. Idempotent: the same report again is accepted; a
/// different winner for a game already reported is refused.
pub async fn report_game(State(state): State<AppState>, Json(req): Json<ReportGameReq>) -> ApiResult<Json<SetState>> {
    let uid = reporter(&state, &req.uid, &req.play_key).await?;
    let row = players_match(&state.pool, &req.match_id, uid).await?;
    if !(1..=MAX_GAME_INDEX).contains(&req.game_index) {
        return Err(ApiError::bad_request("invalid_game_index", "Invalid game index"));
    }
    let winner = match req.winner.as_deref().map(str::trim).filter(|w| !w.is_empty()) {
        None => None,
        Some(w) => match Uuid::parse_str(w) {
            Ok(w) if row.players.contains(&w) => Some(w),
            _ => return Err(ApiError::bad_request("invalid_winner", "The winner is not a player of this set")),
        },
    };
    let detail = serde_json::json!({ "players": req.players });
    let inserted = sqlx::query(
        "INSERT INTO game_reports (match_id, game_index, reporter, winner, stage_id, duration_frames, detail, created_at)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8) ON CONFLICT DO NOTHING",
    )
    .bind(&req.match_id)
    .bind(req.game_index as i32)
    .bind(uid)
    .bind(winner)
    .bind(req.stage_id)
    .bind(req.duration_frames)
    .bind(&detail)
    .bind(Utc::now())
    .execute(&state.pool)
    .await?
    .rows_affected();
    if inserted == 0 {
        let earlier: Option<Uuid> = sqlx::query_scalar(
            "SELECT winner FROM game_reports WHERE match_id = $1 AND game_index = $2 AND reporter = $3",
        )
        .bind(&req.match_id)
        .bind(req.game_index as i32)
        .bind(uid)
        .fetch_one(&state.pool)
        .await?;
        if earlier != winner {
            return Err(ApiError::conflict("already_reported", "This game was already reported with another winner"));
        }
    } else {
        tracing::info!(match_id = %req.match_id, game = req.game_index, %uid, winner = ?winner, "ranked game reported");
    }
    let timing = state.cfg.ranked_timing();
    settle(&state.pool, &req.match_id, Utc::now(), timing).await?;
    Ok(Json(set_state(&state.pool, &req.match_id, uid, timing).await?))
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReportLeaveReq {
    pub uid: String,
    pub play_key: String,
    pub match_id: String,
    /// `left` (this player left the set) or `opponent_left` (the opponent disconnected).
    pub kind: String,
}

/// `reportOnlineMatchStatus(abandoned)`: the set ended before it was decided.
pub async fn report_leave(State(state): State<AppState>, Json(req): Json<ReportLeaveReq>) -> ApiResult<Json<SetState>> {
    let uid = reporter(&state, &req.uid, &req.play_key).await?;
    players_match(&state.pool, &req.match_id, uid).await?;
    let kind = Leave::parse(&req.kind).ok_or_else(|| ApiError::bad_request("invalid_kind", "Unknown leave kind"))?;
    sqlx::query(
        "INSERT INTO leave_reports (match_id, reporter, kind, created_at) VALUES ($1, $2, $3, $4) ON CONFLICT DO NOTHING",
    )
    .bind(&req.match_id)
    .bind(uid)
    .bind(kind.as_str())
    .bind(Utc::now())
    .execute(&state.pool)
    .await?;
    tracing::info!(match_id = %req.match_id, %uid, kind = kind.as_str(), "ranked leave reported");
    let timing = state.cfg.ranked_timing();
    settle(&state.pool, &req.match_id, Utc::now(), timing).await?;
    Ok(Json(set_state(&state.pool, &req.match_id, uid, timing).await?))
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ResultQuery {
    pub match_id: String,
    pub uid: String,
}

/// `getRankedMatchPersonalResult`: a set's state and the player's rating change. Public, like
/// the ratings themselves.
pub async fn result(State(state): State<AppState>, Query(q): Query<ResultQuery>) -> ApiResult<Json<SetState>> {
    let uid = Uuid::parse_str(q.uid.trim()).map_err(|_| ApiError::not_found())?;
    players_match(&state.pool, &q.match_id, uid).await?;
    Ok(Json(set_state(&state.pool, &q.match_id, uid, state.cfg.ranked_timing()).await?))
}
