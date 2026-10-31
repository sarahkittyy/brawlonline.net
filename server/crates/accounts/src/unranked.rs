//! Unranked's hidden rating ([`common::unranked`]): rated from the same game reports as the match
//! history (`reportOnlineGame`, [`crate::ranked::report_game`]), read only by mm, never shown.
//!
//! A game is rated on its first report with a winner; the other player's report of the same game
//! changes nothing (`unranked_rated_games` holds each game once). Nothing is settled, voided or
//! checked against mm: both players' reports agree on all but a handful of games, and a wrong
//! game here only shifts whom someone meets next.

use std::collections::HashMap;

use chrono::{DateTime, Utc};
use common::unranked::{self, Game, Standing};
use sqlx::{PgConnection, PgPool};
use uuid::Uuid;

/// `mm_matches.mode` of an Unranked match (Slippi's mode numbers).
pub const MODE_UNRANKED: i16 = 1;

/// Rates game `game_index` of Unranked match `match_id`, which `winner` won against `loser`, if
/// no report of it was rated yet. A player's first game starts them at their Ranked rating.
pub async fn rate_game(
    pool: &PgPool,
    match_id: &str,
    game_index: i32,
    winner: Uuid,
    loser: Uuid,
    now: DateTime<Utc>,
) -> sqlx::Result<()> {
    let mut tx = pool.begin().await?;
    // Rows for first-timers, then both rows locked in uid order (no deadlock between two games
    // that share a player).
    let mut sorted = [winner, loser];
    sorted.sort();
    sqlx::query(
        "INSERT INTO unranked_ratings (uid, rating)
         SELECT u, COALESCE(r.rating, $2) FROM unnest($1::uuid[]) AS u LEFT JOIN ratings r ON r.uid = u
         ORDER BY u ON CONFLICT (uid) DO NOTHING",
    )
    .bind(&sorted[..])
    .bind(common::ranked::DEFAULT_RATING)
    .execute(&mut *tx)
    .await?;
    let rows: Vec<(Uuid, f64, i32)> =
        sqlx::query_as("SELECT uid, rating, games FROM unranked_ratings WHERE uid = ANY($1) ORDER BY uid FOR UPDATE")
            .bind(&sorted[..])
            .fetch_all(&mut *tx)
            .await?;
    let standing: HashMap<Uuid, Standing> =
        rows.into_iter().map(|(u, rating, games)| (u, Standing { rating, games: games.max(0) as u32 })).collect();
    let (Some(&w), Some(&l)) = (standing.get(&winner), standing.get(&loser)) else {
        return Ok(()); // an account deleted meanwhile
    };
    let (w2, l2) = unranked::rate(w, l);
    let inserted = sqlx::query(
        "INSERT INTO unranked_rated_games (match_id, game_index, winner, loser, winner_before, loser_before,
                                           winner_after, loser_after, created_at)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9) ON CONFLICT DO NOTHING",
    )
    .bind(match_id)
    .bind(game_index)
    .bind(winner)
    .bind(loser)
    .bind(w.rating)
    .bind(l.rating)
    .bind(w2.rating)
    .bind(l2.rating)
    .bind(now)
    .execute(&mut *tx)
    .await?
    .rows_affected();
    if inserted == 0 {
        return Ok(()); // the other player's report rated it (the rows for first-timers roll back too)
    }
    for (uid, s) in [(winner, w2), (loser, l2)] {
        sqlx::query("UPDATE unranked_ratings SET rating = $2, games = $3, updated_at = $4 WHERE uid = $1")
            .bind(uid)
            .bind(s.rating)
            .bind(s.games as i32)
            .bind(now)
            .execute(&mut *tx)
            .await?;
    }
    tx.commit().await
}

/// What [`rebuild`] did.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Rebuilt {
    pub games: usize,
    pub players: usize,
}

/// At startup: fills the Unranked ratings from every Unranked game reported so far if none was
/// rated yet (the release that adds them). `None`: nothing to do.
pub async fn backfill(pool: &PgPool) -> sqlx::Result<Option<Rebuilt>> {
    let rated: bool = sqlx::query_scalar("SELECT EXISTS (SELECT 1 FROM unranked_rated_games)").fetch_one(pool).await?;
    if rated {
        return Ok(None);
    }
    let done = rebuild(pool).await?;
    if done.games == 0 {
        return Ok(None);
    }
    tracing::info!(games = done.games, players = done.players, "unranked ratings filled from the game reports");
    Ok(Some(done))
}

