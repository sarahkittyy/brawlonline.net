# Production deployment notes

**Production now runs on `sarahvps2` (user decision, 2026-10-07), with nginx instead of Caddy and Postgres 17. See `PROD.md` for what is installed there; the files used are in `sarahvps2/`.** The rest of this page is the original generic plan.

These files follow the plan in `docs/backend-design.md` section 2.1: one OVH dedicated Debian box, Postgres 16 from Debian packages, Caddy for TLS, systemd units, nightly backups off the box. No containers in production; `docker-compose.yml` is only for development and tests.

The product is called Brawl Online (`PRODUCT_NAME` in `crates/common`: email subjects and bodies, page titles; `MAIL_FROM` sets the sender name). The subdomains are not decided. The examples use placeholder subdomains of `fluffycat.gay`: `accounts.fluffycat.gay` (HTTP API) and `mm.fluffycat.gay` (UDP matchmaking). Change them in one place each: the Caddyfile, `PUBLIC_BASE_URL`, and the client build's mm hostname.

The original plan kept these services off the existing VPSes (`sarahvps`, `sarahvps2`). The user later chose `sarahvps2` for production; `PROD.md` covers how it shares that box.

## Files

| File | Goes to |
|---|---|
| `systemd/pp-accounts.service` | `/etc/systemd/system/` |
| `systemd/pp-mm.service` | `/etc/systemd/system/` |
| `systemd/pp-backup.service`, `pp-backup.timer` | `/etc/systemd/system/` |
| `caddy/Caddyfile` | `/etc/caddy/Caddyfile` |
| `env/accounts.env.example` | `/etc/ppserver/accounts.env` (0640 root) |
| `env/mm.env.example` | `/etc/ppserver/mm.env` (0640 root) |
| `env/backup.env.example` | `/etc/ppserver/backup.env` (0600 root) |
| release binaries `accounts`, `mm`, `admin` | `/opt/ppserver/bin/` |

`PLAY_KEY_SECRET` must be identical in `accounts.env` and `mm.env`. Generate it once with `admin gen-secret`. Changing it invalidates every play key; launchers fetch new keys on their next Play, and running games need a relaunch.

## Steps (Debian 12/13)

```sh
# Packages
apt install postgresql caddy restic

# Database and roles. accounts owns the schema and runs migrations;
# mm only reads users and records matches.
sudo -u postgres psql <<'SQL'
CREATE ROLE pp_accounts LOGIN PASSWORD 'CHANGE_ME';
CREATE ROLE pp_mm LOGIN PASSWORD 'CHANGE_ME';
CREATE DATABASE pp OWNER pp_accounts;
SQL
# After the first start of pp-accounts (which creates the tables):
sudo -u postgres psql -d pp <<'SQL'
GRANT USAGE ON SCHEMA public TO pp_mm;
GRANT SELECT (uid, display_name, connect_code, play_key_version, banned_until, email_verified_at) ON users TO pp_mm;
GRANT INSERT ON mm_matches TO pp_mm;
-- mm inserts with ON CONFLICT (match_id) DO NOTHING, which needs SELECT on the
-- conflict column. Without it every match fails to record ("permission denied").
GRANT SELECT (match_id) ON mm_matches TO pp_mm;
SQL

# Binaries (built with `cargo build --release` on the same Debian release, or in CI)
install -d /opt/ppserver/bin /etc/ppserver
install -m 0755 target/release/{accounts,mm,admin} /opt/ppserver/bin/
# Config: copy the env examples, fill in secrets, chmod as in the table above.

systemctl daemon-reload
systemctl enable --now pp-accounts pp-mm caddy pp-backup.timer

# First invite
DATABASE_URL=... /opt/ppserver/bin/admin invite create --note "first friend"
```

Firewall (nftables or OVH's network firewall): allow TCP 22, 80, 443 and **UDP 43113**. Nothing else needs to be public; Postgres listens on localhost only (Debian's default).

## Cloudflare DNS

- `mm.fluffycat.gay`: an A record, **DNS only (grey cloud)**. Cloudflare's proxy does not carry arbitrary UDP, and the mm server must see each player's real address and port: that observed address is what the peer hole-punches to.
- `accounts.fluffycat.gay`: start with **DNS only** too. Caddy then gets its certificate directly and sees real client IPs, which the rate limits use (`TRUST_PROXY_HEADERS=true` reads Caddy's `X-Forwarded-For`).
  If you later turn the orange cloud on: set SSL mode to Full (strict), add Cloudflare's ranges to Caddy's `trusted_proxies` so `X-Forwarded-For` carries the real client IP, and make sure the ACME HTTP challenge still reaches Caddy (or use a Cloudflare Origin certificate).
- Email (Resend): add the SPF, DKIM and (optionally) DMARC records Resend shows for `fluffycat.gay` when you verify the sending domain. Until the domain is verified, Resend refuses to send from `noreply@fluffycat.gay`.

## Backups

`pp-backup.timer` runs nightly at about 04:15 UTC:

1. `pg_dump --format=custom` of the `pp` database to `/var/backups/ppserver/` (14 days kept locally);
2. `restic backup` of that folder plus `/etc/ppserver` (the secrets: the restic repository is encrypted) to an off-box repository, for example OVH Object Storage;
3. `restic forget --prune` keeping 14 daily, 8 weekly and 12 monthly snapshots.

Initialise the repository once with `restic init` (using `backup.env`). Keep the restic password somewhere other than the box, or the backup is useless when the box is lost.

**Restore drill** (do it once before friends rely on the service, then every few months):

```sh
restic snapshots --tag ppserver
restic restore latest --tag ppserver --target /tmp/restore
createdb -U postgres pp_restore
pg_restore -U postgres -d pp_restore /tmp/restore/var/backups/ppserver/pp-*.dump
psql -U postgres -d pp_restore -c 'select count(*) from users'
```

At friends scale a nightly dump is enough. Add WAL archiving (for point-in-time recovery) once ranked results exist and losing a day of them matters.

## Monitoring

- `journalctl -u pp-accounts -u pp-mm`. mm logs a `mm stats` line (connections, waiting tickets) every minute.
- `GET https://accounts.fluffycat.gay/healthz` returns `ok` when the service can reach Postgres; point a free external uptime check at it.
- The Prometheus `/metrics` endpoints from the design are not built yet (Phase 5).
