#pragma once

#include "dxmt9/core.hpp"
#include "dxmt9/assert.hpp"

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

struct PresentCommandRecord {
  SwapDesc present{};
  Handle presentSource{};
};

struct DrawRunCommandRecord {
  std::uint32_t stateIndex = 0;
  std::uint32_t firstParam = 0;
  std::uint32_t paramCount = 0;
  std::uint32_t payloadOffset = 0;
  std::uint32_t payloadSize = 0;
  DrawUniformHandle uniformHandle{};
};

struct DrawUniformPayloadRecord {
  DrawUniformHandle handle{};
  DrawUniformPayload payload{};
};

struct MetalCommandHeader {
  MetalCommandKind kind = MetalCommandKind::DrawRun;
  std::uint32_t payloadIndex = 0;
};

struct MetalCommandView {
  MetalCommandKind kind = MetalCommandKind::DrawRun;
  const DrawRunCommandRecord* drawRunRecord = nullptr;
  FlatDrawStateView drawState{};
  const DrawUniformPayload* drawUniformPayload = nullptr;
  std::span<const DrawParam> drawParams{};
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
                                                std::uint32_t& payloadIndex) noexcept {
  const bool canAppend = chunkSlotCanAppendCommandPayload(commandHeaderCount,
                                                         payloadRecordCount);
  DXMT_ASSERT(canAppend && "command payload SoA storage exceeded 32-bit range storage");
  if (!canAppend) {
    return false;
  }

  payloadIndex = static_cast<std::uint32_t>(payloadRecordCount);
  return true;
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

}  // namespace detail

struct ChunkSlot {
  enum class State { Free, Writing, Pending, Encoding, GPU };

  State state = State::Free;
  u64 seqId = 0;

  // Data-oriented execution storage. The replay loop walks commandHeaders
  // linearly and indexes into type-specific payload arrays. This avoids the
  // old fat record shape where every command carried every possible payload.
  std::vector<MetalCommandHeader> commandHeaders;
  std::vector<FlatDrawStateRecord> drawHotStates;
  std::vector<DrawShaderLayoutContext> drawShaderLayouts;
  std::vector<DrawDebugSnapshot> drawDebugSnapshots;
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

  void clearCommands() {
    commandHeaders.clear();
    drawHotStates.clear();
    drawShaderLayouts.clear();
    drawDebugSnapshots.clear();
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
    std::uint32_t payloadIndex = 0;
    if (!detail::chunkSlotTryMakeCommandPayloadIndex(commandHeaders.size(), records.size(),
                                                     payloadIndex)) {
      return;
    }

    commandHeaders.push_back({kind, payloadIndex});
    records.push_back(std::move(record));
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
    drawHotStates.reserve(stateCount);
    drawShaderLayouts.reserve(stateCount);
    drawDebugSnapshots.reserve(stateCount);
  }

