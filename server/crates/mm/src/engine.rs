//! The matchmaking state machine, free of I/O so it can be unit-tested.
//!
//! The ENet loop ([`crate::server`]) feeds it [`Input`]s and carries out the
//! [`Output`]s it returns. Database reads happen outside: the engine asks for
//! an account with [`Output::FetchUser`] and gets [`Input::UserFetched`] back.
//!
//! Per connection the states are:
//!
//! ```text
//! Idle --create-ticket--> Validating --account ok--> Waiting --paired--> Done
//!   |                        |                          |
//!   |                        +--error/timeout--> Done   +--TTL--> Done (get-ticket-resp error)
//!   +--no ticket within idle_timeout--> Done
//!   +-- (client disconnect at any point removes the connection)
//! ```
//!
//! Abuse limits, all checked before the database is asked anything: at most
//! `max_conns_per_ip` connections per source address (IPv6: per /64), tickets per source
//! address (`ip_ticket_window`), and one account lookup in flight per connection. The
//! per-account limit (`ticket_interval`) is checked only after the play key is verified, so
//! nobody can use up someone else's limit by sending tickets with their uid.
//!
//! Every refusal is an explicit `error` the game shows: tickets are never
//! dropped silently.
//!
//! Three queues:
//! - **Direct**: two tickets that name each other's codes are paired.
//! - **Unranked**: a FIFO. The oldest waiting ticket is paired with the next one in arrival order
//!   that is in its region (any region once either has waited `region_widen`) and that it did not
//!   just fail to connect to. Slippi's tickets carry no region field, so the region comes from the
//!   ticket's source address ([`crate::region`]); with no region table there is one bucket.
//! - **Ranked**: like Unranked, but the oldest ticket takes the closest-rated opponent whose
//!   rating is within the rating band: `ranked_band` points, widened by `ranked_band_step` for
//!   every `ranked_band_interval` the longer-waiting of the two has waited (design 2.3: ±150,
//!   +50 every 15 s). The ratings are the accounts' Elo ([`common::ranked`]).
//!
//! Rooms (`crate::rooms`, `docs/rooms-protocol.md`) live on **online connections**: a game that
//! is running and logged in keeps one connection open (`hello`, then `room-*` requests), which is
//! also what the online count counts. A room's game starts with ordinary tickets in mode 3 (Teams)
//! naming the room code; every member gets one `get-ticket-resp` listing all of them.
//!
//! The queues share the failed-connect rule: Slippi's 1v1 client requeues with a new ticket when
//! its 8 s P2P window fails, so a pair matched again within `requeue_window` is taken to have
//! failed. Direct holds such a pair back (P2P window + `repair_backoff` × failures) and errors
//! after `max_connect_failures`; Unranked and Ranked pair each of them with someone else when they
//! can, re-pair them only after the same backoff, and after `max_connect_failures` stop pairing
//! them with each other (they keep searching) until the window has passed.

use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};
use std::path::PathBuf;
use std::time::{Duration, Instant};

use chrono::{DateTime, Utc};
use common::codes::{decode_search_code, ConnectCode};
use common::db::MmUser;
use common::net::{ip_key, sanitize_lan_addr, to_v4};
use common::playkey::PlayKeySecret;
use common::proto::{
    clamp_error, parse_client_message, ClientMessage, CreateTicket, CreateTicketResp, GetTicketResp, Mode, Player,
    Rank, GET_TICKET_RESP,
};
use common::ratelimit::{RateLimiter, Window};
use common::rooms::{decode_room_search_code, HelloResp, MmStatus, RoomError, RoomRequest};
use sha2::{Digest, Sha256};
use uuid::Uuid;

use crate::messages as msg;
use crate::region::RegionMap;
use crate::rooms::{MemberInfo, RoomBook, RoomsConfig};
use crate::ruleset::Rulesets;
use crate::versions::FeedVersions;

/// How often the update feed (`update_feed_dir`) is read again.
const FEED_REFRESH: Duration = Duration::from_secs(10);

pub type ConnId = u64;

/// Largest client packet we accept. A real `create-ticket` is about 400 bytes.
pub const MAX_PACKET: usize = 8 * 1024;

/// Slippi's P2P connect window: 8 s, after which a 1v1 client requeues
/// with a new ticket (`SlippiNetplay.cpp:778-779`, `SlippiMatchmaking.cpp:891-898`).
pub const P2P_CONNECT_WINDOW: Duration = Duration::from_secs(8);

#[derive(Debug, Clone)]
pub struct EngineConfig {
    /// A waiting ticket gets an explicit error after this long (design: 10 min).
    pub ticket_ttl: Duration,
    /// How long an account lookup may take before the ticket is refused. The
    /// client gives up on `create-ticket-resp` after 5 s.
    pub auth_timeout: Duration,
    /// Tickets per uid: at most 1 per this interval (design 3: 1 per 2 s). Checked once the
    /// play key is verified.
    pub ticket_interval: Duration,
    /// Tickets per source address (IPv6: per /64), checked before the account lookup.
    pub ip_ticket_window: Window,
    /// Connections per source address (IPv6: per /64). Further connections are refused at once.
    pub max_conns_per_ip: usize,
    /// A connection that has sent no ticket this long after connecting is closed. The game sends
    /// its ticket right after the ENet handshake.
    pub idle_timeout: Duration,
    /// A pair that is matched again within this window of its last match is
    /// assumed to have failed the P2P connect.
    pub requeue_window: Duration,
    /// Extra delay before re-pairing, per consecutive failure.
    pub repair_backoff: Duration,
    /// After this many consecutive failed connects (counting the first match),
    /// stop re-pairing and tell both players.
    pub max_connect_failures: u32,
    /// Refuse tickets from clients older than this (`appVersion`), if set.
    pub min_app_version: Option<String>,
    /// Sent with version errors so the client can show the update option.
    pub latest_version: Option<String>,
    /// The launcher's update feed folder (`versions`): a game older than its platform's newest
    /// published build is refused (`hello`, tickets, a room's ready), read again every
    /// [`FEED_REFRESH`].
    pub update_feed_dir: Option<PathBuf>,
    pub rulesets: Rulesets,
    /// Region of a ticket's source address (Unranked buckets).
    pub regions: RegionMap,
    /// An Unranked or Ranked ticket that has waited this long takes an opponent from any region.
    pub region_widen: Duration,
    /// Ranked: the largest rating difference of a pair at first ...
    pub ranked_band: f64,
    /// ... widened by this much ...
    pub ranked_band_step: f64,
    /// ... for every this long the longer-waiting player has waited.
    pub ranked_band_interval: Duration,
    /// Rooms: timeouts and limits. Their stages and items are Direct's ruleset.
    pub rooms: RoomsConfig,
    /// `hello`s per account (a game opens its online connection once per start or reconnect).
    pub hello_window: Window,
}

impl Default for EngineConfig {
    fn default() -> Self {
        EngineConfig {
            ticket_ttl: Duration::from_secs(600),
            auth_timeout: Duration::from_secs(4),
            ticket_interval: Duration::from_secs(2),
            ip_ticket_window: Window::new(30, Duration::from_secs(10)),
            max_conns_per_ip: 8,
            idle_timeout: Duration::from_secs(10),
            requeue_window: Duration::from_secs(60),
            repair_backoff: Duration::from_secs(5),
            max_connect_failures: 3,
            min_app_version: None,
            latest_version: None,
            update_feed_dir: None,
            rulesets: Rulesets::default(),
            regions: RegionMap::default(),
            region_widen: Duration::from_secs(30),
            ranked_band: 150.0,
            ranked_band_step: 50.0,
            ranked_band_interval: Duration::from_secs(15),
            rooms: RoomsConfig::default(),
            hello_window: Window::new(10, Duration::from_secs(60)),
        }
    }
}

/// Result of the account lookup the engine asked for.
#[derive(Debug, Clone)]
pub enum FetchResult {
    Found(MmUser),
    NotFound,
    Error(String),
}

#[derive(Debug)]
pub enum Input {
    Connected { conn: ConnId, addr: SocketAddr },
    Packet { conn: ConnId, data: Vec<u8> },
    Disconnected { conn: ConnId },
    UserFetched { conn: ConnId, seq: u64, result: FetchResult },
    Tick,
}

#[derive(Debug, Clone, PartialEq)]
pub struct MatchRecord {
    pub match_id: String,
    pub mode: Mode,
    pub players: Vec<Uuid>,
    pub host: Uuid,
    pub stages: Vec<u16>,
    /// The players' region, or `a+b` when they differ.
    pub region: String,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Output {
    /// Send this JSON reliably on channel 0.
    Send {
        conn: ConnId,
        json: String,
    },
    /// Disconnect after queued packets are delivered.
    Disconnect {
        conn: ConnId,
    },
    /// Disconnect after queued packets are delivered, without the grace period: a connection the
    /// engine refused as soon as it connected (it does not track it).
    Close {
        conn: ConnId,
    },
    FetchUser {
        conn: ConnId,
        seq: u64,
        uid: Uuid,
    },
    RecordMatch(MatchRecord),
}

#[derive(Debug, Clone)]
struct Pending {
    seq: u64,
    since: Instant,
    play_key: String,
    mode: Mode,
    /// Direct: the code typed in-game. Unranked: none (Slippi sends `[]`).
    target: Option<ConnectCode>,
    lan: String,
    /// A `hello` (opens an online connection) rather than a ticket.
    hello: bool,
    /// A room game ticket (mode 3): the room code.
    room: Option<String>,
    client: ClientBuild,
}

/// The build a game says it is (`appVersion`, `platform`).
#[derive(Debug, Clone, Default)]
struct ClientBuild {
    app_version: String,
    platform: String,
}

#[derive(Debug, Clone)]
struct Waiting {
    since: Instant,
    user: MmUser,
    code: String,
    mode: Mode,
    /// Direct only; empty for Unranked.
    target: String,
    lan: String,
    region: String,
}

/// An online connection: a running, logged-in game (rooms, the online count).
#[derive(Debug, Clone)]
struct Online {
    user: MmUser,
    code: String,
    /// Checked again on a room's ready: a game left running across a release is refused then.
    client: ClientBuild,
}

#[derive(Debug, Clone)]
enum State {
    Idle,
    Validating(Pending),
    Waiting(Waiting),
    Online(Online),
    Done,
}

#[derive(Debug)]
struct Conn {
    addr: SocketAddr,
    /// When the connection was made (for `idle_timeout`).
    since: Instant,
    state: State,
}

#[derive(Debug, Clone, Copy)]
struct PairHistory {
    last_match: Instant,
    failures: u32,
}

pub struct Engine {
    cfg: EngineConfig,
    secret: PlayKeySecret,
    conns: HashMap<ConnId, Conn>,
    /// Direct tickets waiting, keyed by (own code, target code).
    direct: HashMap<(String, String), ConnId>,
    /// Unranked tickets waiting, oldest first.
    unranked: Vec<ConnId>,
    /// Ranked tickets waiting, oldest first.
    ranked: Vec<ConnId>,
    /// Recently matched pairs (sorted uids), to space out re-pairing after a
    /// failed P2P connect.
    history: HashMap<(Uuid, Uuid), PairHistory>,
    ticket_limiter: RateLimiter<Uuid>,
    ip_limiter: RateLimiter<IpAddr>,
    hello_limiter: RateLimiter<Uuid>,
    rooms: RoomBook,
    next_seq: u64,
    match_counter: u64,
    /// The newest build of each platform (`update_feed_dir`), and when it was read.
    feed: FeedVersions,
    feed_read: Option<Instant>,
}

fn hello_ok() -> HelloResp {
    HelloResp { kind: common::rooms::HELLO_RESP.into(), error: None, latest_version: None }
}

fn pair_key(a: Uuid, b: Uuid) -> (Uuid, Uuid) {
    if a <= b {
        (a, b)
    } else {
        (b, a)
    }
}

fn uid_hash(uid: Uuid) -> [u8; 32] {
    Sha256::digest(uid.as_bytes()).into()
}

/// Parses `major.minor.patch[-suffix]` into a comparable tuple. Missing parts are 0.
pub fn parse_version(v: &str) -> (u64, u64, u64) {
    let core = v.trim().trim_start_matches('v').split(['-', '+']).next().unwrap_or("");
    let mut it = core.split('.').map(|p| p.parse::<u64>().unwrap_or(0));
    (it.next().unwrap_or(0), it.next().unwrap_or(0), it.next().unwrap_or(0))
}

impl Engine {
    pub fn new(cfg: EngineConfig, secret: PlayKeySecret) -> Self {
        let ticket_limiter = RateLimiter::new(&[Window::new(1, cfg.ticket_interval)]);
        let ip_limiter = RateLimiter::new(&[cfg.ip_ticket_window]);
        let hello_limiter = RateLimiter::new(&[cfg.hello_window]);
        let mut rooms_cfg = cfg.rooms.clone();
        rooms_cfg.rules = cfg.rulesets.for_mode(Mode::Direct);
        Engine {
            cfg,
            secret,
            conns: HashMap::new(),
            direct: HashMap::new(),
            unranked: Vec::new(),
            ranked: Vec::new(),
            history: HashMap::new(),
            ticket_limiter,
            ip_limiter,
            hello_limiter,
            rooms: RoomBook::new(rooms_cfg),
            next_seq: 1,
            match_counter: 0,
            feed: FeedVersions::default(),
            feed_read: None,
        }
    }

