//! The rules of a match: what the server decides and every client applies before the match
//! starts (`docs/design/room-rules.md` §3). Sent as `get-ticket-resp.rules` (ours; Slippi has no
//! such field and plays one fixed ruleset). Dolphin writes them into the game's SESSION block and
//! the game puts them into Brawl's set rule, the menu record's items, the stage select's hazard
//! switch and P+'s Code Menu (`OnlineMenu::applyRules`); the gameplay session compares what was
//! applied on every machine before the match runs.
//!
//! The defaults are P+'s competitive set (P+ v3.2's "Default Settings Modifier": stock, 4 stocks,
//! damage 1.0, an 8-minute time limit, team attack on, pause on; items off) with stage hazards
//! off (the user's, 2026-10-12; P+'s own default is on). Every field is a line of P+'s rules
//! menu (RULES, MORE RULES, the item switch, the hazard switch); where the game keeps each one is
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

/// P+ v3.2's item switch after a fresh boot (its save's default): Stage Items and EX Items on.
pub const PPLUS_ITEM_SWITCH: [u8; 8] = [0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];

/// Our default item switch: P+'s with Stage Items (bit 49) off (the user's, 2026-10-13), so a
/// ruleset that turns an item or two on does not also get the stages' items. EX Items stays on.
pub const DEFAULT_ITEM_SWITCH: [u8; 8] = [0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];

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
            item_switch: DEFAULT_ITEM_SWITCH,
            // Stage hazards off (the user's, 2026-10-12): new rulesets, rooms' P+ default and
            // Unranked, Ranked and Direct.
            hazards: false,
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

// ---------------------------------------------------------------- rulesets

/// The ruleset format this build writes and reads (`Ruleset::format`). A ruleset of a newer
/// format is refused ("made by a newer version"), so a field added later is never dropped.
pub const RULESET_FORMAT: u32 = 1;
/// Letters in a ruleset code: 6 from the room codes' 20 (64 M codes), never confused with a
/// 4-letter room code (`docs/design/room-rules.md` §2).
pub const RULESET_CODE_LEN: usize = 6;
/// A ruleset's name: 1-32 characters of what a display name may hold
/// ([`crate::codes::is_name_char`]: the game's font draws it on the RULES screen).
pub const MAX_RULESET_NAME_LEN: usize = 32;
/// `srStageKind` values fit in a byte (as `mm::ruleset::MAX_STAGE_ID`).
pub const MAX_STAGE_ID: u16 = 0xFF;
/// Winner bans a ruleset can ask for (games 2+ of a set).
pub const MAX_BANS: u8 = 3;

/// P+ v3.2's legal stages (`srStageKind`; `mm::ruleset`'s table and `config/rulesets.json`):
/// what the launcher's ruleset editor offers.
pub const PPLUS_LEGAL_STAGES: [u16; 15] =
    [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0C, 0x0D, 0x1C, 0x1F, 0x21, 0x23, 0x2D, 0x2E];
/// Ranked's starters (`config/rulesets.json`; the user's list, 2026-10-12): Battlefield, Luigi's
/// Mansion, Temple of Time, Smashville, Pokémon Stadium 2. The default ruleset's starters.
pub const PPLUS_STARTERS: [u16; 5] = [0x01, 0x04, 0x09, 0x21, 0x2E];
/// Ranked's counterpicks, the stages games 2+ add to the starters: Frigate Husk, Fountain of
/// Dreams, Green Hill Zone, Dream Land. The default ruleset's counterpicks. Unranked and Direct
/// draw their random stages from the starters and these.
pub const PPLUS_COUNTERPICKS: [u16; 4] = [0x0C, 0x1F, 0x23, 0x2D];

/// A key the format had until 2026-10-11 and no longer has: `players` (1v1 / 2v2 / ffa), when a
/// ruleset was for one player count. Every ruleset works in any room now (room-rules.md decision
/// G); the key of an older draft, file or published ruleset is read and dropped, never written,
/// and is not part of the hash.
#[derive(Debug, Clone, Copy, Default)]
pub struct Dropped;

