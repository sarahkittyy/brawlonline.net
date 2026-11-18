"""P+'s harness tests on a mod of P+ (docs/design/pplus-mods.md §5; harness/README.md, "Mods of P+").

The tests were written against P+ v3.2: they move the cursor to where P+'s icons are, read P+'s
tables at P+'s addresses and add files under /Project+ on the card. A mod (Cobalt, Project+ Ex)
has other screens and tables, and the game code reads them from the running game. With
`PPHARNESS_MOD_FOLDER` set (the mod's folder on the card: "Cobalt", "P+Ex"; from the template's
build.json, ppharness/paths.py, or by hand; with
`PPHARNESS_TEMPLATE`, a template user folder whose `Wii/sd.raw` is the mod's card, made by
`harness/tools/mod_template.py`), `install()` gives the tests the same eyes: it is called once the
test modules are collected (conftest.py) and replaces a few of their helpers.

What each adapter stands in for is in its docstring. They change how a test finds things on the
build, never what it asserts about the game's behaviour, with two exceptions said where they are
(the waiting room's song, the alt costume words). Expectations that are P+'s own data (a costume's
file number, a plate's colour by pixel, a stage's place on P+'s pages) are left failing on a mod:
`harness/tools/mod_suite.py` lists them apart.
"""
from __future__ import annotations

import os
import re
import struct
import sys
from pathlib import Path

from ppharness import paths as _paths  # noqa: E402,F401  (sets the build from the template's descriptor)

MOD_FOLDER = os.environ.get("PPHARNESS_MOD_FOLDER", "")
_pending: dict = {}


def active() -> bool:
    return bool(MOD_FOLDER)


def _game_key() -> str | None:
    """The build's key as Dolphin sends it to mm (tools/gamecode/game_key.py), booted from the
    template's Netplay Launcher."""
    from ppharness import paths
    import game_key
    template = paths.template_user_dir()
    try:
        return game_key.game_key(template / "Wii" / "sd.raw", MOD_FOLDER, template / paths.NETPLAY_LAUNCHER)["key"]
    except OSError:
        return None


def _game() -> dict:
    """mm's entry of this build (server/config/games.json, by the card folder)."""
    import json
    from ppharness import paths
    games = json.loads((paths.workspace_root() / "server" / "config" / "games.json").read_text())["games"]
    return next((g for g in games if g["folder"] == MOD_FOLDER), {})


def card_has(path: str) -> bool:
    """Whether the build's folder on the template's card has `path` ("pf/fighter/fox/FitFoxAltZ.pac";
    P+'s folder, Project+, without PPHARNESS_MOD_FOLDER). The name is matched case-insensitively,
    as the game's file patch does."""
    from ppharness import paths
    sdcard = str(Path(__file__).resolve().parents[2] / "tools" / "sdcard")
    if sdcard not in sys.path:
        sys.path.insert(0, sdcard)
    import fat32
    key = (str(paths.template_user_dir()), MOD_FOLDER, path.lower())
    if key not in _card_files:
        try:
            fs = fat32.open_image(paths.template_user_dir() / "Wii" / "sd.raw")
            e = fs.lookup(f"/{MOD_FOLDER or 'Project+'}/{path}")
            _card_files[key] = e is not None and not e.is_dir
        except OSError:
            _card_files[key] = True   # no card to read: as before
    return _card_files[key]


_card_files: dict = {}


def game_name() -> str:
    """What mm calls this build ("Cobalt 1.34"), as in its lines about it ("Ranked isn't on Cobalt
    1.34 yet.")."""
    return _game().get("name", MOD_FOLDER)


def game_id() -> str:
    """mm's id of this build ("cobalt-1.34"): what its rooms, records and rulesets carry."""
    return _game().get("id", "")


def _install_ruleset_game():
    """The tests publish rulesets for their rooms (POST /v1/rulesets). A ruleset says which build
    its stage ids are of and a room plays its own build's only (Ruleset.game): on a mod the tests'
    rulesets are the mod's."""
    from ppharness.backend import OnlineBackend
    if getattr(OnlineBackend._http, "_mod_wrapped", False):
        return
    orig = OnlineBackend._http

    def _http(self, method, path, body=None, *args, **kw):
        if method == "POST" and path == "/v1/rulesets" and isinstance(body, dict) and isinstance(body.get("ruleset"), dict):
            body = {**body, "ruleset": {**body["ruleset"], "game": game_id()}}
        return orig(self, method, path, body, *args, **kw)

    _http._mod_wrapped = True
    OnlineBackend._http = _http