    /// Replaces the newest build of each platform (what reading `update_feed_dir` gives).
    pub fn set_feed_versions(&mut self, feed: FeedVersions) {
        if feed != self.feed {
            tracing::info!(%feed, "newest client builds");
        }
        self.feed = feed;
    }

    fn refresh_feed(&mut self, now: Instant) {
        let Some(dir) = self.cfg.update_feed_dir.clone() else { return };
        if self.feed_read.is_some_and(|t| now.duration_since(t) < FEED_REFRESH) {
            return;
        }
        self.feed_read = Some(now);
        let feed = FeedVersions::read(&dir);
        if feed.is_empty() && !self.feed.is_empty() {
            // pp-release replaces a feed file by renaming, so it is never missing; an empty read
            // is a mistake (permissions, a moved folder): keep what was known.
            tracing::warn!(dir = %dir.display(), "no update feed found; keeping the last versions");
            return;
        }
        self.set_feed_versions(feed);
    }

    /// The version to update to when `client` is older than what it must have: its platform's
    /// newest published build (`update_feed_dir`), or `min_app_version`.
    fn outdated(&self, client: &ClientBuild) -> Option<String> {
        let have = parse_version(&client.app_version);
        if let Some(f) = self.feed.required(&client.platform).filter(|f| have < parse_version(f)) {
            return Some(f.to_string());
        }
        let min = self.cfg.min_app_version.as_deref().filter(|m| have < parse_version(m))?;
        Some(self.cfg.latest_version.clone().unwrap_or_else(|| min.to_string()))
    }

    pub fn connection_count(&self) -> usize {
        self.conns.len()
    }

    pub fn waiting_count(&self) -> usize {
        self.conns.values().filter(|c| matches!(c.state, State::Waiting(_))).count()
    }

    /// Players whose game is running and logged in: accounts with an online connection.
    pub fn online_count(&self) -> usize {
        let mut uids: Vec<Uuid> = self
            .conns
            .values()
            .filter_map(|c| match &c.state {
                State::Online(o) => Some(o.user.uid),
                _ => None,
            })
            .collect();
        uids.sort_unstable();
        uids.dedup();
        uids.len()
    }

    pub fn room_count(&self) -> usize {
        self.rooms.room_count()
    }

    /// The snapshot the launcher sees: the online count and the public rooms.
    pub fn status(&self, wall: DateTime<Utc>) -> MmStatus {
        MmStatus {
            online: self.online_count(),
            rooms: self.rooms.public_rooms(200),
            updated_at: wall.to_rfc3339_opts(chrono::SecondsFormat::Millis, true),
        }
    }

    pub fn handle(&mut self, now: Instant, wall: DateTime<Utc>, input: Input) -> Vec<Output> {
        let mut out = Vec::new();
        match input {
            Input::Connected { conn, addr } => self.on_connect(now, conn, addr, &mut out),
            Input::Disconnected { conn } => {
                if let Some(c) = self.conns.remove(&conn) {
                    match &c.state {
                        State::Waiting(w) => {
                            tracing::info!(code = %w.code, target = %w.target, "ticket cancelled by client disconnect")
                        }
                        State::Online(o) => tracing::info!(code = %o.code, "online connection closed"),
                        _ => {}
                    }
                    self.unindex(conn);
                    self.rooms.conn_gone(now, conn, &mut out);
                    self.fail_room_tickets(&mut out);
                }
            }
            Input::Packet { conn, data } => self.on_packet(now, conn, &data, &mut out),
            Input::UserFetched { conn, seq, result } => self.on_user(now, wall, conn, seq, result, &mut out),
            Input::Tick => self.on_tick(now, wall, &mut out),
        }
        out
    }

    fn send(out: &mut Vec<Output>, conn: ConnId, msg: &impl serde::Serialize) {
        let json = serde_json::to_string(msg).expect("protocol messages serialize");
        out.push(Output::Send { conn, json });
    }

    /// Refuses a ticket before it was accepted: `create-ticket-resp {error}`.
    fn refuse(&mut self, out: &mut Vec<Output>, conn: ConnId, msg: impl Into<String>) {
        let msg = msg.into();
        tracing::info!(conn, "ticket refused: {msg}");
        Self::send(out, conn, &CreateTicketResp::error(msg));
        self.finish(out, conn);
    }

    /// Ends an accepted ticket: `get-ticket-resp {error}`.
    fn fail_ticket(&mut self, out: &mut Vec<Output>, conn: ConnId, msg: impl Into<String>, latest: Option<String>) {
        let msg = msg.into();
        tracing::info!(conn, "ticket ended: {msg}");
        Self::send(out, conn, &GetTicketResp::error(msg, latest));
        self.finish(out, conn);
    }

    fn finish(&mut self, out: &mut Vec<Output>, conn: ConnId) {
        self.unindex(conn);
        if let Some(c) = self.conns.get_mut(&conn) {
            c.state = State::Done;
        }
        out.push(Output::Disconnect { conn });
        self.rooms.conn_gone(Instant::now(), conn, out);
    }

    /// Answers the room game tickets the room book gave up on (a start that timed out, a member
    /// who left during the start).
    fn fail_room_tickets(&mut self, out: &mut Vec<Output>) {
        for (conn, why) in self.rooms.take_failed_tickets() {
            if matches!(self.conns.get(&conn), Some(Conn { state: State::Waiting(_), .. })) {
                self.fail_ticket(out, conn, why, None);
            }
        }
    }

    /// A fatal error on an online connection (or a room message before `hello`): `error`, then
    /// the connection is closed.
    fn fail_online(&mut self, out: &mut Vec<Output>, conn: ConnId, msg: &str) {
        tracing::info!(conn, "online connection ended: {msg}");
        Self::send(out, conn, &serde_json::json!({"type": common::rooms::ERROR, "error": clamp_error(msg.into())}));
        self.finish(out, conn);
    }

    fn unindex(&mut self, conn: ConnId) {
        self.direct.retain(|_, c| *c != conn);
        self.unranked.retain(|c| *c != conn);
        self.ranked.retain(|c| *c != conn);
    }

    fn on_connect(&mut self, now: Instant, conn: ConnId, addr: SocketAddr, out: &mut Vec<Output>) {
        let key = ip_key(addr.ip());
        let from_key = self.conns.values().filter(|c| ip_key(c.addr.ip()) == key).count();
        if from_key >= self.cfg.max_conns_per_ip {
            tracing::warn!(conn, %addr, from_key, "connection refused: too many from this address");
            // The game reads this as the answer to the ticket it sends next.
            Self::send(out, conn, &CreateTicketResp::error(msg::TOO_MANY_CONNECTIONS));
            out.push(Output::Close { conn });
            return;
        }
        self.conns.insert(conn, Conn { addr, since: now, state: State::Idle });
    }

    fn on_packet(&mut self, now: Instant, conn: ConnId, data: &[u8], out: &mut Vec<Output>) {
        let Some(c) = self.conns.get(&conn) else { return };
        let online = match &c.state {
            State::Done => return,
            // One account lookup in flight per connection: the game never sends a second
            // ticket before the answer to its first.
            State::Validating(_) => return self.refuse(out, conn, msg::INVALID_REQUEST),
            State::Online(o) => Some(o.clone()),
            State::Idle | State::Waiting(_) => None,
        };
        if data.len() > MAX_PACKET {
            if online.is_some() {
                return self.fail_online(out, conn, msg::INVALID_REQUEST);
            }
            self.refuse(out, conn, msg::INVALID_REQUEST);
            return;
        }
        let parsed = parse_client_message(data);
        if let Some(o) = online {
            return self.on_online_packet(now, conn, o, parsed, out);
        }
        let ticket = match parsed {
            Ok(ClientMessage::CreateTicket(t)) => t,
            Ok(ClientMessage::Hello(h)) => {
                if matches!(self.conns.get(&conn), Some(Conn { state: State::Waiting(_), .. })) {
                    return self.refuse(out, conn, msg::INVALID_REQUEST);
                }
                return self.on_hello(now, conn, *h, out);
            }
            Ok(ClientMessage::Room(_)) => {
                // Rooms need an online connection: `hello` first.
                return self.fail_online(out, conn, msg::NOT_LOGGED_IN);
            }
            Ok(ClientMessage::Unknown(kind)) => {
                tracing::warn!(conn, %kind, "unknown message type");
                self.refuse(out, conn, msg::UNKNOWN_REQUEST);
                return;
            }
            Err(e) => {
                tracing::warn!(conn, "malformed packet: {e}");
                self.refuse(out, conn, msg::INVALID_REQUEST);
                return;
            }
        };
        // A second ticket on the same connection replaces the first.
        self.unindex(conn);
        self.on_create_ticket(now, conn, *ticket, out);
    }

    /// A packet on an online connection: `hello` and room requests only. Mistakes are answered
    /// (`room-error`) and the connection stays.
    fn on_online_packet(
        &mut self,
        now: Instant,
        conn: ConnId,
        o: Online,
        parsed: Result<ClientMessage, common::proto::ParseError>,
        out: &mut Vec<Output>,
    ) {
        let room_error = |out: &mut Vec<Output>, op: &str, error: &str| {
            Self::send(
                out,
                conn,
                &RoomError { kind: common::rooms::ROOM_ERROR.into(), op: op.into(), error: error.into() },
            )
        };
        match parsed {
            Ok(ClientMessage::Room(Ok(req))) => {
                // Starting a game on a build older than the newest one: refused (red on the CSS),
                // so the player closes Dolphin and updates.
                if matches!(req, RoomRequest::Ready { ready: true, .. }) {
                    if let Some(latest) = self.outdated(&o.client) {
                        tracing::info!(conn, version = %o.client.app_version, %latest, "ready refused: out of date");
                        return room_error(out, common::rooms::ROOM_READY, &msg::update_to(&latest));
                    }
                }
                let who = MemberInfo {
                    uid: o.user.uid,
                    display_name: o.user.display_name.clone(),
                    connect_code: o.code.clone(),
                };
                self.rooms.request(now, conn, &who, req, out);
                self.fail_room_tickets(out);
            }
            Ok(ClientMessage::Room(Err(why))) => {
                tracing::warn!(conn, "bad room request: {why}");
                room_error(out, "invalid", msg::INVALID_REQUEST);
            }
            Ok(ClientMessage::Hello(_)) => Self::send(out, conn, &hello_ok()),
            Ok(ClientMessage::CreateTicket(_)) => Self::send(out, conn, &CreateTicketResp::error(msg::INVALID_REQUEST)),
            Ok(ClientMessage::Unknown(kind)) => room_error(out, &kind, msg::UNKNOWN_REQUEST),
            Err(e) => {
                tracing::warn!(conn, "malformed packet on an online connection: {e}");
                room_error(out, "invalid", msg::INVALID_REQUEST);
            }
        }
    }

    /// `hello`: the same checks as a ticket (version, uid, play key), then the connection stays
    /// open as an online connection.
    fn on_hello(&mut self, now: Instant, conn: ConnId, h: common::rooms::Hello, out: &mut Vec<Output>) {
        let client = ClientBuild { app_version: h.app_version, platform: h.platform };
        if let Some(latest) = self.outdated(&client) {
            return self.refuse_hello(out, conn, &msg::update_to(&latest), Some(latest.clone()));
        }
        let Ok(uid) = Uuid::parse_str(h.user.uid.trim()) else {
            return self.refuse_hello(out, conn, msg::NOT_LOGGED_IN, None);
        };
        if h.user.play_key.is_empty() || h.user.play_key.len() > 128 {
            return self.refuse_hello(out, conn, msg::NOT_LOGGED_IN, None);
        }
        let Some(addr) = self.conns.get(&conn).map(|c| c.addr) else { return };
        if self.ip_limiter.check(&ip_key(addr.ip()), now).is_err() {
            return self.refuse_hello(out, conn, msg::TOO_MANY_SEARCHES, None);
        }
        let seq = self.next_seq;
        self.next_seq += 1;
        let pending = Pending {
            seq,
            since: now,
            play_key: h.user.play_key,
            mode: Mode::Direct,
            target: None,
            lan: String::new(),
            hello: true,
            room: None,
            client,
        };
        if let Some(c) = self.conns.get_mut(&conn) {
            c.state = State::Validating(pending);
        }
        out.push(Output::FetchUser { conn, seq, uid });
    }

    /// Refuses a `hello`: `hello-resp {error}`, then the connection is closed.
    fn refuse_hello(&mut self, out: &mut Vec<Output>, conn: ConnId, msg: &str, latest: Option<String>) {
        tracing::info!(conn, "hello refused: {msg}");
        Self::send(
            out,
            conn,
            &HelloResp {
                kind: common::rooms::HELLO_RESP.into(),
                error: Some(clamp_error(msg.into())),
                latest_version: latest,
            },
        );
        self.finish(out, conn);
    }

