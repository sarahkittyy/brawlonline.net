//! The builds of the game mm lets play (`docs/design/pplus-mods.md` §3).
//!
//! A game says which build it runs in `hello` and in every ticket (`game`, Dolphin's
//! `Online/GameBuild.h`): `<folder>:<32 hex digits>`, the build's folder on the SD card and a hash
//! of the file Dolphin booted and of the codesets in that folder. P+ and its mods (Cobalt, P+Ex)
//! run the same online menus and gameplay rollback, but two builds can't play each other: their
//! characters, stages and code differ. So mm pairs tickets and admits to a room only within one
//! build, and each build has its own stage lists.
//!
//! `config/games.json` (or `MM_GAMES_FILE`) lists them. The first entry is the default, P+: a
//! game that names no build is P+ (every build before the field), and so is any codeset in P+'s
//! folder (players' own P+ codesets played before this, at their own risk, and still do). A mod
//! must be listed with the exact hashes of its release: a new release of a mod plays once its
//! entry is added, and never against the old one (another `id`).
//!
//! ```json
//! {"games": [
//!   {"id": "pplus", "name": "Project+", "folder": "Project+"},
//!   {"id": "cobalt-1.34", "name": "Cobalt 1.34", "folder": "Cobalt",
//!    "codes": ["fe22b89a8cf4cdaac89bc39227111c9f"],
//!    "modes": ["direct", "unranked", "teams"],
//!    "starters": [1, 19], "counterpicks": [2],
//!    "rulesets": {"unranked": {"stages": [1, 2, 19], "rules": {"pause": "hold"}}}}
//! ]}
//! ```
//!
//! - `codes`: the hash part of the keys this entry is (`tools/gamecode/game_key.py`; one per boot
//!   file a player may start it with). Empty only for the default.
//! - `modes`: the modes it plays (`teams` is a room's game). Default for a mod: Direct, Unranked
//!   and rooms; Ranked and the 2v2 queue are P+'s alone unless listed.
//! - `rulesets`: per mode, in place of `config/rulesets.json`'s entry (the same shape); a mode
//!   without one plays the shared entry.
//! - `starters`, `counterpicks`: the stages of a room's default ruleset, in place of P+'s.
//! - `layout`: where the game code's own things go on a build whose menu art is not P+'s.
//! - `ownLauncher`: for the launcher, which reads this table too.
//! - `plugin`: which of the game plugin's files the build takes (`game-code/build.sh`: `pex` for
//!   P+ v3.2 with BrawlEx, `rex` for REX; none: P+'s own). For the launcher and the harness.

use std::collections::{HashMap, HashSet};

use common::proto::Mode;
use common::rooms::Layout;
use serde::Deserialize;

use crate::ruleset::{ModeRules, Rulesets, MAX_STAGE_ID};

/// The repository's `config/games.json`, used when no file is configured.
pub const DEFAULT_GAMES: &str = include_str!("../../../config/games.json");

