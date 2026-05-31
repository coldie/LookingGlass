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

#include "interface/capture.h"

#include "common/debug.h"
#include "common/windebug.h"
#include "common/option.h"
#include "common/framebuffer.h"
#include "common/rects.h"
#include "common/KVMFR.h"
#include "common/time.h"
#include "common/profile.h"

#include "backend/wgc.h"
#include "d12.h"  // for D12 device helpers and shared D12 frame descriptors

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <math.h>
#include <inttypes.h>
#include <roapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <winstring.h>

#define WIDL_using_Windows_Graphics_Capture
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>

typedef enum WGCCapturePublishMode
{
  WGC_CAPTURE_PUBLISH_AUTO,
  WGC_CAPTURE_PUBLISH_CPU_STAGING,
  WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT,
  WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY
}
WGCCapturePublishMode;

typedef enum WGCCapturePublishFormat
{
  WGC_CAPTURE_PUBLISH_FORMAT_AUTO,
  WGC_CAPTURE_PUBLISH_FORMAT_BGRA8,
  WGC_CAPTURE_PUBLISH_FORMAT_RGBA16F,
  WGC_CAPTURE_PUBLISH_FORMAT_NV12,
  WGC_CAPTURE_PUBLISH_FORMAT_P010,
  WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ
}
WGCCapturePublishFormat;

typedef enum WGCCaptureHDRMode
{
  WGC_CAPTURE_HDR_MODE_AUTO,
  WGC_CAPTURE_HDR_MODE_OFF,
  WGC_CAPTURE_HDR_MODE_TONEMAP,
  WGC_CAPTURE_HDR_MODE_PRESERVE,
  WGC_CAPTURE_HDR_MODE_PRESERVE_PQ
}
WGCCaptureHDRMode;

typedef struct FrameDamage
{
  int             count;
  FrameDamageRect rects[KVMFR_MAX_DAMAGE_RECTS];
}
FrameDamage;

struct WGCCapture
{
  WGCInstance      * wgc;
  IDXGIFactory2   ** factory;
  IDXGIAdapter1   ** adapter;
  IDXGIOutput     ** output;

  CaptureGetPointerBuffer  getPointerBufferFn;
  CapturePostPointerBuffer postPointerBufferFn;
  unsigned                 frameBufferCount;

  bool                     debug;
  bool                     trackDamage;
  bool                     timings;
  bool                     debugStats;
  int                      dirtyFullCopyPercent;

  // IVSHMEM-direct mode state. Populated by wgc_capture_init when the user
  // requests publishMode=ivshmem-direct (or auto, when supported on this
  // hardware). All NULL/zero in cpu-staging mode.
  WGCCapturePublishMode    publishMode;        // resolved at init
  WGCCapturePublishFormat  publishFormat;      // resolved publish encoding
  WGCCapturePublishFormat  requestedEncoding;  // explicit wgc:encoding
  WGCCapturePublishFormat  sdrEncoding;        // auto policy for SDR sources
  WGCCapturePublishFormat  hdrEncoding;        // auto policy for HDR sources
  WGCCaptureHDRMode        hdrMode;
  void                   * ivshmemBase;
  HMODULE                  d3d12Module;        // dynamically loaded
  ID3D12Device3          * d3d12Device;
  ID3D12CommandQueue     * d3d12Queue;
  ID3D12Heap             * ivshmemHeap;
  ID3D11On12Device       * d11on12Device;
  ID3D11Device           * d11Device;          // returned by D3D11On12CreateDevice
  ID3D11DeviceContext    * d11Context;
  bool                     ivshmemSlotRegistered[LGMP_Q_FRAME_LEN];

  // resolved at first successful fetch
  unsigned                 width;
  unsigned                 height;
  unsigned                 pitch;     // output bytes per row
  unsigned                 mappedPitch; // bytes per row from staging Map
  unsigned                 stride;    // pixels per row
  unsigned                 dataHeight;
  unsigned                 formatVer;
  uint8_t                * encodeBuffer;
  size_t                   encodeBufferSize;

  // current frame state between waitFrame() and getFrame()
  bool                     frameMapped;
  void                   * mapped;
  WGCFrameDesc             desc;

  // per-framebuffer accumulated damage; mirrors the DXGI pattern
  FrameDamage              frameDamage[LGMP_Q_FRAME_LEN];

  uint64_t                 statsIntervalStart;
  uint64_t                 cpuReadbackTotalUs;
  uint64_t                 cpuReadbackMaxUs;
  uint64_t                 cpuReadbackCount;
  uint64_t                 cpuMemcpyTotalUs;
  uint64_t                 cpuMemcpyMaxUs;
  uint64_t                 cpuMemcpyCount;
  uint64_t                 callbackPostTotalUs;
  uint64_t                 callbackPostMaxUs;
  uint64_t                 callbackPostCount;
  uint64_t                 copyFull;
  uint64_t                 copyDirty;
  uint64_t                 copyRects;
  uint64_t                 copyPixels;
};

static struct WGCCapture * this = NULL;

static const char * wgc_capture_getName(void)
{
  return "Windows Graphics Capture";
}

static void wgc_capture_initOptions(void)
{
  struct Option options[] =
  {
    {
      .module         = "wgc",
      .name           = "adapter",
      .description    = "The name of the adapter to capture",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = NULL
    },
    {
      .module         = "wgc",
      .name           = "output",
      .description    = "The name of the adapter's output to capture",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = NULL
    },
    {
      .module         = "wgc",
      .name           = "trackDamage",
      .description    = "Perform damage-aware copies (saves bandwidth)",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = true
    },
    {
      .module         = "wgc",
      .name           = "publishMode",
      .description    = "Publish path: auto|ivshmem-direct|ivshmem-d3d12-copy|cpu-staging "
                        "(auto picks ivshmem-direct when supported)",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "auto"
    },
    {
      .module         = "wgc",
      .name           = "publishFormat",
      .description    = "Deprecated alias for wgc:encoding",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "auto"
    },
    {
      .module         = "wgc",
      .name           = "encoding",
      .description    = "WGC publish encoding: auto|bgra8|rgba16f|nv12|p010. "
                        "auto uses wgc:sdrEncoding/wgc:hdrEncoding.",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "auto"
    },
    {
      .module         = "wgc",
      .name           = "sdrEncoding",
      .description    = "WGC publish encoding used by wgc:encoding=auto for SDR sources: bgra8|rgba16f|nv12|p010",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "nv12"
    },
    {
      .module         = "wgc",
      .name           = "hdrEncoding",
      .description    = "WGC publish encoding used by wgc:encoding=auto for HDR sources: bgra8|rgba16f|nv12|p010",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "p010"
    },
    {
      .module         = "wgc",
      .name           = "hdrMode",
      .description    = "HDR source handling for wgc:encoding=auto: auto|off|tonemap|preserve|preserve-pq",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "preserve-pq"
    },
    {
      .module         = "wgc",
      .name           = "debug",
      .description    = "Enable debug logging for the WGC capture path",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "wgc",
      .name           = "cursor",
      .description    = "Cursor mode: separate, embedded, or none",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "separate"
    },
    {
      .module         = "wgc",
      .name           = "maxFPS",
      .description    = "Maximum WGC capture rate via MinUpdateInterval (0 = OS default, commonly 60Hz)",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 240 // 240Hz covers high-refresh displays; 0 uses OS default (~60Hz)
    },
    {
      .module         = "wgc",
      .name           = "cursorMaxHz",
      .description    = "Maximum separate-cursor position update rate (0 = unlimited)",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 120 // 120Hz cursor position polling
    },
    {
      .module         = "wgc",
      .name           = "timings",
      .description    = "Log WGC capture timing summaries once per second",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "wgc",
      .name           = "debugStats",
      .description    = "Log lightweight WGC burst/stall/dirty-copy diagnostics",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "wgc",
      .name           = "asyncCapture",
      .description    = "Copy WGC frames in the FrameArrived callback and publish the newest completed frame",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "wgc",
      .name           = "pollFramePool",
      .description    = "Poll the WGC frame pool on short timeouts instead of relying only on FrameArrived",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "wgc",
      .name           = "pollFramePoolMs",
      .description    = "Frame pool polling wait in milliseconds when wgc:pollFramePool is enabled",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 1
    },
    {
      .module         = "wgc",
      .name           = "includeSecondaryWindows",
      .description    = "Capture secondary windows for the selected WGC item when supported by the OS",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = true
    },
    {
      .module         = "wgc",
      .name           = "dwmFlushOnGap",
      .description    = "Call DwmFlush when WGC callback delivery stalls",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "wgc",
      .name           = "dwmFlushGapMs",
      .description    = "Minimum WGC callback gap before DwmFlush is used",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 50
    },
    {
      .module         = "wgc",
      .name           = "dirtyFullCopyPercent",
      .description    = "Use a full IVSHMEM write when merged dirty area reaches this frame percentage (0 = never)",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 65 // 65% is a sweet spot: benefits of dirty copies before the overhead
                            // of many small rects approaches that of a full copy
    },
    {
      .module         = "wgc",
      .name           = "d3d12CopyQueues",
      .description    = "Number of D3D12 COPY queues used for full-frame IVSHMEM copies",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 1
    },
    {
      .module         = "wgc",
      .name           = "d3d12FullCopyAlways",
      .description    = "Always use full-frame D3D12 bridge-to-IVSHMEM copies while preserving client damage rects",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = true
    },
    {
      .module         = "wgc",
      .name           = "tiled",
      .description    = "Tile dirty D3D12 bridge-to-IVSHMEM copies: none|dirty",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "none"
    },
    {
      .module         = "wgc",
      .name           = "tileSize",
      .description    = "Tile size for wgc:tiled=dirty, formatted WIDTHxHEIGHT",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "256x64"
    },
    {
      .module         = "wgc",
      .name           = "dirtyMaxTiles",
      .description    = "Maximum tile spans before falling back to full D3D12 copy",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 128 // 128 tiles ~= 2 MiB of copy commands; beyond this a full copy is cheaper
    },
    {0}
  };

  option_register(options);
}