    fn on_create_ticket(&mut self, now: Instant, conn: ConnId, t: CreateTicket, out: &mut Vec<Output>) {
        let Some(mode) = Mode::from_u8(t.search.mode) else {
            return self.refuse(out, conn, msg::UNKNOWN_MODE);
        };
        let unsupported = match mode {
            // Mode 3 is a room's game ticket (the room code in `search.connectCode`).
            Mode::Direct | Mode::Unranked | Mode::Ranked | Mode::Teams => None,
            Mode::Party => Some("Party"),
        };
        if let Some(name) = unsupported {
            return self.refuse(out, conn, msg::not_available(name));
        }
        let client = ClientBuild { app_version: t.app_version, platform: t.platform };
        if let Some(latest) = self.outdated(&client) {
            return self.refuse(out, conn, msg::update_to(&latest));
        }
        let Ok(uid) = Uuid::parse_str(t.user.uid.trim()) else {
            return self.refuse(out, conn, msg::NOT_LOGGED_IN);
        };
        if t.user.play_key.is_empty() || t.user.play_key.len() > 128 {
            return self.refuse(out, conn, msg::NOT_LOGGED_IN);
        }
        // Queue modes send an empty code; whatever they send is ignored.
        let target = match mode {
            Mode::Direct => match decode_search_code(&t.search.connect_code) {
                Ok(c) => Some(c),
                Err(_) => return self.refuse(out, conn, msg::INVALID_CODE),
            },
            _ => None,
        };
        let room = match mode {
            Mode::Teams => match decode_room_search_code(&t.search.connect_code) {
                Some(code) => Some(code),
                None => return self.refuse(out, conn, msg::ROOM_NOT_FOUND),
            },
            _ => None,
        };
        let Some(addr) = self.conns.get(&conn).map(|c| c.addr) else { return };
        // The game cannot use an IPv6 peer address (it splits `ipAddress` on ':'), and an empty
        // one crashes older clients, so such tickets are refused before they can be matched.
        if to_v4(addr).is_none() {
            return self.refuse(out, conn, msg::NEEDS_IPV4);
        }
        if self.ip_limiter.check(&ip_key(addr.ip()), now).is_err() {
            return self.refuse(out, conn, msg::TOO_MANY_SEARCHES);
        }
        let seq = self.next_seq;
        self.next_seq += 1;
        let pending = Pending {
            seq,
            since: now,
            play_key: t.user.play_key,
            mode,
            target,
            lan: sanitize_lan_addr(&t.ip_address_lan),
            hello: false,
            room,
            client,
        };
        if let Some(c) = self.conns.get_mut(&conn) {
            c.state = State::Validating(pending);
        }
        out.push(Output::FetchUser { conn, seq, uid });
    }

    fn on_user(
        &mut self,
        now: Instant,
        wall: DateTime<Utc>,
        conn: ConnId,
        seq: u64,
        result: FetchResult,
        out: &mut Vec<Output>,
    ) {
        let Some(c) = self.conns.get(&conn) else { return };
        let State::Validating(p) = &c.state else { return };
        if p.seq != seq {
            return;
        }
        let p = p.clone();
        let user = match result {
            FetchResult::Found(u) => u,
            FetchResult::NotFound => return self.refuse(out, conn, msg::ACCOUNT_NOT_FOUND),
            FetchResult::Error(e) => {
                tracing::error!("account lookup failed: {e}");
                return self.refuse(out, conn, msg::UNAVAILABLE);
            }
        };
        if p.hello {
            return self.on_hello_user(now, wall, conn, p, user, out);
        }
        if !self.secret.verify(user.uid, user.play_key_version, &p.play_key) {
            return self.refuse(out, conn, msg::LOGIN_EXPIRED);
        }
        // Only after the play key: otherwise anyone knowing a uid (opponents see it) could keep
        // that account from searching. A room's game ticket is accepted only from a member of a
        // starting room, so it needs no limit of its own (and must not be held up by one).
        if p.room.is_none() && self.ticket_limiter.check(&user.uid, now).is_err() {
            return self.refuse(out, conn, msg::SEARCHING_TOO_OFTEN);
        }
        if user.is_banned(wall) {
            return self.refuse(out, conn, msg::BANNED);
        }
        let Some(code) = user.connect_code.clone() else {
            return self.refuse(out, conn, msg::NO_CONNECT_CODE);
        };
        let target = p.target.as_ref().map(|t| t.to_string()).unwrap_or_default();
        if p.mode == Mode::Direct && target == code {
            return self.refuse(out, conn, msg::OWN_CODE);
        }
        // One active ticket per account: a newer search replaces an older one.
        let older: Vec<ConnId> = self
            .conns
            .iter()
            .filter(|(id, c)| **id != conn && matches!(&c.state, State::Waiting(w) if w.user.uid == user.uid))
            .map(|(id, _)| *id)
            .collect();
        for old in older {
            self.fail_ticket(out, old, msg::REPLACED, None);
        }
        if let Some(room) = p.room.clone() {
            return self.on_room_ticket(now, conn, p, user, code, room, out);
        }
        Self::send(out, conn, &CreateTicketResp::ok());
        let Some(c) = self.conns.get_mut(&conn) else { return };
        let region = self.cfg.regions.region_of(c.addr);
        c.state = State::Waiting(Waiting {
            since: now,
            user,
            code: code.clone(),
            mode: p.mode,
            target: target.clone(),
            lan: p.lan,
            region: region.clone(),
        });
        if p.mode == Mode::Unranked {
            tracing::info!(conn, %code, %region, queued = self.unranked.len() + 1, "unranked ticket waiting");
            self.unranked.push(conn);
            self.pair_queue(Mode::Unranked, now, wall, out);
        } else if p.mode == Mode::Ranked {
            let rating = self.conns.get(&conn).and_then(|c| match &c.state {
                State::Waiting(w) => Some(w.user.rating),
                _ => None,
            });
            tracing::info!(conn, %code, %region, ?rating, queued = self.ranked.len() + 1, "ranked ticket waiting");
            self.ranked.push(conn);
            self.pair_queue(Mode::Ranked, now, wall, out);
        } else {
            tracing::info!(conn, %code, %target, "direct ticket waiting");
            self.direct.insert((code, target), conn);
            self.try_pair(now, wall, conn, out);
        }
    }

    /// The account behind a `hello` is known: verify it and open the online connection.
    fn on_hello_user(
        &mut self,
        now: Instant,
        wall: DateTime<Utc>,
        conn: ConnId,
        p: Pending,
        user: MmUser,
        out: &mut Vec<Output>,
    ) {
        if !self.secret.verify(user.uid, user.play_key_version, &p.play_key) {
            return self.refuse_hello(out, conn, msg::LOGIN_EXPIRED, None);
        }
        if self.hello_limiter.check(&user.uid, now).is_err() {
            return self.refuse_hello(out, conn, msg::TOO_MANY_REQUESTS, None);
        }
        if user.is_banned(wall) {
            return self.refuse_hello(out, conn, msg::BANNED, None);
        }
        let Some(code) = user.connect_code.clone() else {
            return self.refuse_hello(out, conn, msg::NO_CONNECT_CODE, None);
        };
        // One online connection per account: a newer game replaces the older one (which leaves
        // its room).
        let older: Vec<ConnId> = self
            .conns
            .iter()
            .filter(|(id, c)| **id != conn && matches!(&c.state, State::Online(o) if o.user.uid == user.uid))
            .map(|(id, _)| *id)
            .collect();
        for old in older {
            self.fail_online(out, old, msg::SIGNED_IN_ELSEWHERE);
        }
        self.fail_room_tickets(out);
        tracing::info!(conn, %code, "online");
        if let Some(c) = self.conns.get_mut(&conn) {
            c.state = State::Online(Online { user, code, client: p.client });
        }
        Self::send(out, conn, &hello_ok());
    }

    /// A room's game ticket from a verified account.
    #[allow(clippy::too_many_arguments)]
    fn on_room_ticket(
        &mut self,
        now: Instant,
        conn: ConnId,
        p: Pending,
        user: MmUser,
        code: String,
        room: String,
        out: &mut Vec<Output>,
    ) {
        let Some(addr) = self.conns.get(&conn).and_then(|c| to_v4(c.addr)) else { return };
        let outcome = match self.rooms.ticket(conn, addr, p.lan.clone(), user.uid, &room) {
            Ok(o) => o,
            Err(e) => return self.refuse(out, conn, e),
        };
        Self::send(out, conn, &CreateTicketResp::ok());
        let region = self.cfg.regions.region_of(SocketAddr::V4(addr));
        if let Some(c) = self.conns.get_mut(&conn) {
            c.state = State::Waiting(Waiting {
                since: now,
                user,
                code,
                mode: Mode::Teams,
                target: room.clone(),
                lan: p.lan,
                region,
            });
        }
        if let Some(old) = outcome.replaced {
            self.fail_ticket(out, old, msg::REPLACED, None);
        }
        if outcome.matched.is_empty() {
            return;
        }
        for (c, resp) in &outcome.matched {
            Self::send(out, *c, resp);
        }
        for (c, _) in &outcome.matched {
            self.finish(out, *c);
        }
        self.rooms.announce(&room, out);
    }

    fn on_tick(&mut self, now: Instant, wall: DateTime<Utc>, out: &mut Vec<Output>) {
        self.refresh_feed(now);
        let mut expired = Vec::new();
        let mut auth_late = Vec::new();
        let mut idle = Vec::new();
        let mut waiting = Vec::new();
        for (id, c) in &self.conns {
            match &c.state {
                State::Idle if now.saturating_duration_since(c.since) >= self.cfg.idle_timeout => idle.push(*id),
                State::Waiting(w) if now.saturating_duration_since(w.since) >= self.cfg.ticket_ttl => {
                    expired.push((*id, w.mode, w.target.clone()))
                }
                State::Waiting(w) if w.mode == Mode::Direct => waiting.push(*id),
                // Room game tickets end with their room's start (`RoomBook::tick`).
                State::Validating(p) if now.saturating_duration_since(p.since) >= self.cfg.auth_timeout => {
                    auth_late.push(*id)
                }
                _ => {}
            }
        }
        for id in idle {
            self.refuse(out, id, msg::NO_REQUEST);
        }
        for id in auth_late {
            self.refuse(out, id, msg::UNAVAILABLE);
        }
        for (id, mode, target) in expired {
            let text = if mode == Mode::Direct { msg::did_not_connect(&target) } else { msg::NO_OPPONENT.to_string() };
            self.fail_ticket(out, id, text, None);
        }
        // Retry pairs that were held back by the re-pair backoff, and queued tickets whose
        // region search or rating band has widened.
        waiting.sort_unstable();
        for id in waiting {
            self.try_pair(now, wall, id, out);
        }
        self.pair_queue(Mode::Unranked, now, wall, out);
        self.pair_queue(Mode::Ranked, now, wall, out);
        let window = self.cfg.requeue_window;
        self.history.retain(|_, h| now.saturating_duration_since(h.last_match) < window);
        self.rooms.tick(now, out);
        self.fail_room_tickets(out);
    }

    fn try_pair(&mut self, now: Instant, wall: DateTime<Utc>, conn: ConnId, out: &mut Vec<Output>) {
        let Some(Conn { state: State::Waiting(me), .. }) = self.conns.get(&conn) else { return };
        let Some(&other) = self.direct.get(&(me.target.clone(), me.code.clone())) else { return };
        let Some(Conn { state: State::Waiting(them), .. }) = self.conns.get(&other) else { return };
        let key = pair_key(me.user.uid, them.user.uid);
        let (my_code, their_code) = (me.code.clone(), them.code.clone());

        if let Some(h) = self.history.get(&key).copied() {
            if now.saturating_duration_since(h.last_match) < self.cfg.requeue_window {
                // Both searched for each other again right after a match: the
                // P2P connect failed. Space the retries out, and give up after a few.
                let failures = h.failures + 1;
                if failures >= self.cfg.max_connect_failures {
                    self.history.remove(&key);
                    let n = failures;
                    tracing::info!(%my_code, %their_code, tries = n, "giving up on a pair that cannot connect");
                    self.fail_ticket(out, conn, msg::cannot_connect(&their_code), None);
                    self.fail_ticket(out, other, msg::cannot_connect(&my_code), None);
                    return;
                }
                let not_before = h.last_match + P2P_CONNECT_WINDOW + self.cfg.repair_backoff * failures;
                if now < not_before {
                    return; // on_tick retries
                }
                self.history.insert(key, PairHistory { last_match: now, failures });
            } else {
                self.history.insert(key, PairHistory { last_match: now, failures: 0 });
            }
        } else {
            self.history.insert(key, PairHistory { last_match: now, failures: 0 });
        }
        self.make_match(wall, &[conn, other], out);
    }

    fn queue(&self, mode: Mode) -> &Vec<ConnId> {
        if mode == Mode::Ranked {
            &self.ranked
        } else {
            &self.unranked
        }
    }

    /// Pairs the tickets of a queue (Unranked or Ranked) until no pair is possible.
    fn pair_queue(&mut self, mode: Mode, now: Instant, wall: DateTime<Utc>, out: &mut Vec<Output>) {
        while let Some((a, b, failures)) = self.next_queue_pair(mode, now) {
            let (ua, ub) = match (self.conns.get(&a), self.conns.get(&b)) {
                (Some(Conn { state: State::Waiting(x), .. }), Some(Conn { state: State::Waiting(y), .. })) => {
                    (x.user.uid, y.user.uid)
                }
                _ => break,
            };
            self.history.insert(pair_key(ua, ub), PairHistory { last_match: now, failures });
            let before = self.queue(mode).len();
            self.make_match(wall, &[a, b], out);
            if self.queue(mode).len() == before {
                break; // make_match refused; never loop forever
            }
        }
    }

