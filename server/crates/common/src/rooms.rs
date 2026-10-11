//! Rooms: the wire types of the room protocol (`docs/rooms-protocol.md`) and the room code rules.
//!
//! A room lives on the mm server, in memory. Its members keep one ENet connection to mm open (the
//! "online connection": `hello`, then `room-*` messages), which is also what the online count
//! counts. When a room starts, each member sends an ordinary `create-ticket` (mode 3, the room
//! code as `search.connectCode`) from its P2P port, and mm answers every member with one
//! `get-ticket-resp` that lists all of them, as Slippi's Teams mode does.
//!
//! Room codes are 4 letters without vowels (`BCDFGHJKLMNPQRSTVWXZ`, Y left out too): 160,000
//! codes, unique among live rooms, freed when a room empties (`docs/design/rooms.md` §1 #5).

use serde::{Deserialize, Serialize};
use unicode_normalization::UnicodeNormalization;

/// The letters a room code is made of.
pub const ROOM_CODE_ALPHABET: &[u8; 20] = b"BCDFGHJKLMNPQRSTVWXZ";
/// Letters in a room code.
pub const ROOM_CODE_LEN: usize = 4;
/// Panels (slots) in a room; a slot is the in-game port.
pub const ROOM_SLOTS: usize = 4;
/// Team colours a player can pick with Teams on (Brawl's red, blue, green).
pub const TEAM_COUNT: u8 = 3;

pub const HELLO: &str = "hello";
pub const HELLO_RESP: &str = "hello-resp";
pub const ROOM_CREATE: &str = "room-create";
pub const ROOM_JOIN: &str = "room-join";
pub const ROOM_LEAVE: &str = "room-leave";
pub const ROOM_SLOT: &str = "room-slot";
pub const ROOM_TEAMS: &str = "room-teams";
pub const ROOM_PUBLIC: &str = "room-public";
pub const ROOM_READY: &str = "room-ready";
pub const ROOM_TEAM: &str = "room-team";
pub const ROOM_BACK: &str = "room-back";
pub const ROOM_RULES: &str = "room-rules";
pub const ROOM_RESULT: &str = "room-result";
pub const ROOM_SET: &str = "room-set";
pub const ROOM_RULESET: &str = "room-ruleset";
/// `room-result.winnerSide` of a draw.
pub const SIDE_DRAW: u8 = 0xFE;
pub const ROOM_STATE: &str = "room-state";
pub const ROOM_ERROR: &str = "room-error";
pub const ROOM_LEFT: &str = "room-left";
pub const ROOM_START: &str = "room-start";
pub const ERROR: &str = "error";

/// Normalizes a room code typed anywhere (any case, full width, surrounding spaces) and checks
/// it: `kfqb`, `ＫＦＱＢ` and ` KFQB ` all give `KFQB`. `None` if it is not 4 letters of
/// [`ROOM_CODE_ALPHABET`].
pub fn parse_room_code(input: &str) -> Option<String> {
    let normalized: String = input.nfkc().collect::<String>().trim().to_ascii_uppercase();
    (normalized.len() == ROOM_CODE_LEN && normalized.bytes().all(|b| ROOM_CODE_ALPHABET.contains(&b)))
        .then_some(normalized)
}

/// Decodes the room code a room game ticket carries in `search.connectCode`: the game's
/// Shift-JIS keypad buffer (usually full width) or plain ASCII, trailing NULs stripped.
pub fn decode_room_search_code(bytes: &[u8]) -> Option<String> {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    let bytes = &bytes[..end];
    if bytes.is_empty() || bytes.len() > crate::codes::MAX_SEARCH_CODE_BYTES {
        return None;
    }
    let (decoded, _, had_errors) = encoding_rs::SHIFT_JIS.decode(bytes);
    if had_errors {
        return None;
    }
    parse_room_code(&decoded)
}

/// A random room code (the caller retries while it is taken).
pub fn random_room_code(rng: &mut impl rand::Rng) -> String {
    (0..ROOM_CODE_LEN).map(|_| ROOM_CODE_ALPHABET[rng.gen_range(0..ROOM_CODE_ALPHABET.len())] as char).collect()
}

/// The `user` block of `hello` (the same fields a ticket's `user` has; only `uid` and `playKey`
/// are read).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HelloUser {
    pub uid: String,
    pub play_key: String,
}

