# Launcher: rank, profile, leaderboard, match history and replays (design draft)

_Status 2026-10-08: mockups only, nothing built. All of this UI lives in the launcher. The website gets none of it for now (user decision, 2026-10-08)._

## 0. Summary

- **Template.** Wherever Slippi's launcher already has a screen, the mockup copies it: the Overview rank widget, the user menu, the replay browser (folder tree, file list, stats page) and the player badges. Slippi shows leaderboards, profiles and match history on slippi.gg, not in its launcher. Those screens are **new**. They use only components the launcher already has: `ContentBlock`, `Tabs`, `DualPane`, the `ReplayFile` card, `PlayerBadge`, `LabelledText`, `BasicFooter`, `Dropdown`/`Checkbox`/the search field from `FilterToolbar`, MUI tables, dialogs and menus.
- **Tiers are text only** ("Gold 2", "Grandmaster"). There is no badge art. The rating is the ordinal on Slippi's scale (backend-design.md section 4). Fewer than 5 ranked sets shows "Rank pending".
- **Game assets.** The look comes from the user's own extracted assets, as in the real launcher. Fonts, the tinted nine-slice frames on contained buttons and dialogs, and the palette sampled from P+'s gradient all come from them. Character icons are the extracted stock icons. Stage images are not used, because the launcher does not extract them.
- **Every finished set links to its replays, and every replay links to its set.** The link key is the server's `matchId` plus the game index. The launcher's replay database already has columns for both (section 7).
- **Two open choices are drawn twice:**
  - Navigation: Home tabs (A) or a new "Ranked" menu item (B).
  - Match history: expand in place (A) or a dual pane (B).
- **Recommendation.** Use A for both: no new top-level menu item, and the list stays one column. Use B's "Your position" card if the pinned bottom row in A turns out to be too subtle.

## 1. Mockup files

