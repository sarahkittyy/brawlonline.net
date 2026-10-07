"""Feasibility spike: Slippi-style gameplay-only rollback for P+ (docs/gameplay-rollback-feasibility.md).

Two players boot independently, wander the menus independently, and start the same match. Can
the match simulation still be made identical, and which memory is "gameplay state"?

Subcommands (all offline, independent instances, no netplay; savestates are test fixtures only):

``prep``      Boot A and B in parallel. A goes straight to the CSS; B runs a different menu
              history (waits, picks and drops characters, backs out to the main menu, opens and
              closes Rules, comes back, picks in another order, lingers on the SSS). Both pick the
              same characters and stage and stop on their first scMelee VI field ("0"): full
              MEM1/MEM2 dump (zlib), heap table, REL module list, savestate. Then step to GO.
``simstart``  From the "0" states, step each to the first frame of the match simulation ("s",
              g_GameFrame.frameCounter reset and == 1); dump and save there.
``diff``      Compare A and B at 0 / s / GO per DOL section, REL section and heap; classify
              differing words; walk each gfMemoryPool and split differences into allocated vs free.
``play``      Experiments 2/4: two instances load A/B at s (or 0), optional sync writes (rng,
              frames, serial, scrub), then lockstep one game frame at a time with identical
              closed-loop inputs, comparing per-player state and RNGs every frame.
``replay``    Experiment 4: replay a play run's recorded inputs without per-frame pauses
              (dual core), comparing at checkpoints.
``synctest``  Experiment 3: one instance; save a region set at game frame G (read_mem), play K
              frames, restore the set (write_mem of the changed bytes), replay the K frames,
              compare. Sets are named (REGION_SETS) or JSON files (see ranges_file).

    python harness/tools/gameplay_rollback.py prep --out run/qa/gprb/ps2
    python harness/tools/gameplay_rollback.py simstart --dir run/qa/gprb/ps2
    python harness/tools/gameplay_rollback.py diff --dir run/qa/gprb/ps2
    python harness/tools/gameplay_rollback.py play --dir run/qa/gprb/ps2 --sync none,rng,rng+frames --frames 4000
    python harness/tools/gameplay_rollback.py synctest --state run/qa/gprb/ps2/As.sav --set all,harness/tools/gprb-sets/gp-v1.json

Set PPHARNESS_DOLPHIN_DIR to the frozen harness build. Needs numpy (tool-only dependency).
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import contextlib
import json
import logging
import os
import random
import struct
import sys
import time
import zlib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402  (tool-only dependency: pip install numpy)

from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessClient  # noqa: E402
from ppharness.inifile import IniFile  # noqa: E402
from ppharness.instance import DolphinInstance, InstanceConfig  # noqa: E402

log = logging.getLogger("gprb")

FIXED_RTC = 1735689600            # 2025-01-01 00:00:00 UTC, as in determinism.py
MEM1 = (0x80000000, 0x01800000)
MEM2 = (0x90000000, 0x04000000)
CHUNK = 0x00800000                # read/write_mem chunk (limit is 16 MiB)
OS_START_TIME = 0x800030D8        # __OSStartTime (u64)
LIBC_RAND_NEXT = 0x8059FF58
OBJECT_SERIAL_COUNTER = 0x8059C668  # .sdata u32, next object serial number (see sync "serial")       # rand()/srand() state: lwz/stw -0x44C8(r13) at 0x803F8C40/0x803F8C5C
NEUTRAL = {"buttons": [], "main": [128, 128], "c": [128, 128], "l": 0, "r": 0}
LAUNCHER_INI = "ID-Project+ Offline Launcher.ini"

# main.dol (Rev 1 == Rev 2 layout), from refs/brawl-decomp config/RSBE01_02/symbols.txt (min/max of
# symbol address+size per section, so the ends are approximate to a few bytes).
DOL_SECTIONS: Tuple[Tuple[str, int, int], ...] = (
    ("dol.init", 0x80004000, 0x800064E0),
    ("dol.extab", 0x800064E0, 0x80009760),
    ("dol.extabindex", 0x80009760, 0x8000C860),
    ("dol.text", 0x8000C860, 0x804064E0),
    ("dol.ctors/dtors", 0x804064E0, 0x80406800),
    ("dol.rodata", 0x80406800, 0x80420680),
    ("dol.data", 0x80420680, 0x80494880),
    ("dol.bss", 0x80494880, 0x8059C420),
    ("dol.sdata", 0x8059C420, 0x8059FF80),
    ("dol.sbss", 0x8059FF80, 0x805A1320),
    ("dol.sdata2", 0x805A1320, 0x805A5120),
    ("dol.sbss2", 0x805A5120, 0x805A5160),
    # Between the DOL and the first heap (SystemFW at 0x805B5160): the boot thread's stack.
    ("main stack", 0x805A5160, 0x805B5160),
)


# --------------------------------------------------------------------------- instances


def make_instance(name: str, *, cpu_thread: bool = False, rtc: Optional[int] = FIXED_RTC, video: str = "Null",
                  gpu_determinism: Optional[str] = None, keep: bool = False) -> DolphinInstance:
    args = []
    if rtc is not None:
        args += ["Dolphin.Core.EnableCustomRTC=True", f"Dolphin.Core.CustomRTCValue={rtc:#x}"]
    inst = DolphinInstance(name, config=InstanceConfig(cpu_thread=cpu_thread, video_backend=video, config_args=args),
                           keep=keep, connect_timeout=120)
    inst.create()
    for fname in (LAUNCHER_INI, "RSBE01.ini"):
        p = inst.user_dir / "GameSettings" / fname
        if not p.exists():
            continue
        ini = IniFile.load(p)
        upd: Dict[str, Any] = {}
        if fname == "RSBE01.ini":
            upd["CPUThread"] = cpu_thread   # the template forces CPUThread = True here
        if gpu_determinism is not None:
            upd["GPUDeterminismMode"] = gpu_determinism
        if upd:
            ini.update({"Core": upd})
            ini.save(p)
    return inst


@contextlib.contextmanager
def instances(specs: Sequence[Tuple[str, Dict[str, Any]]], keep: bool = False):
    """Launch several instances in parallel; always kill and clean them up."""
    insts = [make_instance(n, keep=keep, **kw) for n, kw in specs]
    try:
        with cf.ThreadPoolExecutor(len(insts)) as pool:
            list(pool.map(lambda i: (i.launch(), i.connect()), insts))
        for i in insts:
            i.client.wait_state("running", timeout=120)
            for port in (0, 1):
                i.client.pad_set(port)
        yield insts
    finally:
        for i in insts:
            with contextlib.suppress(Exception):
                if i.is_running():
                    i.stop()
            with contextlib.suppress(Exception):
                if i.is_running():
                    i.kill()
            with contextlib.suppress(Exception):
                # Instance dirs hold a 2 GB SD image: remove them on failure too unless asked to keep.
                i.cleanup(not keep)


def par(fns: Sequence[Callable[[], Any]]) -> List[Any]:
    with cf.ThreadPoolExecutor(len(fns)) as pool:
        return [f.result() for f in [pool.submit(fn) for fn in fns]]


# --------------------------------------------------------------------------- memory helpers


def read_big(c: HarnessClient, addr: int, n: int) -> bytes:
    out = bytearray()
    for a in range(addr, addr + n, CHUNK):
        out += c.read_mem(a, min(CHUNK, addr + n - a))
    return bytes(out)


def dump_all(c: HarnessClient) -> Dict[int, bytes]:
    return {MEM1[0]: read_big(c, *MEM1), MEM2[0]: read_big(c, *MEM2)}


def save_dump(d: Mapping[int, bytes], path: Path) -> None:
    with open(path, "wb") as f:
        for base, data in d.items():
            z = zlib.compress(data, 1)
            f.write(struct.pack(">III", base, len(data), len(z)))
            f.write(z)


def load_dump(path: Path) -> Dict[int, bytes]:
    out = {}
    raw = path.read_bytes()
    i = 0
    while i < len(raw):
        base, n, zn = struct.unpack_from(">III", raw, i)
        i += 12
        out[base] = zlib.decompress(raw[i:i + zn])
        assert len(out[base]) == n
        i += zn
    return out


def dview(d: Mapping[int, bytes], addr: int, n: int) -> bytes:
    for base, data in d.items():
        if base <= addr and addr + n <= base + len(data):
            return data[addr - base:addr - base + n]
    raise ValueError(f"{addr:#x}+{n:#x} not in dump")


def diff_runs(a: bytes, b: bytes, gran: int = 32, merge_gap: int = 256) -> List[Tuple[int, int]]:
    """(offset, length) runs where a and b differ, at ``gran`` granularity, merged across gaps."""
    assert len(a) == len(b)
    if a == b:
        return []
    n = len(a)
    pad = (-n) % gran
    x = np.frombuffer(a + b"\0" * pad, dtype=np.uint8).reshape(-1, gran)
    y = np.frombuffer(b + b"\0" * pad, dtype=np.uint8).reshape(-1, gran)
    idx = np.nonzero((x != y).any(axis=1))[0]
    runs: List[List[int]] = []
    for i in idx.tolist():
        o, e = i * gran, min(n, i * gran + gran)
        if runs and o - runs[-1][1] <= merge_gap:
            runs[-1][1] = e
        else:
            runs.append([o, e])
    return [(s_, e - s_) for s_, e in runs]


def count_diff_bytes(a: bytes, b: bytes) -> Tuple[int, int]:
    """(differing bytes, differing aligned u32 words)."""
    if a == b:
        return 0, 0
    pad = (-len(a)) % 4
    x = np.frombuffer(a + b"\0" * pad, dtype=np.uint8)
    y = np.frombuffer(b + b"\0" * pad, dtype=np.uint8)
    d = x != y
    return int(d.sum()), int(d.reshape(-1, 4).any(axis=1).sum())


# --------------------------------------------------------------------------- memory map


def read_modules(read_mem: Callable[[int, int], bytes]) -> List[Dict[str, Any]]:
    """Loaded REL modules from the OS module list (0x800030C8 head). Linked section entries hold
    absolute addresses (bit 0 = executable). Section 1 is .text in Brawl's RELs; the last non-empty
    one is .bss (header +0x33 bssSection, +0x20 bssSize)."""
    m = B.Mem(read_mem)
    out = []
    mod = m.u32(0x800030C8)
    seen = set()
    while mod and mod not in seen and len(out) < 64:
        seen.add(mod)
        hdr = m.read(mod, 0x40)
        mid, nxt, _prv, ns, sio = struct.unpack_from(">5I", hdr, 0)
        bss_size = struct.unpack_from(">I", hdr, 0x20)[0]
        bss_sec = hdr[0x33]
        secs = []
        for i in range(ns):
            off, size = struct.unpack(">II", m.read(sio + 8 * i, 8))
            if size == 0:
                continue
            exe = bool(off & 1)
            addr = off & ~1
            kind = "text" if exe else ("bss" if i == bss_sec else f"data{i}")
            secs.append({"index": i, "addr": addr, "size": size, "kind": kind})
        out.append({"id": mid, "header": mod, "bss_size": bss_size, "sections": secs,
                    "name": MODULE_NAMES.get(mid, f"rel{mid}")})
        mod = nxt
    return out


def _module_names() -> Dict[int, str]:
    """Module ids -> names, from BrawlHeaders' modules.h when the refs checkout is there."""
    names: Dict[int, str] = {}
    p = Path(__file__).resolve().parents[2] / "refs/BrawlHeaders-sammi/Brawl/Include/modules.h"
    with contextlib.suppress(OSError):
        import re
        for nm, num in re.findall(r"([A-Z_0-9]+)\s*=\s*(\d+)", p.read_text()):
            names[int(num)] = nm.lower()
    return names


