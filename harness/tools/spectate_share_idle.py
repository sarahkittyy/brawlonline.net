"""Bug report #17 (2026-10-15): a playback Dolphin boots on the SD card a Play Dolphin has open, as
the launcher's Spectate does while the player's own game runs, and a live playback that waits for
frames (buffering, then paused) stays off the CPU.

A Play Dolphin boots P+ on its card; another process then reads that card (the launcher's read-back);
a playback Dolphin follows a live game cut from `replay.brep` (its first FRMS chunks, no end) on the
same card, plays what came and buffers. Each second: the viewer's CPU (% of one core) and its
replay status, 20 s buffering, then 20 s paused. Windows only (process CPU times).

usage: python harness/tools/spectate_share_idle.py <binaries dir> <out dir> <replay.brep> [video] [--own-card]

--own-card gives the viewer a copy of the card instead (to measure an older build, which can't share).
The instance folders hold 2 GB cards each: delete <out dir> afterwards (unlink each instance's Load
junction first).
"""
from __future__ import annotations

import ctypes
import json
import socket
import struct
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(r"D:\code\pm_rollback")
sys.path.insert(0, str(ROOT / "harness"))
sys.path.insert(0, str(ROOT / "harness" / "tests"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))

from ppharness.instance import DolphinInstance, InstanceConfig  # noqa: E402
import patch_sd  # noqa: E402

PLUGIN = ROOT / "game-code" / "PPOnline" / "PPOnline.rel"


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def cpu_seconds(pid: int) -> float:
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    c, e, kt, ut = (wintypes.FILETIME() for _ in range(4))
    k.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    k.CloseHandle(h)
    f = lambda t: (t.dwHighDateTime << 32 | t.dwLowDateTime) / 1e7  # noqa: E731
    return f(kt) + f(ut)


def live_copy(src: Path, dst: Path, frms: int) -> None:
    """The replay as a live stream that stopped: its header and chunks up to `frms` FRMS chunks,
    no ENDG and no trailer."""
    data = src.read_bytes()
    header = struct.unpack_from("<I", data, 12)[0]
    at, out, n = header, [data[:header]], 0
    while at + 20 <= len(data):
        kind = data[at:at + 4]
        if kind in (b"BRPT", b"ENDG"):
            break
        stored = struct.unpack_from("<I", data, at + 8)[0]
        out.append(data[at:at + 20 + stored])
        at += 20 + stored
        if kind == b"FRMS":
            n += 1
            if n >= frms:
                break
    dst.write_bytes(b"".join(out))


def wait(pred, secs: float, what: str):
    end = time.time() + secs
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.5)
    raise TimeoutError(what)


def main() -> int:
    bins, out, replay = (Path(a).resolve() for a in sys.argv[1:4])
    video = sys.argv[4] if len(sys.argv) > 4 and not sys.argv[4].startswith("--") else "D3D11"
    out.mkdir(parents=True, exist_ok=True)
    exe = bins / "DolphinNoGUI.exe"
    made: list = []
    try:
        return run(exe, out, replay, video, made)
    finally:
        for inst in made:
            try:
                inst.stop()
            except Exception:  # noqa: BLE001
                inst.kill()


