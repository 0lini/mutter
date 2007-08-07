
/* -*- mode:C; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Authored By Tomas Frydrych  <tf@openedhand.com>
 *
 * Copyright (C) 2007 OpenedHand
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

/**
 * SECTION:clutter-behaviour-ellipse
 * @short_description: elliptic path behaviour.
 *
 * #ClutterBehaviourEllipse interpolates actors along a path defined by
 *  an ellipse.
 *
 * Note, on applying an ellipse behaviour to an actor its position will
 * be set to what is dictated by the ellipses initial position.
 *
 * Since: 0.4
 */

#include "clutter-fixed.h"
#include "clutter-marshal.h"
#include "clutter-behaviour-ellipse.h"
#include "clutter-debug.h"
#include "clutter-enum-types.h"
#include "clutter-private.h"

#include <stdlib.h>
#include <memory.h>

/****************************************************************************
 *                                                                          *
 * ClutterBehaviourEllipse                                                  *
 *                                                                          *
 ****************************************************************************/

G_DEFINE_TYPE (ClutterBehaviourEllipse,
               clutter_behaviour_ellipse,
	       CLUTTER_TYPE_BEHAVIOUR);

#define CLUTTER_BEHAVIOUR_ELLIPSE_GET_PRIVATE(obj)    \
              (G_TYPE_INSTANCE_GET_PRIVATE ((obj),    \
               CLUTTER_TYPE_BEHAVIOUR_ELLIPSE,        \
               ClutterBehaviourEllipsePrivate))

enum
{
  PROP_0,

  PROP_CENTER,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_ANGLE_BEGIN,
  PROP_ANGLE_END,
  PROP_ANGLE_TILT_X,
  PROP_ANGLE_TILT_Y,
  PROP_ANGLE_TILT_Z,
  PROP_DIRECTION,
};

struct _ClutterBehaviourEllipsePrivate
{
  ClutterKnot  center;

  gint         a;
  gint         b;

  ClutterAngle angle_begin;
  ClutterAngle angle_end;
  ClutterAngle angle_tilt_x;
  ClutterAngle angle_tilt_y;
  ClutterAngle angle_tilt_z;

  ClutterRotateDirection direction;
};

typedef struct _knot3d
{
  gint x;
  gint y;
  gint z;
} knot3d;

static void
clutter_behaviour_ellipse_advance (ClutterBehaviourEllipse *e,
                                   ClutterAngle             angle,
                                   knot3d                  *knot)
{
  ClutterBehaviourEllipsePrivate *priv = e->priv;
  gint x, y, z;

  x = CLUTTER_FIXED_INT (priv->a * clutter_cosi (angle));
  y = CLUTTER_FIXED_INT (priv->b * clutter_sini (angle));
  z = 0;

  if (priv->angle_tilt_z)
    {
      /*
       * x2 = r * cos (angle + tilt_z)
       * y2 = r * sin (angle + tilt_z)
       *
       * These can be trasformed to the formulas below using properties of
       * sin (a + b) and cos (a + b)
       *
       */
      ClutterFixed x2, y2;

      x2 = x * clutter_cosi (priv->angle_tilt_z)
           - y * clutter_sini (priv->angle_tilt_z);

      y2 = y * clutter_cosi (priv->angle_tilt_z)
           + x * clutter_sini (priv->angle_tilt_z);

      x = CLUTTER_FIXED_INT (x2);
      y = CLUTTER_FIXED_INT (y2);
    }

  if (priv->angle_tilt_x)
    {
      ClutterFixed z2, y2;

      z2 = - y * clutter_sini (priv->angle_tilt_x);

      y2 = y * clutter_cosi (priv->angle_tilt_x);

      z = CLUTTER_FIXED_INT (z2);
      y = CLUTTER_FIXED_INT (y2);
    }

  if (priv->angle_tilt_y)
    {
      ClutterFixed x2, z2;

      x2 = x * clutter_cosi (priv->angle_tilt_y)
        - z * clutter_sini (priv->angle_tilt_y);

      z2 = z * clutter_cosi (priv->angle_tilt_y)
        + x * clutter_sini (priv->angle_tilt_y);

      x = CLUTTER_FIXED_INT (x2);
      z = CLUTTER_FIXED_INT (z2);
    }

  knot->x = x;
  knot->y = y;
  knot->z = z;

  CLUTTER_NOTE (BEHAVIOUR, "advancing to angle %d [%d, %d] (a: %d, b: %d)",
                angle,
                knot->x, knot->y,
                priv->a, priv->b);
}