MODULE_NAMES: Dict[int, str] = _module_names()


@dataclass
class Region:
    start: int
    end: int
    name: str
    kind: str   # heap / dol / rel / low / gap

    @property
    def size(self) -> int:
        return self.end - self.start


def memory_map(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Region]:
    """Named, non-overlapping regions covering MEM1 and MEM2. REL sections are carved out of the
    heap they live in (OverlayCommon, OverlayStage, OverlayFighterN, Syringe...)."""
    regs: List[Region] = [Region(0x80000000, 0x80004000, "lowmem (OS globals, Gecko handler)", "low")]
    regs += [Region(s, e, n, "dol") for n, s, e in DOL_SECTIONS]
    for h in heaps:
        regs.append(Region(h["start"], h["end"], h["name"], "heap"))
    out: List[Region] = []
    # REL sections take precedence over heaps.
    rel = []
    for mod in modules:
        for s in mod["sections"]:
            rel.append(Region(s["addr"], s["addr"] + s["size"], f"{mod['name']}.{s['kind']}", "rel"))
    rel.sort(key=lambda r: r.start)
    for r in sorted(regs, key=lambda r: r.start):
        pieces = [(r.start, r.end)]
        for x in rel:
            nxt = []
            for a, e in pieces:
                if x.end <= a or x.start >= e:
                    nxt.append((a, e))
                    continue
                if a < x.start:
                    nxt.append((a, x.start))
                if x.end < e:
                    nxt.append((x.end, e))
            pieces = nxt
        for a, e in pieces:
            out.append(Region(a, e, r.name if (a, e) == (r.start, r.end) else r.name + " (rest)", r.kind))
    out += rel
    out.sort(key=lambda r: r.start)
    # Fill gaps.
    filled: List[Region] = []
    for base, size in (MEM1, MEM2):
        cur = base
        for r in [r for r in out if base <= r.start < base + size]:
            if r.start > cur:
                filled.append(Region(cur, r.start, f"gap {cur:#x}", "gap"))
            if r.start < cur:      # overlap (shouldn't happen); clip
                if r.end <= cur:
                    continue
                r = Region(cur, r.end, r.name, r.kind)
            filled.append(r)
            cur = r.end
        if cur < base + size:
            filled.append(Region(cur, base + size, f"gap {cur:#x}", "gap"))
    return filled


def heap_dicts(c: HarnessClient) -> List[Dict[str, Any]]:
    return [{"id": h.heap_id, "name": h.name, "start": h.start, "end": h.end, "arena": h.arena}
            for h in B.read_heap_table(c.read_mem)]


# --------------------------------------------------------------------------- observables


def small_state(c: HarnessClient) -> Dict[str, Any]:
    m = B.Mem(c.read_mem)
    st = B.read_match_state(c.read_mem)
    rng = B._try(lambda: (m.u32(B.MTRAND_DEFAULT + 4), m.u32(B.MTRAND_OTHER + 4), m.u32(LIBC_RAND_NEXT)))
    return {
        "scene": st.scene.name,
        "rng": list(rng) if rng else None,
        "frames": st.frame_counters,
        "elapsed": st.frames_elapsed, "remaining": st.remaining_frames, "started": st.started,
        "game_set": st.game_set,
        "players": [[p.port, p.ft_kind, p.damage, p.stocks, p.x, p.y, p.action, p.motion_kind, p.anim_frame, p.facing]
                    for p in st.players],
    }


PLAYER_FIELDS = ("port", "ft_kind", "damage", "stocks", "x", "y", "action", "motion", "anim_frame", "facing")


def state_diff(a: Mapping[str, Any], b: Mapping[str, Any], keys: Sequence[str] = ("rng", "players", "elapsed", "remaining", "game_set")) -> Dict[str, Any]:
    out = {}
    for k in keys:
        if a.get(k) != b.get(k):
            out[k] = [a.get(k), b.get(k)]
    return out


def gameplay_labelled(c: HarnessClient) -> List[Tuple[int, int, str]]:
    return B.gameplay_ranges(c.read_mem)


# --------------------------------------------------------------------------- stepping


def pause_on_field(c: HarnessClient) -> None:
    c.pause()
    c.frame_advance(1)   # land on a VI field boundary


def advance_polls(c: HarnessClient, n: int, max_fields: int = -1) -> int:
    """While paused: frame_advance VI fields until input_polls has grown by ``n``."""
    target = c.status().input_polls + n
    if max_fields < 0:
        max_fields = 2 * n + 300
    fields = 0
    while True:
        p = c.status().input_polls
        if p >= target:
            return p
        c.frame_advance(1)
        fields += 1
        if max_fields and fields > max_fields:
            raise RuntimeError(f"polls stuck at {p} (target {target})")


# --------------------------------------------------------------------------- input stream


def input_stream(seed: Any, frames: int, kinds: str = "full") -> Dict[int, List[Dict[str, Any]]]:
    """Open-loop scripted input per port: seeded random macros (walk, dash, jumps, jab, tilts,
    smashes, aerials, specials incl. side-B/down-B/up-B, shield, crouch, idle), biased to come
    back toward the middle every few seconds so the fighters do not just walk off. Never Start,
    never the D-pad, never L+R. Begins and ends with 30 neutral frames."""
    out: Dict[int, List[Dict[str, Any]]] = {}
    for port in (0, 1):
        rng = random.Random(f"{seed}:{port}")
        seq: List[Dict[str, Any]] = [dict(NEUTRAL, hold=30)]
        t = 30
        toward = 1 if port == 0 else -1
        while t < frames - 30:
            if rng.random() < 0.08:
                toward = -toward
            mac = _macro(rng, toward)
            for f in mac:
                d = dict(NEUTRAL)
                d.update({k: v for k, v in f.items() if k != "hold"})
                d["buttons"] = sorted(d["buttons"])
                seq.append(dict(d, hold=int(f.get("hold", 1))))
                t += int(f.get("hold", 1))
        # trim to frames-30, then 30 neutral
        total, trimmed = 0, []
        for f in seq:
            if total >= frames - 30:
                break
            h = min(f["hold"], frames - 30 - total)
            trimmed.append(dict(f, hold=h))
            total += h
        trimmed.append(dict(NEUTRAL, hold=30))
        out[port] = trimmed
    return out


_FLOWS_MACRO = F._macro


def _macro(rng: random.Random, toward: int) -> List[Dict[str, Any]]:
    """flows._macro plus side-B, down-B (Peach's turnip pull, G&W's Oil Panic), up-B, d-air,
    grabs and dash-backs."""
    t = 255 if toward > 0 else 1
    away = 1 if toward > 0 else 255
    r = rng.random()
    if r < 0.40:
        return _FLOWS_MACRO(rng, toward)
    if r < 0.48:
        return [{"main": [t, 128], "buttons": ["B"], "hold": 2}, {"hold": 30}]       # side-B
    if r < 0.56:
        return [{"main": [128, 1], "buttons": ["B"], "hold": 2}, {"hold": 30}]       # down-B (turnip, etc.)
    if r < 0.60:
        return [{"main": [128, 255], "buttons": ["B"], "hold": 2}, {"main": [t, 200], "hold": 40}]  # up-B
    if r < 0.66:
        return [{"main": [128, 255], "hold": 2}, {"hold": 2}, {"c": [128, 1], "hold": 3}, {"hold": 20}]  # jump + d-air
    if r < 0.72:
        return [{"r": 255, "buttons": ["R"], "hold": 2}, {"r": 255, "buttons": ["R", "A"], "hold": 2}, {"hold": 30}]  # grab
    if r < 0.80:
        return [{"main": [t, 128], "hold": rng.randint(10, 30)}]
    if r < 0.86:
        return [{"main": [away, 128], "hold": rng.randint(3, 8)}, {"main": [t, 128], "hold": 3}, {"hold": 6}]
    if r < 0.92:
        return [{"buttons": ["X"], "hold": 2}, {"hold": 3}, {"main": [t, 128], "c": [t, 128], "hold": 2}, {"hold": 24}]
    return [{"hold": rng.randint(4, 20)}]


def schedule_inputs(c: HarnessClient, stream: Mapping[int, Sequence[Mapping[str, Any]]], start: Any) -> Dict[int, int]:
    out = {}
    for port, frames in stream.items():
        w = c.pad_script(int(port), list(frames), start=start)
        out[int(port)] = w.starts_at
    return out


# --------------------------------------------------------------------------- menu histories


def css_drop_token(c: HarnessClient, port: int) -> None:
    """Pick the placed token back up: B for exactly one frame (never hold B with a token in hand)."""
    B.tap(c, port, ["B"], hold=1, release=20)


def history_a(c: HarnessClient, chars: Sequence[str], stage: str, notes: List[str]) -> None:
    B.wait_scene(c, [B.Scene.CSS], 60 * 90)
    B.wait_css_ready(c, [0, 1])
    notes.append(f"CSS at poll {c.status().input_polls}")
    s1 = F.Seat(c, 0)
    B.css_pick_character(c, 0, B.CSS_ID[chars[0]])
    B.css_pick_character(c, 1, B.CSS_ID[chars[1]])
    notes.append(f"picked at poll {c.status().input_polls}")
    F.start_and_pick_stage(s1, stage)


