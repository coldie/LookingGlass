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

#include "backend.h"
#include "command_group.h"
#include "d12.h"
#include "wgc.h"

#include "com_ref.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/array.h"
#include "common/display.h"
#include "common/option.h"
#include "windows/mousehook.h"
#include "common/time.h"
#include "common/profile.h"
#include "wgc_profile_d3d11.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <d3dcompiler.h>

#define WGC_D3D12_COPY_QUEUE_MAX 8
#define WGC_D3D12_TILE_SPAN_MAX 1024
#define WGC_STATS_SAMPLE_MAX 512

typedef enum WGCTiledCopyMode
{
  WGC_TILED_COPY_NONE,
  WGC_TILED_COPY_DIRTY
}
WGCTiledCopyMode;

typedef enum WGCTimingStage
{
  D12_TIMING_WGC_CONTENT_SIZE,
  D12_TIMING_WGC_SURFACE,
  D12_TIMING_WGC_ACCESS,
  D12_TIMING_WGC_TEXTURE,
  D12_TIMING_WGC_ENSURE,
  D12_TIMING_WGC_METADATA,
  D12_TIMING_WGC_DAMAGE,
  D12_TIMING_WGC_POINTER,
  D12_TIMING_WGC_COPY,
  D12_TIMING_WGC_SIGNAL,
  D12_TIMING_WGC_WAIT,
  D12_TIMING_WGC_TAKE_FRAME,
  D12_TIMING_D12_SYNC
}
WGCTimingStage;

static inline bool d12_timingEnabled(void)
{
  return false;
}

static inline void d12_timingRecord(WGCTimingStage stage, uint64_t value)
{
  (void)stage;
  (void)value;
}

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Graphics
#define WIDL_using_Windows_Graphics_Capture
#define WIDL_using_Windows_Graphics_DirectX
#define WIDL_using_Windows_Graphics_DirectX_Direct3D11

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_6.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.h>
#include <windows.graphics.directx.direct3d11.h>
#if __has_include(<windows.graphics.directx.direct3d11.interop.h>)
#include <windows.graphics.directx.direct3d11.interop.h>
#else
HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(
  IDXGIDevice * dxgiDevice,
  IInspectable ** graphicsDevice);

DEFINE_GUID(IID_IDirect3DDxgiInterfaceAccess,
  0xa9b3d012, 0x3df2, 0x4ee3, 0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1);

typedef interface IDirect3DDxgiInterfaceAccess IDirect3DDxgiInterfaceAccess;
typedef struct IDirect3DDxgiInterfaceAccessVtbl
{
  BEGIN_INTERFACE

  HRESULT (STDMETHODCALLTYPE *QueryInterface)(
    IDirect3DDxgiInterfaceAccess * This,
    REFIID riid,
    void ** ppvObject);

  ULONG (STDMETHODCALLTYPE *AddRef)(IDirect3DDxgiInterfaceAccess * This);
  ULONG (STDMETHODCALLTYPE *Release)(IDirect3DDxgiInterfaceAccess * This);

  HRESULT (STDMETHODCALLTYPE *GetInterface)(
    IDirect3DDxgiInterfaceAccess * This,
    REFIID iid,
    void ** object);

  END_INTERFACE
}
IDirect3DDxgiInterfaceAccessVtbl;

interface IDirect3DDxgiInterfaceAccess
{
  CONST_VTBL IDirect3DDxgiInterfaceAccessVtbl * lpVtbl;
};

#define IDirect3DDxgiInterfaceAccess_QueryInterface(This,riid,ppvObject) \
  (This)->lpVtbl->QueryInterface(This,riid,ppvObject)
#define IDirect3DDxgiInterfaceAccess_AddRef(This) \
  (This)->lpVtbl->AddRef(This)
#define IDirect3DDxgiInterfaceAccess_Release(This) \
  (This)->lpVtbl->Release(This)
#define IDirect3DDxgiInterfaceAccess_GetInterface(This,iid,object) \
  (This)->lpVtbl->GetInterface(This,iid,object)
#endif

static void wgc_setD3D12ObjectName(ID3D12Object * object, const char * name)
{
  (void)object;
  (void)name;
  return;

#if 0
  if (!object || !name || !*name)
    return;

  wchar_t wideName[128];
  const int written = MultiByteToWideChar(CP_UTF8, 0, name, -1,
    wideName, ARRAYSIZE(wideName));
  if (written <= 0)
    return;

  ID3D12Object_SetName(object, wideName);
#endif
}

static void wgc_setD3D12ObjectNameI(ID3D12Object * object,
  const char * prefix, unsigned index)
{
  char name[96];
  snprintf(name, sizeof(name), "%s%u", prefix, index);
  wgc_setD3D12ObjectName(object, name);
}

#ifndef IID_IDirect3D11CaptureFrame2
DEFINE_GUID(IID_IDirect3D11CaptureFrame2,
  0x37869cfa, 0x2b48, 0x5ebf, 0x9a, 0xfb, 0xdf, 0xfd, 0x80, 0x5d, 0xef, 0xdb);

typedef interface IDirect3D11CaptureFrame2 IDirect3D11CaptureFrame2;
typedef struct IDirect3D11CaptureFrame2Vtbl
{
  BEGIN_INTERFACE

  HRESULT (STDMETHODCALLTYPE *QueryInterface)(
    IDirect3D11CaptureFrame2 * This,
    REFIID riid,
    void ** ppvObject);

  ULONG (STDMETHODCALLTYPE *AddRef)(IDirect3D11CaptureFrame2 * This);
  ULONG (STDMETHODCALLTYPE *Release)(IDirect3D11CaptureFrame2 * This);

  HRESULT (STDMETHODCALLTYPE *GetIids)(
    IDirect3D11CaptureFrame2 * This,
    ULONG * iidCount,
    IID ** iids);

  HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(
    IDirect3D11CaptureFrame2 * This,
    HSTRING * className);

  HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(
    IDirect3D11CaptureFrame2 * This,
    TrustLevel * trustLevel);

  HRESULT (STDMETHODCALLTYPE *get_DirtyRegions)(
    IDirect3D11CaptureFrame2 * This,
    IVectorView_RectInt32 ** value);

  HRESULT (STDMETHODCALLTYPE *get_DirtyRegionMode)(
    IDirect3D11CaptureFrame2 * This,
    int * value);

  END_INTERFACE
}
IDirect3D11CaptureFrame2Vtbl;

interface IDirect3D11CaptureFrame2
{
  CONST_VTBL IDirect3D11CaptureFrame2Vtbl * lpVtbl;
};

#define IDirect3D11CaptureFrame2_QueryInterface(This,riid,ppvObject) \
  (This)->lpVtbl->QueryInterface(This,riid,ppvObject)
#define IDirect3D11CaptureFrame2_get_DirtyRegions(This,value) \
  (This)->lpVtbl->get_DirtyRegions(This,value)
#endif

#ifndef IID_IGraphicsCaptureSession5
DEFINE_GUID(IID_IGraphicsCaptureSession5,
  0x67c0ea62, 0x1f85, 0x5061, 0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb);

typedef interface IGraphicsCaptureSession5 IGraphicsCaptureSession5;
typedef struct IGraphicsCaptureSession5Vtbl
{
  BEGIN_INTERFACE

  HRESULT (STDMETHODCALLTYPE *QueryInterface)(
    IGraphicsCaptureSession5 * This,
    REFIID riid,
    void ** ppvObject);

  ULONG (STDMETHODCALLTYPE *AddRef)(IGraphicsCaptureSession5 * This);
  ULONG (STDMETHODCALLTYPE *Release)(IGraphicsCaptureSession5 * This);

  HRESULT (STDMETHODCALLTYPE *GetIids)(
    IGraphicsCaptureSession5 * This,
    ULONG * iidCount,
    IID ** iids);

  HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(
    IGraphicsCaptureSession5 * This,
    HSTRING * className);

  HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(
    IGraphicsCaptureSession5 * This,
    TrustLevel * trustLevel);

  HRESULT (STDMETHODCALLTYPE *get_MinUpdateInterval)(
    IGraphicsCaptureSession5 * This,
    TimeSpan * value);

  HRESULT (STDMETHODCALLTYPE *put_MinUpdateInterval)(
    IGraphicsCaptureSession5 * This,
    TimeSpan value);

  END_INTERFACE
}
IGraphicsCaptureSession5Vtbl;

interface IGraphicsCaptureSession5
{
  CONST_VTBL IGraphicsCaptureSession5Vtbl * lpVtbl;
};

#define IGraphicsCaptureSession5_QueryInterface(This,riid,ppvObject) \
  (This)->lpVtbl->QueryInterface(This,riid,ppvObject)
#define IGraphicsCaptureSession5_put_MinUpdateInterval(This,value) \
  (This)->lpVtbl->put_MinUpdateInterval(This,value)
#endif

#ifndef IID_IGraphicsCaptureSession6
DEFINE_GUID(IID_IGraphicsCaptureSession6,
  0xd7419236, 0xbe20, 0x5e9f, 0xbc, 0xd6, 0xc4, 0xe9, 0x8f, 0xd6, 0xaf, 0xdc);

typedef interface IGraphicsCaptureSession6 IGraphicsCaptureSession6;
typedef struct IGraphicsCaptureSession6Vtbl
{
  BEGIN_INTERFACE

  HRESULT (STDMETHODCALLTYPE *QueryInterface)(
    IGraphicsCaptureSession6 * This,
    REFIID riid,
    void ** ppvObject);

  ULONG (STDMETHODCALLTYPE *AddRef)(IGraphicsCaptureSession6 * This);
  ULONG (STDMETHODCALLTYPE *Release)(IGraphicsCaptureSession6 * This);

  HRESULT (STDMETHODCALLTYPE *GetIids)(
    IGraphicsCaptureSession6 * This,
    ULONG * iidCount,
    IID ** iids);

  HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(
    IGraphicsCaptureSession6 * This,
    HSTRING * className);

  HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(
    IGraphicsCaptureSession6 * This,
    TrustLevel * trustLevel);

  HRESULT (STDMETHODCALLTYPE *get_IncludeSecondaryWindows)(
    IGraphicsCaptureSession6 * This,
    boolean * value);

  HRESULT (STDMETHODCALLTYPE *put_IncludeSecondaryWindows)(
    IGraphicsCaptureSession6 * This,
    boolean value);

  END_INTERFACE
}
IGraphicsCaptureSession6Vtbl;

interface IGraphicsCaptureSession6
{
  CONST_VTBL IGraphicsCaptureSession6Vtbl * lpVtbl;
};

#define IGraphicsCaptureSession6_QueryInterface(This,riid,ppvObject) \
  (This)->lpVtbl->QueryInterface(This,riid,ppvObject)
#define IGraphicsCaptureSession6_put_IncludeSecondaryWindows(This,value) \
  (This)->lpVtbl->put_IncludeSecondaryWindows(This,value)
#endif

#define FRAME_POOL_BUFFERS 2
#define WGC_CURSOR_MAX_SIZE (512 * 512 * 4)

#define WGC_FRAME_FREE      0
#define WGC_FRAME_WRITING   1
#define WGC_FRAME_READY     2
#define WGC_FRAME_CONSUMING 3

#define WGC_FULL_COPY_GAP_100NS (50LL * 10000LL)

typedef enum WGCCursorMode
{
  WGC_CURSOR_MODE_SEPARATE,
  WGC_CURSOR_MODE_EMBEDDED,
  WGC_CURSOR_MODE_NONE
}
WGCCursorMode;

typedef struct WGCInstance WGCInstance;

typedef struct WGCFrameEventHandler
{
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable base;
  LONG   refs;
  LONG   events;
  LONG   framesPulled;
  LONG   framesConsumed;
  LONG   callbackBursts;
  LONG   maxCallbackBatch;
  volatile LONG64 lastCallbackUs;
  volatile LONG64 callbackGapTotalUs;
  volatile LONG64 callbackGapMaxUs;
  volatile LONG64 callbackGapCount;
  volatile LONG activeCallbacks;
  volatile LONG stopping;
  HANDLE event;
  WGCInstance * owner;
}
WGCFrameEventHandler;

typedef struct WGCFrameInfo
{
  D3D11_TEXTURE2D_DESC format;

  ID3D11Texture2D  ** texture;
  ID3D12Resource   ** d12Res;
  ID3D11Fence      ** fence;
  ID3D12Fence      ** d12Fence;
  UINT64              fenceValue;
  bool                ready;
  bool                copiedOnce;
  volatile LONG       state;

  RECT                dirtyRects[D12_MAX_DIRTY_RECTS];
  unsigned            nbDirtyRects;
  RECT                pendingDirtyRects[D12_MAX_DIRTY_RECTS];
  unsigned            nbPendingDirtyRects;
  bool                hasSystemRelativeTime;
  int64_t             systemRelativeTime;
  bool                fullCopy;
  bool                pendingFullCopy;
  bool                copyFailed;
  uint64_t            callbackTimeUs;

  // IVSHMEM-direct only: the underlying placed D3D12 resource that backs the
  // D3D11On12-wrapped `texture` above. Stored so we can release it. The
  // wrapped `texture` is created via D3D11On12 ::CreateWrappedResource and
  // released through the normal D3D11 path.
  ID3D12Resource   ** ivshmemD12Res;
  // Offset within the IVSHMEM heap (bytes) where this slot lives
  uint64_t            ivshmemOffset;
  // True once wgc_setIvshmemSlot has registered an offset for this slot.
  // wgc_ensureFrameIvshmemDirect refuses to create textures for unregistered
  // slots — the caller must register them (typically on first iface->capture
  // for that frameBufferIndex) before they can be used.
  bool                ivshmemSlotReady;
  // Bridge textures for the two-device IVSHMEM_DIRECT path:
  //   bridgeA — D3D11 texture on the WGC frame pool's device (SHARED +
  //             SHARED_NTHANDLE). WGC source → bridgeA on WGC context.
  //   bridgeB — same memory, opened via shared NT handle on the
  //             D3D11On12-side D3D11 device. bridgeB → wrapped texture
  //             (IVSHMEM-resident) on D3D11On12-side context.
  ID3D11Texture2D ** bridgeA;
  ID3D11Texture2D ** bridgeB;
  ID3D12Resource  ** bridge12;
  ID3D11UnorderedAccessView ** encodeUav;
  UINT64             d3d12CopyFenceValue[WGC_D3D12_COPY_QUEUE_MAX];
  unsigned           d3d12CopyQueueCount;
}
WGCFrameInfo;

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

struct WGCInstance
{
  D12Backend base;

  WGCPublishMode publishMode;
  CaptureGetPointerBuffer  getPointerBufferFn;
  CapturePostPointerBuffer postPointerBufferFn;

  // Loaned devices — set via wgc_setLoanedDevices before wgc_initInstance.
  // For IVSHMEM_DIRECT mode the loaned D3D11 device is the D3D11On12 side
  // (used ONLY for wrapping IVSHMEM-resident D3D12 placed resources). WGC's
  // own frame pool gets a separately-created vanilla D3D11 device on the
  // same adapter — using a D3D11On12-backed device for the frame pool
  // causes WGC's compositor capture pipeline to stop after ~1–2 frames.
  //
  // For other modes the loaned device (if any) is used directly.
  ID3D11Device        * loanedD11Device;
  ID3D11DeviceContext * loanedD11Context;
  ID3D12Device3       * loanedD12Device;

  // D3D11On12-side D3D11 device & context. In IVSHMEM_DIRECT mode this is
  // a separate device from `device` (the WGC frame pool's vanilla D3D11).
  // Resolved at end of wgc_init: alias of `device`/`context` for non-IVSHMEM
  // modes (where they coincide); a distinct device pair for IVSHMEM_DIRECT.
  ID3D11Device5         ** on12Device;
  ID3D11DeviceContext4  ** on12Context;

  // Cross-device sync fence for IVSHMEM_DIRECT (NULL otherwise).
  //   wgcFence:     created on `device` (WGC frame pool's D3D11) with
  //                 D3D11_FENCE_FLAG_SHARED. Signaled on WGC context after
  //                 each bridge copy.
  //   wgcFenceOn12: same fence, opened on `on12Device` via a shared NT
  //                 handle. The D3D11On12-side context waits on this fence
  //                 before reading the bridge.
  ID3D11Fence       ** wgcFence;
  ID3D11Fence       ** wgcFenceOn12;
  ID3D12Fence       ** wgcD3D12Fence;
  UINT64               wgcFenceValue;
  ID3D11ComputeShader ** nv12Shader;

  // True iff the two-device bridge path is active (separate WGC + D3D11On12
  // devices, with the cross-device fence and bridge textures). When false,
  // `on12Device`/`on12Context` are NULL and call sites fall back to
  // `device`/`context`.
  bool                 twoDeviceBridge;

  WGCTracyD3D11Context * tracyWgcCtx;
  WGCTracyD3D11Context * tracyOn12Ctx;
  WGCTracyD3D12Context * tracyD3D12CopyCtx[WGC_D3D12_COPY_QUEUE_MAX];

  // IVSHMEM-direct environment — set via wgc_setIvshmemEnv before
  // wgc_initInstance. Only used when publishMode == WGC_PUBLISH_IVSHMEM_DIRECT.
  // The heap and D3D11On12 device are owned by the caller; we hold raw
  // pointers (no AddRef in this file — caller keeps them alive). Per-slot
  // offsets live on each WGCFrameInfo (set via wgc_setIvshmemSlot).
  ID3D12Heap        * ivshmemHeap;
  ID3D11On12Device  * d11on12Device;
  ID3D12CommandQueue * d3d12CopyQueues[WGC_D3D12_COPY_QUEUE_MAX];
  D12CommandGroup     d3d12CopyCommands[WGC_D3D12_COPY_QUEUE_MAX];
  bool                d3d12CopyCommandReady[WGC_D3D12_COPY_QUEUE_MAX];
  unsigned            d3d12CopyQueueCount;
  unsigned            ivshmemWidth;
  unsigned            ivshmemHeight;
  DXGI_FORMAT         ivshmemFormat;
  uint64_t            ivshmemSlotSize;
  bool                ivshmemEnvReady;

  ID3D12Device3          ** d12device;
  ID3D11Device5          ** device;
  ID3D11DeviceContext4   ** context;
  IDirect3DDevice        ** graphicsDevice;
  IGraphicsCaptureItem   ** item;
  IDirect3D11CaptureFramePool ** framePool;
  IGraphicsCaptureSession     ** session;

  WGCFrameEventHandler * handler;
  EventRegistrationToken frameArrivedToken;
  HANDLE frameEvent;
  IDirect3D11CaptureFrame * pendingFrame;

  WGCFrameInfo * frames;
  unsigned frameCount;
  WGCFrameInfo * current;
  WGCFrameInfo * consumerFrame;
  volatile LONG asyncNextSlot;
  bool asyncCapture;
  bool debugStats;
  LONG asyncTimeouts;
  LONG asyncReadyBeforeWait;
  LONG asyncReadyAfterWait;
  LONG fullCopyAfterGap;
  LONG asyncSlotBusy;
  LONG forceNextFullCopy;
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
  uint64_t debugStatsLastLog;

  SizeInt32 size;
  RECT outputRect;
  DXGI_COLOR_SPACE_TYPE colorSpace;
  bool roInitialized;
  unsigned emptyPolls;
  bool loggedFirstFrame;
  WGCCursorMode cursorMode;
  int maxFPS;
  bool includeSecondaryWindows;
  bool dwmFlushOnGap;
  int dwmFlushGapMs;
  uint64_t lastDwmFlushUs;
  bool hasLastSystemRelativeTime;
  int64_t lastSystemRelativeTime;

  bool mouseHookCreated;
  CRITICAL_SECTION cursorLock;
  bool cursorLockCreated;
  int cursorMaxHz;
  int dirtyFullCopyPercent;
  bool d3d12FullCopyAlways;
  WGCTiledCopyMode tiledCopyMode;
  unsigned tileWidth;
  unsigned tileHeight;
  unsigned dirtyMaxTiles;
  uint64_t cursorLastPostUs;
  volatile LONG64 cursorPendingPos;
  HCURSOR lastCursor;
  bool lastCursorValid;
  bool lastCursorVisible;
  int lastCursorX, lastCursorY;
  void * cursorShape;
  size_t cursorShapeSize;
  size_t cursorShapeDataSize;
  unsigned cursorHotX, cursorHotY;
  uint64_t cursorShapeHash;
  bool cursorShapeValid;
};

static WGCInstance * wgcCursorInstance;

static HRESULT STDMETHODCALLTYPE wgc_eventQueryInterface(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface,
  REFIID riid,
  void ** obj);
static ULONG STDMETHODCALLTYPE wgc_eventAddRef(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface);
static ULONG STDMETHODCALLTYPE wgc_eventRelease(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface);
static HRESULT STDMETHODCALLTYPE wgc_eventInvoke(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface,
  IDirect3D11CaptureFramePool * sender,
  IInspectable * args);
static void wgc_boostCallbackThreadPriority(void);

