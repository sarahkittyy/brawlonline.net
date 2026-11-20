//! The newest client build of each platform, read from the launcher's update feed: the folder
//! `pp-release client` publishes electron-builder's `latest.yml` (Windows), `latest-mac.yml` and
//! `latest-linux.yml` (x86_64) and `latest-linux-arm64.yml` to (`/var/www/brawlonline/updates/launcher`
//! in production). A game older than its platform's newest build may not play online
//! (`MM_UPDATE_FEED_DIR`): each platform goes live on its own (the macOS build waits for Apple, the
//! arm64 Linux build is published when it is done), so each is held to its own feed.
//!
//! EmissaryRecomp runs the same online core (`engine: emissary`; docs/design/brawl-online-core.md
//! §10, "Which engine"). Its `appVersion` is the Brawl Online release of the core it was built
//! from, so it is compared exactly as a Dolphin of that release: against the same platform's
//! Dolphin feed, or, once recomp builds are published with their own feed
//! ([`EMISSARY_FEEDS`], naming the core release of the newest recomp build), against that one,
//! as the macOS build is held to its own while it trails.

use std::collections::BTreeMap;
use std::path::Path;

use crate::engine::parse_version;

/// The platforms a build reports (`platform` in `hello` and tickets) and their feed files.
pub const FEEDS: [(&str, &str); 4] = [
    ("win", "latest.yml"),
    ("mac", "latest-mac.yml"),
    ("linux", "latest-linux.yml"),
    ("linux-arm64", "latest-linux-arm64.yml"),
];

/// EmissaryRecomp's feeds, per platform (as [`FEEDS`]'s): `version:` is the Brawl Online release
/// of the core in the newest published recomp build (not the recomp's own version). None is
/// published yet; until a platform's is, its recomp is held to that platform's Dolphin feed.
pub const EMISSARY_FEEDS: [(&str, &str); 4] = [
    ("win", "latest-emissary.yml"),
    ("mac", "latest-emissary-mac.yml"),
    ("linux", "latest-emissary-linux.yml"),
    ("linux-arm64", "latest-emissary-linux-arm64.yml"),
];

/// The version each platform's feed names.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FeedVersions {
    by_platform: BTreeMap<&'static str, String>,
    /// [`EMISSARY_FEEDS`]' versions.
    emissary: BTreeMap<&'static str, String>,
}

