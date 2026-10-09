"""Builds Brawl Online's icon: the Smash Ball (smash-ball.png) with a Wi-Fi symbol in place of the
Smash cross, and writes every size the launcher and the website use.

    python tools/icon/make-icon.py            # writes the files below
    python tools/icon/make-icon.py --preview DIR  # also writes previews on dark and light

Needs Pillow and NumPy. Drawn by this script, not generated:
  1. The cross (two rectangles over the ball, minus the orbit ring passing in front of it) is
     filled from mirror images of the ball (same distance from the centre, so the shading matches),
     blended in with a Poisson solve so no seams show.
  2. The Wi-Fi symbol (a dot and three 90-degree arcs) is rasterized from its signed distance,
     wrapped onto the sphere, in the cross's near-black.

Outputs:
  launcher/assets/icon.png   1024x1024 (Linux, BrowserWindow)
  launcher/assets/icon.ico   16-256 (Windows exe, installer; up to 48 the ball alone)
  launcher/assets/icon.icns  16-1024 (macOS app, DMG; 16 and 32 the ball alone)
  website/favicon.ico        16, 32, 48 (the ball alone, no glow)
  website/assets/icon-180.png (apple-touch-icon), icon-512.png (the same)
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# The ball in smash-ball.png (pixels): centre and radius, and the radius of its coloured part
# (inside the white rim).
CX, CY, R = 293.5, 297.0, 99.0
R_COLOUR = 89.0
# The Smash cross: vertical and horizontal bar (x0, y0, x1, y1), clipped to the ball.
# Each with its white edge highlight.
BARS = ((221, 190, 285, 405), (185, 311, 400, 340))
SCALE = 4  # the symbol is drawn at 4x the source


def load():
    im = Image.open(HERE / "smash-ball.png").convert("RGBA")
    return np.array(im).astype(np.float64)


def cross_mask(a):
    h, w = a.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    disc = np.hypot(xx - CX, yy - CY) < R * 1.01
    bars = np.zeros((h, w), bool)
    for x0, y0, x1, y1 in BARS:
        bars |= (xx >= x0) & (xx < x1) & (yy >= y0) & (yy < y1)
    # The orbit ring's white core where it crosses the bars, plus a pixel of its yellow edge.
    rgb = a[..., :3]
    # The ring runs about along x = 218 + 0.85 (y - 330) there.
    band = (np.abs(xx - (218 + 0.85 * (yy - 330))) < 6) & (yy > 300)
    core = (rgb[..., 0] > 235) & (rgb[..., 1] > 235) & (rgb[..., 2] > 170) & band
    ring = np.array(Image.fromarray((core * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))) > 0
    return bars & disc & ~ring, disc, ring


def fill_cross(a):
    """Poisson fill of the cross, guided by mirror images of the ball."""
    mask, disc, ring = cross_mask(a)
    h, w = mask.shape
    yy, xx = np.mgrid[0:h, 0:w]
    inside = disc & ~ring
    # Per pixel: the first mirror (horizontal, vertical, both) that lands on the ball, off the cross.
    sx = np.zeros((h, w), int)
    sy = np.zeros((h, w), int)
    fxm = np.zeros((h, w), bool)
    fym = np.zeros((h, w), bool)
    todo = mask.copy()
    for fx, fy in ((True, False), (False, True), (True, True)):
        tx = np.clip(np.round(2 * CX - xx if fx else xx).astype(int), 0, w - 1)
        ty = np.clip(np.round(2 * CY - yy if fy else yy).astype(int), 0, h - 1)
        ok = todo & ~mask[ty, tx] & inside[ty, tx]
        sx[ok], sy[ok], fxm[ok], fym[ok] = tx[ok], ty[ok], fx, fy
        todo &= ~ok
    assert not todo.any(), "some cross pixels have no mirror source"
    # Guide: each pixel keeps its source's gradient (towards a neighbour, the source's neighbour in
    # the mirrored direction), so the texture is copied and only its offset is solved for.
    f = a.copy()
    ys, xs = np.nonzero(mask)
    src = a[sy[ys, xs], sx[ys, xs]]
    f[ys, xs] = src
    nbrs = []
    for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
        ny, nx = ys + dy, xs + dx
        mdx = np.where(fxm[ys, xs], -dx, dx)
        mdy = np.where(fym[ys, xs], -dy, dy)
        gy = np.clip(sy[ys, xs] + mdy, 0, h - 1)
        gx = np.clip(sx[ys, xs] + mdx, 0, w - 1)
        nbrs.append((ny, nx, src - a[gy, gx]))
    for _ in range(4000):
        acc = np.zeros_like(src)
        for ny, nx, g in nbrs:
            acc += f[ny, nx] + g
        f[ys, xs] = acc / 4
    return np.clip(f, 0, 255)


def symbol_alpha(h, w, scale, arcs=3):
    """Coverage of the Wi-Fi symbol at the given scale, wrapped onto the ball."""
    cx, cy, r = CX * scale, CY * scale, R * scale
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    u = (xx + 0.5 - cx) / r
    v = (yy + 0.5 - cy) / r
    on = u * u + v * v < 1
    # Design coordinates (source pixels, origin at the ball centre): longitude and latitude.
    vv = np.clip(v, -0.999, 0.999)
    lon = np.arcsin(np.clip(u / np.sqrt(1 - vv * vv), -1, 1))
    lat = np.arcsin(vv)
    dx = R * lon
    dy = R * lat
    # Wi-Fi: dot at P, arcs 45 degrees each side of straight up.
    w_arc, gap, dot = (21.0, 11.0, 15.5) if arcs == 3 else (30.0, 16.0, 22.0)
    outer = dot + arcs * (gap + w_arc)
    py = (outer - dot) / 2 + 4  # P below the centre: the symbol sits centred, a little low
    qx, qy = dx, dy - py
    rho = np.hypot(qx, qy)
    theta = np.arctan2(qx, -qy)  # 0 = up
    sd = rho - dot
    half = np.deg2rad(45)
    ang = np.maximum(np.abs(theta) - half, 0)
    # Past an arc's radial end: the distance to the end line.
    ang_d = np.where(np.abs(theta) > half, rho * np.sin(np.minimum(ang, np.pi / 2)), -1e9)
    r_in = dot + gap
    for _ in range(arcs):
        rc = r_in + w_arc / 2
        radial = np.abs(rho - rc) - w_arc / 2
        arc = np.where(np.abs(theta) > half, np.maximum(radial, ang_d),
                       np.maximum(radial, -(half - np.abs(theta)) * rho))
        sd = np.minimum(sd, arc)
        r_in += w_arc + gap
    px_size = 1.0 / scale
    cov = np.clip(0.5 - sd / px_size, 0, 1)
    # The cross's thin white edge highlight, just outside the symbol.
    edge = np.clip(1 - np.abs(sd - 0.8) / 0.8, 0, 1) * (1 - cov)
    return cov * on, edge * on, np.hypot(u, v)


def compose(arcs=3, edges=True):
    a = load()
    filled = fill_cross(a)
    big = Image.fromarray(filled.astype(np.uint8)).resize(
        (filled.shape[1] * SCALE, filled.shape[0] * SCALE), Image.LANCZOS)
    b = np.array(big).astype(np.float64)
    h, w = b.shape[:2]
    cov, edge, dist = symbol_alpha(h, w, SCALE, arcs)
    # Keep the orbit ring in front of the symbol.
    _, _, ring = cross_mask(a)
    ring = Image.fromarray((ring * 255).astype(np.uint8)).resize((w, h), Image.BILINEAR)
    ring = np.array(ring) / 255
    cov = cov * (1 - ring)
    e = (edge * (1 - ring) * (0.85 if edges else 0))[..., None]
    b[..., :3] = b[..., :3] * (1 - e) + np.array([255, 255, 246]) * e
    # The cross's colour: near black, a little maroon towards the rim.
    t = np.clip(dist, 0, 1)[..., None] ** 2
    col = np.array([9, 1, 3]) * (1 - t) + np.array([52, 12, 24]) * t
    c = cov[..., None]
    b[..., :3] = b[..., :3] * (1 - c) + col * c
    b[..., 3] = np.maximum(b[..., 3], cov * 255)
    return Image.fromarray(np.clip(b, 0, 255).astype(np.uint8))


def square(img, side_r, size):
    """A size x size crop centred on the ball, side_r ball radii across, fading out at the edge."""
    half = R * SCALE * side_r / 2
    cx, cy = CX * SCALE, CY * SCALE
    crop = img.crop((round(cx - half), round(cy - half), round(cx + half), round(cy + half)))
    crop = crop.resize((size, size), Image.LANCZOS)
    arr = np.array(crop).astype(np.float64)
    yy, xx = np.mgrid[0:size, 0:size]
    d = np.hypot(xx + 0.5 - size / 2, yy + 0.5 - size / 2) / (size / 2)
    fade = np.clip((1.0 - d) / 0.22, 0, 1)
    fade = fade * fade * (3 - 2 * fade)
    arr[..., 3] *= fade
    return Image.fromarray(arr.astype(np.uint8))


def ball(img, size):
    """Just the coloured ball, filling a size x size square (transparent corners)."""
    half = R_COLOUR * SCALE
    cx, cy = CX * SCALE, CY * SCALE
    crop = img.crop((round(cx - half), round(cy - half), round(cx + half), round(cy + half)))
    crop = crop.resize((size, size), Image.LANCZOS)
    arr = np.array(crop).astype(np.float64)
    yy, xx = np.mgrid[0:size, 0:size]
    d = np.hypot(xx + 0.5 - size / 2, yy + 0.5 - size / 2)
    arr[..., 3] = np.clip(size / 2 - d, 0, 1) * 255
    return Image.fromarray(arr.astype(np.uint8))


def sized(img, size):
    # Small sizes crop tighter so the ball stays readable.
    return square(img, 2.2 if size <= 48 else 2.6 if size <= 64 else 3.1, size)


def main():
    img = compose()
    tiny = compose(arcs=2, edges=False)  # 16 px: two arcs, no edge highlight, so it stays legible

    def ball_at(size):
        return ball(tiny if size <= 16 else img, size)

    la = REPO / "launcher" / "assets"
    web = REPO / "website"
    sized(img, 1024).save(la / "icon.png", optimize=True)
    # Taskbar and title-bar sizes: the ball alone, like the website (the glow is a white blotch there).
    ico = [ball_at(s) for s in (16, 24, 32, 48)] + [sized(img, s) for s in (64, 128, 256)]
    ico[-1].save(la / "icon.ico", sizes=[(i.width, i.height) for i in ico], append_images=ico[:-1])
    sized(img, 1024).save(la / "icon.icns", append_images=[ball_at(s) for s in (16, 32)]
                          + [sized(img, s) for s in (64, 128, 256, 512)])
    # The website: the ball alone, no glow (it reads as a white blotch on a browser tab).
    fav = [ball_at(s) for s in (16, 32, 48)]
    fav[-1].save(web / "favicon.ico", sizes=[(i.width, i.height) for i in fav], append_images=fav[:-1])
    ball(img, 180).save(web / "assets" / "icon-180.png", optimize=True)
    ball(img, 512).save(web / "assets" / "icon-512.png", optimize=True)
    if "--preview" in sys.argv:
        out = Path(sys.argv[sys.argv.index("--preview") + 1])
        sizes = (256, 128, 64, 48, 32, 16)
        for name, color in (("dark", (32, 32, 40, 255)), ("light", (236, 236, 240, 255))):
            bg = Image.new("RGBA", (1024 + sum(sizes) + 16 * len(sizes) + 16, 1024), color)
            bg.alpha_composite(sized(img, 1024), (0, 0))
            x = 1040
            for s in sizes:
                bg.alpha_composite(sized(img, s), (x, 300 - s // 2))
                bg.alpha_composite(ball_at(s), (x, 700 - s // 2))
                x += s + 16
            bg.save(out / f"preview-{name}.png")


if __name__ == "__main__":
    main()
