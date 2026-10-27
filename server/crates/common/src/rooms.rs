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
pub const ROOM_SIT_OUT: &str = "room-sit-out";
pub const ROOM_WATCH: &str = "room-watch";
pub const ROOM_RULES: &str = "room-rules";
pub const ROOM_RESULT: &str = "room-result";
pub const ROOM_SET: &str = "room-set";
pub const ROOM_STEP: &str = "room-step";
pub const ROOM_BEST_OF: &str = "room-best-of";
pub const ROOM_STAGES: &str = "room-stages";
/// Client → server and server → every member: players of the room's game that could not connect
/// to each other.
pub const ROOM_UNREACHABLE: &str = "room-unreachable";
/// A side's stage actions in one `room-step` at most (a strike pattern of 8 steps of up to 8).
pub const STEP_ACTIONS_MAX: usize = 64;
pub const ROOM_RULESET: &str = "room-ruleset";
pub const ROOM_RULESETS: &str = "room-rulesets";
pub const RULESET_LIST: &str = "ruleset-list";
/// `room-result.winnerSide` of a draw.
pub const SIDE_DRAW: u8 = 0xFE;
/// `room-result.winnerSide` of a game that was quit (the pause screen's quit, L+R+A+START): it did
/// not happen. Once every player of the set reported it, the set plays that game again from its
/// first step (`docs/design/room-rules.md` §1.1, quits in a set).
pub const SIDE_VOID: u8 = 0xFD;
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
    /// The game sends `game-alive` ([`crate::proto::GAME_ALIVE`]) while its emulation runs (bug
    /// report #105, 2026-10-22): mm takes a connection that sends none for a while as a frozen
    /// game. Builds before it leave it out and are never taken so.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub game_alive: bool,
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
    /// `room-teams {on, colors?}` (host only). `colors` (2026-10-21; missing: false): with `on`,
    /// P+'s plain team battle, every player in their team's costume; else its rainbow team battle
    /// (any costume, a team glow on each fighter).
    Teams { on: bool, colors: bool },
    /// `room-public {public}` (host only).
    Public { public: bool },
    /// `room-ready {ready, character?, costume?, stocks?}`: START on the CSS (lock-in) or taking it
    /// back. `stocks`: with the rules' stock control, the count the player picked on their panel
    /// (1-99; missing: none picked, the rules' stocks). With `ready: false` the character and
    /// costume are the coin placed on the CSS (missing: the coin is in the hand), shown to the
    /// others before the player readies.
    Ready { ready: bool, character: Option<u8>, costume: Option<u8>, stocks: Option<u8> },
    /// `room-sit-out {slot, sitOut}`: the player in slot 1-4 sits out the room's next games (the
    /// others start without them) or plays again. A player for themselves, the host for anyone.
    SitOut { slot: u8, sit_out: bool },
    /// `room-watch {want?, watching?}` (2026-10-19, `docs/design/rooms.md` §1.3): `want`: send this
    /// member the room's games they are not in (the watch channel, [`crate::spectate::WATCH_CHANNEL`]),
    /// from now on; false stops the one being sent (the player left it early) until it is sent
    /// again. `watching`: this member's game shows one of them now: the room's next game does not
    /// wait for them. Absent: unchanged.
    Watch { want: Option<bool>, watching: Option<bool> },
    /// `room-team {team}`: 0 red, 1 blue, 2 green.
    Team { team: u8 },
    /// `room-back {pickers?}`: this player is back on the room's CSS after a game. `pickers`: the
    /// ports (bits 0-3) that pick the next game's stage as this player's game saw the game end (the
    /// last game's loser), which the server carries to the next game's players.
    Back { pickers: Option<u8> },
    /// `room-rules {rules}` (host only, between games): the room's match rules
    /// (`docs/design/room-rules.md`), checked by `MatchRules::validate`.
    Rules { rules: crate::rules::MatchRules },
    /// `room-result {game, winnerSide, stage}`: a game of the room's set as this member's game saw
    /// it end (`winnerSide` 0 / 1, [`SIDE_DRAW`], [`SIDE_VOID`] a quit game; `stage` the srStageKind
    /// played). The server counts it once every player of the set reported the same.
    Result { game: u32, winner_side: u8, stage: u16 },
    /// `room-set {action}` (host, between games): `resume` replays the game whose results differed,
    /// `restart` ends any set that is on with no result (RULES > SET > End set; the next start
    /// begins a new one).
    Set { resume: bool },
    /// `room-step {game, actions, matchStarted}`: the set's game being set up as this member's game
    /// sees it: each side's struck / banned / picked stages in order, and whether its match has
    /// started. The server keeps them, so a session that starts again after a player left goes on
    /// from the same step; leaving once the match started loses the game (room-rules.md A).
    Step { game: u32, actions: [Vec<u8>; 2], match_started: bool },
    /// `room-best-of {bestOf}` (host, between games, no set on): the room plays sets of 3, 5 or 7
    /// games, or none (0). Like a rules edit it makes the room's ruleset custom.
    BestOf { best_of: u8 },
    /// `room-stages {stages, stageChoice?}` (host, between games, no set on): the room's stage
    /// lists as the host left the game's STAGE SWITCH, and whether the loser picks from them
    /// (`loser-picks` / `loser-picks-list`; absent: as it is). Like a rules edit it makes the
    /// room's ruleset custom.
    Stages { stages: crate::rules::StageLists, stage_choice: Option<crate::rules::StageChoice> },
    /// `room-ruleset {code}` (host, between games, no set on): play a published ruleset (a code),
    /// or P+'s default (`code` null). The code also becomes the host's remembered ruleset.
    Ruleset { code: Option<String> },
    /// `room-rulesets {}`: the rulesets to offer this player in the game's Ruleset line (their
    /// recent ones, then their own published ones), answered with `ruleset-list`.
    RulesetList,
    /// `room-unreachable {matchId, slots}`: this member's connect to these members (slots 1-4)
    /// for the room's game `matchId` timed out. The server tells the whole room each pair once
    /// ([`RoomUnreachable`]). No answer.
    Unreachable { match_id: String, slots: Vec<u8> },
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
            RoomRequest::SitOut { .. } => ROOM_SIT_OUT,
            RoomRequest::Watch { .. } => ROOM_WATCH,
            RoomRequest::Back { .. } => ROOM_BACK,
            RoomRequest::Rules { .. } => ROOM_RULES,
            RoomRequest::Result { .. } => ROOM_RESULT,
            RoomRequest::Set { .. } => ROOM_SET,
            RoomRequest::Step { .. } => ROOM_STEP,
            RoomRequest::BestOf { .. } => ROOM_BEST_OF,
            RoomRequest::Stages { .. } => ROOM_STAGES,
            RoomRequest::Ruleset { .. } => ROOM_RULESET,
            RoomRequest::RulesetList => ROOM_RULESETS,
            RoomRequest::Unreachable { .. } => ROOM_UNREACHABLE,
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
            RoomRequest::Leave | RoomRequest::RulesetList => json!({}),
            RoomRequest::Back { pickers } => match pickers {
                Some(p) => json!({ "pickers": p }),
                None => json!({}),
            },
            RoomRequest::SitOut { slot, sit_out } => json!({ "slot": slot, "sitOut": sit_out }),
            RoomRequest::Watch { want, watching } => {
                let mut v = json!({});
                if let Some(w) = want {
                    v["want"] = json!(w);
                }
                if let Some(w) = watching {
                    v["watching"] = json!(w);
                }
                v
            }
            RoomRequest::Slot { slot, open } => json!({ "slot": slot, "open": open }),
            RoomRequest::Teams { on, colors } => json!({ "on": on, "colors": colors }),
            RoomRequest::Public { public } => json!({ "public": public }),
            RoomRequest::Ready { ready, character, costume, stocks } => {
                let mut v = json!({ "ready": ready });
                if let Some(c) = character {
                    v["character"] = json!(c);
                }
                if let Some(c) = costume {
                    v["costume"] = json!(c);
                }
                if let Some(n) = stocks {
                    v["stocks"] = json!(n);
                }
                v
            }
            RoomRequest::Team { team } => json!({ "team": team }),
            RoomRequest::Rules { rules } => json!({ "rules": rules }),
            RoomRequest::Result { game, winner_side, stage } => {
                json!({ "game": game, "winnerSide": winner_side, "stage": stage })
            }
            RoomRequest::Set { resume } => json!({ "action": if *resume { "resume" } else { "restart" } }),
            RoomRequest::Step { game, actions, match_started } => {
                json!({ "game": game, "actions": actions, "matchStarted": match_started })
            }
            RoomRequest::BestOf { best_of } => json!({ "bestOf": best_of }),
            RoomRequest::Stages { stages, stage_choice } => match stage_choice {
                Some(c) => json!({ "stages": stages, "stageChoice": c }),
                None => json!({ "stages": stages }),
            },
            RoomRequest::Ruleset { code } => json!({ "code": code }),
            RoomRequest::Unreachable { match_id, slots } => json!({ "matchId": match_id, "slots": slots }),
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
            ROOM_TEAMS => RoomRequest::Teams {
                on: flag("on")?,
                colors: match v.get("colors") {
                    None | Some(serde_json::Value::Null) => false,
                    Some(_) => flag("colors")?,
                },
            },
            ROOM_PUBLIC => RoomRequest::Public { public: flag("public")? },
            ROOM_READY => RoomRequest::Ready {
                ready: flag("ready")?,
                character: small("character", 255)?,
                costume: small("costume", 255)?,
                stocks: match small("stocks", crate::rules::MAX_PICKED_STOCKS as u64)? {
                    Some(0) => return Err(format!("{kind}: `stocks` must be 1 to {}", crate::rules::MAX_PICKED_STOCKS)),
                    n => n,
                },
            },
            ROOM_TEAM => RoomRequest::Team {
                team: small("team", (TEAM_COUNT - 1) as u64)?.ok_or_else(|| format!("{kind}: `team` is missing"))?,
            },
            ROOM_BACK => RoomRequest::Back { pickers: small("pickers", 0x0F)? },
            ROOM_SIT_OUT => {
                let slot = small("slot", ROOM_SLOTS as u64)?
                    .filter(|s| *s >= 1)
                    .ok_or_else(|| format!("{kind}: `slot` must be 1 to {ROOM_SLOTS}"))?;
                RoomRequest::SitOut { slot, sit_out: flag("sitOut")? }
            }
            ROOM_WATCH => {
                let opt = |key: &str| match v.get(key) {
                    None | Some(serde_json::Value::Null) => Ok(None),
                    Some(_) => flag(key).map(Some),
                };
                RoomRequest::Watch { want: opt("want")?, watching: opt("watching")? }
            }
            ROOM_RULES => {
                let raw = v.get("rules").ok_or_else(|| format!("{kind}: `rules` is missing"))?;
                let rules: crate::rules::MatchRules =
                    serde_json::from_value(raw.clone()).map_err(|e| format!("{kind}: {e}"))?;
                rules.validate().map_err(|e| format!("{kind}: {e}"))?;
                RoomRequest::Rules { rules }
            }
            ROOM_BEST_OF => RoomRequest::BestOf {
                best_of: small("bestOf", 7)?
                    .filter(|b| [0, 3, 5, 7].contains(b))
                    .ok_or_else(|| format!("{kind}: `bestOf` must be 0, 3, 5 or 7"))?,
            },
            ROOM_STEP => {
                let game = v
                    .get("game")
                    .and_then(|g| g.as_u64())
                    .filter(|g| (1..=99).contains(g))
                    .ok_or_else(|| format!("{kind}: `game` must be 1 to 99"))? as u32;
                let bad = || format!("{kind}: `actions` must be two lists of stage ids");
                let lists = v.get("actions").and_then(|a| a.as_array()).filter(|a| a.len() == 2).ok_or_else(bad)?;
                let mut actions: [Vec<u8>; 2] = Default::default();
                for (side, list) in lists.iter().enumerate() {
                    let list = list.as_array().filter(|l| l.len() <= STEP_ACTIONS_MAX).ok_or_else(bad)?;
                    for k in list {
                        let k = k.as_u64().filter(|k| (1..=0xFF).contains(k)).ok_or_else(bad)?;
                        actions[side].push(k as u8);
                    }
                }
                let match_started = match v.get("matchStarted") {
                    None => false,
                    Some(b) => b.as_bool().ok_or_else(|| format!("{kind}: `matchStarted` must be true or false"))?,
                };
                RoomRequest::Step { game, actions, match_started }
            }
            ROOM_RESULT => {
                let game = v
                    .get("game")
                    .and_then(|g| g.as_u64())
                    .filter(|g| (1..=99).contains(g))
                    .ok_or_else(|| format!("{kind}: `game` must be 1 to 99"))? as u32;
                let winner_side = small("winnerSide", SIDE_DRAW as u64)?
                    .filter(|s| *s <= 1 || *s == SIDE_DRAW || *s == SIDE_VOID)
                    .ok_or_else(|| format!("{kind}: `winnerSide` must be 0, 1, {SIDE_DRAW} or {SIDE_VOID}"))?;
                let stage = v
                    .get("stage")
                    .and_then(|s| s.as_u64())
                    .filter(|s| *s <= 0xFF)
                    .ok_or_else(|| format!("{kind}: `stage` must be a stage id"))? as u16;
                RoomRequest::Result { game, winner_side, stage }
            }
            ROOM_RULESETS => RoomRequest::RulesetList,
            ROOM_STAGES => {
                let raw = v.get("stages").ok_or_else(|| format!("{kind}: `stages` is missing"))?;
                let stages: crate::rules::StageLists =
                    serde_json::from_value(raw.clone()).map_err(|e| format!("{kind}: {e}"))?;
                let mut seen = std::collections::HashSet::new();
                for s in stages.all() {
                    if s == 0 || s > crate::rules::MAX_STAGE_ID || !seen.insert(s) {
                        return Err(format!("{kind}: `stages` must list stage ids 1-255 once each"));
                    }
                }
                let stage_choice = match v.get("stageChoice") {
                    None | Some(serde_json::Value::Null) => None,
                    Some(c) => Some(serde_json::from_value(c.clone()).map_err(|e| format!("{kind}: {e}"))?),
                };
                RoomRequest::Stages { stages, stage_choice }
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
            ROOM_UNREACHABLE => {
                let match_id = v
                    .get("matchId")
                    .and_then(|m| m.as_str())
                    .filter(|m| !m.is_empty() && m.len() <= 128)
                    .ok_or_else(|| format!("{kind}: `matchId` must be a match id"))?
                    .to_string();
                let bad = || format!("{kind}: `slots` must list 1 to {} slots", ROOM_SLOTS - 1);
                let list = v
                    .get("slots")
                    .and_then(|l| l.as_array())
                    .filter(|l| (1..ROOM_SLOTS).contains(&l.len()))
                    .ok_or_else(bad)?;
                let mut slots = Vec::new();
                for x in list {
                    let n = x.as_u64().filter(|n| (1..=ROOM_SLOTS as u64).contains(n)).ok_or_else(bad)?;
                    slots.push(n as u8);
                }
                RoomRequest::Unreachable { match_id, slots }
            }
            other => return Err(format!("unknown room message {other:?}")),
        })
    }
}

