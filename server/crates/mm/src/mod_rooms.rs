//! Brawl Mods rooms (`common::mods`, `docs/design/brawl-mods.md` §5): in memory, like P+ rooms
//! (`rooms.rs`), with no rules, sets or start. A room is its build (every member has the same),
//! up to four members in slots, and each member's public P2P endpoint, which its session reports
//! with a probe from its own UDP socket (`mods-endpoint`, on a connection of its own) carrying the
//! token only that member got in `mods-state`. Every member gets the room's state after every
//! change; their Dolphins connect to each other from the endpoints in it.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::{Duration, Instant};

use common::mods::{
    self, Build, Controller, Endpoint, ModsPlayer, ModsRequest, ModsSlot, ModsState, PublicModRoom,
};
use common::ratelimit::{RateLimiter, Window};
use common::rooms::RoomError;
use rand::Rng;
use uuid::Uuid;

use crate::engine::{ConnId, Output};
use crate::rooms::MemberInfo;

pub const NOT_FOUND: &str = "Room not found.";
pub const ROOM_FULL: &str = "This room is full.";
pub const NOT_HOST: &str = "Only the host can do that.";
pub const NOT_IN_ROOM: &str = "You are not in a room.";
pub const KICKED: &str = "You were removed from this room.";
pub const SLOT_EMPTY: &str = "Nobody is in that slot.";
pub const SLOT_CLOSED: &str = "That slot is closed.";
pub const OWN_SLOT: &str = "You cannot do that to your own slot.";
pub const TOO_MANY: &str = "Too many requests. Wait a moment.";
pub const REMOVED: &str = "Removed from the room.";

pub fn wrong_build(build: &Build) -> String {
    let short = &build.hash[..8.min(build.hash.len())];
    if build.name.is_empty() {
        format!("You need the room's build ({short}) to join.")
    } else {
        format!("You need {} ({short}) to join.", build.name)
    }
}

#[derive(Debug, Clone)]
pub struct ModRoomsConfig {
    /// How long a kicked player cannot join the same room again.
    pub kick_ban: Duration,
    pub create_windows: Vec<Window>,
    pub join_windows: Vec<Window>,
    pub request_windows: Vec<Window>,
}

impl Default for ModRoomsConfig {
    fn default() -> Self {
        let minute = Duration::from_secs(60);
        ModRoomsConfig {
            kick_ban: Duration::from_secs(600),
            create_windows: vec![Window::new(5, minute), Window::new(30, Duration::from_secs(3600))],
            join_windows: vec![Window::new(20, minute)],
            request_windows: vec![Window::new(40, Duration::from_secs(10))],
        }
    }
}

fn endpoint_of(a: SocketAddr) -> Endpoint {
    Endpoint { host: a.ip().to_canonical().to_string(), port: a.port() }
}

#[derive(Debug, Clone)]
struct Member {
    conn: ConnId,
    info: MemberInfo,
    controller: Controller,
    endpoint: Option<SocketAddr>,
    lan: Option<SocketAddr>,
    token: String,
    joined: u64,
}

#[derive(Debug, Clone)]
struct Room {
    code: String,
    /// Rooms created before this one since mm started, plus one: the `n` of its chat group
    /// (`mods-<CODE>-<n>`), so a new room that reuses a code gets a group of its own.
    serial: u64,
    build: Build,
    public: bool,
    /// Index of the host's slot.
    host: usize,
    slots: [Option<Member>; mods::MAX_PLAYERS],
    open: [bool; mods::MAX_PLAYERS],
}

impl Room {
    fn slot_of(&self, conn: ConnId) -> Option<usize> {
        self.slots.iter().position(|m| m.as_ref().is_some_and(|m| m.conn == conn))
    }

    fn members(&self) -> impl Iterator<Item = (usize, &Member)> {
        self.slots.iter().enumerate().filter_map(|(i, m)| m.as_ref().map(|m| (i, m)))
    }

    fn free_slot(&self) -> Option<usize> {
        (0..mods::MAX_PLAYERS).find(|&i| self.open[i] && self.slots[i].is_none())
    }