/// `hello`, client → server: opens the online connection.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Hello {
    #[serde(rename = "type")]
    pub kind: String,
    pub user: HelloUser,
    #[serde(default)]
    pub app_version: String,
    /// As a ticket's `platform`.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub platform: String,
    /// The install's chat identity key (64 hex digits, Ed25519; `docs/chat-protocol.md` §1). Empty
    /// from builds older than chat: such a connection can't chat. Anything but a string reads as
    /// empty (a malformed key is ignored, never a reason to refuse the `hello`).
    #[serde(default, skip_serializing_if = "String::is_empty", deserialize_with = "string_or_empty")]
    pub chat_key: String,
}

fn string_or_empty<'de, D: serde::Deserializer<'de>>(d: D) -> Result<String, D::Error> {
    Ok(match serde_json::Value::deserialize(d)? {
        serde_json::Value::String(s) => s,
        _ => String::new(),
    })
}

/// A `room-*` request, client → server, on an online connection.
#[derive(Debug, Clone, PartialEq)]
pub enum RoomRequest {
    /// `room-create {public?, ruleset?, remembered?}`: public unless `public: false`. `ruleset`: a
    /// published ruleset's code to play (the launcher's box); without one, `remembered: true` (the
    /// game's own Create Room) plays the account's remembered ruleset (`docs/design/room-rules.md`
    /// decision 11), else P+'s default.
    Create { public: bool, ruleset: Option<String>, remembered: bool },
    /// `room-join {code}`.
    Join { code: String },
    /// `room-leave {}`.
    Leave,
    /// `room-slot {slot, open}` (host only): slot 1-4.
    Slot { slot: u8, open: bool },
    /// `room-teams {on}` (host only).
    Teams { on: bool },
    /// `room-public {public}` (host only).
    Public { public: bool },
    /// `room-ready {ready, character?, costume?}`: START on the CSS (lock-in) or taking it back.
    Ready { ready: bool, character: Option<u8>, costume: Option<u8> },
    /// `room-team {team}`: 0 red, 1 blue, 2 green.
    Team { team: u8 },
    /// `room-back {}`: this player is back on the room's CSS after a game.
    Back,
    /// `room-rules {rules}` (host only, between games): the room's match rules
    /// (`docs/design/room-rules.md`), checked by `MatchRules::validate`.
    Rules { rules: crate::rules::MatchRules },
    /// `room-result {game, winnerSide, stage}`: a game of the room's set as this member's game saw
    /// it end (`winnerSide` 0 / 1, [`SIDE_DRAW`]; `stage` the srStageKind played). The server
    /// counts it once every player of the set reported the same.
    Result { game: u32, winner_side: u8, stage: u16 },
    /// `room-set {action}` (host, between games): `resume` a paused set, or `restart` it.
    Set { resume: bool },
    /// `room-ruleset {code}` (host, between games, no set on): play a published ruleset (a code),
    /// or P+'s default (`code` null). The code also becomes the host's remembered ruleset.
    Ruleset { code: Option<String> },
}

