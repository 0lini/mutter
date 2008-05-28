/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Authored By Matthew Allum  <mallum@openedhand.com>
 *
 * Copyright (C) 2008 OpenedHand
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

#ifndef PANGO_ENABLE_BACKEND
#define PANGO_ENABLE_BACKEND 1
#endif

#include <pango/pango-fontmap.h>
#include <pango/pangocairo.h>
#include <pango/pango-renderer.h>
#include <cairo/cairo.h>

#include "pangoclutter.h"
#include "pangoclutter-private.h"
#include "pangoclutter-glyph-cache.h"
#include "../clutter-debug.h"
#include "cogl/cogl.h"

struct _PangoClutterRenderer
{
  PangoRenderer parent_instance;

  /* The color to draw the glyphs with */
  ClutterColor color;

  /* Two caches of glyphs as textures, one with mipmapped textures and
     one without */
  PangoClutterGlyphCache *glyph_cache;
  PangoClutterGlyphCache *mipmapped_glyph_cache;

  gboolean use_mipmapping;
};

struct _PangoClutterRendererClass
{
  PangoRendererClass class_instance;
};

static void pango_clutter_renderer_finalize (GObject *object);
static void pango_clutter_renderer_draw_glyph (PangoRenderer *renderer,
					       PangoFont     *font,
					       PangoGlyph     glyph,
					       double         x,
					       double         y);
static void pango_clutter_renderer_draw_rectangle (PangoRenderer    *renderer,
						   PangoRenderPart   part,
						   int               x,
						   int               y,
						   int               width,
						   int               height);
static void pango_clutter_renderer_draw_trapezoid (PangoRenderer    *renderer,
						   PangoRenderPart   part,
						   double            y1,
						   double            x11,
						   double            x21,
						   double            y2,
						   double            x12,
						   double            x22);
static void pango_clutter_renderer_prepare_run (PangoRenderer  *renderer,
						PangoLayoutRun *run);

static GObjectClass *parent_class = NULL;

G_DEFINE_TYPE (PangoClutterRenderer, pango_clutter_renderer,
	       PANGO_TYPE_RENDERER);

static void
pango_clutter_renderer_init (PangoClutterRenderer *priv)
{
  priv->glyph_cache = pango_clutter_glyph_cache_new (FALSE);
  priv->mipmapped_glyph_cache = pango_clutter_glyph_cache_new (TRUE);
  priv->use_mipmapping = FALSE;
}

static void
pango_clutter_renderer_class_init (PangoClutterRendererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  PangoRendererClass *renderer_class = PANGO_RENDERER_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);

  object_class->finalize = pango_clutter_renderer_finalize;

  renderer_class->draw_glyph = pango_clutter_renderer_draw_glyph;
  renderer_class->draw_rectangle = pango_clutter_renderer_draw_rectangle;
  renderer_class->draw_trapezoid = pango_clutter_renderer_draw_trapezoid;
  renderer_class->prepare_run = pango_clutter_renderer_prepare_run;
}

