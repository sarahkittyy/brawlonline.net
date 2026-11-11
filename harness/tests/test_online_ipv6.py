"""IPv6 beside IPv4 (dolphin/Source/Core/Core/Online/Ipv6.h): the P2P connect tries both and uses
IPv4 whenever IPv4 connects; IPv6 only for a pair IPv4 cannot connect; nothing over IPv6 when
the server or a game has none. On one machine: IPv4 is 127.0.0.1, IPv6 is ::1 (mm's IPv6 probe
listener on [::1] with ``OnlineBackend(ipv6=True)``; the games' IPv6 server is ::1 with
UseDevServer).

The matchmaking tests need only Dolphin and the server (like tests/test_online.py). The session
tests further down boot P+ (like tests/test_online_game.py) and need the plugin and a GPU.
"""

from __future__ import annotations

import re
import time
from typing import Any, Callable, Iterator

import pytest

from ppharness.backend import OnlineBackend, OnlineUser
from ppharness.instance import DolphinInstance, InstanceConfig

from test_online import _logged_in, _make_backend, _wait

pytestmark = [pytest.mark.dolphin, pytest.mark.server]

V6_PEER = re.compile(r"^\[::1\]:(\d+)$")


@pytest.fixture(scope="module")
def backend6() -> Iterator[OnlineBackend]:
    be = _make_backend(ipv6=True)
    try:
        yield be
    finally:
        be.stop()


@pytest.fixture(scope="module")
def backend4() -> Iterator[OnlineBackend]:
    """mm without an IPv6 listener: the games' probes to [::1] reach nothing."""
    be = _make_backend(ipv6=False)
    try:
        yield be
    finally:
        be.stop()


def _config(be: OnlineBackend, online: dict[str, Any] | None = None) -> InstanceConfig:
    return InstanceConfig(dolphin_ini={"Online": {
        "UseDevServer": True,
        "MatchmakingPort": be.mm_port,
        "DevAccountsUrl": be.accounts_url,
        **(online or {}),
    }})


def _launch(dolphin: Callable[..., DolphinInstance], name: str, be: OnlineBackend,
            user: OnlineUser, env: dict[str, str] | None = None,
            online: dict[str, Any] | None = None) -> DolphinInstance:
    inst = dolphin(name, config=_config(be, online), boot=None, env=env)
    inst.create()
    user.write_user_json(inst.user_dir)
    inst.launch()
    inst.connect()
    _logged_in(inst, user)
    return inst


def _search(a: DolphinInstance, b: DolphinInstance, ua: OnlineUser, ub: OnlineUser,
            timeout: float = 60) -> list[dict[str, Any]]:
    a.client.mm_search_direct(ub.connect_code, session="none")
    time.sleep(2.2)  # one ticket per account per 2 s
    b.client.mm_search_direct(ua.connect_code, session="none")

    def done() -> list[dict[str, Any]] | None:
        st = [i.client.mm_status() for i in (a, b)]
        if any(s["state"] == "error" for s in st):
            raise AssertionError(f"matchmaking error: {[(s['error'], s['error_source']) for s in st]}")
        return st if all(s["state"] == "connection_success" for s in st) else None

    return _wait(done, timeout, "both to match and connect")


def _log(inst: DolphinInstance) -> str:
    return inst.log_path.read_text(errors="replace")


