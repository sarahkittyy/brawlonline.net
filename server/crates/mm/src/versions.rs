//! The newest client build of each platform, read from the launcher's update feed: the folder
//! `pp-release client` publishes electron-builder's `latest.yml` (Windows), `latest-mac.yml` and
//! `latest-linux.yml` to (`/var/www/brawlonline/updates/launcher` in production). A game older
//! than its platform's newest build may not play online (`MM_UPDATE_FEED_DIR`): each platform
//! goes live on its own (the macOS build waits for Apple), so each is held to its own feed.

use std::collections::BTreeMap;
use std::path::Path;

use crate::engine::parse_version;

/// The platforms a build reports (`platform` in `hello` and tickets) and their feed files.
pub const FEEDS: [(&str, &str); 3] = [("win", "latest.yml"), ("mac", "latest-mac.yml"), ("linux", "latest-linux.yml")];

/// The version each platform's feed names.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FeedVersions {
    by_platform: BTreeMap<&'static str, String>,
}

impl FeedVersions {
    /// Reads the feeds in `dir`. A missing or unreadable feed is left out.
    pub fn read(dir: &Path) -> Self {
        let mut by_platform = BTreeMap::new();
        for (platform, file) in FEEDS {
            if let Some(v) = std::fs::read_to_string(dir.join(file)).ok().as_deref().and_then(feed_version) {
                by_platform.insert(platform, v);
            }
        }
        FeedVersions { by_platform }
    }

    pub fn from_pairs(pairs: &[(&str, &str)]) -> Self {
        let mut by_platform = BTreeMap::new();
        for (platform, version) in pairs {
            if let Some((p, _)) = FEEDS.iter().find(|(p, _)| p == platform) {
                by_platform.insert(*p, version.to_string());
            }
        }
        FeedVersions { by_platform }
    }

    pub fn is_empty(&self) -> bool {
        self.by_platform.is_empty()
    }

    /// The version a build for `platform` must have: its own feed's. For a build that names no
    /// platform (older than the field), an unknown one or one without a feed: the oldest any feed
    /// names, so no platform's current build is ever refused.
    pub fn required(&self, platform: &str) -> Option<&str> {
        if let Some(v) = self.by_platform.get(platform) {
            return Some(v);
        }
        self.by_platform.values().min_by_key(|v| parse_version(v)).map(String::as_str)
    }
}

impl std::fmt::Display for FeedVersions {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.by_platform.is_empty() {
            return f.write_str("none");
        }
        let parts: Vec<String> = self.by_platform.iter().map(|(p, v)| format!("{p} {v}")).collect();
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
        assert_eq!(v.required("win"), Some("0.1.43"));
        assert_eq!(v.required("mac"), Some("0.1.42"));
        assert_eq!(v.required("linux"), Some("0.1.43"));
        // A build older than the platform field, or a platform without a feed.
        assert_eq!(v.required(""), Some("0.1.42"));
        assert_eq!(v.required("bsd"), Some("0.1.42"));
        assert_eq!(FeedVersions::default().required("win"), None);
        // Versions compare as numbers, not text.
        let v = FeedVersions::from_pairs(&[("win", "0.1.100"), ("mac", "0.1.99")]);
        assert_eq!(v.required("linux"), Some("0.1.99"));
    }

    #[test]
    fn reads_the_feed_folder() {
        let dir = std::env::temp_dir().join(format!("mm-feed-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("latest.yml"), "version: 0.1.43\n").unwrap();
        std::fs::write(dir.join("latest-mac.yml"), "version: 0.1.42\n").unwrap();
        let v = FeedVersions::read(&dir);
        std::fs::remove_dir_all(&dir).unwrap();
        assert_eq!(v, FeedVersions::from_pairs(&[("win", "0.1.43"), ("mac", "0.1.42")]));
        assert_eq!(v.to_string(), "mac 0.1.42, win 0.1.43");
        assert!(FeedVersions::read(&dir).is_empty());
    }
}
