"""Offline tests for ppharness.brawl: synthetic Brawl memory fed to the parsers, plus a tiny
simulated CSS / SSS / match that the closed-loop recipes drive end to end.

Run: python -m unittest discover -s harness/tests   (from the repo root)
"""
import math
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from ppharness import brawl as b  # noqa: E402


# ------------------------------------------------------------------------------------------
# Synthetic memory
# ------------------------------------------------------------------------------------------


class FakeMem:
    """Sparse big-endian RAM. Unwritten bytes read as zero, like cleared RAM."""

    def __init__(self):
        self.bytes = {}
        self.next_alloc = 0x81000000

    def write(self, addr, data):
        for i, v in enumerate(data):
            self.bytes[addr + i] = v

    def read(self, addr, n):
        return bytes(self.bytes.get(addr + i, 0) for i in range(n))

    __call__ = read

    def u8(self, a, v):
        self.write(a, bytes([v & 0xFF]))

    def u16(self, a, v):
        self.write(a, struct.pack(">H", v & 0xFFFF))

    def u32(self, a, v):
        self.write(a, struct.pack(">I", v & 0xFFFFFFFF))

    def s32(self, a, v):
        self.write(a, struct.pack(">i", v))

    def f32(self, a, v):
        self.write(a, struct.pack(">f", v))

    def alloc(self, size, align=0x20):
        a = (self.next_alloc + align - 1) & ~(align - 1)
        self.next_alloc = a + size
        return a

    def string(self, s):
        a = self.alloc(len(s) + 1, 4)
        self.write(a, s.encode() + b"\0")
        return a


