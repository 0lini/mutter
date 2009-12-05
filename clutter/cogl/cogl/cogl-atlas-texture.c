/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2009 Intel Corporation.
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *  Neil Roberts   <neil@linux.intel.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl.h"
#include "cogl-internal.h"
#include "cogl-util.h"
#include "cogl-texture-private.h"
#include "cogl-atlas-texture-private.h"
#include "cogl-texture-2d-private.h"
#include "cogl-context.h"
#include "cogl-handle.h"
#include "cogl-texture-driver.h"
#include "cogl-atlas.h"

#include <stdlib.h>

#ifdef HAVE_COGL_GLES2

#include "../gles/cogl-gles2-wrapper.h"

#else /* HAVE_COGL_GLES2 */

#define glGenFramebuffers                 ctx->drv.pf_glGenFramebuffers
#define glBindFramebuffer                 ctx->drv.pf_glBindFramebuffer
#define glFramebufferTexture2D            ctx->drv.pf_glFramebufferTexture2D
#define glCheckFramebufferStatus          ctx->drv.pf_glCheckFramebufferStatus
#define glDeleteFramebuffers              ctx->drv.pf_glDeleteFramebuffers

#endif /* HAVE_COGL_GLES2 */

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER		0x8D40
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING  0x8CA6
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0	0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

static void _cogl_atlas_texture_free (CoglAtlasTexture *sub_tex);

COGL_HANDLE_DEFINE (AtlasTexture, atlas_texture);

static const CoglTextureVtable cogl_atlas_texture_vtable;

/* If we want to do mulitple blits from a texture (such as when
   reorganizing the atlas) then it's quicker to download all of the
   data once and upload multiple times from that. This struct is used
   to keep the image data for a series of blits */

typedef struct _CoglAtlasTextureBlitData
{
  CoglHandle src_tex, dst_tex;

  /* If we're using an FBO to blit, then FBO will be non-zero and
     old_fbo will be the previous framebuffer binding */
  GLuint fbo, old_fbo;

  /* If we're not using an FBO then we g_malloc a buffer and copy the
     complete texture data in */
  unsigned char *image_data;
  CoglPixelFormat format;
  gint bpp;
  guint src_height, src_width;

  GLenum dst_gl_target;
} CoglAtlasTextureBlitData;

static void
_cogl_atlas_texture_blit_begin (CoglAtlasTextureBlitData *data,
                                CoglHandle dst_tex,
                                CoglHandle src_tex)
{
  GLenum src_gl_target;
  GLuint src_gl_texture;
  GLuint dst_gl_texture;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  data->dst_tex = dst_tex;
  data->src_tex = src_tex;
  data->fbo = 0;

  /* If we can use an FBO then we don't need to download the data and
     we can tell GL to blit directly between the textures */
  if (cogl_features_available (COGL_FEATURE_OFFSCREEN) &&
      !cogl_texture_is_sliced (dst_tex) &&
      cogl_texture_get_gl_texture (src_tex, &src_gl_texture, &src_gl_target) &&
      cogl_texture_get_gl_texture (dst_tex, &dst_gl_texture,
                                   &data->dst_gl_target))
    {
      /* Ideally we would use the cogl-offscreen API here, but I'd
         rather avoid creating a stencil renderbuffer which you can't
         currently do */
      /* Preserve the previous framebuffer binding so we don't trample
         on cogl-offscreen */
      data->old_fbo = 0;
      GE( glGetIntegerv (GL_FRAMEBUFFER_BINDING, (GLint *) &data->old_fbo) );

      _cogl_texture_set_filters (src_tex, GL_NEAREST, GL_NEAREST);

      /* Create an FBO to read from the src texture */
      GE( glGenFramebuffers (1, &data->fbo) );
      GE( glBindFramebuffer (GL_FRAMEBUFFER, data->fbo) );
      GE( glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  src_gl_target, src_gl_texture, 0) );
      if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
          /* The FBO failed for whatever reason so we'll fallback to
             reading the texture data */
          GE( glBindFramebuffer (GL_FRAMEBUFFER, data->old_fbo) );
          GE( glDeleteFramebuffers (1, &data->fbo) );
          data->fbo = 0;
        }

      GE( glBindTexture (data->dst_gl_target, dst_gl_texture) );
    }

  if (data->fbo)
    COGL_NOTE (ATLAS, "Blit set up using an FBO");
  else
    {
      /* We need to retrieve the entire texture data (there is no
         glGetTexSubImage2D) */

      data->format = cogl_texture_get_format (src_tex);
      data->bpp = _cogl_get_format_bpp (data->format);
      data->src_width = cogl_texture_get_width (src_tex);
      data->src_height = cogl_texture_get_height (src_tex);

      data->image_data = g_malloc (data->bpp * data->src_width *
                                   data->src_height);
      cogl_texture_get_data (src_tex, data->format,
                             data->src_width * data->bpp, data->image_data);
    }
}

