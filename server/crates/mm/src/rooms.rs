//! Rooms: the room model, free of I/O (`docs/rooms-protocol.md`, decisions in
//! `docs/design/rooms.md`).
//!
//! A room is in memory only: it exists while someone is in it, and its code is free again as soon
//! as it is empty, so nothing about it needs to outlive the process (a restart of mm empties every
//! room, and the members' games see their online connection drop, as with a search).
//!
//! Members are online connections ([`crate::engine`] state `Online`): one per account. A room has
//! four slots; a slot is the in-game port and stays the same for the life of the room. A new room
//! has slots 1-2 open and 3-4 closed. A member either plays or sits out (the panel's SIT OUT, and
//! everyone outside a set that is on). The game starts by itself once at least two members play,
//! every one of them is ready and nobody is still in the last game; empty open slots wait for
//! nobody (players join them during a game and play the next). Then each player of the game sends
//! a room game ticket (`create-ticket`, mode 3, the room code as `search.connectCode`) from its
//! P2P port, and once all are in, each gets one `get-ticket-resp` listing all of them, ports =
//! slots.
//!
//! [`RoomBook`] sends the room messages to members itself (`Output::Send`). Answers to tickets go
//! through the engine, which owns ticket connections: [`RoomBook::ticket`] returns them, and
//! tickets that have to fail (a start that timed out, a member who left) wait in
//! [`RoomBook::take_failed_tickets`]. Rooms whose players changed wait in
//! [`RoomBook::take_changed`], so the engine can keep each room's chat group in step
//! ([`crate::chat`]).

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::net::SocketAddrV4;
use std::time::{Duration, Instant};

use chrono::Utc;
use common::proto::{GetTicketResp, Player, GET_TICKET_RESP};
use common::ratelimit::{RateLimiter, Window};
use common::rooms::{
    self, PublicRoom, RoomError, RoomLeft, RoomMember, RoomMode, RoomPlayer, RoomRequest, RoomSlot, RoomStart,
    RoomState, RoomStatus, RoomUnreachable, ROOM_SLOTS, TEAM_COUNT,
};
use uuid::Uuid;

use crate::engine::{ConnId, Output, RulesetFetch};
use crate::messages as msg;
use crate::room_set::{self, SetProgress};
use crate::ruleset::ModeRules;
use common::rules::{Ruleset, StageChoice, StageLists};

#[derive(Debug, Clone)]
pub struct RoomsConfig {
    /// How long a starting room waits for every member's game ticket.
    pub start_timeout: Duration,
    /// A game that has not ended (every player back on the room's CSS) after this long is taken to
    /// be over, so a lost `room-back` cannot hold a room in game forever.
    pub game_max: Duration,
    /// Live rooms at most (memory bound; each account is in one room at most anyway).
    pub max_rooms: usize,
    /// Rooms created per account.
    pub create_windows: Vec<Window>,
    /// Join attempts per account (counted before the code is looked up, so codes cannot be
    /// guessed quickly).
    pub join_windows: Vec<Window>,
    /// Room requests per connection, all kinds together.
    pub request_windows: Vec<Window>,
    /// Stages and items sent with a room's game (Direct's: rooms play like Direct).
    pub rules: ModeRules,
}

impl Default for RoomsConfig {
    fn default() -> Self {
        let minute = Duration::from_secs(60);
        RoomsConfig {
            start_timeout: Duration::from_secs(15),
            game_max: Duration::from_secs(30 * 60),
            max_rooms: 10_000,
            create_windows: vec![Window::new(5, minute), Window::new(30, Duration::from_secs(3600))],
            join_windows: vec![Window::new(10, minute), Window::new(60, Duration::from_secs(3600))],
            request_windows: vec![Window::new(40, Duration::from_secs(10))],
            rules: ModeRules::default(),
        }
    }
}

/// How long after a room's game is over a member may still be watching it ([`RoomBook::tick`]).
const WATCH_AFTER_GAME: Duration = Duration::from_secs(30);
/// A room game without a set counts as played with the room's ruleset (`ruleset_plays`) when it
/// lasted this long from its start (loading included): a quick quit does not count.
const COUNT_GAME_AFTER: Duration = Duration::from_secs(30);

/// Who a member is (from the verified account).
#[derive(Debug, Clone, PartialEq)]
pub struct MemberInfo {
    pub uid: Uuid,
    pub display_name: String,
    pub connect_code: String,
    /// The name the account gives the rooms of this kind it hosts (`users.room_name` or
    /// `users.mods_room_name`): a room it creates gets it.
    pub room_name: Option<String>,
}

#[derive(Debug, Clone)]
struct Member {
    conn: ConnId,
    info: MemberInfo,
    /// Join order (host hand-over goes to the earliest).
    joined: u64,
    ready: bool,
    team: u8,
    character: Option<u8>,
    costume: Option<u8>,
    /// The stock count picked on the panel (stock control), with the lock-in.
    stocks: Option<u8>,
    /// The HP picked on the panel (a stamina match), with the lock-in.
    hp: Option<u16>,
    in_game: bool,
    /// The panel's SIT OUT: the room's games start without this member until they play again.
    sitting_out: bool,
    /// `room-watch` `want` (2026-10-19, `docs/design/rooms.md` §1.3): the room's games this member
    /// is not in go to their game ([`RoomBook::watch_targets`]).
    watch: bool,
    /// `room-watch` `watching`: their game shows one of the room's games now, since then. The
    /// room's next game does not wait for them ([`Room::playing`]).
    watching: Option<Instant>,
}

#[derive(Debug, Clone)]
struct StartTicket {
    conn: ConnId,
    addr: SocketAddrV4,
    lan: String,
}

#[derive(Debug, Clone)]
enum Phase {
    Waiting,
    /// `players`: the slots (0-3) that play this game, the others sit it out; `host`: the game's
    /// host (the session's decider), one of them.
    Starting {
        match_id: String,
        since: Instant,
        tickets: BTreeMap<usize, StartTicket>,
        players: Vec<usize>,
        host: usize,
    },
    /// `unreachable`: the pairs of slots (1-4, lower first) that reported they could not connect
    /// to each other (`room-unreachable`), each told to the room once. `players`: the game's
    /// players (slot, account). `set`: the session plays the room's set (its games are counted as
    /// they are reported, not when it ends).
    InGame {
        since: Instant,
        match_id: String,
        unreachable: BTreeSet<[u8; 2]>,
        players: Vec<(usize, Uuid)>,
        set: bool,
    },
}

#[derive(Debug, Clone)]
struct Room {
    code: String,
    /// The name its host gave it (`docs/design/rooms.md` §1.4): the creator's at the start, then
    /// the host's whenever the host renames (the room keeps it when the host leaves).
    name: Option<String>,
    public: bool,
    teams: bool,
    /// With `teams`: every player in their team's costume (P+'s plain team battle), else P+'s
    /// rainbow team battle (any costume, a team glow). The host's FFA / TEAMS button goes FFA,
    /// rainbow TEAMS, TEAMS (the user's, 2026-10-21).
    team_colors: bool,
    open: [bool; ROOM_SLOTS],
    slots: [Option<Member>; ROOM_SLOTS],
    /// Slot index (0-3) of the host.
    host: usize,
    phase: Phase,
    created: Instant,
    /// Rooms created before this one since mm started, plus one: the `n` of its chat group
    /// `room-<CODE>-<n>`, so a code used again later is another group.
    serial: u64,
    /// What the room plays (`docs/design/room-rules.md`): P+'s default with Direct's rules when it
    /// is created, a published ruleset (`ruleset_code`), or custom rules (the host's `room-rules`;
    /// `based_on` names the published one they were made from). Every game is played with them.
    ruleset: Ruleset,
    ruleset_code: Option<String>,
    based_on: Option<String>,
    /// The ruleset's set (`room_set.rs`): kept across the room's games until someone wins it, or
    /// the host restarts it; a finished one stays (its result shown) until the next set starts or
    /// the rules, ruleset or players change (room-rules.md B).
    set: Option<SetProgress>,
    /// While a set waits for a player who left the room is private (room-rules.md A): whether it
    /// was public before, to go back to when the set goes on.
    public_before_pause: Option<bool>,
    /// The ports (bits) that pick the next game's stage, as the last game's players reported it
    /// with `room-back` (none from an older Dolphin: the players' games keep their own); a player
    /// who left is taken out. `pick_owed`: the last game left someone a pick, so a game its
    /// pickers sit out (or left) gives it to one of its players.
    pickers: Option<u8>,
    pick_owed: bool,
    /// Draws the player who picks when the pickers do not play (the same for the whole time
    /// between two games, so the room's CSS and the start agree).
    pick_draw: usize,
    /// When its last game was over (every player back): a member still marked watching long
    /// after that (their game never said it stopped) is waited for again ([`RoomBook::tick`]).
    game_over_at: Option<Instant>,
}

impl Room {
    fn members(&self) -> impl Iterator<Item = (usize, &Member)> {
        self.slots.iter().enumerate().filter_map(|(i, m)| m.as_ref().map(|m| (i, m)))
    }
    fn player_count(&self) -> usize {
        self.slots.iter().filter(|m| m.is_some()).count()
    }
    fn open_count(&self) -> usize {
        self.open.iter().filter(|o| **o).count()
    }
    fn slot_of(&self, conn: ConnId) -> Option<usize> {
        self.members().find(|(_, m)| m.conn == conn).map(|(i, _)| i)
    }
    /// Sits out the room's games: by choice, or outside the sides of a set that is on (they play
    /// it to the end).
    fn sits_out(&self, m: &Member) -> bool {
        m.sitting_out || self.set.as_ref().is_some_and(|s| s.on() && s.side_of(m.info.uid).is_none())
    }
    /// Played the room's game going on: one who is back on the CSS before the others (`room-back`,
    /// no longer `in_game`) still never watches it (the user, 2026-10-24: "my room game ended and
    /// then suddenly i was spectating my own room").
    fn played_game(&self, m: &Member) -> bool {
        matches!(&self.phase, Phase::InGame { players, .. } if players.iter().any(|(_, uid)| *uid == m.info.uid))
    }
    /// The slots of the members who play the next game.
    /// A member watching the room's game (a few seconds behind its players) is not waited for (the
    /// user's, 2026-10-19: "finish, never block"): the next game starts without them.
    fn playing(&self) -> Vec<usize> {
        self.members().filter(|(_, m)| !self.sits_out(m) && m.watching.is_none()).map(|(i, _)| i).collect()
    }
    /// From the players (the open slots while fewer than two play: what the room is for).
    fn mode(&self) -> RoomMode {
        let n = self.playing().len();
        RoomMode::of(if n >= 2 { n } else { self.open_count() }, self.teams)
    }
    fn free_slot(&self) -> Option<usize> {
        (0..ROOM_SLOTS).find(|&i| self.open[i] && self.slots[i].is_none())
    }
    fn status(&self) -> RoomStatus {
        match self.phase {
            Phase::Waiting => RoomStatus::Waiting,
            Phase::Starting { .. } => RoomStatus::Starting,
            Phase::InGame { .. } => RoomStatus::InGame,
        }
    }
    /// Teams on with three or more players and everyone on one colour (`docs/design/rooms.md` #12).
    fn one_colour(&self) -> bool {
        if self.mode() != RoomMode::Teams {
            return false;
        }
        let mut teams = self.members().filter(|(_, m)| !self.sits_out(m)).map(|(_, m)| m.team);
        let Some(first) = teams.next() else { return false };
        teams.all(|t| t == first)
    }
    /// Two or more play, all of them ready, and nobody is still in the last game. Empty open slots
    /// and the members who sit out are not waited for.
    fn can_start(&self) -> bool {
        let playing = self.playing();
        matches!(self.phase, Phase::Waiting)
            && playing.len() >= 2
            && playing.iter().all(|&i| self.slots[i].as_ref().is_some_and(|m| m.ready))
            && self.members().all(|(_, m)| !m.in_game)
            && !self.one_colour()
            && !self.set.as_ref().is_some_and(|s| s.paused())
    }

    /// The game's host (the session's decider): the room's host when they play it, else its
    /// earliest-joined player.
    fn game_host(&self, players: &[usize]) -> usize {
        if players.contains(&self.host) {
            return self.host;
        }
        players
            .iter()
            .copied()
            .min_by_key(|&i| self.slots[i].as_ref().map_or(u64::MAX, |m| m.joined))
            .unwrap_or(self.host)
    }

    /// The loser picks: the ports that pick the stage of a game `players` play. The last game's
    /// pickers who play it; when none does (they sit out or left), one of its players at random
    /// (the user's, 2026-10-18). `None`: the players' games decide (no report from them, or the
    /// ruleset leaves no stage to the loser).
    fn next_pickers(&self, players: &[usize]) -> Option<u8> {
        let loser_picks = self.ruleset.best_of == 0
            && matches!(self.ruleset.stage_choice, StageChoice::LoserPicks | StageChoice::LoserPicksList);
        let reported = self.pickers.filter(|_| loser_picks)?;
        let mask = players.iter().fold(0u8, |m, &i| m | 1 << i);
        Some(match reported & mask {
            0 if self.pick_owed && !players.is_empty() => 1 << players[self.pick_draw % players.len()],
            q => q,
        })
    }

    /// A set that waits for a player makes the room private; when it goes on (or ends) the room
    /// is public again if it was.
    fn settle_privacy(&mut self) {
        let waiting = self.set.as_ref().is_some_and(|s| s.paused() && !s.away.is_empty());
        match (waiting, self.public_before_pause) {
            (true, None) => {
                self.public_before_pause = Some(self.public);
                self.public = false;
            }
            (false, Some(was)) => {
                self.public = was;
                self.public_before_pause = None;
            }
            _ => {}
        }
    }

    /// The host changed what the room plays: no longer the published ruleset (it becomes custom,
    /// based on it, decision 13); a finished set's score goes; and nobody starts under rules they
    /// did not see (room-rules.md #5).
    fn edited(&mut self) {
        if self.ruleset_code.take().is_some() {
            self.based_on = Some(format!("{} v{}", self.ruleset.name, self.ruleset.version));
        }
        self.ruleset.name = "Custom".into();
        self.ruleset.version = 0;
        self.ruleset.by.clear();
        self.set = None;
        for m in self.slots.iter_mut().flatten() {
            m.ready = false;
        }
    }

