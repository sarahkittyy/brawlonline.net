//! The matchmaking wire protocol: JSON messages carried in reliable ENet
//! packets on channel 0, exactly as Slippi's client sends and reads them
//! (`Ishiiruka Source/Core/Core/Slippi/SlippiMatchmaking.cpp`, `create-ticket`
//! at `:426-436`, `get-ticket-resp` parsing at `:473-660`).
//!
//! The struct shapes follow openmelee (`src/matchmaking.rs:16-109`, GPL-2.0),
//! extended with every field the Slippi client reads (`chatMessages`, `rank`,
//! `isBot`, `items`, `error`, `latestVersion`) and the `search.game` block the
//! Brawlback client adds.
//!
//! Wire notes:
//! - `search.connectCode` is a JSON **array of byte values** (the game's
//!   Shift-JIS buffer), not a string. Unranked sends `[]`.
//! - Every field the client reads with `json.value(key, default)` is optional
//!   for the client, but we always send the full set so behaviour does not
//!   depend on client defaults.
//! - `players[].chatMessages` must have exactly 16 entries or the client
//!   substitutes its defaults.
//! - The client splits `ipAddress`/`ipAddressLan` on `:` so they must be
//!   IPv4 `a.b.c.d:port`.

use serde::{Deserialize, Serialize};

/// Slippi's production mm port (`SlippiMatchmaking.h:100`).
pub const MM_PORT: u16 = 43113;
/// The client sends and expects everything on channel 0.
pub const MM_CHANNEL: u8 = 0;
/// The client creates its host and connects with 3 channels.
pub const MM_CHANNEL_COUNT: usize = 3;
/// The in-game error area shows at most 120 characters.
pub const MAX_ERROR_LEN: usize = 120;

pub const CREATE_TICKET: &str = "create-ticket";
pub const CREATE_TICKET_RESP: &str = "create-ticket-resp";
pub const GET_TICKET_RESP: &str = "get-ticket-resp";

/// Online play modes (`SlippiMatchmaking.h:25-32`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Mode {
    Ranked,
    Unranked,
    Direct,
    Teams,
    Party,
}

impl Mode {
    pub fn from_u8(v: u8) -> Option<Mode> {
        Some(match v {
            0 => Mode::Ranked,
            1 => Mode::Unranked,
            2 => Mode::Direct,
            3 => Mode::Teams,
            4 => Mode::Party,
            _ => return None,
        })
    }

    pub fn as_u8(self) -> u8 {
        match self {
            Mode::Ranked => 0,
            Mode::Unranked => 1,
            Mode::Direct => 2,
            Mode::Teams => 3,
            Mode::Party => 4,
        }
    }

    /// The name used in match ids (`mode.<name>-...`). The client checks for
    /// the substring `mode.ranked` (`SlippiMatchmaking.cpp:651`).
    pub fn name(self) -> &'static str {
        match self {
            Mode::Ranked => "ranked",
            Mode::Unranked => "unranked",
            Mode::Direct => "direct",
            Mode::Teams => "teams",
            Mode::Party => "party",
        }
    }
}

/// `create-ticket`, client → server.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateTicket {
    #[serde(rename = "type")]
    pub kind: String,
    pub user: TicketUser,
    pub search: Search,
    #[serde(default)]
    pub app_version: String,
    /// The platform the build is published for (`win`, `mac`, `linux`): its update feed names
    /// the version it must have. Empty from builds older than the field.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub platform: String,
    #[serde(default)]
    pub ip_address_lan: String,
}

/// `create-ticket.user`. Brawlback's client sends only `uid` and `playKey`;
/// Slippi's also sends `connectCode` and `displayName`. The server never trusts
/// the last two; it reads them from the account.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TicketUser {
    pub uid: String,
    pub play_key: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub connect_code: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub display_name: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Search {
    pub mode: u8,
    /// Shift-JIS bytes of the code typed in-game (Direct/Teams), else empty.
    #[serde(default)]
    pub connect_code: Vec<u8>,
    /// Brawlback's game block `{id, ex_id, revision, type, name}`; accepted and
    /// logged, not enforced yet (the build-hash allow-list is a later phase).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub game: Option<serde_json::Value>,
}

