/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "wayland.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"
#include "common/event.h"
#include "common/option.h"

// Surface-handling listeners.

void waylandWindowUpdateScale(void)
{
  struct WaylandScale maxScale = waylandScaleFromInt(0);
  struct SurfaceOutput * node;

  wl_list_for_each(node, &wlWm.surfaceOutputs, link)
  {
    struct WaylandScale scale = waylandOutputGetScale(node->output);
    if (waylandScaleCmp(scale, maxScale) > 0)
      maxScale = scale;
  }

  if (waylandScaleValid(maxScale))
  {
    wlWm.scale = maxScale;
    wlWm.fractionalScale = waylandScaleIsFractional(maxScale);
    wlWm.needsResize = true;
    waylandCursorScaleChange();
    app_invalidateWindow(true);
    waylandStopWaitFrame();
  }
}

static void wlSurfaceEnterHandler(void * data, struct wl_surface * surface, struct wl_output * output)
{
  struct SurfaceOutput * node = malloc(sizeof(*node));
  if (!node)
  {
    DEBUG_ERROR("out of memory");
    return;
  }

  node->output = output;
  wl_list_insert(&wlWm.surfaceOutputs, &node->link);
  waylandWindowUpdateScale();
}

static void wlSurfaceLeaveHandler(void * data, struct wl_surface * surface, struct wl_output * output)
{
  struct SurfaceOutput * node;
  wl_list_for_each(node, &wlWm.surfaceOutputs, link)
    if (node->output == output)
    {
      wl_list_remove(&node->link);
      break;
    }
  waylandWindowUpdateScale();
}

static const struct wl_surface_listener wlSurfaceListener = {
  .enter = wlSurfaceEnterHandler,
  .leave = wlSurfaceLeaveHandler,
};

struct WaylandImageDescriptionState
{
  bool ready;
  bool failed;
};

struct WaylandImageInfoState
{
  struct wp_image_description_v1 * imageDescription;
  const char * label;
  bool primaryNamed;
  bool tfNamed;
  bool luminances;
  bool targetLuminance;
  bool targetMaxCLL;
  bool targetMaxFALL;
  uint32_t primaries;
  uint32_t tf;
  uint32_t minLum;
  uint32_t maxLum;
  uint32_t referenceLum;
  uint32_t targetMinLum;
  uint32_t targetMaxLum;
  uint32_t maxCLL;
  uint32_t maxFALL;
};

static void imageDescriptionFailed(void * data,
    struct wp_image_description_v1 * imageDescription,
    uint32_t cause, const char * msg)
{
  struct WaylandImageDescriptionState * state = data;
  state->failed = true;
  DEBUG_WARN("Wayland color image description failed (%u): %s",
      cause, msg ? msg : "");
}

static void imageDescriptionReady(void * data,
    struct wp_image_description_v1 * imageDescription, uint32_t identity)
{
  struct WaylandImageDescriptionState * state = data;
  state->ready = true;
}

static const struct wp_image_description_v1_listener imageDescriptionListener = {
  .failed = imageDescriptionFailed,
  .ready  = imageDescriptionReady,
};

static void imageInfoDone(void * data,
    struct wp_image_description_info_v1 * info)
{
  struct WaylandImageInfoState * state = data;

  DEBUG_INFO("Wayland %s HDR image description: primaries:%s%u "
      "tf:%s%u luminance:%s%.4f/%u/%u nits target:%s%.4f/%u nits "
      "maxCLL:%s%u nits maxFALL:%s%u nits",
      state->label ? state->label : "unknown",
      state->primaryNamed ? "" : "?", state->primaries,
      state->tfNamed ? "" : "?", state->tf,
      state->luminances ? "" : "?",
      (double)state->minLum / 10000.0, state->maxLum, state->referenceLum,
      state->targetLuminance ? "" : "?",
      (double)state->targetMinLum / 10000.0, state->targetMaxLum,
      state->targetMaxCLL ? "" : "?", state->maxCLL,
      state->targetMaxFALL ? "" : "?", state->maxFALL);

