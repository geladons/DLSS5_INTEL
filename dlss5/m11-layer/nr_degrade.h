// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_degrade.h - degraded-mode policy for the present layer's daemon path.
 *
 * Pure state + counters, no Vulkan or socket knowledge (nr_framegen style).
 * The layer owns all pixels and logging; this module only decides.
 *
 * Three concerns:
 *  - degenerate swapchains: tiny menu/letterbox sub-elements below a minimum
 *    extent are passed through native without a daemon/engine call (a 160x28
 *    chain otherwise re-inits the daemon engine on every appearance).
 *  - flap hold: a transport-level exchange() failure (connected but the
 *    daemon did not answer in time) re-blits the LAST PROCESSED result
 *    instead of showing the raw frame. While the daemon is alive and a
 *    held result for the extent exists, a raw frame is never inserted into
 *    a processed stream - that alternation reads as flicker. The hold ends
 *    only when no success has been seen for NR_DEGRADE_STALE_MS.
 *  - probe pacing: a connect()-refused failure means the daemon is absent
 *    (and the call is cheap); a transport timeout means it is busy and a
 *    retry every present would pile up TCP backlog. In both cases the next
 *    presents are served from the held frame (or native when nothing is
 *    held) and only every 8th (flap) / 32nd (down) present probes the
 *    socket again. This keeps the present loop responsive while the
 *    daemon drains.
 * ==========================================================================*/
#ifndef NR_DEGRADE_H
#define NR_DEGRADE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log codes returned by the note_* / is_degenerate calls. */
enum {
    NR_DEGRADE_LOG_NONE = 0,
    NR_DEGRADE_LOG_HICCUP,      /* first held fail of a streak */
    NR_DEGRADE_LOG_DOWN,        /* daemon considered down (absent or stale) */
    NR_DEGRADE_LOG_RECOVERY,    /* first success after a fail streak */
    NR_DEGRADE_LOG_DEGENERATE   /* newly seen degenerate extent */
};

/* Kinds of exchange() failure handed to nr_degrade_note_fail. */
enum {
    NR_DEGRADE_FAIL_TRANSPORT = 0, /* connected, but send/recv timed out */
    NR_DEGRADE_FAIL_CONNECT        /* connect() refused: daemon absent */
};

/* With no successful answer for this long the daemon is treated as gone
 * and the stream falls back to native (a single transition, not a strobe). */
#define NR_DEGRADE_STALE_MS 15000u

/* Probe pacing between real socket attempts. A flap keeps a held frame for
 * the skipped presents; a down daemon passes native between probes. */
#define NR_DEGRADE_PROBE_FLAP 8u
#define NR_DEGRADE_PROBE_DOWN 32u

typedef struct NrDegrade {
    unsigned min_extent;         /* below this on either axis -> degenerate */
    unsigned consecutive_fail;   /* consecutive transport failures */
    unsigned cooldown;           /* presents skipped before the next probe */
    uint64_t last_success_ms;    /* GetTickCount64 of the last good answer */
    int daemon_known_up;         /* set only after the first real answer */
    int hiccup_logged;           /* hiccup line already emitted this streak */
    int down_logged;             /* "considered down" already emitted */
    int deg_seen;                /* a degenerate extent was logged */
    uint32_t deg_w, deg_h;       /* last logged degenerate extent */
    unsigned long held;          /* presents served from the last result */
    unsigned long degenerate;    /* presents passed through as degenerate */
} NrDegrade;

void nr_degrade_init(NrDegrade *d);

/* Non-zero when (w,h) falls below the degenerate floor on either axis.
 * Counts the present; *log gets NR_DEGRADE_LOG_DEGENERATE the first time a
 * new degenerate extent is seen, NR_DEGRADE_LOG_NONE otherwise. */
int nr_degrade_is_degenerate(NrDegrade *d, uint32_t w, uint32_t h, int *log);

/* Call before every real socket attempt. Returns non-zero when this present
 * may hit the socket; zero means serve this present from the held result or
 * natively without touching the network (probe pacing). */
int nr_degrade_probe_now(NrDegrade *d);

/* exchange() answered: streak resets and the daemon is marked up. Returns
 * NR_DEGRADE_LOG_RECOVERY when this answer ended a fail streak. */
int nr_degrade_note_success(NrDegrade *d, uint64_t now_ms);

/* exchange() failed. fail_kind is one of NR_DEGRADE_FAIL_*. *hold is
 * non-zero when the caller may re-blit the last processed result for this
 * extent (daemon alive and the outage younger than NR_DEGRADE_STALE_MS).
 * Returns the log code for this fail. */
int nr_degrade_note_fail(NrDegrade *d, int fail_kind, uint64_t now_ms,
                         int *hold);

/* The caller re-blitted the last processed result for this present. */
void nr_degrade_note_held(NrDegrade *d);

#ifdef __cplusplus
}
#endif

#endif /* NR_DEGRADE_H */
