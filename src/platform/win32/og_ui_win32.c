/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_ui_win32.c - tray icon, settings window and the Windows entry point.
 *
 * Controls are created directly rather than from a dialog resource, so the
 * whole app builds from source with no .rc step and no resource compiler.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>

#include "oledguard/og_core.h"
#include "oledguard/og_config.h"
#include "oledguard/og_platform.h"
#include "oledguard/og_log.h"

#include <stdio.h>
#include <string.h>

#define OG_MAIN_CLASS   L"OLEDGuardMain"
#define OG_MUTEX_NAME   L"Local\\OLEDGuardSingleInstance"

#define WM_OG_TRAY      (WM_APP + 1)
#define WM_OG_SHOW      (WM_APP + 2)

#define ID_TICK_TIMER   1
#define ID_UI_TIMER     2

#define IDC_SUMMARY     1000
#define IDC_TIMEOUT     1001
#define IDC_WAKE        1002
#define IDC_INHIBIT     1003
#define IDC_DIM         1004
#define IDC_BLANK       1005
#define IDC_PAUSE       1006
#define IDC_HIDE        1007
#define IDC_DIMLABEL    1008
#define IDC_MON_BASE    2000     /* check = BASE + i*2, status = BASE + i*2 + 1 */

#define IDM_SETTINGS    3001
#define IDM_BLANK       3002
#define IDM_PAUSE       3003
#define IDM_QUIT        3004

#define OG_UI_MAX_ROWS  8
#define ROW_H           44
#define PAD             16

static const int kTimeoutPresets[] = { 15, 30, 60, 120, 300, 600, 1200, 1800 };
#define TIMEOUT_PRESET_COUNT ((int)(sizeof kTimeoutPresets / sizeof kTimeoutPresets[0]))

/* ------------------------------------------------------------------ */

static struct {
    HINSTANCE hinst;
    HWND      hwnd;
    HFONT     font;
    NOTIFYICONDATAW nid;

    og_config cfg;
    char      cfg_path[1024];
    og_core  *core;

    int   row_count;
    char  row_id[OG_UI_MAX_ROWS][OG_ID_LEN];
    int   suppress;
    int   monitors_top;
} U;

/* ------------------------------------------------------------------ */

static void save_config(void)
{
    if (!og_config_save(&U.cfg, U.cfg_path))
        og_log(OG_LOG_WARN, "could not write the config file");
}

static HWND mk(const WCHAR *cls, const WCHAR *text, DWORD style,
               int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, U.hwnd, (HMENU)(INT_PTR)id,
                             U.hinst, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)U.font, TRUE);
    return c;
}

/* ------------------------------------------------------------------ */
/* Monitor rows                                                         */
/* ------------------------------------------------------------------ */

static void destroy_monitor_rows(void)
{
    int i;
    for (i = 0; i < OG_UI_MAX_ROWS; ++i) {
        HWND a = GetDlgItem(U.hwnd, IDC_MON_BASE + i * 2);
        HWND b = GetDlgItem(U.hwnd, IDC_MON_BASE + i * 2 + 1);
        if (a) DestroyWindow(a);
        if (b) DestroyWindow(b);
    }
    U.row_count = 0;
}

static void build_monitor_rows(void)
{
    int i, n, y = U.monitors_top;

    destroy_monitor_rows();
    n = og_core_monitor_count(U.core);
    if (n > OG_UI_MAX_ROWS) n = OG_UI_MAX_ROWS;

    for (i = 0; i < n; ++i) {
        og_core_status st;
        WCHAR wname[OG_NAME_LEN];
        HWND chk;

        if (!og_core_status_at(U.core, i, &st)) continue;
        MultiByteToWideChar(CP_UTF8, 0, st.mon->name, -1, wname, OG_NAME_LEN);

        chk = mk(L"BUTTON", wname, BS_AUTOCHECKBOX,
                 PAD, y, 480, 20, IDC_MON_BASE + i * 2);
        Button_SetCheck(chk, st.enabled ? BST_CHECKED : BST_UNCHECKED);

        mk(L"STATIC", L"", SS_LEFTNOWORDWRAP,
           PAD + 20, y + 20, 470, 18, IDC_MON_BASE + i * 2 + 1);

        {
            size_t idn = strlen(st.mon->id);
            if (idn > OG_ID_LEN - 1) idn = OG_ID_LEN - 1;
            memcpy(U.row_id[i], st.mon->id, idn);
            U.row_id[i][idn] = '\0';
        }
        y += ROW_H;
        ++U.row_count;
    }
}

