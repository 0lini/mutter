#define _GNU_SOURCE
#define _XOPEN_SOURCE 500 /* for usleep() */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <gdk/gdk.h>

#include "display.h"
#include "screen.h"
#include "frame.h"
#include "errors.h"
#include "window.h"
#include "compositor-private.h"
#include "compositor-clutter.h"
#include "compositor-clutter-plugin-manager.h"
#include "tidy-texture-frame.h"
#include "xprops.h"
#include "shaped-texture.h"
#include "tidy-texture-frame.h"
#include <X11/Xatom.h>
#include <X11/Xlibint.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include <clutter/clutter.h>
#include <clutter/clutter-group.h>
#include <clutter/x11/clutter-x11.h>
#ifdef HAVE_GLX_TEXTURE_PIXMAP
#include <clutter/glx/clutter-glx.h>
#endif /* HAVE_GLX_TEXTURE_PIXMAP */

#include <cogl/cogl.h>
#define SHADOW_RADIUS 8
#define SHADOW_OPACITY	0.9
#define SHADOW_OFFSET_X	(SHADOW_RADIUS)
#define SHADOW_OFFSET_Y	(SHADOW_RADIUS)

#define MAX_TILE_SZ 8 	/* Must be <= shaddow radius */
#define TILE_WIDTH  (3*MAX_TILE_SZ)
#define TILE_HEIGHT (3*MAX_TILE_SZ)

/*
 * Register GType wrapper for XWindowAttributes, so we do not have to
 * query window attributes in the MetaCompWindow constructor but can pass
 * them as a property to the constructor (so we can gracefully handle the case
 * where no attributes can be retrieved).
 *
 * NB -- we only need a subset of the attributes; at some point we might want
 * to just store the relevant values rather than the whole struct.
 */
#define META_TYPE_XATTRS (meta_xattrs_get_type ())

GType meta_xattrs_get_type   (void) G_GNUC_CONST;

static XWindowAttributes *
meta_xattrs_copy (const XWindowAttributes *attrs)
{
  XWindowAttributes *result;

  g_return_val_if_fail (attrs != NULL, NULL);

  result = (XWindowAttributes*) Xmalloc (sizeof (XWindowAttributes));
  *result = *attrs;

  return result;
}

static void
meta_xattrs_free (XWindowAttributes *attrs)
{
  g_return_if_fail (attrs != NULL);

  XFree (attrs);
}

GType
meta_xattrs_get_type (void)
{
  static GType our_type = 0;

  if (!our_type)
    our_type = g_boxed_type_register_static ("XWindowAttributes",
		                     (GBoxedCopyFunc) meta_xattrs_copy,
				     (GBoxedFreeFunc) meta_xattrs_free);
  return our_type;
}

static unsigned char* shadow_gaussian_make_tile (void);

#ifdef HAVE_COMPOSITE_EXTENSIONS
static inline gboolean
composite_at_least_version (MetaDisplay *display, int maj, int min)
{
  static int major = -1;
  static int minor = -1;

  if (major == -1)
    meta_display_get_compositor_version (display, &major, &minor);

  return (major > maj || (major == maj && minor >= min));
}
#endif

typedef struct _MetaCompositorClutter
{
  MetaCompositor  compositor;
  MetaDisplay    *display;

  Atom            atom_x_root_pixmap;
  Atom            atom_x_set_root;
  Atom            atom_net_wm_window_opacity;

  ClutterActor   *shadow_src;

  gboolean        show_redraw : 1;
  gboolean        debug       : 1;
} MetaCompositorClutter;

typedef struct _MetaCompScreen
{
  MetaScreen            *screen;

  ClutterActor          *stage, *window_group, *overlay_group;
  GList                 *windows;
  GHashTable            *windows_by_xid;
  MetaWindow            *focus_window;
  Window                 output;
  GSList                *dock_windows;

  gint                   switch_workspace_in_progress;

  MetaCompositorClutterPluginManager *plugin_mgr;
} MetaCompScreen;

/*
 * MetaCompWindow implementation
 */
struct _MetaCompWindowPrivate
{
  XWindowAttributes attrs;

  MetaWindow       *window;
  Window            xwindow;
  MetaScreen       *screen;

  ClutterActor     *actor;
  ClutterActor     *shadow;
  Pixmap            back_pixmap;

  MetaCompWindowType type;
  Damage            damage;

  guint8            opacity;

  /*
   * These need to be counters rather than flags, since more plugins
   * can implement same effect; the practicality of stacking effects
   * might be dubious, but we have to at least handle it correctly.
   */
  gint              minimize_in_progress;
  gint              maximize_in_progress;
  gint              unmaximize_in_progress;
  gint              map_in_progress;
  gint              destroy_in_progress;

  gboolean          needs_shadow           : 1;
  gboolean          shaped                 : 1;
  gboolean          destroy_pending        : 1;
  gboolean          argb32                 : 1;
  gboolean          disposed               : 1;
  gboolean          is_minimized           : 1;

  /* Desktop switching flags */
  gboolean          needs_map              : 1;
  gboolean          needs_unmap            : 1;
  gboolean          needs_repair           : 1;
};

enum
{
  PROP_MCW_META_WINDOW = 1,
  PROP_MCW_META_SCREEN,
  PROP_MCW_X_WINDOW,
  PROP_MCW_X_WINDOW_ATTRIBUTES
};

static void meta_comp_window_class_init (MetaCompWindowClass *klass);
static void meta_comp_window_init       (MetaCompWindow *self);
static void meta_comp_window_dispose    (GObject *object);
static void meta_comp_window_finalize   (GObject *object);
static void meta_comp_window_constructed (GObject *object);
static void meta_comp_window_set_property (GObject       *object,
					   guint         prop_id,
					   const GValue *value,
					   GParamSpec   *pspec);
static void meta_comp_window_get_property (GObject      *object,
					   guint         prop_id,
					   GValue       *value,
					   GParamSpec   *pspec);
static void meta_comp_window_query_window_type (MetaCompWindow *self);
static void meta_comp_window_detach (MetaCompWindow *self);

G_DEFINE_TYPE (MetaCompWindow, meta_comp_window, CLUTTER_TYPE_GROUP);

