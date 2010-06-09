/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2008,2009 Intel Corporation.
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
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 *
 * Authors:
 *   Robert Bragg <robert@linux.intel.com>
 */

#ifndef __COGL_MATERIAL_PRIVATE_H
#define __COGL_MATERIAL_PRIVATE_H

#include "cogl-material.h"
#include "cogl-matrix.h"
#include "cogl-handle.h"

#include <glib.h>

typedef struct _CoglMaterial	      CoglMaterial;
typedef struct _CoglMaterialLayer     CoglMaterialLayer;

typedef enum _CoglMaterialEqualFlags
{
  /* Return FALSE if any component of either material isn't set to its
   * default value. (Note: if the materials have corresponding flush
   * options indicating that e.g. the material color won't be flushed then
   * this will not assert a default color value.) */
  COGL_MATERIAL_EQUAL_FLAGS_ASSERT_ALL_DEFAULTS   = 1L<<0,

} CoglMaterialEqualFlags;

/* XXX: I don't think gtk-doc supports having private enums so these aren't
 * bundled in with CoglMaterialLayerFlags */
typedef enum _CoglMaterialLayerPrivFlags
{
  /* Ref: CoglMaterialLayerFlags
  COGL_MATERIAL_LAYER_FLAG_HAS_USER_MATRIX  = 1L<<0
  */
  COGL_MATERIAL_LAYER_FLAG_DIRTY            = 1L<<1,
  COGL_MATERIAL_LAYER_FLAG_DEFAULT_COMBINE  = 1L<<2
} CoglMaterialLayerPrivFlags;

/* For tracking the state of a layer that's been flushed to OpenGL */
typedef struct _CoglLayerInfo
{
  CoglHandle    handle;
  unsigned long flags;
  GLenum        gl_target;
  GLuint        gl_texture;
  gboolean      fallback;
  gboolean      disabled;
  gboolean      layer0_overridden;
} CoglLayerInfo;

struct _CoglMaterialLayer
{
  CoglHandleObject _parent;
  unsigned int	   index;   /*!< lowest index is blended first then others on
                              top */
  unsigned long    flags;
  CoglHandle       texture; /*!< The texture for this layer, or
                              COGL_INVALID_HANDLE for an empty layer */

  CoglMaterialFilter mag_filter;
  CoglMaterialFilter min_filter;

  CoglMaterialWrapMode wrap_mode_s;
  CoglMaterialWrapMode wrap_mode_t;
  CoglMaterialWrapMode wrap_mode_r;

  /* Determines how the color of individual texture fragments
   * are calculated. */
  GLint texture_combine_rgb_func;
  GLint texture_combine_rgb_src[3];
  GLint texture_combine_rgb_op[3];

  GLint texture_combine_alpha_func;
  GLint texture_combine_alpha_src[3];
  GLint texture_combine_alpha_op[3];

  GLfloat texture_combine_constant[4];

  /* TODO: Support purely GLSL based material layers */

  CoglMatrix matrix;
};

typedef enum _CoglMaterialFlags
{
  COGL_MATERIAL_FLAG_SHOWN_SAMPLER_WARNING  = 1L<<0,

  COGL_MATERIAL_FLAG_DEFAULT_COLOR          = 1L<<1,
  COGL_MATERIAL_FLAG_DEFAULT_GL_MATERIAL    = 1L<<2,
  COGL_MATERIAL_FLAG_DEFAULT_ALPHA_FUNC     = 1L<<3,
  COGL_MATERIAL_FLAG_ENABLE_BLEND	    = 1L<<4,
  COGL_MATERIAL_FLAG_DEFAULT_BLEND          = 1L<<5
} CoglMaterialFlags;

/* ARBfp1.0 program (Fog + ARB_texture_env_combine) */
typedef struct _CoglMaterialProgram
{
  GString *source;
  GLuint gl_program;

  gboolean *sampled;
} CoglMaterialProgram;

struct _CoglMaterial
{
  CoglHandleObject _parent;
  unsigned long    journal_ref_count;

  unsigned long    flags;

  /* If no lighting is enabled; this is the basic material color */
  GLubyte   unlit[4];

  /* Standard OpenGL lighting model attributes */
  GLfloat   ambient[4];
  GLfloat   diffuse[4];
  GLfloat   specular[4];
  GLfloat   emission[4];
  GLfloat   shininess;

  /* Determines what fragments are discarded based on their alpha */
  CoglMaterialAlphaFunc alpha_func;
  GLfloat		alpha_func_reference;

  /* Determines how this material is blended with other primitives */
#ifndef HAVE_COGL_GLES
  GLenum blend_equation_rgb;
  GLenum blend_equation_alpha;
  GLint blend_src_factor_alpha;
  GLint blend_dst_factor_alpha;
  GLfloat blend_constant[4];
#endif
  GLint blend_src_factor_rgb;
  GLint blend_dst_factor_rgb;

  CoglMaterialProgram program;

  GList	       *layers;
  unsigned int  n_layers;
};

/*
 * SECTION:cogl-material-internals
 * @short_description: Functions for creating custom primitives that make use
 *    of Cogl materials for filling.
 *
 * Normally you shouldn't need to use this API directly, but if you need to
 * developing a custom/specialised primitive - probably using raw OpenGL - then
 * this API aims to expose enough of the material internals to support being
 * able to fill your geometry according to a given Cogl material.
 */

/*
 * _cogl_material_init_default_material:
 *
 * This initializes the first material owned by the Cogl context. All
 * subsequently instantiated materials created via the cogl_material_new()
 * API will initially be a copy of this material.
 */
void
_cogl_material_init_default_material (void);

