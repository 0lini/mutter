/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/**
 * \file workspace.h    Workspaces
 *
 * A workspace is a set of windows which all live on the same
 * screen.  (You may also see the name "desktop" around the place,
 * which is the EWMH's name for the same thing.)  Only one workspace
 * of a screen may be active at once; all windows on all other workspaces
 * are unmapped.
 */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2004, 2005 Elijah Newren
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef META_WORKSPACE_H
#define META_WORKSPACE_H

#include "types.h"
#include "boxes.h"
#include "screen.h"

/* Negative to avoid conflicting with real workspace
 * numbers
 */
typedef enum
{
  META_MOTION_UP = -1,
  META_MOTION_DOWN = -2,
  META_MOTION_LEFT = -3,
  META_MOTION_RIGHT = -4,
  /* These are only used for effects */
  META_MOTION_UP_LEFT = -5,
  META_MOTION_UP_RIGHT = -6,
  META_MOTION_DOWN_LEFT = -7,
  META_MOTION_DOWN_RIGHT = -8
} MetaMotionDirection;

int  meta_workspace_index (MetaWorkspace *workspace);
MetaScreen *meta_workspace_get_screen (MetaWorkspace *workspace);
void meta_workspace_get_work_area_all_xineramas (MetaWorkspace *workspace,
                                                 MetaRectangle *area);
void meta_workspace_activate (MetaWorkspace *workspace, guint32 timestamp);
void meta_workspace_activate_with_focus (MetaWorkspace *workspace,
                                         MetaWindow    *focus_this,
                                         guint32        timestamp);
#endif
