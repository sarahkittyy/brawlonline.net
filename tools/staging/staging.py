#!/usr/bin/env python3
"""A local staging environment: a persistent staging server on this PC, and packaged Windows and
macOS clients built from one commit, set up like fresh installs that talk only to that server.

    python tools/staging/staging.py server up            # Postgres + accounts + mm (LAN-reachable)
    python tools/staging/staging.py win build [--ref main]
    python tools/staging/staging.py win run --fresh      # a fresh install's first start
    python tools/staging/staging.py mac build [--ref main]
    python tools/staging/staging.py mac run --fresh
    python tools/staging/staging.py server mail          # sign-up verification links
    python tools/staging/staging.py down                 # stop everything (the database stays)

A second staging beside the first, for a branch whose server the first's database cannot go back
from (a new migration): `--stage NAME` before the command, or STAGING_STAGE=NAME. It has its own
folder (run/staging-NAME: database, server binaries, client build, profiles), its own ports (the
first's + 100) and its own Mac folder, and shares only P+'s release zip.

EmissaryRecomp on a stage (docs/staging.md "EmissaryRecomp"): `emissary build` builds the recomp
with the core of the stage's `win build` and keeps it as a release the stage's launchers install;
`win run --engine emissary` plays Project+ with it. `--controls pad|keyboard` on `win run` and
`mac run` puts port 1 on the controller or the keyboard.

    python tools/staging/staging.py --stage emissary win build --ref emissary-recomp
    python tools/staging/staging.py --stage emissary emissary build
    python tools/staging/staging.py --stage emissary win run --profile a --engine emissary --controls pad

Everything lives in run/staging/ on this PC (gitignored) and on the Mac in
~/code/brawlonline.net/brawl-staging/:

    run/staging/config.json   ports, the LAN address, the database password and play-key secret
    run/staging/pgdata/       the staging database (kept until `server reset-db --yes`)
    run/staging/logs/         postgres, accounts, mm, build logs
    run/staging/mail.jsonl    mail the accounts server "sent" (MAILER=file)
    run/staging/src/          a git worktree of the commit being built (own Dolphin build)
    run/staging/cargo-target/ the staging server binaries, built from that commit
    run/staging/win/app/      the packaged Windows launcher (electron-builder's win-unpacked)
    run/staging/win/profiles/ launcher profiles (PPO_USER_DATA_DIR): settings, logins, Dolphin
    run/staging/pplus/        P+'s release zip, downloaded once for every profile
    ~/code/brawlonline.net/brawl-staging/  on the Mac: repo.git, src/, cache/, app/, profiles/, pplus/

A client is the packaged app, started in the launcher's test mode (PPO_TEST_MODE=1) with its own
profile folder, so it never reads or writes the real install's settings, logins or Dolphin.
`--fresh` deletes that profile first: the next start is a first run (ISO, P+ install, sign-up).
A profile without P+ (new, or --fresh) gets the P+ release zip kept in run/staging/pplus (the
Mac's pplus/) linked into its download folder, so the launcher installs P+ from it instead of
downloading 1.9 GB again: only the first profile ever downloads it.
The launcher talks to the staging accounts server (PPO_ACCOUNTS_URL), and its Dolphin gets the
staging matchmaking server and accounts URL as -C overrides (PPO_DOLPHIN_EXTRA_ARGS). The update
feed points at the staging server, so a staging build never updates itself to a release.

Nothing here touches production, the shared dolphin/build, or the real installs. Windows asks
once whether accounts.exe, mm.exe and Dolphin.exe may accept connections: allow them on private
networks, or the Mac cannot reach the server (docs/staging.md).
"""

from __future__ import annotations

import argparse
import base64
import ctypes
import hashlib
import json
import os
import re
import secrets
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _stage_name() -> str:
    """`--stage NAME` (taken out of the arguments here, before anything is named after it), else
    STAGING_STAGE; empty: the first staging, run/staging."""
    name = os.environ.get("STAGING_STAGE", "")
    if "--stage" in sys.argv:
        at = sys.argv.index("--stage")
        if at + 1 >= len(sys.argv):
            sys.exit("--stage needs a name")
        name = sys.argv[at + 1]
        del sys.argv[at:at + 2]
    if name and not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,30}", name):
        sys.exit("a stage's name is lower-case letters, digits and dashes")
    return name


STAGE_NAME = _stage_name()
# The first staging: where a named stage finds P+'s zip.
FIRST_STAGE = ROOT / "run" / "staging"
STAGE = FIRST_STAGE if not STAGE_NAME else ROOT / "run" / f"staging-{STAGE_NAME}"
CONFIG = STAGE / "config.json"
LOGS = STAGE / "logs"
PIDS = STAGE / "pids.json"
FAKE_STATE = STAGE / "fake-state.json"
SRC = STAGE / "src"
CARGO_TARGET = STAGE / "cargo-target"
WIN = STAGE / "win"
PPLUS_CACHE = FIRST_STAGE / "pplus"
# EmissaryRecomp built for this stage (`emissary build`): its worktree, its build, and the release
# feed the stage's launchers install from (PPO_EMISSARY_RELEASES).
EMISSARY = STAGE / "emissary"
EMISSARY_RELEASES = EMISSARY / "releases"
# The launcher's pinned P+ release (PPLUS_RELEASE), in the commit a build is made from.
PPLUS_RELEASE_TS = Path("launcher") / "src" / "dolphin" / "install" / "pplus_release.ts"
PG_BIN = ROOT / "run" / "postgres-portable" / "pgsql" / "bin"
IS_WINDOWS = os.name == "nt"
EXE = ".exe" if IS_WINDOWS else ""

DEFAULTS = {
    "accounts_port": 18080,   # not production's ports, so a staging client can't be mistaken
    "mm_port": 43213,         # for a production one (8080 / 43113)
    "mm_status_port": 43281,
    "pg_port": 54339,
    "latest_version": "0.1.0",  # the tree's own version (Online::APP_VERSION, release/app)
    "mac_host": "192.168.0.147",
    "mac_user": "sarahkitty",
    "mac_key": "~/.ssh/brawlmac",
    "mac_qt": "~/code/brawlonline.net/brawl-dev/qt/6.8.3/macos",
    "mac_ssh": "",  # the ssh to use; scp is the one next to it. Empty: Git's on Windows, else PATH's
    # `emissary build`: the EmissaryRecomp checkout (its toolchain/ is used as it is), the branch
    # built, and the extracted disc it translates.
    "emissary_repo": "E:/emissary/src",
    "emissary_ref": "brawlonline/windows-build",
    "emissary_game": "D:/code/pm_rollback/game/rev1-extract/DATA",
    # `emissary build --mac`: the Mac's extracted disc (shared by every stage), made from the disc
    # image beside it when it isn't there yet.
    "mac_emissary_game": "$HOME/code/brawlonline.net/emissary-game/DATA",
    "mac_emissary_disc": "$HOME/Games/SSBB_NTSC.iso",
}
if STAGE_NAME:
    # A named stage's own ports, so both servers run at once: the first's + 100.
    for _port in ("accounts_port", "mm_port", "mm_status_port", "pg_port"):
        DEFAULTS[_port] += 100
# The Mac's staging folder, from its home (scp and git paths) and inside double quotes there ("~"
# would not expand).
MAC_DIR = "code/brawlonline.net/brawl-staging" + (f"-{STAGE_NAME}" if STAGE_NAME else "")
MAC_HOME = f"$HOME/{MAC_DIR}"
# The ccache shim from `mac setup`, one for every stage.
MAC_BIN = "$HOME/code/brawlonline.net/brawl-staging/bin"
# Its own folder name. The bundle ID stays the real one: Electron's helper apps carry IDs derived
# from it, and a changed main ID made Electron stop at start-up (EXC_BREAKPOINT in ElectronMain).
# What keeps a Dock or Finder start on staging is the staging-env.json inside the app.
MAC_APP = f"{MAC_HOME}/app/Brawl Online Staging.app"
# EmissaryRecomp built on the Mac for this stage (`emissary build --mac`): its checkout, build and the
# release feed the Mac's launchers install from (PPO_EMISSARY_RELEASES), as EMISSARY on Windows.
MAC_EMISSARY = f"{MAC_HOME}/emissary"
MAC_EMISSARY_DIR = f"{MAC_DIR}/emissary"
# pgrep/pkill pattern for the staging app and the Dolphin a staging profile installed. The
# bracket keeps it from matching the command line of the shell that runs it (or a build).
MAC_PROC = "brawl-stagin[g]" + (f"-{STAGE_NAME}" if STAGE_NAME else "") + "/(app|profiles)/"


# --------------------------------------------------------------------------- helpers


def say(msg: str) -> None:
    print(f"[staging] {msg}", flush=True)


