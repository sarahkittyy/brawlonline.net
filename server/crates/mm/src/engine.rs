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
//!   +-- (client disconnect at any point removes the connection)
//! ```
//!
//! Every refusal is an explicit `error` the game shows: tickets are never
//! dropped silently.
//!
//! Two queues:
//! - **Direct**: two tickets that name each other's codes are paired.
//! - **Unranked**: a FIFO. The oldest waiting ticket is paired with the next one in arrival order
//!   that is in its region (any region once either has waited `region_widen`) and that it did not
//!   just fail to connect to. Slippi's tickets carry no region field, so the region comes from the
//!   ticket's source address ([`crate::region`]); with no region table there is one bucket.
//!
//! Both queues share the failed-connect rule: Slippi's 1v1 client requeues with a new ticket when
//! its 8 s P2P window fails, so a pair matched again within `requeue_window` is taken to have
//! failed. Direct holds such a pair back (P2P window + `repair_backoff` × failures) and errors
//! after `max_connect_failures`; Unranked pairs each of them with someone else when it can, re-pairs
//! them only after the same backoff, and after `max_connect_failures` stops pairing them with
//! each other (they keep searching) until the window has passed.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::{Duration, Instant};

use chrono::{DateTime, Utc};
use common::codes::{decode_search_code, ConnectCode};
use common::db::MmUser;
use common::net::{sanitize_lan_addr, to_v4};
use common::playkey::PlayKeySecret;
use common::proto::{
    parse_client_message, ClientMessage, CreateTicket, CreateTicketResp, GetTicketResp, Mode, Player, GET_TICKET_RESP,
};
use common::ratelimit::{RateLimiter, Window};
use sha2::{Digest, Sha256};
use uuid::Uuid;

use crate::region::RegionMap;
use crate::ruleset::Rulesets;

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
    /// Tickets per uid: at most 1 per this interval (design 3: 1 per 2 s).
    pub ticket_interval: Duration,
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
    pub rulesets: Rulesets,
    /// Region of a ticket's source address (Unranked buckets).
    pub regions: RegionMap,
    /// An Unranked ticket that has waited this long takes an opponent from any region.
    pub region_widen: Duration,
}

