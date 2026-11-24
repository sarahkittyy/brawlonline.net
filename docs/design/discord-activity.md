# Discord activity

The player's Discord activity comes from the launcher (2026-11-24, Sarah's board; mockups in
`run/design/discord-presence`, her picks: member list B, settings B on a Social tab, the mockup strings).
Neither Dolphin nor EmissaryRecomp shows one of their own: every released Dolphin is configured with
`USE_DISCORD_PRESENCE=OFF` (.github/scripts/build-dolphin-*), and the recomp's Brawl Online build
(`MKW_BRAWL_ONLINE_CORE`) turns its runtime's presence off (recomp 3c7c266).

## How

- **Discord application** `1574665391597162608` ("Brawl Online", Sarah's), in `social/discord/types.ts`.
- **Main process** (`social/discord/`): `DiscordRpc` talks to the Discord app's local IPC socket
  (`discord-ipc-0..9`: Windows pipes, Unix sockets incl. Flatpak/Snap), retries every 15 s while there is
  something to show, sends at most one update per 4 s (Discord takes 5 per 20 s). The member list shows
  the first line (`status_display_type` 2). `discord_gameState` reads what the Dolphins show:
  `game-status.json` (`screen`, `scene`, `game`; docs/rooms-protocol.md), which game the netplay Dolphin
  plays, and the replay the playback Dolphin plays (its header).
- **Renderer** (`renderer/lib/discord/`): builds the activity (the main process has no i18n) from the social
  stream (search, set: mode, opponents, game number, wins, live stream; the player's room), the room list
  (room name, ruleset, listed or not), Spectate's state, the player's rank, and the Dolphins' state; polls the
  Dolphins every 3 s while the setting is on.

## What it shows

| When | First line | Second line | Pictures |
|---|---|---|---|
| Launcher only | In the launcher | | logo |
| Searching | Ranked / Unranked / Direct / 2v2 | Searching | Ranked: rank badge |
| A set | Ranked · vs Peachy (2v2: vs A & B) | [with partner ·] Game n [· a - b from game 2] [· Character Select / Stage Select] | in a game: stage + own character; else Ranked: rank badge |
| A room | its name, or "host's room", or its mode when private | mode · ruleset, or mode · Game n; (3 of 4) | in a game: stage + own character |
| Spectating, a replay | Watching | A vs B | stage + eye (Spectate) or play icon (replay) |
| Training (any single-player screen), Project Punch, Other Games | Training / Project Punch (Training) / Other Games | the game's name | |
| The game's menus | (none: "Brawl Online" and the timer) | | |

"Show who I am playing" off leaves every name out. "Show Join and Watch buttons" (off by default):
"Join their room" only for a listed, non-friends-only room with an open slot (never a private room's code),
"Watch their game" only for a set listed on Spectate. Discord's buttons take web links only:
`https://brawlonline.net/?join=KFQB` / `?watch=<stream>`, which the homepage (website/script.js
`launcherLink`) hands to `brawlonline://room/join?code=` / `brawlonline://watch?stream=` (common/brawl_link.ts).

## Pictures

- In a game (a set, a room's game, a spectated game, a replay): the stage as the big picture (its name on hover)
  and the player's character as the small one (Spectate: an eye, replays: a play icon).
- Ranked while searching and between games: the rank badge (brawlonline.net/assets/ranks). Otherwise the logo
  (brawlonline.net/assets/icon-512.png).
- Stage and character pictures are the Discord application's art assets, which Sarah uploads by hand
  (2026-11-24, her choice over hosting them on the website): `stage_<P+ stage id>` (61), `char_<stock key>`
  (42, not Giga Bowser or Wario-Man), `watching`, `replay`, made by tools/discord-assets/make.py from a
  launcher's extracted stage select and stock icons (512x512; stocks on a light disc, as Discord crops small
  pictures round). A stage P+ does not list (common/stages.ts) shows the logo.