def die(msg: str) -> None:
    print(f"[staging] error: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


def run(cmd: list[str] | str, *, cwd: Path | None = None, env: dict[str, str] | None = None,
        log: Path | None = None, check: bool = True, shell: bool = False, label: str | None = None) -> int:
    """Runs a command; its output goes to `log` (appended) when given, else to the console."""
    shown = label or (cmd if isinstance(cmd, str) else " ".join(shlex.quote(str(c)) for c in cmd))
    say(f"$ {shown}" + (f"   (log: {log.relative_to(ROOT)})" if log else ""))
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        with open(log, "ab") as f:
            f.write(f"\n$ {shown}\n".encode())
            f.flush()
            rc = subprocess.call(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT, shell=shell)
    else:
        rc = subprocess.call(cmd, cwd=cwd, env=env, shell=shell)
    if check and rc != 0:
        tail = ""
        if log and log.exists():
            tail = "\n".join(log.read_text(errors="replace").splitlines()[-40:])
        die(f"command failed ({rc}): {shown}\n{tail}")
    return rc


def load_config() -> dict:
    cfg = dict(DEFAULTS)
    first = FIRST_STAGE / "config.json"
    if STAGE_NAME and not CONFIG.exists() and first.exists():
        # A new named stage starts from the first stage's Mac and LAN settings, never its ports or secrets.
        theirs = json.loads(first.read_text())
        cfg.update({k: v for k, v in theirs.items() if k.startswith("mac_") or k in ("lan_ip", "latest_version")})
    if CONFIG.exists():
        cfg.update(json.loads(CONFIG.read_text()))
    changed = False
    if "pg_password" not in cfg:
        cfg["pg_password"] = secrets.token_hex(16)
        changed = True
    if "play_key_secret" not in cfg:
        cfg["play_key_secret"] = secrets.token_hex(32)
        changed = True
    if "lan_ip" not in cfg:
        cfg["lan_ip"] = detect_lan_ip(cfg["mac_host"])
        changed = True
    if changed or not CONFIG.exists():
        save_config(cfg)
    return cfg


def save_config(cfg: dict) -> None:
    STAGE.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(json.dumps(cfg, indent=2) + "\n")


def detect_lan_ip(towards: str) -> str:
    """The address of the interface that reaches `towards` (the Mac), no packet sent."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((towards, 9))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def urls(cfg: dict) -> dict[str, str]:
    accounts = f"http://{cfg['lan_ip']}:{cfg['accounts_port']}"
    return {
        "accounts": accounts,
        "mm_host": cfg["lan_ip"],
        "mm_port": str(cfg["mm_port"]),
        # A feed that answers 404: the staging launcher finds no update instead of a release.
        "updates": f"{accounts}/staging-no-updates",
    }


def pid_alive(pid: int) -> bool:
    if IS_WINDOWS:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        h = k32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not h:
            # Access denied: it runs, started from an administrator window (an elevated token's
            # processes only let administrators open them), so it can't be stopped from this one.
            return ctypes.get_last_error() == 5
        code = ctypes.c_ulong()
        ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
        k32.CloseHandle(h)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def kill_tree(pid: int) -> None:
    if IS_WINDOWS:
        subprocess.call(["taskkill", "/PID", str(pid), "/T", "/F"], stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)
    else:
        try:
            os.kill(pid, 15)
        except OSError:
            pass


def below_normal_priority() -> None:
    """Builds run at below-normal priority (children inherit it): other work stays responsive."""
    if IS_WINDOWS:
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
    else:
        os.nice(10)


def bash() -> str:
    """Git for Windows' bash (a plain "bash" can be WSL's, which can't run the Windows toolchains)."""
    if not IS_WINDOWS:
        return "bash"
    g = shutil.which("git")
    if g:
        for cand in (Path(g).parent.parent / "bin" / "bash.exe", Path(g).parent.parent.parent / "bin" / "bash.exe"):
            if cand.exists():
                return str(cand)
    die("Git for Windows' bash.exe was not found")
    raise AssertionError


def git(*args: str, cwd: Path = ROOT) -> str:
    return subprocess.check_output(["git", *args], cwd=cwd, text=True).strip()


# --------------------------------------------------------------------------- server


def server_bin(name: str) -> Path:
    staged = CARGO_TARGET / "debug" / f"{name}{EXE}"
    return staged if staged.exists() else ROOT / "server" / "target" / "debug" / f"{name}{EXE}"


def server_env(cfg: dict) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MM_", "ACCOUNTS_", "MAIL", "RESEND_",
                                                                    "BREVO_", "SMTP_"))}
    u = urls(cfg)
    env.update({
        "DATABASE_URL": f"postgres://pp:{cfg['pg_password']}@127.0.0.1:{cfg['pg_port']}/brawl_staging",
        "PLAY_KEY_SECRET": cfg["play_key_secret"],
        "LATEST_VERSION": cfg["latest_version"],
        "RUST_LOG": "info,sqlx=warn",
        "PUBLIC_BASE_URL": u["accounts"],
        "MAILER": "file",
        "MAIL_FILE": str(STAGE / "mail.jsonl"),
        "MM_STATUS_URL": f"http://127.0.0.1:{cfg['mm_status_port']}/status",
        # Spectating's kill switch (docs/spectate-protocol.md): on in staging unless config.json's
        # "spectate" says otherwise (off, witness).
        "MM_SPECTATE": cfg.get("spectate", "on"),
        # Nothing serves the website in staging: accounts serves the profile pages' stylesheet,
        # flags, rank badges and font itself (docs/design/profiles.md §9).
        "WEBSITE_DIR": str(SRC / "website"),
        # The fake players' live state (rooms, live games, who is online), which accounts adds to
        # mm's while the file is there: `server seed-fake` writes it, `--remove` deletes it.
        "STAGING_FAKE_STATE": str(FAKE_STATE),
    })
    return env


def pg_ctl(*args: str, check: bool = True) -> int:
    return run([str(PG_BIN / f"pg_ctl{EXE}"), "-D", str(STAGE / "pgdata"), *args],
               log=LOGS / "pg_ctl.log", check=check)


