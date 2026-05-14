/**
 * Looking Glass
 * Copyright (C) 2017-2025 The Looking Glass Authors
 * https://looking-glass.io
 *
 * Standalone repro for Windows Graphics Capture when the WinRT Direct3D
 * device is backed by a D3D11On12 device.
 *
 * Usage:
 *   test-wgc-d3d11on12-framepool.exe [--seconds N] [--output N]
 *                                    [--no-acquire] [--list]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <initguid.h>

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Graphics
#define WIDL_using_Windows_Graphics_Capture
#define WIDL_using_Windows_Graphics_DirectX
#define WIDL_using_Windows_Graphics_DirectX_Direct3D11

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
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
#endif

#ifndef ID3D11On12Device_Release
#define ID3D11On12Device_Release(This) (This)->lpVtbl->Release(This)
#endif

#define FRAME_POOL_BUFFERS 2

typedef struct Options
{
  int  seconds;
  int  outputIndex;
  bool acquire;
  bool listOnly;
}
Options;

typedef struct CaptureOutput
{
  IDXGIAdapter1      * adapter;
  IDXGIOutput        * output;
  DXGI_ADAPTER_DESC1   adapterDesc;
  DXGI_OUTPUT_DESC     outputDesc;
  UINT                 adapterIndex;
  UINT                 outputIndex;
  UINT                 globalIndex;
  bool                 primary;
}
CaptureOutput;

typedef struct FrameHandler
{
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable base;
  volatile LONG refs;
  volatile LONG activeCallbacks;
  volatile LONG stopping;
  bool acquire;

  volatile LONG64 callbacks;
  volatile LONG64 framesAcquired;
  volatile LONG64 emptyAcquires;
  volatile LONG64 acquireFailures;
  volatile LONG64 firstTick;
  volatile LONG64 lastTick;
  volatile LONG64 totalGapTicks;
  volatile LONG64 maxGapTicks;
  volatile LONG64 gapCount;
  volatile LONG lastWidth;
  volatile LONG lastHeight;
}
FrameHandler;

static void printUsage(FILE * out);
static bool parseOptions(int argc, char ** argv, Options * options);
static bool selectOutput(int requestedOutput, bool listOnly,
  CaptureOutput * selected);
static void releaseOutput(CaptureOutput * output);
static void printSelectedOutput(const CaptureOutput * output);
static HRESULT getActivationFactory(const WCHAR * runtimeClass, REFIID iid,
  void ** factory);
static void closeInspectable(IInspectable * obj);
static void releaseFrame(IDirect3D11CaptureFrame ** frame);
static void interlockedMax64(volatile LONG64 * target, LONG64 value);
static void waitCallbacks(FrameHandler * handler);
static void printStats(const Options * options, const FrameHandler * handler,
  LARGE_INTEGER qpcFreq);
static void printHr(const char * what, HRESULT hr);

static HRESULT STDMETHODCALLTYPE frameHandlerQueryInterface(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface,
  REFIID riid,
  void ** obj);
static ULONG STDMETHODCALLTYPE frameHandlerAddRef(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface);
static ULONG STDMETHODCALLTYPE frameHandlerRelease(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface);
static HRESULT STDMETHODCALLTYPE frameHandlerInvoke(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface,
  IDirect3D11CaptureFramePool * sender,
  IInspectable * args);

static ITypedEventHandler_Direct3D11CaptureFramePool_IInspectableVtbl
  frameHandlerVtbl =
{
  .QueryInterface = frameHandlerQueryInterface,
  .AddRef         = frameHandlerAddRef,
  .Release        = frameHandlerRelease,
  .Invoke         = frameHandlerInvoke
};

static void printUsage(FILE * out)
{
  fprintf(out,
    "Usage: test-wgc-d3d11on12-framepool.exe [options]\n"
    "\n"
    "Options:\n"
    "  --seconds N    Capture duration, default 15 seconds\n"
    "  --output N     Use output index N from --list, default primary monitor\n"
    "  --no-acquire   Count FrameArrived callbacks without draining frames\n"
    "  --list         List DXGI outputs and exit\n"
    "  --help         Show this help\n");
}

static bool parseIntArg(const char * name, const char * value, int * out)
{
  char * end = NULL;
  const long parsed = strtol(value, &end, 10);
  if (!value[0] || (end && *end))
  {
    fprintf(stderr, "Invalid %s value: %s\n", name, value);
    return false;
  }

  *out = (int)parsed;
  return true;
}

static bool parseOptions(int argc, char ** argv, Options * options)
{
  options->seconds     = 15;
  options->outputIndex = -1;
  options->acquire     = true;
  options->listOnly    = false;

  for(int i = 1; i < argc; ++i)
  {
    if (strcmp(argv[i], "--seconds") == 0)
    {
      if (++i >= argc || !parseIntArg("--seconds", argv[i],
          &options->seconds))
        return false;
    }
    else if (strcmp(argv[i], "--output") == 0)
    {
      if (++i >= argc || !parseIntArg("--output", argv[i],
          &options->outputIndex))
        return false;
    }
    else if (strcmp(argv[i], "--no-acquire") == 0)
      options->acquire = false;
    else if (strcmp(argv[i], "--list") == 0)
      options->listOnly = true;
    else if (strcmp(argv[i], "--help") == 0 ||
             strcmp(argv[i], "-h") == 0)
    {
      printUsage(stdout);
      exit(0);
    }
    else
    {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      printUsage(stderr);
      return false;
    }
  }

  if (options->seconds < 1 || options->seconds > 120)
  {
    fprintf(stderr, "--seconds must be between 1 and 120\n");
    return false;
  }

  if (options->outputIndex < -1)
  {
    fprintf(stderr, "--output must be >= 0\n");
    return false;
  }

  return true;
}

static void captureOutputStore(CaptureOutput * dst, IDXGIAdapter1 * adapter,
  IDXGIOutput * output, const DXGI_ADAPTER_DESC1 * adapterDesc,
  const DXGI_OUTPUT_DESC * outputDesc, UINT adapterIndex, UINT outputIndex,
  UINT globalIndex, bool primary)
{
  releaseOutput(dst);
  IDXGIAdapter1_AddRef(adapter);
  IDXGIOutput_AddRef(output);
  dst->adapter      = adapter;
  dst->output       = output;
  dst->adapterDesc  = *adapterDesc;
  dst->outputDesc   = *outputDesc;
  dst->adapterIndex = adapterIndex;
  dst->outputIndex  = outputIndex;
  dst->globalIndex  = globalIndex;
  dst->primary      = primary;
}

static bool selectOutput(int requestedOutput, bool listOnly,
  CaptureOutput * selected)
{
  IDXGIFactory1 * factory = NULL;
  HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
  if (FAILED(hr))
  {
    printHr("CreateDXGIFactory1", hr);
    return false;
  }

  const POINT origin = { 0, 0 };
  const HMONITOR primaryMonitor = MonitorFromPoint(origin,
    MONITOR_DEFAULTTOPRIMARY);
  CaptureOutput fallback = { 0 };
  bool found = false;
  UINT globalIndex = 0;

  for(UINT adapterIndex = 0; !found; ++adapterIndex)
  {
    IDXGIAdapter1 * adapter = NULL;
    hr = IDXGIFactory1_EnumAdapters1(factory, adapterIndex, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    if (FAILED(hr))
    {
      printHr("IDXGIFactory1::EnumAdapters1", hr);
      break;
    }

    DXGI_ADAPTER_DESC1 adapterDesc;
    hr = IDXGIAdapter1_GetDesc1(adapter, &adapterDesc);
    if (FAILED(hr) || (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
    {
      IDXGIAdapter1_Release(adapter);
      continue;
    }

    for(UINT outputIndex = 0; ; ++outputIndex)
    {
      IDXGIOutput * output = NULL;
      hr = IDXGIAdapter1_EnumOutputs(adapter, outputIndex, &output);
      if (hr == DXGI_ERROR_NOT_FOUND)
        break;
      if (FAILED(hr))
      {
        printHr("IDXGIAdapter1::EnumOutputs", hr);
        break;
      }

      DXGI_OUTPUT_DESC outputDesc;
      hr = IDXGIOutput_GetDesc(output, &outputDesc);
      if (SUCCEEDED(hr))
      {
        const bool primary = outputDesc.Monitor == primaryMonitor;

        if (listOnly)
        {
          wprintf(L"[%u] adapter %u: %ls | output %u: %ls "
            L"rect=(%ld,%ld)-(%ld,%ld)%ls\n",
            globalIndex, adapterIndex, adapterDesc.Description, outputIndex,
            outputDesc.DeviceName,
            outputDesc.DesktopCoordinates.left,
            outputDesc.DesktopCoordinates.top,
            outputDesc.DesktopCoordinates.right,
            outputDesc.DesktopCoordinates.bottom,
            primary ? L" primary" : L"");
        }

        if (!fallback.output)
          captureOutputStore(&fallback, adapter, output, &adapterDesc,
            &outputDesc, adapterIndex, outputIndex, globalIndex, primary);

        if (!listOnly &&
            ((requestedOutput >= 0 && globalIndex == (UINT)requestedOutput) ||
             (requestedOutput < 0 && primary)))
        {
          captureOutputStore(selected, adapter, output, &adapterDesc,
            &outputDesc, adapterIndex, outputIndex, globalIndex, primary);
          found = true;
        }

        ++globalIndex;
      }

      IDXGIOutput_Release(output);
      if (found)
        break;
    }

    IDXGIAdapter1_Release(adapter);
  }

  IDXGIFactory1_Release(factory);

  if (listOnly)
  {
    releaseOutput(&fallback);
    return globalIndex > 0;
  }

  if (!found && requestedOutput < 0 && fallback.output)
  {
    *selected = fallback;
    memset(&fallback, 0, sizeof(fallback));
    found = true;
    fprintf(stderr, "Primary monitor was not found in DXGI outputs; "
      "falling back to output 0\n");
  }

  if (!found && requestedOutput >= 0)
    fprintf(stderr, "Output index %d was not found\n", requestedOutput);

  releaseOutput(&fallback);
  return found;
}

static void releaseOutput(CaptureOutput * output)
{
  if (output->output)
  {
    IDXGIOutput_Release(output->output);
    output->output = NULL;
  }

  if (output->adapter)
  {
    IDXGIAdapter1_Release(output->adapter);
    output->adapter = NULL;
  }
}

static void printSelectedOutput(const CaptureOutput * output)
{
  wprintf(L"Selected output [%u]: adapter %u: %ls | output %u: %ls "
    L"rect=(%ld,%ld)-(%ld,%ld)%ls\n",
    output->globalIndex, output->adapterIndex, output->adapterDesc.Description,
    output->outputIndex, output->outputDesc.DeviceName,
    output->outputDesc.DesktopCoordinates.left,
    output->outputDesc.DesktopCoordinates.top,
    output->outputDesc.DesktopCoordinates.right,
    output->outputDesc.DesktopCoordinates.bottom,
    output->primary ? L" primary" : L"");
}

static HRESULT getActivationFactory(const WCHAR * runtimeClass, REFIID iid,
  void ** factory)
{
  HSTRING className = NULL;
  HRESULT hr = WindowsCreateString(runtimeClass, (UINT32)wcslen(runtimeClass),
    &className);
  if (FAILED(hr))
    return hr;

  hr = RoGetActivationFactory(className, iid, factory);
  WindowsDeleteString(className);
  return hr;
}

static void closeInspectable(IInspectable * obj)
{
  if (!obj)
    return;

  IClosable * closable = NULL;
  HRESULT hr = IInspectable_QueryInterface(obj, &IID_IClosable,
    (void **)&closable);
  if (SUCCEEDED(hr))
  {
    IClosable_Close(closable);
    IClosable_Release(closable);
  }
}

static void releaseFrame(IDirect3D11CaptureFrame ** frame)
{
  if (!frame || !*frame)
    return;

  closeInspectable((IInspectable *)*frame);
  IDirect3D11CaptureFrame_Release(*frame);
  *frame = NULL;
}

static void interlockedMax64(volatile LONG64 * target, LONG64 value)
{
  LONG64 current = InterlockedCompareExchange64(target, 0, 0);
  while(value > current &&
    InterlockedCompareExchange64(target, value, current) != current)
    current = InterlockedCompareExchange64(target, 0, 0);
}

static HRESULT STDMETHODCALLTYPE frameHandlerQueryInterface(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface,
  REFIID riid,
  void ** obj)
{
  if (!obj)
    return E_POINTER;

  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IAgileObject) ||
      IsEqualGUID(riid,
        &IID_ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable))
  {
    *obj = iface;
    frameHandlerAddRef(iface);
    return S_OK;
  }

  *obj = NULL;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE frameHandlerAddRef(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface)
{
  FrameHandler * handler = (FrameHandler *)iface;
  return (ULONG)InterlockedIncrement(&handler->refs);
}

static ULONG STDMETHODCALLTYPE frameHandlerRelease(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface)
{
  FrameHandler * handler = (FrameHandler *)iface;
  const ULONG refs = (ULONG)InterlockedDecrement(&handler->refs);
  if (refs == 0)
    free(handler);

  return refs;
}

static HRESULT STDMETHODCALLTYPE frameHandlerInvoke(
  ITypedEventHandler_Direct3D11CaptureFramePool_IInspectable * iface,
  IDirect3D11CaptureFramePool * sender,
  IInspectable * args)
{
  (void)args;

  FrameHandler * handler = (FrameHandler *)iface;
  InterlockedIncrement(&handler->activeCallbacks);

  if (!InterlockedCompareExchange(&handler->stopping, 0, 0))
  {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    const LONG64 previous = InterlockedExchange64(&handler->lastTick,
      now.QuadPart);
    if (previous == 0)
      InterlockedCompareExchange64(&handler->firstTick, now.QuadPart, 0);
    else if (now.QuadPart > previous)
    {
      const LONG64 gap = now.QuadPart - previous;
      InterlockedExchangeAdd64(&handler->totalGapTicks, gap);
      InterlockedIncrement64(&handler->gapCount);
      interlockedMax64(&handler->maxGapTicks, gap);
    }

    InterlockedIncrement64(&handler->callbacks);

    if (handler->acquire)
    {
      for(;;)
      {
        IDirect3D11CaptureFrame * frame = NULL;
        HRESULT hr = IDirect3D11CaptureFramePool_TryGetNextFrame(sender,
          &frame);
        if (FAILED(hr))
        {
          InterlockedIncrement64(&handler->acquireFailures);
          break;
        }

        if (!frame)
        {
          InterlockedIncrement64(&handler->emptyAcquires);
          break;
        }

        SizeInt32 size;
        if (SUCCEEDED(IDirect3D11CaptureFrame_get_ContentSize(frame, &size)))
        {
          InterlockedExchange(&handler->lastWidth , size.Width );
          InterlockedExchange(&handler->lastHeight, size.Height);
        }

        InterlockedIncrement64(&handler->framesAcquired);
        releaseFrame(&frame);
      }
    }
  }

  InterlockedDecrement(&handler->activeCallbacks);
  return S_OK;
}

static void waitCallbacks(FrameHandler * handler)
{
  for(unsigned i = 0; i < 5000 &&
      InterlockedCompareExchange(&handler->activeCallbacks, 0, 0) > 0;
      ++i)
    Sleep(1);

  if (InterlockedCompareExchange(&handler->activeCallbacks, 0, 0) > 0)
    fprintf(stderr, "Timed out waiting for WGC callbacks to finish\n");
}

static double ticksToMs(LONG64 ticks, LARGE_INTEGER qpcFreq)
{
  return (double)ticks * 1000.0 / (double)qpcFreq.QuadPart;
}

static void printStats(const Options * options, const FrameHandler * handler,
  LARGE_INTEGER qpcFreq)
{
  const LONG64 callbacks = InterlockedCompareExchange64(
    (volatile LONG64 *)&handler->callbacks, 0, 0);
  const LONG64 framesAcquired = InterlockedCompareExchange64(
    (volatile LONG64 *)&handler->framesAcquired, 0, 0);
  const LONG64 emptyAcquires = InterlockedCompareExchange64(
    (volatile LONG64 *)&handler->emptyAcquires, 0, 0);
  const LONG64 acquireFailures = InterlockedCompareExchange64(
    (volatile LONG64 *)&handler->acquireFailures, 0, 0);
  const LONG64 firstTick = InterlockedCompareExchange64(
    (volatile LONG64 *)&handler->firstTick, 0, 0);
  const LONG64 lastTick = InterlockedCompareExchange64(
    (volatile LONG64 *)&handler->lastTick, 0, 0);
  const LONG64 gapCount = InterlockedCompareExchange64(
    (volatile LONG64 *)&handler->gapCount, 0, 0);
  const LONG64 totalGapTicks = InterlockedCompareExchange64(
    (volatile LONG64 *)&handler->totalGapTicks, 0, 0);
  const LONG64 maxGapTicks = InterlockedCompareExchange64(
    (volatile LONG64 *)&handler->maxGapTicks, 0, 0);
  const LONG lastWidth = InterlockedCompareExchange(
    (volatile LONG *)&handler->lastWidth, 0, 0);
  const LONG lastHeight = InterlockedCompareExchange(
    (volatile LONG *)&handler->lastHeight, 0, 0);

  printf("\n== WGC + D3D11On12 frame-pool results ==\n");
  printf("duration_seconds=%d\n", options->seconds);
  printf("acquire_frames=%s\n", options->acquire ? "yes" : "no");
  printf("callbacks=%lld\n", (long long)callbacks);
  printf("frames_acquired=%lld\n", (long long)framesAcquired);
  printf("empty_acquires=%lld\n", (long long)emptyAcquires);
  printf("acquire_failures=%lld\n", (long long)acquireFailures);
  if (lastWidth > 0 && lastHeight > 0)
    printf("last_frame_size=%ldx%ld\n", lastWidth, lastHeight);

  if (callbacks > 1 && firstTick > 0 && lastTick > firstTick)
  {
    const double activeSeconds =
      (double)(lastTick - firstTick) / (double)qpcFreq.QuadPart;
    printf("callback_rate_active=%.3f/s\n",
      (double)(callbacks - 1) / activeSeconds);
  }

  if (gapCount > 0)
  {
    printf("callback_gap_avg_ms=%.3f\n",
      ticksToMs(totalGapTicks / gapCount, qpcFreq));
    printf("callback_gap_max_ms=%.3f\n",
      ticksToMs(maxGapTicks, qpcFreq));
  }
}

static void printHr(const char * what, HRESULT hr)
{
  fprintf(stderr, "FAIL: %s (hr=0x%08lx)\n", what, (unsigned long)hr);
}

int main(int argc, char ** argv)
{
  Options options;
  if (!parseOptions(argc, argv, &options))
    return 1;

  CaptureOutput output = { 0 };
  if (!selectOutput(options.outputIndex, options.listOnly, &output))
    return 1;
  if (options.listOnly)
    return 0;

  printSelectedOutput(&output);

  int exitCode = 1;
  HRESULT hr;
  bool roInitialized = false;
  LARGE_INTEGER qpcFreq;
  QueryPerformanceFrequency(&qpcFreq);

  ID3D12Device * d12Device = NULL;
  ID3D12CommandQueue * d12Queue = NULL;
  ID3D11Device * d11Device = NULL;
  ID3D11DeviceContext * d11Context = NULL;
  ID3D11On12Device * d11On12 = NULL;
  IDXGIDevice * dxgiDevice = NULL;
  IInspectable * inspectableDevice = NULL;
  IDirect3DDevice * graphicsDevice = NULL;
  IGraphicsCaptureSessionStatics * sessionStatics = NULL;
  IGraphicsCaptureItemInterop * itemInterop = NULL;
  IGraphicsCaptureItem * item = NULL;
  IDirect3D11CaptureFramePoolStatics2 * framePoolStatics = NULL;
  IDirect3D11CaptureFramePool * framePool = NULL;
  IGraphicsCaptureSession * session = NULL;
  FrameHandler * handler = NULL;
  EventRegistrationToken frameArrivedToken = { 0 };

#define CHECK_HR(expr, what) do { \
  hr = (expr); \
  if (FAILED(hr)) { \
    printHr((what), hr); \
    goto cleanup; \
  } \
} while (0)

  hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
  {
    printHr("RoInitialize", hr);
    goto cleanup;
  }
  roInitialized = SUCCEEDED(hr);

  CHECK_HR(getActivationFactory(
      RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
      &IID_IGraphicsCaptureSessionStatics, (void **)&sessionStatics),
    "RoGetActivationFactory(GraphicsCaptureSession)");

  boolean supported = false;
  CHECK_HR(IGraphicsCaptureSessionStatics_IsSupported(sessionStatics,
      &supported),
    "IGraphicsCaptureSessionStatics::IsSupported");
  if (!supported)
  {
    fprintf(stderr, "FAIL: Windows Graphics Capture is not supported\n");
    goto cleanup;
  }

  CHECK_HR(D3D12CreateDevice((IUnknown *)output.adapter,
      D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&d12Device),
    "D3D12CreateDevice");

  D3D12_COMMAND_QUEUE_DESC queueDesc = { 0 };
  queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
  CHECK_HR(ID3D12Device_CreateCommandQueue(d12Device, &queueDesc,
      &IID_ID3D12CommandQueue, (void **)&d12Queue),
    "ID3D12Device::CreateCommandQueue");
  ID3D12CommandQueue_SetName(d12Queue,
    L"test-wgc-d3d11on12-framepool direct queue");
  printf("D3D12 device and direct queue created\n");

  static const D3D_FEATURE_LEVEL featureLevels[] =
  {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0
  };
  D3D_FEATURE_LEVEL featureLevel = 0;
  IUnknown * queues[] = { (IUnknown *)d12Queue };
  CHECK_HR(D3D11On12CreateDevice((IUnknown *)d12Device,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      featureLevels, 2,
      queues, 1,
      0,
      &d11Device, &d11Context, &featureLevel),
    "D3D11On12CreateDevice");
  printf("D3D11On12 device created (feature level 0x%x)\n",
    (unsigned)featureLevel);

  CHECK_HR(ID3D11Device_QueryInterface(d11Device, &IID_ID3D11On12Device,
      (void **)&d11On12),
    "QueryInterface(ID3D11On12Device)");

  CHECK_HR(ID3D11Device_QueryInterface(d11Device, &IID_IDXGIDevice,
      (void **)&dxgiDevice),
    "QueryInterface(IDXGIDevice)");

  CHECK_HR(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice,
      &inspectableDevice),
    "CreateDirect3D11DeviceFromDXGIDevice");

  CHECK_HR(IInspectable_QueryInterface(inspectableDevice,
      &IID_IDirect3DDevice, (void **)&graphicsDevice),
    "QueryInterface(IDirect3DDevice)");
  printf("WinRT Direct3D device created from the D3D11On12 DXGI device\n");

  CHECK_HR(getActivationFactory(
      RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem,
      &IID_IGraphicsCaptureItemInterop, (void **)&itemInterop),
    "RoGetActivationFactory(GraphicsCaptureItem)");

  CHECK_HR(IGraphicsCaptureItemInterop_CreateForMonitor(itemInterop,
      output.outputDesc.Monitor, &IID_IGraphicsCaptureItem, (void **)&item),
    "IGraphicsCaptureItemInterop::CreateForMonitor");

  SizeInt32 itemSize;
  CHECK_HR(IGraphicsCaptureItem_get_Size(item, &itemSize),
    "IGraphicsCaptureItem::get_Size");
  printf("WGC item created: %dx%d\n", itemSize.Width, itemSize.Height);

  CHECK_HR(getActivationFactory(
      RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool,
      &IID_IDirect3D11CaptureFramePoolStatics2,
      (void **)&framePoolStatics),
    "RoGetActivationFactory(Direct3D11CaptureFramePool)");

  CHECK_HR(IDirect3D11CaptureFramePoolStatics2_CreateFreeThreaded(
      framePoolStatics,
      graphicsDevice,
      DirectXPixelFormat_B8G8R8A8UIntNormalized,
      FRAME_POOL_BUFFERS,
      itemSize,
      &framePool),
    "IDirect3D11CaptureFramePoolStatics2::CreateFreeThreaded");

  CHECK_HR(IDirect3D11CaptureFramePool_CreateCaptureSession(framePool, item,
      &session),
    "IDirect3D11CaptureFramePool::CreateCaptureSession");

  handler = calloc(1, sizeof(*handler));
  if (!handler)
  {
    fprintf(stderr, "FAIL: out of memory\n");
    goto cleanup;
  }
  handler->base.lpVtbl = &frameHandlerVtbl;
  handler->refs        = 1;
  handler->acquire     = options.acquire;

  CHECK_HR(IDirect3D11CaptureFramePool_add_FrameArrived(framePool,
      &handler->base, &frameArrivedToken),
    "IDirect3D11CaptureFramePool::add_FrameArrived");

  CHECK_HR(IGraphicsCaptureSession_StartCapture(session),
    "IGraphicsCaptureSession::StartCapture");

  printf("Capture started for %d seconds; acquire_frames=%s\n",
    options.seconds, options.acquire ? "yes" : "no");
  Sleep((DWORD)options.seconds * 1000);

  InterlockedExchange(&handler->stopping, 1);
  if (frameArrivedToken.value)
  {
    IDirect3D11CaptureFramePool_remove_FrameArrived(framePool,
      frameArrivedToken);
    frameArrivedToken.value = 0;
  }
  waitCallbacks(handler);
  printStats(&options, handler, qpcFreq);

  const LONG64 callbacks = InterlockedCompareExchange64(&handler->callbacks,
    0, 0);
  const LONG64 framesAcquired = InterlockedCompareExchange64(
    &handler->framesAcquired, 0, 0);
  if (callbacks == 0)
  {
    fprintf(stderr, "FAIL: no FrameArrived callbacks were observed\n");
    exitCode = 2;
  }
  else if (options.acquire && framesAcquired == 0)
  {
    fprintf(stderr, "FAIL: callbacks fired, but no frames were acquired\n");
    exitCode = 3;
  }
  else
  {
    printf("PASS: FrameArrived callbacks continued on the D3D11On12-backed "
      "WGC device\n");
    exitCode = 0;
  }

cleanup:
  if (handler)
    InterlockedExchange(&handler->stopping, 1);

  if (framePool && frameArrivedToken.value)
  {
    IDirect3D11CaptureFramePool_remove_FrameArrived(framePool,
      frameArrivedToken);
    frameArrivedToken.value = 0;
  }

  if (handler)
  {
    waitCallbacks(handler);
    frameHandlerRelease(&handler->base);
    handler = NULL;
  }

  closeInspectable((IInspectable *)session);
  closeInspectable((IInspectable *)framePool);

  if (session)
    IGraphicsCaptureSession_Release(session);
  if (framePool)
    IDirect3D11CaptureFramePool_Release(framePool);
  if (framePoolStatics)
    IDirect3D11CaptureFramePoolStatics2_Release(framePoolStatics);
  if (item)
    IGraphicsCaptureItem_Release(item);
  if (itemInterop)
    IGraphicsCaptureItemInterop_Release(itemInterop);
  if (sessionStatics)
    IGraphicsCaptureSessionStatics_Release(sessionStatics);
  if (graphicsDevice)
    IDirect3DDevice_Release(graphicsDevice);
  if (inspectableDevice)
    IInspectable_Release(inspectableDevice);
  if (dxgiDevice)
    IDXGIDevice_Release(dxgiDevice);
  if (d11On12)
    ID3D11On12Device_Release(d11On12);
  if (d11Context)
    ID3D11DeviceContext_Release(d11Context);
  if (d11Device)
    ID3D11Device_Release(d11Device);
  if (d12Queue)
    ID3D12CommandQueue_Release(d12Queue);
  if (d12Device)
    ID3D12Device_Release(d12Device);
  if (roInitialized)
    RoUninitialize();
  releaseOutput(&output);
  return exitCode;

#undef CHECK_HR
}
