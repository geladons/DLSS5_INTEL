/* SPDX-License-Identifier: Apache-2.0 */
/* =====================================================================
 * nr_ui - in-process in-game UI overlay, layer side (see nr_ui.h).
 *
 * Shared-memory contract with the m13 producer. The section
 * "Local\DLSS5_UI_<pid>" (pid = the game process the layer is injected
 * into) is opened lazily on the present path - never created here - and
 * retried once per composite call until it exists. Every check after that
 * is a plain memory read; the wndproc gate issues zero syscalls.
 *
 * Frozen ABI layout (all fields little-endian u32/i32):
 *   offset    0  header, 64 B (16 u32):
 *     [0]  u32 magic        = 0x3049554E ('NUI0')
 *     [1]  u32 seq          seqlock: odd while the producer writes
 *     [2]  u32 w            bitmap extent (cap 1024x1200)
 *     [3]  u32 h
 *     [4]  i32 dst_x        target rect, game CLIENT coords
 *     [5]  i32 dst_y
 *     [6]  u32 flags        bit0 ui_visible (also the capture flag)
 *                         bit1 cursor_visible
 *     [7]  i32 cursor_x     logical cursor position, written by the LAYER
 *     [8]  i32 cursor_y
 *     [9]  u32 heartbeat    producer increments on each publish
 *     [10] u32 evt_head     ring write index (layer produces)
 *     [11] u32 evt_tail     ring read index (m13 consumes)
 *     [12] u32 evt_dropped  overflow counter
 *     [13..15] u32 reserved
 *   offset   64  event ring: 256 x 16 B { u32 type; i32 a; i32 b; u32 c }
 *     types: 1=move(x,y) 2=btn_down 3=btn_up 4=wheel(delta; c:0 vert,1 horiz)
 *            5=key_down(vk) 6=key_up(vk) 7=char(utf32)
 *   offset 4160  cursor sprite slot: 32x32 RGBA straight-alpha
 *   offset 8256  BGRA bitmap payload, w*h*4 straight-alpha
 * ===================================================================== */
#include "nr_ui.h"
#include "nr_util.h"

#include <stdio.h>
#include <string.h>

/* ---- ABI constants -------------------------------------------------- */
#define NR_UI_MAGIC        0x3049554Eu  /* 'NUI0' */
#define NR_UI_EVT_OFF      64u
#define NR_UI_EVT_COUNT    256u
#define NR_UI_EVT_MASK     (NR_UI_EVT_COUNT - 1u)
#define NR_UI_CURSOR_OFF   4160u        /* 64 + 256*16 */
#define NR_UI_CURSOR_W     32u
#define NR_UI_CURSOR_H     32u
#define NR_UI_BITMAP_OFF   8256u        /* 4160 + 32*32*4 */
#define NR_UI_MAX_W        1024u
#define NR_UI_MAX_H        1200u

#define NR_UI_F_VISIBLE    0x1u
#define NR_UI_F_CURSOR     0x2u
#define NR_UI_HB_STALE_MS  2000u

#define NR_UI_EVT_MOVE   1u
#define NR_UI_EVT_BTNDN  2u
#define NR_UI_EVT_BTNUP  3u
#define NR_UI_EVT_WHEEL  4u
#define NR_UI_EVT_KEYDN  5u
#define NR_UI_EVT_KEYUP  6u
#define NR_UI_EVT_CHAR   7u

/* Header image over the mapped section; fields are volatile because the
 * producer lives in another process. 16 u32 = 64 B. */
typedef struct NrUiHdr {
    volatile uint32_t magic;
    volatile uint32_t seq;
    volatile uint32_t w, h;
    volatile int32_t  dst_x, dst_y;
    volatile uint32_t flags;
    volatile int32_t  cursor_x, cursor_y;
    volatile uint32_t heartbeat;
    volatile uint32_t evt_head;
    volatile uint32_t evt_tail;
    volatile uint32_t evt_dropped;
    volatile uint32_t reserved[3];
} NrUiHdr;