def history_b(c: HarnessClient, chars: Sequence[str], stage: str, notes: List[str]) -> None:
    B.wait_scene(c, [B.Scene.CSS], 60 * 90)
    B.wait_css_ready(c, [0, 1])
    notes.append(f"CSS at poll {c.status().input_polls}")
    B.step(c, 137)                                   # idle a different number of frames
    # Hover and pick other characters, then drop the tokens again.
    B.css_pick_character(c, 0, B.CSS_ID["mario"])
    B.css_pick_character(c, 1, B.CSS_ID["ike"])
    notes.append(f"picked Mario/Ike at poll {c.status().input_polls}")
    css_drop_token(c, 0)
    css_drop_token(c, 1)
    B.css_pick_character(c, 0, B.CSS_ID["kirby"])
    css_drop_token(c, 0)
    a = B.read_css_area(c.read_mem, 0)
    notes.append(f"dropped tokens: P1 in_hand={a.in_hand if a else None}")
    # Back out to the main menu (hold B with the token in hand), open and close Rules.
    B.neutral(c, 0)
    c.pad_script(0, [{"buttons": ["B"], "hold": 90}, {"hold": 5}])
    B.wait_scene(c, [B.Scene.MAIN_MENU], 600, every=4)
    notes.append(f"main menu at poll {c.status().input_polls}")
    B.step(c, 60)
    c.pad_script(0, [dict(NEUTRAL, main=[255, 128], hold=4), dict(NEUTRAL, hold=20)])
    B.step(c, 30)
    B.tap(c, 0, ["A"], 3, 90)                        # Rules
    B.tap(c, 0, ["B"], 3, 40)                        # close Rules
    c.pad_script(0, [dict(NEUTRAL, main=[1, 128], hold=4), dict(NEUTRAL, hold=20)])
    B.step(c, 41)
    B.tap(c, 0, ["A"], 3, 10)                        # FIGHT!
    B.wait_scene(c, [B.Scene.CSS], 1200, every=4)
    B.wait_css_ready(c, [0, 1])
    notes.append(f"back on CSS at poll {c.status().input_polls}")
    B.step(c, 53)
    # Same characters, other order.
    for port in (1, 0):
        a = B.read_css_area(c.read_mem, port)
        if a is not None and a.placed:
            css_drop_token(c, port)
    B.css_pick_character(c, 1, B.CSS_ID[chars[1]])
    B.css_pick_character(c, 0, B.CSS_ID[chars[0]])
    notes.append(f"picked at poll {c.status().input_polls}")
    B.css_start(c, 0)
    B.step(c, 77)                                    # linger on the SSS
    B.sss_pick_stage(c, B.STAGE_KIND[stage], 0)


def to_match_frame0(c: HarnessClient, max_fields: int = 3000) -> Dict[str, Any]:
    """After the stage is taken: pause and step VI fields until the current scene is scMelee."""
    pause_on_field(c)
    n = 0
    while B.read_scene(c.read_mem).scene is not B.Scene.IN_MATCH:
        c.frame_advance(1)
        n += 1
        if n > max_fields:
            raise RuntimeError("scMelee never started")
    st = c.status()
    return {"vi": st.frame, "poll": st.input_polls, "fields_from_sss": n}


def load_fixture(c: HarnessClient, path: Path, timeout: float = 120.0) -> None:
    """Load a savestate (test fixture only) and leave the instance paused on it, neutral pads.
    A load issued while the P+ launcher DOL is still running is silently ignored, so wait for
    RSBE01 first and verify the load landed in a match."""
    deadline = time.monotonic() + timeout
    while c.status().game_id != "RSBE01":
        if time.monotonic() > deadline:
            raise RuntimeError("game never booted")
        time.sleep(0.2)
    for attempt in range(5):
        c.pause()
        c.load_state(str(Path(path).resolve()))
        # Never step here: the fixture must start exactly on the saved field.
        for _ in range(100):
            if B.read_scene(c.read_mem).scene is B.Scene.IN_MATCH:
                for port in (0, 1):
                    c.pad_set(port)
                return
            time.sleep(0.1)
        c.resume()
        time.sleep(1.0)
    raise RuntimeError(f"load_state {path} did not take effect")


def to_sim_start(c: HarnessClient, max_fields: int = 600) -> Dict[str, Any]:
    """Paused anywhere in scMelee before GO: step VI fields until g_GameFrame.frameCounter has been
    reset for the match (frameCounter < persistentFrameCounter) and reads 1, i.e. the first frame
    of the match simulation. The load before it takes a menu-history-dependent number of fields
    (A and B differed by 5), so this, not the first scMelee field, is where the two line up."""
    n = 0
    while True:
        fc = B.read_frame_counters(c.read_mem)
        gf, pf = fc.get("game_frame"), fc.get("persistent")
        if gf is not None and pf is not None and pf > gf and gf >= 1:
            if gf != 1:
                raise RuntimeError(f"passed the simulation start (frameCounter {gf})")
            st = c.status()
            return {"vi": st.frame, "poll": st.input_polls, "fields": n, "frames": fc}
        c.frame_advance(1)
        n += 1
        if n > max_fields:
            raise RuntimeError("simulation never started")


def cmd_simstart(args: argparse.Namespace) -> int:
    d = Path(args.dir)
    prep = json.loads((d / "prep.json").read_text())
    specs = [(f"gprb-sim{lbl}", dict(cpu_thread=args.cpu == "dc", video="Null")) for lbl in ("A", "B")]
    with instances(specs) as insts:
        def one(lbl: str, c: HarnessClient) -> Dict[str, Any]:
            load_fixture(c, d / f"{lbl}0.sav")
            r = to_sim_start(c)
            r["heaps"] = heap_dicts(c)
            r["small"] = small_state(c)
            save_dump(dump_all(c), d / f"{lbl}s.mem")
            c.save_state(str((d / f"{lbl}s.sav").resolve()))
            return r
        res = par([lambda lbl=lbl, c=i.client: one(lbl, c) for lbl, i in zip(("A", "B"), insts)])
    for lbl, r in zip(("A", "B"), res):
        prep["runs"][lbl]["heaps_s"] = r.pop("heaps")
        prep["runs"][lbl]["simstart"] = r
        print(lbl, {k: v for k, v in r.items() if k != "small"}, r["small"]["rng"])
    (d / "prep.json").write_text(json.dumps(prep, indent=1, default=str))
    return 0


def trace_to_go(c: HarnessClient, max_fields: int = 600) -> List[Dict[str, Any]]:
    rows = []
    p0 = c.status().input_polls
    for _ in range(max_fields):
        c.frame_advance(1)
        st = c.status()
        ms = B.read_match_state(c.read_mem)
        rows.append({"vi": st.frame, "dpoll": st.input_polls - p0, "frames": ms.frame_counters,
                     "started": ms.started, "elapsed": ms.frames_elapsed})
        if (ms.frames_elapsed or 0) >= 2:
            break
    return rows


# --------------------------------------------------------------------------- prep


def cmd_prep(args: argparse.Namespace) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    chars = args.chars.split(",")
    rtc_b = int(args.rtc_b, 0) if args.rtc_b else FIXED_RTC
    specs = [("gprb-A", dict(cpu_thread=args.cpu == "dc", rtc=FIXED_RTC, video=args.video)),
             ("gprb-B", dict(cpu_thread=args.cpu == "dc", rtc=rtc_b, video=args.video))]
    report: Dict[str, Any] = {"chars": chars, "stage": args.stage, "cpu": args.cpu, "rtc_b": rtc_b, "runs": {}}
    with instances(specs) as insts:
        ca, cb = insts[0].client, insts[1].client
        # Boots that straddle a host second read different RTCs (determinism doc, cause 1).
        time.sleep(2)
        st = [B.Mem(c.read_mem).read(OS_START_TIME, 8).hex() for c in (ca, cb)]
        report["os_start_time"] = st
        if rtc_b == FIXED_RTC and st[0] != st[1]:
            log.warning("__OSStartTime differs between A and B: %s", st)

        def run(label: str, c: HarnessClient, hist: Callable[..., None], inst: DolphinInstance) -> Dict[str, Any]:
            notes: List[str] = []
            t0 = time.monotonic()
            hist(c, chars, args.stage, notes)
            f0 = to_match_frame0(c)
            r: Dict[str, Any] = {"notes": notes, "frame0": f0, "wall_s": round(time.monotonic() - t0, 1)}
            r["small"] = small_state(c)
            r["heaps"] = heap_dicts(c)
            r["modules"] = read_modules(c.read_mem)
            r["setup"] = asdict(B.read_match_setup(c.read_mem)) if B.read_match_setup(c.read_mem) else None
            save_dump(dump_all(c), out / f"{label}0.mem")
            c.save_state(str((out / f"{label}0.sav").resolve()))
            r["trace"] = trace_to_go(c)
            r["small_go"] = small_state(c)
            r["heaps_go"] = heap_dicts(c)
            save_dump(dump_all(c), out / f"{label}go.mem")
            c.resume()
            return r

        res = par([lambda: run("A", ca, history_a, insts[0]), lambda: run("B", cb, history_b, insts[1])])
        report["runs"] = {"A": res[0], "B": res[1]}
    (out / "prep.json").write_text(json.dumps(report, indent=1, default=str))
    for k, r in report["runs"].items():
        go = next((i for i, x in enumerate(r["trace"]) if x["started"]), None)
        print(k, r["notes"], r["frame0"], "fields to GO:", go, "rng:", r["small"]["rng"])
    return 0


# --------------------------------------------------------------------------- diff (experiment 1)


def _is_ptr(v: int) -> bool:
    return 0x80000000 <= v < 0x81800000 or 0x90000000 <= v < 0x94000000


def _is_floatish(v: int) -> bool:
    e = (v >> 23) & 0xFF
    return v != 0 and 0x60 <= e <= 0x9E and not _is_ptr(v)


