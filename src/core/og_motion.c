/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
#include "oledguard/og_motion.h"

#include <string.h>

void og_motion_reset(og_motion *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
}

double og_sig_change_ratio(const og_signature *a, const og_signature *b,
                           int cell_delta)
{
    int i, changed = 0;

    if (!a || !b || !a->valid || !b->valid) return 0.0;
    if (cell_delta < 0) cell_delta = 0;

    for (i = 0; i < OG_SIG_CELLS; ++i) {
        int d = (int)a->cell[i] - (int)b->cell[i];
        if (d < 0) d = -d;
        if (d > cell_delta) ++changed;
    }
    return (double)changed / (double)OG_SIG_CELLS;
}

int og_motion_update(og_motion *m, double ratio, double threshold,
                     int on_samples, int off_samples)
{
    if (!m) return 0;
    if (on_samples  < 1) on_samples  = 1;
    if (off_samples < 1) off_samples = 1;

    m->last_ratio = ratio;

    if (ratio >= threshold) {
        m->still_run = 0;
        if (m->motion_run < on_samples) ++m->motion_run;
        if (m->motion_run >= on_samples) m->active = 1;
    } else {
        m->motion_run = 0;
        if (m->still_run < off_samples) ++m->still_run;
        if (m->still_run >= off_samples) m->active = 0;
    }
    return m->active;
}