impl PartialEq for Dropped {
    fn eq(&self, _: &Self) -> bool {
        true
    }
}

impl<'de> Deserialize<'de> for Dropped {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        serde::de::IgnoredAny::deserialize(d).map(|_| Dropped)
    }
}

/// How the stage is chosen without a set (`bestOf` 0), every game (decision 17).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum StageChoice {
    /// Game 1 random, then the loser picks any stage of P+'s stage select, alternate layouts too
    /// (today's rooms); the stage list only feeds the random stages.
    #[default]
    LoserPicks,
    /// Game 1 random, then the loser picks from the stage list, on the stage's normal layout: the
    /// stage select greys the other stages (the user's, 2026-10-12, `room-rules.md` §2).
    LoserPicksList,
    /// Strike from the starters with the ruleset's pattern, the first striker random each game
    /// (a tournament's Bo1). Not for FFA.
    Strike,
    /// A random stage every game.
    Random,
}

/// The stage lists. Two separate lists, as tournament rulesets write them: game 1 of a set (and
/// every game with [`StageChoice::Strike`]) strikes from `starters`; games 2+ pick from both.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default, deny_unknown_fields)]
pub struct StageLists {
    pub starters: Vec<u16>,
    /// The extra stages of games 2+ (none of the starters).
    pub counterpicks: Vec<u16>,
}

impl Default for StageLists {
    fn default() -> Self {
        StageLists {
            starters: PPLUS_STARTERS.to_vec(),
            counterpicks: PPLUS_COUNTERPICKS.to_vec(),
        }
    }
}

impl StageLists {
    /// Every stage, starters first.
    pub fn all(&self) -> impl Iterator<Item = u16> + '_ {
        self.starters.iter().chain(&self.counterpicks).copied()
    }
}

/// A published ruleset (`docs/design/room-rules.md` §4): a name, the match rules, the stages and
/// the set. Published snapshots are immutable and named by a code; the full SHA-256 of the
/// canonical JSON ([`Ruleset::hash`]) names it for machines. JSON as the accounts API
/// (`docs/rulesets-protocol.md`), the launcher's import/export files and `get-ticket-resp` carry it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default, deny_unknown_fields)]
pub struct Ruleset {
    /// [`RULESET_FORMAT`] when written.
    pub format: u32,
    pub name: String,
    /// 1, 2, ... within one ruleset's versions; assigned by the server when published (0 in a
    /// draft or a file).
    pub version: u32,
    /// The publisher's display name, filled by the server. Not part of the hash (it follows
    /// renames).
    pub by: String,
    pub rules: MatchRules,
    pub stages: StageLists,
    /// The old `players` key ([`Dropped`]).
    #[serde(rename = "players", skip_serializing)]
    pub legacy_players: Dropped,
    /// 0 (no set: one game at a time), 3, 5 or 7. A set (and striking) is played when the room
    /// makes two sides (two players, or two teams of two); otherwise its games are single games.
    pub best_of: u8,
    /// No set only (with a set it is [`StageChoice::LoserPicks`] and unused).
    pub stage_choice: StageChoice,
    /// The strike pattern over the starters, players taking turns (`[1, 2, 1]`: the first striker
    /// strikes one, the other two, the first one): it leaves exactly one stage. Used by game 1 of
    /// a set and by [`StageChoice::Strike`]; empty otherwise.
    pub strikes: Vec<u8>,
    /// Games 2+ of a set: the winner bans 0-3 stages.
    pub bans: u8,
    /// Games 2+ of a set: the loser can't pick a stage they won on (Slippi's and Ranked's rule).
    pub dsr: bool,
}

impl Default for Ruleset {
    /// P+'s defaults as rooms play them today: P+'s competitive rules, Ranked's starters and
    /// counterpicks, no set, loser picks.
    fn default() -> Self {
        Ruleset {
            format: RULESET_FORMAT,
            name: "P+ default".into(),
            version: 0,
            by: String::new(),
            rules: MatchRules::default(),
            stages: StageLists::default(),
            legacy_players: Dropped,
            best_of: 0,
            stage_choice: StageChoice::LoserPicks,
            strikes: Vec::new(),
            bans: 0,
            dsr: false,
        }
    }
}