static uint64_t wgc_capture_rectArea(const FrameDamageRect * rects, int count)
{
  uint64_t area = 0;

  for (int i = 0; i < count; ++i)
    area += (uint64_t)rects[i].width * rects[i].height;

  return area;
}

static WGCCapturePublishFormat wgc_capture_parseEncoding(
  const char * value, WGCCapturePublishFormat fallback, const char * optionName)
{
  if (!value || strcmp(value, "auto") == 0)
    return fallback;
  if (strcmp(value, "bgra8") == 0)
    return WGC_CAPTURE_PUBLISH_FORMAT_BGRA8;
  if (strcmp(value, "rgba16f") == 0)
    return WGC_CAPTURE_PUBLISH_FORMAT_RGBA16F;
  if (strcmp(value, "nv12") == 0)
    return WGC_CAPTURE_PUBLISH_FORMAT_NV12;
  if (strcmp(value, "p010") == 0)
    return WGC_CAPTURE_PUBLISH_FORMAT_P010;
  if (strcmp(value, "rgba10pq") == 0 || strcmp(value, "rgb10pq") == 0)
    return WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ;

  DEBUG_WARN("Unknown wgc:%s \"%s\", using fallback", optionName, value);
  return fallback;
}

static const char * wgc_capture_encodingName(WGCCapturePublishFormat format)
{
  switch(format)
  {
    case WGC_CAPTURE_PUBLISH_FORMAT_BGRA8  : return "bgra8";
    case WGC_CAPTURE_PUBLISH_FORMAT_RGBA16F: return "rgba16f";
    case WGC_CAPTURE_PUBLISH_FORMAT_NV12   : return "nv12";
    case WGC_CAPTURE_PUBLISH_FORMAT_P010   : return "p010";
    case WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ: return "rgba10pq";
    case WGC_CAPTURE_PUBLISH_FORMAT_AUTO   : return "auto";
  }
  return "unknown";
}

static WGCCaptureHDRMode wgc_capture_parseHDRMode(const char * value)
{
  if (!value || strcmp(value, "auto") == 0)
    return WGC_CAPTURE_HDR_MODE_AUTO;
  if (strcmp(value, "off") == 0)
    return WGC_CAPTURE_HDR_MODE_OFF;
  if (strcmp(value, "tonemap") == 0)
    return WGC_CAPTURE_HDR_MODE_TONEMAP;
  if (strcmp(value, "preserve") == 0)
    return WGC_CAPTURE_HDR_MODE_PRESERVE;
  if (strcmp(value, "preserve-pq") == 0)
    return WGC_CAPTURE_HDR_MODE_PRESERVE_PQ;

  DEBUG_WARN("Unknown wgc:hdrMode \"%s\", using tonemap", value);
  return WGC_CAPTURE_HDR_MODE_TONEMAP;
}

static const char * wgc_capture_hdrModeName(WGCCaptureHDRMode mode)
{
  switch(mode)
  {
    case WGC_CAPTURE_HDR_MODE_AUTO    : return "auto";
    case WGC_CAPTURE_HDR_MODE_OFF     : return "off";
    case WGC_CAPTURE_HDR_MODE_TONEMAP : return "tonemap";
    case WGC_CAPTURE_HDR_MODE_PRESERVE: return "preserve";
    case WGC_CAPTURE_HDR_MODE_PRESERVE_PQ: return "preserve-pq";
  }
  return "unknown";
}

static bool wgc_capture_colorSpaceIsHDR(DXGI_COLOR_SPACE_TYPE colorSpace)
{
  return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
         colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 ||
         colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

static float wgc_capture_halfToFloat(uint16_t h)
{
  const uint16_t sign = h >> 15;
  const uint16_t exp  = (h >> 10) & 0x1f;
  const uint16_t mant = h & 0x03ff;
  float value;

  if (exp == 0)
    value = mant ? ldexpf((float)mant / 1024.0f, -14) : 0.0f;
  else if (exp == 31)
    value = mant ? 0.0f : 65504.0f;
  else
    value = ldexpf(1.0f + (float)mant / 1024.0f, (int)exp - 15);

  return sign ? -value : value;
}

static float wgc_capture_pqOETF(float nits)
{
  const float m1 = 2610.0f / 16384.0f;
  const float m2 = 2523.0f / 32.0f;
  const float c1 = 3424.0f / 4096.0f;
  const float c2 = 2413.0f / 128.0f;
  const float c3 = 2392.0f / 128.0f;
  const float n = min(max(nits / 10000.0f, 0.0f), 1.0f);
  const float p = powf(n, m1);
  return powf((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

static uint32_t wgc_capture_packRGBA10PQ(float r709, float g709, float b709)
{
  const float r2020 =
    max(0.0f, r709) * 0.6274039f +
    max(0.0f, g709) * 0.3292829f +
    max(0.0f, b709) * 0.0433131f;
  const float g2020 =
    max(0.0f, r709) * 0.0690973f +
    max(0.0f, g709) * 0.9195404f +
    max(0.0f, b709) * 0.0113622f;
  const float b2020 =
    max(0.0f, r709) * 0.0163914f +
    max(0.0f, g709) * 0.0880133f +
    max(0.0f, b709) * 0.8955953f;

  const uint32_t r = (uint32_t)(wgc_capture_pqOETF(r2020 * 80.0f) * 1023.0f + 0.5f);
  const uint32_t g = (uint32_t)(wgc_capture_pqOETF(g2020 * 80.0f) * 1023.0f + 0.5f);
  const uint32_t b = (uint32_t)(wgc_capture_pqOETF(b2020 * 80.0f) * 1023.0f + 0.5f);
  return min(r, 1023u) | (min(g, 1023u) << 10) |
    (min(b, 1023u) << 20) | (3u << 30);
}

static bool wgc_capture_encodeRGBA10PQ(void)
{
  const size_t needed = (size_t)this->pitch * this->height;
  if (this->encodeBufferSize < needed)
  {
    uint8_t * newBuffer = realloc(this->encodeBuffer, needed);
    if (!newBuffer)
    {
      DEBUG_ERROR("Failed to allocate WGC rgba10pq encode buffer");
      return false;
    }
    this->encodeBuffer = newBuffer;
    this->encodeBufferSize = needed;
  }

  for(unsigned y = 0; y < this->height; ++y)
  {
    const uint8_t * src = (const uint8_t *)this->mapped +
      (size_t)y * this->mappedPitch;
    uint32_t * dst = (uint32_t *)(this->encodeBuffer +
      (size_t)y * this->pitch);
    for(unsigned x = 0; x < this->width; ++x)
    {
      const uint16_t * px = (const uint16_t *)src + (size_t)x * 4;
      dst[x] = wgc_capture_packRGBA10PQ(
        wgc_capture_halfToFloat(px[0]),
        wgc_capture_halfToFloat(px[1]),
        wgc_capture_halfToFloat(px[2]));
    }
  }

  return true;
}

static DXGI_COLOR_SPACE_TYPE wgc_capture_getOutputColorSpace(IDXGIOutput * output)
{
  DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  IDXGIOutput6 * output6 = NULL;
  HRESULT hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6,
    (void **)&output6);
  if (SUCCEEDED(hr))
  {
    DXGI_OUTPUT_DESC1 desc1;
    hr = IDXGIOutput6_GetDesc1(output6, &desc1);
    if (SUCCEEDED(hr))
      colorSpace = desc1.ColorSpace;
    IDXGIOutput6_Release(output6);
  }
  return colorSpace;
}

static void wgc_capture_resolveEncoding(DXGI_COLOR_SPACE_TYPE colorSpace)
{
  const bool hdrSource = wgc_capture_colorSpaceIsHDR(colorSpace);

  if (this->requestedEncoding != WGC_CAPTURE_PUBLISH_FORMAT_AUTO)
  {
    this->publishFormat = this->requestedEncoding;
    DEBUG_INFO("WGC encoding: %s (explicit, colorSpace:0x%x)",
      wgc_capture_encodingName(this->publishFormat), (unsigned)colorSpace);
    return;
  }

  if (!hdrSource || this->hdrMode == WGC_CAPTURE_HDR_MODE_OFF)
    this->publishFormat = this->sdrEncoding;
  else if (this->hdrMode == WGC_CAPTURE_HDR_MODE_PRESERVE)
    this->publishFormat = WGC_CAPTURE_PUBLISH_FORMAT_RGBA16F;
  else
    this->publishFormat = this->hdrEncoding;

  DEBUG_INFO("WGC encoding: %s (source:%s colorSpace:0x%x hdrMode:%s "
    "sdrEncoding:%s hdrEncoding:%s)",
    wgc_capture_encodingName(this->publishFormat),
    hdrSource ? "HDR" : "SDR", (unsigned)colorSpace,
    wgc_capture_hdrModeName(this->hdrMode),
    wgc_capture_encodingName(this->sdrEncoding),
    wgc_capture_encodingName(this->hdrEncoding));
}

static void wgc_capture_forcePublishModeForEncoding(void)
{
  if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ &&
      this->publishMode != WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT &&
      this->publishMode != WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY)
  {
    DEBUG_WARN("WGC RGBA10/PQ encoding currently requires "
      "GPU IVSHMEM publish; forcing wgc:publishMode=ivshmem-d3d12-copy");
    this->publishMode = WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY;
  }

  if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12 &&
      this->publishMode != WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT &&
      this->publishMode != WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY)
  {
    DEBUG_WARN("WGC NV12 encoding currently requires "
      "GPU IVSHMEM publish; forcing wgc:publishMode=ivshmem-direct");
    this->publishMode = WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT;
  }
  if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_P010 &&
      this->publishMode != WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT &&
      this->publishMode != WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY)
  {
    DEBUG_WARN("WGC P010 encoding currently requires "
      "GPU IVSHMEM publish; forcing wgc:publishMode=ivshmem-d3d12-copy");
    this->publishMode = WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY;
  }
}

