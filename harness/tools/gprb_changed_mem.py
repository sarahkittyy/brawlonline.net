"""Which memory changes from frame to frame during a match, outside the gameplay region set?"""
import json
import sys
from pathlib import Path
import time
from collections import defaultdict
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gameplay_rollback as G
import gprb_synctest as T
from ppharness import brawl as B

CHUNK = 256
SET = json.load(open(Path(__file__).resolve().parents[2] / "dolphin-gprb" / "Data" / "Sys" / "Rollback" / "gp-v9.json"))


def hashes(c):
    r = c.call("mem_chunk_hashes", chunk=CHUNK)
    return r["mem1"], r["mem2"]


with G.instances([("gprb-changed", dict())]) as (inst,):
    c = inst.client
    T.setup_match(c, "mario", "marth", "final_destination", False)
    while (G.B.read_frame_counters(c.read_mem).get("game_frame") or 0) < 400:
        time.sleep(0.2)
    # Move both fighters a bit so gameplay runs.
    c.pad_set(0, main=[255, 128])
    c.pad_set(1, main=[1, 128])
    c.pause()
    heaps = B.read_heap_table(c.read_mem)
    prev = hashes(c)
    changed = set()
    for step in range(8):
        c.frame_advance(1)
        cur = hashes(c)
        for bank, base in ((0, 0x80000000), (1, 0x90000000)):
            for i, (a, b) in enumerate(zip(prev[bank], cur[bank])):
                if a != b:
                    changed.add(base + i * CHUNK)
        prev = cur
    in_set_heaps = set(SET["heaps"])
    adds = [(int(a, 16), int(n, 16)) for a, n, *_ in SET["add"]]

    def where(a):
        for h in heaps:
            if h.start <= a < h.end:
                return h.name
        return "static"

    def in_adds(a):
        return any(s <= a < s + n or a <= s < a + CHUNK for s, n in adds)

    groups = defaultdict(list)
    for a in sorted(changed):
        w = where(a)
        if w in in_set_heaps:
            continue
        groups[w].append(a)
    for w, lst in sorted(groups.items(), key=lambda kv: kv[0]):
        rows = [f"{a:08x}{'*' if in_adds(a) else ''}" for a in lst]
        print(f"{w}: {len(lst)} chunks: {' '.join(rows[:60])}")