static bool wgc_create(D12Backend ** instance, unsigned frameBuffers);
static bool wgc_init(D12Backend * instance, bool debug, ID3D12Device3 * device,
  IDXGIAdapter1 * adapter, IDXGIOutput * output);
static bool wgc_deinit(D12Backend * instance);
static void wgc_free(D12Backend ** instance);
static CaptureResult wgc_capture(D12Backend * instance,
  unsigned frameBufferIndex);
static CaptureResult wgc_sync(D12Backend * instance,
  ID3D12CommandQueue * commandQueue);
static ID3D12Resource * wgc_fetch(D12Backend * instance,
  unsigned frameBufferIndex, D12FrameDesc * desc);
static void wgc_releaseSlot(D12Backend * instance, void * token);
static CaptureResult wgc_processFrame(WGCInstance * this,
  IDirect3D11CaptureFrame * frame, unsigned frameBufferIndex,
  uint64_t callbackTimeUs);
static bool wgc_selectAsyncSlot(WGCInstance * this, unsigned * frameBufferIndex);

static bool wgc_createCaptureItem(WGCInstance * this, HMONITOR monitor);
static bool wgc_createFramePool(WGCInstance * this);
static bool wgc_asyncFrameReady(WGCInstance * this);
static void wgc_accumulateDamage(WGCInstance * this, const WGCFrameInfo * src);
static void wgc_clearAccumulatedDamage(WGCFrameInfo * frame);
static void wgc_copyFrameTexture(WGCInstance * this, WGCFrameInfo * dst,
  ID3D11Texture2D * src, bool publishCopy);
static bool wgc_copyFrameTextureD3D12(WGCInstance * this, WGCFrameInfo * dst);
static void wgc_waitFrameD3D12Copy(WGCInstance * this, WGCFrameInfo * frame);
static void wgc_copyFrameTextureRectCtx(ID3D11DeviceContext4 * ctx,
  ID3D11Resource * dst, ID3D11Resource * src, const RECT * rect);
static bool wgc_ensureAllFrames(WGCInstance * this, ID3D11Texture2D * src);
static uint64_t wgc_copyFramePixels(const WGCFrameInfo * frame);
static void wgc_recordCopyStats(WGCInstance * this, const WGCFrameInfo * frame,
  bool publishCopy, uint64_t pixels);
static bool wgc_ensureFrame(WGCInstance * this, WGCFrameInfo * frame,
  ID3D11Texture2D * src);
static bool wgc_shareFrame(WGCInstance * this, WGCFrameInfo * frame);
static void wgc_releaseFrameInfo(WGCFrameInfo * frame);
static void wgc_updateDamage(WGCInstance * this, WGCFrameInfo * info,
  IDirect3D11CaptureFrame * frame);
static bool wgc_shouldForceFullCopyAfterGap(WGCInstance * this,
  WGCFrameInfo * frame);
static void wgc_maybeLogDebugStats(WGCInstance * this);
static void wgc_maybeDwmFlushOnGap(WGCInstance * this);
static void wgc_recordProfileStage(WGCInstance * this, WGCProfileStage stage,
  uint64_t elapsedUs);
static void wgc_interlockedMax64(volatile LONG64 * target, LONG64 value);
static void wgc_updatePointer(WGCInstance * this, int x, int y);
static void wgc_onMouseMove(int x, int y);
static LONG64 wgc_packCursorPos(int x, int y);
static void wgc_unpackCursorPos(LONG64 packed, int * x, int * y);
static bool wgc_updatePointerShape(WGCInstance * this,
  CapturePointer * pointer, HCURSOR cursor);
static uint64_t wgc_hashCursorShape(const void * data, size_t size,
  unsigned width, unsigned height, unsigned pitch, unsigned hotX,
  unsigned hotY);
static void wgc_setMinUpdateInterval(IGraphicsCaptureSession * session,
  int maxFPS);
static WGCCursorMode wgc_parseCursorMode(void);
static void wgc_closeInspectable(IInspectable * obj);
static void wgc_releaseFrame(IDirect3D11CaptureFrame ** frame);
static bool wgc_createHString(const WCHAR * str, HSTRING * result);
static void wgc_waitCallbacks(WGCFrameEventHandler * handler);
static bool wgc_isIvshmemPublishMode(WGCPublishMode mode);
static bool wgc_isNV12PackedIvshmem(const WGCInstance * this);
static bool wgc_colorSpaceIsHDR(DXGI_COLOR_SPACE_TYPE colorSpace);

static const ITypedEventHandler_Direct3D11CaptureFramePool_IInspectableVtbl
  wgc_eventVtbl =
{
  .QueryInterface = wgc_eventQueryInterface,
  .AddRef         = wgc_eventAddRef,
  .Release        = wgc_eventRelease,
  .Invoke         = wgc_eventInvoke
};

static bool wgc_isIvshmemPublishMode(WGCPublishMode mode)
{
  return mode == WGC_PUBLISH_IVSHMEM_DIRECT ||
         mode == WGC_PUBLISH_IVSHMEM_D3D12_COPY;
}

static HRESULT STDMETHODCALLTYPE wgc_eventQueryInterface(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface,
  REFIID riid,
  void ** obj)
{
  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IAgileObject) ||
      IsEqualGUID(riid,
        &IID_ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable))
  {
    *obj = iface;
    wgc_eventAddRef(iface);
    return S_OK;
  }

  *obj = NULL;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE wgc_eventAddRef(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface)
{
  WGCFrameEventHandler * this = UPCAST(WGCFrameEventHandler, iface);
  return InterlockedIncrement(&this->refs);
}

static ULONG STDMETHODCALLTYPE wgc_eventRelease(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface)
{
  WGCFrameEventHandler * this = UPCAST(WGCFrameEventHandler, iface);
  const ULONG refs = InterlockedDecrement(&this->refs);
  if (refs == 0)
    free(this);

  return refs;
}

static HRESULT STDMETHODCALLTYPE wgc_eventInvoke(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface,
  IDirect3D11CaptureFramePool * sender,
  IInspectable * args)
{
  wgc_boostCallbackThreadPriority();
  LG_PROFILE_THREAD("WGC FrameArrived");
  LG_PROFILE_ZONE_BEGIN(zoneCallback, "wgc FrameArrived callback");

  WGCFrameEventHandler * this = UPCAST(WGCFrameEventHandler, iface);
  InterlockedIncrement(&this->activeCallbacks);
  WGCInstance * owner = this->owner;
  const uint64_t callbackTimeUs = microtime();

  if (owner)
  {
    const LONG64 last = InterlockedExchange64(&this->lastCallbackUs,
      (LONG64)callbackTimeUs);
    if (owner->debugStats && last > 0 && callbackTimeUs > (uint64_t)last)
    {
      const LONG64 gapUs = (LONG64)(callbackTimeUs - (uint64_t)last);
      InterlockedExchangeAdd64(&this->callbackGapTotalUs, gapUs);
      InterlockedIncrement64(&this->callbackGapCount);
      wgc_interlockedMax64(&this->callbackGapMaxUs, gapUs);
    }
  }

  if (owner && !InterlockedCompareExchange(&this->stopping, 0, 0))
  {
    LONG callbackBatch = 0;
    for(;;)
    {
      IDirect3D11CaptureFrame * next = NULL;
      HRESULT hr = IDirect3D11CaptureFramePool_TryGetNextFrame(sender, &next);
      if (FAILED(hr))
        break;

      if (!next)
        break;
      ++callbackBatch;
      LG_PROFILE_FRAME("wgc frame");

      const bool callbackProcessed = owner->asyncCapture &&
        !wgc_isIvshmemPublishMode(owner->publishMode);
      if (callbackProcessed)
      {
        if (wgc_processFrame(owner, next, 0, callbackTimeUs) ==
            CAPTURE_RESULT_OK)
        {
          InterlockedIncrement(&this->framesPulled);
          SetEvent(this->event);
        }
        wgc_releaseFrame(&next);
      }
      else
      {
        IDirect3D11CaptureFrame * old = InterlockedExchangePointer(
          (PVOID volatile *)&owner->pendingFrame, next);
        wgc_releaseFrame(&old);
        InterlockedIncrement(&this->framesPulled);
      }
    }
    if (owner->debugStats && callbackBatch > 1)
    {
      InterlockedIncrement(&this->callbackBursts);
      LONG maxBatch = InterlockedCompareExchange(&this->maxCallbackBatch, 0, 0);
      while(callbackBatch > maxBatch &&
          InterlockedCompareExchange(&this->maxCallbackBatch,
            callbackBatch, maxBatch) != maxBatch)
        maxBatch = InterlockedCompareExchange(&this->maxCallbackBatch, 0, 0);
    }
  }

  if (!InterlockedCompareExchange(&this->stopping, 0, 0))
  {
    InterlockedIncrement(&this->events);
    SetEvent(this->event);
  }
  InterlockedDecrement(&this->activeCallbacks);
  LG_PROFILE_ZONE_END(zoneCallback);
  return S_OK;
}

static void wgc_boostCallbackThreadPriority(void)
{
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

static void wgc_parseTileSize(const char * value,
  unsigned * width, unsigned * height)
{
  unsigned w = 0;
  unsigned h = 0;
  if (value)
    sscanf(value, "%ux%u", &w, &h);

  if (w == 0 || h == 0)
  {
    w = 256;
    h = 64;
  }

  *width  = max(16, min(w, 1024));
  *height = max(16, min(h, 1024));
}

static void wgc_waitCallbacks(WGCFrameEventHandler * handler)
{
  for(unsigned i = 0; i < 5000 &&
      InterlockedCompareExchange(&handler->activeCallbacks, 0, 0) > 0;
      ++i)
    Sleep(1);

  if (InterlockedCompareExchange(&handler->activeCallbacks, 0, 0) > 0)
    DEBUG_WARN("Timed out waiting for WGC frame callbacks to finish");
}

static bool wgc_create(D12Backend ** instance, unsigned frameBuffers)
{
  WGCInstance * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("out of memory");
    return false;
  }

  this->frameCount = max(2, frameBuffers + 1);
  this->frames = calloc(this->frameCount, sizeof(*this->frames));
  if (!this->frames)
  {
    DEBUG_ERROR("out of memory");
    free(this);
    return false;
  }

  this->publishMode = WGC_PUBLISH_D12_SHARE;
  this->cursorPendingPos = wgc_packCursorPos(INT_MIN, INT_MIN);
  InitializeCriticalSection(&this->cursorLock);
  this->cursorLockCreated = true;
  *instance = &this->base;
  return true;
}

static bool wgc_init(D12Backend * instance, bool debug, ID3D12Device3 * device,
  IDXGIAdapter1 * adapter, IDXGIOutput * output)
{
  WGCInstance * this = UPCAST(WGCInstance, instance);

  bool result = false;
  HRESULT hr;
  comRef_scopePush(20);

  this->cursorMode = wgc_parseCursorMode();
  this->maxFPS     = option_get_int("wgc", "maxFPS");
  this->cursorMaxHz = option_get_int("wgc", "cursorMaxHz");
  this->dirtyFullCopyPercent = option_get_int("wgc", "dirtyFullCopyPercent");
  this->d3d12FullCopyAlways =
    option_get_bool("wgc", "d3d12FullCopyAlways");
  const char * tiled = option_get_string("wgc", "tiled");
  if (tiled && strcmp(tiled, "dirty") == 0)
    this->tiledCopyMode = WGC_TILED_COPY_DIRTY;
  else
  {
    if (tiled && strcmp(tiled, "none") != 0)
      DEBUG_WARN("Unknown wgc:tiled \"%s\", defaulting to none", tiled);
    this->tiledCopyMode = WGC_TILED_COPY_NONE;
  }
  wgc_parseTileSize(option_get_string("wgc", "tileSize"),
    &this->tileWidth, &this->tileHeight);
  this->dirtyMaxTiles = max(1, option_get_int("wgc", "dirtyMaxTiles"));
  this->asyncCapture = option_get_bool("wgc", "asyncCapture");
  this->debugStats = option_get_bool("wgc", "debugStats");
  this->includeSecondaryWindows =
    option_get_bool("wgc", "includeSecondaryWindows");
  this->dwmFlushOnGap = option_get_bool("wgc", "dwmFlushOnGap");
  this->dwmFlushGapMs = max(1, option_get_int("wgc", "dwmFlushGapMs"));
  this->lastDwmFlushUs = 0;
  DEBUG_INFO("WGC cursor:%s cursorMaxHz:%d maxFPS:%d asyncCapture:%d includeSecondaryWindows:%d debugStats:%d dwmFlushOnGap:%d/%dms tiled:%s tileSize:%ux%u dirtyMaxTiles:%u",
    this->cursorMode == WGC_CURSOR_MODE_SEPARATE ? "separate" :
    this->cursorMode == WGC_CURSOR_MODE_EMBEDDED ? "embedded" : "none",
    this->cursorMaxHz, this->maxFPS, this->asyncCapture,
    this->includeSecondaryWindows, this->debugStats, this->dwmFlushOnGap,
    this->dwmFlushGapMs,
    this->tiledCopyMode == WGC_TILED_COPY_DIRTY ? "dirty" : "none",
    this->tileWidth, this->tileHeight, this->dirtyMaxTiles);

  hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
  {
    DEBUG_WINERROR("RoInitialize failed", hr);
    goto exit;
  }
  this->roInitialized = hr != RPC_E_CHANGED_MODE;

  comRef_defineLocal(IGraphicsCaptureSessionStatics, sessionStatics);
  HSTRING className = NULL;
  if (!wgc_createHString(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
      &className))
    goto exit;

  hr = RoGetActivationFactory(className, &IID_IGraphicsCaptureSessionStatics,
    (void **)sessionStatics);
  WindowsDeleteString(className);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get GraphicsCaptureSession statics", hr);
    goto exit;
  }

  boolean supported = false;
  hr = IGraphicsCaptureSessionStatics_IsSupported(*sessionStatics, &supported);
  if (FAILED(hr) || !supported)
  {
    DEBUG_WINERROR("Windows Graphics Capture is not supported", hr);
    goto exit;
  }

  comRef_defineLocal(IDXGIAdapter, _adapter);
  hr = IDXGIAdapter1_QueryInterface(adapter, &IID_IDXGIAdapter,
    (void **)_adapter);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get the IDXGIAdapter interface", hr);
    goto exit;
  }

  static const D3D_FEATURE_LEVEL featureLevels[] =
  {
    D3D_FEATURE_LEVEL_11_1
  };
  D3D_FEATURE_LEVEL featureLevel;

  comRef_defineLocal(ID3D11Device       , d11device);
  comRef_defineLocal(ID3D11DeviceContext, d11context);

  // IVSHMEM modes MUST NOT drive the WGC frame pool from the loaned
  // D3D11On12-backed device — empirically that causes the compositor capture
  // pipeline to stop producing frames after 1–2 callbacks (the pool itself
  // goes empty, not just FrameArrived). Create a fresh vanilla D3D11 device
  // on the same adapter for the frame pool, and use the loaned device only
  // for the IVSHMEM-resident wrapped textures via the bridge texture path.
  const bool useLoanedForWgc = this->loanedD11Device &&
    !wgc_isIvshmemPublishMode(this->publishMode);

  if (useLoanedForWgc)
  {
    // Caller supplied a D3D11 device (typically from D3D11On12CreateDevice).
    // Borrow it — caller retains ownership. We still go through the COM
    // ref scope so subsequent QueryInterface chains work uniformly.
    ID3D11Device_AddRef(this->loanedD11Device);
    *d11device = this->loanedD11Device;
    ID3D11DeviceContext_AddRef(this->loanedD11Context);
    *d11context = this->loanedD11Context;
    featureLevel = ID3D11Device_GetFeatureLevel(this->loanedD11Device);
    DEBUG_INFO("WGC: using loaned D3D11 device (FL 0x%x)", (unsigned)featureLevel);
  }
  else
  {
    hr = D3D11CreateDevice(
      *_adapter,
      D3D_DRIVER_TYPE_UNKNOWN,
      NULL,
      D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
        (debug ? D3D11_CREATE_DEVICE_DEBUG : 0),
      featureLevels,
      ARRAY_LENGTH(featureLevels),
      D3D11_SDK_VERSION,
      d11device,
      &featureLevel,
      d11context);

    if (FAILED(hr))
    {
      DEBUG_WINERROR("Failed to create the D3D11Device", hr);
      goto exit;
    }

    DEBUG_INFO("Feature Level     : 0x%x", featureLevel);
  }

  comRef_defineLocal(ID3D11DeviceContext4, d11context4);
  hr = ID3D11DeviceContext_QueryInterface(
    *d11context, &IID_ID3D11DeviceContext4, (void **)d11context4);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get the ID3D11Context4 interface", hr);
    goto exit;
  }

  comRef_defineLocal(ID3D11Device5, d11device5);
  hr = ID3D11Device_QueryInterface(
    *d11device, &IID_ID3D11Device5, (void **)d11device5);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get the ID3D11Device5 interface", hr);
    goto exit;
  }

  comRef_defineLocal(IDXGIDevice, dxgiDevice);
  hr = ID3D11Device_QueryInterface(
    *d11device, &IID_IDXGIDevice, (void **)dxgiDevice);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to query the DXGI interface from the device", hr);
    goto exit;
  }

  IInspectable * inspectableDevice = NULL;
  hr = CreateDirect3D11DeviceFromDXGIDevice(*dxgiDevice, &inspectableDevice);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the WinRT Direct3D device", hr);
    goto exit;
  }

  comRef_defineLocal(IDirect3DDevice, graphicsDevice);
  hr = IInspectable_QueryInterface(inspectableDevice, &IID_IDirect3DDevice,
    (void **)graphicsDevice);
  IInspectable_Release(inspectableDevice);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to query IDirect3DDevice", hr);
    goto exit;
  }

  DXGI_OUTPUT_DESC outputDesc;
  hr = IDXGIOutput_GetDesc(output, &outputDesc);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("WGC output description query failed", hr);
    goto exit;
  }
  this->outputRect = outputDesc.DesktopCoordinates;

  DISPLAYCONFIG_PATH_INFO pathInfo;
  if (display_getPathInfo(outputDesc.Monitor, &pathInfo))
  {
    const DISPLAYCONFIG_RATIONAL refresh =
      pathInfo.targetInfo.refreshRate;
    if (refresh.Denominator)
      DEBUG_INFO("WGC source refresh: %.3f Hz (%u/%u)",
        (double)refresh.Numerator / refresh.Denominator,
        refresh.Numerator, refresh.Denominator);
    else
      DEBUG_WARN("WGC source refresh denominator is zero");
  }
  else
    DEBUG_WARN("WGC source refresh query failed for %ls",
      outputDesc.DeviceName);

  if (!wgc_createCaptureItem(this, outputDesc.Monitor))
    goto exit;

  hr = IGraphicsCaptureItem_get_Size(*this->item, &this->size);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get WGC item size", hr);
    goto exit;
  }

  comRef_defineLocal(IDXGIOutput6, output6);
  hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6, (void **)output6);
  if (FAILED(hr))
    this->colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  else
  {
    DXGI_OUTPUT_DESC1 desc1;
    IDXGIOutput6_GetDesc1(*output6, &desc1);
    this->colorSpace = desc1.ColorSpace;
  }

  // Prefer the explicitly passed D12 device; else use the loaned one
  // (typically the one bound to the D3D11On12 wrapper).
  ID3D12Device3 * effectiveD12 = device ? device : this->loanedD12Device;
  if (effectiveD12)
  {
    ID3D12Device3_AddRef(effectiveD12);
    comRef_toGlobal(this->d12device  , &effectiveD12 );
  }
  comRef_toGlobal(this->device       , d11device5    );
  comRef_toGlobal(this->context      , d11context4   );
  comRef_toGlobal(this->graphicsDevice, graphicsDevice);
  this->tracyWgcCtx = WGC_TRACY_D3D11_CREATE(*this->device, *this->context,
    "WGC D3D11");

  // Resolve on12Device / on12Context.
  //
  // For non-IVSHMEM_DIRECT modes (or when no loaned device was provided),
  // they alias the WGC frame pool's device — the same context handles
  // wgc-publishes and any wraps.
  //
  // For IVSHMEM_DIRECT with a loaned D3D11On12 device, queryInterface to
  // the ID3D11Device5 / ID3D11DeviceContext4 forms so the existing call
  // sites work uniformly.
  if (this->publishMode == WGC_PUBLISH_IVSHMEM_DIRECT &&
      this->loanedD11Device && !useLoanedForWgc)
  {
    comRef_defineLocal(ID3D11Device5       , on12dev5);
    comRef_defineLocal(ID3D11DeviceContext4, on12ctx4);
    hr = ID3D11Device_QueryInterface(this->loanedD11Device,
      &IID_ID3D11Device5, (void **)on12dev5);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-direct: QI ID3D11Device5 on loaned device failed",
        hr);
      goto exit;
    }

    hr = ID3D11DeviceContext_QueryInterface(this->loanedD11Context,
      &IID_ID3D11DeviceContext4, (void **)on12ctx4);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-direct: QI ID3D11DeviceContext4 on loaned ctx failed",
        hr);
      goto exit;
    }

    // Cross-device sync fence: signal on WGC's context, wait on
    // D3D11On12-side context. Create shared on WGC's device, open on the
    // D3D11On12-side device via shared NT handle. Promote locals to global
    // AFTER OpenSharedFence — comRef_toGlobal NULLs the source pointer.
    comRef_defineLocal(ID3D11Fence, fence);
    hr = ID3D11Device5_CreateFence(*this->device, 0,
      D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, (void **)fence);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-direct: CreateFence on WGC device failed", hr);
      goto exit;
    }

    HANDLE fenceHandle = NULL;
    hr = ID3D11Fence_CreateSharedHandle(*fence, NULL, GENERIC_ALL, NULL,
      &fenceHandle);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-direct: ID3D11Fence_CreateSharedHandle failed", hr);
      goto exit;
    }

    comRef_defineLocal(ID3D11Fence, fenceOn12);
    hr = ID3D11Device5_OpenSharedFence(*on12dev5, fenceHandle,
      &IID_ID3D11Fence, (void **)fenceOn12);
    CloseHandle(fenceHandle);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-direct: OpenSharedFence on D3D11On12 device failed",
        hr);
      goto exit;
    }

    comRef_toGlobal(this->on12Device  , on12dev5 );
    comRef_toGlobal(this->on12Context , on12ctx4 );
    comRef_toGlobal(this->wgcFence    , fence    );
    comRef_toGlobal(this->wgcFenceOn12, fenceOn12);
    this->wgcFenceValue    = 0;
    this->twoDeviceBridge  = true;
    this->tracyOn12Ctx = WGC_TRACY_D3D11_CREATE(*this->on12Device,
      *this->on12Context, "WGC D3D11On12");
    DEBUG_INFO("ivshmem-direct: two-device path active");
  }

  if (this->publishMode == WGC_PUBLISH_IVSHMEM_D3D12_COPY)
  {
    if (!this->ivshmemHeap || !this->d3d12CopyQueues[0] || !*this->d12device)
    {
      DEBUG_ERROR("ivshmem-d3d12-copy: missing D3D12 heap/device/queue");
      goto exit;
    }

    int requestedQueues = option_get_int("wgc", "d3d12CopyQueues");
    if (requestedQueues < 1)
      requestedQueues = 1;
    if (requestedQueues > WGC_D3D12_COPY_QUEUE_MAX)
      requestedQueues = WGC_D3D12_COPY_QUEUE_MAX;
    this->d3d12CopyQueueCount = (unsigned)requestedQueues;

    for(unsigned i = 1; i < this->d3d12CopyQueueCount; ++i)
    {
      D3D12_COMMAND_QUEUE_DESC qDesc =
      {
        .Type     = D3D12_COMMAND_LIST_TYPE_COPY,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE
      };
      hr = ID3D12Device3_CreateCommandQueue(*this->d12device, &qDesc,
        &IID_ID3D12CommandQueue, (void **)&this->d3d12CopyQueues[i]);
      if (FAILED(hr))
      {
        DEBUG_WINERROR("ivshmem-d3d12-copy: CreateCommandQueue failed", hr);
        goto exit;
      }
      wgc_setD3D12ObjectNameI((ID3D12Object *)this->d3d12CopyQueues[i],
        "WGC IVSHMEM D3D12 copy queue ", i);
    }

    for(unsigned i = 0; i < this->d3d12CopyQueueCount; ++i)
    {
      wgc_setD3D12ObjectNameI((ID3D12Object *)this->d3d12CopyQueues[i],
        "WGC IVSHMEM D3D12 copy queue ", i);
      if (!d12_commandGroupCreate(*this->d12device,
          D3D12_COMMAND_LIST_TYPE_COPY, &this->d3d12CopyCommands[i],
          L"WGC IVSHMEM D3D12 copy"))
      {
        DEBUG_ERROR("ivshmem-d3d12-copy: failed to create copy command group");
        goto exit;
      }
      hr = ID3D12GraphicsCommandList_Close(
        *this->d3d12CopyCommands[i].gfxList);
      if (FAILED(hr))
      {
        DEBUG_WINERROR("ivshmem-d3d12-copy: failed to close initial command list",
          hr);
        goto exit;
      }
      this->d3d12CopyCommandReady[i] = true;
      char name[32];
      snprintf(name, sizeof(name), "WGC D3D12 COPY q%u", i);
      this->tracyD3D12CopyCtx[i] = WGC_TRACY_D3D12_CREATE(
        *this->d12device, this->d3d12CopyQueues[i], name);
    }

    comRef_defineLocal(ID3D11Fence, fence);
    hr = ID3D11Device5_CreateFence(*this->device, 0,
      D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, (void **)fence);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-d3d12-copy: CreateFence on WGC device failed", hr);
      goto exit;
    }

    HANDLE fenceHandle = NULL;
    hr = ID3D11Fence_CreateSharedHandle(*fence, NULL, GENERIC_ALL, NULL,
      &fenceHandle);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-d3d12-copy: fence CreateSharedHandle failed", hr);
      goto exit;
    }

    comRef_defineLocal(ID3D12Fence, d12Fence);
    hr = ID3D12Device3_OpenSharedHandle(*this->d12device, fenceHandle,
      &IID_ID3D12Fence, (void **)d12Fence);
    CloseHandle(fenceHandle);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-d3d12-copy: OpenSharedHandle fence failed", hr);
      goto exit;
    }

    comRef_toGlobal(this->wgcFence, fence);
    comRef_toGlobal(this->wgcD3D12Fence, d12Fence);
    wgc_setD3D12ObjectName((ID3D12Object *)this->wgcD3D12Fence,
      "WGC source-to-D3D12 shared fence");
    this->wgcFenceValue = 0;
    DEBUG_INFO("ivshmem-d3d12-copy: native D3D12 copy path active (%u copy queues)",
      this->d3d12CopyQueueCount);
  }

  this->frameEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!this->frameEvent)
  {
    DEBUG_WINERROR("CreateEvent failed", GetLastError());
    goto exit;
  }

  this->handler = calloc(1, sizeof(*this->handler));
  if (!this->handler)
  {
    DEBUG_ERROR("out of memory");
    goto exit;
  }
  this->handler->base.lpVtbl  =
    (ITypedEventHandler_Direct3D11CaptureFramePool_IInspectableVtbl *)
      &wgc_eventVtbl;
  this->handler->refs         = 1;
  this->handler->event        = this->frameEvent;
  this->handler->owner        = this;

  if (!wgc_createFramePool(this))
    goto exit;

  hr = IGraphicsCaptureSession_StartCapture(*this->session);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to start WGC capture", hr);
    goto exit;
  }

  result = true;

