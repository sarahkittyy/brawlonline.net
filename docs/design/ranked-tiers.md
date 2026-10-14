# Ranked tiers

Status (2026-10-14):
- **Built, committed locally, not pushed:** the server side (§6) and the badge in the launcher (Home ranked box, Leaderboard) and on the website (top 10).
- **Not started:** the rank-up and rank-down animations (§7).
- **Cutoffs:** revised the same day, after the user's review (§2). Platinum is called **Emerald** (the user: it looks like emerald).

The rating stays plain Elo (`server/crates/common/src/ranked.rs`). The rank is computed from it and only changes what is shown.

## 0. What the user asked for

2026-10-14, first request:
- Ranks instead of only a number. Badges found online, maybe tweaked, never generated.
- A demotion buffer.
- Elo ranges that fit today's prod ladder and a bigger pool later.

2026-10-14, after seeing the proposal:
- **Art:** Layerlab's [2D League Icon Pack](https://layerlab.itch.io/2d-league-icon-pack), bought by the user: 7 tiers, Bronze to Legendary.
- **Where:** a badge on Home's ranked box, and next to each player on the launcher's Leaderboard and the website's top 10.
- **Not now:** no rank-up or rank-down animations (many players grind ranked with the launcher closed), and no sounds.
- **Distribution:** tiers should feel rewarding and be evenly spread. Divisions count down: Gold 3 → 2 → 1 → Emerald 3, and a demotion goes from Emerald 3 back to Gold 1.
- **Placing:** a player whose rank is still too uncertain shows a dotted, unfilled outline with "3 more". The number comes from the uncertainty, within about 5-10 sets.
- **Division on the emblem:** at first glance the tags were too big next to the emblem. The division is now drawn on the emblem itself, procedurally. The tier name ("Gold 2") shows only on hover.
- **Wording:** the user asked for "3 more games" for placing. It counts ranked sets (best of three), so it says "3 more sets", and "3 more" where space is short. Swapping the word is one line in `rank_badge.messages.ts` and `website/script.js`.

## 1. Answers to the user's questions

**How much Elo per set (prod, 268 rated sets, 2026-10-09 to 10-14):**

| Phase | Average win | Average loss | Typical change (median) |
|---|---|---|---|
| Established (set 11 on, K = 32) | +8.6 | −13.8 | 8.7 |
| Provisional (sets 1-10, K 200 → 32) | +59.0 | −52.4 | 45 |

- Established gains are small because prod's pool is small. The average rating gap between opponents is 213, so the favourite usually wins a little and loses a lot.
- Between even players a set moves ±16.
- An active player plays about 4 sets a day. Their net change is about 33 a day (p90 74).

**How many tiers:**
- Seven fits the pack and gives an even spread. Every tier except the top two holds about a fifth of the players on a grown ladder (§2).
- Fewer tiers would make each one a long grind. More would need art the pack doesn't have.
- The top two are deliberately thin (Master a few percent, Legendary about one), so they stay something to chase.

**Are sub-tiers worth it?** Yes. Simulated, with the 100-point buffer below:

| Option | Median sets between rank-ups (young / mature ladder) |
|---|---|
| 7 tiers, no divisions | 55 / 90 (2-3 weeks of daily play) |
| 7 × 2 divisions | 20 / 36 |
| **7 × 3 divisions (chosen)** | **10 / 26** (a few days of play) |

- With the buffer, only 4-5 % of demotions are a bounce back over a line just crossed. Without one, 86-89 % are.
- Divisions don't make the badge more accurate: Elo itself jitters about ±50 around a player's skill at K = 32, so the tier shown matches true skill about 64 % of the time either way. Divisions are there for progress.

## 2. Cutoffs

Revised after the user's review (2026-10-14: "Legendary 1875 seems low ... Master 9 % and Legendary 6 % way too high"). The user was right that ratings keep spreading as people play more. Simulated, the 95th percentile rises from about 1800 on a young ladder to 1900-1950 on a mature one and about 2035 on a very mature one; the 99th from about 1970 to 2130-2230 to about 2330.

