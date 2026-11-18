#!/usr/bin/env python3
"""P+'s online tests on one build of the game, with a summary (docs/design/pplus-mods.md §5).

    # a mod (its template from mod_template.py)
    python harness/tools/mod_suite.py --template run/mods/cobalt-1.34 --folder Cobalt --out run/mods/out/cobalt
    # a mod with launchers of its own
    python harness/tools/mod_suite.py --template run/mods/rex-0.5 --folder rex_ --out run/mods/out/rex \
        --offline-launcher "REX Offline Launcher.elf" --netplay-launcher "REX Netplay Launcher.elf"
    # stock P+, the control: what fails there too says nothing about the mod
    python harness/tools/mod_suite.py --out run/mods/out/pplus
    # the mod's result against the control
    python harness/tools/mod_suite.py --summary run/mods/out/cobalt --control run/mods/out/pplus

Each test file runs as its own pytest process (its own backend: accounts, mm), `--jobs` at a time;
the logs go to `--out`. The files are the scope of mods today: 1v1 matchmaking (Unranked, Direct,
the waiting room) and rooms. A test needs what it needs on P+: a Dolphin with the harness
(PPHARNESS_DOLPHIN_DIR), the server's binaries and a Postgres (harness/README.md). More than about
six files at a time on one PC starve the instances: boots time out and tests are skipped.

A failure is one of three kinds, which the summary tells apart where it can:
- it fails on the control too (the environment, or the test itself);
- the test expects P+'s own data: a costume's file number, a stage's place on P+'s pages, a plate's
  colour sampled by pixel (mod_adapt.py does not hide these);
- the build is broken: fix it in the plugin or Dolphin, for every build.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# 1v1 matchmaking and rooms (the user's scope for mods, 2026-11-14).
FILES = [
    "test_rooms_game.py", "test_online_game.py", "test_rooms_ui.py", "test_room_sets.py", "test_online_css.py",
    "test_rooms.py", "test_special_modes.py", "test_waiting_room.py", "test_room_stage_list.py",
    "test_online_unranked.py", "test_room_stock_control.py", "test_room_anyone_picks.py",
    "test_room_stage_switch.py", "test_room_sit_out.py", "test_room_watch.py", "test_rules_pages.py",
    "test_room_rules.py", "test_room_sit_out_ui.py", "test_online_leave.py", "test_online_mirror.py",
    "test_salty_runback.py", "test_sudden_death.py", "test_pause_after_a_game.py",
    "test_online_alt_costumes.py", "test_name_tags.py", "test_online_slot_colours.py",
    "test_online_start_hotkeys.py", "test_online_big_costumes.py", "test_build_profile.py",
]


def log_name(target: str) -> str:
    return re.sub(r"[:/.\[\]]", "_", target) + ".log"


def run_one(target: str, out: Path, env: dict[str, str], tag: str, timeout: float | None = None) -> str:
    e = dict(env, PPHARNESS_INSTANCE_PREFIX=f"{tag}-{log_name(target)[:-4]}-")
    log = out / log_name(target)
    with log.open("w") as f:
        # Its own process group, so that a test whose game hung is stopped with its Dolphins.
        p = subprocess.Popen([sys.executable, "-m", "pytest", f"harness/tests/{target}", "-q", "--no-header", "-rA",
                              "-p", "no:cacheprovider"], cwd=ROOT, env=e, stdout=f, stderr=subprocess.STDOUT,
                             start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            import signal
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(p.pid, sig)
                except ProcessLookupError:
                    break
                try:
                    p.wait(timeout=20)
                    break
                except subprocess.TimeoutExpired:
                    continue
            f.write(f"\n1 failed (mod_suite: no result in {timeout:.0f} s, stopped)\n")
    lines = log.read_text(errors="replace").strip().splitlines()
    return f"{time.strftime('%H:%M:%S')} {target}: {lines[-1][:80] if lines else 'no output'}"


def split(target: str, env: dict[str, str]) -> list[str]:
    """A file's tests as targets of their own (a node id is left as it is)."""
    if "::" in target:
        return [target]
    out = subprocess.run([sys.executable, "-m", "pytest", f"harness/tests/{target}", "--collect-only", "-q",
                          "--no-header", "-p", "no:cacheprovider"], cwd=ROOT, env=env, capture_output=True, text=True)
    # (pytest names a test from its rootdir: "tests/test_x.py::test_name[param]")
    ids = [m.group(1) for line in out.stdout.splitlines() if (m := re.match(r"(?:.*/)?tests/(\S+::.+)$", line.strip()))]
    return ids or [target]


