/* SPDX-License-Identifier: Apache-2.0 */
/* =====================================================================
 * nr_ui - in-process in-game UI overlay, layer side of the m13 overlay.
 *
 * Two responsibilities behind a small interface:
 *   - composite: straight-alpha blend of the shared-memory UI bitmap (and
 *     the cursor sprite) into the host-visible staging frame before the
 *     write-back copy in transfer(). CPU-side, no GPU pipeline. The
 *     producer-published hole rect splits blend rows so the center of the
 *     frame stays the untouched game.
 *   - input: while the overlay is up, the guarded wndproc routes window
 *     messages by REGION - pointer on the band (bitmap minus hole) -> ring
 *     event + eaten; pointer in the hole/outside -> game unchanged; key
 *     messages eaten only while the producer's kbd_capture flag is set.
 *     It tracks a logical cursor position and ClipCursors to the panel
 *     rect unless the producer sets no_clip (windowed framed overlay).
 *
 * The shared-memory section "Local\DLSS5_UI_<pid>" is a frozen contract
 * with the m13 producer; the layout lives in nr_ui.c. The layer OPENS the
 * mapping lazily (never creates it); a missing section or a bad magic is
 * zero behaviour - identical to a build without the overlay.
 * ===================================================================== */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame handed to the compositor: the host-visible staging pixels plus the
 * geometry the layer already knows. The UI module never sees the layer's
 * device_data/swapchain_data internals - this POD is the whole interface. */
typedef struct NrUiFrame {
    unsigned char *mapped;  /* staging pixels, 4 bytes per pixel */
    uint32_t width;         /* frame extent, pixels */
    uint32_t height;        /* frame extent, pixels */
    int rgba_order;         /* 0 = bytes are B,G,R,A; 1 = R,G,B,A */
} NrUiFrame;

/* Present-path entry, called on every write-back transfer. Lazily opens
 * the section (retries each call until success), republishes the logical
 * cursor position, and alpha-blends the UI bitmap + cursor sprite into the
 * frame while the overlay is active. No-op for anything else. */
void nr_ui_composite(const NrUiFrame *frame);

/* Overlay-visible gate (mapping open, magic ok, flags&1, producer
 * heartbeat fresh). Pure reads, no syscalls - safe per present. */
int nr_ui_active(void);

/* Wndproc-path gate: overlay visible with a live producer. Performs the
 * HWND association (the first guarded window that observes the flag owns
 * the client-coordinate space) and the ClipCursor rising/falling edge
 * (skipped while the producer's no_clip flag is set). Pure memory reads -
 * safe per message. */
int nr_ui_capture_active(HWND hwnd);

/* Input filter for the guarded wndproc; call only when
 * nr_ui_capture_active() is true. Returns nonzero when the message was
 * consumed by the overlay (ring event pushed - do NOT forward to the game),
 * zero when it must flow to the previous wndproc. Per-region routing:
 * mouse messages are eaten only when the pointer lands on the band
 * (inside the bitmap rect, outside the hole) or a captured button is
 * held; key/char messages only while the producer's kbd_capture flag is
 * set; focus/capture/cursor bookkeeping and anything unlisted always
 * flows to the game. */
int nr_ui_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* Release ClipCursor if we own it, unmap the shared view, reset the
 * module state. Idempotent; called from the unguard path (which runs on
 * instance destroy and on a real DLL_PROCESS_DETACH). */
void nr_ui_detach(void);

#ifdef __cplusplus
}
#endif