typedef struct NrUiEvt {
    volatile uint32_t type;
    volatile int32_t  a;
    volatile int32_t  b;
    volatile uint32_t c;
} NrUiEvt;

/* Seqlock snapshot of the header fields the compositor needs. */
typedef struct NrUiSnap {
    uint32_t w, h;
    int32_t  dst_x, dst_y;
    uint32_t flags;
} NrUiSnap;

/* Module state: one per process (the section is per-process). GUI-thread
 * fields are only touched from the wndproc path and nr_ui_detach; the
 * mapping fields are written on the present path and read on both. */
typedef struct NrUiState {
    HANDLE map;                     /* OpenFileMappingA handle */
    volatile unsigned char *view;   /* section base; published last */
    /* heartbeat freshness: last value seen + when it last changed */
    uint32_t last_hb;
    uint64_t hb_ms;
    int      hb_seen;
    /* input capture (wndproc thread) */
    HWND     ui_hwnd;               /* window owning the client coords */
    int      clipped;               /* ClipCursor currently ours */
    int32_t  cur_x, cur_y;          /* logical cursor, client coords */
    int      cur_set;               /* a real position arrived */
} NrUiState;

static NrUiState g_ui;

/* ---- mapping ------------------------------------------------------- */

/* Lazy open: retry on each present until the producer has created the
 * section. Publishes g_ui.view LAST so a reader on another thread never
 * sees a half-initialised mapping. */
static int ui_open(void)
{
    char name[64];
    MEMORY_BASIC_INFORMATION mi;
    HANDLE m;
    unsigned char *v;
    if (g_ui.view) return 1;
    _snprintf(name, sizeof(name), "Local\\DLSS5_UI_%lu",
              (unsigned long)GetCurrentProcessId());
    name[sizeof(name) - 1] = 0;
    m = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!m) return 0;
    v = (unsigned char *)MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE,
                                       0, 0, 0);
    if (!v) {
        CloseHandle(m);
        return 0;
    }
    /* Contract sanity: the fixed slots (header + ring + cursor) must be
     * inside the region; a truncated section can never become valid. */
    if (!VirtualQuery(v, &mi, sizeof(mi))
        || mi.RegionSize < NR_UI_BITMAP_OFF) {
        UnmapViewOfFile(v);
        CloseHandle(m);
        return 0;
    }
    g_ui.map = m;
    MemoryBarrier();
    g_ui.view = v;
    nr_log("[nr_ui] shared UI section mapped (%s)", name);
    return 1;
}

/* Header pointer when the view is mapped AND the magic matches; NULL is
 * the zero-behaviour answer for every caller. */
static volatile NrUiHdr *ui_hdr(void)
{
    volatile NrUiHdr *h;
    if (!g_ui.view) return NULL;
    h = (volatile NrUiHdr *)g_ui.view;
    return (h->magic == NR_UI_MAGIC) ? h : NULL;
}

/* The producer increments heartbeat on each publish. Fresh = the value
 * CHANGED within the last NR_UI_HB_STALE_MS - a stalled writer (m13 busy,
 * frozen, dead) hides the UI instead of leaving a ghost panel + a live
 * input capture behind. */
static int ui_hb_fresh(void)
{
    volatile NrUiHdr *h = ui_hdr();
    uint32_t hb;
    uint64_t now;
    if (!h) return 0;
    hb = h->heartbeat;
    now = nr_now_ms();
    if (!g_ui.hb_seen || hb != g_ui.last_hb) {
        g_ui.last_hb = hb;
        g_ui.hb_ms = now;
        g_ui.hb_seen = 1;
        return 1;
    }
    return (now - g_ui.hb_ms) <= (uint64_t)NR_UI_HB_STALE_MS;
}

/* Seqlock-guarded snapshot of w/h/dst/flags. Bounded retries: a producer
 * mid-write on every attempt just skips this frame - next present retries. */
