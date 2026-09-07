/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_ui_win32.c - tray icon, settings window and the Windows entry point.
 *
 * Controls are still created in code rather than from a dialog resource, but
 * nothing about their size is written down. Every metric is derived from two
 * things measured at run time: the DPI of the monitor the window is on, and
 * the extent of the actual strings in the actual font. A dialog template
 * cannot do the second, which is why labels used to be clipped whenever the
 * system font came out larger than the 18 pixels the layout assumed.
 *
 * The three things that genuinely need a resource - the icon, the version
 * block and the manifest asking for themed controls and per-monitor DPI - live
 * in packaging/windows/oledguard.rc.
 *
 * Dark mode follows the system setting. The documented half is the title bar
 * (DWM) and the per-control theme names; the half that makes combo box drop
 * downs and the tray menu follow is an unexported uxtheme entry point, called
 * by ordinal and only on builds known to have it. If it is missing the window
 * is still readable, just lighter in places.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <objbase.h>

#include "oledguard/og_core.h"
#include "oledguard/og_config.h"
#include "oledguard/og_platform.h"
#include "oledguard/og_log.h"
#include "og_resource.h"

#include <stdio.h>
#include <string.h>

#define OG_MAIN_CLASS   L"OLEDGuardMain"
#define OG_MUTEX_NAME   L"Local\\OLEDGuardSingleInstance"
#define OG_RUN_VALUE    L"OLEDGuard"
#define OG_REG_APP      L"Software\\OLEDGuard"
#define OG_REG_RUN      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define OG_REG_APPROVED \
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"

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
#define IDC_SECTION     1008
#define IDC_DIMVAL      1009
#define IDC_LOGIN       1010
#define IDC_LOGINHINT   1011
#define IDC_L_TIMEOUT   1012
#define IDC_L_WAKE      1013
#define IDC_L_INHIBIT   1014
#define IDC_L_DIM       1015
#define IDC_MON_BASE    2000     /* check = BASE + i*2, status = BASE + i*2 + 1 */

#define IDM_SETTINGS    3001
#define IDM_BLANK       3002
#define IDM_PAUSE       3003
#define IDM_QUIT        3004

#define OG_UI_MAX_ROWS  8

/* Not in the Windows 7 headers this file compiles against. */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define OG_DWMWA_DARK_PRE20H1 19

static const int kTimeoutPresets[] = { 15, 30, 60, 120, 300, 600, 1200, 1800 };
#define TIMEOUT_PRESET_COUNT ((int)(sizeof kTimeoutPresets / sizeof kTimeoutPresets[0]))

static const WCHAR *kWakeItems[] = {
    L"Pointer movement over that screen only",
    L"Pointer movement, or typing while the pointer rests there",
    L"Any keyboard or mouse activity wakes every screen"
};
static const WCHAR *kInhibitItems[] = {
    L"Ignore them - what is on screen decides",
    L"Trust them only when screen sampling is unavailable",
    L"Always trust them"
};

/* ------------------------------------------------------------------ */

static struct {
    HINSTANCE hinst;
    HWND      hwnd;
    HFONT     font;
    HFONT     font_bold;
    HBRUSH    bg;
    HICON     icon_big;
    HICON     icon_small;
    NOTIFYICONDATAW nid;

    og_config cfg;
    char      cfg_path[1024];
    og_core  *core;

    WCHAR exe_path[MAX_PATH];
    WCHAR install_path[MAX_PATH];

    int   dark;
    int   row_count;
    char  row_id[OG_UI_MAX_ROWS][OG_ID_LEN];
    int   suppress;
    int   monitors_top;
} U;

/* Everything the layout needs, all of it measured rather than assumed. */
static struct {
    int dpi;
    int pad;        /* window margin */
    int gap;        /* between stacked controls */
    int line;       /* one line of text in the message font */
    int ctl_h;      /* combo box and button height */
    int label_w;    /* left column */
    int field_w;    /* right column */
    int indent;     /* status line under a monitor checkbox */
    int row_h;      /* one monitor: name line, status line, breathing room */
    int win_w;      /* client width */
    int win_h;      /* client height, filled in by relayout */
} M;

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

#define OG_SCALE(px) MulDiv((px), M.dpi, 96)

static void save_config(void)
{
    if (!og_config_save(&U.cfg, U.cfg_path))
        og_log(OG_LOG_WARN, "could not write the config file");
}

static HWND item(int id) { return GetDlgItem(U.hwnd, id); }

static void set_text(int id, const WCHAR *s)
{
    HWND c = item(id);
    if (c) SetWindowTextW(c, s);
}

/* Windows build number, for the dark-mode entry points that only exist on
 * some of them. GetVersionEx lies unless manifested; RtlGetVersion never does. */
static DWORD windows_build(void)
{
    typedef LONG (WINAPI *pfnRtlGetVersion)(PRTL_OSVERSIONINFOW);
    static DWORD build;
    static int done;

    if (!done) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        pfnRtlGetVersion f = nt ? (pfnRtlGetVersion)(void *)
            GetProcAddress(nt, "RtlGetVersion") : NULL;
        RTL_OSVERSIONINFOW vi;
        memset(&vi, 0, sizeof vi);
        vi.dwOSVersionInfoSize = sizeof vi;
        if (f && f(&vi) == 0) build = vi.dwBuildNumber;
        done = 1;
    }
    return build;
}

/* ------------------------------------------------------------------ */
/* Theme                                                                */
/* ------------------------------------------------------------------ */

