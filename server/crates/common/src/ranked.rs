//! Ranked: the Elo rating and how a ranked set is decided from the two clients' reports.
//!
//! **Rating.** Standard Elo on Slippi's scale: the expected score is
//! `1 / (1 + 10^((opponent - own) / 400))` and the new rating is `own + K * (score - expected)`,
//! with `score` 1 for a set win and 0 for a loss, moved at least [`MIN_CHANGE`] either way so
//! every set counts. One update per set, as Slippi updates once per
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
//! middle.
//!
//! **Ranks** (docs/design/ranked-tiers.md) are shown on top of the rating: seven tiers, Bronze to
//! Legendary, each but Legendary in three divisions (3 lowest, 1 highest). A player's rank follows
//! from their sets in order ([`TierState::after_set`]): none while placing, then the division the
//! rating reaches, held until the rating falls [`DEMOTION_BUFFER`] below its floor. Placing ends
//! when the rating's uncertainty (Glicko's rating deviation) is small enough ([`RD_PLACED`]); the
//! RD decides nothing else.
//!
//! **Sets.** Ranked is best of three ([`WINS_NEEDED`] game wins), as on Slippi. Both clients
//! report every game; [`resolve`] turns the reports into the set's outcome. See its docs for the
//! rules (agreement, one-sided reports, abandonment, void).

use std::collections::HashMap;

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

/// The smallest rating change a set makes: a win is worth at least this much and a loss costs at
/// least this much, even against a far weaker or far stronger opponent.
pub const MIN_CHANGE: f64 = 1.0;

/// The rating after one set: `score` 1.0 for a win, 0.0 for a loss.
pub fn updated(own: f64, sets_played: u32, opponent: f64, score: f64) -> f64 {
    let change = k_factor(sets_played) * (score - expected(own, opponent));
    own + if score > 0.5 { change.max(MIN_CHANGE) } else { change.min(-MIN_CHANGE) }
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

// ---------------------------------------------------------------- ranks

/// A rank tier, lowest first.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Tier {
    Bronze,
    Silver,
    Gold,
    Emerald,
    Diamond,
    Master,
    Legendary,
}

impl Tier {
    pub const ALL: [Tier; 7] =
        [Tier::Bronze, Tier::Silver, Tier::Gold, Tier::Emerald, Tier::Diamond, Tier::Master, Tier::Legendary];

    /// The API's name.
    pub fn as_str(self) -> &'static str {
        match self {
            Tier::Bronze => "bronze",
            Tier::Silver => "silver",
            Tier::Gold => "gold",
            Tier::Emerald => "emerald",
            Tier::Diamond => "diamond",
            Tier::Master => "master",
            Tier::Legendary => "legendary",
        }
    }
}

/// The lowest rating of each rank, lowest rank first: Bronze 3, 2, 1, Silver 3, 2, 1, ..., Master 1,
/// Legendary. Bronze 3 has no floor. Tiers: Silver 1150, Gold 1300, Emerald 1450, Diamond 1625,
/// Master 2000, Legendary 2250, chosen so that on a grown ladder Bronze to Diamond each hold about a
/// fifth of the players, Master a few percent and Legendary about one (docs/design/ranked-tiers.md
/// §2). A change here needs `pp-admin ranked recompute` to apply to stored ranks.
pub const RANK_FLOORS: [f64; 19] = [
    f64::NEG_INFINITY,
    1050.0,
    1100.0,
    1150.0,
    1200.0,
    1250.0,
    1300.0,
    1350.0,
    1400.0,
    1450.0,
    1510.0,
    1570.0,
    1625.0,
    1750.0,
    1875.0,
    2000.0,
    2085.0,
    2170.0,
    2250.0,
];

/// A rank: an index into [`RANK_FLOORS`] (0 is Bronze 3, 18 Legendary), stored as `ratings.rank`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Rank(u8);

