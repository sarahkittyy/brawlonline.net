//! The ENet event loop around [`Engine`].
//!
//! One thread owns the ENet host and the engine (design 2.3: single-threaded
//! loop, all queue state in memory). Account lookups go to a tokio task over a
//! bounded channel and come back as [`Input::UserFetched`], so a slow database
//! never blocks the loop; a full queue refuses the ticket instead of growing.
//!
//! Chat identity keys and chat reports go to the database the same way as match records: a task
//! per write on the runtime, never waited for. A report's signatures are checked there (against
//! the reported account's identity keys), off the loop.
//!
//! The loop also publishes the engine's status (the online count and the public rooms) twice a
//! second for a small HTTP listener on a local address (`MM_STATUS_LISTEN`, `GET /status`), which
//! accounts reads to serve the launcher's `GET /v1/rooms`. The same listener serves the live games
//! to accounts (`GET /live/{stream}`, `docs/spectate-protocol.md` §2): mm keeps the uploaded
//! chunks ([`crate::live::LiveStore`]) and each spectator's stream reads them from there.

use std::collections::HashMap;
use std::future::Future;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc as std_mpsc, Arc, RwLock};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use chrono::{DateTime, Utc};
use common::db;
use common::net::EnetSocket;
use common::playkey::PlayKeySecret;
use common::proto::{MM_CHANNEL, MM_CHANNEL_COUNT};
use common::rooms::MmStatus;
use rusty_enet::{EventNoRef, Host, HostSettings, Packet, PeerID};
use sqlx::PgPool;
use tokio::sync::mpsc::error::TrySendError;
use tokio::sync::{mpsc as tk_mpsc, Semaphore};
use uuid::Uuid;

use crate::chat::{report_row, ChatReportRecord};
use crate::engine::{ConnId, Engine, EngineConfig, FetchResult, Input, MatchRecord, Output};
use crate::live::{Cursor, LiveStore};

const TICK: Duration = Duration::from_millis(100);
const IDLE_SLEEP: Duration = Duration::from_millis(2);
/// How long the server waits before carrying out its own disconnect of a client (after a match or
/// a refusal). Slippi's client disconnects itself as soon as it has the answer and then waits up
/// to 3 s for the server to acknowledge before it binds its P2P port. When the server disconnected
/// at the same moment the two disconnects crossed and the client's was sometimes never answered,
/// which cost that client 3 s of its 8 s P2P connect window. Waiting a moment lets the client go
/// first; a client that does not is still disconnected.
const DISCONNECT_GRACE: Duration = Duration::from_secs(1);
/// Account lookups waiting for the database worker. When the queue is full the ticket is refused
/// ("temporarily unavailable") instead of the queue growing without bound.
const FETCH_QUEUE: usize = 256;
/// Account lookups running at once. The pool has 5 connections; the rest wait for one (up to the
/// lookup timeout), and further lookups wait in [`FETCH_QUEUE`].
const MAX_FETCHES_IN_FLIGHT: usize = 16;
/// How often the status snapshot for the launcher is refreshed.
const STATUS_INTERVAL: Duration = Duration::from_millis(500);

/// The latest status snapshot, shared with the status listener.
pub type SharedStatus = Arc<RwLock<MmStatus>>;

struct FetchJob {
    conn: ConnId,
    seq: u64,
    uid: Uuid,
}

type Fetched = (ConnId, u64, FetchResult);
/// A room's ruleset lookup and its answer (code and ruleset; `None`: no such ruleset, or none
/// remembered).
type RulesetFetched = (crate::engine::RulesetFetch, Result<Option<(String, common::rules::Ruleset)>, String>);
type RulesetListFetched = (ConnId, Result<Vec<common::rooms::RulesetEntry>, String>);

/// A running mm server. Dropping it stops the loop.
pub struct MmHandle {
    pub addr: SocketAddr,
    /// Where the status listener answers `GET /status` (None when it is off).
    pub status_addr: Option<SocketAddr>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
    status_task: Option<tokio::task::JoinHandle<()>>,
}

