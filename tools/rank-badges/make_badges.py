# Rank badges (docs/design/ranked-tiers.md §5): bakes each division's numeral onto Layerlab's 2D League Icon
# Pack emblems, procedurally (PIL drawing, no generative tools; the pack's license forbids any generative-AI use).
#
#   python make_badges.py <pack>/512 <BarlowCondensed-Black.ttf> <out>
#
# <pack>/512 holds League_1_Bronze.Png ... League_7_Legendary.Png (Platinum is our Emerald). The font is the
# upright cut of the website's (Barlow Condensed Black, OFL): google/fonts ofl/barlowcondensed/
# BarlowCondensed-Black.ttf. Not the website's italic: slanted numerals looked wrong on the plaque. Writes <out>/256/<tier>-<division>.png with Roman numerals (the launcher's
# large badge) and <out>/128/... with a bigger plaque and bars (small sizes: the launcher's rows and the
# website). Copy 256 to launcher/src/renderer/components/rank_badge/images, 128 to .../images/small and
# website/assets/ranks. Those folders are not in git (the pack's license): builds fetch them from
# production, so a new set goes there too (docs/design/ranked-tiers.md §5.1, fetch_badges.sh).
import sys, os
from PIL import Image, ImageDraw, ImageFont, ImageFilter
src, font_path, out = sys.argv[1:4]
TIERS = ["bronze","silver","gold","emerald","diamond","master","legendary"]
# plaque gradient top, bottom, ink (the tag colours)
COL = {
 "bronze": ("#f0c48a","#b07a42","#3b2412"), "silver": ("#f2f3f3","#9aa3a6","#262b2d"),
 "gold": ("#fff0a0","#d9a12c","#4a2d05"), "emerald": ("#8ff0d4","#2a9a70","#0b3022"),
 "diamond": ("#c4f6fd","#33b4ea","#082f48"), "master": ("#ff8c7d","#c7312c","#ffffff"),
}
NUM = {1: "I", 2: "II", 3: "III"}
S = 4  # supersample
def hexrgb(h): return tuple(int(h[i:i+2],16) for i in (1,3,5))
def plaque(tier, div, size, small=False):
    W = size * S
    top, bot, ink = (hexrgb(c) for c in COL[tier])
    layer = Image.new("RGBA", (W, W), (0,0,0,0))
    pw, ph = (int(W*0.60), int(W*0.34)) if small else (int(W*0.46), int(W*0.235))
    cx, cy = (W//2, int(W*0.79)) if small else (W//2, int(W*0.825))
    x0, y0 = cx - pw//2, cy - ph//2
    r = int(ph*0.32); ol = int(W*0.026)
    # outline + drop shadow
    sh = Image.new("RGBA", (W, W), (0,0,0,0)); d = ImageDraw.Draw(sh)
    d.rounded_rectangle((x0-ol, y0-ol+int(W*0.012), x0+pw+ol, y0+ph+ol+int(W*0.012)), r+ol, fill=(0,0,0,110))
    sh = sh.filter(ImageFilter.GaussianBlur(W*0.01)); layer.alpha_composite(sh)
    d = ImageDraw.Draw(layer)
    d.rounded_rectangle((x0-ol, y0-ol, x0+pw+ol, y0+ph+ol), r+ol, fill=(20,20,20,255))
    # gradient fill
    g = Image.new("RGBA", (pw, ph))
    for y in range(ph):
        t = y/(ph-1); c = tuple(int(top[i]*(1-t)+bot[i]*t) for i in range(3))
        ImageDraw.Draw(g).line([(0,y),(pw,y)], fill=c+(255,))
    m = Image.new("L", (pw, ph), 0); ImageDraw.Draw(m).rounded_rectangle((0,0,pw-1,ph-1), r, fill=255)
    layer.paste(g, (x0,y0), m)
    # top highlight
    hl = Image.new("RGBA", (W, W), (0,0,0,0))
    ImageDraw.Draw(hl).rounded_rectangle((x0+int(pw*0.12), y0+int(ph*0.08), x0+pw-int(pw*0.12), y0+int(ph*0.16)), int(ph*0.04), fill=(255,255,255,80))
    layer.alpha_composite(hl)
    shade = (255,255,255,110) if ink != (255,255,255) else (60,0,0,170)
    if small:
        # upright bars read better than letters at 30-40 px
        bw, gap, bh = int(ph*0.17), int(ph*0.14), int(ph*0.64)
        total = div*bw + (div-1)*gap
        x = cx - total//2
        for k in range(div):
            d.rectangle((x, cy-bh//2+int(W*0.006), x+bw, cy+bh//2+int(W*0.006)), fill=shade)
            d.rectangle((x, cy-bh//2, x+bw, cy+bh//2), fill=ink+(255,))
            x += bw + gap
        return layer.resize((size, size), Image.LANCZOS)
    # numeral
    f = ImageFont.truetype(font_path, int(ph*1.02))
    text = NUM[div]
    bb = d.textbbox((0,0), text, font=f)
    tw, th = bb[2]-bb[0], bb[3]-bb[1]
    tx, ty = cx - tw/2 - bb[0], cy - th/2 - bb[1]
    shade = (255,255,255,110) if ink != (255,255,255) else (60,0,0,170)
    d.text((tx, ty+int(W*0.006)), text, font=f, fill=shade)
    d.text((tx, ty), text, font=f, fill=ink+(255,))
    return layer.resize((size, size), Image.LANCZOS)
for i, tier in enumerate(TIERS):
    base = Image.open(os.path.join(src, f"League_{i+1}_{'Platinum' if tier == 'emerald' else tier.capitalize()}.Png")).convert("RGBA")
    divs = [None] if tier == "legendary" else [3,2,1]
    for div in divs:
        for size in (256, 128):
            im = base.resize((size,size), Image.LANCZOS) if base.size[0] != size else base.copy()
            if div: im.alpha_composite(plaque(tier, div, size, small=(size == 128)))
            name = tier if not div else f"{tier}-{div}"
            os.makedirs(os.path.join(out, str(size)), exist_ok=True)
            im.save(os.path.join(out, str(size), name + ".png"), optimize=True)
print("done")
