"""Rollback regression tests against the real build (rollback-fixes branch of dolphin/).

Each test checks a bug that was fixed in the rollback core:

* ``test_synctest_*``: single-instance sync test (``PPR_SYNCTEST=N``, GekkoNet stress session).
  Every frame is loaded N frames back and resimulated with the same inputs; the GekkoNet
  checksum (frame counter + fighters) must agree between a frame and its re-runs. Catches
  save/load and determinism bugs without any network (wrong snapshot slot, untracked writes,
  timing not restored, resimulation shortcuts).
* ``test_netplay_match_*``: two instances through netsim, CSS -> SSS -> match -> fighting.
  No side may freeze (the scene-transition hangs), private memory must stay bounded (the
  snapshot leak), both peers must feed the game identical pads on every confirmed frame and
  compute identical per-frame checksums (desyncs), and GekkoNet must report no desync.
* ``test_netplay_press_lands_on_same_frame``: a single-frame press on either side lands on the
  same GekkoNet frame on both peers (the live-vs-delayed input asymmetry).
* ``test_rollback_presents_one_frame_per_displayed_frame``: with a real video backend, frames of
  resimulated iterations never reach the presenter: exactly one frame is sent to the presenter per
  displayed game frame, also right after a rollback (marked ``gpu``; skipped without one).

Every test runs in single core (``sc``) and dual core (``dc``, CPUThread = True, the default).

Run: ``pytest -m dolphin tests/test_rollback.py`` (about 15 minutes; uses 1-2 Dolphins at a time).
``-k sc`` or ``-k dc`` picks one mode; ``--no-gpu`` skips the real-backend test.
"""

from __future__ import annotations

import math
import random
import socket
import sys
import threading
import time

import pytest

from ppharness import brawl as B
from ppharness import flows as F
from ppharness.client import HarnessError
from ppharness.instance import DolphinInstance, InstanceConfig, find_free_port
from ppharness.session import two_player_netplay

pytestmark = pytest.mark.dolphin

# Single core and dual core (CPUThread = True, Dolphin's and the fork's default).
CPU_MODES = [pytest.param(False, id="sc"), pytest.param(True, id="dc")]

# Peak private memory allowed per Dolphin. The leak grew instances to 11-16 GB in ~90 s; a healthy
# rollback instance stays around 1.1 GB.
MAX_PRIVATE_MB = 3000


# --------------------------------------------------------------------------- helpers


def _private_mb(pid: int) -> float | None:
    """Private memory of a process in MiB (Windows: private bytes; Linux: anonymous RSS + swap;
    None elsewhere)."""
    if sys.platform.startswith("linux"):
        try:
            with open(f"/proc/{pid}/status") as f:
                kb = {k: int(v.split()[0]) for k, v in (ln.split(":", 1) for ln in f if ":" in ln)
                      if k in ("RssAnon", "VmSwap")}
        except (OSError, ValueError):
            return None
        return sum(kb.values()) / 1024 if kb else None
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes

    class PMC(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                    ("PrivateUsage", ctypes.c_size_t)]

    k32 = ctypes.WinDLL("kernel32")
    psapi = ctypes.WinDLL("psapi")
    h = k32.OpenProcess(0x1000 | 0x0010, False, pid)
    if not h:
        return None
    try:
        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        if not psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
            return None
        return pmc.PrivateUsage / 2 ** 20
    finally:
        k32.CloseHandle(h)


class Watch(threading.Thread):
    """Samples GekkoNet frames and private memory of each instance every 0.5 s."""

    def __init__(self, instances):
        super().__init__(daemon=True)
        self.instances = instances
        self.stop_ev = threading.Event()
        self.frames: dict[str, list[tuple[float, int]]] = {i.name: [] for i in instances}
        self.peak_mb: dict[str, float] = {i.name: 0.0 for i in instances}

    def run(self):
        t0 = time.monotonic()
        while not self.stop_ev.is_set():
            for inst in self.instances:
                try:
                    rb = inst.client.netplay_status().rollback or {}
                    if rb.get("session_started"):
                        self.frames[inst.name].append((time.monotonic() - t0, int(rb["current_frame"])))
                except HarnessError:
                    pass
                mb = _private_mb(inst.process.pid) if inst.process else None
                if mb:
                    self.peak_mb[inst.name] = max(self.peak_mb[inst.name], mb)
            self.stop_ev.wait(0.5)

    def longest_stall(self, name: str) -> float:
        """Longest time (s) the GekkoNet frame did not move once the session ran."""
        worst, last_v, last_t = 0.0, None, None
        for t, f in self.frames[name]:
            if f != last_v:
                last_v, last_t = f, t
            else:
                worst = max(worst, t - last_t)
        return worst

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop_ev.set()
        self.join(timeout=10)


