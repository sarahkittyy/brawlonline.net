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
//! after [`PLACEMENT_SETS`] rated sets, so "N more" counts down by one every set.
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

/// When production started rating with [`MIN_CHANGE`] (57e90fed, accounts restarted 2026-10-16
/// 06:15:39 UTC). Sets settled before it moved by `K * (score - expected)` alone, sometimes less
/// than a point; [`rerate`] replays them that way.
pub const MIN_CHANGE_SINCE_UNIX: i64 = 1_792_131_339;

/// [`updated`] under the rules of a set settled at `at`: without [`MIN_CHANGE`] before
/// [`MIN_CHANGE_SINCE_UNIX`].
pub fn updated_at(own: f64, sets_played: u32, opponent: f64, score: f64, at: DateTime<Utc>) -> f64 {
    if at.timestamp() < MIN_CHANGE_SINCE_UNIX {
        return own + k_factor(sets_played) * (score - expected(own, opponent));
    }
    updated(own, sets_played, opponent, score)
}

/// One stored rating change (a `rating_events` row) as [`rerate`] reads it.
#[derive(Debug, Clone, PartialEq)]
pub struct StoredChange {
    pub id: i64,
    pub uid: Uuid,
    pub match_id: String,
    pub opponent: Option<Uuid>,
    pub score: f64,
    pub rating_before: f64,
    pub rating_after: f64,
    pub opponent_rating: f64,
    pub k: f64,
    pub sets_before: u32,
    pub at: DateTime<Utc>,
}

/// What [`rerate`] computes for one rating change.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RecomputedChange {
    pub id: i64,
    pub rating_before: f64,
    pub rating_after: f64,
    pub opponent_rating: f64,
    pub k: f64,
    pub sets_before: u32,
}

impl RecomputedChange {
    /// Whether the stored row holds exactly these values.
    pub fn matches(&self, s: &StoredChange) -> bool {
        self.rating_before == s.rating_before
            && self.rating_after == s.rating_after
            && self.opponent_rating == s.opponent_rating
            && self.k == s.k
            && self.sets_before == s.sets_before
    }
}

/// Re-rates the rating changes `window` (every change settled from some point on, in settle order:
/// `created_at`, then `id`) as if the changes in `removed` (those of one voided set, which must be
/// in `window`) had never been made. Elo depends on the order of sets, so every later set of the
/// voided set's players changes, and through them their opponents' later sets, and so on.
///
/// Each player starts from their first change in the window (a voided set's players from before
/// it). A player who has no change of their own in a set (the other player of an abandon before any
/// game, settled before 2026-10-23 when such a set rated only the leaver) stands at the stored opponent rating until a change of theirs is replayed. Both changes of
/// a set use the ratings from before the set, as `settle` does.
///
/// Returns every change of `window` not in `removed`, in order, and each player's rating and
/// rated-set count after the window. With `removed` empty it reproduces the stored rows exactly
/// when the history follows today's rules ([`updated_at`]), which callers check first.
pub fn rerate(window: &[StoredChange], removed: &[i64]) -> (Vec<RecomputedChange>, HashMap<Uuid, Standing>) {
    let mut state: HashMap<Uuid, Standing> = HashMap::new();
    for s in window.iter().filter(|s| removed.contains(&s.id)) {
        state.insert(s.uid, Standing { rating: s.rating_before, sets_played: s.sets_before });
    }
    let mut out = Vec::with_capacity(window.len());
    let mut i = 0;
    while i < window.len() {
        let mut j = i + 1;
        while j < window.len() && window[j].match_id == window[i].match_id {
            j += 1;
        }
        let group: Vec<&StoredChange> = window[i..j].iter().filter(|s| !removed.contains(&s.id)).collect();
        for s in &group {
            state.entry(s.uid).or_insert(Standing { rating: s.rating_before, sets_played: s.sets_before });
        }
        let before: HashMap<Uuid, Standing> = group
            .iter()
            .flat_map(|s| [Some(s.uid), s.opponent])
            .flatten()
            .filter_map(|u| state.get(&u).map(|st| (u, *st)))
            .collect();
        for s in group {
            let me = before[&s.uid];
            let opponent = s.opponent.and_then(|o| before.get(&o)).map(|o| o.rating).unwrap_or(s.opponent_rating);
            let after = updated_at(me.rating, me.sets_played, opponent, s.score, s.at);
            out.push(RecomputedChange {
                id: s.id,
                rating_before: me.rating,
                rating_after: after,
                opponent_rating: opponent,
                k: k_factor(me.sets_played),
                sets_before: me.sets_played,
            });
            state.insert(s.uid, Standing { rating: after, sets_played: me.sets_played + 1 });
        }
        i = j;
    }
    (out, state)
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

/// The end reason of a set left before the stage select with no game played (not rated, and not
/// in the match history).
pub const LEFT_BEFORE_STAGE_SELECT: &str = "left before the stage select";

/// The end reason of a set whose game was aborted for its match quality ([`Leave::MatchQuality`]).
pub const MATCH_QUALITY_LOW: &str = "match quality low";

/// The end reason of a set whose connection broke with nobody to blame.
pub const CONNECTION_BROKE: &str = "the connection between the players broke";

/// What a game says made its match quality too low ([`LeaveReport::cause`]): where most of the
/// debt of its bad intervals came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cause {
    /// This PC missed its own frame schedule (it could not run the game at full speed). The only
    /// cause that blames anyone on a game's word: its own player.
    OwnPc,
    /// The other player's PC said it missed its own schedule.
    PeerPc,
    /// The link: waits for the other player's inputs, or a high round trip.
    Link,
}

impl Cause {
    pub fn parse(s: &str) -> Option<Cause> {
        match s {
            "self" => Some(Cause::OwnPc),
            "peer" => Some(Cause::PeerPc),
            "link" => Some(Cause::Link),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Cause::OwnPc => "self",
            Cause::PeerPc => "peer",
            Cause::Link => "link",
        }
    }
}

/// How a set that one player's own end ended counts against them ([`Outcome::Abandoned`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Fault {
    /// Their PC or connection made the match quality so low that the game was aborted.
    MatchQuality,
    /// Their connection went (mm saw it go silent), and the players' link with it.
    Connection,
}

impl Fault {
    /// The end reason's tail: `abandoned by <uid>: <this>`.
    pub fn as_str(self) -> &'static str {
        match self {
            Fault::MatchQuality => MATCH_QUALITY_LOW,
            Fault::Connection => CONNECTION_LOST,
        }
    }
}

/// [`Fault::Connection`]'s end reason tail.
pub const CONNECTION_LOST: &str = "connection lost";

/// A trouble or an abort counts against the player whose own end caused it as a set loss only
/// after the first [`EARLY_SECONDS`] of the set's first game (the user's, 2026-10-29: "games that
/// are slow from the start of game 1 are obviously like, not intentional ... maybe after 10-15s
/// after the start of the first game that it becomes potentially intentional"). Before that, or
/// when a game says the set's first game was already poor in that time, the set is void, but it
/// still counts toward their cooldown ([`COOLDOWN_FAULTS`]). Dolphin decides it (the reports'
/// `early`).
pub const EARLY_SECONDS: u32 = 15;

