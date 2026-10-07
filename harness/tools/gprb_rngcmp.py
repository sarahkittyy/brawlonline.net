"""Compare two peers' mtRand call logs (PPR_GPRB_RNG_LOG): for each session frame, the calls of
its newest run (pass), and print the first frame where they differ.

    rngcmp.py A.log A_from A_to B.log B_from B_to limit_frame
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gprb_debug import sym  # noqa: E402

RX = re.compile(r"gprb rng: frame (-?\d+) (first|again) pass (\d+) rng ([0-9a-f]+) lr ([0-9a-f]+) state ([0-9a-f]+)(?: up ([0-9a-f ]+))?")


def load(path, start_line, end_line):
    runs = {}   # frame -> (pass, calls) of the newest pass
    with open(path, "rb") as fh:
        for n, raw in enumerate(fh, 1):
            if n < start_line or n > end_line:
                continue
            m = RX.search(raw.decode("utf-8", "replace"))
            if not m:
                continue
            f, ps = int(m.group(1)), int(m.group(3))
            cur = runs.get(f)
            if cur is None or cur[0] != ps:
                if cur is not None and ps < cur[0]:
                    continue
                cur = (ps, [])
                runs[f] = cur
            up = tuple(int(x, 16) for x in m.group(7).split()) if m.group(7) else (0, 0)
            cur[1].append((m.group(4), int(m.group(5), 16), m.group(6), up))
    return {f: v[1] for f, v in runs.items()}


a = load(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
b = load(sys.argv[4], int(sys.argv[5]), int(sys.argv[6]))
limit = int(sys.argv[7])
common = sorted(set(a) & set(b))
print("frames with calls:", len(a), len(b), "common", len(common))
for f in common:
    if f > limit:
        print("no difference up to", limit)
        break
    if a[f] != b[f]:
        print("first differing frame", f, len(a[f]), len(b[f]))
        for i in range(max(len(a[f]), len(b[f]))):
            x = a[f][i] if i < len(a[f]) else None
            y = b[f][i] if i < len(b[f]) else None
            mark = "  " if x == y else "**"
            fx = (f"{x[1]:08x} {x[2]} <- " + " <- ".join(f"{u:08x}" for u in x[3])) if x else "-"
            fy = (f"{y[1]:08x} {y[2]} <- " + " <- ".join(f"{u:08x}" for u in y[3])) if y else "-"
            print(mark, fx, "|", fy)
        for g in range(f - 3, f):
            print("frame", g, len(a.get(g, [])), len(b.get(g, [])))
        break
