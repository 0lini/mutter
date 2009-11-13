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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "cogl.h"

#include "cogl-internal.h"
#include "cogl-context.h"

#define COGL_CHECK_GL_VERSION(driver_major, driver_minor, \
                              target_major, target_minor) \
  ((driver_major) > (target_major) || \
   ((driver_major) == (target_major) && (driver_minor) >= (target_minor)))

typedef struct _CoglGLSymbolTableEntry
{
  const char *name;
  void *ptr;
} CoglGLSymbolTableEntry;

gboolean
cogl_check_extension (const gchar *name, const gchar *ext)
{
  gchar *end;
  gint name_len, n;

  if (name == NULL || ext == NULL)
    return FALSE;

  end = (gchar*)(ext + strlen(ext));

  name_len = strlen(name);

  while (ext < end)
    {
      n = strcspn(ext, " ");

      if ((name_len == n) && (!strncmp(name, ext, n)))
	return TRUE;
      ext += (n + 1);
    }

  return FALSE;
}

gboolean
_cogl_resolve_gl_symbols (CoglGLSymbolTableEntry *symbol_table,
                          const char *suffix)
{
  int i;
  gboolean status = TRUE;
  for (i = 0; symbol_table[i].name; i++)
    {
      char *full_name = g_strdup_printf ("%s%s", symbol_table[i].name, suffix);
      *((CoglFuncPtr *)symbol_table[i].ptr) = cogl_get_proc_address (full_name);
      g_free (full_name);
      if (!*((CoglFuncPtr *)symbol_table[i].ptr))
        {
          status = FALSE;
          break;
        }
    }
  return status;
}

#ifdef HAVE_CLUTTER_OSX
static gboolean
really_enable_npot (void)
{
  /* OSX backend + ATI Radeon X1600 + NPOT texture + GL_REPEAT seems to crash
   * http://bugzilla.openedhand.com/show_bug.cgi?id=929
   *
   * Temporary workaround until post 0.8 we rejig the features set up a
   * little to allow the backend to overide.
   */
  const char *gl_renderer;
  const char *env_string;

  /* Regardless of hardware, allow user to decide. */
  env_string = g_getenv ("COGL_ENABLE_NPOT");
  if (env_string != NULL)
    return env_string[0] == '1';

  gl_renderer = (char*)glGetString (GL_RENDERER);
  if (strstr (gl_renderer, "ATI Radeon X1600") != NULL)
    return FALSE;

  return TRUE;
}
#endif

static gboolean
_cogl_get_gl_version (int *major_out, int *minor_out)
{
  const char *version_string, *major_end, *minor_end;
  int major = 0, minor = 0;

  /* Get the OpenGL version number */
  if ((version_string = (const char *) glGetString (GL_VERSION)) == NULL)
    return FALSE;

  /* Extract the major number */
  for (major_end = version_string; *major_end >= '0'
	 && *major_end <= '9'; major_end++)
    major = (major * 10) + *major_end - '0';
  /* If there were no digits or the major number isn't followed by a
     dot then it is invalid */
  if (major_end == version_string || *major_end != '.')
    return FALSE;

  /* Extract the minor number */
  for (minor_end = major_end + 1; *minor_end >= '0'
	 && *minor_end <= '9'; minor_end++)
    minor = (minor * 10) + *minor_end - '0';
  /* If there were no digits or there is an unexpected character then
     it is invalid */
  if (minor_end == major_end + 1
      || (*minor_end && *minor_end != ' ' && *minor_end != '.'))
    return FALSE;

  *major_out = major;
  *minor_out = minor;

  return TRUE;
}

gboolean
_cogl_check_driver_valid (GError **error)
{
  int major, minor;

  if (!_cogl_get_gl_version (&major, &minor))
    {
      g_set_error (error,
                   COGL_DRIVER_ERROR,
                   COGL_DRIVER_ERROR_UNKNOWN_VERSION,
                   "The OpenGL version could not be determined");
      return FALSE;
    }

  /* OpenGL 1.2 is required */
  if (!COGL_CHECK_GL_VERSION (major, minor, 1, 2))
    {
      g_set_error (error,
                   COGL_DRIVER_ERROR,
                   COGL_DRIVER_ERROR_INVALID_VERSION,
                   "The OpenGL version of your driver (%i.%i) "
                   "is not compatible with Cogl",
                   major, minor);
      return FALSE;
    }

  return TRUE;
}