def pg_running() -> bool:
    return subprocess.call([str(PG_BIN / f"pg_ctl{EXE}"), "-D", str(STAGE / "pgdata"), "status"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0


def start_postgres(cfg: dict) -> None:
    if not (PG_BIN / f"pg_ctl{EXE}").exists():
        die(f"no portable Postgres in {PG_BIN} (server/README.md: extract EDB's binaries zip there)")
    pgdata = STAGE / "pgdata"
    fresh = not (pgdata / "PG_VERSION").exists()
    if fresh:
        say("creating the staging database cluster (first run)")
        pw = STAGE / "pgpass.tmp"
        pw.write_text(cfg["pg_password"])
        try:
            run([str(PG_BIN / f"initdb{EXE}"), "-D", str(pgdata), "-U", "pp", "-A", "scram-sha-256",
                 f"--pwfile={pw}", "-E", "UTF8", "--locale=C"], log=LOGS / "initdb.log")
        finally:
            pw.unlink(missing_ok=True)
    if not pg_running():
        # Only on loopback: the servers are the only clients.
        pg_ctl("-l", str(LOGS / "postgres.log"), "-o",
               f"-p {cfg['pg_port']} -c listen_addresses=127.0.0.1", "-w", "start")
    ensure_database(cfg)


def ensure_database(cfg: dict) -> None:
    """Creates brawl_staging if it is missing. The first connections to a cluster that just started
    are sometimes dropped on a loaded machine (the harness retries the same way)."""
    env = dict(os.environ, PGPASSWORD=cfg["pg_password"], PGCONNECT_TIMEOUT="10")
    conn = ["-h", "127.0.0.1", "-p", str(cfg["pg_port"]), "-U", "pp"]
    for attempt in range(10):
        q = subprocess.run([str(PG_BIN / f"psql{EXE}"), *conn, "-d", "postgres", "-Atc",
                            "select 1 from pg_database where datname = 'brawl_staging'"],
                           env=env, capture_output=True, text=True)
        if q.returncode == 0 and q.stdout.strip() == "1":
            return
        if q.returncode == 0 and run([str(PG_BIN / f"createdb{EXE}"), *conn, "brawl_staging"], env=env,
                                     log=LOGS / "initdb.log", check=False) == 0:
            return
        time.sleep(1 + attempt)
    die(f"could not create the staging database; see {LOGS / 'postgres.log'}")


def process_name(pid: int) -> str | None:
    """A running process's image name (Windows), also of one this window may not open."""
    out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout.strip()
    m = re.match(r'"([^"]+)","(\d+)"', out)
    return m.group(1) if m and int(m.group(2)) == pid else None


def port_owners() -> dict[tuple[str, int], int]:
    """(TCP|UDP, port) -> the pid listening on it (Windows; netstat shows every user's)."""
    out = subprocess.run(["netstat", "-ano"], capture_output=True, text=True).stdout
    owners: dict[tuple[str, int], int] = {}
    for parts in (line.split() for line in out.splitlines()):
        if len(parts) < 4 or parts[0] not in ("TCP", "UDP") or not parts[-1].isdigit():
            continue
        if parts[0] == "TCP" and parts[3] != "LISTENING":
            continue
        port = parts[1].rsplit(":", 1)[-1]
        if port.isdigit():
            owners.setdefault((parts[0], int(port)), int(parts[-1]))
    return owners


def load_pids() -> dict[str, int]:
    """The staging server's accounts and mm. On Windows, when pids.json has lost a running one (it
    was started from another window, or a start that failed on its port wrote its own pid over
    it), the accounts.exe / mm.exe holding the staging port: a build stops it, `server up` keeps
    it instead of starting a second one that can't bind."""
    pids = json.loads(PIDS.read_text()) if PIDS.exists() else {}
    if not IS_WINDOWS or not CONFIG.exists():
        return pids
    cfg = load_config()
    found = dict(pids)
    owners = None
    for name, proto, port in (("accounts", "TCP", cfg["accounts_port"]), ("mm", "UDP", cfg["mm_port"])):
        pid = pids.get(name)
        if pid and pid_alive(pid) and process_name(pid) == f"{name}.exe":
            continue
        owners = port_owners() if owners is None else owners
        held = owners.get((proto, int(port)))
        if held and process_name(held) == f"{name}.exe":
            found[name] = held
        else:
            found.pop(name, None)
    if found != pids:
        PIDS.write_text(json.dumps(found))
    return found


def spawn_service(name: str, env: dict[str, str]) -> int:
    LOGS.mkdir(parents=True, exist_ok=True)
    out = open(LOGS / f"{name}.log", "ab")  # noqa: SIM115 - inherited by the detached process
    flags = 0x00000008 | 0x00000200 | 0x08000000 if IS_WINDOWS else 0  # detached, own group, no window
    proc = subprocess.Popen([str(server_bin(name))], cwd=STAGE, env=env, stdout=out,
                            stderr=subprocess.STDOUT, creationflags=flags,
                            start_new_session=not IS_WINDOWS)
    pids = load_pids()
    pids[name] = proc.pid
    PIDS.write_text(json.dumps(pids))
    return proc.pid


def accounts_healthy(cfg: dict) -> bool:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{cfg['accounts_port']}/healthz", timeout=2) as r:
            return r.read().decode().strip() == "ok"
    except OSError:
        return False


def server_up(args: argparse.Namespace) -> None:
    cfg = load_config()
    for name in ("accounts", "mm", "admin"):
        if not server_bin(name).exists():
            die(f"{server_bin(name)} is missing: run `win build` (or `cargo build --workspace` in server/)")
    start_postgres(cfg)
    pids = load_pids()
    env = server_env(cfg)
    if not (pids.get("accounts") and pid_alive(pids["accounts"])):
        aenv = dict(env, ACCOUNTS_LISTEN=f"0.0.0.0:{cfg['accounts_port']}")
        say(f"starting accounts ({server_bin('accounts').relative_to(ROOT)})")
        spawn_service("accounts", aenv)
        deadline = time.monotonic() + 60
        while not accounts_healthy(cfg):
            if time.monotonic() > deadline:
                die(f"accounts did not answer; see {LOGS / 'accounts.log'}")
            time.sleep(0.3)
    if not (pids.get("mm") and pid_alive(pids["mm"])):
        menv = dict(env, MM_LISTEN=f"0.0.0.0:{cfg['mm_port']}",
                    MM_STATUS_LISTEN=f"127.0.0.1:{cfg['mm_status_port']}")
        say(f"starting mm ({server_bin('mm').relative_to(ROOT)})")
        mm_log = LOGS / "mm.log"
        start_size = mm_log.stat().st_size if mm_log.exists() else 0
        pid = spawn_service("mm", menv)
        deadline = time.monotonic() + 30
        while "mm listening" not in mm_log.read_bytes()[start_size:].decode(errors="replace"):
            if not pid_alive(pid) or time.monotonic() > deadline:
                die(f"mm did not start; see {mm_log}")
            time.sleep(0.2)
    server_status(args)


def server_down(_args: argparse.Namespace | None = None) -> None:
    pids = load_pids()
    for name in ("mm", "accounts"):
        pid = pids.pop(name, None)
        if pid and pid_alive(pid):
            say(f"stopping {name} ({pid})")
            kill_tree(pid)
            deadline = time.monotonic() + 5
            while pid_alive(pid) and time.monotonic() < deadline:
                time.sleep(0.2)
            if pid_alive(pid):
                die(f"could not stop {name} (pid {pid}): it was started from an administrator "
                    "window (an elevated board or shell) and this one isn't. Stop it from an "
                    f"administrator PowerShell (Stop-Process -Id {pid} -Force), or run this "
                    "from the same kind of window that started it.")
    if STAGE.exists():
        PIDS.write_text(json.dumps(pids))
    if (STAGE / "pgdata").exists() and pg_running():
        pg_ctl("-m", "fast", "-w", "stop", check=False)


def server_status(_args: argparse.Namespace) -> None:
    cfg = load_config()
    pids = load_pids()
    u = urls(cfg)
    pg = (STAGE / "pgdata").exists() and pg_running()
    print(f"  postgres  {'running' if pg else 'stopped'}  127.0.0.1:{cfg['pg_port']}  ({STAGE / 'pgdata'})")
    for name in ("accounts", "mm"):
        pid = pids.get(name)
        state = f"running (pid {pid})" if pid and pid_alive(pid) else "stopped"
        print(f"  {name:<9} {state}")
    print(f"  accounts  {u['accounts']}   healthz: {'ok' if accounts_healthy(cfg) else '-'}")
    print(f"  mm        udp {u['mm_host']}:{u['mm_port']}")
    print(f"  binaries  {server_bin('accounts').parent}")


def server_mail(args: argparse.Namespace) -> None:
    f = STAGE / "mail.jsonl"
    if not f.exists():
        say("no mail yet")
        return
    lines = f.read_text(encoding="utf-8", errors="replace").splitlines()[-args.n:]
    for line in lines:
        try:
            m = json.loads(line)
        except json.JSONDecodeError:
            continue
        text = json.dumps(m)
        links = sorted(set(re.findall(r"https?://[^\s\"'<>\\]+", text)))
        print(f"- to {m.get('to')}: {m.get('subject')}")
        for link in links:
            print(f"    {link}")


def admin(*args: str) -> None:
    cfg = load_config()
    run([str(server_bin("admin")), *args], cwd=STAGE, env=server_env(cfg))


def server_seed_fake(args: argparse.Namespace) -> None:
    """tools/staging/seed_fake.py: fake players, their history and live state (docs/staging.md)."""
    cfg = load_config()
    if not accounts_healthy(cfg):
        die("the staging server isn't up: run `server up` first")
    run([sys.executable, str(ROOT / "tools" / "staging" / "seed_fake.py"), "--state", str(FAKE_STATE),
         *(["--remove"] if args.remove else []), *(["--include-seed-ranked"] if args.include_seed_ranked else [])])


def server_reset_db(args: argparse.Namespace) -> None:
    if not args.yes:
        die("this deletes the staging database (accounts, ratings, history); pass --yes")
    server_down()
    shutil.rmtree(STAGE / "pgdata", ignore_errors=True)
    (STAGE / "mail.jsonl").unlink(missing_ok=True)
    say("staging database deleted; `server up` creates an empty one")


# --------------------------------------------------------------------------- clients (shared)


def client_env(cfg: dict, profile: Path) -> dict[str, str]:
    u = urls(cfg)
    dolphin_args = [
        "-C", f"Dolphin.Online.MatchmakingHost={u['mm_host']}",
        "-C", f"Dolphin.Online.MatchmakingPort={u['mm_port']}",
        "-C", f"Dolphin.Online.AccountsUrl={u['accounts']}",
        "-C", "Dolphin.Online.UseDevServer=False",
    ]
    return {
        "PPO_TEST_MODE": "1",
        "PPO_USER_DATA_DIR": str(profile),
        "PPO_ACCOUNTS_URL": u["accounts"],
        "PPO_WEBSITE_URL": u["accounts"],
        "PPO_MM_HOST": u["mm_host"],
        "PPO_UPDATES_URL": u["updates"],
        "PPO_DOLPHIN_EXTRA_ARGS": json.dumps(dolphin_args),
    }


# Port 1 of a client (`run --controls`). Written into its netplay Dolphin's own Dolphin.ini, not
# given as -C: this Dolphin defaults every port to the adapter and opens the adapter while any
# config layer names it, so a keyboard client given only -C would still take the adapter from the
# other one. EmissaryRecomp's keyboard switch is in its Config.toml.
SI_GC_CONTROLLER = 6  # a standard controller: GCPadNew.ini's [GCPad1], the keyboard as Dolphin sets it up
SI_ADAPTER = 12
CONTROLS = {"pad": SI_ADAPTER, "keyboard": SI_GC_CONTROLLER}
DOLPHIN_INI = Path("netplay") / "User" / "Config" / "Dolphin.ini"
EMISSARY_CONFIG = Path("netplay") / "emissary-recomp" / "UserData" / "Config.toml"  # the launcher's EMISSARY_FOLDER


def with_si_devices(ini: str, si: int) -> str:
    """Dolphin.ini with [Core] SIDevice0 = si and SIDevice1-3 = 0 (Dolphin plays the first port set up)."""
    rest = re.sub(r"^SIDevice\d\s*=.*(\r?\n|$)", "", ini, flags=re.M)
    ports = "".join(f"SIDevice{i} = {v}\n" for i, v in enumerate([si, 0, 0, 0]))
    if re.search(r"^\[Core\]\r?\n", rest, flags=re.M):
        return re.sub(r"^\[Core\]\r?\n", lambda m: m.group(0) + ports, rest, count=1, flags=re.M)
    return f"[Core]\n{ports}{rest}"


def set_toml_value(text: str, section: str, key: str, value: str) -> str:
    """`key = value` in `[section]` of a TOML text (either added), every other line as it was (the
    launcher's setTomlValue)."""
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if line.strip() == f"[{section}]"), -1)
    entry = f"{key} = {value}"
    if start < 0:
        while lines and not lines[-1].strip():
            lines.pop()
        return "\n".join([*lines, *([""] if lines else []), f"[{section}]", entry]) + "\n"
    end = next((i for i in range(start + 1, len(lines)) if lines[i].lstrip().startswith("[")), len(lines))
    for i in range(start + 1, end):
        if re.match(rf"^\s*{re.escape(key)}\s*=", lines[i]):
            lines[i] = entry
            return "\n".join(lines) + "\n"
    at = end
    while at > start + 1 and not lines[at - 1].strip():
        at -= 1
    lines.insert(at, entry)
    return "\n".join(lines) + "\n"


def set_win_controls(profile: Path, controls: str) -> None:
    """Port 1 of a Windows profile: its netplay Dolphin's and its EmissaryRecomp's (written before
    the first start too: the launcher's installs keep both files)."""
    ini = profile / DOLPHIN_INI
    ini.parent.mkdir(parents=True, exist_ok=True)
    text = ini.read_text(encoding="utf-8", errors="replace") if ini.exists() else ""
    ini.write_text(with_si_devices(text, CONTROLS[controls]), encoding="utf-8", newline="\n")
    toml = profile / EMISSARY_CONFIG
    toml.parent.mkdir(parents=True, exist_ok=True)
    text = toml.read_text(encoding="utf-8", errors="replace") if toml.exists() else ""
    toml.write_text(set_toml_value(text, "gc_keyboard", "enabled", "true" if controls == "keyboard" else "false"),
                    encoding="utf-8", newline="\n")
    say(f"port 1: {'the GameCube adapter (EmissaryRecomp: a controller)' if controls == 'pad' else 'the keyboard'}")


def with_play_with(settings: str, engine: str) -> str:
    """A launcher Settings file's text with Settings > Add-ons' choice (playProjectPlusWith) = engine."""
    data = json.loads(settings) if settings.strip() else {}
    data.setdefault("settings", {})["playProjectPlusWith"] = engine
    return json.dumps(data, indent=2)


def set_play_with(profile: Path, engine: str) -> None:
    """The launcher's Settings > Add-ons choice (playProjectPlusWith) in the profile's Settings file."""
    f = profile / "Settings"
    f.write_text(with_play_with(f.read_text(encoding="utf-8") if f.exists() else "", engine), encoding="utf-8")


def emissary_release() -> str | None:
    """The tag `emissary build` last kept for this stage."""
    if not EMISSARY_RELEASES.exists():
        return None
    tags = sorted(d.name for d in EMISSARY_RELEASES.glob("v*") if (d / "manifest.json").exists())
    return tags[-1] if tags else None


def staging_env_json(cfg: dict, profile: str) -> str:
    """resources/staging-env.json: the launcher applies it at start-up (launcher main.ts), so the
    staging app keeps its profile and servers even when started from the Dock, Finder or the .exe."""
    env = client_env(cfg, Path("PROFILE"))
    env["PPO_USER_DATA_DIR"] = profile
    return json.dumps(env, indent=2)


def resolve_ref(ref: str) -> str:
    try:
        return git("rev-parse", "--verify", f"{ref}^{{commit}}")
    except subprocess.CalledProcessError:
        die(f"unknown ref {ref!r}")
        raise


# --------------------------------------------------------------------------- P+ for new profiles
#
# A profile's first start downloads P+'s release zip (1.9 GB) to <profile>/netplay/pplus/downloads,
# installs P+ from it and deletes it. The launcher uses a zip it finds there if it verifies
# (installProjectPlusFiles), so a profile without P+ gets a hard link to the one zip staging keeps:
# after the first, no profile downloads P+ again, and the install is still the launcher's own.


def pinned_pplus(ts: str) -> dict | None:
    """PPLUS_RELEASE's version, url, size and sha256 from pplus_release.ts's text (None if absent)."""
    m = re.search(r"PPLUS_RELEASE\b[^=]*=\s*\{(.*?)\n\};", ts, re.S)
    if not m:
        return None
    rel = {}
    for key in ("version", "url", "size", "sha256"):
        f = re.search(rf'^\s*{key}:\s*"?([^",\n]+)"?,', m.group(1), re.M)
        if not f:
            return None
        rel[key] = f.group(1).strip()
    rel["size"] = int(rel["size"].replace("_", ""))
    return rel


