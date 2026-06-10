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

#include "wgc_stats.h"
#include "wgc_util.h"

#include "common/debug.h"
#include "common/windebug.h"
#include "common/time.h"

#include <float.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define WGC_HDR_STATS_GRID 16

void wgcStats_init(WGCStats * stats, bool enabled)
{
  memset(stats, 0, sizeof(*stats));
  stats->enabled = enabled;
}

void wgcStats_free(WGCStats * stats)
{
  if (stats->hdrStatsTexture)
  {
    ID3D11Texture2D_Release(stats->hdrStatsTexture);
    stats->hdrStatsTexture = NULL;
  }
}

void wgcStats_interlockedMax64(volatile LONG64 * target, LONG64 value)
{
  LONG64 old = InterlockedCompareExchange64(target, 0, 0);
  while(value > old &&
      InterlockedCompareExchange64(target, value, old) != old)
    old = InterlockedCompareExchange64(target, 0, 0);
}

static int wgcStats_compareLong64(const void * a, const void * b)
{
  const LONG64 av = *(const LONG64 *)a;
  const LONG64 bv = *(const LONG64 *)b;
  return (av > bv) - (av < bv);
}

static LONG64 wgcStats_percentileLong64(LONG64 * values, LONG count,
  unsigned pct)
{
  if (count <= 0)
    return 0;
  qsort(values, count, sizeof(values[0]), wgcStats_compareLong64);
  LONG idx = (LONG)(((uint64_t)pct * (uint64_t)(count - 1) + 99) / 100);
  if (idx < 0)
    idx = 0;
  if (idx >= count)
    idx = count - 1;
  return values[idx];
}

static LONG wgcStats_copyAndResetSamples(volatile LONG * sampleCount,
  LONG64 * samples, LONG64 * out)
{
  LONG count = InterlockedExchange(sampleCount, 0);
  if (count < 0)
    count = 0;
  if (count > WGC_STATS_SAMPLE_MAX)
    count = WGC_STATS_SAMPLE_MAX;
  for(LONG i = 0; i < count; ++i)
    out[i] = samples[i];
  return count;
}

static void wgcStats_recordSample(volatile LONG * sampleCount,
  volatile LONG * overflow, LONG64 * samples, LONG64 value)
{
  const LONG idx = InterlockedIncrement(sampleCount) - 1;
  if (idx >= 0 && idx < WGC_STATS_SAMPLE_MAX)
    samples[idx] = value;
  else
    InterlockedExchange(overflow, 1);
}

void wgcStats_recordProfileStage(WGCStats * stats, WGCProfileStage stage,
  uint64_t elapsedUs)
{
  if (!stats->enabled)
    return;

  InterlockedExchangeAdd64(&stats->profileTotalUs[stage], (LONG64)elapsedUs);
  InterlockedIncrement64(&stats->profileCount[stage]);
  wgcStats_interlockedMax64(&stats->profileMaxUs[stage], (LONG64)elapsedUs);
  wgcStats_recordSample(&stats->profileSampleCount[stage],
    &stats->profileSampleOverflow[stage], stats->profileSamples[stage],
    (LONG64)elapsedUs);
}

void wgcStats_recordD3D12CopySubmit(WGCStats * stats, uint64_t elapsedUs)
{
  if (!stats->enabled)
    return;

  InterlockedExchangeAdd64(&stats->d3d12CopySubmitTotalUs,
    (LONG64)elapsedUs);
  InterlockedIncrement64(&stats->d3d12CopySubmitCount);
  wgcStats_interlockedMax64(&stats->d3d12CopySubmitMaxUs, (LONG64)elapsedUs);
  wgcStats_recordSample(&stats->d3d12CopySubmitSampleCount,
    &stats->d3d12CopySubmitSampleOverflow, stats->d3d12CopySubmitSamples,
    (LONG64)elapsedUs);
}

void wgcStats_recordD3D12FenceWait(WGCStats * stats, uint64_t elapsedUs)
{
  if (!stats->enabled)
    return;

  InterlockedExchangeAdd64(&stats->d3d12FenceWaitTotalUs,
    (LONG64)elapsedUs);
  InterlockedIncrement64(&stats->d3d12FenceWaitCount);
  wgcStats_interlockedMax64(&stats->d3d12FenceWaitMaxUs, (LONG64)elapsedUs);
  wgcStats_recordSample(&stats->d3d12FenceWaitSampleCount,
    &stats->d3d12FenceWaitSampleOverflow, stats->d3d12FenceWaitSamples,
    (LONG64)elapsedUs);
}

