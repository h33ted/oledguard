/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_types.h - shared value types for OLEDGuard.
 *
 * This header is pure C99 and must not include any platform headers.
 */
#ifndef OG_TYPES_H
#define OG_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OG_MAX_MONITORS 16
#define OG_ID_LEN       128
#define OG_NAME_LEN     128

/* Screen signature grid. Small on purpose: the goal is to answer
 * "is a large part of this screen changing?", not to reconstruct the image.
 * 32x18 keeps 16:9 proportions and fits in a cache line-ish 576 bytes. */
#define OG_SIG_COLS  32
#define OG_SIG_ROWS  18
#define OG_SIG_CELLS (OG_SIG_COLS * OG_SIG_ROWS)

/* Core scheduler period. Also the upper bound on wake latency. */
#define OG_TICK_MS 40

typedef struct {
    int x, y, w, h;
} og_rect;

typedef struct {
    /* Stable across reboots and cable reconnects. Derived from EDID where
     * available, otherwise from the connector/adapter name. Used as the
     * config key so per-monitor choices survive a replug. */
    char id[OG_ID_LEN];
    /* Human readable, e.g. "DP-2 - LG Ultrafine (2560x1440)". */
    char name[OG_NAME_LEN];
    /* Position and size in virtual-desktop coordinates. */
    og_rect bounds;
    int primary;
    /* Backend-owned handle (HMONITOR, CGDirectDisplayID, RROutput, ...). */
    void *native;
    /* Backend-owned overlay window handle, NULL when not created. */
    void *overlay;
} og_monitor;

/* A downsampled luma grid of one monitor. */
typedef struct {
    unsigned char cell[OG_SIG_CELLS];
    int valid;
} og_signature;

typedef enum {
    OG_ST_AWAKE = 0,
    OG_ST_BLANK = 1
} og_mon_state;

/* Reasons the core refused to blank a monitor. Surfaced in the UI so the
 * behaviour is explainable rather than mysterious. */
typedef enum {
    OG_HOLD_NONE          = 0,
    OG_HOLD_DISABLED      = 1 << 0, /* monitor not selected by the user */
    OG_HOLD_INPUT         = 1 << 1, /* recent keyboard/mouse activity */
    OG_HOLD_MOTION        = 1 << 2, /* screen content is changing */
    OG_HOLD_SYS_INHIBIT   = 1 << 3, /* an OS-level display assertion is held */
    OG_HOLD_NO_CAPTURE    = 1 << 4, /* capture unavailable on this platform */
    OG_HOLD_INHIBIT_SEEN  = 1 << 5  /* an assertion is held but being ignored */
} og_hold_flags;

/* Bit flags returned by og_plat_inhibit_flags(). */
typedef enum {
    OG_INHIBIT_NONE    = 0,
    OG_INHIBIT_DISPLAY = 1 << 0, /* something asked the display to stay on */
    OG_INHIBIT_IDLE    = 1 << 1  /* something asked the session to stay non-idle */
} og_inhibit_flags;

#ifdef __cplusplus
}
#endif
#endif /* OG_TYPES_H */
