#include "device_c_provider.hpp"

#include <cstdint>
#include <span>

using namespace dxmt9::d3d9::devicec;

namespace {

bool failed(int32_t hr) {
  return hr < 0;
}

std::uint32_t primitiveVertexCount(std::uint32_t primitiveType, std::uint32_t primitiveCount) {
  switch (primitiveType) {
  case 1: return primitiveCount;       // D3DPT_POINTLIST
  case 2: return primitiveCount * 2u;  // D3DPT_LINELIST
  case 3: return primitiveCount + 1u;  // D3DPT_LINESTRIP
  case 4: return primitiveCount * 3u;  // D3DPT_TRIANGLELIST
  case 5: return primitiveCount + 2u;  // D3DPT_TRIANGLESTRIP
  case 6: return primitiveCount + 2u;  // D3DPT_TRIANGLEFAN
  default: return 0;
  }
}

bool checkedMul(std::uint32_t a, std::uint32_t b, std::uint32_t& out) {
  const std::uint64_t value = static_cast<std::uint64_t>(a) * b;
  if (value > 0xffffffffull) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

}  // namespace

extern "C" int32_t dxmt9c_device_present(D9CDevice* d, const D9CRect* src, const D9CRect* dst,
                                         uint64_t destWindow, const void* dirty, uint32_t flags) {
  dxmt9DebugLog("device_present begin destWindow=%llu flags=0x%x src=%d dst=%d",
                static_cast<unsigned long long>(destWindow), flags, src ? 1 : 0, dst ? 1 : 0);
  using Rect = dxmt9::core::Rect;
  Rect* srcRect = src ? new Rect{src->left, src->top, src->right, src->bottom} : nullptr;
  Rect* dstRect = dst ? new Rect{dst->left, dst->top, dst->right, dst->bottom} : nullptr;
  const auto hr = d->iface->PresentEx(srcRect, dstRect, {destWindow}, dirty, flags);
  delete srcRect;
  delete dstRect;
  dxmt9DebugLog("device_present hr=0x%08x", static_cast<unsigned>(hr));
  return hr;
}

extern "C" int32_t dxmt9c_device_begin_scene(D9CDevice* d) {
  return d->iface->BeginScene();
}

extern "C" int32_t dxmt9c_device_end_scene(D9CDevice* d) {
  return d->iface->EndScene();
}

extern "C" int32_t dxmt9c_device_clear(D9CDevice* d, uint32_t count, const D9CRect* rects,
                                       uint32_t flags, uint32_t colorARGB, float z,
                                       uint32_t stencil) {
  dxmt9::core::ClearDesc desc;
  desc.clearColor = (flags & 1) != 0;
  desc.clearDepth = (flags & 2) != 0;
  desc.clearStencil = (flags & 4) != 0;
  desc.color.a = ((colorARGB >> 24) & 0xff) / 255.0f;
  desc.color.r = ((colorARGB >> 16) & 0xff) / 255.0f;
  desc.color.g = ((colorARGB >> 8) & 0xff) / 255.0f;
  desc.color.b = (colorARGB & 0xff) / 255.0f;
  desc.depth = z;
  desc.stencil = stencil;
  for (uint32_t i = 0; i < count; ++i) {
    desc.rects.push_back({rects[i].left, rects[i].top, rects[i].right, rects[i].bottom});
  }
  return d->iface->Clear(desc);
}

extern "C" int32_t dxmt9c_device_set_vertex_declaration(D9CDevice* d, D9CVertexDecl* vd) {
  if (!vd) {
    return d->iface->SetVertexDeclaration({});
  }
  return d->iface->SetVertexDeclaration(vd->elements);
}

extern "C" int32_t dxmt9c_device_set_stream_source(D9CDevice* d, uint32_t stream,
                                                   D9CBuffer* buf, uint32_t off,
                                                   uint32_t stride) {
  auto buffer = buf ? buf->obj : nullptr;
  return d->iface->SetStreamSource(stream, buffer, off, stride);
}

extern "C" int32_t dxmt9c_device_set_stream_source_freq(D9CDevice* d, uint32_t stream,
                                                        uint32_t freq) {
  (void)d;
  (void)stream;
  (void)freq;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_indices(D9CDevice* d, D9CBuffer* buf) {
  auto buffer = buf ? buf->obj : nullptr;
  return d->iface->SetIndices(buffer, buf ? idxTypeFromD3D(buf->desc.format) : dxmt9::core::IndexType::UInt16);
}

extern "C" int32_t dxmt9c_device_set_texture(D9CDevice* d, uint32_t stage, D9CTexture* tex) {
  auto texture = tex ? tex->obj : nullptr;
  return d->iface->SetTexture(stage, texture);
}

extern "C" int32_t dxmt9c_device_set_vertex_shader(D9CDevice* d, D9CShader* s) {
  if (!s) {
    return d->iface->SetVertexShader({});
  }
  return d->iface->SetVertexShader(s->ref);
}

extern "C" int32_t dxmt9c_device_set_pixel_shader(D9CDevice* d, D9CShader* s) {
  if (!s) {
    return d->iface->SetPixelShader({});
  }
  return d->iface->SetPixelShader(s->ref);
}

extern "C" int32_t dxmt9c_device_set_render_target(D9CDevice* d, uint32_t idx,
                                                   D9CSurface* surf) {
  return d->iface->SetRenderTarget(idx, surf ? surf->obj : nullptr);
}

extern "C" D9CSurface* dxmt9c_device_get_render_target(D9CDevice* d, uint32_t idx) {
  auto swapChain = d->iface->GetSwapChain(0);
  if (!swapChain) {
    return nullptr;
  }
  auto surface = idx == 0 ? swapChain->backBuffer() : nullptr;
  if (!surface) {
    return nullptr;
  }
  return new D9CSurface{surface};
}

extern "C" int32_t dxmt9c_device_set_depth_stencil(D9CDevice* d, D9CSurface* surf) {
  return d->iface->SetDepthStencilSurface(surf ? surf->obj : nullptr);
}

extern "C" D9CSurface* dxmt9c_device_get_depth_stencil(D9CDevice* d) {
  auto swapChain = d->iface->GetSwapChain(0);
  if (!swapChain) {
    return nullptr;
  }
  auto surface = swapChain->depthStencilSurface();
  if (!surface) {
    return nullptr;
  }
  return new D9CSurface{surface};
}

extern "C" int32_t dxmt9c_device_draw_primitive(D9CDevice* d, uint32_t type,
                                                uint32_t startVertex, uint32_t count) {
  return d->iface->DrawPrimitive(ptFromD3D(type), count, startVertex);
}

extern "C" int32_t dxmt9c_device_draw_indexed_primitive(D9CDevice* d, uint32_t type,
                                                        int32_t baseVertex, uint32_t minV,
                                                        uint32_t numV, uint32_t startIdx,
                                                        uint32_t count) {
  (void)minV;
  (void)numV;
  const auto& state = d->dev().state();
  return d->iface->DrawIndexedPrimitive(ptFromD3D(type), count, 0, baseVertex, startIdx,
                                        state.indexType);
}

extern "C" int32_t dxmt9c_device_draw_primitive_up(D9CDevice* d, uint32_t type,
                                                   uint32_t count, const void* data,
                                                   uint32_t stride) {
  std::uint32_t bytes = 0;
  if (!checkedMul(primitiveVertexCount(type, count), stride, bytes) ||
      (bytes != 0 && !data)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto span = std::span<const dxmt9::core::u8>(
      reinterpret_cast<const dxmt9::core::u8*>(data), bytes);
  return d->dev().drawPrimitiveUP(ptFromD3D(type), count, span, stride);
}

extern "C" int32_t dxmt9c_device_draw_indexed_primitive_up(D9CDevice* d, uint32_t type,
                                                           uint32_t minV, uint32_t numV,
                                                           uint32_t count, const void* idxData,
                                                           uint32_t idxFmt,
                                                           const void* vtxData,
                                                           uint32_t stride) {
  if (minV > 0xffffffffu - numV) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  std::uint32_t vertexBytes = 0;
  const uint32_t indexSize = idxFmt == 102 ? 4 : 2;
  std::uint32_t indexBytes = 0;
  if (!checkedMul(minV + numV, stride, vertexBytes) ||
      !checkedMul(primitiveVertexCount(type, count), indexSize, indexBytes) ||
      (indexBytes != 0 && !idxData) ||
      (vertexBytes != 0 && !vtxData)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto vertexSpan = std::span<const dxmt9::core::u8>(
      reinterpret_cast<const dxmt9::core::u8*>(vtxData), vertexBytes);
  auto indexSpan = std::span<const dxmt9::core::u8>(
      reinterpret_cast<const dxmt9::core::u8*>(idxData), indexBytes);
  return d->iface->DrawIndexedPrimitiveUP(ptFromD3D(type), count, vertexSpan, indexSpan,
                                          idxTypeFromD3D(idxFmt), stride);
}

extern "C" int32_t dxmt9c_device_update_surface(D9CDevice* d, D9CSurface* src,
                                                const D9CRect*, D9CSurface* dst,
                                                const D9CRect*) {
  if (!src || !dst) {
    dxmt9DebugLog("device_update_surface invalid src=%p dst=%p",
                  static_cast<void*>(src), static_cast<void*>(dst));
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const int32_t hr = d->iface->UpdateSurface(src->obj, dst->obj);
  if (failed(hr)) {
    dxmt9DebugLog("device_update_surface failed src=%p dst=%p srcObj=%p dstObj=%p hr=0x%08x",
                  static_cast<void*>(src), static_cast<void*>(dst),
                  static_cast<void*>(src->obj.get()), static_cast<void*>(dst->obj.get()),
                  static_cast<std::uint32_t>(hr));
  }
  return hr;
}

extern "C" int32_t dxmt9c_device_update_texture(D9CDevice* d, D9CTexture* src,
                                                D9CTexture* dst) {
  if (!src || !dst) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return d->iface->UpdateTexture(src->obj, dst->obj);
}

extern "C" int32_t dxmt9c_device_stretch_rect(D9CDevice* d, D9CSurface* src, const D9CRect* sr,
                                              D9CSurface* dst, const D9CRect* dr,
                                              uint32_t filter) {
  if (!src || !dst) {
    dxmt9DebugLog("device_stretch_rect invalid wrapper src=%p dst=%p",
                  static_cast<void*>(src), static_cast<void*>(dst));
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  constexpr uint32_t kD3DTexfNone = 0;
  constexpr uint32_t kD3DTexfPoint = 1;
  constexpr uint32_t kD3DTexfLinear = 2;
  if (filter != kD3DTexfNone && filter != kD3DTexfPoint && filter != kD3DTexfLinear) {
    dxmt9DebugLog("device_stretch_rect invalid filter=%u", filter);
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto* srcRect =
      sr ? new dxmt9::core::Rect{sr->left, sr->top, sr->right, sr->bottom} : nullptr;
  auto* dstRect =
      dr ? new dxmt9::core::Rect{dr->left, dr->top, dr->right, dr->bottom} : nullptr;
  const auto hr = d->iface->StretchRect(src->obj, srcRect, dst->obj, dstRect,
                                        filter == kD3DTexfLinear);
  if (failed(hr)) {
    const auto* srcObj = src->obj.get();
    const auto* dstObj = dst->obj.get();
    const bool srcValid = srcObj && srcObj->valid();
    const bool dstValid = dstObj && dstObj->valid();
    const auto srcDesc = srcObj ? srcObj->desc() : dxmt9::core::SurfaceDesc{};
    const auto dstDesc = dstObj ? dstObj->desc() : dxmt9::core::SurfaceDesc{};
    dxmt9DebugLog(
        "device_stretch_rect failed hr=0x%08x src=%p srcObj=%p srcValid=%u "
        "srcHandle=0x%llx srcFmt=%u srcUsage=0x%x srcPool=%u srcRT=%u srcDS=%u srcSize=%ux%u "
        "dst=%p dstObj=%p dstValid=%u dstHandle=0x%llx dstFmt=%u dstUsage=0x%x dstPool=%u "
        "dstRT=%u dstDS=%u dstSize=%ux%u filter=%u",
        static_cast<std::uint32_t>(hr),
        static_cast<void*>(src), static_cast<const void*>(srcObj), srcValid ? 1u : 0u,
        static_cast<unsigned long long>(srcObj ? srcObj->handle().value : 0ull),
        static_cast<unsigned>(srcDesc.format), srcDesc.usage,
        static_cast<unsigned>(srcDesc.pool), srcDesc.renderTarget ? 1u : 0u,
        srcDesc.depthStencil ? 1u : 0u, srcDesc.width, srcDesc.height,
        static_cast<void*>(dst), static_cast<const void*>(dstObj), dstValid ? 1u : 0u,
        static_cast<unsigned long long>(dstObj ? dstObj->handle().value : 0ull),
        static_cast<unsigned>(dstDesc.format), dstDesc.usage,
        static_cast<unsigned>(dstDesc.pool), dstDesc.renderTarget ? 1u : 0u,
        dstDesc.depthStencil ? 1u : 0u, dstDesc.width, dstDesc.height, filter);
  }
  delete srcRect;
  delete dstRect;
  return hr;
}

extern "C" int32_t dxmt9c_device_color_fill(D9CDevice* d, D9CSurface* surf, const D9CRect* r,
                                            uint32_t colorARGB) {
  if (!surf) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto* rect = r ? new dxmt9::core::Rect{r->left, r->top, r->right, r->bottom} : nullptr;
  dxmt9::core::ColorRGBA rgba{
      ((colorARGB >> 16) & 0xff) / 255.0f,
      ((colorARGB >> 8) & 0xff) / 255.0f,
      (colorARGB & 0xff) / 255.0f,
      ((colorARGB >> 24) & 0xff) / 255.0f,
  };
  const auto hr = d->iface->FillSurface(surf->obj, rect, rgba);
  delete rect;
  return hr;
}

extern "C" int32_t dxmt9c_device_get_render_target_data(D9CDevice* d, D9CSurface* rt,
                                                        D9CSurface* dst) {
  if (!rt || !dst) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return d->iface->GetRenderTargetData(rt->obj, dst->obj);
}
