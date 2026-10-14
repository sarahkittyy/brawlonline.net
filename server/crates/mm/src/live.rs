//! Spectating: games' uploads, their roles, and the live store (`docs/spectate-protocol.md` §1-2,
//! design in `docs/design/spectating.md`).
//!
//! A player's game offers each game it plays (`spectate-offer`) when the player lets others
//! stream their games. Once every player of the match offered, or [`LiveConfig::decide_after`]
//! after the first offer, mm makes one of them the **primary** (it sends the replay's chunks) and
//! the others **witnesses** (they send digests of the same `FRMS` chunks). A primary that pauses,
//! stops, stalls, sends something wrong or goes away hands the game to a witness ("takeover").
//!
//! Every chunk is checked before it is kept ([`common::spectate`]): header, CRC, sizes, order,
//! contiguous frames, `INFO`'s match and players, a budget and a rate per game. Kept chunks go to
//! the [`LiveStore`], which the loopback listener reads for accounts ([`crate::server`]); nothing
//! is written to disk.
//!
//! A **stream** is a match: a Direct, Unranked or Ranked set, or a room's games while the same
//! players play in the same room. [`MM_SPECTATE`](crate::config::Config::spectate) is the kill
//! switch: `off` (the default) answers every offer with `none`.

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::str::FromStr;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use chrono::{DateTime, SecondsFormat, Utc};
use common::spectate::{
    self, ChunkKind, GameState, LiveEntry, LiveGame, LiveLine, LivePlayer, Offer, Role, RoleMsg, SpectateErrorMsg,
    SpectateRequest, SPECTATE_ERROR, SPECTATE_ROLE,
};
use tokio::sync::watch;
use uuid::Uuid;

use crate::engine::{ConnId, MatchRecord, Output};

/// Answers to a refused `spectate-*` request (`spectate-error`). The game logs them.
pub mod errors {
    pub const NOT_YOUR_MATCH: &str = "That match isn't yours to stream.";
    pub const TOO_MANY_GAMES: &str = "Too many games in this match.";
    pub const NOT_YOUR_SLOT: &str = "That upload isn't yours.";
    pub const INVALID: &str = "Invalid spectate message.";
    pub const UPDATE: &str = "Your game needs an update.";
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SpectateMode {
    /// Every offer is answered with `none`: nothing is uploaded.
    Off,
    /// Everyone is a witness: only digests flow, nothing is listed.
    Witness,
    On,
}

impl FromStr for SpectateMode {
    type Err = String;
    fn from_str(s: &str) -> Result<Self, String> {
        match s.trim().to_ascii_lowercase().as_str() {
            "" | "off" => Ok(SpectateMode::Off),
            "witness" => Ok(SpectateMode::Witness),
            "on" => Ok(SpectateMode::On),
            other => Err(format!("MM_SPECTATE must be off, witness or on, not {other:?}")),
        }
    }
}

#[derive(Debug, Clone)]
pub struct LiveConfig {
    pub mode: SpectateMode,
    /// Roles are given once every player offered, or this long after the first offer.
    pub decide_after: Duration,
    /// The pacer rate told to primaries, bytes per second.
    pub rate: u32,
    /// What mm takes from one game: a token bucket (bytes per second, burst).
    pub bucket_rate: f64,
    pub bucket_burst: f64,
    /// The most a game may upload.
    pub game_budget: usize,
    /// Offers are taken for matches made within this long.
    pub match_ttl: Duration,
    /// A stream is over this long after its last game ended with no new game offered ...
    pub between_games: Duration,
    /// ... or when nothing of it was uploaded for this long.
    pub idle: Duration,
    /// An over stream stays readable this long.
    pub keep: Duration,
    /// A primary that sent no frames this long, while a witness shows the game going on, is
    /// replaced.
    pub stall: Duration,
    pub max_games_per_match: usize,
}

impl Default for LiveConfig {
    fn default() -> Self {
        LiveConfig {
            mode: SpectateMode::Off,
            decide_after: Duration::from_secs(2),
            rate: 2048,
            bucket_rate: 4096.0,
            bucket_burst: 65536.0,
            game_budget: 2 << 20,
            match_ttl: Duration::from_secs(6 * 3600),
            between_games: Duration::from_secs(120),
            idle: Duration::from_secs(600),
            keep: Duration::from_secs(600),
            stall: Duration::from_secs(5),
            max_games_per_match: 64,
        }
    }
}

// ---------------------------------------------------------------- the store (shared with HTTP)

/// One game of a stream as readers see it.
#[derive(Debug, Clone, Default)]
pub struct GameView {
    pub number: u32,
    pub uid: String,
    pub chunks: Vec<Arc<[u8]>>,
    pub ended: bool,
    pub bad: Option<String>,
}

struct StreamSlot {
    entry: LiveEntry,
    games: Vec<GameView>,
    over: bool,
    tx: watch::Sender<u64>,
}

/// The kept chunks of every stream, written by the engine thread and read by the loopback
/// listener's tasks. Each stream has a version that readers wait on ([`LiveStore::subscribe`]).
#[derive(Default)]
pub struct LiveStore {
    streams: Mutex<HashMap<String, StreamSlot>>,
}

/// Where a reader of [`LiveStore::read`] is: the game (0-based) and the next chunk of it, and
/// whether its `game` line went out.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Cursor {
    pub game: usize,
    pub chunk: usize,
    pub announced: bool,
}

impl LiveStore {
    fn with<R>(&self, f: impl FnOnce(&mut HashMap<String, StreamSlot>) -> R) -> R {
        let mut g = self.streams.lock().unwrap_or_else(|e| e.into_inner());
        f(&mut g)
    }

    fn change(&self, id: &str, f: impl FnOnce(&mut StreamSlot)) {
        self.with(|m| {
            if let Some(s) = m.get_mut(id) {
                f(s);
                s.tx.send_modify(|v| *v += 1);
            }
        })
    }

    /// The listed streams, newest first. `Witness` streams are never in the store.
    pub fn entries(&self) -> Vec<LiveEntry> {
        let mut v: Vec<LiveEntry> = self.with(|m| m.values().map(|s| s.entry.clone()).collect());
        v.sort_by(|a, b| b.started_at.cmp(&a.started_at).then_with(|| a.id.cmp(&b.id)));
        v
    }

    pub fn entry(&self, id: &str) -> Option<LiveEntry> {
        self.with(|m| m.get(id).map(|s| s.entry.clone()))
    }

    pub fn subscribe(&self, id: &str) -> Option<watch::Receiver<u64>> {
        self.with(|m| m.get(id).map(|s| s.tx.subscribe()))
    }

    /// The lines from `cursor` on, and whether the stream is over and fully read (the reader
    /// stops then). `None`: no such stream.
    pub fn read(&self, id: &str, cursor: &mut Cursor) -> Option<(Vec<LiveLine>, bool)> {
        use base64::Engine as _;
        self.with(|m| {
            let s = m.get(id)?;
            let mut lines = Vec::new();
            while let Some(g) = s.games.get(cursor.game) {
                let ordinal = cursor.game as u32 + 1;
                if !cursor.announced {
                    lines.push(LiveLine::Game { game: ordinal, number: g.number, uid: g.uid.clone() });
                    cursor.announced = true;
                }
                while let Some(c) = g.chunks.get(cursor.chunk) {
                    let b = base64::engine::general_purpose::STANDARD.encode(c);
                    lines.push(LiveLine::Chunk { game: ordinal, i: cursor.chunk as u32, b });
                    cursor.chunk += 1;
                }
                if let Some(e) = &g.bad {
                    lines.push(LiveLine::Bad { game: ordinal, error: e.clone() });
                } else if g.ended {
                    lines.push(LiveLine::End { game: ordinal });
                } else {
                    // Still being uploaded.
                    return Some((lines, false));
                }
                *cursor = Cursor { game: cursor.game + 1, chunk: 0, announced: false };
            }
            if s.over {
                lines.push(LiveLine::Over);
            }
            Some((lines, s.over))
        })
    }

    fn create(&self, id: &str, entry: LiveEntry) {
        self.with(|m| {
            m.entry(id.to_string()).or_insert_with(|| StreamSlot {
                entry,
                games: Vec::new(),
                over: false,
                tx: watch::channel(0).0,
            });
        })
    }

    fn remove(&self, id: &str) {
        self.with(|m| {
            if let Some(s) = m.remove(id) {
                // Wakes readers, who then find the stream gone and stop.
                s.tx.send_modify(|v| *v += 1);
            }
        })
    }
}

// ---------------------------------------------------------------- bookkeeping (engine thread)

#[derive(Debug, Clone)]
struct MatchInfo {
    mode: &'static str,
    room: Option<String>,
    players: Vec<Uuid>,
    host: Uuid,
    names: HashMap<Uuid, (String, String)>,
    made: Instant,
    stream: String,
}

#[derive(Debug, Clone)]
struct Offerer {
    conn: ConnId,
    account: Uuid,
    paused: u32,
    rollbacks: f64,
    host: bool,
    /// The current slot, if it has a role.
    slot: Option<u32>,
    /// Paused and replaced: sends nothing until mm gives it a role again.
    standby: bool,
    /// Sent `spectate-resume` while on standby: can take the game back.
    resumable: bool,
    /// Stopped, failed or gone: never chosen again for this game.
    out: bool,
}

