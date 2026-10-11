//! The rules of a match: what the server decides and every client applies before the match
//! starts (`docs/design/room-rules.md` §3). Sent as `get-ticket-resp.rules` (ours; Slippi has no
//! such field and plays one fixed ruleset). Dolphin writes them into the game's SESSION block and
//! the game puts them into Brawl's set rule, the menu record's items, the stage select's hazard
//! switch and P+'s Code Menu (`OnlineMenu::applyRules`); the gameplay session compares what was
//! applied on every machine before the match runs.
//!
//! The defaults are P+'s competitive set (P+ v3.2's "Default Settings Modifier": stock, 4 stocks,
//! damage 1.0, an 8-minute time limit, team attack on, pause on; items off, hazards on), which
//! every mode played before rules were sent. Every field is a line of P+'s rules menu (RULES,
//! MORE RULES, the item switch, the hazard switch); where the game keeps each one is
//! `docs/game-code.md` §14, "Room rules RE".

use serde::{Deserialize, Serialize};

/// Brawl's `GameRule` (`gmSetRule::m_rule`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Style {
    Time,
    #[default]
    Stock,
    Coin,
}

/// P+'s PAUSE line (MORE RULES): NORMAL, or HOLD (P+'s "Pause Set to Off -> Hold Start to
/// Pause": START held for 50 frames). P+ has no pause that is off.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Pause {
    #[default]
    Normal,
    Hold,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default, deny_unknown_fields)]
pub struct MatchRules {
    pub style: Style,
    /// Stock: the stock count, 1-99.
    pub stocks: u8,
    /// MORE RULES' TIME LIMIT in minutes, for every style (P+: "Stock Time Limit Handles Time
    /// Limit for All Rules"): 1-99 for time and coin; for stock 0-99, 0 = no limit.
    pub time_minutes: u8,
    /// 0.5-2.0 in steps of 0.1 (the rules menu's damage ratio; Brawl keeps it times 10).
    pub damage_ratio: f64,
    pub team_attack: bool,
    pub pause: Pause,
    /// Brawl's item frequency: 0 none, 1 low, 2 medium, 3 high.
    pub item_frequency: u8,
    /// The ITEM SWITCH screen's 8 bytes (the menu record's switches, `+0x818`).
    pub item_switch: [u8; 8],
    pub hazards: bool,
    /// MORE RULES' AUTO L-CANCEL (for every player).
    pub auto_l_cancel: bool,
    /// MORE RULES' INPUT ASSIST (P+'s 3-frame input buffer, for every player).
    pub input_assist: bool,
}

/// P+ v3.2's item switch after a fresh boot (its save's default).
pub const PPLUS_ITEM_SWITCH: [u8; 8] = [0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];

impl Default for MatchRules {
    fn default() -> Self {
        MatchRules {
            style: Style::Stock,
            stocks: 4,
            time_minutes: 8,
            damage_ratio: 1.0,
            team_attack: true,
            pause: Pause::Normal,
            item_frequency: 0,
            item_switch: PPLUS_ITEM_SWITCH,
            hazards: true,
            auto_l_cancel: false,
            input_assist: false,
        }
    }
}

impl MatchRules {
    /// The damage ratio as Brawl keeps it (times 10, 5-20).
    pub fn damage_ratio_tenths(&self) -> u8 {
        (self.damage_ratio * 10.0).round().clamp(0.0, 255.0) as u8
    }

    /// Checks every value against what the rules menu allows.
    pub fn validate(&self) -> Result<(), String> {
        if !(1..=99).contains(&self.stocks) {
            return Err(format!("stocks {} is not 1-99", self.stocks));
        }
        let min_time = if self.style == Style::Stock { 0 } else { 1 };
        if !(min_time..=99).contains(&self.time_minutes) {
            return Err(format!("timeMinutes {} is not {min_time}-99", self.time_minutes));
        }
        let tenths = self.damage_ratio * 10.0;
        if !self.damage_ratio.is_finite()
            || !(5..=20).contains(&self.damage_ratio_tenths())
            || (tenths - tenths.round()).abs() > 1e-6
        {
            return Err(format!("damageRatio {} is not 0.5-2.0 in steps of 0.1", self.damage_ratio));
        }
        if self.item_frequency > 3 {
            return Err(format!("itemFrequency {} is not 0-3", self.item_frequency));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_pplus_competitive_and_valid() {
        let r = MatchRules::default();
        assert_eq!((r.style, r.stocks, r.time_minutes, r.damage_ratio_tenths()), (Style::Stock, 4, 8, 10));
        assert!(r.team_attack && r.hazards && !r.auto_l_cancel && !r.input_assist);
        assert_eq!(r.pause, Pause::Normal);
        assert_eq!((r.item_frequency, r.item_switch), (0, PPLUS_ITEM_SWITCH));
        r.validate().unwrap();
    }

    #[test]
    fn wire_shape() {
        let r = MatchRules { auto_l_cancel: true, pause: Pause::Hold, ..Default::default() };
        assert_eq!(
            serde_json::to_value(&r).unwrap(),
            serde_json::json!({
                "style": "stock", "stocks": 4, "timeMinutes": 8, "damageRatio": 1.0,
                "teamAttack": true, "pause": "hold", "itemFrequency": 0,
                "itemSwitch": [0, 6, 0, 0, 0, 0, 0, 0], "hazards": true, "autoLCancel": true,
                "inputAssist": false
            })
        );
        // Missing fields take the defaults; unknown ones are refused.
        let partial: MatchRules = serde_json::from_str(r#"{"stocks": 3, "style": "time"}"#).unwrap();
        assert_eq!((partial.stocks, partial.style, partial.time_minutes), (3, Style::Time, 8));
        assert!(serde_json::from_str::<MatchRules>(r#"{"stock": 3}"#).is_err());
    }

    #[test]
    fn bad_values_are_refused() {
        let bad = [
            MatchRules { stocks: 0, ..Default::default() },
            MatchRules { stocks: 100, ..Default::default() },
            MatchRules { style: Style::Time, time_minutes: 0, ..Default::default() },
            MatchRules { time_minutes: 100, ..Default::default() },
            MatchRules { damage_ratio: 0.4, ..Default::default() },
            MatchRules { damage_ratio: 2.1, ..Default::default() },
            MatchRules { damage_ratio: 1.05, ..Default::default() },
            MatchRules { damage_ratio: f64::NAN, ..Default::default() },
            MatchRules { item_frequency: 4, ..Default::default() },
        ];
        for r in bad {
            assert!(r.validate().is_err(), "{r:?}");
        }
        MatchRules { time_minutes: 0, ..Default::default() }.validate().unwrap();
        MatchRules { damage_ratio: 0.5, ..Default::default() }.validate().unwrap();
        MatchRules { damage_ratio: 2.0, style: Style::Coin, time_minutes: 2, ..Default::default() }.validate().unwrap();
    }
}
