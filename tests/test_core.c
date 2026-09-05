/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* End-to-end tests of the per-monitor state machine against a mock backend.
 *
 * These are the behaviours the tool exists for, written down as assertions:
 * blank a static screen, never blank one that is playing something, wake the
 * screen the pointer moved onto and only that one.
 */
#include "oledguard/og_core.h"
#include "oledguard/og_config.h"
#include "mock_platform.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); ++failures; \
    } \
} while (0)

static og_config cfg;
static og_core  *core;

static void setup(int timeout_sec)
{
    mock_reset();
    og_config_defaults(&cfg);
    cfg.timeout_sec = timeout_sec;
    cfg.sample_interval_ms = 500;
    cfg.fade_ms = 0;
    og_config_set_monitor_enabled(&cfg, "mock-0", 1);
    og_config_set_monitor_enabled(&cfg, "mock-1", 1);
    core = og_core_create(&cfg);
}

static void teardown(void)
{
    og_core_destroy(core);
    core = NULL;
}

/* Advance simulated time in tick-sized steps, driving the core each step. */
static void run_ms(long ms)
{
    long done = 0;
    while (done < ms) {
        mock_advance(OG_TICK_MS);
        og_core_tick(core);
        done += OG_TICK_MS;
    }
}

/* ------------------------------------------------------------------ */

static void test_blanks_when_idle(void)
{
    printf("blanks an idle screen\n");
    setup(10);

    run_ms(5000);
    CHECK(!mock_overlay_visible(0), "blanked before the timeout expired");

    run_ms(6000);
    CHECK(mock_overlay_visible(0), "did not blank after the timeout");
    CHECK(mock_overlay_visible(1), "second screen did not blank");
    CHECK(mock_overlay_opacity(0) == 100,
          "curtain should be fully opaque by default (%d)", mock_overlay_opacity(0));
    teardown();
}

static void test_disabled_monitor_never_blanks(void)
{
    printf("leaves unguarded screens alone\n");
    setup(10);
    og_config_set_monitor_enabled(&cfg, "mock-1", 0);

    run_ms(20000);
    CHECK(mock_overlay_visible(0), "guarded screen should have blanked");
    CHECK(!mock_overlay_visible(1), "unguarded screen was blanked");
    teardown();
}

static void test_pointer_wakes_only_its_own_screen(void)
{
    printf("pointer wakes only the screen it is on\n");
    setup(10);
    run_ms(20000);
    CHECK(mock_overlay_visible(0) && mock_overlay_visible(1), "both should be blanked");

    /* Move the pointer onto monitor 0. */
    mock_set_cursor(500, 400);
    run_ms(OG_TICK_MS * 2);
    CHECK(!mock_overlay_visible(0), "the screen under the pointer did not wake");
    CHECK(mock_overlay_visible(1), "the other screen woke too");

    /* And it wakes fast: one tick, not one timeout. */
    teardown();
}

static void test_pointer_wake_is_prompt(void)
{
    printf("wake latency is a single tick\n");
    setup(10);
    run_ms(20000);
    mock_set_cursor(2500, 700);       /* onto monitor 1 */
    mock_advance(OG_TICK_MS);
    og_core_tick(core);
    CHECK(!mock_overlay_visible(1), "wake took longer than one tick");
    teardown();
}

static void test_video_prevents_blanking(void)
{
    printf("fullscreen playback prevents blanking\n");
    setup(10);
    mock_set_content(0, MOCK_VIDEO);
    mock_set_fullscreen(0, 1);
    mock_set_content(1, MOCK_STATIC);

    run_ms(40000);   /* four timeouts' worth */
    CHECK(!mock_overlay_visible(0), "blanked a screen that was playing video");
    CHECK(mock_overlay_visible(1), "the static screen should still have blanked");
    teardown();
}

static void test_static_ui_still_blanks(void)
{
    printf("a chat window with a blinking caret still blanks\n");
    setup(10);
    mock_set_content(0, MOCK_SUBTLE);   /* Discord idle */
    mock_set_content(1, MOCK_SUBTLE);

    run_ms(20000);
    CHECK(mock_overlay_visible(0), "small UI animation defeated the blanker");
    CHECK(mock_overlay_visible(1), "small UI animation defeated the blanker");
    teardown();
}

static void test_windowed_video_prevents_blanking(void)
{
    printf("windowed playback also prevents blanking\n");
    setup(10);
    mock_set_content(0, MOCK_VIDEO);
    mock_set_fullscreen(0, 0);          /* not fullscreen: higher bar, still met */

    run_ms(30000);
    CHECK(!mock_overlay_visible(0), "blanked a screen playing windowed video");
    teardown();
}

static void test_paused_video_eventually_blanks(void)
{
    printf("paused playback releases the hold\n");
    setup(10);
    mock_set_content(0, MOCK_VIDEO);
    mock_set_fullscreen(0, 1);
    run_ms(20000);
    CHECK(!mock_overlay_visible(0), "should be held awake while playing");

    mock_set_content(0, MOCK_STATIC);   /* user hits pause */
    run_ms(20000);
    CHECK(mock_overlay_visible(0), "still awake long after playback stopped");
    teardown();
}

