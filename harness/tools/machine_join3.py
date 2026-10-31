"""A third player joining a whole-machine session (Core/Rollback/MachineSession.cpp) whose second player
joined with the host's machine image (bug 25, 2026-10-16: in a REX room every third player was refused,
"player 2 does not vouch for this machine", and the two players desynced from then on).

The host starts alone; past ``--start-frame`` the first joiner starts and joins; ``--gap`` seconds after
it is in, the second joiner starts and joins. All three play their own random input
(PPR_MACHINE_TEST_INPUT). Reported: whether the second joiner got in, every epoch's checksums ("is at
epoch N with checksum"), and the desyncs on each machine afterwards. ``PPR_MACHINE_DUMP_DIR`` keeps
each epoch's machine image parts per player, to compare the players whose checksums differ.

    python harness/tools/machine_join3.py run/scratch/join3/a --gap 1 \\
        --boot "<REX>/user/Launcher/REX Netplay Launcher.elf" --sd "<REX>/user/Wii/sd.raw"
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
BAD_LOAD = re.compile(r"machine: rollback \d+ frame\(s\) back \(ring \d+\): frame \d+ starts at")
EPOCH_SUM = re.compile(r"machine: player (\d) is at epoch (\d+) with checksum ([0-9a-f]+), not ([0-9a-f]+)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--boot", help="what to boot (a mod's launcher .elf/.dol); the disc by default")
    ap.add_argument("--sd", help="the mod's SD card image")
    ap.add_argument("--iso", help="the disc (default: PPHARNESS_ISO or <root>/game/SSBB_NTSC.iso)")
    ap.add_argument("--exe", default=str(paths.dolphin_nogui()))
    ap.add_argument("--rtt", type=float, default=90)
    ap.add_argument("--jitter", type=float, default=3.0, help="ms each way")
    ap.add_argument("--start-frame", type=int, default=1500, help="the first joiner starts after this frame")
    ap.add_argument("--gap", type=float, default=1.0,
                    help="seconds after the first joiner is in that the second one starts (negative: before)")
    ap.add_argument("--seconds", type=float, default=60, help="play after the second join")
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--no-input", action="store_true", help="no PPR_MACHINE_TEST_INPUT (everyone idle)")
    ap.add_argument("--dump", action="store_true", help="PPR_MACHINE_DUMP_DIR: each epoch's image parts")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for every Dolphin")
    ap.add_argument("--video", default="Null")
    ap.add_argument("--slot-config", action="append", default=[],
                    help="SLOT:System.Section.Key=Value for one player only (slot 0-2), e.g. 1:Dolphin.Core.CPUThread=False")
    a = ap.parse_args()

    out = Path(a.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    iso = Path(a.iso) if a.iso else (paths.game_iso() or paths.fallback_iso())
    boot = a.boot or str(iso)
    sd_cfg = (["Dolphin.Core.WiiSDCard=True", "Dolphin.Core.WiiSDCardEnableFolderSync=False",
               f"Dolphin.General.WiiSDCardPath={a.sd}"] if a.sd else ["Dolphin.Core.WiiSDCard=False"])
    link = LinkProfile(latency_ms=a.rtt / 2, jitter_ms=a.jitter)
    ports = [free_port(socket.SOCK_DGRAM) for _ in range(3)]
    proxies: dict[tuple[int, int], NetSim] = {}
    for i in range(3):
        for j in range(3):
            if i != j:
                proxies[(i, j)] = NetSim(("127.0.0.1", ports[j]), ("127.0.0.1", 0),
                                         NetProfile(up=link, down=link)).start()
    print(f"link: {link.describe()}", flush=True)
    dump = out / "dumps"
    dump.mkdir()

    def peer(slot: int, frm: int, late: bool = False) -> dict:
        p = {"slot": slot, "host": "127.0.0.1", "port": proxies[(frm, slot)].listen_addr[1]}
        if late:
            p["late"] = True
        return p

    def make(name: str, slot: int, session: dict) -> DolphinInstance:
        env = dict(kv.split("=", 1) for kv in a.env)
        env["PPR_MACHINE_SESSION"] = json.dumps(session)
        if not a.no_input:
            env["PPR_MACHINE_TEST_INPUT"] = str(a.seed + slot)
        if a.dump:
            env["PPR_MACHINE_DUMP_DIR"] = str(dump)
        cfg = InstanceConfig(video_backend=a.video, standard_controllers=(0,), other_ports="none",
                             config_args=sd_cfg + [c.split(":", 1)[1] for c in a.slot_config
                                                   if c.split(":", 1)[0] == str(slot)] + [
                                                   f"Dolphin.Core.DefaultISO={iso}",
                                                   "Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4",
                                                   "Logger.Logs.Brawlback=True", "Logger.Logs.CORE=True"])
        inst = DolphinInstance(name, port=free_port(socket.SOCK_STREAM), exe=a.exe, config=cfg, boot=boot,
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
            if "could not join" in log_of(inst) or "Could not load" in log_of(inst):
                return False
            time.sleep(0.25)
        return False

    host = make("host", 0, {"slot": 0, "port": ports[0], "peers": [peer(1, 0, True), peer(2, 0, True)]})
    insts = [host]
    result = {"second_in": False}
    try:
        deadline = time.time() + 240
        while (status(host).get("frame") or 0) < a.start_frame:
            if time.time() > deadline or not host.is_running():
                raise SystemExit("the host's game did not start")
            time.sleep(1)
        one = make("one", 1, {"slot": 1, "port": ports[1], "join": True,
                              "peers": [peer(0, 1), peer(2, 1, True)]})
        insts.append(one)
        two = None
        if a.gap < 0:
            time.sleep(-a.gap)
            two = make("two", 2, {"slot": 2, "port": ports[2], "join": True, "peers": [peer(0, 2), peer(1, 2)]})
            insts.append(two)
        if not wait_running(one, 180):
            raise SystemExit("the first joiner did not get in")
        print(f"first joiner in at frame {status(host).get('frame')}", flush=True)
        if two is None:
            time.sleep(max(a.gap, 0))
            two = make("two", 2, {"slot": 2, "port": ports[2], "join": True, "peers": [peer(0, 2), peer(1, 2)]})
            insts.append(two)
        deadline = time.time() + 180
        while time.time() < deadline:
            st = status(host)
            if st.get("players", 0) & 4 or (status(two).get("phase") == "running"):
                result["second_in"] = True
                break
            if "could not join" in log_of(host) or not two.is_running():
                break
            time.sleep(0.5)
        print(f"second joiner {'in' if result['second_in'] else 'NOT in'} (host frame "
              f"{status(host).get('frame')})", flush=True)
        time.sleep(a.seconds)
        for inst in insts:
            text = log_of(inst)
            sums = EPOCH_SUM.findall(text)
            desyncs = DESYNC.findall(text)
            st = status(inst) if inst.is_running() else {}
            # A load that restored another frame's state (MachineSession's CheckLoadedTime).
            bad_loads = len(BAD_LOAD.findall(text))
            print(f"{inst.name}: phase {st.get('phase')}, frame {st.get('frame')}, players {st.get('players')}, "
                  f"rollbacks {st.get('rollbacks')}, loads of the wrong frame {bad_loads}, "
                  f"desync lines {len(desyncs)} (first {desyncs[0] if desyncs else '-'}), "
                  f"epoch checksum mismatches {sorted(set(sums))}", flush=True)
            result[inst.name] = {"desyncs": len(desyncs), "epoch_mismatch": sorted(set(sums)), "bad_loads": bad_loads,
                                 "rollbacks": st.get("rollbacks")}
            for line in re.findall(r"machine: ((?:epoch \d+|at epoch \d+|player \d+ left|notice).*)", text):
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
