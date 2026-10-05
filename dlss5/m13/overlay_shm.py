# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_shm - shared-memory transport between the m13 overlay and the
# in-game Vulkan layer (m11-layer\nr_ui.c). The layer lives INSIDE the game
# process and opens the section lazily; this module is the producer side:
# m13 creates the mapping, publishes the panel bitmap + flags + heartbeat
# and drains the input event ring the layer's wndproc filter fills.
#
# Frozen ABI (mirrors nr_ui.c verbatim - never change one side alone):
#   offset    0  header, 64 B (16 x u32):
#     [0]  u32 magic        = 0x3049554E ('NUI0')
#     [1]  u32 seq          seqlock: ODD while the producer writes, EVEN
#                          once (w,h,dst,bitmap) is a coherent set again
#     [2]  u32 w            bitmap extent (cap 1024x1200)
#     [3]  u32 h
#     [4]  i32 dst_x        target rect, game CLIENT coords
#     [5]  i32 dst_y
#     [6]  u32 flags        bit0 ui_visible (ALSO the layer's input-capture
#                          flag), bit1 cursor_visible (sprite slot valid)
#     [7]  i32 cursor_x     logical cursor position, written BY THE LAYER
#     [8]  i32 cursor_y     (readable for diagnostics via cursor_pos())
#     [9]  u32 heartbeat    producer increments EVERY publish tick; the
#                          layer hides the UI when it stalls > ~2 s
#     [10] u32 evt_head     ring write index (layer produces)
#     [11] u32 evt_tail     ring read index (m13 consumes)
#     [12] u32 evt_dropped  producer-side overflow counter
#     [13..15] u32 reserved
#   offset   64  event ring: 256 x 16 B { u32 type; i32 a; i32 b; u32 c }
#     types: 1=move(a=x, b=y client coords)
#            2=btn_down / 3=btn_up (a: 0=L, 1=R, 2=M, 3=X1, 4=X2)
#            4=wheel (a=notches, c: 0=vertical, 1=horizontal)
#            5=key_down / 6=key_up (a=Win32 VK code)
#            7=char (a=UTF-16 code unit / BMP codepoint)
#   offset 4160  cursor sprite slot: 32x32 RGBA straight-alpha
#   offset 8256  BGRA bitmap payload, w*h*4 straight-alpha
# Total = 8256 + 1024*1200*4 = 4923456 bytes.
#
# Threading: every public method takes the instance lock - the publish
# worker thread writes bitmaps while the Tk drain pump consumes events
# and close() can unmap underneath either of them.
# ============================================================================
import ctypes
import struct
import threading
from ctypes import wintypes

_k32 = ctypes.windll.kernel32

PAGE_READWRITE = 0x04
FILE_MAP_ALL_ACCESS = 0x000F001F          # SECTION_ALL_ACCESS
_INVALID_HANDLE = ctypes.c_void_p(-1).value

SECTION_PREFIX = "Local\\DLSS5_UI_"       # + <game pid>

MAGIC = 0x3049554E                        # 'NUI0'
HDR_SIZE = 64
EVT_OFF = 64
EVT_COUNT = 256
EVT_SIZE = 16
CURSOR_OFF = EVT_OFF + EVT_COUNT * EVT_SIZE       # 4160
CURSOR_W = CURSOR_H = 32
CURSOR_SIZE = CURSOR_W * CURSOR_H * 4             # 4096
BITMAP_OFF = CURSOR_OFF + CURSOR_SIZE             # 8256
MAX_W, MAX_H = 1024, 1200
TOTAL_SIZE = BITMAP_OFF + MAX_W * MAX_H * 4       # 4923456

FLAG_VISIBLE = 0x1
FLAG_CURSOR = 0x2

EVT_MOVE = 1            # a=x, b=y (game client coords)
EVT_BTNDN = 2           # a=button: 0=L, 1=R, 2=M, 3=X1, 4=X2
EVT_BTNUP = 3
EVT_WHEEL = 4           # a=notches (+/-), c: 0=vertical 1=horizontal
EVT_KEYDN = 5           # a=Win32 VK code
EVT_KEYUP = 6
EVT_CHAR = 7            # a=UTF-16 code unit (BMP codepoint)

