//! A room's set (`docs/design/room-rules.md` §5, decisions A, B and D): the score the server keeps
//! across the room's games. The games themselves are played in one gameplay session (Dolphin's
//! GameSetup runs the strikes, bans and picks); every player's game reports each game's result
//! (`room-result`), and a game counts once every player of the set reported the same. Their games
//! also report the steps of the game being set up (`room-step`), so a session that starts again
//! after a player left goes on from the same strike, ban or pick.
//!
//! A player who leaves (or whose connection drops) pauses the set: the room turns private, their
//! slot waits for them, and the set goes on by itself when they are back (decision A). Leaving
//! after the game's match started loses that game. The host closing an absent player's slot
//! forfeits the set for that side.
//!
//! Sides are the set's two players (1v1) or two teams (2v2): side 0 is the side with the lowest
//! slot. In 2v2 either teammate strikes, bans and picks (the session host's game decides between
//! their presses, `docs/design/room-rules.md`).

use std::collections::HashMap;

use common::proto::SetTicket;
use common::rooms::{RoomSet, SIDE_DRAW, SIDE_VOID};
use common::rules::{Players, Ruleset, StageChoice};
use uuid::Uuid;

/// No last winner yet (`SetTicket.last_winner`).
pub const SIDE_NONE: u8 = 0xFF;
pub const NO_STAGE: u16 = 0xFFFF;
/// Stage actions kept per side for the game being set up (a strike pattern of 8 steps of up to 8).
const MAX_ACTIONS: usize = 64;

/// Whether `ruleset` plays a set in a room of `open_slots` open slots with the Teams switch `teams`.
pub fn plays_set(ruleset: &Ruleset, open_slots: usize, teams: bool) -> bool {
    ruleset.best_of > 0
        && match ruleset.players {
            Players::OneVsOne => open_slots == 2,
            Players::TwoVsTwo => open_slots == 4 && teams,
            Players::Ffa => false,
        }
}

/// Whether every game of `ruleset` is struck without a set (a tournament's Bo1, decision 17).
pub fn strikes_every_game(ruleset: &Ruleset, open_slots: usize) -> bool {
    ruleset.best_of == 0 && ruleset.stage_choice == StageChoice::Strike && open_slots == 2
}

/// A player of the set.
#[derive(Debug, Clone, PartialEq)]
pub struct Seat {
    pub slot: usize,
    pub uid: Uuid,
    pub name: String,
}

/// The game being set up: each side's stage actions so far and whether its match has started.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Steps {
    pub game: u32,
    pub actions: [Vec<u8>; 2],
    pub match_started: bool,
}

#[derive(Debug, Clone)]
pub struct SetProgress {
    pub best_of: u8,
    pub game: u32,
    pub wins: [u8; 2],
    pub last_winner: u8,
    pub last_win_stage: [u16; 2],
    /// Each side's players, lowest slot first.
    pub sides: [Vec<Seat>; 2],
    pub first_striker_bits: u32,
    /// This game's reports by player: (winner side, stage).
    reports: HashMap<Uuid, (u8, u16)>,
    pub steps: Steps,
    /// Players of the set who left: the set waits for them.
    pub away: Vec<Uuid>,
    /// The players' reports of a game differed: the set waits for the host (restart).
    pub differ: bool,
    /// Someone won it (or the other side forfeited it).
    pub finished: bool,
    /// The side that won, once finished.
    pub winner: Option<usize>,
}

/// What a report did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reported {
    /// Waiting for the others' reports.
    Waiting,
    /// Everyone agreed: the game counts.
    Counted,
    /// Everyone agreed it was quit: it did not happen, and is played again from its first step.
    Voided,
    /// They differ: the set is paused.
    Differ,
    /// Not a report this set takes (another game, not a player of the set, the set is over).
    Ignored,
}

