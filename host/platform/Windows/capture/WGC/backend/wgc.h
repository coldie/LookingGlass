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
  // Publish slot is a USAGE_STAGING + CPU_ACCESS_READ D3D11 texture mapped on
  // the consumer thread. Used by the top-level Capture_WGC interface to
  // memcpy directly into IVSHMEM.
  WGC_PUBLISH_CPU_STAGING,

  // Publish slot is a ROW_MAJOR TEXTURE2D placed in the IVSHMEM heap.
  // WGC first copies into a shared D3D11 bridge texture; that same bridge is
  // opened as an ID3D12Resource and copied into IVSHMEM by a native D3D12
  // copy queue.
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
// of creating its own. Required for WGC_PUBLISH_IVSHMEM_D3D12_COPY (the
// D3D12 device must match the one that opened the IVSHMEM heap).
//
// Caller retains ownership and must keep the devices alive for the WGC
// instance's lifetime. Must be called BEFORE wgc_initInstance.
//
// Pointers passed as IUnknown* to keep wgc.h free of full D3D headers.
void wgc_setLoanedDevices(WGCInstance * this,
  IUnknown * d11Device,        // ID3D11Device*
  IUnknown * d11Context,       // ID3D11DeviceContext*
  IUnknown * d12Device);       // ID3D12Device3*

// Request a source capture format independent of the publish target. This is
// used by wrapper-level encoders that consume WGC RGBA16F frames and repack
// them before publishing.
void wgc_setCaptureFormatHint(WGCInstance * this, unsigned format);

// Configure the IVSHMEM publish environment. Must be called BEFORE
// wgc_initInstance when publishMode is WGC_PUBLISH_IVSHMEM_D3D12_COPY.
//
// Per-slot IVSHMEM offsets are registered lazily via wgc_setIvshmemSlot —
// the caller typically learns them on first iface->capture() after app.c
// has allocated its FrameBuffer regions. wgc_ensureFrame fails for any
// publish slot whose offset has not been registered yet.
//
// Caller retains ownership of `ivshmemHeap`; it must outlive the
// WGCInstance.
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

// IVSHMEM GPU-publish consumer side. WGC has written the frame bytes
// directly into IVSHMEM at *ivshmemOffset; no map needed. Issues a Flush to make
// sure the GPU writes have committed before the caller signals the
// consumer. desc->backendToken must be passed back to wgc_releaseIvshmemDirect.
bool wgc_fetchIvshmemDirect(WGCInstance * this, unsigned frameBufferIndex,
  WGCFrameDesc * desc, uint64_t * ivshmemOffset,
  unsigned * pitch, unsigned * width, unsigned * height);

void wgc_releaseIvshmemDirect(WGCInstance * this, void * token);

#endif