static int system_dark_mode(void)
{
    DWORD v = 1, cb = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &cb)
        != ERROR_SUCCESS)
        return 0;
    return v == 0;
}

/* uxtheme's dark-mode switch has never been exported by name. Without it the
 * documented DarkMode_* theme names do nothing, so combo box drop downs and
 * the tray menu stay white on an otherwise dark window. Ordinal 135 has meant
 * this since 1809; below that build we do not call it at all. */
static void allow_dark_mode(int on)
{
    typedef int (WINAPI *pfnSetPreferredAppMode)(int);   /* 0 default, 1 allow */
    typedef void (WINAPI *pfnFlushMenuThemes)(void);
    static pfnSetPreferredAppMode set_mode;
    static pfnFlushMenuThemes flush;
    static int tried;

    if (windows_build() < 17763) return;
    if (!tried) {
        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (ux) {
            set_mode = (pfnSetPreferredAppMode)(void *)
                GetProcAddress(ux, MAKEINTRESOURCEA(135));
            flush = (pfnFlushMenuThemes)(void *)
                GetProcAddress(ux, MAKEINTRESOURCEA(136));
        }
        tried = 1;
    }
    if (set_mode) set_mode(on ? 1 : 0);
    if (flush) flush();
}

static void dark_title_bar(HWND h, int on)
{
    BOOL b = on ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                     &b, sizeof b)))
        DwmSetWindowAttribute(h, OG_DWMWA_DARK_PRE20H1, &b, sizeof b);
}

static COLORREF col_text(void)
{
    return U.dark ? RGB(240, 240, 240) : GetSysColor(COLOR_WINDOWTEXT);
}
static COLORREF col_dim(void)
{
    return U.dark ? RGB(168, 168, 168) : GetSysColor(COLOR_GRAYTEXT);
}
static COLORREF col_back(void)
{
    return U.dark ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW);
}

static void theme_control(HWND c)
{
    WCHAR cls[32];
    if (!c) return;
    GetClassNameW(c, cls, 32);
    if (lstrcmpiW(cls, L"COMBOBOX") == 0)
        SetWindowTheme(c, U.dark ? L"DarkMode_CFD" : NULL, NULL);
    else
        SetWindowTheme(c, U.dark ? L"DarkMode_Explorer" : NULL, NULL);
}

static BOOL CALLBACK theme_child(HWND c, LPARAM p)
{
    (void)p;
    theme_control(c);
    return TRUE;
}

static void apply_theme(void)
{
    U.dark = system_dark_mode();
    allow_dark_mode(U.dark);
    if (U.bg) DeleteObject(U.bg);
    U.bg = CreateSolidBrush(col_back());
    if (U.hwnd) {
        dark_title_bar(U.hwnd, U.dark);
        EnumChildWindows(U.hwnd, theme_child, 0);
        InvalidateRect(U.hwnd, NULL, TRUE);
    }
}

/* ------------------------------------------------------------------ */
/* Metrics                                                              */
/* ------------------------------------------------------------------ */

static int dpi_of_window(HWND h)
{
    typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);
    static pfnGetDpiForWindow f;
    static int tried;

    if (!tried) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) f = (pfnGetDpiForWindow)(void *)GetProcAddress(u, "GetDpiForWindow");
        tried = 1;
    }
    if (f && h) {
        UINT d = f(h);
        if (d) return (int)d;
    }
    {
        HDC dc = GetDC(NULL);
        int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(NULL, dc);
        return d ? d : 96;
    }
}

/* The message font at a given DPI. SystemParametersInfoForDpi is the honest
 * answer; the plain call reports the font for the system DPI, which is the
 * wrong size on every monitor that is not the primary one. */
static void make_fonts(int dpi)
{
    typedef BOOL (WINAPI *pfnSPIForDpi)(UINT, UINT, PVOID, UINT, UINT);
    static pfnSPIForDpi f;
    static int tried;
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    HFONT old_font = U.font, old_bold = U.font_bold;

    if (!tried) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) f = (pfnSPIForDpi)(void *)
            GetProcAddress(u, "SystemParametersInfoForDpi");
        tried = 1;
    }

    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;

    if (f && f(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, (UINT)dpi)) {
        lf = ncm.lfMessageFont;
    } else if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        lf = ncm.lfMessageFont;
        lf.lfHeight = MulDiv(lf.lfHeight, dpi, 96);
    } else {
        memset(&lf, 0, sizeof lf);
        lf.lfHeight = -MulDiv(9, dpi, 72);
        lstrcpyW(lf.lfFaceName, L"Segoe UI");
    }

    U.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    U.font_bold = CreateFontIndirectW(&lf);

    if (!U.font) U.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!U.font_bold) U.font_bold = U.font;

    if (old_bold && old_bold != old_font) DeleteObject(old_bold);
    if (old_font) DeleteObject(old_font);
}

static int text_w(HDC dc, const WCHAR *s)
{
    SIZE sz;
    if (!GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz)) return 0;
    return sz.cx;
}

