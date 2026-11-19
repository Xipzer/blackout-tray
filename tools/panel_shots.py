#!/usr/bin/env python3
"""Render every panel state off-screen with `blackout.exe --panel-shot` and save PNGs.

No window is ever shown and no hardware, blackout.ini or registry is touched, so this is safe to run at any time
(run it from WSL or Git Bash on Windows; the exe has to sit on a Windows drive).

  tools/panel_shots.py <blackout.exe> <out dir> [--flat | --readme]

Without options it writes every state as a transparent PNG plus sheet.png (a contact sheet for design review).
--flat puts each PNG on a dark background, for viewers that show transparency as white.
--readme writes only the docs/panel-*.png images.
"""
import os
import subprocess
import sys

from PIL import Image, ImageDraw

BACK = (58, 62, 66)
STATES = {
    # Nothing set up: no blackout.ini, a fresh install.
    "first-run": "nostart",
    "first-run-light": "nostart light",
    "first-run-rule-on": "hotkey hover=4",
    "first-run-woken": "wakes=1",
    # Lighting through OpenRGB only, the common setup.
    "openrgb": "openrgb wakes=3",
    "openrgb-rgb-off": "openrgb rgboff nosync nostart wakes=2",
    # Everything set up.
    "all": "all hotkey wakes=5",
    "all-light": "all hotkey wakes=5 light",
    "all-rule-off": "all noesc wakes=5",
    "all-click-only": "all hotkey noesc wakes=4 hover=5",
    "all-esc-only": "all hotkey noclick wakes=4 light hover=6",
    "hover-screens": "all wakes=2 hover=1",
    "hover-row": "all wakes=2 hover=2 light",
    "hover-exit": "all wakes=2 hover=8",
    # Practical extremes: check every one for clipping, overlap and imbalance after a panel change.
    "x-long-wakes": "all hotkey longwakes",
    "x-long-wakes-light": "all hotkey longwakes light",
    "x-long-ddc": "all longddc wakes=3",
    "x-bad-path": "badpath wakes=2",
    "x-broken": "badpath nohub wakes=2",
    "x-broken-light": "badpath nohub ram longddc longwakes light",
    "x-hub-only": "link",
    "x-ram-only": "ram wakes=1",
    "x-ddc-only": "ddc hotkey",
    "x-top": "all hotkey wakes=5 top",
    "x-scale-125": "all hotkey longwakes scale=125",
    "x-scale-175": "all hotkey longwakes longddc scale=175",
}
README = {"panel": "all hotkey wakes=3", "panel-first-run": "nostart", "panel-light": "openrgb hotkey wakes=2 light"}


def winpath(path):
    if path.startswith("/mnt/"):
        return path[5].upper() + ":" + path[6:].replace("/", "\\")
    return path


def shot(exe, flags, png):
    bmp = png[:-4] + ".bmp"
    if os.path.exists(bmp):
        os.remove(bmp)
    scale = [] if "scale=" in flags else ["x2"]
    subprocess.run([exe, "--panel-shot", *flags.split(), *scale, "out=" + winpath(bmp)], check=False, cwd=os.path.dirname(exe))
    # 32-bit top-down BMP with premultiplied alpha: the rounded corner is already in the picture.
    data = open(bmp, "rb").read()
    width = int.from_bytes(data[18:22], "little", signed=True)
    height = abs(int.from_bytes(data[22:26], "little", signed=True))
    img = Image.frombuffer("RGBa", (width, height), data[int.from_bytes(data[10:14], "little"):], "raw", "BGRa", 0, 1).convert("RGBA")
    os.remove(bmp)
    return img


def main():
    exe, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    os.makedirs(out, exist_ok=True)
    if "--readme" in sys.argv:
        for name, flags in README.items():
            shot(exe, flags, os.path.join(out, name + ".png")).save(os.path.join(out, name + ".png"))
            print(name)
        return
    images = [(name, shot(exe, flags, os.path.join(out, name + ".png"))) for name, flags in STATES.items()]
    for name, img in images:
        if "--flat" in sys.argv:
            flat = Image.new("RGB", (img.width + 48, img.height + 48), BACK)
            flat.paste(img, (24, 24), img)
            img = flat
        img.save(os.path.join(out, name + ".png"))
    cols = 7
    cell_w = max(i.width for _, i in images) // 2 + 24
    cell_h = max(i.height for _, i in images) // 2 + 44
    rows = (len(images) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell_w + 24, rows * cell_h + 24), BACK)
    draw = ImageDraw.Draw(sheet)
    for n, (name, img) in enumerate(images):
        x, y = 24 + (n % cols) * cell_w, 24 + (n // cols) * cell_h
        small = img.resize((img.width // 2, img.height // 2), Image.LANCZOS)
        draw.text((x, y), name, fill=(235, 235, 235))
        sheet.paste(small, (x, y + 18), small)
    sheet.save(os.path.join(out, "sheet.png"))
    print(len(images), "states ->", os.path.join(out, "sheet.png"))


if __name__ == "__main__":
    main()