def download_pplus(rel: dict, dest: Path) -> None:
    """Downloads the release zip to `dest` (continuing a .part an earlier try left) and checks its
    size and sha256 as the launcher does."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    have = part.stat().st_size if part.exists() else 0
    if have > rel["size"]:
        part.unlink()
        have = 0
    if have < rel["size"]:
        say(f"downloading Project+ {rel['version']} once for every staging profile: {rel['url']}"
            + (f" (from byte {have})" if have else ""))
        headers = {"User-Agent": "brawl-staging"} | ({"Range": f"bytes={have}-"} if have else {})
        with urllib.request.urlopen(urllib.request.Request(rel["url"], headers=headers), timeout=60) as r:
            resumed = have > 0 and r.status == 206
            done = have if resumed else 0
            shown = done
            with open(part, "ab" if resumed else "wb") as f:
                while chunk := r.read(1 << 20):
                    f.write(chunk)
                    done += len(chunk)
                    if done - shown >= rel["size"] // 10:
                        shown = done
                        say(f"  {done * 100 // rel['size']}%")
    h = hashlib.sha256()
    with open(part, "rb") as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    size = part.stat().st_size
    if size != rel["size"] or h.hexdigest() != rel["sha256"]:
        part.unlink()
        die(f"the Project+ {rel['version']} download has {size} bytes, sha256 {h.hexdigest()}; "
            f"expected {rel['size']}, {rel['sha256']} (deleted: run again)")
    part.replace(dest)
    say(f"kept {dest} for every staging profile")


def seed_pplus_win(profile: Path) -> None:
    """Gives a Windows profile without P+ (new, or --fresh) the kept release zip (see above)."""
    ts = SRC / PPLUS_RELEASE_TS
    rel = pinned_pplus(ts.read_text(encoding="utf-8")) if ts.exists() else None
    if not rel:
        say(f"warning: no PPLUS_RELEASE in {ts}: the launcher downloads P+ itself")
        return
    try:
        if json.loads((profile / "netplay" / "pplus" / "pplus.json").read_text()).get("version") == rel["version"]:
            return
    except (OSError, ValueError, AttributeError):
        pass
    name = f"pplus-{rel['version']}.zip"
    kept = PPLUS_CACHE / name
    if not kept.exists():
        download_pplus(rel, kept)
    dest = profile / "netplay" / "pplus" / "downloads" / name
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(kept, dest)  # the launcher deletes it after the install; the kept one stays
        except OSError:
            shutil.copy2(kept, dest)
    say(f"profile {profile.name} installs Project+ {rel['version']} from {kept} (no download)")


def mac_seed_pplus(rel: dict, profile: str) -> str:
    """seed_pplus_win's script for a Mac profile (zip kept in brawl-staging/pplus). A failed
    download only warns: the launcher then downloads P+ itself."""
    q = shlex.quote
    name = f"pplus-{rel['version']}.zip"
    marker = f'"version": "{rel["version"]}"'
    return f"""
if ! grep -qsF {q(marker)} "{profile}/netplay/pplus/pplus.json"; then
  Z="{MAC_HOME}/pplus/{name}"; D="{profile}/netplay/pplus/downloads"
  if [ ! -f "$Z" ]; then
    mkdir -p "{MAC_HOME}/pplus"
    echo "[staging] downloading Project+ {rel['version']} once for every staging profile"
    if [ "$(stat -f %z "$Z.part" 2>/dev/null || echo 0)" = {rel['size']} ] || curl -fsSL --retry 3 -C - -o "$Z.part" {q(rel['url'])}; then
      if [ "$(stat -f %z "$Z.part")" = {rel['size']} ] && [ "$(shasum -a 256 "$Z.part" | cut -d' ' -f1)" = {rel['sha256']} ]; then
        mv "$Z.part" "$Z"
      else
        rm -f "$Z.part"; echo "[staging] warning: the Project+ download did not verify (deleted)"
      fi
    else
      echo "[staging] warning: the Project+ download failed"
    fi
  fi
  if [ -f "$Z" ]; then
    mkdir -p "$D"; [ -e "$D/{name}" ] || ln "$Z" "$D/{name}" || cp "$Z" "$D/{name}"
    echo "[staging] profile {profile.rsplit('/', 1)[-1]} installs Project+ {rel['version']} from $Z (no download)"
  fi
fi
"""


# --------------------------------------------------------------------------- Windows client


def junction(link: Path, target: Path) -> None:
    if link.exists() and any(link.iterdir()):
        return
    if link.exists():
        link.rmdir()
    link.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call(["cmd", "/c", "mklink", "/J", str(link), str(target)], stdout=subprocess.DEVNULL)


def unlink_junctions(root: Path) -> int:
    """Removes the junctions `prepare_worktree` made (never their targets): at this checkout's
    submodule paths and at the worktree's own, which differ once a submodule is dropped."""
    n = 0
    for rel in [*sorted({*submodule_paths(), *submodule_paths(root)}), "toolchains"]:
        p = root / rel
        if p.is_junction():
            os.rmdir(p)  # the junction itself, not its target
            n += 1
    return n


def submodule_paths(cwd: Path = ROOT) -> list[str]:
    out = git("ls-files", "-s", cwd=cwd)
    return [line.split("\t", 1)[1] for line in out.splitlines() if line.startswith("160000 ")]


def prepare_worktree(sha: str) -> None:
    if not (SRC / ".git").exists():
        say(f"creating the staging worktree {SRC.relative_to(ROOT)}")
        run(["git", "worktree", "add", "--detach", str(SRC), sha])
    else:
        dirty = git("status", "--porcelain", "--ignore-submodules=all", "--untracked-files=no", cwd=SRC)
        if dirty:
            die(f"the staging worktree has local changes:\n{dirty}")
        # Out of the way first: a submodule that became plain files (game-code/lib/brawlback-common)
        # would otherwise be checked out through its junction, into this checkout's copy.
        unlink_junctions(SRC)
        run(["git", "checkout", "--detach", "--quiet", sha], cwd=SRC)
    # The submodules (Dolphin's Externals, game-code's headers) and the plugin toolchain: links to
    # this checkout's copies, as the other worktrees have.
    for path in submodule_paths():
        src = ROOT / path
        if src.exists() and any(src.iterdir()):
            junction(SRC / path, src)
    if (ROOT / "toolchains").exists():
        junction(SRC / "toolchains", ROOT / "toolchains")


# The rank badge PNGs are not in git (Layerlab's license, tools/rank-badges/fetch_badges.sh): a staging
# build gets this checkout's copies, fetched (or placeholders) first if they are missing.
RANK_BADGE_DIRS = ("launcher/src/renderer/components/rank_badge/images", "website/assets/ranks")


def rank_badges_here(log: Path) -> None:
    run([bash(), "tools/rank-badges/fetch_badges.sh"], cwd=ROOT, log=log)


def copy_rank_badges(dest: Path, log: Path) -> None:
    rank_badges_here(log)
    for d in RANK_BADGE_DIRS:
        shutil.rmtree(dest / d, ignore_errors=True)
        shutil.copytree(ROOT / d, dest / d)


def find_vcvars() -> Path:
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                      "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                      "-property", "installationPath"], text=True).strip()
        if vs:
            return Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    die("Visual Studio with the C++ tools was not found")
    raise AssertionError


def build_vcvars(build_dir: Path | None) -> Path:
    """vcvars64.bat of the compiler the build dir was configured with (else the latest)."""
    vcvars = find_vcvars()
    cache = build_dir / "CMakeCache.txt" if build_dir else None
    if cache and cache.exists():
        m = re.search(r"^CMAKE_CXX_COMPILER:\w+=(.+?)/VC/Tools/", cache.read_text(errors="replace"), re.M)
        if m:
            vcvars = Path(m.group(1)) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    return vcvars


def toolchain_stamp(build_dir: Path) -> str | None:
    """Size and mtime of the build dir's compiler and of the Windows SDKs' headers."""
    cache = build_dir / "CMakeCache.txt"
    m = re.search(r"^CMAKE_CXX_COMPILER:\w+=(.+)$", cache.read_text(errors="replace"), re.M) \
        if cache.exists() else None
    if not m:
        return None
    bindir = Path(m.group(1).strip()).parent
    files = [bindir / n for n in ("cl.exe", "c1xx.dll", "c2.dll")]
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10"
    for sdk in sorted((kits / "Include").glob("10.*")):
        files += [sdk / "um" / "Windows.h", sdk / "cppwinrt" / "winrt" / "base.h"]
    return "\n".join(f"{f} {f.stat().st_size} {f.stat().st_mtime_ns}" for f in files if f.exists())


def clean_after_toolchain_update(build_dir: Path, log: Path) -> None:
    """Builds Dolphin from scratch when the compiler or the Windows SDK changed under the build dir.

    A Visual Studio update reinstalls cl.exe and the Windows SDK in their same folders (14.44.35207,
    10.0.26100.0), and the new files keep their old build dates, so ninja rebuilds nothing: the old
    precompiled header then fails every object with C1853 ("from a different version of the
    compiler"), and objects from before the update don't link with new ones (LNK2038 'C++/WinRT
    version' 2.0.250303.1 vs 2.0.250303.5, 2026-11-13). Their sizes and dates are stamped in the
    build dir; when they differ (or there is no stamp yet), the build dir is cleaned first."""
    stamp = toolchain_stamp(build_dir)
    if stamp is None:
        return
    stamp_file = build_dir / "staging-toolchain.txt"
    if stamp_file.exists() and stamp_file.read_text() == stamp:
        return
    if (build_dir / ".ninja_log").exists():
        say("the compiler or Windows SDK changed since the last build: building Dolphin from scratch")
        msvc(f'cmake --build "{build_dir}" --target clean', cwd=build_dir, log=log, build_dir=build_dir)
    stamp_file.write_text(stamp)


def dedupe_path() -> None:
    """Lists each of this process's PATH folders once, for every command it starts.

    cmd refuses a line over 8191 characters and can't search a PATH that long, and this PC's
    machine and user PATHs list many folders twice, about 7,700 characters together (4,900 without
    the repeats). Anything that adds to PATH went over (Sarah's board, card 818: ""build everything"
    failed"): vcvars64.bat puts its folders in front in one `set` line ("The input line is too
    long." then "The syntax of the command is incorrect.", exit 255, at `cmake --build`), and
    `npm run` puts node_modules/.bin folders in front ("'node' is not recognized", at
    `npm run stage:plugin`)."""
    seen: set[str] = set()
    folders = []
    for p in os.environ.get("PATH", "").split(os.pathsep):
        key = os.path.normcase(os.path.normpath(p.strip())) if p.strip() else ""
        if key and key not in seen:
            seen.add(key)
            folders.append(p)
    os.environ["PATH"] = os.pathsep.join(folders)


