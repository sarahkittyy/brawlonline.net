//! Region buckets for the Unranked queue.
//!
//! Slippi's tickets carry no region, ping or latency field (design 1.4), so the server has to infer
//! the region from the address the ticket came from. The design's plan is an offline GeoIP
//! database (DB-IP Lite). For the friends-only start we keep it to a small table of IPv4 prefixes
//! per region, read from `MM_REGIONS_FILE`:
//!
//! ```json
//! { "na": ["203.0.113.0/24", "198.51.100.0/22"], "eu": ["192.0.2.0/24"] }
//! ```
//!
//! The longest matching prefix wins; an address no prefix matches (and every address when no file
//! is configured) is in [`DEFAULT_REGION`]. With no file the queue is therefore one FIFO bucket.
//! A GeoIP lookup can later replace [`RegionMap::region_of`] without touching the queue.

use std::collections::HashMap;
use std::net::{Ipv4Addr, SocketAddr};

use common::net::to_v4;

/// The region of addresses no prefix matches.
pub const DEFAULT_REGION: &str = "other";

#[derive(Debug, Clone, Default, PartialEq)]
pub struct RegionMap {
    /// (network, prefix length, region), longest prefix first.
    nets: Vec<(u32, u8, String)>,
}

fn parse_cidr(s: &str) -> anyhow::Result<(u32, u8)> {
    let (ip, len) = s.trim().split_once('/').unwrap_or((s.trim(), "32"));
    let ip: Ipv4Addr = ip.parse().map_err(|_| anyhow::anyhow!("bad IPv4 address in {s:?}"))?;
    let len: u8 = len.parse().map_err(|_| anyhow::anyhow!("bad prefix length in {s:?}"))?;
    anyhow::ensure!(len <= 32, "bad prefix length in {s:?}");
    Ok((u32::from(ip) & mask(len), len))
}

fn mask(len: u8) -> u32 {
    if len == 0 {
        0
    } else {
        u32::MAX << (32 - len)
    }
}

impl RegionMap {
    pub fn parse(json: &str) -> anyhow::Result<Self> {
        let raw: HashMap<String, Vec<String>> = serde_json::from_str(json)?;
        let mut nets = Vec::new();
        for (region, cidrs) in raw {
            let region = region.trim().to_lowercase();
            anyhow::ensure!(!region.is_empty(), "empty region name");
            for c in cidrs {
                let (net, len) = parse_cidr(&c)?;
                nets.push((net, len, region.clone()));
            }
        }
        nets.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
        Ok(RegionMap { nets })
    }

    pub fn load(path: Option<&str>) -> anyhow::Result<Self> {
        match path {
            Some(p) if !p.is_empty() => Self::parse(&std::fs::read_to_string(p)?),
            _ => Ok(Self::default()),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.nets.is_empty()
    }

    /// The region of a client address (IPv6 other than IPv4-mapped: the default region).
    pub fn region_of(&self, addr: SocketAddr) -> String {
        let Some(v4) = to_v4(addr) else { return DEFAULT_REGION.into() };
        let ip = u32::from(*v4.ip());
        self.nets
            .iter()
            .find(|(net, len, _)| ip & mask(*len) == *net)
            .map(|(_, _, r)| r.clone())
            .unwrap_or_else(|| DEFAULT_REGION.into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn a(s: &str) -> SocketAddr {
        format!("{s}:41000").parse().unwrap()
    }

    #[test]
    fn longest_prefix_wins_and_unknown_is_default() {
        let m = RegionMap::parse(r#"{"NA": ["10.0.0.0/8", "203.0.113.7"], "eu": ["10.1.0.0/16"]}"#).unwrap();
        assert_eq!(m.region_of(a("10.2.3.4")), "na");
        assert_eq!(m.region_of(a("10.1.3.4")), "eu");
        assert_eq!(m.region_of(a("203.0.113.7")), "na");
        assert_eq!(m.region_of(a("203.0.113.8")), DEFAULT_REGION);
        let mapped: SocketAddr = "[::ffff:10.1.0.1]:5".parse().unwrap();
        assert_eq!(m.region_of(mapped), "eu");
        let v6: SocketAddr = "[2001:db8::1]:5".parse().unwrap();
        assert_eq!(m.region_of(v6), DEFAULT_REGION);
    }

    #[test]
    fn empty_map_puts_everyone_in_one_bucket() {
        let m = RegionMap::load(None).unwrap();
        assert!(m.is_empty());
        assert_eq!(m.region_of(a("1.2.3.4")), DEFAULT_REGION);
        assert_eq!(m.region_of(a("127.0.0.1")), DEFAULT_REGION);
    }

    #[test]
    fn bad_input_is_an_error() {
        for bad in [r#"{"na": ["10.0.0.0/33"]}"#, r#"{"na": ["nonsense"]}"#, r#"{"": ["1.2.3.4"]}"#, "[]"] {
            assert!(RegionMap::parse(bad).is_err(), "{bad}");
        }
        assert_eq!(RegionMap::parse(r#"{"x": ["0.0.0.0/0"]}"#).unwrap().region_of(a("8.8.8.8")), "x");
    }
}
