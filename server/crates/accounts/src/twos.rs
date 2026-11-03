//! The 2v2 queue's hidden rating ([`common::twos`]): rated from the same game reports as the match
//! history (`reportOnlineGame`, [`crate::ranked::report_game`]), read only by mm, never shown.
//!
//! A game is rated on its first report with a winner, from what that report says: each player's
//! team, who left and whether they were out of stocks then (`players`); the other players' reports
//! of the same game change nothing (`twos_rated_games` holds each game once). As Unranked's, nothing
//! is settled, voided or checked against mm: a wrong game only shifts whom someone meets next. A
//! draw is not rated.

use std::collections::HashMap;

use chrono::{DateTime, Utc};
use common::twos::{self, Player};
use common::unranked::Standing;
use serde::Deserialize;
use sqlx::PgPool;
use uuid::Uuid;

/// `mm_matches.mode` of a 2v2 match (`common::proto::Mode::Twos`).
pub const MODE_TWOS: i16 = 5;

/// A player as a 2v2 game's report has them (`players`, Dolphin's `Ranked.cpp`).
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Reported {
    uid: String,
    #[serde(default)]
    team: Option<u8>,
    #[serde(default)]
    left: bool,
    /// Out of stocks when they left.
    #[serde(default)]
    eliminated: bool,
}

/// The report's four players as (uid, team, left, eliminated), if it names the match's four
/// players, two on each team.
fn seats(players: &serde_json::Value, match_players: &[Uuid]) -> Option<Vec<(Uuid, u8, bool, bool)>> {
    let list: Vec<Reported> = serde_json::from_value(players.clone()).ok()?;
    let mut seats: Vec<(Uuid, u8, bool, bool)> = Vec::with_capacity(4);
    for r in list {
        let uid = Uuid::parse_str(r.uid.trim()).ok()?;
        let team = r.team.filter(|t| *t <= 1)?;
        if !match_players.contains(&uid) || seats.iter().any(|(u, ..)| *u == uid) {
            return None;
        }
        seats.push((uid, team, r.left, r.left && r.eliminated));
    }
    let on = |t: u8| seats.iter().filter(|s| s.1 == t).count();
    (seats.len() == 4 && match_players.len() == 4 && on(0) == 2 && on(1) == 2).then_some(seats)
}