impl Rank {
    pub const LOWEST: Rank = Rank(0);
    pub const HIGHEST: Rank = Rank(RANK_FLOORS.len() as u8 - 1);

    /// The rank a rating reaches.
    pub fn for_rating(rating: f64) -> Rank {
        let reached = RANK_FLOORS.iter().filter(|&&floor| rating >= floor).count();
        Rank(reached.saturating_sub(1) as u8)
    }

    pub fn from_index(i: i16) -> Option<Rank> {
        u8::try_from(i).ok().filter(|&i| usize::from(i) < RANK_FLOORS.len()).map(Rank)
    }

    pub fn index(self) -> i16 {
        i16::from(self.0)
    }

    pub fn tier(self) -> Tier {
        Tier::ALL[usize::from(self.0 / 3).min(Tier::ALL.len() - 1)]
    }

    /// 3 (a tier's lowest) to 1 (its highest); `None` for Legendary.
    pub fn division(self) -> Option<u8> {
        (self.tier() != Tier::Legendary).then_some(3 - self.0 % 3)
    }

    pub fn floor(self) -> f64 {
        RANK_FLOORS[usize::from(self.0)]
    }
}

/// How far below its floor a rank is kept: a placed player drops a division only once their
/// rating is this far below the held division's floor, and then one division at a time.
pub const DEMOTION_BUFFER: f64 = 100.0;

/// The rank a placed player holds after a set: `held` before it, `rating` after it. A rating at a
/// higher division's floor promotes to the highest one reached (divisions can be skipped); a rating
/// more than [`DEMOTION_BUFFER`] below the held division's floor demotes to `rank(rating + buffer)`,
/// which is one division down for a set's usual change and never below the rating's own rank.
pub fn held_rank(held: Rank, rating: f64) -> Rank {
    let reached = Rank::for_rating(rating);
    if reached >= held {
        return reached;
    }
    held.min(Rank::for_rating(rating + DEMOTION_BUFFER)).max(reached)
}

/// The rating deviation (Glicko-1) of a player who has not played a ranked set.
pub const RD_START: f64 = 350.0;
/// RD at or below which a player is placed and gets a rank.
pub const RD_PLACED: f64 = 150.0;
/// The lowest RD kept, so a veteran's opponents still learn something from a set.
pub const RD_MIN: f64 = 30.0;
/// The opponent RD assumed when counting the sets a player has left to place.
pub const RD_ESTABLISHED: f64 = 80.0;
/// Placing takes at least this many sets...
pub const MIN_PLACEMENT_SETS: u32 = 5;
/// ...and at most this many, whatever the RD (K reaches its floor here too).
pub const MAX_PLACEMENT_SETS: u32 = 10;

const GLICKO_Q: f64 = std::f64::consts::LN_10 / 400.0;

/// The RD after one set (Glicko-1's update; the outcome does not matter, only how much a set
/// against this opponent can tell): `1/RD'^2 = 1/RD^2 + 1/d^2`. An even set against an established
/// player tells the most; a lopsided one, or one against another new player, less.
pub fn rd_after(rd: f64, own: f64, opponent: f64, opponent_rd: f64) -> f64 {
    let pi2 = std::f64::consts::PI * std::f64::consts::PI;
    let g = 1.0 / (1.0 + 3.0 * GLICKO_Q * GLICKO_Q * opponent_rd * opponent_rd / pi2).sqrt();
    let e = 1.0 / (1.0 + 10f64.powf(-g * (own - opponent) / 400.0));
    let d2_inv = GLICKO_Q * GLICKO_Q * g * g * e * (1.0 - e);
    (1.0 / (1.0 / (rd * rd) + d2_inv)).sqrt().max(RD_MIN)
}

/// Whether a player with this RD after `sets_played` rated sets is placed.
pub fn placed(rd: f64, sets_played: u32) -> bool {
    sets_played >= MAX_PLACEMENT_SETS || (sets_played >= MIN_PLACEMENT_SETS && rd <= RD_PLACED)
}

