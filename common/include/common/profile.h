/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#pragma once

#ifdef ENABLE_TRACY
#include <tracy/TracyC.h>

#define LG_PROFILE_THREAD(name) TracyCSetThreadName(name)
#define LG_PROFILE_FRAME_DEFAULT() TracyCFrameMark
#define LG_PROFILE_FRAME(name) TracyCFrameMarkNamed(name)
#define LG_PROFILE_ZONE_BEGIN(ctx, name) TracyCZoneN(ctx, name, true)
#define LG_PROFILE_ZONE_END(ctx) TracyCZoneEnd(ctx)
#define LG_PROFILE_ZONE_VALUE(ctx, value) TracyCZoneValue(ctx, value)
#define LG_PROFILE_PLOT_I(name, value) TracyCPlotI(name, value)

#else

#define LG_PROFILE_THREAD(name) do { (void)(name); } while(0)
#define LG_PROFILE_FRAME_DEFAULT() do { } while(0)
#define LG_PROFILE_FRAME(name) do { (void)(name); } while(0)
#define LG_PROFILE_ZONE_BEGIN(ctx, name) do { (void)(name); } while(0)
#define LG_PROFILE_ZONE_END(ctx) do { } while(0)
#define LG_PROFILE_ZONE_VALUE(ctx, value) do { (void)(value); } while(0)
#define LG_PROFILE_PLOT_I(name, value) do { (void)(value); } while(0)

#endif
