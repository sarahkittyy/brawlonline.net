# Online backend design (DRAFT, partial)

_Status 2026-10-06: **partial draft**, written under a usage cut-off. Sections marked **TODO** are unfinished. The per-file inventory in section 1 reuses the line references in `research/04-slippi-reference-architecture.md` (HEAD commits listed there). They have not all been re-checked one by one. Of the three read-only code sweeps, only the launcher sweep finished, and its results are folded into sections 1, 2.4 and 4.2. The Dolphin/Rust and Brawlback sweeps did not finish before the cut-off._

Path prefixes: `L` = `refs/slippi-launcher`, `R` = `refs/slippi-rust-extensions`, `I` = `refs/slippi-Ishiiruka/Source/Core/Core`, `C` = `refs/slippi-ssbm-c`, `ASM` = `refs/slippi-ssbm-asm`, `OM` = `refs/openmelee`, `LAD` = `refs/ashebennet-ladder`.

New clones made for this doc (source only): `refs/openmelee` (panchaea/openmelee @ `bbbbff5`, 2022-11-27) and `refs/ashebennet-ladder` (AsheBennet/ladder @ `240500a`, 2026-09-20). openskill.js was cloned into the session scratchpad only, not into `refs/`.

---

## 0. Summary of recommendations

| Decision | Recommendation |
|---|---|
| Matchmaking server | **New Rust service** (tokio + `rusty_enet` or the `enet` C bindings) that speaks Slippi's `create-ticket` / `get-ticket-resp` ENet+JSON protocol byte for byte. Use openmelee **as a protocol reference only** (GPL-2.0, so we could take code, but it is a 2022 alpha with blocking bugs; see 2.3). |
| Accounts, codes, rating, reports, website API | **One Rust (axum + sqlx) HTTP service** backed by **PostgreSQL 16**, exposing a small GraphQL-shaped JSON API that mirrors the operations the Slippi launcher and Rust extensions call. One codebase and one language with the matchmaking server, and the matchmaking server shares the crate for types and play-key checks. |
| Auth | Self-hosted: Argon2id password hashes, opaque random session tokens for the launcher, and a separate long-lived **play key** for Dolphin, exactly as Slippi does. Email is optional at the friends-only stage (see 3). |
| Rating | **OpenSkill (Weng-Lin, Plackett-Luce) with `tau` and `limitSigma`**, displayed as the ordinal (mu - 3 sigma) scaled onto Slippi's 0-2500-ish range. This is very likely what Slippi itself uses (see 4.1). It also handles doubles natively. |
| Replays | Object storage on the box's disk (MinIO or plain files behind signed URLs). The client gzips and PUTs, like Slippi. |
| Website | Server-rendered pages (askama/minijinja templates) from the same axum service: sign-up, login, profile (code, rank), leaderboard. No SPA. |
| Relay | **Yes, add a minimal UDP relay** (TURN-like, ENet-agnostic) as a fallback only. Slippi has none and CGNAT users simply cannot play; with a tiny friends group, one CGNAT friend blocks the whole project. |
| Hosting | One OVH Debian box: systemd units (no Kubernetes), Postgres from Debian packages, Caddy for TLS, restic backups off-box, Prometheus node exporter + a `/metrics` endpoint per service. docker-compose for local development and the harness only. |

**Top risks** (details in section 8):
1. **Session start model.** Brawlback Gen 2 rolls back the *whole machine* from `scBoot`, using Dolphin's host/guest netplay lobby. Slippi's UX starts the online session *mid-game* at the CSS. Whole-machine rollback needs identical machines at session start, so in-game matchmaking needs either a state transfer, a synchronized reboot, or gameplay-only state. That decision belongs to the netcode work, not the backend, but it shapes the EXI contract in section 5.
2. **NAT.** There is no relay in Slippi. CGNAT and symmetric-NAT users fail silently. Mitigation: hole-punch exactly like Slippi, plus a relay fallback.
3. **Trust in client reports.** Ranked results come only from clients. Slippi cross-checks both reports and keeps replays. We must do the same from day one of ranked, and reject results with banned states (section 6).

---

## 1. Inventory of Slippi's network interactions

Verdict column: **R** = replicate for parity, **A** = adapt, **D** = drop.

### 1.1 Auth and account creation (Firebase)

| Interaction | Where | Verdict |
|---|---|---|
| Email/password login `signInWithEmailAndPassword` | `L/src/renderer/services/auth/multi_account.service.ts:26` | **R** (own auth API) |
| Sign-up via callable `createUserNew({email,password,displayName})`, then login | `multi_account.service.ts:104-117` | **R** |
| Email verification, password reset, display-name update | `L/src/renderer/services/auth/auth.service.ts:107-182` | **R** for reset and rename; verification optional (3.3) |
| Up to 5 signed-in accounts (multi-account) | `multi_account.service.ts:1-19,47` | **R** (launcher-side only, cheap) |
| Firebase ID token as `Authorization: Bearer` on every GraphQL call | `L/src/renderer/services/slippi/slippi.service.ts:68-82` | **R** (our session token instead) |
| Rules acceptance `userAcceptRules`, `currentRulesVersion = 1` | `L/src/common/constants.ts:10` | **R** |

