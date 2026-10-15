"""Dolphin's stall tracer (Source/Core/Core/StallTracer.h): an emulated field that does not come
for 250 ms while the emulation runs is logged with each thread's CPU time and (Windows) where the
CPU, GPU and UI threads were. The stalls are made from outside: the CPU thread alone suspended
(the tracer samples it; it used no CPU in the stall), the whole process suspended (every thread
idle: what a busy machine looks like), the CPU thread suspended past LONG_STALL_MS (traced again
while it lasts), and the video thread suspended for good, then the emulation stopped as closing
the game window does: Dolphin exits by itself STOP_HANG_MS later with STOP_HANG_EXIT_CODE (a
player's frozen game on Linux, 2026-10-14, could only be force quit). Windows only (the
suspensions use its API).
"""
from __future__ import annotations

import ctypes
import re
import sys
import time
from ctypes import wintypes

import pytest

from ppharness.instance import DolphinInstance

pytestmark = [pytest.mark.dolphin, pytest.mark.skipif(sys.platform != "win32", reason="Windows API")]

TH32CS_SNAPTHREAD = 0x4
THREAD_SUSPEND_RESUME = 0x2
THREAD_QUERY_LIMITED_INFORMATION = 0x800
PROCESS_SUSPEND_RESUME = 0x800


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ThreadID", wintypes.DWORD),
                ("th32OwnerProcessID", wintypes.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", wintypes.DWORD)]


def thread_named(pid: int, name: str) -> int:
    k = ctypes.windll.kernel32
    snap = k.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    try:
        ok = k.Thread32First(snap, ctypes.byref(e))
        while ok:
            if e.th32OwnerProcessID == pid:
                h = k.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, e.th32ThreadID)
                if h:
                    desc = ctypes.c_wchar_p()
                    try:
                        if k.GetThreadDescription(h, ctypes.byref(desc)) >= 0 and desc.value == name:
                            return e.th32ThreadID
                    finally:
                        k.CloseHandle(h)
            ok = k.Thread32Next(snap, ctypes.byref(e))
    finally:
        k.CloseHandle(snap)
    raise AssertionError(f"no thread named {name!r} in {pid}")


def suspend_thread(tid: int, seconds: float) -> None:
    k = ctypes.windll.kernel32
    h = k.OpenThread(THREAD_SUSPEND_RESUME, False, tid)
    assert h
    try:
        k.SuspendThread(h)
        time.sleep(seconds)
        k.ResumeThread(h)
    finally:
        k.CloseHandle(h)


def suspend_thread_for_good(tid: int) -> None:
    k = ctypes.windll.kernel32
    h = k.OpenThread(THREAD_SUSPEND_RESUME, False, tid)
    assert h
    try:
        assert k.SuspendThread(h) != 0xFFFFFFFF
    finally:
        k.CloseHandle(h)


def suspend_process(pid: int, seconds: float) -> None:
    k = ctypes.windll.kernel32
    nt = ctypes.windll.ntdll
    h = k.OpenProcess(PROCESS_SUSPEND_RESUME, False, pid)
    assert h
    try:
        nt.NtSuspendProcess(h)
        time.sleep(seconds)
        nt.NtResumeProcess(h)
    finally:
        k.CloseHandle(h)


@pytest.fixture(scope="module")
def booted(request, dolphin_exe):
    keep = request.config.getoption("--keep")
    inst = DolphinInstance("stall-tracer", exe=dolphin_exe, keep=keep, connect_timeout=90)
    inst.start()
    yield inst
    failed = request.session.testsfailed > 0
    if failed:
        inst.mark_failed()
    inst.cleanup(not failed)


def stall_lines(inst: DolphinInstance, since: int) -> str:
    return inst.log.read()[since:]