def _room_module() -> str | None:
    """The stage module of the build's Online Training stage, from its Training.param on the
    template's card (P+: st_mansion.rel; Cobalt: st_battle_noshadowrotate.rel)."""
    from ppharness import paths
    import fat32
    try:
        fs = fat32.open_image(paths.template_user_dir() / "Wii" / "sd.raw")
        data = fs.read_file(f"/{MOD_FOLDER}/pf/stage/stageinfo/Training.param")
    except (OSError, fat32.FatError):
        return None
    names = re.findall(rb"[ -~]{4,}\.rel", data)
    return names[0].decode().lower() if names else None


def _closed_loop_pick(c, pad, right):
    from ppharness import brawl as B
    roster = B.PPLUS_CSS_ROSTER
    css = roster[9 + right] if 9 + right < len(roster) else roster[9]
    B.css_pick_character(c, 0, css, pad_port=pad)


def _install_pick_intercept():
    import drive
    from ppharness import brawl as B
    orig = drive.run
    if getattr(orig, "_mod_wrapped", False):
        return

    def run(c, steps, port=0, out="run/artifacts/game-code/screens"):
        st = [x.strip() for x in steps]
        key = id(c)
        try:
            on_css = B.read_scene(c.read_mem).scene is B.Scene.CSS
        except Exception:  # noqa: BLE001
            on_css = False
        if on_css and st == ["stick up 30", "wait 5", "tap A 8", "wait 30"]:
            _closed_loop_pick(c, port, 0)
            return orig(c, ["wait 30"], port=port, out=out)
        if on_css and st == ["stick up 30", "wait 5"]:
            _pending[key] = 0
            return None
        if on_css and key in _pending and len(st) == 2 and st[0].startswith("stick right ") and st[1] == "wait 5":
            _pending[key] = int(st[0].split()[2]) // 6
            return None
        if on_css and key in _pending and st == ["tap A 8", "wait 30"]:
            right = _pending.pop(key)
            _closed_loop_pick(c, port, right)
            return orig(c, ["wait 30"], port=port, out=out)
        _pending.pop(key, None)
        return orig(c, steps, port=port, out=out)

    run._mod_wrapped = True
    drive.run = run


def _steer_to_css(c, pad, css_id, timeout=1500):
    """The hand onto the character's icon, by closed loop, pressing nothing."""
    from ppharness import brawl as B
    route = B._serpentine(B.CSS_SCAN_X, B.CSS_SCAN_Y)
    wp = 0
    start = B.polls(c)
    prev, still = None, 0
    while B.polls(c) - start < timeout:
        a = B.read_css_area(c.read_mem, 0)
        if a is None:
            return False
        if a.hand_target == B.CSS_HAND_GRID_HOLDING and a.character != B.CSS_NONE:
            B.LEARNED_CSS.add(a.character, a.hand_x, a.hand_y)
        if a.hand_target == B.CSS_HAND_GRID_HOLDING and a.character == css_id:
            B.neutral(c, pad)
            B.step(c, 8)
            a = B.read_css_area(c.read_mem, 0)
            if a is not None and a.hand_target == B.CSS_HAND_GRID_HOLDING and a.character == css_id:
                return True
            continue
        # The coin is down (the area's character is the placed one, not the one under the hand):
        # to the icon's place as learned while the coin was in the hand.
        if not a.in_hand and css_id in B.LEARNED_CSS:
            tx, ty = B.LEARNED_CSS[css_id]
            if abs(tx - a.hand_x) < 0.4 and abs(ty - a.hand_y) < 0.4:
                B.neutral(c, pad)
                B.step(c, 4)
                return True
        if css_id in B.LEARNED_CSS:
            tx, ty = B.LEARNED_CSS[css_id]
            tilt = 1.2
        else:
            tx, ty = route[wp % len(route)]
            tilt = 3.0
            # (a waypoint past the hand's reach on this build, the sweep being wide for REX's grid:
            # on when the hand stops moving)
            still = still + 1 if prev is not None and abs(a.hand_x - prev[0]) + abs(a.hand_y - prev[1]) < 0.05 else 0
            if abs(tx - a.hand_x) + abs(ty - a.hand_y) < 2.5 or still > 10:
                wp, still = wp + 1, 0
        prev = (a.hand_x, a.hand_y)
        c.pad_set(pad, buttons=(), main=B.steer_toward(a.hand_x, a.hand_y, tx, ty, full_tilt_at=tilt))
        B.step(c, 1)
    B.neutral(c, pad)
    return False


