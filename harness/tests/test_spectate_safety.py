"""Spectating's safety tests (docs/design/spectating.md §4.4): the spectate upload must not make a
player's game worse, on a weak connection above all.

The rig: two Dolphins play a Direct set under a local backend with spectating on. The first
player streams; the second has [Online] StreamGames off. The first player's whole upload goes
through one capped queue (netsim SharedUplink, 250 ms of buffer like a bufferbloated home
router): their game's packets to the other player (the P2P relay, "up" = first -> second) and
their connection to mm (a second relay in front of mm, which carries the spectate upload). Every
game of the set runs one mode, switched between games (harness `spectate_config`):

- ``off``: StreamGames off, nothing uploaded (the baseline);
- ``on``: the uploader as it ships (2 KB/s pacer, 512-byte packets, backoff);
- ``forced``: no backoff, no start delay, 16 KB/s in 1,200-byte packets, nothing held back: the
  same data sent the naive way;
- ``flood``: StreamGames off and a stand-in for a harmful upload: 1,200-byte datagrams at 16 KB/s
  from the first player's side through the mm relay (mm drops them). The upload's real data is
  only ~40 KB a game, too little to hurt much even sent naively, so this is what proves the
  measurement sees harm when there is some.

What the upload costs the game is measured directly, per game packet: the SharedUplink counts,
for each of the first player's game packets, the upload bytes queued ahead of it (the delay the
upload added to that packet) and whether a packet dropped at a full queue had upload bytes ahead.
Games differ too much from each other (their traffic, how long they run) for an "on" game's total
delay to be compared with an "off" game's; the upload's own share is exact. Total delay, each
player's rollbacks, late frames and the confirmed checksums are reported too. Before every game
the last game's upload is let finish, so a game in one mode never carries another's upload.

T1 (shared uplink): a game with no cap measures the game's own upload rate; then off/on/forced at
uplinks of 1.2x, 1.6x and 3x it (PPHARNESS_T1_FACTORS), and a flood at the first. T2 (CPU starvation): off/on/forced with a CPU burner per
core. T3 (misbehaving server): the mm relay drops everything from mm for a game (no
acknowledgements, the online connection times out), delays it 2 s, or is slow; the game must be
as off's.

Results: run/artifacts/spectate-safety/<test>.json and a table on stdout. About 25 minutes for
T1, 10 for T2 and T3 each.
"""

from __future__ import annotations

import json
import os
import re
import socket
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterator

import pytest

from ppharness.backend import OnlineBackend, OnlineUser
from ppharness.instance import DolphinInstance
from ppharness.netsim import LinkProfile, NetSim, SharedUplink, enet_class

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from test_online import _make_backend  # noqa: E402
from test_online_game import ROOT, _connected_direct  # noqa: E402
import online_set  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu, pytest.mark.slow]

ARTIFACTS = ROOT / "run" / "artifacts" / "spectate-safety"
ONE_WAY_MS = 15.0       # P2P latency each way (30 ms ping) before any queueing
MM_ONE_WAY_MS = 10.0
UNLIMITED_KBPS = 1_000_000.0
UPLOAD_PACKET = 256 + 28      # the uploader's largest datagram on the wire
LATE = re.compile(r"gprb net: session: .*?late frames (\d+)")


@pytest.fixture(scope="module")
def spectate_backend() -> Iterator[OnlineBackend]:
    be = _make_backend(spectate="on")
    try:
        yield be
    finally:
        be.stop()


@pytest.fixture
def video(request: pytest.FixtureRequest) -> str:
    """The video backend: the GPU one the other online tests use, or PPHARNESS_SAFETY_VIDEO
    (`Null` while a GPU is busy with something else on the machine: these tests read nothing on
    screen, so their screenshots are skipped)."""
    v = os.environ.get("PPHARNESS_SAFETY_VIDEO")
    if not v:
        return request.getfixturevalue("gpu_backend")
    if v == "Null":
        import test_online_game as tog
        original = tog.Game.shot

        def shot(self: Any, name: str) -> Path:
            try:
                return original(self, name)
            except Exception:  # noqa: BLE001 - no screenshots with the Null backend
                return self.out / f"{name}.png"
        tog.Game.shot = shot  # type: ignore[method-assign]
    return v


