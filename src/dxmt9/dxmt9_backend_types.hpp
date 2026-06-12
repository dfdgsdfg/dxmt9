#pragma once

#include "dxmt9/core.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_perf_counters.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::core {

enum class MetalCommandKind : std::uint8_t {
  DrawRun,        // Flat draw state SoA + N DrawParam
  Clear,
  SurfaceCopy,
  StretchRect,
  Readback,
  ColorFill,
  DepthResolve,
  Present,
};

namespace detail {

template <typename Tag>
struct ChunkSoaIndex {
  std::uint32_t value = 0;

  static constexpr ChunkSoaIndex fromU32(std::uint32_t index) noexcept {
    return ChunkSoaIndex{index};
  }

  friend constexpr bool operator==(const ChunkSoaIndex&, const ChunkSoaIndex&) = default;
  friend constexpr bool operator==(ChunkSoaIndex lhs, std::uint32_t rhs) noexcept {
    return lhs.value == rhs;
  }
  friend constexpr bool operator==(std::uint32_t lhs, ChunkSoaIndex rhs) noexcept {
    return lhs == rhs.value;
  }
};

}  // namespace detail

struct CommandPayloadIndexTag;
struct DrawRunRecordIndexTag;
struct ClearRecordIndexTag;
struct SurfaceCopyRecordIndexTag;
struct StretchRectRecordIndexTag;
struct ReadbackRecordIndexTag;
struct ColorFillRecordIndexTag;
struct DepthResolveRecordIndexTag;
struct PresentRecordIndexTag;

using CommandPayloadIndex = detail::ChunkSoaIndex<CommandPayloadIndexTag>;
using DrawRunRecordIndex = detail::ChunkSoaIndex<DrawRunRecordIndexTag>;
using ClearRecordIndex = detail::ChunkSoaIndex<ClearRecordIndexTag>;
using SurfaceCopyRecordIndex = detail::ChunkSoaIndex<SurfaceCopyRecordIndexTag>;
using StretchRectRecordIndex = detail::ChunkSoaIndex<StretchRectRecordIndexTag>;
using ReadbackRecordIndex = detail::ChunkSoaIndex<ReadbackRecordIndexTag>;
using ColorFillRecordIndex = detail::ChunkSoaIndex<ColorFillRecordIndexTag>;
using DepthResolveRecordIndex = detail::ChunkSoaIndex<DepthResolveRecordIndexTag>;
using PresentRecordIndex = detail::ChunkSoaIndex<PresentRecordIndexTag>;
static_assert(sizeof(CommandPayloadIndex) == sizeof(std::uint32_t));
static_assert(sizeof(DrawRunRecordIndex) == sizeof(std::uint32_t));

struct PresentCommandRecord {
  SwapDesc present{};
  Handle presentSource{};
};

struct DrawRunInvariant {
  u64 viewportScissorHash = 0;
  u64 runStableBindingHash = 0;
  u32 streamMask = 0;
  u32 textureMask = 0;
  u32 samplerStateMask = 0;
};

// DrawParam is the per-draw item payload: index/vertex range, primitive
// shape, instance-adjacent base indices, volatile draw constants, and the
// user-buffer changed spans. Keep the alias explicit so command views can
// talk in RunInvariant / DrawItem terms without duplicating storage.
using DrawItem = DrawParam;

struct DrawRunCommandRecord {
  std::uint32_t stateIndex = 0;
  std::uint32_t firstParam = 0;
  std::uint32_t paramCount = 0;
  std::uint32_t payloadOffset = 0;
  std::uint32_t payloadSize = 0;
  DrawUniformHandle uniformHandle{};
  PsoHandle renderPsoHandle{};
  PsoHandle tilePsoHandle{};
  DepthStencilHandle depthStencilHandle{};
  DrawRunInvariant invariant{};
};
static_assert(sizeof(DrawRunCommandRecord) <= 96,
              "DrawRunCommandRecord must keep hot draw-run metadata compact");

struct DrawPsoSubview {
  bool hasShaderContext = false;
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  u64 vertexDeclHash = 0;
  u64 renderStateHash = 0;
  u32 textureMask = 0;
  u32 samplerStateMask = 0;
  u32 renderTargetMask = 0;
  bool samplerLodBias = false;
  std::array<Handle, kMaxRenderTargets> colorAttachmentHandles{};
  Handle depthStencilHandle{};

  friend constexpr bool operator==(const DrawPsoSubview&, const DrawPsoSubview&) = default;
};

struct DrawUniformPayloadRecord {
  DrawUniformHandle handle{};
  DrawUniformPayload payload{};
};

struct MetalCommandHeader {
  MetalCommandKind kind = MetalCommandKind::DrawRun;
  CommandPayloadIndex payloadIndex{};
};

struct MetalCommandView {
  MetalCommandKind kind = MetalCommandKind::DrawRun;
  const DrawRunCommandRecord* drawRunRecord = nullptr;
  const DrawPsoSubview* drawPsoSubview = nullptr;
  const DrawRunInvariant* drawRunInvariant = nullptr;
  FlatDrawStateView drawState{};
  const DrawUniformPayload* drawUniformPayload = nullptr;
  std::span<const DrawUniformPayloadRecord> drawUniformPayloadRecords{};
  std::span<const DrawParam> drawParams{};
  std::span<const DrawItem> drawItems{};
  std::span<const u8> drawPayloadBytes{};
  const ClearDesc* clear = nullptr;
  const SurfaceCopyDesc* surfaceCopy = nullptr;
  const StretchRectDesc* stretchRect = nullptr;
  const ReadbackDesc* readback = nullptr;
  const ColorFillDesc* colorFill = nullptr;
  const DepthResolveDesc* depthResolve = nullptr;
  const PresentCommandRecord* present = nullptr;
};

inline std::span<const u8> drawRunPayloadBytes(const MetalCommandView& command) noexcept {
  return command.drawPayloadBytes;
}