static void
_cogl_atlas_texture_blit (CoglAtlasTextureBlitData *data,
                          guint src_x,
                          guint src_y,
                          guint dst_x,
                          guint dst_y,
                          guint width,
                          guint height)
{
  /* If we have an FBO then we can do a fast blit */
  if (data->fbo)
    GE( glCopyTexSubImage2D (data->dst_gl_target, 0, dst_x, dst_y, src_x, src_y,
                             width, height) );
  else
    cogl_texture_set_region (data->dst_tex,
                             src_x, src_y,
                             dst_x, dst_y,
                             width, height,
                             data->src_width, data->src_height,
                             data->format,
                             data->src_width * data->bpp,
                             data->image_data);
}

static void
_cogl_atlas_texture_blit_end (CoglAtlasTextureBlitData *data)
{
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  if (data->fbo)
    {
      GE( glBindFramebuffer (GL_FRAMEBUFFER, data->old_fbo) );
      GE( glDeleteFramebuffers (1, &data->fbo) );
    }
  else
    g_free (data->image_data);
}

static void
_cogl_atlas_texture_foreach_sub_texture_in_region (
                                       CoglTexture *tex,
                                       float virtual_tx_1,
                                       float virtual_ty_1,
                                       float virtual_tx_2,
                                       float virtual_ty_2,
                                       CoglTextureSliceCallback callback,
                                       void *user_data)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  _cogl_texture_foreach_sub_texture_in_region (atlas_tex->sub_texture,
                                               virtual_tx_1,
                                               virtual_ty_1,
                                               virtual_tx_2,
                                               virtual_ty_2,
                                               callback,
                                               user_data);
}

static void
_cogl_atlas_texture_set_wrap_mode_parameter (CoglTexture *tex,
                                             GLenum wrap_mode)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  _cogl_texture_set_wrap_mode_parameter (atlas_tex->sub_texture, wrap_mode);
}

static void
_cogl_atlas_texture_free (CoglAtlasTexture *atlas_tex)
{
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  /* Remove the texture from the atlas */
  if (atlas_tex->in_atlas)
    {
      CoglAtlas *atlas = ((atlas_tex->format & COGL_A_BIT) ?
                          ctx->atlas_alpha :
                          ctx->atlas_no_alpha);

      cogl_atlas_remove_rectangle (atlas, &atlas_tex->rectangle);

      COGL_NOTE (ATLAS, "Removed rectangle sized %ix%i",
                 atlas_tex->rectangle.width,
                 atlas_tex->rectangle.height);
      COGL_NOTE (ATLAS, "Atlas is %ix%i, has %i textures and is %i%% waste",
                 cogl_atlas_get_width (atlas),
                 cogl_atlas_get_height (atlas),
                 cogl_atlas_get_n_rectangles (atlas),
                 cogl_atlas_get_remaining_space (atlas) * 100 /
                 (cogl_atlas_get_width (atlas) *
                  cogl_atlas_get_height (atlas)));
    }

  cogl_handle_unref (atlas_tex->sub_texture);
}

static gint
_cogl_atlas_texture_get_max_waste (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return cogl_texture_get_max_waste (atlas_tex->sub_texture);
}

static gboolean
_cogl_atlas_texture_is_sliced (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return cogl_texture_is_sliced (atlas_tex->sub_texture);
}

static gboolean
_cogl_atlas_texture_can_hardware_repeat (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return _cogl_texture_can_hardware_repeat (atlas_tex->sub_texture);
}

static void
_cogl_atlas_texture_transform_coords_to_gl (CoglTexture *tex,
                                            float *s,
                                            float *t)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  _cogl_texture_transform_coords_to_gl (atlas_tex->sub_texture, s, t);
}

