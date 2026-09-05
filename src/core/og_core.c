/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
#include "oledguard/og_core.h"
#include "oledguard/og_platform.h"
#include "oledguard/og_motion.h"
#include "oledguard/og_log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    og_monitor   mon;
    int          enabled;
    og_mon_state state;
    long         last_activity_ms;
    long         last_sample_ms;
    og_signature prev_sig;
    og_motion    motion;
    int          fullscreen;
    /* When a sample last came back good, and the verdict derived from it.
     * Not a per-call flag: an asynchronous backend legitimately returns
     * nothing on a tick where its capture is still in flight, and treating
     * that single tick as "capture is broken" is what let a permanently held
     * display assertion take over again. */
    long         last_good_ms;
    int          sampling_ok;
    int          force_blank;   /* manual "Blank now", overrides content holds */
    unsigned     hold;
} og_mon_rt;

struct og_core {
    og_config *cfg;
    og_mon_rt  m[OG_MAX_MONITORS];
    int        count;
    int        paused;

    int  last_cx, last_cy;
    int  have_cursor;
    long prev_idle_ms;
    long created_ms;
};

/* ------------------------------------------------------------------ */

static int rect_contains(const og_rect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w &&
           y >= r->y && y < r->y + r->h;
}

static void mon_unblank(og_core *c, og_mon_rt *rt)
{
    if (rt->state != OG_ST_BLANK) return;
    og_plat_overlay_hide(&rt->mon);
    rt->state = OG_ST_AWAKE;
    /* The first frame after a wake must not be diffed against whatever was
     * on screen before the blank; that would look like a scene cut and could
     * latch playback on a static desktop. */
    rt->prev_sig.valid = 0;
    rt->last_sample_ms = og_plat_now_ms();
    rt->last_good_ms = rt->last_good_ms ? og_plat_now_ms() : 0;
    og_motion_reset(&rt->motion);
    og_log(OG_LOG_INFO, "wake   %s", rt->mon.name);
    (void)c;
}

static void mon_blank(og_core *c, og_mon_rt *rt)
{
    if (rt->state == OG_ST_BLANK) return;
    if (!og_plat_overlay_show(&rt->mon, c->cfg->curtain_opacity, c->cfg->fade_ms)) {
        og_log(OG_LOG_WARN, "overlay failed on %s", rt->mon.name);
        return;
    }
    rt->state = OG_ST_BLANK;
    rt->prev_sig.valid = 0;
    og_log(OG_LOG_INFO, "blank  %s", rt->mon.name);
}

/* ------------------------------------------------------------------ */