def _players(st: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    ps = st["match"]["players"]
    return [p for p in ps if p["is_local"]][0], [p for p in ps if not p["is_local"]][0]


def test_ipv4_is_used_when_it_connects(backend6: OnlineBackend,
                                        dolphin: Callable[..., DolphinInstance]) -> None:
    """Both games have IPv6 (mm hands each the other's [::1] address) and IPv4 connects: the
    match is made on IPv4, exactly the address IPv4 alone gives, though IPv6 was tried beside it."""
    ua, ub = backend6.create_user("ivy", "IVY"), backend6.create_user("jon", "JON")
    a = _launch(dolphin, "v6-both-a", backend6, ua)
    b = _launch(dolphin, "v6-both-b", backend6, ub)
    sa, sb = _search(a, b, ua, ub)
    for st, other, inst in ((sa, sb, a), (sb, sa, b)):
        me, peer = _players(st)
        # mm saw both probes from [::1] and the games' own ports.
        assert me["ip_address6"] == f"[::1]:{st['local_port']}", me
        assert peer["ip_address6"] == f"[::1]:{other['local_port']}", peer
        m = st["match"]
        assert m["local_ip6"] == "::1"
        assert m["remote6_addresses"] == [f"[::1]:{other['local_port']}"]
        # IPv4 won: the same address as without IPv6 (tests/test_online.py::_check_match).
        assert m["connected_addresses"] == [f"127.0.0.1:{other['local_port']}"], m
        log = _log(inst)
        assert "IPv6: probing the server at [::1]:" in log
        assert f"IPv6: the server sees us at [::1]:{st['local_port']}" in log
        assert "(IPv6: IPv4 did not connect)" not in log
    for i in (a, b):
        i.client.mm_cancel()


def test_ipv6_connects_a_pair_ipv4_cannot(backend6: OnlineBackend,
                                          dolphin: Callable[..., DolphinInstance]) -> None:
    """IPv4's connect packets between the two are dropped (PPR_TEST_P2P_BLOCK, two NATs that let
    nothing through): without IPv6 this match fails; with it, both connect over IPv6, after
    IPv4 had its 2 s."""
    ua, ub = backend6.create_user("kai", "KAI"), backend6.create_user("lea", "LEA")
    a = _launch(dolphin, "v6-blocked-a", backend6, ua, env={"PPR_TEST_P2P_BLOCK": ub.connect_code})
    b = _launch(dolphin, "v6-blocked-b", backend6, ub, env={"PPR_TEST_P2P_BLOCK": ua.connect_code})
    sa, sb = _search(a, b, ua, ub)
    for st, other, inst in ((sa, sb, a), (sb, sa, b)):
        m = st["match"]
        assert m["connected_addresses"] == [f"[::1]:{other['local_port']}"], m
        assert m["connect_ms"] >= 2000, m
        assert st["connect_attempts"] == 1
        assert "(IPv6: IPv4 did not connect)" in _log(inst)
    for i in (a, b):
        i.client.mm_cancel()


def test_both_blocked_fails_as_before(backend6: OnlineBackend,
                                      dolphin: Callable[..., DolphinInstance]) -> None:
    """Neither IPv4 nor IPv6 gets through: the connect times out as it always did (and Direct
    searches again), with a line for IPv6 beside IPv4's."""
    ua, ub = backend6.create_user("max", "MAX"), backend6.create_user("nia", "NIA")
    env_a = {"PPR_TEST_P2P_BLOCK": ub.connect_code, "PPR_TEST_P2P_BLOCK6": ub.connect_code}
    env_b = {"PPR_TEST_P2P_BLOCK": ua.connect_code, "PPR_TEST_P2P_BLOCK6": ua.connect_code}
    a = _launch(dolphin, "v6-none-a", backend6, ua, env=env_a)
    b = _launch(dolphin, "v6-none-b", backend6, ub, env=env_b)
    a.client.mm_search_direct(ub.connect_code, session="none")
    time.sleep(2.2)
    b.client.mm_search_direct(ua.connect_code, session="none")
    for inst in (a, b):
        inst.wait_for_log(r"\[Matchmaking\] IPv6: no connection to \[::1\]:\d+ either", timeout=40)
        inst.wait_for_log(r"Connection attempt failed, looking for someone else", timeout=10)
        assert inst.client.mm_status()["state"] != "connection_success"
    for i in (a, b):
        i.client.mm_cancel()


def test_no_ipv6_on_the_server_is_ipv4_as_before(backend4: OnlineBackend,
                                                 dolphin: Callable[..., DolphinInstance]) -> None:
    """mm has no IPv6 listener (as prod until MM_LISTEN6 is set): the probes reach nothing, mm
    sends no ipAddress6, and the connect is IPv4 alone."""
    ua, ub = backend4.create_user("oli", "OLI"), backend4.create_user("pam", "PAM")
    a = _launch(dolphin, "v4only-a", backend4, ua)
    b = _launch(dolphin, "v4only-b", backend4, ub)
    sa, sb = _search(a, b, ua, ub)
    for st, other, inst in ((sa, sb, a), (sb, sa, b)):
        me, peer = _players(st)
        assert me["ip_address6"] == "" and peer["ip_address6"] == ""
        assert st["match"]["remote6_addresses"] == [""]
        assert st["match"]["local_ip6"] == ""
        assert st["match"]["connected_addresses"] == [f"127.0.0.1:{other['local_port']}"]
        log = _log(inst)
        assert "IPv6: the server sees us" not in log
        assert "IPv6: connected to" not in log and "(tried beside IPv4)" not in log
    # mm's own log: no IPv6 listener.
    assert "IPv6 probes on" not in backend4.log_text("mm")
    for i in (a, b):
        i.client.mm_cancel()


def test_ipv6_off_in_one_game(backend6: OnlineBackend,
                              dolphin: Callable[..., DolphinInstance]) -> None:
    """[Online] IPv6 = False (or a build before IPv6): that game sends no probe and no token, so
    mm has no IPv6 address for it, and neither game tries IPv6 with the other."""
    ua, ub = backend6.create_user("quin", "QUIN"), backend6.create_user("rae", "RAE")
    a = _launch(dolphin, "v6off-a", backend6, ua, online={"IPv6": False})
    b = _launch(dolphin, "v6off-b", backend6, ub)
    sa, sb = _search(a, b, ua, ub)
    for st, other in ((sa, sb), (sb, sa)):
        assert st["match"]["remote6_addresses"] == [""]
        assert st["match"]["connected_addresses"] == [f"127.0.0.1:{other['local_port']}"]
    me_a, peer_a = _players(sa)
    assert me_a["ip_address6"] == ""
    assert peer_a["ip_address6"] == f"[::1]:{sb['local_port']}"   # b probed, as ever
    assert "IPv6:" not in _log(a)
    for i in (a, b):
        i.client.mm_cancel()


# --------------------------------------------------------------------------- Other Games rooms
# (Rollback/MachineSession.cpp): two DolphinNoGUI with the Null video backend booting P+ from their
# own card, as tests/test_mods_room.py.

def _mods_room(dolphin: Callable[..., DolphinInstance], be: OnlineBackend, tag: str,
               block: bool) -> tuple[DolphinInstance, DolphinInstance]:
    from test_mods_room import BUILD, _boot, _host
    host_user = be.create_user(f"hsix{tag}", f"HSX{tag}")
    join_user = be.create_user(f"jsix{tag}", f"JSX{tag}")
    host, code = _host(dolphin, be, f"v6-mods-host-{tag}", host_user,
                       block=join_user.connect_code if block else "")
    joiner = _boot(dolphin, be, f"v6-mods-join-{tag}", join_user,
                   {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes"},
                   block=host_user.connect_code if block else "")
    _wait(lambda: "machine: running" in _mods_log(joiner), 120, "the joiner's session running",
          interval=1)
    st = joiner.client.call("mods_status")
    assert not st.get("error"), st
    # mm passed on both players' IPv6 endpoints.
    slots = host.client.call("mods_status")["room"]["slots"]
    assert sum(1 for s in slots if V6_PEER.match(s.get("endpoint6") or "")) == 2, slots
    return host, joiner


def _mods_log(inst: DolphinInstance) -> str:
    return (inst.create() / "Logs" / "dolphin.log").read_text(errors="replace")


def test_other_games_room_keeps_ipv4(backend6: OnlineBackend,
                                     dolphin: Callable[..., DolphinInstance]) -> None:
    """Both have IPv6 and IPv4 connects: the room's link is IPv4, as without IPv6."""
    host, joiner = _mods_room(dolphin, backend6, "A", block=False)
    for inst in (host, joiner):
        log = _mods_log(inst)
        assert "is also tried over IPv6 at [::1]:" in log
        assert re.search(r"peer \d connected \(127\.0\.0\.1:\d+\)", log), log[-3000:]
        assert "connected over IPv6" not in log


def test_other_games_room_over_ipv6_when_ipv4_is_blocked(
        backend6: OnlineBackend, dolphin: Callable[..., DolphinInstance]) -> None:
    """IPv4 between host and joiner is dropped both ways (PPR_TEST_P2P_BLOCK; without IPv6 the
    join fails, tests/test_mods_room.py::test_joiner_says_who_it_cannot_reach): the host (the
    lower slot) keeps its IPv6 connection after IPv4's 2 s and tells the joiner, and the joiner
    joins and plays over IPv6."""
    host, joiner = _mods_room(dolphin, backend6, "B", block=True)
    for inst in (host, joiner):
        log = _mods_log(inst)
        assert re.search(r"peer \d connected over IPv6 \(\[::1\]:\d+; IPv4 127\.0\.0\.1:\d+ did not "
                         r"connect\)", log), log[-3000:]
        assert not re.search(r"peer \d connected \(127\.0\.0\.1", log)


# --------------------------------------------------------------------------- matches
# The gameplay session (Rollback/GameplaySession.cpp, Rollback/UdpSocket6.h) in a real Direct
# match: P+ with our plugin on both, as tests/test_online_game.py.

def _peer(g: Any) -> dict[str, Any]:
    import online_set
    return online_set.gstatus(g.c)["peers"][0]


def _running(games: tuple[Any, Any], frames: int = 300) -> None:
    import online_set
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in games), 120,
                    "the match to run on both")
    games[0].steps(f"wait {frames}")
    assert all(online_set.gstatus(g.c)["phase"] == "running" for g in games)


