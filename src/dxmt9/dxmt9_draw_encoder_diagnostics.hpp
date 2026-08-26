#pragma once

// Draw-encoder diagnostic state and call seams shared only by the encoder core
// and its diagnostics translation unit. Diagnostic spans remain call-local;
// session-owned values retain their existing flat member order.

#include "dxmt9_draw_encoder_internal.hpp"
#include "dxmt9_argbuf_hybrid.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_state.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_resource_pool.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dxmt9::encoders {

using PerfCounterFn = void (*)(std::uint64_t);

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


// M1/M2 — printf-style label/group-name builder. Returns a non-owning
// WMT::String view backed by an autoreleased NSString. Lifetime is safe
// because the receiving setLabel:/pushDebugGroup: selector retains
// immediately and encodeChunk runs inside an @autoreleasepool.
template <std::size_t Cap = 96>
inline WMT::String makeLabelStringFmt(const char* fmt, ...) {
  char buf[Cap];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return WMT::String::string(buf, WMTUTF8StringEncoding);
}


struct VisibilityScoutDrawRecord {
  u64 seqId = 0;
  u64 encoderIndex = 0;
  std::uint32_t commandIndex = 0;
  u64 drawOrdinal = 0;
  std::uint32_t resultIndex = 0;
  std::uint32_t metalDrawIndex = 0;
  std::uint32_t primitiveType = 0;
  std::uint32_t sourcePrimitiveCount = 0;
  u64 submittedPrimitiveCount = 0;
  u64 submittedElementCount = 0;
  std::uint32_t indexed = 0;
  std::uint32_t expandedIndexed = 0;
  std::uint32_t splitChunk = 0;
  u64 rt0 = 0;
  u64 depth = 0;
  std::uint32_t textureMask = 0;
  std::uint32_t colorWrite = 0;
  std::uint32_t zEnable = 0;
  std::uint32_t zWrite = 0;
  std::uint32_t zFunc = 0;
  std::uint32_t alphaBlend = 0;
  std::uint32_t alphaTest = 0;
  std::uint32_t scissor = 0;
  std::uint32_t cull = 0;
  std::uint32_t fill = 0;
};

struct VisibilityScoutPass {
  WMT::Reference<WMT::Buffer> buffer{};
  std::uint64_t* results = nullptr;
  std::vector<VisibilityScoutDrawRecord> records;
  std::string path;
  u64 seqId = 0;
  u64 encoderIndex = 0;
  std::uint32_t capacity = 0;
  std::uint32_t metalDrawIndex = 0;
  bool overflow = false;
};

struct ActiveDepthAttachmentDump {
  core::Handle handle{};
  WMT::Reference<WMT::Texture> texture{};
  core::Format format = core::Format::Unknown;
  enum WMTPixelFormat metalPixelFormat = WMTPixelFormatInvalid;
  u32 width = 0;
  u32 height = 0;
  u64 seq = 0;
  u64 enc = 0;
  bool hasDepth = false;
  bool hasStencil = false;
};

struct ActiveColorAttachmentDump {
  core::Handle handle{};
  WMT::Reference<WMT::Texture> texture{};
  core::Format format = core::Format::Unknown;
  enum WMTPixelFormat metalPixelFormat = WMTPixelFormatInvalid;
  u32 width = 0;
  u32 height = 0;
  u32 index = 0;
  u64 seq = 0;
  u64 enc = 0;
  u64 draw = 0;
  u64 commandIndex = 0;
  u64 commandDrawIndex = 0;
  u64 commandDrawCount = 0;
  u64 texture0 = 0;
  bool afterDraw = false;
};

struct ColorAttachmentReadbackRegion {
  u32 x = 0;
  u32 y = 0;
  u32 width = 0;
  u32 height = 0;
};

struct ActiveDrawTextureDump {
  core::Handle handle{};
  WMT::Texture texture{};
  core::Format format = core::Format::Unknown;
  core::TextureType type = core::TextureType::TwoD;
  enum WMTPixelFormat storageMetalPixelFormat = WMTPixelFormatInvalid;
  enum WMTPixelFormat shaderMetalPixelFormat = WMTPixelFormatInvalid;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 levels = 1;
  u32 textureIndex = 0;
  u32 stage = 0;
  bool vertexStage = false;
  bool srgb = false;
  bool shaderReadView = false;
  u64 seq = 0;
  u64 enc = 0;
};

struct TextureSubresourceReadback {
  u32 level = 0;
  u32 slice = 0;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 rowBytes = 0;
  u64 bytesPerImage = 0;
  u64 byteCount = 0;
  std::string basename;
  WMT::Reference<WMT::Buffer> buffer{};
  const void* bytes = nullptr;
};

struct TextureSidecarReadbackBatch {
  ActiveDrawTextureDump active{};
  std::vector<TextureSubresourceReadback> subresources;
};

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