  std::uint32_t appendDrawState(CanonicalDrawState state) {
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

    drawUniformPayloadLookupNext.reserve(payloadCount);
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
    if (const auto* record = drawUniformPayloadRecord(candidate)) {
      if (record->handle.hash == payload.hash && record->payload == payload) {
        lastUniformHandle = record->handle;
        return record->handle;
      }
    }

    if (const auto* record = drawUniformPayloadRecord(lastUniformHandle)) {
      if (record->handle.hash == payload.hash && record->payload == payload) {
        return record->handle;
      }
    }

    if (drawUniformPayloadLookupReady()) {
      const auto bucketIndex = detail::chunkSlotUniformLookupBucket(
          payload.hash, drawUniformPayloadLookupHeads.size());
      auto uniformIndex = drawUniformPayloadLookupHeads[bucketIndex];
      bool lookupIntact = true;
      for (std::size_t visited = 0;
           uniformIndex != detail::kChunkSlotInvalidUniformIndex &&
           visited < drawUniformPayloads.size();
           ++visited) {
        if (uniformIndex >= drawUniformPayloads.size()) {
          lookupIntact = false;
          break;
        }

        const auto& record = drawUniformPayloads[uniformIndex];
        if (record.handle.hash == payload.hash && record.payload == payload) {
          lastUniformHandle = record.handle;
          return record.handle;
        }
        uniformIndex = drawUniformPayloadLookupNext[uniformIndex];
      }
      if (lookupIntact && uniformIndex == detail::kChunkSlotInvalidUniformIndex) {
        return {};
      }
    }

    for (std::size_t i = 0; i < drawUniformPayloads.size(); ++i) {
      const auto& record = drawUniformPayloads[i];
      if (record.handle.hash == payload.hash && record.payload == payload) {
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

  void appendDrawRun(CanonicalDrawState state,
                     const DrawUniformPayload& uniformPayload,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads,
                     DrawUniformHandle uniformHandleCandidate = {}) {
    if (draws.empty()) {
      return;
    }

    DrawUniformHandle uniformHandle = findDrawUniformPayload(uniformPayload, uniformHandleCandidate);
    const bool needsUniformAppend = !uniformHandle.valid();
    std::uint32_t drawRunRecordIndex = 0;
    if (!detail::chunkSlotTryMakeCommandPayloadIndex(commandHeaders.size(), drawRunRecords.size(),
                                                     drawRunRecordIndex)) {
      return;
    }

    std::uint64_t payloadBytes64 = 0;
    for (std::size_t i = 0; i < draws.size(); ++i) {
      const auto payload = detail::chunkSlotPayloadAt(payloads, i);
      payloadBytes64 += payload.userVertexData.size();
      payloadBytes64 += payload.userIndexData.size();
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

    commandHeaders.reserve(commandHeaders.size() + 1u);
    drawRunRecords.reserve(drawRunRecords.size() + 1u);
    reserveDrawStateStorage(drawHotStates.size() + 1u);
    drawParams.reserve(drawParams.size() + draws.size());
    drawPayloadArena.reserve(drawPayloadArena.size() + payloadBytes);
    if (needsUniformAppend) {
      drawUniformPayloads.reserve(drawUniformPayloads.size() + 1u);
      reserveDrawUniformPayloadLookup(drawUniformPayloads.size() + 1u);
    }

    if (needsUniformAppend) {
      uniformHandle = appendDrawUniformPayload(uniformPayload);
      if (!uniformHandle.valid()) {
        return;
      }
    }

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
    });
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
    const std::size_t payloadIndex = header.payloadIndex;
    MetalCommandView view{.kind = header.kind};
    switch (header.kind) {
    case MetalCommandKind::DrawRun:
      if (payloadIndex < drawRunRecords.size()) {
        const auto& record = drawRunRecords[payloadIndex];
        view.drawRunRecord = &record;
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
        if (record.firstParam <= drawParams.size() &&
            record.paramCount <= drawParams.size() - record.firstParam) {
          view.drawParams = std::span<const DrawParam>(
              drawParams.data() + record.firstParam, record.paramCount);
        }
      }
      break;
    case MetalCommandKind::Clear:
      if (payloadIndex < clearRecords.size()) view.clear = &clearRecords[payloadIndex];
      break;
    case MetalCommandKind::SurfaceCopy:
      if (payloadIndex < surfaceCopyRecords.size()) view.surfaceCopy = &surfaceCopyRecords[payloadIndex];
      break;
    case MetalCommandKind::StretchRect:
      if (payloadIndex < stretchRectRecords.size()) view.stretchRect = &stretchRectRecords[payloadIndex];
      break;
    case MetalCommandKind::Readback:
      if (payloadIndex < readbackRecords.size()) view.readback = &readbackRecords[payloadIndex];
      break;
    case MetalCommandKind::ColorFill:
      if (payloadIndex < colorFillRecords.size()) view.colorFill = &colorFillRecords[payloadIndex];
      break;
    case MetalCommandKind::DepthResolve:
      if (payloadIndex < depthResolveRecords.size()) view.depthResolve = &depthResolveRecords[payloadIndex];
      break;
    case MetalCommandKind::Present:
      if (payloadIndex < presentRecords.size()) view.present = &presentRecords[payloadIndex];
      break;
    }
    return view;
  }
};

}  // namespace dxmt9::core
