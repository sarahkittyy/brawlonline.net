"""Analysis helper for docs/gameplay-rollback-feasibility.md, "late divergence".

Loads the two lockstep fixtures saved by
``gameplay_rollback.py play --save-at 8420`` (A and B at the same game frame, RNG and frame
counters synced at the simulation start, identical inputs so far), re-issues the closed-loop
inputs that the original 9,000-frame run recorded, steps both one game frame at a time, and prints
every 32-byte granule of the gameplay heaps that newly differs, with the containing allocator
block, the block's first words (vtable), and both values.

    python harness/tools/gprb_late_divergence.py --dir run/qa/gprb/ps2 \
        --inputs run/qa/gprb/ps2/play-AB-sc-long.json --from 8420 --to 8460
"""

from __future__ import annotations

import argparse
import bisect
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gameplay_rollback as G  # noqa: E402

PAD_STATUS = 0x805BAD00   # gfPadStatus[4], 0x40 bytes each (protocol doc, rollback_pad_history)
HEAPS = ("System", "Effect", "Fighter1Instance", "Fighter2Instance", "StageInstance", "ItemInstance", "Physics",
         "InfoInstance", "GameGlobal", "GlobalMode", "StageResoruce", "Fighter1Resoruce", "Fighter2Resoruce")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--inputs", required=True)
    ap.add_argument("--from", dest="start", type=int, default=8420)
    ap.add_argument("--to", type=int, default=8460)
    ap.add_argument("--show", type=int, default=6)
    ap.add_argument("--statics", action="store_true", help="also DOL .data/.bss/.sdata/.sbss and REL data/bss")
    ap.add_argument("--churn", type=int, default=20, help="frames used to learn churning granules (with --statics)")
    args = ap.parse_args()
    d = Path(args.dir)
    rec = json.loads(Path(args.inputs).read_text())[0]["inputs"]
    f0 = args.start
    with G.instances([("gprb-late0", dict(video="Null")), ("gprb-late1", dict(video="Null"))]) as insts:
        cs = [i.client for i in insts]
        for i, c in enumerate(cs):
            G.load_fixture(c, d / f"lockstep-{f0}-{i}.sav")
        p0 = G.par([lambda c=c: c.status().input_polls for c in cs])
        # Scripts still running at the save point, cut to what is left (original start: frame f+2).
        for f, port, seq in rec:
            if f > f0:
                continue
            t = f + 2
            rest = []
            for x in seq:
                h = int(x["hold"])
                if t + h > f0 + 1:
                    skip = max(0, f0 + 1 - t)
                    rest.append(dict(x, hold=h - skip))
                t += h
            if rest:
                for c, p in zip(cs, p0):
                    c.pad_script(int(port), rest, start=p + 1)
        heaps = {h["name"]: h for h in G.heap_dicts(cs[0])}
        ranges = [(heaps[n]["start"], heaps[n]["end"] - heaps[n]["start"], n) for n in HEAPS if n in heaps]
        if args.statics:
            # DOL writable sections and REL data/bss too. Granules that churn (newly differ in the
            # first --churn frames) are reported once as churn and then ignored.
            ranges += [(a, e - a, n) for n, a, e in G.DOL_SECTIONS if n in G.CAND_DOL]
            ranges += [(a, n, lbl) for a, n, lbl in G._rel_ranges(G.read_modules(cs[0].read_mem))]
        churn: dict = {}
        prev = None
        for f in range(f0 + 1, args.to + 1):
            for port in (0, 1):
                for g, p, seq in rec:
                    if g == f - 1 and int(p) == port:     # decided at frame g, starts at g+2 = f+1
                        for c, pp in zip(cs, p0):
                            c.pad_script(port, seq, start=pp + (g + 2 - f0))
            G.par([lambda c=c, p=p: G._step_to(c, p + (f - f0)) for c, p in zip(cs, p0)])
            sm = G.par([lambda c=c: G.small_state(c) for c in cs])
            dumps = [{a: G.read_big(c, a, n) for a, n, _ in ranges} for c in cs]
            cur = {}
            for a, n, name in ranges:
                cur[(a, name)] = set(o for o, _ in G.diff_runs(dumps[0][a], dumps[1][a], gran=32, merge_gap=0))
            obs = G.state_diff(sm[0], sm[1], ("rng", "players"))
            # The pad status the game actually used this frame (gfPadStatus slots, 0x40 per port).
            pads = G.par([lambda c=c: c.read_mem(PAD_STATUS, 0x80).hex() for c in cs])
            st = G.par([lambda c=c: (c.status().input_polls, G.game_frame(c)) for c in cs])
            line = [f"frame {f}: observables {'DIFFER ' + json.dumps(obs)[:200] if obs else 'equal'}; "
                    f"pads {'equal' if pads[0] == pads[1] else 'DIFFER A=' + pads[0][:48] + ' B=' + pads[1][:48]}; "
                    f"poll-p0/gameframe A {st[0][0] - p0[0]}/{st[0][1]} B {st[1][0] - p0[1]}/{st[1][1]}"]
            if prev is not None:
                for a, n, name in ranges:
                    new = sorted(cur[(a, name)] - prev[(a, name)])
                    if args.statics and f - f0 <= args.churn:
                        churn.setdefault((a, name), set()).update(new)
                        continue
                    new = [o for o in new if o not in churn.get((a, name), set())]
                    if not new:
                        continue
                    blocks, _ = G.heap_blocks({a: dumps[0][a]}, a, a + n)
                    starts = [b[0] for b in blocks or []]
                    line.append(f"  {name}: {len(new)} new granules")
                    for o in new[:args.show]:
                        ad = a + o
                        i = bisect.bisect_right(starts, ad) - 1
                        blk = blocks[i] if blocks and i >= 0 else (a, n, False)
                        hdr = dumps[0][a][blk[0] - a + 0x20: blk[0] - a + 0x30].hex()
                        line.append(f"    {ad:#010x} block {blk[0]:#x}+{ad - blk[0]:#x} size {blk[1]:#x} free={blk[2]} "
                                    f"head={hdr} A={dumps[0][a][o:o + 32].hex()} B={dumps[1][a][o:o + 32].hex()}")
            print("\n".join(line), flush=True)
            prev = cur
    return 0


if __name__ == "__main__":
    sys.exit(main())
