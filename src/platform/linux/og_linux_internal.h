/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* Internal glue between the Linux X11 backend and its GLib/GDBus helpers. */
#ifndef OG_LINUX_INTERNAL_H
#define OG_LINUX_INTERNAL_H

#include "oledguard/og_types.h"

void     og_linux_dbus_init(void);
void     og_linux_dbus_shutdown(void);

/* Cached, cheap to call every tick. Probes, in order, whatever answers:
 *   - systemd-logind ListInhibitors (system bus), looking for "idle"
 *   - org.gnome.SessionManager.IsInhibited(8)   (session bus)
 *   - org.freedesktop.PowerManagement.Inhibit.HasInhibit (session bus)
 * Returns og_inhibit_flags. */
unsigned og_linux_dbus_inhibit_flags(void);

#endif /* OG_LINUX_INTERNAL_H */