/// How a room plays, from the number of players and the Teams switch: two are 1v1 whatever the
/// switch says (`docs/design/rooms.md` #12). The server counts the players who play (not sitting
/// out), or the open slots while fewer than two do.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum RoomMode {
    #[serde(rename = "1v1")]
    OneVsOne,
    Ffa,
    Teams,
}

impl RoomMode {
    pub fn of(players: usize, teams: bool) -> RoomMode {
        if players <= 2 {
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
    /// With the rules' stock control: the stock count the player picked on their panel, shown as
    /// the lock-in is (missing: none picked, or not shown yet).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub stocks: Option<u8>,
    /// Still in the last game (not back on the room's CSS).
    pub in_game: bool,
    /// Sits out the room's games (the panel's SIT OUT), by choice or outside a set that is on.
    #[serde(default)]
    pub sitting_out: bool,
    /// Watching the room's game in their own game (`room-watch` `watching`, 2026-10-19): the next
    /// game does not wait for them. Written only when true.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub watching: bool,
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
    /// With `teams`: every player in their team's costume (P+'s plain team battle); else P+'s
    /// rainbow team battle. Absent from servers before 2026-10-21.
    #[serde(default)]
    pub team_colors: bool,
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
    /// The ports (bits 0-3) that pick the next game's stage (the loser picks): the last game's
    /// loser when they play it, else one of its players drawn once a game. Absent: the players'
    /// games decide (an older mm, no report yet, or a ruleset that leaves no stage to the loser).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pickers: Option<u8>,
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
    pub best_of: u8,
    pub stage_choice: crate::rules::StageChoice,
    /// The ruleset's stages (an older mm sent none: P+'s default lists).
    #[serde(default)]
    pub stages: crate::rules::StageLists,
    /// The picks play P+'s alternate stages (`Ruleset::alternate_stages`).
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub alternate_stages: bool,
}

/// A room's set (`room-state.set`): the score by side, side 0 being the lower slot.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomSet {
    pub best_of: u8,
    /// The game being played or next (1-based).
    pub game: u32,
    pub score: [u8; 2],
    /// Each side's first (lowest) slot, 1-4.
    pub sides: [u8; 2],
    /// Each side's slots (1-4): one player in 1v1, a team in 2v2.
    #[serde(default)]
    pub members: [Vec<u8>; 2],
    /// A player of the set left (or the results differed): the set waits.
    pub paused: bool,
    /// The missing players' names while they are away (the set goes on when they are back).
    pub waiting_for: Option<String>,
    /// The host may restart the game (the players' results differed, everyone is here).
    pub can_resume: bool,
    /// Someone won it (the score stays until the next set starts).
    pub finished: bool,
    /// The side that won it, once finished.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub winner: Option<u8>,
}

/// One ruleset in `ruleset-list`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RulesetEntry {
    pub code: String,
    pub name: String,
    pub version: u32,
    pub by: String,
    pub best_of: u8,
}

