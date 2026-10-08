// Iso-contour tracing of a coverage bitmap (marching squares with linear interpolation).
//
// The input is a coverage grid (0..1 per pixel). Contours are traced at `threshold` through the pixel centres,
// producing closed polygons in pixel coordinates (x right, y down). All contours share one orientation rule, so outer
// contours and holes come out with opposite winding; callers fix the global direction afterwards.
//
// SPDX-License-Identifier: GPL-3.0-or-later

export type Point = { x: number; y: number };
export type Contour = Point[];

/**
 * Traces the `threshold` iso-line of `coverage` (row-major, `width` x `height`). Pixels outside the grid count as 0,
 * so every contour is closed.
 */
export function traceContours(coverage: Float32Array, width: number, height: number, threshold = 0.5): Contour[] {
  // Sample grid with a 1-sample zero border: sample (i, j) is pixel (i - 1, j - 1), centred at (i - 0.5, j - 0.5).
  const sw = width + 2;
  const sh = height + 2;
  const sample = (i: number, j: number) => {
    const x = i - 1;
    const y = j - 1;
    if (x < 0 || y < 0 || x >= width || y >= height) {
      return 0;
    }
    return coverage[y * width + x];
  };
  const inside = (v: number) => v >= threshold;
  // Edge identifiers: horizontal edge (i,j)-(i+1,j) => "h:i:j"; vertical edge (i,j)-(i,j+1) => "v:i:j".
  const points = new Map<string, Point>();
  const crossing = (key: string, ax: number, ay: number, va: number, bx: number, by: number, vb: number) => {
    let p = points.get(key);
    if (!p) {
      const t = va === vb ? 0.5 : Math.min(1, Math.max(0, (threshold - va) / (vb - va)));
      p = { x: ax + (bx - ax) * t - 0.5, y: ay + (by - ay) * t - 0.5 };
      points.set(key, p);
    }
    return key;
  };
  const next = new Map<string, string>();
  for (let j = 0; j < sh - 1; j++) {
    for (let i = 0; i < sw - 1; i++) {
      const tl = sample(i, j);
      const tr = sample(i + 1, j);
      const br = sample(i + 1, j + 1);
      const bl = sample(i, j + 1);
      const corners = [inside(tl), inside(tr), inside(br), inside(bl)];
      const n = corners.filter(Boolean).length;
      if (n === 0 || n === 4) {
        continue;
      }
      // Edges in clockwise order (image space): top, right, bottom, left. Edge k goes corner k -> corner k+1.
      const edgeKeys = [
        () => crossing(`h:${i}:${j}`, i, j, tl, i + 1, j, tr),
        () => crossing(`v:${i + 1}:${j}`, i + 1, j, tr, i + 1, j + 1, br),
        () => crossing(`h:${i}:${j + 1}`, i + 1, j + 1, br, i, j + 1, bl),
        () => crossing(`v:${i}:${j}`, i, j + 1, bl, i, j, tl),
      ];
      // A crossing on edge k is an "exit" when corner k is inside, an "entry" otherwise.
      const cross: { k: number; entry: boolean }[] = [];
      for (let k = 0; k < 4; k++) {
        const a = corners[k];
        const b = corners[(k + 1) % 4];
        if (a !== b) {
          cross.push({ k, entry: !a });
        }
      }
      // Pair every entry with the next exit clockwise (inside corners lie between them), except for a saddle whose
      // centre is inside, where diagonal inside corners are joined by pairing with the previous exit instead.
      const saddle = n === 2 && corners[0] === corners[2];
      const centreInside = inside((tl + tr + br + bl) / 4);
      const m = cross.length;
      for (let c = 0; c < m; c++) {
        if (!cross[c].entry) {
          continue;
        }
        const exit = saddle && centreInside ? cross[(c - 1 + m) % m] : cross[(c + 1) % m];
        const from = edgeKeys[cross[c].k]();
        const to = edgeKeys[exit.k]();
        next.set(from, to);
      }
    }
  }
  const contours: Contour[] = [];
  const visited = new Set<string>();
  for (const start of next.keys()) {
    if (visited.has(start)) {
      continue;
    }
    const contour: Point[] = [];
    let cur: string | undefined = start;
    while (cur !== undefined && !visited.has(cur)) {
      visited.add(cur);
      contour.push(points.get(cur)!);
      cur = next.get(cur);
    }
    if (contour.length >= 3) {
      contours.push(contour);
    }
  }
  return contours;
}

/** Signed area (shoelace); positive = counter-clockwise in a y-up system. */
export function signedArea(c: Contour): number {
  let a = 0;
  for (let i = 0; i < c.length; i++) {
    const p = c[i];
    const q = c[(i + 1) % c.length];
    a += p.x * q.y - q.x * p.y;
  }
  return a / 2;
}

/** Removes duplicate and (nearly) collinear points; `tolerance` is the max deviation in contour units. */
export function simplify(c: Contour, tolerance: number): Contour {
  let pts = c.filter((p, i) => {
    const q = c[(i + 1) % c.length];
    return p.x !== q.x || p.y !== q.y;
  });
  let changed = true;
  while (changed && pts.length > 3) {
    changed = false;
    const out: Point[] = [];
    for (let i = 0; i < pts.length; i++) {
      const prev = out.length ? out[out.length - 1] : pts[pts.length - 1];
      const p = pts[i];
      const nxt = pts[(i + 1) % pts.length];
      const dx = nxt.x - prev.x;
      const dy = nxt.y - prev.y;
      const len = Math.hypot(dx, dy);
      const dist =
        len === 0 ? Math.hypot(p.x - prev.x, p.y - prev.y) : Math.abs(dx * (p.y - prev.y) - dy * (p.x - prev.x)) / len;
      // Drop p only if it lies within tolerance of the prev->next chord and between them.
      const along = len === 0 ? 0 : ((p.x - prev.x) * dx + (p.y - prev.y) * dy) / (len * len);
      if (dist <= tolerance && along > 0 && along < 1 && out.length + (pts.length - i - 1) >= 3) {
        changed = true;
        continue;
      }
      out.push(p);
    }
    pts = out;
  }
  return pts;
}