static void
actor_apply_knot_foreach (ClutterBehaviour *behave,
                          ClutterActor     *actor,
                          gpointer          data)
{
  knot3d *knot = data;

  clutter_actor_set_position (actor, knot->x, knot->y);
  clutter_actor_set_depth (actor, knot->z);
}

static void
clutter_behaviour_ellipse_alpha_notify (ClutterBehaviour *behave,
                                        guint32           alpha)
{
  ClutterBehaviourEllipse *self = CLUTTER_BEHAVIOUR_ELLIPSE (behave);
  ClutterBehaviourEllipsePrivate *priv = self->priv;
  knot3d knot;
  ClutterAngle angle = 0;

  if ((priv->angle_end >= priv->angle_begin &&
       priv->direction == CLUTTER_ROTATE_CW) ||
      (priv->angle_end < priv->angle_begin &&
       priv->direction == CLUTTER_ROTATE_CCW))
    {
      angle = (priv->angle_end - priv->angle_begin) * alpha
              / CLUTTER_ALPHA_MAX_ALPHA
              + priv->angle_begin;
    }
  else if (priv->angle_end >= priv->angle_begin &&
           priv->direction == CLUTTER_ROTATE_CCW)
    {
      ClutterAngle diff;

      /* Work out the angular length of the arch represented by the
       * end angle in CCW direction
       */
      if (priv->angle_end > 1024)
        {
          gint rounds = priv->angle_end / 1024;
          ClutterAngle a1 = rounds * 1024;
          ClutterAngle a2 = 1024 - (priv->angle_end - a1);

          diff = a1 + a2 + priv->angle_begin;
        }
      else
        {
          diff = 1024 - priv->angle_end + priv->angle_begin;
        }
      
      angle = priv->angle_begin - (diff * alpha / CLUTTER_ALPHA_MAX_ALPHA);
    }
  else if (priv->angle_end < priv->angle_begin &&
           priv->direction == CLUTTER_ROTATE_CW)
    {
      ClutterAngle diff;

      /* Work out the angular length of the arch represented by the
       * begin angle in CW direction
       */
      if (priv->angle_begin > 1024)
        {
          gint rounds = priv->angle_begin/ 1024;
          ClutterAngle a1 = rounds * 1024;
          ClutterAngle a2 = 1024 - (priv->angle_begin - a1);

          diff = a1 + a2 + priv->angle_end;
        }
      else
        {
          diff = 1024 - priv->angle_begin + priv->angle_end;
        }
      
      angle = priv->angle_begin + (diff * alpha / CLUTTER_ALPHA_MAX_ALPHA);
    }
  
  clutter_behaviour_ellipse_advance (self, angle, &knot);

  knot.x += priv->center.x;
  knot.y += priv->center.y;

  clutter_behaviour_actors_foreach (behave, actor_apply_knot_foreach, &knot);
}

