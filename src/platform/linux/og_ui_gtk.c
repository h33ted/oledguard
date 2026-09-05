/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_ui_gtk.c - GTK4 settings window and the Linux entry point.
 *
 * GTK4 dropped GtkStatusIcon and there is no portable tray on Linux, so the
 * single-instance behaviour of GtkApplication stands in for one: running the
 * binary a second time re-presents the settings window of the instance that
 * is already guarding your screens.
 */
#include "oledguard/og_core.h"
#include "oledguard/og_config.h"
#include "oledguard/og_platform.h"
#include "oledguard/og_log.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *row;
    GtkWidget *check;
    GtkWidget *status;
    char id[OG_ID_LEN];
} og_mon_row;

typedef struct {
    og_config  cfg;
    char       cfg_path[1024];
    og_core   *core;

    GtkApplication *app;
    GtkWidget *win;
    GtkWidget *mon_box;
    GtkWidget *summary;
    GtkWidget *pause_btn;

    og_mon_row rows[OG_MAX_MONITORS];
    int        row_count;

    guint tick_src, ui_src;
    int   suppress_signals;
} og_app;

static og_app A;

/* ------------------------------------------------------------------ */

static void save_config(void)
{
    if (!og_config_save(&A.cfg, A.cfg_path))
        og_log(OG_LOG_WARN, "could not write %s", A.cfg_path);
}

static gboolean tick_cb(gpointer u)
{
    (void)u;
    og_core_tick(A.core);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Monitor rows                                                         */
/* ------------------------------------------------------------------ */

static void on_monitor_toggled(GtkCheckButton *btn, gpointer u)
{
    int index = GPOINTER_TO_INT(u);
    if (A.suppress_signals) return;
    og_core_set_enabled(A.core, index, gtk_check_button_get_active(btn));
    save_config();
}

static int rows_match_core(void)
{
    int i, n = og_core_monitor_count(A.core);
    if (n != A.row_count) return 0;
    for (i = 0; i < n; ++i) {
        og_core_status st;
        if (!og_core_status_at(A.core, i, &st)) return 0;
        if (strcmp(st.mon->id, A.rows[i].id) != 0) return 0;
    }
    return 1;
}

static void rebuild_monitor_rows(void)
{
    int i, n;
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(A.mon_box)) != NULL)
        gtk_box_remove(GTK_BOX(A.mon_box), child);
    A.row_count = 0;

    n = og_core_monitor_count(A.core);
    for (i = 0; i < n && i < OG_MAX_MONITORS; ++i) {
        og_core_status st;
        GtkWidget *row, *vb;
        og_mon_row *r;

        if (!og_core_status_at(A.core, i, &st)) continue;
        r = &A.rows[A.row_count];
        memset(r, 0, sizeof(*r));
        g_strlcpy(r->id, st.mon->id, sizeof r->id);

        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 4);

        r->check = gtk_check_button_new();
        gtk_widget_set_valign(r->check, GTK_ALIGN_CENTER);
        A.suppress_signals = 1;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(r->check), st.enabled);
        A.suppress_signals = 0;
        g_signal_connect(r->check, "toggled",
                         G_CALLBACK(on_monitor_toggled), GINT_TO_POINTER(i));

        vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        {
            GtkWidget *title = gtk_label_new(st.mon->name);
            gtk_widget_set_halign(title, GTK_ALIGN_START);
            gtk_widget_add_css_class(title, "heading");
            gtk_box_append(GTK_BOX(vb), title);
        }
        r->status = gtk_label_new("");
        gtk_widget_set_halign(r->status, GTK_ALIGN_START);
        gtk_widget_add_css_class(r->status, "dim-label");
        gtk_box_append(GTK_BOX(vb), r->status);

        gtk_box_append(GTK_BOX(row), r->check);
        gtk_box_append(GTK_BOX(row), vb);
        gtk_box_append(GTK_BOX(A.mon_box), row);

        r->row = row;
        ++A.row_count;
    }
}