#[derive(Debug, Clone)]
struct Slot {
    conn: ConnId,
    game: [u8; 16],
    role: Role,
    /// The chunk being put together: its seq and bytes so far.
    partial: Option<(u32, u32, Vec<u8>)>,
    next_seq: u32,
    /// A takeover's first frame.
    from: Option<u32>,
}

#[derive(Debug)]
struct Game {
    match_id: String,
    stream: String,
    number: u32,
    uid: [u8; 16],
    offers: Vec<Offerer>,
    first_offer: Instant,
    decided: bool,
    primary: Option<u32>,
    /// When the current primary got its role, or last sent frames.
    progress: Instant,
    /// The game's index in its stream's store, once its first chunk is kept.
    ordinal: Option<usize>,
    have: Vec<ChunkKind>,
    next_frame: Option<u32>,
    /// `first_frame` → digest of every kept `FRMS`.
    hashes: BTreeMap<u32, [u8; 8]>,
    /// Witness digests for chunks not kept yet.
    pending: VecDeque<(u32, [u8; 8])>,
    /// The newest `first_frame` a witness reported.
    witness_frame: Option<u32>,
    bytes: usize,
    tokens: f64,
    refilled: Instant,
    ended: bool,
    bad: bool,
    finished_at: Option<Instant>,
}

impl Game {
    fn open(&self) -> bool {
        !self.ended && !self.bad
    }

    fn offerer_of_slot(&mut self, slot: u32) -> Option<&mut Offerer> {
        self.offers.iter_mut().find(|o| o.slot == Some(slot))
    }

    fn headers_done(&self) -> bool {
        self.have.contains(&ChunkKind::Info) && self.have.contains(&ChunkKind::Setp)
    }
}

#[derive(Debug)]
struct StreamBook {
    /// The room's code and sorted players, for linking a room's next game.
    room_key: Option<(String, Vec<Uuid>)>,
    players: Vec<Uuid>,
    /// Set when the stream is in the store (its first `INFO` was kept).
    listed: bool,
    last_upload: Instant,
    last_end: Option<Instant>,
    /// The set can't go on (a game ended by a disconnect, a player leaving or an error; a Ranked
    /// quit; a Ranked set won): the stream is over at the next tick, not `between_games` later.
    set_over: bool,
    over_at: Option<Instant>,
    wins: HashMap<Uuid, u32>,
    started_at: Option<String>,
}

impl StreamBook {
    fn new(now: Instant, room_key: Option<(String, Vec<Uuid>)>, players: Vec<Uuid>) -> StreamBook {
        StreamBook {
            room_key,
            players,
            listed: false,
            last_upload: now,
            last_end: None,
            set_over: false,
            over_at: None,
            wins: HashMap::new(),
            started_at: None,
        }
    }
}

/// Whether a game that ended with `method` (ENDG's `endMethod`, `GameplaySession.cpp` EndReplay)
/// ends its stream: nothing can follow a disconnect, a player leaving the match or an error; in
/// Ranked the first to `WINS_NEEDED` wins the set. A quit never ends a set (in Ranked it loses
/// that game, docs/game-code.md "The quit per mode"; in a room's set the game didn't happen), a
/// desync restarts the game, and runbacks play on, as does a room's next game after its set.
fn ends_stream(mode: &str, method: &str, best_wins: u32) -> bool {
    matches!(method, "disconnect" | "stopped" | "error")
        || (mode == "ranked" && best_wins >= common::ranked::WINS_NEEDED)
}

pub struct LiveBook {
    cfg: LiveConfig,
    store: Arc<LiveStore>,
    matches: HashMap<String, MatchInfo>,
    games: HashMap<[u8; 16], Game>,
    slots: HashMap<u32, Slot>,
    streams: HashMap<String, StreamBook>,
    next_slot: u32,
}

fn send(out: &mut Vec<Output>, conn: ConnId, m: &impl serde::Serialize) {
    out.push(Output::Send { conn, json: serde_json::to_string(m).expect("protocol messages serialize") });
}

fn error(out: &mut Vec<Output>, conn: ConnId, op: &str, error: &str) {
    send(out, conn, &SpectateErrorMsg { kind: SPECTATE_ERROR.into(), op: op.into(), error: error.into() });
}

fn rfc3339(t: DateTime<Utc>) -> String {
    t.to_rfc3339_opts(SecondsFormat::Millis, true)
}

fn new_stream_id() -> String {
    hex::encode(rand::random::<[u8; 6]>())
}

impl LiveBook {
    pub fn new(cfg: LiveConfig) -> Self {
        LiveBook {
            cfg,
            store: Arc::new(LiveStore::default()),
            matches: HashMap::new(),
            games: HashMap::new(),
            slots: HashMap::new(),
            streams: HashMap::new(),
            next_slot: 1,
        }
    }

    pub fn store(&self) -> Arc<LiveStore> {
        self.store.clone()
    }

    pub fn mode(&self) -> SpectateMode {
        self.cfg.mode
    }

    pub fn stream_count(&self) -> usize {
        self.streams.len()
    }

    /// A match mm made (a pairing or a room's start). `names`: each player's name and connect code
    /// as mm knows them (from their online connection), which the list uses instead of what an
    /// upload says.
    pub fn register_match(&mut self, now: Instant, m: &MatchRecord, names: HashMap<Uuid, (String, String)>) {
        let mut sorted = m.players.clone();
        sorted.sort_unstable();
        let room_key = m.room.as_ref().map(|r| (r.code.clone(), sorted.clone()));
        // A room's next game with the same players goes on in the same stream.
        let stream = room_key
            .as_ref()
            .and_then(|k| {
                self.streams
                    .iter()
                    .find(|(_, s)| s.over_at.is_none() && s.room_key.as_ref() == Some(k))
                    .map(|(id, _)| id.clone())
            })
            .unwrap_or_else(|| {
                let id = new_stream_id();
                self.streams.insert(id.clone(), StreamBook::new(now, room_key, m.players.clone()));
                id
            });
        let mode = if m.room.is_some() { "room" } else { m.mode.name() };
        self.matches.insert(
            m.match_id.clone(),
            MatchInfo {
                mode,
                room: m.room.as_ref().map(|r| r.code.clone()),
                players: m.players.clone(),
                host: m.host,
                names,
                made: now,
                stream,
            },
        );
    }

    /// `uid` left the chat group `group` (a match group's id is its match id; a room's is
    /// `room-<CODE>-<n>`): they left the match or the room (their game's online session ended, a
    /// new search, another room, their connection dropped). Its stream can't go on with the same
    /// players: it is over once no game of it is going on (a game under way still ends, or is cut,
    /// first). Players without in-game chat never join a group; for them the 120 s wait stays.
    pub fn player_left(&mut self, is_room: bool, group: &str, uid: Uuid) {
        let streams: Vec<String> = if is_room {
            let Some(code) = group.strip_prefix("room-").and_then(|r| r.rsplit_once('-')).map(|(c, _)| c) else {
                return;
            };
            self.streams
                .iter()
                .filter(|(_, b)| b.room_key.as_ref().is_some_and(|(c, p)| c == code && p.contains(&uid)))
                .map(|(id, _)| id.clone())
                .collect()
        } else {
            self.matches.get(group).filter(|m| m.players.contains(&uid)).map(|m| m.stream.clone()).into_iter().collect()
        };
        for id in streams {
            if let Some(b) = self.streams.get_mut(&id).filter(|b| b.over_at.is_none() && !b.set_over) {
                tracing::info!(stream = %id, group, %uid, "spectate: a player left; the stream ends after its game");
                b.set_over = true;
            }
        }
    }

    /// A new stream for `match_id`'s next games (its own is over), with the same players and
    /// room.
    fn fresh_stream(&mut self, now: Instant, match_id: &str) -> String {
        let id = new_stream_id();
        let (players, room_key) = match self.matches.get(match_id) {
            Some(m) => (m.players.clone(), self.streams.get(&m.stream).and_then(|b| b.room_key.clone())),
            None => (Vec::new(), None),
        };
        self.streams.insert(id.clone(), StreamBook::new(now, room_key, players));
        if let Some(m) = self.matches.get_mut(match_id) {
            m.stream = id.clone();
        }
        tracing::info!(match_id, stream = %id, "spectate: the match's stream was over; its next game starts a new one");
        id
    }

    fn next_slot(&mut self) -> u32 {
        let s = self.next_slot;
        self.next_slot = self.next_slot.wrapping_add(1).max(1);
        s
    }

    fn role_msg(&self, game: &Game, role: Role, slot: u32, from: Option<u32>, headers: bool) -> RoleMsg {
        RoleMsg {
            kind: SPECTATE_ROLE.into(),
            game_uid: hex::encode(game.uid),
            role,
            slot,
            from,
            headers,
            rate: self.cfg.rate,
        }
    }

    fn send_none(&self, out: &mut Vec<Output>, conn: ConnId, uid: &[u8; 16]) {
        send(
            out,
            conn,
            &RoleMsg {
                kind: SPECTATE_ROLE.into(),
                game_uid: hex::encode(uid),
                role: Role::None,
                slot: 0,
                from: None,
                headers: false,
                rate: 0,
            },
        );
    }