/// Faults ([`Outcome::fault`]) in [`COOLDOWN_WINDOW`] that keep a player out of Ranked for
/// [`COOLDOWN`] after the last of them (the user's, 2026-10-29: "a ranked cooldown after 3 low
/// quality aborts, only when we know for a fact its their fault").
pub const COOLDOWN_FAULTS: i64 = 3;
pub const COOLDOWN_WINDOW: Duration = Duration::hours(24);
pub const COOLDOWN: Duration = Duration::hours(1);

/// How far before and after a set's first abort or "opponent left" mm's record of a player's
/// connection trouble ([`NetTrouble`]) counts. An abort comes once its debt has built up (20 s at
/// a steady 52 frames a second, longer with healthy intervals between), an "opponent left" about
/// 7 s after the link went; the reports reach the server a moment later.
pub const TROUBLE_BEFORE: Duration = Duration::seconds(90);
pub const TROUBLE_AFTER: Duration = Duration::seconds(10);
/// The least trouble on a player's connection in that window that blames them (a short blip on a
/// Wi-Fi is nobody's doing); the other player's must be at most a quarter of it.
pub const TROUBLE_MIN: Duration = Duration::seconds(2);

/// A client's report that the set ended early.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Leave {
    /// The reporter left (held Z, closed the menu): they abandoned the set.
    Left,
    /// The reporter's opponent disconnected or went silent.
    OpponentLeft,
    /// The game was aborted for its match quality, by either player's game (too slow, or too high
    /// a round trip, for too long: Slippi's poor-performance termination, the user's, 2026-10-29).
    /// Both games report it, each with its [`Cause`].
    MatchQuality,
}