static int ui_snapshot(NrUiSnap *s)
{
    volatile NrUiHdr *h = ui_hdr();
    int i;
    if (!h) return 0;
    for (i = 0; i < 8; i++) {
        uint32_t s0 = h->seq;
        uint32_t w, hh, fl;
        int32_t dx, dy;
        if (s0 & 1u) continue;          /* producer mid-write */
        MemoryBarrier();
        w  = h->w;    hh = h->h;
        dx = h->dst_x; dy = h->dst_y;
        fl = h->flags;
        MemoryBarrier();
        if (h->seq == s0) {
            s->w = w; s->h = hh;
            s->dst_x = dx; s->dst_y = dy;
            s->flags = fl;
            return 1;
        }
    }
    return 0;
}

/* Panel-rect sanity: caps the published extent and keeps dst offsets far
 * from i32 overflow in clip math. Used before trusting any snapshot. */
static int ui_panel_sane(const NrUiSnap *s)
{
    return s->w && s->h && s->w <= NR_UI_MAX_W && s->h <= NR_UI_MAX_H
        && s->dst_x > -65536 && s->dst_x < 65536
        && s->dst_y > -65536 && s->dst_y < 65536;
}

/* ---- composite ------------------------------------------------------ */

/* Straight-alpha src-over on a clipped rect, byte channels. swap selects
 * the R/B byte order relation between source and destination so a BGRA
 * payload lands correctly on an RGBA-ordered frame (and vice versa). */
static void ui_blend(unsigned char *dst, uint32_t dw, uint32_t dh,
                     const volatile unsigned char *src, uint32_t sw, uint32_t sh,
                     int32_t ox, int32_t oy, int swap)
{
    int32_t x0 = ox < 0 ? 0 : ox;
    int32_t y0 = oy < 0 ? 0 : oy;
    int32_t x1 = ox + (int32_t)sw;
    int32_t y1 = oy + (int32_t)sh;
    int32_t x, y;
    if (x1 > (int32_t)dw) x1 = (int32_t)dw;
    if (y1 > (int32_t)dh) y1 = (int32_t)dh;
    for (y = y0; y < y1; y++) {
        const volatile unsigned char *sp =
            src + ((uint32_t)(y - oy) * sw + (uint32_t)(x0 - ox)) * 4u;
        unsigned char *dp =
            dst + ((uint32_t)y * dw + (uint32_t)x0) * 4u;
        for (x = x0; x < x1; x++, sp += 4, dp += 4) {
            unsigned a = sp[3];
            unsigned inv, s0, s1, s2;
            if (!a) continue;
            s0 = sp[swap ? 2 : 0];
            s1 = sp[1];
            s2 = sp[swap ? 0 : 2];
            if (a >= 255) {
                dp[0] = (unsigned char)s0;
                dp[1] = (unsigned char)s1;
                dp[2] = (unsigned char)s2;
                dp[3] = 255;
                continue;
            }
            inv = 255 - a;
            dp[0] = (unsigned char)((s0 * a + dp[0] * inv + 127u) / 255u);
            dp[1] = (unsigned char)((s1 * a + dp[1] * inv + 127u) / 255u);
            dp[2] = (unsigned char)((s2 * a + dp[2] * inv + 127u) / 255u);
            /* src-over alpha: a_src + a_dst*(1-a_src) */
            dp[3] = (unsigned char)(a + (dp[3] * inv + 127u) / 255u);
        }
    }
}

int nr_ui_active(void)
{
    volatile NrUiHdr *h = ui_hdr();
    if (!h) return 0;
    if (!(h->flags & NR_UI_F_VISIBLE)) return 0;
    return ui_hb_fresh();
}

