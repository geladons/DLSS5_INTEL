// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_residual.h - present-time "serve-residual" compositor for the async
 * daemon path.
 *
 * The async worker (nr_async.c) decoupled the game from the ~60-85 ms
 * daemon round-trip, but the serve-best policy still re-shows the freshest
 * COMPLETED reply: on animated content the display stream then moves at
 * the daemon's reply rate (~12 fps) while the game presents far faster -
 * the slideshow the owner saw.
 *
 * This module keeps the newest (source_raw, proc_reply) pair the layer
 * hands it. The enhancement delta res = proc - source is largely content-
 * independent (denoise/sharpen/tone), so while the CURRENT raw frame stays
 * close to the pair's source, out = cur + res shows CURRENT content with
 * the freshest available enhancement instead of the stale processed
 * frame. It is the same residual-transfer idea as framegen mode 1, minus
 * the motion warp: the similarity bound below replaces the warp.
 *
 * Staleness policy - the AGE bound must pass, else the caller falls back
 * to the serve-best order (stale processed -> raw):
 *   - age:     now_ms - reply_ms <= max_age_ms; a daemon that stops
 *              answering lets the pair expire instead of riding forever.
 *   - cut:     a coarse sampled probe (mean |cur - src| + moved share)
 *              still trips NR_RESIDUAL_DIFF on a full scene CUT - there is
 *              nothing to enhance and the pass is skipped wholesale.
 *
 * Per-pixel mask (the actual gate): the residual is a positional map of
 * an OLDER frame - applying it frame-wide ghosts every moved edge even
 * when the sampled mean stays inside the bound. So the compose is masked
 * per pixel: the delta lands only where the CURRENT raw still matches the
 * pair source (max |dB|,|dG|,|dR| <= px_eps). Where content moved, the
 * output is the current raw pixel untouched - ghosts cannot occur BY
 * CONSTRUCTION (the residual can never sit on displaced content), and
 * moving silhouettes show unenhanced current pixels instead of a hard
 * frame-level apply/reject pump. A frame may pass the cut probe yet take
 * residual on only its static regions; coverage (below) reports how much.
 *
 * The pair is FULL-RES already: the daemon replies at the submitted extent
 * (the render scale lives inside the chain's residual reconstruction -
 * verified: replies are chain-extent w*h*4 bytes and are blitted at that
 * extent with no scaling), so res is computed/applied at the presented
 * extent. The math is channel-order agnostic (per-byte delta); only the
 * alpha byte (offset 3 for both BGRA and RGBA) is special - its residual
 * is forced to 0 so the displayed alpha always comes from the current
 * frame. Packed 10-bit formats are not per-byte channels - the caller
 * flags them and the pair is rejected.
 *
 * Anchor invariant: apply() writes ONLY into the caller's display buffer
 * (the staging frame about to be blitted). It never feeds back into the
 * held result / FG mid / earlier compare state, so a residual-composited
 * display frame can never become a mode-2 anchor.
 *
 * Threading: present-thread only, like the rest of the serve policy. No
 * allocs after the extent is sized: update() is O(frame) once per reply,
 * apply() is a ~4k-sample diff probe plus one SIMD saturating-add pass -
 * ~1 ms at 800x600.
 * ==========================================================================*/
#ifndef NR_RESIDUAL_H
#define NR_RESIDUAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NrResidual NrResidual;

/* apply() verdict - also the diag class for the serve log. */
enum {
    NR_RESIDUAL_NONE = 0,   /* no usable pair held */
    NR_RESIDUAL_EXTENT,     /* extent/chain mismatch */
    NR_RESIDUAL_AGE,        /* pair older than the time bound */
    NR_RESIDUAL_DIFF,       /* scene cut, or zero pixels matched the mask */
    NR_RESIDUAL_APPLIED,    /* cur holds cur_raw + masked residual */
    NR_RESIDUAL_NCLASS
};

NrResidual *nr_residual_create(void);
void nr_residual_destroy(NrResidual *r);

/* Policy bounds. max_age_ms <= 0 disables the age bound. diff_x100 /
 * big_pm are the scene-cut short-circuit (sampled mean |cur-src| per
 * channel in hundredths, and the allowed share of sampled pixels moved
 * > big_pix in per-mille) - they only skip the pass on full cuts; the
 * per-pixel mask is what gates application. px_eps is that mask: a pixel
 * takes the residual only while every colour channel of the current raw
 * stays within +-px_eps LSB of the pair source (hard mask - both sides of
 * the boundary show CURRENT content, so no soft edge is needed).
 * Passing <0 leaves a bound unchanged. */
void nr_residual_configure(NrResidual *r, long max_age_ms,
                           long diff_x100, long big_pm, long px_eps);

/* Drop the held pair (knob turn invalidates the enhancement it encodes). */
void nr_residual_invalidate(NrResidual *r);

/* Store the newest reply pair: src = the exact payload the daemon was
 * sent, proc = its full-res reply (both w*h*4). Computes res once here.
 * channels_ok must be 0 for non-per-byte formats (packed A2*10). Returns
 * 1 stored, 0 skipped (bad args/format - previous pair kept). */
int nr_residual_update(NrResidual *r, const unsigned char *src,
                       const unsigned char *proc, uint32_t w, uint32_t h,
                       uint64_t chain, unsigned long seq,
                       int channels_ok, uint64_t now_ms);

/* Try to composite cur (w*h*4 BGRA/RGBA, in/out) with the held residual
 * under the per-pixel mask. Returns an NR_RESIDUAL_* verdict; on APPLIED
 * cur has been rewritten in place (moved pixels keep their raw value).
 * DIFF means the cut probe tripped OR every pixel missed the mask - in
 * both cases cur is untouched. */
int nr_residual_apply(NrResidual *r, unsigned char *cur,
                      uint32_t w, uint32_t h, uint64_t chain,
                      uint64_t now_ms);

/* Diag: per-verdict counters, the last sampled mean diff (x100), and the
 * coverage of the last apply - the share of pixels that actually took the
 * residual, in per-mille (1000 = every pixel; this is the field metric
 * that distinguishes "enhancement working" from "silently all-raw"). */
unsigned long nr_residual_stat(const NrResidual *r, int verdict);
long nr_residual_last_diff_x100(const NrResidual *r);
long nr_residual_last_cov_pm(const NrResidual *r);

#ifdef __cplusplus
}
#endif

#endif /* NR_RESIDUAL_H */
