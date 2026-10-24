"""The player's name tag on the online CSSs (game-code online_menu.cpp, room_css.cpp; Dolphin
GameBridge.cpp, Online/last-tag.json), and over the fighters in the match (online_match.cpp,
match_hud.cpp; Dolphin VertexManagerBase.cpp).

Staging feedback 2026-10-11: on a room's CSS the player's own plate showed the account name, so
the tag picked there was not to be seen, and every online CSS started without a tag. Now the plate
shows the picked tag (the account name without one), the tag goes from one online CSS to the next,
and Dolphin keeps the tag of the last lock-in for the next boot.

Screenshots: run/artifacts/game-code/name-tags/<test>/<player>/.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import ROOT, Game, _boot, _connected_direct, online_set
from test_online_game import backend  # noqa: F401  (the backend fixture)
from test_rooms_game import CSS, create_room, lock_of, menu_state, pick, room_view

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

ART = ROOT / "run" / "artifacts" / "game-code" / "name-tags"
NOTAP = "ＮｏＴａｐ"   # run/template-user's save: the first tag in the CSS's list (slot 0)


def last_tag_file(g: Game) -> Path:
    return Path(g.inst.user_dir) / "Online" / "last-tag.json"


def saved_tag(g: Game) -> str | None:
    p = last_tag_file(g)
    return json.loads(p.read_text(encoding="utf-8"))["tag"] if p.exists() else None


def test_tag_shown_in_a_room_and_kept(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                      gpu_backend: str) -> None:
    """A room's CSS: no tag at first (the plate shows the account name), the tag picked shows on
    the plate, the lock-in (START: ready) has its controls and Dolphin saves its name. Out of the
    room and into Direct's CSS: the tag is still on the panel."""
    test = "room-then-direct"
    u = backend.create_user("tagroom", "TGRM")
    g = _boot(dolphin, "tags-a", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
    g.to_main_menu()
    g.to_online_page()
    assert saved_tag(g) is None
    create_room(g)
    assert g.tag() == -1, g.tag()
    g.steps("wait 30")
    g.shot("03-no-tag-account-name")
    g.pick_tag(1)
    assert g.tag() == 0, g.tag()
    g.steps("wait 30")
    g.shot("04-tag-on-the-plate")
    pick(g)
    g.steps("tap START 8", "wait 40")
    _wait(lambda: lock_of(g)["ready"], 10, "ready in the room")
    _wait(lambda: saved_tag(g) == NOTAP, 10, "last-tag.json with the tag")
    g.shot("05-ready-with-the-tag")

    # Out of the room (hold B: WITH FRIENDS, Create Room highlighted), into Direct 1v1's CSS.
    g.steps("hold B 120", "until muMenuMain 600", "wait 90", "tap DRIGHT 8", "wait 30")
    g.shot("06-with-friends-direct")
    g.steps("tap A", "wait 400", f"until {CSS} 600", "wait 60")
    _wait(lambda: menu_state(g) == 3, 10, "Direct's CSS")
    assert g.tag() == 0, g.tag()
    g.shot("07-direct-css-with-the-tag")


@pytest.mark.parametrize("saved", [NOTAP, "", "GONE"])
def test_tag_from_the_last_boot(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                gpu_backend: str, saved: str) -> None:
    """A boot with last-tag.json (an earlier boot's lock-in): the first online CSS starts with that
    tag; none for "" or a tag the save no longer has."""
    letter = {NOTAP: "T", "": "N", "GONE": "G"}[saved]
    test = f"last-boot-{letter}"
    u = backend.create_user(f"tagboot{letter.lower()}", f"TGB{letter}")

    def put(user_dir: Path) -> None:
        p = Path(user_dir) / "Online" / "last-tag.json"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps({"tag": saved}), encoding="utf-8")

    g = _boot(dolphin, "tags-b", backend, u, gpu_backend, test, "gameplay", artifacts=ART, before_launch=put)
    g.to_main_menu()
    g.to_online_page()
    create_room(g)
    want = 0 if saved == NOTAP else -1
    assert g.tag() == want, (g.tag(), room_view(g))
    g.steps("wait 30")
    g.shot("03-room-css")


NTAP2 = "ＮＴａｐ２"   # run/template-user's save: the second tag in the CSS's list (slot 1)


def match_names(g: Game) -> list[str]:
    """The match players' names (gmGlobalModeMelee [[0x805A00E0]+8] +0x98 + 0x5C * port, +0x0C:
    5 UTF-16 characters and a 0), the name Brawl renders into the tag over the fighter."""
    mm = g.c.read_u32(g.c.read_u32(0x805A00E0) + 8)
    out = []
    for port in range(4):
        raw = g.c.read_mem(mm + 0x98 + 0x5C * port + 0x0C, 12)
        out.append(raw.decode("utf-16-be").split("\0")[0])
    return out


def test_tags_over_the_fighters(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                gpu_backend: str) -> None:
    """Sarah, 2026-10-23: "can we get the tags you pick in the css to show up above everyone's
    head". A Direct match where A picked NoTap and B NTap2: on both machines each port's match
    player has its player's tag as its name (Brawl's own name tag over the fighter), from the
    lock-ins' port values. The shots show the tags drawn in full on a fresh instance (no shader
    cache: before the plugin marked the tag render for Dolphin, Skip Drawing left an empty grey
    box). Screenshots: run/artifacts/game-bridge/tags-over-fighters/."""
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "tags-over-fighters", ("tagov", "tagow"),
                                     stocks=4, tags=(1, 2))
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    want = {ua.display_name: NOTAP, ub.display_name: NTAP2}
    for g in (a, b):
        se = online_set.session(g.c)
        names = match_names(g)
        for port, pl in enumerate(se["players"]):
            assert pl["pv"]["tag_name"] == want[pl["name"]], pl
            assert names[port] == want[pl["name"]], (g.name, names, se["players"])
        assert names[len(se["players"]):] == [""] * (4 - len(se["players"])), names
    a.steps("wait 120")
    for g in (a, b):
        g.shot("01-match")
    for g in (a, b):
        g.c.mm_cancel()
