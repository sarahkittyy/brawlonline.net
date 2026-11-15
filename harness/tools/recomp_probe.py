"""Two EmissaryRecomp instances into a Direct match (test_online_game's own steps), then watch
both every second without tearing them down: scene, pad polls, the session's phase. For finding
where a recomp stops (docs/design/brawl-online-core.md §10). Needs PPHARNESS_RECOMP and the
usual PPHARNESS_* settings (E:/emissary/scripts/recomp_test.ps1 sets them).

    python tools/recomp_probe.py [seconds]
"""

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))

import test_online_game as T  # noqa: E402
drive = T.drive
from ppharness.recomp import RecompInstance  # noqa: E402


def main() -> None:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 90
    be = T._make_backend()
    made = []

    from ppharness import paths
    from ppharness.instance import DolphinInstance
    from ppharness.recomp import wants_recomp

    def factory(name=None, **kw):
        kw["keep"] = True
        full = name or "probe"
        if wants_recomp(full):
            kw.pop("exe", None)
            inst = RecompInstance(full, **kw)
        else:
            kw.setdefault("exe", paths.dolphin_nogui())
            inst = DolphinInstance(full, **kw)
        made.append(inst)
        return inst

    try:
        a, b, ua, ub = T._connected_direct(be, factory, "D3D11", "recomp-probe", ("iris", "jack"),
                                           pick_game_1=False)
        for g in (a, b):
            pads = g.c.read_mem(0x805BACC0, 0x40)
            print(f"{g.name} ({type(g.inst).__name__}) gfPadSystem 805BACC0: {pads.hex()}", flush=True)
        import os
        for spec in filter(None, os.environ.get("PROBE_DUMP", "").split(",")):
            addr, n = (int(x, 16) for x in spec.split(":"))
            for g in (a, b):
                Path(os.environ.get("PROBE_DUMP_DIR", "."), f"{g.name}-{addr:08x}.bin").write_bytes(
                    g.c.read_mem(addr, n))
        if os.environ.get("PROBE_NO_PICK"):
            return
        T.online_set.random_pick_where_anyone_picks([a, b], 1)
        print("connected; watching", flush=True)
        t0 = time.monotonic()
        while time.monotonic() - t0 < seconds:
            row = []
            for g in (a, b):
                try:
                    st = g.c.status()
                    gs = g.c.call("gprb_status")
                    sc = drive.scene(g.c)
                    row.append(f"{g.name}: polls {st.input_polls} scene {sc.name} phase {gs.get('phase')} "
                               f"frame {gs.get('current_frame', gs.get('frame'))}")
                except Exception as e:  # noqa: BLE001
                    row.append(f"{g.name}: {type(e).__name__}: {e}"[:160])
            print(f"{time.monotonic() - t0:6.1f}s  " + " | ".join(row), flush=True)
            for inst in made:
                if inst.process and inst.process.poll() is not None and not getattr(inst, '_reported', False):
                    inst._reported = True
                    print(f'  {inst.name} exited with {hex(inst.process.poll() & 0xFFFFFFFF)}', flush=True)
            time.sleep(1)
    finally:
        for inst in made:
            code = inst.process.poll() if inst.process else None
            print(f"{inst.name}: running={inst.is_running()} exit={code} ({hex(code & 0xFFFFFFFF) if code is not None else None}) dir={inst.user_dir}", flush=True)
        be.stop()


if __name__ == "__main__":
    main()
