#!/usr/bin/env python3
"""Install the game-side plugin toolchain into <workspace>/toolchains (portable, no system changes).

This is what brawlback-asm's `bbk.py setup` and its CI install, pinned to the same versions:

- kuribo-llvm (clang + ld.lld with the `powerpc-gekko-ibm-kuribo-eabi` target), DotKuribo's
  LLVM fork, commit 377c67cd575495a2f818733e42b4913c4b365d65
- Sammi-Husky's elf2rel, commit b44c71434a68489061ab2550166f354a58faff14

Both come from the toolchain mirror brawlback-asm uses (dol-rvl-toolchains.s3.amazonaws.com).
GNU make and a POSIX shell (Git for Windows' bash, or MSYS2) must already be on PATH; nothing
else is needed. devkitPro/devkitPPC is NOT used by Syriinge plugins.

Layout after install:
    toolchains/kuribo-llvm-377c67c/bin/{clang,ld.lld}[.exe]
    toolchains/elf2rel-b44c714/elf2rel[.exe]
    toolchains/MANIFEST.json            (urls + sha256 of what was installed)

Usage:  python tools/gamecode/setup_toolchain.py [--force]
Stdlib only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import stat
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

MIRROR = "https://dol-rvl-toolchains.s3.amazonaws.com"
KURIBO_LLVM_VER = "377c67cd575495a2f818733e42b4913c4b365d65"
ELF2REL_VER = "b44c71434a68489061ab2550166f354a58faff14"

ROOT = Path(__file__).resolve().parents[2]
TOOLCHAINS = ROOT / "toolchains"


def host_triple() -> str:
    arch = platform.machine()
    arch = {"AMD64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}.get(arch)
    if not arch:
        sys.exit(f"unsupported architecture {platform.machine()}")
    sysname = platform.system()
    if sysname == "Darwin":
        return f"{arch}-apple-darwin"
    if sysname == "Linux":
        return f"{arch}-unknown-linux"
    if sysname == "Windows":
        return f"{arch}-pc-windows-msvc"
    sys.exit(f"unsupported OS {sysname}")


def fetch(url: str, dest: Path) -> str:
    print(f"  downloading {url}")
    h = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=60) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while chunk := r.read(1 << 20):
            f.write(chunk)
            h.update(chunk)
            done += len(chunk)
            if total:
                print(f"\r  {done >> 20} / {total >> 20} MiB", end="", flush=True)
    print()
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="reinstall even if present")
    args = ap.parse_args()

    triple = host_triple()
    exe = ".exe" if os.name == "nt" else ""
    TOOLCHAINS.mkdir(exist_ok=True)
    manifest_path = TOOLCHAINS / "MANIFEST.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}

    print(f"host {triple}; fetching {MIRROR}/toolchains.json")
    with urllib.request.urlopen(f"{MIRROR}/toolchains.json", timeout=60) as r:
        index = json.load(r)

    llvm_dir = TOOLCHAINS / f"kuribo-llvm-{KURIBO_LLVM_VER[:7]}"
    if args.force and llvm_dir.exists():
        shutil.rmtree(llvm_dir)
    if not (llvm_dir / "bin" / f"clang{exe}").exists():
        rel = index["kuribo-llvm"][KURIBO_LLVM_VER][triple]
        with tempfile.TemporaryDirectory(dir=TOOLCHAINS) as td:
            tar = Path(td) / "llvm.tar.bz2"
            sha = fetch(f"{MIRROR}/{rel}", tar)
            print("  extracting")
            staging = Path(td) / "x"
            with tarfile.open(tar) as tf:
                for m in tf.getmembers():
                    parts = m.name.split("/")[1:]       # strip the archive's top folder
                    if not parts or ".." in parts:
                        continue
                    m.name = "/".join(parts)
                    tf.extract(m, staging, filter="tar") if hasattr(tarfile, "data_filter") \
                        else tf.extract(m, staging)
            staging.rename(llvm_dir)
        manifest["kuribo-llvm"] = {"version": KURIBO_LLVM_VER, "url": f"{MIRROR}/{rel}", "sha256": sha,
                                   "dir": llvm_dir.name}
    else:
        print(f"  {llvm_dir.name} already installed")

    e2r_dir = TOOLCHAINS / f"elf2rel-{ELF2REL_VER[:7]}"
    e2r = e2r_dir / f"elf2rel{exe}"
    if args.force and e2r_dir.exists():
        shutil.rmtree(e2r_dir)
    if not e2r.exists():
        e2r_dir.mkdir(parents=True, exist_ok=True)
        rel = index["sammihusky-elf2rel"][ELF2REL_VER][triple]
        sha = fetch(f"{MIRROR}/{rel}", e2r)
        if os.name == "posix":
            e2r.chmod(e2r.stat().st_mode | stat.S_IEXEC)
        manifest["elf2rel"] = {"version": ELF2REL_VER, "url": f"{MIRROR}/{rel}", "sha256": sha,
                               "dir": e2r_dir.name}
    else:
        print(f"  {e2r_dir.name} already installed")

    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print("ok:")
    print(f"  LLVMDIR={llvm_dir}")
    print(f"  ELF2REL={e2r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
