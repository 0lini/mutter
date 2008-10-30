#if !defined(__COGL_H_INSIDE__) && !defined(CLUTTER_COMPILATION)
#error "Only <cogl/cogl.h> can be included directly."
#endif

#ifndef __COGL_OFFSCREEN_H__
#define __COGL_OFFSCREEN_H__

#include <cogl/cogl-types.h>

G_BEGIN_DECLS

/**
 * SECTION:cogl-offscreen
 * @short_description: Fuctions for creating and manipulating offscreen
 *   frame buffer objects
 *
 * COGL allows creating and operating on FBOs (Framebuffer Objects).
 */

/* Offscreen api */

/**
 * cogl_offscreen_new_to_texture:
 * @texhandle:
 *
 * Returns:
 */
CoglHandle      cogl_offscreen_new_to_texture (CoglHandle         texhandle);

/**
 * cogl_offscreen_new_multisample:
 * 
 *
 * Returns:
 */
CoglHandle      cogl_offscreen_new_multisample (void);

/**
 * cogl_offscreen_ref:
 * @handle:
 *
 * Returns:
 */
CoglHandle      cogl_offscreen_ref            (CoglHandle          handle);

/**
 * cogl_is_offscreen:
 * @handle: A CoglHandle
 *
 * Gets whether the given handle references an existing offscreen
 * buffer object.
 *
 * Returns: %TRUE if the handle references an offscreen buffer,
 *   %FALSE otherwise
 */
gboolean        cogl_is_offscreen             (CoglHandle          handle);

/**
 * cogl_offscreen_unref:
 * @handle:
 *
 */
void            cogl_offscreen_unref          (CoglHandle          handle);

/**
 * cogl_offscreen_blit:
 * @src_buffer:
 * @dst_buffer:
 *
 */
void            cogl_offscreen_blit           (CoglHandle          src_buffer,
                                               CoglHandle          dst_buffer);

/**
 * cogl_offscreen_blit_region:
 * @src_buffer:
 * @dst_buffer:
 * @src_x:
 * @src_y:
 * @src_w:
 * @src_h:
 * @dst_x:
 * @dst_y:
 * @dst_w:
 * @dst_h:
 *
 */
void            cogl_offscreen_blit_region    (CoglHandle          src_buffer,
                                               CoglHandle          dst_buffer,
                                               gint                src_x,
                                               gint                src_y,
                                               gint                src_w,
                                               gint                src_h,
                                               gint                dst_x,
                                               gint                dst_y,
                                               gint                dst_w,
                                               gint                dst_h);

/**
 * cogl_draw_buffer:
 * @target:
 * @offscreen:
 *
 */
void            cogl_draw_buffer              (CoglBufferTarget    target,
                                               CoglHandle          offscreen);

G_END_DECLS

#endif /* __COGL_OFFSCREEN_H__ */