def read(d: Path) -> dict[tuple[str, str], list[str]]:
    """{(file, test): [outcome, first error line, where]} from a run's logs."""
    res: dict[tuple[str, str], list[str]] = {}
    for f in sorted(d.glob("*.log")):
        s = f.read_text(errors="replace")
        last = s.strip().splitlines()[-1] if s.strip() else ""
        for m in re.finditer(r"^(PASSED|FAILED|ERROR|SKIPPED)(?: \[\d+\])? harness/tests/(\S+?)(?:::|:\d+: )(\S*)", s, re.M):
            if m.group(1) == "SKIPPED":
                res[(m.group(2), f"(skipped) {m.group(3)[:60]}")] = ["SKIPPED", "", ""]
            else:
                res[(m.group(2), m.group(3))] = [m.group(1), "", ""]
        parts = re.split(r"\n_{3,} (?:ERROR at \w+ of )?(test_\w+(?:\[[^\]]*\])?) _{3,}\n", s)
        for i in range(1, len(parts) - 1, 2):
            name, body = parts[i], parts[i + 1]
            errors = [ln for ln in body.splitlines() if ln.startswith("E ")]
            where = re.findall(r"^harness/tests/(\S+\.py):(\d+): ", body, re.M)
            for k in res:
                if k[1] == name and res[k][0] in ("FAILED", "ERROR"):
                    res[k][1] = re.sub(r"\s+", " ", errors[0][1:].strip())[:150] if errors else ""
                    res[k][2] = ", ".join(f"{a}:{b}" for a, b in where[-2:])
        if not re.search(r"\d+ (passed|failed|error|skipped)", last):
            res[(f.stem, "(file not finished)")] = ["UNFINISHED", last[:80], ""]
    return res


def summary(out: Path, control: Path | None) -> int:
    mine = read(out)
    ctl = read(control) if control else None
    totals: dict[str, int] = {}
    for v in mine.values():
        totals[v[0]] = totals.get(v[0], 0) + 1
    print(f"{out}: " + ", ".join(f"{n} {k.lower()}" for k, n in sorted(totals.items())))
    own = 0
    for k in sorted(mine):
        state, error, where = mine[k]
        if state == "PASSED":
            continue
        there = ctl.get(k, ["not run"])[0] if ctl is not None else None
        if there is not None and there != "PASSED":
            print(f"  ({there.lower()} on the control too) {k[0]}::{k[1]}")
            continue
        own += 1
        print(f"  {state} {k[0]}::{k[1]}")
        if error or where:
            print(f"      {error}\n      at {where}")
    return 1 if own else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--template", type=Path, help="the build's template user folder (default: P+'s)")
    ap.add_argument("--folder", help='the build\'s folder on the card ("Cobalt"), for a template without a '
                                     'build.json (harness/tools/mod_template.py writes it)')
    ap.add_argument("--offline-launcher", help="the build's own offline launcher, a file of the template's Launcher "
                                               "folder (REX: \"REX Offline Launcher.elf\"); default P+'s")
    ap.add_argument("--netplay-launcher", help="the same for the netplay launcher")
    ap.add_argument("--out", type=Path, help="where the logs go")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--timeout", type=float, default=1800.0, help="seconds a target may take before it is stopped")
    ap.add_argument("--split", action="store_true",
                    help="every test of a file as a job of its own (a file's tests otherwise run one after another)")
    ap.add_argument("--stagger", type=float, default=4.0,
                    help="seconds between the first jobs' starts (games all booting at once time out the "
                         "harness's start probe)")
    ap.add_argument("--summary", type=Path, help="only print the summary of this run")
    ap.add_argument("--control", type=Path, help="a run on stock P+ to compare with")
    ap.add_argument("targets", nargs="*", help="test files or node ids (default: the scope's files)")
    a = ap.parse_args()
    if a.summary:
        return summary(a.summary, a.control)
    if not a.out or (a.folder and not a.template):
        ap.error("--out; --folder only with --template")
    a.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.pop("PPHARNESS_MOD_FOLDER", None)
    if a.template:
        # The template says which build it is (its build.json, ppharness/paths.py); --folder and
        # the launcher flags are for a template made without one.
        env.update(PPHARNESS_TEMPLATE=str(a.template.resolve()))
        if a.folder:
            env.update(PPHARNESS_MOD_FOLDER=a.folder)
    for name, value in (("PPHARNESS_OFFLINE_LAUNCHER", a.offline_launcher),
                        ("PPHARNESS_NETPLAY_LAUNCHER", a.netplay_launcher)):
        env.pop(name, None)
        if value:
            env[name] = value
    tag = re.sub(r"[^A-Za-z0-9]", "", a.out.name)[:12] or "suite"
    targets = a.targets or FILES
    if a.split:
        targets = [t for target in targets for t in split(target, env)]

    def job(numbered: tuple[int, str]) -> str:
        i, target = numbered
        if i < a.jobs:
            time.sleep(a.stagger * i)
        return run_one(target, a.out, env, tag, a.timeout)

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for line in pool.map(job, enumerate(targets)):
            print(line, flush=True)
    return summary(a.out, a.control)


if __name__ == "__main__":
    sys.exit(main())