def classify_words(a: bytes, b: bytes) -> Dict[str, Any]:
    """Differing aligned u32 words by kind: both pointers, pointer vs other, floats, small ints."""
    pad = (-len(a)) % 4
    x = np.frombuffer(a + b"\0" * pad, dtype=">u4").astype(np.uint64)
    y = np.frombuffer(b + b"\0" * pad, dtype=">u4").astype(np.uint64)
    idx = np.nonzero(x != y)[0]
    out = {"ptr_ptr": 0, "ptr_other": 0, "float": 0, "small_int": 0, "zero_vs_x": 0, "other": 0}
    deltas: Dict[int, int] = {}
    for i in idx.tolist():
        va, vb = int(x[i]), int(y[i])
        pa, pb = _is_ptr(va), _is_ptr(vb)
        if pa and pb:
            out["ptr_ptr"] += 1
            d = vb - va
            deltas[d] = deltas.get(d, 0) + 1
        elif pa or pb:
            out["ptr_other"] += 1
        elif va == 0 or vb == 0:
            out["zero_vs_x"] += 1
        elif _is_floatish(va) and _is_floatish(vb):
            out["float"] += 1
        elif va < 0x10000 and vb < 0x10000:
            out["small_int"] += 1
        else:
            out["other"] += 1
    top = sorted(deltas.items(), key=lambda kv: -kv[1])[:3]
    out["top_ptr_deltas"] = [(f"{d:+#x}", n) for d, n in top]
    return out


GAMEPLAY_HEAP_NAMES = ("System", "Effect", "Fighter1Instance", "Fighter2Instance", "Fighter3Instance",
                       "Fighter4Instance", "FighterTechqniq", "StageInstance", "ItemInstance", "Physics",
                       "InfoInstance", "GameGlobal", "GlobalMode", "WiiPad")


def heap_blocks(d: Mapping[int, bytes], pool: int, end: int) -> Tuple[Optional[List[Tuple[int, int, bool]]], str]:
    """Walk a gfMemoryPool's blocks (reverse engineered from the dumps): the pool object sits at the
    heap start, +0x20 points at a sentinel block of size 0; every block has a 0x20-byte header
    {+0 pool, +4 block size including the header, +8 next free block (free list from the
    sentinel)}; blocks are contiguous up to the heap end; allocations are taken from the top.
    Returns [(block addr, size, is_free)]."""
    def u(a: int) -> int:
        return struct.unpack(">I", dview(d, a, 4))[0]
    sent = u(pool + 0x20)
    if not pool < sent < end:
        return None, "no sentinel"
    free = set()
    f = u(sent + 8)
    while f and pool <= f < end and f not in free and len(free) < 200000:
        free.add(f)
        f = u(f + 8)
    a = sent + 0x20 if u(sent + 4) == 0 else sent
    out: List[Tuple[int, int, bool]] = []
    while a < end:
        if u(a) != pool:
            return out, f"bad header at {a:#x}"
        sz = u(a + 4)
        if sz == 0 or a + sz > end:
            return out, f"bad size at {a:#x}"
        out.append((a, sz, a in free))
        a += sz
    return out, "ok"


def heap_alloc_report(da: Mapping[int, bytes], db: Mapping[int, bytes], heaps: Sequence[Mapping[str, Any]]) -> List[Dict[str, Any]]:
    """Per heap: same block layout on A and B? bytes used, and differing bytes inside allocated
    blocks versus inside free blocks (garbage left over from earlier scenes)."""
    out = []
    for h in heaps:
        ba, sa = heap_blocks(da, h["start"], h["end"])
        bb, sb = heap_blocks(db, h["start"], h["end"])
        row: Dict[str, Any] = {"heap": h["name"], "start": f"{h['start']:#010x}", "size": h["end"] - h["start"],
                               "walk": [sa, sb]}
        if ba is None:
            out.append(row)
            continue
        row["blocks"] = [len(ba), len(bb) if bb is not None else None]
        row["same_layout"] = ba == bb
        row["used"] = sum(sz for _, sz, fr in ba if not fr)
        dal = dfr = 0
        for a, sz, fr in ba:
            x, y = dview(da, a, sz), dview(db, a, sz)
            if x != y:
                nb, _ = count_diff_bytes(x, y)
                if fr:
                    dfr += nb
                else:
                    dal += nb
        row["diff_alloc"], row["diff_free"] = dal, dfr
        if not row["same_layout"] and bb is not None:
            sa_, sb_ = set(ba), set(bb)
            row["only_a"] = [(f"{a:#x}", sz, fr) for a, sz, fr in sorted(sa_ - sb_)][:8]
            row["only_b"] = [(f"{a:#x}", sz, fr) for a, sz, fr in sorted(sb_ - sa_)][:8]
        out.append(row)
    return out


def compare_dumps(da: Mapping[int, bytes], db: Mapping[int, bytes], regions: Sequence[Region],
                  samples: int = 4) -> List[Dict[str, Any]]:
    rows = []
    for r in regions:
        a, b = dview(da, r.start, r.size), dview(db, r.start, r.size)
        row: Dict[str, Any] = {"region": r.name, "kind": r.kind, "start": f"{r.start:#010x}", "size": r.size}
        if a == b:
            row.update(diff_bytes=0, diff_words=0)
        else:
            nb, nw = count_diff_bytes(a, b)
            nz = int(np.count_nonzero(np.frombuffer(a, dtype=np.uint8)))
            runs = diff_runs(a, b, gran=4, merge_gap=16)
            row.update(diff_bytes=nb, diff_words=nw, nonzero_bytes_a=nz, runs=len(runs),
                       first_diff=f"{r.start + runs[0][0]:#010x}", last_diff=f"{r.start + runs[-1][0] + runs[-1][1]:#010x}",
                       words=classify_words(a, b),
                       samples=[{"addr": f"{r.start + o:#010x}", "a": a[o:o + min(ln, 32)].hex(), "b": b[o:o + min(ln, 32)].hex()}
                                for o, ln in runs[:samples]])
        rows.append(row)
    return rows


def known_values(d: Mapping[int, bytes]) -> Dict[str, Any]:
    def u32(a: int) -> int:
        return struct.unpack(">I", dview(d, a, 4))[0]
    out = {"mtRand_seed": u32(B.MTRAND_DEFAULT + 4), "mtRandOther_seed": u32(B.MTRAND_OTHER + 4),
           "libc_rand_next": u32(LIBC_RAND_NEXT), "OSStartTime": dview(d, OS_START_TIME, 8).hex(),
           "g_GameFrame": dview(d, B.GAME_FRAME, 0x18).hex(),
           "mtPrngLogManager": dview(d, 0x804977B4, 0xC).hex()}
    app = u32(B.APPLICATION_PTR)
    if _is_ptr(app):
        out["gfApplication_frame"] = u32(app + 0x100)
    return out


def cmd_diff(args: argparse.Namespace) -> int:
    d = Path(args.dir)
    prep = json.loads((d / "prep.json").read_text())
    out: Dict[str, Any] = {}
    for tag in args.points.split(","):
        ra, rb = prep["runs"]["A"], prep["runs"]["B"]
        key = {"0": "heaps", "go": "heaps_go", "s": "heaps_s"}[tag]
        if key not in ra:
            print(f"== point {tag}: not recorded")
            continue
        ha, hb = ra[key], rb[key]
        same_heaps = [(h["name"], h["start"], h["end"]) for h in ha] == [(h["name"], h["start"], h["end"]) for h in hb]
        mods_a = [(m_["id"], m_["header"]) for m_ in ra["modules"]]
        mods_b = [(m_["id"], m_["header"]) for m_ in rb["modules"]]
        regions = memory_map(ha, ra["modules"])
        da, db = load_dump(d / f"A{tag}.mem"), load_dump(d / f"B{tag}.mem")
        rows = compare_dumps(da, db, regions)
        res = {"heap_tables_equal": same_heaps, "modules_equal": mods_a == mods_b,
               "heap_diffs": [] if same_heaps else [(x, y) for x, y in zip(ha, hb) if (x["name"], x["start"], x["end"]) != (y["name"], y["start"], y["end"])],
               "modules_a": mods_a, "modules_b": mods_b,
               "known_a": known_values(da), "known_b": known_values(db), "regions": rows}
        tot = {k: sum(r["size"] for r in rows if r["kind"] == k) for k in ("heap", "dol", "rel", "low", "gap")}
        dif = {k: sum(r["diff_bytes"] for r in rows if r["kind"] == k) for k in tot}
        res["totals"] = {"bytes": tot, "diff_bytes": dif}
        res["heap_alloc"] = heap_alloc_report(da, db, ha)
        out[tag] = res
        print(f"== point {tag}: heap tables equal {same_heaps}, modules equal {mods_a == mods_b}")
        print("   known A:", res["known_a"])
        print("   known B:", res["known_b"])
        print(f"   {'region':34s} {'start':>10s} {'size':>9s} {'diffB':>9s} {'%':>6s}  words(ptr/ptr-x/float/int/zero/other)  top ptr deltas")
        for r in rows:
            if r["diff_bytes"] == 0 and not (r["kind"] == "heap" and r["region"] in GAMEPLAY_HEAP_NAMES):
                continue
            w = r.get("words", {})
            ws = "/".join(str(w.get(k, 0)) for k in ("ptr_ptr", "ptr_other", "float", "small_int", "zero_vs_x", "other"))
            print(f"   {r['region'][:34]:34s} {r['start']:>10s} {r['size']:9d} {r['diff_bytes']:9d} {100 * r['diff_bytes'] / max(1, r['size']):6.2f}  {ws:30s} {w.get('top_ptr_deltas', '')}")
        print(f"   {'heap':22s} {'size':>9s} {'used%':>6s} {'blocks A/B':>11s} layout  {'diff alloc':>10s} {'diff free':>10s}")
        for r in res["heap_alloc"]:
            if "blocks" not in r:
                print(f"   {r['heap']:22s} walk failed {r['walk']}")
                continue
            if not (r["diff_alloc"] or r["diff_free"] or not r["same_layout"] or r["heap"] in GAMEPLAY_HEAP_NAMES):
                continue
            print(f"   {r['heap']:22s} {r['size']:9d} {100 * r['used'] / r['size']:6.1f} {r['blocks'][0]:5d}/{r['blocks'][1]!s:5s} "
                  f"{'same' if r['same_layout'] else 'DIFF':6s} {r['diff_alloc']:10d} {r['diff_free']:10d}"
                  + ("" if r["same_layout"] else f"  only A {r.get('only_a')} only B {r.get('only_b')}"))
    (d / "diff.json").write_text(json.dumps(out, indent=1, default=str))
    return 0


# --------------------------------------------------------------------------- play (experiments 2 and 4)


