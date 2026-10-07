#!/usr/bin/env python3
"""Drive a running harness instance through menus with a tiny step language.

    python tools/gamecode/drive.py --port P scene
    python tools/gamecode/drive.py --port P "wait 120" "tap A" "hold B 40" "until muMenuMain" "shot out.png"

Steps (each one argument):
    scene                    print the current scene / sequence
    wait N                   wait N game frames (input polls)
    tap BTN[+BTN] [N]        press for N frames (default 3), then release for 6
    hold BTN[+BTN] N         hold for N frames, then release
    stick DIR N              main stick up/down/left/right for N frames, then neutral 6
    until SCENE [MAXFRAMES]  wait until the scene name (or sequence name) matches
    shot PATH                screenshot (needs D3D11/Vulkan)
    mem ADDR LEN             hex dump
    u32 ADDR                 read a u32
    w32 ADDR VALUE           write a u32 (debug only)
    port N                   which controller port the following input steps use (default 0)
    mbx-serve MAXFRAMES [STATE [NAME [CODE]]]
                             PPOM mailbox: wait up to MAXFRAMES for game requests and answer
                             each (FIND_OPPONENT -> GET_MATCH_STATE with mmState STATE, default 4;
                             GET_ONLINE_STATUS -> logged in). Stops after the first FIND_OPPONENT.
    mbx-dump                 print the mailbox state
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from ppharness import brawl  # noqa: E402
from ppharness.client import HarnessClient, PadInput  # noqa: E402

STICK = {"up": (128, 255), "down": (128, 0), "left": (0, 128), "right": (255, 128)}


def polls(c: HarnessClient) -> int:
    return c.status().input_polls


def wait(c: HarnessClient, n: int) -> None:
    c.wait_frame(input_polls=polls(c) + n, timeout_ms=max(30000, n * 100))


def scene(c: HarnessClient) -> brawl.SceneInfo:
    return brawl.read_scene(c.read_mem)


SCENARIOS = Path(__file__).resolve().parent / "scenarios"


def expand(steps: list[str], out: str = "run/artifacts/game-code/screens") -> list[str]:
    """`@file.txt` includes a scenario file (tools/gamecode/scenarios); `{out}` is replaced."""
    res: list[str] = []
    for s in steps:
        s = s.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("@"):
            p = Path(s[1:])
            if not p.exists():
                p = SCENARIOS / s[1:]
            res += expand(p.read_text().splitlines(), out)
        else:
            res.append(s.replace("{out}", out))
    return res


def run(c: HarnessClient, steps: list[str], port: int = 0, out: str = "run/artifacts/game-code/screens") -> None:
    c.wait_state("running", timeout=120)
    for s in expand(steps, out):
        print(f"> {s}", flush=True) if s.startswith(("shot", "until", "mbx")) else None
        a = s.split()
        op = a[0]
        if op == "scene":
            si = scene(c)
            print(f"scene={si.name} seq={si.sequence} next={si.next_name} polls={polls(c)}")
        elif op == "wait":
            wait(c, int(a[1]))
        elif op == "port":
            port = int(a[1])
        elif op in ("tap", "hold"):
            btns = a[1].split("+")
            n = int(a[2]) if len(a) > 2 else 3
            c.pad_script(port, [PadInput(buttons=btns, hold=n), PadInput(hold=6)])
            wait(c, n + 8)
        elif op == "stick":
            n = int(a[2])
            c.pad_script(port, [PadInput(main=STICK[a[1]], hold=n), PadInput(hold=6)])
            wait(c, n + 8)
        elif op == "until":
            want = a[1]
            mx = int(a[2]) if len(a) > 2 else 3600
            start = polls(c)
            while True:
                si = scene(c)
                if want in (si.name, si.sequence):
                    break
                if polls(c) - start > mx:
                    raise SystemExit(f"until {want}: timeout (scene={si.name} seq={si.sequence})")
                wait(c, 10)
        elif op == "shot":
            Path(a[1]).parent.mkdir(parents=True, exist_ok=True)
            c.screenshot(str(Path(a[1]).resolve()))
            print(f"shot {a[1]}")
        elif op == "mem":
            addr, ln = int(a[1], 16), int(a[2], 0)
            b = c.read_mem(addr, ln)
            for i in range(0, len(b), 16):
                print(f"{addr + i:08X}: {b[i:i + 16].hex(' ')}")
        elif op == "u32":
            print(hex(c.read_u32(int(a[1], 16))))
        elif op == "w32":
            c.write_mem(int(a[1], 16), int(a[2], 16).to_bytes(4, "big"))
        elif op == "mbx-serve":
            import ppom
            b = ppom.find_block(c)
            mx = int(a[1])
            state = int(a[2]) if len(a) > 2 else 4
            name = a[3] if len(a) > 3 else "Opponent"
            code = a[4] if len(a) > 4 else ""
            start = polls(c)
            done = False
            while not done and polls(c) - start <= mx:
                (wr, rd, _rc, _rs), reqs = ppom.read_requests(c, b)
                for seq, cmd, pl in sorted(reqs):
                    if not seq or seq <= rd:
                        continue
                    print(f"mailbox request seq={seq}: {ppom.describe_request(cmd, pl)}")
                    if cmd == 0xB4:
                        want = ppom.from_u16s(pl[4:4 + 2 * ppom.CODE_LEN])
                        ppom.write_response(c, b, seq, 0xB3, ppom.match_state_payload(
                            state, name, code or want or "OPPO#123", role=1))
                        print(f"  answered GET_MATCH_STATE mmState={state} peer={name} ({code or want})")
                        done = True
                    elif cmd == 0xB9:
                        import struct as _st
                        ppom.write_response(c, b, seq, 0xB9, _st.pack(">BB", 1, 0)
                                            + ppom.u16s("Sarah", ppom.NAME_LEN) + ppom.u16s("SARA#001", ppom.CODE_LEN))
                        print("  answered GET_ONLINE_STATUS: Sarah (SARA#001)")
                    ppom.consume(c, b, seq)
                if not done:
                    wait(c, 5)
            if not done:
                print("mbx-serve: no FIND_OPPONENT request")
        elif op == "mbx-dump":
            import ppom
            b = ppom.find_block(c)
            (wr, rd, rc, rs), reqs = ppom.read_requests(c, b)
            print(f"mailbox reqWrite={wr} reqRead={rd} respCount={rc} respSeen={rs}")
            for seq, cmd, pl in reqs:
                if seq:
                    print(f"  req seq={seq}: {ppom.describe_request(cmd, pl)}")
        elif op == "sleep":
            time.sleep(float(a[1]))
        else:
            raise SystemExit(f"unknown step {s!r}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True, help="harness port")
    ap.add_argument("--out", default="run/artifacts/game-code/screens", help="{out} in scenario files")
    ap.add_argument("steps", nargs="+")
    a = ap.parse_args()
    c = HarnessClient.connect(a.port, timeout=30)
    try:
        run(c, a.steps, out=a.out)
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
