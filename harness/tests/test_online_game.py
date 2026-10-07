"""The game's own online menus driving Dolphin's online client, end to end.

Each instance boots P+ with our plugin (``game-code/PPOnline/PPOnline.rel``) on its own copy of
the SD card, logs in from its own ``user.json`` and is driven with controller input only:
main menu -> PLAY ONLINE -> WITH FRIENDS (Direct) -> START on the character select -> the connect
code typed on Brawl's keypad -> OK. The plugin posts PPOM mailbox requests; Dolphin's GameBridge
(``Source/Core/Core/Online/GameBridge.cpp``) services them at the frame boundary and drives the
matchmaking client against our ``mm`` server. Nothing plays Dolphin's part from the outside.

Checked through the mailbox in game memory (what the game was told), Dolphin's ``mm_status`` and
``game_bridge_status``, the mm server's log, and screenshots of the CSS header under
``run/artifacts/game-bridge/<test>/``.

Needs the real Dolphin build, a working video backend for screenshots, the built plugin
(``game-code/build.sh``) and the server binaries plus Postgres (``ppharness/backend.py``).
"""

from __future__ import annotations

import concurrent.futures
import re
import shutil
import struct
import sys
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import paths
from ppharness.backend import OnlineBackend, OnlineUser
from ppharness.client import HarnessClient, PadInput
from ppharness.instance import DolphinInstance, InstanceConfig

from test_online import _logged_in, _make_backend, _wait

ROOT = paths.workspace_root()
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))

import drive  # noqa: E402
import patch_sd  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

PLUGIN = ROOT / "game-code" / "PPOnline" / "PPOnline.rel"
ARTIFACTS = ROOT / "run" / "artifacts" / "game-bridge"

UNRANKED_ERROR = "Unranked is not supported yet. Only Direct works for now."

# PPOM layout (game-code/PPOnline/include/ppom.h)
MB_RESP = 0x210
CMD_GET_MATCH_STATE = 0xB3


@pytest.fixture(scope="module")
def backend() -> Any:
    be = _make_backend()
    try:
        yield be
    finally:
        be.stop()


def _u16text(b: bytes) -> str:
    out = []
    for i in range(0, len(b), 2):
        c = struct.unpack_from(">H", b, i)[0]
        if c == 0:
            break
        out.append(chr(c))
    return "".join(out)


# --------------------------------------------------------------------------- one game instance


