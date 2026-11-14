"""Other Games (Brawl Mods) rooms: a joiner's wait for the room's players (Online/ModsRoom.cpp,
Rollback/MachineSession.cpp JoinStep; docs/design/brawl-mods.md §6.7).

Two DolphinNoGUI against our local accounts + mm servers, armed from PPR_MODS_ROOM as the
launcher arms them: a host opens a room for a made-up build, a joiner joins it. They boot P+ from
the instance's own card (the same on both, so there is nothing of the card to download). No GPU
is needed: the Null video backend.
"""

from __future__ import annotations

import json
import os
import re
import selectors
import shutil
import socket
import subprocess
import threading
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import paths
from ppharness.backend import OnlineBackend, OnlineUser
from ppharness.fake_igd import FakeIGD
from ppharness.instance import DolphinInstance, InstanceConfig

from test_online import _lan_address, _logged_in, _make_backend, _wait
from test_rooms import backend  # noqa: F401  (the backend fixture)

pytestmark = [pytest.mark.dolphin, pytest.mark.server]

BUILD = {"hash": "ab" * 32, "name": "Test build"}


def _boot(dolphin: Callable[..., DolphinInstance], be: OnlineBackend, name: str, user: OnlineUser,
          room: dict[str, Any], block: str = "", config_args: list[str] | None = None,
          mm_port: int | None = None, env: dict[str, str] | None = None,
          prepare: Callable[[Path], None] | None = None, **kw: Any) -> DolphinInstance:
    if be.mm_host.startswith("127."):
        ini = {"Online": {"UseDevServer": True, "MatchmakingPort": mm_port or be.mm_port,
                          "DevAccountsUrl": be.accounts_url}}
    else:
        ini = {"Online": {"MatchmakingHost": be.mm_host, "MatchmakingPort": be.mm_port,
                          "AccountsUrl": be.accounts_url}}
    env = {"PPR_MODS_ROOM": json.dumps(room), **({"PPR_TEST_P2P_BLOCK": block} if block else {}), **(env or {})}
    inst = dolphin(name, config=InstanceConfig(cpu_thread=True, video_backend="Null", dolphin_ini=ini,
                                               config_args=config_args or []),
                   client_timeout=60.0, env=env, **kw)
    user_dir = inst.create()
    user.write_user_json(user_dir)
    if prepare:
        prepare(user_dir)
    inst.launch()
    inst.connect()
    _logged_in(inst, user)
    return inst


def _host(dolphin: Callable[..., DolphinInstance], be: OnlineBackend, name: str, user: OnlineUser,
          block: str = "", env: dict[str, str] | None = None) -> tuple[DolphinInstance, str]:
    host = _boot(dolphin, be, name, user, {"host": True, "build": BUILD, "public": False, "controller": "gc"},
                 block, env=env)
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
            osd = joiner.client.call("osd_messages", timeout=5).get("messages", [])
        except Exception:  # noqa: BLE001  (Dolphin closes after a join that failed)
            break
        for ln in lines:
            seen.setdefault(f"chat: {ln.get('text', '')}", time.monotonic() - t0)
        for text in osd:
            seen.setdefault(f"osd: {text}", time.monotonic() - t0)
        if st.get("error"):
            seen.setdefault(f"error: {st['error']}", time.monotonic() - t0)
        time.sleep(0.5)
    print({k: round(v, 1) for k, v in seen.items()})
    then = ("The SD card download starts once connected." if sd_sync == "yes" else "Joining waits for it.")
    notice = f"chat: Can't connect to hosty yet. {then}"
    assert notice in seen, seen
    assert 8 <= seen[notice] <= 20, seen
    # On screen too, whatever the chat window shows (bug report #175: folded, it showed nothing),
    # with the seconds left, and not twice (the notice itself stays in the chat).
    countdown = {k: v for k, v in seen.items() if k.startswith("osd: Can't connect to hosty yet. Joining waits for it: ")}
    assert len(countdown) >= 20, seen
    assert 8 <= min(countdown.values()) <= 20, seen
    assert "osd: Can't connect to hosty yet. Joining waits for it: 50 s left." in countdown, seen
    assert any(f"for it: {n} s left." in k for k in countdown for n in range(1, 10)), seen
    assert f"osd: Can't connect to hosty yet. {then}" not in seen, seen
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