inline std::size_t drawRunPayloadSize(const MetalCommandView& command) noexcept {
  return command.drawPayloadBytes.size();
}

inline std::size_t drawRunDrawCount(const MetalCommandView& command) noexcept {
  return command.drawParams.size();
}

inline const DrawUniformPayload* drawRunUniformPayloadForParam(
    const MetalCommandView& command,
    const DrawParam& param) noexcept {
  if (param.uniformHandle.valid() &&
      param.uniformHandle.index < command.drawUniformPayloadRecords.size()) {
    const auto& record = command.drawUniformPayloadRecords[param.uniformHandle.index];
    if (record.handle == param.uniformHandle) {
      return &record.payload;
    }
  }
  return command.drawUniformPayload;
}

namespace detail {

inline constexpr std::size_t kChunkSlotU32Max =
    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

inline constexpr std::uint32_t kChunkSlotInvalidUniformIndex =
    std::numeric_limits<std::uint32_t>::max();

inline constexpr bool chunkSlotCanAppendU32IndexedElement(std::size_t currentCount) noexcept {
  return currentCount < kChunkSlotU32Max;
}

inline constexpr bool chunkSlotCanAppendU32Range(std::size_t currentCount,
                                                 std::size_t appendCount) noexcept {
  return currentCount <= kChunkSlotU32Max &&
         appendCount <= kChunkSlotU32Max - currentCount;
}

inline constexpr bool chunkSlotCanAppendCommandPayload(std::size_t commandHeaderCount,
                                                       std::size_t payloadRecordCount) noexcept {
  return chunkSlotCanAppendU32IndexedElement(commandHeaderCount) &&
         chunkSlotCanAppendU32IndexedElement(payloadRecordCount);
}

inline bool chunkSlotTryMakeCommandPayloadIndex(std::size_t commandHeaderCount,
                                                std::size_t payloadRecordCount,
                                                CommandPayloadIndex& payloadIndex) noexcept {
  const bool canAppend = chunkSlotCanAppendCommandPayload(commandHeaderCount,
                                                         payloadRecordCount);
  DXMT_ASSERT(canAppend && "command payload SoA storage exceeded 32-bit range storage");
  if (!canAppend) {
    return false;
  }

  payloadIndex = CommandPayloadIndex::fromU32(
      static_cast<std::uint32_t>(payloadRecordCount));
  return true;
}

template <typename Index>
inline constexpr Index chunkSlotPayloadIndex(CommandPayloadIndex index) noexcept {
  return Index::fromU32(index.value);
}

template <typename Index, typename Record>
inline bool chunkSlotIndexInRange(Index index,
                                  const std::vector<Record>& records) noexcept {
  return index.value < records.size();
}

inline DrawParamPayloadView chunkSlotPayloadAt(std::span<const DrawParamPayloadView> payloads,
                                               std::size_t index) noexcept {
  return index < payloads.size() ? payloads[index] : DrawParamPayloadView{};
}

inline constexpr DrawUniformHandle chunkSlotUniformHandle(std::uint32_t index,
                                                          u64 hash) noexcept {
  return DrawUniformHandle{
      .index = index,
      .generation = index + 1u,
      .hash = hash,
  };
}

inline std::size_t chunkSlotUniformLookupBucketCount(std::size_t payloadCount) noexcept {
  std::size_t bucketCount = 8u;
  const std::size_t target = payloadCount > kChunkSlotU32Max / 2u
      ? kChunkSlotU32Max
      : payloadCount * 2u;
  while (bucketCount < target && bucketCount <= kChunkSlotU32Max / 2u) {
    bucketCount *= 2u;
  }
  return bucketCount;
}

inline std::size_t chunkSlotUniformLookupBucket(u64 hash, std::size_t bucketCount) noexcept {
  DXMT_ASSERT(bucketCount > 0 && "uniform lookup bucket table must not be empty");
  const u64 mixedHash = hash ^ (hash >> 32u) ^ (hash >> 17u);
  return static_cast<std::size_t>(mixedHash % bucketCount);
}

class ChunkSlotPerfScope {
 public:
  explicit ChunkSlotPerfScope(void (*record)(std::uint64_t)) noexcept
      : record_(dxmt9::perf::enabled() ? record : nullptr) {
    if (record_) {
      started_ = std::chrono::steady_clock::now();
    }
  }

  ~ChunkSlotPerfScope() {
    if (!record_) {
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    record_(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  }

  ChunkSlotPerfScope(const ChunkSlotPerfScope&) = delete;
  ChunkSlotPerfScope& operator=(const ChunkSlotPerfScope&) = delete;

 private:
  void (*record_)(std::uint64_t) = nullptr;
  std::chrono::steady_clock::time_point started_{};
};

template <typename Vector>
void chunkSlotReserveAtLeast(Vector& storage, std::size_t required) {
  if (storage.capacity() >= required) {
    return;
  }

  std::size_t capacity = storage.capacity();
  if (capacity == 0) {
    capacity = 8u;
  }
  while (capacity < required && capacity <= kChunkSlotU32Max / 2u) {
    capacity *= 2u;
  }
  if (capacity < required) {
    capacity = required;
  }
  storage.reserve(capacity);
}

}  // namespace detail

struct ChunkSlot {
  enum class State { Free, Writing, Pending, Encoding, GPU };

  State state = State::Free;
  u64 seqId = 0;
  bool pipelinePrefetchSealed = false;