void nr_ui_composite(const NrUiFrame *frame)
{
    volatile NrUiHdr *h;
    NrUiSnap s;
    if (!frame || !frame->mapped || !frame->width || !frame->height)
        return;
    if (!ui_open()) return;             /* lazy retry, once per present */
    h = ui_hdr();
    if (!h) return;
    /* The logical cursor position is the layer's slot to publish: once per
     * present so a producer reading it always sees fresh coordinates. */
    if (g_ui.cur_set) {
        h->cursor_x = g_ui.cur_x;
        h->cursor_y = g_ui.cur_y;
    }
    if (!nr_ui_active()) return;
    if (!ui_snapshot(&s)) return;
    /* Bitmap payload: BGRA (src order 0) blended at (dst_x,dst_y), clipped
     * to the frame. */
    if (ui_panel_sane(&s)) {
        ui_blend(frame->mapped, frame->width, frame->height,
                 g_ui.view + NR_UI_BITMAP_OFF, s.w, s.h,
                 s.dst_x, s.dst_y, frame->rgba_order != 0);
    }
    /* Cursor sprite: RGBA (src order 1) at the logical position; moves at
     * present rate even when the panel bitmap itself is static. */
    if ((s.flags & NR_UI_F_CURSOR) && g_ui.cur_set) {
        ui_blend(frame->mapped, frame->width, frame->height,
                 g_ui.view + NR_UI_CURSOR_OFF,
                 NR_UI_CURSOR_W, NR_UI_CURSOR_H,
                 g_ui.cur_x, g_ui.cur_y, frame->rgba_order == 0);
    }
}

/* ---- input capture --------------------------------------------------- */

/* Clamp the logical cursor into the panel rect while the overlay is up:
 * the OS cursor is already ClipCursor'd to the same rect, so raw-input
 * deltas past the edge must not drag the UI cursor out. */
static void ui_clamp_cursor(void)
{
    NrUiSnap s;
    int32_t x1, y1;
    if (!ui_snapshot(&s) || !ui_panel_sane(&s)) return;
    x1 = s.dst_x + (int32_t)s.w - 1;
    y1 = s.dst_y + (int32_t)s.h - 1;
    if (g_ui.cur_x < s.dst_x) g_ui.cur_x = s.dst_x;
    if (g_ui.cur_x > x1)      g_ui.cur_x = x1;
    if (g_ui.cur_y < s.dst_y) g_ui.cur_y = s.dst_y;
    if (g_ui.cur_y > y1)      g_ui.cur_y = y1;
}

static void ui_publish_cursor(void)
{
    volatile NrUiHdr *h = ui_hdr();
    if (!h) return;
    h->cursor_x = g_ui.cur_x;
    h->cursor_y = g_ui.cur_y;
}

static void ui_note_pos(int32_t x, int32_t y)
{
    g_ui.cur_x = x;
    g_ui.cur_y = y;
    g_ui.cur_set = 1;
    ui_clamp_cursor();
    ui_publish_cursor();
}

static void ui_note_delta(int32_t dx, int32_t dy)
{
    if (!g_ui.cur_set) {
        /* First raw delta without a prior WM_MOUSEMOVE: start centred on
         * the panel (the UI owns the cursor while it is up). */
        NrUiSnap s;
        if (ui_snapshot(&s) && ui_panel_sane(&s)) {
            g_ui.cur_x = s.dst_x + (int32_t)s.w / 2;
            g_ui.cur_y = s.dst_y + (int32_t)s.h / 2;
        } else {
            g_ui.cur_x = 0;
            g_ui.cur_y = 0;
        }
        g_ui.cur_set = 1;
    }
    g_ui.cur_x += dx;
    g_ui.cur_y += dy;
    ui_clamp_cursor();
    ui_publish_cursor();
}

/* Push one event onto the shared ring. Producers serialize on the global
 * layer lock (the wndproc path can run on more than one GUI thread); the
 * entry is fully written BEFORE evt_head is bumped so the consumer never
 * observes a claimed-but-unwritten slot. The header/ring pointers are
 * re-fetched under the lock: nr_ui_detach unmaps the view while holding
 * the same lock, so a stale base can never slip through. */
