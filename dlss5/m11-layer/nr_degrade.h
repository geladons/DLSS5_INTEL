// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_degrade.h - degraded-mode policy for the present layer's daemon path.
 *
 * Pure state + counters, no Vulkan or socket knowledge (nr_framegen style).
 * The layer owns all pixels and logging; this module only decides.
 *
 * Two concerns:
 *  - degenerate swapchains: tiny menu/letterbox sub-elements below a minimum
 *    extent are passed through native without a daemon/engine call (a 160x28
 *    chain otherwise re-inits the daemon engine on every appearance).
 *  - flap control: a transient exchange() failure re-blits the LAST
 *    PROCESSED result for a bounded streak instead of strobing
 *    native<->processed; only a long streak declares the daemon down.
 *    daemon_known_up is set ONLY after the first successful answer, so a
 *    daemon absent at launch still passes native from frame one.
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
    NR_DEGRADE_LOG_DOWN,        /* streak crossed the flap window */
    NR_DEGRADE_LOG_RECOVERY,    /* first success after a fail streak */
    NR_DEGRADE_LOG_DEGENERATE   /* newly seen degenerate extent */
};

typedef struct NrDegrade {
    unsigned window;             /* fail streak that still holds (default 8) */
    unsigned min_extent;         /* below this on either axis -> degenerate */
    unsigned consecutive_fail;   /* consecutive exchange() failures */
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

/* exchange() answered: streak resets and the daemon is marked up. Returns
 * NR_DEGRADE_LOG_RECOVERY when this answer ended a fail streak. */
int nr_degrade_note_success(NrDegrade *d);

/* exchange() failed: the streak grows. *hold is non-zero while the daemon is
 * known up and the streak is inside the window - the caller may re-blit the
 * last processed result for this size. Returns the log code for this fail. */
int nr_degrade_note_fail(NrDegrade *d, int *hold);

/* The caller re-blitted the last processed result for this present. */
void nr_degrade_note_held(NrDegrade *d);

#ifdef __cplusplus
}
#endif

#endif /* NR_DEGRADE_H */