static void
meta_comp_window_class_init (MetaCompWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec   *pspec;

  g_type_class_add_private (klass, sizeof (MetaCompWindowPrivate));

  object_class->dispose      = meta_comp_window_dispose;
  object_class->finalize     = meta_comp_window_finalize;
  object_class->set_property = meta_comp_window_set_property;
  object_class->get_property = meta_comp_window_get_property;
  object_class->constructed  = meta_comp_window_constructed;

  pspec = g_param_spec_pointer ("meta-window",
				"MetaWindow",
				"MetaWindow",
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_property (object_class,
                                   PROP_MCW_META_WINDOW,
                                   pspec);

  pspec = g_param_spec_pointer ("meta-screen",
				"MetaScreen",
				"MetaScreen",
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_property (object_class,
                                   PROP_MCW_META_SCREEN,
                                   pspec);

  pspec = g_param_spec_ulong ("x-window",
			      "Window",
			      "Window",
			      0,
			      G_MAXULONG,
			      0,
			      G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_property (object_class,
                                   PROP_MCW_X_WINDOW,
                                   pspec);

  pspec = g_param_spec_boxed ("x-window-attributes",
			      "XWindowAttributes",
			      "XWindowAttributes",
			      META_TYPE_XATTRS,
			      G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_property (object_class,
                                   PROP_MCW_X_WINDOW_ATTRIBUTES,
                                   pspec);
}

static void
meta_comp_window_init (MetaCompWindow *self)
{
  MetaCompWindowPrivate *priv;

  priv = self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
						   META_TYPE_COMP_WINDOW,
						   MetaCompWindowPrivate);
  priv->opacity = 0xff;
}

static gboolean is_shaped (MetaDisplay *display, Window xwindow);
static gboolean meta_comp_window_has_shadow (MetaCompWindow *self);
static void update_shape (MetaCompositorClutter *compositor,
                          MetaCompWindow *cw);

static void
meta_comp_window_constructed (GObject *object)
{
  MetaCompWindow        *self     = META_COMP_WINDOW (object);
  MetaCompWindowPrivate *priv     = self->priv;
  MetaScreen            *screen   = priv->screen;
  MetaDisplay           *display  = meta_screen_get_display (screen);
  Window                 xwindow  = priv->xwindow;
  Display               *xdisplay = meta_display_get_xdisplay (display);
  XRenderPictFormat     *format;

  meta_comp_window_query_window_type (self);

#ifdef HAVE_SHAPE
  /* Listen for ShapeNotify events on the window */
  if (meta_display_has_shape (display))
    XShapeSelectInput (xdisplay, xwindow, ShapeNotifyMask);
#endif

  priv->shaped = is_shaped (display, xwindow);

  if (priv->attrs.class == InputOnly)
    priv->damage = None;
  else
    priv->damage = XDamageCreate (xdisplay, xwindow, XDamageReportNonEmpty);

  format = XRenderFindVisualFormat (xdisplay, priv->attrs.visual);

  if (format && format->type == PictTypeDirect && format->direct.alphaMask)
    priv->argb32 = TRUE;

  if (meta_comp_window_has_shadow (self))
    {
      MetaCompositorClutter *compositor =
	(MetaCompositorClutter*)meta_display_get_compositor (display);

      priv->shadow =
	tidy_texture_frame_new (CLUTTER_TEXTURE (compositor->shadow_src),
				MAX_TILE_SZ,
				MAX_TILE_SZ,
				MAX_TILE_SZ,
				MAX_TILE_SZ);

      clutter_actor_set_position (priv->shadow,
				  SHADOW_OFFSET_X , SHADOW_OFFSET_Y);
      clutter_container_add_actor (CLUTTER_CONTAINER (self), priv->shadow);
    }

  priv->actor = meta_shaped_texture_new ();
  clutter_container_add_actor (CLUTTER_CONTAINER (self), priv->actor);

  update_shape ((MetaCompositorClutter *)
                 meta_display_get_compositor (display),
                 self);
}

static void
meta_comp_window_dispose (GObject *object)
{
  MetaCompWindow        *self = META_COMP_WINDOW (object);
  MetaCompWindowPrivate *priv = self->priv;
  MetaScreen            *screen;
  MetaDisplay           *display;
  Display               *xdisplay;
  MetaCompScreen        *info;

  if (priv->disposed)
    return;

  priv->disposed = TRUE;

  screen   = priv->screen;
  display  = meta_screen_get_display (screen);
  xdisplay = meta_display_get_xdisplay (display);
  info     = meta_screen_get_compositor_data (screen);

  meta_comp_window_detach (self);

  if (priv->damage != None)
    {
      meta_error_trap_push (display);
      XDamageDestroy (xdisplay, priv->damage);
      meta_error_trap_pop (display, FALSE);

      priv->damage = None;
    }

  /*
   * Check we are not in the dock list -- FIXME (do this in a cleaner way)
   */
  if (priv->type == META_COMP_WINDOW_DOCK)
    info->dock_windows = g_slist_remove (info->dock_windows, self);

  info->windows = g_list_remove (info->windows, (gconstpointer) self);
  g_hash_table_remove (info->windows_by_xid, (gpointer) priv->xwindow);

  G_OBJECT_CLASS (meta_comp_window_parent_class)->dispose (object);
}

static void
meta_comp_window_finalize (GObject *object)
{
  G_OBJECT_CLASS (meta_comp_window_parent_class)->finalize (object);
}

static void
meta_comp_window_set_property (GObject      *object,
			       guint         prop_id,
			       const GValue *value,
			       GParamSpec   *pspec)
{
  MetaCompWindowPrivate *priv = META_COMP_WINDOW (object)->priv;

  switch (prop_id)
    {
    case PROP_MCW_META_WINDOW:
      priv->window = g_value_get_pointer (value);
      break;
    case PROP_MCW_META_SCREEN:
      priv->screen = g_value_get_pointer (value);
      break;
    case PROP_MCW_X_WINDOW:
      priv->xwindow = g_value_get_ulong (value);
      break;
    case PROP_MCW_X_WINDOW_ATTRIBUTES:
      priv->attrs = *((XWindowAttributes*)g_value_get_boxed (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_comp_window_get_property (GObject      *object,
			       guint         prop_id,
			       GValue       *value,
			       GParamSpec   *pspec)
{
  MetaCompWindowPrivate *priv = META_COMP_WINDOW (object)->priv;

  switch (prop_id)
    {
    case PROP_MCW_META_WINDOW:
      g_value_set_pointer (value, priv->window);
      break;
    case PROP_MCW_META_SCREEN:
      g_value_set_pointer (value, priv->screen);
      break;
    case PROP_MCW_X_WINDOW:
      g_value_set_ulong (value, priv->xwindow);
      break;
    case PROP_MCW_X_WINDOW_ATTRIBUTES:
      g_value_set_boxed (value, &priv->attrs);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static MetaCompWindow*
find_window_for_screen (MetaScreen *screen, Window xwindow)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  if (info == NULL)
      return NULL;

  return g_hash_table_lookup (info->windows_by_xid, (gpointer) xwindow);
}

static MetaCompWindow *
find_window_in_display (MetaDisplay *display, Window xwindow)
{
  GSList *index;

  for (index = meta_display_get_screens (display);
       index;
       index = index->next)
    {
      MetaCompWindow *cw = find_window_for_screen (index->data, xwindow);

      if (cw != NULL)
        return cw;
    }

  return NULL;
}

static MetaCompWindow *
find_window_for_child_window_in_display (MetaDisplay *display, Window xwindow)
{
  Window ignored1, *ignored2, parent;
  guint  ignored_children;

  XQueryTree (meta_display_get_xdisplay (display), xwindow, &ignored1,
              &parent, &ignored2, &ignored_children);

  if (parent != None)
    return find_window_in_display (display, parent);

  return NULL;
}

static void
meta_comp_window_query_window_type (MetaCompWindow *self)
{
  MetaCompWindowPrivate *priv    = self->priv;
  MetaScreen            *screen  = priv->screen;
  MetaDisplay           *display = meta_screen_get_display (screen);
  Window                 xwindow = priv->xwindow;
  gint                   n_atoms;
  Atom                  *atoms;
  gint                   i;

  if (priv->attrs.override_redirect)
    {
      priv->type = META_COMP_WINDOW_OVERRIDE;
      return;
    }

  /*
   * If the window is managed by the WM, get the type from the WM,
   * otherwise do it the hard way.
   */
  if (priv->window && meta_window_get_type_atom (priv->window) != None)
    {
      priv->type = (MetaCompWindowType) meta_window_get_type (priv->window);
      return;
    }

  n_atoms = 0;
  atoms = NULL;

  /*
   * Assume normal
   */
  priv->type = META_COMP_WINDOW_NORMAL;

  meta_prop_get_atom_list (display, xwindow,
                           meta_display_get_atom (display,
					       META_ATOM__NET_WM_WINDOW_TYPE),
                           &atoms, &n_atoms);

  for (i = 0; i < n_atoms; i++)
    {
      if (atoms[i] ==
	  meta_display_get_atom (display,
				 META_ATOM__NET_WM_WINDOW_TYPE_DND))
	{
	  priv->type = META_COMP_WINDOW_DND;
	  break;
	}
      else if (atoms[i] ==
	       meta_display_get_atom (display,
				      META_ATOM__NET_WM_WINDOW_TYPE_DESKTOP))
	{
	  priv->type = META_COMP_WINDOW_DESKTOP;
	  break;
	}
      else if (atoms[i] ==
	       meta_display_get_atom (display,
				      META_ATOM__NET_WM_WINDOW_TYPE_DOCK))
	{
	  priv->type = META_COMP_WINDOW_DOCK;
	  break;
	}
      else if (atoms[i] ==
	       meta_display_get_atom (display,
				      META_ATOM__NET_WM_WINDOW_TYPE_TOOLBAR) ||
	       atoms[i] ==
	       meta_display_get_atom (display,
				      META_ATOM__NET_WM_WINDOW_TYPE_MENU)    ||
	       atoms[i] ==
	       meta_display_get_atom (display,
				      META_ATOM__NET_WM_WINDOW_TYPE_DIALOG)  ||
	       atoms[i] ==
	       meta_display_get_atom (display,
				      META_ATOM__NET_WM_WINDOW_TYPE_NORMAL)  ||
	       atoms[i] ==
	       meta_display_get_atom (display,
				      META_ATOM__NET_WM_WINDOW_TYPE_UTILITY) ||
	       atoms[i] ==
	       meta_display_get_atom (display,
				      META_ATOM__NET_WM_WINDOW_TYPE_SPLASH))
        {
	  priv->type = META_COMP_WINDOW_NORMAL;
	  break;
        }
    }

  meta_XFree (atoms);
}

static gboolean
is_shaped (MetaDisplay *display, Window xwindow)
{
  Display *xdisplay = meta_display_get_xdisplay (display);
  gint     xws, yws, xbs, ybs;
  guint    wws, hws, wbs, hbs;
  gint     bounding_shaped, clip_shaped;

  if (meta_display_has_shape (display))
    {
      XShapeQueryExtents (xdisplay, xwindow, &bounding_shaped,
                          &xws, &yws, &wws, &hws, &clip_shaped,
                          &xbs, &ybs, &wbs, &hbs);
      return (bounding_shaped != 0);
    }

  return FALSE;
}

static gboolean
meta_comp_window_has_shadow (MetaCompWindow *self)
{
  MetaCompWindowPrivate * priv = self->priv;

  /*
   * Do not add shadows to ARGB windows (since they are probably transparent)
   */
  if (priv->argb32 || priv->opacity != 0xff)
    {
      meta_verbose ("Window 0x%x has no shadow as it is ARGB\n",
		    (guint)priv->xwindow);
      return FALSE;
    }

  /*
   * Always put a shadow around windows with a frame - This should override
   * the restriction about not putting a shadow around shaped windows
   * as the frame might be the reason the window is shaped
   */
  if (priv->window)
    {
      if (meta_window_get_frame (priv->window))
	{
	  meta_verbose ("Window 0x%x has shadow because it has a frame\n",
			(guint)priv->xwindow);
	  return TRUE;
	}
    }

  /*
   * Never put a shadow around shaped windows
   */
  if (priv->shaped)
    {
      meta_verbose ("Window 0x%x has no shadow as it is shaped\n",
		    (guint)priv->xwindow);
      return FALSE;
    }

  /*
   * Add shadows to override redirect windows (e.g., Gtk menus).
   * This must have lower priority than window shape test.
   */
  if (priv->attrs.override_redirect)
    {
      meta_verbose ("Window 0x%x has shadow because it is override redirect.\n",
		    (guint)priv->xwindow);
      return TRUE;
    }

  /*
   * Don't put shadow around DND icon windows
   */
  if (priv->type == META_COMP_WINDOW_DND ||
      priv->type == META_COMP_WINDOW_DESKTOP)
    {
      meta_verbose ("Window 0x%x has no shadow as it is DND or Desktop\n",
		    (guint)priv->xwindow);
      return FALSE;
    }

  if (priv->type == META_COMP_WINDOW_MENU
#if 0
      || priv->type == META_COMP_WINDOW_DROP_DOWN_MENU
#endif
      )
    {
      meta_verbose ("Window 0x%x has shadow as it is a menu\n",
		    (guint)priv->xwindow);
      return TRUE;
    }

#if 0
  if (priv->type == META_COMP_WINDOW_TOOLTIP)
    {
      meta_verbose ("Window 0x%x has shadow as it is a tooltip\n",
		    (guint)priv->xwindow);
      return TRUE;
    }
#endif

  meta_verbose ("Window 0x%x has no shadow as it fell through\n",
		(guint)priv->xwindow);
  return FALSE;
}

Window
meta_comp_window_get_x_window (MetaCompWindow *mcw)
{
  if (!mcw)
    return None;

  return mcw->priv->xwindow;
}

MetaCompWindowType
meta_comp_window_get_window_type (MetaCompWindow *mcw)
{
  if (!mcw)
    return 0;

  return mcw->priv->type;
}

gint
meta_comp_window_get_workspace (MetaCompWindow *mcw)
{
  MetaCompWindowPrivate *priv;
  MetaWorkspace         *workspace;

  if (!mcw)
    return -1;

  priv = mcw->priv;

  if (!priv->window || meta_window_is_on_all_workspaces (priv->window))
    return -1;

  workspace = meta_window_get_workspace (priv->window);

  return meta_workspace_index (workspace);
}

static void repair_win (MetaCompWindow *cw);
static void map_win    (MetaCompWindow *cw);
static void unmap_win  (MetaCompWindow *cw);

static void
meta_compositor_clutter_finish_workspace_switch (MetaCompScreen *info)
{
  GList *last = g_list_last (info->windows);
  GList *l    = last;

  while (l)
    {
      MetaCompWindow        *cw   = l->data;
      MetaCompWindowPrivate *priv = cw->priv;

      if (priv->needs_map && !priv->needs_unmap)
	{
	  map_win (cw);
	}

      if (priv->needs_unmap)
	{
	  unmap_win (cw);
	}

      l = l->prev;
    }

  /*
   * Now fix up stacking order in case the plugin messed it up.
   */
  l = last;
  while (l)
    {
      ClutterActor *a     = l->data;
      GList        *prev  = l->prev;

      if (prev)
	{
	  ClutterActor *above_me = prev->data;

	  clutter_actor_raise (above_me, a);
	}
      else
	{
	  ClutterActor *a = l->data;
	  clutter_actor_raise_top (a);
	}

      l = prev;
    }
}

void
meta_compositor_clutter_window_effect_completed (MetaCompWindow *cw,
						 gulong          event)
{
  MetaCompWindowPrivate *priv   = cw->priv;
  MetaScreen            *screen = priv->screen;
  MetaCompScreen        *info   = meta_screen_get_compositor_data (screen);
  ClutterActor          *actor  = CLUTTER_ACTOR (cw);

    switch (event)
    {
    case META_COMPOSITOR_CLUTTER_PLUGIN_MINIMIZE:
      {
	ClutterActor *a      = CLUTTER_ACTOR (cw);
	gint          height = clutter_actor_get_height (a);

	priv->minimize_in_progress--;
	if (priv->minimize_in_progress < 0)
	  {
	    g_warning ("Error in minimize accounting.");
	    priv->minimize_in_progress = 0;
	  }

	if (!priv->minimize_in_progress)
	  {
	    priv->is_minimized = TRUE;
	    clutter_actor_set_position (a, 0, -height);
	  }
      }
      break;
    case META_COMPOSITOR_CLUTTER_PLUGIN_MAP:
      /*
       * Make sure that the actor is at the correct place in case
       * the plugin fscked.
       */
      priv->map_in_progress--;

      if (priv->map_in_progress < 0)
	{
	  g_warning ("Error in map accounting.");
	  priv->map_in_progress = 0;
	}

      if (!priv->map_in_progress)
	{
	  priv->is_minimized = FALSE;
	  clutter_actor_set_anchor_point (actor, 0, 0);
	  clutter_actor_set_position (actor, priv->attrs.x, priv->attrs.y);
	  clutter_actor_show_all (actor);
	}
      break;
    case META_COMPOSITOR_CLUTTER_PLUGIN_DESTROY:
      priv->destroy_in_progress--;

      if (priv->destroy_in_progress < 0)
	{
	  g_warning ("Error in destroy accounting.");
	  priv->destroy_in_progress = 0;
	}

      if (!priv->destroy_in_progress)
	{
	  clutter_actor_destroy (actor);
	}
      break;
    case META_COMPOSITOR_CLUTTER_PLUGIN_UNMAXIMIZE:
      priv->unmaximize_in_progress--;
      if (priv->unmaximize_in_progress < 0)
	{
	  g_warning ("Error in unmaximize accounting.");
	  priv->unmaximize_in_progress = 0;
	}

      if (!priv->unmaximize_in_progress)
	{
	  clutter_actor_set_position (actor, priv->attrs.x, priv->attrs.y);
	  meta_comp_window_detach (cw);
	  repair_win (cw);
	}
      break;
    case META_COMPOSITOR_CLUTTER_PLUGIN_MAXIMIZE:
      priv->maximize_in_progress--;
      if (priv->maximize_in_progress < 0)
	{
	  g_warning ("Error in maximize accounting.");
	  priv->maximize_in_progress = 0;
	}

      if (!priv->maximize_in_progress)
	{
	  clutter_actor_set_position (actor, priv->attrs.x, priv->attrs.y);
	  meta_comp_window_detach (cw);
	  repair_win (cw);
	}
      break;
    case META_COMPOSITOR_CLUTTER_PLUGIN_SWITCH_WORKSPACE:
      /* FIXME -- must redo stacking order */
      info->switch_workspace_in_progress--;
      if (info->switch_workspace_in_progress < 0)
	{
	  g_warning ("Error in workspace_switch accounting!");
	  info->switch_workspace_in_progress = 0;
	}

      if (!info->switch_workspace_in_progress)
	meta_compositor_clutter_finish_workspace_switch (info);
      break;
    default: ;
    }
}


static void
clutter_cmp_destroy (MetaCompositor *compositor)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS

#endif
}

/*
 * If force is TRUE, free the back pixmap; if FALSE, only free it if the
 * backing pixmap has actually changed.
 */
static void
meta_comp_window_detach (MetaCompWindow *self)
{
  MetaCompWindowPrivate *priv     = self->priv;
  MetaScreen            *screen   = priv->screen;
  MetaDisplay           *display  = meta_screen_get_display (screen);
  Display               *xdisplay = meta_display_get_xdisplay (display);

  if (!priv->back_pixmap)
    return;

  XFreePixmap (xdisplay, priv->back_pixmap);
  priv->back_pixmap = None;
}

static void
destroy_win (MetaDisplay *display, Window xwindow)
{
  MetaCompWindow *cw;

  cw = find_window_in_display (display, xwindow);

  if (cw == NULL)
    return;

  clutter_actor_destroy (CLUTTER_ACTOR (cw));
}

static void
restack_win (MetaCompWindow *cw, Window above)
{
  MetaCompWindowPrivate *priv = cw->priv;
  MetaScreen            *screen = priv->screen;
  MetaCompScreen        *info = meta_screen_get_compositor_data (screen);
  Window                 previous_above;
  GList                 *sibling, *next;

  sibling = g_list_find (info->windows, (gconstpointer) cw);
  next = g_list_next (sibling);
  previous_above = None;

  if (next)
    {
      MetaCompWindow *ncw = next->data;
      previous_above = ncw->priv->xwindow;
    }

  /* If above is set to None, the window whose state was changed is on
   * the bottom of the stack with respect to sibling.
   */
  if (above == None)
    {
      /* Insert at bottom of window stack */
      info->windows = g_list_delete_link (info->windows, sibling);
      info->windows = g_list_append (info->windows, cw);

      if (!info->switch_workspace_in_progress)
	clutter_actor_raise_top (CLUTTER_ACTOR (cw));
    }
  else if (previous_above != above)
    {
      GList *index;

      for (index = info->windows; index; index = index->next)
	{
	  MetaCompWindow *cw2 = (MetaCompWindow *) index->data;
	  if (cw2->priv->xwindow == above)
	    break;
	}

      if (index != NULL)
        {
          ClutterActor *above_win = index->data;

          info->windows = g_list_delete_link (info->windows, sibling);
          info->windows = g_list_insert_before (info->windows, index, cw);

	  if (!info->switch_workspace_in_progress)
	    clutter_actor_raise (CLUTTER_ACTOR (cw), above_win);
        }
    }
}

static void
resize_win (MetaCompWindow *cw,
            int             x,
            int             y,
            int             width,
            int             height,
            int             border_width,
            gboolean        override_redirect)
{
  MetaCompWindowPrivate *priv = cw->priv;

  if (priv->attrs.width != width || priv->attrs.height != height)
    meta_comp_window_detach (cw);

  priv->attrs.width = width;
  priv->attrs.height = height;
  priv->attrs.x = x;
  priv->attrs.y = y;
  priv->attrs.border_width      = border_width;
  priv->attrs.override_redirect = override_redirect;

  if (priv->maximize_in_progress   ||
      priv->unmaximize_in_progress ||
      priv->map_in_progress)
    return;

  clutter_actor_set_position (CLUTTER_ACTOR (cw), x, y);
}

static void
map_win (MetaCompWindow *cw)
{
  MetaCompWindowPrivate *priv;
  MetaCompScreen        *info;

  if (cw == NULL)
    return;

  priv = cw->priv;
  info = meta_screen_get_compositor_data (priv->screen);

  if (priv->attrs.map_state == IsViewable)
    return;

  priv->attrs.map_state = IsViewable;

  /*
   * Now repair the window; this ensures that the actor is correctly sized
   * before we run any effects on it.
   */
  priv->needs_map = FALSE;
  meta_comp_window_detach (cw);
  repair_win (cw);

  /*
   * Make sure the position is set correctly (we might have got moved while
   * unmapped.
   */
  if (!info->switch_workspace_in_progress)
    {
      clutter_actor_set_anchor_point (CLUTTER_ACTOR (cw), 0, 0);
      clutter_actor_set_position (CLUTTER_ACTOR (cw),
				  cw->priv->attrs.x, cw->priv->attrs.y);
    }

  priv->map_in_progress++;

  /*
   * If a plugin manager is present, try to run an effect; if no effect of this
   * type is present, destroy the actor.
   */
  if (info->switch_workspace_in_progress || !info->plugin_mgr ||
      !meta_compositor_clutter_plugin_manager_event_simple (info->plugin_mgr,
				cw,
                                META_COMPOSITOR_CLUTTER_PLUGIN_MAP))
    {
      clutter_actor_show_all (CLUTTER_ACTOR (cw));
      priv->map_in_progress--;
      priv->is_minimized = FALSE;
    }
}

static void
unmap_win (MetaCompWindow *cw)
{
  MetaCompWindowPrivate *priv;
  MetaCompScreen        *info;

  if (cw == NULL)
    return;

  priv = cw->priv;
  info = meta_screen_get_compositor_data (priv->screen);

  /*
   * If the needs_unmap flag is set, we carry on even if the winow is
   * already marked as unmapped; this is necessary so windows temporarily
   * shown during an effect (like desktop switch) are properly hidden again.
   */
  if (priv->attrs.map_state == IsUnmapped && !priv->needs_unmap)
    return;

  if (priv->window && priv->window == info->focus_window)
    info->focus_window = NULL;

  if (info->switch_workspace_in_progress)
    {
      /*
       * Cannot unmap windows while switching desktops effect is in progress.
       */
      priv->needs_unmap = TRUE;
      return;
    }

  priv->attrs.map_state = IsUnmapped;

  if (!priv->minimize_in_progress)
    {
      ClutterActor *a = CLUTTER_ACTOR (cw);
      clutter_actor_hide (a);
    }

  priv->needs_unmap = FALSE;
  priv->needs_map   = FALSE;
}


static void
add_win (MetaScreen *screen, MetaWindow *window, Window xwindow)
{
  MetaDisplay           *display = meta_screen_get_display (screen);
  MetaCompScreen        *info = meta_screen_get_compositor_data (screen);
  MetaCompWindow        *cw;
  MetaCompWindowPrivate *priv;
  Display               *xdisplay = meta_display_get_xdisplay (display);
  XWindowAttributes      attrs;

  if (info == NULL)
    return;

  if (xwindow == info->output)
    return;

  if (!XGetWindowAttributes (xdisplay, xwindow, &attrs))
      return;

  /*
   * If Metacity has decided not to manage this window then the input events
   * won't have been set on the window
   */
  if (!(attrs.your_event_mask & PropertyChangeMask))
    {
      gulong event_mask;

      event_mask = attrs.your_event_mask | PropertyChangeMask;
      XSelectInput (xdisplay, xwindow, event_mask);
    }

  meta_verbose ("add window: Meta %p, xwin 0x%x\n", window, (guint) xwindow);

  cw = g_object_new (META_TYPE_COMP_WINDOW,
		     "meta-window",         window,
		     "x-window",            xwindow,
		     "meta-screen",         screen,
		     "x-window-attributes", &attrs,
		     NULL);

  priv = cw->priv;

  clutter_actor_set_position (CLUTTER_ACTOR (cw),
			      priv->attrs.x, priv->attrs.y);
  clutter_container_add_actor (CLUTTER_CONTAINER (info->window_group),
			       CLUTTER_ACTOR (cw));
  clutter_actor_hide (CLUTTER_ACTOR (cw));

  if (priv->type == META_COMP_WINDOW_DOCK)
    {
      meta_verbose ("Appending 0x%x to dock windows\n", (guint)xwindow);
      info->dock_windows = g_slist_append (info->dock_windows, cw);
    }

#if 0
  printf ("added 0x%x (%p) type:", (guint)xwindow, cw);

  switch (cw->type)
    {
    case META_COMP_WINDOW_NORMAL:
      printf("normal"); break;
    case META_COMP_WINDOW_DND:
      printf("dnd"); break;
    case META_COMP_WINDOW_DESKTOP:
      printf("desktop"); break;
    case META_COMP_WINDOW_DOCK:
      printf("dock"); break;
    case META_COMP_WINDOW_MENU:
      printf("menu"); break;
    case META_COMP_WINDOW_DROP_DOWN_MENU:
      printf("menu"); break;
    case META_COMP_WINDOW_TOOLTIP:
      printf("tooltip"); break;
    default:
      printf("unknown");
      break;
    }

  if (window && meta_window_get_frame (window))
    printf(" *HAS FRAME* ");

  printf("\n");
#endif

  /*
   * Add this to the list at the top of the stack before it is mapped so that
   * map_win can find it again
   */
  info->windows = g_list_prepend (info->windows, cw);
  g_hash_table_insert (info->windows_by_xid, (gpointer) xwindow, cw);

  if (priv->attrs.map_state == IsViewable)
    {
      /* Need to reset the map_state for map_win() to work */
      priv->attrs.map_state = IsUnmapped;
      map_win (cw);
    }
}

static void
repair_win (MetaCompWindow *cw)
{
  MetaCompWindowPrivate *priv     = cw->priv;
  MetaScreen            *screen   = priv->screen;
  MetaDisplay           *display  = meta_screen_get_display (screen);
  Display               *xdisplay = meta_display_get_xdisplay (display);
  MetaCompScreen        *info     = meta_screen_get_compositor_data (screen);
  Window                 xwindow  = priv->xwindow;
  gboolean               full     = FALSE;

  if (xwindow == meta_screen_get_xroot (screen) ||
      xwindow == clutter_x11_get_stage_window (CLUTTER_STAGE (info->stage)))
    return;

  meta_error_trap_push (display);

  if (priv->back_pixmap == None)
    {
      gint pxm_width, pxm_height;
      XWindowAttributes attr;

      meta_error_trap_push (display);

      XGrabServer (xdisplay);

      XGetWindowAttributes (xdisplay, xwindow, &attr);

      if (attr.map_state == IsViewable)
	priv->back_pixmap = XCompositeNameWindowPixmap (xdisplay, xwindow);
      else
	{
	  priv->back_pixmap = None;
	}

      XUngrabServer (xdisplay);
      meta_error_trap_pop (display, FALSE);

      if (priv->back_pixmap == None)
        {
          meta_verbose ("Unable to get named pixmap for %p\n", cw);
          return;
        }

      clutter_x11_texture_pixmap_set_pixmap
                       (CLUTTER_X11_TEXTURE_PIXMAP (priv->actor),
                        priv->back_pixmap);

      g_object_get (priv->actor,
                    "pixmap-width", &pxm_width,
                    "pixmap-height", &pxm_height,
                    NULL);

      clutter_actor_set_size (priv->actor, pxm_width, pxm_height);

      if (priv->shadow)
        clutter_actor_set_size (priv->shadow, pxm_width, pxm_height);

      full = TRUE;
    }

  /*
   * TODO -- on some gfx hardware updating the whole texture instead of
   * the individual rectangles is actually quicker, so we might want to
   * make this a configurable option (on desktop HW with multiple pipelines
   * it is usually quicker to just update the damaged parts).
   *
   * If we are using TFP we update the whole texture (this simply trigers
   * the texture rebind).
   */
  if (full
#ifdef HAVE_GLX_TEXTURE_PIXMAP
      || (CLUTTER_GLX_IS_TEXTURE_PIXMAP (priv->actor) &&
          clutter_glx_texture_pixmap_using_extension
                  (CLUTTER_GLX_TEXTURE_PIXMAP (priv->actor)))
#endif /* HAVE_GLX_TEXTURE_PIXMAP */
      )
    {
      XDamageSubtract (xdisplay, priv->damage, None, None);

      clutter_x11_texture_pixmap_update_area
	(CLUTTER_X11_TEXTURE_PIXMAP (priv->actor),
	 0,
	 0,
	 clutter_actor_get_width (priv->actor),
	 clutter_actor_get_height (priv->actor));
    }
  else
    {
      XRectangle   *r_damage;
      XRectangle    r_bounds;
      XserverRegion parts;
      int           i, r_count;

      parts = XFixesCreateRegion (xdisplay, 0, 0);
      XDamageSubtract (xdisplay, priv->damage, None, parts);

      r_damage = XFixesFetchRegionAndBounds (xdisplay,
					     parts,
					     &r_count,
					     &r_bounds);

      if (r_damage)
	{
	  for (i = 0; i < r_count; ++i)
	    {
	      clutter_x11_texture_pixmap_update_area
		(CLUTTER_X11_TEXTURE_PIXMAP (priv->actor),
		 r_damage[i].x,
		 r_damage[i].y,
		 r_damage[i].width,
		 r_damage[i].height);
	    }
	}

      XFree (r_damage);
      XFixesDestroyRegion (xdisplay, parts);
    }

  meta_error_trap_pop (display, FALSE);

  priv->needs_repair = FALSE;
}


static void
process_create (MetaCompositorClutter *compositor,
                XCreateWindowEvent    *event,
                MetaWindow            *window)
{
  MetaScreen *screen;

  screen = meta_display_screen_for_root (compositor->display, event->parent);

  if (screen == NULL)
    return;

  /*
   * This is quite silly as we end up creating windows as then immediatly
   * destroying them as they (likely) become framed and thus reparented.
   */
  if (!find_window_in_display (compositor->display, event->window))
    add_win (screen, window, event->window);
}

static void
process_reparent (MetaCompositorClutter *compositor,
                  XReparentEvent        *event,
                  MetaWindow            *window)
{
  MetaScreen *screen;

  screen = meta_display_screen_for_root (compositor->display, event->parent);

  if (screen != NULL)
    {
      meta_verbose ("reparent: adding a new window 0x%x\n",
		    (guint)event->window);
      add_win (screen, window, event->window);
    }
  else
    {
      meta_verbose ("reparent: destroying a window 0%x\n",
		    (guint)event->window);
      destroy_win (compositor->display, event->window);
    }
}

static void
process_destroy (MetaCompositorClutter *compositor,
                 XDestroyWindowEvent   *event)
{
  destroy_win (compositor->display, event->window);
}

static void
process_damage (MetaCompositorClutter *compositor,
                XDamageNotifyEvent    *event)
{
  XEvent   next;
  Display *dpy = event->display;
  Drawable drawable = event->drawable;
  MetaCompWindowPrivate *priv;
  MetaCompWindow *cw = find_window_in_display (compositor->display, drawable);

  if (!cw)
    return;

  priv = cw->priv;

  if (priv->destroy_pending        ||
      priv->maximize_in_progress   ||
      priv->unmaximize_in_progress)
    {
      priv->needs_repair = TRUE;
      return;
    }

  /*
   * Check if the event queue does not already contain DetstroyNotify for this
   * window -- if it does, we need to stop updating the pixmap (to avoid damage
   * notifications that come from the window teardown), and process the destroy
   * immediately.
   */
  if (XCheckTypedWindowEvent (dpy, drawable, DestroyNotify, &next))
    {
      priv->destroy_pending = TRUE;
      process_destroy (compositor, (XDestroyWindowEvent *) &next);
      return;
    }

  repair_win (cw);
}

static void
update_shape (MetaCompositorClutter *compositor,
              MetaCompWindow *cw)
{
  MetaCompWindowPrivate *priv = cw->priv;

  meta_shaped_texture_clear_rectangles (META_SHAPED_TEXTURE (priv->actor));

#ifdef HAVE_SHAPE
  if (priv->shaped)
    {
      Display *xdisplay = meta_display_get_xdisplay (compositor->display);
      XRectangle *rects;
      int n_rects, ordering;

      rects = XShapeGetRectangles (xdisplay,
                                   priv->xwindow,
                                   ShapeBounding,
                                   &n_rects,
                                   &ordering);

      if (rects)
        {
          meta_shaped_texture_add_rectangles (META_SHAPED_TEXTURE (priv->actor),
                                              n_rects, rects);

          XFree (rects);
        }
    }
#endif
}

#ifdef HAVE_SHAPE
static void
process_shape (MetaCompositorClutter *compositor,
               XShapeEvent           *event)
{
  MetaCompWindow *cw = find_window_in_display (compositor->display,
                                               event->window);
  MetaCompWindowPrivate *priv = cw->priv;

  if (cw == NULL)
    return;

  if (event->kind == ShapeBounding)
    {
      priv->shaped = event->shaped;
      update_shape (compositor, cw);
    }
}
#endif

static void
process_configure_notify (MetaCompositorClutter  *compositor,
                          XConfigureEvent        *event)
{
  MetaDisplay *display = compositor->display;
  MetaCompWindow *cw = find_window_in_display (display, event->window);

  if (cw)
    {
      restack_win (cw, event->above);
      resize_win (cw,
                  event->x, event->y, event->width, event->height,
                  event->border_width, event->override_redirect);
    }
  else
    {
      /*
       * Check for root window geometry change
       */
      GSList *l = meta_display_get_screens (display);

      while (l)
	{
	  MetaScreen *screen = l->data;
	  Window      xroot  = meta_screen_get_xroot (screen);

	  if (event->window == xroot)
	    {
	      gint            width;
	      gint            height;
	      MetaCompScreen *info = meta_screen_get_compositor_data (screen);

	      meta_screen_get_size (screen, &width, &height);
	      clutter_actor_set_size (info->stage, width, height);

	      meta_verbose ("Changed size for stage on screen %d to %dx%d\n",
			    meta_screen_get_screen_number (screen),
			    width, height);
	      break;
	    }

	  l = l->next;
	}
    }
}

static void
process_circulate_notify (MetaCompositorClutter  *compositor,
                          XCirculateEvent        *event)
{
  MetaCompWindow *cw = find_window_in_display (compositor->display,
                                               event->window);
  MetaCompWindow *top;
  MetaCompScreen *info;
  Window          above;
  MetaCompWindowPrivate *priv;

  if (!cw)
    return;

  priv = cw->priv;

  info   = meta_screen_get_compositor_data (priv->screen);
  top    = info->windows->data;

  if ((event->place == PlaceOnTop) && top)
    above = top->priv->xwindow;
  else
    above = None;

  restack_win (cw, above);
}

static void
process_unmap (MetaCompositorClutter *compositor,
               XUnmapEvent           *event)
{
  MetaCompWindow *cw;
  Window          xwin = event->window;
  Display        *dpy = event->display;

  if (event->from_configure)
    {
      /* Ignore unmap caused by parent's resize */
      return;
    }

  cw = find_window_in_display (compositor->display, xwin);

  if (cw)
    {
      XEvent next;
      MetaCompWindowPrivate *priv = cw->priv;

      if (priv->attrs.map_state == IsUnmapped || priv->destroy_pending)
	return;

      if (XCheckTypedWindowEvent (dpy, xwin, DestroyNotify, &next))
	{
	  priv->destroy_pending = TRUE;
	  process_destroy (compositor, (XDestroyWindowEvent *) &next);
	  return;
	}

      meta_verbose ("processing unmap  of 0x%x (%p)\n", (guint)xwin, cw);
      unmap_win (cw);
    }
}

static void
process_map (MetaCompositorClutter *compositor,
             XMapEvent             *event,
             MetaWindow            *window)
{
  MetaCompWindow *cw = find_window_in_display (compositor->display,
                                               event->window);

  if (cw)
    map_win (cw);
}

static void
process_property_notify (MetaCompositorClutter *compositor,
                         XPropertyEvent        *event)
{
  MetaDisplay *display = compositor->display;

  /* Check for the opacity changing */
  if (event->atom == compositor->atom_net_wm_window_opacity)
    {
      MetaCompWindow *cw = find_window_in_display (display, event->window);
      gulong          value;

      if (!cw)
        {
          /* Applications can set this for their toplevel windows, so
           * this must be propagated to the window managed by the compositor
           */
          cw = find_window_for_child_window_in_display (display,
                                                        event->window);
        }

      if (!cw)
        return;

      if (meta_prop_get_cardinal (display, event->window,
                                  compositor->atom_net_wm_window_opacity,
                                  &value) == FALSE)
	{
	  guint8 opacity;

	  opacity = (guint8)((gfloat)value * 255.0 / ((gfloat)0xffffffff));

	  cw->priv->opacity = opacity;
	  clutter_actor_set_opacity (CLUTTER_ACTOR (cw), opacity);
	}

      return;
    }
  else if (event->atom == meta_display_get_atom (display,
					       META_ATOM__NET_WM_WINDOW_TYPE))
    {
      MetaCompWindow *cw = find_window_in_display (display, event->window);

      if (!cw)
        return;

      meta_comp_window_query_window_type (cw);
      return;
    }
}

static void
show_overlay_window (MetaScreen *screen, Window cow)
{
  MetaDisplay   *display  = meta_screen_get_display (screen);
  Display       *xdisplay = meta_display_get_xdisplay (display);
  XserverRegion  region;

  region = XFixesCreateRegion (xdisplay, NULL, 0);

  XFixesSetWindowShapeRegion (xdisplay, cow, ShapeBounding, 0, 0, 0);
  XFixesSetWindowShapeRegion (xdisplay, cow, ShapeInput, 0, 0, region);

  XFixesDestroyRegion (xdisplay, region);
}

static Window
get_output_window (MetaScreen *screen)
{
  MetaDisplay *display = meta_screen_get_display (screen);
  Display     *xdisplay = meta_display_get_xdisplay (display);
  Window       output, xroot;

  xroot = meta_screen_get_xroot (screen);

  output = XCompositeGetOverlayWindow (xdisplay, xroot);
  XSelectInput (xdisplay, output, ExposureMask);

  return output;
}

ClutterActor *
meta_compositor_clutter_get_stage_for_screen (MetaScreen *screen)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  if (!info)
    return NULL;

  return info->stage;
}

ClutterActor *
meta_compositor_clutter_get_overlay_group_for_screen (MetaScreen *screen)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  if (!info)
    return NULL;

  return info->overlay_group;
}


static void
clutter_cmp_manage_screen (MetaCompositor *compositor,
                           MetaScreen     *screen)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompScreen *info;
  MetaDisplay    *display       = meta_screen_get_display (screen);
  Display        *xdisplay      = meta_display_get_xdisplay (display);
  int             screen_number = meta_screen_get_screen_number (screen);
  Window          xroot         = meta_screen_get_xroot (screen);
  Window          xwin;
  gint            width, height;

  /* Check if the screen is already managed */
  if (meta_screen_get_compositor_data (screen))
    return;

  meta_error_trap_push_with_return (display);
  XCompositeRedirectSubwindows (xdisplay, xroot, CompositeRedirectManual);
  XSync (xdisplay, FALSE);

  if (meta_error_trap_pop_with_return (display, FALSE))
    {
      g_warning ("Another compositing manager is running on screen %i",
                 screen_number);
      return;
    }

  info = g_new0 (MetaCompScreen, 1);
  info->screen = screen;

  meta_screen_set_compositor_data (screen, info);

  info->output = get_output_window (screen);

  info->windows = NULL;
  info->windows_by_xid = g_hash_table_new (g_direct_hash, g_direct_equal);

  info->focus_window = meta_display_get_focus_window (display);

  XClearArea (xdisplay, info->output, 0, 0, 0, 0, TRUE);

  meta_screen_set_cm_selection (screen);

  info->stage = clutter_stage_get_default ();

  meta_screen_get_size (screen, &width, &height);
  clutter_actor_set_size (info->stage, width, height);

  xwin = clutter_x11_get_stage_window (CLUTTER_STAGE (info->stage));

  XReparentWindow (xdisplay, xwin, info->output, 0, 0);

  info->window_group = clutter_group_new ();
  info->overlay_group = clutter_group_new ();

  {
    ClutterActor *foo;
    foo = clutter_label_new_with_text ("Sans Bold 148", "OVERLAY");
    clutter_actor_set_opacity (foo, 100);
    clutter_container_add_actor (CLUTTER_CONTAINER (info->overlay_group),
                                 foo);
  }

  clutter_container_add (CLUTTER_CONTAINER (info->stage),
                         info->window_group,
                         info->overlay_group,
                         NULL);



  info->plugin_mgr =
    meta_compositor_clutter_plugin_manager_new (screen);

  clutter_actor_show_all (info->stage);
  clutter_actor_show_all (info->overlay_group);

  /* Now we're up and running we can show the output if needed */
  show_overlay_window (screen, info->output);
#endif
}

static void
clutter_cmp_unmanage_screen (MetaCompositor *compositor,
                             MetaScreen     *screen)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS

#endif
}

static void
clutter_cmp_add_window (MetaCompositor    *compositor,
                        MetaWindow        *window,
                        Window             xwindow,
                        XWindowAttributes *attrs)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompositorClutter *xrc = (MetaCompositorClutter *) compositor;
  MetaScreen            *screen = meta_screen_for_x_screen (attrs->screen);

  meta_error_trap_push (xrc->display);
  add_win (screen, window, xwindow);
  meta_error_trap_pop (xrc->display, FALSE);
#endif
}

static void
clutter_cmp_remove_window (MetaCompositor *compositor,
                           Window          xwindow)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS

#endif
}

static void
clutter_cmp_set_updates (MetaCompositor *compositor,
                         MetaWindow     *window,
                         gboolean        update)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS

#endif
}