class World:
    """Builds the game objects the parsers walk."""

    def __init__(self):
        self.m = FakeMem()
        m = self.m
        self.manager = m.alloc(0x320)
        m.u32(b.SCENE_MANAGER_PTR, self.manager)
        self.gg = m.alloc(0x50)
        m.u32(b.GAME_GLOBAL_PTR, self.gg)
        self.mm = m.alloc(b.MM_SIZE)
        self.scd = m.alloc(0x340)
        self.rule = m.alloc(0x88)
        self.result = m.alloc(0x1388)
        self.record = m.alloc(0x1000)
        for off, p in ((b.GG_MODE_MELEE, self.mm), (b.GG_SEL_CHAR_DATA, self.scd),
                       (b.GG_SET_RULE, self.rule), (b.GG_RESULT_INFO, self.result), (b.GG_RECORD, self.record)):
            m.u32(self.gg + off, p)
        self.scenes = {}
        self.app = m.alloc(0x170)
        m.u32(b.APPLICATION_PTR, self.app)

    def set_scene(self, name, sequence="sqVsMelee"):
        m = self.m
        if name not in self.scenes:
            obj = m.alloc(0x420)
            m.u32(obj, m.string(name))
            self.scenes[name] = obj
        m.u32(self.manager + b.SM_CURRENT_SCENE, self.scenes[name])
        seq = m.alloc(8)
        m.u32(seq, m.string(sequence))
        m.u32(self.manager + b.SM_CURRENT_SEQUENCE, seq)
        return self.scenes[name]

    def set_player_setup(self, port, char_kind, state=b.PLAYER_HUMAN, stocks=4):
        base = self.mm + b.MM_PLAYERS + port * b.MM_PLAYER_SIZE
        self.m.write(base, bytes([char_kind, state, port, port, stocks & 0xFF, 0, 0, port]))

    def set_mode(self, game_mode=0, rule=1, players=2, stage=1, frames=8 * 3600, stamina=False):
        m = self.m
        m.u8(self.mm + 0x08, game_mode << 2)
        m.u8(self.mm + 0x09, (rule << 5) | (players << 2))
        m.u8(self.mm + 0x0B, 0x20 if stamina else 0)
        m.u16(self.mm + b.MM_STAGE_KIND, stage)
        m.s32(self.mm + b.MM_TIME_LIMIT_FRAMES, frames)

    def add_fighter(self, index, port, char_kind, ft_kind, damage, stocks, pos, prev, lr=1.0, action=0x10):
        m = self.m
        if m.read(b.FT_ENTRY_MANAGER, 4) == b"\0\0\0\0":
            self.entries = m.alloc(b.FTE_SIZE * 4)
            m.u32(b.FT_ENTRY_MANAGER, self.entries)
            m.u32(b.FT_ENTRY_MANAGER + 4, 4)
        e = self.entries + index * b.FTE_SIZE
        m.s32(e + b.FTE_ENTRY_ID, 0x10000 * index + 1)
        m.u8(e + b.FTE_ACTIVE_INSTANCE, 0)
        owner = m.alloc(0x10)
        data = m.alloc(0x100)
        m.u32(e + b.FTE_OWNER, owner)
        m.u32(owner + b.OWNER_DATA, data)
        m.f32(data + b.OWNER_DAMAGE, damage)
        m.s32(data + b.OWNER_STOCKS, stocks)
        fighter = m.alloc(0x194)
        m.u32(e + b.FTE_INSTANCES, ft_kind)
        m.u32(e + b.FTE_INSTANCES + 4, fighter)
        m.s32(e + b.FTE_PLAYER_NO, port)
        m.u32(e + b.FTE_CHARACTER_KIND, char_kind)
        m.u32(fighter + b.FIGHTER_KIND, ft_kind)
        acc = m.alloc(0xE0)
        m.u32(fighter + b.FIGHTER_ACCESSER, acc)
        en = acc + 0x0C
        m.u32(acc + b.ACC_ENUMERATION, en)
        posture = m.alloc(0x78)
        status = m.alloc(0xA4)
        motion = m.alloc(0x170)
        m.u32(en + b.ENUM_POSTURE, posture)
        m.u32(en + b.ENUM_STATUS, status)
        m.u32(en + b.ENUM_MOTION, motion)
        m.write(posture + b.POSTURE_POS, struct.pack(">fff", pos[0], pos[1], 0.0))
        m.write(posture + b.POSTURE_PREV_POS, struct.pack(">fff", prev[0], prev[1], 0.0))
        m.f32(posture + b.POSTURE_LR, lr)
        m.s32(status + b.STATUS_KIND, action)
        m.f32(motion + b.MOTION_FRAME, 3.0)
        m.s32(motion + b.MOTION_KIND, 0x1A)
        return e, data, posture

    def melee_rule(self, game_set=False, start=True, remaining=8 * 3600 - 100, elapsed=100):
        scene = self.scenes["scMelee"]
        rule = self.m.alloc(0x260)
        self.m.u32(scene + b.SCM_OPERATOR_RULE_MELEE, rule)
        self.m.u8(rule + b.OPR_IS_GAME_SET, 1 if game_set else 0)
        self.m.u8(rule + b.OPR_IS_START, 1 if start else 0)
        self.m.u32(rule + b.OPR_REMAINING_FRAMES, remaining)
        self.m.u32(rule + b.OPR_FRAMES_ELAPSED, elapsed)
        return rule


# ------------------------------------------------------------------------------------------
# Parser tests
# ------------------------------------------------------------------------------------------


