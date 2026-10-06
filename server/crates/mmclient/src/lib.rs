//! A matchmaking client that behaves like Slippi's `SlippiMatchmaking`
//! (`Ishiiruka Source/Core/Core/Slippi/SlippiMatchmaking.cpp`):
//!
//! 1. bind a local UDP port (`41000 + rand % 10000`, or a forced one) and keep
//!    it for the P2P connection, so the NAT mapping the mm server saw is the
//!    one the peer punches through;
//! 2. ENet-connect to the mm server (20 × 500 ms), send `create-ticket` with the
//!    LAN address found by "connecting" a UDP socket to the server;
//! 3. wait 5 s for `create-ticket-resp`, then for `get-ticket-resp`;
//! 4. pick the peer address with Slippi's rule: LAN address when both players
//!    share an external IP, else the external one;
//! 5. optionally (`punch`), disconnect from mm and ENet-connect to the peer
//!    from the same port within Slippi's 8 s window, exchanging one packet each
//!    way to prove the path works in both directions.
//!
//! Used by the end-to-end tests, and by ppharness / game-integration work via
//! the `mmclient` binary.

use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, ToSocketAddrs};
use std::time::{Duration, Instant};

use common::codes::encode_search_code_fullwidth;
use common::net::{local_ip_towards, EnetSocket};
use common::proto::{
    CreateTicket, CreateTicketResp, GetTicketResp, Search, TicketUser, CREATE_TICKET, CREATE_TICKET_RESP,
    GET_TICKET_RESP, MM_CHANNEL, MM_CHANNEL_COUNT,
};
use rand::Rng;
use rusty_enet::{Event, Host, HostSettings, Packet, PeerID};
use serde::{Deserialize, Serialize};

/// How the target code is put into `search.connectCode`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum CodeEncoding {
    /// Full-width Shift-JIS, as Melee's name-entry screen produces.
    Fullwidth,
    /// Plain ASCII bytes.
    Ascii,
}

#[derive(Debug, Clone)]
pub struct Credentials {
    pub uid: String,
    pub play_key: String,
    pub connect_code: Option<String>,
    pub display_name: Option<String>,
}

/// The `user.json` the launcher writes (Slippi-identical).
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UserJsonFile {
    pub uid: String,
    pub play_key: String,
    #[serde(default)]
    pub connect_code: Option<String>,
    #[serde(default)]
    pub display_name: Option<String>,
}

impl From<UserJsonFile> for Credentials {
    fn from(u: UserJsonFile) -> Self {
        Credentials { uid: u.uid, play_key: u.play_key, connect_code: u.connect_code, display_name: u.display_name }
    }
}

#[derive(Debug, Clone)]
pub struct SearchOptions {
    pub server: SocketAddr,
    pub creds: Credentials,
    pub mode: u8,
    /// Target connect code (Direct/Teams), empty for queue modes.
    pub target: String,
    pub encoding: CodeEncoding,
    pub app_version: String,
    /// Forced local port ("Force Netplay Port"); random 41000-50999 otherwise.
    pub local_port: Option<u16>,
    /// Forced LAN IP ("Force LAN IP").
    pub lan_ip: Option<Ipv4Addr>,
    /// Give up waiting for a match after this long (the real client waits forever).
    pub match_timeout: Option<Duration>,
    /// After a match, try the P2P connection.
    pub punch: bool,
    pub punch_timeout: Duration,
}