static void
clutter_behaviour_ellipse_set_property (GObject      *gobject,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ClutterBehaviourEllipse *el = CLUTTER_BEHAVIOUR_ELLIPSE (gobject);
  ClutterBehaviourEllipsePrivate *priv = el->priv;

  switch (prop_id)
    {
    case PROP_ANGLE_BEGIN:
      priv->angle_begin =
        CLUTTER_ANGLE_FROM_DEG (g_value_get_double (value)) - 256;
      break;
    case PROP_ANGLE_END:
      priv->angle_end =
        CLUTTER_ANGLE_FROM_DEG (g_value_get_double (value)) - 256;
      break;
    case PROP_ANGLE_TILT_X:
      priv->angle_tilt_x =
        CLUTTER_ANGLE_FROM_DEG (g_value_get_double (value)) - 256;
      break;
    case PROP_ANGLE_TILT_Y:
      priv->angle_tilt_y =
        CLUTTER_ANGLE_FROM_DEG (g_value_get_double (value)) - 256;
      break;
    case PROP_ANGLE_TILT_Z:
      priv->angle_tilt_z =
        CLUTTER_ANGLE_FROM_DEG (g_value_get_double (value)) - 256;
      break;
    case PROP_WIDTH:
      priv->a = g_value_get_int (value) >> 1;
      break;
    case PROP_HEIGHT:
      priv->b = g_value_get_int (value) >> 1;
      break;
    case PROP_CENTER:
      {
        ClutterKnot *knot = g_value_get_boxed (value);
        if (knot)
          clutter_behaviour_ellipse_set_center (el, knot->x, knot->y);
      }
      break;
    case PROP_DIRECTION:
      priv->direction = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_behaviour_ellipse_get_property (GObject    *gobject,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ClutterBehaviourEllipsePrivate *priv;

  priv = CLUTTER_BEHAVIOUR_ELLIPSE (gobject)->priv;

  switch (prop_id)
    {
    case PROP_ANGLE_BEGIN:
      g_value_set_double (value,
                          CLUTTER_ANGLE_TO_DEG (priv->angle_begin + 256));
      break;
    case PROP_ANGLE_END:
      g_value_set_double (value,
                          CLUTTER_ANGLE_TO_DEG (priv->angle_end + 256));
      break;
    case PROP_ANGLE_TILT_X:
      g_value_set_double (value,
                          CLUTTER_ANGLE_TO_DEG (priv->angle_tilt_x + 256));
      break;
    case PROP_ANGLE_TILT_Y:
      g_value_set_double (value,
                          CLUTTER_ANGLE_TO_DEG (priv->angle_tilt_y + 256));
      break;
    case PROP_ANGLE_TILT_Z:
      g_value_set_double (value,
                          CLUTTER_ANGLE_TO_DEG (priv->angle_tilt_z + 256));
      break;
    case PROP_WIDTH:
      g_value_set_int (value, (priv->a << 1));
      break;
    case PROP_HEIGHT:
      g_value_set_int (value, (priv->b << 1));
      break;
    case PROP_CENTER:
      g_value_set_boxed (value, &priv->center);
      break;
    case PROP_DIRECTION:
      g_value_set_enum (value, priv->direction);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_behaviour_ellipse_applied (ClutterBehaviour *behave,
                                   ClutterActor     *actor)
{
  ClutterBehaviourEllipse *e = CLUTTER_BEHAVIOUR_ELLIPSE (behave);
  knot3d knot;

  clutter_behaviour_ellipse_advance (e, e->priv->angle_begin, &knot);

  clutter_actor_set_position (actor, knot.x, knot.y);
  clutter_actor_set_depth (actor, knot.z);

#if 0
  /* no need to chain up: ClutterBehaviourEllipse's parent class does
   * not have a class closure for ::apply
   */
  if (CLUTTER_BEHAVIOUR_CLASS (clutter_behaviour_ellipse_parent_class)->apply)
    CLUTTER_BEHAVIOUR_CLASS (clutter_behaviour_ellipse_parent_class)->apply (behave, actor);
#endif
}

static void
clutter_behaviour_ellipse_class_init (ClutterBehaviourEllipseClass *klass)
{
  GObjectClass          *object_class = G_OBJECT_CLASS (klass);
  ClutterBehaviourClass *behave_class = CLUTTER_BEHAVIOUR_CLASS (klass);

  object_class->set_property = clutter_behaviour_ellipse_set_property;
  object_class->get_property = clutter_behaviour_ellipse_get_property;

  behave_class->alpha_notify = clutter_behaviour_ellipse_alpha_notify;
  behave_class->applied = clutter_behaviour_ellipse_applied;

  /**
   * ClutterBehaviourEllipse:angle-begin:
   *
   * The initial angle from where the rotation should begin.
   *
   * Since: 0.4
   */
  g_object_class_install_property (object_class,
                                   PROP_ANGLE_BEGIN,
                                   g_param_spec_double ("angle-begin",
                                                        "Angle Begin",
                                                        "Initial angle",
                                                        0.0,
                                                        CLUTTER_ANGLE_MAX_DEG,
                                                        0.0,
                                                        CLUTTER_PARAM_READWRITE));
  /**
   * ClutterBehaviourEllipse:angle-end:
   *
   * The final angle to where the rotation should end.
   *
   * Since: 0.4
   */
  g_object_class_install_property (object_class,
                                   PROP_ANGLE_END,
                                   g_param_spec_double ("angle-end",
                                                        "Angle End",
                                                        "Final angle",
                                                        0.0,
                                                        CLUTTER_ANGLE_MAX_DEG,
                                                        360.0,
                                                        CLUTTER_PARAM_READWRITE));
  /**
   * ClutterBehaviourEllipse:angle-tilt-x:
   *
   * The tilt angle for the rotation around center in x axis
   *
   * Since: 0.4
   */
  g_object_class_install_property (object_class,
                                   PROP_ANGLE_TILT_X,
                                   g_param_spec_double ("angle-tilt-x",
                                                        "Angle x tilt",
                                                        "Tilt of the ellipse around x axis",
                                                        0.0, 360.0, 360.0,
                                                        CLUTTER_PARAM_READWRITE));
  /**
   * ClutterBehaviourEllipse:angle-tilt-y:
   *
   * The tilt angle for the rotation around center in y axis
   *
   * Since: 0.4
   */
  g_object_class_install_property (object_class,
                                   PROP_ANGLE_TILT_Y,
                                   g_param_spec_double ("angle-tilt-y",
                                                        "Angle y tilt",
                                                        "Tilt of the ellipse around y axis",
                                                        0.0, 360.0, 360.0,
                                                        CLUTTER_PARAM_READWRITE));
  /**
   * ClutterBehaviourEllipse:angle-tilt-z:
   *
   * The tilt_z angle for the rotation
   *
   * Since: 0.4
   */
  g_object_class_install_property (object_class,
                                   PROP_ANGLE_TILT_Z,
                                   g_param_spec_double ("angle-tilt-z",
                                                        "Angle z tilt",
                                                        "Tilt of the ellipse around z axis",
                                                        0.0, 360.0, 360.0,
                                                        CLUTTER_PARAM_READWRITE));
  /**
   * ClutterBehaviourEllipse:width:
   *
   * Width of the ellipse.
   *
   * Since: 0.4
   */
  g_object_class_install_property (object_class,
                                   PROP_WIDTH,
                                   g_param_spec_int ("width",
                                                     "Width",
                                                     "Width of ellipse",
                                                     0, G_MAXINT, 100,
                                                     CLUTTER_PARAM_READWRITE));
  /**
   * ClutterBehaviourEllipse:height:
   *
   * Height of the ellipse.
   *
   * Since: 0.4
   */
  g_object_class_install_property (object_class,
                                   PROP_HEIGHT,
                                   g_param_spec_int ("height",
                                                     "Height",
                                                     "Height of ellipse",
                                                     0, G_MAXINT, 50,
                                                     CLUTTER_PARAM_READWRITE));
  /**
   * ClutterBehaviourEllipse:center:
   *
   * The center of the ellipse.
   *
   * Since: 0.4
   */
  g_object_class_install_property (object_class,
                                   PROP_CENTER,
                                   g_param_spec_boxed ("center",
                                                       "Center",
                                                       "Center of ellipse",
                                                       CLUTTER_TYPE_KNOT,
                                                       CLUTTER_PARAM_READWRITE));

  /**
   * ClutterBehaviourEllipse:direction:
   *
   * The direction of the rotation.
   *
   * Since: 0.4
   */
  g_object_class_install_property (object_class,
                                   PROP_DIRECTION,
                                   g_param_spec_enum ("direction",
                                                      "Direction",
                                                      "Direction of rotation",
                                                      CLUTTER_TYPE_ROTATE_DIRECTION,
                                                      CLUTTER_ROTATE_CW,
                                                      CLUTTER_PARAM_READWRITE));
  g_type_class_add_private (klass, sizeof (ClutterBehaviourEllipsePrivate));
}

static void
clutter_behaviour_ellipse_init (ClutterBehaviourEllipse * self)
{
  ClutterBehaviourEllipsePrivate *priv;

  self->priv = priv = CLUTTER_BEHAVIOUR_ELLIPSE_GET_PRIVATE (self);

  priv->direction = CLUTTER_ROTATE_CW;
}

/**
 * clutter_behaviour_ellipse_new:
 * @alpha: a #ClutterAlpha, or %NULL
 * @x: x coordinace of the center
 * @y: y coordiance of the center
 * @width: width of the ellipse
 * @height: height of the ellipse
 * @direction: #ClutterRotateDirection of rotation
 * @begin: angle in degrees at which movement begins
 * @end: angle in degrees at which movement ends
 *
 * Creates a behaviour that drives actors along an elliptical path with
 * given center, width and height; the movement begins at @angle_begin
 * degrees (with 0 corresponding to 12 o'clock) and ends at @angle_end degrees;
 * Return value: the newly created #ClutterBehaviourEllipse
 *
 * Since: 0.4
 */
ClutterBehaviour *
clutter_behaviour_ellipse_new (ClutterAlpha          *alpha,
                               gint                   x,
                               gint                   y,
                               gint                   width,
                               gint                   height,
                               ClutterRotateDirection direction,
                               gdouble                begin,
                               gdouble                end)
{
  ClutterKnot center;

  g_return_val_if_fail (alpha == NULL || CLUTTER_IS_ALPHA (alpha), NULL);

  center.x = x;
  center.y = y;

  return g_object_new (CLUTTER_TYPE_BEHAVIOUR_ELLIPSE,
                       "alpha", alpha,
                       "center", &center,
                       "width", width,
                       "height", height,
                       "direction", direction,
                       "angle-begin", begin,
                       "angle-end", end,
                       NULL);
}

/**
 * clutter_behaviour_ellipse_newx:
 * @alpha: a #ClutterAlpha, or %NULL
 * @x: x coordinace of the center
 * @y: y coordiance of the center
 * @width: width of the ellipse
 * @height: height of the ellipse
 * @direction: #ClutterRotateDirection of rotation
 * @begin: #ClutterFixed angle in degrees at which movement begins
 * @end: #ClutterFixed angle in degrees at which movement ends
 *
 * Creates a behaviour that drives actors along an elliptical path. This
 * is the fixed point variant of clutter_behaviour_ellipse_new().
 *
 * Return value: the newly created #ClutterBehaviourEllipse
 *
 * Since: 0.4
 */
ClutterBehaviour *
clutter_behaviour_ellipse_newx (ClutterAlpha          * alpha,
                                gint                    x,
                                gint                    y,
                                gint                    width,
                                gint                    height,
                                ClutterRotateDirection  direction,
                                ClutterFixed            begin,
                                ClutterFixed            end)
{
  ClutterKnot center;

  g_return_val_if_fail (alpha == NULL || CLUTTER_IS_ALPHA (alpha), NULL);

  center.x = x;
  center.y = y;

  return g_object_new (CLUTTER_TYPE_BEHAVIOUR_ELLIPSE,
                       "alpha", alpha,
                       "center", &center,
                       "width", width,
                       "height", height,
                       "direction", direction,
                       "angle-begin", CLUTTER_ANGLE_FROM_DEGX (begin),
                       "angle-end", CLUTTER_ANGLE_FROM_DEGX (end),
                       NULL);
}


/**
 * clutter_behaviour_ellipse_set_center
 * @self: a #ClutterBehaviourEllipse
 * @x: x coordinace of centre
 * @y: y coordinace of centre
 *
 * Sets the center of the elliptical path to the point represented by knot.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_center (ClutterBehaviourEllipse *self,
                                      gint                     x,
                                      gint                     y)
{
  ClutterBehaviourEllipsePrivate *priv;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  priv = self->priv;

  if (priv->center.x != x || priv->center.y != y)
    {
      priv->center.x = x;
      priv->center.y = y;

      g_object_notify (G_OBJECT (self), "center");
    }
}

/**
 * clutter_behaviour_ellipse_get_center
 * @self: a #ClutterBehaviourEllipse
 * @x: location to store the x coordinace of the center, or NULL
 * @y: location to store the y coordinace of the center, or NULL
 *
 * Gets the center of the elliptical path path.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_get_center (ClutterBehaviourEllipse  *self,
                                      gint                     *x,
                                      gint                     *y)
{
  ClutterBehaviourEllipsePrivate *priv;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  priv = self->priv;

  if (x)
    *x = priv->center.x;

  if (y)
    *y = priv->center.y;
}


/**
 * clutter_behaviour_ellipse_set_width
 * @self: a #ClutterBehaviourEllipse
 * @width: width of the ellipse
 *
 * Sets the width of the elliptical path.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_width (ClutterBehaviourEllipse * self,
                                     gint                      width)
{
  ClutterBehaviourEllipsePrivate *priv;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  priv = self->priv;

  if (priv->a != width >> 1)
    {
      priv->a = width >> 1;

      g_object_notify (G_OBJECT (self), "width");
    }
}

/**
 * clutter_behaviour_ellipse_get_width
 * @self: a #ClutterBehaviourEllipse
 *
 * Gets the width of the elliptical path.
 *
 * Return value: the width of the path
 *
 * Since: 0.4
 */
gint
clutter_behaviour_ellipse_get_width (ClutterBehaviourEllipse *self)
{
  g_return_val_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self), 0);

  return self->priv->a << 1;
}

/**
 * clutter_behaviour_ellipse_set_height
 * @self: a #ClutterBehaviourEllipse
 * @height: height of the ellipse
 *
 * Sets the height of the elliptical path.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_height (ClutterBehaviourEllipse *self,
                                      gint                     height)
{
  ClutterBehaviourEllipsePrivate *priv;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  priv = self->priv;

  if (priv->b != height >> 1)
    {
      priv->b = height >> 1;

      g_object_notify (G_OBJECT (self), "height");
    }
}

/**
 * clutter_behaviour_ellipse_get_height
 * @self: a #ClutterBehaviourEllipse
 *
 * Gets the height of the elliptical path.
 *
 * Return value: the height of the path
 *
 * Since: 0.4
 */
gint
clutter_behaviour_ellipse_get_height (ClutterBehaviourEllipse *self)
{
  g_return_val_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self), 0);

  return self->priv->b << 1;
}

