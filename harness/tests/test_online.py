"""Online play end to end: user.json login, Direct matchmaking against our own server, the hand-off
to a rollback session, and Slippi's disconnect timeout.

Needs the real Dolphin build (``dolphin``) and the server binaries plus a Postgres (``server``,
see ``ppharness/backend.py``): ``cargo build --workspace`` in ``server/``, then
``pytest -m "dolphin and server" tests/test_online.py``.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Callable, Iterator

import pytest

from ppharness.backend import BackendError, OnlineBackend, OnlineUser, server_binary
from ppharness.instance import DolphinInstance, InstanceConfig
from ppharness.netsim import NetSim

pytestmark = [pytest.mark.dolphin, pytest.mark.server]


def _wait(pred: Callable[[], Any], timeout: float, what: str, interval: float = 0.2) -> Any:
    deadline = time.monotonic() + timeout
    while True:
        v = pred()
        if v:
            return v
        if time.monotonic() > deadline:
            raise AssertionError(f"timed out after {timeout:.0f}s waiting for {what}")
        time.sleep(interval)


def _make_backend(**kw: Any) -> OnlineBackend:
    for b in ("accounts", "mm", "admin"):
        if not server_binary(b).exists():
            pytest.skip(f"{server_binary(b)} not built (cargo build --workspace in server/)")
    try:
        return OnlineBackend(**kw).start()
    except BackendError as e:
        pytest.skip(f"no local backend: {e}")


@pytest.fixture(scope="module")
def backend() -> Iterator[OnlineBackend]:
    be = _make_backend()
    try:
        yield be
    finally:
        be.stop()


def _online_config(be: OnlineBackend, mm_port: int | None = None, **kw: Any) -> InstanceConfig:
    return InstanceConfig(dolphin_ini={"Online": {
        "UseDevServer": True,
        "MatchmakingPort": mm_port or be.mm_port,
        "DevAccountsUrl": be.accounts_url,
    }}, **kw)


def _launch(dolphin: Callable[..., DolphinInstance], name: str, cfg: InstanceConfig,
            user: OnlineUser | None) -> DolphinInstance:
    inst = dolphin(name, config=cfg, boot=None)
    inst.create()
    if user is not None:
        user.write_user_json(inst.user_dir)
    inst.launch()
    inst.connect()
    return inst


def _logged_in(inst: DolphinInstance, user: OnlineUser) -> dict[str, Any]:
    st = _wait(lambda: (lambda s: s if s["logged_in"] and s["user_fetch"] == "fetched" else None)(
        inst.client.online_status()), 20, "the login")
    assert st["uid"] == user.uid
    assert st["connect_code"] == user.connect_code
    assert st["display_name"] == user.display_name
    assert st["app_state_name"] == "logged_in"
    assert st["has_play_key"] and "playKey" not in st and "play_key" not in st
    assert Path(st["user_json_path"]) == Path(inst.user_dir) / "Online" / "user.json"
    return st


def _search_both(a: DolphinInstance, b: DolphinInstance, ua: OnlineUser, ub: OnlineUser,
                 **kw: Any) -> list[dict[str, Any]]:
    a.client.mm_search_direct(ub.connect_code, **kw)
    b.client.mm_search_direct(ua.connect_code, **kw)

    def done() -> list[dict[str, Any]] | None:
        st = [i.client.mm_status() for i in (a, b)]
        if any(s["state"] == "error" for s in st):
            raise AssertionError(f"matchmaking error: {[(s['error'], s['error_source']) for s in st]}")
        return st if all(s["state"] == "connection_success" for s in st) else None

    return _wait(done, 60, "both instances to match and connect")


def _check_match(sa: dict[str, Any], sb: dict[str, Any], ua: OnlineUser, ub: OnlineUser) -> None:
    ma, mb = sa["match"], sb["match"]
    assert ma["match_id"] == mb["match_id"] and ma["match_id"].startswith("mode.direct-")
    # Exactly one decider, and it is port 1 on both sides.
    assert ma["is_host"] != mb["is_host"]
    assert [p["uid"] for p in ma["players"]] == [p["uid"] for p in mb["players"]]
    host_uid = ua.uid if ma["is_host"] else ub.uid
    assert ma["players"][0]["uid"] == host_uid and ma["players"][0]["port"] == 1
    for st, me, other in ((sa, ua, ub), (sb, ub, ua)):
        m = st["match"]
        local = [p for p in m["players"] if p["is_local"]]
        remote = [p for p in m["players"] if not p["is_local"]]
        assert len(local) == 1 and len(remote) == 1
        assert (local[0]["uid"], local[0]["connect_code"], local[0]["display_name"]) == \
            (me.uid, me.connect_code, me.display_name)
        assert (remote[0]["uid"], remote[0]["connect_code"], remote[0]["display_name"]) == \
            (other.uid, other.connect_code, other.display_name)
        assert m["local_player_index"] == local[0]["port"] - 1
        assert m["local_port"] == st["local_port"]
        assert st["lan_address"].endswith(f":{st['local_port']}")
        assert st["tickets"] == 1 and st["connect_attempts"] == 1
    # Peer endpoints: each side reached the other's punched port (both share 127.0.0.1 as their
    # external address, so Slippi's rule picks the LAN address).
    for st, other in ((sa, sb), (sb, sa)):
        remote = [p for p in st["match"]["players"] if not p["is_local"]][0]
        assert st["match"]["remote_addresses"] == [remote["ip_address_lan"]]
        assert st["match"]["remote_addresses"] == [other["lan_address"]]
        assert st["match"]["connected_addresses"] == [f"127.0.0.1:{other['local_port']}"]


def test_online_status_logged_out_then_login(backend: OnlineBackend,
                                             dolphin: Callable[..., DolphinInstance]) -> None:
    """No user.json: logged out. Writing it (as the launcher does on Play) logs in within the
    500 ms watch interval, without a restart."""
    user = backend.create_user("carol", "CARO")
    inst = _launch(dolphin, "online-login", _online_config(backend), None)
    st = inst.client.online_status()
    assert not st["logged_in"] and st["app_state_name"] == "logged_out" and st["watching"]
    user.write_user_json(inst.user_dir)
    _logged_in(inst, user)
    assert not inst.client.online_status()["watching"]  # Slippi stops watching once logged in


def test_direct_match(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance]) -> None:
    """Two instances search Direct for each other's code: both match, with one host, the right
    peer info and a working P2P connection on the punched ports."""
    ua, ub = backend.create_user("alice", "ALIC"), backend.create_user("bob", "BOB")
    cfg = _online_config(backend)
    a = _launch(dolphin, "online-a", cfg, ua)
    b = _launch(dolphin, "online-b", cfg, ub)
    _logged_in(a, ua)
    _logged_in(b, ub)
    sa, sb = _search_both(a, b, ua, ub, session="none")
    _check_match(sa, sb, ua, ub)
    assert sa["handoff"] == sb["handoff"] == "kept"
    for i in (a, b):
        st = i.client.mm_cancel()
        assert st["state"] == "idle" and not st["searching"]


def test_direct_match_through_netsim(backend: OnlineBackend,
                                     dolphin: Callable[..., DolphinInstance]) -> None:
    """The same with each client's matchmaking traffic going through a netsim proxy (`typical`:
    40 ms RTT, jitter, 0.5 % loss). The P2P connection does not: the server hands each client the
    other's real address (see docs/harness-protocol.md, Online)."""
    ua, ub = backend.create_user("dave", "DAVE"), backend.create_user("erin", "ERIN")
    sims = [NetSim(("127.0.0.1", backend.mm_port), ("127.0.0.1", 0), "typical", seed=s).start()
            for s in (1, 2)]
    try:
        a = _launch(dolphin, "online-ns-a", _online_config(backend, sims[0].listen_port), ua)
        b = _launch(dolphin, "online-ns-b", _online_config(backend, sims[1].listen_port), ub)
        _logged_in(a, ua)
        _logged_in(b, ub)
        sa, sb = _search_both(a, b, ua, ub, session="none")
        _check_match(sa, sb, ua, ub)
        for st, sim in ((sa, sims[0]), (sb, sims[1])):
            local = [p for p in st["match"]["players"] if p["is_local"]][0]
            # The server saw the proxy's upstream socket, not the client's own port.
            assert local["ip_address"] != local["ip_address_lan"]
            stats = sim.stats()
            assert stats["up"]["packets_in"] > 0 and stats["down"]["packets_in"] > 0, stats
            assert st["server"] == f"127.0.0.1:{sim.listen_port}"
        for i in (a, b):
            i.client.mm_cancel()
    finally:
        for s in sims:
            s.stop()


