#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_blit_encoders.hpp"

#include "dxmt9_command_queue.hpp"
#include "dxmt9_device.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_state.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_presenter.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace dxmt9::encoders {

using core::ClearDesc;
using core::DrawDesc;
using core::Handle;
using core::IndexType;
using core::SamplerSnapshot;
using core::SAMP_ADDRESS_U;
using core::SAMP_ADDRESS_V;
using core::SAMP_ADDRESS_W;
using core::SAMP_BORDER_COLOR;
using core::SAMP_MAG_FILTER;
using core::SAMP_MIN_FILTER;
using core::SAMP_MIP_FILTER;
using core::kMaxTextureStages;

using core::CompareFunc;
using core::TextureOp;

using core::RS_ALPHABLEND_ENABLE;
using core::RS_ALPHA_FUNC;
using core::RS_ALPHA_REF;
using core::RS_ALPHA_TEST_ENABLE;
using core::RS_COLOR_WRITE_ENABLE;
using core::RS_CULL_MODE;
using core::RS_DEST_BLEND;
using core::RS_SRC_BLEND;
using core::RS_TEXTURE_FACTOR;
using core::RS_Z_ENABLE;
using core::RS_Z_FUNC;
using core::RS_Z_WRITE_ENABLE;

using core::TSS_ALPHA_ARG1;
using core::TSS_ALPHA_ARG2;
using core::TSS_ALPHA_OP;
using core::TSS_COLOR_ARG1;
using core::TSS_COLOR_ARG2;
using core::TSS_COLOR_OP;
using core::TSS_TEXCOORD_INDEX;
using core::TSS_TEXTURE_TRANSFORM_FLAGS;

using dxmt9::ffp::kD3DDeclTypeD3DColor;
using dxmt9::ffp::kD3DDeclTypeFloat4;
using dxmt9::ffp::kD3DDeclUsageColor;
using dxmt9::ffp::kD3DDeclUsagePosition;
using dxmt9::ffp::kD3DDeclUsageTexcoord;

using dxmt9::convert::formatHasDepthAspect;
using dxmt9::convert::formatHasStencilAspect;
using dxmt9::convert::toCullMode;
using dxmt9::convert::toIndexType;
using dxmt9::convert::toPrimitiveType;
using dxmt9::ffp::computeVertexDeclStride;
using dxmt9::ffp::decodeFixedFunctionVertexLayout;

using dxmt9::core::metalqueue::emitQueueTraceLine;
using dxmt9::core::metalqueue::emitTextureTraceLine;
using dxmt9::core::metalqueue::queueTraceEnabled;

using dxmt9::state::DrawUniforms;
using dxmt9::state::buildDrawUniforms;
using dxmt9::state::makeDepthStencilKey;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using f32 = float;

namespace {

// Attachment key + hazard bloom used by encodeChunk to decide whether to
// flush + restart the render pass between commands. Previously file-local
// to backend_metal.mm.
struct AttachmentKey {
  std::array<u64, core::kMaxRenderTargets> colorHandles{};
  u64 depthHandle = 0;
  u32 sampleCount = 1;
  friend bool operator==(const AttachmentKey&, const AttachmentKey&) = default;
};

u64 bloomMix64(u64 value, u64 salt) {
  u64 x = value + salt + 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

struct HazardBloom {
  std::array<u64, 2> bits{};
  void add(u64 value) {
    if (value == 0) return;
    const u64 hash0 = bloomMix64(value, 0x4d595df4d0f33173ull);
    const u64 hash1 = bloomMix64(value, 0x9e3779b97f4a7c15ull);
    bits[0] |= 1ull << (hash0 & 63u);
    bits[1] |= 1ull << (hash1 & 63u);
  }
  bool overlaps(const HazardBloom& other) const {
    return ((bits[0] & other.bits[0]) != 0) || ((bits[1] & other.bits[1]) != 0);
  }
};

HazardBloom makeAttachmentBloom(const core::RenderTargetSnapshot& rts) {
  HazardBloom bloom;
  for (const auto& attachment : rts.color) bloom.add(attachment.handle.value);
  bloom.add(rts.depthStencil.handle.value);
  return bloom;
}

HazardBloom makeAttachmentBloom(const core::ClearDesc& clear) {
  HazardBloom bloom;
  for (const auto& attachment : clear.colorAttachments) bloom.add(attachment.handle.value);
  bloom.add(clear.depthStencil.handle.value);
  return bloom;
}

HazardBloom makeDrawReadBloom(const core::DrawDesc& draw) {
  HazardBloom bloom;
  bloom.add(draw.indexBuffer.value);
  for (const auto& stream : draw.vertexDecl.streams) {
    if (stream.buffer) bloom.add(stream.buffer->handle().value);
  }
  for (const auto& texture : draw.textures) bloom.add(texture.handle.value);
  return bloom;
}

AttachmentKey makeAttachmentKey(const core::RenderTargetSnapshot& rts) {
  AttachmentKey key;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    key.colorHandles[i] = rts.color[i].handle.value;
    key.sampleCount = std::max(key.sampleCount, rts.color[i].sampleCount);
  }
  key.depthHandle = rts.depthStencil.handle.value;
  key.sampleCount = std::max(key.sampleCount, rts.depthStencil.sampleCount);
  return key;
}

AttachmentKey makeAttachmentKey(const core::ClearDesc& clear) {
  AttachmentKey key;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    key.colorHandles[i] = clear.colorAttachments[i].handle.value;
    key.sampleCount = std::max(key.sampleCount, clear.colorAttachments[i].sampleCount);
  }
  key.depthHandle = clear.depthStencil.handle.value;
  key.sampleCount = std::max(key.sampleCount, clear.depthStencil.sampleCount);
  return key;
}

u32 primitiveVertexCount(core::PrimitiveType type, u32 primitiveCount) {
  switch (type) {
    case core::PrimitiveType::PointList: return primitiveCount;
    case core::PrimitiveType::LineList: return primitiveCount * 2u;
    case core::PrimitiveType::LineStrip: return primitiveCount + 1u;
    case core::PrimitiveType::TriangleList: return primitiveCount * 3u;
    case core::PrimitiveType::TriangleStrip:
    case core::PrimitiveType::TriangleFan:
      return primitiveCount + 2u;
  }
  return 0u;
}

