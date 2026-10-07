"""A stand-in for DolphinNoGUI: same command line, runs the mock harness server.

Used by the instance and session tests, and handy for developing harness code
without a Dolphin build::

    DolphinInstance("dev", exe=fake_dolphin.command())

It accepts Dolphin's flags (``-u -p -v -e -C --harness-port``, ``PPR_HARNESS_PORT``),
writes ``<user>/Logs/dolphin.log`` and prints a little to stdout/stderr.

Misbehaviour for tests is selected with the ``PPH_FAKE`` environment variable, a
comma-separated list of:

``ignore_quit``       answer ``quit`` but keep running, and ignore SIGTERM (tests the kill path)
``crash``             exit with code 3 before listening
``no_harness``        reject ``--harness-port`` like an old build (exit code 2)
``listen_delay=S``    wait S seconds before listening (tests connect-with-retry)
``exit_code=N``       exit code after a clean quit
``speed=X``           emulation speed factor for the fake frames
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import threading
import time
from pathlib import Path

PACKAGE_ROOT = Path(__file__).resolve().parents[1]


def command() -> list[str]:
    """Command prefix that runs this module with the current interpreter."""
    code = (f"import sys; sys.path.insert(0, {str(PACKAGE_ROOT)!r}); "
            "from ppharness.fake_dolphin import main; sys.exit(main())")
    return [sys.executable, "-c", code]


def _parse_knobs() -> dict[str, str]:
    knobs: dict[str, str] = {}
    for item in os.environ.get("PPH_FAKE", "").split(","):
        item = item.strip()
        if not item:
            continue
        k, _, v = item.partition("=")
        knobs[k] = v or "1"
    return knobs


def main(argv: list[str] | None = None) -> int:
    knobs = _parse_knobs()
    ap = argparse.ArgumentParser(prog="fake-dolphin", add_help=True)
    ap.add_argument("-u", "--user")
    ap.add_argument("-p", "--platform", default="headless")
    ap.add_argument("-v", "--video_backend", default="")
    ap.add_argument("-e", "--exec", action="append")
    ap.add_argument("-C", "--config", action="append", default=[])
    ap.add_argument("-a", "--audio_emulation")
    if "no_harness" not in knobs:
        ap.add_argument("--harness-port", type=int)
    args = ap.parse_args(argv)

    user = Path(args.user) if args.user else Path.cwd() / "fake-user"
    log_path = user / "Logs" / "dolphin.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = open(log_path, "a", encoding="utf-8", buffering=1)
    lock = threading.Lock()

    def log(text: str) -> None:
        with lock:
            stamp = time.strftime("%H:%M:%S")
            log_file.write(f"{stamp} Core/FakeDolphin: {text}\n")

    print(f"fake-dolphin starting pid={os.getpid()} user={user}", flush=True)
    print("fake-dolphin: this is stderr", file=sys.stderr, flush=True)
    log(f"Fake Dolphin starting, platform={args.platform} video={args.video_backend}")
    for c in args.config:
        log(f"Config override: {c}")
    log(f"PPR_HARNESS_AUDIO={os.environ.get('PPR_HARNESS_AUDIO', '')}")
    if "crash" in knobs:
        print("fake-dolphin: simulated crash", file=sys.stderr, flush=True)
        log("Simulated crash")
        log_file.close()
        return 3

    port = getattr(args, "harness_port", None)
    if port is None and os.environ.get("PPR_HARNESS_PORT"):
        port = int(os.environ["PPR_HARNESS_PORT"])
    if port is None:
        log("No harness port, exiting")
        return 0

    if "listen_delay" in knobs:
        time.sleep(float(knobs["listen_delay"]))

    if "ignore_quit" in knobs and hasattr(signal, "SIGTERM") and sys.platform != "win32":
        # Stubborn for real: the POSIX shutdown path tries SIGTERM before SIGKILL.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)

    from .mock_server import MockHarnessServer

    done = threading.Event()
    video = args.video_backend or "Null"
    srv = MockHarnessServer(
        port,
        boot=bool(args.exec),
        video_backend=video,
        speed=float(knobs.get("speed", "1.0")),
        log_fn=log,
        on_quit=None if "ignore_quit" in knobs else done.set,
    )
    try:
        srv.start()
    except OSError as e:
        print(f"fake-dolphin: cannot listen on {port}: {e}", file=sys.stderr, flush=True)
        return 4
    log(f"Harness: listening on 127.0.0.1:{srv.port}")
    if args.exec:
        log(f"Booting {args.exec[0]}")
    print(f"fake-dolphin: harness on {srv.port}", flush=True)
    try:
        while not done.wait(0.2):
            pass
    except KeyboardInterrupt:
        pass
    srv.stop()
    log("Shutdown complete")
    log_file.close()
    print("fake-dolphin: bye", flush=True)
    return int(knobs.get("exit_code", "0"))


if __name__ == "__main__":
    raise SystemExit(main())
