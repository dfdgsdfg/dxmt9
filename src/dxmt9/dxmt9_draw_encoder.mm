#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_encoder_internal.hpp"
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

using dxmt9::state::DrawVolatile;
using dxmt9::state::FfpPsConsts;
using dxmt9::state::FfpVsConsts;
using dxmt9::state::PsConsts;
using dxmt9::state::VsConsts;
using dxmt9::state::buildDrawVolatile;
using dxmt9::state::buildFfpPsConsts;
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

// M1/M2 — printf-style label/group-name builder. Returns a non-owning
// WMT::String view backed by an autoreleased NSString. Lifetime is safe
// because the receiving setLabel:/pushDebugGroup: selector retains
// immediately and encodeChunk runs inside an @autoreleasepool.
template <std::size_t Cap = 96>
WMT::String makeLabelStringFmt(const char* fmt, ...) {
  char buf[Cap];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return WMT::String::string(buf, WMTUTF8StringEncoding);
}

// M2 — RAII debug-group helper. Pairs a pushDebugGroup with the
// matching popDebugGroup on scope exit, even on early-return paths.
// Holds a non-owning view of the encoder; the caller retains the
// encoder's lifetime through Reference<>.
class DebugGroupScope {
 public:
  DebugGroupScope(WMT::CommandEncoder encoder, WMT::String name)
      : encoder_(encoder) {
    if (encoder_ && name) {
      encoder_.pushDebugGroup(name);
      active_ = true;
    }
  }

  ~DebugGroupScope() {
    if (active_) {
      encoder_.popDebugGroup();
    }
  }

  // Non-copyable, non-movable — RAII pair must stay paired with one
  // scope entry.
  DebugGroupScope(const DebugGroupScope&) = delete;
  DebugGroupScope& operator=(const DebugGroupScope&) = delete;
  DebugGroupScope(DebugGroupScope&&) = delete;
  DebugGroupScope& operator=(DebugGroupScope&&) = delete;

 private:
  WMT::CommandEncoder encoder_{};
  bool active_ = false;
};

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

bool suppressRecordedMetalCalls(const EncodeContext& ctx) noexcept {
  return ctx.drawRecorder && ctx.drawRecorder->suppressMetalCalls;
}

bool suppressBaseStateLookup(const EncodeContext& ctx) noexcept {
  return ctx.drawRecorder && ctx.drawRecorder->suppressBaseStateLookup;
}

void recordedSetRenderPipelineState(EncodeContext& ctx,
                                    WMT::RenderCommandEncoder& encoder,
                                    WMT::RenderPipelineState pipeline) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setRenderPipelineState) {
    recorder->setRenderPipelineState(recorder->userdata, pipeline);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setRenderPipelineState(pipeline);
  }
}

void recordedSetDepthStencilState(EncodeContext& ctx,
                                  WMT::RenderCommandEncoder& encoder,
                                  WMT::DepthStencilState depthStencil,
                                  std::uint8_t stencilRef = 0) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setDepthStencilState) {
    recorder->setDepthStencilState(recorder->userdata, depthStencil, stencilRef);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setDepthStencilState(depthStencil, stencilRef);
  }
}

void recordedSetViewport(EncodeContext& ctx,
                         WMT::RenderCommandEncoder& encoder,
                         WMTViewport viewport) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setViewport) {
    recorder->setViewport(recorder->userdata, viewport);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setViewport(viewport);
  }
}

void recordedSetScissorRect(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            WMTScissorRect rect) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setScissorRect) {
    recorder->setScissorRect(recorder->userdata, rect);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setScissorRect(rect);
  }
}

void recordedSetRasterizerState(EncodeContext& ctx,
                                WMT::RenderCommandEncoder& encoder,
                                WMTTriangleFillMode fillMode,
                                WMTCullMode cullMode,
                                WMTDepthClipMode depthClipMode,
                                WMTWinding winding,
                                float depthBias,
                                float slopeScale,
                                float depthBiasClamp) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setRasterizerState) {
    recorder->setRasterizerState(recorder->userdata, fillMode, cullMode,
                                 depthClipMode, winding, depthBias,
                                 slopeScale, depthBiasClamp);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setRasterizerState(fillMode, cullMode, depthClipMode, winding,
                               depthBias, slopeScale, depthBiasClamp);
  }
}

void recordedSetFragmentTexture(EncodeContext& ctx,
                                WMT::RenderCommandEncoder& encoder,
                                WMT::Texture texture,
                                std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setFragmentTexture) {
    recorder->setFragmentTexture(recorder->userdata, texture, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setFragmentTexture(texture, index);
  }
}

void recordedSetFragmentSamplerState(EncodeContext& ctx,
                                     WMT::RenderCommandEncoder& encoder,
                                     WMT::SamplerState sampler,
                                     std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setFragmentSamplerState) {
    recorder->setFragmentSamplerState(recorder->userdata, sampler, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setFragmentSamplerState(sampler, index);
  }
}

void recordedSetVertexBuffer(EncodeContext& ctx,
                             WMT::RenderCommandEncoder& encoder,
                             WMT::Buffer buffer,
                             u64 offset,
                             std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexBuffer) {
    recorder->setVertexBuffer(recorder->userdata, buffer, offset, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexBuffer(buffer, offset, index);
  }
}

void recordedSetVertexBytes(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            const void* bytes,
                            u64 length,
                            std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexBytes) {
    recorder->setVertexBytes(recorder->userdata, bytes, length, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexBytes(bytes, length, index);
  }
}

void recordedDrawPrimitives(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            WMTPrimitiveType primitiveType,
                            u64 vertexStart,
                            u64 vertexCount) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->drawPrimitives) {
    recorder->drawPrimitives(recorder->userdata, primitiveType,
                             vertexStart, vertexCount);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.drawPrimitives(primitiveType, vertexStart, vertexCount);
  }
}

void recordedDrawIndexedPrimitives(EncodeContext& ctx,
                                   WMT::RenderCommandEncoder& encoder,
                                   WMTPrimitiveType primitiveType,
                                   WMTIndexType indexType,
                                   u64 indexCount,
                                   WMT::Buffer indexBuffer,
                                   u64 indexBufferOffset,
                                   u32 instanceCount,
                                   i32 baseVertex,
                                   u32 baseInstance) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->drawIndexedPrimitives) {
    recorder->drawIndexedPrimitives(recorder->userdata, primitiveType,
                                    indexType, indexCount, indexBuffer,
                                    indexBufferOffset, instanceCount,
                                    baseVertex, baseInstance);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.drawIndexedPrimitives(primitiveType, indexType, indexCount,
                                  indexBuffer, indexBufferOffset,
                                  instanceCount, baseVertex, baseInstance);
  }
}

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

WMTTriangleFillMode triangleFillModeFromRenderState(
    const core::FlatStateSet<core::kMaxStateSlots>& renderStates) {
  constexpr u32 kD3DFillWireframe = 2u;
  return core::flatStateOr(renderStates, core::RS_FILL_MODE, 3u) == kD3DFillWireframe
             ? WMTTriangleFillModeLines
             : WMTTriangleFillModeFill;
}

