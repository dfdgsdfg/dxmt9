#include "dxmt9_queue.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_compat.hpp"
#include "dxmt9_hud.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_signposts.hpp"
#include "../winemetal/Metal.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <thread>

namespace dxmt9::core::metalqueue {

namespace {

using enum dxmt9::core::metalcompat::CompatFlagBits;

u32 compatFlagsForSurface(const std::function<u32(Handle)>& resolveSurfaceFlags, Handle handle) {
  if (!handle || !resolveSurfaceFlags) {
    return 0;
  }
  return resolveSurfaceFlags(handle);
}

u32 compatFlagsForDraw(FlatDrawStateView draw, const std::function<u32(Handle)>& resolveSurfaceFlags) {
  const auto& hot = *draw.hot;
  u32 flags = 0;
  u32 colorTargets = 0;
  for (const auto& attachment : hot.colorAttachments) {
    if (!attachment.handle) {
      continue;
    }
    ++colorTargets;
    flags |= compatFlagsForSurface(resolveSurfaceFlags, attachment.handle);
    if (attachment.sampleCount > 1) {
      flags |= CompatFlagMsaa;
    }
  }
  if (colorTargets > 1) {
    flags |= CompatFlagMrt;
  }
  if (hot.depthStencil.handle && hot.depthStencil.sampleCount > 1) {
    flags |= CompatFlagMsaa;
  }
  if (flatStateOr(hot.renderStates, RS_SRGB_WRITE_ENABLE, 0u) != 0) {
    flags |= CompatFlagSrgb;
  }
  for (const auto& sampler : hot.samplerStates) {
    if (flatStateOr(sampler, SAMP_SRGB_TEXTURE, 0u) != 0) {
      flags |= CompatFlagSrgb;
    }
  }
  if ((hot.nonIdentityTextureTransformStageMask & hot.textureMask) != 0) {
    flags |= CompatFlagProjected;
  }
  return flags;
}

u64 shaderVariantHashForDraw(FlatDrawStateView draw) {
  if (!draw.hot || !draw.hasShaderContext()) {
    return 0;
  }
  const auto& hot = *draw.hot;
  const auto& shader = draw.shaderContext();
  u64 hash = shader.vertexShader.hash ^ (shader.pixelShader.hash << 1) ^
             (hot.key.vertexDeclHash << 2) ^ hot.key.renderStateHash ^
             (hot.textureMask << 3) ^ (hot.renderTargetMask << 4);
  hash ^= hot.vertexConstantsHash << 1;
  hash ^= hot.pixelConstantsHash << 2;
  return hash;
}

std::uint32_t completionSignalDelayMs() noexcept {
  static const std::uint32_t delayMs = [] {
    const char* env = std::getenv("DXMT9_PERF_COMPLETION_SIGNAL_DELAY_MS");
    if (!env || env[0] == '\0') {
      return 0u;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(env, &end, 10);
    if (end == env || parsed == 0) {
      return 0u;
    }
    return static_cast<std::uint32_t>(std::min<unsigned long>(parsed, 250));
  }();
  return delayMs;
}

void delayCompletionSignalForPerfProbe() {
  const std::uint32_t delayMs = completionSignalDelayMs();
  if (delayMs == 0) {
    return;
  }
  const auto delay = std::chrono::milliseconds(delayMs);
  std::this_thread::sleep_for(delay);
  perf::countCompletionSignalDelay(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count()));
}

u32 compatFlagsForClear(const ClearDesc& clear, const std::function<u32(Handle)>& resolveSurfaceFlags) {
  u32 flags = 0;
  u32 colorTargets = 0;
  for (const auto& attachment : clear.colorAttachments) {
    if (!attachment.handle) {
      continue;
    }
    ++colorTargets;
    flags |= compatFlagsForSurface(resolveSurfaceFlags, attachment.handle);
    if (attachment.sampleCount > 1) {
      flags |= CompatFlagMsaa;
    }
  }
  if (colorTargets > 1) {
    flags |= CompatFlagMrt;
  }
  if (clear.depthStencil.handle && clear.depthStencil.sampleCount > 1) {
    flags |= CompatFlagMsaa;
  }
  return flags;
}

u32 compatFlagsForPresent(const SwapDesc& present,
                          Handle sourceHandle,
                          const std::function<u32(Handle)>& resolveSurfaceFlags) {
  u32 flags = compatFlagsForSurface(resolveSurfaceFlags, sourceHandle);
  if (present.multiSampleType != MultiSampleType::None) {
    flags |= CompatFlagMsaa;
  }
  return flags;
}

ChunkObservation makeChunkObservation(const MetalCommandView& command,
                                      const std::function<u32(Handle)>& resolveSurfaceFlags) {
  switch (command.kind) {
    case MetalCommandKind::DrawRun: {
      return ChunkObservation{
          .kind = ChunkObservationKind::Draw,
          .compatFlags = command.drawState.hot
              ? compatFlagsForDraw(command.drawState, resolveSurfaceFlags)
              : 0,
          .vertexShaderHash = command.drawState.hasShaderContext()
              ? command.drawState.shaderContext().vertexShader.hash
              : 0,
          .pixelShaderHash = command.drawState.hasShaderContext()
              ? command.drawState.shaderContext().pixelShader.hash
              : 0,
          .shaderVariantHash = shaderVariantHashForDraw(command.drawState),
      };
    }
    case MetalCommandKind::Clear:
      return ChunkObservation{
          .kind = ChunkObservationKind::Draw,
          .compatFlags = command.clear ? compatFlagsForClear(*command.clear, resolveSurfaceFlags) : 0,
      };
    case MetalCommandKind::SurfaceCopy:
    case MetalCommandKind::Readback:
    // R-FORMAT-11: RESZ depth resolve runs on a short-lived helper render
    // encoder with no draws — classify it as a Blit observation like the
    // other helper-encoder surface ops.
    case MetalCommandKind::DepthResolve:
      return ChunkObservation{
          .kind = ChunkObservationKind::Blit,
          .compatFlags = 0,
      };
    case MetalCommandKind::StretchRect:
      return ChunkObservation{
          .kind = ChunkObservationKind::StretchRect,
          .compatFlags = 0,
      };
    case MetalCommandKind::ColorFill:
      return ChunkObservation{
          .kind = ChunkObservationKind::Draw,
          .compatFlags = 0,
      };
    case MetalCommandKind::Present:
      return ChunkObservation{
          .kind = ChunkObservationKind::Present,
          .compatFlags = command.present
              ? compatFlagsForPresent(command.present->present, command.present->presentSource, resolveSurfaceFlags)
              : 0,
      };
  }
  return ChunkObservation{};
}

QueueLifecycleContext makeLifecycleContext(const QueueControllerState& state) {
  return QueueLifecycleContext{
      .writingSlot = state.writingSlot,
      .writeIndex = state.writeIndex,
      .readyCount = state.readyCount,
      .completedQueueCount = state.completedQueueCount,
      .inflightCount = state.inflightCount,
      .completedSeqId = state.completedSeqId,
      .lastCommittedSeqId = state.lastCommittedSeqId,
  };
}

QueueControllerState makeBoundQueueState(
    const QueueLifecycleController::SubmissionBinding& state) {
  return QueueControllerState{
      .writingSlot = state.writingSlot ? *state.writingSlot : std::optional<size_t>{},
      .writeIndex = state.writeIndex ? *state.writeIndex : 0,
      .readyCount = state.readySlots ? state.readySlots->size() : 0,
      .completedQueueCount = state.completedSeqQueue ? state.completedSeqQueue->size() : 0,
      .inflightCount = state.inflightCount ? *state.inflightCount : 0,
      .completedSeqId = state.completedSeqId ? *state.completedSeqId : 0,
      .lastCommittedSeqId = state.lastCommittedSeqId ? *state.lastCommittedSeqId : 0,
      .slots = state.slots,
  };
}

std::optional<ChunkSlot::State> slotStateFor(const QueueControllerState& state,
                                             std::optional<size_t> slotIndex) {
  if (!slotIndex.has_value() || *slotIndex >= state.slots.size()) {
    return std::nullopt;
  }
  return state.slots[*slotIndex].state;
}

size_t slotCommandCountFor(const QueueControllerState& state,
                           std::optional<size_t> slotIndex) {
  if (!slotIndex.has_value() || *slotIndex >= state.slots.size()) {
    return 0;
  }
  return state.slots[*slotIndex].commandCount();
}

bool writerCanProceed(const QueueControllerState& state,
                      std::optional<size_t> slotIndex,
                      size_t inflightLimit) {
  if (!slotIndex.has_value() || *slotIndex >= state.slots.size()) {
    return state.inflightCount < inflightLimit;
  }
  return state.slots[*slotIndex].state == ChunkSlot::State::Free &&
         state.inflightCount < inflightLimit;
}

void traceEncodeIterationStage(const char* stage, size_t slotIndex, const ChunkSlot& slot) {
  if (!queueTraceEnabled()) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-encode-iteration]"
      << " stage=" << stage
      << " seq=" << static_cast<unsigned long long>(slot.seqId)
      << " slot=" << slotIndex
      << " commands=" << slot.commandCount();
  emitQueueTraceLine(out.str());
}

bool envFlagEnabled(const char* name) noexcept {
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return false;
  }
  return !(env[0] == '0' && env[1] == '\0');
}

bool encodeReadySlotCoalescingEnabled() noexcept {
  static const bool enabled = [] {
    const char* explicitEnv = std::getenv("DXMT9_ENCODE_COALESCE_READY_SLOTS");
    if (explicitEnv && explicitEnv[0] != '\0') {
      return !(explicitEnv[0] == '0' && explicitEnv[1] == '\0');
    }
    return envFlagEnabled("DXMT9_OFFSCREEN_RUN_AHEAD");
  }();
  return enabled;
}

size_t encodeReadySlotCoalesceLimit() noexcept {
  static const size_t limit = [] {
    const char* env = std::getenv("DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT");
    if (!env || env[0] == '\0') {
      return static_cast<size_t>(4);
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(env, &end, 10);
    if (end == env || parsed < 2) {
      return static_cast<size_t>(1);
    }
    return static_cast<size_t>(std::min<unsigned long>(parsed, 16));
  }();
  return limit;
}

bool chunkBlocksEncodeCoalescing(const ChunkSlot& slot) noexcept {
  for (const auto& header : slot.commandHeaders) {
    if (header.kind == MetalCommandKind::Present ||
        header.kind == MetalCommandKind::Readback) {
      return true;
    }
  }
  return false;
}

void traceEncodeCoalesce(size_t primarySlotIndex,
                         size_t appendedSlotIndex,
                         u64 primarySeqId,
                         u64 appendedSeqId,
                         size_t commandCount) {
  if (!queueTraceEnabled()) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-encode-coalesce]"
      << " primary_slot=" << primarySlotIndex
      << " appended_slot=" << appendedSlotIndex
      << " primary_seq=" << static_cast<unsigned long long>(primarySeqId)
      << " appended_seq=" << static_cast<unsigned long long>(appendedSeqId)
      << " commands=" << commandCount;
  emitQueueTraceLine(out.str());
}