impl RoomRequest {
    /// The message type, for logs and `room-error.op`.
    pub fn op(&self) -> &'static str {
        match self {
            RoomRequest::Create { .. } => ROOM_CREATE,
            RoomRequest::Join { .. } => ROOM_JOIN,
            RoomRequest::Leave => ROOM_LEAVE,
            RoomRequest::Slot { .. } => ROOM_SLOT,
            RoomRequest::Teams { .. } => ROOM_TEAMS,
            RoomRequest::Public { .. } => ROOM_PUBLIC,
            RoomRequest::Ready { .. } => ROOM_READY,
            RoomRequest::Team { .. } => ROOM_TEAM,
            RoomRequest::Back => ROOM_BACK,
            RoomRequest::Rules { .. } => ROOM_RULES,
            RoomRequest::Result { .. } => ROOM_RESULT,
            RoomRequest::Set { .. } => ROOM_SET,
            RoomRequest::Ruleset { .. } => ROOM_RULESET,
        }
    }

    /// The wire form (for clients).
    pub fn to_json(&self) -> serde_json::Value {
        use serde_json::json;
        let mut v = match self {
            RoomRequest::Create { public, ruleset, remembered } => {
                let mut v = json!({ "public": public });
                if let Some(code) = ruleset {
                    v["ruleset"] = json!(code);
                }
                if *remembered {
                    v["remembered"] = json!(true);
                }
                v
            }
            RoomRequest::Join { code } => json!({ "code": code }),
            RoomRequest::Leave | RoomRequest::Back => json!({}),
            RoomRequest::Slot { slot, open } => json!({ "slot": slot, "open": open }),
            RoomRequest::Teams { on } => json!({ "on": on }),
            RoomRequest::Public { public } => json!({ "public": public }),
            RoomRequest::Ready { ready, character, costume } => {
                let mut v = json!({ "ready": ready });
                if let Some(c) = character {
                    v["character"] = json!(c);
                }
                if let Some(c) = costume {
                    v["costume"] = json!(c);
                }
                v
            }
            RoomRequest::Team { team } => json!({ "team": team }),
            RoomRequest::Rules { rules } => json!({ "rules": rules }),
            RoomRequest::Result { game, winner_side, stage } => {
                json!({ "game": game, "winnerSide": winner_side, "stage": stage })
            }
            RoomRequest::Set { resume } => json!({ "action": if *resume { "resume" } else { "restart" } }),
            RoomRequest::Ruleset { code } => json!({ "code": code }),
        };
        v["type"] = json!(self.op());
        v
    }

    /// Parses a `room-*` message. `Err` names what is wrong (the server answers `room-error`).
    pub fn parse(kind: &str, v: &serde_json::Value) -> Result<RoomRequest, String> {
        let flag = |key: &str| -> Result<bool, String> {
            v.get(key).and_then(|b| b.as_bool()).ok_or_else(|| format!("{kind}: `{key}` must be true or false"))
        };
        let small = |key: &str, max: u64| -> Result<Option<u8>, String> {
            match v.get(key) {
                None | Some(serde_json::Value::Null) => Ok(None),
                Some(x) => match x.as_u64() {
                    Some(n) if n <= max => Ok(Some(n as u8)),
                    _ => Err(format!("{kind}: `{key}` must be a number from 0 to {max}")),
                },
            }
        };
        Ok(match kind {
            ROOM_CREATE => RoomRequest::Create {
                public: match v.get("public") {
                    None | Some(serde_json::Value::Null) => true,
                    Some(_) => flag("public")?,
                },
                ruleset: match v.get("ruleset") {
                    None | Some(serde_json::Value::Null) => None,
                    Some(c) => Some(
                        c.as_str()
                            .and_then(crate::rules::parse_ruleset_code)
                            .ok_or_else(|| format!("{kind}: `ruleset` is not a ruleset code"))?,
                    ),
                },
                remembered: match v.get("remembered") {
                    None | Some(serde_json::Value::Null) => false,
                    Some(_) => flag("remembered")?,
                },
            },
            ROOM_JOIN => RoomRequest::Join {
                code: v
                    .get("code")
                    .and_then(|c| c.as_str())
                    .filter(|c| c.len() <= 32)
                    .ok_or_else(|| format!("{kind}: `code` must be a string"))?
                    .to_string(),
            },
            ROOM_LEAVE => RoomRequest::Leave,
            ROOM_SLOT => {
                let slot = small("slot", ROOM_SLOTS as u64)?
                    .filter(|s| *s >= 1)
                    .ok_or_else(|| format!("{kind}: `slot` must be 1 to {ROOM_SLOTS}"))?;
                RoomRequest::Slot { slot, open: flag("open")? }
            }
            ROOM_TEAMS => RoomRequest::Teams { on: flag("on")? },
            ROOM_PUBLIC => RoomRequest::Public { public: flag("public")? },
            ROOM_READY => RoomRequest::Ready {
                ready: flag("ready")?,
                character: small("character", 255)?,
                costume: small("costume", 255)?,
            },
            ROOM_TEAM => RoomRequest::Team {
                team: small("team", (TEAM_COUNT - 1) as u64)?.ok_or_else(|| format!("{kind}: `team` is missing"))?,
            },
            ROOM_BACK => RoomRequest::Back,
            ROOM_RULES => {
                let raw = v.get("rules").ok_or_else(|| format!("{kind}: `rules` is missing"))?;
                let rules: crate::rules::MatchRules =
                    serde_json::from_value(raw.clone()).map_err(|e| format!("{kind}: {e}"))?;
                rules.validate().map_err(|e| format!("{kind}: {e}"))?;
                RoomRequest::Rules { rules }
            }
            ROOM_RESULT => {
                let game = v
                    .get("game")
                    .and_then(|g| g.as_u64())
                    .filter(|g| (1..=99).contains(g))
                    .ok_or_else(|| format!("{kind}: `game` must be 1 to 99"))? as u32;
                let winner_side = small("winnerSide", SIDE_DRAW as u64)?
                    .filter(|s| *s <= 1 || *s == SIDE_DRAW)
                    .ok_or_else(|| format!("{kind}: `winnerSide` must be 0, 1 or {SIDE_DRAW}"))?;
                let stage = v
                    .get("stage")
                    .and_then(|s| s.as_u64())
                    .filter(|s| *s <= 0xFF)
                    .ok_or_else(|| format!("{kind}: `stage` must be a stage id"))? as u16;
                RoomRequest::Result { game, winner_side, stage }
            }
            ROOM_RULESET => RoomRequest::Ruleset {
                code: match v.get("code") {
                    None | Some(serde_json::Value::Null) => None,
                    Some(c) => Some(
                        c.as_str()
                            .and_then(crate::rules::parse_ruleset_code)
                            .ok_or_else(|| format!("{kind}: `code` is not a ruleset code"))?,
                    ),
                },
            },
            ROOM_SET => RoomRequest::Set {
                resume: match v.get("action").and_then(|a| a.as_str()) {
                    Some("resume") => true,
                    Some("restart") => false,
                    _ => return Err(format!("{kind}: `action` must be resume or restart")),
                },
            },
            other => return Err(format!("unknown room message {other:?}")),
        })
    }
}