    /// The room's ruleset with sets of `best_of` (0: none; played when the room makes two sides);
    /// a ruleset without a strike pattern gets Ranked's ([1, 2, 1] over five starters), and
    /// turning a set on gives the winner Ranked's two bans and Dave's rule.
    fn with_best_of(&self, best_of: u8) -> Result<Ruleset, &'static str> {
        let mut r = self.ruleset.clone();
        if best_of > 0 {
            if r.strikes.is_empty() {
                r.strikes = room_set::default_strikes(r.stages.starters.len());
            }
            if r.best_of == 0 {
                r.bans = 2.min(r.stages.all().count().saturating_sub(2) as u8);
                r.dsr = true;
            }
        }
        r.best_of = best_of;
        r.normalize();
        r.validate().map_err(|e| {
            tracing::debug!(code = %self.code, "best of {best_of}: {e}");
            msg::SETS_NOT_POSSIBLE
        })?;
        Ok(r)
    }

    /// The room's ruleset with the host's stages from the game's STAGE SWITCH, and the loser's
    /// choice between any stage and the list (only between those two: strike and random are the
    /// launcher's). A strike pattern that no longer fits the starters becomes the default one.
    fn with_stages(&self, stages: StageLists, choice: Option<StageChoice>) -> Result<Ruleset, &'static str> {
        if stages.starters.is_empty() {
            return Err(msg::NO_STARTER_STAGE);
        }
        let mut r = self.ruleset.clone();
        r.stages = stages;
        let loser_picks = |c: StageChoice| matches!(c, StageChoice::LoserPicks | StageChoice::LoserPicksList);
        if let Some(c) = choice.filter(|&c| loser_picks(c) && loser_picks(r.stage_choice) && r.best_of == 0) {
            r.stage_choice = c;
        }
        if r.strikes_used() {
            let fits = r.strikes.iter().map(|&n| n as usize).sum::<usize>() + 1 == r.stages.starters.len();
            if !fits {
                r.strikes = room_set::default_strikes(r.stages.starters.len());
            }
        }
        r.normalize();
        r.validate().map_err(|e| {
            tracing::debug!(code = %self.code, "stages: {e}");
            msg::STAGES_NOT_POSSIBLE
        })?;
        Ok(r)
    }

    /// The room's ruleset with the host's MATCH SETUP page (`room-format`, docs/design/special-rules.md
    /// §4): the stage choice (no set only), the strike pattern, the bans and DSR (a set only) and
    /// alternate stages, normalized as a published ruleset is; one that does not fit its stages
    /// (a pattern that does not leave one starter, more bans than stages) is refused.
    fn with_format(&self, f: &rooms::RoomFormat) -> Result<Ruleset, &'static str> {
        let mut r = self.ruleset.clone();
        if r.best_of == 0 {
            r.stage_choice = f.stage_choice;
        }
        r.strikes = f.strikes.clone();
        r.bans = f.bans;
        r.dsr = f.dsr;
        r.alternate_stages = f.alternate_stages;
        if r.strikes_used() && r.strikes.is_empty() {
            r.strikes = room_set::default_strikes(r.stages.starters.len());
        }
        r.normalize();
        r.validate().map_err(|e| {
            tracing::debug!(code = %self.code, "format: {e}");
            msg::FORMAT_NOT_POSSIBLE
        })?;
        Ok(r)
    }

    /// The room's two sides, when it makes two (room-rules.md decision G): two players (a 1v1,
    /// whatever the Teams switch), or four on two teams of two (Teams on). A set or a struck game
    /// is played between them; any other room plays single games.
    fn set_sides(&self) -> Option<[Vec<room_set::Seat>; 2]> {
        self.sides_of(&self.playing())
    }

    /// The two sides `players` (slots) make, as [`Self::set_sides`].
    fn sides_of(&self, players: &[usize]) -> Option<[Vec<room_set::Seat>; 2]> {
        let seat =
            |i: usize, m: &Member| room_set::Seat { slot: i, uid: m.info.uid, name: m.info.display_name.clone() };
        let members: Vec<(usize, &Member)> =
            players.iter().filter_map(|&i| self.slots[i].as_ref().map(|m| (i, m))).collect();
        match members.as_slice() {
            [(a, ma), (b, mb)] => Some([vec![seat(*a, ma)], vec![seat(*b, mb)]]),
            [_, _, _, _] if self.teams => {
                let mut teams: Vec<(u8, Vec<room_set::Seat>)> = Vec::new();
                for (i, m) in &members {
                    match teams.iter_mut().find(|(t, _)| *t == m.team) {
                        Some((_, seats)) => seats.push(seat(*i, m)),
                        None => teams.push((m.team, vec![seat(*i, m)])),
                    }
                }
                match <[_; 2]>::try_from(teams) {
                    Ok([(_, a), (_, b)]) if a.len() == 2 && b.len() == 2 => Some([a, b]),
                    _ => None,
                }
            }
            _ => None,
        }
    }

    /// The ruleset plays a set and the room makes two sides.
    fn plays_set(&self) -> bool {
        self.ruleset.best_of > 0 && self.set_sides().is_some()
    }

    /// The ruleset plays a set or strikes every game, but the room does not make two sides: the
    /// status line's note (its games are single games, nothing waits).
    fn two_sides_note(&self) -> Option<&'static str> {
        if self.set_sides().is_some() {
            return None;
        }
        // More players than two sides take: the others can sit out.
        let more = self.playing().len() > 2;
        if self.ruleset.best_of > 0 {
            Some(if more { msg::SETS_SIT_OUT } else { msg::SETS_NEED_TWO_SIDES })
        } else if room_set::strikes_every_game(&self.ruleset) {
            Some(if more { msg::STRIKES_SIT_OUT } else { msg::STRIKES_NEED_TWO_SIDES })
        } else {
            None
        }
    }

    /// The set is on and the room plays it: slots, Teams and rules wait for it.
    fn set_on(&self) -> bool {
        self.set.as_ref().is_some_and(|s| s.on())
    }

    /// Game 1 of a set is a blind pick (room-rules.md §2): the others' characters stay hidden.
    fn blind(&self) -> bool {
        self.plays_set() && self.set.as_ref().is_none_or(|s| s.finished || s.game == 1)
    }

    fn ruleset_view(&self) -> rooms::RoomRuleset {
        rooms::RoomRuleset {
            code: self.ruleset_code.clone(),
            name: self.ruleset.name.clone(),
            version: self.ruleset.version,
            by: self.ruleset.by.clone(),
            based_on: self.based_on.clone(),
            best_of: self.ruleset.best_of,
            stage_choice: self.ruleset.stage_choice,
            stages: self.ruleset.stages.clone(),
            alternate_stages: self.ruleset.alternate_stages,
            strikes: self.ruleset.strikes.clone(),
            bans: self.ruleset.bans,
            dsr: self.ruleset.dsr,
        }
    }

    fn status_text(&self, me: &Member) -> String {
        match &self.phase {
            Phase::Starting { .. } => msg::STARTING_GAME.into(),
            Phase::InGame { .. } if me.in_game => msg::IN_GAME.into(),
            Phase::InGame { .. } => msg::WAITING_FOR_GAME.into(),
            Phase::Waiting => {
                let players = self.player_count();
                let playing = self.playing();
                if let Some(set) = self.set.as_ref().filter(|s| s.paused()) {
                    let view = set.view();
                    return match view.waiting_for {
                        Some(name) => msg::set_paused(&name),
                        None if view.can_resume => msg::SET_HOST_DECIDES.into(),
                        None => msg::SET_RESULTS_DIFFER.into(),
                    };
                }
                // After a set: its result until someone readies for the next one (decision B).
                if let Some((set, w)) = self.set.as_ref().and_then(|s| s.winner.map(|w| (s, w))) {
                    if self.members().all(|(_, m)| !m.ready) {
                        return msg::set_won(&set.side_name(w), set.wins[w], set.wins[1 - w]);
                    }
                }
                if players == 1 {
                    msg::ROOM_WAITING.to_string()
                } else if playing.len() < 2 {
                    msg::NEED_TWO_PLAYERS.into()
                } else if self.one_colour() {
                    msg::PICK_TEAMS.into()
                } else {
                    // The players not ready yet, and anyone still in the last game.
                    let waiting: Vec<&str> = self
                        .members()
                        .filter(|(i, m)| m.in_game || (!m.ready && playing.contains(i)))
                        .map(|(_, m)| m.info.display_name.as_str())
                        .collect();
                    let none_ready = playing.iter().all(|&i| self.slots[i].as_ref().is_some_and(|m| !m.ready));
                    if waiting.is_empty() {
                        msg::STARTING_GAME.into()
                    } else if let Some(note) = self.two_sides_note().filter(|_| none_ready) {
                        // Nobody ready yet: why this room plays single games.
                        note.into()
                    } else {
                        msg::waiting_on(&waiting)
                    }
                }
            }
        }
    }

    fn state_for(&self, slot: usize) -> Option<RoomState> {
        let me = self.slots[slot].as_ref()?;
        let blind = self.blind();
        Some(RoomState {
            kind: rooms::ROOM_STATE.into(),
            code: self.code.clone(),
            public: self.public,
            teams: self.teams,
            team_colors: self.teams && self.team_colors,
            mode: self.mode(),
            status: self.status(),
            you: slot as u8 + 1,
            host: self.host as u8 + 1,
            slots: (0..ROOM_SLOTS)
                .map(|i| RoomSlot {
                    slot: i as u8 + 1,
                    open: self.open[i],
                    host: i == self.host,
                    player: self.slots[i].as_ref().map(|m| RoomPlayer {
                        display_name: m.info.display_name.clone(),
                        connect_code: m.info.connect_code.clone(),
                        ready: m.ready,
                        team: m.team,
                        // The coin placed on the CSS, before the player readies too (the user's,
                        // 2026-10-18; `docs/design/rooms.md` #15 showed only the lock-in), but not
                        // to the others in game 1 of a set (a blind pick).
                        character: m.character.filter(|_| !blind || i == slot),
                        costume: m.costume.filter(|_| !blind || i == slot),
                        // Only with the rules' stock control (a pick left from before it was
                        // turned off shows nothing and plays nothing).
                        stocks: m
                            .stocks
                            .filter(|_| m.ready && (!blind || i == slot) && self.ruleset.rules.stock_control),
                        hp: m.hp.filter(|_| {
                            m.ready
                                && (!blind || i == slot)
                                && self.ruleset.rules.special.stamina == common::rules::Stamina::Stamina
                        }),
                        in_game: m.in_game,
                        sitting_out: self.sits_out(m),
                        watching: m.watching.is_some(),
                    }),
                })
                .collect(),
            status_text: self.status_text(me),
            rules: self.ruleset.rules.clone(),
            ruleset: self.ruleset_view(),
            set: self.set.as_ref().map(|s| s.view()),
            pickers: self.next_pickers(&self.playing()),
        })
    }

    fn public_entry(&self) -> PublicRoom {
        let host = self.slots[self.host].as_ref().map(|m| m.info.display_name.clone()).unwrap_or_default();
        PublicRoom {
            code: self.code.clone(),
            host,
            players: self.player_count(),
            open_slots: self.open_count(),
            mode: self.mode(),
            status: if matches!(self.phase, Phase::Waiting) { RoomStatus::Waiting } else { RoomStatus::InGame },
            names: self.members().map(|(_, m)| m.info.display_name.clone()).collect(),
            joinable: self.free_slot().is_some(),
            ruleset: self.ruleset_view(),
            name: self.name.clone(),
        }
    }
}

/// What a room game ticket did.
#[derive(Debug, Default)]
pub struct TicketOutcome {
    /// The same member's earlier ticket for this start (it is replaced).
    pub replaced: Option<ConnId>,
    /// Every member's ticket is in: answer each of these connections with its response.
    pub matched: Vec<(ConnId, GetTicketResp)>,
    /// Every member's ticket is in: the game to record in `mm_matches` (match history).
    pub record: Option<RoomRecord>,
}

/// A room's game as `mm_matches` records it.
#[derive(Debug, Clone, PartialEq)]
pub struct RoomRecord {
    pub match_id: String,
    /// In slot order.
    pub players: Vec<Uuid>,
    /// The room's host (the session's decider).
    pub host: Uuid,
    pub stages: Vec<u16>,
    pub room: common::db::MatchRoom,
}

pub struct RoomBook {
    cfg: RoomsConfig,
    rooms: HashMap<String, Room>,
    /// Online connection → the code of its room.
    member_of: HashMap<ConnId, String>,
    /// Room game ticket connection → the code of its room.
    ticket_of: HashMap<ConnId, String>,
    failed_tickets: Vec<(ConnId, String)>,
    /// [`Self::take_watch_stops`].
    watch_stops: Vec<ConnId>,
    /// Rooms whose players changed (code, serial), for [`Self::take_changed`].
    changed: Vec<(String, u64)>,
    join_seq: u64,
    match_counter: u64,
    room_counter: u64,
    create_limiter: RateLimiter<Uuid>,
    join_limiter: RateLimiter<Uuid>,
    request_limiter: RateLimiter<ConnId>,
}

fn send(out: &mut Vec<Output>, conn: ConnId, m: &impl serde::Serialize) {
    let json = serde_json::to_string(m).expect("room messages serialize");
    out.push(Output::Send { conn, json });
}

fn room_error(out: &mut Vec<Output>, conn: ConnId, op: &str, error: &str) {
    send(out, conn, &RoomError { kind: rooms::ROOM_ERROR.into(), op: op.into(), error: error.into() });
}

impl RoomBook {
    pub fn new(cfg: RoomsConfig) -> Self {
        RoomBook {
            create_limiter: RateLimiter::new(&cfg.create_windows),
            join_limiter: RateLimiter::new(&cfg.join_windows),
            request_limiter: RateLimiter::new(&cfg.request_windows),
            cfg,
            rooms: HashMap::new(),
            member_of: HashMap::new(),
            ticket_of: HashMap::new(),
            failed_tickets: Vec::new(),
            watch_stops: Vec::new(),
            changed: Vec::new(),
            join_seq: 0,
            match_counter: 0,
            room_counter: 0,
        }
    }

    pub fn room_count(&self) -> usize {
        self.rooms.len()
    }

    /// A live room has this code (Brawl Mods rooms take codes no P+ room has).
    pub fn has_code(&self, code: &str) -> bool {
        self.rooms.contains_key(code)
    }

    /// The code of the room this online connection is in.
    pub fn room_of(&self, conn: ConnId) -> Option<&str> {
        self.member_of.get(&conn).map(String::as_str)
    }

    /// Tickets that have to be answered with an error (and closed) by the engine.
    pub fn take_failed_tickets(&mut self) -> Vec<(ConnId, String)> {
        std::mem::take(&mut self.failed_tickets)
    }

    /// The rooms whose players changed since the last call, as (code, serial), each once, in the
    /// order they changed. A room that closed is in it too (and is gone from [`Self::players`]).
    pub fn take_changed(&mut self) -> Vec<(String, u64)> {
        let mut changed = std::mem::take(&mut self.changed);
        let mut seen = HashSet::new();
        changed.retain(|c| seen.insert(c.clone()));
        changed
    }

    /// The online connections and accounts of a room's players in joining order, if the room
    /// with this code is still the one with this serial.
    pub fn players(&self, code: &str, serial: u64) -> Vec<(ConnId, Uuid)> {
        let Some(room) = self.rooms.get(code).filter(|r| r.serial == serial) else { return Vec::new() };
        let mut players: Vec<&Member> = room.members().map(|(_, m)| m).collect();
        players.sort_by_key(|m| m.joined);
        players.into_iter().map(|m| (m.conn, m.info.uid)).collect()
    }

    /// Public rooms for the launcher: joinable first, then waiting before in game, then fuller,
    /// then older.
    pub fn public_rooms(&self, limit: usize) -> Vec<PublicRoom> {
        let mut list: Vec<(&Room, PublicRoom)> =
            self.rooms.values().filter(|r| r.public).map(|r| (r, r.public_entry())).collect();
        list.sort_by(|(ra, a), (rb, b)| {
            b.joinable
                .cmp(&a.joinable)
                .then((a.status != RoomStatus::Waiting).cmp(&(b.status != RoomStatus::Waiting)))
                .then(b.players.cmp(&a.players))
                .then(ra.created.cmp(&rb.created))
                .then(a.code.cmp(&b.code))
        });
        list.into_iter().take(limit).map(|(_, p)| p).collect()
    }

    /// Every room player's room ([`MmStatus::members`](common::rooms::MmStatus::members)), at most
    /// `limit` players.
    pub fn member_rooms(&self, limit: usize) -> Vec<RoomMember> {
        let mut list = Vec::new();
        for room in self.rooms.values() {
            let entry = room.public_entry();
            for (i, m) in room.members() {
                if list.len() >= limit {
                    return list;
                }
                list.push(RoomMember {
                    uid: m.info.uid.to_string(),
                    code: entry.code.clone(),
                    players: entry.players,
                    open_slots: entry.open_slots,
                    mode: entry.mode,
                    status: entry.status,
                    joinable: entry.joinable,
                    host: i == room.host,
                    name: entry.name.clone(),
                });
            }
        }
        list
    }

    /// The next game's pickers changed (someone sits out, plays again, joins or left): whoever
    /// gained or lost the pick readies again, since their lock-in said whether they pick (the
    /// game's STAGE_PENDING).
    /// P+ default, a room's ruleset until the host picks one: [`Ruleset::default`] with the server's
    /// match rules and P+'s alternate stages on, as Direct (the user's, 2026-10-27, bug report #164:
    /// players hold L/R/Z on the loser's pick). Not in `Ruleset::default`, which fills a published
    /// ruleset's missing keys: every ruleset written without `alternateStages` keeps it off.
    fn pplus_default(&self) -> Ruleset {
        Ruleset { rules: self.cfg.rules.rules.clone(), alternate_stages: true, ..Default::default() }
    }

    fn settle_pickers(&mut self, code: &str, before: Option<u8>) {
        let Some(room) = self.rooms.get_mut(code) else { return };
        if !matches!(room.phase, Phase::Waiting) {
            return;
        }
        let changed = before.unwrap_or(0) ^ room.next_pickers(&room.playing()).unwrap_or(0);
        for (i, slot) in room.slots.iter_mut().enumerate() {
            if let Some(m) = slot.as_mut().filter(|m| m.ready && changed & (1 << i) != 0) {
                tracing::info!(%code, slot = i + 1, "room stage pick moved: ready taken back");
                m.ready = false;
            }
        }
    }

    fn pickers_now(&self, code: &str) -> Option<u8> {
        self.rooms.get(code).and_then(|r| r.next_pickers(&r.playing()))
    }

    fn broadcast(&self, code: &str, out: &mut Vec<Output>) {
        let Some(room) = self.rooms.get(code) else { return };
        for (slot, m) in room.members() {
            if let Some(state) = room.state_for(slot) {
                send(out, m.conn, &state);
            }
        }
    }

    /// One `room-*` request from an online connection.
    pub fn request(&mut self, now: Instant, conn: ConnId, who: &MemberInfo, req: RoomRequest, out: &mut Vec<Output>) {
        let op = req.op();
        if self.request_limiter.check(&conn, now).is_err() {
            return room_error(out, conn, op, msg::TOO_MANY_REQUESTS);
        }
        let result = match req {
            RoomRequest::Create { public, ruleset, remembered } => {
                self.create(now, conn, who, public, out).map(|()| {
                    // The room is there at once with P+'s default; the ruleset follows its lookup.
                    if ruleset.is_some() || remembered {
                        if let Some(room) = self.member_of.get(&conn).cloned() {
                            out.push(Output::FetchRuleset(RulesetFetch {
                                conn,
                                uid: who.uid,
                                room,
                                code: ruleset,
                                op: rooms::ROOM_CREATE,
                                remember: false,
                            }));
                        }
                    }
                })
            }
            RoomRequest::Ruleset { code } => self.ruleset_request(conn, who, code, out),
            RoomRequest::RulesetList => {
                out.push(Output::FetchRulesetList { conn, uid: who.uid });
                Ok(())
            }
            RoomRequest::Join { code } => self.join(now, conn, who, &code, out),
            RoomRequest::Leave => match self.member_of.get(&conn).cloned() {
                Some(code) => {
                    self.leave(now, &code, conn, None, true, out);
                    Ok(())
                }
                None => Err(msg::NOT_IN_ROOM),
            },
            other => self.change(now, conn, other, out),
        };
        if let Err(e) = result {
            tracing::debug!(conn, op, "room request refused: {e}");
            room_error(out, conn, op, e);
        }
    }

    fn create(
        &mut self,
        now: Instant,
        conn: ConnId,
        who: &MemberInfo,
        public: bool,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        if self.create_limiter.check(&who.uid, now).is_err() {
            return Err(msg::ROOMS_TOO_OFTEN);
        }
        if self.rooms.len() >= self.cfg.max_rooms {
            return Err(msg::NO_ROOMS_LEFT);
        }
        let mut rng = rand::thread_rng();
        let code = (0..64)
            .map(|_| rooms::random_room_code(&mut rng))
            .find(|c| !self.rooms.contains_key(c))
            .ok_or(msg::NO_ROOMS_LEFT)?;
        if let Some(old) = self.member_of.get(&conn).cloned() {
            self.leave(now, &old, conn, None, true, out);
        }
        self.join_seq += 1;
        self.room_counter += 1;
        let mut slots: [Option<Member>; ROOM_SLOTS] = Default::default();
        slots[0] = Some(Member {
            conn,
            info: who.clone(),
            joined: self.join_seq,
            ready: false,
            team: 0,
            character: None,
            costume: None,
            stocks: None,
            hp: None,
            in_game: false,
            sitting_out: false,
            watch: false,
            watching: None,
        });
        tracing::info!(%code, host = %who.connect_code, public, "room created");
        self.rooms.insert(
            code.clone(),
            Room {
                code: code.clone(),
                name: who.room_name.clone(),
                public,
                teams: false,
                team_colors: false,
                open: [true, true, false, false],
                slots,
                host: 0,
                phase: Phase::Waiting,
                created: now,
                serial: self.room_counter,
                ruleset: self.pplus_default(),
                ruleset_code: None,
                based_on: None,
                set: None,
                public_before_pause: None,
                pickers: None,
                pick_owed: false,
                pick_draw: 0,
                game_over_at: None,
            },
        );
        self.changed.push((code.clone(), self.room_counter));
        self.member_of.insert(conn, code.clone());
        self.broadcast(&code, out);
        Ok(())
    }

    fn join(
        &mut self,
        now: Instant,
        conn: ConnId,
        who: &MemberInfo,
        typed: &str,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        if self.join_limiter.check(&who.uid, now).is_err() {
            return Err(msg::JOINS_TOO_OFTEN);
        }
        let code = rooms::parse_room_code(typed).ok_or(msg::ROOM_NOT_FOUND)?;
        let room = self.rooms.get(&code).ok_or(msg::ROOM_NOT_FOUND)?;
        if room.slot_of(conn).is_some() {
            // Already in it: the answer is the state, as for a join.
            self.broadcast(&code, out);
            return Ok(());
        }
        // A set that waits keeps its missing players' slots for them (room-rules.md A).
        let reserved = room.set.as_ref().map(|s| s.reserved()).unwrap_or_default();
        let slot = match reserved.iter().find(|(kept, uid)| *uid == who.uid && room.open[*kept]) {
            Some((kept, _)) if room.slots[*kept].is_none() => *kept,
            _ => (0..ROOM_SLOTS)
                .find(|&i| room.open[i] && room.slots[i].is_none() && !reserved.iter().any(|(k, _)| *k == i))
                .ok_or(msg::ROOM_FULL)?,
        };
        if let Some(old) = self.member_of.get(&conn).cloned() {
            self.leave(now, &old, conn, None, true, out);
        }
        let before = self.pickers_now(&code);
        self.join_seq += 1;
        let seq = self.join_seq;
        let Some(room) = self.rooms.get_mut(&code) else { return Err(msg::ROOM_NOT_FOUND) };
        room.slots[slot] = Some(Member {
            conn,
            info: who.clone(),
            joined: seq,
            ready: false,
            // Default colours by slot: red, blue, green, red (a valid split for 4).
            team: slot as u8 % TEAM_COUNT,
            character: None,
            costume: None,
            stocks: None,
            hp: None,
            in_game: false,
            // Joiners play (the user's, 2026-10-18): the next game waits for them to ready.
            sitting_out: false,
            watch: false,
            watching: None,
        });
        tracing::info!(%code, player = %who.connect_code, slot = slot + 1, "joined room");
        if let Some(set) = room.set.as_mut() {
            if set.back(who.uid) {
                tracing::info!(%code, game = set.game, "room set goes on");
            }
        }
        room.settle_privacy();
        self.changed.push((code.clone(), room.serial));
        self.member_of.insert(conn, code.clone());
        self.settle_pickers(&code, before);
        self.broadcast(&code, out);
        Ok(())
    }

