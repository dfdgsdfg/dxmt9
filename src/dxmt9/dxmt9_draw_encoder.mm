#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9_draw_encoder.hpp"

#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_resource_pool.hpp"

#include <algorithm>
#include <optional>

namespace dxmt9::encoders {

using core::ClearDesc;
using core::DrawDesc;
using core::SamplerSnapshot;
using core::SAMP_ADDRESS_U;
using core::SAMP_ADDRESS_V;
using core::SAMP_ADDRESS_W;
using core::SAMP_BORDER_COLOR;
using core::SAMP_MAG_FILTER;
using core::SAMP_MIN_FILTER;
using core::SAMP_MIP_FILTER;

using dxmt9::convert::formatHasDepthAspect;
using dxmt9::convert::formatHasStencilAspect;
using dxmt9::ffp::decodeFixedFunctionVertexLayout;

using u32 = std::uint32_t;

WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device, bool linear) {
  WMTSamplerInfo info{};
  auto f = linear ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  info.min_filter = f;
  info.mag_filter = f;
  info.mip_filter = WMTSamplerMipFilterNotMipmapped;
  info.s_address_mode = WMTSamplerAddressModeClampToEdge;
  info.t_address_mode = WMTSamplerAddressModeClampToEdge;
  info.r_address_mode = WMTSamplerAddressModeClampToEdge;
  info.normalized_coords = true;
  return device.newSamplerState(info);
}

WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const SamplerSnapshot& snapshot) {
  const auto minFilter = snapshot.states.contains(SAMP_MIN_FILTER) ? snapshot.states.at(SAMP_MIN_FILTER) : 0u;
  const auto magFilter = snapshot.states.contains(SAMP_MAG_FILTER) ? snapshot.states.at(SAMP_MAG_FILTER) : 0u;
  const auto mipFilter = snapshot.states.contains(SAMP_MIP_FILTER) ? snapshot.states.at(SAMP_MIP_FILTER) : 0u;
  const auto addressU = snapshot.states.contains(SAMP_ADDRESS_U) ? snapshot.states.at(SAMP_ADDRESS_U) : 1u;
  const auto addressV = snapshot.states.contains(SAMP_ADDRESS_V) ? snapshot.states.at(SAMP_ADDRESS_V) : 1u;
  const auto addressW = snapshot.states.contains(SAMP_ADDRESS_W) ? snapshot.states.at(SAMP_ADDRESS_W) : 1u;
  const auto borderColor = snapshot.states.contains(SAMP_BORDER_COLOR) ? snapshot.states.at(SAMP_BORDER_COLOR) : 0u;
  auto resolveAddressMode = [](u32 value) -> WMTSamplerAddressMode {
    switch (value) {
      case 1u: return WMTSamplerAddressModeRepeat;
      case 2u: return WMTSamplerAddressModeMirrorRepeat;
      case 4u: return WMTSamplerAddressModeClampToBorderColor;
      case 3u:
      default: return WMTSamplerAddressModeClampToEdge;
    }
  };
  auto resolveBorderColor = [](u32 value) -> WMTSamplerBorderColor {
    switch (value) {
      case 0x00000000u: return WMTSamplerBorderColorTransparentBlack;
      case 0xff000000u: return WMTSamplerBorderColorOpaqueBlack;
      case 0xffffffffu: return WMTSamplerBorderColorOpaqueWhite;
      default: return (value >> 24) == 0u ? WMTSamplerBorderColorTransparentBlack : WMTSamplerBorderColorOpaqueBlack;
    }
  };

  WMTSamplerInfo info{};
  info.min_filter = minFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  info.mag_filter = magFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  switch (mipFilter) {
    case 2u: info.mip_filter = WMTSamplerMipFilterLinear; break;
    case 1u: info.mip_filter = WMTSamplerMipFilterNearest; break;
    default: info.mip_filter = WMTSamplerMipFilterNotMipmapped; break;
  }
  info.s_address_mode = resolveAddressMode(addressU);
  info.t_address_mode = resolveAddressMode(addressV);
  info.r_address_mode = resolveAddressMode(addressW);
  if (info.s_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.t_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.r_address_mode == WMTSamplerAddressModeClampToBorderColor) {
    info.border_color = resolveBorderColor(borderColor);
  }
  info.normalized_coords = true;
  return device.newSamplerState(info);
}

WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(
    const EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    const DrawDesc& draw,
    const std::optional<ClearDesc>& clear) {
  auto* surface = ctx.pool.findSurface(draw.rts.color[0].handle.value);
  if (!surface || !surface->texture) {
    return {};
  }
  WMTRenderPassInfo passInfo{};
  auto& attachment = passInfo.colors[0];
  attachment.texture = surface->texture.handle;
  const bool discardAfterPresent = !clear.has_value() && ctx.queue.backBufferDiscardAfterPresent_ &&
                                   draw.rts.color[0].handle == ctx.queue.currentBackBuffer_;
  attachment.load_action = clear.has_value() ? WMTLoadActionClear
                                              : (discardAfterPresent ? WMTLoadActionDontCare
                                                                     : WMTLoadActionLoad);
  attachment.store_action = WMTStoreActionStore;
  if (surface->resolveTexture) {
    attachment.resolve_texture = surface->resolveTexture.handle;
    attachment.store_action = WMTStoreActionMultisampleResolve;
  }
  if (clear.has_value()) {
    attachment.clear_color = WMTClearColor{clear->color.r, clear->color.g,
                                           clear->color.b, clear->color.a};
  }

  if (auto* depthSurface = ctx.pool.findSurface(draw.rts.depthStencil.handle.value);
      depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    if (formatHasDepthAspect(depthSurface->desc.format)) {
      passInfo.depth.texture = depthSurface->texture.handle;
      passInfo.depth.load_action = (clear.has_value() && clear->clearDepth)
                                       ? WMTLoadActionClear : WMTLoadActionLoad;
      passInfo.depth.store_action = WMTStoreActionStore;
      if (clear.has_value()) {
        passInfo.depth.clear_depth = clear->depth;
      }
    }
    if (formatHasStencilAspect(depthSurface->desc.format)) {
      passInfo.stencil.texture = depthSurface->texture.handle;
      passInfo.stencil.load_action = (clear.has_value() && clear->clearStencil)
                                         ? WMTLoadActionClear : WMTLoadActionLoad;
      passInfo.stencil.store_action = WMTStoreActionStore;
      if (clear.has_value()) {
        passInfo.stencil.clear_stencil = clear->stencil;
      }
    }
  }

  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) {
    return {};
  }
  if (discardAfterPresent) {
    ctx.queue.backBufferDiscardAfterPresent_ = false;
  }
  const auto ffLayout = decodeFixedFunctionVertexLayout(draw);
  double viewportWidth = static_cast<double>(std::max(1u, draw.viewport.viewport.width));
  double viewportHeight = static_cast<double>(std::max(1u, draw.viewport.viewport.height));
  double viewportOriginX = 0.0;
  double viewportOriginY = 0.0;
  if (ffLayout && ffLayout->preTransformed) {
    viewportWidth = static_cast<double>(std::max(1u, surface->desc.width));
    viewportHeight = static_cast<double>(std::max(1u, surface->desc.height));
  }
  WMTViewport vp{viewportOriginX, viewportOriginY, viewportWidth, viewportHeight,
                 static_cast<double>(draw.viewport.viewport.minZ),
                 static_cast<double>(draw.viewport.viewport.maxZ)};
  encoder.setViewport(vp);
  encoder.setRasterizerState(WMTTriangleFillModeFill, WMTCullModeNone,
                              WMTDepthClipModeClip, WMTWindingClockwise,
                              0.0f, 0.0f, 0.0f);
  return WMT::Reference<WMT::RenderCommandEncoder>(encoder);
}

}  // namespace dxmt9::encoders