  // Data-oriented execution storage. The replay loop walks commandHeaders
  // linearly and indexes into type-specific payload arrays. This avoids the
  // old fat record shape where every command carried every possible payload.
  std::vector<MetalCommandHeader> commandHeaders;
  std::vector<FlatDrawStateRecord> drawHotStates;
  std::vector<DrawShaderLayoutContext> drawShaderLayouts;
  std::vector<DrawDebugSnapshot> drawDebugSnapshots;
  std::vector<DrawPsoSubview> drawPsoSubviews;
  std::vector<DrawUniformPayloadRecord> drawUniformPayloads;
  // Slot-local hash chains for uniform interning; indices point into
  // drawUniformPayloads.
  std::vector<std::uint32_t> drawUniformPayloadLookupHeads;
  std::vector<std::uint32_t> drawUniformPayloadLookupTails;
  std::vector<std::uint32_t> drawUniformPayloadLookupNext;
  DrawUniformHandle lastUniformHandle{};
  std::vector<DrawParam> drawParams;
  std::vector<u8> drawPayloadArena;
  std::vector<DrawRunCommandRecord> drawRunRecords;
  std::vector<ClearDesc> clearRecords;
  std::vector<SurfaceCopyDesc> surfaceCopyRecords;
  std::vector<StretchRectDesc> stretchRectRecords;
  std::vector<ReadbackDesc> readbackRecords;
  std::vector<ColorFillDesc> colorFillRecords;
  std::vector<DepthResolveDesc> depthResolveRecords;
  std::vector<PresentCommandRecord> presentRecords;

  bool commandsEmpty() const noexcept {
    return commandHeaders.empty();
  }

  std::size_t commandCount() const noexcept {
    return commandHeaders.size();
  }

  bool drawOnlyCommandStream() const noexcept {
    if (commandHeaders.empty() || commandHeaders.size() != drawRunRecords.size()) {
      return false;
    }
    for (const auto& header : commandHeaders) {
      if (header.kind != MetalCommandKind::DrawRun) {
        return false;
      }
    }
    return true;
  }

  void clearCommands() {
    pipelinePrefetchSealed = false;
    commandHeaders.clear();
    drawHotStates.clear();
    drawShaderLayouts.clear();
    drawDebugSnapshots.clear();
    drawPsoSubviews.clear();
    drawUniformPayloads.clear();
    drawUniformPayloadLookupHeads.clear();
    drawUniformPayloadLookupTails.clear();
    drawUniformPayloadLookupNext.clear();
    lastUniformHandle = {};
    drawParams.clear();
    drawPayloadArena.clear();
    drawRunRecords.clear();
    clearRecords.clear();
    surfaceCopyRecords.clear();
    stretchRectRecords.clear();
    readbackRecords.clear();
    colorFillRecords.clear();
    depthResolveRecords.clear();
    presentRecords.clear();
  }

  template <typename Record>
  void appendCommandRecord(MetalCommandKind kind, std::vector<Record>& records, Record record) {
    DXMT_ASSERT(!pipelinePrefetchSealed && "cannot append commands after slot prefetch seal");
    if (pipelinePrefetchSealed) {
      return;
    }
    CommandPayloadIndex payloadIndex{};
    if (!detail::chunkSlotTryMakeCommandPayloadIndex(commandHeaders.size(), records.size(),
                                                     payloadIndex)) {
      return;
    }

    commandHeaders.push_back({kind, payloadIndex});
    records.push_back(std::move(record));
  }

  bool prefetchedPipelinesSealed() const noexcept {
    return pipelinePrefetchSealed;
  }

  void sealPrefetchedPipelines() noexcept {
    pipelinePrefetchSealed = true;
  }

  const DrawUniformPayloadRecord* drawUniformPayloadRecord(DrawUniformHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= drawUniformPayloads.size()) {
      return nullptr;
    }

    const auto& record = drawUniformPayloads[handle.index];
    if (!(record.handle == handle)) {
      return nullptr;
    }

    return &record;
  }

  bool drawStateStorageConsistent() const noexcept {
    return drawHotStates.size() == drawShaderLayouts.size() &&
           drawHotStates.size() == drawDebugSnapshots.size();
  }

  void reserveDrawStateStorage(std::size_t stateCount) {
    detail::chunkSlotReserveAtLeast(drawHotStates, stateCount);
    detail::chunkSlotReserveAtLeast(drawShaderLayouts, stateCount);
    detail::chunkSlotReserveAtLeast(drawDebugSnapshots, stateCount);
  }

  std::uint32_t appendDrawState(CanonicalDrawState&& state) {
    DXMT_ASSERT(drawStateStorageConsistent() && "draw state SoA arrays diverged");
    const auto stateIndex = static_cast<std::uint32_t>(drawHotStates.size());
    drawHotStates.push_back(std::move(state.hot));
    drawShaderLayouts.push_back(std::move(state.shaderLayout));
    drawDebugSnapshots.push_back(std::move(state.debug));
    return stateIndex;
  }

  bool drawUniformPayloadLookupReady() const noexcept {
    return !drawUniformPayloadLookupHeads.empty() &&
           drawUniformPayloadLookupHeads.size() == drawUniformPayloadLookupTails.size() &&
           drawUniformPayloadLookupNext.size() == drawUniformPayloads.size();
  }

  void linkDrawUniformPayloadLookupEntry(std::uint32_t uniformIndex) noexcept {
    const auto bucketIndex = detail::chunkSlotUniformLookupBucket(
        drawUniformPayloads[uniformIndex].handle.hash, drawUniformPayloadLookupHeads.size());
    const auto tailIndex = drawUniformPayloadLookupTails[bucketIndex];
    if (tailIndex == detail::kChunkSlotInvalidUniformIndex) {
      drawUniformPayloadLookupHeads[bucketIndex] = uniformIndex;
    } else {
      drawUniformPayloadLookupNext[tailIndex] = uniformIndex;
    }
    drawUniformPayloadLookupTails[bucketIndex] = uniformIndex;
  }