std::size_t indexElementSize(IndexType type) {
  return type == IndexType::UInt16 ? 2u : 4u;
}

}  // namespace

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
    EncodeContext& ctx,
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

void encodeDraw(EncodeContext& ctx,
                 WMT::CommandBuffer& commandBuffer,
                 WMT::RenderCommandEncoder& encoder,
                 const DrawDesc& draw,
                 u64 seqId) {
  (void)commandBuffer;
  if (debug::skipAllDraws()) {
    if (queueTraceEnabled()) {
      std::ostringstream out;
      out << "[dxmt9-debug] skip all draws seq=" << static_cast<unsigned long long>(seqId)
          << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value);
      emitQueueTraceLine(out.str());
    }
    return;
  }
  const bool traceEncode = debug::shouldTraceEncode(draw, seqId);
  if (!encoder) {
    if (traceEncode) {
      emitQueueTraceLine("[dxmt9-encode] seq=" + std::to_string(seqId) + " skipped reason=no-encoder");
    }
    return;
  }
  const auto depthKey = makeDepthStencilKey(draw);
  auto pipeline = ctx.cache.getOrBuildDrawPipelineForDraw(
      ctx.device, ctx.limits, ctx.pool, draw, ctx.shaderArchive, ctx.shaderArchivePath).get();
  if (!pipeline) {
    if (traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
          << " skipped reason=no-pipeline"
          << " rt0=" << static_cast<unsigned long long>(draw.rts.color[0].handle.value)
          << " ds=" << static_cast<unsigned long long>(draw.rts.depthStencil.handle.value)
          << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
          << " fvf=0x" << std::hex << draw.vertexDecl.fvf << std::dec
          << " alphaBlend="
          << (draw.rs.values.contains(RS_ALPHABLEND_ENABLE) ? draw.rs.values.at(RS_ALPHABLEND_ENABLE) : 0u)
          << " colorWrite="
          << (draw.rs.values.contains(RS_COLOR_WRITE_ENABLE) ? draw.rs.values.at(RS_COLOR_WRITE_ENABLE) : 0xfu);
      emitQueueTraceLine(out.str());
    }
    return;
  }
  auto depthState = ctx.cache.depthStencilStateFor(ctx.device, depthKey);
  if (depthState) {
    encoder.setDepthStencilState(depthState);
  }
  encoder.setRenderPipelineState(pipeline);
  auto* uniforms = ctx.allocators.argbuf.allocate<DrawUniforms>(seqId);
  DrawUniforms fallbackUniforms{};
  if (!uniforms) {
    uniforms = &fallbackUniforms;
  }
  *uniforms = buildDrawUniforms(draw);
  WMTBufferInfo uniformInfo{};
  uniformInfo.length = sizeof(DrawUniforms);
  uniformInfo.options = WMTResourceStorageModeShared;
  uniformInfo.memory.set((void *)uniforms);
  auto transientUniformBuffer = ctx.device.newBuffer(uniformInfo);
  if (!transientUniformBuffer) {
    return;
  }
  encoder.setVertexBuffer(transientUniformBuffer, 0, 0);
  encoder.setFragmentBuffer(transientUniformBuffer, 0, 0);
  const auto ffLayout = decodeFixedFunctionVertexLayout(draw);
  if (auto* surface = ctx.pool.findSurface(draw.rts.color[0].handle.value); surface && surface->texture) {
    double viewportWidth = static_cast<double>(std::max(1u, draw.viewport.viewport.width));
    double viewportHeight = static_cast<double>(std::max(1u, draw.viewport.viewport.height));
    double viewportOriginX = 0.0;
    double viewportOriginY = 0.0;
    if (ffLayout && ffLayout->preTransformed) {
      viewportWidth = static_cast<double>(std::max(1u, surface->desc.width));
      viewportHeight = static_cast<double>(std::max(1u, surface->desc.height));
    }
    encoder.setViewport(WMTViewport{viewportOriginX, viewportOriginY, viewportWidth, viewportHeight,
                                    static_cast<double>(draw.viewport.viewport.minZ),
                                    static_cast<double>(draw.viewport.viewport.maxZ)});
    WMTScissorRect scissor{};
    if (draw.viewport.scissorEnabled && !debug::disableScissor()) {
      scissor.x = static_cast<uint64_t>(std::max(0, draw.viewport.scissor.left));
      scissor.y = static_cast<uint64_t>(std::max(0, draw.viewport.scissor.top));
      scissor.width = static_cast<uint64_t>(std::max(0, draw.viewport.scissor.right - draw.viewport.scissor.left));
      scissor.height =
          static_cast<uint64_t>(std::max(0, draw.viewport.scissor.bottom - draw.viewport.scissor.top));
    } else {
      scissor.x = 0;
      scissor.y = 0;
      scissor.width = static_cast<uint64_t>(std::max(1u, surface->desc.width));
      scissor.height = static_cast<uint64_t>(std::max(1u, surface->desc.height));
    }
    encoder.setScissorRect(scissor);
    if (ffLayout && ffLayout->preTransformed) {
      encoder.setCullMode(WMTCullModeNone);
    } else {
      encoder.setCullMode(static_cast<WMTCullMode>(toCullMode(
          draw.rs.values.contains(RS_CULL_MODE) ? draw.rs.values.at(RS_CULL_MODE) : 1u)));
    }
  }
  static std::atomic<int> ffTraceRemaining{debug::fixedFunctionTraceBudget()};
  const u32 primitiveCount = std::max<u32>(1, draw.primitiveCount);
  const uint64_t vertexCount =
      static_cast<uint64_t>(std::max(1u, primitiveVertexCount(draw.primitiveType, primitiveCount)));
  const bool indexedDraw = draw.indexBuffer || !draw.userIndexData.empty();
  WMT::Reference<WMT::Buffer> transientVertexBuffer;
  std::span<const u8> vertexBytes;
  WMT::Buffer vertexBuffer{};
  uint64_t vertexBufferOffset = 0;
  auto makeTransientBuffer = [&](const void* data, std::size_t len) -> WMT::Reference<WMT::Buffer> {
    WMTBufferInfo bi{};
    bi.length = len;
    bi.options = WMTResourceStorageModeShared;
    bi.memory.set((void *)data);
    return ctx.device.newBuffer(bi);
  };
  if (!draw.userVertexData.empty()) {
    transientVertexBuffer = makeTransientBuffer(draw.userVertexData.data(),
                                                 draw.userVertexData.size());
    vertexBuffer = transientVertexBuffer;
    vertexBufferOffset = draw.vertexDecl.streams[0].offset;
    vertexBytes = draw.userVertexData;
  } else if (draw.vertexDecl.streams[0].buffer) {
    if (auto* buffer = ctx.pool.findBuffer(draw.vertexDecl.streams[0].buffer->handle().value);
        buffer && buffer->buffer) {
      vertexBuffer = WMT::Buffer{buffer->buffer.handle};
      vertexBufferOffset = draw.vertexDecl.streams[0].offset;
      if (!buffer->shadow.empty()) {
        vertexBytes = buffer->shadow;
      } else if (buffer->contents) {
        vertexBytes = std::span<const u8>(static_cast<const u8*>(buffer->contents),
                                          static_cast<std::size_t>(buffer->desc.size));
      }
    } else {
      const auto bytes = draw.vertexDecl.streams[0].buffer->bytes();
      if (!bytes.empty()) {
        transientVertexBuffer = makeTransientBuffer(bytes.data(), bytes.size());
        vertexBuffer = transientVertexBuffer;
        vertexBufferOffset = draw.vertexDecl.streams[0].offset;
        vertexBytes = bytes;
      }
    }
  }
  if (traceEncode && !ffLayout && !vertexBytes.empty() && !draw.vertexDecl.elements.empty()) {
    auto readF32 = [&](std::size_t absoluteOffset) {
      float value = 0.0f;
      if (absoluteOffset + sizeof(float) <= vertexBytes.size()) {
        std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(float));
      }
      return value;
    };
    auto readU32 = [&](std::size_t absoluteOffset) {
      u32 value = 0;
      if (absoluteOffset + sizeof(u32) <= vertexBytes.size()) {
        std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(u32));
      }
      return value;
    };

    std::optional<u32> positionOffset;
    std::optional<u32> colorOffset;
    std::optional<u32> texcoord0Offset;
    for (const auto& element : draw.vertexDecl.elements) {
      if (!positionOffset && element.usage == kD3DDeclUsagePosition && element.usageIndex == 0 &&
          element.type == kD3DDeclTypeFloat4) {
        positionOffset = element.offset;
      } else if (!colorOffset && element.usage == kD3DDeclUsageColor && element.usageIndex == 0 &&
                 element.type == kD3DDeclTypeD3DColor) {
        colorOffset = element.offset;
      } else if (!texcoord0Offset && element.usage == kD3DDeclUsageTexcoord && element.usageIndex == 0 &&
                 element.type == kD3DDeclTypeFloat4) {
        texcoord0Offset = element.offset;
      }
    }

    if (positionOffset && texcoord0Offset) {
      const std::size_t stride = static_cast<std::size_t>(computeVertexDeclStride(draw));
      const std::size_t streamBase = static_cast<std::size_t>(draw.vertexDecl.streams[0].offset);
      std::ostringstream trace;
      trace << "[dxmt9-encode-verts] seq=" << static_cast<unsigned long long>(seqId)
            << " startVertex=" << draw.startVertex
            << " baseVertex=" << draw.baseVertexIndex
            << " stride=" << stride
            << " bytes=" << vertexBytes.size();
      const u32 tracedVertexCount = std::min<u32>(static_cast<u32>(vertexCount), 6u);
      for (u32 i = 0; i < tracedVertexCount; ++i) {
        const std::size_t base = streamBase +
                            static_cast<std::size_t>(draw.startVertex + i) * stride;
        trace << " v" << i << "=("
              << readF32(base + *positionOffset + 0) << ","
              << readF32(base + *positionOffset + 4) << ","
              << readF32(base + *positionOffset + 8) << ","
              << readF32(base + *positionOffset + 12) << ")";
        if (colorOffset) {
          trace << " c=0x" << std::hex << readU32(base + *colorOffset) << std::dec;
        }
        trace << " uv=("
              << readF32(base + *texcoord0Offset + 0) << ","
              << readF32(base + *texcoord0Offset + 4) << ","
              << readF32(base + *texcoord0Offset + 8) << ","
              << readF32(base + *texcoord0Offset + 12) << ")";
      }
      emitQueueTraceLine(trace.str());
    }
  }
  if (ffLayout) {
    if (!vertexBuffer) {
      if (traceEncode) {
        emitQueueTraceLine("[dxmt9-encode] seq=" + std::to_string(seqId) + " skipped reason=no-vertex-buffer");
      }
      return;
    }
    uniforms->vertexStreamOffset = 0;
    uniforms->vertexStreamStride =
        draw.vertexDecl.streams[0].stride ? draw.vertexDecl.streams[0].stride : ffLayout->stride;
    if (!indexedDraw && uniforms->vertexStreamStride != 0u) {
      vertexBufferOffset += static_cast<uint64_t>(draw.startVertex) *
                            static_cast<uint64_t>(uniforms->vertexStreamStride);
      uniforms->vertexBaseIndex = 0;
    } else {
      uniforms->vertexBaseIndex = indexedDraw ? draw.baseVertexIndex : static_cast<i32>(draw.startVertex);
    }
    if (ffLayout->preTransformed) {
      if (auto* targetSurface = ctx.pool.findSurface(draw.rts.color[0].handle.value); targetSurface) {
        uniforms->viewportOrigin = {0.0f, 0.0f};
        uniforms->viewportSize = {static_cast<f32>(std::max(1u, targetSurface->desc.width)),
                                  static_cast<f32>(std::max(1u, targetSurface->desc.height))};
      }
    }
    {
      WMTBufferInfo bi{}; bi.length = sizeof(DrawUniforms);
      bi.options = WMTResourceStorageModeShared; bi.memory.set((void *)uniforms);
      transientUniformBuffer = ctx.device.newBuffer(bi);
    }
    if (!transientUniformBuffer) {
      return;
    }
    encoder.setVertexBuffer(transientUniformBuffer, 0, 0);
    encoder.setFragmentBuffer(transientUniformBuffer, 0, 0);
    encoder.setVertexBuffer(vertexBuffer, vertexBufferOffset, 1);

    const u64 ffTraceTex0 = debug::fixedFunctionTraceTextureHandle();
    const bool forceTrace =
        ffTraceTex0 != 0 && draw.textures[0].handle && draw.textures[0].handle.value == ffTraceTex0;
    if ((forceTrace || ffTraceRemaining.load(std::memory_order_relaxed) > 0) && !vertexBytes.empty()) {
      bool shouldTrace = forceTrace;
      if (!shouldTrace) {
        int expected = ffTraceRemaining.load(std::memory_order_relaxed);
        while (expected > 0 &&
               !ffTraceRemaining.compare_exchange_weak(expected, expected - 1, std::memory_order_relaxed)) {
        }
        shouldTrace = expected > 0;
      }
      if (shouldTrace) {
        std::ostringstream trace;
        const auto stageStateValue = [&](u32 key, u32 fallback) -> u32 {
          const auto it = draw.textures[0].stageStates.find(key);
          return it != draw.textures[0].stageStates.end() ? it->second : fallback;
        };
        const auto stageStateValueAt = [&](std::size_t stageIndex, u32 key, u32 fallback) -> u32 {
          if (stageIndex >= draw.textures.size()) {
            return fallback;
          }
          const auto it = draw.textures[stageIndex].stageStates.find(key);
          return it != draw.textures[stageIndex].stageStates.end() ? it->second : fallback;
        };
        trace << "[dxmt9-ffp] seq=" << static_cast<unsigned long long>(seqId)
              << " fvf=0x" << std::hex << draw.vertexDecl.fvf << std::dec
              << " ffLayout=1"
              << " preT=" << (ffLayout->preTransformed ? 1 : 0)
              << " baseVertex=" << draw.baseVertexIndex
              << " startIndex=" << draw.startIndex
              << " primCount=" << draw.primitiveCount
              << " stride=" << uniforms->vertexStreamStride
              << " viewport=(" << uniforms->viewportOrigin[0] << "," << uniforms->viewportOrigin[1]
              << " " << uniforms->viewportSize[0] << "x" << uniforms->viewportSize[1] << ")"
              << " zEnable=" << (draw.rs.values.contains(RS_Z_ENABLE) ? draw.rs.values.at(RS_Z_ENABLE) : 0u)
              << " zFunc=" << (draw.rs.values.contains(RS_Z_FUNC) ? draw.rs.values.at(RS_Z_FUNC) : 0u)
              << " alphaTest="
              << (draw.rs.values.contains(RS_ALPHA_TEST_ENABLE) ? draw.rs.values.at(RS_ALPHA_TEST_ENABLE) : 0u)
              << " alphaFunc="
              << (draw.rs.values.contains(RS_ALPHA_FUNC) ? draw.rs.values.at(RS_ALPHA_FUNC)
                                                         : static_cast<u32>(CompareFunc::Always))
              << " alphaRef="
              << (draw.rs.values.contains(RS_ALPHA_REF) ? draw.rs.values.at(RS_ALPHA_REF) : 0u)
              << " alphaBlend="
              << (draw.rs.values.contains(RS_ALPHABLEND_ENABLE) ? draw.rs.values.at(RS_ALPHABLEND_ENABLE) : 0u)
              << " srcBlend=" << (draw.rs.values.contains(RS_SRC_BLEND) ? draw.rs.values.at(RS_SRC_BLEND) : 0u)
              << " dstBlend=" << (draw.rs.values.contains(RS_DEST_BLEND) ? draw.rs.values.at(RS_DEST_BLEND) : 0u)
              << " tci0=0x" << std::hex
              << stageStateValue(TSS_TEXCOORD_INDEX, 0u)
              << std::dec
              << " ttff0=0x" << std::hex
              << stageStateValue(TSS_TEXTURE_TRANSFORM_FLAGS, 0u)
              << std::dec
              << " colorOp0=" << stageStateValue(TSS_COLOR_OP, static_cast<u32>(TextureOp::Disable))
              << " colorArg10=" << stageStateValue(TSS_COLOR_ARG1, 0u)
              << " colorArg20=" << stageStateValue(TSS_COLOR_ARG2, 0u)
              << " alphaOp0=" << stageStateValue(TSS_ALPHA_OP, static_cast<u32>(TextureOp::Disable))
              << " alphaArg10=" << stageStateValue(TSS_ALPHA_ARG1, 0u)
              << " alphaArg20=" << stageStateValue(TSS_ALPHA_ARG2, 0u)
              << " colorOp1=" << stageStateValueAt(1, TSS_COLOR_OP, static_cast<u32>(TextureOp::Disable))
              << " colorArg11=" << stageStateValueAt(1, TSS_COLOR_ARG1, 0u)
              << " colorArg21=" << stageStateValueAt(1, TSS_COLOR_ARG2, 0u)
              << " alphaOp1=" << stageStateValueAt(1, TSS_ALPHA_OP, static_cast<u32>(TextureOp::Disable))
              << " alphaArg11=" << stageStateValueAt(1, TSS_ALPHA_ARG1, 0u)
              << " alphaArg21=" << stageStateValueAt(1, TSS_ALPHA_ARG2, 0u)
              << " elems=" << draw.vertexDecl.elements.size()
              << " tfactor=0x"
              << std::hex
              << (draw.rs.values.contains(RS_TEXTURE_FACTOR) ? draw.rs.values.at(RS_TEXTURE_FACTOR) : 0u)
              << std::dec;
        trace << " texM0=["
              << uniforms->ffpTextureTransforms[0][0][0] << "," << uniforms->ffpTextureTransforms[0][0][1] << ","
              << uniforms->ffpTextureTransforms[0][0][2] << "," << uniforms->ffpTextureTransforms[0][0][3] << ";"
              << uniforms->ffpTextureTransforms[0][1][0] << "," << uniforms->ffpTextureTransforms[0][1][1] << ","
              << uniforms->ffpTextureTransforms[0][1][2] << "," << uniforms->ffpTextureTransforms[0][1][3] << ";"
              << uniforms->ffpTextureTransforms[0][2][0] << "," << uniforms->ffpTextureTransforms[0][2][1] << ","
              << uniforms->ffpTextureTransforms[0][2][2] << "," << uniforms->ffpTextureTransforms[0][2][3] << ";"
              << uniforms->ffpTextureTransforms[0][3][0] << "," << uniforms->ffpTextureTransforms[0][3][1] << ","
              << uniforms->ffpTextureTransforms[0][3][2] << "," << uniforms->ffpTextureTransforms[0][3][3] << "]";
        for (std::size_t i = 0; i < draw.vertexDecl.elements.size(); ++i) {
          const auto& e = draw.vertexDecl.elements[i];
          trace << " e" << i << "={s=" << e.stream
                << ",off=" << e.offset
                << ",type=" << e.type
                << ",usage=" << e.usage
                << ",idx=" << e.usageIndex
                << "}";
        }

        auto readF32 = [&](std::size_t absoluteOffset) {
          float value = 0.0f;
          if (absoluteOffset + sizeof(float) <= vertexBytes.size()) {
            std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(float));
          }
          return value;
        };
        auto readU32 = [&](std::size_t absoluteOffset) {
          u32 value = 0;
          if (absoluteOffset + sizeof(u32) <= vertexBytes.size()) {
            std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(u32));
          }
          return value;
        };

        const std::size_t stride = static_cast<std::size_t>(uniforms->vertexStreamStride ? uniforms->vertexStreamStride
                                                                              : ffLayout->stride);
        const u32 tracedVertexCount = std::min<u32>(static_cast<u32>(vertexCount), 24u);
        for (u32 i = 0; i < tracedVertexCount; ++i) {
          const std::size_t base = static_cast<std::size_t>(draw.vertexDecl.streams[0].offset) +
                              static_cast<std::size_t>(draw.baseVertexIndex + static_cast<int>(i)) * stride;
          trace << " v" << i << "=("
                << readF32(base + ffLayout->positionOffset + 0) << ","
                << readF32(base + ffLayout->positionOffset + 4) << ","
                << readF32(base + ffLayout->positionOffset + 8) << ","
                << readF32(base + ffLayout->positionOffset + 12) << ")";
          if (ffLayout->hasDiffuse) {
            const u32 rgba = readU32(base + ffLayout->diffuseOffset);
            trace << " c" << i << "=0x" << std::hex << rgba << std::dec;
          }
          if (ffLayout->hasTexcoord[0]) {
            trace << " uv" << i << "=("
                  << readF32(base + ffLayout->texcoordOffset[0] + 0) << ","
                  << readF32(base + ffLayout->texcoordOffset[0] + 4) << ")";
          }
        }

        if (draw.indexBuffer) {
          const auto* indexRecord = ctx.pool.findBuffer(draw.indexBuffer.value);
          std::span<const u8> indexBytes;
          if (indexRecord && !indexRecord->shadow.empty()) {
            indexBytes = indexRecord->shadow;
          } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
            indexBytes = std::span<const u8>(static_cast<const u8*>(indexRecord->contents),
                                             static_cast<std::size_t>(indexRecord->desc.size));
          }
          if (!indexBytes.empty()) {
            trace << " idx=";
            const std::size_t start = static_cast<std::size_t>(draw.startIndex) * indexElementSize(draw.indexType);
            const u32 tracedIndexCount =
                std::min<u32>(primitiveCount * 3u, 36u);
            for (u32 i = 0; i < tracedIndexCount; ++i) {
              if (i) {
                trace << ",";
              }
              if (draw.indexType == IndexType::UInt16 &&
                  start + static_cast<std::size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
                u16 index = 0;
                std::memcpy(&index, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u16),
                            sizeof(u16));
                trace << index;
              } else if (draw.indexType == IndexType::UInt32 &&
                         start + static_cast<std::size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
                u32 index = 0;
                std::memcpy(&index, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u32),
                            sizeof(u32));
                trace << index;
              } else {
                trace << '?';
              }
            }
            trace << " ref=";
            const u32 tracedRefs = std::min<u32>(12u, tracedIndexCount);
            for (u32 i = 0; i < tracedRefs; ++i) {
              u32 vertexIndex = 0;
              bool haveIndex = false;
              if (draw.indexType == IndexType::UInt16 &&
                  start + static_cast<std::size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
                u16 index = 0;
                std::memcpy(&index, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u16),
                            sizeof(u16));
                vertexIndex = static_cast<u32>(index);
                haveIndex = true;
              } else if (draw.indexType == IndexType::UInt32 &&
                         start + static_cast<std::size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
                std::memcpy(&vertexIndex, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u32),
                            sizeof(u32));
                haveIndex = true;
              }
              if (!haveIndex) {
                break;
              }
              const std::size_t refBase = static_cast<std::size_t>(draw.vertexDecl.streams[0].offset) +
                                     static_cast<std::size_t>(draw.baseVertexIndex + static_cast<int>(vertexIndex)) *
                                         stride;
              trace << " r" << i << "#" << vertexIndex << "=("
                    << readF32(refBase + ffLayout->positionOffset + 0) << ","
                    << readF32(refBase + ffLayout->positionOffset + 4) << ","
                    << readF32(refBase + ffLayout->positionOffset + 8) << ","
                    << readF32(refBase + ffLayout->positionOffset + 12) << ")";
              if (ffLayout->hasTexcoord[0]) {
                trace << " uv=("
                      << readF32(refBase + ffLayout->texcoordOffset[0] + 0) << ","
                      << readF32(refBase + ffLayout->texcoordOffset[0] + 4) << ")";
              }
              if (ffLayout->hasDiffuse) {
                const u32 rgba = readU32(refBase + ffLayout->diffuseOffset);
                trace << " c=0x" << std::hex << rgba << std::dec;
              }
            }
          }
        }
        trace << " tex0=";
        if (draw.textures[0].handle) {
          trace << static_cast<unsigned long long>(draw.textures[0].handle.value);
        } else {
          trace << 0;
        }
        trace << " tex1=";
        if (draw.textures.size() > 1 && draw.textures[1].handle) {
          trace << static_cast<unsigned long long>(draw.textures[1].handle.value);
        } else {
          trace << 0;
        }
        emitQueueTraceLine(trace.str());
      }
    }
  }
  if (vertexBuffer && !ffLayout) {
    const u64 ffTraceTex0 = debug::fixedFunctionTraceTextureHandle();
    const bool forceTrace =
        ffTraceTex0 != 0 && draw.textures[0].handle && draw.textures[0].handle.value == ffTraceTex0;
    if (forceTrace) {
      std::ostringstream trace;
      trace << "[dxmt9-ffp] seq=" << static_cast<unsigned long long>(seqId)
            << " fvf=0x" << std::hex << draw.vertexDecl.fvf << std::dec
            << " ffLayout=" << (ffLayout ? 1 : 0)
            << " baseVertex=" << draw.baseVertexIndex
            << " startIndex=" << draw.startIndex
            << " primCount=" << draw.primitiveCount
            << " stride="
            << (ffLayout ? (draw.vertexDecl.streams[0].stride ? draw.vertexDecl.streams[0].stride : ffLayout->stride)
                         : computeVertexDeclStride(draw))
            << " elems=" << draw.vertexDecl.elements.size();
      for (std::size_t i = 0; i < draw.vertexDecl.elements.size(); ++i) {
        const auto& e = draw.vertexDecl.elements[i];
        trace << " e" << i << "={s=" << e.stream
              << ",off=" << e.offset
              << ",type=" << e.type
              << ",usage=" << e.usage
              << ",idx=" << e.usageIndex
              << "}";
      }
      emitQueueTraceLine(trace.str());
    }
    uniforms->vertexStreamOffset = 0;
    uniforms->vertexStreamStride =
        ffLayout ? (draw.vertexDecl.streams[0].stride ? draw.vertexDecl.streams[0].stride : ffLayout->stride)
                 : computeVertexDeclStride(draw);
    if (!indexedDraw && uniforms->vertexStreamStride != 0u) {
      vertexBufferOffset += static_cast<uint64_t>(draw.startVertex) *
                            static_cast<uint64_t>(uniforms->vertexStreamStride);
      uniforms->vertexBaseIndex = 0;
    } else {
      uniforms->vertexBaseIndex = indexedDraw ? draw.baseVertexIndex : static_cast<i32>(draw.startVertex);
    }
    {
      WMTBufferInfo bi{}; bi.length = sizeof(DrawUniforms);
      bi.options = WMTResourceStorageModeShared; bi.memory.set((void *)uniforms);
      transientUniformBuffer = ctx.device.newBuffer(bi);
    }
    if (!transientUniformBuffer) {
      return;
    }
    encoder.setVertexBuffer(transientUniformBuffer, 0, 0);
    encoder.setFragmentBuffer(transientUniformBuffer, 0, 0);
    encoder.setVertexBuffer(vertexBuffer, vertexBufferOffset, 1);
  }
  for (std::size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    if (!draw.textures[stage].handle) {
      continue;
    }
    if (const u64 skipped = debug::skippedTextureHandle();
        skipped != 0ull && draw.textures[stage].handle.value == skipped) {
      if (traceEncode || debug::shouldTraceTexture(draw.textures[stage].handle)) {
        std::ostringstream out;
        out << "[dxmt9-debug] skip draw seq=" << static_cast<unsigned long long>(seqId)
            << " tex" << stage << "=" << static_cast<unsigned long long>(draw.textures[stage].handle.value);
        emitQueueTraceLine(out.str());
      }
      return;
    }
    if (auto* texture = ctx.pool.findTexture(draw.textures[stage].handle.value); texture && texture->texture) {
      if (debug::shouldTraceTexture(draw.textures[stage].handle)) {
        std::ostringstream out;
        out << "[dxmt9-texture] bind stage=" << stage
            << " handle=0x" << std::hex << draw.textures[stage].handle.value << std::dec
            << " format=" << static_cast<unsigned>(texture->desc.format)
            << " size=" << texture->desc.width << "x" << texture->desc.height
            << " levels=" << texture->desc.levels;
        emitTextureTraceLine(out.str());
      }
      encoder.setFragmentTexture(WMT::Texture{texture->texture.handle}, (uint8_t)stage);
    }
    auto sampler = makeSampler(ctx.device, draw.samplers[stage]);
    if (sampler) {
      encoder.setFragmentSamplerState(sampler, (uint8_t)stage);
    }
  }
  const auto primitiveType = toPrimitiveType(draw.primitiveType);
  bool expandedIndexedDraw = false;
  if (traceEncode) {
    std::ostringstream out;
    out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
        << " draw rt0=" << static_cast<unsigned long long>(draw.rts.color[0].handle.value)
        << " ds=" << static_cast<unsigned long long>(draw.rts.depthStencil.handle.value)
        << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
        << " ffLayout=" << (ffLayout ? 1 : 0)
        << " indexed=" << (indexedDraw ? 1 : 0)
        << " primType=" << static_cast<unsigned>(draw.primitiveType)
        << " primCount=" << draw.primitiveCount
        << " vertexCount=" << static_cast<unsigned long long>(vertexCount)
        << " vertexStreamStride=" << uniforms->vertexStreamStride
        << " vertexBufferOffset=" << vertexBufferOffset
        << " vertexStreamOffset=" << uniforms->vertexStreamOffset
        << " vertexBaseIndex=" << uniforms->vertexBaseIndex
        << " colorWrite="
        << (draw.rs.values.contains(RS_COLOR_WRITE_ENABLE) ? draw.rs.values.at(RS_COLOR_WRITE_ENABLE) : 0xfu)
        << " zEnable=" << (draw.rs.values.contains(RS_Z_ENABLE) ? draw.rs.values.at(RS_Z_ENABLE) : 0u)
        << " zWrite=" << (draw.rs.values.contains(RS_Z_WRITE_ENABLE) ? draw.rs.values.at(RS_Z_WRITE_ENABLE) : 0u)
        << " zFunc=" << (draw.rs.values.contains(RS_Z_FUNC) ? draw.rs.values.at(RS_Z_FUNC) : 0u)
        << " alphaBlend="
        << (draw.rs.values.contains(RS_ALPHABLEND_ENABLE) ? draw.rs.values.at(RS_ALPHABLEND_ENABLE) : 0u)
        << " srcBlend=" << (draw.rs.values.contains(RS_SRC_BLEND) ? draw.rs.values.at(RS_SRC_BLEND) : 0u)
        << " dstBlend=" << (draw.rs.values.contains(RS_DEST_BLEND) ? draw.rs.values.at(RS_DEST_BLEND) : 0u)
        << " forceVisible=" << (debug::forceVisibleDraw() ? 1 : 0);
    emitQueueTraceLine(out.str());
  }
  if (indexedDraw) {
    std::span<const u8> indexBytes;
    if (!draw.userIndexData.empty()) {
      indexBytes = draw.userIndexData;
    } else {
      auto* indexRecord = ctx.pool.findBuffer(draw.indexBuffer.value);
      if (indexRecord && !indexRecord->shadow.empty()) {
        indexBytes = indexRecord->shadow;
      } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
        indexBytes = std::span<const u8>(static_cast<const u8*>(indexRecord->contents),
                                         static_cast<std::size_t>(indexRecord->desc.size));
      }
    }
    const std::size_t stride = static_cast<std::size_t>(ffLayout ? (uniforms->vertexStreamStride ? uniforms->vertexStreamStride
                                                                                       : ffLayout->stride)
                                                       : computeVertexDeclStride(draw));
    const std::size_t streamBase = static_cast<std::size_t>(draw.vertexDecl.streams[0].offset);
    const std::size_t firstIndexByte = static_cast<std::size_t>(draw.startIndex) * indexElementSize(draw.indexType);
    if (debug::forceExpandIndexed()) {
      std::ostringstream out;
      out << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
          << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
          << " ff=" << (ffLayout ? 1 : 0)
          << " vertexBytes=" << vertexBytes.size()
          << " indexBytes=" << indexBytes.size()
          << " stride=" << stride
          << " startIndex=" << draw.startIndex
          << " baseVertex=" << draw.baseVertexIndex;
      emitQueueTraceLine(out.str());
    }
    if (!vertexBytes.empty() && !indexBytes.empty() && stride != 0) {
      std::vector<u8> expandedVertices(static_cast<std::size_t>(vertexCount) * stride, 0);
      for (uint64_t i = 0; i < vertexCount; ++i) {
        i32 vertexIndex = draw.baseVertexIndex;
        bool haveIndex = false;
        if (draw.indexType == IndexType::UInt16 &&
            firstIndexByte + static_cast<std::size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
          u16 index = 0;
          std::memcpy(&index, indexBytes.data() + firstIndexByte + static_cast<std::size_t>(i) * sizeof(u16),
                      sizeof(u16));
          vertexIndex += static_cast<i32>(index);
          haveIndex = true;
        } else if (draw.indexType == IndexType::UInt32 &&
                   firstIndexByte + static_cast<std::size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
          u32 index = 0;
          std::memcpy(&index, indexBytes.data() + firstIndexByte + static_cast<std::size_t>(i) * sizeof(u32),
                      sizeof(u32));
          vertexIndex += static_cast<i32>(index);
          haveIndex = true;
        }
        if (!haveIndex || vertexIndex < 0) {
          continue;
        }
        const std::size_t sourceOffset = streamBase + static_cast<std::size_t>(vertexIndex) * stride;
        if (sourceOffset + stride > vertexBytes.size()) {
          continue;
        }
        std::memcpy(expandedVertices.data() + static_cast<std::size_t>(i) * stride,
                    vertexBytes.data() + sourceOffset, stride);
      }
      {
        WMTBufferInfo bi{};
        bi.length = expandedVertices.size();
        bi.options = WMTResourceStorageModeShared;
        bi.memory.set((void *)expandedVertices.data());
        transientVertexBuffer = ctx.device.newBuffer(bi);
      }
      if (transientVertexBuffer) {
        encoder.setVertexBuffer(transientVertexBuffer, 0, 1);
        if (ffLayout && ffLayout->preTransformed && vertexCount >= 6 && draw.textures[0].handle != Handle{}) {
          const bool traceExpanded = [] {
            const char* env = std::getenv("DXMT_TRACE_FVF_EXPANDED");
            return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
          }();
          if (traceExpanded) {
            auto readExpandedF32 = [&](std::size_t absoluteOffset) {
              float value = 0.0f;
              if (absoluteOffset + sizeof(float) <= expandedVertices.size()) {
                std::memcpy(&value, expandedVertices.data() + absoluteOffset, sizeof(float));
              }
              return value;
            };
            std::ostringstream trace;
            trace << "[dxmt9-expanded] seq=" << static_cast<unsigned long long>(seqId)
                  << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
                  << " stride=" << stride;
            for (uint64_t i = 0; i < std::min<uint64_t>(vertexCount, 6); ++i) {
              const std::size_t base = static_cast<std::size_t>(i) * stride;
              trace << " v" << i << "=("
                    << readExpandedF32(base + ffLayout->positionOffset + 0) << ","
                    << readExpandedF32(base + ffLayout->positionOffset + 4) << ","
                    << readExpandedF32(base + ffLayout->positionOffset + 8) << ","
                    << readExpandedF32(base + ffLayout->positionOffset + 12) << ")";
              if (ffLayout->hasTexcoord[0]) {
                trace << " uv" << i << "=("
                      << readExpandedF32(base + ffLayout->texcoordOffset[0] + 0) << ","
                      << readExpandedF32(base + ffLayout->texcoordOffset[0] + 4) << ")";
              }
            }
            emitQueueTraceLine(trace.str());
          }
        }
        vertexBytes = std::span<const u8>(expandedVertices.data(), expandedVertices.size());
        uniforms->vertexStreamOffset = 0;
        uniforms->vertexBaseIndex = 0;
        {
          WMTBufferInfo bi{}; bi.length = sizeof(DrawUniforms);
          bi.options = WMTResourceStorageModeShared; bi.memory.set((void *)uniforms);
          transientUniformBuffer = ctx.device.newBuffer(bi);
        }
        if (!transientUniformBuffer) {
          return;
        }
        encoder.setVertexBuffer(transientUniformBuffer, 0, 0);
        encoder.setFragmentBuffer(transientUniformBuffer, 0, 0);
        expandedIndexedDraw = true;
      }
    }
    if (debug::forceExpandIndexed()) {
      std::ostringstream out;
      out << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
          << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
          << " expanded=" << (expandedIndexedDraw ? 1 : 0);
      emitQueueTraceLine(out.str());
    }
    if (expandedIndexedDraw) {
      encoder.drawPrimitives(primitiveType, 0, (uint64_t)vertexCount);
      return;
    }
    WMT::Reference<WMT::Buffer> transientIndexBuffer;
    WMT::Buffer indexBuffer{};
    uint64_t indexBufferOffset = static_cast<uint64_t>(draw.startIndex) * indexElementSize(draw.indexType);
    if (!draw.userIndexData.empty()) {
      WMTBufferInfo bi{}; bi.length = draw.userIndexData.size();
      bi.options = WMTResourceStorageModeShared;
      bi.memory.set((void *)draw.userIndexData.data());
      transientIndexBuffer = ctx.device.newBuffer(bi);
      indexBuffer = transientIndexBuffer;
    } else {
      auto* buffer = ctx.pool.findBuffer(draw.indexBuffer.value);
      if (buffer && buffer->buffer) {
        indexBuffer = WMT::Buffer{buffer->buffer.handle};
      } else if (buffer && !buffer->shadow.empty()) {
        WMTBufferInfo bi{}; bi.length = buffer->shadow.size();
        bi.options = WMTResourceStorageModeShared;
        bi.memory.set((void *)buffer->shadow.data());
        transientIndexBuffer = ctx.device.newBuffer(bi);
        indexBuffer = transientIndexBuffer;
      }
    }
    if (indexBuffer) {
      encoder.drawIndexedPrimitives(primitiveType, toIndexType(draw.indexType),
                                    (uint64_t)vertexCount, indexBuffer, indexBufferOffset,
                                    1, 0, 0);
      return;
    }
  }
  encoder.drawPrimitives(primitiveType, 0, (uint64_t)vertexCount);
}