impl Ruleset {
    /// Whether this ruleset strikes (game 1 of a set, or every game with [`StageChoice::Strike`]).
    pub fn strikes_used(&self) -> bool {
        self.best_of > 0 || self.stage_choice == StageChoice::Strike
    }

    /// Whether a loser's stage pick is held to the stage list, on the stage's normal layout
    /// ([`StageChoice::LoserPicksList`]; a set picks from its list in its own steps).
    pub fn pick_from_stages(&self) -> bool {
        self.best_of == 0 && self.stage_choice == StageChoice::LoserPicksList
    }

    /// Clears what the set format leaves unused, so two rulesets that play the same have the same
    /// JSON (and hash): trims the name; with a set the stage choice is `loser-picks`; without one
    /// no bans and no DSR; no strike pattern unless one is used. The server normalizes before it
    /// validates and publishes.
    pub fn normalize(&mut self) {
        self.name = self.name.trim().to_string();
        if self.best_of > 0 {
            self.stage_choice = StageChoice::LoserPicks;
        } else {
            self.bans = 0;
            self.dsr = false;
        }
        if !self.strikes_used() {
            self.strikes.clear();
        }
    }

    /// Checks everything a published ruleset must satisfy. Expects [`Ruleset::normalize`]d input:
    /// an unused field with a value is refused rather than silently kept.
    pub fn validate(&self) -> Result<(), String> {
        if self.format == 0 || self.format > RULESET_FORMAT {
            return Err(if self.format > RULESET_FORMAT {
                "This ruleset was made by a newer version. Update to use it.".into()
            } else {
                format!("format {} is not a ruleset format", self.format)
            });
        }
        let len = self.name.chars().count();
        if len == 0 || self.name.trim() != self.name {
            return Err("The name can't be empty or start or end with a space".into());
        }
        if len > MAX_RULESET_NAME_LEN {
            return Err(format!("The name is longer than {MAX_RULESET_NAME_LEN} characters"));
        }
        if !self.name.chars().all(crate::codes::is_name_char) {
            return Err("The name can only have letters, numbers, Hiragana, Katakana and punctuation".into());
        }
        self.rules.validate()?;

        if self.stages.starters.is_empty() {
            return Err("There are no starter stages".into());
        }
        let mut seen = std::collections::HashSet::new();
        for s in self.stages.all() {
            if s == 0 || s > MAX_STAGE_ID {
                return Err(format!("Stage id {s} is out of range"));
            }
            if !seen.insert(s) {
                return Err(format!("Stage id {s} is listed twice"));
            }
        }

        if ![0, 3, 5, 7].contains(&self.best_of) {
            return Err(format!("bestOf {} is not 0 (no set), 3, 5 or 7", self.best_of));
        }
        if self.best_of > 0 && self.stage_choice != StageChoice::LoserPicks {
            return Err("The stage choice is for rulesets without a set".into());
        }

        if self.strikes_used() {
            let left = self.stages.starters.len() - 1;
            if self.strikes.contains(&0) {
                return Err("A strike turn strikes at least one stage".into());
            }
            let total: usize = self.strikes.iter().map(|&n| n as usize).sum();
            if total != left {
                return Err(format!(
                    "The strike pattern strikes {total} of {} starters; it has to leave exactly one ({left} strikes)",
                    self.stages.starters.len()
                ));
            }
        } else if !self.strikes.is_empty() {
            return Err("This ruleset doesn't strike, so it has no strike pattern".into());
        }

        if self.bans > MAX_BANS {
            return Err(format!("bans {} is not 0-{MAX_BANS}", self.bans));
        }
        if self.best_of == 0 && (self.bans > 0 || self.dsr) {
            return Err("Bans and DSR are for sets".into());
        }
        if self.bans as usize >= self.stages.all().count() {
            return Err("The winner's bans would leave no stage to pick".into());
        }
        Ok(())
    }