impl Leave {
    pub fn parse(s: &str) -> Option<Leave> {
        match s {
            "left" => Some(Leave::Left),
            "opponent_left" => Some(Leave::OpponentLeft),
            "match_quality" => Some(Leave::MatchQuality),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Leave::Left => "left",
            Leave::OpponentLeft => "opponent_left",
            Leave::MatchQuality => "match_quality",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct LeaveReport {
    pub reporter: Uuid,
    pub kind: Leave,
    pub at: DateTime<Utc>,
    /// A `Left` made before the reporter's game was sent to the set's stage select.
    pub before_stage_select: bool,
    /// An `OpponentLeft` or `MatchQuality` in the first [`EARLY_SECONDS`] of the set's first game
    /// or before it, or after a first game whose start was already poor: nobody loses the set for
    /// it. `None`: a game from before 2026-10-29 that does not say, taken as early.
    pub early: Option<bool>,
    /// A `MatchQuality`'s cause as the reporter's game saw it (`None`: an older game).
    pub cause: Option<Cause>,
}

/// mm's record of trouble on one player's own connection while in a Ranked set
/// (`ranked_net_events`): their game went silent on its connection to mm (the network went, or the
/// game stopped running), or their round trip to mm jumped. mm is a third party: trouble only one
/// player's connection had is that player's own.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct NetTrouble {
    pub player: Uuid,
    pub from: DateTime<Utc>,
    pub to: DateTime<Utc>,
}

/// What mm saw of a set's games (`ranked_presence`, `ranked_game_results`): Ranked games are
/// always streamed, the other player's game a witness, and each player's game tells mm how each
/// game ended. A lone report is checked against it.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Seen {
    /// Whether each player's game is connected to mm. A player missing here is unknown (mm
    /// recorded nothing: a set from before this, or from a test without mm).
    pub online: Vec<(Uuid, bool)>,
    /// A player's game that stopped running while it stayed connected to mm (bug report #105,
    /// 2026-10-22: frozen, its emulation stopped), and since when: its last `game-alive`. mm marks
    /// a game so once it sent no `game-alive` for its `game_stall` (30 s); a game that runs again
    /// is no longer here. Builds without `game-alive` are never here.
    pub stalled: Vec<(Uuid, DateTime<Utc>)>,
    /// How games ended as each player's own game told mm (`spectate-end`): only Ranked games that
    /// ended by a game set or a quit.
    pub games: Vec<SeenGame>,
    /// Trouble on each player's connection to mm.
    pub trouble: Vec<NetTrouble>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SeenGame {
    /// As [`GameReport::game_index`].
    pub game_index: u32,
    /// Whose game said it.
    pub player: Uuid,
    /// `None` for a draw.
    pub winner: Option<Uuid>,
}

impl Seen {
    /// The player's game is known to be connected to mm.
    fn online(&self, player: Uuid) -> bool {
        self.online.iter().any(|(p, on)| *p == player && *on)
    }

    /// The player's game had already stopped running at `at` (and still has): frozen when a
    /// report about it was made.
    fn stalled_by(&self, player: Uuid, at: DateTime<Utc>) -> bool {
        self.stalled.iter().any(|(p, since)| *p == player && *since <= at)
    }

    /// The one player of `players` whose connection mm saw in trouble around `at`
    /// ([`TROUBLE_BEFORE`], [`TROUBLE_AFTER`]): [`TROUBLE_MIN`] of it or more, and the other
    /// player's at most a quarter of theirs. None otherwise: both had trouble, or neither much.
    fn troubled_alone(&self, players: [Uuid; 2], at: DateTime<Utc>) -> Option<usize> {
        let (lo, hi) = (at - TROUBLE_BEFORE, at + TROUBLE_AFTER);
        let length = |p: Uuid| -> Duration {
            self.trouble
                .iter()
                .filter(|t| t.player == p && t.from < hi && t.to > lo)
                .map(|t| t.to.min(hi) - t.from.max(lo))
                .fold(Duration::zero(), |sum, d| sum + d)
        };
        let len = [length(players[0]), length(players[1])];
        (0..2).find(|&p| len[p] >= TROUBLE_MIN && len[1 - p] * 4 <= len[p])
    }

    /// The winner of `game` as `player`'s game told mm.
    fn result(&self, game: u32, player: Uuid) -> Option<Option<Uuid>> {
        self.games.iter().find(|g| g.game_index == game && g.player == player).map(|g| g.winner)
    }
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
    /// A player abandoned the set before it was decided. The leaver takes a set loss and the other
    /// player the win, also before any game was played (the user's, 2026-10-23: quitting game 1
    /// gave the one who stayed nothing). `fault`: the leaver did not leave, their own PC or
    /// connection ended the set after its first [`EARLY_SECONDS`]; it also counts toward their
    /// cooldown.
    Abandoned { leaver: Uuid, other: Uuid, wins: [u32; 2], fault: Option<Fault> },
    /// No rating change. `status` is the `mm_matches.status` (Slippi's set states):
    /// `TERMINATED` (the connection broke, or the match quality was too low), `ERROR` (the reports
    /// disagree; held for review), `ORPHANED` (nothing more was reported). `fault`: the player
    /// whose own PC or connection did it, early in the set: nobody is rated, but it counts toward
    /// their cooldown.
    Void { status: &'static str, reason: String, wins: [u32; 2], fault: Option<Uuid> },
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

    /// The player whose own PC or connection ended the set, as far as is known for a fact
    /// (`mm_matches.fault`): counted toward their Ranked cooldown.
    pub fn fault(&self) -> Option<Uuid> {
        match self {
            Outcome::Abandoned { leaver, fault: Some(_), .. } => Some(*leaver),
            Outcome::Void { fault, .. } => *fault,
            _ => None,
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
///   only one client reported counts on that report when the other player's game told mm the
///   same result (`seen`; another result: `ERROR`), or once `report_grace` has passed while the
///   other player's game is not connected to mm (it crashed or was closed after the game; also
///   when mm recorded nothing), or at once when the other player's game had stopped running
///   (`seen.stalled`) before the report: connected to mm, but frozen. While the other's game is
///   connected, running and said nothing, the lone report waits: it may be for a game still being
///   played. Waiting `stale_after` voids the set as `ORPHANED`. Games count in order: a game
///   still waiting for its second report holds back the ones after it. A draw counts for nobody
///   and is replayed.
/// - The first player to [`WINS_NEEDED`] counted games wins the set.
/// - Otherwise a leave decides it, the other player winning the set even with no game played: a
///   player who reports [`Leave::Left`] abandoned it (unless it was before the stage select with
///   no game played: void, nobody rated). A player
///   whose opponent reports [`Leave::OpponentLeft`] and who does not report the same within
///   `report_grace` abandoned it (their game crashed or was closed). Both reporting
///   `OpponentLeft` is a broken connection: void as `TERMINATED`, unless mm saw only one
///   player's own connection go then ([`NetTrouble`]): the set ended on that player's end. An
///   `OpponentLeft` about a player whose game is still connected to mm and running blames nobody
///   (it may be untrue) unless mm saw that player's connection go; the set is orphaned if
///   nothing more comes. One about a player whose game had stopped running before it was made
///   abandons the set for that player at once: a frozen game can't report anything, and one that
///   runs again is no longer stalled.
/// - A [`Leave::MatchQuality`] from either player (their game aborted the set's game for its
///   match quality, and the other's ended with it) ends the set, whatever the games before it
///   were. It ended on a player's own end when their own game says its PC could not keep up
///   ([`Cause::OwnPc`]) or mm saw only their connection in trouble; without either it waits a
///   little for the other game's report (its cause), then is void as `TERMINATED` with nobody
///   rated (the user's, 2026-10-29: "no loss to either player"). Not when a player had left before
///   it: that player abandoned the set.
/// - A set that ended on one player's own end (their PC or connection, known for a fact) is their
///   loss, [`Outcome::Abandoned`] with a `fault`, unless it was early in the set
///   ([`EARLY_SECONDS`]): then void with that `fault`. Either counts toward their cooldown (the
///   user's, 2026-10-29: a player losing a set must not get it voided by slowing their game down
///   or pulling the cable).
/// - A set with no report for `stale_after` is void as `ORPHANED`.
///
/// Reports from anyone but the two players, and winners who are not one of them, are ignored
/// (the report endpoint refuses them anyway).
pub fn resolve(
    players: [Uuid; 2],
    created: DateTime<Utc>,
    games: &[GameReport],
    leaves: &[LeaveReport],
    seen: &Seen,
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
                    fault: None,
                };
            }
            [Some(a), Some(_)] => a.winner,
            [Some(r), None] | [None, Some(r)] => {
                let other = players[1 - idx(r.reporter).unwrap()];
                match seen.result(game, other) {
                    Some(w) if w == r.winner => r.winner,
                    Some(_) => {
                        return Outcome::Void {
                            status: "ERROR",
                            reason: format!("the other player's game saw another winner for game {game}"),
                            wins,
                            fault: None,
                        };
                    }
                    None if seen.stalled_by(other, r.at) => r.winner,
                    None if !seen.online(other) && now - r.at >= timing.report_grace => r.winner,
                    None if now - r.at >= timing.stale_after => {
                        return Outcome::Void {
                            status: "ORPHANED",
                            reason: format!("only one player reported game {game}"),
                            wins,
                            fault: None,
                        };
                    }
                    None => {
                        waiting = true;
                        break;
                    }
                }
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
    let left = [reported(0, Leave::Left), reported(1, Leave::Left)];
    let first_left = left.iter().flatten().map(|l| l.at).min();
    let abandon = |leaver: usize, fault: Option<Fault>| Outcome::Abandoned {
        leaver: players[leaver],
        other: players[1 - leaver],
        wins,
        fault,
    };
    // The set's first abort or "opponent left": about when the trouble came. Whether that was
    // early in the set: any report says so, or does not say (an older game).
    let ended = leaves.iter().filter(|l| l.kind != Leave::Left);
    let trouble_at = ended.clone().map(|l| l.at).min();
    let early = ended.clone().any(|l| l.early != Some(false));
    // The one player whose connection mm saw in trouble then.
    let troubled = trouble_at.and_then(|at| seen.troubled_alone(players, at));
    // The set ended on player `p`'s own end: their loss, or early in the set a void; it counts
    // toward their cooldown either way.
    let on_end = |p: usize, fault: Fault, reason: &str| {
        if early {
            Outcome::Void { status: "TERMINATED", reason: reason.into(), wins, fault: Some(players[p]) }
        } else {
            abandon(p, Some(fault))
        }
    };

    let quality: Vec<&LeaveReport> = leaves.iter().copied().filter(|l| l.kind == Leave::MatchQuality).collect();
    if let Some(at) = quality.iter().map(|l| l.at).min().filter(|at| first_left.is_none_or(|l| *at <= l)) {
        // A game that says its own PC could not keep up blames its own player, and mm's record of
        // one player's connection trouble that player. Nothing else blames anyone.
        let own_pc = |p: usize| quality.iter().any(|l| l.reporter == players[p] && l.cause == Some(Cause::OwnPc));
        let at_fault = match (own_pc(0), own_pc(1)) {
            (true, false) => Some(0),
            (false, true) => Some(1),
            (true, true) => None,
            (false, false) => troubled,
        };
        if let Some(p) = at_fault {
            return on_end(p, Fault::MatchQuality, MATCH_QUALITY_LOW);
        }
        // The other game's own report, normally a moment behind, may yet say its PC was the slow
        // one: it waits for that.
        if let Some(p) = (0..2).find(|&p| !leaves.iter().any(|l| l.reporter == players[p])) {
            if now - at < timing.report_grace {
                return Outcome::InProgress { wins };
            }
            // It never came, and that game is gone from mm, whose PC the other game saw fall behind.
            let peer_pc = quality.iter().any(|l| l.reporter == players[1 - p] && l.cause == Some(Cause::PeerPc));
            if peer_pc && !seen.online(players[p]) {
                return on_end(p, Fault::MatchQuality, MATCH_QUALITY_LOW);
            }
        }
        return Outcome::Void { status: "TERMINATED", reason: MATCH_QUALITY_LOW.into(), wins, fault: None };
    }
    let opp_left = [reported(0, Leave::OpponentLeft), reported(1, Leave::OpponentLeft)];
    let played = wins.iter().sum::<u32>() > 0 || games.iter().any(|g| g.winner.is_none());
    // A player who left before their game was sent to the stage select, with no game played,
    // abandoned nothing: the search was cancelled as the match was made, or the connection never
    // came up (bug report #37, 2026-10-16). Nobody is rated.
    let left_early = |l: &LeaveReport| {
        (!played && l.before_stage_select).then(|| Outcome::Void {
            status: "TERMINATED",
            reason: LEFT_BEFORE_STAGE_SELECT.into(),
            wins,
            fault: None,
        })
    };
    match left {
        [Some(_), Some(_)] => {
            return Outcome::Void { status: "TERMINATED", reason: "both players left".into(), wins, fault: None };
        }
        [Some(l), None] => return left_early(l).unwrap_or_else(|| abandon(0, None)),
        [None, Some(l)] => return left_early(l).unwrap_or_else(|| abandon(1, None)),
        [None, None] => {}
    }
    // A player whose connection mm saw go (and the other's not) lost the link on their own end.
    let lost_link = |p: usize| (troubled == Some(p)).then_some(Fault::Connection);
    match opp_left {
        [Some(_), Some(_)] => {
            return match troubled {
                Some(p) => on_end(p, Fault::Connection, CONNECTION_BROKE),
                None => Outcome::Void { status: "TERMINATED", reason: CONNECTION_BROKE.into(), wins, fault: None },
            };
        }
        // Player 0 says player 1 left; player 1 has had `report_grace` to say the same, and their
        // game is not connected to mm. A player whose game is still connected did not leave (or
        // the report is untrue): nobody is blamed, and the set is orphaned if nothing more comes,
        // unless mm saw that player's connection go (it came back). Unless that game had already
        // stopped running when the report was made (bug report #105: frozen 19 minutes with its
        // mm connection up): abandoned at once.
        [Some(r), None] if seen.stalled_by(players[1], r.at) => return abandon(1, None),
        [None, Some(r)] if seen.stalled_by(players[0], r.at) => return abandon(0, None),
        [Some(_), None] if seen.online(players[1]) && troubled == Some(1) => {
            return on_end(1, Fault::Connection, CONNECTION_BROKE);
        }
        [None, Some(_)] if seen.online(players[0]) && troubled == Some(0) => {
            return on_end(0, Fault::Connection, CONNECTION_BROKE);
        }
        [Some(_), None] if seen.online(players[1]) => {}
        [None, Some(_)] if seen.online(players[0]) => {}
        [Some(r), None] if now - r.at >= timing.report_grace => return abandon(1, lost_link(1)),
        [None, Some(r)] if now - r.at >= timing.report_grace => return abandon(0, lost_link(0)),
        [Some(_), None] | [None, Some(_)] => return Outcome::InProgress { wins },
        [None, None] => {}
    }

    let last = games.iter().map(|g| g.at).chain(leaves.iter().map(|l| l.at)).max().unwrap_or(created).max(created);
    if now - last >= timing.stale_after {
        return Outcome::Void { status: "ORPHANED", reason: "no result was reported".into(), wins, fault: None };
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
        Outcome::Abandoned { other, .. } => players.map(|p| Some(score_of(*other, p))),
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
/// §2). A change here applies to the stored ranks when accounts next starts (`backfill_tiers`).
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
pub const DEMOTION_BUFFER: f64 = 50.0;

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

/// The rating deviation (Glicko-1) of a player who has not played a ranked set. The RD is kept and
/// stored beside each rating but decides nothing today (placement until 2026-10-16; possibly
/// re-placing players back from a long break later).
pub const RD_START: f64 = 350.0;
/// The lowest RD kept, so a veteran's opponents still learn something from a set.
pub const RD_MIN: f64 = 30.0;
/// The rated sets a player plays before they get a rank.
pub const PLACEMENT_SETS: u32 = 5;

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

/// Whether a player is placed after `sets_played` rated sets.
pub fn placed(sets_played: u32) -> bool {
    sets_played >= PLACEMENT_SETS
}

/// Rated sets a player still needs to place (0 once placed): one fewer after every set.
pub fn sets_to_place(sets_played: u32) -> u32 {
    PLACEMENT_SETS.saturating_sub(sets_played)
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
        let rank = if !placed(sets_before + 1) {
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
        seen: Seen,
    }

    impl Set {
        fn new() -> Set {
            Set {
                p: [uid(1), uid(2)],
                t0: DateTime::parse_from_rfc3339("2026-10-08T12:00:00Z").unwrap().into(),
                games: vec![],
                leaves: vec![],
                seen: Seen::default(),
            }
        }
        /// Whether player `p`'s game is connected to mm.
        fn online(&mut self, p: usize, on: bool) -> &mut Self {
            let u = self.p[p];
            self.seen.online.retain(|(x, _)| *x != u);
            self.seen.online.push((u, on));
            self
        }
        /// Player `p`'s game stopped running (still connected to mm) at minute `min`.
        fn stalled(&mut self, p: usize, min: i64) -> &mut Self {
            let (u, since) = (self.p[p], self.at(min));
            self.seen.stalled.push((u, since));
            self
        }
        /// Player `by`'s game told mm game `g` was won by `w` (None: draw).
        fn told_mm(&mut self, by: usize, g: u32, w: Option<usize>) -> &mut Self {
            self.seen.games.push(SeenGame { game_index: g, player: self.p[by], winner: w.map(|i| self.p[i]) });
            self
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
            let r = LeaveReport {
                reporter: self.p[by],
                kind,
                at: self.at(min),
                before_stage_select: false,
                early: Some(false),
                cause: None,
            };
            self.leaves.push(r);
            self
        }
        /// Player `by` leaves before their game was sent to the stage select.
        fn leave_early(&mut self, by: usize, min: i64) -> &mut Self {
            let r = LeaveReport {
                reporter: self.p[by],
                kind: Leave::Left,
                at: self.at(min),
                before_stage_select: true,
                early: None,
                cause: None,
            };
            self.leaves.push(r);
            self
        }
        /// Player `by`'s game reports a leave `kind` (not `Left`) at second `sec`, saying its cause
        /// (`MatchQuality`) and whether it came early in the set.
        fn report(&mut self, by: usize, kind: Leave, cause: Option<Cause>, early: bool, sec: i64) -> &mut Self {
            let r = LeaveReport {
                reporter: self.p[by],
                kind,
                at: self.t0 + Duration::seconds(sec),
                before_stage_select: false,
                early: Some(early),
                cause,
            };
            self.leaves.push(r);
            self
        }
        /// mm saw player `p`'s connection in trouble from second `from` to `to`.
        fn trouble(&mut self, p: usize, from: i64, to: i64) -> &mut Self {
            let t = NetTrouble {
                player: self.p[p],
                from: self.t0 + Duration::seconds(from),
                to: self.t0 + Duration::seconds(to),
            };
            self.seen.trouble.push(t);
            self
        }
        fn at_min(&self, min: i64) -> Outcome {
            resolve(self.p, self.t0, &self.games, &self.leaves, &self.seen, self.at(min), Timing::default())
        }
        fn at_sec(&self, sec: i64) -> Outcome {
            let now = self.t0 + Duration::seconds(sec);
            resolve(self.p, self.t0, &self.games, &self.leaves, &self.seen, now, Timing::default())
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

    /// The cheat this closes: a player reports their own wins as the set starts. The other
    /// player's game is connected to mm and still playing, so the reports wait for it.
    #[test]
    fn a_lone_report_waits_while_the_other_players_game_is_connected() {
        let mut s = Set::new();
        s.online(0, true).online(1, true).game(0, 1, Some(0), 0).game(0, 2, Some(0), 0);
        assert_eq!(s.at_min(5), Outcome::InProgress { wins: [0, 0] });
        // The other player's game ends game 1 the other way: their game's word to mm is enough,
        // before their own report comes.
        s.told_mm(1, 1, Some(1));
        let o = s.at_min(6);
        assert!(matches!(o, Outcome::Void { status: "ERROR", .. }), "{o:?}");
        // With nothing more, the set is orphaned: nobody is rated.
        let mut s = Set::new();
        s.online(0, true).online(1, true).game(0, 1, Some(0), 0);
        assert_eq!(s.at_min(29), Outcome::InProgress { wins: [0, 0] });
        let o = s.at_min(30);
        assert!(matches!(o, Outcome::Void { status: "ORPHANED", .. }), "{o:?}");
        assert_eq!(scores(s.p, &o), [None, None]);
    }

    #[test]
    fn a_lone_report_counts_when_the_other_players_game_agrees_or_is_gone() {
        // The other player's game told mm the same result: counts at once.
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(0), 5).game(0, 2, Some(0), 10).told_mm(1, 2, Some(0));
        assert_eq!(s.at_min(10), Outcome::Complete { winner: s.p[0], loser: s.p[1], wins: [2, 0] });
        // A draw it agrees on is replayed as any draw.
        let mut s = Set::new();
        s.online(0, true).online(1, true).game(0, 1, None, 5).told_mm(1, 1, None);
        assert_eq!(s.at_min(5), Outcome::InProgress { wins: [0, 0] });
        // The other player's game left mm (closed, crashed): the grace period, as before.
        let mut s = Set::new();
        s.online(0, true).online(1, false).both(1, Some(0), 5).game(0, 2, Some(0), 10);
        assert_eq!(s.at_min(11), Outcome::InProgress { wins: [1, 0] });
        assert_eq!(s.at_min(12), Outcome::Complete { winner: s.p[0], loser: s.p[1], wins: [2, 0] });
        // What the reporter's own game told mm proves nothing.
        let mut s = Set::new();
        s.online(0, true).online(1, true).game(0, 1, Some(0), 0).told_mm(0, 1, Some(0));
        assert_eq!(s.at_min(5), Outcome::InProgress { wins: [0, 0] });
    }

    #[test]
    fn opponent_left_about_a_connected_player_blames_nobody() {
        let mut s = Set::new();
        s.online(0, true).online(1, true).leave(0, Leave::OpponentLeft, 0);
        assert_eq!(s.at_min(5), Outcome::InProgress { wins: [0, 0] });
        let o = s.at_min(30);
        assert!(matches!(o, Outcome::Void { status: "ORPHANED", .. }), "{o:?}");
        // Gone from mm: abandoned after the grace period, as before.
        let mut s = Set::new();
        s.online(0, true).online(1, false).both(1, Some(0), 1).leave(0, Leave::OpponentLeft, 2);
        assert_eq!(s.at_min(3), Outcome::InProgress { wins: [1, 0] });
        assert_eq!(s.at_min(4), Outcome::Abandoned { leaver: s.p[1], other: s.p[0], wins: [1, 0], fault: None });
    }

    /// Bug report #105 (2026-10-22): the opponent's game froze 18 s into game 2 and stayed
    /// connected to mm for 19 minutes; the set waited all that time (and was orphaned had the
    /// window stayed open 30). A game that had stopped running when the report was made is gone.
    #[test]
    fn opponent_left_about_a_frozen_game_abandons_at_once() {
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(0), 1).stalled(1, 4).leave(0, Leave::OpponentLeft, 4);
        assert_eq!(s.at_min(4), Outcome::Abandoned { leaver: s.p[1], other: s.p[0], wins: [1, 0], fault: None });
        // Either player.
        let mut s = Set::new();
        s.online(0, true).online(1, true).stalled(0, 2).leave(1, Leave::OpponentLeft, 3);
        assert_eq!(s.at_min(3), Outcome::Abandoned { leaver: s.p[0], other: s.p[1], wins: [0, 0], fault: None });
        // A game that stopped running only after the report was running when it was made: the
        // report may be untrue, so nobody is blamed, as for any connected game.
        let mut s = Set::new();
        s.online(0, true).online(1, true).leave(0, Leave::OpponentLeft, 4).stalled(1, 5);
        assert_eq!(s.at_min(6), Outcome::InProgress { wins: [0, 0] });
        assert!(matches!(s.at_min(34), Outcome::Void { status: "ORPHANED", .. }));
        // Its own "opponent left" (it ran again, saw the link gone) is the broken connection.
        let mut s = Set::new();
        s.online(0, true).online(1, true).stalled(1, 4).leave(0, Leave::OpponentLeft, 4);
        s.leave(1, Leave::OpponentLeft, 5);
        assert!(matches!(s.at_min(5), Outcome::Void { status: "TERMINATED", .. }));
    }

    #[test]
    fn a_lone_report_counts_at_once_when_the_other_players_game_is_frozen() {
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(0), 5).stalled(1, 9).game(0, 2, Some(0), 10);
        assert_eq!(s.at_min(10), Outcome::Complete { winner: s.p[0], loser: s.p[1], wins: [2, 0] });
        // Frozen only after the report: it waits, as for a running game.
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(0), 5).game(0, 2, Some(0), 10).stalled(1, 11);
        assert_eq!(s.at_min(12), Outcome::InProgress { wins: [1, 0] });
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
        assert_eq!(o, Outcome::Abandoned { leaver: s.p[0], other: s.p[1], wins: [1, 0], fault: None });
        assert_eq!(o.status(), "ABANDONED");
        assert_eq!(scores(s.p, &o), [Some(0.0), Some(1.0)]);
        // Before any game (quitting game 1, or leaving once the opponent's name shows): the leaver
        // still loses, and the other player wins.
        let mut s = Set::new();
        s.leave(1, Leave::Left, 1);
        let o = s.at_min(1);
        assert_eq!(o, Outcome::Abandoned { leaver: s.p[1], other: s.p[0], wins: [0, 0], fault: None });
        assert_eq!(scores(s.p, &o), [Some(1.0), Some(0.0)]);
    }

    #[test]
    fn opponent_left_needs_the_grace_period_and_no_counter_report() {
        // Player 1 says player 0 left; player 0 says nothing: player 0 abandoned.
        let mut s = Set::new();
        s.both(1, Some(1), 5).leave(1, Leave::OpponentLeft, 7);
        assert_eq!(s.at_min(8), Outcome::InProgress { wins: [0, 1] });
        assert_eq!(s.at_min(9), Outcome::Abandoned { leaver: s.p[0], other: s.p[1], wins: [0, 1], fault: None });
        // Both say the other left: the connection broke, nobody is blamed.
        let mut s = Set::new();
        s.both(1, Some(1), 5).leave(1, Leave::OpponentLeft, 7).leave(0, Leave::OpponentLeft, 7);
        let o = s.at_min(7);
        assert!(matches!(o, Outcome::Void { status: "TERMINATED", .. }), "{o:?}");
        // Their own "left" settles it at once.
        let mut s = Set::new();
        s.leave(1, Leave::OpponentLeft, 7).leave(0, Leave::Left, 7);
        assert_eq!(s.at_min(7), Outcome::Abandoned { leaver: s.p[0], other: s.p[1], wins: [0, 0], fault: None });
    }

    #[test]
    fn leaving_before_the_stage_select_rates_nobody() {
        // Bug report #37: the search cancelled 0.2 s after the match was made; the opponent's
        // "opponent left" came 22 s later.
        let mut s = Set::new();
        s.leave_early(0, 0).leave(1, Leave::OpponentLeft, 1);
        let o = s.at_min(1);
        assert_eq!(
            o,
            Outcome::Void { status: "TERMINATED", reason: LEFT_BEFORE_STAGE_SELECT.into(), wins: [0, 0], fault: None }
        );
        assert_eq!(scores(s.p, &o), [None, None]);
        // Settled at once, before the opponent says anything.
        let mut s = Set::new();
        s.leave_early(1, 0);
        assert!(matches!(s.at_min(0), Outcome::Void { status: "TERMINATED", .. }));
        // After a game it is an abandon as any other leave (a later game's CSS is past the stage
        // select anyway).
        let mut s = Set::new();
        s.both(1, Some(0), 5).leave_early(1, 6);
        assert_eq!(s.at_min(6), Outcome::Abandoned { leaver: s.p[1], other: s.p[0], wins: [1, 0], fault: None });
        // A drawn game counts as played.
        let mut s = Set::new();
        s.both(1, None, 5).leave_early(1, 6);
        assert_eq!(s.at_min(6), Outcome::Abandoned { leaver: s.p[1], other: s.p[0], wins: [0, 0], fault: None });
        // Without the leaver's own report (an old client, a crash) the opponent's report decides,
        // as before.
        let mut s = Set::new();
        s.leave(1, Leave::OpponentLeft, 1);
        assert_eq!(s.at_min(3), Outcome::Abandoned { leaver: s.p[0], other: s.p[1], wins: [0, 0], fault: None });
    }

    #[test]
    fn an_abort_for_match_quality_voids_the_set() {
        // Bug report #182: game 1 won by player 1, game 2 aborted 20 s in; both games say so.
        let mut s = Set::new();
        s.both(1, Some(1), 5).leave(0, Leave::MatchQuality, 7).leave(1, Leave::MatchQuality, 7);
        let o = s.at_min(7);
        assert_eq!(
            o,
            Outcome::Void { status: "TERMINATED", reason: MATCH_QUALITY_LOW.into(), wins: [0, 1], fault: None }
        );
        assert_eq!(scores(s.p, &o), [None, None]);
        // One player's report waits a little for the other's (its cause may blame its own PC),
        // then is enough (the other's abort never came, its game saw the connection go).
        let mut s = Set::new();
        s.leave(1, Leave::MatchQuality, 2);
        assert_eq!(s.at_min(3), Outcome::InProgress { wins: [0, 0] });
        assert_eq!(
            s.at_min(4),
            Outcome::Void { status: "TERMINATED", reason: MATCH_QUALITY_LOW.into(), wins: [0, 0], fault: None }
        );
        let mut s = Set::new();
        s.leave(0, Leave::OpponentLeft, 2).leave(1, Leave::MatchQuality, 2);
        assert_eq!(
            s.at_min(2),
            Outcome::Void { status: "TERMINATED", reason: MATCH_QUALITY_LOW.into(), wins: [0, 0], fault: None }
        );
        // A player who left before it abandoned the set; a "left" after it changes nothing.
        let mut s = Set::new();
        s.leave(0, Leave::Left, 2).leave(1, Leave::MatchQuality, 3);
        assert_eq!(s.at_min(3), Outcome::Abandoned { leaver: s.p[0], other: s.p[1], wins: [0, 0], fault: None });
        let mut s = Set::new();
        s.leave(1, Leave::MatchQuality, 2).leave(0, Leave::Left, 3);
        assert!(matches!(s.at_min(3), Outcome::Void { status: "TERMINATED", .. }));
        // A set decided before it stays decided.
        let mut s = Set::new();
        s.both(1, Some(0), 5).both(2, Some(0), 10).leave(1, Leave::MatchQuality, 11);
        assert_eq!(s.at_min(11), Outcome::Complete { winner: s.p[0], loser: s.p[1], wins: [2, 0] });
        assert_eq!(Leave::parse("match_quality"), Some(Leave::MatchQuality));
        assert_eq!(Leave::MatchQuality.as_str(), "match_quality");
    }

    /// The user's, 2026-10-29: "if the game starts fast and then they decide to manually slow it
    /// down, it should be aborted as a loss for them. games that are slow from the start of game 1
    /// are obviously like, not intentional."
    #[test]
    fn an_abort_on_ones_own_pc_is_their_loss_after_the_first_seconds() {
        // Player 1 won game 1; player 0's PC falls behind in game 2 and its game says so.
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(1), 5);
        s.report(0, Leave::MatchQuality, Some(Cause::OwnPc), false, 400);
        let o = s.at_sec(400);
        assert_eq!(
            o,
            Outcome::Abandoned { leaver: s.p[0], other: s.p[1], wins: [0, 1], fault: Some(Fault::MatchQuality) }
        );
        assert_eq!(scores(s.p, &o), [Some(0.0), Some(1.0)]);
        assert_eq!(o.fault(), Some(s.p[0]));
        // The other game's word that it was player 0's PC is not enough on its own: it waits for
        // player 0's own report, which says so.
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(1), 5);
        s.report(1, Leave::MatchQuality, Some(Cause::PeerPc), false, 400);
        assert_eq!(s.at_sec(401), Outcome::InProgress { wins: [0, 1] });
        s.report(0, Leave::MatchQuality, Some(Cause::OwnPc), false, 401);
        assert!(matches!(s.at_sec(401), Outcome::Abandoned { fault: Some(Fault::MatchQuality), .. }));
        // Player 0's report never comes: a void if their game is still with mm, their loss if it
        // is gone.
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(1), 5);
        s.report(1, Leave::MatchQuality, Some(Cause::PeerPc), false, 400);
        assert_eq!(
            s.at_sec(530),
            Outcome::Void { status: "TERMINATED", reason: MATCH_QUALITY_LOW.into(), wins: [0, 1], fault: None }
        );
        s.online(0, false);
        assert_eq!(s.at_sec(530).fault(), Some(s.p[0]));
        assert_eq!(s.at_sec(519), Outcome::InProgress { wins: [0, 1] });
        // Early in the set (game 1's first seconds, or a game 1 poor from its start): void, but
        // counted against player 0.
        let mut s = Set::new();
        s.report(0, Leave::MatchQuality, Some(Cause::OwnPc), true, 60);
        let o = s.at_sec(60);
        assert_eq!(
            o,
            Outcome::Void { status: "TERMINATED", reason: MATCH_QUALITY_LOW.into(), wins: [0, 0], fault: Some(s.p[0]) }
        );
        assert_eq!(scores(s.p, &o), [None, None]);
        // Either game saying early is enough; an older game that does not say is early.
        let mut s = Set::new();
        s.both(1, Some(1), 5);
        s.report(0, Leave::MatchQuality, Some(Cause::OwnPc), false, 400).report(
            1,
            Leave::MatchQuality,
            None,
            true,
            400,
        );
        assert!(matches!(s.at_sec(400), Outcome::Void { fault: Some(_), .. }));
        let mut s = Set::new();
        s.both(1, Some(1), 5).report(0, Leave::MatchQuality, Some(Cause::OwnPc), false, 420);
        let old = LeaveReport { early: None, ..s.leaves[0].clone() };
        s.leaves.push(LeaveReport { reporter: s.p[1], cause: None, ..old });
        assert!(matches!(s.at_min(7), Outcome::Void { fault: Some(_), .. }));
        // Both PCs fell behind: nobody is blamed.
        let mut s = Set::new();
        s.report(0, Leave::MatchQuality, Some(Cause::OwnPc), false, 400).report(
            1,
            Leave::MatchQuality,
            Some(Cause::OwnPc),
            false,
            400,
        );
        assert_eq!(
            s.at_sec(400),
            Outcome::Void { status: "TERMINATED", reason: MATCH_QUALITY_LOW.into(), wins: [0, 0], fault: None }
        );
    }

    #[test]
    fn an_abort_on_ones_own_connection_is_their_loss() {
        // The link stalled (both games say "link"); mm saw player 1's connection go silent for a
        // few seconds twice before the abort, player 0's never.
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(0), 5).trouble(1, 380, 383).trouble(1, 386, 389);
        s.report(0, Leave::MatchQuality, Some(Cause::Link), false, 392);
        let o = s.at_sec(392);
        assert_eq!(
            o,
            Outcome::Abandoned { leaver: s.p[1], other: s.p[0], wins: [1, 0], fault: Some(Fault::MatchQuality) }
        );
        // Both connections in trouble, or neither: nobody is blamed (after the wait for the other
        // game's report).
        let mut s = Set::new();
        s.trouble(1, 380, 383).trouble(0, 350, 351);
        s.report(0, Leave::MatchQuality, Some(Cause::Link), false, 392).report(
            1,
            Leave::MatchQuality,
            Some(Cause::Link),
            false,
            392,
        );
        assert_eq!(s.at_sec(392).fault(), None);
        let mut s = Set::new();
        s.trouble(1, 200, 230);
        s.report(0, Leave::MatchQuality, Some(Cause::Link), false, 392).report(
            1,
            Leave::MatchQuality,
            Some(Cause::Link),
            false,
            392,
        );
        assert!(
            matches!(s.at_sec(392), Outcome::Void { status: "TERMINATED", fault: None, .. }),
            "trouble long before"
        );
        // A blip of a second on one connection blames nobody.
        let mut s = Set::new();
        s.trouble(0, 385, 386);
        s.report(0, Leave::MatchQuality, Some(Cause::Link), false, 392).report(
            1,
            Leave::MatchQuality,
            Some(Cause::Link),
            false,
            392,
        );
        assert_eq!(s.at_sec(392).fault(), None);
        // A PC that says it fell behind decides it, whatever mm saw of the other's connection.
        let mut s = Set::new();
        s.trouble(1, 380, 383);
        s.report(0, Leave::MatchQuality, Some(Cause::OwnPc), false, 392);
        assert_eq!(s.at_sec(392).fault(), Some(s.p[0]));
    }

    /// The user's, 2026-10-29: "it's when they decide to pull the network cable or something like
    /// maybe after 10-15s after the start of the first game that it becomes potentially
    /// intentional."
    #[test]
    fn a_connection_lost_on_ones_own_end_is_their_loss() {
        // Player 1 pulls the cable in game 2: mm sees their connection go, and both games say the
        // other left (player 1's once it is back).
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(0), 5).trouble(1, 400, 410);
        s.report(0, Leave::OpponentLeft, None, false, 407).report(1, Leave::OpponentLeft, None, false, 430);
        let o = s.at_sec(430);
        assert_eq!(
            o,
            Outcome::Abandoned { leaver: s.p[1], other: s.p[0], wins: [1, 0], fault: Some(Fault::Connection) }
        );
        // Back with mm but no report of its own: decided at once all the same.
        let mut s = Set::new();
        s.online(0, true).online(1, true).both(1, Some(0), 5).trouble(1, 400, 410);
        s.report(0, Leave::OpponentLeft, None, false, 407);
        assert_eq!(s.at_sec(420).fault(), Some(s.p[1]));
        // Gone from mm for good: abandoned after the grace period, as before, now counted as their
        // connection.
        let mut s = Set::new();
        s.online(0, true).online(1, false).trouble(1, 400, 410);
        s.report(0, Leave::OpponentLeft, None, false, 407);
        assert_eq!(s.at_sec(420), Outcome::InProgress { wins: [0, 0] });
        assert_eq!(
            s.at_sec(530),
            Outcome::Abandoned { leaver: s.p[1], other: s.p[0], wins: [0, 0], fault: Some(Fault::Connection) }
        );
        // In game 1's first seconds: void, counted against them.
        let mut s = Set::new();
        s.trouble(1, 100, 110);
        s.report(0, Leave::OpponentLeft, None, true, 107).report(1, Leave::OpponentLeft, None, true, 130);
        assert_eq!(
            s.at_sec(130),
            Outcome::Void { status: "TERMINATED", reason: CONNECTION_BROKE.into(), wins: [0, 0], fault: Some(s.p[1]) }
        );
        // Both connections went, or neither (the link between them broke): nobody is blamed.
        let mut s = Set::new();
        s.trouble(1, 400, 410).trouble(0, 401, 409);
        s.report(0, Leave::OpponentLeft, None, false, 407).report(1, Leave::OpponentLeft, None, false, 407);
        assert_eq!(
            s.at_sec(407),
            Outcome::Void { status: "TERMINATED", reason: CONNECTION_BROKE.into(), wins: [0, 0], fault: None }
        );
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
                                               // Down: kept until 50 below the floor...
        assert_eq!(held_rank(e3, 1440.0), e3);
        assert_eq!(held_rank(e3, 1400.0), e3);
        // ...then one division (Gold 1, though the rating is already Gold 2's).
        assert_eq!(held_rank(e3, 1399.0), rank(Tier::Gold, Some(1)));
        // From Gold 1 (1400) the next drop needs below 1350.
        let g1 = rank(Tier::Gold, Some(1));
        assert_eq!(held_rank(g1, 1350.0), g1);
        assert_eq!(held_rank(g1, 1349.0), rank(Tier::Gold, Some(2)));
        // A collapse never shows a rank above what rating + buffer reaches.
        assert_eq!(held_rank(rank(Tier::Master, Some(3)), 1500.0), rank(Tier::Emerald, Some(2)));
        // Legendary keeps its buffer too.
        assert_eq!(held_rank(Rank::HIGHEST, 2200.0), Rank::HIGHEST);
        assert_eq!(held_rank(Rank::HIGHEST, 2199.0), rank(Tier::Master, Some(1)));
    }