/// The modes a mod plays unless its entry says otherwise.
const MOD_MODES: [&str; 3] = ["direct", "unranked", "teams"];
const MODE_NAMES: [&str; 6] = ["ranked", "unranked", "direct", "teams", "party", "twos"];
/// The game plugin's files (`game-code/build.sh`), as an entry's `plugin` names them.
const PLUGINS: [&str; 3] = ["pplus", "pex", "rex"];
/// A key's hash part: 32 lowercase hex digits.
const CODE_LEN: usize = 32;
pub const MAX_KEY_LEN: usize = 96;

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Game {
    /// What rooms, records and logs call it (`cobalt-1.34`).
    pub id: String,
    /// What players read ("Cobalt 1.34").
    pub name: String,
    pub folder: String,
    #[serde(default)]
    pub codes: Vec<String>,
    #[serde(default)]
    pub modes: Vec<String>,
    #[serde(default)]
    pub rulesets: HashMap<String, ModeRules>,
    #[serde(default)]
    pub starters: Vec<u16>,
    #[serde(default)]
    pub counterpicks: Vec<u16>,
    /// Where the game code's own things go on the build's screens, for a build whose menu art is
    /// not P+'s (`common::rooms::Layout`): the stage select's turn line, the cards' name plates,
    /// the character select's bar and its word buttons, each as the numbers the game code places
    /// them by. It goes to the build's Dolphin in `hello-resp` (`layout`), which passes it on to
    /// the game; a build with none has P+'s.
    #[serde(default)]
    pub layout: Option<Layout>,
    /// The launcher's (`launcher/src/dolphin/mods/plus_family.ts`, which reads this table): the
    /// build boots its own launcher file instead of P+'s Netplay Launcher. mm does not use it.
    #[serde(default)]
    pub own_launcher: bool,
    /// The launcher's and the harness's: which of the game plugin's files goes on the build's
    /// card (`game-code/PPOnline/include/variant.h`: "pplus", "pex" or "rex"; none is P+'s own).
    /// mm does not use it; a build with the wrong one has its online modes locked by the plugin.
    #[serde(default)]
    pub plugin: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(deny_unknown_fields)]
