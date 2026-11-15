"""Seeds the staging database with made-up players, so the launcher shows full screens against the
staging server (docs/staging.md, "Fake players"). Staging only: nothing here runs against production.

    python tools/staging/staging.py server seed-fake            # after `server up`
    python tools/staging/staging.py server seed-fake --remove   # take it all out again

It makes 60 fake accounts (fake-<name>@staging.test, password `staging-pass`) with profiles
(country, mains, a few socials), Ranked ratings at every tier with their history, and Unranked,
Direct, room and 2v2 matches among them. Every real account with a connect code gets 8 fake friends,
2 friend requests from fakes and 2 to fakes, and about 40 matches against fakes over the last two
weeks (Ranked sets with their rating changes, a few against the same player). The Ranked history
follows the server's Elo rules and `admin ranked recompute` gives the ranks from it; `admin unranked
rebuild` rates the Unranked games. `queue_searches` gets two weeks of searches, so each queue's wait
by hour of day is a few tens of seconds most hours and long or "never" late at night.

It also writes `run/staging/fake-state.json`: the fakes online, their public rooms, Brawl Mods rooms
and live games, which accounts adds to what it serves while `STAGING_FAKE_STATE` names the file
(staging.py sets it for the staging server; accounts/src/staging_fake.rs).

Running it again replaces every fake row; `--remove` deletes the fake accounts and every row that
names one, puts back the real accounts' ratings as they were without the fake sets, and deletes the
state file. Accounts from seed_ranked.py (seed-*@staging.test) are left out unless
--include-seed-ranked: fake sets would move their ratings off the rank each one stands for.
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import random
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import staging  # noqa: E402  (the paths and the admin binary staging.py uses)

PASSWORD = "staging-pass"
FAKE_LIKE = "fake-%@staging.test"
TEMPLATE = "fake-template@staging.test"
NS = uuid.UUID("5c1e7a60-0f3a-4b8e-9d5e-0b5f0a7e5eed")  # fake uids are uuid5(NS, name): the same every run
ROOM_LETTERS = "BCDFGHJKLMNPQRSTVWXZ"

# --------------------------------------------------------------------------- the server's rules

DEFAULT_RATING = 1400.0
RANK_FLOORS = [float("-inf"), 1050, 1100, 1150, 1200, 1250, 1300, 1350, 1400, 1450, 1510, 1570, 1625,
               1750, 1875, 2000, 2085, 2170, 2250]
TIERS = ["bronze", "silver", "gold", "emerald", "diamond", "master", "legendary"]
PLACEMENT_SETS = 5


def k_ranked(n: int) -> float:  # common::ranked::k_factor
    return 32.0 if n >= 10 else 32.0 + (200.0 - 32.0) * (10 - n) / 10


def k_unranked(n: int) -> float:  # common::unranked::k_factor (2v2 too)
    return 32.0 if n >= 30 else 32.0 + (160.0 - 32.0) * (30 - n) / 30


def expected(own: float, opp: float) -> float:
    return 1.0 / (1.0 + 10 ** ((opp - own) / 400.0))


def updated(own: float, n: int, opp: float, score: float) -> float:
    """common::ranked::updated: at least 1 point either way."""
    change = k_ranked(n) * (score - expected(own, opp))
    return own + (max(change, 1.0) if score > 0.5 else min(change, -1.0))


def rank_of(rating: float) -> int:
    return bisect.bisect_right(RANK_FLOORS, rating) - 1


def tier_label(rank: int) -> str:
    tier = TIERS[min(rank // 3, 6)]
    return tier if tier == "legendary" else f"{tier} {3 - rank % 3}"


# Brawl's gmCharacterKind of each stock key (launcher/src/renderer/lib/utils.ts characterStockKeys).
CHAR_ID = {
    "mario": 0x00, "donkey_kong": 0x01, "link": 0x02, "samus": 0x03, "zero_suit_samus": 0x04, "yoshi": 0x05,
    "kirby": 0x06, "fox": 0x07, "pikachu": 0x08, "luigi": 0x09, "captain_falcon": 0x0A, "ness": 0x0B,
    "bowser": 0x0C, "peach": 0x0D, "zelda": 0x0E, "sheik": 0x0F, "ice_climbers": 0x10, "marth": 0x13,
    "mr_game_and_watch": 0x14, "falco": 0x15, "ganondorf": 0x16, "wario": 0x17, "meta_knight": 0x18,
    "pit": 0x19, "olimar": 0x1A, "lucas": 0x1B, "diddy_kong": 0x1C, "charizard": 0x1E, "squirtle": 0x20,
    "ivysaur": 0x22, "king_dedede": 0x23, "lucario": 0x24, "ike": 0x25, "rob": 0x26, "jigglypuff": 0x27,
    "toon_link": 0x28, "wolf": 0x29, "snake": 0x2A, "sonic": 0x2B, "roy": 0x32, "mewtwo": 0x33, "knuckles": 0x35,
}
# P+ stage ids (launcher/src/common/stages.ts): Ranked's starters and counterpicks, then more.
STARTERS = [0x01, 0x04, 0x09, 0x21, 0x2E]
COUNTERPICKS = [0x0C, 0x1F, 0x23, 0x2D]
OTHER_STAGES = [0x02, 0x13, 0x2A, 0x1D, 0x0D, 0x2F, 0x29, 0x0E, 0x30, 0x2C]
BRAWL_DISC = "a56ffa9437ddf80f4e4fa9c9bfddc7094d216483"  # launcher/src/dolphin/brawl_disc.ts

# --------------------------------------------------------------------------- the fakes

# name, code, mains, country, target rating (None: placing / no Ranked), Ranked activity, socials
ROSTER = [
    ("sosa", "SOSA#160", ["jigglypuff", "marth"], "us", 2390, 9, {"twitter": "sosa_pm", "twitch": "sosapm"}),
    ("The Doctor", "TDOC#795", ["mario"], "us", 2335, 9, {"youtube": "@thedoctorpm"}),
    ("Veka", "VEKA#31", ["marth", "roy"], "fr", 2285, 8, {}),
    ("CerealKiD69", "PEE#590", ["wolf"], "", 2170, 8, {"discord": "cerealkid"}),
    ("Cloudburst", "AZPM#187", ["squirtle", "ivysaur", "charizard"], "us", 2120, 8, {"twitch": "cloudburstpm"}),
    ("SorryInAdvance", "MASH#952", ["ice_climbers"], "ca", 2080, 7, {}),
    ("Fritze107", "FRTZ#627", ["olimar"], "de", 2050, 7, {"twitter": "fritze107"}),
    ("MatzoBallin", "GELT#202", ["snake", "falco"], "us", 2020, 7, {}),
    ("Mr. Mario", "SGS#324", ["mario", "luigi"], "it", 1960, 6, {}),
    ("Peachy", "PEAC#33", ["peach", "zelda"], "jp", 1920, 6, {"instagram": "peachy.pm"}),
    ("Knuckles", "KNUX#127", ["knuckles", "sonic"], "us", 1870, 6, {}),
    ("Ferrum", "FERR#207", ["ike"], "br", 1820, 6, {"twitch": "ferrumssb"}),
    ("tetrahedra", "TETR#12", ["meta_knight"], "de", 1770, 5, {}),
    ("MayoTayo1", "TOES#683", ["sonic"], "us", 1720, 5, {"tiktok": "mayotayo"}),
    ("Zephyr", "ZEPH#418", ["falco", "fox"], "gb", 1700, 4, {}),
    ("IMPOSSIBLE", "IMPY#5", ["roy"], "mx", 1680, 5, {}),
    ("KoF_Rise", "HONE#802", ["snake"], "kr", 1645, 4, {}),
    ("Ozakai", "OZAK#924", ["wolf", "fox"], "", 1600, 4, {}),
    ("SanjiWatsuki", "SANJ#964", ["marth"], "", 1575, 4, {}),
    ("Bluejay", "BLUE#330", ["falco"], "gb", 1545, 4, {}),
    ("Lhasheil", "LSL#488", ["lucario"], "jp", 1525, 3, {}),
    ("mintleaf", "MINT#71", ["ivysaur", "squirtle"], "ca", 1500, 3, {}),
    ("Gale", "GALE#650", ["pit"], "au", 1490, 3, {}),
    ("E-C", "EC#319", ["fox"], "", 1480, 3, {}),
    ("iPunch", "IPU#23", ["mewtwo"], "se", 1465, 3, {}),
    ("Lucas", "LUCA#5", ["lucas", "ness"], "se", 1440, 3, {}),
    ("roy jones jr", "BOOM#216", ["roy", "ike"], "us", 1420, 3, {}),
    ("kimchi", "KIM#45", ["pikachu"], "kr", 1400, 3, {}),
    ("wobbly", "WOB#58", ["ice_climbers"], "us", 1385, 3, {}),
    ("Pixel", "PIXL#91", ["mr_game_and_watch", "rob"], "nl", 1375, 2, {}),
    ("chillguy", "CHIL#88", ["lucario", "toon_link"], "us", 1365, 2, {}),
    ("Metaternal", "META#150", ["meta_knight", "kirby"], "us", 1345, 2, {}),
    ("zenkraken", "ZEN#612", ["donkey_kong"], "nz", 1330, 2, {}),
    ("Ness Main", "NESS#404", ["ness"], "", 1315, 2, {}),
    ("Wario Fan", "WARI#77", ["wario"], "us", 1290, 2, {}),
    ("cartman", "KAY#709", ["king_dedede"], "us", 1275, 2, {}),
    ("tomato", "TOMA#13", ["yoshi", "peach"], "es", 1255, 2, {}),
    ("grapejuice", "GRAP#506", ["diddy_kong"], "ph", 1240, 2, {}),
    ("Mochi", "MOCH#27", ["kirby", "jigglypuff"], "jp", 1215, 2, {}),
    ("Pebblez", "PEBB#330", ["donkey_kong", "bowser"], "pl", 1225, 2, {}),
    ("NoJohns", "NOJO#100", ["captain_falcon"], "us", 1205, 2, {}),
    ("Shmoopy", "SHMO#44", ["zero_suit_samus", "samus"], "cl", 1185, 2, {}),
    ("dashdancer", "DASH#808", ["sheik", "zelda"], "ar", 1165, 1, {}),
    ("Bucket", "BUCK#2", ["link"], "ie", 1140, 1, {}),
    ("ledgehog", "LEDG#222", ["ganondorf"], "us", 1120, 1, {}),
    ("Sunny D", "SUNY#73", ["yoshi"], "us", 1095, 1, {}),
    ("waveland", "WAVE#64", ["samus"], "fi", 1075, 1, {}),
    ("Soggy", "SOGG#19", ["squirtle"], "us", 1060, 1, {}),
    ("PlatDrop", "PLAT#55", ["toon_link", "link"], "pt", 1050, 1, {}),
    ("Kirbo", "KIRB#300", ["kirby"], "br", 1030, 1, {}),
    ("Pudding", "PUDD#81", ["jigglypuff"], "us", 1010, 1, {}),
    ("l-cancel", "LCAN#1", ["charizard"], "ca", 990, 1, {}),
    ("Tumbleweed", "TUMB#76", ["bowser", "ganondorf"], "us", 960, 1, {}),
    ("freshman", "FRSH#12", ["pikachu"], "us", None, 2, {}),
    ("newgrass", "NEWG#33", ["lucas"], "ca", None, 3, {}),
    ("Rook", "ROOK#9", ["rob"], "gb", None, 4, {}),
    ("CasualCarl", "CARL#420", ["king_dedede", "wario"], "us", None, 0, {}),
    ("partyhat", "PRTY#7", ["random"], "", None, 0, {}),
    ("ItemsOnly", "ITEM#99", ["kirby"], "de", None, 0, {}),
    ("Sakurai Fan", "SAKU#64", ["pit", "olimar"], "jp", None, 0, {}),
]

# What the online fakes do now (fake-state.json). Rooms: (host, others, max players, mode, status,
# friends-only, ruleset key, room name). The host's room name is stored on their account too.
ROOMS = [
    ("Peachy", ["Ness Main", "Wario Fan"], 4, "ffa", "waiting", True, None, None),
    ("chillguy", ["cartman"], 3, "ffa", "waiting", False, "alc", "chill ffa, no camping"),
    ("Ferrum", ["tetrahedra"], 2, "1v1", "in-game", False, "catfight", "bo5 money match"),
    ("Lhasheil", ["SanjiWatsuki"], 2, "1v1", "in-game", False, None, "friday netplay"),
    ("mintleaf", ["wobbly", "Bluejay", "kimchi"], 4, "teams", "in-game", False, "doubles", "doubles practice"),
    ("Ozakai", [], 2, "1v1", "waiting", False, "alc", None),
]
# Live games: (mode, players, game number, set score, stage, state, seconds into the game, spectators)
LIVE = [
    ("ranked", ["sosa", "The Doctor"], 3, (1, 1), 0x2E, "playing", 161, 14),
    ("ranked", ["Veka", "Cloudburst"], 1, (0, 0), 0x01, "playing", 58, 6),
    ("direct", ["Knuckles", "E-C"], 2, (1, 0), 0x21, "playing", 76, 3),
    ("unranked", ["iPunch", "MayoTayo1"], 4, (2, 1), 0x23, "playing", 37, 2),
    ("room", ["Ferrum", "tetrahedra"], 1, (0, 0), 0x2D, "playing", 112, 1),
    ("unranked", ["IMPOSSIBLE", "KoF_Rise"], 2, (0, 1), 0x13, "between", 0, 1),
    ("twos", ["CerealKiD69", "SorryInAdvance", "Fritze107", "MatzoBallin"], 1, (0, 0), 0x1F, "playing", 12, 0),
]
IN_LAUNCHER = ["Lucas", "Mr. Mario", "tomato"]
# Brawl Mods rooms: (host, game name, build hash, disc, disc label, SD card, codes, players, open, name)
MOD_ROOMS = [
    ("zenkraken", "REX Release 0.5 Hotfix 1", BRAWL_DISC, "RSBE01 Rev 1", True,
     ["QOL - Faster Boot Time", "Stage Hazards Off", "Crowd Cheers Off"], 2, 4, None),
    ("Metaternal", "Tatsunoko vs. Capcom", "6a1d4f0c2b9e8d7a5c3f1e0b9a8d7c6e5f4a3b2c", "STKE08", False,
     ["Unlock All Characters", "Widescreen", "60 fps menus", "No Music"], 1, 4, None),
    ("grapejuice", "Mario Kart Wii", "479693f2a8c7e1d9b3f60a5c4e2d1b0a9f8e7d6c", "RMCE01", False,
     [], 3, 4, "kart night"),
]
RULESETS = {
    # key: (owner, ruleset as POST /v1/rulesets takes it)
    "alc": ("chillguy", {"name": "Auto L Cancel v4", "bestOf": 0, "stageChoice": "loser-picks-list"}),
    "catfight": ("Ferrum", {"name": "Catfight BO5", "bestOf": 5, "strikes": [1, 2, 1], "bans": 1, "dsr": True}),
    "doubles": ("mintleaf", {"name": "Friday Doubles", "bestOf": 3, "strikes": [1, 2, 1], "dsr": True}),
}


class Fake:
    def __init__(self, i: int, row: tuple) -> None:
        name, code, mains, country, target, activity, socials = row
        self.i = i
        self.name, self.code, self.mains, self.country = name, code, mains, country
        self.target, self.activity, self.socials = target, activity, socials
        self.uid = uuid.uuid5(NS, name)
        slug = "".join(c for c in name.lower() if c.isalnum()) or f"p{i}"
        self.email = f"fake-{slug}@staging.test"
        self.rating, self.sets, self.wins, self.losses = DEFAULT_RATING, 0, 0, 0
        self.twos: tuple[float, int] | None = None
        self.created_at: datetime | None = None
        self.show_country = True
        self.show_main = True
        self.room_name: str | None = None
        self.mods_room_name: str | None = None

    def char(self, rng: random.Random) -> int:
        playable = [m for m in self.mains if m in CHAR_ID] or ["mario"]
        pick = playable[0] if rng.random() < 0.7 else rng.choice(playable)
        return CHAR_ID[pick]


class Real:
    """A real account the fakes play and befriend."""

    def __init__(self, row: dict) -> None:
        self.uid = uuid.UUID(row["uid"])
        self.name = row["name"]
        self.code = row["code"]
        self.rating = row["rating"] if row["rating"] is not None else DEFAULT_RATING
        self.sets = row["sets"] or 0
        self.wins = row["wins"] or 0
        self.losses = row["losses"] or 0
        self.had_rating = row["rating"] is not None
        self.start = self.rating  # how strong they play against the fakes: as strong as they are now
        self.last_event = datetime.fromisoformat(row["last_event"]) if row["last_event"] else None
        self.twos = (row["twos"], row["twos_games"]) if row["twos"] is not None else None
        self.twos_seed = row["unranked"] if row["unranked"] is not None else self.rating
        self.mains = ["mario"]

    def char(self, rng: random.Random) -> int:
        return rng.choice([0x14, 0x23, 0x1B, 0x17])


# --------------------------------------------------------------------------- database


class Db:
    def __init__(self, cfg: dict, pg_bin: Path) -> None:
        self.cfg = cfg
        self.psql = pg_bin / f"psql{staging.EXE}"
        if not self.psql.exists():
            raise SystemExit(f"no psql at {self.psql}")
        self.database = cfg.get("pg_database", "brawl_staging")

    def url(self) -> str:
        return (f"postgres://pp:{self.cfg['pg_password']}@127.0.0.1:{self.cfg['pg_port']}/{self.database}")

    def run(self, sql: str) -> str:
        fd, path = tempfile.mkstemp(suffix=".sql")
        with os.fdopen(fd, "wb") as f:
            f.write(sql.encode("utf-8"))
        try:
            out = subprocess.run(
                [str(self.psql), "-h", "127.0.0.1", "-p", str(self.cfg["pg_port"]), "-U", "pp", "-d", self.database,
                 "-v", "ON_ERROR_STOP=1", "-q", "-A", "-t", "-w", "-f", path],
                env=dict(os.environ, PGPASSWORD=self.cfg["pg_password"], PGCONNECT_TIMEOUT="10"),
                capture_output=True, timeout=600)
        finally:
            os.unlink(path)
        if out.returncode != 0:
            raise SystemExit(f"psql failed:\n{out.stderr.decode(errors='replace')[-4000:]}")
        return out.stdout.decode("utf-8", errors="replace")

    def json(self, query: str):
        text = self.run(f"SELECT coalesce(json_agg(t), '[]'::json) FROM ({query}) t;").strip()
        return json.loads(text or "[]")


def q(v) -> str:
    """A SQL literal."""
    if v is None:
        return "NULL"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, datetime):
        return f"'{v.isoformat()}'::timestamptz"
    if isinstance(v, uuid.UUID):
        return f"'{v}'::uuid"
    if isinstance(v, (dict, list)) and not (isinstance(v, list) and v and isinstance(v[0], uuid.UUID)):
        return "'" + json.dumps(v).replace("'", "''") + "'::jsonb"
    return "'" + str(v).replace("'", "''") + "'"


def uuids(vs) -> str:
    return "ARRAY[" + ", ".join(f"'{v}'" for v in vs) + "]::uuid[]"


def smallints(vs) -> str:
    return ("ARRAY[" + ", ".join(str(int(v)) for v in vs) + "]::smallint[]") if vs is not None else "NULL"


class Sql:
    """Rows to insert, batched per table."""

    def __init__(self) -> None:
        self.rows: dict[str, tuple[str, list[str]]] = {}

    def add(self, table: str, cols: str, values: str) -> None:
        self.rows.setdefault(table + "|" + cols, (table, []))[1].append(values)

    ORDER = ["users", "profiles", "mm_matches"]  # what the others' foreign keys point at

    def text(self) -> str:
        out = []
        first = lambda kv: self.ORDER.index(kv[1][0]) if kv[1][0] in self.ORDER else len(self.ORDER)  # noqa: E731
        for key, (table, rows) in sorted(self.rows.items(), key=first):
            cols = key.split("|", 1)[1]
            for i in range(0, len(rows), 400):
                out.append(f"INSERT INTO {table} ({cols}) VALUES\n" + ",\n".join(f"({r})" for r in rows[i:i + 400]) + ";")
        return "\n".join(out) + "\n"


def ruleset_lineage(key: str) -> uuid.UUID:
    """The fake rulesets' lineage, set after publishing, so they are found again without an owner."""
    return uuid.uuid5(NS, "ruleset:" + key)