    /// The largest rating difference a Ranked ticket accepts after waiting `waited`.
    fn rating_band(&self, waited: Duration) -> f64 {
        let steps = if self.cfg.ranked_band_interval.is_zero() {
            0.0
        } else {
            (waited.as_secs_f64() / self.cfg.ranked_band_interval.as_secs_f64()).floor()
        };
        self.cfg.ranked_band + self.cfg.ranked_band_step * steps
    }

    /// The pair a queue makes next: the oldest ticket and a later one it can play. Same region
    /// unless either has waited `region_widen`. Unranked takes the first such ticket in arrival
    /// order; Ranked the closest-rated one within the rating band of the longer-waiting of the
    /// two (ties: arrival order). A pair matched within `requeue_window` failed its P2P connect
    /// (Slippi requeues with a new ticket): anyone else in the queue goes first; the same two
    /// again only after the backoff, and not at all after `max_connect_failures`. Returns
    /// (older, newer, failures so far).
    fn next_queue_pair(&self, mode: Mode, now: Instant) -> Option<(ConnId, ConnId, u32)> {
        let ranked = mode == Mode::Ranked;
        let queue: Vec<(ConnId, &Waiting)> = self
            .queue(mode)
            .iter()
            .filter_map(|id| match self.conns.get(id) {
                Some(Conn { state: State::Waiting(w), .. }) => Some((*id, w)),
                _ => None,
            })
            .collect();
        let waited = |w: &Waiting| now.saturating_duration_since(w.since);
        let widened = |w: &Waiting| waited(w) >= self.cfg.region_widen;
        for (i, (a, wa)) in queue.iter().enumerate() {
            // (ticket, rating difference); Unranked compares 0 everywhere, so the first wins.
            let mut fresh: Option<(ConnId, f64)> = None;
            let mut retry: Option<(ConnId, u32, f64)> = None;
            for (b, wb) in &queue[i + 1..] {
                if wa.user.uid == wb.user.uid {
                    continue;
                }
                if wa.region != wb.region && !widened(wa) && !widened(wb) {
                    continue;
                }
                let diff = if ranked { (wa.user.rating - wb.user.rating).abs() } else { 0.0 };
                if ranked && diff > self.rating_band(waited(wa).max(waited(wb))) {
                    continue;
                }
                let recent = self
                    .history
                    .get(&pair_key(wa.user.uid, wb.user.uid))
                    .filter(|h| now.saturating_duration_since(h.last_match) < self.cfg.requeue_window);
                let Some(h) = recent else {
                    if fresh.is_none_or(|(_, d)| diff < d) {
                        fresh = Some((*b, diff));
                    }
                    continue;
                };
                let failures = h.failures + 1;
                let not_before = h.last_match + P2P_CONNECT_WINDOW + self.cfg.repair_backoff * failures;
                if failures < self.cfg.max_connect_failures
                    && now >= not_before
                    && retry.is_none_or(|(_, _, d)| diff < d)
                {
                    retry = Some((*b, failures, diff));
                }
            }
            if let Some((b, _)) = fresh {
                return Some((*a, b, 0));
            }
            if let Some((b, failures, _)) = retry {
                return Some((*a, b, failures));
            }
        }
        None
    }

