"""Rollback netplay between two machines/OSes (e.g. a Windows host and a Linux container).

Two roles:

``serve``  (on the joiner's machine, e.g. inside the Linux container) launches one Dolphin with no
           game and exposes its harness server on ``0.0.0.0:<--fwd>`` through a small TCP relay
           (Dolphin's harness server only listens on 127.0.0.1). Prints one JSON line with the
           joiner's netplay launcher path, then stays up until the driver quits Dolphin.

``drive``  (on the host's machine, e.g. Windows) launches the host Dolphin locally, connects to the
           remote joiner's harness through the relay, hosts rollback netplay through netsim,
           makes the joiner join, then plays CSS -> Fox vs Falco -> Battlefield -> ~25 s of
           random play (each side drives its own port), and compares the per-frame pads and
           checksums both peers recorded for every confirmed frame (rollback_pad_history),
           exactly like tests/test_rollback.py does for two local instances.

Docker example (Windows host, Linux joiner)::

    # container: publish the relay port to the Windows loopback
    Tools\\docker\\run.ps1 -DockerArgs @("-p","127.0.0.1:47011:47011") bash /src/Tools/docker/test.sh \\
        tool xplat_netplay.py serve --fwd 47011
    # Windows:
    .venv\\Scripts\\python harness\\tools\\xplat_netplay.py drive --remote-port 47011 \\
        --remote-launcher "<launcher path printed by serve>" --join-host host.docker.internal \\
        --exe run\\bin\\rf-15de378723\\DolphinNoGUI.exe --out run\\qa\\xplat-netplay.json

Both builds must report the same revision (Dolphin's netplay refuses other versions).
"""

from __future__ import annotations

import argparse
import contextlib
import json
import logging
import platform
import socket
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessClient, HarnessError  # noqa: E402
from ppharness.instance import DolphinInstance, InstanceConfig, find_free_port  # noqa: E402
from ppharness.netsim import NetSim  # noqa: E402
from ppharness.session import _start_when_ready, get_profile  # noqa: E402

log = logging.getLogger("xplat_netplay")


# --------------------------------------------------------------------------- serve


def _pipe(a: socket.socket, b: socket.socket) -> None:
    try:
        while True:
            data = a.recv(65536)
            if not data:
                break
            b.sendall(data)
    except OSError:
        pass
    finally:
        for s in (a, b):
            with contextlib.suppress(OSError):
                s.shutdown(socket.SHUT_RDWR)


def relay(listen_port: int, target_port: int, stop: threading.Event) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", listen_port))
    srv.listen(4)
    srv.settimeout(0.5)
    while not stop.is_set():
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        up = socket.create_connection(("127.0.0.1", target_port))
        for s in (conn, up):
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=_pipe, args=(conn, up), daemon=True).start()
        threading.Thread(target=_pipe, args=(up, conn), daemon=True).start()
    srv.close()


def serve(fwd: int, cpu_thread: bool, hold_s: float) -> int:
    inst = DolphinInstance("xplat-joiner", config=InstanceConfig(cpu_thread=cpu_thread), boot=None,
                           connect_timeout=90)
    stop = threading.Event()
    ok = False
    try:
        inst.start()
        st = inst.client.status()
        inst.client.close()  # one harness client at a time: leave the slot to the remote driver
        threading.Thread(target=relay, args=(fwd, inst.port, stop), daemon=True).start()
        print(json.dumps({"launcher": str(inst.netplay_launcher()), "fwd": fwd, "harness_port": inst.port,
                          "system": platform.system(), "machine": platform.machine(),
                          "version": st.raw.get("version")}), flush=True)
        deadline = time.monotonic() + hold_s
        while inst.is_running() and time.monotonic() < deadline:
            time.sleep(0.5)
        ok = True
        return 0
    finally:
        stop.set()
        inst.cleanup(ok)


# --------------------------------------------------------------------------- drive


def compare_histories(host: HarnessClient, joiner: HarnessClient) -> Dict[str, Any]:
    """Same as tests/test_rollback.py: pads and per-frame checksums on frames both confirmed."""
    h, j = host.rollback_pad_history(0), joiner.rollback_pad_history(0)
    upto = min(max(h, default=0), max(j, default=0)) - 10
    common = sorted(f for f in h if f in j and f <= upto)
    pads = [f for f in common if h[f][:5] != j[f][:5]]
    state = [f for f in common if len(h[f]) > 8 and h[f][5] and j[f][5] and h[f][5:9] != j[f][5:9]]
    in_match = [f for f in common if len(h[f]) > 8 and h[f][5] and j[f][5]]
    return {"compared": len(common), "in_match": len(in_match), "first": common[0] if common else None,
            "last": common[-1] if common else None, "pad_mismatch": pads[:10], "state_mismatch": state[:10]}


