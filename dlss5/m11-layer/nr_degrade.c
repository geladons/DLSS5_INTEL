// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_degrade.c - degraded-mode policy (see nr_degrade.h).
 * ==========================================================================*/
#include <string.h>
#include "nr_degrade.h"

void nr_degrade_init(NrDegrade *d)
{
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->window = 8;
    d->min_extent = 64;
}

int nr_degrade_is_degenerate(NrDegrade *d, uint32_t w, uint32_t h, int *log)
{
    if (log) *log = NR_DEGRADE_LOG_NONE;
    if (!d) return 0;
    if (w >= d->min_extent && h >= d->min_extent) return 0;
    d->degenerate++;
    if (log && (!d->deg_seen || d->deg_w != w || d->deg_h != h)) {
        d->deg_seen = 1;
        d->deg_w = w;
        d->deg_h = h;
        *log = NR_DEGRADE_LOG_DEGENERATE;
    }
    return 1;
}

int nr_degrade_note_success(NrDegrade *d)
{
    unsigned streak;
    if (!d) return NR_DEGRADE_LOG_NONE;
    streak = d->consecutive_fail;
    d->consecutive_fail = 0;
    d->daemon_known_up = 1;
    d->hiccup_logged = 0;
    d->down_logged = 0;
    return streak ? NR_DEGRADE_LOG_RECOVERY : NR_DEGRADE_LOG_NONE;
}

int nr_degrade_note_fail(NrDegrade *d, int *hold)
{
    if (hold) *hold = 0;
    if (!d) return NR_DEGRADE_LOG_NONE;
    d->consecutive_fail++;
    /* Known-up daemon inside the flap window: the caller may hold the last
     * processed frame. A daemon that never answered never enters this
     * branch - it stays native from the very first fail. */
    if (d->daemon_known_up && d->consecutive_fail <= d->window) {
        if (hold) *hold = 1;
        if (!d->hiccup_logged) {
            d->hiccup_logged = 1;
            return NR_DEGRADE_LOG_HICCUP;
        }
        return NR_DEGRADE_LOG_NONE;
    }
    /* Streak past the window: the daemon is considered down (also covers a
     * daemon that was never up - the first 8 fails stay silent/native and
     * the ninth emits the single "down" line). */
    if (d->consecutive_fail > d->window) {
        d->daemon_known_up = 0;
        if (!d->down_logged) {
            d->down_logged = 1;
            return NR_DEGRADE_LOG_DOWN;
        }
    }
    return NR_DEGRADE_LOG_NONE;
}

void nr_degrade_note_held(NrDegrade *d)
{
    if (d) d->held++;
}