### 1.2 `user.json` lifecycle

- Written by the launcher (`L/src/dolphin/playkey.ts:5-9`, `L/src/dolphin/setup.ts:69-87`): `{uid, playKey, connectCode, displayName, latestVersion}`. Rewritten on every Play if it differs (`setup.ts:75-87`).
- Paths: Ishiiruka `%APPDATA%\Slippi Launcher\netplay\User\Slippi\user.json`, macOS `~/Library/Application Support/com.project-slippi.dolphin/Slippi/user.json` (`L/src/dolphin/install/ishiiruka_installation.ts:35-55,245-262`; mainline `mainline_installation.ts:35-56,255-258`).
- Dolphin's Rust `user` crate polls for the file every 500 ms (`R/user/src/watcher.rs:67,71`). It fetches chat messages and rank from `GET users-rest-dot-slippi.uc.r.appspot.com/user/{uid}?additionalFields=chatMessages,rank` (`R/user/src/lib.rs:23,373-421`), and **deletes user.json on logout** (`R/user/src/lib.rs:274-287`).
- **Correction from the launcher sweep:** the *launcher* never deletes `user.json`. `deletePlayKeyFile` (`L/src/dolphin/playkey.ts:11-17`) has no callers. The file is rewritten on Play only when its content differs, using a lodash `isEqual` check (`L/src/dolphin/setup.ts:75-92`). Deletion happens only from Dolphin's in-game logout (Rust, above) or on a reinstall.
- Legacy in-game login opens `https://slippi.gg/online/enable?path=<user.json path>` (`R/user/src/lib.rs:212-228`).
- Side files: `direct-codes.json` and `teams-codes.json` (recent codes, `R/user/src/lib.rs:100-110`). They are local only.

Verdict: **R**, with an identical schema so the Rust crate needs only new URLs. openmelee added `matchmakingHost` / `userDiscoveryUrl` fields to `user.json` (`OM/src/models.rs:289-305`). We should **not** do that: hosts belong in the build/config, not in a per-user file.

### 1.3 Connect-code assignment

- Prefix: 2-4 letters, `/^[a-zA-Z]+$/`, defaults to the first 4 letters of the display name (`L/src/renderer/lib/validate/validate.ts:26-44`, `L/src/renderer/components/activate_online_form/activate_online_form.tsx:41-45,94-104`). The server appends `#NNN` via GraphQL `userInitNetplay(codeStart)` (`L/src/renderer/services/slippi/graphql_endpoints.ts:181-194`).
- In-game maximum is 8 characters including `#` (`CONNECT_CODE_LENGTH 8`, `I/HW/EXI_DeviceSlippi.h:24`). Display name: 1-15 characters (`validate.ts:46-61`, `MAX_NAME_LENGTH 15`).
- Changing a code later is a one-time paid action on Slippi. **D** for the paid part; we allow one admin-approved change.
- **Our rule:** uppercase the prefix, then pick the lowest free numeric suffix from a random start in 1..999. Fall back to 4 digits only if 999 are taken. The total must stay at 8 characters or fewer, so a 4-letter prefix with a 4-digit suffix is not allowed. Codes are unique and immutable, and a deleted account's code is reserved for 90 days.

### 1.4 Matchmaking ticket protocol (ENet + JSON)

Client: `I/Slippi/SlippiMatchmaking.cpp`. Server: closed (`mm.slippi.gg`, dev `mm2.slippi.gg`, UDP **43113**, `SlippiMatchmaking.h:98-100`).

1. The game sends EXI 0xB4 FIND_OPPONENT: mode (1 byte) + Shift-JIS connect code (18 bytes) (`I/HW/EXI_DeviceSlippi.cpp:1916`).
2. Fixed-rules modes (ranked, unranked, party) require the ISO MD5 check first: "Cannot queue for this mode with a modded ISO known to desync" (`SlippiMatchmaking.cpp:284-307`).
3. The client binds a local UDP port `41000 + rand()%10000`, or the user's forced port, and **reuses that port for P2P**. Talking to the mm server opens the NAT mapping, and that is the hole punch (`:313-331`, comment `:321-324`; `SlippiNetplay.cpp:101-115`).
4. `create-ticket` (`:426-436`): `{type:"create-ticket", user:{uid, playKey, connectCode, displayName}, search:{mode, connectCode:[Shift-JIS bytes]}, appVersion, ipAddressLan:"a.b.c.d:port"}`. The reply `create-ticket-resp` may carry `error`, which is shown in-game.
5. `get-ticket-resp` (`:473-660`): `{matchId, isHost, players:[{uid, displayName, connectCode, port(1-4), isLocalPlayer, ipAddress, ipAddressLan, chatMessages[16], rank{...}, isBot}], stages:[ids], items, latestVersion}`.
   - If both players share an external IP, the LAN address is used (`:590-609`).
   - `isHost` is the decider.
   - The default stage list applies if `stages` is missing (`:625-641`).
   - `matchId` containing `mode.ranked` marks ranked (`:651`).
   - openmelee builds `matchId` as `mode.<mode>-<rfc3339>` (`OM/src/matchmaking.rs:281-284`).