class SceneTests(unittest.TestCase):
    def test_classify(self):
        self.assertIs(b.classify_scene("scSelctCharacter"), b.Scene.CSS)
        self.assertIs(b.classify_scene("scSelStage"), b.Scene.SSS)
        self.assertIs(b.classify_scene("scMelee"), b.Scene.IN_MATCH)
        self.assertIs(b.classify_scene("scVsResult"), b.Scene.RESULTS)
        self.assertIs(b.classify_scene("scMemoryChange"), b.Scene.LOADING)
        self.assertIs(b.classify_scene("muMenuMain"), b.Scene.MAIN_MENU)
        self.assertIs(b.classify_scene("scStrap"), b.Scene.STRAP)
        self.assertIs(b.classify_scene("scBoot"), b.Scene.BOOT)
        self.assertIs(b.classify_scene("scTitle"), b.Scene.TITLE)
        self.assertIs(b.classify_scene("scAdvMap"), b.Scene.OTHER)
        self.assertIs(b.classify_scene("\x01garbage"), b.Scene.UNKNOWN)

    def test_read_scene(self):
        w = World()
        w.set_scene("scSelctCharacter", "sqVsMelee")
        info = b.read_scene(w.m)
        self.assertIs(info.scene, b.Scene.CSS)
        self.assertEqual(info.name, "scSelctCharacter")
        self.assertEqual(info.sequence, "sqVsMelee")
        self.assertFalse(info.is_banned_mode)

    def test_unknown_when_not_alive(self):
        m = FakeMem()
        self.assertIs(b.read_scene(m).scene, b.Scene.UNKNOWN)          # null manager
        m.u32(b.SCENE_MANAGER_PTR, 0x12345678)                          # launcher garbage
        self.assertIs(b.read_scene(m).scene, b.Scene.UNKNOWN)
        w = World()
        obj = w.m.alloc(8)
        w.m.u32(obj, w.m.string("\x7f\x01xx"))
        w.m.u32(w.manager + 4, obj)
        self.assertIs(b.read_scene(w.m).scene, b.Scene.UNKNOWN)

    def test_frame_counters(self):
        w = World()
        w.m.u32(w.app + b.APP_FRAME_COUNTER_OFF, 1234)
        w.m.u32(b.GAME_FRAME + 0x14, 99)
        fc = b.read_frame_counters(w.m)
        self.assertEqual(fc["app"], 1234)
        self.assertEqual(fc["persistent"], 99)


class MatchTests(unittest.TestCase):
    def make_match(self):
        w = World()
        w.set_scene("scMelee")
        w.set_mode(stage=b.STAGE_KIND["battlefield"])
        w.set_player_setup(0, b.CHAR_KIND["fox"])
        w.set_player_setup(1, b.CHAR_KIND["falco"], state=b.PLAYER_CPU)
        for p in (2, 3):
            w.set_player_setup(p, 0, state=b.PLAYER_NONE)
        w.add_fighter(0, 0, b.CHAR_KIND["fox"], b.FT_KIND["fox"], 42.5, 4, (10.0, 20.0), (9.0, 21.5), lr=-1.0, action=0x22)
        w.add_fighter(1, 1, b.CHAR_KIND["falco"], b.FT_KIND["falco"], 0.0, 3, (-30.0, 0.0), (-30.0, 0.0))
        w.melee_rule()
        return w

    def test_setup(self):
        w = self.make_match()
        s = b.read_match_setup(w.m)
        self.assertEqual((s.game_mode, s.game_rule, s.num_players, s.stage_kind), (0, 1, 2, 1))
        self.assertEqual(s.time_limit_frames, 28800)
        self.assertEqual(s.players[0].character, 0x07)
        self.assertTrue(s.players[1].present)
        self.assertFalse(s.players[2].present)
        self.assertEqual(b.verify_match_setup(w.m, {0: 0x07}, 1), [])
        self.assertTrue(b.verify_match_setup(w.m, {0: 0x00}, 2))

    def test_players(self):
        w = self.make_match()
        st = b.read_match_state(w.m)
        self.assertTrue(st.in_match)
        self.assertTrue(st.started)
        self.assertFalse(st.ended)
        self.assertEqual(st.frames_elapsed, 100)
        self.assertAlmostEqual(st.remaining_seconds, (28800 - 100) / 60)
        p1, p2 = st.players
        self.assertEqual((p1.port, p1.ft_kind, p1.stocks, p1.action, p1.facing), (0, 6, 4, 0x22, -1.0))
        self.assertAlmostEqual(p1.damage, 42.5)
        self.assertAlmostEqual(p1.vel_x, 1.0)
        self.assertAlmostEqual(p1.vel_y, -1.5)
        self.assertTrue(p1.human)
        self.assertFalse(p2.human)
        self.assertEqual(p2.stocks, 3)

    def test_game_set(self):
        w = self.make_match()
        w.melee_rule(game_set=True)
        self.assertTrue(b.read_match_state(w.m).ended)

    def test_results(self):
        w = World()
        w.m.u16(w.result + b.RI_STAGE, 1)
        w.m.u8(w.result + b.RI_DECISION, 2)
        rec = w.result + b.RI_PLAYERS + b.RI_PLAYER_SIZE
        w.m.write(rec, bytes([0x15, 0]))
        w.m.u8(rec + 0x0E, 0)
        r = b.read_results(w.m)
        self.assertEqual((r["stage"], r["decision"], r["players"][1]["character"]), (1, 2, 0x15))


