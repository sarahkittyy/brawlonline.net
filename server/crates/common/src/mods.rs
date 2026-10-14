//! Brawl Mods rooms (`docs/design/brawl-mods.md` §5): rooms for whole-machine rollback sessions
//! of any Brawl build. A mod room has no rules, sets or start: its host's session runs from the
//! moment the room is made and members plug in (their Dolphins join the session P2P). mm only
//! keeps the members, the build they all have, and each member's public P2P endpoint, which it
//! learns from a probe the member's session sends from its own UDP socket (`mods-endpoint`).
//!
//! Requests come on the online connection after `hello`; every member gets `mods-state` after
//! every change.

use serde::{Deserialize, Serialize};

pub const MODS_CREATE: &str = "mods-create";
pub const MODS_JOIN: &str = "mods-join";
pub const MODS_LEAVE: &str = "mods-leave";
pub const MODS_SLOT: &str = "mods-slot";
pub const MODS_KICK: &str = "mods-kick";
pub const MODS_PUBLIC: &str = "mods-public";
pub const MODS_CONTROLLER: &str = "mods-controller";
pub const MODS_MOVE: &str = "mods-move";
pub const MODS_STATE: &str = "mods-state";
pub const MODS_LEFT: &str = "mods-left";
pub const MODS_ERROR: &str = "mods-error";
/// Sent on a connection of its own from the member's P2P socket (no `hello`).
pub const MODS_ENDPOINT: &str = "mods-endpoint";
pub const MODS_ENDPOINT_RESP: &str = "mods-endpoint-resp";

pub const MAX_PLAYERS: usize = 4;
/// A build hash: SHA-256, 64 lowercase hex digits.
pub const BUILD_HASH_LEN: usize = 64;
pub const MAX_BUILD_NAME: usize = 64;

/// The build a room plays (`docs/design/brawl-mods.md` §4).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Build {
    pub hash: String,
    #[serde(default)]
    pub name: String,
}