/**
 * clutter_behaviour_ellipse_set_angle_begin
 * @self: a #ClutterBehaviourEllipse
 * @angle_begin: angle at which movement begins in degrees
 *
 * Sets the angle at which movement begins.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_angle_begin (ClutterBehaviourEllipse *self,
                                           gdouble                  angle_begin)
{
  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  clutter_behaviour_ellipse_set_angle_beginx (self,
                                              CLUTTER_ANGLE_FROM_DEG (angle_begin));
}

/**
 * clutter_behaviour_ellipse_set_angle_beginx
 * @self: a #ClutterBehaviourEllipse
 * @angle_begin: #ClutterAngle at which movement begins
 *
 * Sets the angle at which movement begins.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_angle_beginx (ClutterBehaviourEllipse *self,
                                            ClutterAngle             angle_begin)
{
  ClutterBehaviourEllipsePrivate *priv;
  ClutterAngle new_angle;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  new_angle = angle_begin - 256;

  priv = self->priv;
  if (priv->angle_begin != new_angle)
    {
      priv->angle_begin = new_angle;
      g_object_notify (G_OBJECT (self), "angle-begin");
    }
}

/**
 * clutter_behaviour_ellipse_get_angle_begin
 * @self: a #ClutterBehaviourEllipse
 *
 * Gets the angle at which movements begins.
 *
 * Return value: angle in degrees
 *
 * Since: 0.4
 */