/// `create-ticket-resp`, server → client. An `error` is shown in-game.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateTicketResp {
    #[serde(rename = "type")]
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

impl CreateTicketResp {
    pub fn ok() -> Self {
        CreateTicketResp { kind: CREATE_TICKET_RESP.into(), error: None }
    }
    pub fn error(msg: impl Into<String>) -> Self {
        CreateTicketResp { kind: CREATE_TICKET_RESP.into(), error: Some(clamp_error(msg.into())) }
    }
}

/// `get-ticket-resp`, server → client: either a match or an error.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GetTicketResp {
    #[serde(rename = "type")]
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub latest_version: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub match_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub is_host: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub players: Option<Vec<Player>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub stages: Option<Vec<u16>>,
    /// Ranked: the starter stages struck for game 1 (ours; Slippi's client has them built in).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub starters: Option<Vec<u16>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub items: Option<u32>,
    /// The match's rules (ours; `crate::rules`). A client without them plays P+'s competitive set.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rules: Option<crate::rules::MatchRules>,
    /// A room's set: the session plays it game by game (`SetTicket`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub set: Option<SetTicket>,
    /// A room whose loser picks from the stage list (`StageChoice::LoserPicksList`): a stage pick
    /// must be one of `stages`. Absent: any stage.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pick_from_stages: Option<bool>,
    /// P+'s alternate stages: a stage pick (a loser's, a set's counterpick) plays the layout the
    /// picker's held L, R or Z selects (Direct, Teams, a room whose ruleset has them on). Absent:
    /// every pick plays the stage's normal layout.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub alternate_stages: Option<bool>,
    /// Every player picks the stage of every game, game 1 too, all on the stage select at once,
    /// and the first pick the session's host sees is played (Direct, Sarah's board, 2026-10-29:
    /// "could we make this the default for DIRECT only?"; rooms do it with their ruleset's
    /// `anyone-picks` and the pickers instead). Absent: Slippi's (Direct: game 1 random, then
    /// the loser picks).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub anyone_picks: Option<bool>,
}

/// A room's set as its session starts it (ours; Dolphin's `Online/SetFormat.h` `Format` and
/// `SetState`): the format and the score so far. Sides are the set's two players, side 0 the lower
/// slot; `sides` gives each side's in-game port (0-3).
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SetTicket {
    /// 3, 5, 7; 0 with `strikeEveryGame` (no set, every game struck).
    pub best_of: u8,
    pub strike_every_game: bool,
    pub starters: Vec<u16>,
    /// The stages of games 2+: the starters and the counterpicks.
    pub counterpicks: Vec<u16>,
    pub strikes: Vec<u8>,
    pub bans: u8,
    pub dsr: bool,
    /// Bit (game - 1) % 32: the side that strikes first in that game.
    pub first_striker_bits: u32,
    pub sides: [u8; 2],
    /// Each side's ports (0-3), lowest first: one player in 1v1, a team in 2v2.
    #[serde(default)]
    pub members: Vec<Vec<u8>>,
    /// The set's game the session starts with (1 for a new set).
    pub game: u32,
    pub wins: [u8; 2],
    /// The last game's winning side; 0xFE a draw, 0xFF none yet.
    pub last_winner: u8,
    /// Each side's stage of its last win (0xFFFF none).
    pub last_win_stage: [u16; 2],
    /// The game's stage actions so far, per side (`room-step`): a session that starts again in the
    /// middle of a game's strikes goes on from them. Empty for a game not set up yet.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub actions: Vec<Vec<u8>>,
}

