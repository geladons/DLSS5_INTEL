// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_residual.c - serve-residual compositor (see nr_residual.h).
 *
 *   reply lands (drain)                present, no fresh reply needed
 *   -------------------                ------------------------------
 *   update(src, proc):                 apply(cur):
 *     res = sat8(proc - src)             per pixel: max|cur-src| <= eps
 *     keep src copy                        -> cur += res, else raw (SIMD)
 *
 * The pair is replaced wholesale per reply - no blending, no history.
 * The mask is the gate: res is a positional map of an older frame, so it
 * may only land where the CURRENT raw still matches the pair source.
 * ==========================================================================*/
#include "nr_residual.h"
#include "nr_warp.h"

#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <emmintrin.h>
#define NR_RESIDUAL_SSE2 1
#endif

/* Diff-probe sampling: stride through the frame so ~this many pixels are
 * measured (~4k x 3 channel reads, well under 0.5 ms). */
#define NR_RESIDUAL_SAMPLES 4096
/* Fixed-point divisor so update()'s sat8 clamp matches apply()'s add. */
#define NR_RESIDUAL_CLIP 127

struct NrResidual {
    signed char *res;      /* w*h*4 signed delta; alpha bytes stay 0 */
    unsigned char *src;    /* w*h*4 pair source (the submitted raw frame) */
    size_t bytes;
    uint32_t w, h;
    uint64_t chain;
    unsigned long seq;
    uint64_t stamp_ms;     /* tick the reply landed (age bound) */
    int valid;
    long max_age_ms;
    long diff_x100;        /* scene-CUT probe only: mean |d| bound (x100) */
    long big_pm;           /* scene-CUT probe only: moved-share bound (pm) */
    int big_pix;           /* per-pixel max-channel delta that is "moved" */
    int px_eps;            /* mask eps: max |cur-src| per channel, 0..255 */
    unsigned long st[NR_RESIDUAL_NCLASS];
    long last_diff_x100;
    long last_cov_pm;      /* last apply: share of pixels enhanced, pm */
    struct NrWarp *warp;   /* optional displaced-transfer probe (nr_warp.h) */
    uint32_t *failpx;      /* warp: pixel indices the mask rejected */
    size_t failcap;
    long last_recov_pm;    /* last apply: probe-recovered share, pm */
};