static void measure(void)
{
    HDC dc = GetDC(U.hwnd);
    HGDIOBJ old;
    TEXTMETRICW tm;
    int i, w, longest;

    if (!dc) return;
    old = SelectObject(dc, U.font);
    GetTextMetricsW(dc, &tm);

    M.line = tm.tmHeight + tm.tmExternalLeading;
    M.pad = OG_SCALE(16);
    M.gap = OG_SCALE(9);
    M.indent = OG_SCALE(22);
    M.ctl_h = M.line + OG_SCALE(10);
    if (M.ctl_h < OG_SCALE(26)) M.ctl_h = OG_SCALE(26);
    M.row_h = M.line * 2 + OG_SCALE(10);

    /* Left column: the widest label, whatever the font makes of it. */
    longest = 0;
    {
        static const WCHAR *labels[] = {
            L"Blank after", L"Wake on",
            L"Apps keeping the display on", L"Curtain opacity"
        };
        for (i = 0; i < 4; ++i) {
            w = text_w(dc, labels[i]);
            if (w > longest) longest = w;
        }
    }
    M.label_w = longest + OG_SCALE(14);

    /* Right column: the widest thing that has to fit inside a combo box,
     * plus the drop-down arrow and the border it sits in. */
    longest = 0;
    for (i = 0; i < 3; ++i) {
        w = text_w(dc, kWakeItems[i]);
        if (w > longest) longest = w;
        w = text_w(dc, kInhibitItems[i]);
        if (w > longest) longest = w;
    }
    w = text_w(dc, L"1800 seconds (from config file)");
    if (w > longest) longest = w;
    M.field_w = longest + GetSystemMetrics(SM_CXVSCROLL) + OG_SCALE(16);

    SelectObject(dc, old);
    ReleaseDC(U.hwnd, dc);

    M.win_w = M.pad * 2 + M.label_w + M.gap + M.field_w;
}

static BOOL CALLBACK set_font_child(HWND c, LPARAM p)
{
    int id = GetDlgCtrlID(c);
    SendMessageW(c, WM_SETFONT,
                 (WPARAM)(id == IDC_SECTION ? U.font_bold : U.font), TRUE);
    (void)p;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Monitor rows                                                         */
/* ------------------------------------------------------------------ */

static HWND mk(const WCHAR *cls, const WCHAR *text, DWORD style,
               int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             0, 0, 10, 10, U.hwnd, (HMENU)(INT_PTR)id,
                             U.hinst, NULL);
    if (c) {
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        theme_control(c);
    }
    return c;
}

static void destroy_monitor_rows(void)
{
    int i;
    for (i = 0; i < OG_UI_MAX_ROWS; ++i) {
        HWND a = item(IDC_MON_BASE + i * 2);
        HWND b = item(IDC_MON_BASE + i * 2 + 1);
        if (a) DestroyWindow(a);
        if (b) DestroyWindow(b);
    }
    U.row_count = 0;
}

static void build_monitor_rows(void)
{
    int i, n;

    destroy_monitor_rows();
    n = og_core_monitor_count(U.core);
    if (n > OG_UI_MAX_ROWS) n = OG_UI_MAX_ROWS;

    for (i = 0; i < n; ++i) {
        og_core_status st;
        WCHAR wname[OG_NAME_LEN];
        HWND chk;
        size_t idn;

        if (!og_core_status_at(U.core, i, &st)) continue;
        MultiByteToWideChar(CP_UTF8, 0, st.mon->name, -1, wname, OG_NAME_LEN);

        chk = mk(L"BUTTON", wname, BS_AUTOCHECKBOX | WS_TABSTOP,
                 IDC_MON_BASE + i * 2, U.font);
        Button_SetCheck(chk, st.enabled ? BST_CHECKED : BST_UNCHECKED);

        mk(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
           IDC_MON_BASE + i * 2 + 1, U.font);

        idn = strlen(st.mon->id);
        if (idn > OG_ID_LEN - 1) idn = OG_ID_LEN - 1;
        memcpy(U.row_id[i], st.mon->id, idn);
        U.row_id[i][idn] = '\0';
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

/* ------------------------------------------------------------------ */
/* Open at login                                                        */
/* ------------------------------------------------------------------ */

static int reg_dword(HKEY root, const WCHAR *key, const WCHAR *name, DWORD *out)
{
    DWORD cb = sizeof *out;
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, NULL, out, &cb)
           == ERROR_SUCCESS;
}

/* Task Manager's Startup tab does not remove the Run value, it records a veto
 * beside it. Reporting that honestly beats a checkbox that claims to be on. */
static int startup_vetoed(void)
{
    BYTE b[16];
    DWORD cb = sizeof b;
    if (RegGetValueW(HKEY_CURRENT_USER, OG_REG_APPROVED, OG_RUN_VALUE,
                     RRF_RT_REG_BINARY, NULL, b, &cb) != ERROR_SUCCESS)
        return 0;
    return cb >= 1 && (b[0] & 1);
}

static int run_key_set(const WCHAR *want)
{
    WCHAR cur[MAX_PATH + 8];
    DWORD cb = sizeof cur;
    if (RegGetValueW(HKEY_CURRENT_USER, OG_REG_RUN, OG_RUN_VALUE,
                     RRF_RT_REG_SZ, NULL, cur, &cb) != ERROR_SUCCESS)
        return 0;
    return want ? (lstrcmpiW(cur, want) == 0) : 1;
}

static void apply_login_item(void)
{
    WCHAR quoted[MAX_PATH + 8];
    const WCHAR *note = L"";
    HKEY k;

    wsprintfW(quoted, L"\"%s\"", U.exe_path);

    if (RegCreateKeyExW(HKEY_CURRENT_USER, OG_REG_RUN, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        if (U.cfg.open_at_login) {
            if (RegSetValueExW(k, OG_RUN_VALUE, 0, REG_SZ, (const BYTE *)quoted,
                               (DWORD)((lstrlenW(quoted) + 1) * sizeof(WCHAR)))
                != ERROR_SUCCESS)
                note = L"Could not write the startup entry.";
        } else {
            RegDeleteValueW(k, OG_RUN_VALUE);
        }
        RegCloseKey(k);
    } else {
        note = L"Could not open the startup key.";
    }

    if (U.cfg.open_at_login && !*note) {
        if (startup_vetoed())
            note = L"Turned off in Task Manager, under Startup apps. "
                   L"Only you can turn it back on there.";
        else if (lstrcmpiW(U.exe_path, U.install_path) != 0)
            note = L"Registered at this copy's current location. Install it to "
                   L"your user folder if you plan to keep it there.";
    }

    set_text(IDC_LOGINHINT, note);
    ShowWindow(item(IDC_LOGINHINT), *note ? SW_SHOW : SW_HIDE);

    U.suppress = 1;
    Button_SetCheck(item(IDC_LOGIN),
                    U.cfg.open_at_login ? BST_CHECKED : BST_UNCHECKED);
    U.suppress = 0;
}

/* ------------------------------------------------------------------ */
/* Installing itself                                                    */
/* ------------------------------------------------------------------ */

static int install_dir(WCHAR *buf, size_t n)
{
    WCHAR base[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        return 0;
    if ((size_t)lstrlenW(base) + 32 > n) return 0;
    wsprintfW(buf, L"%s\\Programs\\OLEDGuard", base);
    return 1;
}

static void compute_paths(void)
{
    WCHAR dir[MAX_PATH];
    GetModuleFileNameW(NULL, U.exe_path, MAX_PATH);
    if (install_dir(dir, MAX_PATH))
        wsprintfW(U.install_path, L"%s\\OLEDGuard.exe", dir);
    else
        U.install_path[0] = L'\0';
}

/* A Start menu entry, so an installed copy can be found the usual way. */
static void make_start_menu_shortcut(const WCHAR *target)
{
    WCHAR programs[MAX_PATH], link[MAX_PATH], dir[MAX_PATH];
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;

    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, programs)))
        return;
    wsprintfW(link, L"%s\\OLEDGuard.lnk", programs);
    lstrcpynW(dir, target, MAX_PATH);
    {
        WCHAR *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = L'\0';
    }

    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkW, (void **)&sl))) {
        IShellLinkW_SetPath(sl, target);
        IShellLinkW_SetWorkingDirectory(sl, dir);
        IShellLinkW_SetDescription(sl, L"Per-monitor OLED burn-in curtain");
        IShellLinkW_SetIconLocation(sl, target, 0);
        if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile,
                                                 (void **)&pf))) {
            IPersistFile_Save(pf, link, TRUE);
            IPersistFile_Release(pf);
        }
        IShellLinkW_Release(sl);
    }
    CoUninitialize();
}