@dataclass
class Rig:
    a: Any
    b: Any
    p2p: NetSim
    mm: NetSim
    uplink: SharedUplink
    panels: dict[str, Any]
    games: int = 0
    late_seen: dict[str, int] = field(default_factory=dict)

    def stop(self, test: str = "") -> None:
        for sim in (self.p2p, self.mm):
            sim.stop()
        # Both games' logs, kept whether the test passes or not.
        if test:
            import shutil
            out = ARTIFACTS / f"{test}-logs"
            out.mkdir(parents=True, exist_ok=True)
            for g in (self.a, self.b):
                src = g.inst.user_dir / "Logs" / "dolphin.log"
                if src.exists():
                    shutil.copy2(src, out / f"{g.name}.log")


def make_rig(be: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu: str, test: str) -> Rig:
    # Only the spectate upload (ENet channel 1 to mm) is blamed: the online connection's own
    # traffic (pings, acks, room messages) is there whether or not anything is streamed.
    uplink = SharedUplink(UNLIMITED_KBPS, queue_ms=250.0, blame="upload")
    ports = []
    for _ in range(2):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            ports.append(probe.getsockname()[1])
    aliases = ("127.0.0.2", "127.0.0.3")
    p2p = NetSim(("127.0.0.1", ports[1]), (aliases[1], ports[1]), LinkProfile(latency_ms=ONE_WAY_MS),
                 seed="p2p", upstream_bind=(aliases[0], ports[0]), client=("127.0.0.1", ports[0]),
                 shared_up=uplink).start()
    mm = NetSim(("127.0.0.1", be.mm_port), ("127.0.0.1", 0), LinkProfile(latency_ms=MM_ONE_WAY_MS),
                seed="mm", shared_up=uplink, shared_class=enet_class(1, "upload", "online")).start()
    inis = [{"Online": {"ForceNetplayPort": True, "NetplayPort": ports[i], "ForceLanIP": True,
                        "LanIP": aliases[i]}} for i in range(2)]
    inis[0]["Online"]["MatchmakingPort"] = mm.listen_port   # the streamer reaches mm through the relay
    inis[1]["Online"]["StreamGames"] = False
    try:
        a, b, _, _ = _connected_direct(be, dolphin, gpu, test, ("ulla", "vern"), stocks=3,
                                       dolphin_ini=(inis[0], inis[1]))
    except BaseException:
        p2p.stop()
        mm.stop()
        raise
    return Rig(a, b, p2p, mm, uplink, panels={g.name: g.panel for g in (a, b)})


