//! Rooms: the room model, free of I/O (`docs/rooms-protocol.md`, decisions in
//! `docs/design/rooms.md`).
//!
//! A room is in memory only: it exists while someone is in it, and its code is free again as soon
//! as it is empty, so nothing about it needs to outlive the process (a restart of mm empties every
//! room, and the members' games see their online connection drop, as with a search).
//!
//! Members are online connections ([`crate::engine`] state `Online`): one per account. A room has
//! four slots; a slot is the in-game port and stays the same for the life of the room. A new room
//! has slots 1-2 open and 3-4 closed. The game starts by itself once every open slot is taken,
//! every member is ready and nobody is still in the last game. Then each member sends a room game
//! ticket (`create-ticket`, mode 3, the room code as `search.connectCode`) from its P2P port, and
//! once all are in, every member gets one `get-ticket-resp` listing all of them, ports = slots.
//!
//! [`RoomBook`] sends the room messages to members itself (`Output::Send`). Answers to tickets go
//! through the engine, which owns ticket connections: [`RoomBook::ticket`] returns them, and
//! tickets that have to fail (a start that timed out, a member who left) wait in
//! [`RoomBook::take_failed_tickets`].

use std::collections::{BTreeMap, HashMap, HashSet};
use std::net::SocketAddrV4;
use std::time::{Duration, Instant};

use chrono::Utc;
use common::proto::{GetTicketResp, Player, GET_TICKET_RESP};
use common::ratelimit::{RateLimiter, Window};
use common::rooms::{
    self, PublicRoom, RoomError, RoomLeft, RoomMode, RoomPlayer, RoomRequest, RoomSlot, RoomStart, RoomState,
    RoomStatus, ROOM_SLOTS, TEAM_COUNT,
};
use uuid::Uuid;

use crate::engine::{ConnId, Output};
use crate::messages as msg;
use crate::ruleset::ModeRules;

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

/// Who a member is (from the verified account).
#[derive(Debug, Clone, PartialEq)]
pub struct MemberInfo {
    pub uid: Uuid,
    pub display_name: String,
    pub connect_code: String,
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
    in_game: bool,
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
    Starting { match_id: String, since: Instant, tickets: BTreeMap<usize, StartTicket> },
    InGame { since: Instant },
}

#[derive(Debug, Clone)]
struct Room {
    code: String,
    public: bool,
    teams: bool,
    open: [bool; ROOM_SLOTS],
    slots: [Option<Member>; ROOM_SLOTS],
    /// Slot index (0-3) of the host.
    host: usize,
    phase: Phase,
    /// Accounts the host removed: they cannot join this room again.
    kicked: HashSet<Uuid>,
    created: Instant,
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
    fn mode(&self) -> RoomMode {
        RoomMode::of(self.open_count(), self.teams)
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
        let mut teams = self.members().map(|(_, m)| m.team);
        let Some(first) = teams.next() else { return false };
        teams.all(|t| t == first)
    }
    fn can_start(&self) -> bool {
        matches!(self.phase, Phase::Waiting)
            && self.player_count() >= 2
            && (0..ROOM_SLOTS).all(|i| self.open[i] == self.slots[i].is_some())
            && self.members().all(|(_, m)| m.ready && !m.in_game)
            && !self.one_colour()
    }

    fn status_text(&self, me: &Member) -> String {
        match &self.phase {
            Phase::Starting { .. } => msg::STARTING_GAME.into(),
            Phase::InGame { .. } if me.in_game => msg::IN_GAME.into(),
            Phase::InGame { .. } => msg::WAITING_FOR_GAME.into(),
            Phase::Waiting => {
                let (players, open) = (self.player_count(), self.open_count());
                if players == 1 {
                    msg::room_waiting(&self.code)
                } else if players < open {
                    msg::waiting_for_players(players, open)
                } else if self.one_colour() {
                    msg::PICK_TEAMS.into()
                } else {
                    let waiting: Vec<&str> = self
                        .members()
                        .filter(|(_, m)| !m.ready || m.in_game)
                        .map(|(_, m)| m.info.display_name.as_str())
                        .collect();
                    if waiting.is_empty() {
                        msg::STARTING_GAME.into()
                    } else {
                        msg::waiting_on(&waiting)
                    }
                }
            }
        }
    }