static bool wgc_capture_damageTooLarge(const FrameDamageRect * rects, int count)
{
  if (!this || this->dirtyFullCopyPercent <= 0 || count <= 0 ||
      !this->width || !this->dataHeight)
    return false;

  const uint64_t frameArea = (uint64_t)this->width * this->dataHeight;
  const uint64_t dirtyArea = wgc_capture_rectArea(rects, count);

  return dirtyArea * 100 >= frameArea * (uint64_t)this->dirtyFullCopyPercent;
}

static void wgc_capture_logStats(void)
{
  if (!this || (!this->timings && !this->debugStats))
    return;

  const uint64_t now = microtime();
  if (now - this->statsIntervalStart < 1000000)
    return;

  DEBUG_INFO(
    "WGC cpu stats readback-avg-us:%" PRIu64 " readback-max-us:%" PRIu64 " readback-count:%" PRIu64 " memcpy-avg-us:%" PRIu64 " memcpy-max-us:%" PRIu64 " memcpy-count:%" PRIu64 " callback-post-avg-us:%" PRIu64 " callback-post-max-us:%" PRIu64 " callback-post-count:%" PRIu64 " ivshmem-full:%" PRIu64 " ivshmem-dirty:%" PRIu64 " ivshmem-rects:%" PRIu64 " ivshmem-kpix:%" PRIu64,
    this->cpuReadbackCount ?
      this->cpuReadbackTotalUs / this->cpuReadbackCount : 0,
    this->cpuReadbackMaxUs,
    this->cpuReadbackCount,
    this->cpuMemcpyCount ? this->cpuMemcpyTotalUs / this->cpuMemcpyCount : 0,
    this->cpuMemcpyMaxUs,
    this->cpuMemcpyCount,
    this->callbackPostCount ?
      this->callbackPostTotalUs / this->callbackPostCount : 0,
    this->callbackPostMaxUs,
    this->callbackPostCount,
    this->copyFull,
    this->copyDirty,
    this->copyRects,
    this->copyPixels / 1000);

  this->statsIntervalStart = now;
  this->cpuReadbackTotalUs = 0;
  this->cpuReadbackMaxUs   = 0;
  this->cpuReadbackCount   = 0;
  this->cpuMemcpyTotalUs   = 0;
  this->cpuMemcpyMaxUs     = 0;
  this->cpuMemcpyCount     = 0;
  this->callbackPostTotalUs = 0;
  this->callbackPostMaxUs   = 0;
  this->callbackPostCount   = 0;
  this->copyFull           = 0;
  this->copyDirty          = 0;
  this->copyRects          = 0;
  this->copyPixels         = 0;
}

static bool wgc_capture_create(
  CaptureGetPointerBuffer  getPointerBufferFn,
  CapturePostPointerBuffer postPointerBufferFn,
  unsigned                 frameBuffers)
{
  this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("out of memory");
    return false;
  }

  this->getPointerBufferFn  = getPointerBufferFn;
  this->postPointerBufferFn = postPointerBufferFn;
  this->frameBufferCount    = frameBuffers;
  this->debug               = option_get_bool("wgc", "debug");
  this->trackDamage         = option_get_bool("wgc", "trackDamage");
  this->timings             = option_get_bool("wgc", "timings");
  this->debugStats          = option_get_bool("wgc", "debugStats");
  this->dirtyFullCopyPercent =
    option_get_int("wgc", "dirtyFullCopyPercent");
  this->statsIntervalStart  = microtime();

  // publishMode: auto / ivshmem-direct / ivshmem-d3d12-copy / cpu-staging
  const char * pm = option_get_string("wgc", "publishMode");
  if      (pm && strcmp(pm, "ivshmem-direct") == 0)
    this->publishMode = WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT;
  else if (pm && strcmp(pm, "ivshmem-d3d12-copy") == 0)
    this->publishMode = WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY;
  else if (pm && strcmp(pm, "cpu-staging"   ) == 0)
    this->publishMode = WGC_CAPTURE_PUBLISH_CPU_STAGING;
  else
  {
    if (pm && strcmp(pm, "auto") != 0)
      DEBUG_WARN("Unknown wgc:publishMode \"%s\", defaulting to auto", pm);
    this->publishMode = WGC_CAPTURE_PUBLISH_AUTO;
  }

  // encoding: auto/bgra8/rgba16f/nv12. publishFormat is kept as a deprecated
  // alias so old launch scripts still resolve to the same format.
  const char * enc = option_get_string("wgc", "encoding");
  const char * pf  = option_get_string("wgc", "publishFormat");
  if ((!enc || strcmp(enc, "auto") == 0) && pf && strcmp(pf, "auto") != 0)
  {
    DEBUG_INFO("wgc:publishFormat is deprecated; use wgc:encoding instead");
    enc = pf;
  }

  this->requestedEncoding = wgc_capture_parseEncoding(enc,
    WGC_CAPTURE_PUBLISH_FORMAT_AUTO, "encoding");
  this->publishFormat = this->requestedEncoding;
  this->sdrEncoding = wgc_capture_parseEncoding(
    option_get_string("wgc", "sdrEncoding"),
    WGC_CAPTURE_PUBLISH_FORMAT_NV12, "sdrEncoding");
  this->hdrEncoding = wgc_capture_parseEncoding(
    option_get_string("wgc", "hdrEncoding"),
    WGC_CAPTURE_PUBLISH_FORMAT_P010, "hdrEncoding");
  this->hdrMode = wgc_capture_parseHDRMode(option_get_string("wgc", "hdrMode"));

  wgc_capture_forcePublishModeForEncoding();

  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    this->frameDamage[i].count = -1; // unknown -> first frame is full damage

  if (!wgc_createInstance(&this->wgc, frameBuffers, WGC_PUBLISH_CPU_STAGING))
  {
    DEBUG_ERROR("wgc_createInstance failed");
    free(this);
    this = NULL;
    return false;
  }

  wgc_setPointerCallbacks(this->wgc, getPointerBufferFn, postPointerBufferFn);
  if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ)
    wgc_setCaptureFormatHint(this->wgc,
      (unsigned)DXGI_FORMAT_R16G16B16A16_FLOAT);

  DEBUG_INFO("WGC (top-level): trackDamage:%d frameBuffers:%u debug:%d timings:%d debugStats:%d dirtyFullCopyPercent:%d",
    this->trackDamage, frameBuffers, this->debug, this->timings,
    this->debugStats, this->dirtyFullCopyPercent);

  return true;
}