    /// A `spectate-*` request from an online connection (`account`).
    pub fn request(&mut self, now: Instant, conn: ConnId, account: Uuid, req: SpectateRequest, out: &mut Vec<Output>) {
        let op = req.op();
        let result = match req {
            SpectateRequest::Offer(o) => self.offer(now, conn, account, o, out),
            SpectateRequest::Digest { slot, frms } => self.digest(now, conn, slot, frms, out),
            SpectateRequest::Pause { slot, reason } => self.pause(now, conn, slot, &reason, false, out),
            SpectateRequest::Stop { slot, reason } => self.pause(now, conn, slot, &reason, true, out),
            SpectateRequest::Resume { slot } => self.resume(conn, slot),
        };
        if let Err(e) = result {
            error(out, conn, op, e);
        }
    }

    fn offer(
        &mut self,
        now: Instant,
        conn: ConnId,
        account: Uuid,
        o: Offer,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        let uid = spectate::decode_uid(&o.game_uid).ok_or(errors::INVALID)?;
        let Some(m) = self.matches.get(&o.match_id).filter(|m| now.duration_since(m.made) < self.cfg.match_ttl) else {
            return Err(errors::NOT_YOUR_MATCH);
        };
        if !m.players.contains(&account) {
            return Err(errors::NOT_YOUR_MATCH);
        }
        if self.cfg.mode == SpectateMode::Off {
            self.send_none(out, conn, &uid);
            return Ok(());
        }
        let host = m.host == account;
        let mut stream = m.stream.clone();
        let n_players = m.players.len();
        let offerer = Offerer {
            conn,
            account,
            paused: o.paused,
            rollbacks: o.rollbacks,
            host,
            slot: None,
            standby: false,
            resumable: false,
            out: false,
        };
        if !self.games.contains_key(&uid) {
            let of_match: Vec<&Game> = self.games.values().filter(|g| g.match_id == o.match_id).collect();
            if of_match.len() >= self.cfg.max_games_per_match {
                return Err(errors::TOO_MANY_GAMES);
            }
            // One game of a match at a time: a player's offer for a new game means the last one is
            // over for them, so a game still open (its upload stopped before ENDG, or it never
            // got one) is closed: what was kept stays, marked as cut there.
            let open: Vec<[u8; 16]> = of_match.iter().filter(|g| g.open()).map(|g| g.uid).collect();
            for old in open {
                self.close_unfinished(now, old);
            }
            // A stream that is over stays over (its row keeps ENDED until it is forgotten, 10
            // minutes after it ended): a later game of the match (Direct and Unranked play on with
            // no set length) starts a new stream. Reopening the old one would let its forget timer
            // delete it mid-game.
            if self.streams.get(&stream).is_none_or(|b| b.over_at.is_some()) {
                stream = self.fresh_stream(now, &o.match_id);
            }
            self.games.insert(
                uid,
                Game {
                    match_id: o.match_id.clone(),
                    stream,
                    number: o.game,
                    uid,
                    offers: Vec::new(),
                    first_offer: now,
                    decided: false,
                    primary: None,
                    progress: now,
                    ordinal: None,
                    have: Vec::new(),
                    next_frame: None,
                    hashes: BTreeMap::new(),
                    pending: VecDeque::new(),
                    witness_frame: None,
                    bytes: 0,
                    tokens: self.cfg.bucket_burst,
                    refilled: now,
                    ended: false,
                    bad: false,
                    finished_at: None,
                },
            );
        }
        let game = self.games.get_mut(&uid).expect("just made");
        if game.match_id != o.match_id {
            return Err(errors::NOT_YOUR_MATCH);
        }
        if !game.open() {
            self.send_none(out, conn, &uid);
            return Ok(());
        }
        // A second offer from the same account replaces the first (a reconnect).
        let old_slot =
            game.offers.iter().position(|x| x.account == account).map(|i| game.offers.remove(i)).and_then(|x| x.slot);
        if let Some(s) = old_slot {
            self.slots.remove(&s);
            if game.primary == Some(s) {
                game.primary = None;
            }
        }
        game.offers.push(offerer);
        if game.decided {
            self.assign_late(now, uid, conn, out);
        } else if game.offers.len() >= n_players {
            self.decide(now, uid, out);
        }
        Ok(())
    }

    /// Gives every offering player its role: the healthiest is the primary (fewest pauses last
    /// game, then fewest rollbacks, then the host), the others witnesses.
    fn decide(&mut self, now: Instant, uid: [u8; 16], out: &mut Vec<Output>) {
        let Some(game) = self.games.get_mut(&uid) else { return };
        game.decided = true;
        let mut order: Vec<usize> = (0..game.offers.len()).collect();
        order.sort_by(|&a, &b| {
            let (x, y) = (&game.offers[a], &game.offers[b]);
            x.paused.cmp(&y.paused).then(x.rollbacks.total_cmp(&y.rollbacks)).then(y.host.cmp(&x.host))
        });
        let witness_only = self.cfg.mode == SpectateMode::Witness;
        for (rank, i) in order.into_iter().enumerate() {
            let role = if rank == 0 && !witness_only { Role::Primary } else { Role::Witness };
            self.give_role(now, uid, i, role, out);
        }
    }

    /// An offer after the roles were given: a witness, or the primary when the game has none.
    fn assign_late(&mut self, now: Instant, uid: [u8; 16], conn: ConnId, out: &mut Vec<Output>) {
        let Some(game) = self.games.get(&uid) else { return };
        let Some(i) = game.offers.iter().position(|o| o.conn == conn) else { return };
        let role =
            if game.primary.is_none() && self.cfg.mode == SpectateMode::On { Role::Primary } else { Role::Witness };
        self.give_role(now, uid, i, role, out);
    }

    /// A new slot with `role` for offer `i`. A primary's `from` is the next frame mm needs.
    fn give_role(&mut self, now: Instant, uid: [u8; 16], i: usize, role: Role, out: &mut Vec<Output>) {
        let slot = self.next_slot();
        let Some(game) = self.games.get_mut(&uid) else { return };
        if let Some(old) = game.offers[i].slot.take() {
            self.slots.remove(&old);
        }
        let conn = game.offers[i].conn;
        game.offers[i].slot = Some(slot);
        game.offers[i].standby = false;
        let (from, headers) = if role == Role::Primary {
            game.primary = Some(slot);
            game.progress = now;
            (game.next_frame, !game.headers_done())
        } else {
            (None, false)
        };
        self.slots.insert(slot, Slot { conn, game: uid, role, partial: None, next_seq: 0, from });
        let game = &self.games[&uid];
        let msg = self.role_msg(game, role, slot, from, headers);
        send(out, conn, &msg);
        tracing::info!(match_id = %game.match_id, game = game.number, slot, ?role, ?from, "spectate role");
    }

    /// Hands the game to the healthiest witness (or a standby that resumed). The old primary, if
    /// still there, was already told. Without anyone left the game waits for a late offer.
    fn takeover(&mut self, now: Instant, uid: [u8; 16], why: &str, out: &mut Vec<Output>) {
        let Some(game) = self.games.get_mut(&uid) else { return };
        game.primary = None;
        if !game.open() || self.cfg.mode != SpectateMode::On {
            return;
        }
        let pick = game
            .offers
            .iter()
            .enumerate()
            .filter(|(_, o)| !o.out && (!o.standby || o.resumable))
            .min_by(|(_, x), (_, y)| {
                x.standby.cmp(&y.standby).then(x.paused.cmp(&y.paused)).then(x.rollbacks.total_cmp(&y.rollbacks))
            })
            .map(|(i, _)| i);
        match pick {
            Some(i) => {
                tracing::info!(match_id = %game.match_id, game = game.number, why, "spectate takeover");
                self.give_role(now, uid, i, Role::Primary, out);
            }
            None => {
                tracing::info!(match_id = %game.match_id, game = game.number, why, "spectate: nobody left to upload");
                // Nobody will send the rest (a pause comes back within seconds; anything else
                // doesn't): spectators get what came instead of "Buffering…" until the next game.
                if why != "paused" {
                    self.cut_game(now, uid);
                }
            }
        }
    }

    /// The game's upload ended for good with nobody to take it over (its uploader stopped for the
    /// players' sake, left, or sent a bad chunk): spectators keep what came, with "The rest of
    /// this game wasn't streamed.", and the stream goes on as after a game's end (the match's next
    /// game, or over once the wait between games has passed). The list's row is not "between":
    /// the players are still playing it.
    fn cut_game(&mut self, now: Instant, uid: [u8; 16]) {
        let Some(game) = self.games.get_mut(&uid) else { return };
        if game.bad || game.ended {
            return;
        }
        game.bad = true;
        game.finished_at = Some(now);
        game.primary = None;
        tracing::info!(match_id = %game.match_id, game = game.number, "spectate: game cut; spectators get what came");
        let (stream_id, ordinal) = (game.stream.clone(), game.ordinal);
        self.store.change(&stream_id, |s| {
            if let Some(g) = ordinal.and_then(|o| s.games.get_mut(o)) {
                g.bad = Some("The rest of this game wasn't streamed.".into());
            }
        });
        if let Some(b) = self.streams.get_mut(&stream_id) {
            b.last_end = Some(now);
        }
        self.slots.retain(|_, s| s.game != uid);
    }

