#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_blit_encoders.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_command_queue.hpp"
#include "dxmt9_device.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_state.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_presenter.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace dxmt9::encoders {

using core::ClearDesc;
using core::Handle;
using core::IndexType;
using core::SamplerSnapshot;
using core::SAMP_ADDRESS_U;
using core::SAMP_ADDRESS_V;
using core::SAMP_ADDRESS_W;
using core::SAMP_BORDER_COLOR;
using core::SAMP_MAG_FILTER;
using core::SAMP_MAX_ANISOTROPY;
using core::SAMP_MIN_FILTER;
using core::SAMP_MIP_FILTER;
using core::kMaxSamplers;
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

class PerfScope {
 public:
  explicit PerfScope(void (*record)(std::uint64_t)) : record_(record) {}
  ~PerfScope() {
    if (!record_) {
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    record_(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  }

  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  void (*record_)(std::uint64_t) = nullptr;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

void countTextureBind() {
  perf::countBaseStateBind(1, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

void countSamplerBind() {
  perf::countBaseStateBind(0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
}

void countVertexBufferBind() {
  perf::countBaseStateBind(0, 0, 1, 0, 0, 0, 0, 0, 0, 0);
}

void countIndexBufferBind() {
  perf::countBaseStateBind(0, 0, 0, 1, 0, 0, 0, 0, 0, 0);
}

void countUniformBufferBinds(std::uint32_t count) {
  perf::countBaseStateBind(0, 0, 0, 0, count, 0, 0, 0, 0, 0);
}

void countPipelineBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 1, 0, 0, 0, 0);
}

void countDepthStateBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 1, 0, 0, 0);
}

void countViewportBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 0, 1, 0, 0);
}

void countScissorBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 0, 0, 1, 0);
}

void countRasterizerBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
}

bool splitPresentBeforeAcquireEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_SPLIT_PRESENT_ACQUIRE");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool presentBoundaryAfterAcquireEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

WMTWinding frontFaceWinding() {
  return debug::frontFaceCounterClockwise() ? WMTWindingCounterClockwise : WMTWindingClockwise;
}

WMTCullMode applyDebugCullOverride(WMTCullMode cullMode) {
  const char* env = std::getenv("DXMT_DEBUG_FORCE_CULL_MODE");
  if (!env || env[0] == '\0') {
    return cullMode;
  }
  if (std::strcmp(env, "none") == 0) {
    return WMTCullModeNone;
  }
  if (std::strcmp(env, "front") == 0) {
    return WMTCullModeFront;
  }
  if (std::strcmp(env, "back") == 0) {
    return WMTCullModeBack;
  }
  return cullMode;
}

void setRasterizerCullMode(WMT::RenderCommandEncoder& encoder, WMTCullMode cullMode) {
  cullMode = applyDebugCullOverride(cullMode);
  encoder.setRasterizerState(WMTTriangleFillModeFill, cullMode, WMTDepthClipModeClip,
                             frontFaceWinding(), 0.0f, 0.0f, 0.0f);
  countRasterizerBind();
}

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

struct HazardHandles {
  static constexpr std::size_t kCapacity =
      1u + core::kMaxRenderTargets + core::kMaxStreams + core::kMaxTextureStages;
  std::array<u64, kCapacity> handles{};
  std::size_t count = 0;

  void add(u64 value) {
    if (value == 0) return;
    for (std::size_t i = 0; i < count; ++i) {
      if (handles[i] == value) return;
    }
    if (count < handles.size()) {
      handles[count++] = value;
    }
  }

  bool overlaps(const HazardHandles& other) const {
    for (std::size_t i = 0; i < count; ++i) {
      for (std::size_t j = 0; j < other.count; ++j) {
        if (handles[i] == other.handles[j]) return true;
      }
    }
    return false;
  }
};

struct HazardProbe {
  HazardBloom bloom;
  HazardHandles exact;

  void add(u64 value) {
    bloom.add(value);
    exact.add(value);
  }

  bool bloomOverlaps(const HazardProbe& other) const {
    return bloom.overlaps(other.bloom);
  }

  bool exactOverlaps(const HazardProbe& other) const {
    return exact.overlaps(other.exact);
  }
};

HazardProbe makeAttachmentHazard(const core::FlatDrawStateRecord& hot) {
  HazardProbe hazard;
  for (const auto& attachment : hot.colorAttachments) hazard.add(attachment.handle.value);
  hazard.add(hot.depthStencil.handle.value);
  return hazard;
}

HazardProbe makeAttachmentHazard(const core::ClearDesc& clear) {
  HazardProbe hazard;
  if (clear.clearColor) {
    for (const auto& attachment : clear.colorAttachments) hazard.add(attachment.handle.value);
  }
  if (clear.clearDepth || clear.clearStencil) {
    hazard.add(clear.depthStencil.handle.value);
  }
  return hazard;
}

HazardProbe makeDrawReadHazard(core::FlatDrawStateView state) {
  HazardProbe hazard;
  const auto& hot = *state.hot;
  hazard.add(hot.indexBuffer.value);
  for (const auto& handle : hot.streamBuffers) {
    hazard.add(handle.value);
  }
  for (const auto& texture : hot.textures) hazard.add(texture.value);
  return hazard;
}

AttachmentKey makeAttachmentKey(const core::FlatDrawStateRecord& hot) {
  AttachmentKey key;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    key.colorHandles[i] = hot.colorAttachments[i].handle.value;
    key.sampleCount = std::max(key.sampleCount, hot.colorAttachments[i].sampleCount);
  }
  key.depthHandle = hot.depthStencil.handle.value;
  key.sampleCount = std::max(key.sampleCount, hot.depthStencil.sampleCount);
  return key;
}

AttachmentKey makeAttachmentKey(const core::ClearDesc& clear) {
  AttachmentKey key;
  if (clear.clearColor) {
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      key.colorHandles[i] = clear.colorAttachments[i].handle.value;
      key.sampleCount = std::max(key.sampleCount, clear.colorAttachments[i].sampleCount);
    }
  }
  if (clear.clearDepth || clear.clearStencil) {
    key.depthHandle = clear.depthStencil.handle.value;
    key.sampleCount = std::max(key.sampleCount, clear.depthStencil.sampleCount);
  }
  return key;
}

bool clearMatchesColorAttachment(const std::optional<ClearDesc>& clear,
                                 std::size_t index,
                                 Handle attachment) {
  return clear.has_value() && clear->clearColor && attachment &&
         clear->colorAttachments[index].handle == attachment;
}