static void
clutter_cmp_process_event (MetaCompositor *compositor,
                           XEvent         *event,
                           MetaWindow     *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompositorClutter *xrc = (MetaCompositorClutter *) compositor;

  if (window)
    {
      MetaCompScreen *info;
      MetaScreen     *screen;

      screen = meta_window_get_screen (window);
      info = meta_screen_get_compositor_data (screen);

      if (meta_compositor_clutter_plugin_manager_xevent_filter
                                                       (info->plugin_mgr,
                                                        event) == TRUE)
        return;
    }

  /*
   * This trap is so that none of the compositor functions cause
   * X errors. This is really a hack, but I'm afraid I don't understand
   * enough about Metacity/X to know how else you are supposed to do it
   */


  meta_error_trap_push (xrc->display);
  switch (event->type)
    {
    case CirculateNotify:
      process_circulate_notify (xrc, (XCirculateEvent *) event);
      break;

    case ConfigureNotify:
      process_configure_notify (xrc, (XConfigureEvent *) event);
      break;

    case PropertyNotify:
      process_property_notify (xrc, (XPropertyEvent *) event);
      break;

    case Expose:
      break;

    case UnmapNotify:
      process_unmap (xrc, (XUnmapEvent *) event);
      break;

    case MapNotify:
      process_map (xrc, (XMapEvent *) event, window);
      break;

    case ReparentNotify:
      process_reparent (xrc, (XReparentEvent *) event, window);
      break;

    case CreateNotify:
      process_create (xrc, (XCreateWindowEvent *) event, window);
      break;

    case DestroyNotify:
      process_destroy (xrc, (XDestroyWindowEvent *) event);
      break;

    default:
      if (event->type == meta_display_get_damage_event_base (xrc->display) + XDamageNotify)
        {
          process_damage (xrc, (XDamageNotifyEvent *) event);
        }
#ifdef HAVE_SHAPE
      else if (event->type == meta_display_get_shape_event_base (xrc->display) + ShapeNotify)
        process_shape (xrc, (XShapeEvent *) event);
#endif /* HAVE_SHAPE */
      /* else
        {
          meta_error_trap_pop (xrc->display, FALSE);
          return;
        }
      */
      break;
    }

  meta_error_trap_pop (xrc->display, FALSE);

#endif
}