/// `ruleset-list`, server → the player who asked (`room-rulesets`): their recent rulesets, newest
/// first, then their own published ones, at most [`RULESET_LIST_MAX`].
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RulesetList {
    #[serde(rename = "type")]
    pub kind: String,
    pub rulesets: Vec<RulesetEntry>,
}

/// The game shows at most this many (the mailbox's payload).
pub const RULESET_LIST_MAX: usize = 15;

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
    /// The slots (1-4) that play this game; the others sit it out and send no ticket. Empty from
    /// an older mm: every member.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub players: Vec<u8>,
    /// The game's host (the session's decider): the room's host when it plays, else the
    /// earliest-joined player of the game.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub host: Option<u8>,
    /// The ports (bits 0-3) that pick this game's stage: the last game's loser when they play,
    /// else one player of this game drawn at random. Absent: the players' own (an older mm).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pickers: Option<u8>,
}

/// `room-unreachable`, server → every member: the pairs of the room's game `match_id` (slots,
/// lower first) whose connect to each other timed out, as reported so far; sent when one is new.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomUnreachable {
    #[serde(rename = "type")]
    pub kind: String,
    pub code: String,
    pub match_id: String,
    pub pairs: Vec<[u8; 2]>,
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
    /// This mm takes `game-alive` ([`crate::proto::GAME_ALIVE`]): a game sends it only then (an
    /// older mm answers an unknown message with a `room-error`, which the game shows).
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub game_alive: bool,
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
    /// What the room plays (its ruleset's code, name and set), for the room list.
    #[serde(default)]
    pub ruleset: RoomRuleset,
    /// The name its host gave it (`users.room_name`, `docs/design/rooms.md` §1.4); `None`: the
    /// launcher shows "alice's room". Absent from an older mm.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
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
    /// Players searching Unranked and Ranked (the launcher's Play buttons). Absent from an older
    /// mm: zero.
    #[serde(default)]
    pub queues: QueueCounts,
    /// Players in an Unranked and a Ranked match (from the pairing until their game leaves the
    /// online CSS, as long as the opponent is still there), shown with the queues. Absent from an
    /// older mm: zero.
    #[serde(default)]
    pub playing: QueueCounts,
    /// RFC 3339 time of the snapshot.
    pub updated_at: String,
    /// Public Brawl Mods rooms (`docs/design/brawl-mods.md` §5), for the launcher's Brawl Mods
    /// tab: fuller first. Absent from an older mm: none.
    #[serde(default)]
    pub mod_rooms: Vec<crate::mods::PublicModRoom>,
    /// Every Brawl Mods room, private and full ones too. Internal: accounts looks a room up by its
    /// exact code (`GET /v1/mod-rooms/{code}`: which game a code needs) and never lists it.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub mod_room_index: Vec<crate::mods::PublicModRoom>,
    /// Every room player's room, public and private rooms alike. Internal: accounts reads it to
    /// check and address launcher invites (`docs/social-protocol.md`) and never serves it to
    /// launchers (`GET /v1/rooms` sends only the fields above).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub members: Vec<RoomMember>,
    /// The live games spectators can watch (`docs/spectate-protocol.md` §2), newest first.
    /// accounts serves them as `GET /v1/live`, never with `/v1/rooms`.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub live: Vec<crate::spectate::LiveEntry>,
    /// Every Brawl Mods room player's room. Internal, as [`MmStatus::members`]: accounts reads it
    /// for [`MmStatus::your_room`].
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub mod_members: Vec<ModRoomMember>,
    /// The room the asking player is in (P+ or Brawl Mods, public or private), so their launcher
    /// pins it and lets its host name it. Only accounts fills it in, per launcher, from
    /// [`MmStatus::members`] and [`MmStatus::mod_members`]; mm never sends it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub your_room: Option<YourRoom>,
}

