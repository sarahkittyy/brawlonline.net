//! Where a ticket's address is, and the ping two players can expect between each other.
//!
//! Slippi's tickets carry no ping (design 1.4), and the two players have not talked to each other
//! yet when mm pairs them, so the round trip is estimated from distance: each address is placed
//! with an offline GeoIP database (DB-IP's free "IP to City Lite", CC BY 4.0, MaxMind's `.mmdb`
//! format, read from `MM_GEOIP_FILE`) and the great-circle distance between the two places becomes
//! a round trip with [`estimate_rtt_ms`]. An address the database does not place (LAN and loopback
//! addresses, IPv6 other than IPv4-mapped, every address without a database) has no location, and
//! a pair with no estimate is never held back for ping.
//!
//! The database is memory-mapped. Replace it only by renaming a new file over it (as
//! `deploy/sarahvps2/pp-geoip-update` does): mm keeps the old mapping until it sees the new file's
//! modification time ([`GeoDb::refresh`]), and writing into the mapped file in place is undefined
//! behaviour.

use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::SystemTime;

use common::net::to_v4;
use maxminddb::{path, Mmap, Reader};

// Fitted to what games measured (2026-10-23, 507 prod Unranked, Ranked and Direct matches: the
// speed reports' in-game ping against the distance mm placed the pair at). The first constants
// (15 ms, 1.4) read the pure line cost and were far too low: matches estimated at 80-100 ms played
// at 146 ms (median), so a "Matchmaking Ping Limit" of 100 paired players at 130 and more. Now
// about two thirds of games play at or under the estimate (median 7 ms under); what is left over
// is mostly a few players' own connections, which no distance shows.

/// What a round trip costs before distance: both players' home networks, the routers between and
/// the game's own frame of polling (the in-game ping is a frame longer than the network's).
pub const RTT_BASE_MS: f64 = 55.0;
/// How much longer the routes are than the great circle.
pub const ROUTE_FACTOR: f64 = 1.65;
/// Light in fibre: about 200 km per millisecond.
pub const FIBER_KM_PER_MS: f64 = 200.0;

const EARTH_RADIUS_KM: f64 = 6371.0;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Loc {
    pub lat: f64,
    pub lon: f64,
}

/// Great-circle distance in kilometres (haversine).
pub fn distance_km(a: Loc, b: Loc) -> f64 {
    let (la, lb) = (a.lat.to_radians(), b.lat.to_radians());
    let dlat = lb - la;
    let dlon = (b.lon - a.lon).to_radians();
    let h = (dlat / 2.0).sin().powi(2) + la.cos() * lb.cos() * (dlon / 2.0).sin().powi(2);
    2.0 * EARTH_RADIUS_KM * h.sqrt().min(1.0).asin()
}

/// The round trip (the game's "ping") two players at `a` and `b` can expect, in milliseconds.
/// Roughly: the same city 55-60, New York to Dallas 91, New York to Los Angeles 120, New York to
/// London 147, Los Angeles to Sydney 254.
pub fn estimate_rtt_ms(a: Loc, b: Loc) -> f64 {
    RTT_BASE_MS + 2.0 * distance_km(a, b) * ROUTE_FACTOR / FIBER_KM_PER_MS
}

enum Source {
    Mmdb(Reader<Mmap>),
    /// (network, prefix length, location), longest prefix first. Tests.
    Table(Vec<(u32, u8, Loc)>),
}

/// The GeoIP database, if one is configured. Cheap to clone (shared).
#[derive(Clone, Default)]
pub struct GeoDb {
    source: Option<Arc<Source>>,
    /// The database file and its modification time when it was opened.
    file: Option<(PathBuf, Option<SystemTime>)>,
}

impl std::fmt::Debug for GeoDb {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let kind = match self.source.as_deref() {
            None => "none",
            Some(Source::Mmdb(_)) => "mmdb",
            Some(Source::Table(_)) => "table",
        };
        f.debug_struct("GeoDb").field("source", &kind).field("file", &self.file).finish()
    }
}

fn mtime(path: &Path) -> Option<SystemTime> {
    std::fs::metadata(path).and_then(|m| m.modified()).ok()
}

fn open_mmdb(path: &Path) -> anyhow::Result<Reader<Mmap>> {
    // SAFETY: the file is only ever replaced by a rename (see the module docs), never written in
    // place, so the mapping stays valid for as long as this reader lives.
    let reader = unsafe { Reader::open_mmap(path) }.map_err(|e| anyhow::anyhow!("{}: {e}", path.display()))?;
    Ok(reader)
}

impl GeoDb {
    /// Opens `path` (`MM_GEOIP_FILE`). No path: no database. A path that cannot be opened is a
    /// warning, not an error: mm runs without ping estimates (and opens it once it appears).
    pub fn load(path: Option<&Path>) -> Self {
        let Some(path) = path.filter(|p| !p.as_os_str().is_empty()) else { return Self::default() };
        let mut db = GeoDb { source: None, file: Some((path.to_path_buf(), None)) };
        db.refresh();
        if db.source.is_none() {
            tracing::warn!(file = %path.display(), "no GeoIP database; pairs are not held back for ping");
        }
        db
    }

    /// A fixed prefix table instead of a database (tests): `("a.b.c.d/len", lat, lon)`.
    pub fn from_table(rows: &[(&str, f64, f64)]) -> Self {
        let mut nets: Vec<(u32, u8, Loc)> = rows
            .iter()
            .map(|(cidr, lat, lon)| {
                let (ip, len) = cidr.split_once('/').unwrap_or((cidr, "32"));
                let ip: Ipv4Addr = ip.parse().expect("IPv4 address");
                let len: u8 = len.parse().expect("prefix length");
                (u32::from(ip) & mask(len), len, Loc { lat: *lat, lon: *lon })
            })
            .collect();
        nets.sort_by_key(|n| std::cmp::Reverse(n.1));
        GeoDb { source: Some(Arc::new(Source::Table(nets))), file: None }
    }