impl SetProgress {
    /// A new set between two sides (any order; side 0 becomes the one with the lowest slot).
    pub fn new(best_of: u8, mut sides: [Vec<Seat>; 2], first_striker_bits: u32) -> Self {
        for s in &mut sides {
            s.sort_by_key(|x| x.slot);
        }
        let low = |s: &Vec<Seat>| s.first().map_or(usize::MAX, |x| x.slot);
        if low(&sides[1]) < low(&sides[0]) {
            sides.swap(0, 1);
        }
        SetProgress {
            best_of,
            game: 1,
            wins: [0, 0],
            last_winner: SIDE_NONE,
            last_win_stage: [NO_STAGE, NO_STAGE],
            sides,
            first_striker_bits,
            reports: HashMap::new(),
            steps: Steps { game: 1, ..Default::default() },
            away: Vec::new(),
            differ: false,
            finished: false,
            winner: None,
        }
    }

    pub fn side_of(&self, uid: Uuid) -> Option<usize> {
        (0..2).find(|&s| self.sides[s].iter().any(|x| x.uid == uid))
    }

    fn players(&self) -> impl Iterator<Item = &Seat> {
        self.sides.iter().flatten()
    }

    /// On (started, not won, whether paused or not).
    pub fn on(&self) -> bool {
        !self.finished
    }

    /// Waiting for a player to come back, or for the host after reports that differed.
    pub fn paused(&self) -> bool {
        self.on() && (!self.away.is_empty() || self.differ)
    }

    fn count(&mut self, winner: u8, stage: u16) {
        self.last_winner = winner;
        if winner <= 1 {
            self.wins[winner as usize] += 1;
            self.last_win_stage[winner as usize] = stage;
        }
        self.game += 1;
        self.reports.clear();
        self.steps = Steps { game: self.game, ..Default::default() };
        let need = self.best_of / 2 + 1;
        if let Some(side) = (0..2).find(|&s| self.wins[s] >= need) {
            self.finished = true;
            self.winner = Some(side);
        }
    }

    /// A player's report of `game`.
    pub fn report(&mut self, uid: Uuid, game: u32, winner_side: u8, stage: u16) -> Reported {
        if self.side_of(uid).is_none() || !self.on() || self.differ || game != self.game || !valid_side(winner_side) {
            return Reported::Ignored;
        }
        self.reports.insert(uid, (winner_side, stage));
        let first = self.reports[&uid];
        if self.reports.values().any(|r| *r != first) {
            self.reports.clear();
            self.differ = true;
            return Reported::Differ;
        }
        // Every player of the set who is here has reported it.
        let waiting = self.players().any(|x| !self.away.contains(&x.uid) && !self.reports.contains_key(&x.uid));
        if waiting {
            return Reported::Waiting;
        }
        if first.0 == SIDE_VOID {
            // A quit game: its strikes, picks and "match started" go too, so leaving before the
            // game is played again does not lose it.
            self.replay_game();
            return Reported::Voided;
        }
        self.count(first.0, first.1);
        Reported::Counted
    }

    /// A player's game reports the steps of the game being set up (each side's actions in order,
    /// and whether its match has started). The longest report is kept: every game sees the same
    /// actions, some sooner than others.
    pub fn report_steps(&mut self, uid: Uuid, game: u32, actions: [Vec<u8>; 2], match_started: bool) -> bool {
        if self.side_of(uid).is_none() || !self.on() || game != self.game {
            return false;
        }
        let mut changed = false;
        for (side, list) in actions.into_iter().enumerate() {
            if list.len() > self.steps.actions[side].len() && list.len() <= MAX_ACTIONS {
                self.steps.actions[side] = list;
                changed = true;
            }
        }
        if match_started && !self.steps.match_started {
            self.steps.match_started = true;
            changed = true;
        }
        changed
    }

    /// A player of the set left the room: the set waits for them. Leaving after the game's match
    /// started loses that game for their side (decision A); returns true then.
    pub fn left(&mut self, uid: Uuid) -> bool {
        let Some(side) = self.side_of(uid) else { return false };
        if !self.on() {
            return false;
        }
        // A player who already reported the game saw it end: the others' reports count it.
        let lost = self.steps.game == self.game && self.steps.match_started && !self.reports.contains_key(&uid);
        if lost {
            self.count((1 - side) as u8, NO_STAGE);
        }
        if self.on() && !self.away.contains(&uid) {
            self.away.push(uid);
        }
        lost
    }

    /// A player of the set is back in the room: true when that was the last one away (the set goes
    /// on by itself).
    pub fn back(&mut self, uid: Uuid) -> bool {
        let before = self.away.len();
        self.away.retain(|a| *a != uid);
        before > 0 && self.away.is_empty()
    }