def late_frames(g: Any, rig: Rig) -> int:
    """The late frames of the game that just ended (its session's summary line)."""
    text = (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    found = LATE.findall(text)
    n = len(found)
    assert n > rig.late_seen.get(g.name, 0), f"{g.name}: no session summary for this game"
    rig.late_seen[g.name] = n
    return int(found[-1])


def play(rig: Rig, mode: str, kbps: float = UNLIMITED_KBPS,
         during: Callable[[Rig], Any] | None = None, after: Callable[[Rig], Any] | None = None) -> dict[str, Any]:
    """One game of the set in `mode` with the streamer's uplink at `kbps`."""
    a, b = rig.a, rig.b

    def idle() -> bool:
        s = a.c.call("spectate_status")
        return (s.get("game") is None or s.get("role") != "primary" or s.get("stopped")
                or s.get("sent") == s.get("chunks"))
    online_set.wait(idle, 300, "the last game's upload to finish", every=1.0)
    stream = mode in ("on", "forced")
    st = a.c.call("spectate_config", stream=stream, forced=mode == "forced")
    assert st["stream"] == stream, st
    rig.games += 1
    n = rig.games
    if n > 1:
        online_set.lock_in_next([a, b], n, "direct", print, rig.panels)
    rig.uplink.set_rate(kbps)
    drops0 = rig.uplink.drops
    rig.p2p.reset_stats()
    rig.mm.reset_stats()
    rig.uplink.reset_counters()
    flood_stop = None
    if mode == "flood":
        flood_stop = start_flood(rig.mm.listen_addr)
    if during:
        online_set.wait(lambda: online_set.gstatus(a.c)["phase"] == "running", 240, "the match")
        during(rig)
    t0 = time.monotonic()
    try:
        g = online_set.play_game([a, b], n)
    finally:
        if flood_stop:
            flood_stop.set()
    secs = time.monotonic() - t0
    p2p, mm = rig.p2p.stats(), rig.mm.stats()
    spec = a.c.call("spectate_status")
    rooms = a.c.call("rooms_status")
    if after:
        after(rig)
    online_set.back_on_css([a, b], n, rig.panels)
    up = p2p["up"]
    rep = {
        "game": n, "mode": mode, "kbps": kbps, "seconds": round(secs, 1),
        "checksums": g["checksums"], "frames": g["frames"], "end": g["end"],
        "rollbacks": dict(zip([g["host"], g["join"]], g["rollbacks"])),
        "max_rollback": dict(zip([g["host"], g["join"]], g["max_rollback"])),
        "late": {x.name: late_frames(x, rig) for x in (a, b)},
        # The streamer's game packets to the other player through the shared queue.
        "game_up": {"packets": up["packets_in"], "bytes": up["bytes_in"], "drops": up["dropped"],
                    "drops_by_reason": up["dropped_by_reason"],
                    "delay_ms": {k: round(v, 1) for k, v in up["delay_ms"].items() if k != "n"}},
        "mm_up_bytes": mm["up"]["bytes_in"], "mm_down_bytes": mm["down"]["bytes_in"],
        "uplink_drops": rig.uplink.drops - drops0,
        # What the upload cost the game's packets on the shared uplink.
        "cost": rig.uplink.report("game"),
        "upload_wire": rig.uplink.report("upload"),
        "uploader": spec,
        # The streamer's online connection to mm (the upload's way), and how often it dropped.
        "online": {"connection": rooms.get("connection"), "drops": rooms.get("drops"),
                   "error": rooms.get("connection_error")},
    }
    rate = (up["bytes_in"] + 28 * up["packets_in"]) * 8 / max(secs, 1) / 1000
    rep["game_up_kbps"] = round(rate, 1)
    d = rep["game_up"]["delay_ms"]
    c = rep["cost"]["delay_by_others_ms"]
    print(f"game {n} {mode:>6} @ {kbps:>9.1f} kbit/s, game {rate:5.0f} kbit/s, {secs:5.1f} s: game packets "
          f"p50 {d.get('p50', 0):6.1f} p99 {d.get('p99', 0):6.1f} max {d.get('max', 0):6.1f} ms, drops "
          f"{up['dropped']}; by the upload: p99 {c.get('p99', 0):5.1f} max {c.get('max', 0):5.1f} ms, drops "
          f"{rep['cost']['drops_with_others_ahead']}; upload {rep['upload_wire']['bytes']} B; rollbacks "
          f"{rep['rollbacks']}, late {rep['late']}; uploader pauses {spec.get('pauses')} held "
          f"{spec.get('held_s', 0):.1f} s sent {spec.get('sent')}/{spec.get('chunks')}; online "
          f"{rooms.get('connection')} (drops {rooms.get('drops')})", flush=True)
    assert g["checksums"]["mismatches"] == 0, g["checksums"]
    return rep


def start_flood(target: tuple[str, int], bytes_per_s: int = 16384, size: int = 1200):
    """Sends `size`-byte datagrams at `bytes_per_s` to `target` until the returned event is set."""
    import threading
    stop = threading.Event()

    def run() -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind(("127.0.0.1", 0))
            # Shaped like ENet data on channel 1 (SEND_RELIABLE), so it is classed as upload; mm
            # drops it (no such peer).
            junk = bytes([0, 0, 0x86, 1, 0, 0]) + (size - 8).to_bytes(2, "big") + bytes(size - 8)
            gap = size / bytes_per_s
            nxt = time.perf_counter()
            while not stop.is_set():
                s.sendto(junk, target)
                nxt += gap
                time.sleep(max(0.0, nxt - time.perf_counter()))
    threading.Thread(target=run, daemon=True).start()
    return stop


def packet_ms(kbps: float) -> float:
    """How long one of the uploader's packets holds an uplink of `kbps`."""
    return UPLOAD_PACKET * 8 / kbps


def save(test: str, data: Any) -> None:
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    (ARTIFACTS / f"{test}.json").write_text(json.dumps(data, indent=1))


def extra_queueing(rep: dict[str, Any], base: dict[str, Any], key: str) -> float:
    return rep["game_up"]["delay_ms"].get(key, 0) - base["game_up"]["delay_ms"].get(key, 0)


def test_t1_shared_uplink(spectate_backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                          video: str) -> None:
    TEST = "t1"
    rig = make_rig(spectate_backend, dolphin, video, "spectate-t1")
    results: list[dict[str, Any]] = []
    try:
        cal = play(rig, "off")
        results.append(cal)
        game_kbps = cal["game_up_kbps"]
        print(f"the game's own upload: {game_kbps} kbit/s")
        # Uplinks relative to the streamer's own game traffic, which depends on its role: the
        # host sends ~750 kbit/s (its 5.5 KB state message every 50 ms), the joiner ~150, and the
        # role is fixed for a run (by the accounts). Below about 1.1x the game itself cannot start.
        factors = [float(f) for f in os.environ.get("PPHARNESS_T1_FACTORS", "1.2,1.6,3").split(",")]
        for i, f in enumerate(factors):
            kbps = round(game_kbps * f, 1)
            for mode in ("off", "on", "forced") + (("flood",) if i == 0 else ()):
                rep = play(rig, mode, kbps)
                rep["factor"] = f
                results.append(rep)
                save("t1", {"game_kbps": game_kbps, "games": results})
    finally:
        save("t1", {"game_kbps": cal["game_up_kbps"] if results else None, "games": results})
        rig.stop(TEST)

    verdicts = []
    cost = lambda r, k: round(r["cost"]["delay_by_others_ms"].get(k, 0.0), 1)  # noqa: E731
    for kbps in sorted({r["kbps"] for r in results if "factor" in r}):
        by = {r["mode"]: r for r in results if r.get("factor") is not None and r["kbps"] == kbps}
        f = by["on"]["factor"]
        on, forced = by["on"], by["forced"]
        v = {"factor": f, "kbps": on["kbps"], "packet_ms": round(packet_ms(on["kbps"]), 2),
             "on_cost_p99_ms": cost(on, "p99"), "on_cost_max_ms": cost(on, "max"),
             "on_drops_by_upload": on["cost"]["drops_with_others_ahead"],
             "on_upload_bytes": on["upload_wire"]["bytes"],
             "forced_cost_p99_ms": cost(forced, "p99"), "forced_cost_max_ms": cost(forced, "max"),
             "forced_drops_by_upload": forced["cost"]["drops_with_others_ahead"]}
        if "flood" in by:
            fl = by["flood"]
            v.update(flood_cost_p99_ms=cost(fl, "p99"), flood_cost_max_ms=cost(fl, "max"),
                     flood_drops_by_upload=fl["cost"]["drops_with_others_ahead"])
        verdicts.append(v)
        print("T1", v, flush=True)
    save("t1", {"games": results, "verdicts": verdicts})
    for v in verdicts:
        # The shipping uploader: no game packet dropped with upload bytes ahead of it; at the p99 of
        # the game's packets at most one of its packets ahead, and never more than three (it keeps
        # at most two in flight; the online connection's own pings make the third).
        pkt = v["packet_ms"]
        assert v["on_drops_by_upload"] == 0, v
        assert v["on_cost_p99_ms"] <= 1.5 * pkt, v
        assert v["on_cost_max_ms"] <= 3 * pkt + 1.0, v
    # On a full uplink the uploader rightly sends nothing; where there is room it does upload, or
    # the test proves nothing.
    assert any(v["on_upload_bytes"] > 20000 for v in verdicts), verdicts
    # The measurement sees harm when there is some: the flood costs the game's packets a lot.
    fl = verdicts[0]
    assert fl["flood_drops_by_upload"] > 0 or fl["flood_cost_p99_ms"] > 20, fl


def test_t2_cpu_starvation(spectate_backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                           video: str) -> None:
    from gprb_replay_stress import burners
    TEST = "t2"
    rig = make_rig(spectate_backend, dolphin, video, "spectate-t2")
    results: list[dict[str, Any]] = []
    procs = []
    try:
        procs = burners(max(2, (os.cpu_count() or 8) // 2))
        for mode in ("off", "on", "forced", "off", "on"):
            results.append(play(rig, mode))
            save("t2", {"games": results})
    finally:
        for p in procs:
            p.kill()
        rig.stop(TEST)
    late = {m: [r["late"]["ulla"] for r in results if r["mode"] == m] for m in ("off", "on", "forced")}
    per_s = {m: [r["late"]["ulla"] / max(r["frames"][0], 1) * 60 for r in results if r["mode"] == m]
             for m in ("off", "on", "forced")}
    print("T2 late frames of the streamer", late, "per second", per_s)
    save("t2", {"games": results, "late": late, "late_per_s": per_s})
    # The streamer's late frames with the upload on stay within the spread of off's games.
    worst_off = max(per_s["off"])
    assert max(per_s["on"]) <= worst_off * 1.25 + 0.5, per_s


def test_t3_misbehaving_server(spectate_backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                               video: str) -> None:
    TEST = "t3"
    rig = make_rig(spectate_backend, dolphin, video, "spectate-t3")
    normal = LinkProfile(latency_ms=MM_ONE_WAY_MS)
    cases: list[tuple[str, LinkProfile]] = [
        ("silent", LinkProfile(latency_ms=MM_ONE_WAY_MS, loss=1.0)),      # nothing from mm: no acks
        ("slow", LinkProfile(latency_ms=2000.0)),                         # acks 2 s late
        ("trickle", LinkProfile(latency_ms=MM_ONE_WAY_MS, rate_kbps=1.0, queue_ms=5000.0)),
    ]
    results: list[dict[str, Any]] = []
    try:
        results.append(play(rig, "off"))
        for name, down in cases:
            def bad(r: Rig, down: LinkProfile = down) -> None:
                r.mm.set_link("down", down)

            def good(r: Rig) -> None:
                r.mm.set_link("down", normal)
            rep = play(rig, "on", during=bad, after=good)
            rep["case"] = name
            results.append(rep)
            save("t3", {"games": results})
            # Back to normal: the online connection comes back before the next case.
            online_set.wait(lambda: rig.a.c.call("spectate_status") is not None, 30, "the uploader")
            time.sleep(12)
        results.append(play(rig, "off"))
    finally:
        save("t3", {"games": results})
        rig.stop(TEST)
    offs = [r for r in results if r["mode"] == "off"]
    worst = {k: max(r["game_up"]["delay_ms"].get(k, 0) for r in offs) for k in ("p99", "max")}
    for r in results:
        if r["mode"] != "on":
            continue
        print("T3", r["case"], r["game_up"]["delay_ms"], r["rollbacks"], r["late"])
        assert r["game_up"]["drops"] == 0, r
        assert r["game_up"]["delay_ms"].get("p99", 0) <= worst["p99"] + 8.0, (r["case"], r["game_up"], worst)
        assert all(e == "game set" for e in r["end"]), r
