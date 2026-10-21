"""Bug 70 (XTRM#355, 2026-10-20: "against players over 100 ping my L button feels unresponsive
half the time ... full hopping when I try to wavedash"): the local controller's samples reach the
game one frame apart, rollbacks or not (Gprb::ReadHostPad).

Player A's controller carries a real-time counter: 8 buttons (A B X Y Z DUP DDOWN DLEFT) hold
int(t * 59.94) mod 256 (the harness's pad_counter: Dolphin works it out from its own clock at
every read, so the counter itself has no jitter; what a real controller does: the game samples
whatever is held when it reads the pad). B's stick moves every frame or two (a real stick's noise) so that A rolls back. A's replay then says what the game ran with on every frame for A's port: the counter
should go up by 1 per frame. A step of 2+ is a sample the game never saw (a press that short is
lost), 0 a sample used twice, a step back an old sample used as new.

`lan`: no netsim (few rollbacks). `rtt130`: 57 ms each way (about 127 ms in game, auto delay 2, as in the report).
Before the fix, rtt130 had 12% of the steps 0 and 12% 2 (a sample read a re-run's length late after
each rollback); LAN 0.1%. Both must now have at least 98% of the steps 1.
"""

from __future__ import annotations

import collections
import random
import shutil
import threading
import time
from pathlib import Path
from typing import Callable

import pytest

from ppharness import replay as R
from ppharness.instance import DolphinInstance
from ppharness.netsim import LinkProfile

from test_online_game import ROOT, _connected_direct, _p2p_through_netsim, backend, online_set  # noqa: F401

BITS = [("A", 0x0100), ("B", 0x0200), ("X", 0x0400), ("Y", 0x0800), ("Z", 0x0010),
        ("DUP", 0x0008), ("DDOWN", 0x0004), ("DLEFT", 0x0001)]
FPS = 60.0 / 1.001
OUT = ROOT / "run" / "artifacts" / "bug70"


def _decode(word: int) -> int:
    return sum(1 << i for i, (_, mask) in enumerate(BITS) if word & mask)


@pytest.mark.slow
@pytest.mark.parametrize("preset", ["lan", "rtt130"])
def test_local_samples_reach_the_game_in_order(backend, dolphin: Callable[..., DolphinInstance],  # noqa: F811
                                               gpu_backend: str, preset: str) -> None:
    sims, inis = (_p2p_through_netsim(LinkProfile(latency_ms=57, jitter_ms=1.0)) if preset != "lan"
                  else ([], (None, None)))
    try:
        a, b, _, _ = _connected_direct(backend, dolphin, gpu_backend, f"bug70-{preset}", ("bsev", "bsfo"),
                                       stocks=4, inst_names=(f"b70-{preset}-a", f"b70-{preset}-b"),
                                       dolphin_ini=inis)
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                        "the match running on both")
        st = online_set.gstatus(a.c)
        print(f"A: delay {st.get('input_delay')}", flush=True)
        stop = threading.Event()

        # Dolphin pad 0 plays the local port.
        a.c.pad_counter(0, [name for name, _ in BITS], rate=FPS)

        def drive_b() -> None:
            # A real stick's noise changes B's input on most frames, so A mispredicts B and rolls
            # back about as often as in the report (44-64% of frames).
            rng = random.Random(70)
            while not stop.is_set():
                b.c.pad_set(0, main=(rng.randint(60, 196), rng.randint(110, 146)),
                            buttons=["B"] if rng.random() < 0.1 else [])
                time.sleep(rng.choice((0.017, 0.034)))

        th = [threading.Thread(target=drive_b)]
        for t in th:
            t.start()
        try:
            time.sleep(60)
        finally:
            stop.set()
            for t in th:
                t.join(timeout=120)
        rec = a.c.call("replay_recording", flush=True)
        st = online_set.gstatus(a.c)
        print({k: st.get(k) for k in ("phase", "input_delay", "rollbacks", "current_frame")}, flush=True)
        for g in (a, b):
            g.c.mm_cancel()
        online_set.wait(lambda: not a.c.call("replay_recording")["recording"], 60, "A's recording to end")
        rep = R.read(Path(rec["path"]))
        OUT.mkdir(parents=True, exist_ok=True)
        shutil.copy2(rep.path, OUT / f"{preset}-{rep.path.name}")

        # A's in-game port: the one whose buttons carry the counter.
        def word(f: int, port: int) -> int:
            return int.from_bytes(rep.pad(f, port)[0:4], "big")

        frames = list(range(rep.first_frame, rep.last_frame + 1))
        port = max(range(R.PORTS), key=lambda p: len({word(f, p) for f in frames}))
        vals = [(f, _decode(word(f, port)), rep.pad(f, port)[0x38]) for f in frames]
        vals = [v for v in vals if v[1] != 0 or v[0] > frames[0] + 600]   # from the counter's first value
        steps = collections.Counter((v2 - v1) % 256 for (_, v1, _), (_, v2, _) in zip(vals, vals[1:]))
        errors = collections.Counter(e for _, _, e in vals)
        stray = collections.Counter(word(f, port) & ~sum(m for _, m in BITS) & 0xFFFF for f in frames)
        print(f"{preset}: port {port}, {len(vals)} frames; steps {sorted(steps.items())}; pad error byte "
              f"{dict(errors)}; other button bits {dict(stray)}", flush=True)
        back = [(f1, v1, v2) for (f1, v1, _), (_, v2, _) in zip(vals, vals[1:]) if 128 <= (v2 - v1) % 256]
        print(f"steps back: {len(back)} {back[:10]}", flush=True)
        n = sum(steps.values())
        ones = steps.get(1, 0)
        lost = sum(c for d, c in steps.items() if 2 <= d < 128)
        print(f"{preset}: step 1 on {100 * ones / n:.2f}% of {n} steps, 0 on {100 * steps.get(0, 0) / n:.2f}%, "
              f"2+ on {100 * lost / n:.2f}%", flush=True)
        log = (a.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
        lines = [ln for ln in log.splitlines() if "gprb net: frames" in ln or "gprb: input delay" in ln]
        for line in lines[:1] + lines[-3:]:
            print(line[:500], flush=True)
            if "pad reads" in line:
                print("  ..." + line[line.index("pad sample age"):], flush=True)
        assert n > 1000, "too few frames"
        assert ones >= 0.98 * n, f"step 1 on only {ones} of {n} frames: {sorted(steps.items())}"
    finally:
        for sim in sims:
            print(sim.format_stats())
            sim.stop()