void setRasterizerCullMode(EncodeContext& ctx,
                           WMT::RenderCommandEncoder& encoder,
                           const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
                           WMTCullMode cullMode) {
  cullMode = applyDebugCullOverride(cullMode);
  recordedSetRasterizerState(ctx, encoder, triangleFillModeFromRenderState(renderStates), cullMode,
                             WMTDepthClipModeClip, frontFaceWinding(),
                             0.0f, 0.0f, 0.0f);
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
  DXMT_ASSERT(device && "makeSampler(linear) called with stale/null Metal device handle");
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
  DXMT_ASSERT(device && "makeSampler(snapshot) called with stale/null Metal device handle");
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
  DXMT_ASSERT(device && "makeSampler(FlatStateSet) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

// R-BACK-15.7 / spec section 4.2: depth/stencil DontCare-store look-ahead.
// Walks the remaining records in the current chunk starting from
// `startCommandIndex + 1` and returns true when one of two proofs holds:
//
//   1. The next record that touches `depthHandle` is a Clear of that
//      handle (the original G3 simple-form shortcut).
//   2. The depth handle never reappears in the rest of the chunk AND
//      no Present was seen during the walk (H1 broadening, R-BACK-15.7
//      end-of-chunk fall-through). R-BACK-15.9 still applies — no
//      cross-chunk look-ahead — but within the current chunk the depth
//      is provably dead, so DontCare-store is safe.
//
// Any prior live read or surface op on the handle (Readback /
// SurfaceCopy / StretchRect / ColorFill source-or-dest, a DrawRun that
// re-binds the handle as depth target, or a DrawRun that samples it as
// a shadow-map texture) flips the proof to defensive Store.
//
// Public so the G4 render-pass-actions fixture can exercise the
// contract without a Metal device.
bool nextDepthOperationIsClear(const core::ChunkSlot& slot,
                               std::size_t startCommandIndex,
                               core::Handle depthHandle) {
  if (!depthHandle) {
    return false;
  }
  using Kind = core::MetalCommandKind;
  bool sawPresent = false;
  for (std::size_t i = startCommandIndex + 1; i < slot.commandCount(); ++i) {
    const auto next = slot.commandAt(i);
    switch (next.kind) {
      case Kind::Clear:
        if (next.clear && next.clear->depthStencil.handle == depthHandle) {
          // R-BACK-15.7: the next op on this depth handle is a clear,
          // so storing tile contents would be wasted bandwidth.
          return true;
        }
        break;
      case Kind::DrawRun:
        if (next.drawState.hot) {
          if (next.drawState.hot->depthStencil.handle == depthHandle) {
            // Depth is read by a subsequent draw — must Store.
            return false;
          }
          // R-BACK-15.7 extension: depth-as-shadow-map sample. Walk the
          // active texture bindings and bail if any matches the depth
          // handle (the depth surface is sampled as a texture by this
          // later draw, so its tile contents must be preserved).
          const auto& textures = next.drawState.hot->textures;
          const std::uint32_t mask = next.drawState.hot->textureMask;
          for (std::size_t s = 0; s < textures.size(); ++s) {
            if ((mask & (1u << s)) == 0) continue;
            if (textures[s] == depthHandle) {
              return false;
            }
          }
        }
        break;
      case Kind::SurfaceCopy:
        if (next.surfaceCopy &&
            (next.surfaceCopy->source == depthHandle ||
             next.surfaceCopy->destination == depthHandle)) {
          return false;
        }
        break;
      case Kind::StretchRect:
        if (next.stretchRect &&
            (next.stretchRect->source == depthHandle ||
             next.stretchRect->destination == depthHandle)) {
          return false;
        }
        break;
      case Kind::Readback:
        // R-BACK-15.15: host-visible read of the depth surface must not
        // be served from a DontCare-stored tile.
        if (next.readback &&
            (next.readback->source == depthHandle ||
             next.readback->destination == depthHandle)) {
          return false;
        }
        break;
      case Kind::ColorFill:
        if (next.colorFill && next.colorFill->destination == depthHandle) {
          return false;
        }
        break;
      case Kind::Present:
        // R-BACK-15.13: a Present in this chunk implies the frame may
        // persist depth state across the chunk boundary. Don't return
        // early — a later Clear on the same handle still wins (it
        // proves the tile contents are about to be discarded), but if
        // we fall through to end-of-chunk we must Store defensively.
        sawPresent = true;
        break;
    }
  }
  // R-BACK-15.7 end-of-chunk fall-through. Default keeps the defensive
  // sawPresent guard: a Present in this chunk implies the frame may
  // persist depth state across the chunk boundary (cross-frame shadow
  // map / depth-test reuse), so we Store. Setting
  // DXMT9_AGGRESSIVE_DEPTH_DONTCARE=1 drops the guard so the look-ahead
  // returns DontCare whenever the depth handle does not reappear in
  // the chunk, even when a Present is present. Empirically this is the
  // SFIV win path (Present-per-chunk pattern zeroes the conservative
  // form). Use only on workloads known not to read depth across
  // frames; depth-as-shadow-map within the same chunk is still
  // protected by the texture-sample scan above.
  static const bool aggressive = []() {
    if (const char* v = std::getenv("DXMT9_AGGRESSIVE_DEPTH_DONTCARE")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  if (aggressive) {
    return true;
  }
  return !sawPresent;
}

WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    core::FlatDrawStateView drawState,
    const std::optional<ClearDesc>& clear,
    const core::ChunkSlot* lookaheadSlot,
    std::size_t lookaheadStartIndex) {
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
    const bool srgbWrite =
        core::flatStateOr(hot.renderStates, core::RS_SRGB_WRITE_ENABLE, 0u) != 0;
    attachment.texture =
        (srgbWrite && surface->srgbTexture) ? surface->srgbTexture.handle
                                            : surface->texture.handle;
    const bool discardAttachment = discardAfterPresent && i == 0;
    const bool clearAttachment =
        clearMatchesColorAttachment(clear, i, hot.colorAttachments[i].handle);
    // R-BACK-15.4: first-use of a color RT (handle not yet in the
    // queue-local touched set) may DontCare-load. Precedence:
    // Clear > post-present DontCare > first-use DontCare > Load.
    const bool firstUseAttachment =
        !clearAttachment && !discardAttachment &&
        !ctx.queue.isColorHandleTouched(hot.colorAttachments[i].handle);
    attachment.load_action = clearAttachment       ? WMTLoadActionClear
                           : discardAttachment     ? WMTLoadActionDontCare
                           : firstUseAttachment    ? WMTLoadActionDontCare
                                                   : WMTLoadActionLoad;
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
    // R-BACK-15.7 simple form (specs/backend/render-pass-actions/design.md
    // section 4.2): in-chunk look-ahead — if the very next op on this
    // depth handle is a Clear, the about-to-be-stored tile contents are
    // immediately discarded, so we can DontCare-store. R-BACK-15.14:
    // never DontCare an MSAA depth target with an attached resolve.
    const bool hasResolveTarget = static_cast<bool>(depthSurface->resolveTexture);
    const bool depthDontCareStore =
        lookaheadSlot != nullptr &&
        hot.depthStencil.handle &&
        !hasResolveTarget &&
        nextDepthOperationIsClear(*lookaheadSlot, lookaheadStartIndex,
                                  hot.depthStencil.handle);
    if (formatHasDepthAspect(depthSurface->desc.format)) {
      passInfo.depth.texture = depthSurface->texture.handle;
      passInfo.depth.load_action = clearDepth ? WMTLoadActionClear : WMTLoadActionLoad;
      passInfo.depth.store_action =
          depthDontCareStore ? WMTStoreActionDontCare : WMTStoreActionStore;
      if (clearDepth) {
        passInfo.depth.clear_depth = clear->depth;
      }
    }
    if (formatHasStencilAspect(depthSurface->desc.format)) {
      passInfo.stencil.texture = depthSurface->texture.handle;
      passInfo.stencil.load_action = clearStencil ? WMTLoadActionClear : WMTLoadActionLoad;
      // The simple-form shortcut clears the entire depth/stencil surface
      // on the next op, so stencil tile contents are equally discardable.
      passInfo.stencil.store_action =
          depthDontCareStore ? WMTStoreActionDontCare : WMTStoreActionStore;
      if (clearStencil) {
        passInfo.stencil.clear_stencil = clear->stencil;
      }
    }
  }

  // R-BACK-15.10/15.11/15.12: emit per-attachment load/store action
  // histograms + tile-preservation byte estimates so scripts/
  // assert_perf_counters.py and the SFIV smoke can see the policy
  // outcome. Counted before encoder open so the counters land even when
  // the encoder fails to open below.
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    auto* surface = ctx.pool.findSurface(hot.colorAttachments[i].handle.value);
    if (!surface || !surface->texture) continue;
    const auto& att = passInfo.colors[i];
    perf::countRenderPassLoadActionColor(static_cast<std::uint32_t>(att.load_action));
    perf::countRenderPassStoreActionColor(static_cast<std::uint32_t>(att.store_action));
    const std::uint64_t pixelBytes =
        static_cast<std::uint64_t>(surface->desc.width) *
        static_cast<std::uint64_t>(surface->desc.height) *
        static_cast<std::uint64_t>(core::bytesPerPixel(surface->desc.format));
    if (att.load_action == WMTLoadActionLoad) {
      perf::countRenderPassTilePreservationBytes(pixelBytes);
    }
    if (att.store_action == WMTStoreActionStore ||
        att.store_action == WMTStoreActionMultisampleResolve) {
      perf::countRenderPassTilePreservationBytes(pixelBytes);
    }
  }
  if (auto* depthSurface = ctx.pool.findSurface(hot.depthStencil.handle.value);
      depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    const std::uint64_t depthPixelBytes =
        static_cast<std::uint64_t>(depthSurface->desc.width) *
        static_cast<std::uint64_t>(depthSurface->desc.height) *
        static_cast<std::uint64_t>(core::bytesPerPixel(depthSurface->desc.format));
    if (formatHasDepthAspect(depthSurface->desc.format)) {
      perf::countRenderPassLoadActionDepth(
          static_cast<std::uint32_t>(passInfo.depth.load_action));
      perf::countRenderPassStoreActionDepth(
          static_cast<std::uint32_t>(passInfo.depth.store_action));
      if (passInfo.depth.load_action == WMTLoadActionLoad) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
      }
      if (passInfo.depth.store_action == WMTStoreActionStore ||
          passInfo.depth.store_action == WMTStoreActionMultisampleResolve) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
      }
    }
    if (formatHasStencilAspect(depthSurface->desc.format)) {
      perf::countRenderPassLoadActionStencil(
          static_cast<std::uint32_t>(passInfo.stencil.load_action));
      perf::countRenderPassStoreActionStencil(
          static_cast<std::uint32_t>(passInfo.stencil.store_action));
      if (passInfo.stencil.load_action == WMTLoadActionLoad) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
      }
      if (passInfo.stencil.store_action == WMTStoreActionStore ||
          passInfo.stencil.store_action == WMTStoreActionMultisampleResolve) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
      }
    }
  }

  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) {
    return {};
  }
  perf::countRenderPassBegin();
  // R-BACK-14.3 — issue `useHeap` once per heap instance that actually
  // backs a resource bound on this encoder. Walking the active draw
  // state's stream/index buffers + sampler textures and consulting each
  // record's `isHeapBacked` flag avoids the over-issue case where every
  // pool heap (including heaps holding resources unrelated to this
  // encoder) was made resident at encoder open. The dedup buffer is
  // sized to the static binding cap (kMaxStreams streams + 1 index
  // buffer + kMaxSamplers texture stages = 33 bindings, all of which
  // share the same handful of heap instances per family) so this stays
  // a fixed-size, allocation-free walk on the encoder-open hot path.
  {
    constexpr std::size_t kMaxBoundHeaps =
        core::kMaxStreams + 1u + core::kMaxSamplers;
    std::array<obj_handle_t, kMaxBoundHeaps> usedHeaps{};
    std::size_t usedHeapCount = 0;
    auto pushHeap = [&](WMT::Heap heap) {
      const obj_handle_t h = heap.handle;
      if (h == 0) return;
      for (std::size_t i = 0; i < usedHeapCount; ++i) {
        if (usedHeaps[i] == h) return;
      }
      if (usedHeapCount < usedHeaps.size()) {
        usedHeaps[usedHeapCount++] = h;
      }
    };
    auto considerBuffer = [&](core::Handle handle) {
      if (!handle) return;
      if (auto* rec = ctx.pool.findBuffer(handle.value); rec && rec->isHeapBacked) {
        pushHeap(rec->heap);
      }
    };
    auto considerTexture = [&](core::Handle handle) {
      if (!handle) return;
      if (auto* rec = ctx.pool.findTexture(handle.value); rec && rec->isHeapBacked) {
        pushHeap(rec->heap);
      }
    };
    considerBuffer(hot.indexBuffer);
    for (const auto& streamHandle : hot.streamBuffers) {
      considerBuffer(streamHandle);
    }
    for (const auto& textureHandle : hot.textures) {
      considerTexture(textureHandle);
    }
    for (std::size_t i = 0; i < usedHeapCount; ++i) {
      encoder.useHeap(WMT::Heap{usedHeaps[i]});
      perf::countUseHeap();
    }
  }
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
                 uniform::DirtyState* dirty,
                 bool tileFfpMode,
                 bool argbufHybridMode) {
  // Hot per-draw entry. Per codebase_conventions.rules.md, no heap allocation
  // is permitted on this path; the guard is debug-only and asserts this when
  // DXMT_DEBUG_NO_PER_DRAW_ALLOC=1 is set in env. See dxmt9_debug_alloc_guard.
  DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE("encodeDraw");
  PerfScope scope(perf::countEncodeDrawCpuTime);
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
  std::optional<DebugGroupScope> drawDebugGroup;
  if (!suppressRecordedMetalCalls(ctx)) {
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
  const bool traceEncode = debug::shouldTraceEncode(hot, seqId);
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
  // Phase 3-E: pipeline lookup + depth state + setRenderPipelineState
  // are BaseDrawState-only and survive across iterations of a
  // Kind::DrawRun on the Metal render encoder. Skip on iter 2..N.
  if (!skipBaseStateBind) {
    PerfScope pipelineLookupScope(perf::countEncodeDrawPipelineLookupCpuTime);
    const auto depthKey = makeDepthStencilKey(drawState);
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
    if (suppressBaseStateLookup(ctx)) {
      pipeline = ctx.drawRecorder->renderPipelineState;
    } else {
      pipelineRef = ctx.cache.getOrBuildDrawPipelineForState(
          ctx.device, ctx.limits, ctx.pool, drawState, ctx.shaderArchive,
          ctx.shaderArchivePath, tileFfpMode, argbufHybridMode).get();
      pipeline = WMT::RenderPipelineState{pipelineRef.handle};
    }
    if (!pipeline) {
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
      depthStateRef = ctx.cache.depthStencilStateFor(ctx.device, depthKey);
      depthState = WMT::DepthStencilState{depthStateRef.handle};
    }
    if (depthState) {
      recordedSetDepthStencilState(ctx, encoder, depthState);
      countDepthStateBind();
    }
    // M1: label the pipeline with the shader-variant hash so frame
    // captures show "pso_h<hash>" instead of an anonymous pipeline.
    // The MTL setLabel: API is idempotent — no harm if the same PSO
    // is bound across frames; we keep the most recent label. Skipping
    // when hash == 0 (no shader context) is intentional.
    {
      const auto variantHash = shaderVariantHashForDraw(drawState);
      if (variantHash != 0 && !suppressRecordedMetalCalls(ctx)) {
        WMT::RenderPipelineState psoView{pipeline.handle};
        psoView.setLabel(makeLabelStringFmt("pso_h%016llx",
            static_cast<unsigned long long>(variantHash)));
      }
    }
    if (tileFfpMode) {
      // R-BACK-13.6: tile-shader FFP path. The render encoder hosts
      // both render and tile commands; setTileRenderPipelineState binds
      // the tile-stage variant built from makeFfpTilePixelSource and
      // dispatchThreadsPerTile launches one tile-stage thread per
      // tile. Metal computes the pass tile size from the bound
      // attachment shape and exposes it via tileWidth/tileHeight on
      // the encoder; query that as the source of truth and fall back
      // to 16x16 (Apple GPU's typical tile dimension) if Metal returns
      // 0 — older OS or unsupported attachment shapes.
      // threadgroup_size_matches_tile_size = 1 on the descriptor side
      // keeps Metal in agreement with the dispatch.
      encoder.setTileRenderPipelineState(pipeline);
      uint64_t tileW = encoder.tileWidth();
      uint64_t tileH = encoder.tileHeight();
      if (tileW == 0u || tileH == 0u) {
        tileW = 16u;
        tileH = 16u;
      }
      encoder.dispatchThreadsPerTile(WMTSize{tileW, tileH, 1u});
      countPipelineBind();
    } else {
      recordedSetRenderPipelineState(ctx, encoder, pipeline);
      countPipelineBind();
    }
  }
  auto uploadTransientBuffer = [&](const void* data, std::size_t len, std::size_t alignment) {
    return ctx.queue.uploadTransientBuffer(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len),
        alignment, seqId);
  };
  // Per-frequency UBO bind sequence (R-BACK-12.5/12.8). Each category
  // sub-allocates from the existing transient slab pool, builds its
  // struct via the A2 transform, binds to its slot, and clears the
  // dirty bit. Stale (non-dirty) categories rely on the previous
  // draw's sticky binding on the same Metal render encoder.
  uniform::DirtyState scratchDirty;
  uniform::markAllDirty(scratchDirty);
  uniform::DirtyState* dirtyPtr = dirty ? dirty : &scratchDirty;
  // R-BACK-12.24 — Stage 2 argbuf dirty mirror.
  //
  // When the encoder is on the argbuf-hybrid path AND any per-frequency
  // bit is dirty, mirror the dirty regions into the argbuf so the cbuf
  // [[id(0..3)]] entries point at fresh transient slabs. The Stage 1
  // slot 0 / slot 3 binds below STILL run unmodified — the
  // Stage 2 shaders dereference these cbuf entries through slot 30; the
  // direct slot 0 / slot 3 binds below are retained as harmless Stage 1
  // compatibility shadowing while the backend keeps both paths observable.
  if (argbufHybridMode) {
    const auto bytes = dxmt9::argbuf_hybrid::updateDirtyArgbufRegions(
        ctx.queue, ctx.queue.argbufEncoderResource(), drawState, *dirtyPtr,
        seqId);
    if (bytes != 0) {
      perf::countArgbufHybridBytes(bytes);
    }
    // Do NOT clear the dirty bits here — the direct binding loop below
    // still consumes them so Stage 1-observable slot 0 / slot 3 mirrors
    // remain consistent with the Stage 2 argbuf entries.
  }
  {
    PerfScope uniformBuildScope(perf::countEncodeDrawUniformBuildCpuTime);
    if (uniform::anyDirty(*dirtyPtr, uniform::kVsAny)) {
      VsConsts vs = buildVsConsts(drawState);
      auto slice = uploadTransientBuffer(&vs, sizeof(VsConsts), alignof(VsConsts));
      if (slice) {
        recordedSetVertexBuffer(ctx, encoder, slice.buffer, slice.offset, 0);
        countUniformBufferBinds(1);
        perf::countUniformVsConsts(sizeof(VsConsts));
        uniform::clearBits(*dirtyPtr, uniform::kVsAny);
      }
    }
    if (uniform::anyDirty(*dirtyPtr, uniform::kPsAny)) {
      PsConsts ps = buildPsConsts(drawState);
      auto slice = uploadTransientBuffer(&ps, sizeof(PsConsts), alignof(PsConsts));
      if (slice) {
        if (!suppressRecordedMetalCalls(ctx)) {
          encoder.setFragmentBuffer(slice.buffer, slice.offset, 0);
        }
        countUniformBufferBinds(1);
        perf::countUniformPsConsts(sizeof(PsConsts));
        uniform::clearBits(*dirtyPtr, uniform::kPsAny);
      }
    }
    if (uniform::anyDirty(*dirtyPtr, uniform::kFfpPsAny)) {
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
    auto slice = uploadTransientBuffer(host, sizeof(FfpVsConsts), alignof(FfpVsConsts));
    if (slice) {
      if (argbufHybridMode) {
        dxmt9::argbuf_hybrid::pointFfpVsAtSlice(
            ctx.queue.argbufEncoderResource(), slice.buffer, slice.offset);
      }
      recordedSetVertexBuffer(ctx, encoder, slice.buffer, slice.offset, 3);
      countUniformBufferBinds(1);
      perf::countUniformFfpVs(sizeof(FfpVsConsts));
      uniform::clearBits(*dirtyPtr, uniform::kFfpVsAny);
      ffpVsBound = true;
    }
  };
  std::optional<dxmt9::ffp::FixedFunctionVertexLayout> ffLayout;
  bool fixedFunctionPath = false;
  {
    PerfScope fvfDecodeScope(perf::countEncodeDrawFvfDecodeCpuTime);
    ffLayout = decodeFixedFunctionVertexLayout(vertexDecl);
    fixedFunctionPath = drawUsesFixedFunctionPath(drawState, static_cast<bool>(ffLayout));
  }
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
  if (!skipBaseStateBind) {
    PerfScope streamBindViewportScope(perf::countEncodeDrawStreamBindCpuTime);
    if (auto* surface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value); surface && surface->texture) {
      const auto rasterPlan = makeEncoderRasterStatePlan(
          hot,
          surface->desc.width,
          surface->desc.height,
          ffLayout && ffLayout->preTransformed,
          debug::disableScissor(),
          debug::disableCull());
      recordedSetViewport(ctx, encoder, rasterPlan.viewport);
      countViewportBind();
      recordedSetScissorRect(ctx, encoder, rasterPlan.scissor);
      countScissorBind();
      setRasterizerCullMode(ctx, encoder, hot.renderStates, rasterPlan.cullMode);
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
  {
    PerfScope fvfDecodeBytesScope(perf::countEncodeDrawFvfDecodeCpuTime);
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
        if (traceEncode) {
          std::ostringstream trace;
          trace << "[dxmt9-encode-stream] seq=" << static_cast<unsigned long long>(seqId)
                << " stream=0 slot=1 handle="
                << static_cast<unsigned long long>(hot.streamBuffers[0].value)
                << " liveMetal=0x" << std::hex
                << static_cast<unsigned long long>(buffer->buffer.handle)
                << " capturedMetal=0x"
                << static_cast<unsigned long long>(hot.streamBuffer0Metal)
                << std::dec
                << " boundMetal=0x" << std::hex
                << static_cast<unsigned long long>(vertexBuffer.handle)
                << std::dec
                << " offset=" << vertexBufferOffset
                << " stride=" << hot.streamStrides[0]
                << " shadowBytes=" << buffer->shadow.size()
                << " contents=" << (buffer->contents ? 1 : 0);
          emitQueueTraceLine(trace.str());
        }
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
      PerfScope uniformBuildFfScope(perf::countEncodeDrawUniformBuildCpuTime);
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
      PerfScope streamBindFfScope(perf::countEncodeDrawStreamBindCpuTime);
      recordedSetVertexBuffer(ctx, encoder, vertexBuffer, vertexBufferOffset, 1);
      countVertexBufferBind();
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
      emitQueueTraceLine(trace.str());
    }
    {
      PerfScope uniformBuildVsScope(perf::countEncodeDrawUniformBuildCpuTime);
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
      PerfScope streamBindVsScope(perf::countEncodeDrawStreamBindCpuTime);
      recordedSetVertexBuffer(ctx, encoder, vertexBuffer, vertexBufferOffset, 1);
      countVertexBufferBind();
      for (const auto& streamBinding :
           makeProgrammableVsExtraStreamBindings(vertexDecl, hot, pv)) {
        WMT::Buffer extraVertexBuffer{};
        uint64_t extraVertexBufferOffset = streamBinding.offset;
        const u32 stream = streamBinding.stream;
        uint64_t liveMetalHandle = 0;
        std::size_t shadowBytes = 0;
        bool hasContents = false;
        bool usedDeclBytes = false;
        if (hot.streamBuffers[stream]) {
          if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[stream].value);
              buffer && buffer->buffer) {
            extraVertexBuffer = WMT::Buffer{buffer->buffer.handle};
            liveMetalHandle = buffer->buffer.handle;
            shadowBytes = buffer->shadow.size();
            hasContents = buffer->contents != nullptr;
          }
        }
        if (!extraVertexBuffer && vertexDecl.streams[stream].buffer) {
          const auto bytes = vertexDecl.streams[stream].buffer->bytes();
          if (!bytes.empty()) {
            if (auto slice = makeTransientBuffer(bytes.data(), bytes.size())) {
              extraVertexBuffer = slice.buffer;
              extraVertexBufferOffset += slice.offset;
              usedDeclBytes = true;
            }
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
                << " offset=" << extraVertexBufferOffset
                << " stride=" << streamBinding.stride
                << " declFallback=" << (usedDeclBytes ? 1 : 0)
                << " shadowBytes=" << shadowBytes
                << " contents=" << (hasContents ? 1 : 0)
                << " bound=" << (extraVertexBuffer ? 1 : 0);
          emitQueueTraceLine(trace.str());
        }
        if (extraVertexBuffer) {
          recordedSetVertexBuffer(ctx, encoder, extraVertexBuffer,
                                  extraVertexBufferOffset,
                                  streamBinding.metalSlot);
          countVertexBufferBind();
        }
      }
    }
  }
  // Phase 3-E: texture / sampler binding is BaseDrawState-only.
  // R-BACK-12.24 — Stage 2 shaders read textures/samplers from the
  // argbuf. Re-populate on each base-state bind so a same-pass draw that
  // changes descriptors does not sample stale argbuf entries.
  if (!skipBaseStateBind) {
    PerfScope streamBindTexScope(perf::countEncodeDrawStreamBindCpuTime);
    if (argbufHybridMode) {
      dxmt9::argbuf_hybrid::populateResourceBindings(
          ctx.device, ctx.pool, ctx.queue.argbufEncoderResource(), drawState);
    }
    for (const auto& binding : makeFragmentTextureSamplerBindings(hot)) {
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
        const bool srgbTexture =
            core::flatStateOr(hot.samplerStates[stage], core::SAMP_SRGB_TEXTURE, 0u) != 0;
        recordedSetFragmentTexture(ctx, encoder,
                                   resources::textureForShaderRead(*texture, srgbTexture),
                                   static_cast<std::uint8_t>(stage));
        countTextureBind();
      }
      WMT::Reference<WMT::SamplerState> samplerRef;
      WMT::SamplerState sampler{};
      if (suppressBaseStateLookup(ctx)) {
        sampler = ctx.drawRecorder->fragmentSamplerState;
      } else {
        samplerRef = makeSampler(ctx.device, binding.samplerStates);
        sampler = WMT::SamplerState{samplerRef.handle};
      }
      if (sampler) {
        recordedSetFragmentSamplerState(ctx, encoder, sampler,
                                        static_cast<std::uint8_t>(stage));
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
        << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
        << " draw rt0=" << static_cast<unsigned long long>(hot.colorAttachments[0].handle.value)
        << " ds=" << static_cast<unsigned long long>(hot.depthStencil.handle.value)
        << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
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
      PerfScope fvfDecodeExpandedScope(perf::countEncodeDrawFvfDecodeCpuTime);
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
          ffLayout ? (drawVertexStreamStride ? drawVertexStreamStride : ffLayout->stride)
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
          recordedSetVertexBuffer(ctx, encoder, transientVertexBuffer.buffer,
                                  transientVertexBuffer.offset, 1);
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
          drawVertexStreamOffset = 0;
          drawVertexBaseIndex = 0;
          expandedIndexedDraw = true;
        }
      }

      std::ostringstream resultTrace;
      resultTrace << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
                  << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
                  << " expanded=" << (expandedIndexedDraw ? 1 : 0);
      emitQueueTraceLine(resultTrace.str());
    }
    auto pushDrawVolatile = [&] {
      const DrawVolatile vol = buildDrawVolatile(drawVertexBaseIndex, drawVertexStreamOffset,
                                                  drawVertexStreamStride);
      recordedSetVertexBytes(ctx, encoder, &vol, sizeof(DrawVolatile), 5);
      perf::countUniformVolatilePush();
    };
    if (expandedIndexedDraw) {
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
        PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
        recordedDrawPrimitives(ctx, encoder, primitiveType, 0, (uint64_t)vertexCount);
      }
      return true;
    }
    CommandQueue::TransientBufferSlice transientIndexBuffer;
    WMT::Buffer indexBuffer{};
    uint64_t indexBufferOffset = static_cast<uint64_t>(pv.startIndex) * indexElementSize(pv.indexType);
    {
      PerfScope streamBindIndexScope(perf::countEncodeDrawStreamBindCpuTime);
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
      }
    }
    if (indexBuffer) {
      const bool upDraw = !pv.userVertexData.empty() || !pv.userIndexData.empty();
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
        PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
        recordedDrawIndexedPrimitives(ctx, encoder, primitiveType,
                                      toIndexType(pv.indexType),
                                      (uint64_t)vertexCount, indexBuffer,
                                      indexBufferOffset, 1, 0, 0);
      }
      return true;
    }
  }
  const bool upDraw = !pv.userVertexData.empty();
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
    const DrawVolatile vol = buildDrawVolatile(drawVertexBaseIndex, drawVertexStreamOffset,
                                                drawVertexStreamStride);
    recordedSetVertexBytes(ctx, encoder, &vol, sizeof(DrawVolatile), 5);
    perf::countUniformVolatilePush();
    PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
    recordedDrawPrimitives(ctx, encoder, primitiveType, 0, (uint64_t)vertexCount);
  }
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

  // M3 — Metal frame capture: ask the controller whether this chunk is
  // the first chunk of the target frame. If so, start capture BEFORE
  // `newCommandBuffer()` so Apple's MTLCaptureManager records every CB
  // we create. Capture stays open across every chunk of the target
  // frame; `notePresentChunkForCapture` later returns the request when
  // the target frame's Present chunk is encoded, and that request is
  // attached to the record so the queue's commit closes the capture.
  std::optional<core::metalcapture::MetalCaptureRequest> earlyCaptureRequest =
      ctx.queue.metalCaptureForChunkBegin(slot.seqId);
  bool captureAlreadyStartedAtChunkBegin = false;
  if (earlyCaptureRequest.has_value()) {
    captureAlreadyStartedAtChunkBegin =
        core::metalcapture::startMetalCapture(WMT::Device{ctx.device.handle},
                                               *earlyCaptureRequest);
  }

  auto commandBuffer = ctx.queue.newCommandBuffer();
  if (!commandBuffer) {
    if (captureAlreadyStartedAtChunkBegin && earlyCaptureRequest.has_value()) {
      core::metalcapture::stopMetalCapture(*earlyCaptureRequest);
    }
    return std::nullopt;
  }
  bool commandBufferHasWork = false;
  // Chunk's GPU seqId — feeds every transient-buffer reservation in this
  // chunk so the slab is retained until the matching command buffer
  // completes. R-BACK-12.24 argbuf populator threads this through to
  // `reserveTransientBuffer` / `uploadTransientBuffer`.
  const u64 encodeChunkSeqId = slot.seqId;

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
  // R-BACK-13.1 / 13.6 — current render encoder's chosen FFP path. Set
  // at startRenderPass via selectTileFfpForPass; consulted on each draw
  // to decide whether a mid-pass change forces a portable fallback
  // resplit (tileFfpMidPassResplit / tileFfpFallbackByReason{mid_pass_ineligible}).
  bool activePassUsesTileFfp = false;
  // R-BACK-12.22 / 12.24 — current render encoder's argbuf-hybrid state.
  // Set at startRenderPass when the per-pass selector chose Stage 2 AND
  // the queue-owned encoder resource initialized successfully. Consumed
  // by encodeDraw via the dirty-region mid-pass rewrite. The pass is
  // sticky (R-BACK-12.22 sentence 2: never mid-pass switch).
  //
  // `activeArgbufStorage` / `activeArgbufOffset` track the current pass's
  // backing transient slab. The slot-30 vert/frag bind is issued once at
  // startRenderPass; encodeDraw never re-binds slot 30 because the
  // per-encoder argbuf is sticky. The handle/offset is retained here for
  // future rotation work (when a single argbuf no longer fits the pass)
  // — the design today reserves one argbuf per encoder and rewrites
  // sub-regions in place.
  bool activePassUsesArgbufHybrid = false;
  [[maybe_unused]] WMT::Buffer activeArgbufStorage{};
  [[maybe_unused]] std::uint64_t activeArgbufOffset = 0;
  std::optional<core::FlatDrawStateKey> activeDrawStateKey;
  std::optional<core::ClearDesc> pendingClear;
  // R-BACK-15.4: color attachment handles bound on the active render
  // encoder. Captured in startRenderPass; flushRender marks each one
  // touched on the queue so the next pass on the same handle uses
  // Load (R-BACK-15.4 says "first use" only).
  std::array<core::Handle, core::kMaxRenderTargets> activeColorHandles{};
  // Per-render-encoder uniform dirty state (R-BACK-12.12). Seeded from
  // ctx.dirty so bits accumulated by the chunk-record importer since
  // the last encode flow into the first draw of this chunk; the
  // startRenderPass lambda calls markAllDirty whenever a fresh Metal
  // render encoder opens (sticky bindings are lost on encoder
  // boundary). encodeDraw reads + clears bits as it sub-allocates and
  // binds per-frequency UBOs.
  uniform::DirtyState uniformDirty = ctx.dirty;

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
      // M2: pop the render-pass debug group pushed in startRenderPass.
      // Must happen before endEncoding — the encoder rejects further
      // commands once endEncoding fires.
      activeRenderEncoder.popDebugGroup();
      activeRenderEncoder.endEncoding();
      perf::countRenderPassEnd(reason);
      // R-BACK-15.4: color attachments stored on this pass become "touched"
      // on the queue so the next pass on the same handle Loads instead of
      // DontCare-loads. Color store_action is currently always Store or
      // MultisampleResolve (R-BACK-15.8 color DontCare-store not yet
      // implemented), so every active color handle qualifies.
      for (auto& handle : activeColorHandles) {
        if (handle) {
          ctx.queue.markColorHandleTouched(handle);
          handle = core::Handle{};
        }
      }
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
                             const std::optional<core::ClearDesc>& clear,
                             std::size_t lookaheadStartIndex) {
    // TLA+: EncoderLifecycle / BeginRender(rt)
    // Callers split through None before opening a new render encoder.
    // R-BACK-15.7: pass the slot + current command index so beginRenderPass
    // can run the depth/stencil DontCare-store look-ahead over the
    // remaining records.
    assertNoActiveEncoder();
    activeRenderEncoder = beginRenderPass(ctx, commandBuffer, drawState, clear,
                                          &slot, lookaheadStartIndex);
    hasActiveRender = static_cast<bool>(activeRenderEncoder);
    activeKey = makeAttachmentKey(*drawState.hot);
    activeWriteHazard = makeAttachmentHazard(*drawState.hot);
    activeDrawStateKey.reset();
    // R-BACK-13.1 — per-pass tile-shader FFP selector. Eligibility is
    // computed once at encoder open; the choice is sticky for the pass.
    // Counters: each opened pass bumps exactly one of
    // tileFfpPassCount / portableFfpPassCount, plus the by-reason
    // breakdown when the precision/unsupported_state path forced a
    // fallback. R-BACK-13.5: gpu_family is recorded but only via the
    // dedicated tileFfpFallbackGpuFamily counter, not the pass count.
    {
      const auto selection =
          dxmt9::pipeline::selectTileFfpForPass(drawState, ctx.pool.supportsApple3());
      activePassUsesTileFfp = selection.decision == dxmt9::pipeline::TileFfpDecision::Tile;
      if (activePassUsesTileFfp) {
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
      activeColorHandles[i] = drawState.hot->colorAttachments[i].handle;
    }
    // M2: push a debug group identifying the render pass attachments.
    // Paired with the popDebugGroup() at the head of flushRender.
    //
    // Also set the encoder label with the same string. The debug group is
    // visible in Xcode's frame capture (.gputrace), but xctrace's
    // metal-application-encoders-list schema reports only the encoder
    // label, so without setLabel xctrace shows the Metal default
    // "Render Command N" and per-pass GPU time cannot be attributed to
    // an RT in text-based analysis.
    if (activeRenderEncoder) {
      const auto rt0 = static_cast<unsigned long long>(
          drawState.hot->colorAttachments[0].handle.value);
      const auto depth = static_cast<unsigned long long>(
          drawState.hot->depthStencil.handle.value);
      auto passLabel = makeLabelStringFmt(
          "RenderPass[rt=0x%llx,depth=0x%llx]", rt0, depth);
      activeRenderEncoder.setLabel(passLabel);
      activeRenderEncoder.pushDebugGroup(passLabel);
    }
    // R-BACK-12.22 / 12.24 / 12.25 — Stage 2 argbuf-hybrid per-encoder
    // populator. The selector reads the cached capability bool on the
    // pool. When the gate holds AND the queue-owned encoder resource
    // initialized successfully, the populator reserves the argbuf
    // storage from the transient ring, points the queue's
    // MTLArgumentEncoder at it, writes the four per-frequency cbuf
    // entries + the texture/sampler descriptors, and binds slot 30
    // (vertex + fragment) of the active render encoder. Stage 1's
    // slot 0 / slot 3 binds in encodeDraw are still issued as Stage 1
    // compatibility shadowing, but Stage 2 PSOs read through this slot-30
    // argbuf.
    //
    // When the gate fails (any non-Apple-Silicon device) `openArgbuf`
    // returns an empty handle and we fall through to the Stage 1
    // counter; no slot-30 bind is issued.
    activePassUsesArgbufHybrid = false;
    activeArgbufStorage = {};
    activeArgbufOffset = 0;
    {
      const auto argbufDecision = dxmt9::pipeline::selectArgbufHybridForPass(
          drawState, ctx.pool.argbufHybridEnabled());
      if (argbufDecision == dxmt9::pipeline::ArgbufHybridDecision::Stage2) {
        perf::countArgbufHybridEncoder();
        auto& encoderResource = ctx.queue.argbufEncoderResource();
        const auto populated = dxmt9::argbuf_hybrid::openArgbuf(
            ctx.queue, encoderResource, encodeChunkSeqId);
        if (populated) {
          // Populate texture / sampler entries at encoder open — the
          // direct fragment-stage texture/sampler binds in encodeDraw
          // are skipped on the Stage 2 path, so the argbuf is the
          // single source of truth for [[id(4..19)]]. Constant-buffer
          // entries (VsConsts/PsConsts/FfpVsConsts/FfpPsConsts) are
          // populated lazily from encodeDraw's dirty path on the first
          // draw; the encoder bind ordering only requires the argbuf
          // slot to be bound here.
          dxmt9::argbuf_hybrid::populateResourceBindings(
              ctx.device, ctx.pool, encoderResource, drawState);
          // Bind slot 30 — vertex + fragment. The render encoder reads
          // from this single argbuf for the duration of the pass; the
          // slot-30 bind is the only argbuf-related bind on the encoder
          // (per design.md §11.2; setVertexBytes(slot=5) / vertex stream
          // slot 1 stay direct).
          activeRenderEncoder.setVertexBuffer(populated.storage,
                                              populated.offset,
                                              dxmt9::shaders::kArgbufHybridBindSlot);
          activeRenderEncoder.setFragmentBuffer(populated.storage,
                                                populated.offset,
                                                dxmt9::shaders::kArgbufHybridBindSlot);
          // R-BACK-12.25 — upload accounting. `populated.length` is the
          // argbuf descriptor-table size (matches the encoder's reported
          // encodedLength); per-frequency cbuf bytes are bumped by
          // updateDirtyArgbufRegions on the first draw.
          perf::countArgbufHybridBytes(populated.length);
          activePassUsesArgbufHybrid = true;
          activeArgbufStorage = populated.storage;
          activeArgbufOffset = populated.offset;
        } else {
          // Selector chose Stage 2 but the encoder resource didn't init
          // (sentinel-null device, test fixture, or transient ring
          // exhaustion). R-BACK-12.22 sentence 2: never mid-pass switch
          // — the pass commits to Stage 1 for its lifetime. Fallback
          // counter bumps so a regression that turns this from "rare"
          // into "common" surfaces.
          perf::countArgbufHybridFallback();
        }
      } else {
        perf::countStage1Encoder();
        // Stage 1 byte total so the regression test in design.md §11.5
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
    uniform::markAllDirty(uniformDirty);
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

  // R-BACK-2.29..2.32 — mid-chunk MTLCommandBuffer split. Mirrors
  // splitBeforeBlockingPresent exactly: open the next sub-CB on the same
  // queue, commit the current one (timing the commit), swap, and reset the
  // hasWork bit. CRITICAL invariants:
  //   * Must NEVER be called while an encoder is active. The natural call
  //     site after flushRender(non-Final) already satisfies this — flushRender
  //     ends the active render encoder. Helper-encoder paths
  //     (SurfaceCopy/StretchRect/Readback/ColorFill) own and end their own
  //     short-lived encoders, so calling splitMidChunk after they return is
  //     also safe. Callers must ensure flushBlit() has run if a blit encoder
  //     could be open.
  //   * Must NEVER be called between the present record's encoder open and
  //     the chain tail. The Present arm flushes + calls
  //     splitBeforeBlockingPresent already; do NOT add another split there.
  // Sub-CB completion order is guaranteed by Metal's same-queue in-order
  // submission (R-BACK-2.32). Per-chunk commits (mid + final) are folded
  // into chunkSubCBCountMax via updateMax at chunk exit so the table
  // surfaces both total mid-chunk commits and the worst-case chain length.
  std::uint64_t perChunkSubCBCount = 0;
  auto splitMidChunk = [&] {
    if (!commandBufferHasWork) return;
    auto next = ctx.queue.newCommandBuffer();
    if (!next) return;
    const auto commitStarted = std::chrono::steady_clock::now();
    commandBuffer.commit();
    perf::countCommandBufferCommitCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - commitStarted).count()));
    perf::countSubCommandBufferCommit();
    ++perChunkSubCBCount;
    commandBuffer = std::move(next);
    commandBufferHasWork = false;
  };

  const auto commitPolicy = midChunkCommitPolicy();
  const std::uint32_t splitNRecords = midChunkCommitNRecords();
  const std::uint32_t splitChainCap = midChunkCommitCapPerRenderPass();
  std::uint32_t recordsSinceLastSplit = 0;
  // R-BACK-2.33 — splitMidChunkUnderCap wraps splitMidChunk so callers
  // do not need to repeat the cap check at every split site. cap=0
  // disables the cap (unbounded chain) for diagnostic comparison.
  // perChunkSubCBCount counts mid-chunk commits already issued; the
  // chain tail (final commit at chunk exit) is implicit, so the cap
  // applies to mid-chunk commits + 1 = chain length.
  auto splitMidChunkUnderCap = [&] {
    if (splitChainCap > 0 && perChunkSubCBCount + 1 >= splitChainCap) {
      return;
    }
    splitMidChunk();
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
        // R-BACK-13.6 — mid-pass eligibility. When the active encoder is on
        // the tile path and the next draw's state has become ineligible
        // (e.g. alpha-test reference flipped out of [0,1], fog mode flipped
        // to Exp/Exp2), force a render-pass split so the next encoder opens
        // on the portable path. A pass that opened on the portable path
        // stays portable regardless (portable handles every state).
        auto tileMidPassIneligible = [&]() {
          if (!hasActiveRender || !activePassUsesTileFfp) return false;
          const auto sel =
              dxmt9::pipeline::selectTileFfpForPass(stateView, ctx.pool.supportsApple3());
          return sel.decision != dxmt9::pipeline::TileFfpDecision::Tile;
        };
        if (pendingClear.has_value()) {
          const auto clearKey = makeAttachmentKey(*pendingClear);
          const auto clearHazard = makeAttachmentHazard(*pendingClear);
          if (clearKey == drawKey && !clearHazard.exactOverlaps(drawReadHazard)) {
            startRenderPass(stateView, pendingClear, commandIndex);
            pendingClear.reset();
          } else {
            flushPendingClear();
            const bool renderTargetChanged = hasActiveRender && activeKey != drawKey;
            const bool hazardDetected =
                hasActiveRender && !renderTargetChanged && hasExactRenderHazard();
            const bool tileResplit =
                hasActiveRender && !renderTargetChanged && !hazardDetected &&
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
            if (!hasActiveRender || renderTargetChanged || hazardDetected || tileResplit) {
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
              // R-BACK-2.29..2.32 — per-render-pass policy commits the
              // current sub-CB at every non-Final flushRender. Encoder is
              // already ended by flushRender, so the splitMidChunk
              // invariant (no active encoder) holds. Skip when policy
              // is off so the default 1 CB/chunk behavior is preserved.
              if (commitPolicy == MidChunkCommitPolicy::PerRenderPass) {
                splitMidChunkUnderCap();
              }
              startRenderPass(stateView, std::nullopt, commandIndex);
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
          const bool tileResplit =
              hasActiveRender && !renderTargetChanged && !hazardDetected &&
              tileMidPassIneligible();
          if (tileResplit) {
            // R-BACK-13.6 — see twin call site above.
            perf::countTileFfpMidPassResplit();
            perf::countTileFfpFallbackMidPassIneligible();
          }
          if (!hasActiveRender || renderTargetChanged || hazardDetected || tileResplit) {
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
            // R-BACK-2.29..2.32 — see twin call site above. The split
            // reason here can be Final when neither RT-change nor hazard
            // forced the flush, but per-render-pass policy still
            // commits to start a new sub-CB before the next pass opens.
            if (commitPolicy == MidChunkCommitPolicy::PerRenderPass) {
              splitMidChunkUnderCap();
            }
            startRenderPass(stateView, std::nullopt, commandIndex);
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
        // Per-frequency UBOs (VsConsts/PsConsts/FfpVsConsts/FfpPsConsts)
        // bind only on dirty (R-BACK-12.5/12.8); DrawVolatile is pushed
        // via setVertexBytes per draw with no slab traffic.
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
                         recordPayloadArena,
                         &uniformDirty,
                         /*tileFfpMode=*/activePassUsesTileFfp,
                         /*argbufHybridMode=*/activePassUsesArgbufHybrid)) {
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
        // R-BACK-15.5: destination handle's contents are overwritten;
        // the next render pass on it qualifies as first-use again.
        ctx.queue.invalidateColorHandle(command.surfaceCopy->destination);
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
        // R-BACK-15.5
        ctx.queue.invalidateColorHandle(command.stretchRect->destination);
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
        // R-BACK-15.5: destination receives content; source is unaffected
        ctx.queue.invalidateColorHandle(command.readback->destination);
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
        // R-BACK-15.5
        ctx.queue.invalidateColorHandle(command.colorFill->destination);
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
          // Bump the controller's frame counter and, if this is the
          // target frame's Present chunk, recover the chunk-begin session
          // request so `record.metalCapture` triggers stopCapture at
          // commit time. For non-target frames the call is a no-op apart
          // from the counter bump.
          metalCaptureRequest = ctx.queue.notePresentChunkForCapture(slot.seqId);
          if (metalCaptureRequest.has_value()) {
            // Capture was started at an earlier chunk-begin; this
            // chunk's commit should only call stopCapture, never
            // re-start.
            captureAlreadyStartedAtChunkBegin = true;
          }
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
    // record; splitBeforeBlockingPresent owns the present-tail boundary.
    ++recordsSinceLastSplit;
    if (commitPolicy == MidChunkCommitPolicy::PerNRecords &&
        recordsSinceLastSplit >= splitNRecords &&
        command.kind != Kind::Present) {
      flushBlit();
      flushRender(perf::EncoderSplitReason::Final);
      assertNoActiveEncoder();
      splitMidChunkUnderCap();
      recordsSinceLastSplit = 0;
    }
  }

  flushPendingClear();
  flushRender(perf::EncoderSplitReason::Final);
  flushBlit();

  // R-BACK-2.29..2.32 — fold the chunk's local sub-CB chain length into
  // chunkSubCBCountMax. The chain length includes every mid-chunk commit
  // (counted via splitMidChunk above) PLUS the final commit performed by
  // the queue once this record is returned, so the per-chunk count is
  // perChunkSubCBCount + 1. Always at least 1 for a non-empty chunk.
  if (commandBufferHasWork || perChunkSubCBCount > 0) {
    perf::recordChunkSubCBCount(perChunkSubCBCount + 1);
  }

  const u64 seqId = slot.seqId;
  core::metalqueue::QueueSubmissionRecord record;
  record.commandBuffer = std::move(commandBuffer);
  if (metalCaptureRequest.has_value()) {
    record.metalCaptureDevice = WMT::Device{ctx.device.handle};
    record.metalCapture = std::move(metalCaptureRequest);
    record.metalCaptureAlreadyStarted = captureAlreadyStartedAtChunkBegin;
  }
  record.slotIndex = slotIndex;
  record.seqId = seqId;
  record.context = "queue";
  record.postCommitCallbacks = std::move(postCommitCallbacks);
  return record;
  }  // @autoreleasepool
}

}  // namespace dxmt9::encoders
