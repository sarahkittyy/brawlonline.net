//! Unranked's hidden rating: an Elo that only matchmaking reads (the user's, 2026-10-31: "silent
//! and on the backend only ... it doesn't need all the same protection and void features as
//! regular ranked elo, since players never see it").
//!
//! The same Elo as Ranked ([`crate::ranked::expected`], on the same scale), but updated after
//! every Unranked game rather than every set, as Unranked has no sets: two players play game after
//! game until one leaves. K starts at [`K_START`] and falls linearly over the first
//! [`PROVISIONAL_GAMES`] games to [`K_MIN`]. There is no minimum change and no placement. A player
//! starts from their Ranked rating at their first Unranked game ([`crate::ranked::DEFAULT_RATING`]
//! if they never played Ranked); after that, Ranked sets do not move it.
//!
//! Chosen on prod's Unranked games of 2026-10-23..31 (42,000 games, both reports agreeing on all
//! but 15): replayed with these numbers, the rating before a match's first game picks its winner
//! 80% of the time, against 61% for the Ranked rating (54% when neither player had placed, 70% of
//! Unranked's players). Seeding from 1400 instead of the Ranked rating, or also carrying each
//! Ranked set's change over, made no difference.

use std::collections::HashMap;

use chrono::{DateTime, Utc};
use uuid::Uuid;

use crate::ranked::expected;

/// K of a player's first Unranked game.
pub const K_START: f64 = 160.0;
/// K once a player has played [`PROVISIONAL_GAMES`] Unranked games.
pub const K_MIN: f64 = 32.0;
/// Games over which K falls from [`K_START`] to [`K_MIN`].
pub const PROVISIONAL_GAMES: u32 = 30;

/// K for a player who has played `games` rated Unranked games before this one.
pub fn k_factor(games: u32) -> f64 {
    if games >= PROVISIONAL_GAMES {
        return K_MIN;
    }
    let left = f64::from(PROVISIONAL_GAMES - games) / f64::from(PROVISIONAL_GAMES);
    K_MIN + (K_START - K_MIN) * left
}

/// A player's Unranked rating and the games it counts.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Standing {
    pub rating: f64,
    pub games: u32,
}

impl Standing {
    /// A player's standing before their first Unranked game: their Ranked rating, no games.
    pub fn new(seed: f64) -> Self {
        Standing { rating: seed, games: 0 }
    }
}

/// Both players' standings after a game `winner` won against `loser`. Each moves by their own K,
/// as in Ranked.
pub fn rate(winner: Standing, loser: Standing) -> (Standing, Standing) {
    let p = expected(winner.rating, loser.rating);
    (
        Standing { rating: winner.rating + k_factor(winner.games) * (1.0 - p), games: winner.games + 1 },
        Standing { rating: loser.rating - k_factor(loser.games) * (1.0 - p), games: loser.games + 1 },
    )
}

/// One rated Unranked game: who won, who lost, when.
#[derive(Debug, Clone, PartialEq)]
pub struct Game {
    pub match_id: String,
    pub game_index: i32,
    pub at: DateTime<Utc>,
    pub winner: Uuid,
    pub loser: Uuid,
}

/// A game's rating change: (winner, loser) before and after.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Change {
    pub before: (Standing, Standing),
    pub after: (Standing, Standing),
}

/// Every game's change and every player's standing after `games`, in the order given (the order
/// they were reported). A player's first game starts them at `seed(uid, at)`: their Ranked rating
/// at that time.
pub fn replay(
    games: &[Game],
    mut seed: impl FnMut(Uuid, DateTime<Utc>) -> f64,
) -> (Vec<Change>, HashMap<Uuid, Standing>) {
    let mut now: HashMap<Uuid, Standing> = HashMap::new();
    let mut changes = Vec::with_capacity(games.len());
    for g in games {
        let w = *now.entry(g.winner).or_insert_with(|| Standing::new(seed(g.winner, g.at)));
        let l = *now.entry(g.loser).or_insert_with(|| Standing::new(seed(g.loser, g.at)));
        let (w2, l2) = rate(w, l);
        now.insert(g.winner, w2);
        now.insert(g.loser, l2);
        changes.push(Change { before: (w, l), after: (w2, l2) });
    }
    (changes, now)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-9
    }

    #[test]
    fn k_falls_over_the_first_games() {
        assert!(close(k_factor(0), K_START));
        assert!(close(k_factor(15), (K_START + K_MIN) / 2.0));
        assert!(close(k_factor(PROVISIONAL_GAMES), K_MIN));
        assert!(close(k_factor(500), K_MIN));
    }

    #[test]
    fn an_even_game_moves_both_by_half_their_k() {
        let (w, l) = rate(Standing::new(1400.0), Standing { rating: 1400.0, games: 40 });
        assert!(close(w.rating, 1400.0 + K_START / 2.0));
        assert!(close(l.rating, 1400.0 - K_MIN / 2.0));
        assert_eq!((w.games, l.games), (1, 41));
    }

    #[test]
    fn beating_a_far_weaker_player_is_worth_almost_nothing() {
        // No minimum change, unlike Ranked.
        let (w, l) = rate(Standing { rating: 2400.0, games: 99 }, Standing { rating: 1000.0, games: 99 });
        assert!(w.rating - 2400.0 < 0.05);
        assert!(1000.0 - l.rating < 0.05);
    }

    #[test]
    fn replay_seeds_each_player_once_at_their_first_game() {
        let (a, b, c) = (Uuid::from_u128(1), Uuid::from_u128(2), Uuid::from_u128(3));
        let t = |s: i64| DateTime::from_timestamp(1_800_000_000 + s, 0).unwrap();
        let g = |i: i32, at: i64, winner, loser| Game { match_id: "m".into(), game_index: i, at: t(at), winner, loser };
        let games = [g(1, 0, a, b), g(2, 10, b, a), g(1, 20, c, a)];
        let mut seeded = vec![];
        let (changes, now) = replay(&games, |u, at| {
            seeded.push((u, at));
            if u == c {
                1800.0
            } else {
                1400.0
            }
        });
        assert_eq!(seeded, vec![(a, t(0)), (b, t(0)), (c, t(20))]);
        assert_eq!(changes.len(), 3);
        // Game 2 starts from game 1's result.
        assert_eq!(changes[1].before, (changes[0].after.1, changes[0].after.0));
        assert!(close(changes[2].before.0.rating, 1800.0));
        assert_eq!(now[&a].games, 3);
        assert_eq!(now[&c], changes[2].after.0);
    }
}
