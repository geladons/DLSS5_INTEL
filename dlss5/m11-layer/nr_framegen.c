/* ============================================================================
 * nr_framegen.c - frame-generation cadence bookkeeping (see nr_framegen.h).
 * ==========================================================================*/
#include "nr_framegen.h"

void nr_fg_init(NrFrameGen *fg, long configured_every)
{
    if (!fg) return;
    fg->configured_every = configured_every;
    fg->presents = fg->processed = fg->generated = fg->native = 0;
    fg->unavailable_logged = 0;
}

long nr_fg_effective_every(const NrFrameGen *fg, int enabled, int available)
{
    long configured = fg ? fg->configured_every : 0;
    if (configured <= 1)
        return (enabled && available) ? 2 : 1;
    return configured;
}

int nr_fg_should_process(NrFrameGen *fg, unsigned long frame_counter,
                         int enabled, int available)
{
    long every;
    if (fg) fg->presents++;
    every = nr_fg_effective_every(fg, enabled, available);
    return every <= 1 || frame_counter % (unsigned long)every == 0;
}

void nr_fg_note(NrFrameGen *fg, int kind)
{
    if (!fg) return;
    if (kind == NR_FG_PROCESSED) fg->processed++;
    else if (kind == NR_FG_GENERATED) fg->generated++;
    else fg->native++;
}

int nr_fg_stats_due(const NrFrameGen *fg)
{
    return fg && fg->presents > 0 && fg->presents % 300 == 0;
}
