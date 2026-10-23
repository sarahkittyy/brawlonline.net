"""Where a whole-machine rollback session's time goes (Core/Rollback/MachineSession.cpp): one setup per
run, with the pace log on (PPR_MACHINE_PACE_LOG=1), summarized per machine after a warm-up
(docs/design/brawl-mods.md §6.4).

``solo``  A session with no peer; ``--synctest N`` rolls every frame back N-1 frames and re-runs it,
          and its re-runs are compared with the first runs (``desyncs``, ``diag_frames``).
``pair``  Two sessions, over loopback or through netsim (``--rtt``): real rollbacks.

Input: ``--input random`` sends pad scripts through the harness in real time (the two runs of an
A/B take different paths if their speeds differ); ``--input none`` with
``--env PPR_MACHINE_TEST_INPUT=SEED`` plays a seeded pad as a function of the frame (the same path
whatever the speed). A game that stops advancing for ``--stall`` seconds has its CPU state dumped.

    python harness/tools/machine_pace.py run run/scratch/pace/new --mode pair --rtt 50 \\
        --boot "<mod>/User/Launcher/REX Rauncher.elf" --sd "<mod>/User/Wii/sd.raw" --hold-max 4
    python harness/tools/machine_pace.py run run/scratch/pace/old ... --env PPR_MACHINE_JIT_ERASE_ALL=1
    python harness/tools/machine_pace.py costs run/scratch/pace/new
"""

from __future__ import annotations

import argparse
import ctypes
import json
import random
import re
import shutil
import socket
import statistics as st
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ppharness import paths  # noqa: E402
from ppharness.instance import DolphinInstance, InstanceConfig  # noqa: E402
from ppharness.netsim import LinkProfile, NetProfile, NetSim  # noqa: E402


def free_port(kind: int) -> int:
    # A random port in a range of its own: other sessions' harness instances take the OS's picks.
    for _ in range(200):
        port = random.randint(31000, 38999)
        try:
            with socket.socket(socket.AF_INET, kind) as s:
                s.bind(("127.0.0.1", port))
            return port
        except OSError:
            continue
    raise SystemExit("no free port")


def below_normal(pid: int) -> None:
    if sys.platform != "win32":
        return
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x0200 | 0x0400, False, pid)
    if h:
        k.SetPriorityClass(h, 0x4000)
        k.CloseHandle(h)


def random_script(rng: random.Random, frames: int, hold_max: int) -> list:
    s, t = [], 0
    while t < frames:
        hold = rng.randint(2, hold_max)
        k = rng.random()
        if k < 0.4:
            s.append({"main": [rng.randint(0, 255), rng.randint(0, 255)], "hold": hold})
        elif k < 0.8:
            s.append({"buttons": [rng.choice(["A", "A", "B", "START", "DUP", "DDOWN", "DLEFT", "DRIGHT"])],
                      "hold": hold})
        else:
            s.append({"hold": hold})
        t += hold
    return s


