"""Chat (docs/chat-protocol.md, docs/design/chat.md), end to end through the real servers.

Two booted P+ Dolphins (with our plugin, as test_rooms.py boots them) chat through mm: in a room
(made and joined with `rooms_request`, the game's screen held on the room CSS) and in a Direct
match (`mm_search_direct` with the P2P link kept). Dolphin encrypts, mm only relays, the other
Dolphin decrypts, verifies and cleans; `chat_status` shows what each one has. mmclient joins as a
third member where it is built with chat, so the Rust and C++ sides are checked against each
other too.

    PPHARNESS_INSTANCE_PREFIX=chat- python -m pytest harness/tests/test_chat.py -m "dolphin and server"
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend, OnlineUser
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_rooms import MmClient, Player, _boot, _online, _to_versus_css, backend  # noqa: F401

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
import ppom  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server]

# Bidi override, zero-width space, a control: what a hostile sender would put in.
HOSTILE = "hello \u202Eｗｏｒｌｄ\u200B ねこ\x1b[31m"
CLEANED = "hello ｗｏｒｌｄ ねこ[31m"


def chat(p: Player) -> dict[str, Any]:
    return p.c.call("chat_status")


def until_chat(p: Player, pred: Callable[[dict[str, Any]], Any], what: str,
               timeout: float = 20) -> dict[str, Any]:
    try:
        return _wait(lambda: (lambda s: s if pred(s) else None)(chat(p)), timeout, what, interval=0.1)
    except AssertionError:
        raise AssertionError(f"{p.user.display_name}: {what}: {chat(p)}")


def readers(st: dict[str, Any]) -> int:
    return sum(1 for m in st["members"] if not m["is_me"] and m["can_read"])


def texts(st: dict[str, Any], kind: str | None = None) -> list[str]:
    return [line["text"] for line in st["lines"] if kind is None or line["kind"] == kind]


def hold_screen(p: Player, name: str) -> None:
    p.c.call("rooms_request", op="screen", screen=ppom.SCREENS[name])


def boot_two(dolphin: Callable[..., DolphinInstance], be: OnlineBackend, names: tuple[str, str]
             ) -> tuple[Player, Player, OnlineUser, OnlineUser]:
    ua = be.create_user(names[0], names[0][:4].upper())
    ub = be.create_user(names[1], names[1][:4].upper())
    a = _boot(dolphin, f"chat-{names[0]}", be, ua)
    b = _boot(dolphin, f"chat-{names[1]}", be, ub)
    for p in (a, b):
        _to_versus_css(p)
        _online(p)
    return a, b, ua, ub


def test_chat_in_a_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                        tmp_path: Path) -> None:
    """A room: both directions, hostile text cleaned, hide, report (checked by the server), the
    local rate limit, a member leaving, and the group gone after leaving the room."""
    a, b, ua, ub = boot_two(dolphin, backend, ("ines", "otto"))
    for p in (a, b):
        hold_screen(p, "room")
        st = chat(p)
        assert len(st["identity_key"]) == 64 and st["identity_error"] == ""
        assert st["kind"] == "none"

    a.c.call("rooms_request", op="create", public=False)
    code = _wait(lambda: (a.rooms().get("view") or {}).get("code"), 15, "the room's code")
    b.c.call("rooms_request", op="join", code=code)
    for p in (a, b):
        st = until_chat(p, lambda s: s["kind"] == "room" and readers(s) == 1, "the room's chat")
        assert st["group"].startswith(f"room-{code}-") and st["title"] == f"Room {code}"
        assert {m["name"] for m in st["members"]} == {"ines", "otto"}

    # a -> b, with everything a hostile client could send; b -> a.
    a.c.call("chat_send", text=HOSTILE)
    st = until_chat(b, lambda s: CLEANED in texts(s, "message"), "ines's message")
    line = next(l for l in st["lines"] if l["kind"] == "message")
    assert line["name"] == "ines" and line["uid"] == ua.uid and line["seq"] == 1
    b.c.call("chat_send", text="gg")
    until_chat(a, lambda s: "gg" in texts(s, "message"), "otto's message")
    assert texts(chat(a), "own") == [CLEANED]
    for p in (a, b):
        c = chat(p)["counters"]
        drops = {k: v for k, v in c.items() if k.startswith(("drop_", "bad_")) and v}
        assert drops == {}, drops

    # mmclient (the Rust side, common::chat) as a third member: C++ <-> Rust both ways.
    uc = backend.create_user("rust", "RUST")
    a.c.call("rooms_request", op="slot", slot=3, open=True)
    _wait(lambda: a.rooms()["view"]["slots"][2]["open"], 10, "slot 3 open")
    rust = MmClient(backend, uc, tmp_path, "room", "--join", code, "--chat", "--say",
                    "from rust ねこ", "--hold-secs", "120")
    try:
        rust.until(lambda m: m.get("type") == "room-state", "joining the room")
        for p in (a, b):
            until_chat(p, lambda s: "from rust ねこ" in texts(s, "message") and readers(s) == 2,
                       "the Rust member's message")
        rust.until(lambda m: m.get("type") == "mmclient-chat-group" and
                   sum(1 for x in m.get("members", []) if x.get("canRead")) >= 2,
                   "the Dolphins' keys", timeout=20)
        b.c.call("chat_send", text="from c++ " + HOSTILE)
        m = rust.until(lambda m: m.get("type") == "mmclient-chat-msg" and m.get("from") == ub.uid,
                       "otto's message in Rust")
        assert m["text"] == "from c++ " + CLEANED and m["displayName"] == "otto"
        drops = [m for m in rust.lines if m.get("type") == "mmclient-chat-drop"]
        assert drops == [], drops
    finally:
        rust.stop()
    until_chat(a, lambda s: "rust left" in texts(s, "system"), "the Rust member leaving")

    # Hide: kept in chat-hidden.json; still received (the window covers it).
    a.c.call("chat_hide", uid=ub.uid, hidden=True)
    assert next(m for m in chat(a)["members"] if m["uid"] == ub.uid)["hidden"]
    assert ub.uid in (a.online_dir() / "chat-hidden.json").read_text()
    b.c.call("chat_send", text="still here")
    until_chat(a, lambda s: "still here" in texts(s, "message"), "a hidden player's message")

    # Report: the server checks the signatures against otto's registered key.
    a.c.call("chat_report", uid=ub.uid, reason="test report")
    until_chat(a, lambda s: "Reported otto." in texts(s, "system"), "the report's answer")
    out = _wait(lambda: (lambda o: o if "otto" in o or ub.uid in o else None)(
        backend.admin("chat-reports")), 15, "the stored report")
    assert "messages: 3 of 3 verified" in out, out  # gg, the one to mmclient, still here

    # Local rate limit (mm's: 5 in 5 s), once the messages above are out of the window.
    time.sleep(5.5)
    sent = 0
    for i in range(7):
        try:
            a.c.call("chat_send", text=f"burst {i}")
            sent += 1
        except Exception as e:  # noqa: BLE001 - the harness raises the command's error text
            assert "too fast" in str(e)
    assert sent == 5

    # otto leaves the room: a grey line for ines; otto's chat is gone.
    b.c.call("rooms_request", op="leave")
    until_chat(a, lambda s: "otto left" in texts(s, "system") and readers(s) == 0, "otto leaving")
    until_chat(b, lambda s: s["kind"] == "none", "otto's chat ending")


def test_chat_in_a_direct_match(backend: OnlineBackend,
                                dolphin: Callable[..., DolphinInstance]) -> None:
    """A Direct match's chat: made by mm when the two are paired, shown once connected, ended by
    leaving the online CSS (chat_leave, as CMD_CLEANUP_CONNECTION does)."""
    a, b, ua, ub = boot_two(dolphin, backend, ("pavo", "rhea"))
    for p in (a, b):
        hold_screen(p, "online-css")
    a.c.mm_search_direct(ub.connect_code, session="none")
    b.c.mm_search_direct(ua.connect_code, session="none")
    _wait(lambda: all(p.c.mm_status()["state"] == "connection_success" for p in (a, b)), 60,
          "the match")
    for p in (a, b):
        st = until_chat(p, lambda s: s["kind"] == "match" and readers(s) == 1 and s["match_connected"],
                        "the match's chat")
        assert st["group"].startswith("mode.direct-")

    a.c.call("chat_send", text="ready?")
    until_chat(b, lambda s: "ready?" in texts(s, "message"), "pavo's message")
    b.c.call("chat_send", text="yes")
    until_chat(a, lambda s: "yes" in texts(s, "message"), "rhea's message")

    b.c.call("chat_leave")
    until_chat(a, lambda s: "rhea left" in texts(s, "system"), "rhea leaving")
    assert chat(b)["kind"] == "none"
    for p in (a, b):
        p.c.mm_cancel()
