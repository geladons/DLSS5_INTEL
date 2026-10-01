# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.media_img - pure-python image I/O for the Media tab. There is NO PIL on
# this host, so BMP (24/32-bit uncompressed BI_RGB) and PNG (8-bit RGB/RGBA,
# non-interlaced, all 5 scanline filters) are decoded/encoded with stdlib
# only (zlib + struct). numpy is used opportunistically for channel shuffling
# and preview downscale, never required. JPEG decode needs ffmpeg (it is the
# only codec we deliberately do not reimplement); encoding targets are PNG
# and BMP only.
#
# Pixel format across the module: BGRA bytes (the d5c_chain engine ABI
# format), row-major, top-down, alpha byte last.
# ============================================================================
import base64
import binascii
import json
import os
import struct
import subprocess
import zlib

try:
    import numpy as _np
except ImportError:                      # numpy is present on the host but
    _np = None                           # never a hard dependency here


class ImageError(Exception):
    """Raised for any decode/encode problem; the UI renders .args[0]."""


# ----------------------------------------------------------------------------
# PNG
# ----------------------------------------------------------------------------
PNG_SIG = b"\x89PNG\r\n\x1a\n"

_PNG_CTYPE_BPP = {2: 3, 6: 4}            # RGB, RGBA (8-bit only)


def _paeth(a, b, c):
    """PNG Paeth predictor: nearest of (left, up, up-left) to a+b-c."""
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter(raw, w, h, bpp):
    """Reverse the 5 PNG scanline filters into a flat pixel bytearray.
    Filters 1/2 are vectorized with numpy when available; 3/4 are sequential
    per-byte (their dependency chain inside a row cannot be vectorized)."""
    stride = w * bpp
    if len(raw) < h * (stride + 1):
        raise ImageError("png: truncated image data")
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        row = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 0:
            pass
        elif f == 1:                       # Sub: cumsum per channel mod 256
            if _np is not None:
                a = _np.frombuffer(bytes(row), dtype=_np.uint8)
                a = a.reshape(-1, bpp).astype(_np.uint16)
                a = _np.cumsum(a, axis=0) & 0xFF
                row = bytearray(a.astype(_np.uint8).tobytes())
            else:
                for i in range(bpp, stride):
                    row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif f == 2:                       # Up
            if _np is not None:
                # uint8 + uint8 wraps mod 256 natively - exactly PNG's rule
                a = _np.frombuffer(bytes(row), dtype=_np.uint8)
                a = a + _np.frombuffer(bytes(prev), dtype=_np.uint8)
                row = bytearray(a.tobytes())
            else:
                for i in range(stride):
                    row[i] = (row[i] + prev[i]) & 0xFF
        elif f == 3:                       # Average
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:                       # Paeth
            for i in range(stride):
                a = row[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + _paeth(a, b, c)) & 0xFF
        else:
            raise ImageError("png: unknown filter %d" % f)
        out[y * stride:(y + 1) * stride] = row
        prev = row
    return out


def _to_bgra(px, w, h, bpp):
    """RGB(A) flat bytes -> BGRA bytes (alpha forced to 255 for RGB)."""
    n = w * h
    if _np is not None:
        arr = _np.frombuffer(bytes(px), dtype=_np.uint8).reshape(h, w, bpp)
        if bpp == 4:
            return arr[:, :, [2, 1, 0, 3]].tobytes()
        out = _np.empty((h, w, 4), dtype=_np.uint8)
        out[:, :, :3] = arr[:, :, [2, 1, 0]]
        out[:, :, 3] = 255
        return out.tobytes()
    out = bytearray(n * 4)
    if bpp == 4:
        out[0::4] = px[2::4]               # B
        out[1::4] = px[1::4]               # G
        out[2::4] = px[0::4]               # R
        out[3::4] = px[3::4]               # A
    else:
        out[0::4] = px[2::3]
        out[1::4] = px[1::3]
        out[2::4] = px[0::3]
        out[3::4] = b"\xff" * n
    return bytes(out)


def _from_bgra(bgra, w, h, bpp):
    """BGRA flat bytes -> RGB(A); encoder helper (kept for completeness)."""
    if _np is not None:
        arr = _np.frombuffer(bgra, dtype=_np.uint8).reshape(h, w, 4)
        if bpp == 4:
            return arr[:, :, [2, 1, 0, 3]].tobytes()
        return arr[:, :, [2, 1, 0]].tobytes()
    if bpp == 4:
        out = bytearray(w * h * 4)
        out[0::4] = bgra[2::4]
        out[1::4] = bgra[1::4]
        out[2::4] = bgra[0::4]
        out[3::4] = bgra[3::4]
        return bytes(out)
    out = bytearray(w * h * 3)
    out[0::3] = bgra[2::4]
    out[1::3] = bgra[1::4]
    out[2::3] = bgra[0::4]
    return bytes(out)


