"""Seeds the staging database with ranked players at every rank (docs/design/ranked-tiers.md).

    python tools/staging/seed_ranked.py          # after `staging.py server up` (or `win build`)

One player per rank, Bronze 3 to Legendary, and three still placing. Each gets a rating history that
follows the server's Elo rules: sets against "ghost" opponents (uids with no account) that walk the
rating to the middle of its division from below, so the held rank is that division. Then
`admin ranked recompute` gives every rank from the history, as the server would.

Every seeded account logs in with its email (seed-<tier>-<division>@staging.test, see the table it
prints) and the password `staging-pass`. Running it again replaces the seeded players and their sets;
other accounts are left alone.
"""

from __future__ import annotations

import json
import os
import random
import subprocess
import sys
import tempfile
import urllib.request
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STAGE = ROOT / "run" / "staging"
PSQL = ROOT / "run" / "postgres-portable" / "pgsql" / "bin" / ("psql.exe" if os.name == "nt" else "psql")
PASSWORD = "staging-pass"

# common::ranked: division floors (Bronze 3 first), the start rating and the K schedule.
FLOORS = [None, 1050, 1100, 1150, 1200, 1250, 1300, 1350, 1400, 1450, 1510, 1570, 1625, 1750, 1875,
          2000, 2085, 2170, 2250]
TIERS = ["bronze", "silver", "gold", "emerald", "diamond", "master", "legendary"]
DEFAULT_RATING = 1400.0


def k_factor(n: int) -> float:
    return 32.0 if n >= 10 else 32.0 + (200.0 - 32.0) * (10 - n) / 10


def expected(own: float, opp: float) -> float:
    return 1.0 / (1.0 + 10 ** ((opp - own) / 400.0))