static Pixmap
clutter_cmp_get_window_pixmap (MetaCompositor *compositor,
                               MetaWindow     *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  return None;
#else
  return None;
#endif
}

static void
clutter_cmp_set_active_window (MetaCompositor *compositor,
                               MetaScreen     *screen,
                               MetaWindow     *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS

#endif
}

static void
clutter_cmp_destroy_window (MetaCompositor *compositor,
                            MetaWindow     *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompWindow        *cw     = NULL;
  MetaScreen            *screen = meta_window_get_screen (window);
  MetaCompScreen        *info   = meta_screen_get_compositor_data (screen);
  MetaFrame             *f      = meta_window_get_frame (window);
  MetaCompWindowPrivate *priv;

  /* Chances are we actually get the window frame here */
  cw = find_window_for_screen (screen,
                               f ? meta_frame_get_xwindow (f) :
                               meta_window_get_xwindow (window));
  if (!cw)
    return;

  priv = cw->priv;

  /*
   * We remove the window from internal lookup hashes and thus any other
   * unmap events etc fail
   */
  info->windows = g_list_remove (info->windows, (gconstpointer) cw);
  g_hash_table_remove (info->windows_by_xid,
                       (gpointer) (f ? meta_frame_get_xwindow (f) :
                                   meta_window_get_xwindow (window)));

  /*
   * If a plugin manager is present, try to run an effect; if no effect of this
   * type is present, destroy the actor.
   */
  priv->destroy_in_progress++;

  if (!info->plugin_mgr ||
      !meta_compositor_clutter_plugin_manager_event_simple (info->plugin_mgr,
				cw,
                                META_COMPOSITOR_CLUTTER_PLUGIN_DESTROY))
    {
      priv->destroy_in_progress--;
      clutter_actor_destroy (CLUTTER_ACTOR (cw));
    }
#endif
}