static int rows_match_core(void)
{
    int i, n = og_core_monitor_count(U.core);
    if (n > OG_UI_MAX_ROWS) n = OG_UI_MAX_ROWS;
    if (n != U.row_count) return 0;
    for (i = 0; i < n; ++i) {
        og_core_status st;
        if (!og_core_status_at(U.core, i, &st)) return 0;
        if (strcmp(st.mon->id, U.row_id[i]) != 0) return 0;
    }
    return 1;
}

static void describe(char *buf, size_t len, const og_core_status *st)
{
    if (st->hold & OG_HOLD_DISABLED)  { snprintf(buf, len, "not guarded"); return; }
    if (st->state == OG_ST_BLANK) {
        snprintf(buf, len, "blanked - move the pointer here to wake it");
        return;
    }
    if (st->hold & OG_HOLD_MOTION) {
        snprintf(buf, len, "awake - playback detected (%.0f%% of the screen changing)",
                    st->motion_ratio * 100.0);
        return;
    }
    if (st->hold & OG_HOLD_SYS_INHIBIT) {
        snprintf(buf, len, "awake - a fullscreen app is holding the display on");
        return;
    }
    if (st->hold & OG_HOLD_INHIBIT_SEEN) {
        snprintf(buf, len,
                 "awake - idle %lds of %ds (a keep-awake request is being "
                 "ignored while the screen is sampled)",
                 st->idle_ms / 1000, U.cfg.timeout_sec);
        return;
    }
    snprintf(buf, len, "awake - idle %lds of %ds (last change %.1f%%)",
                st->idle_ms / 1000, U.cfg.timeout_sec, st->motion_ratio * 100.0);
}

static void refresh_ui(void)
{
    int i, blanked = 0;
    char line[256];
    WCHAR wline[256];

    if (!IsWindowVisible(U.hwnd)) return;
    if (!rows_match_core()) build_monitor_rows();

    for (i = 0; i < U.row_count; ++i) {
        og_core_status st;
        if (!og_core_status_at(U.core, i, &st)) continue;
        describe(line, sizeof line, &st);
        MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, 256);
        SetDlgItemTextW(U.hwnd, IDC_MON_BASE + i * 2 + 1, wline);
        U.suppress = 1;
        Button_SetCheck(GetDlgItem(U.hwnd, IDC_MON_BASE + i * 2),
                        st.enabled ? BST_CHECKED : BST_UNCHECKED);
        U.suppress = 0;
        if (st.state == OG_ST_BLANK) ++blanked;
    }

    snprintf(line, sizeof line, "%s - %s - %d of %d screen%s blanked",
                og_plat_name(),
                og_core_paused(U.core) ? "paused" : "guarding",
                blanked, U.row_count, U.row_count == 1 ? "" : "s");
    MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, 256);
    SetDlgItemTextW(U.hwnd, IDC_SUMMARY, wline);
    SetDlgItemTextW(U.hwnd, IDC_PAUSE,
                    og_core_paused(U.core) ? L"Resume" : L"Pause");
}

/* ------------------------------------------------------------------ */
/* Settings controls                                                    */
/* ------------------------------------------------------------------ */

