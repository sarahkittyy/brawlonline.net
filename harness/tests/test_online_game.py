"""The game's own online menus driving Dolphin's online client, end to end.

Each instance boots P+ with our plugin (``game-code/PPOnline/PPOnline.rel``) on its own copy of
the SD card, logs in from its own ``user.json`` and is driven with controller input only:
main menu -> PLAY ONLINE -> WITH FRIENDS (Direct) -> a character -> START on the character select
-> the connect code typed on Brawl's keypad -> START. The plugin posts PPOM mailbox requests; Dolphin's GameBridge
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
import os
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
sys.path.insert(0, str(ROOT / "harness" / "tools"))

import drive  # noqa: E402
import online_set  # noqa: E402
import patch_sd  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

# The plugin put on each instance's SD card. PPHARNESS_PLUGIN picks another build (e.g. one frozen
# at the game-code commit the Dolphin under test speaks PPOM with).
PLUGIN = Path(os.environ.get("PPHARNESS_PLUGIN") or ROOT / "game-code" / "PPOnline" / "PPOnline.rel")
ARTIFACTS = ROOT / "run" / "artifacts" / "game-bridge"
GAME_CODE_ARTIFACTS = ROOT / "run" / "artifacts" / "game-code"

# PPOM layout (game-code/PPOnline/include/ppom.h)
MB_RESP = 0x210
CMD_GET_MATCH_STATE = 0xB3
CMD_FETCH_CODE_SUGGESTION = 0xBE
# DEBUG scratch words the plugin keeps for tests (tools/gamecode/ppom.py read_debug)
SCR_SUGGEST_REQUESTS, SCR_CSS_LOCK, SCR_SUGGESTION = 0, 1, 2

# The CSS's player area (BrawlHeaders mu_selchar_player_area.h / mu_selchar_hand.h)
AREA_HAND, AREA_CHAR, AREA_COSTUME = 0x1A8, 0x1B8, 0x1BC
HAND_COIN, HAND_MODE = 0xA0, 0xA4


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
        self.name = user.display_name

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
        elif cmd == CMD_FETCH_CODE_SUGGESTION:
            r.update(found=bool(p[0]), index=struct.unpack_from(">I", p, 4)[0],
                     code=_u16text(p[8:8 + 18]))
        return r

    def css(self) -> dict[str, int]:
        """The local player's coin and character on the CSS (muSelCharPlayerArea, port 0)."""
        u = self.c.read_u32
        task = u(u(u(0x805A0060) + 4) + 0x400)
        area = u(task + 0x44)
        hand = u(area + AREA_HAND)
        return {"char": u(area + AREA_CHAR), "costume": u(area + AREA_COSTUME),
                "hand_coin": u(hand + HAND_COIN), "hand_mode": u(hand + HAND_MODE)}

    def locked(self) -> bool:
        return bool(self.debug_scratch()[SCR_CSS_LOCK] & 1)

    def suggestions(self) -> int:
        return int(self.bridge()["by_cmd"].get("FETCH_CODE_SUGGESTION", 0))

    def mark(self) -> int:
        return self.inst.log.mark()

    def suggestion_logged(self, inp: str, scroll: int, code: str | None, index: int,
                          since: int = 0, timeout: float = 10) -> None:
        """Dolphin answered a FETCH_CODE_SUGGESTION for `inp` with `code` (None: no suggestion)."""
        ans = (f"suggestion '{re.escape(code)}' index={index}" if code
               else f"no suggestion index={index}")
        self.inst.wait_for_log(r"GameBridge: request \d+ FETCH_CODE_SUGGESTION mode=2 scroll="
                               + str(scroll) + " input='" + re.escape(inp) + r"' index=\d+ -> "
                               + ans, timeout=timeout, since=since)

    def recent_codes(self, **kw: Any) -> dict[str, Any]:
        return self.c.call("online_recent_codes", mode="direct", **kw)

    def debug_scratch(self) -> list[int]:
        st = self.bridge()
        block = int(st["block"])
        debug_off = struct.unpack(">H", self.c.read_mem(block + 0x14, 2))[0]
        return list(struct.unpack(">16I", self.c.read_mem(block + debug_off + 0x18, 64)))

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
        """Main menu -> PLAY ONLINE -> the ONLINE page with WITH FRIENDS (Direct) highlighted.
        The plugin skips Brawl's connect dialog and first-time profile name, as Slippi goes
        straight to its online menu (same steps as tools/gamecode/scenarios/to_online.txt)."""
        self.steps("tap B", "wait 60", "tap DDOWN 4", "wait 40")
        self.shot("01-main-play-online")
        self.steps("tap A", "wait 90")
        self.shot("02-online-page")

    def to_css(self, mode: str) -> None:
        """WITH FRIENDS = Direct; WITH ANYONE -> BASIC VERSUS = Unranked. Then pick a character
        (the hand starts on the P1 panel; up to the second row, Fox): as on Slippi, START does
        nothing on the online CSS until a character is selected."""
        if mode == "unranked":
            self.steps("tap DRIGHT 4", "wait 30", "tap A", "wait 90")
        self.steps("tap A", "wait 400", "until scSelctCharacter 600")
        self.steps("stick up 30", "wait 5", "tap A 8", "wait 30")
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
        # START confirms (Slippi); the plugin then posts FIND_OPPONENT.
        if settle:
            n = self.finds()
            self.press_until("START", lambda: self.finds() > n, "FIND_OPPONENT")
        else:
            # Press without waiting for frames: with the netplay backend this search matches at
            # once and the game is stopped for the netplay boot.
            self.c.pad_script(0, [PadInput(buttons=["START"], hold=8), PadInput(hold=6)])


