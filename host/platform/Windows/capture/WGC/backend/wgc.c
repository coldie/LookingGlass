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
#include "wgc_stats.h"
#include "wgc_util.h"

#include "com_ref.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/array.h"
#include "common/display.h"
#include "common/option.h"
#include "windows/mousehook.h"
#include "common/time.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3dcompiler.h>

#define WGC_D3D12_COPY_QUEUE_MAX 8

// command groups per copy queue, so a new frame can record while the
// previous frame's GPU copy is still in flight
#define WGC_D3D12_COPY_GROUPS 2

// frame pool polling wait when wgc:pollFramePool is enabled
#define WGC_POLL_FRAME_POOL_MS 1
// minimum WGC callback gap before DwmFlush is used (wgc:dwmFlushOnGap)
#define WGC_DWM_FLUSH_GAP_MS 50

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Graphics
#define WIDL_using_Windows_Graphics_Capture
#define WIDL_using_Windows_Graphics_DirectX
#define WIDL_using_Windows_Graphics_DirectX_Direct3D11

#include <d3d11.h>
#include <d3d11_4.h>
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
#define IDirect3D11CaptureFrame2_get_DirtyRegionMode(This,value) \
  (This)->lpVtbl->get_DirtyRegionMode(This,value)
#endif

#ifndef IID_IGraphicsCaptureSession4
DEFINE_GUID(IID_IGraphicsCaptureSession4,
  0xae99813c, 0xc257, 0x5759, 0x8e, 0xd0, 0x66, 0x8c, 0x9b, 0x55, 0x7e, 0xd4);

typedef enum WGCGraphicsCaptureDirtyRegionMode
{
  WGC_GRAPHICS_CAPTURE_DIRTY_REGION_MODE_REPORT_ONLY       = 0,
  WGC_GRAPHICS_CAPTURE_DIRTY_REGION_MODE_REPORT_AND_RENDER = 1
}
WGCGraphicsCaptureDirtyRegionMode;

typedef interface IGraphicsCaptureSession4 IGraphicsCaptureSession4;
typedef struct IGraphicsCaptureSession4Vtbl
{
  BEGIN_INTERFACE

  HRESULT (STDMETHODCALLTYPE *QueryInterface)(
    IGraphicsCaptureSession4 * This,
    REFIID riid,
    void ** ppvObject);

  ULONG (STDMETHODCALLTYPE *AddRef)(IGraphicsCaptureSession4 * This);
  ULONG (STDMETHODCALLTYPE *Release)(IGraphicsCaptureSession4 * This);

  HRESULT (STDMETHODCALLTYPE *GetIids)(
    IGraphicsCaptureSession4 * This,
    ULONG * iidCount,
    IID ** iids);

  HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(
    IGraphicsCaptureSession4 * This,
    HSTRING * className);

  HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(
    IGraphicsCaptureSession4 * This,
    TrustLevel * trustLevel);

  HRESULT (STDMETHODCALLTYPE *get_DirtyRegionMode)(
    IGraphicsCaptureSession4 * This,
    WGCGraphicsCaptureDirtyRegionMode * value);

  HRESULT (STDMETHODCALLTYPE *put_DirtyRegionMode)(
    IGraphicsCaptureSession4 * This,
    WGCGraphicsCaptureDirtyRegionMode value);

  END_INTERFACE
}
IGraphicsCaptureSession4Vtbl;

interface IGraphicsCaptureSession4
{
  CONST_VTBL IGraphicsCaptureSession4Vtbl * lpVtbl;
};

#define IGraphicsCaptureSession4_QueryInterface(This,riid,ppvObject) \
  (This)->lpVtbl->QueryInterface(This,riid,ppvObject)
#define IGraphicsCaptureSession4_put_DirtyRegionMode(This,value) \
  (This)->lpVtbl->put_DirtyRegionMode(This,value)
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
#define WGC_STALL_REINIT_US (3ULL * 1000ULL * 1000ULL)
#define WGC_STALL_BACKOFF_INITIAL_US (2ULL * 1000ULL * 1000ULL)
#define WGC_STALL_BACKOFF_MAX_US (30ULL * 1000ULL * 1000ULL)
#define WGC_STALL_DWM_IDLE_US (1ULL * 1000ULL * 1000ULL)

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

  // IVSHMEM publish only: the placed D3D12 resource that backs this slot in
  // the IVSHMEM heap. Stored so we can release it.
  ID3D12Resource   ** ivshmemD12Res;
  // Offset within the IVSHMEM heap (bytes) where this slot lives
  uint64_t            ivshmemOffset;
  // True once wgc_setIvshmemSlot has registered an offset for this slot.
  // wgc_ensureFrameIvshmem refuses to create textures for unregistered
  // slots — the caller must register them (typically on first iface->capture
  // for that frameBufferIndex) before they can be used.
  bool                ivshmemSlotReady;
  // Bridge textures for the IVSHMEM D3D12 copy path:
  //   bridgeA — D3D11 texture on the WGC frame pool's device (SHARED +
  //             SHARED_NTHANDLE), opened as bridge12 on the D3D12 side.
  //             WGC source → bridgeA on WGC context.
  //   bridgeB — fallback encode-shader UAV target, only created if the
  //             driver refuses a UAV bind on the shared bridgeA (the
  //             encode shader normally writes bridgeA directly).
  ID3D11Texture2D ** bridgeA;
  ID3D11Texture2D ** bridgeB;
  ID3D12Resource  ** bridge12;
  ID3D11UnorderedAccessView ** encodeUav;
  UINT64             d3d12CopyFenceValue[WGC_D3D12_COPY_QUEUE_MAX];
  unsigned           d3d12CopyQueueCount;
  // which command group recorded this frame's copies
  unsigned           d3d12CopyGroup;
  // Per-frame fence wait event. The capture and frame threads may wait on
  // the copy fences concurrently (slot reclaim vs consume); sharing one
  // auto-reset event between threads can lose a wakeup, so each frame owns
  // its own event and only the slot's current owner waits on it.
  HANDLE             d3d12CopyEvent;
}
WGCFrameInfo;

struct WGCInstance
{
  // enable damage tracking
  bool trackDamage;

  WGCPublishMode publishMode;
  CaptureGetPointerBuffer  getPointerBufferFn;
  CapturePostPointerBuffer postPointerBufferFn;

  // Loaned devices — set via wgc_setLoanedDevices before wgc_initInstance.
  // The loaned device (if any) is used directly.
  ID3D11Device        * loanedD11Device;
  ID3D11DeviceContext * loanedD11Context;
  ID3D12Device3       * loanedD12Device;

  // Cross-API sync fence for IVSHMEM_D3D12_COPY (NULL otherwise).
  //   wgcFence:      created on `device` (WGC frame pool's D3D11) with
  //                  D3D11_FENCE_FLAG_SHARED. Signaled on WGC context after
  //                  each bridge copy.
  //   wgcD3D12Fence: same fence, opened on the D3D12 device via a shared NT
  //                  handle. The D3D12 copy queue waits on this fence before
  //                  reading the bridge.
  ID3D11Fence       ** wgcFence;
  ID3D12Fence       ** wgcD3D12Fence;
  UINT64               wgcFenceValue;
  ID3D11ComputeShader ** nv12Shader;
  ID3D11Buffer        ** nv12ShaderConsts;
  bool                 nv12ShaderStateValid;
  bool                 nv12ShaderPqPreserve;
  bool                 nv12ShaderHdrToneMap;
  DXGI_COLOR_SPACE_TYPE nv12ShaderColorSpace;
  DXGI_FORMAT          nv12ShaderIvshmemFormat;

  // IVSHMEM publish environment — set via wgc_setIvshmemD3D12CopyEnv before
  // wgc_initInstance. Only used when publishMode is
  // WGC_PUBLISH_IVSHMEM_D3D12_COPY. The heap is owned by the caller; we hold
  // a raw pointer (no AddRef in this file — caller keeps it alive). Per-slot
  // offsets live on each WGCFrameInfo (set via wgc_setIvshmemSlot).
  ID3D12Heap        * ivshmemHeap;
  ID3D12CommandQueue * d3d12CopyQueues[WGC_D3D12_COPY_QUEUE_MAX];
  D12CommandGroup     d3d12CopyCommands[WGC_D3D12_COPY_QUEUE_MAX]
                                       [WGC_D3D12_COPY_GROUPS];
  bool                d3d12CopyCommandReady[WGC_D3D12_COPY_QUEUE_MAX];
  unsigned            d3d12CopyQueueCount;
  unsigned            d3d12CopyGroupIndex; // capture thread only
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
  // signaled whenever a frame is published; the frame thread waits on it in
  // wgc_waitPublish
  HANDLE publishEvent;
  IDirect3D11CaptureFrame * pendingFrame;

  WGCFrameInfo * frames;
  unsigned frameCount;
  WGCFrameInfo * current;
  WGCFrameInfo * consumerFrame;
  volatile LONG asyncNextSlot;
  bool asyncCapture;
  bool pollFramePool;
  bool debugStats;
  WGCStats stats;
  LONG asyncTimeouts;
  LONG asyncReadyBeforeWait;
  LONG asyncReadyAfterWait;
  LONG fullCopyAfterGap;
  LONG asyncSlotBusy;
  LONG forceNextFullCopy;
  uint64_t debugStatsLastLog;

