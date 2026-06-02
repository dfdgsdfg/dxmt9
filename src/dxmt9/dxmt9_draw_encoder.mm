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
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string>
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

bool colorAttachmentAliasesTracedTexture(resources::Pool& pool,
                                         const core::FlatDrawStateRecord& hot,
                                         std::size_t* attachmentIndex = nullptr) {
  const u64 wanted = debug::traceTextureHandle();
  if (wanted == 0) {
    return false;
  }
  for (std::size_t i = 0; i < hot.colorAttachments.size(); ++i) {
    const auto handle = hot.colorAttachments[i].handle;
    if (!handle) {
      continue;
    }
    const auto* surface = pool.findSurface(handle.value);
    if (surface && surface->aliasTexture.value == wanted) {
      if (attachmentIndex) {
        *attachmentIndex = i;
      }
      return true;
    }
  }
  return false;
}

void traceRenderTargetWriteForTexture(resources::Pool& pool,
                                      const core::FlatDrawStateRecord& hot,
                                      u64 seqId,
                                      u64 drawOrdinal) {
  std::size_t attachmentIndex = 0;
  if (!colorAttachmentAliasesTracedTexture(pool, hot, &attachmentIndex)) {
    return;
  }
  const auto handle = hot.colorAttachments[attachmentIndex].handle;
  std::ostringstream out;
  out << "[dxmt9-texture] render-target-write seq="
      << static_cast<unsigned long long>(seqId)
      << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
      << " colorIndex=" << attachmentIndex
      << " surface=0x" << std::hex << handle.value << std::dec
      << " texture=0x" << std::hex << debug::traceTextureHandle() << std::dec
      << " colorWrite=" << core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu)
      << " srgbWrite=" << core::flatStateOr(hot.renderStates, core::RS_SRGB_WRITE_ENABLE, 0u)
      << " alphaBlend=" << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
      << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
      << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
      << " tex0=0x" << std::hex << hot.textures[0].value << std::dec;
  emitTextureTraceLine(out.str());
}

thread_local DrawBindingPacketCache gDrawBindingPacketCache;

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

