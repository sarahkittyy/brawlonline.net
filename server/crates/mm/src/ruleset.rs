//! Per-mode rules the server hands to clients.
//!
//! `stages` travels in the mm protocol as `get-ticket-resp.stages`, exactly where Slippi's server
//! puts it (`SlippiMatchmaking.cpp:625-641`), `items` as `items`, and `rules` (the match rules
//! every client applies: style, stocks, time, items, hazards, auto L-cancel...,
//! `common::rules::MatchRules`) as `rules`. The user chose the P+ competitive ruleset: 4 stocks,
//! 8 minutes, items off, P+'s legal stages; P+'s normal pause only in Direct (and rooms, which play
//! Direct's rules), elsewhere P+'s hold-START pause, as the game always did.
//!
//! **Stage ids** are Brawl's `srStageKind` values, the ids the game's own match setup and P+'s
//! stage file loader key on (`docs/game-code.md` section 11, "Stages"). The list in
//! `config/rulesets.json` is the user's (2026-10-12), the same nine stages as the rooms' P+
//! default (`common::rules::PPLUS_STARTERS` and `PPLUS_COUNTERPICKS`):
//!
//! | id | stage | | id | stage |
//! |---|---|---|---|---|
//! | 0x01 (1) | Battlefield (starter) | | 0x0C (12) | Frigate Husk |
//! | 0x04 (4) | Luigi's Mansion (starter) | | 0x1F (31) | Fountain of Dreams |
//! | 0x09 (9) | Temple of Time (starter) | | 0x23 (35) | Green Hill Zone |
//! | 0x21 (33) | Smashville (starter) | | 0x2D (45) | Dream Land |
//! | 0x2E (46) | Pokémon Stadium 2 (starter) | | | |
//!
//! How the client uses the list (as Slippi's does, `EXI_DeviceSlippi.cpp:2134-2153, 2707-2722`):
//! Unranked and Ranked (fixed-rules modes) draw every game's stage from it; Direct draws game 1
//! from it, and the loser's pick for later games must be in it (design 5.4, screen 5). An empty
//! list makes the client fall back to its built-in copy of P+ v3.2's 15 legal stages (the ones its
//! random-stage switch "Default" preset, `/Project+/pf/stage/switch/Switch00.rss`, turns on).

use std::collections::{HashMap, HashSet};

use common::rules::MatchRules;
use serde::{Deserialize, Serialize};

/// `srStageKind` values fit in a byte; a larger id in the file is a typo.
pub const MAX_STAGE_ID: u16 = 0xFF;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct ModeRules {
    #[serde(default)]
    pub stages: Vec<u16>,
    /// Ranked: the starters struck 1-2-1 for game 1 (a subset of `stages`); `stages` is the
    /// counterpick list for the games after.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub starters: Vec<u16>,
    /// Item bitfield sent as `items` (Slippi's field; 0 = items off).
    #[serde(default)]
    pub items: u32,
    /// The match rules, sent as `rules`. Missing fields are P+'s competitive defaults.
    #[serde(default)]
    pub rules: MatchRules,
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
            let mut starters = HashSet::new();
            for &s in &rules.starters {
                anyhow::ensure!(seen.contains(&s), "{name}: starter {s} is not in the stage list");
                anyhow::ensure!(starters.insert(s), "{name}: starter {s} listed twice");
            }
            anyhow::ensure!(
                rules.starters.is_empty() || rules.starters.len() == 5,
                "{name}: Ranked strikes 1-2-1, so it needs 5 starters"
            );
            rules.rules.validate().map_err(|e| anyhow::anyhow!("{name}: {e}"))?;
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
    use common::rules::Pause;

    /// The user's nine stages (2026-10-12): the default ruleset's starters and counterpicks.
    fn nine_stages() -> Vec<u16> {
        let mut all: Vec<u16> = common::rules::PPLUS_STARTERS.iter().chain(&common::rules::PPLUS_COUNTERPICKS).copied().collect();
        all.sort();
        all
    }

    #[test]
    fn default_file_has_the_pplus_ruleset() {
        let r = Rulesets::parse(DEFAULT_RULESETS).unwrap();
        for mode in [Mode::Direct, Mode::Unranked, Mode::Ranked] {
            let m = r.for_mode(mode);
            let mut stages = m.stages.clone();
            stages.sort();   // Ranked's are in its stage select's order
            assert_eq!(stages, nine_stages(), "{mode:?}");
            assert_eq!(m.items, 0, "{mode:?}");
            // P+'s competitive set; normal pause only in Direct, else hold START.
            let pause = if mode == Mode::Direct { Pause::Normal } else { Pause::Hold };
            assert_eq!(m.rules, MatchRules { pause, ..Default::default() }, "{mode:?}");
        }
        // Ranked's stage select's bottom row, left to right (the user's, 2026-10-16).
        assert_eq!(r.for_mode(Mode::Ranked).starters, [9, 0x21, 1, 0x2E, 4]);
        let mut starters = common::rules::PPLUS_STARTERS.to_vec();
        starters.sort();
        let mut ranked = r.for_mode(Mode::Ranked).starters.clone();
        ranked.sort();
        assert_eq!(ranked, starters);
        assert!(r.for_mode(Mode::Direct).starters.is_empty());
        assert!(r.for_mode(Mode::Unranked).starters.is_empty());
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
            r#"{"unranked": {"rules": {"stocks": 0}}}"#,
            r#"{"unranked": {"rules": {"stonks": 4}}}"#,
        ] {
            assert!(Rulesets::parse(bad).is_err(), "{bad}");
        }
        assert!(Rulesets::load(Some("does/not/exist.json")).is_err());
        assert_eq!(Rulesets::load(None).unwrap(), Rulesets::parse(DEFAULT_RULESETS).unwrap());
    }
}
