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
//!
//! **Ranked** (2026-10-21): its games are always streamed, one player's game the primary and the
//! other's the witness, whatever `MM_SPECTATE` and the players' "Let others stream my games" say
//! ([`LiveConfig::ranked`], `MM_RANKED_STREAM`): it is the server's record of the set. Such a
//! stream is listed only under `on` when a player of it lets others watch (the offer's `listed`).
//! For accounts' check of a game only one player reported, mm records whether each player's game
//! is connected ([`Output::RankedPresence`], and since when it stopped running if it froze while
//! connected: the engine's `game-alive` check) and how each game ended as each player's own game
//! said ([`Output::RankedGameResult`]).

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::str::FromStr;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use chrono::{DateTime, SecondsFormat, Utc};
use common::spectate::{
    self, ChunkKind, EndReport, GameState, LiveEntry, LiveGame, LiveLine, LivePlayer, Offer, Role, RoleMsg,
    SpectateErrorMsg, SpectateRequest, SPECTATE_ERROR, SPECTATE_ROLE,
};
use tokio::sync::watch;
use uuid::Uuid;

use crate::engine::{ConnId, MatchRecord, Output};

/// A Ranked set's players' presence is recorded for this long after the set was made.
const RANKED_PRESENCE_FOR: Duration = Duration::from_secs(3 * 3600);

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
    /// Ranked games are always streamed (primary and witness), whatever `mode`; listed only under
    /// `On` (`MM_RANKED_STREAM`).
    pub ranked: bool,
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
    /// A primary that sent nothing this long, while a witness shows the game going on, is
    /// replaced.
    pub stall: Duration,
    pub max_games_per_match: usize,
}

/// A game's uploader sends nothing during the game's first 10 s of running (Dolphin's
/// `Online/Spectate.h` `baseline`): a primary without frames yet is stalled only once a witness
/// is this far into the game and a stall more.
const START_HOLD_FRAMES: u32 = 600;
/// With nobody to take its game over, a stalled primary keeps it this many stalls before the game
/// is cut.
const LONE_STALLS: u32 = 6;

