"""What a gameplay rollback session sends (2026-10-13: the spectating session's uplink test measured
555-650 kbit/s from a player, where docs/backend-design.md §2.7 assumed 10-20). Before, about four
GekkoNet datagrams a frame each way (the frame's inputs, the same inputs again when the peer's ack
came in, an ack per frame received, a checksum per frame), the state message every 50 ms all match
long, and the host's with its sync block and task order (about 5.5 KB as 5 datagrams): ~750
kbit/s from the host, ~160 from the joiner. Now one datagram a frame (GekkoBatch, 'B'), the state
only when it changes or every 500 ms in a match, the sync block only until the barrier.

A thin wrapper around ``tools/gprb_traffic.py`` (``gprb_session.py`` through a netsim that
classifies every datagram): `typical` (40 ms round trip, 0.5% loss), a 1-minute match, both
peers on this build (on the build before, 4.4 datagrams a frame from the joiner and 'F' pieces
from the host). About 3 minutes.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

pytestmark = [pytest.mark.dolphin, pytest.mark.slow]


def test_one_datagram_a_frame_and_no_desync(tmp_path: Path) -> None:
    import gprb_traffic

    report = tmp_path / "traffic.json"
    rc = gprb_traffic.main(["--preset", "typical", "--minutes", "1", "--name-prefix", "pytest-traffic",
                            "--json", str(report)])
    out = json.loads(report.read_text())
    t, s = out["traffic"], out["session"]
    print(gprb_traffic.format_report("traffic", t))
    print(json.dumps(s)[:2000])
    assert rc == 0 and not s.get("error"), s.get("error")
    # The match may end early on stocks: enough seconds to measure, all the same.
    assert t["match_seconds"] >= 10, t["match_seconds"]
    for d in ("up", "down"):  # up: the joiner's upload, down: the host's
        x = t[d]
        assert x["packets_per_frame"] <= 1.3, (d, x["packets_per_frame"], x["kinds"])
        assert x["kbps_wire"] <= 80, (d, x["kbps_wire"], x["kinds"])
        # The state message: never split in a match, a few a second at most.
        assert "F:piece" not in x["kinds"], (d, x["kinds"]["F:piece"])
        assert x["kinds"].get("C:st", {}).get("pps", 0) <= 4, (d, x["kinds"].get("C:st"))
    assert s["checksums"]["compared"] >= 600 and s["checksums"]["mismatches"] == 0, s["checksums"]
    assert s["desyncs"] == [0, 0], s["desyncs"]
    assert s["trace"]["identical_through_game_set"], s["trace"]
