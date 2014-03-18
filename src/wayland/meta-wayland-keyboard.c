/*
 * Wayland Support
 *
 * Copyright (C) 2013 Intel Corporation
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
 */

/*
 * Copyright © 2010-2011 Intel Corporation
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2012 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* The file is based on src/input.c from Weston */

#define _GNU_SOURCE

#include "config.h"

#include <glib.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <clutter/evdev/clutter-evdev.h>

#include "meta-wayland-private.h"

static int
create_anonymous_file (off_t size,
                       GError **error)
{
  static const char template[] = "mutter-shared-XXXXXX";
  char *path;
  int fd, flags;

  fd = g_file_open_tmp (template, &path, error);

  if (fd == -1)
    return -1;

  unlink (path);
  g_free (path);

  flags = fcntl (fd, F_GETFD);
  if (flags == -1)
    goto err;

  if (fcntl (fd, F_SETFD, flags | FD_CLOEXEC) == -1)
    goto err;

  if (ftruncate (fd, size) < 0)
    goto err;

  return fd;

 err:
  g_set_error_literal (error,
                       G_FILE_ERROR,
                       g_file_error_from_errno (errno),
                       strerror (errno));
  close (fd);

  return -1;
}

static void
inform_clients_of_new_keymap (MetaWaylandKeyboard *keyboard,
			      int                  flags)
{
  MetaWaylandCompositor *compositor;
  struct wl_client *xclient;
  struct wl_resource *keyboard_resource;

  compositor = meta_wayland_compositor_get_default ();
  xclient = compositor->xwayland_manager.client;

  wl_resource_for_each (keyboard_resource, &keyboard->resource_list)
    {
      if ((flags & META_WAYLAND_KEYBOARD_SKIP_XCLIENTS) &&
	  wl_resource_get_client (keyboard_resource) == xclient)
	continue;

      wl_keyboard_send_keymap (keyboard_resource,
			       WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
			       keyboard->xkb_info.keymap_fd,
			       keyboard->xkb_info.keymap_size);
    }
}

static void
meta_wayland_keyboard_take_keymap (MetaWaylandKeyboard *keyboard,
				   struct xkb_keymap   *keymap,
				   int                  flags)
{
  MetaWaylandXkbInfo  *xkb_info = &keyboard->xkb_info;
  GError *error = NULL;
  char *keymap_str;
  size_t previous_size;

  if (keymap == NULL)
    {
      g_warning ("Attempting to set null keymap (compilation probably failed)");
      return;
    }

  xkb_keymap_unref (xkb_info->keymap);
  xkb_info->keymap = keymap;

  xkb_state_unref (xkb_info->state);
  xkb_info->state = xkb_state_new (keymap);

  keymap_str = xkb_map_get_as_string (xkb_info->keymap);
  if (keymap_str == NULL)
    {
      g_warning ("failed to get string version of keymap");
      return;
    }
  previous_size = xkb_info->keymap_size;
  xkb_info->keymap_size = strlen (keymap_str) + 1;

  if (xkb_info->keymap_fd >= 0)
    close (xkb_info->keymap_fd);

  xkb_info->keymap_fd = create_anonymous_file (xkb_info->keymap_size, &error);
  if (xkb_info->keymap_fd < 0)
    {
      g_warning ("creating a keymap file for %lu bytes failed: %s",
                 (unsigned long) xkb_info->keymap_size,
                 error->message);
      g_clear_error (&error);
      goto err_keymap_str;
    }

  if (xkb_info->keymap_area)
    munmap (xkb_info->keymap_area, previous_size);

  xkb_info->keymap_area = mmap (NULL, xkb_info->keymap_size,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, xkb_info->keymap_fd, 0);
  if (xkb_info->keymap_area == MAP_FAILED)
    {
      g_warning ("failed to mmap() %lu bytes\n",
                 (unsigned long) xkb_info->keymap_size);
      goto err_dev_zero;
    }
  strcpy (xkb_info->keymap_area, keymap_str);
  free (keymap_str);

#if defined(CLUTTER_WINDOWING_EGL)
  /* XXX -- the evdev backend can be used regardless of the
   * windowing backend. To do this properly we need a Clutter
   * API to check the input backend. */
  if (clutter_check_windowing_backend (CLUTTER_WINDOWING_EGL))
    {
      ClutterDeviceManager *manager;
      manager = clutter_device_manager_get_default ();
      clutter_evdev_set_keyboard_map (manager, xkb_info->keymap);
    }
#endif

  inform_clients_of_new_keymap (keyboard, flags);

  return;

err_dev_zero:
  close (xkb_info->keymap_fd);
  xkb_info->keymap_fd = -1;
err_keymap_str:
  free (keymap_str);
  return;
}