fn read_feeds(dir: &Path, feeds: &[(&'static str, &str)]) -> BTreeMap<&'static str, String> {
    let mut by_platform = BTreeMap::new();
    for (platform, file) in feeds {
        if let Some(v) = std::fs::read_to_string(dir.join(file)).ok().as_deref().and_then(feed_version) {
            by_platform.insert(*platform, v);
        }
    }
    by_platform
}

impl FeedVersions {
    /// Reads the feeds in `dir`. A missing or unreadable feed is left out.
    pub fn read(dir: &Path) -> Self {
        FeedVersions { by_platform: read_feeds(dir, &FEEDS), emissary: read_feeds(dir, &EMISSARY_FEEDS) }
    }

    pub fn from_pairs(pairs: &[(&str, &str)]) -> Self {
        let mut by_platform = BTreeMap::new();
        for (platform, version) in pairs {
            if let Some((p, _)) = FEEDS.iter().find(|(p, _)| p == platform) {
                by_platform.insert(*p, version.to_string());
            }
        }
        FeedVersions { by_platform, emissary: BTreeMap::new() }
    }

    /// With the recomp's feeds of [`EMISSARY_FEEDS`] (platform, version).
    pub fn with_emissary(mut self, pairs: &[(&str, &str)]) -> Self {
        for (platform, version) in pairs {
            if let Some((p, _)) = EMISSARY_FEEDS.iter().find(|(p, _)| p == platform) {
                self.emissary.insert(*p, version.to_string());
            }
        }
        self
    }

    pub fn is_empty(&self) -> bool {
        self.by_platform.is_empty() && self.emissary.is_empty()
    }

    /// The version a build of `version` for `platform` must have: its own feed's. A build that
    /// names no platform (older than the field), an unknown one or one without a feed can only be
    /// from a platform whose feed has reached its version (builds are published only through the
    /// feeds): the oldest of those feeds. So a Windows 0.1.41 is refused while the macOS feed is
    /// still at 0.1.28, and a macOS 0.1.28 is not. A build newer than every feed (a development
    /// build) has nothing to meet.
    ///
    /// `engine` ([`common::client_engine::name`]): an `emissary` build is held to its platform's
    /// recomp feed when there is one; otherwise, and for any other engine, as our Dolphin.
    pub fn required(&self, engine: &str, platform: &str, version: &str) -> Option<&str> {
        if engine == common::client_engine::EMISSARY {
            if let Some(v) = self.emissary.get(platform) {
                return Some(v);
            }
        }
        if let Some(v) = self.by_platform.get(platform) {
            return Some(v);
        }
        let have = parse_version(version);
        self.by_platform
            .values()
            .filter(|v| parse_version(v) >= have)
            .min_by_key(|v| parse_version(v))
            .map(String::as_str)
    }
}

impl std::fmt::Display for FeedVersions {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.is_empty() {
            return f.write_str("none");
        }
        let parts: Vec<String> = (self.by_platform.iter().map(|(p, v)| format!("{p} {v}")))
            .chain(self.emissary.iter().map(|(p, v)| format!("emissary-{p} {v}")))
            .collect();
        f.write_str(&parts.join(", "))
    }
}

/// The top-level `version:` of an electron-builder feed (`version: 0.1.42`, maybe quoted).
pub fn feed_version(yml: &str) -> Option<String> {
    let line = yml.lines().find_map(|l| l.strip_prefix("version:"))?;
    let v = line.trim().trim_matches(['\'', '"']).trim();
    let valid = v.split(['-', '+']).next().is_some_and(|core| {
        let parts: Vec<&str> = core.split('.').collect();
        parts.len() == 3 && parts.iter().all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()))
    });
    valid.then(|| v.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_version_of_an_electron_builder_feed() {
        let yml = "version: 0.1.42\nfiles:\n  - url: Brawl-Online-Setup-0.1.42.exe\n    sha512: x\n    size: 1\n\
                   path: Brawl-Online-Setup-0.1.42.exe\nreleaseDate: '2026-10-10T20:00:00.000Z'\n";
        assert_eq!(feed_version(yml).as_deref(), Some("0.1.42"));
        assert_eq!(feed_version("version: '0.2.0'\n").as_deref(), Some("0.2.0"));
        assert_eq!(feed_version("files: []\n"), None);
        assert_eq!(feed_version("version: latest\n"), None);
        // Only the top-level key (an indented "version:" belongs to something else).
        assert_eq!(feed_version("files:\n  version: 9.9.9\n"), None);
    }

    #[test]
    fn each_platform_has_its_own_feed_and_others_the_oldest() {
        let v = FeedVersions::from_pairs(&[("win", "0.1.43"), ("mac", "0.1.42"), ("linux", "0.1.43")]);
        assert_eq!(v.required("dolphin", "win", "0.1.40"), Some("0.1.43"));
        assert_eq!(v.required("dolphin", "mac", "0.1.40"), Some("0.1.42"));
        assert_eq!(v.required("dolphin", "linux", "0.1.40"), Some("0.1.43"));
        // A build older than the platform field, or a platform without a feed: held to the oldest
        // feed it can be from.
        assert_eq!(v.required("dolphin", "", "0.1.42"), Some("0.1.42"));
        assert_eq!(v.required("dolphin", "bsd", "0.1.40"), Some("0.1.42"));
        assert_eq!(FeedVersions::default().required("dolphin", "win", "0.1.40"), None);
        // The macOS build of 0.1.40-0.1.42 waits for Apple: a 0.1.41 that names no platform is not
        // a macOS build, a 0.1.28 may be one.
        let v = FeedVersions::from_pairs(&[("win", "0.1.42"), ("mac", "0.1.28"), ("linux", "0.1.42")]);
        assert_eq!(v.required("dolphin", "", "0.1.41"), Some("0.1.42"));
        assert_eq!(v.required("dolphin", "", "0.1.28"), Some("0.1.28"));
        assert_eq!(v.required("dolphin", "", "0.1.20"), Some("0.1.28"));
        assert_eq!(v.required("dolphin", "", "0.2.0"), None, "newer than every feed");
        // Versions compare as numbers, not text.
        let v = FeedVersions::from_pairs(&[("win", "0.1.100"), ("mac", "0.1.99")]);
        assert_eq!(v.required("dolphin", "linux", "0.1.50"), Some("0.1.99"));
    }

    #[test]
    fn arm64_linux_is_held_to_its_own_feed() {
        // The arm64 Linux build is published on its own: while it lags, an arm64 build of the
        // version it names may still play, an x86_64 one may not.
        let v = FeedVersions::from_pairs(&[("win", "0.2.9"), ("linux", "0.2.9"), ("linux-arm64", "0.2.8")]);
        assert_eq!(v.required("dolphin", "linux-arm64", "0.2.8"), Some("0.2.8"));
        assert_eq!(v.required("dolphin", "linux", "0.2.8"), Some("0.2.9"));
        // Before its first build is published (no feed yet), as any platform without a feed.
        let v = FeedVersions::from_pairs(&[("win", "0.2.9"), ("mac", "0.2.8"), ("linux", "0.2.9")]);
        assert_eq!(v.required("dolphin", "linux-arm64", "0.2.9"), Some("0.2.9"));
        assert_eq!(v.required("dolphin", "linux-arm64", "0.2.10"), None);
    }

    #[test]
    fn a_recomp_is_held_as_a_dolphin_of_its_core_until_it_has_its_own_feed() {
        // No recomp feed: an EmissaryRecomp built from the 0.2.8 core is refused or let in exactly
        // as our Dolphin 0.2.8 of its platform, and so is an engine that does not say.
        let v = FeedVersions::from_pairs(&[("win", "0.2.9"), ("mac", "0.2.8"), ("linux", "0.2.9")]);
        for engine in ["emissary", "unknown", "dolphin"] {
            assert_eq!(v.required(engine, "win", "0.2.8"), Some("0.2.9"), "{engine}");
            assert_eq!(v.required(engine, "mac", "0.2.8"), Some("0.2.8"), "{engine}");
            assert_eq!(v.required(engine, "linux-arm64", "0.2.9"), Some("0.2.9"), "{engine}");
            assert_eq!(v.required(engine, "linux-arm64", "0.2.10"), None, "{engine}");
        }
        // With its own feed (the core release of the newest recomp build on that platform), a
        // recomp is held to that one, as the macOS build is while it trails; Dolphins are not.
        let v = v.with_emissary(&[("win", "0.2.7"), ("bsd", "9.9.9")]);
        assert_eq!(v.required("emissary", "win", "0.2.6"), Some("0.2.7"));
        assert_eq!(v.required("emissary", "win", "0.2.7"), Some("0.2.7"));
        assert_eq!(v.required("dolphin", "win", "0.2.7"), Some("0.2.9"));
        assert_eq!(v.required("emissary", "linux", "0.2.7"), Some("0.2.9"), "no linux recomp feed");
        // The recomp feeds never stand in for a Dolphin that names no platform.
        let only = FeedVersions::default().with_emissary(&[("win", "0.2.7")]);
        assert!(!only.is_empty());
        assert_eq!(only.required("dolphin", "", "0.2.6"), None);
        assert_eq!(only.to_string(), "emissary-win 0.2.7");
    }

    #[test]
    fn reads_the_feed_folder() {
        let dir = std::env::temp_dir().join(format!("mm-feed-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("latest.yml"), "version: 0.1.43\n").unwrap();
        std::fs::write(dir.join("latest-mac.yml"), "version: 0.1.42\n").unwrap();
        std::fs::write(dir.join("latest-emissary.yml"), "version: 0.1.41\n").unwrap();
        let v = FeedVersions::read(&dir);
        std::fs::remove_dir_all(&dir).unwrap();
        assert_eq!(
            v,
            FeedVersions::from_pairs(&[("win", "0.1.43"), ("mac", "0.1.42")]).with_emissary(&[("win", "0.1.41")])
        );
        assert_eq!(v.to_string(), "mac 0.1.42, win 0.1.43, emissary-win 0.1.41");
        assert!(FeedVersions::read(&dir).is_empty());
    }
}
