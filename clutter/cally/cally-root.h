/* CALLY - The Clutter Accessibility Implementation Library
 *
 * Copyright (C) 2009 Igalia, S.L.
 *
 * Author: Alejandro Piñeiro Iglesias <apinheiro@igalia.com>
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

#ifndef __CALLY_ROOT_H__
#define __CALLY_ROOT_H__

#include <atk/atk.h>

G_BEGIN_DECLS

#define CALLY_TYPE_ROOT            (cally_root_get_type ())
#define CALLY_ROOT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), CALLY_TYPE_ROOT, CallyRoot))
#define CALLY_ROOT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), CALLY_TYPE_ROOT, CallyRootClass))
#define CALLY_IS_ROOT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), CALLY_TYPE_ROOT))
#define CALLY_IS_ROOT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), CALLY_TYPE_ROOT))
#define CALLY_ROOT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), CALLY_TYPE_ROOT, CallyRootClass))

typedef struct _CallyRoot        CallyRoot;
typedef struct _CallyRootClass   CallyRootClass;
typedef struct _CallyRootPrivate CallyRootPrivate;

struct _CallyRoot
{
  AtkGObjectAccessible parent;

  /* < private > */
  CallyRootPrivate *priv;
};

struct _CallyRootClass
{
  AtkGObjectAccessibleClass parent_class;

  /* padding for future expansion */
  gpointer _padding_dummy[30];
};


GType      cally_root_get_type (void);
AtkObject *cally_root_new      (void);


G_END_DECLS

#endif /* __CALLY_ROOT_H__ */