# Brawl's name keypad (MuSelctChrNameEntry) in the plugin's connect-code mode, as laid out on
# screen (docs/game-code.md section 7): row 0 is the text field and backspace, rows 1-4 are 3
# columns of multi-tap keys, row 5 is OK. Upper case only; the symbols key (1, 0) types '#'. The
# cursor does not wrap; UP from row 1 goes to backspace, DOWN from there into row 1.
ALPHA_KEYS = {(1, 0): "#", (1, 1): "ABC", (1, 2): "DEF", (2, 0): "GHI", (2, 1): "JKL",
              (2, 2): "MNO", (3, 0): "PQRS", (3, 1): "TUV", (3, 2): "WXYZ"}
DIGIT_KEYS = {(1, 0): "1", (1, 1): "2", (1, 2): "3", (2, 0): "4", (2, 1): "5", (2, 2): "6",
              (3, 0): "7", (3, 1): "8", (3, 2): "9", (4, 1): "0"}
PAGE_KEY = (4, 2)
PAGES_TO_DIGITS = 1  # connect-code mode has two pages: alphabet <-> digits


def keypad_steps(code: str) -> list[str]:
    """drive.py steps that type `code` ("ABCD#123": letters and '#', then digits) on a freshly
    opened keypad, wherever its cursor starts."""
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

    letters = re.match(r"[A-Z#]*", code).group(0)
    digits = code[len(letters):]
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
    assert s.count("tap A 8") == 1 + 2 + 1 + PAGES_TO_DIGITS + 2
    assert keypad_steps("WXYZ#990")[-2:] == ["tap A 8", "wait 10"]


def _boot(dolphin: Callable[..., DolphinInstance], name: str, be: OnlineBackend, user: OnlineUser,
          video: str, test: str, session_backend: str, artifacts: Path = ARTIFACTS) -> Game:
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
    out = artifacts / test / name
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
    # ("Press START to enter code": START opens the keypad again, as on Slippi).
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
    a.open_keypad()
    a.type_code(ub.connect_code)

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


# Recent codes, oldest first (Dolphin keeps them newest first: ABCD#999, ADGJ#123, CARL#123,
# BOB#610), as `online_recent_codes add` writes them to <User>/Online/<uid>/direct-codes.json.
RECENT = ["BOB#610", "CARL#123", "ADGJ#123", "ABCD#999"]
SCROLL_NONE, SCROLL_OLDER, SCROLL_NEWER, SCROLL_RESET = 0, 1, 2, 3


def _seeded_direct_css(dolphin: Callable[..., DolphinInstance], be: OnlineBackend,
                       gpu_backend: str, test: str, name: str, user: OnlineUser) -> Game:
    """One logged-in game on the Direct CSS with Fox picked and a seeded code history."""
    g = _boot(dolphin, name, be, user, gpu_backend, test, "record", GAME_CODE_ARTIFACTS)
    g.to_main_menu()
    r = g.recent_codes(clear=True, add=RECENT)
    assert r["codes"] == list(reversed(RECENT)), r
    g.to_online_page()
    g.to_css("direct")
    return g


