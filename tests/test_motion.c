/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* Unit tests for the changed-cell classifier. */
#include "oledguard/og_motion.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); ++failures; \
    } \
} while (0)

static unsigned rng = 7u;
static unsigned nrand(void) { rng = rng * 1664525u + 1013904223u; return rng >> 16; }

static void fill_base(og_signature *s)
{
    int i;
    for (i = 0; i < OG_SIG_CELLS; ++i) s->cell[i] = (unsigned char)((i * 37) & 0xFF);
    s->valid = 1;
}

static void perturb(og_signature *s, int n_cells)
{
    int k;
    for (k = 0; k < n_cells; ++k) {
        int idx = (int)(nrand() % OG_SIG_CELLS);
        s->cell[idx] = (unsigned char)(s->cell[idx] ^ 0x80);
    }
}

static void test_ratio(void)
{
    og_signature a, b;
    double r;

    printf("ratio\n");
    fill_base(&a);
    b = a;
    r = og_sig_change_ratio(&a, &b, 6);
    CHECK(r == 0.0, "identical frames should report 0, got %f", r);

    b = a;
    perturb(&b, 3);
    r = og_sig_change_ratio(&a, &b, 6);
    CHECK(r > 0.0 && r < 0.01,
          "3 of %d cells should be well under 1%%, got %f", OG_SIG_CELLS, r);

    b = a;
    perturb(&b, 400);
    r = og_sig_change_ratio(&a, &b, 6);
    CHECK(r > 0.30, "a heavily changed frame should exceed 30%%, got %f", r);

    /* An invalid signature must never look like motion. */
    b = a;
    b.valid = 0;
    CHECK(og_sig_change_ratio(&a, &b, 6) == 0.0, "invalid signature must be 0");

    /* cell_delta must actually suppress small changes. */
    fill_base(&a);
    b = a;
    for (int i = 0; i < OG_SIG_CELLS; ++i)
        b.cell[i] = (unsigned char)(b.cell[i] > 3 ? b.cell[i] - 3 : 0);
    CHECK(og_sig_change_ratio(&a, &b, 6) == 0.0,
          "a uniform 3-level shift must stay below a cell_delta of 6");
    CHECK(og_sig_change_ratio(&a, &b, 1) > 0.9,
          "the same shift must register with a cell_delta of 1");
}

static void test_hysteresis(void)
{
    og_motion m;

    printf("hysteresis\n");
    og_motion_reset(&m);

    /* One moving sample is not enough to latch with on_samples = 2. */
    CHECK(og_motion_update(&m, 0.50, 0.06, 2, 5) == 0, "latched after one sample");
    CHECK(og_motion_update(&m, 0.50, 0.06, 2, 5) == 1, "did not latch after two");

    /* A brief still sample during playback (a letterboxed dark shot) must not
     * drop the latch. */
    CHECK(og_motion_update(&m, 0.00, 0.06, 2, 5) == 1, "dropped after 1 still sample");
    CHECK(og_motion_update(&m, 0.00, 0.06, 2, 5) == 1, "dropped after 2");
    CHECK(og_motion_update(&m, 0.00, 0.06, 2, 5) == 1, "dropped after 3");
    CHECK(og_motion_update(&m, 0.00, 0.06, 2, 5) == 1, "dropped after 4");
    CHECK(og_motion_update(&m, 0.00, 0.06, 2, 5) == 0, "did not release after 5");

    /* Discord-scale noise must never latch, however long it runs. */
    og_motion_reset(&m);
    for (int i = 0; i < 500; ++i)
        og_motion_update(&m, 0.005, 0.06, 2, 5);
    CHECK(m.active == 0, "static UI noise latched playback");

    /* A fullscreen window lowers the bar enough that the same faint motion
     * does latch: this is the paused-vs-playing distinction. */
    og_motion_reset(&m);
    og_motion_update(&m, 0.02, 0.015, 2, 5);
    CHECK(og_motion_update(&m, 0.02, 0.015, 2, 5) == 1,
          "faint motion under a fullscreen window should latch");

    /* Degenerate arguments must not misbehave. */
    og_motion_reset(&m);
    CHECK(og_motion_update(&m, 0.5, 0.06, 0, 0) == 1, "on_samples=0 should clamp to 1");
    CHECK(og_motion_update(NULL, 0.5, 0.06, 2, 5) == 0, "NULL must be tolerated");
}

int main(void)
{
    printf("== og_motion ==\n");
    test_ratio();
    test_hysteresis();
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall motion checks passed\n");
    return 0;
}