gdouble
clutter_behaviour_ellipse_get_angle_begin (ClutterBehaviourEllipse *self)
{
  g_return_val_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self), 0.0);

  return CLUTTER_ANGLE_TO_DEG (self->priv->angle_begin + 256);
}

/**
 * clutter_behaviour_ellipse_get_angle_beginx
 * @self: a #ClutterBehaviourEllipse
 *
 * Gets the angle at which movements begins.
 *
 * Return value: a #ClutterAngle
 *
 * Since: 0.4
 */
ClutterAngle
clutter_behaviour_ellipse_get_angle_beginx (ClutterBehaviourEllipse *self)
{
  g_return_val_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self), 0);

  return self->priv->angle_begin;
}

/**
 * clutter_behaviour_ellipse_set_angle_end
 * @self: a #ClutterBehaviourEllipse
 * @angle_end: angle at which movement ends in degrees.
 *
 * Sets the angle at which movement ends.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_angle_end (ClutterBehaviourEllipse *self,
                                         gdouble                  angle_end)
{
  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  clutter_behaviour_ellipse_set_angle_endx (self,
                                            CLUTTER_ANGLE_FROM_DEG (angle_end));
}

/**
 * clutter_behaviour_ellipse_set_angle_endx
 * @self: a #ClutterBehaviourEllipse
 * @angle_end: #ClutterAngle at which movement ends
 *
 * Sets the angle at which movement ends.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_angle_endx (ClutterBehaviourEllipse *self,
                                          ClutterAngle             angle_end)
{
  ClutterBehaviourEllipsePrivate *priv;
  ClutterAngle new_angle;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  new_angle = angle_end - 256;

  priv = self->priv;

  if (priv->angle_end != new_angle)
    {
      priv->angle_end = new_angle;

      g_object_notify (G_OBJECT (self), "angle-end");
    }
}

/**
 * clutter_behaviour_ellipse_get_angle_end
 * @self: a #ClutterBehaviourEllipse
 *
 * Gets the at which movements ends.
 *
 * Return value: angle in degrees
 *
 * Since: 0.4
 */
