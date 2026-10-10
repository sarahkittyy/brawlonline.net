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

/// Rooms (`docs/rooms-protocol.md`): a code that is no live room, or not a room code at all.
pub const ROOM_NOT_FOUND: &str = "Room not found.";
/// Rooms: every open slot is taken.
pub const ROOM_FULL: &str = "This room is full.";
/// Rooms: the host closed this player's slot (and the room refuses them from then on).
pub const REMOVED: &str = "Removed from the room.";
/// Rooms: a host-only request from someone else.
pub const HOST_ONLY: &str = "Only the host can do that.";
/// Rooms: slots and the Teams switch change between games only.
pub const BETWEEN_GAMES: &str = "Wait for the game to end.";
/// Rooms: the host tried to close their own slot.
pub const OWN_SLOT: &str = "You can't close your own slot.";
/// Rooms: closing this slot would leave fewer than two open.
pub const MIN_OPEN_SLOTS: &str = "At least 2 slots stay open.";
/// Rooms: a room request from a player who is in no room.
pub const NOT_IN_ROOM: &str = "You are not in a room.";
/// Rooms: a room game ticket from a player who is not in that room.
pub const NOT_A_MEMBER: &str = "You are not in this room.";
/// Rooms: a room game ticket for a room that is not starting a game.
pub const ROOM_NOT_STARTING: &str = "The room is not starting a game.";
/// Rooms: the game is starting, so the request has to wait.
pub const ROOM_STARTING: &str = "The game is starting.";
/// Rooms: the account created rooms too fast.
pub const ROOMS_TOO_OFTEN: &str = "Too many rooms. Wait a moment.";
/// Rooms: the account tried codes too fast.
pub const JOINS_TOO_OFTEN: &str = "Too many tries. Wait a moment.";
/// Rooms: one connection sent room requests too fast.
pub const TOO_MANY_REQUESTS: &str = "Too many requests. Wait a moment.";
/// Rooms: the server holds as many rooms as it allows.
pub const NO_ROOMS_LEFT: &str = "No rooms free. Try later.";
/// Rooms: a member left while the room's game was starting.
pub const PLAYER_LEFT: &str = "A player left the room.";
/// The same account opened a newer online connection (another game).
pub const SIGNED_IN_ELSEWHERE: &str = "Signed in from another game.";

/// Room status line: only the host is in the room. The code is already in the top window, so the
/// line stays short enough for the status line's large one-line text next to the room's buttons.
pub const ROOM_WAITING: &str = "Waiting for players";

/// Room status line: open slots are still empty.
pub fn waiting_for_players(players: usize, open: usize) -> String {
    format!("Waiting for players ({players}/{open})")
}

/// Room status line: everyone is there, these players are not ready. When the names do not fit
/// the line, as many as fit and "+N" for the rest (display names can be 15 wide characters).
pub fn waiting_on(names: &[&str]) -> String {
    let fits = |s: &str| s.chars().count() <= 36 && rough_width(s) <= 640;
    for shown in (1..=names.len()).rev() {
        let mut s = format!("Waiting on: {}", names[..shown].join(", "));
        if shown < names.len() {
            s += &format!(" +{}", names.len() - shown);
        }
        if fits(&s) {
            return s;
        }
    }
    // Not even one name fits: cut it.
    let rest = if names.len() > 1 { format!(" +{}", names.len() - 1) } else { String::new() };
    let mut cut = String::from("Waiting on: ");
    for c in names.first().copied().unwrap_or("").chars() {
        if !fits(&format!("{cut}{c}...{rest}")) {
            break;
        }
        cut.push(c);
    }
    format!("{cut}...{rest}")
}

/// A rough upper bound of the game's text widths (font units at the normal size): every letter
/// counted as wide as the widest the messages use, so a message within 640 fits the line.
pub(crate) fn rough_width(s: &str) -> usize {
    s.chars()
        .map(|c| match c {
            'W' | 'M' | 'm' | 'w' => 32,
            'i' | 'l' | 'I' | '.' | ',' | '\'' | ' ' | '!' | ':' => 10,
            _ => 19,
        })
        .sum()
}

/// Room status line: Teams on, everyone on one colour.
pub const PICK_TEAMS: &str = "Pick different teams";
/// Room status line: the room plays; this player joined during the game or came back early.
pub const WAITING_FOR_GAME: &str = "Waiting for the game to end";
/// Room status line: this player is in the room's game.
pub const IN_GAME: &str = "In game";
/// Room status line: the members' tickets are coming in.
pub const STARTING_GAME: &str = "Starting the game";

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
            ROOM_NOT_FOUND.to_string(),
            ROOM_FULL.to_string(),
            REMOVED.to_string(),
            HOST_ONLY.to_string(),
            BETWEEN_GAMES.to_string(),
            OWN_SLOT.to_string(),
            MIN_OPEN_SLOTS.to_string(),
            NOT_IN_ROOM.to_string(),
            NOT_A_MEMBER.to_string(),
            ROOM_NOT_STARTING.to_string(),
            ROOM_STARTING.to_string(),
            ROOMS_TOO_OFTEN.to_string(),
            JOINS_TOO_OFTEN.to_string(),
            TOO_MANY_REQUESTS.to_string(),
            NO_ROOMS_LEFT.to_string(),
            PLAYER_LEFT.to_string(),
            SIGNED_IN_ELSEWHERE.to_string(),
            ROOM_WAITING.to_string(),
            waiting_for_players(3, 4),
            waiting_on(&["WWWWWWWWWWWWWWW", "WWWWWWWWWWWWWWW", "WWWWWWWWWWWWWWW"]),
            PICK_TEAMS.to_string(),
            WAITING_FOR_GAME.to_string(),
            IN_GAME.to_string(),
            STARTING_GAME.to_string(),
            waiting_on(&["WW", "BO"]),
            waiting_on(&["WWWWWWWWWWWWWWW"]),
            waiting_on(&["player1", "player2", "player4"]),
        ];
        for m in all {
            assert!(m.chars().count() <= 36, "{m:?} is {} characters", m.chars().count());
            assert!(rough_width(&m) <= 640, "{m:?} is about {} font units", rough_width(&m));
        }
    }
}