/// How a room plays, from its open slots and the Teams switch: two open slots are 1v1 whatever
/// the switch says (`docs/design/rooms.md` #12).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum RoomMode {
    #[serde(rename = "1v1")]
    OneVsOne,
    Ffa,
    Teams,
}

impl RoomMode {
    pub fn of(open_slots: usize, teams: bool) -> RoomMode {
        if open_slots <= 2 {
            RoomMode::OneVsOne
        } else if teams {
            RoomMode::Teams
        } else {
            RoomMode::Ffa
        }
    }
}

/// Where a room is between its games.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum RoomStatus {
    /// Between games: on the room's CSS.
    Waiting,
    /// Everyone was ready; the members' game tickets are coming in.
    Starting,
    /// A game is being played (by at least one member who has not come back yet).
    InGame,
}

/// A player on a panel, as every member sees it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomPlayer {
    pub display_name: String,
    pub connect_code: String,
    pub ready: bool,
    /// 0 red, 1 blue, 2 green. Meaningful with Teams on.
    pub team: u8,
    /// The lock-in, once ready (opaque to the server: the game's character and costume ids).
    pub character: Option<u8>,
    pub costume: Option<u8>,
    /// Still in the last game (not back on the room's CSS).
    pub in_game: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomSlot {
    /// 1-4, the in-game port.
    pub slot: u8,
    pub open: bool,
    pub host: bool,
    pub player: Option<RoomPlayer>,
}

/// `room-state`, server → every member after every change.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomState {
    #[serde(rename = "type")]
    pub kind: String,
    pub code: String,
    pub public: bool,
    pub teams: bool,
    pub mode: RoomMode,
    pub status: RoomStatus,
    /// The receiving member's slot.
    pub you: u8,
    /// The host's slot.
    pub host: u8,
    pub slots: Vec<RoomSlot>,
    /// The status line for the receiving member (`docs/design/rooms.md` §3).
    pub status_text: String,
    /// The room's match rules (`docs/design/room-rules.md`): the host's, sent with every game.
    #[serde(default)]
    pub rules: crate::rules::MatchRules,
    /// The room's ruleset: a published one (its code) or custom.
    #[serde(default)]
    pub ruleset: RoomRuleset,
    /// The room's set, while its ruleset plays one (`docs/design/room-rules.md` §5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub set: Option<RoomSet>,
}

/// What a room plays, as its members see it (`room-state.ruleset`).
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomRuleset {
    /// The published ruleset's code; `None` for P+'s default or custom rules.
    pub code: Option<String>,
    pub name: String,
    pub version: u32,
    pub by: String,
    /// Custom rules made from a published ruleset: "NorCal Weekly v3".
    pub based_on: Option<String>,
    pub players: crate::rules::Players,
    pub best_of: u8,
    pub stage_choice: crate::rules::StageChoice,
}

