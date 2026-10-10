"""Compare what a player hears in a rollback run with the same match without rollback.

Both inputs are DSP dumps (``gprb_mispredict.py run --dump-audio DIR``) of runs from the same
countdown savestate and recorded input: the ground truth (``--no-rollback``) and a rollback run
(``--modes mp``). Gameplay is identical (the traces say so), so the music and nearly every sound
should be too. Each window of the rollback run is looked up in the ground truth (normalized cross-
correlation over the whole file, 8 kHz mono):

- ``match``: the best correlation. Near 1 when the window is heard as in the ground truth; low when
  rollbacks cut pieces out of it (the sound system ran on through resimulations whose samples were
  dropped) or the sounds in it differ.
- ``drift``: where the best match lies minus where the window lies, after the offset of the first
  windows that match well (the countdown). Grows by the resimulated span when every rollback cuts
  it out of the audio; stays near 0 when the audio clock waits for the resimulation.

The waveform match is strict: a sound that starts an AX frame (3 ms) early or late, which nobody
hears, lowers it. What is heard is compared on band levels (32 bands, 32 ms windows every 16 ms,
each against the ground truth's within +-32 ms): the median spectral difference in dB, and the
share of loud hops more than 6 dB quieter than the ground truth (sounds cut off) or louder (sounds
doubled).

    python harness/tools/gprb_audio.py run/qa/sfx/gt/gt-0.wav run/qa/sfx/pause/mp-0.wav
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Dict, Tuple

import numpy as np

RATE = 8000


def read_wav(path: Path) -> Tuple[np.ndarray, int]:
    """Mono float samples and the rate. The data chunk runs to the end of the file when its size
    is missing (a dump copied before Dolphin closed it)."""
    b = path.read_bytes()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a WAVE file")
    pos, rate, channels = 12, 0, 2
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack_from("<I", b, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt ":
            channels, rate = struct.unpack_from("<HI", b, body + 2)
        elif cid == b"data":
            end = body + size if 0 < size <= len(b) - body else len(b)
            n = (end - body) // (2 * channels) * channels
            x = np.frombuffer(b, dtype="<i2", count=n, offset=body).astype(np.float32) / 32768.0
            return x.reshape(-1, channels).mean(axis=1), rate
        pos = body + size + (size & 1)
    raise ValueError(f"{path}: no data chunk")


def resample(x: np.ndarray, rate: int) -> np.ndarray:
    """To RATE by averaging (the dumps are 32 kHz; 4:1 is all this needs)."""
    k = max(1, round(rate / RATE))
    n = len(x) // k * k
    return x[:n].reshape(-1, k).mean(axis=1)


def best_match(ref: np.ndarray, ref_fft: np.ndarray, ref_energy: np.ndarray, nfft: int,
               w: np.ndarray) -> Tuple[int, float]:
    """Offset in ref of the best normalized cross-correlation with w, and its value."""
    w = w - w.mean()
    norm_w = float(np.sqrt((w * w).sum()))
    if norm_w < 1e-6:
        return -1, 0.0
    corr = np.fft.irfft(ref_fft * np.conj(np.fft.rfft(w, nfft)), nfft)[: len(ref) - len(w) + 1]
    # Local energy of ref under the window (the window's mean is removed, ref's is about 0).
    e = ref_energy[len(w):] - ref_energy[: len(ref) - len(w) + 1]
    ncc = corr / (norm_w * np.sqrt(np.maximum(e, 1e-9)))
    i = int(np.argmax(ncc))
    return i, float(ncc[i])


def band_levels(x: np.ndarray, rate: int, n: int = 1024, hop: int = 512, bands: int = 32) -> np.ndarray:
    """Level in dB of `bands` log-spaced bands (60 Hz to 0.45 rate) per hop: what a listener hears,
    without the waveform's phase."""
    frames = (len(x) - n) // hop
    idx = np.arange(n)[None, :] + hop * np.arange(frames)[:, None]
    spec = np.abs(np.fft.rfft(x[idx] * np.hanning(n), axis=1)) ** 2
    freqs = np.fft.rfftfreq(n, 1 / rate)
    edges = np.geomspace(60, 0.45 * rate, bands + 1)
    out = np.stack([spec[:, (freqs >= lo) & (freqs < hi)].sum(axis=1) for lo, hi in zip(edges[:-1], edges[1:])], axis=1)
    return 10 * np.log10(out + 1e-10)