def msvc(cmd: str, cwd: Path, log: Path, build_dir: Path | None = None) -> None:
    """Runs `cmd` with the MSVC environment of the compiler the build dir was configured with."""
    run(f'call "{build_vcvars(build_dir)}" >nul && {cmd}', cwd=cwd, log=log, shell=True)


def vc_runtime_dir(build_dir: Path) -> Path:
    """The x64 Visual C++ runtime DLLs (msvcp140.dll, vcruntime140.dll, ...) of the build's compiler."""
    # `set`, not `echo %VCToolsRedistDir%`: cmd expands %...% before vcvars runs.
    out = subprocess.run(f'call "{build_vcvars(build_dir)}" >nul && set VCToolsRedistDir',
                         shell=True, capture_output=True, text=True).stdout
    m = re.search(r"^VCToolsRedistDir=(.+?)\s*$", out, re.M)
    crt = sorted((Path(m.group(1)) / "x64").glob("Microsoft.VC*.CRT")) if m else []
    if not crt:
        die(f"the Visual C++ runtime was not found ({m.group(1) if m else 'no VCToolsRedistDir'})")
    return crt[-1]


def pe_imports(path: Path) -> list[str]:
    """The DLLs a PE file imports at load time (its import directory's names)."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    dirs = optional + (112 if struct.unpack_from("<H", data, optional)[0] == 0x20B else 96)
    rva = struct.unpack_from("<I", data, dirs + 8)[0]
    # (virtual size, virtual address, raw size, raw offset) of each section
    table = [struct.unpack_from("<IIII", data, optional + optional_size + i * 40 + 8) for i in range(sections)]

    def offset(r: int) -> int | None:
        for vsize, va, raw_size, raw in table:
            if va <= r < va + max(vsize, raw_size):
                return r - va + raw
        return None

    names, at = [], offset(rva) if rva else None
    while at is not None and (name_rva := struct.unpack_from("<I", data, at + 12)[0]):
        if (o := offset(name_rva)) is not None:
            names.append(data[o:data.index(0, o)].decode("latin-1"))
        at += 20
    return names


def copy_vc_runtime(out: Path, crt: Path) -> None:
    """build-dolphin-windows.ps1's runtime step: the runtime DLLs the bundle (and they) import."""
    scan = [p for p in out.rglob("*") if p.suffix.lower() in (".exe", ".dll")]
    while scan:
        names = {n for p in scan for n in pe_imports(p)
                 if re.fullmatch(r"(msvcp|vcruntime|concrt|vccorlib)140\w*\.dll", n, re.I)}
        scan = []
        for name in sorted(names):
            if (out / name).exists():
                continue
            src = next((f for f in crt.iterdir() if f.name.lower() == name.lower()), None)
            if not src:
                die(f"{name} is imported but not in {crt}")
            shutil.copy2(src, out / src.name)
            scan.append(out / src.name)


def win_build(args: argparse.Namespace) -> None:
    if not IS_WINDOWS:
        die("`win build` runs on Windows")
    cfg = load_config()
    sha = resolve_ref(args.ref)
    log = LOGS / "win-build.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(f"win build {args.ref} {sha}\n")
    below_normal_priority()
    prepare_worktree(sha)
    copy_rank_badges(SRC, log)
    say(f"building {sha[:10]} ({git('log', '-1', '--format=%s', sha)[:70]})")

    # 1. The staging server, from the same commit. Windows locks a running .exe, so a running
    # staging server is stopped for the build and started again on the new binaries at the end.
    server_was_up = any(pid_alive(pid) for pid in load_pids().values())
    if server_was_up:
        say("stopping the staging server for the build (it restarts on the new binaries)")
        server_down()
    run(["cargo", "build", "--workspace"], cwd=SRC / "server", log=log,
        env=dict(os.environ, CARGO_TARGET_DIR=str(CARGO_TARGET)))

    # 2. Dolphin: the release preset in the worktree's own build dir (as CI configures it).
    dsrc = SRC / "dolphin"
    build = dsrc / "build" / "release" / "x64"
    if not (build / "build.ninja").exists():
        msvc("cmake --preset ninja-release-x64 -DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF "
             "-DUSE_DISCORD_PRESENCE=OFF -DENABLE_TESTS=OFF -DENABLE_NOGUI=OFF "
             '"-DDISTRIBUTOR=brawlonline.net"', cwd=dsrc, log=log)
    clean_after_toolchain_update(build, log)
    msvc(f'cmake --build "{build}" --target project-plus-dolphin dolphin-tool', cwd=dsrc, log=log,
         build_dir=build)

    # 3. The game plugin.
    run([bash(), "./build.sh"], cwd=SRC / "game-code", log=log)

    # 4. The launcher, packaged like a release (electron-builder's unpacked dir: no installer).
    launcher = SRC / "launcher"
    lock = hashlib.sha256((launcher / "package-lock.json").read_bytes()).hexdigest()
    marker = launcher / "node_modules" / ".staging-lock"
    if not marker.exists() or marker.read_text() != lock:
        run("npm ci --no-audit --no-fund", cwd=launcher, log=log, shell=True)
        marker.write_text(lock)
    run("npm run stage:plugin", cwd=launcher, log=log, shell=True)
    run("npm run build", cwd=launcher, log=log, shell=True)
    stage_windows_dolphin(build / "Binaries", launcher / "release" / "dolphin", dsrc, sha, log,
                          vc_runtime_dir(build))
    shutil.rmtree(launcher / "release" / "build", ignore_errors=True)
    run("npx electron-builder build --win dir --publish never", cwd=launcher, log=log, shell=True)
    unpacked = launcher / "release" / "build" / "win-unpacked"
    if not (unpacked / "Brawl Online.exe").exists():
        die(f"electron-builder made no {unpacked / 'Brawl Online.exe'}")
    win_stop(None)
    app = WIN / "app"
    shutil.rmtree(app, ignore_errors=True)
    shutil.copytree(unpacked, app)
    (app / "resources" / "staging-env.json").write_text(staging_env_json(cfg, str(win_profile("a"))))
    (WIN / "build.json").write_text(json.dumps({"sha": sha, "ref": args.ref, "built": time.ctime()}) + "\n")
    say(f"Windows client ready: {app / 'Brawl Online.exe'} ({sha[:10]})")
    if server_was_up:
        server_up(args)
    else:
        say("the staging server runs this commit's binaries from the next `server up`")


def stage_windows_dolphin(binaries: Path, out: Path, dsrc: Path, sha: str, log: Path,
                          crt: Path) -> None:
    """build-dolphin-windows.ps1's staging part: what Dolphin needs at run time, then the manifest."""
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    skip_names = {"Tests", "Updater.exe", "DolphinNoGUI.exe", "portable.txt"}
    for p in binaries.iterdir():
        if p.name in skip_names or p.suffix in (".pdb", ".ilk", ".exp", ".lib"):
            continue
        (shutil.copytree if p.is_dir() else shutil.copy2)(p, out / p.name)
    copy_vc_runtime(out, crt)
    if not (out / "COPYING").exists():
        shutil.copy2(dsrc / "COPYING", out / "COPYING")
    if not (out / "Licenses").exists():
        shutil.copytree(dsrc / "LICENSES", out / "Licenses")
    # A version per commit, so a new staging build replaces the Dolphin a profile installed.
    run([bash(), str(SRC / ".github" / "scripts" / "dolphin-manifest.sh"), out.as_posix(),
         f"0.1.0-staging.{sha[:10]}", "Dolphin.exe"], log=log)


def win_profile(name: str) -> Path:
    return WIN / "profiles" / name


def win_processes() -> list[tuple[int, str]]:
    """Processes started from the staging app or a staging profile (launcher, Dolphin)."""
    if not IS_WINDOWS:
        return []
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath } | "
                          "ForEach-Object { \"$($_.ProcessId)`t$($_.ExecutablePath)\" }"],
                         capture_output=True, text=True).stdout
    base = str(WIN).lower()
    procs = []
    for line in out.splitlines():
        pid, _, exe = line.partition("\t")
        if exe.lower().startswith(base) and pid.strip().isdigit():
            procs.append((int(pid), exe))
    return procs


def win_stop(_args: argparse.Namespace | None) -> None:
    for pid, exe in win_processes():
        say(f"stopping {Path(exe).name} ({pid})")
        kill_tree(pid)


