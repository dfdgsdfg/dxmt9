#include "device_c_provider.hpp"
#include "util/log/log.hpp"

#include <algorithm>
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
  if (failed(hr)) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-device",
                      "device_present_fail hr=0x%08x destWindow=%llu "
                      "flags=0x%x src=%u dst=%u",
                      static_cast<std::uint32_t>(hr),
                      static_cast<unsigned long long>(destWindow), flags,
                      src ? 1u : 0u, dst ? 1u : 0u);
  }
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
  return d->iface->SetStreamSourceFreq(stream, freq);
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
  const int32_t hr = d->iface->SetRenderTarget(idx, surf ? surf->obj : nullptr);
  if (hr == dxmt9::core::D3D_OK && idx < dxmt9::core::kMaxRenderTargets) {
    d->renderTargets[idx] = surf ? surf->obj : nullptr;
    d->renderTargetExplicit[idx] = true;
  }
  return hr;
}

extern "C" D9CSurface* dxmt9c_device_get_render_target(D9CDevice* d, uint32_t idx) {
  if (idx >= dxmt9::core::kMaxRenderTargets) {
    return nullptr;
  }
  if (d->renderTargetExplicit[idx]) {
    auto surface = d->renderTargets[idx];
    return surface ? new D9CSurface{surface, nullptr, 0u, d} : nullptr;
  }

  auto swapChain = d->iface->GetSwapChain(0);
  if (!swapChain) {
    return nullptr;
  }
  auto surface = idx == 0 ? swapChain->backBuffer() : nullptr;
  if (!surface) {
    return nullptr;
  }
  return new D9CSurface{surface, nullptr, 0u, d};
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
  return new D9CSurface{surface, nullptr, 0u, d};
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
                                                const D9CRect* srcRect, D9CSurface* dst,
                                                const D9CRect* dstPoint) {
  if (!src || !dst) {
    dxmt9DebugLog("device_update_surface invalid src=%p dst=%p",
                  static_cast<void*>(src), static_cast<void*>(dst));
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  // Palettized surface pairs are NOT routed through Device::updateSurface,
  // for the same reason dxmt9c_device_update_texture does not route through
  // Device::updateTexture (see the comment there).
  //
  // A P8/A8P8 texture's Metal backing is a DERIVED A8R8G8B8 expansion of
  // (index shadow x that texture's palette); it is not storage the app ever
  // wrote. Device::updateSurface moves that backing twice — a queued GPU
  // SurfaceCopy of the source's Metal texture, and a CPU lockRect/memcpy of
  // the source's expanded bytes — and both carry the SOURCE's palette. A
  // SYSTEMMEM staging source is never bound through SetTexture, so its
  // palette is still initPalettizedTexture's identity ramp.
  //
  // Unlike the UpdateTexture case there is no corrective re-expansion behind
  // it, so this was not a race but two deterministic failures, both measured
  // on 2026-08-02 with the destination bound and sampled:
  //   1. The destination reads back as the identity ramp — P8 indices
  //      {1,2,3,4} sampled ff010101/ff020202/ff030303/ff040404 instead of the
  //      palette colours.
  //   2. D9CSurface carries no palette state, so the destination TEXTURE's
  //      p8Levels index shadow was never written. The next SetPaletteEntries
  //      or SetCurrentTexturePalette re-expands the destination from that
  //      still-zero shadow and the copy vanishes entirely: P8 went to
  //      ff000000 (palette[0]) and A8P8 to 00000000.
  // Copying the index shadow and re-expanding with the DESTINATION's palette
  // fixes both, and makes the expansion the only writer.
  //
  // Both sides must be levels of palettized textures. D9CSurface has nowhere
  // to store a palette, and a standalone CreateOffscreenPlainSurface(P8) is a
  // genuine Format::P8 surface rather than an expanded backing, so such a
  // pair is left on the unchanged path (where the core format mismatch
  // already rejects it) rather than given invented semantics here.
  //
  // An unservable palettized pair FALLS THROUGH to the routed path below --
  // it must not fail this record.
  //
  // The first version of this returned D3DERR_INVALIDCALL, on the reasoning
  // that a mismatched P8/A8P8 pair is "already rejected by PE-side validation,
  // so unreachable from a real app". Both halves were wrong, and review caught
  // it. PE's format check (`d3d9_pe_device.cpp:11208`) compares
  // `Surface::GetDesc` formats, and `dxmt9c_surface_get_desc` reports the CORE
  // BACKING format -- A8R8G8B8 for a level of either a P8 or an A8P8 texture,
  // because both back onto the expansion. So `21 == 21` and the pair sails
  // through. (That misreport is a D3D9 parity break in its own right; see
  // specs/d3d9/gap.md.)
  //
  // And reaching it is far worse than an INVALIDCALL, because UpdateSurface is
  // a fire-and-forget chunk record (`D9C_COMMAND_RECORD_UPDATE_SURFACE`),
  // replayed asynchronously. A failed record does not become an HRESULT the app
  // sees: it is `commitChunkFail`, which on the engine-default offload lane
  // fail-stops the worker and poisons every later commit. That would turn a
  // call real D3D9 merely rejects into a process-lifetime wedge -- and D3D9-era
  // apps do probe by calling and checking the HRESULT.
  //
  // Falling through restores exactly the pre-fix behaviour for that exotic
  // pair (wrong pixels, app survives) while the ordinary matched pair still
  // takes the correct branch. Strictly no worse than before, for every input.
  if (src->ownerTex && dst->ownerTex && src->ownerTex->palettized &&
      dst->ownerTex->palettized &&
      dxmt9c_copy_palettized_subresource(src->ownerTex, src->ownerLevel,
                                         dst->ownerTex, dst->ownerLevel,
                                         srcRect, dstPoint)) {
    return dxmt9::core::D3D_OK;
  }

  const dxmt9::core::Rect sourceArea = srcRect
      ? dxmt9::core::Rect{srcRect->left, srcRect->top,
                          srcRect->right, srcRect->bottom}
      : dxmt9::core::Rect{};
  const int32_t dstX = dstPoint ? dstPoint->left : 0;
  const int32_t dstY = dstPoint ? dstPoint->top : 0;
  const int32_t hr = d->iface->UpdateSurface(
      src->obj, srcRect ? &sourceArea : nullptr, dst->obj, dstX, dstY);
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
  if (!d || !src || !dst) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (src->palettized != dst->palettized) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (src->palettized && src->d3dFormat != dst->d3dFormat) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  if (!src->palettized) {
    return d->iface->UpdateTexture(src->obj, dst->obj);
  }
  if (!src->obj || !dst->obj || !src->obj->valid() || !dst->obj->valid()) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  // Palettized destinations are NOT routed through Device::updateTexture.
  //
  // A P8/A8P8 texture's Metal backing is not storage the app ever wrote —
  // it is a derived A8R8G8B8 expansion of (index shadow x that texture's
  // palette). Device::updateTexture would (a) queue a GPU SurfaceCopy of
  // the SOURCE's backing and (b) copy the source's expanded bytes into the
  // destination's CPU shadow. Both carry the SOURCE texture's palette,
  // which for a SYSTEMMEM staging texture is never set by the app and so
  // is still initPalettizedTexture's identity ramp (0xff010101,
  // 0xff020202, ...). The destination is then expanded again with its own
  // palette, so the same destination subresource receives two conflicting
  // full-subresource writes on two different submission paths: the
  // chunk-stream blit and the ResourceInitializer's deferred staging
  // upload. Nothing makes the correct one the last writer, and the
  // identity expansion wins often enough to be a ~45% flake
  // (specs/d3d9/gap.md, 2026-08-02). Re-adding just the SurfaceCopy on top
  // of this function reproduces it at 9/10, so the blit alone is the
  // sufficient cause.
  //
  // Copying indices and re-expanding with the DESTINATION's palette makes
  // the expansion the only writer, which is also the only semantically
  // correct source for the bytes.
  const size_t count = std::min(src->p8Levels.size(), dst->p8Levels.size());
  for (size_t subresource = 0; subresource < count; ++subresource) {
    dst->p8Levels[subresource] = src->p8Levels[subresource];
    const auto level = static_cast<uint32_t>(subresource);
    if (dst->lockedLevels.find(level) == dst->lockedLevels.end()) {
      dxmt9c_expand_palettized_subresource(dst, level);
    }
  }
  return dxmt9::core::D3D_OK;
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
