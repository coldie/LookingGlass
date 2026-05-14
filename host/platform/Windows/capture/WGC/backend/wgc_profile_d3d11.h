#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WGCTracyD3D11Context WGCTracyD3D11Context;
typedef struct WGCTracyD3D11Zone    WGCTracyD3D11Zone;
typedef struct WGCTracyD3D12Context WGCTracyD3D12Context;
typedef struct WGCTracyD3D12Zone    WGCTracyD3D12Zone;

#ifdef ENABLE_TRACY

WGCTracyD3D11Context * wgc_tracyD3D11Create(
  ID3D11Device * device, ID3D11DeviceContext * context, const char * name);
void wgc_tracyD3D11Destroy(WGCTracyD3D11Context ** ctx);
void wgc_tracyD3D11Collect(WGCTracyD3D11Context * ctx);

WGCTracyD3D11Zone * wgc_tracyD3D11ZoneBegin(
  WGCTracyD3D11Context * ctx,
  unsigned line,
  const char * source,
  size_t sourceSize,
  const char * function,
  size_t functionSize,
  const char * name,
  size_t nameSize);
void wgc_tracyD3D11ZoneEnd(WGCTracyD3D11Zone ** zone);

WGCTracyD3D12Context * wgc_tracyD3D12Create(
  ID3D12Device * device, ID3D12CommandQueue * queue, const char * name);
void wgc_tracyD3D12Destroy(WGCTracyD3D12Context ** ctx);
void wgc_tracyD3D12NewFrame(WGCTracyD3D12Context * ctx);
void wgc_tracyD3D12Collect(WGCTracyD3D12Context * ctx);

WGCTracyD3D12Zone * wgc_tracyD3D12ZoneBegin(
  WGCTracyD3D12Context * ctx,
  unsigned line,
  const char * source,
  size_t sourceSize,
  const char * function,
  size_t functionSize,
  const char * name,
  size_t nameSize,
  ID3D12GraphicsCommandList * list);
void wgc_tracyD3D12ZoneEnd(WGCTracyD3D12Zone ** zone);

#define WGC_TRACY_D3D11_CREATE(device, context, name) \
  wgc_tracyD3D11Create((ID3D11Device *)(device), \
    (ID3D11DeviceContext *)(context), (name))
#define WGC_TRACY_D3D11_DESTROY(ctx) \
  wgc_tracyD3D11Destroy(&(ctx))
#define WGC_TRACY_D3D11_COLLECT(ctx) \
  wgc_tracyD3D11Collect((ctx))
#define WGC_TRACY_D3D11_ZONE_BEGIN(ctx, name) \
  wgc_tracyD3D11ZoneBegin((ctx), __LINE__, __FILE__, sizeof(__FILE__) - 1, \
    __func__, sizeof(__func__) - 1, (name), sizeof(name) - 1)
#define WGC_TRACY_D3D11_ZONE_BEGIN_N(ctx, name, nameSize) \
  wgc_tracyD3D11ZoneBegin((ctx), __LINE__, __FILE__, sizeof(__FILE__) - 1, \
    __func__, sizeof(__func__) - 1, (name), (nameSize))
#define WGC_TRACY_D3D11_ZONE_END(zone) \
  wgc_tracyD3D11ZoneEnd(&(zone))
#define WGC_TRACY_D3D12_CREATE(device, queue, name) \
  wgc_tracyD3D12Create((ID3D12Device *)(device), \
    (ID3D12CommandQueue *)(queue), (name))
#define WGC_TRACY_D3D12_DESTROY(ctx) \
  wgc_tracyD3D12Destroy(&(ctx))
#define WGC_TRACY_D3D12_NEW_FRAME(ctx) \
  wgc_tracyD3D12NewFrame((ctx))
#define WGC_TRACY_D3D12_COLLECT(ctx) \
  wgc_tracyD3D12Collect((ctx))
#define WGC_TRACY_D3D12_ZONE_BEGIN(ctx, list, name) \
  wgc_tracyD3D12ZoneBegin((ctx), __LINE__, __FILE__, sizeof(__FILE__) - 1, \
    __func__, sizeof(__func__) - 1, (name), sizeof(name) - 1, \
    (ID3D12GraphicsCommandList *)(list))
#define WGC_TRACY_D3D12_ZONE_BEGIN_N(ctx, list, name, nameSize) \
  wgc_tracyD3D12ZoneBegin((ctx), __LINE__, __FILE__, sizeof(__FILE__) - 1, \
    __func__, sizeof(__func__) - 1, (name), (nameSize), \
    (ID3D12GraphicsCommandList *)(list))
#define WGC_TRACY_D3D12_ZONE_END(zone) \
  wgc_tracyD3D12ZoneEnd(&(zone))

#else

#define WGC_TRACY_D3D11_CREATE(device, context, name) \
  ((void)(device), (void)(context), (void)(name), (WGCTracyD3D11Context *)NULL)
#define WGC_TRACY_D3D11_DESTROY(ctx) do { (ctx) = NULL; } while(0)
#define WGC_TRACY_D3D11_COLLECT(ctx) do { (void)(ctx); } while(0)
#define WGC_TRACY_D3D11_ZONE_BEGIN(ctx, name) \
  ((void)(ctx), (void)(name), (WGCTracyD3D11Zone *)NULL)
#define WGC_TRACY_D3D11_ZONE_BEGIN_N(ctx, name, nameSize) \
  ((void)(ctx), (void)(name), (void)(nameSize), (WGCTracyD3D11Zone *)NULL)
#define WGC_TRACY_D3D11_ZONE_END(zone) do { (void)(zone); (zone) = NULL; } while(0)
#define WGC_TRACY_D3D12_CREATE(device, queue, name) \
  ((void)(device), (void)(queue), (void)(name), (WGCTracyD3D12Context *)NULL)
#define WGC_TRACY_D3D12_DESTROY(ctx) do { (ctx) = NULL; } while(0)
#define WGC_TRACY_D3D12_NEW_FRAME(ctx) do { (void)(ctx); } while(0)
#define WGC_TRACY_D3D12_COLLECT(ctx) do { (void)(ctx); } while(0)
#define WGC_TRACY_D3D12_ZONE_BEGIN(ctx, list, name) \
  ((void)(ctx), (void)(list), (void)(name), (WGCTracyD3D12Zone *)NULL)
#define WGC_TRACY_D3D12_ZONE_BEGIN_N(ctx, list, name, nameSize) \
  ((void)(ctx), (void)(list), (void)(name), (void)(nameSize), \
    (WGCTracyD3D12Zone *)NULL)
#define WGC_TRACY_D3D12_ZONE_END(zone) do { (void)(zone); (zone) = NULL; } while(0)

#endif

#ifdef __cplusplus
}
#endif