6. The client disconnects from mm and ENet-connects to every peer from both sides at once. In 1v1, a failure **requeues** (back to step 3). In teams it shows "Could not connect to players: …" (`handleConnecting` `:810-909`).
7. There is no relay. User options are "Force Netplay Port" and "Force LAN IP".

Region and ping: the client sends no region or ping field. Slippi infers region server-side, presumably from IP geolocation; three leaderboards exist (NA, EU, Other; Upcomer). **TODO:** confirm from the unfinished Dolphin sweep whether any latency field exists.

Verdict: **R**, byte-compatible JSON, so the ported client code stays unchanged apart from the hostname.

### 1.5 Direct connect

The same flow with `mode=2` and `search.connectCode`. The server pairs two tickets that name each other's codes. Teams (mode 3) uses a shared host code for 4 players, and `%3` codes for 3 players. Autocomplete comes from local history (0xBE, `I/HW/EXI_DeviceSlippi.cpp:2012-2105`). **R**.

### 1.6 Ranked queue, rating fetch, rank display

- Rank fetch: `ratingOrdinal`, `ratingUpdateCount`, global/regional placement (`R/user/src/lib.rs:375`; `L/src/renderer/services/slippi/slippi.service.ts:44-45`; `L/.../graphql_endpoints.ts:18`).
- Post-set result: `getRankedMatchPersonalResult(matchId, fbUid, playKey)` → `{ordinal, ratingChange, placements, updateCount}` (`R/user/src/rank_fetcher/network.rs:14-63,116-181`).
- In-game rank UI: `C/Scenes/CSS/RankInfo/RankInfo.{h,c}`. The response struct is `{visibility, status, rank, ratingOrdinal, ratingUpdateCount, ratingChange, rankChange}` (`C/ExiSlippi.h:159-167`). There are 20 rank values from `RANK_UNRANKED` ("PENDING") to `RANK_GRANDMASTER` (`RankInfo.h:24-76`), and **`PLACEMENT_THRESHOLD = 5`** sets (`RankInfo.h:93`).
- Match lifecycle reporting `reportOnlineMatchStatus`: `connecting`, `game_setup_N`, `game_start_N`, `normal_completion`, `abnormal_completion`, `abandoned`, `poor_performance` (`I/HW/EXI_DeviceSlippi.h:124-143`, `R/game-reporter/src/queue.rs:111-139`). Server statuses are ASSIGNED, COMPLETE, ABANDONED, ORPHANED, TERMINATED and ERROR (`R/user/src/rank_fetcher/network.rs:85-93`).
- Paywall and "Ranked Day" (`L/src/renderer/pages/home/overview/ranked_day_status/ranked_day_status.tsx`): **D**.

Verdict: **R** for everything except subscriptions.

### 1.7 Match reporting and replay upload

- `reportOnlineGame` (`R/game-reporter/src/types.rs:40-104`, `queue.rs:294-326`) with fields: `fbUid`, `mode`, `players[{fbUid, slotType, damageDone, stocksRemaining, characterId, colorId, startingStocks, startingPercent}]`, `isoHash`, `matchId`, `playKey`, `gameDurationFrames`, `gameIndex`, `tiebreakIndex`, `winnerIdx`, `gameEndMethod`, `lrasInitiator`, `stageId`. It retries 5 times with backoff.
- The response carries a signed `uploadUrl`. The client gzips the replay and PUTs it (`queue.rs:339-384`).
- Verdict: **R**, plus our additions in section 6: a `bannedStateFlags` field, the gecko/codeset hash and the disc revision.

### 1.8 Anti-abuse

- ISO hash: the launcher's SHA-1 table (`L/src/main/verify_iso.ts:14-138`) and Dolphin's MD5 (`R/game-reporter/src/iso_md5_hasher.rs:52-66,151-187`). **A**: for us, this becomes an RSBE01 Rev 1/Rev 2 MD5 plus a hash of the P+ codeset/SD build plus the netplay-codes GCT hash.
- Client version: `appVersion` in the ticket and `latestVersion` in the reply. The in-game "Update" item locks the menu when an update is required (`ASM/Online/Menus/.../HandleOnlineLockedOptions.asm:27-79`). **R**. The server must also *refuse* tickets below a minimum version; the client-side lock alone is not enough.
- Ranked poor-performance termination (`I/HW/EXI_DeviceSlippi.cpp:1326-1415`) and abandonment tracking. **R** (later phase).

### 1.9 Spectating and broadcast relay

- Dolphin spectator ENet server on UDP 51441 (`slippi-wiki/SPECTATOR_PROTOCOL.md`).
- Launcher broadcast through Slippi's WebSocket relay (`L/src/broadcast/broadcast_manager.ts:141-205,480-525`, `spectate_manager.ts:118-266`).
- Spectate Remote Control on localhost:49809.
- Verdict: **D for the first release**, **R later**. It is not needed for "friends play". WebSocket at `SLIPPI_WS_SERVER`, with subprotocols `broadcast-protocol` and `spectate-protocol`. Headers are `api-version: 2` and `authorization: Bearer <token>`, plus `target: <viewer uid>` for broadcasts (`L/src/broadcast/broadcast_manager.ts:141-169`).
  - Broadcaster sends `get-broadcasts`, `start-broadcast{name, broadcastId}`, `send-event{broadcastId, event{type, cursor, nextCursor, payload b64}}` and `stop-broadcast` (`:188-205, 407-412, 496-500`). Server replies `get-broadcasts-resp` and `start-broadcast-resp{broadcastId, recoveryGameCursor}` (`:279-349`).
  - Viewer sends `list-broadcasts`, `watch-broadcast{broadcastId, startCursor}` and `close-broadcast`. Server replies `list-broadcasts-resp{broadcasts[{id, name, broadcaster{name, uid}}]}` and `events{broadcastId, cursor, events[]}` (`L/src/broadcast/spectate_manager.ts:46-209, 264-269`).
  - Close code 1006 means reconnect with a fresh token.