struct GamesFile {
    games: Vec<Game>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Games {
    games: Vec<Game>,
}

/// A key no entry has: the folder it names (for the log), or the key when it has none.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnknownGame(pub String);

impl Default for Games {
    fn default() -> Self {
        Self::parse(DEFAULT_GAMES).expect("config/games.json")
    }
}

impl Games {
    pub fn parse(json: &str) -> anyhow::Result<Self> {
        let file: GamesFile = serde_json::from_str(json)?;
        anyhow::ensure!(!file.games.is_empty(), "no games");
        let mut ids = HashSet::new();
        let mut keys = HashSet::new();
        for (i, g) in file.games.iter().enumerate() {
            let id_ok = !g.id.is_empty()
                && g.id.len() <= 32
                && g.id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"-._".contains(&b));
            anyhow::ensure!(id_ok, "game id {:?}: 1-32 of a-z, 0-9, '-', '.', '_'", g.id);
            anyhow::ensure!(ids.insert(g.id.as_str()), "game id {:?} listed twice", g.id);
            anyhow::ensure!(!g.name.trim().is_empty() && g.name.len() <= 48, "{}: a name of 1-48 bytes", g.id);
            anyhow::ensure!(!g.folder.is_empty() && !g.folder.contains(':'), "{}: a folder", g.id);
            anyhow::ensure!(i == 0 || !g.codes.is_empty(), "{}: a mod needs its codes", g.id);
            anyhow::ensure!(i == 0 || g.folder != file.games[0].folder, "{}: the default game's folder", g.id);
            for c in &g.codes {
                let hex = c.len() == CODE_LEN && c.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b));
                anyhow::ensure!(hex, "{}: code {c:?} is not {CODE_LEN} lowercase hex digits", g.id);
                anyhow::ensure!(keys.insert((g.folder.as_str(), c.as_str())), "{}: code {c} listed twice", g.id);
            }
            for m in &g.modes {
                anyhow::ensure!(MODE_NAMES.contains(&m.as_str()), "{}: unknown mode {m:?}", g.id);
            }
            anyhow::ensure!(g.layout.as_ref().map_or(true, Layout::is_sane), "{}: a layout number out of range", g.id);
            anyhow::ensure!(
                g.plugin.as_deref().map_or(true, |p| PLUGINS.contains(&p)),
                "{}: plugin {:?} is not one of {PLUGINS:?}",
                g.id,
                g.plugin
            );
            Rulesets { modes: g.rulesets.clone() }.validate().map_err(|e| anyhow::anyhow!("{}: {e}", g.id))?;
            let mut seen = HashSet::new();
            for &s in g.starters.iter().chain(&g.counterpicks) {
                anyhow::ensure!(s <= MAX_STAGE_ID, "{}: stage id {s} out of range", g.id);
                anyhow::ensure!(seen.insert(s), "{}: stage id {s} listed twice", g.id);
            }
            anyhow::ensure!(
                g.starters.is_empty() == g.counterpicks.is_empty() || !g.starters.is_empty(),
                "{}: counterpicks without starters",
                g.id
            );
        }
        Ok(Games { games: file.games })
    }

    pub fn load(path: Option<&str>) -> anyhow::Result<Self> {
        match path {
            Some(p) if !p.is_empty() => Self::parse(&std::fs::read_to_string(p)?),
            _ => Self::parse(DEFAULT_GAMES),
        }
    }

    /// P+.
    pub fn default_game(&self) -> &Game {
        &self.games[0]
    }

    pub fn is_default(&self, id: &str) -> bool {
        id.is_empty() || id == self.games[0].id
    }

    /// The mods of P+: every game but the default.
    pub fn mods(&self) -> impl Iterator<Item = &Game> {
        self.games[1..].iter()
    }

    /// The game of a record's or a room's id; an id that is gone from the file reads as P+.
    pub fn get(&self, id: &str) -> &Game {
        self.games.iter().find(|g| g.id == id).unwrap_or(&self.games[0])
    }

    /// The game a `hello` or a ticket names (`game`).
    pub fn resolve(&self, key: &str) -> Result<&Game, UnknownGame> {
        if key.is_empty() {
            return Ok(&self.games[0]);
        }
        if key.len() > MAX_KEY_LEN {
            return Err(UnknownGame(String::new()));
        }
        let Some((folder, code)) = key.split_once(':') else {
            return Err(UnknownGame(key.chars().filter(|c| !c.is_control()).collect()));
        };
        if folder == self.games[0].folder {
            return Ok(&self.games[0]);
        }
        self.games[1..]
            .iter()
            .find(|g| g.folder == folder && g.codes.iter().any(|c| c == code))
            .ok_or_else(|| UnknownGame(folder.chars().filter(|c| !c.is_control()).collect()))
    }

    /// The id a match record and a room keep: empty for P+ (every row before the column).
    pub fn record_id(&self, game: &Game) -> String {
        if self.is_default(&game.id) {
            String::new()
        } else {
            game.id.clone()
        }
    }

    pub fn plays(&self, game: &Game, mode: Mode) -> bool {
        if self.is_default(&game.id) && game.modes.is_empty() {
            return true;
        }
        if game.modes.is_empty() {
            MOD_MODES.contains(&mode.name())
        } else {
            game.modes.iter().any(|m| m == mode.name())
        }
    }

    /// A mode's rules for a game: its own entry, else the shared one.
    pub fn rules(&self, game: &Game, mode: Mode, shared: &Rulesets) -> ModeRules {
        game.rulesets.get(mode.name()).cloned().unwrap_or_else(|| shared.for_mode(mode))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const COBALT: &str = "Cobalt:fe22b89a8cf4cdaac89bc39227111c9f";

    #[test]
    fn the_default_file_has_pplus_and_the_three_mods() {
        let g = Games::default();
        assert_eq!(g.default_game().id, "pplus");
        assert_eq!(g.resolve("").unwrap().id, "pplus");
        // Any codeset in P+'s folder is P+.
        assert_eq!(g.resolve("Project+:bc298983bc1e2e55afc2156e77069341").unwrap().id, "pplus");
        assert_eq!(g.resolve("Project+:00000000000000000000000000000000").unwrap().id, "pplus");
        // The mods by their exact keys: the netplay and the offline launcher's.
        assert_eq!(g.resolve(COBALT).unwrap().id, "cobalt-1.34");
        assert_eq!(g.resolve("Cobalt:ee8a6422b0cd2077861a51574b09e742").unwrap().id, "cobalt-1.34");
        assert_eq!(g.resolve("P+Ex:812d2df3a26681ce57c335c54e675a8a").unwrap().id, "pex-1.8");
        assert_eq!(g.resolve("Cobalt:00000000000000000000000000000000"), Err(UnknownGame("Cobalt".into())));
        assert_eq!(g.resolve("REX:812d2df3a26681ce57c335c54e675a8a"), Err(UnknownGame("REX".into())));
        // REX 0.5 (folder "rex_"), booted from its own .elf launchers.
        assert_eq!(g.resolve("rex_:747b3fc31200c2aea36293fa91b2c657").unwrap().id, "rex-0.5");
        assert_eq!(g.resolve("rex_:2ca73eed0dbd9a83044d040a74bede71").unwrap().id, "rex-0.5");
        assert_eq!(g.get("rex-0.5").name, "REX 0.5");
        // P+Ex's hash under Cobalt's folder is not Cobalt.
        assert!(g.resolve("Cobalt:812d2df3a26681ce57c335c54e675a8a").is_err());
        assert_eq!(g.resolve("nonsense"), Err(UnknownGame("nonsense".into())));
        assert!(g.resolve(&"x".repeat(200)).is_err());
        assert_eq!(g.record_id(g.resolve("").unwrap()), "");
        assert_eq!(g.record_id(g.resolve(COBALT).unwrap()), "cobalt-1.34");
        assert_eq!(g.get("cobalt-1.34").name, "Cobalt 1.34");
        // Cobalt's stage select has its header taken (the turn line under the button legend) and
        // Brawl's own cards (the pencil at the plate's right end); P+ Ex's art is P+'s.
        let cobalt = g.get("cobalt-1.34").layout.clone().unwrap();
        assert_eq!(cobalt.sss_line, Some([726.0, -191.0, 1046.0, -231.0, 0.8]));
        assert_eq!(cobalt.card_plates, Some([-22.6, 14.5, 4.3]));
        assert!(cobalt.css_bar.is_none() && cobalt.css_buttons.is_none());
        assert!(g.get("pex-1.8").layout.is_none() && g.get("").layout.is_none());
        // REX's own stage select header and bar, its buttons as words, P+'s card; its own launcher.
        let rex = g.get("rex-0.5").layout.clone().unwrap();
        assert!(rex.sss_line.is_some() && rex.css_bar.is_some() && rex.card_plates.is_none());
        assert_eq!(rex.css_buttons.unwrap()[3][1], 561.0);
        assert!(g.get("rex-0.5").own_launcher && !g.get("cobalt-1.34").own_launcher);
        // each build's plugin file: P+'s own, the BrawlEx one, REX's
        let plugin = |id: &str| g.get(id).plugin.clone();
        assert_eq!(plugin(""), None);
        assert_eq!((plugin("pex-1.8"), plugin("cobalt-1.34")), (Some("pex".into()), Some("pex".into())));
        assert_eq!(plugin("rex-0.5"), Some("rex".into()));
        let named = r#"{"games": [{"id": "pplus", "name": "P+", "folder": "Project+", "plugin": "vbrawl"}]}"#;
        assert!(Games::parse(named).unwrap_err().to_string().contains("plugin"));
        // a layout group of another length, or a number no screen has
        let bad = |layout: &str| {
            Games::parse(&format!(
                r#"{{"games": [{{"id": "pplus", "name": "P+", "folder": "Project+"}},
                   {{"id": "m", "name": "M", "folder": "M", "codes": ["{}"], "layout": {layout}}}]}}"#,
                "0".repeat(32)
            ))
            .is_err()
        };
        assert!(bad(r#"{"sssLine": [1, 2, 3, 4]}"#) && bad(r#"{"cardPlates": [1, 2, 1e9]}"#) && bad(r#"{"row": 1}"#));
        assert!(!bad(r#"{"cardPlates": [1, 2, 3]}"#));
        assert_eq!(g.get("").id, "pplus");
        assert_eq!(g.get("gone-9.9").id, "pplus");
    }

    #[test]
    fn mods_play_direct_unranked_and_rooms() {
        let g = Games::default();
        let (pplus, cobalt, pex) = (g.get("pplus"), g.get("cobalt-1.34"), g.get("pex-1.8"));
        for mode in [Mode::Ranked, Mode::Unranked, Mode::Direct, Mode::Teams, Mode::Twos] {
            assert!(g.plays(pplus, mode), "{mode:?}");
            let mods = matches!(mode, Mode::Unranked | Mode::Direct | Mode::Teams);
            assert_eq!(g.plays(cobalt, mode), mods, "{mode:?}");
            assert_eq!(g.plays(pex, mode), mods, "{mode:?}");
        }
        let all = Games::parse(
            r#"{"games": [{"id": "a", "name": "A", "folder": "A"},
                           {"id": "b", "name": "B", "folder": "B", "codes": ["00000000000000000000000000000000"],
                            "modes": ["ranked", "unranked"]}]}"#,
        )
        .unwrap();
        assert!(all.plays(all.get("b"), Mode::Ranked) && !all.plays(all.get("b"), Mode::Direct));
    }

    #[test]
    fn a_mod_has_its_own_stages_or_the_shared_ones() {
        let g = Games::default();
        let shared = Rulesets::parse(crate::ruleset::DEFAULT_RULESETS).unwrap();
        let (pplus, cobalt, pex) = (g.get("pplus"), g.get("cobalt-1.34"), g.get("pex-1.8"));
        // P+Ex's stage select is P+'s: the shared lists.
        for mode in [Mode::Unranked, Mode::Direct] {
            assert_eq!(g.rules(pplus, mode, &shared), shared.for_mode(mode));
            assert_eq!(g.rules(pex, mode, &shared), shared.for_mode(mode));
        }
        // Cobalt's (the user's list, 2026-11-15; srStageKind as Cobalt's stage slots number them).
        // Each line of the list is one stage or two that count as one: starters Battlefield 0x01
        // / Zoness 0x13, Pokémon Stadium 2 0x2E / Midair Stadium 0x2A, Smashville 0x21, Castle
        // Siege 0x19 / Dive to the Heart 0x07, Radical Highway 0x4A / Luigi's Mansion 0x04;
        // counterpicks Final Destination 0x02 / High Entia Tomb 0x4C, Frigate Husk 0x0C, Green
        // Hill Zone 0x23 / Dead Line 0x44, Skyworld 0x1E. A room strikes game 1 among the five
        // first-named starters (1-2-1); their partners are picked with the counterpicks.
        // (Sectonia's Gem, Frigate Husk's partner, is Fountain of Dreams' slot 0x1F with L held:
        // a list of stage kinds can't name one alternate, so that slot is not in it.)
        let starters = [0x01, 0x2E, 0x21, 0x19, 0x4A];
        let counterpicks = [0x13, 0x2A, 0x07, 0x04, 0x02, 0x4C, 0x0C, 0x23, 0x44, 0x1E];
        assert_eq!(cobalt.starters, starters);
        assert_eq!(cobalt.counterpicks, counterpicks);
        let all: Vec<u16> = starters.iter().chain(&counterpicks).copied().collect();
        for mode in [Mode::Unranked, Mode::Direct] {
            let r = g.rules(cobalt, mode, &shared);
            assert_eq!(r.stages, all, "{mode:?}");
            assert_eq!(r.items, 0);
            // No Fountain of Dreams in Cobalt's list: no stage keeps its hazards.
            assert!(r.rules.hazard_stages.is_empty());
            assert_eq!(r.rules.stocks, 4);
        }
        assert_eq!(g.rules(cobalt, Mode::Unranked, &shared).rules.pause, common::rules::Pause::Hold);
        assert_eq!(g.rules(cobalt, Mode::Direct, &shared).rules.pause, common::rules::Pause::Normal);
        assert!(pex.starters.is_empty() && pplus.starters.is_empty());
    }

    #[test]
    fn rex_plays_pplus_stages_by_its_own_kinds() {
        // REX 0.5 plays P+'s lists (the user's, 2026-11-16: "Use the P+ legal stage list"), the
        // same stages by name in REX's own slots (its StageTable.asm): Temple of Time is REX's
        // 0x29 (P+ 0x09), Fountain of Dreams 0x07 (P+ 0x1F), Metal Cavern 0x79 (P+ 0x05),
        // Bowser's Castle 0x6B (P+ 0x06); the rest have P+'s kinds.
        let g = Games::default();
        let shared = Rulesets::parse(crate::ruleset::DEFAULT_RULESETS).unwrap();
        let rex = g.get("rex-0.5");
        let to_rex = |k: u16| match k {
            0x09 => 0x29,
            0x1F => 0x07,
            0x05 => 0x79,
            0x06 => 0x6B,
            k => k,
        };
        let starters: Vec<u16> = common::rules::PPLUS_STARTERS.iter().map(|&k| to_rex(k)).collect();
        let counterpicks: Vec<u16> = common::rules::PPLUS_COUNTERPICKS.iter().map(|&k| to_rex(k)).collect();
        assert_eq!(rex.starters, starters);
        assert_eq!(rex.counterpicks, counterpicks);
        for mode in [Mode::Unranked, Mode::Direct] {
            let (p, r) = (shared.for_mode(mode), g.rules(rex, mode, &shared));
            let want: Vec<u16> = p.stages.iter().map(|&k| to_rex(k)).collect();
            assert_eq!(r.stages, want, "{mode:?}");
            assert_eq!(r.items, p.items);
            // Fountain of Dreams keeps its hazards, as P+'s (its platforms move).
            assert_eq!(p.rules.hazard_stages, vec![0x1F]);
            assert_eq!(r.rules, common::rules::MatchRules { hazard_stages: vec![0x07], ..p.rules.clone() }, "{mode:?}");
        }
    }

    #[test]
    fn bad_files_are_refused() {
        let code = "00000000000000000000000000000000";
        for bad in [
            r#"{"games": []}"#.to_string(),
            r#"{"games": [{"id": "A", "name": "A", "folder": "A"}]}"#.into(),
            r#"{"games": [{"id": "a", "name": "", "folder": "A"}]}"#.into(),
            r#"{"games": [{"id": "a", "name": "A", "folder": ""}]}"#.into(),
            r#"{"games": [{"id": "a", "name": "A", "folder": "A"}, {"id": "a", "name": "B", "folder": "B", "codes": ["CODE"]}]}"#
                .replace("CODE", code),
            // A mod without codes, with a bad code, in the default's folder, with a code twice.
            r#"{"games": [{"id": "a", "name": "A", "folder": "A"}, {"id": "b", "name": "B", "folder": "B"}]}"#.into(),
            r#"{"games": [{"id": "a", "name": "A", "folder": "A"}, {"id": "b", "name": "B", "folder": "B", "codes": ["XYZ"]}]}"#.into(),
            r#"{"games": [{"id": "a", "name": "A", "folder": "A"}, {"id": "b", "name": "B", "folder": "A", "codes": ["CODE"]}]}"#
                .replace("CODE", code),
            r#"{"games": [{"id": "a", "name": "A", "folder": "A"}, {"id": "b", "name": "B", "folder": "B", "codes": ["CODE", "CODE"]}]}"#
                .replace("CODE", code),
            r#"{"games": [{"id": "a", "name": "A", "folder": "A", "modes": ["rankd"]}]}"#.into(),
            r#"{"games": [{"id": "a", "name": "A", "folder": "A", "rulesets": {"unranked": {"stages": [1, 1]}}}]}"#.into(),
            r#"{"games": [{"id": "a", "name": "A", "folder": "A", "starters": [1], "counterpicks": [1]}]}"#.into(),
            r#"{"games": [{"id": "a", "name": "A", "folder": "A", "counterpicks": [1]}]}"#.into(),
            r#"{"games": [{"id": "a", "name": "A", "folder": "A", "stagez": [1]}]}"#.into(),
        ] {
            assert!(Games::parse(&bad).is_err(), "{bad}");
        }
        assert!(Games::load(Some("does/not/exist.json")).is_err());
        assert_eq!(Games::load(None).unwrap(), Games::default());
    }
}