  if (state->imageDescription)
    wp_image_description_v1_destroy(state->imageDescription);
  free(state);
}

static void imageInfoICCFile(void * data,
    struct wp_image_description_info_v1 * info, int32_t icc, uint32_t size)
{
  if (icc >= 0)
    close(icc);
}

static void imageInfoPrimaries(void * data,
    struct wp_image_description_info_v1 * info,
    int32_t rx, int32_t ry, int32_t gx, int32_t gy,
    int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
}

static void imageInfoPrimariesNamed(void * data,
    struct wp_image_description_info_v1 * info, uint32_t primaries)
{
  struct WaylandImageInfoState * state = data;
  state->primaryNamed = true;
  state->primaries = primaries;
}

static void imageInfoTFPower(void * data,
    struct wp_image_description_info_v1 * info, uint32_t eexp)
{
}

static void imageInfoTFNamed(void * data,
    struct wp_image_description_info_v1 * info, uint32_t tf)
{
  struct WaylandImageInfoState * state = data;
  state->tfNamed = true;
  state->tf = tf;
}

static void imageInfoLuminances(void * data,
    struct wp_image_description_info_v1 * info,
    uint32_t minLum, uint32_t maxLum, uint32_t referenceLum)
{
  struct WaylandImageInfoState * state = data;
  state->luminances = true;
  state->minLum = minLum;
  state->maxLum = maxLum;
  state->referenceLum = referenceLum;
}

static void imageInfoTargetPrimaries(void * data,
    struct wp_image_description_info_v1 * info,
    int32_t rx, int32_t ry, int32_t gx, int32_t gy,
    int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
}

static void imageInfoTargetLuminance(void * data,
    struct wp_image_description_info_v1 * info,
    uint32_t minLum, uint32_t maxLum)
{
  struct WaylandImageInfoState * state = data;
  state->targetLuminance = true;
  state->targetMinLum = minLum;
  state->targetMaxLum = maxLum;
}

static void imageInfoTargetMaxCLL(void * data,
    struct wp_image_description_info_v1 * info, uint32_t maxCLL)
{
  struct WaylandImageInfoState * state = data;
  state->targetMaxCLL = true;
  state->maxCLL = maxCLL;
}

static void imageInfoTargetMaxFALL(void * data,
    struct wp_image_description_info_v1 * info, uint32_t maxFALL)
{
  struct WaylandImageInfoState * state = data;
  state->targetMaxFALL = true;
  state->maxFALL = maxFALL;
}

static const struct wp_image_description_info_v1_listener imageInfoListener = {
  .done             = imageInfoDone,
  .icc_file         = imageInfoICCFile,
  .primaries        = imageInfoPrimaries,
  .primaries_named  = imageInfoPrimariesNamed,
  .tf_power         = imageInfoTFPower,
  .tf_named         = imageInfoTFNamed,
  .luminances       = imageInfoLuminances,
  .target_primaries = imageInfoTargetPrimaries,
  .target_luminance = imageInfoTargetLuminance,
  .target_max_cll   = imageInfoTargetMaxCLL,
  .target_max_fall  = imageInfoTargetMaxFALL,
};

static void preferredImageDescriptionReady(void * data,
    struct wp_image_description_v1 * imageDescription, uint32_t identity)
{
  struct WaylandImageInfoState * state = data;
  struct wp_image_description_info_v1 * info =
    wp_image_description_v1_get_information(imageDescription);

  if (!info)
  {
    wp_image_description_v1_destroy(imageDescription);
    free(state);
    return;
  }

  state->imageDescription = imageDescription;
  wp_image_description_info_v1_add_listener(info, &imageInfoListener, state);
}

static void preferredImageDescriptionFailed(void * data,
    struct wp_image_description_v1 * imageDescription,
    uint32_t cause, const char * msg)
{
  DEBUG_WARN("Wayland preferred image description failed (%u): %s",
      cause, msg ? msg : "");
  wp_image_description_v1_destroy(imageDescription);
  free(data);
}

static const struct wp_image_description_v1_listener preferredImageListener = {
  .failed = preferredImageDescriptionFailed,
  .ready  = preferredImageDescriptionReady,
};