class ChecksumTests(unittest.TestCase):
    def test_brawlback_checksum(self):
        w = MatchTests().make_match()
        m = w.m
        # Brawlback chain: [0x80624780] -> entries; +0x34 -> fighter; +0x60 -> acc; +0xD8 -> enum;
        # +0xC -> posture; +0xC / +0x10.
        ranges = b.brawlback_checksum_ranges(m)
        expected = 0
        for f in b.BRAWLBACK_CHECKSUM_FIELDS:
            r = b.resolve_checksum_field(m, f)
            if r:
                expected = zlib.crc32(m.read(*r), expected)
        self.assertEqual(b.brawlback_checksum(m), expected)
        names = [f.name for f in b.BRAWLBACK_CHECKSUM_FIELDS]
        self.assertEqual(names[0], "persistentFrameCounter")
        # P3/P4 pointer chains resolve to null fighters -> skipped (12 static + 4 positions)
        self.assertEqual(len(ranges), 1 + 8 + 4 + 4)
        p1x = b.resolve_checksum_field(m, b.BRAWLBACK_CHECKSUM_FIELDS[9])
        self.assertEqual(struct.unpack(">f", m.read(*p1x))[0], 10.0)

    def test_zero_outside_match(self):
        w = World()
        w.set_scene("scSelctCharacter")
        self.assertEqual(b.brawlback_checksum(w.m), 0)


class BannedTests(unittest.TestCase):
    def test_clean(self):
        w = MatchTests().make_match()
        self.assertEqual(b.check_banned(w.m), [])

    def test_giga_and_warioman(self):
        w = MatchTests().make_match()
        w.set_player_setup(1, b.CHAR_GIGA_BOWSER)
        w.add_fighter(0, 0, b.CHAR_KIND["wario"], b.FT_WARIOMAN, 0.0, 4, (0, 0), (0, 0))
        out = " | ".join(b.check_banned(w.m))
        self.assertIn("Giga Bowser", out)
        self.assertIn("Wario-Man", out)

    def test_modes_and_debug(self):
        w = World()
        w.set_scene("scMelee", "sqAdventure")
        w.set_mode(game_mode=6, stage=b.STAGE_SUBSPACE)
        w.m.u8(b.DEBUG_FLAGS + 0xB, 1)
        w.m.u32(b.CODE_MENU_STATE, 4)
        out = " | ".join(b.check_banned(w.m))
        for needle in ("Subspace", "game mode", "special stage", "Debug Mode", "Code Menu"):
            self.assertIn(needle, out)
        w.set_scene("scAdvMap", "sqAdventure")
        self.assertIn("scAdvMap", " | ".join(b.check_banned(w.m)))

    def test_boss_sequence(self):
        w = World()
        w.set_scene("scMelee", "sqSingleBoss")
        self.assertTrue(any("Boss Battles" in x for x in b.check_banned(w.m)))

    def test_css_special_slot(self):
        w = World()
        scene = w.set_scene("scSelctCharacter")
        task = w.m.alloc(0x654)
        w.m.u32(scene + b.CSS_SCENE_TASK, task)
        area = w.m.alloc(0x448)
        w.m.u32(task + b.CSS_TASK_AREAS, area)
        w.m.u32(area + b.AREA_KIND, b.CSS_AREA_HUMAN)
        w.m.u32(area + b.AREA_CHARACTER, b.CSS_PPLUS_GIGA_BOWSER)
        self.assertTrue(any("Giga" in x for x in b.check_banned(w.m)))