/// What a member plays with: their GameCube port or their Wii Remote.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "lowercase")]
pub enum Controller {
    #[default]
    Gc,
    Wiimote,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ModsRequest {
    /// A new room, the sender its host in slot 1.
    Create { build: Build, public: bool, controller: Controller },
    /// The lowest open, empty slot of the room with this code, if the build is the same; `slot`
    /// first if it is open and empty (a member back after its connection to mm dropped, whose
    /// session still plays in that slot).
    Join { code: String, build_hash: String, controller: Controller, slot: Option<u8> },
    Leave,
    /// Host: opens or closes a slot (closing an occupied one removes its player).
    Slot { slot: u8, open: bool },
    /// Host: removes the player in a slot (they cannot come back for a while).
    Kick { slot: u8 },
    /// Host: public (listed) or private (joinable by code).
    Public { public: bool },
    /// Any member: what it plays with now (switched mid-game: the room shows it).
    Controller { controller: Controller },
    /// Host: two slots swap players (either may be empty, but not closed and empty). Sent once the
    /// players' sessions made the swap (at an epoch, the same frame on every machine).
    Move { from: u8, to: u8 },
}

fn valid_hash(h: &str) -> bool {
    h.len() == BUILD_HASH_LEN && h.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// A build's display name: printable, at most [`MAX_BUILD_NAME`] characters.
fn clean_name(name: &str) -> String {
    name.chars().filter(|c| !c.is_control()).take(MAX_BUILD_NAME).collect::<String>().trim().to_string()
}

impl ModsRequest {
    pub fn parse(kind: &str, v: &serde_json::Value) -> Result<ModsRequest, String> {
        let flag = |key: &str, default: Option<bool>| -> Result<bool, String> {
            match v.get(key) {
                None | Some(serde_json::Value::Null) => {
                    default.ok_or_else(|| format!("{kind}: `{key}` must be true or false"))
                }
                Some(b) => b.as_bool().ok_or_else(|| format!("{kind}: `{key}` must be true or false")),
            }
        };
        let slot = || -> Result<u8, String> {
            match v.get("slot").and_then(|s| s.as_u64()) {
                Some(n @ 1..=4) => Ok(n as u8),
                _ => Err(format!("{kind}: `slot` must be 1 to 4")),
            }
        };
        let controller = || -> Result<Controller, String> {
            match v.get("controller") {
                None | Some(serde_json::Value::Null) => Ok(Controller::Gc),
                Some(c) => serde_json::from_value(c.clone())
                    .map_err(|_| format!("{kind}: `controller` must be \"gc\" or \"wiimote\"")),
            }
        };
        Ok(match kind {
            MODS_CREATE => {
                let build: Build = v
                    .get("build")
                    .cloned()
                    .and_then(|b| serde_json::from_value(b).ok())
                    .ok_or_else(|| format!("{kind}: `build` must be {{hash, name}}"))?;
                if !valid_hash(&build.hash) {
                    return Err(format!("{kind}: `build.hash` must be 64 lowercase hex digits"));
                }
                ModsRequest::Create {
                    build: Build { hash: build.hash, name: clean_name(&build.name) },
                    public: flag("public", Some(true))?,
                    controller: controller()?,
                }
            }
            MODS_JOIN => {
                let code = v.get("code").and_then(|c| c.as_str()).ok_or_else(|| format!("{kind}: `code` is missing"))?;
                let build_hash = v
                    .get("build")
                    .and_then(|b| b.get("hash"))
                    .and_then(|h| h.as_str())
                    .filter(|h| valid_hash(h))
                    .ok_or_else(|| format!("{kind}: `build.hash` must be 64 lowercase hex digits"))?;
                ModsRequest::Join {
                    code: code.chars().take(16).collect(),
                    build_hash: build_hash.to_string(),
                    controller: controller()?,
                    slot: match v.get("slot") {
                        None | Some(serde_json::Value::Null) => None,
                        Some(_) => Some(slot()?),
                    },
                }
            }
            MODS_LEAVE => ModsRequest::Leave,
            MODS_SLOT => ModsRequest::Slot { slot: slot()?, open: flag("open", None)? },
            MODS_KICK => ModsRequest::Kick { slot: slot()? },
            MODS_PUBLIC => ModsRequest::Public { public: flag("public", None)? },
            MODS_CONTROLLER => ModsRequest::Controller { controller: controller()? },
            MODS_MOVE => {
                let n = |key: &str| -> Result<u8, String> {
                    match v.get(key).and_then(|s| s.as_u64()) {
                        Some(n @ 1..=4) => Ok(n as u8),
                        _ => Err(format!("{kind}: `{key}` must be 1 to 4")),
                    }
                };
                let (from, to) = (n("from")?, n("to")?);
                if from == to {
                    return Err(format!("{kind}: `from` and `to` must differ"));
                }
                ModsRequest::Move { from, to }
            }
            _ => return Err(format!("unknown request `{kind}`")),
        })
    }
}

/// A member's public P2P endpoint, as mm saw the probe come from it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Endpoint {
    pub host: String,
    pub port: u16,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModsSlot {
    pub slot: u8,
    pub open: bool,
    /// The member in it, if any.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub player: Option<ModsPlayer>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModsPlayer {
    pub display_name: String,
    pub connect_code: String,
    pub controller: Controller,
    /// Null until the member's session probed mm from its P2P socket.
    pub endpoint: Option<Endpoint>,
    /// The member's LAN address and P2P port, as its session reported it: a member with the same
    /// public address (the same network) connects to this one instead (as Slippi's matchmaking).
    pub lan: Option<Endpoint>,
}

/// `mods-state`, to every member after every change (personalised by `you` and `token`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModsState {
    #[serde(rename = "type")]
    pub kind: String,
    pub code: String,
    pub public: bool,
    pub build: Build,
    /// The host's slot (1-4).
    pub host: u8,
    /// The receiver's slot (1-4).
    pub you: u8,
    /// The receiver's endpoint token: its session sends it in `mods-endpoint` from its P2P socket.
    pub token: String,
    pub slots: Vec<ModsSlot>,
}

/// `mods-endpoint`, from the member's P2P socket.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EndpointProbe {
    #[serde(rename = "type")]
    pub kind: String,
    pub token: String,
    /// The session's LAN address and port (`host` an IPv4 or IPv6 address, not a name).
    #[serde(default)]
    pub lan: Option<Endpoint>,
}