class Game:
    """One booted P+ with the plugin, its harness client and its view of the mailbox."""

    def __init__(self, inst: DolphinInstance, user: OnlineUser, out: Path):
        self.inst = inst
        self.user = user
        self.out = out
        self.c: HarnessClient = inst.client

    # -- mailbox in game memory
    def bridge(self) -> dict[str, Any]:
        return self.c.game_bridge_status()

    def mailbox(self) -> int:
        st = self.bridge()
        assert st["found"], st
        return int(st["mailbox"])

    def response(self) -> dict[str, Any]:
        """The last response Dolphin wrote into the game's mailbox, decoded."""
        mb = self.mailbox()
        raw = self.c.read_mem(mb, MB_RESP + 0x200)
        resp_count, resp_seen = struct.unpack_from(">II", raw, 0x8)
        seq, cmd, status = struct.unpack_from(">IBB", raw, MB_RESP)
        p = raw[MB_RESP + 8:]
        r: dict[str, Any] = {"seq": seq, "cmd": cmd, "status": status,
                             "resp_count": resp_count, "resp_seen": resp_seen}
        if cmd == CMD_GET_MATCH_STATE:
            r.update(mm_state=p[0], role=p[1], session_phase=p[2],
                     peer_name=_u16text(p[4:0x24]), peer_code=_u16text(p[0x24:0x36]),
                     error=_u16text(p[0x38:0x38 + 240]))
        return r

    def debug_scratch(self) -> list[int]:
        st = self.bridge()
        block = int(st["block"])
        debug_off = struct.unpack(">H", self.c.read_mem(block + 0x14, 2))[0]
        return list(struct.unpack(">10I", self.c.read_mem(block + debug_off + 0x18, 40)))

    # -- driving
    def steps(self, *steps: str) -> None:
        drive.run(self.c, list(steps), out=str(self.out))

    def shot(self, name: str) -> Path:
        p = self.out / f"{name}.png"
        p.parent.mkdir(parents=True, exist_ok=True)
        self.c.screenshot(str(p.resolve()))
        return p

    def press_until(self, button: str, done: Callable[[], Any], what: str,
                    tries: int = 4, timeout: float = 3.0) -> Any:
        """Press `button` (held 8 frames) until `done()` is true. The plugin sees buttons as held
        state once per game frame (gfPadSystem::updateSystem), while the harness counts pad reads;
        when the game drops frames under load, a short press can fall between two of its frames
        (Brawl's own menus latch presses, so they never miss one)."""
        for _ in range(tries):
            self.steps(f"tap {button} 8")
            try:
                return _wait(done, timeout, what)
            except AssertionError:
                continue
        raise AssertionError(f"{button} x{tries}: still waiting for {what}")

    def finds(self) -> int:
        return int(self.bridge()["by_cmd"].get("FIND_OPPONENT", 0))

    def scene(self) -> str:
        return drive.scene(self.c).name

    def to_main_menu(self) -> None:
        # P+ boots to the Versus character select; hold B back to the main menu.
        self.steps("until scSelctCharacter 3000", "wait 60", "hold B 60", "until muMenuMain 600",
                   "wait 60")

    def to_online_page(self) -> None:
        """Main menu -> PLAY ONLINE -> Brawl's connect dialog (WFC faked) -> first-time profile
        name -> the ONLINE page with WITH FRIENDS (Direct) highlighted.

        This is the flow of the plugin on game-code `pponline` (tools/gamecode/scenarios/
        to_online.txt at the same time). The menu work that skips the connect dialog and the
        profile name changes it: update these steps together with that scenario."""
        self.steps("tap B", "wait 60", "tap DDOWN 4", "wait 40")
        self.shot("01-main-play-online")
        self.steps("tap A", "wait 90", "tap DLEFT 4", "wait 20", "tap A", "wait 300",
                   "tap A", "wait 150",
                   # profile name: one letter, then OK
                   "tap DRIGHT 4", "wait 10", "tap A", "wait 20", "tap START", "wait 20", "tap A",
                   "wait 250")
        self.shot("02-online-page")

    def to_css(self, mode: str) -> None:
        if mode == "unranked":
            self.steps("tap DRIGHT 4", "wait 30")
        self.steps("tap A", "wait 400", "until scSelctCharacter 600")
        self.shot(f"03-css-{mode}")

    def open_keypad(self) -> None:
        """START on the Direct CSS opens Brawl's keypad. The hand sometimes refuses right after
        the CSS appears, so press again until the plugin reports the keypad open (debug
        scratch[4] = 1 opened, scratch[5] = the hand's mode, 8 = keypad)."""
        for _ in range(6):
            self.steps("tap START 8", "wait 40")
            s = self.debug_scratch()
            if s[4] == 1 and s[5] == 8:
                return
            self.steps("wait 60")
        raise AssertionError(f"the code keypad did not open (scratch {self.debug_scratch()})")

    def type_code(self, code: str, settle: bool = True) -> None:
        self.steps(*keypad_steps(code))
        self.shot("04-code-typed")
        # START moves the cursor to OK, A presses it; the plugin then posts FIND_OPPONENT.
        self.steps("tap START 8", "wait 10")
        if settle:
            n = self.finds()
            self.press_until("A", lambda: self.finds() > n, "FIND_OPPONENT")
        else:
            # Press without waiting for frames: with the netplay backend this search matches at
            # once and the game is stopped for the netplay boot.
            self.c.pad_script(0, [PadInput(buttons=["A"], hold=8), PadInput(hold=6)])


# Brawl's name keypad (MuSelctChrNameEntry), as laid out on screen (docs/game-code.md section 7):
# row 0 is the text field and backspace, rows 1-4 are 3 columns of multi-tap keys, row 5 is OK.
# The cursor does not wrap; UP from row 1 goes to backspace, DOWN from there into row 1.
ALPHA_KEYS = {(1, 1): "ABC", (1, 2): "DEF", (2, 0): "GHI", (2, 1): "JKL", (2, 2): "MNO",
              (3, 0): "PQRS", (3, 1): "TUV", (3, 2): "WXYZ"}
DIGIT_KEYS = {(1, 0): "1", (1, 1): "2", (1, 2): "3", (2, 0): "4", (2, 1): "5", (2, 2): "6",
              (3, 0): "7", (3, 1): "8", (3, 2): "9", (4, 1): "0"}
PAGE_KEY = (4, 2)
PAGES_TO_DIGITS = 4  # alphabet -> accented -> hiragana -> katakana -> digits