exit:
  comRef_scopePop();

  if (!result)
    wgc_deinit(instance);

  return result;
}

static bool wgc_deinit(D12Backend * instance)
{
  WGCInstance * this = UPCAST(WGCInstance, instance);

  for(unsigned i = 0; i < WGC_D3D12_COPY_QUEUE_MAX; ++i)
  {
    WGC_TRACY_D3D12_DESTROY(this->tracyD3D12CopyCtx[i]);
    d12_commandGroupFree(&this->d3d12CopyCommands[i]);
    this->d3d12CopyCommandReady[i] = false;
    if (this->d3d12CopyQueues[i])
    {
      ID3D12CommandQueue_Release(this->d3d12CopyQueues[i]);
      this->d3d12CopyQueues[i] = NULL;
    }
  }
  this->d3d12CopyQueueCount = 0;

  WGC_TRACY_D3D11_DESTROY(this->tracyOn12Ctx);
  WGC_TRACY_D3D11_DESTROY(this->tracyWgcCtx);

  if (this->mouseHookCreated)
  {
    mouseHook_remove();
    this->mouseHookCreated = false;
    if (wgcCursorInstance == this)
      wgcCursorInstance = NULL;
  }
  if (this->cursorLockCreated)
  {
    DeleteCriticalSection(&this->cursorLock);
    this->cursorLockCreated = false;
  }

  if (this->handler)
  {
    InterlockedExchange(&this->handler->stopping, 1);
    this->handler->owner = NULL;
  }

  if (this->framePool && *this->framePool && this->frameArrivedToken.value)
  {
    IDirect3D11CaptureFramePool_remove_FrameArrived(
      *this->framePool, this->frameArrivedToken);
    this->frameArrivedToken.value = 0;
  }

  if (this->handler)
    wgc_waitCallbacks(this->handler);

  if (this->session && *this->session)
    wgc_closeInspectable((IInspectable *)*this->session);

  if (this->framePool && *this->framePool)
    wgc_closeInspectable((IInspectable *)*this->framePool);

  comRef_release(this->session);
  comRef_release(this->framePool);
  comRef_release(this->item);
  comRef_release(this->graphicsDevice);
  comRef_release(this->nv12Shader);

  if (this->frameEvent)
  {
    CloseHandle(this->frameEvent);
    this->frameEvent = NULL;
  }

  if (this->handler)
  {
    wgc_eventRelease(&this->handler->base);
    this->handler = NULL;
  }

  IDirect3D11CaptureFrame * pending = InterlockedExchangePointer(
    (PVOID volatile *)&this->pendingFrame, NULL);
  wgc_releaseFrame(&pending);

  for(unsigned i = 0; i < this->frameCount; ++i)
    wgc_releaseFrameInfo(&this->frames[i]);
  this->current = NULL;
  this->consumerFrame = NULL;
  this->cursorPendingPos = wgc_packCursorPos(INT_MIN, INT_MIN);

  free(this->cursorShape);
  this->cursorShape         = NULL;
  this->cursorShapeSize     = 0;
  this->cursorShapeDataSize = 0;
  this->cursorShapeHash     = 0;
  this->cursorShapeValid    = false;

  if (this->roInitialized)
  {
    RoUninitialize();
    this->roInitialized = false;
  }

  return true;
}

static void wgc_free(D12Backend ** instance)
{
  WGCInstance * this = UPCAST(WGCInstance, *instance);

  free(this->frames);
  free(this);
  *instance = NULL;
}

static bool wgc_selectAsyncSlot(WGCInstance * this, unsigned * frameBufferIndex)
{
  for(unsigned i = 1; i < this->frameCount; ++i)
  {
    const unsigned start = InterlockedIncrement(&this->asyncNextSlot);
    const unsigned index = 1 + ((start + i) % (this->frameCount - 1));
    WGCFrameInfo * frame = &this->frames[index];

    if (wgc_isIvshmemPublishMode(this->publishMode) &&
        !frame->ivshmemSlotReady)
      continue;

    if (InterlockedCompareExchange(&frame->state,
        WGC_FRAME_WRITING, WGC_FRAME_FREE) == WGC_FRAME_FREE ||
        InterlockedCompareExchange(&frame->state,
        WGC_FRAME_WRITING, WGC_FRAME_READY) == WGC_FRAME_READY)
    {
      *frameBufferIndex = index;
      return true;
    }
  }

  InterlockedIncrement(&this->asyncSlotBusy);
  return false;
}

static CaptureResult wgc_processFrame(WGCInstance * this,
  IDirect3D11CaptureFrame * frame, unsigned frameBufferIndex,
  uint64_t callbackTimeUs)
{
  CaptureResult result = CAPTURE_RESULT_ERROR;
  HRESULT hr;
  WGCFrameInfo * accum = NULL;
  WGCFrameInfo * dst = NULL;
  comRef_scopePush(11);

  const bool timings = d12_timingEnabled();
  const bool profile = this->debugStats;
  uint64_t timingStart = timings ? microtime() : 0;
  SizeInt32 size;
  hr = IDirect3D11CaptureFrame_get_ContentSize(frame, &size);
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_CONTENT_SIZE, microtime() - timingStart);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get WGC frame size", hr);
    goto exit;
  }

  if (size.Width != this->size.Width || size.Height != this->size.Height)
  {
    this->size = size;
    DEBUG_INFO("WGC frame size changed, reinitializing");
    result = CAPTURE_RESULT_REINIT;
    goto exit;
  }

  if (!this->loggedFirstFrame)
  {
    D3D11_TEXTURE2D_DESC firstDesc;
    comRef_defineLocal(IDirect3DSurface, firstSurface);
    hr = IDirect3D11CaptureFrame_get_Surface(frame, firstSurface);
    if (SUCCEEDED(hr))
    {
      comRef_defineLocal(IDirect3DDxgiInterfaceAccess, firstAccess);
      hr = IDirect3DSurface_QueryInterface(
        *firstSurface, &IID_IDirect3DDxgiInterfaceAccess, (void **)firstAccess);
      if (SUCCEEDED(hr))
      {
        comRef_defineLocal(ID3D11Texture2D, firstSrc);
        hr = IDirect3DDxgiInterfaceAccess_GetInterface(
          *firstAccess, &IID_ID3D11Texture2D, (void **)firstSrc);
        if (SUCCEEDED(hr))
        {
          ID3D11Texture2D_GetDesc(*firstSrc, &firstDesc);
          DEBUG_INFO("WGC first frame: %ux%u format:%u bind:0x%x misc:0x%x events:%ld",
            firstDesc.Width, firstDesc.Height, firstDesc.Format,
            firstDesc.BindFlags, firstDesc.MiscFlags,
            this->handler ? this->handler->events : 0);
        }
      }
    }
    this->loggedFirstFrame = true;
  }

  uint64_t profileStart = profile ? microtime() : 0;

  timingStart = timings ? microtime() : 0;
  comRef_defineLocal(IDirect3DSurface, surface);
  hr = IDirect3D11CaptureFrame_get_Surface(frame, surface);
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_SURFACE, microtime() - timingStart);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get WGC frame surface", hr);
    goto exit;
  }

  timingStart = timings ? microtime() : 0;
  comRef_defineLocal(IDirect3DDxgiInterfaceAccess, access);
  hr = IDirect3DSurface_QueryInterface(
    *surface, &IID_IDirect3DDxgiInterfaceAccess, (void **)access);
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_ACCESS, microtime() - timingStart);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to query IDirect3DDxgiInterfaceAccess", hr);
    goto exit;
  }

  timingStart = timings ? microtime() : 0;
  comRef_defineLocal(ID3D11Texture2D, src);
  hr = IDirect3DDxgiInterfaceAccess_GetInterface(
    *access, &IID_ID3D11Texture2D, (void **)src);
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_TEXTURE, microtime() - timingStart);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get WGC D3D11 texture", hr);
    goto exit;
  }
  if (profile)
    wgc_recordProfileStage(this, WGC_PROFILE_ACQUIRE,
      microtime() - profileStart);

  if (wgc_isIvshmemPublishMode(this->publishMode))
  {
    const unsigned publishIndex = frameBufferIndex + 1;
    if (frameBufferIndex >= this->frameCount - 1)
    {
      DEBUG_ERROR("WGC direct frame index %u is out of range",
        frameBufferIndex);
      goto exit;
    }

    dst = &this->frames[publishIndex];
    if (!dst->ivshmemSlotReady)
    {
      result = CAPTURE_RESULT_TIMEOUT;
      goto exit;
    }

    LONG state = InterlockedCompareExchange(&dst->state,
      WGC_FRAME_WRITING, WGC_FRAME_FREE);
    if (state != WGC_FRAME_FREE)
      state = InterlockedCompareExchange(&dst->state,
        WGC_FRAME_WRITING, WGC_FRAME_READY);

    if (state != WGC_FRAME_READY && state != WGC_FRAME_FREE)
    {
      InterlockedIncrement(&this->asyncSlotBusy);
      InterlockedExchange(&this->forceNextFullCopy, 1);
      result = CAPTURE_RESULT_TIMEOUT;
      goto exit;
    }
    if (state == WGC_FRAME_READY)
      wgc_waitFrameD3D12Copy(this, dst);

    profileStart = profile ? microtime() : 0;
    timingStart = timings ? microtime() : 0;
    if (!wgc_ensureFrame(this, dst, *src))
      goto exit;
    dst->callbackTimeUs = callbackTimeUs;
    if (timings)
      d12_timingRecord(D12_TIMING_WGC_ENSURE, microtime() - timingStart);
    if (profile)
      wgc_recordProfileStage(this, WGC_PROFILE_ENSURE,
        microtime() - profileStart);

    timingStart = timings ? microtime() : 0;
    dst->hasSystemRelativeTime = false;
    dst->systemRelativeTime    = 0;
    TimeSpan systemRelativeTime;
    hr = IDirect3D11CaptureFrame_get_SystemRelativeTime(
      frame, &systemRelativeTime);
    if (SUCCEEDED(hr))
    {
      dst->hasSystemRelativeTime = true;
      dst->systemRelativeTime    = systemRelativeTime.Duration;
    }
    if (timings)
      d12_timingRecord(D12_TIMING_WGC_METADATA, microtime() - timingStart);

    if (this->cursorMode == WGC_CURSOR_MODE_SEPARATE &&
        !this->mouseHookCreated)
    {
      wgcCursorInstance = this;
      mouseHook_install(wgc_onMouseMove);
      this->mouseHookCreated = true;
      wgc_updatePointer(this, INT_MIN, INT_MIN);
    }

    profileStart = profile ? microtime() : 0;
    timingStart = timings ? microtime() : 0;
    wgc_updateDamage(this, dst, frame);
    const bool forceFullCopyAfterGap =
      wgc_shouldForceFullCopyAfterGap(this, dst);
    const bool forceNextFullCopy =
      InterlockedExchange(&this->forceNextFullCopy, 0);
    const bool hasDirtyRects = dst->nbDirtyRects > 0;
    const bool keepGapDamage =
      forceFullCopyAfterGap && this->base.trackDamage && dst->copiedOnce &&
      hasDirtyRects && !forceNextFullCopy;
    if (forceFullCopyAfterGap)
    {
      if (this->debugStats)
        InterlockedIncrement(&this->fullCopyAfterGap);
    }
    if (forceNextFullCopy)
      dst->nbDirtyRects = 0;
    dst->fullCopy = !this->base.trackDamage || !dst->copiedOnce ||
      dst->nbDirtyRects == 0 || forceFullCopyAfterGap;
    if (keepGapDamage)
      dst->fullCopy = false;
    if (timings)
      d12_timingRecord(D12_TIMING_WGC_DAMAGE, microtime() - timingStart);
    if (profile)
      wgc_recordProfileStage(this, WGC_PROFILE_DAMAGE,
        microtime() - profileStart);

    wgc_accumulateDamage(this, dst);
    dst->fullCopy              = dst->pendingFullCopy || !dst->copiedOnce ||
                                 dst->nbPendingDirtyRects == 0;
    dst->nbDirtyRects          = dst->fullCopy ? 0 : dst->nbPendingDirtyRects;
    if (dst->nbDirtyRects > 0)
      memcpy(dst->dirtyRects, dst->pendingDirtyRects,
        dst->nbDirtyRects * sizeof(*dst->dirtyRects));

    profileStart = profile ? microtime() : 0;
    timingStart = timings ? microtime() : 0;
    if (this->cursorMode == WGC_CURSOR_MODE_SEPARATE)
    {
      const LONG64 pending = InterlockedExchange64(
        &this->cursorPendingPos, wgc_packCursorPos(INT_MIN, INT_MIN));
      int x, y;
      wgc_unpackCursorPos(pending, &x, &y);
      wgc_updatePointer(this, x, y);
    }
    if (timings)
      d12_timingRecord(D12_TIMING_WGC_POINTER, microtime() - timingStart);
    if (profile)
      wgc_recordProfileStage(this, WGC_PROFILE_POINTER,
        microtime() - profileStart);

    profileStart = profile ? microtime() : 0;
    timingStart = timings ? microtime() : 0;
    dst->copyFailed = false;
    wgc_copyFrameTexture(this, dst, *src, true);
    if (dst->copyFailed)
      goto exit;
    if (timings)
      d12_timingRecord(D12_TIMING_WGC_COPY, microtime() - timingStart);
    if (profile)
      wgc_recordProfileStage(this, WGC_PROFILE_PUBLISH_COPY,
        microtime() - profileStart);

    profileStart = profile ? microtime() : 0;
    timingStart = timings ? microtime() : 0;
    LG_PROFILE_ZONE_BEGIN(zoneFlushDirect, "wgc d3d11 flush");
    // Two-device path: bridge-A copy and fence Signal are on this->context;
    // wrap copy and Release are on this->on12Context. Flush both.
    ID3D11DeviceContext4_Flush(*this->context);
    if (this->twoDeviceBridge)
      ID3D11DeviceContext4_Flush(*this->on12Context);
    LG_PROFILE_ZONE_END(zoneFlushDirect);
    WGC_TRACY_D3D11_COLLECT(this->tracyWgcCtx);
    WGC_TRACY_D3D11_COLLECT(this->tracyOn12Ctx);
    if (timings)
      d12_timingRecord(D12_TIMING_WGC_SIGNAL, microtime() - timingStart);
    if (profile)
      wgc_recordProfileStage(this, WGC_PROFILE_FLUSH,
        microtime() - profileStart);

    wgc_clearAccumulatedDamage(dst);
    InterlockedExchange(&dst->state, WGC_FRAME_READY);
    WGCFrameInfo * old = InterlockedExchangePointer(
      (PVOID volatile *)&this->current, dst);
    if (old && old != dst)
      InterlockedCompareExchange(&old->state,
        WGC_FRAME_FREE, WGC_FRAME_READY);
    result = CAPTURE_RESULT_OK;
    goto exit;
  }

  profileStart = profile ? microtime() : 0;
  timingStart = timings ? microtime() : 0;
  accum = &this->frames[0];
  if (!wgc_ensureFrame(this, accum, *src))
    goto exit;
  if (!wgc_ensureAllFrames(this, *src))
    goto exit;
  accum->callbackTimeUs = callbackTimeUs;
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_ENSURE, microtime() - timingStart);
  if (profile)
    wgc_recordProfileStage(this, WGC_PROFILE_ENSURE,
      microtime() - profileStart);

  timingStart = timings ? microtime() : 0;
  accum->hasSystemRelativeTime = false;
  accum->systemRelativeTime    = 0;
  TimeSpan systemRelativeTime;
  hr = IDirect3D11CaptureFrame_get_SystemRelativeTime(
    frame, &systemRelativeTime);
  if (SUCCEEDED(hr))
  {
    accum->hasSystemRelativeTime = true;
    accum->systemRelativeTime    = systemRelativeTime.Duration;
  }

  if (timings)
    d12_timingRecord(D12_TIMING_WGC_METADATA, microtime() - timingStart);

  if (this->cursorMode == WGC_CURSOR_MODE_SEPARATE &&
      !this->mouseHookCreated)
  {
    wgcCursorInstance = this;
    mouseHook_install(wgc_onMouseMove);
    this->mouseHookCreated = true;
    wgc_updatePointer(this, INT_MIN, INT_MIN);
  }

  profileStart = profile ? microtime() : 0;
  timingStart = timings ? microtime() : 0;
  wgc_updateDamage(this, accum, frame);
  const bool forceFullCopyAfterGap =
    wgc_shouldForceFullCopyAfterGap(this, accum);
  const bool forceNextFullCopy =
    InterlockedExchange(&this->forceNextFullCopy, 0);
  const bool hasDirtyRects = accum->nbDirtyRects > 0;
  const bool keepGapDamage =
    forceFullCopyAfterGap && this->base.trackDamage && accum->copiedOnce &&
    hasDirtyRects && !forceNextFullCopy;
  if (forceFullCopyAfterGap)
  {
    if (this->debugStats)
      InterlockedIncrement(&this->fullCopyAfterGap);
  }
  if (forceNextFullCopy)
    accum->nbDirtyRects = 0;
  accum->fullCopy = !this->base.trackDamage || !accum->copiedOnce ||
    accum->nbDirtyRects == 0 || forceFullCopyAfterGap;
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_DAMAGE, microtime() - timingStart);
  if (profile)
    wgc_recordProfileStage(this, WGC_PROFILE_DAMAGE,
      microtime() - profileStart);

  profileStart = profile ? microtime() : 0;
  timingStart = timings ? microtime() : 0;
  wgc_copyFrameTexture(this, accum, *src, false);
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_COPY, microtime() - timingStart);
  if (profile)
    wgc_recordProfileStage(this, WGC_PROFILE_ACCUM_COPY,
      microtime() - profileStart);

  if (keepGapDamage)
    accum->fullCopy = false;

  wgc_accumulateDamage(this, accum);

  profileStart = profile ? microtime() : 0;
  timingStart = timings ? microtime() : 0;
  if (this->cursorMode == WGC_CURSOR_MODE_SEPARATE)
  {
    const LONG64 pending = InterlockedExchange64(
      &this->cursorPendingPos, wgc_packCursorPos(INT_MIN, INT_MIN));
    int x, y;
    wgc_unpackCursorPos(pending, &x, &y);
    wgc_updatePointer(this, x, y);
  }
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_POINTER, microtime() - timingStart);
  if (profile)
    wgc_recordProfileStage(this, WGC_PROFILE_POINTER,
      microtime() - profileStart);

  unsigned publishIndex;
  if (wgc_isIvshmemPublishMode(this->publishMode))
  {
    publishIndex = frameBufferIndex + 1;
    if (frameBufferIndex >= this->frameCount - 1)
    {
      DEBUG_ERROR("WGC direct frame index %u is out of range",
        frameBufferIndex);
      goto exit;
    }

    dst = &this->frames[publishIndex];
    if (!dst->ivshmemSlotReady)
    {
      result = CAPTURE_RESULT_TIMEOUT;
      goto exit;
    }

    LONG state = InterlockedCompareExchange(&dst->state,
      WGC_FRAME_WRITING, WGC_FRAME_FREE);
    if (state != WGC_FRAME_FREE)
      state = InterlockedCompareExchange(&dst->state,
        WGC_FRAME_WRITING, WGC_FRAME_READY);

    if (state != WGC_FRAME_READY && state != WGC_FRAME_FREE)
    {
      InterlockedIncrement(&this->asyncSlotBusy);
      InterlockedExchange(&this->forceNextFullCopy, 1);
      result = CAPTURE_RESULT_TIMEOUT;
      goto exit;
    }
    if (state == WGC_FRAME_READY)
      wgc_waitFrameD3D12Copy(this, dst);
  }
  else if (!wgc_selectAsyncSlot(this, &publishIndex))
  {
    result = CAPTURE_RESULT_TIMEOUT;
    goto exit;
  }
  else
    dst = &this->frames[publishIndex];

  if (!wgc_ensureFrame(this, dst, *accum->texture))
    goto exit;

  dst->hasSystemRelativeTime = accum->hasSystemRelativeTime;
  dst->systemRelativeTime    = accum->systemRelativeTime;
  dst->callbackTimeUs        = accum->callbackTimeUs;
  dst->fullCopy              = dst->pendingFullCopy || !dst->copiedOnce ||
                               dst->nbPendingDirtyRects == 0;
  dst->nbDirtyRects          = dst->fullCopy ? 0 : dst->nbPendingDirtyRects;
  if (dst->nbDirtyRects > 0)
    memcpy(dst->dirtyRects, dst->pendingDirtyRects,
      dst->nbDirtyRects * sizeof(*dst->dirtyRects));

  profileStart = profile ? microtime() : 0;
  timingStart = timings ? microtime() : 0;
  dst->copyFailed = false;
  wgc_copyFrameTexture(this, dst, *accum->texture, true);
  if (dst->copyFailed)
    goto exit;
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_COPY, microtime() - timingStart);
  if (profile)
    wgc_recordProfileStage(this, WGC_PROFILE_PUBLISH_COPY,
      microtime() - profileStart);

  profileStart = profile ? microtime() : 0;
  timingStart = timings ? microtime() : 0;
  if (this->publishMode == WGC_PUBLISH_D12_SHARE)
  {
    // D12 consumer waits on this fence before issuing its copy queue work.
    ++dst->fenceValue;
    ID3D11DeviceContext4_Signal(*this->context, *dst->fence, dst->fenceValue);
  }
  // CPU consumer relies on D3D11 Map() blocking for outstanding GPU work; we
  // still Flush to make sure the copy gets to the driver promptly.
  LG_PROFILE_ZONE_BEGIN(zoneFlush, "wgc d3d11 flush");
  ID3D11DeviceContext4_Flush(*this->context);
  LG_PROFILE_ZONE_END(zoneFlush);
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_SIGNAL, microtime() - timingStart);
  if (profile)
    wgc_recordProfileStage(this, WGC_PROFILE_FLUSH,
      microtime() - profileStart);

  wgc_clearAccumulatedDamage(dst);

  InterlockedExchange(&dst->state, WGC_FRAME_READY);
  WGCFrameInfo * old = InterlockedExchangePointer(
    (PVOID volatile *)&this->current, dst);
  if (old && old != dst)
    InterlockedCompareExchange(&old->state,
      WGC_FRAME_FREE, WGC_FRAME_READY);

  result = CAPTURE_RESULT_OK;