impl EndpointProbe {
    /// The LAN endpoint if it is an address and a port (anything else is dropped).
    pub fn lan_addr(&self) -> Option<std::net::SocketAddr> {
        let lan = self.lan.as_ref()?;
        let ip: std::net::IpAddr = lan.host.parse().ok()?;
        (lan.port != 0 && !ip.is_unspecified() && !ip.is_multicast()).then(|| std::net::SocketAddr::new(ip, lan.port))
    }
}

/// A public mod room for the launcher's list.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PublicModRoom {
    pub code: String,
    pub build: Build,
    pub host_name: String,
    pub players: u8,
    pub open: u8,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const HASH: &str = "3f9a1c07aa00000000000000000000000000000000000000000000000000beef";

    #[test]
    fn parses_requests() {
        let r = ModsRequest::parse(MODS_CREATE, &json!({"build": {"hash": HASH, "name": "Brawl Minus 5.1"}})).unwrap();
        assert_eq!(
            r,
            ModsRequest::Create {
                build: Build { hash: HASH.into(), name: "Brawl Minus 5.1".into() },
                public: true,
                controller: Controller::Gc
            }
        );
        let r = ModsRequest::parse(MODS_JOIN, &json!({"code": "KFQB", "build": {"hash": HASH}, "controller": "wiimote"}))
            .unwrap();
        assert_eq!(r, ModsRequest::Join { code: "KFQB".into(), build_hash: HASH.into(), controller: Controller::Wiimote, slot: None });
        let r = ModsRequest::parse(MODS_JOIN, &json!({"code": "KFQB", "build": {"hash": HASH}, "slot": 3})).unwrap();
        assert!(matches!(r, ModsRequest::Join { slot: Some(3), .. }));
        assert!(ModsRequest::parse(MODS_JOIN, &json!({"code": "KFQB", "build": {"hash": HASH}, "slot": 0})).is_err());
        assert_eq!(ModsRequest::parse(MODS_KICK, &json!({"slot": 3})).unwrap(), ModsRequest::Kick { slot: 3 });
    }

    #[test]
    fn refuses_bad_requests() {
        assert!(ModsRequest::parse(MODS_CREATE, &json!({"build": {"hash": "nothex"}})).is_err());
        assert!(ModsRequest::parse(MODS_CREATE, &json!({"build": {"hash": HASH.to_uppercase()}})).is_err());
        assert!(ModsRequest::parse(MODS_JOIN, &json!({"code": "KFQB"})).is_err());
        assert!(ModsRequest::parse(MODS_SLOT, &json!({"slot": 5, "open": true})).is_err());
        assert!(ModsRequest::parse(MODS_SLOT, &json!({"slot": 2})).is_err());
        assert!(ModsRequest::parse(MODS_CREATE, &json!({"build": {"hash": HASH}, "controller": "keyboard"})).is_err());
        assert!(ModsRequest::parse("mods-start", &json!({})).is_err());
    }

    #[test]
    fn probe_lan_addresses() {
        let p = |lan: serde_json::Value| -> EndpointProbe {
            serde_json::from_value(json!({"type": MODS_ENDPOINT, "token": "t", "lan": lan})).unwrap()
        };
        assert_eq!(p(json!({"host": "192.168.0.5", "port": 5000})).lan_addr(), Some("192.168.0.5:5000".parse().unwrap()));
        assert_eq!(p(json!({"host": "example.com", "port": 5000})).lan_addr(), None);
        assert_eq!(p(json!({"host": "0.0.0.0", "port": 5000})).lan_addr(), None);
        assert_eq!(p(json!({"host": "192.168.0.5", "port": 0})).lan_addr(), None);
        assert_eq!(p(serde_json::Value::Null).lan_addr(), None);
    }

    #[test]
    fn build_names_are_cleaned() {
        let r = ModsRequest::parse(MODS_CREATE, &json!({"build": {"hash": HASH, "name": " a\u{0007}b \n".to_string() + &"x".repeat(100)}}))
            .unwrap();
        let ModsRequest::Create { build, .. } = r else { panic!() };
        assert!(build.name.starts_with("ab"));
        assert!(build.name.chars().count() <= MAX_BUILD_NAME);
    }
}
