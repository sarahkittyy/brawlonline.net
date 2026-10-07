"""Online hand-off to the gameplay-only session (docs/backend-design.md 5.5, gameplay-rollback-status.md).

Starts our accounts/mm server (ppharness.backend), logs two independently booted instances in,
brings both to the character select by different menu paths, and runs a Direct search with
``backend="gameplay"``: matchmaking hands the connected peer to ``Gprb::Session`` on the punched
port. Then one match is played under gameplay-only rollback and compared like gprb_session.py,
followed by one of Slippi's disconnect cases:

- ``leave-css``: after the match both return to the CSS and the guest leaves (mm_cancel): the
  host must see it at once (a "leave" message), not after the 7.2 s timeout;
- ``leave-match``: the guest leaves during the next match: the host's match ends at once;
- ``freeze``: during the next match the guest freezes for 6 s (stays), then for longer: the host
  drops it after ~7.2 s (Online::PeerSilenceTimeoutMs).

    python harness/tools/gprb_online.py --after leave-css --json run/qa/gprb5/online.json
"""

from __future__ import annotations

import argparse
import contextlib
import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gameplay_rollback as G  # noqa: E402
import gprb_session as S  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness.backend import OnlineBackend  # noqa: E402


class _NoSim:
    def reset_epoch(self) -> None:
        pass


def _freeze(pid: int, suspend: bool) -> None:
    import ctypes
    ntdll, k32 = ctypes.WinDLL("ntdll"), ctypes.WinDLL("kernel32")
    h = k32.OpenProcess(0x0800, False, pid)
    try:
        (ntdll.NtSuspendProcess if suspend else ntdll.NtResumeProcess)(h)
    finally:
        k32.CloseHandle(h)


def session(c) -> Dict[str, Any]:
    return c.call("mm_status").get("session") or {}


def gstatus(c) -> Dict[str, Any]:
    return c.call("gprb_status")


