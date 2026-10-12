"""Fuzz the Brawl Mods machine-image loader (docs/brawl-mods-security.md §4).

Boots a game in one harness Dolphin, saves its state the way a machine image carries it
(`machine_image_state_save`), then loads mutated copies (`machine_image_state_load`, the image's
rules) one after another: after each one the loader accepted, the machine runs for a moment and
the pristine state is loaded back (which must succeed). A crash ends that Dolphin: its crash report
(harness-stderr.txt) and the mutation are kept, and a new Dolphin carries on.

Mutations (seeded per iteration, so a finding replays with --only N): flipped bytes, a 32-bit field
set to an edge value, truncation, a chunk copied from elsewhere. Most land outside guest RAM (the
device state after it and the video state before it), which is what the loader parses.

usage: python harness/tools/fuzz_machine_image.py OUT --game <dol or disc> [--hours 1] [--exe ..]
       [--template ..] [--only N]
"""
from __future__ import annotations

import argparse
import json
import random
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ppharness.instance import DolphinInstance, InstanceConfig  # noqa: E402

EDGES = [0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
         0x10000, 0x1000000, 0x40000000]


def ram_span(state: bytes) -> tuple[int, int]:
    """[start, end) of MEM1 + L1 + MEM2 in the state (found by the disc ID at MEM1's start)."""
    for disc_id in (b"RSBE01", b"RSBP01", b"RSBJ01", b"RSBK01"):
        m1 = state.find(disc_id)
        if m1 > 18:
            ram, l1 = struct.unpack("<II", state[m1 - 18:m1 - 10])
            ex = struct.unpack("<I", state[m1 - 4:m1])[0]
            return m1, m1 + ram + l1 + 8 + ex
    return 0, 0


def mutate(state: bytes, rng: random.Random, span: tuple[int, int]) -> tuple[bytes, str]:
    data = bytearray(state)
    lo, hi = span

    def offset() -> int:
        # 90% outside guest RAM.
        if hi > lo and rng.random() < 0.9:
            if rng.random() < 0.5 and lo > 0:
                return rng.randrange(0, lo)
            return rng.randrange(hi, len(data)) if hi < len(data) else rng.randrange(0, lo)
        return rng.randrange(0, len(data))

    kind = rng.choice(["flip", "flip", "edge32", "edge32", "edge32", "truncate", "splice"])
    if kind == "flip":
        n = rng.randint(1, 8)
        offs = [offset() for _ in range(n)]
        for o in offs:
            data[o] ^= 1 << rng.randrange(8)
        return bytes(data), f"flip {n} at {offs[:4]}"
    if kind == "edge32":
        o = offset() & ~3
        v = rng.choice(EDGES)
        data[o:o + 4] = struct.pack("<I", v)
        return bytes(data), f"edge32 {v:#x} at {o}"
    if kind == "truncate":
        n = rng.randrange(0, len(data))
        return bytes(data[:n]), f"truncate to {n}"
    a, b = offset(), offset()
    n = rng.randint(4, 4096)
    data[b:b + n] = data[a:a + n]
    return bytes(data[:len(state)]), f"splice {n} from {a} to {b}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--game", required=True)
    ap.add_argument("--exe", default=None)
    ap.add_argument("--template", default=None)
    ap.add_argument("--hours", type=float, default=1.0)
    ap.add_argument("--boot-wait", type=float, default=60.0, help="seconds to let the game boot")
    ap.add_argument("--run", type=float, default=0.5, help="seconds the machine runs after a load")
    ap.add_argument("--only", type=int, default=None, help="replay this one iteration")
    ap.add_argument("--port", type=int, default=None)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    stats = {"iterations": 0, "loaded": 0, "refused": 0, "crashes": 0, "restore_failed": 0}
    findings: list[dict] = []
    pristine_path = out / "pristine.bin"
    mutant_path = out / "mutant.bin"

    def start() -> DolphinInstance:
        inst = DolphinInstance(f"fuzz-img-{int(time.time())}", exe=a.exe, template=a.template,
                               config=InstanceConfig(), boot=a.game, port=a.port, keep=False,
                               connect_timeout=90)
        inst.start()
        time.sleep(a.boot_wait)
        return inst

    inst = start()
    size = inst.client.call("machine_image_state_save", path=str(pristine_path), timeout=60)["size"]
    pristine = pristine_path.read_bytes()
    span = ram_span(pristine)
    print(f"pristine state {size} bytes, guest RAM at {span}", flush=True)

    deadline = time.time() + a.hours * 3600
    iterations = [a.only] if a.only is not None else range(1, 1 << 30)
    for i in iterations:
        if a.only is None and time.time() > deadline:
            break
        rng = random.Random(i)
        data, what = mutate(pristine, rng, span)
        mutant_path.write_bytes(data)
        stats["iterations"] += 1
        try:
            r = inst.client.call("machine_image_state_load", path=str(mutant_path), timeout=60)
            if r.get("loaded"):
                stats["loaded"] += 1
                time.sleep(a.run)
            else:
                stats["refused"] += 1
            back = inst.client.call("machine_image_state_load", path=str(pristine_path), timeout=60)
            if not back.get("loaded"):
                stats["restore_failed"] += 1
                findings.append({"iteration": i, "mutation": what, "kind": "pristine refused after"})
        except Exception as e:  # noqa: BLE001 -- the Dolphin died or stopped answering
            stats["crashes"] += 1
            crash = ""
            stderr = inst.user_dir / "harness-stderr.txt" if inst.user_dir else None
            if stderr and stderr.exists():
                crash = stderr.read_text(errors="replace")[-4000:]
            finding = {"iteration": i, "mutation": what, "error": str(e)[:300], "crash": crash}
            findings.append(finding)
            shutil.copy(mutant_path, out / f"crash-{i}.bin")
            print("CRASH", json.dumps(finding)[:1500], flush=True)
            try:
                inst.cleanup(True)
            except Exception:  # noqa: BLE001
                pass
            inst = start()
        if stats["iterations"] % 50 == 0:
            print(json.dumps(stats), flush=True)
        (out / "fuzz.json").write_text(json.dumps({"stats": stats, "findings": findings}, indent=1))
    try:
        inst.cleanup(True)
    except Exception:  # noqa: BLE001
        pass
    print("DONE", json.dumps(stats), flush=True)
    (out / "fuzz.json").write_text(json.dumps({"stats": stats, "findings": findings}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
