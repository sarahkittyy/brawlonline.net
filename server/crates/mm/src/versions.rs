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
//!
//! Where no such file is published, production's mm reads the recomp's latest GitHub release
//! ([`EMISSARY_RELEASE_URL`], what launchers install): its `manifest.json`'s `brawlOnline` is
//! that feed for each platform the release has a download for ([`emissary_release_platforms`]).
//! So a launcher release does not make an unchanged recomp out of date: a recomp is held to the
//! newest recomp release, which needs building again only when its core changes.

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

/// EmissaryRecomp's latest release as GitHub's API describes it: the one launchers install
/// (`launcher/src/dolphin/emissary`, `EMISSARY_REPO`).
pub const EMISSARY_RELEASE_URL: &str = "https://api.github.com/repos/kalomaze/EmissaryRecomp/releases/latest";

/// A recomp release's downloads per platform, as the launcher picks them (`assetPattern`). None
/// for `linux-arm64`: its launcher would take the x86_64 download, which its core cannot be.
const EMISSARY_ASSETS: [(&str, &str); 3] =
    [("win", "-Windows.zip"), ("linux", "-Linux.tar.gz"), ("mac", "-macOS.tar.gz")];

/// From GitHub's description of a release (`/releases/latest`): its `manifest.json`'s download
/// address, and the platforms it has a download for. None without a manifest (EmissaryRecomp
/// v0.6.0, from before the core: nothing to hold a recomp to).
pub fn emissary_release_platforms(release: &serde_json::Value) -> Option<(String, Vec<&'static str>)> {
    let assets = release.get("assets")?.as_array()?;
    let names = || assets.iter().filter_map(|a| Some((a.get("name")?.as_str()?, a)));
    let manifest = names()
        .find(|(n, _)| *n == "manifest.json")
        .and_then(|(_, a)| a.get("browser_download_url")?.as_str())
        .filter(|u| u.starts_with("https://"))?;
    let platforms = EMISSARY_ASSETS
        .iter()
        .filter(|(_, suffix)| names().any(|(n, _)| n.ends_with(suffix)))
        .map(|(p, _)| *p)
        .collect();
    Some((manifest.to_string(), platforms))
}

/// A recomp release's `manifest.json` `brawlOnline`: the Brawl Online release of its core (what
/// its builds send as `appVersion`), when it is a version.
pub fn emissary_manifest_version(manifest: &serde_json::Value) -> Option<String> {
    let v = manifest.get("brawlOnline")?.as_str()?.trim();
    let parts: Vec<&str> = v.split('.').collect();
    let valid = parts.len() == 3
        && parts.iter().all(|p| !p.is_empty() && p.len() <= 9 && p.bytes().all(|b| b.is_ascii_digit()));
    valid.then(|| v.to_string())
}

/// A recomp release's own version (`manifest.json`'s `version`, its tag: `v0.7.1`), told to a
/// running recomp older than it (`emissary-update`, the game's "Update Available"). None when it
/// is not `v` and three numbers.
pub fn emissary_release_tag(manifest: &serde_json::Value) -> Option<String> {
    let v = manifest.get("version")?.as_str()?.trim();
    let parts: Vec<&str> = v.strip_prefix('v')?.split('.').collect();
    let valid = parts.len() == 3
        && parts.iter().all(|p| !p.is_empty() && p.len() <= 9 && p.bytes().all(|b| b.is_ascii_digit()));
    valid.then(|| v.to_string())
}

/// What mm read of the recomp's latest release.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct EmissaryRelease {
    /// Its core's Brawl Online release per platform with a download ([`emissary_manifest_version`]).
    pub platforms: Vec<(&'static str, String)>,
    /// Its own version ([`emissary_release_tag`]); None from a manifest without one.
    pub tag: Option<String>,
}

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

    /// With the recomp release's versions ([`emissary_release_platforms`]) for the platforms that
    /// have no recomp feed file: a published `latest-emissary*.yml` comes first.
    pub fn with_emissary_release(mut self, pairs: &[(&'static str, String)]) -> Self {
        for (platform, version) in pairs {
            self.emissary.entry(platform).or_insert_with(|| version.clone());
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
    fn a_recomp_release_is_its_platforms_feed() {
        let release = serde_json::json!({
            "tag_name": "v0.7.0",
            "assets": [
                {"name": "EmissaryRecomp-v0.7.0-Windows.zip", "browser_download_url": "https://github.com/k/E/releases/download/v0.7.0/EmissaryRecomp-v0.7.0-Windows.zip"},
                {"name": "EmissaryRecomp-v0.7.0-Linux.tar.gz", "browser_download_url": "https://github.com/k/E/releases/download/v0.7.0/EmissaryRecomp-v0.7.0-Linux.tar.gz"},
                {"name": "manifest.json", "browser_download_url": "https://github.com/k/E/releases/download/v0.7.0/manifest.json"},
                {"name": "SHA256SUMS", "browser_download_url": "https://github.com/k/E/releases/download/v0.7.0/SHA256SUMS"},
            ],
        });
        let (manifest, platforms) = emissary_release_platforms(&release).unwrap();
        assert_eq!(manifest, "https://github.com/k/E/releases/download/v0.7.0/manifest.json");
        assert_eq!(platforms, ["win", "linux"]);
        // v0.6.0 (no core, no manifest) holds no one.
        let old = serde_json::json!({"tag_name": "v0.6.0", "assets": [{"name": "EmissaryRecomp-v0.6.0-Windows.zip", "browser_download_url": "https://x/a.zip"}]});
        assert_eq!(emissary_release_platforms(&old), None);
        assert_eq!(emissary_release_platforms(&serde_json::json!({"message": "Not Found"})), None);

        let manifest = serde_json::json!({"version": "v0.7.0", "brawlOnline": "0.2.9", "protocol": 1});
        assert_eq!(emissary_manifest_version(&manifest).as_deref(), Some("0.2.9"));
        assert_eq!(emissary_release_tag(&serde_json::json!({"version": " v0.7.1 "})).as_deref(), Some("v0.7.1"));
        for bad in ["0.7.1", "v0.7", "v0.7.1-rc.1", "v0.7.x", ""] {
            assert_eq!(emissary_release_tag(&serde_json::json!({"version": bad})), None, "{bad}");
        }
        for bad in [
            serde_json::json!({"brawlOnline": "0.2"}),
            serde_json::json!({"brawlOnline": "latest"}),
            serde_json::json!({}),
        ] {
            assert_eq!(emissary_manifest_version(&bad), None);
        }

        // A launcher release (Dolphin feed 0.2.12) leaves a recomp of the release's core playing;
        // an older recomp is told to update to it; a published recomp feed file comes first.
        let pairs = [("win", "0.2.9".to_string()), ("linux", "0.2.9".to_string())];
        let v = FeedVersions::from_pairs(&[("win", "0.2.12"), ("mac", "0.2.12"), ("linux", "0.2.12")])
            .with_emissary(&[("linux", "0.2.10")])
            .with_emissary_release(&pairs);
        assert_eq!(v.required("emissary", "win", "0.2.9"), Some("0.2.9"));
        assert_eq!(v.required("emissary", "win", "0.2.8"), Some("0.2.9"));
        assert_eq!(v.required("emissary", "linux", "0.2.9"), Some("0.2.10"), "the file's");
        assert_eq!(v.required("emissary", "mac", "0.2.9"), Some("0.2.12"), "no macOS download: the Dolphin feed");
        assert_eq!(v.required("dolphin", "win", "0.2.9"), Some("0.2.12"));
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