static bool wgc_capture_ensureInstance(void)
{
  if (this->wgc)
    return true;

  if (!wgc_createInstance(&this->wgc, this->frameBufferCount,
        WGC_PUBLISH_CPU_STAGING))
  {
    DEBUG_ERROR("wgc_createInstance failed");
    return false;
  }

  wgc_setPointerCallbacks(this->wgc,
    this->getPointerBufferFn, this->postPointerBufferFn);
  if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ)
    wgc_setCaptureFormatHint(this->wgc,
      (unsigned)DXGI_FORMAT_R16G16B16A16_FLOAT);

  return true;
}

static wchar_t * utf8ToWide(const char * str)
{
  if (!str)
    return NULL;
  int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
  if (len <= 0)
    return NULL;
  wchar_t * out = calloc(len, sizeof(wchar_t));
  if (!out)
    return NULL;
  MultiByteToWideChar(CP_UTF8, 0, str, -1, out, len);
  return out;
}

static bool createHString(const WCHAR * str, HSTRING * result)
{
  const HRESULT hr = WindowsCreateString(str, (UINT32)wcslen(str), result);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("WindowsCreateString failed", hr);
    return false;
  }
  return true;
}

static bool wgc_canCreateCaptureItemForMonitor(HMONITOR monitor,
  const wchar_t * name)
{
  HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
  {
    DEBUG_WINERROR("RoInitialize failed while validating WGC output", hr);
    return false;
  }

  HSTRING className = NULL;
  if (!createHString(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem,
      &className))
    return false;

  IGraphicsCaptureItemInterop * interop = NULL;
  hr = RoGetActivationFactory(className, &IID_IGraphicsCaptureItemInterop,
    (void **)&interop);
  WindowsDeleteString(className);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get GraphicsCaptureItem interop while validating "
      "WGC output", hr);
    return false;
  }

  IGraphicsCaptureItem * item = NULL;
  hr = IGraphicsCaptureItemInterop_CreateForMonitor(
    interop, monitor, &IID_IGraphicsCaptureItem, (void **)&item);
  IGraphicsCaptureItemInterop_Release(interop);
  if (FAILED(hr))
  {
    DEBUG_WARN("Skipping WGC output %ls: CreateForMonitor failed "
      "(hr=0x%08lx)", name ? name : L"(unknown)", (unsigned long)hr);
    return false;
  }

  IGraphicsCaptureItem_Release(item);
  return true;
}