static gboolean
_cogl_atlas_texture_get_gl_texture (CoglTexture *tex,
                                    GLuint *out_gl_handle,
                                    GLenum *out_gl_target)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return cogl_texture_get_gl_texture (atlas_tex->sub_texture,
                                      out_gl_handle,
                                      out_gl_target);
}

static void
_cogl_atlas_texture_set_filters (CoglTexture *tex,
                                 GLenum min_filter,
                                 GLenum mag_filter)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  _cogl_texture_set_filters (atlas_tex->sub_texture, min_filter, mag_filter);
}

static void
_cogl_atlas_texture_ensure_mipmaps (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* FIXME: If mipmaps are required then we need to migrate the
     texture out of the atlas because it will show artifacts */

  /* Forward on to the sub texture */
  _cogl_texture_ensure_mipmaps (atlas_tex->sub_texture);
}

static gboolean
_cogl_atlas_texture_set_region (CoglTexture    *tex,
                                int             src_x,
                                int             src_y,
                                int             dst_x,
                                int             dst_y,
                                unsigned int    dst_width,
                                unsigned int    dst_height,
                                int             width,
                                int             height,
                                CoglPixelFormat format,
                                unsigned int    rowstride,
                                const guint8   *data)
{
  CoglAtlasTexture  *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* If the texture is in the atlas then we need to copy the edge
     pixels to the border */
  if (atlas_tex->in_atlas)
    {
      CoglHandle big_texture;

      _COGL_GET_CONTEXT (ctx, FALSE);

      big_texture = ((atlas_tex->format & COGL_A_BIT) ?
                     ctx->atlas_alpha_texture : ctx->atlas_no_alpha_texture);

      /* Copy the central data */
      if (!cogl_texture_set_region (big_texture,
                                    src_x, src_y,
                                    dst_x + atlas_tex->rectangle.x + 1,
                                    dst_y + atlas_tex->rectangle.y + 1,
                                    dst_width,
                                    dst_height,
                                    width, height,
                                    format,
                                    rowstride,
                                    data))
        return FALSE;

      /* Update the left edge pixels */
      if (dst_x == 0 &&
          !cogl_texture_set_region (big_texture,
                                    src_x, src_y,
                                    atlas_tex->rectangle.x,
                                    dst_y + atlas_tex->rectangle.y + 1,
                                    1, dst_height,
                                    width, height,
                                    format, rowstride,
                                    data))
        return FALSE;
      /* Update the right edge pixels */
      if (dst_x + dst_width == atlas_tex->rectangle.width - 2 &&
          !cogl_texture_set_region (big_texture,
                                    src_x + dst_width - 1, src_y,
                                    atlas_tex->rectangle.x +
                                    atlas_tex->rectangle.width - 1,
                                    dst_y + atlas_tex->rectangle.y + 1,
                                    1, dst_height,
                                    width, height,
                                    format, rowstride,
                                    data))
        return FALSE;
      /* Update the top edge pixels */
      if (dst_y == 0 &&
          !cogl_texture_set_region (big_texture,
                                    src_x, src_y,
                                    dst_x + atlas_tex->rectangle.x + 1,
                                    atlas_tex->rectangle.y,
                                    dst_width, 1,
                                    width, height,
                                    format, rowstride,
                                    data))
        return FALSE;
      /* Update the bottom edge pixels */
      if (dst_y + dst_height == atlas_tex->rectangle.height - 2 &&
          !cogl_texture_set_region (big_texture,
                                    src_x, src_y + dst_height - 1,
                                    dst_x + atlas_tex->rectangle.x + 1,
                                    atlas_tex->rectangle.y +
                                    atlas_tex->rectangle.height - 1,
                                    dst_width, 1,
                                    width, height,
                                    format, rowstride,
                                    data))
        return FALSE;

      return TRUE;
    }
  else
    /* Otherwise we can just forward on to the sub texture */
    return cogl_texture_set_region (atlas_tex->sub_texture,
                                    src_x, src_y,
                                    dst_x, dst_y,
                                    dst_width, dst_height,
                                    width, height,
                                    format, rowstride,
                                    data);
}

static int
_cogl_atlas_texture_get_data (CoglTexture     *tex,
                              CoglPixelFormat  format,
                              unsigned int     rowstride,
                              guint8          *data)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return cogl_texture_get_data (atlas_tex->sub_texture,
                                format,
                                rowstride,
                                data);
}