  SizeInt32 size;
  RECT outputRect;
  IDXGIOutput * output;
  DXGI_COLOR_SPACE_TYPE colorSpace;
  bool roInitialized;
  unsigned emptyPolls;
  uint64_t emptyPollStartUs;
  LONG emptyPollStartEvents;
  LONG emptyPollStartPulled;
  LONG emptyPollStartConsumed;
  uint64_t emptyPollStartDwmFrames;
  uint64_t emptyPollLastDwmFrames;
  uint64_t emptyPollLastDwmChangeUs;
  uint64_t emptyPollDwmAdvanceUs;
  bool loggedFirstFrame;
  WGCCursorMode cursorMode;
  int maxFPS;
  bool includeSecondaryWindows;
  bool dwmFlushOnGap;
  uint64_t lastDwmFlushUs;
  bool hasLastSystemRelativeTime;
  int64_t lastSystemRelativeTime;
  bool mouseHookCreated;
  CRITICAL_SECTION cursorLock;
  bool cursorLockCreated;
  int cursorMaxHz;
  int dirtyFullCopyPercent;
  bool d3d12FullCopyAlways;
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

static WGCInstance * volatile wgcCursorInstance;
static volatile uint64_t wgcNextStarvationReinitUs = 0;
static volatile uint64_t wgcStarvationBackoffUs = 0;
static volatile uint64_t wgcLastStarvationReinitUs = 0;

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

static bool wgc_create(WGCInstance ** instance, unsigned frameBuffers);
static bool wgc_init(WGCInstance * this, bool debug,
  IDXGIAdapter1 * adapter, IDXGIOutput * output);
static bool wgc_deinit(WGCInstance * this);
static void wgc_free(WGCInstance ** instance);
static CaptureResult wgc_capture(WGCInstance * this,
  unsigned frameBufferIndex);
static void wgc_releaseSlot(WGCInstance * this, void * token);
static CaptureResult wgc_processFrame(WGCInstance * this,
  IDirect3D11CaptureFrame * frame, unsigned frameBufferIndex,
  uint64_t callbackTimeUs);
static bool wgc_selectAsyncSlot(WGCInstance * this, unsigned * frameBufferIndex);

static bool wgc_createCaptureItem(WGCInstance * this, HMONITOR monitor);
static bool wgc_createFramePool(WGCInstance * this);
static DXGI_FORMAT wgc_expectedSourceFormat(WGCInstance * this);
static bool wgc_asyncFrameReady(WGCInstance * this);
static void wgc_accumulateDamage(WGCInstance * this, const WGCFrameInfo * src);
static void wgc_clearAccumulatedDamage(WGCFrameInfo * frame);
static void wgc_copyFrameTexture(WGCInstance * this, WGCFrameInfo * dst,
  ID3D11Texture2D * src, bool publishCopy);
static bool wgc_copyFrameTextureD3D12(WGCInstance * this, WGCFrameInfo * dst);
static void wgc_waitFrameD3D12Copy(WGCInstance * this, WGCFrameInfo * frame);
static void wgc_drainGpuWork(WGCInstance * this);
static void wgc_copyFrameTextureRectCtx(ID3D11DeviceContext4 * ctx,
  ID3D11Resource * dst, ID3D11Resource * src, const RECT * rect);
static bool wgc_ensureAllFrames(WGCInstance * this, ID3D11Texture2D * src);
static uint64_t wgc_copyFramePixels(const WGCFrameInfo * frame);
static void wgc_recordCopyStats(WGCInstance * this, const WGCFrameInfo * frame,
  bool publishCopy, uint64_t pixels);
static bool wgc_ensureFrame(WGCInstance * this, WGCFrameInfo * frame,
  ID3D11Texture2D * src);
static void wgc_releaseFrameInfo(WGCFrameInfo * frame);
static void wgc_updateDamage(WGCInstance * this, WGCFrameInfo * info,
  IDirect3D11CaptureFrame * frame);
static bool wgc_shouldForceFullCopyAfterGap(WGCInstance * this,
  WGCFrameInfo * frame);
static void wgc_maybeLogDebugStats(WGCInstance * this);
static void wgc_maybeDwmFlushOnGap(WGCInstance * this);
static bool wgc_shouldReinitAfterStarvation(WGCInstance * this, uint64_t now);
static uint64_t wgc_dwmComposedFrames(void);
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
static void wgc_waitCallbacks(WGCFrameEventHandler * handler);
static bool wgc_isIvshmemPublishMode(WGCPublishMode mode);
static bool wgc_isNV12PackedIvshmem(const WGCInstance * this);
static bool wgc_isP010PackedIvshmem(const WGCInstance * this);
static bool wgc_isPackedYuvIvshmem(const WGCInstance * this);
static bool wgc_needsEncodeShader(const WGCInstance * this);

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
  return mode == WGC_PUBLISH_IVSHMEM_D3D12_COPY;
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
      wgcStats_interlockedMax64(&this->callbackGapMaxUs, gapUs);
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
  return S_OK;
}

static void wgc_boostCallbackThreadPriority(void)
{
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
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

static bool wgc_create(WGCInstance ** instance, unsigned frameBuffers)
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

  this->cursorPendingPos = wgc_packCursorPos(INT_MIN, INT_MIN);
  InitializeCriticalSection(&this->cursorLock);
  this->cursorLockCreated = true;
  *instance = this;
  return true;
}

