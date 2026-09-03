#pragma once

#include "dxmt9/core.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_perf_counters.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
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
  GenerateMipmaps,
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
struct GenerateMipmapsRecordIndexTag;
struct PresentRecordIndexTag;

using CommandPayloadIndex = detail::ChunkSoaIndex<CommandPayloadIndexTag>;
using DrawRunRecordIndex = detail::ChunkSoaIndex<DrawRunRecordIndexTag>;
using ClearRecordIndex = detail::ChunkSoaIndex<ClearRecordIndexTag>;
using SurfaceCopyRecordIndex = detail::ChunkSoaIndex<SurfaceCopyRecordIndexTag>;
using StretchRectRecordIndex = detail::ChunkSoaIndex<StretchRectRecordIndexTag>;
using ReadbackRecordIndex = detail::ChunkSoaIndex<ReadbackRecordIndexTag>;
using ColorFillRecordIndex = detail::ChunkSoaIndex<ColorFillRecordIndexTag>;
using DepthResolveRecordIndex = detail::ChunkSoaIndex<DepthResolveRecordIndexTag>;
using GenerateMipmapsRecordIndex =
    detail::ChunkSoaIndex<GenerateMipmapsRecordIndexTag>;
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

struct DrawUniformFixedHandle {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;
  u64 hash = 0;

  constexpr bool valid() const noexcept { return generation != 0; }
  friend constexpr bool operator==(const DrawUniformFixedHandle&,
                                   const DrawUniformFixedHandle&) = default;
};

struct DrawUniformFixedPayloadRecord {
  DrawUniformFixedHandle handle{};
  DrawUniformFixedPayload payload{};
};

struct DrawUniformStageHandle {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;
  u64 hash = 0;

  constexpr bool valid() const noexcept { return generation != 0; }
  friend constexpr bool operator==(const DrawUniformStageHandle&,
                                   const DrawUniformStageHandle&) = default;
};

struct DrawUniformVertexConstantsRecord {
  DrawUniformStageHandle handle{};
  DrawUniformStageConstantsSpan constants{};
};

struct DrawUniformPixelConstantsRecord {
  DrawUniformStageHandle handle{};
  DrawUniformStageConstantsSpan constants{};
};

struct DrawUniformPayloadRecord {
  DrawUniformHandle handle{};
  DrawUniformFixedHandle fixedHandle{};
  DrawUniformStageHandle vertexConstantsHandle{};
  DrawUniformStageHandle pixelConstantsHandle{};
  u64 vertexConstantsHash = 0;
  u64 pixelConstantsHash = 0;
  u64 fixedPayloadHash = 0;
  u64 hash = 0;

