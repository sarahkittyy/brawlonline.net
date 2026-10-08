//! The errors the server sends with a refused or ended ticket.
//!
//! The game shows them on one line in the character select's header, in its own font
//! (`docs/game-code.md` §6, "CSS status line"). That line fits about 640 font units at the
//! normal size, about 35 average characters or 21 of the widest ("W"); longer texts are
//! wrapped onto two smaller lines, and texts too long for those are cut. So every message
//! here is a short sentence that fits one line, measured in the game with the widest
//! connect code ("WWWW#999") where one is part of it. Keep new ones as short.

/// Too many connections from one source address.
pub const TOO_MANY_CONNECTIONS: &str = "Too many connections. Try later.";
/// A packet that is not a ticket the server can read.
pub const INVALID_REQUEST: &str = "Invalid matchmaking request";
/// A message type the server does not know (an old or newer client).
pub const UNKNOWN_REQUEST: &str = "Your game needs an update.";
/// A mode number outside Slippi's list.
pub const UNKNOWN_MODE: &str = "Unknown game mode";
/// The ticket has no usable uid or play key.
pub const NOT_LOGGED_IN: &str = "Log in again in the launcher.";
/// The searched-for code does not parse.
pub const INVALID_CODE: &str = "Invalid connect code";
/// The game reached the server over IPv6 (peers cannot use the address).
pub const NEEDS_IPV4: &str = "Online play needs IPv4.";
/// The source address sent too many tickets.
pub const TOO_MANY_SEARCHES: &str = "Too many searches. Wait a moment.";
/// The uid is not an account.
pub const ACCOUNT_NOT_FOUND: &str = "Account not found. Log in again.";
/// The database could not be asked (or answered too late).
pub const UNAVAILABLE: &str = "Matchmaking unavailable. Try later.";
/// The play key does not match the account (rotated: password change, ban, admin).
pub const LOGIN_EXPIRED: &str = "Login expired. Log in again.";
/// The account sent tickets too fast.
pub const SEARCHING_TOO_OFTEN: &str = "Searching too often. Wait a moment.";
/// The account is banned.
pub const BANNED: &str = "This account is banned.";
/// The account has no connect code yet.
pub const NO_CONNECT_CODE: &str = "Pick a connect code in the launcher.";
/// A Direct search for one's own code.
pub const OWN_CODE: &str = "That is your own connect code.";
/// The same account started a newer search.
pub const REPLACED: &str = "Replaced by a newer search.";
/// A connection that never sent a ticket.
pub const NO_REQUEST: &str = "No search received. Try again.";
/// An Unranked ticket that found nobody within its time.
pub const NO_OPPONENT: &str = "No opponent found. Try again.";

/// A mode that is not built yet (Ranked, Teams, Party).
pub fn not_available(mode: &str) -> String {
    format!("{mode} isn't available yet.")
}

/// The game is older than the minimum version.
pub fn update_to(latest: &str) -> String {
    format!("Update to {latest} to play online.")
}

/// A Direct ticket whose opponent did not search within its time.
pub fn did_not_connect(code: &str) -> String {
    format!("{code} didn't connect in time.")
}

/// Two players matched again and again whose P2P connection kept failing.
pub fn cannot_connect(code: &str) -> String {
    format!("Couldn't connect to {code}.")
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A rough upper bound of the game's widths (font units at the normal size): every letter
    /// counted as wide as the widest the messages use, so a message that passes here fits.
    fn rough_width(s: &str) -> usize {
        s.chars()
            .map(|c| match c {
                'W' | 'M' | 'm' | 'w' => 32,
                'i' | 'l' | 'I' | '.' | ',' | '\'' | ' ' | '!' | ':' => 10,
                _ => 19,
            })
            .sum()
    }

    #[test]
    fn every_message_fits_the_status_line() {
        let all = [
            TOO_MANY_CONNECTIONS.to_string(),
            INVALID_REQUEST.to_string(),
            UNKNOWN_REQUEST.to_string(),
            UNKNOWN_MODE.to_string(),
            NOT_LOGGED_IN.to_string(),
            INVALID_CODE.to_string(),
            NEEDS_IPV4.to_string(),
            TOO_MANY_SEARCHES.to_string(),
            ACCOUNT_NOT_FOUND.to_string(),
            UNAVAILABLE.to_string(),
            LOGIN_EXPIRED.to_string(),
            SEARCHING_TOO_OFTEN.to_string(),
            BANNED.to_string(),
            NO_CONNECT_CODE.to_string(),
            OWN_CODE.to_string(),
            REPLACED.to_string(),
            NO_REQUEST.to_string(),
            NO_OPPONENT.to_string(),
            not_available("Ranked"),
            not_available("Teams"),
            not_available("Party"),
            update_to("10.10.10"),
            did_not_connect("WWWW#999"),
            cannot_connect("WWWW#999"),
        ];
        for m in all {
            assert!(m.chars().count() <= 36, "{m:?} is {} characters", m.chars().count());
            assert!(rough_width(&m) <= 640, "{m:?} is about {} font units", rough_width(&m));
        }
    }
}
