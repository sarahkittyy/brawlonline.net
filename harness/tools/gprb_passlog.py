"""Read and edit gameplay-session pass logs (PPR_GPRB_PASS_LOG, gprb_session.py --pass-log).

A pass log holds every GekkoNet update of one peer's session: the load depth, the initial-save flag
and every pass (frame, save flag, 0x100 bytes of input slots). ``gprb_mispredict.py run
--state <peer's countdown savestate> --modes replay=<log>`` runs that peer's session again, exactly.
The edits below make variants of a log, to find which rollback of a session changed its outcome.

    gprb_passlog.py show LOG LO HI          updates touching frames LO..HI: load depth and frames;
                                            '!' marks a pass whose input differs from the frame's
                                            final (confirmed) input
    gprb_passlog.py fix LOG OUT F1,F2,...   every pass of those frames gets the final input (the
                                            rollbacks stay, the mispredictions go)
    gprb_passlog.py norb LOG OUT U1,U2,...  those updates (by index) neither load nor resimulate:
                                            only their last pass runs
    gprb_passlog.py flat LOG OUT            no rollbacks at all: every frame once, final input
                                            (the session's ground truth)
"""

from __future__ import annotations

import struct
import sys
from typing import Dict, List, Tuple

HEADER = 4 + 0x18 + 12 + 4  # 'GPRH', game frame block, mtRand x3, serial
UPDATE_MAGIC = 0x55525047   # 'GPRU' (little endian)
SLOTS = 0x100

Pass = List  # [frame, save_after, slots]
Update = List  # [load_back, initial_save, [Pass]]


def read(path: str) -> Tuple[bytes, List[Update]]:
    b = open(path, "rb").read()
    ups: List[Update] = []
    o = HEADER
    while o + 10 <= len(b):
        _magic, load_back = struct.unpack_from("<Ii", b, o)
        initial, n = b[o + 8], b[o + 9]
        o += 10
        passes = []
        for _ in range(n):
            frame = struct.unpack_from("<i", b, o)[0]
            passes.append([frame, b[o + 4], b[o + 5:o + 5 + SLOTS]])
            o += 5 + SLOTS
        ups.append([load_back, initial, passes])
    return b[:HEADER], ups


def write(path: str, header: bytes, ups: List[Update]) -> None:
    out = bytearray(header)
    for load_back, initial, passes in ups:
        out += struct.pack("<Ii", UPDATE_MAGIC, load_back) + bytes([initial, len(passes)])
        for frame, save, slots in passes:
            out += struct.pack("<i", frame) + bytes([save]) + slots
    open(path, "wb").write(out)


def final_inputs(ups: List[Update]) -> Dict[int, bytes]:
    last: Dict[int, bytes] = {}
    for _, _, passes in ups:
        for frame, _, slots in passes:
            last[frame] = slots
    return last


def main(argv: List[str]) -> int:
    cmd, path = argv[0], argv[1]
    header, ups = read(path)
    last = final_inputs(ups)
    if cmd == "show":
        lo, hi = int(argv[2]), int(argv[3])
        for i, (lb, _, passes) in enumerate(ups):
            frames = [p[0] for p in passes]
            if frames and frames[-1] >= lo and frames[0] <= hi:
                marks = [f"{p[0]}{'!' if p[2] != last[p[0]] else ''}" for p in passes]
                print(f"update {i}: load {lb} {' '.join(marks)}")
    elif cmd == "fix":
        frames = {int(x) for x in argv[3].split(",")}
        n = 0
        for _, _, passes in ups:
            for p in passes:
                if p[0] in frames and p[2] != last[p[0]]:
                    p[2] = last[p[0]]
                    n += 1
        write(argv[2], header, ups)
        print("passes fixed:", n)
    elif cmd == "norb":
        for i in (int(x) for x in argv[3].split(",")):
            ups[i][0] = 0
            ups[i][2] = ups[i][2][-1:]
        write(argv[2], header, ups)
    elif cmd == "flat":
        for u in ups:
            u[0] = 0
            u[2] = [[p[0], p[1], last[p[0]]] for p in u[2][-1:]]
        write(argv[2], header, ups)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
