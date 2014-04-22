/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2014 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

#include "config.h"

#include "meta-backend-x11.h"

#include <gdk/gdkx.h>
#include <clutter/x11/clutter-x11.h>

#include <meta/util.h>
#include "meta-idle-monitor-xsync.h"
#include "meta-monitor-manager-xrandr.h"
#include "backends/meta-monitor-manager-dummy.h"

G_DEFINE_TYPE (MetaBackendX11, meta_backend_x11, META_TYPE_BACKEND);

static MetaIdleMonitor *
meta_backend_x11_create_idle_monitor (MetaBackend *backend,
                                      int          device_id)
{
  return g_object_new (META_TYPE_IDLE_MONITOR_XSYNC,
                       "device-id", device_id,
                       NULL);
}

static MetaMonitorManager *
meta_backend_x11_create_monitor_manager (MetaBackend *backend)
{
  /* If we're a Wayland compositor using the X11 backend,
   * we're a nested configuration, so return the dummy
   * monitor setup. */
  if (meta_is_wayland_compositor ())
    return g_object_new (META_TYPE_MONITOR_MANAGER_DUMMY, NULL);

  return g_object_new (META_TYPE_MONITOR_MANAGER_XRANDR, NULL);
}

static void
meta_backend_x11_class_init (MetaBackendX11Class *klass)
{
  MetaBackendClass *backend_class = META_BACKEND_CLASS (klass);

  backend_class->create_idle_monitor = meta_backend_x11_create_idle_monitor;
  backend_class->create_monitor_manager = meta_backend_x11_create_monitor_manager;
}

static void
meta_backend_x11_init (MetaBackendX11 *x11)
{
  /* When running as an X11 compositor, we install our own event filter and
   * pass events to Clutter explicitly, so we need to prevent Clutter from
   * handling our events.
   *
   * However, when running as a Wayland compostior under X11 nested, Clutter
   * Clutter needs to see events related to its own window. We need to
   * eventually replace this with a proper frontend / backend split: Clutter
   * under nested is connecting to the "host X server" to get its events it
   * needs to put up a window, and GTK+ is connecting to the "inner X server".
   * The two would the same in the X11 compositor case, but not when running
   * XWayland as a Wayland compositor.
   */
  if (!meta_is_wayland_compositor ())
    {
      clutter_x11_set_display (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()));
      clutter_x11_disable_event_retrieval ();
    }
}

void
meta_backend_x11_handle_alarm_notify (MetaBackend *backend,
                                      XEvent      *xevent)
{
  int i;

  if (!META_IS_BACKEND_X11 (backend))
    return;

  for (i = 0; i <= backend->device_id_max; i++)
    if (backend->device_monitors[i])
      meta_idle_monitor_xsync_handle_xevent (backend->device_monitors[i], (XSyncAlarmNotifyEvent*)xevent);
}