static void
clutter_cmp_minimize_window (MetaCompositor *compositor, MetaWindow *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompWindow *cw;
  MetaCompScreen *info;
  MetaScreen     *screen;
  MetaFrame      *f = meta_window_get_frame (window);

  screen = meta_window_get_screen (window);
  info = meta_screen_get_compositor_data (screen);

  /* Chances are we actually get the window frame here */
  cw = find_window_for_screen (screen,
                               f ? meta_frame_get_xwindow (f) :
                               meta_window_get_xwindow (window));
  if (!cw)
    return;

  /*
   * If there is a plugin manager, try to run an effect; if no effect is
   * executed, hide the actor.
   */
  cw->priv->minimize_in_progress++;

  if (!info->plugin_mgr ||
      !meta_compositor_clutter_plugin_manager_event_simple (info->plugin_mgr,
				cw,
				META_COMPOSITOR_CLUTTER_PLUGIN_MINIMIZE))
    {
      ClutterActor *a      = CLUTTER_ACTOR (cw);
      gint          height = clutter_actor_get_height (a);

      cw->priv->is_minimized = TRUE;
      cw->priv->minimize_in_progress--;
      clutter_actor_set_position (a, 0, -height);
    }
#endif
}

static void
clutter_cmp_maximize_window (MetaCompositor *compositor, MetaWindow *window,
			     gint x, gint y, gint width, gint height)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompWindow *cw;
  MetaCompScreen *info;
  MetaScreen     *screen;
  MetaFrame      *f = meta_window_get_frame (window);

  screen = meta_window_get_screen (window);
  info = meta_screen_get_compositor_data (screen);

  /* Chances are we actually get the window frame here */
  cw = find_window_for_screen (screen,
                               f ? meta_frame_get_xwindow (f) :
                               meta_window_get_xwindow (window));
  if (!cw)
    return;

  cw->priv->maximize_in_progress++;

  if (!info->plugin_mgr ||
      !meta_compositor_clutter_plugin_manager_event_maximize (info->plugin_mgr,
				cw,
				META_COMPOSITOR_CLUTTER_PLUGIN_MAXIMIZE,
				x, y, width, height))
    {
      cw->priv->maximize_in_progress--;
    }