  void rebuildDrawUniformPayloadLookup(std::size_t bucketCount) {
    if (drawUniformPayloads.empty()) {
      drawUniformPayloadLookupHeads.clear();
      drawUniformPayloadLookupTails.clear();
      drawUniformPayloadLookupNext.clear();
      return;
    }

    if (bucketCount < detail::chunkSlotUniformLookupBucketCount(drawUniformPayloads.size())) {
      bucketCount = detail::chunkSlotUniformLookupBucketCount(drawUniformPayloads.size());
    }

    drawUniformPayloadLookupHeads.assign(bucketCount, detail::kChunkSlotInvalidUniformIndex);
    drawUniformPayloadLookupTails.assign(bucketCount, detail::kChunkSlotInvalidUniformIndex);
    drawUniformPayloadLookupNext.assign(drawUniformPayloads.size(),
                                        detail::kChunkSlotInvalidUniformIndex);
    for (std::size_t i = 0; i < drawUniformPayloads.size(); ++i) {
      linkDrawUniformPayloadLookupEntry(static_cast<std::uint32_t>(i));
    }
  }

  void reserveDrawUniformPayloadLookup(std::size_t payloadCount) {
    if (payloadCount == 0) {
      return;
    }

    detail::chunkSlotReserveAtLeast(drawUniformPayloadLookupNext, payloadCount);
    auto bucketCount = detail::chunkSlotUniformLookupBucketCount(payloadCount);
    if (bucketCount < drawUniformPayloadLookupHeads.size()) {
      bucketCount = drawUniformPayloadLookupHeads.size();
    }
    if (drawUniformPayloadLookupHeads.size() < bucketCount ||
        drawUniformPayloadLookupHeads.size() != drawUniformPayloadLookupTails.size() ||
        drawUniformPayloadLookupNext.size() != drawUniformPayloads.size()) {
      rebuildDrawUniformPayloadLookup(bucketCount);
    }
  }

  void appendDrawUniformPayloadLookup(std::uint32_t uniformIndex) {
    if (uniformIndex >= drawUniformPayloads.size()) {
      return;
    }

    if (drawUniformPayloadLookupHeads.empty() ||
        drawUniformPayloadLookupHeads.size() != drawUniformPayloadLookupTails.size() ||
        drawUniformPayloadLookupNext.size() != uniformIndex) {
      rebuildDrawUniformPayloadLookup(
          detail::chunkSlotUniformLookupBucketCount(drawUniformPayloads.size()));
      return;
    }

    drawUniformPayloadLookupNext.push_back(detail::kChunkSlotInvalidUniformIndex);
    linkDrawUniformPayloadLookupEntry(uniformIndex);
  }

  DrawUniformHandle findDrawUniformPayload(const DrawUniformPayload& payload,
                                           DrawUniformHandle candidate = {}) noexcept {
    const bool recordPerf = dxmt9::perf::enabled();
    if (const auto* record = drawUniformPayloadRecord(candidate)) {
      if (record->handle.hash == payload.hash && record->payload == payload) {
        if (recordPerf) {
          dxmt9::perf::countDrawUniformPayloadLookupCandidateHit();
        }
        lastUniformHandle = record->handle;
        return record->handle;
      }
    }

    if (const auto* record = drawUniformPayloadRecord(lastUniformHandle)) {
      if (record->handle.hash == payload.hash && record->payload == payload) {
        if (recordPerf) {
          dxmt9::perf::countDrawUniformPayloadLookupLastHit();
        }
        return record->handle;
      }
    }

    if (drawUniformPayloadLookupReady()) {
      const auto bucketIndex = detail::chunkSlotUniformLookupBucket(
          payload.hash, drawUniformPayloadLookupHeads.size());
      auto uniformIndex = drawUniformPayloadLookupHeads[bucketIndex];
      bool lookupIntact = true;
      std::uint64_t bucketProbes = 0;
      std::uint64_t bucketCollisions = 0;
      std::uint64_t hashCollisions = 0;
      for (std::size_t visited = 0;
           uniformIndex != detail::kChunkSlotInvalidUniformIndex &&
           visited < drawUniformPayloads.size();
           ++visited) {
        if (uniformIndex >= drawUniformPayloads.size()) {
          lookupIntact = false;
          break;
        }

        const auto& record = drawUniformPayloads[uniformIndex];
        ++bucketProbes;
        const bool hashMatches = record.handle.hash == payload.hash;
        if (hashMatches && record.payload == payload) {
          if (recordPerf) {
            dxmt9::perf::countDrawUniformPayloadLookupBucketProbe(bucketProbes);
            dxmt9::perf::countDrawUniformPayloadLookupBucketCollision(
                bucketCollisions);
            dxmt9::perf::countDrawUniformPayloadLookupHashCollision(
                hashCollisions);
            dxmt9::perf::countDrawUniformPayloadLookupBucketHit();
          }
          lastUniformHandle = record.handle;
          return record.handle;
        }
        if (hashMatches) {
          ++hashCollisions;
        } else {
          ++bucketCollisions;
        }
        uniformIndex = drawUniformPayloadLookupNext[uniformIndex];
      }
      if (recordPerf) {
        dxmt9::perf::countDrawUniformPayloadLookupBucketProbe(bucketProbes);
        dxmt9::perf::countDrawUniformPayloadLookupBucketCollision(
            bucketCollisions);
        dxmt9::perf::countDrawUniformPayloadLookupHashCollision(
            hashCollisions);
      }
      if (lookupIntact && uniformIndex == detail::kChunkSlotInvalidUniformIndex) {
        if (recordPerf) {
          dxmt9::perf::countDrawUniformPayloadLookupBucketMiss();
        }
        return {};
      }
    }

    for (std::size_t i = 0; i < drawUniformPayloads.size(); ++i) {
      const auto& record = drawUniformPayloads[i];
      if (record.handle.hash == payload.hash && record.payload == payload) {
        if (recordPerf) {
          dxmt9::perf::countDrawUniformPayloadLookupLinearHit();
        }
        lastUniformHandle = record.handle;
        return record.handle;
      }
    }
    return {};
  }