/// A room's set (`room-state.set`): the score by side, side 0 being the lower slot.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomSet {
    pub best_of: u8,
    /// The game being played or next (1-based).
    pub game: u32,
    pub score: [u8; 2],
    /// Each side's slot (1-4).
    pub sides: [u8; 2],
    /// A player of the set left (or the results differed): the set waits for the host.
    pub paused: bool,
    /// The missing player's name while paused.
    pub waiting_for: Option<String>,
    /// The host may resume (everyone of the set is here).
    pub can_resume: bool,
    /// Someone won it (the score stays until the next set starts).
    pub finished: bool,
}

/// `room-error`, server → one member: a request was refused; nothing changed.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomError {
    #[serde(rename = "type")]
    pub kind: String,
    pub op: String,
    pub error: String,
}

/// `room-left`, server → a former member: it is no longer in the room (it left, or `reason`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomLeft {
    #[serde(rename = "type")]
    pub kind: String,
    pub code: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

/// `room-start`, server → every member: send the room game ticket now.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomStart {
    #[serde(rename = "type")]
    pub kind: String,
    pub code: String,
    pub match_id: String,
    /// Seconds mm waits for every member's ticket.
    pub timeout_secs: u64,
}

/// `hello-resp`, server → client: the online connection is open (or `error`, then a disconnect).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HelloResp {
    #[serde(rename = "type")]
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub latest_version: Option<String>,
}

/// One public room in the launcher's list.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PublicRoom {
    pub code: String,
    pub host: String,
    /// Players in the room.
    pub players: usize,
    /// Open slots (players + empty open slots): the most players the room takes.
    pub open_slots: usize,
    pub mode: RoomMode,
    /// `starting` is listed as `in-game`.
    pub status: RoomStatus,
    /// Every player's display name, by slot.
    pub names: Vec<String>,
    /// An open slot is empty, so the room can be joined (also during a game).
    pub joinable: bool,
}

/// What mm publishes for the launcher (`GET /status` on mm's local status listener, served to
/// launchers by accounts as `GET /v1/rooms`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct MmStatus {
    /// Players whose game is running and logged in: distinct accounts with an online connection.
    pub online: usize,
    /// Public rooms: joinable first, then waiting before in game, then fuller first.
    pub rooms: Vec<PublicRoom>,
    /// RFC 3339 time of the snapshot.
    pub updated_at: String,
    /// Every room player's room, public and private rooms alike. Internal: accounts reads it to
    /// check and address launcher invites (`docs/social-protocol.md`) and never serves it to
    /// launchers (`GET /v1/rooms` sends only the fields above).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub members: Vec<RoomMember>,
}

