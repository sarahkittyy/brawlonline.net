//! The IPv6 probe: how mm learns a game's IPv6 address.
//!
//! Matchmaking and the rooms speak ENet over IPv4 (every player needs IPv4). A game that also has
//! IPv6 sends a few of these datagrams from the IPv6 socket its P2P link will use to mm's IPv6
//! listener (`MM_LISTEN6`); mm notes the address it came from under the probe's token, and tells
//! the players of a match each other's (`ipAddress6`, beside `ipAddress`), so a pair whose IPv4
//! NATs let nothing through can still connect over IPv6, where there is no NAT. The token says
//! whose address it is: a `create-ticket`'s `ipv6Token` (random, sent only to mm), or a Brawl
//! Mods member's endpoint token (`mods-state`).
//!
//! Request: `BOP6`, version 1, the token's length, the token, zero padding to at least
//! [`REQUEST_MIN`] bytes. Reply: `BOP6`, 0x81, the 16 address bytes and the port (big endian) mm
//! saw: [`REPLY_LEN`] bytes, less than any request, so the probe amplifies nothing.

use std::net::{Ipv6Addr, SocketAddrV6};

pub const MAGIC: &[u8; 4] = b"BOP6";
pub const VERSION: u8 = 1;
const REPLY_KIND: u8 = 0x81;
pub const REQUEST_MIN: usize = 48;
pub const REPLY_LEN: usize = 4 + 1 + 16 + 2;
pub const TOKEN_MIN: usize = 8;
pub const TOKEN_MAX: usize = 64;

/// Whether `token` may name an address: [`TOKEN_MIN`]-[`TOKEN_MAX`] characters of `A-Z a-z 0-9 _ -`.
pub fn valid_token(token: &str) -> bool {
    (TOKEN_MIN..=TOKEN_MAX).contains(&token.len())
        && token.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

/// The token of a request, if `data` is one.
pub fn parse_request(data: &[u8]) -> Option<&str> {
    if data.len() < REQUEST_MIN || &data[..4] != MAGIC || data[4] != VERSION {
        return None;
    }
    let len = data[5] as usize;
    let token = std::str::from_utf8(data.get(6..6 + len)?).ok()?;
    valid_token(token).then_some(token)
}

pub fn request(token: &str) -> Vec<u8> {
    let mut v = Vec::with_capacity(REQUEST_MIN.max(6 + token.len()));
    v.extend_from_slice(MAGIC);
    v.push(VERSION);
    v.push(token.len().min(255) as u8);
    v.extend_from_slice(token.as_bytes());
    v.resize(v.len().max(REQUEST_MIN), 0);
    v
}

pub fn reply(seen: SocketAddrV6) -> [u8; REPLY_LEN] {
    let mut r = [0u8; REPLY_LEN];
    r[..4].copy_from_slice(MAGIC);
    r[4] = REPLY_KIND;
    r[5..21].copy_from_slice(&seen.ip().octets());
    r[21..23].copy_from_slice(&seen.port().to_be_bytes());
    r
}

pub fn parse_reply(data: &[u8]) -> Option<SocketAddrV6> {
    if data.len() != REPLY_LEN || &data[..4] != MAGIC || data[4] != REPLY_KIND {
        return None;
    }
    let ip: [u8; 16] = data[5..21].try_into().ok()?;
    Some(SocketAddrV6::new(Ipv6Addr::from(ip), u16::from_be_bytes([data[21], data[22]]), 0, 0))
}

/// Whether peers on the internet can be told to reach this address: a global unicast IPv6
/// address (2000::/3), not an IPv4-mapped, documentation, or other special one. `loopback`
/// allows ::1 (tests on one machine).
pub fn reachable(a: &Ipv6Addr, loopback: bool) -> bool {
    if a.is_loopback() {
        return loopback;
    }
    let s = a.segments();
    // 2000::/3, minus 2001:db8::/32 (documentation), 2002::/16 (6to4) and 2001::/32 (Teredo),
    // whose relays a peer cannot count on.
    (s[0] & 0xe000) == 0x2000 && !(s[0] == 0x2001 && (s[1] == 0x0db8 || s[1] == 0)) && s[0] != 0x2002
}

/// `[addr]:port`, the form the game parses.
pub fn endpoint_text(a: SocketAddrV6) -> String {
    format!("[{}]:{}", a.ip(), a.port())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requests_and_replies_round_trip() {
        let r = request("abcdef0123456789");
        assert!(r.len() >= REQUEST_MIN);
        assert_eq!(parse_request(&r), Some("abcdef0123456789"));
        let a: SocketAddrV6 = "[2600:1700::5]:41234".parse().unwrap();
        let rep = reply(a);
        assert!(rep.len() < REQUEST_MIN, "a reply is smaller than a request");
        assert_eq!(parse_reply(&rep), Some(a));
        assert_eq!(endpoint_text(a), "[2600:1700::5]:41234");
    }

    #[test]
    fn bad_requests_are_refused() {
        let good = request("abcdef0123456789");
        assert_eq!(parse_request(&good[..REQUEST_MIN - 1]), None, "too short");
        let mut v = good.clone();
        v[0] = b'X';
        assert_eq!(parse_request(&v), None, "magic");
        let mut v = good.clone();
        v[4] = 2;
        assert_eq!(parse_request(&v), None, "version");
        let mut v = good.clone();
        v[5] = 200;
        assert_eq!(parse_request(&v), None, "length past the end");
        assert_eq!(parse_request(&request("short")), None, "token too short");
        assert_eq!(parse_request(&request("has space in it")), None);
        assert_eq!(parse_request(&request(&"x".repeat(65))), None);
        assert!(valid_token(&"a".repeat(64)));
        assert_eq!(parse_reply(&good), None);
    }

    #[test]
    fn only_global_addresses_are_reachable() {
        let r = |s: &str, lo| reachable(&s.parse().unwrap(), lo);
        assert!(r("2604:2dc0:101:200::30fc", false));
        assert!(r("2a01:4f8::1", false));
        assert!(!r("::1", false));
        assert!(r("::1", true));
        assert!(!r("fe80::1", false), "link-local");
        assert!(!r("fd7a:115c:a1e0::1", false), "unique local");
        assert!(!r("::ffff:192.0.2.1", false), "IPv4-mapped");
        assert!(!r("2001:db8::1", false), "documentation");
        assert!(!r("2002:c000:201::1", false), "6to4");
        assert!(!r("2001:0:4136:e378::1", false), "Teredo");
        assert!(!r("ff02::1", false), "multicast");
        assert!(!r("::", false));
    }
}
