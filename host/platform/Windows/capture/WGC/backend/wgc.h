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

#ifndef _H_D12_BACKEND_WGC_
#define _H_D12_BACKEND_WGC_

#include "d12.h"

#include <stdbool.h>
#include <d3d11.h>
#include <dxgi.h>
#include "interface/capture.h"

typedef enum WGCPublishMode
{
  // Publish slot is a SHARED + SHARED_NTHANDLE D3D11 texture opened as a
  // D3D12 resource. Used by the D12 frontend to chain effects and write to
  // IVSHMEM via the copy queue.
  WGC_PUBLISH_D12_SHARE,

  // Publish slot is a USAGE_STAGING + CPU_ACCESS_READ D3D11 texture mapped on
  // the consumer thread. Used by the top-level Capture_WGC interface to
  // memcpy directly into IVSHMEM.
  WGC_PUBLISH_CPU_STAGING,

  // Publish slot is a ROW_MAJOR TEXTURE2D placed in the IVSHMEM heap,
  // wrapped as an ID3D11Texture2D via D3D11On12. WGC's D3D11 CopyResource
  // writes directly into IVSHMEM. The consumer reads from the same memory
  // (no map, no second copy). Requires the host to have set up the IVSHMEM
  // heap and a D3D11On12 device via wgc_setIvshmemTarget before
  // wgc_initInstance.
  WGC_PUBLISH_IVSHMEM_DIRECT,

  // Publish slot is a ROW_MAJOR TEXTURE2D placed in the IVSHMEM heap.
  // WGC first copies into a shared D3D11 bridge texture; that same bridge is
  // opened as an ID3D12Resource and copied into IVSHMEM by a native D3D12
  // copy queue. This avoids the D3D11On12 bridge->IVSHMEM CopyResource path.
  WGC_PUBLISH_IVSHMEM_D3D12_COPY
}
WGCPublishMode;

typedef struct WGCInstance WGCInstance;

typedef struct WGCFrameDesc
{
  CaptureRotation       rotation;
  RECT                * dirtyRects;
  unsigned              nbDirtyRects;
  DXGI_COLOR_SPACE_TYPE colorSpace;
  bool                  hasSystemRelativeTime;
  int64_t               systemRelativeTime;
  bool                  hasBackendFrameTime;
  uint64_t              backendFrameTimeUs;
  bool                  hasWgcStats;
  long                  wgcEventCallbacks;
  long                  wgcFramesPulled;
  long                  wgcFramesConsumed;
  bool                  fullCopy;
  void                * backendToken;
}
WGCFrameDesc;

bool wgc_createInstance(WGCInstance ** out, unsigned frameBuffers,
  WGCPublishMode mode);

bool wgc_initInstance(WGCInstance * this, bool debug,
  IDXGIAdapter1 * adapter, IDXGIOutput * output, bool trackDamage);

// Install the cursor publish callbacks. Must be called before init if cursor
// capture is desired.
void wgc_setPointerCallbacks(WGCInstance * this,
  CaptureGetPointerBuffer  getFn,
  CapturePostPointerBuffer postFn);

// Provide loaned D3D11 / D3D12 devices for the WGC backend to use instead
// of creating its own. Required for WGC_PUBLISH_IVSHMEM_DIRECT (the
// D3D11On12 device is the only D3D11 device whose context can write to
// wrapped resources).
//
// Caller retains ownership and must keep the devices alive for the WGC
// instance's lifetime. Must be called BEFORE wgc_initInstance.
//
// Pointers passed as IUnknown* to keep wgc.h free of full D3D headers.
void wgc_setLoanedDevices(WGCInstance * this,
  IUnknown * d11Device,        // ID3D11Device*
  IUnknown * d11Context,       // ID3D11DeviceContext*
  IUnknown * d12Device);       // ID3D12Device3*

// Configure the IVSHMEM-direct publish environment. Must be called BEFORE
// wgc_initInstance when publishMode is WGC_PUBLISH_IVSHMEM_DIRECT.
//
// Per-slot IVSHMEM offsets are registered lazily via wgc_setIvshmemSlot —
// the caller typically learns them on first iface->capture() after app.c
// has allocated its FrameBuffer regions. wgc_ensureFrame fails for any
// publish slot whose offset has not been registered yet.
//
// Caller retains ownership of `ivshmemHeap` and `d11on12Device`; they must
// outlive the WGCInstance.
bool wgc_setIvshmemEnv(WGCInstance * this,
  IUnknown * ivshmemHeap,    // ID3D12Heap*
  IUnknown * d11on12Device,  // ID3D11On12Device*
  unsigned   width,
  unsigned   height,
  unsigned   format);        // DXGI_FORMAT value

bool wgc_setIvshmemD3D12CopyEnv(WGCInstance * this,
  IUnknown * ivshmemHeap,    // ID3D12Heap*
  IUnknown * d3d12Queue,     // ID3D12CommandQueue*
  unsigned   width,
  unsigned   height,
  unsigned   format);        // DXGI_FORMAT value

// Register the IVSHMEM offset for a single publish slot. `slotIndex` is
// 0-based into the publish-slot ring (excludes the accumulator), so the
// valid range is [0, slotCount-1]. Safe to call multiple times for the
// same slot — the latest call wins. Returns false if slotIndex is
// out of range.
bool wgc_setIvshmemSlot(WGCInstance * this,
  unsigned slotIndex,
  uint64_t offset,
  uint64_t size);

bool wgc_deinitInstance(WGCInstance * this);

void wgc_freeInstance(WGCInstance ** this);

CaptureResult wgc_pollFrame(WGCInstance * this, unsigned frameBufferIndex);

// CPU-staging consumer side. Returns false if no frame is currently ready.
// On success, *map points into the mapped staging texture (valid until
// wgc_releaseCpu is called) and *pitch / *width / *height describe it.
// desc->backendToken must be passed back to wgc_releaseCpu.
bool wgc_fetchCpu(WGCInstance * this, unsigned frameBufferIndex,
  WGCFrameDesc * desc, void ** map, unsigned * pitch,
  unsigned * width, unsigned * height);

void wgc_releaseCpu(WGCInstance * this, void * token);

// IVSHMEM-direct consumer side. WGC has written the frame bytes directly
// into IVSHMEM at *ivshmemOffset; no map needed. Issues a Flush to make
// sure the GPU writes have committed before the caller signals the
// consumer. desc->backendToken must be passed back to wgc_releaseIvshmemDirect.
bool wgc_fetchIvshmemDirect(WGCInstance * this, unsigned frameBufferIndex,
  WGCFrameDesc * desc, uint64_t * ivshmemOffset,
  unsigned * pitch, unsigned * width, unsigned * height);

void wgc_releaseIvshmemDirect(WGCInstance * this, void * token);

#endif
