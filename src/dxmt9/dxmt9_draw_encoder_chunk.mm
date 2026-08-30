#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9_capture.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_encode_partition.hpp"
#include "dxmt9_draw_encoder_draw_internal.hpp"
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
#include "dxmt9_parallel_render_pass.hpp"
#include "dxmt9_parallel_render_pass_metal.hpp"
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


using encode_session::ArgbufPayloadDeltaComponentKey;
using encode_session::ArgbufPayloadDeltaKey;

namespace {

core::metalqueue::ReplayCategory replayCategoryFor(
    core::MetalCommandKind kind) noexcept {
  using Kind = core::MetalCommandKind;
  switch (kind) {
  case Kind::DrawRun:
  case Kind::Clear:
  case Kind::ColorFill:
    return core::metalqueue::ReplayCategory::Draw;
  case Kind::Present:
    return core::metalqueue::ReplayCategory::Present;
  case Kind::SurfaceCopy:
  case Kind::StretchRect:
  case Kind::Readback:
  case Kind::DepthResolve:
  case Kind::GenerateMipmaps:
    return core::metalqueue::ReplayCategory::Copy;
  }
  return core::metalqueue::ReplayCategory::Draw;
}

struct DisabledReplayObserver {
  void observe(std::uint32_t, const core::SourceCommandView&) const noexcept {}
};

struct EnabledReplayObserver {
  core::metalqueue::ReplayObserverSink sink{};
  core::CpuReadyTape::SourceRef source{};
  std::uint64_t seqId = 0;
  std::vector<core::ChunkHandleEntry> handles;
  std::vector<std::uint32_t> observedCommandOrdinals;

