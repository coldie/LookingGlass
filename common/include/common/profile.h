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