  DrawUniformPayloadRecord() = default;
  DrawUniformPayloadRecord(DrawUniformHandle uniformHandle,
                           DrawUniformFixedHandle uniformFixedHandle,
                           DrawUniformStageHandle uniformVertexConstantsHandle,
                           DrawUniformStageHandle uniformPixelConstantsHandle,
                           const DrawUniformPayload& uniformPayload)
      : handle(uniformHandle),
        fixedHandle(uniformFixedHandle),
        vertexConstantsHandle(uniformVertexConstantsHandle),
        pixelConstantsHandle(uniformPixelConstantsHandle),
        vertexConstantsHash(uniformPayload.vertexConstantsHash),
        pixelConstantsHash(uniformPayload.pixelConstantsHash),
        fixedPayloadHash(uniformPayload.fixedPayloadHash),
        hash(uniformPayload.hash) {}
  DrawUniformPayloadRecord(DrawUniformHandle uniformHandle,
                           DrawUniformFixedHandle uniformFixedHandle,
                           DrawUniformStageHandle uniformVertexConstantsHandle,
                           DrawUniformStageHandle uniformPixelConstantsHandle,
                           u64 uniformVertexConstantsHash,
                           u64 uniformPixelConstantsHash,
                           u64 uniformFixedPayloadHash,
                           u64 uniformPayloadHash)
      : handle(uniformHandle),
        fixedHandle(uniformFixedHandle),
        vertexConstantsHandle(uniformVertexConstantsHandle),
        pixelConstantsHandle(uniformPixelConstantsHandle),
        vertexConstantsHash(uniformVertexConstantsHash),
        pixelConstantsHash(uniformPixelConstantsHash),
        fixedPayloadHash(uniformFixedPayloadHash),
        hash(uniformPayloadHash) {}
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
  std::span<const DrawUniformFixedPayloadRecord> drawUniformFixedPayloadRecords{};
  std::span<const DrawUniformVertexConstantsRecord> drawUniformVertexConstantsRecords{};
  std::span<const u8> drawUniformVertexConstantBytes{};
  std::span<const DrawUniformPixelConstantsRecord> drawUniformPixelConstantsRecords{};
  std::span<const u8> drawUniformPixelConstantBytes{};
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
  const GenerateMipmapsDesc* generateMipmaps = nullptr;
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

inline std::span<const u8> drawUniformStageConstantsBytes(
    std::span<const u8> arena,
    DrawUniformStageConstantsSpan span) noexcept {
  if (span.byteOffset <= arena.size() &&
      span.byteSize <= arena.size() - span.byteOffset) {
    return arena.subspan(span.byteOffset, span.byteSize);
  }
  return {};
}

template <std::size_t FloatCount>
inline bool drawUniformStageConstantsBytesMatch(
    const ShaderConstantSnapshot<FloatCount>& constants,
    DrawUniformStageConstantsSpan span,
    std::span<const u8> bytes) noexcept {
  const auto expected = makeDrawUniformStageConstantsSpan(
      constants, span.floatCount, span.intCount, span.boolCount, 0);
  if (expected.byteSize != span.byteSize || bytes.size() != span.byteSize ||
      expected.floatCount != span.floatCount ||
      expected.intCount != span.intCount ||
      expected.boolCount != span.boolCount) {
    return false;
  }
  if (span.byteSize == 0) {
    return true;
  }

  const u8* cursor = bytes.data();
  const auto floatBytes =
      static_cast<std::size_t>(span.floatCount) * sizeof(constants.float4[0]);
  if (floatBytes != 0 &&
      std::memcmp(cursor, constants.float4.data(), floatBytes) != 0) {
    return false;
  }
  cursor += floatBytes;

  const auto intBytes =
      static_cast<std::size_t>(span.intCount) * sizeof(constants.int4[0]);
  if (intBytes != 0 &&
      std::memcmp(cursor, constants.int4.data(), intBytes) != 0) {
    return false;
  }
  cursor += intBytes;

  const auto boolBytes =
      static_cast<std::size_t>(span.boolCount) * sizeof(constants.bools[0]);
  return boolBytes == 0 ||
         std::memcmp(cursor, constants.bools.data(), boolBytes) == 0;
}

template <std::size_t FloatCount>
inline void materializeDrawUniformStageConstants(
    DrawUniformStageConstantsSpan span,
    std::span<const u8> bytes,
    ShaderConstantSnapshot<FloatCount>& constants) noexcept {
  constants = {};
  if (bytes.size() != span.byteSize) {
    return;
  }
  if (span.byteSize == 0) {
    return;
  }

  const u8* cursor = bytes.data();
  const auto floatCount =
      std::min<std::size_t>(span.floatCount, constants.float4.size());
  const auto floatBytes = floatCount * sizeof(constants.float4[0]);
  if (floatBytes != 0 && floatBytes <= bytes.size()) {
    std::memcpy(constants.float4.data(), cursor, floatBytes);
  }
  cursor += std::min<std::size_t>(floatBytes, bytes.size());

  const auto consumedFloat =
      static_cast<std::size_t>(cursor - bytes.data());
  const auto intCount =
      std::min<std::size_t>(span.intCount, constants.int4.size());
  const auto intBytes = intCount * sizeof(constants.int4[0]);
  if (intBytes != 0 && consumedFloat <= bytes.size() &&
      intBytes <= bytes.size() - consumedFloat) {
    std::memcpy(constants.int4.data(), cursor, intBytes);
  }
  cursor += std::min<std::size_t>(
      intBytes, consumedFloat <= bytes.size() ? bytes.size() - consumedFloat : 0);

  const auto consumedInt =
      static_cast<std::size_t>(cursor - bytes.data());
  const auto boolCount =
      std::min<std::size_t>(span.boolCount, constants.bools.size());
  const auto boolBytes = boolCount * sizeof(constants.bools[0]);
  if (boolBytes != 0 && consumedInt <= bytes.size() &&
      boolBytes <= bytes.size() - consumedInt) {
    std::memcpy(constants.bools.data(), cursor, boolBytes);
  }
}

inline bool drawUniformFixedPayloadMatches(
    const DrawUniformFixedPayloadRecord& record,
    const DrawUniformFixedPayload& payload,
    u64 fixedPayloadHash) noexcept {
  return record.handle.hash == fixedPayloadHash && record.payload == payload;
}

inline bool drawUniformFixedPayloadMatches(
    const DrawUniformFixedPayloadRecord& record,
    const DrawUniformPayload& payload) noexcept {
  return drawUniformFixedPayloadMatches(
      record, makeDrawUniformFixedPayload(payload), payload.fixedPayloadHash);
}

inline bool drawUniformVertexConstantsMatches(
    const DrawUniformVertexConstantsRecord& record,
    std::span<const u8> constantsBytes,
    const DrawUniformPayload& payload) noexcept {
  const auto expected = makeDrawUniformVertexConstantsSpan(
      payload, record.constants.byteOffset);
  return record.handle.hash == payload.vertexConstantsHash &&
         record.constants.floatCount == expected.floatCount &&
         record.constants.intCount == expected.intCount &&
         record.constants.boolCount == expected.boolCount &&
         drawUniformStageConstantsBytesMatch(
             payload.vsConst, record.constants, constantsBytes);
}

inline bool drawUniformPixelConstantsMatches(
    const DrawUniformPixelConstantsRecord& record,
    std::span<const u8> constantsBytes,
    const DrawUniformPayload& payload) noexcept {
  const auto expected = makeDrawUniformPixelConstantsSpan(
      payload, record.constants.byteOffset);
  return record.handle.hash == payload.pixelConstantsHash &&
         record.constants.floatCount == expected.floatCount &&
         record.constants.intCount == expected.intCount &&
         record.constants.boolCount == expected.boolCount &&
         drawUniformStageConstantsBytesMatch(
             payload.psConst, record.constants, constantsBytes);
}

inline void materializeDrawUniformPayload(
    const DrawUniformPayloadRecord& record,
    const DrawUniformFixedPayloadRecord& fixedRecord,
    const DrawUniformVertexConstantsRecord& vertexRecord,
    std::span<const u8> vertexConstantsBytes,
    const DrawUniformPixelConstantsRecord& pixelRecord,
    std::span<const u8> pixelConstantsBytes,
    DrawUniformPayload& payload) noexcept {
  materializeDrawUniformStageConstants(
      vertexRecord.constants, vertexConstantsBytes, payload.vsConst);
  materializeDrawUniformStageConstants(
      pixelRecord.constants, pixelConstantsBytes, payload.psConst);
  payload.worldViewProj = fixedRecord.payload.worldViewProj;
  payload.ffpView = fixedRecord.payload.ffpView;
  payload.ffpWorldView = fixedRecord.payload.ffpWorldView;
  payload.ffpNormalMatrix = fixedRecord.payload.ffpNormalMatrix;
  payload.material = fixedRecord.payload.material;
  payload.lights = fixedRecord.payload.lights;
  payload.ffpBlendWorldViewProj = fixedRecord.payload.ffpBlendWorldViewProj;
  payload.ffpBlendWorldView = fixedRecord.payload.ffpBlendWorldView;
  payload.ffpBlendNormalMatrix = fixedRecord.payload.ffpBlendNormalMatrix;
  payload.textureTransforms = fixedRecord.payload.textureTransforms;
  payload.clipPlaneMask = fixedRecord.payload.clipPlaneMask;
  payload.clipPlanes = fixedRecord.payload.clipPlanes;
  payload.vertexConstantsHash = record.vertexConstantsHash;
  payload.pixelConstantsHash = record.pixelConstantsHash;
  payload.fixedPayloadHash = record.fixedPayloadHash;
  payload.hash = record.hash;
  payload.vertexFloatConstantCount = vertexRecord.constants.floatCount;
  payload.vertexIntConstantCount = vertexRecord.constants.intCount;
  payload.vertexBoolConstantCount = vertexRecord.constants.boolCount;
  payload.pixelFloatConstantCount = pixelRecord.constants.floatCount;
  payload.pixelIntConstantCount = pixelRecord.constants.intCount;
  payload.pixelBoolConstantCount = pixelRecord.constants.boolCount;
}

inline bool drawUniformPayloadRecordMatches(
    const DrawUniformPayloadRecord& record,
    const DrawUniformFixedPayloadRecord& fixedRecord,
    const DrawUniformVertexConstantsRecord& vertexRecord,
    std::span<const u8> vertexConstantsBytes,
    const DrawUniformPixelConstantsRecord& pixelRecord,
    std::span<const u8> pixelConstantsBytes,
    const DrawUniformPayload& payload) noexcept {
  return record.handle.hash == payload.hash &&
         record.hash == payload.hash &&
         record.vertexConstantsHash == payload.vertexConstantsHash &&
         record.pixelConstantsHash == payload.pixelConstantsHash &&
         record.fixedPayloadHash == payload.fixedPayloadHash &&
         record.vertexConstantsHandle == vertexRecord.handle &&
         record.pixelConstantsHandle == pixelRecord.handle &&
         vertexRecord.handle.hash == payload.vertexConstantsHash &&
         pixelRecord.handle.hash == payload.pixelConstantsHash &&
         drawUniformVertexConstantsMatches(
             vertexRecord, vertexConstantsBytes, payload) &&
         drawUniformPixelConstantsMatches(
             pixelRecord, pixelConstantsBytes, payload) &&
         drawUniformFixedPayloadMatches(fixedRecord, payload);
}

inline const DrawUniformFixedPayloadRecord* drawRunFixedPayloadRecord(
    const MetalCommandView& command,
    DrawUniformFixedHandle handle) noexcept {
  if (handle.valid() &&
      handle.index < command.drawUniformFixedPayloadRecords.size()) {
    const auto& record = command.drawUniformFixedPayloadRecords[handle.index];
    if (record.handle == handle) {
      return &record;
    }
  }
  return nullptr;
}

inline const DrawUniformPayloadRecord* drawRunUniformPayloadRecord(
    const MetalCommandView& command,
    DrawUniformHandle handle) noexcept {
  if (handle.valid() &&
      handle.index < command.drawUniformPayloadRecords.size()) {
    const auto& record = command.drawUniformPayloadRecords[handle.index];
    if (record.handle == handle) {
      return &record;
    }
  }
  return nullptr;
}

inline const DrawUniformVertexConstantsRecord* drawRunVertexConstantsRecord(
    const MetalCommandView& command,
    DrawUniformStageHandle handle) noexcept {
  if (handle.valid() &&
      handle.index < command.drawUniformVertexConstantsRecords.size()) {
    const auto& record = command.drawUniformVertexConstantsRecords[handle.index];
    if (record.handle == handle) {
      return &record;
    }
  }
  return nullptr;
}

inline std::span<const u8> drawRunVertexConstantsBytes(
    const MetalCommandView& command,
    const DrawUniformVertexConstantsRecord& record) noexcept {
  return drawUniformStageConstantsBytes(
      command.drawUniformVertexConstantBytes, record.constants);
}

inline const DrawUniformPixelConstantsRecord* drawRunPixelConstantsRecord(
    const MetalCommandView& command,
    DrawUniformStageHandle handle) noexcept {
  if (handle.valid() &&
      handle.index < command.drawUniformPixelConstantsRecords.size()) {
    const auto& record = command.drawUniformPixelConstantsRecords[handle.index];
    if (record.handle == handle) {
      return &record;
    }
  }
  return nullptr;
}

inline std::span<const u8> drawRunPixelConstantsBytes(
    const MetalCommandView& command,
    const DrawUniformPixelConstantsRecord& record) noexcept {
  return drawUniformStageConstantsBytes(
      command.drawUniformPixelConstantBytes, record.constants);
}

inline const DrawUniformPayload* drawRunUniformPayloadForHandle(
    const MetalCommandView& command,
    DrawUniformHandle handle,
    DrawUniformPayload& scratch,
    dxmt9::perf::DrawUniformPayloadMaterializeSite site =
        dxmt9::perf::DrawUniformPayloadMaterializeSite::Other) noexcept {
  const auto* uniformRecord = drawRunUniformPayloadRecord(command, handle);
  if (!uniformRecord) {
    if (dxmt9::perf::enabled()) {
      dxmt9::perf::countDrawUniformPayloadMaterializeFallback(site);
    }
    return command.drawUniformPayload;
  }
  const auto* fixedRecord =
      drawRunFixedPayloadRecord(command, uniformRecord->fixedHandle);
  if (!fixedRecord) {
    if (dxmt9::perf::enabled()) {
      dxmt9::perf::countDrawUniformPayloadMaterializeFallback(site);
    }
    return command.drawUniformPayload;
  }
  const auto* vertexRecord =
      drawRunVertexConstantsRecord(command, uniformRecord->vertexConstantsHandle);
  const auto* pixelRecord =
      drawRunPixelConstantsRecord(command, uniformRecord->pixelConstantsHandle);
  if (!vertexRecord || !pixelRecord) {
    if (dxmt9::perf::enabled()) {
      dxmt9::perf::countDrawUniformPayloadMaterializeFallback(site);
    }
    return command.drawUniformPayload;
  }
  const auto vertexBytes = drawRunVertexConstantsBytes(command, *vertexRecord);
  const auto pixelBytes = drawRunPixelConstantsBytes(command, *pixelRecord);
  if (vertexBytes.size() != vertexRecord->constants.byteSize ||
      pixelBytes.size() != pixelRecord->constants.byteSize) {
    if (dxmt9::perf::enabled()) {
      dxmt9::perf::countDrawUniformPayloadMaterializeFallback(site);
    }
    return command.drawUniformPayload;
  }
  const bool recordPerf = dxmt9::perf::enabled();
  const auto materializeStarted = recordPerf
      ? std::chrono::steady_clock::now()
      : std::chrono::steady_clock::time_point{};
  materializeDrawUniformPayload(*uniformRecord, *fixedRecord,
                                *vertexRecord, vertexBytes,
                                *pixelRecord, pixelBytes, scratch);
  if (recordPerf) {
    dxmt9::perf::countDrawUniformPayloadMaterialized(
        site, sizeof(DrawUniformPayload));
    dxmt9::perf::countDrawUniformPayloadMaterializeCpuTime(
        site,
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - materializeStarted)
                .count()));
  }
  return &scratch;
}