void recordedSetBlendColorAndStencilRef(EncodeContext& ctx,
                                        WMT::RenderCommandEncoder& encoder,
                                        float red,
                                        float green,
                                        float blue,
                                        float alpha,
                                        std::uint8_t stencilRef) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setBlendColorAndStencilRef) {
    recorder->setBlendColorAndStencilRef(
        recorder->userdata, red, green, blue, alpha, stencilRef);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setBlendColorAndStencilRef(red, green, blue, alpha, stencilRef);
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

void recordedSetVertexTexture(EncodeContext& ctx,
                              WMT::RenderCommandEncoder& encoder,
                              WMT::Texture texture,
                              std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexTexture) {
    recorder->setVertexTexture(recorder->userdata, texture, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexTexture(texture, index);
  }
}

void recordedSetVertexSamplerState(EncodeContext& ctx,
                                   WMT::RenderCommandEncoder& encoder,
                                   WMT::SamplerState sampler,
                                   std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexSamplerState) {
    recorder->setVertexSamplerState(recorder->userdata, sampler, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexSamplerState(sampler, index);
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

void countTextureBindSkipped() {
  perf::countBaseStateBindSkip(1, 0);
}

void countSamplerBindSkipped() {
  perf::countBaseStateBindSkip(0, 1);
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

bool x8ShaderAlphaFillEnabledForDiagnostics();

struct IndexReuseMeasure {
  u64 references = 0;
  u64 unique = 0;
  u64 cacheMiss16 = 0;
  u64 cacheMiss32 = 0;
  u64 cacheMiss64 = 0;
  u32 minIndex = 0;
  u32 maxIndex = 0;
  u32 firstIndex = 0;
  u32 lastIndex = 0;
  u64 adjacentDeltaAbsSum = 0;
  u32 adjacentDeltaMax = 0;
  u64 backwardJumps = 0;
  u64 triangleIndexSpanSum = 0;
  u32 triangleIndexSpanMax = 0;
  bool available = false;
};

struct ActiveEncoderBreakdown {
  static_assert(core::kMaxStreams <= perf::kEncoderBreakdownMaxStreams);

  template <std::size_t Size>
  struct CbufHistory {
    bool valid = false;
    u64 validBytes = 0;
    std::array<std::byte, Size> bytes{};
  };

  struct StreamBindReason {
    bool first = false;
    bool handleChange = false;
    bool offsetChange = false;
  };

  struct VsOutLayoutCacheEntry {
    bool valid = false;
    u64 sourceKey = 0;
    bool tileFfpMode = false;
    u32 layoutKey = 0;
  };

  struct ShaderSourceHashCacheEntry {
    bool valid = false;
    u64 sourceKey = 0;
    bool tileFfpBaseColor = false;
    bool argbufHybridMode = false;
    bool argbufResourceArray = false;
    bool samplerLodBias = false;
    u32 x8AlphaOneTextureMask = 0;
    u64 vertexSourceHash = 0;
    u64 pixelSourceHash = 0;
  };

  template <std::size_t Capacity>
  struct UniqueHandleSet {
    static constexpr std::size_t kCapacity = Capacity;
    std::array<u64, kCapacity> handles{};
    std::size_t count = 0;
    bool overflowed = false;
  };

  enum class TransientVertexSource {
    User,
    Preupload,
    DeclFallback,
    ExpandedMain,
    ExpandedExtra,
  };

  enum class TransientIndexSource {
    User,
    Preupload,
    ShadowFallback,
    ProbeReorder,
    OptimizedOrder,
  };

  bool enabled = false;
  perf::EncoderBreakdown stats{};
  bool ibValid = false;
  u64 ibHandle = 0;
  bool psoValid = false;
  u64 psoHandle = 0;
  bool shaderVariantValid = false;
  u64 shaderVariant = 0;
  bool vsOutLayoutValid = false;
  u32 vsOutLayout = 0;
  bool blendStateValid = false;
  u64 blendState = 0;
  std::array<StreamBindReason, perf::kEncoderBreakdownMaxStreams> streamBindReasons{};
  UniqueHandleSet<2048> streamUniqueHandles{};
  std::array<UniqueHandleSet<512>, perf::kEncoderBreakdownMaxStreams> streamUniqueHandlesByStream{};
  UniqueHandleSet<2048> ibUniqueHandles{};
  UniqueHandleSet<2048> psoUniqueHandles{};
  UniqueHandleSet<2048> shaderVariantUnique{};
  UniqueHandleSet<2048> vsOutLayoutUnique{};
  UniqueHandleSet<2048> blendStateUnique{};
  UniqueHandleSet<2048> x8RtTextureBindingUniqueHandles{};
  UniqueHandleSet<4096> drawGeometrySignatures{};
  std::array<VsOutLayoutCacheEntry, 128> vsOutLayoutCache{};
  std::size_t vsOutLayoutCacheNext = 0;
  std::array<ShaderSourceHashCacheEntry, 128> shaderSourceHashCache{};
  std::size_t shaderSourceHashCacheNext = 0;
  CbufHistory<sizeof(VsConsts)> vsHistory{};
  CbufHistory<sizeof(FfpVsConsts)> ffpVsHistory{};
  bool drawGeometrySignatureValid = false;
  u64 drawGeometrySignatureLast = 0;

  void begin(u64 seqId, u64 encoderIndex, u64 rtHandle, u64 depthHandle) {
    enabled = perf::encoderBreakdownEnabled();
    const auto seqFilter = perf::encoderBreakdownSeqFilter();
    if (enabled && seqFilter != 0 && seqId != seqFilter) {
      enabled = false;
    }
    stats = {};
    ibValid = false;
    ibHandle = 0;
    psoValid = false;
    psoHandle = 0;
    shaderVariantValid = false;
    shaderVariant = 0;
    vsOutLayoutValid = false;
    vsOutLayout = 0;
    blendStateValid = false;
    blendState = 0;
    streamBindReasons = {};
    streamUniqueHandles = {};
    streamUniqueHandlesByStream = {};
    ibUniqueHandles = {};
    psoUniqueHandles = {};
    shaderVariantUnique = {};
    vsOutLayoutUnique = {};
    blendStateUnique = {};
    x8RtTextureBindingUniqueHandles = {};
    drawGeometrySignatures = {};
    vsOutLayoutCache = {};
    vsOutLayoutCacheNext = 0;
    shaderSourceHashCache = {};
    shaderSourceHashCacheNext = 0;
    vsHistory = {};
    ffpVsHistory = {};
    drawGeometrySignatureValid = false;
    drawGeometrySignatureLast = 0;
    stats.seqId = seqId;
    stats.encoderIndex = encoderIndex;
    stats.rtHandle = rtHandle;
    stats.depthHandle = depthHandle;
    if (!enabled) {
      return;
    }
  }

  void recordAttachmentMetadata(const resources::Pool& pool,
                                const core::FlatDrawStateRecord& hot) {
    if (!enabled) {
      return;
    }
    auto fillSurface = [&](core::Handle handle,
                           u64& format,
                           u64& width,
                           u64& height,
                           u64& bytesPerPixel,
                           u64& aliasTexture,
                           u64& textureUsage,
                           u64& formatSwizzle,
                           u64& textureNeedsView) {
      const auto* surface = pool.findSurface(handle.value);
      if (!surface) {
        return;
      }
      format = static_cast<u64>(surface->desc.format);
      width = surface->desc.width;
      height = surface->desc.height;
      bytesPerPixel = core::bytesPerPixel(surface->desc.format);
      formatSwizzle = convert::formatNeedsShaderReadSwizzle(surface->desc.format) ? 1u : 0u;
      aliasTexture = surface->aliasTexture.value;
      const auto* texture =
          surface->aliasTexture ? pool.findTexture(surface->aliasTexture.value) : nullptr;
      if (!texture) {
        return;
      }
      textureUsage = texture->desc.usage;
      textureNeedsView = convert::textureNeedsShaderReadView(texture->desc) ? 1u : 0u;
    };

    fillSurface(hot.colorAttachments[0].handle,
                stats.rtFormat,
                stats.rtWidth,
                stats.rtHeight,
                stats.rtBytesPerPixel,
                stats.rtAliasTexture,
                stats.rtTextureUsage,
                stats.rtFormatNeedsShaderReadSwizzle,
                stats.rtTextureNeedsShaderReadView);
    fillSurface(hot.depthStencil.handle,
                stats.depthFormat,
                stats.depthWidth,
                stats.depthHeight,
                stats.depthBytesPerPixel,
                stats.depthAliasTexture,
                stats.depthTextureUsage,
                stats.depthFormatNeedsShaderReadSwizzle,
                stats.depthTextureNeedsShaderReadView);
  }

  void recordFragmentTextureBinding(u32 stage,
                                    core::Handle textureHandle,
                                    const resources::TextureRecord* texture) {
    if (!enabled || !texture || !textureHandle || stage >= 64u) {
      return;
    }
    ++stats.fragmentTextureBindingSamples;
    stats.fragmentTextureBindingMaskOr |= 1ull << stage;
    const bool x8Format =
        texture->desc.format == core::Format::X8R8G8B8 ||
        texture->desc.format == core::Format::X8B8G8R8;
    const bool renderTargetTexture =
        (texture->desc.usage & core::UsageRenderTarget) != 0u ||
        (texture->desc.usage & core::UsageDepthStencil) != 0u;
    if (!x8Format || !renderTargetTexture) {
      return;
    }
    ++stats.x8RtTextureBindingSamples;
    stats.x8RtTextureBindingMaskOr |= 1ull << stage;
    stats.x8RtTextureBindingLastStage = stage;
    stats.x8RtTextureBindingLastHandle = textureHandle.value;
    if (convert::textureNeedsShaderReadView(texture->desc)) {
      ++stats.x8RtTextureBindingShaderReadViewSamples;
    }
    if (x8ShaderAlphaFillEnabledForDiagnostics()) {
      ++stats.x8ShaderAlphaFillSamples;
      stats.x8ShaderAlphaFillMaskOr |= 1ull << stage;
    }
    if (stats.rtAliasTexture != 0 && textureHandle.value == stats.rtAliasTexture) {
      ++stats.x8RtTextureBindingActiveRtAliasSamples;
    }
    recordUnique(x8RtTextureBindingUniqueHandles,
                 textureHandle.value,
                 stats.x8RtTextureBindingUniqueHandles,
                 stats.x8RtTextureBindingUniqueHandleOverflows);
  }

  void emit(perf::EncoderSplitReason reason) {
    if (!enabled) {
      return;
    }
    stats.endReason = reason;
    perf::emitEncoderBreakdown(stats);
    enabled = false;
  }

  static u64 triangleEstimateFor(core::PrimitiveType primitiveType,
                                 u32 primitiveCount) {
    switch (primitiveType) {
      case core::PrimitiveType::TriangleList:
      case core::PrimitiveType::TriangleStrip:
      case core::PrimitiveType::TriangleFan:
        return primitiveCount;
      default:
        return 0;
    }
  }

  static u64 mixSignature(u64 seed, u64 value) {
    return drawBindingPacketHashMix(seed, value);
  }

  u64 makeDrawGeometrySignature(core::PrimitiveType primitiveType,
                                u32 primitiveCount,
                                u64 vertexCount,
                                bool indexed,
                                bool expandedIndexed,
                                bool fixedFunction,
                                bool preTransformed,
                                u32 textureMask,
                                u32 stream0Stride,
                                i32 drawVertexBaseIndex,
                                u32 drawVertexStreamOffset,
                                u32 startIndex,
                                core::IndexType indexType,
                                const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
                                const core::ViewportScissor& viewport,
                                WMTCullMode cullMode,
                                WMTTriangleFillMode fillMode) const {
    u64 seed = 0x7be1d1f73c46a715ull;
    seed = mixSignature(seed, static_cast<u32>(primitiveType));
    seed = mixSignature(seed, primitiveCount);
    seed = mixSignature(seed, vertexCount);
    seed = mixSignature(seed, indexed ? 1ull : 0ull);
    seed = mixSignature(seed, expandedIndexed ? 1ull : 0ull);
    seed = mixSignature(seed, fixedFunction ? 1ull : 0ull);
    seed = mixSignature(seed, preTransformed ? 1ull : 0ull);
    seed = mixSignature(seed, textureMask);
    seed = mixSignature(seed, stream0Stride);
    seed = mixSignature(seed, static_cast<u64>(static_cast<std::int64_t>(drawVertexBaseIndex)));
    seed = mixSignature(seed, drawVertexStreamOffset);
    seed = mixSignature(seed, startIndex);
    seed = mixSignature(seed, static_cast<u32>(indexType));
    seed = mixSignature(seed, psoHandle);
    seed = mixSignature(seed, shaderVariant);
    seed = mixSignature(seed, vertexShaderHashForSignature());
    seed = mixSignature(seed, pixelShaderHashForSignature());
    seed = mixSignature(seed, vsOutLayout);
    seed = mixSignature(seed, ibValid ? ibHandle : 0ull);
    for (const auto& stream : stats.streams) {
      if (!stream.valid) {
        continue;
      }
      seed = mixSignature(seed, stream.lastHandle);
      seed = mixSignature(seed, stream.lastOffset);
      seed = mixSignature(seed, stream.lastStride);
    }
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_COLOR_WRITE_ENABLE, 0xfu));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_Z_ENABLE, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_Z_FUNC, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_SRC_BLEND, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_DEST_BLEND, 0u));
    seed = mixSignature(seed, static_cast<u32>(cullMode));
    seed = mixSignature(seed, static_cast<u32>(fillMode));
    seed = mixSignature(seed, viewport.scissorEnabled ? 1ull : 0ull);
    seed = mixSignature(seed, static_cast<u32>(viewport.scissor.left));
    seed = mixSignature(seed, static_cast<u32>(viewport.scissor.top));
    seed = mixSignature(seed, static_cast<u32>(viewport.scissor.right));
    seed = mixSignature(seed, static_cast<u32>(viewport.scissor.bottom));
    return seed ? seed : 1ull;
  }

  u64 vertexShaderHashForSignature() const {
    return stats.vertexShaderLast;
  }

  u64 pixelShaderHashForSignature() const {
    return stats.pixelShaderLast;
  }

  void recordDrawGeometrySignature(u64 signature) {
    ++stats.drawGeometrySignatureSamples;
    stats.drawGeometrySignatureLast = signature;
    if (drawGeometrySignatureValid && drawGeometrySignatureLast == signature) {
      ++stats.drawGeometrySignatureConsecutiveDuplicates;
    }
    drawGeometrySignatureValid = true;
    drawGeometrySignatureLast = signature;

    for (std::size_t i = 0; i < drawGeometrySignatures.count; ++i) {
      if (drawGeometrySignatures.handles[i] == signature) {
        ++stats.drawGeometrySignatureDuplicates;
        return;
      }
    }
    if (drawGeometrySignatures.count >= drawGeometrySignatures.handles.size()) {
      if (!drawGeometrySignatures.overflowed) {
        drawGeometrySignatures.overflowed = true;
        ++stats.drawGeometrySignatureUniqueOverflows;
      }
      return;
    }
    drawGeometrySignatures.handles[drawGeometrySignatures.count++] = signature;
    ++stats.drawGeometrySignatureUnique;
  }

  void recordDrawSize(u32 primitiveCount, u64 vertexCount) {
    if (stats.drawPrimitiveCountMin == 0 || primitiveCount < stats.drawPrimitiveCountMin) {
      stats.drawPrimitiveCountMin = primitiveCount;
    }
    if (primitiveCount > stats.drawPrimitiveCountMax) {
      stats.drawPrimitiveCountMax = primitiveCount;
    }
    if (stats.drawVertexCountMin == 0 || vertexCount < stats.drawVertexCountMin) {
      stats.drawVertexCountMin = vertexCount;
    }
    if (vertexCount > stats.drawVertexCountMax) {
      stats.drawVertexCountMax = vertexCount;
    }

    if (primitiveCount < 64) {
      ++stats.drawPrimitiveBucket1_63;
    } else if (primitiveCount < 256) {
      ++stats.drawPrimitiveBucket64_255;
    } else if (primitiveCount < 1024) {
      ++stats.drawPrimitiveBucket256_1023;
    } else if (primitiveCount < 4096) {
      ++stats.drawPrimitiveBucket1024_4095;
    } else {
      ++stats.drawPrimitiveBucket4096Plus;
    }

    if (vertexCount < 256) {
      ++stats.drawVertexBucket1_255;
    } else if (vertexCount < 1024) {
      ++stats.drawVertexBucket256_1023;
    } else if (vertexCount < 4096) {
      ++stats.drawVertexBucket1024_4095;
    } else if (vertexCount < 16384) {
      ++stats.drawVertexBucket4096_16383;
    } else {
      ++stats.drawVertexBucket16384Plus;
    }
  }

  void recordSplitLargeIndexedDraw(u32 primitiveCount,
                                   u32 primitiveLimit,
                                   u64 stream0SpanLimit,
                                   u64 maxChunkStream0Span,
                                   u32 metalDraws) {
    if (!enabled || metalDraws <= 1u) {
      return;
    }
    ++stats.splitLargeIndexedSourceDraws;
    stats.splitLargeIndexedMetalDraws += metalDraws;
    stats.splitLargeIndexedExtraDraws += static_cast<u64>(metalDraws - 1u);
    stats.splitLargeIndexedPrimitiveCount += primitiveCount;
    if (primitiveLimit != 0u) {
      stats.splitLargeIndexedPrimitiveLimit =
          stats.splitLargeIndexedPrimitiveLimit == 0
              ? primitiveLimit
              : std::min<std::uint64_t>(stats.splitLargeIndexedPrimitiveLimit,
                                        primitiveLimit);
    }
    if (stream0SpanLimit != 0u) {
      stats.splitLargeIndexedStream0SpanLimit =
          stats.splitLargeIndexedStream0SpanLimit == 0
              ? stream0SpanLimit
              : std::min<std::uint64_t>(stats.splitLargeIndexedStream0SpanLimit,
                                        stream0SpanLimit);
    }
    stats.splitLargeIndexedChunkStream0SpanMax =
        std::max(stats.splitLargeIndexedChunkStream0SpanMax,
                 maxChunkStream0Span);
  }

  void recordIndexedOrderProbe(bool applied, u64 bytes) {
    if (!enabled) {
      return;
    }
    if (!applied) {
      ++stats.indexedOrderProbeSkipped;
      return;
    }
    ++stats.indexedOrderProbeDraws;
    stats.indexedOrderProbeBytes += bytes;
  }

  void recordIndexedOrderOptimization(bool applied, u64 bytes) {
    if (!enabled) {
      return;
    }
    if (!applied) {
      ++stats.indexedOrderOptimizedSkipped;
      return;
    }
    ++stats.indexedOrderOptimizedDraws;
    stats.indexedOrderOptimizedBytes += bytes;
  }

  static u64 rectAreaPixels(const core::Rect& rect) {
    const auto width = std::max(0, rect.right - rect.left);
    const auto height = std::max(0, rect.bottom - rect.top);
    return static_cast<u64>(width) * static_cast<u64>(height);
  }

  void recordScissorRectProbe(bool applied,
                              const core::Rect& originalRect,
                              const core::Rect& overrideRect) {
    if (!enabled) {
      return;
    }
    if (!applied) {
      ++stats.probeScissorRectSkipped;
      return;
    }
    ++stats.probeScissorRectDraws;
    const auto originalArea = rectAreaPixels(originalRect);
    const auto overrideArea = rectAreaPixels(overrideRect);
    stats.probeScissorRectAreaDeltaPixels +=
        originalArea > overrideArea ? originalArea - overrideArea
                                    : overrideArea - originalArea;
  }

  void emitIndexedOrderProbeDraw(bool probeEligible,
                                 bool probeApplied,
                                 bool optimizedEligible,
                                 bool optimizedApplied,
                                 bool scissorRectEligible,
                                 bool scissorRectApplied,
                                 bool alphaBlendProbeApplied,
                                 bool depthWriteProbeApplied,
                                 bool depthFuncProbeApplied,
                                 bool splitEligible,
                                 bool splitWouldApply,
                                 u32 splitChunkCount,
                                 u32 splitMaxChunksPerDraw,
                                 u64 splitStream0SpanLimit,
                                 u64 splitChunkStream0SpanMax,
                                 u64 splitPrimitiveCount,
                                 u64 reorderBytes,
                                 IndexReuseMeasure originalIndexReuse,
                                 IndexReuseMeasure effectiveIndexReuse,
                                 u64 drawOrdinal,
                                 core::PrimitiveType primitiveType,
                                 u32 primitiveCount,
                                 u64 vertexCount,
                                 u32 textureMask,
                                 const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
                                 const core::ViewportScissor& viewport,
                                 WMTCullMode cullMode,
                                 WMTTriangleFillMode fillMode,
                                 i32 baseVertexIndex,
                                 u32 startIndex,
                                 core::IndexType indexType,
                                 u64 indexBufferHandle,
                                 u64 stream0Handle,
                                 u64 stream0Offset,
                                 u64 stream0Stride,
                                 const core::Rect& originalScissor) {
    if (!enabled) {
      return;
    }
    const bool depthEnabled =
        core::flatStateOr(renderStates, RS_Z_ENABLE, 0u) != 0u;
    const bool depthWrite =
        depthEnabled && core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
    const bool alphaBlendEnabled =
        core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
    const bool alphaTestEnabled =
        core::flatStateOr(renderStates, RS_ALPHA_TEST_ENABLE, 0u) != 0u;
    const bool stencilEnabled =
        core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u;
    const bool clipPlaneEnabled =
        core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u;
    const auto colorWrite =
        core::flatStateOr(renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
    const auto srcBlend = core::flatStateOr(renderStates, RS_SRC_BLEND, 0u);
    const auto dstBlend = core::flatStateOr(renderStates, RS_DEST_BLEND, 0u);
    const auto blendOp = core::flatStateOr(renderStates, RS_BLEND_OP, 0u);
    const auto separateAlpha =
        core::flatStateOr(renderStates, RS_SEPARATE_ALPHA_BLEND_ENABLE, 0u);
    const auto srcBlendAlpha =
        core::flatStateOr(renderStates, RS_SRC_BLEND_ALPHA, srcBlend);
    const auto dstBlendAlpha =
        core::flatStateOr(renderStates, RS_DEST_BLEND_ALPHA, dstBlend);
    const auto blendOpAlpha =
        core::flatStateOr(renderStates, RS_BLEND_OP_ALPHA, blendOp);
    const auto depthFunc = core::flatStateOr(
        renderStates, RS_Z_FUNC, static_cast<u32>(core::CompareFunc::LessEqual));
    auto streamByteMin = [](const IndexReuseMeasure& measure,
                            i32 baseVertex,
                            u64 streamOffset,
                            u64 streamStride) -> u64 {
      if (!measure.available || streamStride == 0u) {
        return 0u;
      }
      const auto minVertex =
          static_cast<std::int64_t>(baseVertex) +
          static_cast<std::int64_t>(measure.minIndex);
      if (minVertex < 0) {
        return 0u;
      }
      return streamOffset + static_cast<u64>(minVertex) * streamStride;
    };
    auto streamByteMax = [](const IndexReuseMeasure& measure,
                            i32 baseVertex,
                            u64 streamOffset,
                            u64 streamStride) -> u64 {
      if (!measure.available || streamStride == 0u) {
        return 0u;
      }
      const auto maxVertex =
          static_cast<std::int64_t>(baseVertex) +
          static_cast<std::int64_t>(measure.maxIndex);
      if (maxVertex < 0) {
        return 0u;
      }
      return streamOffset + static_cast<u64>(maxVertex) * streamStride;
    };
    const auto originalStreamByteMin =
        streamByteMin(originalIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    const auto originalStreamByteMax =
        streamByteMax(originalIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    const auto effectiveStreamByteMin =
        streamByteMin(effectiveIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    const auto effectiveStreamByteMax =
        streamByteMax(effectiveIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    std::fprintf(
        stderr,
        "[dxmt9-perf-indexed-probe-draw seq=%llu encoder=%llu "
        "encoder_draw_index=%llu draw_ordinal=%llu eligible=%u applied=%u "
        "optimized_eligible=%u optimized_applied=%u "
        "scissor_rect_eligible=%u scissor_rect_applied=%u "
        "alpha_blend_probe_applied=%u depth_write_probe_applied=%u "
        "depth_func_probe_applied=%u reorder_bytes=%llu "
        "split_eligible=%u split_would_apply=%u split_chunk_count=%u "
        "split_max_chunks_per_draw=%u split_stream0_span_limit=%llu "
        "split_chunk_stream0_span_max=%llu split_primitive_count=%llu "
        "original_index_available=%u original_index_unique=%llu "
        "original_index_min=%u original_index_max=%u original_index_span=%llu "
        "original_index_first=%u original_index_last=%u "
        "original_cache_miss16=%llu original_cache_miss32=%llu "
        "original_cache_miss64=%llu original_adjacent_delta_sum=%llu "
        "original_adjacent_delta_max=%u original_backward_jumps=%llu "
        "original_triangle_index_span_sum=%llu "
        "original_triangle_index_span_max=%u "
        "original_stream0_byte_min=%llu original_stream0_byte_max=%llu "
        "original_stream0_byte_span=%llu "
        "effective_index_available=%u effective_index_unique=%llu "
        "effective_index_min=%u effective_index_max=%u effective_index_span=%llu "
        "effective_index_first=%u effective_index_last=%u "
        "effective_cache_miss16=%llu effective_cache_miss32=%llu "
        "effective_cache_miss64=%llu effective_adjacent_delta_sum=%llu "
        "effective_adjacent_delta_max=%u effective_backward_jumps=%llu "
        "effective_triangle_index_span_sum=%llu "
        "effective_triangle_index_span_max=%u "
        "effective_stream0_byte_min=%llu effective_stream0_byte_max=%llu "
        "effective_stream0_byte_span=%llu "
        "primitive_type=%u primitive_count=%u vertex_count=%llu "
        "texture_mask=0x%x color_write=0x%x alpha_blend=%u "
        "src_blend=%u dst_blend=%u blend_op=%u separate_alpha=%u "
        "src_blend_alpha=%u dst_blend_alpha=%u blend_op_alpha=%u "
        "alpha_test=%u depth_enabled=%u "
        "depth_write=%u depth_func=%u stencil=%u clip_plane=%u scissor=%u "
        "scissor_l=%d scissor_t=%d scissor_r=%d scissor_b=%d "
        "original_scissor_l=%d original_scissor_t=%d "
        "original_scissor_r=%d original_scissor_b=%d "
        "cull=%u fill=%u base_vertex=%d start_index=%u index_type=%u "
        "index_buffer=0x%llx stream0_handle=0x%llx stream0_offset=%llu "
        "stream0_stride=%llu pso=0x%llx shader_variant=0x%llx "
        "vs=0x%llx ps=0x%llx vsout=0x%x]\n",
        static_cast<unsigned long long>(stats.seqId),
        static_cast<unsigned long long>(stats.encoderIndex),
        static_cast<unsigned long long>(stats.drawCalls),
        static_cast<unsigned long long>(drawOrdinal),
        probeEligible ? 1u : 0u,
        probeApplied ? 1u : 0u,
        optimizedEligible ? 1u : 0u,
        optimizedApplied ? 1u : 0u,
        scissorRectEligible ? 1u : 0u,
        scissorRectApplied ? 1u : 0u,
        alphaBlendProbeApplied ? 1u : 0u,
        depthWriteProbeApplied ? 1u : 0u,
        depthFuncProbeApplied ? 1u : 0u,
        static_cast<unsigned long long>(reorderBytes),
        splitEligible ? 1u : 0u,
        splitWouldApply ? 1u : 0u,
        splitChunkCount,
        splitMaxChunksPerDraw,
        static_cast<unsigned long long>(splitStream0SpanLimit),
        static_cast<unsigned long long>(splitChunkStream0SpanMax),
        static_cast<unsigned long long>(splitPrimitiveCount),
        originalIndexReuse.available ? 1u : 0u,
        static_cast<unsigned long long>(originalIndexReuse.unique),
        originalIndexReuse.minIndex,
        originalIndexReuse.maxIndex,
        static_cast<unsigned long long>(
            originalIndexReuse.available
                ? static_cast<u64>(originalIndexReuse.maxIndex) -
                      static_cast<u64>(originalIndexReuse.minIndex) + 1u
                : 0u),
        originalIndexReuse.firstIndex,
        originalIndexReuse.lastIndex,
        static_cast<unsigned long long>(originalIndexReuse.cacheMiss16),
        static_cast<unsigned long long>(originalIndexReuse.cacheMiss32),
        static_cast<unsigned long long>(originalIndexReuse.cacheMiss64),
        static_cast<unsigned long long>(originalIndexReuse.adjacentDeltaAbsSum),
        originalIndexReuse.adjacentDeltaMax,
        static_cast<unsigned long long>(originalIndexReuse.backwardJumps),
        static_cast<unsigned long long>(originalIndexReuse.triangleIndexSpanSum),
        originalIndexReuse.triangleIndexSpanMax,
        static_cast<unsigned long long>(originalStreamByteMin),
        static_cast<unsigned long long>(originalStreamByteMax),
        static_cast<unsigned long long>(
            originalStreamByteMax >= originalStreamByteMin
                ? originalStreamByteMax - originalStreamByteMin
                : 0u),
        effectiveIndexReuse.available ? 1u : 0u,
        static_cast<unsigned long long>(effectiveIndexReuse.unique),
        effectiveIndexReuse.minIndex,
        effectiveIndexReuse.maxIndex,
        static_cast<unsigned long long>(
            effectiveIndexReuse.available
                ? static_cast<u64>(effectiveIndexReuse.maxIndex) -
                      static_cast<u64>(effectiveIndexReuse.minIndex) + 1u
                : 0u),
        effectiveIndexReuse.firstIndex,
        effectiveIndexReuse.lastIndex,
        static_cast<unsigned long long>(effectiveIndexReuse.cacheMiss16),
        static_cast<unsigned long long>(effectiveIndexReuse.cacheMiss32),
        static_cast<unsigned long long>(effectiveIndexReuse.cacheMiss64),
        static_cast<unsigned long long>(effectiveIndexReuse.adjacentDeltaAbsSum),
        effectiveIndexReuse.adjacentDeltaMax,
        static_cast<unsigned long long>(effectiveIndexReuse.backwardJumps),
        static_cast<unsigned long long>(effectiveIndexReuse.triangleIndexSpanSum),
        effectiveIndexReuse.triangleIndexSpanMax,
        static_cast<unsigned long long>(effectiveStreamByteMin),
        static_cast<unsigned long long>(effectiveStreamByteMax),
        static_cast<unsigned long long>(
            effectiveStreamByteMax >= effectiveStreamByteMin
                ? effectiveStreamByteMax - effectiveStreamByteMin
                : 0u),
        static_cast<unsigned>(primitiveType),
        primitiveCount,
        static_cast<unsigned long long>(vertexCount),
        textureMask,
        colorWrite,
        alphaBlendEnabled ? 1u : 0u,
        srcBlend,
        dstBlend,
        blendOp,
        separateAlpha,
        srcBlendAlpha,
        dstBlendAlpha,
        blendOpAlpha,
        alphaTestEnabled ? 1u : 0u,
        depthEnabled ? 1u : 0u,
        depthWrite ? 1u : 0u,
        depthFunc,
        stencilEnabled ? 1u : 0u,
        clipPlaneEnabled ? 1u : 0u,
        viewport.scissorEnabled ? 1u : 0u,
        static_cast<int>(viewport.scissor.left),
        static_cast<int>(viewport.scissor.top),
        static_cast<int>(viewport.scissor.right),
        static_cast<int>(viewport.scissor.bottom),
        static_cast<int>(originalScissor.left),
        static_cast<int>(originalScissor.top),
        static_cast<int>(originalScissor.right),
        static_cast<int>(originalScissor.bottom),
        static_cast<unsigned>(cullMode),
        static_cast<unsigned>(fillMode),
        baseVertexIndex,
        startIndex,
        static_cast<unsigned>(indexType),
        static_cast<unsigned long long>(indexBufferHandle),
        static_cast<unsigned long long>(stream0Handle),
        static_cast<unsigned long long>(stream0Offset),
        static_cast<unsigned long long>(stream0Stride),
        static_cast<unsigned long long>(psoHandle),
        static_cast<unsigned long long>(shaderVariant),
        static_cast<unsigned long long>(stats.vertexShaderLast),
        static_cast<unsigned long long>(stats.pixelShaderLast),
        vsOutLayout);
  }

  void recordIndexedVertexReuse(IndexReuseMeasure measure) {
    if (!enabled) {
      return;
    }
    stats.indexedVertexReferenceCount += measure.references;
    if (!measure.available) {
      ++stats.indexedVertexReuseSkipped;
      return;
    }
    ++stats.indexedVertexReuseSamples;
    stats.indexedUniqueVertexEstimate += measure.unique;
    stats.indexedVertexCacheMissEstimate16 += measure.cacheMiss16;
    stats.indexedVertexCacheMissEstimate32 += measure.cacheMiss32;
    stats.indexedVertexCacheMissEstimate64 += measure.cacheMiss64;
  }

  void recordDrawIssue(core::PrimitiveType primitiveType,
                       u32 primitiveCount,
                       u64 vertexCount,
                       bool indexed,
                       bool expandedIndexed,
                       bool fixedFunction,
                       bool preTransformed,
                       u32 textureMask,
                       u32 stream0Stride,
                       i32 drawVertexBaseIndex,
                       u32 drawVertexStreamOffset,
                       i32 d3dBaseVertexIndex,
                       bool nativeBaseVertexRequested,
                       bool nativeBaseVertexUsed,
                       u32 startIndex,
                       core::IndexType indexType,
                       const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
                       const core::ViewportScissor& viewport,
                       WMTCullMode cullMode,
                       WMTTriangleFillMode fillMode) {
    if (!enabled) {
      return;
    }
    ++stats.drawCalls;
    if (indexed) {
      ++stats.indexedDraws;
      ++stats.indexedBaseVertexSamples;
      if (d3dBaseVertexIndex != 0) {
        ++stats.indexedBaseVertexNonZeroDraws;
      }
      if (d3dBaseVertexIndex < 0) {
        ++stats.indexedBaseVertexNegativeDraws;
      } else if (d3dBaseVertexIndex > 0) {
        ++stats.indexedBaseVertexPositiveDraws;
      }
      if (stats.indexedBaseVertexSamples == 1 ||
          d3dBaseVertexIndex < stats.indexedBaseVertexMin) {
        stats.indexedBaseVertexMin = d3dBaseVertexIndex;
      }
      if (stats.indexedBaseVertexSamples == 1 ||
          d3dBaseVertexIndex > stats.indexedBaseVertexMax) {
        stats.indexedBaseVertexMax = d3dBaseVertexIndex;
      }
      if (nativeBaseVertexRequested) {
        ++stats.nativeBaseVertexRequestedDraws;
        if (nativeBaseVertexUsed) {
          ++stats.nativeBaseVertexUsedDraws;
        } else if (d3dBaseVertexIndex < 0) {
          ++stats.nativeBaseVertexSkippedNegativeDraws;
        }
      }
    }
    if (expandedIndexed) {
      ++stats.expandedIndexedDraws;
    }
    if (fixedFunction) {
      ++stats.ffpDraws;
    } else {
      ++stats.programmableDraws;
    }
    if (preTransformed) {
      ++stats.preTransformedDraws;
    }
    if (textureMask != 0) {
      ++stats.texturedDraws;
    }
    switch (cullMode) {
      case WMTCullModeNone:
        ++stats.cullNoneDraws;
        break;
      case WMTCullModeFront:
        ++stats.cullFrontDraws;
        break;
      case WMTCullModeBack:
        ++stats.cullBackDraws;
        break;
    }
    switch (fillMode) {
      case WMTTriangleFillModeFill:
        ++stats.fillSolidDraws;
        break;
      case WMTTriangleFillModeLines:
        ++stats.fillWireframeDraws;
        break;
    }
    const bool depthEnabled =
        core::flatStateOr(renderStates, RS_Z_ENABLE, 0u) != 0u;
    const bool depthWrite =
        depthEnabled && core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
    const auto depthFunc = static_cast<core::CompareFunc>(core::flatStateOr(
        renderStates, RS_Z_FUNC, static_cast<u32>(core::CompareFunc::LessEqual)));
    if (depthEnabled) {
      ++stats.depthEnabledDraws;
      if (depthWrite) {
        ++stats.depthWriteDraws;
      }
      switch (depthFunc) {
        case core::CompareFunc::Less:
          ++stats.depthFuncLessDraws;
          break;
        case core::CompareFunc::LessEqual:
          ++stats.depthFuncLessEqualDraws;
          break;
        case core::CompareFunc::Always:
          ++stats.depthFuncAlwaysDraws;
          break;
        default:
          ++stats.depthFuncOtherDraws;
          break;
      }
    }
    const bool scissorEnabled = viewport.scissorEnabled;
    if (scissorEnabled) {
      ++stats.scissorEnabledDraws;
    }
    const bool alphaBlendEnabled =
        core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
    if (alphaBlendEnabled) {
      ++stats.alphaBlendEnabledDraws;
    }
    recordBlendState(renderStates);
    const bool alphaTestEnabled =
        core::flatStateOr(renderStates, RS_ALPHA_TEST_ENABLE, 0u) != 0u;
    if (alphaTestEnabled) {
      ++stats.alphaTestEnabledDraws;
      if (!debug::disableAlphaTest()) {
        ++stats.alphaTestEffectiveDraws;
      }
    }
    const bool clipPlaneEnabled =
        core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u;
    if (clipPlaneEnabled) {
      ++stats.clipPlaneEnabledDraws;
    }
    if (indexed && primitiveType == core::PrimitiveType::TriangleList) {
      auto addIndexedTriangleClass =
          [&](std::uint64_t& draws, std::uint64_t& primitives,
              std::uint64_t& vertices) {
            ++draws;
            primitives += primitiveCount;
            vertices += vertexCount;
          };
      const bool solidFill = fillMode == WMTTriangleFillModeFill;
      const bool stencilEnabled =
          core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u;
      const bool depthFuncPreservesOpaqueOrder =
          depthFunc == core::CompareFunc::Less ||
          depthFunc == core::CompareFunc::LessEqual;
      const bool opaqueDepthWrite =
          solidFill && depthWrite && depthFuncPreservesOpaqueOrder &&
          !alphaBlendEnabled && !alphaTestEnabled && !stencilEnabled &&
          !clipPlaneEnabled;
      if (opaqueDepthWrite) {
        addIndexedTriangleClass(stats.indexedTriangleOpaqueDepthWriteDraws,
                                stats.indexedTriangleOpaqueDepthWritePrimitives,
                                stats.indexedTriangleOpaqueDepthWriteVertices);
      }
      if (depthEnabled && !depthWrite) {
        addIndexedTriangleClass(stats.indexedTriangleDepthReadDraws,
                                stats.indexedTriangleDepthReadPrimitives,
                                stats.indexedTriangleDepthReadVertices);
      }
      if (alphaBlendEnabled) {
        addIndexedTriangleClass(stats.indexedTriangleAlphaBlendDraws,
                                stats.indexedTriangleAlphaBlendPrimitives,
                                stats.indexedTriangleAlphaBlendVertices);
      }
      if (scissorEnabled) {
        addIndexedTriangleClass(stats.indexedTriangleScissorDraws,
                                stats.indexedTriangleScissorPrimitives,
                                stats.indexedTriangleScissorVertices);
      }
      if (textureMask != 0) {
        addIndexedTriangleClass(stats.indexedTriangleTexturedDraws,
                                stats.indexedTriangleTexturedPrimitives,
                                stats.indexedTriangleTexturedVertices);
      }
      if (primitiveCount >= 4096) {
        addIndexedTriangleClass(stats.indexedTriangleLarge4096Draws,
                                stats.indexedTriangleLarge4096Primitives,
                                stats.indexedTriangleLarge4096Vertices);
        if (opaqueDepthWrite) {
          addIndexedTriangleClass(
              stats.indexedTriangleLarge4096OpaqueDepthWriteDraws,
              stats.indexedTriangleLarge4096OpaqueDepthWritePrimitives,
              stats.indexedTriangleLarge4096OpaqueDepthWriteVertices);
        }
        if (depthEnabled && !depthWrite) {
          addIndexedTriangleClass(stats.indexedTriangleLarge4096DepthReadDraws,
                                  stats.indexedTriangleLarge4096DepthReadPrimitives,
                                  stats.indexedTriangleLarge4096DepthReadVertices);
        }
        if (alphaBlendEnabled) {
          addIndexedTriangleClass(stats.indexedTriangleLarge4096AlphaBlendDraws,
                                  stats.indexedTriangleLarge4096AlphaBlendPrimitives,
                                  stats.indexedTriangleLarge4096AlphaBlendVertices);
        }
        if (scissorEnabled) {
          addIndexedTriangleClass(stats.indexedTriangleLarge4096ScissorDraws,
                                  stats.indexedTriangleLarge4096ScissorPrimitives,
                                  stats.indexedTriangleLarge4096ScissorVertices);
        }
        if (textureMask != 0) {
          addIndexedTriangleClass(stats.indexedTriangleLarge4096TexturedDraws,
                                  stats.indexedTriangleLarge4096TexturedPrimitives,
                                  stats.indexedTriangleLarge4096TexturedVertices);
        }
      }
    }
    switch (primitiveType) {
      case core::PrimitiveType::PointList:
        ++stats.pointDraws;
        break;
      case core::PrimitiveType::LineList:
      case core::PrimitiveType::LineStrip:
        ++stats.lineDraws;
        break;
      case core::PrimitiveType::TriangleList:
      case core::PrimitiveType::TriangleStrip:
      case core::PrimitiveType::TriangleFan:
        ++stats.triangleDraws;
        break;
    }
    stats.primitiveCount += primitiveCount;
    stats.triangleEstimate += triangleEstimateFor(primitiveType, primitiveCount);
    stats.vertexCount += vertexCount;
    stats.textureMaskOr |= textureMask;
    recordDrawSize(primitiveCount, vertexCount);
    recordDrawGeometrySignature(makeDrawGeometrySignature(
        primitiveType,
        primitiveCount,
        vertexCount,
        indexed,
        expandedIndexed,
        fixedFunction,
        preTransformed,
        textureMask,
        stream0Stride,
        indexed ? d3dBaseVertexIndex : drawVertexBaseIndex,
        drawVertexStreamOffset,
        startIndex,
        indexType,
        renderStates,
        viewport,
        cullMode,
        fillMode));
    if (stream0Stride != 0) {
      if (stats.stream0StrideMin == 0 || stream0Stride < stats.stream0StrideMin) {
        stats.stream0StrideMin = stream0Stride;
      }
      stats.stream0StrideMax = std::max<std::uint64_t>(stats.stream0StrideMax,
                                                       stream0Stride);
    }
  }

  void recordStreamState(u32 stream, u64 handle, u64 offset, u64 stride) {
    if (!enabled || stream >= stats.streams.size()) {
      return;
    }
    auto& last = stats.streams[stream];
    const bool firstSample = last.samples == 0;
    bool handleChanged = false;
    bool offsetChanged = false;
    last.valid = true;
    ++last.samples;
    ++stats.streamStateSamples;
    if (!firstSample) {
      if (last.lastHandle != handle) {
        ++last.handleChanges;
        ++stats.streamHandleChanges;
        handleChanged = true;
      }
      if (last.lastOffset != offset) {
        ++last.offsetChanges;
        ++stats.streamOffsetChanges;
        offsetChanged = true;
      }
      if (last.lastStride != stride) {
        ++last.strideChanges;
        ++stats.streamStrideChanges;
      }
    }
    streamBindReasons[stream] = StreamBindReason{
        .first = firstSample,
        .handleChange = handleChanged,
        .offsetChange = offsetChanged,
    };
    last.lastHandle = handle;
    last.lastOffset = offset;
    last.lastStride = stride;
    if (stream == 0) {
      stats.stream0LastHandle = handle;
      stats.stream0LastOffset = offset;
      stats.stream0LastStride = stride;
    }
  }

  void recordStreamMetalBind(u32 stream) {
    if (!enabled) {
      return;
    }
    ++stats.streamMetalBinds;
    if (stream < stats.streams.size()) {
      auto& slot = stats.streams[stream];
      auto& reason = streamBindReasons[stream];
      slot.valid = true;
      ++slot.metalBinds;
      if (reason.first) {
        ++slot.metalBindFirsts;
        ++stats.streamMetalBindFirsts;
      }
      if (reason.handleChange) {
        ++slot.metalBindHandleChanges;
        ++stats.streamMetalBindHandleChanges;
      }
      if (reason.offsetChange) {
        ++slot.metalBindOffsetChanges;
        ++stats.streamMetalBindOffsetChanges;
      }
      reason = {};
    }
  }

  template <std::size_t Capacity>
  bool recordUnique(UniqueHandleSet<Capacity>& set, u64 handle, u64& uniqueCounter,
                    u64& overflowCounter) {
    if (!enabled || handle == 0) {
      return false;
    }
    for (std::size_t i = 0; i < set.count; ++i) {
      if (set.handles[i] == handle) {
        return false;
      }
    }
    if (set.count >= set.handles.size()) {
      if (!set.overflowed) {
        set.overflowed = true;
        ++overflowCounter;
      }
      return false;
    }
    set.handles[set.count++] = handle;
    ++uniqueCounter;
    return true;
  }

  static void addPoolBucket(core::Pool pool,
                            u64& defaultPool,
                            u64& managedPool,
                            u64& systemMemPool,
                            u64& scratchPool) {
    switch (pool) {
      case core::Pool::Default:
        ++defaultPool;
        break;
      case core::Pool::Managed:
        ++managedPool;
        break;
      case core::Pool::SystemMem:
        ++systemMemPool;
        break;
      case core::Pool::Scratch:
        ++scratchPool;
        break;
    }
  }

  void recordStreamResource(u32 stream, u64 handle, const core::BufferDesc& desc) {
    if (recordUnique(streamUniqueHandles, handle, stats.streamUniqueHandles,
                     stats.streamUniqueHandleOverflows)) {
      stats.streamUniqueBytes += desc.size;
      if ((desc.usage & core::UsageDynamic) != 0) {
        ++stats.streamUniqueDynamicHandles;
      }
      if ((desc.usage & core::UsageWriteOnly) != 0) {
        ++stats.streamUniqueWriteOnlyHandles;
      }
      addPoolBucket(desc.pool,
                    stats.streamUniqueDefaultPoolHandles,
                    stats.streamUniqueManagedPoolHandles,
                    stats.streamUniqueSystemMemPoolHandles,
                    stats.streamUniqueScratchPoolHandles);
    }
    if (stream >= stats.streams.size()) {
      return;
    }
    auto& slot = stats.streams[stream];
    slot.valid = true;
    if (!recordUnique(streamUniqueHandlesByStream[stream], handle,
                      slot.uniqueHandles, slot.uniqueHandleOverflows)) {
      return;
    }
    slot.uniqueBytes += desc.size;
    if ((desc.usage & core::UsageDynamic) != 0) {
      ++slot.uniqueDynamicHandles;
    }
    if ((desc.usage & core::UsageWriteOnly) != 0) {
      ++slot.uniqueWriteOnlyHandles;
    }
    addPoolBucket(desc.pool,
                  slot.uniqueDefaultPoolHandles,
                  slot.uniqueManagedPoolHandles,
                  slot.uniqueSystemMemPoolHandles,
                  slot.uniqueScratchPoolHandles);
  }

  void recordIndexBufferResource(u64 handle, const core::BufferDesc& desc) {
    if (!recordUnique(ibUniqueHandles, handle, stats.ibUniqueHandles,
                      stats.ibUniqueHandleOverflows)) {
      return;
    }
    stats.ibUniqueBytes += desc.size;
    if ((desc.usage & core::UsageDynamic) != 0) {
      ++stats.ibUniqueDynamicHandles;
    }
    if ((desc.usage & core::UsageWriteOnly) != 0) {
      ++stats.ibUniqueWriteOnlyHandles;
    }
    addPoolBucket(desc.pool,
                  stats.ibUniqueDefaultPoolHandles,
                  stats.ibUniqueManagedPoolHandles,
                  stats.ibUniqueSystemMemPoolHandles,
                  stats.ibUniqueScratchPoolHandles);
  }

  void recordIndexBufferState(u64 handle) {
    if (!enabled) {
      return;
    }
    ++stats.ibStateSamples;
    if (ibValid && ibHandle != handle) {
      ++stats.ibHandleChanges;
    }
    ibValid = true;
    ibHandle = handle;
    stats.ibLastHandle = handle;
  }

  void recordIndexBufferMetalBind() {
    if (enabled) {
      ++stats.ibMetalBinds;
    }
  }

  void recordPsoState(u64 handle,
                      u64 variantHash,
                      u32 layoutKey,
                      u64 vertexShaderHash,
                      u64 pixelShaderHash,
                      u64 vertexShaderSourceHash,
                      u64 pixelShaderSourceHash) {
    if (!enabled) {
      return;
    }
    ++stats.psoStateSamples;
    if (psoValid && psoHandle != handle) {
      ++stats.psoHandleChanges;
    }
    psoValid = true;
    psoHandle = handle;
    stats.psoLastHandle = handle;
    recordUnique(psoUniqueHandles, handle, stats.psoUniqueHandles,
                 stats.psoUniqueHandleOverflows);

    if (shaderVariantValid && shaderVariant != variantHash) {
      ++stats.shaderVariantChanges;
    }
    shaderVariantValid = true;
    shaderVariant = variantHash;
    stats.shaderVariantLast = variantHash;
    stats.vertexShaderLast = vertexShaderHash;
    stats.pixelShaderLast = pixelShaderHash;
    stats.vertexShaderSourceLast = vertexShaderSourceHash;
    stats.pixelShaderSourceLast = pixelShaderSourceHash;
    recordUnique(shaderVariantUnique, variantHash, stats.shaderVariantUnique,
                 stats.shaderVariantUniqueOverflows);

    if (vsOutLayoutValid && vsOutLayout != layoutKey) {
      ++stats.vsOutLayoutChanges;
    }
    vsOutLayoutValid = true;
    vsOutLayout = layoutKey;
    stats.vsOutLayoutLast = layoutKey;
    recordUnique(vsOutLayoutUnique, static_cast<u64>(layoutKey),
                 stats.vsOutLayoutUnique, stats.vsOutLayoutUniqueOverflows);
  }

  void recordBlendState(const core::FlatStateSet<core::kMaxStateSlots>& renderStates) {
    if (!enabled) {
      return;
    }
    const auto blendEnable = core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u);
    const auto srcBlend = core::flatStateOr(
        renderStates, RS_SRC_BLEND, static_cast<u32>(core::BlendFactor::One));
    const auto dstBlend = core::flatStateOr(
        renderStates, RS_DEST_BLEND, static_cast<u32>(core::BlendFactor::Zero));
    const auto blendOp = core::flatStateOr(
        renderStates, RS_BLEND_OP, static_cast<u32>(core::BlendOp::Add));
    const auto separateAlpha = core::flatStateOr(
        renderStates, RS_SEPARATE_ALPHA_BLEND_ENABLE, 0u);
    const auto srcBlendAlpha = core::flatStateOr(
        renderStates, RS_SRC_BLEND_ALPHA, srcBlend);
    const auto dstBlendAlpha = core::flatStateOr(
        renderStates, RS_DEST_BLEND_ALPHA, dstBlend);
    const auto blendOpAlpha = core::flatStateOr(
        renderStates, RS_BLEND_OP_ALPHA, blendOp);
    const auto blendFactor = core::flatStateOr(renderStates, RS_BLEND_FACTOR, 0xffffffffu);
    const auto colorWrite = core::flatStateOr(renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);

    u64 signature = 0x61b451b9273d8fd5ull;
    signature = drawBindingPacketHashMix(signature, blendEnable);
    signature = drawBindingPacketHashMix(signature, srcBlend);
    signature = drawBindingPacketHashMix(signature, dstBlend);
    signature = drawBindingPacketHashMix(signature, blendOp);
    signature = drawBindingPacketHashMix(signature, separateAlpha);
    signature = drawBindingPacketHashMix(signature, srcBlendAlpha);
    signature = drawBindingPacketHashMix(signature, dstBlendAlpha);
    signature = drawBindingPacketHashMix(signature, blendOpAlpha);
    signature = drawBindingPacketHashMix(signature, blendFactor);
    signature = drawBindingPacketHashMix(signature, colorWrite);
    signature = signature ? signature : 1ull;

    ++stats.blendStateSamples;
    if (blendStateValid && blendState != signature) {
      ++stats.blendStateChanges;
    }
    blendStateValid = true;
    blendState = signature;
    stats.blendStateLast = signature;
    recordUnique(blendStateUnique, signature, stats.blendStateUnique,
                 stats.blendStateUniqueOverflows);

    const bool rgbNoop =
        srcBlend == static_cast<u32>(core::BlendFactor::One) &&
        dstBlend == static_cast<u32>(core::BlendFactor::Zero) &&
        blendOp == static_cast<u32>(core::BlendOp::Add);
    const bool alphaNoop =
        separateAlpha == 0u ||
        (srcBlendAlpha == static_cast<u32>(core::BlendFactor::One) &&
         dstBlendAlpha == static_cast<u32>(core::BlendFactor::Zero) &&
         blendOpAlpha == static_cast<u32>(core::BlendOp::Add));
    if (blendEnable != 0u && rgbNoop && alphaNoop) {
      ++stats.blendEnabledNoopDraws;
    }

    const auto isConstantBlend = [](u32 factor) {
      return factor == static_cast<u32>(core::BlendFactor::BlendFactor) ||
             factor == static_cast<u32>(core::BlendFactor::InvBlendFactor);
    };
    if (blendEnable != 0u &&
        (isConstantBlend(srcBlend) || isConstantBlend(dstBlend) ||
         (separateAlpha != 0u &&
          (isConstantBlend(srcBlendAlpha) || isConstantBlend(dstBlendAlpha))))) {
      ++stats.blendConstantFactorDraws;
    }
  }

  bool findCachedVsOutLayout(u64 sourceKey, bool tileFfpMode, u32& layoutKey) {
    if (!enabled) {
      return false;
    }
    for (const auto& entry : vsOutLayoutCache) {
      if (entry.valid && entry.sourceKey == sourceKey &&
          entry.tileFfpMode == tileFfpMode) {
        layoutKey = entry.layoutKey;
        ++stats.vsOutLayoutCacheHits;
        return true;
      }
    }
    ++stats.vsOutLayoutCacheMisses;
    return false;
  }

  void storeCachedVsOutLayout(u64 sourceKey, bool tileFfpMode, u32 layoutKey) {
    if (!enabled || vsOutLayoutCache.empty()) {
      return;
    }
    auto& entry = vsOutLayoutCache[vsOutLayoutCacheNext++ % vsOutLayoutCache.size()];
    entry = VsOutLayoutCacheEntry{
        .valid = true,
        .sourceKey = sourceKey,
        .tileFfpMode = tileFfpMode,
        .layoutKey = layoutKey,
    };
  }

  bool findCachedShaderSourceHashes(u64 sourceKey,
                                    bool tileFfpBaseColor,
                                    bool argbufHybridMode,
                                    bool argbufResourceArray,
                                    bool samplerLodBias,
                                    u32 x8AlphaOneTextureMask,
                                    u64& vertexSourceHash,
                                    u64& pixelSourceHash) const {
    if (!enabled) {
      return false;
    }
    for (const auto& entry : shaderSourceHashCache) {
      if (entry.valid &&
          entry.sourceKey == sourceKey &&
          entry.tileFfpBaseColor == tileFfpBaseColor &&
          entry.argbufHybridMode == argbufHybridMode &&
          entry.argbufResourceArray == argbufResourceArray &&
          entry.samplerLodBias == samplerLodBias &&
          entry.x8AlphaOneTextureMask == x8AlphaOneTextureMask) {
        vertexSourceHash = entry.vertexSourceHash;
        pixelSourceHash = entry.pixelSourceHash;
        return true;
      }
    }
    return false;
  }

  void storeCachedShaderSourceHashes(u64 sourceKey,
                                     bool tileFfpBaseColor,
                                     bool argbufHybridMode,
                                     bool argbufResourceArray,
                                     bool samplerLodBias,
                                     u32 x8AlphaOneTextureMask,
                                     u64 vertexSourceHash,
                                     u64 pixelSourceHash) {
    if (!enabled || shaderSourceHashCache.empty()) {
      return;
    }
    auto& entry =
        shaderSourceHashCache[shaderSourceHashCacheNext++ %
                              shaderSourceHashCache.size()];
    entry = ShaderSourceHashCacheEntry{
        .valid = true,
        .sourceKey = sourceKey,
        .tileFfpBaseColor = tileFfpBaseColor,
        .argbufHybridMode = argbufHybridMode,
        .argbufResourceArray = argbufResourceArray,
        .samplerLodBias = samplerLodBias,
        .x8AlphaOneTextureMask = x8AlphaOneTextureMask,
        .vertexSourceHash = vertexSourceHash,
        .pixelSourceHash = pixelSourceHash,
    };
  }

  void addArgbufTableBytes(u64 bytes) {
    if (enabled) {
      stats.argbufTableBytes += bytes;
    }
  }

  void addArgbufCbufBytes(u64 bytes) {
    if (enabled) {
      stats.argbufCbufBytes += bytes;
    }
  }

  void addArgbufCbufBytes(u32 argbufIndex, u64 bytes) {
    if (!enabled || bytes == 0) {
      return;
    }
    stats.argbufCbufBytes += bytes;
    switch (argbufIndex) {
      case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
        stats.argbufCbufVsBytes += bytes;
        break;
      case dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex:
        stats.argbufCbufFfpVsBytes += bytes;
        break;
      case dxmt9::argbuf_hybrid::kConstantBufferPsIndex:
        stats.argbufCbufPsBytes += bytes;
        break;
      case dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex:
        stats.argbufCbufFfpPsBytes += bytes;
        break;
      default:
        break;
    }
  }

  void addArgbufCbufBindings(
      const dxmt9::argbuf_hybrid::ConstantBufferBindings& bindings) {
    if (!enabled) {
      return;
    }
    for (u32 i = 0; i < bindings.entries.size(); ++i) {
      addArgbufCbufBytes(i, bindings.entries[i].bytes);
    }
  }

  struct ByteDelta {
    u64 first = 0;
    u64 changed = 0;
    u64 unchanged = 0;
  };

  template <std::size_t Size>
  ByteDelta compareRange(CbufHistory<Size>& history,
                         const std::byte* current,
                         u64 uploadBytes,
                         u64 offset,
                         u64 length) {
    ByteDelta delta{};
    const u64 begin = std::min(offset, uploadBytes);
    const u64 end = std::min(offset + length, uploadBytes);
    if (begin >= end) {
      return delta;
    }
    for (u64 i = begin; i < end; ++i) {
      if (!history.valid || i >= history.validBytes) {
        ++delta.first;
      } else if (history.bytes[static_cast<std::size_t>(i)] !=
                 current[static_cast<std::size_t>(i)]) {
        ++delta.changed;
      } else {
        ++delta.unchanged;
      }
    }
    return delta;
  }

  template <std::size_t Size>
  ByteDelta compareWhole(CbufHistory<Size>& history,
                         const std::byte* current,
                         u64 uploadBytes) {
    return compareRange(history, current, uploadBytes, 0, Size);
  }

  template <std::size_t Size>
  void updateHistory(CbufHistory<Size>& history,
                     const std::byte* current,
                     u64 uploadBytes) {
    const u64 clampedBytes = std::min<u64>(uploadBytes, Size);
    std::memcpy(history.bytes.data(), current, static_cast<std::size_t>(clampedBytes));
    history.valid = true;
    history.validBytes = std::max(history.validBytes, clampedBytes);
  }

  void recordVsUploadContent(const void* data, u64 bytes) {
    if (!enabled || !data || bytes == 0) {
      return;
    }
    const auto* current = static_cast<const std::byte*>(data);
    const u64 uploadBytes = std::min<u64>(bytes, sizeof(VsConsts));
    const auto total = compareWhole(vsHistory, current, uploadBytes);
    stats.argbufCbufVsFirstBytes += total.first;
    stats.argbufCbufVsRewriteChangedBytes += total.changed;
    stats.argbufCbufVsRewriteUnchangedBytes += total.unchanged;

    auto addVsGroup = [&](u64 offset, u64 length, u64& counter) {
      const auto delta = compareRange(vsHistory, current, uploadBytes, offset, length);
      counter += delta.changed;
    };
    addVsGroup(offsetof(VsConsts, vsFloatConst), sizeof(VsConsts::vsFloatConst),
               stats.argbufCbufVsFloatChangedBytes);
    addVsGroup(offsetof(VsConsts, vsIntConst), sizeof(VsConsts::vsIntConst),
               stats.argbufCbufVsIntChangedBytes);
    addVsGroup(offsetof(VsConsts, vsBoolConst), sizeof(VsConsts::vsBoolConst),
               stats.argbufCbufVsBoolChangedBytes);
    updateHistory(vsHistory, current, uploadBytes);
  }

  void recordVsUploadPlan(const uniform::DirtyState& dirty,
                          uniform::ShaderConstantUsageBounds usage,
                          uniform::ShaderConstantUploadPlan plan) {
    if (!enabled) {
      return;
    }
    ++stats.argbufCbufVsUploads;
    if (plan.fullStructRequired) {
      ++stats.argbufCbufVsFullStructUploads;
    }
    if (usage.unknown) {
      ++stats.argbufCbufVsUsageUnknownUploads;
    }
    if (usage.indexedFloat) {
      ++stats.argbufCbufVsUsageIndexedFloatUploads;
    }
    stats.argbufCbufVsPlanFloatRegsSum += plan.floatCount;
    stats.argbufCbufVsPlanFloatRegsMax =
        std::max<u64>(stats.argbufCbufVsPlanFloatRegsMax, plan.floatCount);
    stats.argbufCbufVsDirtyFloatRegsSum += dirty.maxChangedVsF;
    stats.argbufCbufVsDirtyFloatRegsMax =
        std::max<u64>(stats.argbufCbufVsDirtyFloatRegsMax, dirty.maxChangedVsF);
    stats.argbufCbufVsUsageFloatRegsSum += usage.floatCount;
    stats.argbufCbufVsUsageFloatRegsMax =
        std::max<u64>(stats.argbufCbufVsUsageFloatRegsMax, usage.floatCount);
  }

  void recordFfpVsUploadContent(const void* data, u64 bytes) {
    if (!enabled || !data || bytes == 0) {
      return;
    }
    const auto* current = static_cast<const std::byte*>(data);
    const u64 uploadBytes = std::min<u64>(bytes, sizeof(FfpVsConsts));
    const auto total = compareWhole(ffpVsHistory, current, uploadBytes);
    stats.argbufCbufFfpVsFirstBytes += total.first;
    stats.argbufCbufFfpVsRewriteChangedBytes += total.changed;
    stats.argbufCbufFfpVsRewriteUnchangedBytes += total.unchanged;

    auto addFfpVsGroup = [&](u64 offset, u64 length, u64& counter) {
      const auto delta = compareRange(ffpVsHistory, current, uploadBytes, offset, length);
      counter += delta.changed;
    };
    addFfpVsGroup(offsetof(FfpVsConsts, ffpWorldViewProj),
                  offsetof(FfpVsConsts, materialEmissive) -
                      offsetof(FfpVsConsts, ffpWorldViewProj),
                  stats.argbufCbufFfpVsMatrixChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, materialEmissive),
                  offsetof(FfpVsConsts, lightDiffuse) -
                      offsetof(FfpVsConsts, materialEmissive),
                  stats.argbufCbufFfpVsMaterialChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, lightDiffuse),
                  offsetof(FfpVsConsts, ffpBlendWorldViewProj) -
                      offsetof(FfpVsConsts, lightDiffuse),
                  stats.argbufCbufFfpVsLightChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, ffpBlendWorldViewProj),
                  sizeof(FfpVsConsts::ffpBlendWorldViewProj),
                  stats.argbufCbufFfpVsBlendChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, ffpTextureTransforms),
                  sizeof(FfpVsConsts::ffpTextureTransforms),
                  stats.argbufCbufFfpVsTexTransformChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, clipPlanes),
                  sizeof(FfpVsConsts::clipPlanes),
                  stats.argbufCbufFfpVsClipChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, halfPixelFixup),
                  offsetof(FfpVsConsts, fogStart) -
                      offsetof(FfpVsConsts, halfPixelFixup),
                  stats.argbufCbufFfpVsViewportChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, fogStart),
                  sizeof(FfpVsConsts) - offsetof(FfpVsConsts, fogStart),
                  stats.argbufCbufFfpVsFogPointChangedBytes);
    updateHistory(ffpVsHistory, current, uploadBytes);
  }

  void recordArgbufCbufUploadContent(u32 argbufIndex,
                                     const void* data,
                                     u64 bytes,
                                     u64 /*hostStructBytes*/) {
    switch (argbufIndex) {
      case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
        recordVsUploadContent(data, bytes);
        break;
      case dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex:
        recordFfpVsUploadContent(data, bytes);
        break;
      default:
        break;
    }
  }

  void addSetVertexBytes(u64 bytes, u32 slot) {
    if (!enabled) {
      return;
    }
    ++stats.setVertexBytesCalls;
    stats.setVertexBytesBytes += bytes;
    if (slot == 5) {
      ++stats.setVertexBytesSlot5Calls;
      stats.setVertexBytesSlot5Bytes += bytes;
    } else {
      ++stats.setVertexBytesOtherCalls;
      stats.setVertexBytesOtherBytes += bytes;
    }
  }

  void addTransientVertexBytes(u64 bytes, TransientVertexSource source) {
    if (!enabled) {
      return;
    }
    stats.transientVertexBytes += bytes;
    switch (source) {
      case TransientVertexSource::User:
        stats.transientVertexUserBytes += bytes;
        break;
      case TransientVertexSource::Preupload:
        stats.transientVertexPreuploadBytes += bytes;
        break;
      case TransientVertexSource::DeclFallback:
        stats.transientVertexDeclFallbackBytes += bytes;
        break;
      case TransientVertexSource::ExpandedMain:
        stats.transientVertexExpandedMainBytes += bytes;
        break;
      case TransientVertexSource::ExpandedExtra:
        stats.transientVertexExpandedExtraBytes += bytes;
        break;
    }
  }

  void addTransientIndexBytes(u64 bytes, TransientIndexSource source) {
    if (!enabled) {
      return;
    }
    stats.transientIndexBytes += bytes;
    switch (source) {
      case TransientIndexSource::User:
        stats.transientIndexUserBytes += bytes;
        break;
      case TransientIndexSource::Preupload:
        stats.transientIndexPreuploadBytes += bytes;
        break;
      case TransientIndexSource::ShadowFallback:
        stats.transientIndexShadowFallbackBytes += bytes;
        break;
      case TransientIndexSource::ProbeReorder:
        stats.transientIndexProbeReorderBytes += bytes;
        break;
      case TransientIndexSource::OptimizedOrder:
        stats.transientIndexOptimizedOrderBytes += bytes;
        break;
    }
  }
};

