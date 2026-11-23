//! Badges and custom main icons (docs/design/profiles.md §10): PNG icons an admin uploads with
//! `pp-admin icon add`, given to players as badges before their name (`pp-admin badge add`) or as
//! the picture that takes the place of their primary main's stock icon (`pp-admin main-icon set`).
//! Only admins set them; the player has no say (their "Show next to name" switches don't apply).
//!
//! An icon is stored once, in the database (`icons`), under its [`icon_id`], and served by accounts
//! at `/v1/icons/<id>.png` ([`icon_path`]); the launcher builds that address from the id alone.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

/// The largest icon file taken: 64 KiB.
pub const MAX_ICON_BYTES: usize = 64 * 1024;
/// The widest and tallest icon taken, in pixels.
pub const MAX_ICON_SIDE: u32 = 256;
/// The most badges one player can have.
pub const MAX_BADGES: usize = 8;
/// The most characters in a badge's hover text.
pub const MAX_LABEL_CHARS: usize = 60;

/// Where a badge shows. A badge with no places listed shows in all of them.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Place {
    /// The profile: the launcher's View Profile dialog and the web page `/profile/<code>`.
    Profile,
    /// The small profile that hovering a name shows.
    Hover,
    /// The online list and the Friends tab.
    Online,
    /// The chat tab and the open chat window's title.
    Chat,
    /// The Ranked leaderboard.
    Leaderboard,
    /// The room lists: P+ rooms' players and Other Games rooms' hosts.
    Rooms,
}

impl Place {
    pub const ALL: [Place; 6] =
        [Place::Profile, Place::Hover, Place::Online, Place::Chat, Place::Leaderboard, Place::Rooms];

    pub fn as_str(self) -> &'static str {
        match self {
            Place::Profile => "profile",
            Place::Hover => "hover",
            Place::Online => "online",
            Place::Chat => "chat",
            Place::Leaderboard => "leaderboard",
            Place::Rooms => "rooms",
        }
    }

    pub fn parse(s: &str) -> Option<Place> {
        Place::ALL.into_iter().find(|p| p.as_str() == s)
    }
}

/// `all` (or nothing): every place, stored as NULL. Otherwise a comma-separated list of
/// [`Place`] names, each once, in [`Place::ALL`]'s order.
pub fn parse_places(s: &str) -> Result<Option<Vec<Place>>, String> {
    let s = s.trim();
    if s.is_empty() || s.eq_ignore_ascii_case("all") {
        return Ok(None);
    }
    let mut places = Vec::new();
    for part in s.split(',').map(str::trim).filter(|p| !p.is_empty()) {
        let p = Place::parse(&part.to_ascii_lowercase()).ok_or_else(|| {
            let all: Vec<&str> = Place::ALL.iter().map(|p| p.as_str()).collect();
            format!("unknown place {part:?} (all, or any of {})", all.join(", "))
        })?;
        places.push(p);
    }
    places.sort_by_key(|p| Place::ALL.iter().position(|q| q == p));
    places.dedup();
    Ok(if places.len() == Place::ALL.len() { None } else { Some(places) })
}

/// A badge as the launcher and the profile page get it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NameBadge {
    /// The icon's [`icon_id`].
    pub icon: String,
    /// The hover text; empty for none.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub label: String,
    /// Where it shows; absent: everywhere.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub places: Option<Vec<Place>>,
}

impl NameBadge {
    pub fn shows_in(&self, place: Place) -> bool {
        self.places.as_ref().is_none_or(|p| p.contains(&place))
    }
}

/// An icon's id: the first 20 hex digits of its SHA-256, so the same file is stored once and its
/// address changes when its content does (served as cacheable forever).
pub fn icon_id(png: &[u8]) -> String {
    hex::encode(Sha256::digest(png))[..20].to_string()
}