def test_joiners_stop_when_nobody_in_the_room_plays(backend: OnlineBackend,
                                                   dolphin: Callable[..., DolphinInstance]) -> None:
    """Bug report #203: DRAC#759 was joining DAN#707's REX room when DAN#707 closed their game;
    mm made DRAC#759 the room's host while their game still waited for its first frame, and
    DAN#707 joined the room again and asked a game that was not running to let them in. Both
    stood still until they closed them. Here two joiners wait for a host they cannot reach
    (PPR_TEST_P2P_BLOCK), the second linked to the first (a joiner, by its hello), and the host
    closes: both say so and leave at once, long before the no-link limit (60 s)."""
    host_user = backend.create_user("hostg", "HSTG")
    one_user = backend.create_user("joing", "JNYG")
    two_user = backend.create_user("joinh", "JNYH")
    host, code = _host(dolphin, backend, "mods-gone-host", host_user,
                       block=f"{one_user.connect_code},{two_user.connect_code}")
    # Its session running first: a host stalled while booting (a busy test PC) can lose mm and
    # open a new room under another code.
    _wait(lambda: "machine: running" in _log(host), 90, "the host's session running", interval=1)
    code = host.client.call("mods_status")["room"]["code"]
    joiners = [_boot(dolphin, backend, f"mods-gone-join{i}", u,
                     {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes"},
                     block=host_user.connect_code)
               for i, u in enumerate((one_user, two_user), 1)]
    _wait(lambda: sum(1 for s in host.client.call("mods_status")["room"]["slots"] if s.get("code")) == 3, 30,
          "both joiners in the room")
    _wait(lambda: all(re.search(r"machine: peer \d connected", _log(j)) for j in joiners), 30,
          "the joiners' link to each other")
    for j in joiners:
        assert not j.client.call("mods_status").get("error")
    host.stop()
    t0 = time.monotonic()
    errors = []
    for j in joiners:
        def gone() -> str | None:
            try:
                return j.client.call("mods_status", timeout=5).get("error")
            except Exception:  # noqa: BLE001  (Dolphin closes after a join that failed)
                return "closed"
        errors.append(_wait(gone, 30, "the joiner to give up"))
    took = time.monotonic() - t0
    print(errors, round(took, 1))
    # What the launcher shows (mods-room.json), or Dolphin had closed already.
    assert all(e in ("closed", "Could not join the game: nobody in the room plays the game any more (its "
                               "players left), so nobody can let you in") for e in errors), errors
    assert took < 15, took
    for j in joiners:
        log = _log(j)
        assert ("machine: nobody in the room plays the game any more (its players left), so nobody can let "
                "you in") in log, log[-3000:]
        assert "Not admitted" not in log and "no connection to" not in log


def test_a_join_while_the_players_games_differ_says_why(backend: OnlineBackend,
                                                         dolphin: Callable[..., DolphinInstance]) -> None:
    """Bug report #214: in a TvC room whose host and one player had desynced, every joiner got the
    25 MB machine image and then refused it ("player 2 does not vouch for this machine"); the
    players saw "could not join" and nothing more, until that player left. Here the first joiner's
    game is made to differ from the host's (a word of free RAM written on it only), and a second
    joiner is refused at the epoch with why, before any image is sent."""
    host_user = backend.create_user("hostd", "HSTD")
    one_user = backend.create_user("joind", "JNYD")
    two_user = backend.create_user("joine", "JNYE")
    host, code = _host(dolphin, backend, "mods-differ-host", host_user)
    one = _boot(dolphin, backend, "mods-differ-join1", one_user,
                {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes"})
    _wait(lambda: "machine: running" in _log(one), 90, "the first joiner's session running", interval=1)
    assert "machine image for player 2" in _log(host)
    # Free RAM above Dolphin's code list (test_host_codes_run_on_every_machine), on one machine only.
    one.client.call("write_mem", addr=0x80002F10, hex="0badf00d")
    _wait(lambda: host.client.call("machine_status").get("desyncs"), 30, "the host to see the desync")
    two = _boot(dolphin, backend, "mods-differ-join2", two_user,
                {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes"})
    _wait(lambda: "cannot join: the game of" in _log(host), 120, "the host to refuse the second joiner",
          interval=1)
    host_log = _log(host)
    assert "machine image for player 3" not in host_log, host_log[-3000:]
    assert re.search(r"notice: joine could not join: joind's game differs from .+'s \(a desync\)\. joind can "
                     r"leave the room and join it again to get .+'s game\.", host_log), host_log[-3000:]
    _wait(lambda: "Could not join: the players' games differ (a desync between" in _log(two), 30,
          "the second joiner's reason")
    assert "Getting the game" not in _log(two)
    # The first joiner was told too, and the two players play on.
    _wait(lambda: "could not join: " in _log(one), 30, "the first joiner's notice")
    for inst in (host, one):
        _wait(lambda: inst.client.call("machine_status").get("phase") == "running", 30, "the game going on")
        assert inst.client.call("machine_status").get("players") == 3
    # One line for the epoch's checksum, not one per wait loop iteration; and the desynced frames
    # are not a line each (60 between two notices).
    assert len(re.findall(r"machine: player 2 is at epoch \d+ with checksum", host_log)) == 1, host_log[-3000:]
    assert len(re.findall(r"machine: desync at frame", host_log)) <= 60 * (host_log.count("notice: Desync with") + 1)


def _watch(inst: DolphinInstance, seen: dict[str, float], t0: float) -> None:
    """What `inst` shows now, on screen and in its chat window, kept with when it first showed."""
    try:
        osd = inst.client.call("osd_messages", timeout=5).get("messages", [])
        lines = inst.client.call("chat_status", timeout=5).get("lines", [])
    except Exception:  # noqa: BLE001  (a Dolphin that closed)
        return
    for text in osd:
        seen.setdefault(f"osd: {text}", time.monotonic() - t0)
    for ln in lines:
        seen.setdefault(f"chat: {ln.get('text', '')}", time.monotonic() - t0)


@pytest.mark.parametrize("how", ["quits", "crashes"])
def test_a_join_goes_on_when_the_player_sending_the_game_leaves(backend: OnlineBackend,
                                                               dolphin: Callable[..., DolphinInstance],
                                                               how: str) -> None:
    """Sarah's board, 2026-11-14: NEST#120 left their REX room 11 s into sending SARA#694 the game
    (37.5 MB); SARA#694's download stood at 20% and KENO#349's game stood still, waiting for
    NEST#120 to say who plays, until both gave up (they would have waited 300 s and 120 s). Here a
    host and a player play, the host sends a second joiner the game slowly
    (PPR_MACHINE_IMAGE_KBPS), and leaves partway: by quitting, or by its process dying (its links
    then time out). The player decides the join in its place, sends the image again, and both
    play on. While the game stands still the players see "Sending <joiner> the game state... N%"
    at the top left (Sarah's board, 2026-11-14: "not in the chat, but in a toast"), and the chat
    window has no such lines."""
    tag = "Q" if how == "quits" else "K"
    host_user = backend.create_user("hostt" + tag.lower(), "HT" + tag)
    one_user = backend.create_user("onet" + tag.lower(), "OT" + tag)
    two_user = backend.create_user("twot" + tag.lower(), "WT" + tag)
    hname, two = host_user.display_name, two_user.display_name
    host, code = _host(dolphin, backend, f"mods-takeover-host-{how}", host_user,
                       env={"PPR_MACHINE_IMAGE_KBPS": "1500"})
    one = _boot(dolphin, backend, f"mods-takeover-one-{how}", one_user,
                {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes"})
    _wait(lambda: "machine: running" in _log(one), 180, "the first joiner's session running", interval=1)
    joiner = _boot(dolphin, backend, f"mods-takeover-two-{how}", two_user,
                   {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes"})
    t0 = time.monotonic()
    seen: dict[str, float] = {}
    sending = re.compile(r"notice: Getting the game: [2-9]0%")

    def under_way() -> bool:
        for inst in (host, one):
            _watch(inst, seen, t0)
        return bool(sending.search(_log(joiner)))
    _wait(under_way, 120, "the image on its way to the second joiner", interval=0.3)
    before = dict(seen)
    assert "machine image for player 3" in _log(host)
    assert "machine image for player 3" not in _log(one)
    if how == "quits":
        host.stop()
    else:
        host.kill()
    left = time.monotonic() - t0

    def joined() -> bool:
        _watch(one, seen, t0)
        log = _log(joiner)
        return "machine: running" in log[log.find("machine image from player 1"):]
    _wait(joined, 150, "the second joiner in the game", interval=0.3)
    took = time.monotonic() - t0 - left
    print(f"{how}: in the game {took:.1f} s after the host left; {[k for k in seen if 'game state' in k]}")
    one_log, two_log = _log(one), _log(joiner)
    assert re.search(r"epoch \d+: player 1 left before it said who plays; player 2 decides it now", one_log), \
        one_log[-4000:]
    assert "machine image for player 3" in one_log, one_log[-4000:]
    assert re.search(r"machine image from player 1 (stopped|dropped) at [\d.]+ of [\d.]+ MB", two_log), two_log[-4000:]
    assert "machine image from player 2 (" in two_log, two_log[-4000:]
    # Both play on, the second joiner in the game.
    for inst in (one, joiner):
        _wait(lambda: inst.client.call("machine_status").get("phase") == "running", 30, "the game going on")
        assert inst.client.call("machine_status").get("players") == 0b110
    assert not joiner.client.call("mods_status").get("error")
    # The top left said why the game stood still, with the joiner's progress: the host's sending,
    # then the player's own.
    assert f"osd: {hname} is sending {two} the game state..." in before, before
    assert any(re.fullmatch(rf"osd: {hname} is sending {two} the game state\.\.\. [1-9]0%", k) for k in before), before
    assert any(re.fullmatch(rf"osd: Sending {two} the game state\.\.\.( [1-9]0%| 100%)?", k) for k in seen), seen
    assert not any(k.startswith("chat:") and ("getting the game" in k or "Sending the game" in k
                                             or "The game waits" in k) for k in seen), seen
    assert f"chat: {two} is in the game." in seen, seen
    assert "Not admitted" not in two_log and "did not agree" not in one_log


def _flip_card_byte(user_dir: Path) -> None:
    """The instance's card differs from the room's in one byte, in the middle of its used part (a
    chunk the session's list holds)."""
    card = user_dir / "Wii" / "sd.raw"
    with card.open("r+b") as f:
        f.seek(200 << 20)
        b = f.read(1)
        f.seek(200 << 20)
        f.write(bytes([b[0] ^ 0x5A]))


@pytest.mark.parametrize("listing", ["room", "other"])
def test_a_joiner_takes_the_room_cards_list_from_the_launcher(backend: OnlineBackend,
                                                             dolphin: Callable[..., DolphinInstance],
                                                             tmp_path: Path, listing: str) -> None:
    """Sarah's board, 2026-11-14: joining a REX room, SARA#694's game waited 70 s for the players to
    send the 2.3 MB list of the room's SD card (34 KB/s, the pace of an upload beside two players'
    game), right after the launcher had fetched the same list from the accounts server for its
    question. The launcher now hands Dolphin that listing (`sd_listing`). Here a joiner whose card
    differs in one byte gets the room card's listing (DolphinTool sdhash --listing of the host's
    card), downloads only the difference and asks the players for no list. A listing of another
    card (the joiner's own) is not taken: the players' list comes as before."""
    tool = paths.binaries_dir() / ("DolphinTool.exe" if os.name == "nt" else "dolphin-tool")
    if not tool.exists():
        pytest.skip(f"no {tool}")
    card = paths.template_user_dir() / "Wii" / "sd.raw"
    if listing == "other":
        (tmp_path / "other" / "Wii").mkdir(parents=True)
        shutil.copyfile(card, tmp_path / "other" / "Wii" / "sd.raw")
        _flip_card_byte(tmp_path / "other")
        card = tmp_path / "other" / "Wii" / "sd.raw"
    listing_file = tmp_path / "room-sd-listing.bin"
    cache = tmp_path / "tool-user"
    cache.mkdir()
    out = subprocess.run([str(tool), "sdhash", "-i", str(card), "-u", str(cache), "-l", str(listing_file)],
                         capture_output=True, text=True, timeout=600)
    assert out.returncode == 0, out.stderr
    tag = "R" if listing == "room" else "O"
    host_user = backend.create_user("hostl" + tag.lower(), "HL" + tag)
    join_user = backend.create_user("joinl" + tag.lower(), "JL" + tag)
    host, code = _host(dolphin, backend, f"mods-listing-host-{listing}", host_user)
    joiner = _boot(dolphin, backend, f"mods-listing-join-{listing}", join_user,
                   {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes",
                    "sd_listing": str(listing_file)}, prepare=_flip_card_byte)
    _wait(lambda: "machine: running" in _log(joiner), 240, "the joiner's session running", interval=1)
    log, host_log = _log(joiner), _log(host)
    assert re.search(r"SD card: got [\d.]+ MB of the host's card", log), log[-4000:]
    if listing == "room":
        assert re.search(r"SD card: the room card's list from the launcher \([\d.]+ MB, \d+ files\): no list "
                         r"asked of the players", log), log[-4000:]
        assert "Getting the host's SD card list" not in log and "got the host's list" not in log, log[-4000:]
        assert "the card's list" not in host_log, host_log[-3000:]
    else:
        assert "SD card: the launcher's list is of another card: asking the players" in log, log[-4000:]
        assert "SD card: got the host's list" in log, log[-4000:]


def test_room_sessions_map_their_port_with_upnp(dolphin: Callable[..., DolphinInstance]) -> None:
    """Bug report #183: TEST#709 and AIR#23 played five Unranked games, then never connected in
    AIR's Other Games room (no packet either way), nor did OHM#217, who plays AIR in P+ rooms.
    Unranked asks the router over UPnP to forward its port before it talks to mm
    (test_online.py::test_direct_match_maps_the_port_with_upnp), so a router that gives every
    destination its own port uses that one toward the peers too; a room's session made no
    mapping. Now each session maps its socket's port before its probe to mm, mm sees that port,
    the join still works, and leaving removes the mapping. mm on the LAN address: with mm on
    127.0.0.1 (the other tests here) no mapping is made. The fake IGD takes leases, so each port
    is mapped once with the 2 h lease."""
    lan = _lan_address()
    if lan.startswith("127."):
        pytest.skip("no LAN address")
    try:
        igd = FakeIGD(lan).start()
    except OSError as e:
        pytest.skip(f"cannot listen for SSDP on {lan}: {e}")
    be = _make_backend(mm_host=lan)
    try:
        host_user = be.create_user("hostu", "HSTU")
        join_user = be.create_user("joinu", "JNYU")
        host, code = _host(dolphin, be, "mods-upnp-host", host_user)
        joiner = _boot(dolphin, be, "mods-upnp-join", join_user,
                       {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes"})
        _wait(lambda: "machine: running" in _log(joiner), 90, "the joiner's session running", interval=1)
        ports: dict[DolphinInstance, str] = {}
        for inst in (host, joiner):
            log = _log(inst)
            seen = re.search(rf"room: the server sees this player at {re.escape(lan)}:(\d+) "
                             rf"\(UDP port (\d+), UPnP-mapped\)", log)
            assert seen and seen.group(1) == seen.group(2), log[-4000:]
            port = seen.group(2)
            ports[inst] = port
            mapped = log.find(f"UPnP: mapped UDP port {port} to {lan}")
            probe = log.find("machine: endpoint probe answered")
            assert 0 <= mapped < probe, (mapped, probe)
        # Other programs on this machine may find the fake router too: only these UDP ports count.
        adds = [c for c in igd.mapping_calls() if c.action == "AddPortMapping" and
                c.args.get("NewProtocol") == "UDP" and c.args.get("NewExternalPort") in ports.values()]
        assert sorted((c.args.get("NewExternalPort"), c.args.get("NewInternalPort"),
                       c.args.get("NewInternalClient"), c.args.get("NewLeaseDuration"), c.error)
                      for c in adds) == sorted((p, p, lan, "7200", 0) for p in ports.values()), adds
        for inst in (joiner, host):
            inst.stop()
            assert f"UPnP: removed the mapping of UDP port {ports[inst]}" in _log(inst)
        deletes = [c for c in igd.mapping_calls() if c.action == "DeletePortMapping" and
                   c.args.get("NewExternalPort") in ports.values()]
        assert sorted(c.args.get("NewExternalPort") for c in deletes) == sorted(ports.values()), deletes
    finally:
        be.stop()
        igd.stop()


class _ForgetfulNat:
    """A UDP NAT between one player's Dolphin and mm (127.0.0.1): each of the player's sockets gets
    a "public" socket of its own toward mm, forgotten once nothing went out on it for `idle`
    seconds, as a router or carrier-grade NAT forgets a mapping; the socket's next datagram gets a
    new one, with a new port. Only what the player sends keeps a mapping (mm's answers do not)."""

    def __init__(self, mm_port: int, idle: float) -> None:
        self.mm = ("127.0.0.1", mm_port)
        self.idle = idle
        self.inner = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.inner.bind(("127.0.0.1", 0))
        self.port = self.inner.getsockname()[1]
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.inner, selectors.EVENT_READ, None)
        self.maps: dict[tuple[str, int], list[Any]] = {}  # player's address -> [socket, last sent]
        self.lock = threading.Lock()
        self.run = True
        self.thread = threading.Thread(target=self._loop, daemon=True)

    def start(self) -> "_ForgetfulNat":
        self.thread.start()
        return self

    def stop(self) -> None:
        self.run = False
        self.thread.join(5)
        for sock, _ in self.maps.values():
            sock.close()
        self.inner.close()

    def live_ports(self) -> set[int]:
        with self.lock:
            return {m[0].getsockname()[1] for m in self.maps.values()}

    def _loop(self) -> None:
        while self.run:
            for key, _ in self.sel.select(0.1):
                try:
                    data, src = key.fileobj.recvfrom(65536)  # type: ignore[union-attr]
                except OSError:
                    continue
                with self.lock:
                    if key.data is None:
                        m = self.maps.get(src)
                        if m is None:
                            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                            sock.bind(("127.0.0.1", 0))
                            self.sel.register(sock, selectors.EVENT_READ, src)
                            m = self.maps[src] = [sock, 0.0]
                        m[1] = time.monotonic()
                        m[0].sendto(data, self.mm)
                    elif key.data in self.maps and src == self.mm:
                        self.inner.sendto(data, key.data)
            with self.lock:
                now = time.monotonic()
                for src, (sock, last) in list(self.maps.items()):
                    if now - last > self.idle:
                        self.sel.unregister(sock)
                        sock.close()
                        del self.maps[src]


@pytest.mark.parametrize("idle", [25, 8])
def test_the_room_keeps_the_hosts_endpoint_open(backend: OnlineBackend,
                                                dolphin: Callable[..., DolphinInstance], idle: int) -> None:
    """Bug report #255: a Mario Party 6 room's host behind a carrier-grade NAT; every friend who
    joined from 2 min after the room opened got no link ("0 hellos"). The session probed mm once,
    when the room opened, and a host alone sends nothing else from its socket, so the NAT forgot
    the mapping mm passes to joiners. Now the probe goes every 15 s: with a NAT that forgets after
    25 s, the endpoint mm has stays one the NAT still maps, after a minute alone; with one that
    forgets after 8 s, each probe comes from a new port, and mm passes it on."""
    nat = _ForgetfulNat(backend.mm_port, idle).start()
    try:
        user = backend.create_user(f"hostn{idle}", "HSNA" if idle == 25 else "HSNB")
        host = _boot(dolphin, backend, f"mods-nat-host-{idle}", user,
                     {"host": True, "build": BUILD, "public": False, "controller": "gc"}, mm_port=nat.port)

        def endpoint() -> str:
            room = host.client.call("mods_status").get("room") or {}
            return next((s["endpoint"] for s in room.get("slots", []) if s.get("endpoint")), "")

        first = _wait(endpoint, 60, "the host's endpoint")
        seen = [first]
        t0 = time.monotonic()
        while time.monotonic() - t0 < 65:
            ep = endpoint()
            if ep and ep != seen[-1]:
                seen.append(ep)
            time.sleep(0.5)
        last = endpoint()
        print(idle, seen, nat.live_ports())
        if idle == 25:
            assert seen == [first], seen
            assert int(last.rsplit(":", 1)[1]) in nat.live_ports(), (last, nat.live_ports())
        else:
            assert len(seen) >= 3, seen
            assert len(re.findall(r"room: the server sees this player at", _log(host))) >= 3
        assert _log(host).count("machine: endpoint probe answered") == 1
    finally:
        nat.stop()


# A GameCube game for the codes (a mod's launcher, P+'s here, has its own Gecko code handler where
# Dolphin's goes). E: is a USB disk that stalls sessions: a copy on D:.
GC_DISC = Path(os.environ.get("PPHARNESS_GC_DISC", r"D:\Roms\GCN\Mario Party 6 (USA).nkit.iso"))
# As the launcher starts a game without an SD card (mods_args.ts).
GC_CONFIG = ["Dolphin.Core.WiiSDCard=False"]


def test_host_codes_run_on_every_machine(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance]) -> None:
    """docs/design/room-codes.md: the host's Gecko and Action Replay codes run on the host and, from
    the machine image, on the joiner, and the room lists their names. Marker codes write words
    into free RAM above Dolphin's code list; a counter goes up each frame on both machines (one
    machine without it would desync)."""
    if not GC_DISC.is_file():
        pytest.skip(f"no GameCube disc at {GC_DISC} (PPHARNESS_GC_DISC)")
    codes = [
        {"name": "Marker (Gecko)", "kind": "gecko", "lines": ["04002F00 12345678"]},
        {"name": "Counter (Gecko)", "kind": "gecko",
         "lines": ["82200000 80002F08", "86000000 00000001", "84200000 80002F08"]},
        {"name": "Marker (AR)", "kind": "ar", "lines": ["04002F04 CAFEBABE"]},
    ]
    host_user = backend.create_user("hostk", "HSTK")
    join_user = backend.create_user("joink", "JNYK")
    host = _boot(dolphin, backend, "mods-codes-host", host_user,
                 {"host": True, "build": BUILD, "public": False, "controller": "gc", "codes": codes},
                 boot=GC_DISC, config_args=GC_CONFIG)
    code = _wait(lambda: (host.client.call("mods_status").get("room") or {}).get("code"), 60, "the room's code")
    _wait(lambda: any(s.get("endpoint") for s in host.client.call("mods_status")["room"]["slots"]), 30,
          "the host's endpoint")
    joiner = _boot(dolphin, backend, "mods-codes-join", join_user,
                   {"join": code, "build": BUILD, "controller": "gc", "sd_sync": "yes"},
                   boot=GC_DISC, config_args=GC_CONFIG)
    _wait(lambda: "machine: running" in _log(joiner), 120, "the joiner's session running", interval=1)
    # The names, as mm gives them to every member, in the room file the launcher shows.
    names = [c["name"] for c in codes]
    room_file = json.loads((joiner.create() / "Online" / "mods-room.json").read_text(encoding="utf-8"))
    assert room_file["room"]["build"].get("codes") == names, room_file["room"]
    assert "codes: running the session's 2 Gecko and 1 Action Replay code(s)" in _log(joiner)
    time.sleep(10)
    counters = []
    for inst in (host, joiner):
        word = lambda a: int(inst.client.call("read_u32", addr=a)["value"]) & 0xFFFFFFFF  # noqa: E731
        assert word(0x80002F00) == 0x12345678
        assert word(0x80002F04) == 0xCAFEBABE
        counters.append(word(0x80002F08))
        status = inst.client.call("machine_status")
        assert status.get("desyncs") == 0, status
    # Running since the host's boot: about as many as frames, on both.
    assert min(counters) > 600 and abs(counters[0] - counters[1]) < 300, counters



def test_a_refused_join_leaves_the_players_nand_alone(backend: OnlineBackend,
                                                    dolphin: Callable[..., DolphinInstance]) -> None:
    """Bug report #212: after Other Games, P+ said at every boot that the Wii System Memory is full;
    the player's P+ save had every file but banner.bin. A join mm refuses ("Room not found.")
    before the game's saves are set up (on the reporter's PC mm answered about 2 s before that)
    stops the session while the game still boots on the session's temporary NAND, and the cleanup
    took that NAND for a TAS movie's and copied every save in it over the player's own (bug #175's
    log: "Wii FS Cleanup: Copying 0001000052534245"). Here the game boots 3 s late
    (PPR_TEST_BOOT_DELAY_MS), after the refusal, and the player's own save stays byte for byte."""
    user = backend.create_user("refused", "RFSD")
    if backend.mm_host.startswith("127."):
        ini = {"Online": {"UseDevServer": True, "MatchmakingPort": backend.mm_port,
                          "DevAccountsUrl": backend.accounts_url}}
    else:
        ini = {"Online": {"MatchmakingHost": backend.mm_host, "MatchmakingPort": backend.mm_port,
                          "AccountsUrl": backend.accounts_url}}
    room = {"join": "ZZZZ", "build": BUILD, "controller": "gc", "sd_sync": "yes"}
    inst = dolphin("mods-refused", config=InstanceConfig(cpu_thread=True, video_backend="Null", dolphin_ini=ini),
                   client_timeout=60.0,
                   env={"PPR_MODS_ROOM": json.dumps(room), "PPR_TEST_BOOT_DELAY_MS": "3000"})
    user_dir = inst.create()
    user.write_user_json(user_dir)
    save = user_dir / "Wii" / "title" / "00010000" / "52534245" / "data"
    save.mkdir(parents=True, exist_ok=True)
    (save / "autosv0.bin").write_bytes(b"the player's own")
    mine = {p.name: p.read_bytes() for p in save.iterdir()}
    assert "banner.bin" in mine, sorted(mine)
    inst.launch()

    def log_text() -> str:
        try:
            return _log(inst)
        except FileNotFoundError:
            return ""
    _wait(lambda: not inst.is_running() or "Room not found" in log_text(), 60, "the refused join")
    # The game boots on regardless, then stops (the room closed this Dolphin): wait for it to go.
    t0 = time.monotonic()
    while inst.is_running() and time.monotonic() - t0 < 60:
        time.sleep(0.5)
    if inst.is_running():
        inst.stop()
    log = log_text()
    assert "PPR_TEST_BOOT_DELAY_MS" in log and "Room not found" in log, log[-3000:]
    assert log.index("Room not found") < log.index("Booting from"), log[-3000:]
    assert "Copying from temporary FS" not in log, log[-3000:]
    assert "a machine session's NAND, thrown away" in log, log[-3000:]
    after = {p.name: p.read_bytes() for p in save.iterdir()}
    assert sorted(after) == sorted(mine)
    assert all(after[n] == mine[n] for n in mine), [n for n in mine if after[n] != mine[n]]
