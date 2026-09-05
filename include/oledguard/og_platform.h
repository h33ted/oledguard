/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_platform.h - the contract every backend implements.
 *
 * The core never includes a platform header; it only calls through here.
 * Exactly one og_plat_*.c/.m translation unit is linked per build.
 *
 * Threading: every function here is called from the UI thread only.
 */
#ifndef OG_PLATFORM_H
#define OG_PLATFORM_H

#include "oledguard/og_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle ---------------------------------------------------- */

int  og_plat_init(void);
void og_plat_shutdown(void);

/* Human-readable name of the active backend, for logs and the about box. */
const char *og_plat_name(void);

/* ---- displays ------------------------------------------------------ */

/* Fills up to `max` entries, returns the count. `native` and `overlay` of
 * returned entries are owned by the backend until og_plat_enum_monitors is
 * called again, which invalidates all previously returned handles. */
int og_plat_enum_monitors(og_monitor *out, int max);

/* True when the display topology changed since the last call (hotplug,
 * resolution change). The backend latches this from its own event stream. */
int og_plat_displays_changed(void);

/* ---- input --------------------------------------------------------- */

/* Pointer position in virtual-desktop coordinates. Returns 1 on success. */
int og_plat_cursor_pos(int *x, int *y);

/* Milliseconds since the last keyboard or pointer event anywhere in the
 * session. Returns -1 when the platform cannot report it. */
long og_plat_idle_ms(void);

/* ---- inhibition ---------------------------------------------------- */

/* Which OS-level "keep the screen on" assertions are currently held by any
 * process. This is the cheap, permission-free half of the detector: media
 * players and browsers assert while playing and release when paused. */
unsigned og_plat_inhibit_flags(void);

/* Names one process currently holding a display assertion, so the UI can say
 * "held by Amphetamine" rather than "an application". Returns 1 when it filled
 * `buf`. Backends that cannot attribute assertions return 0. */
int og_plat_inhibitor_name(char *buf, size_t len);

/* ---- content sampling ---------------------------------------------- */

/* True when a window is covering (approximately) the whole monitor. Used to
 * lower the motion threshold, because a fullscreen window plus any motion is
 * near-certain media playback. */
int og_plat_monitor_has_fullscreen(const og_monitor *m);

/* Capture a downsampled luma grid of the monitor into `sig`.
 * Returns 1 on success. Returns 0 when capture is unsupported or was
 * refused; the core then degrades to inhibitor-only detection rather than
 * blanking over a playing video. */
int og_plat_capture_signature(const og_monitor *m, og_signature *sig);

/* True when this backend can sample pixels at all in the current session.
 * Wayland without a portal grant, or macOS without Screen Recording
 * permission, both report 0. */
int og_plat_capture_available(void);

/* ---- overlays ------------------------------------------------------ */

/* Create (lazily) and show the blanking curtain covering m->bounds.
 * The curtain is always pure black; `opacity_pct` (10-100) decides how much
 * of the screen beneath shows through. 100 is fully opaque, which is the
 * only setting that genuinely turns OLED pixels off.
 * `fade_ms` of 0 means show immediately.
 *
 * Below 100 needs a compositor: X11 without one ignores the request and the
 * curtain stays fully opaque, which fails safe - more protection, not less. */
int  og_plat_overlay_show(og_monitor *m, int opacity_pct, int fade_ms);
void og_plat_overlay_hide(og_monitor *m);
void og_plat_overlay_destroy(og_monitor *m);

/* ---- misc ---------------------------------------------------------- */

/* Called once per core tick, before anything else. Backends use it to drain
 * their own event queue and to step overlay fades. Must not block. */
void og_plat_pump(void);

/* Monotonic milliseconds. Never wall-clock: the state machine must not
 * jump when the user's clock is corrected or DST changes. */
long og_plat_now_ms(void);

#ifdef __cplusplus
}
#endif
#endif /* OG_PLATFORM_H */