exit:
  if (result != CAPTURE_RESULT_OK && dst)
    InterlockedCompareExchange(&dst->state,
      WGC_FRAME_FREE, WGC_FRAME_WRITING);
  comRef_scopePop();
  return result;
}

static CaptureResult wgc_capture(D12Backend * instance,
  unsigned frameBufferIndex)
{
  WGCInstance * this = UPCAST(WGCInstance, instance);
  CaptureResult result = CAPTURE_RESULT_ERROR;
  IDirect3D11CaptureFrame * frame = NULL;
  comRef_scopePush(8);

  const bool timings = d12_timingEnabled();
  const bool callbackProcessed = this->asyncCapture &&
    !wgc_isIvshmemPublishMode(this->publishMode);

  if (callbackProcessed && wgc_asyncFrameReady(this))
  {
    if (this->debugStats)
      InterlockedIncrement(&this->asyncReadyBeforeWait);
    result = CAPTURE_RESULT_OK;
    goto exit;
  }

  uint64_t timingStart = timings ? microtime() : 0;
  const DWORD wait = WaitForSingleObject(this->frameEvent, 1000);
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_WAIT, microtime() - timingStart);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT)
  {
    DEBUG_WINERROR("Waiting for a WGC frame failed", GetLastError());
    goto exit;
  }

  if (callbackProcessed)
  {
    if (wgc_asyncFrameReady(this))
    {
      if (this->debugStats)
        InterlockedIncrement(&this->asyncReadyAfterWait);
      result = CAPTURE_RESULT_OK;
    }
    else
    {
      wgc_maybeDwmFlushOnGap(this);
      if (this->debugStats)
        InterlockedIncrement(&this->asyncTimeouts);
      result = CAPTURE_RESULT_TIMEOUT;
    }
    wgc_maybeLogDebugStats(this);
    goto exit;
  }

  timingStart = timings ? microtime() : 0;
  frame = InterlockedExchangePointer(
    (PVOID volatile *)&this->pendingFrame, NULL);
  if (timings)
    d12_timingRecord(D12_TIMING_WGC_TAKE_FRAME, microtime() - timingStart);

  // IVSHMEM_DIRECT diagnostic: if the callback hasn't given us a pending
  // frame, poll the frame pool directly. This tells us whether (a) the
  // FrameArrived notification stopped firing while frames are still queued
  // (we'd get a frame here) or (b) the compositor capture pipeline itself
  // has stopped producing (we'd get NULL).
  if (!frame && wgc_isIvshmemPublishMode(this->publishMode) &&
      this->framePool)
  {
    IDirect3D11CaptureFrame * polled = NULL;
    HRESULT pollHr = IDirect3D11CaptureFramePool_TryGetNextFrame(
      *this->framePool, &polled);
    if (SUCCEEDED(pollHr) && polled)
    {
      DEBUG_INFO("WGC direct: pulled frame via direct TryGetNextFrame poll "
        "(FrameArrived was silent)");
      frame = polled;
      LG_PROFILE_FRAME("wgc polled frame");
    }
    else if (++this->emptyPolls % 3 == 0)
      DEBUG_INFO("WGC direct: TryGetNextFrame poll returned NULL "
        "(hr=0x%08lx emptyPolls=%u events:%ld) — pool is empty",
        (unsigned long)pollHr, this->emptyPolls,
        this->handler ? this->handler->events : 0);
  }

  if (!frame)
  {
    wgc_maybeDwmFlushOnGap(this);
    if (++this->emptyPolls % 30 == 0)
      DEBUG_INFO("WGC has no pending frame (idx=%u publishMode=%d emptyPolls=%u events:%ld)",
        frameBufferIndex, (int)this->publishMode, this->emptyPolls,
        this->handler ? this->handler->events : 0);
    result = CAPTURE_RESULT_TIMEOUT;
    goto exit;
  }
  this->emptyPolls = 0;
  if (this->handler && !this->asyncCapture)
    InterlockedIncrement(&this->handler->framesConsumed);

  result = wgc_processFrame(this, frame, frameBufferIndex, 0);
  goto exit;

exit:
  wgc_maybeLogDebugStats(this);
  wgc_releaseFrame(&frame);
  comRef_scopePop();
  return result;
}

static void wgc_maybeDwmFlushOnGap(WGCInstance * this)
{
  if (!this->dwmFlushOnGap || !this->handler)
    return;

  const uint64_t now = microtime();
  const LONG64 last = InterlockedCompareExchange64(
    &this->handler->lastCallbackUs, 0, 0);
  if (last <= 0)
    return;

  const uint64_t gapUs = now - (uint64_t)last;
  const uint64_t thresholdUs = (uint64_t)this->dwmFlushGapMs * 1000;
  if (gapUs < thresholdUs || now - this->lastDwmFlushUs < thresholdUs)
    return;

  this->lastDwmFlushUs = now;
  const HRESULT hr = DwmFlush();
  if (FAILED(hr) && this->debugStats)
    DEBUG_WINERROR("DwmFlush failed", hr);
}

static bool wgc_asyncFrameReady(WGCInstance * this)
{
  WGCFrameInfo * frame = InterlockedCompareExchangePointer(
    (PVOID volatile *)&this->current, NULL, NULL);
  return frame && InterlockedCompareExchange(&frame->state,
    WGC_FRAME_READY, WGC_FRAME_READY) == WGC_FRAME_READY;
}

static CaptureResult wgc_sync(D12Backend * instance,
  ID3D12CommandQueue * commandQueue)
{
  WGCInstance * this = UPCAST(WGCInstance, instance);

  WGCFrameInfo * frame = InterlockedCompareExchangePointer(
    (PVOID volatile *)&this->consumerFrame, NULL, NULL);

  if (!frame)
    return CAPTURE_RESULT_TIMEOUT;

  const bool timings = d12_timingEnabled();
  const uint64_t timingStart = timings ? microtime() : 0;
  if (ID3D11Fence_GetCompletedValue(*frame->fence) < frame->fenceValue)
    ID3D12CommandQueue_Wait(commandQueue, *frame->d12Fence, frame->fenceValue);
  if (timings)
    d12_timingRecord(D12_TIMING_D12_SYNC, microtime() - timingStart);

  return CAPTURE_RESULT_OK;
}

static ID3D12Resource * wgc_fetch(D12Backend * instance,
  unsigned frameBufferIndex, D12FrameDesc * desc)
{
  WGCInstance * this = UPCAST(WGCInstance, instance);

  WGCFrameInfo * frame = InterlockedCompareExchangePointer(
    (PVOID volatile *)&this->current, NULL, NULL);

  if (!frame)
    return NULL;

  const LONG state = InterlockedCompareExchange(&frame->state,
    WGC_FRAME_CONSUMING, WGC_FRAME_READY);
  if (state == WGC_FRAME_READY)
    InterlockedExchangePointer((PVOID volatile *)&this->consumerFrame, frame);
  else if (state != WGC_FRAME_CONSUMING ||
      InterlockedCompareExchangePointer(
        (PVOID volatile *)&this->consumerFrame, NULL, NULL) != frame)
    return NULL;

  if (state == WGC_FRAME_READY && this->asyncCapture && this->handler)
    InterlockedIncrement(&this->handler->framesConsumed);

  desc->dirtyRects   = frame->dirtyRects;
  desc->nbDirtyRects = frame->nbDirtyRects;
  desc->rotation     = CAPTURE_ROT_0;
  desc->colorSpace   = this->colorSpace;

  ID3D12Resource_AddRef(*frame->d12Res);
  return *frame->d12Res;
}

static void wgc_releaseSlot(D12Backend * instance, void * token)
{
  if (!token)
    return;

  WGCInstance * this = UPCAST(WGCInstance, instance);
  WGCFrameInfo * frame = token;
  InterlockedCompareExchangePointer(
    (PVOID volatile *)&this->current, NULL, frame);
  InterlockedCompareExchangePointer(
    (PVOID volatile *)&this->consumerFrame, NULL, frame);
  InterlockedCompareExchange(&frame->state,
    WGC_FRAME_FREE, WGC_FRAME_CONSUMING);
}

static bool wgc_createCaptureItem(WGCInstance * this, HMONITOR monitor)
{
  bool result = false;
  HRESULT hr;
  comRef_scopePush(2);

  HSTRING className = NULL;
  if (!wgc_createHString(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem,
      &className))
    goto exit;

  comRef_defineLocal(IGraphicsCaptureItemInterop, interop);
  hr = RoGetActivationFactory(className, &IID_IGraphicsCaptureItemInterop,
    (void **)interop);
  WindowsDeleteString(className);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get GraphicsCaptureItem interop", hr);
    goto exit;
  }

  comRef_defineLocal(IGraphicsCaptureItem, item);
  hr = IGraphicsCaptureItemInterop_CreateForMonitor(
    *interop, monitor, &IID_IGraphicsCaptureItem, (void **)item);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("CreateForMonitor failed", hr);
    goto exit;
  }

  comRef_toGlobal(this->item, item);
  result = true;

exit:
  comRef_scopePop();
  return result;
}

static bool wgc_createFramePool(WGCInstance * this)
{
  bool result = false;
  HRESULT hr;
  comRef_scopePush(5);

  HSTRING className = NULL;
  if (!wgc_createHString(
      RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool,
      &className))
    goto exit;

  comRef_defineLocal(IDirect3D11CaptureFramePoolStatics2, statics);
  hr = RoGetActivationFactory(className,
    &IID_IDirect3D11CaptureFramePoolStatics2, (void **)statics);
  WindowsDeleteString(className);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get WGC frame pool statics", hr);
    goto exit;
  }

  const bool hdrSource = wgc_colorSpaceIsHDR(this->colorSpace);
  const bool hdrCapablePublish =
    this->ivshmemFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
    wgc_isNV12PackedIvshmem(this);
  const DirectXPixelFormat poolFormat =
    (hdrSource && hdrCapablePublish) ?
      DirectXPixelFormat_R16G16B16A16Float :
      DirectXPixelFormat_B8G8R8A8UIntNormalized;

  DEBUG_INFO("WGC frame pool format: %s (colorSpace:0x%x ivshmemFormat:0x%x)",
    poolFormat == DirectXPixelFormat_R16G16B16A16Float ? "RGBA16F" : "BGRA8",
    (unsigned)this->colorSpace, (unsigned)this->ivshmemFormat);

  comRef_defineLocal(IDirect3D11CaptureFramePool, framePool);
  hr = IDirect3D11CaptureFramePoolStatics2_CreateFreeThreaded(
    *statics,
    *this->graphicsDevice,
    poolFormat,
    FRAME_POOL_BUFFERS,
    this->size,
    framePool);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create WGC frame pool", hr);
    goto exit;
  }

  comRef_defineLocal(IGraphicsCaptureSession, session);
  hr = IDirect3D11CaptureFramePool_CreateCaptureSession(
    *framePool, *this->item, session);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create WGC capture session", hr);
    goto exit;
  }

  comRef_defineLocal(IGraphicsCaptureSession2, session2);
  hr = IGraphicsCaptureSession_QueryInterface(
    *session, &IID_IGraphicsCaptureSession2, (void **)session2);
  if (SUCCEEDED(hr))
    IGraphicsCaptureSession2_put_IsCursorCaptureEnabled(
      *session2, this->cursorMode == WGC_CURSOR_MODE_EMBEDDED);

  comRef_defineLocal(IGraphicsCaptureSession3, session3);
  hr = IGraphicsCaptureSession_QueryInterface(
    *session, &IID_IGraphicsCaptureSession3, (void **)session3);
  if (SUCCEEDED(hr))
  {
    hr = IGraphicsCaptureSession3_put_IsBorderRequired(*session3, false);
    if (FAILED(hr))
      DEBUG_WINERROR("Failed to disable WGC capture border", hr);
    else
      DEBUG_INFO("WGC capture border disabled");
  }
  else
    DEBUG_WARN("WGC capture border control is not available on this OS");

  if (this->includeSecondaryWindows)
  {
    comRef_defineLocal(IGraphicsCaptureSession6, session6);
    hr = IGraphicsCaptureSession_QueryInterface(
      *session, &IID_IGraphicsCaptureSession6, (void **)session6);
    if (SUCCEEDED(hr))
    {
      hr = IGraphicsCaptureSession6_put_IncludeSecondaryWindows(
        *session6, true);
      if (FAILED(hr))
        DEBUG_WINERROR("Failed to enable WGC secondary windows", hr);
    }
    else
      DEBUG_WARN("WGC secondary windows are not available on this OS");
  }

  wgc_setMinUpdateInterval(*session, this->maxFPS);

  hr = IDirect3D11CaptureFramePool_add_FrameArrived(
    *framePool, &this->handler->base, &this->frameArrivedToken);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to subscribe to WGC frames", hr);
    goto exit;
  }

  comRef_toGlobal(this->framePool, framePool);
  comRef_toGlobal(this->session  , session  );
  result = true;

exit:
  comRef_scopePop();
  return result;
}

