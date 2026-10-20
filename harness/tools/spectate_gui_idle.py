"""Bug report #67 (2026-10-20): does the GUI playback Dolphin (Dolphin.exe, as the launcher starts it
for Spectate) keep its CPU and video threads busy while a live game buffers or is paused, and does
a catch-up to the live edge show frames? 2bbf7021 (bug #17) was measured on DolphinNoGUI only.

One Dolphin.exe follows a live game in mirror mode (the launcher's comm file). This script is the
stream: `replay.brep`'s header, then its frames a 60-frame FRMS group a second (1x), with a head
start of --head-s seconds so the viewer joins a game under way and catches up. Then:
  join     the join catch-up, until "live";
  live     --live-s seconds at the edge;
  buffer   the stream stops for --still-s seconds (the viewer runs out and buffers);
  pause    the stream goes on, the viewer is paused for --still-s seconds;
  golive   Go live: the catch-up from the pause back to the edge;
  tail     --live-s seconds live again.
Every second: each named thread's CPU (% of one core, GetThreadTimes: "CPU thread", "Video
thread", the process), the playback's status, and the presents of new frames (Present.cpp's
OnPresent, not counting duplicates: the redraws while standing still). Screenshots in each phase.
The stall tracer's "Stall over" lines (CPU time per thread while no emulated field came) are
copied into the result too.

Run it on the interactive desktop (the app's Terminal panel): Dolphin.exe gets no windows from a
service window station and never boots.

usage: python harness/tools/spectate_gui_idle.py --exe <Dolphin.exe> --plugin <PPOnline.rel>
           --replay <replay.brep> --out <dir> [--video D3D11] [--play-anyway]

<out> gets a 2 GB card copy in the instance folder: delete it afterwards with
`python -m ppharness clean` or by unlinking the instance's Load junction first.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import socket
import struct
import sys
import threading
import time
from ctypes import wintypes as wt
from pathlib import Path

ROOT = Path(r"D:\code\pm_rollback")
sys.path.insert(0, str(ROOT / "harness"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))

from ppharness.instance import DolphinInstance, InstanceConfig  # noqa: E402
import patch_sd  # noqa: E402

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
TH32CS_SNAPTHREAD = 0x4
THREAD_QUERY_LIMITED_INFORMATION = 0x0800


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k32.LocalFree.argtypes = [ctypes.c_void_p]
k32.QueryThreadCycleTime.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_uint64)]
k32.GetCurrentThread.restype = wt.HANDLE


class QtInstance(DolphinInstance):
    """Dolphin.exe: no `-p` (a DolphinNoGUI option)."""

    def build_args(self) -> list[str]:
        args = super().build_args()
        i = args.index("-p")
        return args[:i] + args[i + 2:]


def ft(t: wt.FILETIME) -> float:
    return (t.dwHighDateTime << 32 | t.dwLowDateTime) / 1e7


def busy_core_cycles() -> float:
    """Cycles a second of one busy core (this thread spinning for 0.5 s): QueryThreadCycleTime's
    counts are turned into % of a core with it. GetThreadTimes is charged at the clock tick and
    misses much of a thread that runs in bursts between them (the CPU thread syncing with the video
    thread), so the cycle counts are the measure; the tick times are kept as a cross-check."""
    h = k32.GetCurrentThread()
    c0, c1 = ctypes.c_uint64(), ctypes.c_uint64()
    k32.QueryThreadCycleTime(h, ctypes.byref(c0))
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 0.5:
        pass
    k32.QueryThreadCycleTime(h, ctypes.byref(c1))
    return (c1.value - c0.value) / (time.perf_counter() - t0)


def thread_times(pid: int) -> dict[str, float]:
    """CPU seconds (kernel + user, tick-charged) and cycles of the process's threads, by
    description ("CPU thread", "Video thread"; "~cycles" appended for the cycles); "process" is
    the sum of all of them."""
    out: dict[str, float] = {}
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    total = total_cyc = 0.0
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            h = k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, te.th32ThreadID)
            if h:
                c, e, kt, ut = (wt.FILETIME() for _ in range(4))
                cyc = ctypes.c_uint64()
                if k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut)):
                    secs = ft(kt) + ft(ut)
                    k32.QueryThreadCycleTime(h, ctypes.byref(cyc))
                    total += secs
                    total_cyc += cyc.value
                    name = ctypes.c_wchar_p()
                    if k32.GetThreadDescription(h, ctypes.byref(name)) >= 0 and name.value:
                        if name.value in ("CPU thread", "Video thread"):
                            out[name.value] = out.get(name.value, 0.0) + secs
                            out[name.value + "~cycles"] = out.get(name.value + "~cycles", 0.0) + cyc.value
                        k32.LocalFree(name)
                k32.CloseHandle(h)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    out["process"] = total
    out["process~cycles"] = total_cyc
    return out


def chunk_groups(data: bytes) -> tuple[bytes, list[bytes]]:
    """The file's header with every chunk before the first FRMS, and then one group per FRMS chunk
    (with the chunks after it up to the next FRMS); no ENDG and no trailer: a live stream."""
    header = struct.unpack_from("<I", data, 12)[0]
    at, head, groups = header, [data[:header]], []
    while at + 20 <= len(data):
        kind = data[at:at + 4]
        if kind in (b"BRPT", b"ENDG"):
            break
        stored = struct.unpack_from("<I", data, at + 8)[0]
        c = data[at:at + 20 + stored]
        at += 20 + stored
        if kind == b"FRMS":
            groups.append(c)
        elif groups:
            groups[-1] += c
        else:
            head.append(c)
    return b"".join(head), groups


class Stream:
    """The live file, a group a second while `on` is set."""

    def __init__(self, path: Path, head: bytes, groups: list[bytes], head_groups: int):
        self.path, self.groups = path, groups
        self.sent = min(head_groups, len(groups))
        path.write_bytes(head + b"".join(groups[:self.sent]))
        self.on = threading.Event()
        self.on.set()
        self.stop = threading.Event()
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self) -> None:
        next_at = time.time() + 1.0
        while not self.stop.is_set() and self.sent < len(self.groups):
            time.sleep(max(0.0, next_at - time.time()))
            next_at += 1.0
            if self.on.is_set():
                with open(self.path, "ab") as f:
                    f.write(self.groups[self.sent])
                self.sent += 1


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait(pred, secs: float, what: str):
    end = time.time() + secs
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.25)
    raise TimeoutError(what)


def presents(client) -> tuple[int, int]:
    p = client.call("status").get("presentation", {})
    return int(p.get("presents", 0)), int(p.get("duplicate_presents", 0))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, help="Dolphin.exe")
    ap.add_argument("--plugin", required=True, help="PPOnline.rel to patch onto the viewer's card")
    ap.add_argument("--replay", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--video", default="D3D11")
    ap.add_argument("--card", default=None, help="the SD card to copy for the viewer (default: the template's)")
    ap.add_argument("--play-anyway", action="store_true", help="the replay is from another build or card")
    ap.add_argument("--head-s", type=int, default=40, help="seconds of the game there when the viewer joins")
    ap.add_argument("--live-s", type=int, default=10)
    ap.add_argument("--still-s", type=int, default=20)
    a = ap.parse_args()
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "shots").mkdir(exist_ok=True)
    head, groups = chunk_groups(Path(a.replay).read_bytes())
    result: dict = {"exe": a.exe, "plugin": a.plugin, "replay": a.replay, "groups": len(groups), "phases": {}}

    log_args = ("Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4", "Logger.Logs.Brawlback=True",
                "Logger.Logs.CORE=True")
    live = out / "live.brep"
    comm = out / "comm.json"
    cfg = InstanceConfig(cpu_thread=True, video_backend=a.video, standard_controllers=(0,),
                         extra_args=("-i", str(comm)),
                         config_args=("Dolphin.Core.WiiSDCard=True", "Dolphin.Core.WiiSDCardEnableFolderSync=False",
                                      "Dolphin.Core.WiiSDCardAllowWrites=False", "Dolphin.Online.SaveReplays=False",
                                      *log_args))
    v = QtInstance("guiviewer", exe=Path(a.exe), config=cfg, instances_root=out, keep=True, port=free_port(),
                   connect_timeout=180, client_timeout=60.0)
    d = v.create()
    card = d / "Wii" / "sd.raw"
    if a.card:
        import shutil
        shutil.copyfile(a.card, card)
    plugin = Path(a.plugin)
    patch_sd.patch_image(card, [(plugin.read_bytes(), patch_sd.plugin_sd_path(plugin))])
    stream = Stream(live, head, groups, a.head_s)
    comm.write_text(json.dumps({"mode": "mirror", "commandId": "g1", "queue": [{"path": str(live), "cut": False}],
                                "streamOver": False, "playAnyway": a.play_anyway}))
    try:
        v.launch()
        v.connect()
        pid = v.process.pid
        def started():
            st = v.client.call("replay_status")
            if st.get("comm", {}).get("state") == "error":
                raise RuntimeError(f"the viewer refused the game: {st['comm'].get('error')}")
            return st.get("started")

        wait(started, 300, "the viewer to start")
        core = busy_core_cycles()
        result["busy_core_cycles_per_s"] = round(core)
        print("viewer started; a busy core runs", round(core / 1e6), "M cycles/s", flush=True)

        def sample(phase: str, secs: float | None = None, until=None, limit: float = 120.0) -> list[dict]:
            rows: list[dict] = []
            t_end = time.time() + (secs if secs is not None else limit)
            while time.time() < t_end:
                c0, p0, t0 = thread_times(pid), presents(v.client), time.time()
                time.sleep(1.0)
                c1, p1, t1 = thread_times(pid), presents(v.client), time.time()
                st = v.client.call("replay_status")
                dt = t1 - t0
                pct = lambda k: round(100 * (c1.get(k, 0) - c0.get(k, 0)) / dt, 1)  # noqa: E731
                pcy = lambda k: round(100 * (c1.get(k, 0) - c0.get(k, 0)) / (core * dt), 1)  # noqa: E731
                row = {"phase": phase,
                       "cpu": pcy("CPU thread~cycles"), "gpu": pcy("Video thread~cycles"),
                       "proc": pcy("process~cycles"),
                       "cpu_ticks": pct("CPU thread"), "gpu_ticks": pct("Video thread"), "proc_ticks": pct("process"),
                       "new_frames_shown": round(((p1[0] - p1[1]) - (p0[0] - p0[1])) / dt, 1),
                       "presents": round((p1[0] - p0[0]) / dt, 1),
                       **{k: st.get(k) for k in ("frame", "last_frame", "live_edge", "buffering", "catching_up",
                                                 "paused", "catch_rate")},
                       "sent_s": stream.sent}
                rows.append(row)
                print(json.dumps(row), flush=True)
                if until is not None and until(st):
                    break
            for i in range(3):
                t0 = time.time()
                v.client.screenshot(out / "shots" / f"{phase}-{i}.png")
                result.setdefault("screenshot_ms", {}).setdefault(phase, []).append(round((time.time() - t0) * 1000))
                time.sleep(0.3)
            result["phases"][phase] = rows
            return rows

        def screenshots_during(phase: str, n: int) -> None:
            for i in range(n):
                t0 = time.time()
                v.client.screenshot(out / "shots" / f"{phase}-during-{i}.png")
                st = v.client.call("replay_status")
                result.setdefault("during", {}).setdefault(phase, []).append(
                    {"ms": round((time.time() - t0) * 1000), "frame": st.get("frame"), "catching_up": st.get("catching_up")})
                time.sleep(0.4)

        screenshots_during("join", 3)
        sample("join", until=lambda st: not st.get("catching_up") or st.get("finished"))
        sample("live", a.live_s)
        stream.on.clear()
        sample("buffer", a.still_s)
        stream.on.set()
        v.client.call("replay_control", action="pause")
        sample("pause", a.still_s)
        v.client.call("replay_control", action="live")
        time.sleep(0.5)
        screenshots_during("golive", 3)
        sample("golive", until=lambda st: not st.get("catching_up") or st.get("finished"))
        sample("tail", a.live_s)
    finally:
        stream.stop.set()
        try:
            v.stop()
        except Exception:  # noqa: BLE001
            v.kill()
    log = v.log_path.read_text(errors="replace")
    result["stall_lines"] = [ln for ln in log.splitlines() if "Stall over" in ln or "live at frame" in ln
                             or "session running" in ln]
    summary = {}
    for ph, rows in result["phases"].items():
        steady = rows[2:] if ph in ("buffer", "pause") and len(rows) > 4 else rows
        if steady:
            summary[ph] = {k: round(sum(r[k] for r in steady) / len(steady), 1)
                           for k in ("cpu", "gpu", "proc", "new_frames_shown")}
            summary[ph]["seconds"] = len(rows)
            summary[ph]["buffering_s"] = sum(1 for r in rows if r["buffering"])
            summary[ph]["paused_s"] = sum(1 for r in rows if r["paused"])
    result["summary"] = summary
    print(json.dumps({"summary": summary, "stall_lines": result["stall_lines"],
                      "during": result.get("during")}, indent=1), flush=True)
    (out / "result.json").write_text(json.dumps(result, indent=1))
    (out / "dolphin.log").write_text(log)
    return 0


if __name__ == "__main__":
    sys.exit(main())
