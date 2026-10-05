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
 *     [2]  u32 w            bitmap extent (cap 3840x2160)
 *     [3]  u32 h
 *     [4]  i32 dst_x        target rect, game CLIENT coords
 *     [5]  i32 dst_y
 *     [6]  u32 flags        bit0 ui_visible (compositing gate AND the
 *                          input-routing master switch - see below)
 *                         bit1 cursor_visible (sprite slot valid)
 *                         bit2 kbd_capture (producer: a text widget owns
 *                          Tk focus - key/char messages go to the ring
 *                          instead of the game)
 *                         bit3 no_clip (producer: skip ClipCursor - the
 *                          framed overlay on a windowed game must not
 *                          jail the pointer inside the client rect)
 *     [7]  i32 cursor_x     logical cursor position, written by the LAYER
 *     [8]  i32 cursor_y
 *     [9]  u32 heartbeat    producer increments on each publish
 *     [10] u32 evt_head     ring write index (layer produces)
 *     [11] u32 evt_tail     ring read index (m13 consumes)
 *     [12] u32 evt_dropped  overflow counter
 *     [13] u32 hole_lt      packed u16 pair: hole_l | hole_t<<16
 *     [14] u32 hole_rb      packed u16 pair: hole_r | hole_b<<16
 *                         The framed layout publishes a client-sized
 *                         bitmap with a transparent CENTER hole (the live
 *                         game view); (l,t)-(r,b) is that hole in BITMAP
 *                         pixel coords, seqlock-covered with w/h/dst.
 *                         Degenerate (l>=r or t>=b) = no hole: the whole
 *                         bitmap is UI (pre-framing semantics).
 *                         The layer hit-tests input on bitmap-minus-hole
 *                         and skips the hole while blending.
 *     [15] u32 layer_hb     LAYER-WRITTEN stamp: top 4 bits = ABI level
 *                         (>=1 = hole/hit-test aware), low 28 bits =
 *                         present counter bumped per composite. m13 reads
 *                         it to tell "armed but no consumer" apart from
 *                         a healthy session.
 *   offset   64  event ring: 256 x 16 B { u32 type; i32 a; i32 b; u32 c }
 *     types: 1=move(x,y) 2=btn_down 3=btn_up 4=wheel(delta; c:0 vert,1 horiz)
 *            5=key_down(vk) 6=key_up(vk) 7=char(utf32)
 *   offset 4160  cursor sprite slot: 32x32 RGBA straight-alpha
 *   offset 8256  BGRA bitmap payload, w*h*4 straight-alpha
 *
 * INPUT CONTRACT (ABI level 1): ui_visible no longer means "eat all game
 * input". While the overlay is up the wndproc filter routes by REGION:
 *   - pointer inside the bitmap rect AND outside the hole  -> ring event,
 *     message eaten (a click on the band operates the controls);
 *   - pointer in the hole or outside the bitmap            -> the original
 *     message flows to the game unchanged (center stays live game);
 *   - while any overlay-captured button is held (implicit grab) every
 *     mouse message is still eaten, so a drag that wanders into the hole
 *     keeps tracking until the release;
 *   - raw-input titles (RIDEV_NOLEGACY) never see WM_*BUTTON* or
 *     WM_MOUSEWHEEL at all: WM_INPUT usButtonFlags/usButtonData are
 *     decoded with the same region gates so clicks/wheel still reach
 *     the ring;
 *   - WM_SETCURSOR is eaten (SetCursor NULL) while the overlay is up -
 *     the composited sprite is the only cursor over the game window;
 *   - key/char messages are eaten only while bit2 (kbd_capture) is set.
 * The hole rect is the coarse occupancy mask the producer publishes -
 * band pixels are uniformly opaque, so region-test == alpha-test without
 * reading payload bytes under the seqlock.
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
#define NR_UI_MAX_W        3840u    /* framed band spans the game client */
#define NR_UI_MAX_H        2160u    /* (4K UHD covers every real client) */