    /// Removes a member. `reason` goes to the member in `room-left` (with `notify`); a member whose
    /// connection is gone is not told.
    fn leave(
        &mut self,
        now: Instant,
        code: &str,
        conn: ConnId,
        reason: Option<&str>,
        notify: bool,
        out: &mut Vec<Output>,
    ) {
        self.member_of.remove(&conn);
        let before = self.pickers_now(code);
        if notify {
            send(
                out,
                conn,
                &RoomLeft { kind: rooms::ROOM_LEFT.into(), code: code.into(), reason: reason.map(String::from) },
            );
        }
        let Some(room) = self.rooms.get_mut(code) else { return };
        let Some(slot) = room.slot_of(conn) else { return };
        let gone = room.slots[slot].take();
        if gone.as_ref().is_some_and(|m| m.watch) {
            self.watch_stops.push(conn);
        }
        if let (Some(set), Some(m)) = (room.set.as_mut(), gone.as_ref()) {
            if let Some(side) = set.side_of(m.info.uid) {
                if !set.on() {
                    // Another player after this set: the next one starts from 0-0 (decision B).
                    room.set = None;
                } else if reason == Some(msg::REMOVED) || reason == Some(msg::BANNED) {
                    // The host closed their slot, or the account was banned: their side forfeits
                    // the set (decision A) instead of the set waiting for them.
                    set.forfeit(side);
                    tracing::info!(%code, side, "room set forfeited");
                } else if set.left(m.info.uid) {
                    tracing::info!(%code, side, game = set.game - 1, "room set game lost by leaving");
                }
            }
        }
        room.settle_privacy();
        // A picker who left picks nothing (the pick is owed to the next game's players).
        if let Some(p) = room.pickers.as_mut() {
            *p &= !(1 << slot);
        }
        self.changed.push((code.to_string(), room.serial));
        tracing::info!(
            %code,
            player = gone.as_ref().map(|m| m.info.connect_code.as_str()).unwrap_or(""),
            slot = slot + 1,
            reason = reason.unwrap_or("left"),
            "left room"
        );
        if room.player_count() == 0 {
            if let Phase::Starting { tickets, .. } = &room.phase {
                for t in tickets.values() {
                    self.ticket_of.remove(&t.conn);
                    self.failed_tickets.push((t.conn, msg::PLAYER_LEFT.into()));
                }
            }
            for (c, room_code) in self.ticket_of.clone() {
                if room_code == code {
                    self.ticket_of.remove(&c);
                }
            }
            self.rooms.remove(code);
            tracing::info!(%code, "room closed");
            return;
        }
        if slot == room.host {
            // The earliest-joined player takes over (`docs/design/rooms.md` #9).
            if let Some((next, m)) = room.members().min_by_key(|(_, m)| m.joined) {
                tracing::info!(%code, host = %m.info.connect_code, "room host handed over");
                room.host = next;
            }
        }
        // A start waits only for its players' tickets: someone who sits it out may leave.
        let (starting, in_game) = (
            matches!(&room.phase, Phase::Starting { players, .. } if players.contains(&slot)),
            matches!(room.phase, Phase::InGame { .. }),
        );
        if starting {
            self.cancel_start(code, msg::PLAYER_LEFT, out);
        } else if in_game {
            self.maybe_game_over(now, code, out);
        }
        self.settle_pickers(code, before);
        self.broadcast(code, out);
        self.maybe_start(now, code, out);
    }

    /// Requests that change a room the connection is in.
    fn change(
        &mut self,
        now: Instant,
        conn: ConnId,
        req: RoomRequest,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        let code = self.member_of.get(&conn).cloned().ok_or(msg::NOT_IN_ROOM)?;
        let before = self.pickers_now(&code);
        let room = self.rooms.get_mut(&code).ok_or(msg::NOT_IN_ROOM)?;
        let me = room.slot_of(conn).ok_or(msg::NOT_IN_ROOM)?;
        let is_host = me == room.host;
        let waiting = matches!(room.phase, Phase::Waiting);
        let starting = matches!(room.phase, Phase::Starting { .. });
        let mut kick: Option<ConnId> = None;
        match req {
            RoomRequest::Slot { slot, open } => {
                let s = slot as usize - 1;
                if !is_host {
                    return Err(msg::HOST_ONLY);
                }
                if !waiting {
                    return Err(msg::BETWEEN_GAMES);
                }
                // Closing the held slot of a set player who left forfeits the set for their side
                // (decision A); a 1v1 room keeps its two slots open.
                let held = room
                    .set
                    .as_ref()
                    .and_then(|x| x.reserved().iter().find(|(k, _)| *k == s).and_then(|(_, uid)| x.side_of(*uid)));
                if let (false, Some(side), true) = (open, held, room.slots[s].is_none()) {
                    if let Some(set) = room.set.as_mut() {
                        set.forfeit(side);
                    }
                    tracing::info!(%code, side, "room set forfeited (held slot closed)");
                    if room.open_count() > 2 {
                        room.open[s] = false;
                    }
                    room.settle_privacy();
                    self.broadcast(&code, out);
                    return Ok(());
                }
                // Closing a set player's slot (a kick) forfeits the set; anything else waits for it.
                let kicks_a_set_player = !open
                    && room.slots[s]
                        .as_ref()
                        .is_some_and(|m| room.set.as_ref().is_some_and(|x| x.side_of(m.info.uid).is_some()));
                if room.set_on() && !kicks_a_set_player {
                    return Err(msg::SET_ON);
                }
                if open {
                    room.open[s] = true;
                } else if room.open[s] {
                    if s == me {
                        return Err(msg::OWN_SLOT);
                    }
                    // Closing an occupied slot removes its player, who may join again once a slot
                    // is open (`docs/design/rooms.md` #10). With two open the slot stays open: the
                    // host of a 1v1 room can remove a player too (player feedback 2026-10-12).
                    kick = room.slots[s].as_ref().map(|m| m.conn);
                    if room.open_count() > 2 {
                        room.open[s] = false;
                    } else if kick.is_none() {
                        return Err(msg::MIN_OPEN_SLOTS);
                    }
                }
            }
            RoomRequest::Teams { on, colors } => {
                let colors = on && colors;
                if !is_host {
                    return Err(msg::HOST_ONLY);
                }
                if !waiting {
                    return Err(msg::BETWEEN_GAMES);
                }
                if room.set_on() {
                    return Err(msg::SET_ON);
                }
                if room.teams != on || room.team_colors != colors {
                    room.teams = on;
                    room.team_colors = colors;
                    room.set = None;
                    // Everyone's lock-in was made for the other mode (with the team colours, a
                    // costume of the team's).
                    for m in room.slots.iter_mut().flatten() {
                        m.ready = false;
                    }
                }
            }
            RoomRequest::Public { public } => {
                if !is_host {
                    return Err(msg::HOST_ONLY);
                }
                room.public = public;
                // The host's choice while a set waits is the one the room goes back to.
                if room.public_before_pause.is_some() {
                    room.public_before_pause = Some(public);
                    room.public = false;
                }
            }
            RoomRequest::Ready { ready, character, costume, stocks, hp } => {
                if starting {
                    return Err(msg::ROOM_STARTING);
                }
                let outside_set = room.slots[me].as_ref().is_some_and(|m| !m.sitting_out && room.sits_out(m));
                let m = room.slots[me].as_mut().ok_or(msg::NOT_IN_ROOM)?;
                if m.in_game {
                    return Err(msg::BETWEEN_GAMES);
                }
                if ready && outside_set {
                    return Err(msg::SET_SIT_OUT);
                }
                // Ready or not, the coin's character (none: in the hand), shown to the others.
                m.ready = ready;
                m.character = character;
                m.costume = costume;
                m.stocks = stocks.filter(|_| ready);
                m.hp = hp.filter(|_| ready);
                // A ready plays (the game sends `room-sit-out` first; an older one only readies),
                // and is back from watching.
                if ready {
                    m.sitting_out = false;
                    m.watching = None;
                }
            }
            RoomRequest::SitOut { slot, sit_out } => {
                let s = slot as usize - 1;
                if s != me && !is_host {
                    return Err(msg::HOST_ONLY);
                }
                if starting {
                    return Err(msg::ROOM_STARTING);
                }
                let m = room.slots[s].as_ref().ok_or(msg::INVALID_REQUEST)?;
                if m.in_game {
                    return Err(msg::BETWEEN_GAMES);
                }
                // A set that is on is played by its sides to the end; everyone else waits for it.
                if room.set_on() {
                    return Err(msg::SET_SIT_OUT);
                }
                let m = room.slots[s].as_mut().ok_or(msg::INVALID_REQUEST)?;
                if m.sitting_out != sit_out {
                    tracing::info!(%code, slot, sit_out, by = me + 1, "room sit out");
                }
                m.sitting_out = sit_out;
                if sit_out {
                    m.ready = false;
                }
            }
            RoomRequest::Watch { want, watching } => {
                let played = room.slots[me].as_ref().is_some_and(|m| room.played_game(m));
                let m = room.slots[me].as_mut().ok_or(msg::NOT_IN_ROOM)?;
                if let Some(w) = want {
                    if m.watch && !w {
                        self.watch_stops.push(conn);
                    }
                    m.watch = w;
                }
                let Some(w) = watching else {
                    // Nothing the room shows changed.
                    return Ok(());
                };
                if w != m.watching.is_some() {
                    tracing::info!(%code, slot = me + 1, watching = w, "room watch");
                }
                // A player of the game is in it (or played it), not watching it.
                let since = m.watching.unwrap_or(now);
                m.watching = (w && !m.in_game && !played).then_some(since);
                // Their game shows the match, not the CSS: no ready from before stays.
                if m.watching.is_some() {
                    m.ready = false;
                }
            }
            RoomRequest::Team { team } => {
                if starting {
                    return Err(msg::ROOM_STARTING);
                }
                let m = room.slots[me].as_mut().ok_or(msg::NOT_IN_ROOM)?;
                if m.in_game {
                    return Err(msg::BETWEEN_GAMES);
                }
                m.team = team.min(TEAM_COUNT - 1);
            }
            RoomRequest::Back { pickers } => {
                // Only a player of the game knows who picks next (the last to come back saw the
                // game end); of its pickers, those who left the room meanwhile pick nothing.
                let still_here = match &room.phase {
                    Phase::InGame { players, .. } => players
                        .iter()
                        .filter(|(i, uid)| room.slots[*i].as_ref().is_some_and(|m| m.info.uid == *uid))
                        .fold(0u8, |bits, (i, _)| bits | 1 << i),
                    _ => 0,
                };
                if let Some(m) = room.slots[me].as_mut() {
                    if m.in_game {
                        if let Some(p) = pickers {
                            room.pickers = Some(p & still_here);
                            room.pick_owed = p != 0;
                            room.pick_draw = rand::random();
                        }
                    }
                    m.in_game = false;
                }
                self.maybe_game_over(now, &code, out);
            }
            RoomRequest::Rules { rules } => {
                if !is_host {
                    return Err(msg::HOST_ONLY);
                }
                if !waiting {
                    return Err(msg::BETWEEN_GAMES);
                }
                if room.set_on() {
                    return Err(msg::SET_ON);
                }
                if room.ruleset.rules != rules {
                    room.ruleset.rules = rules;
                    room.edited();
                }
            }
            RoomRequest::BestOf { best_of } => {
                if !is_host {
                    return Err(msg::HOST_ONLY);
                }
                if !waiting {
                    return Err(msg::BETWEEN_GAMES);
                }
                if room.set_on() {
                    return Err(msg::SET_ON);
                }
                if room.ruleset.best_of != best_of {
                    room.ruleset = room.with_best_of(best_of)?;
                    tracing::info!(%code, best_of, "room best of");
                    room.edited();
                }
            }
            RoomRequest::Stages { stages, stage_choice } => {
                if !is_host {
                    return Err(msg::HOST_ONLY);
                }
                if !waiting {
                    return Err(msg::BETWEEN_GAMES);
                }
                if room.set_on() {
                    return Err(msg::SET_ON);
                }
                let r = room.with_stages(stages, stage_choice)?;
                if r != room.ruleset {
                    tracing::info!(%code, stages = ?r.stages, choice = ?r.stage_choice, "room stages");
                    room.ruleset = r;
                    room.edited();
                }
            }
            RoomRequest::Format(f) => {
                if !is_host {
                    return Err(msg::HOST_ONLY);
                }
                if !waiting {
                    return Err(msg::BETWEEN_GAMES);
                }
                if room.set_on() {
                    return Err(msg::SET_ON);
                }
                let r = room.with_format(&f)?;
                if r != room.ruleset {
                    tracing::info!(%code, choice = ?r.stage_choice, strikes = ?r.strikes, bans = r.bans, dsr = r.dsr,
                        alternates = r.alternate_stages, "room format");
                    room.ruleset = r;
                    room.edited();
                }
            }
            RoomRequest::Result { game, winner_side, stage } => {
                let uid = room.slots[me].as_ref().map(|m| m.info.uid).ok_or(msg::NOT_IN_ROOM)?;
                let set = room.set.as_mut().ok_or(msg::NO_SET_GAME)?;
                match set.report(uid, game, winner_side, stage) {
                    room_set::Reported::Ignored => return Err(msg::NO_SET_GAME),
                    r => {
                        tracing::info!(%code, game, winner_side, stage, ?r, "set game reported");
                        // Each game of the set the players agree on (a quit or a runback is not one).
                        if let (room_set::Reported::Counted, Some(ruleset)) = (r, &room.ruleset_code) {
                            out.push(Output::RulesetPlayed { code: ruleset.clone() });
                        }
                    }
                }
            }
            RoomRequest::Set { resume } => {
                if !is_host {
                    return Err(msg::HOST_ONLY);
                }
                // End set also works between a running set's games: its session is on the CSS or
                // the stage select (the game's match not started, room-step); never in a match.
                // The players' games end the session when the set is gone from room-state.
                let between_set_games = !resume
                    && matches!(room.phase, Phase::InGame { .. })
                    && room.set.as_ref().is_some_and(|s| s.on() && !s.steps.match_started);
                if !waiting && !between_set_games {
                    return Err(msg::BETWEEN_GAMES);
                }
                // `resume`: results that differed, the game is played again. `restart` (RULES > SET >
                // End set): any set ends with no result; the games played stay as played, nobody
                // wins it, and the next all-ready starts a new one.
                let can_resume = room.set.as_ref().is_some_and(|s| s.view().can_resume);
                if resume {
                    if !can_resume {
                        return Err(msg::SET_NOT_RESUMABLE);
                    }
                    if let Some(s) = room.set.as_mut() {
                        s.replay_game();
                    }
                } else {
                    room.set = None;
                    room.settle_privacy();
                }
                for m in room.slots.iter_mut().flatten() {
                    m.ready = false;
                }
            }
            RoomRequest::Step { game, actions, match_started } => {
                let uid = room.slots[me].as_ref().map(|m| m.info.uid).ok_or(msg::NOT_IN_ROOM)?;
                let set = room.set.as_mut().ok_or(msg::NO_SET_GAME)?;
                if set.report_steps(uid, game, actions, match_started) {
                    tracing::debug!(%code, game, match_started, "set steps");
                }
                // Kept for the next ticket; nothing the room shows changed.
                return Ok(());
            }
            RoomRequest::Unreachable { match_id, slots } => {
                // Another game's report (it ended meanwhile) changes nothing.
                let from = me as u8 + 1;
                let codes: Vec<String> = room
                    .slots
                    .iter()
                    .map(|m| m.as_ref().map_or_else(|| "?".to_string(), |m| m.info.connect_code.clone()))
                    .collect();
                let Phase::InGame { match_id: current, unreachable, .. } = &mut room.phase else {
                    return Ok(());
                };
                if *current != match_id {
                    return Ok(());
                }
                let mut new = false;
                for slot in slots.into_iter().filter(|s| *s != from) {
                    let pair = [from.min(slot), from.max(slot)];
                    if unreachable.insert(pair) {
                        new = true;
                        tracing::info!(
                            %code,
                            %match_id,
                            a = %codes[pair[0] as usize - 1],
                            b = %codes[pair[1] as usize - 1],
                            reporter = %codes[me],
                            "room pair could not connect"
                        );
                    }
                }
                if new {
                    let m = RoomUnreachable {
                        kind: rooms::ROOM_UNREACHABLE.into(),
                        code: code.clone(),
                        match_id,
                        pairs: unreachable.iter().copied().collect(),
                    };
                    for (_, member) in room.members() {
                        send(out, member.conn, &m);
                    }
                }
                // Nothing the room's state shows changed.
                return Ok(());
            }
            RoomRequest::Create { .. }
            | RoomRequest::Join { .. }
            | RoomRequest::Leave
            | RoomRequest::Ruleset { .. }
            | RoomRequest::RulesetList => {
                unreachable!("handled in request()")
            }
        }
        if let Some(k) = kick {
            // Broadcasts the new state (without the kicked player) too.
            self.leave(now, &code, k, Some(msg::REMOVED), true, out);
        } else {
            self.settle_pickers(&code, before);
            self.broadcast(&code, out);
            self.maybe_start(now, &code, out);
        }
        Ok(())
    }

    /// `room-ruleset`: the host plays a published ruleset (looked up first) or P+'s default.
    fn ruleset_request(
        &mut self,
        conn: ConnId,
        who: &MemberInfo,
        code: Option<String>,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        let room_code = self.member_of.get(&conn).cloned().ok_or(msg::NOT_IN_ROOM)?;
        let room = self.rooms.get(&room_code).ok_or(msg::NOT_IN_ROOM)?;
        if room.slot_of(conn) != Some(room.host) {
            return Err(msg::HOST_ONLY);
        }
        if !matches!(room.phase, Phase::Waiting) {
            return Err(msg::BETWEEN_GAMES);
        }
        if room.set_on() {
            return Err(msg::SET_ON);
        }
        match code {
            None => {
                let default = self.pplus_default();
                self.apply_ruleset(&room_code, default, None, out)?;
                // P+'s default becomes the remembered choice too (decision 12).
                out.push(Output::FetchRuleset(RulesetFetch {
                    conn,
                    uid: who.uid,
                    room: room_code,
                    code: None,
                    op: rooms::ROOM_RULESET,
                    remember: true,
                }));
                Ok(())
            }
            Some(code) => {
                out.push(Output::FetchRuleset(RulesetFetch {
                    conn,
                    uid: who.uid,
                    room: room_code,
                    code: Some(code),
                    op: rooms::ROOM_RULESET,
                    remember: true,
                }));
                Ok(())
            }
        }
    }

    /// A ruleset lookup's answer: the room plays it (if it is still waiting and its host asked).
    pub fn ruleset_fetched(
        &mut self,
        f: RulesetFetch,
        result: Result<Option<(String, Ruleset)>, String>,
        out: &mut Vec<Output>,
    ) {
        let error = match result {
            // Clearing the remembered ruleset (P+'s default) has nothing to apply.
            Ok(None) if f.code.is_none() => return,
            Ok(Some((code, ruleset))) => match self.apply_ruleset(&f.room, ruleset, Some(code), out) {
                Ok(()) => return,
                Err(e) => e,
            },
            Ok(None) => msg::RULESET_NOT_FOUND,
            Err(e) => {
                tracing::warn!(room = %f.room, "ruleset lookup failed: {e}");
                msg::RULESETS_UNAVAILABLE
            }
        };
        if self.member_of.get(&f.conn) == Some(&f.room) {
            room_error(out, f.conn, f.op, error);
        }
    }