static void describe_hold(char *buf, size_t len, const og_core_status *st)
{
    if (st->hold & OG_HOLD_DISABLED) {
        snprintf(buf, len, "not guarded");
        return;
    }
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
        char who[128];
        if (og_plat_inhibitor_name(who, sizeof who))
            snprintf(buf, len, "awake - %s is holding the display on", who);
        else
            snprintf(buf, len, "awake - an application is holding the display on");
        return;
    }
    if (st->hold & OG_HOLD_INHIBIT_SEEN) {
        char who[128];
        if (!og_plat_inhibitor_name(who, sizeof who))
            snprintf(who, sizeof who, "an app");
        snprintf(buf, len,
                 "awake - idle %lds of %ds (%s asks to keep the display on; "
                 "ignored while the screen is being sampled)",
                 st->idle_ms / 1000, A.cfg.timeout_sec, who);
        return;
    }
    if (st->hold & OG_HOLD_NO_CAPTURE) {
        snprintf(buf, len, "awake - idle %lds (no screen sampling on this session)",
                 st->idle_ms / 1000);
        return;
    }
    snprintf(buf, len, "awake - idle %lds of %ds  (last change %.1f%%)",
             st->idle_ms / 1000, A.cfg.timeout_sec, st->motion_ratio * 100.0);
}

static gboolean refresh_ui_cb(gpointer u)
{
    int i, n, blanked = 0;
    char line[256];
    (void)u;

    if (!A.win || !gtk_widget_get_visible(A.win)) return G_SOURCE_CONTINUE;

    if (!rows_match_core()) rebuild_monitor_rows();

    n = A.row_count;
    for (i = 0; i < n; ++i) {
        og_core_status st;
        if (!og_core_status_at(A.core, i, &st)) continue;
        describe_hold(line, sizeof line, &st);
        gtk_label_set_text(GTK_LABEL(A.rows[i].status), line);
        A.suppress_signals = 1;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(A.rows[i].check), st.enabled);
        A.suppress_signals = 0;
        if (st.state == OG_ST_BLANK) ++blanked;
    }

    snprintf(line, sizeof line, "%s - %s - %d of %d screen%s blanked",
             og_plat_name(),
             og_core_paused(A.core) ? "paused" : "guarding",
             blanked, n, n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(A.summary), line);
    gtk_button_set_label(GTK_BUTTON(A.pause_btn),
                         og_core_paused(A.core) ? "Resume" : "Pause");
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Settings widgets                                                     */
/* ------------------------------------------------------------------ */

static void on_timeout_changed(GtkSpinButton *sb, gpointer u)
{
    (void)u;
    if (A.suppress_signals) return;
    A.cfg.timeout_sec = (int)(gtk_spin_button_get_value(sb) * 60.0 + 0.5);
    if (A.cfg.timeout_sec < 5) A.cfg.timeout_sec = 5;
    save_config();
}

static void on_wake_changed(GtkDropDown *dd, GParamSpec *p, gpointer u)
{
    (void)p; (void)u;
    if (A.suppress_signals) return;
    A.cfg.wake_mode = (og_wake_mode)gtk_drop_down_get_selected(dd);
    save_config();
}

static void on_inhibit_changed(GtkDropDown *dd, GParamSpec *p, gpointer u)
{
    (void)p; (void)u;
    if (A.suppress_signals) return;
    A.cfg.inhibitor_policy = (og_inhibitor_policy)gtk_drop_down_get_selected(dd);
    save_config();
}

static void on_opacity_changed(GtkRange *r, gpointer u)
{
    (void)u;
    if (A.suppress_signals) return;
    A.cfg.curtain_opacity = (int)gtk_range_get_value(r);
    save_config();
}

static void on_fade_changed(GtkSpinButton *sb, gpointer u)
{
    (void)u;
    if (A.suppress_signals) return;
    A.cfg.fade_ms = (int)gtk_spin_button_get_value(sb);
    save_config();
}

static void on_blank_now(GtkButton *b, gpointer u) { (void)b; (void)u; og_core_blank_now(A.core); }
static void on_pause(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    og_core_set_paused(A.core, !og_core_paused(A.core));
}
static void on_quit(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    og_core_wake_all(A.core);
    g_application_quit(G_APPLICATION(A.app));
}

