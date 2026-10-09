//! Ranked: the Elo rating and how a ranked set is decided from the two clients' reports.
//!
//! **Rating.** Standard Elo on Slippi's scale: the expected score is
//! `1 / (1 + 10^((opponent - own) / 400))` and the new rating is `own + K * (score - expected)`,
//! with `score` 1 for a set win and 0 for a loss. One update per set, as Slippi updates once per
//! set (`ratingUpdateCount` counts sets). Each player uses their own K, as in FIDE's Elo, so a
//! new player's big swings do not move their established opponent by the same amount.
//!
//! K starts high and falls linearly over the first [`PROVISIONAL_SETS`] sets to [`K_MIN`]:
//! a new player leaves [`DEFAULT_RATING`] within a handful of sets (a player who wins their first
//! ten even sets climbs about 620 points, one who loses them all drops as much), and after that a
//! set moves an even pairing by ±16, as in classic Elo.
//!
//! The numbers sit on Slippi's ranked range (design 4.2): Slippi's ratings run from a few hundred
//! to about 2,500-2,800 at the top, with the middle around 1,400 (Silver 3 / Gold 1). A rating
//! gap of 400 means 10:1 odds, so a top player 1,200 above the middle wins about 999 sets in
//! 1,000 against it, which spreads the field over the same range. New players start at the
//! middle. There are no rank tiers: the rating is shown as a number.
//!
//! **Sets.** Ranked is best of three ([`WINS_NEEDED`] game wins), as on Slippi. Both clients
//! report every game; [`resolve`] turns the reports into the set's outcome. See its docs for the
//! rules (agreement, one-sided reports, abandonment, void).

use chrono::{DateTime, Duration, Utc};
use uuid::Uuid;

/// The rating of a player who has not played a ranked set.
pub const DEFAULT_RATING: f64 = 1400.0;
/// K of a player's first set.
pub const K_START: f64 = 200.0;
/// K once a player has played [`PROVISIONAL_SETS`] sets.
pub const K_MIN: f64 = 32.0;
/// Sets over which K falls from [`K_START`] to [`K_MIN`].
pub const PROVISIONAL_SETS: u32 = 10;

/// Ranked sets are best of three.
pub const BEST_OF: u32 = 3;
pub const WINS_NEEDED: u32 = BEST_OF / 2 + 1;
/// Highest game index a report may name: a best of three plus room for drawn games, which are
/// replayed (a draw counts for nobody).
pub const MAX_GAME_INDEX: u32 = 9;
/// Highest game index a report of an Unranked or Direct game may name. Those matches are not
/// sets: the two players play game after game until one leaves.
pub const MAX_SESSION_GAME_INDEX: u32 = 999;

/// K for a player who has played `sets_played` rated sets before this one.
pub fn k_factor(sets_played: u32) -> f64 {
    if sets_played >= PROVISIONAL_SETS {
        return K_MIN;
    }
    let left = f64::from(PROVISIONAL_SETS - sets_played) / f64::from(PROVISIONAL_SETS);
    K_MIN + (K_START - K_MIN) * left
}

/// The expected score of a player rated `own` against one rated `opponent` (0..1).
pub fn expected(own: f64, opponent: f64) -> f64 {
    1.0 / (1.0 + 10f64.powf((opponent - own) / 400.0))
}

/// The rating after one set: `score` 1.0 for a win, 0.0 for a loss.
pub fn updated(own: f64, sets_played: u32, opponent: f64, score: f64) -> f64 {
    own + k_factor(sets_played) * (score - expected(own, opponent))
}

/// One client's report of one game.
#[derive(Debug, Clone, PartialEq)]
pub struct GameReport {
    /// 1-based, counting drawn games.
    pub game_index: u32,
    pub reporter: Uuid,
    /// The winner's uid, or `None` for a draw.
    pub winner: Option<Uuid>,
    pub at: DateTime<Utc>,
}

/// A client's report that the set ended early.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Leave {
    /// The reporter left (held Z, closed the menu): they abandoned the set.
    Left,
    /// The reporter's opponent disconnected or went silent.
    OpponentLeft,
}