void traceCpuReadyStageHold(size_t readyDepth, size_t compatibleDepth) {
  if (!queueTraceEnabled()) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-cpu-ready-stage]"
      << " ready_depth=" << readyDepth
      << " compatible_depth=" << compatibleDepth;
  emitQueueTraceLine(out.str());
}

std::uint32_t checkedU32(std::size_t value) noexcept {
  DXMT_ASSERT(value <= std::numeric_limits<std::uint32_t>::max());
  return static_cast<std::uint32_t>(
      std::min<std::size_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

DrawUniformHandle remapUniformHandle(DrawUniformHandle handle,
                                     std::size_t base) noexcept {
  if (!handle.valid()) {
    return {};
  }
  const auto index = checkedU32(base + handle.index);
  return detail::chunkSlotUniformHandle(index, handle.hash);
}

DrawUniformFixedHandle remapUniformFixedHandle(DrawUniformFixedHandle handle,
                                               std::size_t base) noexcept {
  if (!handle.valid()) {
    return {};
  }
  const auto index = checkedU32(base + handle.index);
  return detail::chunkSlotUniformFixedHandle(index, handle.hash);
}

DrawUniformStageHandle remapUniformStageHandle(DrawUniformStageHandle handle,
                                               std::size_t base) noexcept {
  if (!handle.valid()) {
    return {};
  }
  const auto index = checkedU32(base + handle.index);
  return detail::chunkSlotUniformStageHandle(index, handle.hash);
}

void clearMergedSlotLookupState(ChunkSlot& slot) {
  slot.drawUniformPayloadLookupHeads.clear();
  slot.drawUniformPayloadLookupTails.clear();
  slot.drawUniformPayloadLookupNext.clear();
  slot.drawUniformVertexConstantsLookupHeads.clear();
  slot.drawUniformVertexConstantsLookupTails.clear();
  slot.drawUniformVertexConstantsLookupNext.clear();
  slot.drawUniformPixelConstantsLookupHeads.clear();
  slot.drawUniformPixelConstantsLookupTails.clear();
  slot.drawUniformPixelConstantsLookupNext.clear();
  slot.lastUniformFixedHandle = {};
  slot.lastUniformVertexConstantsHandle = {};
  slot.lastUniformPixelConstantsHandle = {};
  slot.lastUniformHandle = {};
  slot.pipelinePrefetchSealed = false;
  slot.pipelinePrefetchCommandCursor = 0;
}

struct ChunkSlotAppendBases {
  std::uint32_t drawRunRecords = 0;
  std::uint32_t clearRecords = 0;
  std::uint32_t surfaceCopyRecords = 0;
  std::uint32_t stretchRectRecords = 0;
  std::uint32_t readbackRecords = 0;
  std::uint32_t colorFillRecords = 0;
  std::uint32_t depthResolveRecords = 0;
  std::uint32_t presentRecords = 0;
};

std::uint32_t payloadBaseForKind(const ChunkSlotAppendBases& bases,
                                 MetalCommandKind kind) noexcept {
  switch (kind) {
    case MetalCommandKind::DrawRun:
      return bases.drawRunRecords;
    case MetalCommandKind::Clear:
      return bases.clearRecords;
    case MetalCommandKind::SurfaceCopy:
      return bases.surfaceCopyRecords;
    case MetalCommandKind::StretchRect:
      return bases.stretchRectRecords;
    case MetalCommandKind::Readback:
      return bases.readbackRecords;
    case MetalCommandKind::ColorFill:
      return bases.colorFillRecords;
    case MetalCommandKind::DepthResolve:
      return bases.depthResolveRecords;
    case MetalCommandKind::Present:
      return bases.presentRecords;
  }
  return 0;
}

template <typename T>
void appendRange(std::vector<T>& dst, const std::vector<T>& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

void appendChunkSlotForEncodeCoalescing(ChunkSlot& dst, const ChunkSlot& src) {
  if (src.commandHeaders.empty()) {
    return;
  }

  const ChunkSlotAppendBases bases{
      .drawRunRecords = checkedU32(dst.drawRunRecords.size()),
      .clearRecords = checkedU32(dst.clearRecords.size()),
      .surfaceCopyRecords = checkedU32(dst.surfaceCopyRecords.size()),
      .stretchRectRecords = checkedU32(dst.stretchRectRecords.size()),
      .readbackRecords = checkedU32(dst.readbackRecords.size()),
      .colorFillRecords = checkedU32(dst.colorFillRecords.size()),
      .depthResolveRecords = checkedU32(dst.depthResolveRecords.size()),
      .presentRecords = checkedU32(dst.presentRecords.size()),
  };

  const std::size_t drawStateBase = dst.drawHotStates.size();
  const std::size_t drawParamBase = dst.drawParams.size();
  const std::size_t drawPayloadBase = dst.drawPayloadArena.size();
  const std::size_t uniformFixedBase = dst.drawUniformFixedPayloads.size();
  const std::size_t uniformVertexBase = dst.drawUniformVertexConstants.size();
  const std::size_t uniformVertexByteBase = dst.drawUniformVertexConstantBytes.size();
  const std::size_t uniformPixelBase = dst.drawUniformPixelConstants.size();
  const std::size_t uniformPixelByteBase = dst.drawUniformPixelConstantBytes.size();
  const std::size_t uniformPayloadBase = dst.drawUniformPayloads.size();

  dst.commandHeaders.reserve(dst.commandHeaders.size() + src.commandHeaders.size());
  dst.drawHotStates.reserve(dst.drawHotStates.size() + src.drawHotStates.size());
  dst.drawShaderLayouts.reserve(dst.drawShaderLayouts.size() + src.drawShaderLayouts.size());
  dst.drawDebugSnapshots.reserve(dst.drawDebugSnapshots.size() + src.drawDebugSnapshots.size());
  dst.drawPsoSubviews.reserve(dst.drawPsoSubviews.size() + src.drawPsoSubviews.size());
  dst.drawUniformFixedPayloads.reserve(
      dst.drawUniformFixedPayloads.size() + src.drawUniformFixedPayloads.size());
  dst.drawUniformVertexConstants.reserve(
      dst.drawUniformVertexConstants.size() + src.drawUniformVertexConstants.size());
  dst.drawUniformVertexConstantBytes.reserve(
      dst.drawUniformVertexConstantBytes.size() + src.drawUniformVertexConstantBytes.size());
  dst.drawUniformPixelConstants.reserve(
      dst.drawUniformPixelConstants.size() + src.drawUniformPixelConstants.size());
  dst.drawUniformPixelConstantBytes.reserve(
      dst.drawUniformPixelConstantBytes.size() + src.drawUniformPixelConstantBytes.size());
  dst.drawUniformPayloads.reserve(dst.drawUniformPayloads.size() + src.drawUniformPayloads.size());
  dst.drawParams.reserve(dst.drawParams.size() + src.drawParams.size());
  dst.drawPayloadArena.reserve(dst.drawPayloadArena.size() + src.drawPayloadArena.size());
  dst.drawRunRecords.reserve(dst.drawRunRecords.size() + src.drawRunRecords.size());

  appendRange(dst.drawHotStates, src.drawHotStates);
  appendRange(dst.drawShaderLayouts, src.drawShaderLayouts);
  appendRange(dst.drawDebugSnapshots, src.drawDebugSnapshots);
  appendRange(dst.drawPsoSubviews, src.drawPsoSubviews);

  for (const auto& sourceRecord : src.drawUniformFixedPayloads) {
    auto record = sourceRecord;
    record.handle = remapUniformFixedHandle(record.handle, uniformFixedBase);
    dst.drawUniformFixedPayloads.push_back(record);
  }
  appendRange(dst.drawUniformVertexConstantBytes, src.drawUniformVertexConstantBytes);
  for (const auto& sourceRecord : src.drawUniformVertexConstants) {
    auto record = sourceRecord;
    record.handle = remapUniformStageHandle(record.handle, uniformVertexBase);
    record.constants.byteOffset = checkedU32(
        uniformVertexByteBase + record.constants.byteOffset);
    dst.drawUniformVertexConstants.push_back(record);
  }
  appendRange(dst.drawUniformPixelConstantBytes, src.drawUniformPixelConstantBytes);
  for (const auto& sourceRecord : src.drawUniformPixelConstants) {
    auto record = sourceRecord;
    record.handle = remapUniformStageHandle(record.handle, uniformPixelBase);
    record.constants.byteOffset = checkedU32(
        uniformPixelByteBase + record.constants.byteOffset);
    dst.drawUniformPixelConstants.push_back(record);
  }
  for (const auto& sourceRecord : src.drawUniformPayloads) {
    auto record = sourceRecord;
    record.handle = remapUniformHandle(record.handle, uniformPayloadBase);
    record.fixedHandle = remapUniformFixedHandle(record.fixedHandle, uniformFixedBase);
    record.vertexConstantsHandle =
        remapUniformStageHandle(record.vertexConstantsHandle, uniformVertexBase);
    record.pixelConstantsHandle =
        remapUniformStageHandle(record.pixelConstantsHandle, uniformPixelBase);
    dst.drawUniformPayloads.push_back(record);
  }

  for (const auto& sourceParam : src.drawParams) {
    auto param = sourceParam;
    param.uniformHandle = remapUniformHandle(param.uniformHandle, uniformPayloadBase);
    dst.drawParams.push_back(param);
  }
  appendRange(dst.drawPayloadArena, src.drawPayloadArena);

  for (const auto& sourceRecord : src.drawRunRecords) {
    auto record = sourceRecord;
    record.stateIndex = checkedU32(drawStateBase + record.stateIndex);
    record.firstParam = checkedU32(drawParamBase + record.firstParam);
    record.payloadOffset = checkedU32(drawPayloadBase + record.payloadOffset);
    record.uniformHandle = remapUniformHandle(record.uniformHandle, uniformPayloadBase);
    dst.drawRunRecords.push_back(record);
  }
  appendRange(dst.clearRecords, src.clearRecords);
  appendRange(dst.surfaceCopyRecords, src.surfaceCopyRecords);
  appendRange(dst.stretchRectRecords, src.stretchRectRecords);
  appendRange(dst.readbackRecords, src.readbackRecords);
  appendRange(dst.colorFillRecords, src.colorFillRecords);
  appendRange(dst.depthResolveRecords, src.depthResolveRecords);
  appendRange(dst.presentRecords, src.presentRecords);

  for (const auto& sourceHeader : src.commandHeaders) {
    const std::size_t payloadIndex =
        static_cast<std::size_t>(payloadBaseForKind(bases, sourceHeader.kind)) +
        sourceHeader.payloadIndex.value;
    dst.commandHeaders.push_back(MetalCommandHeader{
        .kind = sourceHeader.kind,
        .payloadIndex = CommandPayloadIndex::fromU32(checkedU32(payloadIndex)),
    });
  }

  clearMergedSlotLookupState(dst);
}

}  // namespace

CommandBufferDiagnostics summarizeChunk(const ChunkSummaryInput& input) {
  CommandBufferDiagnostics diagnostics;
  diagnostics.seqId = input.seqId;
  diagnostics.slotIndex = input.slotIndex;
  diagnostics.hasDraw = input.hasDraw;
  diagnostics.hasPresent = input.hasPresent;
  diagnostics.hasBlit = input.hasBlit;
  diagnostics.hasStretchRect = input.hasStretchRect;
  diagnostics.frame = input.frame;
  diagnostics.compatFlags = input.compatFlags;
  diagnostics.vertexShaderHash = input.vertexShaderHash;
  diagnostics.pixelShaderHash = input.pixelShaderHash;
  diagnostics.shaderVariantHash = input.shaderVariantHash;
  return diagnostics;
}

CommandBufferDiagnostics summarizeChunk(u64 seqId,
                                        size_t slotIndex,
                                        std::span<const ChunkObservation> observations) {
  ChunkSummaryInput input;
  input.seqId = seqId;
  input.slotIndex = slotIndex;
  for (const auto& observation : observations) {
    switch (observation.kind) {
      case ChunkObservationKind::Draw:
        input.hasDraw = true;
        input.compatFlags |= observation.compatFlags;
        input.vertexShaderHash = observation.vertexShaderHash;
        input.pixelShaderHash = observation.pixelShaderHash;
        input.shaderVariantHash = observation.shaderVariantHash;
        break;
      case ChunkObservationKind::Blit:
        input.hasBlit = true;
        break;
      case ChunkObservationKind::StretchRect:
        input.hasBlit = true;
        input.hasStretchRect = true;
        break;
      case ChunkObservationKind::Present:
        input.hasPresent = true;
        input.compatFlags |= observation.compatFlags;
        break;
    }
  }
  return summarizeChunk(input);
}

CommandBufferDiagnostics summarizeCommands(u64 seqId,
                                           size_t slotIndex,
                                           const ChunkSlot& slot,
                                           const std::function<u32(Handle)>& resolveSurfaceFlags) {
  std::vector<ChunkObservation> observations;
  observations.reserve(slot.commandCount());
  for (std::size_t i = 0; i < slot.commandCount(); ++i) {
    observations.push_back(makeChunkObservation(slot.commandAt(i), resolveSurfaceFlags));
  }
  return summarizeChunk(seqId, slotIndex, std::span<const ChunkObservation>(observations.data(), observations.size()));
}

Handle selectPresentSourceHandle(const SwapDesc& desc, Handle currentBackBuffer) noexcept {
  return desc.sourceSurface ? desc.sourceSurface : currentBackBuffer;
}

CommandBufferDiagnostics QueueLifecycleController::summarizeSubmission(
    u64 seqId,
    size_t slotIndex) const {
  const auto& resolveSurfaceFlags = submissionBinding_.resolveSurfaceFlags;
  if (slotIndex >= submissionBinding_.slots.size()) {
    return CommandBufferDiagnostics{.seqId = seqId, .slotIndex = slotIndex};
  }
  return summarizeCommands(seqId, slotIndex, submissionBinding_.slots[slotIndex], resolveSurfaceFlags);
}

void QueueLifecycleController::resetNoEnqueueGapProgressLocked() {
  noEnqueueGapCommitPublishRecorded_ = false;
  noEnqueueGapEncodeDequeueRecorded_ = false;
  noEnqueueGapCommandBufferCommitRecorded_ = false;
  noEnqueueGapCommitChunkEntryRecorded_ = false;
  noEnqueueGapCommitChunkReplayStartRecorded_ = false;
  noEnqueueGapCommitChunkReplayEndRecorded_ = false;
  noEnqueueGapCommitPublishOnBeforePublishRecorded_ = false;
  noEnqueueGapCommitChunkEntryTime_ = {};
  noEnqueueGapCommitPublishTime_ = {};
  noEnqueueGapEncodeDequeueTime_ = {};
  noEnqueueGapLastCommitChunkReplayEndTime_ = {};
  noEnqueueGapCommitChunkEntriesBeforePublish_ = 0;
  noEnqueueGapCommitChunkReplayStartsBeforePublish_ = 0;
  noEnqueueGapCommitChunkReplayEndsBeforePublish_ = 0;
  noEnqueueGapCommitChunkCompletedReplayCpuBeforePublishNs_ = 0;
  noEnqueueGapCommitChunkActiveReplayCpuBeforePublishNs_ = 0;
  noEnqueueGapCommitChunkInterReplayGapBeforePublishNs_ = 0;
}

void QueueLifecycleController::recordNoEnqueueWaitGapToCommitPublish() {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommitPublishRecorded_) {
    return;
  }
  const auto elapsed = std::chrono::steady_clock::now() - lastNoEnqueueCompletionWaitEnd_;
  perf::countCompletionNoEnqueueWaitToCommitPublish(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  const auto now = std::chrono::steady_clock::now();
  if (noEnqueueGapCommitChunkEntryTime_ != std::chrono::steady_clock::time_point{}) {
    perf::countCompletionNoEnqueueStageCommitEntryToPublish(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - noEnqueueGapCommitChunkEntryTime_).count()));
  }
  perf::countCompletionNoEnqueueCommitChunksBeforePublish(
      noEnqueueGapCommitChunkEntriesBeforePublish_,
      noEnqueueGapCommitChunkReplayStartsBeforePublish_,
      noEnqueueGapCommitChunkReplayEndsBeforePublish_);
  perf::countCompletionNoEnqueueCommitChunkCompletedReplayCpuBeforePublish(
      noEnqueueGapCommitChunkCompletedReplayCpuBeforePublishNs_);
  perf::countCompletionNoEnqueueCommitChunkActiveReplayCpuBeforePublish(
      noEnqueueGapCommitChunkActiveReplayCpuBeforePublishNs_);
  perf::countCompletionNoEnqueueCommitChunkInterReplayGapBeforePublish(
      noEnqueueGapCommitChunkInterReplayGapBeforePublishNs_);
  noEnqueueGapCommitPublishTime_ = now;
  noEnqueueGapCommitPublishRecorded_ = true;
}