def test_cpu_thread_stall_is_traced(booted: DolphinInstance) -> None:
    c = booted.client
    st = c.wait_state("running", timeout=90)
    c.wait_frame(st.frame + 120, timeout_ms=30000)
    pid = booted.process.pid
    since = len(booted.log.read())
    try:
        tid = thread_named(pid, "CPU thread")
    except AssertionError:
        tid = thread_named(pid, "CPU-GPU thread")   # single core
    suspend_thread(tid, 1.2)
    c.wait_frame(c.status().frame + 30, timeout_ms=20000)
    time.sleep(0.5)
    log = stall_lines(booted, since)
    print(chr(10).join(l for l in log.splitlines() if "Stall" in l or " thread: " in l))
    m = re.search(r"Stall: no emulated field for (\d+) ms", log)
    assert m and int(m.group(1)) >= 250, log[-2000:]
    assert re.search(r"cpu thread: -?\d+ ms CPU since the field, at \S+", log), log[-2000:]
    over = re.search(r"Stall over: (\d+) ms without an emulated field; CPU time in it: cpu (\d+) ms", log)
    assert over and 1000 <= int(over.group(1)) <= 3000 and int(over.group(2)) < 200, log[-2000:]


def test_process_stall_shows_no_cpu_time(booted: DolphinInstance) -> None:
    c = booted.client
    pid = booted.process.pid
    time.sleep(6)   # past the tracer's rate limit
    since = len(booted.log.read())
    suspend_process(pid, 1.5)
    c.wait_frame(c.status().frame + 30, timeout_ms=20000)
    time.sleep(0.5)
    log = stall_lines(booted, since)
    print(chr(10).join(l for l in log.splitlines() if "Stall" in l or " thread: " in l))
    over = re.search(r"Stall over: (\d+) ms without an emulated field; CPU time in it:(.*)", log)
    assert over and int(over.group(1)) >= 1200, log[-2000:]
    used = {k: int(v) for k, v in re.findall(r"(\w+) (\d+) ms", over.group(2))}
    assert used and all(v < 200 for v in used.values()), over.group(0)


def test_long_stall_is_traced_again(booted: DolphinInstance) -> None:
    c = booted.client
    pid = booted.process.pid
    since = len(booted.log.read())
    try:
        tid = thread_named(pid, "CPU thread")
    except AssertionError:
        tid = thread_named(pid, "CPU-GPU thread")
    # The first trace may fall in the rate limit of the tests before; the 2 s one never does.
    suspend_thread(tid, 2.8)
    c.wait_frame(c.status().frame + 30, timeout_ms=20000)
    time.sleep(0.5)
    log = stall_lines(booted, since)
    print(chr(10).join(l for l in log.splitlines() if "Stall" in l or " thread: " in l))
    m = re.search(r"Stall goes on: no emulated field for (\d+) ms \(after field \d+\)\r?\n"
                  r"  cpu thread: -?\d+ ms CPU since the field, at \S+", log)
    assert m and 2000 <= int(m.group(1)) < 2700, log[-2000:]
    assert len(re.findall(r"Stall goes on", log)) == 1, log[-2000:]   # the next is at 8 s
    over = re.search(r"Stall over: (\d+) ms without an emulated field", log)
    assert over and int(over.group(1)) >= 2500, log[-2000:]


def test_stuck_stop_exits_dolphin(request, dolphin_exe) -> None:
    keep = request.config.getoption("--keep")
    inst = DolphinInstance("stall-tracer-stop", exe=dolphin_exe, keep=keep, connect_timeout=90)
    inst.start()
    ok = False
    try:
        c = inst.client
        st = c.wait_state("running", timeout=90)
        c.wait_frame(st.frame + 120, timeout_ms=30000)
        pid = inst.process.pid
        since = len(inst.log.read())
        # As the player's GPU thread: the video thread never comes back; the CPU thread waits on it.
        suspend_thread_for_good(thread_named(pid, "Video thread"))
        time.sleep(1.0)
        c.call("emu_stop")
        t0 = time.monotonic()
        code = inst.process.wait(30)
        took = time.monotonic() - t0
        log = inst.log.read()[since:]
        print(chr(10).join(l for l in log.splitlines()
                           if "Stall" in l or " thread: " in l or "Dolphin exits" in l
                           or "did not stop" in l))
        assert code == 70, (code, log[-3000:])
        assert 9.5 <= took <= 14, took
        m = re.search(r"The emulation did not stop 10 s after the stop was asked for; Dolphin exits "
                      r"\(after field \d+\)\r?\n(?:  cpu thread: .*\r?\n)?  gpu thread: -?\d+ ms CPU since the "
                      r"field, at \S+", log)
        assert m, log[-3000:]
        assert "Dolphin exits (code 70): the emulation is stuck" in log, log[-3000:]
        ok = True
    finally:
        if not ok:
            inst.mark_failed()
        inst.kill()
        inst.cleanup(ok)
