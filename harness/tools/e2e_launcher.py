#!/usr/bin/env python3
"""The whole path a real player takes, end to end on one machine, twice side by side:

    launcher (Electron, its own profile) -> log in -> Play
      -> Dolphin.exe (Qt, windowed, D3D11, muted) with the launcher-patched SD card (PPOnline.rel)
      -> the game's own menus: main menu -> PLAY ONLINE -> WITH FRIENDS -> a character -> START
      -> the other player's connect code on Brawl's keypad -> START
      -> matchmaking (our local accounts + mm services) -> the hand-off to the gameplay-only
         (Slippi-style) session -> a two-game Direct set from the online CSS, no reboot:
         game 1 on a random legal stage, back to the CSS, the loser picks game 2's stage on the
         stage select, game 2, back to the CSS (harness/tools/online_set.py).

Two launcher instances run with separate Electron user-data dirs (``PPO_USER_DATA_DIR``), each
logs in as its own account through the launcher's login form and presses Play. The launcher's
test mode passes ``--harness-port`` (``PPO_HARNESS_PORT``) and a few Dolphin settings
(``PPO_DOLPHIN_EXTRA_ARGS``) through to Dolphin; everything in the game is then driven with
controller input through the harness. Nothing plays Dolphin's or the game's part from outside.

Screenshots: the launcher windows through the DevTools protocol (the page only, never the
desktop) and the game through the harness ``screenshot``. Everything goes to
``run/artifacts/e2e-launcher/<prefix>/``; a ``summary.json`` there lists what was checked.

    python harness/tools/e2e_launcher.py                     # the full run
    python harness/tools/e2e_launcher.py --prefix e2el --keep
    python harness/tools/e2e_launcher.py --direct-dolphin    # skip the launchers (debugging)

Needs: the launcher built (``npm run build`` in launcher/), Dolphin built, the plugin built
(``game-code/build.sh``), the server binaries (``cargo build --workspace`` in server/), the
portable Postgres (server/README.md), a Brawl NTSC-U disc image (rev 1 or 2) and
``run/template-user`` (P+'s User folder: launcher DOLs and SD card).

Cleanup: every process started here is killed (launchers, their Dolphins, the servers). The
per-run dir ``run/e2e/<prefix>-<time>/`` (profiles with their 2 GB SD copies, backend data) is
deleted unless ``--keep``; a kept dir has its ``sd.raw`` files deleted.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness"))
sys.path.insert(0, str(ROOT / "harness" / "tests"))
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))

from ppharness import _platform, paths  # noqa: E402
from ppharness.backend import OnlineBackend, OnlineUser  # noqa: E402
from ppharness.cdp import CdpError, CdpSession, wait_for_page  # noqa: E402
from ppharness.client import HarnessClient, HarnessError  # noqa: E402
from ppharness.instance import find_free_port  # noqa: E402

import drive  # noqa: E402
import online_set  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

LAUNCHER = ROOT / "launcher"
ARTIFACTS = ROOT / "run" / "artifacts" / "e2e-launcher"
RUNS = ROOT / "run" / "e2e"
PLUGIN = ROOT / "game-code" / "PPOnline" / "PPOnline.rel"
NETPLAY_DOL = "Project+ Netplay Launcher.dol"

# PPOM layout (game-code/PPOnline/include/ppom.h): the response area of the mailbox.
MB_RESP = 0x210
CMD_GET_MATCH_STATE = 0xB3


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    # Page text can hold characters the console's code page cannot print (a zero-width space).
    enc = getattr(sys.stdout, "encoding", None) or "utf-8"
    print(line.encode(enc, errors="replace").decode(enc, errors="replace"), flush=True)


def wait_until(pred: Callable[[], Any], timeout: float, what: str, interval: float = 0.25) -> Any:
    deadline = time.monotonic() + timeout
    last_exc: BaseException | None = None
    while time.monotonic() < deadline:
        try:
            v = pred()
            if v:
                return v
        except (HarnessError, CdpError, OSError) as e:
            last_exc = e
        time.sleep(interval)
    raise TimeoutError(f"timed out after {timeout:.0f}s waiting for {what}"
                       + (f" (last error: {last_exc})" if last_exc else ""))


# --------------------------------------------------------------------------- inputs on disk


def find_iso(explicit: str | None) -> Path:
    for cand in [explicit, os.environ.get("PPHARNESS_ISO"), paths.game_iso(),
                 ROOT / "game" / "SSBB_NTSC.iso", ROOT / "Super Smash Bros. Brawl (USA) (Rev 2).iso"]:
        if cand and Path(cand).is_file():
            return Path(cand)
    raise SystemExit("no Brawl disc image found (--iso)")


def electron_exe() -> Path:
    exe = LAUNCHER / "node_modules" / "electron" / "dist" / ("electron.exe" if _platform.IS_WINDOWS
                                                             else "electron")
    if not exe.exists():
        raise SystemExit(f"{exe} not found (npm install in launcher/)")
    return exe


def launcher_build_stale() -> list[str]:
    """Why the built launcher (release/app/dist) is older than its sources, if it is: the bundle
    must be newer than launcher/'s last commit and than every file under src/ and locales/ (an
    e2e run once drove a bundle built before the Brawl Online rename and still saw "PlusOnline
    Online Rules")."""
    app = LAUNCHER / "release" / "app" / "dist"
    bundles = [app / "main" / "main.js", app / "renderer" / "renderer.js"]
    built = min(b.stat().st_mtime for b in bundles if b.exists()) if all(
        b.exists() for b in bundles) else 0.0
    why: list[str] = []
    try:
        head = subprocess.run(["git", "-C", str(LAUNCHER), "log", "-1", "--format=%ct %h"],
                              capture_output=True, text=True, timeout=30).stdout.split()
        if head and float(head[0]) > built:
            why.append(f"launcher commit {head[1]} is newer than the bundle")
    except (OSError, subprocess.TimeoutExpired, ValueError):
        pass
    for d in ("src", "locales"):
        for f in (LAUNCHER / d).rglob("*"):
            if f.is_file() and f.stat().st_mtime > built:
                why.append(f"{f.relative_to(LAUNCHER)} is newer than the bundle")
                break
    return why