static void ui_evt_push(uint32_t type, int32_t a, int32_t b, uint32_t c)
{
    volatile NrUiHdr *h;
    volatile NrUiEvt *ring;
    volatile NrUiEvt *e;
    uint32_t head, tail;
    lock();
    h = ui_hdr();
    if (!h) {
        unlock();
        return;
    }
    ring = (volatile NrUiEvt *)(g_ui.view + NR_UI_EVT_OFF);
    head = h->evt_head;
    tail = h->evt_tail;
    if (head - tail >= NR_UI_EVT_COUNT) {
        InterlockedIncrement((volatile LONG *)&h->evt_dropped);
        unlock();
        return;
    }
    /* Coalesce move events into the newest QUEUED slot (head-1) when the
     * consumer lags: positions are ephemeral, only the latest matters. */
    if (type == NR_UI_EVT_MOVE && head > tail
        && ring[(head - 1) & NR_UI_EVT_MASK].type == NR_UI_EVT_MOVE) {
        e = &ring[(head - 1) & NR_UI_EVT_MASK];
        e->a = a;
        e->b = b;
        unlock();
        return;
    }
    e = &ring[head & NR_UI_EVT_MASK];
    e->type = type;
    e->a = a;
    e->b = b;
    e->c = c;
    MemoryBarrier();
    InterlockedIncrement((volatile LONG *)&h->evt_head);
    unlock();
}

/* ClipCursor edge tracking, driven per message from the wndproc path.
 * Rising edge: clip to the panel rect (client -> screen on the associated
 * window). Falling edge: release. Only transitions touch the OS. */
static void ui_clip_edge(int on)
{
    NrUiSnap s;
    POINT p0, p1;
    RECT rc;
    if (on && !g_ui.clipped) {
        if (!g_ui.ui_hwnd) return;      /* no association yet; retry */
        if (!ui_snapshot(&s) || !ui_panel_sane(&s)) return;
        p0.x = s.dst_x;                p0.y = s.dst_y;
        p1.x = s.dst_x + (LONG)s.w;    p1.y = s.dst_y + (LONG)s.h;
        if (!ClientToScreen(g_ui.ui_hwnd, &p0)
            || !ClientToScreen(g_ui.ui_hwnd, &p1)) return;
        rc.left = p0.x; rc.top = p0.y; rc.right = p1.x; rc.bottom = p1.y;
        ClipCursor(&rc);
        g_ui.clipped = 1;
    } else if (!on && g_ui.clipped) {
        ClipCursor(NULL);
        g_ui.clipped = 0;
    }
}

int nr_ui_capture_active(HWND hwnd)
{
    volatile NrUiHdr *h = ui_hdr();
    int on = 0;
    if (h && (h->flags & NR_UI_F_VISIBLE) && ui_hb_fresh()) {
        /* First guarded window to observe the flag owns the client-
         * coordinate space (ClientToScreen anchor for the clip rect). */
        if (!g_ui.ui_hwnd) g_ui.ui_hwnd = hwnd;
        on = 1;
    } else {
        g_ui.ui_hwnd = NULL;
    }
    ui_clip_edge(on);
    return on;
}

/* The message filter: eaten input becomes a ring event; focus/capture/
 * cursor bookkeeping and anything unlisted flows to the previous wndproc. */