static void test_scroll_burst_does_not_hold_forever(void)
{
    printf("a scroll burst delays but does not cancel blanking\n");
    setup(10);
    mock_set_content(0, MOCK_SCROLL);
    run_ms(30000);
    CHECK(mock_overlay_visible(0), "a momentary burst of change blocked blanking");
    teardown();
}

static void test_inhibitor_holds_awake(void)
{
    printf("an OS display assertion holds the screen awake when trusted\n");
    setup(10);
    cfg.inhibitor_policy = OG_INHIBIT_ALWAYS;
    mock_set_inhibit(OG_INHIBIT_DISPLAY);
    run_ms(30000);
    CHECK(!mock_overlay_visible(0), "blanked despite a display assertion");

    mock_set_inhibit(OG_INHIBIT_NONE);
    run_ms(20000);
    CHECK(mock_overlay_visible(0), "did not blank once the assertion was dropped");
    teardown();
}

static void test_inhibitor_can_be_ignored(void)
{
    printf("assertions can be switched off\n");
    setup(10);
    cfg.inhibitor_policy = OG_INHIBIT_NEVER;
    mock_set_inhibit(OG_INHIBIT_DISPLAY);
    run_ms(20000);
    CHECK(mock_overlay_visible(0), "assertion honoured even though it was disabled");
    teardown();
}

/* The bug this guards against: caffeine-style utilities hold a display
 * assertion permanently. If that outranks the pixels, a completely static
 * screen never blanks - the app silently does nothing at all. */
static void test_permanent_assertion_does_not_block_blanking(void)
{
    og_core_status st;

    printf("a permanent assertion does not stop a static screen blanking\n");
    setup(10);
    mock_set_inhibit(OG_INHIBIT_DISPLAY);   /* held forever, as Amphetamine does */
    mock_set_content(0, MOCK_STATIC);

    run_ms(20000);
    CHECK(mock_overlay_visible(0),
          "a static screen never blanked while an app held a display assertion");

    /* ...and the UI must still be able to say the assertion is there. */
    og_core_status_at(core, 0, &st);
    CHECK(st.hold & OG_HOLD_INHIBIT_SEEN,
          "the ignored assertion was not reported to the UI");
    teardown();
}

/* An asynchronous backend returns nothing on the ticks where its capture is
 * still in flight. Reading a single empty tick as "capture is broken" handed
 * control back to a permanently held assertion, and the screen never blanked. */
static void test_intermittent_capture_is_not_a_failure(void)
{
    og_core_status st;

    printf("an async backend's empty ticks do not count as broken capture\n");
    setup(10);
    mock_set_capture_intermittent(1);
    mock_set_inhibit(OG_INHIBIT_DISPLAY);   /* a caffeine utility, as before */
    mock_set_content(0, MOCK_STATIC);

    run_ms(20000);
    CHECK(mock_overlay_visible(0),
          "a static screen never blanked because sampling looked broken");

    og_core_status_at(core, 0, &st);
    CHECK(!(st.hold & OG_HOLD_NO_CAPTURE),
          "intermittent delivery was reported as no capture at all");
    teardown();
}

/* And a genuinely playing screen must still be held awake through the gaps. */
static void test_intermittent_capture_still_sees_playback(void)
{
    printf("playback is still detected through an async backend's gaps\n");
    setup(10);
    mock_set_capture_intermittent(1);
    mock_set_content(0, MOCK_VIDEO);
    mock_set_fullscreen(0, 1);

    run_ms(30000);
    CHECK(!mock_overlay_visible(0), "blanked a screen that was playing video");
    teardown();
}

static void test_no_capture_falls_back_to_inhibitors(void)
{
    printf("without screen sampling, assertions carry the load\n");
    setup(10);
    mock_set_capture_available(0);
    mock_set_content(0, MOCK_VIDEO);
    mock_set_inhibit(OG_INHIBIT_DISPLAY);
    run_ms(20000);
    CHECK(!mock_overlay_visible(0), "blanked a playing video on a capture-less session");

    /* And the status must say so, rather than pretending all is well. */
    {
        og_core_status st;
        og_core_status_at(core, 0, &st);
        CHECK(st.hold & OG_HOLD_NO_CAPTURE, "missing capture was not reported");
    }
    teardown();
}

static void test_wake_mode_pointer_only(void)
{
    printf("pointer-only mode ignores the keyboard\n");
    setup(10);
    cfg.wake_mode = OG_WAKE_POINTER_ONLY;
    mock_set_cursor(500, 400);          /* pointer resting on monitor 0 */
    run_ms(20000);
    CHECK(mock_overlay_visible(0), "a resting pointer should not hold a screen awake");

    mock_input();                        /* a keypress */
    run_ms(OG_TICK_MS * 3);
    CHECK(mock_overlay_visible(0), "a keypress woke the screen in pointer-only mode");

    mock_set_cursor(510, 400);           /* an actual pointer move */
    run_ms(OG_TICK_MS * 2);
    CHECK(!mock_overlay_visible(0), "a pointer move failed to wake the screen");
    teardown();
}