    /// Ends the primary's upload after a bad chunk and hands the game on.
    fn fail_primary(&mut self, now: Instant, slot: u32, why: &str, out: &mut Vec<Output>) {
        let Some(s) = self.slots.remove(&slot) else { return };
        let uid = s.game;
        if let Some(game) = self.games.get_mut(&uid) {
            tracing::warn!(match_id = %game.match_id, game = game.number, slot, "spectate upload refused: {why}");
            if let Some(o) = game.offerer_of_slot(slot) {
                o.out = true;
                o.slot = None;
            }
        }
        self.send_none(out, s.conn, &uid);
        self.takeover(now, uid, "bad upload", out);
    }

    /// A data packet (channel 1) from `conn`. Packets for a slot that is no primary any more (in
    /// flight when it was replaced) are dropped quietly.
    pub fn data(&mut self, now: Instant, conn: ConnId, packet: &[u8], out: &mut Vec<Output>) {
        let Some((h, bytes)) = spectate::parse_data(packet) else {
            tracing::warn!(conn, len = packet.len(), "malformed spectate data packet");
            if let Some(slot) =
                self.slots.iter().find(|(_, s)| s.conn == conn && s.role == Role::Primary).map(|(k, _)| *k)
            {
                self.fail_primary(now, slot, "malformed data packet", out);
            }
            return;
        };
        let Some(s) = self.slots.get_mut(&h.slot) else { return };
        if s.conn != conn || s.role != Role::Primary {
            return;
        }
        let ok = match &mut s.partial {
            None if h.seq == s.next_seq && h.offset == 0 => {
                let mut buf = Vec::with_capacity(h.total as usize);
                buf.extend_from_slice(bytes);
                s.partial = Some((h.seq, h.total, buf));
                true
            }
            Some((seq, total, buf)) if *seq == h.seq && *total == h.total && h.offset as usize == buf.len() => {
                buf.extend_from_slice(bytes);
                true
            }
            _ => false,
        };
        if !ok {
            return self.fail_primary(now, h.slot, "data packets out of order", out);
        }
        let complete = matches!(&s.partial, Some((_, total, buf)) if buf.len() == *total as usize);
        if !complete {
            return;
        }
        let (_, _, chunk) = s.partial.take().expect("complete");
        s.next_seq += 1;
        let (uid, from) = (s.game, s.from);
        if let Err(why) = self.chunk(now, uid, h.slot, from, chunk) {
            self.fail_primary(now, h.slot, &why, out);
        }
    }

    /// Checks one whole chunk from the primary and keeps it.
    fn chunk(
        &mut self,
        now: Instant,
        uid: [u8; 16],
        slot: u32,
        from: Option<u32>,
        bytes: Vec<u8>,
    ) -> Result<(), String> {
        let cfg = &self.cfg;
        let game = self.games.get_mut(&uid).ok_or("no such game")?;
        if !game.open() {
            return Err("the game is over".into());
        }
        game.tokens =
            (game.tokens + now.duration_since(game.refilled).as_secs_f64() * cfg.bucket_rate).min(cfg.bucket_burst);
        game.refilled = now;
        if game.tokens < bytes.len() as f64 {
            return Err("over the upload rate".into());
        }
        game.tokens -= bytes.len() as f64;
        if game.bytes + bytes.len() > cfg.game_budget {
            return Err("over the game's budget".into());
        }
        let view = spectate::parse_chunk(&bytes)?;
        let kind = view.kind;
        let mut info: Option<serde_json::Value> = None;
        let mut endg: Option<serde_json::Value> = None;
        let mut frames: Option<(u32, u32, [u8; 8])> = None;
        match kind {
            ChunkKind::Info | ChunkKind::Setp | ChunkKind::Sync => {
                if game.have.contains(&kind) {
                    // A takeover's primary that sent its headers anyway: already have them.
                    return Ok(());
                }
                if game.next_frame.is_some() {
                    return Err(format!("{} after the frames", kind.name()));
                }
                if kind == ChunkKind::Info {
                    let v: serde_json::Value =
                        serde_json::from_slice(&view.raw()?).map_err(|e| format!("INFO is not JSON: {e}"))?;
                    let m = self.matches.get(&game.match_id).ok_or("no such match")?;
                    if v.get("matchId").and_then(|x| x.as_str()) != Some(game.match_id.as_str()) {
                        return Err("INFO names another match".into());
                    }
                    let players = v.get("players").and_then(|p| p.as_array()).ok_or("INFO has no players")?;
                    if players.is_empty() || players.len() > 4 {
                        return Err("INFO players".into());
                    }
                    for p in players {
                        let pu = p.get("uid").and_then(|u| u.as_str()).and_then(|u| Uuid::parse_str(u).ok());
                        if !pu.is_some_and(|pu| m.players.contains(&pu)) {
                            return Err("INFO names a player who isn't in the match".into());
                        }
                    }
                    info = Some(v);
                }
            }
            ChunkKind::Frms => {
                if !game.headers_done() {
                    return Err("FRMS before INFO and SETP".into());
                }
                let raw = view.raw()?;
                let head = spectate::frames_head(&raw)?;
                let expected = game.next_frame.or(from);
                if let Some(e) = expected {
                    if head.first_frame != e {
                        return Err(format!("FRMS starts at {}, expected {e}", head.first_frame));
                    }
                }
                frames = Some((head.first_frame, head.count, spectate::frames_digest(&raw)));
            }
            ChunkKind::Evnt => {
                if !game.headers_done() {
                    return Err("EVNT before INFO and SETP".into());
                }
            }
            ChunkKind::Endg => {
                if !game.headers_done() {
                    return Err("ENDG before INFO and SETP".into());
                }
                endg = serde_json::from_slice(&view.raw()?).ok();
            }
        }
        // Kept from here on.
        game.bytes += bytes.len();
        if kind.is_header() {
            game.have.push(kind);
        }
        let mut mismatch = None;
        if let Some((first, count, digest)) = frames {
            game.next_frame = Some(first + count);
            game.hashes.insert(first, digest);
            game.progress = now;
            game.pending.retain(|(f, d)| {
                if *f == first && *d != digest {
                    mismatch = Some(first);
                }
                *f != first
            });
        }
        let stream_id = game.stream.clone();
        let number = game.number;
        let game_uid = hex::encode(uid);
        let ordinal = match game.ordinal {
            Some(o) => o,
            None => {
                let o = self.open_game_in_store(now, &stream_id, number, &game_uid, info.as_ref());
                self.games.get_mut(&uid).expect("present").ordinal = Some(o);
                o
            }
        };
        let chunk: Arc<[u8]> = bytes.into();
        self.store.change(&stream_id, |s| {
            if let Some(g) = s.games.get_mut(ordinal) {
                g.chunks.push(chunk);
            }
        });
        if let Some(b) = self.streams.get_mut(&stream_id) {
            b.last_upload = now;
        }
        if kind == ChunkKind::Endg {
            self.end_game(now, uid, endg.as_ref());
        }
        if let Some(f) = mismatch {
            self.mark_bad(now, uid, &format!("a witness disagrees with frames from {f}"));
        }
        let _ = slot;
        Ok(())
    }

    /// The game's first kept chunk: lists the stream (with `INFO`'s characters and stage) and adds
    /// the game to the store. An open game before it in the stream is cut there.
    fn open_game_in_store(
        &mut self,
        now: Instant,
        stream_id: &str,
        number: u32,
        game_uid: &str,
        info: Option<&serde_json::Value>,
    ) -> usize {
        let wall = Utc::now();
        let m =
            self.games.values().find(|g| g.stream == stream_id).and_then(|g| self.matches.get(&g.match_id)).cloned();
        let book = self.streams.get_mut(stream_id);
        let (players, wins, started_at) = match book {
            Some(b) => {
                let started = b.started_at.get_or_insert_with(|| rfc3339(wall)).clone();
                b.listed = true;
                b.last_end = None;
                (b.players.clone(), b.wins.clone(), started)
            }
            None => (Vec::new(), HashMap::new(), rfc3339(wall)),
        };
        let _ = now;
        let mut entry = LiveEntry {
            id: stream_id.to_string(),
            mode: m.as_ref().map(|m| m.mode.to_string()).unwrap_or_default(),
            room: m.as_ref().and_then(|m| m.room.clone()),
            is_teams: false,
            started_at,
            game: LiveGame { number, place: 0, started_at: rfc3339(wall), stage_id: 0, state: GameState::Playing },
            players: Vec::new(),
            spectators: None,
        };
        if let Some(v) = info {
            entry.is_teams = v.get("isTeams").and_then(|t| t.as_bool()).unwrap_or(false);
            entry.game.stage_id = v.get("stageId").and_then(|s| s.as_u64()).unwrap_or(0) as u32;
            for p in v.get("players").and_then(|p| p.as_array()).into_iter().flatten() {
                let Some(uid) = p.get("uid").and_then(|u| u.as_str()).and_then(|u| Uuid::parse_str(u).ok()) else {
                    continue;
                };
                if !players.contains(&uid) {
                    continue;
                }
                let text = |k: &str| p.get(k).and_then(|x| x.as_str()).unwrap_or("").as_bytes().to_vec();
                let (name, code) = m.as_ref().and_then(|m| m.names.get(&uid).cloned()).unwrap_or_else(|| {
                    (
                        common::chat::clean_text(&text("displayName"), 32).unwrap_or_default(),
                        common::chat::clean_text(&text("connectCode"), 16).unwrap_or_default(),
                    )
                });
                let num = |k: &str| p.get(k).and_then(|x| x.as_u64()).map(|x| x.min(u32::MAX as u64) as u32);
                entry.players.push(LivePlayer {
                    uid: uid.to_string(),
                    display_name: name,
                    connect_code: code,
                    character_id: num("characterId").unwrap_or(0),
                    costume: num("costume").unwrap_or(0),
                    team_id: num("teamId"),
                    wins: wins.get(&uid).copied().unwrap_or(0),
                });
            }
        }
        self.store.create(stream_id, entry.clone());
        let mut ordinal = 0;
        self.store.change(stream_id, |s| {
            for g in s.games.iter_mut() {
                if !g.ended && g.bad.is_none() {
                    g.bad = Some("The rest of this game wasn't streamed.".into());
                }
            }
            s.games.push(GameView { number, uid: game_uid.to_string(), ..Default::default() });
            ordinal = s.games.len() - 1;
            s.entry = entry;
            s.entry.game.place = s.games.len() as u32;
            s.over = false;
        });
        ordinal
    }

