# Launcher redesign (mockups)

_2026-11-14. Sarah's board card: the launcher grew from Slippi's one feature at a time and is cluttered ("things are in weird spots, Other Games is kinda buried"). Keep what she likes: the choosable colour, P+'s colours, the Friends tab, the icons, the profiles. Three rounds of mockups (§3, §7, §8), then built (§9): "go ahead and implement all of this"._

Mockups: `run/design/redesign/` (gitignored). `index.html?s=<screen>&c=<hex>` holds every screen, and `render.sh` renders them with headless Edge at 1264x711, Sarah's window size. The PNGs are in `out/`; screenshots of the launcher as it is today (v0.1.140) are in `before/`. The page reads its fonts, menu frames, stock icons and stage pictures from the local extraction cache (`%APPDATA%\Brawl Online\game-assets\theme`), rank emblems from `components/rank_badge/images`, and flags from `flag-icons`, so nothing is copied. Any theme colour renders with `-c <hex>` (`-c f65098` is Sarah's pink).

## 1. Research: what design guides for coding agents say

These are the most installed guides as of 2026-11-14. Copies are in `run/design/redesign/research-sources/`.

| Guide | Angle |
|---|---|
| Anthropic `frontend-design` (anthropics/skills) | Plan colours, type and wireframes first. Spend boldness in one place. Names five looks that read as AI-made. |
| Vercel Web Interface Guidelines | Engineering correctness: focus, hit targets, states, loading, motion. |
| Impeccable (pbakaus) | The only guide with a mode for app screens ("Operate"). A theme lends type, palette, density and one signature move, and nothing else. The navigation and controls stay standard. |
| ibelick `baseline-ui`, Emil Kowalski, taste-skill, ui-ux-pro-max | Short MUST/NEVER lists (accent once per view, no decorative gradients or glows), motion timing, icon consistency. |

The rules the redesign follows, and where the launcher breaks each one today:

1. **Navigation reads as navigation.** Today the header has five icon-only buttons (home, replays, leaderboard, rulesets, history) that nobody can name without hovering. → A labelled sidebar.
2. **One signature move, used sparingly.** Ours is Brawl's tinted menu frame. Today Home has five framed buttons (PLAY, Play unranked, Play ranked, Play 2v2, Create room), and Other Games adds a framed Host per game, so none stands out. → The frame marks only *where you are* (the selected sidebar item, in Brawl's character-entry frame) and *the one main action* on a screen (Find match, Watch, Invite to game).
3. **The accent only marks selection, the main action and state.** Today the pink or teal colours section titles ("Public rooms", "Rating", "Your games"), the rating number, codes, links and scrollbars. → Titles are white, and links are plain text with the accent only on their arrow.
4. **The display font is for page titles only.** Today BrawlTitle is on every section title. → BrawlTitle for the page name, BrawlMenu for everything else, profile names included.
5. **Nothing under 12 px, and a real type scale** (12/13/15/18/22/28, about 1.2 per step). Today "0 in queue · 0 in game", the host line under a room and the ranked-sets line are 10–11 px.
6. **No all-caps labels.** Today: DRAFTS, PUBLISHED, RANKED, SOCIALS, CURRENT FOLDER, ONLINE · 1, SENT · 4, ADD GAME, JOIN, WATCH, FULL. → Sentence case.
7. **Group by spacing before adding boxes, and no box inside a box.** Today Other Games has panels inside the tab inside the page.
8. **A headline number shows its recent history.** → The rating has a sparkline of the last 24 sets.
9. **Not a modal first.** Today the profile, which Sarah loves, lives in a dialog. → A page. The hover card stays as the quick look.
10. **Secondary actions appear on hover.** Today every online row has an envelope and every replay row has three icons. → They show on the hovered row. Right-click menus stay.
11. **Every state is designed:** searching, full rooms, a card that differs, requests waiting.

## 2. The new structure

Three columns: a **sidebar** (216 px), the **page**, and the **players rail** (272 px, folds to 60 px).

```
┌ Brawl Online ─┬─ Page title ─────────── page actions ┬ Online 185 │ Friends ①  ┐
│ ▸ Play        │                                       │ friends first, then all   │
│   Other Games │   the page                            │ (hover: message)          │
│   Watch    ●7 │                                       │                           │
│   Replays     │                                       │                           │
│   Leaderboard │                                       │                           │
│   Rulesets    │                                       │                           │
│               │                                       │                           │
│ [Searching…]  │                                       │                           │
│ (av) name  ⬢  │                                       │                           │
│ tools row     │                                       │                           │
└───────────────┴───────────────────────────────────────┴───────────────────────────┘
```