static void
keyboard_handle_focus_surface_destroy (struct wl_listener *listener, void *data)
{
  MetaWaylandKeyboard *keyboard = wl_container_of (listener, keyboard, focus_surface_listener);

  keyboard->focus_surface = NULL;

  if (keyboard->focus_resource)
    {
      wl_list_remove (&keyboard->focus_resource_listener.link);
      keyboard->focus_resource = NULL;
    }
}

static void
keyboard_handle_focus_resource_destroy (struct wl_listener *listener, void *data)
{
  MetaWaylandKeyboard *keyboard = wl_container_of (listener, keyboard, focus_resource_listener);

  keyboard->focus_resource = NULL;
}

static gboolean
default_grab_key (MetaWaylandKeyboardGrab *grab,
                  uint32_t time, uint32_t key, uint32_t state)
{
  MetaWaylandKeyboard *keyboard = grab->keyboard;
  struct wl_resource *resource;
  uint32_t serial;

  resource = keyboard->focus_resource;
  if (resource)
    {
      struct wl_client *client = wl_resource_get_client (resource);
      struct wl_display *display = wl_client_get_display (client);
      serial = wl_display_next_serial (display);
      wl_keyboard_send_key (resource, serial, time, key, state);
    }

  return resource != NULL;
}

static struct wl_resource *
find_resource_for_surface (struct wl_list *list, MetaWaylandSurface *surface)
{
  struct wl_client *client;

  if (!surface)
    return NULL;

  if (!surface->resource)
    return NULL;

  client = wl_resource_get_client (surface->resource);

  return wl_resource_find_for_client (list, client);
}

static void
default_grab_modifiers (MetaWaylandKeyboardGrab *grab, uint32_t serial,
                        uint32_t mods_depressed, uint32_t mods_latched,
                        uint32_t mods_locked, uint32_t group)
{
  MetaWaylandKeyboard *keyboard = grab->keyboard;

  if (keyboard->focus_resource)
    {
      wl_keyboard_send_modifiers (keyboard->focus_resource, serial, mods_depressed,
                                  mods_latched, mods_locked, group);
    }
}

static const MetaWaylandKeyboardGrabInterface
  default_keyboard_grab_interface = {
  default_grab_key,
  default_grab_modifiers,
};

gboolean
meta_wayland_keyboard_init (MetaWaylandKeyboard *keyboard,
                            struct wl_display   *display)
{
  memset (keyboard, 0, sizeof *keyboard);
  keyboard->xkb_info.keymap_fd = -1;

  wl_list_init (&keyboard->resource_list);
  wl_array_init (&keyboard->keys);

  keyboard->focus_surface_listener.notify = keyboard_handle_focus_surface_destroy;
  keyboard->focus_resource_listener.notify = keyboard_handle_focus_resource_destroy;

  keyboard->default_grab.interface = &default_keyboard_grab_interface;
  keyboard->default_grab.keyboard = keyboard;
  keyboard->grab = &keyboard->default_grab;

  keyboard->display = display;

  keyboard->xkb_context = xkb_context_new (0 /* flags */);