    /// The slots kept for players who left, and whose they are.
    pub fn reserved(&self) -> Vec<(usize, Uuid)> {
        if !self.on() {
            return Vec::new();
        }
        self.players().filter(|x| self.away.contains(&x.uid)).map(|x| (x.slot, x.uid)).collect()
    }

    /// `side` gives the set up (the host closed an absent player's slot): the other side wins it.
    pub fn forfeit(&mut self, side: usize) {
        self.finished = true;
        self.winner = Some(1 - side);
        self.away.clear();
        self.differ = false;
    }

    /// The host restarts it after reports that differed: the game is played again.
    pub fn replay_game(&mut self) {
        self.differ = false;
        self.reports.clear();
        self.steps = Steps { game: self.game, ..Default::default() };
    }

    /// The side's name for the status line: the player's, or a team's first player and partner.
    pub fn side_name(&self, side: usize) -> String {
        let names: Vec<&str> = self.sides[side].iter().map(|x| x.name.as_str()).collect();
        names.join(" & ")
    }

    pub fn view(&self) -> RoomSet {
        let away: Vec<&str> = self.players().filter(|x| self.away.contains(&x.uid)).map(|x| x.name.as_str()).collect();
        RoomSet {
            best_of: self.best_of,
            game: self.game,
            score: self.wins,
            sides: [
                self.sides[0].first().map_or(0, |x| x.slot as u8 + 1),
                self.sides[1].first().map_or(0, |x| x.slot as u8 + 1),
            ],
            members: [
                self.sides[0].iter().map(|x| x.slot as u8 + 1).collect(),
                self.sides[1].iter().map(|x| x.slot as u8 + 1).collect(),
            ],
            paused: self.paused(),
            waiting_for: (!away.is_empty()).then(|| away.join(", ")),
            can_resume: self.differ && self.away.is_empty(),
            finished: self.finished,
            winner: self.winner.map(|w| w as u8),
        }
    }

    /// The set as a session starts it: the format, the score so far, the game and its steps so far.
    pub fn ticket(&self, ruleset: &Ruleset) -> SetTicket {
        SetTicket {
            best_of: self.best_of,
            strike_every_game: false,
            starters: ruleset.stages.starters.clone(),
            counterpicks: ruleset.stages.all().collect(),
            strikes: ruleset.strikes.clone(),
            bans: ruleset.bans,
            dsr: ruleset.dsr,
            first_striker_bits: self.first_striker_bits,
            sides: [
                self.sides[0].first().map_or(0, |x| x.slot as u8),
                self.sides[1].first().map_or(0, |x| x.slot as u8),
            ],
            members: vec![
                self.sides[0].iter().map(|x| x.slot as u8).collect(),
                self.sides[1].iter().map(|x| x.slot as u8).collect(),
            ],
            game: self.game,
            wins: self.wins,
            last_winner: self.last_winner,
            last_win_stage: self.last_win_stage,
            actions: if self.steps.game == self.game && !self.steps.match_started {
                vec![self.steps.actions[0].clone(), self.steps.actions[1].clone()]
            } else {
                Vec::new()
            },
        }
    }
}

/// A no-set game struck from the starters (`StageChoice::Strike`) between the players in `slots`.
pub fn strike_ticket(ruleset: &Ruleset, mut slots: [usize; 2], first_striker_bits: u32) -> SetTicket {
    slots.sort();
    SetTicket {
        best_of: 0,
        strike_every_game: true,
        starters: ruleset.stages.starters.clone(),
        counterpicks: ruleset.stages.all().collect(),
        strikes: ruleset.strikes.clone(),
        bans: 0,
        dsr: false,
        first_striker_bits,
        sides: [slots[0] as u8, slots[1] as u8],
        members: vec![vec![slots[0] as u8], vec![slots[1] as u8]],
        game: 1,
        wins: [0, 0],
        last_winner: SIDE_NONE,
        last_win_stage: [NO_STAGE, NO_STAGE],
        actions: Vec::new(),
    }
}

/// `room-result.winnerSide` values the set takes.
pub fn valid_side(s: u8) -> bool {
    s <= 1 || s == SIDE_DRAW || s == SIDE_VOID
}

