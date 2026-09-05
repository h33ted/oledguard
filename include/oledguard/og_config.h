/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_config.h - user settings, persisted as a small INI file. */
#ifndef OG_CONFIG_H
#define OG_CONFIG_H

#include "oledguard/og_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What counts as "the user is here" for the purpose of waking a screen. */
typedef enum {
    /* Only pointer movement over that specific monitor wakes it.
     * Keyboard input wakes nothing. Strictest, best protection. */
    OG_WAKE_POINTER_ONLY = 0,
    /* Pointer movement over the monitor, plus any keyboard/mouse input
     * while the pointer happens to rest on it. Sensible default. */
    OG_WAKE_POINTER_OR_INPUT = 1,
    /* Any input anywhere wakes every blanked monitor. Least protection,
     * but never surprises anyone. */
    OG_WAKE_ANY_INPUT = 2
} og_wake_mode;

typedef enum {
    /* Never let an assertion stop a screen blanking. The pixels decide. */
    OG_INHIBIT_NEVER = 0,
    /* Honour it only when we cannot sample the screen ourselves - no Screen
     * Recording permission, or a platform without capture. Then an assertion
     * is the only evidence available, so it is worth having. Default. */
    OG_INHIBIT_WHEN_BLIND = 1,
    /* Always honour it. Correct only if nothing on the machine holds one
     * permanently. */
    OG_INHIBIT_ALWAYS = 2
} og_inhibitor_policy;

#define OG_MAX_MON_PREFS 64

typedef struct {
    char id[OG_ID_LEN];
    int  enabled;
} og_monitor_pref;

typedef struct {
    /* --- timing --- */
    int timeout_sec;          /* idle before blanking. default 120 */
    int sample_interval_ms;   /* how often to fingerprint the screen. default 1500 */

    /* --- motion classifier --- */
    /* A cell counts as "changed" when its luma moves by at least this much
     * (0-255). Low enough to catch dark film scenes, high enough to ignore
     * dither and video-noise on a static desktop. */
    int cell_delta;           /* default 6 */
    /* Fraction of the 576 cells that must change for the frame to count as
     * moving. Two thresholds: a fullscreen window is already strong evidence
     * of media, so it gets a much lower bar. */
    double motion_ratio_windowed;   /* default 0.06 */
    double motion_ratio_fullscreen; /* default 0.015 */
    /* Debounce. Consecutive moving samples needed to latch "playing",
     * and consecutive still samples needed to release it. Asymmetric so a
     * one-second pause in a film does not immediately arm the blanker. */
    int motion_on_samples;    /* default 2 */
    int motion_off_samples;   /* default 5 */

    /* --- policy --- */
    og_wake_mode wake_mode;   /* default OG_WAKE_POINTER_OR_INPUT */
    /* What to do when some application holds an OS "keep the display on"
     * assertion. This is a weaker signal than it looks: Amphetamine, Caffeine,
     * Endurance, remote-desktop and screen-sharing tools all hold one
     * permanently and for reasons that have nothing to do with what is on the
     * screen. Treating that as an absolute veto stops the app blanking
     * anything, ever. */
    og_inhibitor_policy inhibitor_policy;  /* default OG_INHIBIT_WHEN_BLIND */
    /* How opaque the curtain is, 10-100 percent. The curtain is always pure
     * black: on OLED a grey curtain lights every pixel, which defeats the
     * entire point, so brightness is not the knob - coverage is.
     * 100 = fully black, pixels off. Lower lets the screen show faintly
     * through, which costs some of the protection. */
    int curtain_opacity;      /* default 100 */
    int fade_ms;              /* overlay fade-in. 0 = instant. default 400 */
    int start_minimised;      /* default 1 */

    /* --- where the running app is visible (macOS) --- */
    /* At least one of these is always on; the loader repairs a config that
     * turns both off, because a background app with neither is unreachable. */
    /* Register the app as a login item so guarding resumes after a restart.
     * A burn-in guard that only runs when you remember to start it is not
     * doing its job. macOS only for now; SMAppService, macOS 13+. */
    int open_at_login;        /* default 1 */
    int menu_bar_icon;        /* status bar item. default 1 */
    int dock_icon;            /* Dock icon and app menu. default 0 */

    /* --- settings window appearance (macOS) --- */
    /* Which NSVisualEffectView material blurs the desktop behind the window.
     * They differ a lot in how much they let through, and which one looks
     * right depends on the wallpaper, so it is a setting rather than a
     * hardcoded guess:
     *   0 HUD          most see-through
     *   1 popover
     *   2 sidebar
     *   3 under-window background
     *   4 window background
     *   5 full-screen UI
     * Anything else falls back to 0. */
    int window_material;      /* default 0 */
    /* Strength of the purple-to-black tint painted over that blur, as a
     * percentage. 0 leaves the blur untinted; 100 is the designed ramp. */
    int window_tint_pct;      /* default 45 */
    /* Accessibility: drop the translucency and show the tint at full strength,
     * because text over a blurred desktop is harder to read for some people.
     * macOS's own "Reduce transparency" setting is honoured on top of this and
     * goes further, removing the blur entirely. */
    int reduce_transparency;  /* default 0 */

    /* --- per monitor --- */
    og_monitor_pref mon[OG_MAX_MON_PREFS];
    int mon_count;
    /* Applied to monitors not yet seen in the config file. Off by default:
     * the user opts screens in deliberately. */
    int default_enabled;      /* default 0 */
} og_config;

void og_config_defaults(og_config *c);

/* Returns the platform config file path in a caller-supplied buffer.
 * Linux:   $XDG_CONFIG_HOME/oledguard/oledguard.ini
 * Windows: %APPDATA%\OLEDGuard\oledguard.ini
 * macOS:   ~/Library/Application Support/OLEDGuard/oledguard.ini */
int og_config_path(char *buf, size_t len);

/* Both return 1 on success. A missing file is not an error for load: the
 * config is left at defaults and 0 is returned. */
int og_config_load(og_config *c, const char *path);
int og_config_save(const og_config *c, const char *path);

/* Per-monitor enable lookup/assignment by stable id. */
int  og_config_monitor_enabled(const og_config *c, const char *id);
void og_config_set_monitor_enabled(og_config *c, const char *id, int enabled);

#ifdef __cplusplus
}
#endif
#endif /* OG_CONFIG_H */