void QueueLifecycleController::recordNoEnqueueCommitPublishWaitBeforePublish(
    std::uint64_t nanoseconds) {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommitPublishRecorded_) {
    return;
  }
  perf::countCompletionNoEnqueueCommitPublishWaitBeforePublish(nanoseconds);
}

void QueueLifecycleController::recordNoEnqueueCommitPublishOnBeforePublishCpu(
    std::uint64_t nanoseconds) {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommitPublishTime_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapEncodeDequeueRecorded_ ||
      noEnqueueGapCommitPublishOnBeforePublishRecorded_) {
    return;
  }
  perf::countCompletionNoEnqueueCommitPublishOnBeforePublishCpu(nanoseconds);
  noEnqueueGapCommitPublishOnBeforePublishRecorded_ = true;
}

void QueueLifecycleController::recordNoEnqueueWaitGapToEncodeDequeue() {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapEncodeDequeueRecorded_) {
    return;
  }
  const auto elapsed = std::chrono::steady_clock::now() - lastNoEnqueueCompletionWaitEnd_;
  perf::countCompletionNoEnqueueWaitToEncodeDequeue(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  const auto now = std::chrono::steady_clock::now();
  if (noEnqueueGapCommitPublishTime_ != std::chrono::steady_clock::time_point{}) {
    perf::countCompletionNoEnqueueStagePublishToEncodeDequeue(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - noEnqueueGapCommitPublishTime_).count()));
  }
  noEnqueueGapEncodeDequeueTime_ = now;
  noEnqueueGapEncodeDequeueRecorded_ = true;
}

void QueueLifecycleController::recordNoEnqueueWaitGapToCommandBufferCommit() {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommandBufferCommitRecorded_) {
    return;
  }
  const auto elapsed = std::chrono::steady_clock::now() - lastNoEnqueueCompletionWaitEnd_;
  perf::countCompletionNoEnqueueWaitToCommandBufferCommit(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  const auto now = std::chrono::steady_clock::now();
  if (noEnqueueGapEncodeDequeueTime_ != std::chrono::steady_clock::time_point{}) {
    perf::countCompletionNoEnqueueStageEncodeDequeueToCommandBufferCommit(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - noEnqueueGapEncodeDequeueTime_).count()));
  }
  noEnqueueGapCommandBufferCommitRecorded_ = true;
}

void QueueLifecycleController::recordNoEnqueueWaitGapToCommitChunkEntry() {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{}) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!noEnqueueGapCommitPublishRecorded_) {
    if (noEnqueueGapLastCommitChunkReplayEndTime_ !=
        std::chrono::steady_clock::time_point{}) {
      noEnqueueGapCommitChunkInterReplayGapBeforePublishNs_ +=
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              now - noEnqueueGapLastCommitChunkReplayEndTime_).count());
      noEnqueueGapLastCommitChunkReplayEndTime_ = {};
    }
    ++noEnqueueGapCommitChunkEntriesBeforePublish_;
  }
  if (noEnqueueGapCommitChunkEntryRecorded_) {
    return;
  }
  const auto elapsed = now - lastNoEnqueueCompletionWaitEnd_;
  perf::countCompletionNoEnqueueWaitToCommitChunkEntry(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  noEnqueueGapCommitChunkEntryTime_ = now;
  noEnqueueGapCommitChunkEntryRecorded_ = true;
}

void QueueLifecycleController::recordNoEnqueueWaitGapToCommitChunkReplayStart() {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{}) {
    return;
  }
  if (!noEnqueueGapCommitPublishRecorded_) {
    ++noEnqueueGapCommitChunkReplayStartsBeforePublish_;
  }
  if (noEnqueueGapCommitChunkReplayStartRecorded_) {
    return;
  }
  const auto elapsed = std::chrono::steady_clock::now() - lastNoEnqueueCompletionWaitEnd_;
  perf::countCompletionNoEnqueueWaitToCommitChunkReplayStart(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  noEnqueueGapCommitChunkReplayStartRecorded_ = true;
}

void QueueLifecycleController::recordNoEnqueueWaitGapToCommitChunkReplayEnd() {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{}) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!noEnqueueGapCommitPublishRecorded_) {
    ++noEnqueueGapCommitChunkReplayEndsBeforePublish_;
    noEnqueueGapLastCommitChunkReplayEndTime_ = now;
  }
  if (noEnqueueGapCommitChunkReplayEndRecorded_) {
    return;
  }
  const auto elapsed = now - lastNoEnqueueCompletionWaitEnd_;
  perf::countCompletionNoEnqueueWaitToCommitChunkReplayEnd(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  noEnqueueGapCommitChunkReplayEndRecorded_ = true;
}

void QueueLifecycleController::recordNoEnqueueCommitChunkReplayCpuBeforePublish(
    std::uint64_t nanoseconds) {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommitPublishRecorded_) {
    return;
  }
  noEnqueueGapCommitChunkCompletedReplayCpuBeforePublishNs_ += nanoseconds;
}

void QueueLifecycleController::recordNoEnqueueCommitChunkActiveReplayCpuBeforePublish(
    std::uint64_t nanoseconds) {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommitPublishRecorded_) {
    return;
  }
  noEnqueueGapCommitChunkActiveReplayCpuBeforePublishNs_ =
      std::max(noEnqueueGapCommitChunkActiveReplayCpuBeforePublishNs_, nanoseconds);
}