    /// The room plays `ruleset` from now on: a published one (`code`), or P+'s default (`None`).
    /// Like a rules change it takes back every ready; it waits for a set that is on.
    pub fn apply_ruleset(
        &mut self,
        room_code: &str,
        ruleset: Ruleset,
        code: Option<String>,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        let room = self.rooms.get_mut(room_code).ok_or(msg::ROOM_NOT_FOUND)?;
        if !matches!(room.phase, Phase::Waiting) {
            return Err(msg::BETWEEN_GAMES);
        }
        if room.set_on() {
            return Err(msg::SET_ON);
        }
        tracing::info!(code = %room_code, ruleset = ?code, name = %ruleset.name, "room ruleset");
        room.ruleset = ruleset;
        room.ruleset_code = code;
        room.based_on = None;
        room.set = None;
        for m in room.slots.iter_mut().flatten() {
            m.ready = false;
        }
        self.broadcast(room_code, out);
        Ok(())
    }

    /// Back to waiting once nobody is still in the game. A game without a set that lasted
    /// [`COUNT_GAME_AFTER`] counts as played with the room's published ruleset.
    fn maybe_game_over(&mut self, now: Instant, code: &str, out: &mut Vec<Output>) {
        let Some(room) = self.rooms.get_mut(code) else { return };
        if matches!(room.phase, Phase::InGame { .. }) && room.members().all(|(_, m)| !m.in_game) {
            tracing::info!(%code, "room game over");
            if let (Phase::InGame { since, set: false, .. }, Some(ruleset)) = (&room.phase, &room.ruleset_code) {
                if now.saturating_duration_since(*since) >= COUNT_GAME_AFTER {
                    out.push(Output::RulesetPlayed { code: ruleset.clone() });
                }
            }
            room.phase = Phase::Waiting;
            room.game_over_at = Some(Instant::now());
        }
    }

    /// The room games to send to watchers now: each member who wants them (`room-watch` `want`)
    /// and did not play their room's game, with that game's match id. The live book sends each one
    /// that game's kept chunks, also after the game is over (the upload runs behind the players).
    pub fn watch_targets(&self) -> Vec<(ConnId, String)> {
        let mut v = Vec::new();
        for room in self.rooms.values() {
            let Phase::InGame { match_id, .. } = &room.phase else { continue };
            for (_, m) in room.members() {
                if m.watch && !m.in_game && !room.played_game(m) {
                    v.push((m.conn, match_id.clone()));
                }
            }
        }
        v
    }

    /// Members whose watch stopped (`want: false`, or they left the room) since the last call.
    pub fn take_watch_stops(&mut self) -> Vec<ConnId> {
        std::mem::take(&mut self.watch_stops)
    }

    /// Starts the game once two or more play and all of them are ready.
    fn maybe_start(&mut self, now: Instant, code: &str, out: &mut Vec<Output>) {
        let Some(room) = self.rooms.get_mut(code) else { return };
        if !room.can_start() {
            return;
        }
        let players = room.playing();
        let host = room.game_host(&players);
        let pickers = room.next_pickers(&players);
        self.match_counter += 1;
        let match_id = format!(
            "mode.room-{code}-{}-{:x}",
            Utc::now().to_rfc3339_opts(chrono::SecondsFormat::Millis, true),
            self.match_counter
        );
        tracing::info!(
            %code,
            %match_id,
            players = ?players.iter().map(|i| i + 1).collect::<Vec<_>>(),
            host = host + 1,
            ?pickers,
            "room starting"
        );
        if room.set.as_ref().is_none_or(|s| s.finished) {
            room.set = match room.set_sides() {
                Some(sides) if room.ruleset.best_of > 0 => {
                    tracing::info!(%code, best_of = room.ruleset.best_of, "room set starts");
                    Some(SetProgress::new(room.ruleset.best_of, sides, rand::random()))
                }
                // Not a set here: single games (a finished set's score stays until then).
                _ if room.ruleset.best_of > 0 => None,
                _ => room.set.take(),
            };
        }
        room.phase = Phase::Starting {
            match_id: match_id.clone(),
            since: now,
            tickets: BTreeMap::new(),
            players: players.clone(),
            host,
        };
        let start = RoomStart {
            kind: rooms::ROOM_START.into(),
            code: code.into(),
            match_id,
            timeout_secs: self.cfg.start_timeout.as_secs(),
            players: players.iter().map(|&i| i as u8 + 1).collect(),
            host: Some(host as u8 + 1),
            pickers,
        };
        // Only the game's players send a ticket; the others see it start in `room-state`.
        for &i in &players {
            if let Some(m) = room.slots[i].as_ref() {
                send(out, m.conn, &start);
            }
        }
        self.broadcast(code, out);
    }

    /// A start that will not happen: its tickets fail, everyone has to ready up again.
    fn cancel_start(&mut self, code: &str, why: &str, out: &mut Vec<Output>) {
        let Some(room) = self.rooms.get_mut(code) else { return };
        let Phase::Starting { tickets, players, .. } = std::mem::replace(&mut room.phase, Phase::Waiting) else {
            return;
        };
        tracing::info!(%code, "room start cancelled: {why}");
        for t in tickets.values() {
            self.ticket_of.remove(&t.conn);
            self.failed_tickets.push((t.conn, why.into()));
        }
        for (i, slot) in room.slots.iter_mut().enumerate() {
            if let Some(m) = slot.as_mut().filter(|_| players.contains(&i)) {
                m.ready = false;
                room_error(out, m.conn, rooms::ROOM_START, why);
            }
        }
    }