Everything is in `D:\code\pm_rollback\run\design\launcher-mockups\` (gitignored, under `/run/`).

- `index.html?s=<screen>` with `app.js` (screens), `mockup.css` (values copied from the components named in its comments), `data.js` (made-up players) and `theme.js`.
  - `theme.js` is a copy of `prepareTheme`/`buildThemeCss` from `src/renderer/styles/game_theme.tsx`. It reads the local extraction cache `launcher/.asset-cache/theme` by relative path, so the screens show the real tinted frames, fonts and stock icons without copying any asset.
- `icons.js` holds path data from `@mui/icons-material`, the same glyphs the launcher uses. `avatars.js` holds jdenticon identicons, the launcher's default display pictures.
- `render.sh` renders every screen with headless Edge at 1100x750 (the `BrowserWindow` size in `src/main/main.ts`).
  - On Windows the real content area is a little smaller, because the 750 includes the frame.
  - At this height the launcher's `height <= 850px` rule applies, so the rank widget is 130 px tall.

| PNG | Screen |
|---|---|
| `01-home-ranked.png` | Overview, ranked player |
| `02-home-rank-pending.png` | Overview, fewer than 5 ranked sets |
| `03-home-logged-out.png` | Overview, logged out |
| `04-home-after-set.png` | Overview after a ranked set (rating change, toast) |
| `05-user-menu.png` | User menu with "View profile" and "Match history" |
| `06-profile-own.png` | Own profile (nav A: sub-page of Home with a back arrow) |
| `07-profile-other.png` | Another player's profile with head-to-head |
| `08-leaderboard-a-home-tab.png` | Leaderboard, variant A (Home tab, region dropdown, own row pinned at the bottom) |
| `09-leaderboard-b-ranked-menu.png` | Leaderboard, variant B ("Ranked" menu item, region toggle, "Your position" card) |
| `10-match-history-a-expand.png` | Match history, variant A (set expands in place) |
| `11-match-history-b-dual-pane.png` | Match history, variant B (set list on the left, set detail on the right) |
| `12-replay-browser.png` | Replay browser with metadata and "View set" links |
| `13-replay-stats-set.png` | Replay stats page with the set panel |
| `14-replay-missing.png` | A replay missing locally: dialog from match history |
| `15-profile-b-ranked-tab.png` | Own profile in variant B (first tab of "Ranked") |

## 2. Navigation (open choice)

**A: Home tabs** (`01`, `06`, `08`, `10`). The Home page already has Slippi's tab strip (`pages/home/tabs`). Slippi used it for Overview/News/Tournaments, and only Overview survived the port. Add "Match history" and "Leaderboard" as tabs; `HOME_TABS` and `home_routes.ts` grow by two.
- Profiles are a sub-route, `/main/home/profile/:uid`, drawn with a back arrow like the replay stats page.
- They are reached from the rank widget ("View profile"), from the user menu, and from any player name in the leaderboard or match history.
- The header keeps Slippi's two icons (Home, Replays).

**B: "Ranked" menu item** (`09`, `11`, `15`). A third header icon between Home and Replays (MUI `EmojiEventsOutlined`) opens a page with the tabs Profile, Match history and Leaderboard. Home keeps only the widget.

Recommendation: **A**.
- It adds no top-level item.
- The Overview "Recent sets" block and the tabs sit together.
- Match history also covers unranked and direct sets, so a "Ranked" label would be wrong for it.

B fits better if more ranked features arrive (seasons and placements pages).

## 3. Screens

Endpoint names follow backend-design.md 2.4. **New** marks endpoints that do not exist yet.

### 3.1 Home / Overview (`01`-`04`)

- **Purpose:** your rank at a glance, plus your latest sets.
- **Slippi:** `pages/home/overview` with `MyRanking`/`RankedUserProfile` in the right sidebar. The rank name, rating and Refresh are unchanged. The Hide (x) button still appears on hover; it is not drawn in the PNG.
  - **Added:** a "View profile" link in the widget.
  - **Added:** a "Season" block under the widget: ranked sets and games W-L, global and regional placement, and most-played stock icons.
  - **Added:** a "Recent sets" block in the two left columns, where Slippi had its news and tournaments (dropped). Each row has result, characters, score, opponent, mode, time, rating change and "Watch set".
- **States:**
  - Ranked (`01`).
  - Pending, with fewer than 5 ranked sets (`02`): "Rank pending", "3 of 5 ranked sets played", a 5-step bar and no rating. Slippi hides the rating while pending.
  - 0 sets: "No ranking", as now.
  - Logged out (`03`): the sidebar is empty (`AuthGuard` renders nothing, as in Slippi), and the left area shows the existing `LoginNotice`. The Leaderboard tab stays usable logged out because its data is public.
- **Data:** `rank` (rating, tier, update count, placements), season totals, most-played characters, last 6 sets.
- **Source:**
  - `GET /user/{uid}?additionalFields=rank` (exists), extended with `globalPlacement`, `regionalPlacement` and `region`.
  - **New:** season totals and top characters (`GET /v1/users/{uid}/profile?season=`).
  - **New:** recent sets (`GET /v1/me/sets?limit=6`).
- **Open questions:**
  - Is the Season block worth it, or does the widget alone suffice? It duplicates the profile page.
  - Should "Recent sets" show unranked and direct sets? The mockup shows all modes.

### 3.2 Post-set rating change (`04`, optional)

- **Purpose:** confirm what a ranked set did to your rating.
- **Slippi:** shows it **in game** on the CSS (`RankInfo`, from `getRankedMatchPersonalResult`), not in the launcher. Our game shows it there too (backend-design.md 1.6). The launcher part is only a convenience.
- **Design:**
  - When the netplay Dolphin exits, or on Refresh, the launcher refetches the rank.
  - If the update count went up, it shows the launcher's existing toast (MUI `Alert` via notistack, bottom right): "Ranked set won 2-1 against KAZE#204. Rating 1598.2 → 1612.4 (+14.2)".
  - The widget shows the delta next to the rating until the next refresh.
- **Source:**
  - The rank refetch, plus the newest set from **New** `GET /v1/me/sets?limit=1` for opponent and score.
  - `GET /v1/ranked/result` is play-key authenticated (Dolphin's endpoint), so the launcher should not use it.
- **Open questions:**
  - Should it also fire while Dolphin is still open (polling)? Recommendation: no. The game already shows it.
  - Should there be a rank-up wording ("Gold 2 → Gold 3")?

### 3.3 User menu (`05`)

- **Slippi:** `user_menu_items.tsx`. Slippi had "View profile" and "Manage account", which opened slippi.gg; the port removed both.
- **Design:**
  - "View profile" comes back and opens the in-launcher profile.
  - "Match history" is added under it. It is optional and duplicates the tab.
  - "Manage account" stays out until the website has account pages.

### 3.4 Profile (`06`, `07`, `15`)

- **Purpose:** one player's rank, record and characters, plus your sets against them.
- **Slippi:** none in the launcher (slippi.gg/user/CODE). **New.**
  - The header copies the replay stats header (`game_profile_header`): back arrow, then the identicon (`UserIcon`, 64 px), name in the title font, connect code with a copy button, and a 3 px bottom rule.
  - The body uses `ContentBlock`s: Rank (tier, rating, placements), Record (ranked sets and games W-L), Most played (stock icon, sets, win rate) and Recent sets.
- **Other player** (`07`):
  - A "Head-to-head" block (all-mode sets and games, ranked sets separately) and "Your recent sets against <name>".
  - "Sets against <name>" opens match history filtered by that opponent.
  - The copy-code button helps with Direct.
- **Data:**
  - Public profile: name, code, rank, placements, region, season record, characters.
  - Head-to-head totals and the shared sets.
- **Source:**
  - **New:** `GET /v1/users/{uid}/profile?season=` (public).
  - **New:** `GET /v1/me/sets?opponent={uid}` (session).
  - Route by uid, not code: codes can change once (PPLUS_PORTING.md "Adaptations"). A lookup by code (`GET /v1/users/by-code/{code}`) serves the leaderboard search.
- **Open questions:**
  - Is a profile public to everyone? Slippi's are.
  - Should a player be able to hide their profile? Slippi's "Hide ranking" setting is local display only.
  - Should profiles show unranked sets of other players? Recommendation: no. Show only the record and ranked data, and the head-to-head only to the two players involved.

### 3.5 Leaderboard (`08` = A, `09` = B)

- **Purpose:** standings for a region and season.
- **Slippi:** slippi.gg leaderboards (three regions: North America, Europe, Other; refreshed every 2 minutes; backend-design.md 4.1). **New** in the launcher.
  - It uses the `FilterToolbar` layout (controls on the left, black search field on the right) and an MUI table: #, player (identicon, name, code), tier, rating, sets W-L, characters (stock icons).
  - The `BasicFooter` carries "Updated 2 minutes ago", the player count and paging.
- **Your row:**
  - A: pinned at the bottom of the table and highlighted, with "Jump to my position" in the footer.
  - B: a "Your position" card above the table (regional and global place, rating, "Show me").
  - Unranked or pending players get no pinned row and a hint instead ("Play 5 ranked sets to appear").
- **Region filter:** A uses Slippi's `Dropdown` (Global, North America, Europe, Other). B uses a toggle row, which shows all regions at once.
- **Paging:** 50 per page. A: first/previous/next/last with "1-50 of 3,412". B: "Page 1 of 23".
- **Grandmaster:** rating ≥ 2191.75 **and** a leaderboard placement (calculate_rank.ts). The server owns the placement, so the tier column comes from the server, not from the rating alone.
- **Data and source:**
  - `GET /v1/leaderboard?region=&season=&page=` (listed in 2.4, not built). It returns rows `{place, uid, displayName, connectCode, rating, rank, setsWon, setsLost, topCharacters[]}`, plus `updatedAt` and `total`.
  - **New:** `GET /v1/leaderboard/me?region=&season=` for the pinned row and "jump to me".
- **Open questions:**
  - Region assignment: chosen by the player, or from matchmaking geo-IP (2.3)? Slippi's regions come from the account.
  - Should the leaderboard require a minimum number of sets or recent activity?
  - Characters column on or off (the toolbar checkbox)?
  - Tier text colour: plain (current port) or Slippi's per-tier colours from `get_rank_details.ts`? Those are colours, not art.

### 3.6 Match history (`10` = A, `11` = B)

- **Purpose:** all your online sets (Ranked, Unranked, Direct, Teams), with the games inside each set and a way to watch them.
- **Slippi:** none in the launcher. Its replay browser groups games by session only implicitly. **New.**
  - A set card reuses the `ReplayFile` card. Row 1: result, `PlayerBadge`s (stock icon, code, port colour, winner glow), score, rating change, "Watch set", expand. Row 2 uses the `InfoItem` details (date, mode, total time, "2 of 3 replays") and shows the opponent's tier and rating on the right.
  - **A:** expanding shows one row per game: stage name, characters, stocks left, duration, result, then "Show stats" and "Watch replay", or "Replay not on this computer" with "Locate…".
  - **B:** a compact list on the left (`DualPane`, like the replay browser) and the set detail on the right. The header copies the replay stats header (`PlayerInfo` ×2, score, date, mode, rating change, "Watch set" in the stats page's button position, "1 / 31" navigation). The games are full `ReplayFile` cards with Slippi's three actions.
- **Filters:**
  - Mode toggle (All, Ranked, Unranked, Direct, Teams).
  - Character dropdown.
  - "Hide sets without replays".
  - Opponent code search.
  - The footer shows totals and the replay folder used for matching.
- **Watch set:** queues the set's local replays in the playback Dolphin (Slippi's `viewReplays(...files)` already takes several, as "Play all" does) and skips missing ones.
- **Data:** per set `{matchId, mode, startedAt, opponent(s) {uid, name, code, rank}, score, won, ratingChange, games[{gameIndex, tiebreakIndex, stageId, characters, stocksLeft, durationFrames, winner, endMethod}]}`. The server already stores these in `mm_matches`, `games` and `rating_events` (2.2).
- **Source:**
  - **New:** `GET /v1/me/sets?mode=&opponent=&character=&before=&limit=` (session, cursor paged).
  - Replay presence comes from the local replay database (section 7), not from the server.
- **Open questions:**
  - Unranked and Direct have no fixed set length. The proposal treats all games under one `matchId` (one connection) as a set, and the score is games won.
  - Teams cards need four badges. Is the partner shown on the left with you?
  - Should sets that were abandoned or voided (backend-design.md 4.4) be shown, greyed, with the reason?
  - Should local-only games (offline, or played before an account existed) appear here? Recommendation: no. They stay in the replay browser only.

### 3.7 Replay browser (`12`)

- **Purpose:** every local replay, now with real metadata.
- **Slippi:** `pages/replays/replay_browser`. The folder tree, `FilterToolbar` (Refresh, sort, "Hide short games", search), card list, selection toolbar and footer (current folder, count) are unchanged.
  - The cards gain what `.slp` metadata gave Slippi: player badges with stock icons and codes, date, duration and stage name.
  - Two **new** details: mode with the game number ("Ranked · Game 2") and a "View set" link (accent colour) that opens the set in match history.
  - There are no stage background images. Slippi uses Melee stage art; ours would have to be extracted from the user's disc like the stock icons (P+ stage select icons, `menu2/sc_selmap.pac`). That is possible later.
- **Data:** the per-file metadata of section 7, from the launcher's SQLite index (`src/database`, Slippi's `file`/`game`/`player` tables).
- **Open questions:**
  - Should the browser get a "Group by set" toggle?
  - Should search also match `matchId`?
  - Where do teams replays get their four badges? Slippi already handles that through `TeamElements`.

### 3.8 Replay stats page (`13`)

- **Slippi:** `replay_file_stats`. Header with `PlayerInfo` (stock icon, name, port pill, code), date/duration/stage/platform, "Launch replay" and previous/next.
  - The platform item becomes mode plus game number.
  - The content area keeps Slippi's "stats not available" state until a stats parser exists (PPLUS_PORTING.md section 6).
  - **New:** a "Ranked set" panel. It shows the set result and rating change, the games of the set (current one highlighted, other games open their stats, missing ones are marked), "Open in match history" and "Watch whole set". This is the replay → set direction of the link.
- **Source:** local only (the file's metadata plus the index). The rating change needs the server's set record. Cache it in the index when match history is fetched, or fetch `GET /v1/me/sets/{matchId}` (**New**).

### 3.9 Missing replay (`14`, also in `10`/`11`)

- **When:** a game in match history has no local file. Its `(matchId, gameIndex, tiebreakIndex)` is not in the index.
- **Inline:** "Replay not on this computer" plus "Locate…" (A). In B, the card is dashed and dimmed, with "Not on this computer", a menu and "Download".
- **Dialog** (`14`, MUI `Dialog` with the game's panel frame):
  - Explains the likely causes: moved or deleted, replays turned off (`enableNetplayReplays`), or played on another computer. It shows what it looked for (match id, game, expected file name).
  - "Locate file…" opens a file picker. The chosen file is checked against its embedded `matchId`/`gameIndex`, then indexed. "Close" dismisses it.
  - "Download from server" appears **only if** the server keeps replays and offers downloads. backend-design.md 2.5 keeps ranked replays (unranked for 90 days) for anti-cheat review; PPLUS_PORTING.md section 1 dropped Slippi's `slippi://` download path. A download is saved into the normal replay folder under its usual name.