  DrawUniformHandle appendDrawUniformPayload(const DrawUniformPayload& payload) {
    const bool canUseUniformSoA =
        detail::chunkSlotCanAppendU32IndexedElement(drawUniformPayloads.size());
    DXMT_ASSERT(canUseUniformSoA && "draw uniform payload storage exceeded 32-bit range storage");
    if (!canUseUniformSoA) {
      return {};
    }

    const auto uniformIndex = static_cast<std::uint32_t>(drawUniformPayloads.size());
    const auto uniformHandle = detail::chunkSlotUniformHandle(uniformIndex, payload.hash);
    if (dxmt9::perf::enabled()) {
      dxmt9::perf::countDrawUniformPayloadAppend();
    }
    reserveDrawUniformPayloadLookup(drawUniformPayloads.size() + 1u);
    drawUniformPayloads.push_back(DrawUniformPayloadRecord{
        .handle = uniformHandle,
        .payload = payload,
    });
    appendDrawUniformPayloadLookup(uniformIndex);
    lastUniformHandle = uniformHandle;
    return uniformHandle;
  }

  bool canAppendDrawRun(std::size_t drawCount, std::size_t payloadBytes,
                        bool needsUniformAppend) const noexcept {
    return drawStateStorageConsistent() &&
           detail::chunkSlotCanAppendU32IndexedElement(drawHotStates.size()) &&
           (!needsUniformAppend ||
            detail::chunkSlotCanAppendU32IndexedElement(drawUniformPayloads.size())) &&
           detail::chunkSlotCanAppendU32Range(drawParams.size(), drawCount) &&
           detail::chunkSlotCanAppendU32Range(drawPayloadArena.size(), payloadBytes);
  }

  bool canAppendDrawRunBatch(std::size_t drawCount, std::size_t payloadBytes,
                             std::size_t uniformCount) const noexcept {
    return drawStateStorageConsistent() &&
           detail::chunkSlotCanAppendU32IndexedElement(drawHotStates.size()) &&
           detail::chunkSlotCanAppendU32Range(drawUniformPayloads.size(), uniformCount) &&
           detail::chunkSlotCanAppendU32Range(drawParams.size(), drawCount) &&
           detail::chunkSlotCanAppendU32Range(drawPayloadArena.size(), payloadBytes);
  }

  static DrawPsoSubview makeDrawPsoSubview(const CanonicalDrawState& state) noexcept {
    DrawPsoSubview view{};
    const auto& hot = state.hot;
    const auto& key = hot.key;
    view.hasShaderContext =
        state.shaderLayout.vertexShader.kind != ShaderRef::Kind::None ||
        state.shaderLayout.pixelShader.kind != ShaderRef::Kind::None;
    view.vertexShaderHash = key.vertexShaderHash;
    view.pixelShaderHash = key.pixelShaderHash;
    view.vertexDeclHash = key.vertexDeclHash;
    view.renderStateHash = key.renderStateHash;
    view.textureMask = hot.textureMask;
    view.samplerStateMask = key.samplerStateMask;
    view.renderTargetMask = hot.renderTargetMask;
    for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
      view.colorAttachmentHandles[i] = hot.colorAttachments[i].handle;
    }
    view.depthStencilHandle = hot.depthStencil.handle;
    for (u32 stage = 0; stage < kMaxTextureStages; ++stage) {
      const f32 bias = std::bit_cast<f32>(flatStateOr(
          hot.samplerStates[stage], SAMP_MIPMAP_LOD_BIAS,
          std::bit_cast<u32>(0.0f)));
      if (bias != 0.0f) {
        view.samplerLodBias = true;
        break;
      }
    }
    return view;
  }

