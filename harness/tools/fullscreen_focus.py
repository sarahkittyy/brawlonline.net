"""A focus change in exclusive fullscreen during a gameplay rollback session (Windows, D3D11).

The host is Dolphin.exe (Qt) in fullscreen with D3D11 (exclusive fullscreen, Dolphin's default)
and dual core, the joiner DolphinNoGUI; they play a match as gprb_session.py does. Once the
host's session runs, this takes the foreground from the host's render window to its main window
and back, ``--cycles`` times, and after each switch asks the render window for a WM_NULL with
SendMessageTimeout: a window that does not answer in ``--hang-s`` seconds is hung (its UI thread
has not run its message loop).

Player report 2026-10-15 (DR#950, v0.1.75, a ranked match): a focus change in exclusive fullscreen
hung Dolphin for good. Host::SetRenderFocus paused the core from the UI thread
(RunWithGPUThreadInactive), whose FifoManager::PauseAndLock waited for the deterministic video
thread without running the UI's messages, while that thread's Present waited for the UI thread
to answer DXGI's window message.

    python harness/tools/fullscreen_focus.py

Run it from a shell on the interactive desktop: a process in a service window station (as
Claude Code's tool shells are) gets no visible windows and Dolphin.exe never boots. The screen
switches to and from fullscreen while it runs; the joiner plays headless.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gameplay_rollback as G  # noqa: E402
import gprb_session as S  # noqa: E402
from ppharness import paths  # noqa: E402
from ppharness.instance import DolphinInstance  # noqa: E402

user32 = ctypes.windll.user32
WM_NULL = 0
SMTO_ABORTIFHUNG = 0x2
VK_MENU = 0x12
KEYEVENTF_KEYUP = 0x2
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


class QtInstance(DolphinInstance):
    """Dolphin.exe: no `-p` (a DolphinNoGUI option)."""

    def build_args(self) -> list[str]:
        args = super().build_args()
        i = args.index("-p")
        return args[:i] + args[i + 2:]


def windows_of(pid: int) -> List[Dict[str, Any]]:
    """The visible top-level windows of a process: handle, title, rectangle."""
    out: List[Dict[str, Any]] = []

    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            r = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))
            out.append({"hwnd": hwnd, "title": buf.value, "rect": (r.left, r.top, r.right, r.bottom)})
        return True
    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return out


def responds(hwnd: int, timeout_s: float) -> bool:
    res = ctypes.c_size_t()
    return bool(user32.SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, int(timeout_s * 1000),
                                           ctypes.byref(res)))


def to_foreground(hwnd: int) -> None:
    # Windows lets a background process take the foreground right after a key event.
    user32.keybd_event(VK_MENU, 0, 0, 0)
    user32.keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0)
    user32.SetForegroundWindow(hwnd)


def focus_cycles(host: DolphinInstance, args: argparse.Namespace, rep: Dict[str, Any], stop: threading.Event) -> None:
    rep["cycles"] = []
    t0 = time.monotonic()
    while not stop.is_set():
        if host.log_path.exists() and "gprb: session running" in host.log_path.read_text(errors="replace"):
            break
        if time.monotonic() - t0 > 600:
            rep["focus_error"] = "the host's session never ran"
            return
        time.sleep(0.5)
    time.sleep(args.settle_s)
    pid = host.process.pid
    wins = windows_of(pid)
    rep["windows"] = [{"title": w["title"], "rect": w["rect"]} for w in wins]
    if len(wins) < 2:
        rep["focus_error"] = f"expected the render and the main window, got {rep['windows']}"
        return
    # The render window is the fullscreen one (the largest).
    wins.sort(key=lambda w: (w["rect"][2] - w["rect"][0]) * (w["rect"][3] - w["rect"][1]), reverse=True)
    render, main = wins[0]["hwnd"], wins[1]["hwnd"]
    for i in range(args.cycles):
        if stop.is_set():
            break
        for what, hwnd in (("away", main), ("back", render)):
            to_foreground(hwnd)
            time.sleep(args.hold_s)
            t = time.monotonic()
            ok = responds(render, args.hang_s)
            c = {"cycle": i, "switch": what, "responds": ok, "answer_s": round(time.monotonic() - t, 2),
                 "foreground_is_render": user32.GetForegroundWindow() == render}
            rep["cycles"].append(c)
            print(f"  cycle {i} {what}: {'responds' if ok else 'HUNG'} ({c['answer_s']} s)", flush=True)
            if not ok:
                rep["hung"] = True
                return


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cycles", type=int, default=6, help="focus away-and-back switches")
    ap.add_argument("--hold-s", type=float, default=2.0, help="seconds after each switch before the check")
    ap.add_argument("--hang-s", type=float, default=10.0, help="a window that answers no WM_NULL in this is hung")
    ap.add_argument("--settle-s", type=float, default=5.0, help="seconds of the match before the first switch")
    ap.add_argument("--frames", type=int, default=3600, help="session frames the match lasts at most")
    ap.add_argument("--exe", default=None, help="the host's Dolphin.exe (default: the build's)")
    ap.add_argument("--json", default=None)
    a, rest = ap.parse_known_args(argv)

    host: List[DolphinInstance] = []
    orig = G.make_instance

    def mk(name: str, **kw):
        if "-host-" not in name:
            return orig(name, **kw)
        kw["video"] = "D3D11"
        kw["config"] = [*kw.get("config", ()), "Dolphin.Display.Fullscreen=True"]
        inst = orig(name, **kw)
        # The same instance directory, set up by make_instance; Dolphin.exe runs it.
        inst.__class__ = QtInstance
        inst.command_prefix = [str(Path(a.exe) if a.exe else paths.dolphin_gui())]
        host.append(inst)
        return inst

    rep: Dict[str, Any] = {}
    stop = threading.Event()
    th: Optional[threading.Thread] = None

    orig_instances = G.instances

    def instances(specs, keep=False):
        nonlocal th
        cm = orig_instances(specs, keep)
        insts = cm.__enter__()
        th = threading.Thread(target=focus_cycles, args=(host[0], a, rep, stop), daemon=True)
        th.start()

        class _Cm:
            def __enter__(self):
                return insts

            def __exit__(self, *exc):
                stop.set()
                th.join(a.hang_s + a.hold_s + 5)
                return cm.__exit__(*exc)
        return _Cm()

    G.make_instance = mk
    G.instances = instances
    try:
        S.main(["--cpu", "dc", "--preset", "lan", "--frames", str(a.frames), *rest])
    finally:
        G.make_instance, G.instances = orig, orig_instances
    hung = bool(rep.get("hung"))
    ok = not hung and not rep.get("focus_error") and len(rep.get("cycles", [])) == 2 * a.cycles
    print(f"fullscreen_focus: {'PASS' if ok else 'FAIL'}: {len(rep.get('cycles', []))} checks, "
          f"hung {hung}{', ' + rep['focus_error'] if rep.get('focus_error') else ''}")
    if a.json:
        Path(a.json).write_text(json.dumps(rep, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