void QueueLifecycleController::recordNoEnqueueCommitChunkRecordShapeBeforePublish(
    const NoEnqueueCommitChunkRecordShape& shape) {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommitPublishRecorded_) {
    return;
  }
  perf::countCompletionNoEnqueueCommitChunkRecordShapeBeforePublish(
      shape.recordCount,
      shape.drawRecords,
      shape.constRecords,
      shape.applyStateRecords,
      shape.clearRecords,
      shape.presentRecords,
      shape.surfaceRecords,
      shape.queryRecords,
      shape.otherRecords);
}

void QueueLifecycleController::observeTransition(const QueueTransitionRecord& record) const {
  switch (classifyTransition(record)) {
    case QueueLifecycleEvent::PresentEnqueue:
      if (record.slotIndex.has_value() && record.present) {
        notePresentEnqueue(record.after, *record.slotIndex, record.eventSeqId, *record.present,
                           record.sourceHandle);
      }
      return;
    case QueueLifecycleEvent::WriterWaitBegin:
      if (record.slotIndex.has_value()) {
        noteWriterWaitBeginIfNeeded(record.before, *record.slotIndex, record.eventSeqId,
                                    record.inflightLimit);
      }
      return;
    case QueueLifecycleEvent::WriterWaitEnd:
      if (record.slotIndex.has_value()) {
        noteWriterWaitEnd(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::WriterAcquire:
      if (record.slotIndex.has_value()) {
        noteWriterAcquire(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::CommitEmpty:
      if (record.slotIndex.has_value()) {
        noteCommitEmpty(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::CommitWaitBegin:
      if (record.slotIndex.has_value()) {
        noteCommitWaitBeginIfNeeded(record.before, *record.slotIndex, record.eventSeqId,
                                    record.inflightLimit);
      }
      return;
    case QueueLifecycleEvent::CommitWaitEnd:
      if (record.slotIndex.has_value()) {
        noteCommitWaitEnd(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::CommitPublish:
      if (record.slotIndex.has_value()) {
        noteCommitPublish(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::EncodeDequeue:
      if (record.slotIndex.has_value()) {
        noteEncodeDequeue(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::EncodeCommit:
      if (record.slotIndex.has_value()) {
        noteEncodeCommit(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::GpuComplete:
      if (record.slotIndex.has_value()) {
        noteGpuComplete(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::FinishInline:
      if (record.slotIndex.has_value()) {
        noteFinishInline(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::FinishDequeue:
      noteFinishDequeue(record.after, record.eventSeqId);
      return;
    case QueueLifecycleEvent::ReclaimFree:
      if (record.slotIndex.has_value()) {
        noteReclaimFree(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::WaitSeqBegin:
      noteWaitSeqBeginIfNeeded(record.before, record.eventSeqId);
      return;
    case QueueLifecycleEvent::WaitSeqEnd:
      noteWaitSeqEnd(record.after, record.eventSeqId);
      return;
  }
}

void QueueLifecycleController::bindTrackedSubmissionState(SubmissionBinding binding) {
  submissionBinding_ = binding;
}

bool QueueLifecycleController::ensureWriterSlot(std::unique_lock<std::mutex>& lock,
                                                size_t inflightLimit) {
  // TLA+: QueueLifecycleRefinement / WriterAcquire.
  auto* writingSlot = submissionBinding_.writingSlot;
  auto* writeIndex = submissionBinding_.writeIndex;
  auto* inflightCount = submissionBinding_.inflightCount;
  auto* writeCv = submissionBinding_.writeCv;
  auto* stop = submissionBinding_.stop;
  if (!writingSlot || !writeIndex || !inflightCount || !writeCv || !stop) {
    return false;
  }

  if (writingSlot->has_value()) {
    return true;
  }

  auto& slots = submissionBinding_.slots;
  const bool waitNeeded =
      slots[*writeIndex].state != ChunkSlot::State::Free || *inflightCount >= inflightLimit;
  if (waitNeeded) {
    observeWriterWait(*writeIndex, slots[*writeIndex].seqId, inflightLimit);
  }
  const auto waitStarted = std::chrono::steady_clock::now();
  writeCv->wait(lock, [&] {
    return *stop || (slots[*writeIndex].state == ChunkSlot::State::Free &&
                     *inflightCount < inflightLimit);
  });
  if (waitNeeded) {
    const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
    perf::countQueueWriterWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  }
  if (*stop) {
    return false;
  }
  observeWriterWait(*writeIndex, slots[*writeIndex].seqId, inflightLimit);
  acquireWriterSlot(*writeIndex, slots[*writeIndex].seqId, inflightLimit, [&] {
    slots[*writeIndex].state = ChunkSlot::State::Writing;
    slots[*writeIndex].seqId = 0;
    slots[*writeIndex].clearCommands();
    *writingSlot = *writeIndex;
  });
  return true;
}

void QueueLifecycleController::presentAndCommit(
    std::unique_lock<std::mutex>& lock,
    size_t inflightLimit,
    const SwapDesc& present,
    Handle sourceHandle,
    const std::function<void(ChunkSlot&)>& onBeforePublish) {
  // TLA+: PresentFrameLatency / CommitPresent.
  if (!ensureWriterSlot(lock, inflightLimit)) {
    return;
  }
  appendPresentCommand(present, sourceHandle);
  (void)commitCurrentChunk(lock, inflightLimit, onBeforePublish);
}

void QueueLifecycleController::flushAndWait(
    std::unique_lock<std::mutex>& lock,
    size_t inflightLimit,
    const std::function<void(ChunkSlot&)>& onBeforePublish) {
  // TLA+: QueueLifecycleRefinement / CommitPublish then WaitForSequence.
  (void)commitCurrentChunk(lock, inflightLimit, onBeforePublish);

  auto* nextSeqId = submissionBinding_.nextSeqId;
  const u64 targetSeqId = (!nextSeqId || *nextSeqId == 0) ? 0 : *nextSeqId - 1;
  waitForSequence(lock, targetSeqId);
}

bool QueueLifecycleController::commitCurrentChunk(
    std::unique_lock<std::mutex>& lock,
    size_t inflightLimit,
    const std::function<void(ChunkSlot&)>& onBeforePublish) {
  // TLA+: QueueLifecycleRefinement / CommitEmpty or CommitPublish.
  auto* writingSlot = submissionBinding_.writingSlot;
  auto* writeIndex = submissionBinding_.writeIndex;
  auto* nextSeqId = submissionBinding_.nextSeqId;
  auto* readySlots = submissionBinding_.readySlots;
  auto* inflightCount = submissionBinding_.inflightCount;
  auto* lastCommittedSeqId = submissionBinding_.lastCommittedSeqId;
  auto* writeCv = submissionBinding_.writeCv;
  auto* encodeCv = submissionBinding_.encodeCv;
  auto* stop = submissionBinding_.stop;
  if (!writingSlot || !writeIndex || !nextSeqId || !readySlots || !inflightCount ||
      !lastCommittedSeqId || !writeCv || !stop) {
    return false;
  }
  if (!writingSlot->has_value()) {
    return false;
  }

  auto& slots = submissionBinding_.slots;
  auto& slot = slots[**writingSlot];
  if (slot.commandsEmpty()) {
    const size_t slotIndex = **writingSlot;
    commitEmpty(slotIndex, slot.seqId, [&] {
      slot.state = ChunkSlot::State::Free;
      writingSlot->reset();
    });
    return false;
  }

  const bool waitNeeded = *inflightCount >= inflightLimit;
  if (waitNeeded) {
    observeCommitWait(**writingSlot, slot.seqId, inflightLimit);
  }
  const auto waitStarted = std::chrono::steady_clock::now();
  writeCv->wait(lock, [&] { return *stop || *inflightCount < inflightLimit; });
  const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
  if (waitNeeded) {
    perf::countQueueCommitWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  }
  if (*stop) {
    return false;
  }

  const size_t publishedSlotIndex = **writingSlot;
  const u64 publishedSeqId = *nextSeqId;
  observeCommitWait(publishedSlotIndex, slot.seqId, inflightLimit);
  recordNoEnqueueCommitPublishWaitBeforePublish(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  recordNoEnqueueWaitGapToCommitPublish();
  commitPublish(publishedSlotIndex, publishedSeqId, inflightLimit, [&] {
    slot.seqId = (*nextSeqId)++;
    slot.state = ChunkSlot::State::Pending;
    *lastCommittedSeqId = slot.seqId;
    ++(*inflightCount);
    const auto onBeforePublishStart = std::chrono::steady_clock::now();
    if (onBeforePublish) {
      onBeforePublish(slot);
    }
    recordNoEnqueueCommitPublishOnBeforePublishCpu(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - onBeforePublishStart).count()));
    readySlots->push_back(publishedSlotIndex);
    writingSlot->reset();
    *writeIndex = (*writeIndex + 1) % slots.size();
  });
  if (encodeCv) {
    encodeCv->notify_one();
  }
  return true;
}

bool QueueLifecycleController::dequeueReadySlot(std::unique_lock<std::mutex>& lock,
                                                size_t& slotIndex,
                                                ChunkSlot& slotCopy) {
  // TLA+: QueueLifecycleRefinement / EncodeDequeue.
  auto* readySlots = submissionBinding_.readySlots;
  auto* encodeCv = submissionBinding_.encodeCv;
  auto* stop = submissionBinding_.stop;
  if (!readySlots || !encodeCv || !stop) {
    return false;
  }

  const size_t coalesceLimit = encodeReadySlotCoalescingEnabled()
      ? encodeReadySlotCoalesceLimit()
      : 1u;
  encodeCv->wait(lock, [&] {
    return *stop || cpuReadyEncodeGroupAvailable(coalesceLimit);
  });
  if (*stop && readySlots->empty()) {
    return false;
  }

  perf::countEncodeDequeueReadyDepth(
      static_cast<std::uint64_t>(readySlots->size()));
  slotIndex = readySlots->front();
  auto& slot = submissionBinding_.slots[slotIndex];
  traceEncodeIterationStage("dequeue.selected", slotIndex, slot);
  recordNoEnqueueWaitGapToEncodeDequeue();
  encodeDequeue(slotIndex, slot.seqId, [&] {
    readySlots->pop_front();
    slot.state = ChunkSlot::State::Encoding;
  });
  traceEncodeIterationStage("dequeue.after-transition", slotIndex, slot);
  traceEncodeIterationStage("dequeue.before-slot-copy", slotIndex, slot);
  slotCopy = slot;
  traceEncodeIterationStage("dequeue.after-slot-copy", slotIndex, slotCopy);
  return true;
}

bool QueueLifecycleController::cpuReadyEncodeGroupAvailable(size_t coalesceLimit) const {
  // R-BACK-2.40 — readySlots are the current CPU-ready staging lane:
  // records, retained handles, seqIds, and allocator ownership are already
  // queue-owned, but the encoder should not let the first small non-present
  // slot immediately become a Metal CB boundary. Wait deterministically until
  // the encoder can pick a baseline-like carrier: a full compatible group, a
  // hard boundary, idle producer, or ring backpressure. No wallclock / GPU
  // feedback participates in this decision (R-BACK-2.39).
  const auto* readySlots = submissionBinding_.readySlots;
  if (!readySlots || readySlots->empty()) {
    return false;
  }
  if (coalesceLimit <= 1) {
    return true;
  }

  const auto& slots = submissionBinding_.slots;
  const size_t headSlotIndex = readySlots->front();
  if (headSlotIndex >= slots.size()) {
    return true;
  }
  if (chunkBlocksEncodeCoalescing(slots[headSlotIndex])) {
    return true;
  }

  size_t compatibleDepth = 0;
  while (compatibleDepth < readySlots->size() &&
         compatibleDepth < coalesceLimit) {
    const size_t slotIndex = (*readySlots)[compatibleDepth];
    if (slotIndex >= slots.size()) {
      return true;
    }
    if (chunkBlocksEncodeCoalescing(slots[slotIndex])) {
      break;
    }
    ++compatibleDepth;
  }
  if (compatibleDepth >= coalesceLimit) {
    return true;
  }
  if (compatibleDepth < readySlots->size()) {
    return true;
  }

  const auto* writingSlot = submissionBinding_.writingSlot;
  if (!writingSlot || !writingSlot->has_value()) {
    return true;
  }

  const auto* inflightCount = submissionBinding_.inflightCount;
  const size_t backpressureLimit = slots.empty() ? 0 : slots.size() - 1u;
  if (inflightCount && backpressureLimit > 0 &&
      *inflightCount >= backpressureLimit) {
    return true;
  }

  traceCpuReadyStageHold(readySlots->size(), compatibleDepth);
  return false;
}

bool QueueLifecycleController::runEncodeIteration(
    std::unique_lock<std::mutex>& lock,
    const std::function<std::optional<QueueSubmissionRecord>(size_t, ChunkSlot&)>& encodeFn,
    const std::function<void(u64)>& onInlineComplete) {
  // TLA+: EncodeDequeue followed by EncodeSubmitToGpu or EncodeCompleteInline.
  size_t slotIndex = 0;
  ChunkSlot slotCopy;
  if (!dequeueReadySlot(lock, slotIndex, slotCopy)) {
    return false;
  }
  std::array<size_t, 16> coalescedSlotIndices{};
  std::array<u64, 16> coalescedSeqIds{};
  size_t coalescedCount = 1;
  coalescedSlotIndices[0] = slotIndex;
  coalescedSeqIds[0] = slotCopy.seqId;

  auto* readySlots = submissionBinding_.readySlots;
  const size_t coalesceLimit = encodeReadySlotCoalescingEnabled()
      ? encodeReadySlotCoalesceLimit()
      : 1u;
  if (readySlots && coalesceLimit > 1 && !chunkBlocksEncodeCoalescing(slotCopy)) {
    auto& slots = submissionBinding_.slots;
    while (!readySlots->empty() && coalescedCount < coalesceLimit) {
      const size_t nextSlotIndex = readySlots->front();
      if (nextSlotIndex >= slots.size()) {
        break;
      }
      auto& nextSlot = slots[nextSlotIndex];
      if (chunkBlocksEncodeCoalescing(nextSlot)) {
        break;
      }

      const u64 nextSeqId = nextSlot.seqId;
      traceEncodeIterationStage("dequeue.coalesce-selected", nextSlotIndex, nextSlot);
      encodeDequeue(nextSlotIndex, nextSeqId, [&] {
        readySlots->pop_front();
        nextSlot.state = ChunkSlot::State::Encoding;
      });
      appendChunkSlotForEncodeCoalescing(slotCopy, nextSlot);
      coalescedSlotIndices[coalescedCount] = nextSlotIndex;
      coalescedSeqIds[coalescedCount] = nextSeqId;
      ++coalescedCount;
      slotCopy.seqId = nextSeqId;
      traceEncodeCoalesce(slotIndex, nextSlotIndex, coalescedSeqIds[0],
                          nextSeqId, slotCopy.commandCount());
    }
  }
  // R-BACK-2.41 — a coalesced merge is encoded as one command buffer; flag it
  // so encodeChunk suppresses the per-render-pass mid-chunk split that would
  // otherwise re-fragment the merged passes into N sub-CBs and break the
  // R-BACK-2.36 locality gate.
  slotCopy.coalescedRunAhead = coalescedCount > 1;

  traceEncodeIterationStage("iteration.after-dequeue", slotIndex, slotCopy);
  traceEncodeIterationStage("iteration.before-unlock", slotIndex, slotCopy);
  lock.unlock();
  traceEncodeIterationStage("iteration.after-unlock", slotIndex, slotCopy);
  std::optional<QueueSubmissionRecord> submission;
  if (encodeFn) {
    traceEncodeIterationStage("iteration.before-encodefn", slotIndex, slotCopy);
    submission = encodeFn(slotIndex, slotCopy);
    traceEncodeIterationStage(submission.has_value()
                                  ? "iteration.after-encodefn-submission"
                                  : "iteration.after-encodefn-inline",
                              slotIndex,
                              slotCopy);
  } else {
    traceEncodeIterationStage("iteration.no-encodefn", slotIndex, slotCopy);
  }
  traceEncodeIterationStage("iteration.before-relock", slotIndex, slotCopy);
  lock.lock();
  traceEncodeIterationStage("iteration.after-relock", slotIndex, slotCopy);

  if (submission.has_value()) {
    if (coalescedCount > 1) {
      submission->slotIndex = coalescedSlotIndices[0];
      submission->seqId = coalescedSeqIds[coalescedCount - 1u];
      submission->coalescedSlotIndices.assign(coalescedSlotIndices.begin(),
                                              coalescedSlotIndices.begin() + coalescedCount);
      submission->coalescedSeqIds.assign(coalescedSeqIds.begin(),
                                         coalescedSeqIds.begin() + coalescedCount);
      submission->diagnostics =
          summarizeCommands(submission->seqId, submission->slotIndex,
                            slotCopy, submissionBinding_.resolveSurfaceFlags);
      submission->context = "queue-coalesced";
    }
    auto postCommitCallbacks = std::move(submission->postCommitCallbacks);
    traceEncodeIterationStage("iteration.before-submit-record", slotIndex, slotCopy);
    enqueueSubmission(*submission);
    traceEncodeIterationStage("iteration.after-submit-record", slotIndex, slotCopy);
    lock.unlock();
    traceEncodeIterationStage("iteration.after-submit-unlock", slotIndex, slotCopy);
    for (auto& callback : postCommitCallbacks) {
      if (callback) {
        callback();
      }
    }
    traceEncodeIterationStage("iteration.after-post-commit-callbacks", slotIndex, slotCopy);
  } else {
    traceEncodeIterationStage("iteration.before-inline-complete", slotIndex, slotCopy);
    for (std::size_t i = 0; i < coalescedCount; ++i) {
      completeInlineChunk(coalescedSlotIndices[i], coalescedSeqIds[i]);
      if (onInlineComplete) {
        onInlineComplete(coalescedSeqIds[i]);
      }
    }
    traceEncodeIterationStage("iteration.after-inline-complete", slotIndex, slotCopy);
  }
  return true;
}

void QueueLifecycleController::appendPresentCommand(const SwapDesc& present, Handle sourceHandle) {
  // TLA+: PresentFrameLatency / CommitPresent metadata lane.
  auto* writingSlot = submissionBinding_.writingSlot;
  if (!writingSlot || !writingSlot->has_value()) {
    return;
  }

  const size_t slotIndex = **writingSlot;
  auto& slot = submissionBinding_.slots[slotIndex];
  enqueuePresent(slotIndex, slot.seqId, present, sourceHandle, [&] {
    slot.appendPresent(present, sourceHandle);
  });
}

void QueueLifecycleController::submitEncodedChunk(WMT::Reference<WMT::CommandBuffer> commandBuffer,
                                                  size_t slotIndex,
                                                  u64 seqId,
                                                  const char* context) {
  // TLA+: QueueLifecycleRefinement / EncodeSubmitToGpu.
  QueueSubmissionRecord record;
  record.commandBuffer = std::move(commandBuffer);
  record.slotIndex = slotIndex;
  record.seqId = seqId;
  record.diagnostics = summarizeSubmission(seqId, slotIndex);
  record.context = context;
  enqueueSubmission(record);
}

void QueueLifecycleController::completeInlineChunk(size_t slotIndex, u64 seqId) {
  // TLA+: QueueLifecycleRefinement / EncodeCompleteInline.
  if (slotIndex >= submissionBinding_.slots.size()) {
    return;
  }

  auto& slot = submissionBinding_.slots[slotIndex];
  auto* completedSeqId = submissionBinding_.completedSeqId;
  auto* completedSeqQueue = submissionBinding_.completedSeqQueue;
  finishInline(slotIndex, seqId, [&] {
    // TLA+: QueueLifecycleRefinement / EncodeCompleteInline.
    if (completedSeqId && completedSeqQueue) {
      DXMT_ASSERT(seqId == *completedSeqId + completedSeqQueue->size() + 1);
    }
    slot.state = ChunkSlot::State::Free;
    slot.seqId = 0;
    slot.clearCommands();
    if (completedSeqQueue) {
      completedSeqQueue->push_back(seqId);
    }
  });
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  if (submissionBinding_.finishCv) {
    submissionBinding_.finishCv->notify_all();
  }
}

bool QueueLifecycleController::drainCompletedSequence(std::unique_lock<std::mutex>& lock,
                                                      u64& seqId) {
  // TLA+: QueueLifecycleRefinement / FinishDequeue.
  auto* completedSeqQueue = submissionBinding_.completedSeqQueue;
  auto* finishCv = submissionBinding_.finishCv;
  auto* stop = submissionBinding_.stop;
  auto* completedSeqId = submissionBinding_.completedSeqId;
  auto* inflightCount = submissionBinding_.inflightCount;
  if (!completedSeqQueue || !finishCv || !stop || !completedSeqId || !inflightCount) {
    return false;
  }

  finishCv->wait(lock, [&] { return *stop || !completedSeqQueue->empty(); });
  if (*stop && completedSeqQueue->empty()) {
    return false;
  }

  seqId = completedSeqQueue->front();
  // TLA+: QueueLifecycleRefinement / FinishDequeue.
  DXMT_ASSERT(seqId == *completedSeqId + 1);
  finishDequeue(seqId, [&] {
    completedSeqQueue->pop_front();
    *completedSeqId = std::max(*completedSeqId, seqId);
    if (*inflightCount > 0) {
      --(*inflightCount);
    }
    auto* completedPresentSeqQueue = submissionBinding_.completedPresentSeqQueue;
    auto* presentCompletedSeqId = submissionBinding_.presentCompletedSeqId;
    if (completedPresentSeqQueue && presentCompletedSeqId) {
      while (!completedPresentSeqQueue->empty() &&
             completedPresentSeqQueue->front() <= *completedSeqId) {
        *presentCompletedSeqId = std::max(*presentCompletedSeqId, completedPresentSeqQueue->front());
        completedPresentSeqQueue->pop_front();
      }
      // TLA+: PresentFrameLatency / PresentCompletionSafety.
      DXMT_ASSERT(*presentCompletedSeqId <= *completedSeqId);
    }
  });
  if (submissionBinding_.finishCv) {
    submissionBinding_.finishCv->notify_all();
  }
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  if (submissionBinding_.presentCompletedCv) {
    submissionBinding_.presentCompletedCv->notify_all();
  }
  return true;
}

bool QueueLifecycleController::runFinishIteration(std::unique_lock<std::mutex>& lock,
                                                  const std::function<void(u64)>& onAfterFinish) {
  // TLA+: FinishDequeue followed by ReclaimFree.
  u64 seqId = 0;
  if (!drainCompletedSequence(lock, seqId)) {
    return false;
  }
  reclaimCompletedGpuSlots(seqId);
  if (onAfterFinish) {
    onAfterFinish(seqId);
  }
  return true;
}

void QueueLifecycleController::reclaimCompletedGpuSlots(u64 seqId) {
  // TLA+: QueueLifecycleRefinement / ReclaimFree.
  bool reclaimed = false;
  auto& slots = submissionBinding_.slots;
  for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
    auto& slot = slots[slotIndex];
    if (slot.state != ChunkSlot::State::GPU || slot.seqId != seqId) {
      continue;
    }
    reclaimFree(slotIndex, seqId, [&] {
      slot.state = ChunkSlot::State::Free;
      slot.clearCommands();
      slot.seqId = 0;
    });
    reclaimed = true;
  }
  if (reclaimed && submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
}

void QueueLifecycleController::waitForSequence(std::unique_lock<std::mutex>& lock,
                                               u64 targetSeqId) {
  // TLA+: QueueLifecycleRefinement / BeginWaitForSequence + EndWaitForSequence.
  auto* completedSeqId = submissionBinding_.completedSeqId;
  auto* finishCv = submissionBinding_.finishCv;
  auto* stop = submissionBinding_.stop;
  if (!completedSeqId || !finishCv || !stop) {
    return;
  }
  if (*completedSeqId < targetSeqId) {
    observeWaitForSequence(targetSeqId);
  }
  const bool waitNeeded = *completedSeqId < targetSeqId;
  const auto waitStarted = std::chrono::steady_clock::now();
  finishCv->wait(lock, [&] { return *stop || *completedSeqId >= targetSeqId; });
  // TLA+: QueueLifecycleRefinement / WaitForSequenceSafety.
  DXMT_ASSERT(*stop || *completedSeqId >= targetSeqId);
  if (waitNeeded) {
    const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
    perf::countQueueSequenceWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  }
  if (!*stop) {
    observeWaitForSequence(targetSeqId);
  }
}

void QueueLifecycleController::enqueuePresent(size_t slotIndex,
                                              u64 eventSeqId,
                                              const SwapDesc& present,
                                              Handle sourceHandle,
                                              const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
                 .present = &present,
                 .sourceHandle = sourceHandle,
             },
             mutate);
}

void QueueLifecycleController::observeWriterWait(size_t slotIndex,
                                                 u64 eventSeqId,
                                                 size_t inflightLimit) {
  transition(QueueTransitionRecord{
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
      .inflightLimit = inflightLimit,
  });
}

void QueueLifecycleController::acquireWriterSlot(size_t slotIndex,
                                                 u64 eventSeqId,
                                                 size_t inflightLimit,
                                                 const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
                 .inflightLimit = inflightLimit,
             },
             mutate);
}

void QueueLifecycleController::commitEmpty(size_t slotIndex,
                                           u64 eventSeqId,
                                           const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
             },
             mutate);
}

void QueueLifecycleController::observeCommitWait(size_t slotIndex,
                                                 u64 eventSeqId,
                                                 size_t inflightLimit) {
  transition(QueueTransitionRecord{
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
      .inflightLimit = inflightLimit,
  });
}

void QueueLifecycleController::commitPublish(size_t slotIndex,
                                             u64 eventSeqId,
                                             size_t inflightLimit,
                                             const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
                 .inflightLimit = inflightLimit,
             },
             mutate);
}

void QueueLifecycleController::encodeDequeue(size_t slotIndex,
                                             u64 eventSeqId,
                                             const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
             },
             mutate);
}

void QueueLifecycleController::enqueueSubmission(QueueSubmissionRecord record) {
  submit(record);
}

void QueueLifecycleController::finishInline(size_t slotIndex,
                                            u64 eventSeqId,
                                            const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
             },
             mutate);
}

void QueueLifecycleController::finishDequeue(u64 eventSeqId,
                                             const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
      .eventSeqId = eventSeqId,
  }, mutate);
}

void QueueLifecycleController::reclaimFree(size_t slotIndex,
                                           u64 eventSeqId,
                                           const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
             },
             mutate);
}

void QueueLifecycleController::observeWaitForSequence(u64 targetSeqId) {
  transition(QueueTransitionRecord{
      .eventSeqId = targetSeqId,
  });
}

QueueLifecycleEvent QueueLifecycleController::classifyTransition(const QueueTransitionRecord& record) const {
  if (record.present) {
    return QueueLifecycleEvent::PresentEnqueue;
  }

  if (record.after.completedQueueCount < record.before.completedQueueCount) {
    return QueueLifecycleEvent::FinishDequeue;
  }

  if (!record.slotIndex.has_value()) {
    return record.after.completedSeqId >= record.eventSeqId ? QueueLifecycleEvent::WaitSeqEnd
                                                            : QueueLifecycleEvent::WaitSeqBegin;
  }

  const auto beforeState = slotStateFor(record.before, record.slotIndex);
  const auto afterState = slotStateFor(record.after, record.slotIndex);
  const auto beforeCommandCount = slotCommandCountFor(record.before, record.slotIndex);
  const auto afterCommandCount = slotCommandCountFor(record.after, record.slotIndex);

  if (!record.before.writingSlot.has_value() && record.after.writingSlot.has_value()) {
    return QueueLifecycleEvent::WriterAcquire;
  }

  if (beforeState == ChunkSlot::State::Pending && afterState == ChunkSlot::State::Encoding) {
    return QueueLifecycleEvent::EncodeDequeue;
  }

  if (beforeState == ChunkSlot::State::Encoding && afterState == ChunkSlot::State::GPU) {
    return QueueLifecycleEvent::EncodeCommit;
  }

  if (record.after.completedQueueCount > record.before.completedQueueCount &&
      record.after.inflightCount == record.before.inflightCount) {
    return QueueLifecycleEvent::GpuComplete;
  }

  if (record.after.completedQueueCount > record.before.completedQueueCount &&
      record.after.inflightCount < record.before.inflightCount) {
    return QueueLifecycleEvent::FinishInline;
  }

  if (beforeState == ChunkSlot::State::GPU && afterState == ChunkSlot::State::Free) {
    return QueueLifecycleEvent::ReclaimFree;
  }

  if (record.before.writingSlot.has_value() && !record.after.writingSlot.has_value()) {
    if (record.after.readyCount > record.before.readyCount ||
        record.after.lastCommittedSeqId > record.before.lastCommittedSeqId ||
        record.after.inflightCount > record.before.inflightCount ||
        afterState == ChunkSlot::State::Pending) {
      return QueueLifecycleEvent::CommitPublish;
    }
    return QueueLifecycleEvent::CommitEmpty;
  }

  if (record.before.writingSlot.has_value() || record.after.writingSlot.has_value()) {
    return record.after.inflightCount < record.inflightLimit ? QueueLifecycleEvent::CommitWaitEnd
                                                             : QueueLifecycleEvent::CommitWaitBegin;
  }

  if (beforeCommandCount != afterCommandCount) {
    return QueueLifecycleEvent::PresentEnqueue;
  }

  return writerCanProceed(record.after, record.slotIndex, record.inflightLimit)
             ? QueueLifecycleEvent::WriterWaitEnd
             : QueueLifecycleEvent::WriterWaitBegin;
}

QueueControllerState QueueLifecycleController::currentState() const {
  return makeBoundQueueState(submissionBinding_);
}

#ifndef NDEBUG
void QueueLifecycleController::assertQueueLifecycleInvariants(size_t inflightLimit) const {
  const auto& binding = submissionBinding_;
  const auto slots = binding.slots;
  if (slots.empty()) {
    return;
  }

  const u64 completedSeqId = binding.completedSeqId ? *binding.completedSeqId : 0;
  const u64 lastCommittedSeqId = binding.lastCommittedSeqId ? *binding.lastCommittedSeqId : 0;
  DXMT_ASSERT(completedSeqId <= lastCommittedSeqId);

  if (binding.writeIndex) {
    DXMT_ASSERT(*binding.writeIndex < slots.size());
  }
  if (binding.writingSlot && binding.writingSlot->has_value()) {
    DXMT_ASSERT(**binding.writingSlot < slots.size());
    DXMT_ASSERT(slots[**binding.writingSlot].state == ChunkSlot::State::Writing);
  }

  size_t abstractInflight = 0;
  for (const auto& slot : slots) {
    switch (slot.state) {
      case ChunkSlot::State::Free:
        DXMT_ASSERT(slot.seqId == 0);
        break;
      case ChunkSlot::State::Writing:
        DXMT_ASSERT(slot.seqId == 0);
        break;
      case ChunkSlot::State::Pending:
      case ChunkSlot::State::Encoding:
        DXMT_ASSERT(slot.seqId > 0);
        DXMT_ASSERT(slot.seqId <= lastCommittedSeqId);
        ++abstractInflight;
        break;
      case ChunkSlot::State::GPU:
        DXMT_ASSERT(slot.seqId > 0);
        DXMT_ASSERT(slot.seqId <= lastCommittedSeqId);
        if (slot.seqId > completedSeqId) {
          ++abstractInflight;
        }
        break;
    }
  }

  const size_t effectiveInflightLimit = inflightLimit != 0 ? inflightLimit : slots.size();
  if (binding.inflightCount) {
    DXMT_ASSERT(*binding.inflightCount <= effectiveInflightLimit);
    DXMT_ASSERT(abstractInflight <= *binding.inflightCount);
  }

  if (binding.readySlots) {
    for (size_t i = 0; i < binding.readySlots->size(); ++i) {
      const size_t slotIndex = (*binding.readySlots)[i];
      DXMT_ASSERT(slotIndex < slots.size());
      DXMT_ASSERT(slots[slotIndex].state == ChunkSlot::State::Pending);
      for (size_t j = i + 1; j < binding.readySlots->size(); ++j) {
        DXMT_ASSERT(slotIndex != (*binding.readySlots)[j]);
      }
    }
  }

  if (binding.completedSeqQueue) {
    u64 expectedSeqId = completedSeqId + 1;
    for (const u64 seqId : *binding.completedSeqQueue) {
      DXMT_ASSERT(seqId == expectedSeqId);
      DXMT_ASSERT(seqId <= lastCommittedSeqId);
      ++expectedSeqId;
    }
  }

  if (binding.completedPresentSeqQueue) {
    u64 previousSeqId = completedSeqId;
    for (const u64 seqId : *binding.completedPresentSeqQueue) {
      DXMT_ASSERT(seqId > completedSeqId);
      DXMT_ASSERT(seqId > previousSeqId);
      DXMT_ASSERT(seqId <= lastCommittedSeqId);
      previousSeqId = seqId;
    }
  }

  if (binding.presentCompletedSeqId) {
    DXMT_ASSERT(*binding.presentCompletedSeqId <= completedSeqId);
  }
}

void QueueLifecycleController::assertPendingCompletionInvariantsLocked() const {
  const auto& binding = submissionBinding_;
  const auto slots = binding.slots;
  if (slots.empty()) {
    return;
  }

  const u64 lastCommittedSeqId = binding.lastCommittedSeqId ? *binding.lastCommittedSeqId : 0;
  u64 previousSeqId = 0;
  for (const auto& pending : pendingCompletion_) {
    DXMT_ASSERT(pending.coalescedSlotIndices.size() == pending.coalescedSeqIds.size());
    std::array<size_t, 1> singleSlotIndex{pending.slotIndex};
    std::array<u64, 1> singleSeqId{pending.seqId};
    const bool hasCoalescedSlots =
        !pending.coalescedSlotIndices.empty() &&
        pending.coalescedSlotIndices.size() == pending.coalescedSeqIds.size();
    std::span<const size_t> slotIndices =
        hasCoalescedSlots
            ? std::span<const size_t>(pending.coalescedSlotIndices.data(),
                                      pending.coalescedSlotIndices.size())
            : std::span<const size_t>(singleSlotIndex.data(), singleSlotIndex.size());
    std::span<const u64> seqIds =
        hasCoalescedSlots
            ? std::span<const u64>(pending.coalescedSeqIds.data(),
                                   pending.coalescedSeqIds.size())
            : std::span<const u64>(singleSeqId.data(), singleSeqId.size());
    DXMT_ASSERT(slotIndices.size() == seqIds.size());
    for (std::size_t i = 0; i < seqIds.size(); ++i) {
      DXMT_ASSERT(slotIndices[i] < slots.size());
      DXMT_ASSERT(slots[slotIndices[i]].state == ChunkSlot::State::GPU);
      DXMT_ASSERT(slots[slotIndices[i]].seqId == seqIds[i]);
      DXMT_ASSERT(seqIds[i] > 0);
      DXMT_ASSERT(seqIds[i] <= lastCommittedSeqId);
      DXMT_ASSERT(previousSeqId == 0 || seqIds[i] > previousSeqId);
      previousSeqId = seqIds[i];
    }
  }
}
#endif

void QueueLifecycleController::transition(QueueTransitionRecord record,
                                          const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  record.before = before;
  record.after = after;
#ifndef NDEBUG
  assertQueueLifecycleInvariants(record.inflightLimit);
#endif
  observeTransition(record);
}

void QueueLifecycleController::submit(QueueSubmissionRecord& record) {
  std::array<size_t, 1> singleSlotIndex{record.slotIndex};
  std::array<u64, 1> singleSeqId{record.seqId};
  const bool hasCoalescedSlots = !record.coalescedSlotIndices.empty();
  DXMT_ASSERT(record.coalescedSlotIndices.size() == record.coalescedSeqIds.size());
  std::span<const size_t> slotIndices =
      hasCoalescedSlots
          ? std::span<const size_t>(record.coalescedSlotIndices.data(),
                                    record.coalescedSlotIndices.size())
          : std::span<const size_t>(singleSlotIndex.data(), singleSlotIndex.size());
  std::span<const u64> seqIds =
      hasCoalescedSlots
          ? std::span<const u64>(record.coalescedSeqIds.data(),
                                 record.coalescedSeqIds.size())
          : std::span<const u64>(singleSeqId.data(), singleSeqId.size());
  if (slotIndices.size() != seqIds.size() || slotIndices.empty()) {
    return;
  }

  CommandBufferDiagnostics diagnostics = record.diagnostics;
  if (diagnostics.seqId == 0) {
    diagnostics = summarizeSubmission(record.seqId, record.slotIndex);
  }

  const auto beforeCommitState = currentState();
  for (const size_t submittedSlotIndex : slotIndices) {
    if (submittedSlotIndex < submissionBinding_.slots.size()) {
      submissionBinding_.slots[submittedSlotIndex].state = ChunkSlot::State::GPU;
    }
  }
  const auto afterCommitState = currentState();
#ifndef NDEBUG
  assertQueueLifecycleInvariants();
#endif

  for (std::size_t i = 0; i < slotIndices.size(); ++i) {
    observeTransition(QueueTransitionRecord{
        .before = beforeCommitState,
        .after = afterCommitState,
        .slotIndex = slotIndices[i],
        .eventSeqId = seqIds[i],
    });
  }

  if (!record.commandBuffer) {
    return;
  }

  auto commitCommandBuffer = [&] {
    bool captureStarted = false;
    if (record.metalCapture.has_value()) {
      if (record.metalCaptureAlreadyStarted) {
        // Capture was already started at chunk-begin (encodeChunk) so
        // all encoder commands are already in scope. Just remember the
        // active capture so we issue stopCapture after the commit below.
        captureStarted = true;
      } else {
        // Legacy fallback path — should not fire for chunks containing a
        // Present command since encodeChunk pre-starts; kept for safety
        // and for any future capture trigger that bypasses chunk-begin.
        captureStarted = metalcapture::startMetalCapture(record.metalCaptureDevice,
                                                         *record.metalCapture);
      }
    }

    const auto commitStarted = std::chrono::steady_clock::now();
    // M3 — Instruments "commit" interval around the WMT commit call.
    // No-op when no consumer is recording (~5 ns).
    {
      os_log_t signpostLog = dxmt9::signposts::log();
      os_signpost_id_t commitSignpost = os_signpost_id_generate(signpostLog);
      os_signpost_interval_begin(signpostLog, commitSignpost, "commit",
                                 "seq=%llu",
                                 static_cast<unsigned long long>(record.seqId));
      record.commandBuffer.commit();
      os_signpost_interval_end(signpostLog, commitSignpost, "commit",
                               "seq=%llu",
                               static_cast<unsigned long long>(record.seqId));
    }
    perf::countCommandBufferCommitCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - commitStarted).count()));

    if (captureStarted) {
      metalcapture::stopMetalCapture(*record.metalCapture);
    }
  };

  const auto binding = submissionBinding_;
  if (!binding.mutex || !binding.completedSeqQueue) {
    // No binding → just commit; the reference releases when `record` is
    // destroyed by the caller. Covers teardown paths that bypass the
    // finish-thread.
    commitCommandBuffer();
    return;
  }

  CommandBufferDiagnostics preparedDiagnostics = diagnostics;
  auto* diagnosticsController = binding.submissionDiagnostics;
  if (diagnosticsController) {
    preparedDiagnostics = diagnosticsController->prepareQueueSubmission(diagnostics);
  }

  // Commit and hand the tail record (including the retained WMT::CommandBuffer)
  // to the completion-watcher thread via pendingCompletion_. That thread calls
  // waitUntilCompleted() only on this tail CB. For records with
  // commandBufferChainLength > 1, completion of the tail is the public seqId
  // completion point because earlier sub-CBs were committed in encodeChunk on
  // the same Metal queue.
  recordNoEnqueueWaitGapToCommandBufferCommit();
  commitCommandBuffer();

  {
    std::lock_guard lock(pendingCompletionMutex_);
    const bool completionWaitActive = completionWaitActive_;
    const auto enqueueTime = std::chrono::steady_clock::now();
    if (!completionWaitActive &&
        lastNoEnqueueCompletionWaitEnd_ != std::chrono::steady_clock::time_point{}) {
      perf::countCompletionNoEnqueueWaitToNextEnqueue(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              enqueueTime - lastNoEnqueueCompletionWaitEnd_).count()),
          preparedDiagnostics.hasPresent);
      lastNoEnqueueCompletionWaitEnd_ = {};
      resetNoEnqueueGapProgressLocked();
    }
    PendingCompletion pending;
    pending.commandBuffer = std::move(record.commandBuffer);
    pending.diagnostics = preparedDiagnostics;
    pending.contextValue = record.context ? record.context : "queue";
    pending.slotIndex = slotIndices.front();
    pending.seqId = seqIds.back();
    pending.coalescedSlotIndices.assign(slotIndices.begin(), slotIndices.end());
    pending.coalescedSeqIds.assign(seqIds.begin(), seqIds.end());
    pending.commandBufferChainLength = record.commandBufferChainLength;
    pending.enqueueTime = enqueueTime;
    pending.renderEncoderGpuSampleBuffer =
        std::move(record.renderEncoderGpuSampleBuffer);
    pending.renderEncoderGpuSamples =
        std::move(record.renderEncoderGpuSamples);
    pending.completionCallbacks = std::move(record.completionCallbacks);
    pendingCompletion_.push_back(std::move(pending));
    const auto pendingDepthAfterPush = pendingCompletion_.size();
    if (completionWaitActive) {
      ++completionWaitEnqueues_;
    }
    perf::countCompletionEnqueue(static_cast<std::uint64_t>(pendingDepthAfterPush),
                                 completionWaitActive,
                                 preparedDiagnostics.hasPresent);
