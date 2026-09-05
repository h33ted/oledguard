/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
#include "oledguard/og_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <shlobj.h>
#  include <direct.h>
#  define OG_PATHSEP '\\'
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define OG_PATHSEP '/'
#endif

void og_config_defaults(og_config *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->timeout_sec            = 120;
    c->sample_interval_ms     = 1500;
    c->cell_delta             = 6;
    c->motion_ratio_windowed  = 0.06;
    c->motion_ratio_fullscreen= 0.015;
    c->motion_on_samples      = 2;
    c->motion_off_samples     = 5;
    c->wake_mode              = OG_WAKE_POINTER_OR_INPUT;
    c->inhibitor_policy       = OG_INHIBIT_WHEN_BLIND;
    c->curtain_opacity        = 100;
    c->fade_ms                = 400;
    c->start_minimised        = 1;
    c->window_material        = 0;
    c->window_tint_pct        = 45;
    c->reduce_transparency    = 0;
    c->open_at_login          = 1;
    c->menu_bar_icon          = 1;
    c->dock_icon              = 0;
    c->default_enabled        = 0;
    c->mon_count              = 0;
}

/* ------------------------------------------------------------------ */

#if !defined(_WIN32)
#include <errno.h>
static int og_mkdir_posix(const char *dir)
{
    if (mkdir(dir, 0700) == 0) return 1;
    return errno == EEXIST;
}
#endif

int og_config_path(char *buf, size_t len)
{
    if (!buf || len < 8) return 0;

#if defined(_WIN32)
    {
        char base[MAX_PATH];
        if (!SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, base)))
            return 0;
        snprintf(buf, len, "%s\\OLEDGuard", base);
        _mkdir(buf);
        snprintf(buf, len, "%s\\OLEDGuard\\oledguard.ini", base);
        return 1;
    }
#elif defined(__APPLE__)
    {
        const char *home = getenv("HOME");
        char dir[1024];
        if (!home) return 0;
        snprintf(dir, sizeof dir, "%s/Library/Application Support/OLEDGuard", home);
        og_mkdir_posix(dir);
        snprintf(buf, len, "%s/oledguard.ini", dir);
        return 1;
    }
#else
    {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        char dir[1024];
        if (xdg && *xdg) {
            snprintf(dir, sizeof dir, "%s/oledguard", xdg);
        } else if (home) {
            char parent[1024];
            snprintf(parent, sizeof parent, "%s/.config", home);
            og_mkdir_posix(parent);
            snprintf(dir, sizeof dir, "%s/.config/oledguard", home);
        } else {
            return 0;
        }
        og_mkdir_posix(dir);
        snprintf(buf, len, "%s/oledguard.ini", dir);
        return 1;
    }
#endif
}

/* ------------------------------------------------------------------ */

static char *og_trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    if (!*s) return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
    return s;
}

int og_config_monitor_enabled(const og_config *c, const char *id)
{
    int i;
    if (!c || !id) return 0;
    for (i = 0; i < c->mon_count; ++i)
        if (strcmp(c->mon[i].id, id) == 0) return c->mon[i].enabled;
    return c->default_enabled;
}

void og_config_set_monitor_enabled(og_config *c, const char *id, int enabled)
{
    int i;
    if (!c || !id || !*id) return;
    for (i = 0; i < c->mon_count; ++i) {
        if (strcmp(c->mon[i].id, id) == 0) {
            c->mon[i].enabled = enabled ? 1 : 0;
            return;
        }
    }
    if (c->mon_count >= OG_MAX_MON_PREFS) return;
    {
        size_t n = strlen(id);
        if (n > OG_ID_LEN - 1) n = OG_ID_LEN - 1;
        memcpy(c->mon[c->mon_count].id, id, n);
        c->mon[c->mon_count].id[n] = '\0';
    }
    c->mon[c->mon_count].enabled = enabled ? 1 : 0;
    ++c->mon_count;
}