def remove_sql(everything: bool) -> str:
    """Deletes the fakes and every row naming one; real accounts' ratings lose the fake sets' changes.
    Not `everything` (a new seed): the template account and the published rulesets stay (each
    account may publish only 10 an hour); their owner goes, and the seed gives them back."""
    keep = "" if everything else f" AND email <> '{TEMPLATE}'"
    lineages = uuids(ruleset_lineage(k) for k in RULESETS)
    rulesets = "" if not everything else f"""
-- Published rulesets never go away except in an operator's transaction (0014_ruleset_plays.sql).
SET LOCAL pp.ruleset_admin = 'on';
CREATE TEMP TABLE fake_rulesets ON COMMIT DROP AS
  SELECT code FROM rulesets WHERE owner IN (SELECT uid FROM fake_uids) OR lineage = ANY({lineages});
DELETE FROM remembered_rulesets WHERE code IN (SELECT code FROM fake_rulesets);
DELETE FROM ruleset_recent WHERE code IN (SELECT code FROM fake_rulesets);
UPDATE ruleset_drafts SET based_on = NULL WHERE based_on IN (SELECT code FROM fake_rulesets);
DELETE FROM rulesets WHERE code IN (SELECT code FROM fake_rulesets);"""
    return f"""
BEGIN;
CREATE TEMP TABLE fake_uids ON COMMIT DROP AS SELECT uid FROM users WHERE email LIKE '{FAKE_LIKE}'{keep};
CREATE TEMP TABLE fake_matches ON COMMIT DROP AS
  SELECT match_id FROM mm_matches WHERE players && (SELECT coalesce(array_agg(uid), '{{}}') FROM fake_uids);
-- Real players' Ranked changes from fake sets, taken back (the fake sets came after their own).
CREATE TEMP TABLE real_ranked ON COMMIT DROP AS
  SELECT e.uid, sum(e.rating_after - e.rating_before) AS delta, count(*) AS n,
         count(*) FILTER (WHERE e.score > 0.5) AS w, count(*) FILTER (WHERE e.score < 0.5) AS l
    FROM rating_events e JOIN fake_matches USING (match_id)
   WHERE e.uid NOT IN (SELECT uid FROM fake_uids) GROUP BY e.uid;
UPDATE ratings r SET rating = r.rating - d.delta, sets_played = r.sets_played - d.n, wins = r.wins - d.w,
       losses = r.losses - d.l, updated_at = now()
  FROM real_ranked d WHERE r.uid = d.uid;
DELETE FROM ratings WHERE uid IN (SELECT uid FROM real_ranked) AND sets_played <= 0;
CREATE TEMP TABLE real_twos ON COMMIT DROP AS
  SELECT (p ->> 'uid')::uuid AS uid, sum((p ->> 'after')::float8 - (p ->> 'before')::float8) AS delta, count(*) AS n
    FROM twos_rated_games g JOIN fake_matches USING (match_id), jsonb_array_elements(g.players) p
   WHERE (p ->> 'uid')::uuid NOT IN (SELECT uid FROM fake_uids) GROUP BY 1;
UPDATE twos_ratings t SET rating = t.rating - d.delta, games = t.games - d.n, updated_at = now()
  FROM real_twos d WHERE t.uid = d.uid;
DELETE FROM twos_ratings WHERE uid IN (SELECT uid FROM real_twos) AND games <= 0;
DELETE FROM mm_matches WHERE match_id IN (SELECT match_id FROM fake_matches);
DELETE FROM queue_searches WHERE uid IN (SELECT uid FROM fake_uids);{rulesets}
DELETE FROM users WHERE uid IN (SELECT uid FROM fake_uids);
SELECT (SELECT count(*) FROM fake_uids) || ' accounts, ' || (SELECT count(*) FROM fake_matches) || ' matches';
COMMIT;
"""