class RegionTests(unittest.TestCase):
    def test_heap_table_and_ranges(self):
        w = World()
        m = w.m
        heaps = {"System": (0x80611F60, 0x61500), "Fighter1Instance": (0x8123AB60, 0x52000),
                 "Sound": (0x90199800, 0xCC7C00), "MenuInstance": (0x811AA160, 0x4EB900)}
        for i, (name, (start, size)) in enumerate(heaps.items(), start=2):
            m.write(b.HEAP_INFOS + 16 * i, struct.pack(">IIII", m.string(name), start, size, 0))
        table = {h.name: h for h in b.read_heap_table(m)}
        self.assertEqual(set(table), set(heaps))
        self.assertEqual(table["System"].end, 0x80611F60 + 0x61500)
        gr = b.gameplay_ranges(m)
        labels = " ".join(l for _, _, l in gr)
        self.assertIn("Fighter1Instance", labels)
        self.assertNotIn("Sound", labels)
        self.assertIn("ftEntryManager", labels)   # inside System -> merged label
        mr = b.menu_ranges(m)
        self.assertTrue(any("MenuInstance" in l for _, _, l in mr))
        self.assertTrue(any(a == b.CODE_MENU_BASE for a, _, _ in mr))

    def test_merge_subtract(self):
        r = b.merge_ranges([(0x100, 0x10, "a"), (0x108, 0x10, "b"), (0x200, 4, "c")])
        self.assertEqual(r, [(0x100, 0x18, "a+b"), (0x200, 4, "c")])
        s = b.subtract_ranges([(0x100, 0x100, "x")], [(0x140, 0x20)])
        self.assertEqual(s, [(0x100, 0x40, "x"), (0x160, 0xA0, "x")])


class ExclusionTests(unittest.TestCase):
    def test_disk_id_is_never_hashed(self):
        # Rev 1 and Rev 2 differ only in the DVDDiskID at 0x80494938 (0x20 bytes).
        r = b.exclude_state_noise([(0x80400000, 0x100000, "mem")])
        for a, n, _ in r:
            self.assertTrue(a + n <= 0x80494938 or a >= 0x80494958, (hex(a), hex(n)))
        self.assertEqual(sum(n for _, n, _ in r), 0x100000 - 0x20 - 0xC00 - 0x1400)
        self.assertIn(b.DISK_ID_RANGE, b.STATE_EXCLUDED_RANGES)


class IdTests(unittest.TestCase):
    def test_css_to_char_kind(self):
        for name in ("mario", "fox", "falco", "wario", "bowser", "marth", "sonic", "sheik", "zero_suit_samus"):
            self.assertEqual(b.CSS_TO_CHAR_KIND[b.CSS_ID[name]], b.CHAR_KIND[name], name)

    def test_stage_pages(self):
        pages = b.read_stage_pages()
        self.assertEqual(len(pages[0]), 21)
        self.assertEqual(b.stage_positions(b.STAGE_KIND["battlefield"]), [(0, 17)])
        self.assertEqual(b.stage_positions(b.STAGE_KIND["final_destination"]), [(0, 10)])
        m = FakeMem()
        raw = bytearray(len(b.SWITCHFF_PAGES_AND_SLOTS))
        raw[0], raw[1], raw[2] = 2, 5, 0     # page 0: slots 5, 0
        raw[0xC8 + 2 * 5] = 0x21             # slot 5 -> Smashville
        raw[0xC8 + 0] = 0x01                 # slot 0 -> Battlefield
        m.write(b.RSS_PAGES, bytes(raw))
        self.assertEqual(b.read_stage_pages(m)[0], [0x21, 0x01])

    def test_registry(self):
        for name, a in b.ADDRESSES.items():
            self.assertIn(a.verify, {"dol-verified", "decomp", "headers", "orca", "brawlback", "pplus", "live", "live-verified", "wrong"}, name)
            self.assertTrue(b.is_mem_addr(a.addr), name)

    def test_steer(self):
        self.assertEqual(b.steer_toward(0, 0, 0, 0), (128, 128))
        x, y = b.steer_toward(0, 0, 10, 0)
        self.assertEqual((x, y), (228, 128))
        x, y = b.steer_toward(0, 0, 0, -0.5)
        self.assertEqual(x, 128)
        self.assertLessEqual(y, 128 - 30)


# ------------------------------------------------------------------------------------------
# Simulated game for the recipes
# ------------------------------------------------------------------------------------------