def _game_running(inst: DolphinInstance) -> bool:
    ns = inst.client.netplay_status()
    return bool(ns.game_running and ns.rollback and ns.rollback.get("session_started"))


def _freeze(pid: int, suspend: bool) -> None:
    if sys.platform == "win32":
        import ctypes
        ntdll, k32 = ctypes.WinDLL("ntdll"), ctypes.WinDLL("kernel32")
        h = k32.OpenProcess(0x0800, False, pid)  # PROCESS_SUSPEND_RESUME
        try:
            (ntdll.NtSuspendProcess if suspend else ntdll.NtResumeProcess)(h)
        finally:
            k32.CloseHandle(h)
    else:
        import os
        import signal
        os.kill(pid, signal.SIGSTOP if suspend else signal.SIGCONT)


@pytest.mark.slow
def test_direct_match_hands_off_to_rollback_session(
        backend: OnlineBackend, dolphin: Callable[..., DolphinInstance]) -> None:
    """After the match the connected peer is handed to the rollback session (whole-machine
    netplay for now): the decider hosts on its punched port, the other joins from its punched
    port, the game boots on both under GekkoNet. Then Slippi's disconnect rule: a peer silent for
    6 s stays in (GekkoNet's old 5 s would have dropped it); one silent for longer is dropped
    after ~7.2 s (Online::PeerSilenceTimeoutMs) and the game ends on both sides."""
    ua, ub = backend.create_user("frank", "FRAN"), backend.create_user("gina", "GINA")
    cfg = _online_config(backend)
    a = _launch(dolphin, "online-ho-a", cfg, ua)
    b = _launch(dolphin, "online-ho-b", cfg, ub)
    _logged_in(a, ua)
    _logged_in(b, ub)
    sa, sb = _search_both(a, b, ua, ub, session="auto")
    _check_match(sa, sb, ua, ub)
    host, guest = (a, b) if sa["match"]["is_host"] else (b, a)
    hs, gs = (sa, sb) if host is a else (sb, sa)

    _wait(lambda: _game_running(host) and _game_running(guest), 120,
          "the game to start under rollback on both")
    for inst, st, role in ((host, hs, "host"), (guest, gs, "guest")):
        sess = inst.client.mm_status()["session"]
        assert sess["backend"] == "netplay" and sess["phase"] == "running", sess
        assert sess["detail"]["role"] == role
        assert sess["detail"]["local_port"] == st["local_port"]
        assert inst.client.mm_status()["handoff"] == "started"
    assert host.client.netplay_status().role == "host"
    assert guest.client.netplay_status().role == "client"
    assert len(host.client.netplay_status().players) == 2
    # The guest's netplay client connected from its punched port to the host's.
    assert guest.client.mm_status()["session"]["detail"]["peer"] == f"127.0.0.1:{hs['local_port']}"
    host.wait_for_log(r"GekkoNet: disconnect timeout 7191 ms", timeout=10)

    # Let the match run a little, then stall the guest for 6 s: no disconnect.
    f0 = host.client.netplay_status().rollback["current_frame"]
    _wait(lambda: host.client.netplay_status().rollback["current_frame"] > f0 + 120, 30, "frames")
    pid = guest.process.pid
    _freeze(pid, True)
    try:
        time.sleep(6.0)
        assert host.client.netplay_status().rollback["peer_disconnects"] == 0
    finally:
        _freeze(pid, False)
    f1 = host.client.netplay_status().rollback["current_frame"]
    _wait(lambda: host.client.netplay_status().rollback["current_frame"] > f1 + 120, 30,
          "the match to go on after a 6 s stall")
    assert host.client.netplay_status().rollback["peer_disconnects"] == 0

    # Now a peer that stays silent: dropped after Slippi's ~7.2 s, and the game ends.
    _freeze(pid, True)
    t0 = time.monotonic()
    dropped_at = None
    try:
        while time.monotonic() - t0 < 12:
            ns = host.client.netplay_status()
            if ns.rollback and ns.rollback["peer_disconnects"] > 0:
                dropped_at = time.monotonic() - t0
                break
            time.sleep(0.05)
    finally:
        _freeze(pid, False)
    assert dropped_at is not None, "the silent peer was never dropped"
    print(f"silent peer dropped after {dropped_at:.2f} s")
    assert 6.9 <= dropped_at <= 8.5, f"dropped after {dropped_at:.2f}s, expected ~7.2s"
    host.wait_for_log(r"GekkoNet: Player \d+ disconnected", timeout=5)
    _wait(lambda: not host.client.status().running and not guest.client.status().running, 30,
          "the game to stop on both sides")
    for i in (host, guest):
        i.client.mm_cancel()


