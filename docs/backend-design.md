# Online backend design

_Status 2026-10-06: complete design. Line references were checked by three read-only sweeps: the launcher, Dolphin plus the Rust extensions, and Brawlback. Some section 1 refs still come from `research/04-slippi-reference-architecture.md` (HEAD commits listed there)._

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
1. **Session start model (5.1).** Brawlback Gen 2 rolls back the *whole machine* from `scBoot`, using Dolphin's netplay lobby. Slippi's UX starts the online session *mid-game* at the CSS. **Recommendation:** an Orca-style host→guest keyframe at connect (with our own delta transfer) for release, and a synchronized reboot as the Phase-1 interim. Do not use Slippi's gameplay-only approach, which Brawlback Gen 1 never got working.
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

Findings from the Dolphin sweep (Ishiiruka @ `60f7b63`; mainline line numbers are close):
- **No region, ping or latency field exists anywhere in the mm protocol.** The only message types are `create-ticket`, `create-ticket-resp` and `get-ticket-resp` (`SlippiMatchmaking.cpp:26-28`). The only ping in the code is the P2P ack round-trip (`SlippiNetplay.cpp:428-433`). Slippi must infer region server-side, presumably from the source IP; there are three leaderboards (NA, EU, Other; Upcomer). We do the same with an offline GeoIP DB (2.3).
- **The client waits indefinitely for a match.** `get-ticket-resp` is polled in 2000 ms windows with no overall cap (`:480-493`). Only an mm disconnect ends the wait ("Lost connection to the mm server"). The server owns ticket expiry, so our 10-minute ticket TTL must send `get-ticket-resp {error}` rather than silently drop.
- **Other client timeouts:**
  - mm connect: 20 × 500 ms ≈ 10 s (`:358-373`);
  - `create-ticket-resp`: 5000 ms (`:440-446`);
  - ISO-hash wait: about 5.5 s (`:289-300`).
- **The P2P connect window is 8 s total** (`SlippiNetplay.cpp:778-779, 922-937`). After it, 1v1 requeues with a **new ticket** (back to INITIALIZING, `SlippiMatchmaking.cpp:891-898`). The mm server should therefore avoid re-pairing the same two players immediately after a failed connect.
- **`search.connectCode` is a JSON array of byte values, not a string** (`std::vector<u8>`, `:422-424`). `players[].chatMessages` must have exactly 16 entries or the client uses defaults (`:548-560`). `items` is a u32 bitfield (`:645`).
- **Brawlback Gen 1's client** (`refs/brawlback-dolphin` `Source/Core/Core/Brawlback/Netplay/Matchmaking.cpp:493-511`):
  - sends only `uid` and `playKey` in `user`;
  - adds `search.game {id:"RSBE01", revision, type, name}`;
  - ignores `matchId`, `chatMessages`, `rank` and `items`;
  - pointed at `lylat.gg:43113` for both dev and prod (`Matchmaking.h:88-90`).

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

Content-addressed files under `/srv/replays/<yyyy>/<mm>/<sha256>.slp.gz` on the box's second disk. `uploadUrl` is an HMAC-signed, 10-minute PUT URL to the api service, which streams the body to disk with a size cap (10 MB). Keep everything for ranked; prune unranked after 90 days. Under session start B, a replay is the start keyframe plus the confirmed input log, which re-simulates exactly in the same build. The storage path does not care about the format, and the 10 MB cap fits an input log plus a delta keyframe. A full 26-30 MB keyframe would need the cap raised, or the replay could reference the boot instead.

### 2.6 Website

Server-rendered pages: `/signup`, `/login`, `/reset`, `/user/{code}` (code, display name, rank as **text only**, with no badge art), `/leaderboard`, `/admin`. The layout copies slippi.gg's information layout; no new flows.

### 2.7 Relay

Recommended. It is a stateless UDP forwarder: both peers send `RELAY_HELLO{token}` to `relay:43200`, then the relay forwards datagrams between the two bound source addresses, so ENet runs unchanged on top. The client tries direct and LAN for 3 s, then the relay. It is used only when the punch fails, so bandwidth stays small (rollback input traffic is about 10-20 kbit/s per player). Rate-limit it per token and expire it at match end. It is not visible to users (no new UI), so it complies with "no novel UI".

### 2.8 Admin tooling

A CLI in the same binary (`ppo-admin user ban|unban|rename|reset-code|invite create`, `match void`, `rating recompute --season`) plus a read-only `/admin` web page for flagged games. Every action goes into `audit_log`.