inline const DrawUniformPayload* drawRunUniformPayloadForParam(
    const MetalCommandView& command,
    const DrawParam& param,
    DrawUniformPayload& scratch,
    dxmt9::perf::DrawUniformPayloadMaterializeSite site =
        dxmt9::perf::DrawUniformPayloadMaterializeSite::Other) noexcept {
  if ((!param.uniformHandle.valid() ||
       (command.drawRunRecord &&
        param.uniformHandle == command.drawRunRecord->uniformHandle)) &&
      command.drawUniformPayload) {
    return command.drawUniformPayload;
  }
  if (param.uniformHandle.valid()) {
    return drawRunUniformPayloadForHandle(
        command, param.uniformHandle, scratch, site);
  }
  if (command.drawRunRecord) {
    return drawRunUniformPayloadForHandle(
        command, command.drawRunRecord->uniformHandle, scratch, site);
  }
  return command.drawUniformPayload;
}

inline const DrawUniformPayload* drawRunUniformPayloadForParam(
    const MetalCommandView& command,
    const DrawParam& param) noexcept {
  static thread_local DrawUniformPayload scratch;
  return drawRunUniformPayloadForParam(command, param, scratch);
}

struct DrawUniformPayloadMaterializeCache {
  DrawUniformHandle handle{};
  const DrawUniformPayloadRecord* payloadRecords = nullptr;
  std::size_t payloadRecordCount = 0;
  // Returned pointers refer to this single scratch and are invalidated by the
  // next cache miss. Keep long-lived command payloads in a separate scratch.
  DrawUniformPayload payload;
  bool valid = false;

  void reset() noexcept {
    handle = {};
    payloadRecords = nullptr;
    payloadRecordCount = 0;
    valid = false;
  }

  bool matchesSource(const MetalCommandView& command) const noexcept {
    return payloadRecords == command.drawUniformPayloadRecords.data() &&
           payloadRecordCount == command.drawUniformPayloadRecords.size();
  }

  const DrawUniformPayload* payloadForHandle(
      const MetalCommandView& command,
      DrawUniformHandle requestedHandle,
      dxmt9::perf::DrawUniformPayloadMaterializeSite site =
          dxmt9::perf::DrawUniformPayloadMaterializeSite::Other) noexcept {
    if (valid && requestedHandle.valid() && requestedHandle == handle &&
        matchesSource(command)) {
      return &payload;
    }

    const auto* resolved = drawRunUniformPayloadForHandle(
        command, requestedHandle, payload, site);
    if (resolved == &payload) {
      handle = requestedHandle;
      payloadRecords = command.drawUniformPayloadRecords.data();
      payloadRecordCount = command.drawUniformPayloadRecords.size();
      valid = true;
    } else {
      reset();
    }
    return resolved;
  }

  const DrawUniformPayload* payloadForParam(
      const MetalCommandView& command,
      const DrawParam& param,
      dxmt9::perf::DrawUniformPayloadMaterializeSite site =
          dxmt9::perf::DrawUniformPayloadMaterializeSite::Other) noexcept {
    if ((!param.uniformHandle.valid() ||
         (command.drawRunRecord &&
          param.uniformHandle == command.drawRunRecord->uniformHandle)) &&
        command.drawUniformPayload) {
      return command.drawUniformPayload;
    }
    if (param.uniformHandle.valid()) {
      return payloadForHandle(command, param.uniformHandle, site);
    }
    if (command.drawRunRecord) {
      return payloadForHandle(command, command.drawRunRecord->uniformHandle,
                              site);
    }
    return command.drawUniformPayload;
  }
};

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

inline constexpr DrawUniformFixedHandle
chunkSlotUniformFixedHandle(std::uint32_t index, u64 hash) noexcept {
  return DrawUniformFixedHandle{
      .index = index,
      .generation = index + 1u,
      .hash = hash,
  };
}