impl SearchOptions {
    pub fn direct(server: SocketAddr, creds: Credentials, target: &str) -> Self {
        SearchOptions {
            server,
            creds,
            mode: 2,
            target: target.into(),
            encoding: CodeEncoding::Fullwidth,
            app_version: "0.1.0".into(),
            local_port: None,
            lan_ip: None,
            match_timeout: Some(Duration::from_secs(30)),
            punch: false,
            punch_timeout: Duration::from_secs(8),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Status {
    Matched,
    MmConnectFailed,
    CreateTimeout,
    CreateError,
    TicketError,
    MmDisconnected,
    MatchTimeout,
    InvalidResponse,
    P2pFailed,
}

impl Status {
    pub fn exit_code(&self) -> i32 {
        match self {
            Status::Matched => 0,
            Status::CreateError => 2,
            Status::TicketError => 3,
            Status::MatchTimeout => 4,
            Status::P2pFailed => 5,
            _ => 1,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct P2pResult {
    pub connected: bool,
    pub elapsed_ms: u64,
    pub peer_address: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SearchResult {
    pub status: Status,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    pub local_port: u16,
    pub lan_address: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub create_response: Option<CreateTicketResp>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub ticket_response: Option<GetTicketResp>,
    /// Remote addresses chosen with Slippi's LAN rule, one per remote player.
    #[serde(default)]
    pub remote_addresses: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub p2p: Option<P2pResult>,
}

pub fn resolve(server: &str) -> anyhow::Result<SocketAddr> {
    let with_port =
        if server.contains(':') { server.to_string() } else { format!("{server}:{}", common::proto::MM_PORT) };
    with_port.to_socket_addrs()?.find(|a| a.is_ipv4()).ok_or_else(|| anyhow::anyhow!("no IPv4 address for {server}"))
}

fn bind_host(opts: &SearchOptions) -> anyhow::Result<(Host<EnetSocket>, u16)> {
    let mut rng = rand::thread_rng();
    let mut last_err = None;
    for _ in 0..15 {
        let port = opts.local_port.unwrap_or_else(|| 41000 + rng.gen_range(0..10000));
        match EnetSocket::bind(SocketAddr::from((Ipv4Addr::UNSPECIFIED, port))) {
            Ok(sock) => {
                let host = Host::new(
                    sock,
                    HostSettings { peer_limit: 8, channel_limit: MM_CHANNEL_COUNT, ..Default::default() },
                )
                .map_err(|e| anyhow::anyhow!("ENet host: {e:?}"))?;
                return Ok((host, port));
            }
            Err(e) => last_err = Some(e),
        }
        if opts.local_port.is_some() {
            break;
        }
    }
    Err(anyhow::anyhow!("failed to bind a local port: {:?}", last_err))
}

enum Recv {
    Message(serde_json::Value),
    Disconnected,
    Timeout,
}

fn receive(host: &mut Host<EnetSocket>, mm: PeerID, timeout: Duration) -> Recv {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        match host.service() {
            Ok(Some(Event::Receive { peer, packet, .. })) if peer.id() == mm => {
                return match serde_json::from_slice(packet.data()) {
                    Ok(v) => Recv::Message(v),
                    Err(_) => Recv::Message(serde_json::Value::Null),
                };
            }
            Ok(Some(Event::Disconnect { peer, .. })) if peer.id() == mm => return Recv::Disconnected,
            Ok(Some(_)) => {}
            Ok(None) => std::thread::sleep(Duration::from_millis(2)),
            Err(_) => std::thread::sleep(Duration::from_millis(5)),
        }
    }
    Recv::Timeout
}

fn disconnect_mm(host: &mut Host<EnetSocket>, mm: PeerID) {
    host.peer_mut(mm).disconnect(0);
    let deadline = Instant::now() + Duration::from_secs(3);
    while Instant::now() < deadline {
        match host.service() {
            Ok(Some(Event::Disconnect { peer, .. })) if peer.id() == mm => return,
            Ok(Some(_)) => {}
            _ => std::thread::sleep(Duration::from_millis(2)),
        }
    }
    host.peer_mut(mm).reset();
}

/// Connects to the mm server and returns the peer id (Slippi: 20 × 500 ms).
fn connect_mm(host: &mut Host<EnetSocket>, server: SocketAddr) -> Option<PeerID> {
    let mm = host.connect(server, MM_CHANNEL_COUNT, 0).ok()?.id();
    let deadline = Instant::now() + Duration::from_secs(10);
    while Instant::now() < deadline {
        match host.service() {
            Ok(Some(Event::Connect { peer, .. })) if peer.id() == mm => return Some(mm),
            Ok(Some(Event::Disconnect { peer, .. })) if peer.id() == mm => return None,
            Ok(Some(_)) => {}
            _ => std::thread::sleep(Duration::from_millis(2)),
        }
    }
    None
}

/// Slippi's peer-address rule (`SlippiMatchmaking.cpp:590-609`).
pub fn choose_remote_addresses(resp: &GetTicketResp) -> Vec<String> {
    let players = resp.players.clone().unwrap_or_default();
    let local_ext_ip = players
        .iter()
        .find(|p| p.is_local_player)
        .map(|p| p.ip_address.split(':').next().unwrap_or("").to_string())
        .unwrap_or_default();
    players
        .iter()
        .filter(|p| !p.is_local_player)
        .map(|p| {
            let ext_ip = p.ip_address.split(':').next().unwrap_or("");
            if ext_ip != local_ext_ip || p.ip_address_lan.is_empty() {
                p.ip_address.clone()
            } else {
                p.ip_address_lan.clone()
            }
        })
        .collect()
}

fn punch(host: &mut Host<EnetSocket>, remote: SocketAddr, timeout: Duration) -> P2pResult {
    let start = Instant::now();
    let _ = host.connect(remote, MM_CHANNEL_COUNT, 0);
    let mut connected: Option<PeerID> = None;
    let mut got_hello = false;
    let mut sent_hello = false;
    while start.elapsed() < timeout && !(got_hello && sent_hello) {
        match host.service() {
            Ok(Some(Event::Connect { peer, .. })) => {
                if peer.address() == Some(remote) && connected.is_none() {
                    connected = Some(peer.id());
                    let _ = peer.send(0, &Packet::reliable(&b"pp-hello"[..]));
                    sent_hello = true;
                }
            }
            Ok(Some(Event::Receive { peer, packet, .. })) => {
                if peer.address() == Some(remote) && packet.data() == b"pp-hello" {
                    got_hello = true;
                }
            }
            Ok(Some(_)) => {}
            _ => std::thread::sleep(Duration::from_millis(2)),
        }
    }
    let ok = got_hello && sent_hello;
    let elapsed_ms = start.elapsed().as_millis() as u64;
    // Let the hello flush, then close politely.
    let linger = Instant::now();
    while linger.elapsed() < Duration::from_millis(300) {
        if !matches!(host.service(), Ok(Some(_))) {
            std::thread::sleep(Duration::from_millis(2));
        }
    }
    let ids: Vec<PeerID> = host.connected_peers().map(|p| p.id()).collect();
    for id in ids {
        host.peer_mut(id).disconnect(0);
    }
    host.flush();
    P2pResult { connected: ok, elapsed_ms, peer_address: remote.to_string() }
}

/// Runs one search, like one pass of Slippi's matchmaking thread.
pub fn search(opts: &SearchOptions) -> anyhow::Result<SearchResult> {
    let (mut host, local_port) = bind_host(opts)?;
    let lan_ip = opts.lan_ip.or_else(|| local_ip_towards(opts.server));
    let lan_address = lan_ip.map(|ip| SocketAddrV4::new(ip, local_port).to_string()).unwrap_or_default();
    let mut result = SearchResult {
        status: Status::MmConnectFailed,
        error: None,
        local_port,
        lan_address: lan_address.clone(),
        create_response: None,
        ticket_response: None,
        remote_addresses: vec![],
        p2p: None,
    };

    let Some(mm) = connect_mm(&mut host, opts.server) else {
        result.error = Some("Failed to connect to mm server".into());
        return Ok(result);
    };

    let code_bytes = match opts.encoding {
        CodeEncoding::Fullwidth => encode_search_code_fullwidth(&opts.target),
        CodeEncoding::Ascii => opts.target.as_bytes().to_vec(),
    };
    let ticket = CreateTicket {
        kind: CREATE_TICKET.into(),
        user: TicketUser {
            uid: opts.creds.uid.clone(),
            play_key: opts.creds.play_key.clone(),
            connect_code: opts.creds.connect_code.clone(),
            display_name: opts.creds.display_name.clone(),
        },
        search: Search { mode: opts.mode, connect_code: code_bytes, game: None },
        app_version: opts.app_version.clone(),
        ip_address_lan: lan_address,
    };
    send_raw(&mut host, mm, serde_json::to_vec(&ticket)?.as_slice());

    match receive(&mut host, mm, Duration::from_secs(5)) {
        Recv::Message(v) => {
            if v.get("type").and_then(|t| t.as_str()) != Some(CREATE_TICKET_RESP) {
                result.status = Status::InvalidResponse;
                result.error = Some(format!("unexpected reply: {v}"));
                return Ok(result);
            }
            let resp: CreateTicketResp = serde_json::from_value(v)?;
            result.create_response = Some(resp.clone());
            if let Some(err) = resp.error.filter(|e| !e.is_empty()) {
                result.status = Status::CreateError;
                result.error = Some(err);
                disconnect_mm(&mut host, mm);
                return Ok(result);
            }
        }
        Recv::Disconnected => {
            result.status = Status::MmDisconnected;
            result.error = Some("Lost connection to the mm server".into());
            return Ok(result);
        }
        Recv::Timeout => {
            result.status = Status::CreateTimeout;
            result.error = Some("Failed to join mm queue".into());
            disconnect_mm(&mut host, mm);
            return Ok(result);
        }
    }

    let deadline = opts.match_timeout.map(|t| Instant::now() + t);
    let resp = loop {
        let wait = match deadline {
            Some(d) => d.saturating_duration_since(Instant::now()).min(Duration::from_secs(2)),
            None => Duration::from_secs(2),
        };
        if deadline.is_some_and(|d| Instant::now() >= d) {
            result.status = Status::MatchTimeout;
            result.error = Some("No match before the client timeout".into());
            disconnect_mm(&mut host, mm);
            return Ok(result);
        }
        match receive(&mut host, mm, wait.max(Duration::from_millis(10))) {
            Recv::Message(v) => {
                if v.get("type").and_then(|t| t.as_str()) != Some(GET_TICKET_RESP) {
                    result.status = Status::InvalidResponse;
                    result.error = Some(format!("unexpected reply: {v}"));
                    return Ok(result);
                }
                break serde_json::from_value::<GetTicketResp>(v)?;
            }
            Recv::Disconnected => {
                result.status = Status::MmDisconnected;
                result.error = Some("Lost connection to the mm server".into());
                return Ok(result);
            }
            Recv::Timeout => continue,
        }
    };
    result.ticket_response = Some(resp.clone());
    if let Some(err) = resp.error.clone().filter(|e| !e.is_empty()) {
        result.status = Status::TicketError;
        result.error = Some(err);
        return Ok(result);
    }
    result.remote_addresses = choose_remote_addresses(&resp);
    result.status = Status::Matched;
    disconnect_mm(&mut host, mm);

    if opts.punch {
        let Some(remote) = result.remote_addresses.first().and_then(|a| a.parse::<SocketAddr>().ok()) else {
            result.status = Status::P2pFailed;
            result.error = Some("no usable peer address".into());
            return Ok(result);
        };
        let p2p = punch(&mut host, remote, opts.punch_timeout);
        if !p2p.connected {
            result.status = Status::P2pFailed;
            result.error = Some("Timed out waiting for the opponent to connect".into());
        }
        result.p2p = Some(p2p);
    }
    Ok(result)
}

fn send_raw(host: &mut Host<EnetSocket>, peer: PeerID, data: &[u8]) {
    let _ = host.peer_mut(peer).send(MM_CHANNEL, &Packet::reliable(data));
    host.flush();
}

/// Connects, sends one arbitrary packet and collects replies until the server
/// disconnects or `wait` passes. For protocol tests (malformed input).
pub fn send_raw_packet(
    server: SocketAddr,
    data: &[u8],
    wait: Duration,
) -> anyhow::Result<(Vec<serde_json::Value>, bool)> {
    let sock = EnetSocket::bind(SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0)))?;
    let mut host =
        Host::new(sock, HostSettings { peer_limit: 1, channel_limit: MM_CHANNEL_COUNT, ..Default::default() })
            .map_err(|e| anyhow::anyhow!("ENet host: {e:?}"))?;
    let mm = connect_mm(&mut host, server).ok_or_else(|| anyhow::anyhow!("could not connect to mm"))?;
    send_raw(&mut host, mm, data);
    let mut replies = vec![];
    let deadline = Instant::now() + wait;
    while Instant::now() < deadline {
        match receive(&mut host, mm, deadline.saturating_duration_since(Instant::now())) {
            Recv::Message(v) => replies.push(v),
            Recv::Disconnected => return Ok((replies, true)),
            Recv::Timeout => break,
        }
    }
    Ok((replies, false))
}

#[cfg(test)]
mod tests {
    use super::*;
    use common::proto::Player;

    fn player(local: bool, ext: &str, lan: &str) -> Player {
        Player {
            uid: "u".into(),
            display_name: "d".into(),
            connect_code: "AA#1".into(),
            port: if local { 1 } else { 2 },
            is_local_player: local,
            ip_address: ext.into(),
            ip_address_lan: lan.into(),
            chat_messages: vec![],
            rank: None,
            is_bot: false,
        }
    }

    #[test]
    fn slippi_lan_rule() {
        let resp = |a: Player, b: Player| GetTicketResp { players: Some(vec![a, b]), ..Default::default() };
        // Different external IPs: use the external address.
        let r = resp(
            player(true, "1.1.1.1:41000", "192.168.0.2:41000"),
            player(false, "2.2.2.2:42000", "192.168.0.3:42000"),
        );
        assert_eq!(choose_remote_addresses(&r), vec!["2.2.2.2:42000"]);
        // Same external IP (same LAN, VPN node or CGNAT): use the LAN address.
        let r = resp(
            player(true, "1.1.1.1:41000", "192.168.0.2:41000"),
            player(false, "1.1.1.1:42000", "192.168.0.3:42000"),
        );
        assert_eq!(choose_remote_addresses(&r), vec!["192.168.0.3:42000"]);
        // Same external IP but no LAN address: external.
        let r = resp(player(true, "1.1.1.1:41000", ""), player(false, "1.1.1.1:42000", ""));
        assert_eq!(choose_remote_addresses(&r), vec!["1.1.1.1:42000"]);
    }

    #[test]
    fn resolve_adds_default_port() {
        assert_eq!(resolve("127.0.0.1").unwrap().port(), 43113);
        assert_eq!(resolve("127.0.0.1:5").unwrap().port(), 5);
    }
}