static int file_exists(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* What the prompt came back with. */
#define OG_STAY         0    /* run from where this copy sits */
#define OG_COPY         1    /* install, or overwrite what is installed */
#define OG_USE_EXISTING 2    /* start the copy that is already installed */

#define OG_BTN_USE      101
#define OG_BTN_COPY     102
#define OG_BTN_STAY     103

static int ask_to_install(const WCHAR *from, const WCHAR *to, int replacing,
                          int *never_again)
{
    typedef HRESULT (WINAPI *pfnTaskDialogIndirect)(const TASKDIALOGCONFIG *,
                                                    int *, int *, BOOL *);
    static const WCHAR *fresh =
        L"OLEDGuard is running from a folder that is easy to clear out, and a "
        L"startup entry would point straight at it.\n\n"
        L"Copying it to your user programs folder gives it somewhere permanent "
        L"to live and adds it to the Start menu. Nothing is written outside "
        L"your own user folder, and your settings are kept.";
    static const WCHAR *already =
        L"There is already a copy of OLEDGuard in your user programs folder.\n\n"
        L"Starting that one is usually what you want. Replace it only if the "
        L"file you have just opened is a newer build.";
    const WCHAR *body = replacing ? already : fresh;
    WCHAR detail[MAX_PATH * 2 + 64];
    HMODULE cc;
    pfnTaskDialogIndirect td;

    *never_again = 0;
    wsprintfW(detail, L"This copy:\t%s\nInstalled:\t%s", from, to);

    cc = GetModuleHandleW(L"comctl32.dll");
    td = cc ? (pfnTaskDialogIndirect)(void *)
        GetProcAddress(cc, "TaskDialogIndirect") : NULL;

    if (td) {
        TASKDIALOGCONFIG c;
        TASKDIALOG_BUTTON btn[3];
        int n = 0, pressed = 0;
        BOOL checked = FALSE;

        if (replacing) {
            btn[n].nButtonID = OG_BTN_USE;
            btn[n++].pszButtonText =
                L"Start the installed copy\nLeave both files as they are";
            btn[n].nButtonID = OG_BTN_COPY;
            btn[n++].pszButtonText =
                L"Replace the installed copy\nOverwrite it with this build "
                L"and start it";
            btn[n].nButtonID = OG_BTN_STAY;
            btn[n++].pszButtonText =
                L"Run this one from here\nLeave the installed copy alone";
        } else {
            btn[n].nButtonID = OG_BTN_COPY;
            btn[n++].pszButtonText =
                L"Install it\nCopy to my user folder and restart from there";
            btn[n].nButtonID = OG_BTN_STAY;
            btn[n++].pszButtonText =
                L"Run it from here\nLeave the file where it is";
        }

        memset(&c, 0, sizeof c);
        c.cbSize = sizeof c;
        c.hInstance = U.hinst;
        c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_EXPAND_FOOTER_AREA |
                    TDF_POSITION_RELATIVE_TO_WINDOW;
        c.pszWindowTitle = L"OLEDGuard";
        c.pszMainIcon = MAKEINTRESOURCEW(IDI_OLEDGUARD);
        c.pszMainInstruction = replacing ? L"OLEDGuard is already installed"
                                         : L"Install OLEDGuard?";
        c.pszContent = body;
        c.pszExpandedInformation = detail;
        c.pszExpandedControlText = L"Show the paths";
        /* Only meaningful on the fresh prompt: "stop asking, I will run it
         * from wherever it happens to be". It says nothing useful about which
         * of two existing copies to prefer. */
        c.pszVerificationText = replacing ? NULL : L"Do not ask again";
        c.cButtons = (UINT)n;
        c.pButtons = btn;
        c.nDefaultButton = replacing ? OG_BTN_USE : OG_BTN_COPY;

        if (SUCCEEDED(td(&c, &pressed, NULL, &checked))) {
            if (pressed == OG_BTN_COPY) return OG_COPY;
            if (pressed == OG_BTN_USE)  return OG_USE_EXISTING;
            *never_again = (!replacing && checked) ? 1 : 0;
            return OG_STAY;                     /* including the close box */
        }
    }

    /* No themed dialog available: ask plainly. */
    {
        WCHAR msg[1024 + MAX_PATH * 2];
        int r;
        if (replacing) {
            wsprintfW(msg, L"%s\n\n%s\n\nStart the installed copy?\n\n"
                           L"Yes - start it\nNo - replace it with this build\n"
                           L"Cancel - run this one from here", body, detail);
            r = MessageBoxW(NULL, msg, L"OLEDGuard",
                            MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (r == IDYES) return OG_USE_EXISTING;
            if (r == IDNO)  return OG_COPY;
            return OG_STAY;
        }
        wsprintfW(msg, L"%s\n\n%s\n\nInstall it now?", body, detail);
        r = MessageBoxW(NULL, msg, L"OLEDGuard",
                        MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
        if (r == IDYES) return OG_COPY;
        *never_again = 1;
        return OG_STAY;
    }
}

static void remember_not_to_ask(void)
{
    HKEY k;
    DWORD one = 1;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, OG_REG_APP, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, L"SkipInstallPrompt", 0, REG_DWORD,
                   (const BYTE *)&one, sizeof one);
    RegCloseKey(k);
}

/* Returns 1 when this process should quit and let another copy take over,
 * with `launch` naming the copy to start (empty if there is nothing to start).
 *
 * The caller must own the single-instance mutex when this is called and must
 * release it before starting `launch`, or the copy it starts will find the
 * mutex taken, look for a window that does not exist yet, and exit silently.
 */
static int offer_install(WCHAR *launch, size_t n)
{
    WCHAR dir[MAX_PATH];
    DWORD skip = 0;
    int never_again = 0, replacing, choice;

    if (n) launch[0] = L'\0';

    compute_paths();
    if (!U.install_path[0]) return 0;
    if (lstrcmpiW(U.exe_path, U.install_path) == 0) return 0;
    if (reg_dword(HKEY_CURRENT_USER, OG_REG_APP, L"SkipInstallPrompt", &skip)
        && skip)
        return 0;

    replacing = file_exists(U.install_path);
    choice = ask_to_install(U.exe_path, U.install_path, replacing, &never_again);

    if (choice == OG_STAY) {
        if (never_again) remember_not_to_ask();
        return 0;
    }

    if (choice == OG_USE_EXISTING) {
        lstrcpynW(launch, U.install_path, (int)n);
        return 1;
    }

    install_dir(dir, MAX_PATH);
    SHCreateDirectoryExW(NULL, dir, NULL);

    if (!CopyFileW(U.exe_path, U.install_path, FALSE)) {
        DWORD e = GetLastError();
        WCHAR msg[600];

        /* The installed copy holding its own file is the one failure worth
         * naming, because the way out of it is not obvious. */
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED) {
            wsprintfW(msg,
                      L"The installed copy of OLEDGuard is in use, so it "
                      L"cannot be replaced:\n\n%s\n\nQuit it from the "
                      L"notification area and open this file again. For now "
                      L"this copy will start the installed one instead.",
                      U.install_path);
            MessageBoxW(NULL, msg, L"OLEDGuard", MB_OK | MB_ICONINFORMATION);
            lstrcpynW(launch, U.install_path, (int)n);
            return 1;
        }

        wsprintfW(msg,
                  L"Could not copy OLEDGuard to\n\n%s\n\n"
                  L"Windows error %lu. It will keep running from where it is.",
                  U.install_path, (unsigned long)e);
        MessageBoxW(NULL, msg, L"OLEDGuard", MB_OK | MB_ICONWARNING);
        return 0;
    }

    make_start_menu_shortcut(U.install_path);

    /* A startup entry pointing at the old copy would now be wrong. */
    if (run_key_set(NULL)) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, OG_REG_RUN, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
            WCHAR quoted[MAX_PATH + 8];
            wsprintfW(quoted, L"\"%s\"", U.install_path);
            RegSetValueExW(k, OG_RUN_VALUE, 0, REG_SZ, (const BYTE *)quoted,
                           (DWORD)((lstrlenW(quoted) + 1) * sizeof(WCHAR)));
            RegCloseKey(k);
        }
    }

    lstrcpynW(launch, U.install_path, (int)n);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Status text                                                          */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Layout                                                               */