# sel_char's character grid's y range (data, read by muSelCharTask's sel_char text+0xDC38: over
# the grid while bottom <= y <= top), the same address on every build of the family.
CSS_GRID_TOP = 0x806A0C64


def _outside_grid_tol(c, y, tol):
    """The closed loops' tolerance for a spot outside the build's character grid (the CSS's A goes
    to the grid while the hand's y is within it): one that stops outside it too. REX's grid reaches
    y -9.125 (P+'s -5.4), so a hand sent to a panel at -10 stopped within tol of it, on the grid."""
    try:
        top, bottom = struct.unpack(">2f", c.read_mem(CSS_GRID_TOP, 8))
    except Exception:  # noqa: BLE001 - no CSS module: as asked
        return tol
    if not -40.0 < bottom < top < 40.0:
        return tol
    if y < bottom:
        return min(tol, 0.8 * (bottom - y))
    if y > top:
        return min(tol, 0.8 * (y - top))
    return tol


def _install_rooms_game_hand_to():
    """test_rooms_game.hand_to (the room tests' closed loop): a spot outside the build's grid is
    reached outside it (_outside_grid_tol)."""
    m = sys.modules.get("test_rooms_game")
    if m is None or getattr(m.hand_to, "_mod_grid", False):
        return
    orig = m.hand_to

    def hand_to(g, x, y, tol=1.2):
        return orig(g, x, y, _outside_grid_tol(g.c, y, tol))
    hand_to._mod_grid = True
    _replace_everywhere(orig, hand_to)


# The middle of a character's icon, once measured (_centre_on_icon).
_ICON_CENTRE: dict = {}


def _centre_on_icon(g, css_id, goto):
    """From somewhere on the character's icon, the coin in the hand, to the icon's middle: the hand
    goes each way until it is off the icon, a frame of stick at a time, and then between the two
    ends, across and then up and down (`goto`: the test's own closed loop to a place). The closed
    loop to an icon stops where the hand first meets it, at its edge, and the game keeps a coin
    that is put down inside its icon: put down from a corner it is moved more than its own size
    away from the hand, and an A without moving then misses it (on P+ too: 5 of 143 places around
    one icon; REX's small icons side by side make a corner the usual way in)."""
    from ppharness import brawl as B
    PadInput = sys.modules["test_rooms_ui"].PadInput

    def over():
        a = B.read_css_area(g.c.read_mem, 0)
        return a, a is not None and a.hand_target == B.CSS_HAND_GRID_HOLDING and a.character == css_id

    def end(dx, dy):
        a, on = over()
        last, still = (a.hand_x, a.hand_y), 0
        for _ in range(60):
            g.c.pad_script(0, [PadInput(main=(128 + 60 * dx, 128 + 60 * dy), hold=1), PadInput(hold=2)])
            g.sim.service(3)
            a, on = over()
            if not on:
                break
            still = still + 1 if abs(a.hand_x - last[0]) + abs(a.hand_y - last[1]) < 0.01 else 0
            last = (a.hand_x, a.hand_y)
            if still > 3:   # the screen's edge
                break
        return last

    a, on = over()
    if not on:
        return False
    start = (a.hand_x, a.hand_y)
    left = end(-1, 0)
    goto(*start)
    right = end(1, 0)
    mid_x = (left[0] + right[0]) * 0.5
    goto(mid_x, start[1])
    if not over()[1]:
        goto(*start)
        return False
    down = end(0, -1)
    goto(mid_x, start[1])
    up = end(0, 1)
    centre = (mid_x, (down[1] + up[1]) * 0.5)
    goto(*centre)
    if not over()[1]:
        goto(*start)
        return False
    _ICON_CENTRE[css_id] = centre
    return True


