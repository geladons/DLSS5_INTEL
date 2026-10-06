// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_residual.c - serve-residual compositor (see nr_residual.h).
 *
 *   reply lands (drain)                present, no fresh reply needed
 *   -------------------                ------------------------------
 *   update(src, proc):                 apply(cur):
 *     res = sat8(proc - src)             probe |cur - src| (4k samples)
 *     keep src copy                      ok -> cur += res (sat, SIMD)
 *
 * The pair is replaced wholesale per reply - no blending, no history.
 * ==========================================================================*/
#include "nr_residual.h"

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
    long diff_x100;
    long big_pm;
    int big_pix;           /* per-pixel max-channel delta that is "moved" */
    unsigned long st[NR_RESIDUAL_NCLASS];
    long last_diff_x100;
};

NrResidual *nr_residual_create(void)
{
    NrResidual *r = (NrResidual *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->max_age_ms = 400;
    r->diff_x100 = 1000;   /* 10.00 mean |delta| per channel */
    r->big_pm = 400;       /* <=40% sampled pixels moved >big_pix */
    r->big_pix = 48;
    return r;
}

void nr_residual_destroy(NrResidual *r)
{
    if (!r) return;
    free(r->res);
    free(r->src);
    free(r);
}

void nr_residual_configure(NrResidual *r, long max_age_ms,
                           long diff_x100, long big_pm)
{
    if (!r) return;
    r->max_age_ms = max_age_ms;
    if (diff_x100 >= 0) r->diff_x100 = diff_x100;
    if (big_pm >= 0) r->big_pm = big_pm;
}

void nr_residual_invalidate(NrResidual *r)
{
    if (r) r->valid = 0;
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

static void nr_residual_compose(NrResidual *r, unsigned char *cur)
{
    const size_t bytes = r->bytes;
    size_t i = 0;
#if NR_RESIDUAL_SSE2
    const __m128i z = _mm_setzero_si128();
    for (; i + 16 <= bytes; i += 16) {
        __m128i c = _mm_loadu_si128((const __m128i *)(cur + i));
        __m128i d = _mm_loadu_si128((const __m128i *)(r->res + i));
        /* sign-extend the int8 residual to i16 lanes */
        __m128i dlo = _mm_srai_epi16(_mm_unpacklo_epi8(d, d), 8);
        __m128i dhi = _mm_srai_epi16(_mm_unpackhi_epi8(d, d), 8);
        __m128i clo = _mm_unpacklo_epi8(c, z);
        __m128i chi = _mm_unpackhi_epi8(c, z);
        __m128i slo = _mm_adds_epi16(clo, dlo);
        __m128i shi = _mm_adds_epi16(chi, dhi);
        _mm_storeu_si128((__m128i *)(cur + i), _mm_packus_epi16(slo, shi));
    }
#endif
    for (; i < bytes; i++) {
        int v = (int)cur[i] + (int)r->res[i];
        cur[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

int nr_residual_apply(NrResidual *r, unsigned char *cur,
                      uint32_t w, uint32_t h, uint64_t chain,
                      uint64_t now_ms)
{
    long mean_x100, big_share;
    int verdict;
    if (!r || !cur) return NR_RESIDUAL_NONE;
    if (!r->valid) { verdict = NR_RESIDUAL_NONE; goto out; }
    if (w != r->w || h != r->h || chain != r->chain) {
        verdict = NR_RESIDUAL_EXTENT;
        goto out;
    }
    if (r->max_age_ms > 0
        && now_ms - r->stamp_ms > (uint64_t)r->max_age_ms) {
        verdict = NR_RESIDUAL_AGE;
        goto out;
    }
    nr_residual_probe(r, cur, &mean_x100, &big_share);
    r->last_diff_x100 = mean_x100;
    if (mean_x100 > r->diff_x100 || big_share > r->big_pm) {
        verdict = NR_RESIDUAL_DIFF;
        goto out;
    }
    nr_residual_compose(r, cur);
    verdict = NR_RESIDUAL_APPLIED;
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