def _chunk(ctype, payload):
    """One PNG chunk: length + type + data + crc32(type+data)."""
    return (struct.pack(">I", len(payload)) + ctype + payload +
            struct.pack(">I", binascii.crc32(ctype + payload) & 0xFFFFFFFF))


def png_decode(data):
    """PNG bytes -> (w, h, bgra bytes). 8-bit RGB/RGBA non-interlaced only."""
    if not data.startswith(PNG_SIG):
        raise ImageError("png: bad signature")
    pos = len(PNG_SIG)
    w = h = bpp = None
    idat = bytearray()
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        pos += 8
        payload = data[pos:pos + length]
        pos += length + 4                  # skip crc
        if ctype == b"IHDR":
            (w, h, depth, ctyp, _comp, _filt, inter) = \
                struct.unpack(">IIBBBBB", payload)
            if depth != 8:
                raise ImageError("png: bit depth %d unsupported" % depth)
            if ctyp not in _PNG_CTYPE_BPP:
                raise ImageError("png: color type %d unsupported" % ctyp)
            if inter:
                raise ImageError("png: interlaced unsupported")
            bpp = _PNG_CTYPE_BPP[ctyp]
        elif ctype == b"IDAT":
            idat += payload
        elif ctype == b"IEND":
            break
    if w is None or not idat:
        raise ImageError("png: missing IHDR/IDAT")
    try:
        raw = zlib.decompress(bytes(idat))
    except zlib.error as e:
        raise ImageError("png: zlib: %s" % e)
    px = _unfilter(raw, w, h, bpp)
    return w, h, _to_bgra(px, w, h, bpp)


def png_encode(w, h, bgra, level=6):
    """BGRA bytes -> a REAL RGBA PNG (color type 6, every row filter 0);
    the channel swap happens here so the file is a standards-compliant
    RGBA image any viewer can open."""
    rgba = _from_bgra(bgra, w, h, 4)
    stride = w * 4
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return (PNG_SIG + _chunk(b"IHDR", ihdr) +
            _chunk(b"IDAT", zlib.compress(bytes(raw), level)) +
            _chunk(b"IEND", b""))


def png_b64(w, h, bgra, level=6):
    """BGRA -> base64 PNG string for tk.PhotoImage(data=...)."""
    return base64.b64encode(png_encode(w, h, bgra, level)).decode("ascii")


# ----------------------------------------------------------------------------
# BMP
# ----------------------------------------------------------------------------
def bmp_decode(data):
    """BMP bytes -> (w, h, bgra). Uncompressed BI_RGB 24/32-bit only."""
    if len(data) < 54 or data[:2] != b"BM":
        raise ImageError("bmp: bad signature")
    off = struct.unpack_from("<I", data, 10)[0]
    hdr = struct.unpack_from("<I", data, 14)[0]
    if hdr < 40:
        raise ImageError("bmp: header too small")
    w = struct.unpack_from("<i", data, 18)[0]
    h_raw = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    comp = struct.unpack_from("<I", data, 30)[0]
    if comp != 0:
        raise ImageError("bmp: compression %d unsupported" % comp)
    if bpp not in (24, 32):
        raise ImageError("bmp: bpp %d unsupported" % bpp)
    top_down = h_raw < 0
    h = abs(h_raw)
    stride = ((w * bpp + 31) // 32) * 4    # rows padded to 4 bytes
    if len(data) < off + stride * h:
        raise ImageError("bmp: truncated pixel data")
    bpp_b = bpp // 8
    out = bytearray(w * h * 4)
    for y in range(h):
        src_y = y if top_down else (h - 1 - y)
        row = data[off + src_y * stride: off + src_y * stride + w * bpp_b]
        base = y * w * 4
        end = base + w * 4
        if bpp_b == 4:
            out[base:end] = row[:w * 4]
            # BI_RGB 32-bit stores no real alpha; zero alpha would preview
            # as fully transparent, so force opaque.
            out[base + 3:end:4] = b"\xff" * w
        else:
            out[base:end:4] = row[0::3]
            out[base + 1:end:4] = row[1::3]
            out[base + 2:end:4] = row[2::3]
            out[base + 3:end:4] = b"\xff" * w
    return w, h, bytes(out)


def bmp_encode(w, h, bgra):
    """BGRA bytes -> 32-bit BI_RGB BMP (bottom-up, no compression)."""
    img = w * h * 4
    fh = b"BM" + struct.pack("<IHHI", 54 + img, 0, 0, 54)
    ih = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, img,
                     2835, 2835, 0, 0)
    out = bytearray(fh + ih)
    stride = w * 4
    for y in range(h - 1, -1, -1):
        out += bgra[y * stride:(y + 1) * stride]
    return bytes(out)


