//! A room's set (`docs/design/room-rules.md` §5): the score the server keeps across the room's
//! games, so a set paused by a player leaving can go on when they are back (decision 16: the host
//! decides). The games themselves are played in one gameplay session (Dolphin's GameSetup runs the
//! strikes, bans and picks); each player's game reports every game's result (`room-result`), and a
//! game counts once both players of the set reported the same.
//!
//! Sides are the set's two players: side 0 the lower slot.

use std::collections::HashMap;

use common::proto::SetTicket;
use common::rooms::{RoomSet, SIDE_DRAW};
use common::rules::{Players, Ruleset, StageChoice};
use uuid::Uuid;

/// No last winner yet (`SetTicket.last_winner`).
pub const SIDE_NONE: u8 = 0xFF;
pub const NO_STAGE: u16 = 0xFFFF;

/// Whether `ruleset` plays a set in a room of `open_slots` open slots (1v1 sets only for now).
pub fn plays_set(ruleset: &Ruleset, open_slots: usize) -> bool {
    ruleset.best_of > 0 && ruleset.players == Players::OneVsOne && open_slots == 2
}

/// Whether every game of `ruleset` is struck without a set (a tournament's Bo1, decision 17).
pub fn strikes_every_game(ruleset: &Ruleset, open_slots: usize) -> bool {
    ruleset.best_of == 0 && ruleset.stage_choice == StageChoice::Strike && open_slots == 2
}

#[derive(Debug, Clone, PartialEq)]
pub enum Pause {
    /// The side's player left the room.
    Left(usize),
    /// The two players reported different results for a game.
    Differ,
}

#[derive(Debug, Clone)]
pub struct SetProgress {
    pub best_of: u8,
    pub game: u32,
    pub wins: [u8; 2],
    pub last_winner: u8,
    pub last_win_stage: [u16; 2],
    /// Each side's slot (0-3) and player.
    pub sides: [usize; 2],
    pub players: [Uuid; 2],
    pub names: [String; 2],
    pub first_striker_bits: u32,
    /// This game's reports: side -> (winner side, stage).
    reports: HashMap<usize, (u8, u16)>,
    pub paused: Option<Pause>,
    pub finished: bool,
}

/// What a report did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reported {
    /// Waiting for the other player's report.
    Waiting,
    /// Both agreed: the game counts.
    Counted,
    /// They differ: the set is paused.
    Differ,
    /// Not a report this set takes (another game, not a player of the set, the set is over).
    Ignored,
}

impl SetProgress {
    /// A new set between the players in `slots` (two of 0-3, any order).
    pub fn new(best_of: u8, mut seats: [(usize, Uuid, String); 2], first_striker_bits: u32) -> Self {
        seats.sort_by_key(|s| s.0);
        let [(s0, p0, n0), (s1, p1, n1)] = seats;
        SetProgress {
            best_of,
            game: 1,
            wins: [0, 0],
            last_winner: SIDE_NONE,
            last_win_stage: [NO_STAGE, NO_STAGE],
            sides: [s0, s1],
            players: [p0, p1],
            names: [n0, n1],
            first_striker_bits,
            reports: HashMap::new(),
            paused: None,
            finished: false,
        }
    }

    pub fn side_of(&self, uid: Uuid) -> Option<usize> {
        self.players.iter().position(|p| *p == uid)
    }

    /// On (started, not won, whether paused or not).
    pub fn on(&self) -> bool {
        !self.finished
    }

    /// A player's report of `game`.
    pub fn report(&mut self, uid: Uuid, game: u32, winner_side: u8, stage: u16) -> Reported {
        let Some(side) = self.side_of(uid) else { return Reported::Ignored };
        if self.finished || self.paused.is_some() || game != self.game {
            return Reported::Ignored;
        }
        self.reports.insert(side, (winner_side, stage));
        if self.reports.len() < 2 {
            return Reported::Waiting;
        }
        let (a, b) = (self.reports[&0], self.reports[&1]);
        self.reports.clear();
        if a != b {
            self.paused = Some(Pause::Differ);
            return Reported::Differ;
        }
        let (winner, stage) = a;
        self.last_winner = winner;
        if winner <= 1 {
            self.wins[winner as usize] += 1;
            self.last_win_stage[winner as usize] = stage;
        }
        self.game += 1;
        let need = self.best_of / 2 + 1;
        if self.wins.iter().any(|w| *w >= need) {
            self.finished = true;
        }
        Reported::Counted
    }

    /// A player of the set left the room: the set waits for them.
    pub fn left(&mut self, uid: Uuid) {
        if let Some(side) = self.side_of(uid) {
            if self.on() && self.paused.is_none() {
                self.paused = Some(Pause::Left(side));
            }
            self.reports.clear();
        }
    }

    /// The slot kept for a player who left (nobody else joins into it), and whose it is.
    pub fn reserved(&self) -> Option<(usize, Uuid)> {
        match self.paused {
            Some(Pause::Left(side)) if self.on() => Some((self.sides[side], self.players[side])),
            _ => None,
        }
    }

    /// `present(slot)`: the player in that slot now, if any.
    pub fn can_resume(&self, present: impl Fn(usize) -> Option<Uuid>) -> bool {
        self.paused.is_some() && (0..2).all(|s| present(self.sides[s]) == Some(self.players[s]))
    }