bool clearMatchesDepthStencilAttachment(const std::optional<ClearDesc>& clear,
                                        Handle attachment,
                                        bool clearStencil) {
  if (!clear.has_value() || !attachment) {
    return false;
  }
  const bool requested = clearStencil ? clear->clearStencil : clear->clearDepth;
  return requested && clear->depthStencil.handle == attachment;
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

u64 shaderVariantHashForDraw(core::FlatDrawStateView drawState) {
  if (!drawState.hot || !drawState.hasShaderContext()) {
    return 0;
  }
  const auto& hot = *drawState.hot;
  const auto& shader = drawState.shaderContext();
  u64 hash = shader.vertexShader.hash ^ (shader.pixelShader.hash << 1) ^
             (hot.key.vertexDeclHash << 2) ^ hot.key.renderStateHash ^
             (hot.textureMask << 3) ^ (hot.renderTargetMask << 4);
  hash ^= hot.vertexConstantsHash << 1;
  hash ^= hot.pixelConstantsHash << 2;
  return hash;
}

void countDrawIssue(core::FlatDrawStateView drawState,
                    core::PrimitiveType primitiveType,
                    u32 primitiveCount,
                    u64 vertexCount,
                    bool indexed,
                    bool expandedIndexed,
                    std::size_t userVertexBytes,
                    std::size_t userIndexBytes) {
  if (drawState.hasShaderContext()) {
    const auto& shader = drawState.shaderContext();
    perf::countDrawShaderBucket(shader.vertexShader.hash,
                                shader.pixelShader.hash,
                                shaderVariantHashForDraw(drawState));
  }
  perf::countDrawCall(static_cast<std::uint32_t>(primitiveType),
                      primitiveCount,
                      vertexCount,
                      indexed,
                      expandedIndexed,
                      userVertexBytes,
                      userIndexBytes);
}

std::size_t indexElementSize(IndexType type) {
  return type == IndexType::UInt16 ? 2u : 4u;
}

u32 samplerStateOr(const SamplerSnapshot& snapshot, u32 state, u32 fallback) {
  const auto it = snapshot.states.find(state);
  return it != snapshot.states.end() ? it->second : fallback;
}

u32 samplerStateOr(const core::FlatStateSet<core::kMaxSamplerStates>& states,
                   u32 state,
                   u32 fallback) {
  return core::flatStateOr(states, state, fallback);
}

WMTSamplerAddressMode resolveSamplerAddressMode(u32 value) {
  switch (value) {
    case 1u: return WMTSamplerAddressModeRepeat;
    case 2u: return WMTSamplerAddressModeMirrorRepeat;
    case 4u: return WMTSamplerAddressModeClampToBorderColor;
    case 3u:
    default: return WMTSamplerAddressModeClampToEdge;
  }
}

WMTSamplerBorderColor resolveSamplerBorderColor(u32 value) {
  switch (value) {
    case 0x00000000u: return WMTSamplerBorderColorTransparentBlack;
    case 0xff000000u: return WMTSamplerBorderColorOpaqueBlack;
    case 0xffffffffu: return WMTSamplerBorderColorOpaqueWhite;
    default: return (value >> 24) == 0u ? WMTSamplerBorderColorTransparentBlack : WMTSamplerBorderColorOpaqueBlack;
  }
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

WMTSamplerInfo makeSamplerInfo(const SamplerSnapshot& snapshot) {
  const auto minFilter = samplerStateOr(snapshot, SAMP_MIN_FILTER, 0u);
  const auto magFilter = samplerStateOr(snapshot, SAMP_MAG_FILTER, 0u);
  const auto mipFilter = samplerStateOr(snapshot, SAMP_MIP_FILTER, 0u);
  const auto addressU = samplerStateOr(snapshot, SAMP_ADDRESS_U, 1u);
  const auto addressV = samplerStateOr(snapshot, SAMP_ADDRESS_V, 1u);
  const auto addressW = samplerStateOr(snapshot, SAMP_ADDRESS_W, 1u);
  const auto borderColor = samplerStateOr(snapshot, SAMP_BORDER_COLOR, 0u);
  const auto maxAnisotropy = samplerStateOr(snapshot, SAMP_MAX_ANISOTROPY, 0u);
  WMTSamplerInfo info{};
  info.min_filter = minFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  info.mag_filter = magFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  switch (mipFilter) {
    case 2u: info.mip_filter = WMTSamplerMipFilterLinear; break;
    case 1u: info.mip_filter = WMTSamplerMipFilterNearest; break;
    default: info.mip_filter = WMTSamplerMipFilterNotMipmapped; break;
  }
  info.s_address_mode = resolveSamplerAddressMode(addressU);
  info.t_address_mode = resolveSamplerAddressMode(addressV);
  info.r_address_mode = resolveSamplerAddressMode(addressW);
  if (info.s_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.t_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.r_address_mode == WMTSamplerAddressModeClampToBorderColor) {
    info.border_color = resolveSamplerBorderColor(borderColor);
  }
  // Keep explicit texldl / mip-filter sampling from being clamped to level 0.
  // LOD bias is still intentionally ignored because WMTSamplerInfo has no
  // separate bias field.
  info.lod_max_clamp = 1e9f;
  info.max_anisotroy = maxAnisotropy;
  info.normalized_coords = true;
  return info;
}

WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const SamplerSnapshot& snapshot) {
  auto info = makeSamplerInfo(snapshot);
  return device.newSamplerState(info);
}

WMTSamplerInfo makeSamplerInfo(const core::FlatStateSet<core::kMaxSamplerStates>& states) {
  const auto minFilter = samplerStateOr(states, SAMP_MIN_FILTER, 0u);
  const auto magFilter = samplerStateOr(states, SAMP_MAG_FILTER, 0u);
  const auto mipFilter = samplerStateOr(states, SAMP_MIP_FILTER, 0u);
  const auto addressU = samplerStateOr(states, SAMP_ADDRESS_U, 1u);
  const auto addressV = samplerStateOr(states, SAMP_ADDRESS_V, 1u);
  const auto addressW = samplerStateOr(states, SAMP_ADDRESS_W, 1u);
  const auto borderColor = samplerStateOr(states, SAMP_BORDER_COLOR, 0u);
  const auto maxAnisotropy = samplerStateOr(states, SAMP_MAX_ANISOTROPY, 0u);
  WMTSamplerInfo info{};
  info.min_filter = minFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  info.mag_filter = magFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  switch (mipFilter) {
    case 2u: info.mip_filter = WMTSamplerMipFilterLinear; break;
    case 1u: info.mip_filter = WMTSamplerMipFilterNearest; break;
    default: info.mip_filter = WMTSamplerMipFilterNotMipmapped; break;
  }
  info.s_address_mode = resolveSamplerAddressMode(addressU);
  info.t_address_mode = resolveSamplerAddressMode(addressV);
  info.r_address_mode = resolveSamplerAddressMode(addressW);
  if (info.s_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.t_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.r_address_mode == WMTSamplerAddressModeClampToBorderColor) {
    info.border_color = resolveSamplerBorderColor(borderColor);
  }
  info.lod_max_clamp = 1e9f;
  info.max_anisotroy = maxAnisotropy;
  info.normalized_coords = true;
  return info;
}

WMT::Reference<WMT::SamplerState> makeSampler(
    WMT::Reference<WMT::Device> device,
    const core::FlatStateSet<core::kMaxSamplerStates>& states) {
  auto info = makeSamplerInfo(states);
  return device.newSamplerState(info);
}

WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    core::FlatDrawStateView drawState,
    const std::optional<ClearDesc>& clear) {
  const auto& hot = *drawState.hot;
  auto* primarySurface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
  if (!primarySurface || !primarySurface->texture) {
    return {};
  }
  WMTRenderPassInfo passInfo{};
  const bool discardAfterPresent = !clear.has_value() && ctx.queue.backBufferDiscardAfterPresent_ &&
                                   hot.colorAttachments[0].handle == ctx.queue.currentBackBuffer_;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    auto* surface = ctx.pool.findSurface(hot.colorAttachments[i].handle.value);
    if (!surface || !surface->texture) {
      continue;
    }
    auto& attachment = passInfo.colors[i];
    attachment.texture = surface->texture.handle;
    const bool discardAttachment = discardAfterPresent && i == 0;
    const bool clearAttachment =
        clearMatchesColorAttachment(clear, i, hot.colorAttachments[i].handle);
    attachment.load_action = clearAttachment ? WMTLoadActionClear
                                             : (discardAttachment ? WMTLoadActionDontCare
                                                                  : WMTLoadActionLoad);
    attachment.store_action = WMTStoreActionStore;
    if (surface->resolveTexture) {
      attachment.resolve_texture = surface->resolveTexture.handle;
      attachment.store_action = WMTStoreActionMultisampleResolve;
    }
    if (clearAttachment) {
      attachment.clear_color = WMTClearColor{clear->color.r, clear->color.g,
                                             clear->color.b, clear->color.a};
    }
  }

  if (auto* depthSurface = ctx.pool.findSurface(hot.depthStencil.handle.value);
      depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    const bool clearDepth = clearMatchesDepthStencilAttachment(clear, hot.depthStencil.handle, false);
    const bool clearStencil = clearMatchesDepthStencilAttachment(clear, hot.depthStencil.handle, true);
    if (formatHasDepthAspect(depthSurface->desc.format)) {
      passInfo.depth.texture = depthSurface->texture.handle;
      passInfo.depth.load_action = clearDepth ? WMTLoadActionClear : WMTLoadActionLoad;
      passInfo.depth.store_action = WMTStoreActionStore;
      if (clearDepth) {
        passInfo.depth.clear_depth = clear->depth;
      }
    }
    if (formatHasStencilAspect(depthSurface->desc.format)) {
      passInfo.stencil.texture = depthSurface->texture.handle;
      passInfo.stencil.load_action = clearStencil ? WMTLoadActionClear : WMTLoadActionLoad;
      passInfo.stencil.store_action = WMTStoreActionStore;
      if (clearStencil) {
        passInfo.stencil.clear_stencil = clear->stencil;
      }
    }
  }

  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) {
    return {};
  }
  perf::countRenderPassBegin();
  if (discardAfterPresent) {
    ctx.queue.backBufferDiscardAfterPresent_ = false;
  }
  const auto ffLayout = drawState.hasShaderContext()
      ? decodeFixedFunctionVertexLayout(drawState.shaderContext().vertexDecl)
      : std::optional<dxmt9::ffp::FixedFunctionVertexLayout>{};
  double viewportWidth = static_cast<double>(std::max(1u, hot.viewport.viewport.width));
  double viewportHeight = static_cast<double>(std::max(1u, hot.viewport.viewport.height));
  double viewportOriginX = static_cast<double>(hot.viewport.viewport.x);
  double viewportOriginY = static_cast<double>(hot.viewport.viewport.y);
  if (ffLayout && ffLayout->preTransformed) {
    viewportOriginX = 0.0;
    viewportOriginY = 0.0;
    viewportWidth = static_cast<double>(std::max(1u, primarySurface->desc.width));
    viewportHeight = static_cast<double>(std::max(1u, primarySurface->desc.height));
  }
  WMTViewport vp{viewportOriginX, viewportOriginY, viewportWidth, viewportHeight,
                 static_cast<double>(hot.viewport.viewport.minZ),
                 static_cast<double>(hot.viewport.viewport.maxZ)};
  encoder.setViewport(vp);
  countViewportBind();
  encoder.setRasterizerState(WMTTriangleFillModeFill, WMTCullModeNone,
                              WMTDepthClipModeClip, frontFaceWinding(),
                              0.0f, 0.0f, 0.0f);
  countRasterizerBind();
  return WMT::Reference<WMT::RenderCommandEncoder>(encoder);
}