# ----------------------------------------------------------------------------
# dispatch
# ----------------------------------------------------------------------------
def _ext(path):
    return os.path.splitext(path)[1].lower()


class PhotoIO:
    """Load/save entry point. `ffm` is a media.Ffmpeg-like resolver (only
    consulted for JPEG / otherwise-unknown extensions) and stays optional so
    tests and pure-BMP/PNG flows never need ffmpeg."""

    @staticmethod
    def load(path, ffm=None):
        with open(path, "rb") as f:
            head = f.read(16)
        if head.startswith(PNG_SIG):
            return PhotoIO.load_png(path)
        if head[:2] == b"BM":
            return PhotoIO.load_bmp(path)
        if head[:3] == b"\xff\xd8\xff" or _ext(path) in (".jpg", ".jpeg"):
            return PhotoIO.load_jpeg(path, ffm)
        raise ImageError("unsupported image: %s" % os.path.basename(path))

    @staticmethod
    def load_png(path):
        with open(path, "rb") as f:
            return png_decode(f.read())

    @staticmethod
    def load_bmp(path):
        with open(path, "rb") as f:
            return bmp_decode(f.read())

    @staticmethod
    def load_jpeg(path, ffm):
        """JPEG decode goes through ffmpeg -> rawvideo bgra pipe. ffprobe
        gives the dimensions first (rawvideo carries no header)."""
        if ffm is None:
            raise ImageError("jpeg needs ffmpeg (download it below)")
        ffprobe, ffmpeg = ffm.ffprobe(), ffm.resolve()
        if not ffmpeg or not ffprobe:
            raise ImageError("jpeg needs ffmpeg (download it below)")
        info = subprocess.check_output(
            [ffprobe, "-v", "quiet", "-print_format", "json",
             "-show_streams", path])
        doc = json.loads(info.decode("utf-8", "replace"))
        w = h = None
        for s in doc.get("streams", []):
            if s.get("codec_type") == "video":
                w, h = int(s["width"]), int(s["height"])
                break
        if not w:
            raise ImageError("ffprobe: no video stream in %s" % path)
        raw = subprocess.check_output(
            [ffmpeg, "-v", "error", "-i", path,
             "-f", "rawvideo", "-pix_fmt", "bgra", "-"])
        if len(raw) < w * h * 4:
            raise ImageError("jpeg: short decode (%d bytes)" % len(raw))
        return w, h, raw[:w * h * 4]

    @staticmethod
    def save(path, w, h, bgra):
        """Save by extension: .bmp -> BMP32, anything else -> PNG."""
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        if _ext(path) == ".bmp":
            data = bmp_encode(w, h, bgra)
        else:
            data = png_encode(w, h, bgra)
        with open(path, "wb") as f:
            f.write(data)
        return path


# ----------------------------------------------------------------------------
# preview helpers (numpy fast path, strided fallback)
# ----------------------------------------------------------------------------
def downscale_nearest(w, h, bgra, max_w, max_h):
    """Nearest-neighbour downscale to fit (max_w, max_h); returns
    (w2, h2, bgra2). Integer steps only - cheap, aspect-preserving."""
    if w <= 0 or h <= 0:
        return w, h, bgra
    step = max((w + max_w - 1) // max_w, (h + max_h - 1) // max_h, 1)
    if step <= 1:
        return w, h, bgra
    w2, h2 = max(1, w // step), max(1, h // step)
    if _np is not None:
        arr = _np.frombuffer(bgra, dtype=_np.uint8).reshape(h, w, 4)
        small = arr[::step, ::step][:h2, :w2]
        return w2, h2, small.copy().tobytes()
    stride = w * 4
    out = bytearray(w2 * h2 * 4)
    for y in range(h2):
        row = bgra[y * step * stride:(y * step + 1) * stride]
        out[y * w2 * 4:(y + 1) * w2 * 4] = _row_sample(row, w, w2, step)
    return w2, h2, bytes(out)


def _row_sample(row, w, w2, step):
    """Pick every `step`-th pixel (4 bytes) from one row, pure python."""
    out = bytearray(w2 * 4)
    for x in range(w2):
        out[x * 4:x * 4 + 4] = row[x * step * 4:x * step * 4 + 4]
    return bytes(out)