/// Rates every Unranked game reported so far again, in the order of their first reports, from
/// each player's Ranked rating at their first game, replacing all Unranked ratings. Holds both
/// tables locked against new games meanwhile (they wait, then rate on top).
pub async fn rebuild(pool: &PgPool) -> sqlx::Result<Rebuilt> {
    let mut tx = pool.begin().await?;
    sqlx::query("LOCK TABLE unranked_ratings, unranked_rated_games IN SHARE ROW EXCLUSIVE MODE")
        .execute(&mut *tx)
        .await?;
    let games = reported_games(&mut tx).await?;
    let ranked = ranked_history(&mut tx).await?;
    let current: HashMap<Uuid, f64> =
        sqlx::query_as("SELECT uid, rating FROM ratings").fetch_all(&mut *tx).await?.into_iter().collect();
    let (changes, now) = unranked::replay(&games, |uid, at| ranked_at(&ranked, &current, uid, at));

    sqlx::query("DELETE FROM unranked_rated_games").execute(&mut *tx).await?;
    sqlx::query("DELETE FROM unranked_ratings").execute(&mut *tx).await?;
    let col = |f: &dyn Fn(&unranked::Change) -> f64| changes.iter().map(f).collect::<Vec<f64>>();
    sqlx::query(
        "INSERT INTO unranked_rated_games (match_id, game_index, winner, loser, winner_before, loser_before,
                                           winner_after, loser_after, created_at)
         SELECT * FROM unnest($1::text[], $2::int[], $3::uuid[], $4::uuid[], $5::float8[], $6::float8[],
                              $7::float8[], $8::float8[], $9::timestamptz[])",
    )
    .bind(games.iter().map(|g| g.match_id.clone()).collect::<Vec<_>>())
    .bind(games.iter().map(|g| g.game_index).collect::<Vec<_>>())
    .bind(games.iter().map(|g| g.winner).collect::<Vec<_>>())
    .bind(games.iter().map(|g| g.loser).collect::<Vec<_>>())
    .bind(col(&|c| c.before.0.rating))
    .bind(col(&|c| c.before.1.rating))
    .bind(col(&|c| c.after.0.rating))
    .bind(col(&|c| c.after.1.rating))
    .bind(games.iter().map(|g| g.at).collect::<Vec<_>>())
    .execute(&mut *tx)
    .await?;
    // Players since deleted are left out (no users row to point at).
    let uids: Vec<Uuid> = now.keys().copied().collect();
    sqlx::query(
        "INSERT INTO unranked_ratings (uid, rating, games)
         SELECT v.uid, v.rating, v.games FROM unnest($1::uuid[], $2::float8[], $3::int[]) AS v(uid, rating, games)
         JOIN users u ON u.uid = v.uid",
    )
    .bind(&uids)
    .bind(uids.iter().map(|u| now[u].rating).collect::<Vec<_>>())
    .bind(uids.iter().map(|u| now[u].games as i32).collect::<Vec<_>>())
    .execute(&mut *tx)
    .await?;
    tx.commit().await?;
    Ok(Rebuilt { games: games.len(), players: uids.len() })
}

/// Every reported two-player Unranked game with a winner, once (its first report with one), in
/// the order of those reports.
async fn reported_games(conn: &mut PgConnection) -> sqlx::Result<Vec<Game>> {
    #[allow(clippy::type_complexity)]
    let rows: Vec<(String, i32, DateTime<Utc>, Vec<Uuid>, Uuid)> = sqlx::query_as(
        "SELECT DISTINCT ON (g.match_id, g.game_index) g.match_id, g.game_index, g.created_at, m.players, g.winner
           FROM game_reports g JOIN mm_matches m USING (match_id)
          WHERE m.mode = $1 AND cardinality(m.players) = 2 AND g.winner IS NOT NULL
          ORDER BY g.match_id, g.game_index, g.created_at",
    )
    .bind(MODE_UNRANKED)
    .fetch_all(&mut *conn)
    .await?;
    let mut games: Vec<Game> = rows
        .into_iter()
        .filter_map(|(match_id, game_index, at, players, winner)| {
            let loser = *players.iter().find(|p| **p != winner)?;
            Some(Game { match_id, game_index, at, winner, loser })
        })
        .collect();
    games.sort_by(|a, b| {
        a.at.cmp(&b.at).then_with(|| a.match_id.cmp(&b.match_id)).then(a.game_index.cmp(&b.game_index))
    });
    Ok(games)
}

/// Each player's Ranked rating after each rated set, in order.
async fn ranked_history(conn: &mut PgConnection) -> sqlx::Result<HashMap<Uuid, Vec<(DateTime<Utc>, f64)>>> {
    let rows: Vec<(Uuid, DateTime<Utc>, f64)> =
        sqlx::query_as("SELECT uid, created_at, rating_after FROM rating_events ORDER BY created_at, id")
            .fetch_all(&mut *conn)
            .await?;
    let mut by_uid: HashMap<Uuid, Vec<(DateTime<Utc>, f64)>> = HashMap::new();
    for (uid, at, rating) in rows {
        by_uid.entry(uid).or_default().push((at, rating));
    }
    Ok(by_uid)
}

/// A player's Ranked rating at `at`: after their last set settled before it. A player with a
/// rating but no rating history (set by hand) has had it all along.
fn ranked_at(
    history: &HashMap<Uuid, Vec<(DateTime<Utc>, f64)>>,
    current: &HashMap<Uuid, f64>,
    uid: Uuid,
    at: DateTime<Utc>,
) -> f64 {
    let Some(sets) = history.get(&uid) else {
        return current.get(&uid).copied().unwrap_or(common::ranked::DEFAULT_RATING);
    };
    match sets.partition_point(|(t, _)| *t < at) {
        0 => common::ranked::DEFAULT_RATING,
        n => sets[n - 1].1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_ranked_rating_at_a_time_is_the_last_set_before_it() {
        let t = |s: i64| DateTime::from_timestamp(1_800_000_000 + s, 0).unwrap();
        let u = Uuid::from_u128(7);
        let h = HashMap::from([(u, vec![(t(10), 1500.0), (t(20), 1450.0)])]);
        let current = HashMap::from([(u, 1450.0), (Uuid::from_u128(9), 1600.0)]);
        assert_eq!(ranked_at(&h, &current, u, t(5)), common::ranked::DEFAULT_RATING);
        assert_eq!(ranked_at(&h, &current, u, t(15)), 1500.0);
        assert_eq!(ranked_at(&h, &current, u, t(25)), 1450.0);
        assert_eq!(ranked_at(&h, &current, Uuid::from_u128(8), t(25)), common::ranked::DEFAULT_RATING);
        // A rating set by hand, without history.
        assert_eq!(ranked_at(&h, &current, Uuid::from_u128(9), t(5)), 1600.0);
    }
}