int nr_ui_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        /* lp = client coords of the cursor position (signed). */
        {
            int32_t x = (int32_t)(int16_t)LOWORD(lp);
            int32_t y = (int32_t)(int16_t)HIWORD(lp);
            ui_note_pos(x, y);
            ui_evt_push(NR_UI_EVT_MOVE, x, y, 0);
        }
        return 1;
    case WM_INPUT:
        /* Raw mouse deltas: the OS cursor may be pinned at the clip edge,
         * so the logical position integrates deltas on top. */
        {
            unsigned char buf[64];
            RAWINPUT *ri = (RAWINPUT *)buf;
            UINT sz = sizeof(buf);
            UINT got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, ri, &sz,
                                       sizeof(RAWINPUTHEADER));
            if (got != (UINT)-1 && got >= sizeof(RAWINPUTHEADER)
                && ri->header.dwType == RIM_TYPEMOUSE) {
                int32_t dx, dy;
                if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
                    /* Absolute raw input (rare - tablets/virtual HID):
                     * normalize 0..65535 onto the panel rect. */
                    NrUiSnap s;
                    if (ui_snapshot(&s) && ui_panel_sane(&s)) {
                        dx = s.dst_x + (int32_t)(
                            (int64_t)ri->data.mouse.lLastX * s.w / 65535);
                        dy = s.dst_y + (int32_t)(
                            (int64_t)ri->data.mouse.lLastY * s.h / 65535);
                        ui_note_pos(dx, dy);
                        ui_evt_push(NR_UI_EVT_MOVE, dx, dy, 0);
                    }
                } else {
                    dx = ri->data.mouse.lLastX;
                    dy = ri->data.mouse.lLastY;
                    if (dx || dy) {
                        ui_note_delta(dx, dy);
                        ui_evt_push(NR_UI_EVT_MOVE, g_ui.cur_x, g_ui.cur_y, 0);
                    }
                }
            }
        }
        return 1;
    /* Buttons: btn index 0=L 1=R 2=M 3=X1 4=X2. A double-click message is
     * just another press for the consumer - the up/down pairs stay
     * consistent because every press/release is eaten anyway. */
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        ui_evt_push(NR_UI_EVT_BTNDN, 0, 0, 0); return 1;
    case WM_LBUTTONUP:
        ui_evt_push(NR_UI_EVT_BTNUP, 0, 0, 0); return 1;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        ui_evt_push(NR_UI_EVT_BTNDN, 1, 0, 0); return 1;
    case WM_RBUTTONUP:
        ui_evt_push(NR_UI_EVT_BTNUP, 1, 0, 0); return 1;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        ui_evt_push(NR_UI_EVT_BTNDN, 2, 0, 0); return 1;
    case WM_MBUTTONUP:
        ui_evt_push(NR_UI_EVT_BTNUP, 2, 0, 0); return 1;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        ui_evt_push(NR_UI_EVT_BTNDN, 2 + (int32_t)HIWORD(wp), 0, 0);
        return 1;
    case WM_XBUTTONUP:
        ui_evt_push(NR_UI_EVT_BTNUP, 2 + (int32_t)HIWORD(wp), 0, 0);
        return 1;
    case WM_MOUSEWHEEL:
        ui_evt_push(NR_UI_EVT_WHEEL,
                    (int32_t)((int16_t)HIWORD(wp) / WHEEL_DELTA), 0, 0);
        return 1;
    case WM_MOUSEHWHEEL:
        ui_evt_push(NR_UI_EVT_WHEEL,
                    (int32_t)((int16_t)HIWORD(wp) / WHEEL_DELTA), 0, 1);
        return 1;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        ui_evt_push(NR_UI_EVT_KEYDN, (int32_t)wp, 0, 0);
        return 1;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        ui_evt_push(NR_UI_EVT_KEYUP, (int32_t)wp, 0, 0);
        return 1;
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        /* wParam is the UTF-16 code unit = utf32 for the BMP. */
        ui_evt_push(NR_UI_EVT_CHAR, (int32_t)wp, 0, 0);
        return 1;
    default:
        /* Forwarded on purpose: WM_SETFOCUS/WM_KILLFOCUS (focus bookkeeping
         * the game needs), WM_CAPTURECHANGED (release tracking), and
         * WM_SETCURSOR - forwarding keeps the game's own cursor state
         * intact; our sprite is composited on top and the OS cursor is
         * already confined to the panel rect. */
        return 0;
    }
}

/* ---- teardown -------------------------------------------------------- */

void nr_ui_detach(void)
{
    if (g_ui.clipped) {
        ClipCursor(NULL);
        g_ui.clipped = 0;
    }
    g_ui.ui_hwnd = NULL;
    g_ui.cur_set = 0;
    if (g_ui.view) {
        UnmapViewOfFile((LPCVOID)g_ui.view);
        g_ui.view = NULL;
    }
    if (g_ui.map) {
        CloseHandle(g_ui.map);
        g_ui.map = NULL;
    }
    g_ui.hb_seen = 0;
    g_ui.last_hb = 0;
    g_ui.hb_ms = 0;
}