    fn state_for(&self, slot: usize) -> Option<ModsState> {
        let me = self.slots[slot].as_ref()?;
        Some(ModsState {
            kind: mods::MODS_STATE.into(),
            code: self.code.clone(),
            public: self.public,
            build: self.build.clone(),
            host: self.host as u8 + 1,
            you: slot as u8 + 1,
            token: me.token.clone(),
            slots: (0..mods::MAX_PLAYERS)
                .map(|i| ModsSlot {
                    slot: i as u8 + 1,
                    open: self.open[i],
                    player: self.slots[i].as_ref().map(|m| ModsPlayer {
                        display_name: m.info.display_name.clone(),
                        connect_code: m.info.connect_code.clone(),
                        controller: m.controller,
                        endpoint: m.endpoint.map(endpoint_of),
                        lan: m.lan.map(endpoint_of),
                    }),
                })
                .collect(),
        })
    }
}

pub struct ModRoomBook {
    cfg: ModRoomsConfig,
    rooms: HashMap<String, Room>,
    member_of: HashMap<ConnId, String>,
    /// Endpoint token → (room code, member's connection).
    tokens: HashMap<String, (String, ConnId)>,
    /// (room code, account) → until when it cannot join that room.
    banned: HashMap<(String, Uuid), Instant>,
    join_seq: u64,
    room_seq: u64,
    /// Rooms whose members changed since the engine last asked ([`ModRoomBook::take_changed`]):
    /// their chat groups follow them.
    changed: Vec<(String, u64)>,
    create_limiter: RateLimiter<Uuid>,
    join_limiter: RateLimiter<Uuid>,
    request_limiter: RateLimiter<ConnId>,
}

fn send(out: &mut Vec<Output>, conn: ConnId, m: &impl serde::Serialize) {
    let json = serde_json::to_string(m).expect("mod room messages serialize");
    out.push(Output::Send { conn, json });
}

fn mods_error(out: &mut Vec<Output>, conn: ConnId, op: &str, error: &str) {
    send(out, conn, &RoomError { kind: mods::MODS_ERROR.into(), op: op.into(), error: error.into() });
}