def rank_name(i: int) -> tuple[str, int | None]:
    tier = TIERS[min(i // 3, 6)]
    return tier, (None if tier == "legendary" else 3 - i % 3)


def target(i: int) -> float:
    """The middle of rank i's rating range."""
    lo = FLOORS[i] if FLOORS[i] is not None else 950
    hi = FLOORS[i + 1] if i + 1 < len(FLOORS) else 2400
    return (lo + hi) / 2


def history(goal: float, rng: random.Random, min_sets: int) -> list[tuple[float, float, float, float]]:
    """(rating_before, rating_after, opponent_rating, score) per set: down to 130 below the goal (or
    below the start), then up to it, so the last move is a climb and the rank shown is the goal's."""
    r, n, out = DEFAULT_RATING, 0, []
    low = min(goal, DEFAULT_RATING) - 130
    phase = "down"
    while True:
        opp = r + rng.uniform(-60, 60)
        # Climb only once K is at its floor, so the last steps are small and never pass the next floor.
        if phase == "down" and r <= low and n >= max(min_sets - 4, 10):
            phase = "up"
        aim = low if phase == "down" else goal
        score = 1.0 if r < aim else 0.0
        after = r + k_factor(n) * (score - expected(r, opp))
        out.append((r, after, opp, score))
        r, n = after, n + 1
        if phase == "up" and r >= goal - 12:
            return out
        if n > 400:
            raise SystemExit(f"no path to {goal}")


def placing_history(sets: int, rng: random.Random) -> list[tuple[float, float, float, float]]:
    r, out = DEFAULT_RATING, []
    for n in range(sets):
        opp = r + rng.uniform(-80, 80)
        score = float(rng.random() < 0.5)
        after = r + k_factor(n) * (score - expected(r, opp))
        out.append((r, after, opp, score))
        r = after
    return out


def sql_str(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


def psql(cfg: dict, sql: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False, encoding="utf-8") as f:
        f.write(sql)
        path = f.name
    try:
        out = subprocess.run(
            [str(PSQL), "-h", "127.0.0.1", "-p", str(cfg["pg_port"]), "-U", "pp", "-d", "brawl_staging",
             "-v", "ON_ERROR_STOP=1", "-q", "-A", "-t", "-w", "-f", path],
            env=dict(os.environ, PGPASSWORD=cfg["pg_password"]), capture_output=True, text=True, timeout=120)
    finally:
        os.unlink(path)
    if out.returncode != 0:
        raise SystemExit(f"psql failed:\n{out.stderr}")
    return out.stdout


def password_hash(cfg: dict) -> str:
    """A hash of PASSWORD in the accounts server's own format: sign one account up through the API."""
    existing = psql(cfg, "SELECT pw_hash FROM users WHERE email = 'seed-template@staging.test';").strip()
    if existing:
        return existing
    body = json.dumps({"email": "seed-template@staging.test", "password": PASSWORD,
                       "displayName": "seed template"}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{cfg['accounts_port']}/v1/auth/signup", data=body,
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=20) as resp:
        if resp.status != 201:
            raise SystemExit(f"signup: {resp.status}")
    return psql(cfg, "SELECT pw_hash FROM users WHERE email = 'seed-template@staging.test';").strip()


NAMES = ["Brick", "Pebble", "Acorn", "Tin", "Nickel", "Sterling", "Gilded", "Midas", "Bullion", "Jade",
         "Verdant", "Peridot", "Facet", "Prism", "Brilliant", "Sensei", "Grandmaster", "Overlord", "Mythic"]


def main() -> None:
    cfg = json.loads((STAGE / "config.json").read_text())
    rng = random.Random(14)
    pw_hash = password_hash(cfg)

    players = []  # (uid, email, name, code, history)
    for i in range(19):
        tier, div = rank_name(i)
        label = tier + (f"-{div}" if div else "")
        players.append((uuid.uuid4(), f"seed-{label}@staging.test", NAMES[i],
                        f"{NAMES[i][:4].upper()}#{100 + i}", history(target(i), rng, 14)))
    for k, sets in enumerate((1, 3, 6)):
        name = ["Rookie", "Fresh", "Newcomer"][k]
        players.append((uuid.uuid4(), f"seed-placing-{sets}@staging.test", name,
                        f"{name[:4].upper()}#{200 + k}", placing_history(sets, rng)))

    start = datetime.now(timezone.utc) - timedelta(days=3)
    lines = [
        "BEGIN;",
        "DELETE FROM mm_matches WHERE match_id LIKE 'mode.ranked-seed-%';",
        "DELETE FROM users WHERE email LIKE 'seed-%@staging.test' AND email <> 'seed-template@staging.test';",
    ]
    for p, (uid, email, name, code, sets) in enumerate(players):
        lines.append(
            "INSERT INTO users (uid, email, email_verified_at, pw_hash, display_name, connect_code) VALUES "
            f"({sql_str(str(uid))}, {sql_str(email)}, now(), {sql_str(pw_hash)}, {sql_str(name)}, {sql_str(code)});")
        wins = losses = 0
        for n, (before, after, opp, score) in enumerate(sets):
            ghost = uuid.uuid4()
            mid = f"mode.ranked-seed-{p}-{n}"
            at = (start + timedelta(minutes=p * 400 + n * 3)).isoformat()
            winner = str(uid) if score > 0.5 else str(ghost)
            lines.append(
                "INSERT INTO mm_matches (match_id, mode, created_at, players, is_host, status, winner, ended_at) "
                f"VALUES ({sql_str(mid)}, 0, {sql_str(at)}, ARRAY[{sql_str(str(uid))}, {sql_str(str(ghost))}]::uuid[], "
                f"{sql_str(str(uid))}, 'COMPLETE', {sql_str(winner)}, {sql_str(at)});")
            lines.append(
                "INSERT INTO rating_events (uid, match_id, opponent, score, rating_before, rating_after, "
                "opponent_rating, k, sets_before, created_at) VALUES "
                f"({sql_str(str(uid))}, {sql_str(mid)}, {sql_str(str(ghost))}, {score}, {before}, {after}, {opp}, "
                f"{k_factor(n)}, {n}, {sql_str(at)});")
            wins += score > 0.5
            losses += score < 0.5
        lines.append(
            "INSERT INTO ratings (uid, rating, sets_played, wins, losses) VALUES "
            f"({sql_str(str(uid))}, {sets[-1][1]}, {len(sets)}, {wins}, {losses});")
    lines.append("COMMIT;")
    psql(cfg, "\n".join(lines) + "\n")

    out = subprocess.run([sys.executable, str(ROOT / "tools" / "staging" / "staging.py"), "server", "admin",
                          "ranked", "recompute"], capture_output=True, text=True)
    print(out.stdout.strip() or out.stderr.strip())

    rows = psql(cfg, "SELECT u.email, u.display_name, u.connect_code, round(r.rating::numeric, 1), r.sets_played, "
                     "coalesce(r.rank::text, '-') FROM users u JOIN ratings r USING (uid) "
                     "WHERE u.email LIKE 'seed-%' ORDER BY r.rating DESC;")
    print(f"\n{'rank':<12} {'email':<34} {'name':<12} {'code':<9} {'rating':>7} sets")
    for line in rows.strip().splitlines():
        email, name, code, rating, sets, rank = line.split("|")
        shown = "placing" if rank == "-" else " ".join(str(x) for x in rank_name(int(rank)) if x is not None)
        print(f"{shown:<12} {email:<34} {name:<12} {code:<9} {rating:>7} {sets}")
    print(f"\npassword for every seeded account: {PASSWORD}")


if __name__ == "__main__":
    main()
