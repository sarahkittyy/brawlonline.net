//! Per-mode rules the server hands to clients.
//!
//! `stages` travels in the mm protocol as `get-ticket-resp.stages`, exactly where Slippi's server
//! puts it (`SlippiMatchmaking.cpp:625-641`), and `items` as `items`. The rest (stocks, timer) is
//! kept here so Ranked (a later phase) reads the same file. The user chose the P+ competitive
//! ruleset: 4 stocks, 8 minutes, items off, P+'s legal stages.
//!
//! **Stage ids** are Brawl's `srStageKind` values, the ids the game's own match setup and P+'s
//! stage file loader key on (`docs/game-code.md` section 11, "Stages"). The legal list in
//! `config/rulesets.json` is P+ v3.2's own: the 15 stages its random-stage switch "Default" preset
//! (`/Project+/pf/stage/switch/Switch00.rss`) turns on:
//!
//! | id | stage | id | stage | id | stage |
//! |---|---|---|---|---|---|
//! | 0x01 (1) | Battlefield | 0x06 (6) | Bowser's Castle | 0x1C (28) | Wario Land |
//! | 0x02 (2) | Final Destination | 0x09 (9) | Temple of Time | 0x1F (31) | Fountain of Dreams |
//! | 0x03 (3) | Delfino's Secret | 0x0C (12) | Frigate Husk | 0x21 (33) | Smashville |
//! | 0x04 (4) | Luigi's Mansion | 0x0D (13) | Yoshi's Island | 0x23 (35) | Green Hill Zone |
//! | 0x05 (5) | Metal Cavern | | | 0x2D (45) | Dream Land |
//! | | | | | 0x2E (46) | Pokémon Stadium 2 |
//!
//! How the client uses the list (as Slippi's does, `EXI_DeviceSlippi.cpp:2134-2153, 2707-2722`):
//! Unranked and Ranked (fixed-rules modes) draw every game's stage from it; Direct draws game 1
//! from it, and the loser's pick for later games must be in it (design 5.4, screen 5). An empty
//! list makes the client fall back to its built-in copy of the same legal list.

use std::collections::{HashMap, HashSet};

use serde::{Deserialize, Serialize};

/// `srStageKind` values fit in a byte; a larger id in the file is a typo.
pub const MAX_STAGE_ID: u16 = 0xFF;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct ModeRules {
    #[serde(default)]
    pub stages: Vec<u16>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub stocks: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub time_minutes: Option<u8>,
    /// Item bitfield sent as `items` (0 = items off).
    #[serde(default)]
    pub items: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
pub struct Rulesets {
    #[serde(flatten)]
    pub modes: HashMap<String, ModeRules>,
}

/// The repository's `config/rulesets.json`, used when no file is configured.
pub const DEFAULT_RULESETS: &str = include_str!("../../../config/rulesets.json");

impl Rulesets {
    /// Parses and checks a rulesets file: known mode names, stage ids in range, no duplicates.
    pub fn parse(json: &str) -> anyhow::Result<Self> {
        let r: Rulesets = serde_json::from_str(json)?;
        for (name, rules) in &r.modes {
            anyhow::ensure!(
                ["ranked", "unranked", "direct", "teams", "party"].contains(&name.as_str()),
                "unknown mode {name:?} in rulesets"
            );
            let mut seen = HashSet::new();
            for &s in &rules.stages {
                anyhow::ensure!(s <= MAX_STAGE_ID, "{name}: stage id {s} out of range");
                anyhow::ensure!(seen.insert(s), "{name}: stage id {s} listed twice");
            }
        }
        Ok(r)
    }

    pub fn load(path: Option<&str>) -> anyhow::Result<Self> {
        match path {
            Some(p) if !p.is_empty() => Self::parse(&std::fs::read_to_string(p)?),
            _ => Self::parse(DEFAULT_RULESETS),
        }
    }

    pub fn for_mode(&self, mode: common::proto::Mode) -> ModeRules {
        self.modes.get(mode.name()).cloned().unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use common::proto::Mode;

    /// P+ v3.2's legal stages as srStageKind (docs/game-code.md section 11; Dolphin's
    /// `Gprb::Session::DefaultStages()` has the same list).
    const PPLUS_LEGAL: [u16; 15] =
        [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0C, 0x0D, 0x1C, 0x1F, 0x21, 0x23, 0x2D, 0x2E];

    #[test]
    fn default_file_has_the_pplus_ruleset() {
        let r = Rulesets::parse(DEFAULT_RULESETS).unwrap();
        for mode in [Mode::Direct, Mode::Unranked, Mode::Ranked] {
            let m = r.for_mode(mode);
            assert_eq!(m.stages, PPLUS_LEGAL, "{mode:?}");
            assert_eq!((m.stocks, m.time_minutes, m.items), (Some(4), Some(8), 0), "{mode:?}");
        }
        assert_eq!(r.for_mode(Mode::Party), ModeRules::default());
        assert_eq!(r.for_mode(Mode::Teams), ModeRules::default());
    }

    #[test]
    fn bad_files_are_refused() {
        for bad in [
            r#"{"unranked": {"stages": [1, 1]}}"#,
            r#"{"unranked": {"stages": [256]}}"#,
            r#"{"unrnaked": {"stages": [1]}}"#,
            r#"{"unranked": {"stages": "1,2"}}"#,
        ] {
            assert!(Rulesets::parse(bad).is_err(), "{bad}");
        }
        assert!(Rulesets::load(Some("does/not/exist.json")).is_err());
        assert_eq!(Rulesets::load(None).unwrap(), Rulesets::parse(DEFAULT_RULESETS).unwrap());
    }
}
