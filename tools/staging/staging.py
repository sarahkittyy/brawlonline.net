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

Everything lives in run/staging/ on this PC (gitignored) and ~/brawl-staging/ on the Mac:

    run/staging/config.json   ports, the LAN address, the database password and play-key secret
    run/staging/pgdata/       the staging database (kept until `server reset-db --yes`)
    run/staging/logs/         postgres, accounts, mm, build logs
    run/staging/mail.jsonl    mail the accounts server "sent" (MAILER=file)
    run/staging/src/          a git worktree of the commit being built (own Dolphin build)
    run/staging/cargo-target/ the staging server binaries, built from that commit
    run/staging/win/app/      the packaged Windows launcher (electron-builder's win-unpacked)
    run/staging/win/profiles/ launcher profiles (PPO_USER_DATA_DIR): settings, logins, Dolphin
    ~/brawl-staging/          on the Mac: repo.git, src/, cache/, app/Brawl Online.app, profiles/

A client is the packaged app, started in the launcher's test mode (PPO_TEST_MODE=1) with its own
profile folder, so it never reads or writes the real install's settings, logins or Dolphin.
`--fresh` deletes that profile first: the next start is a first run (ISO, P+ download, sign-up).
The launcher talks to the staging accounts server (PPO_ACCOUNTS_URL), and its Dolphin gets the
staging matchmaking server and accounts URL as -C overrides (PPO_DOLPHIN_EXTRA_ARGS). The update
feed points at the staging server, so a staging build never updates itself to a release.

Nothing here touches production, the shared dolphin/build, or the real installs. Windows asks
once whether accounts.exe, mm.exe and Dolphin.exe may accept connections: allow them on private
networks, or the Mac cannot reach the server (docs/staging.md).
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import secrets
import shlex
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STAGE = ROOT / "run" / "staging"
CONFIG = STAGE / "config.json"
LOGS = STAGE / "logs"
PIDS = STAGE / "pids.json"
SRC = STAGE / "src"
CARGO_TARGET = STAGE / "cargo-target"
WIN = STAGE / "win"
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
    "mac_qt": "~/brawl-dev/qt/6.8.3/macos",
}
# Inside double quotes on the Mac ("~" would not expand there).
MAC_HOME = "$HOME/brawl-staging"
# Its own folder name. The bundle ID stays the real one: Electron's helper apps carry IDs derived
# from it, and a changed main ID made Electron stop at start-up (EXC_BREAKPOINT in ElectronMain).
# What keeps a Dock or Finder start on staging is the staging-env.json inside the app.
MAC_APP = f"{MAC_HOME}/app/Brawl Online Staging.app"
# pgrep/pkill pattern for the staging app and the Dolphin a staging profile installed. The
# bracket keeps it from matching the command line of the shell that runs it (or a build).
MAC_PROC = "brawl-stagin[g]/(app|profiles)/"


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
        h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(h)
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


def load_pids() -> dict[str, int]:
    return json.loads(PIDS.read_text()) if PIDS.exists() else {}


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


# --------------------------------------------------------------------------- Windows client


def junction(link: Path, target: Path) -> None:
    if link.exists() and any(link.iterdir()):
        return
    if link.exists():
        link.rmdir()
    link.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call(["cmd", "/c", "mklink", "/J", str(link), str(target)], stdout=subprocess.DEVNULL)


def unlink_junctions(root: Path) -> int:
    """Removes the junctions `prepare_worktree` made (never their targets)."""
    n = 0
    for rel in [*submodule_paths(), "toolchains"]:
        p = root / rel
        if p.is_junction():
            os.rmdir(p)  # the junction itself, not its target
            n += 1
    return n


def submodule_paths() -> list[str]:
    out = git("ls-files", "-s")
    return [line.split("\t", 1)[1] for line in out.splitlines() if line.startswith("160000 ")]