static CoglPixelFormat
_cogl_atlas_texture_get_format (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* We don't want to forward this on the sub-texture because it isn't
     the necessarily the same format. This will happen if the texture
     isn't pre-multiplied */
  return atlas_tex->format;
}

static GLenum
_cogl_atlas_texture_get_gl_format (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return _cogl_texture_get_gl_format (atlas_tex->sub_texture);
}

static gint
_cogl_atlas_texture_get_width (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return cogl_texture_get_width (atlas_tex->sub_texture);
}

static gint
_cogl_atlas_texture_get_height (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return cogl_texture_get_height (atlas_tex->sub_texture);
}

static CoglHandle
_cogl_atlas_texture_create_sub_texture (CoglHandle                full_texture,
                                        const CoglAtlasRectangle *rectangle)
{
  /* Create a subtexture for the given rectangle not including the
     1-pixel border */
  gfloat tex_width = cogl_texture_get_width (full_texture);
  gfloat tex_height = cogl_texture_get_height (full_texture);
  gfloat tx1 = (rectangle->x + 1) / tex_width;
  gfloat ty1 = (rectangle->y + 1) / tex_height;
  gfloat tx2 = (rectangle->x + rectangle->width - 1) / tex_width;
  gfloat ty2 = (rectangle->y + rectangle->height - 1) / tex_height;

  return cogl_texture_new_from_sub_texture (full_texture, tx1, ty1, tx2, ty2);
}

typedef struct _CoglAtlasTextureRepositionData
{
  /* The current texture which already has a position */
  CoglAtlasTexture *texture;
  /* The new position of the texture */
  CoglAtlasRectangle new_position;
} CoglAtlasTextureRepositionData;

static void
_cogl_atlas_texture_migrate (guint                           n_textures,
                             CoglAtlasTextureRepositionData *textures,
                             CoglHandle                      old_texture,
                             CoglHandle                      new_texture,
                             CoglAtlasTexture               *skip_texture)
{
  guint i;
  CoglAtlasTextureBlitData blit_data;

  _cogl_atlas_texture_blit_begin (&blit_data, new_texture, old_texture);

  for (i = 0; i < n_textures; i++)
    {
      /* Skip the texture that is being added because it doesn't contain
         any data yet */
      if (textures[i].texture != skip_texture)
        {
          _cogl_atlas_texture_blit (&blit_data,
                                    textures[i].texture->rectangle.x,
                                    textures[i].texture->rectangle.y,
                                    textures[i].new_position.x,
                                    textures[i].new_position.y,
                                    textures[i].new_position.width,
                                    textures[i].new_position.height);
          /* Update the sub texture */
          cogl_handle_unref (textures[i].texture->sub_texture);
          textures[i].texture->sub_texture =
            _cogl_atlas_texture_create_sub_texture (new_texture,
                                                    &textures[i].new_position);
        }

      /* Update the texture position */
      textures[i].texture->rectangle = textures[i].new_position;
    }

  _cogl_atlas_texture_blit_end (&blit_data);
}

typedef struct _CoglAtlasTextureGetRectanglesData
{
  CoglAtlasTextureRepositionData *textures;
  /* Number of textures found so far */
  guint n_textures;
} CoglAtlasTextureGetRectanglesData;

static void
_cogl_atlas_texture_get_rectangles_cb (const CoglAtlasRectangle *rectangle,
                                       gpointer                  rectangle_data,
                                       gpointer                  user_data)
{
  CoglAtlasTextureGetRectanglesData *data = user_data;

  data->textures[data->n_textures++].texture = rectangle_data;
}

static void
_cogl_atlas_texture_get_next_size (guint *atlas_width,
                                   guint *atlas_height)
{
  /* Double the size of the texture by increasing whichever dimension
     is smaller */
  if (*atlas_width < *atlas_height)
    *atlas_width <<= 1;
  else
    *atlas_height <<= 1;
}