    /// `ENDG` kept: the game is over, its winner counted, the stream waits for the next game.
    fn end_game(&mut self, now: Instant, uid: [u8; 16], endg: Option<&serde_json::Value>) {
        let Some(game) = self.games.get_mut(&uid) else { return };
        game.ended = true;
        game.finished_at = Some(now);
        game.primary = None;
        let (stream_id, ordinal) = (game.stream.clone(), game.ordinal);
        let mode = self.matches.get(&game.match_id).map_or("", |m| m.mode);
        let method = endg.and_then(|e| e.get("endMethod")).and_then(|m| m.as_str()).unwrap_or("").to_string();
        let mut winner_port = endg.and_then(|e| e.get("winnerPort")).and_then(|w| w.as_u64());
        // A Ranked quit loses the game for the quitter: the other player won it, whatever the
        // record says about a winner.
        if mode == "ranked" && method == "quit" {
            let quitter = endg.and_then(|e| e.get("quitterPort")).and_then(|q| q.as_u64());
            if let Some(q) = quitter {
                let ports = self.info_ports(&stream_id, ordinal);
                if ports.len() == 2 {
                    winner_port = ports.keys().map(|&p| p as u64).find(|&p| p != q).or(winner_port);
                }
            }
        }
        let mut winners: Vec<String> = Vec::new();
        if let Some(port) = winner_port {
            // The winner's uid (and team mates) from the stored entry: INFO's players by port.
            if let Some(entry) = self.store.entry(&stream_id) {
                let by_port = self.info_ports(&stream_id, ordinal);
                if let Some(wuid) = by_port.get(&(port as u32)) {
                    let team = entry.players.iter().find(|p| &p.uid == wuid).and_then(|p| p.team_id);
                    winners = entry
                        .players
                        .iter()
                        .filter(|p| &p.uid == wuid || (entry.is_teams && team.is_some() && p.team_id == team))
                        .map(|p| p.uid.clone())
                        .collect();
                }
            }
        }
        if let Some(b) = self.streams.get_mut(&stream_id) {
            b.last_end = Some(now);
            for w in &winners {
                if let Ok(u) = Uuid::parse_str(w) {
                    *b.wins.entry(u).or_default() += 1;
                }
            }
            let best = b.wins.values().copied().max().unwrap_or(0);
            if endg.is_some() && ends_stream(mode, &method, best) {
                tracing::info!(stream = %stream_id, mode, method, best, "spectate: the set can't go on; stream over");
                b.set_over = true;
            }
        }
        self.store.change(&stream_id, |s| {
            if let Some(g) = ordinal.and_then(|o| s.games.get_mut(o)) {
                g.ended = true;
            }
            s.entry.game.state = GameState::Between;
            for p in s.entry.players.iter_mut() {
                if winners.contains(&p.uid) {
                    p.wins += 1;
                }
            }
        });
        // The game's slots are done.
        self.slots.retain(|_, s| s.game != uid);
    }

    /// Port (1-4) → uid from the game's kept `INFO`.
    fn info_ports(&self, stream_id: &str, ordinal: Option<usize>) -> HashMap<u32, String> {
        let mut map = HashMap::new();
        let Some(o) = ordinal else { return map };
        self.store.with(|m| {
            let Some(g) = m.get(stream_id).and_then(|s| s.games.get(o)) else { return };
            for c in &g.chunks {
                let Ok(v) = spectate::parse_chunk(c) else { continue };
                if v.kind != ChunkKind::Info {
                    continue;
                }
                let Ok(raw) = v.raw() else { continue };
                let Ok(j) = serde_json::from_slice::<serde_json::Value>(&raw) else { continue };
                for p in j.get("players").and_then(|p| p.as_array()).into_iter().flatten() {
                    if let (Some(port), Some(uid)) =
                        (p.get("port").and_then(|x| x.as_u64()), p.get("uid").and_then(|x| x.as_str()))
                    {
                        map.insert(port as u32, uid.to_string());
                    }
                }
            }
        });
        map
    }

    /// A game whose upload stopped before its end: kept as it is, its end marked as cut (the
    /// next game of the match starts).
    fn close_unfinished(&mut self, now: Instant, uid: [u8; 16]) {
        let Some(game) = self.games.get_mut(&uid) else { return };
        game.ended = true;
        game.finished_at = Some(now);
        game.primary = None;
        tracing::info!(match_id = %game.match_id, game = game.number, "spectate: game closed unfinished");
        let (stream_id, ordinal) = (game.stream.clone(), game.ordinal);
        self.store.change(&stream_id, |s| {
            if let Some(g) = ordinal.and_then(|o| s.games.get_mut(o)) {
                if !g.ended && g.bad.is_none() {
                    g.bad = Some("The rest of this game wasn't streamed.".into());
                }
            }
        });
        self.slots.retain(|_, s| s.game != uid);
    }

    fn mark_bad(&mut self, now: Instant, uid: [u8; 16], why: &str) {
        let Some(game) = self.games.get_mut(&uid) else { return };
        if game.bad {
            return;
        }
        game.bad = true;
        game.finished_at = Some(now);
        game.primary = None;
        let accounts: Vec<Uuid> = game.offers.iter().map(|o| o.account).collect();
        tracing::warn!(match_id = %game.match_id, game = game.number, ?accounts, "spectate: game marked bad: {why}");
        let (stream_id, ordinal) = (game.stream.clone(), game.ordinal);
        self.store.change(&stream_id, |s| {
            if let Some(g) = ordinal.and_then(|o| s.games.get_mut(o)) {
                g.bad = Some("This game can't be shown.".into());
            }
            s.entry.game.state = GameState::Between;
        });
        if let Some(b) = self.streams.get_mut(&stream_id) {
            b.last_end = Some(now);
        }
        self.slots.retain(|_, s| s.game != uid);
    }

    fn digest(
        &mut self,
        now: Instant,
        conn: ConnId,
        slot: u32,
        frms: Vec<(u32, [u8; 8])>,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        let Some(s) = self.slots.get(&slot).filter(|s| s.conn == conn) else {
            // A slot that ended meanwhile (a takeover): fine.
            return Ok(());
        };
        if s.role != Role::Witness {
            return Ok(());
        }
        let uid = s.game;
        let Some(game) = self.games.get_mut(&uid) else { return Ok(()) };
        let mut bad = None;
        for (f, d) in frms {
            game.witness_frame = Some(game.witness_frame.map_or(f, |w| w.max(f)));
            match game.hashes.get(&f) {
                Some(h) if *h != d => bad = Some(f),
                Some(_) => {}
                None => {
                    if game.pending.len() >= 120 {
                        game.pending.pop_front();
                    }
                    game.pending.push_back((f, d));
                }
            }
        }
        if let Some(f) = bad {
            self.mark_bad(now, uid, &format!("a witness disagrees with frames from {f}"));
        }
        let _ = out;
        Ok(())
    }

    fn pause(
        &mut self,
        now: Instant,
        conn: ConnId,
        slot: u32,
        reason: &str,
        stop: bool,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        let Some(s) = self.slots.get(&slot).filter(|s| s.conn == conn) else { return Ok(()) };
        let uid = s.game;
        let is_primary = s.role == Role::Primary;
        let Some(game) = self.games.get_mut(&uid) else { return Ok(()) };
        tracing::info!(match_id = %game.match_id, game = game.number, slot, reason, stop, "spectate upload paused");
        let has_witness = game.offers.iter().any(|o| !o.out && !o.standby && o.slot.is_some() && o.slot != Some(slot));
        let o = game.offerer_of_slot(slot).ok_or(errors::NOT_YOUR_SLOT)?;
        if stop {
            o.out = true;
        }
        if !is_primary && !stop {
            return Ok(());
        }
        if stop || has_witness {
            o.standby = !stop;
            o.resumable = false;
            o.slot = None;
            self.slots.remove(&slot);
            if stop && !is_primary {
                return Ok(());
            }
            self.send_none(out, conn, &uid);
            self.takeover(now, uid, if stop { "stopped" } else { "paused" }, out);
        }
        // A paused primary with nobody to take over keeps its role and resumes by itself.
        Ok(())
    }

