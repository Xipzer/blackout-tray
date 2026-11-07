from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64]


def draw(n):
    s = 8
    img = Image.new("RGBA", (n * s, n * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = max(1, round(n / 14)) * s
    white = (255, 255, 255, 255)
    # Screen outline
    left, top, right, bottom = 0.06 * n * s, 0.14 * n * s, 0.94 * n * s, 0.70 * n * s
    d.rounded_rectangle([left, top, right, bottom], radius=0.08 * n * s, outline=white, width=w)
    # Stand
    cx = n * s / 2
    d.rectangle([cx - w / 2, bottom, cx + w / 2, 0.84 * n * s], fill=white)
    d.rounded_rectangle([0.30 * n * s, 0.84 * n * s - w / 2, 0.70 * n * s, 0.84 * n * s + w / 2], radius=w / 2, fill=white)
    # Crescent moon inside the screen
    my, r = (top + bottom) / 2, 0.15 * n * s
    d.ellipse([cx - r, my - r, cx + r, my + r], fill=white)
    o = 0.5 * r
    d.ellipse([cx - r + o, my - r - o * 0.6, cx + r + o, my + r - o * 0.6], fill=(0, 0, 0, 0))
    return img.resize((n, n), Image.LANCZOS)


imgs = [draw(n) for n in SIZES]
import sys
out = sys.argv[1] if len(sys.argv) > 1 else "blackout.ico"
imgs[-1].save(out, sizes=[(n, n) for n in SIZES], append_images=imgs[:-1])