static void build_controls(void)
{
    int y, i;
    HWND cb;

    y = PAD;
    mk(L"STATIC", L"", SS_LEFTNOWORDWRAP, PAD, y, 500, 18, IDC_SUMMARY);
    y += 28;

    mk(L"STATIC", L"Screens to guard", SS_LEFTNOWORDWRAP, PAD, y, 300, 18, -1);
    y += 24;
    U.monitors_top = y;
    y += ROW_H * OG_UI_MAX_ROWS;

    mk(L"STATIC", L"Blank after", SS_LEFTNOWORDWRAP, PAD, y + 4, 160, 18, -1);
    cb = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            PAD + 170, y, 300, 300, IDC_TIMEOUT);
    for (i = 0; i < TIMEOUT_PRESET_COUNT; ++i) {
        WCHAR t[64];
        int s = kTimeoutPresets[i];
        if (s < 60) swprintf(t, 64, L"%d seconds", s);
        else        swprintf(t, 64, L"%d minute%s", s / 60, s == 60 ? L"" : L"s");
        ComboBox_AddString(cb, t);
        ComboBox_SetItemData(cb, i, (LPARAM)s);
        if (s == U.cfg.timeout_sec) ComboBox_SetCurSel(cb, i);
    }
    if (ComboBox_GetCurSel(cb) < 0) {
        WCHAR t[64];
        int idx;
        swprintf(t, 64, L"%d seconds (from config file)", U.cfg.timeout_sec);
        idx = ComboBox_AddString(cb, t);
        ComboBox_SetItemData(cb, idx, (LPARAM)U.cfg.timeout_sec);
        ComboBox_SetCurSel(cb, idx);
    }
    y += 32;

    mk(L"STATIC", L"Wake on", SS_LEFTNOWORDWRAP, PAD, y + 4, 160, 18, -1);
    cb = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            PAD + 170, y, 340, 200, IDC_WAKE);
    ComboBox_AddString(cb, L"Pointer movement over that screen only");
    ComboBox_AddString(cb, L"Pointer movement, or typing while the pointer rests there");
    ComboBox_AddString(cb, L"Any keyboard or mouse activity wakes every screen");
    ComboBox_SetCurSel(cb, (int)U.cfg.wake_mode);
    y += 34;

    mk(L"STATIC", L"Apps keeping the display on", SS_LEFTNOWORDWRAP,
       PAD, y + 4, 160, 18, -1);
    cb = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            PAD + 170, y, 340, 200, IDC_INHIBIT);
    ComboBox_AddString(cb, L"Ignore them - what is on screen decides");
    ComboBox_AddString(cb, L"Trust them only when screen sampling is unavailable");
    ComboBox_AddString(cb, L"Always trust them");
    ComboBox_SetCurSel(cb, (int)U.cfg.inhibitor_policy);
    y += 34;

    mk(L"STATIC", L"Curtain opacity % (100 = pixels off)", SS_LEFTNOWORDWRAP,
       PAD, y + 4, 260, 18, IDC_DIMLABEL);
    {
        HWND t = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS,
                    PAD + 270, y, 200, 28, IDC_DIM);
        SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
        SendMessageW(t, TBM_SETPOS, TRUE, U.cfg.curtain_opacity);
    }
    y += 40;

    mk(L"BUTTON", L"Blank now", BS_PUSHBUTTON, PAD, y, 110, 28, IDC_BLANK);
    mk(L"BUTTON", L"Pause",     BS_PUSHBUTTON, PAD + 120, y, 110, 28, IDC_PAUSE);
    mk(L"BUTTON", L"Hide",      BS_PUSHBUTTON, PAD + 240, y, 110, 28, IDC_HIDE);

    build_monitor_rows();
}

/* ------------------------------------------------------------------ */
/* Tray                                                                 */
/* ------------------------------------------------------------------ */

static void tray_add(void)
{
    memset(&U.nid, 0, sizeof U.nid);
    U.nid.cbSize = sizeof U.nid;
    U.nid.hWnd = U.hwnd;
    U.nid.uID = 1;
    U.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    U.nid.uCallbackMessage = WM_OG_TRAY;
    U.nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(U.nid.szTip, L"OLEDGuard");
    Shell_NotifyIconW(NIM_ADD, &U.nid);
}

static void tray_remove(void) { Shell_NotifyIconW(NIM_DELETE, &U.nid); }