#endif
}

static void
clutter_cmp_unmaximize_window (MetaCompositor *compositor, MetaWindow *window,
			       gint x, gint y, gint width, gint height)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompWindow *cw;
  MetaCompScreen *info;
  MetaScreen     *screen;
  MetaFrame      *f = meta_window_get_frame (window);

  screen = meta_window_get_screen (window);
  info = meta_screen_get_compositor_data (screen);

  /* Chances are we actually get the window frame here */
  cw = find_window_for_screen (screen,
                               f ? meta_frame_get_xwindow (f) :
                               meta_window_get_xwindow (window));
  if (!cw)
    return;

  cw->priv->unmaximize_in_progress++;

  if (!info->plugin_mgr ||
      !meta_compositor_clutter_plugin_manager_event_maximize (info->plugin_mgr,
				cw,
				META_COMPOSITOR_CLUTTER_PLUGIN_UNMAXIMIZE,
				x, y, width, height))
    {
      cw->priv->unmaximize_in_progress--;
    }
#endif
}

static void
clutter_cmp_update_workspace_geometry (MetaCompositor *compositor,
				       MetaWorkspace  *workspace)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaScreen     *screen = meta_workspace_get_screen (workspace);
  MetaCompScreen *info;
  MetaCompositorClutterPluginManager *mgr;

  info = meta_screen_get_compositor_data (screen);
  mgr  = info->plugin_mgr;

  if (!mgr || !workspace)
    return;

  meta_compositor_clutter_plugin_manager_update_workspace (mgr, workspace);