    pub fn view(&self, present: impl Fn(usize) -> Option<Uuid>) -> RoomSet {
        RoomSet {
            best_of: self.best_of,
            game: self.game,
            score: self.wins,
            sides: [self.sides[0] as u8 + 1, self.sides[1] as u8 + 1],
            paused: self.paused.is_some(),
            waiting_for: match self.paused {
                Some(Pause::Left(side)) if present(self.sides[side]) != Some(self.players[side]) => {
                    Some(self.names[side].clone())
                }
                _ => None,
            },
            can_resume: self.can_resume(&present),
            finished: self.finished,
        }
    }

    /// The set as a session starts it.
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
            sides: [self.sides[0] as u8, self.sides[1] as u8],
            game: self.game,
            wins: self.wins,
            last_winner: self.last_winner,
            last_win_stage: self.last_win_stage,
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
        game: 1,
        wins: [0, 0],
        last_winner: SIDE_NONE,
        last_win_stage: [NO_STAGE, NO_STAGE],
    }
}

/// `room-result.winnerSide` values the set takes.
pub fn valid_side(s: u8) -> bool {
    s <= 1 || s == SIDE_DRAW
}

#[cfg(test)]
mod tests {
    use super::*;

    fn uid(n: u8) -> Uuid {
        Uuid::from_bytes([n; 16])
    }

    fn bo3() -> SetProgress {
        SetProgress::new(3, [(2, uid(3), "carol".into()), (0, uid(1), "alice".into())], 0b10)
    }

    #[test]
    fn sides_are_the_lower_slot_first() {
        let s = bo3();
        assert_eq!(s.sides, [0, 2]);
        assert_eq!(s.players, [uid(1), uid(3)]);
        assert_eq!(s.side_of(uid(3)), Some(1));
    }

    #[test]
    fn a_game_counts_when_both_agree_and_the_set_ends() {
        let mut s = bo3();
        assert_eq!(s.report(uid(1), 1, 0, 0x21), Reported::Waiting);
        assert_eq!(s.report(uid(3), 1, 0, 0x21), Reported::Counted);
        assert_eq!((s.game, s.wins, s.last_winner, s.last_win_stage), (2, [1, 0], 0, [0x21, NO_STAGE]));
        // A draw counts for nobody.
        s.report(uid(1), 2, SIDE_DRAW, 0x02);
        s.report(uid(3), 2, SIDE_DRAW, 0x02);
        assert_eq!((s.game, s.wins, s.last_winner), (3, [1, 0], SIDE_DRAW));
        // Old games and strangers are ignored.
        assert_eq!(s.report(uid(1), 2, 0, 0x02), Reported::Ignored);
        assert_eq!(s.report(uid(9), 3, 0, 0x02), Reported::Ignored);
        s.report(uid(1), 3, 0, 0x01);
        assert_eq!(s.report(uid(3), 3, 0, 0x01), Reported::Counted);
        assert!(s.finished && !s.on());
        assert_eq!(s.report(uid(1), 4, 0, 0x01), Reported::Ignored);
    }

    #[test]
    fn different_results_pause_the_set() {
        let mut s = bo3();
        s.report(uid(1), 1, 0, 0x21);
        assert_eq!(s.report(uid(3), 1, 1, 0x21), Reported::Differ);
        assert_eq!(s.paused, Some(Pause::Differ));
        assert_eq!((s.game, s.wins), (1, [0, 0]));
        let here = |slot: usize| match slot {
            0 => Some(uid(1)),
            2 => Some(uid(3)),
            _ => None,
        };
        assert!(s.can_resume(here));
    }

    #[test]
    fn a_player_who_leaves_pauses_it_and_keeps_their_slot() {
        let mut s = bo3();
        s.report(uid(1), 1, 0, 0x21);
        s.left(uid(3));
        assert_eq!(s.paused, Some(Pause::Left(1)));
        assert_eq!(s.reserved(), Some((2, uid(3))));
        let gone = |slot: usize| (slot == 0).then(|| uid(1));
        let v = s.view(gone);
        assert!(v.paused && !v.can_resume);
        assert_eq!(v.waiting_for.as_deref(), Some("carol"));
        let back = |slot: usize| match slot {
            0 => Some(uid(1)),
            2 => Some(uid(3)),
            _ => None,
        };
        let v = s.view(back);
        assert!(v.can_resume && v.waiting_for.is_none());
        // The half-reported game is played again.
        s.paused = None;
        assert_eq!(s.report(uid(1), 1, 1, 0x02), Reported::Waiting);
    }

    #[test]
    fn tickets() {
        let mut r = Ruleset { best_of: 3, strikes: vec![1, 2, 1], bans: 2, dsr: true, ..Default::default() };
        r.normalize();
        let s = bo3();
        let t = s.ticket(&r);
        assert_eq!((t.best_of, t.game, t.sides, t.first_striker_bits), (3, 1, [0, 2], 0b10));
        assert_eq!(t.starters, r.stages.starters);
        assert_eq!(t.counterpicks.len(), r.stages.starters.len() + r.stages.counterpicks.len());
        assert!(plays_set(&r, 2) && !plays_set(&r, 3));
        let mut bo1 = Ruleset { stage_choice: StageChoice::Strike, strikes: vec![1, 2, 1], ..Default::default() };
        bo1.normalize();
        assert!(strikes_every_game(&bo1, 2) && !plays_set(&bo1, 2));
        let t = strike_ticket(&bo1, [3, 1], 7);
        assert!(t.strike_every_game && t.sides == [1, 3] && t.best_of == 0);
    }
}