class SimGame:
    """A Driver that simulates P+'s CSS hands, SSS cursor and the match start in FakeMem."""

    ICON_W, ICON_H = 6.0, 4.0

    def __init__(self, ports=(0, 1), delay=0):
        self.delay = delay          # netplay-like input delay, in frames
        self.history = []           # pads as sampled each poll, for the delay
        self.w = World()
        self.m = self.w.m
        self.polls = 0
        self.pads = {p: {"buttons": set(), "main": (128, 128)} for p in range(4)}
        self.scripts = {p: [] for p in range(4)}
        self.prev_buttons = {p: set() for p in range(4)}
        self.writes = []
        self.scene = None
        self.enter("scSelctCharacter")
        self.css = self.w.scenes["scSelctCharacter"]
        self.task = self.m.alloc(0x654)
        self.m.u32(self.css + b.CSS_SCENE_TASK, self.task)
        self.hands = {}
        for p in range(4):
            area = self.m.alloc(0x448)
            hand = self.m.alloc(0xE0)
            self.m.u32(self.task + b.CSS_TASK_AREAS + 4 * p, area)
            self.m.u32(area + b.AREA_HAND, hand)
            self.hands[p] = {"area": area, "hand": hand, "x": -10.0 + 8 * p, "y": -8.0, "kind": 0,
                             "in_hand": True, "char": b.CSS_NONE}
        self.grid = {}
        roster = list(b.PPLUS_CSS_ROSTER)
        for i, cid in enumerate(roster):
            row, col = divmod(i, 9)
            self.grid[cid] = (-27.0 + col * self.ICON_W, 14.0 - row * self.ICON_H)
        self.stage_pages = b.read_stage_pages()
        self.sss_task = None
        self.taken = 0
        self.cursor = None
        self.sync()

    # -- Driver protocol --
    def read_mem(self, addr, length):
        return self.m.read(addr, length)

    def write_mem(self, addr, data):
        self.writes.append((addr, bytes(data)))
        self.m.write(addr, data)

    def pad_set(self, port, pad=None, *, buttons=(), main=(128, 128), c=(128, 128), l=0, r=0):
        self.pads[port] = {"buttons": set(buttons), "main": tuple(main)}

    def pad_script(self, port, frames, start="next"):
        for f in frames:
            for _ in range(f.get("hold", 1)):
                self.scripts[port].append({"buttons": set(f.get("buttons", ())), "main": tuple(f.get("main", (128, 128)))})
        return {"starts_at": self.polls + 1, "ends_at": self.polls + len(self.scripts[port])}

    def wait_frame(self, frame=None, *, input_polls=None, timeout_ms=10000):
        while self.polls < input_polls:
            self.tick()
        return {"frame": self.polls, "input_polls": self.polls}

    def status(self):
        return {"input_polls": self.polls, "frame": self.polls}

    # -- simulation --
    def enter(self, name):
        self.scene = name
        self.w.set_scene(name, "sqVsMelee")
        self.scene_age = 0

    def char_at(self, x, y):
        for cid, (cx, cy) in self.grid.items():
            if abs(x - cx) <= self.ICON_W / 2 and abs(y - cy) <= self.ICON_H / 2:
                return cid
        return None

    def pad_now(self, p):
        return self.scripts[p].pop(0) if self.scripts[p] else self.pads[p]

    def tick(self):
        self.polls += 1
        self.scene_age += 1
        pads = {p: self.pad_now(p) for p in range(4)}
        self.history.append(pads)
        if self.delay:
            pads = self.history[-1 - self.delay] if len(self.history) > self.delay else                 {p: {"buttons": set(), "main": (128, 128)} for p in range(4)}
        pressed = {p: pads[p]["buttons"] - self.prev_buttons[p] for p in range(4)}
        self.prev_buttons = {p: set(pads[p]["buttons"]) for p in range(4)}
        if self.scene == "scSelctCharacter":
            self.tick_css(pads, pressed)
        elif self.scene == "scSelStage":
            self.tick_sss(pads, pressed)
        elif self.scene == "scMemoryChange" and self.scene_age > 30:
            self.enter("scMelee")
            self.rule = self.w.melee_rule(start=False, elapsed=0)
        elif self.scene == "scMelee":
            self.m.u32(self.rule + b.OPR_FRAMES_ELAPSED, max(0, self.scene_age - 100))
        self.sync()

    def tick_css(self, pads, pressed):
        for p, h in self.hands.items():
            sx, sy = pads[p]["main"]
            h["x"] = max(-30.0, min(30.0, h["x"] + (sx - 128) / 100.0))
            h["y"] = max(-10.0, min(18.0, h["y"] + (sy - 128) / 100.0))
            under = self.char_at(h["x"], h["y"])
            if "A" in pressed[p]:
                if h["kind"] == 0:
                    h["kind"] = 1
                elif h["in_hand"] and under is not None:
                    h["in_hand"], h["char"] = False, under
            if "B" in pressed[p] and not h["in_hand"]:
                h["in_hand"] = True
            if h["in_hand"]:
                h["char"] = under if under is not None else b.CSS_NONE
        if "START" in pressed[0]:
            placed = [h for h in self.hands.values() if h["kind"] == 1 and not h["in_hand"]]
            if len(placed) >= 2:
                for p, h in self.hands.items():
                    ck = b.CSS_TO_CHAR_KIND[h["char"]] if h["kind"] == 1 and not h["in_hand"] else 0
                    self.w.set_player_setup(p, ck, b.PLAYER_HUMAN if h["kind"] == 1 and not h["in_hand"] else b.PLAYER_NONE)
                self.enter("scSelStage")
                sc = self.w.scenes["scSelStage"]
                self.sss_task = self.m.alloc(0x300)
                self.cursor = self.m.alloc(0x50)
                self.m.u32(sc + b.SSS_SCENE_TASK, self.sss_task)
                self.m.u32(self.sss_task + b.SSS_CURSOR, self.cursor)
                self.cx, self.cy, self.sss_state = 0.0, -21.0, 0

    def stage_pos_at(self, x, y):
        page = self.stage_pages[0]
        for i in range(len(page)):
            row, col = divmod(i, 7)
            cx, cy = -24.0 + col * 8.0, 15.0 - row * 8.0
            if abs(x - cx) <= 3.5 and abs(y - cy) <= 3.5:
                return i
        return -1

    def tick_sss(self, pads, pressed):
        if self.sss_state == b.SSS_STATE_TAKEN:
            if self.scene_age > 6:
                self.w.set_mode(stage=self.taken)
                self.enter("scMemoryChange")
            return
        sx, sy = pads[0]["main"]
        self.cx = max(-30.0, min(30.0, self.cx + 1.625 * (sx - 128) / 100.0))
        self.cy = max(-19.5, min(19.5, self.cy + 1.625 * (sy - 128) / 100.0))
        pos = self.stage_pos_at(self.cx, self.cy)
        if "A" in pressed[0] and pos >= 0:
            self.sss_state, self.taken, self.scene_age = b.SSS_STATE_TAKEN, self.stage_pages[0][pos], 0

    def sync(self):
        m = self.m
        for p, h in self.hands.items():
            m.u32(h["area"] + b.AREA_KIND, h["kind"])
            m.u32(h["area"] + b.AREA_CHARACTER, h["char"])
            m.u8(h["area"] + b.AREA_IN_HAND, 1 if h["in_hand"] else 0)
            under = self.char_at(h["x"], h["y"])
            target = (b.CSS_HAND_GRID_HOLDING if h["in_hand"] else b.CSS_HAND_GRID) if under is not None else b.CSS_HAND_NOTHING
            m.u32(h["hand"] + b.HAND_TARGET, target)
            m.f32(h["hand"] + b.HAND_X, h["x"])
            m.f32(h["hand"] + b.HAND_Y, h["y"])
        if self.scene == "scSelStage" and self.sss_task:
            t = self.sss_task
            m.f32(self.cursor + b.CURSOR_X, self.cx)
            m.f32(self.cursor + b.CURSOR_Y, self.cy)
            m.u32(t + b.SSS_STATE, self.sss_state)
            m.u32(t + b.SSS_PAGE, 0)
            pos = self.stage_pos_at(self.cx, self.cy)
            # As on the live game: hovered = position + 2 (0 = nothing); "selected" is sticky.
            m.u32(t + b.SSS_HOVERED_ITEM, pos + b.SSS_ITEM_BASE if pos >= 0 else 0)
            if pos >= 0:
                m.s32(t + b.SSS_SELECTED, pos)
            m.u32(t + b.SSS_CLOCK, self.scene_age)
            m.u32(t + b.SSS_CONTROLLER, 0xF0)
            m.u32(t + b.SSS_TAKEN_KIND, self.taken if self.sss_state == b.SSS_STATE_TAKEN else 0)