/// The longest room name, in characters (`users.room_name`'s check).
pub const MAX_ROOM_NAME_LEN: usize = 32;

/// A room name as typed, made what is stored: spaces at the ends dropped and runs of them made
/// one; empty is `None` (no name: "alice's room"). Control characters, and invisible ones that
/// reorder or hide text, are refused, as are names over [`MAX_ROOM_NAME_LEN`].
pub fn clean_room_name(typed: &str) -> Result<Option<String>, &'static str> {
    let name = typed.split_whitespace().collect::<Vec<_>>().join(" ");
    if name.is_empty() {
        return Ok(None);
    }
    let hidden = |c: char| {
        c.is_control()
            || matches!(c, '\u{200B}'..='\u{200F}' | '\u{202A}'..='\u{202E}' | '\u{2060}'..='\u{206F}' | '\u{FEFF}')
    };
    if name.chars().any(hidden) {
        return Err("Room names can't have that character.");
    }
    if name.chars().count() > MAX_ROOM_NAME_LEN {
        return Err("Room names are at most 32 characters.");
    }
    Ok(Some(name))
}

/// One Brawl Mods room player's room, for [`MmStatus::mod_members`].
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModRoomMember {
    /// The player's account (hyphenated lowercase UUID).
    pub uid: String,
    pub code: String,
    /// The player hosts the room.
    pub host: bool,
    pub name: Option<String>,
}