/*
 * cogl_material_get_cogl_enable_flags:
 * @material: A CoglMaterial object
 *
 * This determines what flags need to be passed to cogl_enable before this
 * material can be used. Normally you shouldn't need to use this function
 * directly since Cogl will do this internally, but if you are developing
 * custom primitives directly with OpenGL you may want to use this.
 *
 * Note: This API is hopfully just a stop-gap solution. Ideally cogl_enable
 * will be replaced.
 */
/* TODO: find a nicer solution! */
unsigned long
_cogl_material_get_cogl_enable_flags (CoglHandle handle);

/*
 * CoglMaterialLayerFlags:
 * @COGL_MATERIAL_LAYER_FLAG_USER_MATRIX: Means the user has supplied a
 *                                        custom texture matrix.
 */
typedef enum _CoglMaterialLayerFlags
{
  COGL_MATERIAL_LAYER_FLAG_HAS_USER_MATRIX	= 1L<<0
} CoglMaterialLayerFlags;
/* XXX: NB: if you add flags here you will need to update
 * CoglMaterialLayerPrivFlags!!! */

/*
 * cogl_material_layer_get_flags:
 * @layer_handle: A CoglMaterialLayer layer handle
 *
 * This lets you get a number of flag attributes about the layer.  Normally
 * you shouldn't need to use this function directly since Cogl will do this
 * internally, but if you are developing custom primitives directly with
 * OpenGL you may need this.
 */
unsigned long
_cogl_material_layer_get_flags (CoglHandle layer_handle);

/*
 * Ensures the mipmaps are available for the texture in the layer if
 * the filter settings would require it
 */
void
_cogl_material_layer_ensure_mipmaps (CoglHandle layer_handler);

/*
 * CoglMaterialFlushFlag:
 * @COGL_MATERIAL_FLUSH_FALLBACK_MASK: The fallback_layers member is set to
 *      a guint32 mask of the layers that can't be supported with the user
 *      supplied texture and need to be replaced with fallback textures. (1 =
 *      fallback, and the least significant bit = layer 0)
 * @COGL_MATERIAL_FLUSH_DISABLE_MASK: The disable_layers member is set to
 *      a guint32 mask of the layers that you want to completly disable
 *      texturing for (1 = fallback, and the least significant bit = layer 0)
 * @COGL_MATERIAL_FLUSH_LAYER0_OVERRIDE: The layer0_override_texture member is
 *      set to a GLuint OpenGL texture name to override the texture used for
 *      layer 0 of the material. This is intended for dealing with sliced
 *      textures where you will need to point to each of the texture slices in
 *      turn when drawing your geometry.  Passing a value of 0 is the same as
 *      not passing the option at all.
 * @COGL_MATERIAL_FLUSH_SKIP_GL_COLOR: When flushing the GL state for the
 *      material don't call glColor.
 * @COGL_MATERIAL_FLUSH_WRAP_MODE_OVERRIDES: Specifies that a bitmask
 *      of overrides for the wrap modes for some or all layers is
 *      given.
 */
typedef enum _CoglMaterialFlushFlag
{
  COGL_MATERIAL_FLUSH_FALLBACK_MASK       = 1L<<0,
  COGL_MATERIAL_FLUSH_DISABLE_MASK        = 1L<<1,
  COGL_MATERIAL_FLUSH_LAYER0_OVERRIDE     = 1L<<2,
  COGL_MATERIAL_FLUSH_SKIP_GL_COLOR       = 1L<<3,
  COGL_MATERIAL_FLUSH_WRAP_MODE_OVERRIDES = 1L<<4
} CoglMaterialFlushFlag;

/* These constants are used to fill in wrap_mode_overrides */
#define COGL_MATERIAL_WRAP_MODE_OVERRIDE_NONE            0 /* no override */
#define COGL_MATERIAL_WRAP_MODE_OVERRIDE_REPEAT          1
#define COGL_MATERIAL_WRAP_MODE_OVERRIDE_CLAMP_TO_EDGE   2
#define COGL_MATERIAL_WRAP_MODE_OVERRIDE_CLAMP_TO_BORDER 3

/* There can't be more than 32 layers because we need to fit a bitmask
   of the layers into a guint32 */
#define COGL_MATERIAL_MAX_LAYERS 32

typedef struct _CoglMaterialWrapModeOverrides
{
  struct
  {
    unsigned long s : 2;
    unsigned long t : 2;
    unsigned long r : 2;
  } values[COGL_MATERIAL_MAX_LAYERS];
} CoglMaterialWrapModeOverrides;

/*
 * CoglMaterialFlushOptions:
 *
 */
typedef struct _CoglMaterialFlushOptions
{
  CoglMaterialFlushFlag         flags;

  guint32                       fallback_layers;
  guint32                       disable_layers;
  GLuint                        layer0_override_texture;
  CoglMaterialWrapModeOverrides wrap_mode_overrides;
} CoglMaterialFlushOptions;

void
_cogl_material_get_colorubv (CoglHandle  handle,
                             guint8     *color);

void
_cogl_material_flush_gl_state (CoglHandle material,
                               CoglMaterialFlushOptions *options);

gboolean
_cogl_material_equal (CoglHandle material0_handle,
                      CoglMaterialFlushOptions *material0_flush_options,
                      CoglHandle material1_handle,
                      CoglMaterialFlushOptions *material1_flush_options);

CoglHandle
_cogl_material_journal_ref (CoglHandle material_handle);

void
_cogl_material_journal_unref (CoglHandle material_handle);

/* TODO: These should be made public once we add support for 3D
   textures in Cogl */
void
_cogl_material_set_layer_wrap_mode_r (CoglHandle material,
                                      int layer_index,
                                      CoglMaterialWrapMode mode);

CoglMaterialWrapMode
_cogl_material_layer_get_wrap_mode_r (CoglHandle layer);

#endif /* __COGL_MATERIAL_PRIVATE_H */