static bool wgc_init(WGCInstance * this, bool debug,
  IDXGIAdapter1 * adapter, IDXGIOutput * output)
{
  bool result = false;
  HRESULT hr;
  comRef_scopePush(24);

  this->cursorMode = wgc_parseCursorMode();
  this->maxFPS     = option_get_int("wgc", "maxFPS");
  this->cursorMaxHz = option_get_int("wgc", "cursorMaxHz");
  this->dirtyFullCopyPercent = option_get_int("wgc", "dirtyFullCopyPercent");
  this->d3d12FullCopyAlways =
    option_get_bool("wgc", "d3d12FullCopyAlways");
  this->asyncCapture = option_get_bool("wgc", "asyncCapture");
  this->pollFramePool = option_get_bool("wgc", "pollFramePool");
  this->debugStats = option_get_bool("wgc", "debugStats");
  this->includeSecondaryWindows =
    option_get_bool("wgc", "includeSecondaryWindows");
  this->dwmFlushOnGap = option_get_bool("wgc", "dwmFlushOnGap");
  this->lastDwmFlushUs = 0;
  wgcStats_init(&this->stats, this->debugStats);
  DEBUG_INFO("WGC cursor:%s cursorMaxHz:%d maxFPS:%d asyncCapture:%d pollFramePool:%d includeSecondaryWindows:%d debugStats:%d dwmFlushOnGap:%d",
    this->cursorMode == WGC_CURSOR_MODE_SEPARATE ? "separate" :
    this->cursorMode == WGC_CURSOR_MODE_EMBEDDED ? "embedded" : "none",
    this->cursorMaxHz, this->maxFPS, this->asyncCapture,
    this->pollFramePool,
    this->includeSecondaryWindows, this->debugStats, this->dwmFlushOnGap);

  hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
  {
    DEBUG_WINERROR("RoInitialize failed", hr);
    goto exit;
  }
  this->roInitialized = hr != RPC_E_CHANGED_MODE;

  comRef_defineLocal(IGraphicsCaptureSessionStatics, sessionStatics);
  HSTRING className = NULL;
  if (!wgcUtil_createHString(
      RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
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

  const bool useLoanedForWgc = this->loanedD11Device != NULL;

  if (useLoanedForWgc)
  {
    // Caller supplied a D3D11 device. Borrow it — caller retains ownership.
    // We still go through the COM ref scope so subsequent QueryInterface
    // chains work uniformly.
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

  // With Capture_WGC.asyncCapture the frame thread maps/unmaps the CPU
  // staging texture on this context while the capture thread records copies;
  // multithread protection serializes those calls. The IVSHMEM publish path
  // never touches the context from the frame thread.
  comRef_defineLocal(ID3D11Multithread, d11mt);
  hr = ID3D11DeviceContext_QueryInterface(
    *d11context, &IID_ID3D11Multithread, (void **)d11mt);
  if (SUCCEEDED(hr))
    ID3D11Multithread_SetMultithreadProtected(*d11mt, TRUE);
  else
    DEBUG_WARN("ID3D11Multithread not available, CPU staging mode is not "
      "thread safe");

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
  this->output = output;
  IDXGIOutput_AddRef(this->output);

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

  this->colorSpace = wgcUtil_getOutputColorSpace(this->output);

  // Use the loaned D12 device if provided (the one that opened the IVSHMEM
  // heap).
  ID3D12Device3 * effectiveD12 = this->loanedD12Device;
  if (effectiveD12)
  {
    ID3D12Device3_AddRef(effectiveD12);
    comRef_toGlobal(this->d12device  , &effectiveD12 );
  }
  comRef_toGlobal(this->device       , d11device5    );
  comRef_toGlobal(this->context      , d11context4   );
  comRef_toGlobal(this->graphicsDevice, graphicsDevice);

  if (this->publishMode == WGC_PUBLISH_IVSHMEM_D3D12_COPY)
  {
    if (!this->ivshmemHeap || !this->d3d12CopyQueues[0] || !*this->d12device)
    {
      DEBUG_ERROR("ivshmem-d3d12-copy: missing D3D12 heap/device/queue");
      goto exit;
    }

    this->d3d12CopyQueueCount = 1;

    for(unsigned i = 0; i < this->d3d12CopyQueueCount; ++i)
    {
      wgc_setD3D12ObjectNameI((ID3D12Object *)this->d3d12CopyQueues[i],
        "WGC IVSHMEM D3D12 copy queue ", i);
      for(unsigned g = 0; g < WGC_D3D12_COPY_GROUPS; ++g)
      {
        if (!d12_commandGroupCreate(*this->d12device,
            D3D12_COMMAND_LIST_TYPE_COPY, &this->d3d12CopyCommands[i][g],
            L"WGC IVSHMEM D3D12 copy"))
        {
          DEBUG_ERROR("ivshmem-d3d12-copy: failed to create copy command group");
          goto exit;
        }
        hr = ID3D12GraphicsCommandList_Close(
          *this->d3d12CopyCommands[i][g].gfxList);
        if (FAILED(hr))
        {
          DEBUG_WINERROR("ivshmem-d3d12-copy: failed to close initial command list",
            hr);
          goto exit;
        }
      }
      this->d3d12CopyCommandReady[i] = true;
    }

    for(unsigned i = 0; i < this->frameCount; ++i)
    {
      this->frames[i].d3d12CopyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
      if (!this->frames[i].d3d12CopyEvent)
      {
        DEBUG_WINERROR("ivshmem-d3d12-copy: CreateEvent failed",
          GetLastError());
        goto exit;
      }
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

  this->publishEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!this->publishEvent)
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
    wgc_deinit(this);

  return result;
}

static bool wgc_deinit(WGCInstance * this)
{
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

  wgc_drainGpuWork(this);

  for(unsigned i = 0; i < WGC_D3D12_COPY_QUEUE_MAX; ++i)
  {
    for(unsigned g = 0; g < WGC_D3D12_COPY_GROUPS; ++g)
      d12_commandGroupFree(&this->d3d12CopyCommands[i][g]);
    this->d3d12CopyCommandReady[i] = false;
    if (this->d3d12CopyQueues[i])
    {
      ID3D12CommandQueue_Release(this->d3d12CopyQueues[i]);
      this->d3d12CopyQueues[i] = NULL;
    }
  }
  this->d3d12CopyQueueCount = 0;
  this->d3d12CopyGroupIndex = 0;

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

  if (this->session && *this->session)
    wgc_closeInspectable((IInspectable *)*this->session);

  if (this->framePool && *this->framePool)
    wgc_closeInspectable((IInspectable *)*this->framePool);

  comRef_release(this->session);
  comRef_release(this->framePool);
  comRef_release(this->item);
  comRef_release(this->graphicsDevice);
  comRef_release(this->nv12Shader);
  comRef_release(this->nv12ShaderConsts);
  wgcStats_free(&this->stats);
  if (this->output)
  {
    IDXGIOutput_Release(this->output);
    this->output = NULL;
  }

  if (this->frameEvent)
  {
    CloseHandle(this->frameEvent);
    this->frameEvent = NULL;
  }

  if (this->publishEvent)
  {
    CloseHandle(this->publishEvent);
    this->publishEvent = NULL;
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

static void wgc_free(WGCInstance ** instance)
{
  WGCInstance * this = *instance;

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

  const bool profile = this->debugStats;
  SizeInt32 size;
  hr = IDirect3D11CaptureFrame_get_ContentSize(frame, &size);
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

  const DXGI_COLOR_SPACE_TYPE colorSpace =
    wgcUtil_getOutputColorSpace(this->output);
  if (colorSpace != this->colorSpace)
  {
    DEBUG_INFO("WGC color space changed 0x%x -> 0x%x, reinitializing",
      (unsigned)this->colorSpace, (unsigned)colorSpace);
    this->colorSpace = colorSpace;
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

  comRef_defineLocal(IDirect3DSurface, surface);
  hr = IDirect3D11CaptureFrame_get_Surface(frame, surface);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get WGC frame surface", hr);
    goto exit;
  }

  comRef_defineLocal(IDirect3DDxgiInterfaceAccess, access);
  hr = IDirect3DSurface_QueryInterface(
    *surface, &IID_IDirect3DDxgiInterfaceAccess, (void **)access);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to query IDirect3DDxgiInterfaceAccess", hr);
    goto exit;
  }

  comRef_defineLocal(ID3D11Texture2D, src);
  hr = IDirect3DDxgiInterfaceAccess_GetInterface(
    *access, &IID_ID3D11Texture2D, (void **)src);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get WGC D3D11 texture", hr);
    goto exit;
  }

  D3D11_TEXTURE2D_DESC srcDesc;
  ID3D11Texture2D_GetDesc(*src, &srcDesc);
  const DXGI_FORMAT expectedFormat = wgc_expectedSourceFormat(this);
  if (srcDesc.Format != expectedFormat)
  {
    DEBUG_INFO("WGC source format changed 0x%x -> 0x%x, reinitializing",
      (unsigned)expectedFormat, (unsigned)srcDesc.Format);
    result = CAPTURE_RESULT_REINIT;
    goto exit;
  }
  wgcStats_maybeLogHDR(&this->stats, *this->device, *this->context, *src);

  if (profile)
    wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_ACQUIRE,
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
    if (!wgc_ensureFrame(this, dst, *src))
      goto exit;
    dst->callbackTimeUs = callbackTimeUs;
    if (profile)
      wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_ENSURE,
        microtime() - profileStart);

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

    if (this->cursorMode == WGC_CURSOR_MODE_SEPARATE &&
        !this->mouseHookCreated)
    {
      wgcCursorInstance = this;
      mouseHook_install(wgc_onMouseMove);
      this->mouseHookCreated = true;
      wgc_updatePointer(this, INT_MIN, INT_MIN);
    }

    profileStart = profile ? microtime() : 0;
    wgc_updateDamage(this, dst, frame);
    const bool forceFullCopyAfterGap =
      wgc_shouldForceFullCopyAfterGap(this, dst);
    const bool forceNextFullCopy =
      InterlockedExchange(&this->forceNextFullCopy, 0);
    const bool hasDirtyRects = dst->nbDirtyRects > 0;
    const bool keepGapDamage =
      forceFullCopyAfterGap && this->trackDamage && dst->copiedOnce &&
      hasDirtyRects && !forceNextFullCopy;
    if (forceFullCopyAfterGap)
    {
      if (this->debugStats)
        InterlockedIncrement(&this->fullCopyAfterGap);
    }
    if (forceNextFullCopy)
      dst->nbDirtyRects = 0;
    dst->fullCopy = !this->trackDamage || !dst->copiedOnce ||
      dst->nbDirtyRects == 0 || forceFullCopyAfterGap;
    if (keepGapDamage)
      dst->fullCopy = false;
    if (profile)
      wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_DAMAGE,
        microtime() - profileStart);

    wgc_accumulateDamage(this, dst);
    dst->fullCopy              = dst->pendingFullCopy || !dst->copiedOnce ||
                                 dst->nbPendingDirtyRects == 0;
    dst->nbDirtyRects          = dst->fullCopy ? 0 : dst->nbPendingDirtyRects;
    if (dst->nbDirtyRects > 0)
      memcpy(dst->dirtyRects, dst->pendingDirtyRects,
        dst->nbDirtyRects * sizeof(*dst->dirtyRects));

    profileStart = profile ? microtime() : 0;
    if (this->cursorMode == WGC_CURSOR_MODE_SEPARATE)
    {
      const LONG64 pending = InterlockedExchange64(
        &this->cursorPendingPos, wgc_packCursorPos(INT_MIN, INT_MIN));
      int x, y;
      wgc_unpackCursorPos(pending, &x, &y);
      wgc_updatePointer(this, x, y);
    }
    if (profile)
      wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_POINTER,
        microtime() - profileStart);

    profileStart = profile ? microtime() : 0;
    dst->copyFailed = false;
    wgc_copyFrameTexture(this, dst, *src, true);
    if (dst->copyFailed)
      goto exit;
    if (profile)
      wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_PUBLISH_COPY,
        microtime() - profileStart);

    profileStart = profile ? microtime() : 0;
    ID3D11DeviceContext4_Flush(*this->context);
    if (profile)
      wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_FLUSH,
        microtime() - profileStart);

    wgc_clearAccumulatedDamage(dst);
    InterlockedExchange(&dst->state, WGC_FRAME_READY);
    WGCFrameInfo * old = InterlockedExchangePointer(
      (PVOID volatile *)&this->current, dst);
    if (old && old != dst)
      InterlockedCompareExchange(&old->state,
        WGC_FRAME_FREE, WGC_FRAME_READY);
    SetEvent(this->publishEvent);
    result = CAPTURE_RESULT_OK;
    goto exit;
  }

  profileStart = profile ? microtime() : 0;
  accum = &this->frames[0];
  if (!wgc_ensureFrame(this, accum, *src))
    goto exit;
  if (!wgc_ensureAllFrames(this, *src))
    goto exit;
  accum->callbackTimeUs = callbackTimeUs;
  if (profile)
    wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_ENSURE,
      microtime() - profileStart);

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

  if (this->cursorMode == WGC_CURSOR_MODE_SEPARATE &&
      !this->mouseHookCreated)
  {
    wgcCursorInstance = this;
    mouseHook_install(wgc_onMouseMove);
    this->mouseHookCreated = true;
    wgc_updatePointer(this, INT_MIN, INT_MIN);
  }

  profileStart = profile ? microtime() : 0;
  wgc_updateDamage(this, accum, frame);
  const bool forceFullCopyAfterGap =
    wgc_shouldForceFullCopyAfterGap(this, accum);
  const bool forceNextFullCopy =
    InterlockedExchange(&this->forceNextFullCopy, 0);
  const bool hasDirtyRects = accum->nbDirtyRects > 0;
  const bool keepGapDamage =
    forceFullCopyAfterGap && this->trackDamage && accum->copiedOnce &&
    hasDirtyRects && !forceNextFullCopy;
  if (forceFullCopyAfterGap)
  {
    if (this->debugStats)
      InterlockedIncrement(&this->fullCopyAfterGap);
  }
  if (forceNextFullCopy)
    accum->nbDirtyRects = 0;
  accum->fullCopy = !this->trackDamage || !accum->copiedOnce ||
    accum->nbDirtyRects == 0 || forceFullCopyAfterGap;
  if (profile)
    wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_DAMAGE,
      microtime() - profileStart);

  profileStart = profile ? microtime() : 0;
  wgc_copyFrameTexture(this, accum, *src, false);
  if (profile)
    wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_ACCUM_COPY,
      microtime() - profileStart);

  if (keepGapDamage)
    accum->fullCopy = false;

  wgc_accumulateDamage(this, accum);

  profileStart = profile ? microtime() : 0;
  if (this->cursorMode == WGC_CURSOR_MODE_SEPARATE)
  {
    const LONG64 pending = InterlockedExchange64(
      &this->cursorPendingPos, wgc_packCursorPos(INT_MIN, INT_MIN));
    int x, y;
    wgc_unpackCursorPos(pending, &x, &y);
    wgc_updatePointer(this, x, y);
  }
  if (profile)
    wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_POINTER,
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
  dst->copyFailed = false;
  wgc_copyFrameTexture(this, dst, *accum->texture, true);
  if (dst->copyFailed)
    goto exit;
  if (profile)
    wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_PUBLISH_COPY,
      microtime() - profileStart);

  profileStart = profile ? microtime() : 0;
  // CPU consumer relies on D3D11 Map() blocking for outstanding GPU work; we
  // still Flush to make sure the copy gets to the driver promptly.
  ID3D11DeviceContext4_Flush(*this->context);
  if (profile)
    wgcStats_recordProfileStage(&this->stats, WGC_PROFILE_FLUSH,
      microtime() - profileStart);

  wgc_clearAccumulatedDamage(dst);

  InterlockedExchange(&dst->state, WGC_FRAME_READY);
  WGCFrameInfo * old = InterlockedExchangePointer(
    (PVOID volatile *)&this->current, dst);
  if (old && old != dst)
    InterlockedCompareExchange(&old->state,
      WGC_FRAME_FREE, WGC_FRAME_READY);
  SetEvent(this->publishEvent);

  result = CAPTURE_RESULT_OK;

