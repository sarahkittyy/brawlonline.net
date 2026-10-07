"""Command line: ``python -m ppharness <command> ...``.

Commands (``python -m ppharness <command> -h`` for options):

  boot      launch one instance, print status (``--detach`` leaves it running)
  status    print the status of a running instance (``--port``)
  cmd       send any protocol command to a running instance
  shot      take a screenshot (of a running instance, or boot one for it)
  stop      ask a running instance to quit
  netplay   host + joiner through netsim with a preset
  probe     check whether the Dolphin build has harness support
  netsim    run the UDP impairment proxy (same as ``python -m ppharness.netsim``)
  bench     measure netsim accuracy (same as ``python -m ppharness.netsim_bench``)
  clean     delete leftover instance directories
  mock      run a mock harness server
  scorecard rollback netplay health: N sessions per netsim preset, JSON + text summary
"""

from __future__ import annotations

import argparse
import json
import logging
import signal
import sys
import threading
import time
from pathlib import Path
from typing import Any

from . import _platform, fake_dolphin, paths
from .client import HarnessClient, HarnessError
from .instance import (DolphinInstance, InstanceConfig, InstanceError, clean_instances,
                       probe_harness_support)
from .netsim import PRESETS


def _print(obj: Any) -> None:
    print(json.dumps(obj, indent=2, default=str), flush=True)


def _bool(text: str) -> bool:
    t = text.lower()
    if t in ("1", "on", "true", "yes"):
        return True
    if t in ("0", "off", "false", "no"):
        return False
    raise argparse.ArgumentTypeError(f"expected on/off, got {text!r}")


def _add_instance_args(p: argparse.ArgumentParser, *, video_default: str | None = "Null") -> None:
    p.add_argument("--exe", help=f"DolphinNoGUI path (default {paths.dolphin_nogui()})")
    p.add_argument("--fake", action="store_true", help="use the fake Dolphin (mock server)")
    p.add_argument("--template", help=f"template user dir (default {paths.template_user_dir()})")
    p.add_argument("--video", default=video_default, help="video backend (default %(default)s)")
    p.add_argument("--platform", default=None, help="NoGUI platform (default headless)")
    p.add_argument("--cpu-thread", type=_bool, default=None, metavar="on|off")
    p.add_argument("--ports", default="0,1", help="ports set to Standard Controller (default 0,1)")
    p.add_argument("--audio", action="store_true", help="enable sound (muted by default)")
    p.add_argument("--keep", action="store_true", help="keep the instance dir afterwards")
    p.add_argument("--name", default=None, help="instance name prefix")


def _exe(args: argparse.Namespace) -> Any:
    if getattr(args, "fake", False):
        return fake_dolphin.command()
    return args.exe or None


def _config(args: argparse.Namespace, video: str | None) -> InstanceConfig:
    ports = tuple(int(x) for x in str(args.ports).split(",") if x.strip() != "")
    return InstanceConfig(cpu_thread=args.cpu_thread, video_backend=video or "Null",
                          standard_controllers=ports, audio=args.audio)


def _wait_signal(seconds: float | None) -> None:
    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    stop.wait(seconds if seconds and seconds > 0 else None)


def _connect(port: int, timeout: float) -> HarnessClient:
    return HarnessClient.connect(port, timeout=timeout)


# --------------------------------------------------------------------------- commands


