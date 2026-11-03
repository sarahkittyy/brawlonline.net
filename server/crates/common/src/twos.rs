//! The 2v2 queue's hidden rating (`docs/design/2v2.md`, Sarah's board, 2026-11-02: "separate
//! hidden 2v2 mmr, starting from the unranked mmr, and you can rate a pair that queues higher than
//! its average").
//!
//! Every player has their own 2v2 rating, an Elo on Ranked's scale moved after every 2v2 game,
//! read only by matchmaking and never shown. A player's first 2v2 game starts them at their
//! Unranked rating ([`crate::unranked`], itself their Ranked rating until their first Unranked
//! game). K is Unranked's ([`crate::unranked::k_factor`]): 160 falling to 32 over the first 30
//! games.
//!
//! A game is rated team against team: each team's strength is its two players' average, plus
//! [`PAIR_BONUS`] for two who queued together (they know each other's play), and every player
//! moves by their own K times their result minus their team's expected score. A player who left
//! the game takes a loss whatever their team did, unless they were already out of stocks when they
//! left (Sarah's, 2026-11-02: "if you're already fully dead you can leave and receive the regular
//! mmr change whether your teammate wins or loses"); everyone else gets their team's result, so a
//! player who wins 1v2 after their teammate left still wins.

use uuid::Uuid;

use crate::ranked::expected;
use crate::unranked::{k_factor, Standing};

/// Points a team of two who queued together is taken to be stronger than its average, in
/// matchmaking's team balance and in the expected score its games are rated by.
pub const PAIR_BONUS: f64 = 50.0;

/// A team's strength: its players' average rating, plus [`PAIR_BONUS`] for a pair that queued
/// together.
pub fn team_strength(a: f64, b: f64, pair: bool) -> f64 {
    (a + b) / 2.0 + if pair { PAIR_BONUS } else { 0.0 }
}

/// One player of a rated 2v2 game.
#[derive(Debug, Clone, PartialEq)]
pub struct Player {
    pub uid: Uuid,
    /// Their rating and games before this one.
    pub before: Standing,
    /// 0 or 1.
    pub team: u8,
    /// Left the game before it ended.
    pub left: bool,
    /// Was out of stocks when they left (only with `left`).
    pub eliminated: bool,
}

impl Player {
    /// The player's result: their team's, but a loss for one who left with stocks.
    pub fn won(&self, winner_team: u8) -> bool {
        if self.left && !self.eliminated {
            return false;
        }
        self.team == winner_team
    }
}

/// Every player's standing after a game `winner_team` won. `pairs[t]`: team `t`'s two players
/// queued together. `None` unless the game had two players on each team.
pub fn rate(players: &[Player], pairs: [bool; 2], winner_team: u8) -> Option<Vec<Standing>> {
    let team = |t: u8| -> Vec<&Player> { players.iter().filter(|p| p.team == t).collect() };
    let (a, b) = (team(0), team(1));
    if players.len() != 4 || a.len() != 2 || b.len() != 2 || winner_team > 1 {
        return None;
    }
    let strength = |t: &[&Player], pair: bool| team_strength(t[0].before.rating, t[1].before.rating, pair);
    let e0 = expected(strength(&a, pairs[0]), strength(&b, pairs[1]));
    Some(
        players
            .iter()
            .map(|p| {
                let e = if p.team == 0 { e0 } else { 1.0 - e0 };
                let s = if p.won(winner_team) { 1.0 } else { 0.0 };
                Standing { rating: p.before.rating + k_factor(p.before.games) * (s - e), games: p.before.games + 1 }
            })
            .collect(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::unranked::{K_MIN, K_START};

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-9
    }

    fn p(n: u128, rating: f64, games: u32, team: u8) -> Player {
        Player { uid: Uuid::from_u128(n), before: Standing { rating, games }, team, left: false, eliminated: false }
    }

    #[test]
    fn an_even_game_moves_everyone_by_half_their_k() {
        let players = [p(1, 1400.0, 0, 0), p(2, 1400.0, 40, 0), p(3, 1400.0, 40, 1), p(4, 1400.0, 0, 1)];
        let after = rate(&players, [false, false], 0).unwrap();
        assert!(close(after[0].rating, 1400.0 + K_START / 2.0));
        assert!(close(after[1].rating, 1400.0 + K_MIN / 2.0));
        assert!(close(after[2].rating, 1400.0 - K_MIN / 2.0));
        assert!(close(after[3].rating, 1400.0 - K_START / 2.0));
        assert!(after.iter().zip(&players).all(|(a, p)| a.games == p.before.games + 1));
    }

    #[test]
    fn teams_are_rated_by_their_average() {
        // 1600 + 1200 against 1400 + 1400: an even game.
        let players = [p(1, 1600.0, 99, 0), p(2, 1200.0, 99, 0), p(3, 1400.0, 99, 1), p(4, 1400.0, 99, 1)];
        let after = rate(&players, [false, false], 1).unwrap();
        assert!(close(after[0].rating, 1600.0 - K_MIN / 2.0));
        assert!(close(after[1].rating, 1200.0 - K_MIN / 2.0));
        assert!(close(after[2].rating, 1400.0 + K_MIN / 2.0));
    }

    #[test]
    fn a_pair_is_expected_to_do_better_than_its_average() {
        let players = [p(1, 1400.0, 99, 0), p(2, 1400.0, 99, 0), p(3, 1400.0, 99, 1), p(4, 1400.0, 99, 1)];
        let alone = rate(&players, [false, false], 0).unwrap();
        let paired = rate(&players, [true, false], 0).unwrap();
        // The pair's win is worth less, and the other team's loss costs less.
        assert!(paired[0].rating < alone[0].rating);
        assert!(paired[2].rating > alone[2].rating);
        let e = expected(1400.0 + PAIR_BONUS, 1400.0);
        assert!(close(paired[0].rating, 1400.0 + K_MIN * (1.0 - e)));
    }

    #[test]
    fn a_leaver_loses_unless_already_out() {
        let mut players = [p(1, 1400.0, 99, 0), p(2, 1400.0, 99, 0), p(3, 1400.0, 99, 1), p(4, 1400.0, 99, 1)];
        // P2 left with stocks; P1 won the 1v2 anyway.
        players[1].left = true;
        let after = rate(&players, [false, false], 0).unwrap();
        assert!(after[0].rating > 1400.0, "the teammate who stayed wins");
        assert!(after[1].rating < 1400.0, "the leaver loses");
        assert!(after[2].rating < 1400.0 && after[3].rating < 1400.0);
        // Out of stocks when they left: the team's result.
        players[1].eliminated = true;
        let after = rate(&players, [false, false], 0).unwrap();
        assert!(after[1].rating > 1400.0);
        // The team that stayed loses: an eliminated leaver loses with it, one with stocks too.
        let after = rate(&players, [false, false], 1).unwrap();
        assert!(after[1].rating < 1400.0 && after[0].rating < 1400.0);
    }

    #[test]
    fn only_two_against_two_is_rated() {
        let players = [p(1, 1400.0, 0, 0), p(2, 1400.0, 0, 0), p(3, 1400.0, 0, 0), p(4, 1400.0, 0, 1)];
        assert!(rate(&players, [false, false], 0).is_none());
        assert!(rate(&players[..3], [false, false], 0).is_none());
        let players = [p(1, 1400.0, 0, 0), p(2, 1400.0, 0, 0), p(3, 1400.0, 0, 1), p(4, 1400.0, 0, 1)];
        assert!(rate(&players, [false, false], 2).is_none());
    }
}