- **The sidebar** is the shell's own tinted layer, darker than the page, with the same colour math (`theme_color.ts` L values). Its order is the order of use: Play, Other Games, Watch (with the number of live games), Replays, Leaderboard, Rulesets. At the bottom is the player's card (main, name, code, rank emblem), which opens the profile, with the utilities under it: Settings, Theme Color, Report a bug, Discord, Notifications.
- **The queue lives in the shell.** While a search runs, a chip above the card shows "Searching Ranked · 0:42 · 23 searching" and a Cancel button, on every page.
- **The players rail is on every page**, not only on Home. Friends are visible from Watch, Replays or a profile, and its tabs stay as they are today. Folded, it shows the friends online as stock icons with a green dot, plus the unread envelope. In a narrow window (under about 1150 px) it would fold by itself.
- No header bar. That gives back about 70 px of height, which a 711 px window needs.

### Where everything went

| Today | Mockup |
|---|---|
| Header PLAY (start P+ / Dolphin offline) | Play page, "Play offline" (top right) |
| PLAY offering a launcher update | A strip under the logo (not drawn) |
| Header icon nav | Labelled sidebar |
| Home > Overview: Play unranked / ranked / 2v2 buttons, "0 in queue" | Play: one panel with Unranked / Ranked / 2v2 tabs (each with its count of players searching) and one **Find match** |
| Overview Rating widget (Refresh, Match history) | Play > Ranked tab: emblem, tier, rating, position, sparkline |
| Overview Public rooms | Play > Rooms (players' mains as little stocks, Waiting / In game, Join) |
| Home > Spectate | Watch: the highest-rated live 1v1 at the top (a 2v2 or free-for-all only when no 1v1 is live), then the list with stage pictures |
| Home > Other Games (a tab) | **Other Games** in the sidebar: rooms first, then Your games as tiles with Host |
| Project Punch's lab button (far right of the tab row) | A tile in Other Games > Your games (not drawn) |
| Replays page (folder tree + list), Highlights tab | Replays: tabs Replays / Highlights / Clips, folders in a dropdown (the tree was a third column) |
| Match history (header icon) | Your profile > Match history |
| User menu > View profile, the profile dialog | The sidebar card opens the profile **page**; Switch account / Log out sit in its "…" |
| Online / Friends panel (Home only) | The players rail (every page) |
| Settings | Unchanged (its own full page) |

## 3. Screens (round 3)

Rounds 1 and 2 are in `out/round1/` and `out/round2/`. `-1150`/`-980` are narrower windows. `-nodisc` is the look without a Brawl disc (`render.sh -q nodisc=1 -c d0d0d0`). `-pink` is Sarah's theme colour.

| PNG | Shows |
|---|---|
| `01-play-a.png` (+ `-pink`, `-nodisc`, `-1150`) | Play, variant A: the ranked card, the three Play buttons stacked on its right |
| `02-play-b.png` | Play, variant B: the three Play buttons in a row under the ranked card |
| `03-play-late-night.png` (+ `-980`) | Late night ("10+ min" for Ranked), the rail folded, the narrowest window |
| `05-other-games.png` (+ `-1150`) | A room with Brawl, SD card and Codes chips; "Joins with" open |
| `06-other-games-your-room.png` (+ `-nodisc`) | Hosting: slots, kick, a player being dragged |
| `07-spectate-while-searching.png` (+ `-nodisc`) | Spectate, a Ranked search in the queue box |
| `08-replays.png` (+ `-nodisc`) | Replays, rail folded |
| `09-leaderboard-match-found.png` (+ `-nodisc`) | Leaderboard during a Ranked match |
| `10-profile-own.png` (+ `-nodisc`) | Own profile |
| `11-profile-friend.png` | A friend's profile |
| `12-queue-states.png` | The queue box's states |
| `13-no-disc-pictures.png` | Without the disc: four choices for the player's picture |

## 4. What stays

The theme colour and all its maths, P+'s gradient colours, Brawl's fonts and frames (in fewer places), MUI's icons, the friend glyph on the stock corner, flags and mains beside names, the rank emblems, the right-click player menu, the hover card, the chat windows, and the profile's content.

## 5. If it's built, in this order

Each step ships alone and leaves the launcher working.

1. **Shell:** the sidebar replaces the header, the players rail moves out of Home, and the queue chip is added. Pages stay as they are inside it.
2. **Play:** the mode panel takes over the Play buttons and the Rating widget. Rooms follow.
3. **Profile page:** match history moves into it, and the dialog goes.
4. **Other Games and Watch** become pages; Replays gets its tabs and folder dropdown.
5. **Cleanup pass:** the 12 px floor, no all-caps labels, accent and frame only where §1 allows, BrawlTitle only on page titles.

## 6. Open questions

1. A sidebar (as drawn) or a top bar with text labels? The sidebar has room for whatever comes next. A top bar keeps the page wider but costs height.
2. Should the players rail be on every page (as drawn), or on Play only?
3. Should the profile become a page, with match history in it?
4. Should Replays stay separate from Watch (as drawn), or become a tab of it?
5. Should BrawlMenu stay the body font everywhere (as drawn), or should a plain sans take the dense text (tables, codes)? The guides prefer a plain sans for data. BrawlMenu is the game's own menu font and part of the look.

## 7. Round 2 (Sarah's notes on round 1, 2026-11-14)

Sarah: "i love love love the design", with these notes. What changed:

- **No new words.** Every word on screen is one the launcher already has (its `*.messages.ts`: "Public rooms", "128 players in-game", "Spectate", "Highest Elo", "Next game soon", "Show 3 games", "Join this room with", "Player 3", "Kick", "Close slot", "Invite to your 2v2"...), data, or Sarah's own words: "ETA: 0:44", "ETA: 10+ min", "Play solo or team with a friend", "Searching". Round 1's sentences are gone: the ping band, "Usually under a minute", "Highest rated game right now · both Master 3", "Team up with a friend...". The nav item is "Spectate" (the tab's name), not "Watch". The main buttons say "Play ranked", "Play unranked" and "Play 2v2", as today.
- **ETA instead of players searching.** Players searching is 0–2 at most hours, because they match quickly. Unranked and Ranked show the ETA next to their tab, with the stopwatch icon. The ETA is the median wait of that hour of the day, from `pp-admin stats` (`queue_searches`, the last 14 days). If most searches that hour gave up ("never"), the median is 10 minutes or more, or there are under 5 searches, the tab shows **"ETA: 10+ min"**. 2v2 keeps "0 in queue" (today's string). The launcher would need the hour's ETA per mode from the server (a field beside `queues` in `GET /v1/rooms`). On 2026-11-14 at 21 UTC it was Unranked 0:28 and Ranked 0:44. From 11 to 15 UTC Ranked's median is "never".
- **The search stays in the game.** Play ranked opens the game's Ranked queue, and the player waits in Online Training, as today. The Play page doesn't change while a search runs. The queue box in the sidebar's bottom left shows the search on every page:
  - mode, "Searching · 0:42" and the ETA (or the 2v2 partner);
  - in a match: the opponent's stock and name, "Game 1 · 0-0", drawn in the selected frame.

  It is display only (no Cancel: the game's own Cancel stays the one). A click brings the game's window forward. The launcher doesn't know the game's queue state today (social status only has a 2v2 party's `searching`), so Dolphin would have to send it to the launcher.
- **Ruleset in the room bar.** Left of Create room, a button shows the player's default ruleset (name and code, e.g. "Auto L Cancel -r v4 WQGFFJ"). Its tooltip is today's "… by sarahkitty. Click to see the rules." and it opens Rulesets. The room code field has Join inside it, to make room. "128 players in-game" moved to the page header, beside the P+ version.
- **Other Games:**
  - **No monogram tiles.** Your games are rows again, with today's chips ("Brawl: a56ffa", "SD card: c3bc6e") and the disc ID and file under the name.
  - **Codes are purple again:** the "Codes (4)" chip, from today's look.
  - **"Joins with" is a dropdown again,** and its menu is today's ("Join this room with", SAME MOD / BUILT IN, the SD card line, "Use my pick for every room of this mod").
  - **No Public switch on each game.** Public / Private is a property of the room, so it lives only in the room panel, where it already is today. Host opens the room the way the player last left the switch.
- **In a room.** The room panel sits at the top of Other Games:
  - the room's name with the pencil, its code chip with copy, the Public switch and Leave;
  - the game's name and hash with its Codes chip;
  - four slot cards in port colours. The host sees Kick and Close slot on a player's card, and "Close slot" or "Open slot" on an empty one.
  - Dragging a card moves the player: the card lifts, its old slot stays as a faded copy, and the slot under it gets a dashed outline.

  The room is also first in Public rooms, marked "Your room".
- **Spectate.** No line of explanation on the top game. Viewers are always the eye icon and a number. "Next game soon" fits in its column. The mode moved into the game column ("Ranked · Game 1 · 0-0"), so names aren't cut.
- **2v2.** "Play solo or team with a friend" and an "Invite to your 2v2" button. No character icon, because the player's character isn't known until they pick one.
- **Other copy fixes:**
  - Friends rows use today's statuses: the room's name with Join, "Playing Ranked" with Watch, "In the launcher".
  - A request reads "wants to be friends", with Accept / Decline as icons.
  - The leaderboard filter is "All / Friends", and its columns are "W-L".
  - A friend's profile shows your match history with them instead of round 1's invented "You vs them" boxes.

## 8. Round 3 (Sarah's notes on round 2, 2026-11-14)

- **Play is one panel, no tabs.**
  - The ranked card (emblem, tier, rating, "24 ranked sets · #380 of 871 · 11W-13L", sparkline) is on one side, and the three Play buttons (Unranked, Ranked, 2v2) are on the other.
  - Each button has its status: the stopwatch and the ETA, or "0 in queue" with the Invite to your 2v2 icon.
  - **Variant A** (recommended) stacks the buttons on the right. The panel is 190 px tall, so Public rooms gets more room.
  - **Variant B** lines the buttons up under the card.
  - The unranked game list and the 2v2 line are gone. When the panel is narrow (1150 px window with the rail open), the sparkline hides.
  - The three buttons all wear the Brawl frame: together they are the page's main action.
- **The ETA is the stopwatch and the time** ("0:44", "10+ min"), with no "ETA:" text, and "Estimated queue time" in its tooltip. The buttons are as wide as the longest label and 36 px tall.
- **Public rooms row never wraps.** The ruleset is a quiet icon and name with no box ("Auto L Cancel -r v4"; the code is in its tooltip with today's text). The name shrinks to an ellipsis, then to the icon alone. The room code field has Join inside it. It fits on one line in the 980 px minimum window (rail folded) and at 1150 px with the rail open.
- **Other Games rooms with every chip.** A room with "Brawl: a56ffa ✓", "SD card: f35aaf !" and "Codes (3)":
  - At the default width the three fit on one line, with "Joins with" under them.
  - In a 1150 px window with the rail open they wrap to two lines, and the row grows (`05-other-games-1150.png`).
- **Spectate's top game** reads [character] [name] vs [name] [character] (no ranks); when the players wrap onto more lines, every side reads [character] [name].
- **Leaderboard:** "871 ranked players" sits under the title, left-aligned.
- **No P+ pill** on Play. "128 players in-game" stays beside the title.
- **The player's card** (bottom left) has a lighter rounded background, and is lighter still on hover and on the profile page.
- **Settings** is unchanged: its own page with its own menu, as today.
- **Without the disc** (`-nodisc`):
  - The default grey look and system fonts. Primary buttons are plain filled buttons, and the selected nav item is a lighter row instead of Brawl's frames.
  - No stage pictures.
  - Characters are today's name tags on profiles; elsewhere they are left out (room rows, Spectate and Replays rows, match history).
  - Players get a picture in place of their stock icon. The options (`13-no-disc-pictures.png`) are:
    1. today's dotted Fox outline;
    2. **the name's first letter in a grey circle** (drawn on every no-disc screen);
    3. the jdenticon the launcher already makes from the uid (`lib/display_picture.ts`, today only in the account switcher);
    4. the player's own rank emblem in the picture's place (only for the player's own card; it's the only one we have for everyone, and unranked players have none).

## 9. Built (2026-11-14)

Sarah picked variant A (Play buttons stacked on the right) and the dashed Fox outline for a player without a stock icon (what the launcher already draws). Everything in §7 and §8 is built:

| Part | Where |
|---|---|
| The data contract (8e46e17e) | `RoomList.queueEta`, `PublicRoom.mains`, `LivePlayer.rank`, `SocialState.queue` (`QueueState`), `friendsLeaderboard`, `matchHistory({opponent})` |
| Sidebar, players rail, queue box | `app/shell/` (`sidebar.tsx`, `players_rail.tsx`, `queue_box.tsx`, `nav_badges.tsx`); `app.tsx` lays out sidebar / page / rail |
| The selected page's frame | `styles/game_theme.tsx`: a `selected` frame from the extracted `buttonSelected` texture, tinted like `buttonHover`; `:root[data-game-frames]` says the theme has frames |
| Play | `pages/play/` (the page, the ranked card, `ruleset_chip.tsx`); `play_buttons_view.tsx` stacked with ETAs; `public_rooms_view.tsx` as a section with a one-line bar |
| Other Games | `pages/home/mods/mods_view.tsx`: one column; the room panel's slot cards; no Public switch per game (`MODS_HOST_PUBLIC` remembers the room's) |
| Spectate | `spectate_view.tsx`: the top game (`pickFeatured`, `showFeatured`), stage pictures in rows |
| Replays | `replay_browser.tsx`: the folder dropdown (`FolderSelect`), folder buttons in the footer |
| Leaderboard | All / Friends, the player's own row pinned |
| Profile page | `pages/profile/profile_page.tsx` at `/main/profile/<uid>`; `ProfileNavigator` turns every `useProfileView().open` into it; the old `/main/matches` leads to the player's own |

Old addresses: `/main/home/*` redirect to `/main/play`, `/main/spectate` and `/main/other-games`. Removed: the header, Home's tabs and overview, the Rating widget (its Refresh and Hide ranking are on the ranked card, on hover).

Not done:
- the queue box can't bring the game's window forward (there is no call for it);
- Highlights keeps its own left list of filters (since 2026-11-15 a select in its toolbar, highlights.md §10).

## 10. Round 4 (Sarah's notes from browsing staging, 2026-11-14)

- **The chat bar** is under the page, between the sidebar and the players rail: its windows open over the page only. A narrow tab keeps the name (cut off) and the X; the dot, the friend mark, the invite and the code go.
- **Play:** the Play buttons on the left (each status on its right), the rank on the right. The card's hover X is gone: hiding the rank is Settings > Appearance's alone (a staging profile had it off). Public rooms is an outlined panel again; the room code box is one outlined control (no underline), lit on focus.
- **Other Games:** the left / right split again, as outlined panels: the player's room and the public rooms on the left, the player's games on the right; one column under 600 px of page. The title and the controller share a line; the words and the beta note go under them. A game's name has its row (its chips wrap under it); a room's host and game line wraps.
- **Profiles are popups again** (the profile page is gone). On the player's own, Switch account and Log out sit left of the link, on its row.
- **Match history** is a sidebar page above Replays. Left of All / Ranked / ... a player filter suggests the players the player has played with (`GET /v1/me/opponents?q=`: main, name, code); a pick is a tag with only the code and an X; several tags keep the matches all of them played in (`opponent=` takes up to 4 uids). Its rows are four columns. The mode buttons are less tall.
- **The folded rail** lost its envelope (it only opened the rail, as the arrow does); unread messages count on the arrow.
- **Replays** opens on the replay folder itself (everything in it), not the last folder picked; the folder dropdown's icon is centred; the folder buttons sit in the footer's row (only their icons when narrow).
- **Window sizes:** checked at 1400, 1264, 1100 and 980 px (the smallest window): names in the tables, rooms and chat tabs give way with an ellipsis rather than overflowing; the ranked card shrinks first.

## 11. Round 5 (Sarah's board, 2026-11-14)

- **Other Games:** Host comes before a game's Codes button, so Host never moves. The page's words and the beta note sit behind an info icon beside the title (they took half a short window); the controller is on the title's line and shrinks (its label goes on a narrow page). The panels stack under 680 px of page and the page then scrolls; a panel's head puts its buttons under the title when they don't fit. An error has room under it.
- **Replays:** no bar at the foot (current folder, the count). Open Replay Location and Open Rendered Video Location sit on the title's line, with their labels; while a folder is read, its progress shows beside the filters.
- **Spectate:** who plays takes one of three shapes only: all on one line; the first side, then "vs" and the second side on the next line; or every player on a line of their own when a side doesn't fit. The row measures its widest side, so a side is never half wrapped beside one that isn't.

## 12. Round 6 (Sarah's board, 2026-11-14)

- **Play:** the panel is as tall as the rank beside it: the three Play buttons are 32 px tall (were 48), 5 px apart.
- **Highlights lag:** the tab laid out every row (1622 for a month of replays: 45k elements, 11k of them partly transparent and so a paint layer each); every frame of a scroll took ~37 ms (p95 50-58). Now only the rows near what shows are laid out, the rest is space of their measured height (`windowed_rows.ts`): 4.2 ms a frame (p95 8). The row playing (or picked with ↑/↓) is scrolled to by its offset, below the top bar and above the bottom one.
- **One player search** for Match history, Replays and Highlights (`components/player_search/`): suggestions with a picture, name and code; a pick is a tag of the code with an X; several tags keep what all of them are in. Match history's suggestions are the server's (`/v1/me/opponents`); Replays' and Highlights' are the players of the replays on this computer, the most games first (`searchPlayers`, the replay database's player table). In those two, what is typed still filters as before (Replays' query language, Highlights' name or code); a tag adds `code:` to Replays' query.
- **Highlights' bars:** the top bar is the search and the sort, on one line; the render buttons and One combo movie moved to a bar at the bottom of the list with the keys' hint (the hint goes on a narrow list). Both bars are the page's colour with a line between them and the rows. The category list is on the page's colour too, a line on its right, the open one raised as in the app's sidebar (it was darker than the page, the open one the page's colour).
- **Replays:** the search first, then Refresh (an icon now, its label a tooltip), the sort and Hide short games: one line at the default window.
- **Replays' count and edges (Sarah's board, 2026-11-15):** the count ("614 replays found.", the folder with the filters; while a folder is read, its progress) is back, small and grey over the first replay at its left. The bar, the count, the rows and the first tab's underline start 28 px in, as the title; the rows end 28 px from the right (18 px and the scrollbar's room, kept when the list doesn't scroll). A row's play button is as far from its right edge as the players from its left (11 px; the row's inner box was 20 px wider than the row, so the button sat at the edge and the file name was cut). Narrow (the bar under 660 px, the launcher at its smallest with the rail open) the bar is two lines: the folder, the search and Refresh, then the sort and Hide short games; a row under 560 px drops the file name and its buttons sit closer.

## 13. Rulesets at any size (Sarah's board, 2026-11-14)

The page had fixed column widths (320 + 380 px) wider than the room beside the sidebar and an open players rail, so rows ran under their buttons, the footers' buttons squeezed into walls of one word a line, and a setting's help went one word a line beside its control. Now:
- the page's words are behind an info icon by the title (as on Other Games);
- the list and an open ruleset sit side by side only when the page has 760 px; narrower, the open ruleset takes the page and Close brings the list back;
- a row's buttons go under its name, on the right, when they don't fit beside it; its line of code and summary is cut short instead of running under them;
- an open ruleset's title is smaller and wraps; Copy link and Use sit by its code, the footer has only Close, Hide and Edit; footer buttons wrap whole, never their words;
- an editor setting's control goes under its name and help when they don't fit side by side.

## 14. The sidebar folds (Sarah's board, 2026-11-15)

"the left sidebar should collapse like the right sidebar. they should be equal width when collapsed": arrows at the bottom, over the search and the player's card (at the right while open, centred folded; "move the collapse / expand arrow to the bottom, above the queue if its there / profile"), fold and open it, and this computer remembers which (it doesn't fold by itself in a narrow window as the rail does). Folded it is as wide as the folded rail (60 px and its line), one centred column:
- **the logo**: the app icon's ball (`app/shell/images/logo.png`, from `tools/icon/make-icon.py`) in place of the name; a click checks for updates, as the name does;
- **the update box** is a ring that fills as the launcher update downloads; its tooltip has the button's words ("Update 45%", "v0.2.1 → v0.2.2"). Downloaded, the circle is the accent and a click installs it, as Update does. A set-up step (Downloading Project+ ...) is a ring too, its name and percentage in the tooltip;
- **the pages** are their icons in 36 px squares, each name in a tooltip as the folded rail's friends; Other Games' and Spectate's counts sit on the icon's top right corner, as the rail's unread count;
- **the queue box** is the dot and the search's time, or the opponent and the score; its tooltip has the open box's lines;
- **the player's card** is their stock icon in the card's box, the name and code in its tooltip; logged out, Log in is an arrow in the same box;
- **the tools** (Settings, Theme Color, Report a bug, Discord, the bell) are a column, their tooltips at their right.

The column fits a 1264 x 711 window with a search and an update showing; shorter windows scroll it, as the open sidebar.