class RecipeTests(unittest.TestCase):
    def setUp(self):
        b.LEARNED_CSS.clear()
        b.LEARNED_SSS.clear()

    def test_css_pick_scans_then_learns(self):
        g = SimGame()
        a = b.css_pick_character(g, 0, b.CSS_ID["fox"])
        self.assertTrue(a.placed)
        self.assertEqual(a.character, b.CSS_ID["fox"])
        self.assertIn(b.CSS_ID["fox"], b.LEARNED_CSS)
        before = g.polls
        a2 = b.css_pick_character(g, 1, b.CSS_ID["falco"])
        self.assertEqual(a2.character, b.CSS_ID["falco"])
        self.assertLess(g.polls - before, 1500)

    def test_pick_through_netplay_input_delay(self):
        for delay in (3, 10):
            b.LEARNED_CSS.clear()
            b.LEARNED_SSS.clear()
            g = SimGame(delay=delay)
            a = b.css_pick_character(g, 0, b.CSS_ID["fox"])
            self.assertEqual(a.character, b.CSS_ID["fox"])
            a = b.css_pick_character(g, 1, b.CSS_ID["falco"])
            self.assertEqual(a.character, b.CSS_ID["falco"])
            b.css_start(g, 0)
            b.sss_pick_stage(g, b.STAGE_KIND["final_destination"], 0)
            b.wait_scene(g, [b.Scene.IN_MATCH], 300)
            self.assertEqual(b.read_match_setup(g.m).stage_kind, b.STAGE_KIND["final_destination"])

    def test_css_repick_from_wrong_token(self):
        g = SimGame()
        b.css_pick_character(g, 0, b.CSS_ID["mario"])
        a = b.css_pick_character(g, 0, b.CSS_ID["falco"])
        self.assertEqual(a.character, b.CSS_ID["falco"])

    def test_full_local_match_start(self):
        g = SimGame()
        st = b.start_match_local(g, {0: "fox", 1: "falco"}, stage="battlefield")
        self.assertTrue(st.in_match)
        s = b.read_match_setup(g.m)
        self.assertEqual(s.stage_kind, b.STAGE_KIND["battlefield"])
        self.assertEqual(s.players[0].character, b.CHAR_KIND["fox"])
        self.assertEqual(s.players[1].character, b.CHAR_KIND["falco"])
        self.assertEqual(g.writes, [])  # pure controller input

    def test_final_destination(self):
        g = SimGame()
        for p, name in ((0, "mario"), (1, "mario")):
            b.css_pick_character(g, p, b.CSS_ID[name])
        b.css_start(g, 0)
        v = b.sss_pick_stage(g, b.STAGE_KIND["final_destination"], 0)
        self.assertIsNotNone(v)
        b.wait_scene(g, [b.Scene.IN_MATCH], 300)
        self.assertEqual(b.read_match_setup(g.m).stage_kind, b.STAGE_KIND["final_destination"])

    def test_write_rules_and_seed(self):
        g = SimGame()
        b.write_rules(g, stocks=4, minutes=8)
        r = b.read_set_rule(g.m)
        self.assertEqual((r["rule"], r["stocks"], r["stock_time_minutes"]), (1, 4, 8))
        b.seed_css_record(g, {0: (b.CHAR_KIND["fox"], b.PLAYER_HUMAN)})
        self.assertEqual(g.m.read(g.w.scd + b.SCD_PLAYERS, 2), bytes([0x07, 0]))


if __name__ == "__main__":
    unittest.main()