def heard(g: np.ndarray, t: np.ndarray, rate: int, offset: int) -> Dict[str, object]:
    """Compare the band levels of the test with the ground truth at `offset` samples (test sample i is
    ground truth sample i + offset), each hop against the best of the ground truth's hops within
    +-2 (32 ms: a sound may start an AX frame or a few game frames late). Loud hops only."""
    if offset >= 0:
        g = g[offset:]
    else:
        t = t[-offset:]
    n = min(len(g), len(t))
    G, T = band_levels(g[:n], rate), band_levels(t[:n], rate)
    total_g = 10 * np.log10(np.sum(10 ** (G / 10), axis=1))
    total_t = 10 * np.log10(np.sum(10 ** (T / 10), axis=1))
    loud = np.maximum(total_g, total_t) > np.max(total_g) - 40
    dist, level = [], []
    for i in np.nonzero(loud)[0]:
        lo, hi = max(0, i - 2), min(len(G), i + 3)
        d = np.mean(np.abs(G[lo:hi] - T[i]), axis=1)
        j = lo + int(np.argmin(d))
        dist.append(float(d.min()))
        level.append(float(total_t[i] - total_g[j]))
    dist, level = np.array(dist), np.array(level)
    return {
        "loud_hops": int(len(dist)),
        "spectral_diff_db_median": round(float(np.median(dist)), 2),
        "spectral_diff_db_p90": round(float(np.percentile(dist, 90)), 2),
        "quieter_6db": round(float((level < -6).mean()), 4),
        "louder_6db": round(float((level > 6).mean()), 4),
    }


def compare(gt_path: Path, test_path: Path, window_s: float, skip_s: float) -> Dict[str, object]:
    g32, gr = read_wav(gt_path)
    t32, tr = read_wav(test_path)
    g, t = resample(g32, gr), resample(t32, tr)
    nfft = 1 << int(np.ceil(np.log2(len(g) + int(window_s * RATE))))
    g_fft = np.fft.rfft(g, nfft)
    g_energy = np.concatenate(([0.0], np.cumsum(g.astype(np.float64) ** 2)))
    wl = int(window_s * RATE)
    rows = []
    start = int(skip_s * RATE)
    for pos in range(start, len(t) - wl, wl):
        w = t[pos:pos + wl]
        if float(np.abs(w).max()) < 0.01:
            continue  # silence matches anywhere
        at, ncc = best_match(g, g_fft, g_energy, nfft, w)
        if at < 0:
            continue
        rows.append((pos, ncc, at - pos))
    if not rows:
        return {"windows": 0}
    # The two dumps start at different points of the boot: the offset is the one of the first
    # windows that match well (the countdown, which runs without rollback).
    good = [r[2] for r in rows if r[1] >= 0.9][:10]
    base = float(np.median(good)) if good else rows[0][2]
    m = np.array([r[1] for r in rows])
    d = np.array([(r[2] - base) / RATE for r in rows])
    return {
        "windows": len(rows),
        "match_median": round(float(np.median(m)), 3),
        "match_p10": round(float(np.percentile(m, 10)), 3),
        "windows_matching_0.9": round(float((m >= 0.9).mean()), 3),
        "drift_end_s": round(float(d[-1]), 3),
        "drift_abs_median_s": round(float(np.median(np.abs(d))), 3),
        "test_seconds": round(len(t) / RATE, 1),
        "gt_seconds": round(len(g) / RATE, 1),
        **heard(g32, t32, gr, int(round(base * gr / RATE))),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ground_truth", type=Path)
    ap.add_argument("tests", type=Path, nargs="+")
    ap.add_argument("--window", type=float, default=0.5, help="window length in seconds")
    ap.add_argument("--skip", type=float, default=0.0, help="seconds of each test dump to skip (boot)")
    args = ap.parse_args()
    for p in args.tests:
        print(p.name, json.dumps(compare(args.ground_truth, p, args.window, args.skip)), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