Scaling notes:
- **api:** stateless, so it scales by adding processes behind Caddy.
- **Postgres:** a single instance is ample to six figures of users. Add a read replica only for leaderboard queries, and recompute leaderboards every 2 minutes as Slippi does, not per request.
- **relay:** CPU-bound per packet, at about 20 kbit/s per relayed player.
- **replays:** disk. 2-5 MB per gzipped game, ranked only, means 1 TB holds hundreds of thousands of games.
- **Friends-scale budget:** all services together need under 1 vCPU and under 2 GB RAM. The smallest OVH dedicated box is already oversized; buy for disk and bandwidth, not CPU.

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
- Cross-checked against Dolphin's copy `R/user/src/rank_fetcher/rank.rs:29-112` (same values, written as `<=` upper bounds). Fewer than 5 updates means Unranked. Grandmaster is tested **before** Master. Each lower bound is exclusive with a 0.01 gap, so a rating landing in a gap such as 765.425 falls through to Unranked in Dolphin. Our implementation should use one table with half-open intervals in all three places.

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

Extra path prefixes for this section:
- `BA` = `refs/brawlback-asm`
- `BD` = `refs/brawlback-dolphin` @ `savestates-efficiency-v2` (`55b817d`)
- `G2` = `refs/Project-Plus-Dolphin-brawlback` @ `rollback` (`b7d3413`), the base of our `dolphin/` fork
- `ORCA` = `refs/orca-netplay`

`BA` refs are on branch `origin/master` unless another branch is named.

### 5.1 How the online session starts (top risk 1)

**The problem.** Our fork's rollback is Brawlback Gen 2. It snapshots and restores the **whole emulated machine**:
- Changes are tracked at 64-byte granularity across MEM1/MEM2, plus Dolphin's non-RAM device state (research 01 §2.3).
- It runs from `scBoot` onward, menus included (`G2 Source/Core/Core/HLE/HLE_Misc.cpp:147-156`).
- Both machines boot together through Dolphin's netplay lobby. The host's `NetPlayServer` relays GekkoNet packets (research 01 §2.2).

Whole-machine rollback is only correct if both machines are **bit-identical when the session starts**. That holds for a synchronized netplay boot. It does not hold for Slippi's UX, where each player boots alone, plays with menus, searches from their own character select and connects there. By then the two machines have different heaps, RNG state, timestamps, loaded resources, name tags and saves. Our own measurement already shows menu history changing heap contents: StockResource was filled on one run and empty on the other at the same CSS frame (`docs/determinism-findings.md`, cause 3).

**What the others do**

| Project | Session start | Evidence |
|---|---|---|
| **Slippi** | **Gameplay-only rollback started at match start.** Each Dolphin keeps its own menus. At the CSS the players exchange selections and an RNG offset over ENet (`NP_MSG_SLIPPI_MATCH_SELECTIONS` 0x82, `I/Slippi/SlippiNetplay.cpp:550-559, 620-670`). Each game then builds the same match block (`prepareOnlineMatchState`, `I/HW/EXI_DeviceSlippi.cpp:2108-2744`) and starts the match. Savestates cover only fixed Melee RAM ranges plus the main heap (`I/Slippi/SlippiSavestate.cpp:50-119`). | It works for Melee because Melee's match init rebuilds gameplay state from the match block, and the engine is small. Desyncs still happen and ranked recovers from them (research 04 §2.8). |
| **Brawlback Gen 1** (2021 to mid-2025) | **Same as Slippi.** On entering the online "training room", `setNextAnyOkirakuCaseFive` sends `CMD_START_MATCH` with its `GameSettings` and then `CMD_FIND_OPPONENT` (`BA Brawlback-Online/source/Rollback_Hooks.cpp:1246-1262`). Dolphin merges both sides' `GameSettings` (characters, stage, the host's seed; `BD Source/Core/Core/HW/EXI/EXIBrawlback.cpp:722-805`), and the game boots `scMelee` itself (`BootToScMelee`, `Rollback_Hooks.cpp:1221`). | **It never reached playable** in four years (wiki: "BRAWLBACK IS CURRENTLY NOT PLAYABLE"). Brawl's gameplay state lives in 13+ heaps; Brawlback went from heap allow-lists to dirty pages to page deltas (research 01 §0). |
| **Brawlback Gen 2** (Dec 2025 to now) | **Synchronized boot.** Dolphin's netplay lobby boots both machines together, and rollback covers boot, menus and match. | It has no matchmaking or in-game online menus (research 01 §2.6). It works in our harness today (`two_player_netplay`, `harness/README.md`). |
| **Orca** | **Host-to-guest whole-machine keyframe ("drop-in").** The host plays solo with no rollback. When a friend or a matched stranger arrives, the host captures the machine at a frame boundary (8-10 ms) plus the session NAND, then compresses (zstd), encrypts and uploads it. The guest boots the same disc to `BootNandFrame` (`ORCA Data/Sys/Orca/PPLUS32.ini:25` = 600, `RSBE01.ini:13` = 1800), downloads, loads, and replays the host's logged pads to catch up. Both machines then plug port 2 in at an agreed frame (`ORCA ORCA.md` "Drop-in" §1-5; `Source/Core/Core/Orca/Session/Keyframe.h:17-19`). Queue matches use the same drop-in into the host's character select (`ORCA.md` "Matchmaking and results" §3-4). | Keyframes are **26-30 MB**. Over the internet a join took **5.7-7.2 s** (about 1.5 s with a local store). NAND files the boot writes anyway travel as XXH3 hashes, which halved the upload. Keyframes go through YouGame's HTTP store. Each port's name tag and controls travel as "port values" (`ORCA Source/Core/Core/Orca/UX/NameTags.h`). |