static void wgc_updateDamage(WGCInstance * this, WGCFrameInfo * info,
  IDirect3D11CaptureFrame * frame)
{
  info->nbDirtyRects = 0;

  if (!this->base.trackDamage)
    return;

  HRESULT hr;
  comRef_scopePush(2);

  comRef_defineLocal(IDirect3D11CaptureFrame2, frame2);
  hr = IDirect3D11CaptureFrame_QueryInterface(
    frame, &IID_IDirect3D11CaptureFrame2, (void **)frame2);
  if (FAILED(hr))
    goto exit;

  comRef_defineLocal(IVectorView_RectInt32, regions);
  hr = IDirect3D11CaptureFrame2_get_DirtyRegions(*frame2, regions);
  if (FAILED(hr) || !*regions)
    goto exit;

  UINT32 count = 0;
  hr = IVectorView_RectInt32_get_Size(*regions, &count);
  if (FAILED(hr) || count == 0 || count > ARRAY_LENGTH(info->dirtyRects))
    goto exit;

  for(UINT32 i = 0; i < count; ++i)
  {
    RectInt32 rect;
    hr = IVectorView_RectInt32_GetAt(*regions, i, &rect);
    if (FAILED(hr) || rect.Width <= 0 || rect.Height <= 0)
    {
      info->nbDirtyRects = 0;
      goto exit;
    }

    const LONG left   = max(0, rect.X);
    const LONG top    = max(0, rect.Y);
    const LONG right  = min(this->size.Width , rect.X + rect.Width );
    const LONG bottom = min(this->size.Height, rect.Y + rect.Height);
    if (right <= left || bottom <= top)
      continue;

    info->dirtyRects[info->nbDirtyRects++] = (RECT)
    {
      .left   = left,
      .top    = top,
      .right  = right,
      .bottom = bottom
    };
  }

exit:
  comRef_scopePop();
}

static bool wgc_shouldForceFullCopyAfterGap(WGCInstance * this,
  WGCFrameInfo * frame)
{
  if (!this->base.trackDamage || !frame->hasSystemRelativeTime)
    return false;

  bool forceFullCopy = false;
  if (this->hasLastSystemRelativeTime &&
      frame->systemRelativeTime > this->lastSystemRelativeTime)
  {
    const int64_t delta =
      frame->systemRelativeTime - this->lastSystemRelativeTime;
    forceFullCopy = delta > WGC_FULL_COPY_GAP_100NS;
  }

  this->hasLastSystemRelativeTime = true;
  this->lastSystemRelativeTime = frame->systemRelativeTime;
  return forceFullCopy;
}

static void wgc_interlockedMax64(volatile LONG64 * target, LONG64 value)
{
  LONG64 old = InterlockedCompareExchange64(target, 0, 0);
  while(value > old &&
      InterlockedCompareExchange64(target, value, old) != old)
    old = InterlockedCompareExchange64(target, 0, 0);
}

static int wgc_compareLong64(const void * a, const void * b)
{
  const LONG64 av = *(const LONG64 *)a;
  const LONG64 bv = *(const LONG64 *)b;
  return (av > bv) - (av < bv);
}

static LONG64 wgc_percentileLong64(LONG64 * values, LONG count, unsigned pct)
{
  if (count <= 0)
    return 0;
  qsort(values, count, sizeof(values[0]), wgc_compareLong64);
  LONG idx = (LONG)(((uint64_t)pct * (uint64_t)(count - 1) + 99) / 100);
  if (idx < 0)
    idx = 0;
  if (idx >= count)
    idx = count - 1;
  return values[idx];
}

static LONG wgc_copyAndResetSamples(volatile LONG * sampleCount,
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

static void wgc_recordSample(volatile LONG * sampleCount,
  volatile LONG * overflow, LONG64 * samples, LONG64 value)
{
  const LONG idx = InterlockedIncrement(sampleCount) - 1;
  if (idx >= 0 && idx < WGC_STATS_SAMPLE_MAX)
    samples[idx] = value;
  else
    InterlockedExchange(overflow, 1);
}

static void wgc_maybeLogDebugStats(WGCInstance * this)
{
  if (!this->debugStats)
    return;

  const uint64_t now = microtime();
  if (now - this->debugStatsLastLog < 1000000)
    return;
  this->debugStatsLastLog = now;
  const LONG64 cbGapCount = this->handler ?
    InterlockedExchange64(&this->handler->callbackGapCount, 0) : 0;
  const LONG64 cbGapTotal = this->handler ?
    InterlockedExchange64(&this->handler->callbackGapTotalUs, 0) : 0;
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
    profileCount[i] = InterlockedExchange64(&this->profileCount  [i], 0);
    profileTotal[i] = InterlockedExchange64(&this->profileTotalUs[i], 0);
    profileMax  [i] = InterlockedExchange64(&this->profileMaxUs  [i], 0);
    const LONG n = wgc_copyAndResetSamples(&this->profileSampleCount[i],
      this->profileSamples[i], sampleScratch);
    profileP50[i] = wgc_percentileLong64(sampleScratch, n, 50);
    profileP95[i] = wgc_percentileLong64(sampleScratch, n, 95);
    profileP99[i] = wgc_percentileLong64(sampleScratch, n, 99);
    histOverflow |= InterlockedExchange(&this->profileSampleOverflow[i], 0);
  }
#define WGC_PROFILE_AVG(stage) \
  (long long)(profileCount[(stage)] ? \
    profileTotal[(stage)] / profileCount[(stage)] : 0)
  const LONG64 d3d12CopySubmitCount =
    InterlockedExchange64(&this->d3d12CopySubmitCount, 0);
  const LONG64 d3d12CopySubmitTotal =
    InterlockedExchange64(&this->d3d12CopySubmitTotalUs, 0);
  const LONG64 d3d12CopySubmitMax =
    InterlockedExchange64(&this->d3d12CopySubmitMaxUs, 0);
  LONG submitSampleCount = wgc_copyAndResetSamples(
    &this->d3d12CopySubmitSampleCount,
    this->d3d12CopySubmitSamples, sampleScratch);
  const LONG64 d3d12CopySubmitP50 =
    wgc_percentileLong64(sampleScratch, submitSampleCount, 50);
  const LONG64 d3d12CopySubmitP95 =
    wgc_percentileLong64(sampleScratch, submitSampleCount, 95);
  const LONG64 d3d12CopySubmitP99 =
    wgc_percentileLong64(sampleScratch, submitSampleCount, 99);
  histOverflow |= InterlockedExchange(&this->d3d12CopySubmitSampleOverflow, 0);

  const LONG64 d3d12FenceWaitCount =
    InterlockedExchange64(&this->d3d12FenceWaitCount, 0);
  const LONG64 d3d12FenceWaitTotal =
    InterlockedExchange64(&this->d3d12FenceWaitTotalUs, 0);
  const LONG64 d3d12FenceWaitMax =
    InterlockedExchange64(&this->d3d12FenceWaitMaxUs, 0);
  LONG fenceSampleCount = wgc_copyAndResetSamples(
    &this->d3d12FenceWaitSampleCount,
    this->d3d12FenceWaitSamples, sampleScratch);
  const LONG64 d3d12FenceWaitP50 =
    wgc_percentileLong64(sampleScratch, fenceSampleCount, 50);
  const LONG64 d3d12FenceWaitP95 =
    wgc_percentileLong64(sampleScratch, fenceSampleCount, 95);
  const LONG64 d3d12FenceWaitP99 =
    wgc_percentileLong64(sampleScratch, fenceSampleCount, 99);
  histOverflow |= InterlockedExchange(&this->d3d12FenceWaitSampleOverflow, 0);

  DEBUG_INFO(
    "WGC debug stats ready-pre:%ld ready-post:%ld timeouts:%ld bursts:%ld max-batch:%ld cb-gap-avg-us:%lld cb-gap-max-us:%lld cb-gap-count:%lld gap-full:%ld slot-busy:%ld events:%ld pulled:%ld consumed:%ld copy-accum-full:%lld copy-accum-dirty:%lld copy-accum-kpix:%lld copy-publish-full:%lld copy-publish-dirty:%lld copy-publish-kpix:%lld prof-acquire-avg/max:%lld/%lld prof-ensure-avg/max:%lld/%lld prof-damage-avg/max:%lld/%lld prof-accum-copy-avg/max:%lld/%lld prof-pointer-avg/max:%lld/%lld prof-publish-copy-avg/max:%lld/%lld prof-publish-copy-p50/p95/p99:%lld/%lld/%lld prof-flush-avg/max:%lld/%lld d3d12-copy-submit-avg/max:%lld/%lld d3d12-copy-submit-p50/p95/p99:%lld/%lld/%lld d3d12-fence-wait-avg/max:%lld/%lld d3d12-fence-wait-p50/p95/p99:%lld/%lld/%lld hist-overflow:%ld",
    InterlockedExchange(&this->asyncReadyBeforeWait, 0),
    InterlockedExchange(&this->asyncReadyAfterWait, 0),
    InterlockedExchange(&this->asyncTimeouts, 0),
    this->handler ? InterlockedExchange(&this->handler->callbackBursts, 0) : 0,
    this->handler ? InterlockedExchange(&this->handler->maxCallbackBatch, 0) : 0,
    (long long)(cbGapCount ? cbGapTotal / cbGapCount : 0),
    (long long)(this->handler ?
      InterlockedExchange64(&this->handler->callbackGapMaxUs, 0) : 0),
    (long long)cbGapCount,
    InterlockedExchange(&this->fullCopyAfterGap, 0),
    InterlockedExchange(&this->asyncSlotBusy, 0),
    this->handler ? InterlockedCompareExchange(&this->handler->events, 0, 0) : 0,
    this->handler ? InterlockedCompareExchange(&this->handler->framesPulled, 0, 0) : 0,
    this->handler ? InterlockedCompareExchange(&this->handler->framesConsumed, 0, 0) : 0,
    (long long)InterlockedExchange64(&this->copyAccumFull, 0),
    (long long)InterlockedExchange64(&this->copyAccumDirty, 0),
    (long long)(InterlockedExchange64(&this->copyAccumPixels, 0) / 1000),
    (long long)InterlockedExchange64(&this->copyPublishFull, 0),
    (long long)InterlockedExchange64(&this->copyPublishDirty, 0),
    (long long)(InterlockedExchange64(&this->copyPublishPixels, 0) / 1000),
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

static void wgc_recordProfileStage(WGCInstance * this, WGCProfileStage stage,
  uint64_t elapsedUs)
{
  InterlockedExchangeAdd64(&this->profileTotalUs[stage], (LONG64)elapsedUs);
  InterlockedIncrement64(&this->profileCount[stage]);
  wgc_interlockedMax64(&this->profileMaxUs[stage], (LONG64)elapsedUs);
  wgc_recordSample(&this->profileSampleCount[stage],
    &this->profileSampleOverflow[stage], this->profileSamples[stage],
    (LONG64)elapsedUs);
}

static void wgc_accumulateDamage(WGCInstance * this, const WGCFrameInfo * src)
{
  for(unsigned i = 1; i < this->frameCount; ++i)
  {
    WGCFrameInfo * frame = &this->frames[i];

    if (frame->pendingFullCopy || src->fullCopy)
    {
      frame->pendingFullCopy = true;
      frame->nbPendingDirtyRects = 0;
      continue;
    }

    if (frame->nbPendingDirtyRects + src->nbDirtyRects >
        ARRAY_LENGTH(frame->pendingDirtyRects))
    {
      frame->pendingFullCopy = true;
      frame->nbPendingDirtyRects = 0;
      continue;
    }

    memcpy(frame->pendingDirtyRects + frame->nbPendingDirtyRects,
      src->dirtyRects, src->nbDirtyRects * sizeof(*frame->pendingDirtyRects));
    frame->nbPendingDirtyRects += src->nbDirtyRects;
  }
}

static void wgc_clearAccumulatedDamage(WGCFrameInfo * frame)
{
  frame->pendingFullCopy = false;
  frame->nbPendingDirtyRects = 0;
}

static void wgc_releaseFrameInfo(WGCFrameInfo * frame)
{
  if (frame->texture)
    comRef_release(frame->texture);
  if (frame->d12Res)
    comRef_release(frame->d12Res);
  if (frame->fence)
    comRef_release(frame->fence);
  if (frame->d12Fence)
    comRef_release(frame->d12Fence);
  if (frame->ivshmemD12Res)
    comRef_release(frame->ivshmemD12Res);
  if (frame->bridgeA)
    comRef_release(frame->bridgeA);
  if (frame->bridgeB)
    comRef_release(frame->bridgeB);
  if (frame->bridge12)
    comRef_release(frame->bridge12);
  if (frame->encodeUav)
    comRef_release(frame->encodeUav);
  memset(frame, 0, sizeof(*frame));
}

static void wgc_copyFrameTextureRectCtx(ID3D11DeviceContext4 * ctx,
  ID3D11Resource * dst, ID3D11Resource * src, const RECT * rect)
{
  const D3D11_BOX box =
  {
    .left   = rect->left,
    .top    = rect->top,
    .front  = 0,
    .right  = rect->right,
    .bottom = rect->bottom,
    .back   = 1
  };

  ID3D11DeviceContext4_CopySubresourceRegion1(
    ctx,
    dst,
    0,
    rect->left,
    rect->top,
    0,
    src,
    0,
    &box,
    0);
}

static bool wgc_isNV12PackedIvshmem(const WGCInstance * this)
{
  return this->ivshmemFormat == DXGI_FORMAT_R8G8B8A8_UNORM;
}

static bool wgc_colorSpaceIsHDR(DXGI_COLOR_SPACE_TYPE colorSpace)
{
  return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
         colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 ||
         colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

static unsigned wgc_nv12StorageHeight(unsigned height)
{
  return height + (height + 1) / 2;
}

static unsigned wgc_nv12EncodedWidth(unsigned width)
{
  return ((width + 255u) & ~255u) / 4;
}

static bool wgc_ensureNV12Shader(WGCInstance * this)
{
  if (this->nv12Shader)
    return true;

  static const char shaderCode[] =
    "Texture2D<float4> srcTex : register(t0);\n"
    "RWTexture2D<float4> dstTex : register(u0);\n"
    "\n"
    "float3 bgraToRgb(float4 c)\n"
    "{\n"
    "  return c.rgb;\n"
    "}\n"
    "\n"
    "float3 prepareRgb(float3 rgb)\n"
    "{\n"
    "#if WGC_HDR_TONEMAP\n"
    "  rgb = max(rgb, 0.0.xxx);\n"
    "  float peak = max(rgb.r, max(rgb.g, rgb.b));\n"
    "  if (peak > 1.0)\n"
    "    rgb /= peak;\n"
    "#endif\n"
    "  return saturate(rgb);\n"
    "}\n"
    "\n"
    "float luma(float3 rgb)\n"
    "{\n"
    "  return dot(rgb, float3(0.2126, 0.7152, 0.0722));\n"
    "}\n"
    "\n"
    "[numthreads(16, 16, 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  uint srcW, srcH;\n"
    "  srcTex.GetDimensions(srcW, srcH);\n"
    "  uint2 p0 = uint2(dt.x * 4, dt.y * 2);\n"
    "  if (p0.x >= srcW || p0.y >= srcH)\n"
    "    return;\n"
    "\n"
    "  uint2 p1 = uint2(min(p0.x + 1, srcW - 1), p0.y);\n"
    "  uint2 p2 = uint2(p0.x, min(p0.y + 1, srcH - 1));\n"
    "  uint2 p3 = uint2(p1.x, p2.y);\n"
    "  uint2 p4 = uint2(min(p0.x + 2, srcW - 1), p0.y);\n"
    "  uint2 p5 = uint2(min(p0.x + 3, srcW - 1), p0.y);\n"
    "  uint2 p6 = uint2(p4.x, p2.y);\n"
    "  uint2 p7 = uint2(p5.x, p2.y);\n"
    "  float3 c0 = prepareRgb(bgraToRgb(srcTex[p0]));\n"
    "  float3 c1 = prepareRgb(bgraToRgb(srcTex[p1]));\n"
    "  float3 c2 = prepareRgb(bgraToRgb(srcTex[p2]));\n"
    "  float3 c3 = prepareRgb(bgraToRgb(srcTex[p3]));\n"
    "  float3 c4 = prepareRgb(bgraToRgb(srcTex[p4]));\n"
    "  float3 c5 = prepareRgb(bgraToRgb(srcTex[p5]));\n"
    "  float3 c6 = prepareRgb(bgraToRgb(srcTex[p6]));\n"
    "  float3 c7 = prepareRgb(bgraToRgb(srcTex[p7]));\n"
    "  float y0 = luma(c0);\n"
    "  float y1 = luma(c1);\n"
    "  float y2 = luma(c2);\n"
    "  float y3 = luma(c3);\n"
    "  float y4 = luma(c4);\n"
    "  float y5 = luma(c5);\n"
    "  float y6 = luma(c6);\n"
    "  float y7 = luma(c7);\n"
    "  dstTex[uint2(dt.x, p0.y)] = saturate(float4(y0, y1, y4, y5));\n"
    "  if (p2.y < srcH)\n"
    "    dstTex[uint2(dt.x, p2.y)] = saturate(float4(y2, y3, y6, y7));\n"
    "\n"
    "  float3 avg0 = (c0 + c1 + c2 + c3) * 0.25;\n"
    "  float yy0 = luma(avg0);\n"
    "  float u0 = saturate((avg0.b - yy0) * 0.5389 + 0.5);\n"
    "  float v0 = saturate((avg0.r - yy0) * 0.6350 + 0.5);\n"
    "  float3 avg1 = (c4 + c5 + c6 + c7) * 0.25;\n"
    "  float yy1 = luma(avg1);\n"
    "  float u1 = saturate((avg1.b - yy1) * 0.5389 + 0.5);\n"
    "  float v1 = saturate((avg1.r - yy1) * 0.6350 + 0.5);\n"
    "  uint uvY = srcH + dt.y;\n"
    "  dstTex[uint2(dt.x, uvY)] = float4(u0, v0, u1, v1);\n"
    "}\n";

  bool result = false;
  HRESULT hr;
  comRef_scopePush(3);
  comRef_defineLocal(ID3DBlob, blob);
  comRef_defineLocal(ID3DBlob, error);
  comRef_defineLocal(ID3D11ComputeShader, shader);

  const bool hdrToneMap = wgc_colorSpaceIsHDR(this->colorSpace);
  const D3D_SHADER_MACRO macros[] =
  {
    { "WGC_HDR_TONEMAP", hdrToneMap ? "1" : "0" },
    { NULL, NULL }
  };
  hr = D3DCompile(shaderCode, strlen(shaderCode),
    NULL, macros, NULL, "main", "cs_5_0", 0, 0, blob, error);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Failed to compile WGC NV12 encoder shader");
    if (error && *error)
      DEBUG_ERROR("%s", (const char *)ID3DBlob_GetBufferPointer(*error));
    goto exit;
  }

  hr = ID3D11Device5_CreateComputeShader(*this->device,
    ID3DBlob_GetBufferPointer(*blob),
    ID3DBlob_GetBufferSize(*blob),
    NULL, shader);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("CreateComputeShader WGC NV12 encoder failed", hr);
    goto exit;
  }

  comRef_toGlobal(this->nv12Shader, shader);
  DEBUG_INFO("WGC NV12 encoder shader ready (hdrToneMap:%d colorSpace:0x%x)",
    hdrToneMap, (unsigned)this->colorSpace);
  result = true;

exit:
  comRef_scopePop();
  return result;
}

static bool wgc_encodeFrameNV12(WGCInstance * this, WGCFrameInfo * dst,
  ID3D11Texture2D * src)
{
  if (!dst->bridgeA || !dst->encodeUav || !wgc_ensureNV12Shader(this))
    return false;

  bool result = false;
  HRESULT hr;
  comRef_scopePush(1);
  comRef_defineLocal(ID3D11ShaderResourceView, srv);

  D3D11_TEXTURE2D_DESC srcDesc;
  ID3D11Texture2D_GetDesc(src, &srcDesc);
  D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc =
  {
    .Format        = srcDesc.Format,
    .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
    .Texture2D     = { .MostDetailedMip = 0, .MipLevels = 1 }
  };
  hr = ID3D11Device5_CreateShaderResourceView(*this->device,
    (ID3D11Resource *)src, &srvDesc, srv);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("CreateShaderResourceView WGC NV12 source failed", hr);
    goto exit;
  }

  ID3D11ShaderResourceView * srvs[1] = { *srv };
  ID3D11UnorderedAccessView * uavs[1] = { *dst->encodeUav };
  ID3D11DeviceContext4_CSSetShader(*this->context, *this->nv12Shader, NULL, 0);
  ID3D11DeviceContext4_CSSetShaderResources(*this->context, 0, 1, srvs);
  ID3D11DeviceContext4_CSSetUnorderedAccessViews(*this->context, 0, 1, uavs,
    NULL);
  ID3D11DeviceContext4_Dispatch(*this->context,
    (srcDesc.Width  + 31) / 32,
    (srcDesc.Height + 31) / 32,
    1);

  ID3D11ShaderResourceView * nullSrvs[1] = { NULL };
  ID3D11UnorderedAccessView * nullUavs[1] = { NULL };
  ID3D11DeviceContext4_CSSetShaderResources(*this->context, 0, 1, nullSrvs);
  ID3D11DeviceContext4_CSSetUnorderedAccessViews(*this->context, 0, 1,
    nullUavs, NULL);
  ID3D11DeviceContext4_CSSetShader(*this->context, NULL, NULL, 0);

  dst->fullCopy = true;
  result = true;

exit:
  comRef_scopePop();
  return result;
}

