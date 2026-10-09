# Brawl Online backend

Accounts, connect codes, and Direct and Unranked matchmaking for Brawl Online, the Project+ rollback client. Phase 1 goal: two friends log in, get connect codes and connect to each other by code, the way Slippi's Direct mode works. Phase 3 (started): strangers meet through Slippi's Unranked queue. Phase 4: Ranked, best-of-three sets rated with Elo (see "Ranked"). Design: `docs/backend-design.md` (sections 1-4 and 7).

| Crate | What it is |
|---|---|
| `crates/accounts` | HTTP service (axum + sqlx): open sign-up with email verification, login, sessions, password reset, connect codes, play keys, the data for `user.json`, Slippi's users-rest endpoint. |
| `crates/mm` | Matchmaking server: ENet over UDP 43113, speaking Slippi's `create-ticket` / `get-ticket-resp` JSON protocol. Direct, Unranked and Ranked (rating band); per-mode stage lists. |
| `crates/admin` | Admin CLI: users, password resets, bans, account deletion. |
| `crates/mmclient` | Ticket client library and CLI that behaves like Slippi's `SlippiMatchmaking.cpp`. For tests, ppharness and game-integration work. |
| `crates/common` | Shared code: connect-code and display-name rules, the mm message types, play keys, rate limiter, migrations. |
| `crates/e2e` | End-to-end tests (Postgres + both services + fake game clients). |
| `crates/fakesmtp` | A tiny in-process SMTP server for tests (records what it receives, counts connections). |
| `migrations/` | Postgres schema (applied by `accounts` on start, or `admin migrate`). |
| `config/rulesets.json` | Per-mode rules: the stage list sent in `get-ticket-resp` (P+'s legal stages), items, stocks/timer. See "Rulesets and stage lists". |
| `deploy/` | systemd units, Caddyfile, env examples, backup notes for the OVH box. |
| `tools/c-enet-interop/` | Manual check against Slippi's own C ENet. |

Parts of `common` are derived from [openmelee](https://github.com/panchaea/openmelee) (GPL-2.0): the message shapes and test fixtures, and the Shift-JIS + NFKC decoding of codes. The workspace is GPL-2.0-or-later.

## Running locally

Needs Rust 1.85+ and a Postgres: Docker (for Postgres only; the services run as plain processes) or the portable Postgres below.

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

Every setting is an environment variable (or a flag; `--help` lists them). The binaries load `.env` from the current directory. With `MAILER=stdout` (the default when `RESEND_API_KEY` is empty) emails are printed instead of sent, so the verification link appears in the accounts terminal. `MAILER=file` appends them to `MAIL_FILE` as JSON lines. `MAILER=smtp` sends through a real provider (see "Email providers").

### Try it end to end

```sh
# 1. Sign up (open to everyone); the verification link is printed by accounts (MAILER=stdout)
curl -s localhost:8080/v1/auth/signup -H 'content-type: application/json' \
  -d '{"email":"alice@example.test","password":"a long password","displayName":"alice"}'
#    -> {"sessionToken": "...", "user": {...}}
curl -s "localhost:8080/verify-email?token=..."

# 2. Pick the code prefix; the server appends #N
curl -s localhost:8080/v1/me/netplay -H "authorization: Bearer $SESSION" \
  -H 'content-type: application/json' -d '{"codeStart":"alic"}'

# 3. Write user.json exactly as the launcher would
curl -s localhost:8080/v1/me/user-json -H "authorization: Bearer $SESSION" > alice.user.json

# 4. Do the same for bob, then search for each other
cargo run -p mmclient -- search --user-json alice.user.json --code BOB#123 --punch &
cargo run -p mmclient -- search --user-json bob.user.json --code ALIC#456 --punch
```

Both print a JSON result with `"status":"matched"`, the full `get-ticket-resp`, the peer address chosen with Slippi's LAN rule, and (with `--punch`) whether a P2P ENet connection from the same port succeeded.

## Accounts API

JSON in and out. Errors are `{"error": {"code": "...", "message": "..."}}`. Authenticated endpoints take `Authorization: Bearer <sessionToken>`. Names mirror the Slippi launcher's GraphQL operations.

| Endpoint | Slippi equivalent | Notes |
|---|---|---|
| `POST /v1/auth/signup {email, password, displayName}` | `createUserNew` | 201 with a session. Open to everyone. Sends a verification email. 409 `email_taken`; 429 when a limit below is reached. |
| `POST /v1/auth/login {email, password}` | `signInWithEmailAndPassword` | `{sessionToken, user}`. Sessions slide for 90 days. |
| `POST /v1/auth/logout` | | |
| `POST /v1/auth/verify-email {token}`, `GET /verify-email?token=` | Firebase verification | The GET is the page the email links to. |
| `POST /v1/auth/verify-email/resend` | | Rate-limited (see below). |
| `POST /v1/auth/password-reset/request {email}` | Firebase reset | 202 whether or not the account exists (429 only from the per-IP and per-address limits). Emails a 1-hour link. |
| `POST /v1/auth/password-reset/confirm {token, newPassword}`, `GET/POST /reset-password` | | The page is the emailed link's form. Rotates the play key and ends all sessions. |
| `POST /v1/auth/change-password {currentPassword, newPassword}` | | Same rotation; returns a new session. |
| `GET /v1/me` | `getUser` | Includes `playKey` and `userJson` once the email is verified and a code is set. |
| `POST /v1/me/netplay {codeStart}` | `userInitNetplay` | Assigns the connect code (once). |
| `POST /v1/me/rename {displayName}` | `userRename` | |
| `POST /v1/me/accept-rules {num}` | `userAcceptRules` | |
| `GET /v1/me/user-json` | launcher `user.json` | Exactly `{uid, playKey, connectCode, displayName, latestVersion}`. |
| `GET /v1/me/matches?mode=all\|ranked\|unranked&limit=&before=` | | The player's match history, newest first (see "Match history"). `{matches: [{matchId, mode, createdAt, status, ranked, players: [{uid, displayName, connectCode, wins, ratingBefore, ratingAfter, ratingChange}], winner, endReason}], next}`. `limit` 20 by default, at most 50; `before` is the previous page's `next`. |
| `GET /user/{uid}?additionalFields=chatMessages,rank` | users-rest | Public, as on Slippi. Default chat messages; `rank.ratingOrdinal` is the Elo rating (1400 before the first set), `rank.ratingUpdateCount` the rated sets, `rank.position` the leaderboard position (null before the first rated set), `rank.rankedPlayers` the players on the leaderboard. No daily placements. |
| `POST /v1/ranked/report-game {uid, playKey, matchId, gameIndex, winner, stageId, durationFrames, players}` | `reportOnlineGame` | Play key, as mm's ticket. One client's result of one game (`winner` a uid, null for a draw) of a Ranked, Unranked or Direct match. Idempotent; another winner for the same game is 409. Answers the set's state (below); Unranked and Direct games are stored for the history and never rated (status always `ASSIGNED`, `rating` null). |
| `POST /v1/ranked/report-leave {uid, playKey, matchId, kind}` | `reportOnlineMatchStatus(abandoned)` | `kind`: `left` (this player left the set) or `opponent_left`. |
| `GET /v1/ranked/result?matchId=&uid=` | `getRankedMatchPersonalResult` | Public. `{matchId, status, players, wins, winner, reason, rating: {before, after, change, setsPlayed} \| null}`. |
| `GET /v1/ranked/leaderboard?limit=&after=` | | Public. `{entries: [{position, uid, displayName, connectCode, rating, setsPlayed, wins, losses}], next, total}` (see "Leaderboard"). `limit` 50 by default, at most 100; `after` is the previous page's `next`. |
| `GET /healthz` | | `ok` if Postgres answers. |

Rules:
- **Connect codes**: the user picks 2-4 letters (`/^[a-zA-Z]+$/`, stored uppercase). The server appends `#N`, the lowest free number from a random start in 1-999, falling back to 1000-9999 only if all 999 are taken and the code stays at most 8 characters (so a 4-letter prefix never gets 4 digits). Codes are unique and immutable (`admin user set-code` for the one approved change). Codes typed in-game are matched case- and width-insensitively, and leading zeros in the number are ignored (`ＡＢ＃００７` = `AB#7`).
- **Display names**: 1-15 characters of printable ASCII, Hiragana or Katakana, without `\` or `` ` `` (the Slippi launcher rule).
- **Passwords**: 8-256 characters, Argon2id with m = 64 MiB, t = 3, p = 1.
- **Rate limits** (in memory, per process; `common::ratelimit` and `accounts::Limits`). Sign-up is open to everyone, so every path that sends an email is limited, and neither one client nor one kind of email can use up the email quota (Brevo's free plan: 300 a day). The client IP is the last `X-Forwarded-For` entry behind the proxy, and an IPv6 /64 counts as one address. A refusal is 429 `rate_limited` with `Retry-After`.

  | What | Key | Limit |
  |---|---|---|
  | Login | IP, and email | 5 per minute, 20 per hour (a successful login clears the email's count) |
  | Sign-up attempts, valid or not | IP | 5 per minute, 20 per hour |
  | Accounts created (each sends one verification email) | IP | 3 per hour, 10 per day. A taken email is refused (409) before this count |
  | Verification email resends | account | 1 per minute, 3 per hour, 5 per day |
  | Password-reset requests | IP | 5 per minute, 20 per hour |
  | Password-reset emails | address | 1 per minute, 3 per hour, 5 per day |
  | Verification emails (sign-ups + resends), all clients | global | 2/3 of `MAIL_DAILY_LIMIT` per 24 h (60 with 90). After that, sign-up and resend answer 429 "Too many sign-ups right now", so no account is created whose email cannot go out |
  | Password-reset emails, all clients | global | the other 1/3 (30 with 90). After that, the request still answers 202 (no account enumeration), no email is sent, and accounts logs a WARN |
  | Password change | account | 5 per minute, 20 per hour |
  | Leaderboard pages | IP | 30 per minute, 600 per hour |
  | Match history pages | account | 60 per minute |
  | mm tickets | account | 1 per 2 s |

  Worst case for one IP: 10 verification emails a day. For one inbox: 1 + 5 verification emails (one account per address) and 5 reset emails a day. The two global shares add up to `MAIL_DAILY_LIMIT`, so accounts never sends more than that per UTC day. Set it below the provider's quota, with room for `admin user reset-password --send-email` (a separate process with its own count).
- **Email**: an SMTP provider (`MAILER=smtp`), Brevo's HTTP API (`MAILER=brevo`) or Resend's (`MAILER=resend`) in production, with a daily cap per process (`MAIL_DAILY_LIMIT`, default 90, split as above). A `Mailer` trait has SMTP, Brevo, Resend, stdout, file and in-memory implementations, and `mail::from_config` is the one place that picks one: `accounts` and the admin CLI's `reset-password --send-email` build their mailer from the same settings. Tests only use the in-memory and file mailers, plus the SMTP client against a local fake server (`crates/fakesmtp`) and the Brevo and Resend clients against local fake HTTP servers.

## Email providers

`MAILER=smtp` sends through any provider's SMTP submission server with [lettre](https://lettre.rs) and rustls (no OpenSSL). Each email is a `multipart/alternative` message with the same text and HTML parts the Resend mailer sends, a `Message-ID` on the `MAIL_FROM` domain, and one connection per email.

| Setting | Default | Meaning |
|---|---|---|
| `MAILER` | `stdout` (`resend` when `RESEND_API_KEY` is set, else `brevo` when `BREVO_API_KEY` is set) | `smtp`, `brevo`, `resend`, `stdout` or `file` |
| `MAIL_FROM` | `Brawl Online <noreply@brawlonline.net>` | Sender. Its domain must be verified at the provider |
| `SMTP_HOST` | (required) | The provider's SMTP host |
| `SMTP_PORT` | 587 (465 with `SMTP_TLS=tls`) | |
| `SMTP_USERNAME`, `SMTP_PASSWORD` | unset | Both or neither. Never logged (`Debug` prints `Secret(<redacted>)`) |
| `SMTP_TLS` | `starttls` | `starttls`: plain connect, then STARTTLS, which is required (a server without it is an error; credentials never go out unencrypted). `tls`: TLS from the first byte (port 465). `none`: no encryption, refused unless `SMTP_HOST` is `localhost`, `127.0.0.1` or `::1` (local test servers only) |
| `SMTP_TIMEOUT_SECS` | 15 | Limit for one whole send, connect to QUIT |

Configuration mistakes (no host, username without password, invalid `MAIL_FROM`, `none` with a remote host) stop `accounts` at start and `admin --send-email` before it issues a token. A failed send is logged by accounts as one `ERROR` line naming the server and the provider's answer, for example `sending verification email failed: smtp smtp-relay.brevo.com:587 (starttls): permanent error (535): 5.7.8 Authentication failed`, or `... no answer within 15 s`. At start accounts logs `mail=smtp <host>:<port> tls=Starttls auth=true` (never the username or password).

`MAILER=brevo` sends through Brevo's transactional email HTTP API instead of SMTP: `POST https://api.brevo.com/v3/smtp/email` with the header `api-key: $BREVO_API_KEY` and `sender{name,email}` (from `MAIL_FROM`), `to[{email}]`, `subject`, `textContent` and `htmlContent` (the same content as the other mailers), 15 s timeout. `BREVO_API_KEY` is an API v3 key (`xkeysib-...`), not an SMTP key; Brevo can restrict a key to authorised IP addresses, in which case it only works from the production box. `BREVO_API_URL` exists for tests against a local fake. Errors read `brevo <url> returned 401 Unauthorized: {"code":"unauthorized","message":"Key not found"}`; the key is never logged.

**Switching providers** is a settings change only: set `MAILER=smtp` and the `SMTP_*` values (or, for Brevo's API, `MAILER=brevo` and `BREVO_API_KEY`), then restart accounts. Example submission servers, all STARTTLS on 587 (**verify in provider docs**; hosts, ports and login schemes change):

| Provider | `SMTP_HOST:SMTP_PORT` | Login |
|---|---|---|
| Brevo | `smtp-relay.brevo.com:587` (verify in provider docs) | the account's SMTP login and an SMTP key (Brevo's SMTP & API settings) |
| SMTP2GO | `mail.smtp2go.com:587` (verify in provider docs) | an SMTP user created in SMTP2GO |
| Mailjet | `in-v3.mailjet.com:587` (verify in provider docs) | API key as username, secret key as password |
| Postmark | `smtp.postmarkapp.com:587` (verify in provider docs) | the server API token as both username and password |

**DNS for the sending domain** (`brawlonline.net`). Every provider asks you to verify the domain and shows the exact records in its dashboard; add those, not values from here. In general:

- **SPF**: one TXT record on the domain, `v=spf1 include:<provider's SPF domain> ~all`. A domain has exactly one SPF record: when switching or adding providers, edit the existing record's `include:` list instead of adding a second record. Some providers cover SPF with a return-path (bounce) subdomain CNAME instead of an `include` on the apex; follow their dashboard.
- **DKIM**: a TXT or CNAME record at `<selector>._domainkey.brawlonline.net` with the provider's public key. Copy it exactly; the selector is provider-specific, so two providers can coexist during a switch.
- **DMARC**: a TXT record at `_dmarc.brawlonline.net`, for example `v=DMARC1; p=none; rua=mailto:<an address you read>` to start; tighten to `p=quarantine` once reports show SPF and DKIM pass.
- In Cloudflare, these records are DNS-only (TXT and CNAME records are never proxied anyway). Some providers also ask for a verification TXT or a return-path CNAME.

Until the provider shows the domain as verified, it refuses or spam-folders mail from `noreply@brawlonline.net`. Send a test (sign up, or `admin user reset-password <your account> --send-email`) and check the headers for `spf=pass`, `dkim=pass` and `dmarc=pass`.

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
| Teams, Party | `<Mode> isn't available yet.` |
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
- **Ranked queue** (mode 0, `connectCode: []`): like Unranked, but the oldest ticket takes the closest-rated opponent (Elo, from `ratings`) within the rating band: ±`MM_RANKED_BAND` (150), widened by `MM_RANKED_BAND_STEP` (50) for every `MM_RANKED_BAND_STEP_SECS` (15 s) the longer-waiting of the two has waited, so after 5 minutes anyone is in range. `matchId` is `mode.ranked-<time>-<n>`, the stage list is the Ranked one (the counterpick list), `starters` the five stages struck for game 1 (`config/rulesets.json` `ranked.starters`, ours; Slippi's client has them built in), and every player carries `rank {rating, updateCount}`.
- **Regions**: Slippi's tickets carry no region, ping or latency field, so the server infers the region from the ticket's source address. For the friends-only start that is a small prefix table, `MM_REGIONS_FILE` (JSON `{"na": ["203.0.113.0/24", ...], "eu": [...]}`; the longest prefix wins, everything else is `other`). Without the file there is one bucket, i.e. plain FIFO. A ticket that has waited `MM_REGION_WIDEN_SECS` (30 s) takes an opponent from any region. The region goes into `mm_matches.region` (`a+b` for a cross-region match). The design's GeoIP database (DB-IP Lite) can replace the table later without touching the queue.
- **Failed P2P connects**: after a match, Slippi's client tries the peer for 8 s and, on failure in 1v1, requeues with a new ticket. If the same pair searches again within 60 s of being matched, mm treats the last connect as failed.
  - Direct: it waits before re-pairing them (last match + 8 s + 5 s × failures). After the third failed connect it tells both players `Could not connect to <code> after 3 tries. A firewall or strict NAT may block it.`
  - Unranked: each of the two is paired with anyone else who is searching first. The two are paired with each other again only after the same backoff, and after the third failure not at all until 60 s have passed (no error: they keep searching).
  - A pair that searches again more than 60 s after a match (they played) is paired at once, as strangers.
- A client disconnect (the CSS cancel) removes its ticket at once. Garbage and oversized packets get an error and a disconnect; the loop never panics on input.
- **Disconnects after an answer**: after `get-ticket-resp` (or a refusal) the server waits 1 s before disconnecting the client itself. Slippi's client disconnects on its own as soon as it has the answer and waits up to 3 s for that to be acknowledged before it binds its P2P port; when both sides disconnected at the same moment, the client's disconnect was sometimes never answered (2 of 16 matches with Dolphin), which cost it 3 s of the 8 s connect window. `mmclient` reports the outcome as `mmDisconnect`.

## Ranked

Rules in `crates/common/src/ranked.rs` (unit-tested), storage and endpoints in `crates/accounts/src/ranked.rs`, the leaderboard and match history in `crates/accounts/src/leaderboard.rs` and `history.rs`, schema in `migrations/0003_ranked.sql` and `0004_leaderboard_history.sql`.

- **Rating**: standard Elo on Slippi's scale, no tiers. Expected score `1 / (1 + 10^((opp - own) / 400))`, new rating `own + K × (score - expected)`, score 1 for a set win and 0 for a loss, one update per set (Slippi updates per set). Everyone starts at **1400**, the middle of Slippi's range. K is the player's own: **200** for the first set, falling linearly to **32** at the tenth, then 32 (an even set is ±16). A new player who wins (or loses) their first ten even sets moves about 620 points, so nobody stays at the default for long; an established opponent of a new player still moves by their own K only.
- **Sets**: best of three. Both clients report every game. A game counts when both reports agree; different winners void the set as `ERROR` (held for review, nobody rated). A game only one client reported counts on that report after `RANKED_REPORT_GRACE_SECS` (120 s). Games count in order; a draw counts for nobody and is replayed. Two wins decide the set (`COMPLETE`).
- **Leaving**: `left` abandons the set (`ABANDONED`): the leaver loses, and the other player wins only if at least one game was played. `opponent_left` blames the opponent once the grace period passes without the opponent saying the same; both saying `opponent_left` is a broken connection (`TERMINATED`, nobody rated). A set with no report for `RANKED_STALE_SECS` (30 min) is `ORPHANED` (nobody rated).
- **Once**: the set is settled in one transaction that locks its `mm_matches` row and both `ratings` rows (in uid order), applies the changes, writes `rating_events` and closes the row, so it is rated exactly once. accounts settles after every report and sweeps open sets every `RANKED_SWEEP_SECS` (30 s) for waits that ran out.
- The client side (Dolphin `Online/Ranked.cpp`) reports, counts the wins, ends the session after the deciding game and fetches the result for the game's CSS.
- **Unranked and Direct reports**: the same `report-game` endpoint stores their games in `game_reports` with the same checks (play key, the reporter and the winner are players, idempotent), with game indexes up to 999 since those matches are not sets. They are never settled or rated; the answer is status `ASSIGNED` and the wins counted from the reports (a game counts when its reports agree or only one exists). `report-leave` and `result` stay Ranked-only.
- **Leaderboard**: every player with at least one rated set, by rating (highest first), ties by uid. Keyset pages: `next` is an opaque cursor for the last row's exact (rating, uid), so a deep page is the same index range scan as the first (`ratings_leaderboard_idx`, migration 0004), and a page continues right after the previous one even while ratings change. `position` (1-based, strictly increasing within ties) and `total` are counted in the same snapshot as the page, so they are right on every page. `/user/{uid}` gives the same position. Banned players are not filtered out (an admin deletes or resets them).
- **Match history**: a player's matches of every mode, newest first, keyset pages on (`created_at`, `match_id`). A match is listed once it has a game report, or it is a ranked set that was settled and either changed a rating or has a leave report (an abandoned set with no game). mm records every pairing, including the ones whose P2P connect failed, so an Unranked match without reports and a ranked set that went `ORPHANED` without any report are left out. Wins per player follow `resolve` for Ranked and the agree-or-lone rule above for the others; the rating fields come from `rating_events` (null for Unranked, Direct and sets that did not change that player's rating). `mode=unranked` means Unranked and Direct. Replays are not shared.

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

How Dolphin uses it (as Slippi's client does): the host draws every random stage from it without repeats until the list is used up (Slippi's stage pool). That covers every Unranked game and Direct's game 1. Direct's loser picks games 2+ on P+'s whole stage select, and Dolphin plays that pick even when it is not in the list (Slippi does not restrict Direct). With no list, Dolphin falls back to its built-in copy of the same legal list.

## Admin CLI

```sh
admin migrate
admin gen-secret
admin user list
admin user show IDENT                          # IDENT = uid, email or connect code
admin user reset-password IDENT [--send-email] # prints a one-time link valid 24 h, or sends it with the MAILER accounts uses
admin user ban IDENT [--days N] [--reason TEXT]  # permanent without --days; ends sessions, rotates play key
admin user unban IDENT
admin user verify-email IDENT
admin user rotate-play-key IDENT
admin user set-code IDENT CODE
admin user delete IDENT --yes                  # with its sessions and email tokens; the connect code is free again at once
```

Every change is written to `audit_log`. `reset-password --send-email` reads the same mail settings as accounts (`MAILER`, `MAIL_*`, `SMTP_*`, `BREVO_*`, `RESEND_*`; `admin user reset-password --help` lists them), so with `MAILER=file` the email is appended to `MAIL_FILE` and nothing goes over the network. A bad mail setting fails before the token is issued; the audit row records `emailed` and the mailer.

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

- Unit tests: code and name rules, Shift-JIS decoding, protocol parsing and wire shape (openmelee and Brawlback fixtures), play keys, rate limiter, Argon2, mailers (SMTP against `fakesmtp`: multipart text + HTML, auth, 535 errors without the password, timeout, STARTTLS required; Brevo and Resend against local fakes: request shape, errors without the key; stdout and file never connecting), and the ticket state machine (pairing, host choice, expiry, wrong code, bad key, bans, unsupported modes, malformed input, cancel, replacement, rate limit, DB timeout, version gate, re-pair backoff), the Unranked queue (arrival order, response fields and stage list, code ignored, never paired with Direct, expiry error, cancel, replacement, failed-connect preference and backoff, regions and widening), the region table and the rulesets file, the ranked rules (Elo, sets, the win count of unrated matches) and the leaderboard and history cursors.
- `crates/admin/tests/send_email.rs`: runs the `admin` binary (`user reset-password --send-email`) with `MAILER=file`, `stdout`, `smtp` and `brevo` while `RESEND_API_URL`, `BREVO_API_URL` (with keys) and `SMTP_HOST` point at listeners that count connections: file and stdout never connect, smtp and brevo reach only their own fake server, `resend` without a key fails before issuing a token; and the in-process reset path with an in-memory mailer.
- `crates/e2e/tests/accounts_api.rs`: the HTTP API against Postgres (open sign-up, validation, verification, code assignment, `user.json`, reset and change password rotating the play key, bans, rate limits, the per-IP account limit with IPv6 /64s, the daily email shares).
- `crates/e2e/tests/matchmaking.rs`: Postgres + accounts + mm in process, accounts created over HTTP, two fake ENet clients: matching peer info and a real P2P connection, expiry, wrong code, bad play key, unsupported modes, malformed packets and raw UDP garbage (server keeps running and still pairs), cancel.
- `crates/e2e/tests/ranked.rs`: two players meet in the Ranked queue (each player's rank in the response), both report two games, the set is rated once (±100 for two first-timers), the rating comes back from `/user/{uid}`, the result endpoint and the next ranked ticket; leaving before and after a game, both leaving, disagreeing reports, forged play keys, strangers, bad winners and game indexes, an Unranked match's report accepted unrated; a lone report settled by the background sweep after the grace period.
- `crates/e2e/tests/leaderboard_history.rs`: the leaderboard in pages of 7 and 5 (positions 1..N across pages, ties by uid, players without a rated set left out), the cursor while one player jumps to the top and another drops (page 2 continues after page 1's last row with the new positions; the same cursor gives the same page), bad cursors, `/user/{uid}` positions; the rate limits (30 leaderboard pages a minute per IP and per IPv6 /64, `Retry-After`; 60 history pages a minute per account); Unranked and Direct reports (agreeing, lone, drawn, disagreeing, game 15, the same checks as Ranked, never rated, untouched by the sweep); the match history of Ranked (complete, abandoned with no game, in progress then orphaned), Unranked and Direct matches with every field, the filters, pages of 2 with a newer match added between pages, matches left out (no reports, orphaned without reports, other players'), auth and bad parameters.
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
- **Email from day 1** (user decision, 2026-10-06): verification at sign-up and self-service reset through a mail provider (Resend at first; any SMTP provider since the Resend domain slot was used up). Login is by email; no separate username. Invite codes (the friends-phase gate) were removed on 2026-10-08 (user decision: anyone can create an account); migration 0002 drops the `invites` table and `users.invite_code`.
- **Direct, Unranked and Ranked**: Teams and Party are refused with a clear error. Unranked has no rating band (Slippi's Unranked MMR is hidden; ours is first come, first served). Regions come from a prefix table instead of a GeoIP database (see above).
- **Rating: Elo, not OpenSkill** (user decision, 2026-10-08): the design's OpenSkill with a scaled ordinal and Slippi's tier table is replaced by plain Elo with no tiers (see "Ranked"). Reports exist for every online game, ratings only for Ranked; no replays, no seasons, no rank tiers.
- **Not built yet**: GraphQL facade for the unmodified launcher (the JSON endpoints mirror its operations one to one), `/metrics`, build-hash/ISO allow-list on tickets (`search.game` is accepted and ignored), the reserved-code period after account deletion (only `admin user delete` deletes accounts, and their code is free again at once), website pages beyond the two email-link pages.
- **Known limit**: `rusty_enet` does not expose ENet's maximum packet size (32 MB default), so a client can make the server buffer a large reliable packet before mm rejects it (> 8 KiB is refused after reassembly). Sign-ups are open since 2026-10-08, so this needs revisiting.