def _install_hand_to_fox():
    """test_rooms_ui's Game.hand_to(*FOX) and its token test's portraits (TOKEN_CHARS): an icon by
    its place on P+'s select; on a mod, the same character's icon by closed loop."""
    m = sys.modules.get("test_rooms_ui")
    if m is None or getattr(m.Game, "_mod_hand", False):
        return
    from ppharness import brawl as B
    orig = m.Game.hand_to
    # The token test's four portraits are four of P+'s select (ids and places): on a mod, four
    # characters every build of the family has, each named by a place no select has, which only
    # the closed loop reaches.
    if hasattr(m, "TOKEN_CHARS"):
        m.TOKEN_CHARS[:] = [(B.CSS_ID[name], (1000.0 + i, 1000.0))
                            for i, name in enumerate(("mario", "link", "fox", "samus"))]
    icons = {m.FOX: B.CSS_ID["fox"], **{pos: css for css, pos in getattr(m, "TOKEN_CHARS", [])}}
    # The token test takes its coin up again with an A where it put it down: its portraits are
    # reached at their middle (_centre_on_icon), measured once each.
    middles = {pos for _css, pos in getattr(m, "TOKEN_CHARS", [])}

    def hand_to(self, x, y, tol=1.2):
        for (ix, iy), css in icons.items():
            if abs(x - ix) < 0.01 and abs(y - iy) < 0.01:
                if (ix, iy) in middles and css in _ICON_CENTRE:
                    return orig(self, *_ICON_CENTRE[css], 0.3)
                if _steer_to_css(self.c, 0, css):
                    if (ix, iy) in middles and getattr(self, "sim", None) is not None:
                        _centre_on_icon(self, css, lambda px, py: orig(self, px, py, 0.3))
                    return None
                break
        return orig(self, x, y, _outside_grid_tol(self.c, y, tol))

    m.Game.hand_to = hand_to
    m.Game._mod_hand = True


def _install_stage_picker():
    """ppharness.brawl.sss_pick_stage scans P+'s stage select (three pages of up to 22 icons) and
    expects the wanted stage on the page shown. A mod's pages are other sizes (Cobalt: 27 and 33
    icons): scan the whole cursor range in finer rows, and turn to the stage's page first with
    the page item, as a player does."""
    from ppharness import brawl as B
    if getattr(B.sss_pick_stage, "_mod_wrapped", False):
        return
    B.SSS_SCAN_X = (-29.0, 29.0)
    B.SSS_SCAN_Y = tuple(-19.0 + 2.4 * i for i in range(17))
    orig = B.sss_pick_stage

    def pick_stage(drv, stage_kind, port=0, **kw):
        targets = B.stage_positions(stage_kind, drv.read_mem)
        v = None
        for _ in range(150):
            v = B.read_sss(drv.read_mem)
            if v is not None and v.clock >= 40:
                break
            B.step(drv, 2)
        if v is not None and targets and all(t[0] != v.page for t in targets):
            want = targets[0][0]
            for _ in range(4):
                vv = B.read_sss(drv.read_mem)
                if vv is None or vv.page == want:
                    break
                try:
                    B.sss_hover_item(drv, B.SSS_ITEM_PAGE, port, timeout_frames=500)
                    B.tap(drv, port, ["A"], hold=3, release=25)
                except B.RecipeError:
                    break
        return orig(drv, stage_kind, port, **kw)

    pick_stage._mod_wrapped = True
    B.sss_pick_stage = pick_stage