def launcher_app_dir(allow_stale: bool = False) -> Path:
    app = LAUNCHER / "release" / "app"
    if not (app / "dist" / "main" / "main.js").exists():
        raise SystemExit(f"the launcher is not built ({app / 'dist' / 'main' / 'main.js'} is "
                         "missing): run `npm run build` in launcher/")
    stale = launcher_build_stale()
    if stale and not allow_stale:
        raise SystemExit("the launcher build is stale (" + "; ".join(stale) + "): run "
                         "`npm run build` in launcher/, or pass --allow-stale-launcher")
    return app


# --------------------------------------------------------------------------- Windows process info


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", ctypes.c_uint32), ("cntUsage", ctypes.c_uint32),
                ("th32ProcessID", ctypes.c_uint32), ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", ctypes.c_uint32), ("cntThreads", ctypes.c_uint32),
                ("th32ParentProcessID", ctypes.c_uint32), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", ctypes.c_uint32), ("szExeFile", ctypes.c_wchar * 260)]


def process_table() -> list[tuple[int, int, str]]:
    """(pid, parent pid, exe name) for every process (Windows; empty elsewhere)."""
    if not _platform.IS_WINDOWS:
        return []
    k32 = ctypes.windll.kernel32
    k32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    out: list[tuple[int, int, str]] = []
    e = _PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32FirstW(ctypes.c_void_p(snap), ctypes.byref(e))
    while ok:
        out.append((e.th32ProcessID, e.th32ParentProcessID, e.szExeFile))
        ok = k32.Process32NextW(ctypes.c_void_p(snap), ctypes.byref(e))
    k32.CloseHandle(ctypes.c_void_p(snap))
    return out


def descendants(pid: int, name: str | None = None) -> list[int]:
    table = process_table()
    kids: dict[int, list[tuple[int, str]]] = {}
    for p, pp, n in table:
        kids.setdefault(pp, []).append((p, n))
    out, todo = [], [pid]
    while todo:
        cur = todo.pop()
        for p, n in kids.get(cur, []):
            todo.append(p)
            if name is None or n.lower() == name.lower():
                out.append(p)
    return out


def window_titles(pid: int) -> list[str]:
    """Titles of the visible top-level windows of a process (Windows)."""
    if not _platform.IS_WINDOWS:
        return []
    user32 = ctypes.windll.user32
    titles: list[str] = []
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(hwnd: Any, _: Any) -> bool:
        owner = ctypes.c_uint32()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            n = user32.GetWindowTextLengthW(ctypes.c_void_p(hwnd))
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(ctypes.c_void_p(hwnd), buf, n + 1)
            titles.append(buf.value)
        return True

    user32.EnumWindows(proto(cb), 0)
    return titles


# --------------------------------------------------------------------------- launcher (CDP)

# Small DOM helpers evaluated in the launcher's renderer. React-controlled inputs need the
# native value setter plus an input event.
JS_HELPERS = r"""
window.__e2e = window.__e2e || {
  visible(el) { return !!(el && el.offsetParent !== null); },
  text(el) { return (el.innerText || el.textContent || "").trim(); },
  clickText(text, selector) {
    const els = [...document.querySelectorAll(selector || "button,[role=button],a,label")];
    const want = text.toLowerCase();  // innerText follows CSS text-transform ("PLAY")
    const el = els.find((e) => this.visible(e) && this.text(e).toLowerCase() === want && !e.disabled);
    if (!el) return false;
    el.click();
    return true;
  },
  inputByLabel(label) {
    const l = [...document.querySelectorAll("label")].find(
      (x) => this.visible(x) && this.text(x).replace(/\s*\*$/, "") === label);
    return l && l.htmlFor ? document.getElementById(l.htmlFor) : null;
  },
  hasInput(label) { return !!this.inputByLabel(label); },
  setInput(label, value) {
    const el = this.inputByLabel(label);
    if (!el) return "missing " + label;
    const proto = el.tagName === "TEXTAREA" ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(proto, "value").set.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
    return "ok";
  },
  checkLabel(text) {
    const l = [...document.querySelectorAll("label")].find((x) => this.visible(x) && this.text(x) === text);
    const box = l && l.querySelector("input[type=checkbox]");
    if (!box) return false;
    if (!box.checked) box.click();
    return true;
  },
  hasText(text) { return (document.body.innerText || "").includes(text); },
  buttons() {
    return [...document.querySelectorAll("button")].filter((b) => this.visible(b)).map((b) => this.text(b));
  },
};
true
"""