/// Rated sets a player still needs to place (0 once placed): even sets against an established
/// player that would bring the RD to [`RD_PLACED`], within [`MIN_PLACEMENT_SETS`] and
/// [`MAX_PLACEMENT_SETS`]. Lopsided sets lower the RD less, so the count can stay the same after one.
pub fn sets_to_place(rd: f64, sets_played: u32) -> u32 {
    let (mut rd, mut n) = (rd, sets_played);
    while !placed(rd, n) {
        rd = rd_after(rd, 0.0, 0.0, RD_ESTABLISHED);
        n += 1;
    }
    n - sets_played
}

/// A player's rank state: the RD, and the rank once placed.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TierState {
    pub rd: f64,
    pub rank: Option<Rank>,
}

impl Default for TierState {
    fn default() -> Self {
        TierState { rd: RD_START, rank: None }
    }
}

impl TierState {
    /// The state after one rated set: `sets_before` rated sets before it, `rating_before` and
    /// `rating_after` this player's, `opponent` the opponent's rating and RD before the set.
    pub fn after_set(self, sets_before: u32, rating_before: f64, rating_after: f64, opponent: (f64, f64)) -> TierState {
        let rd = rd_after(self.rd, rating_before, opponent.0, opponent.1);
        let rank = if !placed(rd, sets_before + 1) {
            None
        } else {
            Some(match self.rank {
                // Placed with this set: the rating's own rank.
                None => Rank::for_rating(rating_after),
                Some(held) => held_rank(held, rating_after),
            })
        };
        TierState { rd, rank }
    }
}

/// One stored rating change, as [`replay`] reads it (`rating_events`, in order).
#[derive(Debug, Clone, PartialEq)]
pub struct RatedSet {
    pub id: i64,
    pub uid: Uuid,
    pub match_id: String,
    pub opponent: Option<Uuid>,
    pub rating_before: f64,
    pub rating_after: f64,
    pub opponent_rating: f64,
    pub sets_before: u32,
}

/// What [`replay`] computes for one stored rating change.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ReplayedSet {
    pub id: i64,
    pub rank_before: Option<Rank>,
    pub rank_after: Option<Rank>,
    pub rd_after: f64,
}