/// Rates game `game_index` of 2v2 match `match_id`, which `winner`'s team won, from its report's
/// `players`, if no report of it was rated yet. A player's first game starts them at their
/// Unranked rating. False: the report could not be rated (not four players on two teams).
pub async fn rate_game(
    pool: &PgPool,
    match_id: &str,
    game_index: i32,
    players: &serde_json::Value,
    winner: Uuid,
    now: DateTime<Utc>,
) -> sqlx::Result<bool> {
    let row: Option<(Vec<Uuid>, Option<Vec<i16>>)> =
        sqlx::query_as("SELECT players, pairs FROM mm_matches WHERE match_id = $1 AND mode = $2")
            .bind(match_id)
            .bind(MODE_TWOS)
            .fetch_optional(pool)
            .await?;
    let Some((match_players, pairs)) = row else { return Ok(false) };
    let Some(seats) = seats(players, &match_players) else { return Ok(false) };
    let Some(&(_, winner_team, ..)) = seats.iter().find(|s| s.0 == winner) else { return Ok(false) };
    // Each team: its two players queued together (the same pair number in mm_matches.pairs).
    let pair_of = |uid: Uuid| -> i16 {
        let i = match_players.iter().position(|p| *p == uid);
        i.and_then(|i| pairs.as_ref().and_then(|p| p.get(i).copied())).unwrap_or(0)
    };
    let team_pair = |t: u8| {
        let ids: Vec<i16> = seats.iter().filter(|s| s.1 == t).map(|s| pair_of(s.0)).collect();
        ids.len() == 2 && ids[0] != 0 && ids[0] == ids[1]
    };
    let team_pairs = [team_pair(0), team_pair(1)];

    let mut tx = pool.begin().await?;
    // Rows for first-timers (their Unranked rating, else their Ranked one), then all four locked in
    // uid order (no deadlock between two games that share a player).
    let mut sorted: Vec<Uuid> = seats.iter().map(|s| s.0).collect();
    sorted.sort();
    sqlx::query(
        "INSERT INTO twos_ratings (uid, rating)
         SELECT u, COALESCE(ur.rating, r.rating, $2) FROM unnest($1::uuid[]) AS u
           LEFT JOIN unranked_ratings ur ON ur.uid = u LEFT JOIN ratings r ON r.uid = u
         ORDER BY u ON CONFLICT (uid) DO NOTHING",
    )
    .bind(&sorted)
    .bind(common::ranked::DEFAULT_RATING)
    .execute(&mut *tx)
    .await?;
    let rows: Vec<(Uuid, f64, i32)> =
        sqlx::query_as("SELECT uid, rating, games FROM twos_ratings WHERE uid = ANY($1) ORDER BY uid FOR UPDATE")
            .bind(&sorted)
            .fetch_all(&mut *tx)
            .await?;
    let standing: HashMap<Uuid, Standing> =
        rows.into_iter().map(|(u, rating, games)| (u, Standing { rating, games: games.max(0) as u32 })).collect();
    let mut rated = Vec::with_capacity(4);
    for &(uid, team, left, eliminated) in &seats {
        let Some(&before) = standing.get(&uid) else {
            return Ok(false); // an account deleted meanwhile
        };
        rated.push(Player { uid, before, team, left, eliminated });
    }
    let Some(after) = twos::rate(&rated, team_pairs, winner_team) else { return Ok(false) };
    let detail: Vec<serde_json::Value> = rated
        .iter()
        .zip(&after)
        .map(|(p, a)| {
            serde_json::json!({
                "uid": p.uid, "team": p.team, "left": p.left, "eliminated": p.eliminated,
                "pair": team_pairs[p.team as usize], "before": p.before.rating, "after": a.rating,
            })
        })
        .collect();
    let inserted = sqlx::query(
        "INSERT INTO twos_rated_games (match_id, game_index, winner_team, players, created_at)
         VALUES ($1, $2, $3, $4, $5) ON CONFLICT DO NOTHING",
    )
    .bind(match_id)
    .bind(game_index)
    .bind(i16::from(winner_team))
    .bind(serde_json::Value::Array(detail))
    .bind(now)
    .execute(&mut *tx)
    .await?
    .rows_affected();
    if inserted == 0 {
        return Ok(true); // another player's report rated it (the rows for first-timers roll back too)
    }
    for (p, s) in rated.iter().zip(&after) {
        sqlx::query("UPDATE twos_ratings SET rating = $2, games = $3, updated_at = $4 WHERE uid = $1")
            .bind(p.uid)
            .bind(s.rating)
            .bind(s.games as i32)
            .bind(now)
            .execute(&mut *tx)
            .await?;
    }
    tx.commit().await?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn a_report_names_four_players_on_two_teams() {
        let u: Vec<Uuid> = (1..=4).map(Uuid::from_u128).collect();
        let ok = json!([
            {"uid": u[0], "port": 1, "team": 0, "place": 1},
            {"uid": u[1], "port": 2, "team": 1, "place": 2},
            {"uid": u[2], "port": 3, "team": 0, "left": true, "eliminated": true},
            {"uid": u[3], "port": 4, "team": 1, "left": true},
        ]);
        let s = seats(&ok, &u).unwrap();
        assert_eq!(s[2], (u[2], 0, true, true));
        assert_eq!(s[3], (u[3], 1, true, false));
        // "eliminated" counts only for a leaver.
        let stayed = json!([
            {"uid": u[0], "team": 0, "eliminated": true}, {"uid": u[1], "team": 1},
            {"uid": u[2], "team": 0}, {"uid": u[3], "team": 1},
        ]);
        assert_eq!(seats(&stayed, &u).unwrap()[0], (u[0], 0, false, false));
        let three_on_one = json!([
            {"uid": u[0], "team": 0}, {"uid": u[1], "team": 0}, {"uid": u[2], "team": 0}, {"uid": u[3], "team": 1},
        ]);
        assert!(seats(&three_on_one, &u).is_none());
        let stranger = json!([
            {"uid": u[0], "team": 0}, {"uid": u[1], "team": 1}, {"uid": Uuid::from_u128(9), "team": 0}, {"uid": u[3], "team": 1},
        ]);
        assert!(seats(&stranger, &u).is_none());
        let twice = json!([
            {"uid": u[0], "team": 0}, {"uid": u[0], "team": 1}, {"uid": u[2], "team": 0}, {"uid": u[3], "team": 1},
        ]);
        assert!(seats(&twice, &u).is_none());
        let no_team =
            json!([{"uid": u[0]}, {"uid": u[1], "team": 1}, {"uid": u[2], "team": 0}, {"uid": u[3], "team": 1}]);
        assert!(seats(&no_team, &u).is_none());
        assert!(seats(&json!({"not": "a list"}), &u).is_none());
    }
}
