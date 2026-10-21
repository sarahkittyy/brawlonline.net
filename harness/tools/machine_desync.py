"""Desyncs in a whole-machine session (Core/Rollback/MachineSession.cpp) with a late join over a slow
link, and what each one was made of (docs/design/brawl-mods.md §6.6).

The host starts alone; past ``--join-frame`` the joiner starts and joins with the host's machine image,
through netsim at ``--rtt`` (bug 16: a room between California and the UK, about 167 ms). Both then
play random pad scripts for ``--seconds``. With ``PPR_MACHINE_DIGESTS`` on, every frame either
machine reports as a desync is looked up on both: the granules each save found changed (counted,
skipped as GPU-written, skipped as an excluded region) and the slice of all RAM, so that a frame
whose memory differs is told apart from one whose memory is the same but was counted differently.

    python harness/tools/machine_desync.py run/scratch/desync/a --rtt 167 --seconds 300 \\
        --boot "<mod>/User/Launcher/REX Rauncher.elf" --sd "<mod>/User/Wii/sd.raw"
"""

from __future__ import annotations

import argparse
import json
import random
import re
import shutil
import socket
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from machine_pace import below_normal, free_port, random_script  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import flows, paths  # noqa: E402
from ppharness.instance import DolphinInstance, InstanceConfig  # noqa: E402
from ppharness.netsim import LinkProfile, NetProfile, NetSim  # noqa: E402

DESYNC = re.compile(r"machine: desync at frame (\d+) \(local ([0-9a-f]+), remote ([0-9a-f]+)\)")


def parse_changed(text: str) -> dict:
    out = {}
    for item in text.split():
        addr, crc, kind = item.split(":")
        out[addr] = (crc, int(kind))
    return out