fn new_token() -> String {
    let bytes: [u8; 16] = rand::thread_rng().gen();
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

impl ModRoomBook {
    pub fn new(cfg: ModRoomsConfig) -> Self {
        ModRoomBook {
            create_limiter: RateLimiter::new(&cfg.create_windows),
            join_limiter: RateLimiter::new(&cfg.join_windows),
            request_limiter: RateLimiter::new(&cfg.request_windows),
            cfg,
            rooms: HashMap::new(),
            member_of: HashMap::new(),
            tokens: HashMap::new(),
            banned: HashMap::new(),
            join_seq: 0,
            room_seq: 0,
            changed: Vec::new(),
        }
    }

    pub fn room_count(&self) -> usize {
        self.rooms.len()
    }

    pub fn has_code(&self, code: &str) -> bool {
        self.rooms.contains_key(code)
    }

    pub fn room_of(&self, conn: ConnId) -> Option<&str> {
        self.member_of.get(&conn).map(String::as_str)
    }

    /// Public rooms with an open, empty slot, newest first is not tracked: by code.
    pub fn public_rooms(&self, limit: usize) -> Vec<PublicModRoom> {
        let mut list: Vec<PublicModRoom> = self
            .rooms
            .values()
            .filter(|r| r.public && r.free_slot().is_some())
            .map(|r| PublicModRoom {
                code: r.code.clone(),
                build: r.build.clone(),
                host_name: r.slots[r.host].as_ref().map(|m| m.info.display_name.clone()).unwrap_or_default(),
                players: r.members().count() as u8,
                open: r.open.iter().filter(|&&o| o).count() as u8,
            })
            .collect();
        // Fuller first (a room about to fill), then by code so the list stays put.
        list.sort_by(|a, b| b.players.cmp(&a.players).then_with(|| a.code.cmp(&b.code)));
        list.truncate(limit);
        list
    }

    /// Every room, private and full ones too (for a lookup by code: never listed).
    pub fn all_rooms(&self, limit: usize) -> Vec<PublicModRoom> {
        self.rooms
            .values()
            .take(limit)
            .map(|r| PublicModRoom {
                code: r.code.clone(),
                build: r.build.clone(),
                host_name: r.slots[r.host].as_ref().map(|m| m.info.display_name.clone()).unwrap_or_default(),
                players: r.members().count() as u8,
                open: r.open.iter().filter(|&&o| o).count() as u8,
            })
            .collect()
    }

    fn broadcast(&mut self, code: &str, out: &mut Vec<Output>) {
        let Some(room) = self.rooms.get(code) else { return };
        self.changed.push((code.to_string(), room.serial));
        for (slot, m) in room.members() {
            if let Some(state) = room.state_for(slot) {
                send(out, m.conn, &state);
            }
        }
    }

    /// A request on an online connection. `taken` says whether a code is in use elsewhere (P+
    /// rooms share the codes).
    pub fn request(
        &mut self,
        now: Instant,
        conn: ConnId,
        who: &MemberInfo,
        req: ModsRequest,
        taken: &dyn Fn(&str) -> bool,
        out: &mut Vec<Output>,
    ) {
        if self.request_limiter.check(&conn, now).is_err() {
            return mods_error(out, conn, "invalid", TOO_MANY);
        }
        match req {
            ModsRequest::Create { build, public, controller } => {
                if self.create_limiter.check(&who.uid, now).is_err() {
                    return mods_error(out, conn, mods::MODS_CREATE, TOO_MANY);
                }
                self.leave(conn, out);
                let code = {
                    let mut rng = rand::thread_rng();
                    loop {
                        let c = common::rooms::random_room_code(&mut rng);
                        if !self.rooms.contains_key(&c) && !taken(&c) {
                            break c;
                        }
                    }
                };
                let member = self.new_member(conn, who, controller, &code);
                let mut slots: [Option<Member>; mods::MAX_PLAYERS] = Default::default();
                slots[0] = Some(member);
                self.room_seq += 1;
                let serial = self.room_seq;
                self.rooms.insert(
                    code.clone(),
                    Room { code: code.clone(), serial, build, public, host: 0, slots, open: [true; mods::MAX_PLAYERS] },
                );
                self.member_of.insert(conn, code.clone());
                tracing::info!(conn, %code, "mod room created");
                self.broadcast(&code, out);
            }
            ModsRequest::Join { code, build_hash, controller, slot: prefer } => {
                if self.join_limiter.check(&who.uid, now).is_err() {
                    return mods_error(out, conn, mods::MODS_JOIN, TOO_MANY);
                }
                let Some(code) = common::rooms::parse_room_code(&code) else {
                    return mods_error(out, conn, mods::MODS_JOIN, NOT_FOUND);
                };
                if self.member_of.get(&conn) == Some(&code) {
                    return self.broadcast(&code, out);
                }
                let Some(room) = self.rooms.get(&code) else {
                    return mods_error(out, conn, mods::MODS_JOIN, NOT_FOUND);
                };
                if room.build.hash != build_hash {
                    return mods_error(out, conn, mods::MODS_JOIN, &wrong_build(&room.build));
                }
                if let Some(&until) = self.banned.get(&(code.clone(), who.uid)).filter(|&&until| until > now) {
                    // When they can come back, in whole minutes (rounded up).
                    let mins = until.duration_since(now).as_secs().div_ceil(60).max(1);
                    let unit = if mins == 1 { "minute" } else { "minutes" };
                    let why = format!("{KICKED} You can join it again in {mins} {unit}.");
                    return mods_error(out, conn, mods::MODS_JOIN, &why);
                }
                let preferred = prefer.map(|s| s as usize - 1).filter(|&i| room.open[i] && room.slots[i].is_none());
                let Some(slot) = preferred.or_else(|| room.free_slot()) else {
                    return mods_error(out, conn, mods::MODS_JOIN, ROOM_FULL);
                };
                self.leave(conn, out);
                let member = self.new_member(conn, who, controller, &code);
                let room = self.rooms.get_mut(&code).expect("checked above");
                room.slots[slot] = Some(member);
                self.member_of.insert(conn, code.clone());
                self.broadcast(&code, out);
            }
            ModsRequest::Leave => {
                if self.member_of.contains_key(&conn) {
                    self.leave(conn, out);
                    send(out, conn, &serde_json::json!({"type": mods::MODS_LEFT}));
                } else {
                    mods_error(out, conn, mods::MODS_LEAVE, NOT_IN_ROOM);
                }
            }
            ModsRequest::Slot { slot, open } => {
                let Some((code, idx)) = self.host_request(conn, mods::MODS_SLOT, out) else { return };
                let target = slot as usize - 1;
                if target == idx {
                    return mods_error(out, conn, mods::MODS_SLOT, OWN_SLOT);
                }
                let room = self.rooms.get_mut(&code).expect("host_request found it");
                room.open[target] = open;
                if !open {
                    if let Some(m) = room.slots[target].take() {
                        self.remove_member(&code, m, REMOVED, out);
                    }
                }
                self.broadcast(&code, out);
            }
            ModsRequest::Kick { slot } => {
                let Some((code, idx)) = self.host_request(conn, mods::MODS_KICK, out) else { return };
                let target = slot as usize - 1;
                if target == idx {
                    return mods_error(out, conn, mods::MODS_KICK, OWN_SLOT);
                }
                let room = self.rooms.get_mut(&code).expect("host_request found it");
                let Some(m) = room.slots[target].take() else {
                    return mods_error(out, conn, mods::MODS_KICK, SLOT_EMPTY);
                };
                self.banned.insert((code.clone(), m.info.uid), now + self.cfg.kick_ban);
                self.remove_member(&code, m, REMOVED, out);
                self.broadcast(&code, out);
            }
            ModsRequest::Public { public } => {
                let Some((code, _)) = self.host_request(conn, mods::MODS_PUBLIC, out) else { return };
                self.rooms.get_mut(&code).expect("host_request found it").public = public;
                self.broadcast(&code, out);
            }
            ModsRequest::Move { from, to } => {
                let Some((code, _)) = self.host_request(conn, mods::MODS_MOVE, out) else { return };
                let room = self.rooms.get_mut(&code).expect("host_request found it");
                let (a, b) = (from as usize - 1, to as usize - 1);
                if room.slots[a].is_none() && room.slots[b].is_none() {
                    return mods_error(out, conn, mods::MODS_MOVE, SLOT_EMPTY);
                }
                // Any slot, a closed one too: the two swap whether they are open with their
                // players, so a player moved into a closed slot leaves its old one closed (as
                // many slots stay open).
                room.slots.swap(a, b);
                room.open.swap(a, b);
                if room.host == a {
                    room.host = b;
                } else if room.host == b {
                    room.host = a;
                }
                self.broadcast(&code, out);
            }
            ModsRequest::Controller { controller } => {
                let Some(code) = self.member_of.get(&conn).cloned() else {
                    return mods_error(out, conn, mods::MODS_CONTROLLER, NOT_IN_ROOM);
                };
                let Some(room) = self.rooms.get_mut(&code) else { return };
                let Some(idx) = room.slot_of(conn) else { return };
                let Some(m) = room.slots[idx].as_mut() else { return };
                if m.controller != controller {
                    m.controller = controller;
                    self.broadcast(&code, out);
                }
            }
        }
    }

    fn new_member(&mut self, conn: ConnId, who: &MemberInfo, controller: Controller, code: &str) -> Member {
        let token = new_token();
        self.tokens.insert(token.clone(), (code.to_string(), conn));
        self.join_seq += 1;
        Member { conn, info: who.clone(), controller, endpoint: None, lan: None, token, joined: self.join_seq }
    }

    /// The host's room and slot, or an error to the sender.
    fn host_request(&self, conn: ConnId, op: &str, out: &mut Vec<Output>) -> Option<(String, usize)> {
        let Some(code) = self.member_of.get(&conn) else {
            mods_error(out, conn, op, NOT_IN_ROOM);
            return None;
        };
        let room = self.rooms.get(code)?;
        let idx = room.slot_of(conn)?;
        if idx != room.host {
            mods_error(out, conn, op, NOT_HOST);
            return None;
        }
        Some((code.clone(), idx))
    }

    /// A member taken out by the host (kick, closed slot): told, and forgotten.
    fn remove_member(&mut self, code: &str, m: Member, why: &str, out: &mut Vec<Output>) {
        self.member_of.remove(&m.conn);
        self.tokens.remove(&m.token);
        send(out, m.conn, &serde_json::json!({"type": mods::MODS_LEFT, "reason": why}));
        tracing::info!(conn = m.conn, %code, "removed from mod room");
    }

    /// The connection leaves its room (if any): the earliest other member becomes host; an empty
    /// room closes.
    fn leave(&mut self, conn: ConnId, out: &mut Vec<Output>) {
        let Some(code) = self.member_of.remove(&conn) else { return };
        let Some(room) = self.rooms.get_mut(&code) else { return };
        let Some(idx) = room.slot_of(conn) else { return };
        if let Some(m) = room.slots[idx].take() {
            self.tokens.remove(&m.token);
        }
        if room.members().next().is_none() {
            self.changed.push((code.clone(), room.serial));
            self.rooms.remove(&code);
            self.banned.retain(|(c, _), _| c != &code);
            tracing::info!(%code, "mod room closed");
            return;
        }
        if room.host == idx {
            room.host = room.members().min_by_key(|(_, m)| m.joined).map(|(i, _)| i).expect("not empty");
        }
        self.broadcast(&code, out);
    }

    /// A probe from a member's P2P socket: records where it came from (and the LAN address it
    /// reported). False for an unknown token.
    pub fn endpoint(&mut self, token: &str, addr: SocketAddr, lan: Option<SocketAddr>, out: &mut Vec<Output>) -> bool {
        let Some((code, conn)) = self.tokens.get(token).cloned() else { return false };
        let Some(room) = self.rooms.get_mut(&code) else { return false };
        let Some(idx) = room.slot_of(conn) else { return false };
        let member = room.slots[idx].as_mut().expect("slot_of found it");
        if member.endpoint == Some(addr) && member.lan == lan {
            return true;
        }
        member.endpoint = Some(addr);
        member.lan = lan;
        self.broadcast(&code, out);
        true
    }

    pub fn conn_gone(&mut self, conn: ConnId, out: &mut Vec<Output>) {
        self.leave(conn, out);
    }

    /// The rooms whose members changed since the last call (each once), for their chat groups.
    pub fn take_changed(&mut self) -> Vec<(String, u64)> {
        let mut changed = std::mem::take(&mut self.changed);
        let mut seen = std::collections::HashSet::new();
        changed.retain(|c| seen.insert(c.clone()));
        changed
    }

    /// The online connections of a room's members in joining order, if the room with this code
    /// is still the one with this serial (none once it closed).
    pub fn players(&self, code: &str, serial: u64) -> Vec<ConnId> {
        let Some(room) = self.rooms.get(code).filter(|r| r.serial == serial) else { return Vec::new() };
        let mut members: Vec<&Member> = room.members().map(|(_, m)| m).collect();
        members.sort_by_key(|m| m.joined);
        members.into_iter().map(|m| m.conn).collect()
    }

    pub fn tick(&mut self, now: Instant) {
        self.banned.retain(|_, until| *until > now);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::{json, Value};

    const HASH: &str = "3f9a1c07aa00000000000000000000000000000000000000000000000000beef";

    fn who(n: u8) -> MemberInfo {
        MemberInfo {
            uid: Uuid::from_bytes([n; 16]),
            display_name: format!("player{n}"),
            connect_code: format!("P{n}#1"),
        }
    }

    fn build() -> Build {
        Build { hash: HASH.into(), name: "Brawl Minus 5.1".into() }
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
        msgs(out, conn).into_iter().rev().find(|m| m["type"] == "mods-state").expect("mods-state")
    }

    fn req(b: &mut ModRoomBook, conn: ConnId, n: u8, r: ModsRequest) -> Vec<Output> {
        let mut out = vec![];
        b.request(Instant::now(), conn, &who(n), r, &|_| false, &mut out);
        out
    }

    fn create(b: &mut ModRoomBook, conn: ConnId) -> String {
        let out = req(b, conn, conn as u8, ModsRequest::Create { build: build(), public: true, controller: Controller::Gc });
        last_state(&out, conn)["code"].as_str().unwrap().to_string()
    }

    fn join(b: &mut ModRoomBook, conn: ConnId, code: &str) -> Vec<Output> {
        req(b, conn, conn as u8, ModsRequest::Join { code: code.into(), build_hash: HASH.into(), controller: Controller::Wiimote, slot: None })
    }

    #[test]
    fn create_join_and_state() {
        let mut b = ModRoomBook::new(ModRoomsConfig::default());
        let code = create(&mut b, 1);
        let out = join(&mut b, 2, &code.to_lowercase());
        let s1 = last_state(&out, 1);
        let s2 = last_state(&out, 2);
        assert_eq!(s1["you"], 1);
        assert_eq!(s2["you"], 2);
        assert_eq!(s1["host"], 1);
        assert_ne!(s1["token"], s2["token"]);
        assert_eq!(s2["slots"][1]["player"]["controller"], "wiimote");
        assert_eq!(s2["slots"][0]["player"]["endpoint"], Value::Null);
        assert_eq!(b.public_rooms(10).len(), 1);
    }

    #[test]
    fn a_member_back_gets_its_slot_if_free() {
        let mut b = ModRoomBook::new(ModRoomsConfig::default());
        let code = create(&mut b, 1);
        join(&mut b, 2, &code);
        join(&mut b, 3, &code);
        // Player 2's connection to mm drops; it comes back asking for slot 2 (its session's).
        let _ = req(&mut b, 2, 2, ModsRequest::Leave);
        let out = req(&mut b, 4, 2, ModsRequest::Join { code: code.clone(), build_hash: HASH.into(), controller: Controller::Gc, slot: Some(2) });
        assert_eq!(last_state(&out, 4)["you"], 2);
        // A slot that is taken: the lowest free one instead.
        let out = req(&mut b, 5, 5, ModsRequest::Join { code, build_hash: HASH.into(), controller: Controller::Gc, slot: Some(3) });
        assert_eq!(last_state(&out, 5)["you"], 4);
    }

    #[test]
    fn a_different_build_cannot_join() {
        let mut b = ModRoomBook::new(ModRoomsConfig::default());
        let code = create(&mut b, 1);
        let out = req(&mut b, 2, 2, ModsRequest::Join { code, build_hash: "0".repeat(64), controller: Controller::Gc, slot: None });
        let m = &msgs(&out, 2)[0];
        assert_eq!(m["type"], "mods-error");
        assert!(m["error"].as_str().unwrap().contains("Brawl Minus 5.1"));
    }

    #[test]
    fn endpoints_reach_everyone_and_tokens_are_private() {
        let mut b = ModRoomBook::new(ModRoomsConfig::default());
        let code = create(&mut b, 1);
        let out = join(&mut b, 2, &code);
        let token2 = last_state(&out, 2)["token"].as_str().unwrap().to_string();
        let mut out = vec![];
        assert!(b.endpoint(&token2, "198.51.100.7:40004".parse().unwrap(), Some("192.168.1.20:40004".parse().unwrap()), &mut out));
        let s1 = last_state(&out, 1);
        assert_eq!(s1["slots"][1]["player"]["endpoint"], json!({"host": "198.51.100.7", "port": 40004}));
        assert_eq!(s1["slots"][1]["player"]["lan"], json!({"host": "192.168.1.20", "port": 40004}));
        assert!(!s1.to_string().contains(&token2));
        assert!(!b.endpoint("nope", "198.51.100.7:1".parse().unwrap(), None, &mut vec![]));
    }

    #[test]
    fn host_kicks_closes_slots_and_hands_over() {
        let mut b = ModRoomBook::new(ModRoomsConfig::default());
        let code = create(&mut b, 1);
        join(&mut b, 2, &code);
        join(&mut b, 3, &code);
        // Only the host kicks.
        let out = req(&mut b, 2, 2, ModsRequest::Kick { slot: 3 });
        assert_eq!(msgs(&out, 2)[0]["error"], NOT_HOST);
        let out = req(&mut b, 1, 1, ModsRequest::Kick { slot: 3 });
        assert_eq!(msgs(&out, 3)[0]["type"], "mods-left");
        assert_eq!(last_state(&out, 1)["slots"][2]["player"], Value::Null);
        // A kicked player cannot come back for a while.
        let out = join(&mut b, 3, &code);
        assert_eq!(msgs(&out, 3)[0]["error"], format!("{KICKED} You can join it again in 10 minutes."));
        // Closing an occupied slot removes its player.
        let out = req(&mut b, 1, 1, ModsRequest::Slot { slot: 2, open: false });
        assert_eq!(msgs(&out, 2)[0]["type"], "mods-left");
        // The host leaves: the earliest member left takes over; the room closes when empty.
        join(&mut b, 4, &code);
        let mut out = vec![];
        b.conn_gone(1, &mut out);
        assert_eq!(last_state(&out, 4)["host"], 3);
        b.conn_gone(4, &mut vec![]);
        assert_eq!(b.room_count(), 0);
    }

    #[test]
    fn full_rooms_and_private_rooms() {
        let mut b = ModRoomBook::new(ModRoomsConfig::default());
        let code = create(&mut b, 1);
        for c in 2..=4 {
            join(&mut b, c, &code);
        }
        let out = join(&mut b, 5, &code);
        assert_eq!(msgs(&out, 5)[0]["error"], ROOM_FULL);
        assert!(b.public_rooms(10).is_empty());
        b.conn_gone(4, &mut vec![]);
        assert_eq!(b.public_rooms(10).len(), 1);
        req(&mut b, 1, 1, ModsRequest::Public { public: false });
        assert!(b.public_rooms(10).is_empty());
    }

    #[test]
    fn the_host_moves_players() {
        let mut b = ModRoomBook::new(ModRoomsConfig::default());
        let code = create(&mut b, 1);
        join(&mut b, 2, &code);
        // Only the host.
        let out = req(&mut b, 2, 2, ModsRequest::Move { from: 2, to: 3 });
        assert_eq!(msgs(&out, 2)[0]["error"], NOT_HOST);
        // A player into an open, empty slot.
        let out = req(&mut b, 1, 1, ModsRequest::Move { from: 2, to: 3 });
        let st = last_state(&out, 2);
        assert!(st["slots"][1]["player"].is_null());
        assert_eq!(st["slots"][2]["player"]["controller"], "wiimote");
        assert_eq!(st["you"], 3);
        // The host swaps itself with that player: still the host, in its new slot.
        let out = req(&mut b, 1, 1, ModsRequest::Move { from: 1, to: 3 });
        let st = last_state(&out, 1);
        assert_eq!(st["you"], 3);
        assert_eq!(st["host"], 3);
        // Two empty slots move nothing.
        let out = req(&mut b, 1, 1, ModsRequest::Move { from: 2, to: 4 });
        assert_eq!(msgs(&out, 1)[0]["error"], SLOT_EMPTY);
        // A closed slot takes a player too, and the slot it left is the closed one now.
        req(&mut b, 1, 1, ModsRequest::Slot { slot: 4, open: false });
        let out = req(&mut b, 1, 1, ModsRequest::Move { from: 3, to: 4 });
        let st = last_state(&out, 1);
        assert_eq!(st["you"], 4);
        assert_eq!(st["host"], 4);
        assert_eq!(st["slots"][3]["open"], true);
        assert_eq!(st["slots"][2]["open"], false);
    }

    #[test]
    fn a_member_switches_controller() {
        let mut b = ModRoomBook::new(ModRoomsConfig::default());
        let code = create(&mut b, 1);
        join(&mut b, 2, &code);
        let out = req(&mut b, 2, 2, ModsRequest::Controller { controller: Controller::Gc });
        let s1 = msgs(&out, 1);
        assert_eq!(s1.last().unwrap()["slots"][1]["player"]["controller"], "gc");
        // The same controller again: nothing to tell.
        assert!(req(&mut b, 2, 2, ModsRequest::Controller { controller: Controller::Gc }).is_empty());
        let out = req(&mut b, 9, 9, ModsRequest::Controller { controller: Controller::Gc });
        assert_eq!(msgs(&out, 9)[0]["error"], NOT_IN_ROOM);
    }
}
