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

#ifndef _H_WGC_STATS_
#define _H_WGC_STATS_

#include <stdbool.h>
#include <stdint.h>

#include <d3d11_4.h>

// Diagnostics engine for the wgc:debugStats option. All recording functions
// are no-ops unless wgcStats_init was called with enabled = true.

#define WGC_STATS_SAMPLE_MAX 512

typedef enum WGCProfileStage
{
  WGC_PROFILE_ACQUIRE,
  WGC_PROFILE_ENSURE,
  WGC_PROFILE_DAMAGE,
  WGC_PROFILE_ACCUM_COPY,
  WGC_PROFILE_POINTER,
  WGC_PROFILE_PUBLISH_COPY,
  WGC_PROFILE_FLUSH,
  WGC_PROFILE_COUNT
}
WGCProfileStage;

typedef struct WGCStats
{
  bool enabled;

  volatile LONG64 copyAccumFull;
  volatile LONG64 copyAccumDirty;
  volatile LONG64 copyAccumPixels;
  volatile LONG64 copyPublishFull;
  volatile LONG64 copyPublishDirty;
  volatile LONG64 copyPublishPixels;
  volatile LONG64 profileTotalUs[WGC_PROFILE_COUNT];
  volatile LONG64 profileMaxUs[WGC_PROFILE_COUNT];
  volatile LONG64 profileCount[WGC_PROFILE_COUNT];
  volatile LONG   profileSampleCount[WGC_PROFILE_COUNT];
  volatile LONG   profileSampleOverflow[WGC_PROFILE_COUNT];
  LONG64          profileSamples[WGC_PROFILE_COUNT][WGC_STATS_SAMPLE_MAX];
  volatile LONG64 d3d12CopySubmitTotalUs;
  volatile LONG64 d3d12CopySubmitMaxUs;
  volatile LONG64 d3d12CopySubmitCount;
  volatile LONG   d3d12CopySubmitSampleCount;
  volatile LONG   d3d12CopySubmitSampleOverflow;
  LONG64          d3d12CopySubmitSamples[WGC_STATS_SAMPLE_MAX];
  volatile LONG64 d3d12FenceWaitTotalUs;
  volatile LONG64 d3d12FenceWaitMaxUs;
  volatile LONG64 d3d12FenceWaitCount;
  volatile LONG   d3d12FenceWaitSampleCount;
  volatile LONG   d3d12FenceWaitSampleOverflow;
  LONG64          d3d12FenceWaitSamples[WGC_STATS_SAMPLE_MAX];
  volatile LONG64 systemRelativeGapTotalUs;
  volatile LONG64 systemRelativeGapMaxUs;
  volatile LONG64 systemRelativeGapCount;

  uint64_t hdrStatsLastLog;
  uint64_t p010DumpLastLog;
  ID3D11Texture2D * hdrStatsTexture;
}
WGCStats;

// Counters that live with the caller (event handler/capture loop), sampled
// and reset by the caller when it asks for a report.
typedef struct WGCStatsReport
{
  LONG   readyPre;
  LONG   readyPost;
  LONG   timeouts;
  LONG   bursts;
  LONG   maxBatch;
  LONG64 cbGapTotalUs;
  LONG64 cbGapMaxUs;
  LONG64 cbGapCount;
  LONG   gapFull;
  LONG   slotBusy;
  LONG   events;
  LONG   pulled;
  LONG   consumed;
}
WGCStatsReport;

void wgcStats_init(WGCStats * stats, bool enabled);
void wgcStats_free(WGCStats * stats);

// Atomically raises *target to value if it is larger (generic stats helper,
// also used by callers for their own gauge counters).
void wgcStats_interlockedMax64(volatile LONG64 * target, LONG64 value);

void wgcStats_recordProfileStage(WGCStats * stats, WGCProfileStage stage,
  uint64_t elapsedUs);
void wgcStats_recordD3D12CopySubmit(WGCStats * stats, uint64_t elapsedUs);
void wgcStats_recordD3D12FenceWait(WGCStats * stats, uint64_t elapsedUs);
void wgcStats_recordSystemRelativeGap(WGCStats * stats, LONG64 gapUs);
void wgcStats_recordCopy(WGCStats * stats, bool publishCopy, bool fullCopy,
  uint64_t pixels, uint64_t framePixels);

// Samples a 16x16 grid of an RGBA16F source and logs scRGB luminance stats,
// at most once a second.
void wgcStats_maybeLogHDR(WGCStats * stats, ID3D11Device5 * device,
  ID3D11DeviceContext4 * context, ID3D11Texture2D * src);

// Rate limiter (1s) + readback dump of a packed P010 texture; `what` names
// the texture in the log output.
bool wgcStats_shouldDumpP010(WGCStats * stats);
void wgcStats_dumpP010(ID3D11Device5 * device, ID3D11DeviceContext4 * context,
  ID3D11Texture2D * texture, unsigned width, unsigned height,
  const char * what);

// Aggregates and logs the one second debug stats report, resetting the
// counters.
void wgcStats_report(WGCStats * stats, const WGCStatsReport * ext);

#endif
