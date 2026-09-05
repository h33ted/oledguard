/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* A fake backend so the state machine can be driven deterministically,
 * headless, on any platform. */
#ifndef OG_MOCK_PLATFORM_H
#define OG_MOCK_PLATFORM_H

#include "oledguard/og_types.h"

typedef enum {
    MOCK_STATIC = 0,  /* an unchanging desktop */
    MOCK_SUBTLE,      /* Discord idle: a caret, a clock, one animated emoji */
    MOCK_SCROLL,      /* a burst of change, then still again */
    MOCK_VIDEO        /* fullscreen playback: most of the frame changes */
} mock_content;

void mock_reset(void);
void mock_advance(long ms);          /* also advances the idle counter */
void mock_input(void);               /* a keypress or click happened now */
void mock_set_cursor(int x, int y);  /* moving the cursor implies input */
void mock_set_inhibit(unsigned flags);
void mock_set_content(int mon, mock_content c);
void mock_set_fullscreen(int mon, int on);
void mock_set_capture_available(int on);
/* Deliver a frame only on every other call, as an asynchronous backend does
 * while its capture is still in flight. */
void mock_set_capture_intermittent(int on);
int  mock_overlay_visible(int mon);
int  mock_overlay_opacity(int mon);

#endif
