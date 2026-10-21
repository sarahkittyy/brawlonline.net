#!/usr/bin/env python3
"""Builds the hosted Project+ version data the launcher uses to play replays from older P+ releases
(docs/design/pplus-versions.md): published as https://brawlonline.net/updates/pplus/ by
`sudo pp-release pplus DIR` on sarahvps2.

    python tools/pplus_packs/make_packs.py --card v3.2.0=path/to/v3.2.0/sd.raw \\
        --update launcher/src/dolphin/install/pplus_card_update_v3.2.1.json --out DIR

DIR gets:

    index.json               {"v": 1, "newest": release, "releases": [{release, manifest, sha256,
                              files, bytesFromNewest}]}: the newest release last
    releases/<release>.json  {"v": 1, "release", "probe": {path, sha256}, "files": [{path, sha256,
                              size}]}: every file P+'s release card has (sorted by path)
    blobs/<sha256>.zlib      a file's bytes, zlib-compressed, named by the sha256 of the bytes:
                             every file an older release has that the newest one does not have
                             with the same bytes

A release's manifest comes from its own card (`--card`, a release zip's user/Wii/sd.raw) or, for a
release whose card is not at hand, from the release before it and the launcher's card update list
(`--update`, pplus_card_update_<release>.json: the files P+ wrote and removed, from comparing the two
releases' cards file by file). Blobs can only come from a card that is at hand, so give the card of
every release older than the newest. A player's launcher turns its own card (on the newest release)
into an older release's by writing that release's differing files and deleting the newest's extra
ones (launcher/src/dolphin/install/pplus_versions.ts).

When P+ releases again: add the new release's update list (as for v3.2.1), keep the older cards'
--card arguments, rebuild into a fresh DIR and publish it: blobs are never removed on the server,
so launchers of every version keep finding theirs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdcard"))
import fat32  # noqa: E402

PROBE = "/Project+/NETPLAY.GCT"


def card_files(card: Path) -> dict[str, tuple[str, int, bytes]]:
    """Every file of a FAT32 card image: path -> (sha256, size, bytes)."""
    out: dict[str, tuple[str, int, bytes]] = {}
    with open(card, "rb") as f:
        v = fat32.Fat32(f)
        for path, e in v.walk("/"):
            if e.is_dir:
                continue
            data = v.read_chain(e.first_cluster, e.size) if e.size else b""
            out[path] = (hashlib.sha256(data).hexdigest(), e.size, data)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--card", action="append", default=[], metavar="RELEASE=SD.RAW")
    ap.add_argument("--update", action="append", default=[], metavar="UPDATE.JSON")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    manifests: dict[str, dict[str, tuple[str, int]]] = {}
    data: dict[str, bytes] = {}  # sha256 -> bytes, from the cards at hand
    order: list[str] = []
    for spec in a.card:
        release, card = spec.split("=", 1)
        files = card_files(Path(card))
        manifests[release] = {p: (s, n) for p, (s, n, _) in files.items()}
        for s, _, b in files.values():
            data[s] = b
        order.append(release)
        print(f"{release}: {len(files)} files from {card}")
    for u in a.update:
        upd = json.loads(Path(u).read_text())
        base = manifests.get(upd["from"])
        if base is None:
            sys.exit(f"{u}: no manifest for {upd['from']} (give its --card or the update before it)")
        if base.get(upd["probe"]["path"], ("",))[0] != upd["probe"]["from"]:
            sys.exit(f"{u}: {upd['from']}'s probe does not match the update's")
        nxt = dict(base)
        for p in upd["remove"]:
            nxt.pop(p, None)
        sizes = {}
        for w in upd["write"]:
            # The size: from a card at hand that has these bytes, else unknown until then (0).
            sizes[w["path"]] = len(data[w["sha256"]]) if w["sha256"] in data else None
            nxt[w["path"]] = (w["sha256"], sizes[w["path"]])
        if any(v is None for v in sizes.values()) and upd["to"] not in manifests:
            # The newest release's sizes are only informative: its blobs are never needed.
            nxt = {p: (s, n if n is not None else -1) for p, (s, n) in nxt.items()}
        if nxt[upd["probe"]["path"]][0] != upd["probe"]["to"]:
            sys.exit(f"{u}: the result's probe does not match {upd['to']}'s")
        manifests[upd["to"]] = nxt
        order.append(upd["to"])
        print(f"{upd['to']}: {len(nxt)} files ({upd['from']} + {len(upd['write'])} written, {len(upd['remove'])} removed)")

    newest = order[-1]
    out = Path(a.out)
    (out / "releases").mkdir(parents=True, exist_ok=True)
    (out / "blobs").mkdir(parents=True, exist_ok=True)
    index = {"v": 1, "newest": newest, "blobs": "blobs/{sha256}.zlib", "releases": []}
    have = manifests[newest]
    for release in order:
        m = manifests[release]
        need = sorted({s for p, (s, _) in m.items() if have.get(p, ("",))[0] != s})
        missing = [s for s in need if s not in data]
        if missing:
            sys.exit(f"{release}: {len(missing)} files differ from {newest} and no card at hand has them")
        nbytes = zbytes = 0
        for s in need:
            b = data[s]
            nbytes += len(b)
            dst = out / "blobs" / f"{s}.zlib"
            if not dst.exists():
                dst.write_bytes(zlib.compress(b, 9))
            zbytes += dst.stat().st_size
        doc = {"v": 1, "release": release, "probe": {"path": PROBE, "sha256": m[PROBE][0]},
               "files": [{"path": p, "sha256": s, "size": n} for p, (s, n) in sorted(m.items())]}
        text = json.dumps(doc, separators=(",", ":"))
        name = f"releases/{release}.json"
        (out / name).write_text(text)
        index["releases"].append({"release": release, "manifest": name,
                                  "sha256": hashlib.sha256(text.encode()).hexdigest(),
                                  "files": len(m), "filesFromNewest": len(need),
                                  "bytesFromNewest": nbytes, "downloadFromNewest": zbytes})
        print(f"{release}: {len(need)} files differ from {newest}, {nbytes / 2**20:.1f} MiB "
              f"({zbytes / 2**20:.1f} MiB to download)")
    (out / "index.json").write_text(json.dumps(index, indent=1))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