gdouble
clutter_behaviour_ellipse_get_angle_end (ClutterBehaviourEllipse *self)
{
  g_return_val_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self), 0.0);

  return CLUTTER_ANGLE_TO_DEG (self->priv->angle_end + 256);
}

/**
 * clutter_behaviour_ellipse_get_angle_endx
 * @self: a #ClutterBehaviourEllipse
 *
 * Gets the angle at which movements ends.
 *
 * Return value: a #ClutterAngle
 *
 * Since: 0.4
 */
ClutterAngle
clutter_behaviour_ellipse_get_angle_endx (ClutterBehaviourEllipse *self)
{
  g_return_val_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self), 0);

  return self->priv->angle_end;
}

/**
 * clutter_behaviour_ellipse_set_angle_tilt
 * @self: a #ClutterBehaviourEllipse
 * @axis: a #ClutterRotateAxis
 * @angle_tilt: tilt of the elipse around the center in the given axis in
 * degrees.
 *
 * Sets the angle at which the ellipse should be tilted around it's center.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_angle_tilt (ClutterBehaviourEllipse *self,
                                          ClutterRotateAxis        axis,
                                          gdouble                  angle_tilt)
{
  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  clutter_behaviour_ellipse_set_angle_tiltx (self,
                                             axis,
                                             CLUTTER_ANGLE_FROM_DEG (angle_tilt));
}

/**
 * clutter_behaviour_ellipse_set_angle_tiltx
 * @self: a #ClutterBehaviourEllipse
 * @axis: a #ClutterRoateAxis
 * @angle_tilt: #ClutterAngle tilt of the elipse around the center in the given
 * axis
 *
 * Sets the angle at which the ellipse should be tilted around it's center.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_angle_tiltx (ClutterBehaviourEllipse *self,
                                           ClutterRotateAxis        axis,
                                           ClutterAngle             angle_tilt)
{
  ClutterBehaviourEllipsePrivate *priv;
  ClutterAngle new_angle;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  new_angle = angle_tilt - 256;

  priv = self->priv;

  switch (axis)
    {
    case CLUTTER_X_AXIS:
      if (priv->angle_tilt_x != new_angle)
        {
          priv->angle_tilt_x = new_angle;

          g_object_notify (G_OBJECT (self), "angle-tilt-x");
        }
      break;
    case CLUTTER_Y_AXIS:
      if (priv->angle_tilt_y != new_angle)
        {
          priv->angle_tilt_y = new_angle;

          g_object_notify (G_OBJECT (self), "angle-tilt-y");
        }
      break;
    case CLUTTER_Z_AXIS:
      if (priv->angle_tilt_z != new_angle)
        {
          priv->angle_tilt_z = new_angle;

          g_object_notify (G_OBJECT (self), "angle-tilt-z");
        }
      break;
    default:
      break;
    }
}

/**
 * clutter_behaviour_ellipse_get_angle_tilt
 * @self: a #ClutterBehaviourEllipse
 * @axis: a #ClutterRotateAxis
 *
 * Gets the tilt of the ellipse around the center in the given axis.
 *
 * Return value: angle in degrees.
 *
 * Since: 0.4
 */
