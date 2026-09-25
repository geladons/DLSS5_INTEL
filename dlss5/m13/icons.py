# ============================================================================
# m13.icons - extract the icon of an exe as a tk PhotoImage (stdlib only:
# SHGetFileInfoW -> HICON -> 32bpp DIB -> zlib-packed PNG -> PhotoImage).
# Icons are cached per path; failures return the shared placeholder.
# ============================================================================
import base64
import ctypes
import struct
import zlib
from ctypes import wintypes

_u32 = ctypes.windll.user32
_g32 = ctypes.windll.gdi32
_s32 = ctypes.windll.shell32

SHGFI_ICON = 0x000000100
SHGFI_LARGEICON = 0x000000000
SHGFI_SMALLICON = 0x000000001
BI_RGB = 0
DIB_RGB_COLORS = 0
SRCCOPY = 0x00CC0020


class SHFILEINFOW(ctypes.Structure):
    _fields_ = [("hIcon", wintypes.HANDLE),
                ("iIcon", ctypes.c_int),
                ("dwAttributes", wintypes.DWORD),
                ("szDisplayName", wintypes.WCHAR * 260),
                ("szTypeName", wintypes.WCHAR * 80)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD),
                ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD),
                ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long),
                ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD)]


def _png(width, height, rgba):
    """Minimal PNG encoder (stdlib zlib): RGBA scanlines, filter 0."""
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4]
                   for y in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def exe_icon_png(path, size=32):
    """-> PNG bytes of the exe's large icon, or None on any failure."""
    info = SHFILEINFOW()
    flags = SHGFI_ICON | (SHGFI_LARGEICON if size > 16 else SHGFI_SMALLICON)
    got = _s32.SHGetFileInfoW(path, 0, ctypes.byref(info),
                              ctypes.sizeof(info), flags)
    if not got or not info.hIcon:
        return None
    hdc = hdc_mem = hbmp = old = None
    try:
        hdc = _u32.GetDC(None)
        hdc_mem = _g32.CreateCompatibleDC(hdc)
        bmi = BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = size
        bmi.bmiHeader.biHeight = -size       # top-down
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bmi.bmiHeader.biCompression = BI_RGB
        bits = ctypes.c_void_p()
        hbmp = _g32.CreateDIBSection(hdc_mem, ctypes.byref(bmi),
                                     DIB_RGB_COLORS, ctypes.byref(bits),
                                     None, 0)
        if not hbmp or not bits:
            return None
        old = _g32.SelectObject(hdc_mem, hbmp)
        ctypes.memset(bits, 0, size * size * 4)
        if not _u32.DrawIconEx(hdc_mem, 0, 0, info.hIcon, size, size, 0,
                               None, 0x0003):     # DI_NORMAL
            return None
        raw = ctypes.string_at(bits, size * size * 4)   # BGRA
        rgba = bytearray(size * size * 4)
        rgba[0::4] = raw[2::4]                  # R
        rgba[1::4] = raw[1::4]                  # G
        rgba[2::4] = raw[0::4]                  # B
        alpha = raw[3::4]
        if not any(alpha):
            alpha = b"\xff" * (size * size)     # no alpha written -> opaque
        rgba[3::4] = alpha
        return _png(size, size, bytes(rgba))
    except (OSError, ValueError):
        return None
    finally:
        if old and hdc_mem:
            _g32.SelectObject(hdc_mem, old)
        if hbmp:
            _g32.DeleteObject(hbmp)
        if hdc_mem:
            _g32.DeleteDC(hdc_mem)
        if hdc:
            _u32.ReleaseDC(None, hdc)
        if info.hIcon:
            _u32.DestroyIcon(info.hIcon)


# Shared placeholder: a simple diamond on the panel color, drawn once.
_PLACEHOLDER_PNG = None


def placeholder_png(size=32, bg=(22, 26, 35), fg=(91, 140, 255)):
    global _PLACEHOLDER_PNG
    if _PLACEHOLDER_PNG is not None:
        return _PLACEHOLDER_PNG
    rgba = bytearray(size * size * 4)
    cx = cy = size // 2
    r = size // 3
    for y in range(size):
        for x in range(size):
            inside = abs(x - cx) + abs(y - cy) <= r
            c = fg if inside else bg
            o = (y * size + x) * 4
            rgba[o:o + 4] = bytes((c[0], c[1], c[2], 255))
    _PLACEHOLDER_PNG = _png(size, size, bytes(rgba))
    return _PLACEHOLDER_PNG


class IconCache:
    """path -> tk.PhotoImage, filled lazily off the UI thread if wanted."""

    def __init__(self, size=32):
        self.size = size
        self._cache = {}

    def get(self, tk_owner, path):
        import tkinter as tk
        if path in self._cache:
            return self._cache[path]
        png = exe_icon_png(path, self.size) or placeholder_png(self.size)
        img = tk.PhotoImage(data=base64.b64encode(png), master=tk_owner)
        self._cache[path] = img
        return img