**Options for us**

**(A) Synchronized reboot of both games after matchmaking.** On a match, both Dolphins stop emulation and start a Dolphin netplay session, with `isHost` as the server. Then they boot P+ together into a rollback session.

Pros:
- No new rollback mechanics. This is exactly what our fork and harness run today.
- Determinism is the easiest of the three. The boot is identical, the netplay RTC uses emulated ticks (`docs/determinism-findings.md`, cause 1), and nothing is transferred.
- The CSS, SSS and ranked strike/counterpick become ordinary shared game state, driven by both players' inputs. No selection-exchange protocol is needed.

Cons:
- **Not Slippi's UX.** The screen goes black for a 10-15 s boot: P+ reaches the CSS at about VI field 640 with no input (`docs/brawl-memory-map.md:246`), plus Dolphin teardown and shader warm-up.
- The pre-search character pick must be re-applied after the boot.
- Dolphin netplay syncs the host's save, so the guest loses their own name tags and controls unless they are carried as port values (Orca's approach).
- Each requeue needs another reboot.
- Dolphin's `NetPlayClient` binds a random port, so it must learn to bind the hole-punched port. That change is small.

**(B) Host-to-guest state transfer at connect time (keyframe).** Both players play locally until they are matched. The host captures its machine at its next frame boundary after `get-ticket-resp` and the P2P connect. It sends the capture to the guest over the already hole-punched ENet link, or over our relay. The guest loads it and catches up through the host's logged pads. Port 2 is plugged in at an agreed frame S, carrying the guest's locked-in character/costume, name and controls as port values.

Pros:
- **Slippi's UX:** you search from your own CSS, and the opponent appears on it a few seconds later.
- Keeps whole-machine rollback, so there is no list of gameplay regions to get right.
- Rematches stay inside one session.
- Orca has proven the approach on P+ v3.2, with the same Brawl executable and numbers we can plan around. Orca is GPL-2.0-or-later, so its code is license-compatible.

Cons:
- **Transfer size.** At Orca's 26-30 MB, a 10 Mbit/s home uplink needs about 25 s; at 50 Mbit/s it is about 5 s.
- Our snapshots skip IOS HLE state (`G2 Source/Core/Core/HW/HW.cpp:117`, research 01 §2.4). A keyframe needs the full `DoState`, including IOS/SD file handles (P+ streams its files from the virtual SD), plus the save/NAND.
- Orca saw P+'s first frames after its launcher run differently on a machine whose JIT holds other blocks. It therefore refuses to keyframe during the first 300 frames.
- The guest's own machine is replaced. The guest sees the host's CSS and plays as port 2, not "you on the left" as in Slippi.
- Taking Orca code verbatim is a community-relations question (memory: "do not start from Orca"). Reimplementing on Brawlback's `RollbackManager` is cleaner.

