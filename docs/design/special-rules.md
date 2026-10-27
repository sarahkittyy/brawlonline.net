# Special Versus in rooms, and the rules menu's new pages (design)

_2026-10-27, Sarah's board: "there's a few rules that are only available in the launcher's ruleset editor ... i wish we could make some room for them ... the special versus menu with things like all-star, turbo, bunny mode, metal mode ... people would love to play in rooms ... access these from a regular room css ... using as much built in code/ui as possible". Her answers the same day: L/R page tabs; SET and RULESET stay where they are, stock control etc. move to the new pages; one L/R page is entirely the Special Versus box "just like the original menu looked"; every special mode implemented and tested; locked during a set like every other rule; any change makes the room Custom; part of the launcher's rulesets, the editor's Items section becoming tabs (Items, Stages, Special, ...); existing rulesets keep everything off, only new rulesets can turn these on._

Builds on `room-rules.md` (rooms' rules, rulesets, sets) and `docs/game-code.md` §14 (the RULES button, P+'s rules menu over the room CSS).

---

## 1. Decisions (as built)

| # | Topic | Decision |
|---|---|---|
| 1 | Where | Rooms only (as every room rule). Ranked, Unranked and Direct keep their fixed rules (their SESSION `special` is all zero). |
| 2 | Pages | The RULES button's menu has **L / R page tabs**: RULES (P+'s) ↔ MORE RULES (P+'s) ↔ **MATCH SETUP** (ours) ↔ **SPECIAL** (P+'s Special Versus box). R on RULES opens MORE RULES as its bottom button does; L on MORE RULES goes back. L/R keep their meaning on the item and stage switches (P+'s presets). |
| 3 | RULES page | SET and RULESET stay on RULES' STOCK CONTROL / STAGE CHOICE rows (unchanged). |
| 4 | MATCH SETUP page | The launcher-only rules, in P+'s MORE RULES look (its rows relabelled, as RULESET / SET are): BANS (the TIME LIMIT row's digit, at its colon), DSR, STAGE CHOICE (LOSER / LIST / STRIKE / RANDOM; SET in a set), STRIKES (the pattern), STOCK CONTROL (FRAME CONTROL with ATTACK: REGEN, as P+ renames it), and the bottom button ALT STAGES (A toggles). The footer is each row's help. |
| 5 | SPECIAL page | **P+'s own Special Versus box** over the room CSS (the models of P+'s `muProcSpecial`): STAMINA (OFF / 300% / STAMINA), SIZE (MEGA / MINI), ATTACK (ELEMENT / REGEN), BODY (METAL / CLEAR), STATUS (TURBO / BUNNY), GRAVITY (LIGHT / HEAVY), SPEED (WILD / FAST), OTHER (METER / ALL-STAR), its title band ("300% Mega Versus", P+'s words) and P+'s description of the cursor's row and value in its white area. STAMINA TYPE (P+'s RULES row in stamina) is Y / X on the STAMINA row (the footer names it). |
| 6 | Modes | **Every** special mode is played online, All-Star and Stamina included: each player's HP and All-Star roster travel with their lock-in, as stock control's count does. |
| 7 | Locking | As every room rule: host only, between games, never during a set (the pages are read-only then); any change makes the room **Custom** and takes back every ready. |
| 8 | Rulesets | Every new rule is a ruleset field, **written only when not default**: every published ruleset keeps its JSON, hash and behaviour (all off). |
| 9 | Launcher | Under the rules the editor has **tabs: Stages, Items, Special** (Stages first, the most edited; stock control stays in Rules, the set rows at the top). The website's ruleset page lists the special rows. |
| 10 | Item frequency | P+'s VERY HIGH, INTENSE and BOMB RAIN (4-6, on P+'s own ITEM SWITCH page) are allowed. |
| 11 | Stamina HP | Each player's own, on their panel's box: Brawl's own HP box and list (P+'s 100 entries, 10 HP apart), labelled "HP x10" (the box shows HP / 10, as P+'s does). None picked: 150. With stamina there is no stock control (one box). |
| 12 | All-Star roster | P+'s own CSS code builds it: each coin placed adds the character (X / Y on a placed coin change its costume, as P+). **X / Y with the coin in the hand take the last one off** (ours: P+ has no way to). The corner shows "All-Star: N / X: remove last" while the coin is in the hand. At most 16 go online. |

## 2. What P+ keeps where (RE, 2026-10-27, P+ v3.2.1)

- **The 8 special bytes**: set rule (`gmSetRule`, `GameGlobal+0x1C`) +0x18..+0x1F, one per row of the box in order, each 0 (OFF), 1 (the left value) or 2 (the right): STAMINA (1 = 300%, 2 = stamina), SIZE, ATTACK (1 = random element, 2 = regen / zero-to-death), BODY, STATUS (1 = P+'s Turbo, Brawl's curry; 2 = bunny), GRAVITY, SPEED (1 = P+'s Wild, Brawl's slow; 2 = fast), OTHER (1 = P+'s Final Smash meter, Brawl's fixed camera; 2 = All-Star).
- **STAMINA TYPE**: with STAMINA = 2, P+'s RULES page shows STAMINA TYPE 1-5 on its STAGE CHOICE row: set rule +7 = 0-4 (`Stamina.asm`, its hook in `setupSpMelee` at `0x806DF0E4`: blast zones and knockback).
- **FRAME CONTROL**: with ATTACK = 2, P+'s STOCK CONTROL row is FRAME CONTROL (set rule +5, the handicap byte; P+'s hook at `0x80693138` keeps the CSS's handicap mode in any style), and P+'s "Handicap = Stock Count" is skipped.
- **Stamina HP on the CSS**: Brawl's Special Brawl CSS sets `muSelCharTask+0x5D0` when it starts with stamina (sel_char text+0x2AC; P+ hooks it). With it the panel's handicap box (`area+0xD4`, P+'s STOCKS look) is the HP box and A opens Brawl's list kind 3 (P+: `li r29, 0x64`, 100 entries). The pick is `area+0x1D0` = HP / 10 (the CSS reads a record's `+0x24` / 10 into it, text+0x1C44). The label texture is `MenSelchrCardHandicap` (I4 80x16, model `MenSelchrChandicap4`).
- **All-Star** (`AllStarVs.asm`, `50CC.asm`): per port a roster at `0x80588000 + 0xA0 * port`, 3-byte entries {character, P+'s colour number (or 0x3D AltZ / 0x3E AltR), the CSS's costume index (area +0x1BC)} up to an `0xCC`; `+0x99` 0xFF, `+0x9C` the entry being played (on the CSS: the append offset), `+0x9D` how many were played, `+0x9E` the stock icons' index. Its CSS twin at `0x80588300 + 0x70 * port`: 2-byte entries {CSS id, costume}, `+0x68` their bytes, `+0x69` 0xAC. The CSS hook (`0x8069680C`) appends on each placed coin (a RANDOM coin as `0x3D`) and rewrites the last on a costume change; the Gecko lines reset `+0x99` / `+0x9C` every frame while OTHER is not 2. The Special setup's hook (`0x806DEFD4`) gives each port its entry 0 (`gmGlobalModeMelee` player +0x00, +0x05 / +0x06); the respawn hook (`0x8077FB7C`) moves `+0x9C` on and gives the next one (wrapping at the 0xCC), and writes the word at `0x805882F4`. P+ draws a RANDOM entry there with the game's random numbers.
- **The match**: Brawl's Special setup (`sqSpMelee::setupSpMelee`, sora_scene `0x806DEE98`) is the Versus setup (`0x806DCE94`) plus a loop that turns the 8 bytes into the match: per player start damage / HP (`+0x22` / `+0x24`), body / head flags (`+0x1B`, `+0x1C`), size (`+0x3C`, scale `+0x40`), gravity (`+0x48`), and globally the game speed (`gmGlobalModeMelee+0x50`) and camera bits (`+0x0D`). P+ hooks inside it (stamina, regen, element, All-Star, BOSS tags).
- **The box** (`muProcSpecial`, sora_menu_main): models `MenMainSpecial0001` (frame, OK button: CLR 0 / 1 highlighted; text bones `textN0` the title band and `textN1` the description, each window between its `textL` / `textR` bones), `0002` (the cursor: VIS = column 0-2, 3 on OK; CHR = row 0-7, 8 on OK), `0003`..`0009`, `0010` (rows STAMINA..OTHER: CLR `3 + 4 * value`, CHR 9), with P+'s draw priorities (frame 0x11, rows 0x1A, cursor 0x28); textures in the archive's texture brres; the texts in its msbin (lines 0-33 the title words, `34 + 3 * row + value` the descriptions). They live only in P+'s main-menu archive (`/Project+/pf/menu2/mu_menumain.pac`, LZ11, 12.8 MB unpacked, nested ARC `MenuMain_en`).

## 3. The rules record (additions)

`common::rules::MatchRules` (JSON camelCase; each omitted when default):

| Field | Values | Game |
|---|---|---|
| `itemFrequency` | 0-6 (4 very high, 5 intense, 6 bomb rain) | menu record +0x810 |
| `special` | object, keys `stamina` (`"300"` / `"stamina"`), `size` (`"mega"` / `"mini"`), `attack` (`"element"` / `"regen"`), `body` (`"metal"` / `"clear"`), `status` (`"turbo"` / `"bunny"`), `gravity` (`"light"` / `"heavy"`), `speed` (`"wild"` / `"fast"`), `other` (`"meter"` / `"all-star"`); a key that is off is left out (`"off"` is read), an empty object is left out | set rule +0x18..+0x1F |
| `staminaType` | 1-5, only with `special.stamina: "stamina"` (normalized to 1 otherwise, then left out) | set rule +7 = type - 1 |
| `stockControl` | bool (as before); stock matches, or with `attack: regen` P+'s FRAME CONTROL in any style; not with stamina | set rule +5 = 2 |

A ruleset's `stageChoice`, `strikes`, `bans`, `dsr` and `alternateStages` are as before; the game now edits them (§4). `MatchRules::normalize` (the stamina type, sorted hazard stages) runs on every ruleset and on the host's `room-rules`.

## 4. Room protocol, SESSION and the lock-in

- `room-rules` carries the new fields (the room's `MatchRules`).
- `room-format {stageChoice, strikes, bans, dsr, alternateStages}` (host, between games, no set on): the MATCH SETUP page. The room's ruleset with them is normalized and validated as a ruleset ("The room's stages can't play that." otherwise); makes the room Custom, takes back every ready.
- `room-state.ruleset` gains `strikes`, `bans` and `dsr` (left out when empty / 0 / false). `room-ready` gains `hp` (a stamina match's pick, 1-999), shown on the member to everyone while they are ready in a stamina room.
- **PPOM v9** (`ppom.h`, `docs/rooms-game-interface.md`):
  - SESSION 0x420: `Special` (0x40) at +0x2C0 (the 8 rows, the stamina type, the room's bans / DSR / strike pattern / alternate stages for MATCH SETUP, each slot's picked HP) and `PortExtra extra[4]` (0x48 each) at +0x300: each player's HP and All-Star roster from their lock-in.
  - LOCAL 0xC8: `ownExtra` at +0x80, written with the lock-in (its seq covers it).
  - `ROOM_RULES` gains the 8 rows and the stamina type; `ROOM_FORMAT` (op 14) is `room-format`.
  - `CMD_SPECIAL_ASSETS` (0xD2) `{addr, capacity, nonce}`: the box's files (§5); the answer `{state, written, size}`.
- The lock-in exchange (`GameplaySession`, the lock JSON's `px`) carries `PortExtra`; `PeerData::SanitizePortExtra` keeps a peer's HP up to 999 and its roster up to the first entry the game cannot load (character, colour, costume, CSS id).
- **The setup key** (`AppendSpecialModes`) adds, only when a row is on, the 8 rows, what the Special setup made for each player (+0x1B without the rumble bit, +0x1C, +0x22..+0x25, +0x3C..+0x43, +0x48..+0x4B), the speed (+0x50) and, with All-Star, both roster tables whole. Every other game's key is as before (replays compare it).
- **Rollback region set gp-v24** (the default): gp-v23 plus `0x80588000`, 0x4C0 bytes (the rosters, `0x805882F4`, the twins), which the respawn and stock-icon hooks write during the match.

## 5. The game side (plugin, Dolphin)

- **Pages** (`rules_pages.cpp`): MATCH SETUP relabels MORE RULES' textures (copies in one Fighter1Resoruce block, freed when the menu closes, a frame before any game starts); BANS is the TIME LIMIT digit model moved to the colon's x (put back when the page goes). Edits are staged and sent when the menu closes (`ROOM_FORMAT`; stock control and the special rows with `ROOM_RULES`).
- **The box's files**: Dolphin (`Online/SpecialAssets.cpp`, on a thread) reads the archive from the SD card image (`MachineSDCard::ReadCardFile`), decompresses it, and keeps the box's ARC entries (texture brres, models 0001-0010, msbin: about 330 KB) in a bundle (`SPBX`, a role table, 32-aligned). The SPECIAL page allocates 0x80000 bytes in Fighter1Resoruce with a magic and nonce and asks with `CMD_SPECIAL_ASSETS`; Dolphin writes the bundle only while the magic and nonce are still there. The page says "Loading Special Versus..." until then ("needs Project+'s menu files" if the card has none). Freed when the menu closes.
- **The box** (`special_box.cpp`): the MuObjects made as `muProcSpecial` makes them, in the CSS's task group, MORE RULES' rows hidden under it; a MuMsg on the frame with two windows (the description at `textN1` in P+'s dark red, the title at `textN0` in P+'s grey). Up / down rows and OK, left / right / A the value, Y / X the stamina type, B (or A on OK) back.
- **The room CSS**: the set rule's special rows follow the room's rules every frame (`cssSpecialRows`), so P+'s CSS code runs as on its own Special Versus CSS (the All-Star roster). With stamina the boxes are HP boxes (`room_css.cpp stockBoxes`, task +0x5D0), each player's own, the others' showing their pick once ready.
- **The match** (`online_match.cpp`): with any row on, each player's HP into their record (+0x24), the rosters into P+'s tables (`all_star.cpp`: every port, both tables whole, entry 0 first), and Brawl's Special setup instead of the Versus one.

## 6. Tests

- Server: `common` (wire shape, validation), `mm` (`the_host_edits_the_rooms_format`).
- Dolphin unit tests: `MatchRules.SpecialModes`, `RoomMessages` (format, picked HP), `PeerData.PortExtra`, `SpecialAssets` (LZ11, the bundle).
- Harness, two Dolphins through the real server (`harness/tests/test_special_modes.py`): every value of every row (16), BOMB RAIN, All-Star (two rosters: both machines start with each roster's first, a lost stock brings the next on both, the tables identical, no mismatch), stamina HP (200 and 120 picked, the other's panel shows it, the match gives each theirs on both). The room CSS with a scripted Dolphin (roomsim): `run/scratch/spmelee/explore_*.py` screenshots (the pages, the box, the HP box, the roster hint).
- Launcher: `rulesets.test.ts` (special modes), `ruleset_editor.test.tsx` (tabs, special rows, frame control).

## 7. Not done / limits

- The others' panels show a player's locked-in character, not their All-Star roster.
- The HP box counts in tens (P+'s box); its label says so ("HP x10").
- Stamina with stock control: the box is the HP's; stock control does nothing then.