static gboolean on_close_request(GtkWindow *w, gpointer u)
{
    (void)u;
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;   /* keep guarding in the background */
}

static GtkWidget *labelled(const char *text, GtkWidget *ctl)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *lab = gtk_label_new(text);
    gtk_widget_set_halign(lab, GTK_ALIGN_START);
    gtk_widget_set_hexpand(lab, TRUE);
    gtk_widget_set_valign(ctl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), lab);
    gtk_box_append(GTK_BOX(box), ctl);
    return box;
}

static GtkWidget *section(const char *title)
{
    GtkWidget *l = gtk_label_new(title);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_widget_add_css_class(l, "title-4");
    gtk_widget_set_margin_top(l, 8);
    return l;
}

static void build_window(void)
{
    GtkWidget *root, *scroll, *btns, *sb, *dd, *sw, *scale, *fade;
    static const char *const wake_labels[] = {
        "Pointer movement over that screen only",
        "Pointer movement, or typing while the pointer rests there",
        "Any keyboard or mouse activity wakes every screen",
        NULL
    };

    A.win = gtk_application_window_new(A.app);
    gtk_window_set_title(GTK_WINDOW(A.win), "OLEDGuard");
    gtk_window_set_default_size(GTK_WINDOW(A.win), 560, 640);
    g_signal_connect(A.win, "close-request", G_CALLBACK(on_close_request), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);

    A.summary = gtk_label_new("");
    gtk_widget_set_halign(A.summary, GTK_ALIGN_START);
    gtk_widget_add_css_class(A.summary, "dim-label");
    gtk_box_append(GTK_BOX(root), A.summary);

    gtk_box_append(GTK_BOX(root), section("Screens to guard"));
    A.mon_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), A.mon_box);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);

    gtk_box_append(GTK_BOX(root), section("Timing"));
    sb = gtk_spin_button_new_with_range(0.25, 240.0, 0.25);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(sb), 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sb), A.cfg.timeout_sec / 60.0);
    g_signal_connect(sb, "value-changed", G_CALLBACK(on_timeout_changed), NULL);
    gtk_box_append(GTK_BOX(root), labelled("Blank after (minutes)", sb));

    fade = gtk_spin_button_new_with_range(0, 5000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fade), A.cfg.fade_ms);
    g_signal_connect(fade, "value-changed", G_CALLBACK(on_fade_changed), NULL);
    gtk_box_append(GTK_BOX(root), labelled("Fade in (ms, needs a compositor)", fade));

    gtk_box_append(GTK_BOX(root), section("Behaviour"));
    dd = gtk_drop_down_new_from_strings(wake_labels);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), (guint)A.cfg.wake_mode);
    g_signal_connect(dd, "notify::selected", G_CALLBACK(on_wake_changed), NULL);
    gtk_box_append(GTK_BOX(root), labelled("Wake on", dd));

    {
        static const char *const inhibit_labels[] = {
            "Ignore them - what is on screen decides",
            "Trust them only when screen sampling is unavailable",
            "Always trust them",
            NULL
        };
        sw = gtk_drop_down_new_from_strings(inhibit_labels);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(sw),
                                   (guint)A.cfg.inhibitor_policy);
        g_signal_connect(sw, "notify::selected",
                         G_CALLBACK(on_inhibit_changed), NULL);
        gtk_box_append(GTK_BOX(root),
                       labelled("Apps asking to keep the display on", sw));
    }

    scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 10, 100, 5);
    gtk_range_set_value(GTK_RANGE(scale), A.cfg.curtain_opacity);
    gtk_widget_set_size_request(scale, 200, -1);
    gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
    g_signal_connect(scale, "value-changed", G_CALLBACK(on_opacity_changed), NULL);
    gtk_box_append(GTK_BOX(root),
                   labelled("Curtain opacity % (100 = pixels off)", scale));

    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(btns, 12);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    {
        GtkWidget *b1 = gtk_button_new_with_label("Blank now");
        GtkWidget *b3 = gtk_button_new_with_label("Quit");
        A.pause_btn = gtk_button_new_with_label("Pause");
        g_signal_connect(b1, "clicked", G_CALLBACK(on_blank_now), NULL);
        g_signal_connect(A.pause_btn, "clicked", G_CALLBACK(on_pause), NULL);
        g_signal_connect(b3, "clicked", G_CALLBACK(on_quit), NULL);
        gtk_box_append(GTK_BOX(btns), b1);
        gtk_box_append(GTK_BOX(btns), A.pause_btn);
        gtk_box_append(GTK_BOX(btns), b3);
    }
    gtk_box_append(GTK_BOX(root), btns);

    gtk_window_set_child(GTK_WINDOW(A.win), root);
    rebuild_monitor_rows();
}