  void observe(std::uint32_t commandIndex,
               const core::SourceCommandView& command) {
    DXMT_ASSERT(sink.fn != nullptr);
    if (std::find(observedCommandOrdinals.begin(),
                  observedCommandOrdinals.end(), commandIndex) !=
        observedCommandOrdinals.end()) {
      return;
    }
    observedCommandOrdinals.push_back(commandIndex);
    handles.clear();
    core::visitSourceCommandResources(
        command, [&](const core::SourceCommandResourceRef& resource) {
          const auto duplicate = std::find_if(
              handles.begin(), handles.end(),
              [&](const core::ChunkHandleEntry& existing) {
                return existing.kind == resource.entry.kind &&
                       existing.handle == resource.entry.handle;
              });
          if (duplicate == handles.end()) {
            handles.push_back(resource.entry);
          }
        });

    const auto kind = command.kind();
    sink.fn(sink.context,
            core::metalqueue::ReplayObservation{
                .source = source,
                .seqId = seqId,
                .commandOrdinal = commandIndex,
                .commandKind = kind,
                .category = replayCategoryFor(kind),
                .barrier = kind != core::MetalCommandKind::DrawRun,
                .readback = kind == core::MetalCommandKind::Readback,
                .resourceHandles = handles,
            });
  }
};

[[noreturn]] void abortEncodePartitionInvariant(const char* reason) {
  std::fprintf(
      stderr,
      "[dxmt9-encode] fatal: encode partition execution invariant failed "
      "(%s)\n",
      reason ? reason : "unknown");
  std::abort();
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


bool stageStreamIbProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  if (!debug::probeStageStreamIb()) {
    return false;
  }
  return debug::renderEncoderSelectionMatches(
      debug::probeStageStreamIbRow(), debug::probeStageStreamIbRows(),
      encoderBreakdown != nullptr,
      encoderBreakdown ? encoderBreakdown->stats.seqId : 0,
      encoderBreakdown ? encoderBreakdown->stats.encoderIndex : 0);
}



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

bool resolveParallelPassResourceIdentity(
    const void* context, std::uint64_t raw,
    std::uint64_t& canonical) noexcept {
  const auto* pool = static_cast<const resources::Pool*>(context);
  if (!pool || raw == 0u) {
    return false;
  }
  const auto* surface = pool->findSurface(raw);
  canonical = surface && surface->aliasTexture
      ? surface->aliasTexture.value
      : raw;
  return canonical != 0u;
}

core::RenderRoute resolveParallelPassRenderRoute(
    const void* context, core::FlatDrawStateView state) noexcept {
  const auto* pool = static_cast<const resources::Pool*>(context);
  if (!pool) {
    return core::RenderRoute::Unknown;
  }
  const auto selection = pipeline::selectTileFfpForPass(
      state, pool->supportsApple3());
  return selection.decision == pipeline::TileFfpDecision::Tile
      ? core::RenderRoute::Tile
      : core::RenderRoute::Portable;
}

// Owner-issued snapshot authority for the policy proof core. The producer's
// own bounded batch is the only owner of a sealed pass observation, so the
// certificate re-reads the pass from it by exact source/sequence/interval
// identity instead of trusting the value the caller carried through planning.
// An ambiguous or missing owner entry fails closed.
bool resolveParallelPassSnapshotAuthority(
    const void* context, const core::CpuReadyTape::SourceRef& source,
    std::uint64_t seqId, std::uint32_t replayOrdinalBegin,
    std::uint32_t replayOrdinalEnd,
    encoders::SealedParallelPassSnapshot& authoritative) noexcept {
  const auto* batch =
      static_cast<const encoders::SealedParallelPassSnapshotBatch*>(context);
  if (!batch) {
    return false;
  }
  const encoders::SealedParallelPassSnapshot* found = nullptr;
  for (const auto& pass : batch->view()) {
    if (pass.source != source || pass.seqId != seqId ||
        pass.replayOrdinalBegin != replayOrdinalBegin ||
        pass.replayOrdinalEnd != replayOrdinalEnd) {
      continue;
    }
    if (found) {
      return false;
    }
    found = &pass;
  }
  if (!found) {
    return false;
  }
  authoritative = *found;
  return true;
}

struct ParallelPassCoverageContext {
  const encoders::EncodePartitionReplayStream* stream = nullptr;
  const resources::Pool* pool = nullptr;
};

// Coordinator-issued epoch witness. It returns only the classification facts
// one replay ordinal contributes to the pass-action epoch, re-read from the
// live source under the residency pin. The certificate folds them itself, so
// this callback can never hand back a finished epoch to be trusted.
bool resolveParallelPassActionEpochFact(
    const void* context, const core::CpuReadyTape::SourceRef& source,
    std::uint64_t seqId, std::uint32_t replayOrdinal,
    encoders::ParallelPassActionEpochFact& fact) noexcept {
  const auto* stream =
      static_cast<const encoders::EncodePartitionReplayStream*>(context);
  if (!stream || stream->source.source != source ||
      stream->source.seqId != seqId || seqId == 0u) {
    return false;
  }
  return encoders::readParallelPassActionEpochFact(*stream, replayOrdinal,
                                                   fact);
}

// Exact per-child coverage resolved from the live source while the coordinator
// still holds the residency pin. It re-reads every command the child owns and
// canonicalizes its resources through the same proof owner the producer used,
// so the certificate compares owner-resolved facts rather than replayed values.
bool resolveParallelPassCoverage(
    const void* context,
    const encoders::SealedParallelPassSnapshot& snapshot,
    const encoders::ParallelPassChildPlan& child,
    encoders::ParallelPassResolvedCoverage& coverage) noexcept {
  const auto* state = static_cast<const ParallelPassCoverageContext*>(context);
  if (!state || !state->stream || !state->pool) {
    return false;
  }
  coverage = {};
  coverage.reads.flags = core::ExactResourceSetComplete |
      core::ExactResourceSetCanonicalized;
  coverage.writes.flags = core::ExactResourceSetComplete |
      core::ExactResourceSetCanonicalized;
  coverage.attachments = snapshot.attachments;
  coverage.passActionEpoch = snapshot.passActionEpoch;
  const std::uint32_t commandCount = snapshot.childrenCoverCompleteCommands
      ? child.replayOrdinalCount
      : 1u;
  if (commandCount == 0u) {
    return false;
  }
  // Streaming: a child owning more commands than any fixed row array is a
  // normal input. Each row is checked against the previous boundary at append
  // time and then discarded.
  coverage.commands.open(child.replayOrdinalBegin,
                         snapshot.childrenCoverCompleteCommands);
  for (std::uint32_t offset = 0u; offset < commandCount; ++offset) {
    std::uint32_t commandIndex = 0u;
    if (!state->stream->commandIndexAt(
            static_cast<std::size_t>(child.replayOrdinalBegin) + offset,
            commandIndex)) {
      return false;
    }
    const auto source = state->stream->source.payload.commandAt(commandIndex);
    if (source.kind() != core::MetalCommandKind::DrawRun ||
        !source.command.drawState.hot ||
        !source.command.drawState.shaderLayout ||
        !source.command.drawRunRecord || source.command.drawParams.empty() ||
        source.command.drawParams.size() > UINT32_MAX ||
        core::makeRenderAttachmentKey(*source.command.drawState.hot) !=
            snapshot.attachments) {
      return false;
    }
    const auto route = resolveParallelPassRenderRoute(
        state->pool, source.command.drawState);
    if (route == core::RenderRoute::Unknown ||
        (offset != 0u && route != coverage.route)) {
      return false;
    }
    coverage.route = route;
    const auto rawReads = core::makeDrawEntryReadSet(source.command.drawState);
    const auto rawWrites = core::makeRenderAttachmentWriteSet(
        *source.command.drawState.hot);
    if (!rawReads.complete() || !rawWrites.complete()) {
      return false;
    }
    for (std::uint32_t i = 0u; i < rawReads.count; ++i) {
      std::uint64_t canonical = 0u;
      if (!resolveParallelPassResourceIdentity(
              state->pool, rawReads.handles[i], canonical) ||
          canonical == 0u || !coverage.reads.add(canonical)) {
        return false;
      }
    }
    for (std::uint32_t i = 0u; i < rawWrites.count; ++i) {
      std::uint64_t canonical = 0u;
      if (!resolveParallelPassResourceIdentity(
              state->pool, rawWrites.handles[i], canonical) ||
          canonical == 0u || !coverage.writes.add(canonical)) {
        return false;
      }
    }
    if (!coverage.commands.append({
            .replayOrdinal = child.replayOrdinalBegin + offset,
            .commandIndex = commandIndex,
            .drawParamBegin = source.command.drawRunRecord->firstParam,
            .drawParamCount =
                static_cast<std::uint32_t>(source.command.drawParams.size()),
        })) {
      return false;
    }
  }
  coverage.drawCount = snapshot.childrenCoverCompleteCommands
      ? coverage.commands.drawTotal()
      : child.range.drawEntryCount;
  return coverage.commands.valid() &&
      coverage.commands.commandCount() == commandCount &&
      coverage.drawCount != 0u && coverage.reads.complete() &&
      coverage.writes.complete();
}

// A parallel pass closes at the same coordinator command the serial encoder
// would have flushed on, so the encoder-split reason must stay the one the
// serial lane records for that kind.
perf::EncoderSplitReason parallelPassCloseReason(
    const encoders::SealedParallelPassSnapshot& pass) noexcept {
  if (pass.sealedAtSourceEnd || !pass.sealingCommand.valid) {
    return perf::EncoderSplitReason::Final;
  }
  switch (pass.sealingCommand.kind) {
  case core::MetalCommandKind::Present:
    return perf::EncoderSplitReason::Present;
  case core::MetalCommandKind::Clear:
    return perf::EncoderSplitReason::ClearBarrier;
  case core::MetalCommandKind::SurfaceCopy:
    return perf::EncoderSplitReason::SurfaceCopy;
  case core::MetalCommandKind::StretchRect:
  case core::MetalCommandKind::DepthResolve:
  case core::MetalCommandKind::GenerateMipmaps:
    return perf::EncoderSplitReason::StretchRect;
  case core::MetalCommandKind::Readback:
    return perf::EncoderSplitReason::Readback;
  case core::MetalCommandKind::ColorFill:
    return perf::EncoderSplitReason::ColorFill;
  case core::MetalCommandKind::DrawRun:
    break;
  }
  return perf::EncoderSplitReason::RenderTargetChange;
}

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

static std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunkImpl(
    EncodeContext& ctx,
    std::size_t slotIndex,
    core::SourcePayloadView payload,
    std::uint64_t sourceSeqId,
    EncodeChunkOptions options,
    std::span<const core::metalqueue::ResolvedPublishedSource>
        sessionLookaheadSources) {
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
  std::span<const EncodePartitionRangeSnapshot> partitionRanges =
      options.partitionRanges;
  thread_local ProductionEncodePartitionPlanStorage productionPlanStorage;
  const bool parallelPartitionRequested =
      options.partitionExecutionMode ==
      render::PartitionExecutionMode::ExplicitParallel;
  // DXMT9_PARALLEL_PASS_DRAW_QUANTUM — diagnostic/tuning env knob for the
  // parallel-pass economics floor (R-BACK-2.68..2.75). Read once and shared by
  // both the shadow snapshot producer's child subdivision and the economics
  // classifier below so the eligibility quantum and the imbalance bound stay
  // consistent with each other. The serial partition planner
  // (planProductionEncodePartitions, dxmt9_encode_partition.hpp/.cpp) never
  // reads this value and keeps using kProductionPartitionDrawThreshold.
  const std::uint32_t parallelPassDrawQuantum =
      parallelPartitionRequested
          ? encoders::resolveParallelPassDrawQuantumFromEnv()
          : encoders::kProductionPartitionDrawThreshold;
  // DXMT9_PARALLEL_PASS_IMBALANCE_BOUND — decouples the economics
  // classifier's UnbalancedChild bound from parallelPassDrawQuantum above.
  // It never affects the shadow snapshot producer's child
  // subdivision/eligibility (SealedParallelPassSnapshotInput::drawQuantum),
  // which keeps using parallelPassDrawQuantum only. Resolved once, coupled
  // to parallelPassDrawQuantum when unset (see the resolver doc-comment in
  // dxmt9_parallel_render_pass.hpp), and guarded by ExplicitParallel so
  // identity/serial modes never touch the env cache.
  const std::uint32_t parallelPassImbalanceBound = parallelPartitionRequested
      ? encoders::resolveParallelPassImbalanceBoundFromEnv(
            parallelPassDrawQuantum)
      : parallelPassDrawQuantum;
  if (partitionRanges.empty() &&
      (options.partitionExecutionMode ==
           render::PartitionExecutionMode::ExplicitSerial ||
       parallelPartitionRequested)) {
    const bool measurePlanner = perf::enabled();
    std::chrono::steady_clock::time_point plannerStarted{};
    if (measurePlanner) {
      plannerStarted = std::chrono::steady_clock::now();
    }
    const auto plan = planProductionEncodePartitions(
        partitionReplayStream, productionPlanStorage);
    std::uint64_t plannerNanoseconds = 0u;
    if (measurePlanner) {
      plannerNanoseconds = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - plannerStarted)
              .count());
      perf::countEncodePartitionPlan(
          plan.explicitPlan, plan.rangeCount, plan.drawRangeCount,
          plan.plannedDrawCount, plan.subdividedDrawRunCount,
          plan.mergePreservedIdentityCount,
          plan.fallback, plannerNanoseconds);
    }
    if (plan.explicitPlan) {
      partitionRanges = productionPlanStorage.view();
    }
  } else if (partitionRanges.empty() && perf::enabled()) {
    perf::countEncodePartitionPlan(
        false, 0u, 0u, 0u, 0u, 0u,
        ProductionPartitionFallbackReason::None, 0u);
  }
  bool useExplicitPartitionPlan = false;
  if (!partitionRanges.empty()) {
    const auto validation = validateEncodePartitionRanges(
        partitionRanges, partitionReplayStream);
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
  bool initializerWaitEncoded = false;
  if (firstTransactionFragment) {
    encode_session::initializeGpuSamplingStorage(
        session, WMT::Device{ctx.device.handle},
        sessionLookaheadSources.empty()
            ? replayRange.commandCount()
            : encode_session::gpuSamplingCommandCount(
                  payload, sourceSeqId, replayRange.commandCount(),
                  sessionLookaheadSources));
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
  auto makeDrawBindingPayloadIdentity =
      [](core::FlatDrawStateView drawState) {
        const auto& payload = drawState.uniformPayload();
        return uniform::DrawBindingPayloadIdentity{
            .vertexConstants = drawStateVertexCbufSourceHash(drawState),
            .pixelConstants = drawStatePixelCbufSourceHash(drawState),
            .fixedFunction = payload.fixedPayloadHash,
        };
      };
  auto makeDrawBindingPayloadCounts =
      [](const core::DrawUniformPayload& payload) {
        return uniform::DirectCbufPayloadCounts{
            .vertexFloat = payload.vertexFloatConstantCount,
            .vertexInt = payload.vertexIntConstantCount,
            .vertexBool = payload.vertexBoolConstantCount,
            .pixelFloat = payload.pixelFloatConstantCount,
            .pixelInt = payload.pixelIntConstantCount,
            .pixelBool = payload.pixelBoolConstantCount,
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

  auto buildRenderPassStoreProofLookahead =
      [&](std::size_t lookaheadStartIndex) {
    return makeRenderPassStoreProofLookaheadPlan({
        .slotIndex = slotIndex,
        .payload = payload,
        .sourceSeqId = sourceSeqId,
        .replayRange = replayRange,
        .sessionSource = options.sessionSource
            ? &*options.sessionSource
            : nullptr,
        .partitionSource = options.partitionSource,
        .retainedSources = sessionLookaheadSources,
        .replayCommandPlanActive = options.replayCommandPlanActive,
        .replayCommandOrder = options.replayCommandOrder,
        .replayOrdinalByCommandIndex = replayOrdinalByCommandIndex,
        .lookaheadStartIndex = lookaheadStartIndex,
    });
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
    const auto storeProofLookahead =
        buildRenderPassStoreProofLookahead(lookaheadStartIndex);
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
    bindingState.lastDrawBindingPayloadIdentity.reset();
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
      initializerWaitEncoded = true;
      commandBufferHasWork = true;
    }
  }

  thread_local SealedParallelPassSnapshotBatch parallelPassShadows;
  parallelPassShadows.reset();
  // Coordinator-owned source-wide starting epoch. It seeds the producer's fold
  // and, independently, the certificate's re-derivation, so the seed is never
  // taken from the snapshot under test. It is the same on every source, carried
  // or not: whether this source starts a pass is carried by `sourceStartsPass`
  // below, and a session-dependent seed of zero used to make the certificate's
  // re-derivation structurally impossible for every pass in a carried source.
  // See `kParallelPassSeedActionEpoch`.
  const std::uint64_t parallelPassSeedActionEpoch =
      kParallelPassSeedActionEpoch;
  if (parallelPartitionRequested) {
    // SourcePayloadView cannot represent Query, UpdateTexture, or an ordered
    // control: those dispositions are resolved by the coordinator before this
    // call. Capture and initializer facts become stable only after the source
    // preamble above. This is therefore the last pre-command-effect seam at
    // which a complete immutable coordinator proof can be published.
    const auto coordinatorProof = makeParallelPassCoordinatorProofSnapshot(
        ParallelPassCoordinatorProofSnapshotInput{
            .firstPassActionEpoch = parallelPassSeedActionEpoch,
            .queryAbsent = true,
            .updateTextureAbsent = true,
            .captureInactive = !ctx.queue.metalCaptureEnabled() &&
                !captureAlreadyStartedAtChunkBegin &&
                !earlyCaptureRequest.has_value() &&
                !diagnosticsState.metalCaptureRequest.has_value(),
            .initializerIndependent = !initializerWaitEncoded,
            .orderedControlAbsent = true,
            .sidecarObservationAbsent =
                !parallelRenderPassSidecarObservationEnabled() &&
                !diagnosticsState.renderEncoderGpuSampleBuffer,
        });
    // Pass-local sealing over the final validated replay order. Coordinator
    // commands never enter a child range and no borrowed view escapes this
    // call. Identity/serial modes never pay this proof cost; parallel mode
    // needs the same snapshots even when perf counters are disabled.
    const auto observation = produceSealedParallelPassSnapshots(
        SealedParallelPassSnapshotInput{
            .stream = &partitionReplayStream,
            .ranges = {},
            .proofs = ParallelPassStaticProofInput{
                .resources = ParallelPassResourceIdentityProof{
                    .context = &ctx.pool,
                    .resolve = resolveParallelPassResourceIdentity,
                },
                .route = ParallelPassRenderRouteProof{
                    .context = &ctx.pool,
                    .resolve = resolveParallelPassRenderRoute,
                },
                .coordinator = coordinatorProof,
            },
            .planValidated = partitionReplayStream.valid,
            .sourceStartsPass = options.session == nullptr,
            .sourceEndsPass = !(options.deferSessionFinalization &&
                                options.session != nullptr),
            .drawQuantum = parallelPassDrawQuantum,
        },
        parallelPassShadows);
    if (perf::enabled()) {
      perf::countParallelPassShadow(observation);
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
      const auto drawBindingPath = activePassUsesArgbufTable
          ? uniform::DrawBindingPath::ArgumentTable
          : uniform::DrawBindingPath::Direct;
      const auto drawBindingAbi = !bindingState.activePassUsesArgbufHybrid
          ? uniform::DrawBindingAbi::Stage1Direct
          : bindingState.activePassUsesArgbufDirectCbuf
              ? uniform::DrawBindingAbi::Stage2DirectCbuf
              : uniform::DrawBindingAbi::Stage2ArgumentTable;
      const auto drawBindingPayloadIdentity =
          makeDrawBindingPayloadIdentity(drawStateView);
      const auto drawBindingTransition =
          uniform::planDrawBindingTransition(
              bindingState.lastDrawBindingPayloadIdentity.has_value(),
              bindingState.lastDrawBindingPayloadIdentity.value_or(
                  uniform::DrawBindingPayloadIdentity{}),
              drawBindingPayloadIdentity, drawBindingAbi, drawBindingPath);
      // TLA+: ParallelDrawBinding / PsoBindingAbiMatchesChildBinding.
      DXMT_ASSERT(drawBindingTransition.psoBindingAbiCompatible);
      if (drawBindingPath == uniform::DrawBindingPath::Direct &&
          !uniform::applyDrawBindingTransition(
              bindingState.uniformDirty, drawBindingTransition,
              makeDrawBindingPayloadCounts(*drawUniformPayload))) {
        abortEncodePartitionInvariant(
            "serial draw PSO binding ABI mismatch");
      }
      const bool argbufVsPayloadSourceChanged =
          drawBindingTransition.constantSourceChange.vertex;
      const bool argbufPsPayloadSourceChanged =
          drawBindingTransition.constantSourceChange.pixel;
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
                     /*tileFfpMode=*/passState.activePassUsesTileFfp,
                     /*argbufHybridMode=*/bindingState.activePassUsesArgbufHybrid,
                     /*argbufResourceArray=*/bindingState.activePassUsesArgbufResourceArray,
                     /*argbufDirectCbufMode=*/bindingState.activePassUsesArgbufDirectCbuf,
                     /*reopenArgbufHybrid=*/reopenArgbuf,
                     DrawNativeShadowView{
                         .uniformDirty = &bindingState.uniformDirty,
                         .textureSampler = &bindingState.textureSamplerShadow,
                         .encoderBreakdown =
                             &diagnosticsState.activeEncoderBreakdown,
                         .argbufCbufCache = &bindingState.argbufCbufCache,
                         .streamIbStagingCache =
                             &bindingState.activeStreamIbStaging,
                         .visibilityScout = diagnosticsState.activeVisibilityScout
                             ? &*diagnosticsState.activeVisibilityScout
                             : nullptr,
                         .renderPsoHandle = renderPsoHandle,
                         .tilePsoHandle = tilePsoHandle,
                         .depthStencilHandle = depthStencilHandle,
                         .commandIndex =
                             commandIndex <=
                                     std::numeric_limits<std::uint32_t>::max()
                                 ? static_cast<std::uint32_t>(commandIndex)
                                 : std::numeric_limits<std::uint32_t>::max(),
                         .commandDrawIndex = static_cast<u64>(absoluteDrawIndex),
                         .commandDrawCount = static_cast<u64>(commandDrawCount),
                         .argbufVsPayloadSourceChanged =
                             argbufVsPayloadSourceChanged,
                         .argbufPsPayloadSourceChanged =
                             argbufPsPayloadSourceChanged,
                         .bindingOverridePrefetchedPsoCompatible =
                             bindingOverridePrefetchedPsoCompatible,
                     })) {
        traceEncodeCommand("drawrun.after-encode-draw-ok", commandIndex,
                           Kind::DrawRun, command);
        bindingState.activeDrawStateKey = drawStateView.hot->key;
        bindingState.activeDrawStateUsesPrefetchedPsoLayout = !overrideNeedsBaseStateBind;
        bindingState.lastArgbufPayloadHash = drawArgbufPayloadHash;
        bindingState.lastArgbufPayloadDeltaKey = drawArgbufPayloadDeltaKey;
        bindingState.lastDrawBindingPayloadIdentity =
            drawBindingTransition.next;
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
                commandIndex,
                absoluteDrawIndex)) {
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

  // Fixed-pass child encoder. Unlike encodeDrawRunCommand this owns no
  // encoder/pass/session transition state: the coordinator has already
  // sealed and opened the logical pass, and each caller supplies a distinct
  // child-local binding shadow. All transient allocations go through the
  // queue's locked ResourceArena; pool/cache access is read-only or already
  // internally synchronized. UP payloads and diagnostic sidecars are rejected
  // before the parent Metal encoder is created.
  auto encodeParallelDrawRunCommand =
      [&](std::size_t commandIndex,
          const core::MetalCommandView& command,
          std::span<const core::DrawParam> drawItems,
          std::size_t commandDrawBegin,
          WMT::RenderCommandEncoder encoder,
          const ParallelPassBindingSnapshot& bindingProof,
          encode_session::BindingState& childBinding) noexcept {
    if (!encoder || !command.drawState.hot ||
        !command.drawState.shaderLayout || !command.drawRunRecord ||
        drawItems.empty()) {
      return false;
    }

    core::DrawUniformPayload commandUniformScratch;
    const auto* commandUniformPayload = core::drawRunUniformPayloadForHandle(
        command, command.drawRunRecord->uniformHandle, commandUniformScratch,
        perf::DrawUniformPayloadMaterializeSite::DrawEncoderCommand);
    if (!commandUniformPayload) {
      return false;
    }
    core::DrawUniformPayloadMaterializeCache childUniformCache;
    auto stateView = command.drawState;
    stateView.uniforms = commandUniformPayload;
    const auto& hot = *stateView.hot;
    const auto payloadArena = core::drawRunPayloadBytes(command);
    const auto commandDrawItems =
        command.drawItems.empty() ? command.drawParams : command.drawItems;
    const std::size_t commandDrawCount = commandDrawItems.size();
    const core::PsoHandle renderPsoHandle =
        command.drawRunRecord->renderPsoHandle;
    const core::PsoHandle tilePsoHandle =
        command.drawRunRecord->tilePsoHandle;
    const core::DepthStencilHandle depthStencilHandle =
        command.drawRunRecord->depthStencilHandle;
    const auto renderPsoKey =
        ctx.cache.drawPipelineKeyForHandle(renderPsoHandle);
    const auto bindingDecision = classifyParallelPassBindingKey({
        .psoPresent = renderPsoKey.has_value(),
        .argbufHybrid = renderPsoKey && renderPsoKey->argbufHybridMode,
        .argbufResourceArray =
            renderPsoKey && renderPsoKey->argbufResourceArray,
        .argbufDirectCbuf =
            renderPsoKey && renderPsoKey->argbufDirectCbufMode,
    });
    if (!bindingDecision.accepted() ||
        bindingDecision.mode != bindingProof.mode) {
      return false;
    }
    const auto bindingMode = bindingProof.mode;
    const auto renderPsoBindingAbi =
        bindingMode == ParallelPassDirectBindingMode::Stage2DirectCbuf
        ? uniform::DrawBindingAbi::Stage2DirectCbuf
        : uniform::DrawBindingAbi::Stage1Direct;
    const bool stage2DirectCbuf =
        bindingMode == ParallelPassDirectBindingMode::Stage2DirectCbuf;
    const bool mergeDraws = debug::optimizeCompatibleIndexedDrawMerge();

    for (std::size_t i = 0; i < drawItems.size();) {
      const auto merged = mergeDraws
          ? makeCompatibleIndexedDrawMerge(drawItems.subspan(i), payloadArena)
          : CompatibleIndexedDrawMerge{
                .param = drawItems[i],
                .drawCount = 1u,
            };
      const auto& param = merged.param;
      const std::size_t mergedDrawCount =
          std::max<std::size_t>(1u, merged.drawCount);
      const bool usesCommandUniform =
          !param.uniformHandle.valid() ||
          param.uniformHandle == command.drawRunRecord->uniformHandle;
      const auto* drawUniformPayload = usesCommandUniform
          ? commandUniformPayload
          : childUniformCache.payloadForParam(
                command, param,
                perf::DrawUniformPayloadMaterializeSite::DrawEncoderParam);
      if (!drawUniformPayload) {
        return false;
      }

      core::DrawBindingOverride bindingOverride{};
      core::DrawBindingSnapshot bindingSnapshot{};
      core::FlatDrawStateRecord overrideHot{};
      core::DrawShaderLayoutContext overrideShaderLayout{};
      auto drawStateView = stateView;
      const bool hasBindingOverride =
          drawParamBindingOverride(param, payloadArena, bindingOverride);
      const bool hasBindingSnapshot =
          drawParamBindingSnapshot(param, payloadArena, bindingSnapshot);
      if (hasBindingOverride &&
          core::drawBindingOverrideHasBindings(bindingOverride)) {
        overrideHot = hot;
        overrideShaderLayout = *drawStateView.shaderLayout;
        applyDrawBindingOverride(overrideHot, &overrideShaderLayout,
                                 bindingOverride);
        drawStateView.hot = &overrideHot;
        drawStateView.shaderLayout = &overrideShaderLayout;
      }
      drawStateView.uniforms = drawUniformPayload;

      const auto drawBindingPayloadIdentity =
          makeDrawBindingPayloadIdentity(drawStateView);
      const auto drawBindingPayloadCounts =
          makeDrawBindingPayloadCounts(*drawUniformPayload);
      if (!childBinding.lastDrawBindingPayloadIdentity.has_value() &&
          (renderPsoHandle != bindingProof.firstRenderPso ||
           drawBindingPayloadIdentity != bindingProof.firstPayload ||
           drawBindingPayloadCounts != bindingProof.firstPayloadCounts)) {
        return false;
      }
      const auto drawBindingTransition =
          uniform::planDrawBindingTransition(
              childBinding.lastDrawBindingPayloadIdentity.has_value(),
              childBinding.lastDrawBindingPayloadIdentity.value_or(
                  uniform::DrawBindingPayloadIdentity{}),
              drawBindingPayloadIdentity,
              renderPsoBindingAbi, uniform::DrawBindingPath::Direct);
      // TLA+: ParallelDrawBinding / DrawUsesRequiredUniformGeneration and
      // PsoBindingAbiMatchesChildBinding. childBinding is indexed by child
      // ordinal, which is the production ChildBindingShadowsAreIsolated owner.
      DXMT_ASSERT(drawBindingTransition.psoBindingAbiCompatible);
      if (!uniform::applyDrawBindingTransition(
              childBinding.uniformDirty, drawBindingTransition,
              drawBindingPayloadCounts)) {
        return false;
      }

      if (childBinding.activeDrawStateKey.has_value() &&
          (childBinding.activeDrawStateKey->clipPlaneMask !=
               drawStateView.hot->key.clipPlaneMask ||
           childBinding.activeDrawStateKey->clipPlanesHash !=
               drawStateView.hot->key.clipPlanesHash)) {
        uniform::setBit(childBinding.uniformDirty,
                        uniform::DirtyBit::FfpVsClip);
      }
      const bool overrideNeedsBaseStateBind =
          hasBindingOverride && drawBindingOverrideRequiresBaseStateBind(
                                    bindingOverride, stateView.shaderLayout);
      if (overrideNeedsBaseStateBind) {
        return false;
      }
      const bool baseStateCompatible =
          childBinding.activeDrawStateKey.has_value() &&
          childBinding.activeDrawStateUsesPrefetchedPsoLayout &&
          core::drawStateKeysCompatibleForDrawRunBatch(
              *childBinding.activeDrawStateKey,
              drawStateView.hot->key);
      const bool skipBaseStateBind =
          baseStateCompatible && !overrideNeedsBaseStateBind;
      const bool encoded = encodeDraw(
          ctx, commandBuffer, encoder, drawStateView, sourceSeqId,
          skipBaseStateBind, nullptr, &param, payloadArena,
          hasBindingOverride ? &bindingOverride : nullptr,
          hasBindingSnapshot ? &bindingSnapshot : nullptr,
          /*tileFfpMode=*/false,
          /*argbufHybridMode=*/stage2DirectCbuf,
          /*argbufResourceArray=*/false,
          /*argbufDirectCbufMode=*/stage2DirectCbuf,
          /*reopenArgbufHybrid=*/false,
          DrawNativeShadowView{
              .uniformDirty = &childBinding.uniformDirty,
              .textureSampler = &childBinding.textureSamplerShadow,
              .argbufCbufCache = nullptr,
              .streamIbStagingCache =
                  &childBinding.activeStreamIbStaging,
              .renderPsoHandle = renderPsoHandle,
              .tilePsoHandle = tilePsoHandle,
              .depthStencilHandle = depthStencilHandle,
              .commandIndex = commandIndex <=
                      std::numeric_limits<std::uint32_t>::max()
                  ? static_cast<std::uint32_t>(commandIndex)
                  : std::numeric_limits<std::uint32_t>::max(),
              .commandDrawIndex = static_cast<u64>(commandDrawBegin + i),
              .commandDrawCount = static_cast<u64>(commandDrawCount),
              .bindingOverridePrefetchedPsoCompatible =
                  hasBindingOverride && !overrideNeedsBaseStateBind,
          });
      if (encoded) {
        childBinding.activeDrawStateKey = drawStateView.hot->key;
        childBinding.activeDrawStateUsesPrefetchedPsoLayout =
            !overrideNeedsBaseStateBind;
        childBinding.lastDrawBindingPayloadIdentity =
            drawBindingTransition.next;
      }
      i += mergedDrawCount;
    }
    return true;
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

  auto encodeCompleteCommand = [&](auto& replayObserver,
                                   std::size_t commandIndex,
                                   const core::SourceCommandView& source) {
      const auto& command = source.command;
      replayObserver.observe(static_cast<std::uint32_t>(commandIndex), source);
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
              .rects = {},
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
      case Kind::GenerateMipmaps: {
        if (!command.generateMipmaps) break;
        flushPendingClear();
        lifecycle.resolveLateStoreForStoreCause(
            perf::RenderPassLateStoreResolutionCause::Copy);
        flushRender(perf::EncoderSplitReason::SurfaceCopy);
        flushBlit();
        assertHelperEncoderPrecondition();
        if (!dxmt9::encoders::encodeGenerateMipmaps(
                commandBuffer, ctx.pool, *command.generateMipmaps)) {
          abortEncodePartitionInvariant(
              "ordered mipmap generation failed after record acceptance");
        }
        commandBufferHasWork = true;
        assertNoActiveEncoder();
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
        ctx.queue.noteSchedulingPresentDisposition(sourceSeqId,
                                                    presentEncoded);
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

  auto parallelPassForReplayOrdinal = [&](std::uint32_t replayOrdinal)
      -> const SealedParallelPassSnapshot* {
    for (const auto& pass : parallelPassShadows.view()) {
      if (pass.replayOrdinalBegin == replayOrdinal) {
        return &pass;
      }
    }
    return nullptr;
  };

  // One sealed source-local logical pass backed by ordered DrawRun child
  // spans. The coordinator proves one immutable pass-wide direct binding ABI
  // before parent creation: either Stage 1 or Stage 2b direct-cbuf. Slot-30
  // tables, resource arrays, mixed ABIs, and overrides that can rebuild the
  // prefetched PSO stay on the serial path. Clear/Present, pass actions, and
  // completion remain coordinator-owned.
  auto tryEncodeParallelPass = [&]
      (auto& replayObserver,
       const SealedParallelPassSnapshot& pass,
       std::uint32_t commandIndex,
       const core::MetalCommandView& command) -> bool {
    const auto eligibility = classifyParallelPassEligibility(
        ParallelPassEligibilityInput{
            .ranges = pass.rangeView(),
            .firstDrawSnapshots = pass.firstDrawView(),
            .childReplayOrdinalBegins = std::span<const std::uint32_t>(
                pass.childReplayOrdinalBegins.data(), pass.childCount),
            .childReplayOrdinalCounts = std::span<const std::uint32_t>(
                pass.childReplayOrdinalCounts.data(), pass.childCount),
            .passActionEpoch = pass.passActionEpoch,
            .childrenCoverCompleteCommands =
                pass.childrenCoverCompleteCommands,
            .explicitPlan = true,
            .planValidated = true,
            .logicalPassSealed = true,
        });
    auto rejectBeforeEffects = [&] (
        ParallelPassBindingRejectReason bindingReject =
            ParallelPassBindingRejectReason::None) {
      if (perf::enabled()) {
        if (bindingReject != ParallelPassBindingRejectReason::None) {
          perf::countParallelPassBindingReject(bindingReject);
        }
        auto decision =
            decideParallelPassExecution(true, eligibility, false);
        if (bindingReject != ParallelPassBindingRejectReason::None) {
          decision.fallback =
              parallelPassFallbackForBindingReject(bindingReject);
        }
        perf::countParallelPassDecision(decision);
      }
      return false;
    };
    if (!eligibility.eligible || suppressRecordedMetalCalls(ctx) ||
        ctx.drawRecorder || perf::encoderBreakdownEnabled() ||
        commitPolicy == MidChunkCommitPolicy::PerNRecords ||
        encoderState.activeRenderEncoder || encoderState.activeBlitEncoder ||
        encoderState.hasActiveRender || !command.drawRunRecord ||
        !command.drawState.hot || !command.drawState.shaderLayout ||
        pass.childCount < 2u) {
      return rejectBeforeEffects();
    }
    const auto tileDecision = dxmt9::pipeline::selectTileFfpForPass(
        command.drawState, ctx.pool.supportsApple3());
    if (tileDecision.decision == dxmt9::pipeline::TileFfpDecision::Tile ||
        activeSeedMergeAttributionEnabled ||
        parallelRenderPassSidecarObservationEnabled() ||
        diagnosticsState.renderEncoderGpuSampleBuffer) {
      return rejectBeforeEffects();
    }

    thread_local ParallelPassPlanStorage parallelPlanStorage;
    const auto planned = planParallelRenderPassChildren(
        ParallelPassEligibilityInput{
            .ranges = pass.rangeView(),
            .firstDrawSnapshots = pass.firstDrawView(),
            .childReplayOrdinalBegins = std::span<const std::uint32_t>(
                pass.childReplayOrdinalBegins.data(), pass.childCount),
            .childReplayOrdinalCounts = std::span<const std::uint32_t>(
                pass.childReplayOrdinalCounts.data(), pass.childCount),
            .passActionEpoch = pass.passActionEpoch,
            .childrenCoverCompleteCommands =
                pass.childrenCoverCompleteCommands,
            .explicitPlan = true,
            .planValidated = true,
            .logicalPassSealed = true,
        },
        parallelPlanStorage);
    if (!planned.eligible) {
      return rejectBeforeEffects();
    }

    // Re-resolve every locator and draw while the current SourcePayloadView
    // residency pin is still held. This is the complete legitimate-failure
    // boundary: after it, the immutable source and value snapshots make any
    // binding mismatch an invariant failure rather than a serial fallback.
    std::array<EncodePartitionResolution,
               kParallelRenderPassChildCapacity> resolvedChildren{};
    ParallelPassDirectBindingMode passBindingMode =
        ParallelPassDirectBindingMode::Stage1Direct;
    bool passBindingModeSet = false;
    ParallelPassBindingRejectReason bindingReject =
        ParallelPassBindingRejectReason::None;
    ParallelPassEconomicsSummary economics{
        .childCount = static_cast<std::uint32_t>(parallelPlanStorage.count),
        .minimumChildDraws = UINT32_MAX,
        .valid = true,
    };
    std::optional<core::PsoHandle> previousChildLastPso;
    std::optional<uniform::DrawBindingPayloadIdentity>
        previousChildLastPayload;
    std::uint64_t preflightDrawCount = 0u;
    auto addEconomics = [&](std::uint64_t& value,
                            std::uint64_t increment) {
      if (value > UINT64_MAX - increment) {
        economics.overflow = true;
        return;
      }
      value += increment;
    };
    for (std::size_t i = 0; i < parallelPlanStorage.count; ++i) {
      auto& child = parallelPlanStorage.children[i];
      resolvedChildren[i] = resolveEncodePartition(
          child.range, partitionReplayStream);
      const auto& resolved = resolvedChildren[i];
      if (!resolved || !resolved.partition.entry.drawState.hot ||
          !resolved.partition.entry.drawState.shaderLayout ||
          resolved.partition.drawParams.empty() ||
          child.firstDraw.generation != sourceSeqId ||
          child.firstDraw.provenance != child.range.entry ||
          core::makeRenderAttachmentKey(
              *resolved.partition.entry.drawState.hot) !=
              child.firstDraw.entryRender.attachments ||
          resolveParallelPassRenderRoute(
              &ctx.pool, resolved.partition.entry.drawState) !=
              child.firstDraw.entryRender.route) {
        return rejectBeforeEffects();
      }
      std::uint64_t childDrawCount = 0u;
      std::optional<core::PsoHandle> childLastPso;
      std::optional<uniform::DrawBindingPayloadIdentity>
          childLastPayload;
      auto preflightCommand = [&](
          std::uint32_t childCommandIndex,
          const core::MetalCommandView& childCommand,
          std::span<const core::DrawParam> childDrawItems) {
        if (!childCommand.drawState.hot ||
            !childCommand.drawState.shaderLayout ||
            !childCommand.drawRunRecord || childDrawItems.empty() ||
            makeAttachmentKey(*childCommand.drawState.hot) !=
                makeAttachmentKey(*command.drawState.hot) ||
            resolveParallelPassRenderRoute(
                &ctx.pool, childCommand.drawState) !=
                child.firstDraw.entryRender.route ||
            dxmt9::pipeline::selectTileFfpForPass(
                childCommand.drawState,
                ctx.pool.supportsApple3()).decision ==
                dxmt9::pipeline::TileFfpDecision::Tile) {
          return false;
        }
        const auto rawReads =
            core::makeDrawEntryReadSet(childCommand.drawState);
        core::ExactResourceSet canonicalReads{};
        if (!rawReads.complete()) {
          return false;
        }
        for (std::uint32_t resource = 0u;
             resource < rawReads.count; ++resource) {
          std::uint64_t canonical = 0u;
          if (!resolveParallelPassResourceIdentity(
                  &ctx.pool, rawReads.handles[resource], canonical) ||
              canonical == 0u || !canonicalReads.add(canonical)) {
            return false;
          }
        }
        canonicalReads.flags |= core::ExactResourceSetCanonicalized;
        if (!canonicalReads.complete() ||
            !canonicalReads.canonicalized() ||
            (!child.binding.complete &&
             canonicalReads != child.firstDraw.entryRender.entryReads)) {
          return false;
        }
        const core::PsoHandle renderPso =
            childCommand.drawRunRecord->renderPsoHandle;
        const auto renderPsoKey =
            ctx.cache.drawPipelineKeyForHandle(renderPso);
        const auto binding = classifyParallelPassBindingKey({
            .psoPresent = renderPsoKey.has_value(),
            .argbufHybrid =
                renderPsoKey && renderPsoKey->argbufHybridMode,
            .argbufResourceArray =
                renderPsoKey && renderPsoKey->argbufResourceArray,
            .argbufDirectCbuf =
                renderPsoKey && renderPsoKey->argbufDirectCbufMode,
        });
        if (!binding.accepted()) {
          bindingReject = binding.reject;
          return false;
        }
        if (passBindingModeSet && binding.mode != passBindingMode) {
          bindingReject = ParallelPassBindingRejectReason::MixedAbi;
          return false;
        }
        passBindingMode = binding.mode;
        passBindingModeSet = true;

        core::DrawUniformPayload commandUniformScratch;
        const auto* commandUniformPayload =
            core::drawRunUniformPayloadForHandle(
                childCommand,
                childCommand.drawRunRecord->uniformHandle,
                commandUniformScratch,
                perf::DrawUniformPayloadMaterializeSite::DrawEncoderCommand);
        if (!commandUniformPayload) {
          return false;
        }
        core::DrawUniformPayloadMaterializeCache uniformCache;
        const auto payloadArena = core::drawRunPayloadBytes(childCommand);
        for (const auto& draw : childDrawItems) {
          if (!draw.userVertexRange.empty() ||
              !draw.userIndexRange.empty()) {
            return false;
          }
          const bool usesCommandUniform =
              !draw.uniformHandle.valid() ||
              draw.uniformHandle ==
                  childCommand.drawRunRecord->uniformHandle;
          const auto* drawUniformPayload = usesCommandUniform
              ? commandUniformPayload
              : uniformCache.payloadForParam(
                    childCommand, draw,
                    perf::DrawUniformPayloadMaterializeSite::
                        DrawEncoderParam);
          if (!drawUniformPayload) {
            return false;
          }
          core::DrawBindingOverride bindingOverride{};
          const bool hasBindingOverride = drawParamBindingOverride(
              draw, payloadArena, bindingOverride);
          if (hasBindingOverride &&
              drawBindingOverrideRequiresBaseStateBind(
                  bindingOverride, childCommand.drawState.shaderLayout)) {
            bindingReject =
                ParallelPassBindingRejectReason::OverrideRebuild;
            return false;
          }
          const auto payloadIdentity =
              makeDrawBindingPayloadIdentity(
                  core::FlatDrawStateView{
                      .hot = childCommand.drawState.hot,
                      .shaderLayout = childCommand.drawState.shaderLayout,
                      .uniforms = drawUniformPayload,
                      .debug = childCommand.drawState.debug,
                  });
          if (!child.binding.complete) {
            child.binding = ParallelPassBindingSnapshot{
                .firstRenderPso = renderPso,
                .firstPayload = payloadIdentity,
                .firstPayloadCounts =
                    makeDrawBindingPayloadCounts(*drawUniformPayload),
                .mode = binding.mode,
                .reject = ParallelPassBindingRejectReason::None,
                .complete = true,
            };
          }
          if (binding.mode ==
              ParallelPassDirectBindingMode::Stage2DirectCbuf) {
            addEconomics(economics.stage2bDraws, 1u);
          } else {
            addEconomics(economics.stage1Draws, 1u);
          }
          if (!childLastPso.has_value()) {
            if (previousChildLastPso.has_value() &&
                *previousChildLastPso != renderPso) {
              addEconomics(economics.psoBoundaryTransitions, 1u);
            }
            if (previousChildLastPayload.has_value() &&
                *previousChildLastPayload != payloadIdentity) {
              addEconomics(economics.uniformBoundaryTransitions, 1u);
            }
          }
          childLastPso = renderPso;
          childLastPayload = payloadIdentity;
          if (childDrawCount == UINT64_MAX) {
            return false;
          }
          ++childDrawCount;
        }
        (void)childCommandIndex;
        return true;
      };

      if (pass.childrenCoverCompleteCommands) {
        const std::uint64_t ordinalEnd =
            static_cast<std::uint64_t>(child.replayOrdinalBegin) +
            child.replayOrdinalCount;
        for (std::uint64_t ordinal = child.replayOrdinalBegin;
             ordinal < ordinalEnd; ++ordinal) {
          std::uint32_t childCommandIndex = 0u;
          if (!partitionReplayStream.commandIndexAt(
                  static_cast<std::size_t>(ordinal), childCommandIndex)) {
            return rejectBeforeEffects();
          }
          const auto childSource = payload.commandAt(childCommandIndex);
          const auto& childCommand = childSource.command;
          const auto childDrawItems = childCommand.drawItems.empty()
              ? childCommand.drawParams
              : childCommand.drawItems;
          if (childSource.kind() != Kind::DrawRun ||
              !preflightCommand(
                  childCommandIndex, childCommand, childDrawItems)) {
            return rejectBeforeEffects(bindingReject);
          }
        }
      } else if (!preflightCommand(
                     child.range.entry.commandIndex,
                     resolved.partition.entry.command,
                     resolved.partition.drawParams)) {
        return rejectBeforeEffects(bindingReject);
      }
      if (!child.binding.complete || childDrawCount == 0u ||
          childDrawCount > UINT32_MAX) {
        return rejectBeforeEffects(bindingReject);
      }
      if (preflightDrawCount > UINT64_MAX - childDrawCount) {
        return rejectBeforeEffects(bindingReject);
      }
      preflightDrawCount += childDrawCount;
      economics.minimumChildDraws = std::min(
          economics.minimumChildDraws,
          static_cast<std::uint32_t>(childDrawCount));
      economics.maximumChildDraws = std::max(
          economics.maximumChildDraws,
          static_cast<std::uint32_t>(childDrawCount));
      previousChildLastPso = childLastPso;
      previousChildLastPayload = childLastPayload;
    }
    if (!passBindingModeSet || preflightDrawCount != pass.drawCount ||
        validateParallelPassChildPlans(parallelPlanStorage.view()) !=
            ParallelPassFallbackReason::None) {
      return rejectBeforeEffects(bindingReject);
    }
    economics.totalDraws = preflightDrawCount;
    economics.valid = !economics.overflow &&
        economics.totalDraws == pass.drawCount;

    // R-BACK-2.68/2.69: the certificate gate runs before the existing
    // economics classifier and before any Metal effect. It re-reads the pass
    // from its owning batch and re-resolves exact per-child coverage from the
    // live source, so a plan that drifted during the preflight can never reach
    // the selector. The classifier below is unchanged; the proof core only
    // adds rejections.
    ParallelPassCandidateCost candidateCost{};
    const bool candidateCostValid =
        buildParallelPassCandidateCost(economics, candidateCost);
    const ParallelPassCoverageContext coverageContext{
        .stream = &partitionReplayStream,
        .pool = &ctx.pool,
    };
    const auto adapterDecision = runParallelPassProofCoreAdapter(
        pass, parallelPlanStorage.view(), candidateCost,
        ParallelPassSnapshotAuthority{
            .context = &parallelPassShadows,
            .resolve = resolveParallelPassSnapshotAuthority,
        },
        ParallelPassCoverageResolver{
            .context = &coverageContext,
            .resolve = resolveParallelPassCoverage,
        },
        ParallelPassActionEpochWitness{
            .context = &partitionReplayStream,
            .read = resolveParallelPassActionEpochFact,
            .seedEpoch = parallelPassSeedActionEpoch,
            .replayOrdinalCount = static_cast<std::uint32_t>(
                partitionReplayStream.replayOrdinalCount()),
        });
    if (perf::enabled()) {
      perf::countParallelPassAdapter(adapterDecision);
    }

    // The pure economics classifier is unchanged and still evaluated for every
    // candidate the coordinator considered, so its existing attribution keeps
    // its meaning. It can only add a rejection: nothing below can execute
    // unless the certificate, the classifier, and the selector all agree.
    bool economicsAccepted = false;
    const auto economicsDecision = dispatchParallelPassEconomics(
        economics, [&] { economicsAccepted = true; }, [] {},
        parallelPassDrawQuantum, parallelPassImbalanceBound);
    if (perf::enabled()) {
      perf::countParallelPassEconomics(economics, economicsDecision);
    }
    if (!adapterDecision.certificateValid() || !economicsAccepted ||
        !candidateCostValid || !adapterDecision.selected()) {
      return rejectBeforeEffects();
    }

    const bool leadingClearExpected = pass.leadingClear.valid;
    if (leadingClearExpected != passState.pendingClear.has_value() ||
        (leadingClearExpected &&
         (!passState.pendingClearCommand.valid() ||
          passState.pendingClearCommand.seqId != sourceSeqId ||
          passState.pendingClearCommand.commandIndex !=
              pass.leadingClear.commandIndex))) {
      return rejectBeforeEffects();
    }

    const auto storeProofLookahead =
        buildRenderPassStoreProofLookahead(commandIndex);
    const RenderPassStoreProofActivePass storeProofActivePass{
        .hot = command.drawState.hot,
        .allowSameAttachmentContinuation = true,
        .lookaheadMayHaveFutureSources = deferSessionFinalization,
        .lookaheadInvalid = storeProofLookahead.invalid,
        .lookaheadStorageTruncated = storeProofLookahead.storageTruncated,
    };
    PreparedRenderPass prepared{};
    if (!prepareRenderPassWithStoreProofLookahead(
            ctx, command.drawState, passState.pendingClear,
            storeProofLookahead.view(), storeProofActivePass,
            /*sampleBufferAttachments=*/{}, /*visibilityBuffer=*/{},
            prepared)) {
      return rejectBeforeEffects();
    }
    for (std::size_t i = 0; i < prepared.lateStore.count; ++i) {
      if (prepared.lateStore.attachments[i].unresolved()) {
        return rejectBeforeEffects();
      }
    }

    thread_local std::array<encode_session::BindingState,
                            kParallelRenderPassChildCapacity>
        parallelChildBindings;
    for (std::size_t i = 0; i < parallelPlanStorage.count; ++i) {
      parallelChildBindings[i] = {};
      parallelChildBindings[i].initialized = true;
      uniform::markAllDirty(parallelChildBindings[i].uniformDirty);
      parallelChildBindings[i].activeStreamIbStaging.begin(false);
    }

    struct ProductionParallelContext {
      EncodeContext* ctx = nullptr;
      encode_session::EncodeChunkSessionStorage* session = nullptr;
      encode_session::LifecycleRuntime* lifecycle = nullptr;
      PreparedRenderPass* prepared = nullptr;
      decltype(encodeParallelDrawRunCommand)* encodeParallelDrawRun = nullptr;
      std::array<encode_session::BindingState,
                 kParallelRenderPassChildCapacity>* childBindings = nullptr;
      const EncodePartitionReplayStream* stream = nullptr;
      RenderPassFrameTracker* frameTracker = nullptr;
      RenderPassStoreProofSummary storeProof{};
      RenderPassAttachmentFootprint attachmentFootprint{};
      ReplayWindowProvenance replayWindow{};
      core::MetalCommandView command{};
      std::uint32_t commandIndex = 0;
      std::uint32_t sourceIndex = 0;
      std::uint64_t sourceSeqId = 0;
      std::uint64_t encoderIndex = 0;
      std::uint64_t currentLoadBytes = 0;
      ParallelPassDirectBindingMode bindingMode =
          ParallelPassDirectBindingMode::Stage1Direct;
      bool openedWithClear = false;
      perf::EncoderSplitReason closeReason = perf::EncoderSplitReason::Final;
      ParallelPassFailurePhase failurePhase =
          ParallelPassFailurePhase::None;
      std::uint32_t failureChild = 0u;
      bool failed = false;
    };
    ProductionParallelContext production{
        .ctx = &ctx,
        .session = &session,
        .lifecycle = &lifecycle,
        .prepared = &prepared,
        .encodeParallelDrawRun = &encodeParallelDrawRunCommand,
        .childBindings = &parallelChildBindings,
        .stream = &partitionReplayStream,
        .frameTracker = &renderPassFrameTracker,
        .replayWindow = options.replayWindow,
        .command = command,
        .commandIndex = commandIndex,
        .sourceIndex = options.replayWindow.sourceIndex,
        .sourceSeqId = sourceSeqId,
        .encoderIndex = diagnosticsState.renderEncoderIndex,
        .currentLoadBytes = prepared.actions.colorLoadBytes +
            prepared.actions.depthLoadBytes +
            prepared.actions.stencilLoadBytes,
        .bindingMode = passBindingMode,
        .openedWithClear = prepared.actions.color0Clear != 0u ||
            prepared.actions.depthClear != 0u ||
            prepared.actions.stencilClear != 0u,
        .closeReason = parallelPassCloseReason(pass),
    };
    if (perf::enabled()) {
      production.storeProof = renderPassStoreProofSummaryForLookahead(
          ctx, storeProofLookahead.view(), *command.drawState.hot,
          storeProofActivePass);
      production.attachmentFootprint =
          estimateRenderPassAttachmentFootprintBytes(
              ctx, *command.drawState.hot);
    }

    ParallelPassMetalCallbacks callbacks{
        .context = &production,
        .beginPassActions = +[](void* raw) noexcept {
          auto& state = *static_cast<ProductionParallelContext*>(raw);
          auto& storage = *state.session;
          const auto& hot = *state.command.drawState.hot;
          commitPreparedRenderPassOpen(*state.ctx, *state.prepared);
          storage.encoder.hasActiveRender = true;
          storage.pass.activeKey = makeAttachmentKey(hot);
          storage.pass.activeWriteHazard = makeAttachmentHazard(hot);
          storage.pass.activeInstance = RenderPassInstanceToken{
              .seqId = state.sourceSeqId,
              .encoderIndex = state.encoderIndex,
          };
          if (perf::enabled() && state.frameTracker) {
            state.frameTracker->noteStart(
                makeRenderPassFrameKey(hot), state.attachmentFootprint,
                state.storeProof, state.openedWithClear,
                state.currentLoadBytes, state.sourceSeqId,
                state.encoderIndex, state.replayWindow, state.sourceIndex,
                state.commandIndex, ActiveSeedMergeTicketContext{}, nullptr);
          }
          storage.pass.activePassUsesTileFfp = false;
          storage.pass.lateStore = state.prepared->lateStore;
          for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
            storage.pass.activeColorHandles[i] =
                hot.colorAttachments[i].handle;
          }
          storage.pass.pendingClear.reset();
          storage.pass.pendingClearCommand = {};
          storage.binding = {};
          storage.binding.initialized = true;
          storage.diagnostics.activeColorAttachmentDump = {};
          storage.diagnostics.activeDepthAttachmentDump = {};
          storage.diagnostics.activeDrawTextureDumps.clear();
          storage.diagnostics.activeVisibilityScout.reset();
          storage.diagnostics.activeEncoderBreakdown.begin(
              state.sourceSeqId, state.encoderIndex,
              hot.colorAttachments[0].handle.value,
              hot.depthStencil.handle.value);
          storage.binding.activeStreamIbStaging.begin(
              stageStreamIbProbeRowMatches(
                  &storage.diagnostics.activeEncoderBreakdown));
          storage.diagnostics.activeEncoderBreakdown
              .recordAttachmentMetadata(state.ctx->pool, hot);
          ++storage.diagnostics.renderEncoderIndex;
          perf::countPortableFfpPass();
          if (state.bindingMode ==
              ParallelPassDirectBindingMode::Stage2DirectCbuf) {
            perf::countArgbufHybridEncoder();
          } else {
            perf::countStage1Encoder();
            perf::countStage1Bytes(sizeof(VsConsts) + sizeof(PsConsts) +
                                   sizeof(FfpVsConsts) +
                                   sizeof(FfpPsConsts));
          }
          if (auto* recorder = state.ctx->drawRecorder;
              recorder && recorder->beginRenderPass) {
            recorder->beginRenderPass(recorder->userdata,
                                      state.commandIndex);
          }
          return true;
        },
        .replayLogicalCommands =
            +[](void*, std::span<const ParallelPassChildPlan>) noexcept {
          return true;
        },
        .emitChild = +[](void* raw, const ParallelPassChildPlan& child,
                         WMT::RenderCommandEncoder encoder) noexcept {
          auto& state = *static_cast<ProductionParallelContext*>(raw);
          const bool collectChildSplitPerf =
              perf::enabled() && parallelChildSplitPerfEnabled();
          const auto setupStarted = collectChildSplitPerf
              ? std::chrono::steady_clock::now()
              : std::chrono::steady_clock::time_point{};
          const auto resolved = resolveEncodePartition(
              child.range, *state.stream);
          if (!resolved || resolved.partition.drawParams.empty() ||
              (!child.coversCompleteCommands &&
               child.range.entry.commandIndex != state.commandIndex)) {
            return false;
          }
          auto& childBinding =
              (*state.childBindings)[child.childOrdinal];
          configurePreparedRenderPassEncoder(
              *state.ctx, encoder,
              resolved.partition.entry.drawState, *state.prepared);
          encoder.setLabel(makeLabelStringFmt(
              "ParallelRenderPass[seq=%llu,enc=%llu,child=%u]",
              static_cast<unsigned long long>(state.sourceSeqId),
              static_cast<unsigned long long>(state.encoderIndex),
              child.childOrdinal));
          if (collectChildSplitPerf) {
            perf::countParallelChildSetup(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - setupStarted)
                    .count()));
          }
          const auto bodyStarted = collectChildSplitPerf
              ? std::chrono::steady_clock::now()
              : std::chrono::steady_clock::time_point{};
          const auto recordBody = [&] {
            if (collectChildSplitPerf) {
              perf::countParallelChildBody(static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - bodyStarted)
                      .count()));
            }
          };
          const bool bodyOk = [&]() noexcept {
            if (child.coversCompleteCommands) {
              const std::uint64_t ordinalEnd =
                  static_cast<std::uint64_t>(child.replayOrdinalBegin) +
                  child.replayOrdinalCount;
              for (std::uint64_t ordinal = child.replayOrdinalBegin;
                   ordinal < ordinalEnd; ++ordinal) {
                std::uint32_t childCommandIndex = 0u;
                if (!state.stream->commandIndexAt(
                        static_cast<std::size_t>(ordinal),
                        childCommandIndex)) {
                  return false;
                }
                const auto childSource =
                    state.stream->source.payload.commandAt(childCommandIndex);
                if (childSource.kind() != Kind::DrawRun) {
                  return false;
                }
                const auto childDrawItems =
                    childSource.command.drawItems.empty()
                        ? childSource.command.drawParams
                        : childSource.command.drawItems;
                if (!(*state.encodeParallelDrawRun)(
                        childCommandIndex, childSource.command,
                        childDrawItems, 0u, encoder, child.binding,
                        childBinding)) {
                  return false;
                }
              }
              return true;
            }
            const std::size_t commandDrawBegin =
                child.range.entry.drawParamIndex -
                state.command.drawRunRecord->firstParam;
            return (*state.encodeParallelDrawRun)(
                state.commandIndex, state.command,
                resolved.partition.drawParams, commandDrawBegin, encoder,
                child.binding, childBinding);
          }();
          recordBody();
          return bodyOk;
        },
        .joinChild = +[](void*, std::uint32_t) noexcept { return true; },
        .endPassActions = +[](void*) noexcept { return true; },
        .publishSidecars = +[](void*) noexcept { return true; },
        .publishCompletion = +[](void* raw) noexcept {
          auto& state = *static_cast<ProductionParallelContext*>(raw);
          state.lifecycle->completeParallelRenderPass(state.closeReason);
          return true;
        },
        .failStop = +[](void* raw, ParallelPassFailurePhase phase,
                        std::uint32_t child) noexcept {
          auto& state = *static_cast<ProductionParallelContext*>(raw);
          state.failurePhase = phase;
          state.failureChild = child;
          state.failed = true;
        },
    };
    // Parallel children have no serial per-command effect seam. Publish the
    // selected source commands in effective replay order after every proof and
    // fallback gate, immediately before the first parallel encoder effect.
    for (std::uint32_t ordinal = pass.replayOrdinalBegin;
         ordinal < pass.replayOrdinalEnd; ++ordinal) {
      std::uint32_t observedCommandIndex = 0u;
      if (!partitionReplayStream.commandIndexAt(
              ordinal, observedCommandIndex)) {
        abortEncodePartitionInvariant(
            "parallel observer command resolution failed");
      }
      replayObserver.observe(
          observedCommandIndex, payload.commandAt(observedCommandIndex));
    }

    ParallelPassMetalBackend backend(commandBuffer, prepared.info, callbacks);
    std::array<std::uint32_t, kParallelRenderPassChildCapacity>
        completionOrder{};
    for (std::size_t i = 0; i < parallelPlanStorage.count; ++i) {
      completionOrder[i] = static_cast<std::uint32_t>(i);
    }
    const auto execution = executeParallelRenderPass(
        parallelPlanStorage.view(),
        std::span<const std::uint32_t>(completionOrder.data(),
                                       parallelPlanStorage.count),
        backend);
    if (perf::enabled()) {
      if (execution.status == ParallelPassExecutionStatus::Completed) {
        perf::countParallelPassBindingSelected(
            passBindingMode, static_cast<std::uint32_t>(
                parallelPlanStorage.count), pass.drawCount);
      }
      perf::countParallelPassDecision(decideParallelPassExecution(
          true, eligibility,
          execution.status == ParallelPassExecutionStatus::Completed));
    }
    if (production.failed ||
        execution.status == ParallelPassExecutionStatus::FailStop) {
      std::fprintf(
          stderr,
          "[dxmt9-parallel] fail-stop phase=%u child=%u seq=%llu "
          "command=%u children=%zu\n",
          static_cast<unsigned>(production.failurePhase),
          production.failureChild,
          static_cast<unsigned long long>(sourceSeqId), commandIndex,
          parallelPlanStorage.count);
      abortEncodePartitionInvariant(
          "parallel render-pass execution failed after Metal effects");
    }
    if (execution.status != ParallelPassExecutionStatus::Completed) {
      return false;
    }
    commandBufferHasWork = true;
    return true;
  };

  const bool suppressCommandEncoderSideEffects =
      ctx.drawRecorder &&
      ctx.drawRecorder->suppressCommandEncoderSideEffects;
  auto encodeSelectedCommands = [&](auto& replayObserver) {
    std::uint32_t parallelConsumedReplayOrdinalEnd = 0u;
    EncodePartitionSerialCursor partitionCursor(
        partitionReplayStream, partitionRanges,
        useExplicitPartitionPlan);
    EncodePartitionSerialBatch partitionBatch{};
    while (partitionCursor.next(partitionBatch)) {
      if (partitionBatch.kind == EncodePartitionRangeKind::CommandSegment) {
        const std::uint64_t ordinalEnd =
            static_cast<std::uint64_t>(partitionBatch.replayOrdinalBegin) +
            partitionBatch.replayOrdinalCount;
        for (std::uint64_t ordinal = partitionBatch.replayOrdinalBegin;
             ordinal < ordinalEnd; ++ordinal) {
          if (ordinal < parallelConsumedReplayOrdinalEnd) {
            continue;
          }
          std::uint32_t commandIndex = 0;
          const bool resolvedCommand = partitionReplayStream.commandIndexAt(
              static_cast<std::size_t>(ordinal), commandIndex);
          if (!resolvedCommand) {
            abortEncodePartitionInvariant(
                "CommandSegment replay ordinal resolution failed");
          }
          const auto source = payload.commandAt(commandIndex);
          if (suppressCommandEncoderSideEffects) {
            replayObserver.observe(commandIndex, source);
            continue;
          }
          if (source.kind() == Kind::DrawRun) {
            if (const auto* pass = parallelPassForReplayOrdinal(
                    static_cast<std::uint32_t>(ordinal))) {
              traceEncodeCommand("begin", commandIndex, Kind::DrawRun,
                                 source.command);
              if (tryEncodeParallelPass(replayObserver, *pass, commandIndex,
                                        source.command)) {
                parallelConsumedReplayOrdinalEnd = pass->replayOrdinalEnd;
                traceEncodeCommand("after-encode", commandIndex,
                                   Kind::DrawRun, source.command);
                traceEncodeCommand("before-split-policy", commandIndex,
                                   Kind::DrawRun, source.command);
                applyPerRecordSplitPolicy(/*presentRecord=*/false);
                traceEncodeCommand("end", commandIndex, Kind::DrawRun,
                                   source.command);
                continue;
              }
            }
          }
          encodeCompleteCommand(replayObserver, commandIndex, source);
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
      // TLA+: EncoderLifecycle / opCount advances once for the complete source
      // command even when that DrawRun has multiple explicit subranges.
      bool encodedInParallel = false;
      if (partitionBatch.replayOrdinalBegin <
          parallelConsumedReplayOrdinalEnd) {
        continue;
      }
      const auto source = payload.commandAt(commandIndex);
      if (suppressCommandEncoderSideEffects) {
        replayObserver.observe(commandIndex, source);
        continue;
      }
      traceEncodeCommand("begin", commandIndex, Kind::DrawRun, command);
      if (const auto* pass = parallelPassForReplayOrdinal(
              partitionBatch.replayOrdinalBegin)) {
        encodedInParallel = tryEncodeParallelPass(
            replayObserver, *pass, commandIndex, command);
        if (encodedInParallel) {
          parallelConsumedReplayOrdinalEnd = pass->replayOrdinalEnd;
        }
      }
      if (!encodedInParallel) {
        replayObserver.observe(commandIndex, source);
        encodeDrawRunCommand(
            commandIndex, command,
            partitionBatch.identityResolved
                ? std::span<const EncodePartitionRangeSnapshot>{}
                : partitionBatch.ranges);
      }
      traceEncodeCommand("after-encode", commandIndex, Kind::DrawRun, command);
      traceEncodeCommand("before-split-policy", commandIndex, Kind::DrawRun,
                         command);
      applyPerRecordSplitPolicy(/*presentRecord=*/false);
      traceEncodeCommand("end", commandIndex, Kind::DrawRun, command);
    }
  };

  // One cached gate owns the complete disabled path: no observer-specific
  // storage is constructed and no resource visitor runs when the sink is null.
  if (ctx.replayObserver.fn) {
    EnabledReplayObserver replayObserver{
        .sink = ctx.replayObserver,
        .source = options.partitionSource,
        .seqId = sourceSeqId,
    };
    encodeSelectedCommands(replayObserver);
  } else {
    DisabledReplayObserver replayObserver;
    encodeSelectedCommands(replayObserver);
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
    core::SourcePayloadView payload,
    std::uint64_t sourceSeqId,
    EncodeChunkOptions options) {
  if (options.sessionLookaheadBorrows) {
    std::optional<core::metalqueue::QueueSubmissionRecord> result;
    const bool visited = options.sessionLookaheadBorrows->visitResolved(
        [&](std::span<const core::metalqueue::ResolvedPublishedSource> sources)
            noexcept {
          try {
            result = encodeChunkImpl(ctx, slotIndex, payload, sourceSeqId,
                                     std::move(options), sources);
            return true;
          } catch (...) {
            return false;
          }
        });
    return visited ? std::move(result) : std::nullopt;
  }
  const auto fallbackLookaheadSources = options.sessionLookaheadSources;
  return encodeChunkImpl(ctx, slotIndex, payload, sourceSeqId,
                         std::move(options), fallbackLookaheadSources);
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