// Per-draw view from DrawParam. Constructed once at encodeDraw entry; all
// per-draw field reads inside the function go through this view.
struct ParamView {
  core::PrimitiveType primitiveType;
  u32 primitiveCount;
  u32 startVertex;
  i32 baseVertexIndex;
  u32 startIndex;
  core::IndexType indexType;
  bool indexed;
  std::span<const u8> userVertexData;
  std::span<const u8> userIndexData;
};

std::span<const u8> drawParamVertexBytes(const core::DrawParam& param,
                                         std::span<const u8> arena) {
  if (!param.userVertexRange.empty()) {
    return core::drawRunPayloadBytes(param.userVertexRange, arena);
  }
  return {};
}

std::span<const u8> drawParamIndexBytes(const core::DrawParam& param,
                                        std::span<const u8> arena) {
  if (!param.userIndexRange.empty()) {
    return core::drawRunPayloadBytes(param.userIndexRange, arena);
  }
  return {};
}

std::uint64_t drawGeometryTraceInterval() {
  static const std::uint64_t value = [] {
    const char* env = std::getenv("DXMT9_TRACE_DRAW_GEOMETRY");
    if (!env || env[0] == '\0' || env[0] == '0') {
      return 0ull;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    if (end != env && parsed > 1ull) {
      return parsed;
    }
    return 1ull;
  }();
  return value;
}

std::uint64_t drawGeometryTraceLimit() {
  static const std::uint64_t value = [] {
    const char* env = std::getenv("DXMT9_TRACE_DRAW_GEOMETRY_LIMIT");
    if (!env || env[0] == '\0' || env[0] == '0') {
      return 0ull;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    return end != env ? parsed : 0ull;
  }();
  return value;
}

std::optional<std::uint64_t> nextDrawGeometryTraceSample() {
  const auto interval = drawGeometryTraceInterval();
  if (interval == 0ull) {
    return std::nullopt;
  }
  static std::atomic<std::uint64_t> drawCounter{0};
  static std::atomic<std::uint64_t> emittedCounter{0};
  const auto drawNo = drawCounter.fetch_add(1, std::memory_order_relaxed) + 1ull;
  if (interval > 1ull && (drawNo % interval) != 0ull) {
    return std::nullopt;
  }
  const auto limit = drawGeometryTraceLimit();
  if (limit != 0ull &&
      emittedCounter.fetch_add(1, std::memory_order_relaxed) >= limit) {
    return std::nullopt;
  }
  return drawNo;
}

const char* indexTypeName(IndexType type) {
  return type == IndexType::UInt32 ? "u32" : "u16";
}

const char* drawGeometrySourceName(bool direct, bool up, bool expanded) {
  if (expanded) {
    return "expanded";
  }
  if (up) {
    return "up";
  }
  return direct ? "direct" : "unknown";
}

const char* metalDrawMethodName(bool indexed, bool expanded) {
  if (indexed && !expanded) {
    return "drawIndexedPrimitives";
  }
  return "drawPrimitives";
}

bool drawUsesFixedFunctionPath(core::FlatDrawStateView drawState, bool hasFfpLayout) {
  if (!drawState.hasShaderContext()) {
    return hasFfpLayout;
  }
  return drawState.shaderContext().vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex;
}

void appendVertexDeclSummary(std::ostringstream& out,
                             const core::VertexDeclSnapshot& vertexDecl) {
  out << " elems=" << vertexDecl.elements.size()
      << " decl=[";
  for (std::size_t i = 0; i < vertexDecl.elements.size(); ++i) {
    if (i) {
      out << ';';
    }
    const auto& e = vertexDecl.elements[i];
    out << "{s=" << e.stream
        << ",off=" << e.offset
        << ",type=" << e.type
        << ",method=" << e.method
        << ",usage=" << e.usage
        << ",idx=" << e.usageIndex
        << "}";
  }
  out << "]";
}

void recordDrawGeometryDiagnostics(core::FlatDrawStateView drawState,
                                   const ParamView& pv,
                                   u64 seqId,
                                   u64 vertexCount,
                                   u64 vertexBufferOffset,
                                   u32 vertexStreamOffset,
                                   u32 vertexStreamStride,
                                   bool indexed,
                                   bool direct,
                                   bool up,
                                   bool expanded,
                                   bool fixedFunctionPath) {
  const auto& hot = *drawState.hot;
  const auto& vertexDecl = drawState.shaderContext().vertexDecl;
  perf::countDrawGeometryDiagnostics(fixedFunctionPath,
                                     indexed,
                                     pv.indexType == IndexType::UInt32,
                                     direct,
                                     up,
                                     expanded,
                                     pv.baseVertexIndex != 0,
                                     pv.startIndex != 0u,
                                     hot.streamOffsets[0] != 0u,
                                     hot.streamStrides[0],
                                     hot.key.vertexDeclHash);

  const auto sample = nextDrawGeometryTraceSample();
  if (!sample) {
    return;
  }

  std::ostringstream out;
  out << "[dxmt9-geometry] sample=" << static_cast<unsigned long long>(*sample)
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " api="
      << (indexed ? (up ? "DrawIndexedPrimitiveUP" : "DrawIndexedPrimitive")
                  : (up ? "DrawPrimitiveUP" : "DrawPrimitive"))
      << " metal=" << metalDrawMethodName(indexed, expanded)
      << " source=" << drawGeometrySourceName(direct, up, expanded)
      << " shaderPath=" << (fixedFunctionPath ? "ffp" : "vs")
      << " indexed=" << (indexed ? 1 : 0)
      << " baseVertex=" << pv.baseVertexIndex
      << " startVertex=" << pv.startVertex
      << " startIndex=" << pv.startIndex
      << " indexType=" << indexTypeName(pv.indexType)
      << " minVertex=na numVertices=na"
      << " primType=" << static_cast<unsigned>(pv.primitiveType)
      << " primCount=" << pv.primitiveCount
      << " vertexCount=" << static_cast<unsigned long long>(vertexCount)
      << " stream0Handle=0x" << std::hex
      << static_cast<unsigned long long>(hot.streamBuffers[0].value)
      << " stream0Offset=" << std::dec << hot.streamOffsets[0]
      << " stream0Stride=" << hot.streamStrides[0]
      << " vertexBufferOffset=" << static_cast<unsigned long long>(vertexBufferOffset)
      << " uniformStreamOffset=" << vertexStreamOffset
      << " uniformStreamStride=" << vertexStreamStride
      << " declHash=0x" << std::hex << hot.key.vertexDeclHash
      << " fvf=0x" << vertexDecl.fvf << std::dec
      << " vsHash=0x" << std::hex
      << static_cast<unsigned long long>(drawState.shaderContext().vertexShader.hash)
      << " psHash=0x"
      << static_cast<unsigned long long>(drawState.shaderContext().pixelShader.hash)
      << std::dec
      << " userVertexBytes=" << pv.userVertexData.size()
      << " userIndexBytes=" << pv.userIndexData.size();
  appendVertexDeclSummary(out, vertexDecl);
  emitQueueTraceLine(out.str());
}

bool encodeDraw(EncodeContext& ctx,
                 WMT::CommandBuffer& commandBuffer,
                 WMT::RenderCommandEncoder& encoder,
                 core::FlatDrawStateView drawState,
                 u64 seqId,
                 bool skipBaseStateBind,
                 const PreUploadedDrawData* preUploaded,
                 const core::DrawParam* paramOverride,
                 std::span<const u8> paramPayloadArena) {
  PerfScope scope(perf::countEncodeDrawCpuTime);
  (void)commandBuffer;
  const auto& hot = *drawState.hot;
  const auto& shader = drawState.shaderContext();
  const auto& vertexDecl = shader.vertexDecl;
  const auto* debug = drawState.hasDebugSnapshot() ? &drawState.debugSnapshot() : nullptr;
  const ParamView pv = paramOverride
      ? ParamView{paramOverride->primitiveType,
                  paramOverride->primitiveCount,
                  paramOverride->startVertex,
                  paramOverride->baseVertexIndex,
                  paramOverride->startIndex,
                  paramOverride->indexType,
                  paramOverride->indexed,
                  drawParamVertexBytes(*paramOverride, paramPayloadArena),
                  drawParamIndexBytes(*paramOverride, paramPayloadArena)}
      : ParamView{debug ? debug->primitiveType : core::PrimitiveType::TriangleList,
                  debug ? debug->primitiveCount : 0u,
                  debug ? debug->startVertex : 0u,
                  debug ? debug->baseVertexIndex : 0,
                  debug ? debug->startIndex : 0u,
                  debug ? debug->indexType : IndexType::UInt16,
                  false,
                  {},
                  {}};
  if (debug::skipAllDraws()) {
    if (queueTraceEnabled()) {
      std::ostringstream out;
      out << "[dxmt9-debug] skip all draws seq=" << static_cast<unsigned long long>(seqId)
          << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value);
      emitQueueTraceLine(out.str());
    }
    return false;
  }
  const bool traceEncode = debug::shouldTraceEncode(hot, seqId);
  if (!encoder) {
    if (traceEncode) {
      emitQueueTraceLine("[dxmt9-encode] seq=" + std::to_string(seqId) + " skipped reason=no-encoder");
    }
    return false;
  }
  // Phase 3-E: pipeline lookup + depth state + setRenderPipelineState
  // are BaseDrawState-only and survive across iterations of a
  // Kind::DrawRun on the Metal render encoder. Skip on iter 2..N.
  if (!skipBaseStateBind) {
    const auto depthKey = makeDepthStencilKey(drawState);
    auto pipeline = ctx.cache.getOrBuildDrawPipelineForState(
        ctx.device, ctx.limits, ctx.pool, drawState, ctx.shaderArchive,
        ctx.shaderArchivePath).get();
    if (!pipeline) {
      if (traceEncode) {
        std::ostringstream out;
        out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
            << " skipped reason=no-pipeline"
            << " rt0=" << static_cast<unsigned long long>(hot.colorAttachments[0].handle.value)
            << " ds=" << static_cast<unsigned long long>(hot.depthStencil.handle.value)
            << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
            << " fvf=0x" << std::hex << vertexDecl.fvf << std::dec
            << " alphaBlend="
            << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
            << " colorWrite="
            << core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
        emitQueueTraceLine(out.str());
      }
      return false;
    }
    auto depthState = ctx.cache.depthStencilStateFor(ctx.device, depthKey);
    if (depthState) {
      encoder.setDepthStencilState(depthState);
      countDepthStateBind();
    }
    encoder.setRenderPipelineState(pipeline);
    countPipelineBind();
  }
  auto* uniforms = ctx.allocators.argbuf.allocate<DrawUniforms>(seqId);
  DrawUniforms fallbackUniforms{};
  if (!uniforms) {
    uniforms = &fallbackUniforms;
  }
  *uniforms = buildDrawUniforms(drawState);
  auto uploadTransientBuffer = [&](const void* data, std::size_t len, std::size_t alignment) {
    return ctx.queue.uploadTransientBuffer(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len),
        alignment, seqId);
  };
  auto uploadUniforms = [&] {
    auto slice = uploadTransientBuffer(uniforms, sizeof(DrawUniforms), alignof(DrawUniforms));
    if (!slice) {
      return false;
    }
    encoder.setVertexBuffer(slice.buffer, slice.offset, 0);
    encoder.setFragmentBuffer(slice.buffer, slice.offset, 0);
    countUniformBufferBinds(2);
    return true;
  };
  const auto ffLayout = decodeFixedFunctionVertexLayout(vertexDecl);
  const bool fixedFunctionPath = drawUsesFixedFunctionPath(drawState, static_cast<bool>(ffLayout));
  // Phase 3-E: viewport / scissor / cull are BaseDrawState-only.
  if (!skipBaseStateBind) {
    if (auto* surface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value); surface && surface->texture) {
      double viewportWidth = static_cast<double>(std::max(1u, hot.viewport.viewport.width));
      double viewportHeight = static_cast<double>(std::max(1u, hot.viewport.viewport.height));
      double viewportOriginX = static_cast<double>(hot.viewport.viewport.x);
      double viewportOriginY = static_cast<double>(hot.viewport.viewport.y);
      if (ffLayout && ffLayout->preTransformed) {
        viewportOriginX = 0.0;
        viewportOriginY = 0.0;
        viewportWidth = static_cast<double>(std::max(1u, surface->desc.width));
        viewportHeight = static_cast<double>(std::max(1u, surface->desc.height));
      }
      encoder.setViewport(WMTViewport{viewportOriginX, viewportOriginY, viewportWidth, viewportHeight,
                                      static_cast<double>(hot.viewport.viewport.minZ),
                                      static_cast<double>(hot.viewport.viewport.maxZ)});
      countViewportBind();
      WMTScissorRect scissor{};
      if (hot.viewport.scissorEnabled && !debug::disableScissor()) {
        scissor.x = static_cast<uint64_t>(std::max(0, hot.viewport.scissor.left));
        scissor.y = static_cast<uint64_t>(std::max(0, hot.viewport.scissor.top));
        scissor.width = static_cast<uint64_t>(std::max(0, hot.viewport.scissor.right - hot.viewport.scissor.left));
        scissor.height =
            static_cast<uint64_t>(std::max(0, hot.viewport.scissor.bottom - hot.viewport.scissor.top));
      } else {
        scissor.x = 0;
        scissor.y = 0;
        scissor.width = static_cast<uint64_t>(std::max(1u, surface->desc.width));
        scissor.height = static_cast<uint64_t>(std::max(1u, surface->desc.height));
      }
      encoder.setScissorRect(scissor);
      countScissorBind();
      if (ffLayout && ffLayout->preTransformed) {
        setRasterizerCullMode(encoder, WMTCullModeNone);
      } else if (debug::disableCull()) {
        setRasterizerCullMode(encoder, WMTCullModeNone);
      } else {
        setRasterizerCullMode(encoder, static_cast<WMTCullMode>(toCullMode(
            core::flatStateOr(hot.renderStates, RS_CULL_MODE, 1u))));
      }
    }
  }
  static std::atomic<int> ffTraceRemaining{debug::fixedFunctionTraceBudget()};
  const u32 primitiveCount = std::max<u32>(1, pv.primitiveCount);
  const uint64_t vertexCount =
      static_cast<uint64_t>(std::max(1u, primitiveVertexCount(pv.primitiveType, primitiveCount)));
  const bool indexedDraw = pv.indexed && (hot.indexBuffer || !pv.userIndexData.empty());
  CommandQueue::TransientBufferSlice transientVertexBuffer;
  std::span<const u8> vertexBytes;
  WMT::Buffer vertexBuffer{};
  uint64_t vertexBufferOffset = 0;
  auto makeTransientBuffer = [&](const void* data, std::size_t len) {
    return uploadTransientBuffer(data, len, 16);
  };
  if (!pv.userVertexData.empty()) {
    // Phase 5-B: prefer pre-batched UP vertex slice when the
    // DrawRun handler did the bulk upload; otherwise fall back
    // to the per-draw upload.
    if (preUploaded && preUploaded->vertex) {
      transientVertexBuffer = preUploaded->vertex;
    } else {
      transientVertexBuffer = makeTransientBuffer(pv.userVertexData.data(),
                                                 pv.userVertexData.size());
    }
    if (transientVertexBuffer) {
      vertexBuffer = transientVertexBuffer.buffer;
      vertexBufferOffset = transientVertexBuffer.offset + hot.streamOffsets[0];
      vertexBytes = pv.userVertexData;
    }
  } else if (hot.streamBuffers[0]) {
    if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[0].value);
        buffer && buffer->buffer) {
      vertexBuffer = WMT::Buffer{buffer->buffer.handle};
      vertexBufferOffset = hot.streamOffsets[0];
      if (!buffer->shadow.empty()) {
        vertexBytes = buffer->shadow;
      } else if (buffer->contents) {
        vertexBytes = std::span<const u8>(static_cast<const u8*>(buffer->contents),
                                          static_cast<std::size_t>(buffer->desc.size));
      }
    } else if (vertexDecl.streams[0].buffer) {
      const auto bytes = vertexDecl.streams[0].buffer->bytes();
      if (!bytes.empty()) {
        transientVertexBuffer = makeTransientBuffer(bytes.data(), bytes.size());
        if (transientVertexBuffer) {
          vertexBuffer = transientVertexBuffer.buffer;
          vertexBufferOffset = transientVertexBuffer.offset + hot.streamOffsets[0];
          vertexBytes = bytes;
        }
      }
    }
  }
  if (traceEncode && !ffLayout && !vertexBytes.empty() && !vertexDecl.elements.empty()) {
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
    for (const auto& element : vertexDecl.elements) {
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
      const std::size_t stride = static_cast<std::size_t>(computeVertexDeclStride(vertexDecl));
      const std::size_t streamBase = static_cast<std::size_t>(hot.streamOffsets[0]);
      std::ostringstream trace;
      trace << "[dxmt9-encode-verts] seq=" << static_cast<unsigned long long>(seqId)
            << " startVertex=" << pv.startVertex
            << " baseVertex=" << pv.baseVertexIndex
            << " stride=" << stride
            << " bytes=" << vertexBytes.size();
      const u32 tracedVertexCount = std::min<u32>(static_cast<u32>(vertexCount), 6u);
      for (u32 i = 0; i < tracedVertexCount; ++i) {
        const std::size_t base = streamBase +
                            static_cast<std::size_t>(pv.startVertex + i) * stride;
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
      return false;
    }
    uniforms->vertexStreamOffset = 0;
    uniforms->vertexStreamStride =
        vertexDecl.streams[0].stride ? vertexDecl.streams[0].stride : ffLayout->stride;
    if (!indexedDraw && uniforms->vertexStreamStride != 0u) {
      vertexBufferOffset += static_cast<uint64_t>(pv.startVertex) *
                            static_cast<uint64_t>(uniforms->vertexStreamStride);
      uniforms->vertexBaseIndex = 0;
    } else {
      uniforms->vertexBaseIndex = indexedDraw ? pv.baseVertexIndex : static_cast<i32>(pv.startVertex);
    }
    if (ffLayout->preTransformed) {
      if (auto* targetSurface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value); targetSurface) {
        uniforms->viewportOrigin = {0.0f, 0.0f};
        uniforms->viewportSize = {static_cast<f32>(std::max(1u, targetSurface->desc.width)),
                                  static_cast<f32>(std::max(1u, targetSurface->desc.height))};
      }
    }
    if (!uploadUniforms()) {
      return false;
    }
    encoder.setVertexBuffer(vertexBuffer, vertexBufferOffset, 1);
    countVertexBufferBind();

    const u64 ffTraceTex0 = debug::fixedFunctionTraceTextureHandle();
    const bool forceTrace =
        ffTraceTex0 != 0 && hot.textures[0] && hot.textures[0].value == ffTraceTex0;
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
          return core::flatStateOr(hot.textureStageStates[0], key, fallback);
        };
        const auto stageStateValueAt = [&](std::size_t stageIndex, u32 key, u32 fallback) -> u32 {
          if (stageIndex >= hot.textureStageStates.size()) {
            return fallback;
          }
          return core::flatStateOr(hot.textureStageStates[stageIndex], key, fallback);
        };
        trace << "[dxmt9-ffp] seq=" << static_cast<unsigned long long>(seqId)
              << " fvf=0x" << std::hex << vertexDecl.fvf << std::dec
              << " ffLayout=1"
              << " preT=" << (ffLayout->preTransformed ? 1 : 0)
              << " baseVertex=" << pv.baseVertexIndex
              << " startIndex=" << pv.startIndex
              << " primCount=" << pv.primitiveCount
              << " stride=" << uniforms->vertexStreamStride
              << " viewport=(" << uniforms->viewportOrigin[0] << "," << uniforms->viewportOrigin[1]
              << " " << uniforms->viewportSize[0] << "x" << uniforms->viewportSize[1] << ")"
              << " zEnable=" << core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u)
              << " zFunc=" << core::flatStateOr(hot.renderStates, RS_Z_FUNC, 0u)
              << " alphaTest="
              << core::flatStateOr(hot.renderStates, RS_ALPHA_TEST_ENABLE, 0u)
              << " alphaFunc="
              << core::flatStateOr(hot.renderStates, RS_ALPHA_FUNC,
                                   static_cast<u32>(CompareFunc::Always))
              << " alphaRef="
              << core::flatStateOr(hot.renderStates, RS_ALPHA_REF, 0u)
              << " alphaBlend="
              << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
              << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
              << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
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
              << " elems=" << vertexDecl.elements.size()
              << " tfactor=0x"
              << std::hex
              << core::flatStateOr(hot.renderStates, RS_TEXTURE_FACTOR, 0u)
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
        for (std::size_t i = 0; i < vertexDecl.elements.size(); ++i) {
          const auto& e = vertexDecl.elements[i];
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

        const std::size_t stride = static_cast<std::size_t>(
            uniforms->vertexStreamStride ? uniforms->vertexStreamStride : ffLayout->stride);
        const u32 tracedVertexCount = std::min<u32>(static_cast<u32>(vertexCount), 24u);
        for (u32 i = 0; i < tracedVertexCount; ++i) {
          const std::size_t base = static_cast<std::size_t>(hot.streamOffsets[0]) +
                                   static_cast<std::size_t>(pv.baseVertexIndex + static_cast<int>(i)) *
                                       stride;
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

        if (hot.indexBuffer) {
          const auto* indexRecord = ctx.pool.findBuffer(hot.indexBuffer.value);
          std::span<const u8> indexBytes;
          if (indexRecord && !indexRecord->shadow.empty()) {
            indexBytes = indexRecord->shadow;
          } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
            indexBytes = std::span<const u8>(static_cast<const u8*>(indexRecord->contents),
                                             static_cast<std::size_t>(indexRecord->desc.size));
          }
          if (!indexBytes.empty()) {
            trace << " idx=";
            const std::size_t start = static_cast<std::size_t>(pv.startIndex) * indexElementSize(pv.indexType);
            const u32 tracedIndexCount =
                std::min<u32>(primitiveCount * 3u, 36u);
            for (u32 i = 0; i < tracedIndexCount; ++i) {
              if (i) {
                trace << ",";
              }
              if (pv.indexType == IndexType::UInt16 &&
                  start + static_cast<std::size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
                u16 index = 0;
                std::memcpy(&index, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u16),
                            sizeof(u16));
                trace << index;
              } else if (pv.indexType == IndexType::UInt32 &&
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
              if (pv.indexType == IndexType::UInt16 &&
                  start + static_cast<std::size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
                u16 index = 0;
                std::memcpy(&index, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u16),
                            sizeof(u16));
                vertexIndex = static_cast<u32>(index);
                haveIndex = true;
              } else if (pv.indexType == IndexType::UInt32 &&
                         start + static_cast<std::size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
                std::memcpy(&vertexIndex, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u32),
                            sizeof(u32));
                haveIndex = true;
              }
              if (!haveIndex) {
                break;
              }
              const std::size_t refBase =
                  static_cast<std::size_t>(hot.streamOffsets[0]) +
                  static_cast<std::size_t>(pv.baseVertexIndex + static_cast<int>(vertexIndex)) * stride;
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
        if (hot.textures[0]) {
          trace << static_cast<unsigned long long>(hot.textures[0].value);
        } else {
          trace << 0;
        }
        trace << " tex1=";
        if (hot.textures.size() > 1 && hot.textures[1]) {
          trace << static_cast<unsigned long long>(hot.textures[1].value);
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
        ffTraceTex0 != 0 && hot.textures[0] && hot.textures[0].value == ffTraceTex0;
    if (forceTrace) {
      std::ostringstream trace;
      trace << "[dxmt9-ffp] seq=" << static_cast<unsigned long long>(seqId)
            << " fvf=0x" << std::hex << vertexDecl.fvf << std::dec
            << " ffLayout=" << (ffLayout ? 1 : 0)
            << " baseVertex=" << pv.baseVertexIndex
            << " startIndex=" << pv.startIndex
            << " primCount=" << pv.primitiveCount
            << " stride="
            << (ffLayout ? (vertexDecl.streams[0].stride ? vertexDecl.streams[0].stride : ffLayout->stride)
                         : computeVertexDeclStride(vertexDecl))
            << " elems=" << vertexDecl.elements.size();
      for (std::size_t i = 0; i < vertexDecl.elements.size(); ++i) {
        const auto& e = vertexDecl.elements[i];
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
        ffLayout ? (vertexDecl.streams[0].stride ? vertexDecl.streams[0].stride : ffLayout->stride)
                 : computeVertexDeclStride(vertexDecl);
    if (!indexedDraw && uniforms->vertexStreamStride != 0u) {
      vertexBufferOffset += static_cast<uint64_t>(pv.startVertex) *
                            static_cast<uint64_t>(uniforms->vertexStreamStride);
      uniforms->vertexBaseIndex = 0;
    } else {
      uniforms->vertexBaseIndex = indexedDraw ? pv.baseVertexIndex : static_cast<i32>(pv.startVertex);
    }
    if (!uploadUniforms()) {
      return false;
    }
    encoder.setVertexBuffer(vertexBuffer, vertexBufferOffset, 1);
    countVertexBufferBind();
  }
  // Phase 3-E: texture / sampler binding is BaseDrawState-only.
  if (!skipBaseStateBind) {
    for (std::size_t stage = 0; stage < kMaxSamplers; ++stage) {
      const auto textureHandle = hot.textures[stage];
      if (!textureHandle) {
        continue;
      }
      if (const u64 skipped = debug::skippedTextureHandle();
          skipped != 0ull && textureHandle.value == skipped) {
        if (traceEncode || debug::shouldTraceTexture(textureHandle)) {
          std::ostringstream out;
          out << "[dxmt9-debug] skip draw seq=" << static_cast<unsigned long long>(seqId)
              << " tex" << stage << "=" << static_cast<unsigned long long>(textureHandle.value);
          emitQueueTraceLine(out.str());
        }
        return false;
      }
      if (auto* texture = ctx.pool.findTexture(textureHandle.value); texture && texture->texture) {
        if (debug::shouldTraceTexture(textureHandle)) {
          std::ostringstream out;
          out << "[dxmt9-texture] bind stage=" << stage
              << " handle=0x" << std::hex << textureHandle.value << std::dec
              << " format=" << static_cast<unsigned>(texture->desc.format)
              << " size=" << texture->desc.width << "x" << texture->desc.height
              << " levels=" << texture->desc.levels;
          emitTextureTraceLine(out.str());
        }
        encoder.setFragmentTexture(WMT::Texture{texture->texture.handle}, (uint8_t)stage);
        countTextureBind();
      }
      auto sampler = makeSampler(ctx.device, hot.samplerStates[stage]);
      if (sampler) {
        encoder.setFragmentSamplerState(sampler, (uint8_t)stage);
        countSamplerBind();
      }
    }
  }
  const auto primitiveType = toPrimitiveType(pv.primitiveType);
  bool expandedIndexedDraw = false;
  if (traceEncode) {
    const u32 cullState = core::flatStateOr(
        hot.renderStates, RS_CULL_MODE, static_cast<u32>(core::CullMode::Ccw));
    const bool preTransformed = ffLayout && ffLayout->preTransformed;
    const auto requestedCullMode = (preTransformed || debug::disableCull())
                                       ? WMTCullModeNone
                                       : static_cast<WMTCullMode>(toCullMode(cullState));
    const auto effectiveCullMode = applyDebugCullOverride(requestedCullMode);
    std::ostringstream out;
    out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
        << " draw rt0=" << static_cast<unsigned long long>(hot.colorAttachments[0].handle.value)
        << " ds=" << static_cast<unsigned long long>(hot.depthStencil.handle.value)
        << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
        << " ffLayout=" << (ffLayout ? 1 : 0)
        << " preT=" << (preTransformed ? 1 : 0)
        << " indexed=" << (indexedDraw ? 1 : 0)
        << " primType=" << static_cast<unsigned>(pv.primitiveType)
        << " primCount=" << pv.primitiveCount
        << " vertexCount=" << static_cast<unsigned long long>(vertexCount)
        << " vertexStreamStride=" << uniforms->vertexStreamStride
        << " vertexBufferOffset=" << vertexBufferOffset
        << " vertexStreamOffset=" << uniforms->vertexStreamOffset
        << " vertexBaseIndex=" << uniforms->vertexBaseIndex
        << " colorWrite="
        << core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu)
        << " zEnable=" << core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u)
        << " zWrite=" << core::flatStateOr(hot.renderStates, RS_Z_WRITE_ENABLE, 0u)
        << " zFunc=" << core::flatStateOr(hot.renderStates, RS_Z_FUNC, 0u)
        << " cullState=" << cullState
        << " cullRequested=" << static_cast<unsigned>(requestedCullMode)
        << " cullEffective=" << static_cast<unsigned>(effectiveCullMode)
        << " alphaBlend="
        << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
        << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
        << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
        << " forceVisible=" << (debug::forceVisibleDraw() ? 1 : 0);
    emitQueueTraceLine(out.str());
  }
  if (indexedDraw) {
    const bool forceExpandIndexed = debug::forceExpandIndexed();
    if (forceExpandIndexed) {
      std::span<const u8> indexBytes;
      if (!pv.userIndexData.empty()) {
        indexBytes = pv.userIndexData;
      } else {
        auto* indexRecord = ctx.pool.findBuffer(hot.indexBuffer.value);
        if (indexRecord && !indexRecord->shadow.empty()) {
          indexBytes = indexRecord->shadow;
        } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
          indexBytes = std::span<const u8>(static_cast<const u8*>(indexRecord->contents),
                                           static_cast<std::size_t>(indexRecord->desc.size));
        }
      }
      const std::size_t stride = static_cast<std::size_t>(
          ffLayout ? (uniforms->vertexStreamStride ? uniforms->vertexStreamStride : ffLayout->stride)
                   : computeVertexDeclStride(vertexDecl));
      const std::size_t streamBase = static_cast<std::size_t>(hot.streamOffsets[0]);
      const std::size_t firstIndexByte =
          static_cast<std::size_t>(pv.startIndex) * indexElementSize(pv.indexType);
      std::ostringstream out;
      out << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
          << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
          << " ff=" << (ffLayout ? 1 : 0)
          << " vertexBytes=" << vertexBytes.size()
          << " indexBytes=" << indexBytes.size()
          << " stride=" << stride
          << " startIndex=" << pv.startIndex
          << " baseVertex=" << pv.baseVertexIndex;
      emitQueueTraceLine(out.str());

      if (!vertexBytes.empty() && !indexBytes.empty() && stride != 0) {
        std::vector<u8> expandedVertices(static_cast<std::size_t>(vertexCount) * stride, 0);
        bool expansionComplete = true;
        for (uint64_t i = 0; i < vertexCount; ++i) {
          i32 vertexIndex = pv.baseVertexIndex;
          bool haveIndex = false;
          if (pv.indexType == IndexType::UInt16 &&
              firstIndexByte + static_cast<std::size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
            u16 index = 0;
            std::memcpy(&index, indexBytes.data() + firstIndexByte + static_cast<std::size_t>(i) * sizeof(u16),
                        sizeof(u16));
            vertexIndex += static_cast<i32>(index);
            haveIndex = true;
          } else if (pv.indexType == IndexType::UInt32 &&
                     firstIndexByte + static_cast<std::size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
            u32 index = 0;
            std::memcpy(&index, indexBytes.data() + firstIndexByte + static_cast<std::size_t>(i) * sizeof(u32),
                        sizeof(u32));
            vertexIndex += static_cast<i32>(index);
            haveIndex = true;
          }
          if (!haveIndex || vertexIndex < 0) {
            expansionComplete = false;
            break;
          }
          const std::size_t sourceOffset = streamBase + static_cast<std::size_t>(vertexIndex) * stride;
          if (sourceOffset + stride > vertexBytes.size()) {
            expansionComplete = false;
            break;
          }
          std::memcpy(expandedVertices.data() + static_cast<std::size_t>(i) * stride,
                      vertexBytes.data() + sourceOffset, stride);
        }
        if (expansionComplete) {
          transientVertexBuffer = makeTransientBuffer(expandedVertices.data(), expandedVertices.size());
        }
        if (transientVertexBuffer) {
          encoder.setVertexBuffer(transientVertexBuffer.buffer, transientVertexBuffer.offset, 1);
          countVertexBufferBind();
          if (ffLayout && ffLayout->preTransformed && vertexCount >= 6 && hot.textures[0]) {
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
                    << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
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
          if (!uploadUniforms()) {
            return false;
          }
          expandedIndexedDraw = true;
        }
      }

      std::ostringstream resultTrace;
      resultTrace << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
                  << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
                  << " expanded=" << (expandedIndexedDraw ? 1 : 0);
      emitQueueTraceLine(resultTrace.str());
    }
    if (expandedIndexedDraw) {
      recordDrawGeometryDiagnostics(drawState,
                                    pv,
                                    seqId,
                                    vertexCount,
                                    transientVertexBuffer.offset,
                                    uniforms->vertexStreamOffset,
                                    uniforms->vertexStreamStride,
                                    true,
                                    false,
                                    !pv.userVertexData.empty() || !pv.userIndexData.empty(),
                                    true,
                                    fixedFunctionPath);
      countDrawIssue(drawState,
                     pv.primitiveType,
                     primitiveCount,
                     vertexCount,
                     true,
                     true,
                     pv.userVertexData.size(),
                     pv.userIndexData.size());
      encoder.drawPrimitives(primitiveType, 0, (uint64_t)vertexCount);
      return true;
    }
    CommandQueue::TransientBufferSlice transientIndexBuffer;
    WMT::Buffer indexBuffer{};
    uint64_t indexBufferOffset = static_cast<uint64_t>(pv.startIndex) * indexElementSize(pv.indexType);
    if (!pv.userIndexData.empty()) {
      // Phase 5-B: prefer pre-batched UP index slice from DrawRun
      // bulk upload; fall back to per-draw upload otherwise.
      if (preUploaded && preUploaded->index) {
        transientIndexBuffer = preUploaded->index;
      } else {
        transientIndexBuffer = makeTransientBuffer(pv.userIndexData.data(), pv.userIndexData.size());
      }
      if (transientIndexBuffer) {
        indexBuffer = transientIndexBuffer.buffer;
        indexBufferOffset += transientIndexBuffer.offset;
      }
    } else {
      auto* buffer = ctx.pool.findBuffer(hot.indexBuffer.value);
      if (buffer && buffer->buffer) {
        indexBuffer = WMT::Buffer{buffer->buffer.handle};
      } else if (buffer && !buffer->shadow.empty()) {
        transientIndexBuffer = makeTransientBuffer(buffer->shadow.data(), buffer->shadow.size());
        if (transientIndexBuffer) {
          indexBuffer = transientIndexBuffer.buffer;
          indexBufferOffset += transientIndexBuffer.offset;
        }
      }
    }
    if (indexBuffer) {
      countIndexBufferBind();
      const bool upDraw = !pv.userVertexData.empty() || !pv.userIndexData.empty();
      recordDrawGeometryDiagnostics(drawState,
                                    pv,
                                    seqId,
                                    vertexCount,
                                    vertexBufferOffset,
                                    uniforms->vertexStreamOffset,
                                    uniforms->vertexStreamStride,
                                    true,
                                    !upDraw,
                                    upDraw,
                                    false,
                                    fixedFunctionPath);
      countDrawIssue(drawState,
                     pv.primitiveType,
                     primitiveCount,
                     vertexCount,
                     true,
                     false,
                     pv.userVertexData.size(),
                     pv.userIndexData.size());
      encoder.drawIndexedPrimitives(primitiveType, toIndexType(pv.indexType),
                                    (uint64_t)vertexCount, indexBuffer, indexBufferOffset,
                                    1, 0, 0);
      return true;
    }
  }
  const bool upDraw = !pv.userVertexData.empty();
  recordDrawGeometryDiagnostics(drawState,
                                pv,
                                seqId,
                                vertexCount,
                                vertexBufferOffset,
                                uniforms->vertexStreamOffset,
                                uniforms->vertexStreamStride,
                                false,
                                !upDraw,
                                upDraw,
                                false,
                                fixedFunctionPath);
  countDrawIssue(drawState,
                 pv.primitiveType,
                 primitiveCount,
                 vertexCount,
                 false,
                 false,
                 pv.userVertexData.size(),
                 pv.userIndexData.size());
  encoder.drawPrimitives(primitiveType, 0, (uint64_t)vertexCount);
  return true;
}

std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot) {
  @autoreleasepool {
  PerfScope scope(perf::countEncodeChunkCpuTime);
  if (!ctx.device || !ctx.queue.valid()) {
    return std::nullopt;
  }

  auto commandBuffer = ctx.queue.newCommandBuffer();
  if (!commandBuffer) {
    return std::nullopt;
  }
  bool commandBufferHasWork = false;

  // Deferred-upload fence: flush any pending staging→private blits via
  // the queue-owned ResourceInitializer, then wait for its SharedEvent
  // signal at the head of this chunk's command buffer so textures are
  // fully populated before any draw samples them.
  const auto initializerFlush = ctx.queue.flushInitializerUploads();
  if (initializerFlush.event && initializerFlush.value > 0) {
    commandBuffer.encodeWaitForEvent(initializerFlush.event, initializerFlush.value);
    commandBufferHasWork = true;
  }

  WMT::Reference<WMT::RenderCommandEncoder> activeRenderEncoder{};
  WMT::Reference<WMT::BlitCommandEncoder> activeBlitEncoder{};
  std::vector<std::function<void()>> postCommitCallbacks;
  std::optional<core::metalcapture::MetalCaptureRequest> metalCaptureRequest;
  AttachmentKey activeKey{};
  HazardProbe activeWriteHazard{};
  bool hasActiveRender = false;
  std::optional<core::FlatDrawStateKey> activeDrawStateKey;
  std::optional<core::ClearDesc> pendingClear;

  // TLA+: EncoderLifecycle variable binding:
  // activeKind  := activeRenderEncoder ? "Render" : activeBlitEncoder ? "Blit" : "None"
  // activeRT    := activeKey while activeRenderEncoder is live; NoRT otherwise.
  // hazardFlag  := exact overlap between current attachments and next draw reads, consumed immediately by a split.
  // opCount     := progress through the slot commandHeaders replay loop below.
  // The current blit helpers open and end short-lived encoders internally, so
  // activeBlitEncoder is normally None but remains the local binding for a
  // future chunk-scoped blit encoder.
  auto assertEncoderLifecycleInvariant = [&] {
    DXMT_ASSERT(!(activeRenderEncoder && activeBlitEncoder));
    DXMT_ASSERT(hasActiveRender == static_cast<bool>(activeRenderEncoder));
  };

  auto assertNoActiveEncoder = [&] {
    assertEncoderLifecycleInvariant();
    DXMT_ASSERT(!activeRenderEncoder);
    DXMT_ASSERT(!activeBlitEncoder);
    DXMT_ASSERT(!hasActiveRender);
  };

  auto flushRender = [&](perf::EncoderSplitReason reason = perf::EncoderSplitReason::Final) {
    if (activeRenderEncoder) {
      // TLA+: EncoderLifecycle / EndEncoder(Render)
      DXMT_ASSERT(hasActiveRender);
      DXMT_ASSERT(!activeBlitEncoder);
      activeRenderEncoder.endEncoding();
      perf::countRenderPassEnd(reason);
      activeRenderEncoder = {};
      hasActiveRender = false;
      activeDrawStateKey.reset();
      assertEncoderLifecycleInvariant();
    }
  };

  auto flushBlit = [&] {
    if (activeBlitEncoder) {
      // TLA+: EncoderLifecycle / EndEncoder(Blit)
      DXMT_ASSERT(!activeRenderEncoder);
      DXMT_ASSERT(!hasActiveRender);
      activeBlitEncoder.endEncoding();
      activeBlitEncoder = {};
      assertEncoderLifecycleInvariant();
    }
  };

  auto startRenderPass = [&](core::FlatDrawStateView drawState,
                             const std::optional<core::ClearDesc>& clear) {
    // TLA+: EncoderLifecycle / BeginRender(rt)
    // Callers split through None before opening a new render encoder.
    assertNoActiveEncoder();
    activeRenderEncoder = beginRenderPass(ctx, commandBuffer, drawState, clear);
    hasActiveRender = static_cast<bool>(activeRenderEncoder);
    activeKey = makeAttachmentKey(*drawState.hot);
    activeWriteHazard = makeAttachmentHazard(*drawState.hot);
    activeDrawStateKey.reset();
    assertEncoderLifecycleInvariant();
  };

  auto assertHelperEncoderPrecondition = [&] {
    // TLA+: EncoderLifecycle / BeginBlit
    // Blit-style helpers own any Metal encoder they open and end it before
    // returning; encodeChunk must have ended its active encoder first.
    assertNoActiveEncoder();
  };

  auto flushPendingClear = [&] {
    if (!pendingClear.has_value()) return;
    dxmt9::encoders::encodeClearPass(commandBuffer, ctx.pool, *pendingClear);
    commandBufferHasWork = true;
    pendingClear.reset();
  };

  auto splitBeforeBlockingPresent = [&] {
    if (!splitPresentBeforeAcquireEnabled() || !commandBufferHasWork) {
      return;
    }
    auto presentCommandBuffer = ctx.queue.newCommandBuffer();
    if (!presentCommandBuffer) {
      return;
    }
    const auto commitStarted = std::chrono::steady_clock::now();
    commandBuffer.commit();
    perf::countCommandBufferCommitCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - commitStarted).count()));
    commandBuffer = std::move(presentCommandBuffer);
    commandBufferHasWork = false;
  };

  using Kind = core::MetalCommandKind;
  for (std::size_t commandIndex = 0; commandIndex < slot.commandCount(); ++commandIndex) {
    const auto command = slot.commandAt(commandIndex);
    // TLA+: EncoderLifecycle / opCount observes command replay progress.
    switch (command.kind) {
      case Kind::Clear: {
        if (!command.clear) break;
        const auto& clear = *command.clear;
        flushRender(perf::EncoderSplitReason::ClearBarrier);
        flushBlit();
        flushPendingClear();
        if (clear.rects.empty()) {
          pendingClear = clear;
        } else {
          dxmt9::encoders::encodeClearPass(commandBuffer, ctx.pool, clear);
          commandBufferHasWork = true;
        }
        break;
      }
      case Kind::DrawRun: {
        if (!command.drawState.hot || !command.drawState.shaderLayout ||
            !command.drawUniformPayload ||
            core::drawRunDrawCount(command) == 0) break;
        auto stateView = command.drawState;
        stateView.uniforms = command.drawUniformPayload;
        const auto& hot = *stateView.hot;
        const auto drawParams = command.drawParams;
        // Compact draw-run: state bound from base ONCE (render-pass +
        // resource-binding decisions key off base.rts), then loop over
        // per-DrawParam emits. FlatDrawStateKey is the hot-path decision
        // object for skipping base-state rebinding across compatible
        // Draw/DrawRun records on the same Metal render encoder.
        flushBlit();
        assertEncoderLifecycleInvariant();
        const auto drawKey = makeAttachmentKey(hot);
        const auto drawReadHazard = makeDrawReadHazard(stateView);
        auto hasExactRenderHazard = [&] {
          const bool bloomOverlap = activeWriteHazard.bloomOverlaps(drawReadHazard);
          const bool exactOverlap = activeWriteHazard.exactOverlaps(drawReadHazard);
          perf::countHazardProbe(bloomOverlap, exactOverlap);
          return exactOverlap;
        };
        if (pendingClear.has_value()) {
          const auto clearKey = makeAttachmentKey(*pendingClear);
          const auto clearHazard = makeAttachmentHazard(*pendingClear);
          if (clearKey == drawKey && !clearHazard.exactOverlaps(drawReadHazard)) {
            startRenderPass(stateView, pendingClear);
            pendingClear.reset();
          } else {
            flushPendingClear();
            const bool renderTargetChanged = hasActiveRender && activeKey != drawKey;
            const bool hazardDetected =
                hasActiveRender && !renderTargetChanged && hasExactRenderHazard();
            if (!hasActiveRender || renderTargetChanged || hazardDetected) {
              if (renderTargetChanged) {
                // TLA+: EncoderLifecycle / RenderTargetChange(newRT)
                DXMT_ASSERT(hasActiveRender);
              }
              if (hazardDetected) {
                // TLA+: EncoderLifecycle / HazardDetected
                DXMT_ASSERT(hasActiveRender);
                DXMT_ASSERT(activeKey == drawKey);
              }
              const auto splitReason = renderTargetChanged
                  ? perf::EncoderSplitReason::RenderTargetChange
                  : (hazardDetected ? perf::EncoderSplitReason::Hazard
                                    : perf::EncoderSplitReason::ClearBarrier);
              flushRender(splitReason);
              startRenderPass(stateView, std::nullopt);
            } else {
              // TLA+: EncoderLifecycle / MergeRenderDraw(rt)
              DXMT_ASSERT(hasActiveRender);
              DXMT_ASSERT(activeKey == drawKey);
              DXMT_ASSERT(!activeWriteHazard.exactOverlaps(drawReadHazard));
            }
          }
        } else {
          const bool renderTargetChanged = hasActiveRender && activeKey != drawKey;
          const bool hazardDetected =
              hasActiveRender && !renderTargetChanged && hasExactRenderHazard();
          if (!hasActiveRender || renderTargetChanged || hazardDetected) {
            if (renderTargetChanged) {
              // TLA+: EncoderLifecycle / RenderTargetChange(newRT)
              DXMT_ASSERT(hasActiveRender);
            }
            if (hazardDetected) {
              // TLA+: EncoderLifecycle / HazardDetected
              DXMT_ASSERT(hasActiveRender);
              DXMT_ASSERT(activeKey == drawKey);
            }
            const auto splitReason = renderTargetChanged
                ? perf::EncoderSplitReason::RenderTargetChange
                : (hazardDetected ? perf::EncoderSplitReason::Hazard
                                  : perf::EncoderSplitReason::Final);
            flushRender(splitReason);
            startRenderPass(stateView, std::nullopt);
          } else {
            // TLA+: EncoderLifecycle / MergeRenderDraw(rt)
            DXMT_ASSERT(hasActiveRender);
            DXMT_ASSERT(activeKey == drawKey);
            DXMT_ASSERT(!activeWriteHazard.exactOverlaps(drawReadHazard));
          }
        }
        // Phase 3-E: bind BaseDrawState ONCE on iter 0, then issue-only
        // path on iters 1..N — the Metal render encoder retains
        // pipeline / depth / viewport / scissor / cull / texture /
        // sampler state across draw calls.
        //
        // Phase 5-B: pre-scan for UP vertex/index payloads + batch-
        // upload them all in ONE uploadTransientBufferBatch call
        // (single transientBufferMutex_ acquire, single completedSeqId
        // snapshot, single reclaim pass for the whole run). Per-draw
        // pre-resolved slices are handed to encodeDraw via
        // PreUploadedDrawData.
        //
        // Layout of the batch payload vector (interleaved per draw):
        //   [0]   = draw 0 vertex (empty if no UP)
        //   [1]   = draw 0 index  (empty if no UP)
        //   [2]   = draw 1 vertex
        //   [3]   = draw 1 index
        //   …
        // Returned slices use the same indexing.
        const std::size_t drawCount = core::drawRunDrawCount(command);
        const auto recordPayloadArena = core::drawRunPayloadBytes(command);
        bool anyUpData = false;
        bool hasUpPayloadRanges = false;
        for (const auto& param : drawParams) {
          if (!param.userVertexRange.empty() || !param.userIndexRange.empty()) {
            hasUpPayloadRanges = true;
            break;
          }
        }
        std::vector<CommandQueue::TransientBufferSlice> upSlices;
        if (hasUpPayloadRanges) {
          std::vector<std::span<const std::byte>> upPayloads;
          upPayloads.reserve(drawCount * 2);
          for (const auto& param : drawParams) {
            const auto vertexBytes = drawParamVertexBytes(param, recordPayloadArena);
            if (!vertexBytes.empty()) anyUpData = true;
            upPayloads.emplace_back(reinterpret_cast<const std::byte*>(vertexBytes.data()),
                                    vertexBytes.size());
            const auto indexBytes = drawParamIndexBytes(param, recordPayloadArena);
            if (!indexBytes.empty()) anyUpData = true;
            upPayloads.emplace_back(reinterpret_cast<const std::byte*>(indexBytes.data()),
                                    indexBytes.size());
          }
          if (anyUpData) {
            upSlices = ctx.queue.uploadTransientBufferBatch(upPayloads, /*alignment=*/16, slot.seqId);
          }
        }

        // encodeDraw receives the per-draw fields through DrawParam while
        // all base state is read from the canonical hot/shader view.
        bool baseBound =
            activeDrawStateKey.has_value() && *activeDrawStateKey == hot.key;
        for (std::size_t i = 0; i < drawCount; ++i) {
          const auto& param = drawParams[i];
          PreUploadedDrawData preData{};
          if (i * 2u + 1u < upSlices.size()) {
            preData.vertex = upSlices[i * 2u];
            preData.index = upSlices[i * 2u + 1u];
          }
          if (encodeDraw(ctx, commandBuffer, activeRenderEncoder, stateView, slot.seqId,
                         /*skipBaseStateBind=*/baseBound,
                         anyUpData ? &preData : nullptr,
                         &param,
                         recordPayloadArena)) {
            baseBound = true;
            activeDrawStateKey = hot.key;
          }
        }
        commandBufferHasWork = true;
        break;
      }
      case Kind::SurfaceCopy: {
        if (!command.surfaceCopy) break;
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::SurfaceCopy);
        assertHelperEncoderPrecondition();
        dxmt9::encoders::encodeSurfaceCopy(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                           ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                           *command.surfaceCopy);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::StretchRect: {
        if (!command.stretchRect) break;
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::StretchRect);
        assertHelperEncoderPrecondition();
        dxmt9::encoders::encodeStretchRect(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                            ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                            *command.stretchRect);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::Readback: {
        if (!command.readback) break;
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::Readback);
        assertHelperEncoderPrecondition();
        dxmt9::encoders::encodeReadback(commandBuffer, ctx.pool, *command.readback);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::ColorFill: {
        if (!command.colorFill) break;
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::ColorFill);
        // TLA+: EncoderLifecycle / BeginRender(rt)
        // ColorFill owns a short-lived helper render encoder and ends it before returning.
        assertNoActiveEncoder();
        dxmt9::encoders::encodeColorFill(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                          ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                          *command.colorFill);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::Present: {
        if (!command.present) break;
        const auto& present = command.present->present;
        const auto presentSource = command.present->presentSource;
        if (!metalCaptureRequest.has_value()) {
          metalCaptureRequest = ctx.queue.metalCaptureForPresentChunk(slot.seqId);
        }
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::Present);
        flushBlit();
        splitBeforeBlockingPresent();
        const bool noteAfterAcquire = presentBoundaryAfterAcquireEnabled();
        if (!noteAfterAcquire) {
          ctx.queue.notePresentDequeued(slot.seqId);
        }
        const bool presentEncoded = dxmt9::encodePresent(commandBuffer, ctx.pool,
                                                          present, presentSource, slot.seqId);
        if (noteAfterAcquire) {
          ctx.queue.notePresentDequeued(slot.seqId);
        }
        if (presentEncoded) {
          commandBufferHasWork = true;
          ctx.queue.backBufferDiscardAfterPresent_ = true;
          if (auto* presenter = present.presenter) {
            postCommitCallbacks.push_back([presenter, seqId = slot.seqId] {
              presenter->preAcquireNextDrawable(seqId);
            });
          }
        }
        break;
      }
    }
  }

  flushPendingClear();
  flushRender(perf::EncoderSplitReason::Final);
  flushBlit();

  const u64 seqId = slot.seqId;
  core::metalqueue::QueueSubmissionRecord record;
  record.commandBuffer = std::move(commandBuffer);
  if (metalCaptureRequest.has_value()) {
    record.metalCaptureDevice = WMT::Device{ctx.device.handle};
    record.metalCapture = std::move(metalCaptureRequest);
  }
  record.slotIndex = slotIndex;
  record.seqId = seqId;
  record.context = "queue";
  record.postCommitCallbacks = std::move(postCommitCallbacks);
  return record;
  }  // @autoreleasepool
}

}  // namespace dxmt9::encoders
