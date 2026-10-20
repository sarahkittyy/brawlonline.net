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
    python harness/tools/fullscreen_focus.py --solo
    python harness/tools/fullscreen_focus.py --solo --solo-gpu fake-completion

Bug report #62 (2026-10-19, launcher 0.1.89, Ranked game 1's character screen): the same hang
one step earlier. Outside a session the video thread is not the deterministic one (GPU
determinism "auto", the installs' default since Phase 13), so the CPU thread idling at the CSS
waits for the video thread in FifoManager::FlushGpu; the UI thread's pause waited in
CPU::PauseAndLock for the CPU thread, and the video thread's Present for the UI thread. The fix:
the UI thread no longer pauses for a fullscreen change at all, it only records the request
(SwapChain::SetFullscreen) and the video thread applies it at its next frame.

Caveat of the session mode: the harness's own requests (status, memory reads) pause the core
from the harness thread, which holds Core's core mutex while it waits for the video thread; a
focus change's pause on the UI thread then waited for that mutex, and the video thread's Present
for the UI thread. That hang was of the test's making (in a player's Dolphin no other thread
pauses the core during a match): both builds with and without 6de3ac60 hung on the 5th switch
(2026-10-15). A focus change no longer pauses, and the session mode passes (2026-10-19).
--solo boots the host alone and sends nothing while the focus moves. --solo-at css (the default)
first walks to the Versus CSS, as in bug #62; --solo-at menus stays at Brawl's menus.
--solo-gpu unset (the default) leaves GPU determinism to the installs' default ("auto": not
deterministic outside a session, bug #62's case); --solo-gpu fake-completion makes the video
thread the deterministic one, as in a match (DR#950's case). The report's
"deterministic_gpu_thread" says which one ran. 2026-10-19: --solo (CSS, deterministic GPU thread
false) hung the build before the fix (6914e018) on the 2nd switch, its hang watchdog firing as in
bug #62; with the fix it passed 12 checks, as did --solo-at menus --solo-gpu fake-completion and
the session mode.

Run it from a shell on the interactive desktop: a process in a service window station (as
Claude Code's tool shells are) gets no visible windows and Dolphin.exe never boots. A scheduled
task with /IT runs on the desktop of the signed-in user (schtasks /Create /TN ... /TR <a .cmd>
/SC ONCE /ST 23:59 /IT, then /Run). The screen switches to and from fullscreen while it runs; the
joiner plays headless. Other windows taking the foreground (Steam, a browser) make a switch not
take, which fails the run ("NOT switched").
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import ctypes.wintypes as wt
import json
import re
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gameplay_rollback as G  # noqa: E402
import gprb_session as S  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import paths  # noqa: E402
from ppharness.instance import DolphinInstance  # noqa: E402
from ppharness.inifile import IniFile  # noqa: E402

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


user32.GetForegroundWindow.restype = wt.HWND
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.BringWindowToTop.argtypes = [wt.HWND]
user32.SendMessageTimeoutW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM, wt.UINT, wt.UINT,
                                       ctypes.POINTER(ctypes.c_size_t)]


def foreground() -> int:
    return user32.GetForegroundWindow() or 0


def title(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(wt.HWND(hwnd), buf, 256)
    return buf.value


def to_foreground(hwnd: int) -> bool:
    """Windows lets a background process take the foreground only with the foreground thread's
    input attached (and right after a key event)."""
    fg_tid = user32.GetWindowThreadProcessId(foreground(), None)
    me = ctypes.windll.kernel32.GetCurrentThreadId()
    attached = fg_tid and fg_tid != me and user32.AttachThreadInput(me, fg_tid, True)
    try:
        user32.keybd_event(VK_MENU, 0, 0, 0)
        user32.keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0)
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
    finally:
        if attached:
            user32.AttachThreadInput(me, fg_tid, False)
    return foreground() == hwnd


def focus_cycles(host: DolphinInstance, args: argparse.Namespace, rep: Dict[str, Any], stop: threading.Event,
                 wait_for: str = "gprb: session running") -> None:
    """After ``wait_for`` is in the host's log: the switches. No harness requests to the host."""
    rep["cycles"] = []
    t0 = time.monotonic()
    while not stop.is_set():
        if host.log_path.exists() and wait_for in host.log_path.read_text(errors="replace"):
            break
        if time.monotonic() - t0 > 600:
            rep["focus_error"] = f"never in the host's log: {wait_for!r}"
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
            switched = to_foreground(hwnd)
            time.sleep(args.hold_s)
            t = time.monotonic()
            ok = responds(render, args.hang_s)
            c = {"cycle": i, "switch": what, "switched": switched, "responds": ok,
                 "answer_s": round(time.monotonic() - t, 2), "foreground_is_render": foreground() == render,
                 "front": title(foreground())[:60]}
            rep["cycles"].append(c)
            print(f"  cycle {i} {what}: {'switched' if switched else 'NOT switched'}, "
                  f"{'responds' if ok else 'HUNG'} ({c['answer_s']} s), in front: {c['front']!r}",
                  flush=True)
            if not ok:
                rep["hung"] = True
                return


def solo(a: argparse.Namespace, rep: Dict[str, Any]) -> None:
    """The host alone, at Brawl's menus or the Versus CSS: no session and no harness requests while
    the focus moves (the harness's own memory reads pause the core from its thread). ``--solo-gpu``
    picks the GPU determinism mode: unset (the installs' default, a non-deterministic video thread
    outside a session, as at bug #62's CSS) or fake-completion (the deterministic one, as in a
    match)."""
    name = f"ffocus-solo-{a.solo_at}-{a.solo_gpu}"
    # Logs.VIDEO: FifoManager's "deterministic GPU thread true/false" line, read back below.
    inst = G.make_instance(name, cpu_thread=True, video="D3D11", gpu_determinism=a.solo_gpu,
                           config=["Dolphin.Display.Fullscreen=True", "Logger.Logs.VIDEO=True"])
    if a.solo_gpu == "unset":
        # make_instance only edits the launcher's and RSBE01's INIs; the template's others (e.g.
        # 00000000.ini, a DOL boot's) still say fake-completion. Installs set it in none of them.
        for p in (inst.user_dir / "GameSettings").glob("*.ini"):
            ini = IniFile.load(p)
            if ini.remove("Core", "GPUDeterminismMode"):
                ini.save(p)
    inst.__class__ = QtInstance
    inst.command_prefix = [str(Path(a.exe) if a.exe else paths.dolphin_gui())]
    inst.client_timeout = a.client_timeout_s
    try:
        inst.launch()
        inst.connect()
        inst.client.wait_state("running", timeout=120)
        if a.solo_at == "css":
            B.boot_to_css(inst.client, 0, timeout_frames=60 * 180)
            B.neutral(inst.client, 0)
            rep["at_css"] = True
        log = inst.log_path.read_text(errors="replace")
        modes = re.findall(r"deterministic GPU thread (true|false)", log)
        rep["deterministic_gpu_thread"] = modes[-1] == "true" if modes else None
        focus_cycles(inst, a, rep, threading.Event(), wait_for="Booting from executable")
        rep["hang_watchdog"] = "Hang watchdog" in inst.log_path.read_text(errors="replace")
    finally:
        with contextlib.suppress(Exception):
            inst.kill()
        if a.log_dir:
            with contextlib.suppress(Exception):
                Path(a.log_dir).mkdir(parents=True, exist_ok=True)
                (Path(a.log_dir) / f"{name}.log").write_text(inst.log_path.read_text(errors="replace"))
        inst.cleanup(True)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cycles", type=int, default=6, help="focus away-and-back switches")
    ap.add_argument("--hold-s", type=float, default=2.0, help="seconds after each switch before the check")
    ap.add_argument("--hang-s", type=float, default=10.0, help="a window that answers no WM_NULL in this is hung")
    ap.add_argument("--settle-s", type=float, default=5.0, help="seconds of the match before the first switch")
    ap.add_argument("--frames", type=int, default=3600, help="session frames the match lasts at most")
    ap.add_argument("--exe", default=None, help="the host's Dolphin.exe (default: the build's)")
    ap.add_argument("--client-timeout-s", type=float, default=30.0, help="harness request timeout of both instances")
    ap.add_argument("--solo", action="store_true",
                    help="the host alone, no session and no harness requests during the switches")
    ap.add_argument("--solo-at", choices=("css", "menus"), default="css",
                    help="--solo: switch at the Versus CSS (bug #62) or at Brawl's menus")
    ap.add_argument("--solo-gpu", choices=("unset", "fake-completion"), default="unset",
                    help="--solo: GPU determinism; unset = the installs' default (not deterministic "
                         "outside a session), fake-completion = deterministic as in a match")
    ap.add_argument("--json", default=None)
    a, rest = ap.parse_known_args(argv)
    a.log_dir = next((rest[i + 1] for i, x in enumerate(rest[:-1]) if x == "--log-dir"), None)

    host: List[DolphinInstance] = []
    orig = G.make_instance

    def mk(name: str, **kw):
        if "-host-" not in name:
            inst = orig(name, **kw)
            # A desktop busy with a fullscreen Dolphin (and whatever else runs) answers slower.
            inst.client_timeout = a.client_timeout_s
            return inst
        kw["video"] = "D3D11"
        kw["config"] = [*kw.get("config", ()), "Dolphin.Display.Fullscreen=True"]
        inst = orig(name, **kw)
        # The same instance directory, set up by make_instance; Dolphin.exe runs it.
        inst.__class__ = QtInstance
        inst.command_prefix = [str(Path(a.exe) if a.exe else paths.dolphin_gui())]
        inst.client_timeout = a.client_timeout_s
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

    if a.solo:
        solo(a, rep)
    else:
        G.make_instance = mk
        G.instances = instances
        try:
            S.main(["--cpu", "dc", "--preset", "lan", "--frames", str(a.frames), *rest])
        finally:
            G.make_instance, G.instances = orig, orig_instances
    hung = bool(rep.get("hung"))
    # A switch that did not take tested nothing.
    ok = (not hung and not rep.get("focus_error") and len(rep.get("cycles", [])) == 2 * a.cycles
          and all(c["switched"] for c in rep["cycles"]))
    mode = (f" ({a.solo_at}, deterministic GPU thread {rep.get('deterministic_gpu_thread')})"
            if a.solo else "")
    print(f"fullscreen_focus{mode}: {'PASS' if ok else 'FAIL'}: {len(rep.get('cycles', []))} checks, "
          f"hung {hung}{', ' + rep['focus_error'] if rep.get('focus_error') else ''}")
    if a.json:
        Path(a.json).write_text(json.dumps(rep, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