impl GetTicketResp {
    pub fn error(msg: impl Into<String>, latest_version: Option<String>) -> Self {
        GetTicketResp {
            kind: GET_TICKET_RESP.into(),
            error: Some(clamp_error(msg.into())),
            latest_version,
            ..Default::default()
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Player {
    pub uid: String,
    pub display_name: String,
    pub connect_code: String,
    /// Controller port, 1-4.
    pub port: u8,
    pub is_local_player: bool,
    /// External `ip:port` as the mm server saw it (the hole-punched mapping).
    pub ip_address: String,
    /// The `ipAddressLan` the client reported (may be empty).
    pub ip_address_lan: String,
    pub chat_messages: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rank: Option<Rank>,
    #[serde(default)]
    pub is_bot: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Rank {
    pub rating: f32,
    pub update_count: u32,
    pub global_placement: u16,
    pub regional_placement: u16,
}

/// Truncates an error string to what the game can show (120 characters).
pub fn clamp_error(mut msg: String) -> String {
    if msg.chars().count() > MAX_ERROR_LEN {
        msg = msg.chars().take(MAX_ERROR_LEN).collect();
    }
    msg
}

/// What a client message turned out to be.
#[derive(Debug)]
pub enum ClientMessage {
    CreateTicket(Box<CreateTicket>),
    /// `hello`: opens an online connection (rooms, the online count).
    Hello(Box<crate::rooms::Hello>),
    /// A `room-*` request, or why it could not be read.
    Room(Result<crate::rooms::RoomRequest, String>),
    /// A `chat-*` request (`docs/chat-protocol.md`), or why it could not be read: the `op` to
    /// answer in `chat-error` (the message type, `invalid` for an unknown `chat-*` type) and the
    /// reason.
    Chat(Result<crate::chat::ChatRequest, (&'static str, String)>),
    /// A `mods-*` request (Brawl Mods rooms, `crate::mods`), or why it could not be read.
    Mods(Result<crate::mods::ModsRequest, String>),
    /// `mods-endpoint`: a member's session probing from its P2P socket (no `hello`).
    ModsEndpoint(crate::mods::EndpointProbe),
    /// A `spectate-*` message (`docs/spectate-protocol.md` §1), or why it could not be read: the
    /// `op` to answer in `spectate-error` and the reason.
    Spectate(Result<crate::spectate::SpectateRequest, (&'static str, String)>),
    /// `speed-check`: the waiting room's frame rate (`crate::speed`), or why it could not be read.
    SpeedCheck(Result<crate::speed::SpeedCheck, String>),
    /// `game-alive`: the game's emulation is running ([`GAME_ALIVE`]). `ranked`: it plays a Ranked
    /// set and sends them every [`GAME_ALIVE_RANKED_EVERY`].
    GameAlive {
        ranked: bool,
    },
    /// Valid JSON object with a `type` we do not handle.
    Unknown(String),
}

#[derive(Debug, thiserror::Error)]
pub enum ParseError {
    #[error("packet is not UTF-8")]
    NotUtf8,
    #[error("packet is not valid JSON: {0}")]
    Json(String),
    #[error("message has no string `type`")]
    NoType,
    #[error("malformed {0}: {1}")]
    Malformed(&'static str, String),
}

/// `game-alive`, client → server on an online connection, no other fields: the game's emulation
/// ran in the last [`GAME_ALIVE_EVERY`], sent that often while it does (bug report #105,
/// 2026-10-22). The game ticks it from its CPU thread at every emulated field, every game frame
/// and every round of a wait on its peer, so a game waiting on a slow or silent peer still sends
/// it; one whose emulation froze (or was paused) does not, while its online connection, served by
/// another thread, stays up. A `hello` with `gameAlive` says the game sends it.
pub const GAME_ALIVE: &str = "game-alive";

/// How often a running game sends [`GAME_ALIVE`].
pub const GAME_ALIVE_EVERY: std::time::Duration = std::time::Duration::from_secs(2);

/// How often a game in a Ranked set sends [`GAME_ALIVE`], with `"ranked": true` (2026-10-29): mm
/// times them, and one that comes late is a silence on that player's own connection
/// (`ranked_net_events`), which tells whose end a broken link or an aborted game was on. An older
/// mm ignores the field.
pub const GAME_ALIVE_RANKED_EVERY: std::time::Duration = std::time::Duration::from_millis(500);

/// Parses one client packet. Never panics on any input.
pub fn parse_client_message(data: &[u8]) -> Result<ClientMessage, ParseError> {
    let text = std::str::from_utf8(data).map_err(|_| ParseError::NotUtf8)?;
    let value: serde_json::Value = serde_json::from_str(text).map_err(|e| ParseError::Json(e.to_string()))?;
    let kind = value.get("type").and_then(|t| t.as_str()).ok_or(ParseError::NoType)?.to_string();
    match kind.as_str() {
        CREATE_TICKET => serde_json::from_value::<CreateTicket>(value)
            .map(|t| ClientMessage::CreateTicket(Box::new(t)))
            .map_err(|e| ParseError::Malformed(CREATE_TICKET, e.to_string())),
        crate::rooms::HELLO => serde_json::from_value::<crate::rooms::Hello>(value)
            .map(|h| ClientMessage::Hello(Box::new(h)))
            .map_err(|e| ParseError::Malformed(crate::rooms::HELLO, e.to_string())),
        k if k.starts_with("room-") => Ok(ClientMessage::Room(crate::rooms::RoomRequest::parse(k, &value))),
        crate::mods::MODS_ENDPOINT => serde_json::from_value::<crate::mods::EndpointProbe>(value)
            .map(ClientMessage::ModsEndpoint)
            .map_err(|e| ParseError::Malformed(crate::mods::MODS_ENDPOINT, e.to_string())),
        k if k.starts_with("mods-") => Ok(ClientMessage::Mods(crate::mods::ModsRequest::parse(k, &value))),
        k if k.starts_with("chat-") => {
            Ok(ClientMessage::Chat(crate::chat::ChatRequest::parse(k, &value).map_err(|e| (crate::chat::op_of(k), e))))
        }
        k if k.starts_with("spectate-") => {
            Ok(ClientMessage::Spectate(crate::spectate::SpectateRequest::parse(k, &value)))
        }
        crate::speed::SPEED_CHECK => Ok(ClientMessage::SpeedCheck(crate::speed::SpeedCheck::parse(&value))),
        GAME_ALIVE => {
            Ok(ClientMessage::GameAlive { ranked: value.get("ranked").and_then(|r| r.as_bool()).unwrap_or(false) })
        }
        _ => Ok(ClientMessage::Unknown(kind)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// openmelee's direct-mode fixture, which is what Slippi 2.5.1 sent.
    const SLIPPI_DIRECT: &str = r#"
        {
            "type": "create-ticket",
            "appVersion": "2.5.1",
            "ipAddressLan": "127.0.0.2:50285",
            "search": {
                "connectCode": [130, 115, 130, 100, 130, 114, 130, 115, 129, 148, 130, 79, 130, 79, 130, 81],
                "mode": 2
            },
            "user": {
                "connectCode": "TEST#001",
                "displayName": "test",
                "playKey": "1",
                "uid": "1"
            }
        }"#;

    /// Brawlback Gen 1's ticket (`Matchmaking.cpp:493-511`): no connectCode or
    /// displayName in `user`, plus `search.game`.
    const BRAWLBACK_DIRECT: &str = r#"
        {
            "type": "create-ticket",
            "appVersion": "0.0.1",
            "ipAddressLan": "192.168.1.20:45123",
            "search": {
                "mode": 2,
                "connectCode": [65, 66, 35, 49],
                "game": {"id": "RSBE01", "ex_id": "RSBE01", "revision": 1, "type": 1, "name": "Super Smash Bros. Brawl"}
            },
            "user": {"uid": "u", "playKey": "k"}
        }"#;

    #[test]
    fn parses_slippi_ticket() {
        let ClientMessage::CreateTicket(t) = parse_client_message(SLIPPI_DIRECT.as_bytes()).unwrap() else {
            panic!("expected create-ticket");
        };
        assert_eq!(t.app_version, "2.5.1");
        assert_eq!(t.ip_address_lan, "127.0.0.2:50285");
        assert_eq!(Mode::from_u8(t.search.mode), Some(Mode::Direct));
        assert_eq!(crate::codes::decode_search_code(&t.search.connect_code).unwrap().to_string(), "TEST#2");
        assert_eq!(t.user.connect_code.as_deref(), Some("TEST#001"));
    }

    #[test]
    fn parses_brawlback_ticket() {
        let ClientMessage::CreateTicket(t) = parse_client_message(BRAWLBACK_DIRECT.as_bytes()).unwrap() else {
            panic!("expected create-ticket");
        };
        assert_eq!(t.user.connect_code, None);
        assert_eq!(t.search.game.as_ref().unwrap()["id"], "RSBE01");
        assert_eq!(crate::codes::decode_search_code(&t.search.connect_code).unwrap().to_string(), "AB#1");
    }

    #[test]
    fn unranked_ticket_with_empty_code() {
        let msg = r#"{"type":"create-ticket","user":{"uid":"1","playKey":"2"},"search":{"mode":1,"connectCode":[]},"appVersion":"3.4.0","ipAddressLan":""}"#;
        let ClientMessage::CreateTicket(t) = parse_client_message(msg.as_bytes()).unwrap() else { panic!() };
        assert!(t.search.connect_code.is_empty());
    }

    #[test]
    fn malformed_inputs_are_errors_not_panics() {
        let cases: Vec<&[u8]> = vec![
            b"",
            b"\xff\xfe",
            b"not json",
            b"[]",
            b"{}",
            b"{\"type\":5}",
            b"{\"type\":\"create-ticket\"}",
            b"{\"type\":\"create-ticket\",\"user\":{},\"search\":{\"mode\":2}}",
            b"{\"type\":\"create-ticket\",\"user\":{\"uid\":\"a\",\"playKey\":\"b\"},\"search\":{\"mode\":2,\"connectCode\":[256]}}",
            b"{\"type\":\"create-ticket\",\"user\":{\"uid\":\"a\",\"playKey\":\"b\"},\"search\":{\"mode\":-1}}",
            b"{\"type\":\"create-ticket\",\"user\":{\"uid\":\"a\",\"playKey\":\"b\"},\"search\":\"x\"}",
        ];
        for c in cases {
            assert!(parse_client_message(c).is_err(), "{:?}", String::from_utf8_lossy(c));
        }
        assert!(matches!(parse_client_message(br#"{"type":"get-ticket"}"#), Ok(ClientMessage::Unknown(_))));
    }

    #[test]
    fn chat_requests_are_routed() {
        let ok = br#"{"type":"chat-leave","group":"room-KFQB-1"}"#;
        assert!(matches!(parse_client_message(ok), Ok(ClientMessage::Chat(Ok(_)))));
        let bad = br#"{"type":"chat-leave","group":"no spaces"}"#;
        assert!(matches!(parse_client_message(bad), Ok(ClientMessage::Chat(Err(("chat-leave", _))))));
        let unknown = br#"{"type":"chat-bogus"}"#;
        assert!(matches!(parse_client_message(unknown), Ok(ClientMessage::Chat(Err(("invalid", _))))));
    }

    #[test]
    fn get_ticket_resp_wire_format() {
        let resp = GetTicketResp {
            kind: GET_TICKET_RESP.into(),
            match_id: Some("mode.direct-x".into()),
            is_host: Some(true),
            players: Some(vec![Player {
                uid: "u1".into(),
                display_name: "one".into(),
                connect_code: "ONE#1".into(),
                port: 1,
                is_local_player: true,
                ip_address: "1.2.3.4:41000".into(),
                ip_address_lan: "192.168.0.2:41000".into(),
                chat_messages: crate::DEFAULT_CHAT_MESSAGES.iter().map(|s| s.to_string()).collect(),
                rank: None,
                is_bot: false,
            }]),
            stages: Some(vec![]),
            items: Some(0),
            ..Default::default()
        };
        let v: serde_json::Value = serde_json::to_value(&resp).unwrap();
        assert_eq!(v["type"], "get-ticket-resp");
        assert_eq!(v["matchId"], "mode.direct-x");
        assert_eq!(v["isHost"], true);
        assert!(v.get("error").is_none());
        let p = &v["players"][0];
        for key in [
            "uid",
            "displayName",
            "connectCode",
            "port",
            "isLocalPlayer",
            "ipAddress",
            "ipAddressLan",
            "chatMessages",
            "isBot",
        ] {
            assert!(p.get(key).is_some(), "missing {key}");
        }
        assert_eq!(p["chatMessages"].as_array().unwrap().len(), 16);
        assert_eq!(v["items"], 0);

        let err = serde_json::to_value(GetTicketResp::error("x".repeat(500), None)).unwrap();
        assert_eq!(err["error"].as_str().unwrap().len(), MAX_ERROR_LEN);
        assert!(err.get("players").is_none());
    }
}
