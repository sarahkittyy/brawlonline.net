//! Which engine runs a player's game (docs/design/brawl-online-core.md §10, "Which engine"): our
//! Dolphin or EmissaryRecomp, both running the shared online core (`BrawlOnline::Engine`). The
//! game says it as `engine` and `engineVersion` in mm's `hello` and tickets and in its game and
//! leave reports; mm logs it and holds a recomp to its own update feed (mm `versions`), and the
//! speed and report tables keep it so their numbers can be split by engine (`pp-admin speed`).
//!
//! Builds before the field leave it out: every one of them is our Dolphin.

/// Our Dolphin.
pub const DOLPHIN: &str = "dolphin";
/// EmissaryRecomp (the core in-process in the recompiled game).
pub const EMISSARY: &str = "emissary";
/// An engine that does not say (the core's default) or names something malformed.
pub const UNKNOWN: &str = "unknown";

pub const MAX_NAME_BYTES: usize = 32;
pub const MAX_VERSION_BYTES: usize = 64;

/// The engine a game names, as it is stored and compared: absent or blank is Dolphin (builds
/// before the field); otherwise lowercased, and only `[a-z0-9._-]`, at most [`MAX_NAME_BYTES`]
/// (anything else is [`UNKNOWN`]). Peer-controlled: never trusted beyond that.
pub fn name(raw: &str) -> String {
    let s = raw.trim().to_ascii_lowercase();
    if s.is_empty() {
        return DOLPHIN.into();
    }
    let ok = s.len() <= MAX_NAME_BYTES
        && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"._-".contains(&b));
    if ok {
        s
    } else {
        UNKNOWN.into()
    }
}

/// An engine's version as stored: printable ASCII only (anything else dropped), trimmed, at most
/// [`MAX_VERSION_BYTES`].
pub fn version(raw: &str) -> String {
    let s: String = raw.chars().filter(|c| c.is_ascii_graphic() || *c == ' ').take(MAX_VERSION_BYTES).collect();
    s.trim().to_string()
}

/// `name version`, or just the name when there is no version (for log lines).
pub fn label(name: &str, version: &str) -> String {
    if version.is_empty() {
        name.to_string()
    } else {
        format!("{name} {version}")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn absent_is_dolphin_and_malformed_is_unknown() {
        assert_eq!(name(""), DOLPHIN);
        assert_eq!(name("  "), DOLPHIN);
        assert_eq!(name("dolphin"), DOLPHIN);
        assert_eq!(name("Emissary"), EMISSARY);
        assert_eq!(name(" emissary "), EMISSARY);
        assert_eq!(name("unknown"), UNKNOWN);
        assert_eq!(name("some-engine_2.0"), "some-engine_2.0");
        assert_eq!(name("drop table;"), UNKNOWN);
        assert_eq!(name("émissary"), UNKNOWN);
        assert_eq!(name(&"x".repeat(MAX_NAME_BYTES)), "x".repeat(MAX_NAME_BYTES));
        assert_eq!(name(&"x".repeat(MAX_NAME_BYTES + 1)), UNKNOWN);
    }

    #[test]
    fn versions_are_printable_and_short() {
        assert_eq!(version("v0.2.5"), "v0.2.5");
        assert_eq!(version(" 0.7.0+771c27b "), "0.7.0+771c27b");
        assert_eq!(version("0.7\n.0\u{1b}[31m"), "0.7.0[31m");
        assert_eq!(version("é0.7"), "0.7");
        assert_eq!(version(&"9".repeat(200)).len(), MAX_VERSION_BYTES);
        assert_eq!(label("dolphin", ""), "dolphin");
        assert_eq!(label("emissary", "0.7.0"), "emissary 0.7.0");
    }
}