    /// A room game ticket from a verified account (`create-ticket`, mode 3, the room code).
    /// `Err` is the refusal.
    pub fn ticket(
        &mut self,
        conn: ConnId,
        addr: SocketAddrV4,
        lan: String,
        uid: Uuid,
        code: &str,
    ) -> Result<TicketOutcome, &'static str> {
        let room = self.rooms.get_mut(code).ok_or(msg::ROOM_NOT_FOUND)?;
        let slot = room.members().find(|(_, m)| m.info.uid == uid).map(|(i, _)| i).ok_or(msg::NOT_A_MEMBER)?;
        let (match_id, tickets, old, member_slots, host) = {
            let Phase::Starting { match_id, tickets, players, host, .. } = &mut room.phase else {
                return Err(msg::ROOM_NOT_STARTING);
            };
            if !players.contains(&slot) {
                return Err(msg::NOT_IN_THIS_GAME);
            }
            let old = tickets.insert(slot, StartTicket { conn, addr, lan });
            (match_id.clone(), tickets.clone(), old, players.clone(), *host)
        };
        let mut outcome = TicketOutcome::default();
        if let Some(old) = old {
            if old.conn != conn {
                self.ticket_of.remove(&old.conn);
                outcome.replaced = Some(old.conn);
            }
        }
        self.ticket_of.insert(conn, code.to_string());
        if !member_slots.iter().all(|i| tickets.contains_key(i)) {
            return Ok(outcome);
        }
        // The set (or a struck game without one) the session plays.
        let set_ticket = if let Some(set) = room.set.as_ref().filter(|s| s.on()) {
            Some(set.ticket(&room.ruleset))
        } else if room_set::strikes_every_game(&room.ruleset) {
            room.sides_of(&member_slots).map(|sides| room_set::strike_ticket(&room.ruleset, sides, rand::random()))
        } else {
            None
        };
        let in_game: Vec<(usize, &Member)> =
            member_slots.iter().filter_map(|&i| room.slots[i].as_ref().map(|m| (i, m))).collect();
        let players: Vec<Player> = in_game
            .iter()
            .map(|&(i, m)| {
                let t = &tickets[&i];
                Player {
                    uid: m.info.uid.to_string(),
                    display_name: m.info.display_name.clone(),
                    connect_code: m.info.connect_code.clone(),
                    port: i as u8 + 1,
                    is_local_player: false,
                    ip_address: t.addr.to_string(),
                    ip_address_lan: t.lan.clone(),
                    chat_messages: common::DEFAULT_CHAT_MESSAGES.iter().map(|s| s.to_string()).collect(),
                    rank: None,
                    is_bot: false,
                }
            })
            .collect();
        for (i, t) in &tickets {
            let mut ps = players.clone();
            for p in &mut ps {
                p.is_local_player = p.port as usize == i + 1;
            }
            outcome.matched.push((
                t.conn,
                GetTicketResp {
                    kind: GET_TICKET_RESP.into(),
                    match_id: Some(match_id.clone()),
                    is_host: Some(*i == host),
                    players: Some(ps),
                    stages: Some(room.ruleset.stages.all().collect()),
                    items: Some(self.cfg.rules.items),
                    rules: Some(room.ruleset.rules.clone()),
                    set: set_ticket.clone(),
                    pick_from_stages: room.ruleset.pick_from_stages().then_some(true),
                    alternate_stages: room.ruleset.alternate_stages.then_some(true),
                    ..Default::default()
                },
            ));
            self.ticket_of.remove(&t.conn);
        }
        tracing::info!(
            %code,
            %match_id,
            players = ?players.iter().map(|p| format!("{}@{}", p.connect_code, p.ip_address)).collect::<Vec<_>>(),
            "room matched"
        );
        let mode = RoomMode::of(in_game.len(), room.teams);
        outcome.record = Some(RoomRecord {
            match_id: match_id.clone(),
            players: in_game.iter().map(|(_, m)| m.info.uid).collect(),
            host: room.slots[host].as_ref().map_or(Uuid::nil(), |m| m.info.uid),
            stages: room.ruleset.stages.all().collect(),
            room: common::db::MatchRoom {
                code: code.to_string(),
                mode: match mode {
                    RoomMode::OneVsOne => "1v1",
                    RoomMode::Ffa => "ffa",
                    RoomMode::Teams => "teams",
                }
                .into(),
                ports: in_game.iter().map(|(i, _)| *i as i16 + 1).collect(),
                teams: (mode == RoomMode::Teams).then(|| in_game.iter().map(|(_, m)| i16::from(m.team)).collect()),
                ruleset: room.ruleset_code.as_ref().map(|code| common::db::MatchRuleset {
                    code: code.clone(),
                    name: room.ruleset.name.clone(),
                    version: i32::try_from(room.ruleset.version).unwrap_or(i32::MAX),
                    hash: room.ruleset.hash(),
                }),
            },
        });
        room.phase = Phase::InGame {
            since: Instant::now(),
            match_id: match_id.clone(),
            unreachable: BTreeSet::new(),
            players: in_game.iter().map(|(i, m)| (*i, m.info.uid)).collect(),
            set: room.set.as_ref().is_some_and(|s| s.on()),
        };
        for (i, slot) in room.slots.iter_mut().enumerate() {
            if let Some(m) = slot.as_mut().filter(|_| member_slots.contains(&i)) {
                m.in_game = true;
                m.ready = false;
            }
        }
        Ok(outcome)
    }

    /// Sends every member the state again (after a ticket matched, the engine calls this).
    pub fn announce(&self, code: &str, out: &mut Vec<Output>) {
        self.broadcast(code, out);
    }

    /// An account's display name changed (`pp-admin user rename`): its room shows the new one.
    pub fn rename(&mut self, uid: Uuid, name: &str, out: &mut Vec<Output>) {
        let mut renamed = Vec::new();
        for room in self.rooms.values_mut() {
            for m in room.slots.iter_mut().flatten().filter(|m| m.info.uid == uid && m.info.display_name != name) {
                m.info.display_name = name.to_string();
                renamed.push(room.code.clone());
            }
        }
        for code in renamed {
            self.broadcast(&code, out);
        }
    }

    /// The account named its rooms (`PUT /v1/me/room-name`, seen in `users.room_name`): the room
    /// it hosts takes the name, for the launchers' lists (the games do not show it).
    pub fn set_name(&mut self, uid: Uuid, name: Option<&str>) {
        for room in self.rooms.values_mut() {
            let hosts = room.slots[room.host].as_ref().is_some_and(|h| h.info.uid == uid);
            if hosts && room.name.as_deref() != name {
                tracing::info!(code = %room.code, ?name, "room named");
                room.name = name.map(str::to_string);
            }
        }
    }

    /// The account on this online connection was banned: it leaves its room as a removed player
    /// does (its side forfeits a set that is on) rather than as a dropped connection (waited for).
    pub fn banned(&mut self, now: Instant, conn: ConnId, out: &mut Vec<Output>) {
        if let Some(code) = self.member_of.get(&conn).cloned() {
            self.leave(now, &code, conn, Some(msg::BANNED), false, out);
        }
    }

    /// An online connection or a ticket connection went away.
    pub fn conn_gone(&mut self, now: Instant, conn: ConnId, out: &mut Vec<Output>) {
        if let Some(code) = self.member_of.get(&conn).cloned() {
            self.leave(now, &code, conn, None, false, out);
        }
        if let Some(code) = self.ticket_of.remove(&conn) {
            if let Some(Room { phase: Phase::Starting { tickets, .. }, .. }) = self.rooms.get_mut(&code) {
                tickets.retain(|_, t| t.conn != conn);
            }
        }
    }

    /// Starts that waited too long fail; games that never reported their end are over.
    pub fn tick(&mut self, now: Instant, out: &mut Vec<Output>) {
        let mut late = Vec::new();
        let mut stale = Vec::new();
        for room in self.rooms.values() {
            match &room.phase {
                Phase::Starting { since, tickets, players, .. }
                    if now.saturating_duration_since(*since) >= self.cfg.start_timeout =>
                {
                    let missing = players
                        .iter()
                        .find(|i| !tickets.contains_key(i))
                        .and_then(|&i| room.slots[i].as_ref())
                        .map(|m| m.info.connect_code.clone())
                        .unwrap_or_default();
                    late.push((room.code.clone(), msg::did_not_connect(&missing)));
                }
                Phase::InGame { since, .. } if now.saturating_duration_since(*since) >= self.cfg.game_max => {
                    stale.push(room.code.clone())
                }
                _ => {}
            }
        }
        for (code, why) in late {
            self.cancel_start(&code, &why, out);
            self.broadcast(&code, out);
        }
        // A watcher runs a few seconds behind the players (up to the upload's 10 s hold at a
        // game's start, and its backoff): one still watching well after the room's game is over,
        // with no other game going on, lost its way back and is waited for again; so is one
        // watching for longer than any game.
        let mut unwatched = Vec::new();
        for room in self.rooms.values_mut() {
            let over = matches!(room.phase, Phase::Waiting)
                && room.game_over_at.is_some_and(|t| now.saturating_duration_since(t) >= WATCH_AFTER_GAME);
            let mut changed = false;
            for m in room.slots.iter_mut().flatten() {
                if m.watching.is_some_and(|t| over || now.saturating_duration_since(t) >= self.cfg.game_max) {
                    tracing::info!(code = %room.code, player = %m.info.connect_code, "room watch timed out");
                    m.watching = None;
                    changed = true;
                }
            }
            if changed {
                unwatched.push(room.code.clone());
            }
        }
        for code in unwatched {
            self.broadcast(&code, out);
        }
        for code in stale {
            if let Some(room) = self.rooms.get_mut(&code) {
                tracing::info!(%code, "room game ran too long; back to waiting");
                for m in room.slots.iter_mut().flatten() {
                    m.in_game = false;
                }
                room.phase = Phase::Waiting;
            }
            self.broadcast(&code, out);
            self.maybe_start(now, &code, out);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use common::rules::MatchRules;
    use serde_json::Value;
    use std::collections::HashSet;

    fn info(n: u8) -> MemberInfo {
        MemberInfo {
            uid: Uuid::from_bytes([n; 16]),
            display_name: format!("player{n}"),
            connect_code: format!("P#{n}"),
            room_name: None,
        }
    }

    struct T {
        book: RoomBook,
        now: Instant,
    }

    impl T {
        fn new() -> T {
            T { book: RoomBook::new(RoomsConfig::default()), now: Instant::now() }
        }
        fn req(&mut self, conn: ConnId, req: RoomRequest) -> Vec<Output> {
            let mut out = vec![];
            self.book.request(self.now, conn, &info(conn as u8), req, &mut out);
            out
        }
    }

    fn msgs(out: &[Output], conn: ConnId) -> Vec<Value> {
        out.iter()
            .filter_map(|o| match o {
                Output::Send { conn: c, json } if *c == conn => Some(serde_json::from_str(json).unwrap()),
                _ => None,
            })
            .collect()
    }

    fn last_state(out: &[Output], conn: ConnId) -> Value {
        msgs(out, conn).into_iter().rev().find(|m| m["type"] == "room-state").expect("room-state")
    }

    fn create(t: &mut T, conn: ConnId) -> String {
        let out = t.req(conn, RoomRequest::Create { public: true, ruleset: None, remembered: false });
        last_state(&out, conn)["code"].as_str().unwrap().to_string()
    }

    fn join(t: &mut T, conn: ConnId, code: &str) -> Vec<Output> {
        t.req(conn, RoomRequest::Join { code: code.into() })
    }

    /// What the engine keeps chat groups in step with: the rooms whose players changed, and their
    /// players in joining order; a code used again is another room (another serial).
    #[test]
    fn player_changes_are_recorded() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        assert_eq!(t.book.take_changed(), vec![(code.clone(), 1)]);
        assert!(t.book.take_changed().is_empty());
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        t.req(1, RoomRequest::Ready { ready: true, character: None, costume: None, stocks: None, hp: None });
        assert!(t.book.take_changed().is_empty(), "no player changed");
        join(&mut t, 3, &code);
        join(&mut t, 2, &code);
        assert_eq!(t.book.take_changed(), vec![(code.clone(), 1)]);
        assert_eq!(t.book.players(&code, 1), vec![(1, info(1).uid), (3, info(3).uid), (2, info(2).uid)]);
        assert!(t.book.players(&code, 2).is_empty());
        // A kick (3 is in slot 2), a dropped connection.
        t.req(1, RoomRequest::Slot { slot: 2, open: false });
        assert_eq!(t.book.take_changed(), vec![(code.clone(), 1)]);
        assert_eq!(t.book.players(&code, 1), vec![(1, info(1).uid), (2, info(2).uid)]);
        t.book.conn_gone(t.now, 2, &mut vec![]);
        assert_eq!(t.book.take_changed(), vec![(code.clone(), 1)]);
        // The last one leaves for a new room: the old one is gone, the new one has serial 2.
        let other = create(&mut t, 1);
        assert_eq!(t.book.take_changed(), vec![(code.clone(), 1), (other.clone(), 2)]);
        assert!(t.book.players(&code, 1).is_empty());
        assert_eq!(t.book.players(&other, 2), vec![(1, info(1).uid)]);
    }

    #[test]
    fn create_gives_a_fresh_room() {
        let mut t = T::new();
        let out = t.req(1, RoomRequest::Create { public: true, ruleset: None, remembered: false });
        let s = last_state(&out, 1);
        let code = s["code"].as_str().unwrap();
        assert!(rooms::parse_room_code(code).is_some());
        assert_eq!(s["you"], 1);
        assert_eq!(s["host"], 1);
        assert_eq!(s["mode"], "1v1");
        assert_eq!(s["status"], "waiting");
        assert_eq!(s["public"], true);
        let open: Vec<bool> = s["slots"].as_array().unwrap().iter().map(|x| x["open"].as_bool().unwrap()).collect();
        assert_eq!(open, [true, true, false, false]);
        assert_eq!(s["statusText"], msg::ROOM_WAITING);
        assert_eq!(t.book.room_count(), 1);
    }

    #[test]
    fn codes_are_unique_among_live_rooms() {
        let mut t = T::new();
        let mut codes = HashSet::new();
        for conn in 1..=200 {
            assert!(codes.insert(create(&mut t, conn)));
        }
        assert_eq!(t.book.room_count(), 200);
    }

    #[test]
    fn join_full_not_found_and_case() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        let out = join(&mut t, 2, &code.to_lowercase());
        let s2 = last_state(&out, 2);
        assert_eq!(s2["you"], 2);
        assert_eq!(s2["slots"][1]["player"]["displayName"], "player2");
        assert_eq!(s2["slots"][1]["player"]["connectCode"], "P#2");
        // The host sees the newcomer too.
        assert_eq!(last_state(&out, 1)["slots"][1]["player"]["displayName"], "player2");
        let out = join(&mut t, 3, &code);
        assert_eq!(msgs(&out, 3)[0]["error"], msg::ROOM_FULL);
        for bad in ["ZZZZ", "AEIO", "", "KFQBX"] {
            let code_out = join(&mut t, 4, bad);
            let m = &msgs(&code_out, 4)[0];
            assert_eq!(m["type"], "room-error");
            assert_eq!(m["op"], "room-join");
            assert_eq!(m["error"], msg::ROOM_NOT_FOUND, "{bad}");
        }
    }

    #[test]
    fn slots_kick_and_rules() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        // Not the host.
        assert_eq!(msgs(&t.req(2, RoomRequest::Slot { slot: 3, open: true }), 2)[0]["error"], msg::HOST_ONLY);
        // Own slot, and at least two open: an empty slot of two stays open.
        assert_eq!(msgs(&t.req(1, RoomRequest::Slot { slot: 1, open: false }), 1)[0]["error"], msg::OWN_SLOT);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        let out = t.req(1, RoomRequest::Slot { slot: 3, open: false });
        assert_eq!(last_state(&out, 1)["slots"][2]["open"], false);
        // A 1v1 room: closing the occupied slot removes its player, and the slot stays open.
        let out = t.req(1, RoomRequest::Slot { slot: 2, open: false });
        assert_eq!(msgs(&out, 2).last().unwrap()["reason"], msg::REMOVED);
        let s = last_state(&out, 1);
        assert!(s["slots"][1]["player"].is_null());
        assert_eq!(s["slots"][1]["open"], true);
        assert_eq!(msgs(&t.req(1, RoomRequest::Slot { slot: 2, open: false }), 1)[0]["error"], msg::MIN_OPEN_SLOTS);
        join(&mut t, 2, &code);
        // Open 3 and 4: two play a 1v1, the empty slots wait for nobody (the user's, 2026-10-18).
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        let out = t.req(1, RoomRequest::Slot { slot: 4, open: true });
        let s = last_state(&out, 2);
        assert_eq!(s["mode"], "1v1");
        assert_eq!(s["statusText"], msg::waiting_on(&["player1", "player2"]));
        join(&mut t, 3, &code);
        // Closing an occupied slot removes its player.
        let out = t.req(1, RoomRequest::Slot { slot: 2, open: false });
        let left = msgs(&out, 2);
        assert_eq!(left.last().unwrap()["type"], "room-left");
        assert_eq!(left.last().unwrap()["reason"], msg::REMOVED);
        let s = last_state(&out, 1);
        assert!(s["slots"][1]["player"].is_null());
        assert_eq!(s["slots"][1]["open"], false);
        assert_eq!(t.book.room_of(2), None);
        // Not banned: the slot opened again, the removed player joins again (a misclick undone).
        t.req(1, RoomRequest::Slot { slot: 2, open: true });
        assert_eq!(last_state(&join(&mut t, 2, &code), 2)["you"], 2);
        // Someone else takes the last open slot.
        assert_eq!(last_state(&join(&mut t, 5, &code), 5)["you"], 4);
    }

    #[test]
    fn host_leaves_and_room_closes_when_empty() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        join(&mut t, 3, &code);
        let out = t.req(1, RoomRequest::Leave);
        assert_eq!(msgs(&out, 1)[0]["type"], "room-left");
        let s = last_state(&out, 3);
        assert_eq!(s["host"], 2, "the earliest-joined player is the new host");
        assert_eq!(s["slots"][1]["host"], true);
        // The new host can change slots; the code stays.
        let out = t.req(2, RoomRequest::Slot { slot: 1, open: false });
        assert_eq!(last_state(&out, 3)["code"], code.as_str());
        // A disconnect leaves silently.
        let mut out = vec![];
        t.book.conn_gone(t.now, 2, &mut out);
        assert!(msgs(&out, 2).is_empty());
        assert_eq!(last_state(&out, 3)["host"], 3);
        t.req(3, RoomRequest::Leave);
        assert_eq!(t.book.room_count(), 0);
        assert_eq!(msgs(&join(&mut t, 4, &code), 4)[0]["error"], msg::ROOM_NOT_FOUND);
    }

    #[test]
    fn public_list() {
        let mut t = T::new();
        let a = create(&mut t, 1);
        let b = create(&mut t, 2);
        t.req(2, RoomRequest::Public { public: false });
        assert_eq!(msgs(&t.req(3, RoomRequest::Public { public: false }), 3)[0]["error"], msg::NOT_IN_ROOM);
        join(&mut t, 3, &a);
        let list = t.book.public_rooms(100);
        assert_eq!(list.len(), 1);
        let r = &list[0];
        assert_eq!(r.code, a);
        assert_eq!((r.players, r.open_slots, r.joinable), (2, 2, false));
        assert_eq!(r.host, "player1");
        assert_eq!(r.names, vec!["player1", "player3"]);
        assert_eq!(r.status, RoomStatus::Waiting);
        // Private rooms are joinable by code.
        assert_eq!(last_state(&join(&mut t, 4, &b), 4)["public"], false);
        // Joinable rooms come first.
        t.req(2, RoomRequest::Public { public: true });
        t.req(2, RoomRequest::Slot { slot: 3, open: true });
        let list = t.book.public_rooms(100);
        assert_eq!(list.iter().map(|r| r.code.clone()).collect::<Vec<_>>(), vec![b, a]);
    }

    #[test]
    fn host_sets_the_rules_everyone_sees_them_and_the_game_plays_them() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        let out = join(&mut t, 2, &code);
        // A new room plays Direct's rules (RoomsConfig::default: P+'s set).
        assert_eq!(last_state(&out, 2)["rules"], serde_json::to_value(MatchRules::default()).unwrap());
        t.req(2, RoomRequest::Ready { ready: true, character: Some(3), costume: Some(0), stocks: None, hp: None });
        let rules = MatchRules { stocks: 3, auto_l_cancel: true, item_frequency: 1, ..Default::default() };
        // Only the host.
        let out = t.req(2, RoomRequest::Rules { rules: rules.clone() });
        assert_eq!(msgs(&out, 2)[0]["error"], msg::HOST_ONLY);
        // The host's change reaches everyone and takes back every ready (room-rules.md #5).
        let out = t.req(1, RoomRequest::Rules { rules: rules.clone() });
        for c in [1, 2] {
            let s = last_state(&out, c);
            assert_eq!(s["rules"], serde_json::to_value(&rules).unwrap());
            assert_eq!(s["slots"][1]["player"]["ready"], false);
        }
        // The same rules again change nothing (no ready is taken back).
        t.req(2, RoomRequest::Ready { ready: true, character: Some(3), costume: Some(0), stocks: None, hp: None });
        let out = t.req(1, RoomRequest::Rules { rules: rules.clone() });
        assert_eq!(last_state(&out, 2)["slots"][1]["player"]["ready"], true);
        // The game is played with them; no change while it starts.
        let out = ready_all(&mut t, &[1]);
        assert!(msgs(&out, 1).iter().any(|m| m["type"] == "room-start"));
        let out = t.req(1, RoomRequest::Rules { rules: MatchRules::default() });
        assert_eq!(msgs(&out, 1)[0]["error"], msg::BETWEEN_GAMES);
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        t.book.ticket(101, addr(1), String::new(), info(1).uid, &code).unwrap();
        let done = t.book.ticket(102, addr(2), String::new(), info(2).uid, &code).unwrap();
        assert_eq!(done.matched.len(), 2);
        for (_, resp) in &done.matched {
            assert_eq!(resp.rules.as_ref(), Some(&rules));
        }
    }

    #[test]
    fn picked_stocks_show_with_the_ready_only_under_stock_control() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        let ready = |n| RoomRequest::Ready { ready: true, character: Some(3), costume: Some(0), stocks: n, hp: None };
        // Without stock control a pick shows nothing (the key is left out).
        let out = t.req(2, ready(Some(2)));
        assert!(last_state(&out, 1)["slots"][1]["player"].get("stocks").is_none());
        // With it, the pick shows once the player is ready, as the character does.
        let rules = MatchRules { stock_control: true, ..Default::default() };
        let out = t.req(1, RoomRequest::Rules { rules: rules.clone() });
        assert_eq!(last_state(&out, 2)["rules"]["stockControl"], true);
        assert_eq!(last_state(&out, 1)["slots"][1]["player"]["ready"], false);
        let out = t.req(2, ready(Some(2)));
        assert_eq!(last_state(&out, 1)["slots"][1]["player"]["stocks"], 2);
        let out = t.req(2, RoomRequest::Ready { ready: false, character: None, costume: None, stocks: None, hp: None });
        assert!(last_state(&out, 1)["slots"][1]["player"].get("stocks").is_none());
        // None picked: the rules' stocks, no key.
        let out = t.req(2, ready(None));
        assert!(last_state(&out, 1)["slots"][1]["player"].get("stocks").is_none());
        // The rules carry it into the game.
        t.req(2, ready(Some(5)));
        ready_all(&mut t, &[1]);
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        t.book.ticket(101, addr(1), String::new(), info(1).uid, &code).unwrap();
        let done = t.book.ticket(102, addr(2), String::new(), info(2).uid, &code).unwrap();
        for (_, resp) in &done.matched {
            assert!(resp.rules.as_ref().unwrap().stock_control);
        }
    }

    #[test]
    fn a_bo3_set_is_kept_across_games_pauses_and_resumes() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        let mut r = Ruleset {
            name: "Weekly".into(),
            version: 3,
            best_of: 3,
            strikes: vec![1, 2, 1],
            bans: 2,
            dsr: true,
            ..Default::default()
        };
        r.normalize();
        let mut out = vec![];
        t.book.apply_ruleset(&code, r.clone(), Some("BCDFGH".into()), &mut out).unwrap();
        let s = last_state(&out, 2);
        assert_eq!((s["ruleset"]["code"].as_str(), s["ruleset"]["bestOf"].as_u64()), (Some("BCDFGH"), Some(3)));
        assert!(s.get("set").is_none());
        // Game 1 is a blind pick: the other's character stays hidden even when ready.
        let out = t.req(1, RoomRequest::Ready { ready: true, character: Some(7), costume: Some(0), stocks: None, hp: None });
        assert_eq!(last_state(&out, 2)["slots"][0]["player"]["character"], Value::Null);
        assert_eq!(last_state(&out, 1)["slots"][0]["player"]["character"], 7);
        // The start makes the set; the tickets carry it.
        ready_all(&mut t, &[2]);
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        t.book.ticket(101, addr(1), String::new(), info(1).uid, &code).unwrap();
        let done = t.book.ticket(102, addr(2), String::new(), info(2).uid, &code).unwrap();
        let set = done.matched[0].1.set.clone().expect("the set ticket");
        assert_eq!((set.best_of, set.game, set.sides, set.wins), (3, 1, [0, 1], [0, 0]));
        assert_eq!(set.counterpicks.len(), 9);
        // Rules, slots and Teams wait for the set.
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        // Results: game 1 counts once both report it the same.
        let out = t.req(1, RoomRequest::Result { game: 1, winner_side: 0, stage: 0x21 });
        assert_eq!(last_state(&out, 1)["set"]["score"], serde_json::json!([0, 0]));
        let out = t.req(2, RoomRequest::Result { game: 1, winner_side: 0, stage: 0x21 });
        let s = last_state(&out, 2);
        assert_eq!((s["set"]["score"].clone(), s["set"]["game"].as_u64()), (serde_json::json!([1, 0]), Some(2)));
        // Player 2's game reports game 2's first strikes, then they leave: the set waits for them,
        // the room turns private and keeps their slot; nobody else can take it.
        assert!(t
            .req(2, RoomRequest::Step { game: 2, actions: [vec![0x02, 0x03], vec![]], match_started: false })
            .is_empty());
        t.req(2, RoomRequest::Leave);
        let out = t.req(1, RoomRequest::Back { pickers: None });
        let s = last_state(&out, 1);
        assert_eq!((s["set"]["paused"].as_bool(), s["set"]["waitingFor"].as_str()), (Some(true), Some("player2")));
        assert_eq!((s["statusText"].as_str(), s["public"].as_bool()), (Some("Set paused: player2 left"), Some(false)));
        assert_eq!(s["set"]["canResume"], false);
        assert_eq!(msgs(&join(&mut t, 3, &code), 3)[0]["error"], msg::ROOM_FULL);
        assert_eq!(msgs(&t.req(1, RoomRequest::Teams { on: true, colors: false }), 1)[0]["error"], msg::SET_ON);
        assert_eq!(msgs(&t.req(1, RoomRequest::Set { resume: true }), 1)[0]["error"], msg::SET_NOT_RESUMABLE);
        // They come back into their slot: the set goes on by itself, the room is public again,
        // and the next start goes on at game 2 from the strikes made so far.
        let out = join(&mut t, 2, &code);
        let s = last_state(&out, 2);
        assert_eq!(
            (s["you"].as_u64(), s["set"]["paused"].as_bool(), s["public"].as_bool()),
            (Some(2), Some(false), Some(true))
        );
        let set = play(&mut t, &code, 103);
        assert_eq!((set.game, set.wins, set.last_winner, set.last_win_stage[0]), (2, [1, 0], 0, 0x21));
        assert_eq!(set.actions, vec![vec![0x02, 0x03], vec![]]);
        // Game 2's match starts and player 2 leaves in it: that game is theirs lost, and the set
        // (2-0) is over. Back in the room, the result shows until someone readies.
        t.req(1, RoomRequest::Step { game: 2, actions: [vec![0x02, 0x03], vec![0x21]], match_started: true });
        t.req(2, RoomRequest::Leave);
        let out = t.req(1, RoomRequest::Back { pickers: None });
        let s = last_state(&out, 1);
        assert_eq!((s["set"]["finished"].as_bool(), s["set"]["winner"].as_u64()), (Some(true), Some(0)));
        assert_eq!((s["set"]["score"].clone(), s["public"].as_bool()), (serde_json::json!([2, 0]), Some(true)));
        let out = join(&mut t, 2, &code);
        assert_eq!(last_state(&out, 2)["statusText"], "player1 wins the set 2-0");
        // The next all-ready starts a new set at game 1.
        let set = play(&mut t, &code, 105);
        assert_eq!((set.game, set.wins, set.last_winner), (1, [0, 0], room_set::SIDE_NONE));
        // Different results pause it until the host replays the game, or ends the set.
        t.req(1, RoomRequest::Result { game: 1, winner_side: 0, stage: 0x02 });
        let out = t.req(2, RoomRequest::Result { game: 1, winner_side: 1, stage: 0x02 });
        assert_eq!(last_state(&out, 1)["set"]["paused"], true);
        t.req(1, RoomRequest::Back { pickers: None });
        let out = t.req(2, RoomRequest::Back { pickers: None });
        assert_eq!(last_state(&out, 1)["statusText"], msg::SET_HOST_DECIDES);
        assert_eq!(last_state(&out, 1)["set"]["canResume"], true);
        assert_eq!(msgs(&t.req(2, RoomRequest::Set { resume: true }), 2)[0]["error"], msg::HOST_ONLY);
        let out = t.req(1, RoomRequest::Set { resume: true });
        let s = last_state(&out, 1);
        assert_eq!((s["set"]["paused"].as_bool(), s["set"]["game"].as_u64()), (Some(false), Some(1)));
        // A new ruleset waits for a set that is on; so do the rules.
        let mut out = vec![];
        assert_eq!(t.book.apply_ruleset(&code, Ruleset::default(), None, &mut out), Err(msg::SET_ON));
        assert_eq!(msgs(&t.req(1, RoomRequest::Rules { rules: Default::default() }), 1)[0]["error"], msg::SET_ON);
        // A player who left and whose held slot the host closes forfeits the set (the slot of a
        // 1v1 stays open).
        t.req(2, RoomRequest::Leave);
        let out = t.req(1, RoomRequest::Slot { slot: 2, open: false });
        let s = last_state(&out, 1);
        assert_eq!((s["set"]["finished"].as_bool(), s["set"]["winner"].as_u64()), (Some(true), Some(0)));
        assert_eq!((s["slots"][1]["open"].as_bool(), s["public"].as_bool()), (Some(true), Some(true)));
        join(&mut t, 2, &code);
        // The host ends a set; a ruleset change between sets clears the score.
        let out = t.req(1, RoomRequest::Set { resume: false });
        assert!(last_state(&out, 1).get("set").is_none());
        // The host's rule change makes the ruleset custom, based on the published one.
        let mut out = vec![];
        t.book.apply_ruleset(&code, r.clone(), Some("BCDFGH".into()), &mut out).unwrap();
        let rules = common::rules::MatchRules { stocks: 2, ..Default::default() };
        let out = t.req(1, RoomRequest::Rules { rules });
        let s = last_state(&out, 1)["ruleset"].clone();
        assert_eq!(
            (s["code"].clone(), s["name"].as_str(), s["basedOn"].as_str()),
            (Value::Null, Some("Custom"), Some("Weekly v3"))
        );
    }

    #[test]
    fn the_host_sets_the_set_length() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        assert_eq!(msgs(&t.req(2, RoomRequest::BestOf { best_of: 3 }), 2)[0]["error"], msg::HOST_ONLY);
        // P+'s default with sets of 3: custom, Ranked's strikes, bans and DSR; everyone unready.
        t.req(2, RoomRequest::Ready { ready: true, character: Some(1), costume: Some(0), stocks: None, hp: None });
        let out = t.req(1, RoomRequest::BestOf { best_of: 3 });
        let s = last_state(&out, 2);
        assert_eq!((s["ruleset"]["bestOf"].as_u64(), s["ruleset"]["name"].as_str()), (Some(3), Some("Custom")));
        assert_eq!(s["ruleset"]["stageChoice"], "loser-picks");
        assert_eq!(s["slots"][1]["player"]["ready"], false);
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        ready_all(&mut t, &[1, 2]);
        t.book.ticket(101, addr(1), String::new(), info(1).uid, &code).unwrap();
        let done = t.book.ticket(102, addr(2), String::new(), info(2).uid, &code).unwrap();
        let set = done.matched[0].1.set.clone().expect("a set");
        assert_eq!((set.best_of, set.strikes.clone(), set.bans, set.dsr), (3, vec![1, 2, 1], 2, true));
        // While it is on, the length waits.
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        t.req(1, RoomRequest::Back { pickers: None });
        t.req(2, RoomRequest::Back { pickers: None });
        assert_eq!(msgs(&t.req(1, RoomRequest::BestOf { best_of: 5 }), 1)[0]["error"], msg::SET_ON);
        // The host ends the set that is on (End set): no result, the room is free again. (In its
        // session, between games, too: see the_host_ends_a_running_set_between_games.)
        assert_eq!(msgs(&t.req(2, RoomRequest::Set { resume: false }), 2)[0]["error"], msg::HOST_ONLY);
        let out = t.req(1, RoomRequest::Set { resume: false });
        assert!(last_state(&out, 1).get("set").is_none());
        // Back to no set: no bans, no strikes; a published ruleset becomes custom based on it.
        let out = t.req(1, RoomRequest::BestOf { best_of: 0 });
        assert_eq!(last_state(&out, 1)["ruleset"]["bestOf"], 0);
        let mut r =
            Ruleset { name: "Weekly".into(), version: 2, best_of: 3, strikes: vec![1, 2, 1], ..Default::default() };
        r.normalize();
        let mut out = vec![];
        t.book.apply_ruleset(&code, r, Some("BCDFGH".into()), &mut out).unwrap();
        let out = t.req(1, RoomRequest::BestOf { best_of: 5 });
        let s = last_state(&out, 1)["ruleset"].clone();
        assert_eq!(
            (s["bestOf"].as_u64(), s["basedOn"].as_str(), s["code"].clone()),
            (Some(5), Some("Weekly v2"), Value::Null)
        );
        // Any room takes a set length (a room of three plays single games until it makes two sides).
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        let out = t.req(1, RoomRequest::BestOf { best_of: 3 });
        assert_eq!(last_state(&out, 1)["ruleset"]["bestOf"], 3);
    }

    #[test]
    fn the_host_ends_a_running_set_between_games() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        t.req(1, RoomRequest::BestOf { best_of: 3 });
        let set = play(&mut t, &code, 101);
        assert_eq!(set.game, 1);
        // Game 1's match is on: End set waits for it.
        t.req(1, RoomRequest::Step { game: 1, actions: [vec![0x01, 0x2E], vec![0x02, 0x21]], match_started: true });
        assert_eq!(msgs(&t.req(1, RoomRequest::Set { resume: false }), 1)[0]["error"], msg::BETWEEN_GAMES);
        // Game 1 counted; the players are back on the CSS in the set's session: End set ends it.
        t.req(1, RoomRequest::Result { game: 1, winner_side: 0, stage: 0x21 });
        t.req(2, RoomRequest::Result { game: 1, winner_side: 0, stage: 0x21 });
        assert_eq!(msgs(&t.req(2, RoomRequest::Set { resume: false }), 2)[0]["error"], msg::HOST_ONLY);
        let out = t.req(1, RoomRequest::Set { resume: false });
        let s = last_state(&out, 2);
        assert!(s.get("set").is_none(), "{s}");
        // Replay (resume) still only between games.
        assert_eq!(msgs(&t.req(1, RoomRequest::Set { resume: true }), 1)[0]["error"], msg::BETWEEN_GAMES);
        // Their games end the session; the room waits again, and the next all-ready starts a new set.
        t.req(1, RoomRequest::Back { pickers: None });
        let out = t.req(2, RoomRequest::Back { pickers: None });
        assert_eq!(last_state(&out, 1)["status"], "waiting");
        let set = play(&mut t, &code, 103);
        assert_eq!((set.game, set.wins), (1, [0, 0]));
    }

    #[test]
    fn the_host_switches_to_a_published_ruleset_by_its_code() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        let fetch = |out: &[Output]| {
            out.iter()
                .find_map(|o| match o {
                    Output::FetchRuleset(f) => Some(f.clone()),
                    _ => None,
                })
                .expect("a ruleset lookup")
        };
        // Not the host: refused before any lookup.
        let out = t.req(2, RoomRequest::Ruleset { code: Some("BCDFGH".into()) });
        assert_eq!(msgs(&out, 2)[0]["error"], msg::HOST_ONLY);
        let f = fetch(&t.req(1, RoomRequest::Ruleset { code: Some("BCDFGH".into()) }));
        assert_eq!((f.code.as_deref(), f.remember, f.op), (Some("BCDFGH"), true, rooms::ROOM_RULESET));
        let r = Ruleset { name: "Weekly".into(), version: 2, best_of: 3, strikes: vec![1, 2, 1], ..Default::default() };
        let mut out = vec![];
        t.book.ruleset_fetched(f.clone(), Ok(Some(("BCDFGH".into(), r))), &mut out);
        let s = last_state(&out, 2);
        assert_eq!((s["ruleset"]["code"].as_str(), s["ruleset"]["name"].as_str()), (Some("BCDFGH"), Some("Weekly")));
        // Unknown and failed lookups answer the host with an error; the room keeps its ruleset.
        let mut out = vec![];
        t.book.ruleset_fetched(f.clone(), Ok(None), &mut out);
        assert_eq!(msgs(&out, 1)[0]["error"], msg::RULESET_NOT_FOUND);
        let mut out = vec![];
        t.book.ruleset_fetched(f, Err("db down".into()), &mut out);
        assert_eq!(msgs(&out, 1)[0]["error"], msg::RULESETS_UNAVAILABLE);
        // P+'s default at once (and remembered as the host's choice).
        let out = t.req(1, RoomRequest::Ruleset { code: None });
        assert_eq!(last_state(&out, 1)["ruleset"]["name"], "P+ default");
        assert_eq!(last_state(&out, 1)["ruleset"]["alternateStages"], true);
        assert!(fetch(&out).remember);
        // A create with the launcher's code looks it up for the new room; the game's own create
        // asks for the remembered one.
        let out = t.req(5, RoomRequest::Create { public: true, ruleset: Some("BCDFGH".into()), remembered: false });
        let f = fetch(&out);
        assert_eq!((f.code.as_deref(), f.remember, f.op), (Some("BCDFGH"), false, rooms::ROOM_CREATE));
        let out = t.req(6, RoomRequest::Create { public: true, ruleset: None, remembered: true });
        assert_eq!(fetch(&out).code, None);
        assert!(!t
            .req(7, RoomRequest::Create { public: true, ruleset: None, remembered: false })
            .iter()
            .any(|o| matches!(o, Output::FetchRuleset(_))));
    }

    /// Decision G: a ruleset works in any room. One with a set, in a room that does not make two
    /// sides (three players), says so while nobody is ready and plays single games: no set, no
    /// strikes in the ticket; with a third player gone, the next start is a set.
    #[test]
    fn a_set_ruleset_in_a_room_of_three_plays_single_games() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        let out = join(&mut t, 3, &code);
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player1", "player2", "player3"]));
        let mut r =
            Ruleset { name: "Weekly".into(), version: 1, best_of: 3, strikes: vec![1, 2, 1], ..Default::default() };
        r.normalize();
        let mut out = vec![];
        t.book.apply_ruleset(&code, r.clone(), Some("BCDFGH".into()), &mut out).unwrap();
        assert_eq!(last_state(&out, 1)["statusText"], msg::SETS_SIT_OUT);
        // Once someone readies, the line is the usual one; the start is not held.
        let out = t.req(1, RoomRequest::Ready { ready: true, character: Some(1), costume: Some(0), stocks: None, hp: None });
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player2", "player3"]));
        let out = ready_all(&mut t, &[2, 3]);
        assert!(msgs(&out, 1).iter().any(|m| m["type"] == "room-start"));
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        let mut done = TicketOutcome::default();
        for n in 1..=3u8 {
            done = t.book.ticket(100 + n as ConnId, addr(n), String::new(), info(n).uid, &code).unwrap();
        }
        assert!(done.matched[0].1.set.is_none());
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        assert!(last_state(&out, 1).get("set").is_none());
        // Striking every game: the same, with its own note.
        for n in 1..=3 {
            t.req(n, RoomRequest::Back { pickers: None });
        }
        let strike = Ruleset { best_of: 0, stage_choice: common::rules::StageChoice::Strike, ..r };
        let mut out = vec![];
        t.book.apply_ruleset(&code, strike, Some("CDFGHJ".into()), &mut out).unwrap();
        assert_eq!(last_state(&out, 1)["statusText"], msg::STRIKES_SIT_OUT);
    }

    /// The room's stages go to its members with the ruleset, and a ruleset whose loser picks from
    /// the list says so in the ticket; P+ default (any stage) does not. P+ default plays alternate
    /// stages (2026-10-27); a published ruleset only with its switch on.
    #[test]
    fn a_loser_picks_from_the_list_room_holds_its_tickets_to_the_stages() {
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        let start = |t: &mut T, code: &str, conn: ConnId| {
            ready_all(t, &[1, 2]);
            t.book.ticket(conn, addr(1), String::new(), info(1).uid, code).unwrap();
            t.book.ticket(conn + 1, addr(2), String::new(), info(2).uid, code).unwrap().matched[0].1.clone()
        };
        let mut t = T::new();
        let code = create(&mut t, 1);
        let out = join(&mut t, 2, &code);
        let s = last_state(&out, 1);
        assert_eq!(s["ruleset"]["stageChoice"], "loser-picks");
        assert_eq!(s["ruleset"]["stages"]["starters"], serde_json::json!([1, 4, 9, 0x21, 0x2E]));
        assert_eq!(s["ruleset"]["alternateStages"], true);
        let resp = start(&mut t, &code, 100);
        assert_eq!((resp.pick_from_stages, resp.alternate_stages), (None, Some(true)));
        for n in 1..=2 {
            t.req(n, RoomRequest::Back { pickers: None });
        }

        let mut r = Ruleset {
            name: "Casual".into(),
            version: 1,
            stage_choice: common::rules::StageChoice::LoserPicksList,
            stages: common::rules::StageLists { starters: vec![1, 2], counterpicks: vec![0x2A] },
            ..Default::default()
        };
        r.normalize();
        r.validate().unwrap();
        let mut out = vec![];
        t.book.apply_ruleset(&code, r, Some("BCDFGH".into()), &mut out).unwrap();
        let s = last_state(&out, 2);
        assert_eq!(s["ruleset"]["stageChoice"], "loser-picks-list");
        assert_eq!(s["ruleset"]["stages"], serde_json::json!({"starters": [1, 2], "counterpicks": [0x2A]}));
        let resp = start(&mut t, &code, 200);
        assert!(serde_json::to_string(&resp).unwrap().contains(r#""pickFromStages":true"#));
        assert_eq!((resp.pick_from_stages, resp.stages), (Some(true), Some(vec![1, 2, 0x2A])));
        assert_eq!(resp.alternate_stages, None);

        // A ruleset with alternate stages on: the picks play the held L/R/Z layouts.
        for n in 1..=2 {
            t.req(n, RoomRequest::Back { pickers: None });
        }
        let r = Ruleset { name: "Alts".into(), version: 1, alternate_stages: true, ..Default::default() };
        r.validate().unwrap();
        let mut out = vec![];
        t.book.apply_ruleset(&code, r, Some("BCDFGJ".into()), &mut out).unwrap();
        assert_eq!(last_state(&out, 2)["ruleset"]["alternateStages"], true);
        let resp = start(&mut t, &code, 300);
        assert!(serde_json::to_string(&resp).unwrap().contains(r#""alternateStages":true"#));
        assert_eq!(resp.alternate_stages, Some(true));
    }

    /// The host's STAGE SWITCH (`room-stages`): the room's stages and the loser's choice between any
    /// stage and the list; the rules become custom and every ready is taken back. Strike and random
    /// stay as they are, and a strike pattern that no longer fits becomes the default one.
    #[test]
    fn the_host_edits_the_rooms_stages() {
        use common::rules::StageChoice;
        let stages = |starters: Vec<u16>, counterpicks: Vec<u16>| StageLists { starters, counterpicks };
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        t.req(2, RoomRequest::Ready { ready: true, character: Some(2), costume: Some(0), stocks: None, hp: None });
        let req = |s: StageLists, c: Option<StageChoice>| RoomRequest::Stages { stages: s, stage_choice: c };
        let out = t.req(2, req(stages(vec![1], vec![]), None));
        assert_eq!(msgs(&out, 2)[0]["error"], msg::HOST_ONLY);
        let out = t.req(1, req(stages(vec![], vec![1]), None));
        assert_eq!(msgs(&out, 1)[0]["error"], msg::NO_STARTER_STAGE);

        let out = t.req(1, req(stages(vec![1, 2], vec![0x2A]), Some(StageChoice::LoserPicksList)));
        let s = last_state(&out, 2);
        assert_eq!(s["ruleset"]["name"], "Custom");
        assert_eq!(s["ruleset"]["stageChoice"], "loser-picks-list");
        assert_eq!(s["ruleset"]["stages"], serde_json::json!({"starters": [1, 2], "counterpicks": [0x2A]}));
        assert!(s["slots"][1]["player"]["ready"] == false, "B's ready not taken back: {s}");
        // Back to any stage.
        let out = t.req(1, req(stages(vec![1, 2], vec![0x2A]), Some(StageChoice::LoserPicks)));
        assert_eq!(last_state(&out, 1)["ruleset"]["stageChoice"], "loser-picks");

        // Striking every game: the choice stays, the pattern follows the starters.
        let strike = Ruleset { stage_choice: StageChoice::Strike, strikes: vec![1, 2, 1], ..Default::default() };
        let mut out = vec![];
        t.book.apply_ruleset(&code, strike, Some("BCDFGH".into()), &mut out).unwrap();
        let out = t.req(1, req(stages(vec![1, 2, 0x21], vec![]), Some(StageChoice::LoserPicksList)));
        let s = last_state(&out, 1);
        assert_eq!(
            (s["ruleset"]["stageChoice"].clone(), s["ruleset"]["name"].clone()),
            ("strike".into(), "Custom".into())
        );
        assert_eq!(s["ruleset"]["basedOn"], "P+ default v0");
        assert_eq!(t.book.rooms[&code].ruleset.strikes, vec![1, 1]);
    }

    /// The host's MATCH SETUP page (`room-format`, docs/design/special-rules.md §4): the stage
    /// choice, strike pattern, bans, DSR and alternate stages; Custom, every ready taken back;
    /// what does not fit the stages is refused, and nothing during a set.
    #[test]
    fn the_host_edits_the_rooms_format() {
        use common::rooms::RoomFormat;
        use common::rules::StageChoice;
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        t.req(2, RoomRequest::Ready { ready: true, character: Some(2), costume: Some(0), stocks: None, hp: None });
        let f = |stage_choice, strikes: Vec<u8>, bans, dsr, alternate_stages| {
            RoomRequest::Format(RoomFormat { kind: None, stage_choice, strikes, bans, dsr, alternate_stages })
        };
        let out = t.req(2, f(StageChoice::Random, vec![], 0, false, false));
        assert_eq!(msgs(&out, 2)[0]["error"], msg::HOST_ONLY);
        // Strike every game with a 2-2 pattern over the default's five starters.
        let out = t.req(1, f(StageChoice::Strike, vec![2, 2], 0, false, false));
        let s = last_state(&out, 2);
        assert_eq!(s["ruleset"]["name"], "Custom");
        assert_eq!(s["ruleset"]["stageChoice"], "strike");
        assert_eq!(s["ruleset"]["strikes"], serde_json::json!([2, 2]));
        assert!(s["slots"][1]["player"]["ready"] == false, "B's ready not taken back: {s}");
        // A pattern that does not leave one starter is refused; striking without one gets the default.
        let out = t.req(1, f(StageChoice::Strike, vec![1, 1], 0, false, false));
        assert_eq!(msgs(&out, 1)[0]["error"], msg::FORMAT_NOT_POSSIBLE);
        let out = t.req(1, f(StageChoice::Strike, vec![], 0, false, false));
        assert_eq!(last_state(&out, 1)["ruleset"]["strikes"], serde_json::json!([1, 2, 1]));
        // Loser picks from the list with alternate stages; no set: bans and DSR are normalized away.
        let out = t.req(1, f(StageChoice::LoserPicksList, vec![1, 2, 1], 2, true, true));
        let s = last_state(&out, 1);
        assert_eq!(s["ruleset"]["stageChoice"], "loser-picks-list");
        assert_eq!(s["ruleset"]["alternateStages"], true);
        assert!(s["ruleset"].get("strikes").is_none() && s["ruleset"].get("bans").is_none(), "{s}");
        // With a set: its bans and DSR; the stage choice stays loser picks.
        t.req(1, RoomRequest::BestOf { best_of: 3 });
        let out = t.req(1, f(StageChoice::Random, vec![1, 2, 1], 1, false, false));
        let s = last_state(&out, 1);
        assert_eq!((s["ruleset"]["bans"].clone(), s["ruleset"]["dsr"].clone()), (1.into(), serde_json::Value::Null));
        assert_eq!(s["ruleset"]["stageChoice"], "loser-picks");
        let out = t.req(1, f(StageChoice::LoserPicks, vec![1, 2, 1], 3, true, false));
        let s = last_state(&out, 1);
        assert_eq!((s["ruleset"]["bans"].clone(), s["ruleset"]["dsr"].clone()), (3.into(), true.into()));
        // More bans than the stages leave a pick for: refused (5 starters + 4 counterpicks hold 3).
        let r = t.book.rooms[&code].ruleset.clone();
        assert_eq!(r.bans, 3);
        // A set that is on: refused.
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        ready_all(&mut t, &[1, 2]);
        t.book.ticket(400, addr(1), String::new(), info(1).uid, &code).unwrap();
        t.book.ticket(401, addr(2), String::new(), info(2).uid, &code).unwrap();
        for n in 1..=2 {
            t.req(n, RoomRequest::Back { pickers: None });
        }
        let out = t.req(1, f(StageChoice::LoserPicks, vec![1, 2, 1], 0, false, false));
        assert_eq!(msgs(&out, 1)[0]["error"], msg::SET_ON);
    }

    fn played(out: &[Output]) -> Vec<String> {
        out.iter()
            .filter_map(|o| match o {
                Output::RulesetPlayed { code } => Some(code.clone()),
                _ => None,
            })
            .collect()
    }

    /// Games count for the room's published ruleset (`ruleset_plays`): a game without a set once
    /// it lasted 30 s, each agreed game of a set (not a quit), never once the rules were edited.
    #[test]
    fn games_count_for_the_published_ruleset() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        let mut r = Ruleset { name: "Weekly".into(), version: 1, ..Default::default() };
        r.normalize();
        let mut out = vec![];
        t.book.apply_ruleset(&code, r, Some("BCDFGH".into()), &mut out).unwrap();
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        let start = |t: &mut T, ticket: ConnId| {
            ready_all(t, &[1, 2]);
            t.book.ticket(ticket, addr(1), String::new(), info(1).uid, &code).unwrap();
            let done = t.book.ticket(ticket + 1, addr(2), String::new(), info(2).uid, &code).unwrap();
            assert!(done.matched[0].1.set.is_none());
        };
        // A quick quit counts nothing; a game of 30 s and more counts once, when it is over.
        start(&mut t, 101);
        t.now += Duration::from_secs(10);
        assert!(played(&t.req(1, RoomRequest::Back { pickers: None })).is_empty());
        assert!(played(&t.req(2, RoomRequest::Back { pickers: None })).is_empty());
        start(&mut t, 103);
        t.now += Duration::from_secs(31);
        assert!(played(&t.req(1, RoomRequest::Back { pickers: None })).is_empty());
        assert_eq!(played(&t.req(2, RoomRequest::Back { pickers: None })), vec!["BCDFGH".to_string()]);
        // Edited rules are Custom: nothing counts.
        let rules = MatchRules { stocks: 2, ..t.book.rooms[&code].ruleset.rules.clone() };
        t.req(1, RoomRequest::Rules { rules });
        start(&mut t, 105);
        t.now += Duration::from_secs(60);
        t.req(1, RoomRequest::Back { pickers: None });
        assert!(played(&t.req(2, RoomRequest::Back { pickers: None })).is_empty());

        // A set: each game both report counts at once, however short; a quit (void) does not, nor
        // does the set's end.
        let mut r =
            Ruleset { name: "Bo3".into(), version: 1, best_of: 3, strikes: vec![1, 2, 1], ..Default::default() };
        r.normalize();
        let mut out = vec![];
        t.book.apply_ruleset(&code, r, Some("CDFGHJ".into()), &mut out).unwrap();
        play(&mut t, &code, 107);
        t.req(1, RoomRequest::Result { game: 1, winner_side: 0, stage: 0x21 });
        assert_eq!(
            played(&t.req(2, RoomRequest::Result { game: 1, winner_side: 0, stage: 0x21 })),
            vec!["CDFGHJ".to_string()]
        );
        t.req(1, RoomRequest::Result { game: 2, winner_side: common::rooms::SIDE_VOID, stage: 0x21 });
        assert!(played(&t.req(2, RoomRequest::Result { game: 2, winner_side: common::rooms::SIDE_VOID, stage: 0x21 }))
            .is_empty());
        t.req(1, RoomRequest::Result { game: 2, winner_side: 0, stage: 0x21 });
        assert_eq!(
            played(&t.req(2, RoomRequest::Result { game: 2, winner_side: 0, stage: 0x21 })),
            vec!["CDFGHJ".to_string()]
        );
        t.now += Duration::from_secs(600);
        t.req(1, RoomRequest::Back { pickers: None });
        assert!(played(&t.req(2, RoomRequest::Back { pickers: None })).is_empty());
    }

    /// Players 1 and 2 ready up and their tickets come in (as `ticket..ticket+1`): the set ticket.
    fn play(t: &mut T, code: &str, ticket: ConnId) -> common::proto::SetTicket {
        ready_all(t, &[1, 2]);
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        t.book.ticket(ticket, addr(1), String::new(), info(1).uid, code).unwrap();
        let done = t.book.ticket(ticket + 1, addr(2), String::new(), info(2).uid, code).unwrap();
        let mut out = vec![];
        t.book.announce(code, &mut out);
        done.matched[0].1.set.clone().expect("the set ticket")
    }

    /// A banned player (`pp-admin user ban`) leaves like a removed one: their side forfeits the set
    /// that is on, instead of the set pausing for them with their slot held.
    #[test]
    fn a_banned_player_forfeits_the_set() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        let mut r =
            Ruleset { name: "Weekly".into(), version: 3, best_of: 3, strikes: vec![1, 2, 1], ..Default::default() };
        r.normalize();
        t.book.apply_ruleset(&code, r, Some("BCDFGH".into()), &mut vec![]).unwrap();
        play(&mut t, &code, 101);
        t.req(1, RoomRequest::Back { pickers: None });
        let mut out = vec![];
        t.book.banned(t.now, 2, &mut out);
        assert!(msgs(&out, 2).is_empty(), "the ban's own error tells them");
        let s = last_state(&out, 1);
        assert_eq!((s["set"]["finished"].as_bool(), s["set"]["winner"].as_u64()), (Some(true), Some(0)), "{s}");
        assert!(s["slots"][1]["player"].is_null());
    }

    #[test]
    fn a_2v2_set_is_played_by_teams() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        t.req(1, RoomRequest::Slot { slot: 4, open: true });
        t.req(1, RoomRequest::Teams { on: true, colors: false });
        for n in 2..=4 {
            join(&mut t, n, &code);
        }
        // Teams by colour: players 1 and 3 red, 2 and 4 blue.
        t.req(3, RoomRequest::Team { team: 0 });
        t.req(4, RoomRequest::Team { team: 1 });
        let mut r = Ruleset { best_of: 3, strikes: vec![1, 2, 1], ..Default::default() };
        r.normalize();
        let mut out = vec![];
        t.book.apply_ruleset(&code, r, Some("BCDFGH".into()), &mut out).unwrap();
        ready_all(&mut t, &[1, 2, 3, 4]);
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        let mut done = TicketOutcome::default();
        for n in 1..=4u8 {
            done = t.book.ticket(100 + n as ConnId, addr(n), String::new(), info(n).uid, &code).unwrap();
        }
        let set = done.matched[0].1.set.clone().expect("the set ticket");
        assert_eq!((set.sides, set.members.clone()), ([0, 1], vec![vec![0, 2], vec![1, 3]]));
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        let s = last_state(&out, 1);
        assert_eq!(s["set"]["members"], serde_json::json!([[1, 3], [2, 4]]));
        // Every player reports; the game counts once all four agree.
        for n in 1..=3 {
            t.req(n, RoomRequest::Result { game: 1, winner_side: 1, stage: 0x21 });
        }
        let out = t.req(4, RoomRequest::Result { game: 1, winner_side: 1, stage: 0x21 });
        assert_eq!(last_state(&out, 1)["set"]["score"], serde_json::json!([0, 1]));
        // A teammate who leaves mid-match loses the game for the team.
        t.req(2, RoomRequest::Step { game: 2, actions: [vec![], vec![]], match_started: true });
        t.req(3, RoomRequest::Leave);
        let out = t.req(1, RoomRequest::Back { pickers: None });
        let s = last_state(&out, 1);
        assert_eq!(
            (s["set"]["score"].clone(), s["set"]["finished"].as_bool()),
            (serde_json::json!([0, 2]), Some(true))
        );
        assert_eq!(s["set"]["winner"], 1);
        for n in [2, 4] {
            t.req(n, RoomRequest::Back { pickers: None });
        }
        join(&mut t, 3, &code);
        let out = t.req(3, RoomRequest::Team { team: 0 });
        assert_eq!(last_state(&out, 1)["statusText"], "player2 & player4 wins the set 2-0");
    }

    fn ready_all(t: &mut T, conns: &[ConnId]) -> Vec<Output> {
        let mut out = vec![];
        for &c in conns {
            out = t.req(c, RoomRequest::Ready { ready: true, character: Some(c as u8), costume: Some(0), stocks: None, hp: None });
        }
        out
    }

    fn addr(n: u8) -> SocketAddrV4 {
        SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16)
    }

    fn sit(t: &mut T, by: ConnId, slot: u8, sit_out: bool) -> Vec<Output> {
        t.req(by, RoomRequest::SitOut { slot, sit_out })
    }

    fn start_of(out: &[Output], conn: ConnId) -> Option<Value> {
        msgs(out, conn).into_iter().find(|m| m["type"] == "room-start")
    }

    /// The tickets of `conns` (as `100 + conn`); the last one's outcome.
    fn tickets(t: &mut T, code: &str, conns: &[ConnId]) -> TicketOutcome {
        let mut done = TicketOutcome::default();
        for &c in conns {
            done = t.book.ticket(100 + c, addr(c as u8), String::new(), info(c as u8).uid, code).unwrap();
        }
        done
    }

    /// A room of four open slots with two players starts as soon as both are ready (the user's,
    /// 2026-10-18: "you can always play with open slots"); a third who joins during the game plays
    /// the next one, which waits for them.
    #[test]
    fn two_players_start_with_open_slots_empty() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        t.req(1, RoomRequest::Slot { slot: 4, open: true });
        join(&mut t, 2, &code);
        let out = ready_all(&mut t, &[1, 2]);
        let start = start_of(&out, 1).expect("room-start");
        assert_eq!(start["players"], serde_json::json!([1, 2]));
        assert_eq!(start["host"], 1);
        // Joined during the start: not in this game (no ticket), waits for it.
        let out = join(&mut t, 3, &code);
        assert_eq!(last_state(&out, 3)["statusText"], msg::STARTING_GAME);
        assert_eq!(
            t.book.ticket(103, addr(3), String::new(), info(3).uid, &code).unwrap_err(),
            msg::NOT_IN_THIS_GAME
        );
        let done = tickets(&mut t, &code, &[1, 2]);
        assert_eq!(done.matched.len(), 2);
        assert_eq!(done.record.as_ref().unwrap().room.mode, "1v1");
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        assert_eq!(last_state(&out, 3)["statusText"], msg::WAITING_FOR_GAME);
        assert_eq!(last_state(&out, 3)["slots"][2]["player"]["inGame"], false);
        // They can ready during the game; back, the next game waits only for the two.
        t.req(3, RoomRequest::Ready { ready: true, character: Some(3), costume: Some(0), stocks: None, hp: None });
        t.req(1, RoomRequest::Back { pickers: Some(2) });
        let out = t.req(2, RoomRequest::Back { pickers: Some(2) });
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player1", "player2"]));
        assert_eq!(last_state(&out, 1)["mode"], "ffa");
        let out = ready_all(&mut t, &[1, 2]);
        let start = start_of(&out, 3).expect("room-start");
        assert_eq!(start["players"], serde_json::json!([1, 2, 3]));
        assert_eq!(start["pickers"], 2, "the last game's loser (P2) plays and picks");
    }

    /// SIT OUT (the user's, 2026-10-18): the game starts without who sits out, they get no
    /// `room-start` and their ticket is refused; a player sits only themselves out, the host
    /// anyone; sitting out takes the ready back, a ready plays again.
    #[test]
    fn a_player_who_sits_out_is_left_out_of_the_game() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        join(&mut t, 3, &code);
        t.req(3, RoomRequest::Ready { ready: true, character: Some(3), costume: Some(0), stocks: None, hp: None });
        // Not the host: only themselves.
        assert_eq!(msgs(&sit(&mut t, 2, 3, true), 2)[0]["error"], msg::HOST_ONLY);
        let out = sit(&mut t, 3, 3, true);
        let s = last_state(&out, 1);
        assert_eq!(s["slots"][2]["player"]["sittingOut"], true);
        assert_eq!(s["slots"][2]["player"]["ready"], false, "sitting out takes the ready back");
        assert_eq!(s["statusText"], msg::waiting_on(&["player1", "player2"]));
        // The host sits P2 out too: one player left, nothing starts.
        let out = sit(&mut t, 1, 2, true);
        assert_eq!(last_state(&out, 2)["statusText"], msg::NEED_TWO_PLAYERS);
        assert!(start_of(&ready_all(&mut t, &[1]), 1).is_none());
        // and back in.
        sit(&mut t, 1, 2, false);
        let out = ready_all(&mut t, &[2]);
        let start = start_of(&out, 1).expect("room-start");
        assert_eq!(start["players"], serde_json::json!([1, 2]));
        assert!(start_of(&out, 3).is_none(), "who sits out sends no ticket");
        assert_eq!(last_state(&out, 3)["status"], "starting");
        // During the start a player can't sit out; the one sitting out leaving cancels nothing.
        assert_eq!(msgs(&sit(&mut t, 2, 2, true), 2)[0]["error"], msg::ROOM_STARTING);
        let out = t.req(3, RoomRequest::Leave);
        assert_eq!(last_state(&out, 1)["status"], "starting");
        join(&mut t, 3, &code);
        assert_eq!(
            t.book.ticket(103, addr(3), String::new(), info(3).uid, &code).unwrap_err(),
            msg::NOT_IN_THIS_GAME
        );
        let done = tickets(&mut t, &code, &[1, 2]);
        let rec = done.record.as_ref().unwrap();
        assert_eq!((rec.players.len(), rec.room.mode.as_str(), rec.room.ports.clone()), (2, "1v1", vec![1, 2]));
        // In the game: its players can't sit out; who sits out can play the next one.
        assert_eq!(msgs(&sit(&mut t, 1, 1, true), 1)[0]["error"], msg::BETWEEN_GAMES);
        sit(&mut t, 3, 3, true);
        let out = t.req(3, RoomRequest::Ready { ready: true, character: Some(3), costume: Some(0), stocks: None, hp: None });
        assert_eq!(last_state(&out, 1)["slots"][2]["player"]["sittingOut"], false, "a ready plays");
        assert_eq!(last_state(&out, 3)["statusText"], msg::WAITING_FOR_GAME);
    }

    /// A member who is not in the room's game and wants it (`room-watch`) is a watch target with
    /// the game's match (the live book sends it); one watching it is not waited for: the next game
    /// starts without them (the user's, 2026-10-19: "finish, never block"), and once they are back
    /// they are waited for again. Stopping or leaving ends the watch.
    #[test]
    fn a_member_watching_the_game_is_not_waited_for() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        for c in 1..=2 {
            t.req(c, RoomRequest::Watch { want: Some(true), watching: None });
        }
        ready_all(&mut t, &[1, 2]);
        tickets(&mut t, &code, &[1, 2]);
        assert!(t.book.watch_targets().is_empty(), "the players are in the game");
        // A joiner during the game watches it.
        join(&mut t, 3, &code);
        assert!(t.req(3, RoomRequest::Watch { want: Some(true), watching: None }).is_empty(), "nothing to show");
        let targets = t.book.watch_targets();
        assert_eq!(targets.len(), 1);
        assert_eq!(targets[0].0, 3);
        assert!(targets[0].1.starts_with(&format!("mode.room-{code}-")));
        let out = t.req(3, RoomRequest::Watch { want: None, watching: Some(true) });
        assert_eq!(last_state(&out, 1)["slots"][2]["player"]["watching"], true);
        // The players are back while it still watches: the next game starts without it. One back
        // before the other never watches the game they played.
        t.req(1, RoomRequest::Back { pickers: None });
        assert_eq!(t.book.watch_targets().len(), 1, "player 1 is back, player 2 still plays");
        let out = t.req(1, RoomRequest::Watch { want: None, watching: Some(true) });
        assert!(last_state(&out, 1)["slots"][0]["player"].get("watching").is_none());
        let out = t.req(2, RoomRequest::Back { pickers: None });
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player1", "player2"]));
        let out = ready_all(&mut t, &[1, 2]);
        assert_eq!(start_of(&out, 1).expect("room-start")["players"], serde_json::json!([1, 2]));
        tickets(&mut t, &code, &[1, 2]);
        assert_eq!(t.book.watch_targets().len(), 1, "and watches that one too");
        // Back on the CSS: the next game waits for it.
        let out = t.req(3, RoomRequest::Watch { want: None, watching: Some(false) });
        assert!(last_state(&out, 1)["slots"][2]["player"].get("watching").is_none());
        t.req(1, RoomRequest::Back { pickers: None });
        let out = t.req(2, RoomRequest::Back { pickers: None });
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player1", "player2", "player3"]));
        let out = ready_all(&mut t, &[1, 2]);
        assert!(start_of(&out, 1).is_none());
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player3"]));
        // Left early: no longer a target, and its watch stops.
        assert!(t.book.take_watch_stops().is_empty());
        t.req(3, RoomRequest::Watch { want: Some(false), watching: None });
        assert_eq!(t.book.take_watch_stops(), vec![3]);
        t.req(3, RoomRequest::Watch { want: Some(true), watching: None });
        t.req(3, RoomRequest::Leave);
        assert_eq!(t.book.take_watch_stops(), vec![3]);
    }

    /// A member still marked watching 30 s after the room's game is over (their game never said it
    /// is back) is waited for again; a ready brings them back too.
    #[test]
    fn a_watch_that_never_ends_times_out() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        ready_all(&mut t, &[1, 2]);
        tickets(&mut t, &code, &[1, 2]);
        join(&mut t, 3, &code);
        t.req(3, RoomRequest::Watch { want: Some(true), watching: Some(true) });
        t.req(1, RoomRequest::Back { pickers: None });
        t.req(2, RoomRequest::Back { pickers: None });
        let mut out = vec![];
        t.book.tick(t.now + Duration::from_secs(5), &mut out);
        assert!(out.is_empty(), "still within the game's tail");
        t.book.tick(t.now + Duration::from_secs(31), &mut out);
        assert!(last_state(&out, 1)["slots"][2]["player"].get("watching").is_none());
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player1", "player2", "player3"]));
        t.req(3, RoomRequest::Watch { want: None, watching: Some(true) });
        let out = t.req(3, RoomRequest::Ready { ready: true, character: Some(3), costume: Some(0), stocks: None, hp: None });
        assert!(last_state(&out, 1)["slots"][2]["player"].get("watching").is_none(), "a ready is back");
    }

    /// Sitting out stays until the player plays again: the next games start without them.
    #[test]
    fn sitting_out_lasts_across_games() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        join(&mut t, 3, &code);
        sit(&mut t, 3, 3, true);
        for _ in 0..2 {
            let out = ready_all(&mut t, &[1, 2]);
            assert_eq!(start_of(&out, 1).expect("room-start")["players"], serde_json::json!([1, 2]));
            tickets(&mut t, &code, &[1, 2]);
            t.req(1, RoomRequest::Back { pickers: Some(1) });
            let out = t.req(2, RoomRequest::Back { pickers: Some(1) });
            assert_eq!(last_state(&out, 1)["slots"][2]["player"]["sittingOut"], true);
        }
    }

    /// The host sits out: the game's host (the session's decider) is the earliest-joined player
    /// of the game; the room's host stays the host.
    #[test]
    fn the_host_sits_out_and_the_game_has_another_host() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        join(&mut t, 3, &code);
        sit(&mut t, 1, 1, true);
        let out = ready_all(&mut t, &[3, 2]);
        let start = start_of(&out, 2).expect("room-start");
        assert_eq!((start["players"].clone(), start["host"].clone()), (serde_json::json!([2, 3]), 2.into()));
        let done = tickets(&mut t, &code, &[2, 3]);
        for (conn, resp) in &done.matched {
            assert_eq!(resp.is_host, Some(*conn == 102));
        }
        assert_eq!(done.record.as_ref().unwrap().host, info(2).uid);
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        assert_eq!(last_state(&out, 2)["host"], 1);
        // Still the host: sits P3 out between games.
        t.req(2, RoomRequest::Back { pickers: None });
        t.req(3, RoomRequest::Back { pickers: None });
        let out = sit(&mut t, 1, 3, true);
        assert_eq!(last_state(&out, 3)["slots"][2]["player"]["sittingOut"], true);
    }

    /// The loser picks: the last game's loser when they play the next game, else one of its
    /// players at random (the user's, 2026-10-18), also when the loser left; nothing from an older
    /// Dolphin (no `pickers` in `room-back`), so the players' games decide as before.
    #[test]
    fn the_stage_pick_goes_to_a_player_when_the_loser_sits_out() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        join(&mut t, 3, &code);
        sit(&mut t, 3, 3, true);
        let out = ready_all(&mut t, &[1, 2]);
        assert!(start_of(&out, 1).unwrap().get("pickers").is_none(), "game 1: no report yet");
        tickets(&mut t, &code, &[1, 2]);
        t.req(1, RoomRequest::Back { pickers: Some(0b10) });
        t.req(2, RoomRequest::Back { pickers: Some(0b10) });
        // Who sits out reporting changes nothing (they were not in the game).
        t.req(3, RoomRequest::Back { pickers: Some(0b1) });
        // P2 lost and sits out; P1 and P3 play: one of them picks.
        sit(&mut t, 2, 2, true);
        sit(&mut t, 3, 3, false);
        let mut seen = HashSet::new();
        for draw in 0..40 {
            let mut room = t.book.rooms[&code].clone();
            room.pick_draw = draw;
            assert_eq!(room.next_pickers(&room.playing()), room.next_pickers(&room.playing()), "stable");
            let p = room.next_pickers(&room.playing()).unwrap();
            assert!(p == 0b1 || p == 0b100, "{p:#b}");
            seen.insert(p);
        }
        assert_eq!(seen.len(), 2, "drawn among the players");
        // The room's CSS shows who picks before anyone readies, and the start agrees.
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        let shown = last_state(&out, 1)["pickers"].as_u64().unwrap();
        assert!(shown == 0b1 || shown == 0b100, "{shown:#b}");
        let out = ready_all(&mut t, &[1, 3]);
        assert_eq!(start_of(&out, 1).unwrap()["pickers"].as_u64(), Some(shown));
        tickets(&mut t, &code, &[1, 3]);
        // A draw left both picking; one of them sits out: the other picks.
        t.req(1, RoomRequest::Back { pickers: Some(0b101) });
        t.req(3, RoomRequest::Back { pickers: Some(0b101) });
        sit(&mut t, 3, 3, true);
        sit(&mut t, 2, 2, false);
        let out = ready_all(&mut t, &[1, 2]);
        assert_eq!(start_of(&out, 1).unwrap()["pickers"], 0b1);
        tickets(&mut t, &code, &[1, 2]);
        // The loser plays again after the others readied: they pick, and the one the pick was
        // drawn for readies again (their lock-in said they pick); the other keeps their ready.
        t.req(1, RoomRequest::Back { pickers: Some(0b10) });
        t.req(2, RoomRequest::Back { pickers: Some(0b10) });
        sit(&mut t, 2, 2, true);
        sit(&mut t, 3, 3, false);
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        let drawn = last_state(&out, 1)["pickers"].as_u64().unwrap() as u8;
        let (drawn_conn, other) = if drawn == 0b1 { (1, 3) } else { (3, 1) };
        ready_all(&mut t, &[drawn_conn, other]);
        // (P2 sitting out, both ready: that game starts.) P3 loses it and sits out; P1 and P2
        // play, and one of them gets the pick.
        tickets(&mut t, &code, &[1, 3]);
        t.req(1, RoomRequest::Back { pickers: Some(0b100) });
        t.req(3, RoomRequest::Back { pickers: Some(0b100) });
        sit(&mut t, 3, 3, true);
        sit(&mut t, 2, 2, false);
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        let drawn = last_state(&out, 1)["pickers"].as_u64().unwrap() as u8;
        assert!(drawn == 0b1 || drawn == 0b10, "{drawn:#b}");
        let (drawn_conn, other) = if drawn == 0b1 { (1usize, 2usize) } else { (2, 1) };
        // The one with the pick readies; then P3 plays again and takes the pick back: that
        // ready is taken back (their lock-in said they pick), the other's is kept.
        ready_all(&mut t, &[drawn_conn as ConnId]);
        let out = sit(&mut t, 3, 3, false);
        let s = last_state(&out, 1);
        assert_eq!(s["pickers"], 0b100);
        assert_eq!(s["slots"][drawn_conn - 1]["player"]["ready"], false, "the pick moved away: {s}");
        ready_all(&mut t, &[other as ConnId]);
        let s = last_state(&sit(&mut t, 3, 3, false), 1);
        assert_eq!(s["slots"][other - 1]["player"]["ready"], true, "no pick, no change: {s}");
        let out = ready_all(&mut t, &[3, drawn_conn as ConnId]);
        let start = start_of(&out, 1).unwrap();
        assert_eq!((start["pickers"].clone(), start["players"].clone()), (0b100.into(), serde_json::json!([1, 2, 3])));
        tickets(&mut t, &code, &[1, 2, 3]);
        // The loser left: the pick is still someone's.
        t.req(1, RoomRequest::Back { pickers: Some(0b10) });
        t.req(2, RoomRequest::Back { pickers: Some(0b10) });
        t.req(2, RoomRequest::Leave);
        t.req(3, RoomRequest::Back { pickers: Some(0b10) });
        let out = ready_all(&mut t, &[1, 3]);
        let p = start_of(&out, 1).unwrap()["pickers"].as_u64().unwrap();
        assert!(p == 0b1 || p == 0b100, "{p:#b}");
        tickets(&mut t, &code, &[1, 3]);
        // Game 1's random stage (no picker) stays random.
        t.req(1, RoomRequest::Back { pickers: Some(0) });
        t.req(3, RoomRequest::Back { pickers: Some(0) });
        let out = ready_all(&mut t, &[1, 3]);
        assert_eq!(start_of(&out, 1).unwrap()["pickers"], 0);
        tickets(&mut t, &code, &[1, 3]);
        // A ruleset that strikes leaves no stage to the loser: nothing sent.
        t.req(1, RoomRequest::Back { pickers: Some(0b1) });
        t.req(3, RoomRequest::Back { pickers: Some(0b1) });
        let strike = Ruleset { stage_choice: StageChoice::Random, ..Default::default() };
        t.book.apply_ruleset(&code, strike, None, &mut vec![]).unwrap();
        let out = ready_all(&mut t, &[1, 3]);
        assert!(start_of(&out, 1).unwrap().get("pickers").is_none());
    }

    /// The coin placed on the CSS shows to the others before the player readies (the user's,
    /// 2026-10-18), taken in the hand it is gone; game 1 of a set still hides it (a blind pick).
    #[test]
    fn the_placed_character_shows_before_the_ready() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        let pick = |c: Option<u8>| RoomRequest::Ready { ready: false, character: c, costume: c.map(|_| 2), stocks: None, hp: None };
        let out = t.req(2, pick(Some(7)));
        let p = &last_state(&out, 1)["slots"][1]["player"];
        assert_eq!((p["ready"].clone(), p["character"].clone(), p["costume"].clone()), (false.into(), 7.into(), 2.into()));
        let out = t.req(2, pick(None));
        assert!(last_state(&out, 1)["slots"][1]["player"]["character"].is_null());
        // Stocks show only with a ready.
        let out = t.req(2, RoomRequest::Ready { ready: false, character: Some(7), costume: Some(0), stocks: Some(3), hp: None });
        assert!(last_state(&out, 1)["slots"][1]["player"].get("stocks").is_none());
        // Game 1 of a set: the others see nothing, the player their own pick.
        t.req(1, RoomRequest::BestOf { best_of: 3 });
        let out = t.req(2, pick(Some(7)));
        assert!(last_state(&out, 1)["slots"][1]["player"]["character"].is_null());
        assert_eq!(last_state(&out, 2)["slots"][1]["player"]["character"], 7);
    }

    /// A 1v1 set in a room of three (the user's, 2026-10-18): the third sits out and the set is
    /// played by the other two to the end; meanwhile the third (and anyone who joins) sits out and
    /// can't play, and the set's players can't sit out. After the set the third is as they chose.
    #[test]
    fn a_set_in_a_room_of_three_is_played_by_two() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        t.req(1, RoomRequest::Slot { slot: 4, open: true });
        join(&mut t, 2, &code);
        join(&mut t, 3, &code);
        t.req(1, RoomRequest::BestOf { best_of: 3 });
        let out = sit(&mut t, 3, 3, true);
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player1", "player2"]));
        let set = play(&mut t, &code, 200);
        assert_eq!(set.best_of, 3);
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        let s = last_state(&out, 3);
        assert_eq!(s["set"]["members"], serde_json::json!([[1], [2]]));
        assert_eq!(s["slots"][2]["player"]["sittingOut"], true);
        // Locked until the set is over: the third, a joiner, the set's players.
        assert_eq!(msgs(&sit(&mut t, 3, 3, false), 3)[0]["error"], msg::SET_SIT_OUT);
        assert_eq!(msgs(&sit(&mut t, 1, 3, false), 1)[0]["error"], msg::SET_SIT_OUT);
        let out = join(&mut t, 4, &code);
        assert_eq!(last_state(&out, 4)["slots"][3]["player"]["sittingOut"], true);
        let ready4 = RoomRequest::Ready { ready: true, character: Some(4), costume: Some(0), stocks: None, hp: None };
        assert_eq!(msgs(&t.req(4, ready4.clone()), 4)[0]["error"], msg::SET_SIT_OUT);
        // A pick still shows (not in the set's blind game 1 to the others, but to themselves).
        let out = t.req(4, RoomRequest::Ready { ready: false, character: Some(4), costume: Some(0), stocks: None, hp: None });
        assert_eq!(last_state(&out, 4)["slots"][3]["player"]["character"], 4);
        // The set's games end: its players back, then win it 2-0.
        for g in 1..=2u32 {
            for c in [1, 2] {
                t.req(c, RoomRequest::Result { game: g, winner_side: 0, stage: 1 });
            }
        }
        for c in [1, 2] {
            t.req(c, RoomRequest::Back { pickers: None });
        }
        assert_eq!(msgs(&sit(&mut t, 2, 2, true), 2).iter().find(|m| m["type"] == "room-error"), None);
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        let s = last_state(&out, 1);
        assert_eq!(s["set"]["finished"], true);
        // P3 chose to sit out, P4 joined playing: P4 plays now.
        assert_eq!(s["slots"][2]["player"]["sittingOut"], true);
        assert_eq!(s["slots"][3]["player"]["sittingOut"], false);
        assert_eq!(s["slots"][1]["player"]["sittingOut"], true);
    }

    #[test]
    fn start_tickets_and_game_over() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        join(&mut t, 3, &code);
        let out = t.req(1, RoomRequest::Ready { ready: true, character: Some(9), costume: Some(2), stocks: None, hp: None });
        let s = last_state(&out, 2);
        assert_eq!(s["slots"][0]["player"]["character"], 9);
        assert_eq!(s["statusText"], "Waiting on: player2, player3");
        let out = ready_all(&mut t, &[2, 3]);
        for c in 1..=3 {
            let m = msgs(&out, c);
            let start = m.iter().find(|x| x["type"] == "room-start").expect("room-start");
            assert!(start["matchId"].as_str().unwrap().starts_with(&format!("mode.room-{code}-")));
        }
        assert_eq!(last_state(&out, 1)["status"], "starting");
        // Changes wait; nobody else fits.
        assert_eq!(msgs(&t.req(1, RoomRequest::Teams { on: true, colors: false }), 1)[0]["error"], msg::BETWEEN_GAMES);
        assert_eq!(msgs(&join(&mut t, 9, &code), 9)[0]["error"], msg::ROOM_FULL);
        // Tickets: a stranger is refused, members are matched once all are in.
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        assert_eq!(t.book.ticket(100, addr(9), String::new(), info(9).uid, &code).unwrap_err(), msg::NOT_A_MEMBER);
        assert_eq!(t.book.ticket(100, addr(9), String::new(), info(1).uid, "ZZZZ").unwrap_err(), msg::ROOM_NOT_FOUND);
        assert!(t
            .book
            .ticket(101, addr(1), "192.168.0.2:41001".into(), info(1).uid, &code)
            .unwrap()
            .matched
            .is_empty());
        let waiting = t.book.ticket(102, addr(2), String::new(), info(2).uid, &code).unwrap();
        assert!(waiting.matched.is_empty() && waiting.record.is_none());
        let done = t.book.ticket(103, addr(3), String::new(), info(3).uid, &code).unwrap();
        assert_eq!(done.matched.len(), 3);
        // Recorded for the match history: a free-for-all, players in slot order.
        let rec = done.record.as_ref().expect("record");
        assert_eq!(Some(&rec.match_id), done.matched[0].1.match_id.as_ref());
        assert_eq!(rec.players, vec![info(1).uid, info(2).uid, info(3).uid]);
        assert_eq!(rec.host, info(1).uid);
        assert_eq!(
            rec.room,
            common::db::MatchRoom {
                code: code.clone(),
                mode: "ffa".into(),
                ports: vec![1, 2, 3],
                teams: None,
                ruleset: None
            }
        );
        for (conn, resp) in &done.matched {
            let ps = resp.players.as_ref().unwrap();
            assert_eq!(ps.len(), 3);
            assert_eq!(ps.iter().map(|p| p.port).collect::<Vec<_>>(), vec![1, 2, 3]);
            let local = ps.iter().find(|p| p.is_local_player).unwrap();
            assert_eq!(local.port as u64, conn - 100);
            assert_eq!(resp.is_host, Some(*conn == 101));
            assert_eq!(ps[0].ip_address, "203.0.113.1:41001");
            assert_eq!(ps[0].ip_address_lan, "192.168.0.2:41001");
        }
        let mut out = vec![];
        t.book.announce(&code, &mut out);
        let s = last_state(&out, 1);
        assert_eq!(s["status"], "in-game");
        assert_eq!(s["slots"][0]["player"]["ready"], false);
        assert_eq!(s["slots"][0]["player"]["inGame"], true);
        assert_eq!(t.book.public_rooms(10)[0].status, RoomStatus::InGame);
        // Player 3 drops during the game: the slot is open and empty, a newcomer waits.
        t.book.conn_gone(t.now, 3, &mut vec![]);
        let out = join(&mut t, 4, &code);
        assert_eq!(last_state(&out, 4)["statusText"], msg::WAITING_FOR_GAME);
        assert!(t.book.public_rooms(10)[0].joinable || t.book.public_rooms(10)[0].players == 3);
        t.req(1, RoomRequest::Back { pickers: None });
        let out = t.req(2, RoomRequest::Back { pickers: None });
        assert_eq!(last_state(&out, 1)["status"], "waiting");
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player1", "player2", "player4"]));
        assert_eq!(msg::waiting_on(&["player1", "player2", "player4"]), "Waiting on: player1, player2 +1");
    }

    /// A pair that could not connect (prod 2026-10-14: a VPN's player and the host): the first
    /// report names it to every member, once; the other side's report of the same pair and reports
    /// of another game change nothing.
    #[test]
    fn a_pair_that_could_not_connect_is_told_to_the_room_once() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        t.req(1, RoomRequest::Slot { slot: 4, open: true });
        for c in 2..=4 {
            join(&mut t, c, &code);
        }
        let out = ready_all(&mut t, &[1, 2, 3, 4]);
        let start = msgs(&out, 1).into_iter().find(|m| m["type"] == "room-start").expect("room-start");
        let match_id = start["matchId"].as_str().unwrap().to_string();
        let unreachable = |slots: Vec<u8>| RoomRequest::Unreachable { match_id: match_id.clone(), slots };
        // Before the game: nothing to tell.
        assert!(t.req(1, unreachable(vec![4])).is_empty());
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        for n in 1..=4u8 {
            t.book.ticket(100 + n as ConnId, addr(n), String::new(), info(n).uid, &code).unwrap();
        }
        let told = |out: &[Output], c: ConnId| -> Vec<Value> {
            msgs(out, c).into_iter().filter(|m| m["type"] == rooms::ROOM_UNREACHABLE).collect()
        };
        // The host could not reach slot 4: every member hears the pair (and no room-state).
        let out = t.req(1, unreachable(vec![4]));
        for c in 1..=4 {
            let m = told(&out, c);
            assert_eq!(m.len(), 1);
            assert_eq!(m[0]["matchId"], match_id.as_str());
            assert_eq!(m[0]["code"], code.as_str());
            assert_eq!(m[0]["pairs"], serde_json::json!([[1, 4]]));
            assert!(msgs(&out, c).iter().all(|x| x["type"] != "room-state"));
        }
        // Slot 4 reports the same pair (and itself, which is left out): nothing new.
        assert!(t.req(4, unreachable(vec![1, 4])).is_empty());
        // Another pair is new: the message lists both.
        let out = t.req(3, unreachable(vec![4]));
        assert_eq!(told(&out, 2)[0]["pairs"], serde_json::json!([[1, 4], [3, 4]]));
        // Another game's report changes nothing.
        assert!(t.req(2, RoomRequest::Unreachable { match_id: "mode.room-old".into(), slots: vec![4] }).is_empty());
        // Once the game is over, a late report changes nothing either.
        for c in 1..=4 {
            t.req(c, RoomRequest::Back { pickers: None });
        }
        assert!(t.req(2, unreachable(vec![1])).is_empty());
    }

    #[test]
    fn start_times_out_and_leaving_cancels() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        join(&mut t, 2, &code);
        ready_all(&mut t, &[1, 2]);
        let addr = SocketAddrV4::new([203, 0, 113, 1].into(), 41001);
        t.book.ticket(101, addr, String::new(), info(1).uid, &code).unwrap();
        t.now += Duration::from_secs(16);
        let mut out = vec![];
        t.book.tick(t.now, &mut out);
        let failed = t.book.take_failed_tickets();
        assert_eq!(failed, vec![(101, msg::did_not_connect("P#2"))]);
        assert_eq!(msgs(&out, 1).iter().find(|m| m["type"] == "room-error").unwrap()["op"], "room-start");
        let s = last_state(&out, 2);
        assert_eq!(s["status"], "waiting");
        assert_eq!(s["slots"][0]["player"]["ready"], false);
        // Again, and this time player 2 leaves during the start.
        ready_all(&mut t, &[1, 2]);
        t.book.ticket(102, addr, String::new(), info(1).uid, &code).unwrap();
        t.req(2, RoomRequest::Leave);
        assert_eq!(t.book.take_failed_tickets(), vec![(102, msg::PLAYER_LEFT.to_string())]);
    }

    #[test]
    fn team_colours_are_a_teams_setting() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        // Colours without Teams are nothing.
        let s = last_state(&t.req(1, RoomRequest::Teams { on: false, colors: true }), 1);
        assert_eq!((s["teams"].clone(), s["teamColors"].clone()), (serde_json::json!(false), serde_json::json!(false)));
        // The rainbow team battle first, as the host's button goes.
        let s = last_state(&t.req(1, RoomRequest::Teams { on: true, colors: false }), 2);
        assert_eq!((s["teams"].clone(), s["teamColors"].clone()), (serde_json::json!(true), serde_json::json!(false)));
        // A lock-in made in the rainbow one is taken back for the teams' costumes.
        t.req(2, RoomRequest::Ready { ready: true, character: Some(12), costume: Some(3), stocks: None, hp: None });
        let s = last_state(&t.req(1, RoomRequest::Teams { on: true, colors: true }), 2);
        assert_eq!((s["teams"].clone(), s["teamColors"].clone()), (serde_json::json!(true), serde_json::json!(true)));
        assert_eq!(s["slots"][1]["player"]["ready"], serde_json::json!(false));
        // Only the host.
        assert_eq!(msgs(&t.req(2, RoomRequest::Teams { on: true, colors: false }), 2)[0]["error"], msg::HOST_ONLY);
        // Teams off clears it.
        let s = last_state(&t.req(1, RoomRequest::Teams { on: false, colors: false }), 2);
        assert_eq!(s["teamColors"], serde_json::json!(false));
    }

    #[test]
    fn teams_split_rules() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        t.req(1, RoomRequest::Teams { on: true, colors: false });
        join(&mut t, 2, &code);
        join(&mut t, 3, &code);
        for c in 1..=3 {
            t.req(c, RoomRequest::Team { team: 1 });
        }
        let out = ready_all(&mut t, &[1, 2, 3]);
        let s = last_state(&out, 1);
        assert_eq!(s["mode"], "teams");
        assert_eq!(s["status"], "waiting");
        assert_eq!(s["statusText"], msg::PICK_TEAMS);
        // 2v1 is fine.
        let out = t.req(3, RoomRequest::Team { team: 0 });
        assert!(msgs(&out, 1).iter().any(|m| m["type"] == "room-start"));
        // The teams are recorded with the game.
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        for c in 1..=2u8 {
            t.book.ticket(100 + u64::from(c), addr(c), String::new(), info(c).uid, &code).unwrap();
        }
        let done = t.book.ticket(103, addr(3), String::new(), info(3).uid, &code).unwrap();
        let room = done.record.expect("record").room;
        assert_eq!((room.mode.as_str(), room.ports, room.teams), ("teams", vec![1, 2, 3], Some(vec![1, 1, 0])));
    }

    #[test]
    fn two_player_rooms_record_a_1v1_on_their_slots() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        // Slots 1 and 3, Teams on: still a 1v1.
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        t.req(1, RoomRequest::Slot { slot: 2, open: false });
        t.req(1, RoomRequest::Teams { on: true, colors: false });
        join(&mut t, 2, &code);
        ready_all(&mut t, &[1, 2]);
        let addr = |n: u8| SocketAddrV4::new([203, 0, 113, n].into(), 41000 + n as u16);
        t.book.ticket(102, addr(2), String::new(), info(2).uid, &code).unwrap();
        let done = t.book.ticket(101, addr(1), String::new(), info(1).uid, &code).unwrap();
        let rec = done.record.expect("record");
        assert_eq!(rec.players, vec![info(1).uid, info(2).uid]);
        assert_eq!((rec.room.mode.as_str(), rec.room.ports, rec.room.teams), ("1v1", vec![1, 3], None));
    }

    #[test]
    fn rate_limits() {
        let mut t = T::new();
        for _ in 0..5 {
            create(&mut t, 1);
        }
        assert_eq!(
            msgs(&t.req(1, RoomRequest::Create { public: true, ruleset: None, remembered: false }), 1)[0]["error"],
            msg::ROOMS_TOO_OFTEN
        );
        for _ in 0..10 {
            join(&mut t, 2, "ZZZZ");
        }
        assert_eq!(msgs(&join(&mut t, 2, "ZZZZ"), 2)[0]["error"], msg::JOINS_TOO_OFTEN);
        let mut refused = false;
        for _ in 0..60 {
            let out = t.req(3, RoomRequest::Leave);
            refused |= msgs(&out, 3)[0]["error"] == msg::TOO_MANY_REQUESTS;
        }
        assert!(refused);
    }
}