#endif
}

static void
clutter_cmp_switch_workspace (MetaCompositor *compositor,
			      MetaScreen     *screen,
			      MetaWorkspace  *from,
			      MetaWorkspace  *to,
			      MetaMotionDirection direction)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompScreen *info;
  GList          *l;
  gint            to_indx, from_indx;

  info      = meta_screen_get_compositor_data (screen);
  to_indx   = meta_workspace_index (to);
  from_indx = meta_workspace_index (from);

  printf ("Direction of switch %d\n", direction);

  l = info->windows;
  while (l)
    {
      MetaCompWindow *cw = l->data;
      MetaWindow     *mw = cw->priv->window;
      gboolean        sticky;
      gint            workspace = -1;

      sticky = (!mw || meta_window_is_on_all_workspaces (mw));

      if (!sticky)
	{
	  MetaWorkspace *w;

	  w = meta_window_get_workspace (cw->priv->window);
	  workspace = meta_workspace_index (w);

	  /*
	   * If the window is not on the target workspace, mark it for
	   * unmap.
	   */
	  if (to_indx != workspace)
	    {
	      cw->priv->needs_unmap = TRUE;
	    }
	  else
	    {
	      cw->priv->needs_map = TRUE;
	      cw->priv->needs_unmap = FALSE;
	    }
	}

      l = l->next;
    }

  info->switch_workspace_in_progress++;

  if (!info->plugin_mgr ||
      !meta_compositor_clutter_plugin_manager_switch_workspace (
						info->plugin_mgr,
						(const GList **)&info->windows,
						from_indx,
						to_indx,
						direction))
    {
      info->switch_workspace_in_progress--;
    }
#endif
}


static MetaCompositor comp_info = {
  clutter_cmp_destroy,
  clutter_cmp_manage_screen,
  clutter_cmp_unmanage_screen,
  clutter_cmp_add_window,
  clutter_cmp_remove_window,
  clutter_cmp_set_updates,
  clutter_cmp_process_event,
  clutter_cmp_get_window_pixmap,
  clutter_cmp_set_active_window,
  clutter_cmp_destroy_window,
  clutter_cmp_minimize_window,
  clutter_cmp_maximize_window,
  clutter_cmp_unmaximize_window,
  clutter_cmp_update_workspace_geometry,
  clutter_cmp_switch_workspace
};