/// Every player's rank state from their rating changes, in the order they were made (`created_at`,
/// then `id`). The two changes of one set use each other's RD from before the set. Returns each
/// change's ranks and RD, and each player's final state.
pub fn replay(sets: &[RatedSet]) -> (Vec<ReplayedSet>, HashMap<Uuid, TierState>) {
    let mut states: HashMap<Uuid, TierState> = HashMap::new();
    let mut out = Vec::with_capacity(sets.len());
    let mut i = 0;
    while i < sets.len() {
        // The rows of one set are next to each other (one transaction, same time).
        let mut j = i + 1;
        while j < sets.len() && sets[j].match_id == sets[i].match_id {
            j += 1;
        }
        let before: Vec<(f64, TierState)> = sets[i..j]
            .iter()
            .map(|s| {
                let opp = s.opponent.and_then(|o| states.get(&o).copied()).unwrap_or_default();
                (opp.rd, states.get(&s.uid).copied().unwrap_or_default())
            })
            .collect();
        for (s, (opp_rd, me)) in sets[i..j].iter().zip(before) {
            let next = me.after_set(s.sets_before, s.rating_before, s.rating_after, (s.opponent_rating, opp_rd));
            out.push(ReplayedSet { id: s.id, rank_before: me.rank, rank_after: next.rank, rd_after: next.rd });
            states.insert(s.uid, next);
        }
        i = j;
    }
    (out, states)
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
        assert!(updated(2000.0, 50, 1400.0, 1.0) - 2000.0 < 2.0);
        assert!(2000.0 - updated(2000.0, 50, 1400.0, 0.0) > 31.0);
        // But never less than 1 either way.
        assert!(close(updated(2000.0, 50, 1400.0, 1.0), 2000.0 + MIN_CHANGE));
        assert!(close(updated(1400.0, 50, 2000.0, 0.0), 1400.0 - MIN_CHANGE));
        assert!(close(updated(3000.0, 50, 100.0, 1.0), 3000.0 + MIN_CHANGE));
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
    // ------------------------------------------------------------ ranks

    fn rank(tier: Tier, division: Option<u8>) -> Rank {
        (0..RANK_FLOORS.len() as i16)
            .filter_map(Rank::from_index)
            .find(|r| r.tier() == tier && r.division() == division)
            .unwrap()
    }

    #[test]
    fn ranks_by_rating() {
        assert_eq!(Rank::for_rating(-50.0), Rank::LOWEST);
        assert_eq!(Rank::for_rating(1049.9), rank(Tier::Bronze, Some(3)));
        assert_eq!(Rank::for_rating(1050.0), rank(Tier::Bronze, Some(2)));
        assert_eq!(Rank::for_rating(1149.0), rank(Tier::Bronze, Some(1)));
        assert_eq!(Rank::for_rating(1150.0), rank(Tier::Silver, Some(3)));
        // A new player's rating is Gold 1.
        assert_eq!(Rank::for_rating(DEFAULT_RATING), rank(Tier::Gold, Some(1)));
        assert_eq!(Rank::for_rating(1450.0), rank(Tier::Emerald, Some(3)));
        assert_eq!(Rank::for_rating(1625.0), rank(Tier::Diamond, Some(3)));
        assert_eq!(Rank::for_rating(1999.0), rank(Tier::Diamond, Some(1)));
        assert_eq!(Rank::for_rating(2000.0), rank(Tier::Master, Some(3)));
        assert_eq!(Rank::for_rating(2249.0), rank(Tier::Master, Some(1)));
        assert_eq!(Rank::for_rating(2250.0), Rank::HIGHEST);
        assert_eq!(Rank::HIGHEST.tier(), Tier::Legendary);
        assert_eq!(Rank::HIGHEST.division(), None);
        assert_eq!(Rank::for_rating(3000.0), Rank::HIGHEST);
        for w in RANK_FLOORS.windows(2) {
            assert!(w[0] < w[1]);
        }
        for i in 0..RANK_FLOORS.len() as i16 {
            assert_eq!(Rank::from_index(i).unwrap().index(), i);
        }
        assert_eq!(Rank::from_index(19), None);
        assert_eq!(Rank::from_index(-1), None);
    }

    #[test]
    fn promotion_skips_divisions_and_demotion_waits_for_the_buffer() {
        let g3 = rank(Tier::Gold, Some(3)); // 1300
                                            // Up: the highest division reached.
        assert_eq!(held_rank(g3, 1460.0), rank(Tier::Emerald, Some(3)));
        let e3 = rank(Tier::Emerald, Some(3)); // 1450
                                               // Down: kept until 100 below the floor...
        assert_eq!(held_rank(e3, 1440.0), e3);
        assert_eq!(held_rank(e3, 1350.0), e3);
        // ...then one division (Gold 1, though the rating is already Gold 2's).
        assert_eq!(held_rank(e3, 1349.0), rank(Tier::Gold, Some(1)));
        // From Gold 1 (1400) the next drop needs below 1300.
        let g1 = rank(Tier::Gold, Some(1));
        assert_eq!(held_rank(g1, 1300.0), g1);
        assert_eq!(held_rank(g1, 1299.0), rank(Tier::Gold, Some(2)));
        // A collapse never shows a rank above what rating + buffer reaches.
        assert_eq!(held_rank(rank(Tier::Master, Some(3)), 1500.0), rank(Tier::Emerald, Some(1)));
        // Legendary keeps its buffer too.
        assert_eq!(held_rank(Rank::HIGHEST, 2160.0), Rank::HIGHEST);
        assert_eq!(held_rank(Rank::HIGHEST, 2140.0), rank(Tier::Master, Some(1)));
    }

    #[test]
    fn placement_takes_five_to_ten_sets() {
        // From scratch, even sets against established players: five.
        assert_eq!(sets_to_place(RD_START, 0), MIN_PLACEMENT_SETS);
        let mut rd = RD_START;
        for n in 0..MIN_PLACEMENT_SETS {
            assert!(!placed(rd, n));
            rd = rd_after(rd, 1400.0, 1400.0, RD_ESTABLISHED);
        }
        assert!(placed(rd, MIN_PLACEMENT_SETS));
        // Lopsided sets against other new players tell less: not placed by RD before ten.
        let mut rd = RD_START;
        for n in 0..MAX_PLACEMENT_SETS {
            assert!(!placed(rd, n), "set {n}");
            let left = sets_to_place(rd, n);
            assert!(left >= 1 && left <= MAX_PLACEMENT_SETS - n, "set {n}: {left}");
            rd = rd_after(rd, 1400.0, 1900.0, RD_START);
        }
        assert!(placed(rd, MAX_PLACEMENT_SETS));
        assert_eq!(sets_to_place(rd, MAX_PLACEMENT_SETS), 0);
        // The RD only falls, never below the floor.
        assert!(rd_after(100.0, 1400.0, 1400.0, 50.0) < 100.0);
        assert!(rd_after(RD_MIN, 1400.0, 1400.0, 50.0) >= RD_MIN);
    }

    #[test]
    fn a_player_places_then_holds_their_rank() {
        let mut st = TierState::default();
        let mut rating = DEFAULT_RATING;
        for n in 0..5 {
            let after = rating + 60.0;
            st = st.after_set(n, rating, after, (rating, RD_ESTABLISHED));
            rating = after;
            assert_eq!(st.rank.is_some(), n + 1 >= 5, "after set {}", n + 1);
        }
        // Placed at 1700: Diamond 3.
        assert_eq!(st.rank, Some(rank(Tier::Diamond, Some(3))));
        // A loss to 1610 keeps Diamond 3 (buffer); a fall to 1520 drops to Emerald 1.
        st = st.after_set(5, 1700.0, 1610.0, (1700.0, RD_ESTABLISHED));
        assert_eq!(st.rank, Some(rank(Tier::Diamond, Some(3))));
        st = st.after_set(6, 1610.0, 1520.0, (1610.0, RD_ESTABLISHED));
        assert_eq!(st.rank, Some(rank(Tier::Emerald, Some(1))));
    }

    #[test]
    fn replay_uses_the_opponents_rd_from_before_the_set() {
        let (a, b) = (uid(1), uid(2));
        let set = |id: i64, m: &str, me: Uuid, opp: Uuid, before: f64, after: f64, opp_r: f64, n: u32| RatedSet {
            id,
            uid: me,
            match_id: m.to_string(),
            opponent: Some(opp),
            rating_before: before,
            rating_after: after,
            opponent_rating: opp_r,
            sets_before: n,
        };
        let sets = vec![
            set(1, "m1", a, b, 1400.0, 1500.0, 1400.0, 0),
            set(2, "m1", b, a, 1400.0, 1300.0, 1400.0, 0),
            set(3, "m2", a, b, 1500.0, 1580.0, 1300.0, 1),
        ];
        let (rows, states) = replay(&sets);
        assert_eq!(rows.len(), 3);
        // Both players' first set saw the other at RD_START.
        let first = rd_after(RD_START, 1400.0, 1400.0, RD_START);
        assert!(close(rows[0].rd_after, first) && close(rows[1].rd_after, first));
        assert!(close(rows[2].rd_after, rd_after(first, 1500.0, 1300.0, first)));
        assert_eq!(rows[2].rank_after, None);
        assert!(close(states[&a].rd, rows[2].rd_after));
        assert!(close(states[&b].rd, first));
    }
}