**Size mitigation (to measure): an rsync-style delta keyframe.** Both machines run the same build and scene with the same archives loaded, so most of the 88 MB should already match.
- The guest sends 64 KiB block hashes of its own MEM1/MEM2 (1,408 × 8 B = 11 KiB).
- The host sends only the blocks that differ, zstd-compressed.
- Device state follows in full, and NAND files go as references by hash (Orca's `BootNandFrame` trick).

**(C) Gameplay-only rollback started at the CSS or match start (Slippi / Brawlback Gen 1).** Each machine keeps its own menus. Selections, seed and rules are exchanged, both enter `scMelee` with the same parameters, and the rollback session starts there.

Pros:
- Exactly Slippi's UX: no transfer and no reboot.
- Each player keeps their own save, tags and controls.

Cons:
- **Determinism risk.** It is the approach Brawlback spent four years on without a playable result.
  - Brawl's match init does not rebuild all gameplay state from a small block. Heap layout and contents depend on menu history (our StockResource finding).
  - P+ codes keep their own state, which affects gameplay and would need force-syncing. The Code Menu block alone is 0x2520 bytes at `0x804E0000` (`docs/brawl-memory-map.md`).
- It gives up Gen 2's main advantage, that nobody has to enumerate gameplay state.
- Desync detection must compare player state only, because whole-RAM hashes would always differ.

**Recommendation**
- **Target (B), keyframe at connect, for release.**
  - The guest's pre-search lock-in travels as port values.
  - The transfer runs over our own P2P link or relay, using rsync-style deltas.
- **Use (A), synchronized reboot, as the interim for Phase 1** (two friends, direct connect by code).
  - It needs no new rollback mechanism, so Phase 1 can ship while the keyframe work proceeds.
  - The in-game menus up to "match found" are identical under A and B, so no menu work is thrown away.
- **Reject (C).**
- **Go/no-go test for B:** in the harness, drop a guest into a host's CSS from a keyframe, with dual core on both machines.
  - Pass if confirmed-frame state hashes stay equal for 10 minutes across 20 runs, and the median delta keyframe on a P+ CSS is under 8 MB.
  - If B fails, ship A permanently and tell the user the UX differs from Slippi by one reboot.

The backend contract in section 2 is identical for A and B: it ends at `get-ticket-resp`. This choice does not block backend work.

### 5.2 The channel between game and Dolphin

Gen 2 has **no EXI device**. It was deleted in `10d50e5d2`, and `G2 Source/Core/Core/Config/MainSettings.cpp:136-137` sets Slot B to `None`. Gen 2 uses HLE hooks at the main-loop boundaries instead (`G2 HLE/HLE.cpp:125-141`):
- `BrawlbackGekkoNetUnconditionalFrame` at `0x800171b4`
- `BrawlbackGekkoNetFrameEnd` at `0x80017504`
- `BrawlbackGekkoNetLoopEnd` at `0x80017508`

Under whole-machine rollback, any game-to-Dolphin channel must obey two hard rules:
1. Nothing that differs between the two machines, or between a first run and a re-run of a frame, may flow into simulated state.
2. Side effects (search, report) must not fire again when a frame is re-simulated.

EXI makes both rules easy to break, because every DMA read is an input to the simulation. **Proposal: three memory blocks, serviced only at the `FrameEnd` boundary hook.** Slippi's command numbers are kept for familiarity.

| Block | Who writes | In rollback state / desync hash? | Used when |
|---|---|---|---|
| **MAILBOX** (request ring + response area, about 1 KiB) | The game writes requests; Dolphin writes responses at the boundary | **Excluded** | Only while **no session** is running, i.e. the game is alone: online menu, code entry, searching. The plugin must not read it during a session. |
| **SESSION** (about 4 KiB) | Dolphin, on both machines, at an agreed frame S announced in the session handshake (Orca's `RequireValues` pattern) | **Included** | During a session: opponent names and codes, mode, matchId, ruleset and server stage list, visible ranks, chat message sets, per-port values (name, controls, the guest's lock-in) |
| **LOCAL** (about 512 B) | Dolphin, per machine | **Excluded** | Draw-only data that legitimately differs per player: your rating change after a set, your own "Searching…" state, errors, ping. The plugin may only render it from preallocated buffers: no branching of game logic, no heap allocation. |

The plugin finds the blocks through a magic header (`"PPOM"` plus a version), which the HLE boundary hook scans for once after boot. The block addresses come from P+'s free space and are excluded through the same list as `STATE_EXCLUDED_RANGES`.

During a session the game sends Dolphin **nothing** through the mailbox. Everything the game does is driven by input:
- **Chat and "hold Z to disconnect"** become one extra byte per player per frame in the GekkoNet input (`input_size = sizeof(gfPadStatus)+1`, `G2 NetPlayClient.cpp:1258-1316`). They are then confirmed and replayed exactly like pads.
- **Results, set state, banned-state flags and reports** are read by Dolphin from **confirmed frames only**. This is Orca's `Results` reader pattern, using the addresses in `ORCA.md` "Reading results" (`g_GameGlobal 0x805A00E0` and related; it is the same executable as P+). Each report therefore fires exactly once, after its frame can no longer roll back.

### 5.3 Command mapping

Slippi command bytes come from `I/HW/EXI_DeviceSlippi.h:45-111`. Brawlback Gen-1 bytes come from `BA Brawlback-Online/include/exi_packet.h` (master; `savestate-efficiency` adds 35-39) and are handled in `BD EXIBrawlback.cpp:1433-1510`. Brawlback's `project-plus-fork` branch uses a different, rollback-only protocol (1-18: `END_FRAME`, `GET_REMOTE_INPUTS`, `EXECUTE_SAVE/LOAD/ADVANCE`, …). It has no matchmaking commands and no Dolphin handler anywhere.

| Slippi | Brawlback Gen 1 | Ours (block, phase) | Notes |
|---|---|---|---|
| 0xB0 ONLINE_INPUTS, 0xB1/0xB2 savestate, 0xD5 GET_DELAY | 1 ONLINE_INPUTS, 2/3 CAPTURE/LOAD_SAVESTATE, 15 FRAMEDATA, 16 TIMESYNC, 17 ROLLBACK, 18 FRAMEADVANCE | **none** | Gen 2 does rollback inside Dolphin (HLE + GekkoNet). Input delay is a Dolphin setting. |
| 0xB9 GET_ONLINE_STATUS → `{appState 0/1/2, name[31], code[10]}` (`EXI_DeviceSlippi.cpp:3026-3052`) | 10 GET_ONLINE_STATUS (declared, never handled) | **0xB9 MAILBOX** → `{state: 0 logged out / 1 ok / 2 update required, name UTF-16[16], code UTF-16[9]}` | Brawl's text is UTF-16, not Shift-JIS |
| 0xB6 OPEN_LOGIN, 0xB7 LOGOUT, 0xB8 UPDATE | 9 UPDATE (declared) | **0xB6, 0xB8 MAILBOX**. Dolphin opens the launcher. Logout stays disabled in-game, as on Slippi ("use the launcher"). | |
| 0xB4 FIND_OPPONENT (mode u8 + Shift-JIS code[18], `:1916`) | 5 FIND_OPPONENT (sent with no payload; Dolphin forces UNRANKED, `BD EXIBrawlback.cpp:1114-1122`) + 13 START_MATCH (`GameSettings`) | **0xB4 MAILBOX** `{mode u8, code UTF-16[9], lockedChar u8, costume u8, team u8}` | The lock-in becomes the guest's port values (5.1 B) |
| 0xB3 GET_MATCH_STATE (962 bytes; state byte = `ProcessState` 0-5, `SlippiMatchmaking.h:34-42`) | Polls a DMA read for 14 SETUP_PLAYERS + `GameSettings` (`BA Rollback_Hooks.cpp:1043-1069`) | **0xB3 MAILBOX** `{mmState u8 (same 0-5 values), errorText UTF-16[120], peerName UTF-16[16], peerCode UTF-16[9], role host/guest, sessionPhase: none / connecting / transferring % / plugged}` | Valid only until plug-in; after that the SESSION block is authoritative |
| 0xBA CLEANUP_CONNECTION | 11 CLEANUP_CONNECTION (declared), 34 CANCEL_MATCHMAKING (`BD :1385-1392`) | **0xBA MAILBOX** (cancel search / leave). Inside a session, leaving is the "leave" flag in the input byte. | |
| 0xBE FETCH_CODE_SUGGESTION (31 B, `:2012-2105`) | none (no code-entry UI exists in any Brawlback branch) | **0xBE MAILBOX** `{prefix UTF-16[9], index i32, scroll s8, mode u8}` → suggestion | `direct-codes.json` / `teams-codes.json` reused unchanged |
| 0xE3 GET_RANK / 0xE4 FETCH_RANK (15-byte reply, `:3351-3373`) | none | **0xE3/0xE4 MAILBOX** before the search; **SESSION** for ranks both players see; **LOCAL** for your own post-set change | |
| 0xB5 SET_MATCH_SELECTIONS, 0x82 MATCH_SELECTIONS, 0xBC GET_NEW_SEED | 6 SET_MATCH_SELECTIONS (declared), 12 GET_NEW_SEED (declared), P2P `CMD_GAME_SETTINGS` merge | **none** | The CSS/SSS are shared state driven by inputs. RNG is shared state, seeded by the session RTC (`G2 HLE_Misc.cpp:607-621`). |
| 0xBB SEND_CHAT_MESSAGE, 0xC3 GET_PLAYER_SETTINGS | none | **Input byte** (chat id) + **SESSION** (each player's 16 message strings) | |
| 0xBF OVERWRITE_SELECTIONS, 0xC0/0xC1 GP_COMPLETE/FETCH_STEP, 0x85 COMPLETE_STEP | none | **none**: strike/counterpick runs as game logic on shared state, with the server's stage list in SESSION | |
| 0xBD REPORT_GAME (368 B), 0xC2 REPORT_SET_COMPLETE, 0xC4 REPORT_MATCH_STATUS_UPDATE | 4 MATCH_END (`GameReport` → POST `https://lylat.gg/reports`, `BD :1160-1215`; disabled on the game side with `#if 0`) | **none from the game.** Dolphin's confirmed-frame reader sends `reportOnlineGame`, `reportOnlineMatchStatus`, set completion and `bannedStateFlags`. | Fires once per confirmed event, never on re-sim |
| 0x86 SYNCED_STATE (ranked desync recovery) | none | Later: a recovery keyframe from the host. With option B, recovery becomes a reload instead of a reconciliation. | |
| 0x35-0x3C replay recording | 19-26 replay commands | **none**: Dolphin records the replay as the start keyframe (or the boot) plus the confirmed input log | Replay format still open (2.5) |
| 0xD1/0xD2 file load, 0xD3/0xD4 GCT | 30-33 allocs/dump (Gen-1 heap tracking) | none | |

**Dolphin-side matchmaking client.** Port `I/Slippi/SlippiMatchmaking.cpp`, as Brawlback already did in `BD Source/Core/Core/Brawlback/Netplay/Matchmaking.cpp`.
- Keep Slippi's wire format, but **add back** what Brawlback dropped: `user.connectCode` and `displayName`. Brawlback sends only uid and playKey (`:495`).
- Keep Brawlback's useful `search.game {id, revision, type, name}` block (`:496-506`) and add our `buildHash`.
- Hand the punched local port to Dolphin's `NetPlayServer` (host) or `NetPlayClient` (guest).
- Have the host send punch datagrams to the guest's external address while the guest connects. Slippi gets the same effect by connecting from both sides (`I/Slippi/SlippiNetplay.cpp:125-152`).
- Fall back to the relay after 3 s.

### 5.4 Menu screens to build

**Starting point.** Brawlback Gen 1 hijacked Brawl's own Wi-Fi flow (all hooks installed at `BA Rollback_Hooks.cpp:1539-1584`):
- `SkipDirectlyToCSS` at `sora_menu_main+0x2E4F8` sends the main-menu Wi-Fi path straight to the Wi-Fi character select.
- Other hooks fake the Nintendo WFC login, Mii rendering, friend code and connection: `setToLoggedIn` `0x8014B5F8`, `disableMiiRender` `0x80033b48`, `forceFriendCode` `0x8014b4bc`, `forceConnection` `0x8014b3b8`.
- The CSS/SSS countdowns and network-error dialogs are removed (`sora_menu_sel_char+0x4220/0x56A8/0x53A4`, `sora_menu_sel_stage+0x141C/0x30F0`), and so is the disconnect panel (`sel_char+0x4A70`).
- The game waits for a match in Brawl's online training room, with status text printed on its message bar through `MuMsg::printf` (`ReplaceTrainingRoomText` `0x800fd49c`).
- Exiting returns to the Direct/Quickplay screen (`sora_scene+0x3770C`).

**No code anywhere relabels the main-menu Wi-Fi button or adds code entry** (Brawlback sweep, all branches). The "Brawlback → Direct / Quickplay" menu in our memory notes is not in any public repo, and research 06 §5 is still pending.

The table maps each Slippi screen to the Brawl/P+ screen we reuse, using existing assets only.

| # | Slippi screen (source) | Our screen | Assets and work |
|---|---|---|---|
| 1 | Online submenu: Ranked, Unranked, Direct, Teams (+ Log In / Update when locked), a description line, name + code shown (`ASM/Online/Online.s:70-83`, `HandleOnlineLockedOptions.asm:27-79`) | Brawl's **Wi-Fi top menu**, reached from the main-menu Wi-Fi button (vanilla: With Friends / With Anyone / Spectator / Options; layout to be verified). Buttons relabelled RANKED / UNRANKED / DIRECT / TEAMS through the menu's own message strings. The account line and lock reasons go in the menu's description text. | Existing button frames and font; new strings only. Locked items use the menu's own disabled state if it has one, otherwise a refusal message as on Slippi. |
| 2 | CSS with a 3-line status panel, "Press START to lock in / search", spinner, "Hold Z to disconnect" (`LoadCSSText.asm:93-160, 509-905`) | Brawl's **Wi-Fi CSS** (`sora_menu_sel_char` in net mode) with Gen 1's timer and error hooks. Status text goes in the CSS's existing message window via `MuMsg`, using Slippi's strings verbatim. | No spinner art: use text dots, or the CSS's own "waiting" animation if one exists. **Wait on the CSS as Slippi does, not in Brawl's training room as Gen 1 did.** |
| 3 | Connect-code entry: Melee name-tag keyboard in code mode, 8 chars, full-width `＃`, L/R history scroll, Z to accept (`TextEntryScreen/*`, `Allow8Characters.asm`) | Brawl's **name-entry keyboard** (the Names screen used for tags), restricted to A-Z / 0-9 / `#` | Raise Brawl's tag-length limit to 8, as Slippi raised Melee's. History comes through mailbox 0xBE. Direct and Teams share the screen. |
| 4 | Opponent found: name on the CSS, both press START, VS splash | The CSS shows the opponent's panel (shared state after plug-in) and their name from SESSION | Brawl's Vs mode has no VS splash, so go to Brawl's normal loading screen. No new screen. |
| 5 | Stage choice: Unranked random, Direct loser picks, Teams P1 picks (research 04 §4.6) | Unranked: skip the SSS (random from the server list). Direct/Teams: P+'s **SSS**, restricted to the server list. | Disallowed stages use the SSS's existing unavailable/hidden state |
| 6 | Ranked GameSetup: strikes 1-2-1, then winner bans and loser picks; step timers 30/30/10 s for strikes, 30 s ban/pick, 45 s character (`C/Scenes/Ranked/GameSetup.c`, `.h:16-19`) | P+ **SSS in a strike mode**: struck stages greyed, turn text and timer in the SSS message area. Character re-pick happens on the CSS in Slippi's order. | The largest game-side piece. Text plus existing stage icons only; no Slippi rank badges or other art. |
| 7 | Rank on the CSS: badge + "GOLD 2 1653.5", animated rating change after a set (`C/Scenes/CSS/RankInfo/RankInfo.c`) | A text-only rank line in the CSS info pane, with the rating change from LOCAL | Brawl has no badge art, so text only. Tier names are generic. |
| 8 | Results → back to the CSS still connected; START to rematch | Brawl's results screen, then back to the Wi-Fi CSS in the same session | Hook the results→CSS transition. Exiting goes back to screen 1 (Gen 1's `ExitWifiCSS…` hook). |
| 9 | "DISCONNECTED" (red), "DESYNC DETECTED", poor-connection OSD | `MuMsg` text in red, in the CSS and in-game message windows | Existing font |
| 10 | Quick chat: D-pad category window, 16 messages (`C/Scenes/CSS/Chat/Chat.c`) | A text-only window in the CSS message pane; in-match notifications as text | Later phase |

All new visible content is text in Brawl/P+'s own fonts, placed in existing panes. **No new images, and no AI-generated graphics.** Where Slippi shows art we don't have (rank badges, the chat window frame), we show text instead.

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
  - Game reports carry `bannedStateFlags`: Code Menu opened, debug byte set, transformed fighter, scene left. Dolphin samples them from **confirmed frames** (5.2), so a rolled-back frame never raises a flag.
  - Any flag or illegal stage/character voids the game in ranked and flags the account. The two reports are cross-checked, and replays allow audit.
  - The harness can drive exactly these probes against a test backend (phase tests).

---

## 7. Phased build plan

Effort is in developer-weeks for one experienced developer. **Backend** is this document's services. **Dolphin** is C++/Rust in our fork. **Game** is the Syriinge plugin plus the netplay GCT. **Launcher** is the fork of slippi-launcher. The Dolphin and game columns assume the rollback core (another workstream) already plays a stable 1v1 in a synchronized-boot session.

| Phase | Deliverable | Backend | Dolphin | Game | Launcher | Test approach |
|---|---|---|---|---|---|---|
| **P0** Skeleton | `backend/` Cargo workspace (`core`, `api`, `mm`, `relay`), Postgres migrations, docker-compose for dev, systemd units and Caddyfile | 0.5-1 | — | — | — | `cargo test`; `docker compose up` on Windows (Docker Desktop) or plain processes |
| **P1** Two friends, direct connect by code, with rollback (session start **A**) | Accounts (invite-only), connect codes, mm direct mode with hole punching and the LAN rule. Dolphin: `user` crate from slippi-rust-extensions (user.json watcher), port `SlippiMatchmaking.cpp`, the mailbox (5.2), match-found → netplay session on the punched port (5.3), banned-state blocks (section 6). Game: Wi-Fi menu repurposed (screen 1), code entry (3), CSS status (2, 4, 9), rules lock (section 6). Launcher: rebrand, login/sign-up against our API, `user.json`, ISO and SD/codeset check. | 2-3 | 4-5 | 6-8 | 2-3 | Unit tests for pairing and code assignment. Protocol golden tests using openmelee's JSON fixtures. **ppharness:** mm and api run as plain local processes; two DolphinNoGUI instances each get their own test `user.json` in the instance user dir, search for each other by code through the menus (scripted pads), and their P2P traffic goes through `netsim` presets. NAT tests on Linux CI use network namespaces + nftables masquerade for full-cone, port-restricted and symmetric NAT. `qa_reachability.py` rerun online must find nothing. |
| **P1.5** Relay fallback | Relay service; client falls back after 3 s | 1 | 1 | — | — | A symmetric-NAT namespace pair must connect through the relay |
| **P2** Keyframe session start (**B**) | Host→guest delta keyframe over P2P/relay, catch-up, port values (name, controls, lock-in), full `DoState` including IOS/SD | — | 5-7 | 1 | — | Go/no-go test in 5.1: 20 drop-in runs, dual core, confirmed-frame hashes equal for 10 min, keyframe size and join time measured under `typical`/`bad_wifi` |
| **P3** Unranked matchmaking | Unranked queue with region buckets; confirmed-frame result reader; match reports; replay = keyframe + input log, uploaded | 2 | 3 | 1-2 (random stage, unranked rules) | 0.5 | Harness runs N instance pairs queued at once: every pair matched exactly once, both reports agree, replays stored and re-simulated to the same result |
| **P4** Ranked | OpenSkill, sets, placements, abandon/void rules, rank fetch. Game: SSS strike/counterpick (screen 6), CSS rank text (7). Dolphin: set tracking, poor-performance termination, rank via mailbox/SESSION/LOCAL | 3 | 2-3 | 6-8 | 1 | Rating unit tests against openskill.js reference vectors. Harness scripts a full Bo3 with strikes on both instances. QA probes and forged reports (disagreeing winners, banned flags) must void the game. |
| **P5** Website, leaderboard, ops | Sign-up/profile/leaderboard pages, admin CLI and page, email, restic backups, monitoring | 2 | — | — | 0.5 (links) | Page snapshot tests, a restore drill from restic, an external uptime probe |
| **Later** | Quick chat (screen 10), teams 2v2 (needs rollback core N-player), spectating/broadcast relay | 1-2 | 3-4 | 3-4 | 1-2 | — |

**Totals to ranked (P0-P4):**

| Area | Weeks |
|---|---|
| Backend | about 9-11 |
| Dolphin | about 15-19 |
| Game | about 14-19 |
| Launcher | about 4 |

That is roughly **10-12 months for one person**, or 5-6 months with one person on backend and launcher and another on Dolphin and game. The game-side menus are the least certain estimate: they need Brawl menu reverse engineering beyond what Gen 1 left.

---

## 8. Risks (expanded)

1. **How the online session starts (5.1).**
   - Whole-machine rollback needs identical machines at session start, and Slippi's UX connects players mid-game.
   - Recommendation: option B (Orca-style keyframe at connect, with our own delta transfer), with option A (synchronized reboot) as the Phase 1 interim. Reject C (Slippi/Brawlback-Gen-1 gameplay-only rollback).
   - Residual risks for B:
     - transfer time on slow uplinks;
     - IOS/SD state in the keyframe;
     - P+'s JIT-dependent first frames;
     - dual-core determinism after a load. Our dual-core findings show render-side G3D state diverging, which makes a whole-RAM hash noisy.
   - Fallback: ship A permanently, which costs one visible reboot per connection.
2. **NAT.** Mitigated by the relay. It costs bandwidth and one more service to run.
3. **Client-reported results.** Mitigated by:
   - confirmed-frame reporting from both clients;
   - dual-report agreement;
   - replays;
   - banned-state flags;
   - the build-hash allow-list.

   There is no real anti-cheat; this is the same trust model as Slippi's.
4. **Single box.** An outage stops all matchmaking, though games already connected survive. Mitigated by backups and a documented rebuild; a second region can come later.
5. **Menu reverse engineering.** Gen 1's hooks cover the Wi-Fi CSS path only. The Wi-Fi top menu, the name-entry keyboard in code mode, and the SSS strike mode are new Brawl RE work.

## 9. Open questions for the user

1. **Session start.** Agree with option B as the target and A for Phase 1? If B's transfer is slow on someone's connection, is a visible reboot (A) acceptable for them?
2. **Orca code.** May we reuse Orca's GPL keyframe and result-reader code with attribution, or reimplement it? (Reuse is legal; reimplementing avoids "built on Orca" optics.)
3. **The "Brawlback → Direct / Quickplay" Wi-Fi menu.** Where did you see it? It is in no public Brawlback repo or branch. If it exists unpushed, asking the Brawlback team for it would save weeks.
4. **Email.** Is a friends phase without email (invite codes, admin password resets) OK?
5. **Names.** What domain name and product name should the hostnames and website use?
6. **Ruleset.** Stocks, timer, and stage lists for unranked and ranked: adopt P+'s current competitive ruleset, or ask the P+ team?
7. **Replay retention.** Keep ranked replays forever and the rest for 90 days?
