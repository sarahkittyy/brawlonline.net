"""Compares two Other Games SD cards by their listings on the accounts server (docs/design/
brawl-mods.md §6.5): the files only one card has, the files at other places, and each 64 KiB chunk
that differs with the files that use it.

A room shows its card's root ("SD card: e78976"); every host's launcher puts its card's listing on
the server (sd_card_listings, keyed by root). Fetch two (read-only) and compare:

    ssh debian@sarahvps2 "sudo -u postgres psql -d pp -At -c \\"SELECT root, bytes, created_at,
        used_at FROM sd_card_listings WHERE root LIKE 'e78976%'\\""
    ssh debian@sarahvps2 "sudo -u postgres psql -d pp -At -c \\"SELECT encode(listing,'base64')
        FROM sd_card_listings WHERE root='<full root>'\\"" | tr -d '\\n' | base64 -d > e78976.bin
    python -I tools/sdcard/listing_diff.py e78976.bin a7d19a.bin

A listing has no hashes of whole files, only of 64 KiB card chunks. A chunk holds several small
files, the ends of big ones and free clusters, and its hash covers all of it: a chunk can differ
for a neighbour, or for leftovers of deleted data in its free clusters. So a file is "changed"
here only by chunks that hold it alone (inside one of its runs); the rest is reported as edges.

Listing format (Machine::SDCard::EncodeCardListing): u32 length of the canonical manifest; the
manifest (u64 card size, then per chunk 0, or 1 and its 16-byte hash); the file list: u32 count,
per file u32 path length, path, size as two u32s (low, high), u32 run count, (first, count) runs.
"""

import struct
import sys
from collections import defaultdict


def parse(path):
    b = open(path, "rb").read()
    (clen,) = struct.unpack_from("<I", b, 0)
    canon = b[4 : 4 + clen]
    (size,) = struct.unpack_from("<Q", canon, 0)
    chunks = []
    pos = 8
    while pos < len(canon):
        used = canon[pos]
        pos += 1
        if used:
            chunks.append(canon[pos : pos + 16])
            pos += 16
        else:
            chunks.append(None)
    files = {}
    lst = b[4 + clen :]
    if lst:
        (count,) = struct.unpack_from("<I", lst, 0)
        pos = 4
        for _ in range(count):
            (n,) = struct.unpack_from("<I", lst, pos)
            pos += 4
            name = lst[pos : pos + n].decode("utf-8", "replace")
            pos += n
            lo, hi = struct.unpack_from("<II", lst, pos)
            pos += 8
            (runs,) = struct.unpack_from("<I", lst, pos)
            pos += 4
            rs = []
            for _ in range(runs):
                rs.append(struct.unpack_from("<II", lst, pos))
                pos += 8
            files[name] = (lo | (hi << 32), rs)
    return size, chunks, files


def main(a_path, b_path):
    sa, ca, fa = parse(a_path)
    sb, cb, fb = parse(b_path)
    for tag, p, s, c, f in (("A", a_path, sa, ca, fa), ("B", b_path, sb, cb, fb)):
        used = sum(x is not None for x in c)
        print(f"{tag} {p}: card {s / 2**20:.0f} MiB, {len(f)} files, {used} chunks in use")
    if not fa or not fb:
        print("a listing without its file list (an older launcher): chunks only")

    only_a = sorted(set(fa) - set(fb))
    only_b = sorted(set(fb) - set(fa))
    both = sorted(set(fa) & set(fb))
    resized = [p for p in both if fa[p][0] != fb[p][0]]
    moved = [p for p in both if fa[p][0] == fb[p][0] and fa[p][1] != fb[p][1]]
    for title, names, files in (("only on A", only_a, fa), ("only on B", only_b, fb)):
        print(f"\n{title}: {len(names)} files, {sum(files[n][0] for n in names) / 1e6:.1f} MB")
        for n in names:
            print(f"  {files[n][0]:>11}  {n}")
    print(f"\nanother size: {len(resized)} files")
    for n in resized:
        print(f"  {fa[n][0]:>11} -> {fb[n][0]:>11}  {n}")
    print(f"\nsame size, at other chunks (written again): {len(moved)} files")
    for n in moved:
        print(f"  {fa[n][0]:>11}  {n}")

    # Chunks: which differ, and whose they are (A's layout; moved files are listed above).
    users = defaultdict(set)
    edges = set()
    for name, (_, rs) in fa.items():
        for first, n in rs:
            edges.update((first, first + n - 1))
            for i in range(first, first + n):
                users[i].add(name)
    count = max(len(ca), len(cb))
    diff = [i for i in range(count) if (ca[i] if i < len(ca) else None) != (cb[i] if i < len(cb) else None)]
    inner = defaultdict(int)
    edge_only = set()
    unused = 0
    for i in diff:
        u = users.get(i, set())
        if not u:
            unused += 1
        elif len(u) == 1 and i not in edges:
            inner[next(iter(u))] += 1
        else:
            edge_only.update(u)
    print(f"\nchunks that differ: {len(diff)} ({len(diff) * 64 / 1024:.1f} MiB, what a joiner downloads)")
    print(f"  in no file of A (FAT, folders, B's files elsewhere): {unused}")
    print(f"  inside a run of one file (its own bytes differ): {sum(inner.values())}, in {len(inner)} files")
    for n, k in sorted(inner.items(), key=lambda x: -x[1]):
        print(f"    {k:>4} chunks  {fa[n][0]:>11}  {n}")
    edge_only -= set(inner)
    print(f"  at a run's edge or shared with other files (may be only a neighbour or free space): {len(edge_only)} files")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