def run(exe, out, replay, video, made) -> int:
    result: dict = {"exe": str(exe)}

    # The Play Dolphin: P+ with the plugin on its own card, booted to the menus.
    log_args = ("Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4", "Logger.Logs.OSREPORT=True",
                "Logger.Logs.IOS_SD=True", "Logger.Logs.Brawlback=True")
    pcfg = InstanceConfig(cpu_thread=True, video_backend="Null", standard_controllers=(0,), config_args=log_args)
    p = DolphinInstance("player", exe=exe, config=pcfg, instances_root=out, keep=True, port=free_port(),
                        connect_timeout=120)
    made.append(p)
    pd = p.create()
    card = pd / "Wii" / "sd.raw"
    patch_sd.patch_image(card, [(PLUGIN.read_bytes(), patch_sd.plugin_sd_path(PLUGIN))])
    vcard = card
    if "--own-card" in sys.argv:
        import shutil
        vcard = out / "viewer-card.raw"
        shutil.copyfile(card, vcard)
    p.launch()
    p.connect()
    wait(lambda: "File Read  (SD)" in p.log_path.read_text(errors="replace"), 120, "the player to read its card")
    print("player booted and reading its card", flush=True)

    # Another process reads the card while the player has it open (the launcher's read-back).
    try:
        with open(card, "rb") as f:
            f.read(512)
        result["read_while_open"] = True
    except OSError as e:
        result["read_while_open"] = f"{type(e).__name__}: {e}"
    print("read while open:", result["read_while_open"], flush=True)

    # The playback Dolphin on the player's card (as the launcher starts it), following a live
    # game that stops: it plays what came, then buffers.
    live = out / "live.brep"
    live_copy(replay, live, 6)
    comm = out / "comm.json"
    comm.write_text(json.dumps({"mode": "mirror", "commandId": "w1", "queue": [{"path": str(live), "cut": False}],
                                "streamOver": False}))
    vcfg = InstanceConfig(cpu_thread=True, video_backend=video, standard_controllers=(0,), extra_args=("-i", str(comm)),
                          config_args=("Dolphin.Core.WiiSDCard=True", "Dolphin.Core.WiiSDCardEnableFolderSync=False",
                                       f"Dolphin.General.WiiSDCardPath={vcard}",
                                       "Dolphin.Core.WiiSDCardAllowWrites=False", "Dolphin.Online.SaveReplays=False",
                                       *log_args))
    v = DolphinInstance("viewer", exe=exe, config=vcfg, instances_root=out, keep=True, port=free_port(),
                        connect_timeout=120, client_timeout=60.0)
    made.insert(0, v)
    v.create()
    v.launch()
    v.connect()
    st = wait(lambda: (lambda s: s if s.get("started") else None)(v.client.call("replay_status")), 300,
              "the viewer to start")
    log = v.log_path.read_text(errors="replace")
    result["viewer_sd_open_failed"] = "Failed to open SD Card image" in log
    result["viewer_read_sd"] = "File Read  (SD)" in log
    pid = v.process.pid
    samples = []
    for phase in ("run", "pause"):
        if phase == "pause":
            v.client.call("replay_control", action="pause")
        for _ in range(20):
            c0, t0 = cpu_seconds(pid), time.time()
            time.sleep(1)
            c1, t1 = cpu_seconds(pid), time.time()
            s = v.client.call("replay_status")
            samples.append({"phase": phase, "cpu": round(100 * (c1 - c0) / (t1 - t0), 1),
                            **{k: s.get(k) for k in ("shown", "buffering", "paused", "at_end", "live_edge", "complete")}})
            print(samples[-1], flush=True)
        # While it stands still, only the redraws (Presenter::RedrawLastFrame, about 30 a second,
        # which also draw the overlay: the seek bar, "Buffering...", chat) present a frame, and a
        # harness screenshot is written at the next present. So each one coming within a redraw
        # interval says the redraws go on although the GPU thread sleeps between them.
        shots = []
        for i in range(10):
            t0 = time.time()
            v.client.screenshot(out / "shots" / f"{phase}-{i}.png")
            shots.append(round((time.time() - t0) * 1000))
            time.sleep(0.2)
        result[f"{phase}_screenshot_ms"] = shots
        print(phase, "screenshots (ms):", shots, flush=True)
    result["samples"] = samples
    buf = [x["cpu"] for x in samples if x["phase"] == "run" and x["buffering"]]
    pau = [x["cpu"] for x in samples if x["phase"] == "pause"][3:]
    result["buffering_cpu_avg"] = round(sum(buf) / len(buf), 1) if buf else None
    result["paused_cpu_avg"] = round(sum(pau) / len(pau), 1) if pau else None
    print(json.dumps(result, indent=1), flush=True)
    (out / "result.json").write_text(json.dumps(result, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