def prepare_worktree(sha: str) -> None:
    if not (SRC / ".git").exists():
        say(f"creating the staging worktree {SRC.relative_to(ROOT)}")
        run(["git", "worktree", "add", "--detach", str(SRC), sha])
    else:
        dirty = git("status", "--porcelain", "--ignore-submodules=all", "--untracked-files=no", cwd=SRC)
        if dirty:
            die(f"the staging worktree has local changes:\n{dirty}")
        run(["git", "checkout", "--detach", "--quiet", sha], cwd=SRC)
    # The submodules (Dolphin's Externals, game-code's headers) and the plugin toolchain: links to
    # this checkout's copies, as the other worktrees have.
    for path in submodule_paths():
        src = ROOT / path
        if src.exists() and any(src.iterdir()):
            junction(SRC / path, src)
    if (ROOT / "toolchains").exists():
        junction(SRC / "toolchains", ROOT / "toolchains")


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


def msvc(cmd: str, cwd: Path, log: Path, build_dir: Path | None = None) -> None:
    """Runs `cmd` with the MSVC environment of the compiler the build dir was configured with."""
    vcvars = find_vcvars()
    cache = build_dir / "CMakeCache.txt" if build_dir else None
    if cache and cache.exists():
        m = re.search(r"^CMAKE_CXX_COMPILER:\w+=(.+?)/VC/Tools/", cache.read_text(errors="replace"), re.M)
        if m:
            vcvars = Path(m.group(1)) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    run(f'call "{vcvars}" >nul && {cmd}', cwd=cwd, log=log, shell=True)


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
    stage_windows_dolphin(build / "Binaries", launcher / "release" / "dolphin", dsrc, sha, log)
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


def stage_windows_dolphin(binaries: Path, out: Path, dsrc: Path, sha: str, log: Path) -> None:
    """build-dolphin-windows.ps1's staging part: what Dolphin needs at run time, then the manifest."""
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    skip_names = {"Tests", "Updater.exe", "DolphinNoGUI.exe", "portable.txt"}
    for p in binaries.iterdir():
        if p.name in skip_names or p.suffix in (".pdb", ".ilk", ".exp", ".lib"):
            continue
        (shutil.copytree if p.is_dir() else shutil.copy2)(p, out / p.name)
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
    env = dict(os.environ, **client_env(cfg, profile))
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


# --------------------------------------------------------------------------- macOS client


def mac_target(cfg: dict) -> list[str]:
    return ["ssh", "-i", os.path.expanduser(cfg["mac_key"]), "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10", f"{cfg['mac_user']}@{cfg['mac_host']}"]