class Launcher:
    """One launcher process with its own profile, driven through its DevTools port."""

    def __init__(self, name: str, profile: Path, env: dict[str, str], out: Path):
        self.name = name
        self.profile = profile
        self.env = env
        self.out = out
        self.cdp_port = int(env["PPO_REMOTE_DEBUGGING_PORT"])
        self.proc: subprocess.Popen[bytes] | None = None
        self.s: CdpSession | None = None
        self._log_f: Any = None
        self.shots: list[str] = []

    def start(self) -> None:
        self.profile.mkdir(parents=True, exist_ok=True)
        self._log_f = open(self.profile.parent / f"{self.name}-launcher.log", "wb")  # noqa: SIM115
        # The built launcher, unpackaged (launcher/PPLUS_PORTING.md, "Test mode"): test mode is
        # on, and PPO_REMOTE_DEBUGGING_PORT / PPO_USER_DATA_DIR come from the environment.
        cmd = [str(electron_exe()), "release/app"]
        log(f"{self.name}: launcher {' '.join(cmd)}")
        self.proc = subprocess.Popen(cmd, cwd=LAUNCHER, env=self.env, stdout=self._log_f,
                                     stderr=subprocess.STDOUT, **_platform.popen_kwargs())
        _platform.bind_to_parent(self.proc)
        page = wait_for_page(self.cdp_port, timeout=90, url_prefix="")
        self.s = CdpSession(page["webSocketDebuggerUrl"], timeout=60)
        wait_until(lambda: self.eval("document.readyState") == "complete" and
                   self.eval("document.body && document.body.innerText.length > 0"),
                   60, f"{self.name}: the launcher page")

    def eval(self, js: str, timeout: float | None = None) -> Any:
        assert self.s
        return self.s.evaluate(js, timeout=timeout)

    def helpers(self) -> None:
        self.eval(JS_HELPERS)

    def call(self, fn: str, *args: Any) -> Any:
        self.helpers()
        return self.eval(f"window.__e2e.{fn}({', '.join(json.dumps(a) for a in args)})")

    def shot(self, label: str) -> Path:
        assert self.s
        p = self.s.screenshot(self.out / f"{label}.png")
        self.shots.append(str(p))
        log(f"{self.name}: screenshot {p}")
        return p

    def wait_text(self, text: str, timeout: float = 60) -> None:
        wait_until(lambda: self.call("hasText", text), timeout, f"{self.name}: '{text}'")

    def click(self, text: str, timeout: float = 30) -> None:
        wait_until(lambda: self.call("clickText", text), timeout, f"{self.name}: button '{text}'")

    def body(self) -> str:
        return str(self.eval("document.body.innerText") or "")

    def login(self, user: OnlineUser) -> None:
        """The quick start's login step: email + password, Log in. (Sign-up and the connect
        code were done for the account beforehand, as a returning player would have.)"""
        wait_until(lambda: self.call("hasInput", "Email") or self.call("hasText", "Log in"), 60,
                   f"{self.name}: login form")
        if not self.call("hasInput", "Email"):
            # Not on the form yet: a "Log in" button leads there.
            self.click("Log in")
        wait_until(lambda: self.call("setInput", "Email", user.email) == "ok", 30,
                   f"{self.name}: email field")
        assert self.call("setInput", "Password", user.password) == "ok"
        self.shot("01-login")
        self.click("Log in")

    def finish_quick_start(self, iso: Path, timeout: float = 240) -> None:
        """Whatever quick-start steps follow the login: accept the rules, choose the disc
        (dragged onto the drop zone, as a player can), continue. Done when Play shows."""
        deadline = time.monotonic() + timeout
        iso_dropped = False
        while time.monotonic() < deadline:
            text = self.body()
            if self.call("clickText", "Play", "button,[role=button],div,span") is False and \
                    "Play" in self.call("buttons"):
                pass
            if self._home_ready():
                return
            if "Accept rules and policies" in text:
                # The rules checkbox's label as the page has it: "I accept the Brawl Online
                # Rules" since the rename (launcher 6d0c6913); a stale bundle said "PlusOnline
                # Online Rules". Kept for the summary and checked by run().
                m = re.search(r"I accept the [^\n]*?Rules", text)
                self.rules_label = m.group(0) if m else None
                self.call("checkLabel", m.group(0) if m else f"I accept the {self._product()} Rules")
                self.call("checkLabel",
                          f"I accept the {self._product()} Privacy Policy and Terms of Service")
                self.shot("02-accept-rules")
                self.call("clickText", "Accept All")
            elif "Select Brawl ISO" in text and not iso_dropped:
                self.shot("03-select-iso")
                self.drop_file(str(iso))
                iso_dropped = True
            elif "Nice work!" in text:
                self.shot("04-setup-complete")
                self.call("clickText", "Continue")
            elif "Wrong email or password" in text:
                raise RuntimeError(f"{self.name}: the launcher refused the login")
            time.sleep(1.0)
        raise TimeoutError(f"{self.name}: the quick start did not finish; page says:\n{self.body()[:800]}")

    def _product(self) -> str:
        m = re.search(r"I accept the (.+?) Privacy Policy and Terms of Service", self.body())
        return m.group(1) if m else "Brawl Online"

    def _home_ready(self) -> bool:
        return bool(self.eval(
            "[...document.querySelectorAll('button')]"
            ".some((b) => b.offsetParent !== null && (b.innerText || '').trim().toLowerCase() === 'play')"))

    def drop_file(self, path: str) -> None:
        """Drags a file from outside onto the ISO step's drop zone (CDP Input.dispatchDragEvent,
        what a drag from Explorer produces). Falls back to the settings API the step calls."""
        assert self.s
        rect = self.eval("""(() => {
            const b = [...document.querySelectorAll('button')].find((x) => (x.innerText||'').trim() === 'Select');
            const zone = b ? b.parentElement : null;
            if (!zone) return null;
            const r = zone.getBoundingClientRect();
            return {x: r.x + r.width / 2, y: r.y + r.height / 2};
        })()""")
        if rect:
            data = {"items": [], "files": [path], "dragOperationsMask": 1}
            for t in ("dragEnter", "dragOver", "drop"):
                self.s.send("Input.dispatchDragEvent", {"type": t, "x": rect["x"], "y": rect["y"],
                                                        "data": data})
            log(f"{self.name}: dropped {path} on the ISO step")
            try:
                wait_until(lambda: "Verifying ISO" in self.body() or
                           "Select Brawl ISO" not in self.body(), 10, "the ISO check to start")
                return
            except TimeoutError:
                log(f"{self.name}: the drop was not taken; using the settings API instead")
        self.eval("window.electron.settings.updateSettings([{key: 'isoPath', value: %s}])"
                  % json.dumps(path))

    def play(self) -> None:
        self.shot("05-home")
        self.click("Play")
        log(f"{self.name}: pressed Play")

    def stop(self) -> None:
        if self.s:
            try:
                self.s.close()
            except OSError:
                pass
            self.s = None
        if self.proc and self.proc.poll() is None:
            _platform.kill_process_tree(self.proc)
        if self._log_f:
            self._log_f.close()
            self._log_f = None