/* ------------------------------------------------------------------ */

static void place(int id, int x, int y, int w, int h)
{
    HWND c = item(id);
    if (c) SetWindowPos(c, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Combo boxes size their drop-down list through their own height, so they are
 * created tall and told what the closed control should measure separately. */
static void place_combo(int id, int x, int y, int w, int h, int items)
{
    HWND c = item(id);
    if (!c) return;
    SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, M.line + OG_SCALE(6));
    SendMessageW(c, CB_SETITEMHEIGHT, 0, M.line + OG_SCALE(6));
    SetWindowPos(c, NULL, x, y, w,
                 h + (M.line + OG_SCALE(6)) * items + OG_SCALE(4),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void relayout(void)
{
    int y, right_x, i, half;

    measure();
    EnumChildWindows(U.hwnd, set_font_child, 0);

    right_x = M.pad + M.label_w + M.gap;
    half = (M.ctl_h - M.line) / 2;
    y = M.pad;

    place(IDC_SUMMARY, M.pad, y, M.win_w - M.pad * 2, M.line);
    y += M.line + M.gap * 2;

    place(IDC_SECTION, M.pad, y, M.win_w - M.pad * 2, M.line);
    y += M.line + M.gap;

    U.monitors_top = y;
    for (i = 0; i < U.row_count; ++i) {
        place(IDC_MON_BASE + i * 2, M.pad, y, M.win_w - M.pad * 2, M.line);
        place(IDC_MON_BASE + i * 2 + 1, M.pad + M.indent, y + M.line,
              M.win_w - M.pad * 2 - M.indent, M.line);
        y += M.row_h;
    }
    if (U.row_count == 0) y += M.line;
    y += M.gap;

    place(IDC_L_TIMEOUT, M.pad, y + half, M.label_w, M.line);
    place_combo(IDC_TIMEOUT, right_x, y, M.field_w, M.ctl_h,
                TIMEOUT_PRESET_COUNT + 1);
    y += M.ctl_h + M.gap;

    place(IDC_L_WAKE, M.pad, y + half, M.label_w, M.line);
    place_combo(IDC_WAKE, right_x, y, M.field_w, M.ctl_h, 3);
    y += M.ctl_h + M.gap;

    place(IDC_L_INHIBIT, M.pad, y + half, M.label_w, M.line);
    place_combo(IDC_INHIBIT, right_x, y, M.field_w, M.ctl_h, 3);
    y += M.ctl_h + M.gap;

    {
        int val_w = OG_SCALE(52);
        place(IDC_L_DIM, M.pad, y + half, M.label_w, M.line);
        place(IDC_DIM, right_x, y, M.field_w - val_w - M.gap, M.ctl_h);
        place(IDC_DIMVAL, right_x + M.field_w - val_w, y + half, val_w, M.line);
    }
    y += M.ctl_h + M.gap * 2;

    place(IDC_LOGIN, M.pad, y, M.win_w - M.pad * 2, M.line);
    y += M.line;
    /* The style bit, not IsWindowVisible: the parent is still hidden the first
     * time through, which would make every child look invisible. */
    if (item(IDC_LOGINHINT) &&
        (GetWindowLongPtrW(item(IDC_LOGINHINT), GWL_STYLE) & WS_VISIBLE)) {
        place(IDC_LOGINHINT, M.pad + M.indent, y + M.gap / 2,
              M.win_w - M.pad * 2 - M.indent, M.line * 2);
        y += M.line * 2 + M.gap / 2;
    }
    y += M.gap * 2;

    {
        int bw = (M.win_w - M.pad * 2 - M.gap * 2) / 3;
        place(IDC_BLANK, M.pad, y, bw, M.ctl_h);
        place(IDC_PAUSE, M.pad + bw + M.gap, y, bw, M.ctl_h);
        place(IDC_HIDE, M.pad + (bw + M.gap) * 2, y, bw, M.ctl_h);
        y += M.ctl_h;
    }

    M.win_h = y + M.pad;
    InvalidateRect(U.hwnd, NULL, TRUE);
}

static void size_to_content(void)
{
    typedef BOOL (WINAPI *pfnAdjustForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static pfnAdjustForDpi f;
    static int tried;
    RECT rc;
    DWORD style = (DWORD)GetWindowLongPtrW(U.hwnd, GWL_STYLE);
    DWORD ex = (DWORD)GetWindowLongPtrW(U.hwnd, GWL_EXSTYLE);

    if (!tried) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) f = (pfnAdjustForDpi)(void *)
            GetProcAddress(u, "AdjustWindowRectExForDpi");
        tried = 1;
    }

    rc.left = 0; rc.top = 0; rc.right = M.win_w; rc.bottom = M.win_h;
    if (f) f(&rc, style, FALSE, ex, (UINT)M.dpi);
    else AdjustWindowRectEx(&rc, style, FALSE, ex);

    SetWindowPos(U.hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void rebuild_layout(void)
{
    M.dpi = dpi_of_window(U.hwnd);
    make_fonts(M.dpi);
    relayout();
    size_to_content();
}

/* ------------------------------------------------------------------ */

static void refresh_ui(void)
{
    int i, blanked = 0, rebuilt = 0;
    char line[256];
    WCHAR wline[256];

    if (!IsWindowVisible(U.hwnd)) return;
    if (!rows_match_core()) { build_monitor_rows(); rebuilt = 1; }

    for (i = 0; i < U.row_count; ++i) {
        og_core_status st;
        if (!og_core_status_at(U.core, i, &st)) continue;
        describe(line, sizeof line, &st);
        MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, 256);
        set_text(IDC_MON_BASE + i * 2 + 1, wline);
        U.suppress = 1;
        Button_SetCheck(item(IDC_MON_BASE + i * 2),
                        st.enabled ? BST_CHECKED : BST_UNCHECKED);
        U.suppress = 0;
        if (st.state == OG_ST_BLANK) ++blanked;
    }

    snprintf(line, sizeof line, "%s - %s - %d of %d screen%s blanked",
                og_plat_name(),
                og_core_paused(U.core) ? "paused" : "guarding",
                blanked, U.row_count, U.row_count == 1 ? "" : "s");
    MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, 256);
    set_text(IDC_SUMMARY, wline);
    set_text(IDC_PAUSE, og_core_paused(U.core) ? L"Resume" : L"Pause");

    wsprintfW(wline, L"%d%%", U.cfg.curtain_opacity);
    set_text(IDC_DIMVAL, wline);

    if (rebuilt) rebuild_layout();
}

/* ------------------------------------------------------------------ */
/* Settings controls                                                    */
/* ------------------------------------------------------------------ */

static void build_controls(void)
{
    HWND cb;
    int i;

    mk(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, IDC_SUMMARY, U.font);
    mk(L"STATIC", L"Screens to guard", SS_LEFTNOWORDWRAP, IDC_SECTION,
       U.font_bold);

    mk(L"STATIC", L"Blank after", SS_LEFTNOWORDWRAP, IDC_L_TIMEOUT, U.font);
    cb = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            IDC_TIMEOUT, U.font);
    for (i = 0; i < TIMEOUT_PRESET_COUNT; ++i) {
        WCHAR t[64];
        int s = kTimeoutPresets[i];
        if (s < 60) wsprintfW(t, L"%d seconds", s);
        else        wsprintfW(t, L"%d minute%s", s / 60, s == 60 ? L"" : L"s");
        ComboBox_AddString(cb, t);
        ComboBox_SetItemData(cb, i, (LPARAM)s);
        if (s == U.cfg.timeout_sec) ComboBox_SetCurSel(cb, i);
    }
    if (ComboBox_GetCurSel(cb) < 0) {
        WCHAR t[64];
        int idx;
        wsprintfW(t, L"%d seconds (from config file)", U.cfg.timeout_sec);
        idx = ComboBox_AddString(cb, t);
        ComboBox_SetItemData(cb, idx, (LPARAM)U.cfg.timeout_sec);
        ComboBox_SetCurSel(cb, idx);
    }

    mk(L"STATIC", L"Wake on", SS_LEFTNOWORDWRAP, IDC_L_WAKE, U.font);
    cb = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            IDC_WAKE, U.font);
    for (i = 0; i < 3; ++i) ComboBox_AddString(cb, kWakeItems[i]);
    ComboBox_SetCurSel(cb, (int)U.cfg.wake_mode);

    mk(L"STATIC", L"Apps keeping the display on", SS_LEFTNOWORDWRAP,
       IDC_L_INHIBIT, U.font);
    cb = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            IDC_INHIBIT, U.font);
    for (i = 0; i < 3; ++i) ComboBox_AddString(cb, kInhibitItems[i]);
    ComboBox_SetCurSel(cb, (int)U.cfg.inhibitor_policy);

    mk(L"STATIC", L"Curtain opacity", SS_LEFTNOWORDWRAP, IDC_L_DIM, U.font);
    {
        HWND t = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
                    IDC_DIM, U.font);
        SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
        SendMessageW(t, TBM_SETPOS, TRUE, U.cfg.curtain_opacity);
    }
    mk(L"STATIC", L"", SS_RIGHT, IDC_DIMVAL, U.font);

    mk(L"BUTTON", L"Open at login", BS_AUTOCHECKBOX | WS_TABSTOP,
       IDC_LOGIN, U.font);
    mk(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_EDITCONTROL, IDC_LOGINHINT, U.font);
    ShowWindow(item(IDC_LOGINHINT), SW_HIDE);

    mk(L"BUTTON", L"Blank now", BS_PUSHBUTTON | WS_TABSTOP, IDC_BLANK, U.font);
    mk(L"BUTTON", L"Pause", BS_PUSHBUTTON | WS_TABSTOP, IDC_PAUSE, U.font);
    mk(L"BUTTON", L"Hide", BS_PUSHBUTTON | WS_TABSTOP, IDC_HIDE, U.font);

    build_monitor_rows();
}