def _install_alt_words():
    """The alt costume tests read and write P+'s alt words (0x901C4618). A P+Ex build has its own
    alt code instead (Hidden Alt Costume Sets): a request byte per slot, which its fighter loader
    takes once (the costume becomes 62 / 61) and createFighter clears. On such a build "the alt
    words" of the tests are, per slot, the pending request or else the alt the slot's match
    costume already is; stale words are written as stale requests."""
    import struct
    ma = sys.modules.get("test_online_alt_costumes")
    if ma is None or getattr(ma, "_mod_alt", False):
        return
    ma._mod_alt = True
    orig_words, orig_stale = ma._alt_words, ma._stale_words

    def requests(g):
        if g.c.read_u32(0x8084CE34) != 0x4800001C:
            return None
        p = g.c.read_u32(0x8084CE38)
        if not 0x80001800 <= p < 0x817FFF00 or g.c.read_mem(p + 4, 17) != b"EtcAlt\0AltZ\0AltR\0":
            return None
        return p

    def alt_words(g):
        p = requests(g)
        if p is None:
            return orig_words(g)
        req = list(g.c.read_mem(p, 4))
        worn = [{62: 1, 61: 2}.get(c, 0) for c in ma._mm_costumes(g)]
        return [r or w for r, w in zip(req, worn)]

    def stale_words(words):
        def write(g):
            p = requests(g)
            if p is None:
                return orig_stale(words)(g)
            g.c.write_mem(p, bytes(words))
            assert list(g.c.read_mem(p, 4)) == words
        return write

    for m in (ma, sys.modules.get("test_waiting_room")):
        if m is not None:
            m._alt_words, m._stale_words = alt_words, stale_words


def _install_code_menu():
    """test_online_css finds the Code Menu's value lines within P+'s 0x2520 bytes. A mod's menu
    is longer (Cobalt 0x3D20, P+Ex 0x3B10): look in 0x4000, and add a stale line that lies past
    P+'s size on both (Flight Mode: Cobalt 0x27C0, P+Ex 0x259C) to what the stale-settings test
    leaves behind offline."""
    import struct
    m = sys.modules.get("test_online_css")
    if m is None or getattr(m, "_mod_code_menu", False):
        return
    m._mod_code_menu = True
    m.CODE_MENU_SIZE = 0x4000
    m.CODE_MENU_STALE = dict(m.CODE_MENU_STALE, **{"Flight Mode": struct.pack(">I", 1)})


def _replace_everywhere(old, new):
    """Tests import each other's helpers by name: put `new` wherever a test module has `old`."""
    for name, mod in list(sys.modules.items()):
        if not name.startswith("test_") or mod is None:
            continue
        for k, v in list(vars(mod).items()):
            if v is old:
                setattr(mod, k, new)


def _slot_table(c):
    """The build's character select slot table, from the game's own lookup (as build.cpp)."""
    import struct
    code = struct.unpack(">3I", c.read_mem(0x800AF708, 12))
    if code[0] >> 16 == 0x3C80 and code[2] >> 16 == 0x3884:
        return ((code[0] & 0xFFFF) << 16) + struct.unpack(">h", struct.pack(">H", code[2] & 0xFFFF))[0]
    return 0x80585B00


def _install_slot_table():
    """Tests that read a character's costume list go to P+'s slot table (0x80585B08): on a mod,
    the build's own (BrawlEx's)."""
    rg = sys.modules.get("test_rooms_game")
    if rg is not None and not getattr(rg, "_mod_slot_table", False):
        rg._mod_slot_table = True

        def costume_colour(g, css, costume):
            return g.c.read_mem(rg._u32(g, _slot_table(g.c) + 8 + css * 0x10) + 2 * costume, 1)[0]

        _replace_everywhere(rg.costume_colour, costume_colour)
        # (and the reads of P+'s table inside test functions: a slot's costume list pointer)
        old_u32 = rg._u32

        def _u32(g, a):
            if 0x80585B08 <= a < 0x80585B08 + 0x1000 and (a - 0x80585B08) % 0x10 == 0:
                a = _slot_table(g.c) + 8 + (a - 0x80585B08)
            return old_u32(g, a)

        _replace_everywhere(old_u32, _u32)
    sm = sys.modules.get("test_special_modes")
    if sm is not None and not getattr(sm, "_mod_slot_table", False):
        sm._mod_slot_table = True
        old_files = sm.costume_files

        def costume_files(g, css):
            sm.COSTUME_LIST = _slot_table(g.c) + 8
            return old_files(g, css)

        _replace_everywhere(old_files, costume_files)


