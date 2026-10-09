"""Compare whole-memory dumps of gameplay sessions (PPR_GPRB_DUMP_FULL=1 with PPR_GPRB_DUMP_FRAMES
and PPR_GPRB_DUMP_DIR: m1-f<frame>-<n>.bin, m2-f<frame>-<n>.bin and ranges.txt per save).

    gprb_memdiff.py diff A B F1,F2,... [--noise N1 N2] [--inset] [--max N]
        64-byte granules that differ between two runs at the same session frames, outside the
        region set (with --inset: inside too); --noise drops granules that also differ between two
        identical runs.
    gprb_memdiff.py dol A B FRAME
        DOL data objects (symbols.txt) outside the set that differ between two runs.
    gprb_memdiff.py scan DIR F1,F2,... [--extra ADDR:SIZE ...]
        DOL data words outside the set that point into the set's heaps and change during the match
        (one run, dumps at several frames): the heads of global lists whose nodes are rolled back.

Typical use (docs/gameplay-rollback-status.md, Phase 6): replay one peer's session twice from its
pass log, once as recorded and once with one rollback removed (gprb_passlog.py), with dumps around
the frame, and diff them.
"""

from __future__ import annotations

import argparse
import bisect
import glob
import os
import re
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gprb_debug import SYMBOLS, sym  # noqa: E402

M1, M2 = 0x80000000, 0x90000000
DOL_DATA = (0x80406800, 0x805B5160)


def dump(d: str, kind: str, frame: int, save: int = -1) -> np.ndarray:
    files = sorted(glob.glob(os.path.join(d, f"{kind}-f{frame}-*.bin")),
                   key=lambda p: int(p.rsplit("-", 1)[1].split(".")[0]))
    return np.fromfile(files[save], dtype=np.uint8)


def ranges(d: str, extra=()):
    out = []
    for line in open(os.path.join(d, "ranges.txt")):
        p = line.split(None, 2)
        out.append((int(p[0], 16), int(p[1], 16), p[2].strip() if len(p) > 2 else ""))
    out += list(extra)
    return out


def granule_mask(rs, base: int, size: int) -> np.ndarray:
    m = np.zeros(size >> 6, bool)
    for a, n, _ in rs:
        if base <= a < base + size:
            m[(a - base) >> 6:(a - base + n + 63) >> 6] = True
    return m


def data_objects():
    objs = []
    for line in open(SYMBOLS, encoding="utf-8", errors="replace"):
        m = re.match(r"(\S+) = \.(\w+):0x([0-9A-Fa-f]+); // type:object size:0x([0-9A-Fa-f]+)", line)
        if m and m.group(2) in ("bss", "sbss", "sdata", "data"):
            objs.append((int(m.group(3), 16), int(m.group(4), 16), m.group(1), m.group(2)))
    objs.sort()
    return objs, [o[0] for o in objs]


def obj_at(objs, starts, a):
    k = bisect.bisect_right(starts, a) - 1
    return objs[k] if k >= 0 and objs[k][0] <= a < objs[k][0] + objs[k][1] else None


def cmd_diff(args) -> None:
    frames = [int(f) for f in args.frames.split(",")]
    rs = ranges(args.a)
    noise = {}
    if args.noise:
        for f in frames:
            for kind, base in (("m1", M1), ("m2", M2)):
                x, y = dump(args.noise[0], kind, f), dump(args.noise[1], kind, f)
                g = (x != y).reshape(-1, 64).any(axis=1)
                noise[base] = g if base not in noise else noise[base] | g
    for f in frames:
        print(f"===== frame {f}")
        for kind, base in (("m1", M1), ("m2", M2)):
            x, y = dump(args.a, kind, f), dump(args.b, kind, f)
            g = (x != y).reshape(-1, 64).any(axis=1)
            inset = granule_mask(rs, base, len(x))
            n_in = int((g & inset).sum())
            if args.only_inset:
                g &= inset
            elif not args.inset:
                g &= ~inset
            if base in noise:
                g &= ~noise[base]
            idx = np.nonzero(g)[0]
            print(f"-- {base:08x}: {len(idx)} granules differ{'' if args.inset else ' outside the set'}, {n_in} inside")
            runs = []
            for i in idx:
                if runs and runs[-1][1] == i:
                    runs[-1][1] = i + 1
                else:
                    runs.append([int(i), int(i) + 1])
            for s, e in runs[:args.max]:
                o = s * 64
                a, b = x[o:o + 64], y[o:o + 64]
                dpos = int(np.nonzero(a != b)[0][0]) & ~3
                tag = sym(base + o) if base == M1 and base + o < DOL_DATA[1] else ""
                print(f"  {base + o:08x}+{(e - s) * 64:x} {tag:36s} @{base + o + dpos:08x} "
                      f"{a[dpos:dpos + 16].tobytes().hex()} | {b[dpos:dpos + 16].tobytes().hex()}")


