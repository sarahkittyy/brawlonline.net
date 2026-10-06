//! Connect codes and display names.
//!
//! Rules (docs/backend-design.md section 1.3):
//! - The user picks a 2-4 letter prefix (`/^[a-zA-Z]+$/`, Slippi launcher
//!   `validate.ts:26-44`). We store it uppercased.
//! - The server appends `#N`. `N` is the lowest free number from a random start
//!   in 1..=999, falling back to 1000..=9999 only when all 999 are taken.
//! - The whole code is at most 8 characters including `#`
//!   (`CONNECT_CODE_LENGTH 8` in Slippi's EXI device), so a 4-letter prefix
//!   cannot take a 4-digit number.
//! - Codes are unique and immutable once assigned.
//!
//! Codes typed in-game reach the mm server as Shift-JIS bytes, usually as
//! full-width characters (`ＡＢＣＤ＃１２３`). [`decode_search_code`] turns them
//! into the canonical ASCII form. The Shift-JIS decoding plus NFKC normalization
//! is taken from openmelee (`src/matchmaking.rs:37-46`, GPL-2.0).
//!
//! Display names follow the Slippi launcher rule (`validate.ts:46-61`):
//! 1-15 characters of printable ASCII, Hiragana or Katakana, without `\` or `` ` ``.

use std::fmt;

use encoding_rs::SHIFT_JIS;
use unicode_normalization::UnicodeNormalization;

/// Maximum connect-code length including `#` (Slippi `CONNECT_CODE_LENGTH`).
pub const MAX_CODE_LEN: usize = 8;
/// Maximum display-name length (Slippi `MAX_NAME_LENGTH`).
pub const MAX_NAME_LEN: usize = 15;
/// Prefix length bounds.
pub const MIN_PREFIX_LEN: usize = 2;
pub const MAX_PREFIX_LEN: usize = 4;
/// Size of the in-game code buffer in bytes (Shift-JIS, 2 bytes per character
/// plus slack). Anything longer than this is not a code.
pub const MAX_SEARCH_CODE_BYTES: usize = 18;

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum CodeError {
    #[error("Invalid code")]
    Invalid,
    #[error("Only English characters are allowed")]
    OnlyEnglish,
    #[error("Code is too short")]
    TooShort,
    #[error("Code is too long")]
    TooLong,
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum NameError {
    #[error("Display names can only contain letters, numbers, Hiragana, Katakana, and special characters")]
    InvalidCharacters,
    #[error("Display name is too short")]
    TooShort,
    #[error("Display name is too long")]
    TooLong,
}

/// A parsed connect code in canonical form: uppercase ASCII prefix, number
/// without leading zeros.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ConnectCode {
    prefix: String,
    number: u16,
}

impl ConnectCode {
    /// Builds a code from a validated prefix and a number, checking the length rule.
    pub fn new(prefix: &str, number: u16) -> Result<Self, CodeError> {
        let prefix = validate_code_start(prefix)?;
        if number == 0 || number > 9999 {
            return Err(CodeError::Invalid);
        }
        let code = ConnectCode { prefix, number };
        if code.len() > MAX_CODE_LEN {
            return Err(CodeError::TooLong);
        }
        Ok(code)
    }

    pub fn prefix(&self) -> &str {
        &self.prefix
    }

    pub fn number(&self) -> u16 {
        self.number
    }

    #[allow(clippy::len_without_is_empty)]
    pub fn len(&self) -> usize {
        self.prefix.len() + 1 + digits(self.number)
    }
}

impl fmt::Display for ConnectCode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}#{}", self.prefix, self.number)
    }
}

impl std::str::FromStr for ConnectCode {
    type Err = CodeError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        parse_connect_code(s)
    }
}

fn digits(n: u16) -> usize {
    match n {
        0..=9 => 1,
        10..=99 => 2,
        100..=999 => 3,
        _ => 4,
    }
}

/// Validates the user-chosen prefix ("code start") and returns it uppercased.
/// Same checks and messages as the Slippi launcher's `validateConnectCodeStart`.
pub fn validate_code_start(code_start: &str) -> Result<String, CodeError> {
    if code_start.is_empty() {
        return Err(CodeError::Invalid);
    }
    if !code_start.chars().all(|c| c.is_ascii_alphabetic()) {
        return Err(CodeError::OnlyEnglish);
    }
    if code_start.len() < MIN_PREFIX_LEN {
        return Err(CodeError::TooShort);
    }
    if code_start.len() > MAX_PREFIX_LEN {
        return Err(CodeError::TooLong);
    }
    Ok(code_start.to_ascii_uppercase())
}

/// The launcher's default prefix: the ASCII letters of the display name,
/// uppercased, first four (`activate_online_form.tsx:41-45`).
pub fn default_code_start(display_name: &str) -> String {
    display_name
        .chars()
        .filter(|c| c.is_ascii_alphabetic())
        .take(MAX_PREFIX_LEN)
        .collect::<String>()
        .to_ascii_uppercase()
}