def run(args: argparse.Namespace) -> Dict[str, Any]:
    rep: Dict[str, Any] = {"after": args.after, "cpu": args.cpu, "region_set": args.region_set}
    t0 = time.monotonic()
    be = OnlineBackend().start()
    insts = []
    try:
        ua, ub = be.create_user("anna", "ANNA"), be.create_user("bert", "BERT")
        ini = {"Online": {"UseDevServer": True, "MatchmakingPort": be.mm_port,
                          "DevAccountsUrl": be.accounts_url}}
        ia = G.make_instance("gprb-onl-a", cpu_thread=args.cpu == "dc", dolphin_ini=ini)
        ib = G.make_instance("gprb-onl-b", cpu_thread=args.cpu == "dc", dolphin_ini=ini, rtc=0x69C9A3D0)
        insts = [ia, ib]
        ua.write_user_json(ia.user_dir)
        ub.write_user_json(ib.user_dir)
        G.par([lambda: (ia.launch(), ia.connect()), lambda: (ib.launch(), ib.connect())])
        for i in insts:
            i.client.wait_state("running", timeout=120)
            for port in (0, 1):
                i.client.pad_set(port)
        ca, cb = ia.client, ib.client
        G.par([lambda: B.wait_scene(ca, [B.Scene.CSS], 60 * 120), lambda: B.wait_scene(cb, [B.Scene.CSS], 60 * 120)])
        S.wait(lambda: all(c.call("online_status")["logged_in"] for c in (ca, cb)), 30, "the logins")
        common = dict(session="auto", backend="gameplay", delay=args.delay, region_set=args.region_set)
        ca.call("mm_search_direct", code=ub.connect_code, selections={"character": args.p1, "stage": args.stage},
                **common)
        cb.call("mm_search_direct", code=ua.connect_code, selections={"character": args.p2}, **common)
        t_search = time.monotonic()
        st = S.wait(lambda: (lambda s: s if all(x["state"] == "connection_success" for x in s) else None)(
            [c.call("mm_status") for c in (ca, cb)]), 60, "matchmaking")
        rep["matchmaking_s"] = round(time.monotonic() - t_search, 2)
        host_is_a = st[0]["match"]["is_host"]
        host, guest = (ca, cb) if host_is_a else (cb, ca)
        hinst, ginst = (ia, ib) if host_is_a else (ib, ia)
        S.wait(lambda: all(gstatus(c)["phase"] == "connected" for c in (host, guest)), 20, "the session to connect")
        rep["connected_s"] = round(time.monotonic() - t_search, 2)
        rep["session"] = [session(c) for c in (host, guest)]
        rep["host_peer"] = gstatus(host).get("peer")
        hinst.wait_for_log(r"gprb: online match .* as host", timeout=5)

        # Match 1, the host plays P1 (port 0), the guest P2.
        m: Dict[str, Any] = {}
        S.play_match(host, guest, _NoSim(), args, "online", args.cpu, 0, 0, (args.p1, args.p2, args.stage), m)
        rep["match"] = {k: m.get(k) for k in ("agreed", "checksums", "trace", "error")}
        rep["match"]["status"] = [{k: s.get(k) for k in ("current_frame", "rollbacks", "end_reason", "desyncs_detected")}
                                  for s in (m.get("status") or [])]

        if args.after == "leave-css":
            G.par([lambda: B.results_to_css(host, [0, 1]), lambda: B.results_to_css(guest, [0, 1])])
            t = time.monotonic()
            guest.call("mm_cancel")
            S.wait(lambda: gstatus(host)["disconnected"], 10, "the host to see the guest leave", every=0.02)
            rep["leave_seen_s"] = round(time.monotonic() - t, 3)
            rep["host_after"] = {k: gstatus(host)[k] for k in ("phase", "error", "disconnected")}
        else:
            # Next match: start it, play a little, then leave / freeze.
            S.wait(lambda: all(gstatus(c)["phase"] == "connected" for c in (host, guest)), 30, "the next match")
            sel_args = argparse.Namespace(**vars(args))
            G.par([lambda: B.results_to_css(host, [0, 1]), lambda: B.results_to_css(guest, [0, 1])])
            host.call("gprb_set_selections", selections={"character": args.p1, "stage": args.stage, "match": 1})
            guest.call("gprb_set_selections", selections={"character": args.p2, "match": 1})
            G.par([lambda: S.second_history(host, [args.p1, args.p2], args.stage, []),
                   lambda: S.second_history(guest, [args.p1, args.p2], args.stage, [])])
            S.wait(lambda: all(gstatus(c)["phase"] == "running" for c in (host, guest)), 180, "match 2 to start")
            f0 = gstatus(host)["current_frame"]
            S.wait(lambda: gstatus(host)["current_frame"] > f0 + 300, 60, "match 2 frames")
            if args.after == "leave-match":
                t = time.monotonic()
                guest.call("mm_cancel")
                S.wait(lambda: gstatus(host)["disconnected"], 10, "the host to see the guest leave", every=0.02)
                rep["leave_seen_s"] = round(time.monotonic() - t, 3)
            else:
                pid = ginst.process.pid
                _freeze(pid, True)
                try:
                    time.sleep(6.0)
                    rep["after_6s"] = {k: gstatus(host)[k] for k in ("phase", "disconnected")}
                finally:
                    _freeze(pid, False)
                f1 = gstatus(host)["current_frame"]
                S.wait(lambda: gstatus(host)["current_frame"] > f1 + 120, 30, "the match to go on after 6 s")
                _freeze(pid, True)
                t = time.monotonic()
                try:
                    S.wait(lambda: gstatus(host)["disconnected"], 15, "the host to drop the frozen guest", every=0.05)
                    rep["dropped_after_s"] = round(time.monotonic() - t, 2)
                finally:
                    _freeze(pid, False)
            rep["host_after"] = {k: gstatus(host)[k] for k in ("phase", "error", "disconnected", "end_reason")}
            time.sleep(1)
            with contextlib.suppress(Exception):
                rep["guest_after"] = {k: gstatus(guest)[k] for k in ("phase", "error", "disconnected", "end_reason")}
            rep["host_scene"] = B.read_scene(host.read_mem).name
    except Exception as e:  # noqa: BLE001
        rep["error"] = f"{type(e).__name__}: {e}"
    finally:
        if args.log_dir:
            import shutil
            d = Path(args.log_dir)
            d.mkdir(parents=True, exist_ok=True)
            for i in insts:
                with contextlib.suppress(OSError):
                    shutil.copy(i.user_dir / "Logs" / "dolphin.log", d / f"{i.name}-{args.after}.log")
        for i in insts:
            with contextlib.suppress(Exception):
                if i.is_running():
                    i.kill()
            with contextlib.suppress(Exception):
                i.cleanup(True)
        be.stop()
    rep["wall_s"] = round(time.monotonic() - t0, 1)
    print(json.dumps({k: v for k, v in rep.items() if k not in ("session",)}, indent=1, default=str), flush=True)
    return rep


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--after", choices=("leave-css", "leave-match", "freeze"), default="leave-css")
    ap.add_argument("--cpu", default="sc", choices=("sc", "dc"))
    ap.add_argument("--p1", default="fox")
    ap.add_argument("--p2", default="falco")
    ap.add_argument("--stage", default="battlefield")
    ap.add_argument("--delay", type=int, default=2)
    ap.add_argument("--region-set", default="gp-v11")
    ap.add_argument("--start-frame", type=int, default=240)
    ap.add_argument("--frames", type=int, default=30000)
    ap.add_argument("--minutes", type=int, default=2)
    ap.add_argument("--timeout", type=float, default=400)
    ap.add_argument("--json", default=None)
    ap.add_argument("--log-dir", default=None)
    args = ap.parse_args(argv)
    rep = run(args)
    if args.json:
        p = Path(args.json)
        prev = json.loads(p.read_text()) if p.exists() else []
        prev.append(rep)
        p.write_text(json.dumps(prev, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