/* ------------------------------------------------------------------ */
/* Tray                                                                 */
/* ------------------------------------------------------------------ */

static void load_icons(void)
{
    U.icon_big = (HICON)LoadImageW(U.hinst, MAKEINTRESOURCEW(IDI_OLEDGUARD),
                                   IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                   GetSystemMetrics(SM_CYICON), 0);
    U.icon_small = (HICON)LoadImageW(U.hinst, MAKEINTRESOURCEW(IDI_OLEDGUARD),
                                     IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON), 0);
    if (!U.icon_big) U.icon_big = LoadIconW(NULL, IDI_APPLICATION);
    if (!U.icon_small) U.icon_small = U.icon_big;
}

static void tray_add(void)
{
    memset(&U.nid, 0, sizeof U.nid);
    U.nid.cbSize = sizeof U.nid;
    U.nid.hWnd = U.hwnd;
    U.nid.uID = 1;
    U.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    U.nid.uCallbackMessage = WM_OG_TRAY;
    U.nid.hIcon = U.icon_small;
    lstrcpyW(U.nid.szTip, L"OLEDGuard");
    Shell_NotifyIconW(NIM_ADD, &U.nid);
}

static void tray_remove(void) { Shell_NotifyIconW(NIM_DELETE, &U.nid); }