def keypad_steps(code: str) -> list[str]:
    """drive.py steps that type `code` ("ABCD#123": letters, then digits; the plugin inserts the
    '#') on a freshly opened keypad, wherever its cursor starts."""
    steps: list[str] = []

    def move(d: str) -> None:
        steps.extend([f"tap {d} 8", "wait 6"])

    # Anchor: up to backspace, down into row 1, left to column 0.
    for _ in range(6):
        move("DUP")
    move("DDOWN")
    move("DLEFT")
    move("DLEFT")
    pos = (1, 0)
    last: tuple[int, int] | None = None

    def goto(target: tuple[int, int]) -> None:
        nonlocal pos
        r, c = pos
        while r < target[0]:
            move("DDOWN"); r += 1
        while r > target[0]:
            move("DUP"); r -= 1
        while c < target[1]:
            move("DRIGHT"); c += 1
        while c > target[1]:
            move("DLEFT"); c -= 1
        pos = (r, c)

    def press(times: int) -> None:
        for _ in range(times):
            steps.extend(["tap A 8", "wait 10"])

    letters = re.match(r"[A-Z]*", code.replace("#", "")).group(0)
    digits = code.replace("#", "")[len(letters):]
    assert digits.isdigit() or not digits, code
    for ch in letters:
        key = next(k for k, v in ALPHA_KEYS.items() if ch in v)
        if key == last:
            # Multi-tap: the same key again would cycle the letter. Moving off and back
            # commits it (there is no timeout).
            move("DRIGHT" if pos[1] < 2 else "DLEFT")
            move("DLEFT" if pos[1] < 2 else "DRIGHT")
        goto(key)
        press(ALPHA_KEYS[key].index(ch) + 1)
        last = key
    if digits:
        goto(PAGE_KEY)
        for _ in range(PAGES_TO_DIGITS):
            steps.extend(["tap A 8", "wait 12"])
        last = None
        for ch in digits:
            key = next(k for k, v in DIGIT_KEYS.items() if v == ch)
            if key == last:
                move("DRIGHT" if pos[1] < 2 else "DLEFT")
                move("DLEFT" if pos[1] < 2 else "DRIGHT")
            goto(key)
            press(1)
            last = key
    return steps


def test_keypad_steps_cover_every_code_shape() -> None:
    """Pure check of the typing plan: repeated keys move off and back, digits switch pages."""
    s = keypad_steps("AB#11")
    assert s.count("tap A 8") == 1 + 2 + PAGES_TO_DIGITS + 2
    assert keypad_steps("WXYZ#990")[-2:] == ["tap A 8", "wait 10"]


def _boot(dolphin: Callable[..., DolphinInstance], name: str, be: OnlineBackend, user: OnlineUser,
          video: str, test: str, session_backend: str) -> Game:
    if not PLUGIN.exists():
        pytest.skip(f"{PLUGIN} not built (game-code/build.sh)")
    cfg = InstanceConfig(cpu_thread=True, video_backend=video, dolphin_ini={"Online": {
        "UseDevServer": True, "MatchmakingPort": be.mm_port, "DevAccountsUrl": be.accounts_url}})
    inst = dolphin(name, config=cfg, client_timeout=60.0)
    d = inst.create()
    patch_sd.patch_image(d / "Wii" / "sd.raw",
                         [(PLUGIN.read_bytes(), f"{patch_sd.PLUGIN_DIR}/{PLUGIN.name}")])
    user.write_user_json(d)
    inst.launch()
    inst.connect()
    out = ARTIFACTS / test / name
    shutil.rmtree(out, ignore_errors=True)
    g = Game(inst, user, out)
    g.c.online_session_backend(session_backend)
    _logged_in(inst, user)
    return g


def _both(fa: Callable[[], Any], fb: Callable[[], Any]) -> None:
    with concurrent.futures.ThreadPoolExecutor(2) as ex:
        for f in [ex.submit(fa), ex.submit(fb)]:
            f.result()


def _find_logged(g: Game, mode: int, code: str) -> None:
    """Dolphin read FIND_OPPONENT from the game's mailbox. (The request ring has 4 slots and the
    game polls GET_MATCH_STATE every frame, so the request itself is soon overwritten.)"""
    g.inst.wait_for_log(r"GameBridge: request \d+ FIND_OPPONENT mode=" + str(mode) + " code='"
                        + re.escape(code) + "'", timeout=10)