std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot) {
  @autoreleasepool {
  if (!ctx.device || !ctx.queue.valid()) {
    return std::nullopt;
  }

  auto ownedCommandBuffer = ctx.queue.newCommandBuffer();
  if (!ownedCommandBuffer) {
    return std::nullopt;
  }
  auto commandBuffer = ownedCommandBuffer;

  // Deferred-upload fence: flush any pending staging→private blits via
  // the queue-owned ResourceInitializer, then wait for its SharedEvent
  // signal at the head of this chunk's command buffer so textures are
  // fully populated before any draw samples them.
  const auto initializerFlush = ctx.queue.flushInitializerUploads();
  if (initializerFlush.event && initializerFlush.value > 0) {
    commandBuffer.encodeWaitForEvent(initializerFlush.event, initializerFlush.value);
  }

  WMT::Reference<WMT::RenderCommandEncoder> activeRenderEncoder{};
  WMT::Reference<WMT::BlitCommandEncoder> activeBlitEncoder{};
  AttachmentKey activeKey{};
  HazardBloom activeWriteBloom{};
  bool hasActiveRender = false;
  std::optional<core::ClearDesc> pendingClear;

  auto flushRender = [&] {
    if (activeRenderEncoder) {
      activeRenderEncoder.endEncoding();
      activeRenderEncoder = {};
      hasActiveRender = false;
    }
  };

  auto flushBlit = [&] {
    if (activeBlitEncoder) {
      activeBlitEncoder.endEncoding();
      activeBlitEncoder = {};
    }
  };

  auto startRenderPass = [&](const core::DrawDesc& draw, const std::optional<core::ClearDesc>& clear) {
    activeRenderEncoder = beginRenderPass(ctx, commandBuffer, draw, clear);
    hasActiveRender = static_cast<bool>(activeRenderEncoder);
    activeKey = makeAttachmentKey(draw.rts);
    activeWriteBloom = makeAttachmentBloom(draw.rts);
  };

  auto flushPendingClear = [&] {
    if (!pendingClear.has_value()) return;
    dxmt9::encoders::encodeClearPass(commandBuffer, ctx.pool, *pendingClear);
    pendingClear.reset();
  };

  using Kind = core::MetalCommandRecord::Kind;
  for (const auto& command : slot.commands) {
    switch (command.kind) {
      case Kind::Clear:
        flushRender();
        flushBlit();
        if (command.clear.rects.empty()) {
          pendingClear = command.clear;
        } else {
          dxmt9::encoders::encodeClearPass(commandBuffer, ctx.pool, command.clear);
        }
        break;
      case Kind::Draw: {
        flushBlit();
        const auto drawKey = makeAttachmentKey(command.draw.rts);
        const auto drawReadBloom = makeDrawReadBloom(command.draw);
        if (pendingClear.has_value()) {
          const auto clearKey = makeAttachmentKey(*pendingClear);
          const auto clearBloom = makeAttachmentBloom(*pendingClear);
          if (clearKey == drawKey && !clearBloom.overlaps(drawReadBloom)) {
            startRenderPass(command.draw, pendingClear);
            pendingClear.reset();
          } else {
            flushPendingClear();
            if (!hasActiveRender || activeKey != drawKey || activeWriteBloom.overlaps(drawReadBloom)) {
              flushRender();
              startRenderPass(command.draw, std::nullopt);
            }
          }
        } else if (!hasActiveRender || activeKey != drawKey || activeWriteBloom.overlaps(drawReadBloom)) {
          flushRender();
          startRenderPass(command.draw, std::nullopt);
        }
        encodeDraw(ctx, commandBuffer, activeRenderEncoder, command.draw, slot.seqId);
        break;
      }
      case Kind::SurfaceCopy:
        flushPendingClear();
        flushRender();
        dxmt9::encoders::encodeSurfaceCopy(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                           ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                           command.surfaceCopy);
        break;
      case Kind::StretchRect:
        flushPendingClear();
        flushRender();
        dxmt9::encoders::encodeStretchRect(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                            ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                            command.stretchRect);
        break;
      case Kind::Readback:
        flushPendingClear();
        flushRender();
        dxmt9::encoders::encodeReadback(commandBuffer, ctx.pool, command.readback);
        break;
      case Kind::ColorFill:
        flushPendingClear();
        flushRender();
        dxmt9::encoders::encodeColorFill(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                          ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                          command.colorFill);
        break;
      case Kind::Present:
        flushPendingClear();
        flushRender();
        flushBlit();
        if (dxmt9::encodePresent(commandBuffer, ctx.pool,
                                  command.present, command.presentSource, slot.seqId)) {
          ctx.queue.backBufferDiscardAfterPresent_ = true;
        }
        break;
    }
  }

  flushPendingClear();
  flushRender();
  flushBlit();

  const u64 seqId = slot.seqId;
  core::metalqueue::QueueSubmissionRecord record;
  record.commandBuffer = std::move(commandBuffer);
  record.slotIndex = slotIndex;
  record.seqId = seqId;
  record.commands = std::span<const core::MetalCommandRecord>(slot.commands.data(), slot.commands.size());
  record.context = "queue";
  return record;
  }  // @autoreleasepool
}

}  // namespace dxmt9::encoders