/// A well-formed [`icon_id`].
pub fn valid_icon_id(id: &str) -> bool {
    id.len() == 20 && id.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// Where accounts serves an icon.
pub fn icon_path(id: &str) -> String {
    format!("/v1/icons/{id}.png")
}

/// An icon's name, what `pp-admin` calls it (`crown`, `dev`): 1-32 of a-z, 0-9 and `-`.
pub fn valid_icon_name(name: &str) -> bool {
    (1..=32).contains(&name.len()) && name.bytes().all(|b| matches!(b, b'a'..=b'z' | b'0'..=b'9' | b'-'))
}

/// Checks an icon file: a PNG (animated ones too) of at most [`MAX_ICON_BYTES`] and
/// [`MAX_ICON_SIDE`] a side. Returns its width and height.
pub fn check_png(png: &[u8]) -> Result<(u32, u32), String> {
    const SIGNATURE: &[u8; 8] = b"\x89PNG\r\n\x1a\n";
    if png.len() > MAX_ICON_BYTES {
        return Err(format!("{} bytes; the most is {MAX_ICON_BYTES}", png.len()));
    }
    // Signature, then the IHDR chunk: length 13, "IHDR", width, height (big-endian).
    if png.len() < 33 || &png[..8] != SIGNATURE || png[8..12] != [0, 0, 0, 13] || &png[12..16] != b"IHDR" {
        return Err("not a PNG file".into());
    }
    let width = u32::from_be_bytes(png[16..20].try_into().unwrap());
    let height = u32::from_be_bytes(png[20..24].try_into().unwrap());
    if width == 0 || height == 0 {
        return Err("not a PNG file (no pixels)".into());
    }
    if width > MAX_ICON_SIDE || height > MAX_ICON_SIDE {
        return Err(format!("{width}x{height} pixels; the most is {MAX_ICON_SIDE}x{MAX_ICON_SIDE}"));
    }
    if !png.ends_with(b"IEND\xaeB`\x82") {
        return Err("not a whole PNG file (no end)".into());
    }
    Ok((width, height))
}

/// Checks a badge's hover text: at most [`MAX_LABEL_CHARS`] characters, no control characters.
pub fn check_label(label: &str) -> Result<String, String> {
    let label = label.trim();
    if label.chars().count() > MAX_LABEL_CHARS {
        return Err(format!("the hover text is longer than {MAX_LABEL_CHARS} characters"));
    }
    if label.chars().any(char::is_control) {
        return Err("the hover text has control characters".into());
    }
    Ok(label.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn png(w: u32, h: u32) -> Vec<u8> {
        let mut v = b"\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR".to_vec();
        v.extend(w.to_be_bytes());
        v.extend(h.to_be_bytes());
        v.extend([8, 6, 0, 0, 0, 0, 0, 0, 0]);
        v.extend(b"\0\0\0\0IEND\xaeB`\x82");
        v
    }

    #[test]
    fn pngs_are_checked() {
        assert_eq!(check_png(&png(64, 32)), Ok((64, 32)));
        assert_eq!(check_png(&png(256, 256)), Ok((256, 256)));
        assert!(check_png(&png(257, 16)).is_err());
        assert!(check_png(&png(0, 16)).is_err());
        assert!(check_png(b"GIF89a.................................").is_err());
        let mut cut = png(16, 16);
        cut.truncate(cut.len() - 4);
        assert!(check_png(&cut).is_err());
        let mut big = png(16, 16);
        big.resize(MAX_ICON_BYTES + 1, 0);
        assert!(check_png(&big).is_err());
    }

    #[test]
    fn places_parse() {
        assert_eq!(parse_places("all"), Ok(None));
        assert_eq!(parse_places(""), Ok(None));
        assert_eq!(parse_places("chat, Profile,chat"), Ok(Some(vec![Place::Profile, Place::Chat])));
        assert_eq!(parse_places("profile,hover,online,chat,leaderboard,rooms"), Ok(None));
        assert!(parse_places("spectate").is_err());
    }

    #[test]
    fn badges_show_where_listed() {
        let b = NameBadge { icon: "0".repeat(20), label: String::new(), places: None };
        assert!(Place::ALL.iter().all(|p| b.shows_in(*p)));
        let b = NameBadge { places: Some(vec![Place::Chat]), ..b };
        assert!(b.shows_in(Place::Chat) && !b.shows_in(Place::Profile));
        let json = serde_json::to_value(&b).unwrap();
        assert_eq!(json, serde_json::json!({"icon": "00000000000000000000", "places": ["chat"]}));
    }

    #[test]
    fn ids_and_names() {
        let id = icon_id(b"x");
        assert!(valid_icon_id(&id));
        assert_eq!(icon_path(&id), format!("/v1/icons/{id}.png"));
        assert!(!valid_icon_id("../etc/passwd0000000"));
        assert!(valid_icon_name("dev") && valid_icon_name("top-8-2026"));
        assert!(!valid_icon_name("Dev") && !valid_icon_name("") && !valid_icon_name(&"a".repeat(33)));
    }
}