def _searching(g: Game, code: str) -> dict[str, Any]:
    """The game asked for `code` and was told it is searching."""
    _find_logged(g, 2, code)
    return _wait(lambda: (lambda r: r if r["cmd"] == CMD_GET_MATCH_STATE else None)(g.response()),
                 10, "the GET_MATCH_STATE answer")


def _peer_shown(g: Game, other: OnlineUser, timeout: float = 60) -> dict[str, Any]:
    return _wait(lambda: (lambda r: r if r["cmd"] == CMD_GET_MATCH_STATE and r["mm_state"] == 4
                          and r["peer_name"] == other.display_name else None)(g.response()),
                 timeout, f"the game to be told about {other.display_name}")


def test_direct_from_the_game_menus(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                    gpu_backend: str) -> None:
    """Two players find each other through the in-game Direct flow. The session backend is
    `record`: Online::Session::Start is called with the match and holds the P2P link, so the
    games stay on the CSS showing the opponent (the netplay reboot is the next test)."""
    test = "direct"
    ua, ub = backend.create_user("alice", "ALIC"), backend.create_user("bob", "BOB")
    a = _boot(dolphin, "game-a", backend, ua, gpu_backend, test, "record")
    b = _boot(dolphin, "game-b", backend, ub, gpu_backend, test, "record")

    _both(a.to_main_menu, b.to_main_menu)
    # The main menu asked for the account (GET_ONLINE_STATUS) and Dolphin answered from user.json.
    for g in (a, b):
        st = _wait(lambda g=g: (lambda s: s if s["by_cmd"].get("GET_ONLINE_STATUS") else None)(
            g.bridge()), 10, "GET_ONLINE_STATUS")
        assert st["last_response"]["text"] == (
            f"GET_ONLINE_STATUS state=1 name='{g.user.display_name}' code='{g.user.connect_code}'")
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("direct"), lambda: b.to_css("direct"))

    # A types B's code and searches alone for a while.
    a.open_keypad()
    a.type_code(ub.connect_code)
    r = _searching(a, ub.connect_code)
    assert r["mm_state"] in (1, 2), r
    a.steps("wait 60")
    a.shot("05-searching")
    assert a.c.mm_status()["state"] in ("initializing", "matchmaking")
    assert a.response()["mm_state"] in (1, 2)

    # Z cancels the search (CLEANUP_CONNECTION): Dolphin goes idle, the game back to its prompt
    # (the code stays, so START searches for it again).
    a.press_until("Z", lambda: a.c.mm_status()["state"] == "idle", "the cleanup")
    assert a.bridge()["by_cmd"].get("CLEANUP_CONNECTION", 0) >= 1
    a.shot("06-cancelled")

    # B types A's code and searches alone; then A searches again.
    b.open_keypad()
    b.type_code(ua.connect_code)
    r = _searching(b, ua.connect_code)
    assert r["mm_state"] in (1, 2), r
    b.steps("wait 60")
    b.shot("05-searching")
    assert b.c.mm_status()["state"] in ("initializing", "matchmaking")
    time.sleep(2.2)  # the server takes one ticket per account per 2 s
    n = a.finds()
    a.press_until("START", lambda: a.finds() > n, "the second FIND_OPPONENT")

    # The server pairs them, both connect, both games are told who it is.
    ra = _peer_shown(a, ub)
    rb = _peer_shown(b, ua)
    for g, other, r in ((a, ub, ra), (b, ua, rb)):
        assert r["peer_code"] == other.connect_code and r["error"] == ""
        assert r["role"] in (1, 2) and r["session_phase"] == 3, r
        g.steps("wait 20")
        g.shot("07-opponent")
    assert {ra["role"], rb["role"]} == {1, 2}

    # Dolphin's view: one match, handed to the session backend on both sides.
    sa, sb = a.c.mm_status(), b.c.mm_status()
    assert sa["state"] == sb["state"] == "connection_success"
    assert sa["match"]["match_id"] == sb["match"]["match_id"]
    match_id = sa["match"]["match_id"]
    for s, other in ((sa, ub), (sb, ua)):
        assert s["mode"] == "direct" and s["opponent_code"] == other.connect_code
        assert s["handoff"] == "started", s
        sess = s["session"]
        assert sess["backend"] == "record" and sess["phase"] == "held", sess
        assert sess["detail"]["starts"] == 1 and sess["detail"]["match_id"] == match_id
        assert sess["detail"]["peer_code"] == other.connect_code
        assert sess["detail"]["link_open"]
    # The mm server paired exactly these two.
    log = backend.log_text("mm")
    line = next((l for l in log.splitlines() if "matched" in l and match_id in l), None)
    assert line and ua.connect_code in line and ub.connect_code in line, log[-2000:]
    for g in (a, b):
        g.inst.wait_for_log(r"Online session \(record\): Start match " + re.escape(match_id),
                            timeout=5)
        assert g.bridge()["lost"] == 0

    for g in (a, b):
        g.c.mm_cancel()