void
_cogl_features_init (void)
{
  CoglFeatureFlags  flags = 0;
  const gchar      *gl_extensions;
  GLint             max_clip_planes = 0;
  GLint             num_stencil_bits = 0;
  gboolean          fbo_ARB = FALSE;
  gboolean          fbo_EXT = FALSE;
  const char       *suffix;
  int               gl_major = 0, gl_minor = 0;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  _cogl_get_gl_version (&gl_major, &gl_minor);

  flags = COGL_FEATURE_TEXTURE_READ_PIXELS;

  gl_extensions = (const gchar*) glGetString (GL_EXTENSIONS);

  if (cogl_check_extension ("GL_ARB_texture_non_power_of_two", gl_extensions))
    {
#ifdef HAVE_CLUTTER_OSX
      if (really_enable_npot ())
#endif
        flags |= COGL_FEATURE_TEXTURE_NPOT;
    }

#ifdef GL_YCBCR_MESA
  if (cogl_check_extension ("GL_MESA_ycbcr_texture", gl_extensions))
    {
      flags |= COGL_FEATURE_TEXTURE_YUV;
    }
#endif

  if (cogl_check_extension ("GL_ARB_shader_objects", gl_extensions) &&
      cogl_check_extension ("GL_ARB_vertex_shader", gl_extensions) &&
      cogl_check_extension ("GL_ARB_fragment_shader", gl_extensions))
    {
      ctx->drv.pf_glCreateProgramObjectARB =
	(COGL_PFNGLCREATEPROGRAMOBJECTARBPROC)
	cogl_get_proc_address ("glCreateProgramObjectARB");

      ctx->drv.pf_glCreateShaderObjectARB =
	(COGL_PFNGLCREATESHADEROBJECTARBPROC)
	cogl_get_proc_address ("glCreateShaderObjectARB");

      ctx->drv.pf_glShaderSourceARB =
	(COGL_PFNGLSHADERSOURCEARBPROC)
	cogl_get_proc_address ("glShaderSourceARB");

      ctx->drv.pf_glCompileShaderARB =
	(COGL_PFNGLCOMPILESHADERARBPROC)
	cogl_get_proc_address ("glCompileShaderARB");

      ctx->drv.pf_glAttachObjectARB =
	(COGL_PFNGLATTACHOBJECTARBPROC)
	cogl_get_proc_address ("glAttachObjectARB");

      ctx->drv.pf_glLinkProgramARB =
	(COGL_PFNGLLINKPROGRAMARBPROC)
	cogl_get_proc_address ("glLinkProgramARB");

      ctx->drv.pf_glUseProgramObjectARB =
	(COGL_PFNGLUSEPROGRAMOBJECTARBPROC)
	cogl_get_proc_address ("glUseProgramObjectARB");

      ctx->drv.pf_glGetUniformLocationARB =
	(COGL_PFNGLGETUNIFORMLOCATIONARBPROC)
	cogl_get_proc_address ("glGetUniformLocationARB");

      ctx->drv.pf_glDeleteObjectARB =
	(COGL_PFNGLDELETEOBJECTARBPROC)
	cogl_get_proc_address ("glDeleteObjectARB");

      ctx->drv.pf_glGetInfoLogARB =
	(COGL_PFNGLGETINFOLOGARBPROC)
	cogl_get_proc_address ("glGetInfoLogARB");

      ctx->drv.pf_glGetObjectParameterivARB =
	(COGL_PFNGLGETOBJECTPARAMETERIVARBPROC)
	cogl_get_proc_address ("glGetObjectParameterivARB");

      ctx->drv.pf_glUniform1fARB =
	(COGL_PFNGLUNIFORM1FARBPROC)
	cogl_get_proc_address ("glUniform1fARB");

      ctx->drv.pf_glVertexAttribPointerARB =
	(COGL_PFNGLVERTEXATTRIBPOINTERARBPROC)
	cogl_get_proc_address ("glVertexAttribPointerARB");

      ctx->drv.pf_glEnableVertexAttribArrayARB =
	(COGL_PFNGLENABLEVERTEXATTRIBARRAYARBPROC)
	cogl_get_proc_address ("glEnableVertexAttribArrayARB");

      ctx->drv.pf_glDisableVertexAttribArrayARB =
	(COGL_PFNGLDISABLEVERTEXATTRIBARRAYARBPROC)
	cogl_get_proc_address ("glDisableVertexAttribArrayARB");

      ctx->drv.pf_glUniform2fARB =
	(COGL_PFNGLUNIFORM2FARBPROC)
	cogl_get_proc_address ("glUniform2fARB");

      ctx->drv.pf_glUniform3fARB =
	(COGL_PFNGLUNIFORM3FARBPROC)
	cogl_get_proc_address ("glUniform3fARB");

      ctx->drv.pf_glUniform4fARB =
	(COGL_PFNGLUNIFORM4FARBPROC)
	cogl_get_proc_address ("glUniform4fARB");

      ctx->drv.pf_glUniform1fvARB =
	(COGL_PFNGLUNIFORM1FVARBPROC)
	cogl_get_proc_address ("glUniform1fvARB");

      ctx->drv.pf_glUniform2fvARB =
	(COGL_PFNGLUNIFORM2FVARBPROC)
	cogl_get_proc_address ("glUniform2fvARB");

      ctx->drv.pf_glUniform3fvARB =
	(COGL_PFNGLUNIFORM3FVARBPROC)
	cogl_get_proc_address ("glUniform3fvARB");

      ctx->drv.pf_glUniform4fvARB =
	(COGL_PFNGLUNIFORM4FVARBPROC)
	cogl_get_proc_address ("glUniform4fvARB");

      ctx->drv.pf_glUniform1iARB =
	(COGL_PFNGLUNIFORM1IARBPROC)
	cogl_get_proc_address ("glUniform1iARB");

      ctx->drv.pf_glUniform2iARB =
	(COGL_PFNGLUNIFORM2IARBPROC)
	cogl_get_proc_address ("glUniform2iARB");

      ctx->drv.pf_glUniform3iARB =
	(COGL_PFNGLUNIFORM3IARBPROC)
	cogl_get_proc_address ("glUniform3iARB");

      ctx->drv.pf_glUniform4iARB =
	(COGL_PFNGLUNIFORM4IARBPROC)
	cogl_get_proc_address ("glUniform4iARB");

      ctx->drv.pf_glUniform1ivARB =
	(COGL_PFNGLUNIFORM1IVARBPROC)
	cogl_get_proc_address ("glUniform1ivARB");

      ctx->drv.pf_glUniform2ivARB =
	(COGL_PFNGLUNIFORM2IVARBPROC)
	cogl_get_proc_address ("glUniform2ivARB");

      ctx->drv.pf_glUniform3ivARB =
	(COGL_PFNGLUNIFORM3IVARBPROC)
	cogl_get_proc_address ("glUniform3ivARB");

      ctx->drv.pf_glUniform4ivARB =
	(COGL_PFNGLUNIFORM4IVARBPROC)
	cogl_get_proc_address ("glUniform4ivARB");

      ctx->drv.pf_glUniformMatrix2fvARB =
	(COGL_PFNGLUNIFORMMATRIX2FVARBPROC)
	cogl_get_proc_address ("glUniformMatrix2fvARB");

      ctx->drv.pf_glUniformMatrix3fvARB =
	(COGL_PFNGLUNIFORMMATRIX3FVARBPROC)
	cogl_get_proc_address ("glUniformMatrix3fvARB");

      ctx->drv.pf_glUniformMatrix4fvARB =
	(COGL_PFNGLUNIFORMMATRIX4FVARBPROC)
	cogl_get_proc_address ("glUniformMatrix4fvARB");

      if (ctx->drv.pf_glCreateProgramObjectARB    &&
	  ctx->drv.pf_glCreateShaderObjectARB     &&
	  ctx->drv.pf_glShaderSourceARB           &&
	  ctx->drv.pf_glCompileShaderARB          &&
	  ctx->drv.pf_glAttachObjectARB           &&
	  ctx->drv.pf_glLinkProgramARB            &&
	  ctx->drv.pf_glUseProgramObjectARB       &&
	  ctx->drv.pf_glGetUniformLocationARB     &&
	  ctx->drv.pf_glDeleteObjectARB           &&
	  ctx->drv.pf_glGetInfoLogARB             &&
	  ctx->drv.pf_glGetObjectParameterivARB   &&
	  ctx->drv.pf_glUniform1fARB              &&
	  ctx->drv.pf_glUniform2fARB              &&
	  ctx->drv.pf_glUniform3fARB              &&
	  ctx->drv.pf_glUniform4fARB              &&
	  ctx->drv.pf_glUniform1fvARB             &&
	  ctx->drv.pf_glUniform2fvARB             &&
	  ctx->drv.pf_glUniform3fvARB             &&
	  ctx->drv.pf_glUniform4fvARB             &&
	  ctx->drv.pf_glUniform1iARB              &&
	  ctx->drv.pf_glUniform2iARB              &&
	  ctx->drv.pf_glUniform3iARB              &&
	  ctx->drv.pf_glUniform4iARB              &&
	  ctx->drv.pf_glUniform1ivARB             &&
	  ctx->drv.pf_glUniform2ivARB             &&
	  ctx->drv.pf_glUniform3ivARB             &&
	  ctx->drv.pf_glUniform4ivARB             &&
	  ctx->drv.pf_glUniformMatrix2fvARB       &&
	  ctx->drv.pf_glUniformMatrix3fvARB       &&
	  ctx->drv.pf_glUniformMatrix4fvARB       &&
	  ctx->drv.pf_glVertexAttribPointerARB    &&
	  ctx->drv.pf_glEnableVertexAttribArrayARB &&
	  ctx->drv.pf_glDisableVertexAttribArrayARB)
	flags |= COGL_FEATURE_SHADERS_GLSL;
    }

  fbo_ARB = cogl_check_extension ("GL_ARB_framebuffer_object", gl_extensions);
  if (fbo_ARB)
    suffix = "";
  else
    {
      fbo_EXT = cogl_check_extension ("GL_EXT_framebuffer_object", gl_extensions);
      if (fbo_EXT)
        suffix = "EXT";
    }

  if (fbo_ARB || fbo_EXT)
    {
      CoglGLSymbolTableEntry symbol_table[] = {
            {"glGenRenderbuffers", &ctx->drv.pf_glGenRenderbuffers},
            {"glDeleteRenderbuffers", &ctx->drv.pf_glDeleteRenderbuffers},
            {"glBindRenderbuffer", &ctx->drv.pf_glBindRenderbuffer},
            {"glRenderbufferStorage", &ctx->drv.pf_glRenderbufferStorage},
            {"glGenFramebuffers", &ctx->drv.pf_glGenFramebuffers},
            {"glBindFramebuffer", &ctx->drv.pf_glBindFramebuffer},
            {"glFramebufferTexture2D", &ctx->drv.pf_glFramebufferTexture2D},
            {"glFramebufferRenderbuffer", &ctx->drv.pf_glFramebufferRenderbuffer},
            {"glCheckFramebufferStatus", &ctx->drv.pf_glCheckFramebufferStatus},
            {"glDeleteFramebuffers", &ctx->drv.pf_glDeleteFramebuffers},
            {"glGenerateMipmap", &ctx->drv.pf_glGenerateMipmap},
            {NULL, NULL}
      };

      if (_cogl_resolve_gl_symbols (symbol_table, suffix))
        flags |= COGL_FEATURE_OFFSCREEN;
    }

  if (cogl_check_extension ("GL_EXT_framebuffer_blit", gl_extensions))
    {
      ctx->drv.pf_glBlitFramebufferEXT =
	(COGL_PFNGLBLITFRAMEBUFFEREXTPROC)
	cogl_get_proc_address ("glBlitFramebufferEXT");

      if (ctx->drv.pf_glBlitFramebufferEXT)
	flags |= COGL_FEATURE_OFFSCREEN_BLIT;
    }

  if (cogl_check_extension ("GL_EXT_framebuffer_multisample", gl_extensions))
    {
      ctx->drv.pf_glRenderbufferStorageMultisampleEXT =
	(COGL_PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)
	cogl_get_proc_address ("glRenderbufferStorageMultisampleEXT");

      if (ctx->drv.pf_glRenderbufferStorageMultisampleEXT)
	flags |= COGL_FEATURE_OFFSCREEN_MULTISAMPLE;
    }

  GE( glGetIntegerv (GL_STENCIL_BITS, &num_stencil_bits) );
  /* We need at least three stencil bits to combine clips */
  if (num_stencil_bits > 2)
    flags |= COGL_FEATURE_STENCIL_BUFFER;

  GE( glGetIntegerv (GL_MAX_CLIP_PLANES, &max_clip_planes) );
  if (max_clip_planes >= 4)
    flags |= COGL_FEATURE_FOUR_CLIP_PLANES;

  if (cogl_check_extension ("GL_ARB_vertex_buffer_object", gl_extensions))
    {
      ctx->drv.pf_glGenBuffersARB =
	    (COGL_PFNGLGENBUFFERSARBPROC)
	    cogl_get_proc_address ("glGenBuffersARB");
      ctx->drv.pf_glBindBufferARB =
	    (COGL_PFNGLBINDBUFFERARBPROC)
	    cogl_get_proc_address ("glBindBufferARB");
      ctx->drv.pf_glBufferDataARB =
	    (COGL_PFNGLBUFFERDATAARBPROC)
	    cogl_get_proc_address ("glBufferDataARB");
      ctx->drv.pf_glBufferSubDataARB =
	    (COGL_PFNGLBUFFERSUBDATAARBPROC)
	    cogl_get_proc_address ("glBufferSubDataARB");
      ctx->drv.pf_glDeleteBuffersARB =
	    (COGL_PFNGLDELETEBUFFERSARBPROC)
	    cogl_get_proc_address ("glDeleteBuffersARB");
      ctx->drv.pf_glMapBufferARB =
	    (COGL_PFNGLMAPBUFFERARBPROC)
	    cogl_get_proc_address ("glMapBufferARB");
      ctx->drv.pf_glUnmapBufferARB =
	    (COGL_PFNGLUNMAPBUFFERARBPROC)
	    cogl_get_proc_address ("glUnmapBufferARB");
      if (ctx->drv.pf_glGenBuffersARB
	  && ctx->drv.pf_glBindBufferARB
	  && ctx->drv.pf_glBufferDataARB
	  && ctx->drv.pf_glBufferSubDataARB
	  && ctx->drv.pf_glDeleteBuffersARB
	  && ctx->drv.pf_glMapBufferARB
	  && ctx->drv.pf_glUnmapBufferARB)
      flags |= COGL_FEATURE_VBOS;
    }

  /* These should always be available because they are defined in GL
     1.2, but we can't call it directly because under Windows
     functions > 1.1 aren't exported */
  ctx->drv.pf_glDrawRangeElements =
        (COGL_PFNGLDRAWRANGEELEMENTSPROC)
        cogl_get_proc_address ("glDrawRangeElements");
  ctx->drv.pf_glActiveTexture =
        (COGL_PFNGLACTIVETEXTUREPROC)
        cogl_get_proc_address ("glActiveTexture");
  ctx->drv.pf_glClientActiveTexture =
        (COGL_PFNGLCLIENTACTIVETEXTUREPROC)
        cogl_get_proc_address ("glClientActiveTexture");

  ctx->drv.pf_glBlendEquation =
         (COGL_PFNGLBLENDEQUATIONPROC)
         cogl_get_proc_address ("glBlendEquation");
  ctx->drv.pf_glBlendColor =
         (COGL_PFNGLBLENDCOLORPROC)
         cogl_get_proc_address ("glBlendColor");

  /* Available in 1.4 */
  if (COGL_CHECK_GL_VERSION (gl_major, gl_minor, 1, 4))
    ctx->drv.pf_glBlendFuncSeparate =
      (COGL_PFNGLBLENDFUNCSEPARATEPROC)
      cogl_get_proc_address ("glBlendFuncSeparate");

  /* Available in 2.0 */
  if (COGL_CHECK_GL_VERSION (gl_major, gl_minor, 2, 0))
    ctx->drv.pf_glBlendEquationSeparate =
      (COGL_PFNGLBLENDEQUATIONSEPARATEPROC)
      cogl_get_proc_address ("glBlendEquationSeparate");

  /* Cache features */
  ctx->feature_flags = flags;
  ctx->features_cached = TRUE;
}