inline constexpr DrawUniformStageHandle
chunkSlotUniformStageHandle(std::uint32_t index, u64 hash) noexcept {
  return DrawUniformStageHandle{
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

inline bool chunkSlotDisableDrawUniformPayloadDedup() noexcept {
  static const bool disabled = []() noexcept {
    const char* env = std::getenv("DXMT9_DISABLE_DRAW_UNIFORM_PAYLOAD_DEDUP");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return disabled;
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
  enum class State { Free, Writing, Pending, Encoding, Retiring, GPU };

  u64 seqId = 0;
  dxmt9::perf::ChunkPublishReason publishReason =
      dxmt9::perf::ChunkPublishReason::Unknown;
  bool pipelinePrefetchSealed = false;
  std::size_t pipelinePrefetchCommandCursor = 0;

  // Data-oriented execution storage. The replay loop walks commandHeaders
  // linearly and indexes into type-specific payload arrays. This avoids the
  // old fat record shape where every command carried every possible payload.
  std::vector<MetalCommandHeader> commandHeaders;
  std::vector<FlatDrawStateRecord> drawHotStates;
  std::vector<DrawShaderLayoutContext> drawShaderLayouts;
  std::vector<DrawDebugSnapshot> drawDebugSnapshots;
  std::vector<DrawPsoSubview> drawPsoSubviews;
  std::vector<DrawUniformFixedPayloadRecord> drawUniformFixedPayloads;
  std::vector<DrawUniformVertexConstantsRecord> drawUniformVertexConstants;
  std::vector<u8> drawUniformVertexConstantBytes;
  std::vector<DrawUniformPixelConstantsRecord> drawUniformPixelConstants;
  std::vector<u8> drawUniformPixelConstantBytes;
  std::vector<DrawUniformPayloadRecord> drawUniformPayloads;
  // Slot-local hash chains for uniform interning; indices point into
  // drawUniformPayloads.
  std::vector<std::uint32_t> drawUniformPayloadLookupHeads;
  std::vector<std::uint32_t> drawUniformPayloadLookupTails;
  std::vector<std::uint32_t> drawUniformPayloadLookupNext;
  std::vector<std::uint32_t> drawUniformVertexConstantsLookupHeads;
  std::vector<std::uint32_t> drawUniformVertexConstantsLookupTails;
  std::vector<std::uint32_t> drawUniformVertexConstantsLookupNext;
  std::vector<std::uint32_t> drawUniformPixelConstantsLookupHeads;
  std::vector<std::uint32_t> drawUniformPixelConstantsLookupTails;
  std::vector<std::uint32_t> drawUniformPixelConstantsLookupNext;
  DrawUniformFixedHandle lastUniformFixedHandle{};
  DrawUniformStageHandle lastUniformVertexConstantsHandle{};
  DrawUniformStageHandle lastUniformPixelConstantsHandle{};
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
  std::vector<GenerateMipmapsDesc> generateMipmapsRecords;
  std::vector<PresentCommandRecord> presentRecords;

  // Physical bytes temporarily owned by the out-of-lock resource-owner
  // release buffer. Reconciliation may inspect every compatibility payload
  // while this payload is Reclaiming, so retained-byte accounting must remain
  // exact across the detach/restore window rather than observing a temporary
  // zero-capacity drawShaderLayouts vector.
  std::size_t detachedResourceOwnerRetainedBytes = 0;

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

  // VertexDeclSnapshot retains core buffers. Detach them before clearing a
  // slot under the queue mutex so a last-owner Buffer destructor can re-enter
  // the backend resource pool only after that mutex is released.
  //
  // The transfer is a swap with a default-constructed local, not
  // `return std::move(drawShaderLayouts)`. The standard leaves a moved-from
  // `std::vector` valid but unspecified, and R-BACK-2.105 needs the
  // post-condition to be exact: after this returns, `drawShaderLayouts` is
  // specified empty with capacity 0, and the original allocation is
  // deterministically owned by the returned vector. `restoreResourceOwnerStorage`
  // relies on both halves of that.
  //
  // Surrendering the buffer matters because `drawShaderLayouts` is the only
  // provisioned dimension reclaim detaches. Without the round trip a
  // reclaimed payload would therefore be the one shape a complete physical-
  // capacity coverage predicate can never accept. The caller empties this
  // vector with the mutex released and hands the storage back through
  // `restoreResourceOwnerStorage` after the relock.
  std::vector<DrawShaderLayoutContext> detachResourceOwners() {
    DXMT_ASSERT(detachedResourceOwnerRetainedBytes == 0 &&
                "resource-owner storage may have only one detached owner");
    detachedResourceOwnerRetainedBytes =
        drawShaderLayouts.capacity() >
                std::numeric_limits<std::size_t>::max() /
                    sizeof(DrawShaderLayoutContext)
            ? std::numeric_limits<std::size_t>::max()
            : drawShaderLayouts.capacity() * sizeof(DrawShaderLayoutContext);
    std::vector<DrawShaderLayoutContext> owners;
    using std::swap;
    swap(drawShaderLayouts, owners);
    return owners;
  }

  // Second half of the reclaim round trip (R-BACK-2.105). `owners` must
  // already have been emptied outside the queue mutex -- that is where the
  // last-owner destructors ran, at a cost proportional to the number of layout
  // rows -- and this payload's own vector must still be the specified-empty,
  // capacity-zero shell `detachResourceOwners()` swapped in. The swap back is
  // O(1) and frees nothing, so the retained capacity survives reclaim without
  // any allocation or copy inside the critical section.
  //
  // Deliberately a no-op rather than an assertion when either premise fails:
  // forfeiting the capacity degrades the next use to a full reprovision, which
  // is exactly the pre-R-BACK-2.105 behaviour and always correct.
  void restoreResourceOwnerStorage(
      std::vector<DrawShaderLayoutContext>& owners) noexcept {
    if (!owners.empty() || !drawShaderLayouts.empty() ||
        drawShaderLayouts.capacity() != 0) {
      return;
    }
    using std::swap;
    swap(drawShaderLayouts, owners);
    detachedResourceOwnerRetainedBytes = 0;
  }

  // Failure-path counterpart used only after the detached vector and its
  // allocation have actually been destroyed without a restore. Normal
  // reclaim always restores; an abandoned round trip poisons the queue.
  void abandonDetachedResourceOwnerStorage() noexcept {
    DXMT_ASSERT(drawShaderLayouts.empty() &&
                drawShaderLayouts.capacity() == 0);
    detachedResourceOwnerRetainedBytes = 0;
  }

  void clearCommands() {
    publishReason = dxmt9::perf::ChunkPublishReason::Unknown;
    pipelinePrefetchSealed = false;
    pipelinePrefetchCommandCursor = 0;
    commandHeaders.clear();
    drawHotStates.clear();
    drawShaderLayouts.clear();
    drawDebugSnapshots.clear();
    drawPsoSubviews.clear();
    drawUniformFixedPayloads.clear();
    drawUniformVertexConstants.clear();
    drawUniformVertexConstantBytes.clear();
    drawUniformPixelConstants.clear();
    drawUniformPixelConstantBytes.clear();
    drawUniformPayloads.clear();
    drawUniformPayloadLookupHeads.clear();
    drawUniformPayloadLookupTails.clear();
    drawUniformPayloadLookupNext.clear();
    drawUniformVertexConstantsLookupHeads.clear();
    drawUniformVertexConstantsLookupTails.clear();
    drawUniformVertexConstantsLookupNext.clear();
    drawUniformPixelConstantsLookupHeads.clear();
    drawUniformPixelConstantsLookupTails.clear();
    drawUniformPixelConstantsLookupNext.clear();
    lastUniformFixedHandle = {};
    lastUniformVertexConstantsHandle = {};
    lastUniformPixelConstantsHandle = {};
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
    generateMipmapsRecords.clear();
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

  std::size_t prefetchedPipelineCommandCursor() const noexcept {
    return pipelinePrefetchCommandCursor;
  }

  void setPrefetchedPipelineCommandCursor(std::size_t commandIndex) noexcept {
    pipelinePrefetchCommandCursor =
        commandIndex > commandHeaders.size() ? commandHeaders.size() : commandIndex;
  }

  void sealPrefetchedPipelines() noexcept {
    pipelinePrefetchCommandCursor = commandHeaders.size();
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

  const DrawUniformFixedPayloadRecord*
  drawUniformFixedPayloadRecord(DrawUniformFixedHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= drawUniformFixedPayloads.size()) {
      return nullptr;
    }

    const auto& record = drawUniformFixedPayloads[handle.index];
    if (!(record.handle == handle)) {
      return nullptr;
    }

    return &record;
  }

  template <typename Record>
  const Record* drawUniformStageRecord(const std::vector<Record>& records,
                                       DrawUniformStageHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= records.size()) {
      return nullptr;
    }

    const auto& record = records[handle.index];
    if (!(record.handle == handle)) {
      return nullptr;
    }

    return &record;
  }

  const DrawUniformVertexConstantsRecord*
  drawUniformVertexConstantsRecord(DrawUniformStageHandle handle) const noexcept {
    return drawUniformStageRecord(drawUniformVertexConstants, handle);
  }

  const DrawUniformPixelConstantsRecord*
  drawUniformPixelConstantsRecord(DrawUniformStageHandle handle) const noexcept {
    return drawUniformStageRecord(drawUniformPixelConstants, handle);
  }

  std::span<const u8> drawUniformVertexConstantsBytes(
      const DrawUniformVertexConstantsRecord& record) const noexcept {
    return drawUniformStageConstantsBytes(
        std::span<const u8>(drawUniformVertexConstantBytes.data(),
                            drawUniformVertexConstantBytes.size()),
        record.constants);
  }

  std::span<const u8> drawUniformPixelConstantsBytes(
      const DrawUniformPixelConstantsRecord& record) const noexcept {
    return drawUniformStageConstantsBytes(
        std::span<const u8>(drawUniformPixelConstantBytes.data(),
                            drawUniformPixelConstantBytes.size()),
        record.constants);
  }

  bool drawUniformVertexConstantsRecordMatches(
      const DrawUniformVertexConstantsRecord& record,
      const DrawUniformPayload& payload) const noexcept {
    return drawUniformVertexConstantsMatches(
        record, drawUniformVertexConstantsBytes(record), payload);
  }

  bool drawUniformPixelConstantsRecordMatches(
      const DrawUniformPixelConstantsRecord& record,
      const DrawUniformPayload& payload) const noexcept {
    return drawUniformPixelConstantsMatches(
        record, drawUniformPixelConstantsBytes(record), payload);
  }

  template <typename Record>
  bool drawUniformStageLookupReady(const std::vector<Record>& records,
                                   const std::vector<std::uint32_t>& heads,
                                   const std::vector<std::uint32_t>& tails,
                                   const std::vector<std::uint32_t>& next) const noexcept {
    return !heads.empty() &&
           heads.size() == tails.size() &&
           next.size() == records.size();
  }

  template <typename Record>
  void linkDrawUniformStageLookupEntry(const std::vector<Record>& records,
                                       std::vector<std::uint32_t>& heads,
                                       std::vector<std::uint32_t>& tails,
                                       std::vector<std::uint32_t>& next,
                                       std::uint32_t recordIndex) noexcept {
    const auto bucketIndex = detail::chunkSlotUniformLookupBucket(
        records[recordIndex].handle.hash, heads.size());
    const auto tailIndex = tails[bucketIndex];
    if (tailIndex == detail::kChunkSlotInvalidUniformIndex) {
      heads[bucketIndex] = recordIndex;
    } else {
      next[tailIndex] = recordIndex;
    }
    tails[bucketIndex] = recordIndex;
  }

  template <typename Record>
  void rebuildDrawUniformStageLookup(const std::vector<Record>& records,
                                     std::vector<std::uint32_t>& heads,
                                     std::vector<std::uint32_t>& tails,
                                     std::vector<std::uint32_t>& next,
                                     std::size_t bucketCount) {
    if (records.empty()) {
      heads.clear();
      tails.clear();
      next.clear();
      return;
    }

    if (bucketCount < detail::chunkSlotUniformLookupBucketCount(records.size())) {
      bucketCount = detail::chunkSlotUniformLookupBucketCount(records.size());
    }

    heads.assign(bucketCount, detail::kChunkSlotInvalidUniformIndex);
    tails.assign(bucketCount, detail::kChunkSlotInvalidUniformIndex);
    next.assign(records.size(), detail::kChunkSlotInvalidUniformIndex);
    for (std::size_t i = 0; i < records.size(); ++i) {
      linkDrawUniformStageLookupEntry(records, heads, tails, next,
                                      static_cast<std::uint32_t>(i));
    }
  }

  template <typename Record>
  void reserveDrawUniformStageLookup(const std::vector<Record>& records,
                                     std::vector<std::uint32_t>& heads,
                                     std::vector<std::uint32_t>& tails,
                                     std::vector<std::uint32_t>& next,
                                     std::size_t recordCount) {
    if (recordCount == 0) {
      return;
    }

    detail::chunkSlotReserveAtLeast(next, recordCount);
    auto bucketCount = detail::chunkSlotUniformLookupBucketCount(recordCount);
    if (bucketCount < heads.size()) {
      bucketCount = heads.size();
    }
    if (heads.size() < bucketCount ||
        heads.size() != tails.size() ||
        next.size() != records.size()) {
      rebuildDrawUniformStageLookup(records, heads, tails, next, bucketCount);
    }
  }

  template <typename Record>
  void appendDrawUniformStageLookup(const std::vector<Record>& records,
                                    std::vector<std::uint32_t>& heads,
                                    std::vector<std::uint32_t>& tails,
                                    std::vector<std::uint32_t>& next,
                                    std::uint32_t recordIndex) {
    if (recordIndex >= records.size()) {
      return;
    }

    if (heads.empty() || heads.size() != tails.size() ||
        next.size() != recordIndex) {
      rebuildDrawUniformStageLookup(
          records, heads, tails, next,
          detail::chunkSlotUniformLookupBucketCount(records.size()));
      return;
    }

    next.push_back(detail::kChunkSlotInvalidUniformIndex);
    linkDrawUniformStageLookupEntry(records, heads, tails, next, recordIndex);
  }

  DrawUniformFixedHandle
  findDrawUniformFixedPayload(const DrawUniformPayload& payload) noexcept {
    const auto fixedPayload = makeDrawUniformFixedPayload(payload);
    return findDrawUniformFixedPayload(fixedPayload, payload.fixedPayloadHash);
  }

  DrawUniformFixedHandle
  findDrawUniformFixedPayload(const DrawUniformFixedPayload& payload,
                              u64 fixedPayloadHash) noexcept {
    if (const auto* record = drawUniformFixedPayloadRecord(lastUniformFixedHandle)) {
      if (drawUniformFixedPayloadMatches(*record, payload, fixedPayloadHash)) {
        return record->handle;
      }
    }
    for (const auto& record : drawUniformFixedPayloads) {
      if (drawUniformFixedPayloadMatches(record, payload, fixedPayloadHash)) {
        lastUniformFixedHandle = record.handle;
        return record.handle;
      }
    }
    return {};
  }

  DrawUniformFixedHandle
  appendDrawUniformFixedPayload(const DrawUniformPayload& payload) {
    const auto fixedPayload = makeDrawUniformFixedPayload(payload);
    return appendDrawUniformFixedPayload(fixedPayload, payload.fixedPayloadHash);
  }

  DrawUniformFixedHandle
  appendDrawUniformFixedPayload(const DrawUniformFixedPayload& payload,
                                u64 fixedPayloadHash) {
    detail::ChunkSlotPerfScope appendScope(
        dxmt9::perf::countDrawUniformPayloadAppendFixedAppendCpuTime);
    const bool canUseUniformSoA =
        detail::chunkSlotCanAppendU32IndexedElement(drawUniformFixedPayloads.size());
    DXMT_ASSERT(canUseUniformSoA &&
                "draw uniform fixed payload storage exceeded 32-bit range storage");
    if (!canUseUniformSoA) {
      return {};
    }

    const auto fixedIndex =
        static_cast<std::uint32_t>(drawUniformFixedPayloads.size());
    const auto fixedHandle =
        detail::chunkSlotUniformFixedHandle(fixedIndex, fixedPayloadHash);
    if (dxmt9::perf::enabled()) {
      dxmt9::perf::countDrawUniformFixedPayloadAppend();
      dxmt9::perf::countDrawUniformFixedPayloadAppendBytes(
          sizeof(DrawUniformFixedPayloadRecord));
      dxmt9::perf::countDrawUniformPayloadAppendBytes(
          sizeof(DrawUniformFixedPayloadRecord));
    }
    drawUniformFixedPayloads.push_back(DrawUniformFixedPayloadRecord{
        .handle = fixedHandle,
        .payload = payload,
    });
    lastUniformFixedHandle = fixedHandle;
    return fixedHandle;
  }

  DrawUniformStageHandle
  findDrawUniformVertexConstants(const DrawUniformPayload& payload) noexcept {
    if (const auto* record =
            drawUniformVertexConstantsRecord(lastUniformVertexConstantsHandle)) {
      if (drawUniformVertexConstantsRecordMatches(*record, payload)) {
        return record->handle;
      }
    }

    if (drawUniformStageLookupReady(drawUniformVertexConstants,
                                    drawUniformVertexConstantsLookupHeads,
                                    drawUniformVertexConstantsLookupTails,
                                    drawUniformVertexConstantsLookupNext)) {
      const auto bucketIndex = detail::chunkSlotUniformLookupBucket(
          payload.vertexConstantsHash, drawUniformVertexConstantsLookupHeads.size());
      auto recordIndex = drawUniformVertexConstantsLookupHeads[bucketIndex];
      for (std::size_t visited = 0;
           recordIndex != detail::kChunkSlotInvalidUniformIndex &&
           visited < drawUniformVertexConstants.size();
           ++visited) {
        if (recordIndex >= drawUniformVertexConstants.size()) {
          break;
        }
        const auto& record = drawUniformVertexConstants[recordIndex];
        if (drawUniformVertexConstantsRecordMatches(record, payload)) {
          lastUniformVertexConstantsHandle = record.handle;
          return record.handle;
        }
        recordIndex = drawUniformVertexConstantsLookupNext[recordIndex];
      }
      return {};
    }

    for (const auto& record : drawUniformVertexConstants) {
      if (drawUniformVertexConstantsRecordMatches(record, payload)) {
        lastUniformVertexConstantsHandle = record.handle;
        return record.handle;
      }
    }
    return {};
  }

  DrawUniformStageHandle
  findDrawUniformPixelConstants(const DrawUniformPayload& payload) noexcept {
    if (const auto* record =
            drawUniformPixelConstantsRecord(lastUniformPixelConstantsHandle)) {
      if (drawUniformPixelConstantsRecordMatches(*record, payload)) {
        return record->handle;
      }
    }

    if (drawUniformStageLookupReady(drawUniformPixelConstants,
                                    drawUniformPixelConstantsLookupHeads,
                                    drawUniformPixelConstantsLookupTails,
                                    drawUniformPixelConstantsLookupNext)) {
      const auto bucketIndex = detail::chunkSlotUniformLookupBucket(
          payload.pixelConstantsHash, drawUniformPixelConstantsLookupHeads.size());
      auto recordIndex = drawUniformPixelConstantsLookupHeads[bucketIndex];
      for (std::size_t visited = 0;
           recordIndex != detail::kChunkSlotInvalidUniformIndex &&
           visited < drawUniformPixelConstants.size();
           ++visited) {
        if (recordIndex >= drawUniformPixelConstants.size()) {
          break;
        }
        const auto& record = drawUniformPixelConstants[recordIndex];
        if (drawUniformPixelConstantsRecordMatches(record, payload)) {
          lastUniformPixelConstantsHandle = record.handle;
          return record.handle;
        }
        recordIndex = drawUniformPixelConstantsLookupNext[recordIndex];
      }
      return {};
    }

    for (const auto& record : drawUniformPixelConstants) {
      if (drawUniformPixelConstantsRecordMatches(record, payload)) {
        lastUniformPixelConstantsHandle = record.handle;
        return record.handle;
      }
    }
    return {};
  }

  template <std::size_t FloatCount>
  bool appendDrawUniformStageConstantsBytes(
      std::vector<u8>& arena,
      const ShaderConstantSnapshot<FloatCount>& constants,
      DrawUniformStageConstantsSpan& span,
      bool alreadyReserved = false) {
    if (!detail::chunkSlotCanAppendU32Range(arena.size(), span.byteSize)) {
      return false;
    }
    span.byteOffset = static_cast<std::uint32_t>(arena.size());
    if (span.byteSize == 0) {
      return true;
    }
    const auto oldSize = arena.size();
    if (!alreadyReserved) {
      detail::chunkSlotReserveAtLeast(arena, oldSize + span.byteSize);
    }
    arena.resize(oldSize + span.byteSize);
    auto* cursor = arena.data() + oldSize;

    const auto floatBytes =
        static_cast<std::size_t>(span.floatCount) * sizeof(constants.float4[0]);
    if (floatBytes != 0) {
      std::memcpy(cursor, constants.float4.data(), floatBytes);
      cursor += floatBytes;
    }

    const auto intBytes =
        static_cast<std::size_t>(span.intCount) * sizeof(constants.int4[0]);
    if (intBytes != 0) {
      std::memcpy(cursor, constants.int4.data(), intBytes);
      cursor += intBytes;
    }

    const auto boolBytes =
        static_cast<std::size_t>(span.boolCount) * sizeof(constants.bools[0]);
    if (boolBytes != 0) {
      std::memcpy(cursor, constants.bools.data(), boolBytes);
    }
    return true;
  }

  DrawUniformStageHandle
  appendDrawUniformVertexConstants(const DrawUniformPayload& payload,
                                   bool alreadyReserved = false) {
    detail::ChunkSlotPerfScope appendScope(
        dxmt9::perf::countDrawUniformPayloadAppendVertexAppendCpuTime);
    const bool canUseUniformSoA =
        detail::chunkSlotCanAppendU32IndexedElement(drawUniformVertexConstants.size());
    DXMT_ASSERT(canUseUniformSoA &&
                "draw uniform vertex-constant storage exceeded 32-bit range storage");
    if (!canUseUniformSoA) {
      return {};
    }

    const auto recordIndex =
        static_cast<std::uint32_t>(drawUniformVertexConstants.size());
    const auto handle = detail::chunkSlotUniformStageHandle(
        recordIndex, payload.vertexConstantsHash);
    auto constantsSpan = makeDrawUniformVertexConstantsSpan(payload, 0u);
    if (!appendDrawUniformStageConstantsBytes(
            drawUniformVertexConstantBytes, payload.vsConst, constantsSpan,
            alreadyReserved)) {
      return {};
    }
    const auto appendBytes =
        sizeof(DrawUniformVertexConstantsRecord) + constantsSpan.byteSize;
    if (dxmt9::perf::enabled()) {
      dxmt9::perf::countDrawUniformVertexConstantsAppend();
      dxmt9::perf::countDrawUniformVertexConstantsAppendBytes(appendBytes);
      dxmt9::perf::countDrawUniformPayloadAppendBytes(appendBytes);
    }
    if (!alreadyReserved) {
      reserveDrawUniformStageLookup(
          drawUniformVertexConstants, drawUniformVertexConstantsLookupHeads,
          drawUniformVertexConstantsLookupTails,
          drawUniformVertexConstantsLookupNext,
          drawUniformVertexConstants.size() + 1u);
    }
    drawUniformVertexConstants.push_back(DrawUniformVertexConstantsRecord{
        .handle = handle,
        .constants = constantsSpan,
    });
    appendDrawUniformStageLookup(drawUniformVertexConstants,
                                 drawUniformVertexConstantsLookupHeads,
                                 drawUniformVertexConstantsLookupTails,
                                 drawUniformVertexConstantsLookupNext,
                                 recordIndex);
    lastUniformVertexConstantsHandle = handle;
    return handle;
  }

  DrawUniformStageHandle
  appendDrawUniformPixelConstants(const DrawUniformPayload& payload,
                                  bool alreadyReserved = false) {
    detail::ChunkSlotPerfScope appendScope(
        dxmt9::perf::countDrawUniformPayloadAppendPixelAppendCpuTime);
    const bool canUseUniformSoA =
        detail::chunkSlotCanAppendU32IndexedElement(drawUniformPixelConstants.size());
    DXMT_ASSERT(canUseUniformSoA &&
                "draw uniform pixel-constant storage exceeded 32-bit range storage");
    if (!canUseUniformSoA) {
      return {};
    }

    const auto recordIndex =
        static_cast<std::uint32_t>(drawUniformPixelConstants.size());
    const auto handle = detail::chunkSlotUniformStageHandle(
        recordIndex, payload.pixelConstantsHash);
    auto constantsSpan = makeDrawUniformPixelConstantsSpan(payload, 0u);
    if (!appendDrawUniformStageConstantsBytes(
            drawUniformPixelConstantBytes, payload.psConst, constantsSpan,
            alreadyReserved)) {
      return {};
    }
    const auto appendBytes =
        sizeof(DrawUniformPixelConstantsRecord) + constantsSpan.byteSize;
    if (dxmt9::perf::enabled()) {
      dxmt9::perf::countDrawUniformPixelConstantsAppend();
      dxmt9::perf::countDrawUniformPixelConstantsAppendBytes(appendBytes);
      dxmt9::perf::countDrawUniformPayloadAppendBytes(appendBytes);
    }
    if (!alreadyReserved) {
      reserveDrawUniformStageLookup(
          drawUniformPixelConstants, drawUniformPixelConstantsLookupHeads,
          drawUniformPixelConstantsLookupTails,
          drawUniformPixelConstantsLookupNext,
          drawUniformPixelConstants.size() + 1u);
    }
    drawUniformPixelConstants.push_back(DrawUniformPixelConstantsRecord{
        .handle = handle,
        .constants = constantsSpan,
    });
    appendDrawUniformStageLookup(drawUniformPixelConstants,
                                 drawUniformPixelConstantsLookupHeads,
                                 drawUniformPixelConstantsLookupTails,
                                 drawUniformPixelConstantsLookupNext,
                                 recordIndex);
    lastUniformPixelConstantsHandle = handle;
    return handle;
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

  // R-BACK-2.104: size the uniform lookup bucket tables for a PROVISIONED
  // capacity while the slot is still empty.
  //
  // `reserveDrawUniformPayloadLookup` / `reserveDrawUniformStageLookup` cannot
  // do this: both delegate to a rebuild whose empty-records branch clears
  // heads/tails/next, so on an empty slot they provision nothing and the first
  // append then sizes the tables to that first source alone. Every following
  // adjacent source is then capacity-rejected on the lookup arm of
  // `directContinuationAdmission` even though every record vector has room.
  //
  // Growing the tables later is not an option -- it would rehash a published
  // prefix -- so they are assigned here, once, at the provisioned bucket count.
  // `next` keeps size 0 (it must stay equal to the record count) and only
  // takes capacity, so the append path's `next.size() != recordIndex` guard
  // never fires and never rebuilds the tables back down.
  //
  // Callers must have verified the slot is empty in every direct dimension.
  void provisionEmptyDrawUniformLookup(std::size_t payloadCount,
                                       std::size_t vertexCount,
                                       std::size_t pixelCount) {
    const auto provision = [](std::vector<std::uint32_t>& heads,
                              std::vector<std::uint32_t>& tails,
                              std::vector<std::uint32_t>& next,
                              std::size_t count) {
      if (count == 0) {
        return;
      }
      const auto bucketCount = detail::chunkSlotUniformLookupBucketCount(count);
      if (heads.size() < bucketCount) {
        heads.assign(bucketCount, detail::kChunkSlotInvalidUniformIndex);
        tails.assign(bucketCount, detail::kChunkSlotInvalidUniformIndex);
      } else {
        heads.assign(heads.size(), detail::kChunkSlotInvalidUniformIndex);
        tails.assign(heads.size(), detail::kChunkSlotInvalidUniformIndex);
      }
      next.clear();
      detail::chunkSlotReserveAtLeast(next, count);
    };
    if (!drawUniformPayloads.empty() || !drawUniformVertexConstants.empty() ||
        !drawUniformPixelConstants.empty()) {
      return;
    }
    provision(drawUniformPayloadLookupHeads, drawUniformPayloadLookupTails,
              drawUniformPayloadLookupNext, payloadCount);
    provision(drawUniformVertexConstantsLookupHeads,
              drawUniformVertexConstantsLookupTails,
              drawUniformVertexConstantsLookupNext, vertexCount);
    provision(drawUniformPixelConstantsLookupHeads,
              drawUniformPixelConstantsLookupTails,
              drawUniformPixelConstantsLookupNext, pixelCount);
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
    detail::ChunkSlotPerfScope lookupScope(
        dxmt9::perf::countDrawUniformPayloadLookupCpuTime);
    const bool recordPerf = dxmt9::perf::enabled();
    bool sawSemanticHashFullMismatch = false;
    auto payloadMatches = [&](const DrawUniformPayloadRecord& record) noexcept {
      if (record.handle.hash != payload.hash) {
        return false;
      }
      const auto* fixedRecord = drawUniformFixedPayloadRecord(record.fixedHandle);
      const auto* vertexRecord =
          drawUniformVertexConstantsRecord(record.vertexConstantsHandle);
      const auto* pixelRecord =
          drawUniformPixelConstantsRecord(record.pixelConstantsHandle);
      if (fixedRecord && vertexRecord && pixelRecord &&
          drawUniformPayloadRecordMatches(
              record, *fixedRecord,
              *vertexRecord, drawUniformVertexConstantsBytes(*vertexRecord),
              *pixelRecord, drawUniformPixelConstantsBytes(*pixelRecord),
              payload)) {
        return true;
      }
      sawSemanticHashFullMismatch = true;
      return false;
    };
    auto countSemanticHashMiss = [&]() noexcept {
      if (recordPerf && sawSemanticHashFullMismatch) {
        dxmt9::perf::countDrawUniformPayloadLookupSemanticHashMiss(
            sizeof(DrawUniformPayloadRecord));
      }
    };
    if (const auto* record = drawUniformPayloadRecord(candidate)) {
      if (payloadMatches(*record)) {
        if (recordPerf) {
          dxmt9::perf::countDrawUniformPayloadLookupCandidateHit();
        }
        lastUniformHandle = record->handle;
        return record->handle;
      }
    }

    if (const auto* record = drawUniformPayloadRecord(lastUniformHandle)) {
      if (payloadMatches(*record)) {
        if (recordPerf) {
          dxmt9::perf::countDrawUniformPayloadLookupLastHit();
        }
        return record->handle;
      }
    }

    if (drawUniformPayloadLookupReady()) {
      detail::ChunkSlotPerfScope bucketScope(
          dxmt9::perf::countDrawUniformPayloadLookupBucketCpuTime);
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
        if (payloadMatches(record)) {
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
        countSemanticHashMiss();
        return {};
      }
    }

    for (std::size_t i = 0; i < drawUniformPayloads.size(); ++i) {
      const auto& record = drawUniformPayloads[i];
      if (payloadMatches(record)) {
        if (recordPerf) {
          dxmt9::perf::countDrawUniformPayloadLookupLinearHit();
        }
        lastUniformHandle = record.handle;
        return record.handle;
      }
    }
    countSemanticHashMiss();
    return {};
  }

  DrawUniformHandle appendDrawUniformPayload(
      const DrawUniformPayload& payload,
      DrawUniformFixedHandle fixedHandleCandidate = {},
      bool alreadyReserved = false) {
    const bool canUseUniformSoA =
        detail::chunkSlotCanAppendU32IndexedElement(drawUniformPayloads.size()) &&
        detail::chunkSlotCanAppendU32IndexedElement(drawUniformFixedPayloads.size()) &&
        detail::chunkSlotCanAppendU32IndexedElement(drawUniformVertexConstants.size()) &&
        detail::chunkSlotCanAppendU32IndexedElement(drawUniformPixelConstants.size());
    DXMT_ASSERT(canUseUniformSoA && "draw uniform payload storage exceeded 32-bit range storage");
    if (!canUseUniformSoA) {
      return {};
    }

    DrawUniformFixedHandle fixedHandle = fixedHandleCandidate;
    if (fixedHandle.valid()) {
      const auto* record = drawUniformFixedPayloadRecord(fixedHandle);
      if (!record || record->handle.hash != payload.fixedPayloadHash) {
        fixedHandle = {};
      }
    }
    if (!fixedHandle.valid() &&
        !detail::chunkSlotDisableDrawUniformPayloadDedup()) {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countDrawUniformPayloadAppendFixedFindCpuTime);
      fixedHandle = findDrawUniformFixedPayload(payload);
    }
    if (!fixedHandle.valid()) {
      fixedHandle = appendDrawUniformFixedPayload(payload);
      if (!fixedHandle.valid()) {
        return {};
      }
    }

    DrawUniformStageHandle vertexConstantsHandle{};
    if (!detail::chunkSlotDisableDrawUniformPayloadDedup()) {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countDrawUniformPayloadAppendVertexFindCpuTime);
      vertexConstantsHandle = findDrawUniformVertexConstants(payload);
    }
    if (!vertexConstantsHandle.valid()) {
      vertexConstantsHandle = appendDrawUniformVertexConstants(
          payload, alreadyReserved);
      if (!vertexConstantsHandle.valid()) {
        return {};
      }
    }

    DrawUniformStageHandle pixelConstantsHandle{};
    if (!detail::chunkSlotDisableDrawUniformPayloadDedup()) {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countDrawUniformPayloadAppendPixelFindCpuTime);
      pixelConstantsHandle = findDrawUniformPixelConstants(payload);
    }
    if (!pixelConstantsHandle.valid()) {
      pixelConstantsHandle = appendDrawUniformPixelConstants(
          payload, alreadyReserved);
      if (!pixelConstantsHandle.valid()) {
        return {};
      }
    }

    const auto uniformIndex = static_cast<std::uint32_t>(drawUniformPayloads.size());
    const auto uniformHandle = detail::chunkSlotUniformHandle(uniformIndex, payload.hash);
    if (dxmt9::perf::enabled()) {
      dxmt9::perf::countDrawUniformPayloadAppend();
      dxmt9::perf::countDrawUniformPayloadAppendBytes(sizeof(DrawUniformPayloadRecord));
    }
    {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countDrawUniformPayloadAppendReserveCpuTime);
      if (!alreadyReserved &&
          !detail::chunkSlotDisableDrawUniformPayloadDedup()) {
        reserveDrawUniformPayloadLookup(drawUniformPayloads.size() + 1u);
      }
    }
    {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countDrawUniformPayloadAppendCopyCpuTime);
      drawUniformPayloads.emplace_back(uniformHandle, fixedHandle,
                                       vertexConstantsHandle,
                                       pixelConstantsHandle, payload);
    }
    {
      detail::ChunkSlotPerfScope scope(
          dxmt9::perf::countDrawUniformPayloadAppendLinkCpuTime);
      if (!detail::chunkSlotDisableDrawUniformPayloadDedup()) {
        appendDrawUniformPayloadLookup(uniformIndex);
      }
    }
    lastUniformHandle = uniformHandle;
    return uniformHandle;
  }

  bool canAppendDrawRun(std::size_t drawCount, std::size_t payloadBytes,
                        bool needsUniformAppend) const noexcept {
    return drawStateStorageConsistent() &&
           detail::chunkSlotCanAppendU32IndexedElement(drawHotStates.size()) &&
           (!needsUniformAppend ||
            (detail::chunkSlotCanAppendU32IndexedElement(drawUniformPayloads.size()) &&
             detail::chunkSlotCanAppendU32IndexedElement(drawUniformFixedPayloads.size()) &&
             detail::chunkSlotCanAppendU32IndexedElement(drawUniformVertexConstants.size()) &&
             detail::chunkSlotCanAppendU32IndexedElement(drawUniformPixelConstants.size()) &&
             detail::chunkSlotCanAppendU32Range(drawUniformVertexConstantBytes.size(),
                                                sizeof(VertexShaderConstants)) &&
             detail::chunkSlotCanAppendU32Range(drawUniformPixelConstantBytes.size(),
                                                sizeof(PixelShaderConstants)))) &&
           detail::chunkSlotCanAppendU32Range(drawParams.size(), drawCount) &&
           detail::chunkSlotCanAppendU32Range(drawPayloadArena.size(), payloadBytes);
  }

  bool canAppendDrawRunBatch(std::size_t drawCount, std::size_t payloadBytes,
                             std::size_t uniformCount) const noexcept {
    return drawStateStorageConsistent() &&
           detail::chunkSlotCanAppendU32IndexedElement(drawHotStates.size()) &&
           detail::chunkSlotCanAppendU32Range(drawUniformPayloads.size(), uniformCount) &&
           detail::chunkSlotCanAppendU32Range(drawUniformFixedPayloads.size(), uniformCount) &&
           detail::chunkSlotCanAppendU32Range(drawUniformVertexConstants.size(), uniformCount) &&
           detail::chunkSlotCanAppendU32Range(drawUniformPixelConstants.size(), uniformCount) &&
           detail::chunkSlotCanAppendU32Range(
               drawUniformVertexConstantBytes.size(),
               uniformCount * sizeof(VertexShaderConstants)) &&
           detail::chunkSlotCanAppendU32Range(
               drawUniformPixelConstantBytes.size(),
               uniformCount * sizeof(PixelShaderConstants)) &&
           detail::chunkSlotCanAppendU32Range(drawParams.size(), drawCount) &&
           detail::chunkSlotCanAppendU32Range(drawPayloadArena.size(), payloadBytes);
  }

  static DrawPsoSubview makeDrawPsoSubview(
      const FlatDrawStateRecord& hot,
      const DrawShaderLayoutContext& shaderLayout) noexcept {
    DrawPsoSubview view{};
    const auto& key = hot.key;
    view.hasShaderContext =
        shaderLayout.vertexShader.kind != ShaderRef::Kind::None ||
        shaderLayout.pixelShader.kind != ShaderRef::Kind::None;
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

  static DrawPsoSubview makeDrawPsoSubview(
      const CanonicalDrawState& state) noexcept {
    return makeDrawPsoSubview(state.hot, state.shaderLayout);
  }

  static DrawRunInvariant makeDrawRunInvariant(
      const FlatDrawStateRecord& hot) noexcept {
    return DrawRunInvariant{
        .viewportScissorHash = hot.key.viewportHash,
        .runStableBindingHash =
            hot.key.renderStateHash ^ (hot.key.vertexDeclHash << 1) ^
            (static_cast<u64>(hot.textureMask) << 2) ^
            (static_cast<u64>(hot.key.samplerStateMask) << 3),
        .streamMask = hot.streamMask,
        .textureMask = hot.textureMask,
        .samplerStateMask = hot.key.samplerStateMask,
    };
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

    DrawUniformHandle uniformHandle =
        detail::chunkSlotDisableDrawUniformPayloadDedup()
            ? DrawUniformHandle{}
            : findDrawUniformPayload(uniformPayload, uniformHandleCandidate);
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
      detail::chunkSlotReserveAtLeast(drawUniformFixedPayloads,
                                      drawUniformFixedPayloads.size() + 1u);
      detail::chunkSlotReserveAtLeast(drawUniformVertexConstants,
                                      drawUniformVertexConstants.size() + 1u);
      detail::chunkSlotReserveAtLeast(drawUniformPixelConstants,
                                      drawUniformPixelConstants.size() + 1u);
      detail::chunkSlotReserveAtLeast(drawUniformPayloads, drawUniformPayloads.size() + 1u);
      if (!detail::chunkSlotDisableDrawUniformPayloadDedup()) {
        reserveDrawUniformPayloadLookup(drawUniformPayloads.size() + 1u);
        reserveDrawUniformStageLookup(drawUniformVertexConstants,
                                      drawUniformVertexConstantsLookupHeads,
                                      drawUniformVertexConstantsLookupTails,
                                      drawUniformVertexConstantsLookupNext,
                                      drawUniformVertexConstants.size() + 1u);
        reserveDrawUniformStageLookup(drawUniformPixelConstants,
                                      drawUniformPixelConstantsLookupHeads,
                                      drawUniformPixelConstantsLookupTails,
                                      drawUniformPixelConstantsLookupNext,
                                      drawUniformPixelConstants.size() + 1u);
      }
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
      if (!param.uniformHandle.valid()) {
        param.uniformHandle = uniformHandle;
      }
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

  // Direct replay transfers an already-owned ClearDesc after reservation so
  // copying its nested rect vector cannot allocate on the reserved path.
  void appendClear(ClearDesc&& clear) {
    appendCommandRecord(MetalCommandKind::Clear, clearRecords,
                        std::move(clear));
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

  void appendGenerateMipmaps(const GenerateMipmapsDesc& generateMipmaps) {
    appendCommandRecord(MetalCommandKind::GenerateMipmaps,
                        generateMipmapsRecords, generateMipmaps);
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
    case MetalCommandKind::GenerateMipmaps: {
      const auto payloadIndex = detail::chunkSlotPayloadIndex<
          GenerateMipmapsRecordIndex>(header.payloadIndex);
      if (detail::chunkSlotIndexInRange(payloadIndex,
                                       generateMipmapsRecords)) {
        view.generateMipmaps = &generateMipmapsRecords[payloadIndex.value];
      }
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
    if (!drawUniformFixedPayloads.empty()) {
      view.drawUniformFixedPayloadRecords =
          std::span<const DrawUniformFixedPayloadRecord>(
              drawUniformFixedPayloads.data(), drawUniformFixedPayloads.size());
    }
    if (!drawUniformVertexConstants.empty()) {
      view.drawUniformVertexConstantsRecords =
          std::span<const DrawUniformVertexConstantsRecord>(
              drawUniformVertexConstants.data(), drawUniformVertexConstants.size());
    }
    if (!drawUniformVertexConstantBytes.empty()) {
      view.drawUniformVertexConstantBytes =
          std::span<const u8>(drawUniformVertexConstantBytes.data(),
                              drawUniformVertexConstantBytes.size());
    }
    if (!drawUniformPixelConstants.empty()) {
      view.drawUniformPixelConstantsRecords =
          std::span<const DrawUniformPixelConstantsRecord>(
              drawUniformPixelConstants.data(), drawUniformPixelConstants.size());
    }
    if (!drawUniformPixelConstantBytes.empty()) {
      view.drawUniformPixelConstantBytes =
          std::span<const u8>(drawUniformPixelConstantBytes.data(),
                              drawUniformPixelConstantBytes.size());
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

  bool commandPayloadsInRange() const noexcept {
    if (!drawStateStorageConsistent()) {
      return false;
    }
    for (const auto& header : commandHeaders) {
      switch (header.kind) {
      case MetalCommandKind::DrawRun: {
        const auto payloadIndex =
            detail::chunkSlotPayloadIndex<DrawRunRecordIndex>(header.payloadIndex);
        if (!detail::chunkSlotIndexInRange(payloadIndex, drawRunRecords) ||
            payloadIndex.value >= drawPsoSubviews.size()) {
          return false;
        }
        const auto& record = drawRunRecords[payloadIndex.value];
        if (record.stateIndex >= drawHotStates.size() ||
            record.stateIndex >= drawShaderLayouts.size() ||
            record.stateIndex >= drawDebugSnapshots.size() ||
            record.firstParam > drawParams.size() ||
            record.paramCount > drawParams.size() - record.firstParam ||
            record.payloadOffset > drawPayloadArena.size() ||
            record.payloadSize > drawPayloadArena.size() - record.payloadOffset ||
            (record.uniformHandle.valid() &&
             !drawUniformPayloadRecord(record.uniformHandle))) {
          return false;
        }
        for (std::size_t i = 0; i < record.paramCount; ++i) {
          const auto& param = drawParams[record.firstParam + i];
          if ((param.uniformHandle.valid() &&
               !drawUniformPayloadRecord(param.uniformHandle)) ||
              !drawPayloadRangeFits(record.payloadSize, param.userVertexRange) ||
              !drawPayloadRangeFits(record.payloadSize, param.userIndexRange) ||
              !drawPayloadRangeFits(record.payloadSize, param.bindingOverrideRange) ||
              !drawPayloadRangeFits(record.payloadSize, param.bindingSnapshotRange)) {
            return false;
          }
        }
        break;
      }
      case MetalCommandKind::Clear: {
        const auto payloadIndex =
            detail::chunkSlotPayloadIndex<ClearRecordIndex>(header.payloadIndex);
        if (!detail::chunkSlotIndexInRange(payloadIndex, clearRecords)) {
          return false;
        }
        break;
      }
      case MetalCommandKind::SurfaceCopy: {
        const auto payloadIndex =
            detail::chunkSlotPayloadIndex<SurfaceCopyRecordIndex>(header.payloadIndex);
        if (!detail::chunkSlotIndexInRange(payloadIndex, surfaceCopyRecords)) {
          return false;
        }
        break;
      }
      case MetalCommandKind::StretchRect: {
        const auto payloadIndex =
            detail::chunkSlotPayloadIndex<StretchRectRecordIndex>(header.payloadIndex);
        if (!detail::chunkSlotIndexInRange(payloadIndex, stretchRectRecords)) {
          return false;
        }
        break;
      }
      case MetalCommandKind::Readback: {
        const auto payloadIndex =
            detail::chunkSlotPayloadIndex<ReadbackRecordIndex>(header.payloadIndex);
        if (!detail::chunkSlotIndexInRange(payloadIndex, readbackRecords)) {
          return false;
        }
        break;
      }
      case MetalCommandKind::ColorFill: {
        const auto payloadIndex =
            detail::chunkSlotPayloadIndex<ColorFillRecordIndex>(header.payloadIndex);
        if (!detail::chunkSlotIndexInRange(payloadIndex, colorFillRecords)) {
          return false;
        }
        break;
      }
      case MetalCommandKind::DepthResolve: {
        const auto payloadIndex =
            detail::chunkSlotPayloadIndex<DepthResolveRecordIndex>(header.payloadIndex);
        if (!detail::chunkSlotIndexInRange(payloadIndex, depthResolveRecords)) {
          return false;
        }
        break;
      }
      case MetalCommandKind::GenerateMipmaps: {
        const auto payloadIndex = detail::chunkSlotPayloadIndex<
            GenerateMipmapsRecordIndex>(header.payloadIndex);
        if (!detail::chunkSlotIndexInRange(payloadIndex,
                                          generateMipmapsRecords)) {
          return false;
        }
        break;
      }
      case MetalCommandKind::Present: {
        const auto payloadIndex =
            detail::chunkSlotPayloadIndex<PresentRecordIndex>(header.payloadIndex);
        if (!detail::chunkSlotIndexInRange(payloadIndex, presentRecords)) {
          return false;
        }
        break;
      }
      }
    }
    return true;
  }

  static bool drawPayloadRangeFits(std::uint32_t payloadSize,
                                   DrawPayloadRange range) noexcept {
    return range.offset <= payloadSize && range.size <= payloadSize - range.offset;
  }
};

}  // namespace dxmt9::core