static void waylandQueryPreferredImageDescription(void)
{
  if (!wlWm.colorFeedback)
    return;

  struct WaylandImageInfoState * state = calloc(1, sizeof(*state));
  if (!state)
    return;
  state->label = "preferred";

  struct wp_image_description_v1 * imageDescription =
    wp_color_management_surface_feedback_v1_get_preferred_parametric(
        wlWm.colorFeedback);
  if (!imageDescription)
  {
    free(state);
    return;
  }

  wp_image_description_v1_add_listener(imageDescription,
      &preferredImageListener, state);
}

static void colorFeedbackPreferredChanged(void * data,
    struct wp_color_management_surface_feedback_v1 * feedback,
    uint32_t identity)
{
  waylandQueryPreferredImageDescription();
}

static const struct wp_color_management_surface_feedback_v1_listener
colorFeedbackListener = {
  .preferred_changed = colorFeedbackPreferredChanged,
};

static bool waylandWaitImageDescription(
    struct wp_image_description_v1 * imageDescription)
{
  struct WaylandImageDescriptionState state = { 0 };
  wp_image_description_v1_add_listener(imageDescription,
      &imageDescriptionListener, &state);

  for (int i = 0; i < 4 && !state.ready && !state.failed; ++i)
  {
    if (wl_display_roundtrip(wlWm.display) < 0)
      break;
  }

  return state.ready && !state.failed;
}

static struct wp_image_description_v1 * waylandCreatePQDescription(void)
{
  if (!wlWm.colorFeatureParametric || !wlWm.colorTFPQ ||
      !wlWm.colorPrimariesBT2020)
  {
    DEBUG_WARN("Wayland color-management PQ output unavailable "
        "(parametric:%d pq:%d bt2020:%d)",
        wlWm.colorFeatureParametric, wlWm.colorTFPQ,
        wlWm.colorPrimariesBT2020);
    return NULL;
  }

  struct wp_image_description_creator_params_v1 * params =
    wp_color_manager_v1_create_parametric_creator(wlWm.colorManager);
  if (!params)
    return NULL;