@pytest.mark.gpu
def test_match_over_ipv6_when_ipv4_cannot_connect(backend6: OnlineBackend,
                                                  dolphin: Callable[..., DolphinInstance],
                                                  gpu_backend: str) -> None:
    """IPv4's connect is blocked both ways (PPR_TEST_P2P_BLOCK=*): matchmaking connects over IPv6,
    and the whole match (character select, stage, game) runs over IPv6."""
    from test_online_game import _connected_direct
    env = {"PPR_TEST_P2P_BLOCK": "*"}
    a, b, _, _ = _connected_direct(backend6, dolphin, gpu_backend, "ipv6-match", ("sid", "tess"),
                                   env=(env, env))
    for g in (a, b):
        m = g.c.mm_status()["match"]
        assert V6_PEER.match(m["connected_addresses"][0]), m
        p = _peer(g)
        assert p["path"] == "ipv6" and p["path_switches"] == 0, p
    _running((a, b))
    for g in (a, b):
        p = _peer(g)
        assert p["path"] == "ipv6", p
        g.c.mm_cancel()


@pytest.mark.gpu
def test_match_moves_to_ipv6_when_ipv4_goes_quiet(backend6: OnlineBackend,
                                                  dolphin: Callable[..., DolphinInstance],
                                                  gpu_backend: str, tmp_path: Any) -> None:
    """A pair on IPv4 (both also have IPv6) loses IPv4 in the middle of the match, both ways
    (PPR_GPRB_TEST_DROP_IPV4_FILE): within about a second both send over IPv6 and the match plays
    on; IPv4 back, both go back to it after PATH_BACK (3 s). Before the cut, IPv4 only."""
    from test_online_game import _connected_direct
    cut = tmp_path / "drop-ipv4"
    env = {"PPR_GPRB_TEST_DROP_IPV4_FILE": str(cut)}
    a, b, _, _ = _connected_direct(backend6, dolphin, gpu_backend, "ipv6-failover", ("uma", "vic"),
                                   env=(env, env))
    _running((a, b), 300)
    for g in (a, b):
        p = _peer(g)
        assert p["path"] == "ipv4" and p["path_switches"] == 0, p
        assert p["address6"].startswith("[::1]:"), p
        assert g.c.mm_status()["match"]["connected_addresses"][0].startswith("127.0.0.1:")
    cut.write_text("x")
    t0 = time.monotonic()
    _wait(lambda: all(_peer(g)["path"] == "ipv6" for g in (a, b)), 10, "both on IPv6")
    print(f"on IPv6 after {time.monotonic() - t0:.1f} s")
    assert time.monotonic() - t0 < 4
    _running((a, b), 600)   # ten seconds of match over IPv6
    for g in (a, b):
        log = _log(g.inst)
        assert "went quiet; its packets go over IPv6" in log
        assert "peer timed out" not in log and "nothing from P" not in log
    cut.unlink()
    _wait(lambda: all(_peer(g)["path"] == "ipv4" for g in (a, b)), 15, "both back on IPv4")
    _running((a, b), 300)
    for g in (a, b):
        assert _peer(g)["path_switches"] == 2, _peer(g)
        assert "works both ways again; its packets go over IPv4" in _log(g.inst)
        g.c.mm_cancel()