void wgcStats_recordSystemRelativeGap(WGCStats * stats, LONG64 gapUs)
{
  if (!stats->enabled)
    return;

  InterlockedExchangeAdd64(&stats->systemRelativeGapTotalUs, gapUs);
  InterlockedIncrement64(&stats->systemRelativeGapCount);
  wgcStats_interlockedMax64(&stats->systemRelativeGapMaxUs, gapUs);
}

void wgcStats_recordCopy(WGCStats * stats, bool publishCopy, bool fullCopy,
  uint64_t pixels, uint64_t framePixels)
{
  if (!stats->enabled)
    return;

  volatile LONG64 * fullCounter  = publishCopy ?
    &stats->copyPublishFull : &stats->copyAccumFull;
  volatile LONG64 * dirtyCounter = publishCopy ?
    &stats->copyPublishDirty : &stats->copyAccumDirty;
  volatile LONG64 * pixelCounter = publishCopy ?
    &stats->copyPublishPixels : &stats->copyAccumPixels;

  InterlockedIncrement64(fullCopy ? fullCounter : dirtyCounter);
  InterlockedExchangeAdd64(pixelCounter,
    (LONG64)(fullCopy ? framePixels : pixels));
}

static bool wgcStats_ensureHDRTexture(WGCStats * stats,
  ID3D11Device5 * device)
{
  if (stats->hdrStatsTexture)
    return true;

  D3D11_TEXTURE2D_DESC desc =
  {
    .Width          = WGC_HDR_STATS_GRID,
    .Height         = WGC_HDR_STATS_GRID,
    .MipLevels      = 1,
    .ArraySize      = 1,
    .Format         = DXGI_FORMAT_R16G16B16A16_FLOAT,
    .SampleDesc     = { .Count = 1, .Quality = 0 },
    .Usage          = D3D11_USAGE_STAGING,
    .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
  };

  HRESULT hr = ID3D11Device5_CreateTexture2D(device, &desc, NULL,
    &stats->hdrStatsTexture);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("WGC HDR stats staging texture creation failed", hr);
    return false;
  }

  return true;
}