static bool wgc_capture_enumerate(IDXGIFactory2 * factory,
  IDXGIAdapter1 ** outAdapter, IDXGIOutput ** outOutput)
{
  const char * optAdapterRaw = option_get_string("wgc", "adapter");
  const char * optOutputRaw  = option_get_string("wgc", "output" );
  wchar_t * optAdapter = utf8ToWide(optAdapterRaw);
  wchar_t * optOutput  = utf8ToWide(optOutputRaw );

  IDXGIAdapter1 * pickedAdapter = NULL;
  IDXGIOutput   * pickedOutput  = NULL;

  // skip Microsoft Basic Render Driver / QXL / QEMU std VGA — same blacklist
  // the D12 frontend uses
  static const UINT blacklist[][2] =
  {
    {0x1414, 0x008c},
    {0x1b36, 0x000d},
    {0x1234, 0x1111}
  };

  for (UINT i = 0; ; ++i)
  {
    IDXGIAdapter1 * adapter = NULL;
    HRESULT hr = IDXGIFactory2_EnumAdapters1(factory, i, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    if (FAILED(hr))
    {
      DEBUG_WINERROR("EnumAdapters1 failed", hr);
      break;
    }

    DXGI_ADAPTER_DESC1 ad;
    if (FAILED(IDXGIAdapter1_GetDesc1(adapter, &ad)))
    {
      IDXGIAdapter1_Release(adapter);
      continue;
    }

    bool skip = false;
    for (size_t b = 0; b < sizeof(blacklist)/sizeof(blacklist[0]); ++b)
      if (ad.VendorId == blacklist[b][0] && ad.DeviceId == blacklist[b][1])
      { skip = true; break; }

    if (skip || (optAdapter && wcsstr(ad.Description, optAdapter) == NULL))
    {
      IDXGIAdapter1_Release(adapter);
      continue;
    }

    for (UINT n = 0; ; ++n)
    {
      IDXGIOutput * output = NULL;
      hr = IDXGIAdapter1_EnumOutputs(adapter, n, &output);
      if (hr == DXGI_ERROR_NOT_FOUND)
        break;
      if (FAILED(hr))
        break;

      DXGI_OUTPUT_DESC od;
      if (FAILED(IDXGIOutput_GetDesc(output, &od)))
      {
        IDXGIOutput_Release(output);
        continue;
      }

      if (optOutput && wcsstr(od.DeviceName, optOutput) == NULL)
      {
        IDXGIOutput_Release(output);
        continue;
      }

      if (od.AttachedToDesktop)
      {
        if (!wgc_canCreateCaptureItemForMonitor(od.Monitor, od.DeviceName))
        {
          IDXGIOutput_Release(output);
          continue;
        }

        pickedAdapter = adapter;
        pickedOutput  = output;
        DEBUG_INFO("WGC adapter: %ls", ad.Description);
        DEBUG_INFO("WGC output : %ls", od.DeviceName);
        goto done;
      }

      IDXGIOutput_Release(output);
    }

    IDXGIAdapter1_Release(adapter);
  }

done:
  free(optAdapter);
  free(optOutput);

  if (!pickedAdapter || !pickedOutput)
  {
    if (pickedAdapter) IDXGIAdapter1_Release(pickedAdapter);
    if (pickedOutput)  IDXGIOutput_Release(pickedOutput);
    DEBUG_ERROR("No suitable adapter/output found for WGC capture");
    return false;
  }

  *outAdapter = pickedAdapter;
  *outOutput  = pickedOutput;
  return true;
}

// Lazily load d3d12.dll function pointers into the global DX12 struct.
// d12.c does this in its own init; we replicate it here so we work
// independently when the d12 capture interface isn't active.
static bool ensureDX12Loaded(struct WGCCapture * cap)
{
  if (DX12.D3D12CreateDevice)
    return true;

  cap->d3d12Module = LoadLibrary("d3d12.dll");
  if (!cap->d3d12Module)
  {
    DEBUG_WARN("LoadLibrary(d3d12.dll) failed");
    return false;
  }

  DX12.D3D12CreateDevice = (typeof(DX12.D3D12CreateDevice))
    GetProcAddress(cap->d3d12Module, "D3D12CreateDevice");
  DX12.D3D12GetDebugInterface = (typeof(DX12.D3D12GetDebugInterface))
    GetProcAddress(cap->d3d12Module, "D3D12GetDebugInterface");
  DX12.D3D12SerializeVersionedRootSignature =
    (typeof(DX12.D3D12SerializeVersionedRootSignature))
      GetProcAddress(cap->d3d12Module, "D3D12SerializeVersionedRootSignature");

  if (!DX12.D3D12CreateDevice || !DX12.D3D12GetDebugInterface)
  {
    DEBUG_WARN("d3d12.dll missing required exports");
    return false;
  }
  return true;
}

// Probe whether ROW_MAJOR TEXTURE2D + given format is creatable in the
// IVSHMEM heap. Mirrors d12.c's d12_heapTestTexture but standalone here.
static bool ivshmemHeapTestTexture(ID3D12Device3 * device, ID3D12Heap * heap,
  DXGI_FORMAT format)
{
  D3D12_RESOURCE_DESC desc =
  {
    .Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Alignment          = 0,
    .Width              = 256,
    .Height             = 4,
    .DepthOrArraySize   = 1,
    .MipLevels          = 1,
    .Format             = format,
    .SampleDesc.Count   = 1,
    .SampleDesc.Quality = 0,
    .Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags              = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER
  };
  ID3D12Resource * probe = NULL;
  HRESULT hr = ID3D12Device3_CreatePlacedResource(device, heap, 0, &desc,
    D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&probe);
  if (FAILED(hr))
    return false;
  if (probe) ID3D12Resource_Release(probe);
  return ID3D12Device3_GetDeviceRemovedReason(device) == S_OK;
}

// Set up the IVSHMEM-direct publish path: D3D12 device + queue, IVSHMEM
// heap, format probe, D3D11On12 device, then wire wgc_setLoanedDevices +
// wgc_setIvshmemEnv on the WGC backend.
//
// Returns true on success (publishMode now committed to ivshmem-direct).
// Returns false on any failure (caller falls back to cpu-staging).
//
// On failure the partial state is cleaned up so the cpu-staging fallback
// has a clean slate.
static bool setupIvshmemDirect(IDXGIAdapter1 * adapter,
  unsigned width, unsigned height)
{
  if (!this->ivshmemBase)
  {
    DEBUG_WARN("ivshmem-direct: no ivshmem base address (init() called "
      "with NULL?)");
    return false;
  }

  if (!ensureDX12Loaded(this))
    return false;

  HRESULT hr = DX12.D3D12CreateDevice(
    (IUnknown *)adapter,
    D3D_FEATURE_LEVEL_12_0,
    &IID_ID3D12Device3,
    (void **)&this->d3d12Device);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("ivshmem-direct: D3D12CreateDevice failed", hr);
    return false;
  }

  D3D12_COMMAND_QUEUE_DESC qDesc =
  {
    .Type     = D3D12_COMMAND_LIST_TYPE_DIRECT,
    .Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
    .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE
  };
  hr = ID3D12Device3_CreateCommandQueue(this->d3d12Device, &qDesc,
    &IID_ID3D12CommandQueue, (void **)&this->d3d12Queue);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("ivshmem-direct: CreateCommandQueue failed", hr);
    return false;
  }

  hr = ID3D12Device3_OpenExistingHeapFromAddress(this->d3d12Device,
    this->ivshmemBase, &IID_ID3D12Heap, (void **)&this->ivshmemHeap);
  if (FAILED(hr))
  {
    DEBUG_WARN("ivshmem-direct: OpenExistingHeapFromAddress failed (hr=0x%08lx)",
      (unsigned long)hr);
    return false;
  }

  const DXGI_FORMAT chosenFormat =
    this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA16F
      ? DXGI_FORMAT_R16G16B16A16_FLOAT
    : this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ
      ? DXGI_FORMAT_R8G8B8A8_UNORM
      : this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12
      ? DXGI_FORMAT_R8G8B8A8_UNORM
      : this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_P010
      ? DXGI_FORMAT_R16G16B16A16_UINT
      : DXGI_FORMAT_B8G8R8A8_UNORM;

  if (!ivshmemHeapTestTexture(this->d3d12Device, this->ivshmemHeap, chosenFormat))
  {
    DEBUG_WARN("ivshmem-direct: ROW_MAJOR TEXTURE2D + format 0x%x not "
      "supported in IVSHMEM heap", (unsigned)chosenFormat);
    return false;
  }

  // D3D11On12 device — gives us a D3D11 device + context bound to the
  // D3D12 device. WGC backend uses this loaned D3D11 (instead of creating
  // its own) so the wrapped textures are usable from its context.
  IUnknown * queues[] = { (IUnknown *)this->d3d12Queue };
  D3D_FEATURE_LEVEL fl;
  hr = D3D11On12CreateDevice(
    (IUnknown *)this->d3d12Device,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT |
      (this->debug ? D3D11_CREATE_DEVICE_DEBUG : 0),
    NULL, 0,
    queues, 1,
    0,
    &this->d11Device, &this->d11Context, &fl);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("ivshmem-direct: D3D11On12CreateDevice failed", hr);
    return false;
  }
  hr = ID3D11Device_QueryInterface(this->d11Device,
    &IID_ID3D11On12Device, (void **)&this->d11on12Device);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("ivshmem-direct: QueryInterface ID3D11On12Device failed", hr);
    return false;
  }

  wgc_setLoanedDevices(this->wgc,
    (IUnknown *)this->d11Device,
    (IUnknown *)this->d11Context,
    (IUnknown *)this->d3d12Device);

  if (!wgc_setIvshmemEnv(this->wgc,
        (IUnknown *)this->ivshmemHeap,
        (IUnknown *)this->d11on12Device,
        width, height,
        (unsigned)chosenFormat))
  {
    DEBUG_ERROR("ivshmem-direct: wgc_setIvshmemEnv failed");
    return false;
  }

  DEBUG_INFO("ivshmem-direct: ready (%ux%u, format 0x%x)",
    width, height, (unsigned)chosenFormat);
  return true;
}

static bool setupIvshmemD3D12Copy(IDXGIAdapter1 * adapter,
  unsigned width, unsigned height)
{
  if (!this->ivshmemBase)
  {
    DEBUG_WARN("ivshmem-d3d12-copy: no ivshmem base address");
    return false;
  }

  if (!ensureDX12Loaded(this))
    return false;

  HRESULT hr = DX12.D3D12CreateDevice(
    (IUnknown *)adapter,
    D3D_FEATURE_LEVEL_12_0,
    &IID_ID3D12Device3,
    (void **)&this->d3d12Device);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("ivshmem-d3d12-copy: D3D12CreateDevice failed", hr);
    return false;
  }

  D3D12_COMMAND_QUEUE_DESC qDesc =
  {
    .Type     = D3D12_COMMAND_LIST_TYPE_COPY,
    .Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
    .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE
  };
  hr = ID3D12Device3_CreateCommandQueue(this->d3d12Device, &qDesc,
    &IID_ID3D12CommandQueue, (void **)&this->d3d12Queue);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("ivshmem-d3d12-copy: CreateCommandQueue failed", hr);
    return false;
  }

  hr = ID3D12Device3_OpenExistingHeapFromAddress(this->d3d12Device,
    this->ivshmemBase, &IID_ID3D12Heap, (void **)&this->ivshmemHeap);
  if (FAILED(hr))
  {
    DEBUG_WARN("ivshmem-d3d12-copy: OpenExistingHeapFromAddress failed "
      "(hr=0x%08lx)", (unsigned long)hr);
    return false;
  }

  const DXGI_FORMAT chosenFormat =
    this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA16F
      ? DXGI_FORMAT_R16G16B16A16_FLOAT
    : this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ
      ? DXGI_FORMAT_R8G8B8A8_UNORM
      : this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12
      ? DXGI_FORMAT_R8G8B8A8_UNORM
      : this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_P010
      ? DXGI_FORMAT_R16G16B16A16_UINT
      : DXGI_FORMAT_B8G8R8A8_UNORM;

  if (!ivshmemHeapTestTexture(this->d3d12Device, this->ivshmemHeap,
      chosenFormat))
  {
    DEBUG_WARN("ivshmem-d3d12-copy: ROW_MAJOR TEXTURE2D + format 0x%x not "
      "supported in IVSHMEM heap", (unsigned)chosenFormat);
    return false;
  }

  wgc_setLoanedDevices(this->wgc, NULL, NULL, (IUnknown *)this->d3d12Device);

  if (!wgc_setIvshmemD3D12CopyEnv(this->wgc,
        (IUnknown *)this->ivshmemHeap,
        (IUnknown *)this->d3d12Queue,
        width, height,
        (unsigned)chosenFormat))
  {
    DEBUG_ERROR("ivshmem-d3d12-copy: wgc_setIvshmemD3D12CopyEnv failed");
    return false;
  }

  DEBUG_INFO("ivshmem-d3d12-copy: ready (%ux%u, format 0x%x)",
    width, height, (unsigned)chosenFormat);
  return true;
}

