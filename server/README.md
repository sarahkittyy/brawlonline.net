# Brawl Online backend

Accounts, connect codes, and Direct and Unranked matchmaking for Brawl Online, the Project+ rollback client. Phase 1 goal: two friends log in, get connect codes and connect to each other by code, the way Slippi's Direct mode works. Phase 3 (started): strangers meet through Slippi's Unranked queue. Design: `docs/backend-design.md` (sections 1-4 and 7).

| Crate | What it is |
|---|---|
| `crates/accounts` | HTTP service (axum + sqlx): sign-up with invite code and email verification, login, sessions, password reset, connect codes, play keys, the data for `user.json`, Slippi's users-rest endpoint. |
| `crates/mm` | Matchmaking server: ENet over UDP 43113, speaking Slippi's `create-ticket` / `get-ticket-resp` JSON protocol. Direct and Unranked; per-mode stage lists. |
| `crates/admin` | Admin CLI: invites, users, password resets, bans. |
| `crates/mmclient` | Ticket client library and CLI that behaves like Slippi's `SlippiMatchmaking.cpp`. For tests, ppharness and game-integration work. |
| `crates/common` | Shared code: connect-code and display-name rules, the mm message types, play keys, rate limiter, migrations. |
| `crates/e2e` | End-to-end tests (Postgres + both services + fake game clients). |
| `migrations/` | Postgres schema (applied by `accounts` on start, or `admin migrate`). |
| `config/rulesets.json` | Per-mode rules: the stage list sent in `get-ticket-resp` (P+'s legal stages), items, stocks/timer. See "Rulesets and stage lists". |
| `deploy/` | systemd units, Caddyfile, env examples, backup notes for the OVH box. |
| `tools/c-enet-interop/` | Manual check against Slippi's own C ENet. |

Parts of `common` are derived from [openmelee](https://github.com/panchaea/openmelee) (GPL-2.0): the message shapes and test fixtures, and the Shift-JIS + NFKC decoding of codes. The workspace is GPL-2.0-or-later.

## Running locally

Needs Rust 1.85+ and Docker (for Postgres only; the services run as plain processes).

### Windows (PowerShell)

```powershell
cd D:\code\pm_rollback\server
docker compose up -d --wait                  # Postgres 16 on 127.0.0.1:54329
Copy-Item .env.example .env
cargo run -p admin -- gen-secret             # paste into PLAY_KEY_SECRET in .env
cargo build --workspace
# two terminals:
.\target\debug\accounts.exe                  # http://127.0.0.1:8080, runs migrations
.\target\debug\mm.exe                        # udp 0.0.0.0:43113
```

### Linux / macOS / Steam Deck

```sh
cd server
docker compose up -d --wait
cp .env.example .env
sed -i "s/^PLAY_KEY_SECRET=.*/PLAY_KEY_SECRET=$(cargo run -q -p admin -- gen-secret)/" .env
cargo build --workspace
./target/debug/accounts &
./target/debug/mm &
```

Without Docker, point `DATABASE_URL` at any Postgres 14+ database you own.

**Portable Postgres (Windows, no install, no Docker).** When Docker Desktop is broken, the EDB "binaries" zip runs from any folder without touching the system or WSL. The ppharness online tests use it automatically when it is at `run/postgres-portable/pgsql`:

```powershell
# once: https://get.enterprisedb.com/postgresql/postgresql-16.10-1-windows-x64-binaries.zip
# extracted to D:\code\pm_rollback\run\postgres-portable (pgAdmin, StackBuilder and doc can be deleted)
$pg = "D:\code\pm_rollback\run\postgres-portable\pgsql\bin"
"pp-dev-password" | Out-File -Encoding ascii pw.txt
& $pg\initdb -D pgdata -U pp -A scram-sha-256 --pwfile=pw.txt -E UTF8 --no-locale; Remove-Item pw.txt
& $pg\pg_ctl -D pgdata -l pg.log -o "-p 54329 -h 127.0.0.1" -w start   # same URL as the compose service
# ... cargo test --workspace / the services ...
& $pg\pg_ctl -D pgdata -m fast -w stop
```

With another port, set `TEST_DATABASE_URL=postgres://pp:pp-dev-password@127.0.0.1:<port>/postgres` for `cargo test` (the e2e tests create and drop their own databases).

Every setting is an environment variable (or a flag; `--help` lists them). The binaries load `.env` from the current directory. With `MAILER=stdout` (the default when `RESEND_API_KEY` is empty) emails are printed instead of sent, so the verification link appears in the accounts terminal. `MAILER=file` appends them to `MAIL_FILE` as JSON lines.

### Try it end to end

```sh
# 1. An invite (sign-up is invite-only while SIGNUP_INVITE_ONLY=true)
cargo run -p admin -- invite create --note "alice"

# 2. Sign up; the verification link is printed by accounts (MAILER=stdout)
curl -s localhost:8080/v1/auth/signup -H 'content-type: application/json' \
  -d '{"email":"alice@example.test","password":"a long password","displayName":"alice","inviteCode":"XXXX-XXXX-XXXX-XXXX"}'
#    -> {"sessionToken": "...", "user": {...}}
curl -s "localhost:8080/verify-email?token=..."

# 3. Pick the code prefix; the server appends #N
curl -s localhost:8080/v1/me/netplay -H "authorization: Bearer $SESSION" \
  -H 'content-type: application/json' -d '{"codeStart":"alic"}'

# 4. Write user.json exactly as the launcher would
curl -s localhost:8080/v1/me/user-json -H "authorization: Bearer $SESSION" > alice.user.json

# 5. Do the same for bob, then search for each other
cargo run -p mmclient -- search --user-json alice.user.json --code BOB#123 --punch &
cargo run -p mmclient -- search --user-json bob.user.json --code ALIC#456 --punch
```

Both print a JSON result with `"status":"matched"`, the full `get-ticket-resp`, the peer address chosen with Slippi's LAN rule, and (with `--punch`) whether a P2P ENet connection from the same port succeeded.

## Accounts API

JSON in and out. Errors are `{"error": {"code": "...", "message": "..."}}`. Authenticated endpoints take `Authorization: Bearer <sessionToken>`. Names mirror the Slippi launcher's GraphQL operations.

| Endpoint | Slippi equivalent | Notes |
|---|---|---|
| `POST /v1/auth/signup {email, password, displayName, inviteCode?}` | `createUserNew` | 201 with a session. Sends a verification email. Invite required while `SIGNUP_INVITE_ONLY=true`. |
| `POST /v1/auth/login {email, password}` | `signInWithEmailAndPassword` | `{sessionToken, user}`. Sessions slide for 90 days. |
| `POST /v1/auth/logout` | | |
| `POST /v1/auth/verify-email {token}`, `GET /verify-email?token=` | Firebase verification | The GET is the page the email links to. |
| `POST /v1/auth/verify-email/resend` | | Rate-limited. |
| `POST /v1/auth/password-reset/request {email}` | Firebase reset | Always 202. Emails a 1-hour link. |
| `POST /v1/auth/password-reset/confirm {token, newPassword}`, `GET/POST /reset-password` | | The page is the emailed link's form. Rotates the play key and ends all sessions. |
| `POST /v1/auth/change-password {currentPassword, newPassword}` | | Same rotation; returns a new session. |
| `GET /v1/me` | `getUser` | Includes `playKey` and `userJson` once the email is verified and a code is set. |
| `POST /v1/me/netplay {codeStart}` | `userInitNetplay` | Assigns the connect code (once). |
| `POST /v1/me/rename {displayName}` | `userRename` | |
| `POST /v1/me/accept-rules {num}` | `userAcceptRules` | |
| `GET /v1/me/user-json` | launcher `user.json` | Exactly `{uid, playKey, connectCode, displayName, latestVersion}`. |
| `GET /user/{uid}?additionalFields=chatMessages,rank` | users-rest | Public, as on Slippi. Default chat messages, zero rank. |
| `GET /healthz` | | `ok` if Postgres answers. |

Rules:
- **Connect codes**: the user picks 2-4 letters (`/^[a-zA-Z]+$/`, stored uppercase). The server appends `#N`, the lowest free number from a random start in 1-999, falling back to 1000-9999 only if all 999 are taken and the code stays at most 8 characters (so a 4-letter prefix never gets 4 digits). Codes are unique and immutable (`admin user set-code` for the one approved change). Codes typed in-game are matched case- and width-insensitively, and leading zeros in the number are ignored (`ＡＢ＃００７` = `AB#7`).
- **Display names**: 1-15 characters of printable ASCII, Hiragana or Katakana, without `\` or `` ` `` (the Slippi launcher rule).
- **Passwords**: 8-256 characters, Argon2id with m = 64 MiB, t = 3, p = 1.
- **Rate limits**: per IP and per email on login, sign-up and reset: 5 per minute and 20 per hour. One mm ticket per account per 2 s.
- **Email**: Resend's HTTP API in production, with a daily cap (`MAIL_DAILY_LIMIT`, default 90, under the free tier's 100). A `Mailer` trait has Resend, stdout, file and in-memory implementations; tests only use the in-memory one, and the Resend client is tested against a local fake server.

## Matchmaking server

Byte-compatible with Slippi's client: ENet on UDP 43113, reliable JSON packets on channel 0, three channels. Tested against Slippi's own C ENet (`tools/c-enet-interop`).

- `create-ticket` → `create-ticket-resp` (with `error` on refusal), then `get-ticket-resp` with `matchId`, `isHost`, `players[{uid, displayName, connectCode, port, isLocalPlayer, ipAddress, ipAddressLan, chatMessages[16], isBot}]`, `stages`, `items`, or with `error`.
- `search.connectCode` is the byte array the game sends (Shift-JIS, usually full-width) or plain ASCII.
- **Hole punching**: `ipAddress` is the address and port the server observed, i.e. the NAT mapping of the port the client will reuse for P2P. `ipAddressLan` is the client's reported LAN address, cleaned to `a.b.c.d:port` or `""`. Clients use the LAN address when both players share an external IP (same LAN, VPN node or CGNAT), as Slippi does; `mmclient` implements that rule.
- **Host**: the player whose uid has the lowest SHA-256 is `isHost` and port 1, so the choice is deterministic.
- **Account check**: mm reads `users` from Postgres directly (play key, ban, code), on a worker task so the ENet loop never waits on the database. That is simpler than an internal HTTP call to `accounts`: no extra authenticated endpoint, one less hop, and mm needs Postgres anyway to record matches in `mm_matches`.
- **Errors, never silence**: every refusal and timeout reaches the game as an `error` string (max 120 characters):

| Situation | Message |
|---|---|
| Ranked, Teams, Party | `<Mode> is not supported yet. Only Direct and Unranked work for now.` |
| Direct ticket waited `MM_TICKET_TTL_SECS` (10 min) | `Search timed out: <code> did not connect within 10 minutes.` (`get-ticket-resp`) |
| Unranked ticket waited `MM_TICKET_TTL_SECS` | `Search timed out: no opponent found within 10 minutes.` (`get-ticket-resp`; Slippi's client waits forever, so the server owns expiry) |
| Bad play key (or rotated by password change or ban) | `Invalid play key. Log in again in the launcher.` |
| Unknown uid / account | `Not logged in. Log in again in the launcher.` / `Account not found. Log in again in the launcher.` |
| Banned | `This account is banned from online play.` |
| No connect code yet | `Pick a connect code in the launcher first.` |
| Malformed code / own code | `Invalid connect code` / `That is your own connect code. Enter your opponent's code.` |
| Malformed packet, unknown message | `Invalid matchmaking request` / `Unknown matchmaking request. Your game may need an update.` |
| Second search from the same account | the older ticket gets `This search was replaced by a newer one from the same account.` |
| Database slow or down (4 s) | `Matchmaking is temporarily unavailable. Try again later.` |
| Client older than `MM_MIN_APP_VERSION` | `Your game is out of date. Update to <version> to play online.` |

- **Unranked queue** (mode 1; `search.connectCode` is `[]` and ignored): first come, first served. The oldest waiting ticket is paired with the next ticket in arrival order that is in its region and that it did not just fail to connect to. The answer is the same `get-ticket-resp` as Direct's, with `matchId` `mode.unranked-<time>-<n>` and the Unranked stage list. Direct and Unranked tickets never meet.
- **Regions**: Slippi's tickets carry no region, ping or latency field, so the server infers the region from the ticket's source address. For the friends-only start that is a small prefix table, `MM_REGIONS_FILE` (JSON `{"na": ["203.0.113.0/24", ...], "eu": [...]}`; the longest prefix wins, everything else is `other`). Without the file there is one bucket, i.e. plain FIFO. A ticket that has waited `MM_REGION_WIDEN_SECS` (30 s) takes an opponent from any region. The region goes into `mm_matches.region` (`a+b` for a cross-region match). The design's GeoIP database (DB-IP Lite) can replace the table later without touching the queue.
- **Failed P2P connects**: after a match, Slippi's client tries the peer for 8 s and, on failure in 1v1, requeues with a new ticket. If the same pair searches again within 60 s of being matched, mm treats the last connect as failed.
  - Direct: it waits before re-pairing them (last match + 8 s + 5 s × failures). After the third failed connect it tells both players `Could not connect to <code> after 3 tries. A firewall or strict NAT may block it.`
  - Unranked: each of the two is paired with anyone else who is searching first. The two are paired with each other again only after the same backoff, and after the third failure not at all until 60 s have passed (no error: they keep searching).
  - A pair that searches again more than 60 s after a match (they played) is paired at once, as strangers.
- A client disconnect (the CSS cancel) removes its ticket at once. Garbage and oversized packets get an error and a disconnect; the loop never panics on input.
- **Disconnects after an answer**: after `get-ticket-resp` (or a refusal) the server waits 1 s before disconnecting the client itself. Slippi's client disconnects on its own as soon as it has the answer and waits up to 3 s for that to be acknowledged before it binds its P2P port; when both sides disconnected at the same moment, the client's disconnect was sometimes never answered (2 of 16 matches with Dolphin), which cost it 3 s of the 8 s connect window. `mmclient` reports the outcome as `mmDisconnect`.

## Rulesets and stage lists

`config/rulesets.json` (built in; `MM_RULESETS_FILE` replaces it) holds the rules per mode: `direct`, `unranked` and `ranked` (Ranked is refused until it is built). Each has `stages`, `items` (bitfield, 0 = off) and `stocks`/`timeMinutes` (4 stocks, 8 minutes: the P+ competitive ruleset). mm checks the file at start-up: known mode names, ids up to 255, no duplicates.

`stages` is sent in every `get-ticket-resp` and recorded in `mm_matches.stages`, where Slippi's server puts its list. The ids are Brawl's `srStageKind` values, which the game's match setup and P+'s stage loader key on. The list is P+ v3.2's own legal list: the 15 stages its random-stage "Default" preset (`Switch00.rss`) turns on (`docs/game-code.md` section 11).

| id | stage | id | stage | id | stage |
|---|---|---|---|---|---|
| 1 (0x01) | Battlefield | 6 (0x06) | Bowser's Castle | 28 (0x1C) | Wario Land |
| 2 (0x02) | Final Destination | 9 (0x09) | Temple of Time | 31 (0x1F) | Fountain of Dreams |
| 3 (0x03) | Delfino's Secret | 12 (0x0C) | Frigate Husk | 33 (0x21) | Smashville |
| 4 (0x04) | Luigi's Mansion | 13 (0x0D) | Yoshi's Island | 35 (0x23) | Green Hill Zone |
| 5 (0x05) | Metal Cavern | 45 (0x2D) | Dream Land | 46 (0x2E) | Pokémon Stadium 2 |

How Dolphin uses it (as Slippi's client does): the host draws every random stage from it without repeats until the list is used up (Slippi's stage pool). That covers every Unranked game and Direct's game 1. Direct's loser picks games 2+ on P+'s stage select, and the pick must be in the list; otherwise the stage is drawn at random. With no list, Dolphin falls back to its built-in copy of the same legal list.

## Admin CLI

```sh
admin migrate
admin gen-secret
admin invite create [--uses N] [--expires-days D] [--note TEXT]
admin invite list | revoke CODE
admin user list
admin user show IDENT                          # IDENT = uid, email or connect code
admin user reset-password IDENT [--send-email] # prints (or emails) a one-time link valid 24 h
admin user ban IDENT [--days N] [--reason TEXT]  # permanent without --days; ends sessions, rotates play key
admin user unban IDENT
admin user verify-email IDENT
admin user rotate-play-key IDENT
admin user set-code IDENT CODE
```

Every change is written to `audit_log`.

## mmclient

```
mmclient search [--server HOST[:PORT]] (--user-json FILE | --uid U --play-key K) [--code CODE]
                [--mode direct|unranked|ranked|teams|party | --mode-number N] [--encoding fullwidth|ascii]
                [--port P] [--lan-ip IP] [--timeout-secs S] [--punch [--requeue N]] [--app-version V]
mmclient raw [--server HOST[:PORT]] --data 'JSON' [--wait-secs S]
```

`search` prints one JSON object (`status`, `error`, `localPort`, `lanAddress`, `createResponse`, `ticketResponse`, `remoteAddresses`, `p2p`) and exits 0 matched, 2 create-ticket error, 3 get-ticket error, 4 client timeout, 5 P2P failed, 1 other. `--port` and `--lan-ip` mirror Slippi's "Force Netplay Port" and "Force LAN IP". `MM_SERVER` sets the default server.

## Tests

```sh
cargo test --workspace          # starts the compose Postgres if it is not running
cargo clippy --workspace --all-targets -- -D warnings
```

Without Docker, start the portable Postgres (above) and point the tests at it, e.g. `TEST_DATABASE_URL=postgres://pp:pp-dev-password@127.0.0.1:54329/postgres`. The e2e helpers only fall back to `docker compose up` when `TEST_DATABASE_URL` is unset and nothing answers on 54329.

- Unit tests: code and name rules, Shift-JIS decoding, protocol parsing and wire shape (openmelee and Brawlback fixtures), play keys, rate limiter, Argon2, mailers (Resend against a local fake), and the ticket state machine (pairing, host choice, expiry, wrong code, bad key, bans, unsupported modes, malformed input, cancel, replacement, rate limit, DB timeout, version gate, re-pair backoff), the Unranked queue (arrival order, response fields and stage list, code ignored, never paired with Direct, expiry error, cancel, replacement, failed-connect preference and backoff, regions and widening), the region table and the rulesets file.
- `crates/e2e/tests/accounts_api.rs`: the HTTP API against Postgres (invites, validation, verification, code assignment, `user.json`, reset and change password rotating the play key, bans, rate limits).
- `crates/e2e/tests/matchmaking.rs`: Postgres + accounts + mm in process, accounts created over HTTP, two fake ENet clients: matching peer info and a real P2P connection, expiry, wrong code, bad play key, unsupported modes, malformed packets and raw UDP garbage (server keeps running and still pairs), cancel.
- `crates/e2e/tests/unranked.rs`:
  - two strangers meet: response fields, the ruleset's 15 stages, a real P2P connection, and the `mm_matches` row with mode, stages and region;
  - first come, first served, and the third ticket ends with the expiry error;
  - a cancelled ticket is not paired;
  - Slippi's requeue (`mmclient` `requeue`): A's P2P connect to B fails and A requeues; while B searches again, A is paired with D and B with C;
  - two failed players alone are re-paired only after 8 s + 5 s.

Each test uses its own database `e2e_<time>_<random>`, dropped at the end. `TEST_DATABASE_URL` overrides the Postgres admin URL.

When finished: `docker compose down` (add `-v` to delete the data volume).

## Differences from docs/backend-design.md

- **Layout and names**: the workspace is `server/` (not `backend/`), the HTTP service is `accounts` (the design's `api`), and the shared crate is `common` (`core` clashes with Rust's `core`). There is no `relay` crate yet (Phase 1.5).
- **Play keys are derived, not hashed**: the design says to store only a SHA-256 of the play key, but Slippi's launcher re-fetches the key on every Play (`getUser` → `private.playKey`), so it must be retrievable. The key is `HMAC-SHA256(PLAY_KEY_SECRET, uid, version)`; the database stores only the version. A database dump still reveals no key, and rotation is a version bump.
- **Email from day 1** (user decision, 2026-10-06): verification at sign-up and self-service reset through Resend, with invite codes kept as an optional gate. Login is by email; no separate username.
- **Direct and Unranked**: Ranked, Teams and Party are refused with a clear error. Unranked has no rating band (the design's "widening rating band" needs Phase 4's rating). Regions come from a prefix table instead of a GeoIP database (see above). No rating, reports or replays yet.
- **Not built yet**: GraphQL facade for the unmodified launcher (the JSON endpoints mirror its operations one to one), `/metrics`, build-hash/ISO allow-list on tickets (`search.game` is accepted and ignored), the reserved-code period after account deletion (accounts cannot be deleted yet), website pages beyond the two email-link pages.
- **Known limit**: `rusty_enet` does not expose ENet's maximum packet size (32 MB default), so a client can make the server buffer a large reliable packet before mm rejects it (> 8 KiB is refused after reassembly). Fine for a friends-only server; revisit before opening sign-ups.