og_core *og_core_create(og_config *cfg)
{
    og_core *c;
    if (!cfg) return NULL;
    c = (og_core *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->cfg = cfg;
    c->prev_idle_ms = -1;
    c->created_ms = og_plat_now_ms();
    og_core_refresh_monitors(c);
    return c;
}

void og_core_destroy(og_core *c)
{
    int i;
    if (!c) return;
    for (i = 0; i < c->count; ++i) {
        og_plat_overlay_hide(&c->m[i].mon);
        og_plat_overlay_destroy(&c->m[i].mon);
    }
    free(c);
}

void og_core_refresh_monitors(og_core *c)
{
    og_monitor fresh[OG_MAX_MONITORS];
    og_mon_rt  old[OG_MAX_MONITORS];
    int old_count, n, i, j;
    long now;

    if (!c) return;
    now = og_plat_now_ms();

    /* Tear every overlay down: native handles and geometry are both about to
     * be invalidated by re-enumeration. Blanked monitors are re-blanked below. */
    for (i = 0; i < c->count; ++i) {
        og_plat_overlay_hide(&c->m[i].mon);
        og_plat_overlay_destroy(&c->m[i].mon);
    }

    memcpy(old, c->m, sizeof(old));
    old_count = c->count;

    n = og_plat_enum_monitors(fresh, OG_MAX_MONITORS);
    if (n < 0) n = 0;
    memset(c->m, 0, sizeof(c->m));
    c->count = n;

    for (i = 0; i < n; ++i) {
        og_mon_rt *rt = &c->m[i];
        rt->mon = fresh[i];
        rt->mon.overlay = NULL;
        rt->last_activity_ms = now;
        rt->last_sample_ms = now;
        rt->state = OG_ST_AWAKE;
        og_motion_reset(&rt->motion);

        /* Carry over runtime state for a monitor we already knew. */
        for (j = 0; j < old_count; ++j) {
            if (strcmp(old[j].mon.id, rt->mon.id) != 0) continue;
            rt->last_activity_ms = old[j].last_activity_ms;
            rt->motion           = old[j].motion;
            rt->force_blank      = old[j].force_blank;
            if (old[j].state == OG_ST_BLANK) mon_blank(c, rt);
            break;
        }
        rt->enabled = og_config_monitor_enabled(c->cfg, rt->mon.id);
        og_log(OG_LOG_INFO, "display %s at %d,%d %dx%d (%s)",
               rt->mon.name, rt->mon.bounds.x, rt->mon.bounds.y,
               rt->mon.bounds.w, rt->mon.bounds.h,
               rt->enabled ? "guarded" : "ignored");
    }
}

/* ------------------------------------------------------------------ */

void og_core_tick(og_core *c)
{
    long now, idle;
    int cx = 0, cy = 0, have_pos, cursor_moved, cur_mon = -1, i;
    unsigned inh;
    int input_recent, capture_ok;

    if (!c) return;

    og_plat_pump();
    if (og_plat_displays_changed()) og_core_refresh_monitors(c);

    now = og_plat_now_ms();

    /* --- global input state ---------------------------------------- */
    have_pos = og_plat_cursor_pos(&cx, &cy);
    cursor_moved = have_pos && c->have_cursor &&
                   (cx != c->last_cx || cy != c->last_cy);
    if (have_pos) {
        c->last_cx = cx;
        c->last_cy = cy;
        c->have_cursor = 1;
        for (i = 0; i < c->count; ++i) {
            if (rect_contains(&c->m[i].mon.bounds, cx, cy)) { cur_mon = i; break; }
        }
    }

    idle = og_plat_idle_ms();
    /* An input event is "recent" if the idle counter went backwards since the
     * last tick, or if it is still inside a couple of tick periods. The first
     * test catches events regardless of tick jitter; the second covers the
     * very first tick, when there is no previous reading. */
    input_recent = (idle >= 0) &&
                   ((c->prev_idle_ms >= 0 && idle < c->prev_idle_ms) ||
                    idle <= (long)OG_TICK_MS * 3);
    c->prev_idle_ms = idle;

    inh = (c->cfg->inhibitor_policy == OG_INHIBIT_NEVER)
              ? OG_INHIBIT_NONE : og_plat_inhibit_flags();
    capture_ok = og_plat_capture_available();

    /* --- per monitor ------------------------------------------------ */
    for (i = 0; i < c->count; ++i) {
        og_mon_rt *rt = &c->m[i];
        unsigned hold = OG_HOLD_NONE;
        int activity = 0, user_here = 0;
        long timeout_ms = (long)c->cfg->timeout_sec * 1000L;

        rt->enabled = og_config_monitor_enabled(c->cfg, rt->mon.id);

        if (!rt->enabled || c->paused) {
            if (!rt->enabled) hold |= OG_HOLD_DISABLED;
            mon_unblank(c, rt);
            rt->last_activity_ms = now;
            rt->force_blank = 0;
            rt->hold = hold;
            continue;
        }

        /* Content sampling. Pointless while blanked: the overlay is what we
         * would be measuring. Detection there falls back to inhibitors. */
        if (rt->state == OG_ST_AWAKE && capture_ok &&
            now - rt->last_sample_ms >= (long)c->cfg->sample_interval_ms) {
            og_signature sig;
            memset(&sig, 0, sizeof sig);
            if (og_plat_capture_signature(&rt->mon, &sig) && sig.valid) {
                rt->last_good_ms = now;
                rt->fullscreen = og_plat_monitor_has_fullscreen(&rt->mon);
                if (rt->prev_sig.valid) {
                    double thr = rt->fullscreen ? c->cfg->motion_ratio_fullscreen
                                                : c->cfg->motion_ratio_windowed;
                    double ratio = og_sig_change_ratio(&rt->prev_sig, &sig,
                                                       c->cfg->cell_delta);
                    og_motion_update(&rt->motion, ratio, thr,
                                     c->cfg->motion_on_samples,
                                     c->cfg->motion_off_samples);
                    og_log(OG_LOG_DEBUG,
                           "sample %s ratio=%.3f thr=%.3f fullscreen=%d playing=%d",
                           rt->mon.name, ratio, thr, rt->fullscreen,
                           rt->motion.active);
                }
                rt->prev_sig = sig;
            }
            rt->last_sample_ms = now;
        }

        /* Judge sampling health over a window, and only while awake: a
         * blanked screen is not sampled at all, so its last good sample keeps
         * ageing and would otherwise look like a failure. Freezing the verdict
         * at blanking time keeps that from waking the screen straight back up. */
        if (rt->state == OG_ST_AWAKE) {
            long grace = (long)c->cfg->sample_interval_ms * 4;
            if (grace < 5000) grace = 5000;
            rt->sampling_ok = capture_ok && rt->last_good_ms != 0 &&
                              (now - rt->last_good_ms) <= grace;
        }
        if (!rt->sampling_ok) hold |= OG_HOLD_NO_CAPTURE;

        /* --- what counts as the user being present --- */
        if (cursor_moved && cur_mon == i) user_here = 1;
        switch (c->cfg->wake_mode) {
        case OG_WAKE_POINTER_ONLY:
            break;
        case OG_WAKE_POINTER_OR_INPUT:
            if (input_recent && cur_mon == i) user_here = 1;
            break;
        case OG_WAKE_ANY_INPUT:
            if (input_recent) user_here = 1;
            break;
        }
        if (user_here) {
            activity = 1;
            hold |= OG_HOLD_INPUT;
            rt->force_blank = 0;
        }

        /* --- content and OS assertions hold the screen awake --- */
        if (!rt->force_blank) {
            if (rt->motion.active) {
                activity = 1;
                hold |= OG_HOLD_MOTION;
            }
            if (inh & (OG_INHIBIT_DISPLAY | OG_INHIBIT_IDLE)) {
                /* An assertion is weak evidence. Caffeine-style utilities hold
                 * one permanently, so obeying it unconditionally means nothing
                 * ever blanks. When the pixels are visible they are the better
                 * witness; the assertion only decides when we are blind. */
                if (c->cfg->inhibitor_policy == OG_INHIBIT_ALWAYS ||
                    (c->cfg->inhibitor_policy == OG_INHIBIT_WHEN_BLIND &&
                     !rt->sampling_ok)) {
                    activity = 1;
                    hold |= OG_HOLD_SYS_INHIBIT;
                } else {
                    hold |= OG_HOLD_INHIBIT_SEEN;   /* noted, not obeyed */
                }
            }
        }

        if (activity) {
            rt->last_activity_ms = now;
            mon_unblank(c, rt);
        } else if (rt->state == OG_ST_AWAKE &&
                   (rt->force_blank || now - rt->last_activity_ms >= timeout_ms)) {
            mon_blank(c, rt);
        }

        rt->hold = hold;
    }
}

/* ------------------------------------------------------------------ */

void og_core_wake_all(og_core *c)
{
    int i;
    long now;
    if (!c) return;
    now = og_plat_now_ms();
    for (i = 0; i < c->count; ++i) {
        c->m[i].force_blank = 0;
        c->m[i].last_activity_ms = now;
        mon_unblank(c, &c->m[i]);
    }
}

void og_core_blank_now(og_core *c)
{
    int i;
    if (!c) return;
    c->paused = 0;
    for (i = 0; i < c->count; ++i) {
        if (!og_config_monitor_enabled(c->cfg, c->m[i].mon.id)) continue;
        c->m[i].force_blank = 1;
        mon_blank(c, &c->m[i]);
    }
}

void og_core_set_paused(og_core *c, int paused)
{
    if (!c) return;
    c->paused = paused ? 1 : 0;
    if (c->paused) og_core_wake_all(c);
}

int og_core_paused(const og_core *c) { return c ? c->paused : 0; }

int og_core_monitor_count(const og_core *c) { return c ? c->count : 0; }

int og_core_status_at(const og_core *c, int index, og_core_status *out)
{
    const og_mon_rt *rt;
    if (!c || !out || index < 0 || index >= c->count) return 0;
    rt = &c->m[index];
    out->mon           = &rt->mon;
    out->enabled       = rt->enabled;
    out->state         = rt->state;
    out->hold          = rt->hold;
    out->idle_ms       = og_plat_now_ms() - rt->last_activity_ms;
    out->motion_ratio  = rt->motion.last_ratio;
    out->motion_active = rt->motion.active;
    return 1;
}

void og_core_set_enabled(og_core *c, int index, int enabled)
{
    if (!c || index < 0 || index >= c->count) return;
    og_config_set_monitor_enabled(c->cfg, c->m[index].mon.id, enabled);
    c->m[index].enabled = enabled ? 1 : 0;
    if (!enabled) mon_unblank(c, &c->m[index]);
    else c->m[index].last_activity_ms = og_plat_now_ms();
}