// Tear down whatever setupIvshmemDirect created. Safe to call even on
// partial setup (NULL-checks each pointer).
static void teardownIvshmemDirect(void)
{
  if (this->d11on12Device) { ID3D11On12Device_Release(this->d11on12Device); this->d11on12Device = NULL; }
  if (this->d11Context   ) { ID3D11DeviceContext_Release(this->d11Context  ); this->d11Context    = NULL; }
  if (this->d11Device    ) { ID3D11Device_Release(this->d11Device    );       this->d11Device     = NULL; }
  if (this->ivshmemHeap  ) { ID3D12Heap_Release(this->ivshmemHeap   );        this->ivshmemHeap   = NULL; }
  if (this->d3d12Queue   ) { ID3D12CommandQueue_Release(this->d3d12Queue   ); this->d3d12Queue    = NULL; }
  if (this->d3d12Device  ) { ID3D12Device3_Release(this->d3d12Device  );      this->d3d12Device   = NULL; }
}

static bool wgc_capture_init(void * ivshmemBase, unsigned * alignSize)
{
  if (!d12_comScope)
    comRef_initGlobalScope(512, d12_comScope);

  if (!wgc_capture_ensureInstance())
    return false;

  // Stash for setupIvshmemDirect to use later
  this->ivshmemBase = ivshmemBase;

  IDXGIFactory2 * factory = NULL;
  HRESULT hr = CreateDXGIFactory2(
    this->debug ? DXGI_CREATE_FACTORY_DEBUG : 0,
    &IID_IDXGIFactory2, (void **)&factory);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("CreateDXGIFactory2 failed", hr);
    return false;
  }

  IDXGIAdapter1 * adapter = NULL;
  IDXGIOutput   * output  = NULL;
  if (!wgc_capture_enumerate(factory, &adapter, &output))
  {
    IDXGIFactory2_Release(factory);
    return false;
  }

  // store the COM refs locally; they're released in deinit
  this->factory = malloc(sizeof(*this->factory)); *this->factory = factory;
  this->adapter = malloc(sizeof(*this->adapter)); *this->adapter = adapter;
  this->output  = malloc(sizeof(*this->output )); *this->output  = output;

  const DXGI_COLOR_SPACE_TYPE colorSpace =
    wgc_capture_getOutputColorSpace(output);
  wgc_capture_resolveEncoding(colorSpace);
  wgc_capture_forcePublishModeForEncoding();
  if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ)
    wgc_setCaptureFormatHint(this->wgc,
      (unsigned)DXGI_FORMAT_R16G16B16A16_FLOAT);

  // Try GPU-to-IVSHMEM setup if the user requested it (or auto). On failure
  // we fall back to the next publish path.
  if (this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY ||
      this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT ||
      this->publishMode == WGC_CAPTURE_PUBLISH_AUTO)
  {
    DXGI_OUTPUT_DESC outputDesc;
    if (SUCCEEDED(IDXGIOutput_GetDesc(output, &outputDesc)))
    {
      const RECT r = outputDesc.DesktopCoordinates;
      const unsigned w = (unsigned)(r.right  - r.left);
      const unsigned h = (unsigned)(r.bottom - r.top );
      if ((this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY ||
           this->publishMode == WGC_CAPTURE_PUBLISH_AUTO) &&
          setupIvshmemD3D12Copy(adapter, w, h))
      {
        this->publishMode = WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY;
        D3D12_HEAP_DESC hd = ID3D12Heap_GetDesc(this->ivshmemHeap);
        if (hd.Alignment > *alignSize)
          *alignSize = (unsigned)hd.Alignment;
      }
      else
      {
        const bool explicitD3D12Copy =
          this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY;
        teardownIvshmemDirect();
        if (explicitD3D12Copy)
        {
          DEBUG_WARN("ivshmem-d3d12-copy: setup failed, falling back to cpu-staging");
          this->publishMode = WGC_CAPTURE_PUBLISH_CPU_STAGING;
          if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12 ||
              this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ ||
              this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_P010)
          {
            DEBUG_WARN("cpu-staging does not support packed YUV transport; "
              "falling back to BGRA8");
            this->publishFormat = WGC_CAPTURE_PUBLISH_FORMAT_BGRA8;
          }
        }
      }

      if (this->publishMode != WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY &&
          (this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT ||
           this->publishMode == WGC_CAPTURE_PUBLISH_AUTO) &&
          setupIvshmemDirect(adapter, w, h))
      {
        // ivshmem-direct committed
        this->publishMode = WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT;
        // align IVSHMEM allocations to D3D12 placed-resource alignment so
        // each FrameBuffer.data lands at an offset suitable for placement
        D3D12_HEAP_DESC hd = ID3D12Heap_GetDesc(this->ivshmemHeap);
        if (hd.Alignment > *alignSize)
          *alignSize = (unsigned)hd.Alignment;
      }
      else if (this->publishMode != WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY &&
               this->publishMode != WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT)
      {
        teardownIvshmemDirect();
        this->publishMode = WGC_CAPTURE_PUBLISH_CPU_STAGING;
        if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12 ||
            this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ ||
            this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_P010)
        {
          DEBUG_WARN("cpu-staging does not support packed YUV transport; "
            "falling back to BGRA8");
          this->publishFormat = WGC_CAPTURE_PUBLISH_FORMAT_BGRA8;
        }
      }
      else if (this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT)
      {
        teardownIvshmemDirect();
        DEBUG_WARN("ivshmem-direct: setup failed, falling back to cpu-staging");
        this->publishMode = WGC_CAPTURE_PUBLISH_CPU_STAGING;
        if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12 ||
            this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ ||
            this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_P010)
        {
          DEBUG_WARN("cpu-staging does not support packed YUV transport; "
            "falling back to BGRA8");
          this->publishFormat = WGC_CAPTURE_PUBLISH_FORMAT_BGRA8;
        }
      }
    }
    else
    {
      DEBUG_WARN("Could not query output desc; falling back to cpu-staging");
      this->publishMode = WGC_CAPTURE_PUBLISH_CPU_STAGING;
      if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12 ||
          this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ ||
          this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_P010)
      {
        DEBUG_WARN("cpu-staging does not support packed YUV transport; "
          "falling back to BGRA8");
        this->publishFormat = WGC_CAPTURE_PUBLISH_FORMAT_BGRA8;
      }
    }
  }

  if (!wgc_initInstance(this->wgc, this->debug, adapter, output,
      this->trackDamage))
  {
    DEBUG_ERROR("wgc_initInstance failed");
    return false;
  }

  // A Windows display topology change can tear down and rebuild the capture
  // transport while returning to the same dimensions and pitch. Force the
  // client to rebuild its texture/import state after every successful WGC
  // init instead of relying only on width/height/pitch deltas.
  ++this->formatVer;

  return true;
}

static void wgc_capture_stop(void) { }

static bool wgc_capture_deinit(void)
{
  if (!this)
    return true;

  if (this->wgc)
  {
    wgc_deinitInstance(this->wgc);
    wgc_freeInstance(&this->wgc);
  }
  memset(this->ivshmemSlotRegistered, 0,
    sizeof(this->ivshmemSlotRegistered));
  this->width       = 0;
  this->height      = 0;
  this->pitch       = 0;
  this->mappedPitch = 0;
  this->stride      = 0;
  this->dataHeight  = 0;
  this->mapped      = NULL;
  this->frameMapped = false;

  // Tear down ivshmem-direct state BEFORE the WGC instance is freed —
  // wgc_freeInstance happens in wgc_capture_free, but the loaned devices
  // we hand the WGC backend are owned here and the backend stops touching
  // them after wgc_deinitInstance returns.
  teardownIvshmemDirect();

  if (this->output  && *this->output ) { IDXGIOutput_Release  (*this->output ); *this->output  = NULL; }
  if (this->adapter && *this->adapter) { IDXGIAdapter1_Release(*this->adapter); *this->adapter = NULL; }
  if (this->factory && *this->factory) { IDXGIFactory2_Release(*this->factory); *this->factory = NULL; }

  free(this->output ); this->output  = NULL;
  free(this->adapter); this->adapter = NULL;
  free(this->factory); this->factory = NULL;

  comRef_freeGlobalScope(d12_comScope);

  if (this->d3d12Module)
  {
    FreeLibrary(this->d3d12Module);
    this->d3d12Module = NULL;
    DX12.D3D12CreateDevice = NULL;
    DX12.D3D12GetDebugInterface = NULL;
    DX12.D3D12SerializeVersionedRootSignature = NULL;
  }

  return true;
}

