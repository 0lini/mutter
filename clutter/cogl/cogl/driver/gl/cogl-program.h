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
 */

#ifndef __COGL_PROGRAM_H
#define __COGL_PROGRAM_H

#include "cogl-handle.h"
#include "cogl-shader-private.h"

typedef struct _CoglProgram CoglProgram;

/* The ARBfp spec says at least 24 indices are available */
#define COGL_PROGRAM_MAX_ARBFP_LOCAL_PARAMS 24

struct _CoglProgram
{
  CoglHandleObject _parent;
  CoglShaderLanguage language;
  float arbfp_local_params[COGL_PROGRAM_MAX_ARBFP_LOCAL_PARAMS][4];
  GLuint gl_handle;
  gboolean is_linked;
};

CoglProgram *_cogl_program_pointer_from_handle (CoglHandle handle);

#endif /* __COGL_PROGRAM_H */