NrResidual *nr_residual_create(void)
{
    NrResidual *r = (NrResidual *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->max_age_ms = 400;
    /* The cut probe now only short-circuits a full scene change - the
     * per-pixel mask owns application, so the defaults sit far above any
     * in-motion mean (a real cut reads ~40-90/255 mean |d|). */
    r->diff_x100 = 3000;   /* 30.00 mean |delta| per channel */
    r->big_pm = 800;       /* <=80% sampled pixels moved >big_pix */
    r->big_pix = 48;
    r->px_eps = 12;        /* +-12/255 per channel keeps residual on
                          * content that still matches its source */
    return r;
}

void nr_residual_destroy(NrResidual *r)
{
    if (!r) return;
    free(r->res);
    free(r->src);
    free(r->failpx);
    free(r);
}

void nr_residual_configure(NrResidual *r, long max_age_ms,
                           long diff_x100, long big_pm, long px_eps)
{
    if (!r) return;
    r->max_age_ms = max_age_ms;
    if (diff_x100 >= 0) r->diff_x100 = diff_x100;
    if (big_pm >= 0) r->big_pm = big_pm;
    if (px_eps >= 0) {
        if (px_eps > 255) px_eps = 255;
        r->px_eps = (int)px_eps;
    }
}

void nr_residual_invalidate(NrResidual *r)
{
    if (r) r->valid = 0;
}

void nr_residual_set_warp(NrResidual *r, struct NrWarp *w)
{
    if (r) r->warp = w;
}

static int nr_residual_reserve(NrResidual *r, size_t bytes)
{
    if (r->bytes >= bytes) return 0;
    {
        signed char *res = (signed char *)realloc(r->res, bytes);
        if (!res) return -1;
        r->res = res;
    }
    {
        unsigned char *src = (unsigned char *)realloc(r->src, bytes);
        if (!src) return -1;
        r->src = src;
    }
    r->bytes = bytes;
    return 0;
}

int nr_residual_update(NrResidual *r, const unsigned char *src,
                       const unsigned char *proc, uint32_t w, uint32_t h,
                       uint64_t chain, unsigned long seq,
                       int channels_ok, uint64_t now_ms)
{
    size_t bytes, i;
    if (!r || !src || !proc || !w || !h || !channels_ok) return 0;
    bytes = (size_t)w * h * 4;
    if (nr_residual_reserve(r, bytes)) return 0;
    /* Fused pass: store the pair source + the signed enhancement delta.
     * res is saturated to +-127 - a larger proc-src gap means content, not
     * enhancement, and must not be carried anyway. Alpha (byte 3) forced
     * to 0: the displayed alpha always comes from the current frame. */
    for (i = 0; i + 3 < bytes; i += 4) {
        int d0 = (int)proc[i]     - (int)src[i];
        int d1 = (int)proc[i + 1] - (int)src[i + 1];
        int d2 = (int)proc[i + 2] - (int)src[i + 2];
        r->res[i]     = (signed char)(d0 > NR_RESIDUAL_CLIP ? NR_RESIDUAL_CLIP
                                      : d0 < -NR_RESIDUAL_CLIP ? -NR_RESIDUAL_CLIP
                                      : d0);
        r->res[i + 1] = (signed char)(d1 > NR_RESIDUAL_CLIP ? NR_RESIDUAL_CLIP
                                      : d1 < -NR_RESIDUAL_CLIP ? -NR_RESIDUAL_CLIP
                                      : d1);
        r->res[i + 2] = (signed char)(d2 > NR_RESIDUAL_CLIP ? NR_RESIDUAL_CLIP
                                      : d2 < -NR_RESIDUAL_CLIP ? -NR_RESIDUAL_CLIP
                                      : d2);
        r->res[i + 3] = 0;
    }
    memcpy(r->src, src, bytes);
    r->w = w;
    r->h = h;
    r->chain = chain;
    r->seq = seq;
    r->stamp_ms = now_ms;
    r->valid = 1;
    return 1;
}

/* Sampled mean |cur - src| per colour channel, plus the share of pixels
 * whose max channel delta clears big_pix (a concentrated real move).
 * ~4096 samples over a uniform stride - cheap enough to run per present. */
static void nr_residual_probe(NrResidual *r, const unsigned char *cur,
                            long *mean_x100, long *big_share_pm)
{
    const size_t npix = r->bytes >> 2;
    const size_t step = npix > NR_RESIDUAL_SAMPLES
                        ? npix / NR_RESIDUAL_SAMPLES : 1;
    uint64_t sum = 0, big = 0, n = 0;
    size_t p;
    for (p = 0; p < npix; p += step) {
        const size_t o = p << 2;
        int d0 = (int)cur[o]     - (int)r->src[o];
        int d1 = (int)cur[o + 1] - (int)r->src[o + 1];
        int d2 = (int)cur[o + 2] - (int)r->src[o + 2];
        int m;
        if (d0 < 0) d0 = -d0;
        if (d1 < 0) d1 = -d1;
        if (d2 < 0) d2 = -d2;
        m = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
        sum += (uint64_t)(d0 + d1 + d2);
        if (m > r->big_pix) big++;
        n++;
    }
    *mean_x100 = n ? (long)((sum * 100) / (n * 3)) : 0;
    *big_share_pm = n ? (long)((big * 1000) / n) : 0;
}

/* Masked compose: out = cur + res where the current raw still matches the
 * pair source, out = cur where the content moved. The per-pixel mask is
 * max(|dB|,|dG|,|dR|) <= px_eps on the CURRENT frame vs the pair source -
 * a hard boundary is fine because BOTH sides of it show current content
 * (enhanced vs unenhanced), never displaced pixels.
 * Returns the APPLIED pixel count (caller converts to per-mille). When
 * failpx is non-NULL the pixel indices that MISSED the mask are appended
 * into it (capacity = npix; the warp probe consumes the list). */
static size_t nr_residual_compose(NrResidual *r, unsigned char *cur,
                                  uint32_t *failpx, unsigned long *nfail)
{
    const size_t bytes = r->bytes;
    const int eps = r->px_eps;
    size_t i = 0, applied = 0;
#if NR_RESIDUAL_SSE2
    const __m128i z = _mm_setzero_si128();
    const __m128i epsv = _mm_set1_epi8((char)eps);
    const __m128i lsb = _mm_set1_epi32(0xFF);          /* dword byte 0 */
    const __m128i noalpha = _mm_set1_epi32(0x00FFFFFF);/* mask the alpha byte
        out of the diff - alpha jitter must not gate the enhancement */
    for (; i + 16 <= bytes; i += 16) {
        __m128i c = _mm_loadu_si128((const __m128i *)(cur + i));
        __m128i s = _mm_loadu_si128((const __m128i *)(r->src + i));
        __m128i d = _mm_loadu_si128((const __m128i *)(r->res + i));
        /* per-byte abs diff of the colour channels (alpha lane -> 0) */
        __m128i ad = _mm_and_si128(
            _mm_or_si128(_mm_subs_epu8(c, s), _mm_subs_epu8(s, c)),
            noalpha);
        /* per-pixel max over the 3 colour bytes, landing at byte 0 of
         * each dword: ad>>1 gives the next channel, ad>>2 the last */
        __m128i m = _mm_max_epu8(ad, _mm_srli_si128(ad, 1));
        m = _mm_max_epu8(m, _mm_srli_si128(ad, 2));
        /* broadcast byte 0 of each dword over the whole dword */
        m = _mm_and_si128(m, lsb);
        m = _mm_or_si128(m, _mm_slli_epi32(m, 8));
        m = _mm_or_si128(m, _mm_slli_epi32(m, 16));
        /* keep = 0xFF bytes where max|d| <= eps (subs saturates to 0) */
        {
            __m128i keep = _mm_cmpeq_epi8(_mm_subs_epu8(m, epsv), z);
            /* sign-extend the int8 residual to i16, saturating add,
             * pack back - same math as the unmasked compose */
            __m128i dlo = _mm_srai_epi16(_mm_unpacklo_epi8(d, d), 8);
            __m128i dhi = _mm_srai_epi16(_mm_unpackhi_epi8(d, d), 8);
            __m128i clo = _mm_unpacklo_epi8(c, z);
            __m128i chi = _mm_unpackhi_epi8(c, z);
            __m128i enh = _mm_packus_epi16(_mm_adds_epi16(clo, dlo),
                                           _mm_adds_epi16(chi, dhi));
            unsigned mk;
            _mm_storeu_si128((__m128i *)(cur + i),
                             _mm_or_si128(_mm_and_si128(keep, enh),
                                          _mm_andnot_si128(keep, c)));
            /* coverage: each pixel owns 4 identical mask bytes; count one */
            mk = (unsigned)_mm_movemask_epi8(keep);
            applied += (size_t)((mk & 1) + ((mk >> 4) & 1)
                                + ((mk >> 8) & 1) + ((mk >> 12) & 1));
            if (failpx && mk != 0xFFFF) {
                int lane;
                for (lane = 0; lane < 4; lane++)
                    if (!(mk & (1u << (lane * 4))))
                        failpx[(*nfail)++] =
                            (uint32_t)((i >> 2) + (size_t)lane);
            }
        }
    }
#endif
    for (; i < bytes; i += 4) {
        int d0 = (int)cur[i]     - (int)r->src[i];
        int d1 = (int)cur[i + 1] - (int)r->src[i + 1];
        int d2 = (int)cur[i + 2] - (int)r->src[i + 2];
        int m;
        if (d0 < 0) d0 = -d0;
        if (d1 < 0) d1 = -d1;
        if (d2 < 0) d2 = -d2;
        m = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
        if (m <= eps) {
            int v;
            applied++;
            v = (int)cur[i]     + (int)r->res[i];
            cur[i]     = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            v = (int)cur[i + 1] + (int)r->res[i + 1];
            cur[i + 1] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            v = (int)cur[i + 2] + (int)r->res[i + 2];
            cur[i + 2] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            /* alpha (byte 3) untouched - res alpha is 0 anyway */
        } else if (failpx) {
            failpx[(*nfail)++] = (uint32_t)(i >> 2);
        }
    }
    return applied;
}

int nr_residual_apply(NrResidual *r, unsigned char *cur,
                      uint32_t w, uint32_t h, uint64_t chain,
                      uint64_t now_ms)
{
    long mean_x100, big_share;
    int verdict;
    if (!r || !cur) return NR_RESIDUAL_NONE;
    /* Any reject means zero pixels took the residual this call - keep
     * last_cov_pm honest so the pres= log never reports a stale share. */
    if (!r->valid) { verdict = NR_RESIDUAL_NONE; goto zero; }
    if (w != r->w || h != r->h || chain != r->chain) {
        verdict = NR_RESIDUAL_EXTENT;
        goto zero;
    }
    if (r->max_age_ms > 0
        && now_ms - r->stamp_ms > (uint64_t)r->max_age_ms) {
        verdict = NR_RESIDUAL_AGE;
        goto zero;
    }
    /* Scene-cut short-circuit: the sampled probe only guards against
     * compositing a totally unrelated pair (huge mean |d| or nearly all
     * pixels moved). In-motion frames PASS it now - the per-pixel mask
     * decides, and moved regions simply keep their raw pixels. */
    nr_residual_probe(r, cur, &mean_x100, &big_share);
    r->last_diff_x100 = mean_x100;
    if (mean_x100 > r->diff_x100 || big_share > r->big_pm) {
        r->last_cov_pm = 0;   /* rejected: nothing applied */
        r->last_recov_pm = 0;
        verdict = NR_RESIDUAL_DIFF;
        goto out;
    }
    {
        size_t applied;
        const size_t npix = r->bytes >> 2;
        if (r->warp) {
            /* warp attached: the masked compose also emits the fail list,
             * then the probe re-tries those pixels at displaced source
             * positions (the same mask evaluated against warped pos). */
            unsigned long nfail = 0, rec;
            if (r->failcap < npix) {
                uint32_t *nb = (uint32_t *)realloc(r->failpx,
                                                   npix * sizeof *nb);
                if (nb) { r->failpx = nb; r->failcap = npix; }
            }
            applied = nr_residual_compose(r, cur, r->failpx, &nfail);
            rec = r->failpx
                  ? nr_warp_probe(r->warp, cur, r->src, r->res,
                                  r->w, r->h, r->failpx, nfail, r->px_eps)
                  : 0;
            r->last_recov_pm = npix
                ? (long)((rec * 1000) / npix) : 0;
            r->last_cov_pm = npix
                ? (long)(((applied + rec) * 1000) / npix) : 0;
        } else {
            r->last_recov_pm = 0;
            applied = nr_residual_compose(r, cur, NULL, NULL);
            r->last_cov_pm = npix
                ? (long)((applied * 1000) / npix) : 0;
        }
    }
    /* Coverage 0 = every pixel missed the mask: the output IS the raw
     * frame, so report the honest reject class (identical pixels either
     * way - the mask already kept cur untouched). */
    verdict = r->last_cov_pm > 0 ? NR_RESIDUAL_APPLIED : NR_RESIDUAL_DIFF;
    goto out;
zero:
    r->last_cov_pm = 0;
    r->last_recov_pm = 0;
out:
    if (verdict >= 0 && verdict < NR_RESIDUAL_NCLASS)
        r->st[verdict]++;
    return verdict;
}

unsigned long nr_residual_stat(const NrResidual *r, int verdict)
{
    if (!r || verdict < 0 || verdict >= NR_RESIDUAL_NCLASS) return 0;
    return r->st[verdict];
}

long nr_residual_last_diff_x100(const NrResidual *r)
{
    return r ? r->last_diff_x100 : 0;
}

long nr_residual_last_cov_pm(const NrResidual *r)
{
    return r ? r->last_cov_pm : 0;
}

long nr_residual_last_recov_pm(const NrResidual *r)
{
    return r ? r->last_recov_pm : 0;
}

int nr_residual_pair(const NrResidual *r, const unsigned char **src,
                     const signed char **res, uint32_t *w, uint32_t *h,
                     uint64_t *chain, uint64_t *stamp_ms)
{
    if (!r || !r->valid) return 0;
    if (src) *src = r->src;
    if (res) *res = r->res;
    if (w) *w = r->w;
    if (h) *h = r->h;
    if (chain) *chain = r->chain;
    if (stamp_ms) *stamp_ms = r->stamp_ms;
    return 1;
}
