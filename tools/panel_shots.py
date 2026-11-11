#!/usr/bin/env python3
"""Render every panel view off-screen with `blackout.exe --panel-shot` and save PNGs.

No window is ever shown, so this is safe to run at any time (run it from WSL or Git Bash on Windows).

  tools/panel_shots.py <blackout.exe> <out dir> [--readme]

Without --readme it writes every state plus sheet.png (a contact sheet for design review).
With --readme it writes only docs-ready panel-main.png and panel-settings.png.
"""
import os
import subprocess
import sys

from PIL import Image, ImageDraw

STATES = {
    "main": "",
    "main-hover": "hover=2",
    "main-rgb-off": "rgboff",
    "main-curve-off": "curveoff",
    "main-no-reading": "noread",
    "main-at-max": "atcap",
    "main-no-history": "nohist",
    "main-no-fans": "nofans",
    "main-light": "light",
    "fans": "fans",
    "fans-drag": "fans edited drag=1",
    "fans-light": "fans light",
    "fans-off": "fans notadmin",
    "main-fans-broken": "notadmin",
    "wake-no-fans": "wake nofans",
    "wake": "wake",
    "wake-on": "wake hotkey wakes",
    "wake-light": "wake hotkey wakes light",
    "system": "system",
    "system-no-admin": "system notadmin",
    "system-light": "system light",
}
README = {"panel-main": "", "panel-settings": "fans", "panel-wake": "wake hotkey wakes", "panel-system": "system"}


def winpath(path):
    if path.startswith("/mnt/"):
        return path[5].upper() + ":" + path[6:].replace("/", "\\")
    return path


def shot(exe, flags, png):
    bmp = png[:-4] + ".bmp"
    if os.path.exists(bmp):
        os.remove(bmp)
    subprocess.run([exe, "--panel-shot", *flags.split(), "x2", "out=" + winpath(bmp)], check=False, cwd=os.path.dirname(exe))
    img = Image.open(bmp).convert("RGB").convert("RGBA")
    # The real window rounds only the corner that faces the desktop (top-left): 8 px, here at 2x.
    r = 16
    mask = Image.new("L", img.size, 255)
    ImageDraw.Draw(mask).rectangle([0, 0, r, r], fill=0)
    ImageDraw.Draw(mask).pieslice([0, 0, 2 * r, 2 * r], 180, 270, fill=255)
    img.putalpha(mask)
    img.save(png)
    os.remove(bmp)
    return img


def main():
    exe, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    os.makedirs(out, exist_ok=True)
    if "--readme" in sys.argv:
        for name, flags in README.items():
            shot(exe, flags, os.path.join(out, name + ".png"))
            print(name)
        return
    images = [(name, shot(exe, flags, os.path.join(out, name + ".png"))) for name, flags in STATES.items()]
    cols, cell_w = 5, 744 // 2 + 24
    rows = (len(images) + cols - 1) // cols
    cell_h = max(i.height for _, i in images) // 2 + 44
    sheet = Image.new("RGB", (cols * cell_w + 24, rows * cell_h + 24), (58, 62, 66))
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