// Sequential partition of encodeDraw. PerfScope times the region it wraps, so
// the branches BETWEEN the child scopes are counted by nothing -- 34% of
// encode_draw, 14% of the GT2 frame
// (state-churn-encode-append-decomposition.13). This instead stamps the clock at
// fixed points and attributes each interval to exactly one phase, so the phases
// sum to the parent by construction and the residual has nowhere to hide. The
// destructor closes the final phase, which also catches early returns.
//
// Gated by DXMT9_PERF_ENCODE_DRAW_PHASE_SPLIT, NOT always on. It was first
// committed always-on on the strength of a measurement that priced the whole
// PerfScope family at 0.12ms/present -- but that A/B compared FRAME WALL, which
// .12 had already shown is insensitive to encode-thread CPU because that thread
// carries ~17ms/present of slack. Measured against the encode STAGE wall
// instead -- an event-timestamp counter that survives a no-op PerfScope build --
// the family costs 4.64ms/present and these nine marks add 0.645ms, 1.6% of the
// frame. A clock read here is ~43ns against the PE side's ~180ns through Wine's
// QPC: four times cheaper, not free.
bool encodeDrawPhaseSplitPerfEnabled();

class EncodeDrawPhaseTimer {
 public:
  EncodeDrawPhaseTimer() : enabled_(encodeDrawPhaseSplitPerfEnabled()) {
    if (enabled_) {
      last_ = std::chrono::steady_clock::now();
    }
  }
  // Closes the final phase at every exit. encodeDraw's common path returns from
  // inside a nested block (the `return true` after emitTileFfpPostPass), so on a
  // typical draw this counter -- NOT the phase mark that follows that return --
  // is what measures the indexed-setup + draw-issue region. Named "remainder"
  // rather than for any one region because that is what it is on every path.
  ~EncodeDrawPhaseTimer() { mark(perf::countEncodeDrawPhaseRemainderCpuTime); }

  void mark(void (*record)(std::uint64_t)) {
    if (!enabled_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    record(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_).count()));
    last_ = now;
  }

  EncodeDrawPhaseTimer(const EncodeDrawPhaseTimer&) = delete;
  EncodeDrawPhaseTimer& operator=(const EncodeDrawPhaseTimer&) = delete;

 private:
  bool enabled_ = false;
  std::chrono::steady_clock::time_point last_{};
};

