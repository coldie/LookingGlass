/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
 * https://looking-glass.io
 *
 * Standalone validation for the IVSHMEM-direct buffer design.
 *
 * Tests whether the host GPU can write directly into an IVSHMEM-backed
 * D3D12 placed resource via D3D11On12 interop. If this works, we can put
 * the WGC ring buffers in IVSHMEM and eliminate the second copy entirely.
 *
 * Run on the Windows guest. The same IVSHMEM device that looking-glass-host
 * uses must be available (the IVSHMEM driver must be installed). The test
 * uses IVSHMEM device 0 by default.
 *
 * Usage:
 *   test-d3d11on12-ivshmem.exe [--width N] [--height N] [--iter N]
 *                                  [--format bgra8|rgba16f]
 *
 *   --width   default 1920   horizontal pixels for the test texture
 *   --height  default 1080   vertical pixels
 *   --iter    default 100    benchmark iterations
 *   --format  default bgra8  DXGI texture format to validate
 *
 * Exit codes:
 *   0   all phases passed
 *   1   environment / setup failure (IVSHMEM open, D3D12 device, etc)
 *   2   Phase B failed (the load-bearing test — ROW_MAJOR TEXTURE2D in IVSHMEM heap)
 *   3   Phase E failed (pattern verification — GPU wrote the wrong bytes)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "common/ivshmem.h"
#include "common/option.h"
#include "common/debug.h"

// MinGW's COBJMACROS expansion for aggregate-returning vtable methods
// doesn't compile cleanly. The main host code defines an inline shim in
// d12.h; we define our own here to keep this tool self-contained.
#ifdef ID3D12Heap_GetDesc
#undef ID3D12Heap_GetDesc
static inline D3D12_HEAP_DESC ID3D12Heap_GetDesc(ID3D12Heap * This)
{
  D3D12_HEAP_DESC __ret;
  return *This->lpVtbl->GetDesc(This, &__ret);
}
#endif

// Some MinGW d3d11on12.h variants omit the ID3D11On12Device COBJMACROS
#ifndef ID3D11On12Device_CreateWrappedResource
#define ID3D11On12Device_CreateWrappedResource(This, pResource12, pFlags11, InState, OutState, riid, ppResource11) \
  (This)->lpVtbl->CreateWrappedResource(This, pResource12, pFlags11, InState, OutState, riid, ppResource11)
#define ID3D11On12Device_AcquireWrappedResources(This, ppResources, NumResources) \
  (This)->lpVtbl->AcquireWrappedResources(This, ppResources, NumResources)
#define ID3D11On12Device_ReleaseWrappedResources(This, ppResources, NumResources) \
  (This)->lpVtbl->ReleaseWrappedResources(This, ppResources, NumResources)
#define ID3D11On12Device_Release(This) \
  (This)->lpVtbl->Release(This)
#endif

#define CHECK(hr, msg) do { \
  if (FAILED(hr)) { \
    fprintf(stderr, "FAIL: %s (hr=0x%08lx)\n", (msg), (unsigned long)(hr)); \
    return 1; \
  } \
} while (0)

#define HEADER(name) \
  printf("\n== %s ==\n", (name))

typedef enum TestFormat
{
  TEST_FORMAT_BGRA8,
  TEST_FORMAT_RGBA16F
}
TestFormat;

static const char * formatName(TestFormat format)
{
  switch(format)
  {
    case TEST_FORMAT_BGRA8 : return "bgra8";
    case TEST_FORMAT_RGBA16F: return "rgba16f";
  }
  return "unknown";
}

static DXGI_FORMAT dxgiFormat(TestFormat format)
{
  switch(format)
  {
    case TEST_FORMAT_BGRA8 : return DXGI_FORMAT_B8G8R8A8_UNORM;
    case TEST_FORMAT_RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
  }
  return DXGI_FORMAT_UNKNOWN;
}

static unsigned bytesPerPixel(TestFormat format)
{
  switch(format)
  {
    case TEST_FORMAT_BGRA8 : return 4;
    case TEST_FORMAT_RGBA16F: return 8;
  }
  return 0;
}

static TestFormat parseFormat(const char * s)
{
  if (!s || strcmp(s, "bgra8") == 0)
    return TEST_FORMAT_BGRA8;
  if (strcmp(s, "rgba16f") == 0 || strcmp(s, "r16g16b16a16f") == 0)
    return TEST_FORMAT_RGBA16F;
  fprintf(stderr, "Unknown --format '%s', using bgra8\n", s);
  return TEST_FORMAT_BGRA8;
}