def compare(frame: int, h: dict, j: dict) -> dict:
    """What the two machines' checksums of `frame` were made of, and where they part."""
    hc, jc = parse_changed(h.get("changed", "")), parse_changed(j.get("changed", ""))
    only_h = sorted(set(hc) - set(jc))
    only_j = sorted(set(jc) - set(hc))
    content = sorted(a for a in set(hc) & set(jc) if hc[a][0] != jc[a][0] and hc[a][1] == 0 and jc[a][1] == 0)
    kind = sorted(a for a in set(hc) & set(jc) if hc[a][1] != jc[a][1])
    hs, js = set(h.get("slice_gpu", "").split()), set(j.get("slice_gpu", "").split())
    return {
        "frame": frame,
        "checksums": [h.get("checksum"), j.get("checksum")],
        "passes": ["re-run" if h.get("resim") else "first", "re-run" if j.get("resim") else "first"],
        "change_crc_equal": h.get("change_crc") == j.get("change_crc"),
        "slice_crc_equal": h.get("slice_crc") == j.get("slice_crc"),
        "changed_counts": [len(hc), len(jc)],
        "only_host": [(a, hc[a]) for a in only_h][:20],
        "only_joiner": [(a, jc[a]) for a in only_j][:20],
        "content_differs": [(a, hc[a][0], jc[a][0]) for a in content][:20],
        "counted_differently": [(a, hc[a][1], jc[a][1]) for a in kind][:20],
        "slice_gpu_only_host": sorted(hs - js)[:20],
        "slice_gpu_only_joiner": sorted(js - hs)[:20],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--boot", help="what to boot (a mod's launcher .elf/.dol); the disc by default")
    ap.add_argument("--sd", help="the mod's SD card image")
    ap.add_argument("--iso", help="the disc (default: PPHARNESS_ISO or <root>/game/SSBB_NTSC.iso)")
    ap.add_argument("--joiner-iso", help="the joiner's disc (default: --iso)")
    ap.add_argument("--together", action="store_true",
                    help="both players boot together (each its own disc), no late join")
    ap.add_argument("--exe", default=str(paths.dolphin_nogui()))
    ap.add_argument("--rtt", type=float, default=167)
    ap.add_argument("--jitter", type=float, default=4.0, help="ms each way")
    ap.add_argument("--delay", type=int, default=2)
    ap.add_argument("--join-frame", type=int, default=1500)
    ap.add_argument("--seconds", type=float, default=300)
    ap.add_argument("--hold-max", type=int, default=20)
    ap.add_argument("--seed", type=int, default=3)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--no-diag", action="store_true", help="without the digests")
    ap.add_argument("--match", action="store_true",
                    help="the host goes to the character select, the joiner joins there (as in bug 16), both "
                         "pick, and they play a match: fighters that hover off screen and chase, instead of "
                         "random input")
    ap.add_argument("--chars", default="mario,fox", help="--match: the two characters")
    ap.add_argument("--bytes", type=int, default=0,
                    help="for the first N desync frames, the differing granules' bytes on both machines "
                         "(PPR_MACHINE_DIGESTS=2)")
    ap.add_argument("--video", default="Null")
    ap.add_argument("--platform", default="headless")
    a = ap.parse_args()

    out = Path(a.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    iso = Path(a.iso) if a.iso else (paths.game_iso() or paths.fallback_iso())
    # --joiner-iso: the joiner's disc (another revision or image format of the same game).
    joiner_iso = Path(a.joiner_iso) if a.joiner_iso else iso
    sd_cfg = (["Dolphin.Core.WiiSDCard=True", "Dolphin.Core.WiiSDCardEnableFolderSync=False",
               f"Dolphin.General.WiiSDCardPath={a.sd}"] if a.sd else ["Dolphin.Core.WiiSDCard=False"])
    link = LinkProfile(latency_ms=a.rtt / 2, jitter_ms=a.jitter)
    p0, p1 = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_DGRAM)
    s01 = NetSim(("127.0.0.1", p1), ("127.0.0.1", 0), NetProfile(up=link, down=link)).start()
    s10 = NetSim(("127.0.0.1", p0), ("127.0.0.1", 0), NetProfile(up=link, down=link)).start()
    t1, t0 = s01.listen_addr[1], s10.listen_addr[1]
    print(f"link: {link.describe()}", flush=True)

    def make(name: str, session: dict, disc: Path = iso) -> DolphinInstance:
        env = {} if a.no_diag else {"PPR_MACHINE_DIGESTS": "2" if a.bytes else "1"}
        env.update(dict(kv.split("=", 1) for kv in a.env))
        env["PPR_MACHINE_SESSION"] = json.dumps({**session, "delay": a.delay})
        cfg = InstanceConfig(video_backend=a.video, standard_controllers=(0,), other_ports="none",
                             config_args=sd_cfg + [f"Dolphin.Core.DefaultISO={disc}",
                                                   "Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4",
                                                   "Logger.Logs.Brawlback=True", "Logger.Logs.CORE=True"])
        inst = DolphinInstance(name, port=free_port(socket.SOCK_STREAM), exe=a.exe, config=cfg,
                               boot=a.boot or str(disc),
                               instances_root=out, keep=True, platform=a.platform, connect_timeout=120,
                               env=env)
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

    def call(inst: DolphinInstance, cmd: str, **kw) -> dict:
        for attempt in range(4):
            try:
                return inst.client.call(cmd, timeout=90, **kw)
            except Exception:  # noqa: BLE001
                if attempt == 3 or not inst.is_running():
                    raise
                try:
                    inst.client.close()
                except Exception:  # noqa: BLE001
                    pass
                inst.client = None
                # The dropped connection can stay half open for a while ("another harness client is
                # already connected").
                for retry in range(20):
                    time.sleep(3)
                    try:
                        inst.connect(timeout=30)
                        break
                    except Exception:  # noqa: BLE001
                        if retry == 19 or not inst.is_running():
                            raise
                        inst.client = None
        return {}

    def log_of(inst: DolphinInstance) -> str:
        for f in Path(inst.user_dir).glob("Logs/dolphin*.log"):
            return f.read_text(errors="replace")
        return ""

    def locked(inst: DolphinInstance) -> None:
        # The match flow's thread and the desync checks share each instance's one connection.
        lock = threading.Lock()
        raw = inst.client.call

        def call_locked(*args, **kw):
            with lock:
                return raw(*args, **kw)

        inst.client.call = call_locked

    def to_match(host_seat: flows.Seat, joiner_seat: flows.Seat, chars: list[str]) -> None:
        for seat, who in ((host_seat, chars[0]), (joiner_seat, chars[1])):
            B.css_pick_character(seat.client, seat.port, B.CSS_ID[who], pad_port=seat.pp)
        B.css_start(host_seat.client, host_seat.pp)
        last = [-1000]

        def press(info: B.SceneInfo) -> None:
            # Whatever stage the cursor is on: REX's stage select is not P+'s.
            now = B.polls(host_seat.client)
            if info.scene is not B.Scene.IN_MATCH and now - last[0] >= 20:
                last[0] = now
                B.tap(host_seat.client, host_seat.pp, ["A"], hold=3, release=3)

        B.wait_scene(host_seat.client, [B.Scene.IN_MATCH], 60 * 60, every=4, on_frame=press)

    rng = random.Random(a.seed)
    # --together: both boot together, each from its own disc, instead of a late join.
    host = make("host", {"slot": 0, "port": p0,
                         "peers": [{"slot": 1, "host": "127.0.0.1", "port": t1, **({} if a.together else {"late": True})}]})
    joiner = None
    if a.together:
        joiner = make("joiner", {"slot": 1, "port": p1, "peers": [{"slot": 0, "host": "127.0.0.1", "port": t0}]},
                      joiner_iso)
    reports = []
    try:
        deadline = time.time() + 240
        while (call(host, "machine_status").get("frame") or 0) < a.join_frame:
            if time.time() > deadline or not host.is_running():
                raise SystemExit("the host's game did not start")
            time.sleep(1)
        if a.match:
            B.boot_to_css(host.client, 0)
            B.wait_css_ready(host.client, [0])
            print(f"host at the character select, frame {call(host, 'machine_status').get('frame')}", flush=True)
        if not a.together:
            joiner = make("joiner", {"slot": 1, "port": p1, "join": True,
                                     "peers": [{"slot": 0, "host": "127.0.0.1", "port": t0}]}, joiner_iso)
        deadline = time.time() + 180
        while call(joiner, "machine_status").get("phase") != "running":
            if time.time() > deadline or not joiner.is_running():
                raise SystemExit("the joiner did not get in")
            time.sleep(0.5)
        epoch = call(host, "machine_status").get("frame_base")
        print(f"joined at frame {epoch}", flush=True)
        insts = {"host": host, "joiner": joiner}
        seen: set[int] = set()
        t_end = time.time() + a.seconds
        last_pad = 0.0
        match_thread = None
        match_error: list[BaseException] = []
        if a.match:
            locked(host)
            locked(joiner)
            hs = flows.Seat(host.client, 0, 0, "host")
            js = flows.Seat(joiner.client, 1, 0, "joiner")

            def run_match() -> None:
                try:
                    to_match(hs, js, a.chars.split(","))
                    print(f"in a match at frame {call(host, 'machine_status').get('frame')}", flush=True)
                    k = 0
                    while time.time() < t_end:
                        # Off screen (the magnifier bubble) and back, in turns.
                        modes = ("hover", "chase") if k % 2 == 0 else ("chase", "hover")
                        flows.play([(hs, modes[0]), (js, modes[1])], 600, seed=k)
                        k += 1
                        st = B.read_match_state(host.client.read_mem)
                        if not st.in_match or st.game_set:
                            print(f"the match ended at frame {call(host, 'machine_status').get('frame')}", flush=True)
                            return
                except BaseException as e:  # noqa: BLE001
                    match_error.append(e)

            match_thread = threading.Thread(target=run_match, daemon=True)
            match_thread.start()
        while time.time() < t_end:
            if not host.is_running() or not joiner.is_running():
                raise SystemExit("a Dolphin exited")
            if match_error:
                print(f"match flow: {match_error[0]!r}"[:400], flush=True)
                match_error.clear()
            if not a.match and time.time() - last_pad > 9:
                for n, inst in insts.items():
                    try:
                        inst.client.pad_script(0, random_script(rng, 600, a.hold_max))
                    except Exception as e:  # noqa: BLE001
                        print(f"{n}: pad_script: {e}"[:120], flush=True)
                last_pad = time.time()
            frames = set()
            for inst in insts.values():
                frames |= {int(m.group(1)) for m in DESYNC.finditer(log_of(inst))}
            for f in sorted(frames - seen):
                seen.add(f)
                h, j = call(host, "machine_digest", frame=f), call(joiner, "machine_digest", frame=f)
                if not h or not j:
                    print(f"desync at frame {f}: no digest on {'host' if not h else 'joiner'} (too late)", flush=True)
                    continue
                r = compare(f, h, j)
                reports.append(r)
                print(json.dumps(r), flush=True)
                if a.bytes > 0 and r["content_differs"]:
                    a.bytes -= 1
                    addrs = [c[0] for c in r["content_differs"][:8]]
                    hb = call(host, "machine_digest", frame=f, addrs=addrs).get("bytes", {})
                    jb = call(joiner, "machine_digest", frame=f, addrs=addrs).get("bytes", {})
                    for addr in addrs:
                        x, y = hb.get(addr, ""), jb.get(addr, "")
                        words = [(i // 8, x[i:i + 8], y[i:i + 8]) for i in range(0, min(len(x), len(y)), 8)
                                 if x[i:i + 8] != y[i:i + 8]]
                        print(f"  {addr}: " + " ".join(f"+{w * 4:02x}:{u}/{v}" for w, u, v in words), flush=True)
            time.sleep(1)
        for n, inst in insts.items():
            st = call(inst, "machine_status")
            print(f"{n}: frame {st.get('frame')}, desyncs {st.get('desyncs')}, rollbacks {st.get('rollbacks')}",
                  flush=True)
    finally:
        (out / "desyncs.json").write_text(json.dumps(reports, indent=1))
        for inst in [host, joiner]:
            if inst is None:
                continue
            try:
                inst.stop()
            except Exception:  # noqa: BLE001
                pass
        s01.stop()
        s10.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
