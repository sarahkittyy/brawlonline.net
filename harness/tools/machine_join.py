"""A late join in a whole-machine session (Core/Rollback/MachineSession.cpp) over an impaired link:
how long the joiner's machine image takes and whether the join gets through.

The host starts alone (the joiner's slot ``late``); once its game is past ``--join-frame`` the joiner
starts and asks to join, and the host sends it the machine image. Every packet between them goes
through netsim with ``--rtt``, and optionally a bandwidth cap with tail drop (``--rate-kbps``,
``--queue-ms``: a home uplink whose modem buffer overflows) and random loss (``--loss``), the same
both ways. Repeat with ``--joins N``: the joiner is stopped and started again for each.

    python harness/tools/machine_join.py run/scratch/join/new --rtt 40 --rate-kbps 16000 --queue-ms 60 \\
        --boot "<mod>/User/Launcher/REX Rauncher.elf" --sd "<mod>/User/Wii/sd.raw" --joins 3

A joiner whose SD card differs from the host's (docs/design/brawl-mods.md §6.5): ``--joiner-sd`` is
its card, ``--sd-sync`` its answer to the download (yes, overwrite, no). It gets the host's card
before it asks to join; ``--play-seconds`` keeps the game going after the join and reports any read
of the card that still had to wait for the network (there must be none).
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


# The host's epoch line once the joiner (slot 2) plays.
JOINED = re.compile(r"epoch \d+: frame \d+, players \d*1\d$", re.M)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--boot", help="what to boot (a mod's launcher .elf/.dol); the disc by default")
    ap.add_argument("--sd", help="the mod's SD card image")
    ap.add_argument("--joiner-sd", help="the joiner's SD card image (default: --sd)")
    ap.add_argument("--sd-sync", default="yes", choices=["ask", "yes", "overwrite", "no"],
                    help="the joiner's answer when its SD card differs from the host's")
    ap.add_argument("--play-seconds", type=float, default=5,
                    help="how long both play after a join before their state is read")
    ap.add_argument("--iso", help="the disc (default: PPHARNESS_ISO or <root>/game/SSBB_NTSC.iso)")
    ap.add_argument("--exe", default=str(paths.dolphin_nogui()))
    ap.add_argument("--rtt", type=float, default=40)
    ap.add_argument("--rate-kbps", type=float, default=None)
    ap.add_argument("--queue-ms", type=float, default=60)
    ap.add_argument("--loss", type=float, default=0.0)
    ap.add_argument("--jitter", type=float, default=1.0, help="each direction's jitter, ms (Wi-Fi: pareto)")
    ap.add_argument("--jitter-dist", default="normal", choices=["normal", "uniform", "pareto"])
    ap.add_argument("--join-frame", type=int, default=1500)
    ap.add_argument("--joins", type=int, default=1)
    ap.add_argument("--limit", type=float, default=150, help="seconds a join may take")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--video", default="Null", help="video backend (players use D3D11 or Vulkan)")
    ap.add_argument("--host-config", action="append", default=[],
                    help="System.Section.Key=value set on the host's running game (its current layer, as "
                         "Dolphin's settings windows do) before the first join")
    ap.add_argument("--delay", help="each player's input delay: 0-9 or auto (default: the session's, 2)")
    ap.add_argument("--platform", default="headless", help="Dolphin's window platform (win32 for a real backend)")
    ap.add_argument("--host-play-record", action="store_true",
                    help="the host's session NAND starts with the Wii Menu's play_rec.dat (a disc boot's BS2 "
                         "writes it) and the joiner's without: the game keeps it open (bug #82)")
    a = ap.parse_args()

    out = Path(a.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    iso = Path(a.iso) if a.iso else (paths.game_iso() or paths.fallback_iso())
    boot = a.boot or str(iso)
    def sd_cfg_of(card: str | None) -> list[str]:
        return (["Dolphin.Core.WiiSDCard=True", "Dolphin.Core.WiiSDCardEnableFolderSync=False",
                 f"Dolphin.General.WiiSDCardPath={card}"] if card else ["Dolphin.Core.WiiSDCard=False"])

    link = LinkProfile(latency_ms=a.rtt / 2, jitter_ms=a.jitter, jitter_dist=a.jitter_dist, loss=a.loss,
                       rate_kbps=a.rate_kbps, queue_ms=a.queue_ms)
    p0, p1 = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_DGRAM)
    s01 = NetSim(("127.0.0.1", p1), ("127.0.0.1", 0), NetProfile(up=link, down=link)).start()
    s10 = NetSim(("127.0.0.1", p0), ("127.0.0.1", 0), NetProfile(up=link, down=link)).start()
    t1, t0 = s01.listen_addr[1], s10.listen_addr[1]
    print(f"link: {link.describe()}", flush=True)

    def make(name: str, session: dict, card: str | None) -> DolphinInstance:
        if a.delay is not None:
            session["delay"] = a.delay if a.delay == "auto" else int(a.delay)
        env = dict(kv.split("=", 1) for kv in a.env)
        env["PPR_MACHINE_SESSION"] = json.dumps(session)
        cfg = InstanceConfig(video_backend=a.video, standard_controllers=(0,), other_ports="none",
                             config_args=sd_cfg_of(card) + [f"Dolphin.Core.DefaultISO={iso}",
                                                   "Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4",
                                                   "Logger.Logs.Brawlback=True", "Logger.Logs.CORE=True"])
        inst = DolphinInstance(name, port=free_port(socket.SOCK_STREAM), exe=a.exe, config=cfg, boot=boot,
                               instances_root=out, keep=True, platform=a.platform, connect_timeout=120,
                               env=env)
        inst.create()
        if a.host_play_record and name == "host":
            # WiiRoot.cpp copies <User>/NetplaySave into the session's NAND.
            rec = Path(inst.user_dir) / "NetplaySave" / "title" / "00000001" / "00000002" / "data" / "play_rec.dat"
            rec.parent.mkdir(parents=True, exist_ok=True)
            rec.write_bytes(bytes(0x80))
        inst.launch()
        below_normal(inst.process.pid)
        # A loaded PC can leave the first connection half open ("another harness client is
        # already connected"): try again once it has closed.
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
        # As machine_pace's call: a busy instance can miss a call, so reconnect and ask again.
        for attempt in range(4):
            try:
                return inst.client.call("machine_status", timeout=30)
            except Exception:  # noqa: BLE001
                if attempt == 3 or not inst.is_running():
                    raise
                try:
                    inst.client.close()
                except Exception:  # noqa: BLE001
                    pass
                inst.client = None
                time.sleep(2)
                inst.connect(timeout=30)
        return {}

    def log_of(inst: DolphinInstance) -> str:
        for f in Path(inst.user_dir).glob("Logs/dolphin*.log"):
            return f.read_text(errors="replace")
        return ""

    host = make("host", {"slot": 0, "port": p0,
                         "peers": [{"slot": 1, "host": "127.0.0.1", "port": t1, "late": True}]}, a.sd)
    results = []
    try:
        deadline = time.time() + 240
        while (status(host).get("frame") or 0) < a.join_frame:
            if time.time() > deadline or not host.is_running():
                raise SystemExit("the host's game did not start")
            time.sleep(1)
        for kv in a.host_config:
            name, value = kv.split("=", 1)
            system, section, key = name.split(".", 2)
            host.client.call("config_set", system=system, section=section, key=key, value=value, layer="current")
            print(f"host: {name} = {value} (current layer)", flush=True)
        if a.host_config:
            time.sleep(3)
        for k in range(a.joins):
            host_log_at = len(log_of(host))
            failed_before = log_of(host).count("could not join")
            joined_before = len(JOINED.findall(log_of(host)))
            joiner = make(f"joiner{k}", {"slot": 1, "port": p1, "join": True, "sd_sync": a.sd_sync,
                                         "peers": [{"slot": 0, "host": "127.0.0.1", "port": t0}]},
                          a.joiner_sd or a.sd)
            t_start = time.time()
            outcome = "timeout"
            while time.time() - t_start < a.limit:
                if not joiner.is_running() or not host.is_running():
                    outcome = "a Dolphin exited"
                    break
                if log_of(host).count("could not join") > failed_before:
                    outcome = "could not join"
                    break
                if len(JOINED.findall(log_of(host))) > joined_before and status(joiner).get("phase") == "running":
                    outcome = "joined"
                    break
                time.sleep(0.5)
            took = time.time() - t_start
            hlog = log_of(host)[host_log_at:]
            sent = re.findall(r"machine image for player 2: ([\d.]+) MB", hlog)
            progress = re.findall(r"(\d\d:\d\d:\d\d\d) .*Sending the game to [^:]*: (\d+)%", hlog)
            # The longest wait between two progress lines of this join (stalls).
            stamps = [int(t[:2]) * 60000 + int(t[3:5]) * 1000 + int(t[6:]) for t, _ in progress]
            gaps = [b - c for c, b in zip(stamps, stamps[1:]) if b >= c]
            results.append((outcome, took, sent[-1] if sent else "?", max(gaps, default=0) / 1000))
            print(f"join {k + 1}: {outcome} in {took:.1f} s (image {sent[-1] if sent else '?'} MB, "
                  f"longest stall between progress lines {max(gaps, default=0) / 1000:.1f} s)", flush=True)
            # The joiner's SD card before its join, and any read that still waited while it played.
            for line in re.findall(r"SD card: (?:\d+ chunks of this|[\d.]+ MB of the session|this player's card is "
                                   r"the session's|got [\d.]+ MB|wrote the host's|downloading).*", log_of(joiner)):
                print(f"  joiner: SD card: {line.split('SD card: ', 1)[-1][:160]}", flush=True)
            if outcome == "joined":
                logs_at = {inst.name: len(log_of(inst)) for inst in (host, joiner)}
                time.sleep(a.play_seconds)
                # Freezes while they played (StallTracer: no emulated field for 250 ms or more).
                for inst in (host, joiner):
                    stalls = [int(x) for x in re.findall(r"Stall over: (\d+) ms", log_of(inst)[logs_at[inst.name]:])]
                    print(f"  {inst.name}: {len(stalls)} freezes of 250 ms or more in {a.play_seconds:.0f} s"
                          + (f" ({sum(stalls)} ms in all, longest {max(stalls)} ms)" if stalls else ""), flush=True)
                for inst in (host, joiner):
                    st = status(inst)
                    print(f"  {inst.name}: frame {st.get('frame')}, desyncs {st.get('desyncs')}", flush=True)
                late = re.findall(r"the game read \d+ chunks this player had not got.*|a read waited \d+ ms.*",
                                  log_of(joiner))
                print(f"  joiner: SD reads that waited while playing: {len(late)}"
                      + (f" ({late[0][:120]})" if late else ""), flush=True)
                if late:
                    results[-1] = ("joined, but a read waited",) + results[-1][1:]
            joiner.stop()
            # The host notices the joiner is gone and its slot is free to join again.
            time.sleep(15)
    finally:
        for inst in [host]:
            try:
                inst.stop()
            except Exception:  # noqa: BLE001
                pass
        s01.stop()
        s10.stop()
    ok = sum(1 for r in results if r[0] == "joined")
    print(f"{ok}/{len(results)} joined", flush=True)
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
