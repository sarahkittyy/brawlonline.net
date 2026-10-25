# Rulesets: format, accounts API, links

_Phase 3a of `docs/design/room-rules.md` §8 (2026-10-11): the ruleset format, publishing and storage in accounts, the launcher's Rulesets page and code box, the `brawlonline://` URL scheme and the website's `/r/<code>` page. mm (rooms created with a ruleset, the in-game Ruleset line), Dolphin and the game read what is described here; they are built separately._

---

## 1. The format

`common::rules::Ruleset` (`server/crates/common/src/rules.rs`), JSON in camelCase. Missing fields take the defaults (P+ default, below); an unknown field is refused, so an older server or launcher never drops a field it does not know.

| Field | Values | Notes |
|---|---|---|
| `format` | `1` | `RULESET_FORMAT`. A newer format is refused: "This ruleset was made by a newer version. Update to use it." |
| `name` | 1-32 characters | What a display name may hold (printable ASCII, Hiragana, Katakana; no `\` or `` ` ``): the game draws it. No leading or trailing spaces (the server trims). Not unique. |
| `version` | 1, 2, ... | Assigned by the server when published; 0 in drafts and files. |
| `by` | text | The publisher's display name, filled by the server (follows renames; the name at publishing once the account is deleted). Ignored when sent. |
| `rules` | `MatchRules` | Every field of the room rules: `style`, `stocks`, `timeMinutes`, `damageRatio`, `teamAttack`, `pause`, `itemFrequency` (0-3: None, Low, Medium, High), `itemSwitch` (P+'s ITEM SWITCH, 8 bytes, bit `i` = cell `i` of its screen: `docs/game-code.md` §14, "Item switch layout"; the launcher and the website show the items, never the bytes), `hazards` (default `false`, stage hazards off, since 2026-10-12), `autoLCancel`, `inputAssist`, `stockControl` (2026-10-15: P+'s STOCK CONTROL, each player picks their own stock count on their panel; stock matches only; written **only when `true`**, so every ruleset from before it keeps its JSON and hash, and an older launcher or game reading one that has it refuses it: "Update to ...") (`room-rules.md` §3). |
| `stages.starters` | stage ids | `srStageKind` (1-255), at least one. Game 1 of a set, and every game with `stageChoice: "strike"`, strikes from these. |
| `stages.counterpicks` | stage ids | The extra stages of games 2+: games 2+ pick from starters **and** counterpicks. No stage is in both lists. The launcher offers P+'s 15 legal stages (`PPLUS_LEGAL_STAGES`). |
| `bestOf` | `0`, `3`, `5`, `7` | 0: no set, one game at a time. A ruleset is for any room (no player count since 2026-10-11, the user's decision): a set is played in a room that makes two sides (two players, or two teams of two); elsewhere its games are single games on a random stage of the ruleset's. |
| `stageChoice` | `"loser-picks"`, `"loser-picks-list"`, `"strike"`, `"random"` | No set only (with a set it is `"loser-picks"` and unused). `"loser-picks"`: game 1 random, then the loser picks any stage of P+'s stage select (the lists only feed the random stages). `"loser-picks-list"` (2026-10-12): the same, but only the listed stages (the stage select greys the rest). `"strike"` strikes between two sides; elsewhere a random stage. |
| `alternateStages` | bool | 2026-10-19, default `false` (the user's): P+'s alternate stages. On, every stage pick (a loser's, a set's counterpick) plays the layout the picker held L, R or Z for; off, every stage is played on its normal layout, whatever is held. Only with a pick (a set, or `"loser-picks"` / `"loser-picks-list"`): refused with `"strike"` / `"random"` without a set. Written **only when `true`**, as `stockControl`: every ruleset from before it keeps its JSON and hash (and plays without alternates). |
| `strikes` | e.g. `[1, 2, 1]` | The strike pattern over the starters, turns alternating from the first striker (`[1, 2, 1]`: first strikes 1, the other 2, the first 1). The sum is the starter count minus one: it leaves exactly one stage. Used by sets and `"strike"`; empty otherwise. |
| `bans` | 0-3 | Games 2+ of a set: the winner bans. Fewer than the stages. 0 without a set. |
| `dsr` | bool | Games 2+ of a set: the loser can't pick a stage they won on. false without a set. |

**No player count.** A ruleset has no `players` (1v1 / 2v2 / ffa) since 2026-10-11 (`room-rules.md` decision G): any ruleset works in any room. An older draft, file or published ruleset's `players` key is read and dropped (`Dropped`), never written, and is not in the canonical JSON or the hash (the launcher drops it on import too). `format` stays 1.

**Normalizing.** `Ruleset::normalize()` clears what the format leaves unused (with a set `stageChoice` is `"loser-picks"`; without one `bans` 0 and `dsr` false; `strikes` empty unless striking; `alternateStages` false without a pick; the name trimmed), so two rulesets that play the same have the same JSON. The server normalizes before it validates and publishes; `validate()` refuses an unused field with a value.

**Hash.** `Ruleset::hash()`: SHA-256 (lowercase hex) of `canonical_json()`: every field except `by`, keys sorted at every level, no whitespace, numbers and strings as serde_json writes them (`1.0` for a damage ratio of 1). The name and version are in it: the hash names one published snapshot. accounts stores exactly the canonical bytes (`rulesets.json`, text) next to the hash. A test pins one hash, so a change to the canonical form is noticed.

**Codes.** 6 letters of the room codes' 20 (`BCDFGHJKLMNPQRSTVWXZ`), random, assigned by accounts. `parse_ruleset_code` accepts any case, full width and surrounding spaces.

**P+ default** (`Ruleset::default()`, what "no ruleset" means): P+'s competitive `MatchRules`, Ranked's starters (Battlefield, Luigi's Mansion, Temple of Time, Smashville, Pokémon Stadium 2) and counterpicks (Frigate Husk, Fountain of Dreams, Green Hill Zone, Dream Land), no set, loser picks. These nine are the user's list (2026-10-12) and also the server's Unranked and Direct `stages` (`server/config/rulesets.json`), so a game without a set draws its random stage from all nine.

## 2. Storage (`server/migrations/0008_rulesets.sql`)

| Table | What |
|---|---|
| `rulesets` | Published versions: `code` (PK), `owner` (NULL once the account is deleted), `lineage` (every version of one ruleset), `version`, `name`, `publisher_name`, `json` (canonical, text), `hash`, `hidden`, `created_at`. A trigger refuses every change but `hidden` (and the owner going to NULL) and every delete, except in an operator's transaction (`pp-admin ruleset`, §2.2). |
| `ruleset_plays` | `code` (PK, the version), `games`: games played with it (§2.1). `0014_ruleset_plays.sql`; a row per version, 0 when published. |
| `ruleset_drafts` | The owner's unpublished rulesets (`json` as sent; may not validate), `based_on` (the published version it edits). |
| `remembered_rulesets` | One per account (decision 11). No row: P+ default. |
| `ruleset_recent` | The rulesets the account made its remembered one, newest 10 kept. |

mm's role `pp_mm` may read `rulesets` and read and write `remembered_rulesets` and `ruleset_recent` (`common::db::fetch_ruleset`, `remembered_ruleset`, `set_remembered_ruleset`, `recent_rulesets`, `newer_ruleset_version`), never drafts; it reads `ruleset_plays` and adds to `games` (`count_ruleset_game`). `e2e/tests/mm_grants.rs` checks it.

### 2.1 Games played (the user's, 2026-10-19)

Each published version counts the games played with it, for the public list's order (most played first) and the rows' "played N times". mm adds one (`Output::RulesetPlayed`, `server/crates/mm/src/rooms.rs`) for a game in a room that plays the version unchanged:

- **In a set**, each game both sides report the same (`room-result`, `Reported::Counted`), however short. A quit (void) or a salty runback is no game; a game lost by leaving mid-game is not counted either.
- **Without a set**, a game that lasted 30 s (`COUNT_GAME_AFTER`, from the match start, loading included) once everyone is back. A runback restarts the same match: one game. A game that ran past `game_max` without its end reported counts nothing.
- **Never** once the host edits the room's rules, stages or best-of (the room plays Custom), nor in Ranked, Unranked, Direct, a P+-default room or Other Games rooms. One game is one count, whatever the number of players; watchers add nothing.
- **A new version starts at 0**: its predecessors' games stay theirs (the user's: a version with one rule changed "doesn't deserve the plays").

Counting is best effort (a database error is logged). The migration seeded every version **once** with the estimate the match records gave: every room match recorded with its code since `0009_match_rulesets.sql` (2026-10-11), one per match, so a whole set counted as one game and quick quits counted too. From then on only mm adds.

### 2.2 Operator changes (`pp-admin ruleset`)

`pp-admin ruleset rename CODE NAME [--all-versions]` renames a version (with the flag, every version of its ruleset): the `name` column, the JSON and so the hash. The name must pass the publisher's rule (`Ruleset::validate`). `pp-admin ruleset delete CODE [--all-versions] --yes` deletes it for good: its `/r/` link stops working, its game count goes, and so does every account's remembered or recent use of it; a draft made from it keeps its rules but no longer edits it. Both print the ruleset's versions they left alone and write an audit row (`ruleset.rename`, `ruleset.delete`). They set `pp.ruleset_admin` for their transaction, which the trigger lets through. Match history keeps the name and hash a game was played under (a deleted one's link finds nothing). A room playing a deleted version in mm plays it on until its rules change, and counts nothing.

## 3. accounts API

JSON, errors as everywhere in accounts (`{"error": {"code", "message"}}`, 429 with `Retry-After`). Session auth (`Authorization: Bearer`) except the lookup. Limits: `server/README.md`, "Rate limits".

A **ruleset view** (`RulesetView`):

```json
{
  "code": "KFQBTR",
  "ruleset": { "format": 1, "name": "NorCal Weekly", "version": 3, "by": "sarah", "rules": {...}, "stages": {...}, ... },
  "hash": "2fa7e67d...",
  "createdAt": "2026-10-11T21:30:00Z",
  "newer": { "code": "MNPQRS", "version": 4 },
  "link": "https://brawlonline.net/r/KFQBTR",
  "hidden": false,
  "games": 1423
}
```

`newer` is the newest later version that is not hidden (null if none). `hidden` is only in the owner's own lists. `games` is how many games were played with this version (§2.1).

| Endpoint | What |
|---|---|
| `POST /v1/rulesets` | Publish. Body `{"draftId": "<uuid>"}` (the draft is deleted once published) or `{"ruleset": {...}, "basedOn": "<code>"?}`. Based on one of the account's own rulesets: that ruleset's next version (409 `unchanged` if it plays the same as the newest version, ignoring `version` and `by`); based on someone else's, or on nothing: version 1 of a new ruleset. 201 with the view. 400 `invalid_ruleset` with the reason; 404 `unknown_ruleset` for a `basedOn` nobody published. Needs an account that can play online: 403 `email_not_verified` (when verification is required), 403 `not_activated` (no connect code yet). |
| `GET /v1/rulesets?q=&limit=&after=` | Public: the public list (the user's decision, 2026-10-12: **every published ruleset is public**). Of each ruleset (lineage) its newest version that is not hidden, the most played first (that version's own `games`, §2.1), then newest, in pages of `limit` (default 30, at most 100; **defaults**): `{"rulesets": [view...], "next": cursor | null}`, `after` = the last page's `next`. `q` (64 characters at most) matches the name, the publisher's name and the code, case-insensitively, anywhere in them (`%` and `_` are plain text). Limited per IP together with the code lookup (**default**). 400 `bad_cursor`. Indexes: `0010_rulesets_browse.sql`. |
| `GET /v1/rulesets/<code>` | Public: any published ruleset, hidden ones too. 404 `unknown_ruleset`. Limited per IP. |
| `GET /v1/rulesets/<code>/preview` | Public: an HTML page for link previews (Discord and the like), titled `<name> v<version> by <publisher>` (also `og:title`), with no description, so the embed shows only that. nginx sends unfurlers' `/r/<code>` here (§4). 404 `unknown_ruleset`. Limited per IP with the lookup. |
| `GET /v1/me/rulesets` | `{"drafts": [draft...], "published": [view...]}`: drafts newest edit first, published newest first, hidden ones included. |
| `GET /v1/me/rulesets/recent` | `{"rulesets": [view + "usedAt"...]}`, newest first, at most 10. |
| `PUT /v1/me/rulesets/<code>/hidden` | `{"hidden": true}`: off the public list (a hidden newest version shows the one before it there; **default**) and off "newer version" links; the owner still sees it in their own list (marked hidden); the code and link keep working. 204; 404 unless it is the account's. |
| `POST /v1/me/ruleset-drafts` | `{"ruleset": {...}, "basedOn": "<code>"?}`. 201 with the draft. 409 `too_many_drafts` at 50. |
| `GET` / `PUT` / `DELETE /v1/me/ruleset-drafts/<id>` | Read, replace (same body as POST), delete (204). 404 unless it is the account's. |
| `GET /v1/me/ruleset` | `{"ruleset": view | null}`: the remembered ruleset (null: P+ default). |
| `PUT /v1/me/ruleset` | `{"code": "<code>" | null}`: sets it (null: P+ default); a code also goes to the top of the recent list. Answers like GET. 404 `unknown_ruleset`. |

A **draft**: `{"id", "basedOn", "ruleset", "problem", "createdAt", "updatedAt"}`. `problem` is why it can't be published yet (`validate()` after `normalize()`), or null. A draft's `version` is 0 and `by` empty.

Published versions are never changed or deleted, by the API or by hand (the trigger). A tournament's link always shows the same rules; an old version's page links the newest one.

## 4. Launcher and links

- **Rulesets page** (launcher, `/rulesets`), one column top to bottom (the user's layout, 2026-10-12): drafts (create, edit every field, save, delete), the account's published rulesets (hide / show, edit as a new version, copy the link), then **Public rulesets** with a search bar (300 ms debounce, **default**) and infinite scrolling over `GET /v1/rulesets`. Each row: name, version, "by <publisher>", the code with a copy icon to its left (a click on either copies the code; 2026-10-19), and the set and its games ("Bo3 · strikes 1-2-1 · 2 bans · played 1423 times"); "Use" (was "Use for my rooms") and no copy-link icon on the row. A published or public row opens the read-only preview (rules, set, strikes, bans, stages, items with icons) with the same code chip in its header (no copy-code button at the bottom), Copy link and Use, plus edit and hide for the account's own; a draft opens in the editor. Both lists refresh with the refresh button, when the page opens and when the window comes back into focus. Import and export of a ruleset as a JSON file (the format above; `by`, `version` ignored on import).
- **Code box** next to Home's **Create room**: the remembered ruleset's code with its name and publisher beside it, read from and saved to `GET`/`PUT /v1/me/ruleset`; X clears it (P+ default). Create room writes the code into `join-room.json` (`docs/rooms-game-interface.md`): `{"kind": "create", "ruleset": "<code>"}`.
- **URL scheme** `brawlonline://`, registered on Windows, macOS and Linux (electron-builder `protocols`; `app.setAsDefaultProtocolClient` for unpackaged and AppImage runs). `brawlonline://room/create?ruleset=<code>` makes the code the remembered ruleset (when logged in) and creates a room with it, as Create room does; `&private=1` (or `true`) makes that room private (the create request's `"public": false`, 2026-10-24). Anything else is ignored (logged).
- **`https://brawlonline.net/r/<code>`** (`website/r/index.html`): the ruleset (name, by, version, every rule, the stages, the set format, a "newer version" link) from `GET /v1/rulesets/<code>`, with **CREATE PUBLIC ROOM** and **CREATE PRIVATE ROOM** side by side at the top (the user's, 2026-10-24; they open `brawlonline://room/create?ruleset=<code>` and the same with `&private=1`), "Opens Brawl Online & creates a room with this ruleset." and, on its own line, "Private rooms can only be joined via the room code." under them, and a download link for visitors without the launcher. nginx serves `r/index.html` for every `/r/<code>`, except to link previews (a `User-Agent` with Discordbot, Twitterbot, Slackbot, facebookexternalhit, TelegramBot, WhatsApp, ... in it; `$brawl_unfurler` in the nginx site): they don't run the script, so they get `GET /v1/rulesets/<code>/preview`, and the embed reads e.g. "Auto L Cancel v1 by sarahkitty" with no description (the user's, 2026-10-12). The static page has no description either.
