/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
#include "oledguard/og_log.h"
#include "oledguard/og_platform.h"

#include <stdarg.h>
#include <stdio.h>

static og_log_level g_level = OG_LOG_WARN;

void og_log_set_level(og_log_level lvl) { g_level = lvl; }
og_log_level og_log_get_level(void) { return g_level; }

void og_log(og_log_level lvl, const char *fmt, ...)
{
    static const char *tag[] = { "error", "warn ", "info ", "debug" };
    va_list ap;

    if (lvl > g_level) return;
    fprintf(stderr, "[oledguard %8ld %s] ", og_plat_now_ms(), tag[lvl]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