def run(a: argparse.Namespace) -> int:
    out = Path(a.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    iso = Path(a.iso) if a.iso else (paths.game_iso() or paths.fallback_iso())
    # --iso2: the second player's disc (another revision or image format of the same game).
    iso2 = Path(a.iso2) if a.iso2 else iso
    sd_cfg = (["Dolphin.Core.WiiSDCard=True", "Dolphin.Core.WiiSDCardEnableFolderSync=False",
               f"Dolphin.General.WiiSDCardPath={a.sd}"] if a.sd else ["Dolphin.Core.WiiSDCard=False"])
    insts: dict[str, DolphinInstance] = {}
    sims: list[NetSim] = []

    def boot_one(name: str, session: dict, disc: Path = iso) -> None:
        if a.delay is not None:
            session["delay"] = a.delay if a.delay == "auto" else int(a.delay)
        if a.controller == "wiimote":
            session["controller"] = "wiimote"
        env = dict(kv.split("=", 1) for kv in a.env)
        env["PPR_MACHINE_PACE_LOG"] = "1"
        env["PPR_MACHINE_SESSION"] = json.dumps(session)
        cfg = InstanceConfig(cpu_thread=not a.single_core, video_backend=a.video, emulation_speed=a.speed,
                             standard_controllers=(0,), other_ports="none",
                             wiimote_ini={"Wiimote1": {"Source": 1}} if a.controller == "wiimote" else {},
                             config_args=sd_cfg + [f"Dolphin.Core.DefaultISO={disc}",
                                                   "Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4",
                                                   "Logger.Logs.Brawlback=True", "Logger.Logs.CORE=True"]
                             + list(a.cfg))
        inst = DolphinInstance(name, port=free_port(socket.SOCK_STREAM), exe=a.exe, config=cfg,
                               boot=a.boot or str(disc),
                               instances_root=out, keep=True, platform=a.platform, connect_timeout=120,
                               env=env)
        d = inst.create()
        if a.save_from:
            # A save for the game's title, so its boot finds one: without it, buttons pressed at the
            # boot message about it can hold a session's game for good.
            dst = Path(d) / "Wii" / "title" / "00010000" / "52534245" / "data"
            dst.mkdir(parents=True, exist_ok=True)
            for f in Path(a.save_from).iterdir():
                if f.is_file():
                    shutil.copyfile(f, dst / f.name)
        inst.launch()
        below_normal(inst.process.pid)
        inst.connect()
        insts[name] = inst

    rpc_lock = threading.Lock()  # one RPC at a time on the harness connections (the Wii Remote thread)

    def call(name: str, cmd: str, **kw):
        inst = insts[name]
        for attempt in range(4):
            try:
                with rpc_lock:
                    return inst.client.call(cmd, timeout=30, **kw)
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

    rng = random.Random(a.seed)
    try:
        if a.mode == "solo":
            boot_one("m0", {"slot": 0, "port": free_port(socket.SOCK_DGRAM), "peers": [],
                            **({"synctest": a.synctest} if a.synctest else {})})
        else:
            # --players N: every machine reaches every other one, each way through its own netsim.
            n = a.players
            ports = [free_port(socket.SOCK_DGRAM) for _ in range(n)]
            to = {(i, j): ports[j] for i in range(n) for j in range(n) if i != j}
            if a.rtt > 0 or a.loss > 0:
                link = LinkProfile(latency_ms=a.rtt / 2, jitter_ms=a.jitter, loss=a.loss)
                for i, j in list(to):
                    sim = NetSim(("127.0.0.1", ports[j]), ("127.0.0.1", 0),
                                 NetProfile(up=link, down=link)).start()
                    sims.append(sim)
                    to[(i, j)] = sim.listen_addr[1]
            # All at once: each machine waits only so long for the others (one boot takes ~20 s).
            failures: list[BaseException] = []

            def boot(i: int) -> None:
                peers = [{"slot": j, "host": "127.0.0.1", "port": to[(i, j)]} for j in range(n) if j != i]
                try:
                    boot_one(f"m{i}", {"slot": i, "port": ports[i], "peers": peers}, iso if i == 0 else iso2)
                except BaseException as e:  # noqa: BLE001
                    failures.append(e)

            boots = [threading.Thread(target=boot, args=(i,)) for i in range(n)]
            for t in boots:
                t.start()
            for t in boots:
                t.join()
            if failures:
                raise failures[0]
            insts = dict(sorted(insts.items()))
        names = list(insts)
        deadline = time.time() + 240
        while not all(call(n, "machine_status").get("phase") == "running" for n in names):
            if time.time() > deadline or not all(i.is_running() for i in insts.values()):
                raise SystemExit("the session did not start")
            time.sleep(1)
        print("running; warming up", flush=True)
        t_end = time.time() + a.warmup + a.seconds
        last_pad, stuck = 0.0, {n: [None, 0] for n in names}
        wii_threads: list = []
        while time.time() < t_end:
            if not all(i.is_running() for i in insts.values()):
                raise SystemExit("a Dolphin exited")
            # Not before frame 2400 (past the boot, with a real video backend too): buttons
            # pressed at Brawl's boot message can hold a session's game there for good.
            booted = all((call(n, "machine_status").get("frame") or 0) >= 2400 for n in names)
            if a.input == "random" and booted and a.controller == "wiimote" and not wii_threads:
                def wii_random(n: str, seed: int) -> None:
                    r = random.Random(seed)
                    names_ = ["Buttons/A", "Buttons/B", "Buttons/1", "Buttons/2", "D-Pad/Up", "D-Pad/Down",
                              "D-Pad/Left", "D-Pad/Right"]
                    while time.time() < t_end:
                        held = {r.choice(names_): 1.0} if r.random() < 0.6 else {}
                        try:
                            with rpc_lock:
                                insts[n].client.wiimote_input(0, wiimote=held)
                        except Exception:  # noqa: BLE001
                            pass
                        time.sleep(r.uniform(0.03, 0.03 * a.hold_max))
                for i, n in enumerate(names):
                    th = threading.Thread(target=wii_random, args=(n, a.seed * 10 + i), daemon=True)
                    th.start()
                    wii_threads.append(th)
            if a.input == "random" and booted and a.controller != "wiimote" and time.time() - last_pad > 9:
                for n in names:
                    try:
                        insts[n].client.pad_script(0, random_script(rng, 600, a.hold_max))
                    except Exception as e:  # noqa: BLE001
                        print(f"{n}: pad_script: {e}"[:120], flush=True)
                last_pad = time.time()
            for n in names:
                frame = call(n, "machine_status").get("frame")
                s = stuck[n]
                s[1] = s[1] + 1 if frame is not None and frame == s[0] else 0
                s[0] = frame
                if s[1] == a.stall:
                    print(f"{n}: STALL at frame {frame} ({a.stall} s without a frame)", flush=True)
                    for _ in range(4):
                        c = call(n, "cpu_state")
                        print(f"{n}: pc={c['pc']:08x} lr={c['lr']:08x} ctr={c['ctr']:08x} sp={c['sp']:08x} "
                              f"msr={c['msr']:08x} thread={c['current_thread']:08x}", flush=True)
                        time.sleep(0.2)
                    t_end = time.time()
            time.sleep(1)
        for n in names:
            ms = call(n, "machine_status")
            print(f"{n}: frame={ms.get('frame')} desyncs={ms.get('desyncs')} diag_frames={ms.get('diag_frames')} "
                  f"skipped_loads={ms.get('skipped_loads')} diag={str(ms.get('diag'))[:300]}", flush=True)
    finally:
        for inst in insts.values():
            try:
                inst.stop()
            except Exception:  # noqa: BLE001
                pass
        for s in sims:
            s.stop()
    costs(out, a.skip)
    return 0


def num(rx: str, line: str) -> float:
    m = re.search(rx, line)
    return float(m.group(1)) if m else 0.0


def costs(run_dir: Path, skip: int) -> None:
    """The pace log after `skip` seconds, per machine, weighted by what happened."""
    for m in sorted(Path(run_dir).glob("m*-0")):
        log = (m / "Logs" / "dolphin.log").read_text(errors="replace").splitlines()
        pace = [line for line in log if "machine pace:" in line][skip:]
        cost = [line for line in log if "machine pace costs:" in line][skip:]
        prof = [line for line in log if "machine pace profile:" in line][skip:]
        if not pace:
            print(m.name, "no pace lines")
            continue
        fps = [num(r"\(([\d.]+)/s\)", line) for line in pace]
        secs = sum(num(r"pace: (\d+) ms", line) for line in pace) / 1000
        rb = sum(num(r"(\d+) rollbacks", line) for line in pace)
        rr = sum(num(r"(\d+) re-run", line) for line in pace)
        late = sum(num(r"(\d+) frames over 25", line) for line in pace)
        print(f"{m.name}: {secs:.0f} s  frames/s mean {st.mean(fps):.2f} median {st.median(fps):.2f} "
              f"min {min(fps):.1f}  rollbacks {rb / secs:.2f}/s  re-run {rr / secs:.2f}/s "
              f"({rr / max(rb, 1):.2f} per rollback)  frames over 25 ms {late / secs:.2f}/s")
        if cost:
            per = list(zip(cost, pace))
            r = max(rb, 1)

            def weighted(rx: str) -> float:
                return sum(num(rx, c) * num(r"(\d+) rollbacks", p) for c, p in per) / r

            load_ms = sum(num(r"loads ([\d.]+) ms", c) for c in cost)
            resim_ms = sum(num(r"re-run frames ([\d.]+) ms", c) for c in cost)
            saves = sum(num(r"\); (\d+) saves", c) for c in cost)
            save_ms = sum(num(r"saves ([\d.]+) ms", c) for c in cost)
            dropped = sum(num(r"(\d+) dropped its blocks", c) for c in cost)
            video, devices = weighted(r"video thread ([\d.]+)"), weighted(r"devices ([\d.]+)")
            jit, granules = weighted(r"JIT ([\d.]+), "), weighted(r"devices [\d.]+, (\d+) granules")
            changed = weighted(r"granules, (\d+) changed")
            print(f"    per rollback: load {load_ms / r:.2f} ms (video thread {video:.2f}, devices {devices:.2f}, "
                  f"JIT {jit:.2f}, {granules:.0f} granules, {changed:.0f} changed); "
                  f"{dropped / r * 100:.0f}% of loads dropped the JIT's restored blocks")
            print(f"    per re-run frame: {resim_ms / max(rr, 1):.2f} ms   "
                  f"per save: {save_ms / max(saves, 1):.2f} ms   "
                  f"rollback time: {(load_ms + resim_ms) / secs:.1f} ms/s")
        keys = ["jit", "blocks", "events gpu", "vi", "audio", "ios", "devices", "gpu waits", "gpu busy", "throttle"]
        rxs = [r"jit ([\d.]+)", r"\(([\d.]+) blocks", r"events gpu ([\d.]+)", r"vi ([\d.]+)", r"audio ([\d.]+)",
               r"ios ([\d.]+)", r"devices ([\d.]+)", r"gpu waits ([\d.]+)", r"gpu busy ([\d.]+)", r"throttle ([\d.]+)"]
        for label, part in (("re-run", 0), ("shown", 1)):
            tot, w = [0.0] * len(keys), 0.0
            for line, p in zip(prof, pace):
                seg = line.split("re-run frame:")[1].split("; shown frame:")[part]
                n = num(r"(\d+) re-run", p) if part == 0 else num(r": (\d+) frames", p)
                if n > 0:
                    w += n
                    for i, rx in enumerate(rxs):
                        tot[i] += num(rx, seg) * n
            if w:
                print(f"    {label} frame (ms): " + ", ".join(f"{k} {t / w:.2f}" for k, t in zip(keys, tot)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("out")
    r.add_argument("--mode", choices=["solo", "pair"], required=True)
    r.add_argument("--boot", help="what to boot (a mod's launcher .elf/.dol); the disc by default")
    r.add_argument("--sd", help="the mod's SD card image")
    r.add_argument("--save-from", help="a folder with the game's save files (title 00010000/52534245)")
    r.add_argument("--iso", help="the disc (default: PPHARNESS_ISO or <root>/game/SSBB_NTSC.iso)")
    r.add_argument("--iso2", help="pair: the second player's disc (default: --iso)")
    r.add_argument("--exe", default=str(paths.dolphin_nogui()))
    r.add_argument("--video", default="Null")
    r.add_argument("--platform", default="headless", help="win32 for a real video backend on Windows")
    r.add_argument("--synctest", type=int, default=0)
    r.add_argument("--speed", type=float, default=None, help="emulation speed (0: unlimited)")
    r.add_argument("--single-core", action="store_true")
    r.add_argument("--rtt", type=float, default=0)
    r.add_argument("--jitter", type=float, default=1.0, help="each way, ms (normal, sd)")
    r.add_argument("--loss", type=float, default=0.0, help="each way, the share of datagrams lost (0-1)")
    r.add_argument("--players", type=int, choices=[2, 3, 4], default=2, help="pair: machines in the session")
    r.add_argument("--delay", help="each player's input delay: 0-9 or auto (default: the session's, 2)")
    r.add_argument("--input", choices=["random", "none"], default="random")
    r.add_argument("--controller", choices=["gc", "wiimote"], default="gc",
                   help="what the players play (wiimote: emulated Wii Remote 1, driven by wiimote_input)")
    r.add_argument("--hold-max", type=int, default=20)
    r.add_argument("--seed", type=int, default=3)
    r.add_argument("--seconds", type=float, default=60)
    r.add_argument("--warmup", type=float, default=25)
    r.add_argument("--skip", type=int, default=25, help="pace-log seconds left out of the summary")
    r.add_argument("--stall", type=int, default=20)
    r.add_argument("--env", action="append", default=[])
    r.add_argument("--cfg", action="append", default=[])
    c = sub.add_parser("costs")
    c.add_argument("run_dir")
    c.add_argument("--skip", type=int, default=25)
    a = ap.parse_args()
    if a.cmd == "run":
        return run(a)
    costs(Path(a.run_dir), a.skip)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