#[cfg(test)]
mod tests {
    use super::*;

    fn uid(n: u8) -> Uuid {
        Uuid::from_bytes([n; 16])
    }

    fn seat(slot: usize, n: u8, name: &str) -> Seat {
        Seat { slot, uid: uid(n), name: name.into() }
    }

    fn bo3() -> SetProgress {
        SetProgress::new(3, [vec![seat(2, 3, "carol")], vec![seat(0, 1, "alice")]], 0b10)
    }

    #[test]
    fn sides_are_the_lower_slot_first() {
        let s = bo3();
        assert_eq!(s.sides[0][0].slot, 0);
        assert_eq!(s.side_of(uid(3)), Some(1));
        assert_eq!(s.view().sides, [1, 3]);
    }

    #[test]
    fn a_game_counts_when_everyone_agrees_and_the_set_ends() {
        let mut s = bo3();
        assert_eq!(s.report(uid(1), 1, 0, 0x21), Reported::Waiting);
        assert_eq!(s.report(uid(3), 1, 0, 0x21), Reported::Counted);
        assert_eq!((s.game, s.wins, s.last_winner, s.last_win_stage), (2, [1, 0], 0, [0x21, NO_STAGE]));
        s.report(uid(1), 2, SIDE_DRAW, 0x02);
        s.report(uid(3), 2, SIDE_DRAW, 0x02);
        assert_eq!((s.game, s.wins, s.last_winner), (3, [1, 0], SIDE_DRAW));
        assert_eq!(s.report(uid(1), 2, 0, 0x02), Reported::Ignored);
        assert_eq!(s.report(uid(9), 3, 0, 0x02), Reported::Ignored);
        s.report(uid(1), 3, 0, 0x01);
        assert_eq!(s.report(uid(3), 3, 0, 0x01), Reported::Counted);
        assert!(s.finished && s.winner == Some(0) && s.view().finished);
        assert_eq!(s.report(uid(1), 4, 0, 0x01), Reported::Ignored);
    }

    #[test]
    fn different_results_pause_until_the_host_replays_the_game() {
        let mut s = bo3();
        s.report(uid(1), 1, 0, 0x21);
        assert_eq!(s.report(uid(3), 1, 1, 0x21), Reported::Differ);
        assert!(s.paused() && s.view().can_resume);
        s.replay_game();
        assert!(!s.paused());
        assert_eq!((s.game, s.wins), (1, [0, 0]));
    }

    #[test]
    fn a_quit_game_did_not_happen_and_is_struck_again() {
        let mut s = bo3();
        s.report(uid(1), 1, 0, 0x21);
        s.report(uid(3), 1, 0, 0x21);
        // Game 2: struck, picked and started, then quit (L+R+A+START).
        s.report_steps(uid(1), 2, [vec![0x01, 0x2E], vec![0x02, 0x21]], true);
        assert_eq!(s.report(uid(1), 2, SIDE_VOID, 0x2E), Reported::Waiting);
        assert_eq!(s.report(uid(3), 2, SIDE_VOID, 0x2E), Reported::Voided);
        // Still game 2, the score as it was, and nothing of its steps: struck again from the start,
        // and leaving now does not lose it.
        assert_eq!((s.game, s.wins, s.last_winner), (2, [1, 0], 0));
        assert_eq!(s.steps, Steps { game: 2, ..Default::default() });
        assert!(!s.left(uid(3)));
        assert_eq!((s.game, s.wins), (2, [1, 0]));
        // A void against a result: the reports differ (the host decides).
        s.back(uid(3));
        s.report(uid(1), 2, SIDE_VOID, 0x2E);
        assert_eq!(s.report(uid(3), 2, 1, 0x2E), Reported::Differ);
        // Not a side the set takes.
        s.replay_game();
        assert_eq!(s.report(uid(1), 2, 7, 0x2E), Reported::Ignored);
    }

