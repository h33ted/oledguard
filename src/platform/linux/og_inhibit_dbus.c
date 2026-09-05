/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_inhibit_dbus.c - "is anything asking the screen to stay on?"
 *
 * This is the cheap half of the detector. A browser playing video, mpv, VLC,
 * Steam and most games all raise an idle inhibitor; Discord sitting idle and
 * a static web page do not. When it answers, it answers correctly and costs
 * nothing. When it does not (a player that never learned to inhibit), the
 * pixel sampler in the core is the backstop.
 *
 * Three mechanisms are probed because no single one is universal across
 * GNOME, KDE and the wlroots family. Results are cached for a second: the
 * core ticks 25 times a second and D-Bus round trips are not free.
 */
#include "og_linux_internal.h"
#include "oledguard/og_log.h"
#include "oledguard/og_platform.h"

#include <gio/gio.h>
#include <string.h>

#define OG_INHIBIT_CACHE_MS 1000
#define OG_DBUS_TIMEOUT_MS  250

static GDBusConnection *g_sys = NULL;
static GDBusConnection *g_ses = NULL;
static long     g_cache_ms = -1000000;
static unsigned g_cache_val = 0;
static char     g_cache_who[128] = "";

void og_linux_dbus_init(void)
{
    GError *err = NULL;

    g_sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!g_sys) {
        og_log(OG_LOG_DEBUG, "no system bus: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
    g_ses = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!g_ses) {
        og_log(OG_LOG_DEBUG, "no session bus: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
}

void og_linux_dbus_shutdown(void)
{
    g_clear_object(&g_sys);
    g_clear_object(&g_ses);
}

/* ------------------------------------------------------------------ */

/* systemd-logind knows about every inhibitor taken through it, including the
 * "idle" ones taken by media players and by GNOME/KDE on their behalf. */
static int logind_idle_inhibited(void)
{
    GVariant *reply, *arr;
    GVariantIter it;
    const gchar *what, *who, *why, *mode;
    guint32 uid, pid;
    int found = 0;

    if (!g_sys) return 0;

    reply = g_dbus_connection_call_sync(
        g_sys, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "ListInhibitors", NULL,
        G_VARIANT_TYPE("(a(ssssuu))"), G_DBUS_CALL_FLAGS_NONE,
        OG_DBUS_TIMEOUT_MS, NULL, NULL);
    if (!reply) return 0;

    arr = g_variant_get_child_value(reply, 0);
    g_variant_iter_init(&it, arr);
    while (g_variant_iter_next(&it, "(&s&s&s&su u)",
                               &what, &who, &why, &mode, &uid, &pid)) {
        /* `what` is a colon-separated list such as "idle:sleep". */
        if (what && strstr(what, "idle")) {
            og_log(OG_LOG_DEBUG, "logind idle inhibitor held by %s (%s)",
                   who ? who : "?", why ? why : "");
            if (who) {
                strncpy(g_cache_who, who, sizeof g_cache_who - 1);
                g_cache_who[sizeof g_cache_who - 1] = '\0';
            }
            found = 1;
            break;
        }
    }
    g_variant_unref(arr);
    g_variant_unref(reply);
    return found;
}

/* GNOME: flag 8 is INHIBIT_IDLE. */
static int gnome_idle_inhibited(void)
{
    GVariant *reply;
    gboolean v = FALSE;

    if (!g_ses) return 0;
    reply = g_dbus_connection_call_sync(
        g_ses, "org.gnome.SessionManager", "/org/gnome/SessionManager",
        "org.gnome.SessionManager", "IsInhibited",
        g_variant_new("(u)", (guint32)8),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
        OG_DBUS_TIMEOUT_MS, NULL, NULL);
    if (!reply) return 0;
    g_variant_get(reply, "(b)", &v);
    g_variant_unref(reply);
    return v ? 1 : 0;
}

/* KDE and anything else implementing the freedesktop power-management spec. */
static int fdo_power_inhibited(void)
{
    GVariant *reply;
    gboolean v = FALSE;

    if (!g_ses) return 0;
    reply = g_dbus_connection_call_sync(
        g_ses, "org.freedesktop.PowerManagement.Inhibit",
        "/org/freedesktop/PowerManagement/Inhibit",
        "org.freedesktop.PowerManagement.Inhibit", "HasInhibit", NULL,
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
        OG_DBUS_TIMEOUT_MS, NULL, NULL);
    if (!reply) return 0;
    g_variant_get(reply, "(b)", &v);
    g_variant_unref(reply);
    return v ? 1 : 0;
}

int og_plat_inhibitor_name(char *buf, size_t len)
{
    if (!buf || len == 0 || !g_cache_who[0]) return 0;
    strncpy(buf, g_cache_who, len - 1);
    buf[len - 1] = '\0';
    return 1;
}

unsigned og_linux_dbus_inhibit_flags(void)
{
    long now = og_plat_now_ms();
    unsigned flags = OG_INHIBIT_NONE;

    if (now - g_cache_ms < OG_INHIBIT_CACHE_MS) return g_cache_val;
    g_cache_ms = now;
    g_cache_who[0] = '\0';

    if (logind_idle_inhibited() || gnome_idle_inhibited() || fdo_power_inhibited())
        flags |= OG_INHIBIT_IDLE | OG_INHIBIT_DISPLAY;

    g_cache_val = flags;
    return flags;
}
