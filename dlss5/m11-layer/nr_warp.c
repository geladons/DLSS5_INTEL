// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_warp.c - displaced residual transfer (see nr_warp.h).
 *
 * Probe order, built once at create/configure (pixel-space offsets):
 *   1. hint = last frame's dominant recovered offset (hint first, then
 *      its +-1 ring) - coherent motion resolves in ~1-9 probes instead
 *      of ~40;
 *   2. dense Chebyshev rings max(|dy|,|dx|) = 1..4 (80 offsets) - small
 *      displacements first: the least distortion that satisfies the mask;
 *   3. sparse far probes out to max_off (radii 6,8,12,16,24,32,40,48) on
 *      the 8 compass directions plus the 8 knight-slope directions
 *      (+-1,+-2)/(+-2,+-1) - fast pans/whips, including off-axis ones.
 *      Knight hits land within ~1-2 px of the true offset; the same
 *      px_eps match accepts that slack on smooth content.
 * The cap cuts the tail of that list per pixel; the budget bounds the
 * whole call (huge fail lists are subsampled with a uniform stride so an
 * exhausted budget spreads the shortfall instead of banding the image).
 * ==========================================================================*/
#include "nr_warp.h"

#include <stdlib.h>
#include <string.h>

/* Candidate list bound: dense rings r<=4 (80) + far compass probes
 * (8 dirs x 8 radii = 64) + far knight probes (8 dirs x 4 radii = 32)
 * = <=176. */
#define NR_WARP_MAXCAND 200
#define NR_WARP_DEF_MAXOFF 32
#define NR_WARP_DEF_CAP 48
#define NR_WARP_DEF_BUDGET_PM 4000

/* The hint is the mean of the last call's winning offsets - coherent
 * motion resolves the next frame in ~1-9 probes. A binned histogram
 * quantises small displacements to the zero bin (bin centre = 0), so
 * the mean is used instead; a multi-cluster frame averages out and the
 * dense rings still cover it as fallback. */

struct NrWarp {
    int16_t cand[NR_WARP_MAXCAND][2];   /* ordered (dy,dx) probe offsets */
    int ncand;
    long max_off;
    long probe_cap;
    long budget_pm;
    int hint_dy, hint_dx;               /* mean recovered offset */
    unsigned long last_probes;          /* probes issued, last call */
    unsigned long last_fails;           /* fail list length, last call */
    unsigned long last_hint_hits;       /* resolves via the hint entries */
    unsigned long tot_recovered;
};

static void nr_warp_build_cand(NrWarp *w)
{
    int n = 0, r, dy, dx;
    static const int far_r[] = { 6, 8, 12, 16, 24, 32, 40, 48 };
    static const int knight_r[] = { 8, 16, 24, 32 };
    static const int dirs[8][2] = {
        { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 },
        { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }
    };
    static const int kdirs[8][2] = {
        { 1, 2 }, { 1, -2 }, { -1, 2 }, { -1, -2 },
        { 2, 1 }, { 2, -1 }, { -2, 1 }, { -2, -1 }
    };
    /* dense rings r = 1..4, smallest radius first */
    for (r = 1; r <= 4 && r <= w->max_off; r++) {
        for (dy = -r; dy <= r; dy++)
            for (dx = -r; dx <= r; dx++)
                if ((dy == r || dy == -r || dx == r || dx == -r)
                    && n < NR_WARP_MAXCAND) {
                    w->cand[n][0] = (int16_t)dy;
                    w->cand[n][1] = (int16_t)dx;
                    n++;
                }
    }
    /* sparse far probes on the 8 directions */
    {
        unsigned k, d;
        for (k = 0; k < sizeof(far_r) / sizeof(far_r[0]); k++) {
            int rr = far_r[k];
            if (rr > w->max_off || rr <= 4) continue;
            for (d = 0; d < 8 && n < NR_WARP_MAXCAND; d++) {
                w->cand[n][0] = (int16_t)(dirs[d][0] * rr);
                w->cand[n][1] = (int16_t)(dirs[d][1] * rr);
                n++;
            }
        }
        for (k = 0; k < sizeof(knight_r) / sizeof(knight_r[0]); k++) {
            int rr = knight_r[k];
            if (rr > w->max_off) continue;
            for (d = 0; d < 8 && n < NR_WARP_MAXCAND; d++) {
                /* unit knight step (1,2)/(2,1) scaled by rr/2 puts the
                 * longer leg on the ring radius (e.g. r=8 -> (4,8)) */
                int sy = kdirs[d][0], sx = kdirs[d][1];
                w->cand[n][0] = (int16_t)(sy * (rr / 2));
                w->cand[n][1] = (int16_t)(sx * (rr / 2));
                n++;
            }
        }
    }
    w->ncand = n;
}