/// One room player's room, for [`MmStatus::members`].
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomMember {
    /// The player's account (hyphenated lowercase UUID).
    pub uid: String,
    pub code: String,
    pub players: usize,
    pub open_slots: usize,
    pub mode: RoomMode,
    /// `starting` is listed as `in-game`, as in [`PublicRoom`].
    pub status: RoomStatus,
    /// An open slot is empty.
    pub joinable: bool,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn room_codes() {
        assert_eq!(parse_room_code("kfqb").as_deref(), Some("KFQB"));
        assert_eq!(parse_room_code(" KFQB ").as_deref(), Some("KFQB"));
        assert_eq!(parse_room_code("ＫＦＱＢ").as_deref(), Some("KFQB"));
        for bad in ["", "KFQ", "KFQBB", "KAQB", "KYQB", "KF#B", "KF B", "1234"] {
            assert_eq!(parse_room_code(bad), None, "{bad:?}");
        }
        let fullwidth = crate::codes::encode_search_code_fullwidth("KFQB");
        assert_eq!(decode_room_search_code(&fullwidth).as_deref(), Some("KFQB"));
        assert_eq!(decode_room_search_code(b"kfqb\0\0").as_deref(), Some("KFQB"));
        assert_eq!(decode_room_search_code(b""), None);
        assert_eq!(decode_room_search_code(&[0x82]), None);
        let mut rng = rand::thread_rng();
        for _ in 0..200 {
            assert!(parse_room_code(&random_room_code(&mut rng)).is_some());
        }
    }

    #[test]
    fn requests_parse_and_round_trip() {
        let cases = [
            RoomRequest::Create { public: true, ruleset: None, remembered: false },
            RoomRequest::Create { public: false, ruleset: Some("BCDFGH".into()), remembered: false },
            RoomRequest::Create { public: true, ruleset: None, remembered: true },
            RoomRequest::Ruleset { code: Some("BCDFGH".into()) },
            RoomRequest::Ruleset { code: None },
            RoomRequest::Set { resume: true },
            RoomRequest::Result { game: 2, winner_side: 1, stage: 0x21 },
            RoomRequest::Join { code: "KFQB".into() },
            RoomRequest::Leave,
            RoomRequest::Slot { slot: 3, open: true },
            RoomRequest::Teams { on: true },
            RoomRequest::Public { public: false },
            RoomRequest::Ready { ready: true, character: Some(12), costume: Some(3) },
            RoomRequest::Ready { ready: false, character: None, costume: None },
            RoomRequest::Team { team: 2 },
            RoomRequest::Back,
            RoomRequest::Rules { rules: crate::rules::MatchRules::default() },
            RoomRequest::Rules {
                rules: crate::rules::MatchRules {
                    stocks: 3,
                    auto_l_cancel: true,
                    pause: crate::rules::Pause::Hold,
                    ..Default::default()
                },
            },
        ];
        for c in cases {
            let v = c.to_json();
            assert_eq!(RoomRequest::parse(v["type"].as_str().unwrap(), &v).unwrap(), c);
        }
        assert_eq!(
            RoomRequest::parse(ROOM_CREATE, &json!({})).unwrap(),
            RoomRequest::Create { public: true, ruleset: None, remembered: false }
        );
    }

    #[test]
    fn bad_requests_are_errors() {
        let bad = [
            (ROOM_SLOT, json!({"slot": 0, "open": true})),
            (ROOM_SLOT, json!({"slot": 5, "open": true})),
            (ROOM_SLOT, json!({"slot": 2})),
            (ROOM_SLOT, json!({"slot": "2", "open": true})),
            (ROOM_TEAMS, json!({"on": "yes"})),
            (ROOM_RULES, json!({})),
            (ROOM_RULES, json!({"rules": {"stocks": 0}})),
            (ROOM_RULES, json!({"rules": {"stonks": 4}})),
            (ROOM_RULES, json!({"rules": "stock"})),
            (ROOM_RULESET, json!({"code": "KFQB"})),
            (ROOM_RULESET, json!({"code": 7})),
            (ROOM_CREATE, json!({"ruleset": "AEIOUY"})),
            (ROOM_RESULT, json!({"game": 0, "winnerSide": 0, "stage": 1})),
            (ROOM_RESULT, json!({"game": 1, "winnerSide": 2, "stage": 1})),
            (ROOM_SET, json!({"action": "pause"})),
            (ROOM_PUBLIC, json!({})),
            (ROOM_READY, json!({"ready": true, "character": 256})),
            (ROOM_READY, json!({"ready": true, "costume": -1})),
            (ROOM_TEAM, json!({"team": 3})),
            (ROOM_TEAM, json!({})),
            (ROOM_JOIN, json!({"code": 5})),
            (ROOM_JOIN, json!({"code": "x".repeat(33)})),
            (ROOM_CREATE, json!({"public": 1})),
            ("room-bogus", json!({})),
        ];
        for (kind, v) in bad {
            assert!(RoomRequest::parse(kind, &v).is_err(), "{kind} {v}");
        }
    }

    #[test]
    fn hello_chat_key_is_optional_and_lenient() {
        let parse = |v: serde_json::Value| serde_json::from_value::<Hello>(v).unwrap().chat_key;
        let base = json!({"type": "hello", "user": {"uid": "u", "playKey": "k"}, "appVersion": "0.1.0"});
        assert_eq!(parse(base.clone()), "");
        for (key, want) in [(json!("ab"), "ab"), (json!(5), ""), (json!(null), ""), (json!({"x": 1}), "")] {
            let mut v = base.clone();
            v["chatKey"] = key;
            assert_eq!(parse(v), want);
        }
        let hello = Hello {
            kind: HELLO.into(),
            user: HelloUser { uid: "u".into(), play_key: "k".into() },
            app_version: "0.1.0".into(),
            platform: String::new(),
            chat_key: String::new(),
        };
        assert!(serde_json::to_value(&hello).unwrap().get("chatKey").is_none());
    }

    #[test]
    fn modes() {
        assert_eq!(RoomMode::of(2, true), RoomMode::OneVsOne);
        assert_eq!(RoomMode::of(3, false), RoomMode::Ffa);
        assert_eq!(RoomMode::of(4, true), RoomMode::Teams);
        assert_eq!(serde_json::to_value(RoomMode::OneVsOne).unwrap(), "1v1");
        assert_eq!(serde_json::to_value(RoomStatus::InGame).unwrap(), "in-game");
    }
}