def _require_rollback_history(client) -> None:
    try:
        client.rollback_pad_history(0)
    except HarnessError as e:
        pytest.skip(f"build without rollback_pad_history: {e}")


def _compare_histories(host, joiner) -> dict:
    """Pads and per-frame checksums on frames both peers have confirmed."""
    h, j = host.rollback_pad_history(0), joiner.rollback_pad_history(0)
    upto = min(max(h, default=0), max(j, default=0)) - 10  # well past the rollback window
    common = sorted(f for f in h if f in j and f <= upto)
    pads = [f for f in common if h[f][:5] != j[f][:5]]
    # columns 5..8: combined, legacy, frame counter, fighters (0 outside a match)
    state = [f for f in common if len(h[f]) > 8 and h[f][5] and j[f][5] and h[f][5:9] != j[f][5:9]]
    in_match = [f for f in common if len(h[f]) > 8 and h[f][5] and j[f][5]]
    return {"compared": len(common), "in_match": len(in_match), "pad_mismatch": pads[:10],
            "state_mismatch": state[:10]}


# --------------------------------------------------------------------------- sync test


def _synctest_training(dolphin_exe, distance: int, seconds: float, name: str,
                       cpu_thread: bool = False) -> dict:
    env = {"PPR_SYNCTEST": str(distance)}
    rng = random.Random(distance)
    with DolphinInstance(name, exe=dolphin_exe, config=InstanceConfig(cpu_thread=cpu_thread), boot=None,
                         env=env, connect_timeout=90) as inst:
        c = inst.client
        c.netplay_host(find_free_port(kind=socket.SOCK_DGRAM), inst.netplay_launcher(), name="solo",
                       rollback=True)
        deadline = time.monotonic() + 30
        while True:
            try:
                c.netplay_start()
                break
            except HarnessError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        rb = c.netplay_status().rollback or {}
        if "synctest" not in rb:
            pytest.skip("build without PPR_SYNCTEST support")
        c.pad_set(0, buttons=["L"])  # P+ BootToCSS: L/R held at sqBoot -> Training (1 player + CPU)
        deadline = time.monotonic() + 120
        while True:
            try:
                if B.read_scene(c.read_mem).scene is B.Scene.CSS:
                    break
            except HarnessError:
                pass
            assert time.monotonic() < deadline, "never reached the Training CSS"
            time.sleep(0.5)
        B.neutral(c, 0)
        B.wait_css_ready(c, [0])
        B.css_pick_character(c, 0, B.CSS_ID["fox"])
        B.css_start(c, 0)
        B.sss_pick_stage(c, B.STAGE_KIND["battlefield"], 0)
        B.wait_match_start(c)
        t_end = time.monotonic() + seconds
        while time.monotonic() < t_end:
            ang = rng.uniform(0, 2 * math.pi)
            mag = rng.choice([0, 60, 100])
            c.pad_script(0, [{"buttons": rng.choice([[], [], ["A"], ["B"], ["X"], ["R"]]),
                              "main": [int(128 + mag * math.cos(ang)), int(128 + mag * math.sin(ang))],
                              "hold": rng.randint(2, 12)}])
            time.sleep(rng.uniform(0.05, 0.25))
        rb = c.netplay_status().rollback
        assert B.read_scene(c.read_mem).scene is B.Scene.IN_MATCH
        return rb


@pytest.mark.slow
@pytest.mark.parametrize("cpu_thread", CPU_MODES)
@pytest.mark.parametrize("distance", [2, 4])
def test_synctest_training_match_has_no_desync(request, dolphin_exe, distance, cpu_thread):
    rb = _synctest_training(dolphin_exe, distance, 20, f"st-{request.node.name}", cpu_thread)
    # Enough re-runs to mean something. Distance 4 resimulates 4 frames per frame and runs slowly
    # when the machine is busy (494 in 20 s with four other Dolphins running), so keep this loose.
    assert rb["rollbacks"] > 200, rb
    # GekkoNet's stress session compares every frame's checksum with its re-runs.
    assert rb["desyncs_detected"] == 0, rb


# --------------------------------------------------------------------------- two-player netplay


