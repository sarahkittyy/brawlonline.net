"""The pad thread watchdog (Gprb::PadsWatchdog, Core/Rollback/GameplayRollback.h).

Players lost their controller for good after a ranked stage select (2026-10-11: Brawl's pad thread
made no pass again). The pad thread suspends itself after every pass and the pad alarm resumes the
thread gfPadSystem+0xB54 names, so clearing that pointer stops the thread the same way from the
outside. After a second of emulated time without a pass the log has one report (the pad thread,
its alarm in the OS alarm queue, every guest thread), and with the pointer back the thread runs
again and the log says so.
"""
from __future__ import annotations

import re
import struct
import time

import pytest

from ppharness.instance import DolphinInstance

pytestmark = [pytest.mark.dolphin]

PAD_SYSTEM_PTR = 0x805A0040
PAD_SYS_THREAD_OFF = 0xB54
PAD_THREAD = 0x805BA108


@pytest.fixture(scope="module")
def booted(request, dolphin_exe):
    keep = request.config.getoption("--keep")
    inst = DolphinInstance("pad-watchdog", exe=dolphin_exe, keep=keep, connect_timeout=90)
    inst.start()
    yield inst
    failed = request.session.testsfailed > 0
    if failed:
        inst.mark_failed()
    inst.cleanup(not failed)


def test_stopped_pad_thread_is_reported(booted: DolphinInstance) -> None:
    c = booted.client
    st = c.wait_state("running", timeout=90)
    c.wait_frame(st.frame + 600, timeout_ms=60000)   # the game's menus: the pad thread runs
    pad_system = c.read_u32(PAD_SYSTEM_PTR)
    thread_obj = c.read_u32(pad_system + PAD_SYS_THREAD_OFF)
    assert thread_obj == PAD_THREAD - 8, hex(thread_obj)
    since = len(booted.log.read())

    c.write_mem(pad_system + PAD_SYS_THREAD_OFF, struct.pack(">I", 0))
    try:
        c.wait_frame(c.status().frame + 150, timeout_ms=30000)   # 2.5 s of fields
        time.sleep(0.3)
        log = booted.log.read()[since:]
        print("\n".join(l for l in log.splitlines() if "pad watchdog" in l))
        m = re.search(r"pad watchdog: the pad thread made no pass for ([\d.]+) s \(scene [^)]*\): run flag 1, "
                      rf"gfPadSystem {pad_system:08x} thread 00000000", log)
        assert m and float(m.group(1)) >= 1.0, log[-3000:]
        assert re.search(rf"pad watchdog: pad thread {PAD_THREAD:08x} state \d+ suspend 1 ", log), log[-3000:]
        m = re.search(rf"pad watchdog: pad alarm {pad_system:08x} queued: handler 80028b20 fire in (-?\d+) ", log)
        assert m, log[-3000:]
        assert abs(int(m.group(1))) < 3 * 1013496, m.group(0)   # within a few fields (period 1013496)
        assert re.search(r"pad watchdog: alarm queue .*\[805bacc0 handler 80028b20", log), log[-3000:]
        assert len(re.findall(r"pad watchdog: thread [0-9a-f]{8} state", log)) >= 3, log[-3000:]
        assert log.count("made no pass") == 1, "reported more than once"
        assert "runs again" not in log
    finally:
        c.write_mem(pad_system + PAD_SYS_THREAD_OFF, struct.pack(">I", thread_obj))

    c.wait_frame(c.status().frame + 30, timeout_ms=20000)
    time.sleep(0.3)
    log = booted.log.read()[since:]
    assert "pad watchdog: the pad thread runs again" in log, log[-2000:]