impl Default for EngineConfig {
    fn default() -> Self {
        EngineConfig {
            ticket_ttl: Duration::from_secs(600),
            auth_timeout: Duration::from_secs(4),
            ticket_interval: Duration::from_secs(2),
            requeue_window: Duration::from_secs(60),
            repair_backoff: Duration::from_secs(5),
            max_connect_failures: 3,
            min_app_version: None,
            latest_version: None,
            rulesets: Rulesets::default(),
            regions: RegionMap::default(),
            region_widen: Duration::from_secs(30),
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

#[derive(Debug, Clone)]
enum State {
    Idle,
    Validating(Pending),
    Waiting(Waiting),
    Done,
}

#[derive(Debug)]
struct Conn {
    addr: SocketAddr,
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
    /// Recently matched pairs (sorted uids), to space out re-pairing after a
    /// failed P2P connect.
    history: HashMap<(Uuid, Uuid), PairHistory>,
    ticket_limiter: RateLimiter<Uuid>,
    next_seq: u64,
    match_counter: u64,
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

/// "10 minutes", "1 minute", "20 seconds": the ticket TTL in expiry errors.
fn wait_text(d: Duration) -> String {
    let secs = d.as_secs().max(1);
    let (n, unit) = if secs >= 60 { (secs / 60, "minute") } else { (secs, "second") };
    format!("{n} {unit}{}", if n == 1 { "" } else { "s" })
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
        Engine {
            cfg,
            secret,
            conns: HashMap::new(),
            direct: HashMap::new(),
            unranked: Vec::new(),
            history: HashMap::new(),
            ticket_limiter,
            next_seq: 1,
            match_counter: 0,
        }
    }

    pub fn connection_count(&self) -> usize {
        self.conns.len()
    }

    pub fn waiting_count(&self) -> usize {
        self.conns.values().filter(|c| matches!(c.state, State::Waiting(_))).count()
    }

    pub fn handle(&mut self, now: Instant, wall: DateTime<Utc>, input: Input) -> Vec<Output> {
        let mut out = Vec::new();
        match input {
            Input::Connected { conn, addr } => {
                self.conns.insert(conn, Conn { addr, state: State::Idle });
            }
            Input::Disconnected { conn } => {
                if let Some(c) = self.conns.remove(&conn) {
                    if let State::Waiting(w) = &c.state {
                        tracing::info!(code = %w.code, target = %w.target, "ticket cancelled by client disconnect");
                    }
                    self.unindex(conn);
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
    }

    fn unindex(&mut self, conn: ConnId) {
        self.direct.retain(|_, c| *c != conn);
        self.unranked.retain(|c| *c != conn);
    }

    fn on_packet(&mut self, now: Instant, conn: ConnId, data: &[u8], out: &mut Vec<Output>) {
        let Some(c) = self.conns.get(&conn) else { return };
        if matches!(c.state, State::Done) {
            return;
        }
        if data.len() > MAX_PACKET {
            self.refuse(out, conn, "Invalid matchmaking request");
            return;
        }
        let ticket = match parse_client_message(data) {
            Ok(ClientMessage::CreateTicket(t)) => t,
            Ok(ClientMessage::Unknown(kind)) => {
                tracing::warn!(conn, %kind, "unknown message type");
                self.refuse(out, conn, "Unknown matchmaking request. Your game may need an update.");
                return;
            }
            Err(e) => {
                tracing::warn!(conn, "malformed packet: {e}");
                self.refuse(out, conn, "Invalid matchmaking request");
                return;
            }
        };
        // A second ticket on the same connection replaces the first.
        self.unindex(conn);
        self.on_create_ticket(now, conn, *ticket, out);
    }

    fn on_create_ticket(&mut self, now: Instant, conn: ConnId, t: CreateTicket, out: &mut Vec<Output>) {
        let Some(mode) = Mode::from_u8(t.search.mode) else {
            return self.refuse(out, conn, "Unknown game mode");
        };
        let unsupported = match mode {
            Mode::Direct | Mode::Unranked => None,
            Mode::Ranked => Some("Ranked"),
            Mode::Teams => Some("Teams"),
            Mode::Party => Some("Party"),
        };
        if let Some(name) = unsupported {
            return self.refuse(
                out,
                conn,
                format!("{name} is not supported yet. Only Direct and Unranked work for now."),
            );
        }
        if let Some(min) = &self.cfg.min_app_version {
            if parse_version(&t.app_version) < parse_version(min) {
                let latest = self.cfg.latest_version.clone().unwrap_or_else(|| min.clone());
                return self.refuse(out, conn, format!("Your game is out of date. Update to {latest} to play online."));
            }
        }
        let Ok(uid) = Uuid::parse_str(t.user.uid.trim()) else {
            return self.refuse(out, conn, "Not logged in. Log in again in the launcher.");
        };
        if t.user.play_key.is_empty() || t.user.play_key.len() > 128 {
            return self.refuse(out, conn, "Not logged in. Log in again in the launcher.");
        }
        // Queue modes send an empty code; whatever they send is ignored.
        let target = match mode {
            Mode::Direct => match decode_search_code(&t.search.connect_code) {
                Ok(c) => Some(c),
                Err(_) => return self.refuse(out, conn, "Invalid connect code"),
            },
            _ => None,
        };
        if self.ticket_limiter.check(&uid, now).is_err() {
            return self.refuse(out, conn, "Searching too often. Wait a few seconds and try again.");
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
            FetchResult::NotFound => return self.refuse(out, conn, "Account not found. Log in again in the launcher."),
            FetchResult::Error(e) => {
                tracing::error!("account lookup failed: {e}");
                return self.refuse(out, conn, "Matchmaking is temporarily unavailable. Try again later.");
            }
        };
        if !self.secret.verify(user.uid, user.play_key_version, &p.play_key) {
            return self.refuse(out, conn, "Invalid play key. Log in again in the launcher.");
        }
        if user.is_banned(wall) {
            return self.refuse(out, conn, "This account is banned from online play.");
        }
        let Some(code) = user.connect_code.clone() else {
            return self.refuse(out, conn, "Pick a connect code in the launcher first.");
        };
        let target = p.target.as_ref().map(|t| t.to_string()).unwrap_or_default();
        if p.mode == Mode::Direct && target == code {
            return self.refuse(out, conn, "That is your own connect code. Enter your opponent's code.");
        }
        // One active ticket per account: a newer search replaces an older one.
        let older: Vec<ConnId> = self
            .conns
            .iter()
            .filter(|(id, c)| **id != conn && matches!(&c.state, State::Waiting(w) if w.user.uid == user.uid))
            .map(|(id, _)| *id)
            .collect();
        for old in older {
            self.fail_ticket(out, old, "This search was replaced by a newer one from the same account.", None);
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
            self.pair_unranked(now, wall, out);
        } else {
            tracing::info!(conn, %code, %target, "direct ticket waiting");
            self.direct.insert((code, target), conn);
            self.try_pair(now, wall, conn, out);
        }
    }

    fn on_tick(&mut self, now: Instant, wall: DateTime<Utc>, out: &mut Vec<Output>) {
        let mut expired = Vec::new();
        let mut auth_late = Vec::new();
        let mut waiting = Vec::new();
        for (id, c) in &self.conns {
            match &c.state {
                State::Waiting(w) if now.saturating_duration_since(w.since) >= self.cfg.ticket_ttl => {
                    expired.push((*id, w.mode, w.target.clone()))
                }
                State::Waiting(w) if w.mode == Mode::Direct => waiting.push(*id),
                State::Validating(p) if now.saturating_duration_since(p.since) >= self.cfg.auth_timeout => {
                    auth_late.push(*id)
                }
                _ => {}
            }
        }
        for id in auth_late {
            self.refuse(out, id, "Matchmaking is temporarily unavailable. Try again later.");
        }
        let ttl = wait_text(self.cfg.ticket_ttl);
        for (id, mode, target) in expired {
            let msg = if mode == Mode::Direct {
                format!("Search timed out: {target} did not connect within {ttl}.")
            } else {
                format!("Search timed out: no opponent found within {ttl}.")
            };
            self.fail_ticket(out, id, msg, None);
        }
        // Retry pairs that were held back by the re-pair backoff, and Unranked tickets whose
        // region search has widened.
        waiting.sort_unstable();
        for id in waiting {
            self.try_pair(now, wall, id, out);
        }
        self.pair_unranked(now, wall, out);
        let window = self.cfg.requeue_window;
        self.history.retain(|_, h| now.saturating_duration_since(h.last_match) < window);
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
                    self.fail_ticket(
                        out,
                        conn,
                        format!(
                            "Could not connect to {their_code} after {n} tries. A firewall or strict NAT may block it."
                        ),
                        None,
                    );
                    self.fail_ticket(
                        out,
                        other,
                        format!(
                            "Could not connect to {my_code} after {n} tries. A firewall or strict NAT may block it."
                        ),
                        None,
                    );
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

    /// Pairs Unranked tickets until no pair is possible.
    fn pair_unranked(&mut self, now: Instant, wall: DateTime<Utc>, out: &mut Vec<Output>) {
        while let Some((a, b, failures)) = self.next_unranked_pair(now) {
            let (ua, ub) = match (self.conns.get(&a), self.conns.get(&b)) {
                (Some(Conn { state: State::Waiting(x), .. }), Some(Conn { state: State::Waiting(y), .. })) => {
                    (x.user.uid, y.user.uid)
                }
                _ => break,
            };
            self.history.insert(pair_key(ua, ub), PairHistory { last_match: now, failures });
            let before = self.unranked.len();
            self.make_match(wall, &[a, b], out);
            if self.unranked.len() == before {
                break; // make_match refused; never loop forever
            }
        }
    }

    /// The pair the Unranked queue makes next: the oldest ticket and the first later one (in
    /// arrival order) it can play. Same region unless either has waited `region_widen`. A pair
    /// matched within `requeue_window` failed its P2P connect (Slippi requeues with a new ticket):
    /// anyone else in the queue goes first; the same two again only after the backoff, and not
    /// at all after `max_connect_failures`. Returns (older, newer, failures so far).
    fn next_unranked_pair(&self, now: Instant) -> Option<(ConnId, ConnId, u32)> {
        let queue: Vec<(ConnId, &Waiting)> = self
            .unranked
            .iter()
            .filter_map(|id| match self.conns.get(id) {
                Some(Conn { state: State::Waiting(w), .. }) => Some((*id, w)),
                _ => None,
            })
            .collect();
        let widened = |w: &Waiting| now.saturating_duration_since(w.since) >= self.cfg.region_widen;
        for (i, (a, wa)) in queue.iter().enumerate() {
            let mut retry: Option<(ConnId, u32)> = None;
            for (b, wb) in &queue[i + 1..] {
                if wa.user.uid == wb.user.uid {
                    continue;
                }
                if wa.region != wb.region && !widened(wa) && !widened(wb) {
                    continue;
                }
                let recent = self
                    .history
                    .get(&pair_key(wa.user.uid, wb.user.uid))
                    .filter(|h| now.saturating_duration_since(h.last_match) < self.cfg.requeue_window);
                let Some(h) = recent else { return Some((*a, *b, 0)) };
                let failures = h.failures + 1;
                let not_before = h.last_match + P2P_CONNECT_WINDOW + self.cfg.repair_backoff * failures;
                if failures < self.cfg.max_connect_failures && now >= not_before && retry.is_none() {
                    retry = Some((*b, failures));
                }
            }
            if let Some((b, failures)) = retry {
                return Some((*a, b, failures));
            }
        }
        None
    }

    fn make_match(&mut self, wall: DateTime<Utc>, conns: &[ConnId], out: &mut Vec<Output>) {
        let mut entrants: Vec<(ConnId, SocketAddr, Waiting)> = conns
            .iter()
            .filter_map(|id| match self.conns.get(id) {
                Some(Conn { addr, state: State::Waiting(w) }) => Some((*id, *addr, w.clone())),
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
                ip_address: to_v4(*addr).map(|a| a.to_string()).unwrap_or_default(),
                ip_address_lan: w.lan.clone(),
                chat_messages: common::DEFAULT_CHAT_MESSAGES.iter().map(|s| s.to_string()).collect(),
                rank: None,
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
            let addr: SocketAddr = format!("203.0.113.{}:{port}", conn).parse().unwrap();
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
            assert_eq!(msgs[0]["error"], format!("Search timed out: {target} did not connect within 30 seconds."));
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
        assert_eq!(sent(&out, 1)[0]["error"], "Invalid play key. Log in again in the launcher.");
        assert!(disconnected(&out, 1));

        // Rotated key (password change): the old key stops working.
        let mut rotated = a.clone();
        rotated.play_key_version = 2;
        h.add(&rotated);
        h.connect(2, 2);
        h.now += Duration::from_secs(3);
        let v = h.ticket_json(&a, "BB#1", 2);
        let out = h.raw(2, v.to_string().as_bytes());
        assert_eq!(sent(&out, 2)[0]["error"], "Invalid play key. Log in again in the launcher.");

        let stranger = user("ZZ#9", "z");
        h.connect(3, 3);
        let out = h.ticket(3, &stranger, "AA#1");
        assert_eq!(sent(&out, 3)[0]["error"], "Account not found. Log in again in the launcher.");

        h.connect(4, 4);
        let mut v = h.ticket_json(&a, "BB#1", 2);
        v["user"]["uid"] = json!("not-a-uuid");
        let out = h.raw(4, v.to_string().as_bytes());
        assert_eq!(sent(&out, 4)[0]["error"], "Not logged in. Log in again in the launcher.");
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
        assert_eq!(sent(&h.ticket(1, &banned, "AA#1"), 1)[0]["error"], "This account is banned from online play.");
        h.connect(2, 2);
        assert_eq!(sent(&h.ticket(2, &nocode, "AA#1"), 2)[0]["error"], "Pick a connect code in the launcher first.");
        h.connect(3, 3);
        let err = &sent(&h.ticket(3, &me, "me#1"), 3)[0]["error"];
        assert_eq!(err, "That is your own connect code. Enter your opponent's code.");
    }

    #[test]
    fn other_modes_are_not_supported_yet() {
        let mut h = Harness::new(EngineConfig::default());
        let a = user("AA#1", "a");
        h.add(&a);
        for (i, (mode, name)) in [(0u8, "Ranked"), (3, "Teams"), (4, "Party")].into_iter().enumerate() {
            let conn = 10 + i as u64;
            h.connect(conn, conn as u16);
            let v = h.ticket_json(&a, "", mode);
            let out = h.raw(conn, v.to_string().as_bytes());
            let msgs = sent(&out, conn);
            assert_eq!(msgs[0]["type"], "create-ticket-resp");
            assert_eq!(
                msgs[0]["error"],
                format!("{name} is not supported yet. Only Direct and Unranked work for now.")
            );
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
        assert_eq!(sent(&out, 1)[0]["error"], "This search was replaced by a newer one from the same account.");
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
        assert_eq!(sent(&out, 2)[0]["error"], "Searching too often. Wait a few seconds and try again.");
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
        assert_eq!(sent(&out, 1)[0]["error"], "Matchmaking is temporarily unavailable. Try again later.");
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
        assert_eq!(sent(&out, 2)[0]["error"], "Matchmaking is temporarily unavailable. Try again later.");
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
        assert_eq!(sent(&out, 1)[0]["error"], "Your game is out of date. Update to 3.5.2 to play online.");
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
        assert!(errors
            .contains(&"Could not connect to BB#1 after 3 tries. A firewall or strict NAT may block it.".to_string()));
        assert!(errors
            .contains(&"Could not connect to AA#1 after 3 tries. A firewall or strict NAT may block it.".to_string()));
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
        assert_eq!(msgs[0]["error"], "Search timed out: no opponent found within 30 seconds.");
        assert_eq!(wait_text(Duration::from_secs(600)), "10 minutes");
        assert_eq!(wait_text(Duration::from_secs(60)), "1 minute");
        assert_eq!(wait_text(Duration::from_secs(1)), "1 second");
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
        assert_eq!(sent(&out, 2)[0]["error"], "This search was replaced by a newer one from the same account.");
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
}
