#!/usr/bin/env python3
"""AquaOS: convert the wallpaper image into a raw RGB file (1920x1080)."""
import sys
from PIL import Image

WALL_W, WALL_H = 1920, 1080


def main():
    src, dst = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGB")
    sw, sh = im.size
    scale = max(WALL_W / sw, WALL_H / sh)
    if scale != 1.0:
        im = im.resize((max(WALL_W, round(sw * scale)), max(WALL_H, round(sh * scale))),
                       Image.LANCZOS)
    x = (im.width - WALL_W) // 2
    y = (im.height - WALL_H) // 2
    im = im.crop((x, y, x + WALL_W, y + WALL_H))
    with open(dst, "wb") as f:
        f.write(im.tobytes())
    print(f"wallpaper.raw: {src} -> {dst} ({WALL_W}x{WALL_H} RGB)")


if __name__ == "__main__":
    main()
