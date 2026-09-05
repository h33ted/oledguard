/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_plat_win32.c - Windows backend: GDI capture, layered topmost curtains.
 *
 * Capture uses StretchBlt with HALFTONE straight into a 32x18 bitmap, so the
 * whole sample is one GDI call and 576 pixels come back. That covers windowed
 * and borderless-fullscreen content, which is what modern games and every
 * browser use. Exclusive-fullscreen Direct3D is the one case GDI cannot see;
 * SHQueryUserNotificationState reports it explicitly and is treated as a hard
 * hold, so those are covered without a Desktop Duplication pipeline.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <shellapi.h>

#include "oledguard/og_platform.h"
#include "oledguard/og_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OG_OVERLAY_CLASS L"OLEDGuardCurtain"

typedef struct {
    HWND hwnd;
    int  visible;
    int  opacity_pct;      /* 10-100; the curtain itself is always black */
    long fade_start_ms;
    int  fade_ms;
} og_overlay_win;

typedef struct {
    HMONITOR hmon;
    RECT     rc;
    WCHAR    device[32];
} og_win_monitor;

static struct {
    HINSTANCE hinst;
    HWND      msg_win;          /* hidden, receives WM_DISPLAYCHANGE */
    int       displays_dirty;

    og_win_monitor mons[OG_MAX_MONITORS];
    int            mon_count;

    og_overlay_win overlays[OG_MAX_MONITORS];

    long fs_cache_ms;
    RECT fs_rect[24];
    int  fs_count;
} G;

/* ------------------------------------------------------------------ */

long og_plat_now_ms(void) { return (long)GetTickCount64(); }

const char *og_plat_name(void) { return "Windows (GDI)"; }

/* ------------------------------------------------------------------ */
/* Window classes                                                       */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK curtain_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        /* Always black. Partial coverage comes from the layered window's
         * alpha, never from a lighter fill - grey pixels are lit pixels. */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HBRUSH br = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, br);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   /* WM_PAINT does it all; this stops the flash */
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DISPLAYCHANGE || msg == WM_DPICHANGED)
        G.displays_dirty = 1;
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */

static void enable_dpi_awareness(void)
{
    /* Without per-monitor v2 awareness, monitor rectangles come back in
     * virtualised coordinates and the curtain lands in the wrong place on a
     * mixed-DPI desktop. Resolved dynamically so the binary still runs on
     * Windows 8.1. */
    typedef BOOL (WINAPI *pfnSetCtx)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        pfnSetCtx f = (pfnSetCtx)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (f && f((HANDLE)-4)) return;   /* PER_MONITOR_AWARE_V2 */
    }
    SetProcessDPIAware();
}

int og_plat_init(void)
{
    WNDCLASSEXW wc;

    memset(&G, 0, sizeof G);
    G.hinst = GetModuleHandleW(NULL);
    enable_dpi_awareness();

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = curtain_proc;
    wc.hInstance = G.hinst;
    wc.hCursor = NULL;
    wc.hbrBackground = NULL;
    wc.lpszClassName = OG_OVERLAY_CLASS;
    if (!RegisterClassExW(&wc)) {
        og_log(OG_LOG_ERROR, "RegisterClassEx failed (%lu)", GetLastError());
        return 0;
    }

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = msg_proc;
    wc.hInstance = G.hinst;
    wc.lpszClassName = L"OLEDGuardNotify";
    RegisterClassExW(&wc);
    G.msg_win = CreateWindowExW(0, L"OLEDGuardNotify", L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, NULL, G.hinst, NULL);
    return 1;
}

void og_plat_shutdown(void)
{
    int i;
    for (i = 0; i < OG_MAX_MONITORS; ++i) {
        if (G.overlays[i].hwnd) {
            DestroyWindow(G.overlays[i].hwnd);
            G.overlays[i].hwnd = NULL;
        }
    }
    if (G.msg_win) { DestroyWindow(G.msg_win); G.msg_win = NULL; }
    UnregisterClassW(OG_OVERLAY_CLASS, G.hinst);
    UnregisterClassW(L"OLEDGuardNotify", G.hinst);
}

/* ------------------------------------------------------------------ */
/* Monitor enumeration                                                  */
/* ------------------------------------------------------------------ */

/* QueryDisplayConfig gives both the friendly panel name ("LG ULTRAGEAR")
 * and the device path, which embeds the EDID vendor and product code and is
 * therefore stable across reboots and a move to another port. */
