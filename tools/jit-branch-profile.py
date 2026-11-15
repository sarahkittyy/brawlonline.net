#!/usr/bin/env python3
"""Makes Dolphin's branch profile (dolphin/Data/Sys/Rollback/jit-branches.txt) from recordings.

A recording is what Dolphin writes at exit when it ran with PPR_JIT_BRANCH_RECORD=<file>: one line
per conditional branch the JIT compiled, "address instruction taken not_taken" (hex, hex, counts).
Record a few sessions that cover what players do (menus, several characters and stages), then:

    tools/jit-branch-profile.py rec1.txt rec2.txt ... -o dolphin/Data/Sys/Rollback/jit-branches.txt

The profile lists the branches taken at least --percent of the time over all recordings, among
those that ran at least --min-count times: "address instruction", sorted by address.
"""
import argparse
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recordings", nargs="+")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--percent", type=float, default=90.0)
    ap.add_argument("--min-count", type=int, default=64)
    args = ap.parse_args()

    counts = {}  # (address, instruction) -> [taken, not taken]
    for path in args.recordings:
        with open(path) as f:
            for line in f:
                parts = line.split()
                if line.startswith("#") or len(parts) != 4:
                    continue
                key = (int(parts[0], 16), int(parts[1], 16))
                entry = counts.setdefault(key, [0, 0])
                entry[0] += int(parts[2])
                entry[1] += int(parts[3])

    followed = sorted(key for key, (taken, not_taken) in counts.items()
                      if taken + not_taken >= args.min_count
                      and taken * 100 >= (taken + not_taken) * args.percent)
    # One instruction per address: a module loaded where another was keeps the busier one.
    best = {}
    for address, instruction in followed:
        if address not in best or sum(counts[(address, instruction)]) > sum(counts[(address, best[address])]):
            best[address] = instruction
    with open(args.output, "w") as f:
        f.write("# Conditional branches that are taken nearly every time (Core/PowerPC/BranchProfile.h):\n")
        f.write("# address instruction. Made by tools/jit-branch-profile.py from %d recordings\n" % len(args.recordings))
        f.write("# (taken >= %g%% of >= %d runs).\n" % (args.percent, args.min_count))
        for address in sorted(best):
            f.write("%08x %08x\n" % (address, best[address]))
    print("%d branches of %d recorded -> %s" % (len(best), len(counts), args.output), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