static void wgc_capture_free(void)
{
  if (!this)
    return;

  if (this->wgc)
    wgc_freeInstance(&this->wgc);

  free(this->encodeBuffer);
  free(this);
  this = NULL;
}

static CaptureResult wgc_capture_capture(unsigned frameBufferIndex,
  FrameBuffer * frame)
{
  // In IVSHMEM GPU-publish modes, register this frameBuffer's IVSHMEM offset on
  // first capture for this index. After that, WGC writes pixel data
  // directly into IVSHMEM at this offset.
  if ((this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT ||
       this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY) &&
      frameBufferIndex < LGMP_Q_FRAME_LEN &&
      !this->ivshmemSlotRegistered[frameBufferIndex] &&
      frame)
  {
    void * dataPtr = framebuffer_get_data(frame);
    if (dataPtr && dataPtr >= this->ivshmemBase)
    {
      const uint64_t offset =
        (uint64_t)((uint8_t *)dataPtr - (uint8_t *)this->ivshmemBase);
      // Conservative size estimate — enough for full-frame at the
      // configured format. The backend uses this for the heap-test
      // sanity check only.
      const uint64_t bpp = this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA16F
        ? 8 :
        this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12 ? 2 : 4;
      // We don't yet know precise dimensions; use the env values from setup
      const uint64_t size = bpp * 3840 * 2160; // upper bound for 4K
      if (wgc_setIvshmemSlot(this->wgc, frameBufferIndex, offset, size))
      {
        this->ivshmemSlotRegistered[frameBufferIndex] = true;
        DEBUG_INFO("ivshmem GPU publish: registered slot %u at offset 0x%llx",
          frameBufferIndex, (unsigned long long)offset);
      }
      else
        DEBUG_ERROR("wgc_setIvshmemSlot %u failed", frameBufferIndex);
    }
  }

  return wgc_pollFrame(this->wgc, frameBufferIndex);
}

static CaptureResult wgc_capture_waitFrame(unsigned frameBufferIndex,
  CaptureFrame * frame, const size_t maxFrameSize)
{
  unsigned width, height, pitch;
  void * map = NULL;

  memset(&this->desc, 0, sizeof(this->desc));
  const uint64_t readbackStart = microtime();
  LG_PROFILE_ZONE_BEGIN(zoneFetchCpu, "wgc cpu fetch/map");

  const bool gpuPublish =
    this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT ||
    this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY;
  bool fetched;
  if (gpuPublish)
  {
    // No mapping — WGC has already written directly into IVSHMEM at the
    // slot's offset. We just need the metadata (dimensions, dirty rects)
    // and the slot state transition.
    uint64_t off;
    fetched = wgc_fetchIvshmemDirect(this->wgc, frameBufferIndex,
      &this->desc, &off, &pitch, &width, &height);
  }
  else
  {
    fetched = wgc_fetchCpu(this->wgc, frameBufferIndex, &this->desc,
      &map, &pitch, &width, &height);
  }

  if (!fetched)
  {
    LG_PROFILE_ZONE_END(zoneFetchCpu);
    return CAPTURE_RESULT_TIMEOUT;
  }
  LG_PROFILE_ZONE_END(zoneFetchCpu);
  const uint64_t readbackUs = microtime() - readbackStart;
  if (this->timings || this->debugStats)
  {
    this->cpuReadbackTotalUs += readbackUs;
    this->cpuReadbackMaxUs = max(this->cpuReadbackMaxUs, readbackUs);
    ++this->cpuReadbackCount;
  }

  const bool rgba16f = this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA16F;
  const bool nv12    = this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12;
  const bool p010    = this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_P010;
  const bool rgba10pq =
    this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ;
  const bool packedYuv = nv12 || p010;
  const unsigned bpp = (rgba16f || p010) ? 8 : 4;
  const unsigned outputPitch = rgba10pq ? width * 4 : pitch;

  // Reject formats we don't handle in this wrapper. Today WGC delivers BGRA8
  // by default; explicit RGBA16F publish mode writes half-float pixels.
  if (rgba10pq && !gpuPublish && pitch < width * 8)
  {
    DEBUG_ERROR("WGC rgba10pq requires RGBA16F source pitch, got %u for width %u",
      pitch, width);
    wgc_releaseCpu(this->wgc, this->desc.backendToken);
    return CAPTURE_RESULT_ERROR;
  }
  if (rgba10pq && gpuPublish && pitch < outputPitch)
  {
    DEBUG_ERROR("WGC rgba10pq GPU publish produced an unexpected pitch %u "
      "for width %u", pitch, width);
    return CAPTURE_RESULT_ERROR;
  }
  if (!rgba10pq && !packedYuv && pitch < width * bpp)
  {
    DEBUG_ERROR("WGC produced an unexpected pitch %u for width %u; "
      "use --capture=D12WGC for HDR / format conversion", pitch, width);
    wgc_releaseCpu(this->wgc, this->desc.backendToken);
    return CAPTURE_RESULT_ERROR;
  }

  const unsigned dataHeight = packedYuv ? height + (height + 1) / 2 : height;

  // bump formatVer if dimensions changed
  if (this->width != width || this->height != height ||
      this->pitch != outputPitch || this->mappedPitch != pitch)
    ++this->formatVer;

  this->width       = width;
  this->height      = height;
  this->mappedPitch = pitch;
  this->pitch       = outputPitch;
  this->stride      = outputPitch / bpp;
  this->dataHeight  = dataHeight;
  this->mapped      = map;
  this->frameMapped = true;

  const unsigned maxRows = (unsigned)(maxFrameSize / outputPitch);
  const unsigned outRows = (maxRows < dataHeight) ? maxRows : dataHeight;

  frame->formatVer        = this->formatVer;
  frame->screenWidth      = width;
  frame->screenHeight     = height;
  frame->dataWidth        = packedYuv ? pitch / bpp : width;
  frame->dataHeight       = outRows;
  frame->frameWidth       = width;
  frame->frameHeight      = height;
  frame->truncated        = outRows < dataHeight;
  frame->pitch            = outputPitch;
  frame->stride           = this->stride;
  frame->format           = rgba16f ? CAPTURE_FMT_RGBA16F :
                            rgba10pq ? CAPTURE_FMT_RGBA10 :
                            nv12    ? CAPTURE_FMT_NV12 :
                            p010    ? CAPTURE_FMT_P010 : CAPTURE_FMT_BGRA;
  frame->hdr              = rgba16f || p010 || rgba10pq;
  frame->hdrPQ            = p010 || rgba10pq;
  frame->rotation         = CAPTURE_ROT_0;
  frame->hasBackendFrameTime = this->desc.hasBackendFrameTime;
  frame->backendFrameTimeUs  = this->desc.backendFrameTimeUs;

  // Publish the dirty rects as client render damage. The pixel transport may
  // have used a full-frame IVSHMEM copy; damage still tells the client what
  // it needs to redraw.
  FrameDamageRect merged[KVMFR_MAX_DAMAGE_RECTS];
  int mergedCount = 0;
  if (packedYuv)
  {
    // Packed YUV is encoded from BGRA/RGBA16F into a subsampled transport. Until damage is
    // expanded for chroma blocks and overlay edge cases, present the whole
    // frame to avoid stale output artifacts on alt-tab/compositor changes.
    merged[0].x      = 0;
    merged[0].y      = 0;
    merged[0].width  = width;
    merged[0].height = height;
    mergedCount = 1;
  }
  else for (unsigned i = 0; i < this->desc.nbDirtyRects &&
            mergedCount < KVMFR_MAX_DAMAGE_RECTS; ++i)
  {
    const RECT * r = &this->desc.dirtyRects[i];
    merged[mergedCount].x      = r->left;
    merged[mergedCount].y      = r->top;
    merged[mergedCount].width  = r->right  - r->left;
    merged[mergedCount].height = r->bottom - r->top;
    ++mergedCount;
  }
  if (mergedCount > 1)
  {
    mergedCount = rectsMergeOverlapping(merged, mergedCount);
    mergedCount = rectsRejectContained(merged, mergedCount);
  }
  memcpy(frame->damageRects, merged,
    (size_t)mergedCount * sizeof(*frame->damageRects));
  frame->damageRectsCount = (unsigned)mergedCount;

  return CAPTURE_RESULT_OK;
}