# --------------------------------------------------------------------------- the game


@dataclass
class Player:
    name: str
    code_start: str
    out: Path
    user: OnlineUser | None = None
    harness_port: int = 0
    launcher: Launcher | None = None
    dolphin: subprocess.Popen[bytes] | None = None  # --direct-dolphin only
    dolphin_pid: int = 0
    c: HarnessClient | None = None
    shots: list[str] = field(default_factory=list)
    facts: dict[str, Any] = field(default_factory=dict)

    # -- harness
    def connect(self, timeout: float = 180) -> None:
        def alive() -> bool:
            if self.dolphin is not None:
                return self.dolphin.poll() is None
            return self.launcher is not None and self.launcher.proc is not None and \
                self.launcher.proc.poll() is None
        self.c = HarnessClient.connect_with_retry(self.harness_port, total_timeout=timeout,
                                                  is_alive=alive, timeout=60.0)
        log(f"{self.name}: harness connected on {self.harness_port}: {self.c.ping()}")

    def steps(self, *steps: str) -> None:
        assert self.c
        drive.run(self.c, list(steps), out=str(self.out))

    def shot(self, name: str) -> Path:
        assert self.c
        p = (self.out / f"{name}.png").resolve()
        p.parent.mkdir(parents=True, exist_ok=True)
        self.c.screenshot(str(p))
        self.shots.append(str(p))
        log(f"{self.name}: game screenshot {p}")
        return p

    def bridge(self) -> dict[str, Any]:
        assert self.c
        return self.c.game_bridge_status()

    def debug_scratch(self) -> list[int]:
        import struct
        assert self.c
        block = int(self.bridge()["block"])
        debug_off = struct.unpack(">H", self.c.read_mem(block + 0x14, 2))[0]
        return list(struct.unpack(">16I", self.c.read_mem(block + debug_off + 0x18, 64)))

    def finds(self) -> int:
        return int(self.bridge()["by_cmd"].get("FIND_OPPONENT", 0))

    def press_until(self, button: str, done: Callable[[], Any], what: str, tries: int = 4,
                    timeout: float = 3.0) -> Any:
        for _ in range(tries):
            self.steps(f"tap {button} 8")
            try:
                return wait_until(done, timeout, what)
            except TimeoutError:
                continue
        raise TimeoutError(f"{self.name}: {button} x{tries}: still waiting for {what}")

    # -- menus (same steps as harness/tests/test_online_game.py)
    def boot_to_main_menu(self) -> None:
        assert self.c
        wait_until(lambda: self.c.status().state == "running", 120, f"{self.name}: emulation")
        wait_until(lambda: self.bridge()["found"], 180, f"{self.name}: the plugin's PPOM block")
        # With the plugin, Play boots to the main menu's ONLINE page (game-code boot_menu.cpp).
        self.steps("until muMenuMain 4000", "wait 60")
        self.shot("01-boot-online-page")

    def to_online_css(self) -> None:
        # B to the main menu (PLAY ONLINE highlighted) and back into the ONLINE page.
        self.steps("tap B", "wait 60")
        self.shot("02-play-online")
        self.steps("tap A", "wait 90")
        self.shot("03-online-page")
        # WITH FRIENDS -> its page (BASIC VERSUS = Direct, TEAM BATTLE = Teams).
        self.steps("tap A", "wait 90")
        self.shot("03b-with-friends")
        self.steps("tap A", "wait 400", "until scSelctCharacter 600")
        # A character (the hand starts on the P1 panel; up to the second row).
        self.steps("stick up 30", "wait 5", "tap A 8", "wait 30")
        self.shot("04-css-direct")
        self.panel = online_set.css_panel(self.c)   # the pick the CSS must show again later

    def open_keypad(self) -> None:
        for _ in range(6):
            self.steps("tap START 8", "wait 40")
            s = self.debug_scratch()
            if s[4] == 1 and s[5] == 8:
                return
            self.steps("wait 60")
        raise RuntimeError(f"{self.name}: the code keypad did not open ({self.debug_scratch()})")

    def enter_code(self, code: str, settle: bool) -> None:
        from test_online_game import keypad_steps  # the keypad layout and typing plan
        self.open_keypad()
        self.shot("05-keypad")
        self.steps(*keypad_steps(code))
        self.shot("06-code-typed")
        if settle:
            n = self.finds()
            self.press_until("START", lambda: self.finds() > n, "FIND_OPPONENT")
        else:
            assert self.c
            from ppharness.client import PadInput
            self.c.pad_script(0, [PadInput(buttons=["START"], hold=8), PadInput(hold=6)])


