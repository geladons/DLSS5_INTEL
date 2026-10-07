// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_warp.h - displaced residual transfer for the async serve path.
 *
 * Why this exists (field evidence, 2026-10): the masked residual gate in
 * nr_residual.c rejects every pixel whose CURRENT raw value no longer
 * matches the pair source - on sustained motion that coverage dithers
 * (~55% -> ~40% between daemon replies at the ~85 ms cadence) and the
 * rejected share flashes raw: the owner-visible "shake and color play".
 *
 * A committed block motion field is NOT the right fix on this content -
 * measured on GTA IV benchmark footage (Temp/_warp_probe.py): the motion
 * is heterogeneous (slow pan + local movers + per-frame grain), median
 * block displacement ~0-3 px, and SAD-matched block fields of 4/8/16 px
 * recover ~0 net coverage. Yet ~96% of the rejected pixels DO have a
 * source position within +-24 px whose content still matches the current
 * pixel (per-pixel oracle) - the content is displaced, not destroyed.
 *
 * So instead of committing one vector per block, this module keeps the
 * per-pixel mask as the VERIFIER and probes an ordered displacement set
 * per rejected pixel: candidates are tried smallest-first (a hint vector
 * from last frame's dominant recovered offset is tried first, covering
 * coherent pans in ~1 probe), and the FIRST position whose source content
 * lands within px_eps of the current pixel supplies the residual:
 *
 *     out(p) = cur(p) + res(p + v)   where  maxch|cur(p) - src(p+v)| <= eps
 *
 * Ghost-free by the same construction as the masked path: the residual
 * can only land on a position whose own content matches the current
 * pixel - the warp just widens where that content is allowed to be
 * found. Disoccluded content finds no match and stays raw.
 *
 * Cost: the probe only runs on pixels the mask rejected, with an
 * early-exit first match, a per-pixel probe cap and a strided coverage
 * budget for pathological frames - typical cost is a fraction of a ms,
 * worst case bounded (NR_LAYER_WARP_BUDGET).
 * ==========================================================================*/
#ifndef NR_WARP_H
#define NR_WARP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NrWarp NrWarp;

NrWarp *nr_warp_create(void);
void nr_warp_destroy(NrWarp *w);

/* Policy knobs (pass <0 to leave a bound unchanged):
 *   max_off    - largest probe displacement in px (default 32);
 *   probe_cap  - max candidate probes per failing pixel (default 48);
 *   budget_pm  - per-call probe budget in per-mille of the pixel count
 *                (default 4000 = 4 probes per pixel on average; when the
 *                fail list is huge the probe subsamples it spatially so a
 *                runaway frame degrades coverage uniformly instead of
 *                stalling the present). */
void nr_warp_configure(NrWarp *w, long max_off, long probe_cap,
                       long budget_pm);

/* Probe pass over the pixels the masked compose rejected. fail[] holds
 * failing PIXEL indices (row-major p = y*w + x), nfail entries. For each,
 * tries candidates in order; on the first offset whose src pixel matches
 * cur within px_eps (max channel, alpha ignored) the residual at that
 * displaced position is saturating-added onto cur in place.
 * src/res/cur are w*h*4 (res signed; its alpha byte is 0 by contract).
 * Returns the recovered pixel count. The hint vector is refreshed to the
 * mean of this call's winning offsets for the next call. */
unsigned long nr_warp_probe(NrWarp *w, unsigned char *cur,
                            const unsigned char *src,
                            const signed char *res,
                            uint32_t width, uint32_t height,
                            const uint32_t *fail, unsigned long nfail,
                            int px_eps);

/* Diag counters. */
unsigned long nr_warp_probed(const NrWarp *w);      /* probes last call */
unsigned long nr_warp_fails(const NrWarp *w);       /* fail px last call */
long nr_warp_hint_y(const NrWarp *w);               /* current hint dy */
long nr_warp_hint_x(const NrWarp *w);               /* current hint dx */
long nr_warp_hint_hits(const NrWarp *w);            /* last-call px that
                                                     resolved via hint */

#ifdef __cplusplus
}
#endif

#endif /* NR_WARP_H */
