"""Stacks of every thread of a hung DolphinNoGUI (cdb from the Windows SDK), for the gameplay
rollback tools: call ``dump_stacks(inst, out_path)`` when the harness stops answering, before the
instance is cleaned up. Symbols: the DolphinNoGUI.pdb staged next to the binary, else the build's.
"""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Optional

CDB = Path(r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe")
BUILD_PDB = Path(__file__).resolve().parents[2] / "dolphin-gprb" / "build" / "release" / "x64" / "pdb"


def dump_stacks(inst, out_path: str, timeout: float = 120.0) -> Optional[str]:
    proc = getattr(inst, "process", None)
    if proc is None or proc.poll() is not None or not CDB.exists():
        return None
    exe_dir = None
    try:
        exe_dir = Path(proc.args[0]).parent if isinstance(proc.args, (list, tuple)) else None
    except Exception:  # noqa: BLE001
        exe_dir = None
    sym = str(exe_dir) if exe_dir and (exe_dir / "DolphinNoGUI.pdb").exists() else str(BUILD_PDB)
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    try:
        r = subprocess.run([str(CDB), "-p", str(proc.pid), "-y", sym, "-logo", out_path,
                            "-c", "~*kn 40; qd"], capture_output=True, timeout=timeout)
        return out_path if r.returncode == 0 or Path(out_path).exists() else None
    except Exception:  # noqa: BLE001
        return None
