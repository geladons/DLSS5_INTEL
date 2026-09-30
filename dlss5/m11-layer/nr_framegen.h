// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_framegen.h - frame-generation cadence bookkeeping for the present layer.
 * Pure state + counters; the layer calls these once per present and routes
 * each frame to "process" or "generate" (residual transfer via d5c_chain).
 * ==========================================================================*/
#ifndef NR_FRAMEGEN_H
#define NR_FRAMEGEN_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NR_FG_PROCESSED = 0,
    NR_FG_GENERATED = 1,
    NR_FG_NATIVE = 2
};

typedef struct NrFrameGen {
    long configured_every;      /* NR_LAYER_LIVE, <=1 means every present */
    unsigned long presents, processed, generated, native;
    int unavailable_logged;
} NrFrameGen;

void nr_fg_init(NrFrameGen *fg, long configured_every);
/* configured <= 1 -> 2 when FG is on and usable, else 1; configured > 1 wins. */
long nr_fg_effective_every(const NrFrameGen *fg, int enabled, int available);
/* Counts the present; returns non-zero when this frame should be processed. */
int nr_fg_should_process(NrFrameGen *fg, unsigned long frame_counter,
                         int enabled, int available);
void nr_fg_note(NrFrameGen *fg, int kind);
int nr_fg_stats_due(const NrFrameGen *fg);   /* every 300 presents */

#ifdef __cplusplus
}
#endif

#endif /* NR_FRAMEGEN_H */