def cmd_boot(args: argparse.Namespace) -> int:
    boot = None if args.boot == "none" else args.boot
    inst = DolphinInstance(args.name or "boot", exe=_exe(args), template=args.template,
                           config=_config(args, args.video), boot=boot,
                           platform=args.platform or "headless", keep=args.keep,
                           detach=args.detach, connect_timeout=args.timeout)
    ok = False
    try:
        inst.start()
        c = inst.client
        assert c is not None
        if args.wait_frames:
            c.wait_frames(args.wait_frames, timeout_ms=int(args.timeout * 1000))
        st = c.status()
        info = {"name": inst.name, "port": inst.port, "pid": inst.process.pid if inst.process else None,
                "user_dir": str(inst.user_dir), "status": st.raw}
        if args.detach:
            c.close()
            info["note"] = (f"left running; use `python -m ppharness status --port {inst.port}` "
                            f"and `python -m ppharness stop --port {inst.port}`; remove the dir "
                            f"afterwards with `python -m ppharness clean`")
            _print(info)
            ok = True
            return 0
        _print(info)
        if args.hold is not None:
            print("holding; Ctrl-C to stop", file=sys.stderr, flush=True)
            _wait_signal(args.hold)
        ok = True
        return 0
    except (HarnessError, InstanceError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        if not (args.detach and ok):
            inst.detach = False
            inst.cleanup(ok)
            kept = inst.user_dir is not None and inst.user_dir.exists()
            print(f"exit code {inst.exit_code}{' (killed)' if inst.killed else ''}; "
                  f"instance dir {'kept: ' + str(inst.user_dir) if kept else 'removed'}",
                  file=sys.stderr)


def cmd_status(args: argparse.Namespace) -> int:
    with _connect(args.port, args.timeout) as c:
        st = c.status()
        out = st.raw
        if args.netplay:
            out = {"status": st.raw, "netplay_status": c.netplay_status().raw}
        _print(out)
    return 0


def _parse_value(text: str) -> Any:
    try:
        return json.loads(text)
    except ValueError:
        if text.lower().startswith("0x"):
            return int(text, 16)
        return text


def cmd_cmd(args: argparse.Namespace) -> int:
    kw: dict[str, Any] = {}
    for item in args.args:
        if "=" not in item:
            print(f"error: argument {item!r} must be key=value", file=sys.stderr)
            return 2
        k, v = item.split("=", 1)
        kw[k] = _parse_value(v)
    with _connect(args.port, args.timeout) as c:
        try:
            result = c.call(args.command, timeout=args.timeout, **kw)
        except HarnessError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        _print(result)
    return 0


def cmd_stop(args: argparse.Namespace) -> int:
    with _connect(args.port, args.timeout) as c:
        c.quit()
    print("quit sent", file=sys.stderr)
    return 0


def cmd_shot(args: argparse.Namespace) -> int:
    out = Path(args.path).expanduser().resolve()
    if args.port:
        with _connect(args.port, args.timeout) as c:
            _print(vars(c.screenshot(out)))
        return 0
    platform, video = _platform.default_screenshot_setup()
    inst = DolphinInstance(args.name or "shot", exe=_exe(args), template=args.template,
                           config=_config(args, args.video or video), boot=args.boot,
                           platform=args.platform or platform, keep=args.keep,
                           connect_timeout=args.timeout)
    ok = False
    try:
        inst.start()
        assert inst.client is not None
        inst.client.wait_frames(args.frames, timeout_ms=int(args.timeout * 1000))
        shot = inst.client.screenshot(out)
        _print({"path": str(shot.path), "width": shot.width, "height": shot.height})
        ok = True
        return 0
    except (HarnessError, InstanceError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        inst.cleanup(ok)


def cmd_netplay(args: argparse.Namespace) -> int:
    from .session import two_player_netplay

    cfg = _config(args, args.video)
    try:
        sess = two_player_netplay(args.preset, rollback=args.rollback, delay=args.delay,
                                  seed=args.seed, name=args.name or "netplay", exe=_exe(args),
                                  template=args.template, config=cfg, keep=args.keep,
                                  start_game=not args.no_start)
    except (HarnessError, InstanceError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    ok = False
    try:
        info = {i.name: {"port": i.port, "user_dir": str(i.user_dir)} for i in sess.instances}
        info["netsim"] = {"listen": sess.netsim.listen_port, "forward": sess.netplay_port,
                          "profile": sess.netsim.profile.describe()}
        _print(info)
        stop = threading.Event()
        signal.signal(signal.SIGINT, lambda *_: stop.set())
        t_end = time.monotonic() + args.duration if args.duration > 0 else float("inf")
        while not stop.is_set() and time.monotonic() < t_end:
            stop.wait(min(args.interval, max(0.0, t_end - time.monotonic())))
            line = []
            for i in sess.instances:
                try:
                    st = i.client.status() if i.client else None  # type: ignore[union-attr]
                    np = i.client.netplay_status() if i.client else None  # type: ignore[union-attr]
                    pings = [p.ping_ms for p in np.players] if np else []
                    line.append(f"{i.name}: {st.state if st else '?'} frame={st.frame if st else '?'} "
                                f"ping={pings} rb={np.rollback if np else {}}")
                except HarnessError as e:
                    line.append(f"{i.name}: error {e}")
            print(" | ".join(line), flush=True)
            print(sess.netsim.format_stats(), flush=True)
        ok = True
        return 0
    finally:
        print(sess.netsim.format_stats(), file=sys.stderr)
        sess.close(success=ok)


def cmd_probe(args: argparse.Namespace) -> int:
    ok, detail = probe_harness_support(args.exe, args.template, timeout=args.timeout)
    _print({"harness": ok, "exe": str(args.exe or paths.dolphin_nogui()), "detail": detail})
    return 0 if ok else 1


def cmd_clean(args: argparse.Namespace) -> int:
    root = Path(args.root) if args.root else None
    for d, action in clean_instances(root, dry_run=args.dry_run,
                                     prefix=None if args.all else args.prefix,
                                     include_kept=args.include_kept):
        print(f"{action}: {d}")
    return 0


# --------------------------------------------------------------------------- parser


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="python -m ppharness", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-v", "--verbose", action="count", default=0)
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("boot", help="launch one instance and print its status")
    _add_instance_args(p)
    p.add_argument("--boot", default="offline",
                   help="offline | netplay | none | path to .dol/.iso (default offline)")
    p.add_argument("--wait-frames", type=int, default=0, help="wait N VI frames before printing")
    p.add_argument("--detach", action="store_true", help="leave Dolphin running and exit")
    p.add_argument("--hold", type=float, nargs="?", const=0.0, default=None, metavar="SECONDS",
                   help="keep running (until Ctrl-C, or for SECONDS), then quit")
    p.add_argument("--timeout", type=float, default=60.0)
    p.set_defaults(func=cmd_boot)

    p = sub.add_parser("status", help="status of a running instance")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--netplay", action="store_true", help="also print netplay_status")
    p.add_argument("--timeout", type=float, default=10.0)
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("cmd", help="send a raw protocol command: cmd --port N read_u32 addr=0x80000000")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--timeout", type=float, default=30.0)
    p.add_argument("command")
    p.add_argument("args", nargs="*", help="key=value (values parsed as JSON, or 0x hex)")
    p.set_defaults(func=cmd_cmd)

    p = sub.add_parser("stop", help="send quit to a running instance")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--timeout", type=float, default=10.0)
    p.set_defaults(func=cmd_stop)

    p = sub.add_parser("shot", help="screenshot into PATH (.png)")
    p.add_argument("path")
    p.add_argument("--port", type=int, help="use an already running instance")
    _add_instance_args(p, video_default=None)
    p.add_argument("--boot", default="offline")
    p.add_argument("--frames", type=int, default=1500,
                   help="VI frames to wait after boot (P+ reaches character select by ~1500)")
    p.add_argument("--timeout", type=float, default=120.0)
    p.set_defaults(func=cmd_shot)

    p = sub.add_parser("netplay", help="two instances through netsim")
    _add_instance_args(p)
    p.add_argument("--preset", default="typical", choices=sorted(PRESETS))
    p.add_argument("--rollback", dest="rollback", action="store_true", default=True)
    p.add_argument("--no-rollback", dest="rollback", action="store_false")
    p.add_argument("--delay", type=int, default=None)
    p.add_argument("--seed", default="0")
    p.add_argument("--duration", type=float, default=0.0, help="seconds (0 = until Ctrl-C)")
    p.add_argument("--interval", type=float, default=5.0, help="status print interval")
    p.add_argument("--no-start", action="store_true", help="connect but do not start the game")
    p.set_defaults(func=cmd_netplay)

    p = sub.add_parser("probe", help="does the Dolphin build answer ping on --harness-port?")
    p.add_argument("--exe")
    p.add_argument("--template")
    p.add_argument("--timeout", type=float, default=15.0)
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("clean", help="remove instance dirs whose process is gone")
    p.add_argument("--root", help=f"default {paths.instances_root()}")
    p.add_argument("--dry-run", action="store_true")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--prefix", help="only dirs whose name starts with this (your own runs)")
    g.add_argument("--all", action="store_true",
                   help="every instance dir, including other sessions' (use with care)")
    p.add_argument("--include-kept", action="store_true",
                   help="also remove kept-* dirs that another session kept after a failure")
    p.set_defaults(func=cmd_clean)

    for name, help_ in (("netsim", "UDP impairment proxy"), ("bench", "measure netsim accuracy"),
                        ("mock", "run a mock harness server"),
                        ("scorecard", "rollback netplay health over netsim presets")):
        p = sub.add_parser(name, help=help_, add_help=False)
        p.add_argument("rest", nargs=argparse.REMAINDER)
        p.set_defaults(func=None)
    return ap


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    # Pass-through commands keep their own parsers (and their own -h).
    if argv and argv[0] in ("netsim", "bench", "mock", "scorecard"):
        if argv[0] == "scorecard":
            from .scorecard import main as m
        elif argv[0] == "netsim":
            from .netsim import main as m
        elif argv[0] == "bench":
            from .netsim_bench import main as m
        else:
            from .mock_server import main as m
        return m(argv[1:])
    ap = build_parser()
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.WARNING - 10 * min(args.verbose, 2),
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    try:
        return int(args.func(args) or 0)
    except HarnessError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