NrWarp *nr_warp_create(void)
{
    NrWarp *w = (NrWarp *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->max_off = NR_WARP_DEF_MAXOFF;
    w->probe_cap = NR_WARP_DEF_CAP;
    w->budget_pm = NR_WARP_DEF_BUDGET_PM;
    nr_warp_build_cand(w);
    return w;
}

void nr_warp_destroy(NrWarp *w)
{
    free(w);
}

void nr_warp_configure(NrWarp *w, long max_off, long probe_cap,
                       long budget_pm)
{
    if (!w) return;
    if (max_off >= 0) {
        if (max_off > 128) max_off = 128;
        w->max_off = max_off;
        nr_warp_build_cand(w);
    }
    if (probe_cap >= 0) {
        if (probe_cap > NR_WARP_MAXCAND) probe_cap = NR_WARP_MAXCAND;
        w->probe_cap = probe_cap;
    }
    if (budget_pm >= 0) w->budget_pm = budget_pm;
}

/* One failing pixel: probe candidates until the displaced source matches
 * cur within eps. On a match, res at the winning position is
 * saturating-added onto cur in place.
 * Returns 1 resolved / 0 unresolved; *out_dy/*out_dx get the winning
 * offset, *out_probes the candidates evaluated, *via_hint whether the
 * hint block supplied it. */
static int nr_warp_probe_px(NrWarp *w, unsigned char *cur,
                            const unsigned char *src,
                            const signed char *res,
                            uint32_t width, uint32_t height,
                            uint32_t p, int px_eps,
                            int *out_dy, int *out_dx,
                            unsigned long *out_probes, int *via_hint)
{
    const uint32_t x = p % width;
    const uint32_t y = p / width;
    const size_t o = (size_t)p << 2;
    const int c0 = cur[o], c1 = cur[o + 1], c2 = cur[o + 2];
    const int cap = w->probe_cap < w->ncand ? (int)w->probe_cap
                                          : w->ncand;
    unsigned long probes = 0;
    int i;
    *via_hint = 0;
    /* hint first (+-1 ring): last frame's dominant displacement resolves
     * a coherent move in ~1 probe */
    if (w->hint_dy || w->hint_dx) {
        int hdy, hdx;
        for (hdy = -1; hdy <= 1; hdy++)
            for (hdx = -1; hdx <= 1; hdx++) {
                int nx = (int)x + w->hint_dx + hdx;
                int ny = (int)y + w->hint_dy + hdy;
                const unsigned char *s;
                int d0, d1, d2, m;
                if ((unsigned)nx >= width || (unsigned)ny >= height)
                    continue;
                probes++;
                s = src + (((size_t)ny * width + (uint32_t)nx) << 2);
                d0 = c0 - s[0]; if (d0 < 0) d0 = -d0;
                d1 = c1 - s[1]; if (d1 < 0) d1 = -d1;
                d2 = c2 - s[2]; if (d2 < 0) d2 = -d2;
                m = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
                if (m <= px_eps) {
                    *out_dy = w->hint_dy + hdy;
                    *out_dx = w->hint_dx + hdx;
                    *via_hint = 1;
                    goto apply;
                }
            }
    }
    for (i = 0; i < cap; i++) {
        int nx = (int)x + w->cand[i][1];
        int ny = (int)y + w->cand[i][0];
        const unsigned char *s;
        int d0, d1, d2, m;
        if ((unsigned)nx >= width || (unsigned)ny >= height)
            continue;
        probes++;
        s = src + (((size_t)ny * width + (uint32_t)nx) << 2);
        d0 = c0 - s[0]; if (d0 < 0) d0 = -d0;
        d1 = c1 - s[1]; if (d1 < 0) d1 = -d1;
        d2 = c2 - s[2]; if (d2 < 0) d2 = -d2;
        m = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
        if (m <= px_eps) {
            *out_dy = w->cand[i][0];
            *out_dx = w->cand[i][1];
            goto apply;
        }
    }
    *out_probes = probes;
    return 0;
apply:
    {
        const signed char *rp =
            res + (((size_t)(*out_dy + (int)y) * width
                    + (uint32_t)(*out_dx + (int)x)) << 2);
        int v;
        v = c0 + (int)rp[0];
        cur[o]     = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        v = c1 + (int)rp[1];
        cur[o + 1] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        v = c2 + (int)rp[2];
        cur[o + 2] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    *out_probes = probes;
    return 1;
}

/* mean of the winning offsets -> next call's hint; fewer than 8 winners
 * means no coherent motion and clears the hint (the +-1 ring around a
 * garbage hint only wastes probes). */
static void nr_warp_hint_update(NrWarp *w, long sdy, long sdx,
                                unsigned long nwinner)
{
    if (nwinner < 8) {
        w->hint_dy = w->hint_dx = 0;
        return;
    }
    w->hint_dy = (int)(sdy / (long)nwinner);
    w->hint_dx = (int)(sdx / (long)nwinner);
}

unsigned long nr_warp_probe(NrWarp *w, unsigned char *cur,
                            const unsigned char *src,
                            const signed char *res,
                            uint32_t width, uint32_t height,
                            const uint32_t *fail, unsigned long nfail,
                            int px_eps)
{
    unsigned long recovered = 0, probes = 0, hint_hits = 0, k;
    unsigned long stride = 1;
    long sdy = 0, sdx = 0;
    w->last_fails = nfail;
    if (!nfail) {
        w->last_probes = 0;
        w->last_hint_hits = 0;
        w->hint_dy = w->hint_dx = 0;
        return 0;
    }
    /* Budget: probes_total <= npix * budget_pm/1000 (hard stop below).
     * The fail list is walked with a uniform stride so an exhausted
     * budget spreads the shortfall over the frame instead of leaving a
     * repaired top half above a raw bottom half. The stride is sized by
     * the EXPECTED per-pixel cost (a resolved pixel stops early, way
     * under the worst-case cap), not by the cap itself - sizing by cap
     * would starve the list even when the budget was never hit. */
    {
        uint64_t npix = (uint64_t)width * height;
        uint64_t est = (uint64_t)((w->hint_dy || w->hint_dx) ? 6 : 16);
        uint64_t budget = npix * (uint64_t)w->budget_pm / 1000ull;
        uint64_t allowed_px;
        if (est > (uint64_t)w->probe_cap && w->probe_cap)
            est = (uint64_t)w->probe_cap;
        allowed_px = budget / est;
        if (allowed_px >= nfail) {
            stride = 1;
        } else {
            if (!allowed_px) allowed_px = 1;
            stride = (unsigned long)((nfail + allowed_px - 1)
                                     / allowed_px);
        }
    }
    for (k = 0; k < nfail; k += stride) {
        int dy = 0, dx = 0, via_hint = 0;
        unsigned long pp = 0;
        if (nr_warp_probe_px(w, cur, src, res, width, height,
                             fail[k], px_eps, &dy, &dx, &pp, &via_hint)) {
            recovered++;
            hint_hits += (unsigned long)via_hint;
            sdy += dy; sdx += dx;
        }
        probes += pp;
        if (probes > (uint64_t)((uint64_t)width * height
                                * (uint64_t)w->budget_pm / 1000ull))
            break;      /* hard stop: keep the present on budget */
    }
    w->last_probes = probes;
    w->last_hint_hits = hint_hits;
    w->tot_recovered += recovered;
    nr_warp_hint_update(w, sdy, sdx, recovered);
    return recovered;
}

unsigned long nr_warp_probed(const NrWarp *w) { return w ? w->last_probes : 0; }
unsigned long nr_warp_fails(const NrWarp *w)  { return w ? w->last_fails : 0; }
long nr_warp_hint_y(const NrWarp *w)          { return w ? w->hint_dy : 0; }
long nr_warp_hint_x(const NrWarp *w)          { return w ? w->hint_dx : 0; }
long nr_warp_hint_hits(const NrWarp *w)
{
    return w ? (long)w->last_hint_hits : 0;
}