@pytest.mark.slow
@pytest.mark.parametrize("cpu_thread", CPU_MODES)
@pytest.mark.parametrize("preset", ["lan", "typical"])
def test_netplay_match_no_freeze_no_desync_bounded_memory(request, dolphin_exe, preset, cpu_thread):
    mode = "dc" if cpu_thread else "sc"
    with two_player_netplay(preset, rollback=True, exe=dolphin_exe, name=f"rb-{preset}-{mode}",
                            config=InstanceConfig(cpu_thread=cpu_thread), seed=7) as s:
        host, joiner = s.clients
        _require_rollback_history(host)
        with Watch(s.instances) as w:
            hs, js = F.netplay_seat(host, "host"), F.netplay_seat(joiner, "joiner")
            F.to_css([hs, js])
            F.pick_characters([(hs, "fox"), (js, "falco")])
            F.start_and_pick_stage(hs, "battlefield")
            F.wait_match_started([host, joiner])
            F.play([(hs, "random"), (js, "random")], 60 * 25, seed=preset)
            for c, seat in ((host, hs), (joiner, js)):
                B.neutral(c, seat.pp)
            time.sleep(2)  # quiet window: let every frame we compare become confirmed
            cmp = _compare_histories(host, joiner)
            stats = {i.name: i.client.netplay_status().rollback for i in s.instances}
        for name in w.frames:
            assert w.longest_stall(name) < 3.0, f"{name} froze: {w.frames[name][-5:]}"
            assert w.peak_mb[name] < MAX_PRIVATE_MB or w.peak_mb[name] == 0, w.peak_mb
        assert cmp["compared"] > 1000 and cmp["in_match"] > 600, cmp
        assert not cmp["pad_mismatch"], cmp
        assert not cmp["state_mismatch"], cmp
        for name, rb in stats.items():
            assert rb["desyncs_detected"] == 0, (name, rb)


@pytest.mark.slow
@pytest.mark.parametrize("cpu_thread", CPU_MODES)
def test_netplay_press_lands_on_same_frame(request, dolphin_exe, cpu_thread):
    mode = "dc" if cpu_thread else "sc"
    with two_player_netplay("typical", rollback=True, exe=dolphin_exe, name=f"rb-press-{mode}",
                            config=InstanceConfig(cpu_thread=cpu_thread), seed=3) as s:
        host, joiner = s.clients
        _require_rollback_history(host)
        F.to_css([F.netplay_seat(host, "host"), F.netplay_seat(joiner, "joiner")])
        for _ in range(3):
            joiner.pad_script(0, [{"buttons": ["X"], "hold": 1}])
            time.sleep(1.0)
            host.pad_script(0, [{"buttons": ["Y"], "hold": 1}])
            time.sleep(1.0)
        time.sleep(2)
        seen = {}
        for name, c in (("host", host), ("joiner", joiner)):
            h = c.rollback_pad_history(0)
            seen[name] = {"p1_X": sorted(f for f, r in h.items() if r[1] & 0x400),
                          "p0_Y": sorted(f for f, r in h.items() if r[0] & 0x800)}
        assert seen["host"]["p1_X"] and seen["host"]["p0_Y"], seen
        assert seen["host"] == seen["joiner"], seen


# --------------------------------------------------------------------------- presentation


def _presentation(client) -> dict:
    p = client.status().raw.get("presentation")
    if p is None:
        pytest.skip("build without presentation counters")
    return p


@pytest.mark.slow
@pytest.mark.gpu
@pytest.mark.parametrize("cpu_thread", CPU_MODES)
def test_rollback_presents_one_frame_per_displayed_frame(request, dolphin_exe, gpu_backend, cpu_thread):
    """bad_wifi on the CSS (both players moving): plenty of rollbacks. Before the fix a rollback of
    N frames sent N+1 frames to the presenter within one real frame (a flash/stutter)."""
    mode = "dc" if cpu_thread else "sc"
    with two_player_netplay("bad_wifi", rollback=True, exe=dolphin_exe, name=f"rb-present-{mode}",
                            config=InstanceConfig(cpu_thread=cpu_thread, video_backend=gpu_backend),
                            seed=5) as s:
        host, joiner = s.clients
        hs, js = F.netplay_seat(host, "host"), F.netplay_seat(joiner, "joiner")
        F.to_css([hs, js])
        before = {i.name: _presentation(i.client) for i in s.instances}
        F.run_parallel([lambda: F.menu_wander(hs, 60 * 15, seed=1),
                        lambda: F.menu_wander(js, 60 * 15, seed=2)])
        after = {i.name: _presentation(i.client) for i in s.instances}
        rb = {i.name: i.client.netplay_status().rollback for i in s.instances}
    for name in after:
        a, b = before[name], after[name]
        d = {k: b[k] - a[k] for k in ("displayed_frames", "displayed_frames_after_resim", "presents",
                                      "xfb_copies_skipped", "xfb_fields_skipped", "copy_decision_misses")}
        hist = [y - x for x, y in zip(a["outputs_per_frame"], b["outputs_per_frame"])]
        hist_after = [y - x for x, y in zip(a["outputs_per_frame_after_resim"],
                                            b["outputs_per_frame_after_resim"])]
        info = (name, d, hist, hist_after, rb[name])
        assert not b["present_resimulated"], info
        assert d["displayed_frames"] > 300, info
        # Enough rollbacks to mean something, and their frames were kept off the screen.
        assert d["displayed_frames_after_resim"] >= 10, info
        assert d["xfb_copies_skipped"] + d["xfb_fields_skipped"] > 0, info
        assert d["copy_decision_misses"] == 0, info
        # Exactly one frame sent to the presenter per displayed frame, including right after a rollback.
        assert sum(hist) > 300 and hist[1] == sum(hist), info
        assert hist_after[1] == sum(hist_after) > 0, info
        # And the presenter showed about one frame per displayed frame (it runs a little behind).
        assert abs(d["presents"] - d["displayed_frames"]) <= 5, info