def win_run(args: argparse.Namespace) -> None:
    cfg = load_config()
    exe = WIN / "app" / "Brawl Online.exe"
    if not exe.exists():
        die("no Windows staging build yet: `win build`")
    if not accounts_healthy(cfg):
        say("warning: the staging server is not running (`server up`)")
    profile = win_profile(args.profile)
    if args.fresh:
        win_reset(args)
    profile.mkdir(parents=True, exist_ok=True)
    seed_pplus_win(profile)
    env = dict(os.environ, **client_env(cfg, profile))
    if args.controls:
        set_win_controls(profile, args.controls)
    tag = emissary_release()
    if tag:
        # The stage's own EmissaryRecomp, as the launcher's feed (its latest release).
        env["PPO_EMISSARY_RELEASES"] = str(EMISSARY_RELEASES)
    if args.engine:
        if args.engine == "emissary" and not tag:
            die("this stage has no EmissaryRecomp build: `emissary build` (after `win build`)")
        set_play_with(profile, args.engine)
        say(f"Play plays Project+ with {'EmissaryRecomp ' + tag if args.engine == 'emissary' else 'Dolphin'}")
    flags = 0x00000008 | 0x00000200  # detached, own process group
    subprocess.Popen([str(exe)], cwd=exe.parent, env=env, creationflags=flags,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    built = json.loads((WIN / "build.json").read_text()) if (WIN / "build.json").exists() else {}
    say(f"started the Windows client ({built.get('sha', '?')[:10]}), profile {profile}")


def win_reset(args: argparse.Namespace) -> None:
    profile = win_profile(args.profile)
    if win_processes():
        say("stopping the running staging client first")
        win_stop(None)
        time.sleep(1)
    if profile.exists():
        shutil.rmtree(profile)
        say(f"deleted profile {profile} (the next start is a fresh install)")


# --------------------------------------------------------------------------- EmissaryRecomp


# What a release's Windows archive holds (EmissaryRecomp's docs/releases.md): the program, the
# libraries beside it, and its data folders. A local build is one program with the engine inside.
EMISSARY_FILES = ["WiiCompiled.exe", "cacert.pem", "dsp_coef.bin", "initial_pipeline_cache.db"]
EMISSARY_DIRS = ["Sys", "wii_bootstrap"]
EMISSARY_PROTOCOL = 1  # the launcher's EMISSARY_PROTOCOL


def emissary_build(args: argparse.Namespace) -> None:
    """EmissaryRecomp with the core of this stage's `win build` (its src/ worktree), kept as a release
    in emissary/releases/ for the stage's launchers to install (PPO_EMISSARY_RELEASES): the same
    core as the stage's Dolphin, server and launcher. `--mac`: the Mac's build, for `mac run`."""
    if args.mac:
        mac_emissary_build(args)
        return
    if not IS_WINDOWS:
        die("`emissary build` runs on Windows")
    cfg = load_config()
    built = json.loads((WIN / "build.json").read_text()) if (WIN / "build.json").exists() else {}
    if not built.get("sha") or not (SRC / "core" / "standalone" / "CMakeLists.txt").exists():
        die("no core to build with: `win build --ref <a branch with core/>` first")
    if git("rev-parse", "HEAD", cwd=SRC) != built["sha"]:
        die(f"{SRC} is not at the built commit {built['sha'][:10]}: run `win build` again")
    repo = Path(cfg["emissary_repo"])
    ref = args.ref or cfg["emissary_ref"]
    rsha = git("rev-parse", "--verify", f"{ref}^{{commit}}", cwd=repo)
    toolchain = repo / "toolchain"
    cmake = toolchain / "CMake" / "bin" / "cmake.exe"
    if not cmake.exists():
        die(f"no EmissaryRecomp toolchain in {toolchain}")
    game = Path(cfg["emissary_game"])
    if not (game / "sys" / "main.dol").exists():
        die(f"{game} is not an extracted disc's DATA folder (config.json emissary_game)")
    log = LOGS / "emissary-build.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(f"emissary build {ref} {rsha} with core {built['sha']}\n")
    below_normal_priority()
    wt = EMISSARY / "src"
    if not wt.exists():
        EMISSARY.mkdir(parents=True, exist_ok=True)
        run(["git", "-C", str(repo), "worktree", "add", "--detach", str(wt), rsha], log=log)
    else:
        run(["git", "-C", str(wt), "checkout", "--detach", "--force", "--quiet", rsha], log=log)
    version = (wt / "VERSION").read_text().strip()
    tag = f"v{version}-staging.{time.strftime('%Y%m%d%H%M%S')}"
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([str(toolchain / "llvm-mingw" / "bin"), str(toolchain / "Ninja"),
                                   str(toolchain / "CMake" / "bin"), env.get("PATH", "")])
    build = EMISSARY / "build"
    run([str(cmake), f"-DGAME={game.as_posix()}", f"-DBUILD_DIR={build.as_posix()}", f"-DJOBS={args.jobs}",
         f"-DBRAWL_ONLINE={SRC.as_posix()}", f"-DRELEASE_VERSION={tag}", "-P", str(wt / "build.cmake")],
        cwd=wt, env=env, log=log)
    native = build / "native"
    exe = native / "WiiCompiled.exe"
    if b"BRAWL_ONLINE_CORE" not in exe.read_bytes():
        die(f"{exe} has no Brawl Online core (the launcher would not play it)")
    # The release the launchers install, alone in the feed (its latest).
    shutil.rmtree(EMISSARY_RELEASES, ignore_errors=True)
    dest = EMISSARY_RELEASES / tag
    dest.mkdir(parents=True)
    archive = dest / f"EmissaryRecomp-{tag}-Windows.zip"
    say(f"packing {archive}")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as z:
        for name in [*EMISSARY_FILES, *sorted(f.name for f in native.glob("*.dll"))]:
            if (native / name).exists():
                z.write(native / name, f"EmissaryRecomp/{name}")
        for d in EMISSARY_DIRS:
            for f in sorted((native / d).rglob("*")):
                if f.is_file():
                    z.write(f, f"EmissaryRecomp/{f.relative_to(native).as_posix()}")
    (dest / "manifest.json").write_text(json.dumps(
        {"version": tag, "brawlOnline": cfg["latest_version"], "protocol": EMISSARY_PROTOCOL}, indent=2) + "\n")
    (EMISSARY / "build.json").write_text(json.dumps(
        {"tag": tag, "ref": ref, "sha": rsha, "core": built["sha"], "built": time.ctime()}) + "\n")
    say(f"EmissaryRecomp {tag} ready ({ref} {rsha[:10]}, core {built['sha'][:10]}): "
        f"`win run --engine emissary` plays with it; a running launcher takes it before its next game")


def mac_emissary_build(args: argparse.Namespace) -> None:
    """`emissary build --mac` (the agent board's "Build Mac recomp"): EmissaryRecomp built on the Mac
    (Apple silicon) from the same recomp ref as on Windows, with the core of the stage's `mac build`
    (its src/ on the Mac, the launcher's own commit), kept as a CI-style release
    (EmissaryRecomp-<tag>-macOS.tar.gz + manifest.json) in the Mac's <stage>/emissary/releases, which
    `mac run` gives the launcher (PPO_EMISSARY_RELEASES). The recomp's tools/release-macos.sh builds
    and checks the program (macOS's own libraries only, signed ad hoc)."""
    cfg = load_config()
    repo = Path(cfg["emissary_repo"])
    ref = args.ref or cfg["emissary_ref"]
    rsha = git("rev-parse", "--verify", f"{ref}^{{commit}}", cwd=repo)
    if not git("ls-tree", "--name-only", rsha, "tools/release-macos.sh", cwd=repo):
        die(f"{ref} ({rsha[:10]}) has no macOS build (tools/release-macos.sh, EmissaryRecomp brawlonline/macos)")
    log = LOGS / "mac-emissary-build.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(f"emissary build --mac {ref} {rsha}\n")
    # The core: the Mac's build checkout, at the commit its app was built from.
    out = subprocess.run([*mac_target(cfg), f'cat "{MAC_HOME}/app/build.json" 2>/dev/null; '
                          f'git -C "{MAC_HOME}/src" rev-parse HEAD 2>/dev/null; '
                          f'grep -c HAVE_PIPE2 "{MAC_HOME}/src/core/standalone/CMakeLists.txt" 2>/dev/null'],
                         capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60).stdout
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    core = json.loads(lines[0]).get("sha", "") if lines and lines[0].startswith("{") else ""
    if not core:
        die("no macOS staging build to take the core from: `mac build` first")
    if len(lines) < 2 or lines[1] != core:
        die(f"the Mac's src/ is not at its app's commit {core[:10]}: run `mac build` again")
    if len(lines) < 3 or lines[2] == "0":
        die(f"the Mac's build ({core[:10]}) predates the core's macOS build (5310e80fa): `mac build` a newer commit")
    # The recomp commit, pushed into a bare repository on the Mac (as `mac build` does the monorepo's).
    mac(cfg, f'mkdir -p "{MAC_EMISSARY}"; [ -d "{MAC_EMISSARY}/repo.git" ] || git init -q --bare "{MAC_EMISSARY}/repo.git"')
    key = os.path.expanduser(cfg["mac_key"]).replace("\\", "/")
    ssh = ssh_tool(cfg, "ssh").replace("\\", "/")
    env = dict(os.environ, GIT_SSH_COMMAND=f'"{ssh}" -i "{key}" -o BatchMode=yes')
    run(["git", "-C", str(repo), "push", "--quiet", "--force",
         f"ssh://{cfg['mac_user']}@{cfg['mac_host']}/~/{MAC_EMISSARY_DIR}/repo.git", f"{rsha}:refs/heads/staging"],
        env=env, log=log)
    version = git("show", f"{rsha}:VERSION", cwd=repo).strip()
    tag = f"v{version}-staging.{time.strftime('%Y%m%d%H%M%S')}"
    name = f"EmissaryRecomp-{tag}-macOS.tar.gz"
    manifest = json.dumps({"version": tag, "brawlOnline": cfg["latest_version"], "protocol": EMISSARY_PROTOCOL},
                          indent=2) + "\n"
    built = json.dumps({"tag": tag, "ref": ref, "sha": rsha, "core": core, "built": time.ctime()})
    m, game, disc = MAC_EMISSARY, cfg["mac_emissary_game"], cfg["mac_emissary_disc"]
    script = f"""
export PATH="$HOME/code/brawlonline.net/brawl-dev/venv/bin:$PATH"
cd "{m}"
[ -d src/.git ] || git clone -q repo.git src
git -C src fetch -q origin staging
git -C src -c advice.detachedHead=false checkout -q -f --detach {rsha}
git -C src clean -q -fdx
if [ ! -f "{game}/sys/main.dol" ]; then
  [ -f "{disc}" ] || {{ echo "no extracted disc in {game} and no disc image {disc} (config.json mac_emissary_disc)" >&2; exit 1; }}
  mkdir -p "$(dirname "{game}")"
  nice -n 10 cmake "-DDISC={disc}" "-DGAME={game}" -P src/extract.cmake
fi
EMISSARY_RELEASE_IN_PLACE=1 EMISSARY_RELEASE_WORK="{m}" EMISSARY_RELEASE_CORE="{MAC_HOME}/src" \\
  nice -n 10 bash src/tools/release-macos.sh "{m}/src" "{game}" "{m}/program" {args.jobs} {tag} {rsha}
P=program/Emissary.app/Contents/MacOS/Emissary; [ -f "$P" ] || P=program/WiiCompiled
grep -q BRAWL_ONLINE_CORE "$P" || {{ echo "$P has no Brawl Online core" >&2; exit 1; }}
rm -rf stage releases && mkdir -p stage releases/{tag}
ditto program stage/EmissaryRecomp
COPYFILE_DISABLE=1 tar -C stage -czf "releases/{tag}/{name}" EmissaryRecomp
printf '%s' {shlex.quote(manifest)} > "releases/{tag}/manifest.json"
printf '%s\\n' {shlex.quote(built)} > build.json
du -h "releases/{tag}/{name}" | cut -f1
"""
    below_normal_priority()
    mac(cfg, script, log=log)
    say(f"EmissaryRecomp {tag} ready on the Mac ({ref} {rsha[:10]}, core {core[:10]}): "
        f"`mac run --engine emissary` plays with it; a running launcher takes it before its next game")


# --------------------------------------------------------------------------- macOS client


def ssh_tool(cfg: dict, name: str) -> str:
    """The ssh or scp to run. On Windows, Git's (OpenSSH 10.5) before System32's: Windows' own
    OpenSSH 9.5 stopped passing on a remote command's output after its first chunk and never
    exited, from Python or PowerShell, with or without -n, so `mac build` hung after the Mac
    finished. config.json "mac_ssh" names another ssh."""
    if cfg.get("mac_ssh"):
        ssh = Path(os.path.expanduser(cfg["mac_ssh"]))
        return str(ssh) if name == "ssh" else str(ssh.with_name(name + ssh.suffix))
    if IS_WINDOWS:
        g = shutil.which("git")
        if g:
            for root in (Path(g).parent.parent, Path(g).parent.parent.parent):
                cand = root / "usr" / "bin" / f"{name}.exe"
                if cand.exists():
                    return str(cand)
    return name


def mac_target(cfg: dict) -> list[str]:
    return [ssh_tool(cfg, "ssh"), "-n", "-i", os.path.expanduser(cfg["mac_key"]), "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10", f"{cfg['mac_user']}@{cfg['mac_host']}"]


# The Mac's shell for every command: Homebrew's tools, the user's nvm Node (CI uses Node 24), and
# ~/code/brawlonline.net/brawl-staging/bin (the ccache shim from `mac setup`).
MAC_PRELUDE = (f'export PATH="{MAC_BIN}:/opt/homebrew/bin:/usr/local/bin:$PATH"; '
               'export NVM_DIR="$HOME/.nvm"; [ -s "$NVM_DIR/nvm.sh" ] && . "$NVM_DIR/nvm.sh" && '
               'nvm use --silent 24 >/dev/null; set -euo pipefail; ')

# CI's macOS script builds through ccache. Without it installed, a pass-through stand-in: compiler
# calls run as they are (ninja still rebuilds only what changed) and its statistics calls do nothing.
# Dolphin's macOS build must not see Homebrew: with its Qt (6.11) found first through an rpath to
# /opt/homebrew/lib, the bundled Qt 6.8.3 plugins were refused ("mismatching Qt versions") and
# Dolphin aborted at start. CI's runners have no Homebrew Qt or libraries; this makes the Mac's
# build the same. A cmake stand-in for that step adds the ignore lists to the configure call.
CMAKE_SHIM = """#!/bin/sh
# Staging (tools/staging/staging.py): Dolphin's configure ignores Homebrew's prefix, as on CI.
real="$HOME/code/brawlonline.net/brawl-dev/venv/bin/cmake"
[ -x "$real" ] || real=/usr/local/bin/cmake
for a in "$@"; do
  if [ "$a" = "-S" ]; then
    exec "$real" "$@" -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew -DCMAKE_SYSTEM_IGNORE_PREFIX_PATH=/opt/homebrew
  fi
done
exec "$real" "$@"
"""

CCACHE_SHIM = """#!/bin/sh
# Pass-through stand-in for ccache (tools/staging/staging.py `mac setup`): no cache.
case "$1" in -*) exit 0 ;; esac
exec "$@"
"""


def mac(cfg: dict, script: str, *, log: Path | None = None, check: bool = True) -> int:
    """Runs a bash script on the Mac (see MAC_PRELUDE; errors stop it)."""
    full = MAC_PRELUDE + script
    first = next((line.strip() for line in script.strip().splitlines() if line.strip()), "")
    multi = len(script.strip().splitlines()) > 1
    label = f"(mac) {first[:90]}" + (" ..." if len(first) > 90 or multi else "")
    return run([*mac_target(cfg), "bash", "-lc", shlex.quote(full)], log=log, check=check, label=label)


def mac_setup(_args: argparse.Namespace) -> None:
    """Checks the Mac's tools; installs nothing (Node comes from the user's nvm). Without a real
    ccache, writes the pass-through shim."""
    cfg = load_config()
    mac(cfg, f"""mkdir -p {MAC_BIN}
command -v node >/dev/null || {{ echo 'no Node: install 24 with nvm (nvm install 24)' >&2; exit 1; }}
echo "node $(node -v)"
if ! command -v ccache >/dev/null || [ -f {MAC_BIN}/ccache ]; then
  printf '%s' {shlex.quote(CCACHE_SHIM)} > {MAC_BIN}/ccache && chmod +x {MAC_BIN}/ccache
  echo "ccache: pass-through shim in ~/code/brawlonline.net/brawl-staging/bin"
fi
for t in git cmake ninja xcrun; do command -v $t >/dev/null || {{ echo "missing $t" >&2; exit 1; }}; done
[ -d {cfg['mac_qt']}/lib/QtCore.framework ] && echo "Qt: {cfg['mac_qt']}" || {{ echo "no Qt in {cfg['mac_qt']}" >&2; exit 1; }}
""")


def mac_build(args: argparse.Namespace) -> None:
    cfg = load_config()
    sha = resolve_ref(args.ref)
    log = LOGS / "mac-build.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(f"mac build {args.ref} {sha}\n")
    mac(cfg, "command -v node >/dev/null && command -v ccache >/dev/null || "
             "{ echo 'node or ccache missing on the Mac: run `mac setup` first' >&2; exit 1; }")
    # 1. The commit, pushed into a bare repository on the Mac.
    mac(cfg, f"mkdir -p {MAC_HOME}; [ -d {MAC_HOME}/repo.git ] || git init -q --bare {MAC_HOME}/repo.git")
    key = os.path.expanduser(cfg["mac_key"]).replace("\\", "/")
    ssh = ssh_tool(cfg, "ssh").replace("\\", "/")
    env = dict(os.environ, GIT_SSH_COMMAND=f'"{ssh}" -i "{key}" -o BatchMode=yes')
    run(["git", "push", "--quiet", "--force",
         f"ssh://{cfg['mac_user']}@{cfg['mac_host']}/~/{MAC_DIR}/repo.git", f"{sha}:refs/heads/staging"],
        env=env, log=log)
    # 2. The plugin: built here from the same commit (the Mac has no PowerPC toolchain).
    plugin = SRC / "game-code" / "PPOnline" / "PPOnline.rel"
    built = json.loads((WIN / "build.json").read_text()) if (WIN / "build.json").exists() else {}
    if built.get("sha") != sha or not plugin.exists():
        say("building the plugin for this commit here first")
        below_normal_priority()
        prepare_worktree(sha)
        run([bash(), "./build.sh"], cwd=SRC / "game-code", log=log)
    run([ssh_tool(cfg, "scp"), "-q", "-i", key, "-o", "BatchMode=yes", str(plugin),
         f"{cfg['mac_user']}@{cfg['mac_host']}:{MAC_DIR}/PPOnline.rel"], log=log)
    rank_badges_here(log)
    badges = STAGE / "rank-badges.tar"
    with tarfile.open(badges, "w") as tar:
        for d in RANK_BADGE_DIRS:
            tar.add(ROOT / d, arcname=d)
    run([ssh_tool(cfg, "scp"), "-q", "-i", key, "-o", "BatchMode=yes", str(badges),
         f"{cfg['mac_user']}@{cfg['mac_host']}:{MAC_DIR}/rank-badges.tar"], log=log)
    # 3. Dolphin and the launcher with CI's own scripts, ad-hoc signed (no identity, no notary).
    h = MAC_HOME
    script = f"""
cd {h}
[ -d src/.git ] || git clone -q repo.git src
cd src
git fetch -q origin staging
git checkout -q --detach {sha}
rm -rf launcher/src/renderer/components/rank_badge/images website/assets/ranks
tar -xf {h}/rank-badges.tar
export QT_DIR={cfg['mac_qt']} CI_CACHE={h}/cache JOBS=$(sysctl -n hw.ncpu)
mkdir -p {h}/dolphin-bin
printf '%s' {shlex.quote(CMAKE_SHIM)} > {h}/dolphin-bin/cmake && chmod +x {h}/dolphin-bin/cmake
B={h}/cache/dolphin-build-macos-arm64
if grep -q /opt/homebrew "$B/CMakeCache.txt" 2>/dev/null; then rm -rf "$B"; fi
env -u HOMEBREW_PREFIX -u HOMEBREW_CELLAR -u HOMEBREW_REPOSITORY -u PKG_CONFIG_PATH   PATH="{h}/dolphin-bin:{MAC_BIN}:$HOME/code/brawlonline.net/brawl-dev/venv/bin:$(dirname "$(command -v node)"):/usr/bin:/bin:/usr/sbin:/sbin"   nice -n 10 .github/scripts/build-dolphin-macos.sh 0.1.0-staging.{sha[:10]} launcher/release/dolphin arm64
D=launcher/release/dolphin/Dolphin.app/Contents/MacOS/Dolphin
if otool -l "$D" | grep -q /opt/homebrew || otool -L "$D" | grep -q /opt/homebrew; then
  echo "the staging Dolphin still refers to Homebrew (/opt/homebrew): not shipping it" >&2; exit 1
fi
cd launcher
lock=$(shasum -a 256 package-lock.json | cut -d' ' -f1)
if [ "$(cat node_modules/.staging-lock 2>/dev/null)" != "$lock" ]; then
  npm ci --no-audit --no-fund && node node_modules/electron/install.js && echo "$lock" > node_modules/.staging-lock
fi
PPO_PLUGIN_PATH={h}/PPOnline.rel npm run stage:plugin
npm run build
unset MAC_SIGN_IDENTITY
nice -n 10 ../.github/scripts/package-launcher-macos.sh {h}/out arm64
pkill -f '{MAC_PROC}' || true
LSREG=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
for old in "{h}/app/Brawl Online.app" "{MAC_APP}"; do
  if [ -d "$old" ]; then "$LSREG" -u "$old" 2>/dev/null || true; rm -rf "$old"; fi
done
mkdir -p {h}/app
ditto "release/build/mac-arm64/Brawl Online.app" "{MAC_APP}"
# Its staging settings inside it (applied however it is started), then re-signed ad hoc.
printf '%s' {shlex.quote(staging_env_json(cfg, f"__HOME__/{MAC_DIR}/profiles/a"))}   | sed "s|__HOME__|$HOME|g" > "{MAC_APP}/Contents/Resources/staging-env.json"
codesign --force --deep --sign - "{MAC_APP}"
"$LSREG" -f "{MAC_APP}"
echo '{{"sha": "{sha}"}}' > {h}/app/build.json
"""
    mac(cfg, script, log=log)
    say(f"macOS client ready on the Mac: {MAC_APP} ({sha[:10]})")


def mac_run(args: argparse.Namespace) -> None:
    cfg = load_config()
    if args.fresh:
        mac_reset(args)
    profile = f"{MAC_HOME}/profiles/{args.profile}"
    env = client_env(cfg, Path("PROFILE"))
    if os.environ.get("PPO_REMOTE_DEBUGGING_PORT"):
        # DevTools on the Mac's launcher (127.0.0.1 there), as `win run` passes its environment on.
        env["PPO_REMOTE_DEBUGGING_PORT"] = os.environ["PPO_REMOTE_DEBUGGING_PORT"]
    envs = " ".join(f"--env {shlex.quote(f'{k}={v}')}" for k, v in env.items() if k != "PPO_USER_DATA_DIR")
    # The P+ release the Mac's build pins (its src/ is the commit it was built from).
    ts = subprocess.run([*mac_target(cfg), f'cat "{MAC_HOME}/src/{PPLUS_RELEASE_TS.as_posix()}"'],
                        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60).stdout
    rel = pinned_pplus(ts)
    if not rel:
        say("warning: no PPLUS_RELEASE in the Mac's src/: the launcher downloads P+ itself")
    def read(path: str) -> str:
        return subprocess.run([*mac_target(cfg), f'cat "{path}" 2>/dev/null || true'], capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=60).stdout

    def write(path: str, text: str) -> str:
        return f'mkdir -p "$(dirname "{path}")"; echo {base64.b64encode(text.encode()).decode()} | base64 -D > "{path}"\n'

    controls = ""
    if args.controls:
        # As set_win_controls: the netplay Dolphin's port 1 and EmissaryRecomp's keyboard switch.
        ini = f"{profile}/{DOLPHIN_INI.as_posix()}"
        controls = write(ini, with_si_devices(read(ini), CONTROLS[args.controls]))
        toml = f"{profile}/{EMISSARY_CONFIG.as_posix()}"
        controls += write(toml, set_toml_value(read(toml), "gc_keyboard", "enabled",
                                               "true" if args.controls == "keyboard" else "false"))
        say(f"port 1: {'the GameCube adapter (EmissaryRecomp: a controller)' if args.controls == 'pad' else 'the keyboard'}")
    # The stage's EmissaryRecomp on the Mac (`emissary build --mac`), as the launcher's feed.
    tag = read(f"{MAC_EMISSARY}/build.json").strip()
    tag = json.loads(tag).get("tag", "") if tag.startswith("{") else ""
    if tag:
        envs += f' --env "PPO_EMISSARY_RELEASES={MAC_EMISSARY}/releases"'
    engine = ""
    if args.engine:
        if args.engine == "emissary" and not tag:
            die("this stage has no EmissaryRecomp build on the Mac: `emissary build --mac` (after `mac build`)")
        # Edited there (macOS's python3), not sent: a used profile's Settings file is too big for a command line
        # (Windows cuts it, and the Mac's shell then saw an unmatched quote).
        settings = f"{profile}/Settings"
        engine = ('/usr/bin/python3 -c "import json, os, sys; p = sys.argv[1]; '
                  'd = json.load(open(p)) if os.path.exists(p) and os.path.getsize(p) else {}; '
                  "d.setdefault('settings', {})['playProjectPlusWith'] = sys.argv[2]; "
                  'os.makedirs(os.path.dirname(p), exist_ok=True); json.dump(d, open(p, \'w\'), indent=2)" '
                  f'"{settings}" {args.engine}\n')
        say(f"Play plays Project+ with {'EmissaryRecomp ' + tag if args.engine == 'emissary' else 'Dolphin'}")
    mac(cfg, f'[ -d "{MAC_APP}" ] || {{ echo "no macOS staging build yet: mac build" >&2; exit 1; }}; '
             f'mkdir -p "{profile}"; {mac_seed_pplus(rel, profile) if rel else ""}\n{controls}{engine}'
             f'open -n {envs} --env "PPO_USER_DATA_DIR={profile}" "{MAC_APP}"')
    say(f"started the macOS client on {cfg['mac_host']}, profile ~/{MAC_DIR}/profiles/{args.profile}")


def mac_stop(_args: argparse.Namespace | None) -> None:
    cfg = load_config()
    mac(cfg, f"pkill -f '{MAC_PROC}' || true", check=False)


def mac_reset(args: argparse.Namespace) -> None:
    cfg = load_config()
    mac_stop(None)
    mac(cfg, f'sleep 1; rm -rf "{MAC_HOME}/profiles/{args.profile}"')
    say(f"deleted the Mac profile {args.profile} (the next start is a fresh install)")


# --------------------------------------------------------------------------- whole environment


def status(args: argparse.Namespace) -> None:
    cfg = load_config()
    print("server:")
    server_status(args)
    built = json.loads((WIN / "build.json").read_text()) if (WIN / "build.json").exists() else None
    print(f"windows client: {built['sha'][:10] + ' (' + built['ref'] + ', ' + built['built'] + ')' if built else 'not built'}")
    for pid, exe in win_processes():
        print(f"  running {Path(exe).name} ({pid})")
    profiles = sorted(p.name for p in (WIN / "profiles").iterdir()) if (WIN / "profiles").exists() else []
    print(f"  profiles: {', '.join(profiles) or 'none'}")
    if (EMISSARY / "build.json").exists():
        e = json.loads((EMISSARY / "build.json").read_text())
        print(f"emissary: {e['tag']} ({e['ref']} {e['sha'][:10]}, core {e['core'][:10]}, {e['built']})")
    print(f"mac client ({cfg['mac_host']}):")
    mac(cfg, f"cat {MAC_HOME}/app/build.json 2>/dev/null || echo '  not built'; "
             f"[ -f {MAC_EMISSARY}/build.json ] && sed 's/^/  emissary: /' {MAC_EMISSARY}/build.json; "
             f"ls {MAC_HOME}/profiles 2>/dev/null | sed 's/^/  profile /'; "
             f"pgrep -fl '{MAC_PROC}' | sed 's/^/  running /' || true", check=False)


def down(args: argparse.Namespace) -> None:
    win_stop(None)
    try:
        mac_stop(None)
    except SystemExit:
        say("could not reach the Mac to stop its client")
    server_down(args)


def destroy(args: argparse.Namespace) -> None:
    """Everything staging made: processes, the worktree (its links first), builds, the database."""
    if not args.yes:
        die(f"this deletes {STAGE.relative_to(ROOT)} (database included) and ~/{MAC_DIR} on the Mac; pass --yes")
    down(args)
    if SRC.exists():
        n = unlink_junctions(SRC)
        say(f"unlinked {n} junctions in the staging worktree")
        run(["git", "worktree", "remove", "--force", str(SRC)], check=False)
    if (EMISSARY / "src").exists():
        run(["git", "-C", str(EMISSARY / "src"), "worktree", "remove", "--force", str(EMISSARY / "src")], check=False)
    shutil.rmtree(STAGE, ignore_errors=True)
    if args.mac:
        cfg_mac = dict(DEFAULTS)
        mac(cfg_mac, f"rm -rf {MAC_HOME}", check=False)
    say("staging removed")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="area", required=True)

    s = sub.add_parser("server", help="the staging server on this PC").add_subparsers(dest="cmd", required=True)
    s.add_parser("up").set_defaults(func=server_up)
    s.add_parser("down").set_defaults(func=server_down)
    s.add_parser("restart").set_defaults(func=lambda a: (server_down(a), server_up(a)))
    s.add_parser("status").set_defaults(func=server_status)
    p = s.add_parser("mail", help="the last mails (verification links)")
    p.add_argument("-n", type=int, default=5)
    p.set_defaults(func=server_mail)
    p = s.add_parser("verify", help="mark an account's email verified")
    p.add_argument("email")
    p.set_defaults(func=lambda a: admin("user", "verify-email", a.email))
    p = s.add_parser("admin", help="run the admin tool against the staging database")
    p.add_argument("rest", nargs=argparse.REMAINDER)
    p.set_defaults(func=lambda a: admin(*a.rest))
    p = s.add_parser("seed-fake", help="fake players, rooms, live games and history (--remove: take them out)")
    p.add_argument("--remove", action="store_true", help="delete the fake accounts and their rows")
    p.add_argument("--include-seed-ranked", action="store_true",
                   help="also give seed_ranked.py's accounts fake friends and matches")
    p.set_defaults(func=server_seed_fake)
    p = s.add_parser("reset-db", help="delete the staging database")
    p.add_argument("--yes", action="store_true")
    p.set_defaults(func=server_reset_db)

    for area, build, run_, stop, reset in (("win", win_build, win_run, win_stop, win_reset),
                                           ("mac", mac_build, mac_run, mac_stop, mac_reset)):
        c = sub.add_parser(area, help=f"the {'Windows' if area == 'win' else 'macOS'} client")
        cs = c.add_subparsers(dest="cmd", required=True)
        p = cs.add_parser("build", help="build and package a commit")
        p.add_argument("--ref", default="main", help="commit to build (default main; committed work only)")
        p.set_defaults(func=build)
        p = cs.add_parser("run", help="start the client against the staging server")
        p.add_argument("--fresh", action="store_true", help="delete the profile first (a fresh install)")
        p.add_argument("--profile", default="a", help="profile name (a second account: --profile b)")
        p.add_argument("--controls", choices=sorted(CONTROLS),
                       help="port 1: the GameCube adapter (EmissaryRecomp: a controller) or the keyboard")
        p.add_argument("--engine", choices=["dolphin", "emissary"],
                       help="what Play plays Project+ with (emissary: the stage's "
                            + ("`emissary build`)" if area == "win" else "`emissary build --mac`)"))
        p.set_defaults(func=run_)
        cs.add_parser("stop").set_defaults(func=stop)
        p = cs.add_parser("reset", help="delete a profile (fresh install on the next run)")
        p.add_argument("--profile", default="a")
        p.set_defaults(func=reset)
        if area == "mac":
            cs.add_parser("setup", help="check the Mac's tools (nvm Node 24, Qt) and add the ccache shim"
                          ).set_defaults(func=mac_setup)

    c = sub.add_parser("emissary", help="EmissaryRecomp for this stage's launchers")
    cs = c.add_subparsers(dest="cmd", required=True)
    p = cs.add_parser("build", help="build it with the core of the stage's `win build`, kept as the stage's release")
    p.add_argument("--ref", default="", help="EmissaryRecomp commit (default: config.json emissary_ref)")
    p.add_argument("--jobs", type=int, default=6)
    p.add_argument("--mac", action="store_true",
                   help="build it on the Mac (Apple silicon) with the core of the stage's `mac build`, for `mac run`")
    p.set_defaults(func=emissary_build)

    sub.add_parser("status", help="server, builds, profiles, running clients").set_defaults(func=status)
    sub.add_parser("down", help="stop the clients and the server (the database stays)").set_defaults(func=down)
    p = sub.add_parser("destroy", help="remove everything staging made")
    p.add_argument("--yes", action="store_true")
    p.add_argument("--mac", action="store_true", help="also delete ~/code/brawlonline.net/brawl-staging on the Mac")
    p.set_defaults(func=destroy)

    args = ap.parse_args()
    dedupe_path()
    args.func(args)


if __name__ == "__main__":
    main()