#ifndef NDEBUG
    assertPendingCompletionInvariantsLocked();
#endif
  }
  pendingCompletionCv_.notify_all();
}

bool QueueLifecycleController::processOnePendingCompletion(bool& stop) {
  PendingCompletion pending;
  size_t pendingDepthAfterPop = 0;
  {
    std::unique_lock<std::mutex> lock(pendingCompletionMutex_);
    pendingCompletionCv_.wait(lock, [&] { return stop || !pendingCompletion_.empty(); });
    if (stop && pendingCompletion_.empty()) {
      return false;
    }
    pending = std::move(pendingCompletion_.front());
    pendingCompletion_.pop_front();
    pendingDepthAfterPop = pendingCompletion_.size();
  }

  const WMTCommandBufferStatus dequeueStatus =
      pending.commandBuffer ? pending.commandBuffer.status()
                            : WMTCommandBufferStatusNotEnqueued;
  if (pending.enqueueTime != std::chrono::steady_clock::time_point{}) {
    const auto age = std::chrono::steady_clock::now() - pending.enqueueTime;
    perf::countCompletionDequeue(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(age).count()),
        static_cast<std::uint64_t>(pendingDepthAfterPop),
        static_cast<std::uint64_t>(dequeueStatus));
  }

  // Block until GPU completes — upstream dxmt's WaitForFinishThread pattern.
  if (pending.commandBuffer &&
      dequeueStatus <= WMTCommandBufferStatusScheduled) {
    {
      std::lock_guard lock(pendingCompletionMutex_);
      completionWaitActive_ = true;
      completionWaitEnqueues_ = 0;
    }
    const auto started = std::chrono::steady_clock::now();
    pending.commandBuffer.waitUntilCompleted();
    const auto completed = std::chrono::steady_clock::now();
    const auto elapsed = completed - started;
    const auto elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    std::uint64_t enqueuesDuringWait = 0;
    {
      std::lock_guard lock(pendingCompletionMutex_);
      enqueuesDuringWait = completionWaitEnqueues_;
      completionWaitActive_ = false;
      completionWaitEnqueues_ = 0;
      if (enqueuesDuringWait == 0) {
        lastNoEnqueueCompletionWaitEnd_ = completed;
        resetNoEnqueueGapProgressLocked();
      }
    }
    perf::countCompletionWaitStatus(elapsedNs,
                                    static_cast<std::uint64_t>(dequeueStatus));
    perf::countCompletionWaitOverlap(elapsedNs,
                                     enqueuesDuringWait,
                                     pending.diagnostics.hasPresent);
    perf::countCompletionWait(
        elapsedNs,
        pending.diagnostics.hasDraw,
        pending.diagnostics.hasPresent,
        pending.diagnostics.hasBlit,
        pending.diagnostics.hasStretchRect,
        pending.diagnostics.compatFlags,
        pending.diagnostics.vertexShaderHash,
        pending.diagnostics.pixelShaderHash,
        pending.diagnostics.shaderVariantHash);
  }

  delayCompletionSignalForPerfProbe();

  const auto binding = submissionBinding_;
  auto* diagnosticsController = binding.submissionDiagnostics;
  if (diagnosticsController && pending.commandBuffer) {
    (void)diagnosticsController->observeQueueSubmission(pending.commandBuffer.handle,
                                                        pending.diagnostics,
                                                        pending.contextValue.c_str());
  }
  if (pending.renderEncoderGpuSampleBuffer &&
      !pending.renderEncoderGpuSamples.empty()) {
    std::uint32_t sampleCount = 0;
    for (const auto& sample : pending.renderEncoderGpuSamples) {
      sampleCount = std::max(sampleCount, sample.endIndex + 1u);
    }
    if (sampleCount > 0) {
      std::vector<std::uint64_t> timestamps(sampleCount, 0);
      pending.renderEncoderGpuSampleBuffer.resolveCounterRange(
          0, sampleCount, timestamps.data(),
          timestamps.size() * sizeof(timestamps[0]));
      for (const auto& sample : pending.renderEncoderGpuSamples) {
        if (sample.startIndex >= timestamps.size() ||
            sample.endIndex >= timestamps.size()) {
          continue;
        }
        const auto start = timestamps[sample.startIndex];
        const auto end = timestamps[sample.endIndex];
        if (start == 0 || end <= start) {
          continue;
        }
        perf::countRenderEncoderGpuTime(
            end - start,
            static_cast<std::uint32_t>(sample.passType),
            sample.rtHandle,
            sample.depthHandle,
            sample.psoHandle);
      }
    }
  }
  for (auto& callback : pending.completionCallbacks) {
    if (callback) {
      callback();
    }
  }
  if (binding.mutex && binding.completedSeqQueue) {
    std::lock_guard completionLock(*binding.mutex);
    std::array<size_t, 1> singleSlotIndex{pending.slotIndex};
    std::array<u64, 1> singleSeqId{pending.seqId};
    const bool hasCoalescedSlots =
        !pending.coalescedSlotIndices.empty() &&
        pending.coalescedSlotIndices.size() == pending.coalescedSeqIds.size();
    std::span<const size_t> slotIndices =
        hasCoalescedSlots
            ? std::span<const size_t>(pending.coalescedSlotIndices.data(),
                                      pending.coalescedSlotIndices.size())
            : std::span<const size_t>(singleSlotIndex.data(), singleSlotIndex.size());
    std::span<const u64> seqIds =
        hasCoalescedSlots
            ? std::span<const u64>(pending.coalescedSeqIds.data(),
                                   pending.coalescedSeqIds.size())
            : std::span<const u64>(singleSeqId.data(), singleSeqId.size());

    for (std::size_t i = 0; i < seqIds.size(); ++i) {
      const QueueControllerState before = makeBoundQueueState(binding);
      // TLA+: QueueLifecycleRefinement / GpuComplete.
      DXMT_ASSERT(seqIds[i] == (binding.completedSeqId ? *binding.completedSeqId : 0) +
                                  binding.completedSeqQueue->size() + 1);
      binding.completedSeqQueue->push_back(seqIds[i]);
      if (pending.diagnostics.hasPresent && i + 1 == seqIds.size() &&
          binding.completedPresentSeqQueue) {
        binding.completedPresentSeqQueue->push_back(seqIds[i]);
      }
      const QueueControllerState after = makeBoundQueueState(binding);
#ifndef NDEBUG
      assertQueueLifecycleInvariants();
#endif
      observeTransition(QueueTransitionRecord{
          .before = before,
          .after = after,
          .slotIndex = slotIndices[i],
          .eventSeqId = seqIds[i],
      });
    }
    if (binding.finishCv) {
      binding.finishCv->notify_all();
    }
  }
  // pending.commandBuffer is released when `pending` goes out of scope.
  return true;
}

