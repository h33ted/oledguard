/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_motion.h - the part that decides "is this a film, or is it Discord?".
 *
 * Deliberately free of any platform or core dependency so it can be driven
 * by synthetic frame sequences in tests. See tests/test_motion.c.
 */
#ifndef OG_MOTION_H
#define OG_MOTION_H

#include "oledguard/og_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  motion_run;    /* consecutive samples classified as moving */
    int  still_run;     /* consecutive samples classified as still */
    int  active;        /* latched output: content is playing */
    double last_ratio;  /* most recent changed-cell fraction, for the UI */
} og_motion;

void og_motion_reset(og_motion *m);

/* Fraction of cells whose luma differs by more than `cell_delta`.
 * Returns 0.0 when either signature is invalid. */
double og_sig_change_ratio(const og_signature *a, const og_signature *b,
                           int cell_delta);

/* Feed one sample. `threshold` is the caller-chosen ratio bar (lower when a
 * fullscreen window is present). Returns the latched `active` value.
 *
 * Hysteresis rather than a plain threshold is what separates the two cases
 * the user cares about:
 *
 *   fullscreen video   ~0.30-0.80 of cells change every sample  -> latches on
 *   a game             ~0.20-0.90                               -> latches on
 *   scrolling a page   ~0.40 for a moment, then still           -> brief, decays
 *   Discord idle       ~0.00-0.01 (caret, an animated emoji)    -> never latches
 *   a static web page  ~0.00                                    -> never latches
 *   paused video       ~0.00                                    -> releases
 */
int og_motion_update(og_motion *m, double ratio, double threshold,
                     int on_samples, int off_samples);

#ifdef __cplusplus
}
#endif
#endif /* OG_MOTION_H */