def test_recent_codes_on_the_keypad(backend: OnlineBackend,
                                    dolphin: Callable[..., DolphinInstance],
                                    gpu_backend: str) -> None:
    """Slippi's recent connect codes on Brawl's keypad (0xBE FETCH_CODE_SUGGESTION): the newest
    code is suggested when the keypad opens, typing narrows it, L/R scroll older/newer, Z takes
    the suggestion and moves to OK, START searches with it; a typed prefix alone is sent as
    typed (the suggestion is only shown). Screenshots: run/artifacts/game-code/recent-codes/."""
    u = backend.create_user("gina", "GINA")
    g = _seeded_direct_css(dolphin, backend, gpu_backend, "recent-codes", "game-r", u)

    # Opened: the newest code (Slippi NameEntryThinkOneShot.asm).
    m = g.mark()
    g.open_keypad()
    g.suggestion_logged("", SCROLL_RESET, "ABCD#999", 0, since=m)
    g.steps("wait 20")
    g.shot("01-keypad-suggestion")
    r = g.response()
    assert r["cmd"] == CMD_FETCH_CODE_SUGGESTION and r["found"] and r["code"] == "ABCD#999", r

    # Typing narrows it (OnEnterText.asm: reset with the new text).
    m = g.mark()
    g.steps(*keypad_steps("AD"))
    g.suggestion_logged("A", SCROLL_RESET, "ABCD#999", 0, since=m)
    g.suggestion_logged("AD", SCROLL_RESET, "ADGJ#123", 1, since=m)
    g.steps("wait 20")
    g.shot("02-typed-AD")

    # B deletes (and asks again); on the empty field the newest code is back.
    m = g.mark()
    g.steps("tap B 8", "wait 20", "tap B 8", "wait 30")
    g.suggestion_logged("A", SCROLL_RESET, "ABCD#999", 0, since=m)
    g.suggestion_logged("", SCROLL_RESET, "ABCD#999", 0, since=m)
    assert g.debug_scratch()[4] == 1, "B on a non-empty field must not leave the keypad"

    # L = older, R = newer (OnLPress/OnRPress.asm).
    for button, scroll, code, index, shot in (("L", SCROLL_OLDER, "ADGJ#123", 1, "03-L-older"),
                                              ("L", SCROLL_OLDER, "CARL#123", 2, "04-L-older"),
                                              ("R", SCROLL_NEWER, "ADGJ#123", 1, "05-R-newer")):
        m = g.mark()
        n = g.suggestions()
        g.press_until(button, lambda n=n: g.suggestions() > n, f"FETCH_CODE_SUGGESTION ({button})",
                      tries=3)
        g.suggestion_logged("", scroll, code, index, since=m)
        g.steps("wait 20")
        g.shot(shot)

    # Z takes it (CheckTriggersAndZ.asm): success sound, highlight on OK.
    accepted = g.debug_scratch()[SCR_SUGGESTION] >> 24
    g.press_until("Z", lambda: g.debug_scratch()[SCR_SUGGESTION] >> 24 > accepted, "Z to accept",
                  tries=3)
    g.steps("wait 20")
    g.shot("06-z-accepted")

    # START searches with the whole code, which becomes the newest recent code.
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _searching(g, "ADGJ#123")
    g.steps("wait 40")
    g.shot("07-searching-accepted")
    assert g.recent_codes()["codes"][0] == "ADGJ#123"
    g.press_until("Z", lambda: g.c.mm_status()["state"] == "idle", "the cleanup")

    # A typed prefix with a suggestion shown is sent as typed (OnConfirmButtonHandler.asm).
    m = g.mark()
    g.open_keypad()
    g.suggestion_logged("", SCROLL_RESET, "ADGJ#123", 0, since=m)
    g.steps(*keypad_steps("CA"))
    g.suggestion_logged("CA", SCROLL_RESET, "CARL#123", 2, since=m)
    g.steps("wait 20")
    g.shot("08-typed-CA")
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _searching(g, "CA")
    g.press_until("Z", lambda: g.c.mm_status()["state"] == "idle", "the cleanup")

    # No history: no suggestion, and Z only buzzes.
    g.recent_codes(clear=True)
    m = g.mark()
    g.open_keypad()
    g.suggestion_logged("", SCROLL_RESET, None, 0, since=m)
    accepted = g.debug_scratch()[SCR_SUGGESTION] >> 24
    g.steps("tap Z 8", "wait 20")
    assert g.debug_scratch()[SCR_SUGGESTION] >> 24 == accepted
    g.shot("09-no-history")
    g.c.mm_cancel()