// Pattern: per-pixel value derived from (x,y) so we can verify every byte.
// For rgba16f the values are raw half-float bit patterns; CopyResource should
// preserve them exactly, so this validates layout/copy correctness.
static void fillPattern(uint8_t * dst, unsigned w, unsigned h, unsigned pitch,
  TestFormat format)
{
  for (unsigned y = 0; y < h; ++y)
  {
    uint8_t * row = dst + (size_t)y * pitch;
    for (unsigned x = 0; x < w; ++x)
    {
      if (format == TEST_FORMAT_BGRA8)
      {
        row[x * 4 + 0] = (uint8_t)(x);        // B
        row[x * 4 + 1] = (uint8_t)(y);        // G
        row[x * 4 + 2] = (uint8_t)(x ^ y);    // R
        row[x * 4 + 3] = 0xFF;                // A
      }
      else
      {
        uint16_t * px = (uint16_t *)(row + x * 8);
        px[0] = (uint16_t)(0x3C00u + (x & 0x03FFu));       // R
        px[1] = (uint16_t)(0x4000u + (y & 0x03FFu));       // G
        px[2] = (uint16_t)(0x4400u + ((x ^ y) & 0x03FFu)); // B
        px[3] = 0x3C00u;                                   // A = 1.0
      }
    }
  }
}

static bool checkPattern(const volatile uint8_t * dst, unsigned w, unsigned h,
  unsigned pitch, TestFormat format)
{
  for (unsigned y = 0; y < h; ++y)
  {
    const volatile uint8_t * row = dst + (size_t)y * pitch;
    for (unsigned x = 0; x < w; ++x)
    {
      if (format == TEST_FORMAT_BGRA8)
      {
        if (row[x * 4 + 0] != (uint8_t)x      ) return false;
        if (row[x * 4 + 1] != (uint8_t)y      ) return false;
        if (row[x * 4 + 2] != (uint8_t)(x ^ y)) return false;
        if (row[x * 4 + 3] != 0xFF            ) return false;
      }
      else
      {
        const volatile uint16_t * px =
          (const volatile uint16_t *)(row + x * 8);
        if (px[0] != (uint16_t)(0x3C00u + (x & 0x03FFu))) return false;
        if (px[1] != (uint16_t)(0x4000u + (y & 0x03FFu))) return false;
        if (px[2] != (uint16_t)(0x4400u + ((x ^ y) & 0x03FFu))) return false;
        if (px[3] != 0x3C00u) return false;
      }
    }
  }
  return true;
}

static unsigned parseUInt(const char * s, unsigned dflt)
{
  if (!s) return dflt;
  char * end;
  unsigned long v = strtoul(s, &end, 10);
  return (*end == '\0' && v > 0) ? (unsigned)v : dflt;
}