    pub fn is_empty(&self) -> bool {
        self.source.is_none()
    }

    /// Opens the database file again if it has changed (or appeared) since it was opened.
    pub fn refresh(&mut self) {
        let Some((path, seen)) = self.file.clone() else { return };
        let now = mtime(&path);
        if now.is_none() || (now == seen && self.source.is_some()) {
            return;
        }
        match open_mmdb(&path) {
            Ok(reader) => {
                let build = reader.metadata().build_epoch;
                tracing::info!(file = %path.display(), build_epoch = build, "GeoIP database opened");
                self.source = Some(Arc::new(Source::Mmdb(reader)));
                self.file = Some((path, now));
            }
            Err(e) => {
                tracing::warn!(error = %e, "cannot open the GeoIP database; keeping the last one");
                self.file = Some((path, now));
            }
        }
    }

    /// Where a client address is, if the database knows.
    pub fn locate(&self, addr: SocketAddr) -> Option<Loc> {
        let v4 = *to_v4(addr)?.ip();
        if v4.is_private() || v4.is_loopback() || v4.is_link_local() || v4.is_unspecified() {
            return None;
        }
        match self.source.as_deref()? {
            Source::Mmdb(reader) => {
                let found = reader.lookup(IpAddr::V4(v4)).ok()?;
                let lat: f64 = found.decode_path(&path!["location", "latitude"]).ok()??;
                let lon: f64 = found.decode_path(&path!["location", "longitude"]).ok()??;
                Some(Loc { lat, lon })
            }
            Source::Table(nets) => {
                let ip = u32::from(v4);
                nets.iter().find(|(net, len, _)| ip & mask(*len) == *net).map(|(_, _, l)| *l)
            }
        }
    }
}

fn mask(len: u8) -> u32 {
    if len == 0 {
        0
    } else {
        u32::MAX << (32 - len)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const NEW_YORK: Loc = Loc { lat: 40.71, lon: -74.01 };
    const LOS_ANGELES: Loc = Loc { lat: 34.05, lon: -118.24 };
    const LONDON: Loc = Loc { lat: 51.51, lon: -0.13 };
    const SYDNEY: Loc = Loc { lat: -33.87, lon: 151.21 };

    #[test]
    fn distances_and_round_trips() {
        assert!((distance_km(NEW_YORK, LONDON) - 5570.0).abs() < 30.0);
        assert!((distance_km(NEW_YORK, LOS_ANGELES) - 3940.0).abs() < 30.0);
        assert_eq!(distance_km(LONDON, LONDON), 0.0);
        assert_eq!(estimate_rtt_ms(LONDON, LONDON), RTT_BASE_MS);
        let rtt = |a, b| estimate_rtt_ms(a, b).round();
        assert_eq!(rtt(NEW_YORK, LOS_ANGELES), 120.0);
        assert_eq!(rtt(NEW_YORK, LONDON), 147.0);
        assert_eq!(rtt(LOS_ANGELES, SYDNEY), 254.0);
        assert_eq!(rtt(SYDNEY, LOS_ANGELES), rtt(LOS_ANGELES, SYDNEY));
    }

    fn a(s: &str) -> SocketAddr {
        format!("{s}:41000").parse().unwrap()
    }

    #[test]
    fn table_lookup_and_unplaceable_addresses() {
        let db = GeoDb::from_table(&[
            ("203.0.113.0/24", 1.0, 2.0),
            ("203.0.113.128/25", 3.0, 4.0),
            ("10.0.0.0/8", 5.0, 6.0),
        ]);
        assert_eq!(db.locate(a("203.0.113.5")), Some(Loc { lat: 1.0, lon: 2.0 }));
        assert_eq!(db.locate(a("203.0.113.200")), Some(Loc { lat: 3.0, lon: 4.0 }));
        assert_eq!(db.locate("[::ffff:203.0.113.5]:1".parse().unwrap()), Some(Loc { lat: 1.0, lon: 2.0 }));
        assert_eq!(db.locate(a("198.51.100.1")), None);
        // Private, loopback and IPv6 addresses are never placed.
        assert_eq!(db.locate(a("10.1.2.3")), None);
        assert_eq!(db.locate(a("127.0.0.1")), None);
        assert_eq!(db.locate("[2001:db8::1]:1".parse().unwrap()), None);
    }

    #[test]
    fn no_database_or_a_missing_file_places_nothing() {
        assert!(GeoDb::load(None).is_empty());
        let missing = GeoDb::load(Some(Path::new("does/not/exist.mmdb")));
        assert!(missing.is_empty());
        assert_eq!(missing.locate(a("8.8.8.8")), None);
    }

    /// Against the real database, when `MM_TEST_GEOIP_FILE` points at one (not in CI).
    #[test]
    fn real_database_places_public_addresses() {
        let Some(file) = std::env::var_os("MM_TEST_GEOIP_FILE") else { return };
        let db = GeoDb::load(Some(Path::new(&file)));
        assert!(!db.is_empty());
        let google = db.locate(a("8.8.8.8")).expect("8.8.8.8 placed");
        assert!((-90.0..=90.0).contains(&google.lat) && (-180.0..=180.0).contains(&google.lon));
        assert_eq!(db.locate(a("192.168.1.1")), None);
        // Telstra in Sydney: far over the first limit from California.
        let sydney = db.locate(a("139.130.4.5")).expect("139.130.4.5 placed");
        assert!(estimate_rtt_ms(google, sydney) > 150.0);
    }
}
