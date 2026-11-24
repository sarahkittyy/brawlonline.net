# Discord art assets for the launcher's activity (renderer/lib/discord, docs/design/discord-activity.md):
# file name = asset key. stage_<P+ stage id>.png, char_<stock key>.png, watching.png, replay.png; 512x512.
# Made from a launcher's extracted game assets (its stage select icons and stock icons); uploaded by hand
# to the Discord application's Rich Presence > Art Assets. Usage: python make.py <out dir> [theme dir]
import os
import sys

from PIL import Image, ImageDraw

OUT = sys.argv[1]
THEME = (
    sys.argv[2]
    if len(sys.argv) > 2
    else os.path.join(os.environ.get("APPDATA", ""), "Brawl Online", "game-assets", "theme")
)
os.makedirs(OUT, exist_ok=True)
SIZE = 512
SKIP_CHARS = {"giga_bowser", "wario_man"}  # transformations, never picked

n = 0
for f in sorted(os.listdir(os.path.join(THEME, "stages"))):
    stage_id = os.path.splitext(f)[0]
    im = Image.open(os.path.join(THEME, "stages", f)).convert("RGBA")
    w, h = im.size
    side = min(w, h)
    im = im.crop(((w - side) // 2, (h - side) // 2, (w - side) // 2 + side, (h - side) // 2 + side))
    im.resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(OUT, f"stage_{stage_id}.png"))
    n += 1

for f in sorted(os.listdir(os.path.join(THEME, "stocks"))):
    key = os.path.splitext(f)[0]
    if key in SKIP_CHARS:
        continue
    stock = Image.open(os.path.join(THEME, "stocks", f)).convert("RGBA")
    canvas = Image.new("RGBA", (SIZE, SIZE), (227, 229, 232, 255))
    # Pixel art: whole-number scale, nearest neighbour, centred (Discord shows it in a circle).
    scale = (SIZE * 3 // 4) // max(stock.size)
    big = stock.resize((stock.width * scale, stock.height * scale), Image.NEAREST)
    canvas.alpha_composite(big, ((SIZE - big.width) // 2, (SIZE - big.height) // 2))
    canvas.save(os.path.join(OUT, f"char_{key}.png"))
    n += 1


def icon(name, draw_fn):
    canvas = Image.new("RGBA", (SIZE * 4, SIZE * 4), (88, 101, 242, 255))
    draw_fn(ImageDraw.Draw(canvas), SIZE * 4)
    canvas.resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(OUT, f"{name}.png"))


def eye(d, s):
    white = (255, 255, 255, 255)
    blue = (88, 101, 242, 255)
    d.ellipse((s * 0.14, s * 0.30, s * 0.86, s * 0.70), fill=white)
    d.ellipse((s * 0.36, s * 0.36, s * 0.64, s * 0.64), fill=blue)
    d.ellipse((s * 0.43, s * 0.43, s * 0.57, s * 0.57), fill=white)


def play(d, s):
    d.polygon([(s * 0.36, s * 0.26), (s * 0.36, s * 0.74), (s * 0.76, s * 0.50)], fill=(255, 255, 255, 255))


icon("watching", eye)
icon("replay", play)
n += 2
print(n, "images in", OUT)