def test_character_locked_while_searching(backend: OnlineBackend,
                                          dolphin: Callable[..., DolphinInstance],
                                          gpu_backend: str) -> None:
    """While locked in (searching, connected, or an error not yet cleared) A and B on the
    character and the costume buttons do nothing, as on Slippi (PreventAPressCharUnselect.asm,
    PreventBPressCharUnselect.asm, PreventColorChange.asm); Z unlocks; holding B still leaves.
    Screenshots: run/artifacts/game-code/css-lock/."""
    u = backend.create_user("hank", "HANK")
    g = _seeded_direct_css(dolphin, backend, gpu_backend, "css-lock", "game-l", u)
    before = g.css()
    assert before["char"] == 0x7 and before["hand_coin"] == 0, before   # Fox, coin placed

    g.open_keypad()
    g.press_until("Z", lambda: g.debug_scratch()[SCR_SUGGESTION] >> 24 > 0, "Z to accept", tries=3)
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _searching(g, "ABCD#999")
    _wait(g.locked, 5, "the CSS lock")
    g.shot("01-searching-locked")

    # The hand is over the placed coin: A would pick it up, B would take it back, X/Y would
    # change the costume. None of it happens.
    g.steps("tap A 8", "wait 20", "tap B 8", "wait 20", "tap X 8", "wait 20", "tap Y 8", "wait 20")
    assert g.css() == before, g.css()
    assert g.c.mm_status()["state"] in ("initializing", "matchmaking")
    g.shot("02-after-A-B-X-Y")

    # Z cancels: unlocked, B takes the coin back to the hand, A puts it down again.
    g.press_until("Z", lambda: g.c.mm_status()["state"] == "idle", "the cleanup")
    _wait(lambda: not g.locked(), 5, "the CSS to unlock")
    g.press_until("B", lambda: g.css()["hand_coin"] != 0, "B to take the coin back")
    g.shot("03-unlocked-B-coin-in-hand")
    g.press_until("A", lambda: g.css()["hand_coin"] == 0, "A to place the coin")
    assert g.css()["char"] == 0x7
    g.shot("04-unlocked-A-coin-placed")
    g.press_until("X", lambda: g.css()["costume"] != before["costume"], "X to change costume")

    # Locked again; holding B still leaves the CSS (Slippi keeps Melee's hold B).
    g.open_keypad()
    g.press_until("Z", lambda: g.debug_scratch()[SCR_SUGGESTION] >> 24 > 1, "Z to accept", tries=3)
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _wait(g.locked, 10, "the CSS lock")
    g.steps("hold B 90", "until muMenuMain 600", "wait 30")
    g.shot("05-hold-B-left")
    _wait(lambda: g.c.mm_status()["state"] == "idle", 10, "the cleanup on the menu")
    assert not g.locked()


