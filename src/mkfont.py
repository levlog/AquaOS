#!/usr/bin/env python3
"""AquaOS: generate font.h — 8x16 one-bit-per-pixel glyphs for ASCII 32..126.
Source: DejaVu Sans Mono (fallback: any system monospace TTF or PSF console font).
"""
import os
import sys
import glob

CELL_W, CELL_H = 8, 16
FIRST, LAST = 32, 126

TTF_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/english/DejaVuSansMono.ttf",
]

PSF_CANDIDATES = [
    "/usr/share/consolefonts/default8x16.psfu.gz",
    "/usr/share/kbd/consolefonts/default8x16.psfu.gz",
    "/lib/kbd/consolefonts/default8x16.psfu.gz",
]


def from_ttf():
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return None
    for path in TTF_CANDIDATES:
        if not os.path.exists(path):
            continue
        for size in (13, 12, 11):
            try:
                font = ImageFont.truetype(path, size)
            except Exception:
                continue
            try:
                ascent, descent = font.getmetrics()
            except Exception:
                continue
            if ascent + descent > CELL_H:
                continue
            glyphs = []
            for cp in range(FIRST, LAST + 1):
                img = Image.new("L", (CELL_W, CELL_H), 0)
                ImageDraw.Draw(img).text((0, 0), chr(cp), font=font, fill=255)
                px = img.load()
                rows = []
                for y in range(CELL_H):
                    b = 0
                    for x in range(CELL_W):
                        if px[x, y] >= 128:
                            b |= 0x80 >> x
                    rows.append(b)
                glyphs.append(rows)
            if any(any(g) for g in glyphs):
                print(f"font source: {path} size {size}", file=sys.stderr)
                return glyphs
    return None


def from_psf():
    import gzip
    paths = list(PSF_CANDIDATES) + sorted(
        glob.glob("/usr/share/consolefonts/*8x16*.psf*")
        + glob.glob("/usr/share/kbd/consolefonts/*8x16*.psf*")
    )[:8]
    for path in paths:
        if not os.path.exists(path):
            continue
        try:
            op = gzip.open if path.endswith(".gz") else open
            with op(path, "rb") as f:
                data = f.read()
        except Exception:
            continue
        glyphs = None
        if data[:2] == b"\x36\x04":  # PSF1
            charsize = data[3]
            if charsize <= CELL_H:
                base = data[4:]
                glyphs = [
                    list(base[i * charsize:(i + 1) * charsize]) + [0] * (CELL_H - charsize)
                    for i in range(FIRST, min(LAST, 255) + 1)
                ]
        elif data[:4] == b"\x72\xb5\x4a\x86":  # PSF2
            import struct
            _, _, hsize, _, ngl, cs, hh, ww = struct.unpack("<8I", data[:32])
            if ww == 8 and cs <= CELL_H:
                fd = data[hsize:hsize + ngl * cs]
                glyphs = [
                    list(fd[i * cs:(i + 1) * cs]) + [0] * (CELL_H - cs)
                    for i in range(FIRST, min(LAST, ngl - 1) + 1)
                ]
        if glyphs and len(glyphs) == LAST - FIRST + 1:
            print(f"font source: {path}", file=sys.stderr)
            return glyphs
    return None


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "font.h"
    glyphs = from_ttf() or from_psf()
    if glyphs is None:
        print("ERROR: no usable font source found", file=sys.stderr)
        return 1
    with open(out_path, "w") as f:
        f.write("/* AquaOS auto-generated bitmap font (DO NOT EDIT) */\n")
        f.write("#pragma once\n")
        f.write("#define FONT_W %d\n#define FONT_H %d\n" % (CELL_W, CELL_H))
        f.write("#define FONT_FIRST %d\n#define FONT_COUNT %d\n"
                % (FIRST, LAST - FIRST + 1))
        f.write("static const unsigned char FONT_BITMAP[FONT_COUNT][FONT_H] = {\n")
        for g in glyphs:
            f.write("    {" + ",".join("0x%02x" % b for b in g) + "},\n")
        f.write("};\n")
    print(f"font.h written: {LAST - FIRST + 1} glyphs, {CELL_W}x{CELL_H}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