# The Mac's shell for every command: Homebrew's tools, the user's nvm Node (CI uses Node 24), and
# ~/brawl-staging/bin (the ccache shim from `mac setup`).
MAC_PRELUDE = ('export PATH="$HOME/brawl-staging/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"; '
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
real="$HOME/brawl-dev/venv/bin/cmake"
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
    mac(cfg, f"""mkdir -p {MAC_HOME}/bin
command -v node >/dev/null || {{ echo 'no Node: install 24 with nvm (nvm install 24)' >&2; exit 1; }}
echo "node $(node -v)"
if ! command -v ccache >/dev/null || [ -f {MAC_HOME}/bin/ccache ]; then
  printf '%s' {shlex.quote(CCACHE_SHIM)} > {MAC_HOME}/bin/ccache && chmod +x {MAC_HOME}/bin/ccache
  echo "ccache: pass-through shim in ~/brawl-staging/bin"
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
    env = dict(os.environ, GIT_SSH_COMMAND=f'ssh -i "{key}" -o BatchMode=yes')
    run(["git", "push", "--quiet", "--force",
         f"ssh://{cfg['mac_user']}@{cfg['mac_host']}/~/brawl-staging/repo.git", f"{sha}:refs/heads/staging"],
        env=env, log=log)
    # 2. The plugin: built here from the same commit (the Mac has no PowerPC toolchain).
    plugin = SRC / "game-code" / "PPOnline" / "PPOnline.rel"
    built = json.loads((WIN / "build.json").read_text()) if (WIN / "build.json").exists() else {}
    if built.get("sha") != sha or not plugin.exists():
        say("building the plugin for this commit here first")
        below_normal_priority()
        prepare_worktree(sha)
        run([bash(), "./build.sh"], cwd=SRC / "game-code", log=log)
    run(["scp", "-q", "-i", key, "-o", "BatchMode=yes", str(plugin),
         f"{cfg['mac_user']}@{cfg['mac_host']}:brawl-staging/PPOnline.rel"], log=log)
    # 3. Dolphin and the launcher with CI's own scripts, ad-hoc signed (no identity, no notary).
    h = MAC_HOME
    script = f"""
cd {h}
[ -d src/.git ] || git clone -q repo.git src
cd src
git fetch -q origin staging
git checkout -q --detach {sha}
export QT_DIR={cfg['mac_qt']} CI_CACHE={h}/cache JOBS=$(sysctl -n hw.ncpu)
mkdir -p {h}/dolphin-bin
printf '%s' {shlex.quote(CMAKE_SHIM)} > {h}/dolphin-bin/cmake && chmod +x {h}/dolphin-bin/cmake
B={h}/cache/dolphin-build-macos-arm64
if grep -q /opt/homebrew "$B/CMakeCache.txt" 2>/dev/null; then rm -rf "$B"; fi
env -u HOMEBREW_PREFIX -u HOMEBREW_CELLAR -u HOMEBREW_REPOSITORY -u PKG_CONFIG_PATH   PATH="{h}/dolphin-bin:{h}/bin:$HOME/brawl-dev/venv/bin:$(dirname "$(command -v node)"):/usr/bin:/bin:/usr/sbin:/sbin"   nice -n 10 .github/scripts/build-dolphin-macos.sh 0.1.0-staging.{sha[:10]} launcher/release/dolphin arm64
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
printf '%s' {shlex.quote(staging_env_json(cfg, "__HOME__/brawl-staging/profiles/a"))}   | sed "s|__HOME__|$HOME|g" > "{MAC_APP}/Contents/Resources/staging-env.json"
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
    envs = " ".join(f"--env {shlex.quote(f'{k}={v}')}" for k, v in client_env(cfg, Path("PROFILE")).items()
                    if k != "PPO_USER_DATA_DIR")
    mac(cfg, f'[ -d "{MAC_APP}" ] || {{ echo "no macOS staging build yet: mac build" >&2; exit 1; }}; '
             f'mkdir -p "{profile}"; open -n {envs} --env "PPO_USER_DATA_DIR={profile}" "{MAC_APP}"')
    say(f"started the macOS client on {cfg['mac_host']}, profile ~/brawl-staging/profiles/{args.profile}")


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
    print(f"mac client ({cfg['mac_host']}):")
    mac(cfg, f"cat {MAC_HOME}/app/build.json 2>/dev/null || echo '  not built'; "
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
        die("this deletes run/staging (database included) and ~/brawl-staging on the Mac; pass --yes")
    down(args)
    if SRC.exists():
        n = unlink_junctions(SRC)
        say(f"unlinked {n} junctions in the staging worktree")
        run(["git", "worktree", "remove", "--force", str(SRC)], check=False)
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
        p.set_defaults(func=run_)
        cs.add_parser("stop").set_defaults(func=stop)
        p = cs.add_parser("reset", help="delete a profile (fresh install on the next run)")
        p.add_argument("--profile", default="a")
        p.set_defaults(func=reset)
        if area == "mac":
            cs.add_parser("setup", help="check the Mac's tools (nvm Node 24, Qt) and add the ccache shim"
                          ).set_defaults(func=mac_setup)

    sub.add_parser("status", help="server, builds, profiles, running clients").set_defaults(func=status)
    sub.add_parser("down", help="stop the clients and the server (the database stays)").set_defaults(func=down)
    p = sub.add_parser("destroy", help="remove everything staging made")
    p.add_argument("--yes", action="store_true")
    p.add_argument("--mac", action="store_true", help="also delete ~/brawl-staging on the Mac")
    p.set_defaults(func=destroy)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
