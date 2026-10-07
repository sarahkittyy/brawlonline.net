#!/usr/bin/env python3
"""Tile screenshots into one image for quick review: montage.py OUT.png IN1.png IN2.png ... [--cols 2 --width 900]"""
import argparse
from PIL import Image, ImageDraw
ap = argparse.ArgumentParser()
ap.add_argument("out"); ap.add_argument("inputs", nargs="+")
ap.add_argument("--cols", type=int, default=2); ap.add_argument("--width", type=int, default=900)
a = ap.parse_args()
ims = [Image.open(p).convert("RGB") for p in a.inputs]
w = a.width; h = int(ims[0].height * w / ims[0].width)
rows = (len(ims) + a.cols - 1) // a.cols
sheet = Image.new("RGB", (w * a.cols, (h + 24) * rows), "black")
d = ImageDraw.Draw(sheet)
for i, (im, p) in enumerate(zip(ims, a.inputs)):
    x, y = (i % a.cols) * w, (i // a.cols) * (h + 24)
    sheet.paste(im.resize((w, h)), (x, y + 24))
    d.text((x + 6, y + 6), p.replace("\\", "/").split("/")[-1], fill="white")
sheet.save(a.out)
