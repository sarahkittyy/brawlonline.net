//! Per-mode rules the server hands to clients.
//!
//! Only `stages` travels in the mm protocol today (`get-ticket-resp.stages`).
//! The rest (stocks, timer) is kept here so unranked and ranked (later phases)
//! read one file. The user chose the P+ competitive ruleset: 4 stocks, 8
//! minutes, P+ legal stages. The stage ids are Brawl/P+ stage ids and are left
//! empty until the legal list is confirmed; an empty list makes the client use
//! its built-in default.

use std::collections::HashMap;

use serde::{Deserialize, Serialize};

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
    pub fn parse(json: &str) -> anyhow::Result<Self> {
        Ok(serde_json::from_str(json)?)
    }

    pub fn load(path: Option<&str>) -> anyhow::Result<Self> {
        match path {
            Some(p) => Self::parse(&std::fs::read_to_string(p)?),
            None => Self::parse(DEFAULT_RULESETS),
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

    #[test]
    fn default_file_parses() {
        let r = Rulesets::parse(DEFAULT_RULESETS).unwrap();
        assert_eq!(r.for_mode(Mode::Unranked).stocks, Some(4));
        assert_eq!(r.for_mode(Mode::Ranked).time_minutes, Some(8));
        assert_eq!(r.for_mode(Mode::Direct).items, 0);
        assert_eq!(r.for_mode(Mode::Party), ModeRules::default());
    }
}