class PerfScope {
 public:
  // The optional second target splits a child counter that has more than one
  // call site. It costs one atomic add and NO extra clock read -- the elapsed
  // value is already computed -- and is null unless
  // DXMT9_PERF_ENCODE_DRAW_PHASE_SPLIT is set. Without it the per-phase
  // named/unnamed split is uncomputable, because stream_bind's five sites and
  // fvf_decode's three straddle phases and the aggregate belongs to none of
  // them (state-churn-encode-append-decomposition.14).
  explicit PerfScope(void (*record)(std::uint64_t),
                     void (*site)(std::uint64_t) = nullptr)
      : record_(record),
        site_(site && encodeDrawPhaseSplitPerfEnabled() ? site : nullptr) {
    if (record_) {
      started_ = std::chrono::steady_clock::now();
    }
  }
  ~PerfScope() {
    if (!record_) {
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    record_(ns);
    if (site_) {
      site_(ns);
    }
  }

  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  void (*record_)(std::uint64_t) = nullptr;
  void (*site_)(std::uint64_t) = nullptr;
  std::chrono::steady_clock::time_point started_{};
};

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

bool x8ShaderAlphaFillEnabledForDiagnostics();
bool encoderBreakdownCbufContentEnabled();

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
    bool argbufDirectCbufMode = false;
    bool samplerLodBias = false;
    u32 fetch4SamplerMask = 0;
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
    StagedStream,
  };

  enum class TransientIndexSource {
    User,
    Preupload,
    ShadowFallback,
    ProbeReorder,
    OptimizedOrder,
    StagedIb,
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

  std::string streamExtraBindingsSummary() const {
    std::ostringstream out;
    bool first = true;
    for (std::size_t stream = 1; stream < stats.streams.size(); ++stream) {
      const auto& slot = stats.streams[stream];
      if (!slot.valid || slot.samples == 0) {
        continue;
      }
      if (!first) {
        out << ';';
      }
      first = false;
      out << 's' << stream << ":0x" << std::hex << slot.lastHandle << std::dec
          << '@' << slot.lastOffset << '/' << slot.lastStride;
    }
    return out.str();
  }

  void begin(u64 seqId, u64 encoderIndex, u64 rtHandle, u64 depthHandle) {
    enabled = perf::encoderBreakdownEnabled();
    if (enabled && !perf::encoderBreakdownSeqAllowed(seqId)) {
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

  void recordRenderPassActions(const RenderPassActionSummary& summary) {
    if (!enabled) {
      return;
    }
    stats.colorAttachmentCount = summary.colorAttachmentCount;
    stats.color0Included = summary.color0Included;
    stats.color0LoadAction = summary.color0LoadAction;
    stats.color0StoreAction = summary.color0StoreAction;
    stats.color0Clear = summary.color0Clear;
    stats.colorLoadBytes = summary.colorLoadBytes;
    stats.colorStoreBytes = summary.colorStoreBytes;
    stats.depthIncluded = summary.depthIncluded;
    stats.depthLoadAction = summary.depthLoadAction;
    stats.depthStoreAction = summary.depthStoreAction;
    stats.depthClear = summary.depthClear;
    stats.depthLoadBytes = summary.depthLoadBytes;
    stats.depthStoreBytes = summary.depthStoreBytes;
    stats.stencilIncluded = summary.stencilIncluded;
    stats.stencilLoadAction = summary.stencilLoadAction;
    stats.stencilStoreAction = summary.stencilStoreAction;
    stats.stencilClear = summary.stencilClear;
    stats.stencilLoadBytes = summary.stencilLoadBytes;
    stats.stencilStoreBytes = summary.stencilStoreBytes;
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
                                const core::FlatRenderStateSet& renderStates,
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
                                 bool fragmentlessDepthOnlyProbeApplied,
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
                                 IndexReuseMeasure candidateIndexReuse,
                                 bool candidateBuilt,
                                 bool candidateGatePassed,
                                 u64 drawOrdinal,
                                 u64 commandIndex,
                                 core::PrimitiveType primitiveType,
                                 u32 primitiveCount,
                                 u64 vertexCount,
                                 u32 textureMask,
                                 std::span<const core::Handle> fragmentTextures,
                                 const core::FlatRenderStateSet& renderStates,
                                 const core::ViewportScissor& viewport,
                                 WMTCullMode cullMode,
                                 WMTTriangleFillMode fillMode,
                                 i32 baseVertexIndex,
                                 u32 startIndex,
                                 core::IndexType indexType,
                                 u64 indexBufferHandle,
                                 const char* effectiveIndexSource,
                                 u64 effectiveIndexOffset,
                                 u64 effectiveIndexBytes,
                                 u64 stream0Handle,
                                 u64 stream0Offset,
                                 u64 stream0Stride,
                                 const char* streamExtraBindings,
                                 u64 vertexConstantsHash,
                                 u64 pixelConstantsHash,
                                 u64 uniformPayloadHash,
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
    auto textureHandleValue = [&](std::size_t stage) -> u64 {
      return stage < fragmentTextures.size() ? fragmentTextures[stage].value : 0ull;
    };
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
    const auto candidateStreamByteMin =
        streamByteMin(candidateIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    const auto candidateStreamByteMax =
        streamByteMax(candidateIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    std::fprintf(
        stderr,
        "[dxmt9-perf-indexed-probe-draw seq=%llu encoder=%llu "
        "encoder_draw_index=%llu draw_ordinal=%llu command_index=%llu "
        "eligible=%u applied=%u "
        "optimized_eligible=%u optimized_applied=%u "
        "scissor_rect_eligible=%u scissor_rect_applied=%u "
        "alpha_blend_probe_applied=%u depth_write_probe_applied=%u "
        "depth_func_probe_applied=%u "
        "fragmentless_depth_only_probe_applied=%u reorder_bytes=%llu "
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
        "candidate_built=%u candidate_gate_passed=%u "
        "candidate_index_available=%u candidate_index_unique=%llu "
        "candidate_index_min=%u candidate_index_max=%u candidate_index_span=%llu "
        "candidate_index_first=%u candidate_index_last=%u "
        "candidate_cache_miss16=%llu candidate_cache_miss32=%llu "
        "candidate_cache_miss64=%llu candidate_adjacent_delta_sum=%llu "
        "candidate_adjacent_delta_max=%u candidate_backward_jumps=%llu "
        "candidate_triangle_index_span_sum=%llu "
        "candidate_triangle_index_span_max=%u "
        "candidate_stream0_byte_min=%llu candidate_stream0_byte_max=%llu "
        "candidate_stream0_byte_span=%llu "
        "primitive_type=%u primitive_count=%u vertex_count=%llu "
        "texture_mask=0x%x "
        "texture0=0x%llx texture1=0x%llx texture2=0x%llx texture3=0x%llx "
        "texture4=0x%llx texture5=0x%llx texture6=0x%llx texture7=0x%llx "
        "color_write=0x%x alpha_blend=%u "
        "src_blend=%u dst_blend=%u blend_op=%u separate_alpha=%u "
        "src_blend_alpha=%u dst_blend_alpha=%u blend_op_alpha=%u "
        "alpha_test=%u depth_enabled=%u "
        "depth_write=%u depth_func=%u stencil=%u clip_plane=%u scissor=%u "
        "scissor_l=%d scissor_t=%d scissor_r=%d scissor_b=%d "
        "original_scissor_l=%d original_scissor_t=%d "
        "original_scissor_r=%d original_scissor_b=%d "
        "cull=%u fill=%u base_vertex=%d start_index=%u index_type=%u "
        "index_buffer=0x%llx effective_index_source=%s "
        "effective_index_offset=%llu effective_index_bytes=%llu "
        "stream0_handle=0x%llx stream0_offset=%llu "
        "stream0_stride=%llu stream_extra_bindings=%s "
        "pso=0x%llx shader_variant=0x%llx "
        "vs=0x%llx ps=0x%llx vs_constants_hash=0x%llx "
        "ps_constants_hash=0x%llx uniform_payload_hash=0x%llx "
        "vsout=0x%x]\n",
        static_cast<unsigned long long>(stats.seqId),
        static_cast<unsigned long long>(stats.encoderIndex),
        static_cast<unsigned long long>(stats.drawCalls),
        static_cast<unsigned long long>(drawOrdinal),
        static_cast<unsigned long long>(commandIndex),
        probeEligible ? 1u : 0u,
        probeApplied ? 1u : 0u,
        optimizedEligible ? 1u : 0u,
        optimizedApplied ? 1u : 0u,
        scissorRectEligible ? 1u : 0u,
        scissorRectApplied ? 1u : 0u,
        alphaBlendProbeApplied ? 1u : 0u,
        depthWriteProbeApplied ? 1u : 0u,
        depthFuncProbeApplied ? 1u : 0u,
        fragmentlessDepthOnlyProbeApplied ? 1u : 0u,
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
        candidateBuilt ? 1u : 0u,
        candidateGatePassed ? 1u : 0u,
        candidateIndexReuse.available ? 1u : 0u,
        static_cast<unsigned long long>(candidateIndexReuse.unique),
        candidateIndexReuse.minIndex,
        candidateIndexReuse.maxIndex,
        static_cast<unsigned long long>(
            candidateIndexReuse.available
                ? static_cast<u64>(candidateIndexReuse.maxIndex) -
                      static_cast<u64>(candidateIndexReuse.minIndex) + 1u
                : 0u),
        candidateIndexReuse.firstIndex,
        candidateIndexReuse.lastIndex,
        static_cast<unsigned long long>(candidateIndexReuse.cacheMiss16),
        static_cast<unsigned long long>(candidateIndexReuse.cacheMiss32),
        static_cast<unsigned long long>(candidateIndexReuse.cacheMiss64),
        static_cast<unsigned long long>(candidateIndexReuse.adjacentDeltaAbsSum),
        candidateIndexReuse.adjacentDeltaMax,
        static_cast<unsigned long long>(candidateIndexReuse.backwardJumps),
        static_cast<unsigned long long>(candidateIndexReuse.triangleIndexSpanSum),
        candidateIndexReuse.triangleIndexSpanMax,
        static_cast<unsigned long long>(candidateStreamByteMin),
        static_cast<unsigned long long>(candidateStreamByteMax),
        static_cast<unsigned long long>(
            candidateStreamByteMax >= candidateStreamByteMin
                ? candidateStreamByteMax - candidateStreamByteMin
                : 0u),
        static_cast<unsigned>(primitiveType),
        primitiveCount,
        static_cast<unsigned long long>(vertexCount),
        textureMask,
        static_cast<unsigned long long>(textureHandleValue(0)),
        static_cast<unsigned long long>(textureHandleValue(1)),
        static_cast<unsigned long long>(textureHandleValue(2)),
        static_cast<unsigned long long>(textureHandleValue(3)),
        static_cast<unsigned long long>(textureHandleValue(4)),
        static_cast<unsigned long long>(textureHandleValue(5)),
        static_cast<unsigned long long>(textureHandleValue(6)),
        static_cast<unsigned long long>(textureHandleValue(7)),
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
        effectiveIndexSource ? effectiveIndexSource : "",
        static_cast<unsigned long long>(effectiveIndexOffset),
        static_cast<unsigned long long>(effectiveIndexBytes),
        static_cast<unsigned long long>(stream0Handle),
        static_cast<unsigned long long>(stream0Offset),
        static_cast<unsigned long long>(stream0Stride),
        streamExtraBindings ? streamExtraBindings : "",
        static_cast<unsigned long long>(psoHandle),
        static_cast<unsigned long long>(shaderVariant),
        static_cast<unsigned long long>(stats.vertexShaderLast),
        static_cast<unsigned long long>(stats.pixelShaderLast),
        static_cast<unsigned long long>(vertexConstantsHash),
        static_cast<unsigned long long>(pixelConstantsHash),
        static_cast<unsigned long long>(uniformPayloadHash),
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

  void recordIndexedCacheOptCandidate(IndexReuseMeasure original,
                                      IndexReuseMeasure candidate,
                                      u64 bytes) {
    if (!enabled) {
      return;
    }
    if (!original.available || !candidate.available) {
      ++stats.indexedCacheOptCandidateSkipped;
      return;
    }
    ++stats.indexedCacheOptCandidateDraws;
    stats.indexedCacheOptCandidateBytes += bytes;
    stats.indexedCacheOptCandidateOriginalMiss16 += original.cacheMiss16;
    stats.indexedCacheOptCandidateOriginalMiss32 += original.cacheMiss32;
    stats.indexedCacheOptCandidateOriginalMiss64 += original.cacheMiss64;
    stats.indexedCacheOptCandidateMiss16 += candidate.cacheMiss16;
    stats.indexedCacheOptCandidateMiss32 += candidate.cacheMiss32;
    stats.indexedCacheOptCandidateMiss64 += candidate.cacheMiss64;
  }

  void recordIndexedCacheOptCandidateGate(bool passed,
                                          u64 primitiveCount,
                                          bool opaqueDepth,
                                          bool screenBlend) {
    if (!enabled) {
      return;
    }
    if (passed) {
      ++stats.indexedCacheOptCandidateGatePass;
    } else {
      ++stats.indexedCacheOptCandidateGateFail;
    }
    if (opaqueDepth) {
      ++stats.indexedCacheOptCandidateOpaqueDepthDraws;
    }
    if (screenBlend) {
      ++stats.indexedCacheOptCandidateScreenBlendDraws;
    }
    if (primitiveCount < 64) {
      ++stats.indexedCacheOptCandidatePrimitiveBucket1_63;
    } else if (primitiveCount < 256) {
      ++stats.indexedCacheOptCandidatePrimitiveBucket64_255;
    } else if (primitiveCount < 1024) {
      ++stats.indexedCacheOptCandidatePrimitiveBucket256_1023;
    } else if (primitiveCount < 4096) {
      ++stats.indexedCacheOptCandidatePrimitiveBucket1024_4095;
    } else {
      ++stats.indexedCacheOptCandidatePrimitiveBucket4096Plus;
    }
  }

  void recordReorderedIndexCacheLookup(bool hit,
                                       bool rejected,
                                       bool created,
                                       u64 createdBytes) {
    if (!enabled) {
      return;
    }
    ++stats.reorderedIndexCacheLookups;
    if (hit) {
      ++stats.reorderedIndexCacheHits;
    } else if (rejected) {
      ++stats.reorderedIndexCacheRejectedHits;
    } else {
      ++stats.reorderedIndexCacheMisses;
    }
    if (created) {
      ++stats.reorderedIndexCacheCreated;
      stats.reorderedIndexCacheCreatedBytes += createdBytes;
    }
  }

  void recordTileFfpCoverage(const pipeline::TileFfpSelection& eligibility,
                             bool routedTile,
                             u32 primitiveCount,
                             u64 vertexCount) {
    if (!enabled) {
      return;
    }
    auto addDraw = [&](std::uint64_t& draws,
                       std::uint64_t& primitives,
                       std::uint64_t& vertices) {
      ++draws;
      primitives += primitiveCount;
      vertices += vertexCount;
    };
    auto addDrawNoVertices = [&](std::uint64_t& draws,
                                 std::uint64_t& primitives) {
      ++draws;
      primitives += primitiveCount;
    };

    if (routedTile) {
      addDraw(stats.tileFfpRoutedTileDraws,
              stats.tileFfpRoutedTilePrimitives,
              stats.tileFfpRoutedTileVertices);
    } else {
      addDraw(stats.tileFfpRoutedPortableDraws,
              stats.tileFfpRoutedPortablePrimitives,
              stats.tileFfpRoutedPortableVertices);
    }

    if (eligibility.decision == pipeline::TileFfpDecision::Tile) {
      addDraw(stats.tileFfpEligibleDraws,
              stats.tileFfpEligiblePrimitives,
              stats.tileFfpEligibleVertices);
      return;
    }

    switch (eligibility.reason) {
      case pipeline::TileFfpFallbackReason::GpuFamily:
        addDrawNoVertices(stats.tileFfpFallbackGpuFamilyDraws,
                          stats.tileFfpFallbackGpuFamilyPrimitives);
        break;
      case pipeline::TileFfpFallbackReason::NotFfp:
        addDrawNoVertices(stats.tileFfpFallbackNotFfpDraws,
                          stats.tileFfpFallbackNotFfpPrimitives);
        break;
      case pipeline::TileFfpFallbackReason::Precision:
        addDrawNoVertices(stats.tileFfpFallbackPrecisionDraws,
                          stats.tileFfpFallbackPrecisionPrimitives);
        break;
      case pipeline::TileFfpFallbackReason::UnsupportedState:
        addDrawNoVertices(stats.tileFfpFallbackUnsupportedStateDraws,
                          stats.tileFfpFallbackUnsupportedStatePrimitives);
        break;
      case pipeline::TileFfpFallbackReason::None:
        break;
    }
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
                       const core::FlatRenderStateSet& renderStates,
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
      if (textureMask != 0) {
        ++stats.alphaBlendTexturedDraws;
        stats.alphaBlendTexturedPrimitives += primitiveCount;
        stats.alphaBlendTexturedVertices += vertexCount;
      }
      if (primitiveCount <= 63u) {
        ++stats.alphaBlendSmallDraws;
        stats.alphaBlendSmallPrimitives += primitiveCount;
        stats.alphaBlendSmallVertices += vertexCount;
      }
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
    const auto colorWrite =
        core::flatStateOr(renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
    const auto addRouteClass =
        [&](std::uint64_t& draws, std::uint64_t& primitives,
            std::uint64_t& vertices) {
          ++draws;
          primitives += primitiveCount;
          vertices += vertexCount;
        };
    if (depthWrite && colorWrite == 0u && !alphaBlendEnabled && !alphaTestEnabled) {
      addRouteClass(stats.routeDepthOnlyDraws,
                    stats.routeDepthOnlyPrimitives,
                    stats.routeDepthOnlyVertices);
    } else if (textureMask != 0) {
      addRouteClass(stats.routeProgrammableTexturedDraws,
                    stats.routeProgrammableTexturedPrimitives,
                    stats.routeProgrammableTexturedVertices);
    } else {
      addRouteClass(stats.routeProgrammableColorDraws,
                    stats.routeProgrammableColorPrimitives,
                    stats.routeProgrammableColorVertices);
    }
    if (alphaBlendEnabled) {
      stats.routeAlphaBlendPrimitives += primitiveCount;
    }
    if (alphaTestEnabled) {
      stats.routeAlphaTestPrimitives += primitiveCount;
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

  void recordBlendState(const core::FlatRenderStateSet& renderStates) {
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

    const bool rgbAdd = blendOp == static_cast<u32>(core::BlendOp::Add);
    if (blendEnable != 0u && rgbAdd) {
      if (srcBlend == static_cast<u32>(core::BlendFactor::InvDestColor) &&
          dstBlend == static_cast<u32>(core::BlendFactor::One)) {
        ++stats.blendScreenDraws;
      }
      if (srcBlend == static_cast<u32>(core::BlendFactor::One) &&
          dstBlend == static_cast<u32>(core::BlendFactor::One)) {
        ++stats.blendAdditiveDraws;
      }
      if (srcBlend == static_cast<u32>(core::BlendFactor::SrcAlpha) &&
          dstBlend == static_cast<u32>(core::BlendFactor::InvSrcAlpha)) {
        ++stats.blendAlphaCompositeDraws;
      }
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
                                    bool argbufDirectCbufMode,
                                    bool samplerLodBias,
                                    u32 fetch4SamplerMask,
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
          entry.argbufDirectCbufMode == argbufDirectCbufMode &&
          entry.samplerLodBias == samplerLodBias &&
          entry.fetch4SamplerMask == fetch4SamplerMask &&
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
                                     bool argbufDirectCbufMode,
                                     bool samplerLodBias,
                                     u32 fetch4SamplerMask,
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
        .argbufDirectCbufMode = argbufDirectCbufMode,
        .samplerLodBias = samplerLodBias,
        .fetch4SamplerMask = fetch4SamplerMask,
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
                  offsetof(FfpVsConsts, ffpTextureTransforms) -
                      offsetof(FfpVsConsts, ffpBlendWorldViewProj),
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
      case TransientVertexSource::StagedStream:
        stats.transientVertexStagedStreamBytes += bytes;
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
      case TransientIndexSource::StagedIb:
        stats.transientIndexStagedIbBytes += bytes;
        break;
    }
  }
};


bool traceEncodeProgressForSeq(u64 seqId);

void emitEncodeProgressDrawStage(u64 seqId,
                                 std::uint32_t commandIndex,
                                 u64 commandDrawIndex,
                                 u64 commandDrawCount,
                                 const char* stage);

std::optional<VisibilityScoutPass> makeVisibilityScoutPass(
    WMT::Device device,
    u64 seqId,
    u64 encoderIndex);

std::optional<std::uint32_t> beginVisibilityScoutDraw(
    VisibilityScoutPass* pass,
    WMT::RenderCommandEncoder& encoder,
    VisibilityScoutDrawRecord record);

void endVisibilityScoutDraw(VisibilityScoutPass* pass,
                            WMT::RenderCommandEncoder& encoder,
                            std::optional<std::uint32_t> resultIndex);

bool colorAttachmentDumpAfterDrawWantsSplit(
    const ActiveColorAttachmentDump& active,
    core::FlatDrawStateView drawState,
    u64 encoderDrawIndex,
    u64 commandIndex);

bool drawTextureDumpPassMatches(u64 seq, u64 enc);

void maybeCollectDrawTextureDump(
    std::vector<ActiveDrawTextureDump>& activeDumps,
    const resources::Pool& pool,
    core::FlatDrawStateView drawState,
    u64 seq,
    u64 enc);

bool colorAttachmentAliasesTracedTexture(resources::Pool& pool,
                                         const core::FlatDrawStateRecord& hot,
                                         std::size_t* attachmentIndex = nullptr);

void traceRenderTargetWriteForTexture(resources::Pool& pool,
                                      const core::FlatDrawStateRecord& hot,
                                      u64 seqId,
                                      u64 drawOrdinal);

bool suppressRecordedMetalCalls(const EncodeContext& ctx) noexcept;

bool suppressBaseStateLookup(const EncodeContext& ctx) noexcept;

void recordedSetRenderPipelineState(EncodeContext& ctx,
                                    WMT::RenderCommandEncoder& encoder,
                                    WMT::RenderPipelineState pipeline);

void recordedSetDepthStencilState(EncodeContext& ctx,
                                  WMT::RenderCommandEncoder& encoder,
                                  WMT::DepthStencilState depthStencil,
                                  std::uint8_t stencilRef = 0);

void recordedSetBlendColorAndStencilRef(EncodeContext& ctx,
                                        WMT::RenderCommandEncoder& encoder,
                                        float red,
                                        float green,
                                        float blue,
                                        float alpha,
                                        std::uint8_t stencilRef);

void recordedSetViewport(EncodeContext& ctx,
                         WMT::RenderCommandEncoder& encoder,
                         WMTViewport viewport);

void recordedSetScissorRect(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            WMTScissorRect rect);

void recordedSetRasterizerState(EncodeContext& ctx,
                                WMT::RenderCommandEncoder& encoder,
                                WMTTriangleFillMode fillMode,
                                WMTCullMode cullMode,
                                WMTDepthClipMode depthClipMode,
                                WMTWinding winding,
                                float depthBias,
                                float slopeScale,
                                float depthBiasClamp);

void recordedSetFragmentTexture(EncodeContext& ctx,
                                WMT::RenderCommandEncoder& encoder,
                                WMT::Texture texture,
                                std::uint8_t index);

void recordedSetFragmentSamplerState(EncodeContext& ctx,
                                     WMT::RenderCommandEncoder& encoder,
                                     WMT::SamplerState sampler,
                                     std::uint8_t index);

void recordedSetVertexTexture(EncodeContext& ctx,
                              WMT::RenderCommandEncoder& encoder,
                              WMT::Texture texture,
                              std::uint8_t index);

void recordedSetVertexSamplerState(EncodeContext& ctx,
                                   WMT::RenderCommandEncoder& encoder,
                                   WMT::SamplerState sampler,
                                   std::uint8_t index);

void recordedSetVertexBuffer(EncodeContext& ctx,
                             WMT::RenderCommandEncoder& encoder,
                             WMT::Buffer buffer,
                             u64 offset,
                             std::uint8_t index);

void recordedSetVertexBytes(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            const void* bytes,
                            u64 length,
                            std::uint8_t index);

void recordedSetFragmentBytes(EncodeContext& ctx,
                              WMT::RenderCommandEncoder& encoder,
                              const void* bytes,
                              u64 length,
                              std::uint8_t index);

void recordedDrawPrimitives(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            WMTPrimitiveType primitiveType,
                            u64 vertexStart,
                            u64 vertexCount,
                            u32 instanceCount,
                            u32 baseInstance);

void recordedDrawIndexedPrimitives(EncodeContext& ctx,
                                   WMT::RenderCommandEncoder& encoder,
                                   WMTPrimitiveType primitiveType,
                                   WMTIndexType indexType,
                                   u64 indexCount,
                                   WMT::Buffer indexBuffer,
                                   u64 indexBufferOffset,
                                   u32 instanceCount,
                                   i32 baseVertex,
                                   u32 baseInstance);

void countTextureBind();

void countSamplerBind();

void countTextureBindSkipped();

void countSamplerBindSkipped();

void countVertexBufferBind();

void countIndexBufferBind();

void countUniformBufferBinds(std::uint32_t count);

void countPipelineBind();

void countDepthStateBind();

void countViewportBind();

void countScissorBind();

void countRasterizerBind();

void countVertexBufferBindSkipped();

void countPipelineBindSkipped();

void countDepthStateBindSkipped();

bool x8ShaderAlphaFillEnabledForDiagnostics();

bool effectDrawTraceEnabled();

bool effectDrawTraceGeometryEnabled();

// True when render-pass diagnostics require draw-order observation or a
// post-pass readback sidecar. Parallel children must fail closed while any of
// these process-immutable diagnostic modes is enabled.
bool parallelRenderPassSidecarObservationEnabled();

void traceEffectDraw(const ActiveEncoderBreakdown* encoderBreakdown,
                     const core::FlatDrawStateRecord& hot,
                     const resources::Pool& pool,
                     u64 seqId,
                     u64 drawOrdinal,
                     std::uint32_t commandIndex,
                     u64 commandDrawIndex,
                     u64 commandDrawCount,
                     core::PrimitiveType primitiveType,
                     u32 primitiveCount,
                     u64 vertexCount,
                     bool indexedDraw,
                     bool fixedFunctionPath,
                     bool preTransformed,
                     u64 vertexShaderHash,
                     u64 pixelShaderHash);

bool renderEncoderGpuTimeEnabled();

bool encoderBreakdownCbufContentEnabled();

bool textureSamplerDirectSplitPerfEnabled();

bool streamBindPhaseSplitPerfEnabled();

bool bindingPacketPlanSplitPerfEnabled();

bool drawIssueSplitPerfEnabled();

bool argbufCbufProbeSplitPerfEnabled();

bool argbufReopenSplitPerfEnabled();

bool argbufCbufDirtyIdentityPerfEnabled();

bool argbufPayloadDeltaPerfEnabled();

bool argbufPayloadDeltaSourcePerfEnabled();

const char* metalCommandKindName(core::MetalCommandKind kind);

std::size_t drawTextureDumpReserveCapacity();

void selectActiveDepthAttachmentDump(
    const resources::Pool& pool,
    const core::BackendLimits& limits,
    core::FlatDrawStateView drawState,
    bool renderEncoderActive,
    u64 seqId,
    u64 encoderIndex,
    ActiveDepthAttachmentDump& activeDump);

void selectActiveColorAttachmentDump(
    const resources::Pool& pool,
    const core::BackendLimits& limits,
    core::FlatDrawStateView drawState,
    bool renderEncoderActive,
    u64 seqId,
    u64 encoderIndex,
    ActiveColorAttachmentDump& activeDump);

void recordArgbufCbufUploadForBreakdown(void* userdata,
                                        u32 argbufIndex,
                                        const void* data,
                                        u64 bytes,
                                        u64 hostStructBytes);

PerfCounterFn argbufCbufCachedRepointCpuRecorder(u32 argbufIndex) noexcept;

PerfCounterFn argbufCbufContentProbeCpuRecorder(u32 argbufIndex) noexcept;

void countArgbufCbufCachedRepointStage(u32 argbufIndex, u64 bytes) noexcept;

bool x8ShaderAlphaFillEnabledForDiagnostics();

void traceEffectIndexedGeometry(const ActiveEncoderBreakdown* encoderBreakdown,
                                core::FlatDrawStateView drawState,
                                const resources::Pool& pool,
                                std::span<const u8> indexBytes,
                                std::span<const u8> vertexBytes,
                                IndexType indexType,
                                u32 startIndex,
                                u64 indexCount,
                                i32 baseVertexIndex,
                                u64 stream0Offset,
                                u64 stream0Stride,
                                u64 stream0Handle,
                                u64 indexBufferHandle,
                                u64 seqId,
                                u64 drawOrdinal,
                                std::uint32_t commandIndex,
                                u64 commandDrawIndex,
                                u64 commandDrawCount,
                                core::PrimitiveType primitiveType,
                                u32 primitiveCount,
                                bool fixedFunctionPath,
                                bool preTransformed,
                                u64 vertexShaderHash,
                                u64 pixelShaderHash);

VisibilityScoutDrawRecord makeVisibilityScoutDrawRecord(
    const VisibilityScoutPass& pass,
    core::FlatDrawStateView drawState,
    const core::ViewportScissor& viewport,
    WMTPrimitiveType primitiveType,
    const ParamView& pv,
    u64 drawOrdinal,
    std::uint32_t commandIndex,
    u64 submittedPrimitiveCount,
    u64 submittedElementCount,
    bool indexed,
    bool expandedIndexed,
    std::uint32_t splitChunk,
    WMTCullMode cullMode,
    WMTTriangleFillMode fillMode);

void enqueueVisibilityScoutCompletion(
    VisibilityScoutPass& pass,
    std::vector<std::function<void()>>& completionCallbacks);

void maybeEncodeDepthAttachmentDump(
    WMT::CommandBuffer& commandBuffer,
    WMT::Reference<WMT::Device> device,
    const ActiveDepthAttachmentDump& active,
    std::vector<std::function<void()>>& completionCallbacks);

void maybeEncodeColorAttachmentDump(
    WMT::CommandBuffer& commandBuffer,
    WMT::Reference<WMT::Device> device,
    const ActiveColorAttachmentDump& active,
    std::vector<std::function<void()>>& completionCallbacks);

void maybeEncodeDrawTextureDumps(
    WMT::CommandBuffer& commandBuffer,
    WMT::Reference<WMT::Device> device,
    std::span<const ActiveDrawTextureDump> activeDumps,
    std::vector<std::function<void()>>& completionCallbacks);

}  // namespace dxmt9::encoders