gdouble
clutter_behaviour_ellipse_get_angle_tilt (ClutterBehaviourEllipse *self,
                                          ClutterRotateAxis        axis)
{
  g_return_val_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self), 0.0);

  return CLUTTER_ANGLE_TO_DEG (clutter_behaviour_ellipse_get_angle_tiltx (self,
                                                                          axis));
}

/**
 * clutter_behaviour_ellipse_get_angle_tiltx
 * @self: a #ClutterBehaviourEllipse
 * @axis: a #ClutterRotateAxis
 *
 * Gets the tilt of the ellipse around the center in the given axis.
 *
 * Return value: a #ClutterAngle
 *
 * Since: 0.4
 */
ClutterAngle
clutter_behaviour_ellipse_get_angle_tiltx (ClutterBehaviourEllipse *self,
                                           ClutterRotateAxis        axis)
{
  g_return_val_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self), 0);

  switch (axis)
    {
    case CLUTTER_X_AXIS:
      return self->priv->angle_tilt_x + 256;
    case CLUTTER_Y_AXIS:
      return self->priv->angle_tilt_y + 256;
    case CLUTTER_Z_AXIS:
      return self->priv->angle_tilt_z + 256;
    default:
      break;
    }

  return 0;
}

/**
 * clutter_behaviour_ellipse_set_tilt
 * @self: a #ClutterBehaviourEllipse
 * @angle_tilt_x: tilt of the elipse around the center in X axis in degrees.
 * @angle_tilt_y: tilt of the elipse around the center in Y axis in degrees.
 * @angle_tilt_z: tilt of the elipse around the center in Z axis in degrees.
 *
 * Sets the angles at which the ellipse should be tilted around it's center.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_tilt (ClutterBehaviourEllipse *self,
                                    gdouble                  angle_tilt_x,
                                    gdouble                  angle_tilt_y,
                                    gdouble                  angle_tilt_z)
{
  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  clutter_behaviour_ellipse_set_tiltx (self,
                                       CLUTTER_ANGLE_FROM_DEG (angle_tilt_x),
                                       CLUTTER_ANGLE_FROM_DEG (angle_tilt_y),
                                       CLUTTER_ANGLE_FROM_DEG (angle_tilt_z));
}

/**
 * clutter_behaviour_ellipse_set_tiltx
 * @self: a #ClutterBehaviourEllipse
 * @angle_tilt_x: #ClutterAngle tilt of the elipse around the center in X axis
 * @angle_tilt_y: #ClutterAngle tilt of the elipse around the center in Y axis
 * @angle_tilt_z: #ClutterAngle tilt of the elipse around the center in Z axis
 *
 * Sets the angle at which the ellipse should be tilted around it's center.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_tiltx (ClutterBehaviourEllipse *self,
                                     ClutterAngle             angle_tilt_x,
                                     ClutterAngle             angle_tilt_y,
                                     ClutterAngle             angle_tilt_z)
{
  ClutterBehaviourEllipsePrivate *priv;
  ClutterAngle new_angle_x, new_angle_y, new_angle_z;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  new_angle_x = angle_tilt_x - 256;
  new_angle_y = angle_tilt_y - 256;
  new_angle_z = angle_tilt_z - 256;

  priv = self->priv;

  g_object_freeze_notify (G_OBJECT (self));

  if (priv->angle_tilt_x != new_angle_x)
    {
      priv->angle_tilt_x = new_angle_x;

      g_object_notify (G_OBJECT (self), "angle-tilt-x");
    }

  if (priv->angle_tilt_y != new_angle_y)
    {
      priv->angle_tilt_y = new_angle_y;

      g_object_notify (G_OBJECT (self), "angle-tilt-y");
    }

  if (priv->angle_tilt_z != new_angle_z)
    {
      priv->angle_tilt_z = new_angle_z;

      g_object_notify (G_OBJECT (self), "angle-tilt-z");
    }
  
  g_object_thaw_notify (G_OBJECT (self));
}

/**
 * clutter_behaviour_ellipse_get_tilt
 * @self: a #ClutterBehaviourEllipse
 * @angle_tilt_x: location for tilt of the elipse around the center in X axis in
 * degrees, or NULL.
 * @angle_tilt_y: location for tilt of the elipse around the center in Y axis in
 * degrees, or NULL.
 * @angle_tilt_z: location for tilt of the elipse around the center in Z axis in
 * degrees, or NULL.
 *
 * Gets the tilt of the ellipse around the center in Y axis.
 *
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_get_tilt (ClutterBehaviourEllipse *self,
                                    gdouble                 *angle_tilt_x,
                                    gdouble                 *angle_tilt_y,
                                    gdouble                 *angle_tilt_z)
{
  ClutterBehaviourEllipsePrivate *priv;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  priv = self->priv;

  if (angle_tilt_x)
    *angle_tilt_x = CLUTTER_ANGLE_TO_DEG (priv->angle_tilt_x + 256);

  if (angle_tilt_y)
    *angle_tilt_y = CLUTTER_ANGLE_TO_DEG (priv->angle_tilt_y + 256);

  if (angle_tilt_z)
    *angle_tilt_z = CLUTTER_ANGLE_TO_DEG (priv->angle_tilt_z + 256);
}

/**
 * clutter_behaviour_ellipse_get_tiltx
 * @self: a #ClutterBehaviourEllipse
 * @angle_tilt_x: #ClutterAngle location for tilt of the elipse around the
 * center in X axis, or NULL.
 * @angle_tilt_y: #ClutterAngle location for tilt of the elipse around the
 * center in Y axis, or NULL.
 * @angle_tilt_z: #ClutterAngle location for tilt of the elipse around the
 * center in Z axis, or NULL.
 *
 * Gets the tilt of the ellipse around the center in Y axis.
 *
 * Return value: a #ClutterAngle
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_get_tiltx (ClutterBehaviourEllipse *self,
                                     ClutterAngle            *angle_tilt_x,
                                     ClutterAngle            *angle_tilt_y,
                                     ClutterAngle            *angle_tilt_z)
{
  ClutterBehaviourEllipsePrivate *priv;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  priv = self->priv;

  if (angle_tilt_x)
    *angle_tilt_x = priv->angle_tilt_x + 256;

  if (angle_tilt_y)
    *angle_tilt_y = priv->angle_tilt_y + 256;

  if (angle_tilt_z)
    *angle_tilt_z = priv->angle_tilt_z + 256;
}

/**
 * clutter_behaviour_ellipse_get_direction:
 * @self: a #ClutterBehaviourEllipse
 *
 * Retrieves the #ClutterRotateDirection used by the ellipse behaviour.
 *
 * Return value: the rotation direction
 *
 * Since: 0.4
 */
ClutterRotateDirection
clutter_behaviour_ellipse_get_direction (ClutterBehaviourEllipse *self)
{
  g_return_val_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self),
                        CLUTTER_ROTATE_CW);

  return self->priv->direction;
}

/**
 * clutter_behaviour_ellipse_set_direction:
 * @self: a #ClutterBehaviourEllipse
 * @direction: the rotation direction
 *
 * Sets the rotation direction used by the ellipse behaviour.
 *
 * Since: 0.4
 */
void
clutter_behaviour_ellipse_set_direction (ClutterBehaviourEllipse *self,
                                         ClutterRotateDirection  direction)
{
  ClutterBehaviourEllipsePrivate *priv;

  g_return_if_fail (CLUTTER_IS_BEHAVIOUR_ELLIPSE (self));

  priv = self->priv;

  if (priv->direction != direction)
    {
      priv->direction = direction;

      g_object_notify (G_OBJECT (self), "direction");
    }
}