struct ArgbufCbufCache {
  bool valid = false;
  u64 payloadHash = 0;
  dxmt9::argbuf_hybrid::ConstantBufferBindings bindings{};
  bool ffpVsValid = false;
  std::array<std::byte, sizeof(FfpVsConsts)> ffpVsBytes{};

  void reset() noexcept {
    valid = false;
    payloadHash = 0;
    bindings = {};
    ffpVsValid = false;
    ffpVsBytes = {};
  }

  bool matches(u64 hash) const noexcept {
    return valid && payloadHash == hash && bindings.complete();
  }

  bool hasMatchingFfpVs(const FfpVsConsts& host) const noexcept {
    const auto& binding =
        bindings.entries[dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex];
    return ffpVsValid && binding &&
           std::memcmp(ffpVsBytes.data(), &host, sizeof(FfpVsConsts)) == 0;
  }

  dxmt9::argbuf_hybrid::ConstantBufferBinding ffpVsBinding() const noexcept {
    return bindings.entries[dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex];
  }

  void merge(u64 hash,
             const dxmt9::argbuf_hybrid::ConstantBufferBindings& written) noexcept {
    for (std::size_t i = 0; i < bindings.entries.size(); ++i) {
      if (written.entries[i]) {
        bindings.entries[i] = written.entries[i];
      }
    }
    if (bindings.complete()) {
      payloadHash = hash;
      valid = true;
    } else {
      valid = false;
    }
  }