- **Open question (needs a decision):** may players download their own uploaded replays? Should they also get their opponent's view? Both clients upload the same game.

## 4. Visual rules used in the mockups

- Tier text is plain off-white. The rating is bold in the accent colour, as in `ranked_user_profile.tsx`. Win/loss pills and rating deltas use MUI's success/error greens and reds. Those are functional colours, not identity.
- Port colours are Slippi's (`player_colors.ts`). In match history, you are drawn as port 1 (red) and the opponent as port 2 (blue). Replays use the real ports.
- Contained buttons and dialogs use the game's nine-slice frames, as the theme does (`theme.ts`, `MuiButton.contained`, `MuiDialog.paper`). Text buttons and icon buttons use the MUI glyphs the launcher already uses (PPLUS_PORTING.md section 5).
- No new art. Avatars are the launcher's jdenticon identicons, and character pictures are the extracted stock icons.

## 5. New server endpoints (summary)

| Endpoint | Auth | Used by |
|---|---|---|
| `GET /user/{uid}?additionalFields=rank` + placements, region | public | widget, profile |
| `GET /v1/users/{uid}/profile?season=` | public | profile, Season block |
| `GET /v1/users/by-code/{code}` | public | leaderboard search, links |
| `GET /v1/me/sets?mode=&opponent=&character=&before=&limit=` | session | match history, recent sets, head-to-head, post-set toast |
| `GET /v1/me/sets/{matchId}` | session | stats page set panel, "View set" |
| `GET /v1/leaderboard?region=&season=&page=` | public | leaderboard (in 2.4, not built) |
| `GET /v1/leaderboard/me?region=&season=` | session | pinned row, "Jump to my position" |
| `GET /v1/replays/{matchId}/{gameIndex}/{tiebreakIndex}` → signed URL | session, own games | "Download from server" (only if decided) |