static CoglAtlas *
_cogl_atlas_texture_create_atlas (guint                           atlas_width,
                                  guint                           atlas_height,
                                  guint                           n_textures,
                                  CoglAtlasTextureRepositionData *textures)
{
  GLint max_texture_size = 1024;

  GE( glGetIntegerv (GL_MAX_TEXTURE_SIZE, &max_texture_size) );

  /* Sanity check that we're not going to get stuck in an infinite
     loop if the maximum texture size has the high bit set */
  if ((max_texture_size & (1 << (sizeof (GLint) * 8 - 2))))
    max_texture_size >>= 1;

  /* Keep trying increasingly larger atlases until we can fit all of
     the textures */
  while (atlas_width < max_texture_size && atlas_height < max_texture_size)
    {
      CoglAtlas *new_atlas = cogl_atlas_new (atlas_width, atlas_height, NULL);
      guint i;

      /* Add all of the textures and keep track of the new position */
      for (i = 0; i < n_textures; i++)
        if (!cogl_atlas_add_rectangle (new_atlas,
                                       textures[i].texture->rectangle.width,
                                       textures[i].texture->rectangle.height,
                                       textures[i].texture,
                                       &textures[i].new_position))
          break;

      /* If the atlas can contain all of the textures then we have a
         winner */
      if (i >= n_textures)
        return new_atlas;

      cogl_atlas_free (new_atlas);
      _cogl_atlas_texture_get_next_size (&atlas_width, &atlas_height);
    }

  /* If we get here then there's no atlas that can accommodate all of
     the rectangles */

  return NULL;
}

static int
_cogl_atlas_texture_compare_size_cb (const void *a,
                                     const void *b)
{
  const CoglAtlasTextureRepositionData *ta = a;
  const CoglAtlasTextureRepositionData *tb = b;
  guint a_size, b_size;

  a_size = ta->texture->rectangle.width * ta->texture->rectangle.height;
  b_size = tb->texture->rectangle.width * tb->texture->rectangle.height;

  return a_size < b_size ? 1 : a_size > b_size ? -1 : 0;
}