_EVT = struct.Struct("<IiiI")


class _Header(ctypes.Structure):
    """Header image over the mapped section - 16 u32 = 64 B."""
    _fields_ = [("magic", wintypes.DWORD),
                ("seq", wintypes.DWORD),
                ("w", wintypes.DWORD),
                ("h", wintypes.DWORD),
                ("dst_x", ctypes.c_int32),
                ("dst_y", ctypes.c_int32),
                ("flags", wintypes.DWORD),
                ("cursor_x", ctypes.c_int32),
                ("cursor_y", ctypes.c_int32),
                ("heartbeat", wintypes.DWORD),
                ("evt_head", wintypes.DWORD),
                ("evt_tail", wintypes.DWORD),
                ("evt_dropped", wintypes.DWORD),
                ("reserved", wintypes.DWORD * 3)]


assert ctypes.sizeof(_Header) == HDR_SIZE
assert ctypes.sizeof(_Header) + EVT_COUNT * EVT_SIZE == CURSOR_OFF

_k32.CreateFileMappingW.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                    wintypes.DWORD, wintypes.DWORD,
                                    wintypes.DWORD, wintypes.LPCWSTR]
_k32.CreateFileMappingW.restype = wintypes.HANDLE
_k32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                               wintypes.DWORD, wintypes.DWORD,
                               ctypes.c_size_t]
_k32.MapViewOfFile.restype = ctypes.c_void_p
_k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
_k32.UnmapViewOfFile.restype = wintypes.BOOL
# Shared kernel32 WinDLL: HANDLE argtype also fixes callers in
# processes.py that pass raw ints/c_void_p through the same dll object.
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_k32.CloseHandle.restype = wintypes.BOOL