impl Default for LiveConfig {
    fn default() -> Self {
        LiveConfig {
            mode: SpectateMode::Off,
            ranked: true,
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
    /// Not listed or served: a Ranked set streamed only for the server's record.
    hidden: bool,
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

    /// The listed streams, newest first. `Witness` streams are never in the store, hidden ones
    /// (a Ranked set nobody lets others watch) are not listed.
    pub fn entries(&self) -> Vec<LiveEntry> {
        let mut v: Vec<LiveEntry> = self.with(|m| m.values().filter(|s| !s.hidden).map(|s| s.entry.clone()).collect());
        v.sort_by(|a, b| b.started_at.cmp(&a.started_at).then_with(|| a.id.cmp(&b.id)));
        v
    }

    pub fn entry(&self, id: &str) -> Option<LiveEntry> {
        self.with(|m| m.get(id).filter(|s| !s.hidden).map(|s| s.entry.clone()))
    }

    pub fn subscribe(&self, id: &str) -> Option<watch::Receiver<u64>> {
        self.with(|m| m.get(id).filter(|s| !s.hidden).map(|s| s.tx.subscribe()))
    }

    /// The lines from `cursor` on, and whether the stream is over and fully read (the reader
    /// stops then). `None`: no such stream.
    pub fn read(&self, id: &str, cursor: &mut Cursor) -> Option<(Vec<LiveLine>, bool)> {
        use base64::Engine as _;
        self.with(|m| {
            let s = m.get(id).filter(|s| !s.hidden)?;
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
                hidden: true,
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

/// A match read back from the database when mm starts ([`LiveBook::restore_match`]).
#[derive(Debug, Clone, PartialEq)]
pub struct RestoredMatch {
    pub match_id: String,
    pub mode: common::proto::Mode,
    pub players: Vec<Uuid>,
    pub host: Uuid,
    /// A room's game: the room's code.
    pub room: Option<String>,
    /// How long ago it was made.
    pub age: Duration,
}

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
    /// The game's id and number as this player's game knows them (`spectate-offer`). Its roles
    /// name this id. Builds up to 0.1.72 numbered a room's games with a count of their own, which
    /// went into the id, so the players of one room game could offer different ids.
    uid: [u8; 16],
    number: u32,
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
    /// When the current primary got its role, or last sent a chunk.
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
    /// Its result is in the stream's score (from its `ENDG` or a player's `spectate-end`).
    counted: bool,
    /// A player's `spectate-end` that came while the upload was still going: counted when the
    /// upload ends (with its `ENDG`, or cut), so the list's score doesn't run ahead of what
    /// spectators have seen.
    reported: Option<EndResult>,
}

/// How a game ended, from its `ENDG` or a player's `spectate-end`.
#[derive(Debug, Clone, Default)]
struct EndResult {
    method: String,
    winner_port: Option<u32>,
    quitter_port: Option<u32>,
    teams: bool,
    /// Port (1-4) → the player's uid and team, as the game's `INFO` names them.
    ports: HashMap<u32, (Uuid, Option<u32>)>,
}

impl EndResult {
    fn from_endg(endg: Option<&serde_json::Value>) -> EndResult {
        let port = |k: &str| {
            endg.and_then(|e| e.get(k)).and_then(|p| p.as_u64()).and_then(|p| u32::try_from(p).ok())
        };
        EndResult {
            method: endg.and_then(|e| e.get("endMethod")).and_then(|m| m.as_str()).unwrap_or("").to_string(),
            winner_port: port("winnerPort"),
            quitter_port: port("quitterPort"),
            ..Default::default()
        }
    }

    fn from_report(e: EndReport) -> EndResult {
        EndResult {
            ports: e
                .players
                .iter()
                .filter_map(|p| Some((p.port, (Uuid::parse_str(&p.uid).ok()?, p.team_id))))
                .collect(),
            method: e.end_method,
            winner_port: e.winner_port,
            quitter_port: e.quitter_port,
            teams: e.is_teams,
        }
    }

    /// The players who won: the winning port's player and, in a team battle, their team.
    fn winners(&self, ranked: bool) -> Vec<Uuid> {
        let mut port = self.winner_port;
        // A Ranked quit loses the game for the quitter: the other player won it, whatever the
        // record says about a winner.
        if ranked && self.method == "quit" && self.ports.len() == 2 {
            if let Some(q) = self.quitter_port {
                port = self.ports.keys().copied().find(|&p| p != q).or(port);
            }
        }
        let Some((uid, team)) = port.and_then(|p| self.ports.get(&p)) else { return Vec::new() };
        match team.filter(|_| self.teams) {
            Some(t) => self.ports.values().filter(|(_, pt)| *pt == Some(t)).map(|(u, _)| *u).collect(),
            None => vec![*uid],
        }
    }
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
    /// A player of it lets others watch (`MM_SPECTATE` on): spectators may see it. Otherwise it
    /// is kept hidden (a Ranked set streamed only for the server's record).
    public: bool,
    last_upload: Instant,
    last_end: Option<Instant>,
    /// The set can't go on (a game ended by a disconnect, a player leaving or an error; a Ranked
    /// quit; a Ranked set won): the stream is over at the next tick, not `between_games` later.
    set_over: bool,
    over_at: Option<Instant>,
    wins: HashMap<Uuid, u32>,
    started_at: Option<String>,
    /// A room stream's latest game and its number: mm numbers a room's games itself (each
    /// player's game counts them its own way).
    last_game: Option<([u8; 16], u32)>,
}

impl StreamBook {
    fn new(now: Instant, room_key: Option<(String, Vec<Uuid>)>, players: Vec<Uuid>) -> StreamBook {
        StreamBook {
            room_key,
            players,
            listed: false,
            public: false,
            last_upload: now,
            last_end: None,
            set_over: false,
            over_at: None,
            wins: HashMap::new(),
            started_at: None,
            last_game: None,
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
    /// Room members watching their room's game on the watch channel ([`Self::feed_watchers`]).
    watchers: HashMap<ConnId, Watcher>,
    /// The last [`Output::RankedPresence`] time: each is later than the one before, so the
    /// database keeps the newest when they are written out of order.
    presence_at: DateTime<Utc>,
}

/// The most of a room's game sent to one watcher per [`LiveBook::feed_watchers`] (a tick): a
/// member who joined late gets the game so far within a few ticks.
const WATCH_BYTES_PER_FEED: usize = 64 * 1024;

/// A room member watching the room's games they are not in (`room-watch`, 2026-10-19,
/// `docs/rooms-protocol.md` §2): where the watch channel is in them.
#[derive(Debug, Clone, Default)]
struct Watcher {
    match_id: String,
    /// The room's next game, to go on with once this match has nothing more to send.
    next: Option<String>,
    /// The game being sent (its uid) and how many of its chunks went out; its `WATCH_BEGIN` did.
    game: Option<[u8; 16]>,
    chunk: usize,
    /// The match's games sent in full, or over before the watch began (a watcher starts with the
    /// game being played, not the match's earlier games).
    done: Vec<[u8; 16]>,
}

fn watch_packet(kind: u8, body: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(1 + body.len());
    v.push(kind);
    v.extend_from_slice(body);
    v
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
            watchers: HashMap::new(),
            presence_at: DateTime::<Utc>::MIN_UTC,
        }
    }

    pub fn store(&self) -> Arc<LiveStore> {
        self.store.clone()
    }

    pub fn mode(&self) -> SpectateMode {
        self.cfg.mode
    }

    fn presence_time(&mut self) -> DateTime<Utc> {
        let now = Utc::now();
        self.presence_at =
            if now > self.presence_at { now } else { self.presence_at + chrono::Duration::microseconds(1) };
        self.presence_at
    }

    /// `uid`'s game opened its first online connection, or closed its last one, or stopped
    /// running while connected (`stalled_since`: its last `game-alive`), or runs again: recorded
    /// for their recent Ranked sets.
    pub fn presence(
        &mut self,
        now: Instant,
        uid: Uuid,
        online: bool,
        stalled_since: Option<DateTime<Utc>>,
        out: &mut Vec<Output>,
    ) {
        let sets: Vec<String> = self
            .matches
            .iter()
            .filter(|(_, m)| {
                m.mode == "ranked" && m.players.contains(&uid) && now.duration_since(m.made) < RANKED_PRESENCE_FOR
            })
            .map(|(id, _)| id.clone())
            .collect();
        for match_id in sets {
            let at = self.presence_time();
            tracing::info!(match_id, %uid, online, stalled_since = ?stalled_since, "ranked presence");
            out.push(Output::RankedPresence { match_id, uid, online, stalled_since, at });
        }
    }

    /// Whether `match_id`'s games are streamed with a primary: spectating on, or a Ranked set.
    fn uploads(&self, match_id: &str) -> bool {
        self.cfg.mode == SpectateMode::On || self.forced(match_id)
    }

    /// A Ranked set streamed for the server's record, whatever `MM_SPECTATE` says.
    fn forced(&self, match_id: &str) -> bool {
        self.cfg.ranked && self.matches.get(match_id).is_some_and(|m| m.mode == "ranked")
    }

    pub fn stream_count(&self) -> usize {
        self.streams.len()
    }

    /// A match mm made (a pairing or a room's start). `names`: each player's name and connect code
    /// as mm knows them (from their online connection), which the list uses instead of what an
    /// upload says.
    /// A Ranked set's players' presence is recorded from here on ([`Self::presence`]): online when
    /// they have an online connection (`names` has them).
    pub fn register_match(
        &mut self,
        now: Instant,
        m: &MatchRecord,
        names: HashMap<Uuid, (String, String)>,
        out: &mut Vec<Output>,
    ) {
        if m.room.is_none() && m.mode == common::proto::Mode::Ranked {
            for p in &m.players {
                let at = self.presence_time();
                out.push(Output::RankedPresence {
                    match_id: m.match_id.clone(),
                    uid: *p,
                    online: names.contains_key(p),
                    stalled_since: None,
                    at,
                });
            }
        }
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

    /// A match an earlier mm made, read back from the database when this one started
    /// ([`common::db::recent_matches`]: mm keeps matches in memory only, and a restart, every
    /// deploy, forgot the sets under way, bug report #80). Offers for its games are taken again;
    /// its first one starts a new stream (its score so far is not known). A Ranked set's players'
    /// presence is recorded from here on, as for a new match: online when `names` has them (their
    /// game is back online), offline until it is. A match this mm knows already is left alone.
    pub fn restore_match(
        &mut self,
        now: Instant,
        r: RestoredMatch,
        names: HashMap<Uuid, (String, String)>,
        out: &mut Vec<Output>,
    ) {
        if self.matches.contains_key(&r.match_id) || r.age >= self.cfg.match_ttl {
            return;
        }
        let made = now.checked_sub(r.age).unwrap_or(now);
        if r.room.is_none() && r.mode == common::proto::Mode::Ranked && r.age < RANKED_PRESENCE_FOR {
            for p in &r.players {
                let at = self.presence_time();
                out.push(Output::RankedPresence {
                    match_id: r.match_id.clone(),
                    uid: *p,
                    online: names.contains_key(p),
                    stalled_since: None,
                    at,
                });
            }
        }
        let mode = if r.room.is_some() { "room" } else { r.mode.name() };
        // No stream yet: the next offer makes one (fresh_stream).
        self.matches.insert(
            r.match_id,
            MatchInfo { mode, room: r.room, players: r.players, host: r.host, names, made, stream: String::new() },
        );
    }

    /// `uid`'s game came online as `name` / `code`: the matches it plays in that didn't know its
    /// name yet (a match read back after a restart, made while it was offline) list it so.
    pub fn named(&mut self, uid: Uuid, name: &str, code: &str) {
        for m in self.matches.values_mut().filter(|m| m.players.contains(&uid)) {
            m.names.entry(uid).or_insert_with(|| (name.to_string(), code.to_string()));
        }
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
    /// room, and the match's score and game count so far: it is the same set going on.
    fn fresh_stream(&mut self, now: Instant, match_id: &str) -> String {
        let id = new_stream_id();
        let old = self.matches.get(match_id).and_then(|m| self.streams.get(&m.stream));
        let players = self.matches.get(match_id).map(|m| m.players.clone()).unwrap_or_default();
        // A match read back after a restart has no stream before this one: its room's key is made
        // as register_match makes it.
        let room_key = old.map(|b| b.room_key.clone()).unwrap_or_else(|| {
            self.matches.get(match_id).and_then(|m| m.room.clone()).map(|code| {
                let mut sorted = players.clone();
                sorted.sort_unstable();
                (code, sorted)
            })
        });
        let mut book = StreamBook::new(now, room_key, players);
        if let Some(b) = old {
            book.wins = b.wins.clone();
            book.last_game = b.last_game;
        }
        self.streams.insert(id.clone(), book);
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

    /// `uid`: the game's id as the receiving player's game knows it.
    fn role_msg(&self, uid: &[u8; 16], role: Role, slot: u32, from: Option<u32>, headers: bool) -> RoleMsg {
        RoleMsg {
            kind: SPECTATE_ROLE.into(),
            game_uid: hex::encode(uid),
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
    /// Match `match_id`'s mode as speed reports name it (`room` for a room's game), when `account`
    /// plays in it.
    pub fn speed_mode(&self, match_id: &str, account: Uuid) -> Option<&'static str> {
        let m = self.matches.get(match_id).filter(|m| m.players.contains(&account))?;
        Some(if m.room.is_some() { "room" } else { m.mode })
    }

    pub fn request(&mut self, now: Instant, conn: ConnId, account: Uuid, req: SpectateRequest, out: &mut Vec<Output>) {
        let op = req.op();
        let result = match req {
            SpectateRequest::Offer(o) => self.offer(now, conn, account, o, out),
            SpectateRequest::End(e) => self.end_report(now, account, e, out),
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
        let offered = spectate::decode_uid(&o.game_uid).ok_or(errors::INVALID)?;
        let Some(m) = self.matches.get(&o.match_id).filter(|m| now.duration_since(m.made) < self.cfg.match_ttl) else {
            return Err(errors::NOT_YOUR_MATCH);
        };
        if !m.players.contains(&account) {
            return Err(errors::NOT_YOUR_MATCH);
        }
        let forced = self.forced(&o.match_id);
        if self.cfg.mode == SpectateMode::Off && !forced {
            self.send_none(out, conn, &offered);
            return Ok(());
        }
        let host = m.host == account;
        let mut stream = m.stream.clone();
        let n_players = m.players.len();
        let is_room = m.room.is_some();
        let uid = self.game_for_offer(&o.match_id, account, offered);
        let offerer = Offerer {
            conn,
            account,
            uid: offered,
            number: o.game,
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
            let number =
                if is_room { self.room_game_number(&stream, &o.match_id, account, o.game, uid) } else { o.game };
            // A game is going on: the wait between games is over, whether or not anything of
            // this game is ever uploaded.
            if let Some(b) = self.streams.get_mut(&stream) {
                b.last_end = None;
                b.last_upload = now;
            }
            self.games.insert(
                uid,
                Game {
                    match_id: o.match_id.clone(),
                    stream,
                    number,
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
                    counted: false,
                    reported: None,
                },
            );
        }
        let game = self.games.get_mut(&uid).expect("just made");
        if game.match_id != o.match_id {
            return Err(errors::NOT_YOUR_MATCH);
        }
        // Spectators may see the stream once a player of it lets others watch.
        if o.listed && self.cfg.mode == SpectateMode::On {
            let stream = game.stream.clone();
            if let Some(b) = self.streams.get_mut(&stream).filter(|b| !b.public) {
                b.public = true;
                self.store.change(&stream, |s| s.hidden = false);
            }
        }
        let game = self.games.get_mut(&uid).expect("just made");
        if !game.open() {
            self.send_none(out, conn, &offered);
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

    /// The game an offer is for: the game with that id (or one this player already offered under
    /// that id), else the match's open game when this player hasn't offered for it, else a new
    /// game (`offered`). The players of a match play its games together, one at a time, so a
    /// player's first offer while the match's game is open is for that game, whatever id its game
    /// gave it: builds up to 0.1.72 put their own count of a room's games into the id, and two
    /// players whose counts differed (one had played in another room, or joined after the room's
    /// first game) were taken for two games, each with its own primary and no witness (production
    /// 2026-10-14, room TLSZ: games 4 and 1, then 5 and 2). The pairing never reaches another
    /// match, and a second id from a player who already offered for the open game is a new game,
    /// as before.
    fn game_for_offer(&self, match_id: &str, account: Uuid, offered: [u8; 16]) -> [u8; 16] {
        if self.games.contains_key(&offered) {
            return offered;
        }
        let of_match = || self.games.values().filter(|g| g.match_id == match_id);
        if let Some(g) = of_match().find(|g| g.offers.iter().any(|x| x.account == account && x.uid == offered)) {
            return g.uid;
        }
        match of_match().find(|g| g.open() && !g.offers.iter().any(|x| x.account == account)) {
            Some(g) => {
                tracing::info!(match_id, game = g.number, "spectate: an offer with another id joins the open game");
                g.uid
            }
            None => offered,
        }
    }

    /// A room game's number: the next in its stream, or the stream's latest game's again when it
    /// is that game played again (a set's game after a desync: the player's game gave it the same
    /// number as last time, in the same match).
    fn room_game_number(&mut self, stream: &str, match_id: &str, account: Uuid, offered: u32, uid: [u8; 16]) -> u32 {
        let Some(book) = self.streams.get(stream) else { return 1 };
        let number = match book.last_game {
            None => 1,
            Some((last, n)) => {
                let again = self.games.get(&last).is_some_and(|g| {
                    g.match_id == match_id && g.offers.iter().any(|x| x.account == account && x.number == offered)
                });
                if again {
                    n
                } else {
                    n + 1
                }
            }
        };
        if let Some(book) = self.streams.get_mut(stream) {
            book.last_game = Some((uid, number));
        }
        number
    }

    /// Gives every offering player its role: the healthiest is the primary (fewest pauses last
    /// game, then fewest rollbacks, then the host), the others witnesses.
    fn decide(&mut self, now: Instant, uid: [u8; 16], out: &mut Vec<Output>) {
        let witness_only = self.games.get(&uid).is_some_and(|g| !self.uploads(&g.match_id));
        let Some(game) = self.games.get_mut(&uid) else { return };
        game.decided = true;
        let mut order: Vec<usize> = (0..game.offers.len()).collect();
        order.sort_by(|&a, &b| {
            let (x, y) = (&game.offers[a], &game.offers[b]);
            x.paused.cmp(&y.paused).then(x.rollbacks.total_cmp(&y.rollbacks)).then(y.host.cmp(&x.host))
        });
        for (rank, i) in order.into_iter().enumerate() {
            let role = if rank == 0 && !witness_only { Role::Primary } else { Role::Witness };
            self.give_role(now, uid, i, role, out);
        }
    }

    /// An offer after the roles were given: a witness, or the primary when the game has none.
    fn assign_late(&mut self, now: Instant, uid: [u8; 16], conn: ConnId, out: &mut Vec<Output>) {
        let Some(game) = self.games.get(&uid) else { return };
        let Some(i) = game.offers.iter().position(|o| o.conn == conn) else { return };
        let role = if game.primary.is_none() && self.uploads(&game.match_id) { Role::Primary } else { Role::Witness };
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
        let msg = self.role_msg(&game.offers[i].uid, role, slot, from, headers);
        send(out, conn, &msg);
        tracing::info!(match_id = %game.match_id, game = game.number, slot, ?role, ?from, "spectate role");
    }

    /// Hands the game to the healthiest witness (or a standby that resumed). The old primary, if
    /// still there, was already told. Without anyone left the game waits for a late offer.
    fn takeover(&mut self, now: Instant, uid: [u8; 16], why: &str, out: &mut Vec<Output>) {
        let uploads = self.games.get(&uid).is_some_and(|g| self.uploads(&g.match_id));
        let Some(game) = self.games.get_mut(&uid) else { return };
        game.primary = None;
        if !game.open() || !uploads {
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
    /// this game wasn't streamed.". The list's row is not "between" and the wait between games
    /// doesn't start: the players are still playing it. Its end comes from the players' games
    /// (`spectate-end`), which count its result and start the wait.
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
        self.slots.retain(|_, s| s.game != uid);
        self.count_reported(now, uid);
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
        // Any chunk is progress: the headers come before the first frames, and a primary whose
        // headers just arrived is not stalled (production 2026-10-14: 435 of 572 games were taken
        // from their primary the moment its headers landed, 12-14 s in).
        game.progress = now;
        if kind.is_header() {
            game.have.push(kind);
        }
        let mut mismatch = None;
        if let Some((first, count, digest)) = frames {
            game.next_frame = Some(first + count);
            game.hashes.insert(first, digest);
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
        let (players, wins, started_at, public) = match book {
            Some(b) => {
                let started = b.started_at.get_or_insert_with(|| rfc3339(wall)).clone();
                b.listed = true;
                b.last_end = None;
                (b.players.clone(), b.wins.clone(), started, b.public)
            }
            None => (Vec::new(), HashMap::new(), rfc3339(wall), false),
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
                    rating: None,
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
            s.hidden = !public;
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
        self.store.change(&stream_id, |s| {
            if let Some(g) = ordinal.and_then(|o| s.games.get_mut(o)) {
                g.ended = true;
            }
        });
        // The game's slots are done.
        self.slots.retain(|_, s| s.game != uid);
        // The upload's own record; a player's report that came first is the same game's end.
        if let Some(g) = self.games.get_mut(&uid) {
            g.reported = None;
        }
        let (teams, ports) = self.info_ports(&stream_id, ordinal);
        self.count_result(now, uid, EndResult { teams, ports, ..EndResult::from_endg(endg) });
    }

    /// A player's game says how a game it offered ended (`spectate-end`). Counted at once when
    /// the game's upload is over (cut, or nothing came); while it still goes on, its `ENDG` is on
    /// the way and counts, and the report waits in case it never comes.
    fn end_report(
        &mut self,
        now: Instant,
        account: Uuid,
        e: EndReport,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        let offered = spectate::decode_uid(&e.game_uid).ok_or(errors::INVALID)?;
        let Some(m) = self.matches.get(&e.match_id) else { return Ok(()) };
        if !m.players.contains(&account) {
            return Err(errors::NOT_YOUR_MATCH);
        }
        // A Ranked game's result as this player's own game says it, for accounts' check of a
        // game only one player reported. A desync, a disconnect and the like are no result.
        if let Some(game_index) = e.set_game.filter(|_| m.mode == "ranked" && m.room.is_none()) {
            if matches!(e.end_method.as_str(), "game" | "quit") {
                let winners = EndResult::from_report(e.clone()).winners(true);
                let winner = winners.into_iter().find(|w| m.players.contains(w));
                tracing::info!(match_id = %e.match_id, game_index, %account, ?winner, "ranked game result from a player's game");
                out.push(Output::RankedGameResult {
                    match_id: e.match_id.clone(),
                    game_index,
                    uid: account,
                    winner,
                    end_method: e.end_method.clone(),
                });
            }
        }
        // Only a game this player offered (spectating off: there is none, and nothing to count).
        let Some(game) = self
            .games
            .values_mut()
            .find(|g| g.match_id == e.match_id && g.offers.iter().any(|o| o.account == account && o.uid == offered))
        else {
            return Ok(());
        };
        let uid = game.uid;
        if let Some(b) = self.streams.get_mut(&game.stream) {
            b.last_upload = now;
        }
        if game.counted {
            return Ok(());
        }
        let result = EndResult::from_report(e);
        if game.open() {
            game.reported.get_or_insert(result);
            return Ok(());
        }
        self.count_result(now, uid, result);
        Ok(())
    }

    /// Counts the report a game's upload was waiting on, now that the upload is over.
    fn count_reported(&mut self, now: Instant, uid: [u8; 16]) {
        if let Some(r) = self.games.get_mut(&uid).and_then(|g| g.reported.take()) {
            self.count_result(now, uid, r);
        }
    }

    /// A game's end, once: its winners' wins, the wait between games, and whether the set can go
    /// on. The players come from the game's kept `INFO`, else from the report (players of the
    /// match only).
    fn count_result(&mut self, now: Instant, uid: [u8; 16], mut r: EndResult) {
        let Some(game) = self.games.get_mut(&uid) else { return };
        if game.counted {
            return;
        }
        game.counted = true;
        game.reported = None;
        let (stream_id, ordinal, number) = (game.stream.clone(), game.ordinal, game.number);
        let (mode, players) =
            self.matches.get(&game.match_id).map_or(("", Vec::new()), |m| (m.mode, m.players.clone()));
        let (teams, kept) = self.info_ports(&stream_id, ordinal);
        if !kept.is_empty() {
            (r.teams, r.ports) = (teams, kept);
        }
        r.ports.retain(|_, (u, _)| players.contains(u));
        let winners = r.winners(mode == "ranked");
        tracing::info!(stream = %stream_id, game = number, method = r.method, ?winners, "spectate: game result");
        if let Some(b) = self.streams.get_mut(&stream_id) {
            b.last_end = Some(now);
            for w in &winners {
                *b.wins.entry(*w).or_default() += 1;
            }
            let best = b.wins.values().copied().max().unwrap_or(0);
            if ends_stream(mode, &r.method, best) {
                let method = &r.method;
                tracing::info!(stream = %stream_id, mode, method, best, "spectate: the set can't go on; stream over");
                b.set_over = true;
            }
        }
        let winners: Vec<String> = winners.iter().map(|u| u.to_string()).collect();
        self.store.change(&stream_id, |s| {
            // The row shows this game (a game never listed leaves the last one there, ended).
            if ordinal.is_some_and(|o| o + 1 == s.games.len()) {
                s.entry.game.state = GameState::Between;
            }
            for p in s.entry.players.iter_mut() {
                if winners.contains(&p.uid) {
                    p.wins += 1;
                }
            }
        });
    }

    /// Whether the game's kept `INFO` is a team battle, and its players by port (1-4): uid and
    /// team. Empty without a kept `INFO`.
    fn info_ports(&self, stream_id: &str, ordinal: Option<usize>) -> (bool, HashMap<u32, (Uuid, Option<u32>)>) {
        let mut map = HashMap::new();
        let mut teams = false;
        let Some(o) = ordinal else { return (teams, map) };
        self.store.with(|m| {
            let Some(g) = m.get(stream_id).and_then(|s| s.games.get(o)) else { return };
            for c in &g.chunks {
                let Ok(v) = spectate::parse_chunk(c) else { continue };
                if v.kind != ChunkKind::Info {
                    continue;
                }
                let Ok(raw) = v.raw() else { continue };
                let Ok(j) = serde_json::from_slice::<serde_json::Value>(&raw) else { continue };
                teams = j.get("isTeams").and_then(|t| t.as_bool()).unwrap_or(false);
                for p in j.get("players").and_then(|p| p.as_array()).into_iter().flatten() {
                    let uid = p.get("uid").and_then(|x| x.as_str()).and_then(|u| Uuid::parse_str(u).ok());
                    if let (Some(port), Some(uid)) = (p.get("port").and_then(|x| x.as_u64()), uid) {
                        let team = p.get("teamId").and_then(|t| t.as_u64()).map(|t| t as u32);
                        map.insert(port as u32, (uid, team));
                    }
                }
            }
        });
        (teams, map)
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
        self.count_reported(now, uid);
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
        // The players play on: the wait between games starts at the game's end (its report).
        self.slots.retain(|_, s| s.game != uid);
        self.count_reported(now, uid);
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
        self.watchers.remove(&conn);
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

    /// Room members watching their room's game (`targets`: each one's connection and the match of
    /// the room's game going on; `stops`: watches that stopped): every kept chunk of that game goes
    /// to them on the watch channel as it comes, the game's start and end around them. A watcher
    /// starts with the game being played (its chunks so far first), goes on to the match's next
    /// game (a room's set), and to the room's next game once the one it watches is sent in full:
    /// the upload runs behind the players, so a game's end may come after the room's next game
    /// started. Nothing comes for a game that isn't streamed (`MM_SPECTATE` off, every player's
    /// "Let others stream my games" off): its watchers just wait.
    pub fn feed_watchers(&mut self, targets: &[(ConnId, String)], stops: &[ConnId], out: &mut Vec<Output>) {
        for conn in stops {
            self.watchers.remove(conn);
        }
        for (conn, match_id) in targets {
            match self.watchers.get_mut(conn) {
                Some(w) if w.match_id == *match_id => {}
                Some(w) => w.next = Some(match_id.clone()),
                None => {
                    let done = self.games_of(match_id).into_iter().filter(|g| !self.game_open(g)).collect();
                    self.watchers
                        .insert(*conn, Watcher { match_id: match_id.clone(), done, ..Default::default() });
                }
            }
        }
        let conns: Vec<ConnId> = self.watchers.keys().copied().collect();
        for conn in conns {
            self.feed_watcher(conn, out);
        }
    }

    /// The games of a match that have chunks kept, in the order they were played.
    fn games_of(&self, match_id: &str) -> Vec<[u8; 16]> {
        let mut v: Vec<&Game> =
            self.games.values().filter(|g| g.match_id == match_id && g.ordinal.is_some()).collect();
        v.sort_by_key(|g| g.first_offer);
        v.into_iter().map(|g| g.uid).collect()
    }

    /// The game's chunks still come (in the store, neither ended nor bad).
    fn game_open(&self, uid: &[u8; 16]) -> bool {
        let Some(g) = self.games.get(uid) else { return false };
        let Some(o) = g.ordinal else { return true };
        self.store
            .with(|m| m.get(&g.stream).and_then(|s| s.games.get(o)).is_some_and(|v| !v.ended && v.bad.is_none()))
    }

    fn feed_watcher(&mut self, conn: ConnId, out: &mut Vec<Output>) {
        let mut budget = WATCH_BYTES_PER_FEED;
        loop {
            let Some(w) = self.watchers.get(&conn) else { return };
            let current = match w.game {
                Some(uid) => uid,
                None => {
                    let next_game = self.games_of(&w.match_id).into_iter().find(|g| !w.done.contains(g));
                    match (next_game, w.next.clone()) {
                        (Some(uid), _) => {
                            let number = self.games.get(&uid).map_or(0, |g| g.number);
                            let begin = serde_json::json!({
                                "matchId": w.match_id, "number": number, "uid": hex::encode(uid),
                            });
                            out.push(Output::SendData {
                                conn,
                                channel: spectate::WATCH_CHANNEL,
                                data: watch_packet(spectate::WATCH_BEGIN, begin.to_string().as_bytes()),
                            });
                            let w = self.watchers.get_mut(&conn).expect("present");
                            w.game = Some(uid);
                            w.chunk = 0;
                            uid
                        }
                        (None, Some(next)) => {
                            // This match has nothing more: the room's next game.
                            let done = self.games_of(&next).into_iter().filter(|g| !self.game_open(g)).collect();
                            self.watchers.insert(conn, Watcher { match_id: next, done, ..Default::default() });
                            continue;
                        }
                        (None, None) => return,
                    }
                }
            };
            let (stream, ordinal) = match self.games.get(&current) {
                Some(g) => (g.stream.clone(), g.ordinal),
                None => (String::new(), None),
            };
            let from = self.watchers.get(&conn).map_or(0, |w| w.chunk);
            // The chunks not sent yet, and how the game stands (gone from the store: over).
            let (chunks, ended, bad) = self.store.with(|m| {
                match m.get(&stream).and_then(|s| ordinal.and_then(|o| s.games.get(o))) {
                    Some(v) => (v.chunks.get(from..).unwrap_or_default().to_vec(), v.ended, v.bad.clone()),
                    None => (Vec::new(), false, Some("This game is no longer kept.".to_string())),
                }
            });
            let mut sent = 0;
            for c in &chunks {
                if budget < c.len() && sent > 0 {
                    break;
                }
                let mut data = Vec::with_capacity(5 + c.len());
                data.push(spectate::WATCH_CHUNK);
                data.extend_from_slice(&((from + sent) as u32).to_le_bytes());
                data.extend_from_slice(c);
                out.push(Output::SendData { conn, channel: spectate::WATCH_CHANNEL, data });
                budget = budget.saturating_sub(c.len());
                sent += 1;
            }
            let w = self.watchers.get_mut(&conn).expect("present");
            w.chunk += sent;
            if sent < chunks.len() || !(ended || bad.is_some()) {
                return;
            }
            let mut end = serde_json::json!({ "matchId": w.match_id, "uid": hex::encode(current) });
            if let Some(why) = bad {
                end["bad"] = serde_json::json!(why);
            }
            out.push(Output::SendData {
                conn,
                channel: spectate::WATCH_CHANNEL,
                data: watch_packet(spectate::WATCH_END, end.to_string().as_bytes()),
            });
            w.done.push(current);
            w.game = None;
            w.chunk = 0;
        }
    }

    pub fn tick(&mut self, now: Instant, out: &mut Vec<Output>) {
        // Roles for games whose other players never offered.
        let due: Vec<[u8; 16]> = self
            .games
            .values()
            .filter(|g| !g.decided && g.open() && now.duration_since(g.first_offer) >= self.cfg.decide_after)
            .map(|g| g.uid)
            .collect();
        for uid in due {
            self.decide(now, uid, out);
        }
        // A primary that went quiet while a witness shows the game going on.
        let stall_frames = (self.cfg.stall.as_secs_f64() * 60.0) as u32;
        let stalled: Vec<(u32, [u8; 16], ConnId)> = self
            .games
            .values()
            .filter(|g| g.open() && now.duration_since(g.progress) >= self.cfg.stall)
            .filter(|g| match (g.witness_frame, g.next_frame) {
                (Some(w), Some(n)) => w >= n,
                (Some(w), None) => w >= START_HOLD_FRAMES + stall_frames,
                _ => false,
            })
            .filter_map(|g| g.primary.map(|p| (p, g.uid)))
            .filter_map(|(p, uid)| self.slots.get(&p).map(|s| (p, uid, s.conn)))
            .collect();
        for (slot, uid, conn) in stalled {
            let Some(g) = self.games.get_mut(&uid) else { continue };
            let Some(i) = g.offers.iter().position(|o| o.slot == Some(slot)) else { continue };
            // With nobody to take over, the primary keeps the game a while: it may only be held
            // up (its uploader waits out a queue on its uplink without pausing), and a cut ends
            // the game for every spectator.
            let other = g.offers.iter().enumerate().any(|(j, o)| j != i && !o.out && (!o.standby || o.resumable));
            if !other && now.duration_since(g.progress) < self.cfg.stall * LONE_STALLS {
                continue;
            }
            let o = &mut g.offers[i];
            o.standby = true;
            o.resumable = false;
            o.slot = None;
            self.slots.remove(&slot);
            self.takeover(now, uid, "primary stalled", out);
            if self.games.get(&uid).is_some_and(|g| g.open() && g.primary.is_some()) {
                // The stalled player's game sends digests again: they show whether the new
                // primary keeps up, and a later takeover can hand the game back to it.
                self.give_role(now, uid, i, Role::Witness, out);
            } else {
                self.send_none(out, conn, &uid);
            }
        }
        // Streams: over between games or idle, then forgotten.
        let mut over = Vec::new();
        let mut forget = Vec::new();
        for (id, b) in self.streams.iter_mut() {
            match b.over_at {
                Some(t) if now.duration_since(t) >= self.cfg.keep => forget.push(id.clone()),
                Some(_) => {}
                None => {
                    // A game going on holds the stream, listed or not (its upload may not have
                    // started, or was cut): its end, reported by the players' games, starts the
                    // wait. Without one (older games) the idle timeout ends it.
                    let open = self.games.values().any(|g| &g.stream == id && g.open());
                    let between = !open
                        && (b.set_over || b.last_end.is_some_and(|t| now.duration_since(t) >= self.cfg.between_games));
                    let idle = now.duration_since(b.last_upload) >= self.cfg.idle;
                    if between || idle {
                        b.over_at = Some(now);
                        over.push(id.clone());
                    }
                }
            }
        }
        for id in over {
            // Ended games' reports waiting on their uploads count first.
            let waiting: Vec<[u8; 16]> =
                self.games.values().filter(|g| g.stream == id && g.reported.is_some()).map(|g| g.uid).collect();
            for uid in waiting {
                self.count_reported(now, uid);
            }
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
            T::with_config(LiveConfig { mode, ..Default::default() }, match_mode)
        }

        fn with_config(cfg: LiveConfig, match_mode: Mode) -> T {
            let mut book = LiveBook::new(cfg);
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
            book.register_match(now, &rec, names, &mut vec![]);
            T { book, now, a, b }
        }

        fn offer(&mut self, conn: ConnId, who: Uuid, game: u8, paused: u32) -> Vec<Output> {
            self.offer_with(conn, who, game, paused, true)
        }

        /// An offer saying whether the player lets others watch.
        fn offer_with(&mut self, conn: ConnId, who: Uuid, game: u8, paused: u32, listed: bool) -> Vec<Output> {
            let mut out = vec![];
            let o = Offer {
                match_id: MATCH.into(),
                game_uid: uid_hex(game),
                game: game as u32,
                paused,
                rollbacks: 0.0,
                listed,
            };
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

    /// Ranked (2026-10-21): a set is streamed, one primary and one witness, whatever `MM_SPECTATE`
    /// and the players' "Let others stream my games" say; it is listed only when spectating is on
    /// and a player of it lets others watch.
    #[test]
    fn ranked_sets_stream_for_the_record_and_are_listed_only_when_a_player_lets_others_watch() {
        // (spectating, A lets others watch, B lets others watch) -> listed
        let cases = [
            (SpectateMode::Off, true, true, false),
            (SpectateMode::Witness, true, true, false),
            (SpectateMode::On, false, false, false),
            (SpectateMode::On, false, true, true),
        ];
        for (mode, la, lb, listed) in cases {
            let mut t = T::with_match_mode(mode, Mode::Ranked);
            let (a, b) = (t.a, t.b);
            t.offer_with(A, a, 1, 0, la);
            let out = t.offer_with(B, b, 1, 1, lb);
            let (ra, rb) = (role(&out, A).unwrap(), role(&out, B).unwrap());
            assert_eq!((ra["role"].as_str(), rb["role"].as_str()), (Some("primary"), Some("witness")), "{mode:?}");
            let slot = ra["slot"].as_u64().unwrap() as u32;
            let info = t.info(1);
            t.send_chunk(A, slot, 0, &info);
            t.send_chunk(A, slot, 1, &encode_chunk(b"SETP", b"{}", false));
            t.send_chunk(A, slot, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
            let id = t.book.matches[MATCH].stream.clone();
            assert_eq!(t.book.store.entries().len(), usize::from(listed), "{mode:?} {la} {lb}");
            assert_eq!(t.book.store.entry(&id).is_some(), listed);
            assert_eq!(t.book.store.read(&id, &mut Cursor::default()).is_some(), listed);
            // Kept either way: the witness's digests are checked against it.
            assert_eq!(t.book.games[&[1u8; 16]].hashes.len(), 1);
        }
        // Not Ranked: as before, spectating off streams nothing.
        let mut t = T::with_match_mode(SpectateMode::Off, Mode::Unranked);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        assert_eq!(role(&t.offer(B, b, 1, 0), B).unwrap()["role"], "none");
        // MM_RANKED_STREAM off: Ranked follows MM_SPECTATE.
        let cfg = LiveConfig { mode: SpectateMode::Off, ranked: false, ..Default::default() };
        let mut t = T::with_config(cfg, Mode::Ranked);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        assert_eq!(role(&t.offer(B, b, 1, 0), B).unwrap()["role"], "none");
    }

    fn presence(out: &[Output]) -> Vec<(Uuid, bool)> {
        out.iter()
            .filter_map(|o| match o {
                Output::RankedPresence { uid, online, .. } => Some((*uid, *online)),
                _ => None,
            })
            .collect()
    }

    fn results(out: &[Output]) -> Vec<(u32, Uuid, Option<Uuid>)> {
        out.iter()
            .filter_map(|o| match o {
                Output::RankedGameResult { game_index, uid, winner, .. } => Some((*game_index, *uid, *winner)),
                _ => None,
            })
            .collect()
    }

    /// What accounts checks a lone report against: whether each player's game is connected, and
    /// each game's result as each player's own game said it.
    #[test]
    fn ranked_sets_record_presence_and_each_players_result() {
        let mut book = LiveBook::new(LiveConfig::default());
        let (a, b) = (Uuid::new_v4(), Uuid::new_v4());
        let now = Instant::now();
        let rec = |id: &str, mode: Mode| MatchRecord {
            match_id: id.into(),
            mode,
            players: vec![a, b],
            host: a,
            stages: vec![],
            region: String::new(),
            room: None,
        };
        // Only A has an online connection when the set is made.
        let names = HashMap::from([(a, ("alice".into(), "ALI#1".into()))]);
        let mut out = vec![];
        book.register_match(now, &rec(MATCH, Mode::Ranked), names.clone(), &mut out);
        assert_eq!(presence(&out), vec![(a, true), (b, false)]);
        let mut out = vec![];
        book.register_match(now, &rec("mode.unranked-x", Mode::Unranked), names, &mut out);
        assert!(presence(&out).is_empty(), "only Ranked");
        // Later changes go to the recent Ranked sets only, each newer than the last.
        let mut out = vec![];
        book.presence(now, b, true, None, &mut out);
        book.presence(now, b, false, None, &mut out);
        assert_eq!(presence(&out), vec![(b, true), (b, false)]);
        let at: Vec<DateTime<Utc>> = out
            .iter()
            .filter_map(|o| match o {
                Output::RankedPresence { at, .. } => Some(*at),
                _ => None,
            })
            .collect();
        assert!(at[0] < at[1]);
        let mut out = vec![];
        book.presence(now + RANKED_PRESENCE_FOR, b, true, None, &mut out);
        assert!(presence(&out).is_empty(), "an old set");

        // Each player's game's word on how a game ended.
        let mut t = T::with_match_mode(SpectateMode::On, Mode::Ranked);
        let (a, b) = (t.a, t.b);
        t.offer_with(A, a, 1, 0, false);
        t.offer_with(B, b, 1, 0, false);
        let out = report(&mut t, B, b, 1, json!({"endMethod": "game", "winnerPort": 1, "setGame": 2}));
        assert_eq!(results(&out), vec![(2, b, Some(a))]);
        // A quit loses the game for the quitter.
        let out = report(&mut t, A, a, 1, json!({"endMethod": "quit", "quitterPort": 2, "setGame": 2}));
        assert_eq!(results(&out), vec![(2, a, Some(a))]);
        // A draw.
        let out = report(&mut t, A, a, 1, json!({"endMethod": "game", "setGame": 2}));
        assert_eq!(results(&out), vec![(2, a, None)]);
        // No result: a desync, a disconnect, an older build without the set game.
        for fields in [
            json!({"endMethod": "desync", "setGame": 2}),
            json!({"endMethod": "disconnect", "winnerPort": 1, "setGame": 2}),
            json!({"endMethod": "game", "winnerPort": 1}),
        ] {
            assert!(results(&report(&mut t, B, b, 1, fields)).is_empty());
        }
        // Not Ranked: nothing.
        let mut t = T::with_match_mode(SpectateMode::On, Mode::Direct);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = report(&mut t, B, b, 1, json!({"endMethod": "game", "winnerPort": 1, "setGame": 1}));
        assert!(results(&out).is_empty());
    }

    /// Bug report #80: mm restarted during a set (a deploy), and every offer of the set's games
    /// from then on was refused ("That match isn't yours to stream"). The matches read back from
    /// the database take offers again, and a Ranked set's presence is recorded again.
    #[test]
    fn matches_read_back_after_a_restart_take_offers_again() {
        let restored = |id: &str, mode: Mode, players: Vec<Uuid>, room: Option<&str>, age: u64| RestoredMatch {
            match_id: id.into(),
            mode,
            players: players.clone(),
            host: players[0],
            room: room.map(str::to_string),
            age: Duration::from_secs(age),
        };
        // A fresh book: mm restarted. Only A's game is online again when the matches are read.
        let mut t = T::new(SpectateMode::On);
        t.book = LiveBook::new(LiveConfig { mode: SpectateMode::On, ..Default::default() });
        let (a, b) = (t.a, t.b);
        let names = HashMap::from([(a, ("alice".into(), "ALI#1".into()))]);
        let mut out = vec![];
        t.book.restore_match(t.now, restored(MATCH, Mode::Ranked, vec![a, b], None, 1800), names.clone(), &mut out);
        assert_eq!(presence(&out), vec![(a, true), (b, false)]);
        // Not Ranked, or a Ranked set older than RANKED_PRESENCE_FOR: no presence.
        let mut out = vec![];
        t.book.restore_match(
            t.now,
            restored("mode.unranked-x-1", Mode::Unranked, vec![a, b], None, 60),
            names.clone(),
            &mut out,
        );
        let old = RANKED_PRESENCE_FOR.as_secs() + 1;
        t.book.restore_match(
            t.now,
            restored("mode.ranked-x-2", Mode::Ranked, vec![a, b], None, old),
            names.clone(),
            &mut out,
        );
        assert!(presence(&out).is_empty());
        // B's game is back online: its presence, and its name in the list.
        let mut out = vec![];
        t.book.named(b, "bob", "BOB#2");
        t.book.presence(t.now, b, true, None, &mut out);
        assert_eq!(presence(&out), vec![(b, true)], "the recent Ranked set only");

        // The game under way is offered again: roles as for any match, a new stream with mm's names.
        t.offer(A, a, 3, 0);
        let out = t.offer(B, b, 3, 1);
        let ra = role(&out, A).unwrap();
        assert_eq!((ra["role"].as_str(), ra["headers"].as_bool()), (Some("primary"), Some(true)));
        assert_eq!(role(&out, B).unwrap()["role"], "witness");
        let slot = ra["slot"].as_u64().unwrap() as u32;
        t.send_chunk(A, slot, 0, &t.info(3));
        let e = &t.book.store.entries()[0];
        assert_eq!((e.mode.as_str(), e.game.number), ("ranked", 3));
        let name = |u: Uuid| e.players.iter().find(|p| p.uid == u.to_string()).unwrap().display_name.clone();
        assert_eq!((name(a), name(b)), ("alice".to_string(), "bob".to_string()));

        // A match this mm made itself is not replaced; one older than match_ttl is not read back.
        let mut out = vec![];
        let stream = t.book.matches[MATCH].stream.clone();
        t.book.restore_match(t.now, restored(MATCH, Mode::Ranked, vec![a, b], None, 10), HashMap::new(), &mut out);
        assert!(out.is_empty());
        assert_eq!(t.book.matches[MATCH].stream, stream);
        let ttl = t.book.cfg.match_ttl.as_secs();
        t.book.restore_match(
            t.now,
            restored("mode.direct-x-3", Mode::Direct, vec![a, b], None, ttl),
            HashMap::new(),
            &mut out,
        );
        assert!(!t.book.matches.contains_key("mode.direct-x-3"));

        // A room's game read back: its stream has the room's key, so the room's next game goes on
        // in it.
        let room_id = "mode.room-TLSZ-2026-10-21T14:37:43.060Z-4a0";
        t.book.restore_match(t.now, restored(room_id, Mode::Teams, vec![b, a], Some("TLSZ"), 30), names, &mut out);
        offer_in(&mut t, room_id, A, a, 9, 4);
        let stream = t.book.matches[room_id].stream.clone();
        let mut sorted = vec![a, b];
        sorted.sort_unstable();
        assert_eq!(t.book.streams[&stream].room_key, Some(("TLSZ".to_string(), sorted)));
        assert_eq!(t.book.matches[room_id].mode, "room");
    }

    /// What a watcher got on the watch channel: (kind, the chunk index or the JSON).
    fn watched(out: &[Output], conn: ConnId) -> Vec<(u8, Value)> {
        out.iter()
            .filter_map(|o| match o {
                Output::SendData { conn: c, channel, data } if *c == conn && *channel == spectate::WATCH_CHANNEL => {
                    Some(match data[0] {
                        spectate::WATCH_CHUNK => {
                            (data[0], json!(u32::from_le_bytes(data[1..5].try_into().unwrap())))
                        }
                        k => (k, serde_json::from_slice(&data[1..]).unwrap()),
                    })
                }
                _ => None,
            })
            .collect()
    }

    /// A room member who is not in the room's game (`room-watch`) gets its kept chunks on the watch
    /// channel: what came so far when the watch starts, then each one as it comes, the game's
    /// start and end around them; also after the room's next game started (the upload runs behind
    /// the players), then that next game. A stopped watch gets nothing more.
    #[test]
    fn room_watchers_get_the_game_on_the_watch_channel() {
        const W: ConnId = 9;
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        let m1 = "mode.room-TLSZ-2026-10-19T15:20:07.812Z-1";
        room_match(&mut t, m1);
        offer_in(&mut t, m1, A, a, 0x01, 1);
        let out = offer_in(&mut t, m1, B, b, 0x01, 1);
        let slot = role(&out, A).filter(|r| r["role"] == "primary").or_else(|| role(&out, B)).unwrap()["slot"]
            .as_u64()
            .unwrap() as u32;
        let up = if role(&out, A).unwrap()["role"] == "primary" { A } else { B };
        // Nothing streamed yet: nothing to send.
        let mut out = vec![];
        t.book.feed_watchers(&[(W, m1.into())], &[], &mut out);
        assert!(watched(&out, W).is_empty());
        let info = info_in(&t, m1);
        t.send_chunk(up, slot, 0, &info);
        t.send_chunk(up, slot, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(up, slot, 2, &encode_chunk(b"FRMS", &frms(0, 60, 1), true));
        let mut out = vec![];
        t.book.feed_watchers(&[(W, m1.into())], &[], &mut out);
        let got = watched(&out, W);
        assert_eq!(got[0].0, spectate::WATCH_BEGIN);
        assert_eq!(got[0].1["matchId"], m1);
        assert_eq!(got[1..].iter().map(|(k, i)| (*k, i.as_u64().unwrap())).collect::<Vec<_>>(), vec![
            (spectate::WATCH_CHUNK, 0),
            (spectate::WATCH_CHUNK, 1),
            (spectate::WATCH_CHUNK, 2)
        ]);
        // The players are back and the room's next game started: the first one's end still comes.
        let m2 = "mode.room-TLSZ-2026-10-19T15:25:00.000Z-2";
        room_match(&mut t, m2);
        t.send_chunk(up, slot, 3, &encode_chunk(b"FRMS", &frms(60, 60, 2), true));
        let endg = encode_chunk(b"ENDG", json!({"winnerPort": 2}).to_string().as_bytes(), false);
        t.send_chunk(up, slot, 4, &endg);
        let mut out = vec![];
        t.book.feed_watchers(&[(W, m2.into())], &[], &mut out);
        let kinds: Vec<u8> = watched(&out, W).iter().map(|(k, _)| *k).collect();
        assert_eq!(kinds, vec![spectate::WATCH_CHUNK, spectate::WATCH_CHUNK, spectate::WATCH_END]);
        // The next game.
        offer_in(&mut t, m2, A, a, 0x02, 2);
        let out = offer_in(&mut t, m2, B, b, 0x02, 2);
        let r = [role(&out, A).unwrap(), role(&out, B).unwrap()];
        let (up2, r2) = if r[0]["role"] == "primary" { (A, &r[0]) } else { (B, &r[1]) };
        let slot2 = r2["slot"].as_u64().unwrap() as u32;
        t.send_chunk(up2, slot2, 0, &info_in(&t, m2));
        let mut out = vec![];
        t.book.feed_watchers(&[(W, m2.into())], &[], &mut out);
        let got = watched(&out, W);
        assert_eq!((got[0].0, got[0].1["matchId"].as_str()), (spectate::WATCH_BEGIN, Some(m2)));
        assert_eq!(got.len(), 2);
        // Stopped (left early): nothing more of it.
        t.send_chunk(up2, slot2, 1, &encode_chunk(b"SETP", b"{}", false));
        let mut out = vec![];
        t.book.feed_watchers(&[], &[W], &mut out);
        assert!(watched(&out, W).is_empty());
        // A member who starts watching mid-game gets that game from its start, not the match's
        // games before it.
        const L: ConnId = 10;
        let mut out = vec![];
        t.book.feed_watchers(&[(L, m2.into())], &[], &mut out);
        let got = watched(&out, L);
        assert_eq!(got.iter().filter(|(k, _)| *k == spectate::WATCH_CHUNK).count(), 2);
        assert_eq!(got[0].1["matchId"], m2);
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
        assert_eq!(role(&out, A).unwrap()["role"], "witness", "A stays a backup");
        assert_eq!(role(&out, B).unwrap()["role"], "primary");
        assert_eq!(role(&out, B).unwrap()["from"], 180);
    }

    /// The uploader holds a game's first 10 s, then sends its headers: their arrival is progress,
    /// not a stall (production 2026-10-14: most games lost their primary 12-14 s in, the moment
    /// its headers landed, and with it their backup).
    #[test]
    fn headers_after_the_start_hold_are_not_a_stall() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = t.offer(B, b, 1, 0);
        let slot_a = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let slot_b = role(&out, B).unwrap()["slot"].as_u64().unwrap() as u32;
        assert_eq!(role(&out, A).unwrap()["role"], "primary");
        let digest = |t: &mut T, upto: u32| {
            let frms = (0..=upto).step_by(60).map(|f| (f, [0; 8])).collect();
            let mut out = vec![];
            t.book.request(t.now, B, b, SpectateRequest::Digest { slot: slot_b, frms }, &mut out);
        };
        for s in 1..=12 {
            digest(&mut t, s * 60);
            assert!(role(&t.advance(Duration::from_secs(1)), B).is_none(), "{s} s: A is in its start hold");
        }
        let info = t.info(1);
        t.send_chunk(A, slot_a, 0, &info);
        t.send_chunk(A, slot_a, 1, &encode_chunk(b"SETP", b"{}", false));
        digest(&mut t, 900);
        let out = t.advance(Duration::from_secs(1));
        assert!(role(&out, A).is_none() && role(&out, B).is_none(), "headers just came");
        t.send_chunk(A, slot_a, 2, &encode_chunk(b"FRMS", &frms(0, 60, 1), true));
        let out = t.advance(Duration::from_secs(3));
        assert!(role(&out, A).is_none() && role(&out, B).is_none());
    }

    /// A primary alone (its witness stopped) keeps a stalled game for a while instead of it being
    /// cut at once: it may only be waiting out a queue on its uplink.
    #[test]
    fn a_lone_stalled_primary_keeps_its_game_a_while() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = t.offer(B, b, 1, 0);
        let slot_a = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let slot_b = role(&out, B).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot_a, 0, &info);
        t.send_chunk(A, slot_a, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot_a, 2, &encode_chunk(b"FRMS", &frms(0, 60, 1), true));
        let id = stream_id(&t);
        let mut out = vec![];
        t.book.request(t.now, B, b, SpectateRequest::Digest { slot: slot_b, frms: vec![(1200, [0; 8])] }, &mut out);
        t.book.request(t.now, B, b, SpectateRequest::Stop { slot: slot_b, reason: "frame".into() }, &mut out);
        let cut = |t: &T| read_all(&t.book.store, &id).iter().any(|l| matches!(l, LiveLine::Bad { .. }));
        for _ in 0..28 {
            assert!(role(&t.advance(Duration::from_secs(1)), A).is_none());
            assert!(!cut(&t));
        }
        let out = t.advance(Duration::from_secs(2));
        assert_eq!(role(&out, A).unwrap()["role"], "none");
        assert!(cut(&t), "30 s without anything: spectators get what came");
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
    /// row stays playing (the players are), and the wait between games doesn't start until the
    /// players' games report the end. A pause doesn't cut.
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
        // The players play on past the wait between games: the stream stays.
        t.advance(Duration::from_secs(121));
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Playing);
        // Their end starts the wait (Direct: no set length), which then ends the stream.
        report(&mut t, A, a, 1, json!({"endMethod": "game", "winnerPort": 1}));
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Between);
        t.advance(Duration::from_secs(121));
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Over);
        // Without a report (older games), nothing uploaded for 10 minutes ends it.
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        t.offer(A, a, 1, 0);
        t.advance(Duration::from_secs(3));
        let slot = t.book.slots.keys().copied().next().unwrap();
        let info = t.info(1);
        t.send_chunk(A, slot, 0, &info);
        t.book.request(t.now, A, a, SpectateRequest::Stop { slot, reason: "frame".into() }, &mut out);
        t.advance(Duration::from_secs(590));
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Playing);
        t.advance(Duration::from_secs(20));
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Over);
    }

    /// `spectate-end` from player `who` for game `game` (its id as in `T::offer`).
    fn report(t: &mut T, conn: ConnId, who: Uuid, game: u8, fields: Value) -> Vec<Output> {
        let mut v = json!({"matchId": MATCH, "gameUid": uid_hex(game), "players": [
            {"port": 1, "uid": t.a.to_string()}, {"port": 2, "uid": t.b.to_string()}]});
        v.as_object_mut().unwrap().extend(fields.as_object().unwrap().clone());
        let SpectateRequest::End(e) = SpectateRequest::parse(spectate::SPECTATE_END, &v).unwrap() else { panic!() };
        let mut out = vec![];
        t.book.request(t.now, conn, who, SpectateRequest::End(e), &mut out);
        out
    }

    fn wins(t: &T) -> (u32, u32) {
        let e = &t.book.store.entries()[0];
        let w = |u: Uuid| e.players.iter().find(|p| p.uid == u.to_string()).map_or(0, |p| p.wins);
        (w(t.a), w(t.b))
    }

    /// Production 2026-10-15, a Ranked set listed as "Game 3 · 1-0": game 1's only uploader
    /// stopped mid-game (late frames), so its ENDG never came and its win wasn't counted, and the
    /// cut started the 120 s wait while the players were still playing, so game 2 opened a new
    /// stream at 0-0. Now the players' reports count the cut game, and the set stays one stream.
    #[test]
    fn a_cut_game_counts_the_players_report_and_the_set_stays_one_stream() {
        let mut t = T::with_match_mode(SpectateMode::On, Mode::Ranked);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = t.offer(B, b, 1, 0);
        let slot_a = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let slot_b = role(&out, B).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot_a, 0, &info);
        t.send_chunk(A, slot_a, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot_a, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        // The witness stops, then the primary: nobody left, the game is cut.
        let mut out = vec![];
        t.book.request(t.now, B, b, SpectateRequest::Stop { slot: slot_b, reason: "frame".into() }, &mut out);
        t.book.request(t.now, A, a, SpectateRequest::Stop { slot: slot_a, reason: "frame".into() }, &mut out);
        let id = stream_id(&t);
        assert!(read_all(&t.book.store, &id).iter().any(|l| matches!(l, LiveLine::Bad { game: 1, .. })));
        t.advance(Duration::from_secs(100));
        // Both games report the end; it counts once.
        report(&mut t, B, b, 1, json!({"endMethod": "game", "winnerPort": 2}));
        report(&mut t, A, a, 1, json!({"endMethod": "game", "winnerPort": 2}));
        assert_eq!(wins(&t), (0, 1));
        // A slow stage pick: 135 s after the cut, game 2 goes on in the same stream.
        t.advance(Duration::from_secs(35));
        play_game(&mut t, 2, r#"{"endMethod":"game","winnerPort":1}"#);
        let entries = t.book.store.entries();
        assert_eq!(entries.len(), 1, "{entries:?}");
        assert_eq!((entries[0].game.number, entries[0].game.place), (2, 2));
        assert_eq!(wins(&t), (1, 1));
        play_game(&mut t, 3, r#"{"endMethod":"game","winnerPort":2}"#);
        assert_eq!(wins(&t), (1, 2));
        t.advance(Duration::from_secs(1));
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Over, "2-1 ends the Ranked set");
    }

    /// A report that comes while the game's upload still goes on waits for it: the score doesn't
    /// run ahead of what spectators see, and the ENDG that follows counts the game once.
    #[test]
    fn a_report_waits_for_the_upload_and_counts_once() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        t.offer(A, a, 1, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(1);
        t.send_chunk(A, slot, 0, &info);
        t.send_chunk(A, slot, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        assert!(report(&mut t, A, a, 1, json!({"endMethod": "game", "winnerPort": 1})).is_empty());
        assert_eq!(wins(&t), (0, 0), "spectators haven't seen the end yet");
        assert_eq!(t.book.store.entries()[0].game.state, GameState::Playing);
        t.send_chunk(A, slot, 3, &encode_chunk(b"ENDG", br#"{"endMethod":"game","winnerPort":1}"#, false));
        assert_eq!(wins(&t), (1, 0));
        report(&mut t, B, b, 1, json!({"endMethod": "game", "winnerPort": 1}));
        assert_eq!(wins(&t), (1, 0), "once");
        // The upload stalls after the end instead: the next game's offer counts the report.
        t.offer(A, a, 2, 0);
        let out = t.advance(Duration::from_secs(3));
        let slot = role(&out, A).unwrap()["slot"].as_u64().unwrap() as u32;
        let info = t.info(2);
        t.send_chunk(A, slot, 0, &info);
        report(&mut t, A, a, 2, json!({"endMethod": "game", "winnerPort": 2}));
        assert_eq!(wins(&t), (1, 0));
        t.offer(A, a, 3, 0);
        assert_eq!(wins(&t), (1, 1));
    }

    /// A game nothing of was uploaded (the uploader held for its link all game) still counts,
    /// and a team battle's win counts for the team. Strangers' reports are refused, and reports
    /// for games mm doesn't know (spectating off) are ignored.
    #[test]
    fn reports_count_games_never_uploaded_and_teams() {
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        t.offer(A, a, 1, 0);
        t.advance(Duration::from_secs(3));
        report(&mut t, A, a, 1, json!({"endMethod": "game", "winnerPort": 2}));
        assert!(!t.book.games[&[1; 16]].counted, "the upload may still come");
        play_game(&mut t, 2, r#"{"endMethod":"game","winnerPort":2}"#);
        assert_eq!(wins(&t), (0, 2), "game 1's report counted when game 2 began");

        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        let c = Uuid::new_v4();
        t.offer(A, a, 1, 0);
        t.advance(Duration::from_secs(3));
        let slot = t.book.slots.keys().copied().next().unwrap();
        let mut out = vec![];
        t.book.request(t.now, A, a, SpectateRequest::Stop { slot, reason: "rtt".into() }, &mut out);
        let teams = json!({"endMethod": "game", "winnerPort": 3, "isTeams": true, "players": [
            {"port": 1, "uid": t.a.to_string(), "teamId": 0}, {"port": 2, "uid": t.b.to_string(), "teamId": 1},
            {"port": 3, "uid": c.to_string(), "teamId": 0}]});
        report(&mut t, A, a, 1, teams);
        let book = &t.book.streams[&t.book.games[&[1; 16]].stream];
        assert!(book.wins.is_empty(), "port 3 isn't a player of the match: nobody's win");
        let mut t2 = T::new(SpectateMode::On);
        let a2 = t2.a;
        t2.offer(A, a2, 1, 0);
        t2.advance(Duration::from_secs(3));
        let slot = t2.book.slots.keys().copied().next().unwrap();
        t2.book.request(t2.now, A, a2, SpectateRequest::Stop { slot, reason: "rtt".into() }, &mut out);
        let teams2 = json!({"endMethod": "game", "winnerPort": 2, "isTeams": true, "players": [
            {"port": 1, "uid": t2.a.to_string(), "teamId": 1}, {"port": 2, "uid": t2.b.to_string(), "teamId": 1}]});
        report(&mut t2, A, a2, 1, teams2);
        let book = &t2.book.streams[&t2.book.games[&[1; 16]].stream];
        assert_eq!((book.wins.get(&t2.a), book.wins.get(&t2.b)), (Some(&1), Some(&1)), "the winner's team");

        let out = report(&mut t, 3, Uuid::new_v4(), 1, json!({"endMethod": "game"}));
        assert_eq!(sent(&out, 3)[0]["error"], errors::NOT_YOUR_MATCH);
        let mut t = T::new(SpectateMode::Off);
        let a = t.a;
        t.offer(A, a, 1, 0);
        assert!(report(&mut t, A, a, 1, json!({"endMethod": "game", "winnerPort": 1})).is_empty());
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
        let won = |e: &LiveEntry, u: Uuid| e.players.iter().find(|p| p.uid == u.to_string()).unwrap().wins;
        assert_eq!((won(second, t.a), won(second, t.b)), (1, 1), "the match's score goes on");
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

    /// A room's matches for the pairing tests: each room start is a match of its own.
    fn room_match(t: &mut T, match_id: &str) {
        let rec = MatchRecord {
            match_id: match_id.into(),
            mode: Mode::Teams,
            players: vec![t.a, t.b],
            host: t.a,
            stages: vec![],
            region: String::new(),
            room: Some(common::db::MatchRoom {
                code: "TLSZ".into(),
                mode: "1v1".into(),
                ports: vec![1, 2],
                teams: None,
                ruleset: None,
            }),
        };
        let names =
            HashMap::from([(t.a, ("kyran".into(), "KYRN#1".into())), (t.b, ("manbat".into(), "MBAT#2".into()))]);
        t.book.register_match(t.now, &rec, names, &mut vec![]);
    }

    fn offer_in(t: &mut T, match_id: &str, conn: ConnId, who: Uuid, uid: u8, number: u32) -> Vec<Output> {
        let mut out = vec![];
        let o = Offer {
            match_id: match_id.into(),
            game_uid: uid_hex(uid),
            game: number,
            paused: 0,
            rollbacks: 0.0,
            listed: true,
        };
        t.book.request(t.now, conn, who, SpectateRequest::Offer(o), &mut out);
        out
    }

    fn info_in(t: &T, match_id: &str) -> Vec<u8> {
        let v = json!({
            "matchId": match_id, "stageId": 3, "isTeams": false,
            "players": [{"port": 1, "uid": t.a.to_string()}, {"port": 2, "uid": t.b.to_string()}]
        });
        encode_chunk(b"INFO", v.to_string().as_bytes(), false)
    }

    /// Production 2026-10-14, room TLSZ: the host's game (older build) had played 3 games in
    /// another room and offered its room game as game 4, the guest's as game 1, each with an id of
    /// its own. mm took them for two games, made both primaries and refused one ("the game is
    /// over"): no witness, and a pause stalled the stream. The two offers are now one game: one
    /// primary, one witness, each told its role under the id its own game knows, and the list
    /// numbers the room's games itself (1, 2), not 4 then 2.
    #[test]
    fn offers_with_other_ids_for_a_room_game_pair_up() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        let m1 = "mode.room-TLSZ-2026-10-14T15:20:07.812Z-1";
        room_match(&mut t, m1);
        assert!(role(&offer_in(&mut t, m1, A, a, 0x41, 4), A).is_none(), "waits for the other player");
        let out = offer_in(&mut t, m1, B, b, 0x01, 1);
        let (ra, rb) = (role(&out, A).unwrap(), role(&out, B).unwrap());
        assert_eq!((ra["role"].as_str(), rb["role"].as_str()), (Some("primary"), Some("witness")), "the host on a tie");
        assert_eq!(ra["gameUid"], uid_hex(0x41));
        assert_eq!(rb["gameUid"], uid_hex(0x01), "B's game ignores a role for an id it doesn't know");
        let slot_a = ra["slot"].as_u64().unwrap() as u32;
        let info = info_in(&t, m1);
        t.send_chunk(A, slot_a, 0, &info);
        t.send_chunk(A, slot_a, 1, &encode_chunk(b"SETP", b"{}", false));
        t.send_chunk(A, slot_a, 2, &encode_chunk(b"FRMS", &frms(120, 60, 1), true));
        assert_eq!(t.book.store.entries()[0].game.number, 1);

        // The primary breaks: the witness takes over, under its own id.
        let mut broken = encode_chunk(b"FRMS", &frms(180, 60, 1), true);
        let n = broken.len();
        broken[n - 1] ^= 0xFF;
        let out = t.send_chunk(A, slot_a, 3, &broken);
        let rb = role(&out, B).unwrap();
        assert_eq!((rb["role"].as_str(), rb["from"].as_u64()), (Some("primary"), Some(180)));
        assert_eq!(rb["gameUid"], uid_hex(0x01));
        let slot_b = rb["slot"].as_u64().unwrap() as u32;
        t.send_chunk(B, slot_b, 0, &encode_chunk(b"FRMS", &frms(180, 60, 1), true));
        t.send_chunk(B, slot_b, 1, &encode_chunk(b"ENDG", br#"{"endMethod":"game","winnerPort":1}"#, false));

        // The room's next game (a match of its own): 5 and 2, other ids again; one game, game 2.
        let m2 = "mode.room-TLSZ-2026-10-14T15:23:58.984Z-2";
        room_match(&mut t, m2);
        offer_in(&mut t, m2, B, b, 0x02, 2);
        let out = offer_in(&mut t, m2, A, a, 0x42, 5);
        let (ra, rb) = (role(&out, A).unwrap(), role(&out, B).unwrap());
        assert_eq!((ra["gameUid"].as_str(), rb["gameUid"].as_str()), (Some(&*uid_hex(0x42)), Some(&*uid_hex(0x02))));
        let mut roles = [ra["role"].as_str().unwrap(), rb["role"].as_str().unwrap()];
        roles.sort_unstable();
        assert_eq!(roles, ["primary", "witness"]);
        let (conn, slot) = if ra["role"] == "primary" { (A, &ra) } else { (B, &rb) };
        let slot = slot["slot"].as_u64().unwrap() as u32;
        let info = info_in(&t, m2);
        t.send_chunk(conn, slot, 0, &info);
        let e = &t.book.store.entries()[0];
        assert_eq!((e.game.number, e.game.place), (2, 2));
        assert_eq!(t.book.store.entries().len(), 1, "one stream for the room");
    }

    /// A set's game played again in the same match (a desync's restart): the player's game gives
    /// it the same number again, and so does the list. The next game is the next number.
    #[test]
    fn a_room_game_played_again_keeps_its_number() {
        let mut t = T::new(SpectateMode::On);
        let a = t.a;
        let m = "mode.room-TLSZ-2026-10-14T16:00:00.000Z-7";
        room_match(&mut t, m);
        let numbers = |t: &T| -> Vec<u32> {
            let mut v: Vec<u32> = t.book.games.values().map(|g| g.number).collect();
            v.sort_unstable();
            v
        };
        offer_in(&mut t, m, A, a, 0x51, 9);
        offer_in(&mut t, m, A, a, 0x52, 9);
        assert_eq!(numbers(&t), [1, 1], "game 9 again: the same game");
        offer_in(&mut t, m, A, a, 0x53, 10);
        assert_eq!(numbers(&t), [1, 1, 2]);
    }

    /// The pairing stays inside a match: an offer for another match's game never joins it, and a
    /// player who already offered for the open game starts a new game with a second id.
    #[test]
    fn the_pairing_stays_in_its_match() {
        let mut t = T::new(SpectateMode::On);
        let (a, b) = (t.a, t.b);
        let (m1, m2) = ("mode.room-TLSZ-x-1", "mode.room-TLSZ-x-2");
        room_match(&mut t, m1);
        room_match(&mut t, m2);
        offer_in(&mut t, m1, A, a, 0x61, 1);
        offer_in(&mut t, m2, B, b, 0x62, 1);
        assert_eq!(t.book.games.len(), 2);
        assert!(t.book.games[&[0x61; 16]].offers.iter().all(|o| o.account == a));
        offer_in(&mut t, m1, A, a, 0x63, 2);
        assert_eq!(t.book.games.len(), 3);
        assert!(!t.book.games[&[0x61; 16]].open(), "A's new game closed its last one");
    }
}