static int friendly_for_device(const WCHAR *gdi_device,
                               WCHAR *name_out, size_t name_len,
                               WCHAR *path_out, size_t path_len)
{
    UINT32 npath = 0, nmode = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    int found = 0;

    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npath, &nmode) != ERROR_SUCCESS)
        return 0;
    paths = (DISPLAYCONFIG_PATH_INFO *)calloc(npath, sizeof *paths);
    modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nmode, sizeof *modes);
    if (!paths || !modes) goto done;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npath, paths,
                           &nmode, modes, NULL) != ERROR_SUCCESS)
        goto done;

    for (i = 0; i < npath; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;

        memset(&src, 0, sizeof src);
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, gdi_device) != 0) continue;

        memset(&tgt, 0, sizeof tgt);
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof tgt;
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;

        if (tgt.flags.friendlyNameFromEdid && tgt.monitorFriendlyDeviceName[0])
            wcsncpy(name_out, tgt.monitorFriendlyDeviceName, name_len - 1);
        else
            wcsncpy(name_out, gdi_device, name_len - 1);
        wcsncpy(path_out, tgt.monitorDevicePath, path_len - 1);
        found = 1;
        break;
    }
done:
    free(paths);
    free(modes);
    return found;
}

static BOOL CALLBACK enum_mon_cb(HMONITOR h, HDC dc, LPRECT rc, LPARAM lp)
{
    MONITORINFOEXW mi;
    (void)dc; (void)rc; (void)lp;

    if (G.mon_count >= OG_MAX_MONITORS) return FALSE;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(h, (MONITORINFO *)&mi)) return TRUE;

    G.mons[G.mon_count].hmon = h;
    G.mons[G.mon_count].rc = mi.rcMonitor;
    wcsncpy(G.mons[G.mon_count].device, mi.szDevice, 31);
    ++G.mon_count;
    return TRUE;
}

int og_plat_enum_monitors(og_monitor *out, int max)
{
    int i;
    if (!out || max <= 0) return 0;

    G.mon_count = 0;
    EnumDisplayMonitors(NULL, NULL, enum_mon_cb, 0);

    for (i = 0; i < G.mon_count && i < max; ++i) {
        og_monitor *m = &out[i];
        WCHAR name[128] = L"", path[256] = L"";
        MONITORINFO mi;
        int primary = 0;

        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(G.mons[i].hmon, &mi))
            primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;

        memset(m, 0, sizeof(*m));
        m->bounds.x = G.mons[i].rc.left;
        m->bounds.y = G.mons[i].rc.top;
        m->bounds.w = G.mons[i].rc.right  - G.mons[i].rc.left;
        m->bounds.h = G.mons[i].rc.bottom - G.mons[i].rc.top;
        m->primary  = primary;

        if (friendly_for_device(G.mons[i].device, name, 128, path, 256) && path[0]) {
            WideCharToMultiByte(CP_UTF8, 0, path, -1, m->id, OG_ID_LEN, NULL, NULL);
        } else {
            char dev[64];
            WideCharToMultiByte(CP_UTF8, 0, G.mons[i].device, -1, dev,
                                (int)sizeof dev, NULL, NULL);
            snprintf(m->id, OG_ID_LEN, "gdi-%s", dev);
        }
        {
            char nm[160];
            if (name[0])
                WideCharToMultiByte(CP_UTF8, 0, name, -1, nm, (int)sizeof nm, NULL, NULL);
            else
                strcpy(nm, "Display");
            snprintf(m->name, OG_NAME_LEN, "%s (%dx%d)%s",
                        nm, m->bounds.w, m->bounds.h, primary ? " *" : "");
        }
        m->native = (void *)(size_t)(i + 1);
    }
    return (i > max) ? max : i;
}

int og_plat_displays_changed(void)
{
    int r = G.displays_dirty;
    G.displays_dirty = 0;
    return r;
}

/* ------------------------------------------------------------------ */
/* Input                                                                */
/* ------------------------------------------------------------------ */

int og_plat_cursor_pos(int *x, int *y)
{
    POINT p;
    if (!GetCursorPos(&p)) return 0;
    if (x) *x = p.x;
    if (y) *y = p.y;
    return 1;
}

long og_plat_idle_ms(void)
{
    LASTINPUTINFO li;
    li.cbSize = sizeof li;
    if (!GetLastInputInfo(&li)) return -1;
    return (long)(GetTickCount() - li.dwTime);
}