static void wgc_copyFrameTexture(WGCInstance * this, WGCFrameInfo * dst,
  ID3D11Texture2D * src, bool publishCopy)
{
  const uint64_t pixels = wgc_copyFramePixels(dst);
  wgc_recordCopyStats(this, dst, publishCopy, pixels);

  const bool directPublish = publishCopy &&
    wgc_isIvshmemPublishMode(this->publishMode) &&
    dst != &this->frames[0];

  // Two-device path: WGC source lives on the vanilla D3D11 device; the
  // IVSHMEM wrap lives on the D3D11On12-backed device. Bridge via a shared
  // VRAM texture (bridgeA on WGC device, bridgeB on On12 device — same
  // memory) with a cross-device fence between stages.
  const bool twoDeviceBridge = directPublish && this->twoDeviceBridge &&
    dst->bridgeA && dst->bridgeB;

  if (directPublish &&
      this->publishMode == WGC_PUBLISH_IVSHMEM_D3D12_COPY &&
      dst->bridgeA && dst->bridge12)
  {
    LG_PROFILE_ZONE_BEGIN(zoneCopy1, "wgc src->bridgeA (WGC ctx)");
    LG_PROFILE_ZONE_VALUE(zoneCopy1, pixels);
    const char * gpuName = dst->fullCopy ?
      "gpu wgc src->bridge" : "gpu wgc dirty src->bridge";
    WGCTracyD3D11Zone * gpuZone = WGC_TRACY_D3D11_ZONE_BEGIN_N(
      this->tracyWgcCtx, gpuName, strlen(gpuName));
    if (wgc_isNV12PackedIvshmem(this))
    {
      if (!wgc_encodeFrameNV12(this, dst, src))
      {
        WGC_TRACY_D3D11_ZONE_END(gpuZone);
        LG_PROFILE_ZONE_END(zoneCopy1);
        InterlockedExchange(&this->forceNextFullCopy, 1);
        dst->copyFailed = true;
        return;
      }
    }
    else if (dst->fullCopy)
      ID3D11DeviceContext4_CopyResource(*this->context,
        (ID3D11Resource *)*dst->bridgeA, (ID3D11Resource *)src);
    else
      for(const RECT * rect = dst->dirtyRects;
          rect < dst->dirtyRects + dst->nbDirtyRects; ++rect)
        wgc_copyFrameTextureRectCtx(*this->context,
          (ID3D11Resource *)*dst->bridgeA, (ID3D11Resource *)src, rect);
    WGC_TRACY_D3D11_ZONE_END(gpuZone);
    LG_PROFILE_ZONE_END(zoneCopy1);

    ++this->wgcFenceValue;
    ID3D11DeviceContext4_Signal(*this->context, *this->wgcFence,
      this->wgcFenceValue);
    ID3D11DeviceContext4_Flush(*this->context);

    if (!wgc_copyFrameTextureD3D12(this, dst))
    {
      InterlockedExchange(&this->forceNextFullCopy, 1);
      dst->copyFailed = true;
      return;
    }
    dst->copiedOnce = true;
    return;
  }

  if (twoDeviceBridge)
  {
    // Stage 1: WGC source → bridgeA on WGC context (VRAM→VRAM, same device).
    {
      LG_PROFILE_ZONE_BEGIN(zoneCopy1, "wgc src->bridgeA (WGC ctx)");
      LG_PROFILE_ZONE_VALUE(zoneCopy1, pixels);
      WGCTracyD3D11Zone * gpuZone =
        WGC_TRACY_D3D11_ZONE_BEGIN(this->tracyWgcCtx,
          "gpu wgc src->bridgeA");
      if (dst->fullCopy)
        ID3D11DeviceContext4_CopyResource(*this->context,
          (ID3D11Resource *)*dst->bridgeA, (ID3D11Resource *)src);
      else
        for(const RECT * rect = dst->dirtyRects;
            rect < dst->dirtyRects + dst->nbDirtyRects; ++rect)
          wgc_copyFrameTextureRectCtx(*this->context,
            (ID3D11Resource *)*dst->bridgeA, (ID3D11Resource *)src, rect);
      WGC_TRACY_D3D11_ZONE_END(gpuZone);
      LG_PROFILE_ZONE_END(zoneCopy1);
    }

    // Cross-device sync: WGC signals, D3D11On12 side waits.
    const UINT64 fenceVal = ++this->wgcFenceValue;
    ID3D11DeviceContext4_Signal(*this->context, *this->wgcFence, fenceVal);
    ID3D11DeviceContext4_Wait(*this->on12Context, *this->wgcFenceOn12,
      fenceVal);

    // Stage 2: bridgeB → IVSHMEM wrap on D3D11On12 context. Acquire/Release
    // brackets the whole copy batch so the D3D12-side state transitions are
    // emitted once per frame.
    ID3D11Resource * acquired[1] = { (ID3D11Resource *)*dst->texture };
    ID3D11On12Device_AcquireWrappedResources(this->d11on12Device, acquired, 1);
    {
      LG_PROFILE_ZONE_BEGIN(zoneCopy2, "wgc bridgeB->IVSHMEM wrap (On12 ctx)");
      LG_PROFILE_ZONE_VALUE(zoneCopy2, pixels);
      WGCTracyD3D11Zone * gpuZone =
        WGC_TRACY_D3D11_ZONE_BEGIN(this->tracyOn12Ctx,
          "gpu wgc bridgeB->IVSHMEM");
      if (dst->fullCopy)
        ID3D11DeviceContext4_CopyResource(*this->on12Context,
          (ID3D11Resource *)*dst->texture, (ID3D11Resource *)*dst->bridgeB);
      else
        for(const RECT * rect = dst->dirtyRects;
            rect < dst->dirtyRects + dst->nbDirtyRects; ++rect)
          wgc_copyFrameTextureRectCtx(*this->on12Context,
            (ID3D11Resource *)*dst->texture,
            (ID3D11Resource *)*dst->bridgeB, rect);
      WGC_TRACY_D3D11_ZONE_END(gpuZone);
      LG_PROFILE_ZONE_END(zoneCopy2);
    }
    ID3D11On12Device_ReleaseWrappedResources(this->d11on12Device, acquired, 1);
  }
  else
  {
    // Single-device path: dst is reachable from this->context directly. For
    // a D3D11On12 wrap (IVSHMEM_DIRECT publish with no bridge), the wrap
    // still needs Acquire/Release.
    const bool needsAcquire = directPublish && dst->ivshmemD12Res;
    ID3D11Resource * acquired[1];
    if (needsAcquire)
    {
      acquired[0] = (ID3D11Resource *)*dst->texture;
      ID3D11On12Device_AcquireWrappedResources(this->d11on12Device, acquired, 1);
    }

    LG_PROFILE_ZONE_BEGIN(zoneCopy, "wgc copy (single device)");
    LG_PROFILE_ZONE_VALUE(zoneCopy, pixels);
    WGCTracyD3D11Zone * gpuZone =
      WGC_TRACY_D3D11_ZONE_BEGIN(this->tracyWgcCtx, "gpu wgc copy");
    if (dst->fullCopy)
      ID3D11DeviceContext4_CopyResource(*this->context,
        (ID3D11Resource *)*dst->texture, (ID3D11Resource *)src);
    else
      for(const RECT * rect = dst->dirtyRects;
          rect < dst->dirtyRects + dst->nbDirtyRects; ++rect)
        wgc_copyFrameTextureRectCtx(*this->context,
          (ID3D11Resource *)*dst->texture, (ID3D11Resource *)src, rect);
    WGC_TRACY_D3D11_ZONE_END(gpuZone);
    LG_PROFILE_ZONE_END(zoneCopy);

    if (needsAcquire)
      ID3D11On12Device_ReleaseWrappedResources(this->d11on12Device, acquired, 1);
  }

  dst->copiedOnce = true;
}

static bool wgc_buildTileSpans(WGCInstance * this, const WGCFrameInfo * frame,
  RECT * spans, unsigned spanCapacity, unsigned * spanCount,
  uint64_t * spanPixels)
{
  *spanCount  = 0;
  *spanPixels = 0;

  if (this->tiledCopyMode != WGC_TILED_COPY_DIRTY ||
      this->tileWidth == 0 || this->tileHeight == 0 ||
      frame->nbDirtyRects == 0)
    return false;

  const unsigned width  = frame->format.Width;
  const unsigned height = frame->format.Height;
  const unsigned tilesX = (width  + this->tileWidth  - 1) / this->tileWidth;
  const unsigned tilesY = (height + this->tileHeight - 1) / this->tileHeight;
  const size_t tileCount = (size_t)tilesX * tilesY;
  if (tilesX == 0 || tilesY == 0 || tileCount > 65536)
    return false;

  uint8_t * mask = calloc(tileCount, 1);
  if (!mask)
    return false;

  for(const RECT * rect = frame->dirtyRects;
      rect < frame->dirtyRects + frame->nbDirtyRects; ++rect)
  {
    const LONG left   = max(0, min(rect->left,   (LONG)width));
    const LONG top    = max(0, min(rect->top,    (LONG)height));
    const LONG right  = max(0, min(rect->right,  (LONG)width));
    const LONG bottom = max(0, min(rect->bottom, (LONG)height));
    if (right <= left || bottom <= top)
      continue;

    const unsigned tx0 = (unsigned)left / this->tileWidth;
    const unsigned ty0 = (unsigned)top  / this->tileHeight;
    const unsigned tx1 = ((unsigned)right  + this->tileWidth  - 1) /
      this->tileWidth;
    const unsigned ty1 = ((unsigned)bottom + this->tileHeight - 1) /
      this->tileHeight;

    for(unsigned ty = ty0; ty < ty1 && ty < tilesY; ++ty)
      for(unsigned tx = tx0; tx < tx1 && tx < tilesX; ++tx)
        mask[(size_t)ty * tilesX + tx] = 1;
  }

  bool ok = true;
  for(unsigned ty = 0; ty < tilesY && ok; ++ty)
  {
    unsigned tx = 0;
    while (tx < tilesX)
    {
      while (tx < tilesX && !mask[(size_t)ty * tilesX + tx])
        ++tx;
      if (tx >= tilesX)
        break;

      const unsigned startX = tx;
      while (tx < tilesX && mask[(size_t)ty * tilesX + tx])
        ++tx;

      if (*spanCount >= spanCapacity || *spanCount >= this->dirtyMaxTiles)
      {
        ok = false;
        break;
      }

      RECT * span = &spans[(*spanCount)++];
      span->left   = (LONG)(startX * this->tileWidth);
      span->top    = (LONG)(ty * this->tileHeight);
      span->right  = (LONG)min(tx * this->tileWidth, width);
      span->bottom = (LONG)min((ty + 1) * this->tileHeight, height);
      *spanPixels += (uint64_t)(span->right - span->left) *
        (uint64_t)(span->bottom - span->top);
    }
  }

  free(mask);
  return ok && *spanCount > 0;
}

static bool wgc_copyFrameTextureD3D12(WGCInstance * this, WGCFrameInfo * dst)
{
  if (this->d3d12CopyQueueCount == 0 || !this->d3d12CopyCommandReady[0] ||
      !this->d3d12CopyQueues[0] || !this->wgcD3D12Fence ||
      !dst->bridge12 || !dst->ivshmemD12Res)
    return false;

  uint64_t dirtyPixels = 0;
  for(const RECT * rect = dst->dirtyRects;
      rect < dst->dirtyRects + dst->nbDirtyRects; ++rect)
  {
    const LONG width  = rect->right  - rect->left;
    const LONG height = rect->bottom - rect->top;
    if (width > 0 && height > 0)
      dirtyPixels += (uint64_t)width * height;
  }

  const uint64_t framePixels =
    (uint64_t)dst->format.Width * dst->format.Height;
  bool copyFull = this->d3d12FullCopyAlways || dst->fullCopy ||
    (this->dirtyFullCopyPercent > 0 && dst->nbDirtyRects > 0 &&
     dirtyPixels * 100 >= framePixels *
       (uint64_t)this->dirtyFullCopyPercent);
  RECT tileSpans[WGC_D3D12_TILE_SPAN_MAX];
  unsigned tileSpanCount = 0;
  uint64_t tilePixels = 0;
  const RECT * copyRects = dst->dirtyRects;
  unsigned copyRectCount = dst->nbDirtyRects;

  if (!copyFull && this->tiledCopyMode == WGC_TILED_COPY_DIRTY)
  {
    if (wgc_buildTileSpans(this, dst, tileSpans,
        WGC_D3D12_TILE_SPAN_MAX, &tileSpanCount, &tilePixels))
    {
      copyRects = tileSpans;
      copyRectCount = tileSpanCount;
    }
    else
      copyFull = true;
  }

  unsigned activeQueues = 1;
  if (copyFull && this->d3d12CopyQueueCount > 1)
  {
    activeQueues = this->d3d12CopyQueueCount;
    if (activeQueues > dst->format.Height)
      activeQueues = dst->format.Height;
  }

  for(unsigned i = 0; i < activeQueues; ++i)
  {
    if (!this->d3d12CopyCommandReady[i] || !this->d3d12CopyQueues[i])
      return false;

    D12CommandGroup * cmd = &this->d3d12CopyCommands[i];
    d12_commandGroupWait(cmd);
    if (!d12_commandGroupReset(cmd))
      return false;

    HRESULT hr = ID3D12CommandQueue_Wait(this->d3d12CopyQueues[i],
      *this->wgcD3D12Fence, this->wgcFenceValue);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-d3d12-copy: copy queue wait on WGC fence failed",
        hr);
      return false;
    }
  }

  D3D12_TEXTURE_COPY_LOCATION srcLoc =
  {
    .pResource        = *dst->bridge12,
    .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = 0
  };

  D3D12_TEXTURE_COPY_LOCATION dstLoc =
  {
    .pResource        = *dst->ivshmemD12Res,
    .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = 0
  };

  for(unsigned i = 0; i < activeQueues; ++i)
  {
    D12CommandGroup * cmd = &this->d3d12CopyCommands[i];
    char gpuName[64];
    snprintf(gpuName, sizeof(gpuName),
      copyFull ? "gpu wgc full bridge->ivshmem q%u" :
                 "gpu wgc dirty bridge->ivshmem q%u",
      i);
    WGCTracyD3D12Zone * gpuZone = WGC_TRACY_D3D12_ZONE_BEGIN_N(
      this->tracyD3D12CopyCtx[i], *cmd->gfxList, gpuName, strlen(gpuName));

    if (copyFull)
    {
      if (activeQueues == 1)
      {
        ID3D12GraphicsCommandList_CopyTextureRegion(*cmd->gfxList,
          &dstLoc, 0, 0, 0, &srcLoc, NULL);
      }
      else
      {
        const UINT y0 = (UINT)(((uint64_t)dst->format.Height * i) /
          activeQueues);
        const UINT y1 = (UINT)(((uint64_t)dst->format.Height * (i + 1)) /
          activeQueues);
        const D3D12_BOX box =
        {
          .left   = 0,
          .top    = y0,
          .front  = 0,
          .right  = dst->format.Width,
          .bottom = y1,
          .back   = 1
        };
        ID3D12GraphicsCommandList_CopyTextureRegion(*cmd->gfxList,
          &dstLoc, 0, y0, 0, &srcLoc, &box);
      }
    }
    else
    {
      for(const RECT * rect = copyRects;
          rect < copyRects + copyRectCount; ++rect)
      {
        const D3D12_BOX box =
        {
          .left   = (UINT)rect->left,
          .top    = (UINT)rect->top,
          .front  = 0,
          .right  = (UINT)rect->right,
          .bottom = (UINT)rect->bottom,
          .back   = 1
        };
        ID3D12GraphicsCommandList_CopyTextureRegion(*cmd->gfxList,
          &dstLoc, box.left, box.top, 0, &srcLoc, &box);
      }
    }

    WGC_TRACY_D3D12_ZONE_END(gpuZone);
  }

  bool executed = true;
  LG_PROFILE_ZONE_BEGIN(zoneD3D12Submit, "wgc d3d12 copy submit");
  const uint64_t d3d12SubmitStartUs = microtime();
  for(unsigned i = 0; i < activeQueues; ++i)
  {
    executed &= d12_commandGroupExecute(this->d3d12CopyQueues[i],
      &this->d3d12CopyCommands[i]);
    dst->d3d12CopyFenceValue[i] = this->d3d12CopyCommands[i].fenceValue;
  }
  const uint64_t d3d12SubmitUs = microtime() - d3d12SubmitStartUs;
  LG_PROFILE_ZONE_END(zoneD3D12Submit);
  if (this->debugStats)
  {
    InterlockedExchangeAdd64(&this->d3d12CopySubmitTotalUs,
      (LONG64)d3d12SubmitUs);
    InterlockedIncrement64(&this->d3d12CopySubmitCount);
    wgc_interlockedMax64(&this->d3d12CopySubmitMaxUs, (LONG64)d3d12SubmitUs);
    wgc_recordSample(&this->d3d12CopySubmitSampleCount,
      &this->d3d12CopySubmitSampleOverflow, this->d3d12CopySubmitSamples,
      (LONG64)d3d12SubmitUs);
  }
  if (!executed)
  {
    memset(dst->d3d12CopyFenceValue, 0, sizeof(dst->d3d12CopyFenceValue));
    dst->d3d12CopyQueueCount = 0;
    return false;
  }

  dst->d3d12CopyQueueCount = activeQueues;
  for(unsigned i = 0; i < activeQueues; ++i)
  {
    WGC_TRACY_D3D12_NEW_FRAME(this->tracyD3D12CopyCtx[i]);
    WGC_TRACY_D3D12_COLLECT(this->tracyD3D12CopyCtx[i]);
  }
  return true;
}

static void wgc_waitFrameD3D12Copy(WGCInstance * this, WGCFrameInfo * frame)
{
  const unsigned count = min(frame->d3d12CopyQueueCount,
    WGC_D3D12_COPY_QUEUE_MAX);
  if (!count)
    return;

  LG_PROFILE_ZONE_BEGIN(zoneD3D12Wait, "wgc d3d12 fence wait");
  const uint64_t fenceWaitStartUs = microtime();
  for(unsigned i = 0; i < count; ++i)
  {
    const UINT64 fenceValue = frame->d3d12CopyFenceValue[i];
    if (!fenceValue || !this->d3d12CopyCommandReady[i])
      continue;

    D12CommandGroup * cmd = &this->d3d12CopyCommands[i];
    if (ID3D12Fence_GetCompletedValue(*cmd->fence) >= fenceValue)
      continue;

    ID3D12Fence_SetEventOnCompletion(*cmd->fence, fenceValue, cmd->event);
    WaitForSingleObject(cmd->event, INFINITE);
  }
  const uint64_t fenceWaitUs = microtime() - fenceWaitStartUs;
  LG_PROFILE_ZONE_END(zoneD3D12Wait);

  if (this->debugStats)
  {
    InterlockedExchangeAdd64(&this->d3d12FenceWaitTotalUs,
      (LONG64)fenceWaitUs);
    InterlockedIncrement64(&this->d3d12FenceWaitCount);
    wgc_interlockedMax64(&this->d3d12FenceWaitMaxUs, (LONG64)fenceWaitUs);
    wgc_recordSample(&this->d3d12FenceWaitSampleCount,
      &this->d3d12FenceWaitSampleOverflow, this->d3d12FenceWaitSamples,
      (LONG64)fenceWaitUs);
  }

  memset(frame->d3d12CopyFenceValue, 0, sizeof(frame->d3d12CopyFenceValue));
  frame->d3d12CopyQueueCount = 0;
}

static uint64_t wgc_copyFramePixels(const WGCFrameInfo * frame)
{
  if (frame->fullCopy)
    return (uint64_t)frame->format.Width * frame->format.Height;

  uint64_t pixels = 0;
  for(const RECT * rect = frame->dirtyRects;
      rect < frame->dirtyRects + frame->nbDirtyRects; ++rect)
  {
    const LONG width  = rect->right  - rect->left;
    const LONG height = rect->bottom - rect->top;
    if (width > 0 && height > 0)
      pixels += (uint64_t)width * height;
  }
  return pixels;
}

