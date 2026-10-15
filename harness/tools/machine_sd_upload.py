"""What a joiner's SD card download costs the players' game (docs/design/brawl-mods.md §6.5): two
players play a whole-machine session (Core/Rollback/MachineSession.cpp) and a third joins with a card
that differs, so the players upload the difference (PaceSD) while they play. The host's upload goes
through one shared home uplink (netsim SharedUplink: its game packets to the other player and the SD
card's chunks to the joiner queue together), so an upload that fills it delays the game.

Reported for the download's window, against the same length of play before it: freezes of 250 ms
or more (StallTracer) and rollbacks on both players, and the delay the SD card's bytes queued ahead
of the host's game packets added to them (SharedUplink's per-class report).

    python harness/tools/machine_sd_upload.py C:/scratch/sdup/paced --uplink-kbps 8000 \\
        --boot "<REX>/user/Launcher/REX Netplay Launcher.elf" --sd "<REX>/user/Wii/sd.raw" \\
        --joiner-sd C:/scratch/joiner.raw --iso D:/Roms/Wii/SSBB_NTSC2.iso
    ... --env PPR_MACHINE_SD_PACE=0   (forced: the upload unpaced, to show the test sees harm)
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
from ppharness.netsim import LinkProfile, NetProfile, NetSim, SharedUplink, enet_class  # noqa: E402

CHANNEL_SD = 3


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--boot", required=True)
    ap.add_argument("--sd", required=True, help="the players' SD card")
    ap.add_argument("--joiner-sd", required=True, help="the joiner's SD card (differing)")
    ap.add_argument("--iso", default=None)
    ap.add_argument("--exe", default=str(paths.dolphin_nogui()))
    ap.add_argument("--rtt", type=float, default=40)
    ap.add_argument("--jitter", type=float, default=2.0)
    ap.add_argument("--uplink-kbps", type=float, default=8000, help="the host's home uplink")
    ap.add_argument("--queue-ms", type=float, default=200, help="its router's buffer")
    ap.add_argument("--start-frame", type=int, default=1500, help="the joiner starts after this frame")
    ap.add_argument("--before-seconds", type=float, default=60, help="play measured before the joiner starts")
    ap.add_argument("--limit", type=float, default=900, help="seconds the joiner may take to join")
    ap.add_argument("--seed", type=int, default=7, help="PPR_MACHINE_TEST_INPUT's seed (the players play)")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for every Dolphin")
    a = ap.parse_args()

    out = Path(a.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    iso = Path(a.iso) if a.iso else (paths.game_iso() or paths.fallback_iso())

    link = LinkProfile(latency_ms=a.rtt / 2, jitter_ms=a.jitter)
    # Each player's home uplink: the host's (slot 0) and the other player's (slot 1), whichever
    # proxy direction their sending takes. The joiner's is not limited.
    uplinks = {i: SharedUplink(a.uplink_kbps, queue_ms=a.queue_ms, blame="sd") for i in (0, 1)}
    uplink = uplinks[0]
    ports = [free_port(socket.SOCK_DGRAM) for _ in range(3)]
    proxies: dict[tuple[int, int], NetSim] = {}
    for i in range(3):
        for j in range(3):
            if i == j:
                continue
            # i -> j: "up" is i's sending, "down" j's (on a connection i started; a lower slot
            # keeps the one it starts).
            proxies[(i, j)] = NetSim(("127.0.0.1", ports[j]), ("127.0.0.1", 0),
                                     NetProfile(up=link, down=link), shared_up=uplinks.get(i),
                                     shared_down=uplinks.get(j),
                                     shared_class=enet_class(CHANNEL_SD, "sd", "game")).start()
    print(f"link {link.describe()}; the players' uplinks {a.uplink_kbps:g} kbit/s, {a.queue_ms:g} ms buffer",
          flush=True)

    def make(name: str, slot: int, session: dict, card: str) -> DolphinInstance:
        env = dict(kv.split("=", 1) for kv in a.env)
        env["PPR_MACHINE_SESSION"] = json.dumps(session)
        env["PPR_MACHINE_TEST_INPUT"] = str(a.seed + slot)
        cfg = InstanceConfig(video_backend="Null", standard_controllers=(0,), other_ports="none",
                             config_args=["Dolphin.Core.WiiSDCard=True", "Dolphin.Core.WiiSDCardEnableFolderSync=False",
                                          f"Dolphin.General.WiiSDCardPath={card}", f"Dolphin.Core.DefaultISO={iso}",
                                          "Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4",
                                          "Logger.Logs.Brawlback=True", "Logger.Logs.CORE=True"])
        inst = DolphinInstance(name, port=free_port(socket.SOCK_STREAM), exe=a.exe, config=cfg, boot=a.boot,
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

    def peer(slot: int, frm: int, late: bool = False) -> dict:
        p = {"slot": slot, "host": "127.0.0.1", "port": proxies[(frm, slot)].listen_addr[1]}
        if late:
            p["late"] = True
        return p

    host = make("host", 0, {"slot": 0, "port": ports[0], "peers": [peer(1, 0), peer(2, 0, True)]}, a.sd)
    player = make("player", 1, {"slot": 1, "port": ports[1], "peers": [peer(0, 1), peer(2, 1, True)]}, a.sd)
    players = (host, player)
    joiner = None
    ok = False
    try:
        deadline = time.time() + 300
        while min(status(p).get("frame") or 0 for p in players) < a.start_frame:
            if time.time() > deadline or not all(p.is_running() for p in players):
                raise SystemExit("the players' game did not start")
            time.sleep(1)

        def snapshot() -> dict:
            snap = {"t": time.time(), "uplink": {i: up.report("game") for i, up in uplinks.items()}}
            for p in players:
                st = status(p)
                snap[p.name] = {"rollbacks": st.get("rollbacks", 0), "frames_resimulated": st.get("frames_resimulated", 0),
                                "log": len(log_of(p))}
            return snap

        def window(before: dict, after: dict) -> None:
            secs = after["t"] - before["t"]
            print(f"  {secs:.0f} s:", flush=True)
            for p in players:
                text = log_of(p)[before[p.name]["log"]:after[p.name]["log"]]
                stalls = [int(x) for x in re.findall(r"Stall over: (\d+) ms", text)]
                rb = after[p.name]["rollbacks"] - before[p.name]["rollbacks"]
                rs = after[p.name]["frames_resimulated"] - before[p.name]["frames_resimulated"]
                per_min = 60 / max(secs, 1)
                print(f"    {p.name}: {len(stalls) * per_min:.1f} freezes of 250 ms or more a minute"
                      + (f" ({sum(stalls) * per_min / 1000:.1f} s frozen a minute, longest {max(stalls)} ms)"
                         if stalls else "")
                      + f", {rb * per_min:.0f} rollbacks a minute ({rs / max(rb, 1):.1f} frames each)",
                      flush=True)

        # The play before the download.
        base = snapshot()
        time.sleep(a.before_seconds)
        joiner = make("joiner", 2, {"slot": 2, "port": ports[2], "join": True, "sd_sync": "yes",
                                    "peers": [peer(0, 2), peer(1, 2)]}, a.joiner_sd)
        t_start = time.time()
        started = done = None
        while time.time() - t_start < a.limit:
            if not joiner.is_running() or not all(p.is_running() for p in players):
                print("a Dolphin exited", flush=True)
                break
            jl = log_of(joiner)
            if started is None and "SD card: downloading the host's card" in jl:
                for up in uplinks.values():
                    up.reset_counters()
                started = snapshot()
            # The window ends at 90 %: the join right after resets the session's counters.
            if started is not None and (re.search(r"Getting the host's SD card: (9|10)0%", jl)
                                        or re.search(r"SD card: got [\d.]+ MB", jl)):
                done = snapshot()
                while time.time() - t_start < a.limit and not re.search(r"SD card: got [\d.]+ MB", log_of(joiner)):
                    time.sleep(0.5)
                break
            if "the host's SD card was not downloaded" in jl or "could not get the host's SD card" in jl:
                print("the download failed", flush=True)
                break
            time.sleep(0.5)
        jl = log_of(joiner)
        for line in re.findall(r"SD card: ([\d.]+ MB of the session.*|got [\d.]+ MB.*)", jl):
            print(f"  joiner: {line[:150]}", flush=True)
        for p in players:
            for line in re.findall(r"SD card: (sent player 3 .*)", log_of(p)):
                print(f"  {p.name}: {line[:150]}", flush=True)
            for line in re.findall(r"notice: (.*SD card does not match.*)", log_of(p)):
                print(f"  {p.name} chat: {line[:150]}", flush=True)
        if started and done:
            rate = re.search(r"got ([\d.]+) MB of the host's card in ([\d.]+) s", jl)
            if rate:
                print(f"  download: {float(rate.group(1)) / float(rate.group(2)):.2f} MB/s", flush=True)
            print("before the download (as long as it took, back to back with it):", flush=True)
            # The same length of play right before the joiner started.
            length = done["t"] - started["t"]
            print(f"  (measured {started['t'] - base['t']:.0f} s of play before; the download took {length:.0f} s)",
                  flush=True)
            window(base, started)
            print("during the download:", flush=True)
            window(started, done)
            for i, name in ((0, "host"), (1, "player")):
                g = done["uplink"][i]
                d = g["delay_by_others_ms"]
                print(f"  {name}'s game packets on its uplink: {g['datagrams']}, delayed by SD card bytes ahead: "
                      f"median {d.get('p50', 0):.1f} ms, p99 {d.get('p99', 0):.1f} ms, max {d.get('max', 0):.1f} ms; "
                      f"dropped {g['drops']} ({g['drops_with_others_ahead']} behind SD card bytes)", flush=True)
            ok = True
    finally:
        for inst in [*players, *([joiner] if joiner else [])]:
            try:
                inst.stop()
            except Exception:  # noqa: BLE001
                pass
        for sim in proxies.values():
            sim.stop()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