void QueueLifecycleController::notePresentEnqueue(const QueueControllerState& state,
                                                  size_t slotIndex,
                                                  u64 eventSeqId,
                                                  const SwapDesc& present,
                                                  Handle sourceHandle) const {
  if (queueTraceEnabled()) {
    std::ostringstream out;
    out << "[dxmt9-present] enqueue"
        << " hwnd=" << static_cast<unsigned long long>(present.window.value)
        << " source=0x" << std::hex << static_cast<unsigned long long>(sourceHandle.value) << std::dec
        << " size=" << present.width << "x" << present.height;
    emitQueueTraceLine(out.str());
  }
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::PresentEnqueue, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWriterWaitBeginIfNeeded(const QueueControllerState& state,
                                                           size_t slotIndex,
                                                           u64 eventSeqId,
                                                           size_t inflightLimit) const {
  if (slotIndex >= state.slots.size()) {
    return;
  }
  if (state.slots[slotIndex].state == ChunkSlot::State::Free && state.inflightCount < inflightLimit) {
    return;
  }
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WriterWaitBegin, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWriterWaitEnd(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WriterWaitEnd, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWriterAcquire(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WriterAcquire, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteCommitEmpty(const QueueControllerState& state,
                                               size_t slotIndex,
                                               u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::CommitEmpty, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteCommitWaitBeginIfNeeded(const QueueControllerState& state,
                                                           size_t slotIndex,
                                                           u64 eventSeqId,
                                                           size_t inflightLimit) const {
  if (state.inflightCount < inflightLimit) {
    return;
  }
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::CommitWaitBegin, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteCommitWaitEnd(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::CommitWaitEnd, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteCommitPublish(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::CommitPublish, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteEncodeDequeue(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::EncodeDequeue, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteEncodeCommit(const QueueControllerState& state,
                                                size_t slotIndex,
                                                u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::EncodeCommit, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteGpuComplete(const QueueControllerState& state,
                                               size_t slotIndex,
                                               u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::GpuComplete, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteFinishInline(const QueueControllerState& state,
                                                size_t slotIndex,
                                                u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::FinishInline, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteFinishDequeue(const QueueControllerState& state,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::FinishDequeue, std::nullopt, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteReclaimFree(const QueueControllerState& state,
                                               size_t slotIndex,
                                               u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::ReclaimFree, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWaitSeqBeginIfNeeded(const QueueControllerState& state,
                                                        u64 targetSeqId) const {
  if (state.completedSeqId >= targetSeqId) {
    return;
  }
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WaitSeqBegin, std::nullopt, targetSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWaitSeqEnd(const QueueControllerState& state,
                                              u64 targetSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WaitSeqEnd, std::nullopt, targetSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

std::string CompletionTracker::commandBufferStatusName(WMTCommandBufferStatus status) const {
  switch (status) {
    case WMTCommandBufferStatusNotEnqueued:
      return "not-enqueued";
    case WMTCommandBufferStatusEnqueued:
      return "enqueued";
    case WMTCommandBufferStatusCommitted:
      return "committed";
    case WMTCommandBufferStatusScheduled:
      return "scheduled";
    case WMTCommandBufferStatusCompleted:
      return "completed";
    case WMTCommandBufferStatusError:
      return "error";
  }
  return "unknown";
}

bool CompletionTracker::inspect(obj_handle_t commandBuffer,
                                const CommandBufferDiagnostics& diagnostics,
                                const char* context) {
  if (!commandBuffer) {
    return false;
  }

  WMT::CommandBuffer wrapped{commandBuffer};
  const WMTCommandBufferStatus status = wrapped.status();
  if (queueTraceEnabled()) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Debug, "dxmt9-metal",
                      "%s seq=%llu slot=%zu frame=%u status=%s draw=%d present=%d blit=%d stretch=%d",
                      context,
                      static_cast<unsigned long long>(diagnostics.seqId),
                      diagnostics.slotIndex,
                      diagnostics.frame,
                      commandBufferStatusName(status).c_str(),
                      diagnostics.hasDraw ? 1 : 0,
                      diagnostics.hasPresent ? 1 : 0,
                      diagnostics.hasBlit ? 1 : 0,
                      diagnostics.hasStretchRect ? 1 : 0);
  }

  if (status == WMTCommandBufferStatusError) {
    std::ostringstream summary;
    summary << context << " seq=" << diagnostics.seqId << " status=error";
    WMT::Error error = wrapped.error();
    if (error) {
      summary << " error=" << error.description().getUTF8String();
    }
    lastErrorSummary_ = summary.str();
    dxmt9::util::logLine(dxmt9::util::LogLevel::Error, "dxmt9-metal", lastErrorSummary_);
    // M5 — increment the GPU fault sentinel. Failures are rare so a
    // single counter increment per faulted command buffer is fine and
    // pairs with expected_counters{max=0} on perf probes (L3 gate).
    dxmt9::perf::countGpuCommandBufferError();
  } else if (status == WMTCommandBufferStatusCompleted) {
    // M4 — sample MTLCommandBuffer GPU wall time. Apple Silicon
    // returns nanosecond timestamps via MTLCommandBuffer_property; on
    // older / unsupported drivers the values can be 0 or non-monotonic,
    // so guard before recording.
    const std::uint64_t gpuStart = wrapped.gpuStartTime();
    const std::uint64_t gpuEnd = wrapped.gpuEndTime();
    if (gpuStart > 0 && gpuEnd > gpuStart) {
      dxmt9::perf::countGpuCommandBufferTime(gpuEnd - gpuStart);
    }
    if (diagnostics.hasPresent) {
      lastErrorSummary_.clear();
    }
  } else if (diagnostics.hasPresent) {
    lastErrorSummary_.clear();
  }

  WMT::LogContainer logs = wrapped.logs();
  if (logs) {
    for (WMT::Object logEntry : logs.elements()) {
      WMT::String description = logEntry.description();
      if (!description) {
        continue;
      }
      dxmt9::util::logf(dxmt9::util::LogLevel::Warn,
                        "dxmt9-metal",
                        "%s seq=%llu metal-log=%s",
                        context,
                        static_cast<unsigned long long>(diagnostics.seqId),
                        description.getUTF8String().c_str());
    }
  }

  return diagnostics.hasPresent || status == WMTCommandBufferStatusError;
}

}  // namespace dxmt9::core::metalqueue