def dolphin_extra_args(be: OnlineBackend) -> list[str]:
    """What the launcher's test mode appends to Dolphin's command line. The User folder and the
    patched SD card come from the launcher itself (-u, -C ...WiiSDCardPath)."""
    return ["-v", "D3D11",
            "-C", "Dolphin.DSP.Muted=True",
            "-C", "Dolphin.Core.CPUThread=True",
            "-C", "Dolphin.Core.SIDevice0=6",          # a standard controller for the harness
            "-C", "Dolphin.Input.BackgroundInput=False",
            "-C", "Dolphin.Online.UseDevServer=True",
            "-C", f"Dolphin.Online.MatchmakingPort={be.mm_port}",
            "-C", f"Dolphin.Online.DevAccountsUrl={be.accounts_url}"]


def launcher_env(p: Player, be: OnlineBackend, profile: Path, dolphin: Path, cdp_port: int) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PPO_", "ELECTRON_"))}
    env.update({
        "PPO_TEST_MODE": "1",
        "PPO_USER_DATA_DIR": str(profile),
        "PPO_REMOTE_DEBUGGING_PORT": str(cdp_port),
        "PPO_HARNESS_PORT": str(p.harness_port),
        "PPO_DOLPHIN_EXTRA_ARGS": json.dumps(dolphin_extra_args(be)),
        "PPO_ACCOUNTS_URL": be.accounts_url,
        "PPO_DOLPHIN_PATH": str(dolphin),
        "PPO_DOLPHIN_USER_TEMPLATE": str(paths.template_user_dir()),
        "PPO_PLUGIN_PATH": str(PLUGIN),
        "NODE_ENV": "production",
    })
    return env


