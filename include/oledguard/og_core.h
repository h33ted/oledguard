/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_core.h - the portable engine. Owns the per-monitor state machines.
 *
 * The platform owns the run loop and calls og_core_tick() every OG_TICK_MS.
 */
#ifndef OG_CORE_H
#define OG_CORE_H

#include "oledguard/og_types.h"
#include "oledguard/og_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct og_core og_core;

/* Snapshot of one monitor for the settings UI. */
typedef struct {
    const og_monitor *mon;
    int          enabled;
    og_mon_state state;
    unsigned     hold;          /* og_hold_flags, why it is not blanked */
    long         idle_ms;       /* how long this monitor has been idle */
    double       motion_ratio;  /* last measured changed-cell fraction */
    int          motion_active; /* latched playback detection */
} og_core_status;

/* `cfg` must outlive the core; the core reads it live so UI changes take
 * effect on the next tick without a restart. */
og_core *og_core_create(og_config *cfg);
void     og_core_destroy(og_core *c);

/* Re-enumerate displays. Safe to call at any time; overlays for monitors
 * that went away are destroyed. Called automatically by tick when the
 * backend reports a topology change. */
void og_core_refresh_monitors(og_core *c);

/* Advance all state machines. */
void og_core_tick(og_core *c);

/* Unblank everything now (used when the settings window opens, on exit,
 * and from the tray menu). */
void og_core_wake_all(og_core *c);

/* Blank every enabled monitor now, ignoring the timer but still honouring
 * nothing else. Bound to a "Blank now" menu item. */
void og_core_blank_now(og_core *c);

/* Global pause: the engine keeps running but never blanks. */
void og_core_set_paused(og_core *c, int paused);
int  og_core_paused(const og_core *c);

int  og_core_monitor_count(const og_core *c);
int  og_core_status_at(const og_core *c, int index, og_core_status *out);

/* Toggle a monitor and persist the choice into the config struct. */
void og_core_set_enabled(og_core *c, int index, int enabled);

#ifdef __cplusplus
}
#endif
#endif /* OG_CORE_H */
