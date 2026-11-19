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
    "main-override": "override",
    "main-override-hot": "ovhot",
    "main-override-curve-off": "override curveoff",
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
    "gpu": "gpu",
    "gpu-light": "gpu light",
    "gpu-view": "gpu profiles=3 view=2",
    "gpu-view-light": "gpu profiles=3 view=2 light hover=32",
    "gpu-hover-keep": "gpu profiles=3 hover=33",
    "gpu-hover-xbar": "gpu profiles=3 light hover=31",
    "gpu-new": "gpu profiles=2 view=3",
    "gpu-armed": "gpu profiles=2 armed hover=33",
    "gpu-busy": "gpu profiles=2 busy",
    "gpu-reading": "gpu profiles=2 view=3 reading",
    "gpu-naming": "gpu profiles=3 view=3 naming",
    "gpu-hover-name": "gpu profiles=2 longname hover=35",
    "main-gpu": "withgpu",
    "main-gpu-light": "withgpu light hover=34",
    "main-gpu-only": "withgpu nofans",
    # Practical extremes: check every one for clipping, overlap and imbalance after a panel change.
    "x-main-cold": "temp=99 names",
    "x-main-hot": "temp=452 rpm=9990 longnames board",
    "x-main-top": "top",
    "x-main-why4": "fanwhy=4",
    "x-main-gpu-long": "withgpu longname slot=2 gpustate=-104 what=1 fanwhy=4",
    "x-main-gpu-xbar": "withgpu profiles=0 nofans",
    "x-main-gpu-unsent": "withgpu profiles=1 noxbar unsent nofans",
    "x-main-ov100": "withgpu profiles=2 override ovduty=100 temp=452 rpm=9990",
    "x-main-ov20": "ovduty=20 light",
    "x-fans-cap": "fans max=60 temp=452 edited",
    "x-fans-ends": "fans max=30 hyst=0 withgpu",
    "x-fans-ends-hi": "fans max=100 hyst=5 board light hover=12",
    "x-fans-why4": "fans fanwhy=4",
    "x-fans-why6": "fans fanwhy=6",
    "x-wake-long": "wake hotkey longwakes",
    "x-system-long": "system fanwhy=4 longstatus",
    "x-system-why5": "system fanwhy=5 withgpu",
    "x-system-board": "system board temp=452",
    "x-gpu-long5": "gpu longname slot=2 mem=-1000 power=100",
    "x-gpu-5named": "gpu fivenames",
    "x-gpu-long5-view": "gpu longname slot=2 view=3 hover=28",
    "x-gpu-3": "gpu profiles=3 longname slot=3 mem=0 power=104 xbar=0",
    "x-gpu-2": "gpu profiles=2 longname slot=2 xbar=500",
    "x-gpu-1": "gpu profiles=1 longname",
    "x-gpu-wide": "gpu profiles=2 xbar=1000 mem=-1000 power=121 hover=30",
    "x-gpu-0": "gpu profiles=0",
    "x-gpu-0-noxbar": "gpu profiles=0 noxbar unsent",
    "x-gpu-noxbar": "gpu profiles=2 noxbar",
    "x-gpu-unsent": "gpu profiles=2 unsent nobase",
    "x-gpu-mismatch": "gpu profiles=2 mismatch gpustate=-1000",
    "x-gpu-refused": "gpu profiles=2 gpustate=-104 what=1",
    "x-gpu-changed": "gpu profiles=2 gpustate=-1001",
    "x-gpu-nodriver": "gpu profiles=2 gpustate=-2",
    "x-gpu-xbar-refused": "gpu profiles=2 xbarstate=-104",
    "x-gpu-notadmin": "gpu profiles=2 notadmin gpustate=-137",
    "x-gpu-keptfail": "gpu profiles=2 keptfail",
}
README = {"panel-main": "withgpu profiles=2", "panel-settings": "fans withgpu", "panel-wake": "wake hotkey wakes withgpu", "panel-system": "system withgpu", "panel-gpu": "gpu profiles=2"}


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
    cols, cell_w = 8, 744 // 2 + 24
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