### 1.10 Launcher update checks

- Launcher self-update via electron-updater from GitHub releases. **R**, pointed at our releases.
- Dolphin builds via backend `getLatestDolphin(purpose, includeBeta)` → per-OS URLs, compared with `dolphin --version` (`L/src/dolphin/install/fetch_latest_version.ts:56-104`). **R**, served by our API from a static table the release script updates.
- P+ Dolphin's own GitHub updater (`DolphinQt/MainWindow.cpp:1443,1479`): disable it in our fork; the launcher owns updates.
- News feed (Bluesky, GitHub releases, Medium): **D** for now.

- **Telemetry:** none. There is no Sentry or analytics; logs stay local.
- **NAT diagnostic:** Google STUN (`stun1/stun2.l.google.com:19302`), UPnP/NAT-PMP on the LAN, and a traceroute to the user's own public IP for CGNAT (`L/src/main/network_diagnostics.ts:8-111`). **R**, pointed at our own STUN. The mm server can answer a STUN-like probe.
- Other launcher fetches (news via rss2json/GitHub/Bluesky, meleemajors.gg, ip-api.com opt-in, smash-map.com): **D**.
- The launcher uploads no replays. Dolphin's game-reporter does (1.7). The launcher only *downloads* `slippi://` replays from GCS (`L/src/main/main.ts:232-259`).

---

## 2. Component architecture

```
                 HTTPS (Caddy, TLS)                           UDP
 Launcher ───▶ api  (axum: auth, codes, users-rest, ratings,  mm (ENet :43113) ◀── Dolphin
 Website  ───▶       reports, signed upload URLs, admin)       relay (UDP :43200-43299, fallback)
 Dolphin  ───▶       │                                         │
                     ▼                                         ▼
               PostgreSQL 16  ◀──────── shared crate `core` (types, play-key check, rules)
               /srv/replays (files, or MinIO behind signed URLs)
```

### 2.1 Stack

