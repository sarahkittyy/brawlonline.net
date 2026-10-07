#!/usr/bin/env python3
"""Boot P+ in an isolated harness instance whose SD card carries our plugin(s).

The harness copies the template user dir (including the 2 GB sd.raw) into
run/instances/<name>-<n>; this tool then patches *that copy* with tools/sdcard/patch_sd.py
before Dolphin starts. The template is never touched.

    python tools/gamecode/ppboot.py boot --plugin game-code/PPOnline/PPOnline.rel [--detach]
    python tools/gamecode/ppboot.py stop --port P            # quit + remove the instance dir

Defaults: muted, headless, D3D11 (Windows) so screenshots work, dual core, P+ Offline Launcher.
Set PPHARNESS_DOLPHIN_DIR to a frozen copy of the Dolphin binaries (run/bin/<name>).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from ppharness import DolphinInstance, InstanceConfig  # noqa: E402
from ppharness import _platform  # noqa: E402
import patch_sd  # noqa: E402

DEFAULT_VIDEO = "D3D11" if os.name == "nt" else "Vulkan"


def make_instance(name: str, plugins: list[str] = (), adds: list[str] = (), boot: str = "offline",
                  video: str = DEFAULT_VIDEO, cpu_thread: bool = True, detach: bool = False,
                  keep: bool = False, ports=(0, 1)) -> DolphinInstance:
    cfg = InstanceConfig(cpu_thread=cpu_thread, video_backend=video, standard_controllers=ports)
    inst = DolphinInstance(name, config=cfg, boot=boot, detach=detach, keep=keep, client_timeout=60.0)
    d = inst.create()
    files: list[tuple[bytes, str]] = []
    for p in plugins:
        pp = Path(p)
        files.append((pp.read_bytes(), f"{patch_sd.PLUGIN_DIR}/{pp.name}"))
    for a in adds:
        local, _, sd_path = a.partition("=")
        files.append((Path(local).read_bytes(), sd_path))
    if files:
        log = patch_sd.patch_image(d / "Wii" / "sd.raw", files)
        (d / "sd-patch.txt").write_text("\n".join(log) + "\n")
    return inst


def cmd_boot(a: argparse.Namespace) -> int:
    inst = make_instance(a.name, a.plugin or [], a.add or [], a.boot, a.video, not a.single_core,
                         detach=True, keep=True)
    inst.launch()
    inst.connect()
    print(json.dumps({"port": inst.port, "pid": inst.process.pid, "user_dir": str(inst.user_dir)}))
    return 0


def cmd_run(a: argparse.Namespace) -> int:
    """Boot, run drive.py steps, then quit and delete the instance dir (kept on failure,
    with sd.raw stripped)."""
    import shutil
    import drive
    inst = make_instance(a.name, a.plugin or [], a.add or [], a.boot, a.video, not a.single_core)
    ok = False
    try:
        inst.launch()
        c = inst.connect()
        drive.run(c, a.steps, out=a.out)
        ok = True
    finally:
        try:
            inst.stop()
        except Exception as e:  # noqa: BLE001
            print(f"stop: {e}")
            inst.kill()
        d = inst.user_dir
        if d and d.exists():
            if ok and not a.keep:
                from ppharness.instance import remove_tree
                remove_tree(d, inst.linked_dirs)
            else:
                sd = d / "Wii" / "sd.raw"
                if sd.exists():
                    sd.unlink()
                print(f"kept {d} (sd.raw removed)")
    return 0 if ok else 1


def cmd_stop(a: argparse.Namespace) -> int:
    from ppharness.client import HarnessClient
    try:
        c = HarnessClient.connect(a.port, timeout=5)
        c.quit(timeout=10)
    except Exception as e:  # noqa: BLE001
        print(f"quit failed: {e}")
    if a.pid:
        import time
        time.sleep(1.5)
        try:
            os.kill(a.pid, 9)
        except OSError:
            pass
    if a.dir:
        import time
        from ppharness.instance import remove_tree
        time.sleep(1.0)
        d = Path(a.dir)
        linked = json.loads((d / "harness-instance.json").read_text()).get("linked_dirs", [])             if (d / "harness-instance.json").exists() else ["Load"]
        remove_tree(d, linked)
        print(f"removed {d}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("boot")
    p.add_argument("--plugin", action="append")
    p.add_argument("--add", action="append", metavar="LOCAL=SD_PATH")
    p.add_argument("--boot", default="offline")
    p.add_argument("--video", default=DEFAULT_VIDEO)
    p.add_argument("--single-core", action="store_true")
    p.add_argument("--name", default="gamecode")
    p.set_defaults(func=cmd_boot)
    p = sub.add_parser("run", help="boot, run drive.py steps, quit, clean up")
    p.add_argument("--plugin", action="append")
    p.add_argument("--add", action="append", metavar="LOCAL=SD_PATH")
    p.add_argument("--boot", default="offline")
    p.add_argument("--video", default=DEFAULT_VIDEO)
    p.add_argument("--single-core", action="store_true")
    p.add_argument("--name", default="gamecode")
    p.add_argument("--keep", action="store_true")
    p.add_argument("--out", default="run/artifacts/game-code/screens")
    p.add_argument("steps", nargs="+")
    p.set_defaults(func=cmd_run)
    p = sub.add_parser("stop")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--pid", type=int)
    p.add_argument("--dir", help="instance dir to delete afterwards")
    p.set_defaults(func=cmd_stop)
    a = ap.parse_args()
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
