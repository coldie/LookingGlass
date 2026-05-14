#include "wgc_profile_d3d11.h"

#ifdef ENABLE_TRACY

#include <new>
#include <string.h>
#include <tracy/TracyD3D11.hpp>
#include <tracy/TracyD3D12.hpp>

struct WGCTracyD3D11Context
{
  TracyD3D11Ctx ctx;
};

struct WGCTracyD3D11Zone
{
  tracy::D3D11ZoneScope zone;

  WGCTracyD3D11Zone(
    TracyD3D11Ctx ctx,
    unsigned line,
    const char * source,
    size_t sourceSize,
    const char * function,
    size_t functionSize,
    const char * name,
    size_t nameSize) :
    zone(ctx, line, source, sourceSize, function, functionSize,
      name, nameSize, true)
  {
  }
};

struct WGCTracyD3D12Context
{
  TracyD3D12Ctx ctx;
};

struct WGCTracyD3D12Zone
{
  tracy::D3D12ZoneScope zone;

  WGCTracyD3D12Zone(
    TracyD3D12Ctx ctx,
    unsigned line,
    const char * source,
    size_t sourceSize,
    const char * function,
    size_t functionSize,
    const char * name,
    size_t nameSize,
    ID3D12GraphicsCommandList * list) :
    zone(ctx, line, source, sourceSize, function, functionSize,
      name, nameSize, list, true)
  {
  }
};

WGCTracyD3D11Context * wgc_tracyD3D11Create(
  ID3D11Device * device, ID3D11DeviceContext * context, const char * name)
{
  if (!device || !context)
    return nullptr;

  auto * wrapper = new(std::nothrow) WGCTracyD3D11Context;
  if (!wrapper)
    return nullptr;

  wrapper->ctx = TracyD3D11Context(device, context);
  if (name && *name)
    TracyD3D11ContextName(wrapper->ctx, name, (uint16_t)strlen(name));
  return wrapper;
}

void wgc_tracyD3D11Destroy(WGCTracyD3D11Context ** ctx)
{
  if (!ctx || !*ctx)
    return;

  TracyD3D11Destroy((*ctx)->ctx);
  delete *ctx;
  *ctx = nullptr;
}

void wgc_tracyD3D11Collect(WGCTracyD3D11Context * ctx)
{
  if (!ctx)
    return;
  TracyD3D11Collect(ctx->ctx);
}

WGCTracyD3D11Zone * wgc_tracyD3D11ZoneBegin(
  WGCTracyD3D11Context * ctx,
  unsigned line,
  const char * source,
  size_t sourceSize,
  const char * function,
  size_t functionSize,
  const char * name,
  size_t nameSize)
{
  if (!ctx)
    return nullptr;

  return new(std::nothrow) WGCTracyD3D11Zone(ctx->ctx, line, source,
    sourceSize, function, functionSize, name, nameSize);
}

void wgc_tracyD3D11ZoneEnd(WGCTracyD3D11Zone ** zone)
{
  if (!zone || !*zone)
    return;

  delete *zone;
  *zone = nullptr;
}

WGCTracyD3D12Context * wgc_tracyD3D12Create(
  ID3D12Device * device, ID3D12CommandQueue * queue, const char * name)
{
  if (!device || !queue)
    return nullptr;

  auto * wrapper = new(std::nothrow) WGCTracyD3D12Context;
  if (!wrapper)
    return nullptr;

  wrapper->ctx = TracyD3D12Context(device, queue);
  if (name && *name)
    TracyD3D12ContextName(wrapper->ctx, name, (uint16_t)strlen(name));
  return wrapper;
}

void wgc_tracyD3D12Destroy(WGCTracyD3D12Context ** ctx)
{
  if (!ctx || !*ctx)
    return;

  TracyD3D12Destroy((*ctx)->ctx);
  delete *ctx;
  *ctx = nullptr;
}

void wgc_tracyD3D12NewFrame(WGCTracyD3D12Context * ctx)
{
  if (!ctx)
    return;
  TracyD3D12NewFrame(ctx->ctx);
}

void wgc_tracyD3D12Collect(WGCTracyD3D12Context * ctx)
{
  if (!ctx)
    return;
  TracyD3D12Collect(ctx->ctx);
}

WGCTracyD3D12Zone * wgc_tracyD3D12ZoneBegin(
  WGCTracyD3D12Context * ctx,
  unsigned line,
  const char * source,
  size_t sourceSize,
  const char * function,
  size_t functionSize,
  const char * name,
  size_t nameSize,
  ID3D12GraphicsCommandList * list)
{
  if (!ctx || !list)
    return nullptr;

  return new(std::nothrow) WGCTracyD3D12Zone(ctx->ctx, line, source,
    sourceSize, function, functionSize, name, nameSize, list);
}

void wgc_tracyD3D12ZoneEnd(WGCTracyD3D12Zone ** zone)
{
  if (!zone || !*zone)
    return;

  delete *zone;
  *zone = nullptr;
}

#endif