static void wgc_recordCopyStats(WGCInstance * this, const WGCFrameInfo * frame,
  bool publishCopy, uint64_t pixels)
{
  if (!this->debugStats)
    return;

  volatile LONG64 * fullCounter  = publishCopy ?
    &this->copyPublishFull : &this->copyAccumFull;
  volatile LONG64 * dirtyCounter = publishCopy ?
    &this->copyPublishDirty : &this->copyAccumDirty;
  volatile LONG64 * pixelCounter = publishCopy ?
    &this->copyPublishPixels : &this->copyAccumPixels;

  const uint64_t framePixels =
    (uint64_t)frame->format.Width * frame->format.Height;
  const bool fullCopy = frame->fullCopy ||
    (publishCopy && this->d3d12FullCopyAlways) ||
    (publishCopy && this->dirtyFullCopyPercent > 0 &&
     frame->nbDirtyRects > 0 &&
     pixels * 100 >= framePixels * (uint64_t)this->dirtyFullCopyPercent);

  InterlockedIncrement64(fullCopy ? fullCounter : dirtyCounter);
  InterlockedExchangeAdd64(pixelCounter,
    (LONG64)(fullCopy ? framePixels : pixels));
}

// IVSHMEM-direct: create a ROW_MAJOR TEXTURE2D placed in the IVSHMEM heap
// at the slot's offset, then wrap it as a D3D11 texture via D3D11On12 so
// WGC's D3D11 CopyResource can target it. The wrapped texture is stored on
// frame->texture; the placed D3D12 resource on frame->ivshmemD12Res so we
// can release it when the slot is torn down.
static bool wgc_ensureFrameIvshmemDirect(WGCInstance * this, WGCFrameInfo * frame,
  const D3D11_TEXTURE2D_DESC * srcDesc)
{
  const unsigned frameIndex = (unsigned)(frame - this->frames);

  if (!this->ivshmemEnvReady)
  {
    DEBUG_ERROR("wgc_ensureFrameIvshmemDirect called but ivshmem environment "
      "not set up (call wgc_setIvshmemEnv first)");
    return false;
  }

  if (!frame->ivshmemSlotReady)
  {
    // The caller (top-level interface) hasn't registered this slot's offset
    // yet. Caller is expected to call wgc_setIvshmemSlot during the first
    // iface->capture(idx, ...) for each frameBufferIndex, before WGC tries
    // to publish into the slot.
    DEBUG_ERROR("wgc_ensureFrameIvshmemDirect: slot offset not registered");
    return false;
  }

  const bool nv12Packed = wgc_isNV12PackedIvshmem(this);
  if (srcDesc->Width  != this->ivshmemWidth ||
      srcDesc->Height != this->ivshmemHeight ||
      (!nv12Packed && srcDesc->Format != this->ivshmemFormat))
  {
    DEBUG_ERROR("WGC source (%ux%u fmt 0x%x) does not match the IVSHMEM-direct "
      "target (%ux%u fmt 0x%x). Cannot recover without renegotiating the "
      "slot offsets — fall back to a different publishMode for now.",
      srcDesc->Width, srcDesc->Height, srcDesc->Format,
      this->ivshmemWidth, this->ivshmemHeight, this->ivshmemFormat);
    return false;
  }

  const bool d3d12Copy =
    this->publishMode == WGC_PUBLISH_IVSHMEM_D3D12_COPY;

  comRef_scopePush(7);
  comRef_defineLocal(ID3D12Resource , placed );
  comRef_defineLocal(ID3D11Texture2D, wrapped);
  comRef_defineLocal(ID3D11Texture2D, bridgeA);
  comRef_defineLocal(ID3D11Texture2D, bridgeB);
  comRef_defineLocal(ID3D12Resource , bridge12);
  comRef_defineLocal(ID3D11UnorderedAccessView, encodeUav);

  D3D12_RESOURCE_DESC d12Desc =
  {
    .Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Alignment          = 0,
    .Width              = nv12Packed ?
      wgc_nv12EncodedWidth(this->ivshmemWidth) : this->ivshmemWidth,
    .Height             = nv12Packed ?
      wgc_nv12StorageHeight(this->ivshmemHeight) : this->ivshmemHeight,
    .DepthOrArraySize   = 1,
    .MipLevels          = 1,
    .Format             = nv12Packed ? DXGI_FORMAT_R8G8B8A8_UNORM :
      srcDesc->Format,
    .SampleDesc         = { .Count = 1, .Quality = 0 },
    .Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags              = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER
  };

  HRESULT hr = ID3D12Device3_CreatePlacedResource(
    *this->d12device,
    this->ivshmemHeap,
    frame->ivshmemOffset,
    &d12Desc,
    D3D12_RESOURCE_STATE_COMMON,
    NULL,
    &IID_ID3D12Resource,
    (void **)placed);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("CreatePlacedResource (ivshmem-direct) failed", hr);
    comRef_scopePop();
    return false;
  }
  wgc_setD3D12ObjectNameI((ID3D12Object *)*placed,
    "WGC IVSHMEM placed frame slot ", frameIndex);

  if (!d3d12Copy)
  {
    D3D11_RESOURCE_FLAGS wrapFlags =
    {
      .BindFlags           = D3D11_BIND_SHADER_RESOURCE,
      .MiscFlags           = 0,
      .CPUAccessFlags      = 0,
      .StructureByteStride = 0
    };

    hr = ID3D11On12Device_CreateWrappedResource(
      this->d11on12Device,
      (IUnknown *)*placed,
      &wrapFlags,
      D3D12_RESOURCE_STATE_COMMON,
      D3D12_RESOURCE_STATE_COMMON,
      &IID_ID3D11Texture2D,
      (void **)wrapped);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("D3D11On12 CreateWrappedResource (ivshmem-direct) failed", hr);
      comRef_scopePop();
      return false;
    }
  }

  // Two-device path: the WGC frame pool's vanilla D3D11 device cannot write
  // to a D3D11On12-wrapped texture (different device). Stage the data through
  // a shared bridge texture: WGC source → bridgeA (WGC device) → fence →
  // bridgeB (D3D11On12 device) → wrapped (IVSHMEM). bridgeA and bridgeB are
  // two D3D11 handles backed by the same VRAM via a shared NT handle.
  if (this->twoDeviceBridge || d3d12Copy)
  {
    D3D11_TEXTURE2D_DESC bridgeDesc =
    {
      .Width          = (UINT)d12Desc.Width,
      .Height         = d12Desc.Height,
      .MipLevels      = 1,
      .ArraySize      = 1,
      .Format         = d12Desc.Format,
      .SampleDesc     = { .Count = 1, .Quality = 0 },
      .Usage          = D3D11_USAGE_DEFAULT,
      .BindFlags      = D3D11_BIND_SHADER_RESOURCE |
                        (nv12Packed ? D3D11_BIND_UNORDERED_ACCESS : 0),
      .CPUAccessFlags = 0,
      .MiscFlags      = D3D11_RESOURCE_MISC_SHARED |
                        D3D11_RESOURCE_MISC_SHARED_NTHANDLE
    };

    hr = ID3D11Device5_CreateTexture2D(*this->device, &bridgeDesc, NULL,
      bridgeA);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-direct: CreateTexture2D bridgeA failed", hr);
      comRef_scopePop();
      return false;
    }

    if (nv12Packed)
    {
      D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc =
      {
        .Format        = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
        .Texture2D     = { .MipSlice = 0 }
      };
      hr = ID3D11Device5_CreateUnorderedAccessView(*this->device,
        (ID3D11Resource *)*bridgeA, &uavDesc, encodeUav);
      if (FAILED(hr))
      {
        DEBUG_WINERROR("ivshmem-d3d12-copy: CreateUnorderedAccessView NV12 "
          "bridge failed", hr);
        comRef_scopePop();
        return false;
      }
    }

    comRef_defineLocal(IDXGIResource1, bridgeRes);
    hr = ID3D11Texture2D_QueryInterface(*bridgeA, &IID_IDXGIResource1,
      (void **)bridgeRes);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-direct: bridgeA QI IDXGIResource1 failed", hr);
      comRef_scopePop();
      return false;
    }

    HANDLE bridgeHandle = NULL;
    hr = IDXGIResource1_CreateSharedHandle(*bridgeRes, NULL, GENERIC_ALL, NULL,
      &bridgeHandle);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-direct: bridgeA CreateSharedHandle failed", hr);
      comRef_scopePop();
      return false;
    }

    if (d3d12Copy)
    {
      hr = ID3D12Device3_OpenSharedHandle(*this->d12device, bridgeHandle,
        &IID_ID3D12Resource, (void **)bridge12);
      CloseHandle(bridgeHandle);
      if (FAILED(hr))
      {
        DEBUG_WINERROR("ivshmem-d3d12-copy: OpenSharedHandle bridge failed", hr);
        comRef_scopePop();
        return false;
      }
      wgc_setD3D12ObjectNameI((ID3D12Object *)*bridge12,
        "WGC shared bridge texture ", frameIndex);
    }
    else
    {
      hr = ID3D11Device5_OpenSharedResource1(*this->on12Device, bridgeHandle,
        &IID_ID3D11Texture2D, (void **)bridgeB);
      CloseHandle(bridgeHandle);
      if (FAILED(hr))
      {
        DEBUG_WINERROR("ivshmem-direct: OpenSharedResource1 bridgeB failed", hr);
        comRef_scopePop();
        return false;
      }
    }
  }

  comRef_toGlobal(frame->ivshmemD12Res, placed );
  if (!d3d12Copy)
    comRef_toGlobal(frame->texture, wrapped);
  if (this->twoDeviceBridge)
  {
    comRef_toGlobal(frame->bridgeA, bridgeA);
    comRef_toGlobal(frame->bridgeB, bridgeB);
  }
  if (d3d12Copy)
  {
    comRef_toGlobal(frame->bridgeA , bridgeA );
    comRef_toGlobal(frame->bridge12, bridge12);
    if (nv12Packed)
      comRef_toGlobal(frame->encodeUav, encodeUav);
  }
  memcpy(&frame->format, srcDesc, sizeof(frame->format));
  if (nv12Packed)
  {
    frame->format.Width  = wgc_nv12EncodedWidth(this->ivshmemWidth);
    frame->format.Height = wgc_nv12StorageHeight(this->ivshmemHeight);
    frame->format.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  }
  frame->fenceValue = 0;
  frame->ready      = true;
  comRef_scopePop();
  return true;
}

static bool wgc_ensureFrame(WGCInstance * this, WGCFrameInfo * frame,
  ID3D11Texture2D * src)
{
  D3D11_TEXTURE2D_DESC srcDesc;
  ID3D11Texture2D_GetDesc(src, &srcDesc);
  const bool ivshmemDirectSlot =
    wgc_isIvshmemPublishMode(this->publishMode) &&
    frame != &this->frames[0];
  const bool nv12PackedSlot = ivshmemDirectSlot &&
    wgc_isNV12PackedIvshmem(this);

  if (frame->ready && (
      (!nv12PackedSlot &&
       frame->format.Width  == srcDesc.Width  &&
       frame->format.Height == srcDesc.Height &&
       frame->format.Format == srcDesc.Format) ||
      (nv12PackedSlot &&
       frame->format.Width  == wgc_nv12EncodedWidth(this->ivshmemWidth) &&
       frame->format.Height == wgc_nv12StorageHeight(this->ivshmemHeight) &&
       frame->format.Format == DXGI_FORMAT_R8G8B8A8_UNORM)))
    return true;

  frame->ready = false;

  if (frame->texture)
  {
    comRef_release(frame->texture);
    comRef_release(frame->d12Res);
    comRef_release(frame->fence);
    comRef_release(frame->d12Fence);
    comRef_release(frame->ivshmemD12Res);
    comRef_release(frame->bridgeA);
    comRef_release(frame->bridgeB);
    comRef_release(frame->bridge12);
    comRef_release(frame->encodeUav);
    memset(&frame->format, 0, sizeof(frame->format));
    frame->fenceValue = 0;
    frame->copiedOnce = false;
  }

  // IVSHMEM-direct path: only publish slots (not the accumulator) live in
  // IVSHMEM. Accumulator stays VRAM-local in this mode.
  if (ivshmemDirectSlot)
    return wgc_ensureFrameIvshmemDirect(this, frame, &srcDesc);

  D3D11_TEXTURE2D_DESC dstDesc =
  {
    .Width          = srcDesc.Width,
    .Height         = srcDesc.Height,
    .MipLevels      = 1,
    .ArraySize      = 1,
    .Format         = srcDesc.Format,
    .SampleDesc     = { .Count = 1, .Quality = 0 }
  };

  const bool cpuPublishSlot = this->publishMode == WGC_PUBLISH_CPU_STAGING &&
    frame != &this->frames[0];

  if (cpuPublishSlot)
  {
    // CPU-mappable staging: D3D11 Map on USAGE_STAGING intrinsically waits
    // for outstanding GPU work, so no separate fence is needed.
    dstDesc.Usage          = D3D11_USAGE_STAGING;
    dstDesc.BindFlags      = 0;
    dstDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dstDesc.MiscFlags      = 0;
  }
  else if (this->publishMode == WGC_PUBLISH_D12_SHARE)
  {
    // D12 share: SHARED+SHARED_NTHANDLE so the texture can be opened as a
    // D3D12 resource via NT handle in wgc_shareFrame.
    dstDesc.Usage          = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
    dstDesc.CPUAccessFlags = 0;
    dstDesc.MiscFlags      = D3D11_RESOURCE_MISC_SHARED |
                             D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  }
  else
  {
    // CPU-staging accumulator: GPU-local copy-only texture.
    dstDesc.Usage          = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags      = wgc_isNV12PackedIvshmem(this) ?
      D3D11_BIND_SHADER_RESOURCE : 0;
    dstDesc.CPUAccessFlags = 0;
    dstDesc.MiscFlags      = 0;
  }

  comRef_scopePush(1);
  comRef_defineLocal(ID3D11Texture2D, texture);
  HRESULT hr = ID3D11Device5_CreateTexture2D(
    *this->device, &dstDesc, NULL, texture);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the WGC slot texture", hr);
    comRef_scopePop();
    return false;
  }

  comRef_toGlobal(frame->texture, texture);
  memcpy(&frame->format, &srcDesc, sizeof(frame->format));
  comRef_scopePop();

  if (cpuPublishSlot)
  {
    frame->fenceValue = 0;
    frame->ready      = true;
    return true;
  }

  if (this->publishMode == WGC_PUBLISH_CPU_STAGING)
  {
    // The accumulator is never mapped by the CPU. Keep it GPU-local so the
    // WGC source -> accumulator damage copy does not write into staging memory;
    // only the final publish slot is staging/readback.
    frame->fenceValue = 0;
    frame->ready      = true;
    return true;
  }

  if (wgc_isIvshmemPublishMode(this->publishMode))
  {
    // The accumulator is a normal GPU-local D3D11 texture. Only publish slots
    // are D3D11On12-wrapped ROW_MAJOR resources placed in IVSHMEM.
    frame->fenceValue = 0;
    frame->ready      = true;
    return true;
  }

  return wgc_shareFrame(this, frame);
}

static bool wgc_ensureAllFrames(WGCInstance * this, ID3D11Texture2D * src)
{
  for(unsigned i = 1; i < this->frameCount; ++i)
  {
    if (wgc_isIvshmemPublishMode(this->publishMode) &&
        !this->frames[i].ivshmemSlotReady)
      continue;

    if (!wgc_ensureFrame(this, &this->frames[i], src))
      return false;
  }

  return true;
}

static bool wgc_shareFrame(WGCInstance * this, WGCFrameInfo * frame)
{
  bool result = false;
  HRESULT hr;
  comRef_scopePush(8);

  comRef_defineLocal(IDXGIResource1, dxgiRes);
  hr = ID3D11Texture2D_QueryInterface(
    *frame->texture, &IID_IDXGIResource1, (void **)dxgiRes);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to obtain the shared ID3D11Resource1 interface", hr);
    goto exit;
  }

  HANDLE sharedHandle;
  hr = IDXGIResource1_CreateSharedHandle(
    *dxgiRes, NULL, DXGI_SHARED_RESOURCE_READ, NULL, &sharedHandle);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the WGC shared handle", hr);
    goto exit;
  }

  comRef_defineLocal(ID3D12Resource, d12Res);
  hr = ID3D12Device3_OpenSharedHandle(
    *this->d12device, sharedHandle, &IID_ID3D12Resource, (void **)d12Res);
  CloseHandle(sharedHandle);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to open the WGC D3D12 resource", hr);
    goto exit;
  }
  wgc_setD3D12ObjectName((ID3D12Object *)*d12Res,
    "WGC shared accumulator texture");

  comRef_defineLocal(ID3D11Fence, fence);
  hr = ID3D11Device5_CreateFence(
    *this->device, 0, D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, (void **)fence);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the WGC fence", hr);
    goto exit;
  }

  hr = ID3D11Fence_CreateSharedHandle(
    *fence, NULL, GENERIC_ALL, NULL, &sharedHandle);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the WGC fence shared handle", hr);
    goto exit;
  }

  comRef_defineLocal(ID3D12Fence, d12Fence);
  hr = ID3D12Device3_OpenSharedHandle(
    *this->d12device, sharedHandle, &IID_ID3D12Fence, (void **)d12Fence);
  CloseHandle(sharedHandle);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to open the WGC D3D12 fence", hr);
    goto exit;
  }
  wgc_setD3D12ObjectName((ID3D12Object *)*d12Fence,
    "WGC shared accumulator fence");

  comRef_toGlobal(frame->d12Res  , d12Res  );
  comRef_toGlobal(frame->fence   , fence   );
  comRef_toGlobal(frame->d12Fence, d12Fence);
  frame->fenceValue = 0;
  frame->ready      = true;
  result = true;

exit:
  comRef_scopePop();
  return result;
}

static void wgc_updatePointer(WGCInstance * this, int x, int y)
{
  if (this->cursorLockCreated)
    EnterCriticalSection(&this->cursorLock);

  CURSORINFO info =
  {
    .cbSize = sizeof(info)
  };

  if (!GetCursorInfo(&info))
  {
    if (this->cursorLockCreated)
      LeaveCriticalSection(&this->cursorLock);
    return;
  }

  const bool visible = (info.flags & CURSOR_SHOWING) != 0;
  if (x == INT_MIN || y == INT_MIN)
  {
    x = info.ptScreenPos.x;
    y = info.ptScreenPos.y;
  }

  LG_PROFILE_ZONE_BEGIN(zonePointer, "wgc update pointer");
  CapturePointer pointer = {0};
  bool changed = false;

  if (!this->lastCursorValid ||
      visible != this->lastCursorVisible ||
      x != this->lastCursorX ||
      y != this->lastCursorY)
  {
    pointer.positionUpdate = visible;
    pointer.visible        = visible;
    pointer.x              = x - this->outputRect.left - this->cursorHotX;
    pointer.y              = y - this->outputRect.top  - this->cursorHotY;
    changed                = true;
  }

  if (info.hCursor && info.hCursor != this->lastCursor)
    if (wgc_updatePointerShape(this, &pointer, info.hCursor))
    {
      if (pointer.positionUpdate)
      {
        pointer.x = x - this->outputRect.left - this->cursorHotX;
        pointer.y = y - this->outputRect.top  - this->cursorHotY;
      }
      changed = true;
    }

  if (changed)
  {
    const bool positionOnly = pointer.positionUpdate && !pointer.shapeUpdate &&
      this->lastCursorValid && visible == this->lastCursorVisible;
    if (positionOnly && this->cursorMaxHz > 0 && this->cursorLastPostUs)
    {
      const uint64_t now = microtime();
      const uint64_t intervalUs = 1000000ULL / this->cursorMaxHz;
      if (now - this->cursorLastPostUs < intervalUs)
      {
        LG_PROFILE_ZONE_END(zonePointer);
        if (this->cursorLockCreated)
          LeaveCriticalSection(&this->cursorLock);
        return;
      }
      this->cursorLastPostUs = now;
    }
    else
      this->cursorLastPostUs = microtime();

    if (this->postPointerBufferFn)
    {
      if (pointer.shapeUpdate && this->getPointerBufferFn)
      {
        void * dst;
        UINT   dstSize;
        if (!this->getPointerBufferFn(&dst, &dstSize))
        {
          DEBUG_ERROR("Failed to obtain a buffer for the pointer shape");
          pointer.shapeUpdate = false;
        }
        else
        {
          const size_t copySize = min(dstSize, this->cursorShapeDataSize);
          memcpy(dst, this->cursorShape, copySize);
        }
      }
      this->postPointerBufferFn(&pointer);
    }
    else
      d12_updatePointer(&pointer, this->cursorShape,
        this->cursorShapeDataSize);
  }

  this->lastCursorValid   = true;
  this->lastCursorVisible = visible;
  this->lastCursorX       = x;
  this->lastCursorY       = y;
  this->lastCursor        = info.hCursor;
  LG_PROFILE_ZONE_END(zonePointer);
  if (this->cursorLockCreated)
    LeaveCriticalSection(&this->cursorLock);
}

static void wgc_onMouseMove(int x, int y)
{
  WGCInstance * this = wgcCursorInstance;
  if (!this)
    return;

  InterlockedExchange64(&this->cursorPendingPos, wgc_packCursorPos(x, y));
  wgc_updatePointer(this, x, y);
}