impl MmHandle {
    /// True while the event loop thread is alive (it never exits on bad input).
    pub fn is_running(&self) -> bool {
        self.thread.as_ref().is_some_and(|t| !t.is_finished())
    }

    pub fn stop(mut self) {
        self.shutdown();
    }

    fn shutdown(&mut self) {
        if let Some(t) = self.status_task.take() {
            t.abort();
        }
        self.stop.store(true, Ordering::SeqCst);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }

    /// Blocks until the loop exits (used by `main`).
    pub fn join(mut self) {
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Drop for MmHandle {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// Binds `listen` and starts the loop on its own thread. The database worker and the status
/// listener (on `status_listen`, if given) run on `rt`.
#[allow(clippy::too_many_arguments)]
pub fn start(
    listen: SocketAddr,
    status_listen: Option<SocketAddr>,
    max_peers: usize,
    engine_cfg: EngineConfig,
    secret: PlayKeySecret,
    pool: PgPool,
    rt: tokio::runtime::Handle,
) -> anyhow::Result<MmHandle> {
    let socket = EnetSocket::bind(listen)?;
    let addr = socket.local_addr()?;
    let host = Host::new(
        socket,
        HostSettings { peer_limit: max_peers.max(1), channel_limit: MM_CHANNEL_COUNT, ..Default::default() },
    )
    .map_err(|e| anyhow::anyhow!("creating ENet host: {e:?}"))?;

    let (job_tx, job_rx) = tk_mpsc::channel::<FetchJob>(FETCH_QUEUE);
    let (done_tx, done_rx) = std_mpsc::channel::<Fetched>();
    let (rulesets_tx, rulesets_rx) = std_mpsc::channel::<RulesetFetched>();
    let (lists_tx, lists_rx) = std_mpsc::channel::<RulesetListFetched>();
    let (accounts_tx, accounts_rx) = std_mpsc::channel::<Vec<db::MmUser>>();
    rt.spawn(watch_accounts(pool.clone(), accounts_tx));
    let fetch_pool = pool.clone();
    rt.spawn(fetch_worker(
        move |uid| fetch_user(fetch_pool.clone(), uid),
        job_rx,
        done_tx.clone(),
        MAX_FETCHES_IN_FLIGHT,
    ));

    let status: SharedStatus = Arc::new(RwLock::new(MmStatus::default()));
    let engine = Engine::new(engine_cfg, secret);
    let live = engine.live_store();
    let (status_addr, status_task) = match status_listen {
        Some(l) => {
            let listener = std::net::TcpListener::bind(l)?;
            listener.set_nonblocking(true)?;
            let status_addr = listener.local_addr()?;
            let _guard = rt.enter();
            let listener = tokio::net::TcpListener::from_std(listener)?;
            let task = rt.spawn(serve_status(listener, status.clone(), live));
            tracing::info!("mm status on http://{status_addr}/status");
            (Some(status_addr), Some(task))
        }
        None => (None, None),
    };

    let stop = Arc::new(AtomicBool::new(false));
    let stop2 = stop.clone();
    let db = Db { fetches: job_tx, fetched: done_tx, rulesets: rulesets_tx, ruleset_lists: lists_tx, pool, rt };
    let thread = std::thread::Builder::new()
        .name("mm-enet".into())
        .spawn(move || event_loop(host, engine, db, done_rx, rulesets_rx, lists_rx, accounts_rx, stop2, status))?;
    tracing::info!("mm listening on udp://{addr}");
    Ok(MmHandle { addr, status_addr, stop, thread: Some(thread), status_task })
}

/// `GET /status`: the latest snapshot as JSON ([`MmStatus`]). `GET /live/{stream}`: a live
/// stream for accounts. Local only: accounts reads them and serves launchers.
async fn serve_status(listener: tokio::net::TcpListener, status: SharedStatus, live: Arc<LiveStore>) {
    use axum::routing::get;
    let app = axum::Router::new()
        .route(
            "/status",
            get(move || {
                let status = status.clone();
                async move {
                    let snapshot = status.read().map(|s| s.clone()).unwrap_or_default();
                    axum::Json(snapshot)
                }
            }),
        )
        .route("/live/{stream}", get(serve_live))
        .with_state(live);
    if let Err(e) = axum::serve(listener, app).await {
        tracing::error!("status listener stopped: {e}");
    }
}

#[derive(Debug, serde::Deserialize)]
struct LiveQuery {
    /// The game's place in the stream (1-based) to start from.
    game: Option<u32>,
    /// Its first chunk to send; above 0 the game's `game` line is not sent again.
    chunk: Option<u32>,
}

/// How often an idle live stream gets an empty line (accounts passes a ping on).
const LIVE_KEEPALIVE: Duration = Duration::from_secs(15);

/// `GET /live/{stream}?game=&chunk=`: newline-delimited JSON ([`common::spectate::LiveLine`]) from
/// there on, as chunks come, until the stream is over (or gone). 404 for an unknown stream.
async fn serve_live(
    axum::extract::State(store): axum::extract::State<Arc<LiveStore>>,
    axum::extract::Path(id): axum::extract::Path<String>,
    axum::extract::Query(q): axum::extract::Query<LiveQuery>,
) -> axum::response::Response {
    use axum::response::IntoResponse;
    let Some(mut changed) = store.subscribe(&id) else {
        return (axum::http::StatusCode::NOT_FOUND, "no such stream").into_response();
    };
    let chunk = q.chunk.unwrap_or(0) as usize;
    let mut cursor = Cursor { game: q.game.unwrap_or(1).saturating_sub(1) as usize, chunk, announced: chunk > 0 };
    let (tx, rx) = tk_mpsc::channel::<String>(64);
    tokio::spawn(async move {
        loop {
            let Some((lines, done)) = store.read(&id, &mut cursor) else { return };
            for l in lines {
                let mut s = serde_json::to_string(&l).expect("live lines serialize");
                s.push('\n');
                if tx.send(s).await.is_err() {
                    return;
                }
            }
            if done {
                return;
            }
            tokio::select! {
                r = changed.changed() => if r.is_err() { return },
                () = tokio::time::sleep(LIVE_KEEPALIVE) => {
                    if tx.send("\n".into()).await.is_err() {
                        return;
                    }
                }
            }
        }
    });
    let body = axum::body::Body::from_stream(LineStream(rx));
    ([(axum::http::header::CONTENT_TYPE, "application/x-ndjson")], body).into_response()
}

struct LineStream(tk_mpsc::Receiver<String>);

impl futures_core::Stream for LineStream {
    type Item = Result<String, std::convert::Infallible>;
    fn poll_next(
        mut self: std::pin::Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
    ) -> std::task::Poll<Option<Self::Item>> {
        self.0.poll_recv(cx).map(|l| l.map(Ok))
    }
}

/// Runs account lookups from `jobs`, at most `max_in_flight` at once. While all slots are busy it
/// stops taking jobs, so the bounded channel fills and [`queue_fetch`] refuses further tickets.
async fn fetch_worker<F, Fut>(
    fetch: F,
    mut jobs: tk_mpsc::Receiver<FetchJob>,
    done: std_mpsc::Sender<Fetched>,
    max_in_flight: usize,
) where
    F: Fn(Uuid) -> Fut,
    Fut: Future<Output = FetchResult> + Send + 'static,
{
    let slots = Arc::new(Semaphore::new(max_in_flight.max(1)));
    while let Some(job) = jobs.recv().await {
        let Ok(slot) = slots.clone().acquire_owned().await else { break };
        let lookup = fetch(job.uid);
        let done = done.clone();
        tokio::spawn(async move {
            let result = lookup.await;
            drop(slot);
            let _ = done.send((job.conn, job.seq, result));
        });
    }
}

async fn fetch_user(pool: PgPool, uid: Uuid) -> FetchResult {
    match tokio::time::timeout(Duration::from_secs(3), db::fetch_mm_user(&pool, uid)).await {
        Ok(Ok(Some(u))) => FetchResult::Found(u),
        Ok(Ok(None)) => FetchResult::NotFound,
        Ok(Err(e)) => FetchResult::Error(e.to_string()),
        Err(_) => FetchResult::Error("account lookup timed out".into()),
    }
}

/// Hands an account lookup to the worker, or answers it at once with an error when the queue is
/// full (the engine then refuses the ticket as "temporarily unavailable").
fn queue_fetch(jobs: &tk_mpsc::Sender<FetchJob>, done: &std_mpsc::Sender<Fetched>, job: FetchJob) {
    let (job, why) = match jobs.try_send(job) {
        Ok(()) => return,
        Err(TrySendError::Full(job)) => (job, "account lookup queue full"),
        Err(TrySendError::Closed(job)) => (job, "account lookup worker stopped"),
    };
    tracing::warn!(conn = job.conn, "{why}");
    let _ = done.send((job.conn, job.seq, FetchResult::Error(why.into())));
}

/// A room's ruleset: the code's (made the account's remembered one when `remember`), or with no
/// code the account's remembered one (`remember`: forget it, P+'s default). Hidden rulesets play
/// by their code (room-rules.md §2).
async fn fetch_room_ruleset(
    pool: &PgPool,
    f: &crate::engine::RulesetFetch,
) -> Result<Option<(String, common::rules::Ruleset)>, String> {
    let work = async {
        match &f.code {
            Some(code) => {
                let found = db::fetch_ruleset(pool, code).await?;
                if found.is_some() && f.remember {
                    db::set_remembered_ruleset(pool, f.uid, Some(code)).await?;
                }
                Ok::<_, sqlx::Error>(found.map(|p| (p.code, p.ruleset)))
            }
            None if f.remember => {
                db::set_remembered_ruleset(pool, f.uid, None).await?;
                Ok(None)
            }
            None => Ok(db::remembered_ruleset(pool, f.uid).await?.map(|p| (p.code, p.ruleset))),
        }
    };
    match tokio::time::timeout(Duration::from_secs(3), work).await {
        Ok(Ok(r)) => Ok(r),
        Ok(Err(e)) => Err(e.to_string()),
        Err(_) => Err("ruleset lookup timed out".into()),
    }
}

/// A player's rulesets for the game's Ruleset line: the recent ones (newest first), then their own
/// published ones that are not hidden, each once, at most `RULESET_LIST_MAX`.
async fn fetch_ruleset_list(pool: &PgPool, uid: Uuid) -> Result<Vec<common::rooms::RulesetEntry>, String> {
    let work = async {
        let mut out: Vec<common::rooms::RulesetEntry> = Vec::new();
        let recent = db::recent_rulesets(pool, uid).await?;
        let own = db::fetch_rulesets_where(pool, "WHERE r.owner = $1 AND NOT r.hidden ORDER BY r.created_at DESC", uid)
            .await?;
        for p in recent.into_iter().map(|(p, _)| p).chain(own) {
            if out.len() >= common::rooms::RULESET_LIST_MAX || out.iter().any(|e| e.code == p.code) {
                continue;
            }
            out.push(common::rooms::RulesetEntry {
                code: p.code,
                name: p.ruleset.name.clone(),
                version: p.ruleset.version,
                by: p.ruleset.by.clone(),
                best_of: p.ruleset.best_of,
            });
        }
        Ok::<_, sqlx::Error>(out)
    };
    match tokio::time::timeout(Duration::from_secs(3), work).await {
        Ok(Ok(r)) => Ok(r),
        Ok(Err(e)) => Err(e.to_string()),
        Err(_) => Err("ruleset list timed out".into()),
    }
}

/// Reads the accounts that changed every [`db::ACCOUNT_POLL`] and hands them to the loop
/// ([`Input::AccountsChanged`]): `pp-admin` bans, renames and mutes write only the database. Ends
/// when the loop has gone.
async fn watch_accounts(pool: PgPool, changed: std_mpsc::Sender<Vec<db::MmUser>>) {
    let mut since = Utc::now() - db::ACCOUNT_POLL_OVERLAP;
    let mut every = tokio::time::interval(db::ACCOUNT_POLL);
    every.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    loop {
        every.tick().await;
        match tokio::time::timeout(Duration::from_secs(10), db::changed_users(&pool, since)).await {
            Ok(Ok((users, now))) => {
                since = now - db::ACCOUNT_POLL_OVERLAP;
                if !users.is_empty() && changed.send(users).is_err() {
                    return;
                }
            }
            Ok(Err(e)) => tracing::warn!("reading changed accounts failed: {e}"),
            Err(_) => tracing::warn!("reading changed accounts timed out"),
        }
    }
}

/// Match records are not queued with the lookups: they need two verified, rate-limited accounts,
/// and must not be lost when the lookup queue is full.
async fn record_match(pool: PgPool, m: MatchRecord) {
    let stages: Vec<i16> = m.stages.iter().map(|s| *s as i16).collect();
    if let Err(e) = db::insert_match(
        &pool,
        &m.match_id,
        m.mode.as_u8() as i16,
        &m.players,
        m.host,
        &stages,
        &m.region,
        m.room.as_ref(),
    )
    .await
    {
        tracing::error!(match_id = %m.match_id, "recording match failed: {e}");
    }
}

/// Records a chat identity key (best effort, like a match).
async fn record_chat_key(pool: PgPool, uid: Uuid, key: [u8; 32]) {
    if let Err(e) = db::record_chat_key(&pool, uid, &key).await {
        tracing::error!(%uid, "recording chat key failed: {e}");
    }
}

/// What mm saw of a Ranked set (best effort, like a match): accounts checks a game only one
/// player reported against it.
async fn record_ranked_presence(pool: PgPool, match_id: String, uid: Uuid, online: bool, at: DateTime<Utc>) {
    if let Err(e) = db::record_ranked_presence(&pool, &match_id, uid, online, at).await {
        tracing::error!(match_id, %uid, "recording ranked presence failed: {e}");
    }
}

async fn record_ranked_game_result(
    pool: PgPool,
    match_id: String,
    game_index: u32,
    uid: Uuid,
    winner: Option<Uuid>,
    end_method: String,
) {
    if let Err(e) = db::record_ranked_game_result(&pool, &match_id, game_index, uid, winner, &end_method).await {
        tracing::error!(match_id, game_index, %uid, "recording a ranked game result failed: {e}");
    }
}

async fn count_ruleset_game(pool: PgPool, code: String) {
    if let Err(e) = db::count_ruleset_game(&pool, &code).await {
        tracing::error!(%code, "counting a ruleset game failed: {e}");
    }
}

/// Stores a chat report: each message's signature is checked against the reported account's
/// identity keys (on a blocking thread: up to 20 messages × 20 keys), the texts are cleaned.
async fn store_chat_report(pool: PgPool, r: ChatReportRecord) {
    let keys = match db::fetch_chat_keys(&pool, r.reported).await {
        Ok(k) => k,
        Err(e) => {
            // Stored anyway, with nothing verified: the report is not lost.
            tracing::error!(reported = %r.reported, "reading chat keys failed: {e}");
            Vec::new()
        }
    };
    let row = match tokio::task::spawn_blocking(move || report_row(&r, &keys)).await {
        Ok(row) => row,
        Err(e) => return tracing::error!("checking a chat report failed: {e}"),
    };
    tracing::info!(reported = %row.reported, verified = row.verified, total = row.total, "chat report stored");
    if let Err(e) = db::insert_chat_report(&pool, &row).await {
        tracing::error!(reported = %row.reported, "storing chat report failed: {e}");
    }
}

/// The event loop's way to the database.
struct Db {
    fetches: tk_mpsc::Sender<FetchJob>,
    /// Where lookup results come back; also used to refuse a lookup when the queue is full.
    fetched: std_mpsc::Sender<Fetched>,
    /// Where a room's ruleset lookups come back.
    rulesets: std_mpsc::Sender<RulesetFetched>,
    /// Where ruleset lists come back.
    ruleset_lists: std_mpsc::Sender<RulesetListFetched>,
    pool: PgPool,
    rt: tokio::runtime::Handle,
}

struct Loop {
    host: Host<EnetSocket>,
    engine: Engine,
    db: Db,
    peer_conn: HashMap<PeerID, ConnId>,
    conn_peer: HashMap<ConnId, PeerID>,
    next_conn: ConnId,
    /// Server-initiated disconnects waiting for DISCONNECT_GRACE.
    pending_disconnects: Vec<(ConnId, Instant)>,
}

impl Loop {
    fn feed(&mut self, input: Input) {
        let outputs = self.engine.handle(Instant::now(), Utc::now(), input);
        for o in outputs {
            self.apply(o);
        }
    }

    fn apply(&mut self, o: Output) {
        match o {
            Output::Send { conn, json } => {
                if let Some(pid) = self.conn_peer.get(&conn) {
                    let packet = Packet::reliable(json.as_bytes());
                    if let Err(e) = self.host.peer_mut(*pid).send(MM_CHANNEL, &packet) {
                        tracing::warn!(conn, "send failed: {e:?}");
                    }
                }
            }
            Output::SendData { conn, channel, data } => {
                if let Some(pid) = self.conn_peer.get(&conn) {
                    if let Err(e) = self.host.peer_mut(*pid).send(channel, &Packet::reliable(&data[..])) {
                        tracing::warn!(conn, "send failed: {e:?}");
                    }
                }
            }
            Output::Disconnect { conn } => {
                if self.conn_peer.contains_key(&conn) && !self.pending_disconnects.iter().any(|(c, _)| *c == conn) {
                    self.pending_disconnects.push((conn, Instant::now() + DISCONNECT_GRACE));
                }
            }
            Output::Close { conn } => {
                if let Some(pid) = self.conn_peer.get(&conn) {
                    self.host.peer_mut(*pid).disconnect_later(0);
                }
            }
            Output::FetchUser { conn, seq, uid } => {
                queue_fetch(&self.db.fetches, &self.db.fetched, FetchJob { conn, seq, uid });
            }
            Output::RecordMatch(m) => {
                self.db.rt.spawn(record_match(self.db.pool.clone(), m));
            }
            Output::RecordChatKey { uid, key } => {
                self.db.rt.spawn(record_chat_key(self.db.pool.clone(), uid, key));
            }
            Output::ChatReport(r) => {
                self.db.rt.spawn(store_chat_report(self.db.pool.clone(), r));
            }
            Output::RulesetPlayed { code } => {
                self.db.rt.spawn(count_ruleset_game(self.db.pool.clone(), code));
            }
            Output::RankedPresence { match_id, uid, online, at } => {
                self.db.rt.spawn(record_ranked_presence(self.db.pool.clone(), match_id, uid, online, at));
            }
            Output::RankedGameResult { match_id, game_index, uid, winner, end_method } => {
                let pool = self.db.pool.clone();
                self.db.rt.spawn(record_ranked_game_result(pool, match_id, game_index, uid, winner, end_method));
            }
            Output::FetchRulesetList { conn, uid } => {
                let (pool, done) = (self.db.pool.clone(), self.db.ruleset_lists.clone());
                self.db.rt.spawn(async move {
                    let result = fetch_ruleset_list(&pool, uid).await;
                    let _ = done.send((conn, result));
                });
            }
            Output::FetchRuleset(f) => {
                let (pool, done) = (self.db.pool.clone(), self.db.rulesets.clone());
                self.db.rt.spawn(async move {
                    let result = fetch_room_ruleset(&pool, &f).await;
                    let _ = done.send((f, result));
                });
            }
        }
    }

    /// Carries out the server-initiated disconnects whose grace period is over, unless the client
    /// has disconnected meanwhile.
    fn run_pending_disconnects(&mut self, now: Instant) -> bool {
        if self.pending_disconnects.is_empty() {
            return false;
        }
        let mut busy = false;
        let mut i = 0;
        while i < self.pending_disconnects.len() {
            let (conn, due) = self.pending_disconnects[i];
            if !self.conn_peer.contains_key(&conn) {
                self.pending_disconnects.swap_remove(i);
            } else if now >= due {
                self.pending_disconnects.swap_remove(i);
                if let Some(pid) = self.conn_peer.get(&conn) {
                    self.host.peer_mut(*pid).disconnect_later(0);
                    busy = true;
                }
            } else {
                i += 1;
            }
        }
        busy
    }

    fn on_event(&mut self, ev: EventNoRef) {
        match ev {
            EventNoRef::Connect { peer, .. } => {
                let Some(addr) = self.host.peer(peer).address() else { return };
                let conn = self.next_conn;
                self.next_conn += 1;
                // Design 2.3: ENet peer timeout about 10 s.
                self.host.peer_mut(peer).set_timeout(32, 5000, 10_000);
                self.peer_conn.insert(peer, conn);
                self.conn_peer.insert(conn, peer);
                tracing::debug!(conn, %addr, "peer connected");
                self.feed(Input::Connected { conn, addr });
            }
            EventNoRef::Disconnect { peer, .. } => {
                if let Some(conn) = self.peer_conn.remove(&peer) {
                    self.conn_peer.remove(&conn);
                    tracing::debug!(conn, "peer disconnected");
                    self.feed(Input::Disconnected { conn });
                }
            }
            EventNoRef::Receive { peer, channel_id, packet } => {
                if let Some(conn) = self.peer_conn.get(&peer).copied() {
                    let data = packet.data().to_vec();
                    if channel_id == common::spectate::DATA_CHANNEL {
                        self.feed(Input::Data { conn, data });
                    } else {
                        self.feed(Input::Packet { conn, data });
                    }
                }
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn event_loop(
    host: Host<EnetSocket>,
    engine: Engine,
    db: Db,
    done: std_mpsc::Receiver<Fetched>,
    rulesets: std_mpsc::Receiver<RulesetFetched>,
    ruleset_lists: std_mpsc::Receiver<RulesetListFetched>,
    accounts: std_mpsc::Receiver<Vec<db::MmUser>>,
    stop: Arc<AtomicBool>,
    status: SharedStatus,
) {
    let mut lp = Loop {
        host,
        engine,
        db,
        peer_conn: HashMap::new(),
        conn_peer: HashMap::new(),
        next_conn: 1,
        pending_disconnects: Vec::new(),
    };
    let mut last_tick = Instant::now();
    let mut last_stats = Instant::now();
    let mut last_status: Option<Instant> = None;
    while !stop.load(Ordering::Relaxed) {
        let mut busy = false;
        // Bounded so one flood cannot starve timers.
        for _ in 0..1000 {
            match lp.host.service() {
                Ok(Some(ev)) => {
                    busy = true;
                    let ev = ev.no_ref();
                    lp.on_event(ev);
                }
                Ok(None) => break,
                Err(e) => {
                    tracing::warn!("socket error: {e}");
                    break;
                }
            }
        }
        while let Ok((conn, seq, result)) = done.try_recv() {
            busy = true;
            lp.feed(Input::UserFetched { conn, seq, result });
        }
        while let Ok((f, result)) = rulesets.try_recv() {
            busy = true;
            lp.feed(Input::RulesetFetched(f, result));
        }
        while let Ok((conn, result)) = ruleset_lists.try_recv() {
            busy = true;
            lp.feed(Input::RulesetListFetched { conn, result });
        }
        while let Ok(users) = accounts.try_recv() {
            busy = true;
            lp.feed(Input::AccountsChanged(users));
        }
        if lp.run_pending_disconnects(Instant::now()) {
            busy = true;
        }
        if last_tick.elapsed() >= TICK {
            last_tick = Instant::now();
            lp.feed(Input::Tick);
        }
        if last_status.is_none_or(|t| t.elapsed() >= STATUS_INTERVAL) {
            last_status = Some(Instant::now());
            let snapshot = lp.engine.status(Utc::now());
            if let Ok(mut s) = status.write() {
                *s = snapshot;
            }
        }
        if last_stats.elapsed() >= Duration::from_secs(60) {
            last_stats = Instant::now();
            tracing::info!(
                connections = lp.engine.connection_count(),
                waiting = lp.engine.waiting_count(),
                online = lp.engine.online_count(),
                rooms = lp.engine.room_count(),
                live_streams = lp.engine.live_stream_count(),
                "mm stats"
            );
        }
        lp.host.flush();
        if !busy {
            std::thread::sleep(IDLE_SLEEP);
        }
    }
    // Tell connected clients we are going away.
    let ids: Vec<PeerID> = lp.conn_peer.values().copied().collect();
    for pid in ids {
        lp.host.peer_mut(pid).disconnect_now(0);
    }
    lp.host.flush();
    tracing::info!("mm stopped");
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    fn job(conn: ConnId) -> FetchJob {
        FetchJob { conn, seq: conn * 10, uid: Uuid::new_v4() }
    }

    #[test]
    fn full_or_closed_fetch_queue_answers_with_an_error() {
        let (tx, mut rx) = tk_mpsc::channel(1);
        let (done_tx, done_rx) = std_mpsc::channel();
        queue_fetch(&tx, &done_tx, job(1));
        queue_fetch(&tx, &done_tx, job(2));
        assert!(matches!(done_rx.try_recv(), Ok((2, 20, FetchResult::Error(_)))));
        assert!(done_rx.try_recv().is_err(), "the first job was queued, not answered");
        assert_eq!(rx.try_recv().unwrap().conn, 1);
        drop(rx);
        queue_fetch(&tx, &done_tx, job(3));
        assert!(matches!(done_rx.try_recv(), Ok((3, 30, FetchResult::Error(_)))));
    }

    /// A flood of lookups against a stuck database: at most `max_in_flight` run, the queue holds
    /// its capacity, everything else is refused at once, and all queued lookups finish once the
    /// database answers again.
    #[tokio::test]
    async fn fetch_worker_bounds_lookups_in_flight() {
        let running = Arc::new(AtomicUsize::new(0));
        let peak = Arc::new(AtomicUsize::new(0));
        let gate = Arc::new(Semaphore::new(0));
        let (r, p, g) = (running.clone(), peak.clone(), gate.clone());
        let fetch = move |_uid| {
            let (r, p, g) = (r.clone(), p.clone(), g.clone());
            async move {
                p.fetch_max(r.fetch_add(1, Ordering::SeqCst) + 1, Ordering::SeqCst);
                let _open = g.acquire().await;
                r.fetch_sub(1, Ordering::SeqCst);
                FetchResult::NotFound
            }
        };
        let (tx, rx) = tk_mpsc::channel(4);
        let (done_tx, done_rx) = std_mpsc::channel();
        let worker = tokio::spawn(fetch_worker(fetch, rx, done_tx.clone(), 2));

        queue_fetch(&tx, &done_tx, job(1));
        queue_fetch(&tx, &done_tx, job(2));
        let deadline = Instant::now() + Duration::from_secs(5);
        while running.load(Ordering::SeqCst) < 2 {
            assert!(Instant::now() < deadline, "lookups did not start");
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
        for conn in 3..=20 {
            queue_fetch(&tx, &done_tx, job(conn));
        }
        let mut refused = vec![];
        while let Ok((conn, _, result)) = done_rx.try_recv() {
            assert!(matches!(result, FetchResult::Error(_)));
            refused.push(conn);
        }
        // The worker holds at most one job waiting for a slot; the channel holds 4.
        assert!((13..=14).contains(&refused.len()), "{refused:?}");

        gate.add_permits(1000);
        let mut found = 0;
        let deadline = Instant::now() + Duration::from_secs(5);
        while found < 20 - refused.len() {
            assert!(Instant::now() < deadline, "queued lookups did not finish ({found} done)");
            match done_rx.try_recv() {
                Ok((_, _, FetchResult::NotFound)) => found += 1,
                Ok(other) => panic!("unexpected {other:?}"),
                Err(_) => tokio::time::sleep(Duration::from_millis(5)).await,
            }
        }
        assert_eq!(peak.load(Ordering::SeqCst), 2);
        drop(tx);
        tokio::time::timeout(Duration::from_secs(5), worker).await.expect("worker stops").unwrap();
    }
}