# --------------------------------------------------------------------------- errors


def _search_error(inst: DolphinInstance, timeout: float = 30, **search: Any) -> dict[str, Any]:
    if "mode" in search:
        inst.client.mm_search(**search)
    else:
        inst.client.mm_search_direct(**search)
    st = _wait(lambda: (lambda s: s if s["state"] == "error" else None)(inst.client.mm_status()),
               timeout, "the matchmaking error")
    assert st["state_code"] == 5
    inst.client.mm_cancel()
    return st


def test_search_errors(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance]) -> None:
    """Server refusals reach the client as ERROR_ENCOUNTERED with the server's text."""
    me, other = backend.create_user("hank", "HANK"), backend.create_user("ivy", "IVY")
    inst = _launch(dolphin, "online-err", _online_config(backend), me)
    _logged_in(inst, me)

    st = _search_error(inst, mode="unranked")
    assert st["error"] == "Unranked is not supported yet. Only Direct works for now."
    assert st["error_source"] == "create_ticket"

    st = _search_error(inst, code=me.connect_code)
    assert st["error"] == "That is your own connect code. Enter your opponent's code."

    time.sleep(2.2)  # the server takes one ticket per account per 2 s
    # A rotated play key (password change, ban, admin): the file's key is no longer valid.
    backend.admin("user", "rotate-play-key", me.email)
    st = _search_error(inst, code=other.connect_code)
    assert st["error"] == "Invalid play key. Log in again in the launcher."
    assert st["error_source"] == "create_ticket"


