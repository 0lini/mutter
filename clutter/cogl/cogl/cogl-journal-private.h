/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2007,2008,2009 Intel Corporation.
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

#ifndef __COGL_JOURNAL_PRIVATE_H
#define __COGL_JOURNAL_PRIVATE_H

#include "cogl-handle.h"

/* To improve batching of geometry when submitting vertices to OpenGL we
 * log the texture rectangles we want to draw to a journal, so when we
 * later flush the journal we aim to batch data, and gl draw calls. */
typedef struct _CoglJournalEntry
{
  CoglPipeline            *pipeline;
  int                      n_layers;
  CoglMatrix               model_view;
  /* XXX: These entries are pretty big now considering the padding in
   * CoglPipelineFlushOptions and CoglMatrix, so we might need to optimize this
   * later. */
} CoglJournalEntry;

void
_cogl_journal_log_quad (const float  *position,
                        CoglPipeline *pipeline,
                        int           n_layers,
                        guint32       fallback_layers,
                        GLuint        layer0_override_texture,
                        const CoglPipelineWrapModeOverrides *
                                      wrap_mode_overrides,
                        const float  *tex_coords,
                        unsigned int  tex_coords_len);

void
_cogl_journal_flush (void);

#endif /* __COGL_JOURNAL_PRIVATE_H */
