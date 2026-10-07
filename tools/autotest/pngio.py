# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# pngio - minimal PNG writer + false-colour heatmap renderer.
#
# stdlib only (zlib + struct). Writes 8-bit grayscale and RGB PNGs so the
# rig never needs cv2/PIL. Colormap is a compact jet-like gradient with a
# black floor so "no signal" stays dark on the heatmaps.
# ============================================================================
import struct
import zlib

import numpy as np


def _chunk(tag, payload):
    c = struct.pack(">I", len(payload)) + tag + payload
    return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)


def write_png(path, img):
    """img: uint8 array (h,w) grayscale or (h,w,3) RGB."""
    a = np.ascontiguousarray(img, dtype=np.uint8)
    if a.ndim == 2:
        color_type, bpp = 0, 1
        rows = a.reshape(a.shape[0], a.shape[1])
    elif a.ndim == 3 and a.shape[2] == 3:
        color_type, bpp = 2, 3
        rows = a.reshape(a.shape[0], a.shape[1] * 3)
    else:
        raise ValueError("img must be (h,w) or (h,w,3) uint8")
    h, w = a.shape[0], a.shape[1]
    ihdr = struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0)
    # filter byte 0 per scanline
    raw = b"".join(b"\x00" + rows[y].tobytes() for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr)
           + _chunk(b"IDAT", zlib.compress(raw, 6)) + _chunk(b"IEND", b""))
    with open(path, "wb") as fp:
        fp.write(png)


def read_png(path):
    """Minimal PNG decoder: 8-bit non-interlaced, colour types
    0 (gray), 2 (RGB), 3 (palette), 4 (gray+alpha), 6 (RGBA).
    Returns uint8 (h,w) or (h,w,3); alpha is dropped. Enough for
    frames dumped by this rig or by ffmpeg image2."""
    with open(path, "rb") as fp:
        data = fp.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat = 8, []
    w = h = depth = ctype = None
    palette = None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if tag == b"IHDR":
            w, h, depth, ctype, comp, filt, inter = struct.unpack(
                ">IIBBBBB", payload)
            if depth != 8 or inter != 0:
                raise ValueError("%s: unsupported PNG (depth=%d inter=%d)"
                                 % (path, depth, inter))
        elif tag == b"PLTE":
            palette = np.frombuffer(payload, np.uint8).reshape(-1, 3)
        elif tag == b"IDAT":
            idat.append(payload)
        elif tag == b"IEND":
            break
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = w * ch
    out = np.empty((h, stride), np.uint8)
    prev = np.zeros(stride, np.uint8)
    i = 0
    for y in range(h):
        ft = raw[i]
        i += 1
        line = np.frombuffer(raw[i:i + stride], np.uint8).copy()
        i += stride
        if ft == 0:
            pass
        elif ft == 1:                       # Sub
            for x in range(ch, stride):
                line[x] = (int(line[x]) + int(line[x - ch])) & 0xFF
        elif ft == 2:                       # Up
            line = (line.astype(np.uint16) + prev.astype(np.uint16)
                    ).astype(np.uint8)
        elif ft == 3:                       # Average
            for x in range(stride):
                left = int(line[x - ch]) if x >= ch else 0
                up = int(prev[x])
                line[x] = (int(line[x]) + ((left + up) >> 1)) & 0xFF
        elif ft == 4:                       # Paeth
            for x in range(stride):
                a_ = int(line[x - ch]) if x >= ch else 0
                b_ = int(prev[x])
                c_ = int(prev[x - ch]) if x >= ch else 0
                pp = a_ + b_ - c_
                pa, pb, pc = abs(pp - a_), abs(pp - b_), abs(pp - c_)
                pr = a_ if (pa <= pb and pa <= pc) else (
                    b_ if pb <= pc else c_)
                line[x] = (int(line[x]) + pr) & 0xFF
        else:
            raise ValueError("%s: bad filter %d" % (path, ft))
        out[y] = line
        prev = line
    px = out.reshape(h, w, ch)
    if ctype == 0:
        return px[:, :, 0]
    if ctype == 3:
        return palette[px[:, :, 0]]
    if ctype == 4:
        return px[:, :, 0]
    return px[:, :, :3]


def colormap(v):
    """v: float array in [0,1] -> uint8 (h,w,3). Jet-like, black at 0."""
    v = np.clip(v, 0.0, 1.0)
    r = np.clip(1.6 * v - 0.25, 0.0, 1.0)
    g = np.clip(1.6 * np.abs(v - 0.55) * -1.0 + 0.9, 0.0, 1.0)
    b = np.clip(1.2 - 1.8 * v, 0.0, 1.0)
    out = np.stack([r, g, b], axis=-1)
    # dark floor so zero reads as near-black, not deep blue
    floor = (v < 0.02)[..., None]
    out = np.where(floor, 0.0, out)
    return (out * 255.0).astype(np.uint8)


def heatmap_png(path, m, log=True, clip_pct=99.5):
    """Render a non-negative float map to a colour PNG with a 1px scale bar
    of headroom: values are normalised to the clip_pct percentile."""
    m = np.asarray(m, dtype=np.float64)
    if log:
        m = np.log1p(m)
    hi = np.percentile(m, clip_pct) if m.size else 0.0
    if hi <= 0:
        hi = float(m.max()) or 1.0
    write_png(path, colormap(m / hi))


def upscale_nn(m, target_w):
    """Nearest-neighbour upscale so small analysis maps stay readable."""
    h, w = m.shape
    if w >= target_w:
        return m
    k = max(1, int(round(target_w / w)))
    return np.repeat(np.repeat(m, k, axis=0), k, axis=1)