static CaptureResult wgc_capture_getFrame(unsigned frameBufferIndex,
  FrameBuffer * frame, const size_t maxFrameSize)
{
  (void)maxFrameSize;

  if (!this->frameMapped)
    return CAPTURE_RESULT_ERROR;

  // IVSHMEM GPU-publish: WGC already wrote pixels directly into IVSHMEM at the
  // slot's offset (and Flush in wgc_fetchIvshmemDirect committed the GPU
  // work). All we need to do is signal write-pointer completion and
  // release the slot back to the WGC backend.
  if (this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_DIRECT ||
      this->publishMode == WGC_CAPTURE_PUBLISH_IVSHMEM_D3D12_COPY)
  {
    framebuffer_set_write_ptr(frame, (uint32_t)((size_t)this->pitch * this->dataHeight));
    wgc_releaseIvshmemDirect(this->wgc, this->desc.backendToken);
    this->frameMapped = false;
    this->mapped      = NULL;
    this->desc.backendToken = NULL;

    // Update damage accumulator the same way the cpu-staging path does
    // (clears the just-published fb, accumulates rects into the others)
    for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    {
      FrameDamage * d = &this->frameDamage[i];
      if (i == frameBufferIndex)
        d->count = 0;
      else if (this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_NV12)
        d->count = -1;
      else if (this->desc.nbDirtyRects == 0 ||
        (d->count >= 0 &&
         (unsigned)d->count + this->desc.nbDirtyRects > KVMFR_MAX_DAMAGE_RECTS))
        d->count = -1;
      else if (d->count >= 0)
      {
        for (unsigned j = 0; j < this->desc.nbDirtyRects; ++j)
        {
          const RECT * r = &this->desc.dirtyRects[j];
          d->rects[d->count].x      = r->left;
          d->rects[d->count].y      = r->top;
          d->rects[d->count].width  = r->right  - r->left;
          d->rects[d->count].height = r->bottom - r->top;
          ++d->count;
        }
        d->count = rectsMergeOverlapping(d->rects, d->count);
        d->count = rectsRejectContained(d->rects, d->count);
        if (wgc_capture_damageTooLarge(d->rects, d->count))
          d->count = -1;
      }
    }
    return CAPTURE_RESULT_OK;
  }

  FrameDamage * damage = &this->frameDamage[frameBufferIndex];
  const unsigned bpp = 4;
  const bool rgba10pq =
    this->publishFormat == WGC_CAPTURE_PUBLISH_FORMAT_RGBA10PQ;

  // Build the damage list to copy for THIS framebuffer:
  //   - if damage->count < 0 (first use, or overflow): full copy
  //   - else: prior accumulated rects + this frame's new dirty rects
  bool fullCopy = damage->count < 0 || this->desc.nbDirtyRects == 0 ||
    (unsigned)damage->count + this->desc.nbDirtyRects > KVMFR_MAX_DAMAGE_RECTS;
  if (rgba10pq)
    fullCopy = true;
  FrameDamage local = { 0 };

  if (!fullCopy)
  {
    // append the new frame's dirty rects to the framebuffer's accumulator,
    // then copy the merged set
    local = *damage;
    for (unsigned i = 0; i < this->desc.nbDirtyRects; ++i)
    {
      const RECT * r = &this->desc.dirtyRects[i];
      local.rects[local.count].x      = r->left;
      local.rects[local.count].y      = r->top;
      local.rects[local.count].width  = r->right  - r->left;
      local.rects[local.count].height = r->bottom - r->top;
      ++local.count;
    }
    local.count = rectsMergeOverlapping(local.rects, local.count);
    local.count = rectsRejectContained(local.rects, local.count);
    fullCopy = wgc_capture_damageTooLarge(local.rects, local.count);
  }

  uint64_t copyPixels = 0;
  const uint64_t memcpyStart = microtime();
  LG_PROFILE_ZONE_BEGIN(zoneMemcpy, "wgc ivshmem copy");
  if (fullCopy)
  {
    const void * src = this->mapped;
    if (rgba10pq)
    {
      if (!wgc_capture_encodeRGBA10PQ())
      {
        LG_PROFILE_ZONE_END(zoneMemcpy);
        wgc_releaseCpu(this->wgc, this->desc.backendToken);
        this->frameMapped = false;
        this->mapped      = NULL;
        this->desc.backendToken = NULL;
        return CAPTURE_RESULT_ERROR;
      }
      src = this->encodeBuffer;
    }
    framebuffer_write(frame, src, (size_t)this->pitch * this->dataHeight);
    copyPixels = (uint64_t)this->width * this->dataHeight;
  }
  else
  {
    rectsBufferToFramebuffer(local.rects, local.count, bpp, frame,
      this->pitch, this->dataHeight, this->mapped, this->pitch);
    for (int i = 0; i < local.count; ++i)
      copyPixels += (uint64_t)local.rects[i].width * local.rects[i].height;

    if (this->timings || this->debugStats)
      this->copyRects += local.count;
  }
  LG_PROFILE_ZONE_VALUE(zoneMemcpy, copyPixels);
  LG_PROFILE_ZONE_END(zoneMemcpy);
  LG_PROFILE_PLOT_I("WGC dirty rects", fullCopy ? 0 : local.count);
  LG_PROFILE_PLOT_I("WGC copied kpix", copyPixels / 1000);
  const uint64_t memcpyUs = microtime() - memcpyStart;
  if (this->timings || this->debugStats)
  {
    this->cpuMemcpyTotalUs += memcpyUs;
    this->cpuMemcpyMaxUs = max(this->cpuMemcpyMaxUs, memcpyUs);
    ++this->cpuMemcpyCount;
    if (this->desc.hasBackendFrameTime)
    {
      const uint64_t now = microtime();
      if (now >= this->desc.backendFrameTimeUs)
      {
        const uint64_t callbackPostUs = now - this->desc.backendFrameTimeUs;
        this->callbackPostTotalUs += callbackPostUs;
        this->callbackPostMaxUs = max(this->callbackPostMaxUs, callbackPostUs);
        ++this->callbackPostCount;
      }
    }
    if (fullCopy)
      ++this->copyFull;
    else
      ++this->copyDirty;
    this->copyPixels += copyPixels;
    wgc_capture_logStats();
  }

  // Update each framebuffer's damage accumulator. The one we just published
  // resets to zero (it's now in sync with the source). The others accumulate
  // the new frame's dirty rects so they know what to copy when they're next
  // used. Overflow → mark unknown so the next publish full-copies.
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    FrameDamage * d = &this->frameDamage[i];
    if (i == frameBufferIndex)
    {
      d->count = 0;
    }
    else if (d->count >= 0 &&
        (unsigned)d->count + this->desc.nbDirtyRects <= KVMFR_MAX_DAMAGE_RECTS &&
        this->desc.nbDirtyRects > 0)
    {
      for (unsigned j = 0; j < this->desc.nbDirtyRects; ++j)
      {
        const RECT * r = &this->desc.dirtyRects[j];
        d->rects[d->count].x      = r->left;
        d->rects[d->count].y      = r->top;
        d->rects[d->count].width  = r->right  - r->left;
        d->rects[d->count].height = r->bottom - r->top;
        ++d->count;
      }
      d->count = rectsMergeOverlapping(d->rects, d->count);
      d->count = rectsRejectContained(d->rects, d->count);
      if (wgc_capture_damageTooLarge(d->rects, d->count))
        d->count = -1;
    }
    else if (this->desc.nbDirtyRects == 0)
    {
      // full source frame → mark this fb as needing full copy too
      d->count = -1;
    }
    else if ((unsigned)d->count + this->desc.nbDirtyRects > KVMFR_MAX_DAMAGE_RECTS)
    {
      d->count = -1;
    }
  }

  wgc_releaseCpu(this->wgc, this->desc.backendToken);
  this->frameMapped = false;
  this->mapped      = NULL;
  this->desc.backendToken = NULL;

  return CAPTURE_RESULT_OK;
}

struct CaptureInterface Capture_WGC =
{
  .shortName       = "WGC",
  .asyncCapture    = false,
  .getName         = wgc_capture_getName,
  .initOptions     = wgc_capture_initOptions,
  .create          = wgc_capture_create,
  .init            = wgc_capture_init,
  .stop            = wgc_capture_stop,
  .deinit          = wgc_capture_deinit,
  .free            = wgc_capture_free,
  .capture         = wgc_capture_capture,
  .waitFrame       = wgc_capture_waitFrame,
  .getFrame        = wgc_capture_getFrame
};