static void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT p;

    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings...");
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

static void centre_on_cursor_monitor(void)
{
    MONITORINFO mi;
    RECT rc;
    POINT p;

    GetCursorPos(&p);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY), &mi))
        return;
    GetWindowRect(U.hwnd, &rc);
    SetWindowPos(U.hwnd, NULL,
                 mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left -
                                   (rc.right - rc.left)) / 2,
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top -
                                  (rc.bottom - rc.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void show_settings(void)
{
    static int placed;

    og_core_wake_all(U.core);
    if (!placed) { centre_on_cursor_monitor(); placed = 1; }
    ShowWindow(U.hwnd, SW_SHOW);
    SetForegroundWindow(U.hwnd);
    refresh_ui();
    apply_login_item();
    rebuild_layout();
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
            WCHAR t[16];
            U.cfg.curtain_opacity = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
            wsprintfW(t, L"%d%%", U.cfg.curtain_opacity);
            set_text(IDC_DIMVAL, t);
            save_config();
        }
        return 0;

    /* The window moved to a monitor with a different scale factor. Windows
     * hands over the rectangle it wants; everything inside it is ours. */
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        M.dpi = (int)LOWORD(wp);
        make_fonts(M.dpi);
        relayout();
        /* Windows suggests a rectangle; take its position but keep our own
         * size, which the content at the new scale has just decided. */
        SetWindowPos(h, NULL, r->left, r->top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        size_to_content();
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (lp && lstrcmpiW((const WCHAR *)lp, L"ImmersiveColorSet") == 0) {
            apply_theme();
        } else if (wp == SPI_SETNONCLIENTMETRICS) {
            rebuild_layout();
        }
        return 0;

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, U.bg);
        return 1;
    }

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
        case IDC_LOGIN:
            if (!U.suppress) {
                U.cfg.open_at_login = Button_GetCheck((HWND)lp) == BST_CHECKED;
                apply_login_item();
                save_config();
                relayout();
                size_to_content();
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
        rebuild_layout();
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        int id = GetDlgCtrlID((HWND)lp);
        int dim = (id == IDC_LOGINHINT) ||
                  (id >= IDC_MON_BASE && ((id - IDC_MON_BASE) % 2) == 1);
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, dim ? col_dim() : col_text());
        SetBkColor((HDC)wp, col_back());
        return (LRESULT)U.bg;
    }

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

    (void)hPrev; (void)nShow;

    U.hinst = hInst;
    if (wcsstr(cmdline, L"--verbose")) og_log_set_level(OG_LOG_DEBUG);

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);

    /* One instance guards the screens; launching again just shows its window.
     *
     * This has to come first. Opening a second copy while one is already
     * running is the ordinary "where did the window go" case, not a reason to
     * offer anything: asking first meant the prompt appeared, the user said
     * yes, and the copy failed with a sharing violation because the running
     * instance holds its own executable open. */
    mutex = CreateMutexW(NULL, TRUE, OG_MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(OG_MAIN_CLASS, NULL);
        if (other) PostMessageW(other, WM_OG_SHOW, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    /* Now that this process is the only one, it is safe to offer. Handing over
     * means releasing the mutex before the other copy looks for it. */
    {
        WCHAR handover[MAX_PATH];
        if (offer_install(handover, MAX_PATH)) {
            if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
            if (handover[0])
                ShellExecuteW(NULL, L"open", handover, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
    }

    og_config_defaults(&U.cfg);
    if (!og_config_path(U.cfg_path, sizeof U.cfg_path)) return 1;
    og_config_load(&U.cfg, U.cfg_path);

    if (!og_plat_init()) return 1;

    apply_theme();
    load_icons();
    M.dpi = dpi_of_window(NULL);
    make_fonts(M.dpi);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;              /* painted in WM_ERASEBKGND */
    wc.lpszClassName = OG_MAIN_CLASS;
    wc.hIcon = U.icon_big;
    wc.hIconSm = U.icon_small;
    if (!RegisterClassExW(&wc)) return 1;

    U.hwnd = CreateWindowExW(0, OG_MAIN_CLASS, L"OLEDGuard",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                             NULL, NULL, hInst, NULL);
    if (!U.hwnd) return 1;

    SendMessageW(U.hwnd, WM_SETICON, ICON_BIG, (LPARAM)U.icon_big);
    SendMessageW(U.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)U.icon_small);
    dark_title_bar(U.hwnd, U.dark);

    U.core = og_core_create(&U.cfg);
    if (!U.core) { og_plat_shutdown(); return 1; }

    build_controls();
    apply_login_item();
    rebuild_layout();
    centre_on_cursor_monitor();
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
    if (U.font_bold && U.font_bold != U.font) DeleteObject(U.font_bold);
    if (U.font) DeleteObject(U.font);
    if (U.bg) DeleteObject(U.bg);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return (int)msg.wParam;
}