static LONG64 wgc_packCursorPos(int x, int y)
{
  return ((LONG64)(DWORD)x << 32) | (DWORD)y;
}

static void wgc_unpackCursorPos(LONG64 packed, int * x, int * y)
{
  *x = (LONG)(packed >> 32);
  *y = (LONG)packed;
}

static bool wgc_updatePointerShape(WGCInstance * this,
  CapturePointer * pointer, HCURSOR cursor)
{
  ICONINFO iconInfo;
  if (!GetIconInfo(cursor, &iconInfo))
    return false;

  BITMAP bitmap = {0};
  HBITMAP sizeBitmap = iconInfo.hbmColor ?
    iconInfo.hbmColor : iconInfo.hbmMask;
  if (!sizeBitmap || !GetObject(sizeBitmap, sizeof(bitmap), &bitmap))
    goto fail;

  const unsigned width  = bitmap.bmWidth;
  const unsigned height = iconInfo.hbmColor ?
    bitmap.bmHeight : bitmap.bmHeight / 2;
  const unsigned pitch  = width * 4;
  const size_t shapeSize = (size_t)pitch * height;
  if (!width || !height || shapeSize == 0)
    goto fail;

  if (width > 512 || height > 512 || shapeSize > WGC_CURSOR_MAX_SIZE)
  {
    DEBUG_WARN("Ignoring oversized WGC cursor: %ux%u pitch:%u size:%llu",
      width, height, pitch, (unsigned long long)shapeSize);
    goto fail;
  }

  if (this->cursorShapeSize < shapeSize)
  {
    void * shape = realloc(this->cursorShape, shapeSize);
    if (!shape)
    {
      DEBUG_ERROR("out of memory");
      goto fail;
    }

    this->cursorShape     = shape;
    this->cursorShapeSize = shapeSize;
  }

  BITMAPINFO bmi =
  {
    .bmiHeader =
    {
      .biSize        = sizeof(bmi.bmiHeader),
      .biWidth       = width,
      .biHeight      = -(LONG)height,
      .biPlanes      = 1,
      .biBitCount    = 32,
      .biCompression = BI_RGB
    }
  };

  void * bits = NULL;
  HDC dc = GetDC(NULL);
  HDC memDC = dc ? CreateCompatibleDC(dc) : NULL;
  HBITMAP dib = memDC ?
    CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
  HGDIOBJ oldBitmap = dib ? SelectObject(memDC, dib) : NULL;

  if (!dib || !bits || !DrawIconEx(memDC, 0, 0, cursor, width, height, 0,
      NULL, DI_NORMAL))
  {
    if (oldBitmap)
      SelectObject(memDC, oldBitmap);
    if (dib)
      DeleteObject(dib);
    if (memDC)
      DeleteDC(memDC);
    if (dc)
      ReleaseDC(NULL, dc);
    goto fail;
  }

  const uint64_t hash = wgc_hashCursorShape(bits, shapeSize,
    width, height, pitch, iconInfo.xHotspot, iconInfo.yHotspot);
  if (this->cursorShapeValid && this->cursorShapeHash == hash)
  {
    this->cursorHotX = iconInfo.xHotspot;
    this->cursorHotY = iconInfo.yHotspot;

    if (oldBitmap)
      SelectObject(memDC, oldBitmap);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(NULL, dc);
    DeleteObject(iconInfo.hbmColor);
    DeleteObject(iconInfo.hbmMask);
    return false;
  }

  memcpy(this->cursorShape, bits, shapeSize);

  if (oldBitmap)
    SelectObject(memDC, oldBitmap);
  DeleteObject(dib);
  DeleteDC(memDC);
  ReleaseDC(NULL, dc);

  pointer->shapeUpdate = true;
  pointer->format      = CAPTURE_FMT_COLOR;
  pointer->hx          = iconInfo.xHotspot;
  pointer->hy          = iconInfo.yHotspot;
  pointer->width       = width;
  pointer->height      = height;
  pointer->pitch       = pitch;

  this->cursorShapeDataSize = shapeSize;
  this->cursorHotX          = iconInfo.xHotspot;
  this->cursorHotY          = iconInfo.yHotspot;
  this->cursorShapeHash     = hash;
  this->cursorShapeValid    = true;

  DeleteObject(iconInfo.hbmColor);
  DeleteObject(iconInfo.hbmMask);
  return true;

fail:
  if (iconInfo.hbmColor)
    DeleteObject(iconInfo.hbmColor);
  if (iconInfo.hbmMask)
    DeleteObject(iconInfo.hbmMask);
  return false;
}

static uint64_t wgc_hashCursorShape(const void * data, size_t size,
  unsigned width, unsigned height, unsigned pitch, unsigned hotX,
  unsigned hotY)
{
  uint64_t hash = 1469598103934665603ULL;
  const uint8_t * bytes = data;

  for(size_t i = 0; i < size; ++i)
  {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }

  const uint32_t meta[] = { width, height, pitch, hotX, hotY };
  for(size_t i = 0; i < ARRAY_LENGTH(meta); ++i)
  {
    hash ^= meta[i];
    hash *= 1099511628211ULL;
  }

  return hash;
}

static void wgc_setMinUpdateInterval(IGraphicsCaptureSession * session,
  int maxFPS)
{
  if (maxFPS <= 0)
  {
    DEBUG_INFO("WGC MinUpdateInterval: OS default");
    return;
  }

  HRESULT hr;
  comRef_scopePush(1);

  comRef_defineLocal(IGraphicsCaptureSession5, session5);
  hr = IGraphicsCaptureSession_QueryInterface(
    session, &IID_IGraphicsCaptureSession5, (void **)session5);
  if (FAILED(hr))
  {
    DEBUG_WARN("WGC MinUpdateInterval is not available, capture may be capped");
    goto exit;
  }

  TimeSpan interval =
  {
    .Duration = 10000000LL / maxFPS
  };
  hr = IGraphicsCaptureSession5_put_MinUpdateInterval(*session5, interval);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to set WGC MinUpdateInterval", hr);
    goto exit;
  }

  DEBUG_INFO("WGC MinUpdateInterval: %d fps (%lld ticks)",
    maxFPS, (long long)interval.Duration);

exit:
  comRef_scopePop();
}

static WGCCursorMode wgc_parseCursorMode(void)
{
  const char * mode = option_get_string("wgc", "cursor");
  if (!mode || strcmp(mode, "separate") == 0)
    return WGC_CURSOR_MODE_SEPARATE;

  if (strcmp(mode, "embedded") == 0)
    return WGC_CURSOR_MODE_EMBEDDED;

  if (strcmp(mode, "none") == 0)
    return WGC_CURSOR_MODE_NONE;

  DEBUG_WARN("Invalid WGC cursor mode \"%s\", using separate", mode);
  return WGC_CURSOR_MODE_SEPARATE;
}

static void wgc_closeInspectable(IInspectable * obj)
{
  IClosable * closable = NULL;
  HRESULT hr = IInspectable_QueryInterface(obj, &IID_IClosable,
    (void **)&closable);
  if (SUCCEEDED(hr))
  {
    IClosable_Close(closable);
    IClosable_Release(closable);
  }
}

static void wgc_releaseFrame(IDirect3D11CaptureFrame ** frame)
{
  if (!frame || !*frame)
    return;

  wgc_closeInspectable((IInspectable *)*frame);
  IDirect3D11CaptureFrame_Release(*frame);
  *frame = NULL;
}

static bool wgc_createHString(const WCHAR * str, HSTRING * result)
{
  const HRESULT hr = WindowsCreateString(str, wcslen(str), result);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("WindowsCreateString failed", hr);
    return false;
  }

  return true;
}

// Public WGC API used by the standalone Capture_WGC interface.

bool wgc_createInstance(WGCInstance ** out, unsigned frameBuffers,
  WGCPublishMode mode)
{
  D12Backend * backend;
  if (!wgc_create(&backend, frameBuffers))
    return false;
  WGCInstance * inst = UPCAST(WGCInstance, backend);
  inst->publishMode = mode;
  *out = inst;
  return true;
}

bool wgc_initInstance(WGCInstance * this, bool debug,
  IDXGIAdapter1 * adapter, IDXGIOutput * output, bool trackDamage)
{
  this->base.trackDamage = trackDamage;
  return wgc_init(&this->base, debug, NULL, adapter, output);
}

void wgc_setPointerCallbacks(WGCInstance * this,
  CaptureGetPointerBuffer  getFn,
  CapturePostPointerBuffer postFn)
{
  this->getPointerBufferFn  = getFn;
  this->postPointerBufferFn = postFn;
}

void wgc_setLoanedDevices(WGCInstance * this,
  IUnknown * d11Device,
  IUnknown * d11Context,
  IUnknown * d12Device)
{
  if (!this)
    return;
  this->loanedD11Device  = (ID3D11Device        *)d11Device;
  this->loanedD11Context = (ID3D11DeviceContext *)d11Context;
  this->loanedD12Device  = (ID3D12Device3       *)d12Device;
}

bool wgc_setIvshmemEnv(WGCInstance * this,
  IUnknown * ivshmemHeap,
  IUnknown * d11on12Device,
  unsigned   width,
  unsigned   height,
  unsigned   format)
{
  if (!this || !ivshmemHeap || !d11on12Device)
    return false;

  this->ivshmemHeap      = (ID3D12Heap *)ivshmemHeap;
  this->d11on12Device    = (ID3D11On12Device *)d11on12Device;
  this->ivshmemWidth     = width;
  this->ivshmemHeight    = height;
  this->ivshmemFormat    = (DXGI_FORMAT)format;
  this->ivshmemEnvReady  = true;
  this->publishMode      = WGC_PUBLISH_IVSHMEM_DIRECT;

  DEBUG_INFO("wgc_setIvshmemEnv: %ux%u, format 0x%x", width, height,
    (unsigned)this->ivshmemFormat);
  return true;
}

bool wgc_setIvshmemD3D12CopyEnv(WGCInstance * this,
  IUnknown * ivshmemHeap,
  IUnknown * d3d12Queue,
  unsigned   width,
  unsigned   height,
  unsigned   format)
{
  if (!this || !ivshmemHeap || !d3d12Queue)
    return false;

  this->ivshmemHeap        = (ID3D12Heap *)ivshmemHeap;
  this->d3d12CopyQueues[0] = (ID3D12CommandQueue *)d3d12Queue;
  ID3D12CommandQueue_AddRef(this->d3d12CopyQueues[0]);
  this->ivshmemWidth       = width;
  this->ivshmemHeight      = height;
  this->ivshmemFormat      = (DXGI_FORMAT)format;
  this->ivshmemEnvReady    = true;
  this->publishMode        = WGC_PUBLISH_IVSHMEM_D3D12_COPY;

  DEBUG_INFO("wgc_setIvshmemD3D12CopyEnv: %ux%u, format 0x%x",
    width, height, (unsigned)this->ivshmemFormat);
  return true;
}

bool wgc_setIvshmemSlot(WGCInstance * this,
  unsigned slotIndex,
  uint64_t offset,
  uint64_t size)
{
  // slotIndex is 0-based into the publish slots (frames[1..N-1]).
  if (!this || slotIndex + 1 >= this->frameCount)
  {
    DEBUG_ERROR("wgc_setIvshmemSlot: slotIndex %u out of range (publish "
      "slots: %u)", slotIndex, this->frameCount - 1);
    return false;
  }

  WGCFrameInfo * frame = &this->frames[slotIndex + 1];
  if (frame->ivshmemSlotReady && frame->ivshmemOffset != offset)
  {
    // Offset changed for an already-ready slot. Drop the wrapped texture +
    // bridges so wgc_ensureFrame recreates them at the new offset.
    if (frame->texture || frame->ivshmemD12Res ||
        frame->bridgeA || frame->bridgeB || frame->bridge12)
    {
      comRef_release(frame->texture);
      comRef_release(frame->ivshmemD12Res);
      comRef_release(frame->bridgeA);
      comRef_release(frame->bridgeB);
      comRef_release(frame->bridge12);
      comRef_release(frame->encodeUav);
      memset(&frame->format, 0, sizeof(frame->format));
      frame->ready      = false;
      frame->copiedOnce = false;
    }
  }

  frame->ivshmemOffset    = offset;
  frame->ivshmemSlotReady = true;
  // The largest slot size we expect drives the heap-test sanity-check path.
  if (size > this->ivshmemSlotSize)
    this->ivshmemSlotSize = size;
  return true;
}

bool wgc_deinitInstance(WGCInstance * this)
{
  return wgc_deinit(&this->base);
}

void wgc_freeInstance(WGCInstance ** this)
{
  if (!this || !*this)
    return;
  D12Backend * backend = &(*this)->base;
  wgc_free(&backend);
  *this = NULL;
}

CaptureResult wgc_pollFrame(WGCInstance * this, unsigned frameBufferIndex)
{
  return wgc_capture(&this->base, frameBufferIndex);
}

bool wgc_fetchCpu(WGCInstance * this, unsigned frameBufferIndex,
  WGCFrameDesc * desc, void ** map, unsigned * pitch,
  unsigned * width, unsigned * height)
{
  (void)frameBufferIndex; // current/consumerFrame are global to the instance

  WGCFrameInfo * frame = InterlockedCompareExchangePointer(
    (PVOID volatile *)&this->current, NULL, NULL);

  if (!frame)
    return false;

  const LONG state = InterlockedCompareExchange(&frame->state,
    WGC_FRAME_CONSUMING, WGC_FRAME_READY);
  if (state == WGC_FRAME_READY)
    InterlockedExchangePointer((PVOID volatile *)&this->consumerFrame, frame);
  else if (state != WGC_FRAME_CONSUMING ||
      InterlockedCompareExchangePointer(
        (PVOID volatile *)&this->consumerFrame, NULL, NULL) != frame)
    return false;

  if (state == WGC_FRAME_READY && this->asyncCapture && this->handler)
    InterlockedIncrement(&this->handler->framesConsumed);

  // D3D11 Map on USAGE_STAGING blocks for outstanding GPU work — no fence
  // needed. This also handles the GPU/CPU sync for the prior CopyResource
  // issued in wgc_processFrame.
  D3D11_MAPPED_SUBRESOURCE mapped;
  LG_PROFILE_ZONE_BEGIN(zoneMap, "wgc map staging texture");
  HRESULT hr = ID3D11DeviceContext4_Map(*this->context,
    (ID3D11Resource *)*frame->texture, 0, D3D11_MAP_READ, 0, &mapped);
  LG_PROFILE_ZONE_END(zoneMap);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to map WGC staging texture", hr);
    InterlockedCompareExchange(&frame->state,
      WGC_FRAME_FREE, WGC_FRAME_CONSUMING);
    return false;
  }

  *map    = mapped.pData;
  *pitch  = mapped.RowPitch;
  *width  = frame->format.Width;
  *height = frame->format.Height;

  desc->dirtyRects            = frame->dirtyRects;
  desc->nbDirtyRects          = frame->nbDirtyRects;
  desc->rotation              = CAPTURE_ROT_0;
  desc->colorSpace            = this->colorSpace;
  desc->hasSystemRelativeTime = frame->hasSystemRelativeTime;
  desc->systemRelativeTime    = frame->systemRelativeTime;
  desc->hasBackendFrameTime   = frame->callbackTimeUs != 0;
  desc->backendFrameTimeUs    = frame->callbackTimeUs;
  desc->hasWgcStats           = this->handler != NULL;
  desc->wgcEventCallbacks     = this->handler ?
    InterlockedCompareExchange(&this->handler->events, 0, 0) : 0;
  desc->wgcFramesPulled       = this->handler ?
    InterlockedCompareExchange(&this->handler->framesPulled, 0, 0) : 0;
  desc->wgcFramesConsumed     = this->handler ?
    InterlockedCompareExchange(&this->handler->framesConsumed, 0, 0) : 0;
  desc->fullCopy              = frame->fullCopy;
  desc->backendToken          = frame;
  return true;
}

void wgc_releaseCpu(WGCInstance * this, void * token)
{
  if (!token)
    return;

  WGCFrameInfo * frame = token;
  ID3D11DeviceContext4_Unmap(*this->context,
    (ID3D11Resource *)*frame->texture, 0);
  wgc_releaseSlot(&this->base, token);
}

// IVSHMEM-direct: WGC has already written pixels to IVSHMEM via the
// D3D11On12-wrapped texture. The consumer doesn't need to map anything —
// it just needs the metadata (dirty rects, dimensions) and to know which
// IVSHMEM slot was written.
//
// Returns the slot's ivshmem offset via *ivshmemOffset on success. width,
// height, pitch describe the dimensions and row stride. The state machine
// transitions are identical to wgc_fetchCpu — no Map/Unmap involved.
//
// Note: D3D11 CopyResource into a wrapped resource is queued on the D3D11
// immediate context; it executes asynchronously on the GPU. Before the
// CPU consumer reads the IVSHMEM bytes, we need a barrier. The
// Acquire/Release in wgc_copyFrameTexture (publish path) inserts that
// state transition; the GPU writes are visible after the
// ReleaseWrappedResources + a Flush. For correctness we Flush here too.
bool wgc_fetchIvshmemDirect(WGCInstance * this, unsigned frameBufferIndex,
  WGCFrameDesc * desc, uint64_t * ivshmemOffset,
  unsigned * pitch, unsigned * width, unsigned * height)
{
  if (frameBufferIndex >= this->frameCount - 1)
    return false;

  WGCFrameInfo * frame = &this->frames[frameBufferIndex + 1];
  if (!frame->ivshmemSlotReady)
    return false;

  const LONG state = InterlockedCompareExchange(&frame->state,
    WGC_FRAME_CONSUMING, WGC_FRAME_READY);
  if (state == WGC_FRAME_READY)
    InterlockedExchangePointer((PVOID volatile *)&this->consumerFrame, frame);
  else if (state != WGC_FRAME_CONSUMING ||
      InterlockedCompareExchangePointer(
        (PVOID volatile *)&this->consumerFrame, NULL, NULL) != frame)
    return false;

  if (state == WGC_FRAME_READY && this->asyncCapture && this->handler)
    InterlockedIncrement(&this->handler->framesConsumed);

  if (this->publishMode == WGC_PUBLISH_IVSHMEM_D3D12_COPY)
    wgc_waitFrameD3D12Copy(this, frame);

  // Force any pending GPU writes from the D3D11On12 wrapped resource to
  // commit. The Release in wgc_copyFrameTexture queued a state transition
  // back to D3D12 ownership; the Flush guarantees the GPU executes it
  // (and the prior CopyResource) before we tell the consumer the slot is
  // ready.
  ID3D11DeviceContext4_Flush(*this->context);

  *ivshmemOffset = frame->ivshmemOffset;
  // For row_major TEXTURE2D placed resources, the natural pitch is
  // width * bpp aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256). We
  // recover bpp from the format.
  const unsigned bpp = (frame->format.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    ? 8 : 4;
  *pitch  = (frame->format.Width * bpp + 255u) & ~255u;
  *width  = wgc_isNV12PackedIvshmem(this) ?
    this->ivshmemWidth : frame->format.Width;
  *height = wgc_isNV12PackedIvshmem(this) ?
    this->ivshmemHeight : frame->format.Height;

  desc->dirtyRects            = frame->dirtyRects;
  desc->nbDirtyRects          = frame->nbDirtyRects;
  desc->rotation              = CAPTURE_ROT_0;
  desc->colorSpace            = this->colorSpace;
  desc->hasSystemRelativeTime = frame->hasSystemRelativeTime;
  desc->systemRelativeTime    = frame->systemRelativeTime;
  desc->hasBackendFrameTime   = frame->callbackTimeUs != 0;
  desc->backendFrameTimeUs    = frame->callbackTimeUs;
  desc->hasWgcStats           = this->handler != NULL;
  desc->wgcEventCallbacks     = this->handler ?
    InterlockedCompareExchange(&this->handler->events, 0, 0) : 0;
  desc->wgcFramesPulled       = this->handler ?
    InterlockedCompareExchange(&this->handler->framesPulled, 0, 0) : 0;
  desc->wgcFramesConsumed     = this->handler ?
    InterlockedCompareExchange(&this->handler->framesConsumed, 0, 0) : 0;
  desc->fullCopy              = frame->fullCopy;
  desc->backendToken          = frame;
  return true;
}

void wgc_releaseIvshmemDirect(WGCInstance * this, void * token)
{
  // No Unmap — we never mapped anything. Slot state transition only.
  if (!token)
    return;
  wgc_releaseSlot(&this->base, token);
}

const D12Backend D12Backend_WGC =
{
  .name        = "Windows Graphics Capture",
  .codeName    = "WGC",
  .trackDamage = false,

  .create   = wgc_create,
  .init     = wgc_init,
  .deinit   = wgc_deinit,
  .free     = wgc_free,
  .capture  = wgc_capture,
  .sync     = wgc_sync,
  .fetch    = wgc_fetch
};