  /* Compute a default until gnome-settings-daemon starts and sets
     the appropriate values
  */
  meta_wayland_keyboard_set_keymap_names (keyboard,
					  "evdev",
					  "pc105",
					  "us", "", "", 0);

  return TRUE;
}

static void
meta_wayland_xkb_info_destroy (MetaWaylandXkbInfo *xkb_info)
{
  xkb_keymap_unref (xkb_info->keymap);
  xkb_state_unref (xkb_info->state);

  if (xkb_info->keymap_area)
    munmap (xkb_info->keymap_area, xkb_info->keymap_size);
  if (xkb_info->keymap_fd >= 0)
    close (xkb_info->keymap_fd);
}

static void
update_pressed_keys (MetaWaylandKeyboard   *keyboard,
		     uint32_t               evdev_code,
		     gboolean               is_press)
{
  if (is_press)
    {
      uint32_t *end = (void *) ((char *) keyboard->keys.data +
                                keyboard->keys.size);
      uint32_t *k;

      /* Make sure we don't already have this key. */
      for (k = keyboard->keys.data; k < end; k++)
        if (*k == evdev_code)
          return;

      /* Otherwise add the key to the list of pressed keys */
      k = wl_array_add (&keyboard->keys, sizeof (*k));
      *k = evdev_code;
    }
  else
    {
      uint32_t *end = (void *) ((char *) keyboard->keys.data +
                                keyboard->keys.size);
      uint32_t *k;

      /* Remove the key from the array */
      for (k = keyboard->keys.data; k < end; k++)
        if (*k == evdev_code)
          {
            *k = *(end - 1);
            keyboard->keys.size -= sizeof (*k);
            return;
          }

      g_warning ("unexpected key release event for key 0x%x", evdev_code);
    }
}

static guint
evdev_code (const ClutterKeyEvent *event)
{
  /* clutter-xkb-utils.c adds a fixed offset of 8 to go into XKB's
   * range, so we do the reverse here. */
  return event->hardware_keycode - 8;
}

void
meta_wayland_keyboard_update (MetaWaylandKeyboard *keyboard,
                              const ClutterKeyEvent *event)
{
  MetaWaylandKeyboardGrab *grab = keyboard->grab;
  gboolean is_press = event->type == CLUTTER_KEY_PRESS;
  struct xkb_state *state = keyboard->xkb_info.state;
  enum xkb_state_component changed_state;

  update_pressed_keys (keyboard, evdev_code (event), is_press);

  changed_state = xkb_state_update_key (state,
                                        event->hardware_keycode,
                                        is_press ? XKB_KEY_DOWN : XKB_KEY_UP);
  if (changed_state == 0)
    return;

  grab->interface->modifiers (grab,
                              wl_display_next_serial (keyboard->display),
                              xkb_state_serialize_mods (state, XKB_STATE_MODS_DEPRESSED),
                              xkb_state_serialize_mods (state, XKB_STATE_MODS_LATCHED),
                              xkb_state_serialize_mods (state, XKB_STATE_MODS_LOCKED),
                              xkb_state_serialize_layout (state, XKB_STATE_LAYOUT_EFFECTIVE));
}

gboolean
meta_wayland_keyboard_handle_event (MetaWaylandKeyboard *keyboard,
                                    const ClutterKeyEvent *event)
{
  gboolean is_press = event->type == CLUTTER_KEY_PRESS;
  gboolean handled;

  /* Synthetic key events are for autorepeat. Ignore those, as
   * autorepeat in Wayland is done on the client side. */
  if (event->flags & CLUTTER_EVENT_FLAG_SYNTHETIC)
    return FALSE;

  meta_verbose ("Handling key %s event code %d\n",
		is_press ? "press" : "release",
		event->hardware_keycode);

  handled = keyboard->grab->interface->key (keyboard->grab,
					    event->time,
					    evdev_code (event),
					    is_press);

  if (handled)
    meta_verbose ("Sent event to wayland client\n");
  else
    meta_verbose ("No wayland surface is focused, continuing normal operation\n");

  return handled;
}

