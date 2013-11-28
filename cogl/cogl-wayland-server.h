/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2012 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#ifndef __COGL_WAYLAND_SERVER_H
#define __COGL_WAYLAND_SERVER_H

#include <wayland-server.h>

/* NB: this is a top-level header that can be included directly but we
 * want to be careful not to define __COGL_H_INSIDE__ when this is
 * included internally while building Cogl itself since
 * __COGL_H_INSIDE__ is used in headers to guard public vs private api
 * definitions
 */
#ifndef COGL_COMPILATION

/* Note: When building Cogl .gir we explicitly define
 * __COGL_H_INSIDE__ */
#ifndef __COGL_H_INSIDE__
#define __COGL_H_INSIDE__
#define __COGL_MUST_UNDEF_COGL_H_INSIDE__
#endif

#endif /* COGL_COMPILATION */

#include <cogl/cogl-context.h>
#include <cogl/cogl-texture-2d.h>

COGL_BEGIN_DECLS

/**
 * cogl_wayland_display_set_compositor_display:
 * @display: a #CoglDisplay
 * @wayland_display: A compositor's Wayland display pointer
 *
 * Informs Cogl of a compositor's Wayland display pointer. This
 * enables Cogl to register private wayland extensions required to
 * pass buffers between the clients and compositor.
 *
 * Since: 1.10
 * Stability: unstable
 */
void
cogl_wayland_display_set_compositor_display (CoglDisplay *display,
                                          struct wl_display *wayland_display);

/**
 * cogl_wayland_texture_2d_new_from_buffer:
 * @ctx: A #CoglContext
 * @buffer: A Wayland resource for a buffer
 * @error: A #CoglError for exceptions
 *
 * Uploads the @buffer referenced by the given Wayland resource to a
 * #CoglTexture2D. The buffer resource may refer to a wl_buffer or a
 * wl_shm_buffer.
 *
 * <note>The results are undefined for passing an invalid @buffer
 * pointer</note>
 * <note>It is undefined if future updates to @buffer outside the
 * control of Cogl will affect the allocated #CoglTexture2D. In some
 * cases the contents of the buffer are copied (such as shm buffers),
 * and in other cases the underlying storage is re-used directly (such
 * as drm buffers)</note>
 *
 * Returns: A newly allocated #CoglTexture2D, or if Cogl could not
 *          validate the @buffer in some way (perhaps because of
 *          an unsupported format) it will return %NULL and set
 *          @error.
 *
 * Since: 1.10
 * Stability: unstable
 */
CoglTexture2D *
cogl_wayland_texture_2d_new_from_buffer (CoglContext *ctx,
                                         struct wl_resource *buffer,
                                         CoglError **error);

COGL_END_DECLS

/* The gobject introspection scanner seems to parse public headers in
 * isolation which means we need to be extra careful about how we
 * define and undefine __COGL_H_INSIDE__ used to detect when internal
 * headers are incorrectly included by developers. In the gobject
 * introspection case we have to manually define __COGL_H_INSIDE__ as
 * a commandline argument for the scanner which means we must be
 * careful not to undefine it in a header...
 */
#ifdef __COGL_MUST_UNDEF_COGL_H_INSIDE__
#undef __COGL_H_INSIDE__
#undef __COGL_MUST_UNDEF_COGL_H_INSIDE__
#endif

#endif /* __COGL_WAYLAND_SERVER_H */