def test_unranked_shows_the_server_error(backend: OnlineBackend,
                                         dolphin: Callable[..., DolphinInstance],
                                         gpu_backend: str) -> None:
    """UNRANKED -> START: the server refuses the ticket and the game shows its text."""
    test = "unranked"
    u = backend.create_user("carl", "CARL")
    g = _boot(dolphin, "game-u", backend, u, gpu_backend, test, "record")
    g.to_main_menu()
    g.to_online_page()
    g.to_css("unranked")
    g.press_until("START", lambda: g.finds() > 0, "FIND_OPPONENT")
    _find_logged(g, 1, "")
    r = _wait(lambda: (lambda r: r if r["cmd"] == CMD_GET_MATCH_STATE and r["mm_state"] == 5
                       else None)(g.response()), 30, "the error in the mailbox")
    assert r["error"] == UNRANKED_ERROR, r
    st = g.c.mm_status()
    assert st["state"] == "error" and st["error"] == UNRANKED_ERROR
    assert st["error_source"] == "create_ticket"
    g.steps("wait 30")
    g.shot("05-unranked-error")
    # Z clears the error (Slippi: "Press Z to clear error").
    g.press_until("Z", lambda: g.c.mm_status()["state"] == "idle", "the cleanup")
    g.shot("06-after-clear")


@pytest.mark.slow
def test_direct_from_the_game_menus_hands_off_to_netplay(
        backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    """The same Direct flow with the real session backend (whole-machine netplay): after the
    match both Dolphins stop their games and boot P+ together under rollback. The mailbox is
    not serviced while netplay runs."""
    test = "direct-netplay"
    ua, ub = backend.create_user("dora", "DORA"), backend.create_user("eve", "EVE")
    a = _boot(dolphin, "game-c", backend, ua, gpu_backend, test, "netplay")
    b = _boot(dolphin, "game-d", backend, ub, gpu_backend, test, "netplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("direct"), lambda: b.to_css("direct"))
    a.open_keypad()
    a.type_code(ub.connect_code)
    _searching(a, ub.connect_code)
    a.shot("05-searching")
    b.open_keypad()
    b.type_code(ua.connect_code, settle=False)

    # Both are told who they play before their games stop.
    for g, other in ((a, ub), (b, ua)):
        g.inst.wait_for_log(r"GameBridge: request \d+ GET_MATCH_STATE -> GET_MATCH_STATE "
                            r"mmState=[34] .*peer='" + re.escape(other.display_name) + "' '"
                            + re.escape(other.connect_code) + "'", timeout=60)
    sa = _wait(lambda: (lambda s: s if s["handoff"] == "started" else None)(a.c.mm_status()), 30,
               "the hand-off")
    sb = _wait(lambda: (lambda s: s if s["handoff"] == "started" else None)(b.c.mm_status()), 30,
               "the hand-off")
    assert sa["match"]["match_id"] == sb["match"]["match_id"]
    assert "matched" in backend.log_text("mm")

    def running(g: Game) -> bool:
        ns = g.c.netplay_status()
        return bool(ns.game_running and ns.rollback and ns.rollback.get("session_started"))

    _wait(lambda: running(a) and running(b), 120, "the game to boot under rollback on both")
    for g in (a, b):
        sess = g.c.mm_status()["session"]
        assert sess["backend"] == "netplay" and sess["phase"] == "running", sess
        # Booted again under netplay: the plugin is back, and the bridge leaves it alone.
        st = _wait(lambda g=g: (lambda s: s if s["found"] and s["netplay_paused"] else None)(
            g.bridge()), 60, "the bridge to find the block and pause")
        assert st["lost"] == 0
        g.inst.wait_for_log(r"Excluded the PPOM mailbox", timeout=5)
    time.sleep(2)
    for g in (a, b):
        g.shot("06-netplay-boot")
    # Leaving ends the session on both sides (the peer's game stops too), so only after the shots.
    for g in (a, b):
        g.c.mm_cancel()