def _connected_direct(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                      gpu_backend: str, test: str, names: tuple[str, str],
                      stocks: int = 2) -> tuple[Game, Game, OnlineUser, OnlineUser]:
    """Two games on the Direct CSS with the gameplay backend (the default), searching for each
    other from the keypad until both are connected. Shorter rules than P+'s (`stocks` stocks,
    2 minutes, written into the set rule on both CSSs where the online rules are) keep the games
    short; they are part of the setup both games build, so both must have the same."""
    ua = backend.create_user(names[0], names[0][:4].upper())
    ub = backend.create_user(names[1], names[1][:4].upper())
    a = _boot(dolphin, "game-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "game-b", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("direct"), lambda: b.to_css("direct"))
    for g in (a, b):
        B.write_rules(g.c, stocks=stocks, minutes=2, items_off=True)
    a.open_keypad()
    a.type_code(ub.connect_code)
    _searching(a, ub.connect_code)
    time.sleep(2.2)  # the server takes one ticket per account per 2 s
    b.open_keypad()
    b.type_code(ua.connect_code)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    for g in (a, b):
        st = _wait(lambda g=g: (lambda s: s if s["handoff"] == "started" else None)(g.c.mm_status()),
                   30, "the hand-off")
        assert st["session"]["backend"] == "gameplay", st["session"]
    return a, b, ua, ub


LEGAL_STAGES = (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0C, 0x0D, 0x1C, 0x1F, 0x21, 0x23,
                0x2D, 0x2E)


@pytest.mark.slow
def test_direct_set_under_the_gameplay_session(backend: OnlineBackend,
                                               dolphin: Callable[..., DolphinInstance],
                                               gpu_backend: str) -> None:
    """The real online path (backend-design 5.1 C): two players connect from the online CSS and
    play a two-game Direct set without any reboot. Game 1 starts by itself (both locked in by
    their search) on a random stage from P+'s legal list; both games build the same match from
    SESSION and start it in step at the first simulation frame (RNG, frame counters and setup
    synced); the confirmed frames of both peers agree and both reach game set on the same frame;
    both go straight back to the online CSS, still connected. Game 2: the loser picks the stage
    on P+'s stage select, the winner locks in with START, and that stage is played.
    Screenshots: run/artifacts/game-bridge/gameplay-set/."""
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "gameplay-set", ("iris", "jack"))
    for g, other in ((a, ub), (b, ua)):
        blk = ppom.find_block(g.c)
        se = ppom.read_session(g.c, blk)
        assert se["num_players"] == 2 and se["mode"] == 2, se
        assert {p["name"] for p in se["players"]} == {ua.display_name, ub.display_name}, se
        lo = ppom.read_local(g.c, blk)
        assert lo["peer_name"] == other.display_name, lo
        assert lo["lock"]["ready"] and lo["lock"]["game"] == 1, lo
        # Both games keep running their own scenes: no netplay session, no boot.
        assert not g.c.netplay_status().game_running
    rep = online_set.play_set([a, b], games=2, mode="direct")
    stages = [g["stage"] for g in rep["games"]]
    assert stages[0] in LEGAL_STAGES, stages
    assert stages[1] == B.STAGE_KIND[online_set.STAGE_PICK], stages
    for g in (a, b):
        assert g.bridge()["lost"] == 0
        # No reboot: the GameBridge found the plugin's block once and never lost it.
        assert "is gone" not in (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    for g in (a, b):
        g.c.mm_cancel()


@pytest.mark.slow
def test_opponent_leaves_in_the_middle_of_a_game(backend: OnlineBackend,
                                                  dolphin: Callable[..., DolphinInstance],
                                                  gpu_backend: str) -> None:
    """Slippi's disconnect flow (backend-design 5.6) in a match: the opponent closes Dolphin; after
    the silence limit (7.2 s at delay 2) the remaining game plays the error sound, ends the game
    and goes straight back to its online CSS, where the next GET_MATCH_STATE reads IDLE: the
    idle prompt, no error."""
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "gameplay-leave", ("kate", "liam"),
                                     stocks=4)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    a.steps("wait 300")
    t0 = time.monotonic()
    b.inst.kill()
    _wait(lambda: online_set.gstatus(a.c)["disconnected"], 15, "the drop", interval=0.05)
    dropped = time.monotonic() - t0
    assert 6.5 < dropped < 9.5, dropped
    a.shot("01-dropped-in-match")
    _wait(lambda: online_set.scene(a.c) == online_set.CSS, 60, "back on the CSS")
    _wait(lambda: a.c.mm_status()["state"] == "idle", 30, "idle")
    a.steps("wait 60")
    a.shot("02-back-on-css-idle")
    assert a.debug_scratch()[10] & 0x30000 == 0x30000   # the error sound, the game ended
    assert not a.locked()
    lo = ppom.read_local(a.c, ppom.find_block(a.c))
    assert lo["disconnected"] == 1 and lo["state"] == 0, lo


@pytest.mark.slow
def test_direct_from_the_game_menus_hands_off_to_netplay(
        backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    """The same Direct flow with the fallback session backend (whole-machine netplay, selected
    with [Online] SessionBackend = netplay): after the match both Dolphins stop their games and
    boot P+ together under rollback. The mailbox is not serviced while netplay runs."""
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