  void appendDrawRun(CanonicalDrawState state,
                     const DrawUniformPayload& uniformPayload,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads,
                     DrawUniformHandle uniformHandleCandidate = {}) {
    if (draws.empty()) {
      return;
    }
    DXMT_ASSERT(!pipelinePrefetchSealed && "cannot append draw-runs after slot prefetch seal");
    if (pipelinePrefetchSealed) {
      return;
    }

    DrawUniformHandle uniformHandle = findDrawUniformPayload(uniformPayload, uniformHandleCandidate);
    const bool needsUniformAppend = !uniformHandle.valid();
    CommandPayloadIndex drawRunRecordIndex{};
    if (!detail::chunkSlotTryMakeCommandPayloadIndex(commandHeaders.size(), drawRunRecords.size(),
                                                     drawRunRecordIndex)) {
      return;
    }

    std::uint64_t payloadBytes64 = 0;
    for (std::size_t i = 0; i < draws.size(); ++i) {
      const auto payload = detail::chunkSlotPayloadAt(payloads, i);
      payloadBytes64 += payload.userVertexData.size();
      payloadBytes64 += payload.userIndexData.size();
      payloadBytes64 += payload.bindingOverrideData.size();
      payloadBytes64 += payload.bindingSnapshotData.size();
      if (payloadBytes64 > detail::kChunkSlotU32Max) {
        break;
      }
    }
    const bool payloadBytesFit = payloadBytes64 <= detail::kChunkSlotU32Max;
    const auto payloadBytes = static_cast<std::size_t>(
        payloadBytesFit ? payloadBytes64 : detail::kChunkSlotU32Max);
    const bool canUseSlotSoA =
        payloadBytesFit && canAppendDrawRun(draws.size(), payloadBytes, needsUniformAppend);
    DXMT_ASSERT(canUseSlotSoA && "draw-run SoA storage exceeded 32-bit range storage");
    if (!canUseSlotSoA) {
      return;
    }

    detail::chunkSlotReserveAtLeast(commandHeaders, commandHeaders.size() + 1u);
    detail::chunkSlotReserveAtLeast(drawRunRecords, drawRunRecords.size() + 1u);
    reserveDrawStateStorage(drawHotStates.size() + 1u);
    detail::chunkSlotReserveAtLeast(drawPsoSubviews, drawPsoSubviews.size() + 1u);
    detail::chunkSlotReserveAtLeast(drawParams, drawParams.size() + draws.size());
    detail::chunkSlotReserveAtLeast(drawPayloadArena, drawPayloadArena.size() + payloadBytes);
    if (needsUniformAppend) {
      detail::chunkSlotReserveAtLeast(drawUniformPayloads, drawUniformPayloads.size() + 1u);
      reserveDrawUniformPayloadLookup(drawUniformPayloads.size() + 1u);
    }

    if (needsUniformAppend) {
      uniformHandle = appendDrawUniformPayload(uniformPayload);
      if (!uniformHandle.valid()) {
        return;
      }
    }

    const auto psoSubview = makeDrawPsoSubview(state);
    const DrawRunInvariant invariant{
        .viewportScissorHash = state.hot.key.viewportHash,
        .runStableBindingHash =
            state.hot.key.renderStateHash ^ (state.hot.key.vertexDeclHash << 1) ^
            (static_cast<u64>(state.hot.textureMask) << 2) ^
            (static_cast<u64>(state.hot.key.samplerStateMask) << 3),
        .streamMask = state.hot.streamMask,
        .textureMask = state.hot.textureMask,
        .samplerStateMask = state.hot.key.samplerStateMask,
    };
    const auto stateIndex = appendDrawState(std::move(state));
    const auto firstParam = static_cast<std::uint32_t>(drawParams.size());
    const auto payloadOffset = static_cast<std::uint32_t>(drawPayloadArena.size());

    std::uint32_t recordPayloadSize = 0;
    auto appendPayloadBytes = [&](std::span<const u8> bytes) -> DrawPayloadRange {
      if (bytes.empty()) {
        return {};
      }
      const DrawPayloadRange range{
          .offset = recordPayloadSize,
          .size = static_cast<std::uint32_t>(bytes.size()),
      };
      drawPayloadArena.insert(drawPayloadArena.end(), bytes.begin(), bytes.end());
      recordPayloadSize += range.size;
      return range;
    };
    for (std::size_t i = 0; i < draws.size(); ++i) {
      DrawParam param = draws[i];
      const auto payload = detail::chunkSlotPayloadAt(payloads, i);
      param.userVertexRange = appendPayloadBytes(payload.userVertexData);
      param.userIndexRange = appendPayloadBytes(payload.userIndexData);
      param.bindingOverrideRange = appendPayloadBytes(payload.bindingOverrideData);
      param.bindingSnapshotRange = appendPayloadBytes(payload.bindingSnapshotData);
      drawParams.push_back(std::move(param));
    }

    commandHeaders.push_back({MetalCommandKind::DrawRun, drawRunRecordIndex});
    drawRunRecords.push_back(DrawRunCommandRecord{
        .stateIndex = stateIndex,
        .firstParam = firstParam,
        .paramCount = static_cast<std::uint32_t>(draws.size()),
        .payloadOffset = payloadOffset,
        .payloadSize = recordPayloadSize,
        .uniformHandle = uniformHandle,
        .invariant = invariant,
    });
    drawPsoSubviews.push_back(psoSubview);
  }

  void appendDrawRunBatch(std::span<DrawRunSubmission> submissions) {
    if (submissions.empty()) {
      return;
    }
    DXMT_ASSERT(!pipelinePrefetchSealed && "cannot append draw-runs after slot prefetch seal");
    if (pipelinePrefetchSealed) {
      return;
    }

    CommandPayloadIndex drawRunRecordIndex{};
    if (!detail::chunkSlotTryMakeCommandPayloadIndex(commandHeaders.size(), drawRunRecords.size(),
                                                     drawRunRecordIndex)) {
      return;
    }

    std::uint64_t payloadBytes64 = 0;
    for (const auto& submission : submissions) {
      payloadBytes64 += submission.payload.userVertexData.size();
      payloadBytes64 += submission.payload.userIndexData.size();
      payloadBytes64 += submission.payload.bindingOverrideData.size();
      payloadBytes64 += submission.payload.bindingSnapshotData.size();
      if (payloadBytes64 > detail::kChunkSlotU32Max) {
        break;
      }
    }
    const bool payloadBytesFit = payloadBytes64 <= detail::kChunkSlotU32Max;
    const auto payloadBytes = static_cast<std::size_t>(
        payloadBytesFit ? payloadBytes64 : detail::kChunkSlotU32Max);
    const bool canUseSlotSoA =
        payloadBytesFit && canAppendDrawRunBatch(submissions.size(), payloadBytes,
                                                 submissions.size());
    DXMT_ASSERT(canUseSlotSoA && "draw-run batch SoA storage exceeded 32-bit range storage");
    if (!canUseSlotSoA) {
      return;
    }

    dxmt9::perf::countSubmitDrawRunBatchAppendParams(submissions.size());
    dxmt9::perf::countSubmitDrawRunBatchAppendPayloadBytes(payloadBytes);

    {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countSubmitDrawRunBatchAppendReserveCpuTime);
      detail::chunkSlotReserveAtLeast(commandHeaders, commandHeaders.size() + 1u);
      detail::chunkSlotReserveAtLeast(drawRunRecords, drawRunRecords.size() + 1u);
      reserveDrawStateStorage(drawHotStates.size() + 1u);
      detail::chunkSlotReserveAtLeast(drawPsoSubviews, drawPsoSubviews.size() + 1u);
      detail::chunkSlotReserveAtLeast(drawParams, drawParams.size() + submissions.size());
      detail::chunkSlotReserveAtLeast(drawPayloadArena, drawPayloadArena.size() + payloadBytes);
      detail::chunkSlotReserveAtLeast(drawUniformPayloads,
                                      drawUniformPayloads.size() + submissions.size());
      reserveDrawUniformPayloadLookup(drawUniformPayloads.size() + submissions.size());
    }