static void
pango_clutter_renderer_finalize (GObject *object)
{
  PangoClutterRenderer *priv = PANGO_CLUTTER_RENDERER (object);

  pango_clutter_glyph_cache_free (priv->mipmapped_glyph_cache);
  pango_clutter_glyph_cache_free (priv->glyph_cache);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

void
pango_clutter_render_layout_subpixel (PangoLayout  *layout,
				      int           x,
				      int           y,
				      ClutterColor *color,
				      int           flags)
{
  PangoContext         *context;
  PangoFontMap         *font_map;
  PangoRenderer        *renderer;
  PangoClutterRenderer *priv;

  context = pango_layout_get_context (layout);
  font_map = pango_context_get_font_map (context);
  g_return_if_fail (PANGO_CLUTTER_IS_FONT_MAP (font_map));
  renderer = _pango_clutter_font_map_get_renderer
    (PANGO_CLUTTER_FONT_MAP (font_map));
  priv = PANGO_CLUTTER_RENDERER (renderer);

  priv->color = *color;

  pango_renderer_draw_layout (renderer, layout, x, y);
}

void
pango_clutter_render_layout (PangoLayout  *layout,
			     int           x,
			     int           y,
			     ClutterColor *color,
			     int           flags)
{
  return pango_clutter_render_layout_subpixel (layout,
					       x * PANGO_SCALE,
					       y * PANGO_SCALE,
					       color, flags);
}

void
pango_clutter_render_layout_line (PangoLayoutLine  *line,
				  int               x,
				  int               y,
				  ClutterColor     *color)
{
  PangoContext         *context;
  PangoFontMap         *font_map;
  PangoRenderer        *renderer;
  PangoClutterRenderer *priv;

  context = pango_layout_get_context (line->layout);
  font_map = pango_context_get_font_map (context);
  g_return_if_fail (PANGO_CLUTTER_IS_FONT_MAP (font_map));
  renderer = _pango_clutter_font_map_get_renderer
    (PANGO_CLUTTER_FONT_MAP (font_map));
  priv = PANGO_CLUTTER_RENDERER (renderer);

  priv->color = *color;

  pango_renderer_draw_layout_line (renderer, line, x, y);
}

void
_pango_clutter_renderer_clear_glyph_cache (PangoClutterRenderer *renderer)
{
  pango_clutter_glyph_cache_clear (renderer->glyph_cache);
  pango_clutter_glyph_cache_clear (renderer->mipmapped_glyph_cache);
}

void
_pango_clutter_renderer_set_use_mipmapping (PangoClutterRenderer *renderer,
					    gboolean value)
{
  renderer->use_mipmapping = value;
}

static PangoClutterGlyphCacheValue *
pango_clutter_renderer_get_cached_glyph (PangoRenderer *renderer,
					 PangoFont     *font,
					 PangoGlyph     glyph)
{
  PangoClutterRenderer *priv = PANGO_CLUTTER_RENDERER (renderer);
  PangoClutterGlyphCacheValue *value;
  PangoClutterGlyphCache *glyph_cache;

  glyph_cache = priv->use_mipmapping
    ? priv->mipmapped_glyph_cache : priv->glyph_cache;

  if ((value = pango_clutter_glyph_cache_lookup (glyph_cache,
						 font,
						 glyph)) == NULL)
    {
      cairo_surface_t *surface;
      cairo_t *cr;
      cairo_scaled_font_t *scaled_font;
      PangoRectangle ink_rect;
      cairo_glyph_t cairo_glyph;

      pango_font_get_glyph_extents (font, glyph, &ink_rect, NULL);
      pango_extents_to_pixels (&ink_rect, NULL);

      surface = cairo_image_surface_create (CAIRO_FORMAT_A8,
					    ink_rect.width,
					    ink_rect.height);
      cr = cairo_create (surface);

      scaled_font = pango_cairo_font_get_scaled_font (PANGO_CAIRO_FONT (font));
      cairo_set_scaled_font (cr, scaled_font);

      cairo_glyph.x = -ink_rect.x;
      cairo_glyph.y = -ink_rect.y;
      /* The PangoCairo glyph numbers directly map to Cairo glyph
	 numbers */
      cairo_glyph.index = glyph;
      cairo_show_glyphs (cr, &cairo_glyph, 1);

      cairo_destroy (cr);
      cairo_surface_flush (surface);

      /* Copy the glyph to the cache */
      value = pango_clutter_glyph_cache_set
	(glyph_cache, font, glyph,
	 cairo_image_surface_get_data (surface),
	 cairo_image_surface_get_width (surface),
	 cairo_image_surface_get_height (surface),
	 cairo_image_surface_get_stride (surface),
	 ink_rect.x, ink_rect.y);

      cairo_surface_destroy (surface);

      CLUTTER_NOTE (PANGO, "cache fail    %i", glyph);
    }
  else
    CLUTTER_NOTE (PANGO, "cache success %i", glyph);

  return value;
}

void
pango_clutter_ensure_glyph_cache_for_layout (PangoLayout *layout)
{
  PangoContext    *context;
  PangoFontMap    *fontmap;
  PangoRenderer   *renderer;
  PangoLayoutIter *iter;
 
  g_return_if_fail (PANGO_IS_LAYOUT (layout));
 
  context = pango_layout_get_context (layout);
  fontmap = pango_context_get_font_map (context);
  g_return_if_fail (PANGO_CLUTTER_IS_FONT_MAP (fontmap));
  renderer = _pango_clutter_font_map_get_renderer
    (PANGO_CLUTTER_FONT_MAP (fontmap));
 
  if ((iter = pango_layout_get_iter (layout)) == NULL)
    return;
 
  do
    {
      PangoLayoutLine *line;
      GSList *l;
 
      line = pango_layout_iter_get_line_readonly (iter);
 
      for (l = line->runs; l; l = l->next)
        {
          PangoLayoutRun *run = l->data;
          PangoGlyphString *glyphs = run->glyphs;
	  int i;
 
          for (i = 0; i < glyphs->num_glyphs; i++)
            {
              PangoGlyphInfo *gi = &glyphs->glyphs[i];
 
	      pango_clutter_renderer_get_cached_glyph (renderer,
						       run->item->analysis.font,
						       gi->glyph);
            }
        }
    }
  while (pango_layout_iter_next_line (iter));
 
  pango_layout_iter_free (iter);
}

static void
pango_clutter_renderer_draw_box (int x,     int y,
				 int width, int height)
{
  cogl_path_rectangle (CLUTTER_INT_TO_FIXED (x),
		       CLUTTER_INT_TO_FIXED (y - height),
		       CLUTTER_INT_TO_FIXED (width),
		       CLUTTER_INT_TO_FIXED (height));
  cogl_path_stroke ();
}

static void
pango_clutter_renderer_draw_rectangle (PangoRenderer    *renderer,
				       PangoRenderPart   part,
				       int               x,
				       int               y,
				       int               width,
				       int               height)
{
  float x1, x2, y1, y2;
  const PangoMatrix *matrix;

  if ((matrix = pango_renderer_get_matrix (renderer)))
    {
      /* Convert user-space coords to device coords */
      x1 = (x * matrix->xx + y * matrix->xy) / PANGO_SCALE + matrix->x0;
      x2 = ((x + width) * matrix->xx + (y + height) * matrix->xy)
	/ PANGO_SCALE + matrix->x0;
      y1 = (y * matrix->yy + x * matrix->yx) / PANGO_SCALE + matrix->y0;
      y2 = ((y + height) * matrix->yy + (x + width) * matrix->yx)
	/ PANGO_SCALE + matrix->y0;
    }
  else
    {
      x1 = x / PANGO_SCALE;
      x2 = (x + width) / PANGO_SCALE;
      y1 = y / PANGO_SCALE;
      y2 = (y + height) / PANGO_SCALE;
    }

  cogl_rectangle (x1, y1, x2 - x1, y2 - y1);
}

static void
pango_clutter_renderer_draw_trapezoid (PangoRenderer    *renderer,
				       PangoRenderPart   part,
				       double            y1,
				       double            x11,
				       double            x21,
				       double            y2,
				       double            x12,
				       double            x22)
{
  ClutterFixed points[8];

  points[0] = CLUTTER_FLOAT_TO_FIXED (x11);
  points[1] = CLUTTER_FLOAT_TO_FIXED (y1);
  points[2] = CLUTTER_FLOAT_TO_FIXED (x12);
  points[3] = CLUTTER_FLOAT_TO_FIXED (y2);
  points[4] = CLUTTER_FLOAT_TO_FIXED (x22);
  points[5] = points[3];
  points[6] = CLUTTER_FLOAT_TO_FIXED (x21);
  points[7] = points[1];

  cogl_path_polygon (points, 4);
  cogl_path_fill ();
}

static void
pango_clutter_renderer_draw_glyph (PangoRenderer *renderer,
				   PangoFont     *font,
				   PangoGlyph     glyph,
				   double         xd,
				   double         yd)
{
  PangoClutterGlyphCacheValue *cache_value;
  ClutterFixed x = CLUTTER_FLOAT_TO_FIXED ((float) xd);
  ClutterFixed y = CLUTTER_FLOAT_TO_FIXED ((float) yd);

  if ((glyph & PANGO_GLYPH_UNKNOWN_FLAG))
    {
      PangoFontMetrics *metrics;

      if (font == NULL
	  || (metrics = pango_font_get_metrics (font, NULL)) == NULL)
	pango_clutter_renderer_draw_box (CLUTTER_FIXED_TO_INT (x),
					 CLUTTER_FIXED_TO_INT (y),
					 PANGO_UNKNOWN_GLYPH_WIDTH,
					 PANGO_UNKNOWN_GLYPH_HEIGHT);
      else
	{
	  pango_clutter_renderer_draw_box (CLUTTER_FIXED_TO_INT (x),
					   CLUTTER_FIXED_TO_INT (y),
					   metrics->approximate_char_width
					   / PANGO_SCALE,
					   metrics->ascent / PANGO_SCALE);

	  pango_font_metrics_unref (metrics);
	}

      return;
    }

  /* Get the texture containing the glyph. This will create the cache
     entry if there isn't already one */
  cache_value = pango_clutter_renderer_get_cached_glyph (renderer, font, glyph);

  if (cache_value == NULL)
    {
      pango_clutter_renderer_draw_box (CLUTTER_FIXED_TO_INT (x),
				       CLUTTER_FIXED_TO_INT (y),
				       PANGO_UNKNOWN_GLYPH_WIDTH,
				       PANGO_UNKNOWN_GLYPH_HEIGHT);

      return;
    }

  x += CLUTTER_INT_TO_FIXED (cache_value->draw_x);
  y += CLUTTER_INT_TO_FIXED (cache_value->draw_y);

  /* Render the glyph from the texture */
  cogl_texture_rectangle (cache_value->texture, x, y,
			  x + CLUTTER_INT_TO_FIXED (cache_value->draw_width),
			  y + CLUTTER_INT_TO_FIXED (cache_value->draw_height),
			  cache_value->tx1, cache_value->ty1,
			  cache_value->tx2, cache_value->ty2);
}

static void
pango_clutter_renderer_prepare_run (PangoRenderer  *renderer,
				    PangoLayoutRun *run)
{
  GSList               *node;
  PangoColor           *fg = NULL;
  ClutterColor          col;
  PangoClutterRenderer *priv = PANGO_CLUTTER_RENDERER (renderer);

  for (node = run->item->analysis.extra_attrs; node; node = node->next)
    {
      PangoAttribute *attr = node->data;

      switch (attr->klass->type)
	{
	case PANGO_ATTR_FOREGROUND:
	  fg = &((PangoAttrColor *) attr)->color;

	default:
	  break;
	}
    }

   if (fg)
    {
      col.red   = (fg->red   * 255) / 65535;
      col.green = (fg->green * 255) / 65535;
      col.blue  = (fg->blue  * 255) / 65535;
    }
  else
    {
      col.red   = priv->color.red;
      col.green = priv->color.green;
      col.blue  = priv->color.blue;
    }

  col.alpha = priv->color.alpha;

  cogl_color (&col);

  /* Chain up */
  (* PANGO_RENDERER_CLASS (parent_class)->prepare_run) (renderer, run);
}
