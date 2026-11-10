#!/usr/bin/env python3
"""
Desktop preview of the overlay UI (needs Pillow and a C compiler).

    python3 tools/preview/render.py <background image> <out_dir> [crop x0 y0 x1 y1]

Scales the background (optionally cropped) to 1280x720, runs the preview program (build.sh) and
converts its raw output to PNG files in out_dir.
"""

import os
import subprocess
import sys
from PIL import Image

W, H = 1280, 720


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    im = Image.open(src).convert("RGB")
    if len(sys.argv) == 7:
        im = im.crop(tuple(int(v) for v in sys.argv[3:7]))
    im = im.resize((W, H), Image.LANCZOS)
    raw = bytearray()
    for r, g, b in im.getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        raw += bytes((v & 0xFF, v >> 8))
    bg = os.path.join(out, "background.rgb565")
    with open(bg, "wb") as f:
        f.write(raw)
    here = os.path.dirname(os.path.abspath(__file__))
    files = subprocess.check_output([os.path.join(here, "build.sh"), os.path.abspath(bg), os.path.abspath(out)],
                                    text=True).split()
    for path in files:
        with open(path, "rb") as f:
            Image.frombytes("RGB", (W, H), f.read()).save(path[:-4] + ".png")
        os.remove(path)
    os.remove(bg)


if __name__ == "__main__":
    main()