def start_direct_dolphin(p: Player, be: OnlineBackend, run_dir: Path, dolphin: Path) -> None:
    """--direct-dolphin: what the launcher does on Play, without the launcher (debugging aid):
    a User folder from the template, the plugin on a copy of its SD card, user.json, then
    Dolphin.exe with -u/-e and the same extra arguments."""
    import patch_sd
    assert p.user
    user_dir = run_dir / p.name / "User"
    if not user_dir.exists():
        tpl = paths.template_user_dir()
        shutil.copytree(tpl, user_dir, ignore=shutil.ignore_patterns(
            "Load", "Logs", "Cache", "Dump", "ScreenShots", "StateSaves", "Shaders", "sd.raw"))
        (user_dir / "Wii").mkdir(parents=True, exist_ok=True)
    # As ppharness.instance does: SDL must not open the user's GameCube adapter.
    from ppharness.inifile import IniFile
    from ppharness.instance import SDL_HINT_GC_ADAPTER
    ini_path = user_dir / "Config" / "Dolphin.ini"
    ini_path.parent.mkdir(parents=True, exist_ok=True)
    ini = IniFile.load(ini_path)
    ini.update({"SDL_Hints": {SDL_HINT_GC_ADAPTER: "0"}})
    ini.save(ini_path)
    sd = run_dir / p.name / "sd" / "sd.raw"
    sd.parent.mkdir(parents=True, exist_ok=True)
    _platform.fast_copy_file(paths.template_user_dir() / "Wii" / "sd.raw", sd)
    patch_sd.patch_image(sd, [(PLUGIN.read_bytes(), f"{patch_sd.PLUGIN_DIR}/{PLUGIN.name}")])
    p.user.write_user_json(user_dir)
    cmd = [str(dolphin), "-u", str(user_dir), "-C", f"Dolphin.General.WiiSDCardPath={sd}",
           "-e", str(user_dir / "Launcher" / NETPLAY_DOL), "--harness-port", str(p.harness_port),
           *dolphin_extra_args(be)]
    log(f"{p.name}: {' '.join(cmd)}")
    f = open(run_dir / p.name / "dolphin-stdout.txt", "wb")  # noqa: SIM115
    p.dolphin = subprocess.Popen(cmd, cwd=run_dir / p.name, stdout=f, stderr=subprocess.STDOUT,
                                 **_platform.popen_kwargs())
    _platform.bind_to_parent(p.dolphin)
    p.dolphin_pid = p.dolphin.pid


def both(fa: Callable[[], Any], fb: Callable[[], Any]) -> None:
    with concurrent.futures.ThreadPoolExecutor(2) as ex:
        for f in [ex.submit(fa), ex.submit(fb)]:
            f.result()


# --------------------------------------------------------------------------- the run