MetaCompositor *
meta_compositor_clutter_new (MetaDisplay *display)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  char *atom_names[] = {
    "_XROOTPMAP_ID",
    "_XSETROOT_ID",
    "_NET_WM_WINDOW_OPACITY",
  };
  Atom                   atoms[G_N_ELEMENTS(atom_names)];
  MetaCompositorClutter *clc;
  MetaCompositor        *compositor;
  Display               *xdisplay = meta_display_get_xdisplay (display);
  guchar                *data;

  if (!composite_at_least_version (display, 0, 3))
    return NULL;

  clc = g_new0 (MetaCompositorClutter, 1);
  clc->compositor = comp_info;

  compositor = (MetaCompositor *) clc;

  clc->display = display;

  meta_verbose ("Creating %d atoms\n", (int) G_N_ELEMENTS (atom_names));
  XInternAtoms (xdisplay, atom_names, G_N_ELEMENTS (atom_names),
                False, atoms);

  clc->atom_x_root_pixmap = atoms[0];
  clc->atom_x_set_root = atoms[1];
  clc->atom_net_wm_window_opacity = atoms[2];

  /* Shadow setup */

  data = shadow_gaussian_make_tile ();

  clc->shadow_src = clutter_texture_new ();

  clutter_texture_set_from_rgb_data (CLUTTER_TEXTURE (clc->shadow_src),
                                     data,
                                     TRUE,
                                     TILE_WIDTH,
                                     TILE_HEIGHT,
                                     TILE_WIDTH*4,
                                     4,
                                     0,
                                     NULL);
  free (data);

  return compositor;
#else
  return NULL;
#endif
}

Window
meta_compositor_clutter_get_overlay_window (MetaScreen *screen)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  return info->output;
}


/* ------------------------------- */
/* Shadow Generation */

typedef struct GaussianMap
{
  int	   size;
  double * data;
} GaussianMap;

static double
gaussian (double r, double x, double y)
{
  return ((1 / (sqrt (2 * M_PI * r))) *
	  exp ((- (x * x + y * y)) / (2 * r * r)));
}


static GaussianMap *
make_gaussian_map (double r)
{
  GaussianMap  *c;
  int	          size = ((int) ceil ((r * 3)) + 1) & ~1;
  int	          center = size / 2;
  int	          x, y;
  double          t = 0.0;
  double          g;

  c = malloc (sizeof (GaussianMap) + size * size * sizeof (double));
  c->size = size;

  c->data = (double *) (c + 1);

  for (y = 0; y < size; y++)
    for (x = 0; x < size; x++)
      {
	g = gaussian (r, (double) (x - center), (double) (y - center));
	t += g;
	c->data[y * size + x] = g;
      }

  for (y = 0; y < size; y++)
    for (x = 0; x < size; x++)
      c->data[y*size + x] /= t;

  return c;
}

static unsigned char
sum_gaussian (GaussianMap * map, double opacity,
              int x, int y, int width, int height)
{
  int	           fx, fy;
  double         * g_data;
  double         * g_line = map->data;
  int	           g_size = map->size;
  int	           center = g_size / 2;
  int	           fx_start, fx_end;
  int	           fy_start, fy_end;
  double           v;
  unsigned int     r;

  /*
   * Compute set of filter values which are "in range",
   * that's the set with:
   *	0 <= x + (fx-center) && x + (fx-center) < width &&
   *  0 <= y + (fy-center) && y + (fy-center) < height
   *
   *  0 <= x + (fx - center)	x + fx - center < width
   *  center - x <= fx	fx < width + center - x
   */

  fx_start = center - x;
  if (fx_start < 0)
    fx_start = 0;
  fx_end = width + center - x;
  if (fx_end > g_size)
    fx_end = g_size;

  fy_start = center - y;
  if (fy_start < 0)
    fy_start = 0;
  fy_end = height + center - y;
  if (fy_end > g_size)
    fy_end = g_size;

  g_line = g_line + fy_start * g_size + fx_start;

  v = 0;
  for (fy = fy_start; fy < fy_end; fy++)
    {
      g_data = g_line;
      g_line += g_size;

      for (fx = fx_start; fx < fx_end; fx++)
	v += *g_data++;
    }
  if (v > 1)
    v = 1;

  v *= (opacity * 255.0);

  r = (unsigned int) v;

  return (unsigned char) r;
}

static unsigned char *
shadow_gaussian_make_tile ()
{
  unsigned char              * data;
  int		               size;
  int		               center;
  int		               x, y;
  unsigned char                d;
  int                          pwidth, pheight;
  double                       opacity = SHADOW_OPACITY;
  static GaussianMap       * gaussian_map = NULL;

  struct _mypixel
  {
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned char a;
  } * _d;


  if (!gaussian_map)
    gaussian_map =
      make_gaussian_map (SHADOW_RADIUS);

  size   = gaussian_map->size;
  center = size / 2;

  /* Top & bottom */

  pwidth  = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  data = g_malloc0 (4 * TILE_WIDTH * TILE_HEIGHT);

  _d = (struct _mypixel*) data;

  /* N */
  for (y = 0; y < pheight; y++)
    {
      d = sum_gaussian (gaussian_map, opacity,
                        center, y - center,
                        TILE_WIDTH, TILE_HEIGHT);
      for (x = 0; x < pwidth; x++)
	{
	  _d[y*3*pwidth + x + pwidth].r = 0;
	  _d[y*3*pwidth + x + pwidth].g = 0;
	  _d[y*3*pwidth + x + pwidth].b = 0;
	  _d[y*3*pwidth + x + pwidth].a = d;
	}

    }

  /* S */
  pwidth = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  for (y = 0; y < pheight; y++)
    {
      d = sum_gaussian (gaussian_map, opacity,
                        center, y - center,
                        TILE_WIDTH, TILE_HEIGHT);
      for (x = 0; x < pwidth; x++)
	{
	  _d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x + pwidth].r = 0;
	  _d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x + pwidth].g = 0;
	  _d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x + pwidth].b = 0;
	  _d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x + pwidth].a = d;
	}

    }


  /* w */
  pwidth = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  for (x = 0; x < pwidth; x++)
    {
      d = sum_gaussian (gaussian_map, opacity,
                        x - center, center,
                        TILE_WIDTH, TILE_HEIGHT);
      for (y = 0; y < pheight; y++)
	{
	  _d[y*3*pwidth + 3*pwidth*pheight + x].r = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + x].g = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + x].b = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + x].a = d;
	}

    }

  /* E */
  for (x = 0; x < pwidth; x++)
    {
      d = sum_gaussian (gaussian_map, opacity,
					       x - center, center,
					       TILE_WIDTH, TILE_HEIGHT);
      for (y = 0; y < pheight; y++)
	{
	  _d[y*3*pwidth + 3*pwidth*pheight + (pwidth-x-1) + 2*pwidth].r = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + (pwidth-x-1) + 2*pwidth].g = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + (pwidth-x-1) + 2*pwidth].b = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + (pwidth-x-1) + 2*pwidth].a = d;
	}

    }

  /* NW */
  pwidth = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	d = sum_gaussian (gaussian_map, opacity,
                          x-center, y-center,
                          TILE_WIDTH, TILE_HEIGHT);

	_d[y*3*pwidth + x].r = 0;
	_d[y*3*pwidth + x].g = 0;
	_d[y*3*pwidth + x].b = 0;
	_d[y*3*pwidth + x].a = d;
      }

  /* SW */
  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	d = sum_gaussian (gaussian_map, opacity,
                          x-center, y-center,
                          TILE_WIDTH, TILE_HEIGHT);

	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x].r = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x].g = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x].b = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x].a = d;
      }

  /* SE */
  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	d = sum_gaussian (gaussian_map, opacity,
                          x-center, y-center,
                          TILE_WIDTH, TILE_HEIGHT);

	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + (pwidth-x-1) +
	   2*pwidth].r = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + (pwidth-x-1) +
	   2*pwidth].g = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + (pwidth-x-1) +
	   2*pwidth].b = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + (pwidth-x-1) +
	   2*pwidth].a = d;
      }

  /* NE */
  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	d = sum_gaussian (gaussian_map, opacity,
                          x-center, y-center,
                          TILE_WIDTH, TILE_HEIGHT);

	_d[y*3*pwidth + (pwidth - x - 1) + 2*pwidth].r = 0;
	_d[y*3*pwidth + (pwidth - x - 1) + 2*pwidth].g = 0;
	_d[y*3*pwidth + (pwidth - x - 1) + 2*pwidth].b = 0;
	_d[y*3*pwidth + (pwidth - x - 1) + 2*pwidth].a = d;
      }

  /* center */
  pwidth = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  d = sum_gaussian (gaussian_map, opacity,
                    center, center, TILE_WIDTH, TILE_HEIGHT);

  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	_d[y*3*pwidth + 3*pwidth*pheight + x + pwidth].r = 0;
	_d[y*3*pwidth + 3*pwidth*pheight + x + pwidth].g = 0;
	_d[y*3*pwidth + 3*pwidth*pheight + x + pwidth].b = 0;
	_d[y*3*pwidth + 3*pwidth*pheight + x + pwidth].a = d;
      }

  return data;
}


