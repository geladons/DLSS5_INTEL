#!/usr/bin/env python
"""Convert a layer raw capture (16-byte header + BGRA) to PNG.

Usage: python raw2png.py in.raw out.png
Header: uint32 magic, w, h, format (all little-endian).
"""
import struct
import sys

from PIL import Image


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = f.read()
    magic, w, h, fmt = struct.unpack_from("<IIII", data, 0)
    payload = data[16:]
    need = w * h * 4
    if len(payload) < need:
        print("truncated: %d bytes, need %d (%dx%d)" % (len(payload), need, w, h))
        sys.exit(1)
    img = Image.frombytes("RGBA", (w, h), payload[:need])
    # BGRA in memory -> show as RGB
    b, g, r, a = img.split()
    Image.merge("RGB", (r, g, b)).save(dst)
    print("ok %dx%d fmt=%d magic=0x%08X -> %s" % (w, h, fmt, magic, dst))


if __name__ == "__main__":
    main()