def cmd_dol(args) -> None:
    objs, starts = data_objects()
    x, y = dump(args.a, "m1", args.frame), dump(args.b, "m1", args.frame)
    inset = np.repeat(granule_mask(ranges(args.a), M1, len(x)), 64)
    seen = {}
    for i in np.nonzero((x != y) & ~inset)[0]:
        a = M1 + int(i)
        if not DOL_DATA[0] <= a < DOL_DATA[1]:
            continue
        o = obj_at(objs, starts, a)
        key = o[2] if o else f"?{a & ~63:08x}"
        seen.setdefault(key, [o, 0, a])[1] += 1
    for key, (o, n, a) in seen.items():
        start, size = (o[0], o[1]) if o else (a & ~63, 64)
        show = min(size, 32)
        p = start - M1
        print(f"{key:28s} {o[3] if o else '':5s} {start:08x}+{size:x} {n:5d} B  "
              f"{x[p:p + show].tobytes().hex()} | {y[p:p + show].tobytes().hex()}")


def cmd_scan(args) -> None:
    objs, starts = data_objects()
    extra = []
    for e in args.extra or []:
        a, n = e.split(":")
        extra.append((int(a, 16), int(n, 16), "extra"))
    rs = ranges(args.dir, extra)
    heaps = [(a, n, lab) for a, n, lab in rs if n >= 0x10000]
    frames = args.frames.split(",")
    mems = [dump(args.dir, "m1", int(f)).tobytes() for f in frames]
    inset = np.repeat(granule_mask(rs, M1, len(mems[0])), 64)

    def heap_of(v):
        for s, n, lab in heaps:
            if s <= v < s + n:
                return lab
        return None

    out = {}
    for a in range(DOL_DATA[0], DOL_DATA[1], 4):
        if inset[a - M1]:
            continue
        vals = [struct.unpack_from(">I", m, a - M1)[0] for m in mems]
        if len(set(vals)) == 1:
            continue
        hs = [heap_of(v) for v in vals]
        if not any(hs):
            continue
        o = obj_at(objs, starts, a)
        key = f"{o[2]} {o[3]} {o[0]:08x}+{o[1]:x}" if o else f"? {a & ~0x3F:08x}"
        out.setdefault(key, []).append((a, vals, hs))
    for key, rows in out.items():
        a, vals, hs = rows[0]
        print(f"{key}: {len(rows)} words, e.g. {a:08x} {' '.join('%08x' % v for v in vals)}  "
              f"[{','.join(sorted(set(h for h in hs if h)))}]")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("diff")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("frames")
    p.add_argument("--noise", nargs=2)
    p.add_argument("--inset", action="store_true")
    p.add_argument("--only-inset", action="store_true", help="only granules inside the set")
    p.add_argument("--max", type=int, default=300)
    p = sub.add_parser("dol")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("frame", type=int)
    p = sub.add_parser("scan")
    p.add_argument("dir")
    p.add_argument("frames")
    p.add_argument("--extra", nargs="*")
    args = ap.parse_args()
    {"diff": cmd_diff, "dol": cmd_dol, "scan": cmd_scan}[args.cmd](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