    fn make_match(&mut self, wall: DateTime<Utc>, conns: &[ConnId], out: &mut Vec<Output>) {
        let mut entrants: Vec<(ConnId, SocketAddr, Waiting)> = conns
            .iter()
            .filter_map(|id| match self.conns.get(id) {
                Some(Conn { addr, state: State::Waiting(w), .. }) => Some((*id, *addr, w.clone())),
                _ => None,
            })
            .collect();
        if entrants.len() != conns.len() {
            return;
        }
        // Deterministic host ("decider"): the lowest hash of the uid (design 2.3).
        entrants.sort_by_key(|(_, _, w)| uid_hash(w.user.uid));
        let mode = entrants[0].2.mode;
        let rules = self.cfg.rulesets.for_mode(mode);
        self.match_counter += 1;
        let match_id = format!(
            "mode.{}-{}-{:x}",
            mode.name(),
            wall.to_rfc3339_opts(chrono::SecondsFormat::Millis, true),
            self.match_counter
        );
        let host_uid = entrants[0].2.user.uid;
        let players: Vec<Player> = entrants
            .iter()
            .enumerate()
            .map(|(i, (_, addr, w))| Player {
                uid: w.user.uid.to_string(),
                display_name: w.user.display_name.clone(),
                connect_code: w.code.clone(),
                port: (i + 1) as u8,
                is_local_player: false,
                // Never empty: tickets without an IPv4 address are refused in on_create_ticket.
                ip_address: to_v4(*addr).map(|a| a.to_string()).unwrap_or_default(),
                ip_address_lan: w.lan.clone(),
                chat_messages: common::DEFAULT_CHAT_MESSAGES.iter().map(|s| s.to_string()).collect(),
                // Slippi sends each player's rank with a ranked match; the rating is all we have.
                rank: (mode == Mode::Ranked).then(|| Rank {
                    rating: w.user.rating as f32,
                    update_count: w.user.ranked_sets.max(0) as u32,
                    global_placement: 0,
                    regional_placement: 0,
                }),
                is_bot: false,
            })
            .collect();
        let mut regions: Vec<&str> = entrants.iter().map(|(_, _, w)| w.region.as_str()).collect();
        regions.sort_unstable();
        regions.dedup();
        let region = regions.join("+");
        tracing::info!(
            %match_id,
            %region,
            players = ?entrants.iter().map(|(_, a, w)| format!("{}@{}", w.code, a)).collect::<Vec<_>>(),
            "matched"
        );
        for (i, (conn, _, _)) in entrants.iter().enumerate() {
            let mut ps = players.clone();
            ps[i].is_local_player = true;
            let resp = GetTicketResp {
                kind: GET_TICKET_RESP.into(),
                match_id: Some(match_id.clone()),
                is_host: Some(i == 0),
                players: Some(ps),
                stages: Some(rules.stages.clone()),
                starters: (mode == Mode::Ranked && !rules.starters.is_empty()).then(|| rules.starters.clone()),
                items: Some(rules.items),
                ..Default::default()
            };
            Self::send(out, *conn, &resp);
        }
        out.push(Output::RecordMatch(MatchRecord {
            match_id,
            mode,
            players: entrants.iter().map(|(_, _, w)| w.user.uid).collect(),
            host: host_uid,
            stages: rules.stages.clone(),
            region,
        }));
        for (conn, _, _) in &entrants {
            self.finish(out, *conn);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use common::codes::encode_search_code_fullwidth;
    use serde_json::{json, Value};

    struct Harness {
        e: Engine,
        t0: Instant,
        now: Instant,
        secret: PlayKeySecret,
        users: HashMap<Uuid, MmUser>,
    }

    fn user(code: &str, name: &str) -> MmUser {
        MmUser {
            uid: Uuid::new_v4(),
            display_name: name.into(),
            connect_code: Some(code.into()),
            play_key_version: 1,
            banned_until: None,
            email_verified: true,
            rating: common::ranked::DEFAULT_RATING,
            ranked_sets: 0,
        }
    }

    impl Harness {
        fn new(cfg: EngineConfig) -> Self {
            let secret = PlayKeySecret::parse(&"42".repeat(32)).unwrap();
            let t0 = Instant::now();
            Harness { e: Engine::new(cfg, secret.clone()), t0, now: t0, secret, users: HashMap::new() }
        }
        fn add(&mut self, u: &MmUser) {
            self.users.insert(u.uid, u.clone());
        }
        fn input(&mut self, i: Input) -> Vec<Output> {
            self.e.handle(self.now, Utc::now(), i)
        }
        fn advance(&mut self, d: Duration) -> Vec<Output> {
            self.now += d;
            self.input(Input::Tick)
        }
        fn connect(&mut self, conn: ConnId, port: u16) {
            self.connect_from(conn, &format!("203.0.113.{}:{port}", conn));
        }
        fn connect_from(&mut self, conn: ConnId, addr: &str) {
            let addr: SocketAddr = addr.parse().unwrap();
            assert!(self.input(Input::Connected { conn, addr }).is_empty());
        }
        fn ticket_json(&self, u: &MmUser, target: &str, mode: u8) -> Value {
            json!({
                "type": "create-ticket",
                "user": {"uid": u.uid.to_string(), "playKey": self.secret.derive(u.uid, u.play_key_version),
                         "connectCode": u.connect_code, "displayName": u.display_name},
                "search": {"mode": mode, "connectCode": encode_search_code_fullwidth(target)},
                "appVersion": "3.4.0",
                "ipAddressLan": "192.168.1.10:41234"
            })
        }
        /// Sends a ticket and answers the account lookup like the DB would.
        fn ticket(&mut self, conn: ConnId, u: &MmUser, target: &str) -> Vec<Output> {
            let v = self.ticket_json(u, target, 2);
            self.raw(conn, v.to_string().as_bytes())
        }
        fn raw(&mut self, conn: ConnId, data: &[u8]) -> Vec<Output> {
            let mut out = self.input(Input::Packet { conn, data: data.to_vec() });
            let mut i = 0;
            while i < out.len() {
                if let Output::FetchUser { conn, seq, uid } = out[i].clone() {
                    let result = match self.users.get(&uid) {
                        Some(u) => FetchResult::Found(u.clone()),
                        None => FetchResult::NotFound,
                    };
                    let more = self.input(Input::UserFetched { conn, seq, result });
                    out.extend(more);
                }
                i += 1;
            }
            out
        }
    }

    fn sent(out: &[Output], conn: ConnId) -> Vec<Value> {
        out.iter()
            .filter_map(|o| match o {
                Output::Send { conn: c, json } if *c == conn => Some(serde_json::from_str(json).unwrap()),
                _ => None,
            })
            .collect()
    }

    fn disconnected(out: &[Output], conn: ConnId) -> bool {
        out.contains(&Output::Disconnect { conn })
    }

    #[test]
    fn direct_pairing_gives_matching_peer_info() {
        let mut h = Harness::new(EngineConfig::default());
        let (a, b) = (user("AAAA#1", "alice"), user("BB#22", "bob"));
        h.add(&a);
        h.add(&b);
        h.connect(1, 41001);
        h.connect(2, 41002);

        let out = h.ticket(1, &a, "bb#22");
        assert_eq!(sent(&out, 1), vec![json!({"type": "create-ticket-resp"})]);
        assert!(!disconnected(&out, 1));
        assert_eq!(h.e.waiting_count(), 1);

        let out = h.ticket(2, &b, "AAAA#1");
        let ra = sent(&out, 1);
        let rb = sent(&out, 2);
        assert_eq!(ra.len(), 1);
        assert_eq!(rb.len(), 2); // create-ticket-resp, then get-ticket-resp
        let (ga, gb) = (&ra[0], &rb[1]);
        assert_eq!(ga["type"], "get-ticket-resp");
        assert_eq!(ga["matchId"], gb["matchId"]);
        assert!(ga["matchId"].as_str().unwrap().starts_with("mode.direct-"));
        assert_ne!(ga["isHost"], gb["isHost"]);
        assert_eq!(ga["players"].as_array().unwrap().len(), 2);
        // Same player list for both, except the isLocalPlayer flag.
        for i in 0..2 {
            for key in ["uid", "connectCode", "port", "ipAddress", "ipAddressLan", "displayName"] {
                assert_eq!(ga["players"][i][key], gb["players"][i][key]);
            }
            assert_ne!(ga["players"][i]["isLocalPlayer"], gb["players"][i]["isLocalPlayer"]);
        }
        let local_a = ga["players"].as_array().unwrap().iter().find(|p| p["isLocalPlayer"] == true).unwrap();
        assert_eq!(local_a["connectCode"], "AAAA#1");
        assert_eq!(local_a["ipAddress"], "203.0.113.1:41001");
        assert_eq!(local_a["ipAddressLan"], "192.168.1.10:41234");
        // The host takes port 1.
        let host_players = if ga["isHost"] == true { &ga } else { &gb };
        let host_local =
            host_players["players"].as_array().unwrap().iter().find(|p| p["isLocalPlayer"] == true).unwrap();
        assert_eq!(host_local["port"], 1);
        assert!(disconnected(&out, 1) && disconnected(&out, 2));
        assert!(out.iter().any(|o| matches!(o, Output::RecordMatch(m) if m.players.len() == 2)));
        assert_eq!(h.e.waiting_count(), 0);
    }

    #[test]
    fn host_choice_is_deterministic() {
        for _ in 0..20 {
            let mut h = Harness::new(EngineConfig::default());
            let (a, b) = (user("AA#1", "a"), user("BB#1", "b"));
            h.add(&a);
            h.add(&b);
            h.connect(1, 1);
            h.connect(2, 2);
            h.ticket(2, &b, "AA#1");
            let out = h.ticket(1, &a, "BB#1");
            let rec = out
                .iter()
                .find_map(|o| match o {
                    Output::RecordMatch(m) => Some(m.clone()),
                    _ => None,
                })
                .unwrap();
            let expected = if uid_hash(a.uid) < uid_hash(b.uid) { a.uid } else { b.uid };
            assert_eq!(rec.host, expected);
        }
    }

    #[test]
    fn wrong_code_does_not_match_and_expires_with_error() {
        let mut h = Harness::new(EngineConfig { ticket_ttl: Duration::from_secs(30), ..Default::default() });
        let (a, b, c) = (user("AA#1", "a"), user("BB#1", "b"), user("CC#1", "c"));
        for u in [&a, &b, &c] {
            h.add(u);
        }
        h.connect(1, 1);
        h.connect(2, 2);
        h.ticket(1, &a, "BB#1");
        let out = h.ticket(2, &b, "CC#1"); // b names someone else
        assert_eq!(sent(&out, 2), vec![json!({"type": "create-ticket-resp"})]);
        assert!(sent(&out, 1).is_empty());
        assert!(h.advance(Duration::from_secs(29)).is_empty());
        let out = h.advance(Duration::from_secs(1));
        for (conn, target) in [(1, "BB#1"), (2, "CC#1")] {
            let msgs = sent(&out, conn);
            assert_eq!(msgs.len(), 1);
            assert_eq!(msgs[0]["type"], "get-ticket-resp");
            assert_eq!(msgs[0]["error"], msg::did_not_connect(target));
            assert!(disconnected(&out, conn));
        }
        assert_eq!(h.e.waiting_count(), 0);
    }

    #[test]
    fn bad_play_key_and_unknown_account_are_refused() {
        let mut h = Harness::new(EngineConfig::default());
        let a = user("AA#1", "a");
        h.add(&a);
        h.connect(1, 1);
        let mut v = h.ticket_json(&a, "BB#1", 2);
        v["user"]["playKey"] = json!("not-the-key");
        let out = h.raw(1, v.to_string().as_bytes());
        assert_eq!(sent(&out, 1)[0]["error"], msg::LOGIN_EXPIRED);
        assert!(disconnected(&out, 1));

        // Rotated key (password change): the old key stops working.
        let mut rotated = a.clone();
        rotated.play_key_version = 2;
        h.add(&rotated);
        h.connect(2, 2);
        h.now += Duration::from_secs(3);
        let v = h.ticket_json(&a, "BB#1", 2);
        let out = h.raw(2, v.to_string().as_bytes());
        assert_eq!(sent(&out, 2)[0]["error"], msg::LOGIN_EXPIRED);

        let stranger = user("ZZ#9", "z");
        h.connect(3, 3);
        let out = h.ticket(3, &stranger, "AA#1");
        assert_eq!(sent(&out, 3)[0]["error"], msg::ACCOUNT_NOT_FOUND);

        h.connect(4, 4);
        let mut v = h.ticket_json(&a, "BB#1", 2);
        v["user"]["uid"] = json!("not-a-uuid");
        let out = h.raw(4, v.to_string().as_bytes());
        assert_eq!(sent(&out, 4)[0]["error"], msg::NOT_LOGGED_IN);
    }

    #[test]
    fn banned_no_code_and_self_target() {
        let mut h = Harness::new(EngineConfig::default());
        let mut banned = user("BA#1", "b");
        banned.banned_until = Some(Utc::now() + chrono::Duration::days(1));
        let mut nocode = user("XX#1", "n");
        nocode.connect_code = None;
        let me = user("ME#1", "me");
        for u in [&banned, &nocode, &me] {
            h.add(u);
        }
        h.connect(1, 1);
        assert_eq!(sent(&h.ticket(1, &banned, "AA#1"), 1)[0]["error"], msg::BANNED);
        h.connect(2, 2);
        assert_eq!(sent(&h.ticket(2, &nocode, "AA#1"), 2)[0]["error"], msg::NO_CONNECT_CODE);
        h.connect(3, 3);
        let err = &sent(&h.ticket(3, &me, "me#1"), 3)[0]["error"];
        assert_eq!(err, msg::OWN_CODE);
    }

    #[test]
    fn party_is_not_supported_and_teams_needs_a_room() {
        let mut h = Harness::new(EngineConfig::default());
        let a = user("AA#1", "a");
        h.add(&a);
        // Mode 3 is a room's game ticket now: without a live room it is refused.
        for (i, (mode, code, err)) in [
            (3u8, "", msg::ROOM_NOT_FOUND.to_string()),
            (3, "KFQB", msg::ROOM_NOT_FOUND.to_string()),
            (4, "", msg::not_available("Party")),
        ]
        .into_iter()
        .enumerate()
        {
            let conn = 10 + i as u64;
            h.connect(conn, conn as u16);
            let v = h.ticket_json(&a, code, mode);
            let out = h.raw(conn, v.to_string().as_bytes());
            let msgs = sent(&out, conn);
            assert_eq!(msgs[0]["type"], "create-ticket-resp");
            assert_eq!(msgs[0]["error"], err);
            assert!(disconnected(&out, conn));
        }
        h.connect(20, 20);
        let v = h.ticket_json(&a, "BB#1", 9);
        assert_eq!(sent(&h.raw(20, v.to_string().as_bytes()), 20)[0]["error"], "Unknown game mode");
    }

    #[test]
    fn malformed_packets_get_an_error_not_a_crash() {
        let mut h = Harness::new(EngineConfig::default());
        let garbage: Vec<Vec<u8>> = vec![
            vec![],
            b"\x00\x01\x02".to_vec(),
            b"{".to_vec(),
            b"null".to_vec(),
            br#"{"type":"create-ticket","user":5}"#.to_vec(),
            br#"{"type":"create-ticket","user":{"uid":"x","playKey":"y"},"search":{"mode":2,"connectCode":"ABCD#1"}}"#
                .to_vec(),
            br#"{"type":"get-ticket"}"#.to_vec(),
            vec![b'a'; MAX_PACKET + 1],
        ];
        for (i, g) in garbage.into_iter().enumerate() {
            let conn = i as u64 + 1;
            h.connect(conn, 1);
            let out = h.raw(conn, &g);
            let msgs = sent(&out, conn);
            assert_eq!(msgs.len(), 1, "case {i}");
            assert_eq!(msgs[0]["type"], "create-ticket-resp");
            assert!(msgs[0]["error"].is_string());
            assert!(disconnected(&out, conn));
            // Further packets on a finished connection are ignored.
            assert!(h.raw(conn, b"{}").is_empty());
        }
        // Invalid target code.
        let a = user("AA#1", "a");
        h.add(&a);
        h.connect(100, 1);
        let mut v = h.ticket_json(&a, "", 2);
        v["search"]["connectCode"] = json!([0x41, 0x41]);
        assert_eq!(sent(&h.raw(100, v.to_string().as_bytes()), 100)[0]["error"], "Invalid connect code");
        // Packets for unknown connections are ignored.
        assert!(h.input(Input::Packet { conn: 999, data: b"{}".to_vec() }).is_empty());
    }

    #[test]
    fn disconnect_cancels_ticket() {
        let mut h = Harness::new(EngineConfig::default());
        let (a, b) = (user("AA#1", "a"), user("BB#1", "b"));
        h.add(&a);
        h.add(&b);
        h.connect(1, 1);
        h.ticket(1, &a, "BB#1");
        h.input(Input::Disconnected { conn: 1 });
        assert_eq!(h.e.connection_count(), 0);
        h.connect(2, 2);
        let out = h.ticket(2, &b, "AA#1");
        assert_eq!(sent(&out, 2), vec![json!({"type": "create-ticket-resp"})]);
        assert_eq!(h.e.waiting_count(), 1);
    }

    #[test]
    fn newer_ticket_replaces_older_one_for_same_account() {
        let mut h = Harness::new(EngineConfig::default());
        let a = user("AA#1", "a");
        h.add(&a);
        h.connect(1, 1);
        h.connect(2, 2);
        h.ticket(1, &a, "BB#1");
        h.now += Duration::from_secs(3);
        let out = h.ticket(2, &a, "CC#1");
        assert_eq!(sent(&out, 1)[0]["error"], msg::REPLACED);
        assert!(disconnected(&out, 1));
        assert_eq!(sent(&out, 2), vec![json!({"type": "create-ticket-resp"})]);
    }

    #[test]
    fn ticket_rate_limit_per_account() {
        let mut h = Harness::new(EngineConfig::default());
        let a = user("AA#1", "a");
        h.add(&a);
        h.connect(1, 1);
        h.connect(2, 2);
        h.ticket(1, &a, "BB#1");
        let out = h.ticket(2, &a, "BB#1");
        assert_eq!(sent(&out, 2)[0]["error"], msg::SEARCHING_TOO_OFTEN);
    }

    /// Anyone can send a ticket with someone else's uid (opponents see it, `/user/{uid}` is
    /// public). Without the play key such tickets must not use up that account's ticket limit.
    #[test]
    fn uid_rate_limit_needs_the_play_key() {
        let mut h = Harness::new(EngineConfig::default());
        let (victim, b) = (user("VI#1", "victim"), user("BB#1", "b"));
        h.add(&victim);
        h.add(&b);
        for conn in 1..=5 {
            h.connect(conn, 1);
            let mut v = h.ticket_json(&victim, "BB#1", 2);
            v["user"]["playKey"] = json!("x");
            let out = h.raw(conn, v.to_string().as_bytes());
            assert_eq!(sent(&out, conn)[0]["error"], msg::LOGIN_EXPIRED);
        }
        h.connect(10, 1);
        let out = h.ticket(10, &victim, "BB#1");
        assert_eq!(sent(&out, 10), vec![json!({"type": "create-ticket-resp"})]);
        assert_eq!(h.e.waiting_count(), 1);
    }

    /// Tickets per source address are limited before the account lookup, so random uids cannot
    /// flood the database.
    #[test]
    fn ip_ticket_limit_applies_before_the_account_lookup() {
        let mut h = Harness::new(EngineConfig {
            ip_ticket_window: Window::new(3, Duration::from_secs(10)),
            ..Default::default()
        });
        let fetches = |out: &[Output]| out.iter().filter(|o| matches!(o, Output::FetchUser { .. })).count();
        for conn in 1..=3 {
            h.connect_from(conn, &format!("198.51.100.7:{}", 40000 + conn));
            let v = h.ticket_json(&user("RA#1", "r"), "BB#1", 2);
            let out = h.raw(conn, v.to_string().as_bytes());
            assert_eq!(fetches(&out), 1);
            assert_eq!(sent(&out, conn)[0]["error"], msg::ACCOUNT_NOT_FOUND);
        }
        h.connect_from(4, "198.51.100.7:40004");
        let v = h.ticket_json(&user("RA#1", "r"), "BB#1", 2);
        let out = h.raw(4, v.to_string().as_bytes());
        assert_eq!(fetches(&out), 0);
        assert_eq!(sent(&out, 4)[0]["error"], msg::TOO_MANY_SEARCHES);
        assert!(disconnected(&out, 4));
        // Another address is not affected; the limit frees up after its window.
        h.connect_from(5, "198.51.100.8:40000");
        let v = h.ticket_json(&user("RA#1", "r"), "BB#1", 2);
        assert_eq!(fetches(&h.raw(5, v.to_string().as_bytes())), 1);
        h.now += Duration::from_secs(10);
        h.connect_from(6, "198.51.100.7:40006");
        let a = user("AA#1", "a");
        h.add(&a);
        let out = h.ticket(6, &a, "BB#1");
        assert_eq!(sent(&out, 6), vec![json!({"type": "create-ticket-resp"})]);
    }

    /// One account lookup in flight per connection: a second ticket before the first is answered
    /// is refused, and the late answer to the first is ignored.
    #[test]
    fn second_ticket_during_lookup_is_refused() {
        let mut h = Harness::new(EngineConfig::default());
        let a = user("AA#1", "a");
        h.add(&a);
        h.connect(1, 1);
        let v = h.ticket_json(&a, "BB#1", 2);
        let out = h.input(Input::Packet { conn: 1, data: v.to_string().into_bytes() });
        let Some(Output::FetchUser { seq, .. }) = out.last().cloned() else { panic!("{out:?}") };
        let out = h.input(Input::Packet { conn: 1, data: v.to_string().into_bytes() });
        assert!(!out.iter().any(|o| matches!(o, Output::FetchUser { .. })));
        assert_eq!(sent(&out, 1)[0]["error"], msg::INVALID_REQUEST);
        assert!(disconnected(&out, 1));
        assert!(h.input(Input::UserFetched { conn: 1, seq, result: FetchResult::Found(a) }).is_empty());
        assert_eq!(h.e.waiting_count(), 0);
    }

    #[test]
    fn idle_connection_is_closed() {
        let mut h = Harness::new(EngineConfig::default());
        let a = user("AA#1", "a");
        h.add(&a);
        h.connect(1, 1);
        h.connect(2, 2);
        h.ticket(2, &a, "BB#1");
        assert!(h.advance(Duration::from_secs(9)).is_empty());
        let out = h.advance(Duration::from_secs(1));
        assert_eq!(sent(&out, 1), vec![json!({"type": "create-ticket-resp", "error": msg::NO_REQUEST})]);
        assert!(disconnected(&out, 1));
        // The searching connection is not idle.
        assert!(sent(&out, 2).is_empty() && !disconnected(&out, 2));
        assert_eq!(h.e.waiting_count(), 1);
        // A closed connection is not closed again.
        assert!(h.advance(Duration::from_secs(10)).is_empty());
    }

    #[test]
    fn connections_per_address_are_capped() {
        let mut h = Harness::new(EngineConfig { max_conns_per_ip: 3, ..Default::default() });
        for conn in 1..=3 {
            h.connect_from(conn, &format!("198.51.100.7:{}", 40000 + conn));
        }
        let addr: SocketAddr = "198.51.100.7:40004".parse().unwrap();
        let out = h.input(Input::Connected { conn: 4, addr });
        assert_eq!(sent(&out, 4)[0]["error"], msg::TOO_MANY_CONNECTIONS);
        assert!(out.contains(&Output::Close { conn: 4 }));
        assert_eq!(h.e.connection_count(), 3);
        // The refused connection is not tracked: its packets and disconnect are ignored.
        let a = user("AA#1", "a");
        h.add(&a);
        assert!(h.ticket(4, &a, "BB#1").is_empty());
        assert!(h.input(Input::Disconnected { conn: 4 }).is_empty());
        // Other addresses are not affected, and a slot frees up when a connection goes away.
        h.connect_from(5, "198.51.100.8:40000");
        h.input(Input::Disconnected { conn: 1 });
        h.connect_from(6, "198.51.100.7:40006");
        assert_eq!(h.e.connection_count(), 4);

        // IPv6 addresses count per /64; IPv4-mapped ones as their IPv4 address.
        for conn in 10..=12 {
            h.connect_from(conn, &format!("[2001:db8:1:2::{conn}]:41000"));
        }
        let addr: SocketAddr = "[2001:db8:1:2:ffff::1]:41000".parse().unwrap();
        assert!(h.input(Input::Connected { conn: 13, addr }).contains(&Output::Close { conn: 13 }));
        let addr: SocketAddr = "[::ffff:198.51.100.7]:41000".parse().unwrap();
        assert!(h.input(Input::Connected { conn: 14, addr }).contains(&Output::Close { conn: 14 }));
    }

    /// The game cannot use an IPv6 peer address, and an empty `ipAddress` crashes older clients:
    /// such tickets get an explicit error before the account lookup. IPv4-mapped addresses work.
    #[test]
    fn ipv6_tickets_are_refused_ipv4_mapped_work() {
        let mut h = Harness::new(EngineConfig::default());
        let (a, b) = (user("AA#1", "a"), user("BB#1", "b"));
        h.add(&a);
        h.add(&b);
        h.connect_from(1, "[2001:db8::1]:41000");
        let v = h.ticket_json(&a, "BB#1", 2);
        let out = h.raw(1, v.to_string().as_bytes());
        assert!(!out.iter().any(|o| matches!(o, Output::FetchUser { .. })));
        assert_eq!(sent(&out, 1)[0]["error"], msg::NEEDS_IPV4);
        assert!(disconnected(&out, 1));

        h.now += Duration::from_secs(3);
        h.connect_from(2, "[::ffff:198.51.100.9]:41002");
        h.connect(3, 41003);
        h.ticket(2, &a, "BB#1");
        let out = h.ticket(3, &b, "AA#1");
        let g = &sent(&out, 2)[0];
        let ips: Vec<&str> =
            g["players"].as_array().unwrap().iter().map(|p| p["ipAddress"].as_str().unwrap()).collect();
        assert!(ips.contains(&"198.51.100.9:41002"), "{ips:?}");
        assert!(ips.contains(&"203.0.113.3:41003"), "{ips:?}");
    }

    #[test]
    fn slow_account_lookup_times_out_with_error() {
        let mut h = Harness::new(EngineConfig::default());
        let a = user("AA#1", "a");
        h.connect(1, 1);
        let v = h.ticket_json(&a, "BB#1", 2);
        let out = h.input(Input::Packet { conn: 1, data: v.to_string().into_bytes() });
        let seq = match out[0] {
            Output::FetchUser { seq, .. } => seq,
            _ => panic!(),
        };
        let out = h.advance(Duration::from_secs(4));
        assert_eq!(sent(&out, 1)[0]["error"], msg::UNAVAILABLE);
        // A late answer is ignored.
        assert!(h.input(Input::UserFetched { conn: 1, seq, result: FetchResult::Found(a) }).is_empty());
        // A database error is reported the same way.
        let b = user("BB#1", "b");
        h.connect(2, 2);
        let v = h.ticket_json(&b, "AA#1", 2);
        let out = h.input(Input::Packet { conn: 2, data: v.to_string().into_bytes() });
        let seq = match out[0] {
            Output::FetchUser { seq, .. } => seq,
            _ => panic!(),
        };
        let out = h.input(Input::UserFetched { conn: 2, seq, result: FetchResult::Error("down".into()) });
        assert_eq!(sent(&out, 2)[0]["error"], msg::UNAVAILABLE);
    }

    #[test]
    fn version_gate() {
        let mut h = Harness::new(EngineConfig {
            min_app_version: Some("3.5.0".into()),
            latest_version: Some("3.5.2".into()),
            ..Default::default()
        });
        let a = user("AA#1", "a");
        h.add(&a);
        h.connect(1, 1);
        let out = h.ticket(1, &a, "BB#1"); // sends 3.4.0
        assert_eq!(sent(&out, 1)[0]["error"], msg::update_to("3.5.2"));
        assert!(parse_version("3.5.0-beta.1") >= parse_version("3.5.0"));
        assert!(parse_version("v10.0") > parse_version("9.9.9"));
        assert_eq!(parse_version("garbage"), (0, 0, 0));
    }

    /// The Slippi client requeues with a new ticket after its 8 s P2P window
    /// fails. The same two players must not be re-paired straight away, and
    /// after repeated failures both get an explicit error.
    #[test]
    fn failed_connects_back_off_then_error() {
        let mut h = Harness::new(EngineConfig::default());
        let (a, b) = (user("AA#1", "a"), user("BB#1", "b"));
        h.add(&a);
        h.add(&b);
        let mut conn = 0;
        let mut pair = |h: &mut Harness| -> Vec<Output> {
            conn += 2;
            h.connect(conn - 1, 1);
            h.connect(conn, 2);
            let mut out = h.ticket(conn - 1, &a, "BB#1");
            out.extend(h.ticket(conn, &b, "AA#1"));
            out
        };
        let matched = |out: &[Output]| out.iter().any(|o| matches!(o, Output::RecordMatch(_)));

        assert!(matched(&pair(&mut h)), "first search pairs at once");
        let first_match = h.now;

        // Requeue after the 8 s connect window: held back, not re-paired.
        h.now = first_match + Duration::from_secs(9);
        let out = pair(&mut h);
        assert!(!matched(&out));
        assert_eq!(h.e.waiting_count(), 2);
        // Released at last_match + 8 s + 5 s.
        assert!(!matched(&h.advance(Duration::from_secs(3))));
        let out = h.advance(Duration::from_secs(1));
        assert!(matched(&out), "re-paired after the backoff");
        let second = h.now;

        // Second failure: + 10 s.
        h.now = second + Duration::from_secs(9);
        assert!(!matched(&pair(&mut h)));
        h.now = second + Duration::from_secs(17);
        assert!(!matched(&h.advance(Duration::ZERO)));
        h.now = second + Duration::from_secs(18);
        assert!(matched(&h.advance(Duration::ZERO)));
        let third = h.now;

        // Third failure: give up with an explicit error to both.
        h.now = third + Duration::from_secs(9);
        let out = pair(&mut h);
        assert!(!matched(&out));
        let errors: Vec<String> = out
            .iter()
            .filter_map(|o| match o {
                Output::Send { json, .. } => {
                    serde_json::from_str::<Value>(json).ok()?["error"].as_str().map(String::from)
                }
                _ => None,
            })
            .collect();
        assert!(errors.contains(&msg::cannot_connect("BB#1")));
        assert!(errors.contains(&msg::cannot_connect("AA#1")));
        assert_eq!(h.e.waiting_count(), 0);

        // Searching again later starts fresh.
        h.now += Duration::from_secs(120);
        h.advance(Duration::ZERO);
        assert!(matched(&pair(&mut h)));
        let _ = h.t0;
    }

    #[test]
    fn rematch_long_after_a_good_connection_is_immediate() {
        let mut h = Harness::new(EngineConfig::default());
        let (a, b) = (user("AA#1", "a"), user("BB#1", "b"));
        h.add(&a);
        h.add(&b);
        h.connect(1, 1);
        h.connect(2, 2);
        h.ticket(1, &a, "BB#1");
        assert!(h.ticket(2, &b, "AA#1").iter().any(|o| matches!(o, Output::RecordMatch(_))));
        h.advance(Duration::from_secs(61));
        h.connect(3, 1);
        h.connect(4, 2);
        h.ticket(3, &a, "BB#1");
        assert!(h.ticket(4, &b, "AA#1").iter().any(|o| matches!(o, Output::RecordMatch(_))));
    }

    // ------------------------------------------------------------------ Unranked

    impl Harness {
        /// An Unranked ticket as Slippi's client sends it (mode 1, `connectCode: []`).
        fn unranked(&mut self, conn: ConnId, u: &MmUser) -> Vec<Output> {
            let v = self.ticket_json(u, "", 1);
            self.raw(conn, v.to_string().as_bytes())
        }
        fn users(&mut self, n: usize) -> Vec<MmUser> {
            (0..n)
                .map(|i| {
                    let u = user(&format!("U{}#{}", (b'A' + i as u8) as char, i + 1), &format!("p{i}"));
                    self.add(&u);
                    u
                })
                .collect()
        }
    }

    fn matches(out: &[Output]) -> Vec<MatchRecord> {
        out.iter()
            .filter_map(|o| match o {
                Output::RecordMatch(m) => Some(m.clone()),
                _ => None,
            })
            .collect()
    }

    fn pair_of(m: &MatchRecord) -> (Uuid, Uuid) {
        pair_key(m.players[0], m.players[1])
    }

    fn default_rules() -> EngineConfig {
        EngineConfig { rulesets: Rulesets::parse(crate::ruleset::DEFAULT_RULESETS).unwrap(), ..Default::default() }
    }

    #[test]
    fn unranked_pairs_strangers_in_arrival_order() {
        let mut h = Harness::new(default_rules());
        let u = h.users(4);
        for c in 1..=4 {
            h.connect(c, 41000 + c as u16);
        }
        let out = h.unranked(1, &u[0]);
        assert_eq!(sent(&out, 1), vec![json!({"type": "create-ticket-resp"})]);
        assert!(!disconnected(&out, 1));
        h.now += Duration::from_secs(1);

        let out = h.unranked(2, &u[1]);
        let m = matches(&out);
        assert_eq!(m.len(), 1);
        assert_eq!(pair_of(&m[0]), pair_key(u[0].uid, u[1].uid));
        assert_eq!(m[0].mode, Mode::Unranked);
        assert_eq!(m[0].region, crate::region::DEFAULT_REGION);
        let (ga, gb) = (&sent(&out, 1)[0], &sent(&out, 2)[1]);
        for g in [ga, gb] {
            assert_eq!(g["type"], "get-ticket-resp");
            assert!(g["matchId"].as_str().unwrap().starts_with("mode.unranked-"), "{g}");
            assert_eq!(g["players"].as_array().unwrap().len(), 2);
            assert_eq!(g["players"].as_array().unwrap().iter().filter(|p| p["isLocalPlayer"] == true).count(), 1);
            assert!(g.get("starters").is_none(), "only Ranked strikes starters: {g}");
            // The ruleset's stage list, as Slippi's server sends it for the mode.
            assert_eq!(g["stages"], json!([1, 2, 3, 4, 5, 6, 9, 12, 13, 28, 31, 33, 35, 45, 46]));
            assert_eq!(g["items"], 0);
            for p in g["players"].as_array().unwrap() {
                assert_eq!(p["chatMessages"].as_array().unwrap().len(), 16);
                assert!(p["ipAddress"].as_str().unwrap().starts_with("203.0.113."));
            }
        }
        assert_eq!(ga["matchId"], gb["matchId"]);
        assert_ne!(ga["isHost"], gb["isHost"]);
        assert!(disconnected(&out, 1) && disconnected(&out, 2));

        // The third waits; the fourth is paired with it.
        assert!(matches(&h.unranked(3, &u[2])).is_empty());
        assert_eq!(h.e.waiting_count(), 1);
        let m = matches(&h.unranked(4, &u[3]));
        assert_eq!(pair_of(&m[0]), pair_key(u[2].uid, u[3].uid));
        assert_eq!(h.e.waiting_count(), 0);
    }

    #[test]
    fn unranked_ignores_the_code_and_never_meets_direct() {
        let mut h = Harness::new(EngineConfig::default());
        let u = h.users(3);
        for c in 1..=3 {
            h.connect(c, c as u16);
        }
        // A Direct ticket naming u1's code does not take u1's Unranked ticket, nor the reverse.
        h.ticket(1, &u[0], &u[1].connect_code.clone().unwrap());
        // Whatever bytes an Unranked ticket carries as its code are ignored.
        let mut v = h.ticket_json(&u[1], "", 1);
        v["search"]["connectCode"] = json!([0x41, 0x41]);
        let out = h.raw(2, v.to_string().as_bytes());
        assert_eq!(sent(&out, 2), vec![json!({"type": "create-ticket-resp"})]);
        assert!(matches(&out).is_empty());
        assert_eq!(h.e.waiting_count(), 2);
        let m = matches(&h.unranked(3, &u[2]));
        assert_eq!(pair_of(&m[0]), pair_key(u[1].uid, u[2].uid));
        assert_eq!(h.e.waiting_count(), 1, "the Direct ticket still waits");
    }

    #[test]
    fn unranked_expiry_is_an_explicit_error() {
        let mut h = Harness::new(EngineConfig { ticket_ttl: Duration::from_secs(30), ..Default::default() });
        let u = h.users(1);
        h.connect(1, 1);
        h.unranked(1, &u[0]);
        assert!(h.advance(Duration::from_secs(29)).is_empty());
        let out = h.advance(Duration::from_secs(1));
        let msgs = sent(&out, 1);
        assert_eq!(msgs.len(), 1);
        assert_eq!(msgs[0]["type"], "get-ticket-resp");
        assert_eq!(msgs[0]["error"], msg::NO_OPPONENT);
        assert!(disconnected(&out, 1));
        assert_eq!(h.e.waiting_count(), 0);
    }

    #[test]
    fn unranked_cancel_and_replacement() {
        let mut h = Harness::new(EngineConfig::default());
        let u = h.users(3);
        for c in 1..=5 {
            h.connect(c, c as u16);
        }
        // Cancelled (the CSS's Z disconnects): never paired.
        h.unranked(1, &u[0]);
        h.input(Input::Disconnected { conn: 1 });
        assert!(matches(&h.unranked(2, &u[1])).is_empty());
        // A second search from the same account replaces the first, and is not paired with it.
        h.now += Duration::from_secs(3);
        let out = h.unranked(3, &u[1]);
        assert_eq!(sent(&out, 2)[0]["error"], msg::REPLACED);
        assert!(matches(&out).is_empty());
        assert_eq!(h.e.waiting_count(), 1);
        let m = matches(&h.unranked(4, &u[2]));
        assert_eq!(pair_of(&m[0]), pair_key(u[1].uid, u[2].uid));
    }

    /// Slippi's client requeues with a new ticket when the P2P connect fails. The two players who
    /// just failed are not paired again while someone else is searching, and only after a backoff
    /// when they are alone.
    #[test]
    fn unranked_failed_connect_prefers_someone_else() {
        let mut h = Harness::new(EngineConfig::default());
        let u = h.users(4);
        for c in 1..=8 {
            h.connect(c, c as u16);
        }
        h.unranked(1, &u[0]);
        let t = h.now;
        assert_eq!(matches(&h.unranked(2, &u[1])).len(), 1);

        // Both requeue after the 8 s window: held back from each other.
        h.now = t + Duration::from_secs(9);
        assert!(matches(&h.unranked(3, &u[0])).is_empty());
        assert!(matches(&h.unranked(4, &u[1])).is_empty());
        assert_eq!(h.e.waiting_count(), 2);
        // A third player arrives: the oldest of the two gets them.
        h.now = t + Duration::from_secs(10);
        let m = matches(&h.unranked(5, &u[2]));
        assert_eq!(pair_of(&m[0]), pair_key(u[0].uid, u[2].uid));
        h.now = t + Duration::from_secs(11);
        let m = matches(&h.unranked(6, &u[3]));
        assert_eq!(pair_of(&m[0]), pair_key(u[1].uid, u[3].uid));
    }

    #[test]
    fn unranked_failed_pair_backs_off_then_stops_pairing_them() {
        let mut h = Harness::new(EngineConfig::default());
        let u = h.users(3);
        let mut conn = 0;
        let mut requeue = |h: &mut Harness| -> Vec<Output> {
            conn += 2;
            h.connect(conn - 1, 1);
            h.connect(conn, 2);
            let mut out = h.unranked(conn - 1, &u[0]);
            out.extend(h.unranked(conn, &u[1]));
            out
        };
        assert_eq!(matches(&requeue(&mut h)).len(), 1);
        let first = h.now;

        // Alone together after a failed connect: re-paired at last match + 8 s + 5 s.
        h.now = first + Duration::from_secs(9);
        assert!(matches(&requeue(&mut h)).is_empty());
        h.now = first + Duration::from_secs(12);
        assert!(matches(&h.advance(Duration::ZERO)).is_empty());
        h.now = first + Duration::from_secs(13);
        assert_eq!(matches(&h.advance(Duration::ZERO)).len(), 1);
        let second = h.now;

        // Second failure: + 10 s.
        h.now = second + Duration::from_secs(9);
        assert!(matches(&requeue(&mut h)).is_empty());
        h.now = second + Duration::from_secs(17);
        assert!(matches(&h.advance(Duration::ZERO)).is_empty());
        h.now = second + Duration::from_secs(18);
        assert_eq!(matches(&h.advance(Duration::ZERO)).len(), 1);
        let third = h.now;

        // Third failure: no more pairing of these two, but no error either: they keep searching
        // and anyone else is welcome.
        h.now = third + Duration::from_secs(9);
        let out = requeue(&mut h);
        assert!(matches(&out).is_empty());
        assert!(out.iter().all(|o| !matches!(o, Output::Send { json, .. } if json.contains("error"))));
        h.now = third + Duration::from_secs(50);
        assert!(matches(&h.advance(Duration::ZERO)).is_empty());
        assert_eq!(h.e.waiting_count(), 2);
        h.connect(100, 3);
        let m = matches(&h.unranked(100, &u[2]));
        assert_eq!(pair_of(&m[0]), pair_key(u[0].uid, u[2].uid));
        // Once the window has passed they are strangers again.
        h.now = third + Duration::from_secs(61);
        h.advance(Duration::ZERO);
        let m = matches(&requeue(&mut h));
        assert_eq!(m.len(), 1);
        assert_eq!(pair_of(&m[0]), pair_key(u[0].uid, u[1].uid));
    }

    #[test]
    fn unranked_regions_then_widening() {
        let regions = RegionMap::parse(r#"{"na": ["203.0.113.0/28"], "eu": ["203.0.113.16/28"]}"#).unwrap();
        let mut h = Harness::new(EngineConfig { regions, ..Default::default() });
        let u = h.users(6);
        // conn n connects from 203.0.113.n: 1-15 are na, 16-31 eu.
        for c in [1, 2, 3, 16, 17, 18] {
            h.connect(c, 41000);
        }
        h.unranked(1, &u[0]); // na
        assert!(matches(&h.unranked(16, &u[1])).is_empty()); // eu: not with na yet
        let m = matches(&h.unranked(2, &u[2])); // na: takes the na ticket, not the older eu one
        assert_eq!(pair_of(&m[0]), pair_key(u[0].uid, u[2].uid));
        assert_eq!(m[0].region, "na");
        let m = matches(&h.unranked(17, &u[3]));
        assert_eq!(pair_of(&m[0]), pair_key(u[1].uid, u[3].uid));
        assert_eq!(m[0].region, "eu");

        // One na, one eu: they wait 30 s for their own region, then take each other.
        h.unranked(3, &u[4]);
        h.now += Duration::from_secs(5);
        assert!(matches(&h.unranked(18, &u[5])).is_empty());
        assert!(matches(&h.advance(Duration::from_secs(24))).is_empty());
        let m = matches(&h.advance(Duration::from_secs(1)));
        assert_eq!(m.len(), 1);
        assert_eq!(pair_of(&m[0]), pair_key(u[4].uid, u[5].uid));
        assert_eq!(m[0].region, "eu+na");
    }

    #[test]
    fn stages_come_from_ruleset() {
        let rulesets = Rulesets::parse(r#"{"direct": {"stages": [1, 2, 3], "items": 0}}"#).unwrap();
        let mut h = Harness::new(EngineConfig { rulesets, ..Default::default() });
        let (a, b) = (user("AA#1", "a"), user("BB#1", "b"));
        h.add(&a);
        h.add(&b);
        h.connect(1, 1);
        h.connect(2, 2);
        h.ticket(1, &a, "BB#1");
        let out = h.ticket(2, &b, "AA#1");
        assert_eq!(sent(&out, 1)[0]["stages"], json!([1, 2, 3]));
    }

    // ------------------------------------------------------------------ Ranked

    impl Harness {
        /// A Ranked ticket (mode 0, `connectCode: []`).
        fn ranked(&mut self, conn: ConnId, u: &MmUser) -> Vec<Output> {
            let v = self.ticket_json(u, "", 0);
            self.raw(conn, v.to_string().as_bytes())
        }
        /// Players with these ratings.
        fn rated(&mut self, ratings: &[f64]) -> Vec<MmUser> {
            ratings
                .iter()
                .enumerate()
                .map(|(i, r)| {
                    let mut u = user(&format!("R{}#{}", (b'A' + i as u8) as char, i + 1), &format!("r{i}"));
                    u.rating = *r;
                    u.ranked_sets = 12;
                    self.add(&u);
                    u
                })
                .collect()
        }
    }

    #[test]
    fn ranked_pairs_close_ratings_and_sends_them() {
        let mut h = Harness::new(default_rules());
        let u = h.rated(&[1400.0, 1900.0, 1500.0]);
        for c in 1..=3 {
            h.connect(c, 41000 + c as u16);
        }
        assert!(matches(&h.ranked(1, &u[0])).is_empty());
        // 500 apart: outside the band.
        assert!(matches(&h.ranked(2, &u[1])).is_empty());
        let out = h.ranked(3, &u[2]);
        let m = matches(&out);
        assert_eq!(m.len(), 1);
        assert_eq!(m[0].mode, Mode::Ranked);
        assert_eq!(pair_of(&m[0]), pair_key(u[0].uid, u[2].uid));
        let g = &sent(&out, 3)[1];
        assert!(g["matchId"].as_str().unwrap().starts_with("mode.ranked-"), "{g}");
        assert_eq!(g["stages"].as_array().unwrap().len(), 15);
        // The starters struck for game 1: P+'s Battlefield, FD, Smashville, Dream Land, PS2.
        assert_eq!(g["starters"], json!([1, 2, 33, 45, 46]));
        let ps = g["players"].as_array().unwrap();
        for p in ps {
            let me = if p["uid"] == u[0].uid.to_string() { &u[0] } else { &u[2] };
            assert_eq!(p["rank"]["rating"].as_f64().unwrap(), me.rating);
            assert_eq!(p["rank"]["updateCount"], 12);
        }
        // The unmatched one keeps waiting, and never meets the Unranked queue.
        let v = h.users(1);
        h.connect(4, 41004);
        assert!(matches(&h.unranked(4, &v[0])).is_empty());
        assert_eq!(h.e.waiting_count(), 2);
    }

    #[test]
    fn ranked_takes_the_closest_rating_in_the_band() {
        let mut h = Harness::new(default_rules());
        let u = h.rated(&[1500.0, 1400.0, 1360.0, 1610.0]);
        for c in 1..=4 {
            h.connect(c, 41000 + c as u16);
        }
        // 1500 waits; 1400 is 100 away, inside ±150.
        h.ranked(1, &u[0]);
        let m = matches(&h.ranked(2, &u[1]));
        assert_eq!(pair_of(&m[0]), pair_key(u[0].uid, u[1].uid));
        // Now 1360 waits; 1610 comes: 250 apart, outside ±150 at first.
        h.ranked(3, &u[2]);
        assert!(matches(&h.ranked(4, &u[3])).is_empty());
        // The band grows 50 every 15 s of the longer wait: 250 needs 30 s (150 + 2 × 50).
        assert!(matches(&h.advance(Duration::from_secs(29))).is_empty());
        let m = matches(&h.advance(Duration::from_secs(1)));
        assert_eq!(m.len(), 1);
        assert_eq!(pair_of(&m[0]), pair_key(u[2].uid, u[3].uid));
    }

    #[test]
    fn ranked_prefers_the_closer_of_several_opponents() {
        let mut h = Harness::new(default_rules());
        let u = h.rated(&[1500.0, 1700.0, 1450.0, 1520.0]);
        for c in 1..=4 {
            h.connect(c, 41000 + c as u16);
        }
        h.ranked(1, &u[0]);
        h.ranked(2, &u[1]);
        // After 15 s the band is ±200, so 1700 would fit 1500 now; but the next ticket, 1450, is
        // closer, and the closest goes first.
        h.now += Duration::from_secs(15);
        let m = matches(&h.ranked(3, &u[2]));
        assert_eq!(m.len(), 1);
        assert_eq!(pair_of(&m[0]), pair_key(u[0].uid, u[2].uid));
        let m = matches(&h.ranked(4, &u[3]));
        // 1700 and 1520 (180 apart, 1700 waited 15 s: ±200) take each other.
        assert_eq!(m.len(), 1);
        assert_eq!(pair_of(&m[0]), pair_key(u[1].uid, u[3].uid));
    }

    impl Harness {
        fn hello(&mut self, conn: ConnId, u: &MmUser, version: &str) -> Vec<Output> {
            let v = json!({
                "type": "hello",
                "user": {"uid": u.uid.to_string(), "playKey": self.secret.derive(u.uid, u.play_key_version)},
                "appVersion": version,
            });
            self.raw(conn, v.to_string().as_bytes())
        }
        fn room(&mut self, conn: ConnId, v: Value) -> Vec<Output> {
            self.raw(conn, v.to_string().as_bytes())
        }
    }

    fn state_of(out: &[Output], conn: ConnId) -> Value {
        sent(out, conn).into_iter().rev().find(|m| m["type"] == "room-state").expect("room-state")
    }

    #[test]
    fn hello_opens_an_online_connection() {
        let mut h = Harness::new(EngineConfig::default());
        let (a, b) = (user("AA#1", "a"), user("BB#1", "b"));
        h.add(&a);
        h.add(&b);
        h.connect(1, 1);
        h.connect(2, 2);
        let out = h.hello(1, &a, "3.4.0");
        assert_eq!(sent(&out, 1), vec![json!({"type": "hello-resp"})]);
        assert!(!disconnected(&out, 1));
        h.hello(2, &b, "3.4.0");
        assert_eq!(h.e.online_count(), 2);
        // Online connections never hit the idle timeout (ENet's own keepalive times them out).
        assert!(!disconnected(&h.advance(Duration::from_secs(60)), 1));
        // A second game with the same account replaces the first.
        h.connect(3, 3);
        let out = h.hello(3, &a, "3.4.0");
        assert_eq!(sent(&out, 1)[0], json!({"type": "error", "error": msg::SIGNED_IN_ELSEWHERE}));
        assert!(disconnected(&out, 1));
        assert_eq!(h.e.online_count(), 2);
        h.input(Input::Disconnected { conn: 2 });
        assert_eq!(h.e.online_count(), 1);
        assert_eq!(h.e.status(Utc::now()).online, 1);
        // A ticket on an online connection is refused; the connection stays.
        let v = h.ticket_json(&a, "BB#1", 2);
        let out = h.raw(3, v.to_string().as_bytes());
        assert_eq!(sent(&out, 3)[0]["error"], msg::INVALID_REQUEST);
        assert!(!disconnected(&out, 3));
        // Garbage gets a room-error, not a disconnect.
        let out = h.raw(3, b"{");
        assert_eq!(sent(&out, 3)[0]["type"], "room-error");
        assert!(!disconnected(&out, 3));
    }

    fn hello_on(h: &mut Harness, conn: ConnId, u: &MmUser, version: &str, platform: &str) -> Vec<Output> {
        let v = json!({
            "type": "hello",
            "user": {"uid": u.uid.to_string(), "playKey": h.secret.derive(u.uid, u.play_key_version)},
            "appVersion": version,
            "platform": platform,
        });
        h.raw(conn, v.to_string().as_bytes())
    }

    #[test]
    fn each_platform_must_have_its_newest_build() {
        let mut h = Harness::new(EngineConfig::default());
        // The macOS build of 3.6.0 still waits for Apple.
        h.e.set_feed_versions(FeedVersions::from_pairs(&[("win", "3.6.0"), ("mac", "3.5.0"), ("linux", "3.6.0")]));
        let (a, b, c) = (user("AA#1", "a"), user("BB#1", "b"), user("CC#1", "c"));
        for u in [&a, &b, &c] {
            h.add(u);
        }
        h.connect(1, 1);
        let out = hello_on(&mut h, 1, &a, "3.5.0", "win");
        assert_eq!(
            sent(&out, 1),
            vec![json!({"type": "hello-resp", "error": msg::update_to("3.6.0"), "latestVersion": "3.6.0"})]
        );
        assert!(disconnected(&out, 1));
        h.connect(2, 2);
        let out = hello_on(&mut h, 2, &b, "3.5.0", "mac");
        assert_eq!(sent(&out, 2), vec![json!({"type": "hello-resp"})]);
        // A build older than the platform field is held to the oldest feed.
        h.connect(3, 3);
        let out = h.hello(3, &c, "3.5.0");
        assert_eq!(sent(&out, 3), vec![json!({"type": "hello-resp"})]);

        // Tickets (Direct, Unranked, a room's game) the same way.
        h.connect(4, 4);
        let mut v = h.ticket_json(&a, "BB#1", 2);
        v["appVersion"] = json!("3.5.0");
        v["platform"] = json!("win");
        let out = h.raw(4, v.to_string().as_bytes());
        assert_eq!(sent(&out, 4), vec![json!({"type": "create-ticket-resp", "error": msg::update_to("3.6.0")})]);
        h.connect(5, 5);
        v["appVersion"] = json!("3.6.0");
        let out = h.raw(5, v.to_string().as_bytes());
        assert_eq!(sent(&out, 5), vec![json!({"type": "create-ticket-resp"})]);
    }

    #[test]
    fn a_game_left_running_across_a_release_cannot_ready() {
        let mut h = Harness::new(EngineConfig::default());
        h.e.set_feed_versions(FeedVersions::from_pairs(&[("win", "3.5.0"), ("mac", "3.5.0")]));
        let (a, b) = (user("AA#1", "a"), user("BB#1", "b"));
        h.add(&a);
        h.add(&b);
        for (conn, u) in [(1, &a), (2, &b)] {
            h.connect(conn, conn as u16);
            assert_eq!(sent(&hello_on(&mut h, conn, u, "3.5.0", "win"), conn), vec![json!({"type": "hello-resp"})]);
        }
        let out = h.room(1, json!({"type": "room-create"}));
        let code = state_of(&out, 1)["code"].as_str().unwrap().to_string();
        h.room(2, json!({"type": "room-join", "code": code}));
        // 3.6.0 is published for Windows while both games run 3.5.0.
        h.e.set_feed_versions(FeedVersions::from_pairs(&[("win", "3.6.0"), ("mac", "3.5.0")]));
        let out = h.room(1, json!({"type": "room-ready", "ready": true, "character": 3}));
        assert_eq!(
            sent(&out, 1),
            vec![json!({"type": "room-error", "op": "room-ready", "error": msg::update_to("3.6.0")})]
        );
        assert!(sent(&out, 2).is_empty(), "the room did not see a ready");
        assert!(!disconnected(&out, 1));
        // Taking the ready back and leaving still work.
        let out = h.room(1, json!({"type": "room-ready", "ready": false}));
        assert!(sent(&out, 1).iter().all(|m| m["type"] != "room-error"));
        let out = h.room(2, json!({"type": "room-leave"}));
        assert!(sent(&out, 2).iter().all(|m| m["type"] != "room-error"));
    }

    #[test]
    fn the_update_feed_is_read_again_while_mm_runs() {
        let dir = std::env::temp_dir().join(format!("mm-engine-feed-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("latest.yml"), "version: 3.6.0\n").unwrap();
        let mut h = Harness::new(EngineConfig { update_feed_dir: Some(dir.clone()), ..Default::default() });
        let a = user("AA#1", "a");
        h.add(&a);
        h.advance(Duration::from_millis(100));
        h.connect(1, 1);
        assert_eq!(sent(&hello_on(&mut h, 1, &a, "3.5.0", "win"), 1)[0]["error"], msg::update_to("3.6.0"));
        // Rolled back to 3.5.0: let in after the next read, not before.
        std::fs::write(dir.join("latest.yml"), "version: 3.5.0\n").unwrap();
        h.advance(Duration::from_secs(1));
        h.connect(2, 2);
        assert_eq!(sent(&hello_on(&mut h, 2, &a, "3.5.0", "win"), 2)[0]["error"], msg::update_to("3.6.0"));
        h.advance(FEED_REFRESH);
        h.connect(3, 3);
        assert_eq!(sent(&hello_on(&mut h, 3, &a, "3.5.0", "win"), 3), vec![json!({"type": "hello-resp"})]);
        // A feed folder that reads empty keeps the last versions.
        std::fs::remove_dir_all(&dir).unwrap();
        h.advance(FEED_REFRESH);
        h.e.cfg.min_app_version = None;
        h.connect(4, 4);
        assert_eq!(sent(&hello_on(&mut h, 4, &a, "3.4.0", "win"), 4)[0]["error"], msg::update_to("3.5.0"));
    }

    #[test]
    fn hello_is_checked_like_a_ticket() {
        let mut h = Harness::new(EngineConfig {
            min_app_version: Some("3.5.0".into()),
            latest_version: Some("3.5.2".into()),
            ..Default::default()
        });
        let a = user("AA#1", "a");
        h.add(&a);
        h.connect(1, 1);
        let out = h.hello(1, &a, "3.4.0");
        assert_eq!(
            sent(&out, 1),
            vec![json!({"type": "hello-resp", "error": msg::update_to("3.5.2"), "latestVersion": "3.5.2"})]
        );
        assert!(disconnected(&out, 1));
        h.connect(2, 2);
        let mut rotated = a.clone();
        rotated.play_key_version = 2;
        h.add(&rotated);
        let out = h.hello(2, &a, "3.5.0");
        assert_eq!(sent(&out, 2)[0]["error"], msg::LOGIN_EXPIRED);
        assert_eq!(h.e.online_count(), 0);
        // Room requests need `hello` first.
        h.connect(3, 3);
        let out = h.room(3, json!({"type": "room-create"}));
        assert_eq!(sent(&out, 3)[0], json!({"type": "error", "error": msg::NOT_LOGGED_IN}));
        assert!(disconnected(&out, 3));
    }

    #[test]
    fn a_room_game_starts_with_tickets() {
        // Rooms play with Direct's ruleset.
        let mut h = Harness::new(EngineConfig { rulesets: Rulesets::load(None).unwrap(), ..Default::default() });
        let (a, b, c) = (user("AA#1", "alice"), user("BB#1", "bob"), user("CC#1", "carol"));
        for u in [&a, &b, &c] {
            h.add(u);
        }
        for (conn, u) in [(1, &a), (2, &b), (3, &c)] {
            h.connect(conn, conn as u16);
            h.hello(conn, u, "3.4.0");
        }
        let out = h.room(1, json!({"type": "room-create"}));
        let code = state_of(&out, 1)["code"].as_str().unwrap().to_string();
        let out = h.room(2, json!({"type": "room-join", "code": code.to_lowercase()}));
        assert_eq!(state_of(&out, 1)["slots"][1]["player"]["connectCode"], "BB#1");
        let status = h.e.status(Utc::now());
        assert_eq!(status.rooms.len(), 1);
        assert_eq!(status.rooms[0].names, vec!["alice", "bob"]);
        h.room(1, json!({"type": "room-ready", "ready": true}));
        let out = h.room(2, json!({"type": "room-ready", "ready": true, "character": 3, "costume": 1}));
        for conn in [1, 2] {
            assert!(sent(&out, conn).iter().any(|m| m["type"] == "room-start" && m["code"] == code.as_str()));
        }
        // carol is not a member; her ticket for the room is refused.
        h.connect(13, 13);
        let v = h.ticket_json(&c, &code, 3);
        assert_eq!(sent(&h.raw(13, v.to_string().as_bytes()), 13)[0]["error"], msg::NOT_A_MEMBER);
        // The members' tickets, from their P2P ports (the code full width, as the keypad sends it).
        h.connect(11, 41011);
        h.connect(12, 41012);
        let v = h.ticket_json(&a, &code, 3);
        let out = h.raw(11, v.to_string().as_bytes());
        assert_eq!(sent(&out, 11), vec![json!({"type": "create-ticket-resp"})]);
        let v = h.ticket_json(&b, &code, 3);
        let out = h.raw(12, v.to_string().as_bytes());
        for (conn, port) in [(11, 1), (12, 2)] {
            let resp = sent(&out, conn).into_iter().find(|m| m["type"] == "get-ticket-resp").unwrap();
            assert!(resp["matchId"].as_str().unwrap().starts_with(&format!("mode.room-{code}-")));
            assert_eq!(resp["isHost"], conn == 11);
            let players = resp["players"].as_array().unwrap();
            assert_eq!(players.len(), 2);
            let local = players.iter().find(|p| p["isLocalPlayer"] == true).unwrap();
            assert_eq!(local["port"], port);
            assert_eq!(players[1]["ipAddress"], "203.0.113.12:41012");
            assert_eq!(resp["stages"].as_array().unwrap().len(), 15);
            assert!(disconnected(&out, conn));
        }
        assert_eq!(state_of(&out, 1)["status"], "in-game");
        assert_eq!(h.e.status(Utc::now()).rooms[0].status, common::rooms::RoomStatus::InGame);
        // The ticket connections closing does not touch the room.
        h.input(Input::Disconnected { conn: 11 });
        h.input(Input::Disconnected { conn: 12 });
        h.room(1, json!({"type": "room-back"}));
        let out = h.room(2, json!({"type": "room-back"}));
        assert_eq!(state_of(&out, 2)["status"], "waiting");
        // The host's game closes: bob is the host, and the room stays.
        let mut out = h.input(Input::Disconnected { conn: 1 });
        out.extend(h.room(2, json!({"type": "room-public", "public": false})));
        let s = state_of(&out, 2);
        assert_eq!((s["host"].as_u64(), s["public"].as_bool()), (Some(2), Some(false)));
        assert!(h.e.status(Utc::now()).rooms.is_empty());
    }
}