unsigned og_plat_inhibit_flags(void)
{
    /* Windows exposes no supported way to enumerate other processes' power
     * requests (powercfg /requests uses a private interface), so the one
     * documented global signal is used instead. It covers exactly the case
     * GDI capture cannot see: an exclusive-fullscreen Direct3D game, plus
     * presentation mode. Everything else is caught by pixel sampling. */
    QUERY_USER_NOTIFICATION_STATE s;
    if (SHQueryUserNotificationState(&s) != S_OK) return OG_INHIBIT_NONE;
    if (s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE)
        return OG_INHIBIT_DISPLAY | OG_INHIBIT_IDLE;
    return OG_INHIBIT_NONE;
}

int og_plat_inhibitor_name(char *buf, size_t len)
{
    /* SHQueryUserNotificationState reports a state, not an owner, and
     * enumerating other processes' power requests has no supported API. */
    (void)buf; (void)len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fullscreen detection                                                 */
/* ------------------------------------------------------------------ */

static BOOL CALLBACK enum_fs_cb(HWND h, LPARAM lp)
{
    RECT rc;
    LONG_PTR ex;
    (void)lp;

    if (G.fs_count >= 24) return FALSE;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    if (h == GetShellWindow()) return TRUE;
    ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (!GetWindowRect(h, &rc)) return TRUE;
    if (rc.right - rc.left < 64 || rc.bottom - rc.top < 64) return TRUE;

    G.fs_rect[G.fs_count++] = rc;
    return TRUE;
}

static void refresh_fullscreen_cache(void)
{
    long now = og_plat_now_ms();
    if (now - G.fs_cache_ms < 500) return;
    G.fs_cache_ms = now;
    G.fs_count = 0;
    EnumWindows(enum_fs_cb, 0);
}

int og_plat_monitor_has_fullscreen(const og_monitor *m)
{
    int i;
    if (!m) return 0;

    /* An exclusive-fullscreen game is reported here rather than by geometry. */
    {
        QUERY_USER_NOTIFICATION_STATE s;
        if (SHQueryUserNotificationState(&s) == S_OK &&
            s == QUNS_RUNNING_D3D_FULL_SCREEN)
            return 1;
    }

    refresh_fullscreen_cache();
    for (i = 0; i < G.fs_count; ++i) {
        RECT r = G.fs_rect[i];
        int w = r.right - r.left, h = r.bottom - r.top;
        int dx = r.left - m->bounds.x, dy = r.top - m->bounds.y;
        if (dx < -8 || dx > m->bounds.w / 2) continue;
        if (dy < -8 || dy > m->bounds.h / 2) continue;
        if (w * 10 >= m->bounds.w * 9 && h * 10 >= m->bounds.h * 9) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Content sampling                                                     */
/* ------------------------------------------------------------------ */

int og_plat_capture_available(void) { return 1; }

int og_plat_capture_signature(const og_monitor *m, og_signature *sig)
{
    int idx;
    HDC src = NULL, mem = NULL;
    HBITMAP bmp = NULL, old = NULL;
    BITMAPINFO bi;
    unsigned char buf[OG_SIG_CELLS * 4];
    int ok = 0, k;

    if (!m || !sig) return 0;
    idx = (int)(size_t)m->native - 1;
    if (idx < 0 || idx >= G.mon_count) return 0;

    /* A DC for this display alone: source coordinates start at 0,0 and no
     * virtual-desktop offset arithmetic is needed. */
    src = CreateDCW(L"DISPLAY", G.mons[idx].device, NULL, NULL);
    if (!src) return 0;
    mem = CreateCompatibleDC(src);
    if (!mem) goto done;
    bmp = CreateCompatibleBitmap(src, OG_SIG_COLS, OG_SIG_ROWS);
    if (!bmp) goto done;
    old = (HBITMAP)SelectObject(mem, bmp);

    /* HALFTONE averages the source block instead of point sampling it, so a
     * small moving subject on a still background is not lost in the downscale. */
    SetStretchBltMode(mem, HALFTONE);
    SetBrushOrgEx(mem, 0, 0, NULL);
    if (!StretchBlt(mem, 0, 0, OG_SIG_COLS, OG_SIG_ROWS,
                    src, 0, 0, m->bounds.w, m->bounds.h,
                    SRCCOPY | CAPTUREBLT))
        goto done;

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = OG_SIG_COLS;
    bi.bmiHeader.biHeight = -OG_SIG_ROWS;   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    if (!GetDIBits(mem, bmp, 0, OG_SIG_ROWS, buf, &bi, DIB_RGB_COLORS))
        goto done;

    for (k = 0; k < OG_SIG_CELLS; ++k) {
        unsigned b = buf[k * 4 + 0], g = buf[k * 4 + 1], r = buf[k * 4 + 2];
        sig->cell[k] = (unsigned char)((77u * r + 150u * g + 29u * b) >> 8);
    }
    sig->valid = 1;
    ok = 1;

done:
    if (old) SelectObject(mem, old);
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    if (src) DeleteDC(src);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Overlays                                                             */
/* ------------------------------------------------------------------ */

static og_overlay_win *slot_for(og_monitor *m)
{
    int i;
    if (m->overlay) return (og_overlay_win *)m->overlay;
    for (i = 0; i < OG_MAX_MONITORS; ++i) {
        if (!G.overlays[i].hwnd) {
            m->overlay = &G.overlays[i];
            return &G.overlays[i];
        }
    }
    return NULL;
}

int og_plat_overlay_show(og_monitor *m, int opacity_pct, int fade_ms)
{
    og_overlay_win *ov;
    BYTE alpha;

    if (!m) return 0;
    ov = slot_for(m);
    if (!ov) return 0;

    if (opacity_pct < 10) opacity_pct = 10;
    if (opacity_pct > 100) opacity_pct = 100;
    ov->opacity_pct = opacity_pct;

    if (!ov->hwnd) {
        ov->hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
            WS_EX_LAYERED | WS_EX_TRANSPARENT,   /* TRANSPARENT: click-through */
            OG_OVERLAY_CLASS, L"", WS_POPUP,
            m->bounds.x, m->bounds.y, m->bounds.w, m->bounds.h,
            NULL, NULL, G.hinst, NULL);
        if (!ov->hwnd) {
            og_log(OG_LOG_ERROR, "curtain window failed (%lu)", GetLastError());
            return 0;
        }
    }
    ov->fade_ms = fade_ms > 0 ? fade_ms : 0;
    ov->fade_start_ms = og_plat_now_ms();
    alpha = (BYTE)(ov->fade_ms ? 0 : (255 * opacity_pct / 100));
    SetLayeredWindowAttributes(ov->hwnd, 0, alpha, LWA_ALPHA);

    SetWindowPos(ov->hwnd, HWND_TOPMOST,
                 m->bounds.x, m->bounds.y, m->bounds.w, m->bounds.h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(ov->hwnd, NULL, TRUE);
    ov->visible = 1;
    return 1;
}

void og_plat_overlay_hide(og_monitor *m)
{
    og_overlay_win *ov;
    if (!m || !m->overlay) return;
    ov = (og_overlay_win *)m->overlay;
    if (!ov->hwnd || !ov->visible) return;
    ShowWindow(ov->hwnd, SW_HIDE);
    ov->visible = 0;
    ov->fade_ms = 0;
}

void og_plat_overlay_destroy(og_monitor *m)
{
    og_overlay_win *ov;
    if (!m || !m->overlay) return;
    ov = (og_overlay_win *)m->overlay;
    if (ov->hwnd) DestroyWindow(ov->hwnd);
    memset(ov, 0, sizeof(*ov));
    m->overlay = NULL;
}

/* ------------------------------------------------------------------ */

void og_plat_pump(void)
{
    long now = og_plat_now_ms();
    int i;

    for (i = 0; i < OG_MAX_MONITORS; ++i) {
        og_overlay_win *ov = &G.overlays[i];
        long dt;
        if (!ov->hwnd || !ov->visible) continue;

        if (ov->fade_ms) {
            BYTE full = (BYTE)(255 * ov->opacity_pct / 100);
            dt = now - ov->fade_start_ms;
            if (dt >= ov->fade_ms) {
                SetLayeredWindowAttributes(ov->hwnd, 0, full, LWA_ALPHA);
                ov->fade_ms = 0;
            } else {
                SetLayeredWindowAttributes(ov->hwnd, 0,
                    (BYTE)((int)full * dt / ov->fade_ms), LWA_ALPHA);
            }
        }
        /* Some apps grab HWND_TOPMOST when they take focus; reassert quietly. */
        SetWindowPos(ov->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}