static void test_wake_mode_any_input(void)
{
    printf("any-input mode wakes everything\n");
    setup(10);
    cfg.wake_mode = OG_WAKE_ANY_INPUT;
    run_ms(20000);
    CHECK(mock_overlay_visible(0) && mock_overlay_visible(1), "both should be blanked");

    mock_input();
    run_ms(OG_TICK_MS * 2);
    CHECK(!mock_overlay_visible(0) && !mock_overlay_visible(1),
          "a keypress should have woken every screen");
    teardown();
}

static void test_pause_and_blank_now(void)
{
    printf("manual pause and blank-now\n");
    setup(600);
    og_core_blank_now(core);
    CHECK(mock_overlay_visible(0), "blank now did nothing");

    og_core_wake_all(core);
    CHECK(!mock_overlay_visible(0), "wake all did nothing");

    og_core_set_paused(core, 1);
    og_core_blank_now(core);            /* blank-now also clears the pause */
    CHECK(!og_core_paused(core), "blank now should resume a paused guard");

    og_core_wake_all(core);
    og_core_set_paused(core, 1);
    run_ms(20000);
    CHECK(!mock_overlay_visible(0), "blanked while paused");
    teardown();
}

static void test_blank_now_survives_playback(void)
{
    printf("blank now overrides content detection\n");
    setup(600);
    mock_set_content(0, MOCK_VIDEO);
    mock_set_fullscreen(0, 1);
    run_ms(3000);
    og_core_blank_now(core);
    run_ms(3000);
    CHECK(mock_overlay_visible(0), "playback cancelled an explicit blank-now");

    /* ...but real input still wins. */
    mock_set_cursor(500, 400);
    run_ms(OG_TICK_MS * 2);
    CHECK(!mock_overlay_visible(0), "input did not override blank-now");
    teardown();
}

static void test_toggle_enables_live(void)
{
    printf("toggling a screen takes effect without a restart\n");
    setup(10);
    run_ms(20000);
    CHECK(mock_overlay_visible(1), "should be blanked");

    og_core_set_enabled(core, 1, 0);
    CHECK(!mock_overlay_visible(1), "unchecking a screen did not wake it");
    run_ms(20000);
    CHECK(!mock_overlay_visible(1), "it blanked again after being unchecked");

    og_core_set_enabled(core, 1, 1);
    run_ms(20000);
    CHECK(mock_overlay_visible(1), "rechecking a screen did not resume guarding");
    teardown();
}

static void test_config_roundtrip(void)
{
    og_config a, b;
    const char *path = "og_test_config.ini";

    printf("config round-trip\n");
    og_config_defaults(&a);
    a.timeout_sec = 321;
    a.curtain_opacity = 65;
    a.wake_mode = OG_WAKE_ANY_INPUT;
    a.motion_ratio_windowed = 0.0925;
    og_config_set_monitor_enabled(&a, "edid-0102030405060708090a", 1);
    og_config_set_monitor_enabled(&a, "conn-DP-2", 0);

    CHECK(og_config_save(&a, path), "save failed");
    CHECK(og_config_load(&b, path), "load failed");
    CHECK(b.timeout_sec == 321, "timeout not preserved (%d)", b.timeout_sec);
    CHECK(b.curtain_opacity == 65, "curtain opacity not preserved (%d)",
          b.curtain_opacity);
    CHECK(b.wake_mode == OG_WAKE_ANY_INPUT, "wake mode not preserved");
    CHECK(b.motion_ratio_windowed > 0.09 && b.motion_ratio_windowed < 0.095,
          "ratio not preserved (%f)", b.motion_ratio_windowed);
    CHECK(og_config_monitor_enabled(&b, "edid-0102030405060708090a") == 1,
          "per-monitor enable not preserved");
    CHECK(og_config_monitor_enabled(&b, "conn-DP-2") == 0,
          "per-monitor disable not preserved");
    CHECK(og_config_monitor_enabled(&b, "never-seen") == 0,
          "unknown monitor should fall back to default_enabled");
    remove(path);
}

int main(void)
{
    printf("== og_core ==\n");
    test_blanks_when_idle();
    test_disabled_monitor_never_blanks();
    test_pointer_wakes_only_its_own_screen();
    test_pointer_wake_is_prompt();
    test_video_prevents_blanking();
    test_static_ui_still_blanks();
    test_windowed_video_prevents_blanking();
    test_paused_video_eventually_blanks();
    test_scroll_burst_does_not_hold_forever();
    test_inhibitor_holds_awake();
    test_permanent_assertion_does_not_block_blanking();
    test_intermittent_capture_is_not_a_failure();
    test_intermittent_capture_still_sees_playback();
    test_inhibitor_can_be_ignored();
    test_no_capture_falls_back_to_inhibitors();
    test_wake_mode_pointer_only();
    test_wake_mode_any_input();
    test_pause_and_blank_now();
    test_blank_now_survives_playback();
    test_toggle_enables_live();
    test_config_roundtrip();

    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall core checks passed\n");
    return 0;
}