def run(args: argparse.Namespace) -> dict[str, Any]:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    run_name = f"{args.prefix}-{stamp}"
    run_dir = RUNS / run_name
    out_root = ARTIFACTS / run_name
    run_dir.mkdir(parents=True, exist_ok=True)
    out_root.mkdir(parents=True, exist_ok=True)
    dolphin = Path(args.dolphin) if args.dolphin else paths.dolphin_gui()
    iso = find_iso(args.iso)
    if not PLUGIN.exists():
        raise SystemExit(f"{PLUGIN} not built (game-code/build.sh)")
    if not dolphin.exists():
        raise SystemExit(f"{dolphin} not found")
    if not args.direct_dolphin:
        launcher_app_dir(args.allow_stale_launcher)   # built, and not older than its sources
    summary: dict[str, Any] = {"run": run_name, "dolphin": str(dolphin), "iso": str(iso),
                               "direct_dolphin": args.direct_dolphin, "ok": False}
    log(f"run {run_name}: dolphin {dolphin}, iso {iso}")

    be = OnlineBackend(run_dir / "backend")
    players = [Player("alice", "ALIC", out_root / "alice"), Player("bob", "BOB", out_root / "bob")]
    try:
        for attempt in range(3):
            try:
                be.start()
                break
            except Exception as e:  # noqa: BLE001
                # The portable Postgres on Windows sometimes drops its first connections.
                log(f"servers did not start ({str(e)[:300]}); retrying")
                be.stop()
                if attempt == 2:
                    raise
                be = OnlineBackend(run_dir / f"backend{attempt + 1}")
        log(f"servers: accounts {be.accounts_url}, mm udp {be.mm_port}")
        for p in players:
            p.user = be.create_user(p.name, p.code_start)
            p.harness_port = find_free_port()
            log(f"{p.name}: account {p.user.email} code {p.user.connect_code}")
        summary["accounts"] = {p.name: {"email": p.user.email, "code": p.user.connect_code}
                               for p in players if p.user}

        # 1. Launchers: log in, quick start, Play.
        if args.direct_dolphin:
            for p in players:
                start_direct_dolphin(p, be, run_dir, dolphin)
        else:
            for p in players:
                cdp_port = find_free_port()
                profile = run_dir / p.name / "profile"
                p.launcher = Launcher(p.name, profile,
                                      launcher_env(p, be, profile, dolphin, cdp_port),
                                      p.out / "launcher")

            def launch(p: Player) -> None:
                assert p.launcher and p.user
                p.launcher.start()
                p.launcher.shot("00-start")
                p.launcher.login(p.user)
                p.launcher.finish_quick_start(iso)
                # The rules page of the quick start (seen in login or finish_quick_start).
                label = getattr(p.launcher, "rules_label", None)
                summary.setdefault("rules_labels", {})[p.name] = label
                if label is not None and label != "I accept the Brawl Online Rules":
                    raise RuntimeError(f"{p.name}: the launcher's rules label is {label!r}")
                p.launcher.play()

            both(lambda: launch(players[0]), lambda: launch(players[1]))

        # 2. Dolphin answers on the harness port; the game boots with the plugin.
        both(players[0].connect, players[1].connect)
        for p in players:
            if p.launcher and p.launcher.proc:
                pids = descendants(p.launcher.proc.pid, "Dolphin.exe")
                p.dolphin_pid = pids[0] if pids else 0
            st = p.c.status() if p.c else None
            p.facts["dolphin_pid"] = p.dolphin_pid
            p.facts["video_backend"] = st.video_backend if st else None
            p.facts["cpu_thread"] = st.cpu_thread if st else None
            p.facts["audio_muted"] = st.raw.get("audio_muted") if st else None
            assert p.facts["audio_muted"] is True, "Dolphin must run muted"
        if not args.direct_dolphin:
            for p in players:
                p.launcher.shot("06-playing")  # type: ignore[union-attr]

        both(players[0].boot_to_main_menu, players[1].boot_to_main_menu)
        for p in players:
            st = p.bridge()
            p.facts["online_status"] = (st.get("last_response") or {}).get("text")
            assert st["by_cmd"].get("GET_ONLINE_STATUS"), st
            assert p.user and p.facts["online_status"] == (
                f"GET_ONLINE_STATUS state=1 name='{p.user.display_name}' "
                f"code='{p.user.connect_code}'"), p.facts["online_status"]
        both(players[0].to_online_css, players[1].to_online_css)
        # Test shortcut: 2 stocks and 2 minutes instead of P+'s 4 and 8 (written into the set
        # rule, where the plugin put the online rules; both must be the same, they are part of
        # the match setup), so that the two games take minutes, not a quarter of an hour.
        for p in players:
            assert p.c
            B.write_rules(p.c, stocks=2, minutes=2, items_off=True)
            ppom.allow_test_rules(p.c)   # the plugin's match setup keeps these stocks and times

        # 3. Each enters the other's code. Alice first, then Bob: the server pairs them.
        a, b = players
        assert a.user and b.user
        a.enter_code(b.user.connect_code, settle=True)
        wait_until(lambda: a.c.mm_status()["state"] in ("initializing", "matchmaking"), 20,  # type: ignore[union-attr]
                   "alice searching")
        a.steps("wait 30")
        a.shot("07-searching")
        time.sleep(2.2)  # the server takes one ticket per account per 2 s
        b.enter_code(a.user.connect_code, settle=False)

        # 4. Matched, connected, handed off to the gameplay session ([Online] SessionBackend's
        # default): both games stay where they are, on their own online CSS.
        sa = wait_until(lambda: (lambda s: s if s["handoff"] == "started" else None)(a.c.mm_status()),  # type: ignore[union-attr]
                        90, "alice's hand-off")
        sb = wait_until(lambda: (lambda s: s if s["handoff"] == "started" else None)(b.c.mm_status()),  # type: ignore[union-attr]
                        90, "bob's hand-off")
        assert sa["match"]["match_id"] == sb["match"]["match_id"], (sa, sb)
        summary["match"] = {"match_id": sa["match"]["match_id"],
                            "alice_role": "host" if sa["match"]["is_host"] else "guest",
                            "bob_role": "host" if sb["match"]["is_host"] else "guest"}
        log(f"matched {summary['match']}")
        for p in players:
            assert p.c
            p.facts["session"] = p.c.mm_status()["session"]
            assert p.facts["session"]["backend"] == "gameplay", p.facts["session"]
            p.facts["windows"] = window_titles(p.dolphin_pid) if p.dolphin_pid else []
            # Slippi shows no netplay window; neither do we. The main window is Brawl Online's.
            assert not any("netplay" in t.lower() for t in p.facts["windows"]), p.facts["windows"]
            assert any(t == "Brawl Online" for t in p.facts["windows"]), p.facts["windows"]

        # 5. The set: two games, no reboot (online_set checks it), back on the CSS after each.
        set_rep = online_set.play_set(players, games=2, mode="direct", log=log,
                                      panels={p.name: p.panel for p in players})
        summary["set"] = set_rep
        for p in players:
            assert p.c
            p.facts["rollback"] = {
                "session_started": True,
                "games": [{k: g[k] for k in ("stage", "frames", "end", "rollbacks", "checksums")}
                          for g in set_rep["games"]],
                "desyncs_detected": sum(g["checksums"]["mismatches"] for g in set_rep["games"])}
            p.facts["windows"] = window_titles(p.dolphin_pid) if p.dolphin_pid else []
        if not args.direct_dolphin:
            for p in players:
                p.launcher.shot("07-after-the-set")  # type: ignore[union-attr]
        if not args.direct_dolphin:
            for p in players:
                assert p.launcher
                main_log = p.launcher.profile / "logs" / "main.log"
                text = main_log.read_text(errors="replace") if main_log.exists() else ""
                launch = [l for l in text.splitlines() if "Launching dolphin" in l]
                assert launch, f"{p.name}: no 'Launching dolphin' in {main_log}"
                assert "WiiSDCardPath=" in launch[-1] and "pponline-sd" in launch[-1], launch[-1]
                assert f"--harness-port {p.harness_port}" in launch[-1], launch[-1]
                assert re.search(r"Online plugin \w+ on .*: (copied|replaced|repaired|unchanged)",
                                 text), f"{p.name}: the launcher did not install the plugin"
        summary["ok"] = True
        log("end to end: OK")
    except BaseException as e:
        summary["error"] = f"{type(e).__name__}: {e}"
        summary["traceback"] = traceback.format_exc()
        log(f"FAILED: {summary['error']}")
        for p in players:
            if p.c:
                # A stalled game (the harness stops answering, or frames stop): what the CPU, the
                # scene and the plugin were doing. Each step on its own, so one timeout doesn't
                # hide the rest.
                for key, fn in (("status", lambda: p.c.status().raw),
                                ("cpu_state", lambda: p.c.call("cpu_state", timeout=10)),
                                ("scene", lambda: str(drive.scene(p.c))),
                                ("plugin_debug_scratch", p.debug_scratch),
                                ("mm_status", p.c.mm_status),
                                ("bridge", p.bridge)):
                    try:
                        p.facts[key] = fn()
                    except Exception as diag_e:  # noqa: BLE001
                        p.facts[key] = f"unavailable: {diag_e}"
                try:
                    p.shot("99-failure")
                except Exception:  # noqa: BLE001
                    pass
            if p.launcher and p.launcher.s:
                try:
                    p.launcher.shot("99-failure")
                    p.facts["launcher_text"] = p.launcher.body()[:2000]
                except Exception:  # noqa: BLE001
                    pass
    finally:
        for p in players:
            if p.launcher:
                # What the launcher itself logged on Play: the SD install and Dolphin's command line.
                main_log = p.launcher.profile / "logs" / "main.log"
                if main_log.exists():
                    p.facts["launcher_log"] = [
                        l for l in main_log.read_text(errors="replace").splitlines()
                        if "Online plugin" in l or "Launching dolphin" in l][-4:]
        for p in players:
            summary.setdefault("players", {})[p.name] = {
                "facts": p.facts, "game_screenshots": p.shots,
                "launcher_screenshots": p.launcher.shots if p.launcher else []}
        try:
            summary["mm_log_matched"] = [re.sub(r"\x1b\[[0-9;]*m", "", l)
                                         for l in be.log_text("mm").splitlines()
                                         if "matched" in l][-3:]
        except Exception:  # noqa: BLE001
            summary["mm_log_matched"] = []
        teardown(players, be)
        # Dolphin must not write next to whoever started it (P+ did: ./ReplayData on Windows).
        stray = [str(d) for d in (LAUNCHER / "ReplayData", ROOT / "ReplayData") if d.exists()]
        if stray:
            summary["ok"] = False
            summary.setdefault("error", f"files written outside the User folder: {stray}")
        (out_root / "summary.json").write_text(json.dumps(summary, indent=2, default=str))
        log(f"summary: {out_root / 'summary.json'}")
        cleanup_run_dir(run_dir, keep=args.keep or not summary["ok"])
    return summary