#define NR_UI_F_VISIBLE    0x1u
#define NR_UI_F_CURSOR     0x2u
#define NR_UI_F_KBDCAP     0x4u     /* producer: text focus owns keys */
#define NR_UI_F_NOCLIP     0x8u     /* producer: do not ClipCursor */
#define NR_UI_HB_STALE_MS  2000u
#define NR_UI_ABI_LEVEL    1u       /* layer_hb top nibble: this build is
                                     * hole/hit-test aware */

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
    volatile uint32_t hole_lt;      /* hole_l | hole_t<<16, u16 pair */
    volatile uint32_t hole_rb;      /* hole_r | hole_b<<16, u16 pair */
    volatile uint32_t layer_hb;     /* layer stamp: ABI lvl + counter */
} NrUiHdr;

typedef struct NrUiEvt {
    volatile uint32_t type;
    volatile int32_t  a;
    volatile int32_t  b;
    volatile uint32_t c;
} NrUiEvt;

/* Seqlock snapshot of the header fields the compositor + hit-test need. */
typedef struct NrUiSnap {
    uint32_t w, h;
    int32_t  dst_x, dst_y;
    uint32_t flags;
    int32_t  hole_l, hole_t, hole_r, hole_b;    /* bitmap coords, unpacked */
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
    int      raw_reg;               /* RIDEV_INPUTSINK mouse on ui_hwnd */
    int      clipped;               /* ClipCursor currently ours */
    int32_t  cur_x, cur_y;          /* logical cursor, client coords */
    int      cur_set;               /* a real position arrived */
    int      real_out;              /* real cursor last seen OUTSIDE the
                                     * client rect - the next WM_INPUT
                                     * that finds it back inside re-syncs
                                     * the virtual cursor to the real
                                     * one (see WM_INPUT). */
    uint32_t ui_btns;               /* overlay-captured buttons held (bit
                                     * per btn idx 0..4): while nonzero
                                     * all mouse messages are eaten -
                                     * the Tk-side implicit grab needs
                                     * the release even in the hole. */
    uint32_t lhb_cnt;               /* own layer_hb counter (28-bit) */
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
        uint32_t w, hh, fl, hlt, hrb;
        int32_t dx, dy;
        if (s0 & 1u) continue;          /* producer mid-write */
        MemoryBarrier();
        w  = h->w;    hh = h->h;
        dx = h->dst_x; dy = h->dst_y;
        fl = h->flags;
        hlt = h->hole_lt; hrb = h->hole_rb;
        MemoryBarrier();
        if (h->seq == s0) {
            s->w = w; s->h = hh;
            s->dst_x = dx; s->dst_y = dy;
            s->flags = fl;
            s->hole_l = (int32_t)(hlt & 0xFFFFu);
            s->hole_t = (int32_t)(hlt >> 16);
            s->hole_r = (int32_t)(hrb & 0xFFFFu);
            s->hole_b = (int32_t)(hrb >> 16);
            if (s->hole_l >= s->hole_r || s->hole_t >= s->hole_b) {
                s->hole_l = s->hole_t = s->hole_r = s->hole_b = 0;
            }
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

/* One row of straight-alpha src-over, byte channels. swap selects the R/B
 * byte order relation between source and destination so a BGRA payload
 * lands correctly on an RGBA-ordered frame (and vice versa). sp/dp are
 * already stepped to the segment start. */
static void ui_blend_row(unsigned char *dp, const volatile unsigned char *sp,
                         int32_t xa, int32_t xb, int swap)
{
    int32_t x;
    for (x = xa; x < xb; x++, sp += 4, dp += 4) {
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

/* Straight-alpha src-over on a clipped rect. hx0/hy0/hx1/hy1 = the
 * transparent hole in SOURCE (bitmap) coords: rows crossing it are split
 * into left/right segments so the hole never touches the frame even if
 * the payload alpha there were nonzero. Degenerate hole = full rows. */
static void ui_blend(unsigned char *dst, uint32_t dw, uint32_t dh,
                     const volatile unsigned char *src, uint32_t sw, uint32_t sh,
                     int32_t ox, int32_t oy, int swap,
                     int32_t hx0, int32_t hy0, int32_t hx1, int32_t hy1)
{
    int32_t x0 = ox < 0 ? 0 : ox;
    int32_t y0 = oy < 0 ? 0 : oy;
    int32_t x1 = ox + (int32_t)sw;
    int32_t y1 = oy + (int32_t)sh;
    int32_t y;
    if (x1 > (int32_t)dw) x1 = (int32_t)dw;
    if (y1 > (int32_t)dh) y1 = (int32_t)dh;
    /* Hole sanity in bitmap coords; degenerate disables the split. */
    if (hx0 < 0) hx0 = 0;
    if (hy0 < 0) hy0 = 0;
    if (hx1 > (int32_t)sw) hx1 = (int32_t)sw;
    if (hy1 > (int32_t)sh) hy1 = (int32_t)sh;
    if (hx0 >= hx1 || hy0 >= hy1) hx0 = hx1 = hy0 = hy1 = 0;
    for (y = y0; y < y1; y++) {
        int32_t by = y - oy;
        int32_t e0 = x1, s1 = x1;
        if (by >= hy0 && by < hy1) {        /* row crosses the hole */
            e0 = ox + hx0;
            s1 = ox + hx1;
            if (e0 > x1) e0 = x1;
            if (e0 < x0) e0 = x0;
            if (s1 < x0) s1 = x0;
            if (s1 > x1) s1 = x1;
        }
        /* left (or whole) segment */
        if (e0 > x0) {
            const volatile unsigned char *sp =
                src + ((uint32_t)by * sw + (uint32_t)(x0 - ox)) * 4u;
            unsigned char *dp =
                dst + ((uint32_t)y * dw + (uint32_t)x0) * 4u;
            ui_blend_row(dp, sp, x0, e0, swap);
        }
        /* right segment - only on holed rows */
        if (s1 < x1) {
            const volatile unsigned char *sp =
                src + ((uint32_t)by * sw + (uint32_t)(s1 - ox)) * 4u;
            unsigned char *dp =
                dst + ((uint32_t)y * dw + (uint32_t)s1) * 4u;
            ui_blend_row(dp, sp, s1, x1, swap);
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
    /* layer_hb: ABI level in the top nibble + a present counter below.
     * Bumped on EVERY present while the section is live - m13 uses it to
     * tell "armed but nobody is consuming" (stuck at 0) from a healthy
     * composite, and whether the consumer understands the hole fields. */
    g_ui.lhb_cnt = (g_ui.lhb_cnt + 1u) & 0x0FFFFFFFu;
    h->layer_hb = (NR_UI_ABI_LEVEL << 28) | g_ui.lhb_cnt;
    /* The logical cursor position is the layer's slot to publish: once per
     * present so a producer reading it always sees fresh coordinates. */
    if (g_ui.cur_set) {
        h->cursor_x = g_ui.cur_x;
        h->cursor_y = g_ui.cur_y;
    }
    if (!nr_ui_active()) return;
    if (!ui_snapshot(&s)) return;
    /* Bitmap payload: BGRA (src order 0) blended at (dst_x,dst_y), clipped
     * to the frame; the hole rect splits rows so the center stays the
     * untouched game frame. */
    if (ui_panel_sane(&s)) {
        ui_blend(frame->mapped, frame->width, frame->height,
                 g_ui.view + NR_UI_BITMAP_OFF, s.w, s.h,
                 s.dst_x, s.dst_y, frame->rgba_order != 0,
                 s.hole_l, s.hole_t, s.hole_r, s.hole_b);
    }
    /* Cursor sprite: RGBA (src order 1) at the logical position; moves at
     * present rate even when the panel bitmap itself is static. */
    if ((s.flags & NR_UI_F_CURSOR) && g_ui.cur_set) {
        ui_blend(frame->mapped, frame->width, frame->height,
                 g_ui.view + NR_UI_CURSOR_OFF,
                 NR_UI_CURSOR_W, NR_UI_CURSOR_H,
                 g_ui.cur_x, g_ui.cur_y, frame->rgba_order == 0,
                 0, 0, 0, 0);
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
    g_ui.real_out = 0;              /* an authoritative inside pos */
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

/* Raw-input sink on the guarded window. WM_INPUT only reaches our
 * wndproc when the game registered raw devices targeting THIS window
 * (or takes foreground delivery); a title whose registration points at
 * a different hwndTarget - or one that never registers at all - leaves
 * the band blind to the ONLY mouse channel a RIDEV_NOLEGACY path emits.
 * RIDEV_INPUTSINK with hwndTarget = the guarded window makes delivery
 * deterministic: WM_INPUT lands here for every mouse event while the
 * overlay is up. It is additive (the game's own registrations are
 * untouched), window-lifetime scoped, and unregistered on every
 * disassociate/detach so a hidden overlay carries zero delivery. */
/* Foreign-confinement drop for the WM_INPUT path. A game that re-clips
 * every frame loses the pin within one raw event instead of the next
 * Python watchdog pass. Two policies, mirroring the m13 input guard:
 *   noclip  (windowed session): the layer never clips, so ANY armed
 *             rect is the game's - an unconfined pointer reports the
 *             virtual screen, anything smaller is foreign and dropped;
 *   clipped (fullscreen): the layer's own ClipCursor covers the whole
 *             client - drop only a foreign pin smaller than the client. */
static void ui_foreign_clip(HWND hwnd, int noclip)
{
    RECT clip, cr;
    POINT a, b;
    if (!GetClipCursor(&clip)) return;
    if (noclip) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (clip.left <= vx && clip.top <= vy
            && clip.right >= vx + vw && clip.bottom >= vy + vh)
            return;                            /* desktop = unconfined */
        ClipCursor(NULL);
        return;
    }
    if (!GetClientRect(hwnd, &cr)) return;
    a.x = cr.left;  a.y = cr.top;
    b.x = cr.right; b.y = cr.bottom;
    if (!ClientToScreen(hwnd, &a) || !ClientToScreen(hwnd, &b)) return;
    if (clip.left <= a.x && clip.top <= a.y
        && clip.right >= b.x && clip.bottom >= b.y)
        return;                                /* covers client = ours */
    ClipCursor(NULL);                          /* foreign pin: drop it */
}

static void ui_raw_reg(HWND hwnd, int on)
{
    RAWINPUTDEVICE rid;
    if (on) {
        if (g_ui.raw_reg) return;
        memset(&rid, 0, sizeof(rid));
        rid.usUsagePage = 0x01;             /* generic desktop page */
        rid.usUsage     = 0x02;             /* mouse */
        rid.dwFlags     = RIDEV_INPUTSINK;
        rid.hwndTarget  = hwnd;
        g_ui.raw_reg = RegisterRawInputDevices(&rid, 1, sizeof(rid))
            ? 1 : 0;
        if (g_ui.raw_reg)
            nr_log("[nr_ui] raw-input sink on hwnd=0x%p", (void *)hwnd);
        else
            nr_log("[nr_ui] raw-input sink FAILED hwnd=0x%p err=%lu",
                   (void *)hwnd, (unsigned long)GetLastError());
    } else if (g_ui.raw_reg) {
        memset(&rid, 0, sizeof(rid));
        rid.usUsagePage = 0x01;
        rid.usUsage     = 0x02;
        rid.dwFlags     = RIDEV_REMOVE;
        rid.hwndTarget  = NULL;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        g_ui.raw_reg = 0;
        nr_log("[nr_ui] raw-input sink removed");
    }
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
    uint32_t flags = h ? h->flags : 0;
    int on = 0;
    if (h && (flags & NR_UI_F_VISIBLE) && ui_hb_fresh()) {
        /* First guarded window to observe the flag owns the client-
         * coordinate space (ClientToScreen anchor for the clip rect) AND
         * becomes the raw-input sink target - WM_INPUT must reach this
         * wndproc for NOLEGACY titles to have any band input at all. */
        if (!g_ui.ui_hwnd) {
            g_ui.ui_hwnd = hwnd;
            ui_raw_reg(hwnd, 1);
        }
        on = 1;
    } else {
        ui_raw_reg(g_ui.ui_hwnd, 0);
        g_ui.ui_hwnd = NULL;
        g_ui.ui_btns = 0;       /* release any stuck implicit grab */
    }
    /* ClipCursor only while the producer wants it: the framed overlay
     * sets NOCLIP for windowed games (the band covers only the client
     * rect - jailing the cursor inside it would trap the pointer against
     * the desktop side). Fullscreen keeps the confinement, which is a
     * no-op there anyway (the client rect IS the screen). */
    ui_clip_edge(on && !(flags & NR_UI_F_NOCLIP));
    return on;
}

/* Region hit test: the point (game CLIENT coords) is owned by the overlay
 * when it lands inside the published bitmap rect AND outside the hole.
 * The hole is the producer-published coarse occupancy mask - in the
 * framed layout band pixels are uniformly opaque, so region == alpha
 * without touching payload bytes under the seqlock. */
static int ui_hit(const NrUiSnap *s, int32_t x, int32_t y)
{
    if (x < s->dst_x || x >= s->dst_x + (int32_t)s->w
        || y < s->dst_y || y >= s->dst_y + (int32_t)s->h)
        return 0;
    if (x >= s->hole_l && x < s->hole_r
        && y >= s->hole_t && y < s->hole_b)
        return 0;
    return 1;
}

/* Producer-controlled keyboard gate (flags bit2): a text Entry holds Tk
 * focus, so key/char messages route to the ring instead of the game. */
static int ui_kbd_cap(void)
{
    volatile NrUiHdr *h = ui_hdr();
    return h && (h->flags & NR_UI_F_KBDCAP);
}

/* Button-region gate shared by the WM_*BUTTON* messages and the WM_INPUT
 * raw-button decode (idx: 0=L 1=R 2=M 3=X1 4=X2). A press is overlay
 * input only when the logical cursor sits on the band or that button's
 * grab bit is already set (the grab bit is checked OUTSIDE sane: a torn
 * snapshot must never leak a down to the game while the grab is live); a
 * release is overlay input when it was grabbed or lands on the band -
 * either way the ring's down/up pairs stay consistent and the game never
 * sees half a click. Returns 1 = consumed into the ring (eat), 0 = the
 * originating message must flow to the game. */
static int ui_btn_gate(int idx, int down)
{
    NrUiSnap s;
    int sane = ui_snapshot(&s) && ui_panel_sane(&s);
    uint32_t bit = 1u << idx;
    if (down) {
        if (!((sane && ui_hit(&s, g_ui.cur_x, g_ui.cur_y))
              || (g_ui.ui_btns & bit)))
            return 0;
        g_ui.ui_btns |= bit;
        ui_evt_push(NR_UI_EVT_BTNDN, idx, 0, 0);
        return 1;
    }
    if (!(g_ui.ui_btns & bit)
        && !(sane && ui_hit(&s, g_ui.cur_x, g_ui.cur_y)))
        return 0;
    g_ui.ui_btns &= ~bit;
    ui_evt_push(NR_UI_EVT_BTNUP, idx, 0, 0);
    return 1;
}

/* The message filter (ABI level 1): input routes by REGION - a message
 * whose pointer position lands on the band becomes a ring event and is
 * eaten; anything over the hole (or outside the bitmap) flows to the
 * previous wndproc untouched. Moves are ALSO pushed while passing through
 * so the producer's hover/leave tracking stays truthful. While a captured
 * button is held (g_ui.ui_btns) all mouse messages are eaten - the
 * Tk-side implicit grab must see the release even in the hole.
 * Focus/capture/cursor bookkeeping and anything unlisted always flows. */
int nr_ui_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NrUiSnap s;
    int sane = ui_snapshot(&s) && ui_panel_sane(&s);
    switch (msg) {
    case WM_MOUSEMOVE:
        /* lp = client coords of the cursor position (signed). */
        {
            int32_t x = (int32_t)(int16_t)LOWORD(lp);
            int32_t y = (int32_t)(int16_t)HIWORD(lp);
            ui_note_pos(x, y);
            ui_evt_push(NR_UI_EVT_MOVE, x, y, 0);
            if (g_ui.ui_btns || (sane && ui_hit(&s, x, y)))
                return 1;
        }
        return 0;
    case WM_INPUT:
        /* Raw mouse input: the OS cursor may be pinned at the clip edge,
         * so the logical position integrates deltas on top. RIDEV_NOLEGACY
         * titles never see WM_xBUTTON / WM_MOUSEWHEEL at all - WM_INPUT is
         * their ONLY mouse channel - so usButtonFlags and usButtonData are
         * decoded with the same region gates as the legacy path, or clicks
         * die in a blind spot (field report: overlay visible, clicks dead).
         * Eaten only when the logical position sits on the band (or a drag
         * is live); hole input keeps driving the game's own raw path. */
        {
            unsigned char buf[64];
            RAWINPUT *ri = (RAWINPUT *)buf;
            UINT sz = sizeof(buf);
            UINT got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, ri, &sz,
                                       sizeof(RAWINPUTHEADER));
            if (got != (UINT)-1 && got >= sizeof(RAWINPUTHEADER)
                && ri->header.dwType == RIM_TYPEMOUSE) {
                int32_t dx, dy;
                USHORT bf;
                if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
                    /* Absolute raw input (rare - tablets/virtual HID):
                     * normalize 0..65535 onto the panel rect. */
                    if (sane) {
                        dx = s.dst_x + (int32_t)(
                            (int64_t)ri->data.mouse.lLastX * s.w / 65535);
                        dy = s.dst_y + (int32_t)(
                            (int64_t)ri->data.mouse.lLastY * s.h / 65535);
                        ui_note_pos(dx, dy);
                        ui_evt_push(NR_UI_EVT_MOVE, dx, dy, 0);
                    }
                } else {
                    /* The logical cursor is a VIRTUAL pointer: it seeds
                     * from and re-syncs to the real cursor only when the
                     * real one crosses back INTO the client rect
                     * (real_out edge). A clip-pinned cursor never left,
                     * so a game's point-clip cannot snap the sprite back
                     * mid-gesture - the deltas integrate on top, and the
                     * foreign pin itself is dropped within one event
                     * instead of the next Python watchdog pass. */
                    {
                        POINT p;
                        RECT cr;
                        int inside;
                        dx = ri->data.mouse.lLastX;
                        dy = ri->data.mouse.lLastY;
                        ui_foreign_clip(hwnd,
                            sane && (s.flags & NR_UI_F_NOCLIP));
                        inside = GetCursorPos(&p)
                            && ScreenToClient(hwnd, &p)
                            && GetClientRect(hwnd, &cr)
                            && p.x >= 0 && p.y >= 0
                            && p.x < cr.right && p.y < cr.bottom;
                        if (inside && (g_ui.real_out || !g_ui.cur_set)) {
                            ui_note_pos(p.x, p.y);
                            ui_evt_push(NR_UI_EVT_MOVE, p.x, p.y, 0);
                        } else if (dx || dy) {
                            ui_note_delta(dx, dy);
                            ui_evt_push(NR_UI_EVT_MOVE,
                                        g_ui.cur_x, g_ui.cur_y, 0);
                        }
                        g_ui.real_out = !inside;
                    }
                }
                bf = ri->data.mouse.usButtonFlags;
                if (bf & RI_MOUSE_LEFT_BUTTON_DOWN)   ui_btn_gate(0, 1);
                if (bf & RI_MOUSE_LEFT_BUTTON_UP)     ui_btn_gate(0, 0);
                if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN)  ui_btn_gate(1, 1);
                if (bf & RI_MOUSE_RIGHT_BUTTON_UP)    ui_btn_gate(1, 0);
                if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN) ui_btn_gate(2, 1);
                if (bf & RI_MOUSE_MIDDLE_BUTTON_UP)   ui_btn_gate(2, 0);
                if (bf & RI_MOUSE_BUTTON_4_DOWN)      ui_btn_gate(3, 1);
                if (bf & RI_MOUSE_BUTTON_4_UP)        ui_btn_gate(3, 0);
                if (bf & RI_MOUSE_BUTTON_5_DOWN)      ui_btn_gate(4, 1);
                if (bf & RI_MOUSE_BUTTON_5_UP)        ui_btn_gate(4, 0);
                if (bf & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) {
                    /* usButtonData is the signed wheel delta; the wheel
                     * is overlay input only on a band hit or a live drag
                     * (same gate as WM_MOUSEWHEEL below). */
                    if (g_ui.ui_btns
                        || (sane && ui_hit(&s, g_ui.cur_x, g_ui.cur_y)))
                        ui_evt_push(NR_UI_EVT_WHEEL,
                                    (int32_t)((int16_t)ri->data.mouse
                                              .usButtonData / WHEEL_DELTA),
                                    0,
                                    (bf & RI_MOUSE_HWHEEL) ? 1u : 0u);
                }
            }
        }
        if (g_ui.ui_btns || (sane && ui_hit(&s, g_ui.cur_x, g_ui.cur_y)))
            return 1;
        return 0;
    /* Buttons: btn index 0=L 1=R 2=M 3=X1 4=X2 (lp = client coords). A
     * double-click message is just another press. ui_btn_gate() owns the
     * hit/grab rule: downs are eaten on a band hit, ups are eaten when
     * that button was captured or lands on the band - the ring's down/up
     * pairs stay consistent and the game never sees half a click. */
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        ui_note_pos((int32_t)(int16_t)LOWORD(lp),
                    (int32_t)(int16_t)HIWORD(lp));
        return ui_btn_gate(0, 1);
    case WM_LBUTTONUP:
        ui_note_pos((int32_t)(int16_t)LOWORD(lp),
                    (int32_t)(int16_t)HIWORD(lp));
        return ui_btn_gate(0, 0);
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        ui_note_pos((int32_t)(int16_t)LOWORD(lp),
                    (int32_t)(int16_t)HIWORD(lp));
        return ui_btn_gate(1, 1);
    case WM_RBUTTONUP:
        ui_note_pos((int32_t)(int16_t)LOWORD(lp),
                    (int32_t)(int16_t)HIWORD(lp));
        return ui_btn_gate(1, 0);
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        ui_note_pos((int32_t)(int16_t)LOWORD(lp),
                    (int32_t)(int16_t)HIWORD(lp));
        return ui_btn_gate(2, 1);
    case WM_MBUTTONUP:
        ui_note_pos((int32_t)(int16_t)LOWORD(lp),
                    (int32_t)(int16_t)HIWORD(lp));
        return ui_btn_gate(2, 0);
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        ui_note_pos((int32_t)(int16_t)LOWORD(lp),
                    (int32_t)(int16_t)HIWORD(lp));
        {
            int xi = (int)HIWORD(wp);                /* 1=X1, 2=X2 */
            if (xi < 1 || xi > 2) xi = 1;
            return ui_btn_gate(2 + xi, 1);
        }
    case WM_XBUTTONUP:
        ui_note_pos((int32_t)(int16_t)LOWORD(lp),
                    (int32_t)(int16_t)HIWORD(lp));
        {
            int xi = (int)HIWORD(wp);
            if (xi < 1 || xi > 2) xi = 1;
            return ui_btn_gate(2 + xi, 0);
        }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        /* Wheel lp is SCREEN coords - convert on the guarded window; fall
         * back to the tracked logical position when conversion fails. */
        {
            POINT pt;
            int32_t x = g_ui.cur_x, y = g_ui.cur_y;
            pt.x = (int32_t)(int16_t)LOWORD(lp);
            pt.y = (int32_t)(int16_t)HIWORD(lp);
            if (ScreenToClient(hwnd, &pt)) { x = pt.x; y = pt.y; }
            if (!(g_ui.ui_btns || (sane && ui_hit(&s, x, y)))) return 0;
            ui_evt_push(NR_UI_EVT_WHEEL,
                        (int32_t)((int16_t)HIWORD(wp) / WHEEL_DELTA), 0,
                        msg == WM_MOUSEHWHEEL ? 1u : 0u);
        }
        return 1;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!ui_kbd_cap()) return 0;
        ui_evt_push(NR_UI_EVT_KEYDN, (int32_t)wp, 0, 0);
        return 1;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (!ui_kbd_cap()) return 0;
        ui_evt_push(NR_UI_EVT_KEYUP, (int32_t)wp, 0, 0);
        return 1;
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        /* wParam is the UTF-16 code unit = utf32 for the BMP. */
        if (!ui_kbd_cap()) return 0;
        ui_evt_push(NR_UI_EVT_CHAR, (int32_t)wp, 0, 0);
        return 1;
    case WM_SETCURSOR:
        /* The composited sprite is the ONLY cursor while the overlay is
         * up - the producer publishes it unconditionally now, so a
         * game-drawn or class-cursor arrow over the window would
         * double-draw. Eating the message (SetCursor NULL) keeps the OS
         * cursor hidden over the game window for the whole session; the
         * game's own cursor logic resumes untouched on overlay close. */
        SetCursor(NULL);
        return 1;
    default:
        /* Forwarded on purpose: WM_SETFOCUS/WM_KILLFOCUS (focus bookkeeping
         * the game needs) and WM_CAPTURECHANGED (release tracking) keep
         * the game's input bookkeeping intact; WM_SETCURSOR is handled
         * above because the sprite replaces the OS cursor while visible. */
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
    ui_raw_reg(g_ui.ui_hwnd, 0);
    g_ui.ui_hwnd = NULL;
    g_ui.ui_btns = 0;
    g_ui.cur_set = 0;
    g_ui.real_out = 0;
    g_ui.lhb_cnt = 0;
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
