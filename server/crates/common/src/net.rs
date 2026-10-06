//! Networking helpers shared by the mm server and `mmclient`.

use std::io::{self, ErrorKind};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, SocketAddrV4, UdpSocket};

use rusty_enet::{PacketReceived, Socket, SocketOptions, MTU_MAX};

/// A UDP socket for ENet that survives the errors a public UDP server sees.
///
/// On Windows, `recv_from` fails with `WSAECONNRESET` after an earlier send hit
/// an ICMP port-unreachable (a client that went away). Linux reports
/// `ECONNREFUSED` the same way on connected sockets. Both are harmless for a
/// server; returning them from `receive` would abort `Host::service`, so we
/// skip them and keep reading.
pub struct EnetSocket(pub UdpSocket);

impl EnetSocket {
    pub fn bind(addr: SocketAddr) -> io::Result<Self> {
        Ok(EnetSocket(UdpSocket::bind(addr)?))
    }

    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        self.0.local_addr()
    }
}

fn is_transient(err: &io::Error) -> bool {
    matches!(err.kind(), ErrorKind::ConnectionReset | ErrorKind::ConnectionRefused | ErrorKind::Interrupted)
}

impl Socket for EnetSocket {
    type Address = SocketAddr;
    type Error = io::Error;

    fn init(&mut self, _opts: SocketOptions) -> Result<(), io::Error> {
        self.0.set_nonblocking(true)?;
        Ok(())
    }

    fn send(&mut self, address: SocketAddr, buffer: &[u8]) -> Result<usize, io::Error> {
        match self.0.send_to(buffer, address) {
            Ok(n) => Ok(n),
            Err(e) if e.kind() == ErrorKind::WouldBlock => Ok(0),
            // Treat as sent-and-lost; ENet's reliability layer handles it.
            Err(e) if is_transient(&e) => Ok(buffer.len()),
            // Unroutable destinations (bad LAN address from a client) must not kill the loop.
            Err(e) if matches!(e.kind(), ErrorKind::AddrNotAvailable | ErrorKind::PermissionDenied) => Ok(buffer.len()),
            Err(e) => Err(e),
        }
    }

    fn receive(&mut self, buffer: &mut [u8; MTU_MAX]) -> Result<Option<(SocketAddr, PacketReceived)>, io::Error> {
        // Bounded so a flood of resets cannot spin forever.
        for _ in 0..64 {
            match self.0.recv_from(buffer) {
                Ok((n, from)) => return Ok(Some((from, PacketReceived::Complete(n)))),
                Err(e) if e.kind() == ErrorKind::WouldBlock => return Ok(None),
                Err(e) if is_transient(&e) => continue,
                // Windows reports datagrams larger than the buffer as an error; drop them.
                Err(e) if e.raw_os_error() == Some(10040) => continue,
                Err(e) => return Err(e),
            }
        }
        Ok(None)
    }
}

/// Converts a peer address to the IPv4 form the Slippi client can parse
/// (it splits on `:`). IPv4-mapped IPv6 addresses are unwrapped; real IPv6
/// addresses are not supported by the client and give `None`.
pub fn to_v4(addr: SocketAddr) -> Option<SocketAddrV4> {
    match addr {
        SocketAddr::V4(a) => Some(a),
        SocketAddr::V6(a) => a.ip().to_ipv4_mapped().map(|ip| SocketAddrV4::new(ip, a.port())),
    }
}

/// Sanitizes the `ipAddressLan` a client reported. Slippi sends `"a.b.c.d:port"`,
/// or `""` when it could not find a local address; "Force LAN IP" lets users
/// type anything. Anything that is not an IPv4 `ip:port` with a non-zero port
/// becomes `""`, which makes the peer fall back to the external address
/// (`SlippiMatchmaking.cpp:600`).
pub fn sanitize_lan_addr(s: &str) -> String {
    match s.trim().parse::<SocketAddrV4>() {
        Ok(a) if a.port() != 0 && !a.ip().is_unspecified() && !a.ip().is_broadcast() && !a.ip().is_multicast() => {
            a.to_string()
        }
        _ => String::new(),
    }
}

/// The local IPv4 address the OS would use to reach `remote`, found by
/// "connecting" a UDP socket (no packet is sent). Same method as Slippi's
/// `getLocalAddress` (`SlippiMatchmaking.cpp:247-276`).
pub fn local_ip_towards(remote: SocketAddr) -> Option<Ipv4Addr> {
    let s = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).ok()?;
    s.connect(remote).ok()?;
    match s.local_addr().ok()?.ip() {
        IpAddr::V4(ip) if !ip.is_unspecified() => Some(ip),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lan_addr_sanitizing() {
        assert_eq!(sanitize_lan_addr("192.168.1.5:41234"), "192.168.1.5:41234");
        assert_eq!(sanitize_lan_addr(" 10.0.0.2:50000 "), "10.0.0.2:50000");
        assert_eq!(sanitize_lan_addr("100.64.1.1:41000"), "100.64.1.1:41000");
        assert_eq!(sanitize_lan_addr(""), "");
        assert_eq!(sanitize_lan_addr("192.168.1.5"), "");
        assert_eq!(sanitize_lan_addr("192.168.1.5:0"), "");
        assert_eq!(sanitize_lan_addr("0.0.0.0:41000"), "");
        assert_eq!(sanitize_lan_addr("[::1]:41000"), "");
        assert_eq!(sanitize_lan_addr("host:41000"), "");
        assert_eq!(sanitize_lan_addr("1.2.3.4:41000:5"), "");
    }

    #[test]
    fn v4_mapping() {
        let mapped: SocketAddr = "[::ffff:1.2.3.4]:5".parse().unwrap();
        assert_eq!(to_v4(mapped).unwrap().to_string(), "1.2.3.4:5");
        let v6: SocketAddr = "[2001:db8::1]:5".parse().unwrap();
        assert!(to_v4(v6).is_none());
    }

    #[test]
    fn local_ip_for_loopback() {
        let ip = local_ip_towards("127.0.0.1:43113".parse().unwrap());
        assert_eq!(ip, Some(Ipv4Addr::LOCALHOST));
    }
}