static gboolean
_cogl_atlas_texture_reserve_space (CoglPixelFormat      format,
                                   CoglAtlas          **atlas_ptr,
                                   CoglHandle          *atlas_tex_ptr,
                                   CoglAtlasTexture    *new_sub_tex,
                                   guint                width,
                                   guint                height)
{
  CoglAtlasTextureGetRectanglesData data;
  CoglAtlas *new_atlas;
  CoglHandle new_tex;
  guint atlas_width, atlas_height;
  gboolean ret;

  _COGL_GET_CONTEXT (ctx, FALSE);

  /* Check if we can fit the rectangle into the existing atlas */
  if (*atlas_ptr && cogl_atlas_add_rectangle (*atlas_ptr, width, height,
                                              new_sub_tex,
                                              &new_sub_tex->rectangle))
    {
      COGL_NOTE (ATLAS, "Atlas is %ix%i, has %i textures and is %i%% waste",
                 cogl_atlas_get_width (*atlas_ptr),
                 cogl_atlas_get_height (*atlas_ptr),
                 cogl_atlas_get_n_rectangles (*atlas_ptr),
                 cogl_atlas_get_remaining_space (*atlas_ptr) * 100 /
                 (cogl_atlas_get_width (*atlas_ptr) *
                  cogl_atlas_get_height (*atlas_ptr)));
      return TRUE;
    }

  /* We need to reorganise the atlas so we'll get an array of all the
     textures currently in the atlas. */
  data.n_textures = 0;
  if (*atlas_ptr == NULL)
    data.textures = g_malloc (sizeof (CoglAtlasTextureRepositionData));
  else
    {
      data.textures = g_malloc (sizeof (CoglAtlasTextureRepositionData) *
                                (cogl_atlas_get_n_rectangles (*atlas_ptr) + 1));
      cogl_atlas_foreach (*atlas_ptr, _cogl_atlas_texture_get_rectangles_cb,
                          &data);
    }

  /* Add the new rectangle as a dummy texture so that it can be
     positioned with the rest */
  data.textures[data.n_textures++].texture = new_sub_tex;

  /* The atlasing algorithm works a lot better if the rectangles are
     added in decreasing order of size so we'll first sort the
     array */
  qsort (data.textures, data.n_textures,
         sizeof (CoglAtlasTextureRepositionData),
         _cogl_atlas_texture_compare_size_cb);

  /* Try to create a new atlas that can contain all of the textures */
  if (*atlas_ptr)
    {
      atlas_width = cogl_atlas_get_width (*atlas_ptr);
      atlas_height = cogl_atlas_get_height (*atlas_ptr);

      /* If there is enough space in the existing for the new
         rectangle in the existing atlas we'll start with the same
         size, otherwise we'll immediately double it */
      if (cogl_atlas_get_remaining_space (*atlas_ptr) < width * height)
        _cogl_atlas_texture_get_next_size (&atlas_width, &atlas_height);
    }
  else
    {
      /* Start with an initial size of 256x256 */
      atlas_width = 256;
      atlas_height = 256;
    }

  new_atlas = _cogl_atlas_texture_create_atlas (atlas_width, atlas_height,
                                                data.n_textures, data.textures);

  /* If we can't create an atlas with the texture then give up */
  if (new_atlas == NULL)
    {
      COGL_NOTE (ATLAS, "Could not fit texture in the atlas");
      ret = FALSE;
    }
  else
    {
      /* We need to migrate the existing textures into a new texture */
      new_tex =
        _cogl_texture_2d_new_with_size (cogl_atlas_get_width (new_atlas),
                                        cogl_atlas_get_height (new_atlas),
                                        COGL_TEXTURE_NONE,
                                        format);

      COGL_NOTE (ATLAS,
                 "Atlas %s with size %ix%i",
                 *atlas_ptr == NULL ||
                 cogl_atlas_get_width (*atlas_ptr) !=
                 cogl_atlas_get_width (new_atlas) ||
                 cogl_atlas_get_height (*atlas_ptr) !=
                 cogl_atlas_get_height (new_atlas) ?
                 "resized" : "reorganized",
                 cogl_atlas_get_width (new_atlas),
                 cogl_atlas_get_height (new_atlas));

      if (*atlas_ptr)
        {
          /* Move all the textures to the right position in the new
             texture. This will also update the texture's rectangle */
          _cogl_atlas_texture_migrate (data.n_textures,
                                       data.textures,
                                       *atlas_tex_ptr,
                                       new_tex,
                                       new_sub_tex);
          cogl_atlas_free (*atlas_ptr);
          cogl_handle_unref (*atlas_tex_ptr);
        }
      else
        /* We know there's only one texture so we can just directly
           update the rectangle from its new position */
        data.textures[0].texture->rectangle = data.textures[0].new_position;

      *atlas_ptr = new_atlas;
      *atlas_tex_ptr = new_tex;

      COGL_NOTE (ATLAS, "Atlas is %ix%i, has %i textures and is %i%% waste",
                 cogl_atlas_get_width (*atlas_ptr),
                 cogl_atlas_get_height (*atlas_ptr),
                 cogl_atlas_get_n_rectangles (*atlas_ptr),
                 cogl_atlas_get_remaining_space (*atlas_ptr) * 100 /
                 (cogl_atlas_get_width (*atlas_ptr) *
                  cogl_atlas_get_height (*atlas_ptr)));

      ret = TRUE;
    }

  g_free (data.textures);

  return ret;
}

