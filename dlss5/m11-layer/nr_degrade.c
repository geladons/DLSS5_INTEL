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

int nr_degrade_probe_now(NrDegrade *d)
{
    if (!d) return 1;
    if (d->cooldown) {
        d->cooldown--;
        return 0;
    }
    return 1;
}

int nr_degrade_note_success(NrDegrade *d, uint64_t now_ms)
{
    unsigned streak;
    if (!d) return NR_DEGRADE_LOG_NONE;
    streak = d->consecutive_fail;
    d->consecutive_fail = 0;
    d->cooldown = 0;
    d->daemon_known_up = 1;
    d->last_success_ms = now_ms;
    d->hiccup_logged = 0;
    d->down_logged = 0;
    return streak ? NR_DEGRADE_LOG_RECOVERY : NR_DEGRADE_LOG_NONE;
}

int nr_degrade_note_fail(NrDegrade *d, int fail_kind, uint64_t now_ms,
                         int *hold)
{
    if (hold) *hold = 0;
    if (!d) return NR_DEGRADE_LOG_NONE;

    /* connect() refused: nobody listens on the port - the daemon is absent.
     * Mark down immediately and pace probes slowly; the failed call is
     * cheap so no hold is needed for responsiveness. */
    if (fail_kind == NR_DEGRADE_FAIL_CONNECT) {
        d->daemon_known_up = 0;
        d->consecutive_fail = 0;
        d->cooldown = NR_DEGRADE_PROBE_DOWN - 1;
        if (!d->down_logged) {
            d->down_logged = 1;
            d->hiccup_logged = 0;
            return NR_DEGRADE_LOG_DOWN;
        }
        return NR_DEGRADE_LOG_NONE;
    }

    /* Transport failure after a successful connect: the daemon is there but
     * did not answer in time (saturated or wedged mid-frame). */
    d->consecutive_fail++;

    /* No success for longer than the stale window: treat the daemon as
     * gone. The stream falls back to native and probes slowly - a single
     * transition, not per-frame alternation. */
    if (d->last_success_ms &&
        now_ms - d->last_success_ms > NR_DEGRADE_STALE_MS) {
        d->daemon_known_up = 0;
        d->cooldown = NR_DEGRADE_PROBE_DOWN - 1;
        if (!d->down_logged) {
            d->down_logged = 1;
            d->hiccup_logged = 0;
            return NR_DEGRADE_LOG_DOWN;
        }
        return NR_DEGRADE_LOG_NONE;
    }

    /* Fresh outage on a known-up daemon: the caller may keep showing the
     * last processed frame; the next real socket attempt is paced. */
    if (d->daemon_known_up) {
        if (hold) *hold = 1;
        d->cooldown = NR_DEGRADE_PROBE_FLAP - 1;
        if (!d->hiccup_logged) {
            d->hiccup_logged = 1;
            return NR_DEGRADE_LOG_HICCUP;
        }
        return NR_DEGRADE_LOG_NONE;
    }

    /* Transport fail on a daemon that never answered (socket closed
     * mid-exchange on the first attempts, or stale above): pace probes. */
    d->cooldown = NR_DEGRADE_PROBE_DOWN - 1;
    return NR_DEGRADE_LOG_NONE;
}

void nr_degrade_note_held(NrDegrade *d)
{
    if (d) d->held++;
}