void
meta_wayland_keyboard_set_focus (MetaWaylandKeyboard *keyboard,
                                 MetaWaylandSurface *surface)
{
  if (keyboard->focus_surface == surface && keyboard->focus_resource != NULL)
    return;

  if (keyboard->focus_surface != NULL)
    {
      if (keyboard->focus_resource)
        {
          if (keyboard->focus_surface->resource)
            {
              struct wl_client *client = wl_resource_get_client (keyboard->focus_resource);
              struct wl_display *display = wl_client_get_display (client);
              uint32_t serial = wl_display_next_serial (display);
              wl_keyboard_send_leave (keyboard->focus_resource, serial, keyboard->focus_surface->resource);
            }

          wl_list_remove (&keyboard->focus_resource_listener.link);
          keyboard->focus_resource = NULL;
        }

      wl_list_remove (&keyboard->focus_surface_listener.link);
      keyboard->focus_surface = NULL;
    }

  if (surface != NULL)
    {
      keyboard->focus_surface = surface;
      wl_resource_add_destroy_listener (keyboard->focus_surface->resource, &keyboard->focus_surface_listener);

      keyboard->focus_resource = find_resource_for_surface (&keyboard->resource_list, surface);
      if (keyboard->focus_resource)
        {
          struct wl_client *client = wl_resource_get_client (keyboard->focus_resource);
          struct wl_display *display = wl_client_get_display (client);
          struct xkb_state *state = keyboard->xkb_info.state;
          uint32_t serial = wl_display_next_serial (display);

          wl_keyboard_send_modifiers (keyboard->focus_resource, serial,
                                      xkb_state_serialize_mods (state, XKB_STATE_MODS_DEPRESSED),
                                      xkb_state_serialize_mods (state, XKB_STATE_MODS_LATCHED),
                                      xkb_state_serialize_mods (state, XKB_STATE_MODS_LOCKED),
                                      xkb_state_serialize_layout (state, XKB_STATE_LAYOUT_EFFECTIVE));
          wl_keyboard_send_enter (keyboard->focus_resource, serial, keyboard->focus_surface->resource,
                                  &keyboard->keys);

          wl_resource_add_destroy_listener (keyboard->focus_resource, &keyboard->focus_resource_listener);
          keyboard->focus_serial = serial;
        }
    }
}

void
meta_wayland_keyboard_start_grab (MetaWaylandKeyboard *keyboard,
                                  MetaWaylandKeyboardGrab *grab)
{
  keyboard->grab = grab;
  grab->keyboard = keyboard;

  /* XXX focus? */
}

void
meta_wayland_keyboard_end_grab (MetaWaylandKeyboard *keyboard)
{
  keyboard->grab = &keyboard->default_grab;
}

void
meta_wayland_keyboard_release (MetaWaylandKeyboard *keyboard)
{
  meta_wayland_xkb_info_destroy (&keyboard->xkb_info);
  xkb_context_unref (keyboard->xkb_context);

  /* XXX: What about keyboard->resource_list? */
  wl_array_release (&keyboard->keys);
}

void
meta_wayland_keyboard_set_keymap_names (MetaWaylandKeyboard *keyboard,
					const char          *rules,
					const char          *model,
					const char          *layout,
					const char          *variant,
					const char          *options,
					int                  flags)
{
  struct xkb_rule_names xkb_names;

  xkb_names.rules = rules;
  xkb_names.model = model;
  xkb_names.layout = layout;
  xkb_names.variant = variant;
  xkb_names.options = options;

  meta_wayland_keyboard_take_keymap (keyboard,
				     xkb_keymap_new_from_names (keyboard->xkb_context,
								&xkb_names,
								0 /* flags */),
				     flags);
}