def teardown(players: list[Player], be: OnlineBackend) -> None:
    for p in players:
        if p.c:
            try:
                p.c.mm_cancel()
            except Exception:  # noqa: BLE001
                pass
            try:
                p.c.quit()
            except Exception:  # noqa: BLE001
                pass
            p.c = None
    time.sleep(2)
    for p in players:
        if p.dolphin and p.dolphin.poll() is None:
            _platform.kill_process_tree(p.dolphin)
        if p.launcher:
            # Dolphin is the launcher's child: killing the tree takes it too.
            if p.dolphin_pid and _platform.pid_alive(p.dolphin_pid):
                subprocess.run(["taskkill", "/PID", str(p.dolphin_pid), "/T", "/F"],
                               capture_output=True) if _platform.IS_WINDOWS else None
            p.launcher.stop()
    try:
        be.stop()
    except Exception as e:  # noqa: BLE001
        log(f"backend stop: {e}")


def cleanup_run_dir(run_dir: Path, keep: bool) -> None:
    if not keep:
        shutil.rmtree(run_dir, ignore_errors=True)
        log(f"removed {run_dir}")
        return
    # Kept for debugging: never keep the 2 GB SD card images.
    for sd in run_dir.rglob("sd.raw"):
        try:
            sd.unlink()
        except OSError as e:
            log(f"could not delete {sd}: {e}")
    for d in run_dir.rglob("pgdata"):
        shutil.rmtree(d, ignore_errors=True)
    log(f"kept {run_dir} (sd.raw removed)")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--prefix", default="e2el", help="run name prefix (run/e2e/<prefix>-<time>)")
    ap.add_argument("--dolphin", help="Dolphin.exe (default: the Qt build in dolphin/build)")
    ap.add_argument("--iso", help="Brawl NTSC-U disc image")
    ap.add_argument("--keep", action="store_true", help="keep the run dir (sd.raw removed)")
    ap.add_argument("--direct-dolphin", action="store_true",
                    help="start Dolphin.exe directly instead of through the launchers")
    ap.add_argument("--allow-stale-launcher", action="store_true",
                    help="drive the launcher bundle even if it is older than launcher/'s sources")
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    summary = run(args)
    print(json.dumps({k: summary[k] for k in ("run", "ok", "error") if k in summary}, indent=2))
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