The cutoffs aim at the grown ladder: Bronze to Diamond about a fifth of the players each, Master a few percent, Legendary about one. Division 3 is each tier's lowest; divisions split a tier in thirds, so they widen toward the top, and a rank-up there takes longer. Legendary has no divisions.

| Tier | 3 | 2 | 1 | Young | Mature | Very mature | Prod now (≥5 sets) |
|---|---|---|---|---|---|---|---|
| Bronze | < 1050 | 1050 | 1100 | 13 % | 19-21 % | 22 % | 3 |
| Silver | 1150 | 1200 | 1250 | 20 % | 17-19 % | 16 % | 10 |
| Gold | 1300 | 1350 | 1400 | 26 % | 18 % | 17 % | 6 |
| Emerald | 1450 | 1510 | 1570 | 25 % | 19-21 % | 17 % | 12 |
| Diamond | 1625 | 1750 | 1875 | 17 % | 20 % | 22 % | 4 |
| Master | 2000 | 2085 | 2170 | 1 % | 2-3 % | 4 % | 0 |
| Legendary | 2250 | | | 0 % | 0.6-0.9 % | 1.6 % | 0 |

- **Ladder columns:** young is a median of 25 sets per player, mature 120, very mature 300. They come from the simulation: 1,200-1,500 players, true skill normal around 1400 with a standard deviation of 300-420, our K schedule, and matchmaking by rating. A skill fit of all prod results gives a spread in that range.
- **Prod:** 85 rated players. The top two today, 1887 and 1827, would be Diamond 1 and Diamond 2; Master and Legendary are still empty.
- **New players** start at 1400, which is Gold 1, the top of Gold. They are placing at first anyway (§4).
- **Where it lives:** `common::ranked::RANK_FLOORS`. If the spread drifts, change it, and run `pp-admin ranked recompute` to fix every stored rank.

## 3. Demotion buffer

Each player has a **held rank**, which changes only after a rated set:
- **Promotion:** when the rating reaches a higher division's floor, the held rank becomes the highest division reached. A big win can skip divisions.
- **Demotion:** only once the rating is **100 below the held division's floor**, and then **one division down**. Formally, the held rank becomes `max(rank(rating), rank(rating + 100))`, which is never above the held rank.
  - Example: a player promoted to Emerald 3 at 1450 drops to Gold 1 only below 1350, which is already Gold 2 territory.
  - Each further division down needs the rating another 50 below.
  - Someone who really declines does keep dropping.
- 100 points is about 6-7 more even-set losses than wins in a row: two divisions in the lower tiers, less than one in Diamond and Master.

## 4. Placement (variance-based)

Elo has no uncertainty of its own, so the server keeps one per player beside the rating: Glicko's rating deviation (RD).
- RD starts at 350.
- After each rated set, RD shrinks by how much that set tells about the player: `1/RD'² = 1/RD² + 1/d²`, with Glicko's `d²` from the opponent's rating and RD.
  - An even set against an established player tells the most.
  - A lopsided set, or one against another new player, tells less.
- RD only decides the placement badge. The rating itself still moves by the K schedule.
- **Placed** when RD ≤ 150, or after 10 sets, whichever comes first.
- **"X more"** is the number of even sets against an established player that would bring RD to 150. Lopsided sets bring it down more slowly, so X can stay the same after one.
- Replayed over prod's real history, players place after 6-10 sets:

| Sets to place | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|
| Players | 2 | 6 | 10 | 5 | 3 |

  Five even sets from scratch is enough.
- While placing, the badge is the dotted outline of the emblem's silhouette with a dashed "3 more sets" tag. The buffer starts once placed.
- Not done, possible later: RD growing during long inactivity, so a returning player re-places.

## 5. The badge (built)