/// Parses a code typed by a user, in any width or case. `abcd#007`,
/// `ＡＢＣＤ＃７` and `ABCD#7` all give `ABCD#7`.
pub fn parse_connect_code(input: &str) -> Result<ConnectCode, CodeError> {
    let normalized: String = input.nfkc().collect::<String>();
    let normalized = normalized.trim();
    let (prefix, number) = normalized.split_once('#').ok_or(CodeError::Invalid)?;
    if number.is_empty() || number.len() > 4 || !number.chars().all(|c| c.is_ascii_digit()) {
        return Err(CodeError::Invalid);
    }
    let number: u16 = number.parse().map_err(|_| CodeError::Invalid)?;
    ConnectCode::new(prefix, number).map_err(|e| match e {
        // A malformed prefix in a full code is just an invalid code.
        CodeError::OnlyEnglish => CodeError::Invalid,
        other => other,
    })
}

/// Decodes `search.connectCode` from a `create-ticket` message: the raw
/// Shift-JIS bytes of the in-game code buffer (the client strips trailing NULs,
/// we strip them again in case another client does not).
pub fn decode_search_code(bytes: &[u8]) -> Result<ConnectCode, CodeError> {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    let bytes = &bytes[..end];
    if bytes.is_empty() {
        return Err(CodeError::Invalid);
    }
    if bytes.len() > MAX_SEARCH_CODE_BYTES {
        return Err(CodeError::TooLong);
    }
    let (decoded, _, had_errors) = SHIFT_JIS.decode(bytes);
    if had_errors {
        return Err(CodeError::Invalid);
    }
    parse_connect_code(&decoded)
}

/// Encodes a code the way the game sends it: full-width Shift-JIS characters.
/// Used by tests and by `mmclient --fullwidth`.
pub fn encode_search_code_fullwidth(code: &str) -> Vec<u8> {
    let fullwidth: String = code
        .chars()
        .map(|c| match c {
            '!'..='~' => char::from_u32(c as u32 - 0x21 + 0xFF01).unwrap_or(c),
            other => other,
        })
        .collect();
    let (bytes, _, _) = SHIFT_JIS.encode(&fullwidth);
    bytes.into_owned()
}

fn is_name_char(c: char) -> bool {
    matches!(c, ' '..='~' | '\u{3041}'..='\u{3093}' | '\u{30A1}'..='\u{30F3}') && c != '\\' && c != '`'
}

/// Validates a display name with the launcher's rule. Also rejects names that
/// are only whitespace (the launcher accepts them, but they render as nothing).
pub fn validate_display_name(name: &str) -> Result<(), NameError> {
    let len = name.chars().count();
    if len == 0 || name.trim().is_empty() {
        return Err(NameError::TooShort);
    }
    if !name.chars().all(is_name_char) {
        return Err(NameError::InvalidCharacters);
    }
    if len > MAX_NAME_LEN {
        return Err(NameError::TooLong);
    }
    Ok(())
}