class ShmUI:
    """Producer side of "Local\\DLSS5_UI_<pid>".

    open() creates-or-opens the section (the layer never creates it, so a
    missing/broken layer simply never consumes). write_bitmap() publishes
    a frame under the seqlock, bump_heartbeat() proves producer liveness
    and poll_events() drains the input ring. All methods are lock-guarded
    so the publish worker, the Tk drain pump and close() can race safely.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._hmap = None       # section handle
        self._base = 0          # mapped view base address
        self._hdr = None        # _Header view onto the mapping
        self._view = None       # byte view of the whole section
        self._seq = 0           # private seqlock counter (even = stable)
        self.pid = None         # game pid this section belongs to
        self.evt_dropped = 0    # last observed producer overflow count

    # ------------------------------------------------------------ mapping --
    def open(self, pid):
        """Create-or-open the game session's section. True on success.
        Any previous mapping is closed first (one session per ShmUI)."""
        self.close()
        name = SECTION_PREFIX + str(int(pid))
        h = _k32.CreateFileMappingW(_INVALID_HANDLE, None,
                                    PAGE_READWRITE, 0, TOTAL_SIZE, name)
        if not h:
            return False
        base = _k32.MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, TOTAL_SIZE)
        if not base:
            _k32.CloseHandle(h)
            return False
        with self._lock:
            self._hmap = h
            self._base = base
            self._hdr = _Header.from_address(base)
            self._view = (ctypes.c_char * TOTAL_SIZE).from_address(base)
            self._seq = 0
            self.pid = int(pid)
            hdr = self._hdr
            if hdr.magic != MAGIC:
                # Fresh section (Windows zero-fills) or a stale one a
                # crashed run left behind: zero every field we own, stamp
                # the magic LAST so the layer either sees the old magic
                # or the fully reset header - never a torn one.
                ctypes.memset(base, 0, HDR_SIZE)
                hdr.magic = MAGIC
            # Drop any ring backlog produced before we attached - stale
            # input events must never replay into a fresh UI session.
            hdr.evt_tail = hdr.evt_head
            self.evt_dropped = hdr.evt_dropped
        return True

    def close(self):
        """Unmap + close the section. Safe to call twice; concurrent
        users observe is_open() going False before the unmap lands."""
        with self._lock:
            base, h = self._base, self._hmap
            self._hmap = None
            self._base = 0
            self._hdr = None
            self._view = None
            self.pid = None
        if base:
            _k32.UnmapViewOfFile(base)
        if h:
            _k32.CloseHandle(h)

    def is_open(self):
        return self._hdr is not None

    # ------------------------------------------------------------- writes --
    def write_bitmap(self, bgra, w, h, dst_x, dst_y):
        """Publish one BGRA frame + its target rect (client coords).

        Seqlock: seq is bumped to ODD, header fields and the bitmap are
        written, then seq is bumped back EVEN. Single producer + in-order
        u32 stores are all the layer's retry-loop reader needs - no fence
        beyond program order is required on x86/x64."""
        w, h = int(w), int(h)
        if w <= 0 or h <= 0 or w > MAX_W or h > MAX_H:
            return False
        n = w * h * 4
        if len(bgra) < n:
            return False
        with self._lock:
            hdr = self._hdr
            if hdr is None:
                return False
            self._seq = (self._seq + 1) & 0xFFFFFFFF     # odd: in flight
            hdr.seq = self._seq
            hdr.w = w
            hdr.h = h
            hdr.dst_x = int(dst_x)
            hdr.dst_y = int(dst_y)
            ctypes.memmove(self._base + BITMAP_OFF, bgra, n)
            self._seq = (self._seq + 1) & 0xFFFFFFFF     # even: coherent
            hdr.seq = self._seq
        return True

    def _flag(self, bit, on):
        """Atomic single-u32 flag update (independent of the seqlock -
        the layer reads flags with one aligned load)."""
        with self._lock:
            if self._hdr is None:
                return
            f = self._hdr.flags
            self._hdr.flags = (f | bit) if on else (f & ~bit) & 0xFFFFFFFF

    def set_visible(self, on):
        """flags bit0 - ui_visible; ALSO the layer's input-capture gate."""
        self._flag(FLAG_VISIBLE, bool(on))

    def set_cursor_visible(self, on):
        """flags bit1 - the cursor sprite slot holds a valid image."""
        self._flag(FLAG_CURSOR, bool(on))

    def bump_heartbeat(self):
        """Producer liveness tick. The layer drops the panel (and input
        capture) when the heartbeat is stale > ~2 s, so callers must bump
        on every publish tick while the session is armed."""
        with self._lock:
            if self._hdr is not None:
                self._hdr.heartbeat = \
                    (self._hdr.heartbeat + 1) & 0xFFFFFFFF

    def write_cursor_sprite(self, rgba):
        """32x32 RGBA straight-alpha cursor image (4096 bytes)."""
        if len(rgba) < CURSOR_SIZE:
            return False
        with self._lock:
            if self._hdr is None:
                return False
            ctypes.memmove(self._base + CURSOR_OFF, rgba, CURSOR_SIZE)
        return True

    # ------------------------------------------------------------- reads ---
    def poll_events(self):
        """Drain the input ring -> list of (type, a, b, c). Advances
        evt_tail so the layer keeps producing; refreshes evt_dropped.
        head/tail are monotonic u32 counters indexed with & 255."""
        out = []
        with self._lock:
            hdr = self._hdr
            if hdr is None:
                return out
            head = hdr.evt_head
            tail = hdr.evt_tail
            avail = (head - tail) & 0xFFFFFFFF
            if avail > EVT_COUNT:
                # Wraparound defence: the layer never lets head-tail
                # exceed the ring (it bumps evt_dropped instead), so this
                # can only trigger on a misbehaving producer - resync to
                # the newest full ring rather than trust stale slots.
                tail = (head - EVT_COUNT) & 0xFFFFFFFF
                avail = EVT_COUNT
            view = self._view
            while avail:
                off = EVT_OFF + (tail & (EVT_COUNT - 1)) * EVT_SIZE
                out.append(_EVT.unpack_from(view, off))
                tail = (tail + 1) & 0xFFFFFFFF
                avail -= 1
            hdr.evt_tail = tail
            self.evt_dropped = hdr.evt_dropped
        return out

    def cursor_pos(self):
        """(x, y) the layer last published as the logical cursor, in game
        client coords; None while unmapped. Diagnostic use only."""
        with self._lock:
            if self._hdr is None:
                return None
            return (self._hdr.cursor_x, self._hdr.cursor_y)