    /// The canonical JSON the hash is taken of: every field but `by`, keys sorted, no spaces,
    /// numbers and strings as serde_json writes them.
    pub fn canonical_json(&self) -> String {
        let mut v = serde_json::to_value(self).expect("a ruleset serializes");
        if let serde_json::Value::Object(m) = &mut v {
            m.remove("by");
        }
        let mut out = String::new();
        write_canonical(&v, &mut out);
        out
    }

    /// SHA-256 of [`Ruleset::canonical_json`], lowercase hex: what a match, result and replay
    /// record of the ruleset.
    pub fn hash(&self) -> String {
        use sha2::Digest;
        hex::encode(sha2::Sha256::digest(self.canonical_json().as_bytes()))
    }

    /// Whether two rulesets play the same (everything but `version` and `by`): publishing an
    /// unchanged copy of the latest version is refused.
    pub fn same_content(&self, other: &Ruleset) -> bool {
        Ruleset { version: 0, by: String::new(), ..self.clone() }
            == Ruleset { version: 0, by: String::new(), ..other.clone() }
    }
}

fn write_canonical(v: &serde_json::Value, out: &mut String) {
    use serde_json::Value;
    match v {
        Value::Object(m) => {
            let mut keys: Vec<&String> = m.keys().collect();
            keys.sort();
            out.push('{');
            for (i, k) in keys.into_iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                out.push_str(&serde_json::to_string(k).expect("a string serializes"));
                out.push(':');
                write_canonical(&m[k], out);
            }
            out.push('}');
        }
        Value::Array(a) => {
            out.push('[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_canonical(x, out);
            }
            out.push(']');
        }
        other => out.push_str(&other.to_string()),
    }
}

/// Normalizes a ruleset code typed anywhere (any case, full width, spaces around it): `None`
/// unless it is 6 letters of [`crate::rooms::ROOM_CODE_ALPHABET`].
pub fn parse_ruleset_code(input: &str) -> Option<String> {
    use unicode_normalization::UnicodeNormalization;
    let normalized: String = input.nfkc().collect::<String>().trim().to_ascii_uppercase();
    (normalized.len() == RULESET_CODE_LEN && normalized.bytes().all(|b| crate::rooms::ROOM_CODE_ALPHABET.contains(&b)))
        .then_some(normalized)
}

