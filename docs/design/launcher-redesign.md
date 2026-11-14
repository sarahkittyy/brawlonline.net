# Launcher redesign (mockups)

_2026-11-14. Sarah's board card: the launcher grew from Slippi's one feature at a time and is cluttered ("things are in weird spots, Other Games is kinda buried"). Keep what she likes: the choosable colour, P+'s colours, the Friends tab, the icons, the profiles. Mockups only, nothing built._

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
| Home > Spectate | Watch: the highest-rated live game at the top, then the list with stage pictures |
| Home > Other Games (a tab) | **Other Games** in the sidebar: rooms first, then Your games as tiles with Host |
| Project Punch's lab button (far right of the tab row) | A tile in Other Games > Your games (not drawn) |
| Replays page (folder tree + list), Highlights tab | Replays: tabs Replays / Highlights / Clips, folders in a dropdown (the tree was a third column) |
| Match history (header icon) | Your profile > Match history |
| User menu > View profile, the profile dialog | The sidebar card opens the profile **page**; Switch account / Log out sit in its "…" |
| Online / Friends panel (Home only) | The players rail (every page) |
| Settings | Unchanged (its own full page) |

## 3. Screens

| PNG | Shows |
|---|---|
| `00-before-after.png` | Home and Other Games today next to the mockups |
| `01-play-ranked.png` / `-pink` | Play, Ranked tab, P+ colours and Sarah's pink |
| `02-play-unranked-searching.png` | Searching Unranked: the panel and the sidebar chip |
| `03-other-games.png` | Other Games as its own page |
| `04-watch.png` / `-blue` | Watch, with a search running (chip), and a blue theme |
| `05-replays.png` | Replays with the rail folded |
| `06-leaderboard.png` | Everyone / Friends, the player's own row pinned at the bottom, friends marked |
| `07-profile-own.png` / `-pink` | Own profile page: hero, stats with sparkline, match history by day, rail on Friends |
| `08-profile-friend.png` | A friend's profile from the leaderboard: Invite to game is the main action, plus "You vs them" and your sets together |
| `09-play-2v2-friends.png` | 2v2 tab with "Invite a friend", rail on Friends with a request |
| `10-play-narrow.png` | 1084 px window, rail folded |

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