    auto& state = submissions.front().state;
    DrawPsoSubview psoSubview{};
    DrawRunInvariant invariant{};
    std::uint32_t stateIndex = 0;
    {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countSubmitDrawRunBatchAppendStateCpuTime);
      psoSubview = makeDrawPsoSubview(state);
      invariant = DrawRunInvariant{
          .viewportScissorHash = state.hot.key.viewportHash,
          .runStableBindingHash =
              state.hot.key.renderStateHash ^ (state.hot.key.vertexDeclHash << 1) ^
              (static_cast<u64>(state.hot.textureMask) << 2) ^
              (static_cast<u64>(state.hot.key.samplerStateMask) << 3),
          .streamMask = state.hot.streamMask,
          .textureMask = state.hot.textureMask,
          .samplerStateMask = state.hot.key.samplerStateMask,
      };
      stateIndex = appendDrawState(std::move(state));
    }
    const auto firstParam = static_cast<std::uint32_t>(drawParams.size());
    const auto payloadOffset = static_cast<std::uint32_t>(drawPayloadArena.size());

    std::uint32_t recordPayloadSize = 0;
    auto appendPayloadBytes = [&](std::span<const u8> bytes) -> DrawPayloadRange {
      if (bytes.empty()) {
        return {};
      }
      const DrawPayloadRange range{
          .offset = recordPayloadSize,
          .size = static_cast<std::uint32_t>(bytes.size()),
      };
      drawPayloadArena.insert(drawPayloadArena.end(), bytes.begin(), bytes.end());
      recordPayloadSize += range.size;
      return range;
    };

    DrawUniformHandle firstUniformHandle{};
    {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countSubmitDrawRunBatchAppendUniformCpuTime);
      for (std::size_t i = 0; i < submissions.size(); ++i) {
        DrawUniformHandle uniformHandle = findDrawUniformPayload(submissions[i].uniforms);
        if (!uniformHandle.valid()) {
          uniformHandle = appendDrawUniformPayload(submissions[i].uniforms);
          if (!uniformHandle.valid()) {
            return;
          }
        }
        if (i == 0) {
          firstUniformHandle = uniformHandle;
        }
        submissions[i].draw.uniformHandle = uniformHandle;
      }
    }

    {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countSubmitDrawRunBatchAppendPayloadCpuTime);
      for (auto& submission : submissions) {
        submission.draw.userVertexRange =
            appendPayloadBytes(submission.payload.userVertexData);
        submission.draw.userIndexRange =
            appendPayloadBytes(submission.payload.userIndexData);
        submission.draw.bindingOverrideRange =
            appendPayloadBytes(submission.payload.bindingOverrideData);
        submission.draw.bindingSnapshotRange =
            appendPayloadBytes(submission.payload.bindingSnapshotData);
      }
    }

    {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countSubmitDrawRunBatchAppendParamCpuTime);
      for (auto& submission : submissions) {
        drawParams.push_back(std::move(submission.draw));
      }
    }

    {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countSubmitDrawRunBatchAppendRecordCpuTime);
      commandHeaders.push_back({MetalCommandKind::DrawRun, drawRunRecordIndex});
      drawRunRecords.push_back(DrawRunCommandRecord{
          .stateIndex = stateIndex,
          .firstParam = firstParam,
          .paramCount = static_cast<std::uint32_t>(submissions.size()),
          .payloadOffset = payloadOffset,
          .payloadSize = recordPayloadSize,
          .uniformHandle = firstUniformHandle,
          .invariant = invariant,
      });
      drawPsoSubviews.push_back(psoSubview);
    }
  }

  void setDrawRunPsoHandles(std::size_t commandIndex,
                            PsoHandle renderPsoHandle,
                            PsoHandle tilePsoHandle = {}) {
    DXMT_ASSERT(!pipelinePrefetchSealed && "cannot patch draw-run PSO handles after slot prefetch seal");
    if (pipelinePrefetchSealed) {
      return;
    }
    if (commandIndex >= commandHeaders.size()) {
      return;
    }
    const auto& header = commandHeaders[commandIndex];
    const auto payloadIndex =
        detail::chunkSlotPayloadIndex<DrawRunRecordIndex>(header.payloadIndex);
    if (header.kind != MetalCommandKind::DrawRun ||
        !detail::chunkSlotIndexInRange(payloadIndex, drawRunRecords)) {
      return;
    }
    auto& record = drawRunRecords[payloadIndex.value];
    record.renderPsoHandle = renderPsoHandle;
    record.tilePsoHandle = tilePsoHandle;
  }

  void setDrawRunDepthStencilHandle(std::size_t commandIndex,
                                    DepthStencilHandle depthStencilHandle) {
    DXMT_ASSERT(!pipelinePrefetchSealed && "cannot patch draw-run depth/stencil handle after slot prefetch seal");
    if (pipelinePrefetchSealed) {
      return;
    }
    if (commandIndex >= commandHeaders.size()) {
      return;
    }
    const auto& header = commandHeaders[commandIndex];
    const auto payloadIndex =
        detail::chunkSlotPayloadIndex<DrawRunRecordIndex>(header.payloadIndex);
    if (header.kind != MetalCommandKind::DrawRun ||
        !detail::chunkSlotIndexInRange(payloadIndex, drawRunRecords)) {
      return;
    }
    auto& record = drawRunRecords[payloadIndex.value];
    record.depthStencilHandle = depthStencilHandle;
  }

  void appendClear(const ClearDesc& clear) {
    appendCommandRecord(MetalCommandKind::Clear, clearRecords, clear);
  }

  void appendSurfaceCopy(const SurfaceCopyDesc& surfaceCopy) {
    appendCommandRecord(MetalCommandKind::SurfaceCopy, surfaceCopyRecords, surfaceCopy);
  }

  void appendStretchRect(const StretchRectDesc& stretchRect) {
    appendCommandRecord(MetalCommandKind::StretchRect, stretchRectRecords, stretchRect);
  }

  void appendReadback(const ReadbackDesc& readback) {
    appendCommandRecord(MetalCommandKind::Readback, readbackRecords, readback);
  }

  void appendColorFill(const ColorFillDesc& colorFill) {
    appendCommandRecord(MetalCommandKind::ColorFill, colorFillRecords, colorFill);
  }

  void appendDepthResolve(const DepthResolveDesc& depthResolve) {
    appendCommandRecord(MetalCommandKind::DepthResolve, depthResolveRecords, depthResolve);
  }

  void appendPresent(const SwapDesc& present, Handle presentSource) {
    appendCommandRecord(MetalCommandKind::Present, presentRecords, PresentCommandRecord{
        .present = present,
        .presentSource = presentSource,
    });
  }

  MetalCommandView commandAt(std::size_t index) const {
    if (index >= commandHeaders.size()) {
      return {};
    }
    const auto& header = commandHeaders[index];
    MetalCommandView view{.kind = header.kind};
    switch (header.kind) {
    case MetalCommandKind::DrawRun:
      view = drawRunCommandAt(index);
      break;
    case MetalCommandKind::Clear: {
      const auto payloadIndex =
          detail::chunkSlotPayloadIndex<ClearRecordIndex>(header.payloadIndex);
      if (detail::chunkSlotIndexInRange(payloadIndex, clearRecords)) view.clear = &clearRecords[payloadIndex.value];
      break;
    }
    case MetalCommandKind::SurfaceCopy: {
      const auto payloadIndex =
          detail::chunkSlotPayloadIndex<SurfaceCopyRecordIndex>(header.payloadIndex);
      if (detail::chunkSlotIndexInRange(payloadIndex, surfaceCopyRecords)) view.surfaceCopy = &surfaceCopyRecords[payloadIndex.value];
      break;
    }
    case MetalCommandKind::StretchRect: {
      const auto payloadIndex =
          detail::chunkSlotPayloadIndex<StretchRectRecordIndex>(header.payloadIndex);
      if (detail::chunkSlotIndexInRange(payloadIndex, stretchRectRecords)) view.stretchRect = &stretchRectRecords[payloadIndex.value];
      break;
    }
    case MetalCommandKind::Readback: {
      const auto payloadIndex =
          detail::chunkSlotPayloadIndex<ReadbackRecordIndex>(header.payloadIndex);
      if (detail::chunkSlotIndexInRange(payloadIndex, readbackRecords)) view.readback = &readbackRecords[payloadIndex.value];
      break;
    }
    case MetalCommandKind::ColorFill: {
      const auto payloadIndex =
          detail::chunkSlotPayloadIndex<ColorFillRecordIndex>(header.payloadIndex);
      if (detail::chunkSlotIndexInRange(payloadIndex, colorFillRecords)) view.colorFill = &colorFillRecords[payloadIndex.value];
      break;
    }
    case MetalCommandKind::DepthResolve: {
      const auto payloadIndex =
          detail::chunkSlotPayloadIndex<DepthResolveRecordIndex>(header.payloadIndex);
      if (detail::chunkSlotIndexInRange(payloadIndex, depthResolveRecords)) view.depthResolve = &depthResolveRecords[payloadIndex.value];
      break;
    }
    case MetalCommandKind::Present: {
      const auto payloadIndex =
          detail::chunkSlotPayloadIndex<PresentRecordIndex>(header.payloadIndex);
      if (detail::chunkSlotIndexInRange(payloadIndex, presentRecords)) view.present = &presentRecords[payloadIndex.value];
      break;
    }
    }
    return view;
  }

  MetalCommandView drawRunCommandAt(std::size_t index) const {
    MetalCommandView view{.kind = MetalCommandKind::DrawRun};
    if (index >= commandHeaders.size()) {
      return view;
    }
    const auto& header = commandHeaders[index];
    if (header.kind != MetalCommandKind::DrawRun) {
      return view;
    }
    const auto payloadIndex =
        detail::chunkSlotPayloadIndex<DrawRunRecordIndex>(header.payloadIndex);
    if (!detail::chunkSlotIndexInRange(payloadIndex, drawRunRecords)) {
      return view;
    }

    const auto& record = drawRunRecords[payloadIndex.value];
    view.drawRunRecord = &record;
    view.drawRunInvariant = &record.invariant;
    if (payloadIndex.value < drawPsoSubviews.size()) {
      view.drawPsoSubview = &drawPsoSubviews[payloadIndex.value];
    }
    if (record.payloadSize > 0 &&
        record.payloadOffset <= drawPayloadArena.size() &&
        record.payloadSize <= drawPayloadArena.size() - record.payloadOffset) {
      view.drawPayloadBytes = std::span<const u8>(
          drawPayloadArena.data() + record.payloadOffset, record.payloadSize);
    }
    if (record.stateIndex < drawHotStates.size() &&
        record.stateIndex < drawShaderLayouts.size() &&
        record.stateIndex < drawDebugSnapshots.size()) {
      view.drawState.hot = &drawHotStates[record.stateIndex];
      view.drawState.shaderLayout = &drawShaderLayouts[record.stateIndex];
      view.drawState.debug = &drawDebugSnapshots[record.stateIndex];
    }
    if (const auto* uniformRecord = drawUniformPayloadRecord(record.uniformHandle)) {
      view.drawUniformPayload = &uniformRecord->payload;
      view.drawState.uniforms = &uniformRecord->payload;
    }
    if (!drawUniformPayloads.empty()) {
      view.drawUniformPayloadRecords = std::span<const DrawUniformPayloadRecord>(
          drawUniformPayloads.data(), drawUniformPayloads.size());
    }
    if (record.firstParam <= drawParams.size() &&
        record.paramCount <= drawParams.size() - record.firstParam) {
      view.drawParams = std::span<const DrawParam>(
          drawParams.data() + record.firstParam, record.paramCount);
      view.drawItems = std::span<const DrawItem>(
          drawParams.data() + record.firstParam, record.paramCount);
    }
    return view;
  }
};

}  // namespace dxmt9::core