static void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT p;

    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings…");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_BLANK, L"Blank now");
    AppendMenuW(m, MF_STRING, IDM_PAUSE,
                og_core_paused(U.core) ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"Quit OLEDGuard");

    GetCursorPos(&p);
    /* Required so the menu closes when the user clicks elsewhere. */
    SetForegroundWindow(U.hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, U.hwnd, NULL);
    PostMessageW(U.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void show_settings(void)
{
    og_core_wake_all(U.core);
    ShowWindow(U.hwnd, SW_SHOW);
    SetForegroundWindow(U.hwnd);
    refresh_ui();
}

/* ------------------------------------------------------------------ */

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == ID_TICK_TIMER) og_core_tick(U.core);
        else if (wp == ID_UI_TIMER) refresh_ui();
        return 0;

    case WM_OG_SHOW:
        show_settings();
        return 0;

    case WM_OG_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK)
            show_settings();
        else if (LOWORD(lp) == WM_RBUTTONUP)
            tray_menu();
        return 0;

    case WM_HSCROLL:
        if (GetDlgCtrlID((HWND)lp) == IDC_DIM) {
            U.cfg.curtain_opacity = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
            save_config();
        }
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id >= IDC_MON_BASE && id < IDC_MON_BASE + OG_UI_MAX_ROWS * 2 &&
            ((id - IDC_MON_BASE) % 2) == 0) {
            if (!U.suppress) {
                int index = (id - IDC_MON_BASE) / 2;
                int on = Button_GetCheck((HWND)lp) == BST_CHECKED;
                og_core_set_enabled(U.core, index, on);
                save_config();
            }
            return 0;
        }

        switch (id) {
        case IDC_TIMEOUT:
            if (code == CBN_SELCHANGE) {
                HWND cb = (HWND)lp;
                int sel = ComboBox_GetCurSel(cb);
                if (sel >= 0) {
                    U.cfg.timeout_sec = (int)ComboBox_GetItemData(cb, sel);
                    save_config();
                }
            }
            return 0;
        case IDC_WAKE:
            if (code == CBN_SELCHANGE) {
                U.cfg.wake_mode = (og_wake_mode)ComboBox_GetCurSel((HWND)lp);
                save_config();
            }
            return 0;
        case IDC_INHIBIT:
            if (code == CBN_SELCHANGE) {
                U.cfg.inhibitor_policy =
                    (og_inhibitor_policy)ComboBox_GetCurSel((HWND)lp);
                save_config();
            }
            return 0;
        case IDC_BLANK:
        case IDM_BLANK:
            og_core_blank_now(U.core);
            return 0;
        case IDC_PAUSE:
        case IDM_PAUSE:
            og_core_set_paused(U.core, !og_core_paused(U.core));
            refresh_ui();
            return 0;
        case IDC_HIDE:
            ShowWindow(U.hwnd, SW_HIDE);
            return 0;
        case IDM_SETTINGS:
            show_settings();
            return 0;
        case IDM_QUIT:
            DestroyWindow(U.hwnd);
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
        /* Closing the window keeps guarding; Quit is on the tray menu. */
        ShowWindow(h, SW_HIDE);
        return 0;

    case WM_DISPLAYCHANGE:
        og_core_refresh_monitors(U.core);
        build_monitor_rows();
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);

    case WM_DESTROY:
        KillTimer(h, ID_TICK_TIMER);
        KillTimer(h, ID_UI_TIMER);
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdline, int nShow)
{
    MSG msg;
    HANDLE mutex;
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    int height;

    (void)hPrev; (void)nShow;

    /* One instance guards the screens; launching again just shows its window. */
    mutex = CreateMutexW(NULL, TRUE, OG_MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(OG_MAIN_CLASS, NULL);
        if (other) PostMessageW(other, WM_OG_SHOW, 0, 0);
        return 0;
    }

    if (wcsstr(cmdline, L"--verbose")) og_log_set_level(OG_LOG_DEBUG);

    U.hinst = hInst;

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    og_config_defaults(&U.cfg);
    if (!og_config_path(U.cfg_path, sizeof U.cfg_path)) return 1;
    og_config_load(&U.cfg, U.cfg_path);

    if (!og_plat_init()) return 1;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = OG_MAIN_CLASS;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    height = PAD * 2 + 28 + 24 + ROW_H * OG_UI_MAX_ROWS + 32 + 34 + 30 + 40 + 40;
    U.hwnd = CreateWindowExW(0, OG_MAIN_CLASS, L"OLEDGuard",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 560, height,
                             NULL, NULL, hInst, NULL);
    if (!U.hwnd) return 1;

    {
        NONCLIENTMETRICSW ncm;
        ncm.cbSize = sizeof ncm;
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
            U.font = CreateFontIndirectW(&ncm.lfMessageFont);
        if (!U.font) U.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }

    U.core = og_core_create(&U.cfg);
    if (!U.core) { og_plat_shutdown(); return 1; }

    build_controls();
    tray_add();

    SetTimer(U.hwnd, ID_TICK_TIMER, OG_TICK_MS, NULL);
    SetTimer(U.hwnd, ID_UI_TIMER, 250, NULL);

    /* Show the window on the very first run so the user can pick screens. */
    if (!U.cfg.start_minimised || U.cfg.mon_count == 0) show_settings();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(U.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    og_core_wake_all(U.core);
/* Deliberately not saving here.
 *
 * Every setting is written the moment it changes, so a save on the way out
 * adds nothing - and it actively destroys work: edit the ini by hand while
 * the app is running, quit, and this would write the stale in-memory copy
 * straight back over the edit. Hand-editing then looks like it did nothing. */
    og_core_destroy(U.core);
    og_plat_shutdown();
    if (U.font) DeleteObject(U.font);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return (int)msg.wParam;
}
