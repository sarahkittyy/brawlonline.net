# Player profiles

_Built 2026-11-09 from Sarah's board card ("let's create some user features ..."). Replaces the launcher's "Profile & match history" page: the match history is its own page now, and the name, rank and rating are on a profile anyone can open, in the launcher and on the website._

## 1. What there is

| Where | What |
|---|---|
| Launcher sidebar | **Match history** (History icon, `/main/matches`): the player's matches only. Each name has the player's rank badge at the time of the match before it (no number; the rank's name on hover) and opens their profile. |
| The account, top right | Clicking it opens the player's own profile (no dropdown since 2026-11-10). Under its web link, on the right: **Switch account** (every account on this computer, the current one checked, and + Add another account) and **Log out**; on the left **Activate online play** / **Verify email** until they're done. The name glows gold until this client's player has clicked their profile's edit pencil, which glows once the profile is open. |
| A player's right-click menu (online list, chat tab) | **View Profile**, below Message. |
| Chat window title | Clicking the name opens the profile. |
| Leaderboard | Clicking a name opens the profile. |
| Profile dialog | Badge, name, code, rank (or "Placing: N more ranked sets" / "Not ranked yet"); Ranked: rating (`????` while placing), leaderboard position, sets played, record (`34W-20L`); the web page's address (opens in the browser, copy button). Someone else's, when logged in with online play: **Message** (mail icon) and **Ignore user** / Unignore user. |
| `brawlonline.net/profile/<code>` | The same profile as a web page, for search engines and link previews. The website's Ranked top 10 links each name to it. |

Profiles are public: `GET /v1/profiles/<id>` needs no login, and the page is indexable. The match history stays private (only the player's own, with a session); nothing on a profile says who someone played or when.

Accounts with no connect code (online play never set up) have a profile by uid in the launcher but no web page. Banned accounts have neither (404).

## 2. The address: by connect code

`brawlonline.net/profile/sond-810` for `SOND#810`: the code in lower case with `-` for `#` (`#` starts a URL fragment, so it can't be in the path).

| Option | For | Against |
|---|---|---|
| **Connect code** (chosen) | Short, readable, the thing players already share and type in game. Changes only when an admin or the slur filter replaces a code. | When it changes, old links stop working. |
| Display name | Most readable. | Changes often (anyone can rename), not unique. |
| uid | Never changes. | `/profile/0e6f3c1a-...`: unreadable, nobody can type or remember it. |

Losing old links when a code is replaced is acceptable, and for a slur code it's wanted. Every other spelling redirects (308, permanent) to the one address, so search engines see one page per player: any case (`SOND-810`), leading zeros (`sond-0810`), `SOND%23810`, and the uid (`/profile/<uid>`, which the launcher could use if it ever needs an address that survives a code change). The page has `<link rel="canonical">` too.

## 3. Link preview (Discord)

The page is HTML that accounts renders, the same for people, crawlers and unfurlers (they don't run scripts, so there is no separate preview page as there is for `/r/<code>`). What Discord shows:

- **Site name**: Brawl Online (`og:site_name`).
- **Title** (the link): `SonderMeaning (SOND#810)` (`og:title`).
- **Description**: `Gold 2 · Rating 1523.4 · #12 of 340 · 34W-20L`; without a rank (placing, or no sets yet) `Placing. 1W-1L` (`og:description`).
- **Thumbnail** (top right): the rank badge (`/assets/ranks/gold-2.png`); without a rank the emblems' outline, dotted, with no "N more" tag (`/assets/placing-badge.png`, drawn from the same silhouette as the launcher's placing badge; in git, unlike the Layerlab badges). `og:image` with `twitter:card` = `summary`, the small image; `summary_large_image` would put a big banner under the text.
- **Side bar**: the tier's colour (`theme-color`), green (the site's) without a rank.

Search engines get the title `SonderMeaning (SOND#810) · Brawl Online`, a description sentence, the canonical address and a schema.org `ProfilePage` (JSON-LD). The page has no scripts (CSP `default-src 'none'`), is cached for a minute and is limited per IP together with the JSON lookup (60 a minute, 1,000 an hour).

## 4. One profile, two views

Sarah's question: new profile fields (a country flag, mains, a bio or tag) should show in both the launcher and the web page, ideally from one place.

What is shared:

- **The data.** `accounts::profile::Profile` (`server/crates/accounts/src/profile.rs`) is the one profile model. `load()` builds it; both `GET /v1/profiles/<id>` (the launcher) and `GET /profile/<slug>` (the page) use it. A field added there reaches both.
- **The names and images.** Rank names, the badge images (the website's `assets/ranks/` and the launcher's bundled ones are the same Layerlab files) and the tier colours.
- **The order.** The launcher's `ProfileView` (`launcher/src/renderer/components/profile/profile_view.tsx`) and the page (`page_html` in `profile.rs`) show the same parts in the same order; each file names the other.

What isn't: the two renderers. The page has to be finished HTML when it leaves the server (crawlers and Discord don't run scripts), and the website has no build step; the launcher's view is React with MUI and the launcher's own buttons (Message, Ignore). Sharing one renderer would mean either a Node service rendering the launcher's React components for the website (a new server process and a website build), or the launcher showing server-made HTML (no launcher actions or theme, and nothing when the server is older). Neither is worth it for a handful of fields; if the profile grows into a big page, the Node renderer is the way.

**Adding a field** (say a country flag):

1. Storage and an edit endpoint (a migration; `PUT /v1/me/...`).
2. `Profile` in `profile.rs`, filled in `profile_of()`; `page_html()` shows it; a unit test there.
3. `PlayerProfile` in `launcher/src/accounts/types.ts`; `ProfileView` shows it; the mock in `backend.service.mock.ts`; `profile_view.stories.tsx`.
4. If it should show beside names (a flag, mains): a field on `UserNameData` and a `show...` switch on `UserName` (below), and the lists that have it pass it.

## 5. `UserName`

`launcher/src/renderer/components/user_name/user_name.tsx` is the one way the launcher draws a player's name: `[badge] name CODE`, each part switched on where it's wanted (`showBadge`, `showCode`), and `profileLink` makes the name open the profile. `classes` restyles the parts for a place with its own look (the chat's `PlayerName` is `UserName` in the chat's CSS). With `showBadge` a player without a rank gets a badge-wide gap, so names in a list line up.

Used by: the match history (badge, code, link), the leaderboard (link; the badge has its own column), the online list and chat tabs (code), the chat window title (code, link).

The online list and chat don't show badges yet: the social stream's players have no rank. Adding it is a field on mm's/accounts' online list, then `showBadge` there.

## 6. Match history ranks

Each player's `rank` in `GET /v1/me/matches` is their rank when the match was made: the `rank_after` of their last rating change (`rating_events`) before the match's `created_at`. For a ranked set that is the rank going into it. Null while they were placing or before their first rated set. No new storage: the ranks were already kept per rating change for the tier rules (`docs/design/ranked-tiers.md`), and `rating_events_uid_idx (uid, created_at)` makes it one index lookup per player per row.

## 7. Customization (2026-11-09)

Sarah's board, second card: the player edits their own profile in place.

- **Edit mode**: on the player's own profile a pencil sits right beside the name; while editing, a check left of the X (top right) leaves edit mode. Every change is saved as it is made; there is no Save button.
- **Name**: a text box in place of the name. Leaving it or Enter renames through `POST /v1/me/rename`, the display-name rename path, so the slur filter applies; Escape puts it back. The launcher's 15-character rule is checked first.
- **Mains** (under the name and badge, above Ranked): "Mains:" and the characters, the primary first. Full-size 32 px stock icons in one row while they fit (9 in the profile's 324 px row); when they don't, 16 px icons in two even rows, never three: 36 (`MAX_MAINS`, two rows of 18) is the most the server takes, whatever the window size (the dialog's width doesn't change with it, and the interface zoom scales everything alike). Without a disc set up the launcher has no stock icons, so the mains are rounded name tags that wrap. The row is hidden when there are none (except in edit mode). Edit mode: a pencil before "Mains:" opens a popover with no title, every character's stock icon in a grid (or the name tags), with a bright rounded outline on the picked ones; its line says "Click to toggle. Double-click to select primary." A click picks or drops one. The **primary** is the first one picked until another is double-clicked (which also picks it); it has a gold outline and a star, and it comes first everywhere. Dropping the primary makes the next one primary. Ok saves. Under the row, "Show next to name" (below).
- **Random** is a main, shown as a "?" tile the size of a stock icon (Sarah: an actual "?", not the Smash emblem stock tried first).
- **Country**, under the connect code: the flag and the country's short name ("United States"); hidden when unset. Edit mode: one button with the flag (a dashed outline without one), the name and a pencil opens a popover with a search box and a grid of every flag, the dashed "no country" first; a click picks and closes. Beside it, "Show next to name". The list is flag-icons 7.6.0's (MIT): ISO 3166-1 plus England, Scotland, Wales, Northern Ireland, Catalonia, Basque Country, Galicia, the EU, the UN and a few more (270 flags), with the short everyday name where flag-icons gives an official one (`tools/profiles/countries.py` writes both the launcher's and the server's list). The launcher loads each flag's SVG when shown; the website serves the same files (`website/assets/flags/`).
- **Socials**, their own section under Ranked: one row of logos (Discord, X/Twitter, Twitch, YouTube, TikTok, Instagram), only the ones set; the section is hidden when none (except in edit mode). A logo opens the account in the browser; Discord's copies the username (it has no page). Edit mode: each platform's logo and a text box that takes a username, `@username` or a link to the account; leaving it saves it, blank removes it. The server keeps only the handle and builds the link itself, so a profile never links anywhere but the platform's own site. A username the slur filter calls a slur is refused (not replaced). The logos are Simple Icons (CC0).
- **The web page** shows the same, in the same places: the flag and country under the code, mains as name tags (the website has no game art), and a Socials section with the logos linking to the accounts (Discord's with the username beside it).
- **Show next to name**: two choices in edit mode, one for the country and one for the primary main (both on until turned off; `profiles.show_country`, `show_main`). They decide whether the flag and main show beside the player's name around the launcher (§8); the profile itself always shows both.

Server: `profiles` table (migration 0032: mains as character keys, country, one column per platform), `PUT /v1/me/profile` (partial: each field sent replaces what is stored), rules in `common::profile` and `common::countries` (the launcher keeps the same lists in `src/common/profile.ts`; the country list there is read from flag-icons itself).

## 8. Flags and mains outside the profile

Sarah's picks from the mockups (2026-11-09), each place on its own, and each player's two "Show next to name" choices on top (§7):

| Place | Shows |
| --- | --- |
| Leaderboard | A flag column before the name and a Main column (the primary main's stock icon) after it, blank when not shown. |
| Online list | The primary main's stock icon before the name (20 px, its own slot so names line up; Sarah's pick 2026-11-11 from the mockups). Without one shown (no main, "Show next to name" off, no icon for it on the card) a 6 px dot instead, never a stand-in character: one perceived lightness (OKLCH L 46%) in the theme accent's hue at half chroma, grey in the plain look and muted green in P+'s. Without a disc (no stock icons) no slot at all. The flag right of the name, just left of the invite envelope. |
| Chat | The tab and the open window's title show the name the same way: the flag before it; the window also has the primary main by the minimize button. |
| P+ rooms (Home's room list) | As before, with each player's flag before their name. No main. |
| Other Games rooms | The host's flag before their name; nothing else changes. |
| Spectate | Unchanged: it shows the character each player is playing in that game. Hovering a name shows the hover card below (it used to show only the code). |
| Any name | Hovering it shows a small floating profile: badge, name, code, flag and country, rank and rating, and up to 8 mains. |
| Header account (and Switch account's rows) | The picture is the primary main's stock icon, on its own (no frame), whatever "Show next to name" says (it's the player's own). Without one (no main, Random, no disc set up, offline) Fox's stock icon's outline, dotted like a placing rank emblem; in the header, hovering the account puts the profile's pencil inside it in the dots' colour (red while online play needs setting up). Nothing while the profile loads. |

Data: the server adds what to show beside each name (`profile::NameTag`: `country`, `main`, each only when set and shown) to the leaderboard's entries, the social stream's members (sent again when the player changes their profile), and the room list (`countries` by slot, from the slot uids mm now sends, filled once per mm snapshot; `hostCountry` for Other Games rooms). The hover card reads the profile (`GET /v1/profiles/<uid>`, kept 5 minutes, after a 450 ms pause on the name).

## 9. Deploying

- accounts: the two routes (`server/crates/accounts/src/lib.rs`).
- website: `website/profile/profile.css` (the page's only style), the flags (`assets/flags/`), the placing badge, and the top 10 links (`script.js`, `style.css`).
- **nginx (by hand on sarahvps2)**: `/profile/<code>` goes to accounts (`server/deploy/sarahvps2/nginx-brawl-online.conf`, the accounts `location`). Installed on prod 2026-11-09 (backup `/etc/nginx/brawl-online.conf.bak-20261109T215204Z`), ahead of the accounts code: until it ships, `/profile/...` is accounts' plain 404. The Caddyfile has the same route.
- **Staging and local runs** have nothing in front of accounts, so accounts serves the page's own files (`/profile/profile.css`, `/assets/...`: flags, rank badges, the font) from `WEBSITE_DIR` (staging.py sets it to its checkout's `website/`). Unset in production, where nginx serves them and accounts never sees those paths.
