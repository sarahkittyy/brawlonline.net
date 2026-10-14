"""The SD card in a netplay session (Dolphin's Core/NetPlaySDCard.h), end to end.

* ``test_guest_plays_on_the_hosts_sd_card``: the joiner's SD card has every 7th chunk that holds
  a file damaged (P+'s codes, fighters, stages). With the host loading its save data (Dolphin's
  default), the joiner must play on the host's card: both boot P+ and play a rollback match with
  identical pads and checksums on every confirmed frame, the joiner fetching the host's chunks as
  the game reads them. Neither card image may change (the session's writes are thrown away).
* ``test_no_save_data_plays_on_each_players_own_card``: with "No Save Data" each player plays on
  its own card (no manifest, nothing fetched), and still nothing reaches either image.

Run: ``pytest -m dolphin tests/test_netplay_sd.py`` (two Dolphins, a few minutes).
"""

from __future__ import annotations

import hashlib
import struct
import time
from pathlib import Path

import pytest

from ppharness import brawl as B
from ppharness import flows as F
from ppharness.client import HarnessError
from ppharness.instance import InstanceConfig
from ppharness.session import _start_when_ready, _wait, two_player_netplay

pytestmark = pytest.mark.dolphin

CHUNK = 64 * 1024


def _allocated_chunks(path: Path) -> list[int]:
    """The 64 KiB chunks of a FAT32 image that hold a cluster in use (data area only)."""
    with open(path, "rb") as f:
        bs = f.read(512)
        bps, spc = struct.unpack_from("<HB", bs, 0x0B)
        reserved, nfats = struct.unpack_from("<HB", bs, 0x0E)
        total = struct.unpack_from("<I", bs, 0x20)[0]
        fat_sectors = struct.unpack_from("<I", bs, 0x24)[0]
        assert bps and spc and fat_sectors, "not a FAT32 image"
        f.seek(reserved * bps)
        fat = f.read(fat_sectors * bps)
    data = (reserved + nfats * fat_sectors) * bps
    clusters = (total * bps - data) // (spc * bps)
    entries = struct.unpack_from(f"<{clusters + 2}I", fat)
    chunks = set()
    for cl in range(clusters):
        if entries[cl + 2] & 0x0FFFFFFF:
            start = data + cl * spc * bps
            for c in range(start // CHUNK, (start + spc * bps - 1) // CHUNK + 1):
                chunks.add(c)
    return sorted(chunks)


def _damage(path: Path, chunks: list[int]) -> None:
    with open(path, "r+b") as f:
        for c in chunks:
            f.seek(c * CHUNK + 4096)
            b = bytearray(f.read(256))
            f.seek(c * CHUNK + 4096)
            f.write(bytes(x ^ 0x5A for x in b))


def _digest(path: Path) -> tuple[int, str]:
    h = hashlib.blake2b(digest_size=16)
    with open(path, "rb") as f:
        while block := f.read(16 << 20):
            h.update(block)
    return path.stat().st_mtime_ns, h.hexdigest()


def _sd(client) -> dict:
    sd = client.netplay_status().raw.get("sd_card")
    if sd is None and "sd_card" not in client.netplay_status().raw:
        pytest.skip("build without the netplay SD card")
    return sd or {}


def _compare_histories(host, joiner) -> dict:
    """Pads and per-frame checksums on frames both peers have confirmed (as test_rollback)."""
    h, j = host.rollback_pad_history(0), joiner.rollback_pad_history(0)
    upto = min(max(h, default=0), max(j, default=0)) - 10
    common = sorted(f for f in h if f in j and f <= upto)
    pads = [f for f in common if h[f][:5] != j[f][:5]]
    state = [f for f in common if len(h[f]) > 8 and h[f][5] and j[f][5] and h[f][5:9] != j[f][5:9]]
    in_match = [f for f in common if len(h[f]) > 8 and h[f][5] and j[f][5]]
    return {"compared": len(common), "in_match": len(in_match), "pad_mismatch": pads[:10],
            "state_mismatch": state[:10]}


def _end(s) -> dict:
    """Leave the session (stopping both games, so each Dolphin is done with its card), then the
    cards' digests."""
    for c in s.clients[::-1]:
        c.netplay_leave()
    _wait(lambda: all(c.status().state == "uninitialized" for c in s.clients), 120,
          "both games to stop")
    return {"host": _digest(s.host.user_dir / "Wii" / "sd.raw"),
            "joiner": _digest(s.joiner.user_dir / "Wii" / "sd.raw")}


def _start(s) -> None:
    host, joiner = s.clients
    _start_when_ready(host, 300)
    s.netsim.reset_epoch()
    _wait(lambda: all(c.netplay_status().game_running and c.status().running for c in (host, joiner)),
          300, "the game to start on both")


@pytest.mark.slow
def test_guest_plays_on_the_hosts_sd_card(dolphin_exe):
    with two_player_netplay("lan", rollback=True, exe=dolphin_exe, name="sd-guest", seed=5,
                            config=InstanceConfig(), start_game=False) as s:
        host_sd = s.host.user_dir / "Wii" / "sd.raw"
        joiner_sd = s.joiner.user_dir / "Wii" / "sd.raw"
        damaged = _allocated_chunks(joiner_sd)[1::7]
        assert len(damaged) > 100, len(damaged)
        _damage(joiner_sd, damaged)
        before = {"host": _digest(host_sd), "joiner": _digest(joiner_sd)}

        _start(s)
        host, joiner = s.clients
        try:
            host.rollback_pad_history(0)
        except HarnessError as e:
            pytest.skip(f"build without rollback_pad_history: {e}")
        assert _sd(host)["mode"] == "host"
        sd = _sd(joiner)
        assert sd["mode"] == "guest", sd
        assert sd["size"] == host_sd.stat().st_size

        hs, js = F.netplay_seat(host, "host"), F.netplay_seat(joiner, "joiner")
        F.to_css([hs, js])
        F.pick_characters([(hs, "mario"), (js, "pikachu")])
        F.start_and_pick_stage(hs, "battlefield")
        F.wait_match_started([host, joiner])
        F.play([(hs, "random"), (js, "random")], 60 * 15, seed="sd")
        for c, seat in ((host, hs), (joiner, js)):
            B.neutral(c, seat.pp)
        time.sleep(2)
        cmp = _compare_histories(host, joiner)
        stats = {"host": host.netplay_status().rollback, "joiner": joiner.netplay_status().rollback}
        sd = _sd(joiner)
        after = _end(s)

    assert cmp["compared"] > 600 and cmp["in_match"] > 300, cmp
    assert not cmp["pad_mismatch"], cmp
    assert not cmp["state_mismatch"], cmp
    for name, rb in stats.items():
        assert rb["desyncs_detected"] == 0, (name, rb)
    # The joiner played on the host's card: it fetched the host's chunks, some while the game read.
    assert sd["fetched_bytes"] >= 32 << 20, sd
    assert sd["waits"] > 0, sd
    # Nothing reached either image.
    assert after == before


@pytest.mark.slow
def test_no_save_data_plays_on_each_players_own_card(dolphin_exe):
    cfg = InstanceConfig(dolphin_ini={"NetPlay": {"SyncSaves": False}})
    with two_player_netplay("lan", rollback=True, exe=dolphin_exe, name="sd-own", seed=6,
                            config=cfg, start_game=False) as s:
        host_sd = s.host.user_dir / "Wii" / "sd.raw"
        joiner_sd = s.joiner.user_dir / "Wii" / "sd.raw"
        before = {"host": _digest(host_sd), "joiner": _digest(joiner_sd)}
        _start(s)
        host, joiner = s.clients
        F.to_css([F.netplay_seat(host, "host"), F.netplay_seat(joiner, "joiner")])
        for c in (host, joiner):
            sd = _sd(c)
            assert sd["mode"] == "own", sd
            assert sd["fetched_bytes"] == 0, sd
        after = _end(s)
    assert after == before