    #[test]
    fn placement_takes_five_sets_counting_down() {
        for n in 0..PLACEMENT_SETS {
            assert!(!placed(n));
            assert_eq!(sets_to_place(n), PLACEMENT_SETS - n);
        }
        assert_eq!(sets_to_place(0), 5);
        assert!(placed(PLACEMENT_SETS));
        assert_eq!(sets_to_place(PLACEMENT_SETS), 0);
        assert_eq!(sets_to_place(500), 0);
        // Lopsided sets against other new players place at five too, whatever the RD.
        let (mut st, mut rating) = (TierState::default(), DEFAULT_RATING);
        for n in 0..PLACEMENT_SETS {
            st = st.after_set(n, rating, rating - 5.0, (1900.0, RD_START));
            rating -= 5.0;
            assert_eq!(st.rank.is_some(), n + 1 == PLACEMENT_SETS, "after set {}", n + 1);
        }
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
            st = st.after_set(n, rating, after, (rating, 80.0));
            rating = after;
            assert_eq!(st.rank.is_some(), n + 1 >= 5, "after set {}", n + 1);
        }
        // Placed at 1700: Diamond 3.
        assert_eq!(st.rank, Some(rank(Tier::Diamond, Some(3))));
        // A loss to 1610 keeps Diamond 3 (buffer); a fall to 1520 drops to Emerald 1.
        st = st.after_set(5, 1700.0, 1610.0, (1700.0, 80.0));
        assert_eq!(st.rank, Some(rank(Tier::Diamond, Some(3))));
        st = st.after_set(6, 1610.0, 1520.0, (1610.0, 80.0));
        assert_eq!(st.rank, Some(rank(Tier::Emerald, Some(1))));
    }

    /// A set as [`settled`] plays it: match id, its rated players as (uid, opponent, score), settle
    /// time.
    type PlayedSet<'a> = (&'a str, Vec<(Uuid, Uuid, f64)>, DateTime<Utc>);

    /// Plays `sets` as `settle` stores them.
    fn settled(sets: &[PlayedSet]) -> Vec<StoredChange> {
        let mut st: HashMap<Uuid, Standing> = HashMap::new();
        let mut out = vec![];
        for (m, rated, at) in sets {
            let before = st.clone();
            for &(me, opp, score) in rated {
                let (s, o) =
                    (before.get(&me).copied().unwrap_or_default(), before.get(&opp).copied().unwrap_or_default());
                let after = updated_at(s.rating, s.sets_played, o.rating, score, *at);
                out.push(StoredChange {
                    id: out.len() as i64 + 1,
                    uid: me,
                    match_id: m.to_string(),
                    opponent: Some(opp),
                    score,
                    rating_before: s.rating,
                    rating_after: after,
                    opponent_rating: o.rating,
                    k: k_factor(s.sets_played),
                    sets_before: s.sets_played,
                    at: *at,
                });
                st.insert(me, Standing { rating: after, sets_played: s.sets_played + 1 });
            }
        }
        out
    }

    #[test]
    fn rerate_reproduces_the_history_and_removes_a_set() {
        let (a, b, c) = (uid(1), uid(2), uid(3));
        let t = |s: i64| DateTime::from_timestamp(MIN_CHANGE_SINCE_UNIX + s, 0).unwrap();
        fn win(m: &str, w: Uuid, l: Uuid, at: DateTime<Utc>) -> PlayedSet<'_> {
            (m, vec![(w, l, 1.0), (l, w, 0.0)], at)
        }
        // Before MIN_CHANGE a far stronger player gains under a point; after it, at least one.
        let names: Vec<String> = (0..80).map(|n| format!("a{n}")).collect();
        let mut sets = vec![];
        for (n, m) in names.iter().enumerate() {
            sets.push(win(m, a, c, t(-200 + n as i64)));
        }
        sets.push(win("ab", a, b, t(-50)));
        // The set to void: b abandons before any game, only b is rated (as until 2026-10-23).
        sets.push(("void", vec![(b, a, 0.0)], t(-40)));
        sets.push(win("bc", b, c, t(-30)));
        sets.push(win("ac", a, c, t(10)));
        sets.push(win("ba", b, a, t(20)));
        let history = settled(&sets);
        assert!(history.iter().any(|s| s.at < t(0) && (s.rating_after - s.rating_before).abs() < MIN_CHANGE));

        let (same, finals) = rerate(&history, &[]);
        assert_eq!(same.len(), history.len());
        assert!(same.iter().zip(&history).all(|(r, s)| r.matches(s)));
        assert_eq!(finals[&a].rating, history.iter().rev().find(|s| s.uid == a).unwrap().rating_after);

        // From the voided set on, as if it never happened.
        let start = history.iter().position(|s| s.match_id == "void").unwrap();
        let void_id = history[start].id;
        let (rerated, finals) = rerate(&history[start..], &[void_id]);
        let without: Vec<_> = sets.iter().filter(|s| s.0 != "void").cloned().collect();
        let expected = settled(&without);
        let expected_later: Vec<&StoredChange> = expected.iter().skip(start).collect();
        assert_eq!(rerated.len(), expected_later.len());
        for (r, e) in rerated.iter().zip(expected_later) {
            assert_eq!(
                (r.rating_before, r.rating_after, r.opponent_rating, r.k, r.sets_before),
                (e.rating_before, e.rating_after, e.opponent_rating, e.k, e.sets_before)
            );
        }
        let last = |u: Uuid| expected.iter().rev().find(|s| s.uid == u).unwrap();
        for u in [a, b, c] {
            assert_eq!(finals[&u], Standing { rating: last(u).rating_after, sets_played: last(u).sets_before + 1 });
        }
        // b played one rated set fewer.
        assert_eq!(finals[&b].sets_played, 3);
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