**Art:** Layerlab's PNGs, with each division's numeral baked onto a plaque at the emblem's foot by a Python/PIL script. That is a procedural edit; the pack's license allows modifying the files, and it forbids any use with generative AI.
- **Large** (256 px): Roman numerals in Barlow Condensed 900 italic, the website's font (OFL).
- **Small** (128 px): a bigger plaque with plain bars, because letters blur at 30-40 px.
- **Plaque colours:** each tier's own palette (Bronze tan, Silver grey, Gold yellow, Emerald green, Diamond blue, Master red). Legendary has no plaque.

**Launcher:** `components/rank_badge/`, `RankBadge({ rank, setsToPlace, size })`.
- **Home ranked box:** the rating on the left, the emblem on the right (116 px, or 68 px on windows 850 px tall or less), the buttons under both.
- **Leaderboard:** a Rank column with a 40 px emblem.
- **Hover:** a tooltip names the rank ("Emerald 2"), and the image's alt text says the same.

**Website top 10:** a Rank column with the small emblem and a CSS hover or focus label.
- In the corner panel the emblem shrinks a little, names are cut with "…", and the panel's text scales with the window's height too. Ten rows then fit from 1180 × 560 up, clear of the Discord and GitHub icons.
- On the phone page the badges are full size.

**API (the launcher and site already read it; the server doesn't send it yet):**
- **Leaderboard entries and the Home ranked box:** `rank: { tier, division } | null` and `setsToPlace: number`.
  - The ranked box gets them from `GET /user/{uid}` `rank.tier` and `rank.setsToPlace`.
  - `tier` is one of bronze, silver, gold, emerald, diamond, master or legendary. `division` is 1-3, or null for Legendary.
- **Older servers:** without these fields there is no badge.
- **Mock data:** the launcher's mock backend gives ranks from the rating with these cutoffs. In the dev URL, `?mockRank=gold-2`, `?mockRank=legendary` or `?mockRank=placing-3` sets the mock user's rank.

**Previews:**
- Storybook stories: `components/RankBadge` (Large, Small) and `pages/Home/RankedBox` (Narrow, Wide, Short, Leaderboard).
- The website was checked against a mock leaderboard at 1600 × 900, 1180 × 560 and 1920 × 600, and on a 390 px phone.

## 6. Server (built)

- **Rules** in `common::ranked`: `RANK_FLOORS`, `Rank`, `held_rank` (§3), `rd_after`, `placed` and `sets_to_place` (§4), `TierState::after_set`, and `replay`, which folds `rating_events` in order. The two changes of one set use each other's RD from before the set. Unit-tested.
- **Migration 0012:**
  - `ratings.rank` (smallint 0-18, null while placing) and `ratings.rd`.
  - `rating_events.rank_before`, `rank_after` and `rd_after`.
- **Backfill:** after migrating, accounts (and `pp-admin migrate`) fills them by replaying the history, before serving, once (while any `ratings.rd` is NULL).
  - A replay of prod's history (2026-10-14) places about 24 of the 84 rated players; the rest show "X more sets".
- **Settle:** in the same transaction as the rating, both players' RD, placement and held rank are updated. The rating change row carries them too.
- **API:**
  - Leaderboard entries: `rank` and `setsToPlace`.
  - `GET /user/{uid}`: `rank.tier` and `rank.setsToPlace`.
  - Match history doesn't show ranks yet.
- **`pp-admin ranked recompute`:** replays every player with `ratings` locked against settles, and writes an audit row. Run it after a cutoff change or a manual void or re-rate (the prod corrections procedure).
- **Tests:**
  - e2e `ranked_tiers.rs`: sets through the report endpoint place both players. The API fields match, and recompute after real settles changes nothing. The backfill fills once.
  - admin `ranked_recompute.rs`: `migrate` fills, `recompute` fixes a wrong rank, and the audit row is written.

## 7. Later

- Rank-up and rank-down animations, when there is a place players see them. An earlier draft is in git history.
- A badge in match history beside each rating change, and the opponent's badge. Neither is asked for yet.
- **License:** Layerlab's license allows use and modification in the project, but forbids redistributing the files.
  - This repository is public on GitHub, so committing the PNGs (even edited) arguably makes them available for download.
  - Before committing, decide: keep them out of git and fetch them at build time, or ask Layerlab.