    #[test]
    fn leaving_during_the_steps_waits_and_the_return_resumes_from_them() {
        let mut s = bo3();
        assert!(s.report_steps(uid(1), 1, [vec![0x01], vec![0x02, 0x21]], false));
        // A shorter (older) report changes nothing.
        assert!(!s.report_steps(uid(3), 1, [vec![], vec![0x02]], false));
        assert!(!s.left(uid(3)));
        assert!(s.paused());
        assert_eq!(s.reserved(), vec![(2, uid(3))]);
        assert_eq!(s.view().waiting_for.as_deref(), Some("carol"));
        assert!(s.back(uid(3)));
        assert!(!s.paused());
        // The session starts again from the strikes made so far.
        let t = s.ticket(&Ruleset::default());
        assert_eq!(t.actions, vec![vec![0x01], vec![0x02, 0x21]]);
        assert_eq!(t.game, 1);
    }

    #[test]
    fn leaving_mid_match_loses_the_game() {
        let mut s = bo3();
        s.report_steps(uid(1), 1, [vec![0x01, 0x2E], vec![0x02, 0x21]], true);
        assert!(s.left(uid(1)));
        assert_eq!((s.game, s.wins, s.last_winner), (2, [0, 1], 1));
        assert!(s.paused());
        // Not mid-match any more (the next game has no steps yet).
        assert!(s.ticket(&Ruleset::default()).actions.is_empty() || s.steps.actions.iter().all(|a| a.is_empty()));
        // A second mid-match leave the next game: the set is won by the side still there.
        s.back(uid(1));
        s.report_steps(uid(3), 2, [vec![], vec![]], true);
        assert!(s.left(uid(1)));
        assert!(s.finished && s.winner == Some(1));
        assert!(s.reserved().is_empty());
    }

    #[test]
    fn closing_an_absent_players_slot_forfeits_the_set() {
        let mut s = bo3();
        s.left(uid(3));
        s.forfeit(1);
        assert!(s.finished && s.winner == Some(0) && !s.paused());
    }

    #[test]
    fn a_2v2_set_is_two_teams_and_every_player_reports() {
        let red = vec![seat(1, 2, "bo"), seat(0, 1, "alice")];
        let blue = vec![seat(3, 4, "dan"), seat(2, 3, "carol")];
        let mut s = SetProgress::new(3, [blue, red], 0);
        assert_eq!((s.sides[0][0].slot, s.sides[1][0].slot), (0, 2));
        assert_eq!(s.side_name(0), "alice & bo");
        let t = s.ticket(&Ruleset::default());
        assert_eq!((t.sides, t.members.clone()), ([0, 2], vec![vec![0, 1], vec![2, 3]]));
        for n in [1, 2, 3] {
            assert_eq!(s.report(uid(n), 1, 1, 0x21), Reported::Waiting);
        }
        assert_eq!(s.report(uid(4), 1, 1, 0x21), Reported::Counted);
        // One player away: the others' reports are enough.
        s.left(uid(4));
        s.back(uid(4));
        s.left(uid(4));
        for n in [1, 2] {
            s.report(uid(n), 2, 0, 0x02);
        }
        // (paused while away: reports still count with those present)
        assert_eq!(s.report(uid(3), 2, 0, 0x02), Reported::Counted);
    }

    #[test]
    fn tickets() {
        let mut r = Ruleset { best_of: 3, strikes: vec![1, 2, 1], bans: 2, dsr: true, ..Default::default() };
        r.normalize();
        let s = bo3();
        let t = s.ticket(&r);
        assert_eq!((t.best_of, t.game, t.sides, t.first_striker_bits), (3, 1, [0, 2], 0b10));
        assert_eq!(t.counterpicks.len(), r.stages.starters.len() + r.stages.counterpicks.len());
        assert!(plays_set(&r, 2, false) && !plays_set(&r, 3, false));
        let r2 = Ruleset { players: Players::TwoVsTwo, ..r.clone() };
        assert!(plays_set(&r2, 4, true) && !plays_set(&r2, 4, false) && !plays_set(&r2, 2, false));
        let mut bo1 = Ruleset { stage_choice: StageChoice::Strike, strikes: vec![1, 2, 1], ..Default::default() };
        bo1.normalize();
        assert!(strikes_every_game(&bo1, 2) && !plays_set(&bo1, 2, false));
        let t = strike_ticket(&bo1, [3, 1], 7);
        assert!(t.strike_every_game && t.sides == [1, 3] && t.best_of == 0);
    }
}