/// Which kind of room [`YourRoom`] is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum RoomKind {
    /// A P+ room (`rooms.rs`).
    Pplus,
    /// A Brawl Mods (Other Games) room (`mod_rooms.rs`).
    Mods,
}

/// [`MmStatus::your_room`].
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct YourRoom {
    pub kind: RoomKind,
    pub code: String,
    /// The player hosts it: only the host names it.
    pub host: bool,
    /// Its name; `None`: none given.
    pub name: Option<String>,
}

/// A count per queue, for [`MmStatus::queues`] and [`MmStatus::playing`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct QueueCounts {
    pub unranked: usize,
    pub ranked: usize,
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
    /// The player hosts the room. Absent from an older mm: false.
    #[serde(default)]
    pub host: bool,
    /// The room's name ([`PublicRoom::name`]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn room_names_are_cleaned() {
        assert_eq!(clean_room_name("  friday   doubles "), Ok(Some("friday doubles".into())));
        assert_eq!(clean_room_name("   "), Ok(None));
        assert_eq!(clean_room_name(""), Ok(None));
        assert_eq!(clean_room_name("Marth dittos only ⚔"), Ok(Some("Marth dittos only ⚔".into())));
        assert_eq!(clean_room_name(&"x".repeat(32)), Ok(Some("x".repeat(32))));
        assert!(clean_room_name(&"x".repeat(33)).is_err());
        assert!(clean_room_name("evil\u{202E}name").is_err());
        assert!(clean_room_name("bell\u{7}").is_err());
    }

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
            RoomRequest::Result { game: 1, winner_side: SIDE_VOID, stage: 0x2E },
            RoomRequest::Step { game: 1, actions: [vec![0x01], vec![0x02, 0x21]], match_started: false },
            RoomRequest::Step { game: 3, actions: [vec![], vec![]], match_started: true },
            RoomRequest::BestOf { best_of: 5 },
            RoomRequest::BestOf { best_of: 0 },
            RoomRequest::Stages {
                stages: crate::rules::StageLists { starters: vec![1, 2], counterpicks: vec![0x2A] },
                stage_choice: Some(crate::rules::StageChoice::LoserPicksList),
            },
            RoomRequest::Stages { stages: crate::rules::StageLists::default(), stage_choice: None },
            RoomRequest::RulesetList,
            RoomRequest::Unreachable { match_id: "mode.room-KFQB-1".into(), slots: vec![4] },
            RoomRequest::Unreachable { match_id: "mode.room-KFQB-1".into(), slots: vec![1, 2, 3] },
            RoomRequest::Join { code: "KFQB".into() },
            RoomRequest::Leave,
            RoomRequest::Slot { slot: 3, open: true },
            RoomRequest::Teams { on: true, colors: false },
            RoomRequest::Teams { on: true, colors: true },
            RoomRequest::Public { public: false },
            RoomRequest::Ready { ready: true, character: Some(12), costume: Some(3), stocks: None },
            RoomRequest::Ready { ready: true, character: Some(12), costume: Some(3), stocks: Some(2) },
            RoomRequest::Ready { ready: false, character: None, costume: None, stocks: None },
            RoomRequest::Team { team: 2 },
            RoomRequest::Back { pickers: None },
            RoomRequest::Back { pickers: Some(2) },
            RoomRequest::SitOut { slot: 3, sit_out: true },
            RoomRequest::SitOut { slot: 1, sit_out: false },
            RoomRequest::Watch { want: Some(true), watching: None },
            RoomRequest::Watch { want: Some(false), watching: Some(false) },
            RoomRequest::Watch { want: None, watching: Some(true) },
            RoomRequest::Ready { ready: false, character: Some(7), costume: Some(1), stocks: None },
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
        // A game from before 2026-10-21: the rainbow team battle.
        assert_eq!(
            RoomRequest::parse(ROOM_TEAMS, &json!({"on": true})).unwrap(),
            RoomRequest::Teams { on: true, colors: false }
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
            (ROOM_TEAMS, json!({"on": true, "colors": 1})),
            (ROOM_SIT_OUT, json!({"slot": 0, "sitOut": true})),
            (ROOM_SIT_OUT, json!({"slot": 2})),
            (ROOM_BACK, json!({"pickers": 16})),
            (ROOM_RULES, json!({})),
            (ROOM_RULES, json!({"rules": {"stocks": 0}})),
            (ROOM_RULES, json!({"rules": {"stonks": 4}})),
            (ROOM_RULES, json!({"rules": "stock"})),
            (ROOM_RULESET, json!({"code": "KFQB"})),
            (ROOM_RULESET, json!({"code": 7})),
            (ROOM_CREATE, json!({"ruleset": "AEIOUY"})),
            (ROOM_RESULT, json!({"game": 0, "winnerSide": 0, "stage": 1})),
            (ROOM_RESULT, json!({"game": 1, "winnerSide": 2, "stage": 1})),
            (ROOM_RESULT, json!({"game": 1, "winnerSide": 252, "stage": 1})),
            (ROOM_SET, json!({"action": "pause"})),
            (ROOM_STEP, json!({"game": 1, "actions": [[1]]})),
            (ROOM_BEST_OF, json!({"bestOf": 1})),
            (ROOM_BEST_OF, json!({"bestOf": 9})),
            (ROOM_BEST_OF, json!({})),
            (ROOM_STAGES, json!({})),
            (ROOM_STAGES, json!({"stages": {"starters": [0]}})),
            (ROOM_STAGES, json!({"stages": {"starters": [1], "counterpicks": [1]}})),
            (ROOM_STAGES, json!({"stages": {"starters": [1], "bans": []}})),
            (ROOM_STAGES, json!({"stages": {"starters": [1]}, "stageChoice": "pick"})),
            (ROOM_STEP, json!({"game": 1, "actions": [[0], []]})),
            (ROOM_STEP, json!({"game": 1, "actions": [[256], []]})),
            (ROOM_STEP, json!({"game": 0, "actions": [[], []]})),
            (ROOM_STEP, json!({"game": 1, "actions": [vec![1; 65], []]})),
            (ROOM_STEP, json!({"game": 1, "actions": [[], []], "matchStarted": 1})),
            (ROOM_PUBLIC, json!({})),
            (ROOM_READY, json!({"ready": true, "character": 256})),
            (ROOM_READY, json!({"ready": true, "costume": -1})),
            (ROOM_READY, json!({"ready": true, "stocks": 0})),
            (ROOM_READY, json!({"ready": true, "stocks": 100})),
            (ROOM_TEAM, json!({"team": 3})),
            (ROOM_TEAM, json!({})),
            (ROOM_JOIN, json!({"code": 5})),
            (ROOM_JOIN, json!({"code": "x".repeat(33)})),
            (ROOM_CREATE, json!({"public": 1})),
            (ROOM_UNREACHABLE, json!({"slots": [4]})),
            (ROOM_UNREACHABLE, json!({"matchId": "", "slots": [4]})),
            (ROOM_UNREACHABLE, json!({"matchId": "m", "slots": []})),
            (ROOM_UNREACHABLE, json!({"matchId": "m", "slots": [1, 2, 3, 4]})),
            (ROOM_UNREACHABLE, json!({"matchId": "m", "slots": [0]})),
            (ROOM_UNREACHABLE, json!({"matchId": "m", "slots": [5]})),
            (ROOM_UNREACHABLE, json!({"matchId": "x".repeat(129), "slots": [4]})),
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
            game_alive: false,
        };
        assert!(serde_json::to_value(&hello).unwrap().get("chatKey").is_none());
        assert!(serde_json::to_value(&hello).unwrap().get("gameAlive").is_none());
        let mut v = base.clone();
        v["gameAlive"] = json!(true);
        assert!(serde_json::from_value::<Hello>(v).unwrap().game_alive);
        assert!(!serde_json::from_value::<Hello>(base.clone()).unwrap().game_alive);
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