/// A random ruleset code (the server retries while it is taken).
pub fn random_ruleset_code(rng: &mut impl rand::Rng) -> String {
    let alphabet = crate::rooms::ROOM_CODE_ALPHABET;
    (0..RULESET_CODE_LEN).map(|_| alphabet[rng.gen_range(0..alphabet.len())] as char).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_pplus_competitive_and_valid() {
        let r = MatchRules::default();
        assert_eq!((r.style, r.stocks, r.time_minutes, r.damage_ratio_tenths()), (Style::Stock, 4, 8, 10));
        assert!(r.team_attack && !r.hazards && !r.auto_l_cancel && !r.input_assist);
        assert_eq!(r.pause, Pause::Normal);
        assert_eq!((r.item_frequency, r.item_switch), (0, DEFAULT_ITEM_SWITCH));
        assert_eq!(DEFAULT_ITEM_SWITCH, [PPLUS_ITEM_SWITCH[0], PPLUS_ITEM_SWITCH[1] & !0x02, 0, 0, 0, 0, 0, 0]);
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
                "itemSwitch": [0, 4, 0, 0, 0, 0, 0, 0], "hazards": false, "autoLCancel": true,
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

    /// A Bo3 like a tournament's: 5 starters struck 1-2-1, 2 bans, DSR.
    fn weekly() -> Ruleset {
        Ruleset {
            name: "NorCal Weekly".into(),
            stages: StageLists {
                starters: vec![0x01, 0x02, 0x21, 0x2D, 0x2E],
                counterpicks: vec![0x03, 0x04, 0x05, 0x06, 0x09, 0x0C, 0x0D, 0x1C, 0x1F, 0x23],
            },
            best_of: 3,
            strikes: vec![1, 2, 1],
            bans: 2,
            dsr: true,
            ..Default::default()
        }
    }

    #[test]
    fn default_ruleset_is_pplus_rooms_and_valid() {
        let r = Ruleset::default();
        r.validate().unwrap();
        assert_eq!(r.rules, MatchRules::default());
        assert_eq!((r.best_of, r.stage_choice), (0, StageChoice::LoserPicks));
        assert_eq!(r.stages.starters, PPLUS_STARTERS);
        assert_eq!(r.stages.counterpicks, PPLUS_COUNTERPICKS);
        // Every one of them is a legal stage (the editor's first tiles).
        assert!(r.stages.all().all(|s| PPLUS_LEGAL_STAGES.contains(&s)));
        assert_eq!(serde_json::to_value(&r.stages).unwrap()["starters"], serde_json::json!([1, 4, 9, 33, 46]));
        assert_eq!(serde_json::to_value(&r.stages).unwrap()["counterpicks"], serde_json::json!([12, 31, 35, 45]));
        weekly().validate().unwrap();
    }

    #[test]
    fn ruleset_wire_shape() {
        let v = serde_json::to_value(weekly()).unwrap();
        for key in
            ["format", "name", "version", "by", "rules", "stages", "bestOf", "stageChoice", "strikes", "bans", "dsr"]
        {
            assert!(v.get(key).is_some(), "{key} in {v}");
        }
        assert!(v.get("players").is_none(), "{v}");
        assert_eq!(v["stageChoice"], "loser-picks");
        assert_eq!(v["stages"]["starters"], serde_json::json!([1, 2, 33, 45, 46]));
        let back: Ruleset = serde_json::from_value(v).unwrap();
        assert_eq!(back, weekly());
        // Missing fields are the defaults; unknown ones are refused (in the rules too).
        let r: Ruleset = serde_json::from_str(r#"{"name": "x", "stageChoice": "random"}"#).unwrap();
        assert_eq!((r.stage_choice, r.format, r.best_of), (StageChoice::Random, 1, 0));
        r.validate().unwrap();
        // Loser picks from the list: held to the stages without a set; a set normalizes it away.
        let mut r: Ruleset = serde_json::from_str(r#"{"name": "x", "stageChoice": "loser-picks-list"}"#).unwrap();
        assert_eq!((r.stage_choice, r.pick_from_stages()), (StageChoice::LoserPicksList, true));
        r.validate().unwrap();
        assert!(!Ruleset::default().pick_from_stages());
        r.best_of = 3;
        r.strikes = vec![1, 2, 1];
        r.normalize();
        assert_eq!((r.stage_choice, r.pick_from_stages()), (StageChoice::LoserPicks, false));
        // The old `players` key (any value) is read and dropped: older drafts, files and published
        // rulesets still load, and play the same as without it.
        for old in [r#""1v1""#, r#""ffa""#, r#""3v3""#, "7"] {
            let text = format!(r#"{{"name": "x", "players": {old}, "bestOf": 3, "strikes": [1, 2, 1]}}"#);
            let o: Ruleset = serde_json::from_str(&text).unwrap();
            assert_eq!(o, Ruleset { name: "x".into(), best_of: 3, strikes: vec![1, 2, 1], ..Default::default() });
            assert!(!serde_json::to_string(&o).unwrap().contains("players"));
        }
        assert!(serde_json::from_str::<Ruleset>(r#"{"bestof": 3}"#).is_err());
        assert!(serde_json::from_str::<Ruleset>(r#"{"rules": {"stonks": 3}}"#).is_err());
        assert!(serde_json::from_str::<Ruleset>(r#"{"stages": {"starters": [1], "extra": []}}"#).is_err());
        assert!(serde_json::from_str::<Ruleset>(r#"{"stageChoice": "rps"}"#).is_err());
    }

    #[test]
    fn hash_is_of_the_canonical_json() {
        let r = weekly();
        let c = r.canonical_json();
        assert!(!c.contains("\"by\""), "{c}");
        assert_eq!(
            c,
            r#"{"bans":2,"bestOf":3,"dsr":true,"format":1,"name":"NorCal Weekly","rules":{"autoLCancel":false,"damageRatio":1.0,"hazards":false,"inputAssist":false,"itemFrequency":0,"itemSwitch":[0,4,0,0,0,0,0,0],"pause":"normal","stocks":4,"style":"stock","teamAttack":true,"timeMinutes":8},"stageChoice":"loser-picks","stages":{"counterpicks":[3,4,5,6,9,12,13,28,31,35],"starters":[1,2,33,45,46]},"strikes":[1,2,1],"version":0}"#
        );
        // Pinned, so a change to the canonical form shows up here (the hash is in matches,
        // results and replays).
        assert_eq!(r.hash(), "1e778cce399b34eb7efaa0b9b5773d2246998660e3b1af6f8c8f76085d1bdf23");
        // `by` is left out; every other field counts, even stage order.
        assert_eq!(Ruleset { by: "sarah".into(), ..r.clone() }.hash(), r.hash());
        assert_ne!(Ruleset { version: 2, ..r.clone() }.hash(), r.hash());
        assert_ne!(Ruleset { name: "NorCal Weekly ".into(), ..r.clone() }.hash(), r.hash());
        let mut swapped = r.clone();
        swapped.stages.starters.swap(0, 1);
        assert_ne!(swapped.hash(), r.hash());
        let mut alc = r.clone();
        alc.rules.auto_l_cancel = true;
        assert_ne!(alc.hash(), r.hash());
        // A JSON round trip (any key order, an old `players` key) gives the same hash.
        let text = r#"{"version":0,"strikes":[1,2,1],"stages":{"starters":[1,2,33,45,46],"counterpicks":[3,4,5,6,9,12,13,28,31,35]},"stageChoice":"loser-picks","rules":{},"players":"1v1","name":"NorCal Weekly","format":1,"dsr":true,"bestOf":3,"bans":2,"by":"x"}"#;
        assert_eq!(serde_json::from_str::<Ruleset>(text).unwrap().hash(), r.hash());
        assert!(Ruleset { version: 3, by: "x".into(), ..r.clone() }.same_content(&r));
        assert!(!alc.same_content(&r));
    }

    #[test]
    fn normalize_clears_what_the_format_leaves_unused() {
        let mut r = Ruleset {
            name: "  Bo1 strikes ".into(),
            stage_choice: StageChoice::Strike,
            strikes: vec![1, 2, 1],
            bans: 2,
            dsr: true,
            ..Default::default()
        };
        r.normalize();
        assert_eq!((r.name.as_str(), r.bans, r.dsr, r.strikes.clone()), ("Bo1 strikes", 0, false, vec![1, 2, 1]));
        r.validate().unwrap();
        // Loser picks: no pattern.
        r.stage_choice = StageChoice::LoserPicks;
        r.normalize();
        assert!(r.strikes.is_empty());
        r.validate().unwrap();
        // With a set the stage choice is unused.
        let mut s = Ruleset { stage_choice: StageChoice::Random, ..weekly() };
        assert!(s.validate().is_err());
        s.normalize();
        assert_eq!(s.stage_choice, StageChoice::LoserPicks);
        s.validate().unwrap();
        // No set and no striking: no strike pattern.
        let mut f = Ruleset { strikes: vec![4], ..Default::default() };
        f.normalize();
        assert!(f.strikes.is_empty());
        f.validate().unwrap();
        // Normalizing twice changes nothing.
        let mut w = weekly();
        w.normalize();
        assert_eq!(w, weekly());
    }

    #[test]
    fn bad_rulesets_are_refused() {
        let three = StageLists { starters: vec![1, 2, 33], counterpicks: vec![] };
        let bad: Vec<(Ruleset, &str)> = vec![
            (Ruleset { format: 2, ..weekly() }, "newer"),
            (Ruleset { format: 0, ..weekly() }, "format"),
            (Ruleset { name: "".into(), ..weekly() }, "empty"),
            (Ruleset { name: " x".into(), ..weekly() }, "space"),
            (Ruleset { name: "x".repeat(33), ..weekly() }, "longer"),
            (Ruleset { name: "tab\there".into(), ..weekly() }, "letters"),
            (Ruleset { name: "emoji 🎮".into(), ..weekly() }, "letters"),
            (Ruleset { rules: MatchRules { stocks: 0, ..Default::default() }, ..weekly() }, "stocks"),
            (Ruleset { stages: StageLists { starters: vec![], counterpicks: vec![1] }, ..weekly() }, "starter"),
            (Ruleset { stages: StageLists { starters: vec![0], counterpicks: vec![] }, ..Default::default() }, "range"),
            (
                Ruleset { stages: StageLists { starters: vec![256], counterpicks: vec![] }, ..Default::default() },
                "range",
            ),
            (
                Ruleset { stages: StageLists { starters: vec![1, 1], counterpicks: vec![] }, ..Default::default() },
                "twice",
            ),
            (
                Ruleset { stages: StageLists { starters: vec![1], counterpicks: vec![1] }, ..Default::default() },
                "twice",
            ),
            (Ruleset { best_of: 1, ..weekly() }, "bestOf"),
            (Ruleset { best_of: 2, ..weekly() }, "bestOf"),
            (Ruleset { strikes: vec![1, 2], ..weekly() }, "leave exactly one"),
            (Ruleset { strikes: vec![2, 2, 1], ..weekly() }, "leave exactly one"),
            (Ruleset { strikes: vec![], ..weekly() }, "leave exactly one"),
            (Ruleset { strikes: vec![0, 1, 2, 1], ..weekly() }, "at least one"),
            (Ruleset { stages: three.clone(), ..weekly() }, "leave exactly one"),
            (Ruleset { strikes: vec![1, 2, 1], ..Default::default() }, "doesn't strike"),
            (Ruleset { bans: 4, ..weekly() }, "bans"),
            (Ruleset { bans: 1, ..Default::default() }, "for sets"),
            (Ruleset { dsr: true, ..Default::default() }, "for sets"),
            (
                Ruleset {
                    stages: StageLists { starters: vec![1, 2, 3], counterpicks: vec![] },
                    strikes: vec![1, 1],
                    bans: 3,
                    ..weekly()
                },
                "no stage",
            ),
        ];
        for (r, why) in bad {
            let err = r.validate().expect_err(&format!("{r:?}"));
            assert!(err.contains(why), "{why:?} not in {err:?}");
        }
        // Good edge cases: 3 starters struck 1-1, one starter and nothing to strike, 7 starters
        // 1-2-2-1, Japanese names, 32 characters.
        Ruleset { stages: three, strikes: vec![1, 1], ..weekly() }.validate().unwrap();
        Ruleset {
            stages: StageLists { starters: vec![2], counterpicks: vec![1] },
            stage_choice: StageChoice::Strike,
            ..Default::default()
        }
        .validate()
        .unwrap();
        Ruleset {
            stages: StageLists { starters: vec![1, 2, 3, 4, 5, 6, 9], counterpicks: vec![] },
            strikes: vec![1, 2, 2, 1],
            ..weekly()
        }
        .validate()
        .unwrap();
        Ruleset { name: "スマブラ カップ".into(), ..weekly() }.validate().unwrap();
        Ruleset { name: "x".repeat(32), ..weekly() }.validate().unwrap();
    }

    #[test]
    fn ruleset_codes() {
        assert_eq!(parse_ruleset_code("bcdfgh").as_deref(), Some("BCDFGH"));
        assert_eq!(parse_ruleset_code(" BCDFGH\n").as_deref(), Some("BCDFGH"));
        assert_eq!(parse_ruleset_code("ＢＣＤＦＧＨ").as_deref(), Some("BCDFGH"));
        for bad in ["", "BCDF", "BCDFG", "BCDFGHJ", "ABCDFG", "BCDFGY", "BCD FG", "123456"] {
            assert_eq!(parse_ruleset_code(bad), None, "{bad:?}");
        }
        let mut rng = rand::thread_rng();
        for _ in 0..200 {
            assert!(parse_ruleset_code(&random_ruleset_code(&mut rng)).is_some());
        }
    }
}