  wp_image_description_creator_params_v1_set_tf_named(params,
      WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
  wp_image_description_creator_params_v1_set_primaries_named(params,
      WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);

  // hdrMetadataPeak is also the EGL renderer's PQ tone map target; the
  // max_cll advertised here must match it or the compositor will tone map
  // the content a second time
  const int metadataPeak = option_get_int("egl", "hdrMetadataPeak");
  const int metadataFALL = option_get_int("egl", "hdrMetadataFALL");
  const uint32_t metadataMax =
    metadataPeak > 0 ? (uint32_t)metadataPeak : 10000;
  DEBUG_INFO("Wayland HDR metadata request: BT.2020/PQ luminance:0.0050/10000/203 nits "
      "mastering:0.0050/%u nits maxCLL:%u nits maxFALL:%d nits",
      metadataMax, metadataMax, metadataFALL);

  if (wlWm.colorFeatureSetLuminances)
    wp_image_description_creator_params_v1_set_luminances(params,
        50, 10000, 203);

  if (wlWm.colorFeatureSetMastering)
  {
    wp_image_description_creator_params_v1_set_mastering_display_primaries(
        params,
        708000, 292000,
        170000, 797000,
        131000,  46000,
        312700, 329000);
    wp_image_description_creator_params_v1_set_mastering_luminance(
        params, 50, metadataMax);
  }

  if (metadataPeak > 0)
    wp_image_description_creator_params_v1_set_max_cll(params, metadataMax);
  if (metadataFALL > 0)
    wp_image_description_creator_params_v1_set_max_fall(
        params, (uint32_t)metadataFALL);

  return wp_image_description_creator_params_v1_create(params);
}

static bool waylandWindowInitColorManagement(void)
{
  if (!wlWm.colorManager || !wlWm.colorManagerDone)
    return false;

  if (!wlWm.colorIntentPerceptual)
  {
    DEBUG_WARN("Wayland color-management did not advertise perceptual intent");
    return false;
  }

  wlWm.colorSurface =
    wp_color_manager_v1_get_surface(wlWm.colorManager, wlWm.surface);
  if (!wlWm.colorSurface)
    return false;

  wlWm.colorFeedback =
    wp_color_manager_v1_get_surface_feedback(wlWm.colorManager, wlWm.surface);
  if (wlWm.colorFeedback)
  {
    wp_color_management_surface_feedback_v1_add_listener(
        wlWm.colorFeedback, &colorFeedbackListener, NULL);
    waylandQueryPreferredImageDescription();
  }

  struct wp_image_description_v1 * imageDescription =
    waylandCreatePQDescription();
  if (!imageDescription)
    return false;

  if (!waylandWaitImageDescription(imageDescription))
  {
    wp_image_description_v1_destroy(imageDescription);
    return false;
  }

  wp_color_management_surface_v1_set_image_description(
      wlWm.colorSurface, imageDescription,
      WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
  wp_image_description_v1_destroy(imageDescription);

  DEBUG_INFO("Wayland HDR output: BT.2020/PQ via color-management-v1");
  return true;
}

static void waylandWindowInitHDR(void)
{
  const char * hdrOutput = option_get_string("egl", "hdrOutput");
  if (!hdrOutput || strcmp(hdrOutput, "pq") != 0)
    return;

  if (!waylandWindowInitColorManagement())
    DEBUG_WARN("egl:hdrOutput=pq requested, but color-management-v1 is "
        "unavailable");
}

bool waylandWindowInit(const char * title, const char * appId, bool fullscreen, bool maximize, bool borderless, bool resizable)
{
  wlWm.scale = waylandScaleFromInt(1);

  wlWm.frameEvent = lgCreateEvent(true, 0);
  if (!wlWm.frameEvent)
  {
    DEBUG_ERROR("Failed to initialize event for waitFrame");
    return false;
  }
  lgSignalEvent(wlWm.frameEvent);

  if (!wlWm.compositor)
  {
    DEBUG_ERROR("Compositor missing wl_compositor (version 3+), will not proceed");
    return false;
  }

  wlWm.surface = wl_compositor_create_surface(wlWm.compositor);
  if (!wlWm.surface)
  {
    DEBUG_ERROR("Failed to create wl_surface");
    return false;
  }

  wl_surface_add_listener(wlWm.surface, &wlSurfaceListener, NULL);
  waylandWindowInitHDR();

  if (!wlWm.desktop->shellInit(wlWm.display, wlWm.surface,
        title, appId, fullscreen, maximize, borderless, resizable))
    return false;

  wl_surface_commit(wlWm.surface);
  return true;
}

void waylandWindowFree(void)
{
  if (wlWm.colorFeedback)
    wp_color_management_surface_feedback_v1_destroy(wlWm.colorFeedback);
  if (wlWm.colorSurface)
    wp_color_management_surface_v1_destroy(wlWm.colorSurface);
  wl_surface_destroy(wlWm.surface);
  lgFreeEvent(wlWm.frameEvent);
}

void waylandSetWindowSize(int x, int y)
{
    wlWm.desktop->shellResize(x, y);
}

bool waylandIsValidPointerPos(int x, int y)
{
  int width, height;
  wlWm.desktop->getSize(&width, &height);
  return x >= 0 && x < width && y >= 0 && y < height;
}

static void frameHandler(void * opaque, struct wl_callback * callback, unsigned int data)
{
  lgSignalEvent(wlWm.frameEvent);
  wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
   .done = frameHandler,
};

bool waylandWaitFrame(void)
{
  lgWaitEvent(wlWm.frameEvent, TIMEOUT_INFINITE);

  struct wl_callback * callback = wl_surface_frame(wlWm.surface);
  if (callback)
    wl_callback_add_listener(callback, &frame_listener, NULL);

  return false;
}

void waylandSkipFrame(void)
{
  // If we decided to not render, we must commit the surface so that the callback is registered.
  wl_surface_commit(wlWm.surface);
}

void waylandStopWaitFrame(void)
{
  lgSignalEvent(wlWm.frameEvent);
}
