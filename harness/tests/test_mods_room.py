"""Other Games (Brawl Mods) rooms: a joiner's wait for the room's players (Online/ModsRoom.cpp,
Rollback/MachineSession.cpp JoinStep; docs/design/brawl-mods.md §6.7).

Two DolphinNoGUI against our local accounts + mm servers, armed from PPR_MODS_ROOM as the
launcher arms them: a host opens a room for a made-up build, a joiner joins it. They boot P+ from
the instance's own card (the same on both, so there is nothing of the card to download). No GPU
is needed: the Null video backend.
"""

from __future__ import annotations

import json
import time
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend, OnlineUser
from ppharness.instance import DolphinInstance, InstanceConfig

from test_online import _logged_in, _wait
from test_rooms import backend  # noqa: F401  (the backend fixture)

pytestmark = [pytest.mark.dolphin, pytest.mark.server]

BUILD = {"hash": "ab" * 32, "name": "Test build"}


def _boot(dolphin: Callable[..., DolphinInstance], be: OnlineBackend, name: str, user: OnlineUser,
          room: dict[str, Any], block: str = "") -> DolphinInstance:
    ini = {"Online": {"UseDevServer": True, "MatchmakingPort": be.mm_port, "DevAccountsUrl": be.accounts_url}}
    env = {"PPR_MODS_ROOM": json.dumps(room), **({"PPR_TEST_P2P_BLOCK": block} if block else {})}
    inst = dolphin(name, config=InstanceConfig(cpu_thread=True, video_backend="Null", dolphin_ini=ini),
                   client_timeout=60.0, env=env)
    user.write_user_json(inst.create())
    inst.launch()
    inst.connect()
    _logged_in(inst, user)
    return inst


def _host(dolphin: Callable[..., DolphinInstance], be: OnlineBackend, name: str, user: OnlineUser,
          block: str = "") -> tuple[DolphinInstance, str]:
    host = _boot(dolphin, be, name, user, {"host": True, "build": BUILD, "public": False, "controller": "gc"},
                 block)
    code = _wait(lambda: (host.client.call("mods_status").get("room") or {}).get("code"), 60, "the room's code")
    _wait(lambda: any(s.get("endpoint") for s in host.client.call("mods_status")["room"]["slots"]), 30,
          "the host's endpoint")
    return host, code


def _log(inst: DolphinInstance) -> str:
    return (inst.create() / "Logs" / "dolphin.log").read_text(errors="replace")


@pytest.mark.parametrize("sd_sync", ["yes", "ask"])
def test_joiner_says_who_it_cannot_reach(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                        sd_sync: str) -> None:
    """Bug report #149: a REX room's host never answered the joiner (30 attempts, no hello either
    way), after the player said Yes to downloading the host's SD card; the download waits for
    that link, so it never started, and the frozen game said nothing until JOIN_TIMEOUT (300 s).
    Here host and joiner drop each other's packets (PPR_TEST_P2P_BLOCK). The joiner names the
    host in its chat after 10 s (with Yes: the download starts once connected), and gives up
    after 60 s with that reason, which the launcher shows ("Could not join the game: ...")."""
    tag = "Y" if sd_sync == "yes" else "A"
    host_user = backend.create_user("hosty", "HST" + tag)
    join_user = backend.create_user("joiny", "JNY" + tag)
    host, code = _host(dolphin, backend, f"mods-nolink-host-{sd_sync}", host_user, block=join_user.connect_code)
    joiner = _boot(dolphin, backend, f"mods-nolink-join-{sd_sync}", join_user,
                   {"join": code, "build": BUILD, "controller": "gc", "sd_sync": sd_sync},
                   block=host_user.connect_code)
    t0 = time.monotonic()
    seen: dict[str, float] = {}
    while time.monotonic() - t0 < 100 and not any(k.startswith("error") for k in seen):
        try:
            st = joiner.client.call("mods_status", timeout=5)
            lines = joiner.client.call("chat_status", timeout=5).get("lines", [])
        except Exception:  # noqa: BLE001  (Dolphin closes after a join that failed)
            break
        for ln in lines:
            seen.setdefault(f"chat: {ln.get('text', '')}", time.monotonic() - t0)
        if st.get("error"):
            seen.setdefault(f"error: {st['error']}", time.monotonic() - t0)
        time.sleep(0.5)
    print({k: round(v, 1) for k, v in seen.items()})
    then = ("The SD card download starts once connected." if sd_sync == "yes" else "Joining waits for it.")
    notice = f"chat: Can't connect to hosty yet. {then}"
    assert notice in seen, seen
    assert 8 <= seen[notice] <= 20, seen
    err = ("error: Could not join the game: no connection to hosty after 60 s: your network or theirs "
           "blocks a direct connection between you")
    assert err in seen, seen
    assert 55 <= seen[err] <= 75, seen
    log = _log(joiner)
    assert "no link to player 1" in log and "asked to join" not in log
    assert "Not admitted" not in log


def test_joiner_with_a_link_joins(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance]) -> None:
    """The same room with nothing blocked: the joiner joins and plays, with no such notice."""
    host_user = backend.create_user("hostc", "HSTC")
    join_user = backend.create_user("joinc", "JNYC")
    host, code = _host(dolphin, backend, "mods-link-host", host_user)
    joiner = _boot(dolphin, backend, "mods-link-join", join_user,
                   {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes"})
    _wait(lambda: "machine: running" in _log(joiner), 90, "the joiner's session running", interval=1)
    st = joiner.client.call("mods_status")
    lines = joiner.client.call("chat_status").get("lines", [])
    assert not st.get("error"), st
    assert not any("Can't connect" in ln.get("text", "") for ln in lines), lines
