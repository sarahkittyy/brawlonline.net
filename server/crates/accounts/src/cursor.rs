//! Opaque keyset-pagination cursors: base64url (no padding) of `<kind>:<field>:<field>...`.
//! Clients pass them back unchanged; the kind keeps a leaderboard cursor out of the match
//! history and the other way round.

use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine;

/// Longest cursor accepted (the real ones are well under 100 characters).
const MAX_LEN: usize = 256;

pub fn encode(kind: &str, fields: &[&str]) -> String {
    let mut s = kind.to_string();
    for f in fields {
        s.push(':');
        s.push_str(f);
    }
    URL_SAFE_NO_PAD.encode(s)
}

/// The `n` fields of a cursor of this kind (the last one may contain `:`), or `None` for
/// anything else.
pub fn decode(kind: &str, cursor: &str, n: usize) -> Option<Vec<String>> {
    if cursor.len() > MAX_LEN {
        return None;
    }
    let raw = String::from_utf8(URL_SAFE_NO_PAD.decode(cursor.trim()).ok()?).ok()?;
    let rest = raw.strip_prefix(kind)?.strip_prefix(':')?;
    let fields: Vec<String> = rest.splitn(n, ':').map(str::to_string).collect();
    (fields.len() == n && fields.iter().all(|f| !f.is_empty())).then_some(fields)
}

/// A page size from the query: `default` when absent, at least 1 and at most `max`.
pub fn page_limit(limit: Option<u32>, default: u32, max: u32) -> u32 {
    limit.unwrap_or(default).clamp(1, max)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn page_limits() {
        assert_eq!(page_limit(None, 50, 100), 50);
        assert_eq!(page_limit(Some(0), 50, 100), 1);
        assert_eq!(page_limit(Some(7), 50, 100), 7);
        assert_eq!(page_limit(Some(1000), 50, 100), 100);
    }

    #[test]
    fn round_trip_and_rejects() {
        let c = encode("h1", &["1700000000000000", "mode.ranked-1:2"]);
        assert!(c.chars().all(|ch| ch.is_ascii_alphanumeric() || ch == '-' || ch == '_'), "{c}");
        assert_eq!(decode("h1", &c, 2).unwrap(), vec!["1700000000000000", "mode.ranked-1:2"]);
        // Another kind, the wrong field count, garbage, empty fields, too long.
        assert!(decode("l1", &c, 2).is_none());
        assert!(decode("h1", &encode("h1", &["x"]), 2).is_none());
        assert!(decode("h1", &encode("h1", &["", "y"]), 2).is_none());
        assert!(decode("h1", "!!!", 2).is_none());
        assert!(decode("h1", "", 2).is_none());
        assert!(decode("h1", &"A".repeat(300), 2).is_none());
    }
}
