/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
#ifndef OG_LOG_H
#define OG_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OG_LOG_ERROR = 0,
    OG_LOG_WARN  = 1,
    OG_LOG_INFO  = 2,
    OG_LOG_DEBUG = 3
} og_log_level;

/* Default level is OG_LOG_WARN; --verbose raises it to OG_LOG_DEBUG. */
void og_log_set_level(og_log_level lvl);
og_log_level og_log_get_level(void);

void og_log(og_log_level lvl, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#ifdef __cplusplus
}
#endif
#endif /* OG_LOG_H */