# --------------------------------------------------------------------------- the accounts API


def api(cfg: dict, method: str, path: str, body: dict | None = None, token: str | None = None):
    req = urllib.request.Request(f"http://127.0.0.1:{cfg['accounts_port']}{path}",
                                 data=None if body is None else json.dumps(body).encode(), method=method,
                                 headers={"Content-Type": "application/json",
                                          **({"Authorization": f"Bearer {token}"} if token else {})})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return resp.status, json.loads(resp.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def password_hash(db: Db, cfg: dict) -> str:
    """A hash of PASSWORD in the server's own format, from a template account whose password the
    server set: a password reset with a token made here (sign-ups are limited per network)."""
    for email in (TEMPLATE, "seed-template@staging.test"):
        rows = db.json(f"SELECT pw_hash FROM users WHERE email = {q(email)} AND pw_hash LIKE '$argon2%'")
        if rows:
            return rows[0]["pw_hash"]
    uid, token = uuid.uuid5(NS, "template"), uuid.uuid4().hex
    db.run(f"""DELETE FROM users WHERE email = {q(TEMPLATE)};
INSERT INTO users (uid, email, email_verified_at, pw_hash, display_name)
     VALUES ({q(uid)}, {q(TEMPLATE)}, now(), '!', 'fake template');
INSERT INTO email_tokens (token_hash, uid, purpose, expires_at)
     VALUES (sha256({q(token)}::bytea), {q(uid)}, 'reset_password', now() + interval '10 minutes');""")
    status, body = api(cfg, "POST", "/v1/auth/password-reset/confirm", {"token": token, "newPassword": PASSWORD})
    rows = db.json(f"SELECT pw_hash FROM users WHERE uid = {q(uid)} AND pw_hash LIKE '$argon2%'")
    if not rows:
        raise SystemExit(f"setting the template's password failed: {status} {body} "
                         f"(is the staging accounts server up on port {cfg['accounts_port']}?)")
    return rows[0]["pw_hash"]


def publish_rulesets(db: Db, cfg: dict, fakes: dict[str, Fake]) -> dict[str, dict]:
    """The rooms' rulesets: {key: {code, name, version, hash, by, ruleset}}. Published by their fake
    owners through the API (the server makes the canonical JSON and hash), with a session row made
    here (no login, whose limit a few seeds in a row would reach); one kept from an earlier seed gets
    its owner back. A ruleset the server refuses is left out: those rooms play P+ default."""
    for key, (owner, ruleset) in RULESETS.items():
        lineage, uid = ruleset_lineage(key), fakes[owner].uid
        if db.json(f"SELECT code FROM rulesets WHERE lineage = {q(lineage)}"):
            db.run(f"BEGIN; SET LOCAL pp.ruleset_admin = 'on'; "
                   f"UPDATE rulesets SET owner = {q(uid)} WHERE lineage = {q(lineage)}; COMMIT;")
            continue
        token = uuid.uuid4().hex + uuid.uuid4().hex
        db.run(f"INSERT INTO sessions (token_hash, uid, expires_at, user_agent) VALUES "
               f"(sha256({q(token)}::bytea), {q(uid)}, now() + interval '10 minutes', 'seed_fake.py');")
        try:
            status, body = api(cfg, "POST", "/v1/rulesets", {"ruleset": {"format": 1, **ruleset}}, token)
        finally:
            db.run(f"DELETE FROM sessions WHERE token_hash = sha256({q(token)}::bytea);")
        if status != 201:
            print(f"  publishing {ruleset['name']!r} as {owner} failed ({status}): {body}")
            continue
        db.run(f"BEGIN; SET LOCAL pp.ruleset_admin = 'on'; "
               f"UPDATE rulesets SET lineage = {q(lineage)} WHERE code = {q(body['code'])}; COMMIT;")
    out = {}
    for key in RULESETS:
        rows = db.json(f"SELECT code, name, version, hash, publisher_name AS by, json FROM rulesets "
                       f"WHERE lineage = {q(ruleset_lineage(key))} ORDER BY version DESC LIMIT 1")
        if rows:
            out[key] = dict(rows[0], ruleset=json.loads(rows[0]["json"]))
    return out


# --------------------------------------------------------------------------- history


class History:
    def __init__(self, sql: Sql, rng: random.Random, now: datetime) -> None:
        self.sql, self.rng, self.now = sql, rng, now
        self.counter = 0xFA0000
        self.ranked_rows = 0
        self.matches = 0
        self.busy: dict[uuid.UUID, datetime] = {}  # each player's last Ranked set's end

    def mid(self, kind: str, t: datetime) -> str:
        self.counter += 1
        return f"mode.{kind}-{t.strftime('%Y-%m-%dT%H:%M:%S')}.{t.microsecond // 1000:03d}Z-{self.counter:x}"

    def match(self, match_id: str, mode: int, t: datetime, players: list, extra: dict | None = None) -> None:
        row = {"match_id": q(match_id), "mode": str(mode), "created_at": q(t), "players": uuids(players),
               "is_host": q(players[0]), "region": q("other"), "status": q("ASSIGNED"), **(extra or {})}
        self.sql.add("mm_matches", ", ".join(row), ", ".join(row.values()))
        self.matches += 1

    def report(self, match_id: str, game: int, reporter, winner, stage: int, frames: int, players: list,
               at: datetime) -> None:
        self.sql.add("game_reports",
                     "match_id, game_index, reporter, winner, stage_id, duration_frames, detail, created_at",
                     ", ".join([q(match_id), str(game), q(reporter), q(winner), str(stage), str(frames),
                                q({"players": players}), q(at)]))

    def game_times(self, t: datetime, n: int) -> list[tuple[datetime, int]]:
        """Each game's end and length in frames, from a match made at t."""
        out, at = [], t + timedelta(seconds=self.rng.uniform(25, 50))
        for _ in range(n):
            secs = self.rng.uniform(75, 260)
            at += timedelta(seconds=secs)
            out.append((at, int(secs * 60)))
            at += timedelta(seconds=self.rng.uniform(30, 60))
        return out

    def ranked(self, a, b, t: datetime, a_wins: bool, ghost: tuple | None = None) -> datetime | None:
        """A Ranked set (Bo3) between a and b, rated with each one's standing now. `ghost`:
        (uid, rating) of an opponent without an account (a's early climb). A player's sets never
        overlap: one still being played moves this one after it. Returns the set's end (None: it
        would end in the future, and is not played)."""
        rng = self.rng
        b_uid, b_rating, b_sets = (ghost[0], ghost[1], 30) if ghost else (b.uid, b.rating, b.sets)
        free = max(self.busy.get(a.uid, t), self.busy.get(b_uid, t))
        if free > t:
            t = free + timedelta(seconds=rng.uniform(20, 90))
        if t > self.now - timedelta(minutes=25):
            return None
        match_id = self.mid("ranked", t)
        # True: the set's winner takes the game. 2-0 or 2-1, the winner taking the last game.
        order = [True, False] if rng.random() < 0.4 else [True]
        rng.shuffle(order)
        order.append(True)
        winners = [(a.uid if (w == a_wins) else b_uid) for w in order]
        times = self.game_times(t, len(winners))
        stages = [rng.choice(STARTERS)] + [rng.choice(STARTERS + COUNTERPICKS) for _ in winners[1:]]
        ca, cb = a.char(rng), (b.char(rng) if not ghost else rng.choice(list(CHAR_ID.values())))
        for g, (winner, (at, frames), stage) in enumerate(zip(winners, times, stages), start=1):
            players = [{"uid": str(u), "characterId": c, "stocksRemaining": (rng.randint(1, 3) if u == winner else 0),
                        "damage": round(rng.uniform(40, 160), 1)} for u, c in ((a.uid, ca), (b_uid, cb))]
            for i, reporter in enumerate((a.uid, b_uid)):
                if ghost and reporter == b_uid:
                    continue
                self.report(match_id, g, reporter, winner, stage, frames, players, at + timedelta(milliseconds=150 * i + 40))
        end = times[-1][0] + timedelta(milliseconds=400)
        winner = a.uid if a_wins else b_uid
        self.match(match_id, 0, t, [a.uid, b_uid], {
            "status": q("COMPLETE"), "winner": q(winner), "ended_at": q(end), "stages": smallints(STARTERS)})
        sa = 1.0 if a_wins else 0.0
        a_after = updated(a.rating, a.sets, b_rating, sa)
        b_after = updated(b_rating, b_sets, a.rating, 1.0 - sa)
        rows = [(a.uid, b_uid, sa, a.rating, a_after, b_rating, a.sets)]
        if not ghost:
            rows.append((b.uid, a.uid, 1.0 - sa, b.rating, b_after, a.rating, b.sets))
        for uid, opp, score, before, after, opp_rating, n in rows:
            self.sql.add("rating_events",
                         "uid, match_id, opponent, score, rating_before, rating_after, opponent_rating, k, sets_before, created_at",
                         ", ".join([q(uid), q(match_id), q(opp), str(score), repr(before), repr(after), repr(opp_rating),
                                    repr(k_ranked(n)), str(n), q(end)]))
            self.ranked_rows += 1
        for p, score, after in ((a, sa, a_after), (None if ghost else b, 1.0 - sa, b_after)):
            if p is None:
                continue
            p.rating, p.sets = after, p.sets + 1
            p.wins += score > 0.5
            p.losses += score < 0.5
            self.busy[p.uid] = end
        return end

    def session(self, mode: int, kind: str, a, b, t: datetime, games: int) -> None:
        """An Unranked or Direct match: game after game, each reported by both."""
        rng = self.rng
        match_id = self.mid(kind, t)
        self.match(match_id, mode, t, [a.uid, b.uid])
        pool = STARTERS + COUNTERPICKS + OTHER_STAGES
        ca, cb = a.char(rng), b.char(rng)
        for g, (at, frames) in enumerate(self.game_times(t, games), start=1):
            winner = rng.choice((a.uid, b.uid))
            players = [{"uid": str(u), "characterId": c, "stocksRemaining": (rng.randint(1, 3) if u == winner else 0),
                        "damage": round(rng.uniform(40, 160), 1)} for u, c in ((a.uid, ca), (b.uid, cb))]
            stage = rng.choice(pool)
            for i, reporter in enumerate((a.uid, b.uid)):
                self.report(match_id, g, reporter, winner, stage, frames, players, at + timedelta(milliseconds=150 * i + 40))

    def room(self, players: list, t: datetime, mode: str, code: str, ruleset: dict | None, games: int) -> None:
        """A room's match (mode 3): every player reports every game with everyone's place."""
        rng = self.rng
        match_id = self.mid(f"room-{code}", t)
        ports = list(range(1, len(players) + 1))
        teams = [i % 2 for i in range(len(players))] if mode == "teams" else None
        extra = {"room_code": q(code), "room_mode": q(mode), "ports": smallints(ports), "teams": smallints(teams)}
        if ruleset:
            extra.update({"ruleset_code": q(ruleset["code"]), "ruleset_name": q(ruleset["name"]),
                          "ruleset_version": str(ruleset["version"]), "ruleset_hash": q(ruleset["hash"])})
        self.match(match_id, 3, t, [p.uid for p in players], extra)
        chars = [p.char(rng) for p in players]
        for g, (at, frames) in enumerate(self.game_times(t, games), start=1):
            if mode == "teams":
                win_team = rng.randint(0, 1)
                places = [1 if teams[i] == win_team else 2 for i in range(len(players))]
            else:
                order = list(range(len(players)))
                rng.shuffle(order)
                places = [order.index(i) + 1 for i in range(len(players))]
            winner = players[places.index(1)].uid
            detail = []
            for i, p in enumerate(players):
                d = {"uid": str(p.uid), "port": ports[i], "place": places[i], "characterId": chars[i],
                     "stocksRemaining": rng.randint(1, 3) if places[i] == 1 else 0}
                if teams:
                    d["team"] = teams[i]
                detail.append(d)
            stage = rng.choice(STARTERS + COUNTERPICKS)
            for i, p in enumerate(players):
                self.report(match_id, g, p.uid, winner, stage, frames, detail, at + timedelta(milliseconds=120 * i + 40))

    def twos(self, players: list, pairs: list[int], t: datetime) -> None:
        """A 2v2 match (mode 5): players 0 and 1 against 2 and 3, rated per game (common::twos)."""
        rng = self.rng
        match_id = self.mid("twos", t)
        self.match(match_id, 5, t, [p.uid for p in players], {"pairs": smallints(pairs)})
        teams = [0, 0, 1, 1]
        team_pair = [pairs[0] != 0 and pairs[0] == pairs[1], pairs[2] != 0 and pairs[2] == pairs[3]]
        chars = [p.char(rng) for p in players]
        score = [0, 0]
        games = []
        while max(score) < 2:
            w = rng.randint(0, 1)
            score[w] += 1
            games.append(w)
        for g, (w, (at, frames)) in enumerate(zip(games, self.game_times(t, len(games))), start=1):
            detail = [{"uid": str(p.uid), "port": i + 1, "team": teams[i], "place": 1 if teams[i] == w else 2,
                       "characterId": chars[i], "stocksRemaining": rng.randint(1, 3) if teams[i] == w else 0}
                      for i, p in enumerate(players)]
            winner = players[0 if w == 0 else 2].uid
            stage = rng.choice(STARTERS + COUNTERPICKS)
            for i, p in enumerate(players):
                self.report(match_id, g, p.uid, winner, stage, frames, detail, at + timedelta(milliseconds=120 * i + 40))
            # The first report rates the game: everyone from their 2v2 standing (their Unranked or
            # Ranked rating at their first 2v2 game).
            before = []
            for p in players:
                if p.twos is None:
                    p.twos = (getattr(p, "twos_seed", None) or p.rating, 0)
                before.append(p.twos)
            strength = [(before[0][0] + before[1][0]) / 2 + (50.0 if team_pair[0] else 0.0),
                        (before[2][0] + before[3][0]) / 2 + (50.0 if team_pair[1] else 0.0)]
            e0 = expected(strength[0], strength[1])
            seats = []
            for i, p in enumerate(players):
                e = e0 if teams[i] == 0 else 1.0 - e0
                s = 1.0 if teams[i] == w else 0.0
                after = (before[i][0] + k_unranked(before[i][1]) * (s - e), before[i][1] + 1)
                seats.append({"uid": str(p.uid), "team": teams[i], "left": False, "eliminated": False,
                              "pair": team_pair[teams[i]], "before": before[i][0], "after": after[0]})
                p.twos = after
            self.sql.add("twos_rated_games", "match_id, game_index, winner_team, players, created_at",
                         ", ".join([q(match_id), str(g), str(w), q(seats), q(at + timedelta(milliseconds=40))]))


def climb(h: History, f: Fake, start: datetime, end: datetime) -> None:
    """A fake's early Ranked sets, against players since gone (no account), walking its rating to
    its target: wins below it, losses above it, now and then the other way."""
    rng = h.rng
    t = start
    for n in range(400):
        if (n >= 12 and abs(f.rating - f.target) < 14) or t > end:
            return
        win = f.rating < f.target
        if rng.random() < (0.12 if n >= 10 else 0.03):
            win = not win
        t = h.ranked(f, None, t, win, ghost=(uuid.uuid4(), f.rating + rng.uniform(-70, 70)))
        # A session's sets a minute or two apart, then a day or so off.
        t += timedelta(minutes=rng.uniform(1, 3)) if rng.random() < 0.85 else timedelta(hours=rng.uniform(8, 26))


def nearest(pool: list, rating: float, k: int, exclude=()) -> list:
    return sorted((p for p in pool if p not in exclude), key=lambda p: abs(p.rating - rating))[:k]


# --------------------------------------------------------------------------- queue searches


def queue_searches(sql: Sql, fakes: list[Fake], rng: random.Random, now: datetime) -> int:
    """Two weeks of finished searches (mm's `queue_searches` rows): many in the evenings (UTC 0-5
    and 17-23), few late at night. Ranked at UTC 11-14 and 2v2 at UTC 9-15 mostly gave up, so
    their median is "never"; Unranked at UTC 12-13 has too few searches to tell."""
    activity = [10, 10, 9, 8, 7, 5, 4, 3, 2, 2, 2, 1, 1, 1, 1, 2, 4, 6, 8, 9, 10, 11, 11, 11]
    modes = {
        # mode: (searches a day at activity 10, median wait when matched, chance matched by hour)
        "unranked": (14, 22.0, lambda h: 0.92),
        "ranked": (11, 38.0, lambda h: 0.2 if 11 <= h <= 14 else 0.85),
        "twos": (4, 55.0, lambda h: 0.25 if 9 <= h <= 15 else 0.75),
    }
    last_end: dict[uuid.UUID, datetime] = {}
    n = 0
    start = now - timedelta(days=13, hours=23)
    for day in range(14):
        for hour in range(24):
            for mode, (per_day, median, matched_p) in modes.items():
                if mode == "unranked" and hour in (12, 13):
                    count = 1 if day in (3, 10) else 0
                else:
                    lam = per_day * activity[hour] / 10.0
                    count = max(0, int(rng.gauss(lam, lam ** 0.5)))
                for _ in range(count):
                    began = start + timedelta(days=day, hours=hour, seconds=rng.uniform(0, 3600))
                    if began > now - timedelta(minutes=5):
                        continue
                    matched = rng.random() < matched_p(hour)
                    if matched:
                        wait = rng.lognormvariate(0, 0.55) * median * (1.4 if activity[hour] < 4 else 1.0)
                    else:
                        wait = rng.uniform(40, 420)
                    ended = began + timedelta(seconds=wait)
                    for _ in range(20):  # a player not searching then
                        uid = rng.choice(fakes).uid
                        if last_end.get(uid, start - timedelta(days=1)) < began - timedelta(minutes=10):
                            break
                    last_end[uid] = ended
                    sql.add("queue_searches", "uid, mode, waited_ms, matched, requeue, ended_at",
                            ", ".join([q(uid), q(mode), str(int(wait * 1000)), q(matched), "false", q(ended)]))
                    n += 1
    return n


# --------------------------------------------------------------------------- live state


def room_ruleset(published: dict | None) -> dict:
    if not published:
        return {"code": None, "name": "P+ default", "version": 0, "by": "", "basedOn": None, "bestOf": 0,
                "stageChoice": "loser-picks", "alternateStages": True}
    r = published["ruleset"]
    return {"code": published["code"], "name": published["name"], "version": published["version"],
            "by": published["by"], "basedOn": None,
            "bestOf": r.get("bestOf", 0), "stageChoice": r.get("stageChoice", "loser-picks"),
            "stages": r.get("stages") or {"starters": STARTERS, "counterpicks": COUNTERPICKS},
            "strikes": r.get("strikes", []), "bans": r.get("bans", 0), "dsr": r.get("dsr", False)}


def room_code(rng: random.Random, used: set) -> str:
    while True:
        c = "".join(rng.choice(ROOM_LETTERS) for _ in range(4))
        if c not in used:
            used.add(c)
            return c


def live_state(fakes: dict[str, Fake], rulesets: dict, rng: random.Random, now: datetime) -> dict:
    used: set = set()
    rooms, friend_rooms, members = [], [], []
    room_of: dict[str, str] = {}
    for host, others, max_players, mode, status, friends, rs, _name in ROOMS:
        code = room_code(rng, used)
        names = [host] + others
        ps = [fakes[n] for n in names]
        entry = {
            "code": code, "host": host, "players": len(ps), "openSlots": max_players, "mode": mode,
            "status": status, "names": names, "uids": [str(p.uid) for p in ps],
            "joinable": len(ps) < max_players, "ruleset": room_ruleset(rulesets.get(rs) if rs else None),
            "name": fakes[host].room_name, "friends": friends,
        }
        (friend_rooms if friends else rooms).append(entry)
        for p in ps:
            room_of[p.name] = code
            members.append({"uid": str(p.uid), "code": code, "players": len(ps), "openSlots": max_players,
                            "mode": mode, "status": status, "joinable": len(ps) < max_players,
                            "host": p.name == host, "name": fakes[host].room_name})
    live = []
    for n, (mode, names, number, score, stage, state, clock, spectators) in enumerate(LIVE):
        ps = [fakes[x] for x in names]
        teams = len(ps) == 4
        wins = [score[0], score[0], score[1], score[1]] if teams else [score[0], score[1]]
        started = now - timedelta(seconds=clock + (number - 1) * 300 + 60)
        live.append({
            "id": f"fa{n + 1:010x}", "mode": mode, "room": room_of.get(names[0]) if mode == "room" else None,
            "isTeams": teams, "startedAt": started.isoformat().replace("+00:00", "Z"),
            "game": {"number": number, "place": number, "startedAt": (now - timedelta(seconds=clock)).isoformat()
                     .replace("+00:00", "Z"), "stageId": stage, "state": state},
            "players": [{"uid": str(p.uid), "displayName": p.name, "connectCode": p.code,
                         "characterId": CHAR_ID.get(p.mains[0], 0), "costume": 0,
                         "teamId": (i // 2) if teams else None, "wins": wins[i]} for i, p in enumerate(ps)],
            "spectators": spectators, "clock": clock,
        })
    mod_rooms, mod_members = [], []
    for host, game, disc, label, card, codes, players, open_, name in MOD_ROOMS:
        code = room_code(rng, used)
        h = fakes[host]
        build = {"hash": "".join(rng.choice("0123456789abcdef") for _ in range(64)), "name": game, "disc": disc,
                 "discLabel": label}
        if card:
            build["sd"] = "".join(rng.choice("0123456789abcdef") for _ in range(64))
        if codes:
            build["codes"] = codes
        mod_rooms.append({"code": code, "build": build, "hostName": host, "hostUid": str(h.uid),
                          "players": players, "open": open_, **({"name": name} if name else {})})
        mod_members.append({"uid": str(h.uid), "code": code, "host": True, "name": name})
    online_names = ([n for r in ROOMS for n in [r[0], *r[1]]] + [n for g in LIVE for n in g[1]] + IN_LAUNCHER
                    + [m[0] for m in MOD_ROOMS])
    players = []
    for n in dict.fromkeys(online_names):
        p = fakes[n]
        tag = {}
        if p.show_country and p.country:
            tag["country"] = p.country
        if p.show_main and p.mains:
            tag["main"] = p.mains[0]
        players.append({"uid": str(p.uid), "displayName": p.name, "connectCode": p.code, **tag})
    in_game = len({n for r in ROOMS for n in [r[0], *r[1]]} | {n for g in LIVE for n in g[1]}) + sum(m[7] for m in MOD_ROOMS)
    return {
        "_comment": "Staging only: written by tools/staging/seed_fake.py, read by accounts (STAGING_FAKE_STATE).",
        "online": in_game + 3,
        "queues": {"unranked": 2, "ranked": 1, "twos": 0},
        "playing": {"unranked": 4, "ranked": 4, "twos": 4},
        "rooms": rooms, "friendRooms": friend_rooms, "modRooms": mod_rooms,
        "members": members, "modMembers": mod_members, "live": live, "players": players,
    }


# --------------------------------------------------------------------------- main


def seed(db: Db, cfg: dict, args: argparse.Namespace) -> dict:
    rng = random.Random(20261114)
    now = datetime.now(timezone.utc).replace(microsecond=0)
    print("removing earlier fakes: " + db.run(remove_sql(everything=False)).strip())
    pw_hash = password_hash(db, cfg)

    fakes = [Fake(i, row) for i, row in enumerate(ROSTER)]
    by_name = {f.name: f for f in fakes}
    taken = {r["code"] for r in db.json("SELECT connect_code AS code FROM users WHERE connect_code IS NOT NULL")}
    for f in fakes:
        prefix, number = f.code.split("#")
        while f.code in taken:
            number = str((int(number) + 1000) % 9999 or 1)
            f.code = f"{prefix}#{number}"
        taken.add(f.code)
        f.show_country = rng.random() > 0.08
        f.show_main = rng.random() > 0.05
    for host, *_rest, name in ROOMS:
        by_name[host].room_name = name
    for m in MOD_ROOMS:
        by_name[m[0]].mods_room_name = m[8]

    seed_filter = "" if args.include_seed_ranked else " AND u.email NOT LIKE 'seed-%@staging.test'"
    reals = [Real(r) for r in db.json(f"""
        SELECT u.uid, u.display_name AS name, u.connect_code AS code, r.rating, r.sets_played AS sets, r.wins, r.losses,
               (SELECT max(created_at) FROM rating_events e WHERE e.uid = u.uid) AS last_event,
               t.rating AS twos, t.games AS twos_games, ur.rating AS unranked
          FROM users u LEFT JOIN ratings r USING (uid) LEFT JOIN twos_ratings t USING (uid)
          LEFT JOIN unranked_ratings ur USING (uid)
         WHERE u.connect_code IS NOT NULL AND u.email NOT LIKE '{FAKE_LIKE}'{seed_filter}
           AND (u.banned_until IS NULL OR u.banned_until <= now())
         ORDER BY u.created_at, u.uid""")]

    # Accounts and profiles first: the rulesets are published through the API as their owners.
    sql = Sql()
    for f in fakes:
        f.created_at = now - timedelta(days=rng.uniform(62, 120))
        sql.add("users", "uid, email, email_verified_at, pw_hash, display_name, connect_code, rules_version, "
                "room_name, mods_room_name, created_at, updated_at",
                ", ".join([q(f.uid), q(f.email), q(f.created_at), q(pw_hash), q(f.name), q(f.code), "1",
                           q(f.room_name), q(f.mods_room_name), q(f.created_at), q(f.created_at)]))
        s = f.socials
        sql.add("profiles", "uid, mains, country, discord, twitter, twitch, youtube, tiktok, instagram, "
                "show_country, show_main, updated_at",
                ", ".join([q(f.uid), "ARRAY[" + ", ".join(q(m) for m in f.mains) + "]::text[]", q(f.country or None),
                           q(s.get("discord")), q(s.get("twitter")), q(s.get("twitch")), q(s.get("youtube")),
                           q(s.get("tiktok")), q(s.get("instagram")), q(f.show_country), q(f.show_main),
                           q(f.created_at + timedelta(days=1))]))
    db.run("BEGIN;\n" + sql.text() + "COMMIT;\n")
    rulesets = publish_rulesets(db, cfg, by_name)
    rs_for_history = list(rulesets.values())

    sql = Sql()
    h = History(sql, rng, now)
    ranked_fakes = [f for f in fakes if f.target is not None]
    # 1. Each rated fake's early climb, 60 to 26 days ago.
    for f in ranked_fakes:
        climb(h, f, now - timedelta(days=60) + timedelta(hours=rng.uniform(0, 72)), now - timedelta(days=26))
    # 2. The last 25 days: sets among the fakes (true strength = target), the placing fakes' first
    #    sets, and every real account's sets, all in time order so each set rates both players as
    #    they stood then.
    events = []  # (time, kind, payload)
    window = (now - timedelta(days=25), now - timedelta(minutes=20))
    span = (window[1] - window[0]).total_seconds()
    for f in ranked_fakes:
        for _ in range(f.activity * 6):
            events.append((window[0] + timedelta(seconds=rng.uniform(0, span)), "fake", f))
    for f in fakes:
        if f.target is None and f.activity:
            for _ in range(f.activity):
                events.append((now - timedelta(days=rng.uniform(0.2, 6)), "placing", f))
    real_plans = []
    for j, r in enumerate(reals):
        lo = now - timedelta(days=14)
        if r.last_event and r.last_event + timedelta(minutes=30) > lo:
            lo = r.last_event + timedelta(minutes=30)
        hi = now - timedelta(minutes=30)
        if hi <= lo:
            hi = lo + timedelta(hours=2)
        # More of them lately, so the match history starts with today's.
        times = sorted(hi - (hi - lo) * rng.random() ** 1.6 for _ in range(22))
        for t in times:
            events.append((t, "real", (r, j)))
        real_plans.append((r, lo, hi))
    events.sort(key=lambda e: e[0])
    rivals: dict[int, Fake] = {}
    placed = [f for f in ranked_fakes]
    for t, kind, payload in events:
        if kind == "fake":
            a = payload
            b = rng.choice(nearest(placed, a.rating, 6, exclude=(a,)))
            p = expected(a.target, b.target)
            h.ranked(a, b, t, rng.random() < p)
        elif kind == "placing":
            a = payload
            b = rng.choice(nearest(placed, a.rating, 8, exclude=(a,)))
            h.ranked(a, b, t, rng.random() < 0.45)
        else:
            r, j = payload
            if j not in rivals:
                rivals[j] = rng.choice(nearest(placed, r.rating, 3))
            b = rivals[j] if rng.random() < 0.2 else rng.choice(nearest(placed, r.rating, 10))
            h.ranked(r, b, t, rng.random() < expected(r.start, b.rating))
    ranked_sets = h.matches

    # 3. Unranked, Direct, room and 2v2 matches among the fakes over the last 30 days.
    def when(days: float = 30) -> datetime:
        return now - timedelta(seconds=rng.uniform(1800, days * 86400))
    for _ in range(260):
        a = rng.choice(fakes)
        b = rng.choice(nearest([f for f in fakes if f is not a], a.rating, 12))
        h.session(1, "unranked", a, b, when(), rng.choice([1, 1, 2, 2, 3, 4, 5]))
    for _ in range(50):
        a, b = rng.sample(fakes, 2)
        h.session(2, "direct", a, b, when(), rng.choice([1, 2, 3, 3, 5, 7]))
    used_codes: set = set()
    for _ in range(110):
        mode = rng.choice(["1v1", "1v1", "ffa", "teams"])
        ps = rng.sample(fakes, 2 if mode == "1v1" else (4 if mode == "teams" else rng.choice([3, 4])))
        h.room(ps, when(), mode, room_code(rng, used_codes), rng.choice(rs_for_history + [None]) if rs_for_history else None,
               rng.choice([1, 1, 2, 3]))
    for _ in range(60):
        ps = rng.sample(fakes, 4)
        pairs = rng.choice([[1, 1, 0, 0], [0, 0, 0, 0], [1, 1, 2, 2], [0, 0, 1, 1]])
        h.twos(ps, pairs, when())

    # 4. Each real account: friends, requests, and their other matches over the last two weeks.
    online_live = [n for g in LIVE for n in g[1] if g[0] != "room"]
    launcher_only = IN_LAUNCHER + [m[0] for m in MOD_ROOMS]
    busy = {n for r in ROOMS for n in [r[0], *r[1]]} | {n for g in LIVE for n in g[1]} | set(launcher_only)
    offline = [f for f in fakes if f.name not in busy]
    friends_of: dict[uuid.UUID, list[Fake]] = {}
    for j, (r, lo, hi) in enumerate(real_plans):
        friends = [by_name["Peachy"], by_name[online_live[j % len(online_live)]],
                   by_name[launcher_only[(2 * j) % len(launcher_only)]],
                   by_name[launcher_only[(2 * j + 1) % len(launcher_only)]]]
        friends += [offline[(4 * j + k) % len(offline)] for k in range(4)]
        friends_of[r.uid] = friends
        for f in friends:
            lo_uid, hi_uid = sorted([r.uid, f.uid])
            sql.add("friendships", "lo, hi, since", ", ".join([q(lo_uid), q(hi_uid), q(now - timedelta(days=rng.uniform(2, 50)))]))
        strangers = [f for f in fakes if f not in friends]
        incoming = [s for s in strangers if s in offline][(2 * j) % 10:(2 * j) % 10 + 2] or strangers[:2]
        outgoing = [s for s in strangers if s not in incoming and s.name in busy][j % 5:j % 5 + 1] + \
                   [s for s in strangers if s not in incoming and s in offline][(j + 7) % 9:(j + 7) % 9 + 1]
        for s in incoming:
            sql.add("friend_requests", "from_uid, to_uid, created_at, declined",
                    ", ".join([q(s.uid), q(r.uid), q(now - timedelta(hours=rng.uniform(1, 96))), "false"]))
        for s in outgoing:
            sql.add("friend_requests", "from_uid, to_uid, created_at, declined",
                    ", ".join([q(r.uid), q(s.uid), q(now - timedelta(hours=rng.uniform(1, 96))), "false"]))

        def in_window() -> datetime:
            return hi - (hi - lo) * rng.random() ** 1.6 if hi > lo else hi
        near = nearest(fakes, r.rating, 12)
        for _ in range(7):
            h.session(1, "unranked", r, rng.choice(near), in_window(), rng.choice([1, 2, 3, 4]))
        for _ in range(3):
            h.session(2, "direct", r, rng.choice(friends[:4]), in_window(), rng.choice([2, 3, 5]))
        for k in range(5):
            mode = ["1v1", "ffa", "teams", "1v1", "ffa"][k]
            others = rng.sample(friends if k < 2 else fakes, {"1v1": 1, "ffa": 2, "teams": 3}[mode])
            h.room([r, *others], in_window(), mode, room_code(rng, used_codes),
                   rng.choice(rs_for_history + [None]) if rs_for_history else None, rng.choice([1, 2, 3]))
        for k in range(3):
            partner = friends[1 + k]
            opp = rng.sample([f for f in fakes if f is not partner], 2)
            h.twos([r, partner, *opp], [1, 1, 0, 0] if k < 2 else [0, 0, 0, 0], in_window())

    # 5. Final standings: the fakes' ratings, the real accounts' updated ones, the 2v2 ratings.
    for f in fakes:
        if f.sets:
            sql.add("ratings", "uid, rating, sets_played, wins, losses, updated_at",
                    ", ".join([q(f.uid), repr(f.rating), str(f.sets), str(f.wins), str(f.losses), q(now)]))
        if f.twos:
            sql.add("twos_ratings", "uid, rating, games, updated_at", ", ".join([q(f.uid), repr(f.twos[0]), str(f.twos[1]), q(now)]))
    tail = []
    for r in reals:
        tail.append(f"INSERT INTO ratings (uid, rating, sets_played, wins, losses, updated_at) VALUES "
                    f"({q(r.uid)}, {r.rating!r}, {r.sets}, {r.wins}, {r.losses}, now()) ON CONFLICT (uid) DO UPDATE SET "
                    f"rating = EXCLUDED.rating, sets_played = EXCLUDED.sets_played, wins = EXCLUDED.wins, "
                    f"losses = EXCLUDED.losses, updated_at = now();")
        if r.twos:
            tail.append(f"INSERT INTO twos_ratings (uid, rating, games, updated_at) VALUES ({q(r.uid)}, {r.twos[0]!r}, "
                        f"{r.twos[1]}, now()) ON CONFLICT (uid) DO UPDATE SET rating = EXCLUDED.rating, "
                        f"games = EXCLUDED.games, updated_at = now();")
    searches = queue_searches(sql, fakes, rng, now)
    db.run("BEGIN;\n" + sql.text() + "\n".join(tail) + "\nCOMMIT;\n")

    state = live_state(by_name, rulesets, rng, now)
    return {"fakes": fakes, "reals": reals, "friends": friends_of, "state": state, "searches": searches,
            "matches": h.matches, "ranked_sets": ranked_sets}


def admin(cfg: dict, db: Db, admin_bin: Path, *args: str) -> None:
    out = subprocess.run([str(admin_bin), *args], env=dict(os.environ, DATABASE_URL=db.url()),
                         capture_output=True, text=True, timeout=600)
    text = (out.stdout.strip() or out.stderr.strip()).splitlines()
    print(f"  admin {' '.join(args)}: {text[-1] if text else ''}")
    if out.returncode != 0:
        raise SystemExit(f"admin {' '.join(args)} failed:\n{out.stderr}")


def write_state(path: Path, state: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_bytes((json.dumps(state, indent=1) + "\n").encode())
    os.replace(tmp, path)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--remove", action="store_true", help="delete the fakes and their rows, and the state file")
    ap.add_argument("--include-seed-ranked", action="store_true",
                    help="also give seed_ranked.py's accounts (seed-*@staging.test) friends and matches")
    ap.add_argument("--config", type=Path, default=staging.CONFIG,
                    help="a staging config.json (pg_port, pg_password, accounts_port; pg_database optional)")
    ap.add_argument("--state", type=Path, default=staging.STAGE / "fake-state.json", help="the live state file")
    ap.add_argument("--admin", type=Path, default=None, help="the admin binary (default: staging's)")
    ap.add_argument("--pg-bin", type=Path, default=staging.PG_BIN, help="the folder with psql")
    args = ap.parse_args()
    if not args.config.exists():
        raise SystemExit(f"no {args.config}: start the staging server first (staging.py server up)")
    cfg = dict(staging.DEFAULTS, **json.loads(args.config.read_text()))
    db = Db(cfg, args.pg_bin)
    admin_bin = args.admin or staging.server_bin("admin")

    if args.remove:
        print("removed " + db.run(remove_sql(everything=True)).strip())
        args.state.unlink(missing_ok=True)
        admin(cfg, db, admin_bin, "ranked", "recompute")
        admin(cfg, db, admin_bin, "unranked", "rebuild")
        print(f"deleted {args.state}")
        return

    out = seed(db, cfg, args)
    admin(cfg, db, admin_bin, "ranked", "recompute")
    admin(cfg, db, admin_bin, "unranked", "rebuild")
    write_state(args.state, out["state"])

    board = db.json(f"""SELECT u.display_name AS name, u.connect_code AS code, r.rating, r.rank, r.wins, r.losses
                          FROM ratings r JOIN users u USING (uid)
                         WHERE u.email LIKE '{FAKE_LIKE}' ORDER BY r.rating DESC""")
    tiers: dict[str, int] = {}
    for row in board:
        label = "placing" if row["rank"] is None else TIERS[min(row["rank"] // 3, 6)]
        tiers[label] = tiers.get(label, 0) + 1
    print(f"\n{len(out['fakes'])} fake accounts (password {PASSWORD}), {out['matches']} matches "
          f"({out['ranked_sets']} Ranked sets), {out['searches']} queue searches")
    print("fakes per tier: " + ", ".join(f"{t} {tiers.get(t, 0)}" for t in TIERS + ["placing"]))
    for row in board[:5]:
        print(f"  {row['name']:<15} {row['code']:<9} {row['rating']:7.1f} {tier_label(row['rank']) if row['rank'] is not None else 'placing':<10} "
              f"{row['wins']}-{row['losses']}")
    for r in out["reals"]:
        names = ", ".join(f.name for f in out["friends"][r.uid])
        print(f"real {r.name} ({r.code}): friends {names}")
    if not out["reals"]:
        print("no real accounts with a connect code yet: run this again after signing up in the launcher")
    print(f"\nlive state: {args.state} ({len(out['state']['players'])} online, "
          f"{len(out['state']['rooms']) + len(out['state']['friendRooms'])} rooms, "
          f"{len(out['state']['modRooms'])} Brawl Mods rooms, {len(out['state']['live'])} live games)")


if __name__ == "__main__":
    main()