  void storeFfpVs(u64 hash,
                  const FfpVsConsts& host,
                  WMT::Buffer buffer,
                  u64 offset,
                  u64 bytes) noexcept {
    bindings.entries[dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex] =
        dxmt9::argbuf_hybrid::ConstantBufferBinding{
            .buffer = buffer,
            .offset = offset,
            .bytes = bytes,
        };
    std::memcpy(ffpVsBytes.data(), &host, sizeof(FfpVsConsts));
    ffpVsValid = true;
    if (bindings.complete()) {
      payloadHash = hash;
      valid = true;
    } else {
      valid = false;
    }
  }
};

void recordArgbufCbufUploadForBreakdown(void* userdata,
                                        u32 argbufIndex,
                                        const void* data,
                                        u64 bytes,
                                        u64 hostStructBytes) {
  if (!userdata) {
    return;
  }
  static_cast<ActiveEncoderBreakdown*>(userdata)->recordArgbufCbufUploadContent(
      argbufIndex, data, bytes, hostStructBytes);
}

bool blendFactorNeedsConstantColor(const core::FlatStateSet<core::kMaxStateSlots>& rs) {
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

u64 argbufTableShadowHash(obj_handle_t storage, u64 offset) noexcept {
  return drawBindingPacketHashMix(static_cast<u64>(storage), offset);
}

constexpr u64 kFragmentTextureShadowTag = 0x667261675f746578ull;
constexpr u64 kFragmentSamplerShadowTag = 0x667261675f73616dull;
constexpr u64 kVertexTextureShadowTag = 0x766572745f746578ull;
constexpr u64 kVertexSamplerShadowTag = 0x766572745f73616dull;

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

bool renderEncoderGpuTimeEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ENCODER_GPU_TIME");
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
  // D3D9 RS_DEPTH_BIAS / RS_SLOPE_SCALE_DEPTH_BIAS are stored as DWORDs but
  // semantically float; bit_cast restores the IEEE 754 layout that
  // MTLRenderCommandEncoder.setDepthBias:slopeScale:clamp: expects. clamp is
  // not exposed by D3D9 RS and is left at 0.0f (Metal's "unbounded" sentinel).
  const float depthBias = std::bit_cast<float>(
      core::flatStateOr(renderStates, core::RS_DEPTH_BIAS, 0u));
  const float slopeScale = std::bit_cast<float>(
      core::flatStateOr(renderStates, core::RS_SLOPE_SCALE_DEPTH_BIAS, 0u));
  recordedSetRasterizerState(ctx, encoder, triangleFillModeFromRenderState(renderStates), cullMode,
                             WMTDepthClipModeClip, frontFaceWinding(),
                             depthBias, slopeScale, 0.0f);
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

struct RenderPassFrameTracker {
  std::array<RenderPassFrameKey, 64> seen{};
  std::size_t seenCount = 0;
  std::optional<RenderPassFrameKey> last{};

  void reset() noexcept {
    seenCount = 0;
    last.reset();
  }

  bool hasSeen(RenderPassFrameKey key) const noexcept {
    for (std::size_t i = 0; i < seenCount; ++i) {
      if (seen[i] == key) {
        return true;
      }
    }
    return false;
  }

  void remember(RenderPassFrameKey key) noexcept {
    if (seenCount < seen.size()) {
      seen[seenCount++] = key;
    }
  }

  void noteStart(RenderPassFrameKey key, RenderPassAttachmentFootprint footprint) {
    if (!key.valid()) {
      return;
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
    const bool seenBefore = hasSeen(key);
    if (seenBefore) {
      if (last.has_value() && *last == key) {
        perf::countRenderPassSameKeyAdjacent();
      } else {
        perf::countRenderPassSameKeyReentry();
        // A same-key re-entry generally implies the previous pass stored
        // the attachment contents and this pass loads them again. Count
        // that store+load footprint as the preservation budget to attack.
        perf::countRenderPassSameKeyReentryPreservationBytes(
            footprint.totalBytes() * 2u);
        perf::countRenderPassSameKeyReentryColorPreservationBytes(
            footprint.color0Bytes * 2u);
        perf::countRenderPassSameKeyReentryDepthPreservationBytes(
            footprint.depthBytes * 2u);
      }
    } else {
      remember(key);
    }
    last = key;
  }
};

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

bool x8ShaderAlphaFillEnabledForDiagnostics() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_X8_SHADER_ALPHA_FILL");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

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
                             const resources::Pool* pool = nullptr) {
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
  return hash;
}

u64 shaderSourceAttributionKeyForDraw(core::FlatDrawStateView drawState) {
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
  return hash;
}

u32 vsOutLayoutKeyForDraw(core::FlatDrawStateView drawState,
                          bool tileFfpBaseColor) {
  if (!drawState.hot || !drawState.hasShaderContext()) {
    return 0;
  }
  auto context =
      drawshader::makeShaderSourceContext(drawState.shaderContext(), *drawState.hot);
  context.stripFogAlphaTestForTileBase = tileFfpBaseColor;
  context.stripAlphaTestForDebug = debug::disableAlphaTest();
  context.stripFogForDebug = debug::disableFog();
  context.forceTextureWhiteForDebug = debug::forceTextureWhite();
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
                                             bool samplerLodBias,
                                             u32 x8AlphaOneTextureMask) {
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
  context.forceTextureWhiteForDebug = debug::forceTextureWhite();
  context.argbufHybridMode = argbufHybridMode;
  context.argbufResourceArray = argbufHybridMode && argbufResourceArray;
  context.samplerLodBias = samplerLodBias;
  context.x8AlphaOneTextureMask = x8AlphaOneTextureMask;
  try {
    context.vsOutLayout = drawshader::resolveVSOutLayoutForShaderPair(context);
    const auto vertex = drawshader::makeDrawShaderSource(context, true);
    const auto pixel = drawshader::makeDrawShaderSource(context, false);
    hashes.vertex = core::hashString(vertex);
    hashes.pixel = core::hashString(pixel);
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
                                 bool argbufResourceArray) {
  if (!encoderBreakdown || !encoderBreakdown->enabled) {
    return;
  }
  const auto variantHash = shaderVariantHashForDraw(drawState, &pool);
  const auto sourceKey = shaderSourceAttributionKeyForDraw(drawState);
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
    vsOutLayoutKey = vsOutLayoutKeyForDraw(drawState, tileFfpMode);
    encoderBreakdown->storeCachedVsOutLayout(sourceKey, tileFfpMode,
                                             vsOutLayoutKey);
  }
  const bool samplerLodBias = anySamplerLodBiasNonzero(drawState);
  const u32 x8AlphaOneTextureMask = x8AlphaOneTextureMaskForDraw(drawState, pool);
  u64 vertexShaderSourceHash = 0;
  u64 pixelShaderSourceHash = 0;
  if (!encoderBreakdown->findCachedShaderSourceHashes(
          sourceKey, tileFfpMode, argbufHybridMode,
          argbufHybridMode && argbufResourceArray, samplerLodBias,
          x8AlphaOneTextureMask,
          vertexShaderSourceHash, pixelShaderSourceHash)) {
    const auto sourceHashes = shaderSourceHashesForDraw(
        drawState, tileFfpMode, argbufHybridMode, argbufResourceArray,
        samplerLodBias, x8AlphaOneTextureMask);
    vertexShaderSourceHash = sourceHashes.vertex;
    pixelShaderSourceHash = sourceHashes.pixel;
    encoderBreakdown->storeCachedShaderSourceHashes(
        sourceKey, tileFfpMode, argbufHybridMode,
        argbufHybridMode && argbufResourceArray, samplerLodBias,
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

u64 stream0ByteSpanForIndexMeasure(const IndexReuseMeasure& measure,
                                   u64 stream0Stride) {
  if (!measure.available || stream0Stride == 0u) {
    return 0u;
  }
  return static_cast<u64>(measure.maxIndex - measure.minIndex) * stream0Stride;
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

void maybeDumpIndexedGeometryPayload(
    const ActiveEncoderBreakdown* encoderBreakdown,
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
       << "vsconsts_byte_count=" << vsConstsBytes.size() << "\n"
       << "psconsts_byte_count=" << psConstsBytes.size() << "\n"
       << "ffpvs_byte_count=" << ffpVsConstsBytes.size() << "\n"
       << "ffpps_byte_count=" << ffpPsConstsBytes.size() << "\n"
       << "wrote_vsconsts=" << (wroteVsConsts ? 1 : 0) << "\n"
       << "wrote_psconsts=" << (wrotePsConsts ? 1 : 0) << "\n"
       << "wrote_ffpvs=" << (wroteFfpVsConsts ? 1 : 0) << "\n"
       << "wrote_ffpps=" << (wroteFfpPsConsts ? 1 : 0) << "\n";
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

bool buildVertexCacheOptimizedTriangleOrderIndexBytes(
    std::span<const u8> indexBytes,
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

  struct Triangle {
    std::array<u32, 3> indices{};
    u32 minIndex = 0;
    u32 maxIndex = 0;
    std::size_t original = 0;
  };

  const std::size_t triangleCount = static_cast<std::size_t>(indexCount / 3u);
  std::vector<Triangle> triangles;
  triangles.reserve(triangleCount);
  std::unordered_map<u32, std::vector<u32>> vertexTriangles;
  std::unordered_map<u32, u32> remainingVertexUse;
  vertexTriangles.reserve(triangleCount * 2u);
  remainingVertexUse.reserve(triangleCount * 2u);

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
      vertexTriangles[*value].push_back(static_cast<u32>(triangle));
      ++remainingVertexUse[*value];
    }
    triangles.push_back(tri);
  }

  constexpr std::size_t kProbeCacheSize = 64u;
  std::vector<u32> cache;
  cache.reserve(kProbeCacheSize);
  std::vector<u32> candidates;
  candidates.reserve(std::min<std::size_t>(triangleCount, 256u));
  std::vector<u8> emitted(triangleCount, 0u);
  std::vector<u8> inCandidates(triangleCount, 0u);
  std::vector<u32> order;
  order.reserve(triangleCount);
  std::size_t nextOriginal = 0;

  auto cachePosition = [&](u32 index) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < cache.size(); ++i) {
      if (cache[i] == index) {
        return i;
      }
    }
    return std::nullopt;
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
    auto it = vertexTriangles.find(index);
    if (it == vertexTriangles.end()) {
      return;
    }
    for (const u32 triangle : it->second) {
      addCandidate(triangle);
    }
  };

  auto chooseBestCandidate = [&]() -> std::optional<u32> {
    std::optional<std::size_t> bestSlot;
    std::int64_t bestScore = std::numeric_limits<std::int64_t>::min();
    for (std::size_t slot = 0; slot < candidates.size(); ++slot) {
      const u32 candidate = candidates[slot];
      const std::size_t triangleIndex = static_cast<std::size_t>(candidate);
      if (triangleIndex >= triangleCount || emitted[triangleIndex]) {
        continue;
      }
      const auto& tri = triangles[triangleIndex];
      u32 cachedVertices = 0;
      u32 cacheDistance = 0;
      u32 remainingUse = 0;
      for (const u32 index : tri.indices) {
        if (const auto pos = cachePosition(index)) {
          ++cachedVertices;
          cacheDistance += static_cast<u32>(*pos);
        }
        remainingUse += remainingVertexUse[index];
      }
      const std::int64_t score =
          static_cast<std::int64_t>(cachedVertices) * 1'000'000ll -
          static_cast<std::int64_t>(cacheDistance) * 1'000ll -
          static_cast<std::int64_t>(remainingUse) * 10ll -
          static_cast<std::int64_t>(tri.minIndex) / 256ll;
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
    for (std::size_t i = 0; i < cache.size(); ++i) {
      if (cache[i] == index) {
        for (std::size_t j = i; j > 0u; --j) {
          cache[j] = cache[j - 1u];
        }
        cache[0] = index;
        return;
      }
    }
    if (cache.size() < kProbeCacheSize) {
      cache.push_back(index);
    } else {
      for (std::size_t i = cache.size() - 1u; i > 0u; --i) {
        cache[i] = cache[i - 1u];
      }
    }
    cache[0] = index;
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
    inCandidates[triangleIndex] = 0u;
    order.push_back(chosen);
    const auto& tri = triangles[triangleIndex];
    for (const u32 index : tri.indices) {
      if (remainingVertexUse[index] != 0u) {
        --remainingVertexUse[index];
      }
      touchCacheVertex(index);
    }
    for (const u32 index : tri.indices) {
      addVertexNeighbors(index);
    }
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
  out.resize(byteCount);
  for (std::size_t triangle = 0; triangle < order.size(); ++triangle) {
    const auto& tri = triangles[order[triangle]];
    for (std::size_t i = 0; i < 3u; ++i) {
      writeIndexValue(out, indexType, triangle * 3u + i, tri.indices[i]);
    }
  }
  return true;
}

bool isOpaqueDepthWritingReorderProbeEligible(
    const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
    WMTTriangleFillMode fillMode) {
  if (fillMode != WMTTriangleFillModeFill) {
    return false;
  }
  if (core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u) {
    return false;
  }
  if (core::flatStateOr(renderStates, RS_ALPHA_TEST_ENABLE, 0u) != 0u) {
    return false;
  }
  if (core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u) {
    return false;
  }
  if (core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u) {
    return false;
  }
  const bool depthEnabled =
      core::flatStateOr(renderStates, RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && !debug::probeDisableDepthWrite() &&
      core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
  if (!depthWrite) {
    return false;
  }
  switch (static_cast<core::CompareFunc>(core::flatStateOr(
      renderStates, RS_Z_FUNC, static_cast<u32>(core::CompareFunc::LessEqual)))) {
    case core::CompareFunc::Less:
    case core::CompareFunc::LessEqual:
      return true;
    default:
      return false;
  }
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

bool indexedTriangleEncoderDrawRangeMatches(
    const ActiveEncoderBreakdown* encoderBreakdown) {
  const auto range = debug::probeIndexedTriangleEncoderDrawRange();
  if (!debug::drawOrdinalRangeEnabled(range)) {
    return true;
  }
  if (!encoderBreakdown) {
    return false;
  }
  return !debug::shouldSkipDrawOrdinal(encoderBreakdown->stats.drawCalls, range);
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
    const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
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
    const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
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
    const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
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
    const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
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
                                  const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
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
                                    const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
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

bool disableAlphaBlendProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
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
    const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
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
    const core::FlatStateSet<core::kMaxStateSlots>& renderStates,
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
  info.lod_min_clamp = std::max(lodMinClamp, static_cast<float>(maxMipLevel));
  info.lod_max_clamp = 1e9f;
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
  info.lod_min_clamp = std::max(lodMinClamp, static_cast<float>(maxMipLevel));
  info.lod_max_clamp = 1e9f;
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
perf::RenderPassDepthStoreProof depthStoreProofForLookahead(
    const core::ChunkSlot& slot,
    std::size_t startCommandIndex,
    core::Handle depthHandle) {
  using Proof = perf::RenderPassDepthStoreProof;
  if (!depthHandle) {
    return Proof::BlockNullDepth;
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
          return Proof::AllowNextClear;
        }
        break;
      case Kind::DrawRun:
        if (next.drawState.hot) {
          if (next.drawState.hot->depthStencil.handle == depthHandle) {
            // Depth is read by a subsequent draw — must Store.
            return Proof::BlockDrawDepth;
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
              return Proof::BlockShadowSample;
            }
          }
        }
        break;
      case Kind::SurfaceCopy:
        if (next.surfaceCopy &&
            (next.surfaceCopy->source == depthHandle ||
             next.surfaceCopy->destination == depthHandle)) {
          return Proof::BlockSurfaceCopy;
        }
        break;
      case Kind::StretchRect:
        if (next.stretchRect &&
            (next.stretchRect->source == depthHandle ||
             next.stretchRect->destination == depthHandle)) {
          return Proof::BlockStretchRect;
        }
        break;
      case Kind::Readback:
        // R-BACK-15.15: host-visible read of the depth surface must not
        // be served from a DontCare-stored tile.
        if (next.readback &&
            (next.readback->source == depthHandle ||
             next.readback->destination == depthHandle)) {
          return Proof::BlockReadback;
        }
        break;
      case Kind::ColorFill:
        if (next.colorFill && next.colorFill->destination == depthHandle) {
          return Proof::BlockColorFill;
        }
        break;
      case Kind::DepthResolve:
        // R-FORMAT-11: a later RESZ resolve reads the MSAA depth surface as
        // its source (and writes the INTZ destination). If either endpoint is
        // this depth handle its tile contents must survive — force a Store
        // exactly like the StretchRect/Readback depth-touch cases above.
        if (next.depthResolve &&
            (next.depthResolve->msaaDepth == depthHandle ||
             next.depthResolve->intzDest == depthHandle)) {
          return Proof::BlockDepthResolve;
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
    return Proof::AllowDeadNoPresent;
  }
  return sawPresent ? Proof::BlockPresent : Proof::AllowDeadNoPresent;
}

bool nextDepthOperationIsClear(const core::ChunkSlot& slot,
                               std::size_t startCommandIndex,
                               core::Handle depthHandle) {
  const auto proof =
      depthStoreProofForLookahead(slot, startCommandIndex, depthHandle);
  return proof == perf::RenderPassDepthStoreProof::AllowNextClear ||
         proof == perf::RenderPassDepthStoreProof::AllowDeadNoPresent;
}

perf::RenderPassColorStoreProof colorStoreProofForLookahead(
    const core::ChunkSlot& slot,
    std::size_t startCommandIndex,
    core::Handle colorHandle) {
  using Proof = perf::RenderPassColorStoreProof;
  if (!colorHandle) {
    return Proof::BlockNullColor;
  }
  using Kind = core::MetalCommandKind;
  bool sawPresent = false;
  for (std::size_t i = startCommandIndex + 1; i < slot.commandCount(); ++i) {
    const auto next = slot.commandAt(i);
    switch (next.kind) {
      case Kind::Clear:
        if (next.clear && next.clear->clearColor) {
          for (const auto& attachment : next.clear->colorAttachments) {
            if (attachment.handle == colorHandle) {
              return Proof::AllowNextClear;
            }
          }
        }
        break;
      case Kind::DrawRun:
        if (next.drawState.hot) {
          const auto& hot = *next.drawState.hot;
          for (const auto& attachment : hot.colorAttachments) {
            if (attachment.handle == colorHandle) {
              return Proof::BlockDrawTarget;
            }
          }
          const auto& textures = hot.textures;
          const std::uint32_t mask = hot.textureMask;
          for (std::size_t s = 0; s < textures.size(); ++s) {
            if ((mask & (1u << s)) == 0) continue;
            if (textures[s] == colorHandle) {
              return Proof::BlockTextureSample;
            }
          }
        }
        break;
      case Kind::SurfaceCopy:
        if (next.surfaceCopy &&
            (next.surfaceCopy->source == colorHandle ||
             next.surfaceCopy->destination == colorHandle)) {
          return Proof::BlockSurfaceCopy;
        }
        break;
      case Kind::StretchRect:
        if (next.stretchRect &&
            (next.stretchRect->source == colorHandle ||
             next.stretchRect->destination == colorHandle)) {
          return Proof::BlockStretchRect;
        }
        break;
      case Kind::Readback:
        if (next.readback &&
            (next.readback->source == colorHandle ||
             next.readback->destination == colorHandle)) {
          return Proof::BlockReadback;
        }
        break;
      case Kind::ColorFill:
        if (next.colorFill && next.colorFill->destination == colorHandle) {
          return Proof::BlockColorFill;
        }
        break;
      case Kind::Present:
        if (next.present && next.present->presentSource == colorHandle) {
          return Proof::BlockPresent;
        }
        sawPresent = true;
        break;
      case Kind::DepthResolve:
        break;
    }
  }
  static const bool aggressive = []() {
    if (const char* v = std::getenv("DXMT9_AGGRESSIVE_COLOR_DONTCARE")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  if (sawPresent) {
    return Proof::BlockPresent;
  }
  return aggressive ? Proof::AllowDeadNoPresent
                    : Proof::BlockDeadNoPresentDisabled;
}

bool nextColorOperationIsClear(const core::ChunkSlot& slot,
                               std::size_t startCommandIndex,
                               core::Handle colorHandle) {
  return colorStoreProofForLookahead(slot, startCommandIndex, colorHandle) ==
         perf::RenderPassColorStoreProof::AllowNextClear;
}

WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    core::FlatDrawStateView drawState,
    const std::optional<ClearDesc>& clear,
    const core::ChunkSlot* lookaheadSlot,
    std::size_t lookaheadStartIndex,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments = {}) {
  const auto& hot = *drawState.hot;
  auto* primarySurface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
  // R-FORMAT-12: a D3DFMT_NULL render target is colorless and has no Metal
  // color texture by design. When RT0 is a NULL render target the render
  // pass is depth/stencil-only — proceed (the per-attachment loop below
  // omits every color attachment that has no texture, so the NULL RT
  // contributes no color attachment and the bound depth/stencil becomes
  // the effective target). Only abort when RT0 is genuinely missing, or it
  // is a normal color RT that failed to allocate its texture.
  const ColorlessRenderPassRt0 rt0{
      .surfaceExists = primarySurface != nullptr,
      .hasTexture = primarySurface && static_cast<bool>(primarySurface->texture),
      .isNullRt =
          primarySurface && primarySurface->desc.format == core::Format::NullRt,
  };
  if (!renderPassAdmitsRt0(rt0)) {
    return {};
  }
  WMTRenderPassInfo passInfo{};
  const bool discardAfterPresent = !clear.has_value() && ctx.queue.backBufferDiscardAfterPresent_ &&
                                   hot.colorAttachments[0].handle == ctx.queue.currentBackBuffer_;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    auto* surface = ctx.pool.findSurface(hot.colorAttachments[i].handle.value);
    // R-FORMAT-12: omit any color slot whose surface owns no backend texture
    // (a NULL render target is the colorless case). See colorAttachmentIncluded.
    if (!colorAttachmentIncluded(surface != nullptr,
                                 surface && static_cast<bool>(surface->texture))) {
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
    auto colorStoreProof = lookaheadSlot != nullptr
        ? colorStoreProofForLookahead(*lookaheadSlot, lookaheadStartIndex,
                                      hot.colorAttachments[i].handle)
        : perf::RenderPassColorStoreProof::BlockNoLookahead;
    if (surface->resolveTexture &&
        (colorStoreProof == perf::RenderPassColorStoreProof::AllowNextClear ||
         colorStoreProof == perf::RenderPassColorStoreProof::AllowDeadNoPresent)) {
      colorStoreProof = perf::RenderPassColorStoreProof::BlockMsaaResolve;
    }
    perf::countRenderPassColorStoreProof(colorStoreProof);
    const bool colorDontCareStore =
        colorStoreProof == perf::RenderPassColorStoreProof::AllowNextClear ||
        colorStoreProof == perf::RenderPassColorStoreProof::AllowDeadNoPresent;
    attachment.store_action =
        colorDontCareStore ? WMTStoreActionDontCare : WMTStoreActionStore;
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
    auto depthStoreProof = lookaheadSlot != nullptr
        ? depthStoreProofForLookahead(*lookaheadSlot, lookaheadStartIndex,
                                      hot.depthStencil.handle)
        : perf::RenderPassDepthStoreProof::BlockNoLookahead;
    if (hasResolveTarget &&
        (depthStoreProof == perf::RenderPassDepthStoreProof::AllowNextClear ||
         depthStoreProof == perf::RenderPassDepthStoreProof::AllowDeadNoPresent)) {
      depthStoreProof = perf::RenderPassDepthStoreProof::BlockMsaaResolve;
    }
    perf::countRenderPassDepthStoreProof(depthStoreProof);
    const bool depthDontCareStore =
        !hasResolveTarget &&
        (depthStoreProof == perf::RenderPassDepthStoreProof::AllowNextClear ||
         depthStoreProof == perf::RenderPassDepthStoreProof::AllowDeadNoPresent);
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

  constexpr std::size_t kMaxSampleBufferAttachments =
      sizeof(passInfo.sample_buffer_attachments) /
      sizeof(passInfo.sample_buffer_attachments[0]);
  const auto attachmentCount = std::min<std::size_t>(
      sampleBufferAttachments.size(), kMaxSampleBufferAttachments);
  for (std::size_t i = 0; i < attachmentCount; ++i) {
    passInfo.sample_buffer_attachments[i] = sampleBufferAttachments[i];
  }
  passInfo.num_sample_buffer_attachments =
      static_cast<std::uint8_t>(attachmentCount);

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
  // D3D9 RS_DEPTH_BIAS / RS_SLOPE_SCALE_DEPTH_BIAS are stored as DWORDs but
  // semantically float (see setRasterizerCullMode for the per-draw equivalent).
  // Prologue value is the initial baseline; per-draw rebinds will override.
  const float prologueDepthBias = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_DEPTH_BIAS, 0u));
  const float prologueSlopeScale = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_SLOPE_SCALE_DEPTH_BIAS, 0u));
  encoder.setRasterizerState(WMTTriangleFillModeFill, WMTCullModeNone,
                              WMTDepthClipModeClip, frontFaceWinding(),
                              prologueDepthBias, prologueSlopeScale, 0.0f);
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

bool drawParamBindingOverride(const core::DrawParam& param,
                              std::span<const u8> arena,
                              core::DrawBindingOverride& out) {
  const auto bytes = core::drawRunPayloadBytes(param.bindingOverrideRange, arena);
  if (bytes.size() != sizeof(core::DrawBindingOverride)) {
    return false;
  }
  std::memcpy(&out, bytes.data(), sizeof(out));
  return out.streamMask != 0 || out.indexBufferValid;
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
                 uniform::DirtyState* dirty,
                 bool tileFfpMode,
                 bool argbufHybridMode,
                 bool argbufResourceArray,
                 bool reopenArgbufHybrid,
                 core::PsoHandle renderPsoHandle,
                 core::PsoHandle tilePsoHandle,
                 core::DepthStencilHandle depthStencilHandle,
                 TextureSamplerBindShadow* textureSamplerShadow,
                 std::uint32_t commandIndex,
                 ActiveEncoderBreakdown* encoderBreakdown,
                 ArgbufCbufCache* argbufCbufCache) {
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
  const bool traceEncode = debug::shouldTraceEncode(hot, seqId) ||
                           colorAttachmentAliasesTracedTexture(ctx.pool, hot);
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
  const bool effectiveSkipBaseStateBind =
      skipBaseStateBind && !depthStateProbeRequested;
  if (effectiveSkipBaseStateBind) {
    recordPsoAttributionForDraw(encoderBreakdown, drawState, ctx.pool, renderPsoHandle,
                                tileFfpMode, argbufHybridMode,
                                argbufResourceArray);
  }
  std::optional<dxmt9::ffp::FixedFunctionVertexLayout> ffLayout;
  bool fixedFunctionPath = false;
  {
    PerfScope fvfDecodeScope(perf::countEncodeDrawFvfDecodeCpuTime);
    ffLayout = decodeFixedFunctionVertexLayout(vertexDecl);
    fixedFunctionPath = drawUsesFixedFunctionPath(drawState, static_cast<bool>(ffLayout));
  }
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
      disableAlphaBlendProbeClassMatches(primitiveCount,
                                         hot.textureMask,
                                         hot.renderStates,
                                         hot.viewport,
                                         fillMode);
  const bool depthFuncAlwaysProbeApplied =
      debug::probeDepthFuncAlways() &&
      indexedTriangleDraw &&
      depthFuncAlwaysProbeRowMatches(encoderBreakdown) &&
      depthFuncAlwaysProbeClassMatches(primitiveCount,
                                       hot.textureMask,
                                       hot.renderStates,
                                       hot.viewport,
                                       fillMode);
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
    PerfScope pipelineLookupScope(perf::countEncodeDrawPipelineLookupCpuTime);
    auto depthKey = makeDepthStencilKey(drawState);
    if (disableDepthWriteProbeApplied) {
      depthKey.depthWrite = false;
    }
    if (depthFuncAlwaysProbeApplied) {
      depthKey.depthFunc = static_cast<u32>(core::CompareFunc::Always);
    }
    const std::uint8_t stencilRef = state::computeStencilRef(drawState);
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
      pipeline = ctx.drawRecorder->renderPipelineState;
    } else if (tileFfpMode) {
      // R-BACK-13.1 two-stage tile-FFP encode: the render command encoder
      // first rasterizes the geometry with the BASE-COLOUR fragment PSO
      // (fog / alpha-test / A2C stripped), and only afterwards runs the tile
      // kernel over the imageblock. So in tile mode the PSO bound here via
      // setRenderPipelineState is the base-colour render PSO, NOT the tile
      // PSO. The tile PSO is fetched + dispatched after drawPrimitives below.
      pipelineRef =
          renderPsoHandle.valid() && !disableAlphaBlendProbeApplied
              ? ctx.cache.drawPipelineForHandle(renderPsoHandle,
                                                renderPsoLookup).get()
              : ctx.cache.getOrBuildTileFfpBaseColorPipelineForState(
                    ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                    ctx.shaderArchive, ctx.shaderArchivePath).get();
      pipeline = WMT::RenderPipelineState{pipelineRef.handle};
    } else {
      pipelineRef =
          renderPsoHandle.valid() && !disableAlphaBlendProbeApplied
              ? ctx.cache.drawPipelineForHandle(renderPsoHandle,
                                                renderPsoLookup).get()
              : ctx.cache.getOrBuildDrawPipelineForState(
                    ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                    ctx.shaderArchive, ctx.shaderArchivePath, tileFfpMode,
                    argbufHybridMode, argbufResourceArray,
                    disableAlphaBlendProbeApplied).get();
      pipeline = WMT::RenderPipelineState{pipelineRef.handle};
    }
    if (!pipeline) {
      perf::countDrawSkippedNoPipeline();
      static std::mutex logMutex;
      static std::unordered_set<std::uint64_t> loggedNoPipelineKeys;
      const std::uint64_t noPipelineKey =
          hot.key.vertexShaderHash ^ (hot.key.pixelShaderHash << 1u) ^
          (static_cast<std::uint64_t>(tileFfpMode) << 2u) ^
          (static_cast<std::uint64_t>(argbufHybridMode) << 3u) ^
          (static_cast<std::uint64_t>(argbufResourceArray) << 4u);
      bool shouldLog = false;
      {
        std::lock_guard lock(logMutex);
        shouldLog = loggedNoPipelineKeys.size() < 64u &&
                    loggedNoPipelineKeys.insert(noPipelineKey).second;
      }
      if (shouldLog) {
        util::logf(util::LogLevel::Error, "dxmt9-encode",
                   "draw skipped: no render pipeline seq=%llu command=%u vs=0x%llx ps=0x%llx tile=%u argbuf=%u resource_array=%u rt0=0x%llx ds=0x%llx",
                   static_cast<unsigned long long>(seqId),
                   commandIndex,
                   static_cast<unsigned long long>(hot.key.vertexShaderHash),
                   static_cast<unsigned long long>(hot.key.pixelShaderHash),
                   tileFfpMode ? 1u : 0u,
                   argbufHybridMode ? 1u : 0u,
                   argbufResourceArray ? 1u : 0u,
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
        recordedSetDepthStencilState(ctx, encoder, depthState, stencilRef);
        if (textureSamplerShadow) {
          textureSamplerShadowStore(textureSamplerShadow->depthStencil,
                                    stencilRef, depthState.handle);
        }
        countDepthStateBind();
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
      const auto variantHash = shaderVariantHashForDraw(drawState, &ctx.pool);
      const bool recordPsoBreakdown = encoderBreakdown && encoderBreakdown->enabled;
      u32 vsOutLayoutKey = 0;
      if (recordPsoBreakdown) {
        recordPsoAttributionForDraw(encoderBreakdown, drawState, ctx.pool, renderPsoHandle,
                                    tileFfpMode, argbufHybridMode,
                                    argbufResourceArray);
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
      recordedSetRenderPipelineState(ctx, encoder, pipeline);
      if (textureSamplerShadow) {
        bindShadowStore(textureSamplerShadow->renderPipeline, pipeline.handle);
      }
      countPipelineBind();
    }
  }
  auto uploadTransientBuffer = [&](const void* data, std::size_t len, std::size_t alignment) {
    return ctx.queue.uploadTransientBuffer(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len),
        alignment, seqId);
  };
  auto setVertexBufferCached = [&](WMT::Buffer buffer, u64 offset, std::uint8_t index) {
    if (textureSamplerShadow && index < textureSamplerShadow->vertexBuffers.size() &&
        bufferBindShadowMatches(textureSamplerShadow->vertexBuffers[index],
                                buffer.handle, offset)) {
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
  // NSException for). This matches design.md §13.5. Because that overwrites
  // the render PSO, we rebind the base-colour PSO after the dispatch so any
  // subsequent draw in a DrawRun (which skips the base-state bind) still has
  // its base-colour pipeline current.
  //
  // We fetch both PSOs on every tile-mode draw — the cache lookup is a hit
  // after the first build — so each draw in a DrawRun gets its own post-pass.
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
        tilePsoHandle.valid() && !disableAlphaBlendProbeApplied
            ? ctx.cache.drawPipelineForHandle(tilePsoHandle,
                                              tileLookup).get()
            : ctx.cache.getOrBuildDrawPipelineForState(
                  ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                  ctx.shaderArchive, ctx.shaderArchivePath,
                  /*tileFfpMode=*/true, /*argbufHybridMode=*/false,
                  /*argbufResourceArray=*/false,
                  disableAlphaBlendProbeApplied).get();
    tileFfpPso = WMT::RenderPipelineState{tileFfpPsoRef.handle};
    tileFfpBasePsoRef =
        renderPsoHandle.valid() && !disableAlphaBlendProbeApplied
            ? ctx.cache.drawPipelineForHandle(renderPsoHandle,
                                              tileBaseLookup).get()
            : ctx.cache.getOrBuildTileFfpBaseColorPipelineForState(
                  ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                  ctx.shaderArchive, ctx.shaderArchivePath).get();
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
      argbufResourceArray && argbufHybridMode &&
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
  if (argbufHybridMode && reopenArgbufHybrid) {
    const auto populated = dxmt9::argbuf_hybrid::openArgbuf(
        ctx.queue, argbufEncoderForDraw, seqId);
    if (populated && !suppressRecordedMetalCalls(ctx)) {
      const u64 tableHash =
          argbufTableShadowHash(populated.storage.handle, populated.offset);
      const bool tableUnchanged =
          textureSamplerShadow && textureSamplerShadow->argbufTableValid &&
          textureSamplerShadow->argbufTableHash == tableHash;
      if (!tableUnchanged) {
        encoder.setVertexBuffer(populated.storage, populated.offset,
                                dxmt9::shaders::kArgbufHybridBindSlot);
        encoder.setFragmentBuffer(populated.storage, populated.offset,
                                  dxmt9::shaders::kArgbufHybridBindSlot);
        if (textureSamplerShadow) {
          textureSamplerShadow->argbufTableValid = true;
          textureSamplerShadow->argbufTableHash = tableHash;
          if (dxmt9::shaders::kArgbufHybridBindSlot <
              textureSamplerShadow->vertexBuffers.size()) {
            bufferBindShadowStore(
                textureSamplerShadow->vertexBuffers[dxmt9::shaders::kArgbufHybridBindSlot],
                populated.storage.handle, populated.offset);
          }
        }
      }
      perf::countArgbufHybridBytes(populated.length);
      if (encoderBreakdown) {
        encoderBreakdown->addArgbufTableBytes(populated.length);
      }
      const bool canRepointCachedCbufs =
          argbufCbufCache &&
          argbufCbufCache->matches(argbufPayloadHash) &&
          !uniform::anyDirty(*dirtyPtr, argbufConsumedBits);
      if (canRepointCachedCbufs) {
        for (u32 i = 0; i < argbufCbufCache->bindings.entries.size(); ++i) {
          dxmt9::argbuf_hybrid::pointConstantBufferBinding(
              argbufEncoderForDraw, i, argbufCbufCache->bindings.entries[i],
              nullptr, encoder);
        }
      } else {
        uniform::markAllDirty(*dirtyPtr);
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
  if (argbufHybridMode) {
    dxmt9::argbuf_hybrid::ConstantBufferBindings writtenCbufBindings{};
    dxmt9::argbuf_hybrid::ConstantBufferUploadObserver cbufUploadObserver{};
    if (encoderBreakdown && encoderBreakdown->enabled) {
      cbufUploadObserver.userdata = encoderBreakdown;
      cbufUploadObserver.upload = recordArgbufCbufUploadForBreakdown;
    }
    auto dirtyForArgbuf = *dirtyPtr;
    uniform::clearBits(dirtyForArgbuf, uniform::kFfpVsAny);
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
      perf::countArgbufHybridBytes(bytes);
      if (encoderBreakdown) {
        encoderBreakdown->addArgbufCbufBindings(writtenCbufBindings);
      }
      if (argbufCbufCache) {
        argbufCbufCache->merge(argbufPayloadHash, writtenCbufBindings);
      }
    }
    uniform::clearBits(
        *dirtyPtr,
        static_cast<std::uint16_t>(argbufConsumedBits & ~uniform::kFfpVsAny));
  }
  {
    PerfScope uniformBuildScope(perf::countEncodeDrawUniformBuildCpuTime);
    if (!argbufHybridMode && uniform::anyDirty(*dirtyPtr, uniform::kVsAny)) {
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
    if (!argbufHybridMode && uniform::anyDirty(*dirtyPtr, uniform::kPsAny)) {
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
    if (!argbufHybridMode && uniform::anyDirty(*dirtyPtr, uniform::kFfpPsAny)) {
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
    if (argbufHybridMode && argbufCbufCache &&
        argbufCbufCache->hasMatchingFfpVs(*host)) {
      dxmt9::argbuf_hybrid::pointConstantBufferBinding(
          argbufEncoderForDraw,
          dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex,
          argbufCbufCache->ffpVsBinding(), nullptr, encoder);
      uniform::clearBits(*dirtyPtr, uniform::kFfpVsAny);
      ffpVsBound = true;
      return;
    }
    auto slice = uploadTransientBuffer(host, sizeof(FfpVsConsts), alignof(FfpVsConsts));
    if (slice) {
      if (argbufHybridMode) {
        dxmt9::argbuf_hybrid::pointFfpVsAtSlice(
            argbufEncoderForDraw, slice.buffer, slice.offset,
            nullptr, encoder);
        perf::countArgbufHybridBytes(sizeof(FfpVsConsts));
        if (encoderBreakdown) {
          encoderBreakdown->addArgbufCbufBytes(
              dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex,
              sizeof(FfpVsConsts));
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
  const auto bindingPacketPlan = makeDrawBindingPacketPlan(
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
  const auto& bindingPacket =
      cacheDrawBindingPacket(gDrawBindingPacketCache, bindingPacketPlan);
  if (encoderBreakdown) {
    for (const auto& binding : bindingPacket.fragmentTextureSamplers) {
      const auto* texture = ctx.pool.findTexture(binding.texture.value);
      encoderBreakdown->recordFragmentTextureBinding(binding.stage,
                                                     binding.texture,
                                                     texture);
    }
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
  if (!effectiveSkipBaseStateBind) {
    PerfScope streamBindViewportScope(perf::countEncodeDrawStreamBindCpuTime);
    if (bindingPacketHasRasterTarget) {
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
  {
    PerfScope fvfDecodeBytesScope(perf::countEncodeDrawFvfDecodeCpuTime);
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
          buffer && buffer->buffer) {
        stream0Record = buffer;
        vertexBuffer = WMT::Buffer{buffer->buffer.handle};
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
      if (encoderBreakdown) {
        if (stream0Record) {
          encoderBreakdown->recordStreamResource(0, hot.streamBuffers[0].value,
                                                 stream0Record->desc);
        }
        encoderBreakdown->recordStreamState(
            0, hot.streamBuffers[0].value, hot.streamOffsets[0],
            drawVertexStreamStride);
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
      if (encoderBreakdown) {
        if (stream0Record) {
          encoderBreakdown->recordStreamResource(0, hot.streamBuffers[0].value,
                                                 stream0Record->desc);
        }
        encoderBreakdown->recordStreamState(
            0, hot.streamBuffers[0].value, hot.streamOffsets[0],
            drawVertexStreamStride);
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
        const resources::BufferRecord* extraRecord = nullptr;
        if (hot.streamBuffers[stream]) {
          if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[stream].value);
              buffer && buffer->buffer) {
            extraRecord = buffer;
            extraVertexBuffer = WMT::Buffer{buffer->buffer.handle};
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
          if (encoderBreakdown) {
            if (extraRecord) {
              encoderBreakdown->recordStreamResource(
                  stream, hot.streamBuffers[stream].value, extraRecord->desc);
            }
            encoderBreakdown->recordStreamState(
                stream, hot.streamBuffers[stream].value,
                hot.streamOffsets[stream], streamBinding.stride);
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
  if (!effectiveSkipBaseStateBind) {
    PerfScope streamBindTexScope(perf::countEncodeDrawStreamBindCpuTime);
    if (!bindingPacket.fragmentTextureSamplers.empty()) {
      struct ResolvedFragmentTextureSamplerBinding {
        u32 stage = 0;
        core::Handle textureHandle{};
        u32 textureLod = 0;
        const resources::TextureRecord* textureRecord = nullptr;
        WMT::Texture texture{};
        core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
        bool srgbTexture = false;
        WMT::Reference<WMT::SamplerState> samplerRef{};
        WMT::SamplerState sampler{};
      };
      std::array<ResolvedFragmentTextureSamplerBinding, core::kMaxSamplers>
          resolvedFragmentBindings{};
      std::size_t resolvedFragmentBindingCount = 0;
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
        resolved.samplerStates = binding.samplerStates;
        if (auto* texture = ctx.pool.findTexture(textureHandle.value); texture && texture->texture) {
          const bool srgbTexture =
              core::flatStateOr(hot.samplerStates[stage], core::SAMP_SRGB_TEXTURE, 0u) != 0;
          resolved.srgbTexture = srgbTexture;
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
        std::array<dxmt9::argbuf_hybrid::ResourceArrayBinding,
                   dxmt9::shaders::kArgbufResourceArrayStageCount>
            argbufBindings{};
        std::size_t argbufBindingCount = 0;
        for (std::size_t i = 0; i < resolvedFragmentBindingCount; ++i) {
          const auto& binding = resolvedFragmentBindings[i];
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
        for (std::size_t i = 0; i < resolvedFragmentBindingCount; ++i) {
          const auto& binding = resolvedFragmentBindings[i];
          if (binding.texture) {
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
              recordedSetFragmentTexture(ctx, encoder, binding.texture,
                                         static_cast<std::uint8_t>(binding.stage));
              countTextureBind();
            }
          }
          if (binding.sampler) {
            bool skipSamplerBind = false;
            if (textureSamplerShadow && binding.stage < core::kMaxSamplers) {
              auto& slot = textureSamplerShadow->fragmentSamplers[binding.stage];
              const auto hash = textureSamplerShadowHash(
                  kFragmentSamplerShadowTag,
                  static_cast<std::uint8_t>(binding.stage),
                  binding.sampler.handle);
              skipSamplerBind = textureSamplerShadowMatches(
                  slot, hash, binding.sampler.handle);
              if (!skipSamplerBind) {
                textureSamplerShadowStore(slot, hash, binding.sampler.handle);
              }
            }
            if (skipSamplerBind) {
              countSamplerBindSkipped();
            } else {
              recordedSetFragmentSamplerState(ctx, encoder, binding.sampler,
                                              static_cast<std::uint8_t>(binding.stage));
              countSamplerBind();
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
        WMT::Reference<WMT::SamplerState> samplerRef{};
        WMT::SamplerState sampler{};
      };
      std::array<ResolvedVertexTextureSamplerBinding, core::kMaxVertexTextureSamplers>
          resolvedVertexBindings{};
      std::size_t resolvedVertexBindingCount = 0;
      for (const auto& binding : bindingPacket.vertexTextureSamplers) {
        const auto stage = binding.stage;
        const auto textureHandle = binding.texture;
        auto& resolved = resolvedVertexBindings[resolvedVertexBindingCount++];
        resolved.stage = stage;
        resolved.textureHandle = textureHandle;
        resolved.textureLod = binding.textureLod;
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
          if (textureSamplerShadow && binding.stage < core::kMaxVertexTextureSamplers) {
            auto& slot = textureSamplerShadow->vertexSamplers[binding.stage];
            const auto hash = textureSamplerShadowHash(
                kVertexSamplerShadowTag,
                static_cast<std::uint8_t>(binding.stage),
                binding.sampler.handle);
            skipSamplerBind = textureSamplerShadowMatches(
                slot, hash, binding.sampler.handle);
            if (!skipSamplerBind) {
              textureSamplerShadowStore(slot, hash, binding.sampler.handle);
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
    const bool forceExpandIndexed =
        debug::forceExpandIndexed() ||
        (autoExpandIndexed && !debug::disableAutoExpandIndexed());
    if (traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-expand-policy] seq=" << static_cast<unsigned long long>(seqId)
          << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
          << " auto=" << (autoExpandIndexed ? 1 : 0)
          << " env=" << (debug::forceExpandIndexed() ? 1 : 0)
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
      const DrawVolatile vol = buildDrawVolatile(drawVertexBaseIndex, drawVertexStreamOffset,
                                                  drawVertexStreamStride);
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
        PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
        recordedDrawPrimitives(ctx, encoder, primitiveType, 0, (uint64_t)vertexCount);
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
    {
      PerfScope streamBindIndexScope(perf::countEncodeDrawStreamBindCpuTime);
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
        if (buffer && buffer->buffer) {
          indexBuffer = WMT::Buffer{buffer->buffer.handle};
          if (!buffer->shadow.empty()) {
            indexBytesForReuse = buffer->shadow;
          } else if (buffer->contents) {
            indexBytesForReuse = std::span<const u8>(
                static_cast<const u8*>(buffer->contents),
                static_cast<std::size_t>(buffer->desc.size));
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
      const std::span<const u8> originalIndexBytesForReuse = indexBytesForReuse;
      const u32 originalIndexReuseStartIndex = indexReuseStartIndex;
      const u64 reverseStream0SpanMin =
          debug::probeReverseIndexedTrianglesStream0SpanMin();
      const u64 optimizeStream0SpanMin =
          debug::optimizeScreenBlendIndexOrderStream0SpanMin();
      splitPrimitiveLimit = debug::splitLargeIndexedDrawPrimitiveLimit();
      splitStream0SpanLimit = debug::splitLargeIndexedDrawStream0SpanMax();
      splitMaxChunksPerDraw = debug::splitLargeIndexedDrawMaxChunksPerDraw();
      const bool dumpIndexedGeometryRequested =
          !debug::indexedGeometryDumpDir().empty() &&
          debug::indexedGeometryDumpMaxDraws() != 0u &&
          pv.primitiveType == core::PrimitiveType::TriangleList;
      const bool measureProbeIndexLocality =
          debug::measureIndexReuse() || reverseStream0SpanMin != 0u ||
          optimizeStream0SpanMin != 0u || splitStream0SpanLimit != 0u ||
          dumpIndexedGeometryRequested;
      const u64 stream0StrideForProbe =
          encoderBreakdown ? encoderBreakdown->stats.streams[0].lastStride
                           : static_cast<u64>(hot.streamStrides[0]);
      const IndexReuseMeasure originalIndexReuseForProbe =
          measureProbeIndexLocality
              ? measureIndexReuseForDraw(originalIndexBytesForReuse,
                                         pv.indexType,
                                         originalIndexReuseStartIndex,
                                         vertexCount)
              : IndexReuseMeasure{.references = vertexCount};
      auto stream0SpanFilterMatches = [&](u64 minSpan) {
        if (minSpan == 0u) {
          return true;
        }
        return stream0ByteSpanForIndexMeasure(originalIndexReuseForProbe,
                                              stream0StrideForProbe) >= minSpan;
      };
      const bool reverseAllIndexedTriangles = debug::probeReverseIndexedTriangles();
      const bool reverseOpaqueIndexedTriangles =
          debug::probeReverseOpaqueIndexedTriangles();
      const bool reverseNonOpaqueIndexedTriangles =
          debug::probeReverseNonOpaqueIndexedTriangles();
      const bool sortIndexedTrianglesByMinIndex =
          debug::probeSortIndexedTrianglesByMinIndex();
      const bool optimizeIndexedTrianglesVertexCache =
          debug::probeOptimizeIndexedTrianglesVertexCache();
      const bool reverseTriangleProbeRequested =
          (reverseAllIndexedTriangles || reverseOpaqueIndexedTriangles ||
           reverseNonOpaqueIndexedTriangles || sortIndexedTrianglesByMinIndex ||
           optimizeIndexedTrianglesVertexCache) &&
          pv.primitiveType == core::PrimitiveType::TriangleList;
      const bool optimizeScreenBlendIndexOrderRequested =
          debug::optimizeScreenBlendIndexOrder() &&
          pv.primitiveType == core::PrimitiveType::TriangleList;
      bool probeConsidered = false;
      bool probeEligible = false;
      bool probeApplied = false;
      bool optimizedConsidered = false;
      bool optimizedEligible = false;
      bool optimizedApplied = false;
      const bool splitConsidered =
          (splitPrimitiveLimit != 0u || splitStream0SpanLimit != 0u) &&
          pv.primitiveType == core::PrimitiveType::TriangleList &&
          splitLargeIndexedDrawRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown);
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
      if (reverseTriangleProbeRequested &&
          reverseIndexedTriangleRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown)) {
        probeConsidered = true;
        const bool opaqueDepthWritingEligible =
            isOpaqueDepthWritingReorderProbeEligible(hot.renderStates, fillMode);
        const bool classEligible =
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
        probeEligible =
            classEligible &&
            (optimizeIndexedTrianglesVertexCache ||
             sortIndexedTrianglesByMinIndex || reverseAllIndexedTriangles ||
             (reverseOpaqueIndexedTriangles && opaqueDepthWritingEligible) ||
             (reverseNonOpaqueIndexedTriangles && !opaqueDepthWritingEligible));
        const bool probeIndexBytesBuilt =
            probeEligible &&
            (optimizeIndexedTrianglesVertexCache
                 ? buildVertexCacheOptimizedTriangleOrderIndexBytes(
                       indexBytesForReuse,
                       pv.indexType,
                       pv.startIndex,
                       vertexCount,
                       probeReorderedIndexBytes)
                 : (sortIndexedTrianglesByMinIndex
                        ? buildMinIndexSortedTriangleOrderIndexBytes(
                              indexBytesForReuse,
                              pv.indexType,
                              pv.startIndex,
                              vertexCount,
                              probeReorderedIndexBytes)
                        : buildReverseTriangleOrderIndexBytes(indexBytesForReuse,
                                                              pv.indexType,
                                                              pv.startIndex,
                                                              vertexCount,
                                                              probeReorderedIndexBytes)));
        if (probeIndexBytesBuilt) {
          transientIndexBuffer = makeTransientIndexBuffer(
              probeReorderedIndexBytes.data(), probeReorderedIndexBytes.size(),
              ActiveEncoderBreakdown::TransientIndexSource::ProbeReorder);
          if (transientIndexBuffer) {
            indexBuffer = transientIndexBuffer.buffer;
            indexBufferOffset = transientIndexBuffer.offset;
            indexBytesForReuse = std::span<const u8>(probeReorderedIndexBytes.data(),
                                                     probeReorderedIndexBytes.size());
            indexReuseStartIndex = 0;
            probeApplied = true;
          }
        }
      }
      if (!probeApplied && optimizeScreenBlendIndexOrderRequested &&
          screenBlendIndexOrderRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown)) {
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
            optimizedApplied = true;
          }
        }
      }
      const bool emitMeasureOnlyIndexedDraw =
          debug::measureIndexReuse() &&
          pv.primitiveType == core::PrimitiveType::TriangleList;
      bool dumpIndexedGeometryEligible = false;
      if (dumpIndexedGeometryRequested &&
          reverseIndexedTriangleRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
          indexedGeometryDumpShaderMatches(drawState)) {
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
      if (encoderBreakdown &&
          (probeConsidered || optimizedConsidered || scissorRectProbeConsidered ||
           splitConsidered || emitMeasureOnlyIndexedDraw ||
           dumpIndexedGeometryEligible)) {
        const auto& stream0 = encoderBreakdown->stats.streams[0];
        const u64 reorderBytes =
            (probeApplied || optimizedApplied)
                ? static_cast<u64>(probeReorderedIndexBytes.size())
                : 0u;
        const auto originalIndexReuse =
            measureProbeIndexLocality ? originalIndexReuseForProbe
                                      : IndexReuseMeasure{.references = vertexCount};
        const auto effectiveIndexReuse =
            measureProbeIndexLocality
                ? measureIndexReuseForDraw(indexBytesForReuse,
                                           pv.indexType,
                                           indexReuseStartIndex,
                                           vertexCount)
                : IndexReuseMeasure{.references = vertexCount};
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
            drawOrdinal,
            pv.primitiveType,
            primitiveCount,
            vertexCount,
            hot.textureMask,
            hot.renderStates,
            effectiveViewport,
            effectiveCullMode,
            fillMode,
            pv.baseVertexIndex,
            pv.startIndex,
            pv.indexType,
            hot.indexBuffer.value,
            stream0.lastHandle,
            stream0.lastOffset,
            stream0.lastStride,
            hot.viewport.scissor);
        if (probeConsidered) {
          encoderBreakdown->recordIndexedOrderProbe(
              probeApplied,
              probeApplied ? static_cast<u64>(probeReorderedIndexBytes.size()) : 0u);
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
              originalIndexBytesForReuse,
              vertexBytes,
              originalIndexReuse,
              pv.indexType,
              originalIndexReuseStartIndex,
              vertexCount,
              pv.baseVertexIndex,
              stream0.lastOffset,
              stream0.lastStride,
              stream0.lastHandle,
              hot.indexBuffer.value,
              drawState.hasShaderContext()
                  ? drawState.shaderContext().vertexShader.hash
                  : 0u,
              drawState.hasShaderContext()
                  ? drawState.shaderContext().pixelShader.hash
                  : 0u,
              drawOrdinal,
              primitiveCount,
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
          encoderBreakdown->recordIndexBufferState(hot.indexBuffer.value);
        }
        countIndexBufferBind();
        if (encoderBreakdown) {
          encoderBreakdown->recordIndexBufferMetalBind();
        }
      }
    }
    if (indexBuffer) {
      if (encoderBreakdown && debug::measureIndexReuse()) {
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
        PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
        const i32 metalBaseVertex = nativeBaseVertexUsed ? pv.baseVertexIndex : 0;
        const auto metalIndexType = toIndexType(pv.indexType);
        const bool submitSplitIndexed =
            splitWouldApply && !indexReorderApplied;
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
            recordedDrawIndexedPrimitives(ctx, encoder, primitiveType,
                                          metalIndexType,
                                          static_cast<u64>(chunkPrimitives) * 3u,
                                          indexBuffer, chunkIndexOffset, 1,
                                          metalBaseVertex, 0);
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
          recordedDrawIndexedPrimitives(ctx, encoder, primitiveType,
                                        metalIndexType,
                                        (uint64_t)vertexCount, indexBuffer,
                                        indexBufferOffset, 1, metalBaseVertex, 0);
        }
      }
      // R-BACK-13.1: run the tile-FFP imageblock post-pass after this draw.
      emitTileFfpPostPass();
      return true;
    }
  }
  const bool upDraw = !pv.userVertexData.empty();
  if (encoderBreakdown) {
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
    const DrawVolatile vol = buildDrawVolatile(drawVertexBaseIndex, drawVertexStreamOffset,
                                                drawVertexStreamStride);
    recordedSetVertexBytes(ctx, encoder, &vol, sizeof(DrawVolatile), 5);
    perf::countUniformVolatilePush();
    if (encoderBreakdown) {
      encoderBreakdown->addSetVertexBytes(sizeof(DrawVolatile), 5);
    }
    PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
    recordedDrawPrimitives(ctx, encoder, primitiveType, 0, (uint64_t)vertexCount);
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
                bool reopenArgbufHybrid,
                TextureSamplerBindShadow* textureSamplerShadow,
                std::uint32_t commandIndex) {
  return encodeDraw(ctx, commandBuffer, encoder, drawState, seqId,
                    skipBaseStateBind, preUploaded, paramOverride,
                    paramPayloadArena, dirty, tileFfpMode, argbufHybridMode,
                    argbufResourceArray, reopenArgbufHybrid, core::PsoHandle{},
                    core::PsoHandle{}, core::DepthStencilHandle{},
                    textureSamplerShadow, commandIndex, nullptr, nullptr);
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
  constexpr std::uint32_t kMaxRenderEncoderGpuSamples = 8192;
  const bool renderEncoderGpuSampling =
      renderEncoderGpuTimeEnabled() &&
      WMT::Device{ctx.device.handle}.supportsCounterSampling(
          WMTCounterSamplingPointAtStageBoundary);
  const std::uint32_t requestedRenderEncoderGpuSamples =
      static_cast<std::uint32_t>(std::min<std::size_t>(
          kMaxRenderEncoderGpuSamples,
          std::max<std::size_t>(2u, slot.commandCount() * 2u + 16u)));
  WMT::Reference<WMT::CounterSampleBuffer> renderEncoderGpuSampleBuffer =
      renderEncoderGpuSampling
          ? WMT::Device{ctx.device.handle}.newCounterSampleBuffer(
                requestedRenderEncoderGpuSamples, /*shared=*/true)
          : WMT::Reference<WMT::CounterSampleBuffer>{};
  std::vector<core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample>
      renderEncoderGpuSamples;
  std::uint32_t renderEncoderGpuSampleCursor = 0;
  struct RenderEncoderGpuAttachment {
    std::array<WMTSampleBufferAttachmentInfo, 1> attachments{};
    core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample sample{};
    bool active = false;

    std::span<const WMTSampleBufferAttachmentInfo> span() const {
      return active ? std::span<const WMTSampleBufferAttachmentInfo>(
                          attachments.data(), attachments.size())
                    : std::span<const WMTSampleBufferAttachmentInfo>{};
    }
  };
  auto makeRenderEncoderGpuAttachment = [&](
      core::metalqueue::RenderEncoderGpuPassType passType,
      std::size_t commandIndex,
      std::uint64_t rtHandle,
      std::uint64_t depthHandle,
      std::uint64_t psoHandle = 0) {
    RenderEncoderGpuAttachment result{};
    if (!renderEncoderGpuSampleBuffer ||
        renderEncoderGpuSampleCursor + 1u >= requestedRenderEncoderGpuSamples) {
      return result;
    }
    const std::uint32_t startSample = renderEncoderGpuSampleCursor++;
    const std::uint32_t endSample = renderEncoderGpuSampleCursor++;
    result.attachments[0] = WMTSampleBufferAttachmentInfo{
        .sample_buffer = renderEncoderGpuSampleBuffer.handle,
        .start_of_encoder_sample_index = startSample,
        .end_of_encoder_sample_index = endSample,
    };
    result.sample =
        core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample{
            .startIndex = startSample,
            .endIndex = endSample,
            .passType = passType,
            .seqId = slot.seqId,
            .slotIndex = slotIndex <= std::numeric_limits<std::uint32_t>::max()
                ? static_cast<std::uint32_t>(slotIndex)
                : std::numeric_limits<std::uint32_t>::max(),
            .commandIndex = commandIndex <= std::numeric_limits<std::uint32_t>::max()
                ? static_cast<std::uint32_t>(commandIndex)
                : std::numeric_limits<std::uint32_t>::max(),
            .rtHandle = rtHandle,
            .depthHandle = depthHandle,
            .psoHandle = psoHandle,
        };
    result.active = true;
    return result;
  };
  auto recordRenderEncoderGpuAttachment =
      [&](const RenderEncoderGpuAttachment& attachment) {
        if (attachment.active) {
          renderEncoderGpuSamples.push_back(attachment.sample);
        }
      };
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
  // R-BACK-12.22..12.26 (resource-array sub-mode) — current render
  // encoder's resource-array sub-state. Set at startRenderPass when the
  // resource-array lane is active for the queue AND the pass opened the
  // resource-array argbuf. Consumed by encodeDraw to (a) stamp the
  // ShaderVariantKey::argbufResourceArray PSO bit and (b) route fragment
  // textures/samplers through populateResourceBindings + useResource. Like
  // activePassUsesArgbufHybrid the pass is sticky — never mid-pass switch.
  bool activePassUsesArgbufResourceArray = false;
  [[maybe_unused]] WMT::Buffer activeArgbufStorage{};
  [[maybe_unused]] std::uint64_t activeArgbufOffset = 0;
  std::optional<core::FlatDrawStateKey> activeDrawStateKey;
  std::optional<core::ClearDesc> pendingClear;
  std::size_t pendingClearCommandIndex = std::numeric_limits<std::size_t>::max();
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

  // R-BACK-12.22..12.26 — constants-only argbuf reopen gate. Tracks the
  // uniform payload hash last written into the active encoder's argbuf
  // descriptor table. A DrawRun whose payload matches reuses that table
  // (no fresh reservation, no rebind); a changed payload forces a fresh
  // table so draws can't observe last-write-wins on a shared table. Reset
  // whenever a new encoder opens (its argbuf table starts empty).
  std::optional<u64> lastArgbufPayloadHash;
  ArgbufCbufCache argbufCbufCache;
  TextureSamplerBindShadow textureSamplerShadow{};
  ActiveEncoderBreakdown activeEncoderBreakdown;
  static thread_local RenderPassFrameTracker renderPassFrameTracker;
  u64 renderEncoderIndex = 0;

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
      activeEncoderBreakdown.emit(reason);
      // R-BACK-15.4: color attachments stored on this pass become "touched"
      // on the queue so the next pass on the same handle Loads instead of
      // DontCare-loads. The current color DontCare-store proof only fires
      // when the next touch is a Clear, whose load action ignores this
      // touched bit, so conservatively marking every active color handle
      // keeps the touched-set contract simple without losing that win.
      for (auto& handle : activeColorHandles) {
        if (handle) {
          ctx.queue.markColorHandleTouched(handle);
          handle = core::Handle{};
        }
      }
      activeRenderEncoder = {};
      hasActiveRender = false;
      activeDrawStateKey.reset();
      textureSamplerShadow.reset();
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
                             std::size_t lookaheadStartIndex,
                             core::PsoHandle renderPsoHandle) {
    // TLA+: EncoderLifecycle / BeginRender(rt)
    // Callers split through None before opening a new render encoder.
    // R-BACK-15.7: pass the slot + current command index so beginRenderPass
    // can run the depth/stencil DontCare-store look-ahead over the
    // remaining records.
    assertNoActiveEncoder();
    const auto sampleAttachment = makeRenderEncoderGpuAttachment(
        core::metalqueue::RenderEncoderGpuPassType::Draw,
        lookaheadStartIndex,
        drawState.hot->colorAttachments[0].handle.value,
        drawState.hot->depthStencil.handle.value,
        psoHandleBucket(renderPsoHandle));
    const u64 openedRenderEncoderIndex = renderEncoderIndex;
    activeRenderEncoder = beginRenderPass(ctx, commandBuffer, drawState, clear,
                                          &slot, lookaheadStartIndex,
                                          sampleAttachment.span());
    hasActiveRender = static_cast<bool>(activeRenderEncoder);
    if (hasActiveRender) {
      renderPassFrameTracker.noteStart(
          makeRenderPassFrameKey(*drawState.hot),
          estimateRenderPassAttachmentFootprintBytes(ctx, *drawState.hot));
      activeEncoderBreakdown.begin(
          encodeChunkSeqId, openedRenderEncoderIndex,
          drawState.hot->colorAttachments[0].handle.value,
          drawState.hot->depthStencil.handle.value);
      activeEncoderBreakdown.recordAttachmentMetadata(ctx.pool, *drawState.hot);
      ++renderEncoderIndex;
      recordRenderEncoderGpuAttachment(sampleAttachment);
    }
    activeKey = makeAttachmentKey(*drawState.hot);
    activeWriteHazard = makeAttachmentHazard(*drawState.hot);
    activeDrawStateKey.reset();
    textureSamplerShadow.reset();
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
          "RenderPass[seq=%llu,enc=%llu,rt=0x%llx,depth=0x%llx]",
          static_cast<unsigned long long>(encodeChunkSeqId),
          static_cast<unsigned long long>(openedRenderEncoderIndex), rt0,
          depth);
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
    // (vertex + fragment) of the active render encoder. Stage 2 PSOs
    // read through this slot-30 argbuf; encodeDraw skips the direct
    // Stage 1 slot 0 / slot 3 and texture/sampler binds in this mode.
    //
    // When the gate fails (any non-Apple-Silicon device) `openArgbuf`
    // returns an empty handle and we fall through to the Stage 1
    // counter; no slot-30 bind is issued.
    activePassUsesArgbufHybrid = false;
    activePassUsesArgbufResourceArray = false;
    activeArgbufStorage = {};
    activeArgbufOffset = 0;
    {
      const auto argbufDecision = dxmt9::pipeline::selectArgbufHybridForPass(
          drawState, ctx.pool.argbufHybridEnabled());
      if (argbufDecision == dxmt9::pipeline::ArgbufHybridDecision::Stage2) {
        perf::countArgbufHybridEncoder();
        // R-BACK-12.22..12.26 (resource-array sub-mode) — pick the
        // resource-array encoder (20-entry table, larger encodedLength) when
        // the lane is active for the queue; otherwise the constants-only
        // encoder. Both anchor onto a fresh transient slab; the only delta is
        // the reservation size and whether texture/sampler slots are written.
        const bool resourceArrayLane = ctx.queue.resourceArrayLaneActive() &&
            ctx.queue.resourceArrayEncoderResource().initialized();
        auto& encoderResource = resourceArrayLane
                                    ? ctx.queue.resourceArrayEncoderResource()
                                    : ctx.queue.argbufEncoderResource();
        const auto populated = dxmt9::argbuf_hybrid::openArgbuf(
            ctx.queue, encoderResource, encodeChunkSeqId);
        if (populated) {
          // Constant-buffer entries (VsConsts/PsConsts/FfpVsConsts/
          // FfpPsConsts) are populated lazily from encodeDraw's dirty
          // path on the first draw. Texture/sampler resources remain on
          // the direct fragment binding lane for texture-bound Stage 2
          // draws, so encoder open only binds the argbuf storage.
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
          activeEncoderBreakdown.addArgbufTableBytes(populated.length);
          activePassUsesArgbufHybrid = true;
          activePassUsesArgbufResourceArray = resourceArrayLane;
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
    // The fresh encoder's argbuf table (opened above) is empty, so the
    // first draw of this pass must reopen + populate regardless of its
    // payload hash.
    lastArgbufPayloadHash.reset();
    argbufCbufCache.reset();
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
    const auto& clear = *pendingClear;
    const auto sampleAttachment = makeRenderEncoderGpuAttachment(
        core::metalqueue::RenderEncoderGpuPassType::Clear,
        pendingClearCommandIndex,
        clear.colorAttachments[0].handle.value,
        clear.depthStencil.handle.value);
    dxmt9::encoders::encodeClearPass(commandBuffer, ctx.pool, clear,
                                     sampleAttachment.span());
    recordRenderEncoderGpuAttachment(sampleAttachment);
    commandBufferHasWork = true;
    pendingClear.reset();
    pendingClearCommandIndex = std::numeric_limits<std::size_t>::max();
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
      perf::countSubCommandBufferSplitSuppressedByCap();
      return;
    }
    splitMidChunk();
  };

  auto encodeDrawRunCommand = [&](std::size_t commandIndex,
                                  const core::MetalCommandView& command) {
    if (!command.drawState.hot || !command.drawState.shaderLayout ||
        !command.drawUniformPayload ||
        core::drawRunDrawCount(command) == 0) return;
    auto stateView = command.drawState;
    stateView.uniforms = command.drawUniformPayload;
    const auto& hot = *stateView.hot;
    const auto drawItems =
        command.drawItems.empty() ? command.drawParams : command.drawItems;
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
        startRenderPass(stateView, pendingClear, commandIndex, renderPsoHandle);
        pendingClear.reset();
        pendingClearCommandIndex = std::numeric_limits<std::size_t>::max();
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
          startRenderPass(stateView, std::nullopt, commandIndex, renderPsoHandle);
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
        startRenderPass(stateView, std::nullopt, commandIndex, renderPsoHandle);
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
    const std::size_t drawCount = drawItems.size();
    const auto recordPayloadArena = core::drawRunPayloadBytes(command);
    bool anyUpData = false;
    bool hasUpPayloadRanges = false;
    for (const auto& param : drawItems) {
      if (!param.userVertexRange.empty() || !param.userIndexRange.empty()) {
        hasUpPayloadRanges = true;
        break;
      }
    }
    std::vector<CommandQueue::TransientBufferSlice> upSlices;
    if (hasUpPayloadRanges) {
      std::vector<std::span<const std::byte>> upPayloads;
      upPayloads.reserve(drawCount * 2);
      for (const auto& param : drawItems) {
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
        if (!upSlices.empty()) {
          for (const auto& param : drawItems) {
            const auto vertexBytes = drawParamVertexBytes(param, recordPayloadArena);
            const auto indexBytes = drawParamIndexBytes(param, recordPayloadArena);
            activeEncoderBreakdown.addTransientVertexBytes(
                static_cast<u64>(vertexBytes.size()),
                ActiveEncoderBreakdown::TransientVertexSource::Preupload);
            activeEncoderBreakdown.addTransientIndexBytes(
                static_cast<u64>(indexBytes.size()),
                ActiveEncoderBreakdown::TransientIndexSource::Preupload);
          }
        }
      }
    }

    // encodeDraw receives the per-draw fields through DrawParam while
    // all base state is read from the canonical hot/shader view.
    // Per-frequency UBOs (VsConsts/PsConsts/FfpVsConsts/FfpPsConsts)
    // bind only on dirty (R-BACK-12.5/12.8); DrawVolatile is pushed
    // via setVertexBytes per draw with no slab traffic.
    for (std::size_t i = 0; i < drawCount; ++i) {
      const auto& param = drawItems[i];
      const auto* drawUniformPayload =
          core::drawRunUniformPayloadForParam(command, param);
      if (!drawUniformPayload) {
        continue;
      }
      PreUploadedDrawData preData{};
      if (i * 2u + 1u < upSlices.size()) {
        preData.vertex = upSlices[i * 2u];
        preData.index = upSlices[i * 2u + 1u];
      }
      core::DrawBindingOverride bindingOverride{};
      core::FlatDrawStateRecord overrideHot{};
      core::DrawShaderLayoutContext overrideShaderLayout{};
      auto drawStateView = stateView;
      bool hasBindingOverride =
          drawParamBindingOverride(param, recordPayloadArena, bindingOverride);
      if (hasBindingOverride) {
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
      const u64 drawArgbufPayloadHash = drawUniformPayload->hash;
      const bool argbufPayloadChanged =
          !lastArgbufPayloadHash.has_value() ||
          *lastArgbufPayloadHash != drawArgbufPayloadHash;
      const bool reopenArgbuf =
          activePassUsesArgbufResourceArray ||
          argbufPayloadChanged;
      const bool baseStateCompatible =
          activeDrawStateKey.has_value() &&
          core::drawStateKeysCompatibleForDrawRunBatch(
              *activeDrawStateKey, drawStateView.hot->key);
      const bool overrideNeedsBaseStateBind =
          hasBindingOverride &&
          drawBindingOverrideRequiresBaseStateBind(
              bindingOverride, stateView.shaderLayout);
      const bool skipBaseStateBind =
          baseStateCompatible && !overrideNeedsBaseStateBind;
      if (encodeDraw(ctx, commandBuffer, activeRenderEncoder, drawStateView, slot.seqId,
                     /*skipBaseStateBind=*/skipBaseStateBind,
                     anyUpData ? &preData : nullptr,
                     &param,
                     recordPayloadArena,
                     &uniformDirty,
                     /*tileFfpMode=*/activePassUsesTileFfp,
                     /*argbufHybridMode=*/activePassUsesArgbufHybrid,
                     /*argbufResourceArray=*/activePassUsesArgbufResourceArray,
                     /*reopenArgbufHybrid=*/reopenArgbuf,
                     renderPsoHandle,
                     tilePsoHandle,
                     depthStencilHandle,
                     &textureSamplerShadow,
                     commandIndex <= std::numeric_limits<std::uint32_t>::max()
                         ? static_cast<std::uint32_t>(commandIndex)
                         : std::numeric_limits<std::uint32_t>::max(),
                     &activeEncoderBreakdown,
                     &argbufCbufCache)) {
        activeDrawStateKey = drawStateView.hot->key;
        lastArgbufPayloadHash = drawArgbufPayloadHash;
      }
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
    // record; splitBeforeBlockingPresent owns the present-tail boundary.
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

  using Kind = core::MetalCommandKind;
  if (slot.drawOnlyCommandStream()) {
    for (std::size_t commandIndex = 0; commandIndex < slot.commandCount(); ++commandIndex) {
      const auto command = slot.drawRunCommandAt(commandIndex);
      // TLA+: EncoderLifecycle / opCount observes command replay progress.
      encodeDrawRunCommand(commandIndex, command);
      applyPerRecordSplitPolicy(/*presentRecord=*/false);
    }
  } else {
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
          pendingClearCommandIndex = commandIndex;
        } else {
          const auto sampleAttachment = makeRenderEncoderGpuAttachment(
              core::metalqueue::RenderEncoderGpuPassType::Clear,
              commandIndex,
              clear.colorAttachments[0].handle.value,
              clear.depthStencil.handle.value);
          dxmt9::encoders::encodeClearPass(commandBuffer, ctx.pool, clear,
                                           sampleAttachment.span());
          recordRenderEncoderGpuAttachment(sampleAttachment);
          commandBufferHasWork = true;
        }
        break;
      }
      case Kind::DrawRun: {
        encodeDrawRunCommand(commandIndex, command);
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
        // Resolve the queue-local Presenter binding once per Present
        // packet and reclaim any acquire-before-present token stashed by
        // submitPresent. A stale PresentId (swapchain destroyed since
        // submission) produces a nullptr Presenter — encodePresent then
        // short-circuits to a skipped present.
        dxmt9::Presenter* const presenter = ctx.queue.lookupPresenter(present.presentId);
        auto pendingDrawableToken = ctx.queue.takeDrawableToken(present.presentId);
        const bool noteAfterAcquire = presentBoundaryAfterAcquireEnabled();
        if (!noteAfterAcquire) {
          ctx.queue.notePresentDequeued(slot.seqId);
        }
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::Present,
            commandIndex,
            presentSource.value,
            0);
        const bool presentEncoded = dxmt9::encodePresent(commandBuffer, ctx.pool,
                                                          presenter,
                                                          std::move(pendingDrawableToken),
                                                          present, presentSource, slot.seqId,
                                                          sampleAttachment.span());
        if (presentEncoded) {
          recordRenderEncoderGpuAttachment(sampleAttachment);
        }
        if (noteAfterAcquire) {
          ctx.queue.notePresentDequeued(slot.seqId);
        }
        if (presentEncoded) {
          commandBufferHasWork = true;
          ctx.queue.backBufferDiscardAfterPresent_ = true;
          if (presenter) {
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
        renderPassFrameTracker.reset();
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
      applyPerRecordSplitPolicy(command.kind == Kind::Present);
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
  record.commandBufferChainLength = perChunkSubCBCount + 1;
  if (metalCaptureRequest.has_value()) {
    record.metalCaptureDevice = WMT::Device{ctx.device.handle};
    record.metalCapture = std::move(metalCaptureRequest);
    record.metalCaptureAlreadyStarted = captureAlreadyStartedAtChunkBegin;
  }
  record.slotIndex = slotIndex;
  record.seqId = seqId;
  record.context = "queue";
  record.renderEncoderGpuSampleBuffer = std::move(renderEncoderGpuSampleBuffer);
  record.renderEncoderGpuSamples = std::move(renderEncoderGpuSamples);
  record.postCommitCallbacks = std::move(postCommitCallbacks);
  return record;
  }  // @autoreleasepool
}

}  // namespace dxmt9::encoders
