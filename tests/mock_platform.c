/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
#include "mock_platform.h"
#include "oledguard/og_platform.h"

#include <stdio.h>
#include <string.h>

#define MOCK_MONS 2

static struct {
    long now;
    long last_input;
    int  cx, cy;
    unsigned inhibit;
    int  capture_avail;
    int  capture_intermittent;
    int  capture_parity[MOCK_MONS];   /* per monitor: they sample in the same tick */
    mock_content content[MOCK_MONS];
    int  fullscreen[MOCK_MONS];
    int  overlay_on[MOCK_MONS];
    int  overlay_opacity[MOCK_MONS];
    unsigned rng;
    int  scroll_left[MOCK_MONS];
} M;

static og_rect bounds_for(int i)
{
    og_rect r;
    if (i == 0) { r.x = 0;    r.y = 0; r.w = 1920; r.h = 1080; }
    else        { r.x = 1920; r.y = 0; r.w = 2560; r.h = 1440; }
    return r;
}

void mock_reset(void)
{
    memset(&M, 0, sizeof M);
    M.now = 1000;
    M.last_input = 1000;
    M.cx = -5000;           /* pointer parked off every monitor */
    M.cy = -5000;
    M.capture_avail = 1;
    M.rng = 12345u;
}

void mock_advance(long ms) { M.now += ms; }
void mock_input(void)      { M.last_input = M.now; }

void mock_set_cursor(int x, int y)
{
    if (x != M.cx || y != M.cy) M.last_input = M.now;
    M.cx = x; M.cy = y;
}

void mock_set_inhibit(unsigned f) { M.inhibit = f; }

void mock_set_content(int mon, mock_content c)
{
    if (mon < 0 || mon >= MOCK_MONS) return;
    M.content[mon] = c;
    if (c == MOCK_SCROLL) M.scroll_left[mon] = 2;
}

void mock_set_fullscreen(int mon, int on)
{
    if (mon < 0 || mon >= MOCK_MONS) return;
    M.fullscreen[mon] = on ? 1 : 0;
}

void mock_set_capture_available(int on) { M.capture_avail = on ? 1 : 0; }
void mock_set_capture_intermittent(int on) { M.capture_intermittent = on ? 1 : 0; }
int  mock_overlay_visible(int mon)
{
    return (mon >= 0 && mon < MOCK_MONS) ? M.overlay_on[mon] : 0;
}
int  mock_overlay_opacity(int mon)
{
    return (mon >= 0 && mon < MOCK_MONS) ? M.overlay_opacity[mon] : -1;
}

/* ------------------------------------------------------------------ */

int  og_plat_init(void) { return 1; }
void og_plat_shutdown(void) {}
const char *og_plat_name(void) { return "mock"; }
void og_plat_pump(void) {}
long og_plat_now_ms(void) { return M.now; }
int  og_plat_displays_changed(void) { return 0; }

int og_plat_enum_monitors(og_monitor *out, int max)
{
    int i;
    for (i = 0; i < MOCK_MONS && i < max; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].bounds = bounds_for(i);
        out[i].primary = (i == 0);
        out[i].native = (void *)(size_t)(i + 1);
        snprintf(out[i].id, OG_ID_LEN, "mock-%d", i);
        snprintf(out[i].name, OG_NAME_LEN, "Mock display %d", i);
    }
    return i;
}

int og_plat_cursor_pos(int *x, int *y)
{
    if (x) *x = M.cx;
    if (y) *y = M.cy;
    return 1;
}

long og_plat_idle_ms(void) { return M.now - M.last_input; }

unsigned og_plat_inhibit_flags(void) { return M.inhibit; }

int og_plat_inhibitor_name(char *buf, size_t len)
{
    if (!buf || len == 0 || !M.inhibit) return 0;
    snprintf(buf, len, "MockKeepAwake");
    return 1;
}

int og_plat_monitor_has_fullscreen(const og_monitor *m)
{
    int i = (int)(size_t)m->native - 1;
    return (i >= 0 && i < MOCK_MONS) ? M.fullscreen[i] : 0;
}

int og_plat_capture_available(void) { return M.capture_avail; }

static unsigned mock_rand(void)
{
    M.rng = M.rng * 1664525u + 1013904223u;
    return M.rng >> 16;
}

int og_plat_capture_signature(const og_monitor *m, og_signature *sig)
{
    int i = (int)(size_t)m->native - 1;
    int k;
    if (!M.capture_avail || i < 0 || i >= MOCK_MONS) return 0;
    if (M.capture_intermittent && ((++M.capture_parity[i]) & 1)) return 0;

    /* A stable "desktop" base image, identical every call. */
    for (k = 0; k < OG_SIG_CELLS; ++k)
        sig->cell[k] = (unsigned char)((k * 37) & 0xFF);

    switch (M.content[i]) {
    case MOCK_STATIC:
        break;
    case MOCK_SUBTLE:
        /* Three cells out of 576 wobble: a text caret and a clock. */
        sig->cell[10]  ^= (unsigned char)(mock_rand() & 0x7F);
        sig->cell[200] ^= (unsigned char)(mock_rand() & 0x7F);
        sig->cell[400] ^= (unsigned char)(mock_rand() & 0x7F);
        break;
    case MOCK_SCROLL:
        if (M.scroll_left[i] > 0) {
            --M.scroll_left[i];
            for (k = 0; k < OG_SIG_CELLS; ++k)
                sig->cell[k] = (unsigned char)mock_rand();
        }
        break;
    case MOCK_VIDEO:
        /* Roughly 60% of the frame changes between samples. */
        for (k = 0; k < OG_SIG_CELLS; ++k)
            if ((mock_rand() % 10u) < 6u)
                sig->cell[k] = (unsigned char)mock_rand();
        break;
    }
    sig->valid = 1;
    return 1;
}

int og_plat_overlay_show(og_monitor *m, int opacity_pct, int fade_ms)
{
    int i = (int)(size_t)m->native - 1;
    if (i < 0 || i >= MOCK_MONS) return 0;
    M.overlay_on[i] = 1;
    M.overlay_opacity[i] = opacity_pct;
    m->overlay = (void *)(size_t)(i + 1);
    return 1;
}

void og_plat_overlay_hide(og_monitor *m)
{
    int i = (int)(size_t)m->native - 1;
    if (i >= 0 && i < MOCK_MONS) M.overlay_on[i] = 0;
}

void og_plat_overlay_destroy(og_monitor *m)
{
    og_plat_overlay_hide(m);
    if (m) m->overlay = NULL;
}