    fn resume(&mut self, conn: ConnId, slot: u32) -> Result<(), &'static str> {
        // A primary that kept its role just goes on. A standby (its slot ended) may be chosen
        // again by a later takeover.
        if self.slots.contains_key(&slot) {
            return Ok(());
        }
        for g in self.games.values_mut() {
            for o in g.offers.iter_mut().filter(|o| o.conn == conn && o.standby) {
                o.resumable = true;
            }
        }
        Ok(())
    }

    /// An online connection went away: its uploads end, and games it was the primary of are
    /// handed on.
    pub fn conn_gone(&mut self, now: Instant, conn: ConnId, out: &mut Vec<Output>) {
        let gone: Vec<(u32, [u8; 16], Role)> =
            self.slots.iter().filter(|(_, s)| s.conn == conn).map(|(k, s)| (*k, s.game, s.role)).collect();
        for g in self.games.values_mut() {
            for o in g.offers.iter_mut().filter(|o| o.conn == conn) {
                o.out = true;
                o.slot = None;
            }
        }
        for (slot, uid, role) in gone {
            self.slots.remove(&slot);
            if role == Role::Primary {
                self.takeover(now, uid, "primary disconnected", out);
            }
        }
    }

    pub fn tick(&mut self, now: Instant, out: &mut Vec<Output>) {
        // Roles for games whose other players never offered.
        let due: Vec<[u8; 16]> = self
            .games
            .values()
            .filter(|g| !g.decided && now.duration_since(g.first_offer) >= self.cfg.decide_after)
            .map(|g| g.uid)
            .collect();
        for uid in due {
            self.decide(now, uid, out);
        }
        // A primary that went quiet while a witness shows the game going on.
        let stalled: Vec<(u32, [u8; 16], ConnId)> = self
            .games
            .values()
            .filter(|g| g.open() && now.duration_since(g.progress) >= self.cfg.stall)
            .filter(|g| match (g.witness_frame, g.next_frame) {
                (Some(w), Some(n)) => w >= n,
                (Some(_), None) => g.headers_done() || now.duration_since(g.progress) >= self.cfg.stall * 3,
                _ => false,
            })
            .filter_map(|g| g.primary.map(|p| (p, g.uid)))
            .filter_map(|(p, uid)| self.slots.get(&p).map(|s| (p, uid, s.conn)))
            .collect();
        for (slot, uid, conn) in stalled {
            if let Some(g) = self.games.get_mut(&uid) {
                if let Some(o) = g.offerer_of_slot(slot) {
                    o.standby = true;
                    o.resumable = false;
                    o.slot = None;
                }
            }
            self.slots.remove(&slot);
            self.send_none(out, conn, &uid);
            self.takeover(now, uid, "primary stalled", out);
        }
        // Streams: over between games or idle, then forgotten.
        let mut over = Vec::new();
        let mut forget = Vec::new();
        for (id, b) in self.streams.iter_mut() {
            match b.over_at {
                Some(t) if now.duration_since(t) >= self.cfg.keep => forget.push(id.clone()),
                Some(_) => {}
                None => {
                    let open = self.games.values().any(|g| &g.stream == id && g.open() && g.ordinal.is_some());
                    let between = !open
                        && b.last_end.is_some_and(|t| b.set_over || now.duration_since(t) >= self.cfg.between_games);
                    let idle = now.duration_since(b.last_upload) >= self.cfg.idle;
                    if between || idle {
                        b.over_at = Some(now);
                        over.push(id.clone());
                    }
                }
            }
        }
        for id in over {
            self.store.change(&id, |s| {
                s.over = true;
                s.entry.game.state = GameState::Over;
                for g in s.games.iter_mut() {
                    if !g.ended && g.bad.is_none() {
                        g.bad = Some("The rest of this game wasn't streamed.".into());
                    }
                }
            });
            for g in self.games.values_mut().filter(|g| g.stream == id && g.open()) {
                g.ended = true;
                g.finished_at = Some(now);
            }
        }
        for id in &forget {
            self.streams.remove(id);
            self.store.remove(id);
        }
        // Old bookkeeping.
        let ttl = self.cfg.match_ttl;
        let streams = &self.streams;
        self.matches.retain(|_, m| now.duration_since(m.made) < ttl || streams.contains_key(&m.stream));
        let keep = self.cfg.keep;
        let dead: Vec<[u8; 16]> = self
            .games
            .values()
            .filter(|g| {
                !streams.contains_key(&g.stream) || g.finished_at.is_some_and(|t| now.duration_since(t) >= keep)
            })
            .map(|g| g.uid)
            .collect();
        for uid in dead {
            self.games.remove(&uid);
            self.slots.retain(|_, s| s.game != uid);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use common::proto::Mode;
    use common::spectate::{data_packets, encode_chunk, frames_digest};
    use serde_json::{json, Value};

    struct T {
        book: LiveBook,
        now: Instant,
        a: Uuid,
        b: Uuid,
    }

    const MATCH: &str = "mode.direct-2026-10-13T18:00:00.000Z-1";
    const A: ConnId = 1;
    const B: ConnId = 2;

    fn uid_hex(n: u8) -> String {
        hex::encode([n; 16])
    }

    impl T {
        fn new(mode: SpectateMode) -> T {
            T::with_match_mode(mode, Mode::Direct)
        }

        fn with_match_mode(mode: SpectateMode, match_mode: Mode) -> T {
            let mut book = LiveBook::new(LiveConfig { mode, ..Default::default() });
            let (a, b) = (Uuid::new_v4(), Uuid::new_v4());
            let now = Instant::now();
            let rec = MatchRecord {
                match_id: MATCH.into(),
                mode: match_mode,
                players: vec![a, b],
                host: a,
                stages: vec![],
                region: String::new(),
                room: None,
            };
            let names = HashMap::from([(a, ("alice".into(), "ALI#1".into())), (b, ("bob".into(), "BOB#2".into()))]);
            book.register_match(now, &rec, names);
            T { book, now, a, b }
        }

        fn offer(&mut self, conn: ConnId, who: Uuid, game: u8, paused: u32) -> Vec<Output> {
            let mut out = vec![];
            let o =
                Offer { match_id: MATCH.into(), game_uid: uid_hex(game), game: game as u32, paused, rollbacks: 0.0 };
            self.book.request(self.now, conn, who, SpectateRequest::Offer(o), &mut out);
            out
        }

        fn send_chunk(&mut self, conn: ConnId, slot: u32, seq: u32, chunk: &[u8]) -> Vec<Output> {
            let mut out = vec![];
            for p in data_packets(slot, seq, chunk) {
                self.book.data(self.now, conn, &p, &mut out);
            }
            out
        }

        fn advance(&mut self, d: Duration) -> Vec<Output> {
            self.now += d;
            let mut out = vec![];
            self.book.tick(self.now, &mut out);
            out
        }

        fn info(&self, game: u8) -> Vec<u8> {
            let v = json!({
                "matchId": MATCH, "gameIndex": game, "stageId": 3, "isTeams": false,
                "players": [
                    {"port": 1, "uid": self.a.to_string(), "displayName": "x", "connectCode": "X#1", "characterId": 17, "costume": 1},
                    {"port": 2, "uid": self.b.to_string(), "displayName": "y", "connectCode": "Y#1", "characterId": 9, "costume": 0}
                ]
            });
            encode_chunk(b"INFO", v.to_string().as_bytes(), false)
        }
    }

    fn sent(out: &[Output], conn: ConnId) -> Vec<Value> {
        out.iter()
            .filter_map(|o| match o {
                Output::Send { conn: c, json } if *c == conn => serde_json::from_str(json).ok(),
                _ => None,
            })
            .collect()
    }

    fn role(out: &[Output], conn: ConnId) -> Option<Value> {
        sent(out, conn).into_iter().rev().find(|m| m["type"] == "spectate-role")
    }

    fn frms(first: u32, count: u16, salt: u8) -> Vec<u8> {
        let mut raw = Vec::new();
        raw.extend_from_slice(&first.to_le_bytes());
        raw.extend_from_slice(&count.to_le_bytes());
        raw.push(0b11);
        raw.push(0);
        raw.extend(std::iter::repeat_n(salt, 2 * count as usize * 0x40));
        raw.extend(std::iter::repeat_n(5u8, count as usize * 4));
        raw.extend_from_slice(&0u16.to_le_bytes());
        raw
    }

    fn read_all(store: &LiveStore, id: &str) -> Vec<LiveLine> {
        let mut c = Cursor::default();
        store.read(id, &mut c).unwrap().0
    }

    fn stream_id(t: &T) -> String {
        t.book.store.entries()[0].id.clone()
    }

    /// Two players offer; the healthier one uploads; the stream is listed with mm's names and
    /// INFO's characters; frames are kept in order; ENDG counts the win.
    #[test]
    fn a_game_streams_from_its_primary() {
        let mut t = T::new(SpectateMode::On);
        assert!(role(&t.offer(A, t.a, 1, 2), A).is_none(), "waits for the other player");
        let (a, b) = (t.a, t.b);
        let out = t.offer(B, b, 1, 0);
        let rb = role(&out, B).unwrap();
        let ra = role(&out, A).unwrap();
        assert_eq!(rb["role"], "primary", "B paused less last game");
        assert_eq!(ra["role"], "witness");
        assert_eq!(rb["headers"], true);
        let slot = rb["slot"].as_u64().unwrap() as u32;

        let info = t.info(1);
        assert!(t.send_chunk(B, slot, 0, &info).is_empty());
        t.send_chunk(B, slot, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(B, slot, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        t.send_chunk(B, slot, 3, &encode_chunk(b"FRMS", &frms(180, 60, 2), true));

        let entries = t.book.store.entries();
        assert_eq!(entries.len(), 1);
        let e = &entries[0];
        assert_eq!(
            (e.mode.as_str(), e.game.number, e.game.stage_id, e.game.state),
            ("direct", 1, 3, GameState::Playing)
        );
        assert_eq!(e.players[0].display_name, "alice", "mm's name, not INFO's");
        assert_eq!(e.players[0].character_id, 17);
        let lines = read_all(&t.book.store, &e.id);
        assert!(matches!(&lines[0], LiveLine::Game { game: 1, number: 1, .. }));
        assert_eq!(lines.iter().filter(|l| matches!(l, LiveLine::Chunk { .. })).count(), 4);

        let endg = encode_chunk(b"ENDG", json!({"winnerPort": 2}).to_string().as_bytes(), false);
        t.send_chunk(B, slot, 4, &endg);
        let e = &t.book.store.entries()[0];
        assert_eq!(e.game.state, GameState::Between);
        assert_eq!(e.players.iter().find(|p| p.uid == b.to_string()).unwrap().wins, 1);
        assert_eq!(e.players.iter().find(|p| p.uid == a.to_string()).unwrap().wins, 0);
        assert!(matches!(read_all(&t.book.store, &e.id).last(), Some(LiveLine::End { game: 1 })));
    }

    #[test]
    fn off_answers_none_and_lists_nothing() {
        let mut t = T::new(SpectateMode::Off);
        let a = t.a;
        let out = t.offer(A, a, 1, 0);
        assert_eq!(role(&out, A).unwrap()["role"], "none");
        assert!(t.book.store.entries().is_empty());
    }

    #[test]
    fn witness_mode_has_no_primary() {
        let mut t = T::new(SpectateMode::Witness);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = t.offer(B, b, 1, 0);
        assert_eq!(role(&out, A).unwrap()["role"], "witness");
        assert_eq!(role(&out, B).unwrap()["role"], "witness");
    }

    #[test]
    fn strangers_and_unknown_matches_are_refused() {
        let mut t = T::new(SpectateMode::On);
        let out = t.offer(3, Uuid::new_v4(), 1, 0);
        let m = &sent(&out, 3)[0];
        assert_eq!((m["type"].as_str(), m["error"].as_str()), (Some("spectate-error"), Some(errors::NOT_YOUR_MATCH)));
    }

    /// One player has streaming off: after `decide_after` the other uploads alone.
    #[test]
    fn a_lone_offer_is_decided_after_a_moment() {
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        t.offer(A, a, 1, 0);
        assert!(t.advance(Duration::from_millis(500)).is_empty());
        let out = t.advance(Duration::from_secs(2));
        assert_eq!(role(&out, A).unwrap()["role"], "primary");
    }

    /// A bad chunk ends the primary's upload and the witness takes over from the next frame,
    /// without headers (mm has them).
    #[test]
    fn a_bad_chunk_hands_the_game_to_the_witness() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = t.offer(B, b, 1, 0);
        let slot_a = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        assert_eq!(role(&out, A).unwrap()["role"], "primary", "the host on a tie");
        let info = t.info(1);
        t.send_chunk(A, slot_a, 0, &info);
        t.send_chunk(A, slot_a, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot_a, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        let mut broken = encode_chunk(b"FRMS", &frms(180, 60, 1), true);
        let n = broken.len();
        broken[n - 1] ^= 0xFF;
        let out = t.send_chunk(A, slot_a, 3, &broken);
        assert_eq!(role(&out, A).unwrap()["role"], "none");
        let rb = role(&out, B).unwrap();
        assert_eq!(
            (rb["role"].as_str(), rb["from"].as_u64(), rb["headers"].as_bool()),
            (Some("primary"), Some(180), Some(false))
        );
        let slot_b = rb["slot"].as_u64().unwrap() as u32;
        // Frames from anywhere but 180 are refused; from 180 they are kept.
        let out = t.send_chunk(B, slot_b, 0, &encode_chunk(b"FRMS", &frms(240, 60, 1), true));
        assert_eq!(role(&out, B).unwrap()["role"], "none");
    }

    #[test]
    fn a_takeover_continues_at_its_frame() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = t.offer(B, b, 1, 0);
        let slot_a = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot_a, 0, &info);
        t.send_chunk(A, slot_a, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot_a, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        let mut out = vec![];
        t.book.request(t.now, A, a, SpectateRequest::Pause { slot: slot_a, reason: "rtt".into() }, &mut out);
        assert_eq!(role(&out, A).unwrap()["role"], "none");
        let slot_b = role(&out, B).unwrap()["slot"].as_u64().unwrap() as u32;
        // Data still in flight from the old primary is dropped quietly.
        assert!(t.send_chunk(A, slot_a, 3, &encode_chunk(b"FRMS", &frms(180, 60, 1), true)).is_empty());
        assert!(t.send_chunk(B, slot_b, 0, &encode_chunk(b"FRMS", &frms(180, 60, 1), true)).is_empty());
        let id = stream_id(&t);
        let chunks = read_all(&t.book.store, &id).iter().filter(|l| matches!(l, LiveLine::Chunk { .. })).count();
        assert_eq!(chunks, 4);
    }

    #[test]
    fn a_witness_that_disagrees_marks_the_game_bad() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = t.offer(B, b, 1, 0);
        let slot_a = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let slot_b = role(&out, B).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot_a, 0, &info);
        t.send_chunk(A, slot_a, 1, &encode_chunk(b"SETP", b"{}", false));
        let good = frms(120, 60, 1);
        t.send_chunk(A, slot_a, 2, &encode_chunk(b"FRMS", &good, true));
        let mut out = vec![];
        let agree = SpectateRequest::Digest { slot: slot_b, frms: vec![(120, frames_digest(&good))] };
        t.book.request(t.now, B, b, agree, &mut out);
        let id = stream_id(&t);
        assert!(!read_all(&t.book.store, &id).iter().any(|l| matches!(l, LiveLine::Bad { .. })));
        // A digest that arrives before its chunk is checked when the chunk comes.
        let later = frms(180, 60, 9);
        let differ = SpectateRequest::Digest { slot: slot_b, frms: vec![(180, frames_digest(&frms(180, 60, 8)))] };
        t.book.request(t.now, B, b, differ, &mut out);
        t.send_chunk(A, slot_a, 3, &encode_chunk(b"FRMS", &later, true));
        assert!(read_all(&t.book.store, &id).iter().any(|l| matches!(l, LiveLine::Bad { game: 1, .. })));
    }

    #[test]
    fn a_stalled_primary_is_replaced() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = t.offer(B, b, 1, 0);
        let slot_a = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let slot_b = role(&out, B).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot_a, 0, &info);
        t.send_chunk(A, slot_a, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot_a, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        let mut out = vec![];
        let ahead = SpectateRequest::Digest { slot: slot_b, frms: vec![(240, [0; 8])] };
        t.book.request(t.now, B, b, ahead, &mut out);
        assert!(t.advance(Duration::from_secs(2)).is_empty());
        let out = t.advance(Duration::from_secs(4));
        assert_eq!(role(&out, A).unwrap()["role"], "none");
        assert_eq!(role(&out, B).unwrap()["role"], "primary");
        assert_eq!(role(&out, B).unwrap()["from"], 180);
    }

    #[test]
    fn the_rate_and_budget_are_enforced() {
        let mut t = T::new(SpectateMode::On);
        t.book.cfg.bucket_burst = 5000.0;
        t.book.cfg.bucket_rate = 1000.0;
        let a = t.a;
        t.offer(A, a, 1, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let big = encode_chunk(b"SETP", &vec![b' '; 6000], false);
        let out = t.send_chunk(A, slot, 0, &big);
        assert_eq!(role(&out, A).unwrap()["role"], "none", "6 KB at once is over a 5 KB burst");
    }

    #[test]
    fn frames_before_the_headers_and_strangers_in_info_are_refused() {
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        t.offer(A, a, 1, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let out = t.send_chunk(A, slot, 0, &encode_chunk(b"FRMS", &frms(0, 60, 1), true));
        assert_eq!(role(&out, A).unwrap()["role"], "none");

        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        t.offer(A, a, 1, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let v = json!({"matchId": MATCH, "players": [{"port": 1, "uid": Uuid::new_v4().to_string()}]});
        let out = t.send_chunk(A, slot, 0, &encode_chunk(b"INFO", v.to_string().as_bytes(), false));
        assert_eq!(role(&out, A).unwrap()["role"], "none");
        assert!(t.book.store.entries().is_empty());
    }

    /// Plays game `game` of the match to its end, uploaded by A, with `endg`.
    fn play_game(t: &mut T, game: u8, endg: &str) {
        let a = t.a;
        t.offer(A, a, game, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(game);
        t.send_chunk(A, slot, 0, &info);
        t.send_chunk(A, slot, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        t.send_chunk(A, slot, 3, &encode_chunk(b"ENDG", endg.as_bytes(), false));
    }

    /// A set that can't go on ends its stream at once (the user, 2026-10-14: the list should show
    /// it ended right away): a disconnect, a player leaving the match or an error in any mode; in
    /// Ranked a quit, or a player's second win. A quit or a desync elsewhere plays on.
    #[test]
    fn a_set_that_cant_go_on_ends_its_stream_at_once() {
        let over = |t: &T| t.book.store.entries()[0].game.state == GameState::Over;
        for method in ["disconnect", "stopped", "error"] {
            let mut t = T::new(SpectateMode::On);
            play_game(&mut t, 1, &format!("{{\"endMethod\":\"{method}\"}}"));
            t.advance(Duration::from_secs(1));
            assert!(over(&t), "{method}");
        }
        for method in ["quit", "desync", "runback"] {
            let mut t = T::new(SpectateMode::On);
            play_game(&mut t, 1, &format!("{{\"endMethod\":\"{method}\"}}"));
            t.advance(Duration::from_secs(1));
            assert!(!over(&t), "Direct {method}");
        }
        // A Ranked quit loses that game, not the set (docs/game-code.md): the stream goes on, and
        // the quit counts as the other player's win.
        let mut t = T::with_match_mode(SpectateMode::On, Mode::Ranked);
        play_game(&mut t, 1, r#"{"endMethod":"quit","quitterPort":1}"#);
        t.advance(Duration::from_secs(1));
        assert!(!over(&t), "a Ranked quit loses one game");
        let e = &t.book.store.entries()[0];
        assert_eq!(e.players.iter().find(|p| p.uid == t.b.to_string()).unwrap().wins, 1, "the quitter's opponent won it");
        play_game(&mut t, 2, r#"{"endMethod":"game","winnerPort":2}"#);
        t.advance(Duration::from_secs(1));
        assert!(over(&t), "2-0 after the quit");
        let mut t = T::with_match_mode(SpectateMode::On, Mode::Ranked);
        play_game(&mut t, 1, r#"{"endMethod":"game","winnerPort":1}"#);
        t.advance(Duration::from_secs(1));
        assert!(!over(&t), "1-0 in a best of three");
        play_game(&mut t, 2, r#"{"endMethod":"game","winnerPort":1}"#);
        t.advance(Duration::from_secs(1));
        assert!(over(&t), "2-0 wins a Ranked set");
        assert!(matches!(read_all(&t.book.store, &t.book.store.entries()[0].id).last(), Some(LiveLine::Over)));
    }

    /// The only uploader stops mid-game (players first): spectators get the rest cut at once
    /// ("The rest of this game wasn't streamed.") instead of "Buffering…" until the next game; the
    /// row stays playing (the players are), and the wait between games starts. A pause doesn't cut.
    #[test]
    fn a_game_whose_only_uploader_stops_is_cut_at_once() {
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        t.offer(A, a, 1, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot, 0, &info);
        t.send_chunk(A, slot, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        let id = t.book.store.entries()[0].id.clone();
        let mut out = vec![];
        t.book.request(t.now, A, a, SpectateRequest::Pause { slot, reason: "rtt".into() }, &mut out);
        assert!(!read_all(&t.book.store, &id).iter().any(|l| matches!(l, LiveLine::Bad { .. })), "a pause comes back");
        t.book.request(t.now, A, a, SpectateRequest::Stop { slot, reason: "frame".into() }, &mut out);
        let lines = read_all(&t.book.store, &id);
        assert!(
            lines.iter().any(|l| matches!(l, LiveLine::Bad { game: 1, error } if error.contains("wasn't streamed"))),
            "{lines:?}"
        );
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Playing);
        // Then the wait between games (Direct: no set length) ends the stream.
        t.advance(Duration::from_secs(121));
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Over);
    }

    /// A player leaving the match (their chat group) ends its stream once no game is going on;
    /// a game under way still ends first. Leaving another match or a room changes nothing here.
    #[test]
    fn a_player_leaving_the_match_ends_its_stream() {
        let over = |t: &T| t.book.store.entries()[0].game.state == GameState::Over;
        let mut t = T::new(SpectateMode::On);
        play_game(&mut t, 1, r#"{"endMethod":"game","winnerPort":1}"#);
        let b = t.b;
        t.book.player_left(false, "another-match", b);
        t.book.player_left(true, "room-KFQB-1", b);
        t.advance(Duration::from_secs(1));
        assert!(!over(&t), "not this match");
        t.book.player_left(false, MATCH, b);
        t.advance(Duration::from_secs(1));
        assert!(over(&t), "between games: over at once");
        // During a game: over once it ends.
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        t.offer(A, a, 1, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot, 0, &info);
        t.send_chunk(A, slot, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        t.book.player_left(false, MATCH, t.b);
        t.advance(Duration::from_secs(5));
        assert!(!over(&t), "the game is still going");
        t.send_chunk(A, slot, 3, &encode_chunk(b"ENDG", br#"{"endMethod":"disconnect"}"#, false));
        t.advance(Duration::from_secs(1));
        assert!(over(&t));
    }

    /// A game after the match's stream ended starts a new stream: the ended one stays ended and is
    /// forgotten on time, never mid-game (it used to reopen with its forget timer still running).
    #[test]
    fn a_game_after_the_stream_ended_starts_a_new_stream() {
        let mut t = T::new(SpectateMode::On);
        play_game(&mut t, 1, r#"{"endMethod":"game","winnerPort":1}"#);
        t.advance(Duration::from_secs(121));
        let first = t.book.store.entries()[0].id.clone();
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Over);
        t.advance(Duration::from_secs(300));
        play_game(&mut t, 2, r#"{"endMethod":"game","winnerPort":2}"#);
        let entries = t.book.store.entries();
        assert_eq!(entries.len(), 2, "{entries:?}");
        let second = entries.iter().find(|e| e.id != first).expect("a new stream");
        assert_eq!(entries.iter().find(|e| e.id == first).unwrap().game.state, GameState::Over);
        assert_eq!(second.game.number, 2);
        assert_eq!(second.game.place, 1);
        // The first is forgotten 10 minutes after it ended; the second is still there.
        t.advance(Duration::from_secs(301));
        let ids: Vec<String> = t.book.store.entries().iter().map(|e| e.id.clone()).collect();
        assert!(!ids.contains(&first) && ids.len() == 1, "{ids:?}");
    }

    /// After a game, the next game of the match goes on in the same stream; 120 s with no game
    /// ends it; 10 minutes later it is gone.
    #[test]
    fn a_set_is_one_stream_until_it_ends() {
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        for game in 1..=2u8 {
            t.offer(A, a, game, 0);
            let out = t.advance(Duration::from_secs(3));
            let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
            let info = t.info(game);
            t.send_chunk(A, slot, 0, &info);
            t.send_chunk(A, slot, 1, &encode_chunk(b"SETP", b"{}", false));
            t.send_chunk(A, slot, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
            t.send_chunk(A, slot, 3, &encode_chunk(b"ENDG", b"{\"winnerPort\":1}", false));
        }
        let entries = t.book.store.entries();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].game.number, 2);
        assert_eq!(entries[0].game.place, 2, "a watch from ?game=2 starts with the game being played");
        assert_eq!(entries[0].players[0].wins, 2);
        let id = entries[0].id.clone();
        let lines = read_all(&t.book.store, &id);
        assert_eq!(lines.iter().filter(|l| matches!(l, LiveLine::End { .. })).count(), 2);
        t.advance(Duration::from_secs(121));
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Over);
        assert!(matches!(read_all(&t.book.store, &id).last(), Some(LiveLine::Over)));
        t.advance(Duration::from_secs(601));
        assert!(t.book.store.entries().is_empty());
        assert!(t.book.store.subscribe(&id).is_none());
    }

    /// The upload of game 1 stopped before its ENDG; game 2's offer closes it and streams.
    #[test]
    fn a_new_game_closes_an_unfinished_one() {
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        t.offer(A, a, 1, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot, 0, &info);
        t.send_chunk(A, slot, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        let out = t.offer(A, a, 2, 0);
        assert!(sent(&out, A).iter().all(|m| m["type"] != "spectate-error"), "{out:?}");
        let out = t.advance(Duration::from_secs(3));
        let slot2 = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let info2 = t.info(2);
        t.send_chunk(A, slot2, 0, &info2);
        let id = stream_id(&t);
        let lines = read_all(&t.book.store, &id);
        assert!(matches!(
            lines.iter().find(|l| matches!(l, LiveLine::Bad { .. })),
            Some(LiveLine::Bad { game: 1, .. })
        ));
        assert!(lines.iter().any(|l| matches!(l, LiveLine::Game { game: 2, number: 2, .. })));
    }

    #[test]
    fn a_reader_resumes_from_its_cursor() {
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        t.offer(A, a, 1, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot, 0, &info);
        t.send_chunk(A, slot, 1, &encode_chunk(b"SETP", b"{}", false));
        let id = stream_id(&t);
        let mut c = Cursor::default();
        let (lines, over) = t.book.store.read(&id, &mut c).unwrap();
        assert_eq!((lines.len(), over), (3, false));
        t.send_chunk(A, slot, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        let (lines, _) = t.book.store.read(&id, &mut c).unwrap();
        assert!(matches!(&lines[..], [LiveLine::Chunk { game: 1, i: 2, .. }]));
    }

    #[test]
    fn a_primary_that_disconnects_is_replaced() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        t.offer(B, b, 1, 0);
        let mut out = vec![];
        t.book.conn_gone(t.now, A, &mut out);
        assert_eq!(role(&out, B).unwrap()["role"], "primary");
        assert_eq!(role(&out, B).unwrap()["headers"], true);
    }
}