/* ------------------------------------------------------------------ */

static void on_activate(GtkApplication *app, gpointer u)
{
    (void)u;
    if (!A.win) build_window();
    /* Seeing the settings window means the user is here. */
    og_core_wake_all(A.core);
    gtk_window_present(GTK_WINDOW(A.win));
}

static void on_startup(GtkApplication *app, gpointer u)
{
    (void)app; (void)u;
    A.tick_src = g_timeout_add(OG_TICK_MS, tick_cb, NULL);
    A.ui_src   = g_timeout_add(250, refresh_ui_cb, NULL);
    /* Keep running with no window on screen. */
    g_application_hold(G_APPLICATION(A.app));
}

static void on_shutdown(GtkApplication *app, gpointer u)
{
    (void)app; (void)u;
    og_core_wake_all(A.core);
/* Deliberately not saving here.
 *
 * Every setting is written the moment it changes, so a save on the way out
 * adds nothing - and it actively destroys work: edit the ini by hand while
 * the app is running, quit, and this would write the stale in-memory copy
 * straight back over the edit. Hand-editing then looks like it did nothing. */
}

int main(int argc, char **argv)
{
    int i, headless = 0, status;
    int gtk_argc = 1;
    char *gtk_argv[2];

    og_config_defaults(&A.cfg);
    og_log_set_level(OG_LOG_WARN);

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v"))
            og_log_set_level(OG_LOG_DEBUG);
        else if (!strcmp(argv[i], "--no-gui") || !strcmp(argv[i], "-b"))
            headless = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf(
"OLEDGuard - per-monitor OLED burn-in curtain\n\n"
"  oledguard              open the settings window and start guarding\n"
"  oledguard --no-gui     guard without a window (for autostart)\n"
"  oledguard --verbose    log every state change to stderr\n\n"
"Running the binary again while it is already guarding re-opens the\n"
"settings window of the running instance.\n\n"
"Configuration file: ");
            {
                char p[1024];
                og_config_path(p, sizeof p);
                printf("%s\n", p);
            }
            return 0;
        }
    }

    if (!og_config_path(A.cfg_path, sizeof A.cfg_path)) {
        fprintf(stderr, "oledguard: cannot determine a config directory\n");
        return 1;
    }
    og_config_load(&A.cfg, A.cfg_path);

    if (!og_plat_init()) return 1;

    A.core = og_core_create(&A.cfg);
    if (!A.core) { og_plat_shutdown(); return 1; }

    if (headless) {
        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        g_timeout_add(OG_TICK_MS, tick_cb, NULL);
        og_log(OG_LOG_WARN, "guarding %d display(s) headless; Ctrl-C to stop",
               og_core_monitor_count(A.core));
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
        og_core_wake_all(A.core);
        og_core_destroy(A.core);
        og_plat_shutdown();
        return 0;
    }

    A.app = gtk_application_new("io.oledguard.OLEDGuard", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(A.app, "startup",  G_CALLBACK(on_startup),  NULL);
    g_signal_connect(A.app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(A.app, "shutdown", G_CALLBACK(on_shutdown), NULL);

    gtk_argv[0] = argv[0];
    gtk_argv[1] = NULL;
    status = g_application_run(G_APPLICATION(A.app), gtk_argc, gtk_argv);

    og_core_destroy(A.core);
    og_plat_shutdown();
    g_object_unref(A.app);
    return status;
}