void wgcStats_maybeLogHDR(WGCStats * stats, ID3D11Device5 * device,
  ID3D11DeviceContext4 * context, ID3D11Texture2D * src)
{
  if (!stats->enabled)
    return;

  const uint64_t now = microtime();
  if (now - stats->hdrStatsLastLog < 1000000)
    return;

  D3D11_TEXTURE2D_DESC srcDesc;
  ID3D11Texture2D_GetDesc(src, &srcDesc);
  if (srcDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
    return;

  stats->hdrStatsLastLog = now;
  if (!wgcStats_ensureHDRTexture(stats, device))
    return;

  for(unsigned y = 0; y < WGC_HDR_STATS_GRID; ++y)
  {
    const UINT sy = WGC_HDR_STATS_GRID == 1 ? 0 :
      (UINT)(((uint64_t)y * (srcDesc.Height - 1)) /
        (WGC_HDR_STATS_GRID - 1));
    for(unsigned x = 0; x < WGC_HDR_STATS_GRID; ++x)
    {
      const UINT sx = WGC_HDR_STATS_GRID == 1 ? 0 :
        (UINT)(((uint64_t)x * (srcDesc.Width - 1)) /
          (WGC_HDR_STATS_GRID - 1));
      const D3D11_BOX box =
      {
        .left   = sx,
        .top    = sy,
        .front  = 0,
        .right  = sx + 1,
        .bottom = sy + 1,
        .back   = 1
      };
      ID3D11DeviceContext4_CopySubresourceRegion1(context,
        (ID3D11Resource *)stats->hdrStatsTexture,
        0, x, y, 0, (ID3D11Resource *)src, 0, &box, 0);
    }
  }

  D3D11_MAPPED_SUBRESOURCE mapped;
  HRESULT hr = ID3D11DeviceContext4_Map(context,
    (ID3D11Resource *)stats->hdrStatsTexture, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("WGC HDR stats staging map failed", hr);
    return;
  }

  LONG64 lumMilli[WGC_HDR_STATS_GRID * WGC_HDR_STATS_GRID];
  float minLum = FLT_MAX;
  float maxLum = 0.0f;
  double sumLum = 0.0;
  unsigned count = 0;

  for(unsigned y = 0; y < WGC_HDR_STATS_GRID; ++y)
  {
    const uint8_t * row = (const uint8_t *)mapped.pData +
      (size_t)y * mapped.RowPitch;
    for(unsigned x = 0; x < WGC_HDR_STATS_GRID; ++x)
    {
      const uint16_t * px = (const uint16_t *)row + x * 4;
      const float r = max(0.0f, wgcUtil_halfToFloat(px[0]));
      const float g = max(0.0f, wgcUtil_halfToFloat(px[1]));
      const float b = max(0.0f, wgcUtil_halfToFloat(px[2]));
      const float lum = r * 0.2126f + g * 0.7152f + b * 0.0722f;
      minLum = min(minLum, lum);
      maxLum = max(maxLum, lum);
      sumLum += lum;
      lumMilli[count++] = (LONG64)(lum * 1000.0f + 0.5f);
    }
  }

  ID3D11DeviceContext4_Unmap(context,
    (ID3D11Resource *)stats->hdrStatsTexture, 0);

  const LONG64 p95 = wgcStats_percentileLong64(lumMilli, (LONG)count, 95);
  const LONG64 p99 = wgcStats_percentileLong64(lumMilli, (LONG)count, 99);
  const double avgLum = count ? sumLum / count : 0.0;
  DEBUG_INFO("WGC HDR RGBA16F stats scRGB-luma min/avg/p95/p99/max: "
    "%.3f/%.3f/%.3f/%.3f/%.3f samples:%u grid:%ux%u",
    (double)minLum, avgLum, (double)p95 / 1000.0,
    (double)p99 / 1000.0, (double)maxLum, count,
    WGC_HDR_STATS_GRID, WGC_HDR_STATS_GRID);
}

bool wgcStats_shouldDumpP010(WGCStats * stats)
{
  if (!stats->enabled)
    return false;

  const uint64_t now = microtime();
  if (now - stats->p010DumpLastLog < 1000 * 1000)
    return false;

  stats->p010DumpLastLog = now;
  return true;
}

void wgcStats_dumpP010(ID3D11Device5 * device, ID3D11DeviceContext4 * context,
  ID3D11Texture2D * texture, unsigned width, unsigned height,
  const char * what)
{
  D3D11_TEXTURE2D_DESC desc;
  ID3D11Texture2D_GetDesc(texture, &desc);
  desc.BindFlags      = 0;
  desc.MiscFlags      = 0;
  desc.Usage          = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  ID3D11Texture2D * staging = NULL;
  HRESULT hr = ID3D11Device5_CreateTexture2D(device, &desc, NULL, &staging);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Create WGC P010 dump staging failed", hr);
    return;
  }

  ID3D11DeviceContext4_CopyResource(context,
    (ID3D11Resource *)staging, (ID3D11Resource *)texture);
  D3D11_MAPPED_SUBRESOURCE mapped;
  hr = ID3D11DeviceContext4_Map(context,
    (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (SUCCEEDED(hr))
  {
    uint16_t minY = UINT16_MAX;
    uint16_t maxY = 0;
    uint64_t sumY = 0;
    unsigned nonzeroY = 0;
    unsigned samplesY = 0;
    for(unsigned y = 0; y < height; y += max(1u, height / 36))
    {
      for(unsigned x = 0; x < width; x += max(1u, width / 64))
      {
        const uint8_t * row = (const uint8_t *)mapped.pData +
          (size_t)y * mapped.RowPitch;
        const uint16_t * px = (const uint16_t *)(row +
          (size_t)(x / 4) * 8);
        const uint16_t value = px[x % 4];
        minY = min(minY, value);
        maxY = max(maxY, value);
        sumY += value;
        nonzeroY += value != 0;
        ++samplesY;
      }
    }
    DEBUG_INFO("WGC P010 %s samples rowPitch:%u size:%ux%u "
      "scan:min/avg/max:%u/%" PRIu64 "/%u nonzero:%u/%u",
      what, mapped.RowPitch, width, height,
      minY == UINT16_MAX ? 0 : minY,
      samplesY ? sumY / samplesY : 0,
      maxY, nonzeroY, samplesY);
    ID3D11DeviceContext4_Unmap(context, (ID3D11Resource *)staging, 0);
  }
  else
    DEBUG_WINERROR("Map WGC P010 dump staging failed", hr);

  ID3D11Texture2D_Release(staging);
}

void wgcStats_report(WGCStats * stats, const WGCStatsReport * ext)
{
  const LONG64 sysRelGapCount =
    InterlockedExchange64(&stats->systemRelativeGapCount, 0);
  const LONG64 sysRelGapTotal =
    InterlockedExchange64(&stats->systemRelativeGapTotalUs, 0);
  LONG64 profileCount[WGC_PROFILE_COUNT];
  LONG64 profileTotal[WGC_PROFILE_COUNT];
  LONG64 profileMax  [WGC_PROFILE_COUNT];
  LONG64 profileP50  [WGC_PROFILE_COUNT];
  LONG64 profileP95  [WGC_PROFILE_COUNT];
  LONG64 profileP99  [WGC_PROFILE_COUNT];
  LONG64 sampleScratch[WGC_STATS_SAMPLE_MAX];
  LONG histOverflow = 0;
  for(unsigned i = 0; i < WGC_PROFILE_COUNT; ++i)
  {
    profileCount[i] = InterlockedExchange64(&stats->profileCount  [i], 0);
    profileTotal[i] = InterlockedExchange64(&stats->profileTotalUs[i], 0);
    profileMax  [i] = InterlockedExchange64(&stats->profileMaxUs  [i], 0);
    const LONG n = wgcStats_copyAndResetSamples(&stats->profileSampleCount[i],
      stats->profileSamples[i], sampleScratch);
    profileP50[i] = wgcStats_percentileLong64(sampleScratch, n, 50);
    profileP95[i] = wgcStats_percentileLong64(sampleScratch, n, 95);
    profileP99[i] = wgcStats_percentileLong64(sampleScratch, n, 99);
    histOverflow |= InterlockedExchange(&stats->profileSampleOverflow[i], 0);
  }
#define WGC_PROFILE_AVG(stage) \
  (long long)(profileCount[(stage)] ? \
    profileTotal[(stage)] / profileCount[(stage)] : 0)
  const LONG64 d3d12CopySubmitCount =
    InterlockedExchange64(&stats->d3d12CopySubmitCount, 0);
  const LONG64 d3d12CopySubmitTotal =
    InterlockedExchange64(&stats->d3d12CopySubmitTotalUs, 0);
  const LONG64 d3d12CopySubmitMax =
    InterlockedExchange64(&stats->d3d12CopySubmitMaxUs, 0);
  LONG submitSampleCount = wgcStats_copyAndResetSamples(
    &stats->d3d12CopySubmitSampleCount,
    stats->d3d12CopySubmitSamples, sampleScratch);
  const LONG64 d3d12CopySubmitP50 =
    wgcStats_percentileLong64(sampleScratch, submitSampleCount, 50);
  const LONG64 d3d12CopySubmitP95 =
    wgcStats_percentileLong64(sampleScratch, submitSampleCount, 95);
  const LONG64 d3d12CopySubmitP99 =
    wgcStats_percentileLong64(sampleScratch, submitSampleCount, 99);
  histOverflow |=
    InterlockedExchange(&stats->d3d12CopySubmitSampleOverflow, 0);

  const LONG64 d3d12FenceWaitCount =
    InterlockedExchange64(&stats->d3d12FenceWaitCount, 0);
  const LONG64 d3d12FenceWaitTotal =
    InterlockedExchange64(&stats->d3d12FenceWaitTotalUs, 0);
  const LONG64 d3d12FenceWaitMax =
    InterlockedExchange64(&stats->d3d12FenceWaitMaxUs, 0);
  LONG fenceSampleCount = wgcStats_copyAndResetSamples(
    &stats->d3d12FenceWaitSampleCount,
    stats->d3d12FenceWaitSamples, sampleScratch);
  const LONG64 d3d12FenceWaitP50 =
    wgcStats_percentileLong64(sampleScratch, fenceSampleCount, 50);
  const LONG64 d3d12FenceWaitP95 =
    wgcStats_percentileLong64(sampleScratch, fenceSampleCount, 95);
  const LONG64 d3d12FenceWaitP99 =
    wgcStats_percentileLong64(sampleScratch, fenceSampleCount, 99);
  histOverflow |=
    InterlockedExchange(&stats->d3d12FenceWaitSampleOverflow, 0);

  DEBUG_INFO(
    "WGC debug stats ready-pre:%ld ready-post:%ld timeouts:%ld bursts:%ld max-batch:%ld cb-gap-avg-us:%lld cb-gap-max-us:%lld cb-gap-count:%lld sysrel-gap-avg-us:%lld sysrel-gap-max-us:%lld sysrel-gap-count:%lld gap-full:%ld slot-busy:%ld events:%ld pulled:%ld consumed:%ld copy-accum-full:%lld copy-accum-dirty:%lld copy-accum-kpix:%lld copy-publish-full:%lld copy-publish-dirty:%lld copy-publish-kpix:%lld prof-acquire-avg/max:%lld/%lld prof-ensure-avg/max:%lld/%lld prof-damage-avg/max:%lld/%lld prof-accum-copy-avg/max:%lld/%lld prof-pointer-avg/max:%lld/%lld prof-publish-copy-avg/max:%lld/%lld prof-publish-copy-p50/p95/p99:%lld/%lld/%lld prof-flush-avg/max:%lld/%lld d3d12-copy-submit-avg/max:%lld/%lld d3d12-copy-submit-p50/p95/p99:%lld/%lld/%lld d3d12-fence-wait-avg/max:%lld/%lld d3d12-fence-wait-p50/p95/p99:%lld/%lld/%lld hist-overflow:%ld",
    ext->readyPre,
    ext->readyPost,
    ext->timeouts,
    ext->bursts,
    ext->maxBatch,
    (long long)(ext->cbGapCount ? ext->cbGapTotalUs / ext->cbGapCount : 0),
    (long long)ext->cbGapMaxUs,
    (long long)ext->cbGapCount,
    (long long)(sysRelGapCount ? sysRelGapTotal / sysRelGapCount : 0),
    (long long)InterlockedExchange64(&stats->systemRelativeGapMaxUs, 0),
    (long long)sysRelGapCount,
    ext->gapFull,
    ext->slotBusy,
    ext->events,
    ext->pulled,
    ext->consumed,
    (long long)InterlockedExchange64(&stats->copyAccumFull, 0),
    (long long)InterlockedExchange64(&stats->copyAccumDirty, 0),
    (long long)(InterlockedExchange64(&stats->copyAccumPixels, 0) / 1000),
    (long long)InterlockedExchange64(&stats->copyPublishFull, 0),
    (long long)InterlockedExchange64(&stats->copyPublishDirty, 0),
    (long long)(InterlockedExchange64(&stats->copyPublishPixels, 0) / 1000),
    WGC_PROFILE_AVG(WGC_PROFILE_ACQUIRE),
    (long long)profileMax[WGC_PROFILE_ACQUIRE],
    WGC_PROFILE_AVG(WGC_PROFILE_ENSURE),
    (long long)profileMax[WGC_PROFILE_ENSURE],
    WGC_PROFILE_AVG(WGC_PROFILE_DAMAGE),
    (long long)profileMax[WGC_PROFILE_DAMAGE],
    WGC_PROFILE_AVG(WGC_PROFILE_ACCUM_COPY),
    (long long)profileMax[WGC_PROFILE_ACCUM_COPY],
    WGC_PROFILE_AVG(WGC_PROFILE_POINTER),
    (long long)profileMax[WGC_PROFILE_POINTER],
    WGC_PROFILE_AVG(WGC_PROFILE_PUBLISH_COPY),
    (long long)profileMax[WGC_PROFILE_PUBLISH_COPY],
    (long long)profileP50[WGC_PROFILE_PUBLISH_COPY],
    (long long)profileP95[WGC_PROFILE_PUBLISH_COPY],
    (long long)profileP99[WGC_PROFILE_PUBLISH_COPY],
    WGC_PROFILE_AVG(WGC_PROFILE_FLUSH),
    (long long)profileMax[WGC_PROFILE_FLUSH],
    (long long)(d3d12CopySubmitCount ?
      d3d12CopySubmitTotal / d3d12CopySubmitCount : 0),
    (long long)d3d12CopySubmitMax,
    (long long)d3d12CopySubmitP50,
    (long long)d3d12CopySubmitP95,
    (long long)d3d12CopySubmitP99,
    (long long)(d3d12FenceWaitCount ?
      d3d12FenceWaitTotal / d3d12FenceWaitCount : 0),
    (long long)d3d12FenceWaitMax,
    (long long)d3d12FenceWaitP50,
    (long long)d3d12FenceWaitP95,
    (long long)d3d12FenceWaitP99,
    histOverflow);
#undef WGC_PROFILE_AVG
}