int main(int argc, char ** argv)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  debug_init();

  unsigned width      = 1920;
  unsigned height     = 1080;
  unsigned iterations = 100;
  TestFormat format   = TEST_FORMAT_BGRA8;

  for (int i = 1; i < argc - 1; ++i)
  {
    if      (strcmp(argv[i], "--width" ) == 0) width      = parseUInt(argv[i+1], width);
    else if (strcmp(argv[i], "--height") == 0) height     = parseUInt(argv[i+1], height);
    else if (strcmp(argv[i], "--iter"  ) == 0) iterations = parseUInt(argv[i+1], iterations);
    else if (strcmp(argv[i], "--format") == 0) format     = parseFormat(argv[i+1]);
  }

  // Pitch must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256 bytes)
  const unsigned bpp   = bytesPerPixel(format);
  unsigned pitch       = ((width * bpp) + 255u) & ~255u;
  size_t   neededBytes = (size_t)pitch * height;

  printf("test-d3d11on12-ivshmem: %ux%u %s @ %u-byte pitch, %u iterations\n",
    width, height, formatName(format), pitch, iterations);
  printf("Required IVSHMEM size: %zu bytes (%.2f MiB)\n",
    neededBytes, neededBytes / (1024.0 * 1024.0));

  // --- Open IVSHMEM --------------------------------------------------------

  HEADER("Phase 0: IVSHMEM");
  ivshmemOptionsInit();
  // We don't call option_parse — default device 0 is fine for the test.

  struct IVSHMEM dev = {0};
  if (!ivshmemInit(&dev))
  {
    fprintf(stderr, "ivshmemInit failed — is the Looking Glass IVSHMEM driver installed?\n");
    return 1;
  }
  if (!ivshmemOpen(&dev))
  {
    fprintf(stderr, "ivshmemOpen failed\n");
    ivshmemFree(&dev);
    return 1;
  }
  printf("IVSHMEM: %u bytes at %p [PASS]\n", dev.size, dev.mem);

  if ((size_t)dev.size < neededBytes)
  {
    fprintf(stderr, "IVSHMEM region too small (%u bytes available, %zu needed). "
      "Try a smaller --width/--height.\n", dev.size, neededBytes);
    ivshmemClose(&dev);
    ivshmemFree(&dev);
    return 1;
  }

  // --- Phase A: D3D12 device + IVSHMEM heap --------------------------------

  HEADER("Phase A: D3D12 device + IVSHMEM heap");
  IDXGIFactory4 * factory = NULL;
  CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory),
    "CreateDXGIFactory2");

  IDXGIAdapter1 * adapter = NULL;
  for (UINT i = 0; ; ++i)
  {
    HRESULT hr = IDXGIFactory4_EnumAdapters1(factory, i, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(hr)) break;

    DXGI_ADAPTER_DESC1 ad;
    if (FAILED(IDXGIAdapter1_GetDesc1(adapter, &ad)) ||
        (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
    {
      IDXGIAdapter1_Release(adapter);
      adapter = NULL;
      continue;
    }
    wprintf(L"Adapter: %ls\n", ad.Description);
    break;
  }
  if (!adapter)
  {
    fprintf(stderr, "No suitable adapter found\n");
    return 1;
  }

  ID3D12Device3 * d12dev = NULL;
  CHECK(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_12_0,
    &IID_ID3D12Device3, (void **)&d12dev),
    "D3D12CreateDevice");
  printf("D3D12 device created [PASS]\n");

  D3D12_COMMAND_QUEUE_DESC qDesc =
  {
    .Type     = D3D12_COMMAND_LIST_TYPE_DIRECT,
    .Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
    .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE
  };
  ID3D12CommandQueue * d12queue = NULL;
  CHECK(ID3D12Device3_CreateCommandQueue(d12dev, &qDesc,
    &IID_ID3D12CommandQueue, (void **)&d12queue),
    "ID3D12Device3_CreateCommandQueue");
  ID3D12CommandQueue_SetName(d12queue, L"TestDirect");

  ID3D12Heap * ivHeap = NULL;
  HRESULT hr = ID3D12Device3_OpenExistingHeapFromAddress(d12dev, dev.mem,
    &IID_ID3D12Heap, (void **)&ivHeap);
  if (FAILED(hr))
  {
    fprintf(stderr, "OpenExistingHeapFromAddress failed (hr=0x%08lx)\n",
      (unsigned long)hr);
    fprintf(stderr, "This means GPU-direct writes to IVSHMEM aren't available "
      "on this hardware. The cpu-staging fallback would have to be used.\n");
    return 1;
  }

  D3D12_HEAP_DESC heapDesc = ID3D12Heap_GetDesc(ivHeap);
  printf("IVSHMEM heap opened: size=%llu align=%llu [PASS]\n",
    (unsigned long long)heapDesc.SizeInBytes,
    (unsigned long long)heapDesc.Alignment);

  // --- Phase B: ROW_MAJOR TEXTURE2D placed in IVSHMEM heap -----------------

  HEADER("Phase B: ROW_MAJOR TEXTURE2D placed resource");
  D3D12_RESOURCE_DESC texDesc =
  {
    .Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Alignment          = 0,
    .Width              = width,
    .Height             = height,
    .DepthOrArraySize   = 1,
    .MipLevels          = 1,
    .Format             = dxgiFormat(format),
    .SampleDesc         = { .Count = 1, .Quality = 0 },
    .Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags              = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER
  };

  ID3D12Resource * placedTex = NULL;
  hr = ID3D12Device3_CreatePlacedResource(d12dev, ivHeap, 0, &texDesc,
    D3D12_RESOURCE_STATE_COMMON, NULL,
    &IID_ID3D12Resource, (void **)&placedTex);

  if (FAILED(hr))
  {
    fprintf(stderr, "***** PHASE B FAILED (hr=0x%08lx) *****\n",
      (unsigned long)hr);
    fprintf(stderr, "Could not create ROW_MAJOR TEXTURE2D in the IVSHMEM heap.\n");
    fprintf(stderr, "This is the load-bearing test for the IVSHMEM-direct buffer plan.\n");
    fprintf(stderr, "If this fails, the cpu-staging fallback (which we already\n");
    fprintf(stderr, "built) is the only path available — and the ivshmem-direct\n");
    fprintf(stderr, "design needs to be revisited.\n");
    return 2;
  }
  printf("Placed resource created [PASS]\n");

  // --- Phase C: D3D11On12 device + wrapped resource ------------------------

  HEADER("Phase C: D3D11On12 wrapping");
  ID3D11Device        * d11dev = NULL;
  ID3D11DeviceContext * d11ctx = NULL;
  D3D_FEATURE_LEVEL     fl;
  IUnknown * queues[] = { (IUnknown *)d12queue };

  CHECK(D3D11On12CreateDevice((IUnknown *)d12dev,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    NULL, 0,
    queues, 1,
    0,
    &d11dev, &d11ctx, &fl),
    "D3D11On12CreateDevice");
  printf("D3D11On12 device created (feature level 0x%x) [PASS]\n", (unsigned)fl);

  ID3D11On12Device * d11on12 = NULL;
  CHECK(ID3D11Device_QueryInterface(d11dev,
    &IID_ID3D11On12Device, (void **)&d11on12),
    "QueryInterface ID3D11On12Device");

  D3D11_RESOURCE_FLAGS wrapFlags =
  {
    .BindFlags           = D3D11_BIND_SHADER_RESOURCE,
    .MiscFlags           = 0,
    .CPUAccessFlags      = 0,
    .StructureByteStride = 0
  };

  ID3D11Texture2D * wrappedTex = NULL;
  hr = ID3D11On12Device_CreateWrappedResource(d11on12,
    (IUnknown *)placedTex, &wrapFlags,
    D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_COMMON,
    &IID_ID3D11Texture2D, (void **)&wrappedTex);

  if (FAILED(hr))
  {
    fprintf(stderr, "***** PHASE C FAILED (hr=0x%08lx) *****\n",
      (unsigned long)hr);
    fprintf(stderr, "D3D11On12 could not wrap the placed resource.\n");
    fprintf(stderr, "Placed resource exists but D3D11 interop isn't possible —\n");
    fprintf(stderr, "the design would need a different mechanism for WGC's\n");
    fprintf(stderr, "D3D11 immediate context to write into IVSHMEM.\n");
    return 2;
  }
  printf("Wrapped placed texture as D3D11 texture [PASS]\n");

  // --- Source D3D11 texture with pattern -----------------------------------

  uint8_t * pattern = malloc(neededBytes);
  if (!pattern)
  {
    fprintf(stderr, "malloc failed\n");
    return 1;
  }
  fillPattern(pattern, width, height, pitch, format);

  D3D11_TEXTURE2D_DESC srcDesc =
  {
    .Width          = width,
    .Height         = height,
    .MipLevels      = 1,
    .ArraySize      = 1,
    .Format         = dxgiFormat(format),
    .SampleDesc     = { .Count = 1, .Quality = 0 },
    .Usage          = D3D11_USAGE_DEFAULT,
    .BindFlags      = D3D11_BIND_SHADER_RESOURCE,
    .CPUAccessFlags = 0,
    .MiscFlags      = 0
  };
  D3D11_SUBRESOURCE_DATA srcData =
  {
    .pSysMem     = pattern,
    .SysMemPitch = pitch
  };
  ID3D11Texture2D * srcTex = NULL;
  CHECK(ID3D11Device_CreateTexture2D(d11dev, &srcDesc, &srcData, &srcTex),
    "ID3D11Device_CreateTexture2D src");

  // --- Phase D: CopyResource via D3D11 -------------------------------------

  HEADER("Phase D: D3D11 CopyResource into IVSHMEM-resident texture");

  ID3D12Fence * fence = NULL;
  CHECK(ID3D12Device3_CreateFence(d12dev, 0, D3D12_FENCE_FLAG_NONE,
    &IID_ID3D12Fence, (void **)&fence),
    "CreateFence");
  HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);
  UINT64 fenceValue = 0;

  ID3D11Resource * acquired[] = { (ID3D11Resource *)wrappedTex };

  ID3D11On12Device_AcquireWrappedResources(d11on12, acquired, 1);
  ID3D11DeviceContext_CopyResource(d11ctx,
    (ID3D11Resource *)wrappedTex, (ID3D11Resource *)srcTex);
  ID3D11On12Device_ReleaseWrappedResources(d11on12, acquired, 1);
  ID3D11DeviceContext_Flush(d11ctx);

  ++fenceValue;
  CHECK(ID3D12CommandQueue_Signal(d12queue, fence, fenceValue), "Signal");
  if (ID3D12Fence_GetCompletedValue(fence) < fenceValue)
  {
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, fenceValue, event),
      "SetEventOnCompletion");
    WaitForSingleObject(event, INFINITE);
  }
  printf("CopyResource executed [PASS]\n");

  // --- Phase E: verify pattern landed in IVSHMEM ---------------------------

  HEADER("Phase E: verify pattern in IVSHMEM");
  bool ok = checkPattern((const volatile uint8_t *)dev.mem, width, height,
    pitch, format);
  printf("Pattern verification %s\n", ok ? "[PASS]" : "[FAIL]");

  if (!ok)
  {
    fprintf(stderr, "***** PHASE E FAILED *****\n");
    fprintf(stderr, "The GPU wrote SOMETHING into IVSHMEM but it doesn't match\n");
    fprintf(stderr, "the source pattern. First few pixels of row 0:\n");
    const volatile uint8_t * row = (const volatile uint8_t *)dev.mem;
    for (int x = 0; x < 8 && x < (int)width; ++x)
    {
      if (format == TEST_FORMAT_BGRA8)
        fprintf(stderr,
          "  x=%d got B=%02x G=%02x R=%02x A=%02x (expected %02x %02x %02x ff)\n",
          x, row[x*4+0], row[x*4+1], row[x*4+2], row[x*4+3],
          (uint8_t)x, 0, (uint8_t)x);
      else
      {
        const volatile uint16_t * px =
          (const volatile uint16_t *)(row + x * 8);
        fprintf(stderr,
          "  x=%d got R=%04x G=%04x B=%04x A=%04x\n",
          x, px[0], px[1], px[2], px[3]);
      }
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "Possible causes:\n");
    fprintf(stderr, "  - Cache coherency: GPU writes haven't flushed to BAR.\n");
    fprintf(stderr, "  - Texture tiling: ROW_MAJOR was requested but driver\n");
    fprintf(stderr, "    silently used a tiled layout.\n");
    fprintf(stderr, "  - D3D11On12 didn't actually back the wrapper with the\n");
    fprintf(stderr, "    placed resource.\n");
    fprintf(stderr, "  - GPU's PCIe writes hit a different physical address.\n");
    fprintf(stderr, "\nThe design needs a different mechanism if this is the\n");
    fprintf(stderr, "case — likely the cpu-staging path is the only option.\n");
    return 3;
  }

  // --- Phase F: throughput benchmark ---------------------------------------

  HEADER("Phase F: throughput benchmark");
  printf("Running %u iterations of %ux%u %s CopyResource...\n",
    iterations, width, height, formatName(format));

  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);

  for (unsigned i = 0; i < iterations; ++i)
  {
    ID3D11On12Device_AcquireWrappedResources(d11on12, acquired, 1);
    ID3D11DeviceContext_CopyResource(d11ctx,
      (ID3D11Resource *)wrappedTex, (ID3D11Resource *)srcTex);
    ID3D11On12Device_ReleaseWrappedResources(d11on12, acquired, 1);
    ID3D11DeviceContext_Flush(d11ctx);

    ++fenceValue;
    ID3D12CommandQueue_Signal(d12queue, fence, fenceValue);
    if (ID3D12Fence_GetCompletedValue(fence) < fenceValue)
    {
      ID3D12Fence_SetEventOnCompletion(fence, fenceValue, event);
      WaitForSingleObject(event, INFINITE);
    }
  }

  QueryPerformanceCounter(&t1);
  double seconds      = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
  double bytesPerFrm  = (double)pitch * (double)height;
  double totalBytes   = bytesPerFrm * (double)iterations;
  double throughputGB = totalBytes / seconds / (1024.0 * 1024.0 * 1024.0);
  double msPerFrame   = seconds * 1000.0 / (double)iterations;

  printf("Total time         : %.3f s\n", seconds);
  printf("Time per frame     : %.3f ms\n", msPerFrame);
  printf("Effective bandwidth: %.2f GB/s\n", throughputGB);
  printf("\nFor reference, sustained 4K@144Hz needs ~4.7 GB/s.\n");

  // --- Cleanup -------------------------------------------------------------

  CloseHandle(event);
  ID3D12Fence_Release(fence);
  ID3D11Texture2D_Release(srcTex);
  free(pattern);
  ID3D11Texture2D_Release(wrappedTex);
  ID3D11On12Device_Release(d11on12);
  ID3D11DeviceContext_Release(d11ctx);
  ID3D11Device_Release(d11dev);
  ID3D12Resource_Release(placedTex);
  ID3D12Heap_Release(ivHeap);
  ID3D12CommandQueue_Release(d12queue);
  ID3D12Device3_Release(d12dev);
  IDXGIAdapter1_Release(adapter);
  IDXGIFactory4_Release(factory);
  ivshmemClose(&dev);
  ivshmemFree(&dev);

  printf("\nAll phases passed. The IVSHMEM-direct buffer design is viable on this hardware.\n");
  return 0;
}