/// Order in which to try code numbers for a prefix: a random start in 1..=999,
/// wrapping, then (only if the prefix leaves room) 1000..=9999 in order.
pub fn candidate_numbers(prefix_len: usize, start: u16) -> impl Iterator<Item = u16> {
    let start = start.clamp(1, 999);
    let three = (start..=999).chain(1..start);
    let four_allowed = prefix_len + 1 + 4 <= MAX_CODE_LEN;
    let four = (1000..=9999u16).filter(move |_| four_allowed);
    three.chain(four)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn code_start_rules_match_launcher() {
        assert_eq!(validate_code_start("abcd"), Ok("ABCD".into()));
        assert_eq!(validate_code_start("Ab"), Ok("AB".into()));
        assert_eq!(validate_code_start(""), Err(CodeError::Invalid));
        assert_eq!(validate_code_start("a"), Err(CodeError::TooShort));
        assert_eq!(validate_code_start("abcde"), Err(CodeError::TooLong));
        assert_eq!(validate_code_start("ab1"), Err(CodeError::OnlyEnglish));
        assert_eq!(validate_code_start("ab#"), Err(CodeError::OnlyEnglish));
        assert_eq!(validate_code_start("ＡＢ"), Err(CodeError::OnlyEnglish));
        assert_eq!(validate_code_start("リッピー"), Err(CodeError::OnlyEnglish));
    }

    #[test]
    fn default_prefix_from_display_name() {
        assert_eq!(default_code_start("sarah"), "SARA");
        assert_eq!(default_code_start("M2K 2024"), "MK");
        assert_eq!(default_code_start("x"), "X");
        assert_eq!(default_code_start("ひらがな"), "");
    }

    #[test]
    fn parse_canonicalizes() {
        assert_eq!(parse_connect_code("ABCD#123").unwrap().to_string(), "ABCD#123");
        assert_eq!(parse_connect_code("abcd#123").unwrap().to_string(), "ABCD#123");
        assert_eq!(parse_connect_code(" ab#007 ").unwrap().to_string(), "AB#7");
        assert_eq!(parse_connect_code("ＡＢＣＤ＃１２３").unwrap().to_string(), "ABCD#123");
        assert_eq!(parse_connect_code("ABC#1000").unwrap().to_string(), "ABC#1000");
    }

    #[test]
    fn parse_rejects_bad_codes() {
        for bad in [
            "",
            "#",
            "ABCD",
            "ABCD#",
            "#123",
            "A#1",
            "ABCDE#1",
            "ABCD#1000",
            "AB#12345",
            "AB#0",
            "AB#000",
            "AB#1A",
            "A1#12",
            "AB##1",
            "AB#-1",
            "AB#+1",
            "&-.%#123",
            "リッピー#0",
            "AB#１２３４５",
        ] {
            assert!(parse_connect_code(bad).is_err(), "{bad:?} should be rejected");
        }
    }

    #[test]
    fn length_rule() {
        assert!(ConnectCode::new("ABCD", 999).is_ok());
        assert_eq!(ConnectCode::new("ABCD", 1000), Err(CodeError::TooLong));
        assert!(ConnectCode::new("ABC", 9999).is_ok());
        assert!(ConnectCode::new("AB", 9999).is_ok());
        assert_eq!(ConnectCode::new("AB", 10000), Err(CodeError::Invalid));
        assert_eq!(ConnectCode::new("AB", 0), Err(CodeError::Invalid));
    }

    #[test]
    fn decode_shift_jis_fullwidth() {
        // openmelee fixture: "ＴＥＳＴ＃００２" in Shift-JIS.
        let bytes = [130, 115, 130, 100, 130, 114, 130, 115, 129, 148, 130, 79, 130, 79, 130, 81];
        assert_eq!(decode_search_code(&bytes).unwrap().to_string(), "TEST#2");
        // ASCII is valid Shift-JIS too, and trailing NULs from the game buffer are ignored.
        let mut ascii = b"ABCD#123".to_vec();
        ascii.extend([0, 0, 0]);
        assert_eq!(decode_search_code(&ascii).unwrap().to_string(), "ABCD#123");
        // Round trip through our own encoder.
        let enc = encode_search_code_fullwidth("SARA#42");
        assert_eq!(enc.len(), 14);
        assert_eq!(decode_search_code(&enc).unwrap().to_string(), "SARA#42");
    }

    #[test]
    fn decode_rejects_garbage() {
        assert!(decode_search_code(&[]).is_err());
        assert!(decode_search_code(&[0, 65, 66]).is_err());
        assert!(decode_search_code(&[0x81]).is_err()); // truncated double-byte char
        assert!(decode_search_code(&[0xFF; 18]).is_err());
        assert_eq!(decode_search_code(&[b'A'; 40]), Err(CodeError::TooLong));
    }

    #[test]
    fn display_names() {
        assert!(validate_display_name("sarah").is_ok());
        assert!(validate_display_name("Mr. Game & Watch").is_err()); // 16 chars
        assert!(validate_display_name("Mr. Game&Watch").is_ok());
        assert!(validate_display_name("site/user").is_ok());
        assert!(validate_display_name("ひらがなカタカナ").is_ok());
        assert_eq!(validate_display_name(""), Err(NameError::TooShort));
        assert_eq!(validate_display_name("   "), Err(NameError::TooShort));
        assert_eq!(validate_display_name("a\\b"), Err(NameError::InvalidCharacters));
        assert_eq!(validate_display_name("a`b"), Err(NameError::InvalidCharacters));
        assert_eq!(validate_display_name("tab\there"), Err(NameError::InvalidCharacters));
        assert_eq!(validate_display_name("é"), Err(NameError::InvalidCharacters));
        assert_eq!(validate_display_name("漢字"), Err(NameError::InvalidCharacters));
        assert_eq!(validate_display_name("0123456789abcdef"), Err(NameError::TooLong));
    }

    #[test]
    fn candidates_wrap_and_respect_length() {
        let four: Vec<u16> = candidate_numbers(4, 998).collect();
        assert_eq!(&four[..3], &[998, 999, 1]);
        assert_eq!(four.len(), 999);
        assert!(four.iter().all(|&n| (1..=999).contains(&n)));
        let three: Vec<u16> = candidate_numbers(3, 1).collect();
        assert_eq!(three.len(), 999 + 9000);
        assert_eq!(three[999], 1000);
        let mut sorted = three.clone();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(sorted.len(), three.len());
    }
}