int og_config_load(og_config *c, const char *path)
{
    FILE *f;
    char line[512];

    if (!c) return 0;
    og_config_defaults(c);
    if (!path) return 0;

    f = fopen(path, "r");
    if (!f) return 0;

    while (fgets(line, sizeof line, f)) {
        char *k, *v, *eq;
        char *p = og_trim(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        k = og_trim(p);
        v = og_trim(eq + 1);

        if      (!strcmp(k, "timeout_sec"))             c->timeout_sec = atoi(v);
        else if (!strcmp(k, "sample_interval_ms"))      c->sample_interval_ms = atoi(v);
        else if (!strcmp(k, "cell_delta"))              c->cell_delta = atoi(v);
        else if (!strcmp(k, "motion_ratio_windowed"))   c->motion_ratio_windowed = atof(v);
        else if (!strcmp(k, "motion_ratio_fullscreen")) c->motion_ratio_fullscreen = atof(v);
        else if (!strcmp(k, "motion_on_samples"))       c->motion_on_samples = atoi(v);
        else if (!strcmp(k, "motion_off_samples"))      c->motion_off_samples = atoi(v);
        else if (!strcmp(k, "wake_mode"))               c->wake_mode = (og_wake_mode)atoi(v);
        else if (!strcmp(k, "inhibitor_policy"))
            c->inhibitor_policy = (og_inhibitor_policy)atoi(v);
        /* Migrated from the old boolean: "respect" meant an absolute veto,
         * which is now the ALWAYS setting, but the honest translation of
         * "on" is the new default rather than the old behaviour. */
        else if (!strcmp(k, "respect_inhibitors"))
            c->inhibitor_policy = atoi(v) ? OG_INHIBIT_WHEN_BLIND
                                          : OG_INHIBIT_NEVER;
        else if (!strcmp(k, "curtain_opacity"))         c->curtain_opacity = atoi(v);
        /* dim_level was a 0-255 grey level in an earlier version. It is read
         * and discarded rather than migrated: a grey curtain and a partly
         * transparent one are different things, so there is no honest
         * conversion. The default is restored instead. */
        else if (!strcmp(k, "dim_level"))               (void)0;
        else if (!strcmp(k, "fade_ms"))                 c->fade_ms = atoi(v);
        else if (!strcmp(k, "start_minimised"))         c->start_minimised = atoi(v);
        else if (!strcmp(k, "window_material"))         c->window_material = atoi(v);
        else if (!strcmp(k, "window_tint_pct"))         c->window_tint_pct = atoi(v);
        else if (!strcmp(k, "reduce_transparency"))     c->reduce_transparency = atoi(v);
        else if (!strcmp(k, "open_at_login"))           c->open_at_login = atoi(v);
        else if (!strcmp(k, "menu_bar_icon"))           c->menu_bar_icon = atoi(v);
        else if (!strcmp(k, "dock_icon"))               c->dock_icon = atoi(v);
        else if (!strcmp(k, "default_enabled"))         c->default_enabled = atoi(v);
        else if (!strncmp(k, "monitor.", 8)) {
            /* monitor.<id>.enabled = 1 */
            char *tail = strrchr(k, '.');
            if (tail && !strcmp(tail, ".enabled")) {
                char id[OG_ID_LEN];
                size_t n = (size_t)(tail - (k + 8));
                if (n >= OG_ID_LEN) n = OG_ID_LEN - 1;
                memcpy(id, k + 8, n);
                id[n] = '\0';
                og_config_set_monitor_enabled(c, id, atoi(v));
            }
        }
    }
    fclose(f);

    /* Clamp anything a hand-edit could have made nonsensical. */
    if (c->timeout_sec < 5)              c->timeout_sec = 5;
    if (c->timeout_sec > 86400)          c->timeout_sec = 86400;
    if (c->sample_interval_ms < 200)     c->sample_interval_ms = 200;
    if (c->sample_interval_ms > 30000)   c->sample_interval_ms = 30000;
    if (c->cell_delta < 0)               c->cell_delta = 0;
    if (c->cell_delta > 128)             c->cell_delta = 128;
    if (c->motion_ratio_windowed <= 0.0) c->motion_ratio_windowed = 0.06;
    if (c->motion_ratio_fullscreen<= 0.0)c->motion_ratio_fullscreen = 0.015;
    if (c->motion_on_samples < 1)        c->motion_on_samples = 1;
    if (c->motion_off_samples < 1)       c->motion_off_samples = 1;
    if (c->inhibitor_policy < OG_INHIBIT_NEVER ||
        c->inhibitor_policy > OG_INHIBIT_ALWAYS)
        c->inhibitor_policy = OG_INHIBIT_WHEN_BLIND;
    if (c->window_material < 0 || c->window_material > 5) c->window_material = 0;
    if (c->window_tint_pct < 0)          c->window_tint_pct = 0;
    if (c->window_tint_pct > 100)        c->window_tint_pct = 100;
    if (c->curtain_opacity < 10)         c->curtain_opacity = 10;
    if (c->curtain_opacity > 100)        c->curtain_opacity = 100;
    if (c->fade_ms < 0)                  c->fade_ms = 0;
    if (c->fade_ms > 10000)              c->fade_ms = 10000;
    if (c->wake_mode < OG_WAKE_POINTER_ONLY || c->wake_mode > OG_WAKE_ANY_INPUT)
        c->wake_mode = OG_WAKE_POINTER_OR_INPUT;
    /* A background app with no menu bar item and no Dock icon cannot be
     * reached at all, so refuse to load that combination. */
    if (!c->menu_bar_icon && !c->dock_icon) c->dock_icon = 1;

    return 1;
}

int og_config_save(const og_config *c, const char *path)
{
    FILE *f;
    int i;

    if (!c || !path) return 0;
    f = fopen(path, "w");
    if (!f) return 0;

    fprintf(f, "# OLEDGuard configuration\n");
    fprintf(f, "# Regenerated by the app; hand edits are preserved in value only.\n\n");
    fprintf(f, "[general]\n");
    fprintf(f, "timeout_sec = %d\n", c->timeout_sec);
    fprintf(f, "start_minimised = %d\n", c->start_minimised);
    fprintf(f, "# macOS: where the running app is visible. Turning both off is\n");
    fprintf(f, "# rejected on load, since that leaves no way to reach it.\n");
    fprintf(f, "# macOS settings window: how much of the desktop shows through.\n");
    fprintf(f, "# window_material 0=HUD (most see-through) 1=popover 2=sidebar\n");
    fprintf(f, "#                 3=under-window 4=window background 5=full-screen\n");
    fprintf(f, "# window_tint_pct 0 = untinted blur, 100 = the full purple-to-black ramp\n");
    fprintf(f, "window_material = %d\n", c->window_material);
    fprintf(f, "window_tint_pct = %d\n", c->window_tint_pct);
    fprintf(f, "# 1 shows the tint at full strength for legibility. macOS's own\n");
    fprintf(f, "# Reduce Transparency setting is honoured regardless.\n");
    fprintf(f, "reduce_transparency = %d\n", c->reduce_transparency);
    fprintf(f, "open_at_login = %d\n", c->open_at_login);
    fprintf(f, "menu_bar_icon = %d\n", c->menu_bar_icon);
    fprintf(f, "dock_icon = %d\n", c->dock_icon);
    fprintf(f, "# 100 = fully black (OLED pixels off). Lower lets the screen\n");
    fprintf(f, "# show faintly through, at the cost of some protection.\n");
    fprintf(f, "curtain_opacity = %d\n", c->curtain_opacity);
    fprintf(f, "fade_ms = %d\n", c->fade_ms);
    fprintf(f, "# 0 = pointer movement over that screen only\n");
    fprintf(f, "# 1 = pointer movement, or any input while the pointer rests there\n");
    fprintf(f, "# 2 = any input wakes every screen\n");
    fprintf(f, "wake_mode = %d\n", (int)c->wake_mode);
    fprintf(f, "default_enabled = %d\n\n", c->default_enabled);

    fprintf(f, "[detection]\n");
    fprintf(f, "# 0 = ignore apps asking to keep the display on (pixels decide)\n");
    fprintf(f, "# 1 = honour them only when screen sampling is unavailable\n");
    fprintf(f, "# 2 = always honour them\n");
    fprintf(f, "inhibitor_policy = %d\n", (int)c->inhibitor_policy);
    fprintf(f, "sample_interval_ms = %d\n", c->sample_interval_ms);
    fprintf(f, "cell_delta = %d\n", c->cell_delta);
    fprintf(f, "motion_ratio_windowed = %.4f\n", c->motion_ratio_windowed);
    fprintf(f, "motion_ratio_fullscreen = %.4f\n", c->motion_ratio_fullscreen);
    fprintf(f, "motion_on_samples = %d\n", c->motion_on_samples);
    fprintf(f, "motion_off_samples = %d\n\n", c->motion_off_samples);

    fprintf(f, "[monitors]\n");
    for (i = 0; i < c->mon_count; ++i)
        fprintf(f, "monitor.%s.enabled = %d\n", c->mon[i].id, c->mon[i].enabled);

    fclose(f);
    return 1;
}
