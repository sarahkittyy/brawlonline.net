"""Wii controllers in P+'s online games over a bad link: a Wii Remote player and a Nunchuk player
press at random for a minute through netsim (60 ms each way, 8 ms jitter, 2% loss each way), and
the two machines' confirmed checksums must agree on every frame. Run with PPR_GPRB_EARLY_INPUT=0
for the A/B (early local input off): the test prints each machine's input, network and rollback
lines from its log (read->use, rollbacks, waits).

Instances are named ``wbl-*`` (``ppharness clean --prefix wbl-``).
"""

from __future__ import annotations

import random
import threading
import time
from typing import Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance
from ppharness.netsim import LinkProfile

from test_online_game import _boot, _confirmed_checksums, _p2p_through_netsim, _peer_shown, ppom
from test_online_unranked import backend  # noqa: F401
from test_online_wii_remote import NUNCHUK, WIIMOTE, Wii, _check_wii_port, _port, _running, \
    _search_with, _to_unranked_css

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

SECONDS = 60
LINES = ("gprb input:", "gprb net:", "gprb: input delay", "early local input")


def _log_lines(g) -> list[str]:
    path = g.inst.user_dir / "Logs" / "dolphin.log"
    if not path.exists():
        return []
    return [line.strip()[:400] for line in path.read_text(errors="replace").splitlines()
            if any(k in line for k in LINES)]


@pytest.mark.slow
def test_wii_controllers_over_a_bad_link(backend: OnlineBackend,  # noqa: F811
                                         dolphin: Callable[..., DolphinInstance],
                                         gpu_backend: str) -> None:
    test = "wii-bad-link"
    sims, inis = _p2p_through_netsim(LinkProfile(latency_ms=60, jitter_ms=8, loss=0.02))
    for ini in inis:
        ini["Online"]["InputDelay"] = 2   # both at 2 (auto picks 2 or 3 at this round trip)
    try:
        ua, ub = backend.create_user("wren", "WREN"), backend.create_user("nell", "NELL")
        a = _boot(dolphin, "wbl-a", backend, ua, gpu_backend, test, "gameplay", dolphin_ini=inis[0],
                  wiimote_ini={"Wiimote1": {"Source": 1, "Extension": "None"}})
        b = _boot(dolphin, "wbl-b", backend, ub, gpu_backend, test, "gameplay", dolphin_ini=inis[1],
                  wiimote_ini={"Wiimote1": {"Source": 1, "Extension": "Nunchuk"}})
        remote, nunchuk = Wii(a, "none"), Wii(b, "nunchuk")
        remote.set()
        nunchuk.set()
        _to_unranked_css((a, b))
        _search_with(a, lambda: remote.press(wiimote={"Buttons/+": 1}))
        time.sleep(2.2)
        _search_with(b, lambda: nunchuk.press(wiimote={"Buttons/+": 1}))
        for g in (a, b):
            assert ppom.read_local(g.c, ppom.find_block(g.c))["lock_pad"] == 4, g.name
        _peer_shown(a, ub)
        _peer_shown(b, ua)
        players = (a, b)
        _running(players)
        a.steps("wait 120")
        _check_wii_port(players, _port(a), WIIMOTE)
        _check_wii_port(players, _port(b), NUNCHUK)

        # Random presses on both (never PLUS or MINUS: no quit, no runback).
        stop = threading.Event()
        errors: list[str] = []

        def mash(w: Wii, seed: int, nunchuk_stick: bool) -> None:
            r = random.Random(seed)
            buttons = ["Buttons/A", "Buttons/B", "Buttons/1", "Buttons/2", "D-Pad/Up", "D-Pad/Down",
                       "D-Pad/Left", "D-Pad/Right"]
            while not stop.is_set():
                held = {r.choice(buttons): 1.0} if r.random() < 0.6 else {}
                parts = {}
                if nunchuk_stick:
                    parts["nunchuk"] = {"Stick/X": r.uniform(-1, 1), "Stick/Y": r.uniform(-1, 1)}
                    if r.random() < 0.3:
                        parts["nunchuk"]["Buttons/Z" if r.random() < 0.5 else "Buttons/C"] = 1.0
                try:
                    w.set(wiimote=held, **parts)
                except Exception as e:  # noqa: BLE001
                    errors.append(str(e))
                    return
                time.sleep(r.uniform(0.03, 0.25))

        threads = [threading.Thread(target=mash, args=(remote, 1, False), daemon=True),
                   threading.Thread(target=mash, args=(nunchuk, 2, True), daemon=True)]
        for t in threads:
            t.start()
        time.sleep(SECONDS)
        stop.set()
        for t in threads:
            t.join(10)
        remote.set()
        nunchuk.set()
        assert not errors, errors
        ck = _confirmed_checksums(a, b)
        for g in players:
            print(f"== {g.name}", flush=True)
            for line in _log_lines(g):
                print("   " + line, flush=True)
        print(f"CHECKSUMS {ck}", flush=True)
        assert ck["compared"] > SECONDS * 40 and ck["mismatches"] == 0, ck
    finally:
        for s in sims:
            s.stop()