# --------------------------------------------------------------------------- dual-core stress


def _save_samples(client, since: int) -> tuple[int, list[int], list[int]]:
    try:
        r = client.call("rollback_timings", since=since)
    except HarnessError as e:
        pytest.skip(f"build without rollback_timings: {e}")
    return int(r["next"]), list(r["save_us"]), list(r["sync_us"])


@pytest.mark.slow
@pytest.mark.gpu
@pytest.mark.parametrize("preset", ["bad_wifi", "awful"])
def test_dualcore_real_backend_long_session_no_desync(request, dolphin_exe, gpu_backend, preset):
    """Dual core with a real video backend over a bad link, several matches in a row (about
    PPR_TEST_LONG_SECONDS of match time, default 150): the GPU thread renders and writes EFB copies
    to RAM while the CPU thread snapshots, loads and resimulates up to 7 frames. Both peers must
    feed the game identical pads and compute identical checksums on every confirmed frame, GekkoNet
    must see no desync, no side may freeze, the GPU thread must run in deterministic mode, and the
    snapshots must not wait for the GPU thread (they did, 1.5-3.5 ms each, with 0.2-1 s spikes)."""
    import os

    seconds = float(os.environ.get("PPR_TEST_LONG_SECONDS", "150"))
    rng = random.Random(preset)
    with two_player_netplay(preset, rollback=True, exe=dolphin_exe, name=f"rb-dcstress-{preset}",
                            config=InstanceConfig(cpu_thread=True, video_backend=gpu_backend),
                            seed=11) as s:
        host, joiner = s.clients
        _require_rollback_history(host)
        hs, js = F.netplay_seat(host, "host"), F.netplay_seat(joiner, "joiner")
        nexts = [0, 0]
        saves: list[int] = []
        syncs: list[int] = []
        played, matches = 0.0, 0
        with Watch(s.instances) as w:
            while played < seconds:
                F.to_css([hs, js])
                F.pick_characters([(hs, rng.choice(["fox", "falco"])), (js, rng.choice(["fox", "falco"]))])
                F.start_and_pick_stage(hs, "battlefield")
                F.wait_match_started([host, joiner])
                matches += 1
                for i, c in enumerate((host, joiner)):
                    nexts[i], _, _ = _save_samples(c, nexts[i])
                t0 = time.monotonic()
                F.play([(hs, "random"), (js, "random")], int(60 * min(seconds - played, 75)),
                       seed=rng.randrange(1 << 30))
                played += time.monotonic() - t0
                for i, c in enumerate((host, joiner)):
                    nexts[i], sv, sy = _save_samples(c, nexts[i])
                    saves += sv
                    syncs += sy
                st = B.read_match_state(host.read_mem)
                if st.in_match and not st.game_set:
                    break
                B.wait_scene(host, [B.Scene.RESULTS], 60 * 20)
            for c, seat in ((host, hs), (joiner, js)):
                B.neutral(c, seat.pp)
            time.sleep(2)
            cmp = _compare_histories(host, joiner)
            stats = {i.name: i.client.netplay_status().rollback for i in s.instances}
    for name in w.frames:
        assert w.longest_stall(name) < 3.0, f"{name} froze: {w.frames[name][-5:]}"
    assert cmp["compared"] > 1000 and cmp["in_match"] > 600, cmp
    assert not cmp["pad_mismatch"], cmp
    assert not cmp["state_mismatch"], cmp
    for name, rb in stats.items():
        assert rb["desyncs_detected"] == 0, (name, rb)
        assert rb["rollbacks"] > 100, (name, rb)
        assert rb.get("gpu_deterministic") is True, (name, rb)
    # Snapshots run without waiting for the GPU thread: at most a handful of waits for a capture
    # the video thread had not reached a whole ring of snapshots later.
    saves.sort()
    assert len(saves) > 60 * 30, len(saves)
    assert sum(1 for x in syncs if x > 0) <= len(syncs) // 100, sorted(syncs)[-20:]
    p99 = saves[int(0.99 * (len(saves) - 1))]
    assert p99 < 10_000, ("save p99 us", p99, saves[-20:])