CoglHandle
_cogl_atlas_texture_new_from_bitmap (CoglHandle       bmp_handle,
                                     CoglTextureFlags flags,
                                     CoglPixelFormat  internal_format)
{
  CoglAtlasTexture       *atlas_tex;
  CoglBitmap             *bmp = (CoglBitmap *) bmp_handle;
  CoglTextureUploadData   upload_data;
  CoglAtlas             **atlas_ptr;
  CoglHandle             *atlas_tex_ptr;

  _COGL_GET_CONTEXT (ctx, COGL_INVALID_HANDLE);

  g_return_val_if_fail (bmp_handle != COGL_INVALID_HANDLE, COGL_INVALID_HANDLE);

  /* We can't put the texture in the atlas if there are any special
     flags. This precludes textures with COGL_TEXTURE_NO_ATLAS and
     COGL_TEXTURE_NO_SLICING from being atlased */
  if (flags)
    return COGL_INVALID_HANDLE;

  /* We can't atlas zero-sized textures because it breaks the atlas
     data structure */
  if (bmp->width < 1 || bmp->height < 1)
    return COGL_INVALID_HANDLE;

  /* If we can't read back texture data then it will be too slow to
     migrate textures and we shouldn't use the atlas */
  if (!cogl_features_available (COGL_FEATURE_TEXTURE_READ_PIXELS))
    return COGL_INVALID_HANDLE;

  upload_data.bitmap = *bmp;
  upload_data.bitmap_owner = FALSE;

  if (!_cogl_texture_upload_data_prepare_format (&upload_data,
                                                 &internal_format))
    {
      _cogl_texture_upload_data_free (&upload_data);
      return COGL_INVALID_HANDLE;
    }

  COGL_NOTE (ATLAS, "Adding texture of size %ix%i", bmp->width, bmp->height);

  /* If the texture is in a strange format then we can't use it */
  if (internal_format != COGL_PIXEL_FORMAT_RGB_888 &&
      (internal_format & ~COGL_PREMULT_BIT) != COGL_PIXEL_FORMAT_RGBA_8888)
    {
      COGL_NOTE (ATLAS, "Texture can not be added because the "
                 "format is unsupported");

      _cogl_texture_upload_data_free (&upload_data);
      return COGL_INVALID_HANDLE;
    }

  if ((internal_format & COGL_A_BIT))
    {
      atlas_ptr = &ctx->atlas_alpha;
      atlas_tex_ptr = &ctx->atlas_alpha_texture;
    }
  else
    {
      atlas_ptr = &ctx->atlas_no_alpha;
      atlas_tex_ptr = &ctx->atlas_no_alpha_texture;
    }

  /* We need to allocate the texture now because we need the pointer
     to set as the data for the rectangle in the atlas */
  atlas_tex = g_new (CoglAtlasTexture, 1);
  /* We need to fill in the texture size now because it is used in the
     reserve_space function below. We add two pixels for the border */
  atlas_tex->rectangle.width = upload_data.bitmap.width + 2;
  atlas_tex->rectangle.height = upload_data.bitmap.height + 2;

  /* Try to make some space in the atlas for the texture */
  if (!_cogl_atlas_texture_reserve_space (internal_format,
                                          atlas_ptr,
                                          atlas_tex_ptr,
                                          atlas_tex,
                                          atlas_tex->rectangle.width,
                                          atlas_tex->rectangle.height))
    {
      g_free (atlas_tex);
      _cogl_texture_upload_data_free (&upload_data);
      return COGL_INVALID_HANDLE;
    }

  if (!_cogl_texture_upload_data_convert (&upload_data, internal_format))
    {
      cogl_atlas_remove_rectangle (*atlas_ptr, &atlas_tex->rectangle);
      g_free (atlas_tex);
      _cogl_texture_upload_data_free (&upload_data);
      return COGL_INVALID_HANDLE;
    }

  atlas_tex->_parent.vtable = &cogl_atlas_texture_vtable;
  atlas_tex->format = internal_format;
  atlas_tex->in_atlas = TRUE;
  atlas_tex->sub_texture =
    _cogl_atlas_texture_create_sub_texture (*atlas_tex_ptr,
                                            &atlas_tex->rectangle);

  /* Defer to set_region so that we can share the code for copying the
     edge pixels to the border */
  _cogl_atlas_texture_set_region (COGL_TEXTURE (atlas_tex),
                                  0, 0,
                                  0, 0,
                                  upload_data.bitmap.width,
                                  upload_data.bitmap.height,
                                  upload_data.bitmap.width,
                                  upload_data.bitmap.height,
                                  upload_data.bitmap.format,
                                  upload_data.bitmap.rowstride,
                                  upload_data.bitmap.data);

  return _cogl_atlas_texture_handle_new (atlas_tex);
}

static const CoglTextureVtable
cogl_atlas_texture_vtable =
  {
    _cogl_atlas_texture_set_region,
    _cogl_atlas_texture_get_data,
    _cogl_atlas_texture_foreach_sub_texture_in_region,
    _cogl_atlas_texture_get_max_waste,
    _cogl_atlas_texture_is_sliced,
    _cogl_atlas_texture_can_hardware_repeat,
    _cogl_atlas_texture_transform_coords_to_gl,
    _cogl_atlas_texture_get_gl_texture,
    _cogl_atlas_texture_set_filters,
    _cogl_atlas_texture_ensure_mipmaps,
    _cogl_atlas_texture_set_wrap_mode_parameter,
    _cogl_atlas_texture_get_format,
    _cogl_atlas_texture_get_gl_format,
    _cogl_atlas_texture_get_width,
    _cogl_atlas_texture_get_height
  };