    fn state_for(&self, slot: usize) -> Option<RoomState> {
        let me = self.slots[slot].as_ref()?;
        Some(RoomState {
            kind: rooms::ROOM_STATE.into(),
            code: self.code.clone(),
            public: self.public,
            teams: self.teams,
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
                        // The lock-in shows once the player is ready (`docs/design/rooms.md` #15).
                        character: m.character.filter(|_| m.ready),
                        costume: m.costume.filter(|_| m.ready),
                        in_game: m.in_game,
                    }),
                })
                .collect(),
            status_text: self.status_text(me),
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
}

pub struct RoomBook {
    cfg: RoomsConfig,
    rooms: HashMap<String, Room>,
    /// Online connection → the code of its room.
    member_of: HashMap<ConnId, String>,
    /// Room game ticket connection → the code of its room.
    ticket_of: HashMap<ConnId, String>,
    failed_tickets: Vec<(ConnId, String)>,
    join_seq: u64,
    match_counter: u64,
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
            join_seq: 0,
            match_counter: 0,
        }
    }

    pub fn room_count(&self) -> usize {
        self.rooms.len()
    }

    /// The code of the room this online connection is in.
    pub fn room_of(&self, conn: ConnId) -> Option<&str> {
        self.member_of.get(&conn).map(String::as_str)
    }

    /// Tickets that have to be answered with an error (and closed) by the engine.
    pub fn take_failed_tickets(&mut self) -> Vec<(ConnId, String)> {
        std::mem::take(&mut self.failed_tickets)
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
            RoomRequest::Create { public } => self.create(now, conn, who, public, out),
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
        let mut slots: [Option<Member>; ROOM_SLOTS] = Default::default();
        slots[0] = Some(Member {
            conn,
            info: who.clone(),
            joined: self.join_seq,
            ready: false,
            team: 0,
            character: None,
            costume: None,
            in_game: false,
        });
        tracing::info!(%code, host = %who.connect_code, public, "room created");
        self.rooms.insert(
            code.clone(),
            Room {
                code: code.clone(),
                public,
                teams: false,
                open: [true, true, false, false],
                slots,
                host: 0,
                phase: Phase::Waiting,
                kicked: HashSet::new(),
                created: now,
            },
        );
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
        if room.kicked.contains(&who.uid) {
            return Err(msg::REMOVED);
        }
        let slot = room.free_slot().ok_or(msg::ROOM_FULL)?;
        if let Some(old) = self.member_of.get(&conn).cloned() {
            self.leave(now, &old, conn, None, true, out);
        }
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
            in_game: false,
        });
        tracing::info!(%code, player = %who.connect_code, slot = slot + 1, "joined room");
        self.member_of.insert(conn, code.clone());
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
        let (starting, in_game) =
            (matches!(room.phase, Phase::Starting { .. }), matches!(room.phase, Phase::InGame { .. }));
        if starting {
            self.cancel_start(code, msg::PLAYER_LEFT, out);
        } else if in_game {
            self.maybe_game_over(code);
        }
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
                if open {
                    room.open[s] = true;
                } else if room.open[s] {
                    if s == me {
                        return Err(msg::OWN_SLOT);
                    }
                    if room.open_count() <= 2 {
                        return Err(msg::MIN_OPEN_SLOTS);
                    }
                    room.open[s] = false;
                    if let Some(m) = &room.slots[s] {
                        // Closing an occupied slot removes its player (`docs/design/rooms.md` #10).
                        room.kicked.insert(m.info.uid);
                        kick = Some(m.conn);
                    }
                }
            }
            RoomRequest::Teams { on } => {
                if !is_host {
                    return Err(msg::HOST_ONLY);
                }
                if !waiting {
                    return Err(msg::BETWEEN_GAMES);
                }
                if room.teams != on {
                    room.teams = on;
                    // Everyone's lock-in was made for the other mode.
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
            }
            RoomRequest::Ready { ready, character, costume } => {
                if starting {
                    return Err(msg::ROOM_STARTING);
                }
                let m = room.slots[me].as_mut().ok_or(msg::NOT_IN_ROOM)?;
                if m.in_game {
                    return Err(msg::BETWEEN_GAMES);
                }
                m.ready = ready;
                if ready {
                    m.character = character;
                    m.costume = costume;
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
            RoomRequest::Back => {
                if let Some(m) = room.slots[me].as_mut() {
                    m.in_game = false;
                }
                self.maybe_game_over(&code);
            }
            RoomRequest::Create { .. } | RoomRequest::Join { .. } | RoomRequest::Leave => {
                unreachable!("handled in request()")
            }
        }
        if let Some(k) = kick {
            // Broadcasts the new state (without the kicked player) too.
            self.leave(now, &code, k, Some(msg::REMOVED), true, out);
        } else {
            self.broadcast(&code, out);
            self.maybe_start(now, &code, out);
        }
        Ok(())
    }

    /// Back to waiting once nobody is still in the game.
    fn maybe_game_over(&mut self, code: &str) {
        let Some(room) = self.rooms.get_mut(code) else { return };
        if matches!(room.phase, Phase::InGame { .. }) && room.members().all(|(_, m)| !m.in_game) {
            tracing::info!(%code, "room game over");
            room.phase = Phase::Waiting;
        }
    }

    /// Starts the game once every open slot is taken and everyone is ready.
    fn maybe_start(&mut self, now: Instant, code: &str, out: &mut Vec<Output>) {
        let Some(room) = self.rooms.get_mut(code) else { return };
        if !room.can_start() {
            return;
        }
        self.match_counter += 1;
        let match_id = format!(
            "mode.room-{code}-{}-{:x}",
            Utc::now().to_rfc3339_opts(chrono::SecondsFormat::Millis, true),
            self.match_counter
        );
        tracing::info!(%code, %match_id, players = room.player_count(), "room starting");
        room.phase = Phase::Starting { match_id: match_id.clone(), since: now, tickets: BTreeMap::new() };
        let start = RoomStart {
            kind: rooms::ROOM_START.into(),
            code: code.into(),
            match_id,
            timeout_secs: self.cfg.start_timeout.as_secs(),
        };
        for (_, m) in room.members() {
            send(out, m.conn, &start);
        }
        self.broadcast(code, out);
    }

    /// A start that will not happen: its tickets fail, everyone has to ready up again.
    fn cancel_start(&mut self, code: &str, why: &str, out: &mut Vec<Output>) {
        let Some(room) = self.rooms.get_mut(code) else { return };
        let Phase::Starting { tickets, .. } = std::mem::replace(&mut room.phase, Phase::Waiting) else {
            return;
        };
        tracing::info!(%code, "room start cancelled: {why}");
        for t in tickets.values() {
            self.ticket_of.remove(&t.conn);
            self.failed_tickets.push((t.conn, why.into()));
        }
        for m in room.slots.iter_mut().flatten() {
            m.ready = false;
            room_error(out, m.conn, rooms::ROOM_START, why);
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
        let host = room.host;
        let member_slots: Vec<usize> = room.members().map(|(i, _)| i).collect();
        let (match_id, tickets, old) = {
            let Phase::Starting { match_id, tickets, .. } = &mut room.phase else {
                return Err(msg::ROOM_NOT_STARTING);
            };
            let old = tickets.insert(slot, StartTicket { conn, addr, lan });
            (match_id.clone(), tickets.clone(), old)
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
        let players: Vec<Player> = room
            .members()
            .map(|(i, m)| {
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
                    stages: Some(self.cfg.rules.stages.clone()),
                    items: Some(self.cfg.rules.items),
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
        room.phase = Phase::InGame { since: Instant::now() };
        for m in room.slots.iter_mut().flatten() {
            m.in_game = true;
            m.ready = false;
        }
        Ok(outcome)
    }

    /// Sends every member the state again (after a ticket matched, the engine calls this).
    pub fn announce(&self, code: &str, out: &mut Vec<Output>) {
        self.broadcast(code, out);
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
                Phase::Starting { since, tickets, .. }
                    if now.saturating_duration_since(*since) >= self.cfg.start_timeout =>
                {
                    let missing = room
                        .members()
                        .find(|(i, _)| !tickets.contains_key(i))
                        .map(|(_, m)| m.info.connect_code.clone())
                        .unwrap_or_default();
                    late.push((room.code.clone(), msg::did_not_connect(&missing)));
                }
                Phase::InGame { since } if now.saturating_duration_since(*since) >= self.cfg.game_max => {
                    stale.push(room.code.clone())
                }
                _ => {}
            }
        }
        for (code, why) in late {
            self.cancel_start(&code, &why, out);
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
    use serde_json::Value;

    fn info(n: u8) -> MemberInfo {
        MemberInfo {
            uid: Uuid::from_bytes([n; 16]),
            display_name: format!("player{n}"),
            connect_code: format!("P#{n}"),
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
        let out = t.req(conn, RoomRequest::Create { public: true });
        last_state(&out, conn)["code"].as_str().unwrap().to_string()
    }

    fn join(t: &mut T, conn: ConnId, code: &str) -> Vec<Output> {
        t.req(conn, RoomRequest::Join { code: code.into() })
    }

    #[test]
    fn create_gives_a_fresh_room() {
        let mut t = T::new();
        let out = t.req(1, RoomRequest::Create { public: true });
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
        assert_eq!(s["statusText"], format!("Room {code}: waiting for players"));
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
        // Own slot, and at least two open.
        assert_eq!(msgs(&t.req(1, RoomRequest::Slot { slot: 1, open: false }), 1)[0]["error"], msg::OWN_SLOT);
        assert_eq!(msgs(&t.req(1, RoomRequest::Slot { slot: 2, open: false }), 1)[0]["error"], msg::MIN_OPEN_SLOTS);
        // Open 3 and 4: four slots, FFA.
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        let out = t.req(1, RoomRequest::Slot { slot: 4, open: true });
        let s = last_state(&out, 2);
        assert_eq!(s["mode"], "ffa");
        assert_eq!(s["statusText"], "Waiting for players (2/4)");
        join(&mut t, 3, &code);
        // Closing an occupied slot removes its player, who cannot come back.
        let out = t.req(1, RoomRequest::Slot { slot: 2, open: false });
        let left = msgs(&out, 2);
        assert_eq!(left.last().unwrap()["type"], "room-left");
        assert_eq!(left.last().unwrap()["reason"], msg::REMOVED);
        let s = last_state(&out, 1);
        assert!(s["slots"][1]["player"].is_null());
        assert_eq!(s["slots"][1]["open"], false);
        assert_eq!(t.book.room_of(2), None);
        t.req(1, RoomRequest::Slot { slot: 2, open: true });
        assert_eq!(msgs(&join(&mut t, 2, &code), 2)[0]["error"], msg::REMOVED);
        // Someone else takes the slot.
        assert_eq!(last_state(&join(&mut t, 5, &code), 5)["you"], 2);
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

    fn ready_all(t: &mut T, conns: &[ConnId]) -> Vec<Output> {
        let mut out = vec![];
        for &c in conns {
            out = t.req(c, RoomRequest::Ready { ready: true, character: Some(c as u8), costume: Some(0) });
        }
        out
    }

    #[test]
    fn start_tickets_and_game_over() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        join(&mut t, 2, &code);
        join(&mut t, 3, &code);
        let out = t.req(1, RoomRequest::Ready { ready: true, character: Some(9), costume: Some(2) });
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
        assert_eq!(msgs(&t.req(1, RoomRequest::Teams { on: true }), 1)[0]["error"], msg::BETWEEN_GAMES);
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
        assert!(t.book.ticket(102, addr(2), String::new(), info(2).uid, &code).unwrap().matched.is_empty());
        let done = t.book.ticket(103, addr(3), String::new(), info(3).uid, &code).unwrap();
        assert_eq!(done.matched.len(), 3);
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
        t.req(1, RoomRequest::Back);
        let out = t.req(2, RoomRequest::Back);
        assert_eq!(last_state(&out, 1)["status"], "waiting");
        assert_eq!(last_state(&out, 1)["statusText"], msg::waiting_on(&["player1", "player2", "player4"]));
        assert_eq!(msg::waiting_on(&["player1", "player2", "player4"]), "Waiting on: player1, player2 +1");
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
    fn teams_split_rules() {
        let mut t = T::new();
        let code = create(&mut t, 1);
        t.req(1, RoomRequest::Slot { slot: 3, open: true });
        t.req(1, RoomRequest::Teams { on: true });
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
    }

    #[test]
    fn rate_limits() {
        let mut t = T::new();
        for _ in 0..5 {
            create(&mut t, 1);
        }
        assert_eq!(msgs(&t.req(1, RoomRequest::Create { public: true }), 1)[0]["error"], msg::ROOMS_TOO_OFTEN);
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