def test_search_expires(dolphin: Callable[..., DolphinInstance]) -> None:
    """A Direct search the opponent never answers ends with the server's expiry error
    (get-ticket-resp), not silence. Server TTL shortened to 3 s for the test."""
    be = _make_backend(ticket_ttl_secs=3)
    try:
        me, other = be.create_user("jack", "JACK"), be.create_user("kim", "KIM")
        inst = _launch(dolphin, "online-exp", _online_config(be), me)
        _logged_in(inst, me)
        st = _search_error(inst, timeout=30, code=other.connect_code)
        assert st["error"].startswith(f"Search timed out: {other.connect_code} did not connect")
        assert st["error_source"] == "get_ticket"
    finally:
        be.stop()


def test_mm_server_unreachable(backend: OnlineBackend,
                               dolphin: Callable[..., DolphinInstance]) -> None:
    """Nobody on the configured port: Slippi's 20 x 500 ms connect, then its message."""
    from ppharness.instance import find_free_port
    import socket
    me = backend.create_user("lou", "LOU")
    dead_port = find_free_port(kind=socket.SOCK_DGRAM)
    inst = _launch(dolphin, "online-dead", _online_config(backend, dead_port), me)
    _logged_in(inst, me)
    t0 = time.monotonic()
    st = _search_error(inst, timeout=30, code="ZZ#1")
    assert st["error"] == "Failed to connect to mm server" and st["error_source"] == "client"
    assert 9 <= time.monotonic() - t0 <= 20