impl Leave {
    pub fn parse(s: &str) -> Option<Leave> {
        match s {
            "left" => Some(Leave::Left),
            "opponent_left" => Some(Leave::OpponentLeft),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Leave::Left => "left",
            Leave::OpponentLeft => "opponent_left",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct LeaveReport {
    pub reporter: Uuid,
    pub kind: Leave,
    pub at: DateTime<Utc>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Timing {
    /// How long a game report waits for the other client's before it counts on its own, and
    /// how long an "opponent left" waits for the opponent's own report before the opponent is
    /// taken to have abandoned.
    pub report_grace: Duration,
    /// A set with no report for this long (counted from the match, or from its last report) is
    /// void.
    pub stale_after: Duration,
}

impl Default for Timing {
    fn default() -> Self {
        Timing { report_grace: Duration::minutes(2), stale_after: Duration::minutes(30) }
    }
}

/// What the reports say about a set.
#[derive(Debug, Clone, PartialEq)]
pub enum Outcome {
    /// Not decided yet.
    InProgress { wins: [u32; 2] },
    /// A player won [`WINS_NEEDED`] games. Both ratings change.
    Complete { winner: Uuid, loser: Uuid, wins: [u32; 2] },
    /// A player abandoned the set before it was decided. The leaver takes a set loss; the other
    /// player gets the win only once at least one game was played (`credit_other`).
    Abandoned { leaver: Uuid, other: Uuid, credit_other: bool, wins: [u32; 2] },
    /// No rating change. `status` is the `mm_matches.status` (Slippi's set states):
    /// `TERMINATED` (the connection broke and nobody can be blamed), `ERROR` (the reports
    /// disagree; held for review), `ORPHANED` (nothing more was reported).
    Void { status: &'static str, reason: String, wins: [u32; 2] },
}

impl Outcome {
    /// The `mm_matches.status` a decided set gets (Slippi's ASSIGNED .. ERROR).
    pub fn status(&self) -> &'static str {
        match self {
            Outcome::InProgress { .. } => "ASSIGNED",
            Outcome::Complete { .. } => "COMPLETE",
            Outcome::Abandoned { .. } => "ABANDONED",
            Outcome::Void { status, .. } => status,
        }
    }

    pub fn wins(&self) -> [u32; 2] {
        match self {
            Outcome::InProgress { wins }
            | Outcome::Complete { wins, .. }
            | Outcome::Abandoned { wins, .. }
            | Outcome::Void { wins, .. } => *wins,
        }
    }
}

/// Decides a set of `players` (exactly two) from the reports so far.
///
/// - A game counts once both clients reported it with the same winner. Different winners void
///   the set as `ERROR` (held for review, like Slippi's dual-report check, design 4.4). A game
///   only one client reported counts on that report once `report_grace` has passed without the
///   other's (its client crashed or lost its connection to us after the game). Games count in
///   order: a game still waiting for its second report holds back the ones after it.
///   A draw counts for nobody and is replayed.
/// - The first player to [`WINS_NEEDED`] counted games wins the set.
/// - Otherwise a leave decides it: a player who reports [`Leave::Left`] abandoned it. A player
///   whose opponent reports [`Leave::OpponentLeft`] and who does not report the same within
///   `report_grace` abandoned it (their game crashed or was closed). Both reporting
///   `OpponentLeft` is a broken connection: void as `TERMINATED`, nobody is blamed.
/// - A set with no report for `stale_after` is void as `ORPHANED`.
///
/// Reports from anyone but the two players, and winners who are not one of them, are ignored
/// (the report endpoint refuses them anyway).
pub fn resolve(
    players: [Uuid; 2],
    created: DateTime<Utc>,
    games: &[GameReport],
    leaves: &[LeaveReport],
    now: DateTime<Utc>,
    timing: Timing,
) -> Outcome {
    let idx = |u: Uuid| players.iter().position(|p| *p == u);
    let games: Vec<&GameReport> =
        games.iter().filter(|g| idx(g.reporter).is_some() && g.winner.is_none_or(|w| idx(w).is_some())).collect();
    let leaves: Vec<&LeaveReport> = leaves.iter().filter(|l| idx(l.reporter).is_some()).collect();

    let mut wins = [0u32; 2];
    let mut waiting = false;
    for game in 1..=MAX_GAME_INDEX {
        let mut by = [None, None];
        for g in games.iter().filter(|g| g.game_index == game) {
            by[idx(g.reporter).unwrap()] = Some(*g);
        }
        let winner = match by {
            [Some(a), Some(b)] if a.winner != b.winner => {
                return Outcome::Void {
                    status: "ERROR",
                    reason: format!("the players reported different winners for game {game}"),
                    wins,
                };
            }
            [Some(a), Some(_)] => a.winner,
            [Some(r), None] | [None, Some(r)] if now - r.at >= timing.report_grace => r.winner,
            [Some(_), None] | [None, Some(_)] => {
                waiting = true;
                break;
            }
            [None, None] => break,
        };
        if let Some(w) = winner {
            let i = idx(w).unwrap();
            wins[i] += 1;
            if wins[i] >= WINS_NEEDED {
                return Outcome::Complete { winner: players[i], loser: players[1 - i], wins };
            }
        }
    }
    if waiting {
        return Outcome::InProgress { wins };
    }

    let reported = |p: usize, kind: Leave| leaves.iter().find(|l| l.reporter == players[p] && l.kind == kind);
    let left = [reported(0, Leave::Left).is_some(), reported(1, Leave::Left).is_some()];
    let opp_left = [reported(0, Leave::OpponentLeft), reported(1, Leave::OpponentLeft)];
    let played = wins.iter().sum::<u32>() > 0 || games.iter().any(|g| g.winner.is_none());
    let abandon = |leaver: usize| Outcome::Abandoned {
        leaver: players[leaver],
        other: players[1 - leaver],
        credit_other: played,
        wins,
    };
    match left {
        [true, true] => {
            return Outcome::Void { status: "TERMINATED", reason: "both players left".into(), wins };
        }
        [true, false] => return abandon(0),
        [false, true] => return abandon(1),
        [false, false] => {}
    }
    match opp_left {
        [Some(_), Some(_)] => {
            return Outcome::Void {
                status: "TERMINATED",
                reason: "the connection between the players broke".into(),
                wins,
            };
        }
        // Player 0 says player 1 left; player 1 has had `report_grace` to say the same.
        [Some(r), None] if now - r.at >= timing.report_grace => return abandon(1),
        [None, Some(r)] if now - r.at >= timing.report_grace => return abandon(0),
        [Some(_), None] | [None, Some(_)] => return Outcome::InProgress { wins },
        [None, None] => {}
    }

    let last = games.iter().map(|g| g.at).chain(leaves.iter().map(|l| l.at)).max().unwrap_or(created).max(created);
    if now - last >= timing.stale_after {
        return Outcome::Void { status: "ORPHANED", reason: "no result was reported".into(), wins };
    }
    Outcome::InProgress { wins }
}

/// Games won per player (in the order of `players`) in a match that is not rated (Unranked,
/// Direct): the simple form of [`resolve`]'s counting. A game counts when its reports agree or
/// only one player reported it, at once (nothing waits on these). Reports that disagree, draws,
/// reports from anyone but the players and winners who are not players count for nobody.
pub fn count_wins(players: &[Uuid], games: &[GameReport]) -> Vec<u32> {
    let idx = |u: Uuid| players.iter().position(|p| *p == u);
    let mut by_game: std::collections::BTreeMap<u32, Vec<Option<Uuid>>> = std::collections::BTreeMap::new();
    for g in games.iter().filter(|g| idx(g.reporter).is_some()) {
        by_game.entry(g.game_index).or_default().push(g.winner);
    }
    let mut wins = vec![0u32; players.len()];
    for winners in by_game.values() {
        let first = winners[0];
        if winners.iter().any(|w| *w != first) {
            continue;
        }
        if let Some(i) = first.and_then(idx) {
            wins[i] += 1;
        }
    }
    wins
}

/// A player's rating row: the rating and the rated sets so far.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Standing {
    pub rating: f64,
    pub sets_played: u32,
}

impl Default for Standing {
    fn default() -> Self {
        Standing { rating: DEFAULT_RATING, sets_played: 0 }
    }
}

/// The rating changes an outcome causes, for players in the order of `players`:
/// `Some(score)` (1 win, 0 loss) for each player whose rating changes.
pub fn scores(players: [Uuid; 2], outcome: &Outcome) -> [Option<f64>; 2] {
    let score_of = |winner: Uuid, p: Uuid| if p == winner { 1.0 } else { 0.0 };
    match outcome {
        Outcome::Complete { winner, .. } => players.map(|p| Some(score_of(*winner, p))),
        Outcome::Abandoned { leaver, credit_other, .. } => players.map(|p| {
            if p == *leaver {
                Some(0.0)
            } else if *credit_other {
                Some(1.0)
            } else {
                None
            }
        }),
        Outcome::InProgress { .. } | Outcome::Void { .. } => [None, None],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-9
    }

    #[test]
    fn k_falls_from_start_to_min_over_the_provisional_sets() {
        assert!(close(k_factor(0), K_START));
        assert!(close(k_factor(5), (K_START + K_MIN) / 2.0));
        assert!(close(k_factor(PROVISIONAL_SETS), K_MIN));
        assert!(close(k_factor(500), K_MIN));
        for n in 0..PROVISIONAL_SETS {
            assert!(k_factor(n) > k_factor(n + 1));
        }
    }

    #[test]
    fn classic_elo_numbers() {
        assert!(close(expected(1400.0, 1400.0), 0.5));
        // 400 points is 10:1.
        assert!(close(expected(1800.0, 1400.0), 10.0 / 11.0));
        assert!(close(expected(1400.0, 1800.0) + expected(1800.0, 1400.0), 1.0));
        // Established, even pairing: ±16.
        assert!(close(updated(1500.0, 50, 1500.0, 1.0), 1516.0));
        assert!(close(updated(1500.0, 50, 1500.0, 0.0), 1484.0));
        // A first set moves 100 either way against an even opponent.
        assert!(close(updated(DEFAULT_RATING, 0, DEFAULT_RATING, 1.0), DEFAULT_RATING + 100.0));
        // Beating a much weaker player is worth little; losing to them costs a lot.
        assert!(updated(2000.0, 50, 1400.0, 1.0) - 2000.0 < 1.0);
        assert!(2000.0 - updated(2000.0, 50, 1400.0, 0.0) > 31.0);
    }

    /// A new player leaves the default fast: ten straight wins (or losses) against opponents at
    /// their own rating move them about 620 points; established players move ±16 a set.
    #[test]
    fn placement_sets_move_new_players_far() {
        let (mut up, mut down) = (DEFAULT_RATING, DEFAULT_RATING);
        for n in 0..PROVISIONAL_SETS {
            up = updated(up, n, up, 1.0);
            down = updated(down, n, down, 0.0);
        }
        assert!((600.0..640.0).contains(&(up - DEFAULT_RATING)), "{up}");
        assert!((600.0..640.0).contains(&(DEFAULT_RATING - down)), "{down}");
    }

    fn uid(n: u8) -> Uuid {
        Uuid::from_bytes([n; 16])
    }

    struct Set {
        p: [Uuid; 2],
        t0: DateTime<Utc>,
        games: Vec<GameReport>,
        leaves: Vec<LeaveReport>,
    }

    impl Set {
        fn new() -> Set {
            Set {
                p: [uid(1), uid(2)],
                t0: DateTime::parse_from_rfc3339("2026-10-08T12:00:00Z").unwrap().into(),
                games: vec![],
                leaves: vec![],
            }
        }
        fn at(&self, min: i64) -> DateTime<Utc> {
            self.t0 + Duration::minutes(min)
        }
        /// Player `by` reports game `g` won by player `w` (None: draw) at minute `min`.
        fn game(&mut self, by: usize, g: u32, w: Option<usize>, min: i64) -> &mut Self {
            let r = GameReport { game_index: g, reporter: self.p[by], winner: w.map(|i| self.p[i]), at: self.at(min) };
            self.games.push(r);
            self
        }
        /// Both report game `g` won by `w`.
        fn both(&mut self, g: u32, w: Option<usize>, min: i64) -> &mut Self {
            self.game(0, g, w, min).game(1, g, w, min)
        }
        fn leave(&mut self, by: usize, kind: Leave, min: i64) -> &mut Self {
            let r = LeaveReport { reporter: self.p[by], kind, at: self.at(min) };
            self.leaves.push(r);
            self
        }
        fn at_min(&self, min: i64) -> Outcome {
            resolve(self.p, self.t0, &self.games, &self.leaves, self.at(min), Timing::default())
        }
    }

    #[test]
    fn two_wins_take_the_set() {
        let mut s = Set::new();
        s.both(1, Some(0), 5);
        assert_eq!(s.at_min(5), Outcome::InProgress { wins: [1, 0] });
        s.both(2, Some(1), 10);
        assert_eq!(s.at_min(10), Outcome::InProgress { wins: [1, 1] });
        s.both(3, Some(1), 15);
        let o = s.at_min(15);
        assert_eq!(o, Outcome::Complete { winner: s.p[1], loser: s.p[0], wins: [1, 2] });
        assert_eq!(o.status(), "COMPLETE");
        assert_eq!(scores(s.p, &o), [Some(0.0), Some(1.0)]);
    }

    #[test]
    fn a_draw_is_replayed() {
        let mut s = Set::new();
        s.both(1, Some(0), 5).both(2, None, 10).both(3, Some(1), 15);
        assert_eq!(s.at_min(15), Outcome::InProgress { wins: [1, 1] });
        s.both(4, Some(0), 20);
        assert_eq!(s.at_min(20), Outcome::Complete { winner: s.p[0], loser: s.p[1], wins: [2, 1] });
    }

    #[test]
    fn disagreeing_reports_void_the_set_for_review() {
        let mut s = Set::new();
        s.both(1, Some(0), 5).game(0, 2, Some(0), 10).game(1, 2, Some(1), 10);
        let o = s.at_min(10);
        assert!(matches!(o, Outcome::Void { status: "ERROR", .. }), "{o:?}");
        assert_eq!(scores(s.p, &o), [None, None]);
    }

    #[test]
    fn a_lone_report_counts_after_the_grace_period() {
        let mut s = Set::new();
        s.both(1, Some(0), 5).game(0, 2, Some(0), 10);
        assert_eq!(s.at_min(11), Outcome::InProgress { wins: [1, 0] });
        assert_eq!(s.at_min(12), Outcome::Complete { winner: s.p[0], loser: s.p[1], wins: [2, 0] });
        // The other report arriving in time and agreeing is the same result, at once.
        let mut s = Set::new();
        s.both(1, Some(0), 5).game(0, 2, Some(0), 10).game(1, 2, Some(0), 11);
        assert_eq!(s.at_min(11), Outcome::Complete { winner: s.p[0], loser: s.p[1], wins: [2, 0] });
    }

    #[test]
    fn games_count_in_order() {
        // Game 2 waits for its second report; game 3's reports do not jump ahead of it.
        let mut s = Set::new();
        s.both(1, Some(0), 5).game(0, 2, Some(1), 10).both(3, Some(1), 11);
        assert_eq!(s.at_min(11), Outcome::InProgress { wins: [1, 0] });
        assert_eq!(s.at_min(12), Outcome::Complete { winner: s.p[1], loser: s.p[0], wins: [1, 2] });
    }

    #[test]
    fn leaving_abandons_the_set() {
        // After a game: the leaver loses, the other player wins.
        let mut s = Set::new();
        s.both(1, Some(0), 5).leave(0, Leave::Left, 6);
        let o = s.at_min(6);
        assert_eq!(o, Outcome::Abandoned { leaver: s.p[0], other: s.p[1], credit_other: true, wins: [1, 0] });
        assert_eq!(o.status(), "ABANDONED");
        assert_eq!(scores(s.p, &o), [Some(0.0), Some(1.0)]);
        // Before any game: the leaver still loses, the other player gets nothing.
        let mut s = Set::new();
        s.leave(1, Leave::Left, 1);
        let o = s.at_min(1);
        assert_eq!(o, Outcome::Abandoned { leaver: s.p[1], other: s.p[0], credit_other: false, wins: [0, 0] });
        assert_eq!(scores(s.p, &o), [None, Some(0.0)]);
    }

    #[test]
    fn opponent_left_needs_the_grace_period_and_no_counter_report() {
        // Player 1 says player 0 left; player 0 says nothing: player 0 abandoned.
        let mut s = Set::new();
        s.both(1, Some(1), 5).leave(1, Leave::OpponentLeft, 7);
        assert_eq!(s.at_min(8), Outcome::InProgress { wins: [0, 1] });
        assert_eq!(s.at_min(9), Outcome::Abandoned { leaver: s.p[0], other: s.p[1], credit_other: true, wins: [0, 1] });
        // Both say the other left: the connection broke, nobody is blamed.
        let mut s = Set::new();
        s.both(1, Some(1), 5).leave(1, Leave::OpponentLeft, 7).leave(0, Leave::OpponentLeft, 7);
        let o = s.at_min(7);
        assert!(matches!(o, Outcome::Void { status: "TERMINATED", .. }), "{o:?}");
        // Their own "left" settles it at once.
        let mut s = Set::new();
        s.leave(1, Leave::OpponentLeft, 7).leave(0, Leave::Left, 7);
        assert!(matches!(s.at_min(7), Outcome::Abandoned { credit_other: false, .. }));
    }

    #[test]
    fn a_finished_set_ignores_later_leaves() {
        let mut s = Set::new();
        s.both(1, Some(0), 5).both(2, Some(0), 10).leave(1, Leave::OpponentLeft, 11);
        assert_eq!(s.at_min(20), Outcome::Complete { winner: s.p[0], loser: s.p[1], wins: [2, 0] });
    }

    #[test]
    fn a_silent_set_is_orphaned() {
        let s = Set::new();
        assert_eq!(s.at_min(29), Outcome::InProgress { wins: [0, 0] });
        assert!(matches!(s.at_min(30), Outcome::Void { status: "ORPHANED", .. }));
        let mut s = Set::new();
        s.both(1, Some(0), 20);
        assert_eq!(s.at_min(49), Outcome::InProgress { wins: [1, 0] });
        assert!(matches!(s.at_min(50), Outcome::Void { status: "ORPHANED", wins: [1, 0], .. }));
    }

    #[test]
    fn unrated_matches_count_agreeing_and_lone_reports() {
        let mut s = Set::new();
        // Game 1 agreed, game 2 only one report, game 3 a draw, game 4 disagreeing,
        // game 12 (past a best of three) agreed.
        s.both(1, Some(0), 1).game(1, 2, Some(1), 2).both(3, None, 3);
        s.game(0, 4, Some(0), 4).game(1, 4, Some(1), 4).both(12, Some(1), 12);
        assert_eq!(count_wins(&s.p, &s.games), vec![1, 2]);
        // Strangers' reports and winners who are not players count for nobody.
        s.games.push(GameReport { game_index: 20, reporter: uid(9), winner: Some(s.p[0]), at: s.at(20) });
        s.games.push(GameReport { game_index: 21, reporter: s.p[0], winner: Some(uid(9)), at: s.at(21) });
        assert_eq!(count_wins(&s.p, &s.games), vec![1, 2]);
        assert_eq!(count_wins(&s.p, &[]), vec![0, 0]);
    }

    #[test]
    fn strangers_and_bad_winners_are_ignored() {
        let mut s = Set::new();
        s.both(1, Some(0), 5);
        s.games.push(GameReport { game_index: 2, reporter: uid(9), winner: Some(s.p[1]), at: s.at(6) });
        s.games.push(GameReport { game_index: 2, reporter: s.p[0], winner: Some(uid(9)), at: s.at(6) });
        assert_eq!(s.at_min(20), Outcome::InProgress { wins: [1, 0] });
    }
}