def _install_card_folder():
    """Tests add files to the card under /Project+/: on a mod, its own folder. And the waiting
    room's stage module is the build's own (its Training.param names it; P+'s st_mansion.rel)."""
    import patch_sd
    if not getattr(patch_sd, "_mod_folder", False):
        patch_sd._mod_folder = True
        orig = patch_sd.patch_image
        folder = "/" + MOD_FOLDER + "/"

        def patch_image(image, adds, removes=()):
            adds = [(d, folder + pth[len("/Project+/"):] if pth.startswith("/Project+/") else pth) for d, pth in adds]
            # (The waiting room's song test gives two stages a tracklist with only the room's
            # song, X07, which P+'s room plays; a mod's room list may not have it: the room too.)
            adds += [(d, pth.rsplit("/", 1)[0] + "/Training_Room.tlst") for d, pth in adds
                     if pth.endswith("/Smashville.tlst")]
            return orig(image, adds, removes)

        patch_sd.patch_image = patch_image
    m = sys.modules.get("test_waiting_room")
    module = _room_module()
    if m is not None and module:
        m.ST_MANSION = module


def _install_hover_other():
    """test_online_unranked._hover_other moves the hand right from Fox to find another character:
    on a build whose grid has Fox at a row's end there is none that way. Try the other ways."""
    m = sys.modules.get("test_online_unranked")
    if m is None or getattr(m, "_mod_hover", False):
        return
    m._mod_hover = True
    old = m._hover_other

    def _hover_other(g, direction, prev):
        last = None
        for d in (direction, "left", "down", "up"):
            try:
                return old(g, d, prev)
            except AssertionError as e:
                last = e
        raise last

    m._hover_other = _hover_other


def _relax_room_song(m):
    """test_waiting_room._check_room asserts Brawl's own waiting room song (X07), which the
    plugin plays only on a card whose Training Room tracklist has it (P+'s): a mod keeps its own
    pick. Drop those lines so that the rest of the check runs on a mod."""
    import inspect
    import textwrap
    src = textwrap.dedent(inspect.getsource(m._check_room))
    out, skip = [], False
    for ln in src.splitlines():
        if "assert _room(g) & ROOM_SONG" in ln or 'songs = re.findall' in ln or 'assert songs and songs[-1] == "X07"' in ln \
                or 'text = (g.inst.user_dir / "Logs" / "dolphin.log")' in ln:
            continue
        out.append(ln)
    ns = m.__dict__
    exec(compile("\n".join(out), "<mod _check_room>", "exec"), ns)


def _install_network_heap():
    """test_waiting_room looks for the plugin's banner text in the Network heap by P+'s address
    and size: the build's own, from the game's heap table (Cobalt's is 0x166700 bytes)."""
    m = sys.modules.get("test_waiting_room")
    if m is None or getattr(m, "_mod_heap", False):
        return
    m._mod_heap = True
    orig = m._banner_shown

    def banner_shown(g, text):
        import gameplay_rollback as G
        for h in G.heap_dicts(g.c):
            if h["name"] == "Network":
                m.NETWORK_HEAP, m.NETWORK_HEAP_SIZE = h["start"], h["end"] - h["start"]
        return orig(g, text)

    m._banner_shown = banner_shown


def install() -> None:
    """Called by conftest.py when the tests are collected, on a mod only."""
    if not active():
        return
    root = Path(__file__).resolve().parents[2]
    for sub in ("tools/sdcard", "tools/gamecode", "harness/tools"):
        if str(root / sub) not in sys.path:
            sys.path.insert(0, str(root / sub))
    # The tests' fake players (server/target/debug/mmclient) are of the mod's build, as mm takes
    # into a room and pairs only players of one build (docs/design/pplus-mods.md §3).
    key = _game_key()
    if key:
        os.environ["MMCLIENT_GAME"] = key
    _install_ruleset_game()
    _install_pick_intercept()
    # A build's grid may be wider than P+'s (REX's rows of 14 reach x 36.4, its RANDOM at the right
    # end): the character searches sweep to x 37.
    from ppharness import brawl as B
    B.CSS_SCAN_X = (-37.0, 37.0)
    _install_hand_to_fox()
    _install_rooms_game_hand_to()
    _install_stage_picker()
    _install_alt_words()
    _install_code_menu()
    _install_slot_table()
    _install_card_folder()
    _install_hover_other()
    _install_network_heap()
    m = sys.modules.get("test_waiting_room")
    if m is not None and not getattr(m, "_mod_song", False):
        m._mod_song = True
        _relax_room_song(m)