Moving rank into `GET /v1/me` (server issue 3 in PPLUS_PORTING.md) saves a round trip on every refresh.

## 6. General open questions

1. Navigation A or B (section 2). Match history A or B (3.6).
2. Seasons: the Season dropdown assumes `seasons(id, starts_at, ends_at)` (2.2) and per-season ratings. Is there a season 1 at launch, or one unbounded season?
3. Offline behaviour: show the last fetched match history and leaderboard with an "Offline" note, or nothing? Slippi's user menu already shows "Offline".
4. Privacy of other players' match histories: only head-to-head, as proposed?
5. i18n: all new strings go through `*.messages.ts` like the rest.

## 7. Replay storage and naming that this UI assumes

This is what the replay format and the Dolphin recorder have to provide so the screens above work. It matches what Slippi's browser and index expect (PPLUS_PORTING.md section 6).

**Folder layout** (Slippi's, unchanged):

```
<rootSlpPath>                       default Documents/Brawl Online (settings: rootSlpPath)
  2026-10/                          monthly subfolders (useMonthlySubfolders, default on)
    Game_20261007T213125.rep        one file per game, local start time
    Game_20261007T213833.rep
  2026-09/ …
<extraSlpPaths…>                    extra folders, also indexed (settings: extraSlpPaths)
```

- Dolphin writes the file when a game ends, if `enableNetplayReplays` is on (the `[Online]` section of Dolphin.ini). Writing is Dolphin fork issue 5.
- The name format `Game_YYYYMMDDTHHMMSS.rep` is the one `replay_format.ts` already parses for the date.
- Files are never renamed or moved by the launcher. The link to the set lives **inside** the file, so a moved or renamed file still matches once its folder is indexed.
- One game per file, including tiebreak games.

**Metadata per file.** It must be readable without decoding the whole replay, from a fixed header block like Slippi's metadata, because the indexer reads thousands of files.

| Field | Fills | DB column (`src/database/schema.ts`) |
|---|---|---|
| format version, build hash, disc revision | compatibility check before "Launch replay" | (new column or file table) |
| `startTime` (UTC, ISO 8601) | date, sort | `game.start_time` |
| `matchId` (server's, null offline) | **set link** | `game.session_id` |
| `gameIndex`, `tiebreakIndex` | "Game 2 of 3", set link | `game.game_number`, `game.tiebreak_number` |
| `mode` (ranked, unranked, direct, teams, offline) | mode label, filters | `game.mode` + `game.is_ranked` |
| `isTeams` | badge grouping | `game.is_teams` |
| `stageId` (P+ id) | stage name | `game.stage` |
| `durationFrames` | duration, "Hide short games", sort | `game.last_frame` |
| `endMethod` (game, LRAS, disconnect) | match history detail | (new) |
| per player: `port`, `uid`, `connectCode`, `displayName`, `characterId`, `costume`, `teamId`, `startStocks`, `stocksLeft`, `isWinner` | badges, stock icons, winner glow, "my games" | `player.*` (`user_id`, `connect_code`, `display_name`, `character_id`, `character_color`, `team_id`, `start_stocks`, `is_winner`) |

- `characterId` must map to the stock-icon keys in `src/brawl_assets/catalog.ts` (`characterStockKeys` in `src/renderer/lib/utils.ts`).
- The stage name needs a P+ stage id → name table in the launcher.

**Lookups the UI makes:**

- Set → replays: `SELECT … FROM game WHERE session_id = :matchId ORDER BY game_number, tiebreak_number`. The `(session_id, game_number)` index exists. A game counts as missing when no row exists.
- Replay → set: `game.session_id` → match history entry. "View set" is hidden when it is null (offline games).
- "Locate…": read the picked file's header, compare `matchId`/`gameIndex`, add its folder or file to the index.
- Downloaded replays (if allowed) are written to `<rootSlpPath>/<yyyy-mm>/Game_<startTime>.rep` like recorded ones, so nothing else changes.
