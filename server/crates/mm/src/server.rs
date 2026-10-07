//! The ENet event loop around [`Engine`].
//!
//! One thread owns the ENet host and the engine (design 2.3: single-threaded
//! loop, all queue state in memory). Database work goes to a tokio task over a
//! channel and comes back as [`Input::UserFetched`], so a slow database never
//! blocks the loop.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc as std_mpsc, Arc};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use chrono::Utc;
use common::db;
use common::net::EnetSocket;
use common::playkey::PlayKeySecret;
use common::proto::{MM_CHANNEL, MM_CHANNEL_COUNT};
use rusty_enet::{EventNoRef, Host, HostSettings, Packet, PeerID};
use sqlx::PgPool;
use tokio::sync::mpsc as tk_mpsc;
use uuid::Uuid;

use crate::engine::{ConnId, Engine, EngineConfig, FetchResult, Input, MatchRecord, Output};

const TICK: Duration = Duration::from_millis(100);
const IDLE_SLEEP: Duration = Duration::from_millis(2);
/// How long the server waits before carrying out its own disconnect of a client (after a match or
/// a refusal). Slippi's client disconnects itself as soon as it has the answer and then waits up
/// to 3 s for the server to acknowledge before it binds its P2P port. When the server disconnected
/// at the same moment the two disconnects crossed and the client's was sometimes never answered,
/// which cost that client 3 s of its 8 s P2P connect window. Waiting a moment lets the client go
/// first; a client that does not is still disconnected.
const DISCONNECT_GRACE: Duration = Duration::from_secs(1);

enum DbJob {
    Fetch { conn: ConnId, seq: u64, uid: Uuid },
    Record(MatchRecord),
}

type Fetched = (ConnId, u64, FetchResult);

/// A running mm server. Dropping it stops the loop.
pub struct MmHandle {
    pub addr: SocketAddr,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
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

/// Binds `listen` and starts the loop on its own thread. The database worker
/// runs on `rt`.
pub fn start(
    listen: SocketAddr,
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

    let (job_tx, job_rx) = tk_mpsc::unbounded_channel::<DbJob>();
    let (done_tx, done_rx) = std_mpsc::channel::<Fetched>();
    rt.spawn(db_worker(pool, job_rx, done_tx));

    let stop = Arc::new(AtomicBool::new(false));
    let stop2 = stop.clone();
    let engine = Engine::new(engine_cfg, secret);
    let thread = std::thread::Builder::new()
        .name("mm-enet".into())
        .spawn(move || event_loop(host, engine, job_tx, done_rx, stop2))?;
    tracing::info!("mm listening on udp://{addr}");
    Ok(MmHandle { addr, stop, thread: Some(thread) })
}

async fn db_worker(pool: PgPool, mut jobs: tk_mpsc::UnboundedReceiver<DbJob>, done: std_mpsc::Sender<Fetched>) {
    while let Some(job) = jobs.recv().await {
        let pool = pool.clone();
        let done = done.clone();
        tokio::spawn(async move {
            match job {
                DbJob::Fetch { conn, seq, uid } => {
                    let result = match tokio::time::timeout(Duration::from_secs(3), db::fetch_mm_user(&pool, uid)).await
                    {
                        Ok(Ok(Some(u))) => FetchResult::Found(u),
                        Ok(Ok(None)) => FetchResult::NotFound,
                        Ok(Err(e)) => FetchResult::Error(e.to_string()),
                        Err(_) => FetchResult::Error("account lookup timed out".into()),
                    };
                    let _ = done.send((conn, seq, result));
                }
                DbJob::Record(m) => {
                    let stages: Vec<i16> = m.stages.iter().map(|s| *s as i16).collect();
                    if let Err(e) =
                        db::insert_match(&pool, &m.match_id, m.mode.as_u8() as i16, &m.players, m.host, &stages).await
                    {
                        tracing::error!(match_id = %m.match_id, "recording match failed: {e}");
                    }
                }
            }
        });
    }
}

struct Loop {
    host: Host<EnetSocket>,
    engine: Engine,
    jobs: tk_mpsc::UnboundedSender<DbJob>,
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
            Output::Disconnect { conn } => {
                if self.conn_peer.contains_key(&conn) && !self.pending_disconnects.iter().any(|(c, _)| *c == conn) {
                    self.pending_disconnects.push((conn, Instant::now() + DISCONNECT_GRACE));
                }
            }
            Output::FetchUser { conn, seq, uid } => {
                let _ = self.jobs.send(DbJob::Fetch { conn, seq, uid });
            }
            Output::RecordMatch(m) => {
                let _ = self.jobs.send(DbJob::Record(m));
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
            EventNoRef::Receive { peer, packet, .. } => {
                if let Some(conn) = self.peer_conn.get(&peer).copied() {
                    self.feed(Input::Packet { conn, data: packet.data().to_vec() });
                }
            }
        }
    }
}

fn event_loop(
    host: Host<EnetSocket>,
    engine: Engine,
    jobs: tk_mpsc::UnboundedSender<DbJob>,
    done: std_mpsc::Receiver<Fetched>,
    stop: Arc<AtomicBool>,
) {
    let mut lp = Loop {
        host,
        engine,
        jobs,
        peer_conn: HashMap::new(),
        conn_peer: HashMap::new(),
        next_conn: 1,
        pending_disconnects: Vec::new(),
    };
    let mut last_tick = Instant::now();
    let mut last_stats = Instant::now();
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
        if lp.run_pending_disconnects(Instant::now()) {
            busy = true;
        }
        if last_tick.elapsed() >= TICK {
            last_tick = Instant::now();
            lp.feed(Input::Tick);
        }
        if last_stats.elapsed() >= Duration::from_secs(60) {
            last_stats = Instant::now();
            tracing::info!(connections = lp.engine.connection_count(), waiting = lp.engine.waiting_count(), "mm stats");
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
