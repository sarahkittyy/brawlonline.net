//! A room client: the online connection a running game keeps to mm (`hello`, then `room-*`
//! requests and the room's pushed state), and the room game ticket. See `docs/rooms-protocol.md`.
//!
//! [`OnlineClient`] owns one ENet host with one peer (mm). Nothing runs in the background, so a
//! caller that waits for something else must keep calling [`OnlineClient::pump`] (ENet's
//! keepalive; mm drops a peer it has not heard from for 10 s).

use std::net::{Ipv4Addr, SocketAddr};
use std::time::{Duration, Instant};

use common::net::EnetSocket;
use common::proto::{MM_CHANNEL, MM_CHANNEL_COUNT};
use common::rooms::{Hello, HelloUser, RoomRequest, HELLO};
use rusty_enet::{Event, Host, HostSettings, Packet, PeerID};
use serde_json::Value;

use crate::{CodeEncoding, Credentials, SearchOptions, SearchResult};

/// What [`OnlineClient::recv`] got.
#[derive(Debug, Clone, PartialEq)]
pub enum Received {
    Message(Value),
    Disconnected,
    Timeout,
}

pub struct OnlineClient {
    host: Host<EnetSocket>,
    mm: PeerID,
    pub local_port: u16,
    connected: bool,
}

impl OnlineClient {
    /// Connects and sends `hello`. Returns the client and the `hello-resp` (which may carry an
    /// `error`, after which mm disconnects).
    pub fn connect(server: SocketAddr, creds: &Credentials, app_version: &str) -> anyhow::Result<(Self, Value)> {
        let sock = EnetSocket::bind(SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0)))?;
        let local_port = sock.local_addr()?.port();
        let mut host =
            Host::new(sock, HostSettings { peer_limit: 1, channel_limit: MM_CHANNEL_COUNT, ..Default::default() })
                .map_err(|e| anyhow::anyhow!("ENet host: {e:?}"))?;
        let mm = host.connect(server, MM_CHANNEL_COUNT, 0).map_err(|e| anyhow::anyhow!("connect: {e:?}"))?.id();
        let deadline = Instant::now() + Duration::from_secs(10);
        let mut connected = false;
        while Instant::now() < deadline && !connected {
            match host.service() {
                Ok(Some(Event::Connect { peer, .. })) if peer.id() == mm => connected = true,
                Ok(Some(Event::Disconnect { peer, .. })) if peer.id() == mm => break,
                Ok(Some(_)) => {}
                _ => std::thread::sleep(Duration::from_millis(2)),
            }
        }
        anyhow::ensure!(connected, "could not connect to mm at {server}");
        let mut client = OnlineClient { host, mm, local_port, connected: true };
        let hello = Hello {
            kind: HELLO.into(),
            user: HelloUser { uid: creds.uid.clone(), play_key: creds.play_key.clone() },
            app_version: app_version.into(),
        };
        client.send_json(&serde_json::to_value(&hello)?);
        let resp = client.wait_for(Duration::from_secs(5), |m| m["type"] == "hello-resp")?;
        Ok((client, resp))
    }

    pub fn is_connected(&self) -> bool {
        self.connected
    }

    pub fn send_json(&mut self, v: &Value) {
        let data = v.to_string();
        let _ = self.host.peer_mut(self.mm).send(MM_CHANNEL, &Packet::reliable(data.as_bytes()));
        self.host.flush();
    }

    pub fn send(&mut self, req: &RoomRequest) {
        self.send_json(&req.to_json());
    }

    /// The next message from mm, or `Timeout` after `timeout`.
    pub fn recv(&mut self, timeout: Duration) -> Received {
        let deadline = Instant::now() + timeout;
        loop {
            match self.host.service() {
                Ok(Some(Event::Receive { peer, packet, .. })) if peer.id() == self.mm => {
                    return Received::Message(serde_json::from_slice(packet.data()).unwrap_or(Value::Null));
                }
                Ok(Some(Event::Disconnect { peer, .. })) if peer.id() == self.mm => {
                    self.connected = false;
                    return Received::Disconnected;
                }
                Ok(Some(_)) => continue,
                Ok(None) | Err(_) => {}
            }
            if Instant::now() >= deadline {
                return Received::Timeout;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
    }

    /// Waits for the first message `pred` accepts (others are dropped). Errors on disconnect or
    /// timeout.
    pub fn wait_for(&mut self, timeout: Duration, pred: impl Fn(&Value) -> bool) -> anyhow::Result<Value> {
        let deadline = Instant::now() + timeout;
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            match self.recv(left) {
                Received::Message(m) if pred(&m) => return Ok(m),
                Received::Message(_) => {}
                Received::Disconnected => anyhow::bail!("mm disconnected"),
                Received::Timeout => anyhow::bail!("timed out waiting for mm"),
            }
        }
    }

    /// Every message that arrives within `wait`.
    pub fn drain(&mut self, wait: Duration) -> Vec<Value> {
        let deadline = Instant::now() + wait;
        let mut got = vec![];
        loop {
            match self.recv(deadline.saturating_duration_since(Instant::now())) {
                Received::Message(m) => got.push(m),
                Received::Disconnected | Received::Timeout => return got,
            }
        }
    }

    /// Services the connection (keepalive) without waiting.
    pub fn pump(&mut self) -> Vec<Value> {
        self.drain(Duration::ZERO)
    }

    /// Disconnects politely (as a game that closes).
    pub fn close(mut self) {
        if self.connected {
            self.host.peer_mut(self.mm).disconnect(0);
            let deadline = Instant::now() + Duration::from_secs(1);
            while Instant::now() < deadline {
                if let Ok(Some(Event::Disconnect { .. })) = self.host.service() {
                    break;
                }
                std::thread::sleep(Duration::from_millis(2));
            }
        }
    }
}

/// The search options of a room's game ticket: mode 3, the room code as the target (full width,
/// as Brawl's keypad sends it).
pub fn room_ticket_options(server: SocketAddr, creds: Credentials, code: &str) -> SearchOptions {
    SearchOptions {
        mode: 3,
        target: code.into(),
        encoding: CodeEncoding::Fullwidth,
        match_timeout: Some(Duration::from_secs(30)),
        ..SearchOptions::direct(server, creds, code)
    }
}

/// Sends a room's game ticket and waits for the `get-ticket-resp` (from a fresh local port, as the
/// game does from its P2P port).
pub fn room_ticket(server: SocketAddr, creds: Credentials, code: &str) -> anyhow::Result<SearchResult> {
    crate::search(&room_ticket_options(server, creds, code))
}