- **Rust** for every service. The mm server must handle ENet/UDP with tight timers, and the Dolphin side already links Rust (`slippi-rust-extensions`), so client and server can share type definitions. openmelee proves the ENet + axum + sqlx combination. The alternative is TypeScript/Fastify like the ladder MVP. It is fine for HTTP, but it would give us two languages and weaker ENet bindings.
- **PostgreSQL 16** from Debian packages. Avoid SQLite (openmelee uses it) because the rating and report transactions need row locks.
- **Caddy** for automatic TLS. **systemd** units with `DynamicUser=` and `ProtectSystem=strict`. **restic** nightly to an off-box target (OVH Object Storage or the user's second VPS). Postgres backups via `pg_dump` plus WAL archiving once it matters.
- **Monitoring:** `prometheus-node-exporter` and a `/metrics` endpoint on each service. Grafana is optional; a cheap external uptime check (read-only) is enough at first.

### 2.2 Data model (Postgres, first cut)

```
users(uid uuid pk, email citext unique null, email_verified bool, pw_hash text,
      display_name text, connect_code text unique null, play_key_hash bytea,
      rules_version int, created_at, banned_until timestamptz null, role text)
sessions(token_hash bytea pk, uid fk, created_at, last_seen, user_agent)
mm_matches(match_id text pk, mode smallint, created_at, players uuid[], is_host uuid,
           stages smallint[], ruleset_version int, region text, status text)   -- ASSIGNED..ERROR
game_reports(match_id fk, game_index int, tiebreak_index int, reporter uuid,
             payload jsonb, iso_hash text, build_hash text, banned_flags int, created_at,
             primary key(match_id, game_index, tiebreak_index, reporter))
games(match_id, game_index, tiebreak_index, winner_idx, end_method, stage_id,
      agreed bool, replay_key text null, flagged text null)
ratings(uid, season_id, mu float8, sigma float8, update_count int, updated_at,
        primary key(uid, season_id))
rating_events(id bigserial, uid, season_id, match_id, mu_before, sigma_before,
              mu_after, sigma_after, ordinal_change, created_at)
seasons(id, starts_at, ends_at)
audit_log(id, actor, action, target, detail jsonb, at)
```

### 2.3 Matchmaking server: evaluation of openmelee

`OM` = panchaea/openmelee, GPL-2.0 (`OM/LICENSE`), so its license is compatible. It has 2,248 lines total and its last commit was 2022-11-27, so it is unmaintained. **Quality is not enough to fork:**

- `host.service(1000).map(|event| handle_enet_event(event, pool.clone()))` calls an `async fn` and drops the returned future without awaiting it (`OM/src/matchmaking.rs:133-136` vs `:150`). After the last commit ("validate play_key", which made the handler async), **no ticket is ever processed**.
- Direct pairing uses itertools `group_by` on an unsorted iterator (`:196-214`). That only groups *adjacent* peers, so two players searching for each other are usually not matched.
- Unranked pairs the queue in join order every loop (`:216-235`). Matched peers are never removed or disconnected, so they are re-matched every second. It has no ping, region or rating awareness, and no timeouts.
- Ranked and teams are not implemented (`:178-181`). Stages are hard-coded Melee IDs (`OM/src/game.rs`).
- Panics on malformed input (`.unwrap()` on UTF-8/JSON, `:154-155`), so one bad packet kills the server.
- **Reusable:** the serde model of the ticket messages and its tests (`OM/src/matchmaking.rs:16-109, 315-...`), including Shift-JIS decoding with NFKC normalization of `search.connectCode` (`:37-46`); connect-code and display-name validators (`OM/src/models.rs:307-390`); Argon2 hashing (`:249-256`).

**Recommendation:** write a new mm server and copy the openmelee message structs and validators, with attribution under GPL-2.0-or-later.

Design of the new server:
- A single-threaded ENet event loop owning all queue state in memory. Postgres is only for play-key checks (cached), match records and rating lookups, via a tokio channel to an async worker.
- On `create-ticket`: validate the play key, minimum `appVersion`, build hash, mode and code format. Reply `create-ticket-resp` with `error` on any failure (the string is shown in-game, maximum 120 characters).
- Queues per (mode, region bucket). Direct: a `HashMap<(me, target), peer>`; match when `(target, me)` exists. Unranked: FIFO with a widening rating band. Ranked: rating band ±150 ordinal widening by 50 every 15 s. Region: the ENet peer IP geolocated with an offline DB (DB-IP Lite, CC-BY); later also a measured RTT to each region's relay.
- On match: write `mm_matches`, send `get-ticket-resp` to each peer with `isLocalPlayer` set per recipient, `ipAddress` = the observed external `ip:port`, `ipAddressLan`, `isHost` (lowest uid hash, so it is deterministic), the `stages` list from the ruleset, and **a relay allocation** (new optional field `relay: {addr, token}`; old clients ignore it). Then `disconnect_later` both peers.
- Timeouts: ticket TTL 10 minutes; ENet peer timeout 10 s; a direct-connect ticket stays until cancelled (the CSS Z button sends `CLEANUP_CONNECTION`, which disconnects).
- Scaling: one process handles thousands of tickets. A second region is a second mm instance; the shared Postgres is reached over WireGuard.

### 2.4 Auth/accounts, connect codes, rating, reports: API surface

JSON over HTTPS. Names mirror Slippi's GraphQL operations so the launcher fork maps one to one. The launcher's complete operation list (`L/src/renderer/services/slippi/graphql_endpoints.ts:46-211`) is:
- `getUser` (validate / user data incl. `private{playKey}`, `rankedNetplayProfile`, `rulesAccepted`, `activeSubscription`)
- `getLatestDolphin(purpose, includeBeta)`
- `GetChatMessageConfigData`
- `userRename`
- `userAcceptRules(num)`
- `userInitNetplay(codeStart)`
- `userSetChatMessages`

Option: serve a tiny GraphQL endpoint with exactly these operations (async-graphql) so the launcher's Apollo code is untouched.

| Endpoint | Mirrors | Auth |
|---|---|---|
| `POST /v1/auth/signup {email?, password, displayName}` | `createUserNew` | invite code (friends phase) |
| `POST /v1/auth/login` → `{sessionToken}` | `signInWithEmailAndPassword` | — |
| `POST /v1/auth/logout`, `POST /v1/auth/password-reset/{request,confirm}` | Firebase | session |
| `GET /v1/me` → `{uid, displayName, connectCode, playKey, rulesVersion, ...}` | `getUser` incl. `private.playKey` | session |
| `POST /v1/me/netplay {codeStart}` | `userInitNetplay` | session |
| `POST /v1/me/rename`, `POST /v1/me/accept-rules` | `userRename`, `userAcceptRules` | session |
| `GET /user/{uid}?additionalFields=chatMessages,rank` | users-rest | public (as Slippi) |
| `GET /v1/dolphin/latest?purpose=&beta=` | `getLatestDolphin` | — |
| `POST /v1/report/game` → `{uploadUrl}` | `reportOnlineGame` | play key |
| `POST /v1/report/match-status` | `reportOnlineMatchStatus` | play key |
| `POST /v1/report/set-complete` | 0xC2 path | play key |
| `GET /v1/ranked/result?matchId=` | `getRankedMatchPersonalResult` | play key |
| `GET /v1/leaderboard?region=&season=` | slippi.gg | — |

Play key: 32 random bytes in base64url. Store only its SHA-256. Rotate it on password change and on admin action.

### 2.5 Replay storage

Content-addressed files under `/srv/replays/<yyyy>/<mm>/<sha256>.slp.gz` on the box's second disk. `uploadUrl` is an HMAC-signed, 10-minute PUT URL to the api service, which streams the body to disk with a size cap (10 MB). Keep everything for ranked; prune unranked after 90 days. **TODO:** the Brawl replay format itself is a client-side decision (see research 04 §7 item 10).

### 2.6 Website

Server-rendered pages: `/signup`, `/login`, `/reset`, `/user/{code}` (code, display name, rank badge as **text plus a P+/Brawl-derived glyph only**, no new art), `/leaderboard`, `/admin`. **TODO:** wireframes are not needed; they copy slippi.gg's information layout only.

### 2.7 Relay

Recommended. It is a stateless UDP forwarder: both peers send `RELAY_HELLO{token}` to `relay:43200`, then the relay forwards datagrams between the two bound source addresses, so ENet runs unchanged on top. The client tries direct and LAN for 3 s, then the relay. It is used only when the punch fails, so bandwidth stays small (rollback input traffic is about 10-20 kbit/s per player). Rate-limit it per token and expire it at match end. It is not visible to users (no new UI), so it complies with "no novel UI".

### 2.8 Admin tooling

A CLI in the same binary (`ppo-admin user ban|unban|rename|reset-code|invite create`, `match void`, `rating recompute --season`) plus a read-only `/admin` web page for flagged games. Every action goes into `audit_log`.

**TODO:** per-component scaling notes beyond the mm server; resource budget for the box (expected tiny: under 1 vCPU and under 2 GB RAM at friends scale).

---

## 3. Self-hosted auth (replacing Firebase)

- **Hashing:** Argon2id (m=64 MiB, t=3, p=1) via the `argon2` crate, as openmelee does (`OM/src/models.rs:249-256`).
- **Sessions:** opaque 32-byte tokens, hashed at rest, sliding 90-day expiry; the launcher stores them the way it stores Firebase refresh tokens. **Play key** as in 2.4: Dolphin never sees the password or session.
- **Client-visible flow** stays identical: launcher login → `GET /v1/me` → write `user.json {uid, playKey, connectCode, displayName, latestVersion}` → Dolphin's Rust crate watches it. Logout deletes it.
- **Email:** *not required* for the friends phase. Sign-up takes an **invite code** issued by the admin CLI, and password reset is an admin-issued one-time link. Add email (verification plus self-service reset through an SMTP relay) before opening sign-ups publicly. The Quick Start `VERIFY_EMAIL` step is skipped when the server reports `emailVerificationRequired: false`.
- **Rate limiting:** per-IP and per-account token buckets on login, signup and reset (5/min, 20/h); mm `create-ticket` limited to 1 per 2 s per uid. Use fail2ban on Caddy logs for floods.

---

## 4. Rating system

### 4.1 What Slippi uses (evidence)

1. The API field is `ratingOrdinal` (`R/user/src/lib.rs:375`, `C/ExiSlippi.h:163`). "Ordinal" is OpenSkill's term for mu - 3 sigma.
2. Fizzi (Slippi's developer) said on 2022-03-07 that openskill.js is used for hidden unranked MMR (https://x.com/Fizzi36/status/1500897660932403204).
3. **New finding:** on **2022-03-16** Jas Laferriere (Fizzi36@gmail.com) authored openskill.js commit `624b777` "feature: add tau option to rate (#233)". It adds the `tau` additive dynamics factor ("ensure the rating will stay more pliable after many games") and `preventSigmaIncrease` ("prevents sigma from ever going up … helps prevent ordinal from ever dropping after winning a game which can feel unfair"). Upstream later renamed the option `limitSigma` (`50ccbed`, 2024) and removed the old name (`376a6ed`, 2026). Ranked launched nine months later. **Conclusion: Slippi ranked is almost certainly openskill.js (Weng-Lin) with `tau` and sigma limiting, displayed as a scaled ordinal.** The exact mu/sigma/beta/tau values and the display scaling are not public.
4. Placements: 5 sets (`PLACEMENT_THRESHOLD = 5`, `C/Scenes/CSS/RankInfo/RankInfo.h:93`). Before that the rank shows "PENDING".
5. Leaderboards: three regions (NA, EU, Other), refreshed every 2 minutes (Upcomer coverage of the launch). Decay: no evidence it exists; players were still asking for it in Jan 2024 (https://x.com/SsbmGinger/status/1744434985317089545). Treat it as **no decay**, with seasons resetting ratings instead (Season 4: 2025-10-20 to 2026-04-20; research 04 §3.4).
6. Ratings are updated per **set** (`ratingUpdateCount` is "sets played", `L/src/renderer/services/slippi/slippi.service.ts:44-45`).

### 4.2 Tier thresholds (copy exactly)

From `I/Slippi/SlippiMatchmaking.cpp:677-790`, `R/user/src/rank_fetcher/rank.rs:4-112` and `L/src/renderer/services/slippi/calculate_rank.ts` (via research 04 §3.4). Names come from `C/Scenes/CSS/RankInfo/RankInfo.h:55-76`.

| Rank | Ordinal (display) |
|---|---|
| Pending | fewer than 5 sets |
| Bronze 1 / 2 / 3 | ≤ 765.42 / ≤ 913.71 / ≤ 1054.86 |
| Silver 1 / 2 / 3 | ≤ 1188.87 / ≤ 1315.74 / ≤ 1435.47 |
| Gold 1 / 2 / 3 | ≤ 1548.06 / ≤ 1653.51 / ≤ 1751.82 |
| Platinum 1 / 2 / 3 | ≤ 1842.99 / ≤ 1927.02 / ≤ 2003.91 |
| Diamond 1 / 2 / 3 | ≤ 2073.66 / ≤ 2136.27 / ≤ 2191.74 |
| Master 1 / 2 / 3 | ≤ 2274.99 / ≤ 2350 / > 2350 |
| Grandmaster | ≥ 2191.75 **and** a global or regional leaderboard placement (top 300 global by community account; regional cut-off unverified) |

Verified against the launcher: `L/src/renderer/services/slippi/calculate_rank.ts:3-51`.
- 0 sets is NONE and fewer than 5 sets is PENDING (`:3-9`).
- Tiers are checked top-down with `>=` on the bucket floors (765.43, 913.72, … 2275, 2350), which is equivalent to the ≤ table above.
- **Grandmaster** = `rating >= 2191.75 && (dailyGlobalPlacement || dailyRegionalPlacement)` (`:12-14`; `slippi.service.ts:39-53`). The placement fields come from `rankedNetplayProfile{ratingOrdinal ratingUpdateCount dailyGlobalPlacement dailyRegionalPlacement}` (`graphql_endpoints.ts:61-108`).
- **TODO:** cross-check the Dolphin `rank.rs` copy.

### 4.3 Our choice

**OpenSkill** (Rust crate or a direct port of openskill.js PL, MIT-licensed so it is GPL-compatible), not Glicko-2:
- It matches Slippi's behaviour, including "your rating never drops after a win" (`limitSigma`).
- It supports 2v2 natively, which Glicko-2 does not. Slippi has no ranked doubles, but our unranked teams MMR can use it.
- The AsheBennet ladder's Glicko-2 (`LAD/src/rating/glicko2.ts`) is per-game with a rating period of one, and the repo has **no license file**, so it is not reusable. Its one-report-decides-the-winner logic (`LAD/src/match/result.ts:57-104`) is also unsafe for ranked.

Parameters (initial, to tune on our data): mu = 25, sigma = 25/3, beta = 25/6, tau = 0.1, limitSigma = true, model Plackett-Luce. Display = `(mu - 3·sigma) · K + C`, chosen so a new player's first placed ordinal lands around 1100 (Silver 1). The game, launcher and website all compute the tier from the display value with the table above. One update per completed set (Bo3), matching Slippi.

### 4.4 Disconnects and desyncs (Slippi-equivalent)

- Abandon (a player closes Dolphin, or holds Z to disconnect in ranked): reported `abandoned` by the remaining side. The leaver takes a set loss; the remaining player gets a win only if at least one game has been played, otherwise the set is voided.
- `poor_performance` termination and `abnormal_completion` with desync recovery failing: **void the set, no rating change**, and log it per uid. Repeat offenders (more than 3 in 24 h) get a cooldown from ranked (Slippi's "ban warning").
- The two clients' reports must agree on winner, stocks and end method. On disagreement the game is flagged, the rating is held, and the replay is kept for admin review.

---

## 5. Game-integration contract

**TODO (largely unfinished).** Plan:

- Keep Slippi's command *semantics* and numbering where possible, and send them over the channel our fork actually uses. Brawlback Gen 2 has no EXI device: it was deleted in P+ Dolphin `10d50e5d2`, and HLE hooks are used instead (research 01 §3.2). The proposal is a small EXI device (or a Syriinge plugin to HLE mailbox) that carries: `FIND_OPPONENT(mode, code[18])`, `GET_MATCH_STATE` (state IDLE / INITIALIZING / MATCHMAKING / OPPONENT_CONNECTING / CONNECTION_SUCCESS / ERROR, error string, local port, peer names/codes, `isHost`, rules block), `SET_MATCH_SELECTIONS(char, costume, stage, team)`, `CLEANUP_CONNECTION`, `GET_ONLINE_STATUS`, `OPEN_LOGIN`, `UPDATE`, `REPORT_GAME`, `FETCH_CODE_SUGGESTION`, `SEND_CHAT_MESSAGE`, `GET_RANK`/`FETCH_RANK`, and later the ranked GameSetup steps (`GP_COMPLETE_STEP`/`GP_FETCH_STEP`, `OVERWRITE_SELECTIONS`, `REPORT_SET_COMPLETE`).
- Mapping table Slippi 0xB3-0xC4/0xE1-0xE5 ↔ Brawlback Gen-1 `CMD_*` (`refs/brawlback-asm/include/exi_packet.h`) ↔ ours: **TODO** (the Brawlback sweep did not finish).
- Menus to build, reusing Brawl's Wi-Fi flow (Main menu → "Play Wi-Fi", relabelled → With Friends = **Direct**, With Anyone = **Unranked/Ranked**, plus Teams), using only existing Brawl/P+ graphics and fonts:
  1. the online mode list with name and code display;
  2. connect-code entry (Brawl's name-entry keyboard in code mode, with history autocomplete);
  3. CSS status panel texts (searching, connecting, opponent name, errors);
  4. rank text on the CSS and rating change after a set;
  5. ranked stage strike/counterpick.
  Screen details and hook addresses: **TODO**.
- **Session start (top risk 1).** Whole-machine rollback needs identical emulated machines when the session starts. Slippi's UX connects at the CSS, mid-game. The options are (a) a synchronized netplay reboot after matchmaking, (b) a host-to-guest state transfer at connect time, or (c) game-state-only rollback as Slippi does. The backend contract above is the same for all three: the backend's job ends at `get-ticket-resp`. The netcode work must choose; it is listed in Open questions.

---

## 6. Online-mode rules enforcement

QA finding (`harness/tools/qa_reachability.py`, run `run/artifacts/qa-reachability/report.json`, 2026-10-06, vanilla P+ v3.2 fixed-delay netplay):
- The joiner's controller opened the **Code Menu** on the CSS and in a match (`0x804E0034 == 4` on both instances), and turned on **Debug Mode** (`0x80583FFF = 1`).
- **Wario-Man / Giga Bowser** were reachable by holding L when leaving the CSS (CSS char 0x2D/0x2C, fighter 0x31/0x30).
- **Holding B** on the CSS took both players back to `muMenuMain`.

| | Direct | Unranked | Ranked |
|---|---|---|---|
| Code Menu (L+R+D-Down) | disabled while online | disabled | disabled |
| Debug Mode / other debug bytes | forced off every frame online | forced off | forced off |
| Giga Bowser / Wario-Man (hold L) | blocked: the hold-L transform is ignored online | blocked | blocked |
| Hold B to the main menu | replaced by Slippi's "hold Z to disconnect" (cleanup, return to the online menu) | same | same, counts as abandon after game 1 |
| Stages | any legal P+ stage (loser picks, as Slippi Direct) | random from the server `stages` list | strike/counterpick from the server list |
| Rules | P+ competitive default (stocks/timer TBD with the P+ community); pause allowed | fixed, no pause | fixed, no pause |
| Characters | all P+ roster except transformations | same | same, locked for the set as Slippi |

- **Client side:** the netplay codes GCT that ships with our build gates the Code Menu input handler and the hold-L/hold-B paths while an online session flag is set. Its hash goes into every ticket and report.
- **Server side:**
  - The mm server refuses tickets whose `buildHash`/`isoHash` is not on the allow-list (Rev 1 and Rev 2 MD5s from the project memory file, plus the current codeset hash).
  - Game reports carry `bannedStateFlags`: Code Menu opened, debug byte set, transformed fighter, scene left. The client's Rust reporter samples them from memory every frame.
  - Any flag or illegal stage/character voids the game in ranked and flags the account. The two reports are cross-checked, and replays allow audit.
  - The harness can drive exactly these probes against a test backend (phase tests).

---

## 7. Phased build plan

Rough effort is for one developer.

| Phase | Deliverable | Backend effort | Test approach |
|---|---|---|---|
| B0 | Repo skeleton (`backend/` Cargo workspace: `core`, `api`, `mm`, `relay`), Postgres migrations, docker-compose for dev, systemd units and Caddyfile for the box | 3-4 days | `cargo test`; compose up on Windows (Docker Desktop) or plain processes |
| B1 | **Direct connect by code**: accounts (invite-only), connect codes, `user.json` via the launcher fork, mm server direct mode with hole punching plus LAN rule | 2-3 weeks | Unit tests on pairing; protocol golden tests (openmelee's JSON fixtures); **ppharness**: two Dolphin instances with `user.json` for two test accounts → mm on localhost → netsim between peers. Linux CI: network namespaces + nftables masquerade for full-cone, port-restricted and symmetric NAT |
| B1.5 | Relay fallback | 1 week | Symmetric-NAT namespace pair must connect via the relay |
| B2 | Unranked queue, region buckets, match reports, replay upload | 2 weeks | Harness: N instance pairs queued concurrently; reports agree; replay stored |
| B3 | Ranked: OpenSkill, sets, placements, abandon/void rules, rank fetch for in-game display, rules enforcement checks | 3 weeks | Rating unit tests against openskill.js reference vectors; harness QA probes (section 6) must be rejected |
| B4 | Website (signup, profile, leaderboard), admin CLI and page, email, backups, monitoring | 2 weeks | Snapshot tests of pages; restore drill from restic |

**TODO:** client-side (Dolphin/game) effort per phase; it dominates and depends on top risk 1.

---

## 8. Risks (expanded)

1. **Session start model** (section 5). It blocks the in-game flow, not the backend.
2. **NAT:** mitigated by the relay. It costs bandwidth and adds one more service to run.
3. **Client-reported results:** mitigated by dual reports, replays, banned-state flags and a build hash allow-list. There is no real anti-cheat; this is the same trust model as Slippi.
4. **Single box:** an outage stops all matchmaking, though already-connected games survive. Mitigated by backups and a documented rebuild; a second region can come later.

## 9. Open questions for the user

1. Session start: may matchmaking end in a short synchronized reboot (about 10 s) instead of Slippi's instant CSS connect, if state transfer proves too slow?
2. Friends phase without email (invite codes, admin password resets): OK?
3. Domain name and product name for the hostnames and website?
4. Ruleset (stocks, timer, stage lists for unranked and ranked): adopt P+'s current competitive ruleset, or ask the P+ team?
5. Keep replays forever for ranked, and 90 days for the rest?