exit:
  if (result != CAPTURE_RESULT_OK && dst)
    InterlockedCompareExchange(&dst->state,
      WGC_FRAME_FREE, WGC_FRAME_WRITING);
  comRef_scopePop();
  return result;
}

static CaptureResult wgc_capture(WGCInstance * this,
  unsigned frameBufferIndex)
{
  CaptureResult result = CAPTURE_RESULT_ERROR;
  IDirect3D11CaptureFrame * frame = NULL;
  comRef_scopePush(8);

  const bool callbackProcessed = this->asyncCapture &&
    !wgc_isIvshmemPublishMode(this->publishMode);

  if (callbackProcessed && wgc_asyncFrameReady(this))
  {
    if (this->debugStats)
      InterlockedIncrement(&this->asyncReadyBeforeWait);
    result = CAPTURE_RESULT_OK;
    goto exit;
  }

  const DWORD wait = WaitForSingleObject(this->frameEvent,
    this->pollFramePool ? WGC_POLL_FRAME_POOL_MS : 1000);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT)
  {
    DEBUG_WINERROR("Waiting for a WGC frame failed, reinitializing",
      GetLastError());
    result = CAPTURE_RESULT_REINIT;
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

  frame = InterlockedExchangePointer(
    (PVOID volatile *)&this->pendingFrame, NULL);

  // IVSHMEM diagnostic: if the callback hasn't given us a pending frame,
  // optionally poll the frame pool directly. This tells us whether
  // FrameArrived notification delivery is the bottleneck.
  if (!frame && (this->pollFramePool ||
        wgc_isIvshmemPublishMode(this->publishMode)) &&
      this->framePool)
  {
    IDirect3D11CaptureFrame * polled = NULL;
    HRESULT pollHr = IDirect3D11CaptureFramePool_TryGetNextFrame(
      *this->framePool, &polled);
    if (FAILED(pollHr))
    {
      DEBUG_WINERROR("WGC direct TryGetNextFrame failed, reinitializing",
        pollHr);
      result = CAPTURE_RESULT_REINIT;
      goto exit;
    }

    if (polled)
    {
      if (!this->pollFramePool)
        DEBUG_INFO("WGC direct: pulled frame via direct TryGetNextFrame poll "
          "(FrameArrived was silent)");
      frame = polled;
    }
    else if (!this->pollFramePool && ++this->emptyPolls % 3 == 0)
      DEBUG_INFO("WGC direct: TryGetNextFrame poll returned NULL "
        "(hr=0x%08lx emptyPolls=%u events:%ld) — pool is empty",
        (unsigned long)pollHr, this->emptyPolls,
        this->handler ?
          InterlockedCompareExchange(&this->handler->events, 0, 0) : 0);
  }

  if (!frame)
  {
    const uint64_t now = microtime();
    const DXGI_COLOR_SPACE_TYPE colorSpace =
    wgcUtil_getOutputColorSpace(this->output);
    if (colorSpace != this->colorSpace)
    {
      DEBUG_INFO("WGC color space changed while idle 0x%x -> 0x%x, reinitializing",
        (unsigned)this->colorSpace, (unsigned)colorSpace);
      this->colorSpace = colorSpace;
      result = CAPTURE_RESULT_REINIT;
      goto exit;
    }

    if (wgc_shouldReinitAfterStarvation(this, now))
    {
      result = CAPTURE_RESULT_REINIT;
      goto exit;
    }

    wgc_maybeDwmFlushOnGap(this);
    if (++this->emptyPolls % 30 == 0)
    {
      const LONG events = this->handler ?
        InterlockedCompareExchange(&this->handler->events, 0, 0) : 0;
      const LONG pulled = this->handler ?
        InterlockedCompareExchange(&this->handler->framesPulled, 0, 0) : 0;
      const LONG consumed = this->handler ?
        InterlockedCompareExchange(&this->handler->framesConsumed, 0, 0) : 0;
      DEBUG_INFO("WGC has no pending frame (idx=%u publishMode=%d emptyPolls=%u "
        "events:%ld pulled:%ld consumed:%ld)",
        frameBufferIndex, (int)this->publishMode, this->emptyPolls,
        events, pulled, consumed);
    }
    result = CAPTURE_RESULT_TIMEOUT;
    goto exit;
  }
  this->emptyPolls = 0;
  this->emptyPollStartUs = 0;
  this->emptyPollStartEvents = 0;
  this->emptyPollStartPulled = 0;
  this->emptyPollStartConsumed = 0;
  this->emptyPollStartDwmFrames = 0;
  this->emptyPollLastDwmFrames = 0;
  this->emptyPollLastDwmChangeUs = 0;
  this->emptyPollDwmAdvanceUs = 0;
  // a fresh session usually delivers an initial frame before starving again,
  // so only forgive the backoff after sustained delivery since the last
  // starvation reinit
  if (wgcStarvationBackoffUs &&
      microtime() - wgcLastStarvationReinitUs > 2 * WGC_STALL_BACKOFF_MAX_US)
  {
    wgcNextStarvationReinitUs = 0;
    wgcStarvationBackoffUs = 0;
  }
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

static bool wgc_shouldReinitAfterStarvation(WGCInstance * this, uint64_t now)
{
  const LONG events = this->handler ?
    InterlockedCompareExchange(&this->handler->events, 0, 0) : 0;
  const LONG pulled = this->handler ?
    InterlockedCompareExchange(&this->handler->framesPulled, 0, 0) : 0;
  const LONG consumed = this->handler ?
    InterlockedCompareExchange(&this->handler->framesConsumed, 0, 0) : 0;

  if (!this->emptyPollStartUs ||
      events   != this->emptyPollStartEvents ||
      pulled   != this->emptyPollStartPulled ||
      consumed != this->emptyPollStartConsumed)
  {
    this->emptyPollStartUs = now;
    this->emptyPollStartEvents = events;
    this->emptyPollStartPulled = pulled;
    this->emptyPollStartConsumed = consumed;
    this->emptyPollStartDwmFrames = wgc_dwmComposedFrames();
    this->emptyPollLastDwmFrames = this->emptyPollStartDwmFrames;
    this->emptyPollLastDwmChangeUs = now;
    this->emptyPollDwmAdvanceUs = 0;
    return false;
  }

  const uint64_t stalledUs = now - this->emptyPollStartUs;
  if (stalledUs < WGC_STALL_REINIT_US)
    return false;

  // WGC legitimately delivers nothing while the desktop is static; only
  // treat the silence as starvation if DWM composed new frames that WGC
  // failed to deliver. A failed DWM query (sample of 0) means progress is
  // unknown, so fall back to time-only detection instead of disabling the
  // watchdog
  const uint64_t dwmFrames = wgc_dwmComposedFrames();
  if (dwmFrames && !this->emptyPollStartDwmFrames)
    this->emptyPollStartDwmFrames = dwmFrames;
  if (dwmFrames && this->emptyPollStartDwmFrames)
  {
    if (dwmFrames == this->emptyPollStartDwmFrames)
      return false;

    if (dwmFrames != this->emptyPollLastDwmFrames)
    {
      this->emptyPollLastDwmFrames = dwmFrames;
      this->emptyPollLastDwmChangeUs = now;
    }
    else if (now - this->emptyPollLastDwmChangeUs >= WGC_STALL_DWM_IDLE_US)
    {
      // an isolated composition (cursor, another monitor) followed by idle
      // is not starvation; re-arm the gate against the current count
      this->emptyPollStartDwmFrames = dwmFrames;
      this->emptyPollDwmAdvanceUs = 0;
      return false;
    }

    // content can resume after a long idle gap; require a full stall window
    // of sustained DWM composition before declaring the session dead
    if (!this->emptyPollDwmAdvanceUs)
      this->emptyPollDwmAdvanceUs = now;
    if (now - this->emptyPollDwmAdvanceUs < WGC_STALL_REINIT_US)
      return false;
  }

  if (now < wgcNextStarvationReinitUs)
  {
    if (this->emptyPolls % 120 == 0)
      DEBUG_WARN("WGC capture appears starved for %.2fs, reinit suppressed "
        "by backoff for %.2fs (events:%ld pulled:%ld consumed:%ld)",
        (double)stalledUs / 1000000.0,
        (double)(wgcNextStarvationReinitUs - now) / 1000000.0,
        events, pulled, consumed);
    return false;
  }

  if (!wgcStarvationBackoffUs)
    wgcStarvationBackoffUs = WGC_STALL_BACKOFF_INITIAL_US;
  else
    wgcStarvationBackoffUs = min(
      wgcStarvationBackoffUs * 2, WGC_STALL_BACKOFF_MAX_US);
  wgcNextStarvationReinitUs = now + wgcStarvationBackoffUs;
  wgcLastStarvationReinitUs = now;

  const uint64_t dwmAdvanced = dwmFrames && this->emptyPollStartDwmFrames ?
    dwmFrames - this->emptyPollStartDwmFrames : 0;
  DEBUG_WARN("WGC capture starved for %.2fs with no callback/queue progress "
    "while DWM composed %llu frames (events:%ld pulled:%ld consumed:%ld), "
    "reinitializing; next starvation reinit backoff %.2fs",
    (double)stalledUs / 1000000.0,
    (unsigned long long)dwmAdvanced,
    events, pulled, consumed,
    (double)wgcStarvationBackoffUs / 1000000.0);
  return true;
}

static uint64_t wgc_dwmComposedFrames(void)
{
  DWM_TIMING_INFO timing = { .cbSize = sizeof(timing) };
  if (FAILED(DwmGetCompositionTimingInfo(NULL, &timing)))
    return 0;
  return timing.cFrame;
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
  const uint64_t thresholdUs = WGC_DWM_FLUSH_GAP_MS * UINT64_C(1000);
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

static void wgc_releaseSlot(WGCInstance * this, void * token)
{
  if (!token)
    return;

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
  comRef_scopePush(4);

  HSTRING className = NULL;
  if (!wgcUtil_createHString(
      RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem,
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
  comRef_scopePush(7);

  HSTRING className = NULL;
  if (!wgcUtil_createHString(
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

  const bool hdrSource = wgcUtil_colorSpaceIsHDR(this->colorSpace);
  const bool hdrCapablePublish =
    this->ivshmemFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
    wgc_isPackedYuvIvshmem(this);
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

  if (this->trackDamage)
  {
    comRef_defineLocal(IGraphicsCaptureSession4, session4);
    hr = IGraphicsCaptureSession_QueryInterface(
      *session, &IID_IGraphicsCaptureSession4, (void **)session4);
    if (SUCCEEDED(hr))
    {
      hr = IGraphicsCaptureSession4_put_DirtyRegionMode(
        *session4, WGC_GRAPHICS_CAPTURE_DIRTY_REGION_MODE_REPORT_ONLY);
      if (FAILED(hr))
        DEBUG_WINERROR("Failed to enable WGC dirty region reporting", hr);
      else
        DEBUG_INFO("WGC dirty region reporting enabled");
    }
    else
      DEBUG_WARN("WGC dirty region reporting is not available on this OS");
  }

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

  if (!this->trackDamage)
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
  if (!this->trackDamage || !frame->hasSystemRelativeTime)
    return false;

  bool forceFullCopy = false;
  if (this->hasLastSystemRelativeTime &&
      frame->systemRelativeTime > this->lastSystemRelativeTime)
  {
    const int64_t delta =
      frame->systemRelativeTime - this->lastSystemRelativeTime;
    wgcStats_recordSystemRelativeGap(&this->stats, (LONG64)(delta / 10));
    forceFullCopy = delta > WGC_FULL_COPY_GAP_100NS;
  }

  this->hasLastSystemRelativeTime = true;
  this->lastSystemRelativeTime = frame->systemRelativeTime;
  return forceFullCopy;
}

static void wgc_maybeLogDebugStats(WGCInstance * this)
{
  if (!this->debugStats)
    return;

  const uint64_t now = microtime();
  if (now - this->debugStatsLastLog < 1000000)
    return;
  this->debugStatsLastLog = now;

  WGCFrameEventHandler * handler = this->handler;
  const WGCStatsReport ext =
  {
    .readyPre  = InterlockedExchange(&this->asyncReadyBeforeWait, 0),
    .readyPost = InterlockedExchange(&this->asyncReadyAfterWait , 0),
    .timeouts  = InterlockedExchange(&this->asyncTimeouts       , 0),
    .bursts    = handler ?
      InterlockedExchange(&handler->callbackBursts, 0) : 0,
    .maxBatch  = handler ?
      InterlockedExchange(&handler->maxCallbackBatch, 0) : 0,
    .cbGapTotalUs = handler ?
      InterlockedExchange64(&handler->callbackGapTotalUs, 0) : 0,
    .cbGapMaxUs   = handler ?
      InterlockedExchange64(&handler->callbackGapMaxUs, 0) : 0,
    .cbGapCount   = handler ?
      InterlockedExchange64(&handler->callbackGapCount, 0) : 0,
    .gapFull   = InterlockedExchange(&this->fullCopyAfterGap, 0),
    .slotBusy  = InterlockedExchange(&this->asyncSlotBusy, 0),
    .events    = handler ?
      InterlockedCompareExchange(&handler->events, 0, 0) : 0,
    .pulled    = handler ?
      InterlockedCompareExchange(&handler->framesPulled, 0, 0) : 0,
    .consumed  = handler ?
      InterlockedCompareExchange(&handler->framesConsumed, 0, 0) : 0
  };
  wgcStats_report(&this->stats, &ext);
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

static bool wgc_frameHasResources(const WGCFrameInfo * frame)
{
  return frame->texture || frame->ivshmemD12Res || frame->bridgeA ||
    frame->bridgeB || frame->bridge12 || frame->encodeUav;
}

static void wgc_releaseFrameResources(WGCFrameInfo * frame)
{
  comRef_release(frame->texture);
  comRef_release(frame->ivshmemD12Res);
  comRef_release(frame->bridgeA);
  comRef_release(frame->bridgeB);
  comRef_release(frame->bridge12);
  comRef_release(frame->encodeUav);
  memset(&frame->format, 0, sizeof(frame->format));
  frame->copiedOnce = false;
  frame->ready      = false;
}

static void wgc_releaseFrameInfo(WGCFrameInfo * frame)
{
  wgc_releaseFrameResources(frame);
  if (frame->d3d12CopyEvent)
    CloseHandle(frame->d3d12CopyEvent);
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

static bool wgc_isP010PackedIvshmem(const WGCInstance * this)
{
  return this->ivshmemFormat == DXGI_FORMAT_R16G16B16A16_UINT;
}

static bool wgc_isPackedYuvIvshmem(const WGCInstance * this)
{
  return wgc_isNV12PackedIvshmem(this) || wgc_isP010PackedIvshmem(this);
}

static bool wgc_needsEncodeShader(const WGCInstance * this)
{
  return wgc_isPackedYuvIvshmem(this);
}

typedef struct WGCEncodeShaderConsts
{
  UINT originX;
  UINT originY;
  UINT pad[2];
}
WGCEncodeShaderConsts;

static void wgc_invalidateNV12Shader(WGCInstance * this)
{
  if (!this)
    return;

  comRef_release(this->nv12Shader);
  comRef_release(this->nv12ShaderConsts);
  this->nv12ShaderStateValid = false;
}

static DXGI_FORMAT wgc_expectedSourceFormat(WGCInstance * this)
{
  const bool hdrSource = wgcUtil_colorSpaceIsHDR(this->colorSpace);
  const bool hdrCapablePublish =
    this->ivshmemFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
    wgc_isPackedYuvIvshmem(this);

  return (hdrSource && hdrCapablePublish) ?
    DXGI_FORMAT_R16G16B16A16_FLOAT :
    DXGI_FORMAT_B8G8R8A8_UNORM;
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
  const bool pqPreserve = wgc_isP010PackedIvshmem(this);
  const bool hdrToneMap = !pqPreserve &&
    wgcUtil_colorSpaceIsHDR(this->colorSpace);

  if (this->nv12Shader && this->nv12ShaderStateValid &&
      this->nv12ShaderPqPreserve     == pqPreserve &&
      this->nv12ShaderHdrToneMap     == hdrToneMap &&
      this->nv12ShaderColorSpace     == this->colorSpace &&
      this->nv12ShaderIvshmemFormat  == this->ivshmemFormat)
    return true;

  if (this->nv12Shader)
  {
    DEBUG_INFO("WGC YUV encoder shader state changed, rebuilding "
      "(old toneMap:%d pq:%d colorSpace:0x%x format:0x%x, "
      "new toneMap:%d pq:%d colorSpace:0x%x format:0x%x)",
      this->nv12ShaderHdrToneMap,
      this->nv12ShaderPqPreserve,
      (unsigned)this->nv12ShaderColorSpace,
      (unsigned)this->nv12ShaderIvshmemFormat,
      hdrToneMap,
      pqPreserve,
      (unsigned)this->colorSpace,
      (unsigned)this->ivshmemFormat);
    wgc_invalidateNV12Shader(this);
  }

  static const char shaderCode[] =
    "Texture2D<float4> srcTex : register(t0);\n"
    "#if WGC_HDR_PQ_PRESERVE\n"
    "RWTexture2D<uint4> dstTex : register(u0);\n"
    "#else\n"
    "RWTexture2D<float4> dstTex : register(u0);\n"
    "#endif\n"
    "cbuffer EncodeConsts : register(b0)\n"
    "{\n"
    "  uint2 origin;\n"
    "  uint2 pad;\n"
    "};\n"
    "\n"
    "uint pack16(float v)\n"
    "{\n"
    "  return (uint)floor(saturate(v) * 65535.0 + 0.5);\n"
    "}\n"
    "\n"
    "void writePacked(uint2 p, float4 v)\n"
    "{\n"
    "#if WGC_HDR_PQ_PRESERVE\n"
    "  dstTex[p] = uint4(pack16(v.r), pack16(v.g), pack16(v.b), pack16(v.a));\n"
    "#else\n"
    "  dstTex[p] = saturate(v);\n"
    "#endif\n"
    "}\n"
    "\n"
    "float3 bgraToRgb(float4 c)\n"
    "{\n"
    "  return c.rgb;\n"
    "}\n"
    "\n"
    "float3 prepareRgb(float3 rgb)\n"
    "{\n"
    "#if WGC_HDR_PQ_PRESERVE\n"
    "  float3 linear2020;\n"
    "  linear2020.r = dot(rgb, float3(0.6274039, 0.3292829, 0.0433131));\n"
    "  linear2020.g = dot(rgb, float3(0.0690973, 0.9195404, 0.0113622));\n"
    "  linear2020.b = dot(rgb, float3(0.0163914, 0.0880133, 0.8955953));\n"
    "  linear2020 = max(linear2020, float3(0.0, 0.0, 0.0));\n"
    "  float sdrWhiteNits = 80.0;\n"
    "  float m1 = 2610.0 / 16384.0;\n"
    "  float m2 = 2523.0 / 32.0;\n"
    "  float pqC1 = 3424.0 / 4096.0;\n"
    "  float pqC2 = 2413.0 / 128.0;\n"
    "  float pqC3 = 2392.0 / 128.0;\n"
    "  float3 n = saturate((linear2020 * sdrWhiteNits) / 10000.0);\n"
    "  float3 p = pow(n, float3(m1, m1, m1));\n"
    "  rgb = pow((float3(pqC1, pqC1, pqC1) + float3(pqC2, pqC2, pqC2) * p) /\n"
    "    (float3(1.0, 1.0, 1.0) + float3(pqC3, pqC3, pqC3) * p),\n"
    "    float3(m2, m2, m2));\n"
    "  rgb = floor(saturate(rgb) * 1023.0 + 0.5) / 1023.0;\n"
    "#else\n"
    "#if WGC_HDR_TONEMAP\n"
    "  rgb = max(rgb, 0.0.xxx);\n"
    "  float peak = max(rgb.r, max(rgb.g, rgb.b));\n"
    "  if (peak > 1.0)\n"
    "    rgb /= peak;\n"
    "#endif\n"
    "#endif\n"
    "  return saturate(rgb);\n"
    "}\n"
    "\n"
    "float luma(float3 rgb)\n"
    "{\n"
    "#if WGC_HDR_PQ_PRESERVE\n"
    "  return dot(rgb, float3(0.2627, 0.6780, 0.0593));\n"
    "#else\n"
    "  return dot(rgb, float3(0.2126, 0.7152, 0.0722));\n"
    "#endif\n"
    "}\n"
    "\n"
    "[numthreads(16, 16, 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  uint srcW, srcH;\n"
    "  srcTex.GetDimensions(srcW, srcH);\n"
    "  uint2 p0 = uint2(dt.x * 4, dt.y * 2) + origin;\n"
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
    "  uint dstX = p0.x / 4;\n"
    "  writePacked(uint2(dstX, p0.y), float4(y0, y1, y4, y5));\n"
    "  if (p2.y < srcH)\n"
    "    writePacked(uint2(dstX, p2.y), float4(y2, y3, y6, y7));\n"
    "\n"
    "  float3 avg0 = (c0 + c1 + c2 + c3) * 0.25;\n"
    "  float yy0 = luma(avg0);\n"
    "#if WGC_HDR_PQ_PRESERVE\n"
    "  float u0 = saturate((avg0.b - yy0) * 0.5315 + 0.5);\n"
    "  float v0 = saturate((avg0.r - yy0) * 0.6782 + 0.5);\n"
    "#else\n"
    "  float u0 = saturate((avg0.b - yy0) * 0.5389 + 0.5);\n"
    "  float v0 = saturate((avg0.r - yy0) * 0.6350 + 0.5);\n"
    "#endif\n"
    "  float3 avg1 = (c4 + c5 + c6 + c7) * 0.25;\n"
    "  float yy1 = luma(avg1);\n"
    "#if WGC_HDR_PQ_PRESERVE\n"
    "  float u1 = saturate((avg1.b - yy1) * 0.5315 + 0.5);\n"
    "  float v1 = saturate((avg1.r - yy1) * 0.6782 + 0.5);\n"
    "#else\n"
    "  float u1 = saturate((avg1.b - yy1) * 0.5389 + 0.5);\n"
    "  float v1 = saturate((avg1.r - yy1) * 0.6350 + 0.5);\n"
    "#endif\n"
    "  uint uvY = srcH + p0.y / 2;\n"
    "  writePacked(uint2(dstX, uvY), float4(u0, v0, u1, v1));\n"
    "}\n";

  bool result = false;
  HRESULT hr;
  comRef_scopePush(4);
  comRef_defineLocal(ID3DBlob, blob);
  comRef_defineLocal(ID3DBlob, error);
  comRef_defineLocal(ID3D11ComputeShader, shader);
  comRef_defineLocal(ID3D11Buffer, consts);

  const D3D_SHADER_MACRO macros[] =
  {
    { "WGC_HDR_TONEMAP", hdrToneMap ? "1" : "0" },
    { "WGC_HDR_PQ_PRESERVE", pqPreserve ? "1" : "0" },
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

  D3D11_BUFFER_DESC bufferDesc =
  {
    .ByteWidth = sizeof(WGCEncodeShaderConsts),
    .Usage     = D3D11_USAGE_DEFAULT,
    .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
  };
  hr = ID3D11Device5_CreateBuffer(*this->device, &bufferDesc, NULL, consts);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Create WGC encode constant buffer failed", hr);
    goto exit;
  }

  comRef_toGlobal(this->nv12Shader, shader);
  comRef_toGlobal(this->nv12ShaderConsts, consts);
  this->nv12ShaderStateValid    = true;
  this->nv12ShaderPqPreserve    = pqPreserve;
  this->nv12ShaderHdrToneMap    = hdrToneMap;
  this->nv12ShaderColorSpace    = this->colorSpace;
  this->nv12ShaderIvshmemFormat = this->ivshmemFormat;
  DEBUG_INFO("WGC encode shader ready "
    "(hdrToneMap:%d pqPreserve:%d colorSpace:0x%x format:0x%x)",
    hdrToneMap, pqPreserve, (unsigned)this->colorSpace,
    (unsigned)this->ivshmemFormat);
  result = true;

exit:
  comRef_scopePop();
  return result;
}

static void wgc_setEncodeOrigin(WGCInstance * this, UINT originX, UINT originY)
{
  const WGCEncodeShaderConsts consts =
  {
    .originX = originX,
    .originY = originY,
  };

  ID3D11DeviceContext4_UpdateSubresource(*this->context,
    (ID3D11Resource *)*this->nv12ShaderConsts, 0, NULL, &consts, 0, 0);
}

static void wgc_dispatchEncodeFull(WGCInstance * this,
  const D3D11_TEXTURE2D_DESC * srcDesc)
{
  wgc_setEncodeOrigin(this, 0, 0);

  ID3D11DeviceContext4_Dispatch(*this->context,
    (srcDesc->Width  + 31) / 32,
    (srcDesc->Height + 31) / 32,
    1);
}

static bool wgc_dispatchEncodeDirtyPackedYuv(WGCInstance * this,
  const WGCFrameInfo * dst, const D3D11_TEXTURE2D_DESC * srcDesc)
{
  if (!wgc_isPackedYuvIvshmem(this) || dst->fullCopy ||
      this->d3d12FullCopyAlways ||
      dst->nbDirtyRects == 0)
    return false;

  bool dispatched = false;
  for(const RECT * rect = dst->dirtyRects;
      rect < dst->dirtyRects + dst->nbDirtyRects; ++rect)
  {
    UINT left   = (UINT)max(0, min(rect->left  , (LONG)srcDesc->Width ));
    UINT top    = (UINT)max(0, min(rect->top   , (LONG)srcDesc->Height));
    UINT right  = (UINT)max(0, min(rect->right , (LONG)srcDesc->Width ));
    UINT bottom = (UINT)max(0, min(rect->bottom, (LONG)srcDesc->Height));
    if (right <= left || bottom <= top)
      continue;

    left   &= ~3u;
    top    &= ~1u;
    right   = min(srcDesc->Width , (right  + 3u) & ~3u);
    bottom  = min(srcDesc->Height, (bottom + 1u) & ~1u);
    if (right <= left || bottom <= top)
      continue;

    wgc_setEncodeOrigin(this, left, top);
    const UINT threadsX = (right  - left + 3u) / 4u;
    const UINT threadsY = (bottom - top  + 1u) / 2u;
    ID3D11DeviceContext4_Dispatch(*this->context,
      (threadsX + 15u) / 16u,
      (threadsY + 15u) / 16u,
      1);
    dispatched = true;
  }

  return dispatched;
}

static bool wgc_encodeFrameNV12(WGCInstance * this, WGCFrameInfo * dst,
  ID3D11Texture2D * src)
{
  if (!dst->bridgeA || !dst->encodeUav || !wgc_ensureNV12Shader(this))
    return false;

  bool result = false;
  HRESULT hr;
  comRef_scopePush(5);
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
  ID3D11Buffer * constBuffers[1] = { *this->nv12ShaderConsts };
  ID3D11DeviceContext4_CSSetShader(*this->context, *this->nv12Shader, NULL, 0);
  ID3D11DeviceContext4_CSSetShaderResources(*this->context, 0, 1, srvs);
  ID3D11DeviceContext4_CSSetConstantBuffers(*this->context, 0, 1,
    constBuffers);
  ID3D11DeviceContext4_CSSetUnorderedAccessViews(*this->context, 0, 1, uavs,
    NULL);
  if (!wgc_dispatchEncodeDirtyPackedYuv(this, dst, &srcDesc))
    wgc_dispatchEncodeFull(this, &srcDesc);

  ID3D11ShaderResourceView * nullSrvs[1] = { NULL };
  ID3D11UnorderedAccessView * nullUavs[1] = { NULL };
  ID3D11Buffer * nullConstBuffers[1] = { NULL };
  ID3D11DeviceContext4_CSSetShaderResources(*this->context, 0, 1, nullSrvs);
  ID3D11DeviceContext4_CSSetConstantBuffers(*this->context, 0, 1,
    nullConstBuffers);
  ID3D11DeviceContext4_CSSetUnorderedAccessViews(*this->context, 0, 1,
    nullUavs, NULL);
  ID3D11DeviceContext4_CSSetShader(*this->context, NULL, NULL, 0);

  const bool doEncodeBridgeLog = wgc_isP010PackedIvshmem(this) &&
    wgcStats_shouldDumpP010(&this->stats);

  if (doEncodeBridgeLog && dst->bridgeB)
    wgcStats_dumpP010(*this->device, *this->context, *dst->bridgeB,
      this->ivshmemWidth, this->ivshmemHeight, "encode UAV");

  // Fallback path only: the encode shader normally writes the shared bridge
  // (bridgeA) directly; bridgeB exists only if the driver refused a UAV bind
  // on the shared texture, in which case copy the encoded frame across.
  if (this->publishMode == WGC_PUBLISH_IVSHMEM_D3D12_COPY && dst->bridgeB)
  {
    ID3D11DeviceContext4_Flush(*this->context);
    ID3D11DeviceContext4_CopyResource(*this->context,
      (ID3D11Resource *)*dst->bridgeA, (ID3D11Resource *)*dst->bridgeB);
  }

  if (doEncodeBridgeLog)
    wgcStats_dumpP010(*this->device, *this->context, *dst->bridgeA,
      this->ivshmemWidth, this->ivshmemHeight, "bridge");

  // Packed YUV/P010 does not share the source frame's dimensions, but the
  // D3D12 IVSHMEM copy path remaps source dirty rects into packed-buffer
  // rects before publishing.
  if (!(wgc_isPackedYuvIvshmem(this) &&
        this->publishMode == WGC_PUBLISH_IVSHMEM_D3D12_COPY))
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

  if (directPublish && dst->bridgeA && dst->bridge12)
  {
    if (wgc_needsEncodeShader(this))
    {
      if (!wgc_encodeFrameNV12(this, dst, src))
      {
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

  if (dst->fullCopy)
    ID3D11DeviceContext4_CopyResource(*this->context,
      (ID3D11Resource *)*dst->texture, (ID3D11Resource *)src);
  else
    for(const RECT * rect = dst->dirtyRects;
        rect < dst->dirtyRects + dst->nbDirtyRects; ++rect)
      wgc_copyFrameTextureRectCtx(*this->context,
        (ID3D11Resource *)*dst->texture, (ID3D11Resource *)src, rect);

  dst->copiedOnce = true;
}

static bool wgc_mapPackedYuvCopyRects(WGCInstance * this,
  const RECT * srcRects, unsigned srcCount,
  RECT * dstRects, unsigned dstCapacity, unsigned * dstCount)
{
  *dstCount = 0;

  const LONG frameWidth  = (LONG)this->ivshmemWidth;
  const LONG frameHeight = (LONG)this->ivshmemHeight;

  for(const RECT * src = srcRects; src < srcRects + srcCount; ++src)
  {
    const LONG left   = max(0, min(src->left  , frameWidth ));
    const LONG top    = max(0, min(src->top   , frameHeight));
    const LONG right  = max(0, min(src->right , frameWidth ));
    const LONG bottom = max(0, min(src->bottom, frameHeight));
    if (right <= left || bottom <= top)
      continue;

    if (*dstCount + 2 > dstCapacity)
      return false;

    const LONG yLeft  = left / 4;
    const LONG yRight = (right + 3) / 4;
    dstRects[(*dstCount)++] = (RECT)
    {
      .left   = yLeft,
      .top    = top,
      .right  = yRight,
      .bottom = bottom,
    };

    const LONG pairStart = left / 2;
    const LONG pairEnd   = (right + 1) / 2;
    const LONG uvLeft    = (pairStart * 2) / 4;
    const LONG uvRight   = (pairEnd * 2 + 3) / 4;
    const LONG uvTop     = frameHeight + top / 2;
    const LONG uvBottom  = frameHeight + (bottom + 1) / 2;
    dstRects[(*dstCount)++] = (RECT)
    {
      .left   = uvLeft,
      .top    = uvTop,
      .right  = uvRight,
      .bottom = uvBottom,
    };
  }

  return *dstCount > 0;
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
  const RECT * copyRects = dst->dirtyRects;
  unsigned copyRectCount = dst->nbDirtyRects;

  RECT packedYuvRects[D12_MAX_DIRTY_RECTS * 2];
  unsigned packedYuvRectCount = 0;
  if (!copyFull && wgc_isPackedYuvIvshmem(this))
  {
    if (wgc_mapPackedYuvCopyRects(this, copyRects, copyRectCount,
        packedYuvRects, ARRAY_LENGTH(packedYuvRects), &packedYuvRectCount))
    {
      copyRects = packedYuvRects;
      copyRectCount = packedYuvRectCount;
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
  const bool needsEncode = wgc_needsEncodeShader(this);
  if (needsEncode)
    activeQueues = 1;

  // alternate between the per-queue command groups so this frame can record
  // while the previous frame's copy is still executing on the GPU
  this->d3d12CopyGroupIndex ^= 1;
  const unsigned group = this->d3d12CopyGroupIndex;

  for(unsigned i = 0; i < activeQueues; ++i)
  {
    if (!this->d3d12CopyCommandReady[i] || !this->d3d12CopyQueues[i])
      return false;

    D12CommandGroup * cmd = &this->d3d12CopyCommands[i][group];
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
    D12CommandGroup * cmd = &this->d3d12CopyCommands[i][group];

    if (needsEncode)
    {
      const D3D12_RESOURCE_BARRIER barriers[2] =
      {
        {
          .Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags      = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition =
          {
            .pResource   = *dst->bridge12,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COMMON,
            .StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE
          }
        },
        {
          .Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags      = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition =
          {
            .pResource   = *dst->ivshmemD12Res,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COMMON,
            .StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST
          }
        }
      };
      ID3D12GraphicsCommandList_ResourceBarrier(*cmd->gfxList,
        ARRAY_LENGTH(barriers), barriers);
    }

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

    if (needsEncode)
    {
      const D3D12_RESOURCE_BARRIER barriers[2] =
      {
        {
          .Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags      = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition =
          {
            .pResource   = *dst->bridge12,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE,
            .StateAfter  = D3D12_RESOURCE_STATE_COMMON
          }
        },
        {
          .Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags      = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition =
          {
            .pResource   = *dst->ivshmemD12Res,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
            .StateAfter  = D3D12_RESOURCE_STATE_COMMON
          }
        }
      };
      ID3D12GraphicsCommandList_ResourceBarrier(*cmd->gfxList,
        ARRAY_LENGTH(barriers), barriers);
    }
  }

  bool executed = true;
  const uint64_t d3d12SubmitStartUs = microtime();
  for(unsigned i = 0; i < activeQueues; ++i)
  {
    executed &= d12_commandGroupExecute(this->d3d12CopyQueues[i],
      &this->d3d12CopyCommands[i][group]);
    dst->d3d12CopyFenceValue[i] = this->d3d12CopyCommands[i][group].fenceValue;
  }
  const uint64_t d3d12SubmitUs = microtime() - d3d12SubmitStartUs;
  wgcStats_recordD3D12CopySubmit(&this->stats, d3d12SubmitUs);
  if (!executed)
  {
    memset(dst->d3d12CopyFenceValue, 0, sizeof(dst->d3d12CopyFenceValue));
    dst->d3d12CopyQueueCount = 0;
    return false;
  }

  dst->d3d12CopyQueueCount = activeQueues;
  dst->d3d12CopyGroup      = group;
  return true;
}

static void wgc_waitFrameD3D12Copy(WGCInstance * this, WGCFrameInfo * frame)
{
  const unsigned count = min(frame->d3d12CopyQueueCount,
    WGC_D3D12_COPY_QUEUE_MAX);
  if (!count)
    return;

  const unsigned group = frame->d3d12CopyGroup % WGC_D3D12_COPY_GROUPS;
  const uint64_t fenceWaitStartUs = microtime();
  for(unsigned i = 0; i < count; ++i)
  {
    const UINT64 fenceValue = frame->d3d12CopyFenceValue[i];
    if (!fenceValue || !this->d3d12CopyCommandReady[i])
      continue;

    D12CommandGroup * cmd = &this->d3d12CopyCommands[i][group];
    if (ID3D12Fence_GetCompletedValue(*cmd->fence) >= fenceValue)
      continue;

    // wait on the frame's own event, NOT cmd->event: the producer may be in
    // d12_commandGroupWait on this group's fence at the same time and an
    // auto-reset event shared between two waiters can lose a wakeup.
    // SetEventOnCompletion with a NULL event blocks until completion.
    ID3D12Fence_SetEventOnCompletion(*cmd->fence, fenceValue,
      frame->d3d12CopyEvent);
    if (frame->d3d12CopyEvent)
      WaitForSingleObject(frame->d3d12CopyEvent, INFINITE);
  }
  const uint64_t fenceWaitUs = microtime() - fenceWaitStartUs;
  wgcStats_recordD3D12FenceWait(&this->stats, fenceWaitUs);

  memset(frame->d3d12CopyFenceValue, 0, sizeof(frame->d3d12CopyFenceValue));
  frame->d3d12CopyQueueCount = 0;
}

static void wgc_drainGpuWork(WGCInstance * this)
{
  if (this->frames)
    for(unsigned i = 0; i < this->frameCount; ++i)
      wgc_waitFrameD3D12Copy(this, &this->frames[i]);

  for(unsigned i = 0; i < WGC_D3D12_COPY_QUEUE_MAX; ++i)
    if (this->d3d12CopyCommandReady[i])
      for(unsigned g = 0; g < WGC_D3D12_COPY_GROUPS; ++g)
        d12_commandGroupWait(&this->d3d12CopyCommands[i][g]);

  if (this->context && *this->context)
    ID3D11DeviceContext4_Flush(*this->context);
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

  const uint64_t framePixels =
    (uint64_t)frame->format.Width * frame->format.Height;
  const bool fullCopy = frame->fullCopy ||
    (publishCopy && this->d3d12FullCopyAlways) ||
    (publishCopy && this->dirtyFullCopyPercent > 0 &&
     frame->nbDirtyRects > 0 &&
     pixels * 100 >= framePixels * (uint64_t)this->dirtyFullCopyPercent);

  wgcStats_recordCopy(&this->stats, publishCopy, fullCopy, pixels,
    framePixels);
}

// IVSHMEM publish: create a ROW_MAJOR TEXTURE2D placed in the IVSHMEM heap
// at the slot's offset (frame->ivshmemD12Res), plus the shared D3D11 bridge
// texture WGC copies into and its D3D12 view (bridge12) that the copy queue
// reads from.
static bool wgc_ensureFrameIvshmem(WGCInstance * this, WGCFrameInfo * frame,
  const D3D11_TEXTURE2D_DESC * srcDesc)
{
  const unsigned frameIndex = (unsigned)(frame - this->frames);

  if (!this->ivshmemEnvReady)
  {
    DEBUG_ERROR("wgc_ensureFrameIvshmem called but ivshmem environment "
      "not set up (call wgc_setIvshmemD3D12CopyEnv first)");
    return false;
  }

  if (!frame->ivshmemSlotReady)
  {
    // The caller (top-level interface) hasn't registered this slot's offset
    // yet. Caller is expected to call wgc_setIvshmemSlot during the first
    // iface->capture(idx, ...) for each frameBufferIndex, before WGC tries
    // to publish into the slot.
    DEBUG_ERROR("wgc_ensureFrameIvshmem: slot offset not registered");
    return false;
  }

  const bool packedYuv = wgc_isPackedYuvIvshmem(this);
  if (srcDesc->Width  != this->ivshmemWidth ||
      srcDesc->Height != this->ivshmemHeight ||
      (!packedYuv && srcDesc->Format != this->ivshmemFormat))
  {
    DEBUG_ERROR("WGC source (%ux%u fmt 0x%x) does not match the IVSHMEM "
      "target (%ux%u fmt 0x%x). Cannot recover without renegotiating the "
      "slot offsets — fall back to a different publishMode for now.",
      srcDesc->Width, srcDesc->Height, srcDesc->Format,
      this->ivshmemWidth, this->ivshmemHeight, this->ivshmemFormat);
    return false;
  }

  comRef_scopePush(7);
  comRef_defineLocal(ID3D12Resource , placed );
  comRef_defineLocal(ID3D11Texture2D, bridgeA);
  comRef_defineLocal(ID3D11Texture2D, bridgeB);
  comRef_defineLocal(ID3D12Resource , bridge12);
  comRef_defineLocal(ID3D11UnorderedAccessView, encodeUav);

  D3D12_RESOURCE_DESC d12Desc =
  {
    .Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Alignment          = 0,
    .Width              = packedYuv ?
      wgc_nv12EncodedWidth(this->ivshmemWidth) : this->ivshmemWidth,
    .Height             = packedYuv ?
      wgc_nv12StorageHeight(this->ivshmemHeight) : this->ivshmemHeight,
    .DepthOrArraySize   = 1,
    .MipLevels          = 1,
    .Format             = packedYuv ? this->ivshmemFormat : srcDesc->Format,
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
    DEBUG_WINERROR("CreatePlacedResource (ivshmem) failed", hr);
    comRef_scopePop();
    return false;
  }
  wgc_setD3D12ObjectNameI((ID3D12Object *)*placed,
    "WGC IVSHMEM placed frame slot ", frameIndex);

  // Stage the data through a shared bridge texture: WGC source → bridgeA
  // (D3D11) → fence → bridge12 (the same memory, opened on the D3D12 side)
  // → placed (IVSHMEM) on the D3D12 copy queue.
  D3D11_TEXTURE2D_DESC bridgeDesc =
  {
    .Width          = (UINT)d12Desc.Width,
    .Height         = d12Desc.Height,
    .MipLevels      = 1,
    .ArraySize      = 1,
    .Format         = d12Desc.Format,
    .SampleDesc     = { .Count = 1, .Quality = 0 },
    .Usage          = D3D11_USAGE_DEFAULT,
    .BindFlags      = D3D11_BIND_SHADER_RESOURCE,
    .CPUAccessFlags = 0,
    .MiscFlags      = D3D11_RESOURCE_MISC_SHARED |
                      D3D11_RESOURCE_MISC_SHARED_NTHANDLE
  };

  // The encode shader writes the shared bridge directly so the packed frame
  // does not need an extra full-frame copy. Some drivers may refuse a UAV
  // bind on a shared texture; fall back to a separate encode target that is
  // copied into bridgeA each frame.
  bool encodeOnBridgeA = wgc_needsEncodeShader(this);
  if (encodeOnBridgeA)
    bridgeDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

  hr = ID3D11Device5_CreateTexture2D(*this->device, &bridgeDesc, NULL,
    bridgeA);
  if (FAILED(hr) && encodeOnBridgeA)
  {
    DEBUG_WINERROR("ivshmem-d3d12-copy: shared bridge with UAV failed, "
      "falling back to a separate encode texture", hr);
    encodeOnBridgeA = false;
    bridgeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device5_CreateTexture2D(*this->device, &bridgeDesc, NULL,
      bridgeA);
  }
  if (FAILED(hr))
  {
    DEBUG_WINERROR("ivshmem-d3d12-copy: CreateTexture2D bridgeA failed", hr);
    comRef_scopePop();
    return false;
  }

  if (wgc_needsEncodeShader(this))
  {
    if (!encodeOnBridgeA)
    {
      D3D11_TEXTURE2D_DESC encodeDesc = bridgeDesc;
      encodeDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      encodeDesc.MiscFlags = 0;
      hr = ID3D11Device5_CreateTexture2D(*this->device, &encodeDesc, NULL,
        bridgeB);
      if (FAILED(hr))
      {
        DEBUG_WINERROR("ivshmem-d3d12-copy: CreateTexture2D encode "
          "bridge failed", hr);
        comRef_scopePop();
        return false;
      }
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc =
    {
      .Format        = this->ivshmemFormat,
      .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
      .Texture2D     = { .MipSlice = 0 }
    };
    hr = ID3D11Device5_CreateUnorderedAccessView(*this->device,
      (ID3D11Resource *)(encodeOnBridgeA ? *bridgeA : *bridgeB),
      &uavDesc, encodeUav);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("ivshmem-d3d12-copy: CreateUnorderedAccessView encode "
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
    DEBUG_WINERROR("ivshmem-d3d12-copy: bridgeA QI IDXGIResource1 failed", hr);
    comRef_scopePop();
    return false;
  }

  HANDLE bridgeHandle = NULL;
  hr = IDXGIResource1_CreateSharedHandle(*bridgeRes, NULL, GENERIC_ALL, NULL,
    &bridgeHandle);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("ivshmem-d3d12-copy: bridgeA CreateSharedHandle failed", hr);
    comRef_scopePop();
    return false;
  }

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

  comRef_toGlobal(frame->ivshmemD12Res, placed );
  comRef_toGlobal(frame->bridgeA , bridgeA );
  if (wgc_needsEncodeShader(this))
  {
    if (!encodeOnBridgeA)
      comRef_toGlobal(frame->bridgeB, bridgeB);
    comRef_toGlobal(frame->encodeUav, encodeUav);
  }
  comRef_toGlobal(frame->bridge12, bridge12);
  memcpy(&frame->format, srcDesc, sizeof(frame->format));
  if (packedYuv)
  {
    frame->format.Width  = wgc_nv12EncodedWidth(this->ivshmemWidth);
    frame->format.Height = wgc_nv12StorageHeight(this->ivshmemHeight);
    frame->format.Format = this->ivshmemFormat;
  }
  frame->ready = true;
  comRef_scopePop();
  return true;
}

static bool wgc_ensureFrame(WGCInstance * this, WGCFrameInfo * frame,
  ID3D11Texture2D * src)
{
  D3D11_TEXTURE2D_DESC srcDesc;
  ID3D11Texture2D_GetDesc(src, &srcDesc);
  const bool ivshmemSlot =
    wgc_isIvshmemPublishMode(this->publishMode) &&
    frame != &this->frames[0];
  const bool packedYuvSlot = ivshmemSlot &&
    wgc_isPackedYuvIvshmem(this);

  if (frame->ready && (
      (!packedYuvSlot &&
       frame->format.Width  == srcDesc.Width  &&
       frame->format.Height == srcDesc.Height &&
       frame->format.Format == srcDesc.Format) ||
      (packedYuvSlot &&
       frame->format.Width  == wgc_nv12EncodedWidth(this->ivshmemWidth) &&
       frame->format.Height == wgc_nv12StorageHeight(this->ivshmemHeight) &&
       frame->format.Format == this->ivshmemFormat)))
    return true;

  frame->ready = false;

  if (wgc_frameHasResources(frame))
    wgc_releaseFrameResources(frame);

  // IVSHMEM path: only publish slots (not the accumulator) live in
  // IVSHMEM. Accumulator stays VRAM-local in this mode.
  if (ivshmemSlot)
    return wgc_ensureFrameIvshmem(this, frame, &srcDesc);

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
  else
  {
    // Accumulator: GPU-local copy-only texture. It is never mapped by the
    // CPU; only the final publish slot is staging/readback (CPU_STAGING) or
    // IVSHMEM-resident (IVSHMEM modes).
    dstDesc.Usage          = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags      = wgc_needsEncodeShader(this) ?
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

  frame->ready = true;
  return true;
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

// Public WGC API used by the standalone Capture_WGC interface.

bool wgc_createInstance(WGCInstance ** out, unsigned frameBuffers,
  WGCPublishMode mode)
{
  if (!wgc_create(out, frameBuffers))
    return false;
  (*out)->publishMode = mode;
  return true;
}

bool wgc_initInstance(WGCInstance * this, bool debug,
  IDXGIAdapter1 * adapter, IDXGIOutput * output, bool trackDamage)
{
  this->trackDamage = trackDamage;
  return wgc_init(this, debug, adapter, output);
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

bool wgc_setIvshmemD3D12CopyEnv(WGCInstance * this,
  IUnknown * ivshmemHeap,
  IUnknown * d3d12Queue,
  unsigned   width,
  unsigned   height,
  unsigned   format)
{
  if (!this || !ivshmemHeap || !d3d12Queue)
    return false;

  const DXGI_FORMAT oldFormat = this->ivshmemFormat;
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
  if (oldFormat != this->ivshmemFormat)
    wgc_invalidateNV12Shader(this);
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
    // Offset changed for an already-ready slot. Drop the placed resource +
    // bridges so wgc_ensureFrame recreates them at the new offset.
    if (wgc_frameHasResources(frame))
      wgc_releaseFrameResources(frame);
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
  return wgc_deinit(this);
}

void wgc_freeInstance(WGCInstance ** this)
{
  if (!this || !*this)
    return;
  wgc_free(this);
}

CaptureResult wgc_pollFrame(WGCInstance * this, unsigned frameBufferIndex)
{
  return wgc_capture(this, frameBufferIndex);
}

bool wgc_waitPublish(WGCInstance * this, unsigned timeoutMs)
{
  if (!this || !this->publishEvent)
    return false;
  return WaitForSingleObject(this->publishEvent, timeoutMs) == WAIT_OBJECT_0;
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
  HRESULT hr = ID3D11DeviceContext4_Map(*this->context,
    (ID3D11Resource *)*frame->texture, 0, D3D11_MAP_READ, 0, &mapped);
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
  wgc_releaseSlot(this, token);
}

// IVSHMEM GPU-publish: WGC has already written pixels to IVSHMEM via the
// D3D12 copy queue. The consumer doesn't need to map anything — it just
// needs the metadata (dirty rects, dimensions) and to know which IVSHMEM
// slot was written.
//
// Returns the slot's ivshmem offset via *ivshmemOffset on success. width,
// height, pitch describe the dimensions and row stride. The state machine
// transitions are identical to wgc_fetchCpu — no Map/Unmap involved.
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

  // The producer already signaled and flushed the D3D11 bridge copy before
  // submitting the D3D12 copy; once the copy-queue fence has signaled the
  // IVSHMEM bytes are committed, so no D3D11 Flush is needed here (and the
  // frame thread must not touch the D3D11 context).
  wgc_waitFrameD3D12Copy(this, frame);

  *ivshmemOffset = frame->ivshmemOffset;
  // For row_major TEXTURE2D placed resources, the natural pitch is
  // width * bpp aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256). We
  // recover bpp from the format.
  const unsigned bpp =
    (frame->format.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
     frame->format.Format == DXGI_FORMAT_R16G16B16A16_UINT) ? 8 : 4;
  *pitch  = (frame->format.Width * bpp + 255u) & ~255u;
  *width  = wgc_isPackedYuvIvshmem(this) ?
    this->ivshmemWidth : frame->format.Width;
  *height = wgc_isPackedYuvIvshmem(this) ?
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
  wgc_releaseSlot(this, token);
}