PROBE_HEAPS = ("System", "Effect", "Fighter1Instance", "Fighter2Instance", "FighterTechqniq", "StageInstance",
               "ItemInstance", "Physics", "InfoInstance", "GameGlobal", "GlobalMode", "WiiPad", "StageResoruce",
               "Fighter1Resoruce", "Fighter2Resoruce", "Fighter1Resoruce2", "Fighter2Resoruce2", "IteamResource",
               "PokemonResource", "AssistFigureResource")


# Heaps created for the match (empty at the first scMelee field) plus the match's resource heaps.
SCRUB_HEAPS = ("Physics", "ItemInstance", "StageInstance", "WeaponInstance", "EnemyInstance", "Fighter1Instance",
               "Fighter2Instance", "Fighter3Instance", "Fighter4Instance", "FighterTechqniq", "InfoInstance",
               "StageResoruce", "Fighter1Resoruce", "Fighter2Resoruce", "Fighter3Resoruce", "Fighter4Resoruce",
               "Fighter1Resoruce2", "Fighter2Resoruce2", "Fighter3Resoruce2", "Fighter4Resoruce2",
               "AssistFigureResource", "PokemonResource", "ItemExtraResource")


def scrub_writes(ca: HarnessClient, cb: HarnessClient) -> Dict[str, Any]:
    da, db = dump_all(ca), dump_all(cb)
    writes: List[Tuple[int, bytes]] = []
    used = []
    for h in heap_dicts(ca):
        if h["name"] not in SCRUB_HEAPS:
            continue
        ba, _ = heap_blocks(da, h["start"], h["end"])
        bb, _ = heap_blocks(db, h["start"], h["end"])
        if not ba or ba != bb:
            continue
        n0 = len(writes)
        for a, sz, fr in ba:
            x, y = dview(da, a + 0x20, sz - 0x20), dview(db, a + 0x20, sz - 0x20)
            if x == y:
                continue
            if fr:
                for o, ln in diff_runs(x, y, gran=32, merge_gap=64):
                    writes.append((a + 0x20 + o, x[o:o + ln]))
            else:
                xa = np.frombuffer(x[:len(x) // 4 * 4], dtype=">u4")
                ya = np.frombuffer(y[:len(y) // 4 * 4], dtype=">u4")
                idx = np.nonzero((xa == 0xCCCCCCCC) & (ya != xa))[0]
                for i in idx.tolist():
                    writes.append((a + 0x20 + 4 * i, bytes([0xCC] * 4)))
        if len(writes) > n0:
            used.append(h["name"])
    return {"writes": writes, "bytes": sum(len(d) for _, d in writes), "heaps": used}


def sync_writes(c_ref: HarnessClient, what: Sequence[str]) -> List[Tuple[int, bytes, str]]:
    """The bytes to write into every instance at match frame 0 (taken from the reference)."""
    m = B.Mem(c_ref.read_mem)
    w: List[Tuple[int, bytes, str]] = []
    if "rng" in what:
        w += [(B.MTRAND_DEFAULT + 4, m.read(B.MTRAND_DEFAULT + 4, 4), "g_mtRand seed"),
              (B.MTRAND_OTHER + 4, m.read(B.MTRAND_OTHER + 4, 4), "g_mtRandOther seed"),
              (LIBC_RAND_NEXT, m.read(LIBC_RAND_NEXT, 4), "libc rand next")]
    if "frames" in what:
        w += [(B.GAME_FRAME, m.read(B.GAME_FRAME, 0x18), "g_GameFrame")]
        app = m.ptr(B.APPLICATION_PTR)
        w += [(app + 0x100, m.read(app + 0x100, 4), "gfApplication frame counter")]
    if "serial" in what:
        # .sdata 0x8059C668: a global creation counter whose value every new item/stage/fighter
        # object stores as its serial number (A 0x37 / B 0x4D at the first scMelee field: B's
        # menus created 0x16 more objects). Objects created at different serials diverged the
        # match after ~140 s (docs/gameplay-rollback-feasibility.md).
        w += [(OBJECT_SERIAL_COUNTER, m.read(OBJECT_SERIAL_COUNTER, 4), "object serial counter")]
    if "prnglog" in what:
        w += [(0x804977B4, m.read(0x804977B4, 0xC), "g_mtPrngLogManager")]
    return w


def lockstep(clients: Sequence[HarnessClient], frames: int, seed: Any, *, every_hash: int = 300,
             stop_after_diverge: int = 0, log_every: int = 600, mode: str = "closed",
             stage_kind: Optional[int] = None, probe_from: int = 0, save_at: int = 0,
             save_dir: Optional[Path] = None) -> Dict[str, Any]:
    """Clients paused at match frame 0 with neutral pads: schedule the same input stream on all
    of them, then step them together one game frame at a time and compare observables."""
    starts = par([lambda c=c: c.status().input_polls for c in clients])
    fighters: List[F.Fighter] = []
    busy = [0, 0]
    submitted: List[Any] = []
    if mode == "open":
        stream = input_stream(seed, frames)
        par([lambda c=c, s=s: schedule_inputs(c, stream, s + 3) for c, s in zip(clients, starts)])
    else:
        # Closed loop: decisions come from instance 0's state (flows.Fighter "random": seeded macros
        # biased toward the opponent, recovery when off stage) and the very same pad scripts go to
        # every instance at the same relative poll. Inputs stay identical even if states diverge.
        F._macro = _macro
        fighters = [F.Fighter(F.Seat(clients[0], port), "random", seed, stage_kind) for port in (0, 1)]
    rep: Dict[str, Any] = {"frames": frames, "seed": seed, "mode": mode, "first_diverged": None, "first_by_field": {},
                           "diffs": [], "heap_hash_diffs": [], "end": None, "inputs": submitted, "probe": []}
    probe_ranges: Optional[List[Tuple[int, int, str]]] = None
    probe_prev: Optional[Dict[Tuple[int, str], set]] = None
    t0 = time.monotonic()
    diverged_at = None
    for f in range(1, frames + 1):
        par([lambda c=c, s=s: _step_to(c, s + f) for c, s in zip(clients, starts)])
        sm = par([lambda c=c: small_state(c) for c in clients])
        if fighters and any(b <= f for b in busy):
            st0 = B.read_match_state(clients[0].read_mem)
            for port, ft in enumerate(fighters):
                if busy[port] > f:
                    continue
                seq = []
                for x in ft.decide(st0):
                    dd = dict(NEUTRAL)
                    dd.update({k: v for k, v in x.items() if k != "hold"})
                    seq.append(dict(dd, hold=int(x.get("hold", 1))))
                par([lambda c=c, s=s: c.pad_script(port, seq, start=s + f + 2) for c, s in zip(clients, starts)])
                busy[port] = f + 2 + sum(x["hold"] for x in seq)
                submitted.append([f, port, seq])
        d = state_diff(sm[0], sm[1], ("rng", "players", "elapsed", "remaining", "game_set", "frames"))
        if d:
            for k, v in d.items():
                if k == "players":
                    pa, pb = v
                    for i, fld in enumerate(PLAYER_FIELDS):
                        for x, y in zip(pa or [], pb or []):
                            if x[i] != y[i] and f"player.{fld}" not in rep["first_by_field"]:
                                rep["first_by_field"][f"player.{fld}"] = {"frame": f, "a": x, "b": y}
                elif k not in rep["first_by_field"]:
                    rep["first_by_field"][k] = {"frame": f, "a": v[0], "b": v[1]}
            # Boot-relative frame counters alone are not a gameplay divergence.
            if diverged_at is None and set(d) - {"frames"}:
                diverged_at = f
                rep["first_diverged"] = {"frame": f, "diff": d, "a": sm[0], "b": sm[1]}
            if len(rep["diffs"]) < 40 or f % 60 == 0:
                rep["diffs"].append({"frame": f, "diff": d})
        if probe_from and f >= probe_from and (diverged_at is None or f - diverged_at <= 10):
            # Which memory starts to differ newly, frame by frame, before the observables do?
            if probe_ranges is None:
                hp = heap_dicts(clients[0])
                probe_ranges = [(h["start"], h["end"] - h["start"], h["name"]) for h in hp
                                if h["name"] in PROBE_HEAPS]
                probe_ranges += [(a, e - a, n) for n, a, e in DOL_SECTIONS if n in CAND_DOL]
                probe_ranges += _rel_ranges(read_modules(clients[0].read_mem))
            cur = {}
            for a, n, lbl in probe_ranges:
                xa, xb = par([lambda c=c: read_big(c, a, n) for c in clients])
                cur[(a, lbl)] = set(o for o, _ in diff_runs(xa, xb, gran=32, merge_gap=0))
            if probe_prev is not None:
                new_g = {f"{lbl}@{a:#x}": sorted(cur[(a, lbl)] - probe_prev[(a, lbl)]) for a, lbl in cur}
                new_g = {k: [f"+{o:#x}" for o in v[:12]] + ([f"... {len(v)} granules"] if len(v) > 12 else [])
                         for k, v in new_g.items() if v}
                if new_g:
                    rep["probe"].append({"frame": f, "new_diff_granules": new_g})
            probe_prev = cur
        if save_at and f == save_at and save_dir is not None:
            # Analysis fixture: both instances' full state at this frame (not a rollback mechanism).
            for i, c in enumerate(clients):
                c.save_state(str((save_dir / f"lockstep-{f}-{i}.sav").resolve()))
            rep["saved_at"] = {"frame": f, "polls": [c.status().input_polls for c in clients],
                               "starts": starts, "busy": list(busy)}
        if every_hash and f % every_hash == 0:
            rs = gameplay_labelled(clients[0])
            hs = par([lambda c=c: {lbl: c.hash_mem([[a, n]]) for a, n, lbl in rs} for c in clients])
            rep["heap_hash_diffs"].append({"frame": f, "differ": [lbl for lbl in hs[0] if hs[0][lbl] != hs[1][lbl]]})
        if f % log_every == 0:
            log.info("frame %d: diverged=%s fields=%s players=%s", f, diverged_at, list(rep["first_by_field"]),
                     [p[2:6] for p in sm[0]["players"]])
        if sm[0].get("game_set") and sm[1].get("game_set"):
            break
        if diverged_at is not None and stop_after_diverge and f - diverged_at >= stop_after_diverge:
            break
    rep["end"] = {"frame": f, "a": sm[0], "b": sm[1]}
    rep["wall_s"] = round(time.monotonic() - t0, 1)
    return rep


def _step_to(c: HarnessClient, poll: int) -> None:
    n = 0
    while c.status().input_polls < poll:
        c.frame_advance(1)
        n += 1
        if n > 600:
            raise RuntimeError(f"stuck before poll {poll}")


def cmd_play(args: argparse.Namespace) -> int:
    d = Path(args.dir)
    pair = args.pair.split(",")
    specs = [(f"gprb-play{i}", dict(cpu_thread=args.cpu == "dc", video="Null",
                                    gpu_determinism=args.gpu)) for i in range(2)]
    reps = []
    with instances(specs) as insts:
        cs = [i.client for i in insts]
        for variant in args.sync.split(","):
            what = [x for x in variant.split("+") if x and x != "none"]
            for c, lbl in zip(cs, pair):
                load_fixture(c, d / f"{lbl}{args.point}.sav")
            ws = sync_writes(cs[0], what)
            for c in cs:
                for a, data, _ in ws:
                    c.write_mem(a, data)
            if "scrub" in what:
                # Make leftover garbage identical: in every heap whose block layout matches, copy
                # instance 0's free-block bytes into instance 1, and inside allocated blocks write
                # 0xCC fill wherever instance 0 still has the fill. Emulates a deterministic
                # fill of free memory before the match (a scrub both peers would do).
                scrub = scrub_writes(cs[0], cs[1])
                for a, data in scrub["writes"]:
                    cs[1].write_mem(a, data)
                ws = ws + [(0, b"", f"scrub: {scrub['bytes']} bytes in {len(scrub['writes'])} writes, heaps {scrub['heaps']}")]
            sim = None
            if args.point == "0":
                # Synced at the first scMelee field, before the stage and fighters are created: now
                # run each instance (neutral input) to its own simulation start, where they line up.
                sim = par([lambda c=c: to_sim_start(c) for c in cs])
            pre = par([lambda c=c: small_state(c) for c in cs])
            setup = B.read_match_setup(cs[0].read_mem)
            rep = lockstep(cs, args.frames, args.seed, stop_after_diverge=args.stop_after, mode=args.input,
                           stage_kind=setup.stage_kind if setup else None, probe_from=args.probe_from,
                           save_at=args.save_at, save_dir=d)
            rep.update(pair=pair, sync=variant, cpu=args.cpu, gpu=args.gpu, written=[(f"{a:#x}", x.hex(), n) for a, x, n in ws],
                       at_frame0=pre, point=args.point, sim_start=sim)
            if args.end_dump:
                # Both still paused on the last compared frame: where do the two memories differ now?
                dumps = par([lambda c=c: dump_all(c) for c in cs])
                heaps = heap_dicts(cs[0])
                rep["end_heap_alloc"] = heap_alloc_report(dumps[0], dumps[1], heaps)
                rep["end_regions"] = [r for r in compare_dumps(dumps[0], dumps[1], memory_map(heaps, read_modules(cs[0].read_mem)), samples=2)
                                      if r["diff_bytes"]]
                tag = f"{'-'.join(pair)}-{variant}-{args.cpu}"
                for lbl, dd in zip(pair, dumps):
                    save_dump(dd, d / f"end-{tag}-{lbl}.mem")
                print("    end heaps (alloc diff / free diff / same layout):",
                      [(r["heap"], r.get("diff_alloc"), r.get("diff_free"), r.get("same_layout")) for r in rep["end_heap_alloc"]
                       if r.get("diff_alloc") or not r.get("same_layout", True)])
            reps.append(rep)
            fd = rep["first_diverged"]
            print(f"{'/'.join(pair)} sync={variant}: frames {rep['end']['frame']}, first divergence "
                  f"{fd['frame'] if fd else None}, by field {json.dumps({k: v['frame'] for k, v in rep['first_by_field'].items()})}, "
                  f"wall {rep['wall_s']} s", flush=True)
            if fd:
                print("   ", json.dumps(fd["diff"], default=str)[:800])
            print("    end A:", rep["end"]["a"]["players"], rep["end"]["a"]["rng"])
            print("    end B:", rep["end"]["b"]["players"], rep["end"]["b"]["rng"])
            print("    heap hash diffs:", [(h["frame"], len(h["differ"])) for h in rep["heap_hash_diffs"]][:20])
            if args.json:
                Path(args.json).write_text(json.dumps(reps, indent=1, default=str))
            for c in cs:
                with contextlib.suppress(Exception):
                    c.resume()
    return 0


# --------------------------------------------------------------------------- replay (experiment 4)


def timeline_from_inputs(inputs: Sequence[Sequence[Any]], frames: int) -> Dict[int, List[Dict[str, Any]]]:
    """A play run's closed-loop submissions [(frame, port, seq)] (each scheduled at sim-relative
    poll frame+2, replacing the port's script, neutral after it) as one script per port."""
    out: Dict[int, List[Dict[str, Any]]] = {}
    for port in (0, 1):
        subs = sorted((int(f) + 2, seq) for f, p, seq in inputs if int(p) == port)
        seq_out: List[Dict[str, Any]] = []
        t = 1
        for i, (start, seq) in enumerate(subs):
            nxt = subs[i + 1][0] if i + 1 < len(subs) else frames
            if start > t:
                seq_out.append(dict(NEUTRAL, hold=start - t))
                t = start
            for x in seq:
                if t >= nxt:
                    break
                h = min(int(x["hold"]), nxt - t)
                seq_out.append(dict(x, hold=h))
                t += h
        if t < frames:
            seq_out.append(dict(NEUTRAL, hold=frames - t))
        out[port] = seq_out
    return out


def pause_at_game_frame(c: HarnessClient, target: int, margin: int = 40, timeout: float = 600.0) -> None:
    """Running instance: wait (without pausing) until close to game frame ``target``, then pause
    and frame_advance to it. Pauses only once per checkpoint (dual core crashed under frequent
    pause/frame_advance, determinism doc)."""
    deadline = time.monotonic() + timeout
    while game_frame(c) < target - margin:
        if time.monotonic() > deadline:
            raise RuntimeError(f"game frame stuck at {game_frame(c)}")
        time.sleep(0.25)
    c.pause()
    step_to_game_frame(c, target)


def cmd_replay(args: argparse.Namespace) -> int:
    """Replay a recorded play run's inputs on A and B (or any pair) without per-frame pauses and
    compare at checkpoints. For dual core."""
    d = Path(args.dir)
    src = json.loads(Path(args.inputs).read_text())
    src = src[args.run_index] if isinstance(src, list) else src
    tl = timeline_from_inputs(src["inputs"], args.frames)
    pair = args.pair.split(",")
    specs = [(f"gprb-rep{i}", dict(cpu_thread=args.cpu == "dc", video=args.video, gpu_determinism=args.gpu))
             for i in range(2)]
    out: Dict[str, Any] = {"pair": pair, "cpu": args.cpu, "gpu": args.gpu, "sync": args.sync, "frames": args.frames,
                           "checkpoints": [], "source": args.inputs, "reference": None}
    ok = True
    with instances(specs) as insts:
        cs = [i.client for i in insts]
        for c, lbl in zip(cs, pair):
            load_fixture(c, d / f"{lbl}s.sav")
        ws = sync_writes(cs[0], [x for x in args.sync.split("+") if x and x != "none"])
        for c in cs:
            for a, data, _ in ws:
                c.write_mem(a, data)
        g0 = par([lambda c=c: game_frame(c) for c in cs])
        starts = par([lambda c=c: c.status().input_polls for c in cs])
        par([lambda c=c, s=s: schedule_inputs(c, tl, s + 1) for c, s in zip(cs, starts)])
        for c in cs:
            c.resume()
        t0 = time.monotonic()
        for cp in range(args.every, args.frames - 30, args.every):
            try:
                par([lambda c=c, g=g: pause_at_game_frame(c, g + cp) for c, g in zip(cs, g0)])
            except Exception as e:  # noqa: BLE001
                out["error"] = f"{type(e).__name__}: {e}"
                out["died"] = [{"name": i.name, "exit_code": i.process.poll() if i.process else None} for i in insts]
                ok = False
                break
            sm = par([lambda c=c: small_state(c) for c in cs])
            rs = gameplay_labelled(cs[0])
            hs = par([lambda c=c: {lbl: c.hash_mem([[a, n]]) for a, n, lbl in rs} for c in cs])
            row = {"frame": cp, "diff": state_diff(sm[0], sm[1], ("rng", "players", "elapsed", "game_set")),
                   "heap_hash_differ": [lbl for lbl in hs[0] if hs[0][lbl] != hs[1][lbl]],
                   "players": sm[0]["players"]}
            out["checkpoints"].append(row)
            log.info("cp %d: diff=%s players=%s", cp, list(row["diff"]), [p[2:6] for p in sm[0]["players"]])
            for c in cs:
                c.resume()
            if sm[0].get("game_set") or sm[1].get("game_set"):
                break
        out["wall_s"] = round(time.monotonic() - t0, 1)
    first = next((r["frame"] for r in out["checkpoints"] if r["diff"]), None)
    out["first_diverged_checkpoint"] = first
    print(f"replay {'/'.join(pair)} cpu={args.cpu} gpu={args.gpu} sync={args.sync}: checkpoints {len(out['checkpoints'])}, "
          f"first differing checkpoint {first}, error {out.get('error')}")
    if args.json:
        p = Path(args.json)
        prev = json.loads(p.read_text()) if p.exists() else []
        prev.append(out)
        p.write_text(json.dumps(prev, indent=1, default=str))
    return 0 if ok else 1


# --------------------------------------------------------------------------- region sets


def ranges_all(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
    """Everything except Brawlback's audio/framebuffer exclusions and the disc ID."""
    return B.exclude_state_noise([(MEM1[0], MEM1[1], "MEM1"), (MEM2[0], MEM2[1], "MEM2")])


# Heaps whose contents are match simulation state (instances, managers, globals).
CAND_HEAPS = ("System", "Effect", "Fighter1Instance", "Fighter2Instance", "Fighter3Instance", "Fighter4Instance",
              "FighterTechqniq", "StageInstance", "ItemInstance", "WeaponInstance", "EnemyInstance", "Physics",
              "InfoInstance", "GameGlobal", "GlobalMode", "WiiPad", "FighterEffect")
# DOL sections holding mutable globals.
CAND_DOL = ("dol.data", "dol.bss", "dol.sdata", "dol.sbss", "dol.sdata2", "dol.sbss2")


def _heap_ranges(heaps: Sequence[Mapping[str, Any]], names: Iterable[str]) -> List[Tuple[int, int, str]]:
    want = set(names)
    return [(h["start"], h["end"] - h["start"], h["name"]) for h in heaps if h["name"] in want]


def _rel_ranges(modules: Sequence[Mapping[str, Any]], kinds: Callable[[str], bool] = lambda k: k != "text") -> List[Tuple[int, int, str]]:
    return [(s["addr"], s["size"], f"{m_['name']}.{s['kind']}") for m_ in modules for s in m_["sections"] if kinds(s["kind"])]


def _dol_ranges(names: Iterable[str]) -> List[Tuple[int, int, str]]:
    want = set(names)
    return [(a, e - a, n) for n, a, e in DOL_SECTIONS if n in want]


def ranges_candidate(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
    rs = _heap_ranges(heaps, CAND_HEAPS) + _rel_ranges(modules) + _dol_ranges(CAND_DOL)
    return B.exclude_state_noise(rs)


def ranges_heaps_only(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
    return B.exclude_state_noise(_heap_ranges(heaps, CAND_HEAPS))


def ranges_brawl_gameplay(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
    """brawl.GAMEPLAY_HEAPS plus the RNG and g_GameFrame (what gameplay_ranges hashes)."""
    rs = _heap_ranges(heaps, B.GAMEPLAY_HEAPS) + [(B.MTRAND_DEFAULT, 8, "g_mtRand"), (B.MTRAND_OTHER, 8, "g_mtRandOther")]
    return B.exclude_state_noise(rs)


def ranges_all_minus_system(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
    """'all' minus OS/audio/video/thread state: lowmem, the boot stack, SystemFW, Sound, RenderFifo,
    CopyFB, Thread, Network, Replay, Tmp, and MEM2 below the first MEM2 heap (framebuffers)."""
    holes = [(0x80000000, 0x4000), (0x805A5160, 0x10000)]
    holes += [(h["start"], h["end"] - h["start"]) for h in heaps if h["name"] in SYSTEM_EXCLUDE_HEAPS]
    mem2_first = min((h["start"] for h in heaps if h["start"] >= 0x90000000), default=0x90000000)
    holes.append((0x90000000, mem2_first - 0x90000000))
    return B.subtract_ranges(ranges_all(heaps, modules), holes)


AV_HEAPS = ("Sound", "RenderFifo", "CopyFB")
RESOURCE_HEAPS = ("IteamResource", "InfoResource", "CommonResource", "StageResoruce", "StageCommonResource",
                  "Fighter1Resoruce", "Fighter2Resoruce", "Fighter3Resoruce", "Fighter4Resoruce",
                  "Fighter1Resoruce2", "Fighter2Resoruce2", "Fighter3Resoruce2", "Fighter4Resoruce2",
                  "FighterKirbyResource1", "FighterKirbyResource2", "FighterKirbyResource3", "AssistFigureResource",
                  "ItemExtraResource", "PokemonResource", "MenuResource", "StockResource", "MeleeFont",
                  "InfoExtraResource")


def ranges_all_minus_av(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
    """'all' minus audio and video memory only: Sound, RenderFifo, CopyFB and MEM2 below the first
    MEM2 heap (XFB/EFB copies). OS state (lowmem, stacks, threads, alarms) is restored."""
    holes = [(h["start"], h["end"] - h["start"]) for h in heaps if h["name"] in AV_HEAPS]
    mem2_first = min((h["start"] for h in heaps if h["start"] >= 0x90000000), default=0x90000000)
    holes.append((0x90000000, mem2_first - 0x90000000))
    return B.subtract_ranges(ranges_all(heaps, modules), holes)


def ranges_all_minus_av_res(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
    """all-av without the resource heaps (loaded files) and overlay code."""
    holes = [(h["start"], h["end"] - h["start"]) for h in heaps if h["name"] in RESOURCE_HEAPS]
    holes += [(s_["addr"], s_["size"]) for m_ in modules for s_ in m_["sections"] if s_["kind"] == "text"]
    holes += [(a, e - a) for n, a, e in DOL_SECTIONS if n in ("dol.init", "dol.extab", "dol.extabindex", "dol.text",
                                                               "dol.ctors/dtors", "dol.rodata")]
    return B.subtract_ranges(ranges_all_minus_av(heaps, modules), holes)


def ranges_all_minus_system_dol(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
    """all-sys without any of main.dol's writable sections (keeps REL data/bss and heaps)."""
    holes = [(a, e - a) for n, a, e in DOL_SECTIONS if n in CAND_DOL]
    return B.subtract_ranges(ranges_all_minus_system(heaps, modules), holes)


def ranges_file(path: str) -> Callable[[Sequence[Mapping[str, Any]], Sequence[Mapping[str, Any]]], List[Tuple[int, int, str]]]:
    """A set from a JSON file: {"base": <set name>, "add": [[addr, len, label]], "remove": [[addr, len]],
    "add_heaps": [...], "remove_heaps": [...], "add_rel": bool (REL data/bss), "add_dol": [DOL section names]}."""
    def f(heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
        spec = json.loads(Path(path).read_text())
        rs = list(REGION_SETS[spec.get("base", "heaps")](heaps, modules))
        rs += [(int(str(a), 0), int(str(n), 0), lbl) for a, n, lbl in spec.get("add", [])]
        rs += _heap_ranges(heaps, spec.get("add_heaps", []))
        if spec.get("add_rel"):
            rs += _rel_ranges(modules)
        rs += _dol_ranges(spec.get("add_dol", []))
        holes = [(int(str(a), 0), int(str(n), 0)) for a, n, *_ in spec.get("remove", [])]
        holes += [(h["start"], h["end"] - h["start"]) for h in heaps if h["name"] in spec.get("remove_heaps", [])]
        return B.subtract_ranges(B.merge_ranges(rs), holes)
    return f


REGION_SETS: Dict[str, Callable[[Sequence[Mapping[str, Any]], Sequence[Mapping[str, Any]]], List[Tuple[int, int, str]]]] = {
    "all": ranges_all,
    "candidate": ranges_candidate,
    "heaps": ranges_heaps_only,
    "brawl_gameplay": ranges_brawl_gameplay,
    "all-sys": ranges_all_minus_system,
    "all-av": ranges_all_minus_av,
    "all-av-res": ranges_all_minus_av_res,
    "all-sys-dol": ranges_all_minus_system_dol,
}


def set_ranges(name: str, heaps: Sequence[Mapping[str, Any]], modules: Sequence[Mapping[str, Any]]) -> List[Tuple[int, int, str]]:
    if name.endswith(".json"):
        return B.merge_ranges(ranges_file(name)(heaps, modules))
    if name not in REGION_SETS:
        raise SystemExit(f"unknown region set {name!r}; known: {sorted(REGION_SETS)}")
    return B.merge_ranges(REGION_SETS[name](heaps, modules))


def restore_ranges(c: HarnessClient, saved: Mapping[int, bytes], current: Mapping[int, bytes],
                   ranges: Sequence[Tuple[int, int, str]]) -> Dict[str, int]:
    """Write back the bytes of ``ranges`` that differ between ``saved`` and ``current`` (full
    dumps). Writing only what changed is equivalent to a full copy, and keeps unchanged code
    out of write_mem's JIT invalidation."""
    nbytes = nwrites = 0
    for a, n, _ in ranges:
        old, cur = dview(saved, a, n), dview(current, a, n)
        for off, ln in diff_runs(cur, old):
            c.write_mem(a + off, old[off:off + ln])
            nbytes += ln
            nwrites += 1
    return {"bytes": nbytes, "writes": nwrites}


def region_report(d1: Mapping[int, bytes], d2: Mapping[int, bytes], regions: Sequence[Region],
                  limit: int = 60) -> List[Dict[str, Any]]:
    """Per named region: differing bytes/words between two dumps (only regions that differ)."""
    out = []
    for r in regions:
        a, b = dview(d1, r.start, r.size), dview(d2, r.start, r.size)
        if a == b:
            continue
        nb, nw = count_diff_bytes(a, b)
        runs = diff_runs(a, b, gran=4, merge_gap=0)
        out.append({"region": r.name, "kind": r.kind, "start": f"{r.start:#010x}", "size": r.size,
                    "diff_bytes": nb, "diff_words": nw, "runs": len(runs),
                    "first": [f"{r.start + o:#010x}+{ln:#x}" for o, ln in runs[:6]]})
    out.sort(key=lambda x: -x["diff_bytes"])
    return out[:limit] if limit else out


def split_regions(regions: Sequence[Region], ranges: Sequence[Tuple[int, int, str]]) -> Tuple[List[Region], List[Region]]:
    """Regions cut into the parts inside ``ranges`` and the parts outside."""
    inside, outside = [], []
    rs = sorted((a, a + n) for a, n, _ in ranges)
    for r in regions:
        cur = r.start
        for a, e in rs:
            if e <= cur or a >= r.end:
                continue
            if a > cur:
                outside.append(Region(cur, a, r.name, r.kind))
            inside.append(Region(max(a, cur), min(e, r.end), r.name, r.kind))
            cur = min(e, r.end)
        if cur < r.end:
            outside.append(Region(cur, r.end, r.name, r.kind))
    return inside, outside


def game_frame(c: HarnessClient) -> int:
    return struct.unpack(">I", c.read_mem(B.GAME_FRAME + 4, 4))[0]


def step_to_game_frame(c: HarnessClient, target: int, max_fields: int = 0) -> List[Tuple[int, int]]:
    """While paused: frame_advance VI fields until g_GameFrame.frameCounter == target. Returns the
    (input_polls, frameCounter) seen after each field."""
    max_fields = max_fields or 3 * max(1, target - game_frame(c)) + 120
    trace = []
    for _ in range(max_fields):
        g = game_frame(c)
        if g == target:
            return trace
        if g > target:
            raise RuntimeError(f"passed game frame {target} (at {g})")
        c.frame_advance(1)
        trace.append((c.status().input_polls, game_frame(c)))
    raise RuntimeError(f"game frame stuck at {game_frame(c)} (target {target}): the game hung")


SYSTEM_EXCLUDE_HEAPS = ("Sound", "RenderFifo", "CopyFB", "Thread", "Network", "Replay", "Tmp", "System FW")


def synctest_once(c: HarnessClient, set_name: str, k: int, seed: Any, out: Optional[Path] = None,
                  label: str = "") -> Dict[str, Any]:
    """Paused at a VI field boundary with neutral pads, at game frame G: save the region set,
    play K game frames of scripted input (scheduled when the game is at G+2), restore the set,
    replay the same K frames (scheduled the same way), and compare at game frame G+K.
    Keyed on the game's own frame counter (g_GameFrame+4), not on harness polls: after a
    memory-only restore the VI/poll phase can slip by a field."""
    heaps, modules = heap_dicts(c), read_modules(c.read_mem)
    regions = memory_map(heaps, modules)
    rset = set_ranges(set_name, heaps, modules)
    g0 = game_frame(c)
    p_n = c.status().input_polls
    d_n = dump_all(c)
    small_n = small_state(c)
    stream = input_stream(seed, k - 12)
    rep: Dict[str, Any] = {"label": label, "set": set_name, "k": k, "game_frame_n": g0, "poll_n": p_n,
                           "set_bytes": sum(n for _, n, _ in rset), "small_n": small_n}
    tr1 = step_to_game_frame(c, g0 + 2)
    w1 = schedule_inputs(c, stream, "next")
    rep["run1"] = {"fields_to_g+2": len(tr1), "script_start_minus_poll": w1[0] - c.status().input_polls}
    try:
        step_to_game_frame(c, g0 + k)
    except RuntimeError as e:
        rep["error"] = f"first pass did not reach G+K (match over?): {e}"
        rep["observables_equal"] = None
        return rep
    d1, small1 = dump_all(c), small_state(c)
    t0 = time.monotonic()
    rep["restored"] = restore_ranges(c, d_n, d1, rset)
    rep["restore_s"] = round(time.monotonic() - t0, 2)
    check = dump_all(c)
    rep["readback_mismatch_ranges"] = sum(1 for a, n, _ in rset if dview(check, a, n) != dview(d_n, a, n))
    g_after = game_frame(c)
    rep["game_frame_after_restore"] = g_after
    try:
        if g_after != g0:
            raise RuntimeError(f"frameCounter not restored ({g_after}, wanted {g0}); the set must include g_GameFrame")
        tr2 = step_to_game_frame(c, g0 + 2)
        w2 = schedule_inputs(c, stream, "next")
        rep["run2"] = {"fields_to_g+2": len(tr2), "script_start_minus_poll": w2[0] - c.status().input_polls,
                       "trace_after_restore": tr2[:6]}
        step_to_game_frame(c, g0 + k)
    except Exception as e:  # noqa: BLE001 - a hang or crash is a result too
        rep["error"] = f"{type(e).__name__}: {e}"
        rep["observables_equal"] = False
        rep["small_run1"] = small1
        with contextlib.suppress(Exception):
            rep["small_run2"] = small_state(c)
        _, outside = split_regions(regions, rset)
        rep["written_outside_set_in_window"] = region_report(d_n, d1, outside)
        if out is not None:
            out.mkdir(parents=True, exist_ok=True)
            for nm, dd in (("N", d_n), ("run1", d1)):
                save_dump(dd, out / f"synctest-{label}-{nm}.mem")
        return rep
    d2, small2 = dump_all(c), small_state(c)
    inside, outside = split_regions(regions, rset)
    keys = ("rng", "players", "elapsed", "remaining")
    rep.update({
        "small_run1": small1, "small_run2": small2,
        "observables_equal": state_diff(small1, small2, keys) == {},
        "observable_diff": state_diff(small1, small2, keys),
        "moved_in_window": state_diff(small_n, small1, ("players",)) != {},
        "diverged_inside_set": region_report(d1, d2, inside),
        "differs_outside_set_run1_vs_run2": region_report(d1, d2, outside),
        "written_outside_set_in_window": region_report(d_n, d1, outside),
    })
    if out is not None:
        out.mkdir(parents=True, exist_ok=True)
        for nm, dd in (("N", d_n), ("run1", d1), ("run2", d2)):
            save_dump(dd, out / f"synctest-{label}-{nm}.mem")
    return rep


def print_synctest(rep: Mapping[str, Any]) -> None:
    keys = ("set", "k", "game_frame_n", "set_bytes", "restored", "readback_mismatch_ranges", "observables_equal",
            "moved_in_window", "observable_diff", "run1", "run2", "error")
    print(json.dumps({k: rep.get(k) for k in keys}, default=str))
    for key, title in (("diverged_inside_set", "diverged inside set"),
                       ("differs_outside_set_run1_vs_run2", "differs outside (run1 vs run2)"),
                       ("written_outside_set_in_window", "written outside in window")):
        if key in rep:
            print(f"  {title}:", [(x["region"], x["start"], x["diff_bytes"]) for x in rep[key][:24]])
    sys.stdout.flush()


def cmd_synctest(args: argparse.Namespace) -> int:
    reps = []
    outdir = Path(args.dir) if args.dir else None

    def body(c: HarnessClient) -> None:
        for set_name in args.set.split(","):
            for rep_i in range(args.repeat):
                if args.state:
                    load_fixture(c, Path(args.state))
                    if args.pre_inputs:
                        # Replay a play run's recorded closed-loop inputs up to N (fighters stay alive).
                        src = json.loads(Path(args.pre_inputs).read_text())
                        src = src[0] if isinstance(src, list) else src
                        tl = timeline_from_inputs([x for x in src["inputs"] if int(x[0]) + 40 < args.n], args.n)
                        schedule_inputs(c, tl, c.status().input_polls + 1)
                    elif args.n >= 80:
                        # Scripted play up to N (ends with 30 neutral frames).
                        schedule_inputs(c, input_stream(f"pre:{args.seed}", args.n - 6), c.status().input_polls + 3)
                    advance_polls(c, args.n)
                else:
                    pause_on_field(c)
                    advance_polls(c, args.n)
                rep = synctest_once(c, set_name, args.k, f"{args.seed}:{rep_i}",
                                    outdir if args.save_dumps else None, label=f"{Path(set_name).stem}-{rep_i}")
                reps.append(rep)
                print_synctest(rep)
                if not args.state:
                    c.resume()
                if args.json:
                    Path(args.json).parent.mkdir(parents=True, exist_ok=True)
                    Path(args.json).write_text(json.dumps(reps, indent=1, default=str))

    if args.port:
        c = HarnessClient.connect(args.port, timeout=120)
        try:
            body(c)
        finally:
            c.close()
    else:
        with instances([("gprb-sync", dict(cpu_thread=args.cpu == "dc", video="Null"))]) as insts:
            body(insts[0].client)
    return 0


# --------------------------------------------------------------------------- main


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prep")
    p.add_argument("--out", required=True)
    p.add_argument("--chars", default="peach,game_and_watch")
    p.add_argument("--stage", default="pokemon_stadium_2")
    p.add_argument("--cpu", default="sc", choices=("sc", "dc"))
    p.add_argument("--video", default="Null")
    p.add_argument("--rtc-b", default=None, help="B's custom RTC (default: same as A)")
    p.set_defaults(fn=cmd_prep)
    p = sub.add_parser("simstart")
    p.add_argument("--dir", required=True)
    p.add_argument("--cpu", default="sc", choices=("sc", "dc"))
    p.set_defaults(fn=cmd_simstart)
    p = sub.add_parser("diff")
    p.add_argument("--dir", required=True)
    p.add_argument("--points", default="0,s,go", help="0 = first scMelee field, s = simulation start, go = GO")
    p.set_defaults(fn=cmd_diff)
    p = sub.add_parser("play")
    p.add_argument("--dir", required=True)
    p.add_argument("--pair", default="A,B", help="which frame-0 states the two instances load (A,B / A,A / B,B)")
    p.add_argument("--sync", default="none", help="comma-separated variants, each a '+' list of rng, frames, prnglog (or none)")
    p.add_argument("--frames", type=int, default=3900)
    p.add_argument("--seed", default="play1")
    p.add_argument("--cpu", default="sc", choices=("sc", "dc"))
    p.add_argument("--gpu", default=None, help="GPUDeterminismMode override (default: P+ INI fake-completion)")
    p.add_argument("--stop-after", type=int, default=0, help="stop this many frames after the first divergence")
    p.add_argument("--input", default="closed", choices=("closed", "open"))
    p.add_argument("--save-at", type=int, default=0, help="savestate both instances at this frame (analysis fixture)")
    p.add_argument("--probe-from", type=int, default=0, help="from this frame on, report memory that newly differs each frame")
    p.add_argument("--end-dump", action="store_true", help="dump both memories at the end and compare heaps")
    p.add_argument("--point", default="s", help="savestate to start from: s (simulation start, default) or 0 (first scMelee field)")
    p.add_argument("--json", default=None)
    p.set_defaults(fn=cmd_play)
    p = sub.add_parser("replay")
    p.add_argument("--dir", required=True)
    p.add_argument("--inputs", required=True, help="a play --json file whose recorded closed-loop inputs are replayed")
    p.add_argument("--run-index", type=int, default=0)
    p.add_argument("--pair", default="A,B")
    p.add_argument("--sync", default="rng+frames")
    p.add_argument("--frames", type=int, default=4000)
    p.add_argument("--every", type=int, default=300)
    p.add_argument("--cpu", default="dc", choices=("sc", "dc"))
    p.add_argument("--gpu", default=None)
    p.add_argument("--video", default="Null")
    p.add_argument("--json", default=None)
    p.set_defaults(fn=cmd_replay)
    p = sub.add_parser("synctest")
    p.add_argument("--state", default=None, help="savestate to start from (else: attach and use the live state)")
    p.add_argument("--port", type=int, default=None, help="attach to a running instance")
    p.add_argument("--dir", default=None)
    p.add_argument("--set", default="all", help="comma-separated region sets")
    p.add_argument("--n", type=int, default=300, help="frames to play (neutral) before the save point")
    p.add_argument("--k", type=int, default=120)
    p.add_argument("--seed", default="sync")
    p.add_argument("--repeat", type=int, default=1)
    p.add_argument("--cpu", default="sc", choices=("sc", "dc"))
    p.add_argument("--save-dumps", action="store_true")
    p.add_argument("--pre-inputs", default=None, help="play --json whose recorded inputs drive the frames before N")
    p.add_argument("--json", default=None)
    p.set_defaults(fn=cmd_synctest)
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    os.environ.setdefault("PPHARNESS_DOLPHIN_DIR", str(Path(__file__).resolve().parents[2] / "run/bin/harness-100b8fd189"))
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
