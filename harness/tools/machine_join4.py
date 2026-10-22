"""Players joining a whole-machine session (Core/Rollback/MachineSession.cpp) one after another, up to four
(bug 90, 2026-10-22: in NSMB Wii rooms a third or fourth player was refused, "player 2 does not vouch for
this machine", always naming a player who had joined with an image before; machine_join3.py stops at three).

The host starts alone; past ``--start-frame`` the first joiner starts and joins; ``--gap`` seconds after
each joiner is in, the next one starts. ``--controller wiimote`` plays emulated Wii Remotes, as NSMB Wii
rooms do (no input then; PPR_MACHINE_TEST_INPUT drives only a GameCube pad). Reported: who got in, every
epoch's digests and checksums, refusals, and the desyncs on each machine. ``--dump`` keeps each epoch's
image parts per player (PPR_MACHINE_DUMP_DIR), to compare a refused image with the refusing player's.

    python harness/tools/machine_join4.py run/scratch/join4/a --iso "<NSMB Wii>.iso" --controller wiimote --dump
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from machine_pace import below_normal, free_port  # noqa: E402
from ppharness import paths  # noqa: E402
from ppharness.instance import DolphinInstance, InstanceConfig  # noqa: E402
from ppharness.netsim import LinkProfile, NetProfile, NetSim  # noqa: E402

DESYNC = re.compile(r"machine: desync at frame (\d+)")
EPOCH_SUM = re.compile(r"machine: player (\d) is at epoch (\d+) with checksum ([0-9a-f]+), not ([0-9a-f]+)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--players", type=int, default=4)
    ap.add_argument("--boot", help="what to boot (a mod's launcher .elf/.dol); the disc by default")
    ap.add_argument("--sd", help="the mod's SD card image")
    ap.add_argument("--iso", help="the disc (default: PPHARNESS_ISO or <root>/game/SSBB_NTSC.iso)")
    ap.add_argument("--exe", default=str(paths.dolphin_nogui()))
    ap.add_argument("--controller", choices=("gc", "wiimote"), default="gc")
    ap.add_argument("--rtt", type=float, default=90)
    ap.add_argument("--jitter", type=float, default=3.0, help="ms each way")
    ap.add_argument("--start-frame", type=int, default=1500, help="the first joiner starts after this frame")
    ap.add_argument("--gap", type=float, default=3.0, help="seconds after a joiner is in that the next one starts")
    ap.add_argument("--seconds", type=float, default=20, help="play after the last join")
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--no-input", action="store_true", help="no PPR_MACHINE_TEST_INPUT (everyone idle)")
    ap.add_argument("--dump", action="store_true", help="PPR_MACHINE_DUMP_DIR: each epoch's image parts")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for every Dolphin")
    ap.add_argument("--config", action="append", default=[], help="System.Section.Key=Value for every Dolphin")
    ap.add_argument("--video", default="Null")
    a = ap.parse_args()
    n = a.players

    out = Path(a.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    iso = Path(a.iso) if a.iso else (paths.game_iso() or paths.fallback_iso())
    boot = a.boot or str(iso)
    sd_cfg = (["Dolphin.Core.WiiSDCard=True", "Dolphin.Core.WiiSDCardEnableFolderSync=False",
               f"Dolphin.General.WiiSDCardPath={a.sd}"] if a.sd else ["Dolphin.Core.WiiSDCard=False"])
    link = LinkProfile(latency_ms=a.rtt / 2, jitter_ms=a.jitter)
    ports = [free_port(socket.SOCK_DGRAM) for _ in range(n)]
    proxies: dict[tuple[int, int], NetSim] = {}
    for i in range(n):
        for j in range(n):
            if i != j:
                proxies[(i, j)] = NetSim(("127.0.0.1", ports[j]), ("127.0.0.1", 0),
                                         NetProfile(up=link, down=link)).start()
    print(f"link: {link.describe()}", flush=True)
    dump = out / "dumps"
    dump.mkdir()

    def peer(slot: int, frm: int, late: bool) -> dict:
        p = {"slot": slot, "host": "127.0.0.1", "port": proxies[(frm, slot)].listen_addr[1]}
        if late:
            p["late"] = True
        return p

    def make(slot: int) -> DolphinInstance:
        # Players before this one play; the ones after it join later.
        session = {"slot": slot, "port": ports[slot], "controller": a.controller,
                   "peers": [peer(o, slot, o > slot) for o in range(n) if o != slot]}
        if slot > 0:
            session["join"] = True
        env = dict(kv.split("=", 1) for kv in a.env)
        env["PPR_MACHINE_SESSION"] = json.dumps(session)
        if not a.no_input:
            env["PPR_MACHINE_TEST_INPUT"] = str(a.seed + slot)
        if a.dump:
            env["PPR_MACHINE_DUMP_DIR"] = str(dump)
        cfg = InstanceConfig(video_backend=a.video, standard_controllers=(0,), other_ports="none",
                             config_args=sd_cfg + [f"Dolphin.Core.DefaultISO={iso}",
                                                   "Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4",
                                                   "Logger.Logs.Brawlback=True", "Logger.Logs.CORE=True"]
                             + a.config)
        inst = DolphinInstance(f"p{slot + 1}", port=free_port(socket.SOCK_STREAM), exe=a.exe, config=cfg, boot=boot,
                               instances_root=out, keep=True, platform="headless", connect_timeout=120, env=env)
        inst.create()
        inst.launch()
        below_normal(inst.process.pid)
        for attempt in range(4):
            try:
                inst.connect()
                break
            except Exception:  # noqa: BLE001
                if attempt == 3 or not inst.is_running():
                    raise
                inst.client = None
                time.sleep(5)
        return inst

    def status(inst: DolphinInstance) -> dict:
        for attempt in range(4):
            try:
                return inst.client.call("machine_status", timeout=30)
            except Exception:  # noqa: BLE001
                if attempt == 3 or not inst.is_running():
                    raise
                inst.client = None
                time.sleep(2)
                inst.connect(timeout=30)
        return {}

    def log_of(inst: DolphinInstance) -> str:
        for f in Path(inst.user_dir).glob("Logs/dolphin*.log"):
            return f.read_text(errors="replace")
        return ""

    def wait_running(inst: DolphinInstance, limit: float) -> bool:
        deadline = time.time() + limit
        while time.time() < deadline:
            if not inst.is_running():
                return False
            if status(inst).get("phase") == "running":
                return True
            text = log_of(inst)
            if "Could not join" in text or "Could not load" in text:
                return False
            time.sleep(0.25)
        return False

    host = make(0)
    insts = [host]
    result: dict = {"in": [0]}
    try:
        deadline = time.time() + 240
        while (status(host).get("frame") or 0) < a.start_frame:
            if time.time() > deadline or not host.is_running():
                raise SystemExit("the host's game did not start")
            time.sleep(1)
        for slot in range(1, n):
            inst = make(slot)
            insts.append(inst)
            ok = wait_running(inst, 180)
            print(f"player {slot + 1} {'in' if ok else 'NOT in'} (host frame {status(host).get('frame')})",
                  flush=True)
            if not ok:
                break
            result["in"].append(slot)
            time.sleep(a.gap)
        time.sleep(a.seconds)
        for inst in insts:
            text = log_of(inst)
            sums = EPOCH_SUM.findall(text)
            desyncs = DESYNC.findall(text)
            st = status(inst) if inst.is_running() else {}
            print(f"{inst.name}: phase {st.get('phase')}, frame {st.get('frame')}, players {st.get('players')}, "
                  f"rollbacks {st.get('rollbacks')}, desync lines {len(desyncs)} "
                  f"(first {desyncs[0] if desyncs else '-'}), epoch checksum mismatches {sorted(set(sums))}",
                  flush=True)
            result[inst.name] = {"desyncs": len(desyncs), "epoch_mismatch": sorted(set(sums)),
                                 "rollbacks": st.get("rollbacks")}
            for line in re.findall(r"machine: ((?:epoch \d+|at epoch \d+|player \d+ left|notice|"
                                   r"Could not|machine image from).*)", text):
                if "%" not in line:
                    print(f"    {line[:160]}", flush=True)
    finally:
        (out / "result.json").write_text(json.dumps(result, indent=1))
        for inst in insts:
            try:
                inst.stop()
            except Exception:  # noqa: BLE001
                pass
        for p in proxies.values():
            p.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