def drive(args: argparse.Namespace) -> Dict[str, Any]:
    report: Dict[str, Any] = {"preset": args.preset, "cpu_thread": args.cpu_thread,
                              "host_system": platform.system()}
    host = DolphinInstance("xplat-host", exe=args.exe, config=InstanceConfig(cpu_thread=args.cpu_thread),
                           boot=None, connect_timeout=90)
    sim: Optional[NetSim] = None
    ok = False
    try:
        hc = host.start().client
        jc = HarnessClient.connect_with_retry(args.remote_port, total_timeout=60, timeout=10)
        report["versions"] = {"host": hc.status().raw.get("version"), "joiner": jc.status().raw.get("version")}
        port = args.netplay_port or find_free_port(kind=socket.SOCK_DGRAM)
        hc.netplay_host(port, host.netplay_launcher(), name="host", rollback=True)
        sim = NetSim(("127.0.0.1", port), (args.sim_listen, args.sim_port), get_profile(args.preset),
                     seed=7).start()
        report["netplay"] = {"host_port": port, "netsim_port": sim.listen_port, "join_host": args.join_host}
        jc.netplay_join(args.join_host, sim.listen_port, name="joiner", game=args.remote_launcher)
        deadline = time.monotonic() + 60
        while not (len(hc.netplay_status().players) >= 2 and jc.netplay_status().connected):
            if time.monotonic() > deadline:
                raise RuntimeError("the remote joiner never connected")
            time.sleep(0.2)
        _start_when_ready(hc, 120)
        sim.reset_epoch()
        deadline = time.monotonic() + 120
        while not all(c.netplay_status().game_running and c.status().running for c in (hc, jc)):
            if time.monotonic() > deadline:
                raise RuntimeError("the game did not start on both")
            time.sleep(0.2)
        hs, js = F.netplay_seat(hc, "host"), F.netplay_seat(jc, "joiner")
        F.to_css([hs, js])
        F.pick_characters([(hs, "fox"), (js, "falco")])
        F.start_and_pick_stage(hs, "battlefield")
        F.wait_match_started([hc, jc])
        t0 = time.monotonic()
        F.play([(hs, "random"), (js, "random")], 60 * args.seconds, seed="xplat")
        report["play_wall_s"] = round(time.monotonic() - t0, 1)
        for c, seat in ((hc, hs), (jc, js)):
            B.neutral(c, seat.pp)
        time.sleep(2)
        report["compare"] = compare_histories(hc, jc)
        report["rollback"] = {"host": hc.netplay_status().rollback, "joiner": jc.netplay_status().rollback}
        report["players"] = {"host": [(p.port, p.character, p.stocks, round(p.damage, 2))
                                      for p in B.read_players(hc.read_mem)],
                             "joiner": [(p.port, p.character, p.stocks, round(p.damage, 2))
                                        for p in B.read_players(jc.read_mem)]}
        report["netsim"] = sim.format_stats()
        cmp = report["compare"]
        report["pass"] = (cmp["compared"] > 1000 and cmp["in_match"] > 600 and not cmp["pad_mismatch"]
                          and not cmp["state_mismatch"]
                          and all((rb or {}).get("desyncs_detected", 0) == 0 for rb in report["rollback"].values()))
        ok = True
        with contextlib.suppress(HarnessError):
            jc.quit()
        return report
    except Exception as e:  # noqa: BLE001 - keep what was measured
        report["error"] = f"{type(e).__name__}: {e}"
        with contextlib.suppress(Exception):
            report["host_log_tail"] = host.log.tail(15)
        return report
    finally:
        if sim is not None:
            sim.stop()
        host.cleanup(ok)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="cross-machine rollback netplay check")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("serve")
    p.add_argument("--fwd", type=int, default=47011)
    p.add_argument("--cpu-thread", type=lambda s: s.lower() in ("1", "on", "true"), default=False)
    p.add_argument("--hold", type=float, default=1800)
    p = sub.add_parser("drive")
    p.add_argument("--remote-port", type=int, default=47011)
    p.add_argument("--remote-launcher", required=True)
    p.add_argument("--join-host", default="host.docker.internal",
                   help="address the joiner uses to reach this machine")
    p.add_argument("--exe", default=None)
    p.add_argument("--preset", default="typical")
    p.add_argument("--netplay-port", type=int, default=None)
    p.add_argument("--sim-listen", default="127.0.0.1")
    p.add_argument("--sim-port", type=int, default=0)
    p.add_argument("--seconds", type=int, default=25)
    p.add_argument("--cpu-thread", type=lambda s: s.lower() in ("1", "on", "true"), default=False)
    p.add_argument("--out", type=Path, default=None)
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    if args.cmd == "serve":
        return serve(args.fwd, args.cpu_thread, args.hold)
    rep = drive(args)
    text = json.dumps(rep, indent=1, default=str)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text)
    print(text)
    return 0 if rep.get("pass") else 1


if __name__ == "__main__":
    sys.exit(main())
