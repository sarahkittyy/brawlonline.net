# Player profiles

_Built 2026-11-09 from Sarah's board card ("let's create some user features ..."). Replaces the launcher's "Profile & match history" page: the match history is its own page now, and the name, rank and rating are on a profile anyone can open, in the launcher and on the website._

## 1. What there is

| Where | What |
|---|---|
| Launcher sidebar | **Match history** (History icon, `/main/matches`): the player's matches only. Each name has the player's rank badge at the time of the match before it (no number; the rank's name on hover) and opens their profile. |
| User menu (top right) | **View Profile**, above Edit display name: the player's own profile. |
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

## 7. Deploying

- accounts: the two routes (`server/crates/accounts/src/lib.rs`).
- website: `website/profile/profile.css` (the page's only style) and the top 10 links (`script.js`, `style.css`).
- **nginx (by hand on sarahvps2)**: `/profile/<code>` goes to accounts (`server/deploy/sarahvps2/nginx-brawl-online.conf`, the accounts `location`). Until it's installed, `/profile/...` is a 404 from the static site, including the launcher's "open in browser" link. The Caddyfile has the same route.
