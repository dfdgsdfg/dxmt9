#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9_capture.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_encode_partition.hpp"
#include "dxmt9_draw_encoder_internal.hpp"
#include "dxmt9_draw_encoder_diagnostics.hpp"
#include "dxmt9_encode_session_storage_internal.hpp"
#include "dxmt9_render_pass_close_ledger.hpp"
#include "dxmt9_render_pass_internal.hpp"
#include "dxmt9_encode_session_internal.hpp"
#include "dxmt9_argbuf_hybrid.hpp"
#include "dxmt9_blit_encoders.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_command_queue.hpp"
#include "dxmt9_debug_alloc_guard.hpp"
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
#include "dxmt9_signposts.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
using core::SAMP_MAX_MIP_LEVEL;
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
using core::RS_BLEND_OP;
using core::RS_BLEND_OP_ALPHA;
using core::RS_BLEND_FACTOR;
using core::RS_COLOR_WRITE_ENABLE;
using core::RS_CULL_MODE;
using core::RS_DEST_BLEND;
using core::RS_DEST_BLEND_ALPHA;
using core::RS_POINT_SCALE_ENABLE;
using core::RS_POINT_SPRITE_ENABLE;
using core::RS_POINTSIZE;
using core::RS_POINTSIZE_MAX;
using core::RS_POINTSIZE_MIN;
using core::RS_SEPARATE_ALPHA_BLEND_ENABLE;
using core::RS_SRC_BLEND;
using core::RS_SRC_BLEND_ALPHA;
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
using dxmt9::ffp::kD3DDeclTypeFloat1;
using dxmt9::ffp::kD3DDeclTypeFloat2;
using dxmt9::ffp::kD3DDeclTypeFloat3;
using dxmt9::ffp::kD3DDeclTypeFloat4;
using dxmt9::ffp::kD3DDeclUsageColor;
using dxmt9::ffp::kD3DDeclUsagePosition;
using dxmt9::ffp::kD3DDeclUsagePositionT;
using dxmt9::ffp::kD3DDeclUsageTexcoord;

using dxmt9::convert::formatHasDepthAspect;
using dxmt9::convert::formatHasStencilAspect;
using dxmt9::convert::toPixelFormat;
using dxmt9::convert::toCullMode;
using dxmt9::convert::toIndexType;
using dxmt9::convert::toPrimitiveType;
using dxmt9::ffp::computeVertexDeclStreamStride;
using dxmt9::ffp::computeVertexDeclStride;
using dxmt9::ffp::decodeFixedFunctionVertexLayout;

using dxmt9::core::metalqueue::emitQueueTraceLine;
using dxmt9::core::metalqueue::emitTextureTraceLine;
using dxmt9::core::metalqueue::queueTraceEnabled;

using dxmt9::state::DrawVolatile;
using dxmt9::state::FfpPsConsts;
using dxmt9::state::FfpVsConsts;
using dxmt9::state::PsConsts;
using dxmt9::state::SamplerLodBias;
using dxmt9::state::VsConsts;
using dxmt9::state::anySamplerLodBiasNonzero;
using dxmt9::state::buildDrawVolatile;
using dxmt9::state::buildFfpPsConsts;
using dxmt9::state::buildSamplerLodBias;
using dxmt9::state::buildFfpVsConsts;
using dxmt9::state::buildPsConsts;
using dxmt9::state::buildVsConsts;
using dxmt9::state::makeDepthStencilKey;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using f32 = float;

namespace {

using encode_session::ArgbufCbufCache;
using encode_session::ArgbufPayloadDeltaComponentKey;
using encode_session::ArgbufPayloadDeltaKey;
using encode_session::AttachmentKey;
using encode_session::StreamIbStagingCache;

[[noreturn]] void abortEncodePartitionInvariant(const char* reason) {
  std::fprintf(
      stderr,
      "[dxmt9-encode] fatal: encode partition execution invariant failed "
      "(%s)\n",
      reason ? reason : "unknown");
  std::abort();
}

thread_local DrawBindingPacketCache gDrawBindingPacketCache;

u64 textureSamplerShadowHash(u64 tag,
                             std::uint8_t stage,
                             obj_handle_t handle) noexcept {
  u64 seed = drawBindingPacketHashMix(tag, stage);
  return drawBindingPacketHashMix(seed, static_cast<u64>(handle));
}

bool textureSamplerShadowMatches(const TextureSamplerBindShadowSlot& slot,
                                 u64 hash,
                                 obj_handle_t handle) noexcept {
  return slot.valid && slot.hash == hash && slot.handle == handle;
}

void textureSamplerShadowStore(TextureSamplerBindShadowSlot& slot,
                               u64 hash,
                               obj_handle_t handle) noexcept {
  slot.valid = true;
  slot.hash = hash;
  slot.handle = handle;
}

u64 samplerBindShadowHash(u64 tag,
                          std::uint8_t stage,
                          u64 samplerStateHash,
                          u32 textureLod,
                          bool supportArgumentBuffers) noexcept {
  u64 seed = drawBindingPacketHashMix(tag, stage);
  seed = drawBindingPacketHashMix(seed, textureLod);
  seed = drawBindingPacketHashMix(seed, supportArgumentBuffers ? 1ull : 0ull);
  return drawBindingPacketHashMix(seed, samplerStateHash);
}

bool samplerBindShadowMatches(const SamplerBindShadowSlot& slot,
                              u64 hash,
                              const core::FlatStateSet<core::kMaxSamplerStates>& states,
                              u32 textureLod,
                              bool supportArgumentBuffers) noexcept {
  return slot.valid &&
         slot.hash == hash &&
         slot.textureLod == textureLod &&
         slot.supportArgumentBuffers == supportArgumentBuffers &&
         drawBindingPacketFlatStateSetsEqual(slot.samplerStates, states);
}

void samplerBindShadowStore(SamplerBindShadowSlot& slot,
                            u64 hash,
                            const core::FlatStateSet<core::kMaxSamplerStates>& states,
                            u32 textureLod,
                            bool supportArgumentBuffers,
                            obj_handle_t handle) noexcept {
  slot.valid = true;
  slot.hash = hash;
  slot.handle = handle;
  slot.textureLod = textureLod;
  slot.supportArgumentBuffers = supportArgumentBuffers;
  slot.samplerStates = states;
}

bool bindShadowMatches(const TextureSamplerBindShadowSlot& slot,
                       obj_handle_t handle) noexcept {
  return slot.valid && slot.handle == handle;
}

void bindShadowStore(TextureSamplerBindShadowSlot& slot,
                     obj_handle_t handle) noexcept {
  slot.valid = true;
  slot.hash = 0;
  slot.handle = handle;
}

bool bufferBindShadowMatches(const BufferBindShadowSlot& slot,
                             obj_handle_t handle,
                             u64 offset) noexcept {
  return slot.valid && slot.handle == handle && slot.offset == offset;
}

void bufferBindShadowStore(BufferBindShadowSlot& slot,
                           obj_handle_t handle,
                           u64 offset) noexcept {
  slot.valid = true;
  slot.handle = handle;
  slot.offset = offset;
}

bool streamIbStagingActive(const StreamIbStagingCache* cache) noexcept {
  return cache && cache->enabled;
}

struct ArgbufCbufIdentityProbe {
  u64 bytes = 0;
  u64 hash = 0;
};

u64 makeArgbufCbufIdentityHash(u64 tag, u64 sourceHash, u64 bytes) noexcept {
  u64 hash = drawBindingPacketHashMix(tag, sourceHash);
  hash = drawBindingPacketHashMix(hash, bytes);
  return hash;
}

u64 drawStateVertexCbufSourceHash(core::FlatDrawStateView drawState) noexcept {
  if (drawState.hasUniformPayload() &&
      drawState.uniformPayload().vertexConstantsHash != 0) {
    return drawState.uniformPayload().vertexConstantsHash;
  }
  return drawState.hot ? drawState.hot->vertexConstantsHash : 0;
}

u64 drawStatePixelCbufSourceHash(core::FlatDrawStateView drawState) noexcept {
  if (drawState.hasUniformPayload() &&
      drawState.uniformPayload().pixelConstantsHash != 0) {
    return drawState.uniformPayload().pixelConstantsHash;
  }
  return drawState.hot ? drawState.hot->pixelConstantsHash : 0;
}

u64 makeArgbufFfpPsIdentityHash(core::FlatDrawStateView drawState,
                                u64 bytes) noexcept {
  if (!drawState.hot) return 0;
  u64 hash = drawBindingPacketHashMix(
      0x6666705f70735f63ull, drawState.hot->key.renderStateHash);
  for (const auto stageHash : drawState.hot->key.textureStageStateHashes) {
    hash = drawBindingPacketHashMix(hash, stageHash);
  }
  hash = drawBindingPacketHashMix(hash, bytes);
  return hash;
}

void stampArgbufCbufBindingIdentities(
    dxmt9::argbuf_hybrid::ConstantBufferBindings& bindings,
    core::FlatDrawStateView drawState) noexcept {
  if (!drawState.hot) return;
  auto stamp = [&](u32 argbufIndex, u64 identityHash) {
    if (argbufIndex < bindings.entries.size() && bindings.entries[argbufIndex]) {
      bindings.entries[argbufIndex].identityHash = identityHash;
    }
  };
  const auto& entries = bindings.entries;
  if (dxmt9::argbuf_hybrid::kConstantBufferVsIndex < entries.size()) {
    const auto& binding =
        entries[dxmt9::argbuf_hybrid::kConstantBufferVsIndex];
    stamp(dxmt9::argbuf_hybrid::kConstantBufferVsIndex,
          makeArgbufCbufIdentityHash(
              0x76735f636275665full,
              drawStateVertexCbufSourceHash(drawState), binding.bytes));
  }
  if (dxmt9::argbuf_hybrid::kConstantBufferPsIndex < entries.size()) {
    const auto& binding =
        entries[dxmt9::argbuf_hybrid::kConstantBufferPsIndex];
    stamp(dxmt9::argbuf_hybrid::kConstantBufferPsIndex,
          makeArgbufCbufIdentityHash(
              0x70735f636275665full,
              drawStatePixelCbufSourceHash(drawState), binding.bytes));
  }
  if (dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex < entries.size()) {
    const auto& binding =
        entries[dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex];
    stamp(dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex,
          makeArgbufFfpPsIdentityHash(drawState, binding.bytes));
  }
}

bool blendFactorNeedsConstantColor(const core::FlatRenderStateSet& rs) {
  const auto isConstantBlend = [](u32 factor) {
    return factor == static_cast<u32>(core::BlendFactor::BlendFactor) ||
           factor == static_cast<u32>(core::BlendFactor::InvBlendFactor);
  };
  if (core::flatStateOr(rs, RS_ALPHABLEND_ENABLE, 0u) == 0u) {
    return false;
  }
  if (isConstantBlend(core::flatStateOr(rs, RS_SRC_BLEND,
                                        static_cast<u32>(core::BlendFactor::One))) ||
      isConstantBlend(core::flatStateOr(rs, RS_DEST_BLEND,
                                        static_cast<u32>(core::BlendFactor::Zero)))) {
    return true;
  }
  if (core::flatStateOr(rs, RS_SEPARATE_ALPHA_BLEND_ENABLE, 0u) == 0u) {
    return false;
  }
  return isConstantBlend(core::flatStateOr(rs, RS_SRC_BLEND_ALPHA,
                                           static_cast<u32>(core::BlendFactor::One))) ||
         isConstantBlend(core::flatStateOr(rs, RS_DEST_BLEND_ALPHA,
                                           static_cast<u32>(core::BlendFactor::Zero)));
}

std::array<float, 4> decodeD3DBlendFactor(u32 argb) {
  constexpr float scale = 1.0f / 255.0f;
  return {
      static_cast<float>((argb >> 16) & 0xffu) * scale,
      static_cast<float>((argb >> 8) & 0xffu) * scale,
      static_cast<float>(argb & 0xffu) * scale,
      static_cast<float>((argb >> 24) & 0xffu) * scale,
  };
}

constexpr u64 kFragmentTextureShadowTag = 0x667261675f746578ull;
constexpr u64 kFragmentSamplerShadowTag = 0x667261675f73616dull;
constexpr u64 kVertexTextureShadowTag = 0x766572745f746578ull;
constexpr u64 kVertexSamplerShadowTag = 0x766572745f73616dull;

bool presentBoundaryAfterAcquireEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

// R-BACK-2.29..2.32 — env-driven mid-chunk commit policy. Read once at
// process start; subsequent runs need a re-launch to change it. This is
// the cleanest invariant under R-BACK-2.31 (deterministic split
// decisions, no wallclock or GPU-feedback inputs).
enum class MidChunkCommitPolicy : std::uint8_t {
  Off,
  PerRenderPass,
  PerNRecords,
};

MidChunkCommitPolicy midChunkCommitPolicy() {
  static const MidChunkCommitPolicy policy = [] {
    // R-BACK-2.34 — production default flipped from Off to PerRenderPass
    // 2026-05-10. The X1 chain-probe measurement showed wall-time -5%,
    // encode CPU -63%, present_acquire_wait -20% under the cap=4 from
    // R-BACK-2.33 (`docs/boundary-baseline-measurements.md`). SFIV
    // heavy-scene (U1) was neutral on fps but -44% on
    // `gpu_command_buffer_time_ms` p99. The cap from R-BACK-2.33
    // bounds tile-flush + commit overhead at ~2.1 ms / frame on the
    // SFIV envelope per `docs/research/g-axis-tuning.md`.
    // `DXMT9_MID_CHUNK_COMMIT_POLICY=off` remains a one-line opt-out.
    const char* env = std::getenv("DXMT9_MID_CHUNK_COMMIT_POLICY");
    if (!env || env[0] == '\0') return MidChunkCommitPolicy::PerRenderPass;
    if (std::strcmp(env, "per-render-pass") == 0) {
      return MidChunkCommitPolicy::PerRenderPass;
    }
    if (std::strcmp(env, "per-n-records") == 0) {
      return MidChunkCommitPolicy::PerNRecords;
    }
    if (std::strcmp(env, "off") == 0) {
      return MidChunkCommitPolicy::Off;
    }
    // Unrecognized token → fall back to the production default rather
    // than silently turning the policy off. R-BACK-2.31 determinism
    // is preserved because the env is read-once and the table of
    // accepted tokens is closed.
    return MidChunkCommitPolicy::PerRenderPass;
  }();
  return policy;
}

std::uint32_t midChunkCommitNRecords() {
  static const std::uint32_t n = [] {
    const char* env = std::getenv("DXMT9_MID_CHUNK_COMMIT_RECORDS");
    if (!env || env[0] == '\0') return 64u;
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (parsed <= 0 || end == env) return 64u;
    return static_cast<std::uint32_t>(parsed);
  }();
  return n;
}

// R-BACK-2.33 — per-chunk sub-CB chain length cap. The encode thread
// stops splitting once a chunk has produced this many sub-CBs (counting
// the chain tail toward the cap), so a 27-render-pass chunk does not
// turn into a 27-CB chain whose tile-flush + commit overhead overwhelms
// the pipelining win. 4 was chosen by `docs/research/g-axis-tuning.md`
// against an estimated TBDR cost model; it is configurable so empirical
// re-measurement can move the default. Read once at process start.
std::uint32_t midChunkCommitCapPerRenderPass() {
  static const std::uint32_t cap = [] {
    const char* env = std::getenv("DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS");
    if (!env || env[0] == '\0') return 4u;
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    // 0 disables the cap (unbounded chain). Negative or unparseable
    // tokens fall back to the default to preserve R-BACK-2.31 determinism.
    if (parsed < 0 || end == env) return 4u;
    return static_cast<std::uint32_t>(parsed);
  }();
  return cap;
}


WMTCullMode applyDebugCullOverride(WMTCullMode cullMode) {
  // Read-once contract (environment_variables.rules.md): this runs twice per
  // draw, and an uncached getenv here sampled at >1% of the encode thread on
  // draw-heavy scenes (GT2, ~1.7k draws/frame).
  static const std::optional<WMTCullMode> override = []() -> std::optional<WMTCullMode> {
    const char* env = std::getenv("DXMT_DEBUG_FORCE_CULL_MODE");
    if (!env || env[0] == '\0') {
      return std::nullopt;
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
    return std::nullopt;
  }();
  return override.value_or(cullMode);
}

WMTTriangleFillMode triangleFillModeFromRenderState(
    const core::FlatRenderStateSet& renderStates) {
  constexpr u32 kD3DFillWireframe = 2u;
  return core::flatStateOr(renderStates, core::RS_FILL_MODE, 3u) == kD3DFillWireframe
             ? WMTTriangleFillModeLines
             : WMTTriangleFillModeFill;
}

WMTDepthClipMode depthClipModeFromRenderState(
    const core::FlatRenderStateSet& renderStates) {
  return core::flatStateOr(renderStates, core::RS_CLIPPING, 1u) != 0u
             ? WMTDepthClipModeClip
             : WMTDepthClipModeClamp;
}

void setRasterizerCullMode(EncodeContext& ctx,
                           WMT::RenderCommandEncoder& encoder,
                           const core::FlatRenderStateSet& renderStates,
                           WMTCullMode cullMode) {
  cullMode = applyDebugCullOverride(cullMode);
  // D3D9 RS_DEPTH_BIAS / RS_SLOPE_SCALE_DEPTH_BIAS are stored as DWORDs but
  // semantically float; bit_cast restores the IEEE 754 layout that
  // MTLRenderCommandEncoder.setDepthBias:slopeScale:clamp: expects. clamp is
  // not exposed by D3D9 RS and is left at 0.0f (Metal's "unbounded" sentinel).
  const float depthBias = std::bit_cast<float>(
      core::flatStateOr(renderStates, core::RS_DEPTH_BIAS, 0u));
  const float slopeScale = std::bit_cast<float>(
      core::flatStateOr(renderStates, core::RS_SLOPE_SCALE_DEPTH_BIAS, 0u));
  recordedSetRasterizerState(ctx, encoder, triangleFillModeFromRenderState(renderStates), cullMode,
                             depthClipModeFromRenderState(renderStates), frontFaceWinding(),
                             depthBias, slopeScale, 0.0f);
  countRasterizerBind();
}


}  // namespace

namespace {

using encode_session::EncodeChunkSessionStorage;


struct RenderPassFrameKey {
  u64 color0 = 0;
  u64 depth = 0;
  u32 sampleCount = 1;

  bool valid() const noexcept {
    return color0 != 0 || depth != 0;
  }

  friend bool operator==(const RenderPassFrameKey&, const RenderPassFrameKey&) = default;
};

RenderPassFrameKey makeRenderPassFrameKey(const core::FlatDrawStateRecord& hot) {
  return RenderPassFrameKey{
      .color0 = hot.colorAttachments[0].handle.value,
      .depth = hot.depthStencil.handle.value,
      .sampleCount = std::max(hot.colorAttachments[0].sampleCount,
                              hot.depthStencil.sampleCount),
  };
}

std::size_t renderPassReentryTopLimit() {
  static const std::size_t value = []() -> std::size_t {
    const char* env = std::getenv("DXMT9_PERF_RENDER_PASS_REENTRY_TOP");
    if (!env || env[0] == '\0' || env[0] == '0') {
      return 0;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    if (end == env || parsed == 1u) {
      return 8;
    }
    return static_cast<std::size_t>(std::min<unsigned long long>(parsed, 32ull));
  }();
  return perf::enabled() ? value : 0;
}

struct RenderPassAttachmentFootprint {
  std::uint64_t color0Bytes = 0;
  std::uint64_t depthBytes = 0;

  std::uint64_t totalBytes() const noexcept {
    return color0Bytes + depthBytes;
  }
};

RenderPassAttachmentFootprint estimateRenderPassAttachmentFootprintBytes(
    EncodeContext& ctx,
    const core::FlatDrawStateRecord& hot) {
  RenderPassAttachmentFootprint footprint{};
  auto addSurface = [&](core::Handle handle) {
    if (!handle) {
      return std::uint64_t{0};
    }
    const auto* surface = ctx.pool.findSurface(handle.value);
    if (!surface || !surface->texture) {
      return std::uint64_t{0};
    }
    return static_cast<std::uint64_t>(surface->desc.width) *
           static_cast<std::uint64_t>(surface->desc.height) *
           static_cast<std::uint64_t>(core::bytesPerPixel(surface->desc.format));
  };
  footprint.color0Bytes = addSurface(hot.colorAttachments[0].handle);
  footprint.depthBytes = addSurface(hot.depthStencil.handle);
  return footprint;
}


struct RenderPassReentryTopEntry {
  RenderPassFrameKey a{};
  RenderPassFrameKey b{};
  std::uint64_t count = 0;
  std::uint64_t preservationBytes = 0;
  std::uint64_t priorASeq = 0;
  std::uint64_t priorAEncoder = 0;
  std::uint32_t priorAPass = 0;
  std::uint64_t firstSeq = 0;
  std::uint64_t firstEncoder = 0;
  std::uint32_t firstPass = 0;
  std::uint64_t firstBSeq = 0;
  std::uint64_t firstBEncoder = 0;
  std::uint32_t firstBPass = 0;
  std::uint64_t lastSeq = 0;
  std::uint64_t lastEncoder = 0;
  std::uint32_t lastPass = 0;
  std::uint64_t lastBSeq = 0;
  std::uint64_t lastBEncoder = 0;
  std::uint32_t lastBPass = 0;
  bool bReadsAColor = false;
  bool bReadsADepth = false;
  bool aReadsBColor = false;
  bool aReadsBDepth = false;
  perf::RenderPassColorStoreProof aColorProof =
      perf::RenderPassColorStoreProof::BlockNullColor;
  perf::RenderPassDepthStoreProof aDepthProof =
      perf::RenderPassDepthStoreProof::BlockNullDepth;
  perf::RenderPassColorStoreProof bColorProof =
      perf::RenderPassColorStoreProof::BlockNullColor;
  perf::RenderPassDepthStoreProof bDepthProof =
      perf::RenderPassDepthStoreProof::BlockNullDepth;
  std::uint32_t aColorTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t aDepthTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t bColorTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t bDepthTouchDistance =
      std::numeric_limits<std::uint32_t>::max();

  bool used() const noexcept {
    return count != 0;
  }
};

struct RenderPassReadRelation {
  bool color = false;
  bool depth = false;
};

RenderPassReadRelation readRelationToKey(const core::FlatDrawStateRecord& hot,
                                         RenderPassFrameKey key) noexcept {
  RenderPassReadRelation relation{};
  if (!key.valid()) {
    return relation;
  }
  const auto& textures = hot.textures;
  const std::uint32_t mask = hot.textureMask;
  for (std::size_t i = 0; i < textures.size(); ++i) {
    if ((mask & (1u << i)) == 0) {
      continue;
    }
    const u64 handle = textures[i].value;
    relation.color = relation.color || (key.color0 != 0 && handle == key.color0);
    relation.depth = relation.depth || (key.depth != 0 && handle == key.depth);
  }
  return relation;
}

struct PendingRenderPassReentryTop {
  RenderPassFrameKey a{};
  RenderPassFrameKey b{};
  std::uint64_t preservationBytes = 0;
  std::uint64_t priorASeq = 0;
  std::uint64_t priorAEncoder = 0;
  std::uint32_t priorAPass = 0;
  std::uint64_t seq = 0;
  std::uint64_t encoder = 0;
  std::uint32_t pass = 0;
  std::uint64_t bSeq = 0;
  std::uint64_t bEncoder = 0;
  std::uint32_t bPass = 0;
  bool bReadsAColor = false;
  bool bReadsADepth = false;
  RenderPassStoreProofSummary aProof{};
  RenderPassStoreProofSummary bProof{};
};

struct RenderPassFrameTracker {
  std::array<RenderPassFrameKey, 64> seen{};
  std::array<std::uint32_t, 64> lastSeenPassIndex{};
  std::array<std::uint64_t, 64> lastSeenSeq{};
  std::array<std::uint64_t, 64> lastSeenEncoder{};
  std::array<std::uint32_t, 64> lastSeenPass{};
  std::array<ReplayWindowProvenance, 64> lastSeenReplayWindow{};
  std::array<ReplayWindowProvenance, 4> recentReplayWindows{};
  std::array<std::uint64_t, 4> recentSeqIds{};
  std::array<RenderPassReentryTopEntry, 32> reentryTop{};
  std::size_t seenCount = 0;
  std::uint32_t passIndex = 0;
  std::uint64_t frameIndex = 0;
  std::optional<RenderPassFrameKey> last{};
  std::uint64_t lastSeq = 0;
  std::uint64_t lastEncoder = 0;
  std::uint32_t lastPass = 0;
  RenderPassStoreProofSummary lastProof{};
  bool lastOpenedWithClear = false;
  std::optional<RenderPassFrameKey> previousKeyForCurrent{};
  bool currentReadsPreviousColor = false;
  bool currentReadsPreviousDepth = false;
  std::optional<PendingRenderPassReentryTop> pendingReentry{};
  RenderPassCloseLedger<> closeLedger{};

  void noteClose(RenderPassCloseRecord record) noexcept {
    if (!perf::enabled()) {
      return;
    }
    if (record.splitReason == perf::EncoderSplitReason::Final) {
      perf::countRenderPassFinalCloseCause(record.finalizeCause);
    }
    const bool recorded = closeLedger.noteClose(record);
    if (recorded) {
      perf::countRenderPassCloseLedgerRecorded();
    } else {
      perf::countRenderPassCloseLedgerMissing();
    }
    if (record.splitReason == perf::EncoderSplitReason::Final) {
      if (recorded) {
        perf::countRenderPassFinalCloseLedgerRecorded();
      } else {
        perf::countRenderPassFinalCloseLedgerMissing();
      }
    }
  }

  void finishCloseLedgerAtPresent() noexcept {
    if (!perf::enabled()) {
      return;
    }
    const auto terminal = closeLedger.finishFrame();
    perf::countRenderPassCloseLedgerTerminalNotReopenedBeforePresent(
        terminal.notReopenedBeforePresent);
    perf::countRenderPassFinalCloseLedgerTerminalNotReopenedBeforePresent(
        terminal.finalNotReopenedBeforePresent);
  }

  void reset() {
    finishCloseLedgerAtPresent();
    finalizePendingReentry(currentReadsPreviousColor, currentReadsPreviousDepth);
    emitReentryTop();
    seenCount = 0;
    lastSeenPassIndex.fill(0);
    lastSeenSeq.fill(0);
    lastSeenEncoder.fill(0);
    lastSeenPass.fill(0);
    if (perf::enabled()) {
      lastSeenReplayWindow = {};
      recentReplayWindows = {};
      recentSeqIds = {};
    }
    reentryTop = {};
    passIndex = 0;
    ++frameIndex;
    last.reset();
    lastSeq = 0;
    lastEncoder = 0;
    lastPass = 0;
    lastProof = {};
    lastOpenedWithClear = false;
    previousKeyForCurrent.reset();
    currentReadsPreviousColor = false;
    currentReadsPreviousDepth = false;
    pendingReentry.reset();
  }

  std::optional<std::size_t> findSeenIndex(RenderPassFrameKey key) const noexcept {
    for (std::size_t i = 0; i < seenCount; ++i) {
      if (seen[i] == key) {
        return i;
      }
    }
    return std::nullopt;
  }

  void remember(RenderPassFrameKey key,
                std::uint32_t currentPassIndex,
                std::uint64_t seq,
                std::uint64_t encoder,
                ReplayWindowProvenance replayWindow,
                bool replayWindowAttributionEnabled) noexcept {
    if (seenCount < seen.size()) {
      seen[seenCount] = key;
      lastSeenPassIndex[seenCount] = currentPassIndex;
      lastSeenSeq[seenCount] = seq;
      lastSeenEncoder[seenCount] = encoder;
      lastSeenPass[seenCount] = currentPassIndex;
      if (replayWindowAttributionEnabled) {
        lastSeenReplayWindow[seenCount] = replayWindow;
      }
      ++seenCount;
    }
  }

  void recordReentryTop(RenderPassFrameKey a,
                        RenderPassFrameKey b,
                        std::uint64_t preservationBytes,
                        std::uint64_t priorASeq,
                        std::uint64_t priorAEncoder,
                        std::uint32_t priorAPass,
                        std::uint64_t seq,
                        std::uint64_t encoder,
                        std::uint64_t bSeq,
                        std::uint64_t bEncoder,
                        std::uint32_t bPass,
                        bool bReadsAColor,
                        bool bReadsADepth,
                        bool aReadsBColor,
                        bool aReadsBDepth,
                        RenderPassStoreProofSummary aProof,
                        RenderPassStoreProofSummary bProof,
                        std::uint32_t currentPassIndex) noexcept {
    if (renderPassReentryTopLimit() == 0) {
      return;
    }
    RenderPassReentryTopEntry* insert = nullptr;
    RenderPassReentryTopEntry* weakest = nullptr;
    for (auto& entry : reentryTop) {
      if (entry.used() && entry.a == a && entry.b == b &&
          entry.firstEncoder == encoder && entry.firstBEncoder == bEncoder &&
          entry.priorASeq == priorASeq &&
          entry.priorAEncoder == priorAEncoder &&
          entry.priorAPass == priorAPass &&
          entry.bReadsAColor == bReadsAColor &&
          entry.bReadsADepth == bReadsADepth &&
          entry.aReadsBColor == aReadsBColor &&
          entry.aReadsBDepth == aReadsBDepth &&
          entry.aColorProof == aProof.color &&
          entry.aDepthProof == aProof.depth &&
          entry.bColorProof == bProof.color &&
          entry.bDepthProof == bProof.depth &&
          entry.aColorTouchDistance == aProof.colorTouchDistance &&
          entry.aDepthTouchDistance == aProof.depthTouchDistance &&
          entry.bColorTouchDistance == bProof.colorTouchDistance &&
          entry.bDepthTouchDistance == bProof.depthTouchDistance) {
        ++entry.count;
        entry.preservationBytes += preservationBytes;
        entry.lastSeq = seq;
        entry.lastEncoder = encoder;
        entry.lastPass = currentPassIndex;
        entry.lastBSeq = bSeq;
        entry.lastBEncoder = bEncoder;
        entry.lastBPass = bPass;
        return;
      }
      if (!entry.used() && !insert) {
        insert = &entry;
      }
      if (entry.used() && (!weakest || entry.preservationBytes < weakest->preservationBytes)) {
        weakest = &entry;
      }
    }
    if (!insert) {
      insert = weakest;
      if (!insert || insert->preservationBytes >= preservationBytes) {
        return;
      }
    }
    *insert = RenderPassReentryTopEntry{
        .a = a,
        .b = b,
        .count = 1,
        .preservationBytes = preservationBytes,
        .priorASeq = priorASeq,
        .priorAEncoder = priorAEncoder,
        .priorAPass = priorAPass,
        .firstSeq = seq,
        .firstEncoder = encoder,
        .firstPass = currentPassIndex,
        .firstBSeq = bSeq,
        .firstBEncoder = bEncoder,
        .firstBPass = bPass,
        .lastSeq = seq,
        .lastEncoder = encoder,
        .lastPass = currentPassIndex,
        .lastBSeq = bSeq,
        .lastBEncoder = bEncoder,
        .lastBPass = bPass,
        .bReadsAColor = bReadsAColor,
        .bReadsADepth = bReadsADepth,
        .aReadsBColor = aReadsBColor,
        .aReadsBDepth = aReadsBDepth,
        .aColorProof = aProof.color,
        .aDepthProof = aProof.depth,
        .bColorProof = bProof.color,
        .bDepthProof = bProof.depth,
        .aColorTouchDistance = aProof.colorTouchDistance,
        .aDepthTouchDistance = aProof.depthTouchDistance,
        .bColorTouchDistance = bProof.colorTouchDistance,
        .bDepthTouchDistance = bProof.depthTouchDistance,
    };
  }

  void finalizePendingReentry(bool aReadsBColor, bool aReadsBDepth) noexcept {
    if (!pendingReentry.has_value()) {
      return;
    }
    const auto pending = *pendingReentry;
    pendingReentry.reset();
    recordReentryTop(pending.a, pending.b, pending.preservationBytes,
                     pending.priorASeq, pending.priorAEncoder,
                     pending.priorAPass, pending.seq, pending.encoder, pending.bSeq,
                     pending.bEncoder, pending.bPass, pending.bReadsAColor,
                     pending.bReadsADepth, aReadsBColor, aReadsBDepth,
                     pending.aProof, pending.bProof, pending.pass);
  }

  void emitReentryTop() const {
    const std::size_t limit = renderPassReentryTopLimit();
    if (limit == 0) {
      return;
    }
    std::array<const RenderPassReentryTopEntry*, 32> sorted{};
    std::size_t count = 0;
    for (const auto& entry : reentryTop) {
      if (entry.used()) {
        sorted[count++] = &entry;
      }
    }
    if (count == 0) {
      return;
    }
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count),
              [](const auto* a, const auto* b) {
                if (a->preservationBytes != b->preservationBytes) {
                  return a->preservationBytes > b->preservationBytes;
                }
                return a->count > b->count;
              });
    const std::size_t rows = std::min(count, limit);
    for (std::size_t i = 0; i < rows; ++i) {
      const auto& entry = *sorted[i];
      std::fprintf(
          stderr,
          "[dxmt9-perf-render-pass-reentry frame=%llu rank=%zu "
          "a_rt=0x%llx a_depth=0x%llx a_samples=%u "
          "b_rt=0x%llx b_depth=0x%llx b_samples=%u "
          "count=%llu preservation_bytes=%llu "
          "prior_a_seq=%llu prior_a_encoder=%llu prior_a_pass=%u "
          "first_seq=%llu first_encoder=%llu first_pass=%u "
          "first_b_seq=%llu first_b_encoder=%llu first_b_pass=%u "
          "last_seq=%llu last_encoder=%llu last_pass=%u "
          "last_b_seq=%llu last_b_encoder=%llu last_b_pass=%u "
          "b_reads_a_color=%u b_reads_a_depth=%u "
          "a_reads_b_color=%u a_reads_b_depth=%u "
          "a_color_proof=%u a_depth_proof=%u "
          "b_color_proof=%u b_depth_proof=%u "
          "a_color_touch_distance=%u a_depth_touch_distance=%u "
          "b_color_touch_distance=%u b_depth_touch_distance=%u]\n",
          static_cast<unsigned long long>(frameIndex),
          i + 1,
          static_cast<unsigned long long>(entry.a.color0),
          static_cast<unsigned long long>(entry.a.depth),
          entry.a.sampleCount,
          static_cast<unsigned long long>(entry.b.color0),
          static_cast<unsigned long long>(entry.b.depth),
          entry.b.sampleCount,
          static_cast<unsigned long long>(entry.count),
          static_cast<unsigned long long>(entry.preservationBytes),
          static_cast<unsigned long long>(entry.priorASeq),
          static_cast<unsigned long long>(entry.priorAEncoder),
          entry.priorAPass,
          static_cast<unsigned long long>(entry.firstSeq),
          static_cast<unsigned long long>(entry.firstEncoder),
          entry.firstPass,
          static_cast<unsigned long long>(entry.firstBSeq),
          static_cast<unsigned long long>(entry.firstBEncoder),
          entry.firstBPass,
          static_cast<unsigned long long>(entry.lastSeq),
          static_cast<unsigned long long>(entry.lastEncoder),
          entry.lastPass,
          static_cast<unsigned long long>(entry.lastBSeq),
          static_cast<unsigned long long>(entry.lastBEncoder),
          entry.lastBPass,
          entry.bReadsAColor ? 1u : 0u,
          entry.bReadsADepth ? 1u : 0u,
          entry.aReadsBColor ? 1u : 0u,
          entry.aReadsBDepth ? 1u : 0u,
          static_cast<unsigned>(entry.aColorProof),
          static_cast<unsigned>(entry.aDepthProof),
          static_cast<unsigned>(entry.bColorProof),
          static_cast<unsigned>(entry.bDepthProof),
          entry.aColorTouchDistance,
          entry.aDepthTouchDistance,
          entry.bColorTouchDistance,
          entry.bDepthTouchDistance);
    }
  }

  ActiveSeedMergeJoinRelation noteStart(RenderPassFrameKey key,
                 RenderPassAttachmentFootprint footprint,
                 RenderPassStoreProofSummary proof,
                 bool openedWithClear,
                 std::uint64_t currentLoadBytes,
                 std::uint64_t seq,
                 std::uint64_t encoder,
                 ReplayWindowProvenance replayWindow,
                 std::uint32_t sourceIndex,
                 std::uint32_t commandIndex,
                 const ActiveSeedMergeTicketContext& ticket,
                 const ActiveSeedMergeTargetWitness* target) {
    ActiveSeedMergeJoinRelation seedJoin = target
        ? ActiveSeedMergeJoinRelation::Mismatch
        : ActiveSeedMergeJoinRelation::NotTarget;
    if (!key.valid()) {
      return seedJoin;
    }
    const bool previousPassReadsPreviousColor = currentReadsPreviousColor;
    const bool previousPassReadsPreviousDepth = currentReadsPreviousDepth;
    const bool previousPassOpenedWithClear = lastOpenedWithClear;
    finalizePendingReentry(previousPassReadsPreviousColor, previousPassReadsPreviousDepth);
    const std::uint32_t currentPassIndex = passIndex++;
    const bool replayWindowAttributionEnabled = perf::enabled();
    if (replayWindowAttributionEnabled) {
      const auto closeObservation = closeLedger.noteStart(
          last.has_value()
              ? RenderPassInstanceToken{
                    .seqId = lastSeq,
                    .encoderIndex = lastEncoder,
                }
              : RenderPassInstanceToken{},
          RenderPassCloseKey{
              .color0 = key.color0,
              .depth = key.depth,
              .sampleCount = key.sampleCount,
          });
      if (closeObservation.terminal ==
          RenderPassCloseTerminalRelation::AdjacentSameKey) {
        perf::countRenderPassCloseLedgerTerminalAdjacent();
        perf::countRenderPassCloseLedgerAdjacentCause(
            closeObservation.terminalCause);
        if (closeObservation.terminalSplitReason ==
            perf::EncoderSplitReason::Final) {
          perf::countRenderPassFinalCloseLedgerTerminalAdjacent();
        }
      } else if (closeObservation.terminal ==
                 RenderPassCloseTerminalRelation::AdjacentDifferentKey) {
        perf::countRenderPassCloseLedgerTerminalNonAdjacent();
        if (closeObservation.terminalSplitReason ==
            perf::EncoderSplitReason::Final) {
          perf::countRenderPassFinalCloseLedgerTerminalNonAdjacent();
        }
      }
    }
    if (replayWindowAttributionEnabled &&
        isNaturalFallbackWindow(replayWindow)) {
      perf::countRenderPassNaturalFallbackBegin();
    }
    if (last.has_value() && *last != key) {
      const bool sameRt = last->color0 != 0 && last->color0 == key.color0;
      const bool sameDepth = last->depth != 0 && last->depth == key.depth;
      if (!sameRt && sameDepth) {
        perf::countRenderPassTransitionRtChangeSameDepth();
      } else if (sameRt && !sameDepth) {
        perf::countRenderPassTransitionSameRtDepthChange();
      } else if (!sameRt && !sameDepth) {
        perf::countRenderPassTransitionRtDepthChange();
      }
    }
    const auto seenIndex = findSeenIndex(key);
    if (seenIndex.has_value()) {
      if (last.has_value() && *last == key) {
        perf::countRenderPassSameKeyAdjacent();
      } else {
        perf::countRenderPassSameKeyReentry();
        const auto lastPassIndex = lastSeenPassIndex[*seenIndex];
        const std::uint32_t interveningPasses =
            currentPassIndex > lastPassIndex ? currentPassIndex - lastPassIndex - 1u : 0u;
        perf::countRenderPassSameKeyReentryDistance(interveningPasses);
        std::optional<perf::EncoderSplitReason> shortPriorCloseReason{};
        bool shortPriorCloseLookupAttempted = false;
        if (replayWindowAttributionEnabled &&
            interveningPasses >= 1u && interveningPasses <= 2u) {
          std::array<ReplayWindowProvenance, 2> shortInterval{};
          const std::uint32_t firstIntervening =
              currentPassIndex - interveningPasses;
          for (std::uint32_t i = 0; i < interveningPasses; ++i) {
            shortInterval[i] = recentReplayWindows[
                (firstIntervening + i) % recentReplayWindows.size()];
          }
          const auto disposition = classifyShortReentryDisposition(
              lastSeenReplayWindow[*seenIndex],
              std::span<const ReplayWindowProvenance>(
                  shortInterval.data(), interveningPasses),
              replayWindow);
          perf::countRenderPassShortReentryDisposition(
              interveningPasses, static_cast<std::uint8_t>(disposition));
          std::array<std::uint64_t, 2> interveningSeqs{};
          for (std::uint32_t i = 0; i < interveningPasses; ++i) {
            interveningSeqs[i] = recentSeqIds[
                (firstIntervening + i) % recentSeqIds.size()];
          }
          const auto sourceShape = classifyShortReentrySourceShape(
              lastSeenSeq[*seenIndex],
              std::span<const std::uint64_t>(interveningSeqs.data(),
                                             interveningPasses),
              seq);
          perf::countRenderPassShortReentrySourceShape(
              interveningPasses, static_cast<std::uint8_t>(sourceShape));

          const auto closeObservation = closeLedger.noteStart(
              {},
              RenderPassCloseKey{
                  .color0 = key.color0,
                  .depth = key.depth,
                  .sampleCount = key.sampleCount,
              },
              RenderPassInstanceToken{
                  .seqId = lastSeenSeq[*seenIndex],
                  .encoderIndex = lastSeenEncoder[*seenIndex],
              });
          shortPriorCloseLookupAttempted =
              closeObservation.shortCrossLookupAttempted;
          shortPriorCloseReason =
              closeObservation.shortCrossPriorSplitReason;
          if (shortPriorCloseReason) {
            perf::countRenderPassShortReentryPriorClose(
                *shortPriorCloseReason);
          } else {
            perf::countRenderPassShortReentryPriorCloseMissing();
          }
          const auto clearOpenTarget =
              classifyShortReentryClearOpenTarget(
                  interveningPasses, disposition, sourceShape,
                  previousPassOpenedWithClear, shortPriorCloseReason);
          if (clearOpenTarget !=
              ShortReentryClearOpenTarget::Excluded) {
            perf::countRenderPassShortReentryClearOpenTarget(
                clearOpenTarget ==
                    ShortReentryClearOpenTarget::NaturalCross,
                closeObservation.shortCrossPriorStoreBytes,
                currentLoadBytes);
          }
        }
        if (replayWindowAttributionEnabled &&
            interveningPasses >= 1u && interveningPasses <= 4u) {
          const auto relation =
              classifyNaturalFallbackReentryFromRecentHistory(
              lastSeenReplayWindow[*seenIndex],
              recentReplayWindows, currentPassIndex, interveningPasses,
              replayWindow);
          if (relation != NaturalFallbackReentryRelation::Excluded) {
            perf::countRenderPassNaturalFallbackReentryDistance(
                interveningPasses,
                relation == NaturalFallbackReentryRelation::SameWindow);
            if (relation == NaturalFallbackReentryRelation::CrossWindow) {
              if (shortPriorCloseReason) {
                perf::countRenderPassNaturalShortCrossPriorClose(
                    *shortPriorCloseReason);
              } else if (shortPriorCloseLookupAttempted) {
                perf::countRenderPassNaturalShortCrossPriorCloseMissing();
              } else {
                const auto closeObservation = closeLedger.noteStart(
                    {},
                    RenderPassCloseKey{
                        .color0 = key.color0,
                        .depth = key.depth,
                        .sampleCount = key.sampleCount,
                    },
                    RenderPassInstanceToken{
                        .seqId = lastSeenSeq[*seenIndex],
                        .encoderIndex = lastSeenEncoder[*seenIndex],
                    });
                if (closeObservation.shortCrossPriorSplitReason) {
                  perf::countRenderPassNaturalShortCrossPriorClose(
                      *closeObservation.shortCrossPriorSplitReason);
                } else if (closeObservation.shortCrossLookupAttempted) {
                  perf::countRenderPassNaturalShortCrossPriorCloseMissing();
                }
              }
            }
          }
        }
        if (target) {
          std::array<ReplayWindowProvenance, 4> seedBridgeInterval{};
          if (interveningPasses >= 1u && interveningPasses <= 4u) {
            const std::uint32_t firstIntervening =
                currentPassIndex - interveningPasses;
            for (std::uint32_t i = 0; i < interveningPasses; ++i) {
              seedBridgeInterval[i] = recentReplayWindows[
                  (firstIntervening + i) % recentReplayWindows.size()];
            }
          }
          seedJoin = classifyActiveSeedMergePassStart(
              ticket, *target, replayWindow, sourceIndex, commandIndex,
              RenderPassInstanceToken{
                  .seqId = lastSeenSeq[*seenIndex],
                  .encoderIndex = lastSeenEncoder[*seenIndex],
              },
              std::span<const ReplayWindowProvenance>(
                  seedBridgeInterval.data(),
                  interveningPasses >= 1u && interveningPasses <= 4u
                      ? interveningPasses
                      : 0u));
          if (seedJoin == ActiveSeedMergeJoinRelation::Matched) {
            perf::countRenderPassActiveSeedBridgeReentryDistance(
                interveningPasses);
          }
        }
        const std::uint64_t preservationBytes = footprint.totalBytes() * 2u;
        const std::uint64_t priorASeq = lastSeenSeq[*seenIndex];
        const std::uint64_t priorAEncoder = lastSeenEncoder[*seenIndex];
        const std::uint32_t priorAPass = lastSeenPass[*seenIndex];
        if (interveningPasses == 1 && last.has_value()) {
          perf::countRenderPassSameKeyReentryDistance1Shape(
              last->color0 != 0 && last->color0 == key.color0,
              last->depth != 0 && last->depth == key.depth,
              preservationBytes);
          pendingReentry = PendingRenderPassReentryTop{
              .a = key,
              .b = *last,
              .preservationBytes = preservationBytes,
              .priorASeq = priorASeq,
              .priorAEncoder = priorAEncoder,
              .priorAPass = priorAPass,
              .seq = seq,
              .encoder = encoder,
              .pass = currentPassIndex,
              .bSeq = lastSeq,
              .bEncoder = lastEncoder,
              .bPass = lastPass,
              .bReadsAColor = previousPassReadsPreviousColor,
              .bReadsADepth = previousPassReadsPreviousDepth,
              .aProof = proof,
              .bProof = lastProof,
          };
        }
        // A same-key re-entry generally implies the previous pass stored
        // the attachment contents and this pass loads them again. Count
        // that store+load footprint as the preservation budget to attack.
        perf::countRenderPassSameKeyReentryPreservationBytes(preservationBytes);
        perf::countRenderPassSameKeyReentryColorPreservationBytes(
            footprint.color0Bytes * 2u);
        perf::countRenderPassSameKeyReentryDepthPreservationBytes(
            footprint.depthBytes * 2u);
      }
      lastSeenPassIndex[*seenIndex] = currentPassIndex;
      lastSeenSeq[*seenIndex] = seq;
      lastSeenEncoder[*seenIndex] = encoder;
      lastSeenPass[*seenIndex] = currentPassIndex;
      if (replayWindowAttributionEnabled) {
        lastSeenReplayWindow[*seenIndex] = replayWindow;
      }
    } else {
      remember(key, currentPassIndex, seq, encoder, replayWindow,
               replayWindowAttributionEnabled);
    }
    if (replayWindowAttributionEnabled) {
      recentReplayWindows[currentPassIndex % recentReplayWindows.size()] =
          replayWindow;
      recentSeqIds[currentPassIndex % recentSeqIds.size()] = seq;
    }
    previousKeyForCurrent = last;
    currentReadsPreviousColor = false;
    currentReadsPreviousDepth = false;
    last = key;
    lastSeq = seq;
    lastEncoder = encoder;
    lastPass = currentPassIndex;
    lastProof = proof;
    lastOpenedWithClear = openedWithClear;
    return seedJoin;
  }

  void noteDrawRead(const core::FlatDrawStateRecord& hot) noexcept {
    if (!previousKeyForCurrent.has_value()) {
      return;
    }
    const auto relation = readRelationToKey(hot, *previousKeyForCurrent);
    currentReadsPreviousColor = currentReadsPreviousColor || relation.color;
    currentReadsPreviousDepth = currentReadsPreviousDepth || relation.depth;
  }
};

RenderPassFrameTracker& encodeThreadRenderPassFrameTracker() noexcept {
  static thread_local RenderPassFrameTracker tracker;
  return tracker;
}

}  // namespace

void noteRenderPassFrameClose(RenderPassCloseRecord record) noexcept {
  encodeThreadRenderPassFrameTracker().noteClose(record);
}

namespace {

bool isX8TextureFormat(core::Format format) {
  return format == core::Format::X8R8G8B8 ||
         format == core::Format::X8B8G8R8;
}

u32 x8AlphaOneTextureMaskForDraw(core::FlatDrawStateView drawState,
                                 const resources::Pool& pool) {
  if (!x8ShaderAlphaFillEnabledForDiagnostics() || !drawState.hot) {
    return 0;
  }
  const auto& hot = *drawState.hot;
  u32 mask = 0;
  for (u32 stage = 0; stage < core::kMaxFragmentSamplers; ++stage) {
    if ((hot.textureMask & (1u << stage)) == 0u || !hot.textures[stage]) {
      continue;
    }
    const auto* texture = pool.findTexture(hot.textures[stage].value);
    if (texture && isX8TextureFormat(texture->desc.format)) {
      mask |= 1u << stage;
    }
  }
  return mask;
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

u64 shaderVariantHashForDraw(core::FlatDrawStateView drawState,
                             const resources::Pool* pool = nullptr,
                             bool fragmentlessDepthOnly = false) {
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
  if (pool) {
    hash ^= static_cast<u64>(x8AlphaOneTextureMaskForDraw(drawState, *pool)) << 5;
  }
  if (fragmentlessDepthOnly) {
    hash ^= debug::probeFragmentlessDepthOnlyKeepVSOut()
                ? 0x9e3b7a6c8fb4c521ull
                : 0xf1a974f2b7a25c31ull;
  }
  return hash;
}

u64 shaderSourceAttributionKeyForDraw(
    core::FlatDrawStateView drawState,
    std::optional<bool> forceTextureWhiteOverride = std::nullopt,
    bool fragmentlessDepthOnly = false) {
  if (!drawState.hot || !drawState.hasShaderContext()) {
    return 0;
  }
  const auto& hot = *drawState.hot;
  const auto& shader = drawState.shaderContext();
  auto mix = [](u64 seed, u64 value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
    return seed;
  };

  u64 hash = shader.vertexShader.hash;
  hash = mix(hash, shader.pixelShader.hash);
  hash = mix(hash, hot.key.vertexDeclHash);
  hash = mix(hash, hot.key.renderStateHash);
  hash = mix(hash, hot.textureMask);
  hash = mix(hash, hot.renderTargetMask);
  hash = mix(hash, hot.clipPlaneMask);
  hash = mix(hash, hot.colorAttachments[0].sampleCount);
  for (const auto textureTypeHash : hot.key.textureStageStateHashes) {
    hash = mix(hash, textureTypeHash);
  }
  for (const auto samplerHash : hot.key.samplerStateHashes) {
    hash = mix(hash, samplerHash);
  }
  if (forceTextureWhiteOverride.has_value()) {
    hash = mix(hash, 0x58f71e9d1a3b4c25ull);
    hash = mix(hash, static_cast<u64>(*forceTextureWhiteOverride));
  }
  if (fragmentlessDepthOnly) {
    hash = mix(hash,
               debug::probeFragmentlessDepthOnlyKeepVSOut()
                   ? 0x9e3b7a6c8fb4c521ull
                   : 0xf1a974f2b7a25c31ull);
  }
  return hash;
}

u32 vsOutLayoutKeyForDraw(core::FlatDrawStateView drawState,
                          bool tileFfpBaseColor,
                          std::optional<bool> forceTextureWhiteOverride = std::nullopt,
                          bool fragmentlessDepthOnly = false) {
  if (!drawState.hot || !drawState.hasShaderContext()) {
    return 0;
  }
  if (fragmentlessDepthOnly &&
      !debug::probeFragmentlessDepthOnlyKeepVSOut()) {
    return shaders::vsoutLayoutKey(shaders::positionOnlyVSOutLayout());
  }
  auto context =
      drawshader::makeShaderSourceContext(drawState.shaderContext(), *drawState.hot);
  context.stripFogAlphaTestForTileBase = tileFfpBaseColor;
  context.stripAlphaTestForDebug = debug::disableAlphaTest();
  context.stripFogForDebug = debug::disableFog();
  context.forceTextureWhiteForDebug =
      forceTextureWhiteOverride.value_or(debug::forceTextureWhite());
  try {
    context.vsOutLayout = drawshader::resolveVSOutLayoutForShaderPair(context);
    return shaders::vsoutLayoutKey(context.vsOutLayout);
  } catch (...) {
    return shaders::vsoutLayoutKey(shaders::fullVSOutLayout());
  }
}

struct ShaderSourceHashes {
  u64 vertex = 0;
  u64 pixel = 0;
};

bool shaderSourceHashAttributionEnabled() {
  static const bool enabled = [] {
    const char* dir = std::getenv("DXMT_DUMP_SHADER_DIR");
    return dir && dir[0] != '\0';
  }();
  return enabled;
}

ShaderSourceHashes shaderSourceHashesForDraw(core::FlatDrawStateView drawState,
                                             bool tileFfpBaseColor,
                                             bool argbufHybridMode,
                                             bool argbufResourceArray,
                                             bool argbufDirectCbufMode,
                                             bool samplerLodBias,
                                             u32 x8AlphaOneTextureMask,
                                             std::optional<bool> forceTextureWhiteOverride = std::nullopt,
                                             bool fragmentlessDepthOnly = false) {
  ShaderSourceHashes hashes{};
  if (!shaderSourceHashAttributionEnabled() || !drawState.hot ||
      !drawState.hasShaderContext()) {
    return hashes;
  }
  auto context =
      drawshader::makeShaderSourceContext(drawState.shaderContext(), *drawState.hot);
  context.stripFogAlphaTestForTileBase = tileFfpBaseColor;
  context.stripAlphaTestForDebug = debug::disableAlphaTest();
  context.stripFogForDebug = debug::disableFog();
  context.forceTextureWhiteForDebug =
      forceTextureWhiteOverride.value_or(debug::forceTextureWhite());
  context.argbufHybridMode = argbufHybridMode;
  context.argbufResourceArray = argbufHybridMode && argbufResourceArray;
  context.argbufDirectCbufMode =
      argbufHybridMode && !context.argbufResourceArray && argbufDirectCbufMode;
  context.samplerLodBias = samplerLodBias;
  context.x8AlphaOneTextureMask = x8AlphaOneTextureMask;
  try {
    if (fragmentlessDepthOnly) {
      context.vsOutLayout = debug::probeFragmentlessDepthOnlyKeepVSOut()
                                 ? drawshader::resolveVSOutLayoutForShaderPair(context)
                                 : shaders::positionOnlyVSOutLayout();
      context.fragmentlessDepthOnly = true;
    } else {
      context.vsOutLayout = drawshader::resolveVSOutLayoutForShaderPair(context);
    }
    const auto vertex = drawshader::makeDrawShaderSource(context, true);
    hashes.vertex = core::hashString(vertex);
    if (!fragmentlessDepthOnly) {
      const auto pixel = drawshader::makeDrawShaderSource(context, false);
      hashes.pixel = core::hashString(pixel);
    }
  } catch (...) {
    hashes = {};
  }
  return hashes;
}

u64 psoHandleBucket(core::PsoHandle handle) noexcept {
  return handle.valid()
             ? (static_cast<u64>(handle.generation) << 32) |
                   static_cast<u64>(handle.slot)
             : 0ull;
}

void recordPsoAttributionForDraw(ActiveEncoderBreakdown* encoderBreakdown,
                                 core::FlatDrawStateView drawState,
                                 const resources::Pool& pool,
                                 core::PsoHandle renderPsoHandle,
                                 bool tileFfpMode,
                                 bool argbufHybridMode,
                                 bool argbufResourceArray,
                                 bool argbufDirectCbufMode,
                                 std::optional<bool> forceTextureWhiteOverride = std::nullopt,
                                 bool fragmentlessDepthOnly = false) {
  if (!encoderBreakdown || !encoderBreakdown->enabled) {
    return;
  }
  const auto variantHash =
      shaderVariantHashForDraw(drawState, &pool, fragmentlessDepthOnly);
  const auto sourceKey =
      shaderSourceAttributionKeyForDraw(drawState, forceTextureWhiteOverride,
                                        fragmentlessDepthOnly);
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  if (drawState.hasShaderContext()) {
    const auto& shader = drawState.shaderContext();
    vertexShaderHash = shader.vertexShader.hash;
    pixelShaderHash = shader.pixelShader.hash;
  }
  u32 vsOutLayoutKey = 0;
  if (!encoderBreakdown->findCachedVsOutLayout(sourceKey, tileFfpMode,
                                               vsOutLayoutKey)) {
    vsOutLayoutKey =
        vsOutLayoutKeyForDraw(drawState, tileFfpMode, forceTextureWhiteOverride,
                              fragmentlessDepthOnly);
    encoderBreakdown->storeCachedVsOutLayout(sourceKey, tileFfpMode,
                                             vsOutLayoutKey);
  }
  const bool samplerLodBias = anySamplerLodBiasNonzero(drawState);
  const u32 x8AlphaOneTextureMask = x8AlphaOneTextureMaskForDraw(drawState, pool);
  u64 vertexShaderSourceHash = 0;
  u64 pixelShaderSourceHash = 0;
  if (!encoderBreakdown->findCachedShaderSourceHashes(
          sourceKey, tileFfpMode, argbufHybridMode,
          argbufHybridMode && argbufResourceArray,
          argbufHybridMode && !argbufResourceArray && argbufDirectCbufMode,
          samplerLodBias,
          x8AlphaOneTextureMask,
          vertexShaderSourceHash, pixelShaderSourceHash)) {
    const auto sourceHashes = shaderSourceHashesForDraw(
        drawState, tileFfpMode, argbufHybridMode, argbufResourceArray,
        argbufDirectCbufMode, samplerLodBias, x8AlphaOneTextureMask,
        forceTextureWhiteOverride, fragmentlessDepthOnly);
    vertexShaderSourceHash = sourceHashes.vertex;
    pixelShaderSourceHash = sourceHashes.pixel;
    encoderBreakdown->storeCachedShaderSourceHashes(
        sourceKey, tileFfpMode, argbufHybridMode,
        argbufHybridMode && argbufResourceArray,
        argbufHybridMode && !argbufResourceArray && argbufDirectCbufMode,
        samplerLodBias,
        x8AlphaOneTextureMask,
        vertexShaderSourceHash, pixelShaderSourceHash);
  }
  encoderBreakdown->recordPsoState(
      psoHandleBucket(renderPsoHandle), variantHash, vsOutLayoutKey,
      vertexShaderHash, pixelShaderHash, vertexShaderSourceHash,
      pixelShaderSourceHash);
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

template <std::size_t Capacity>
u64 estimateVertexCacheMisses(std::span<const u32> indices) {
  std::array<u32, Capacity> cache{};
  std::size_t valid = 0;
  u64 misses = 0;

  for (const u32 index : indices) {
    std::size_t hit = valid;
    for (std::size_t i = 0; i < valid; ++i) {
      if (cache[i] == index) {
        hit = i;
        break;
      }
    }

    if (hit == valid) {
      ++misses;
      hit = std::min(valid, Capacity - 1u);
      if (valid < Capacity) {
        ++valid;
      }
    }

    for (std::size_t i = hit; i > 0; --i) {
      cache[i] = cache[i - 1u];
    }
    cache[0] = index;
  }

  return misses;
}

IndexReuseMeasure measureIndexReuseForDraw(std::span<const u8> indexBytes,
                                           IndexType indexType,
                                           u32 startIndex,
                                           u64 indexCount) {
  IndexReuseMeasure out{.references = indexCount};
  if (indexBytes.empty() || indexCount == 0u) {
    return out;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return out;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return out;
  }

  std::vector<u32> indices;
  indices.reserve(static_cast<std::size_t>(indexCount));
  for (u64 i = 0; i < indexCount; ++i) {
    const std::size_t offset = startByte + static_cast<std::size_t>(i) * elementSize;
    if (indexType == IndexType::UInt16) {
      u16 value = 0;
      std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
      indices.push_back(value);
    } else {
      u32 value = 0;
      std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
      indices.push_back(value);
    }
  }
  if (indices.empty()) {
    return out;
  }
  out.firstIndex = indices.front();
  out.lastIndex = indices.back();
  out.minIndex = std::numeric_limits<u32>::max();
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const u32 index = indices[i];
    out.minIndex = std::min(out.minIndex, index);
    out.maxIndex = std::max(out.maxIndex, index);
    if (i != 0u) {
      const u32 prev = indices[i - 1u];
      const u32 delta = index > prev ? index - prev : prev - index;
      out.adjacentDeltaAbsSum += delta;
      out.adjacentDeltaMax = std::max(out.adjacentDeltaMax, delta);
      if (index < prev) {
        ++out.backwardJumps;
      }
    }
  }
  if ((indices.size() % 3u) == 0u) {
    for (std::size_t i = 0; i < indices.size(); i += 3u) {
      const u32 triMin = std::min({indices[i], indices[i + 1u], indices[i + 2u]});
      const u32 triMax = std::max({indices[i], indices[i + 1u], indices[i + 2u]});
      const u32 span = triMax - triMin + 1u;
      out.triangleIndexSpanSum += span;
      out.triangleIndexSpanMax = std::max(out.triangleIndexSpanMax, span);
    }
  }
  out.cacheMiss16 = estimateVertexCacheMisses<16>(indices);
  out.cacheMiss32 = estimateVertexCacheMisses<32>(indices);
  out.cacheMiss64 = estimateVertexCacheMisses<64>(indices);
  std::sort(indices.begin(), indices.end());
  out.unique = static_cast<u64>(std::unique(indices.begin(), indices.end()) -
                                indices.begin());
  out.available = true;
  return out;
}

IndexReuseMeasure measureIndexCacheMiss32ForDraw(std::span<const u8> indexBytes,
                                                 IndexType indexType,
                                                 u32 startIndex,
                                                 u64 indexCount) {
  IndexReuseMeasure out{.references = indexCount};
  if (indexBytes.empty() || indexCount == 0u) {
    return out;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return out;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return out;
  }

  std::array<u32, 32> cache{};
  std::size_t cacheSize = 0;
  u64 misses = 0;
  for (u64 i = 0; i < indexCount; ++i) {
    const std::size_t offset = startByte + static_cast<std::size_t>(i) * elementSize;
    u32 index = 0;
    if (indexType == IndexType::UInt16) {
      u16 value = 0;
      std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
      index = value;
    } else {
      std::memcpy(&index, indexBytes.data() + offset, sizeof(index));
    }

    std::size_t hit = cacheSize;
    for (std::size_t j = 0; j < cacheSize; ++j) {
      if (cache[j] == index) {
        hit = j;
        break;
      }
    }
    if (hit == cacheSize) {
      ++misses;
      if (cacheSize < cache.size()) {
        ++cacheSize;
      }
      hit = cacheSize - 1u;
    }
    for (std::size_t j = hit; j > 0u; --j) {
      cache[j] = cache[j - 1u];
    }
    cache[0] = index;
  }

  out.cacheMiss32 = misses;
  out.available = true;
  return out;
}

u64 stream0ByteSpanForIndexMeasure(const IndexReuseMeasure& measure,
                                   u64 stream0Stride) {
  if (!measure.available || stream0Stride == 0u) {
    return 0u;
  }
  return static_cast<u64>(measure.maxIndex - measure.minIndex) * stream0Stride;
}

bool indexCacheCandidateMeetsGainGate(const IndexReuseMeasure& original,
                                      const IndexReuseMeasure& candidate,
                                      std::uint32_t minGainPct) {
  if (!original.available || !candidate.available ||
      original.cacheMiss32 == 0u ||
      candidate.cacheMiss32 >= original.cacheMiss32) {
    return false;
  }
  const u64 delta = original.cacheMiss32 - candidate.cacheMiss32;
  return (delta * 100u) / original.cacheMiss32 >= minGainPct;
}

bool writeBinaryFile(const std::filesystem::path& path,
                     const u8* data,
                     std::size_t size) {
  if (!data && size != 0u) {
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  if (size != 0u) {
    out.write(reinterpret_cast<const char*>(data),
              static_cast<std::streamsize>(size));
  }
  return out.good();
}

bool writeTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << text;
  return out.good();
}

bool indexedGeometryDumpShaderMatches(core::FlatDrawStateView drawState) {
  const auto vertexFilter = debug::indexedGeometryDumpVertexShaderHash();
  const auto pixelFilter = debug::indexedGeometryDumpPixelShaderHash();
  if (!vertexFilter.has_value() && !pixelFilter.has_value()) {
    return true;
  }
  if (!drawState.hasShaderContext()) {
    return false;
  }
  const auto& shader = drawState.shaderContext();
  return (!vertexFilter.has_value() || shader.vertexShader.hash == *vertexFilter) &&
         (!pixelFilter.has_value() || shader.pixelShader.hash == *pixelFilter);
}

bool texture0FilterMatches(core::FlatDrawStateView drawState,
                           const resources::Pool& pool,
                           std::optional<u64> texture0Filter,
                           std::optional<u64> texture0WidthFilter,
                           std::optional<u64> texture0HeightFilter,
                           std::optional<u64> texture0FormatFilter) {
  if (!texture0Filter.has_value() &&
      !texture0WidthFilter.has_value() &&
      !texture0HeightFilter.has_value() &&
      !texture0FormatFilter.has_value()) {
    return true;
  }
  if (!drawState.hot || !drawState.hot->textures[0]) {
    return false;
  }
  const auto handle = drawState.hot->textures[0].value;
  if (texture0Filter.has_value() && handle != *texture0Filter) {
    return false;
  }
  if (!texture0WidthFilter.has_value() &&
      !texture0HeightFilter.has_value() &&
      !texture0FormatFilter.has_value()) {
    return true;
  }
  const auto* texture = pool.findTexture(handle);
  if (!texture) {
    return false;
  }
  if (texture0WidthFilter.has_value() &&
      texture->desc.width != *texture0WidthFilter) {
    return false;
  }
  if (texture0HeightFilter.has_value() &&
      texture->desc.height != *texture0HeightFilter) {
    return false;
  }
  if (texture0FormatFilter.has_value() &&
      static_cast<u64>(texture->desc.format) != *texture0FormatFilter) {
    return false;
  }
  return true;
}

bool indexedGeometryDumpTextureMatches(core::FlatDrawStateView drawState,
                                       const resources::Pool& pool) {
  return texture0FilterMatches(drawState,
                               pool,
                               debug::indexedGeometryDumpTexture0Handle(),
                               debug::indexedGeometryDumpTexture0Width(),
                               debug::indexedGeometryDumpTexture0Height(),
                               debug::indexedGeometryDumpTexture0Format());
}

bool depthFuncAlwaysProbeTextureMatches(core::FlatDrawStateView drawState,
                                        const resources::Pool& pool) {
  return texture0FilterMatches(drawState,
                               pool,
                               debug::probeDepthFuncAlwaysTexture0Handle(),
                               debug::probeDepthFuncAlwaysTexture0Width(),
                               debug::probeDepthFuncAlwaysTexture0Height(),
                               debug::probeDepthFuncAlwaysTexture0Format());
}

bool forceTextureWhiteProbeTextureMatches(core::FlatDrawStateView drawState,
                                          const resources::Pool& pool) {
  return texture0FilterMatches(drawState,
                               pool,
                               debug::probeForceTextureWhiteTexture0Handle(),
                               debug::probeForceTextureWhiteTexture0Width(),
                               debug::probeForceTextureWhiteTexture0Height(),
                               debug::probeForceTextureWhiteTexture0Format());
}

bool forceTextureWhiteProbeDrawOrdinalMatches(u64 drawOrdinal) {
  const auto range = debug::probeForceTextureWhiteDrawOrdinalRange();
  const auto list = debug::probeForceTextureWhiteDrawOrdinalList();
  if (!debug::drawOrdinalRangeEnabled(range) && !list.enabled) {
    return true;
  }
  if (debug::drawOrdinalRangeEnabled(range) &&
      debug::shouldSkipDrawOrdinal(drawOrdinal, range)) {
    return false;
  }
  return !list.enabled || debug::drawOrdinalListContains(list, drawOrdinal);
}

bool forceTextureWhiteProbeCommandIndexMatches(std::uint32_t commandIndex) {
  const auto range = debug::probeForceTextureWhiteCommandIndexRange();
  const auto list = debug::probeForceTextureWhiteCommandIndexList();
  const auto ordinal = static_cast<u64>(commandIndex);
  if (!debug::drawOrdinalRangeEnabled(range) && !list.enabled) {
    return true;
  }
  if (debug::drawOrdinalRangeEnabled(range) &&
      debug::shouldSkipDrawOrdinal(ordinal, range)) {
    return false;
  }
  return !list.enabled || debug::drawOrdinalListContains(list, ordinal);
}

bool forceTextureWhiteProbeCommandDrawIndexMatches(u64 commandDrawIndex) {
  const auto range = debug::probeForceTextureWhiteCommandDrawIndexRange();
  const auto list = debug::probeForceTextureWhiteCommandDrawIndexList();
  if (!debug::drawOrdinalRangeEnabled(range) && !list.enabled) {
    return true;
  }
  if (debug::drawOrdinalRangeEnabled(range) &&
      debug::shouldSkipDrawOrdinal(commandDrawIndex, range)) {
    return false;
  }
  return !list.enabled || debug::drawOrdinalListContains(list, commandDrawIndex);
}

bool disableAlphaBlendProbeTextureMatches(core::FlatDrawStateView drawState,
                                          const resources::Pool& pool) {
  return texture0FilterMatches(drawState,
                               pool,
                               debug::probeDisableAlphaBlendTexture0Handle(),
                               debug::probeDisableAlphaBlendTexture0Width(),
                               debug::probeDisableAlphaBlendTexture0Height(),
                               debug::probeDisableAlphaBlendTexture0Format());
}

struct IndexedGeometryStreamPayload {
  u32 stream = 0;
  u32 metalSlot = 0;
  u64 handle = 0;
  u64 offset = 0;
  u64 stride = 0;
  std::span<const u8> bytes{};
};

void appendIndexedGeometryTextureMetadata(std::ostringstream& meta,
                                          core::FlatDrawStateView drawState,
                                          const resources::Pool& pool) {
  if (!drawState.hot) {
    return;
  }
  const auto& hot = *drawState.hot;
  meta << "texture_mask=0x" << std::hex << hot.textureMask << std::dec << "\n";
  for (u32 stage = 0; stage < core::kMaxTextureStages; ++stage) {
    const auto handle = hot.textures[stage];
    if (!handle) {
      continue;
    }
    meta << "texture" << stage << "_handle=0x" << std::hex << handle.value
         << std::dec << "\n"
         << "texture" << stage << "_lod=" << hot.textureLods[stage] << "\n";
    if (const auto* texture = pool.findTexture(handle.value)) {
      meta << "texture" << stage << "_format="
           << static_cast<unsigned>(texture->desc.format) << "\n"
           << "texture" << stage << "_type="
           << static_cast<unsigned>(texture->desc.type) << "\n"
           << "texture" << stage << "_pool="
           << static_cast<unsigned>(texture->desc.pool) << "\n"
           << "texture" << stage << "_usage=0x" << std::hex
           << texture->desc.usage << std::dec << "\n"
           << "texture" << stage << "_width=" << texture->desc.width << "\n"
           << "texture" << stage << "_height=" << texture->desc.height << "\n"
           << "texture" << stage << "_depth=" << texture->desc.depth << "\n"
           << "texture" << stage << "_levels=" << texture->desc.levels << "\n"
           << "texture" << stage << "_has_metal_texture="
           << (texture->texture ? 1 : 0) << "\n"
           << "texture" << stage << "_has_shader_read_texture="
           << (texture->shaderReadTexture ? 1 : 0) << "\n"
           << "texture" << stage << "_has_srgb_shader_read_texture="
           << (texture->srgbShaderReadTexture ? 1 : 0) << "\n";
    } else {
      meta << "texture" << stage << "_missing_record=1\n";
    }
  }
}

void appendIndexedGeometryAttachmentMetadata(std::ostringstream& meta,
                                             core::FlatDrawStateView drawState,
                                             const resources::Pool& pool) {
  if (!drawState.hot) {
    return;
  }
  const auto& hot = *drawState.hot;
  auto appendSurface = [&](std::string_view prefix,
                           core::RenderTargetAttachment attachment) {
    const auto handle = attachment.handle;
    meta << prefix << "_handle=0x" << std::hex << handle.value << std::dec << "\n"
         << prefix << "_level=" << attachment.level << "\n"
         << prefix << "_sample_count=" << attachment.sampleCount << "\n";
    if (!handle) {
      return;
    }
    const auto* surface = pool.findSurface(handle.value);
    if (!surface) {
      meta << prefix << "_missing_surface=1\n";
      return;
    }
    meta << prefix << "_format=" << static_cast<unsigned>(surface->desc.format) << "\n"
         << prefix << "_pool=" << static_cast<unsigned>(surface->desc.pool) << "\n"
         << prefix << "_usage=0x" << std::hex << surface->desc.usage << std::dec << "\n"
         << prefix << "_width=" << surface->desc.width << "\n"
         << prefix << "_height=" << surface->desc.height << "\n"
         << prefix << "_bytes_per_pixel=" << core::bytesPerPixel(surface->desc.format) << "\n"
         << prefix << "_render_target=" << (surface->desc.renderTarget ? 1 : 0) << "\n"
         << prefix << "_depth_stencil=" << (surface->desc.depthStencil ? 1 : 0) << "\n"
         << prefix << "_has_metal_texture=" << (surface->texture ? 1 : 0) << "\n"
         << prefix << "_has_srgb_texture=" << (surface->srgbTexture ? 1 : 0) << "\n"
         << prefix << "_has_resolve_texture=" << (surface->resolveTexture ? 1 : 0) << "\n"
         << prefix << "_alias_texture=0x" << std::hex << surface->aliasTexture.value
         << std::dec << "\n"
         << prefix << "_alias_level=" << surface->level << "\n"
         << prefix << "_alias_slice=" << surface->slice << "\n";
    if (surface->aliasTexture) {
      const auto* texture = pool.findTexture(surface->aliasTexture.value);
      if (texture) {
        meta << prefix << "_alias_texture_format="
             << static_cast<unsigned>(texture->desc.format) << "\n"
             << prefix << "_alias_texture_type="
             << static_cast<unsigned>(texture->desc.type) << "\n"
             << prefix << "_alias_texture_usage=0x" << std::hex
             << texture->desc.usage << std::dec << "\n"
             << prefix << "_alias_texture_width=" << texture->desc.width << "\n"
             << prefix << "_alias_texture_height=" << texture->desc.height << "\n"
             << prefix << "_alias_texture_levels=" << texture->desc.levels << "\n";
      }
    }
  };

  for (u32 index = 0; index < core::kMaxRenderTargets; ++index) {
    std::ostringstream prefix;
    prefix << "attachment_color" << index;
    appendSurface(prefix.str(), hot.colorAttachments[index]);
  }
  appendSurface("attachment_depth", hot.depthStencil);
}

void maybeDumpIndexedGeometryPayload(
    const ActiveEncoderBreakdown* encoderBreakdown,
    core::FlatDrawStateView drawState,
    const resources::Pool& pool,
    std::span<const u8> indexBytes,
    std::span<const u8> vertexBytes,
    const IndexReuseMeasure& indexReuse,
    IndexType indexType,
    u32 startIndex,
    u64 indexCount,
    i32 baseVertexIndex,
    u64 stream0Offset,
    u64 stream0Stride,
    u64 stream0Handle,
    u64 indexBufferHandle,
    u64 vertexShaderHash,
    u64 pixelShaderHash,
    u64 drawOrdinal,
    u64 primitiveCount,
    std::span<const IndexedGeometryStreamPayload> extraStreams = {},
    std::span<const u8> vsConstsBytes = {},
    std::span<const u8> psConstsBytes = {},
    std::span<const u8> ffpVsConstsBytes = {},
    std::span<const u8> ffpPsConstsBytes = {}) {
  const auto dir = debug::indexedGeometryDumpDir();
  if (dir.empty() || !encoderBreakdown || !indexReuse.available) {
    return;
  }
  const std::uint32_t maxDumps = debug::indexedGeometryDumpMaxDraws();
  if (maxDumps == 0u) {
    return;
  }

  std::error_code ec;
  const std::filesystem::path outDir{std::string(dir)};
  std::filesystem::create_directories(outDir, ec);
  if (ec) {
    return;
  }

  const std::size_t indexSize = indexElementSize(indexType);
  const u64 indexStartByte64 = static_cast<u64>(startIndex) * indexSize;
  const u64 indexByteCount64 = indexCount * indexSize;
  bool indexRangeValid =
      indexStartByte64 <= static_cast<u64>(indexBytes.size()) &&
      indexByteCount64 <= static_cast<u64>(indexBytes.size()) - indexStartByte64;
  const std::size_t indexStartByte =
      indexRangeValid ? static_cast<std::size_t>(indexStartByte64) : 0u;
  const std::size_t indexByteCount =
      indexRangeValid ? static_cast<std::size_t>(indexByteCount64) : 0u;

  bool streamRangeValid = false;
  std::size_t streamStartByte = 0u;
  std::size_t streamByteCount = 0u;
  const auto minVertex =
      static_cast<std::int64_t>(baseVertexIndex) +
      static_cast<std::int64_t>(indexReuse.minIndex);
  const auto maxVertex =
      static_cast<std::int64_t>(baseVertexIndex) +
      static_cast<std::int64_t>(indexReuse.maxIndex);
  if (stream0Stride != 0u && minVertex >= 0 && maxVertex >= minVertex) {
    const u64 minVertex64 = static_cast<u64>(minVertex);
    const u64 vertexSpan = static_cast<u64>(maxVertex - minVertex + 1);
    const u64 streamStart64 = stream0Offset + minVertex64 * stream0Stride;
    const u64 streamCount64 = vertexSpan * stream0Stride;
    streamRangeValid =
        streamStart64 <= static_cast<u64>(vertexBytes.size()) &&
        streamCount64 <= static_cast<u64>(vertexBytes.size()) - streamStart64;
    if (streamRangeValid) {
      streamStartByte = static_cast<std::size_t>(streamStart64);
      streamByteCount = static_cast<std::size_t>(streamCount64);
    }
  }
  if (!indexRangeValid || !streamRangeValid) {
    return;
  }

  static std::atomic<std::uint32_t> dumpCount{0u};
  std::uint32_t slot = dumpCount.load(std::memory_order_relaxed);
  while (slot < maxDumps &&
         !dumpCount.compare_exchange_weak(slot,
                                          slot + 1u,
                                          std::memory_order_relaxed)) {
  }
  if (slot >= maxDumps) {
    return;
  }

  const auto seqId = encoderBreakdown->stats.seqId;
  const auto encoderIndex = encoderBreakdown->stats.encoderIndex;
  std::ostringstream stem;
  stem << "seq" << seqId
       << "-enc" << encoderIndex
       << "-draw" << drawOrdinal
       << "-slot" << slot;
  const auto base = outDir / stem.str();

  const bool wroteIndex =
      indexRangeValid &&
      writeBinaryFile(base.string() + ".index.bin",
                      indexBytes.data() + indexStartByte,
                      indexByteCount);
  const bool wroteStream =
      streamRangeValid &&
      writeBinaryFile(base.string() + ".stream0.bin",
                      vertexBytes.data() + streamStartByte,
                      streamByteCount);
  struct ExtraStreamDumpResult {
    const IndexedGeometryStreamPayload* payload = nullptr;
    bool rangeValid = false;
    std::size_t startByte = 0u;
    std::size_t byteCount = 0u;
    bool wrote = false;
  };
  std::array<ExtraStreamDumpResult, core::kMaxStreams - 1u> extraResults{};
  std::size_t extraResultCount = 0u;
  for (const auto& stream : extraStreams) {
    if (stream.stream == 0u || stream.stream >= core::kMaxStreams ||
        stream.stride == 0u || stream.bytes.empty() ||
        extraResultCount >= extraResults.size()) {
      continue;
    }
    auto& result = extraResults[extraResultCount++];
    result.payload = &stream;
    const u64 minVertex64 = static_cast<u64>(minVertex);
    const u64 vertexSpan = static_cast<u64>(maxVertex - minVertex + 1);
    const u64 streamStart64 = stream.offset + minVertex64 * stream.stride;
    const u64 streamCount64 = vertexSpan * stream.stride;
    result.rangeValid =
        streamStart64 <= static_cast<u64>(stream.bytes.size()) &&
        streamCount64 <= static_cast<u64>(stream.bytes.size()) - streamStart64;
    if (result.rangeValid) {
      result.startByte = static_cast<std::size_t>(streamStart64);
      result.byteCount = static_cast<std::size_t>(streamCount64);
      std::ostringstream suffix;
      suffix << ".stream" << stream.stream << ".bin";
      result.wrote = writeBinaryFile(base.string() + suffix.str(),
                                     stream.bytes.data() + result.startByte,
                                     result.byteCount);
    }
  }
  const bool wroteVsConsts =
      !vsConstsBytes.empty() &&
      writeBinaryFile(base.string() + ".vsconsts.bin",
                      vsConstsBytes.data(), vsConstsBytes.size());
  const bool wrotePsConsts =
      !psConstsBytes.empty() &&
      writeBinaryFile(base.string() + ".psconsts.bin",
                      psConstsBytes.data(), psConstsBytes.size());
  const bool wroteFfpVsConsts =
      !ffpVsConstsBytes.empty() &&
      writeBinaryFile(base.string() + ".ffpvs.bin",
                      ffpVsConstsBytes.data(), ffpVsConstsBytes.size());
  const bool wroteFfpPsConsts =
      !ffpPsConstsBytes.empty() &&
      writeBinaryFile(base.string() + ".ffpps.bin",
                      ffpPsConstsBytes.data(), ffpPsConstsBytes.size());

  std::ostringstream meta;
  meta << "seq=" << seqId << "\n"
       << "encoder=" << encoderIndex << "\n"
       << "encoder_draw_index=" << encoderBreakdown->stats.drawCalls << "\n"
       << "draw_ordinal=" << drawOrdinal << "\n"
       << "slot=" << slot << "\n"
       << "primitive_count=" << primitiveCount << "\n"
       << "index_count=" << indexCount << "\n"
       << "index_type=" << (indexType == IndexType::UInt16 ? "uint16" : "uint32")
       << "\n"
       << "start_index=" << startIndex << "\n"
       << "base_vertex=" << baseVertexIndex << "\n"
       << "index_buffer=0x" << std::hex << indexBufferHandle << std::dec << "\n"
       << "stream0_handle=0x" << std::hex << stream0Handle << std::dec << "\n"
       << "vs=0x" << std::hex << vertexShaderHash << std::dec << "\n"
       << "ps=0x" << std::hex << pixelShaderHash << std::dec << "\n"
       << "stream0_offset=" << stream0Offset << "\n"
       << "stream0_stride=" << stream0Stride << "\n"
       << "min_index=" << indexReuse.minIndex << "\n"
       << "max_index=" << indexReuse.maxIndex << "\n"
       << "unique_indices=" << indexReuse.unique << "\n"
       << "cache_miss_64=" << indexReuse.cacheMiss64 << "\n"
       << "index_start_byte=" << indexStartByte64 << "\n"
       << "index_byte_count=" << indexByteCount64 << "\n"
       << "index_range_valid=" << (indexRangeValid ? 1 : 0) << "\n"
       << "stream0_start_byte=" << streamStartByte << "\n"
       << "stream0_byte_count=" << streamByteCount << "\n"
       << "stream0_range_valid=" << (streamRangeValid ? 1 : 0) << "\n"
       << "wrote_index=" << (wroteIndex ? 1 : 0) << "\n"
       << "wrote_stream0=" << (wroteStream ? 1 : 0) << "\n"
       << "stream_payload_count=" << (1u + extraResultCount) << "\n"
       << "vsconsts_byte_count=" << vsConstsBytes.size() << "\n"
       << "psconsts_byte_count=" << psConstsBytes.size() << "\n"
       << "ffpvs_byte_count=" << ffpVsConstsBytes.size() << "\n"
       << "ffpps_byte_count=" << ffpPsConstsBytes.size() << "\n"
       << "wrote_vsconsts=" << (wroteVsConsts ? 1 : 0) << "\n"
       << "wrote_psconsts=" << (wrotePsConsts ? 1 : 0) << "\n"
       << "wrote_ffpvs=" << (wroteFfpVsConsts ? 1 : 0) << "\n"
       << "wrote_ffpps=" << (wroteFfpPsConsts ? 1 : 0) << "\n";
  if (drawState.hasShaderContext()) {
    const auto& vertexDecl = drawState.shaderContext().vertexDecl;
    meta << "vertex_decl_fvf=0x" << std::hex << vertexDecl.fvf << std::dec << "\n"
         << "vertex_decl_hash=0x"
         << std::hex
         << (drawState.hot ? drawState.hot->key.vertexDeclHash : 0ull)
         << std::dec << "\n"
         << "vertex_decl_element_count=" << vertexDecl.elements.size() << "\n";
    for (std::size_t i = 0; i < vertexDecl.elements.size(); ++i) {
      const auto& element = vertexDecl.elements[i];
      meta << "vertex_decl_element" << i
           << "_stream=" << element.stream << "\n"
           << "vertex_decl_element" << i
           << "_offset=" << element.offset << "\n"
           << "vertex_decl_element" << i
           << "_type=" << element.type << "\n"
           << "vertex_decl_element" << i
           << "_method=" << element.method << "\n"
           << "vertex_decl_element" << i
           << "_usage=" << element.usage << "\n"
           << "vertex_decl_element" << i
           << "_usage_index=" << element.usageIndex << "\n";
    }
    for (std::size_t stream = 0; stream < vertexDecl.streams.size(); ++stream) {
      const auto& binding = vertexDecl.streams[stream];
      const auto computedStride =
          computeVertexDeclStreamStride(vertexDecl, static_cast<u32>(stream));
      if (!binding.buffer && binding.offset == 0u &&
          binding.stride == 0u && computedStride == 0u) {
        continue;
      }
      meta << "vertex_decl_stream" << stream
           << "_has_buffer=" << (binding.buffer ? 1 : 0) << "\n"
           << "vertex_decl_stream" << stream
           << "_offset=" << binding.offset << "\n"
           << "vertex_decl_stream" << stream
           << "_stride=" << binding.stride << "\n"
           << "vertex_decl_stream" << stream
           << "_computed_stride=" << computedStride << "\n";
      if (drawState.hot) {
        meta << "hot_stream" << stream
             << "_handle=0x" << std::hex
             << drawState.hot->streamBuffers[stream].value
             << std::dec << "\n"
             << "hot_stream" << stream
             << "_offset=" << drawState.hot->streamOffsets[stream] << "\n"
             << "hot_stream" << stream
             << "_stride=" << drawState.hot->streamStrides[stream] << "\n";
      }
    }
  }
  for (std::size_t i = 0; i < extraResultCount; ++i) {
    const auto& result = extraResults[i];
    const auto& stream = *result.payload;
    meta << "stream" << stream.stream << "_handle=0x" << std::hex
         << stream.handle << std::dec << "\n"
         << "stream" << stream.stream << "_metal_slot=" << stream.metalSlot << "\n"
         << "stream" << stream.stream << "_offset=" << stream.offset << "\n"
         << "stream" << stream.stream << "_stride=" << stream.stride << "\n"
         << "stream" << stream.stream << "_start_byte="
         << (result.rangeValid ? result.startByte : 0u) << "\n"
         << "stream" << stream.stream << "_byte_count="
         << (result.rangeValid ? result.byteCount : 0u) << "\n"
         << "stream" << stream.stream << "_range_valid="
         << (result.rangeValid ? 1 : 0) << "\n"
         << "wrote_stream" << stream.stream << "=" << (result.wrote ? 1 : 0)
         << "\n";
  }
  appendIndexedGeometryTextureMetadata(meta, drawState, pool);
  appendIndexedGeometryAttachmentMetadata(meta, drawState, pool);
  writeTextFile(base.string() + ".meta", meta.str());
}

struct IndexedDrawChunk {
  u32 startPrimitive = 0;
  u32 primitiveCount = 0;
  u64 stream0Span = 0;
};

std::optional<u32> readIndexValue(std::span<const u8> indexBytes,
                                  IndexType indexType,
                                  std::size_t elementIndex) {
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t offset = elementIndex * elementSize;
  if (offset + elementSize > indexBytes.size()) {
    return std::nullopt;
  }
  if (indexType == IndexType::UInt16) {
    u16 value = 0;
    std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
    return value;
  }
  u32 value = 0;
  std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
  return value;
}

bool buildIndexedDrawChunks(std::span<const u8> indexBytes,
                            IndexType indexType,
                            u32 startIndex,
                            u32 primitiveCount,
                            u32 primitiveLimit,
                            u64 stream0Stride,
                            u64 stream0SpanLimit,
                            std::vector<IndexedDrawChunk>& chunks) {
  chunks.clear();
  if (primitiveCount == 0u) {
    return false;
  }
  const bool primitiveLimited = primitiveLimit != 0u;
  const bool spanLimited = stream0SpanLimit != 0u && stream0Stride != 0u;
  if (!primitiveLimited && !spanLimited) {
    return false;
  }
  if (spanLimited && indexBytes.empty()) {
    return false;
  }

  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t firstElement = static_cast<std::size_t>(startIndex);
  const std::size_t endElement =
      firstElement + static_cast<std::size_t>(primitiveCount) * 3u;
  if (spanLimited && endElement * elementSize > indexBytes.size()) {
    return false;
  }

  u32 chunkStartPrimitive = 0;
  u32 chunkPrimitiveCount = 0;
  u32 chunkMinIndex = std::numeric_limits<u32>::max();
  u32 chunkMaxIndex = 0;

  auto emitChunk = [&] {
    if (chunkPrimitiveCount == 0u) {
      return;
    }
    const u64 stream0Span =
        chunkMinIndex <= chunkMaxIndex
            ? static_cast<u64>(chunkMaxIndex - chunkMinIndex) * stream0Stride
            : 0u;
    chunks.push_back(IndexedDrawChunk{
        .startPrimitive = chunkStartPrimitive,
        .primitiveCount = chunkPrimitiveCount,
        .stream0Span = stream0Span,
    });
  };

  for (u32 primitive = 0; primitive < primitiveCount; ++primitive) {
    u32 triMin = std::numeric_limits<u32>::max();
    u32 triMax = 0;
    if (spanLimited) {
      const std::size_t triElement =
          firstElement + static_cast<std::size_t>(primitive) * 3u;
      for (std::size_t i = 0; i < 3u; ++i) {
        const auto value = readIndexValue(indexBytes, indexType, triElement + i);
        if (!value.has_value()) {
          chunks.clear();
          return false;
        }
        triMin = std::min(triMin, *value);
        triMax = std::max(triMax, *value);
      }
    }

    bool shouldStartNewChunk = false;
    if (chunkPrimitiveCount != 0u) {
      if (primitiveLimited && chunkPrimitiveCount >= primitiveLimit) {
        shouldStartNewChunk = true;
      }
      if (spanLimited) {
        const u32 nextMin = std::min(chunkMinIndex, triMin);
        const u32 nextMax = std::max(chunkMaxIndex, triMax);
        const u64 nextSpan =
            static_cast<u64>(nextMax - nextMin) * stream0Stride;
        if (nextSpan > stream0SpanLimit) {
          shouldStartNewChunk = true;
        }
      }
    }

    if (shouldStartNewChunk) {
      emitChunk();
      chunkStartPrimitive = primitive;
      chunkPrimitiveCount = 0;
      chunkMinIndex = std::numeric_limits<u32>::max();
      chunkMaxIndex = 0;
    }

    if (chunkPrimitiveCount == 0u) {
      chunkStartPrimitive = primitive;
      if (spanLimited) {
        chunkMinIndex = triMin;
        chunkMaxIndex = triMax;
      }
    } else if (spanLimited) {
      chunkMinIndex = std::min(chunkMinIndex, triMin);
      chunkMaxIndex = std::max(chunkMaxIndex, triMax);
    }
    ++chunkPrimitiveCount;
  }

  emitChunk();
  return chunks.size() > 1u;
}

bool buildReverseTriangleOrderIndexBytes(std::span<const u8> indexBytes,
                                         IndexType indexType,
                                         u32 startIndex,
                                         u64 indexCount,
                                         std::vector<u8>& out) {
  if (indexBytes.empty() || indexCount == 0u || (indexCount % 3u) != 0u) {
    return false;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return false;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return false;
  }

  const std::size_t byteCount = static_cast<std::size_t>(indexCount) * elementSize;
  out.resize(byteCount);
  const std::size_t triangleCount = static_cast<std::size_t>(indexCount / 3u);
  const std::size_t triangleBytes = 3u * elementSize;
  for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
    const std::size_t srcTriangle = triangleCount - 1u - triangle;
    std::memcpy(out.data() + triangle * triangleBytes,
                indexBytes.data() + startByte + srcTriangle * triangleBytes,
                triangleBytes);
  }
  return true;
}

bool buildMinIndexSortedTriangleOrderIndexBytes(std::span<const u8> indexBytes,
                                                IndexType indexType,
                                                u32 startIndex,
                                                u64 indexCount,
                                                std::vector<u8>& out) {
  if (indexBytes.empty() || indexCount == 0u || (indexCount % 3u) != 0u) {
    return false;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return false;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return false;
  }

  struct TriangleKey {
    u32 minIndex = 0;
    u32 maxIndex = 0;
    std::size_t triangle = 0;
  };

  const std::size_t triangleCount = static_cast<std::size_t>(indexCount / 3u);
  std::vector<TriangleKey> keys;
  keys.reserve(triangleCount);
  for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
    const std::size_t triElement =
        static_cast<std::size_t>(startIndex) + triangle * 3u;
    u32 triMin = std::numeric_limits<u32>::max();
    u32 triMax = 0;
    for (std::size_t i = 0; i < 3u; ++i) {
      const auto value = readIndexValue(indexBytes, indexType, triElement + i);
      if (!value.has_value()) {
        return false;
      }
      triMin = std::min(triMin, *value);
      triMax = std::max(triMax, *value);
    }
    keys.push_back(TriangleKey{
        .minIndex = triMin,
        .maxIndex = triMax,
        .triangle = triangle,
    });
  }

  std::stable_sort(keys.begin(), keys.end(), [](const TriangleKey& a,
                                                const TriangleKey& b) {
    if (a.minIndex != b.minIndex) {
      return a.minIndex < b.minIndex;
    }
    if (a.maxIndex != b.maxIndex) {
      return a.maxIndex < b.maxIndex;
    }
    return a.triangle < b.triangle;
  });

  bool changed = false;
  for (std::size_t triangle = 0; triangle < keys.size(); ++triangle) {
    if (keys[triangle].triangle != triangle) {
      changed = true;
      break;
    }
  }
  if (!changed) {
    return false;
  }

  const std::size_t byteCount = static_cast<std::size_t>(indexCount) * elementSize;
  out.resize(byteCount);
  const std::size_t triangleBytes = 3u * elementSize;
  for (std::size_t triangle = 0; triangle < keys.size(); ++triangle) {
    const std::size_t srcTriangle = keys[triangle].triangle;
    std::memcpy(out.data() + triangle * triangleBytes,
                indexBytes.data() + startByte + srcTriangle * triangleBytes,
                triangleBytes);
  }
  return true;
}

void writeIndexValue(std::vector<u8>& out,
                     IndexType indexType,
                     std::size_t elementIndex,
                     u32 value) {
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t offset = elementIndex * elementSize;
  if (indexType == IndexType::UInt16) {
    const u16 v = static_cast<u16>(value);
    std::memcpy(out.data() + offset, &v, sizeof(v));
  } else {
    std::memcpy(out.data() + offset, &value, sizeof(value));
  }
}

// The uncapped vector-rescan selector and its cache warm-up order define the
// accepted LRU candidate bytes; keep this ordering stable.
bool buildVertexCacheOptimizedTriangleOrderIndexBytes(
    std::span<const u8> indexBytes,
    IndexType indexType,
    u32 startIndex,
    u64 indexCount,
    std::vector<u8>& out,
    std::size_t probeCacheSize = 64u) {
  if (indexBytes.empty() || indexCount == 0u || (indexCount % 3u) != 0u ||
      probeCacheSize == 0u) {
    return false;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return false;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return false;
  }

  struct Triangle {
    std::array<u32, 3> indices{};
    u32 minIndex = 0;
    u32 maxIndex = 0;
    std::size_t original = 0;
  };

  const std::size_t triangleCount = static_cast<std::size_t>(indexCount / 3u);
  std::vector<Triangle> triangles;
  triangles.reserve(triangleCount);
  u32 minReferencedIndex = std::numeric_limits<u32>::max();
  u32 maxReferencedIndex = 0u;

  {
    PerfScope scope(perf::countEncodeDrawIndexCacheCandidateReadCpuTime);
    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
      Triangle tri{.original = triangle};
      const std::size_t triElement =
          static_cast<std::size_t>(startIndex) + triangle * 3u;
      tri.minIndex = std::numeric_limits<u32>::max();
      for (std::size_t i = 0; i < 3u; ++i) {
        const auto value = readIndexValue(indexBytes, indexType, triElement + i);
        if (!value.has_value()) {
          return false;
        }
        tri.indices[i] = *value;
        tri.minIndex = std::min(tri.minIndex, *value);
        tri.maxIndex = std::max(tri.maxIndex, *value);
        minReferencedIndex = std::min(minReferencedIndex, *value);
        maxReferencedIndex = std::max(maxReferencedIndex, *value);
      }
      triangles.push_back(tri);
    }
  }
  if (triangles.empty()) {
    return false;
  }

  const u64 referencedRange =
      static_cast<u64>(maxReferencedIndex) - minReferencedIndex + 1u;
  const bool useDenseAdjacency = referencedRange <= 131072u;
  std::vector<std::vector<u32>> denseVertexTriangles;
  std::vector<u32> denseRemainingVertexUse;
  std::unordered_map<u32, std::vector<u32>> sparseVertexTriangles;
  std::unordered_map<u32, u32> sparseRemainingVertexUse;

  {
    PerfScope scope(perf::countEncodeDrawIndexCacheCandidateAdjacencyCpuTime);
    if (useDenseAdjacency) {
      const auto range = static_cast<std::size_t>(referencedRange);
      denseVertexTriangles.resize(range);
      denseRemainingVertexUse.assign(range, 0u);
      for (const auto& tri : triangles) {
        for (const u32 index : tri.indices) {
          ++denseRemainingVertexUse[static_cast<std::size_t>(index - minReferencedIndex)];
        }
      }
      for (std::size_t i = 0; i < range; ++i) {
        if (denseRemainingVertexUse[i] != 0u) {
          denseVertexTriangles[i].reserve(denseRemainingVertexUse[i]);
        }
      }
      for (std::size_t triangle = 0; triangle < triangles.size(); ++triangle) {
        for (const u32 index : triangles[triangle].indices) {
          denseVertexTriangles[static_cast<std::size_t>(index - minReferencedIndex)]
              .push_back(static_cast<u32>(triangle));
        }
      }
    } else {
      sparseVertexTriangles.reserve(triangleCount * 3u);
      sparseRemainingVertexUse.reserve(triangleCount * 3u);
      for (std::size_t triangle = 0; triangle < triangles.size(); ++triangle) {
        for (const u32 index : triangles[triangle].indices) {
          sparseVertexTriangles[index].push_back(static_cast<u32>(triangle));
          ++sparseRemainingVertexUse[index];
        }
      }
    }
  }

  std::vector<u32> cache;
  cache.reserve(probeCacheSize);
  std::vector<u32> candidates;
  candidates.reserve(std::min<std::size_t>(triangleCount, 256u));
  std::vector<u8> emitted(triangleCount, 0u);
  std::vector<u8> inCandidates(triangleCount, 0u);
  std::vector<u32> order;
  order.reserve(triangleCount);
  std::size_t nextOriginal = 0;
  {
    PerfScope scope(perf::countEncodeDrawIndexCacheCandidateSelectCpuTime);
    std::uint64_t selectCalls = 0;
    std::uint64_t selectSlots = 0;
    std::uint64_t selectScored = 0;
    std::uint64_t selectSkipped = 0;
    std::uint64_t selectCandidatesMax = 0;
    constexpr std::size_t kNoCachePosition = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> denseCachePositions;
    std::unordered_map<u32, std::size_t> sparseCachePositions;
    if (useDenseAdjacency) {
      denseCachePositions.assign(static_cast<std::size_t>(referencedRange),
                                 kNoCachePosition);
    } else {
      sparseCachePositions.reserve(std::min<std::size_t>(probeCacheSize, 256u));
    }

    auto clearCachePosition = [&](u32 index) {
      if (useDenseAdjacency) {
        if (index < minReferencedIndex) {
          return;
        }
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local < denseCachePositions.size()) {
          denseCachePositions[local] = kNoCachePosition;
        }
        return;
      }
      sparseCachePositions.erase(index);
    };

    auto setCachePosition = [&](u32 index, std::size_t position) {
      if (useDenseAdjacency) {
        if (index < minReferencedIndex) {
          return;
        }
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local < denseCachePositions.size() &&
            denseCachePositions[local] == kNoCachePosition) {
          denseCachePositions[local] = position;
        }
        return;
      }
      sparseCachePositions.try_emplace(index, position);
    };

    auto clearCachePositions = [&]() {
      for (const u32 index : cache) {
        clearCachePosition(index);
      }
    };

    auto rebuildCachePositions = [&]() {
      for (std::size_t i = 0; i < cache.size(); ++i) {
        setCachePosition(cache[i], i);
      }
    };

    auto cachePosition = [&](u32 index) -> std::optional<std::size_t> {
      if (useDenseAdjacency) {
        if (index < minReferencedIndex) {
          return std::nullopt;
        }
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local < denseCachePositions.size() &&
            denseCachePositions[local] != kNoCachePosition) {
          return denseCachePositions[local];
        }
        return std::nullopt;
      }
      auto it = sparseCachePositions.find(index);
      return it != sparseCachePositions.end() ? std::optional<std::size_t>(it->second)
                                              : std::nullopt;
    };

    auto remainingUseFor = [&](u32 index) -> u32 {
      if (useDenseAdjacency) {
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        return local < denseRemainingVertexUse.size()
                   ? denseRemainingVertexUse[local]
                   : 0u;
      }
      auto it = sparseRemainingVertexUse.find(index);
      return it != sparseRemainingVertexUse.end() ? it->second : 0u;
    };

    auto scoreTriangle = [&](const Triangle& tri) -> std::int64_t {
      ++selectScored;
      u32 cachedVertices = 0;
      u32 cacheDistance = 0;
      u32 remainingUse = 0;
      for (const u32 index : tri.indices) {
        if (const auto pos = cachePosition(index)) {
          ++cachedVertices;
          cacheDistance += static_cast<u32>(*pos);
        }
        remainingUse += remainingUseFor(index);
      }
      return static_cast<std::int64_t>(cachedVertices) * 1'000'000ll -
             static_cast<std::int64_t>(cacheDistance) * 1'000ll -
             static_cast<std::int64_t>(remainingUse) * 10ll -
             static_cast<std::int64_t>(tri.minIndex) / 256ll;
    };

    auto addCandidate = [&](u32 triangle) {
      const std::size_t t = static_cast<std::size_t>(triangle);
      if (t >= triangleCount || emitted[t] || inCandidates[t]) {
        return;
      }
      candidates.push_back(triangle);
      inCandidates[t] = 1u;
    };

    auto addVertexNeighbors = [&](u32 index) {
      if (useDenseAdjacency) {
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local >= denseVertexTriangles.size()) {
          return;
        }
        for (const u32 triangle : denseVertexTriangles[local]) {
          addCandidate(triangle);
        }
        return;
      }
      auto it = sparseVertexTriangles.find(index);
      if (it == sparseVertexTriangles.end()) {
        return;
      }
      for (const u32 triangle : it->second) {
        addCandidate(triangle);
      }
    };

    auto decrementRemainingUse = [&](u32 index) {
      if (useDenseAdjacency) {
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local < denseRemainingVertexUse.size() &&
            denseRemainingVertexUse[local] != 0u) {
          --denseRemainingVertexUse[local];
        }
        return;
      }
      auto it = sparseRemainingVertexUse.find(index);
      if (it != sparseRemainingVertexUse.end() && it->second != 0u) {
        --it->second;
      }
    };

    auto chooseBestCandidate = [&]() -> std::optional<u32> {
      std::optional<std::size_t> bestSlot;
      std::int64_t bestScore = std::numeric_limits<std::int64_t>::min();
      ++selectCalls;
      selectSlots += static_cast<std::uint64_t>(candidates.size());
      selectCandidatesMax = std::max<std::uint64_t>(
          selectCandidatesMax, static_cast<std::uint64_t>(candidates.size()));
      for (std::size_t slot = 0; slot < candidates.size(); ++slot) {
        const u32 candidate = candidates[slot];
        const std::size_t triangleIndex = static_cast<std::size_t>(candidate);
        if (triangleIndex >= triangleCount || emitted[triangleIndex]) {
          ++selectSkipped;
          continue;
        }
        const auto& tri = triangles[triangleIndex];
        const std::int64_t score = scoreTriangle(tri);
        if (score > bestScore ||
            (score == bestScore &&
             (!bestSlot.has_value() ||
              tri.original < triangles[candidates[*bestSlot]].original))) {
          bestScore = score;
          bestSlot = slot;
        }
      }
      if (!bestSlot.has_value()) {
        return std::nullopt;
      }
      const u32 chosen = candidates[*bestSlot];
      inCandidates[chosen] = 0u;
      candidates[*bestSlot] = candidates.back();
      candidates.pop_back();
      return chosen;
    };

    auto chooseNextOriginal = [&]() -> std::optional<u32> {
      while (nextOriginal < triangleCount && emitted[nextOriginal]) {
        ++nextOriginal;
      }
      if (nextOriginal >= triangleCount) {
        return std::nullopt;
      }
      return static_cast<u32>(nextOriginal);
    };

    auto touchCacheVertex = [&](u32 index) {
      if (const auto position = cachePosition(index)) {
        clearCachePositions();
        for (std::size_t j = *position; j > 0u; --j) {
          cache[j] = cache[j - 1u];
        }
        cache[0] = index;
        rebuildCachePositions();
        return;
      }
      clearCachePositions();
      if (cache.size() < probeCacheSize) {
        cache.push_back(index);
      } else {
        for (std::size_t i = cache.size() - 1u; i > 0u; --i) {
          cache[i] = cache[i - 1u];
        }
      }
      cache[0] = index;
      rebuildCachePositions();
    };

    while (order.size() < triangleCount) {
      const std::optional<u32> candidate = chooseBestCandidate();
      const std::optional<u32> fallback =
          candidate.has_value() ? candidate : chooseNextOriginal();
      if (!fallback.has_value()) {
        break;
      }
      const u32 chosen = *fallback;
      const std::size_t triangleIndex = static_cast<std::size_t>(chosen);
      if (triangleIndex >= triangleCount || emitted[triangleIndex]) {
        continue;
      }
      emitted[triangleIndex] = 1u;
      if (inCandidates[triangleIndex]) {
        inCandidates[triangleIndex] = 0u;
      }
      order.push_back(chosen);
      const auto& tri = triangles[triangleIndex];
      for (const u32 index : tri.indices) {
        decrementRemainingUse(index);
        touchCacheVertex(index);
      }
      for (const u32 index : tri.indices) {
        addVertexNeighbors(index);
      }
    }

    perf::countEncodeDrawIndexCacheCandidateSelectVolume(
        selectCalls, selectSlots, selectScored, selectSkipped,
        selectCandidatesMax);
  }

  if (order.size() != triangleCount) {
    return false;
  }
  bool changed = false;
  for (std::size_t triangle = 0; triangle < order.size(); ++triangle) {
    if (order[triangle] != triangle) {
      changed = true;
      break;
    }
  }
  if (!changed) {
    return false;
  }

  const std::size_t byteCount = static_cast<std::size_t>(indexCount) * elementSize;
  {
    PerfScope scope(perf::countEncodeDrawIndexCacheCandidateWriteCpuTime);
    out.resize(byteCount);
    for (std::size_t triangle = 0; triangle < order.size(); ++triangle) {
      const auto& tri = triangles[order[triangle]];
      for (std::size_t i = 0; i < 3u; ++i) {
        writeIndexValue(out, indexType, triangle * 3u + i, tri.indices[i]);
      }
    }
  }
  return true;
}

bool renderEncoderSelectionMatches(const ActiveEncoderBreakdown* encoderBreakdown,
                                   debug::RenderEncoderSelector rowSelector,
                                   const debug::RenderEncoderSelectorList& rowSelectors) {
  if (!rowSelector.enabled && !rowSelectors.enabled) {
    return true;
  }
  if (!encoderBreakdown) {
    return false;
  }
  const auto seqId = encoderBreakdown->stats.seqId;
  const auto encoderIndex = encoderBreakdown->stats.encoderIndex;
  return debug::renderEncoderSelectorMatches(rowSelector, seqId, encoderIndex) ||
         debug::renderEncoderSelectorListMatches(rowSelectors, seqId, encoderIndex);
}

bool reverseIndexedTriangleRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeReverseIndexedTrianglesRow(),
                                       debug::probeReverseIndexedTrianglesRows());
}

bool screenBlendIndexOrderRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::optimizeScreenBlendIndexOrderRow(),
                                       debug::optimizeScreenBlendIndexOrderRows());
}

bool splitLargeIndexedDrawRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::splitLargeIndexedDrawRow(),
                                       debug::splitLargeIndexedDrawRows());
}

bool forceExpandIndexedProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeForceExpandIndexedRow(),
                                       debug::probeForceExpandIndexedRows());
}

bool stageStreamIbProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  if (!debug::probeStageStreamIb()) {
    return false;
  }
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeStageStreamIbRow(),
                                       debug::probeStageStreamIbRows());
}

bool indexedTriangleEncoderDrawRangeMatches(
    const ActiveEncoderBreakdown* encoderBreakdown) {
  const auto range = debug::probeIndexedTriangleEncoderDrawRange();
  const auto excludeList = debug::probeIndexedTriangleEncoderDrawExcludeList();
  if (!debug::drawOrdinalRangeEnabled(range) && !excludeList.enabled) {
    return true;
  }
  if (!encoderBreakdown) {
    return false;
  }
  const auto encoderDrawIndex = encoderBreakdown->stats.drawCalls;
  if (debug::drawOrdinalRangeEnabled(range) &&
      debug::shouldSkipDrawOrdinal(encoderDrawIndex, range)) {
    return false;
  }
  return !debug::drawOrdinalListContains(excludeList, encoderDrawIndex);
}

bool scissorRectProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeScissorRectRow(),
                                       debug::probeScissorRectRows());
}

bool forceCullModeProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeForceCullModeRow(),
                                       debug::probeForceCullModeRows());
}

bool forceTextureWhiteProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeForceTextureWhiteRow(),
                                       debug::probeForceTextureWhiteRows());
}

bool disableAlphaBlendProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeDisableAlphaBlendRow(),
                                       debug::probeDisableAlphaBlendRows());
}

bool disableDepthWriteProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeDisableDepthWriteRow(),
                                       debug::probeDisableDepthWriteRows());
}

bool depthFuncAlwaysProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeDepthFuncAlwaysRow(),
                                       debug::probeDepthFuncAlwaysRows());
}

bool fragmentlessDepthOnlyProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeFragmentlessDepthOnlyRow(),
                                       debug::probeFragmentlessDepthOnlyRows());
}

WMTCullMode toWmtCullMode(debug::CullModeOverride mode,
                          WMTCullMode fallback) noexcept {
  switch (mode) {
    case debug::CullModeOverride::None:
      return WMTCullModeNone;
    case debug::CullModeOverride::Front:
      return WMTCullModeFront;
    case debug::CullModeOverride::Back:
      return WMTCullModeBack;
    case debug::CullModeOverride::Disabled:
      return fallback;
  }
  return fallback;
}

bool indexedTriangleOpaqueDepthWriteClass(
    const core::FlatRenderStateSet& renderStates,
    WMTTriangleFillMode fillMode) {
  if (fillMode != WMTTriangleFillModeFill) {
    return false;
  }
  const bool depthEnabled =
      core::flatStateOr(renderStates, RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
  const auto depthFunc = static_cast<core::CompareFunc>(core::flatStateOr(
      renderStates, RS_Z_FUNC, static_cast<u32>(core::CompareFunc::LessEqual)));
  const bool alphaBlendEnabled =
      core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
  const bool alphaTestEnabled =
      core::flatStateOr(renderStates, RS_ALPHA_TEST_ENABLE, 0u) != 0u;
  const bool stencilEnabled =
      core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u;
  const bool clipPlaneEnabled =
      core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u;
  const bool depthFuncPreservesOpaqueOrder =
      depthFunc == core::CompareFunc::Less ||
      depthFunc == core::CompareFunc::LessEqual;
  return depthWrite && depthFuncPreservesOpaqueOrder &&
         !alphaBlendEnabled && !alphaTestEnabled && !stencilEnabled &&
         !clipPlaneEnabled;
}

bool indexedTriangleBlendEquationMatches(
    const core::FlatRenderStateSet& renderStates,
    core::BlendFactor src,
    core::BlendFactor dst,
    core::BlendOp op) {
  if (core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) == 0u) {
    return false;
  }
  return core::flatStateOr(renderStates,
                           RS_SRC_BLEND,
                           static_cast<u32>(core::BlendFactor::One)) ==
             static_cast<u32>(src) &&
         core::flatStateOr(renderStates,
                           RS_DEST_BLEND,
                           static_cast<u32>(core::BlendFactor::Zero)) ==
             static_cast<u32>(dst) &&
         core::flatStateOr(renderStates,
                           RS_BLEND_OP,
                           static_cast<u32>(core::BlendOp::Add)) ==
             static_cast<u32>(op) &&
         core::flatStateOr(renderStates,
                           RS_SEPARATE_ALPHA_BLEND_ENABLE,
                           0u) == 0u;
}

bool indexedTriangleClassMatches(
    debug::IndexedTriangleClassFilter filter,
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  if (filter == debug::IndexedTriangleClassFilter::Any) {
    return true;
  }

  const bool depthEnabled =
      core::flatStateOr(renderStates, RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
  const bool alphaBlendEnabled =
      core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
  const bool opaqueDepthWrite =
      indexedTriangleOpaqueDepthWriteClass(renderStates, fillMode);

  switch (filter) {
    case debug::IndexedTriangleClassFilter::Any:
      return true;
    case debug::IndexedTriangleClassFilter::OpaqueDepthWrite:
      return opaqueDepthWrite;
    case debug::IndexedTriangleClassFilter::NonOpaque:
      return !opaqueDepthWrite;
    case debug::IndexedTriangleClassFilter::DepthRead:
      return depthEnabled && !depthWrite;
    case debug::IndexedTriangleClassFilter::AlphaBlend:
      return alphaBlendEnabled;
    case debug::IndexedTriangleClassFilter::NoAlphaBlend:
      return !alphaBlendEnabled;
    case debug::IndexedTriangleClassFilter::ScreenBlend:
      return indexedTriangleBlendEquationMatches(renderStates,
                                                 core::BlendFactor::InvDestColor,
                                                 core::BlendFactor::One,
                                                 core::BlendOp::Add);
    case debug::IndexedTriangleClassFilter::StandardAlphaBlend:
      return indexedTriangleBlendEquationMatches(renderStates,
                                                 core::BlendFactor::SrcAlpha,
                                                 core::BlendFactor::InvSrcAlpha,
                                                 core::BlendOp::Add);
    case debug::IndexedTriangleClassFilter::AdditiveAlphaBlend:
      return indexedTriangleBlendEquationMatches(renderStates,
                                                 core::BlendFactor::SrcAlpha,
                                                 core::BlendFactor::One,
                                                 core::BlendOp::Add);
    case debug::IndexedTriangleClassFilter::Scissor:
      return viewport.scissorEnabled;
    case debug::IndexedTriangleClassFilter::NoScissor:
      return !viewport.scissorEnabled;
    case debug::IndexedTriangleClassFilter::Textured:
      return textureMask != 0u;
    case debug::IndexedTriangleClassFilter::Large4096:
      return primitiveCount >= 4096u;
  }
  return true;
}

bool indexedTriangleClassMatches(
    const debug::IndexedTriangleClassFilterList& filters,
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  for (std::size_t i = 0; i < filters.count; ++i) {
    if (!indexedTriangleClassMatches(filters.filters[i],
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode)) {
      return false;
    }
  }
  return true;
}

bool scissorRectProbeClassMatches(u32 primitiveCount,
                                  u32 textureMask,
                                  const core::FlatRenderStateSet& renderStates,
                                  const core::ViewportScissor& viewport,
                                  WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeScissorRectClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeScissorRectClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool forceCullModeProbeClassMatches(u32 primitiveCount,
                                    u32 textureMask,
                                    const core::FlatRenderStateSet& renderStates,
                                    const core::ViewportScissor& viewport,
                                    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeForceCullModeClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeForceCullModeClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool forceTextureWhiteProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeForceTextureWhiteClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeForceTextureWhiteClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool forceExpandIndexedProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeForceExpandIndexedClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeForceExpandIndexedClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool disableAlphaBlendProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeDisableAlphaBlendClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeDisableAlphaBlendClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool disableDepthWriteProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeDisableDepthWriteClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeDisableDepthWriteClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool depthFuncAlwaysProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeDepthFuncAlwaysClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeDepthFuncAlwaysClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool fragmentlessDepthOnlyProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeFragmentlessDepthOnlyClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeFragmentlessDepthOnlyClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool fragmentlessDepthOnlyStateSafe(const core::FlatDrawStateRecord& hot,
                                    WMTTriangleFillMode fillMode) {
  if (fillMode != WMTTriangleFillModeFill || !hot.depthStencil.handle) {
    return false;
  }
  const auto& rs = hot.renderStates;
  const bool depthEnabled = core::flatStateOr(rs, RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && core::flatStateOr(rs, RS_Z_WRITE_ENABLE, 0u) != 0u;
  if (!depthWrite) {
    return false;
  }
  if (core::flatStateOr(rs, RS_ALPHABLEND_ENABLE, 0u) != 0u ||
      core::flatStateOr(rs, RS_ALPHA_TEST_ENABLE, 0u) != 0u ||
      core::flatStateOr(rs, core::RS_STENCIL_ENABLE, 0u) != 0u ||
      core::flatStateOr(rs, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u) {
    return false;
  }
  const u32 adaptiveTessY = core::flatStateOr(rs, core::RS_ADAPTIVETESS_Y, 0u);
  if (adaptiveTessY == core::kFourCcAtoc || adaptiveTessY == core::kFourCcA2M1) {
    return false;
  }

  constexpr std::array<u32, core::kMaxRenderTargets> kColorWriteSlots = {
      core::RS_COLOR_WRITE_ENABLE,
      core::RS_COLOR_WRITE_ENABLE1,
      core::RS_COLOR_WRITE_ENABLE2,
      core::RS_COLOR_WRITE_ENABLE3,
  };
  for (std::size_t i = 0; i < hot.colorAttachments.size(); ++i) {
    if (!hot.colorAttachments[i].handle) {
      continue;
    }
    if (core::flatStateOr(rs, kColorWriteSlots[i], 0xfu) != 0u) {
      return false;
    }
  }
  return true;
}

template <std::size_t MaxEntries>
void overrideFlatStateValue(core::FlatStateSet<MaxEntries>& set,
                            u32 state,
                            u32 value) noexcept {
  auto* first = set.entries.data();
  auto* last = first + (set.count <= MaxEntries ? set.count : MaxEntries);
  auto* hit = std::lower_bound(
      first, last, state,
      [](const core::FlatStateEntry& entry, u32 needle) noexcept {
        return entry.state < needle;
      });
  if (hit != last && hit->state == state) {
    hit->value = value;
    return;
  }
  if (set.count >= MaxEntries) {
    set.overflow = true;
    return;
  }
  auto* insertPos = hit;
  for (auto* it = first + set.count; it != insertPos; --it) {
    *it = *(it - 1);
  }
  *insertPos = core::FlatStateEntry{.state = state, .value = value};
  ++set.count;
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
    case 5u: return WMTSamplerAddressModeMirrorClampToEdge;
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

void appendSamplerTrace(std::ostringstream& out,
                        const core::FlatStateSet<core::kMaxSamplerStates>& states,
                        bool srgbTexture) {
  const auto minFilter = samplerStateOr(states, SAMP_MIN_FILTER, 0u);
  const auto magFilter = samplerStateOr(states, SAMP_MAG_FILTER, 0u);
  const auto mipFilter = samplerStateOr(states, SAMP_MIP_FILTER, 0u);
  const auto addressU = samplerStateOr(states, SAMP_ADDRESS_U, 1u);
  const auto addressV = samplerStateOr(states, SAMP_ADDRESS_V, 1u);
  const auto addressW = samplerStateOr(states, SAMP_ADDRESS_W, 1u);
  const auto borderColor = samplerStateOr(states, SAMP_BORDER_COLOR, 0u);
  const auto maxMipLevel = samplerStateOr(states, SAMP_MAX_MIP_LEVEL, 0u);
  out << " addr=(" << addressU << "," << addressV << "," << addressW << ")"
      << " filter=(" << minFilter << "," << magFilter << "," << mipFilter << ")"
      << " border=0x" << std::hex << borderColor << std::dec
      << " maxMip=" << maxMipLevel
      << " srgbTex=" << (srgbTexture ? 1 : 0);
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
  // R-BACK-12.22..12.26 (resource-array sub-mode): a sampler placed in an
  // argument buffer must be created with supportArgumentBuffers=YES or its
  // gpuResourceID is invalid (GPU page fault on .sample()). Gated on the
  // opt-in lane so the default direct-bind path is byte-identical.
  info.support_argument_buffers = dxmt9::shaders::argbufResourceArrayEnabled();
  DXMT_ASSERT(device && "makeSampler(linear) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

WMTSamplerInfo makeSamplerInfo(const SamplerSnapshot& snapshot, float lodMinClamp) {
  const auto minFilter = samplerStateOr(snapshot, SAMP_MIN_FILTER, 0u);
  const auto magFilter = samplerStateOr(snapshot, SAMP_MAG_FILTER, 0u);
  const auto mipFilter = samplerStateOr(snapshot, SAMP_MIP_FILTER, 0u);
  const auto addressU = samplerStateOr(snapshot, SAMP_ADDRESS_U, 1u);
  const auto addressV = samplerStateOr(snapshot, SAMP_ADDRESS_V, 1u);
  const auto addressW = samplerStateOr(snapshot, SAMP_ADDRESS_W, 1u);
  const auto borderColor = samplerStateOr(snapshot, SAMP_BORDER_COLOR, 0u);
  const auto maxAnisotropy = samplerStateOr(snapshot, SAMP_MAX_ANISOTROPY, 0u);
  const auto maxMipLevel = samplerStateOr(snapshot, SAMP_MAX_MIP_LEVEL, 0u);
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
  // separate bias field. D3DSAMP_MAXMIPLEVEL clamps the sampled mip from
  // above (most-detailed level), and combines with IDirect3DBaseTexture9::
  // SetLOD (already encoded in `lodMinClamp`) as max — picking the coarser
  // (numerically larger) of the two. Wine d3d9 visual.c maxmip_test
  // (gap_d3d9_wine_test §5.1) is the behavioral oracle.
  // Base-level selection follows wined3d/stateblock.c (`mip_base_level`), which
  // the Wine visual.c maxmip_test pins: under MIPFILTER=NONE the base level is
  // SetLOD ALONE -- "with mipmapping disabled, the max mip level is ignored,
  // only level 0 is used" (visual.c:4795) -- and only with a real mip filter is
  // it max(MAXMIPLEVEL, SetLOD). Folding MAXMIPLEVEL in unconditionally makes
  // NONE + MAXMIPLEVEL=N sample level N where D3D9 samples level 0.
  const bool mipFilterNone =
      info.mip_filter == WMTSamplerMipFilterNotMipmapped;
  info.lod_min_clamp = mipFilterNone
                           ? lodMinClamp
                           : std::max(lodMinClamp, static_cast<float>(maxMipLevel));
  info.lod_max_clamp = 1e9f;
  // The other half of the same knob problem: MIPFILTER=NONE means "do not filter
  // BETWEEN mips", SetLOD means "the most-detailed level is N", and
  // MTLSamplerMipFilterNotMipmapped expresses only the first -- Metal ignores
  // lodMinClamp there and always samples level 0. Pin to the single requested
  // level instead; nearest selection with min == max cannot filter between mips,
  // so both semantics hold. See specs/d3d9/gap.md 2026-08-02.
  if (mipFilterNone && info.lod_min_clamp > 0.0f) {
    info.mip_filter = WMTSamplerMipFilterNearest;
    info.lod_max_clamp = info.lod_min_clamp;
  }
  info.max_anisotroy = maxAnisotropy;
  info.normalized_coords = true;
  // R-BACK-12.22..12.26 (resource-array sub-mode): samplers that ride the
  // slot-30 argbuf need supportArgumentBuffers=YES for a valid gpuResourceID
  // (else .sample() page-faults). Gated on the opt-in lane; default
  // direct-bind path is byte-identical.
  info.support_argument_buffers = dxmt9::shaders::argbufResourceArrayEnabled();
  return info;
}

WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const SamplerSnapshot& snapshot) {
  auto info = makeSamplerInfo(snapshot);
  DXMT_ASSERT(device && "makeSampler(snapshot) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const SamplerSnapshot& snapshot,
                                                float lodMinClamp) {
  auto info = makeSamplerInfo(snapshot, lodMinClamp);
  DXMT_ASSERT(device && "makeSampler(snapshot,lod) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

WMTSamplerInfo makeSamplerInfo(const core::FlatStateSet<core::kMaxSamplerStates>& states,
                               float lodMinClamp) {
  const auto minFilter = samplerStateOr(states, SAMP_MIN_FILTER, 0u);
  const auto magFilter = samplerStateOr(states, SAMP_MAG_FILTER, 0u);
  const auto mipFilter = samplerStateOr(states, SAMP_MIP_FILTER, 0u);
  const auto addressU = samplerStateOr(states, SAMP_ADDRESS_U, 1u);
  const auto addressV = samplerStateOr(states, SAMP_ADDRESS_V, 1u);
  const auto addressW = samplerStateOr(states, SAMP_ADDRESS_W, 1u);
  const auto borderColor = samplerStateOr(states, SAMP_BORDER_COLOR, 0u);
  const auto maxAnisotropy = samplerStateOr(states, SAMP_MAX_ANISOTROPY, 0u);
  const auto maxMipLevel = samplerStateOr(states, SAMP_MAX_MIP_LEVEL, 0u);
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
  // See FlatStateSet sibling above: combine D3DSAMP_MAXMIPLEVEL with SetLOD
  // (encoded in `lodMinClamp`) as max — Wine d3d9 visual.c maxmip_test.
  // Base-level selection follows wined3d/stateblock.c (`mip_base_level`), which
  // the Wine visual.c maxmip_test pins: under MIPFILTER=NONE the base level is
  // SetLOD ALONE -- "with mipmapping disabled, the max mip level is ignored,
  // only level 0 is used" (visual.c:4795) -- and only with a real mip filter is
  // it max(MAXMIPLEVEL, SetLOD). Folding MAXMIPLEVEL in unconditionally makes
  // NONE + MAXMIPLEVEL=N sample level N where D3D9 samples level 0.
  const bool mipFilterNone =
      info.mip_filter == WMTSamplerMipFilterNotMipmapped;
  info.lod_min_clamp = mipFilterNone
                           ? lodMinClamp
                           : std::max(lodMinClamp, static_cast<float>(maxMipLevel));
  info.lod_max_clamp = 1e9f;
  // The other half of the same knob problem: MIPFILTER=NONE means "do not filter
  // BETWEEN mips", SetLOD means "the most-detailed level is N", and
  // MTLSamplerMipFilterNotMipmapped expresses only the first -- Metal ignores
  // lodMinClamp there and always samples level 0. Pin to the single requested
  // level instead; nearest selection with min == max cannot filter between mips,
  // so both semantics hold. See specs/d3d9/gap.md 2026-08-02.
  if (mipFilterNone && info.lod_min_clamp > 0.0f) {
    info.mip_filter = WMTSamplerMipFilterNearest;
    info.lod_max_clamp = info.lod_min_clamp;
  }
  info.max_anisotroy = maxAnisotropy;
  info.normalized_coords = true;
  // R-BACK-12.22..12.26 (resource-array sub-mode): samplers that ride the
  // slot-30 argbuf need supportArgumentBuffers=YES for a valid gpuResourceID
  // (else .sample() page-faults). Gated on the opt-in lane; default
  // direct-bind path is byte-identical.
  info.support_argument_buffers = dxmt9::shaders::argbufResourceArrayEnabled();
  return info;
}

WMT::Reference<WMT::SamplerState> makeSampler(
    WMT::Reference<WMT::Device> device,
    const core::FlatStateSet<core::kMaxSamplerStates>& states) {
  auto info = makeSamplerInfo(states);
  DXMT_ASSERT(device && "makeSampler(FlatStateSet) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

WMT::Reference<WMT::SamplerState> makeSampler(
    WMT::Reference<WMT::Device> device,
    const core::FlatStateSet<core::kMaxSamplerStates>& states,
    float lodMinClamp) {
  auto info = makeSamplerInfo(states, lodMinClamp);
  DXMT_ASSERT(device && "makeSampler(FlatStateSet,lod) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

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

bool drawParamBindingOverride(const core::DrawParam& param,
                              std::span<const u8> arena,
                              core::DrawBindingOverride& out) {
  const auto bytes = core::drawRunPayloadBytes(param.bindingOverrideRange, arena);
  if (bytes.size() != sizeof(core::DrawBindingOverride)) {
    return false;
  }
  std::memcpy(&out, bytes.data(), sizeof(out));
  return !core::drawBindingOverrideEmpty(out);
}

bool drawParamBindingSnapshot(const core::DrawParam& param,
                              std::span<const u8> arena,
                              core::DrawBindingSnapshot& out) {
  const auto bytes = core::drawRunPayloadBytes(param.bindingSnapshotRange, arena);
  if (bytes.size() != sizeof(core::DrawBindingSnapshot)) {
    return false;
  }
  std::memcpy(&out, bytes.data(), sizeof(out));
  return !core::drawBindingSnapshotEmpty(out);
}

const core::DrawBufferBindingSnapshot* streamBindingSnapshot(
    const core::DrawBindingSnapshot* binding,
    u32 stream) noexcept {
  if (!binding || stream >= core::kMaxStreams ||
      (binding->streamMask & (1u << stream)) == 0u ||
      !binding->streams[stream].snapshot.valid()) {
    return nullptr;
  }
  return &binding->streams[stream].snapshot;
}

const core::DrawBufferBindingSnapshot* indexBindingSnapshot(
    const core::DrawBindingSnapshot* binding) noexcept {
  if (!binding || !binding->indexSnapshotValid ||
      !binding->indexSnapshot.valid()) {
    return nullptr;
  }
  return &binding->indexSnapshot;
}

std::span<const u8> snapshotBufferBytes(
    const core::DrawBufferBindingSnapshot* snapshot) noexcept {
  if (!snapshot || snapshot->contentsAddress == 0 || snapshot->byteSize == 0) {
    return {};
  }
  return std::span<const u8>(
      reinterpret_cast<const u8*>(
          static_cast<std::uintptr_t>(snapshot->contentsAddress)),
      static_cast<std::size_t>(snapshot->byteSize));
}

void applyDrawBindingOverride(core::FlatDrawStateRecord& hot,
                              core::DrawShaderLayoutContext* shaderLayout,
                              const core::DrawBindingOverride& binding) {
  for (u32 stream = 0; stream < core::kMaxStreams; ++stream) {
    if ((binding.streamMask & (1u << stream)) == 0) {
      continue;
    }
    const bool bufferHandleChanged =
        hot.streamBuffers[stream] != binding.streams[stream].buffer;
    hot.streamBuffers[stream] = binding.streams[stream].buffer;
    hot.streamOffsets[stream] = binding.streams[stream].offset;
    hot.streamStrides[stream] = binding.streams[stream].stride;
    hot.key.streamBuffers[stream] = hot.streamBuffers[stream];
    hot.key.streamOffsets[stream] = hot.streamOffsets[stream];
    hot.key.streamStrides[stream] = hot.streamStrides[stream];
    if (shaderLayout) {
      auto& streamBinding = shaderLayout->vertexDecl.streams[stream];
      if (bufferHandleChanged) {
        streamBinding.buffer.reset();
      }
      streamBinding.offset = binding.streams[stream].offset;
      streamBinding.stride = binding.streams[stream].stride;
    }
  }
  hot.streamMask = 0;
  for (u32 stream = 0; stream < core::kMaxStreams; ++stream) {
    if (hot.streamBuffers[stream]) {
      hot.streamMask |= 1u << stream;
    }
  }
  hot.key.streamMask = hot.streamMask;
  if (binding.indexBufferValid) {
    hot.indexBuffer = binding.indexBuffer;
    hot.key.indexBuffer = binding.indexBuffer;
  }
}

bool drawUsesFixedFunctionPath(core::FlatDrawStateView drawState, bool hasFfpLayout) {
  if (!drawState.hasShaderContext()) {
    return hasFfpLayout;
  }
  return drawState.shaderContext().vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex;
}

bool encodeDraw(EncodeContext& ctx,
                 WMT::CommandBuffer& commandBuffer,
                 WMT::RenderCommandEncoder& encoder,
                 core::FlatDrawStateView drawState,
                 u64 seqId,
                 bool skipBaseStateBind,
                 const PreUploadedDrawData* preUploaded,
                 const core::DrawParam* paramOverride,
                 std::span<const u8> paramPayloadArena,
                 // H228 — parsed per-draw DrawBindingOverride for run/batch
                 // draws (nullptr for canonical draws). encodeDraw reads only
                 // the alphaTest* fields; stream/index rewrites were already
                 // applied by the caller onto drawState's hot record.
                 const core::DrawBindingOverride* paramBindingOverride,
                 const core::DrawBindingSnapshot* bindingSnapshot,
                 uniform::DirtyState* dirty,
                 bool tileFfpMode,
                 bool argbufHybridMode,
                 bool argbufResourceArray,
                 bool argbufDirectCbufMode,
                 bool reopenArgbufHybrid,
                 bool argbufVsPayloadSourceChanged,
                 bool argbufPsPayloadSourceChanged,
                 bool bindingOverridePrefetchedPsoCompatible,
                 core::PsoHandle renderPsoHandle,
                 core::PsoHandle tilePsoHandle,
                 core::DepthStencilHandle depthStencilHandle,
                 TextureSamplerBindShadow* textureSamplerShadow,
                 std::uint32_t commandIndex,
                 u64 commandDrawIndex,
                 u64 commandDrawCount,
                 ActiveEncoderBreakdown* encoderBreakdown,
                 ArgbufCbufCache* argbufCbufCache,
                 StreamIbStagingCache* streamIbStagingCache,
                 VisibilityScoutPass* visibilityScout) {
  // Hot per-draw entry. Per codebase_conventions.rules.md, no heap allocation
  // is permitted on this path; the guard is debug-only and asserts this when
  // DXMT_DEBUG_NO_PER_DRAW_ALLOC=1 is set in env. See dxmt9_debug_alloc_guard.
  DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE("encodeDraw");
  PerfScope scope(perf::countEncodeDrawCpuTime);
  EncodeDrawPhaseTimer drawPhase;
  emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                              commandDrawCount, "enter");
  // M3 — per-draw Instruments interval. os_signpost_id_generate gives each
  // call a unique paired id so overlapping work surfaces correctly in
  // Instruments. No-op when no consumer is recording (~5 ns).
  os_log_t signpostLog = dxmt9::signposts::log();
  os_signpost_id_t drawSignpost = os_signpost_id_generate(signpostLog);
  os_signpost_interval_begin(signpostLog, drawSignpost, "draw",
                             "seq=%llu",
                             static_cast<unsigned long long>(seqId));
  struct DrawSignpostScope {
    os_log_t log;
    os_signpost_id_t id;
    ~DrawSignpostScope() {
      os_signpost_interval_end(log, id, "draw");
    }
  } drawSignpostScope{signpostLog, drawSignpost};
  // M2: per-draw debug group, paired via DebugGroupScope's dtor on
  // every return path (including early-return failures below).
  // primitiveCount may be zero pre-paramOverride; that's OK — captures
  // see whatever is encoded.
  const auto drawDebugPrimCount = paramOverride ? paramOverride->primitiveCount : 0u;
  // Gated on capture -- see perDrawDebugGroupsEnabled(). Nothing reads these at
  // runtime, and they cost an allocation plus three bridge crossings per draw.
  std::optional<DebugGroupScope> drawDebugGroup;
  if (core::metalcapture::perDrawDebugGroupsEnabled() && !suppressRecordedMetalCalls(ctx)) {
    drawDebugGroup.emplace(
        WMT::CommandEncoder{encoder.handle},
        makeLabelStringFmt("Draw[seq=%llu,prim=%u]",
            static_cast<unsigned long long>(seqId), drawDebugPrimCount));
  }
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
                  paramOverride->instanceCount,
                  drawParamVertexBytes(*paramOverride, paramPayloadArena),
                  drawParamIndexBytes(*paramOverride, paramPayloadArena)}
      : ParamView{debug ? debug->primitiveType : core::PrimitiveType::TriangleList,
                  debug ? debug->primitiveCount : 0u,
                  debug ? debug->startVertex : 0u,
                  debug ? debug->baseVertexIndex : 0,
                  debug ? debug->startIndex : 0u,
                  debug ? debug->indexType : IndexType::UInt16,
                  false,
                  1u,
                  {},
                  {}};
  const bool traceEncode = debug::shouldTraceEncode(hot, seqId) ||
                           colorAttachmentAliasesTracedTexture(ctx.pool, hot);
  const bool effectiveArgbufDirectCbufMode =
      argbufHybridMode && !argbufResourceArray && argbufDirectCbufMode;
  const bool argbufTableMode =
      argbufHybridMode && !effectiveArgbufDirectCbufMode;
  const bool directCbufBindings = !argbufTableMode;
  if (debug::skipAllDraws()) {
    if (queueTraceEnabled() || traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-debug] skip all draws seq=" << static_cast<unsigned long long>(seqId)
          << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value);
      emitQueueTraceLine(out.str());
    }
    return false;
  }
  if (debug::shouldSkipDrawSeq(seqId)) {
    if (queueTraceEnabled() || traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-debug] skip draw seq=" << static_cast<unsigned long long>(seqId)
          << " reason=seq-range"
          << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value);
      emitQueueTraceLine(out.str());
    }
    return false;
  }
  const u64 drawOrdinal = debug::nextDrawOrdinal();
  traceRenderTargetWriteForTexture(ctx.pool, hot, seqId, drawOrdinal);
  if (debug::shouldSkipDrawOrdinal(drawOrdinal)) {
    if (queueTraceEnabled() || traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-debug] skip draw seq=" << static_cast<unsigned long long>(seqId)
          << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
          << " reason=ordinal-range"
          << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value);
      emitQueueTraceLine(out.str());
    }
    return false;
  }
  if (!encoder) {
    if (traceEncode) {
      emitQueueTraceLine("[dxmt9-encode] seq=" + std::to_string(seqId) +
                         " ordinal=" + std::to_string(drawOrdinal) +
                         " skipped reason=no-encoder");
    }
    return false;
  }
  const bool depthStateProbeRequested =
      debug::probeDisableDepthWrite() || debug::probeDepthFuncAlways();
  std::optional<dxmt9::ffp::FixedFunctionVertexLayout> ffLayout;
  bool fixedFunctionPath = false;
  {
    PerfScope fvfDecodeScope(perf::countEncodeDrawFvfDecodeCpuTime,
                            perf::countEncodeDrawFvfDecodeDeclCpuTime);
    ffLayout = decodeFixedFunctionVertexLayout(vertexDecl);
    fixedFunctionPath = drawUsesFixedFunctionPath(drawState, static_cast<bool>(ffLayout));
  }
  emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                              commandDrawCount, "after-fvf-decode");
  const u32 primitiveCount = std::max<u32>(1, pv.primitiveCount);
  const uint64_t vertexCount =
      static_cast<uint64_t>(std::max(1u, primitiveVertexCount(pv.primitiveType, primitiveCount)));
  const bool indexedDraw = pv.indexed && (hot.indexBuffer || !pv.userIndexData.empty());
  const auto primitiveType = toPrimitiveType(pv.primitiveType);
  const bool preTransformed = ffLayout && ffLayout->preTransformed;
  const auto fillMode = triangleFillModeFromRenderState(hot.renderStates);
  const bool indexedTriangleDraw =
      indexedDraw && pv.primitiveType == core::PrimitiveType::TriangleList;
  const bool disableDepthWriteProbeApplied =
      debug::probeDisableDepthWrite() &&
      indexedTriangleDraw &&
      disableDepthWriteProbeRowMatches(encoderBreakdown) &&
      disableDepthWriteProbeClassMatches(primitiveCount,
                                         hot.textureMask,
                                         hot.renderStates,
                                         hot.viewport,
                                         fillMode);
  const bool disableAlphaBlendProbeApplied =
      debug::probeDisableAlphaBlend() &&
      indexedTriangleDraw &&
      disableAlphaBlendProbeRowMatches(encoderBreakdown) &&
      indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
      disableAlphaBlendProbeTextureMatches(drawState, ctx.pool) &&
      disableAlphaBlendProbeClassMatches(primitiveCount,
                                         hot.textureMask,
                                         hot.renderStates,
                                         hot.viewport,
                                         fillMode);
  const bool depthFuncAlwaysProbeApplied =
      debug::probeDepthFuncAlways() &&
      indexedTriangleDraw &&
      depthFuncAlwaysProbeRowMatches(encoderBreakdown) &&
      indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
      depthFuncAlwaysProbeTextureMatches(drawState, ctx.pool) &&
      depthFuncAlwaysProbeClassMatches(primitiveCount,
                                       hot.textureMask,
                                       hot.renderStates,
                                       hot.viewport,
                                       fillMode);
  const bool forceTextureWhiteProbeApplied =
      debug::probeForceTextureWhite() &&
      indexedTriangleDraw &&
      forceTextureWhiteProbeRowMatches(encoderBreakdown) &&
      indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
      forceTextureWhiteProbeTextureMatches(drawState, ctx.pool) &&
      forceTextureWhiteProbeDrawOrdinalMatches(drawOrdinal) &&
      forceTextureWhiteProbeCommandIndexMatches(commandIndex) &&
      forceTextureWhiteProbeCommandDrawIndexMatches(commandDrawIndex) &&
      forceTextureWhiteProbeClassMatches(primitiveCount,
                                         hot.textureMask,
                                         hot.renderStates,
                                         hot.viewport,
                                         fillMode);
  const bool fragmentlessDepthOnlyProbeApplied =
      debug::probeFragmentlessDepthOnly() &&
      indexedTriangleDraw &&
      fragmentlessDepthOnlyProbeRowMatches(encoderBreakdown) &&
      fragmentlessDepthOnlyProbeClassMatches(primitiveCount,
                                             hot.textureMask,
                                             hot.renderStates,
                                             hot.viewport,
                                             fillMode) &&
      fragmentlessDepthOnlyStateSafe(hot, fillMode);
  const std::optional<bool> forceTextureWhiteOverride =
      forceTextureWhiteProbeApplied ? std::optional<bool>{true} : std::nullopt;
  const bool hasPerDrawBindingOverride =
      paramOverride && !paramOverride->bindingOverrideRange.empty();
  const bool psoPrefetchBypassProbe =
      disableAlphaBlendProbeApplied || forceTextureWhiteProbeApplied ||
      fragmentlessDepthOnlyProbeApplied;
  const bool bypassPrefetchedPsoHandle =
      psoPrefetchBypassProbe ||
      (hasPerDrawBindingOverride && !bindingOverridePrefetchedPsoCompatible);
  const bool effectiveSkipBaseStateBind =
      skipBaseStateBind && !depthStateProbeRequested && !forceTextureWhiteProbeApplied;
  if (!effectiveSkipBaseStateBind && !suppressBaseStateLookup(ctx)) {
    const bool psoPrefetchHandleAvailable = renderPsoHandle.valid();
    perf::countEncodeDrawPsoPrefetch(
        psoPrefetchHandleAvailable,
        psoPrefetchHandleAvailable && !bypassPrefetchedPsoHandle,
        hasPerDrawBindingOverride,
        bindingOverridePrefetchedPsoCompatible,
        psoPrefetchBypassProbe);
  }
  if (effectiveSkipBaseStateBind) {
    recordPsoAttributionForDraw(encoderBreakdown, drawState, ctx.pool, renderPsoHandle,
                                tileFfpMode, argbufHybridMode,
                                argbufResourceArray, argbufDirectCbufMode,
                                std::nullopt,
                                fragmentlessDepthOnlyProbeApplied);
  }
  if (encoderBreakdown) {
    if (disableAlphaBlendProbeApplied) {
      ++encoderBreakdown->stats.probeDisableAlphaBlendDraws;
    }
    if (disableDepthWriteProbeApplied) {
      ++encoderBreakdown->stats.probeDisableDepthWriteDraws;
    }
    if (depthFuncAlwaysProbeApplied) {
      ++encoderBreakdown->stats.probeDepthFuncAlwaysDraws;
    }
    if (forceTextureWhiteProbeApplied) {
      ++encoderBreakdown->stats.probeForceTextureWhiteDraws;
    }
    if (fragmentlessDepthOnlyProbeApplied) {
      ++encoderBreakdown->stats.probeFragmentlessDepthOnlyDraws;
      encoderBreakdown->stats.probeFragmentlessDepthOnlyPrimitives += primitiveCount;
      encoderBreakdown->stats.probeFragmentlessDepthOnlyVertices += vertexCount;
    }
  }
  core::FlatDrawStateRecord alphaBlendProbeHot{};
  core::FlatDrawStateView pipelineDrawState = drawState;
  if (disableAlphaBlendProbeApplied) {
    alphaBlendProbeHot = hot;
    overrideFlatStateValue(alphaBlendProbeHot.renderStates,
                           RS_ALPHABLEND_ENABLE,
                           0u);
    pipelineDrawState.hot = &alphaBlendProbeHot;
  }
  // Phase 3-E: pipeline lookup + depth state + setRenderPipelineState
  // are BaseDrawState-only and survive across iterations of a
  // Kind::DrawRun on the Metal render encoder. Skip on iter 2..N.
  if (!effectiveSkipBaseStateBind) {
    emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                commandDrawCount, "before-pipeline-lookup");
    PerfScope pipelineLookupScope(perf::countEncodeDrawPipelineLookupCpuTime);
    auto depthKey = makeDepthStencilKey(drawState);
    if (disableDepthWriteProbeApplied) {
      depthKey.depthWrite = false;
    }
    if (depthFuncAlwaysProbeApplied) {
      depthKey.depthFunc = static_cast<u32>(core::CompareFunc::Always);
    }
    const std::uint8_t stencilRef = state::computeStencilRef(drawState);
    emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                commandDrawCount, "after-pipeline-key");
    // R-BACK-13.3: pass `tileFfpMode` through so the cache returns the
    // tile-stage MTLRenderPipelineState (built via
    // newRenderPipelineStateWithTileDescriptor) when the selector chose
    // Tile, and the standard fragment PSO otherwise. The variant key
    // already records this bit, so the two variants land in distinct
    // cache entries.
    // R-BACK-12.22..12.26: pass `argbufHybridMode` through as a real
    // PSO/source variant. Stage 1 uses direct slot 0/3 bindings; Stage 2
    // emits the slot-30 ArgbufLayout prelude and reads cbuf/texture/sampler
    // state through the argument buffer.
    WMT::Reference<WMT::RenderPipelineState> pipelineRef;
    WMT::RenderPipelineState pipeline{};
    const pipeline::HandleLookupContext renderPsoLookup{
        .chunkSeqId = seqId,
        .commandIndex = commandIndex,
        .role = tileFfpMode ? "tile-base-render-pso" : "render-pso",
    };
    if (suppressBaseStateLookup(ctx)) {
      emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                  commandDrawCount, "before-pipeline-recorder");
      pipeline = ctx.drawRecorder->renderPipelineState;
      emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                  commandDrawCount, "after-pipeline-recorder");
    } else if (tileFfpMode) {
      // R-BACK-13.1 two-stage tile-FFP encode: the render command encoder
      // first rasterizes the geometry with the BASE-COLOUR fragment PSO
      // (fog / alpha-test / A2C stripped), and only afterwards runs the tile
      // kernel over the imageblock. So in tile mode the PSO bound here via
      // setRenderPipelineState is the base-colour render PSO, NOT the tile
      // PSO. The tile PSO is fetched + dispatched after drawPrimitives below.
      if (renderPsoHandle.valid() && !bypassPrefetchedPsoHandle) {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "before-pipeline-handle-tile-base");
        pipelineRef =
            ctx.cache.drawPipelineForHandle(renderPsoHandle,
                                            renderPsoLookup).get();
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "after-pipeline-handle-tile-base");
      } else {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "before-pipeline-build-tile-base");
        pipelineRef =
            ctx.cache.getOrBuildTileFfpBaseColorPipelineForState(
                ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                ctx.shaderArchive, ctx.shaderArchivePath,
                forceTextureWhiteOverride).get();
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "after-pipeline-build-tile-base");
      }
      pipeline = WMT::RenderPipelineState{pipelineRef.handle};
    } else {
      if (renderPsoHandle.valid() && !bypassPrefetchedPsoHandle) {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "before-pipeline-handle-render");
        pipelineRef =
            ctx.cache.drawPipelineForHandle(renderPsoHandle,
                                            renderPsoLookup).get();
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "after-pipeline-handle-render");
      } else {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "before-pipeline-build-render");
        pipelineRef =
            ctx.cache.getOrBuildDrawPipelineForState(
                ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                ctx.shaderArchive, ctx.shaderArchivePath, tileFfpMode,
                argbufHybridMode, argbufResourceArray,
                effectiveArgbufDirectCbufMode,
                disableAlphaBlendProbeApplied,
                forceTextureWhiteOverride,
                fragmentlessDepthOnlyProbeApplied).get();
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "after-pipeline-build-render");
      }
      pipeline = WMT::RenderPipelineState{pipelineRef.handle};
    }
    emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                commandDrawCount, "after-pipeline-lookup");
    if (!pipeline) {
      perf::countDrawSkippedNoPipeline();
      static std::mutex logMutex;
      static std::unordered_set<std::uint64_t> loggedNoPipelineKeys;
      const std::uint64_t noPipelineKey =
          hot.key.vertexShaderHash ^ (hot.key.pixelShaderHash << 1u) ^
          (static_cast<std::uint64_t>(tileFfpMode) << 2u) ^
          (static_cast<std::uint64_t>(argbufHybridMode) << 3u) ^
          (static_cast<std::uint64_t>(argbufResourceArray) << 4u) ^
          (static_cast<std::uint64_t>(effectiveArgbufDirectCbufMode) << 5u);
      bool shouldLog = false;
      {
        std::lock_guard lock(logMutex);
        shouldLog = loggedNoPipelineKeys.size() < 64u &&
                    loggedNoPipelineKeys.insert(noPipelineKey).second;
      }
      if (shouldLog) {
        util::logf(util::LogLevel::Error, "dxmt9-encode",
                   "draw skipped: no render pipeline seq=%llu command=%u vs=0x%llx ps=0x%llx tile=%u argbuf=%u resource_array=%u argbuf_direct_cbuf=%u rt0=0x%llx ds=0x%llx",
                   static_cast<unsigned long long>(seqId),
                   commandIndex,
                   static_cast<unsigned long long>(hot.key.vertexShaderHash),
                   static_cast<unsigned long long>(hot.key.pixelShaderHash),
                   tileFfpMode ? 1u : 0u,
                   argbufHybridMode ? 1u : 0u,
                   argbufResourceArray ? 1u : 0u,
                   effectiveArgbufDirectCbufMode ? 1u : 0u,
                   static_cast<unsigned long long>(hot.colorAttachments[0].handle.value),
                   static_cast<unsigned long long>(hot.depthStencil.handle.value));
      }
      if (traceEncode) {
        std::ostringstream out;
        out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
            << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
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
    WMT::Reference<WMT::DepthStencilState> depthStateRef;
    WMT::DepthStencilState depthState{};
    if (suppressBaseStateLookup(ctx)) {
      depthState = ctx.drawRecorder->depthStencilState;
    } else {
      DXMT_ASSERT(ctx.device && "depthStencilStateFor called with stale/null Metal device handle");
      const pipeline::HandleLookupContext depthLookup{
          .chunkSeqId = seqId,
          .commandIndex = commandIndex,
          .role = "depth-stencil",
      };
      depthStateRef =
          depthStencilHandle.valid()
              ? ctx.cache.depthStencilStateForHandle(depthStencilHandle,
                                                     depthLookup)
              : WMT::Reference<WMT::DepthStencilState>{};
      if (!depthStateRef) {
        depthStateRef = ctx.cache.depthStencilStateFor(ctx.device, depthKey);
      }
      depthState = WMT::DepthStencilState{depthStateRef.handle};
    }
    if (depthState) {
      // P0-3: propagate D3DRS_STENCILREF through to Metal. D3D9 has only
      // one stencil ref slot (Wine `wined3d_device_apply_stencil_ref`),
      // so the same byte applies to front and back faces — WMT's
      // `setStencilReferenceValue` mirrors that.
      const bool depthUnchanged =
          textureSamplerShadow &&
          textureSamplerShadowMatches(textureSamplerShadow->depthStencil,
                                      stencilRef, depthState.handle);
      if (!depthUnchanged) {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount, "before-set-depth-state");
        recordedSetDepthStencilState(ctx, encoder, depthState, stencilRef);
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount, "after-set-depth-state");
        if (textureSamplerShadow) {
          textureSamplerShadowStore(textureSamplerShadow->depthStencil,
                                    stencilRef, depthState.handle);
        }
        countDepthStateBind();
      } else {
        countDepthStateBindSkipped();
      }
    }
    if (!disableAlphaBlendProbeApplied &&
        blendFactorNeedsConstantColor(hot.renderStates)) {
      const auto factor = decodeD3DBlendFactor(
          core::flatStateOr(hot.renderStates, RS_BLEND_FACTOR, 0xffffffffu));
      recordedSetBlendColorAndStencilRef(
          ctx, encoder, factor[0], factor[1], factor[2], factor[3], stencilRef);
    }
    // M1: label the pipeline with the shader-variant hash so frame
    // captures show "pso_h<hash>" instead of an anonymous pipeline. When
    // encoder breakdown is active, also retain the pair-local VSOut layout
    // key in both the log and label so Xcode's VS buffer-write counters can
    // be tied back to a concrete stage-in shape.
    {
      const auto variantHash =
          shaderVariantHashForDraw(drawState, &ctx.pool,
                                   fragmentlessDepthOnlyProbeApplied);
      const bool recordPsoBreakdown = encoderBreakdown && encoderBreakdown->enabled;
      u32 vsOutLayoutKey = 0;
      if (recordPsoBreakdown) {
        recordPsoAttributionForDraw(encoderBreakdown, drawState, ctx.pool, renderPsoHandle,
                                    tileFfpMode, argbufHybridMode,
                                    argbufResourceArray,
                                    argbufDirectCbufMode,
                                    forceTextureWhiteOverride,
                                    fragmentlessDepthOnlyProbeApplied);
        vsOutLayoutKey = encoderBreakdown->stats.vsOutLayoutLast;
      }
      if (variantHash != 0 && !suppressRecordedMetalCalls(ctx)) {
        WMT::RenderPipelineState psoView{pipeline.handle};
        if (recordPsoBreakdown) {
          psoView.setLabel(makeLabelStringFmt("pso_h%016llx_vso0x%03x",
              static_cast<unsigned long long>(variantHash),
              static_cast<unsigned>(vsOutLayoutKey)));
        } else {
          psoView.setLabel(makeLabelStringFmt("pso_h%016llx",
              static_cast<unsigned long long>(variantHash)));
        }
      }
    }
    // R-BACK-13.1: in BOTH the portable and the tile-FFP base-colour cases
    // the PSO bound here is an ordinary render pipeline, so the geometry
    // draw below rasterizes into the imageblock. The tile-FFP imageblock
    // post-pass (setTileRenderPipelineState + dispatchThreadsPerTile) is
    // deferred until AFTER drawPrimitives — see emitTileFfpPostPass below.
    const bool pipelineUnchanged =
        textureSamplerShadow &&
        bindShadowMatches(textureSamplerShadow->renderPipeline, pipeline.handle);
    if (!pipelineUnchanged) {
      emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                  commandDrawCount, "before-set-pipeline");
      recordedSetRenderPipelineState(ctx, encoder, pipeline);
      emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                  commandDrawCount, "after-set-pipeline");
      if (textureSamplerShadow) {
        bindShadowStore(textureSamplerShadow->renderPipeline, pipeline.handle);
      }
      countPipelineBind();
    } else {
      countPipelineBindSkipped();
    }
  }
  auto uploadTransientBuffer = [&](const void* data, std::size_t len, std::size_t alignment) {
    return ctx.queue.uploadTransientBufferWithCompletedSeqId(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len),
        alignment, seqId, ctx.transientCompletedSeqId);
  };
  auto setVertexBufferCached = [&](WMT::Buffer buffer, u64 offset, std::uint8_t index) {
    if (textureSamplerShadow && index < textureSamplerShadow->vertexBuffers.size() &&
        bufferBindShadowMatches(textureSamplerShadow->vertexBuffers[index],
                                buffer.handle, offset)) {
      // Cache hit: the shadow already records this (buffer, offset) at
      // this binding slot — skip the Metal call and count the saved
      // bind. Mirrors the texture/sampler skip pattern.
      countVertexBufferBindSkipped();
      return false;
    }
    recordedSetVertexBuffer(ctx, encoder, buffer, offset, index);
    if (textureSamplerShadow && index < textureSamplerShadow->vertexBuffers.size()) {
      bufferBindShadowStore(textureSamplerShadow->vertexBuffers[index],
                            buffer.handle, offset);
    }
    return true;
  };
  // R-BACK-13.1 two-stage tile-FFP encode — tile post-pass.
  //
  // After the base-colour geometry draw rasterizes into the imageblock, a
  // tile kernel (makeFfpTilePixelSource) applies the D3D9 fog / alpha-test /
  // A2C over the imageblock value. The kernel reads `FfpPsConsts` at tile
  // buffer slot 3, so the tile PSO is built in its NON-argbuf form
  // (argbufHybridMode=false) and the FfpPsConsts struct is bound to the TILE
  // stage via setTileBuffer (the render-stage fragment-buffer binding does
  // not feed the tile stage).
  //
  // Metal API note (validated on Apple M1 / AGXG13GFamilyRenderContext): a
  // tile render-pipeline state — even though it is built via
  // newRenderPipelineStateWithTileDescriptor: — is bound with the ordinary
  // `setRenderPipelineState:`, NOT `setTileRenderPipelineState:` (which the
  // M1 render encoder does not respond to and throws an unrecognized-selector
  // NSException for). This matches spec.md §13.5. Because that overwrites
  // the render PSO, we rebind the base-colour PSO after the dispatch so any
  // subsequent draw in a DrawRun (which skips the base-state bind) still has
  // its base-colour pipeline current.
  //
  // We fetch both PSOs on every tile-mode draw — the cache lookup is a hit
  // after the first build — so each draw in a DrawRun gets its own post-pass.
  drawPhase.mark(perf::countEncodeDrawPhaseSetupCpuTime);
  WMT::Reference<WMT::RenderPipelineState> tileFfpPsoRef;
  WMT::RenderPipelineState tileFfpPso{};
  WMT::Reference<WMT::RenderPipelineState> tileFfpBasePsoRef;
  WMT::RenderPipelineState tileFfpBasePso{};
  if (tileFfpMode && !suppressBaseStateLookup(ctx)) {
    const pipeline::HandleLookupContext tileLookup{
        .chunkSeqId = seqId,
        .commandIndex = commandIndex,
        .role = "tile-pso",
    };
    const pipeline::HandleLookupContext tileBaseLookup{
        .chunkSeqId = seqId,
        .commandIndex = commandIndex,
        .role = "tile-base-render-pso",
    };
    tileFfpPsoRef =
        tilePsoHandle.valid() && !bypassPrefetchedPsoHandle
            ? ctx.cache.drawPipelineForHandle(tilePsoHandle,
                                              tileLookup).get()
            : ctx.cache.getOrBuildDrawPipelineForState(
                  ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                  ctx.shaderArchive, ctx.shaderArchivePath,
                  /*tileFfpMode=*/true, /*argbufHybridMode=*/false,
                  /*argbufResourceArray=*/false,
                  /*argbufDirectCbufMode=*/false,
                  disableAlphaBlendProbeApplied,
                  forceTextureWhiteOverride).get();
    tileFfpPso = WMT::RenderPipelineState{tileFfpPsoRef.handle};
    tileFfpBasePsoRef =
        renderPsoHandle.valid() && !bypassPrefetchedPsoHandle
            ? ctx.cache.drawPipelineForHandle(renderPsoHandle,
                                              tileBaseLookup).get()
            : ctx.cache.getOrBuildTileFfpBaseColorPipelineForState(
                  ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                  ctx.shaderArchive, ctx.shaderArchivePath,
                  forceTextureWhiteOverride).get();
    tileFfpBasePso = WMT::RenderPipelineState{tileFfpBasePsoRef.handle};
  }
  // Called immediately after a successful geometry draw (every `return true`
  // path below) when this draw runs the tile-FFP path. No-op otherwise.
  auto emitTileFfpPostPass = [&]() {
    if (!tileFfpMode || !tileFfpPso || suppressRecordedMetalCalls(ctx)) {
      return;
    }
    // Bind FfpPsConsts to the tile-stage buffer table (slot 3). The tile
    // kernel reads alphaRef / alphaTestFunc / fogStart / fogEnd / fogColor
    // / fogMode from here; without this the kernel would read zeroes and
    // leave the base colour unfogged.
    FfpPsConsts ffpPs = buildFfpPsConsts(drawState);
    auto slice = uploadTransientBuffer(&ffpPs, sizeof(FfpPsConsts), alignof(FfpPsConsts));
    if (slice) {
      encoder.setTileBuffer(slice.buffer, slice.offset, 3);
    }
    // R-BACK-13.5: bind the tile-stage variant via setRenderPipelineState
    // (see the API note above) and dispatch one tile thread per imageblock
    // lane. tileWidth/tileHeight come from the bound attachment shape; fall
    // back to 16x16 (typical Apple GPU tile) when Metal reports 0.
    encoder.setRenderPipelineState(tileFfpPso);
    uint64_t tileW = encoder.tileWidth();
    uint64_t tileH = encoder.tileHeight();
    if (tileW == 0u || tileH == 0u) {
      tileW = 16u;
      tileH = 16u;
    }
    encoder.dispatchThreadsPerTile(WMTSize{tileW, tileH, 1u});
    // Restore the base-colour render PSO so a subsequent DrawRun draw (which
    // skips the base-state bind) rasterizes with the correct pipeline.
    if (tileFfpBasePso) {
      encoder.setRenderPipelineState(tileFfpBasePso);
    }
  };
  // Per-frequency UBO bind sequence (R-BACK-12.5/12.8). Each category
  // sub-allocates from the existing transient slab pool, builds its
  // struct via the A2 transform, binds to its slot, and clears the
  // dirty bit. Stale (non-dirty) categories rely on the previous
  // draw's sticky binding on the same Metal render encoder.
  uniform::DirtyState scratchDirty;
  uniform::markAllDirty(scratchDirty);
  uniform::DirtyState* dirtyPtr = dirty ? dirty : &scratchDirty;
  const auto& shaderUsage = drawState.shaderContext();
  // R-BACK-12.22..12.26 (resource-array sub-mode) — when this pass runs the
  // resource-array lane the constant-buffer entries AND the texture/sampler
  // arrays share the SAME argument buffer, so every argbuf write
  // (updateDirtyArgbufRegions / pointFfpVsAtSlice / populateResourceBindings)
  // must target the resource-array encoder. Otherwise (constants-only Stage 2)
  // the constants-only encoder is used, byte-identical to before. Resolved
  // once here so the constant + resource write paths can't diverge.
  const bool useResourceArrayArgbuf =
      argbufResourceArray && argbufTableMode &&
      ctx.queue.resourceArrayEncoderResource().initialized();
  auto& argbufEncoderForDraw = useResourceArrayArgbuf
                                   ? ctx.queue.resourceArrayEncoderResource()
                                   : ctx.queue.argbufEncoderResource();
  const auto argbufConsumedBits = static_cast<std::uint16_t>(
      uniform::kVsAny | uniform::kPsAny | uniform::kFfpVsAny | uniform::kFfpPsAny);
  const u64 argbufPayloadHash =
      drawState.hasUniformPayload() ? drawState.uniformPayload().hash : 0;
  // R-BACK-12.22..12.26 (argbuf lifetime across draws). Reserve a FRESH
  // argbuf per draw and rebind slot 30 so each draw's argbuf is
  // self-contained. This applies to BOTH Stage 2 lanes:
  //
  //   * resource-array lane: texture/sampler arrays write the gpuResourceID
  //     INLINE into the argbuf slot, so a second draw that changes a texture
  //     would overwrite the first draw's slot before the GPU consumed it.
  //
  //   * constants-only lane: the cbuf DATA goes to a fresh transient slab per
  //     dirty draw (uploadTransientBuffer), but the argbuf descriptor table
  //     itself is anchored once per openArgbuf and re-pointed IN PLACE by
  //     updateDirtyArgbufRegions. The GPU reads the descriptor table at
  //     execution time, so multiple draws in one render pass that share a
  //     single descriptor table all observe the LAST pointer written
  //     (last-write-wins on constants — dxut-simple overlay PassMix bug).
  //     Re-opening per draw gives each draw its own descriptor table.
  //
  // A fresh table still needs all four cbuf entries. When an encoder-local
  // cache has slices for the same uniform payload and no pending cbuf dirty
  // bits, point the fresh table at those slices. Otherwise force all cbuf
  // categories dirty so the mirror below repopulates the table.
  //
  // `reopenArgbufHybrid` is the caller's optimisation gate (encodeChunk):
  // the resource-array lane always reopens (texture/sampler inline writes),
  // while the constants-only lane reopens only when this draw's uniform
  // payload differs from the previous draw on the same encoder. When false
  // we leave slot 30 bound to the prior draw's table — correct because its
  // pointers still describe the unchanged constants — and the dirty mirror
  // below is a no-op (the prior draw already consumed the const dirty bits).
  const bool argbufCbufProbeSplit =
      argbufTableMode && argbufCbufProbeSplitPerfEnabled();
  const bool argbufReopenSplit =
      argbufTableMode && argbufReopenSplitPerfEnabled();
  const bool argbufCbufDirtyIdentityProbe =
      argbufTableMode && argbufCbufDirtyIdentityPerfEnabled();
  const bool encoderBreakdownCbufContent =
      encoderBreakdown && encoderBreakdown->enabled &&
      encoderBreakdownCbufContentEnabled();
  if (argbufTableMode && reopenArgbufHybrid) {
    PerfScope argbufSetupScope(perf::countEncodeDrawArgbufSetupCpuTime);
    PerfScope argbufOpenScope(perf::countEncodeDrawArgbufOpenCpuTime);
    const auto populated = [&]() {
      PerfScope argbufOpenCallScope(
          perf::countEncodeDrawArgbufOpenCallCpuTime);
      return dxmt9::argbuf_hybrid::openArgbufWithCompletedSeqId(
          ctx.queue, argbufEncoderForDraw, seqId,
          ctx.transientCompletedSeqId);
    }();
    if (populated && !suppressRecordedMetalCalls(ctx)) {
      PerfScope argbufReopenPostScope(
          perf::countEncodeDrawArgbufReopenPostCpuTime);
      bool tableUnchanged = false;
      {
        PerfScope tableProbeScope(
            argbufReopenSplit
                ? perf::countEncodeDrawArgbufReopenTableProbeCpuTime
                : nullptr);
        tableUnchanged =
            textureSamplerShadow &&
            dxmt9::shaders::kArgbufHybridBindSlot <
                textureSamplerShadow->vertexBuffers.size() &&
            bufferBindShadowMatches(
                textureSamplerShadow
                    ->vertexBuffers[dxmt9::shaders::kArgbufHybridBindSlot],
                populated.storage.handle, populated.offset);
      }
      if (!tableUnchanged) {
        PerfScope tableBindScope(perf::countEncodeDrawArgbufTableBindCpuTime);
        encoder.setVertexBuffer(populated.storage, populated.offset,
                                dxmt9::shaders::kArgbufHybridBindSlot);
        encoder.setFragmentBuffer(populated.storage, populated.offset,
                                  dxmt9::shaders::kArgbufHybridBindSlot);
        perf::countEncodeDrawArgbufTableBindCalls(1u);
        if (textureSamplerShadow) {
          PerfScope tableShadowStoreScope(
              argbufReopenSplit
                  ? perf::countEncodeDrawArgbufReopenTableShadowStoreCpuTime
                  : nullptr);
          if (dxmt9::shaders::kArgbufHybridBindSlot <
              textureSamplerShadow->vertexBuffers.size()) {
            bufferBindShadowStore(
                textureSamplerShadow->vertexBuffers[dxmt9::shaders::kArgbufHybridBindSlot],
                populated.storage.handle, populated.offset);
          }
        }
      } else {
        perf::countEncodeDrawArgbufTableBindSkipped(1u);
      }
      {
        PerfScope byteAccountScope(
            argbufReopenSplit
                ? perf::countEncodeDrawArgbufReopenByteAccountCpuTime
                : nullptr);
        perf::countArgbufHybridBytes(populated.length);
        if (encoderBreakdown) {
          encoderBreakdown->addArgbufTableBytes(populated.length);
        }
      }
      bool canRepointCachedCbufs = false;
      {
        PerfScope cbufCacheProbeScope(
            argbufReopenSplit
                ? perf::countEncodeDrawArgbufReopenCbufCacheProbeCpuTime
                : nullptr);
        canRepointCachedCbufs =
            argbufCbufCache &&
            argbufCbufCache->matches(argbufPayloadHash) &&
            !uniform::anyDirty(*dirtyPtr, argbufConsumedBits);
      }
      if (canRepointCachedCbufs) {
        perf::countEncodeDrawArgbufCbufReopenFullRepointCalls(1u);
        {
          PerfScope fullRepointScope(
              perf::countEncodeDrawArgbufCbufFullRepointCpuTime);
          for (u32 i = 0; i < argbufCbufCache->bindings.entries.size(); ++i) {
            dxmt9::argbuf_hybrid::pointConstantBufferBinding(
                argbufEncoderForDraw, i, argbufCbufCache->bindings.entries[i],
                nullptr, encoder);
          }
        }
      } else {
        auto forceDirty = [&](std::uint16_t mask) {
          PerfScope forceDirtyScope(
              argbufReopenSplit
                  ? perf::countEncodeDrawArgbufReopenCbufForceDirtyCpuTime
                  : nullptr);
          dirtyPtr->mask = static_cast<std::uint16_t>(dirtyPtr->mask | mask);
        };
        auto pointCachedBinding = [&](u32 argbufIndex) -> bool {
          if (!argbufCbufCache || !argbufCbufCache->hasBinding(argbufIndex)) {
            return false;
          }
          const auto binding = argbufCbufCache->binding(argbufIndex);
          {
            PerfScope cachedRepointScope(
                argbufCbufProbeSplit
                    ? perf::countEncodeDrawArgbufCbufCachedRepointCpuTime
                    : nullptr);
            PerfScope cachedRepointStageScope(
                argbufCbufProbeSplit
                    ? argbufCbufCachedRepointCpuRecorder(argbufIndex)
                    : nullptr);
            dxmt9::argbuf_hybrid::pointConstantBufferBinding(
                argbufEncoderForDraw, argbufIndex, binding, nullptr, encoder);
          }
          perf::countEncodeDrawArgbufCbufCachedRepointCalls(1u);
          perf::countEncodeDrawArgbufCbufCachedRepointBytes(binding.bytes);
          if (argbufCbufProbeSplit) {
            countArgbufCbufCachedRepointStage(argbufIndex, binding.bytes);
          }
          return true;
        };
        bool hasAnyCbufDirty = false;
        {
          PerfScope dirtyScanScope(
              argbufReopenSplit
                  ? perf::countEncodeDrawArgbufReopenCbufDirtyScanCpuTime
                  : nullptr);
          hasAnyCbufDirty =
              uniform::anyDirty(*dirtyPtr, argbufConsumedBits);
        }
        if (!hasAnyCbufDirty) {
          // Payload hash drift without matching dirty bits is not trusted as
          // a whole-payload decision. Probe each cbuf category by its current
          // draw-state identity instead; only unmatched categories upload.
          perf::countEncodeDrawArgbufCbufReopenNoDirtyHashMismatch(1u);
          perf::countEncodeDrawArgbufCbufContentProbeCalls(1u);
          ArgbufCbufIdentityProbe vsProbe{};
          ArgbufCbufIdentityProbe psProbe{};
          ArgbufCbufIdentityProbe ffpPsProbe{};
          {
            PerfScope probeScope(
                argbufCbufProbeSplit
                    ? perf::countEncodeDrawArgbufCbufContentProbeCpuTime
                    : nullptr);
            if (!argbufVsPayloadSourceChanged) {
              PerfScope vsProbeScope(
                  argbufCbufProbeSplit
                      ? argbufCbufContentProbeCpuRecorder(
                            dxmt9::argbuf_hybrid::kConstantBufferVsIndex)
                      : nullptr);
              const auto vsPlan =
                  uniform::makeVsConstantUploadPlan(
                      *dirtyPtr, shaderUsage.vertexConstantUsage);
              const auto vsBytes = uniform::vsConstantUploadBytes(vsPlan);
              vsProbe = ArgbufCbufIdentityProbe{
                  .bytes = vsBytes,
                  .hash = makeArgbufCbufIdentityHash(
                      0x76735f636275665full,
                      drawStateVertexCbufSourceHash(drawState),
                      vsBytes),
              };
            }

            if (!argbufPsPayloadSourceChanged) {
              PerfScope psProbeScope(
                  argbufCbufProbeSplit
                      ? argbufCbufContentProbeCpuRecorder(
                            dxmt9::argbuf_hybrid::kConstantBufferPsIndex)
                      : nullptr);
              const auto psPlan =
                  uniform::makePsConstantUploadPlan(
                      *dirtyPtr, shaderUsage.pixelConstantUsage);
              const auto psBytes = uniform::psConstantUploadBytes(psPlan);
              psProbe = ArgbufCbufIdentityProbe{
                  .bytes = psBytes,
                  .hash = makeArgbufCbufIdentityHash(
                      0x70735f636275665full,
                      drawStatePixelCbufSourceHash(drawState),
                      psBytes),
              };
            }

            {
              PerfScope ffpPsProbeScope(
                  argbufCbufProbeSplit
                      ? argbufCbufContentProbeCpuRecorder(
                            dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex)
                      : nullptr);
              ffpPsProbe = ArgbufCbufIdentityProbe{
                  .bytes = sizeof(FfpPsConsts),
                  .hash = makeArgbufFfpPsIdentityHash(
                      drawState, sizeof(FfpPsConsts)),
              };
            }
          }

          auto pointCachedByIdentity =
              [&](std::uint16_t mask,
                  u32 argbufIndex,
                  ArgbufCbufIdentityProbe probe,
                  void (*countHit)(std::uint64_t),
                  void (*countMiss)(std::uint64_t)) {
                if (argbufCbufCache &&
                    argbufCbufCache->hasMatchingIdentity(
                        argbufIndex, probe.hash, probe.bytes) &&
                    pointCachedBinding(argbufIndex)) {
                  countHit(1u);
                  return;
                }
                countMiss(1u);
                forceDirty(mask);
              };

          if (argbufVsPayloadSourceChanged) {
            perf::countEncodeDrawArgbufCbufContentProbeVsMisses(1u);
            forceDirty(uniform::kVsAny);
          } else {
            pointCachedByIdentity(
                uniform::kVsAny,
                dxmt9::argbuf_hybrid::kConstantBufferVsIndex,
                vsProbe,
                perf::countEncodeDrawArgbufCbufContentProbeVsHits,
                perf::countEncodeDrawArgbufCbufContentProbeVsMisses);
          }
          if (argbufPsPayloadSourceChanged) {
            perf::countEncodeDrawArgbufCbufContentProbePsMisses(1u);
            forceDirty(uniform::kPsAny);
          } else {
            pointCachedByIdentity(
                uniform::kPsAny,
                dxmt9::argbuf_hybrid::kConstantBufferPsIndex,
                psProbe,
                perf::countEncodeDrawArgbufCbufContentProbePsHits,
                perf::countEncodeDrawArgbufCbufContentProbePsMisses);
          }
          // FFPVS stays on the deferred path because pre-transformed viewport
          // handling may patch the host bytes later in this draw.
          forceDirty(uniform::kFfpVsAny);
          pointCachedByIdentity(
              uniform::kFfpPsAny,
              dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex,
              ffpPsProbe,
              perf::countEncodeDrawArgbufCbufContentProbeFfpPsHits,
              perf::countEncodeDrawArgbufCbufContentProbeFfpPsMisses);
        } else {
          perf::countEncodeDrawArgbufCbufReopenPartialCandidates(1u);
          {
            PerfScope dirtyScanScope(
                argbufReopenSplit
                    ? perf::countEncodeDrawArgbufReopenCbufDirtyScanCpuTime
                    : nullptr);
            if (uniform::anyDirty(*dirtyPtr, uniform::kVsAny)) {
              perf::countEncodeDrawArgbufCbufReopenDirtyVs(1u);
            }
            if (uniform::anyDirty(*dirtyPtr, uniform::kPsAny)) {
              perf::countEncodeDrawArgbufCbufReopenDirtyPs(1u);
            }
            if (uniform::anyDirty(*dirtyPtr, uniform::kFfpVsAny)) {
              perf::countEncodeDrawArgbufCbufReopenDirtyFfpVs(1u);
            }
            if (uniform::anyDirty(*dirtyPtr, uniform::kFfpPsAny)) {
              perf::countEncodeDrawArgbufCbufReopenDirtyFfpPs(1u);
            }
          }
          auto repointCleanOrForceDirty = [&](std::uint16_t mask, u32 argbufIndex) {
            bool dirty = false;
            {
              PerfScope dirtyScanScope(
                  argbufReopenSplit
                      ? perf::countEncodeDrawArgbufReopenCbufDirtyScanCpuTime
                      : nullptr);
              dirty = uniform::anyDirty(*dirtyPtr, mask);
            }
            if (dirty) {
              return;
            }
            if (!pointCachedBinding(argbufIndex)) {
              forceDirty(mask);
            }
          };

          repointCleanOrForceDirty(
              uniform::kVsAny,
              dxmt9::argbuf_hybrid::kConstantBufferVsIndex);
          repointCleanOrForceDirty(
              uniform::kPsAny,
              dxmt9::argbuf_hybrid::kConstantBufferPsIndex);
          repointCleanOrForceDirty(
              uniform::kFfpVsAny,
              dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex);
          repointCleanOrForceDirty(
              uniform::kFfpPsAny,
              dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex);
        }
      }
    }
  }
  // R-BACK-12.24 — Stage 2 argbuf dirty mirror.
  //
  // When the encoder is on the argbuf-hybrid path AND any per-frequency
  // bit is dirty, mirror the dirty VS/PS/FFPPS regions into the argbuf so
  // the cbuf entries point at fresh transient slabs. FFPVS is deferred until
  // after the pre-transformed viewport override below; that lets the final
  // host bytes reuse a cached stable slice instead of re-uploading an
  // unchanged block on every draw.
  if (argbufTableMode) {
    PerfScope argbufSetupScope(perf::countEncodeDrawArgbufSetupCpuTime);
    auto dirtyForArgbuf = *dirtyPtr;
    uniform::clearBits(dirtyForArgbuf, uniform::kFfpVsAny);
    const auto cbufUpdateMask = static_cast<std::uint16_t>(
        argbufConsumedBits & ~uniform::kFfpVsAny);
    perf::countEncodeDrawArgbufCbufUpdateCalls(1u);
    if (!uniform::anyDirty(dirtyForArgbuf, cbufUpdateMask)) {
      perf::countEncodeDrawArgbufCbufUpdateSkippedClean(1u);
    } else {
      perf::countEncodeDrawArgbufCbufUpdateDirtyCalls(1u);
      PerfScope argbufCbufUpdateScope(
          perf::countEncodeDrawArgbufCbufUpdateCpuTime);
      if (argbufCbufDirtyIdentityProbe &&
          uniform::anyDirty(dirtyForArgbuf, uniform::kVsAny)) {
        perf::countEncodeDrawArgbufCbufDirtyVsIdentityProbeCalls(1u);
        const auto vsPlan =
            uniform::makeVsConstantUploadPlan(
                dirtyForArgbuf, shaderUsage.vertexConstantUsage);
        const auto vsBytes = uniform::vsConstantUploadBytes(vsPlan);
        const auto vsIdentityHash =
            makeArgbufCbufIdentityHash(
                0x76735f636275665full,
                drawStateVertexCbufSourceHash(drawState),
                vsBytes);
        if (!argbufCbufCache ||
            !argbufCbufCache->hasBinding(
                dxmt9::argbuf_hybrid::kConstantBufferVsIndex)) {
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityNoCache(1u);
        } else if (argbufCbufCache->hasMatchingIdentity(
                       dxmt9::argbuf_hybrid::kConstantBufferVsIndex,
                       vsIdentityHash,
                       vsBytes)) {
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityHits(1u);
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityHitBytes(vsBytes);
        } else {
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityMisses(1u);
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityMissBytes(vsBytes);
        }
      }
      dxmt9::argbuf_hybrid::ConstantBufferBindings writtenCbufBindings{};
      dxmt9::argbuf_hybrid::ConstantBufferUploadObserver cbufUploadObserver{};
      if (encoderBreakdownCbufContent) {
        cbufUploadObserver.userdata = encoderBreakdown;
        cbufUploadObserver.upload = recordArgbufCbufUploadForBreakdown;
      }
      if (encoderBreakdown && encoderBreakdown->enabled &&
          uniform::anyDirty(dirtyForArgbuf, uniform::kVsAny)) {
        encoderBreakdown->recordVsUploadPlan(
            dirtyForArgbuf, shaderUsage.vertexConstantUsage,
            uniform::makeVsConstantUploadPlan(
                dirtyForArgbuf, shaderUsage.vertexConstantUsage));
      }
      const auto bytes = dxmt9::argbuf_hybrid::updateDirtyArgbufRegions(
          ctx.queue, argbufEncoderForDraw, drawState, dirtyForArgbuf,
          shaderUsage.vertexConstantUsage, shaderUsage.pixelConstantUsage, seqId,
          nullptr, encoder, &writtenCbufBindings,
          cbufUploadObserver.upload ? &cbufUploadObserver : nullptr);
      if (bytes != 0) {
        stampArgbufCbufBindingIdentities(writtenCbufBindings, drawState);
        perf::countEncodeDrawArgbufCbufUpdateWriteCalls(1u);
        perf::countArgbufHybridBytes(bytes);
        if (encoderBreakdown) {
          encoderBreakdown->addArgbufCbufBindings(writtenCbufBindings);
        }
        if (argbufCbufCache) {
          PerfScope cacheMergeScope(
              perf::countEncodeDrawArgbufCbufCacheMergeCpuTime);
          argbufCbufCache->merge(argbufPayloadHash, writtenCbufBindings);
        }
      }
    }
    uniform::clearBits(*dirtyPtr, cbufUpdateMask);
  }
  {
    PerfScope uniformBuildScope(perf::countEncodeDrawUniformBuildCpuTime,
                            perf::countEncodeDrawUniformBuildMainCpuTime);
    if (directCbufBindings && uniform::anyDirty(*dirtyPtr, uniform::kVsAny)) {
      VsConsts vs = buildVsConsts(drawState);
      const auto plan =
          uniform::makeVsConstantUploadPlan(*dirtyPtr, shaderUsage.vertexConstantUsage);
      const auto bytes = static_cast<std::size_t>(uniform::vsConstantUploadBytes(plan));
      auto slice = uploadTransientBuffer(&vs, bytes, alignof(VsConsts));
      if (slice) {
        if (setVertexBufferCached(slice.buffer, slice.offset, 0)) {
          countUniformBufferBinds(1);
        }
        perf::countUniformVsConsts(bytes);
        uniform::clearBits(*dirtyPtr, uniform::kVsAny);
      }
    }
    if (directCbufBindings && uniform::anyDirty(*dirtyPtr, uniform::kPsAny)) {
      PsConsts ps = buildPsConsts(drawState);
      const auto plan =
          uniform::makePsConstantUploadPlan(*dirtyPtr, shaderUsage.pixelConstantUsage);
      const auto bytes = static_cast<std::size_t>(uniform::psConstantUploadBytes(plan));
      auto slice = uploadTransientBuffer(&ps, bytes, alignof(PsConsts));
      if (slice) {
        if (!suppressRecordedMetalCalls(ctx)) {
          encoder.setFragmentBuffer(slice.buffer, slice.offset, 0);
        }
        countUniformBufferBinds(1);
        perf::countUniformPsConsts(bytes);
        uniform::clearBits(*dirtyPtr, uniform::kPsAny);
      }
    }
    if (directCbufBindings && uniform::anyDirty(*dirtyPtr, uniform::kFfpPsAny)) {
      FfpPsConsts ffpPs = buildFfpPsConsts(drawState);
      auto slice = uploadTransientBuffer(&ffpPs, sizeof(FfpPsConsts), alignof(FfpPsConsts));
      if (slice) {
        if (!suppressRecordedMetalCalls(ctx)) {
          encoder.setFragmentBuffer(slice.buffer, slice.offset, 3);
        }
        countUniformBufferBinds(1);
        perf::countUniformFfpPs(sizeof(FfpPsConsts));
        uniform::clearBits(*dirtyPtr, uniform::kFfpPsAny);
      }
    }
  }
  // FfpVsConsts: every VS shader (FFP or otherwise) declares
  // [[buffer(3)]] FfpVsConsts because halfPixelFixup, clipPlanes, and
  // viewport metadata live there. Build the host copy lazily; the
  // FFP preTransformed path overrides viewportOrigin/Size below before
  // upload+bind via bindFfpVsIfDirty.
  std::optional<FfpVsConsts> ffpVs;
  auto ensureFfpVs = [&] {
    if (!ffpVs) ffpVs = buildFfpVsConsts(drawState);
    return &*ffpVs;
  };
  bool ffpVsBound = false;
  auto bindFfpVsIfDirty = [&] {
    if (ffpVsBound || !uniform::anyDirty(*dirtyPtr, uniform::kFfpVsAny)) {
      return;
    }
    auto* host = ensureFfpVs();
    if (argbufTableMode && argbufCbufCache &&
        argbufCbufCache->hasMatchingFfpVs(*host)) {
      dxmt9::argbuf_hybrid::pointConstantBufferBinding(
          argbufEncoderForDraw,
          dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex,
          argbufCbufCache->ffpVsBinding(), nullptr, encoder);
      argbufCbufCache->promotePayloadHash(argbufPayloadHash);
      uniform::clearBits(*dirtyPtr, uniform::kFfpVsAny);
      ffpVsBound = true;
      return;
    }
    auto slice = uploadTransientBuffer(host, sizeof(FfpVsConsts), alignof(FfpVsConsts));
    if (slice) {
      if (argbufTableMode) {
        dxmt9::argbuf_hybrid::pointFfpVsAtSlice(
            argbufEncoderForDraw, slice.buffer, slice.offset,
            nullptr, encoder);
        perf::countArgbufHybridBytes(sizeof(FfpVsConsts));
        if (encoderBreakdown) {
          encoderBreakdown->addArgbufCbufBytes(
              dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex,
              sizeof(FfpVsConsts));
        }
        if (encoderBreakdownCbufContent) {
          encoderBreakdown->recordArgbufCbufUploadContent(
              dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex,
              host, sizeof(FfpVsConsts), sizeof(FfpVsConsts));
        }
        if (argbufCbufCache) {
          argbufCbufCache->storeFfpVs(
              argbufPayloadHash, *host, slice.buffer, slice.offset,
              sizeof(FfpVsConsts));
        }
      } else {
        if (setVertexBufferCached(slice.buffer, slice.offset, 3)) {
          countUniformBufferBinds(1);
        }
        perf::countUniformFfpVs(sizeof(FfpVsConsts));
      }
      uniform::clearBits(*dirtyPtr, uniform::kFfpVsAny);
      ffpVsBound = true;
    }
  };
  bool expandedIndexedDraw = false;
  const bool scissorDisabled = debug::disableScissor();
  const u32 cullState = core::flatStateOr(
      hot.renderStates, RS_CULL_MODE, static_cast<u32>(core::CullMode::Ccw));
  const auto requestedCullMode = (preTransformed || debug::disableCull())
                                     ? WMTCullModeNone
                                     : static_cast<WMTCullMode>(toCullMode(cullState));
  WMTCullMode effectiveCullMode = applyDebugCullOverride(requestedCullMode);
  const auto forceCullModeProbe = debug::probeForceCullMode();
  if (forceCullModeProbe != debug::CullModeOverride::Disabled &&
      indexedDraw &&
      pv.primitiveType == core::PrimitiveType::TriangleList &&
      forceCullModeProbeRowMatches(encoderBreakdown) &&
      forceCullModeProbeClassMatches(primitiveCount,
                                     hot.textureMask,
                                     hot.renderStates,
                                     hot.viewport,
                                     fillMode)) {
    effectiveCullMode = toWmtCullMode(forceCullModeProbe, effectiveCullMode);
  }
  core::ViewportScissor effectiveViewport = hot.viewport;
  const auto scissorRectOverride = debug::probeScissorRectOverride();
  bool scissorRectProbeConsidered = false;
  bool scissorRectProbeEligible = false;
  bool scissorRectProbeApplied = false;
  if (scissorRectOverride.enabled &&
      !scissorDisabled &&
      indexedDraw &&
      pv.primitiveType == core::PrimitiveType::TriangleList &&
      hot.viewport.scissorEnabled &&
      scissorRectProbeRowMatches(encoderBreakdown)) {
    scissorRectProbeConsidered = true;
    scissorRectProbeEligible =
        scissorRectProbeClassMatches(primitiveCount,
                                     hot.textureMask,
                                     hot.renderStates,
                                     hot.viewport,
                                     fillMode);
    if (scissorRectProbeEligible) {
      effectiveViewport.scissorEnabled = true;
      effectiveViewport.scissor = scissorRectOverride.rect;
      scissorRectProbeApplied = true;
    }
  }
  const auto* bindingPacketSurface =
      ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
  const bool bindingPacketHasRasterTarget =
      bindingPacketSurface && bindingPacketSurface->texture;
  const DrawBindingPacketPlan* bindingPacketPtr = nullptr;
  {
    PerfScope bindingPacketScope(perf::countEncodeDrawBindingPacketCpuTime);
    DrawBindingPacketPlan bindingPacketPlan{};
    {
      PerfScope bindingPacketPlanScope(
          perf::countEncodeDrawBindingPacketPlanCpuTime);
      if (bindingPacketPlanSplitPerfEnabled()) {
        {
          PerfScope fragmentScope(
              perf::countEncodeDrawBindingPacketPlanFragmentCpuTime);
          bindingPacketPlan.fragmentTextureSamplers =
              makeFragmentTextureSamplerBindings(
                  hot,
                  &drawState.shaderContext().pixelShader);
        }
        {
          PerfScope vertexScope(
              perf::countEncodeDrawBindingPacketPlanVertexCpuTime);
          bindingPacketPlan.vertexTextureSamplers =
              makeVertexTextureSamplerBindings(hot);
        }
        {
          PerfScope extraStreamScope(
              perf::countEncodeDrawBindingPacketPlanExtraStreamCpuTime);
          bindingPacketPlan.extraStreams =
              makeProgrammableVsExtraStreamBindings(vertexDecl, hot, pv);
        }
        {
          PerfScope rasterScope(
              perf::countEncodeDrawBindingPacketPlanRasterCpuTime);
          bindingPacketPlan.raster =
              makeEncoderRasterStatePlan(
                  hot,
                  bindingPacketHasRasterTarget
                      ? bindingPacketSurface->desc.width
                      : 1u,
                  bindingPacketHasRasterTarget
                      ? bindingPacketSurface->desc.height
                      : 1u,
                  ffLayout && ffLayout->preTransformed,
                  scissorDisabled,
                  debug::disableCull(),
                  &effectiveViewport);
        }
      } else {
        bindingPacketPlan = makeDrawBindingPacketPlan(
            vertexDecl,
            hot,
            pv,
            bindingPacketHasRasterTarget ? bindingPacketSurface->desc.width : 1u,
            bindingPacketHasRasterTarget ? bindingPacketSurface->desc.height : 1u,
            ffLayout && ffLayout->preTransformed,
            scissorDisabled,
            debug::disableCull(),
            &drawState.shaderContext().pixelShader,
            &effectiveViewport);
      }
    }
    {
      PerfScope bindingPacketCacheScope(
          perf::countEncodeDrawBindingPacketCacheCpuTime);
      DrawBindingPacketCacheStats bindingPacketCacheStats{};
      bindingPacketPtr =
          &cacheDrawBindingPacket(
              gDrawBindingPacketCache,
              bindingPacketPlan,
              &bindingPacketCacheStats);
      perf::countEncodeDrawBindingPacketCacheKeyCpuTime(
          bindingPacketCacheStats.keyCpuNs);
      perf::countEncodeDrawBindingPacketCacheHashCpuTime(
          bindingPacketCacheStats.hashCpuNs);
      perf::countEncodeDrawBindingPacketCacheProbeCpuTime(
          bindingPacketCacheStats.probeCpuNs);
      perf::countEncodeDrawBindingPacketCacheStoreCpuTime(
          bindingPacketCacheStats.storeCpuNs);
      perf::countEncodeDrawBindingPacketCacheHits(
          bindingPacketCacheStats.hits);
      perf::countEncodeDrawBindingPacketCacheMisses(
          bindingPacketCacheStats.misses);
      perf::countEncodeDrawBindingPacketCacheCollisions(
          bindingPacketCacheStats.collisions);
    }
    if (encoderBreakdown) {
      PerfScope bindingPacketTextureRecordScope(
          perf::countEncodeDrawBindingPacketTextureRecordCpuTime);
      for (const auto& binding : bindingPacketPtr->fragmentTextureSamplers) {
        const auto* texture = ctx.pool.findTexture(binding.texture.value);
        encoderBreakdown->recordFragmentTextureBinding(binding.stage,
                                                       binding.texture,
                                                       texture);
      }
    }
  }
  drawPhase.mark(perf::countEncodeDrawPhaseArgbufUniformCpuTime);
  const auto& bindingPacket = *bindingPacketPtr;
  // Apply FFP preTransformed viewport override to the FfpVs host copy
  // before any bindFfpVsIfDirty call uploads it (R-BACK-12.5). The
  // override values come from run-stable sources (ffLayout +
  // targetSurface->desc); only mark dirty when the host copy actually
  // differs to avoid re-uploading the slab every draw.
  if (ffLayout && ffLayout->preTransformed) {
    if (auto* targetSurface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
        targetSurface) {
      auto* host = ensureFfpVs();
      const std::array<f32, 2> wantOrigin{0.0f, 0.0f};
      const std::array<f32, 2> wantSize{
          static_cast<f32>(std::max(1u, targetSurface->desc.width)),
          static_cast<f32>(std::max(1u, targetSurface->desc.height))};
      if (host->viewportOrigin != wantOrigin || host->viewportSize != wantSize) {
        host->viewportOrigin = wantOrigin;
        host->viewportSize = wantSize;
        uniform::setBit(*dirtyPtr, uniform::DirtyBit::FfpVsViewport);
      }
    }
  }
  bindFfpVsIfDirty();
  // Phase 3-E: viewport / scissor / cull are BaseDrawState-only.
  const bool streamBindPhaseSplitPerf = streamBindPhaseSplitPerfEnabled();
  if (!effectiveSkipBaseStateBind) {
    PerfScope streamBindViewportScope(perf::countEncodeDrawStreamBindCpuTime,
                            perf::countEncodeDrawStreamBindViewportCpuTime);
    PerfScope streamBindRasterPhaseScope(
        streamBindPhaseSplitPerf
            ? perf::countEncodeDrawStreamBindRasterPhaseCpuTime
            : nullptr);
    PerfScope rasterStateScope(perf::countEncodeDrawRasterStateCpuTime);
    perf::countEncodeDrawStreamBindRasterPhaseCalls(1u);
    if (bindingPacketHasRasterTarget) {
      // 2026-06-05 — viewport / scissor per-draw shadow cache was tested
      // (commit 5eef5d4) and reverted: bind diversity on GT1 is high
      // enough that the cache hit rate is essentially zero, while the
      // per-draw equality comparisons added +12.7% encode_chunk_cpu_ms.
      // See docs/perfomance/present-pacing/
      // present-pacing-bind-cache-work-a.01.md.
      recordedSetViewport(ctx, encoder, bindingPacket.raster.viewport);
      countViewportBind();
      recordedSetScissorRect(ctx, encoder, bindingPacket.raster.scissor);
      countScissorBind();
      setRasterizerCullMode(ctx, encoder, hot.renderStates, effectiveCullMode);
    }
  }
  static std::atomic<int> ffTraceRemaining{debug::fixedFunctionTraceBudget()};
  CommandQueue::TransientBufferSlice transientVertexBuffer;
  std::span<const u8> vertexBytes;
  const resources::BufferRecord* stream0Record = nullptr;
  WMT::Buffer vertexBuffer{};
  uint64_t vertexBufferOffset = 0;
  bool stream0Staged = false;
  auto makeTransientBuffer = [&](const void* data, std::size_t len) {
    return uploadTransientBuffer(data, len, 16);
  };
  auto makeTransientVertexBuffer = [&](const void* data, std::size_t len,
                                       ActiveEncoderBreakdown::TransientVertexSource source) {
    auto slice = makeTransientBuffer(data, len);
    if (slice && encoderBreakdown) {
      encoderBreakdown->addTransientVertexBytes(static_cast<u64>(len), source);
    }
    return slice;
  };
  auto makeTransientIndexBuffer = [&](const void* data, std::size_t len,
                                      ActiveEncoderBreakdown::TransientIndexSource source) {
    auto slice = makeTransientBuffer(data, len);
    if (slice && encoderBreakdown) {
      encoderBreakdown->addTransientIndexBytes(static_cast<u64>(len), source);
    }
    return slice;
  };
  const auto* stream0Snapshot =
      streamBindingSnapshot(bindingSnapshot, 0u);
  const auto* indexSnapshot =
      indexBindingSnapshot(bindingSnapshot);
  {
    PerfScope fvfDecodeBytesScope(perf::countEncodeDrawFvfDecodeCpuTime,
                            perf::countEncodeDrawFvfDecodeBytesCpuTime);
    if (!pv.userVertexData.empty()) {
      // Phase 5-B: prefer pre-batched UP vertex slice when the
      // DrawRun handler did the bulk upload; otherwise fall back
      // to the per-draw upload.
      if (preUploaded && preUploaded->vertex) {
        transientVertexBuffer = preUploaded->vertex;
      } else {
        transientVertexBuffer = makeTransientVertexBuffer(
            pv.userVertexData.data(),
            pv.userVertexData.size(),
            ActiveEncoderBreakdown::TransientVertexSource::User);
      }
      if (transientVertexBuffer) {
        vertexBuffer = transientVertexBuffer.buffer;
        vertexBufferOffset = transientVertexBuffer.offset + hot.streamOffsets[0];
        vertexBytes = pv.userVertexData;
      }
    } else if (hot.streamBuffers[0]) {
      if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[0].value);
          buffer && (buffer->buffer || (stream0Snapshot && stream0Snapshot->valid()))) {
        stream0Record = buffer;
        vertexBuffer = WMT::Buffer{stream0Snapshot
            ? stream0Snapshot->metalHandle
            : buffer->buffer.handle};
        vertexBufferOffset = hot.streamOffsets[0];
        if (traceEncode) {
          std::ostringstream trace;
          trace << "[dxmt9-encode-stream] seq=" << static_cast<unsigned long long>(seqId)
                << " stream=0 slot=1 handle="
                << static_cast<unsigned long long>(hot.streamBuffers[0].value)
                << " liveMetal=0x" << std::hex
                << static_cast<unsigned long long>(buffer->buffer.handle)
                << std::dec
                << " boundMetal=0x" << std::hex
                << static_cast<unsigned long long>(vertexBuffer.handle)
                << std::dec
                << " snapshot=" << (stream0Snapshot ? 1 : 0)
                << " offset=" << vertexBufferOffset
                << " stride=" << hot.streamStrides[0]
                << " shadowBytes=" << buffer->shadow.size()
                << " contents=" << (buffer->contents ? 1 : 0);
          emitQueueTraceLine(trace.str());
        }
        if (const auto bytes = snapshotBufferBytes(stream0Snapshot);
            !bytes.empty()) {
          vertexBytes = bytes;
        } else if (!buffer->shadow.empty()) {
          vertexBytes = buffer->shadow;
        } else if (buffer->contents) {
          vertexBytes = std::span<const u8>(static_cast<const u8*>(buffer->contents),
                                            static_cast<std::size_t>(buffer->desc.size));
        }
      } else if (vertexDecl.streams[0].buffer) {
        const auto bytes = vertexDecl.streams[0].buffer->bytes();
        if (!bytes.empty()) {
          transientVertexBuffer = makeTransientVertexBuffer(
              bytes.data(), bytes.size(),
              ActiveEncoderBreakdown::TransientVertexSource::DeclFallback);
          if (transientVertexBuffer) {
            vertexBuffer = transientVertexBuffer.buffer;
            vertexBufferOffset = transientVertexBuffer.offset + hot.streamOffsets[0];
            vertexBytes = bytes;
          }
        }
      }
    }
  }
  if (streamIbStagingActive(streamIbStagingCache) &&
      indexedDraw &&
      pv.userVertexData.empty() &&
      !stream0Snapshot &&
      stream0Record &&
      stream0Record->buffer &&
      vertexBuffer) {
    if (auto staged = streamIbStagingCache->findOrStage(
            ctx, seqId, hot.streamBuffers[0].value, stream0Record,
            encoderBreakdown, /*indexBuffer=*/false)) {
      vertexBuffer = staged.buffer;
      vertexBufferOffset = staged.offset + hot.streamOffsets[0];
      stream0Staged = true;
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
  // DrawVolatile field state — derived per draw and pushed via
  // setVertexBytes(slot=5) right before drawPrimitives.
  u32 drawVertexStreamOffset = 0;
  u32 drawVertexStreamStride = 0;
  i32 drawVertexBaseIndex = 0;
  // Branch on fixedFunctionPath (which respects the bound shader context),
  // not just ffLayout. A user-bound programmable VS with an FFP-decodable
  // vertex declaration must take the programmable path; gating on ffLayout
  // alone would force such draws through the FFP setup below.
  drawPhase.mark(perf::countEncodeDrawPhaseStreamPrepCpuTime);
  if (fixedFunctionPath && ffLayout) {
    if (!vertexBuffer) {
      if (traceEncode) {
        emitQueueTraceLine("[dxmt9-encode] seq=" + std::to_string(seqId) +
                           " ordinal=" + std::to_string(drawOrdinal) +
                           " skipped reason=no-vertex-buffer");
      }
      return false;
    }
    {
      PerfScope uniformBuildFfScope(perf::countEncodeDrawUniformBuildCpuTime,
                            perf::countEncodeDrawUniformBuildFfpCpuTime);
      drawVertexStreamOffset = 0;
      drawVertexStreamStride =
          vertexDecl.streams[0].stride ? vertexDecl.streams[0].stride : ffLayout->stride;
      if (!indexedDraw && drawVertexStreamStride != 0u) {
        vertexBufferOffset += static_cast<uint64_t>(pv.startVertex) *
                              static_cast<uint64_t>(drawVertexStreamStride);
        drawVertexBaseIndex = 0;
      } else {
        drawVertexBaseIndex = indexedDraw ? pv.baseVertexIndex : static_cast<i32>(pv.startVertex);
      }
    }
    {
      PerfScope streamBindFfScope(perf::countEncodeDrawStreamBindCpuTime,
                            perf::countEncodeDrawStreamBindFfpCpuTime);
      PerfScope streamBindFfpStreamScope(
          streamBindPhaseSplitPerf
              ? perf::countEncodeDrawStreamBindFfpStreamCpuTime
              : nullptr);
      PerfScope vertexStreamBindFfScope(perf::countEncodeDrawVertexStreamBindCpuTime);
      perf::countEncodeDrawStreamBindFfpStreamCalls(1u);
      if (encoderBreakdown) {
        if (stream0Record) {
          encoderBreakdown->recordStreamResource(0, hot.streamBuffers[0].value,
                                                 stream0Record->desc);
        }
        const u64 stream0StateHandle =
            stream0Staged ? vertexBuffer.handle : hot.streamBuffers[0].value;
        const u64 stream0StateOffset =
            stream0Staged ? vertexBufferOffset : hot.streamOffsets[0];
        encoderBreakdown->recordStreamState(
            0, stream0StateHandle, stream0StateOffset, drawVertexStreamStride);
      }
      if (setVertexBufferCached(vertexBuffer, vertexBufferOffset, 1)) {
        countVertexBufferBind();
        if (encoderBreakdown) {
          encoderBreakdown->recordStreamMetalBind(0);
        }
      }
    }

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
              << " startVertex=" << pv.startVertex
              << " baseVertex=" << pv.baseVertexIndex
              << " startIndex=" << pv.startIndex
              << " primCount=" << pv.primitiveCount
              << " stride=" << drawVertexStreamStride
              << " viewport=(" << ensureFfpVs()->viewportOrigin[0] << "," << ensureFfpVs()->viewportOrigin[1]
              << " " << ensureFfpVs()->viewportSize[0] << "x" << ensureFfpVs()->viewportSize[1] << ")"
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
        const auto& texM0 = ensureFfpVs()->ffpTextureTransforms[0];
        trace << " texM0=["
              << texM0[0][0] << "," << texM0[0][1] << ","
              << texM0[0][2] << "," << texM0[0][3] << ";"
              << texM0[1][0] << "," << texM0[1][1] << ","
              << texM0[1][2] << "," << texM0[1][3] << ";"
              << texM0[2][0] << "," << texM0[2][1] << ","
              << texM0[2][2] << "," << texM0[2][3] << ";"
              << texM0[3][0] << "," << texM0[3][1] << ","
              << texM0[3][2] << "," << texM0[3][3] << "]";
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
            drawVertexStreamStride ? drawVertexStreamStride : ffLayout->stride);
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

        if (hot.indexBuffer || !pv.userIndexData.empty()) {
          const auto* indexRecord = ctx.pool.findBuffer(hot.indexBuffer.value);
          std::span<const u8> indexBytes;
          if (!pv.userIndexData.empty()) {
            indexBytes = pv.userIndexData;
          } else if (indexRecord && !indexRecord->shadow.empty()) {
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
  drawPhase.mark(perf::countEncodeDrawPhaseFfpVertexCpuTime);
  if (vertexBuffer && !fixedFunctionPath) {
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
      if (ffLayout && !vertexBytes.empty()) {
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
        auto appendVertexRef = [&](std::string_view prefix, u32 ordinal, u32 vertexIndex) {
          const int effectiveVertexIndex =
              pv.baseVertexIndex + static_cast<int>(vertexIndex);
          if (effectiveVertexIndex < 0) {
            return;
          }
          const std::size_t refBase =
              static_cast<std::size_t>(hot.streamOffsets[0]) +
              static_cast<std::size_t>(effectiveVertexIndex) *
                  static_cast<std::size_t>(
                      drawVertexStreamStride ? drawVertexStreamStride : ffLayout->stride);
          trace << " " << prefix << ordinal << "#" << vertexIndex << "=("
                << readF32(refBase + ffLayout->positionOffset + 0) << ","
                << readF32(refBase + ffLayout->positionOffset + 4) << ","
                << readF32(refBase + ffLayout->positionOffset + 8) << ")";
          if (ffLayout->hasDiffuse) {
            trace << " c=0x" << std::hex
                  << readU32(refBase + ffLayout->diffuseOffset)
                  << std::dec;
          }
          if (ffLayout->hasTexcoord[0]) {
            trace << " uv=("
                  << readF32(refBase + ffLayout->texcoordOffset[0] + 0)
                  << ","
                  << readF32(refBase + ffLayout->texcoordOffset[0] + 4)
                  << ")";
          }
        };
        std::span<const u8> indexBytes;
        if (!pv.userIndexData.empty()) {
          indexBytes = pv.userIndexData;
        } else if (hot.indexBuffer) {
          const auto* indexRecord = ctx.pool.findBuffer(hot.indexBuffer.value);
          if (indexRecord && !indexRecord->shadow.empty()) {
            indexBytes = indexRecord->shadow;
          } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
            indexBytes = std::span<const u8>(
                static_cast<const u8*>(indexRecord->contents),
                static_cast<std::size_t>(indexRecord->desc.size));
          }
        }
        if (indexedDraw && !indexBytes.empty()) {
          const std::size_t indexStart =
              static_cast<std::size_t>(pv.startIndex) *
              indexElementSize(pv.indexType);
          const u32 tracedIndexCount = std::min<u32>(primitiveCount * 3u, 36u);
          trace << " idx=";
          for (u32 i = 0; i < tracedIndexCount; ++i) {
            if (i != 0u) {
              trace << ",";
            }
            std::optional<u32> vertexIndex;
            const std::size_t offset =
                indexStart + static_cast<std::size_t>(i) *
                indexElementSize(pv.indexType);
            if (pv.indexType == IndexType::UInt16 &&
                offset + sizeof(u16) <= indexBytes.size()) {
              u16 value = 0;
              std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
              vertexIndex = value;
            } else if (pv.indexType == IndexType::UInt32 &&
                       offset + sizeof(u32) <= indexBytes.size()) {
              u32 value = 0;
              std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
              vertexIndex = value;
            }
            if (vertexIndex.has_value()) {
              trace << *vertexIndex;
            } else {
              trace << '?';
            }
          }
          const u32 tracedRefs = std::min<u32>(12u, tracedIndexCount);
          for (u32 i = 0; i < tracedRefs; ++i) {
            const std::size_t offset =
                indexStart + static_cast<std::size_t>(i) *
                indexElementSize(pv.indexType);
            std::optional<u32> vertexIndex;
            if (pv.indexType == IndexType::UInt16 &&
                offset + sizeof(u16) <= indexBytes.size()) {
              u16 value = 0;
              std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
              vertexIndex = value;
            } else if (pv.indexType == IndexType::UInt32 &&
                       offset + sizeof(u32) <= indexBytes.size()) {
              u32 value = 0;
              std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
              vertexIndex = value;
            }
            if (!vertexIndex.has_value()) {
              break;
            }
            appendVertexRef("r", i, *vertexIndex);
          }
        } else {
          const u32 tracedVertexCount =
              std::min<u32>(static_cast<u32>(vertexCount), 12u);
          for (u32 i = 0; i < tracedVertexCount; ++i) {
            appendVertexRef("v", i, pv.startVertex + i);
          }
        }
      }
      emitQueueTraceLine(trace.str());
    }
    {
      PerfScope uniformBuildVsScope(perf::countEncodeDrawUniformBuildCpuTime,
                            perf::countEncodeDrawUniformBuildVsCpuTime);
      drawVertexStreamOffset = 0;
      drawVertexStreamStride =
          ffLayout ? (vertexDecl.streams[0].stride ? vertexDecl.streams[0].stride : ffLayout->stride)
                   : computeVertexDeclStride(vertexDecl);
      if (!indexedDraw && drawVertexStreamStride != 0u) {
        vertexBufferOffset += static_cast<uint64_t>(pv.startVertex) *
                              static_cast<uint64_t>(drawVertexStreamStride);
        drawVertexBaseIndex = 0;
      } else {
        drawVertexBaseIndex = indexedDraw ? pv.baseVertexIndex : static_cast<i32>(pv.startVertex);
      }
    }
    {
      PerfScope streamBindVsScope(perf::countEncodeDrawStreamBindCpuTime,
                            perf::countEncodeDrawStreamBindVsCpuTime);
      PerfScope streamBindShaderStreamScope(
          streamBindPhaseSplitPerf
              ? perf::countEncodeDrawStreamBindShaderStreamCpuTime
              : nullptr);
      PerfScope vertexStreamBindVsScope(perf::countEncodeDrawVertexStreamBindCpuTime);
      perf::countEncodeDrawStreamBindShaderStreamCalls(1u);
      if (encoderBreakdown) {
        if (stream0Record) {
          encoderBreakdown->recordStreamResource(0, hot.streamBuffers[0].value,
                                                 stream0Record->desc);
        }
        const u64 stream0StateHandle =
            stream0Staged ? vertexBuffer.handle : hot.streamBuffers[0].value;
        const u64 stream0StateOffset =
            stream0Staged ? vertexBufferOffset : hot.streamOffsets[0];
        encoderBreakdown->recordStreamState(
            0, stream0StateHandle, stream0StateOffset, drawVertexStreamStride);
      }
      if (setVertexBufferCached(vertexBuffer, vertexBufferOffset, 1)) {
        countVertexBufferBind();
        if (encoderBreakdown) {
          encoderBreakdown->recordStreamMetalBind(0);
        }
      }
      for (const auto& streamBinding : bindingPacket.extraStreams) {
        WMT::Buffer extraVertexBuffer{};
        uint64_t extraVertexBufferOffset = streamBinding.offset;
        const u32 stream = streamBinding.stream;
        uint64_t liveMetalHandle = 0;
        std::size_t shadowBytes = 0;
        bool hasContents = false;
        bool usedDeclBytes = false;
        bool extraStaged = false;
        const resources::BufferRecord* extraRecord = nullptr;
        const auto* extraSnapshot =
            streamBindingSnapshot(bindingSnapshot, stream);
        if (hot.streamBuffers[stream]) {
          if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[stream].value);
              buffer && (buffer->buffer || (extraSnapshot && extraSnapshot->valid()))) {
            extraRecord = buffer;
            extraVertexBuffer = WMT::Buffer{extraSnapshot
                ? extraSnapshot->metalHandle
                : buffer->buffer.handle};
            liveMetalHandle = buffer->buffer.handle;
            shadowBytes = buffer->shadow.size();
            hasContents = buffer->contents != nullptr;
          }
        }
        if (!extraVertexBuffer && vertexDecl.streams[stream].buffer) {
          const auto bytes = vertexDecl.streams[stream].buffer->bytes();
          if (!bytes.empty()) {
            if (auto slice = makeTransientVertexBuffer(
                    bytes.data(), bytes.size(),
                    ActiveEncoderBreakdown::TransientVertexSource::DeclFallback)) {
              extraVertexBuffer = slice.buffer;
              extraVertexBufferOffset += slice.offset;
              usedDeclBytes = true;
            }
          }
        }
        if (streamIbStagingActive(streamIbStagingCache) &&
            indexedDraw &&
            !extraSnapshot &&
            extraRecord &&
            extraRecord->buffer &&
            extraVertexBuffer) {
          if (auto staged = streamIbStagingCache->findOrStage(
                  ctx, seqId, hot.streamBuffers[stream].value, extraRecord,
                  encoderBreakdown, /*indexBuffer=*/false)) {
            extraVertexBuffer = staged.buffer;
            extraVertexBufferOffset = staged.offset + streamBinding.offset;
            extraStaged = true;
          }
        }
        if (traceEncode) {
          std::ostringstream trace;
          trace << "[dxmt9-encode-stream] seq=" << static_cast<unsigned long long>(seqId)
                << " stream=" << stream
                << " slot=" << streamBinding.metalSlot
                << " handle="
                << static_cast<unsigned long long>(hot.streamBuffers[stream].value)
                << " liveMetal=0x" << std::hex
                << static_cast<unsigned long long>(liveMetalHandle)
                << " boundMetal=0x"
                << static_cast<unsigned long long>(extraVertexBuffer.handle)
                << std::dec
                << " snapshot=" << (extraSnapshot ? 1 : 0)
                << " offset=" << extraVertexBufferOffset
                << " stride=" << streamBinding.stride
                << " declFallback=" << (usedDeclBytes ? 1 : 0)
                << " shadowBytes=" << shadowBytes
                << " contents=" << (hasContents ? 1 : 0)
                << " bound=" << (extraVertexBuffer ? 1 : 0);
          emitQueueTraceLine(trace.str());
        }
        if (extraVertexBuffer) {
          if (encoderBreakdown) {
            if (extraRecord) {
              encoderBreakdown->recordStreamResource(
                  stream, hot.streamBuffers[stream].value, extraRecord->desc);
            }
            const u64 extraStateHandle =
                extraStaged ? extraVertexBuffer.handle : hot.streamBuffers[stream].value;
            const u64 extraStateOffset =
                extraStaged ? extraVertexBufferOffset : hot.streamOffsets[stream];
            encoderBreakdown->recordStreamState(
                stream, extraStateHandle, extraStateOffset,
                streamBinding.stride);
          }
          if (setVertexBufferCached(extraVertexBuffer, extraVertexBufferOffset,
                                    streamBinding.metalSlot)) {
            countVertexBufferBind();
            if (encoderBreakdown) {
              encoderBreakdown->recordStreamMetalBind(stream);
            }
          }
        }
      }
    }
  }
  // Phase 3-E: texture / sampler binding is BaseDrawState-only.
  // R-BACK-12.24 — texture/sampler resources travel on the direct render
  // encoder lane (the validated Stage 1 binding path) regardless of
  // whether the constant argbuf hybrid is active.
  drawPhase.mark(perf::countEncodeDrawPhaseVertexBindCpuTime);
  if (!effectiveSkipBaseStateBind) {
    PerfScope streamBindTexScope(perf::countEncodeDrawStreamBindCpuTime,
                            perf::countEncodeDrawStreamBindTextureCpuTime);
    PerfScope streamBindTexturePhaseScope(
        streamBindPhaseSplitPerf
            ? perf::countEncodeDrawStreamBindTexturePhaseCpuTime
            : nullptr);
    PerfScope textureSamplerBindScope(perf::countEncodeDrawTextureSamplerBindCpuTime);
    perf::countEncodeDrawStreamBindTexturePhaseCalls(1u);
    const bool samplerSupportsArgumentBuffers = dxmt9::shaders::argbufResourceArrayEnabled();
    const bool textureSamplerDirectSplitPerf = textureSamplerDirectSplitPerfEnabled();
    auto resolveFragmentSamplerState =
        [&](const core::FlatStateSet<core::kMaxSamplerStates>& samplerStates,
            u32 textureLod) -> std::pair<WMT::Reference<WMT::SamplerState>, WMT::SamplerState> {
      perf::countEncodeDrawTextureSamplerSamplerLookupCalls(1u);
      PerfScope samplerLookupScope(
          perf::countEncodeDrawTextureSamplerSamplerLookupCpuTime);
      if (suppressBaseStateLookup(ctx)) {
        return {WMT::Reference<WMT::SamplerState>{}, ctx.drawRecorder->fragmentSamplerState};
      }
      auto samplerRef = ctx.cache.samplerStateFor(
          ctx.device, samplerStates,
          static_cast<float>(textureLod),
          samplerSupportsArgumentBuffers);
      return {samplerRef, WMT::SamplerState{samplerRef.handle}};
    };
    if (!bindingPacket.fragmentTextureSamplers.empty()) {
      struct ResolvedFragmentTextureSamplerBinding {
        u32 stage = 0;
        core::Handle textureHandle{};
        u32 textureLod = 0;
        const resources::TextureRecord* textureRecord = nullptr;
        WMT::Texture texture{};
        u64 samplerStateHash = 0;
        core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
        bool srgbTexture = false;
        WMT::Reference<WMT::SamplerState> samplerRef{};
        WMT::SamplerState sampler{};
      };
      std::array<ResolvedFragmentTextureSamplerBinding, core::kMaxSamplers>
          resolvedFragmentBindings{};
      std::size_t resolvedFragmentBindingCount = 0;
      {
        PerfScope fragmentResolveScope(
            perf::countEncodeDrawTextureSamplerFragmentResolveCpuTime);
        perf::countEncodeDrawTextureSamplerFragmentResolveCalls(1u);
        for (const auto& binding : bindingPacket.fragmentTextureSamplers) {
          const auto stage = binding.stage;
          const auto textureHandle = binding.texture;
          if (const u64 skipped = debug::skippedTextureHandle();
              skipped != 0ull && textureHandle.value == skipped) {
            if (traceEncode || debug::shouldTraceTexture(textureHandle)) {
              std::ostringstream out;
              out << "[dxmt9-debug] skip draw seq=" << static_cast<unsigned long long>(seqId)
                  << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
                  << " tex" << stage << "=" << static_cast<unsigned long long>(textureHandle.value);
              emitQueueTraceLine(out.str());
            }
            return false;
          }
          auto& resolved = resolvedFragmentBindings[resolvedFragmentBindingCount++];
          resolved.stage = stage;
          resolved.textureHandle = textureHandle;
          resolved.textureLod = binding.textureLod;
          resolved.samplerStateHash = binding.samplerStateHash;
          resolved.samplerStates = binding.samplerStates;
          const bool srgbTexture =
              core::flatStateOr(hot.samplerStates[stage], core::SAMP_SRGB_TEXTURE, 0u) != 0;
          resolved.srgbTexture = srgbTexture;
          if (textureSamplerDirectSplitPerf) {
            perf::countEncodeDrawTextureSamplerFragmentResolveTextureCalls(1u);
          }
          PerfScope fragmentResolveTextureScope(
              textureSamplerDirectSplitPerf
                  ? perf::countEncodeDrawTextureSamplerFragmentResolveTextureCpuTime
                  : nullptr);
          if (auto* texture = ctx.pool.findTexture(textureHandle.value); texture && texture->texture) {
            resolved.textureRecord = texture;
            resolved.texture = resources::textureForShaderRead(*texture, srgbTexture);
          }
        }
      }

      // R-BACK-12.22..12.26 (resource-array sub-mode) — when this pass runs
      // the resource-array lane, fragment-stage textures/samplers travel
      // through the slot-30 argbuf (writes + useResource residency) instead
      // of the direct lane. Only the FFP s0..s7 fragment stages (<
      // kArgbufResourceArrayStageCount) ride the argbuf; any higher stage
      // stays direct. The argbuf texture array is homogeneously
      // texture2d<float>, so this MUST match the emitter's
      // pixelResourceArrayEligible decision: every used stage < 8 AND every
      // bound texture is 2D. A cube/volume binding (or a stage >= 8) forces
      // the WHOLE draw back onto the direct lane — the emitter emitted the
      // constants-only-hybrid form with direct [[texture(N)]] params for
      // exactly this shader, so a split would leave those params unbound.
      // Trace lines are kept identical to the direct path so capture
      // diffing is unchanged.
      bool fragmentResourceArrayEligible = useResourceArrayArgbuf;
      if (fragmentResourceArrayEligible) {
        for (std::size_t i = 0; i < resolvedFragmentBindingCount; ++i) {
          const auto& binding = resolvedFragmentBindings[i];
          if (binding.stage >= dxmt9::shaders::kArgbufResourceArrayStageCount) {
            fragmentResourceArrayEligible = false;
            break;
          }
          if (binding.textureRecord &&
              binding.textureRecord->desc.type != core::TextureType::TwoD &&
              binding.textureRecord->desc.type != core::TextureType::Array2D) {
            fragmentResourceArrayEligible = false;
            break;
          }
        }
      }
      const bool useResourceArrayLane = fragmentResourceArrayEligible;
      if (useResourceArrayLane) {
        PerfScope fragmentResourceArrayScope(
            perf::countEncodeDrawTextureSamplerFragmentResourceArrayCpuTime);
        perf::countEncodeDrawTextureSamplerFragmentResourceArrayCalls(1u);
        std::array<dxmt9::argbuf_hybrid::ResourceArrayBinding,
                   dxmt9::shaders::kArgbufResourceArrayStageCount>
            argbufBindings{};
        std::size_t argbufBindingCount = 0;
        for (std::size_t i = 0; i < resolvedFragmentBindingCount; ++i) {
          auto& binding = resolvedFragmentBindings[i];
          if (binding.stage >= dxmt9::shaders::kArgbufResourceArrayStageCount) {
            continue;
          }
          if ((traceEncode || debug::shouldTraceTexture(binding.textureHandle)) &&
              binding.textureRecord) {
            std::ostringstream out;
            out << "[dxmt9-texture] bind stage=" << binding.stage
                << " handle=0x" << std::hex << binding.textureHandle.value << std::dec
                << " format=" << static_cast<unsigned>(binding.textureRecord->desc.format)
                << " size=" << binding.textureRecord->desc.width << "x"
                << binding.textureRecord->desc.height
                << " levels=" << binding.textureRecord->desc.levels
                << " lod=" << binding.textureLod;
            appendSamplerTrace(out, binding.samplerStates, binding.srgbTexture);
            emitTextureTraceLine(out.str());
          }
          auto& slot = argbufBindings[argbufBindingCount++];
          slot.stage = binding.stage;
          slot.texture = binding.texture;
          auto [samplerRef, sampler] =
              resolveFragmentSamplerState(binding.samplerStates, binding.textureLod);
          binding.samplerRef = samplerRef;
          binding.sampler = sampler;
          slot.sampler = binding.sampler;
          if (binding.samplerRef) {
            ctx.queue.retainSamplerForSeq(binding.samplerRef, seqId);
          }
          if (binding.texture) countTextureBind();
          if (binding.sampler) countSamplerBind();
        }
        if (!suppressRecordedMetalCalls(ctx)) {
          dxmt9::argbuf_hybrid::populateResourceBindings(
              argbufEncoderForDraw,
              std::span<const dxmt9::argbuf_hybrid::ResourceArrayBinding>(
                  argbufBindings.data(), argbufBindingCount),
              /*recorder=*/nullptr, encoder);
        }
      } else {
        PerfScope fragmentDirectScope(
            perf::countEncodeDrawTextureSamplerFragmentDirectCpuTime);
        perf::countEncodeDrawTextureSamplerFragmentDirectCalls(1u);
        for (std::size_t i = 0; i < resolvedFragmentBindingCount; ++i) {
          auto& binding = resolvedFragmentBindings[i];
          if (binding.texture) {
            if (textureSamplerDirectSplitPerf) {
              perf::countEncodeDrawTextureSamplerFragmentDirectTextureCalls(1u);
            }
            PerfScope fragmentDirectTextureScope(
                textureSamplerDirectSplitPerf
                    ? perf::countEncodeDrawTextureSamplerFragmentDirectTextureCpuTime
                    : nullptr);
            bool skipTextureBind = false;
            if (textureSamplerShadow && binding.stage < core::kMaxSamplers) {
              auto& slot = textureSamplerShadow->fragmentTextures[binding.stage];
              const auto hash = textureSamplerShadowHash(
                  kFragmentTextureShadowTag,
                  static_cast<std::uint8_t>(binding.stage),
                  binding.texture.handle);
              skipTextureBind = textureSamplerShadowMatches(
                  slot, hash, binding.texture.handle);
              if (!skipTextureBind) {
                textureSamplerShadowStore(slot, hash, binding.texture.handle);
              }
            }
            if (skipTextureBind) {
              countTextureBindSkipped();
            } else {
              if ((traceEncode || debug::shouldTraceTexture(binding.textureHandle)) &&
                  binding.textureRecord) {
                std::ostringstream out;
                out << "[dxmt9-texture] bind stage=" << binding.stage
                    << " handle=0x" << std::hex << binding.textureHandle.value << std::dec
                    << " format=" << static_cast<unsigned>(binding.textureRecord->desc.format)
                    << " size=" << binding.textureRecord->desc.width << "x"
                    << binding.textureRecord->desc.height
                    << " levels=" << binding.textureRecord->desc.levels
                    << " lod=" << binding.textureLod;
                appendSamplerTrace(out, binding.samplerStates, binding.srgbTexture);
                emitTextureTraceLine(out.str());
              }
              {
                if (textureSamplerDirectSplitPerf) {
                  perf::countEncodeDrawTextureSamplerFragmentDirectTextureSetCalls(1u);
                }
                PerfScope fragmentDirectTextureSetScope(
                    textureSamplerDirectSplitPerf
                        ? perf::countEncodeDrawTextureSamplerFragmentDirectTextureSetCpuTime
                        : nullptr);
                recordedSetFragmentTexture(ctx, encoder, binding.texture,
                                           static_cast<std::uint8_t>(binding.stage));
              }
              countTextureBind();
            }
          }
          {
            if (textureSamplerDirectSplitPerf) {
              perf::countEncodeDrawTextureSamplerFragmentDirectSamplerCalls(1u);
            }
            PerfScope fragmentDirectSamplerScope(
                textureSamplerDirectSplitPerf
                    ? perf::countEncodeDrawTextureSamplerFragmentDirectSamplerCpuTime
                    : nullptr);
            bool skipSamplerBind = false;
            const auto samplerHash = samplerBindShadowHash(
                kFragmentSamplerShadowTag,
                static_cast<std::uint8_t>(binding.stage),
                binding.samplerStateHash,
                binding.textureLod,
                samplerSupportsArgumentBuffers);
            if (textureSamplerShadow && binding.stage < core::kMaxSamplers) {
              auto& slot = textureSamplerShadow->fragmentSamplers[binding.stage];
              skipSamplerBind = samplerBindShadowMatches(
                  slot, samplerHash, binding.samplerStates, binding.textureLod,
                  samplerSupportsArgumentBuffers);
            }
            if (skipSamplerBind) {
              perf::countEncodeDrawTextureSamplerSamplerLookupSkippedPrehandle(1u);
              countSamplerBindSkipped();
            } else {
              auto [samplerRef, sampler] =
                  resolveFragmentSamplerState(binding.samplerStates, binding.textureLod);
              if (sampler) {
                if (textureSamplerShadow && binding.stage < core::kMaxSamplers) {
                  auto& slot = textureSamplerShadow->fragmentSamplers[binding.stage];
                  samplerBindShadowStore(
                      slot, samplerHash, binding.samplerStates, binding.textureLod,
                      samplerSupportsArgumentBuffers, sampler.handle);
                }
                binding.samplerRef = samplerRef;
                binding.sampler = sampler;
                {
                  if (textureSamplerDirectSplitPerf) {
                    perf::countEncodeDrawTextureSamplerFragmentDirectSamplerSetCalls(1u);
                  }
                  PerfScope fragmentDirectSamplerSetScope(
                      textureSamplerDirectSplitPerf
                          ? perf::countEncodeDrawTextureSamplerFragmentDirectSamplerSetCpuTime
                          : nullptr);
                  recordedSetFragmentSamplerState(ctx, encoder, binding.sampler,
                                                  static_cast<std::uint8_t>(binding.stage));
                }
                countSamplerBind();
              }
            }
          }
        }
      }
    }
    // D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3): the per-sampler mip LOD bias is
    // applied at sample time in MSL via `texture.sample(..., bias(b))` —
    // Metal samplers have no LOD-bias field. PSO-variant gated: the fragment
    // shader only declares `constant SamplerLodBias& samplerLodBias
    // [[buffer(4)]]` when the active variant's `samplerLodBias` bit is set, so
    // the slot-4 upload + bind happens on the SAME predicate the PSO key /
    // emitters read — anySamplerLodBiasNonzero(drawState). This keeps emit and
    // bind in lockstep: a declared-but-unbound reference is a Metal error, and
    // a bound-but-undeclared slot is wasted. The common no-bias draw skips both
    // the 32-byte upload and the bind entirely. Bound on the same direct
    // resource lane as textures/samplers, so it is consistent under the
    // argbuf-hybrid path too (textures/samplers also stay direct there).
    if (anySamplerLodBiasNonzero(drawState)) {
      PerfScope lodBiasScope(perf::countEncodeDrawTextureSamplerLodBiasCpuTime);
      perf::countEncodeDrawTextureSamplerLodBiasCalls(1u);
      SamplerLodBias lodBias = buildSamplerLodBias(drawState);
      auto slice = uploadTransientBuffer(&lodBias, sizeof(lodBias), alignof(SamplerLodBias));
      if (slice && !suppressRecordedMetalCalls(ctx)) {
        encoder.setFragmentBuffer(slice.buffer, slice.offset, 4);
        countUniformBufferBinds(1);
      }
    }
    if (!bindingPacket.vertexTextureSamplers.empty()) {
      struct ResolvedVertexTextureSamplerBinding {
        u32 stage = 0;
        core::Handle textureHandle{};
        u32 textureLod = 0;
        const resources::TextureRecord* textureRecord = nullptr;
        WMT::Texture texture{};
        u64 samplerStateHash = 0;
        core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
        WMT::Reference<WMT::SamplerState> samplerRef{};
        WMT::SamplerState sampler{};
      };
      std::array<ResolvedVertexTextureSamplerBinding, core::kMaxVertexTextureSamplers>
          resolvedVertexBindings{};
      std::size_t resolvedVertexBindingCount = 0;
      {
        PerfScope vertexResolveScope(
            perf::countEncodeDrawTextureSamplerVertexResolveCpuTime);
        perf::countEncodeDrawTextureSamplerVertexResolveCalls(1u);
        for (const auto& binding : bindingPacket.vertexTextureSamplers) {
          const auto stage = binding.stage;
          const auto textureHandle = binding.texture;
          auto& resolved = resolvedVertexBindings[resolvedVertexBindingCount++];
          resolved.stage = stage;
          resolved.textureHandle = textureHandle;
          resolved.textureLod = binding.textureLod;
          resolved.samplerStateHash = binding.samplerStateHash;
          resolved.samplerStates = binding.samplerStates;
          if (auto* texture = ctx.pool.findTexture(textureHandle.value); texture && texture->texture) {
            const u32 textureSlot = core::kVertexTextureSampler0 + stage;
            const bool srgbTexture =
                core::flatStateOr(hot.samplerStates[textureSlot], core::SAMP_SRGB_TEXTURE, 0u) != 0;
            resolved.textureRecord = texture;
            resolved.texture = resources::textureForShaderRead(*texture, srgbTexture);
          }
          if (suppressBaseStateLookup(ctx)) {
            resolved.sampler = ctx.drawRecorder->fragmentSamplerState;
          } else {
            resolved.samplerRef = ctx.cache.samplerStateFor(
                ctx.device, binding.samplerStates,
                static_cast<float>(binding.textureLod),
                dxmt9::shaders::argbufResourceArrayEnabled());
            resolved.sampler = WMT::SamplerState{resolved.samplerRef.handle};
          }
        }
      }

      {
        PerfScope vertexDirectScope(
            perf::countEncodeDrawTextureSamplerVertexDirectCpuTime);
        perf::countEncodeDrawTextureSamplerVertexDirectCalls(1u);
        for (std::size_t i = 0; i < resolvedVertexBindingCount; ++i) {
          const auto& binding = resolvedVertexBindings[i];
          if (binding.texture) {
            bool skipTextureBind = false;
            if (textureSamplerShadow && binding.stage < core::kMaxVertexTextureSamplers) {
              auto& slot = textureSamplerShadow->vertexTextures[binding.stage];
              const auto hash = textureSamplerShadowHash(
                  kVertexTextureShadowTag,
                  static_cast<std::uint8_t>(binding.stage),
                  binding.texture.handle);
              skipTextureBind = textureSamplerShadowMatches(
                  slot, hash, binding.texture.handle);
              if (!skipTextureBind) {
                textureSamplerShadowStore(slot, hash, binding.texture.handle);
              }
            }
            if (skipTextureBind) {
              countTextureBindSkipped();
            } else {
              if ((traceEncode || debug::shouldTraceTexture(binding.textureHandle)) &&
                  binding.textureRecord) {
                std::ostringstream out;
                out << "[dxmt9-texture] bind vertex stage=" << binding.stage
                    << " handle=0x" << std::hex << binding.textureHandle.value << std::dec
                    << " format=" << static_cast<unsigned>(binding.textureRecord->desc.format)
                    << " size=" << binding.textureRecord->desc.width << "x"
                    << binding.textureRecord->desc.height
                    << " levels=" << binding.textureRecord->desc.levels
                    << " lod=" << binding.textureLod;
                emitTextureTraceLine(out.str());
              }
              recordedSetVertexTexture(ctx, encoder, binding.texture,
                                       static_cast<std::uint8_t>(binding.stage));
              countTextureBind();
            }
          }
          if (binding.sampler) {
            bool skipSamplerBind = false;
            const auto samplerHash = samplerBindShadowHash(
                kVertexSamplerShadowTag,
                static_cast<std::uint8_t>(binding.stage),
                binding.samplerStateHash,
                binding.textureLod,
                samplerSupportsArgumentBuffers);
            if (textureSamplerShadow && binding.stage < core::kMaxVertexTextureSamplers) {
              auto& slot = textureSamplerShadow->vertexSamplers[binding.stage];
              skipSamplerBind = samplerBindShadowMatches(
                  slot, samplerHash, binding.samplerStates, binding.textureLod,
                  samplerSupportsArgumentBuffers);
              if (!skipSamplerBind) {
                samplerBindShadowStore(
                    slot, samplerHash, binding.samplerStates, binding.textureLod,
                    samplerSupportsArgumentBuffers, binding.sampler.handle);
              }
            }
            if (skipSamplerBind) {
              countSamplerBindSkipped();
            } else {
              recordedSetVertexSamplerState(ctx, encoder, binding.sampler,
                                            static_cast<std::uint8_t>(binding.stage));
              countSamplerBind();
            }
          }
        }
      }
    }
  }
  if (traceEncode) {
    std::ostringstream out;
    out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
        << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
        << " draw rt0=" << static_cast<unsigned long long>(hot.colorAttachments[0].handle.value)
        << " ds=" << static_cast<unsigned long long>(hot.depthStencil.handle.value)
        << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
        << " tex1=" << static_cast<unsigned long long>(hot.textures[1].value)
        << " tex2=" << static_cast<unsigned long long>(hot.textures[2].value)
        << " tex3=" << static_cast<unsigned long long>(hot.textures[3].value)
        << " tex4=" << static_cast<unsigned long long>(hot.textures[4].value)
        << " tex5=" << static_cast<unsigned long long>(hot.textures[5].value)
        << " textureMask=0x" << std::hex << hot.textureMask << std::dec
        << " vsHash=" << static_cast<unsigned long long>(drawState.shaderContext().vertexShader.hash)
        << " psHash=" << static_cast<unsigned long long>(drawState.shaderContext().pixelShader.hash)
        << " ffLayout=" << (ffLayout ? 1 : 0)
        << " preT=" << (preTransformed ? 1 : 0)
        << " indexed=" << (indexedDraw ? 1 : 0)
        << " primType=" << static_cast<unsigned>(pv.primitiveType)
        << " primCount=" << pv.primitiveCount
        << " vertexCount=" << static_cast<unsigned long long>(vertexCount)
        << " vertexStreamStride=" << drawVertexStreamStride
        << " vertexBufferOffset=" << vertexBufferOffset
        << " vertexStreamOffset=" << drawVertexStreamOffset
        << " vertexBaseIndex=" << drawVertexBaseIndex
        << " colorWrite="
        << core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu)
        << " zEnable=" << core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u)
        << " zWrite=" << core::flatStateOr(hot.renderStates, RS_Z_WRITE_ENABLE, 0u)
        << " zFunc=" << core::flatStateOr(hot.renderStates, RS_Z_FUNC, 0u)
        << " cullState=" << cullState
        << " cullRequested=" << static_cast<unsigned>(requestedCullMode)
        << " cullEffective=" << static_cast<unsigned>(effectiveCullMode)
        << " scissor=" << (effectiveViewport.scissorEnabled ? 1 : 0)
        << " scissorRect=" << effectiveViewport.scissor.left << ","
        << effectiveViewport.scissor.top << "-" << effectiveViewport.scissor.right
        << "," << effectiveViewport.scissor.bottom
        << " alphaBlend="
        << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
        << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
        << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
        << " forceVisible=" << (debug::forceVisibleDraw() ? 1 : 0);
    emitQueueTraceLine(out.str());
  }
  // H228 — per-draw fragment alpha-test immediate (fragment buffer 5,
  // shadowed per render encoder). Run/batch draws carrying a per-draw alpha
  // override use its raw trio; canonical draws resolve the trio from the
  // shared flat render state. Both funnel through state::makeFsVolatile so
  // the conversion is single-sourced with the FfpPsConsts upload. Pushed once
  // per draw ahead of both the indexed and non-indexed issue paths below; the
  // shadow collapses redundant pushes on unchanged values. Harmless for PSOs
  // without the tail (debug strip / tile / fragmentless): binding bytes to an
  // unreferenced fragment slot is a Metal no-op.
  {
    state::FsVolatile fsVolatile = state::buildFsVolatile(drawState);
    if (paramBindingOverride && paramBindingOverride->alphaTestStateValid) {
      const auto alpha = state::makeFsVolatile(
          paramBindingOverride->alphaTestEnable,
          paramBindingOverride->alphaTestFunc,
          paramBindingOverride->alphaTestRef, fsVolatile.sampleMask);
      fsVolatile.alphaTest = alpha.alphaTest;
      fsVolatile.alphaRef = alpha.alphaRef;
    }
    bool skipFsVolatilePush = false;
    if (textureSamplerShadow) {
      auto& shadowSlot = textureSamplerShadow->fsVolatile;
      skipFsVolatilePush = shadowSlot.valid &&
                           shadowSlot.alphaTest == fsVolatile.alphaTest &&
                           shadowSlot.alphaRef == fsVolatile.alphaRef &&
                           shadowSlot.sampleMask == fsVolatile.sampleMask;
      if (!skipFsVolatilePush) {
        shadowSlot.valid = true;
        shadowSlot.alphaTest = fsVolatile.alphaTest;
        shadowSlot.alphaRef = fsVolatile.alphaRef;
        shadowSlot.sampleMask = fsVolatile.sampleMask;
      }
    }
    if (!skipFsVolatilePush) {
      recordedSetFragmentBytes(ctx, encoder, &fsVolatile,
                               sizeof(fsVolatile), 5);
    }
  }
  drawPhase.mark(perf::countEncodeDrawPhaseBaseStateCpuTime);
  if (indexedDraw) {
    const bool texture0R32FCube = [&] {
      if (!hot.textures[0]) {
        return false;
      }
      const auto* texture = ctx.pool.findTexture(hot.textures[0].value);
      return texture && texture->desc.format == core::Format::R32F &&
             texture->desc.type == core::TextureType::Cube;
    }();
    const bool autoExpandIndexed =
        shouldAutoExpandIndexedDraw(hot.renderStates,
                                    hot.textureMask,
                                    fixedFunctionPath,
                                    ffLayout.has_value(),
                                    texture0R32FCube);
    const bool probeForceExpandIndexedApplied =
        debug::probeForceExpandIndexed() &&
        indexedTriangleDraw &&
        forceExpandIndexedProbeRowMatches(encoderBreakdown) &&
        indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
        forceExpandIndexedProbeClassMatches(primitiveCount,
                                            hot.textureMask,
                                            hot.renderStates,
                                            effectiveViewport,
                                            fillMode);
    const bool forceExpandIndexed =
        debug::forceExpandIndexed() ||
        probeForceExpandIndexedApplied ||
        (autoExpandIndexed && !debug::disableAutoExpandIndexed());
    if (traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-expand-policy] seq=" << static_cast<unsigned long long>(seqId)
          << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
          << " auto=" << (autoExpandIndexed ? 1 : 0)
          << " env=" << (debug::forceExpandIndexed() ? 1 : 0)
          << " probe=" << (probeForceExpandIndexedApplied ? 1 : 0)
          << " disabled=" << (debug::disableAutoExpandIndexed() ? 1 : 0)
          << " active=" << (forceExpandIndexed ? 1 : 0)
          << " ff=" << (fixedFunctionPath ? 1 : 0)
          << " ffLayout=" << (ffLayout ? 1 : 0)
          << " r32fCube=" << (texture0R32FCube ? 1 : 0)
          << " textureMask=0x" << std::hex << hot.textureMask << std::dec
          << " alphaBlend=" << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
          << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
          << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
          << " primCount=" << pv.primitiveCount
          << " vertexCount=" << static_cast<unsigned long long>(vertexCount);
      emitQueueTraceLine(out.str());
    }
    if (forceExpandIndexed) {
      PerfScope fvfDecodeExpandedScope(perf::countEncodeDrawFvfDecodeCpuTime,
                            perf::countEncodeDrawFvfDecodeExpandedCpuTime);
      std::span<const u8> indexBytes;
      if (!pv.userIndexData.empty()) {
        indexBytes = pv.userIndexData;
      } else {
        auto* indexRecord = ctx.pool.findBuffer(hot.indexBuffer.value);
        if (const auto bytes = snapshotBufferBytes(indexSnapshot);
            !bytes.empty()) {
          indexBytes = bytes;
        } else if (indexRecord && !indexRecord->shadow.empty()) {
          indexBytes = indexRecord->shadow;
        } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
          indexBytes = std::span<const u8>(static_cast<const u8*>(indexRecord->contents),
                                           static_cast<std::size_t>(indexRecord->desc.size));
        }
      }
      const std::size_t stride = static_cast<std::size_t>(
          ffLayout ? (drawVertexStreamStride ? drawVertexStreamStride : ffLayout->stride)
                   : computeVertexDeclStride(vertexDecl));
      const std::size_t streamBase = static_cast<std::size_t>(hot.streamOffsets[0]);
      if (traceEncode) {
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
      }

      if (!vertexBytes.empty() && !indexBytes.empty() && stride != 0) {
        struct ExpandedExtraStream {
          u32 stream = 0;
          u32 metalSlot = 0;
          CommandQueue::TransientBufferSlice slice;
        };
        std::vector<ExpandedExtraStream> expandedExtraStreams;
        std::vector<u8> expandedVertices;
        auto resolveStreamBytes = [&](u32 stream) -> std::span<const u8> {
          if (const auto bytes =
                  snapshotBufferBytes(streamBindingSnapshot(bindingSnapshot, stream));
              !bytes.empty()) {
            return bytes;
          }
          if (hot.streamBuffers[stream]) {
            if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[stream].value);
                buffer) {
              if (!buffer->shadow.empty()) {
                return buffer->shadow;
              }
              if (buffer->contents) {
                return std::span<const u8>(static_cast<const u8*>(buffer->contents),
                                           static_cast<std::size_t>(buffer->desc.size));
              }
            }
          }
          if (vertexDecl.streams[stream].buffer) {
            return vertexDecl.streams[stream].buffer->bytes();
          }
          return {};
        };
        bool expansionComplete =
            expandIndexedStreamToFlatVertexBytes(vertexBytes,
                                                 indexBytes,
                                                 pv.indexType,
                                                 pv.startIndex,
                                                 pv.baseVertexIndex,
                                                 vertexCount,
                                                 streamBase,
                                                 stride,
                                                 expandedVertices);
        if (expansionComplete) {
          for (const auto& streamBinding : bindingPacket.extraStreams) {
            if ((hot.streamFrequencies[streamBinding.stream] &
                 core::kStreamSourceInstanceData) != 0) {
              continue;
            }
            std::vector<u8> expandedStream;
            const auto sourceBytes = resolveStreamBytes(streamBinding.stream);
            if (!expandIndexedStreamToFlatVertexBytes(
                    sourceBytes,
                    indexBytes,
                    pv.indexType,
                    pv.startIndex,
                    pv.baseVertexIndex,
                    vertexCount,
                    static_cast<std::size_t>(hot.streamOffsets[streamBinding.stream]),
                    static_cast<std::size_t>(streamBinding.stride),
                    expandedStream)) {
              expansionComplete = false;
              break;
            }
            auto slice = makeTransientVertexBuffer(
                expandedStream.data(), expandedStream.size(),
                ActiveEncoderBreakdown::TransientVertexSource::ExpandedExtra);
            if (!slice) {
              expansionComplete = false;
              break;
            }
            expandedExtraStreams.push_back(ExpandedExtraStream{
                .stream = streamBinding.stream,
                .metalSlot = streamBinding.metalSlot,
                .slice = slice,
            });
          }
        }
        if (expansionComplete) {
          transientVertexBuffer = makeTransientVertexBuffer(
              expandedVertices.data(), expandedVertices.size(),
              ActiveEncoderBreakdown::TransientVertexSource::ExpandedMain);
        }
        if (transientVertexBuffer) {
          if (setVertexBufferCached(transientVertexBuffer.buffer,
                                    transientVertexBuffer.offset, 1)) {
            countVertexBufferBind();
            if (encoderBreakdown) {
              encoderBreakdown->recordStreamMetalBind(0);
            }
          }
          for (const auto& stream : expandedExtraStreams) {
            if (setVertexBufferCached(stream.slice.buffer, stream.slice.offset,
                                      stream.metalSlot)) {
              countVertexBufferBind();
              if (encoderBreakdown) {
                encoderBreakdown->recordStreamMetalBind(stream.stream);
              }
            }
          }
          if (ffLayout && ffLayout->preTransformed && vertexCount >= 6 && hot.textures[0]) {
            static const bool traceExpanded = [] {
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
          drawVertexStreamOffset = 0;
          drawVertexBaseIndex = 0;
          expandedIndexedDraw = true;
        }
      }

      if (traceEncode) {
        std::ostringstream resultTrace;
        resultTrace << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
                    << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
                    << " expanded=" << (expandedIndexedDraw ? 1 : 0);
        emitQueueTraceLine(resultTrace.str());
      }
    }
    const bool nativeBaseVertexRequested =
        indexedDraw && !expandedIndexedDraw && debug::useNativeMetalBaseVertex();
    const bool nativeBaseVertexUsed =
        nativeBaseVertexRequested && pv.baseVertexIndex >= 0;
    if (nativeBaseVertexUsed) {
      drawVertexBaseIndex = 0;
    }
    auto pushDrawVolatile = [&] {
      const DrawVolatile vol = buildDrawVolatile(
          drawVertexBaseIndex, drawVertexStreamOffset,
          drawVertexStreamStride, hot.streamFrequencies);
      recordedSetVertexBytes(ctx, encoder, &vol, sizeof(DrawVolatile), 5);
      perf::countUniformVolatilePush();
      if (encoderBreakdown) {
        encoderBreakdown->addSetVertexBytes(sizeof(DrawVolatile), 5);
      }
    };
    auto recordEncoderDrawIssue = [&](bool indexed, bool expanded) {
      if (!encoderBreakdown) {
        return;
      }
      encoderBreakdown->recordTileFfpCoverage(
          dxmt9::pipeline::classifyTileFfpForPass(
              drawState, ctx.pool.supportsApple3()),
          tileFfpMode,
          primitiveCount,
          vertexCount);
      encoderBreakdown->recordDrawIssue(
          pv.primitiveType,
          primitiveCount,
          vertexCount,
          indexed,
          expanded,
          fixedFunctionPath,
          preTransformed,
          hot.textureMask,
          drawVertexStreamStride,
          drawVertexBaseIndex,
          drawVertexStreamOffset,
          pv.baseVertexIndex,
          nativeBaseVertexRequested,
          nativeBaseVertexUsed,
          pv.startIndex,
          pv.indexType,
          hot.renderStates,
          effectiveViewport,
          effectiveCullMode,
          fillMode);
    };
    traceEffectDraw(encoderBreakdown,
                    hot,
                    ctx.pool,
                    seqId,
                    drawOrdinal,
                    commandIndex,
                    commandDrawIndex,
                    commandDrawCount,
                    pv.primitiveType,
                    primitiveCount,
                    vertexCount,
                    indexedDraw,
                    fixedFunctionPath,
                    preTransformed,
                    drawState.hasShaderContext()
                        ? drawState.shaderContext().vertexShader.hash
                        : 0u,
                    drawState.hasShaderContext()
                        ? drawState.shaderContext().pixelShader.hash
                        : 0u);
    if (expandedIndexedDraw) {
      if (encoderBreakdown && indexedDraw) {
        encoderBreakdown->recordIndexBufferState(hot.indexBuffer.value);
      }
      recordEncoderDrawIssue(true, true);
      recordDrawGeometryDiagnostics(drawState,
                                    pv,
                                    seqId,
                                    vertexCount,
                                    transientVertexBuffer.offset,
                                    drawVertexStreamOffset,
                                    drawVertexStreamStride,
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
      pushDrawVolatile();
      {
        const bool issueSplit = drawIssueSplitPerfEnabled();
        PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
        PerfScope issuePathScope(
            issueSplit ? perf::countEncodeDrawIssueExpandedIndexedCpuTime
                       : nullptr);
        std::optional<std::uint32_t> visibilityResult;
        if (visibilityScout) {
          PerfScope visibilityScope(
              issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                         : nullptr);
          visibilityResult = beginVisibilityScoutDraw(
              visibilityScout, encoder,
              makeVisibilityScoutDrawRecord(*visibilityScout, drawState,
                                            effectiveViewport, primitiveType,
                                            pv, drawOrdinal, commandIndex,
                                            primitiveCount, vertexCount,
                                            /*indexed=*/true,
                                            /*expandedIndexed=*/true,
                                            /*splitChunk=*/0,
                                            effectiveCullMode, fillMode));
        }
        {
          PerfScope metalScope(
              issueSplit ? perf::countEncodeDrawIssueMetalCpuTime : nullptr);
          recordedDrawPrimitives(ctx, encoder, primitiveType, 0,
                                 (uint64_t)vertexCount, pv.instanceCount, 0);
        }
        if (visibilityScout) {
          PerfScope visibilityScope(
              issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                         : nullptr);
          endVisibilityScoutDraw(visibilityScout, encoder, visibilityResult);
        }
      }
      // R-BACK-13.1: run the tile-FFP imageblock post-pass after this draw.
      emitTileFfpPostPass();
      return true;
    }
    CommandQueue::TransientBufferSlice transientIndexBuffer;
    WMT::Buffer indexBuffer{};
    const resources::BufferRecord* indexBufferRecord = nullptr;
    std::span<const u8> indexBytesForReuse;
    u32 indexReuseStartIndex = pv.startIndex;
    std::vector<u8> probeReorderedIndexBytes;
    uint64_t indexBufferOffset = static_cast<uint64_t>(pv.startIndex) * indexElementSize(pv.indexType);
    u32 splitPrimitiveLimit = 0u;
    u64 splitStream0SpanLimit = 0u;
    u32 splitMaxChunksPerDraw = 0u;
    std::vector<IndexedDrawChunk> splitChunks;
    u64 splitChunkStream0SpanMax = 0u;
    bool splitWouldApply = false;
    bool indexReorderApplied = false;
    const bool encoderBreakdownActive =
        encoderBreakdown && encoderBreakdown->enabled;
    const bool streamIbStagingEnabled =
        streamIbStagingActive(streamIbStagingCache);
    const bool indexedDiagnosticsEnabled =
        debug::indexedTriangleDiagnosticsEnabled() ||
        (effectDrawTraceEnabled() && effectDrawTraceGeometryEnabled());
    const bool needIndexBytesForDiagnostics =
        encoderBreakdownActive || indexedDiagnosticsEnabled;
    {
      PerfScope streamBindIndexScope(perf::countEncodeDrawStreamBindCpuTime,
                            perf::countEncodeDrawStreamBindIndexCpuTime);
      PerfScope streamBindIndexPhaseScope(
          streamBindPhaseSplitPerf
              ? perf::countEncodeDrawStreamBindIndexPhaseCpuTime
              : nullptr);
      PerfScope indexSetupScope(perf::countEncodeDrawIndexSetupCpuTime);
      perf::countEncodeDrawStreamBindIndexPhaseCalls(1u);
      {
        PerfScope indexSourceResolveScope(
            perf::countEncodeDrawIndexSourceResolveCpuTime);
        if (!pv.userIndexData.empty()) {
          indexBytesForReuse = pv.userIndexData;
          // Phase 5-B: prefer pre-batched UP index slice from DrawRun
          // bulk upload; fall back to per-draw upload otherwise.
          if (preUploaded && preUploaded->index) {
            transientIndexBuffer = preUploaded->index;
          } else {
            transientIndexBuffer = makeTransientIndexBuffer(
                pv.userIndexData.data(), pv.userIndexData.size(),
                ActiveEncoderBreakdown::TransientIndexSource::User);
          }
          if (transientIndexBuffer) {
            indexBuffer = transientIndexBuffer.buffer;
            indexBufferOffset += transientIndexBuffer.offset;
          }
        } else {
          auto* buffer = ctx.pool.findBuffer(hot.indexBuffer.value);
          indexBufferRecord = buffer;
          if (buffer && (buffer->buffer || (indexSnapshot && indexSnapshot->valid()))) {
            indexBuffer = WMT::Buffer{indexSnapshot
                ? indexSnapshot->metalHandle
                : buffer->buffer.handle};
            if (needIndexBytesForDiagnostics) {
              if (const auto bytes = snapshotBufferBytes(indexSnapshot);
                  !bytes.empty()) {
                indexBytesForReuse = bytes;
              } else if (!buffer->shadow.empty()) {
                indexBytesForReuse = buffer->shadow;
              } else if (buffer->contents) {
                indexBytesForReuse = std::span<const u8>(
                    static_cast<const u8*>(buffer->contents),
                    static_cast<std::size_t>(buffer->desc.size));
              }
            }
          } else if (buffer && !buffer->shadow.empty()) {
            indexBytesForReuse = buffer->shadow;
            transientIndexBuffer = makeTransientIndexBuffer(
                buffer->shadow.data(), buffer->shadow.size(),
                ActiveEncoderBreakdown::TransientIndexSource::ShadowFallback);
            if (transientIndexBuffer) {
              indexBuffer = transientIndexBuffer.buffer;
              indexBufferOffset += transientIndexBuffer.offset;
            }
          }
        }
      }
      const bool defaultIndexedFastPath =
          !needIndexBytesForDiagnostics && !streamIbStagingEnabled;
      if (defaultIndexedFastPath) {
        if (indexBuffer) {
          countIndexBufferBind();
        }
      } else {
      const std::span<const u8> originalIndexBytesForReuse = indexBytesForReuse;
      const u32 originalIndexReuseStartIndex = indexReuseStartIndex;
      const char* effectiveIndexSource = "original";
      u64 effectiveIndexOffset = indexBufferOffset;
      u64 effectiveIndexBytes = indexBytesForReuse.size();
      u64 effectiveIndexBufferHandle = hot.indexBuffer.value;
      const u64 reverseStream0SpanMin =
          debug::probeReverseIndexedTrianglesStream0SpanMin();
      const u64 optimizeStream0SpanMin =
          debug::optimizeScreenBlendIndexOrderStream0SpanMin();
      splitPrimitiveLimit = debug::splitLargeIndexedDrawPrimitiveLimit();
      splitStream0SpanLimit = debug::splitLargeIndexedDrawStream0SpanMax();
      splitMaxChunksPerDraw = debug::splitLargeIndexedDrawMaxChunksPerDraw();
      const bool triangleList =
          pv.primitiveType == core::PrimitiveType::TriangleList;
      // When a seq filter is active, keep expensive indexed diagnostics inside
      // the selected frame. Otherwise measurement can slow 3DMark05 enough to
      // change which semantic workload a frame/encoder row represents.
      const bool indexedDiagnosticSeqScopeActive =
          !perf::encoderBreakdownSeqFilterActive() || encoderBreakdownActive;
      const bool reverseAllIndexedTriangles = debug::probeReverseIndexedTriangles();
      const bool reverseOpaqueIndexedTriangles =
          debug::probeReverseOpaqueIndexedTriangles();
      const bool reverseNonOpaqueIndexedTriangles =
          debug::probeReverseNonOpaqueIndexedTriangles();
      const bool sortIndexedTrianglesByMinIndex =
          debug::probeSortIndexedTrianglesByMinIndex();
      const bool optimizeIndexedTrianglesVertexCache =
          debug::probeOptimizeIndexedTrianglesVertexCache();
      const bool applyIndexCacheOptCandidateProbe =
          debug::probeApplyIndexCacheOptCandidate() &&
          triangleList;
      const bool optimizeOpaqueDepthIndexCache =
          debug::optimizeOpaqueDepthIndexCache() &&
          triangleList;
      const bool optimizeScreenBlendIndexCache =
          debug::optimizeScreenBlendIndexCache() &&
          triangleList;
      const bool reverseTriangleProbeRequested =
          (reverseAllIndexedTriangles || reverseOpaqueIndexedTriangles ||
           reverseNonOpaqueIndexedTriangles || sortIndexedTrianglesByMinIndex ||
           optimizeIndexedTrianglesVertexCache ||
           applyIndexCacheOptCandidateProbe) &&
          triangleList;
      const bool reverseTriangleProbeScopeMatches =
          reverseTriangleProbeRequested &&
          indexedDiagnosticSeqScopeActive &&
          reverseIndexedTriangleRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown);
      const bool optimizeScreenBlendIndexOrderRequested =
          debug::optimizeScreenBlendIndexOrder() &&
          triangleList;
      const bool optimizeScreenBlendIndexOrderScopeMatches =
          optimizeScreenBlendIndexOrderRequested &&
          indexedDiagnosticSeqScopeActive &&
          screenBlendIndexOrderRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown);
      const bool splitConsidered =
          (splitPrimitiveLimit != 0u || splitStream0SpanLimit != 0u) &&
          triangleList &&
          indexedDiagnosticSeqScopeActive &&
          splitLargeIndexedDrawRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown);
      const bool dumpIndexedGeometryRequested =
          !debug::indexedGeometryDumpDir().empty() &&
          debug::indexedGeometryDumpMaxDraws() != 0u &&
          triangleList;
      const bool dumpIndexedGeometryScopeMatches =
          dumpIndexedGeometryRequested &&
          indexedDiagnosticSeqScopeActive &&
          reverseIndexedTriangleRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
          indexedGeometryDumpShaderMatches(drawState) &&
          indexedGeometryDumpTextureMatches(drawState, ctx.pool);
      const bool opaqueDepthWritingEligible =
          shouldOptimizeOpaqueDepthIndexOrder(
              hot.renderStates,
              fillMode,
              debug::probeDisableDepthWrite(),
              debug::optimizeOpaqueDepthIndexCacheExtendedScope());
      const bool applyProbeCacheOptCandidateSafetyEligible =
          opaqueDepthWritingEligible ||
          debug::probeApplyIndexCacheOptCandidateUnsafeNonOpaque();
      const bool optimizeOpaqueDepthIndexCacheScopeMatches =
          optimizeOpaqueDepthIndexCache &&
          opaqueDepthWritingEligible;
      const bool optimizeScreenBlendIndexCacheScopeMatches =
          optimizeScreenBlendIndexCache &&
          shouldOptimizeScreenBlendIndexOrder(hot.renderStates);
      const bool applyProbeCacheOptCandidateScopeMatches =
          applyIndexCacheOptCandidateProbe &&
          reverseTriangleProbeScopeMatches &&
          applyProbeCacheOptCandidateSafetyEligible;
      const bool stableOriginalIndexBufferForCandidate =
          isStableIndexCacheSource(
              pv.userIndexData.empty(),
              indexBufferRecord != nullptr,
              indexBufferRecord && static_cast<bool>(indexBufferRecord->buffer),
              indexSnapshot);
      const bool reverseTriangleClassEligibleNoSpan =
          indexedTriangleClassMatches(
              debug::probeReverseIndexedTrianglesClassFilter(),
              primitiveCount,
              hot.textureMask,
              hot.renderStates,
              effectiveViewport,
              fillMode) &&
          indexedTriangleClassMatches(
              debug::probeReverseIndexedTrianglesClassFilters(),
              primitiveCount,
              hot.textureMask,
              hot.renderStates,
              effectiveViewport,
              fillMode);
      const bool diagnosticCacheOptCandidatePreEligible =
          applyProbeCacheOptCandidateScopeMatches &&
          stableOriginalIndexBufferForCandidate &&
          reverseTriangleClassEligibleNoSpan;
      const bool productionCacheOptCandidatePreEligible =
          (optimizeOpaqueDepthIndexCacheScopeMatches ||
           optimizeScreenBlendIndexCacheScopeMatches) &&
          stableOriginalIndexBufferForCandidate;
      const bool applyCacheOptCandidatePreEligible =
          diagnosticCacheOptCandidatePreEligible ||
          productionCacheOptCandidatePreEligible;
      resources::ReorderedIndexBufferCacheKey cacheOptReorderKey{};
      if (stableOriginalIndexBufferForCandidate) {
        cacheOptReorderKey.sourceRevision =
            indexSnapshot ? indexSnapshot->contentRevision : 0u;
        cacheOptReorderKey.startIndex = originalIndexReuseStartIndex;
        cacheOptReorderKey.indexCount = vertexCount;
        cacheOptReorderKey.indexType = pv.indexType;
        cacheOptReorderKey.order = resources::ReorderedIndexOrder::VertexCacheLru32;
        cacheOptReorderKey.cacheSize = 32u;
      }
      resources::ReorderedIndexBufferLookup cacheOptPrelookup{};
      const bool cacheOptPrelookupEligible =
          (optimizeOpaqueDepthIndexCacheScopeMatches ||
           optimizeScreenBlendIndexCacheScopeMatches) &&
          stableOriginalIndexBufferForCandidate;
      if (cacheOptPrelookupEligible) {
        PerfScope indexCacheLookupScope(
            perf::countEncodeDrawIndexCacheLookupCpuTime);
        cacheOptPrelookup = ctx.queue.findReorderedIndexBuffer(
            hot.indexBuffer,
            cacheOptReorderKey,
            seqId);
      }
      const bool cacheOptPrelookupPositive =
          cacheOptPrelookup.hit && cacheOptPrelookup.buffer;
      const bool cacheOptPrelookupRejected =
          cacheOptPrelookup.hit && cacheOptPrelookup.rejected;
      if (cacheOptPrelookupEligible && cacheOptPrelookup.hit) {
        perf::countReorderedIndexCacheLookup(
            cacheOptPrelookupPositive,
            cacheOptPrelookupRejected,
            false,
            0u);
      }
      if (cacheOptPrelookupEligible && encoderBreakdown && cacheOptPrelookup.hit) {
        encoderBreakdown->recordReorderedIndexCacheLookup(
            cacheOptPrelookupPositive,
            cacheOptPrelookupRejected,
            false,
            0u);
      }
      const bool explicitMeasureCacheOptCandidate =
          debug::measureIndexCacheOptCandidate() && encoderBreakdownActive;
      const bool measureProductionCacheOptPrelookup =
          cacheOptPrelookupPositive &&
          encoderBreakdownActive &&
          perf::encoderBreakdownSeqFilterActive();
      const bool measureCacheOptCandidate =
          triangleList &&
          (explicitMeasureCacheOptCandidate ||
           measureProductionCacheOptPrelookup ||
           (applyCacheOptCandidatePreEligible &&
            !cacheOptPrelookupPositive &&
            !cacheOptPrelookupRejected));
      const bool cacheOptFullReuseMeasureRequired =
          explicitMeasureCacheOptCandidate ||
          measureProductionCacheOptPrelookup ||
          encoderBreakdownActive ||
          debug::measureIndexReuse() ||
          reverseTriangleProbeScopeMatches ||
          optimizeScreenBlendIndexOrderScopeMatches ||
          splitConsidered ||
          dumpIndexedGeometryScopeMatches;
      const bool cacheOptFastLru32Measure =
          measureCacheOptCandidate && !cacheOptFullReuseMeasureRequired;
      const bool measureProbeIndexLocality =
          (debug::measureIndexReuse() && encoderBreakdownActive) ||
          (reverseTriangleProbeScopeMatches && reverseStream0SpanMin != 0u) ||
          (optimizeScreenBlendIndexOrderScopeMatches && optimizeStream0SpanMin != 0u) ||
          (splitConsidered && splitStream0SpanLimit != 0u) ||
          dumpIndexedGeometryScopeMatches ||
          measureCacheOptCandidate;
      const u64 stream0StrideForProbe =
          encoderBreakdownActive ? encoderBreakdown->stats.streams[0].lastStride
                                 : static_cast<u64>(hot.streamStrides[0]);
      u32 cacheOptMinGainPct =
          debug::probeApplyIndexCacheOptCandidateMinGainPct();
      if (optimizeOpaqueDepthIndexCacheScopeMatches) {
        cacheOptMinGainPct =
            debug::optimizeOpaqueDepthIndexCacheMinGainPct();
      } else if (optimizeScreenBlendIndexCacheScopeMatches) {
        cacheOptMinGainPct =
            debug::optimizeScreenBlendIndexCacheMinGainPct();
      }
      IndexReuseMeasure originalIndexReuseForProbe{.references = vertexCount};
      if (measureProbeIndexLocality) {
        if (measureCacheOptCandidate) {
          PerfScope indexCacheCandidateScope(
              perf::countEncodeDrawIndexCacheCandidateCpuTime);
          PerfScope indexCacheOriginalMeasureScope(
              perf::countEncodeDrawIndexCacheOriginalMeasureCpuTime);
          originalIndexReuseForProbe = cacheOptFastLru32Measure
              ? measureIndexCacheMiss32ForDraw(originalIndexBytesForReuse,
                                               pv.indexType,
                                               originalIndexReuseStartIndex,
                                               vertexCount)
              : measureIndexReuseForDraw(originalIndexBytesForReuse,
                                         pv.indexType,
                                         originalIndexReuseStartIndex,
                                         vertexCount);
        } else {
          originalIndexReuseForProbe =
              measureIndexReuseForDraw(originalIndexBytesForReuse,
                                       pv.indexType,
                                       originalIndexReuseStartIndex,
                                       vertexCount);
        }
      }
      traceEffectIndexedGeometry(
          encoderBreakdown,
          drawState,
          ctx.pool,
          originalIndexBytesForReuse,
          vertexBytes,
          pv.indexType,
          originalIndexReuseStartIndex,
          vertexCount,
          pv.baseVertexIndex,
          hot.streamOffsets[0],
          drawVertexStreamStride,
          hot.streamBuffers[0].value,
          hot.indexBuffer.value,
          seqId,
          drawOrdinal,
          commandIndex,
          commandDrawIndex,
          commandDrawCount,
          pv.primitiveType,
          primitiveCount,
          fixedFunctionPath,
          preTransformed,
          drawState.hasShaderContext()
              ? drawState.shaderContext().vertexShader.hash
              : 0u,
          drawState.hasShaderContext()
              ? drawState.shaderContext().pixelShader.hash
              : 0u);
      std::vector<u8> cacheOptCandidateIndexBytes;
      IndexReuseMeasure cacheOptCandidateReuse{.references = vertexCount};
      bool cacheOptCandidateBuilt = false;
      bool cacheOptCandidateGatePassed = false;
      if (measureCacheOptCandidate) {
        PerfScope indexCacheCandidateScope(
            perf::countEncodeDrawIndexCacheCandidateCpuTime);
        if (originalIndexReuseForProbe.available) {
          PerfScope indexCacheCandidateBuildScope(
              perf::countEncodeDrawIndexCacheCandidateBuildCpuTime);
          cacheOptCandidateBuilt =
              buildVertexCacheOptimizedTriangleOrderIndexBytes(
                  originalIndexBytesForReuse,
                  pv.indexType,
                  originalIndexReuseStartIndex,
                  vertexCount,
                  cacheOptCandidateIndexBytes,
                  32u);
        }
        if (cacheOptCandidateBuilt) {
          PerfScope indexCacheCandidateMeasureScope(
              perf::countEncodeDrawIndexCacheCandidateMeasureCpuTime);
          cacheOptCandidateReuse = cacheOptFastLru32Measure
              ? measureIndexCacheMiss32ForDraw(cacheOptCandidateIndexBytes,
                                               pv.indexType,
                                               0,
                                               vertexCount)
              : measureIndexReuseForDraw(cacheOptCandidateIndexBytes,
                                         pv.indexType,
                                         0,
                                         vertexCount);
        }
        {
          PerfScope indexCacheGateScope(
              perf::countEncodeDrawIndexCacheGateCpuTime);
          cacheOptCandidateGatePassed =
              indexCacheCandidateMeetsGainGate(
                  originalIndexReuseForProbe,
                  cacheOptCandidateReuse,
                  cacheOptMinGainPct);
        }
        const bool cacheOptCandidateAvailable =
            originalIndexReuseForProbe.available && cacheOptCandidateReuse.available;
        perf::countIndexedCacheOptCandidate(
            cacheOptCandidateAvailable,
            static_cast<u64>(cacheOptCandidateIndexBytes.size()),
            originalIndexReuseForProbe.cacheMiss16,
            originalIndexReuseForProbe.cacheMiss32,
            originalIndexReuseForProbe.cacheMiss64,
            cacheOptCandidateReuse.cacheMiss16,
            cacheOptCandidateReuse.cacheMiss32,
            cacheOptCandidateReuse.cacheMiss64);
        if (cacheOptCandidateAvailable) {
          perf::countIndexedCacheOptCandidateGate(
              cacheOptCandidateGatePassed,
              primitiveCount,
              optimizeOpaqueDepthIndexCacheScopeMatches,
              optimizeScreenBlendIndexCacheScopeMatches);
        }
        if (encoderBreakdownActive) {
          encoderBreakdown->recordIndexedCacheOptCandidate(
              originalIndexReuseForProbe,
              cacheOptCandidateReuse,
              static_cast<u64>(cacheOptCandidateIndexBytes.size()));
          if (cacheOptCandidateAvailable) {
            encoderBreakdown->recordIndexedCacheOptCandidateGate(
                cacheOptCandidateGatePassed,
                primitiveCount,
                optimizeOpaqueDepthIndexCacheScopeMatches,
                optimizeScreenBlendIndexCacheScopeMatches);
          }
        }
        if ((optimizeOpaqueDepthIndexCacheScopeMatches ||
             optimizeScreenBlendIndexCacheScopeMatches) &&
            applyCacheOptCandidatePreEligible &&
            !explicitMeasureCacheOptCandidate &&
            !cacheOptCandidateGatePassed) {
          ctx.queue.rememberRejectedReorderedIndexBuffer(
              hot.indexBuffer,
              cacheOptReorderKey,
              seqId);
          perf::countReorderedIndexCacheLookup(false, false, false, 0u);
          if (encoderBreakdown) {
            encoderBreakdown->recordReorderedIndexCacheLookup(false, false, false, 0u);
          }
        }
      }
      auto stream0SpanFilterMatches = [&](u64 minSpan) {
        if (minSpan == 0u) {
          return true;
        }
        return stream0ByteSpanForIndexMeasure(originalIndexReuseForProbe,
                                              stream0StrideForProbe) >= minSpan;
      };
      bool probeConsidered = false;
      bool probeEligible = false;
      bool probeApplied = false;
      bool optimizedConsidered = false;
      bool optimizedEligible = false;
      bool optimizedApplied = false;
      const bool splitEligible =
          splitConsidered &&
          indexedTriangleClassMatches(
              debug::splitLargeIndexedDrawClassFilter(),
              primitiveCount,
              hot.textureMask,
              hot.renderStates,
              effectiveViewport,
              fillMode) &&
          indexedTriangleClassMatches(
              debug::splitLargeIndexedDrawClassFilters(),
              primitiveCount,
              hot.textureMask,
              hot.renderStates,
              effectiveViewport,
              fillMode);
      if (splitEligible &&
          buildIndexedDrawChunks(originalIndexBytesForReuse,
                                 pv.indexType,
                                 originalIndexReuseStartIndex,
                                 primitiveCount,
                                 splitPrimitiveLimit,
                                 stream0StrideForProbe,
                                 splitStream0SpanLimit,
                                 splitChunks)) {
        for (const auto& chunk : splitChunks) {
          splitChunkStream0SpanMax =
              std::max(splitChunkStream0SpanMax, chunk.stream0Span);
        }
        splitWouldApply =
            splitMaxChunksPerDraw == 0u ||
            splitChunks.size() <= static_cast<std::size_t>(splitMaxChunksPerDraw);
      }
      const bool triangleOrderMutationScopeMatches =
          reverseTriangleProbeScopeMatches ||
          optimizeOpaqueDepthIndexCacheScopeMatches ||
          optimizeScreenBlendIndexCacheScopeMatches;
      if (triangleOrderMutationScopeMatches) {
        probeConsidered = true;
        const bool classEligible =
            reverseTriangleClassEligibleNoSpan &&
            stream0SpanFilterMatches(reverseStream0SpanMin);
        const bool cacheOptCandidateClassEligible =
            productionCacheOptCandidatePreEligible ||
            (diagnosticCacheOptCandidatePreEligible && classEligible);
        const bool applyCacheOptCandidateEligible =
            cacheOptCandidateClassEligible &&
            (cacheOptPrelookupPositive ||
             (cacheOptCandidateBuilt &&
              cacheOptCandidateGatePassed));
        probeEligible =
            classEligible &&
            (applyCacheOptCandidateEligible ||
             (reverseTriangleProbeScopeMatches &&
              (optimizeIndexedTrianglesVertexCache ||
               sortIndexedTrianglesByMinIndex || reverseAllIndexedTriangles ||
               (reverseOpaqueIndexedTriangles && opaqueDepthWritingEligible) ||
               (reverseNonOpaqueIndexedTriangles && !opaqueDepthWritingEligible))));
        bool probeIndexBytesBuilt = false;
        if (probeEligible) {
          if (applyCacheOptCandidateEligible) {
            if (cacheOptPrelookupPositive) {
              indexBuffer = cacheOptPrelookup.buffer;
              indexBufferOffset = 0;
              effectiveIndexSource = "cached-reordered-prelookup";
              effectiveIndexOffset = 0;
              effectiveIndexBufferHandle = indexBuffer.handle;
              effectiveIndexBytes = cacheOptPrelookup.byteCount;
              probeApplied = true;
            } else {
              probeReorderedIndexBytes = cacheOptCandidateIndexBytes;
              probeIndexBytesBuilt = !probeReorderedIndexBytes.empty();
            }
          } else if (optimizeIndexedTrianglesVertexCache) {
            probeIndexBytesBuilt =
                buildVertexCacheOptimizedTriangleOrderIndexBytes(
                    indexBytesForReuse,
                    pv.indexType,
                    pv.startIndex,
                    vertexCount,
                    probeReorderedIndexBytes);
          } else if (sortIndexedTrianglesByMinIndex) {
            probeIndexBytesBuilt =
                buildMinIndexSortedTriangleOrderIndexBytes(
                    indexBytesForReuse,
                    pv.indexType,
                    pv.startIndex,
                    vertexCount,
                    probeReorderedIndexBytes);
          } else {
            probeIndexBytesBuilt =
                buildReverseTriangleOrderIndexBytes(indexBytesForReuse,
                                                    pv.indexType,
                                                    pv.startIndex,
                                                    vertexCount,
                                                    probeReorderedIndexBytes);
          }
        }
        if (probeIndexBytesBuilt) {
          if (applyCacheOptCandidateEligible) {
            PerfScope indexCacheApplyScope(
                perf::countEncodeDrawIndexCacheApplyCpuTime);
            const auto cached = ctx.queue.getOrCreateReorderedIndexBuffer(
                hot.indexBuffer,
                cacheOptReorderKey,
                std::span<const u8>(probeReorderedIndexBytes.data(),
                                    probeReorderedIndexBytes.size()),
                seqId);
            perf::countReorderedIndexCacheLookup(
                cached.hit,
                false,
                cached.created,
                cached.created ? cached.byteCount : 0u);
            if (encoderBreakdown) {
              encoderBreakdown->recordReorderedIndexCacheLookup(
                  cached.hit,
                  false,
                  cached.created,
                  cached.created ? cached.byteCount : 0u);
            }
            if (cached.buffer) {
              indexBuffer = cached.buffer;
              indexBufferOffset = 0;
              indexBytesForReuse =
                  std::span<const u8>(probeReorderedIndexBytes.data(),
                                      probeReorderedIndexBytes.size());
              indexReuseStartIndex = 0;
              effectiveIndexSource =
                  cached.created ? "cached-reordered-created"
                                 : "cached-reordered-hit";
              effectiveIndexOffset = 0;
              effectiveIndexBufferHandle = indexBuffer.handle;
              effectiveIndexBytes = cached.byteCount;
              probeApplied = true;
            }
          } else {
            transientIndexBuffer = makeTransientIndexBuffer(
                probeReorderedIndexBytes.data(), probeReorderedIndexBytes.size(),
                ActiveEncoderBreakdown::TransientIndexSource::ProbeReorder);
            if (transientIndexBuffer) {
              indexBuffer = transientIndexBuffer.buffer;
              indexBufferOffset = transientIndexBuffer.offset;
              indexBytesForReuse =
                  std::span<const u8>(probeReorderedIndexBytes.data(),
                                      probeReorderedIndexBytes.size());
              indexReuseStartIndex = 0;
              effectiveIndexSource = "transient-reordered";
              effectiveIndexOffset = transientIndexBuffer.offset;
              effectiveIndexBufferHandle = indexBuffer.handle;
              effectiveIndexBytes = probeReorderedIndexBytes.size();
              probeApplied = true;
            }
          }
        }
      }
      if (!probeApplied && optimizeScreenBlendIndexOrderScopeMatches) {
        optimizedConsidered = true;
        optimizedEligible =
            shouldOptimizeScreenBlendIndexOrder(hot.renderStates) &&
            indexedTriangleClassMatches(
                debug::optimizeScreenBlendIndexOrderClassFilter(),
                primitiveCount,
                hot.textureMask,
                hot.renderStates,
                effectiveViewport,
                fillMode) &&
            indexedTriangleClassMatches(
                debug::optimizeScreenBlendIndexOrderClassFilters(),
                primitiveCount,
                hot.textureMask,
                hot.renderStates,
                effectiveViewport,
                fillMode) &&
            stream0SpanFilterMatches(optimizeStream0SpanMin);
        if (optimizedEligible &&
            buildReverseTriangleOrderIndexBytes(indexBytesForReuse,
                                                pv.indexType,
                                                pv.startIndex,
                                                vertexCount,
                                                probeReorderedIndexBytes)) {
          transientIndexBuffer = makeTransientIndexBuffer(
              probeReorderedIndexBytes.data(), probeReorderedIndexBytes.size(),
              ActiveEncoderBreakdown::TransientIndexSource::OptimizedOrder);
          if (transientIndexBuffer) {
            indexBuffer = transientIndexBuffer.buffer;
            indexBufferOffset = transientIndexBuffer.offset;
            indexBytesForReuse = std::span<const u8>(probeReorderedIndexBytes.data(),
                                                     probeReorderedIndexBytes.size());
            indexReuseStartIndex = 0;
            effectiveIndexSource = "transient-optimized-order";
            effectiveIndexOffset = transientIndexBuffer.offset;
            effectiveIndexBufferHandle = indexBuffer.handle;
            effectiveIndexBytes = probeReorderedIndexBytes.size();
            optimizedApplied = true;
          }
        }
      }
      if (streamIbStagingEnabled &&
          !probeApplied &&
          !optimizedApplied &&
          pv.userIndexData.empty() &&
          indexBufferRecord &&
          indexBufferRecord->buffer &&
          indexBuffer) {
        if (auto staged = streamIbStagingCache->findOrStage(
                ctx, seqId, hot.indexBuffer.value, indexBufferRecord,
                encoderBreakdown, /*indexBuffer=*/true)) {
          indexBuffer = staged.buffer;
          indexBufferOffset = staged.offset +
                              static_cast<uint64_t>(pv.startIndex) *
                                  indexElementSize(pv.indexType);
          effectiveIndexSource = "staged-original";
          effectiveIndexOffset = indexBufferOffset;
          effectiveIndexBufferHandle = indexBuffer.handle;
          effectiveIndexBytes = indexBytesForReuse.size();
        }
      }
      const bool emitMeasureOnlyIndexedDraw =
          debug::measureIndexReuse() &&
          encoderBreakdownActive &&
          triangleList;
      bool dumpIndexedGeometryEligible = false;
      if (dumpIndexedGeometryScopeMatches) {
        dumpIndexedGeometryEligible =
            indexedTriangleClassMatches(
                debug::probeReverseIndexedTrianglesClassFilter(),
                primitiveCount,
                hot.textureMask,
                hot.renderStates,
                effectiveViewport,
                fillMode) &&
            indexedTriangleClassMatches(
                debug::probeReverseIndexedTrianglesClassFilters(),
                primitiveCount,
                hot.textureMask,
                hot.renderStates,
                effectiveViewport,
                fillMode) &&
            stream0SpanFilterMatches(reverseStream0SpanMin);
      }
      if (encoderBreakdownActive &&
          (probeConsidered || optimizedConsidered || scissorRectProbeConsidered ||
           splitConsidered || emitMeasureOnlyIndexedDraw ||
           dumpIndexedGeometryEligible)) {
        const auto& stream0 = encoderBreakdown->stats.streams[0];
        const u64 reorderedIndexByteCount =
            !probeReorderedIndexBytes.empty()
                ? static_cast<u64>(probeReorderedIndexBytes.size())
                : cacheOptPrelookupPositive ? cacheOptPrelookup.byteCount : 0u;
        const u64 reorderBytes =
            (probeApplied || optimizedApplied) ? reorderedIndexByteCount : 0u;
        const bool productionCacheOptOnly =
            (optimizeOpaqueDepthIndexCacheScopeMatches ||
             optimizeScreenBlendIndexCacheScopeMatches) &&
            !reverseTriangleProbeScopeMatches &&
            !explicitMeasureCacheOptCandidate &&
            !splitConsidered &&
            !emitMeasureOnlyIndexedDraw &&
            !dumpIndexedGeometryEligible &&
            !scissorRectProbeConsidered &&
            !optimizedConsidered;
        const bool emitIndexedProbeDrawLine =
            !productionCacheOptOnly || perf::encoderBreakdownSeqFilterActive();
        const auto originalIndexReuse =
            measureProbeIndexLocality ? originalIndexReuseForProbe
                                      : IndexReuseMeasure{.references = vertexCount};
        if (emitIndexedProbeDrawLine) {
          const bool useCacheOptCandidateReuseForEffective =
              probeApplied && cacheOptPrelookupPositive && cacheOptCandidateBuilt;
          const auto effectiveIndexReuse =
              useCacheOptCandidateReuseForEffective
                  ? cacheOptCandidateReuse
                  : measureProbeIndexLocality
                  ? measureIndexReuseForDraw(indexBytesForReuse,
                                             pv.indexType,
                                             indexReuseStartIndex,
                                             vertexCount)
                  : IndexReuseMeasure{.references = vertexCount};
          const auto streamExtraBindings =
              encoderBreakdown->streamExtraBindingsSummary();
          encoderBreakdown->emitIndexedOrderProbeDraw(
              probeEligible,
              probeApplied,
              optimizedEligible,
              optimizedApplied,
              scissorRectProbeEligible,
              scissorRectProbeApplied,
              disableAlphaBlendProbeApplied,
              disableDepthWriteProbeApplied,
              depthFuncAlwaysProbeApplied,
              fragmentlessDepthOnlyProbeApplied,
              splitEligible,
              splitWouldApply,
              static_cast<u32>(std::min<std::size_t>(
                  splitChunks.size(),
                  std::numeric_limits<u32>::max())),
              splitMaxChunksPerDraw,
              splitStream0SpanLimit,
              splitChunkStream0SpanMax,
              splitEligible ? static_cast<u64>(primitiveCount) : 0u,
              reorderBytes,
              originalIndexReuse,
              effectiveIndexReuse,
              cacheOptCandidateReuse,
              cacheOptCandidateBuilt,
              cacheOptCandidateGatePassed,
              drawOrdinal,
              commandIndex,
              pv.primitiveType,
              primitiveCount,
              vertexCount,
              hot.textureMask,
              hot.textures,
              hot.renderStates,
              effectiveViewport,
              effectiveCullMode,
              fillMode,
              pv.baseVertexIndex,
              pv.startIndex,
              pv.indexType,
              effectiveIndexBufferHandle,
              effectiveIndexSource,
              effectiveIndexOffset,
              effectiveIndexBytes,
              stream0.lastHandle,
              stream0.lastOffset,
              stream0.lastStride,
              streamExtraBindings.c_str(),
              hot.vertexConstantsHash,
              hot.pixelConstantsHash,
              drawState.hasUniformPayload() ? drawState.uniformPayload().hash : 0ull,
              hot.viewport.scissor);
        }
        if (probeConsidered) {
          encoderBreakdown->recordIndexedOrderProbe(
              probeApplied,
              probeApplied ? reorderedIndexByteCount : 0u);
        }
        if (optimizedConsidered) {
          encoderBreakdown->recordIndexedOrderOptimization(
              optimizedApplied,
              optimizedApplied ? static_cast<u64>(probeReorderedIndexBytes.size()) : 0u);
        }
        if (scissorRectProbeConsidered) {
          encoderBreakdown->recordScissorRectProbe(scissorRectProbeApplied,
                                                   hot.viewport.scissor,
                                                   effectiveViewport.scissor);
        }
        if (dumpIndexedGeometryEligible) {
          std::array<IndexedGeometryStreamPayload, core::kMaxStreams - 1u>
              dumpExtraStreams{};
          std::size_t dumpExtraStreamCount = 0u;
          auto resolveDumpStreamBytes = [&](u32 stream) -> std::span<const u8> {
            if (const auto bytes =
                    snapshotBufferBytes(streamBindingSnapshot(bindingSnapshot, stream));
                !bytes.empty()) {
              return bytes;
            }
            if (hot.streamBuffers[stream]) {
              if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[stream].value);
                  buffer) {
                if (!buffer->shadow.empty()) {
                  return buffer->shadow;
                }
                if (buffer->contents) {
                  return std::span<const u8>(
                      static_cast<const u8*>(buffer->contents),
                      static_cast<std::size_t>(buffer->desc.size));
                }
              }
            }
            if (vertexDecl.streams[stream].buffer) {
              return vertexDecl.streams[stream].buffer->bytes();
            }
            return {};
          };
          for (const auto& streamBinding : bindingPacket.extraStreams) {
            if (dumpExtraStreamCount >= dumpExtraStreams.size()) {
              break;
            }
            const auto streamBytes = resolveDumpStreamBytes(streamBinding.stream);
            if (streamBytes.empty()) {
              continue;
            }
            dumpExtraStreams[dumpExtraStreamCount++] = IndexedGeometryStreamPayload{
                .stream = streamBinding.stream,
                .metalSlot = streamBinding.metalSlot,
                .handle = hot.streamBuffers[streamBinding.stream].value,
                .offset = streamBinding.offset,
                .stride = streamBinding.stride,
                .bytes = streamBytes,
            };
          }
          std::optional<VsConsts> dumpVsConsts;
          std::optional<PsConsts> dumpPsConsts;
          std::optional<FfpPsConsts> dumpFfpPsConsts;
          std::span<const u8> dumpVsConstsBytes;
          std::span<const u8> dumpPsConstsBytes;
          std::span<const u8> dumpFfpVsConstsBytes;
          std::span<const u8> dumpFfpPsConstsBytes;
          if (debug::indexedGeometryDumpCbufs()) {
            dumpVsConsts = buildVsConsts(drawState);
            dumpPsConsts = buildPsConsts(drawState);
            dumpFfpPsConsts = buildFfpPsConsts(drawState);
            const auto* dumpFfpVsConsts = ensureFfpVs();
            dumpVsConstsBytes = std::span<const u8>(
                reinterpret_cast<const u8*>(&*dumpVsConsts),
                sizeof(VsConsts));
            dumpPsConstsBytes = std::span<const u8>(
                reinterpret_cast<const u8*>(&*dumpPsConsts),
                sizeof(PsConsts));
            dumpFfpVsConstsBytes = std::span<const u8>(
                reinterpret_cast<const u8*>(dumpFfpVsConsts),
                sizeof(FfpVsConsts));
            dumpFfpPsConstsBytes = std::span<const u8>(
                reinterpret_cast<const u8*>(&*dumpFfpPsConsts),
                sizeof(FfpPsConsts));
          }
          maybeDumpIndexedGeometryPayload(
              encoderBreakdown,
              drawState,
              ctx.pool,
              originalIndexBytesForReuse,
              vertexBytes,
              originalIndexReuse,
              pv.indexType,
              originalIndexReuseStartIndex,
              vertexCount,
              pv.baseVertexIndex,
              hot.streamOffsets[0],
              drawVertexStreamStride,
              hot.streamBuffers[0].value,
              hot.indexBuffer.value,
              drawState.hasShaderContext()
                  ? drawState.shaderContext().vertexShader.hash
                  : 0u,
              drawState.hasShaderContext()
                  ? drawState.shaderContext().pixelShader.hash
                  : 0u,
              drawOrdinal,
              primitiveCount,
              std::span<const IndexedGeometryStreamPayload>(
                  dumpExtraStreams.data(), dumpExtraStreamCount),
              dumpVsConstsBytes,
              dumpPsConstsBytes,
              dumpFfpVsConstsBytes,
              dumpFfpPsConstsBytes);
        }
      }
      indexReorderApplied = probeApplied || optimizedApplied;
      if (indexBuffer) {
        if (encoderBreakdown) {
          if (indexBufferRecord) {
            encoderBreakdown->recordIndexBufferResource(
                hot.indexBuffer.value, indexBufferRecord->desc);
          }
          encoderBreakdown->recordIndexBufferState(effectiveIndexBufferHandle);
        }
        countIndexBufferBind();
        if (encoderBreakdown) {
          encoderBreakdown->recordIndexBufferMetalBind();
        }
      }
      }
    }
    if (indexBuffer) {
      if (encoderBreakdown && encoderBreakdown->enabled &&
          debug::measureIndexReuse()) {
        encoderBreakdown->recordIndexedVertexReuse(
            measureIndexReuseForDraw(indexBytesForReuse,
                                     pv.indexType,
                                     indexReuseStartIndex,
                                     vertexCount));
      }
      const bool upDraw = !pv.userVertexData.empty() || !pv.userIndexData.empty();
      recordEncoderDrawIssue(true, false);
      recordDrawGeometryDiagnostics(drawState,
                                    pv,
                                    seqId,
                                    vertexCount,
                                    vertexBufferOffset,
                                    drawVertexStreamOffset,
                                    drawVertexStreamStride,
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
      pushDrawVolatile();
      {
        const bool issueSplit = drawIssueSplitPerfEnabled();
        PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
        const i32 metalBaseVertex = nativeBaseVertexUsed ? pv.baseVertexIndex : 0;
        const auto metalIndexType = toIndexType(pv.indexType);
        const bool submitSplitIndexed =
            splitWouldApply && !indexReorderApplied;
        PerfScope issuePathScope(
            issueSplit
                ? (submitSplitIndexed
                       ? perf::countEncodeDrawIssueSplitIndexedCpuTime
                       : perf::countEncodeDrawIssueIndexedCpuTime)
                : nullptr);
        if (submitSplitIndexed) {
          const u64 indexSize = indexElementSize(pv.indexType);
          for (const auto& chunk : splitChunks) {
            const u32 primitivesEmitted = chunk.startPrimitive;
            const u32 chunkPrimitives = chunk.primitiveCount;
            if (chunkPrimitives == 0u) {
              continue;
            }
            const u64 chunkIndexOffset =
                indexBufferOffset + static_cast<u64>(primitivesEmitted) * 3u * indexSize;
            std::optional<std::uint32_t> visibilityResult;
            if (visibilityScout) {
              PerfScope visibilityScope(
                  issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                             : nullptr);
              visibilityResult = beginVisibilityScoutDraw(
                  visibilityScout, encoder,
                  makeVisibilityScoutDrawRecord(*visibilityScout, drawState,
                                                effectiveViewport, primitiveType,
                                                pv, drawOrdinal, commandIndex,
                                                chunkPrimitives,
                                                static_cast<u64>(chunkPrimitives) * 3u,
                                                /*indexed=*/true,
                                                /*expandedIndexed=*/false,
                                                chunk.startPrimitive,
                                                effectiveCullMode, fillMode));
            }
            {
              PerfScope metalScope(
                  issueSplit ? perf::countEncodeDrawIssueMetalCpuTime
                             : nullptr);
              recordedDrawIndexedPrimitives(ctx, encoder, primitiveType,
                                            metalIndexType,
                                            static_cast<u64>(chunkPrimitives) * 3u,
                                            indexBuffer, chunkIndexOffset,
                                            pv.instanceCount,
                                            metalBaseVertex, 0);
            }
            if (visibilityScout) {
              PerfScope visibilityScope(
                  issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                             : nullptr);
              endVisibilityScoutDraw(visibilityScout, encoder, visibilityResult);
            }
          }
          if (encoderBreakdown) {
            encoderBreakdown->recordSplitLargeIndexedDraw(
                primitiveCount,
                splitPrimitiveLimit,
                splitStream0SpanLimit,
                splitChunkStream0SpanMax,
                static_cast<u32>(splitChunks.size()));
          }
        } else {
          std::optional<std::uint32_t> visibilityResult;
          if (visibilityScout) {
            PerfScope visibilityScope(
                issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                           : nullptr);
            visibilityResult = beginVisibilityScoutDraw(
                visibilityScout, encoder,
                makeVisibilityScoutDrawRecord(*visibilityScout, drawState,
                                              effectiveViewport, primitiveType,
                                              pv, drawOrdinal, commandIndex,
                                              primitiveCount, vertexCount,
                                              /*indexed=*/true,
                                              /*expandedIndexed=*/false,
                                              /*splitChunk=*/0,
                                              effectiveCullMode, fillMode));
          }
          {
            PerfScope metalScope(
                issueSplit ? perf::countEncodeDrawIssueMetalCpuTime : nullptr);
            recordedDrawIndexedPrimitives(ctx, encoder, primitiveType,
                                          metalIndexType,
                                          (uint64_t)vertexCount, indexBuffer,
                                          indexBufferOffset, pv.instanceCount,
                                          metalBaseVertex, 0);
          }
          if (visibilityScout) {
            PerfScope visibilityScope(
                issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                           : nullptr);
            endVisibilityScoutDraw(visibilityScout, encoder, visibilityResult);
          }
        }
      }
      // R-BACK-13.1: run the tile-FFP imageblock post-pass after this draw.
      emitTileFfpPostPass();
      return true;
    }
  }
  drawPhase.mark(perf::countEncodeDrawPhaseTileFfpFallthroughCpuTime);
  const bool upDraw = !pv.userVertexData.empty();
  if (encoderBreakdown) {
    encoderBreakdown->recordTileFfpCoverage(
        dxmt9::pipeline::classifyTileFfpForPass(
            drawState, ctx.pool.supportsApple3()),
        tileFfpMode,
        primitiveCount,
        vertexCount);
    encoderBreakdown->recordDrawIssue(
        pv.primitiveType,
        primitiveCount,
        vertexCount,
        false,
        false,
        fixedFunctionPath,
        preTransformed,
        hot.textureMask,
        drawVertexStreamStride,
        drawVertexBaseIndex,
        drawVertexStreamOffset,
        pv.baseVertexIndex,
        false,
        false,
        pv.startIndex,
        pv.indexType,
        hot.renderStates,
        effectiveViewport,
        effectiveCullMode,
        fillMode);
  }
  recordDrawGeometryDiagnostics(drawState,
                                pv,
                                seqId,
                                vertexCount,
                                vertexBufferOffset,
                                drawVertexStreamOffset,
                                drawVertexStreamStride,
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
  {
    const DrawVolatile vol = buildDrawVolatile(
        drawVertexBaseIndex, drawVertexStreamOffset,
        drawVertexStreamStride, hot.streamFrequencies);
    recordedSetVertexBytes(ctx, encoder, &vol, sizeof(DrawVolatile), 5);
    perf::countUniformVolatilePush();
    if (encoderBreakdown) {
      encoderBreakdown->addSetVertexBytes(sizeof(DrawVolatile), 5);
    }
    const bool issueSplit = drawIssueSplitPerfEnabled();
    PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
    PerfScope issuePathScope(
        issueSplit ? perf::countEncodeDrawIssueNonIndexedCpuTime : nullptr);
    std::optional<std::uint32_t> visibilityResult;
    if (visibilityScout) {
      PerfScope visibilityScope(
          issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime : nullptr);
      visibilityResult = beginVisibilityScoutDraw(
          visibilityScout, encoder,
          makeVisibilityScoutDrawRecord(*visibilityScout, drawState,
                                        effectiveViewport, primitiveType, pv,
                                        drawOrdinal, commandIndex,
                                        primitiveCount, vertexCount,
                                        /*indexed=*/false,
                                        /*expandedIndexed=*/false,
                                        /*splitChunk=*/0,
                                        effectiveCullMode, fillMode));
    }
    {
      PerfScope metalScope(
          issueSplit ? perf::countEncodeDrawIssueMetalCpuTime : nullptr);
      recordedDrawPrimitives(ctx, encoder, primitiveType, 0,
                             (uint64_t)vertexCount, pv.instanceCount, 0);
    }
    if (visibilityScout) {
      PerfScope visibilityScope(
          issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime : nullptr);
      endVisibilityScoutDraw(visibilityScout, encoder, visibilityResult);
    }
  }
  // R-BACK-13.1: run the tile-FFP imageblock post-pass after this draw.
  emitTileFfpPostPass();
  return true;
}

bool encodeDraw(EncodeContext& ctx,
                WMT::CommandBuffer& commandBuffer,
                WMT::RenderCommandEncoder& encoder,
                core::FlatDrawStateView drawState,
                u64 seqId,
                bool skipBaseStateBind,
                const PreUploadedDrawData* preUploaded,
                const core::DrawParam* paramOverride,
                std::span<const u8> paramPayloadArena,
                uniform::DirtyState* dirty,
                bool tileFfpMode,
                bool argbufHybridMode,
                bool argbufResourceArray,
                bool argbufDirectCbufMode,
                bool reopenArgbufHybrid,
                TextureSamplerBindShadow* textureSamplerShadow,
                std::uint32_t commandIndex,
                const core::DrawBindingSnapshot* bindingSnapshot,
                const core::DrawBindingOverride* paramBindingOverride) {
  return encodeDraw(ctx, commandBuffer, encoder, drawState, seqId,
                    skipBaseStateBind, preUploaded, paramOverride,
                    paramPayloadArena, paramBindingOverride,
                    bindingSnapshot, dirty, tileFfpMode,
                    argbufHybridMode, argbufResourceArray,
                    argbufDirectCbufMode, reopenArgbufHybrid,
                    /*argbufVsPayloadSourceChanged=*/false,
                    /*argbufPsPayloadSourceChanged=*/false,
                    /*bindingOverridePrefetchedPsoCompatible=*/false,
                    core::PsoHandle{}, core::PsoHandle{}, core::DepthStencilHandle{},
                    textureSamplerShadow, commandIndex, 0u, 0u, nullptr, nullptr,
                    nullptr, nullptr);
}

bool preRegisteredFragmentMatchesSessionSource(
    std::size_t slotIndex,
    std::uint64_t sourceSeqId,
    const EncodeChunkOptions& options) noexcept {
  if (!options.preRegisteredFragment.has_value()) {
    return true;
  }
  if (!options.session || options.sessionSource.has_value()) {
    return false;
  }

  const auto& fragment = *options.preRegisteredFragment;
  if (fragment.sourceFragmentCount == 0u ||
      fragment.sourceFragmentOrdinal >= fragment.sourceFragmentCount ||
      fragment.transactionFragmentCount == 0u ||
      fragment.transactionFragmentOrdinal >=
          fragment.transactionFragmentCount) {
    return false;
  }
  std::size_t matchCount = 0;
  for (const auto& source : encodeChunkSessionSources(*options.session)) {
    if (source.source != options.partitionSource ||
        source.slotIndex != slotIndex || source.seqId != sourceSeqId) {
      continue;
    }
    ++matchCount;
    if (fragment.commandBegin < source.commandBegin ||
        fragment.commandBegin - source.commandBegin > source.commandCount ||
        fragment.commandCount >
            source.commandCount -
                (fragment.commandBegin - source.commandBegin)) {
      return false;
    }
  }
  return matchCount == 1u;
}

bool encodeChunkAllowsRepeatedSourceFragments() noexcept {
  return !argbufPayloadDeltaSourcePerfEnabled();
}

namespace {

struct ActiveSeedMergeTicketAudit {
  ActiveSeedMergeTargetResolver resolver{};
  std::uint64_t mismatch = 0;
  bool active = false;

  ActiveSeedMergeTicketAudit(
      std::span<const ActiveSeedMergeTargetWitness> targets,
      bool attributionEnabled) noexcept
      : resolver{attributionEnabled
                     ? targets
                     : std::span<const ActiveSeedMergeTargetWitness>{}},
        active(attributionEnabled) {
    if (active) {
      perf::countActiveSeedMergeTicketIssued(targets.size());
    }
  }

  ~ActiveSeedMergeTicketAudit() {
    if (!active) {
      return;
    }
    if (mismatch != 0u) {
      perf::countActiveSeedMergeTicketMismatch(mismatch);
    }
    const std::uint64_t unconsumed = resolver.unconsumed();
    if (unconsumed != 0u) {
      perf::countActiveSeedMergeTicketUnconsumed(unconsumed);
    }
  }

  void beginCommand(std::uint32_t sourceIndex,
                    std::uint32_t commandIndex) noexcept {
    if (!active) {
      return;
    }
    DXMT_ASSERT(resolver.currentTarget() == nullptr);
    if (resolver.currentTarget() != nullptr) {
      ++mismatch;
    }
    resolver.beginCommand(sourceIndex, commandIndex);
  }

  const ActiveSeedMergeTargetWitness* currentTarget() const noexcept {
    return active ? resolver.currentTarget() : nullptr;
  }

  void endCommand() noexcept {
    if (active) {
      resolver.endCommand();
    }
  }

  void consume(ActiveSeedMergeJoinRelation relation) noexcept {
    DXMT_ASSERT(active && resolver.currentTarget() != nullptr);
    if (!active || !resolver.consumeCurrent()) {
      ++mismatch;
      return;
    }
    if (relation == ActiveSeedMergeJoinRelation::Matched) {
      perf::countActiveSeedMergeTicketMatched();
    } else {
      ++mismatch;
    }
  }

  void consumeContinuation(
      ActiveSeedMergeContinuationRelation relation) noexcept {
    DXMT_ASSERT(active && resolver.currentTarget() != nullptr);
    if (!active || !resolver.consumeCurrent()) {
      ++mismatch;
      return;
    }
    if (relation == ActiveSeedMergeContinuationRelation::Continued) {
      perf::countActiveSeedMergeTicketContinued();
    } else {
      ++mismatch;
    }
  }
};

}  // namespace

std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    core::SourcePayloadView payload,
    std::uint64_t sourceSeqId,
    EncodeChunkOptions options) {
  @autoreleasepool {
  PerfScope scope(perf::countEncodeChunkCpuTime);
  if (!ctx.device || !ctx.queue.valid()) {
    return std::nullopt;
  }
  // Session participation and validated replay-command plans are
  // source-kind-neutral. The plan is call-local and indexes the logical source
  // command stream, including segmented Arena sources.
  EncodeChunkReplayRange replayRange =
      encodeChunkReplayRange(slotIndex, payload, sourceSeqId, options);
  if (!replayRange.valid || !options.partitionSource.valid() ||
      (options.sessionSource.has_value() &&
       options.partitionSource != options.sessionSource->source) ||
      !preRegisteredFragmentMatchesSessionSource(
          slotIndex, sourceSeqId, options)) {
    return std::nullopt;
  }
  std::vector<std::size_t> replayOrdinalByCommandIndex;
  if (options.replayCommandPlanActive &&
      !options.replayCommandOrder.empty() &&
      options.replayCommandOrder.size() <= replayRange.commandCount()) {
    replayOrdinalByCommandIndex.resize(replayRange.commandCount());
  }
  EncodePartitionReplayStream partitionReplayStream{};
  partitionReplayStream = makeEncodePartitionReplayStream(
      slotIndex, payload, sourceSeqId, replayRange.commandBegin,
      replayRange.commandCount(), options.replayCommandPlanActive,
      options.replayCommandOrder, replayOrdinalByCommandIndex,
      options.partitionSource);
  if (!partitionReplayStream.valid) {
    return std::nullopt;
  }
  bool useExplicitPartitionPlan = false;
  if (!options.partitionRanges.empty()) {
    const auto validation = validateEncodePartitionRanges(
        options.partitionRanges, partitionReplayStream);
    useExplicitPartitionPlan = static_cast<bool>(validation);
  }

  const bool traceEncodeProgress = traceEncodeProgressForSeq(sourceSeqId);
  auto traceEncodeStage = [&](const char* stage) {
    if (!traceEncodeProgress) {
      return;
    }
    std::ostringstream out;
    out << "[dxmt9-encode-progress]"
        << " stage=" << stage
        << " seq=" << static_cast<unsigned long long>(sourceSeqId)
        << " slot=" << slotIndex
        << " commands=" << payload.commandCount()
        << " command_begin=" << replayRange.commandBegin
        << " command_end=" << replayRange.commandEnd
        << " command_plan=" << (options.replayCommandPlanActive ? 1 : 0)
        << " command_order=" << options.replayCommandOrder.size()
        << " draw_only=" << (payload.drawOnlyCommandStream() ? 1 : 0);
    emitQueueTraceLine(out.str());
  };
  auto traceEncodeCommand = [&](const char* phase,
                                std::size_t commandIndex,
                                core::MetalCommandKind kind,
                                const core::MetalCommandView& command) {
    if (!traceEncodeProgress) {
      return;
    }
    std::ostringstream out;
    out << "[dxmt9-encode-progress]"
        << " stage=command." << phase
        << " seq=" << static_cast<unsigned long long>(sourceSeqId)
        << " slot=" << slotIndex
        << " command=" << commandIndex
        << " kind=" << metalCommandKindName(kind);
    if (kind == core::MetalCommandKind::DrawRun) {
      out << " draws=" << command.drawParams.size();
      if (command.drawRunRecord) {
        out << " first_param=" << command.drawRunRecord->firstParam
            << " param_count=" << command.drawRunRecord->paramCount
            << " state_index=" << command.drawRunRecord->stateIndex;
      }
    } else if (kind == core::MetalCommandKind::Clear && command.clear) {
      const auto& clear = *command.clear;
      out << " clear_color=" << (clear.clearColor ? 1 : 0)
          << " clear_depth=" << (clear.clearDepth ? 1 : 0)
          << " clear_stencil=" << (clear.clearStencil ? 1 : 0)
          << " rects=" << clear.rects.size()
          << " rt=0x" << std::hex
          << static_cast<unsigned long long>(
                 clear.colorAttachments[0].handle.value)
          << " depth=0x"
          << static_cast<unsigned long long>(clear.depthStencil.handle.value)
          << std::dec;
    }
    emitQueueTraceLine(out.str());
  };

  traceEncodeStage("begin");

  const bool injectedCommandBuffer = options.hasInjectedCommandBuffer();
  const bool firstSourceFragment =
      !options.preRegisteredFragment.has_value() ||
      options.preRegisteredFragment->firstSourceFragment();
  const bool firstTransactionFragment =
      !options.preRegisteredFragment.has_value() ||
      options.preRegisteredFragment->firstTransactionFragment();
  if (options.session) {
    if (!options.session->storage ||
        (options.session->storage->commandBufferChainTail !=
             NULL_OBJECT_HANDLE &&
         (!injectedCommandBuffer ||
          options.session->storage->commandBufferChainTail !=
              options.commandBuffer.handle))) {
      traceEncodeStage("session-command-buffer-mismatch");
      return std::nullopt;
    }
  }

  // M3 — Metal frame capture: ask the controller whether this chunk is
  // the first chunk of the target frame. If so, start capture BEFORE
  // `newCommandBuffer()` so Apple's MTLCaptureManager records every CB
  // we create. Capture stays open across every chunk of the target
  // frame; `notePresentChunkForCapture` later returns the request when
  // the target frame's Present chunk is encoded, and that request is
  // attached to the record so the queue's commit closes the capture.
  encode_session::EncodeCallState call{};
  auto& captureAlreadyStartedAtChunkBegin =
      call.captureAlreadyStartedAtChunkBegin;
  std::optional<core::metalcapture::MetalCaptureRequest> earlyCaptureRequest;
  if (firstSourceFragment) {
    if (ctx.drawRecorder &&
        ctx.drawRecorder->beginSourceFragmentPreamble) {
      ctx.drawRecorder->beginSourceFragmentPreamble(
          ctx.drawRecorder->userdata, sourceSeqId);
    }
    earlyCaptureRequest = ctx.queue.metalCaptureForChunkBegin(sourceSeqId);
  }
  if (earlyCaptureRequest.has_value()) {
    traceEncodeStage("before-start-capture");
    captureAlreadyStartedAtChunkBegin =
        core::metalcapture::startMetalCapture(WMT::Device{ctx.device.handle},
                                               *earlyCaptureRequest);
    traceEncodeStage(captureAlreadyStartedAtChunkBegin
                         ? "after-start-capture-ok"
                         : "after-start-capture-failed");
  }

  traceEncodeStage(injectedCommandBuffer ? "before-use-injected-command-buffer"
                                         : "before-new-command-buffer");
  call.commandBuffer = injectedCommandBuffer
      ? std::move(options.commandBuffer)
      : ctx.queue.newCommandBuffer();
  auto& commandBuffer = call.commandBuffer;
  if (!commandBuffer) {
    traceEncodeStage(injectedCommandBuffer ? "injected-command-buffer-null"
                                           : "new-command-buffer-null");
    if (captureAlreadyStartedAtChunkBegin && earlyCaptureRequest.has_value()) {
      core::metalcapture::stopMetalCapture(*earlyCaptureRequest);
    }
    return std::nullopt;
  }
  traceEncodeStage(injectedCommandBuffer ? "after-use-injected-command-buffer"
                                         : "after-new-command-buffer");
  // One-shot storage for direct callers. The opt-in EncodeSession path below
  // supplies persistent storage so source boundaries can remain metadata-only.
  EncodeChunkSessionStorage localSession =
      encode_session::makeStorage(ctx.dirty);
  EncodeChunkSessionStorage& session =
      options.session ? *options.session->storage : localSession;
  if (options.session) {
    encode_session::initializeStorage(session, ctx.dirty);
    if (session.commandBufferChainTail == NULL_OBJECT_HANDLE) {
      session.commandBufferChainTail = commandBuffer.handle;
    }
    DXMT_ASSERT(session.commandBufferChainTail == commandBuffer.handle);
  }
  call.commandBufferHasWork = session.completion.tailCommandBufferHasWork;
  auto& commandBufferHasWork = call.commandBufferHasWork;
  const bool deferSessionFinalization =
      options.deferSessionFinalization && options.session != nullptr;
  auto& encoderState = session.encoder;
  auto& passState = session.pass;
  auto& bindingState = session.binding;
  auto& diagnosticsState = session.diagnostics;
  auto& completionState = session.completion;
  encode_session::LifecycleRuntime lifecycle(ctx, session, call,
                                             options.partitionSource,
                                             sourceSeqId,
                                             slotIndex);
  const bool activeSeedMergeAttributionEnabled =
      !options.activeSeedMergeTargets.empty() &&
      activeSeedMergeTicketAttributionEnabled(
          perf::enabled(), options.activeSeedMergeTicket,
          options.activeSeedMergeTargets.size());
  ActiveSeedMergeTicketAudit activeSeedMergeTicketAudit(
      options.activeSeedMergeTargets,
      activeSeedMergeAttributionEnabled);

  traceEncodeStage("before-gpu-sampling-setup");
  if (firstTransactionFragment) {
    encode_session::initializeGpuSamplingStorage(
        session, WMT::Device{ctx.device.handle},
        options.sessionLookaheadSources.empty()
            ? replayRange.commandCount()
            : encode_session::gpuSamplingCommandCount(
                  payload, sourceSeqId, replayRange.commandCount(),
                  options.sessionLookaheadSources));
  }
  traceEncodeStage("after-gpu-sampling-setup");

  auto makeRenderEncoderGpuAttachment = [&](
      core::metalqueue::RenderEncoderGpuPassType passType,
      std::size_t commandIndex,
      std::uint64_t rtHandle,
      std::uint64_t depthHandle,
      std::uint64_t psoHandle = 0) {
    return lifecycle.makeRenderEncoderGpuAttachment(
        passType, commandIndex, rtHandle, depthHandle, psoHandle);
  };
  auto recordRenderEncoderGpuAttachment =
      [&](const RenderEncoderGpuAttachment& attachment) {
        lifecycle.recordRenderEncoderGpuAttachment(attachment);
      };

  // Chunk's GPU seqId — feeds every transient-buffer reservation in this
  // chunk so the slab is retained until the matching command buffer
  // completes. R-BACK-12.24 argbuf populator threads this through to
  // `reserveTransientBuffer` / `uploadTransientBuffer`.
  const u64 encodeChunkSeqId = sourceSeqId;

  // R-BACK-12.22..12.26 — constants-only argbuf reopen gate. Tracks the
  // uniform payload hash last written into the active encoder's argbuf
  // descriptor table. A DrawRun whose payload matches reuses that table
  // (no fresh reservation, no rebind); a changed payload forces a fresh
  // table so draws can't observe last-write-wins on a shared table. Reset
  // whenever a new encoder opens (its argbuf table starts empty).
  auto hashArgbufPayloadComponentPrefix =
      [](const auto& values, u16 count) {
        const auto clamped =
            std::min<std::size_t>(count, values.size());
        const auto bytes = std::as_bytes(std::span(
            values.data(), clamped));
        u64 hash = drawBindingPacketHashMix(
            0x8fc6d3f8f0b19c45ull, static_cast<u64>(clamped));
        hash = drawBindingPacketHashMix(hash, core::hashBytes(bytes));
        return hash;
      };
  auto makeArgbufPayloadDeltaKey =
      [](core::FlatDrawStateView drawState) {
        const auto& payload = drawState.uniformPayload();
        return ArgbufPayloadDeltaKey{
            .hash = payload.hash,
            .vertexConstantsHash = drawStateVertexCbufSourceHash(drawState),
            .pixelConstantsHash = drawStatePixelCbufSourceHash(drawState),
        };
      };
  auto makeArgbufPayloadDeltaComponentKey =
      [&](const core::DrawUniformPayload& payload) {
        return ArgbufPayloadDeltaComponentKey{
            .vsFloatHash = hashArgbufPayloadComponentPrefix(
                payload.vsConst.float4, payload.vertexFloatConstantCount),
            .vsIntHash = hashArgbufPayloadComponentPrefix(
                payload.vsConst.int4, payload.vertexIntConstantCount),
            .vsBoolHash = hashArgbufPayloadComponentPrefix(
                payload.vsConst.bools, payload.vertexBoolConstantCount),
            .psFloatHash = hashArgbufPayloadComponentPrefix(
                payload.psConst.float4, payload.pixelFloatConstantCount),
            .psIntHash = hashArgbufPayloadComponentPrefix(
                payload.psConst.int4, payload.pixelIntConstantCount),
            .psBoolHash = hashArgbufPayloadComponentPrefix(
                payload.psConst.bools, payload.pixelBoolConstantCount),
        };
      };
  struct ArgbufPayloadChangedPrefixStats {
    u64 changed = 0;
    u64 prefix = 0;
    u64 span = 0;
    bool fullPrefix = false;
  };
  auto measureArgbufPayloadChangedPrefix =
      [](const auto& previousValues, u16 previousCount,
         const auto& currentValues, u16 currentCount) {
        const auto previousClamped =
            std::min<std::size_t>(previousCount, previousValues.size());
        const auto currentClamped =
            std::min<std::size_t>(currentCount, currentValues.size());
        const auto count = std::max(previousClamped, currentClamped);
        std::size_t firstChanged = count;
        std::size_t lastChanged = 0;
        for (std::size_t i = 0; i < count; ++i) {
          if (i >= previousClamped || i >= currentClamped ||
              std::memcmp(&previousValues[i], &currentValues[i],
                          sizeof(previousValues[i])) != 0) {
            firstChanged = std::min(firstChanged, i);
            lastChanged = i;
          }
        }
        if (firstChanged == count) {
          return ArgbufPayloadChangedPrefixStats{
              .changed = 0,
              .prefix = static_cast<u64>(count),
              .span = 0,
              .fullPrefix = false,
          };
        }
        const auto changed = lastChanged - firstChanged + 1u;
        u64 changedCount = 0;
        for (std::size_t i = firstChanged; i <= lastChanged; ++i) {
          if (i >= previousClamped || i >= currentClamped ||
              std::memcmp(&previousValues[i], &currentValues[i],
                          sizeof(previousValues[i])) != 0) {
            ++changedCount;
          }
        }
        return ArgbufPayloadChangedPrefixStats{
            .changed = changedCount,
            .prefix = static_cast<u64>(count),
            .span = static_cast<u64>(changed),
            .fullPrefix = changedCount == static_cast<u64>(count),
        };
      };
  auto recordArgbufPayloadChangedVsFloatRegBucket = [](u64 changedRegs) {
    if (changedRegs <= 1u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1Sum(
          changedRegs);
    } else if (changedRegs <= 4u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4Sum(
          changedRegs);
    } else if (changedRegs <= 16u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16Sum(
          changedRegs);
    } else if (changedRegs <= 64u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64Sum(
          changedRegs);
    } else {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64Sum(
          changedRegs);
    }
  };
  auto recordArgbufPayloadChangedPsFloatRegBucket = [](u64 changedRegs) {
    if (changedRegs <= 1u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1Sum(
          changedRegs);
    } else if (changedRegs <= 4u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4Sum(
          changedRegs);
    } else if (changedRegs <= 16u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16Sum(
          changedRegs);
    } else if (changedRegs <= 64u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64Sum(
          changedRegs);
    } else {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64Sum(
          changedRegs);
    }
  };
  struct ArgbufPayloadDeltaSourceBucket {
    bool valid = false;
    u64 vsHash = 0;
    u64 psHash = 0;
    u64 prefixRegs = 0;
    u64 rows = 0;
    u64 changedRegs = 0;
    u64 spanRegs = 0;
    u64 fullPrefixRows = 0;
    u64 fullPrefixRegs = 0;
  };
  struct ArgbufPayloadDeltaSourceAttribution {
    std::array<ArgbufPayloadDeltaSourceBucket, 128> buckets{};
    u64 overflowRows = 0;
    u64 overflowChangedRegs = 0;

    void record(core::FlatDrawStateView drawState,
                const ArgbufPayloadChangedPrefixStats& stats) noexcept {
      if (stats.changed == 0) {
        return;
      }
      u64 vsHash = 0;
      u64 psHash = 0;
      if (drawState.hasShaderContext()) {
        const auto& shader = drawState.shaderContext();
        vsHash = shader.vertexShader.hash;
        psHash = shader.pixelShader.hash;
      }
      ArgbufPayloadDeltaSourceBucket* target = nullptr;
      for (auto& bucket : buckets) {
        if (bucket.valid && bucket.vsHash == vsHash &&
            bucket.psHash == psHash && bucket.prefixRegs == stats.prefix) {
          target = &bucket;
          break;
        }
        if (!bucket.valid && !target) {
          target = &bucket;
        }
      }
      if (!target) {
        ++overflowRows;
        overflowChangedRegs += stats.changed;
        return;
      }
      if (!target->valid) {
        target->valid = true;
        target->vsHash = vsHash;
        target->psHash = psHash;
        target->prefixRegs = stats.prefix;
      }
      ++target->rows;
      target->changedRegs += stats.changed;
      target->spanRegs += stats.span;
      if (stats.fullPrefix) {
        ++target->fullPrefixRows;
        target->fullPrefixRegs += stats.changed;
      }
    }

    void emit(u64 seqId) const {
      if (overflowRows != 0 || overflowChangedRegs != 0) {
        std::fprintf(
            stderr,
            "[dxmt9-perf-argbuf-payload-delta-source seq=%llu "
            "overflow=1 rows=%llu changed_regs=%llu]\n",
            static_cast<unsigned long long>(seqId),
            static_cast<unsigned long long>(overflowRows),
            static_cast<unsigned long long>(overflowChangedRegs));
      }
      for (const auto& bucket : buckets) {
        if (!bucket.valid || bucket.rows == 0) {
          continue;
        }
        std::fprintf(
            stderr,
            "[dxmt9-perf-argbuf-payload-delta-source seq=%llu "
            "overflow=0 vs_hash=0x%llx ps_hash=0x%llx prefix_regs=%llu "
            "rows=%llu changed_regs=%llu span_regs=%llu "
            "full_prefix_rows=%llu full_prefix_regs=%llu]\n",
            static_cast<unsigned long long>(seqId),
            static_cast<unsigned long long>(bucket.vsHash),
            static_cast<unsigned long long>(bucket.psHash),
            static_cast<unsigned long long>(bucket.prefixRegs),
            static_cast<unsigned long long>(bucket.rows),
            static_cast<unsigned long long>(bucket.changedRegs),
            static_cast<unsigned long long>(bucket.spanRegs),
            static_cast<unsigned long long>(bucket.fullPrefixRows),
            static_cast<unsigned long long>(bucket.fullPrefixRegs));
      }
    }
  };
  const bool argbufPayloadDeltaSourcePerf =
      argbufPayloadDeltaSourcePerfEnabled();
  ArgbufPayloadDeltaSourceAttribution argbufPayloadDeltaSourceAttribution;
  RenderPassFrameTracker& renderPassFrameTracker =
      encodeThreadRenderPassFrameTracker();
  auto consumeActiveSeedMergeContinuation =
      [&](std::size_t commandIndex) noexcept {
        if (!activeSeedMergeAttributionEnabled) {
          return;
        }
        const auto* target = activeSeedMergeTicketAudit.currentTarget();
        if (!target) {
          return;
        }
        activeSeedMergeTicketAudit.consumeContinuation(
            classifyActiveSeedMergeContinuation(
                options.activeSeedMergeTicket, *target,
                options.replayWindow, options.replayWindow.sourceIndex,
                static_cast<std::uint32_t>(commandIndex),
                passState.activeInstance));
      };
  auto traceRenderPassProgress = [&](const char* stage,
                                     std::size_t commandIndex,
                                     bool hasClear) {
    if (!traceEncodeProgress) {
      return;
    }
    std::ostringstream out;
    out << "[dxmt9-encode-progress]"
        << " stage=renderpass." << stage
        << " seq=" << static_cast<unsigned long long>(sourceSeqId)
        << " slot=" << slotIndex
        << " command=" << commandIndex
        << " encoder=" << static_cast<unsigned long long>(diagnosticsState.renderEncoderIndex)
        << " clear=" << (hasClear ? 1 : 0);
    emitQueueTraceLine(out.str());
  };

  // TLA+: EncoderLifecycle variable binding:
  // activeKind  := encoderState.activeRenderEncoder ? "Render" : encoderState.activeBlitEncoder ? "Blit" : "None"
  // activeRT    := passState.activeKey while encoderState.activeRenderEncoder is live; NoRT otherwise.
  // hazardFlag  := exact overlap between current attachments and next draw reads, consumed immediately by a split.
  // opCount     := progress through the slot commandHeaders replay loop below.
  // The current blit helpers open and end short-lived encoders internally, so
  // encoderState.activeBlitEncoder is normally None but remains the local binding for a
  // future chunk-scoped blit encoder.
  auto assertEncoderLifecycleInvariant = [&] {
    lifecycle.assertEncoderLifecycleInvariant();
  };

  auto assertNoActiveEncoder = [&] {
    lifecycle.assertNoActiveEncoder();
  };

  auto flushRender = [&](perf::EncoderSplitReason reason = perf::EncoderSplitReason::Final) {
    lifecycle.endRender(reason);
  };

  auto flushBlit = [&] {
    lifecycle.endBlit();
  };

  auto startRenderPass = [&](core::FlatDrawStateView drawState,
                             const std::optional<core::ClearDesc>& clear,
                             std::size_t lookaheadStartIndex,
                             core::PsoHandle renderPsoHandle) {
    traceRenderPassProgress("begin", lookaheadStartIndex, clear.has_value());
    // TLA+: EncoderLifecycle / BeginRender(rt)
    // Callers split through None before opening a new render encoder.
    // R-BACK-15.7: pass the slot + current command index so beginRenderPass
    // can run the depth/stencil DontCare-store look-ahead over the
    // remaining records.
    assertNoActiveEncoder();
    const core::metalqueue::PublishedCommandRef passCommand =
        clear.has_value() && passState.pendingClearCommand.valid()
            ? passState.pendingClearCommand
            : lifecycle.commandRef(lookaheadStartIndex);
    const auto sampleAttachment = lifecycle.makeRenderEncoderGpuAttachment(
        core::metalqueue::RenderEncoderGpuPassType::Draw,
        passCommand,
        drawState.hot->colorAttachments[0].handle.value,
        drawState.hot->depthStencil.handle.value,
        psoHandleBucket(renderPsoHandle));
    const u64 openedRenderEncoderIndex = diagnosticsState.renderEncoderIndex;
    diagnosticsState.activeVisibilityScout =
        makeVisibilityScoutPass(ctx.device, encodeChunkSeqId,
                                openedRenderEncoderIndex);
    const WMT::Buffer visibilityBuffer{
        diagnosticsState.activeVisibilityScout ? diagnosticsState.activeVisibilityScout->buffer.handle
                              : NULL_OBJECT_HANDLE};
    traceRenderPassProgress("before-begin-render-pass", lookaheadStartIndex,
                            clear.has_value());
    const auto storeProofLookahead = makeRenderPassStoreProofLookaheadPlan({
        .slotIndex = slotIndex,
        .payload = payload,
        .sourceSeqId = sourceSeqId,
        .replayRange = replayRange,
        .sessionSource = options.sessionSource
            ? &*options.sessionSource
            : nullptr,
        .partitionSource = options.partitionSource,
        .retainedSources = options.sessionLookaheadSources,
        .replayCommandPlanActive = options.replayCommandPlanActive,
        .replayCommandOrder = options.replayCommandOrder,
        .replayOrdinalByCommandIndex = replayOrdinalByCommandIndex,
        .lookaheadStartIndex = lookaheadStartIndex,
    });
    const auto storeProofLookaheadSources = storeProofLookahead.view();
    if (ctx.drawRecorder &&
        ctx.drawRecorder->observeRenderPassStoreProofLookahead) {
      ctx.drawRecorder->observeRenderPassStoreProofLookahead(
          ctx.drawRecorder->userdata, lookaheadStartIndex,
          storeProofLookaheadSources.size());
    }
    // Portable passes can extend across consecutive compatible DrawRun
    // records. Let the store proof skip that same-pass prefix so it reasons
    // about the first use after the encoder actually closes. Tile-FFP can
    // resplit on a later eligibility change, so it keeps the conservative
    // one-record proof until that path has an equivalent suffix classifier.
    const auto tileFfpSelection =
        dxmt9::pipeline::selectTileFfpForPass(drawState,
                                               ctx.pool.supportsApple3());
    const RenderPassStoreProofActivePass storeProofActivePass{
        .hot = drawState.hot,
        .allowSameAttachmentContinuation =
            tileFfpSelection.decision !=
            dxmt9::pipeline::TileFfpDecision::Tile,
        .lookaheadMayHaveFutureSources = deferSessionFinalization,
        .lookaheadInvalid = storeProofLookahead.invalid,
        .lookaheadStorageTruncated = storeProofLookahead.storageTruncated,
    };
    RenderPassActionSummary renderPassActions{};
    if (suppressRecordedMetalCalls(ctx)) {
      passState.lateStore = {};
      if (ctx.drawRecorder->prepareLateRenderPassStoreState) {
        ctx.drawRecorder->prepareLateRenderPassStoreState(
            ctx.drawRecorder->userdata, passState.lateStore);
        renderPassActions = passState.lateStore.summary;
      }
      encoderState.activeRenderEncoder =
          WMT::Reference<WMT::RenderCommandEncoder>(
              ctx.drawRecorder->renderCommandEncoder);
    } else {
      encoderState.activeRenderEncoder = beginRenderPassWithStoreProofLookahead(
          ctx, commandBuffer, drawState, clear, storeProofLookaheadSources,
          storeProofActivePass, sampleAttachment.span(), visibilityBuffer,
          &renderPassActions, &passState.lateStore);
    }
    if (auto* recorder = ctx.drawRecorder;
        recorder && recorder->beginRenderPass) {
      recorder->beginRenderPass(recorder->userdata, lookaheadStartIndex);
    }
    traceRenderPassProgress(encoderState.activeRenderEncoder
                                ? "after-begin-render-pass-ok"
                                : "after-begin-render-pass-null",
                            lookaheadStartIndex, clear.has_value());
    encoderState.hasActiveRender = static_cast<bool>(encoderState.activeRenderEncoder);
    if (!encoderState.hasActiveRender) {
      diagnosticsState.activeVisibilityScout.reset();
      // No Metal encoder owns these provisional actions. Discard the copied
      // ledger before a later command can resolve/account stale Unknown state.
      passState.lateStore = {};
      renderPassActions = {};
    }
    if (encoderState.hasActiveRender) {
      for (std::size_t i = 0; i < passState.lateStore.count; ++i) {
        const auto& attachment = passState.lateStore.attachments[i];
        if (!attachment.unresolved()) {
          continue;
        }
        const auto aspect = attachment.aspect == LateRenderPassStoreAspect::Color
            ? perf::RenderPassLateStoreAspect::Color
            : attachment.aspect == LateRenderPassStoreAspect::Depth
                ? perf::RenderPassLateStoreAspect::Depth
                : perf::RenderPassLateStoreAspect::Stencil;
        perf::countRenderPassLateStoreUnknown(aspect);
      }
      passState.activeInstance = RenderPassInstanceToken{
          .seqId = encodeChunkSeqId,
          .encoderIndex = openedRenderEncoderIndex,
      };
      const ActiveSeedMergeTargetWitness* activeSeedMergeTarget = nullptr;
      auto activeSeedMergeJoin = ActiveSeedMergeJoinRelation::NotTarget;
      if (perf::enabled()) {
        const auto storeProof = renderPassStoreProofSummaryForLookahead(
            ctx, storeProofLookaheadSources, *drawState.hot,
            storeProofActivePass);
        activeSeedMergeTarget = activeSeedMergeAttributionEnabled
            ? activeSeedMergeTicketAudit.currentTarget()
            : nullptr;
        const bool openedWithClear = renderPassActions.color0Clear != 0u ||
            renderPassActions.depthClear != 0u ||
            renderPassActions.stencilClear != 0u;
        const std::uint64_t currentLoadBytes =
            renderPassActions.colorLoadBytes +
            renderPassActions.depthLoadBytes +
            renderPassActions.stencilLoadBytes;
        activeSeedMergeJoin = renderPassFrameTracker.noteStart(
            makeRenderPassFrameKey(*drawState.hot),
            estimateRenderPassAttachmentFootprintBytes(ctx, *drawState.hot),
            storeProof, openedWithClear, currentLoadBytes,
            encodeChunkSeqId,
            openedRenderEncoderIndex,
            options.replayWindow,
            options.replayWindow.sourceIndex,
            static_cast<std::uint32_t>(lookaheadStartIndex),
            options.activeSeedMergeTicket,
            activeSeedMergeTarget);
      }
      if (activeSeedMergeTarget) {
        activeSeedMergeTicketAudit.consume(activeSeedMergeJoin);
      }
      diagnosticsState.activeEncoderBreakdown.begin(
          encodeChunkSeqId, openedRenderEncoderIndex,
          drawState.hot->colorAttachments[0].handle.value,
          drawState.hot->depthStencil.handle.value);
      bindingState.activeStreamIbStaging.begin(
          stageStreamIbProbeRowMatches(&diagnosticsState.activeEncoderBreakdown));
      diagnosticsState.activeEncoderBreakdown.recordAttachmentMetadata(ctx.pool, *drawState.hot);
      ++diagnosticsState.renderEncoderIndex;
      recordRenderEncoderGpuAttachment(sampleAttachment);
    } else {
      bindingState.activeStreamIbStaging.begin(false);
    }
    passState.activeKey = makeAttachmentKey(*drawState.hot);
    passState.activeWriteHazard = makeAttachmentHazard(*drawState.hot);
    bindingState.activeDrawStateKey.reset();
    bindingState.activeDrawStateUsesPrefetchedPsoLayout = false;
    bindingState.textureSamplerShadow.reset();
    // R-BACK-13.1 — per-pass tile-shader FFP selector. Eligibility is
    // computed once at encoder open; the choice is sticky for the pass.
    // Counters: each opened pass bumps exactly one of
    // tileFfpPassCount / portableFfpPassCount, plus the by-reason
    // breakdown when the precision/unsupported_state path forced a
    // fallback. R-BACK-13.5: gpu_family is recorded but only via the
    // dedicated tileFfpFallbackGpuFamily counter, not the pass count.
    {
      const auto& selection = tileFfpSelection;
      passState.activePassUsesTileFfp =
          selection.decision == dxmt9::pipeline::TileFfpDecision::Tile;
      if (passState.activePassUsesTileFfp) {
        perf::countTileFfpPass();
      } else {
        perf::countPortableFfpPass();
        switch (selection.reason) {
          case dxmt9::pipeline::TileFfpFallbackReason::GpuFamily:
            perf::countTileFfpFallbackGpuFamily();
            break;
          case dxmt9::pipeline::TileFfpFallbackReason::Precision:
            perf::countTileFfpFallbackPrecision();
            break;
          case dxmt9::pipeline::TileFfpFallbackReason::UnsupportedState:
            perf::countTileFfpFallbackUnsupportedState();
            break;
          case dxmt9::pipeline::TileFfpFallbackReason::None:
          case dxmt9::pipeline::TileFfpFallbackReason::NotFfp:
            // No fallback class is bumped: NotFfp means the pass never
            // had an FFP key to translate, GpuFamily is its own counter,
            // None is the eligible case (already on tile path).
            break;
        }
      }
    }
    // R-BACK-15.4: capture color attachment handles so flushRender can
    // mark them touched on the queue once the encoder closes.
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      passState.activeColorHandles[i] = drawState.hot->colorAttachments[i].handle;
    }
    diagnosticsState.activeColorAttachmentDump = {};
    diagnosticsState.activeDepthAttachmentDump = {};
    diagnosticsState.activeDrawTextureDumps.clear();
    if (drawTextureDumpPassMatches(passState.activeInstance.seqId,
                                   passState.activeInstance.encoderIndex)) {
      diagnosticsState.activeDrawTextureDumps.reserve(
          drawTextureDumpReserveCapacity());
    }
    selectActiveDepthAttachmentDump(
        ctx.pool, ctx.limits, drawState,
        encoderState.activeRenderEncoder != nullptr,
        encodeChunkSeqId, openedRenderEncoderIndex,
        diagnosticsState.activeDepthAttachmentDump);
    selectActiveColorAttachmentDump(
        ctx.pool, ctx.limits, drawState,
        encoderState.activeRenderEncoder != nullptr,
        encodeChunkSeqId, openedRenderEncoderIndex,
        diagnosticsState.activeColorAttachmentDump);
    // M2: push a debug group identifying the render pass attachments.
    // Paired with the popDebugGroup() at the head of flushRender.
    //
    // Also set the encoder label with the same string. The debug group is
    // visible in Xcode's frame capture (.gputrace), but xctrace's
    // metal-application-encoders-list schema reports only the encoder
    // label, so without setLabel xctrace shows the Metal default
    // "Render Command N" and per-pass GPU time cannot be attributed to
    // an RT in text-based analysis.
    if (encoderState.activeRenderEncoder &&
        !suppressRecordedMetalCalls(ctx)) {
      traceRenderPassProgress("before-label", lookaheadStartIndex,
                              clear.has_value());
      const auto rt0 = static_cast<unsigned long long>(
          drawState.hot->colorAttachments[0].handle.value);
      const auto depth = static_cast<unsigned long long>(
          drawState.hot->depthStencil.handle.value);
      auto passLabel = makeLabelStringFmt(
          "RenderPass[seq=%llu,enc=%llu,rt=0x%llx,depth=0x%llx]",
          static_cast<unsigned long long>(encodeChunkSeqId),
          static_cast<unsigned long long>(openedRenderEncoderIndex), rt0,
          depth);
      encoderState.activeRenderEncoder.setLabel(passLabel);
      encoderState.activeRenderEncoder.pushDebugGroup(passLabel);
      traceRenderPassProgress("after-label", lookaheadStartIndex,
                              clear.has_value());
    }
    // R-BACK-12.22 / 12.24 / 12.25 — Stage 2 argbuf-hybrid per-encoder
    // populator. The selector reads the cached capability bool on the
    // pool. When the gate holds AND the queue-owned encoder resource
    // initialized successfully, the populator reserves the argbuf
    // storage from the transient ring, points the queue's
    // MTLArgumentEncoder at it, writes the four per-frequency cbuf
    // entries + the texture/sampler descriptors, and binds slot 30
    // (vertex + fragment) of the active render encoder. Stage 2 PSOs
    // read through this slot-30 argbuf; encodeDraw skips the direct
    // Stage 1 slot 0 / slot 3 and texture/sampler binds in this mode.
    //
    // When the gate fails (any non-Apple-Silicon device) `openArgbuf`
    // returns an empty handle and we fall through to the Stage 1
    // counter; no slot-30 bind is issued.
    bindingState.activePassUsesArgbufHybrid = false;
    bindingState.activePassUsesArgbufResourceArray = false;
    bindingState.activePassUsesArgbufDirectCbuf = false;
    {
      traceRenderPassProgress("before-argbuf-select", lookaheadStartIndex,
                              clear.has_value());
      const auto argbufDecision = dxmt9::pipeline::selectArgbufHybridForPass(
          drawState, ctx.pool.argbufHybridEnabled());
      traceRenderPassProgress("after-argbuf-select", lookaheadStartIndex,
                              clear.has_value());
      if (argbufDecision == dxmt9::pipeline::ArgbufHybridDecision::Stage2) {
        perf::countArgbufHybridEncoder();
        // R-BACK-12.22..12.26 (resource-array sub-mode) — pick the
        // resource-array encoder (20-entry table, larger encodedLength) when
        // the lane is active for the queue; otherwise the constants-only
        // encoder. Both anchor onto a fresh transient slab; the only delta is
        // the reservation size and whether texture/sampler slots are written.
        const bool resourceArrayLane = ctx.queue.resourceArrayLaneActive() &&
            ctx.queue.resourceArrayEncoderResource().initialized();
        const bool directCbufLane =
            !resourceArrayLane && dxmt9::pipeline::argbufDirectCbufEnabled();
        if (directCbufLane) {
          bindingState.activePassUsesArgbufHybrid = true;
          bindingState.activePassUsesArgbufDirectCbuf = true;
          traceRenderPassProgress("argbuf-direct-cbuf", lookaheadStartIndex,
                                  clear.has_value());
        } else {
          auto& encoderResource = resourceArrayLane
                                      ? ctx.queue.resourceArrayEncoderResource()
                                      : ctx.queue.argbufEncoderResource();
          traceRenderPassProgress("before-argbuf-open", lookaheadStartIndex,
                                  clear.has_value());
          const auto populated = dxmt9::argbuf_hybrid::openArgbufWithCompletedSeqId(
              ctx.queue, encoderResource, encodeChunkSeqId,
              ctx.transientCompletedSeqId);
          traceRenderPassProgress(populated ? "after-argbuf-open-ok"
                                            : "after-argbuf-open-empty",
                                  lookaheadStartIndex, clear.has_value());
          if (populated) {
            // Constant-buffer entries (VsConsts/PsConsts/FfpVsConsts/
            // FfpPsConsts) are populated lazily from encodeDraw's dirty
            // path on the first draw. Texture/sampler resources remain on
            // the direct fragment binding lane for texture-bound Stage 2
            // draws, so encoder open only binds the argbuf storage.
            // Bind slot 30 — vertex + fragment. The render encoder reads
            // from this single argbuf for the duration of the pass; the
            // slot-30 bind is the only argbuf-related bind on the encoder
            // (per spec.md §11.2; setVertexBytes(slot=5) / vertex stream
            // slot 1 stay direct).
            traceRenderPassProgress("before-argbuf-bind", lookaheadStartIndex,
                                    clear.has_value());
            encoderState.activeRenderEncoder.setVertexBuffer(populated.storage,
                                                populated.offset,
                                                dxmt9::shaders::kArgbufHybridBindSlot);
            encoderState.activeRenderEncoder.setFragmentBuffer(populated.storage,
                                                  populated.offset,
                                                  dxmt9::shaders::kArgbufHybridBindSlot);
            traceRenderPassProgress("after-argbuf-bind", lookaheadStartIndex,
                                    clear.has_value());
            // R-BACK-12.25 — upload accounting. `populated.length` is the
            // argbuf descriptor-table size (matches the encoder's reported
            // encodedLength); per-frequency cbuf bytes are bumped by
            // updateDirtyArgbufRegions on the first draw.
            perf::countArgbufHybridBytes(populated.length);
            diagnosticsState.activeEncoderBreakdown.addArgbufTableBytes(populated.length);
            bindingState.activePassUsesArgbufHybrid = true;
            bindingState.activePassUsesArgbufResourceArray = resourceArrayLane;
            bindingState.activePassUsesArgbufDirectCbuf = false;
          } else {
            // Selector chose Stage 2 but the encoder resource didn't init
            // (sentinel-null device, test fixture, or transient ring
            // exhaustion). R-BACK-12.22 sentence 2: never mid-pass switch
            // — the pass commits to Stage 1 for its lifetime. Fallback
            // counter bumps so a regression that turns this from "rare"
            // into "common" surfaces.
            perf::countArgbufHybridFallback();
          }
        }
      } else {
        perf::countStage1Encoder();
        // Stage 1 byte total so the regression test in spec.md §11.5
        // can compare Stage 2's expected savings. Bytes scale with the
        // four per-frequency UBOs the encoder may upload (worst-case,
        // dirty-mask all set on encoder open). Stage 2's counter bumps
        // with the argbuf encodedLength when the runtime activates
        // it; both remain comparable per-encoder.
        perf::countStage1Bytes(sizeof(VsConsts) + sizeof(PsConsts) +
                                sizeof(FfpVsConsts) + sizeof(FfpPsConsts));
      }
    }
    // R-BACK-12.12: a fresh Metal render encoder loses any prior
    // sticky bindings — every uniform category must rebind on the
    // first draw of the new encoder.
    uniform::markAllDirty(bindingState.uniformDirty);
    // The fresh encoder's argbuf table (opened above) is empty, so the
    // first draw of this pass must reopen + populate regardless of its
    // payload hash.
    bindingState.lastArgbufPayloadHash.reset();
    bindingState.lastArgbufPayloadDeltaKey.reset();
    bindingState.lastArgbufPayloadDeltaComponentKey.reset();
    bindingState.lastArgbufPayloadDeltaPayload.reset();
    bindingState.argbufCbufCache.reset();
    assertEncoderLifecycleInvariant();
    traceRenderPassProgress("end", lookaheadStartIndex, clear.has_value());
  };

  auto assertHelperEncoderPrecondition = [&] {
    // TLA+: EncoderLifecycle / BeginBlit
    // Blit-style helpers own any Metal encoder they open and end it before
    // returning; encodeChunk must have ended its active encoder first.
    assertNoActiveEncoder();
  };

  auto flushPendingClear = [&] {
    lifecycle.flushPendingClear();
  };

  auto finalizeEncodeChunkSessionForReturn = [&] {
    traceEncodeStage("before-final-flush-pending-clear");
    lifecycle.flushPendingClear();
    traceEncodeStage("after-final-flush-pending-clear");
    traceEncodeStage("before-final-flush-render");
    lifecycle.endRender(perf::EncoderSplitReason::Final);
    traceEncodeStage("after-final-flush-render");
    traceEncodeStage("before-final-flush-blit");
    lifecycle.endBlit();
    traceEncodeStage("after-final-flush-blit");
    traceEncodeStage("before-final-assert-no-active-encoder");
    lifecycle.assertNoActiveEncoder();
    traceEncodeStage("after-final-assert-no-active-encoder");
  };

  // Deferred-upload fence: flush any pending staging->private blits via
  // the queue-owned ResourceInitializer, then wait for its SharedEvent
  // signal before any draw samples those resources. A stale event value
  // from an earlier flush is not a new dependency for this command buffer;
  // waiting for it again would force-close a carried render encoder for no
  // resource-ordering benefit.
  // A pre-registered transaction has already proved that every represented
  // source is initializer-independent and that no upload was pending at its
  // queue preflight. Flush once before its first Metal effect. Uploads arriving
  // later belong to younger work outside the immutable transaction and must not
  // manufacture a pass boundary between its fragments.
  if (firstTransactionFragment) {
    if (ctx.drawRecorder && ctx.drawRecorder->beginTransactionPreamble) {
      ctx.drawRecorder->beginTransactionPreamble(ctx.drawRecorder->userdata);
    }
    traceEncodeStage("before-initializer-flush");
    const auto initializerFlush = ctx.queue.flushInitializerUploads();
    traceEncodeStage("after-initializer-flush");
    if (initializerFlush.didFlush &&
        initializerFlush.event &&
        initializerFlush.value > 0) {
      if (encoderState.activeRenderEncoder || encoderState.activeBlitEncoder || passState.pendingClear.has_value()) {
        traceEncodeStage("before-initializer-wait-finalize-session");
        finalizeEncodeChunkSessionForReturn();
        traceEncodeStage("after-initializer-wait-finalize-session");
      }
      traceEncodeStage("before-initializer-wait");
      commandBuffer.encodeWaitForEvent(initializerFlush.event, initializerFlush.value);
      traceEncodeStage("after-initializer-wait");
      commandBufferHasWork = true;
    }
  }

  // R-BACK-2.29..2.32 — mid-chunk MTLCommandBuffer split: open the next
  // sub-CB on the same queue, commit the current one (timing the commit),
  // swap, and reset the hasWork bit. CRITICAL invariants:
  //   * Must NEVER be called while an encoder is active. The natural call
  //     site after flushRender(non-Final) already satisfies this — flushRender
  //     ends the active render encoder. Helper-encoder paths
  //     (SurfaceCopy/StretchRect/Readback/ColorFill) own and end their own
  //     short-lived encoders, so calling splitMidChunk after they return is
  //     also safe. Callers must ensure flushBlit() has run if a blit encoder
  //     could be open.
  //   * Must NEVER be called between the present record's encoder open and
  //     the chain tail; the Present arm flushes before the tail commit and
  //     does not split there.
  // Sub-CB completion order is guaranteed by Metal's same-queue in-order
  // submission (R-BACK-2.32). Per-chunk commits (mid + final) are folded
  // into chunkSubCBCountMax via updateMax at chunk exit so the table
  // surfaces both total mid-chunk commits and the worst-case chain length.
  auto& perChunkSubCBCount = call.committedSubCommandBuffers;
  auto splitMidChunk = [&] {
    if ((injectedCommandBuffer &&
         !options.allowInjectedCommandBufferMidChunkCommits) ||
        !commandBufferHasWork) {
      return;
    }
    auto* recorder = ctx.drawRecorder;
    const bool testSplit = recorder && recorder->splitCommandBufferForTest;
    auto next = testSplit
        ? recorder->splitCommandBufferForTest(recorder->userdata,
                                              WMT::CommandBuffer{
                                                  commandBuffer.handle})
        : ctx.queue.newCommandBuffer();
    if (!next) return;
    const auto commitStarted = std::chrono::steady_clock::now();
    if (!testSplit) {
      commandBuffer.commit();
    }
    perf::countCommandBufferCommitCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - commitStarted).count()));
    perf::countSubCommandBufferCommit();
    ++perChunkSubCBCount;
    if (options.session) {
      ++session.completion.committedSubCommandBuffers;
    }
    commandBuffer = std::move(next);
    if (options.session) {
      session.commandBufferChainTail = commandBuffer.handle;
    }
    commandBufferHasWork = false;
  };

  const bool injectedCommandBufferCanSplit =
      !injectedCommandBuffer ||
      options.allowInjectedCommandBufferMidChunkCommits;
  const auto commitPolicy =
      options.disableMidChunkCommits || !injectedCommandBufferCanSplit
          ? MidChunkCommitPolicy::Off
          : midChunkCommitPolicy();
  const std::uint32_t splitNRecords = midChunkCommitNRecords();
  const std::uint32_t splitChainCap = midChunkCommitCapPerRenderPass();
  std::uint32_t recordsSinceLastSplit =
      session.completion.recordsSinceLastSplit;
  // R-BACK-2.33 — splitMidChunkUnderCap wraps splitMidChunk so callers
  // do not need to repeat the cap check at every split site. cap=0
  // disables the cap (unbounded chain) for diagnostic comparison.
  // perChunkSubCBCount counts mid-chunk commits issued by this encodeChunk
  // call. A carried EncodeSession also tracks completionState.committedSubCommandBuffers
  // across source boundaries so the cap applies to the logical coalesced
  // session rather than resetting for each source.
  auto splitMidChunkUnderCap = [&] {
    const std::uint64_t committedForCap =
        options.session ? session.completion.committedSubCommandBuffers
                        : perChunkSubCBCount;
    if (splitChainCap > 0 && committedForCap + 1 >= splitChainCap) {
      perf::countSubCommandBufferSplitSuppressedByCap();
      return;
    }
    splitMidChunk();
  };

  core::DrawUniformPayloadMaterializeCache paramUniformPayloadCache;

  using Kind = core::MetalCommandKind;
  auto encodeDrawRunCommand = [&](std::size_t commandIndex,
                                  const core::MetalCommandView& command,
                                  std::span<const EncodePartitionRangeSnapshot>
                                      drawPartitions) {
    paramUniformPayloadCache.reset();
    if (!command.drawState.hot || !command.drawState.shaderLayout ||
        !command.drawRunRecord ||
        core::drawRunDrawCount(command) == 0) {
      traceEncodeCommand("drawrun.skip-invalid", commandIndex, Kind::DrawRun,
                         command);
      return;
    }
    traceEncodeCommand("drawrun.enter", commandIndex, Kind::DrawRun, command);
    // The compact uniform materializer overwrites every field before returning
    // this scratch pointer; avoid a full DrawUniformPayload zero-fill here.
    traceEncodeCommand("drawrun.before-command-uniform", commandIndex,
                       Kind::DrawRun, command);
    core::DrawUniformPayload commandUniformScratch;
    const auto* commandUniformPayload = core::drawRunUniformPayloadForHandle(
        command, command.drawRunRecord->uniformHandle, commandUniformScratch,
        perf::DrawUniformPayloadMaterializeSite::DrawEncoderCommand);
    if (!commandUniformPayload) {
      traceEncodeCommand("drawrun.skip-no-command-uniform", commandIndex,
                         Kind::DrawRun, command);
      return;
    }
    traceEncodeCommand("drawrun.after-command-uniform", commandIndex,
                       Kind::DrawRun, command);
    if (activeSeedMergeAttributionEnabled) {
      activeSeedMergeTicketAudit.beginCommand(
          options.replayWindow.sourceIndex,
          static_cast<std::uint32_t>(commandIndex));
    }
    auto stateView = command.drawState;
    stateView.uniforms = commandUniformPayload;
    const auto& hot = *stateView.hot;
    const auto commandDrawItems =
        command.drawItems.empty() ? command.drawParams : command.drawItems;
    if (auto* recorder = ctx.drawRecorder;
        recorder && recorder->beginDrawRunCommand) {
      recorder->beginDrawRunCommand(
          recorder->userdata, commandIndex, commandDrawItems.size());
    }
    const core::PsoHandle renderPsoHandle =
        command.drawRunRecord ? command.drawRunRecord->renderPsoHandle
                              : core::PsoHandle{};
    const core::PsoHandle tilePsoHandle =
        command.drawRunRecord ? command.drawRunRecord->tilePsoHandle
                              : core::PsoHandle{};
    const core::DepthStencilHandle depthStencilHandle =
        command.drawRunRecord ? command.drawRunRecord->depthStencilHandle
                              : core::DepthStencilHandle{};
    // Compact draw-run: state bound from base ONCE (render-pass +
    // resource-binding decisions key off base.rts), then loop over
    // per-DrawParam emits. FlatDrawStateKey is the hot-path decision
    // object for skipping base-state rebinding across compatible
    // Draw/DrawRun records on the same Metal render encoder.
    traceEncodeCommand("drawrun.before-flush-blit", commandIndex,
                       Kind::DrawRun, command);
    flushBlit();
    traceEncodeCommand("drawrun.after-flush-blit", commandIndex,
                       Kind::DrawRun, command);
    assertEncoderLifecycleInvariant();
    const auto drawKey = makeAttachmentKey(hot);
    const auto drawReadHazard = makeDrawReadHazard(stateView);
    auto hasExactRenderHazard = [&] {
      const bool bloomOverlap = passState.activeWriteHazard.bloomOverlaps(drawReadHazard);
      const bool exactOverlap = passState.activeWriteHazard.exactOverlaps(drawReadHazard);
      perf::countHazardProbe(bloomOverlap, exactOverlap);
      return exactOverlap;
    };
    // R-BACK-13.6 — mid-pass eligibility. When the active encoder is on
    // the tile path and the next draw's state has become ineligible
    // (e.g. alpha-test reference flipped out of [0,1], fog mode flipped
    // to Exp/Exp2), force a render-pass split so the next encoder opens
    // on the portable path. A pass that opened on the portable path
    // stays portable regardless (portable handles every state).
    auto tileMidPassIneligible = [&]() {
      if (!encoderState.hasActiveRender || !passState.activePassUsesTileFfp) return false;
      const auto sel =
          dxmt9::pipeline::selectTileFfpForPass(stateView, ctx.pool.supportsApple3());
      return sel.decision != dxmt9::pipeline::TileFfpDecision::Tile;
    };
    if (passState.pendingClear.has_value()) {
      const auto clearKey = makeAttachmentKey(*passState.pendingClear);
      const auto clearHazard = makeAttachmentHazard(*passState.pendingClear);
      if (clearKey == drawKey && !clearHazard.exactOverlaps(drawReadHazard)) {
        startRenderPass(stateView, passState.pendingClear, commandIndex, renderPsoHandle);
        passState.pendingClear.reset();
        passState.pendingClearCommand = {};
      } else {
        flushPendingClear();
        const bool renderTargetChanged = encoderState.hasActiveRender && passState.activeKey != drawKey;
        const bool hazardDetected =
            encoderState.hasActiveRender && !renderTargetChanged && hasExactRenderHazard();
        const bool tileResplit =
            encoderState.hasActiveRender && !renderTargetChanged && !hazardDetected &&
            tileMidPassIneligible();
        if (tileResplit) {
          // R-BACK-13.6: tile path can't host this draw; fall back
          // to portable for a fresh encoder. The split is a real
          // change of pipeline kind (not a Bloom false positive),
          // so it does not violate R-BACK-2.28's no-false-positive
          // policy.
          perf::countTileFfpMidPassResplit();
          perf::countTileFfpFallbackMidPassIneligible();
        }
        const auto entryDecision = classifyRenderPassEntry(
            encoderState.hasActiveRender,
            !renderTargetChanged,
            hazardDetected,
            tileResplit);
        if (entryDecision != RenderPassEntryDecision::ContinueActive) {
          if (encoderState.hasActiveRender) {
            lifecycle.resolveLateStoreForDraw(stateView);
          }
          if (entryDecision ==
              RenderPassEntryDecision::SplitRenderTargetChange) {
            // TLA+: EncoderLifecycle / RenderTargetChange(newRT)
            DXMT_ASSERT(encoderState.hasActiveRender);
          }
          if (entryDecision == RenderPassEntryDecision::SplitHazard) {
            // TLA+: EncoderLifecycle / HazardDetected
            DXMT_ASSERT(encoderState.hasActiveRender);
            DXMT_ASSERT(passState.activeKey == drawKey);
          }
          const auto splitReason = renderPassEntrySplitReason(
              entryDecision, perf::EncoderSplitReason::ClearBarrier);
          flushRender(splitReason);
          // R-BACK-2.29..2.32 — per-render-pass policy commits the
          // current sub-CB at every non-Final flushRender. Encoder is
          // already ended by flushRender, so the splitMidChunk
          // invariant (no active encoder) holds. Skip when policy
          // is off so the default 1 CB/chunk behavior is preserved.
          if (commitPolicy == MidChunkCommitPolicy::PerRenderPass) {
            splitMidChunkUnderCap();
          }
          startRenderPass(stateView, std::nullopt, commandIndex, renderPsoHandle);
        } else {
          // TLA+: EncoderLifecycle / MergeRenderDraw(rt)
          DXMT_ASSERT(encoderState.hasActiveRender);
          DXMT_ASSERT(passState.activeKey == drawKey);
          DXMT_ASSERT(!passState.activeWriteHazard.exactOverlaps(drawReadHazard));
          consumeActiveSeedMergeContinuation(commandIndex);
        }
      }
    } else {
      const bool renderTargetChanged = encoderState.hasActiveRender && passState.activeKey != drawKey;
      const bool hazardDetected =
          encoderState.hasActiveRender && !renderTargetChanged && hasExactRenderHazard();
      const bool tileResplit =
          encoderState.hasActiveRender && !renderTargetChanged && !hazardDetected &&
          tileMidPassIneligible();
      if (tileResplit) {
        // R-BACK-13.6 — see twin call site above.
        perf::countTileFfpMidPassResplit();
        perf::countTileFfpFallbackMidPassIneligible();
      }
      const auto entryDecision = classifyRenderPassEntry(
          encoderState.hasActiveRender,
          !renderTargetChanged,
          hazardDetected,
          tileResplit);
      if (entryDecision != RenderPassEntryDecision::ContinueActive) {
        if (encoderState.hasActiveRender) {
          lifecycle.resolveLateStoreForDraw(stateView);
        }
        if (entryDecision ==
            RenderPassEntryDecision::SplitRenderTargetChange) {
          // TLA+: EncoderLifecycle / RenderTargetChange(newRT)
          DXMT_ASSERT(encoderState.hasActiveRender);
        }
        if (entryDecision == RenderPassEntryDecision::SplitHazard) {
          // TLA+: EncoderLifecycle / HazardDetected
          DXMT_ASSERT(encoderState.hasActiveRender);
          DXMT_ASSERT(passState.activeKey == drawKey);
        }
        const auto splitReason = renderPassEntrySplitReason(
            entryDecision, perf::EncoderSplitReason::Final);
        flushRender(splitReason);
        // R-BACK-2.29..2.32 — see twin call site above. The split
        // reason here can be Final when neither RT-change nor hazard
        // forced the flush, but per-render-pass policy still
        // commits to start a new sub-CB before the next pass opens.
        if (commitPolicy == MidChunkCommitPolicy::PerRenderPass) {
          splitMidChunkUnderCap();
        }
        startRenderPass(stateView, std::nullopt, commandIndex, renderPsoHandle);
      } else {
        // TLA+: EncoderLifecycle / MergeRenderDraw(rt)
        DXMT_ASSERT(encoderState.hasActiveRender);
        DXMT_ASSERT(passState.activeKey == drawKey);
        DXMT_ASSERT(!passState.activeWriteHazard.exactOverlaps(drawReadHazard));
        consumeActiveSeedMergeContinuation(commandIndex);
      }
    }
    if (perf::enabled() && encoderState.hasActiveRender &&
        passState.activeKey == drawKey) {
      renderPassFrameTracker.noteDrawRead(*stateView.hot);
    }
    // Phase 3-E: bind BaseDrawState ONCE on iter 0, then issue-only
    // path on iters 1..N — the Metal render encoder retains
    // pipeline / depth / viewport / scissor / cull / texture /
    // sampler state across draw calls.
    //
    // Phase 5-B: pre-scan for UP vertex/index payloads + batch-
    // upload them all in ONE uploadTransientBufferBatch call
    // (single TransientResourceArena acquire, single completedSeqId
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
    const std::size_t commandDrawCount = commandDrawItems.size();
    const auto recordPayloadArena = core::drawRunPayloadBytes(command);
    bool anyUpData = false;
    bool hasUpPayloadRanges = false;
    traceEncodeCommand("drawrun.before-up-prescan", commandIndex,
                       Kind::DrawRun, command);
    for (const auto& param : commandDrawItems) {
      if (!param.userVertexRange.empty() || !param.userIndexRange.empty()) {
        hasUpPayloadRanges = true;
        break;
      }
    }
    traceEncodeCommand("drawrun.after-up-prescan", commandIndex,
                       Kind::DrawRun, command);
    std::vector<CommandQueue::TransientBufferSlice> upSlices;
    if (hasUpPayloadRanges) {
      traceEncodeCommand("drawrun.before-up-upload", commandIndex,
                         Kind::DrawRun, command);
      std::vector<std::span<const std::byte>> upPayloads;
      upPayloads.reserve(commandDrawCount * 2);
      for (const auto& param : commandDrawItems) {
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
        if (auto* recorder = ctx.drawRecorder;
            recorder && recorder->uploadTransientBufferBatch) {
          upSlices = recorder->uploadTransientBufferBatch(
              recorder->userdata, upPayloads);
        } else {
          upSlices = ctx.queue.uploadTransientBufferBatchWithCompletedSeqId(
              upPayloads, /*alignment=*/16, sourceSeqId,
              ctx.transientCompletedSeqId);
        }
        if (!upSlices.empty()) {
          for (const auto& param : commandDrawItems) {
            const auto vertexBytes = drawParamVertexBytes(param, recordPayloadArena);
            const auto indexBytes = drawParamIndexBytes(param, recordPayloadArena);
            diagnosticsState.activeEncoderBreakdown.addTransientVertexBytes(
                static_cast<u64>(vertexBytes.size()),
                ActiveEncoderBreakdown::TransientVertexSource::Preupload);
            diagnosticsState.activeEncoderBreakdown.addTransientIndexBytes(
                static_cast<u64>(indexBytes.size()),
                ActiveEncoderBreakdown::TransientIndexSource::Preupload);
          }
        }
      }
      traceEncodeCommand("drawrun.after-up-upload", commandIndex,
                         Kind::DrawRun, command);
    }

    // encodeDraw receives the per-draw fields through DrawParam while
    // all base state is read from the canonical hot/shader view.
    // Per-frequency UBOs (VsConsts/PsConsts/FfpVsConsts/FfpPsConsts)
    // bind only on dirty (R-BACK-12.5/12.8); DrawVolatile is pushed
    // via setVertexBytes per draw with no slab traffic.
    const bool compatibleIndexedDrawMerge =
        debug::optimizeCompatibleIndexedDrawMerge() && !diagnosticsState.activeVisibilityScout;
    const bool compatibleIndexedDrawMergeTelemetry =
        compatibleIndexedDrawMerge && perf::enabled();
    CompatibleIndexedDrawMergeTelemetry mergeTelemetry{};
    auto addMergeTelemetry = [&](std::span<const core::DrawParam> drawItems) {
      if (!compatibleIndexedDrawMergeTelemetry) {
        return;
      }
      const auto subrangeTelemetry = measureCompatibleIndexedDrawMergePairs(
          drawItems, recordPayloadArena);
      mergeTelemetry.pairAttempts += subrangeTelemetry.pairAttempts;
      mergeTelemetry.compatiblePairs += subrangeTelemetry.compatiblePairs;
      mergeTelemetry.multipleRejectPairs +=
          subrangeTelemetry.multipleRejectPairs;
      for (std::size_t i = 0; i < mergeTelemetry.rejectPairs.size(); ++i) {
        mergeTelemetry.rejectPairs[i] += subrangeTelemetry.rejectPairs[i];
        mergeTelemetry.onlyRejectPairs[i] +=
            subrangeTelemetry.onlyRejectPairs[i];
      }
      for (std::size_t i = 0;
           i < mergeTelemetry.exactRelaxationSetPairs.size(); ++i) {
        mergeTelemetry.exactRelaxationSetPairs[i] +=
            subrangeTelemetry.exactRelaxationSetPairs[i];
      }
      mergeTelemetry.otherRelaxationSetPairs +=
          subrangeTelemetry.otherRelaxationSetPairs;
    };
    u64 selectedMergePairs = 0u;
    auto encodeDrawSubrange = [&](std::span<const core::DrawParam> drawItems,
                                  std::size_t commandDrawBegin) {
    const std::size_t drawCount = drawItems.size();
    for (std::size_t i = 0; i < drawCount;) {
      traceEncodeCommand("drawrun.draw-begin", commandIndex,
                         Kind::DrawRun, command);
      const auto merged = compatibleIndexedDrawMerge
          ? makeCompatibleIndexedDrawMerge(drawItems.subspan(i),
                                           recordPayloadArena)
          : CompatibleIndexedDrawMerge{
                .param = drawItems[i],
                .drawCount = 1u,
            };
      const auto& param = merged.param;
      const std::size_t mergedDrawCount =
          std::max<std::size_t>(1u, merged.drawCount);
      selectedMergePairs += static_cast<u64>(mergedDrawCount - 1u);
      const bool usesCommandUniform =
          !param.uniformHandle.valid() ||
          param.uniformHandle == command.drawRunRecord->uniformHandle;
      const auto* drawUniformPayload = usesCommandUniform
          ? commandUniformPayload
          : paramUniformPayloadCache.payloadForParam(
                command, param,
                perf::DrawUniformPayloadMaterializeSite::DrawEncoderParam);
      if (!drawUniformPayload) {
        traceEncodeCommand("drawrun.draw-skip-no-uniform", commandIndex,
                           Kind::DrawRun, command);
        i += mergedDrawCount;
        continue;
      }
      PreUploadedDrawData preData{};
      const std::size_t absoluteDrawIndex = commandDrawBegin + i;
      if (absoluteDrawIndex * 2u + 1u < upSlices.size()) {
        preData.vertex = upSlices[absoluteDrawIndex * 2u];
        preData.index = upSlices[absoluteDrawIndex * 2u + 1u];
      }
      core::DrawBindingOverride bindingOverride{};
      core::DrawBindingSnapshot bindingSnapshot{};
      core::FlatDrawStateRecord overrideHot{};
      core::DrawShaderLayoutContext overrideShaderLayout{};
      auto drawStateView = stateView;
      bool hasBindingOverride =
          drawParamBindingOverride(param, recordPayloadArena, bindingOverride);
      const bool hasBindingSnapshot =
          drawParamBindingSnapshot(param, recordPayloadArena, bindingSnapshot);
      // H228 — an alpha-test-only override (drawBindingOverrideHasBindings
      // false) needs no hot-state copy or binding rewrite; its trio is
      // consumed by encodeDraw's FsVolatile push directly.
      if (hasBindingOverride &&
          core::drawBindingOverrideHasBindings(bindingOverride)) {
        overrideHot = hot;
        if (drawStateView.shaderLayout) {
          overrideShaderLayout = *drawStateView.shaderLayout;
          applyDrawBindingOverride(overrideHot, &overrideShaderLayout, bindingOverride);
          drawStateView.shaderLayout = &overrideShaderLayout;
        } else {
          applyDrawBindingOverride(overrideHot, nullptr, bindingOverride);
        }
        drawStateView.hot = &overrideHot;
      }
      drawStateView.uniforms = drawUniformPayload;
      // Clip planes share FfpVsConsts with the fixed-function uniforms, but
      // their derived coefficients can change when only the vertex-shader
      // coordinate space changes.  That transition has no SetClipPlane
      // record to mark FfpVsClip dirty, so compare the canonical payload key
      // at the draw boundary before either direct or argbuf bindings consume
      // the dirty mask.
      if (bindingState.activeDrawStateKey.has_value() &&
          (bindingState.activeDrawStateKey->clipPlaneMask !=
               drawStateView.hot->key.clipPlaneMask ||
           bindingState.activeDrawStateKey->clipPlanesHash !=
               drawStateView.hot->key.clipPlanesHash)) {
        uniform::setBit(bindingState.uniformDirty, uniform::DirtyBit::FfpVsClip);
      }
      const auto drawArgbufPayloadDeltaKey =
          makeArgbufPayloadDeltaKey(drawStateView);
      const u64 drawArgbufPayloadHash = drawUniformPayload->hash;
      const bool argbufPayloadChanged =
          !bindingState.lastArgbufPayloadHash.has_value() ||
          *bindingState.lastArgbufPayloadHash != drawArgbufPayloadHash;
      const bool activePassUsesArgbufTable =
          bindingState.activePassUsesArgbufHybrid && !bindingState.activePassUsesArgbufDirectCbuf;
      const bool reopenArgbuf =
          activePassUsesArgbufTable &&
          (bindingState.activePassUsesArgbufResourceArray || argbufPayloadChanged);
      const auto previousArgbufPayloadSource =
          bindingState.lastArgbufPayloadDeltaKey.has_value()
              ? uniform::DirectCbufPayloadSourceHashes{
                    .vertexConstants =
                        bindingState.lastArgbufPayloadDeltaKey->vertexConstantsHash,
                    .pixelConstants =
                        bindingState.lastArgbufPayloadDeltaKey->pixelConstantsHash,
                }
              : uniform::DirectCbufPayloadSourceHashes{};
      const auto argbufPayloadSourceChange =
          uniform::classifyDirectCbufPayloadSourceChange(
              bindingState.lastArgbufPayloadDeltaKey.has_value(),
              previousArgbufPayloadSource,
              uniform::DirectCbufPayloadSourceHashes{
                  .vertexConstants =
                      drawArgbufPayloadDeltaKey.vertexConstantsHash,
                  .pixelConstants =
                      drawArgbufPayloadDeltaKey.pixelConstantsHash,
              });
      const bool argbufVsPayloadSourceChanged =
          argbufPayloadSourceChange.vertex;
      const bool argbufPsPayloadSourceChanged =
          argbufPayloadSourceChange.pixel;
      if (bindingState.activePassUsesArgbufDirectCbuf &&
          (argbufPayloadSourceChange.vertex ||
           argbufPayloadSourceChange.pixel)) {
        uniform::applyDirectCbufPayloadSourceChange(
            bindingState.uniformDirty,
            argbufPayloadSourceChange,
            uniform::DirectCbufPayloadCounts{
                .vertexFloat =
                    drawUniformPayload->vertexFloatConstantCount,
                .vertexInt = drawUniformPayload->vertexIntConstantCount,
                .vertexBool = drawUniformPayload->vertexBoolConstantCount,
                .pixelFloat = drawUniformPayload->pixelFloatConstantCount,
                .pixelInt = drawUniformPayload->pixelIntConstantCount,
                .pixelBool = drawUniformPayload->pixelBoolConstantCount,
            });
      }
      const bool argbufPayloadDeltaPerf =
          bindingState.activePassUsesArgbufHybrid && argbufPayloadDeltaPerfEnabled();
      std::optional<ArgbufPayloadDeltaComponentKey>
          drawArgbufPayloadDeltaComponentKey;
      if (argbufPayloadDeltaPerf) {
        drawArgbufPayloadDeltaComponentKey =
            makeArgbufPayloadDeltaComponentKey(*drawUniformPayload);
        perf::countEncodeDrawArgbufPayloadDeltaProbeCalls(1u);
        const bool cbufOnlyReopen =
            reopenArgbuf && !bindingState.activePassUsesArgbufResourceArray;
        if (bindingState.activePassUsesArgbufResourceArray && reopenArgbuf) {
          perf::countEncodeDrawArgbufPayloadDeltaReopenResourceArray(1u);
        }
        if (cbufOnlyReopen) {
          perf::countEncodeDrawArgbufPayloadDeltaReopenCbufOnly(1u);
        }
        if (!bindingState.lastArgbufPayloadDeltaKey.has_value()) {
          perf::countEncodeDrawArgbufPayloadDeltaFirst(1u);
          if (reopenArgbuf) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenFirst(1u);
          }
          if (cbufOnlyReopen) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenCbufOnlyFirst(1u);
          }
        } else if (bindingState.lastArgbufPayloadDeltaKey->hash ==
                   drawArgbufPayloadDeltaKey.hash) {
          perf::countEncodeDrawArgbufPayloadDeltaSame(1u);
          if (reopenArgbuf) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenPayloadSame(1u);
          }
        } else {
          perf::countEncodeDrawArgbufPayloadDeltaChanged(1u);
          if (reopenArgbuf) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenPayloadChanged(1u);
          }
          if (cbufOnlyReopen) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenCbufOnlyPayloadChanged(
                1u);
          }
          const bool vsChanged =
              bindingState.lastArgbufPayloadDeltaKey->vertexConstantsHash !=
              drawArgbufPayloadDeltaKey.vertexConstantsHash;
          const bool psChanged =
              bindingState.lastArgbufPayloadDeltaKey->pixelConstantsHash !=
              drawArgbufPayloadDeltaKey.pixelConstantsHash;
          if (vsChanged) {
            perf::countEncodeDrawArgbufPayloadDeltaChangedVs(1u);
          }
          if (psChanged) {
            perf::countEncodeDrawArgbufPayloadDeltaChangedPs(1u);
          }
          if (vsChanged && psChanged) {
            perf::countEncodeDrawArgbufPayloadDeltaChangedVsPs(1u);
          }
          if (!vsChanged && !psChanged) {
            perf::countEncodeDrawArgbufPayloadDeltaChangedNonConstOnly(1u);
          }
          if (bindingState.lastArgbufPayloadDeltaComponentKey.has_value() &&
              drawArgbufPayloadDeltaComponentKey.has_value()) {
            const auto& lastComponents =
                *bindingState.lastArgbufPayloadDeltaComponentKey;
            const auto& drawComponents =
                *drawArgbufPayloadDeltaComponentKey;
            if (vsChanged) {
              if (lastComponents.vsFloatHash != drawComponents.vsFloatHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloat(1u);
                if (bindingState.lastArgbufPayloadDeltaPayload.has_value()) {
                  const auto stats =
                      measureArgbufPayloadChangedPrefix(
                          bindingState.lastArgbufPayloadDeltaPayload->vsConst.float4,
                          bindingState.lastArgbufPayloadDeltaPayload->vertexFloatConstantCount,
                          drawUniformPayload->vsConst.float4,
                          drawUniformPayload->vertexFloatConstantCount);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegs(
                      stats.changed);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsMax(
                      stats.changed);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegs(
                      stats.prefix);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegsMax(
                      stats.prefix);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegs(
                      stats.span);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegsMax(
                      stats.span);
                  if (stats.fullPrefix) {
                    perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefix(
                        1u);
                    perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefixRegs(
                        stats.changed);
                  }
                  if (argbufPayloadDeltaSourcePerf) {
                    argbufPayloadDeltaSourceAttribution.record(
                        drawStateView, stats);
                  }
                  recordArgbufPayloadChangedVsFloatRegBucket(stats.changed);
                }
              }
              if (lastComponents.vsIntHash != drawComponents.vsIntHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedVsInt(1u);
              }
              if (lastComponents.vsBoolHash != drawComponents.vsBoolHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedVsBool(1u);
              }
            }
            if (psChanged) {
              if (lastComponents.psFloatHash != drawComponents.psFloatHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloat(1u);
                if (bindingState.lastArgbufPayloadDeltaPayload.has_value()) {
                  const auto stats =
                      measureArgbufPayloadChangedPrefix(
                          bindingState.lastArgbufPayloadDeltaPayload->psConst.float4,
                          bindingState.lastArgbufPayloadDeltaPayload->pixelFloatConstantCount,
                          drawUniformPayload->psConst.float4,
                          drawUniformPayload->pixelFloatConstantCount);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegs(
                      stats.changed);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsMax(
                      stats.changed);
                  recordArgbufPayloadChangedPsFloatRegBucket(stats.changed);
                }
              }
              if (lastComponents.psIntHash != drawComponents.psIntHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedPsInt(1u);
              }
              if (lastComponents.psBoolHash != drawComponents.psBoolHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedPsBool(1u);
              }
            }
          }
        }
      }
      const bool baseStateCompatible =
          bindingState.activeDrawStateKey.has_value() &&
          bindingState.activeDrawStateUsesPrefetchedPsoLayout &&
          core::drawStateKeysCompatibleForDrawRunBatch(
              *bindingState.activeDrawStateKey, drawStateView.hot->key);
      const bool overrideNeedsBaseStateBind =
          hasBindingOverride &&
          drawBindingOverrideRequiresBaseStateBind(
              bindingOverride, stateView.shaderLayout);
      const bool bindingOverridePrefetchedPsoCompatible =
          hasBindingOverride && !overrideNeedsBaseStateBind;
      const bool skipBaseStateBind =
          baseStateCompatible && !overrideNeedsBaseStateBind;
      maybeCollectDrawTextureDump(diagnosticsState.activeDrawTextureDumps,
                                  ctx.pool,
                                  drawStateView,
                                  passState.activeInstance.seqId,
                                  passState.activeInstance.encoderIndex);
      const u64 encoderDrawIndexBeforeEncode =
          diagnosticsState.activeEncoderBreakdown.stats.drawCalls;
      const u64 drawTexture0 = drawStateView.hot->textures[0]
          ? drawStateView.hot->textures[0].value
          : 0ull;
      traceEncodeCommand("drawrun.before-encode-draw", commandIndex,
                         Kind::DrawRun, command);
      if (encodeDraw(ctx, commandBuffer, encoderState.activeRenderEncoder, drawStateView, sourceSeqId,
                     /*skipBaseStateBind=*/skipBaseStateBind,
                     anyUpData ? &preData : nullptr,
                     &param,
                     recordPayloadArena,
                     hasBindingOverride ? &bindingOverride : nullptr,
                     hasBindingSnapshot ? &bindingSnapshot : nullptr,
                     &bindingState.uniformDirty,
                     /*tileFfpMode=*/passState.activePassUsesTileFfp,
                     /*argbufHybridMode=*/bindingState.activePassUsesArgbufHybrid,
                     /*argbufResourceArray=*/bindingState.activePassUsesArgbufResourceArray,
                     /*argbufDirectCbufMode=*/bindingState.activePassUsesArgbufDirectCbuf,
                     /*reopenArgbufHybrid=*/reopenArgbuf,
                     /*argbufVsPayloadSourceChanged=*/argbufVsPayloadSourceChanged,
                     /*argbufPsPayloadSourceChanged=*/argbufPsPayloadSourceChanged,
                     /*bindingOverridePrefetchedPsoCompatible=*/bindingOverridePrefetchedPsoCompatible,
                     renderPsoHandle,
                     tilePsoHandle,
                     depthStencilHandle,
                     &bindingState.textureSamplerShadow,
                     commandIndex <= std::numeric_limits<std::uint32_t>::max()
                         ? static_cast<std::uint32_t>(commandIndex)
                         : std::numeric_limits<std::uint32_t>::max(),
                     static_cast<u64>(absoluteDrawIndex),
                     static_cast<u64>(commandDrawCount),
                     &diagnosticsState.activeEncoderBreakdown,
                     &bindingState.argbufCbufCache,
                     &bindingState.activeStreamIbStaging,
                     diagnosticsState.activeVisibilityScout ? &*diagnosticsState.activeVisibilityScout : nullptr)) {
        traceEncodeCommand("drawrun.after-encode-draw-ok", commandIndex,
                           Kind::DrawRun, command);
        bindingState.activeDrawStateKey = drawStateView.hot->key;
        bindingState.activeDrawStateUsesPrefetchedPsoLayout = !overrideNeedsBaseStateBind;
        bindingState.lastArgbufPayloadHash = drawArgbufPayloadHash;
        bindingState.lastArgbufPayloadDeltaKey = drawArgbufPayloadDeltaKey;
        if (argbufPayloadDeltaPerf &&
            drawArgbufPayloadDeltaComponentKey.has_value()) {
          bindingState.lastArgbufPayloadDeltaComponentKey =
              *drawArgbufPayloadDeltaComponentKey;
          bindingState.lastArgbufPayloadDeltaPayload = *drawUniformPayload;
        }
        if (colorAttachmentDumpAfterDrawWantsSplit(
                diagnosticsState.activeColorAttachmentDump,
                drawStateView,
                encoderDrawIndexBeforeEncode,
                commandIndex)) {
          diagnosticsState.activeColorAttachmentDump.afterDraw = true;
          diagnosticsState.activeColorAttachmentDump.draw = encoderDrawIndexBeforeEncode;
          diagnosticsState.activeColorAttachmentDump.commandIndex = commandIndex;
          diagnosticsState.activeColorAttachmentDump.commandDrawIndex =
              absoluteDrawIndex;
          diagnosticsState.activeColorAttachmentDump.commandDrawCount =
              commandDrawCount;
          diagnosticsState.activeColorAttachmentDump.texture0 = drawTexture0;
          flushRender(perf::EncoderSplitReason::Final);
          if (absoluteDrawIndex + mergedDrawCount < commandDrawCount) {
            startRenderPass(drawStateView, std::nullopt, commandIndex,
                            renderPsoHandle);
          }
        }
      } else {
        traceEncodeCommand("drawrun.after-encode-draw-false", commandIndex,
                           Kind::DrawRun, command);
      }
      i += mergedDrawCount;
    }
    };
    if (drawPartitions.empty()) {
      if (auto* recorder = ctx.drawRecorder;
          recorder && recorder->beginDrawSubrange) {
        recorder->beginDrawSubrange(
            recorder->userdata, commandIndex,
            command.drawRunRecord
                ? command.drawRunRecord->firstParam
                : 0u,
            commandDrawItems.size());
      }
      addMergeTelemetry(commandDrawItems);
      encodeDrawSubrange(commandDrawItems, 0u);
    } else {
      for (const auto& drawPartition : drawPartitions) {
        const auto resolved = resolveEncodePartition(
            drawPartition, partitionReplayStream);
        if (!resolved || !resolved.partition.entry.drawRunRecord ||
            resolved.partition.entry.command.drawRunRecord !=
                command.drawRunRecord) {
          abortEncodePartitionInvariant(
              "explicit DrawRun range re-resolution mismatch");
        }
        const std::size_t commandDrawBegin =
            drawPartition.entry.drawParamIndex -
            command.drawRunRecord->firstParam;
        if (auto* recorder = ctx.drawRecorder;
            recorder && recorder->beginDrawSubrange) {
          recorder->beginDrawSubrange(
              recorder->userdata, commandIndex,
              drawPartition.entry.drawParamIndex,
              resolved.partition.drawParams.size());
        }
        addMergeTelemetry(resolved.partition.drawParams);
        encodeDrawSubrange(resolved.partition.drawParams, commandDrawBegin);
      }
    }
    if (compatibleIndexedDrawMergeTelemetry) {
      perf::countCompatibleIndexedDrawMergeTelemetry(
          mergeTelemetry.pairAttempts,
          mergeTelemetry.compatiblePairs,
          mergeTelemetry.multipleRejectPairs,
          selectedMergePairs,
          mergeTelemetry.rejectPairs,
          mergeTelemetry.onlyRejectPairs,
          mergeTelemetry.exactRelaxationSetPairs,
          mergeTelemetry.otherRelaxationSetPairs);
    }
    traceEncodeCommand("drawrun.end", commandIndex, Kind::DrawRun, command);
    if (activeSeedMergeAttributionEnabled) {
      activeSeedMergeTicketAudit.endCommand();
    }
    commandBufferHasWork = true;
  };

  auto applyPerRecordSplitPolicy = [&](bool presentRecord) {
    // R-BACK-2.29..2.32 — per-N-records policy. Counts every replayed
    // record (including helper-encoder commands), and fires a mid-chunk
    // commit when the threshold is hit AND there is no active encoder.
    // The flushBlit + flushRender(non-Final) sequence enforces the
    // splitMidChunk invariant: encoder must be ended before commit.
    // Final reason is used here because we are not opening a new render
    // pass after the commit; the next iteration will start one fresh.
    //
    // R-BACK-2.30: Present records attach drawable + presentDrawable to
    // the CURRENT command buffer; a split right after Present would
    // promote the present-bearing CB out of the chain tail position and
    // violate the "present metadata on the last sub-CB only" rule.
    // Suppress the per-N-records split immediately after a Present
    // record; the present-tail boundary is not split further here.
    if (auto* recorder = ctx.drawRecorder;
        recorder && recorder->applyPerRecordSplitPolicy) {
      recorder->applyPerRecordSplitPolicy(
          recorder->userdata, presentRecord);
    }
    ++recordsSinceLastSplit;
    if (commitPolicy == MidChunkCommitPolicy::PerNRecords &&
        recordsSinceLastSplit >= splitNRecords &&
        !presentRecord) {
      flushBlit();
      flushRender(perf::EncoderSplitReason::Final);
      assertNoActiveEncoder();
      splitMidChunkUnderCap();
      recordsSinceLastSplit = 0;
    }
  };

  auto encodeCompleteCommand = [&](std::size_t commandIndex,
                                   const core::SourceCommandView& source) {
      const auto& command = source.command;
      traceEncodeCommand("begin", commandIndex, command.kind, command);
      // TLA+: EncoderLifecycle / opCount observes command replay progress.
      switch (command.kind) {
      case Kind::Clear: {
        if (!source.clear.has_value()) break;
        const auto& clearView = *source.clear;
        lifecycle.resolveLateStoreForClear(clearView);
        flushRender(perf::EncoderSplitReason::ClearBarrier);
        flushBlit();
        flushPendingClear();
        if (clearView.rects.empty()) {
          passState.pendingClear = core::ClearDesc{
              .colorAttachments = clearView.colorAttachments,
              .depthStencil = clearView.depthStencil,
              .clearColor = clearView.clearColor,
              .clearDepth = clearView.clearDepth,
              .clearStencil = clearView.clearStencil,
              .color = clearView.color,
              .depth = clearView.depth,
              .stencil = clearView.stencil,
          };
          passState.pendingClearCommand = lifecycle.commandRef(commandIndex);
        } else {
          const auto sampleAttachment = makeRenderEncoderGpuAttachment(
              core::metalqueue::RenderEncoderGpuPassType::Clear,
              commandIndex,
              clearView.colorAttachments[0].handle.value,
              clearView.depthStencil.handle.value);
          dxmt9::encoders::encodeClearPass(commandBuffer, ctx.pool, clearView,
                                           sampleAttachment.span());
          recordRenderEncoderGpuAttachment(sampleAttachment);
          commandBufferHasWork = true;
        }
        break;
      }
      case Kind::DrawRun: {
        encodeDrawRunCommand(commandIndex, command, {});
        break;
      }
      case Kind::SurfaceCopy: {
        if (!command.surfaceCopy) break;
        flushPendingClear();
        lifecycle.resolveLateStoreForStoreCause(
            perf::RenderPassLateStoreResolutionCause::Copy);
        flushRender(perf::EncoderSplitReason::SurfaceCopy);
        assertHelperEncoderPrecondition();
        // R-BACK-15.5: destination handle's contents are overwritten;
        // the next render pass on it qualifies as first-use again.
        ctx.queue.invalidateColorHandle(command.surfaceCopy->destination);
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::SurfaceCopy,
            commandIndex,
            command.surfaceCopy->destination.value,
            0);
        dxmt9::encoders::encodeSurfaceCopy(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                           ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                           *command.surfaceCopy,
                                           sampleAttachment.span());
        recordRenderEncoderGpuAttachment(sampleAttachment);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::StretchRect: {
        if (!command.stretchRect) break;
        flushPendingClear();
        lifecycle.resolveLateStoreForStoreCause(
            perf::RenderPassLateStoreResolutionCause::Copy);
        flushRender(perf::EncoderSplitReason::StretchRect);
        assertHelperEncoderPrecondition();
        // R-BACK-15.5
        ctx.queue.invalidateColorHandle(command.stretchRect->destination);
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::StretchRect,
            commandIndex,
            command.stretchRect->destination.value,
            0);
        dxmt9::encoders::encodeStretchRect(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                            ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                            *command.stretchRect,
                                            sampleAttachment.span());
        recordRenderEncoderGpuAttachment(sampleAttachment);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::Readback: {
        if (!command.readback) break;
        flushPendingClear();
        lifecycle.resolveLateStoreForStoreCause(
            perf::RenderPassLateStoreResolutionCause::Readback);
        flushRender(perf::EncoderSplitReason::Readback);
        assertHelperEncoderPrecondition();
        // R-BACK-15.5: destination receives content; source is unaffected
        ctx.queue.invalidateColorHandle(command.readback->destination);
        dxmt9::encoders::encodeReadback(commandBuffer, ctx.pool, *command.readback);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::DepthResolve: {
        if (!command.depthResolve) break;
        flushPendingClear();
        lifecycle.resolveLateStoreForStoreCause(
            perf::RenderPassLateStoreResolutionCause::Resolve);
        // RESZ depth resolve is the DEPTH twin of the color StretchRect
        // resolve — reuse its split-reason bucket rather than expand the
        // perf-counter table for a rarely-hit op.
        flushRender(perf::EncoderSplitReason::StretchRect);
        assertHelperEncoderPrecondition();
        // R-FORMAT-11 — RESZ MSAA depth resolve. The DEPTH twin of the color
        // resolve already wired in encodeStretchRect/encodeColorFill: open a
        // depth-only render pass with store=MultisampleResolve and end it. The
        // INTZ destination's contents are overwritten, so it qualifies as
        // first-use again (R-BACK-15.5).
        ctx.queue.invalidateColorHandle(command.depthResolve->intzDest);
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::DepthResolve,
            commandIndex,
            0,
            command.depthResolve->intzDest.value);
        dxmt9::encoders::encodeDepthResolve(commandBuffer, ctx.pool,
                                            command.depthResolve->msaaDepth,
                                            command.depthResolve->intzDest,
                                            sampleAttachment.span());
        recordRenderEncoderGpuAttachment(sampleAttachment);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::ColorFill: {
        if (!command.colorFill) break;
        flushPendingClear();
        lifecycle.resolveLateStoreForStoreCause(
            perf::RenderPassLateStoreResolutionCause::Copy);
        flushRender(perf::EncoderSplitReason::ColorFill);
        // TLA+: EncoderLifecycle / BeginRender(rt)
        // ColorFill owns a short-lived helper render encoder and ends it before returning.
        assertNoActiveEncoder();
        // R-BACK-15.5
        ctx.queue.invalidateColorHandle(command.colorFill->destination);
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::ColorFill,
            commandIndex,
            command.colorFill->destination.value,
            0);
        dxmt9::encoders::encodeColorFill(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                          ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                          *command.colorFill,
                                          sampleAttachment.span());
        recordRenderEncoderGpuAttachment(sampleAttachment);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::Present: {
        if (!command.present) break;
        const auto& present = command.present->present;
        const auto presentSource = command.present->presentSource;
        if (!diagnosticsState.metalCaptureRequest.has_value()) {
          // Bump the controller's frame counter and, if this is the
          // target frame's Present chunk, recover the chunk-begin session
          // request so `record.metalCapture` triggers stopCapture at
          // commit time. For non-target frames the call is a no-op apart
          // from the counter bump.
          diagnosticsState.metalCaptureRequest =
              ctx.queue.notePresentChunkForCapture(sourceSeqId);
          if (diagnosticsState.metalCaptureRequest.has_value()) {
            // Capture was started at an earlier chunk-begin; this
            // chunk's commit should only call stopCapture, never
            // re-start.
            captureAlreadyStartedAtChunkBegin = true;
          }
        }
        flushPendingClear();
        lifecycle.resolveLateStoreForStoreCause(
            perf::RenderPassLateStoreResolutionCause::Present);
        flushRender(perf::EncoderSplitReason::Present);
        flushBlit();
        // Resolve the queue-local Presenter binding once per Present
        // packet and reclaim any acquire-before-present token stashed by
        // submitPresent. A stale PresentId (swapchain destroyed since
        // submission) produces a nullptr Presenter — encodePresent then
        // short-circuits to a skipped present.
        dxmt9::Presenter* const presenter = ctx.queue.lookupPresenter(present.presentId);
        auto pendingDrawableToken = ctx.queue.takeDrawableToken(present.presentId);
        const bool noteAfterAcquire = presentBoundaryAfterAcquireEnabled();
        if (!noteAfterAcquire) {
          ctx.queue.notePresentDequeued(sourceSeqId);
        }
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::Present,
            commandIndex,
            presentSource.value,
            0);
        const bool presentEncoded = dxmt9::encodePresent(commandBuffer, ctx.pool,
                                                          presenter,
                                                          std::move(pendingDrawableToken),
                                                          present, presentSource, sourceSeqId,
                                                          sampleAttachment.span());
        if (presentEncoded) {
          recordRenderEncoderGpuAttachment(sampleAttachment);
        }
        if (noteAfterAcquire) {
          ctx.queue.notePresentDequeued(sourceSeqId);
        }
        if (presentEncoded) {
          commandBufferHasWork = true;
          ctx.queue.backBufferDiscardAfterPresent_ = true;
          if (presenter) {
            completionState.postCommitCallbacks.push_back([presenter, seqId = sourceSeqId] {
              presenter->preAcquireNextDrawable(seqId);
            });
          }
        }
        // Resolve the final physical close before the frame snapshot so the
        // close-ledger conservation equation is frame-local.
        if (perf::enabled()) {
          renderPassFrameTracker.finishCloseLedgerAtPresent();
        }
        // Per-frame snapshot mode (DXMT9_PERF_FRAME_SAMPLING=1). Fires
        // exactly once per Present packet on the encode thread, so it
        // does not make Present synchronous from the app side.
        // Default off → just one bool check, no atomic loads.
        if (perf::frameSamplingEnabled()) {
          static thread_local perf::CounterSnapshot prevSnapshot{};
          static thread_local std::uint64_t frameId = 0;
          perf::CounterSnapshot curr = perf::snapshot();
          perf::emitFrameDelta(frameId++, prevSnapshot, curr);
          prevSnapshot = curr;
        }
        if (perf::enabled()) {
          renderPassFrameTracker.reset();
        }
        // M3 — Instruments "frame" interval. End the frame that just
        // got a Present commit, then immediately begin the next one so
        // any encode work on this thread before the next Present is
        // attributed to that frame. Single encode thread → EXCLUSIVE
        // is the correct id. Always-on (no_op when no consumer).
        {
          os_log_t signpostLog = dxmt9::signposts::log();
          static thread_local bool frameSignpostActive = false;
          static thread_local std::uint64_t frameSignpostSeq = 0;
          if (frameSignpostActive) {
            os_signpost_interval_end(signpostLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                     "frame", "seq=%llu",
                                     static_cast<unsigned long long>(frameSignpostSeq));
          }
          ++frameSignpostSeq;
          os_signpost_interval_begin(signpostLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                     "frame", "seq=%llu",
                                     static_cast<unsigned long long>(frameSignpostSeq));
          frameSignpostActive = true;
        }
        break;
      }
      }
      traceEncodeCommand("after-encode", commandIndex, command.kind, command);
      traceEncodeCommand("before-split-policy", commandIndex, command.kind, command);
      applyPerRecordSplitPolicy(command.kind == Kind::Present);
      traceEncodeCommand("end", commandIndex, command.kind, command);
  };

  EncodePartitionSerialCursor partitionCursor(
      partitionReplayStream, options.partitionRanges,
      useExplicitPartitionPlan);
  EncodePartitionSerialBatch partitionBatch{};
  while (partitionCursor.next(partitionBatch)) {
      if (partitionBatch.kind == EncodePartitionRangeKind::CommandSegment) {
        const std::uint64_t ordinalEnd =
            static_cast<std::uint64_t>(partitionBatch.replayOrdinalBegin) +
            partitionBatch.replayOrdinalCount;
        for (std::uint64_t ordinal = partitionBatch.replayOrdinalBegin;
             ordinal < ordinalEnd; ++ordinal) {
          std::uint32_t commandIndex = 0;
          const bool resolvedCommand = partitionReplayStream.commandIndexAt(
              static_cast<std::size_t>(ordinal), commandIndex);
          if (!resolvedCommand) {
            abortEncodePartitionInvariant(
                "CommandSegment replay ordinal resolution failed");
          }
          encodeCompleteCommand(commandIndex, payload.commandAt(commandIndex));
        }
        continue;
      }

      std::uint32_t commandIndex = 0;
      core::MetalCommandView command{};
      if (partitionBatch.identityResolved) {
        if (partitionBatch.ranges.size() != 1u ||
            !partitionBatch.identityPartition.entry.drawRunRecord ||
            partitionBatch.identityPartition.drawParams.empty()) {
          abortEncodePartitionInvariant(
              "identity DrawRun batch is malformed");
        }
        commandIndex = partitionBatch.ranges.front().entry.commandIndex;
        command = partitionBatch.identityPartition.entry.command;
      } else {
        const bool resolvedCommand = partitionReplayStream.commandIndexAt(
            partitionBatch.replayOrdinalBegin, commandIndex);
        if (!resolvedCommand) {
          abortEncodePartitionInvariant(
              "explicit DrawRun replay ordinal resolution failed");
        }
        command = payload.commandAt(commandIndex).command;
      }
      traceEncodeCommand("begin", commandIndex, Kind::DrawRun, command);
      // TLA+: EncoderLifecycle / opCount advances once for the complete source
      // command even when that DrawRun has multiple explicit subranges.
      encodeDrawRunCommand(
          commandIndex, command,
          partitionBatch.identityResolved
              ? std::span<const EncodePartitionRangeSnapshot>{}
              : partitionBatch.ranges);
      traceEncodeCommand("after-encode", commandIndex, Kind::DrawRun, command);
      traceEncodeCommand("before-split-policy", commandIndex, Kind::DrawRun,
                         command);
      applyPerRecordSplitPolicy(/*presentRecord=*/false);
      traceEncodeCommand("end", commandIndex, Kind::DrawRun, command);
  }

  if (options.session && options.sessionSource.has_value()) {
    const bool appended =
        appendEncodeChunkSessionSource(*options.session,
                                       *options.sessionSource);
    DXMT_ASSERT(appended);
    if (!appended) {
      return std::nullopt;
    }
  }

  if (!deferSessionFinalization) {
    finalizeEncodeChunkSessionForReturn();
  }

  // A carried EncodeSession owns the tail chain across source and fragment
  // calls. Preserve both split-policy inputs after finalization has flushed
  // any deferred clear/render work.
  session.completion.tailCommandBufferHasWork = commandBufferHasWork;
  session.completion.recordsSinceLastSplit = recordsSinceLastSplit;

  traceEncodeStage("before-record-chunk-sub-cb-count");
  // R-BACK-2.29..2.32 — fold this encode call's sub-CB segment length into
  // chunkSubCBCountMax. A carried EncodeSession enforces the cap across
  // source boundaries with session.completion.committedSubCommandBuffers, but the record
  // still publishes only the segment length added by this call. Queue-side
  // merge then subtracts the shared tail CB once when joining segments.
  std::uint64_t sourceCommittedSubCommandBuffers = perChunkSubCBCount;
  const bool lastSourceFragment =
      !options.preRegisteredFragment.has_value() ||
      options.preRegisteredFragment->lastSourceFragment();
  if (options.preRegisteredSourceAccumulator) {
    options.preRegisteredSourceAccumulator->committedSubCommandBuffers +=
        perChunkSubCBCount;
    sourceCommittedSubCommandBuffers =
        options.preRegisteredSourceAccumulator->committedSubCommandBuffers;
  }
  if (lastSourceFragment &&
      (commandBufferHasWork || sourceCommittedSubCommandBuffers > 0)) {
    perf::recordChunkSubCBCount(sourceCommittedSubCommandBuffers + 1);
  }
  if (lastSourceFragment && ctx.drawRecorder &&
      ctx.drawRecorder->endSourceFragmentEpilogue) {
    ctx.drawRecorder->endSourceFragmentEpilogue(
        ctx.drawRecorder->userdata, sourceSeqId,
        sourceCommittedSubCommandBuffers);
  }
  traceEncodeStage("after-record-chunk-sub-cb-count");

  const u64 seqId = sourceSeqId;
  if (argbufPayloadDeltaSourcePerf && lastSourceFragment) {
    traceEncodeStage("before-argbuf-payload-delta-source-emit");
    argbufPayloadDeltaSourceAttribution.emit(seqId);
    traceEncodeStage("after-argbuf-payload-delta-source-emit");
  }
  traceEncodeStage("before-build-submission-record");
  core::metalqueue::QueueSubmissionRecord record;
  record.commandBufferChainLength = perChunkSubCBCount + 1;
  record.slotIndex = slotIndex;
  record.seqId = seqId;
  record.context = "queue";
  if (deferSessionFinalization) {
    record.commandBuffer = std::move(commandBuffer);
  } else {
    if (options.session) {
      traceEncodeStage("before-publish-session-sources");
    }
    const bool published = lifecycle.publishIntoSubmission(
        record, options.session,
        /*resetSessionAfterPublication=*/false);
    if (options.session) {
      traceEncodeStage(published ? "after-publish-session-sources-ok"
                                 : "after-publish-session-sources-failed");
    }
    DXMT_ASSERT(published);
    if (!published) {
      return std::nullopt;
    }
  }
  traceEncodeStage("after-build-submission-record");
  if (!deferSessionFinalization && options.session) {
    traceEncodeStage("session-owner-retained-by-caller");
  }
  traceEncodeStage("return-record");
  return record;
  }  // @autoreleasepool
}

std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    EncodeChunkOptions options) {
  return encodeChunk(ctx, slotIndex, core::SourcePayloadView(slot), slot.seqId,
                     std::move(options));
}

}  // namespace dxmt9::encoders
