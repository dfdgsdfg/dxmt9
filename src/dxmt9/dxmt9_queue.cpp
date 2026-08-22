#include "dxmt9_queue.hpp"
#include "dxmt9_queue_mutex_diag.hpp"
#include "dxmt9_scheduling_progress_watchdog.hpp"

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
#include <iterator>
#include <sstream>
#include <thread>
#include <utility>

namespace dxmt9::core::metalqueue {

namespace {

using enum dxmt9::core::metalcompat::CompatFlagBits;

void recordCpuReadyTapeStats(const CpuReadyTape& tape) {
  const auto& stats = tape.stats();
  perf::recordCpuReadyTapeStats(
      stats.residentSources, stats.residentPages, stats.readyFifoEntries,
      stats.admissionCloses, stats.admissionReopens, stats.wrapPaddingPages);
}

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
      .readyCount = state.cpuReadyTape ? state.cpuReadyTape->readyCount() : 0,
      .completedQueueCount = state.completedSeqQueue ? state.completedSeqQueue->size() : 0,
      .inflightCount = state.inflightCount ? *state.inflightCount : 0,
      .completedSeqId = state.completedSeqId
                            ? state.completedSeqId->load(std::memory_order_relaxed)
                            : 0,
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

void traceEncodeIterationStage(const char* stage, size_t slotIndex,
                               const ChunkSlotControl& slot) {
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

void traceEncodeIterationStage(const char* stage, size_t slotIndex,
                               const ChunkSlot& slot) {
  if (!queueTraceEnabled()) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-encode-iteration]"
      << " stage=" << stage
      << " slot=" << slotIndex
      << " commands=" << slot.commandCount();
  emitQueueTraceLine(out.str());
}

bool commandRangeWithinSource(const SourcePayloadView& payload,
                              size_t commandBegin,
                              size_t commandCount) noexcept {
  if (!payload.valid()) {
    return false;
  }
  const size_t slotCommandCount = payload.commandCount();
  return commandBegin <= slotCommandCount &&
         commandCount <= slotCommandCount - commandBegin;
}

bool commandRangeHasPresent(const SourcePayloadView& payload,
                            size_t commandBegin,
                            size_t commandCount) noexcept {
  if (!commandRangeWithinSource(payload, commandBegin, commandCount)) {
    return false;
  }
  const size_t commandEnd = commandBegin + commandCount;
  for (size_t i = commandBegin; i < commandEnd; ++i) {
    if (payload.commandAt(i).kind() == MetalCommandKind::Present) {
      return true;
    }
  }
  return false;
}

ReadySlotSnapshot makeReadySlotSnapshot(const CpuReadyTape::ReadyEntry& ready,
                                        [[maybe_unused]] ChunkSlotControl& slot,
                                        SourcePayloadView payload) noexcept {
  // Prefix selection calls this only while the queue lock keeps the Ready
  // entry, both locator generations, and its sealed payload stable.
  DXMT_ASSERT(payload.valid());
  DXMT_ASSERT(slot.sourceId == ready.source.id);
  DXMT_ASSERT(slot.storage == ready.source.storage);
  DXMT_ASSERT(slot.seqId == ready.seqId);
  return ReadySlotSnapshot{
      .slotIndex = ready.controlIndex,
      .seqId = ready.seqId,
      .metadata = ready.metadata,
      .semantic = ready.semantic,
      .hasPresent = commandRangeHasPresent(payload, 0, payload.commandCount()),
      .commandBegin = 0,
      .commandCount = payload.commandCount(),
      .sourceId = ready.source.id,
      .storage = ready.source.storage,
  };
}

}  // namespace

QueueCompletionSource completionSourceForReadySlot(
    const ReadySlotSnapshot& snapshot) noexcept {
  DXMT_ASSERT(snapshot.sourceId.valid());
  DXMT_ASSERT(snapshot.storage.valid());
  return QueueCompletionSource{
      .source = CpuReadyTape::SourceRef{
          .id = snapshot.sourceId,
          .storage = snapshot.storage,
      },
      .slotIndex = snapshot.slotIndex,
      .seqId = snapshot.seqId,
      .hasPresent = snapshot.hasPresent,
      .commandBegin = snapshot.commandBegin,
      .commandCount = snapshot.commandCount,
  };
}

bool queueCompletionSourceExactlyEqual(
    const QueueCompletionSource& left,
    const QueueCompletionSource& right) noexcept {
  return left.source == right.source &&
         left.receipt == right.receipt &&
         left.slotIndex == right.slotIndex && left.seqId == right.seqId &&
         left.hasPresent == right.hasPresent &&
         left.commandBegin == right.commandBegin &&
         left.commandCount == right.commandCount;
}

bool queueCompletionSourceSpansExactlyEqual(
    std::span<const QueueCompletionSource> left,
    std::span<const QueueCompletionSource> right) noexcept {
  return left.size() == right.size() &&
      std::equal(left.begin(), left.end(), right.begin(),
                 queueCompletionSourceExactlyEqual);
}

namespace {

std::optional<encoders::UnverifiedEncodedCompletionSpan>
deriveLocatorFreeCompletionProjection(
    std::span<const QueueCompletionSource> sources) noexcept {
  if (sources.empty() || sources.size() > kMaxEncodeSessionSources) {
    return std::nullopt;
  }
  encoders::SessionCompletionAccumulator accumulator(
      kMaxEncodeSessionSources);
  for (const auto& source : sources) {
    if (accumulator.append(source.seqId, source.hasPresent) !=
        encoders::CompletionSpanAppendResult::Appended) {
      return std::nullopt;
    }
  }
  const auto projection = accumulator.summary();
  return projection && projection->sourceCount() == sources.size()
      ? projection
      : std::nullopt;
}

}  // namespace

class QueueCompletionSpanAuthority {
 public:
  static std::optional<encoders::EncodedCompletionSpan> sealAssignedOldList(
      const EncodeSessionSourceList& sources) noexcept {
    const auto projection =
        deriveLocatorFreeCompletionProjection(sources.span());
    if (!projection) {
      return std::nullopt;
    }
    perf::countCompletionSpanShadowBuilt(sources.size());
    return encoders::EncodedCompletionSpan(
        projection->firstSeqId(), projection->lastSeqId(),
        projection->sourceCount(), projection->tailHasPresent());
  }
};

bool encodedCompletionSpanShadowMatchesProjection(
    const std::optional<encoders::EncodedCompletionSpan>& shadow,
    std::span<const QueueCompletionSource> sources) noexcept {
  if (sources.empty()) {
    const bool matches = !shadow.has_value();
    if (matches) {
      perf::countCompletionSpanShadowValidated();
    } else {
      perf::countCompletionSpanShadowMismatch();
    }
    return matches;
  }
  const auto projection = deriveLocatorFreeCompletionProjection(sources);
  const bool matches = shadow.has_value() && projection.has_value() &&
      shadow->firstSeqId() == projection->firstSeqId() &&
      shadow->lastSeqId() == projection->lastSeqId() &&
      shadow->sourceCount() == projection->sourceCount() &&
      shadow->tailHasPresent() == projection->tailHasPresent();
  if (matches) {
    perf::countCompletionSpanShadowValidated();
  } else {
    perf::countCompletionSpanShadowMismatch();
  }
  return matches;
}

bool QueueSubmissionRecord::assignFixedCompletionSources(
    std::span<const QueueCompletionSource> sources) noexcept {
  if (sources.empty()) {
    clearFixedCompletionSources();
    return true;
  }
  EncodeSessionSourceList staged;
  if (!staged.assign(sources)) {
    return false;
  }
  auto shadow = QueueCompletionSpanAuthority::sealAssignedOldList(staged);
  if (!shadow) {
    return false;
  }
  fixedCompletionSources = staged;
  completionSpanShadow = *shadow;
  return true;
}

bool hasExactRedundantFixedCompletionSources(
    const QueueSubmissionRecord& record,
    std::span<const QueueCompletionSource> pendingSources,
    std::span<const QueueCompletionSource> sessionSources) noexcept {
  const auto explicitSources = record.explicitCompletionSourceSpan();
  return record.completionSpanShadowMatchesSources() &&
      !explicitSources.empty() &&
      queueCompletionSourceSpansExactlyEqual(explicitSources,
                                              pendingSources) &&
      queueCompletionSourceSpansExactlyEqual(pendingSources,
                                              sessionSources);
}

bool assignOrValidateSingleCompletionSource(
    QueueSubmissionRecord& record,
    const ReadySlotSnapshot& source) noexcept {
  const QueueCompletionSource expected = completionSourceForReadySlot(source);
  if (record.fixedCompletionSources.empty()) {
    const std::array completionSources{expected};
    return record.assignFixedCompletionSources(completionSources);
  }
  if (record.fixedCompletionSources.size() != 1u) {
    return false;
  }
  const QueueCompletionSource& actual =
      record.fixedCompletionSources.entries[0];
  return record.completionSpanShadowMatchesSources() &&
      queueCompletionSourceExactlyEqual(actual, expected);
}

void appendCompletionSourcesToQueues(
    std::deque<u64>& completedSeqQueue,
    std::deque<u64>* completedPresentSeqQueue,
    u64 completedSeqId,
    std::span<const QueueCompletionSource> sources) {
  (void)completedSeqId;
  for (const auto& source : sources) {
    DXMT_ASSERT(source.seqId == completedSeqId + completedSeqQueue.size() + 1);
    completedSeqQueue.push_back(source.seqId);
    if (source.hasPresent && completedPresentSeqQueue) {
      completedPresentSeqQueue->push_back(source.seqId);
    }
  }
}

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

NoEnqueueFirstPublishSlotShape summarizeNoEnqueueFirstPublishSlotShape(
    const ChunkSlot& slot) noexcept {
  const auto commandCount = slot.commandCount();
  const auto drawRunCommands = slot.drawRunRecords.size();
  u64 prePresentCommands = 0;
  u64 prePresentDrawRunCommands = 0;
  u64 prePresentDrawItems = 0;
  u64 prePresentPayloadBytes = 0;
  u64 postPresentCommands = 0;
  bool foundPresent = false;

  for (std::size_t index = 0; index < commandCount; ++index) {
    const auto command = slot.commandAt(index);
    if (!foundPresent && command.kind == MetalCommandKind::Present) {
      foundPresent = true;
      continue;
    }
    if (foundPresent) {
      ++postPresentCommands;
      continue;
    }

    ++prePresentCommands;
    if (command.kind == MetalCommandKind::DrawRun) {
      ++prePresentDrawRunCommands;
      prePresentDrawItems += static_cast<u64>(drawRunDrawCount(command));
      prePresentPayloadBytes += static_cast<u64>(drawRunPayloadSize(command));
    }
  }
  const bool presentTail = foundPresent && postPresentCommands == 0;
  const auto prePresentNonDrawCommands =
      prePresentCommands >= prePresentDrawRunCommands
          ? prePresentCommands - prePresentDrawRunCommands
          : 0;
  return NoEnqueueFirstPublishSlotShape{
      .commandCount = static_cast<u64>(commandCount),
      .drawRunCommands = static_cast<u64>(drawRunCommands),
      .drawItems = static_cast<u64>(slot.drawParams.size()),
      .nonDrawCommands = static_cast<u64>(
          commandCount >= drawRunCommands ? commandCount - drawRunCommands : 0),
      .payloadBytes = static_cast<u64>(slot.drawPayloadArena.size()),
      .presentCommands = static_cast<u64>(slot.presentRecords.size()),
      .prePresentCommands = prePresentCommands,
      .prePresentDrawRunCommands = prePresentDrawRunCommands,
      .prePresentDrawItems = prePresentDrawItems,
      .prePresentNonDrawCommands = prePresentNonDrawCommands,
      .prePresentPayloadBytes = prePresentPayloadBytes,
      .postPresentCommands = postPresentCommands,
      .presentTailSlots = presentTail ? 1u : 0u,
      .presentNonTailSlots = foundPresent && !presentTail ? 1u : 0u,
  };
}

CommandBufferDiagnostics mergeCommandBufferDiagnostics(
    CommandBufferDiagnostics aggregate,
    const CommandBufferDiagnostics& source) noexcept {
  if (aggregate.seqId == 0) {
    aggregate.seqId = source.seqId;
    aggregate.slotIndex = source.slotIndex;
  }
  aggregate.hasDraw = aggregate.hasDraw || source.hasDraw;
  aggregate.hasPresent = aggregate.hasPresent || source.hasPresent;
  aggregate.hasBlit = aggregate.hasBlit || source.hasBlit;
  aggregate.hasStretchRect = aggregate.hasStretchRect || source.hasStretchRect;
  if (aggregate.frame == 0) {
    aggregate.frame = source.frame;
  }
  aggregate.compatFlags |= source.compatFlags;
  if (source.vertexShaderHash != 0) {
    aggregate.vertexShaderHash = source.vertexShaderHash;
  }
  if (source.pixelShaderHash != 0) {
    aggregate.pixelShaderHash = source.pixelShaderHash;
  }
  if (source.shaderVariantHash != 0) {
    aggregate.shaderVariantHash = source.shaderVariantHash;
  }
  return aggregate;
}

template <typename T>
void prependMoved(std::vector<T>& target, std::vector<T>&& prefix) {
  if (prefix.empty()) {
    return;
  }
  std::vector<T> merged;
  merged.reserve(prefix.size() + target.size());
  std::move(prefix.begin(), prefix.end(), std::back_inserter(merged));
  std::move(target.begin(), target.end(), std::back_inserter(merged));
  target = std::move(merged);
}

bool canFoldEncodedSessionFragmentCarrier(
    const QueueSubmissionRecord& newTail,
    const QueueSubmissionRecord& oldCarrier,
    obj_handle_t injectedCommandBuffer,
    bool oldCarrierTailAlreadyCommitted) noexcept {
  if (!newTail.completionSpanShadowMatchesSources() ||
      !oldCarrier.completionSpanShadowMatchesSources()) {
    return false;
  }
  const obj_handle_t expectedCommandBuffer =
      injectedCommandBuffer != NULL_OBJECT_HANDLE
      ? injectedCommandBuffer
      : oldCarrier.commandBuffer.handle;
  if (expectedCommandBuffer != NULL_OBJECT_HANDLE) {
    if (!newTail.commandBuffer ||
        (newTail.commandBuffer.handle != expectedCommandBuffer &&
         !oldCarrierTailAlreadyCommitted)) {
      return false;
    }
  }
  if (oldCarrier.renderEncoderGpuSampleBuffer &&
      newTail.renderEncoderGpuSampleBuffer &&
      oldCarrier.renderEncoderGpuSampleBuffer.handle !=
          newTail.renderEncoderGpuSampleBuffer.handle) {
    return false;
  }
  return !(oldCarrier.metalCapture.has_value() &&
           newTail.metalCapture.has_value());
}

void foldEncodedSessionFragmentCarrierPrepared(
    QueueSubmissionRecord& newTail,
    QueueSubmissionRecord& oldCarrier) {
  if (!newTail.commandBuffer && oldCarrier.commandBuffer) {
    newTail.commandBuffer = std::move(oldCarrier.commandBuffer);
  }
  if (!newTail.renderEncoderGpuSampleBuffer &&
      oldCarrier.renderEncoderGpuSampleBuffer) {
    newTail.renderEncoderGpuSampleBuffer =
        std::move(oldCarrier.renderEncoderGpuSampleBuffer);
  }

  const u64 oldChainLength =
      std::max<u64>(1, oldCarrier.commandBufferChainLength);
  const u64 newChainLength =
      std::max<u64>(1, newTail.commandBufferChainLength);
  newTail.commandBufferChainLength = oldChainLength + newChainLength - 1u;

  CommandBufferDiagnostics oldDiagnostics = oldCarrier.diagnostics;
  if (oldDiagnostics.seqId == 0) {
    oldDiagnostics.seqId = oldCarrier.seqId;
    oldDiagnostics.slotIndex = oldCarrier.slotIndex;
  }
  CommandBufferDiagnostics newDiagnostics = newTail.diagnostics;
  if (newDiagnostics.seqId == 0) {
    newDiagnostics.seqId = newTail.seqId;
    newDiagnostics.slotIndex = newTail.slotIndex;
  }
  CommandBufferDiagnostics mergedDiagnostics{
      .seqId = newTail.seqId,
      .slotIndex = newTail.slotIndex,
  };
  mergedDiagnostics = mergeCommandBufferDiagnostics(
      mergedDiagnostics, oldDiagnostics);
  mergedDiagnostics = mergeCommandBufferDiagnostics(
      mergedDiagnostics, newDiagnostics);
  newTail.diagnostics = mergedDiagnostics;

  prependMoved(newTail.renderEncoderGpuSamples,
               std::move(oldCarrier.renderEncoderGpuSamples));
  prependMoved(newTail.postCommitCallbacks,
               std::move(oldCarrier.postCommitCallbacks));
  prependMoved(newTail.completionCallbacks,
               std::move(oldCarrier.completionCallbacks));
  prependMoved(newTail.retainedPayloads,
               std::move(oldCarrier.retainedPayloads));

  if (oldCarrier.metalCapture.has_value()) {
    newTail.metalCaptureDevice = oldCarrier.metalCaptureDevice;
    newTail.metalCapture = std::move(oldCarrier.metalCapture);
    newTail.metalCaptureAlreadyStarted =
        oldCarrier.metalCaptureAlreadyStarted;
  } else {
    newTail.metalCaptureAlreadyStarted =
        newTail.metalCaptureAlreadyStarted ||
        oldCarrier.metalCaptureAlreadyStarted;
  }
}

bool foldEncodedSessionFragmentCarrier(
    QueueSubmissionRecord& newTail,
    QueueSubmissionRecord& oldCarrier,
    obj_handle_t injectedCommandBuffer) {
  const obj_handle_t expectedCommandBuffer =
      injectedCommandBuffer != NULL_OBJECT_HANDLE
      ? injectedCommandBuffer
      : oldCarrier.commandBuffer.handle;
  const bool differentLiveTail =
      expectedCommandBuffer != NULL_OBJECT_HANDLE && newTail.commandBuffer &&
      expectedCommandBuffer != newTail.commandBuffer.handle;
  const bool oldCarrierTailAlreadyCommitted =
      differentLiveTail && newTail.commandBufferChainLength > 1u;
  if (!canFoldEncodedSessionFragmentCarrier(
          newTail, oldCarrier, injectedCommandBuffer,
          oldCarrierTailAlreadyCommitted)) {
    return false;
  }
  foldEncodedSessionFragmentCarrierPrepared(newTail, oldCarrier);
  return true;
}

bool mergeEncodedPendingTailSubmission(
    QueueSubmissionRecord& tail,
    QueueSubmissionRecord& encodedHead,
    std::span<const QueueCompletionSource> encodedHeadSources,
    QueueCompletionSource tailSource,
    bool encodedHeadTailAlreadyCommitted,
    EncodeSessionSourceList* mergedSourcesOut) {
  if (encodedHeadSources.empty() ||
      tailSource.seqId == 0 ||
      tail.seqId == 0 ||
      tailSource.seqId != tail.seqId ||
      tailSource.slotIndex != tail.slotIndex) {
    return false;
  }

  u64 previousSeqId = 0;
  for (const auto& source : encodedHeadSources) {
    if (source.seqId == 0 ||
        (previousSeqId != 0 && source.seqId != previousSeqId + 1)) {
      return false;
    }
    previousSeqId = source.seqId;
  }
  if (tailSource.seqId != previousSeqId + 1) {
    return false;
  }

  std::array<QueueCompletionSource, 1> fallbackTailSources{tailSource};
  std::span<const QueueCompletionSource> tailSources(
      fallbackTailSources.data(), fallbackTailSources.size());
  const auto explicitTailSourceSpan = tail.explicitCompletionSourceSpan();
  if (!explicitTailSourceSpan.empty()) {
    std::span<const QueueCompletionSource> explicitTailSources =
        explicitTailSourceSpan;
    if (explicitTailSources.size() > encodedHeadSources.size() &&
        queueCompletionSourceSpansExactlyEqual(
            explicitTailSources.first(encodedHeadSources.size()),
            encodedHeadSources)) {
      explicitTailSources = explicitTailSources.subspan(
          encodedHeadSources.size());
    }
    tailSources = explicitTailSources;
  }
  if (tailSources.empty() || tailSources.front().seqId != tailSource.seqId) {
    return false;
  }
  previousSeqId = encodedHeadSources.back().seqId;
  for (const auto& source : tailSources) {
    if (source.seqId != previousSeqId + 1) {
      return false;
    }
    previousSeqId = source.seqId;
  }

  if (encodedHead.commandBuffer && tail.commandBuffer &&
      encodedHead.commandBuffer.handle != tail.commandBuffer.handle) {
    if (!encodedHeadTailAlreadyCommitted) {
      return false;
    }
  }
  if (!canFoldEncodedSessionFragmentCarrier(
          tail, encodedHead, encodedHead.commandBuffer.handle,
          encodedHeadTailAlreadyCommitted)) {
    return false;
  }

  EncodeSessionSourceList mergedSourceList;
  for (const auto& source : encodedHeadSources) {
    if (!mergedSourceList.append(source)) {
      return false;
    }
  }
  for (const auto& source : tailSources) {
    if (!mergedSourceList.append(source)) {
      return false;
    }
  }
  const auto mergedSourceSpan = mergedSourceList.span();

  if (!tail.assignFixedCompletionSources(mergedSourceSpan)) {
    return false;
  }
  if (mergedSourcesOut) {
    *mergedSourcesOut = mergedSourceList;
  }

  foldEncodedSessionFragmentCarrierPrepared(tail, encodedHead);

  return true;
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
  const auto& control = submissionBinding_.slots[slotIndex];
  const CpuReadyTape::State tapeState =
      control.state == ChunkSlot::State::GPU
          ? CpuReadyTape::State::GPU
          : CpuReadyTape::State::Encoding;
  const auto payload = submissionBinding_.cpuReadyTape->resolveSourcePayload(
      control.sourceId, control.storage, tapeState);
  if (!payload.valid()) {
    return CommandBufferDiagnostics{.seqId = seqId, .slotIndex = slotIndex};
  }
  if (const auto* legacy = payload.legacyPayload()) {
    return summarizeCommands(seqId, slotIndex, *legacy, resolveSurfaceFlags);
  }
  CommandBufferDiagnostics result{.seqId = seqId, .slotIndex = slotIndex};
  for (std::size_t i = 0; i < payload.commandCount(); ++i) {
    switch (payload.commandAt(i).kind()) {
    case MetalCommandKind::DrawRun:
      result.hasDraw = true;
      break;
    case MetalCommandKind::Present:
      result.hasPresent = true;
      break;
    case MetalCommandKind::StretchRect:
      result.hasStretchRect = true;
      break;
    case MetalCommandKind::Clear:
    case MetalCommandKind::SurfaceCopy:
    case MetalCommandKind::Readback:
    case MetalCommandKind::ColorFill:
    case MetalCommandKind::DepthResolve:
      result.hasBlit = true;
      break;
    }
  }
  return result;
}

CommandBufferDiagnostics QueueLifecycleController::summarizeSubmissionSources(
    const QueueSubmissionRecord& record,
    std::span<const QueueCompletionSource> sources) const {
  if (sources.empty()) {
    return record.diagnostics.seqId == 0
        ? summarizeSubmission(record.seqId, record.slotIndex)
        : record.diagnostics;
  }

  CommandBufferDiagnostics result = record.diagnostics;
  result.seqId = record.seqId;
  result.slotIndex = record.slotIndex;
  for (const auto& source : sources) {
    if (source.receiptBacked()) {
      result.hasPresent = result.hasPresent || source.hasPresent;
      continue;
    }
    auto sourceDiagnostics = summarizeSubmission(source.seqId, source.slotIndex);
    sourceDiagnostics.hasPresent = sourceDiagnostics.hasPresent || source.hasPresent;
    result = mergeCommandBufferDiagnostics(result, sourceDiagnostics);
  }
  return result;
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
  const auto now = std::chrono::steady_clock::now();
  if (completionWaitActive_) {
    perf::countCompletionWaitCommitPublish();
    completionWaitCommitPublishTime_ = now;
  }
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommitPublishRecorded_) {
    return;
  }
  const auto elapsed = now - lastNoEnqueueCompletionWaitEnd_;
  perf::countCompletionNoEnqueueWaitToCommitPublish(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
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

bool QueueLifecycleController::completionWaitActive() {
  std::lock_guard lock(pendingCompletionMutex_);
  return completionWaitActive_;
}

bool QueueLifecycleController::producerSequenceWaitActive() {
  std::lock_guard lock(pendingCompletionMutex_);
  return producerSequenceWaitDepth_ != 0;
}

bool QueueLifecycleController::producerWriterPressureActive() {
  std::lock_guard lock(pendingCompletionMutex_);
  return producerWriterPressureDepth_ != 0;
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
  const auto now = std::chrono::steady_clock::now();
  if (completionWaitActive_) {
    perf::countCompletionWaitEncodeDequeue();
    if (completionWaitCommitPublishTime_ != std::chrono::steady_clock::time_point{}) {
      perf::countCompletionWaitStagePublishToEncodeDequeue(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              now - completionWaitCommitPublishTime_).count()));
    }
    completionWaitEncodeDequeueTime_ = now;
  }
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapEncodeDequeueRecorded_) {
    return;
  }
  const auto elapsed = now - lastNoEnqueueCompletionWaitEnd_;
  perf::countCompletionNoEnqueueWaitToEncodeDequeue(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
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
  const auto now = std::chrono::steady_clock::now();
  if (completionWaitActive_) {
    perf::countCompletionWaitCommandBufferCommit();
    if (completionWaitEncodeDequeueTime_ != std::chrono::steady_clock::time_point{}) {
      perf::countCompletionWaitStageEncodeDequeueToCommandBufferCommit(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              now - completionWaitEncodeDequeueTime_).count()));
    }
  }
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommandBufferCommitRecorded_) {
    return;
  }
  const auto elapsed = now - lastNoEnqueueCompletionWaitEnd_;
  perf::countCompletionNoEnqueueWaitToCommandBufferCommit(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  if (noEnqueueGapEncodeDequeueTime_ != std::chrono::steady_clock::time_point{}) {
    perf::countCompletionNoEnqueueStageEncodeDequeueToCommandBufferCommit(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - noEnqueueGapEncodeDequeueTime_).count()));
  }
  noEnqueueGapCommandBufferCommitRecorded_ = true;
}

void QueueLifecycleController::recordCompletionWaitCommitChunkEntry() {
  std::lock_guard lock(pendingCompletionMutex_);
  if (!completionWaitActive_) {
    return;
  }
  perf::countCompletionWaitCommitChunkEntry();
}

void QueueLifecycleController::recordCompletionWaitCommitChunkReplayStart() {
  std::lock_guard lock(pendingCompletionMutex_);
  if (!completionWaitActive_) {
    return;
  }
  perf::countCompletionWaitCommitChunkReplayStart();
}

void QueueLifecycleController::recordCompletionWaitCommitChunkReplayEnd(
    std::uint64_t replayNanoseconds) {
  std::lock_guard lock(pendingCompletionMutex_);
  if (!completionWaitActive_) {
    return;
  }
  perf::countCompletionWaitCommitChunkReplayEnd(replayNanoseconds);
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

void QueueLifecycleController::recordNoEnqueueFirstPublishSlotShapeBeforePublish(
    const NoEnqueueFirstPublishSlotShape& shape) {
  std::lock_guard lock(pendingCompletionMutex_);
  if (lastNoEnqueueCompletionWaitEnd_ == std::chrono::steady_clock::time_point{} ||
      noEnqueueGapCommitPublishRecorded_) {
    return;
  }
  perf::countCompletionNoEnqueueFirstPublishSlotShape(
      shape.commandCount,
      shape.drawRunCommands,
      shape.drawItems,
      shape.nonDrawCommands,
      shape.payloadBytes,
      shape.presentCommands,
      shape.prePresentCommands,
      shape.prePresentDrawRunCommands,
      shape.prePresentDrawItems,
      shape.prePresentNonDrawCommands,
      shape.prePresentPayloadBytes,
      shape.postPresentCommands,
      shape.presentTailSlots,
      shape.presentNonTailSlots);
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

void QueueLifecycleController::noteCpuReadyCapacityProgress() noexcept {
  ++cpuReadyCapacityProgressGeneration_;
  if (cpuReadyCapacityProgressGeneration_ == 0) {
    ++cpuReadyCapacityProgressGeneration_;
  }
  if (submissionBinding_.encodeCv) {
    submissionBinding_.encodeCv->notify_all();
  }
}

void QueueLifecycleController::poisonTapeFailureLocked(
    std::source_location location) noexcept {
  bool expected = false;
  if (firstPoisonOriginClaimed_.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    firstPoisonOriginFile_.store(location.file_name(),
                                 std::memory_order_relaxed);
    firstPoisonOriginFunction_.store(location.function_name(),
                                     std::memory_order_relaxed);
    firstPoisonOriginLine_.store(static_cast<std::uint32_t>(location.line()),
                                 std::memory_order_relaxed);
    firstPoisonOriginColumn_.store(
        static_cast<std::uint32_t>(location.column()),
        std::memory_order_relaxed);
    firstPoisonOriginPublished_.store(true, std::memory_order_release);
  }
  // Lifecycle callers hold the queue scheduling mutex while mutating the
  // tape. Stop both admission surfaces under the same contract so a release
  // build cannot admit new work after a failed lifecycle mutation.
  if (submissionBinding_.cpuReadyTape) {
    submissionBinding_.cpuReadyTape->stopAdmission();
  }
  if (submissionBinding_.stop) {
    *submissionBinding_.stop = true;
  }
  if (submissionBinding_.schedulingProgressWatchdog) {
    submissionBinding_.schedulingProgressWatchdog->noteTerminal(true);
  }
  const auto wake = render::planSchedulingTerminalWake(
      render::SchedulingTerminalDisposition::DeviceLoss);
  if (wake.wakes(render::SchedulingWakeWriter) &&
      submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  if (wake.wakes(render::SchedulingWakeEncoder) &&
      submissionBinding_.encodeCv) {
    submissionBinding_.encodeCv->notify_all();
  }
  if (wake.wakes(render::SchedulingWakeFinish) &&
      submissionBinding_.finishCv) {
    submissionBinding_.finishCv->notify_all();
  }
  if (wake.wakes(render::SchedulingWakePresentCompleted) &&
      submissionBinding_.presentCompletedCv) {
    submissionBinding_.presentCompletedCv->notify_all();
  }
  if (wake.wakes(render::SchedulingWakePresentDequeued) &&
      submissionBinding_.presentDequeuedCv) {
    submissionBinding_.presentDequeuedCv->notify_all();
  }
  if (wake.wakes(render::SchedulingWakeSessionRelease) &&
      submissionBinding_.sessionReleaseCv) {
    submissionBinding_.sessionReleaseCv->notify_all();
  }
  if (wake.wakes(render::SchedulingWakePendingCompletion)) {
    requestPendingCompletionStop();
  }
}

void QueueLifecycleController::poisonTapeFailure(
    std::source_location location) noexcept {
  if (submissionBinding_.mutex) {
    std::lock_guard lock(*submissionBinding_.mutex);
    poisonTapeFailureLocked(location);
    return;
  }
  poisonTapeFailureLocked(location);
}

void QueueLifecycleController::requestPendingCompletionStop() noexcept {
  {
    std::lock_guard lock(pendingCompletionMutex_);
    pendingCompletionStop_ = true;
  }
  pendingCompletionCv_.notify_all();
}

void QueueLifecycleController::resetPendingCompletionStop() noexcept {
  std::lock_guard lock(pendingCompletionMutex_);
  pendingCompletionStop_ = false;
}

bool QueueLifecycleController::ensureWriterSlot(std::unique_lock<std::mutex>& lock,
                                                size_t inflightLimit) {
  // TLA+: QueueLifecycleRefinement / WriterAcquire.
  auto* writingSlot = submissionBinding_.writingSlot;
  auto* writeIndex = submissionBinding_.writeIndex;
  auto* inflightCount = submissionBinding_.inflightCount;
  auto* writeCv = submissionBinding_.writeCv;
  auto* stop = submissionBinding_.stop;
  auto* cpuReadyTape = submissionBinding_.cpuReadyTape;
  if (!writingSlot || !writeIndex || !inflightCount || !writeCv || !stop ||
      !cpuReadyTape) {
    return false;
  }

  if (writingSlot->has_value()) {
    return true;
  }

  auto& slots = submissionBinding_.slots;
  bool waitNeeded = false;
  bool writerPressureRegistered = false;
  auto beginWriterPressure = [&] {
    if (writerPressureRegistered) {
      return;
    }
    {
      std::lock_guard pendingLock(pendingCompletionMutex_);
      ++producerWriterPressureDepth_;
    }
    writerPressureRegistered = true;
    if (submissionBinding_.encodeCv) {
      submissionBinding_.encodeCv->notify_one();
    }
  };
  auto endWriterPressure = [&] {
    if (!writerPressureRegistered) {
      return;
    }
    {
      std::lock_guard pendingLock(pendingCompletionMutex_);
      DXMT_ASSERT(producerWriterPressureDepth_ > 0);
      if (producerWriterPressureDepth_ > 0) {
        --producerWriterPressureDepth_;
      }
    }
    writerPressureRegistered = false;
    if (submissionBinding_.encodeCv) {
      submissionBinding_.encodeCv->notify_one();
    }
  };
  const auto waitStarted = std::chrono::steady_clock::now();
  // SEGMENT-HOLD: the only interior lock release in this function is the
  // writeCv->wait(lock) below, called at most once per loop iteration. This
  // is the "ensureWritingSlot*"/QueueLifecycleController handoff site the
  // module comment in dxmt9_command_queue.cpp calls out for conversion --
  // reached from ensureWritingSlotUnlocked, in turn called by every
  // submit_* draw/clear/StretchRect/ColorFill/DepthResolve site and the
  // hot submit_draw_run_batch_impl per-batch loop, i.e. per-present traffic.
  // A single "ensure_writer_slot" tag accumulates one sample per bracketed
  // interval (pre-wait bookkeeping through to the next wait, or through to
  // the final acquireWriterSlot() bookkeeping after the loop breaks).
  const bool qmxEnabled = dxmt9::queueMutexSplitEnabled();
  auto qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  // probeReserve() mutates the pressure latch, so keep it out of a condition
  // variable predicate. Each wake re-enters this loop under the scheduling
  // lock; only TemporaryPressure is a normal reason to wait.
  for (;;) {
    if (*stop) {
      endWriterPressure();
      dxmt9::noteQueueMutexSegmentIfEnabled("ensure_writer_slot", qmxEnabled,
                                            qmxSegStart);
      return false;
    }
    const auto probe = cpuReadyTape->probeReserve();
    if (probe == CpuReadyTape::ReserveProbe::InvalidRequest ||
        probe == CpuReadyTape::ReserveProbe::Corrupt) {
      endWriterPressure();
      poisonTapeFailureLocked();
      dxmt9::noteQueueMutexSegmentIfEnabled("ensure_writer_slot", qmxEnabled,
                                            qmxSegStart);
      return false;
    }
    if (probe == CpuReadyTape::ReserveProbe::Stopped) {
      endWriterPressure();
      if (!*stop) {
        poisonTapeFailureLocked();
      }
      dxmt9::noteQueueMutexSegmentIfEnabled("ensure_writer_slot", qmxEnabled,
                                            qmxSegStart);
      return false;
    }
    if (probe == CpuReadyTape::ReserveProbe::Ready &&
        slots[*writeIndex].state == ChunkSlot::State::Free) {
      break;
    }
    if (!waitNeeded) {
      waitNeeded = true;
      observeWriterWait(*writeIndex, slots[*writeIndex].seqId, inflightLimit);
      // The compatibility writer is now unable to publish the next Legacy
      // source. Register the whole real wait interval before dropping the
      // scheduling mutex; the encode lane may re-evaluate an existing ordered
      // event, but this observation cannot create a submission boundary.
      beginWriterPressure();
    }
    dxmt9::noteQueueMutexSegmentIfEnabled("ensure_writer_slot", qmxEnabled,
                                          qmxSegStart);
    writeCv->wait(lock);
    qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
  }
  endWriterPressure();
  if (waitNeeded) {
    const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
    perf::countQueueWriterWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  }
  if (*stop) {
    dxmt9::noteQueueMutexSegmentIfEnabled("ensure_writer_slot", qmxEnabled,
                                          qmxSegStart);
    return false;
  }
  observeWriterWait(*writeIndex, slots[*writeIndex].seqId, inflightLimit);
  acquireWriterSlot(*writeIndex, slots[*writeIndex].seqId, inflightLimit, [&] {
    const auto reservation = cpuReadyTape->reserve();
    DXMT_ASSERT(reservation.has_value());
    if (!reservation) {
      poisonTapeFailureLocked();
      return;
    }
    slots[*writeIndex].state = ChunkSlot::State::Writing;
    slots[*writeIndex].seqId = 0;
    slots[*writeIndex].sourceId = reservation->id;
    slots[*writeIndex].storage = reservation->storage;
    slots[*writeIndex].payload = reservation->payload;
    *writingSlot = *writeIndex;
    recordCpuReadyTapeStats(*cpuReadyTape);
  });
  dxmt9::noteQueueMutexSegmentIfEnabled("ensure_writer_slot", qmxEnabled,
                                        qmxSegStart);
  return writingSlot->has_value();
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
  if (!appendPresentCommand(present, sourceHandle)) {
    return;
  }
  (void)commitCurrentChunk(lock, inflightLimit, onBeforePublish);
}

void QueueLifecycleController::flushAndWait(
    std::unique_lock<std::mutex>& lock,
    size_t inflightLimit,
    const std::function<void(ChunkSlot&)>& onBeforePublish) {
  // TLA+: QueueLifecycleRefinement / CommitPublish then WaitForSequence.
  (void)commitCurrentChunk(lock, inflightLimit, onBeforePublish);

  auto* nextSeqId = submissionBinding_.nextSeqId;
  // Relaxed throughout this file: `lock` is held, and every writer of this
  // variable needs the same mutex, so the value cannot move under us.
  const u64 nextSeqIdValue =
      nextSeqId ? nextSeqId->load(std::memory_order_relaxed) : 0;
  const u64 targetSeqId = nextSeqIdValue == 0 ? 0 : nextSeqIdValue - 1;
  waitForSequence(lock, targetSeqId);
}

bool QueueLifecycleController::commitCurrentChunk(
    std::unique_lock<std::mutex>& lock,
    size_t inflightLimit,
    const std::function<void(ChunkSlot&)>& onBeforePublish) {
  // TLA+: QueueLifecycleRefinement / CommitEmpty or CommitPublish.
  DXMT_ASSERT(lock.owns_lock());
  static_cast<void>(lock);
  // SEGMENT-HOLD: this function owns exactly one interior lock-release point
  // (the writeCv->wait below). Reachable from the game/replay-offload
  // producer via CommandQueue::submitPresent, CommandQueue::mapBuffer, and
  // the draw-chunk/payload-limit helpers -- i.e. per-present traffic -- so
  // it is one of the "commitCurrentChunk / ensureWritingSlot*" sites the
  // module comment in dxmt9_command_queue.cpp calls out for conversion.
  // "pre_wait" covers this function's own bookkeeping (both the CommitEmpty
  // early-out and the CommitPublish setup); "post_wait" covers the
  // publish itself. Both accumulate as separate samples of the same two
  // tags across the early-return paths below.
  const bool qmxEnabled = dxmt9::queueMutexSplitEnabled();
  auto qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  auto* writingSlot = submissionBinding_.writingSlot;
  auto* writeIndex = submissionBinding_.writeIndex;
  auto* nextSeqId = submissionBinding_.nextSeqId;
  auto* inflightCount = submissionBinding_.inflightCount;
  auto* lastCommittedSeqId = submissionBinding_.lastCommittedSeqId;
  auto* writeCv = submissionBinding_.writeCv;
  auto* encodeCv = submissionBinding_.encodeCv;
  auto* stop = submissionBinding_.stop;
  if (!writingSlot || !writeIndex || !nextSeqId || !inflightCount ||
      !lastCommittedSeqId || !writeCv || !stop) {
    dxmt9::noteQueueMutexSegmentIfEnabled("commit_current_chunk/pre_wait",
                                          qmxEnabled, qmxSegStart);
    return false;
  }
  if (!writingSlot->has_value()) {
    dxmt9::noteQueueMutexSegmentIfEnabled("commit_current_chunk/pre_wait",
                                          qmxEnabled, qmxSegStart);
    return false;
  }

  auto& slots = submissionBinding_.slots;
  auto& slot = slots[**writingSlot];
  auto* writingPayload = submissionBinding_.cpuReadyTape->resolveForWrite(
      CpuReadyPublicationTicket{
          .id = slot.sourceId,
          .storage = slot.storage,
      });
  if (!writingPayload || writingPayload != slot.payload) {
    poisonTapeFailureLocked();
    dxmt9::noteQueueMutexSegmentIfEnabled("commit_current_chunk/pre_wait",
                                          qmxEnabled, qmxSegStart);
    return false;
  }
  if (writingPayload->commandsEmpty()) {
    const size_t slotIndex = **writingSlot;
    bool aborted = false;
    commitEmpty(slotIndex, slot.seqId, [&] {
      aborted = submissionBinding_.cpuReadyTape->abort(
          CpuReadyPublicationTicket{
              .id = slot.sourceId,
              .storage = slot.storage,
          });
      DXMT_ASSERT(aborted);
      if (!aborted) {
        poisonTapeFailureLocked();
        return;
      }
      slot.state = ChunkSlot::State::Free;
      slot.seqId = 0;
      slot.sourceId = {};
      slot.storage = {};
      slot.payload = nullptr;
      writingSlot->reset();
      recordCpuReadyTapeStats(*submissionBinding_.cpuReadyTape);
    });
    if (!aborted) {
      dxmt9::noteQueueMutexSegmentIfEnabled("commit_current_chunk/pre_wait",
                                            qmxEnabled, qmxSegStart);
      return false;
    }
    noteCpuReadyCapacityProgress();
    dxmt9::noteQueueMutexSegmentIfEnabled("commit_current_chunk/pre_wait",
                                          qmxEnabled, qmxSegStart);
    return false;
  }

  // Compatibility publication retains the historical GPU-inflight cap. Direct
  // arena sources use their separate sized-admission path and therefore remain
  // independently bounded by CpuReadyTape watermarks.
  const size_t writingSlotIndexBeforeWait = **writingSlot;
  const u64 nextSeqIdBeforeWait = nextSeqId->load(std::memory_order_relaxed);
  const bool waitNeeded = *inflightCount >= inflightLimit;
  if (waitNeeded) {
    observeCommitWait(writingSlotIndexBeforeWait, slot.seqId, inflightLimit);
    // Register the blocked producer as a diagnostic/wakeup observation only.
    // Fixed caps and semantic events remain the sole session-boundary inputs.
    {
      std::lock_guard pendingLock(pendingCompletionMutex_);
      ++producerWriterPressureDepth_;
    }
    if (encodeCv) {
      encodeCv->notify_one();
    }
  }
  dxmt9::noteQueueMutexSegmentIfEnabled("commit_current_chunk/pre_wait",
                                        qmxEnabled, qmxSegStart);
  const auto waitStarted = std::chrono::steady_clock::now();
  writeCv->wait(lock, [&] { return *stop || *inflightCount < inflightLimit; });
  const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
  qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
  if (waitNeeded) {
    {
      std::lock_guard pendingLock(pendingCompletionMutex_);
      DXMT_ASSERT(producerWriterPressureDepth_ > 0);
      if (producerWriterPressureDepth_ > 0) {
        --producerWriterPressureDepth_;
      }
    }
    if (encodeCv) {
      encodeCv->notify_one();
    }
    perf::countQueueCommitWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  }
  if (*stop) {
    dxmt9::noteQueueMutexSegmentIfEnabled("commit_current_chunk/post_wait",
                                          qmxEnabled, qmxSegStart);
    return false;
  }
  // Revalidate rather than repair: if the writing slot moved while we were
  // parked, whoever moved it owns that chunk's commit and this call has nothing
  // left to publish. Returning false is the same answer this function already
  // gives for "no writing slot" at entry.
  if (!writingSlot->has_value() ||
      **writingSlot != writingSlotIndexBeforeWait ||
      nextSeqId->load(std::memory_order_relaxed) != nextSeqIdBeforeWait) {
    dxmt9::noteQueueMutexSegmentIfEnabled("commit_current_chunk/post_wait",
                                          qmxEnabled, qmxSegStart);
    return false;
  }

  const size_t publishedSlotIndex = **writingSlot;
  const u64 publishedSeqId = nextSeqId->load(std::memory_order_relaxed);
  observeCommitWait(publishedSlotIndex, slot.seqId, inflightLimit);
  recordNoEnqueueCommitPublishWaitBeforePublish(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  recordNoEnqueueFirstPublishSlotShapeBeforePublish(
      summarizeNoEnqueueFirstPublishSlotShape(*writingPayload));
  recordNoEnqueueWaitGapToCommitPublish();
  bool sealed = false;
  commitPublish(publishedSlotIndex, publishedSeqId, inflightLimit, [&] {
    slot.seqId = nextSeqId->load(std::memory_order_relaxed);
    writingPayload->seqId = slot.seqId;
    const auto onBeforePublishStart = std::chrono::steady_clock::now();
    if (onBeforePublish) {
      onBeforePublish(*writingPayload);
    }
    recordNoEnqueueCommitPublishOnBeforePublishCpu(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - onBeforePublishStart).count()));
    sealed = submissionBinding_.cpuReadyTape->sealAndPublish(
        CpuReadyPublicationTicket{
            .id = slot.sourceId,
            .storage = slot.storage,
        },
        slot.seqId,
        slot.seqId,
        publishedSlotIndex);
    DXMT_ASSERT(sealed);
    if (!sealed) {
      slot.seqId = 0;
      writingPayload->seqId = 0;
      poisonTapeFailureLocked();
      return;
    }
    // Release: the publish increment is what a lock-free `markTicketAcquire()`
    // reader synchronizes with. TLA+: ProducerMarkReclaim!SlotAdvance.
    nextSeqId->store(nextSeqId->load(std::memory_order_relaxed) + 1,
                     std::memory_order_release);
    slot.state = ChunkSlot::State::Pending;
    *lastCommittedSeqId = slot.seqId;
    ++(*inflightCount);
    writingSlot->reset();
    *writeIndex = (*writeIndex + 1) % slots.size();
    recordCpuReadyTapeStats(*submissionBinding_.cpuReadyTape);
  });
  dxmt9::noteQueueMutexSegmentIfEnabled("commit_current_chunk/post_wait",
                                        qmxEnabled, qmxSegStart);
  if (!sealed) {
    return false;
  }
  noteCpuReadyCapacityProgress();
  return true;
}

bool QueueLifecycleController::dequeueReadySlot(
    std::unique_lock<std::mutex>& lock,
    ReadySlotSnapshot& out) {
  std::array<ReadySlotSnapshot, 1> snapshots{};
  const size_t count = dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(snapshots));
  if (count == 0) {
    return false;
  }
  out = snapshots[0];
  return true;
}

size_t QueueLifecycleController::dequeueReadySlotBatch(
    std::unique_lock<std::mutex>& lock,
    std::span<ReadySlotSnapshot> out,
    const ReadySlotBatchAppendPredicate& canAppend) {
  return dequeueReadySlotBatchPrefix(
      lock, out,
      [&canAppend](std::span<const ResolvedPublishedSource> candidates) {
        if (candidates.empty()) {
          return std::size_t{0};
        }
        std::size_t count = 1;
        for (; count < candidates.size(); ++count) {
          const auto& candidate = candidates[count];
          if (canAppend &&
              !canAppend(candidates.first(count), candidate.slotIndex,
                         candidate.payload)) {
            break;
          }
        }
        return count;
      });
}

size_t QueueLifecycleController::dequeueReadySlotBatchPrefix(
    std::unique_lock<std::mutex>& lock,
    std::span<ReadySlotSnapshot> out,
    const ReadySlotBatchPrefixSelector& selectPrefix) {
  const size_t count = reserveReadySlotBatchPrefix(
      lock, out,
      [&selectPrefix](std::span<const ResolvedPublishedSource> candidates) {
        size_t selected = selectPrefix ? selectPrefix(candidates) : 0u;
        return selected == 0 ? std::size_t{1} : selected;
      });
  if (count == 0) {
    return 0;
  }
  const auto selected =
      std::span<const ReadySlotSnapshot>(out.data(), count);
  // SEGMENT-HOLD: commitReservedReadySlotBatch()/restoreReservedReadySlotBatch()
  // do not unlock/relock -- this covers the caller-side bookkeeping around
  // the same dequeue phase as reserveReadySlotBatchPrefix's own segment.
  dxmt9::QueueMutexSegmentScope qmxCommitSegment("run_encode_loop/dequeue");
  if (!commitReservedReadySlotBatch(lock, selected)) {
    (void)restoreReservedReadySlotBatch(lock, selected);
    poisonTapeFailureLocked();
    return 0;
  }
  return count;
}

size_t QueueLifecycleController::reserveReadySlotBatchPrefix(
    std::unique_lock<std::mutex>& lock,
    std::span<ReadySlotSnapshot> out,
    const ReadySlotBatchPrefixSelector& selectPrefix) {
  // TLA+: QueueLifecycleRefinement / EncodeDequeue.
  if (!lock.owns_lock() || lock.mutex() != submissionBinding_.mutex ||
      out.empty()) {
    return 0;
  }
  auto* cpuReadyTape = submissionBinding_.cpuReadyTape;
  auto* encodeCv = submissionBinding_.encodeCv;
  auto* stop = submissionBinding_.stop;
  if (!cpuReadyTape || !encodeCv || !stop ||
      out.size() > kMaxReadyPrefixSources ||
      tentativeReadyPrefixCount_ != 0) {
    return 0;
  }

  encodeCv->wait(lock, [&] { return *stop || !cpuReadyTape->readyEmpty(); });
  if (*stop && cpuReadyTape->readyEmpty()) {
    return 0;
  }

  // SEGMENT-HOLD: from here to return, `lock` is held continuously (no
  // interior unlock/relock in this function) -- this is the "run_encode_loop"
  // encode-dequeue bookkeeping between cv waits that the module comment in
  // dxmt9_command_queue.cpp calls out as a priority site. The remaining
  // dequeue-phase bookkeeping in the caller (dequeueReadySlotBatchPrefix's
  // commitReservedReadySlotBatch call) and in runEncodeIteration (source
  // resolution before its own unlock) are tagged the same way at their own
  // sites below, since they are separate held intervals in different
  // function scopes.
  dxmt9::QueueMutexSegmentScope qmxDequeueSegment("run_encode_loop/dequeue");
  perf::countEncodeDequeueReadyDepth(
      static_cast<std::uint64_t>(cpuReadyTape->readyCount()));
  const size_t maxCount = std::min(out.size(), cpuReadyTape->readyCount());
  std::array<CpuReadyTape::ReadyEntry, kMaxReadyPrefixSources> ready{};
  if (cpuReadyTape->copyReadyPrefix(
          std::span<CpuReadyTape::ReadyEntry>(ready.data(), maxCount)) !=
      maxCount) {
    poisonTapeFailureLocked();
    return 0;
  }

  std::array<ResolvedPublishedSource, kMaxReadyPrefixSources> resolved{};
  for (size_t i = 0; i < maxCount; ++i) {
    const auto& candidate = ready[i];
    if (candidate.controlIndex >= submissionBinding_.slots.size()) {
      poisonTapeFailureLocked();
      return 0;
    }
    auto& control = submissionBinding_.slots[candidate.controlIndex];
    const auto payload = cpuReadyTape->resolveSourcePayload(
        candidate.source.id, candidate.source.storage,
        CpuReadyTape::State::Ready);
    if (control.state != ChunkSlot::State::Pending ||
        control.sourceId != candidate.source.id ||
        control.storage != candidate.source.storage ||
        control.seqId != candidate.seqId || !payload.valid() ||
        (payload.isLegacy() && payload.legacyPayload() != control.payload) ||
        (payload.isArena() && control.payload != nullptr)) {
      poisonTapeFailureLocked();
      return 0;
    }
    for (size_t j = 0; j < i; ++j) {
      if (ready[j].controlIndex == candidate.controlIndex) {
        poisonTapeFailureLocked();
        return 0;
      }
    }
    const ReadySlotSnapshot snapshot =
        makeReadySlotSnapshot(candidate, control, payload);
    resolved[i] = ResolvedPublishedSource{
        .source = candidate.source,
        .slotIndex = candidate.controlIndex,
        .seqId = candidate.seqId,
        .metadata = candidate.metadata,
        .semantic = candidate.semantic,
        .payload = payload,
        .sourceId = candidate.source.id,
        .storage = candidate.source.storage,
        .slot = payload.legacyPayload(),
        .hasPresent = snapshot.hasPresent,
        .commandBegin = snapshot.commandBegin,
        .commandCount = snapshot.commandCount,
    };
  }

  size_t count = 0;
  if (selectPrefix) {
    count = selectPrefix(std::span<const ResolvedPublishedSource>(
        resolved.data(), maxCount));
    if (count > maxCount) {
      return 0;
    }
  }
  if (count == 0) {
    return 0;
  }

  for (size_t i = 0; i < count; ++i) {
    traceEncodeIterationStage("dequeue.reserved", ready[i].controlIndex,
                              submissionBinding_.slots[ready[i].controlIndex]);
  }
  if (!cpuReadyTape->reserveReadyPrefixForRepresentation(
          std::span<const CpuReadyTape::ReadyEntry>(ready.data(), count))) {
    poisonTapeFailureLocked();
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    const auto& source = resolved[i];
    out[i] = ReadySlotSnapshot{
        .slotIndex = source.slotIndex,
        .seqId = source.seqId,
        .metadata = source.metadata,
        .semantic = source.semantic,
        .hasPresent = source.hasPresent,
        .commandBegin = source.commandBegin,
        .commandCount = source.commandCount,
        .sourceId = source.sourceId,
        .storage = source.storage,
    };
    tentativeReadyPrefix_[i] = out[i];
  }
  tentativeReadyPrefixCount_ = count;
  recordCpuReadyTapeStats(*cpuReadyTape);
  return count;
}

ResolvedPublishedSource QueueLifecycleController::resolveTentativeSource(
    std::unique_lock<std::mutex>& lock,
    const ReadySlotSnapshot& source) const noexcept {
  if (!lock.owns_lock() || lock.mutex() != submissionBinding_.mutex ||
      source.slotIndex >= submissionBinding_.slots.size() ||
      tentativeReadyPrefixCount_ == 0) {
    return {};
  }
  bool exactSnapshot = false;
  for (size_t i = 0; i < tentativeReadyPrefixCount_; ++i) {
    if (tentativeReadyPrefix_[i] == source) {
      exactSnapshot = true;
      break;
    }
  }
  if (!exactSnapshot) {
    return {};
  }
  const CpuReadyTape::SourceRef locator{
      .id = source.sourceId,
      .storage = source.storage,
  };
  if (!locator.valid() || source.seqId == 0 || !source.metadata.valid() ||
      source.metadata.seqId != source.seqId ||
      !submissionBinding_.cpuReadyTape ||
      !submissionBinding_.cpuReadyTape->matches(
          locator, source.metadata, source.semantic,
          CpuReadyTape::State::TentativeRepresented)) {
    return {};
  }
  const auto& control = submissionBinding_.slots[source.slotIndex];
  auto payload = submissionBinding_.cpuReadyTape->resolveSourcePayload(
      locator.id, locator.storage,
      CpuReadyTape::State::TentativeRepresented);
  if (control.state != ChunkSlot::State::Pending ||
      control.sourceId != source.sourceId ||
      control.storage != source.storage || control.seqId != source.seqId ||
      !payload.valid() ||
      (payload.isLegacy() && payload.legacyPayload() != control.payload) ||
      (payload.isArena() && control.payload != nullptr) ||
      !commandRangeWithinSource(payload, source.commandBegin,
                                source.commandCount) ||
      commandRangeHasPresent(payload, source.commandBegin,
                             source.commandCount) != source.hasPresent) {
    return {};
  }
  return ResolvedPublishedSource{
      .source = locator,
      .slotIndex = source.slotIndex,
      .seqId = source.seqId,
      .metadata = source.metadata,
      .semantic = source.semantic,
      .payload = payload,
      .sourceId = source.sourceId,
      .storage = source.storage,
      .slot = payload.legacyPayload(),
      .hasPresent = source.hasPresent,
      .commandBegin = source.commandBegin,
      .commandCount = source.commandCount,
  };
}

bool QueueLifecycleController::commitReservedReadySlotBatch(
    std::unique_lock<std::mutex>& lock,
    std::span<const ReadySlotSnapshot> sources) {
  if (!lock.owns_lock() || lock.mutex() != submissionBinding_.mutex ||
      sources.empty() || sources.size() > kMaxReadyPrefixSources ||
      sources.size() != tentativeReadyPrefixCount_ ||
      !std::equal(sources.begin(), sources.end(),
                  tentativeReadyPrefix_.begin()) ||
      !submissionBinding_.cpuReadyTape) {
    return false;
  }
  std::array<CpuReadyTape::ReadyEntry, kMaxReadyPrefixSources> ready{};
  for (size_t i = 0; i < sources.size(); ++i) {
    const auto resolved = resolveTentativeSource(lock, sources[i]);
    if (!resolved.valid()) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (sources[j].slotIndex == sources[i].slotIndex) {
        return false;
      }
    }
    ready[i] = CpuReadyTape::ReadyEntry{
        .source = resolved.source,
        .controlIndex = resolved.slotIndex,
        .seqId = resolved.seqId,
        .metadata = resolved.metadata,
        .semantic = resolved.semantic,
    };
  }

  auto* cpuReadyTape = submissionBinding_.cpuReadyTape;
  recordNoEnqueueWaitGapToEncodeDequeue();
  bool committed = false;
  encodeDequeue(ready[0].controlIndex, ready[0].seqId, [&] {
    committed = cpuReadyTape->commitReservedReadyPrefix(
        std::span<const CpuReadyTape::ReadyEntry>(ready.data(), sources.size()));
    if (!committed) {
      return;
    }
    for (const auto& source : sources) {
      submissionBinding_.slots[source.slotIndex].state =
          ChunkSlot::State::Encoding;
    }
    recordCpuReadyTapeStats(*cpuReadyTape);
  });
  if (!committed) {
    return false;
  }
  for (size_t i = 0; i < tentativeReadyPrefixCount_; ++i) {
    tentativeReadyPrefix_[i] = {};
  }
  tentativeReadyPrefixCount_ = 0;
  for (const auto& source : sources) {
    traceEncodeIterationStage("dequeue.after-transition", source.slotIndex,
                              submissionBinding_.slots[source.slotIndex]);
  }
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  return true;
}

bool QueueLifecycleController::restoreReservedReadySlotBatch(
    std::unique_lock<std::mutex>& lock,
    std::span<const ReadySlotSnapshot> sources) {
  if (!lock.owns_lock() || lock.mutex() != submissionBinding_.mutex ||
      sources.empty() || sources.size() > kMaxReadyPrefixSources ||
      sources.size() != tentativeReadyPrefixCount_ ||
      !std::equal(sources.begin(), sources.end(),
                  tentativeReadyPrefix_.begin()) ||
      !submissionBinding_.cpuReadyTape) {
    return false;
  }
  std::array<CpuReadyTape::ReadyEntry, kMaxReadyPrefixSources> ready{};
  for (size_t i = 0; i < sources.size(); ++i) {
    const auto resolved = resolveTentativeSource(lock, sources[i]);
    if (!resolved.valid()) {
      return false;
    }
    ready[i] = CpuReadyTape::ReadyEntry{
        .source = resolved.source,
        .controlIndex = resolved.slotIndex,
        .seqId = resolved.seqId,
        .metadata = resolved.metadata,
        .semantic = resolved.semantic,
    };
  }
  if (!submissionBinding_.cpuReadyTape->restoreReservedReadyPrefix(
          std::span<const CpuReadyTape::ReadyEntry>(
              ready.data(), sources.size()))) {
    return false;
  }
  for (size_t i = 0; i < tentativeReadyPrefixCount_; ++i) {
    tentativeReadyPrefix_[i] = {};
  }
  tentativeReadyPrefixCount_ = 0;
  recordCpuReadyTapeStats(*submissionBinding_.cpuReadyTape);
  noteCpuReadyCapacityProgress();
  return true;
}

ResolvedPublishedSource QueueLifecycleController::resolveRepresentedSource(
    const ReadySlotSnapshot& source) const noexcept {
  const CpuReadyTape::SourceRef locator{
      .id = source.sourceId,
      .storage = source.storage,
  };
  if (!locator.valid() || source.seqId == 0 ||
      !source.metadata.valid() || source.metadata.seqId != source.seqId ||
      !submissionBinding_.cpuReadyTape ||
      !submissionBinding_.cpuReadyTape->matches(
          locator, source.metadata, source.semantic,
          CpuReadyTape::State::Represented)) {
    return {};
  }
  auto payload = submissionBinding_.cpuReadyTape->resolveSourcePayload(
      locator.id, locator.storage, CpuReadyTape::State::Represented);
  if (!payload.valid() ||
      !commandRangeWithinSource(payload, source.commandBegin,
                                source.commandCount) ||
      commandRangeHasPresent(payload, source.commandBegin,
                             source.commandCount) != source.hasPresent) {
    return {};
  }
  return ResolvedPublishedSource{
      .source = locator,
      .slotIndex = source.slotIndex,
      .seqId = source.seqId,
      .metadata = source.metadata,
      .semantic = source.semantic,
      .payload = payload,
      .sourceId = source.sourceId,
      .storage = source.storage,
      .slot = payload.legacyPayload(),
      .hasPresent = source.hasPresent,
      .commandBegin = source.commandBegin,
      .commandCount = source.commandCount,
  };
}

PostEncodeReceiptResult QueueLifecycleController::retireEncodedSourcePayload(
    std::unique_lock<std::mutex>& lock,
    QueueCompletionSource& source) {
  if (!lock.owns_lock() || lock.mutex() != submissionBinding_.mutex ||
      !submissionBinding_.cpuReadyTape || !source.locatorBacked() ||
      source.slotIndex >= submissionBinding_.slots.size() ||
      !submissionBinding_.inflightCount ||
      *submissionBinding_.inflightCount == 0u) {
    return PostEncodeReceiptResult::Invalid;
  }

  auto* tape = submissionBinding_.cpuReadyTape;
  auto& control = submissionBinding_.slots[source.slotIndex];
  if (control.state != ChunkSlot::State::Encoding ||
      control.seqId != source.seqId || control.sourceId != source.source.id ||
      control.storage != source.source.storage ||
      !tape->canBeginPostEncodeRetire(source.source.id,
                                     source.source.storage)) {
    return PostEncodeReceiptResult::WrongState;
  }
  const auto payloadKind = tape->payloadKind(
      source.source.id, source.source.storage,
      CpuReadyTape::State::Represented);
  if (!payloadKind) {
    return PostEncodeReceiptResult::WrongState;
  }

  const auto activation =
      postEncodeCompletionLedger_.activate(source.seqId, source.hasPresent);
  if (activation.result != PostEncodeReceiptResult::Succeeded) {
    perf::countPostEncodeReceiptFailure(
        static_cast<std::uint32_t>(activation.result));
    return activation.result;
  }
  perf::recordPostEncodeReceiptDepth(
      postEncodeCompletionLedger_.depth(),
      postEncodeCompletionLedger_.peak());

  SourcePayloadBlock* legacyPayload = nullptr;
  std::optional<CpuReadyTape::DetachedArenaOwner> arenaOwner;
  if (*payloadKind == CpuReadyTape::PayloadKind::Legacy) {
    legacyPayload = tape->beginPostEncodeLegacyRetire(
        source.source.id, source.source.storage);
    if (!legacyPayload) {
      poisonTapeFailureLocked();
      return PostEncodeReceiptResult::WrongState;
    }
  } else {
    auto detached = tape->beginPostEncodeArenaRetire(
        source.source.id, source.source.storage);
    if (!detached) {
      poisonTapeFailureLocked();
      return PostEncodeReceiptResult::WrongState;
    }
    arenaOwner.emplace(std::move(*detached));
  }
  control.state = ChunkSlot::State::Retiring;

  const auto sourceRef = source.source;
  lock.unlock();
  if (legacyPayload) {
    legacyPayload->clearCommands();
    legacyPayload->seqId = 0;
  } else {
    arenaOwner->destroy();
  }
  lock.lock();

  const bool finished = legacyPayload
      ? tape->finishReclaim(sourceRef.id, sourceRef.storage)
      : tape->finishArenaReclaim(sourceRef.id, sourceRef.storage,
                                 std::move(*arenaOwner));
  if (!finished || control.state != ChunkSlot::State::Retiring ||
      control.seqId != source.seqId || control.sourceId != sourceRef.id ||
      control.storage != sourceRef.storage) {
    poisonTapeFailureLocked();
    return PostEncodeReceiptResult::WrongState;
  }

  control.state = ChunkSlot::State::Free;
  control.seqId = 0;
  control.sourceId = {};
  control.storage = {};
  control.payload = nullptr;
  source.source = {};
  source.receipt = activation.receipt;
  --*submissionBinding_.inflightCount;
  recordCpuReadyTapeStats(*tape);
  noteCpuReadyCapacityProgress();
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  return PostEncodeReceiptResult::Succeeded;
}

size_t QueueLifecycleController::retainEncodedSourcesForPendingTail(
    std::unique_lock<std::mutex>& lock,
    std::span<const ReadySlotSnapshot> sources,
    std::span<QueueCompletionSource> out) {
  DXMT_ASSERT(lock.owns_lock());
  static_cast<void>(lock);
  if (sources.empty() || out.size() < sources.size()) {
    return 0;
  }

  u64 previousSeqId = 0;
  for (const auto& source : sources) {
    if (source.slotIndex >= submissionBinding_.slots.size()) {
      return 0;
    }
    const auto& liveSlot = submissionBinding_.slots[source.slotIndex];
    const CpuReadyTape::SourceRef locator{
        .id = source.sourceId,
        .storage = source.storage,
    };
    const auto livePayload = submissionBinding_.cpuReadyTape->resolveSourcePayload(
        source.sourceId, source.storage, CpuReadyTape::State::Encoding);
    if (liveSlot.sourceId != source.sourceId ||
        liveSlot.storage != source.storage ||
        !livePayload.valid() ||
        liveSlot.state != ChunkSlot::State::Encoding ||
        liveSlot.seqId != source.seqId ||
        liveSlot.seqId == 0 ||
        !source.metadata.valid() || source.metadata.seqId != source.seqId ||
        !submissionBinding_.cpuReadyTape->matches(
            locator, source.metadata, source.semantic,
            CpuReadyTape::State::Encoding) ||
        !commandRangeWithinSource(livePayload,
                                  source.commandBegin,
                                  source.commandCount) ||
        commandRangeHasPresent(livePayload,
                               source.commandBegin,
                               source.commandCount) != source.hasPresent) {
      return 0;
    }
    if (previousSeqId != 0 && source.seqId != previousSeqId + 1) {
      return 0;
    }
    previousSeqId = source.seqId;
  }

  for (size_t i = 0; i < sources.size(); ++i) {
    out[i] = QueueCompletionSource{
        .source = CpuReadyTape::SourceRef{
            .id = sources[i].sourceId,
            .storage = sources[i].storage,
        },
        .slotIndex = sources[i].slotIndex,
        .seqId = sources[i].seqId,
        .hasPresent = sources[i].hasPresent,
        .commandBegin = sources[i].commandBegin,
        .commandCount = sources[i].commandCount,
    };
  }
#ifndef NDEBUG
  assertQueueLifecycleInvariants();
#endif
  return sources.size();
}

bool QueueLifecycleController::runEncodeIteration(
    std::unique_lock<std::mutex>& lock,
    const std::function<std::optional<QueueSubmissionRecord>(
        const ReadySlotSnapshot&, const SourcePayloadView&)>& encodeFn,
    const std::function<void(u64)>& onInlineComplete) {
  // TLA+: EncodeDequeue followed by EncodeSubmitToGpu or EncodeCompleteInline.
  // SEGMENT-HOLD: dequeueReadySlot() below reaches down into
  // reserveReadySlotBatchPrefix()/dequeueReadySlotBatchPrefix(), which record
  // their own "run_encode_loop/dequeue" segments around their cv wait and
  // bookkeeping. This function's own remaining held work -- source
  // resolution up to the unlock() before encodeFn(), the post-relock submit
  // bookkeeping, and the inline-completion path -- is tagged separately
  // below so no interval double-counts another function's segment.
  const bool qmxEnabled = dxmt9::queueMutexSplitEnabled();
  ReadySlotSnapshot source{};
  if (!dequeueReadySlot(lock, source)) {
    return false;
  }
  auto qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  std::optional<QueueSubmissionRecord> submission;
  {
    // Resolve only for the synchronous encode call. The locator-only source
    // survives; no arena pointer crosses the relock/completion boundary.
    const auto resolved = resolveRepresentedSource(source);
    if (!resolved.valid()) {
      poisonTapeFailureLocked();
      dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/dequeue",
                                            qmxEnabled, qmxSegStart);
      return false;
    }
    if (const auto* legacy = resolved.payload.legacyPayload()) {
      traceEncodeIterationStage("iteration.before-unlock",
                                source.slotIndex, *legacy);
    }
    dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/dequeue",
                                          qmxEnabled, qmxSegStart);
    lock.unlock();
    if (encodeFn) {
      submission = encodeFn(source, resolved.payload);
      if (submission.has_value() && !submission->commandBuffer &&
          !submission->testOnlyAllowNullCommandBuffer) {
        submission.reset();
      }
    }
  }
  lock.lock();
  qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};

  if (submission.has_value()) {
    if (submission->fixedCompletionSources.empty()) {
      const std::array completionSources{
          completionSourceForReadySlot(source),
      };
      if (!submission->assignFixedCompletionSources(completionSources)) {
        poisonTapeFailureLocked();
        dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/submit",
                                              qmxEnabled, qmxSegStart);
        return false;
      }
    }
    if (!enqueueSubmission(*submission)) {
      dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/submit",
                                            qmxEnabled, qmxSegStart);
      return false;
    }
    auto postCommitCallbacks = std::move(submission->postCommitCallbacks);
    dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/submit", qmxEnabled,
                                          qmxSegStart);
    lock.unlock();
    for (auto& callback : postCommitCallbacks) {
      if (callback) {
        callback();
      }
    }
  } else {
    // completeInlineChunk() has its own interior unlock/relock and records
    // its own "run_encode_loop/inline_reclaim" segments (see below); only
    // the onInlineComplete callback afterward is this function's own held
    // work, tagged "run_encode_loop/submit" since it plays the same "after
    // relock, before next cv wait" role as the real-submission branch above.
    if (!completeInlineChunk(lock, source.slotIndex, source.seqId)) {
      return false;
    }
    qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    if (onInlineComplete) {
      onInlineComplete(source.seqId);
    }
    dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/submit", qmxEnabled,
                                          qmxSegStart);
  }
  return true;
}

bool QueueLifecycleController::appendPresentCommand(const SwapDesc& present,
                                                    Handle sourceHandle) {
  // TLA+: PresentFrameLatency / CommitPresent metadata lane.
  auto* writingSlot = submissionBinding_.writingSlot;
  if (!writingSlot || !writingSlot->has_value()) {
    return false;
  }

  const size_t slotIndex = **writingSlot;
  auto& slot = submissionBinding_.slots[slotIndex];
  auto* payload = submissionBinding_.cpuReadyTape->resolveForWrite(
      CpuReadyPublicationTicket{
          .id = slot.sourceId,
          .storage = slot.storage,
      });
  if (!payload || payload != slot.payload) {
    poisonTapeFailureLocked();
    return false;
  }
  enqueuePresent(slotIndex, slot.seqId, present, sourceHandle, [&] {
    payload->appendPresent(present, sourceHandle);
  });
  return true;
}

bool QueueLifecycleController::submitEncodedChunk(WMT::Reference<WMT::CommandBuffer> commandBuffer,
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
  if (slotIndex < submissionBinding_.slots.size()) {
    const auto& control = submissionBinding_.slots[slotIndex];
    const auto payload = submissionBinding_.cpuReadyTape->resolveSourcePayload(
        control.sourceId, control.storage, CpuReadyTape::State::Represented);
    if (control.seqId != seqId || !payload.valid()) {
      poisonTapeFailureLocked();
      return false;
    }
    const std::array sources{QueueCompletionSource{
        .source = CpuReadyTape::SourceRef{
            .id = control.sourceId,
            .storage = control.storage,
        },
        .slotIndex = slotIndex,
        .seqId = seqId,
        .hasPresent = record.diagnostics.hasPresent,
        .commandBegin = 0,
        .commandCount = payload.commandCount(),
    }};
    if (!record.assignFixedCompletionSources(sources)) {
      poisonTapeFailureLocked();
      return false;
    }
  }
  return enqueueSubmission(record);
}

bool QueueLifecycleController::submitEncodedSubmission(
    std::unique_lock<std::mutex>& lock,
    QueueSubmissionRecord& record) {
  DXMT_ASSERT(lock.owns_lock());
  if (!lock.owns_lock()) {
    poisonTapeFailure();
    return false;
  }
  return submit(record);
}

bool QueueLifecycleController::completeInlineChunk(
    std::unique_lock<std::mutex>& lock,
    size_t slotIndex,
    u64 seqId) {
  // TLA+: QueueLifecycleRefinement / EncodeCompleteInline.
  DXMT_ASSERT(lock.owns_lock());
  if (!lock.owns_lock() || slotIndex >= submissionBinding_.slots.size()) {
    return false;
  }
  // SEGMENT-HOLD: mirrors reclaimCompletedTapeHead()'s shape exactly -- one
  // interior unlock/relock pair around resource destruction. The
  // "run_encode_loop/inline_reclaim" tag brackets the pre-unlock
  // reclaim-begin bookkeeping and the post-relock finish-reclaim bookkeeping
  // as two samples of the same site. Rare fail-stop error paths
  // (poisonTapeFailureLocked()) before the unlock are left unbracketed, same
  // as reclaimCompletedTapeHead().
  const bool qmxEnabled = dxmt9::queueMutexSplitEnabled();
  auto qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};

  auto& slot = submissionBinding_.slots[slotIndex];
  const CpuReadyTape::SourceRef source{
      .id = slot.sourceId,
      .storage = slot.storage,
  };
  const auto payload = submissionBinding_.cpuReadyTape->resolveSourcePayload(
      slot.sourceId, slot.storage, CpuReadyTape::State::Encoding);
  const auto payloadKind = submissionBinding_.cpuReadyTape->payloadKind(
      slot.sourceId, slot.storage, CpuReadyTape::State::Encoding);
  DXMT_ASSERT(payload.valid());
  if (!source.valid() || slot.state != ChunkSlot::State::Encoding ||
      !submissionBinding_.cpuReadyTape->matches(
          source, seqId, CpuReadyTape::State::Represented) ||
      !payload.valid() || !payloadKind ||
      (payload.isLegacy() && payload.legacyPayload() != slot.payload) ||
      (payload.isArena() && slot.payload != nullptr) ||
      slot.seqId != seqId ||
      (payload.isLegacy() && payload.legacyPayload()->seqId != seqId)) {
    poisonTapeFailureLocked();
    return false;
  }
  auto* completedSeqId = submissionBinding_.completedSeqId;
  auto* completedSeqQueue = submissionBinding_.completedSeqQueue;
  if (completedSeqId && completedSeqQueue) {
    const u64 completedSeqIdValue =
        completedSeqId->load(std::memory_order_relaxed);
    if (completedSeqQueue->size() >
            std::numeric_limits<u64>::max() - completedSeqIdValue) {
      poisonTapeFailureLocked();
      return false;
    }
    const u64 queuedWaterline =
        completedSeqIdValue + completedSeqQueue->size();
    if (queuedWaterline == std::numeric_limits<u64>::max() ||
        seqId != queuedWaterline + 1u) {
      poisonTapeFailureLocked();
      return false;
    }
  }

  bool reclaimBegan = false;
  std::vector<DrawShaderLayoutContext> deferredReleases;
  std::optional<CpuReadyTape::DetachedArenaOwner> arenaOwner;
  finishInline(slotIndex, seqId, [&] {
    // TLA+: QueueLifecycleRefinement / EncodeCompleteInline.
    const bool completedInline =
        submissionBinding_.cpuReadyTape->completeInline(
            source.id, source.storage);
    DXMT_ASSERT(completedInline);
    if (!completedInline ||
        !submissionBinding_.cpuReadyTape->beginReclaim(
            source.id, source.storage)) {
      poisonTapeFailureLocked();
      return;
    }
    if (*payloadKind == CpuReadyTape::PayloadKind::Legacy) {
      auto* reclaimingPayload =
          submissionBinding_.cpuReadyTape->reclaimingPayload(
              source.id, source.storage);
      if (!reclaimingPayload ||
          reclaimingPayload != payload.legacyPayload() ||
          reclaimingPayload->seqId != seqId) {
        poisonTapeFailureLocked();
        return;
      }
      deferredReleases = reclaimingPayload->detachResourceOwners();
      reclaimingPayload->clearCommands();
    } else {
      auto detached =
          submissionBinding_.cpuReadyTape->detachReclaimingArenaOwner(
              source.id, source.storage);
      if (!detached) {
        poisonTapeFailureLocked();
        return;
      }
      arenaOwner.emplace(std::move(*detached));
    }
    reclaimBegan = true;
    slot.state = ChunkSlot::State::Free;
    slot.seqId = 0;
    slot.sourceId = {};
    slot.storage = {};
    slot.payload = nullptr;
  });
  if (!reclaimBegan) {
    return false;
  }

  dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/inline_reclaim",
                                        qmxEnabled, qmxSegStart);
  lock.unlock();
  deferredReleases.clear();
  if (arenaOwner) {
    arenaOwner->destroy();
  }
  lock.lock();
  qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};

  bool reclaimed = false;
  if (*payloadKind == CpuReadyTape::PayloadKind::Legacy) {
    auto* reclaimingPayload =
        submissionBinding_.cpuReadyTape->reclaimingPayload(
            source.id, source.storage);
    if (!reclaimingPayload || reclaimingPayload->seqId != seqId) {
      poisonTapeFailureLocked();
      dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/inline_reclaim",
                                            qmxEnabled, qmxSegStart);
      return false;
    }
    reclaimingPayload->seqId = 0;
    reclaimed = submissionBinding_.cpuReadyTape->finishReclaim(
        source.id, source.storage);
  } else {
    reclaimed = arenaOwner &&
        submissionBinding_.cpuReadyTape->finishArenaReclaim(
            source.id, source.storage, std::move(*arenaOwner));
  }
  DXMT_ASSERT(reclaimed);
  if (!reclaimed) {
    poisonTapeFailureLocked();
    dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/inline_reclaim",
                                          qmxEnabled, qmxSegStart);
    return false;
  }
  recordCpuReadyTapeStats(*submissionBinding_.cpuReadyTape);
  perf::countCpuReadyTapeReclaimWakeup();
  noteCpuReadyCapacityProgress();
  // Inline completion becomes visible only after owner destruction and the
  // generation-advancing reclaim transaction both succeed.
  if (completedSeqQueue) {
    completedSeqQueue->push_back(seqId);
  }
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  if (submissionBinding_.finishCv) {
    submissionBinding_.finishCv->notify_all();
  }
  dxmt9::noteQueueMutexSegmentIfEnabled("run_encode_loop/inline_reclaim",
                                        qmxEnabled, qmxSegStart);
  return true;
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

  // SEGMENT-HOLD: from here to return, `lock` is held continuously (no
  // interior unlock/relock in this function) -- this is the
  // "run_finish_loop" completion-dequeue bookkeeping the module comment in
  // dxmt9_command_queue.cpp calls out as a priority site.
  dxmt9::QueueMutexSegmentScope qmxDequeueSegment("run_finish_loop/dequeue");
  seqId = completedSeqQueue->front();
  bool presentSettled = false;
  const bool publicationCreditReleased =
      postEncodeCompletionLedger_.completed(seqId);
  // TLA+: QueueLifecycleRefinement / FinishDequeue.
  DXMT_ASSERT(seqId == completedSeqId->load(std::memory_order_relaxed) + 1);
  finishDequeue(seqId, [&] {
    completedSeqQueue->pop_front();
    // R-BACK-43.4 `owner-published` — the ONE writer of the GPU watermark,
    // and it still runs with `mutex` held. Release pairs with the T2c map
    // fast path's `completedSeqIdAcquire()`, which holds no lock. What that
    // pairing buys is bounded on purpose: it orders the writes BEFORE this
    // store (the `pop_front` above), not the present/inflight bookkeeping
    // that follows inside the same hold. No lock-free reader reads those —
    // the only lock-free consumer is `finalizeBufferMap`, which uses the
    // value and nothing else. Everything else loads relaxed under `mutex`,
    // where the mutex is the ordering.
    completedSeqId->store(
        std::max(completedSeqId->load(std::memory_order_relaxed), seqId),
        std::memory_order_release);
    if (publicationCreditReleased) {
      const auto released =
          postEncodeCompletionLedger_.finishAndRelease(seqId);
      DXMT_ASSERT(released == PostEncodeReceiptResult::Succeeded);
      if (released != PostEncodeReceiptResult::Succeeded) {
        perf::countPostEncodeReceiptFailure(
            static_cast<std::uint32_t>(released));
        poisonTapeFailureLocked();
        return;
      }
      perf::recordPostEncodeReceiptDepth(
          postEncodeCompletionLedger_.depth(),
          postEncodeCompletionLedger_.peak());
    } else if (*inflightCount > 0) {
      --(*inflightCount);
    }
    auto* completedPresentSeqQueue = submissionBinding_.completedPresentSeqQueue;
    auto* presentCompletedSeqId = submissionBinding_.presentCompletedSeqId;
    if (completedPresentSeqQueue && presentCompletedSeqId) {
      const u64 completedSeqIdValue =
          completedSeqId->load(std::memory_order_relaxed);
      while (!completedPresentSeqQueue->empty() &&
             completedPresentSeqQueue->front() <= completedSeqIdValue) {
        if (completedPresentSeqQueue->front() == seqId) {
          presentSettled = true;
        }
        *presentCompletedSeqId = std::max(*presentCompletedSeqId, completedPresentSeqQueue->front());
        completedPresentSeqQueue->pop_front();
        if (submissionBinding_.completedPresentOrdinal) {
          ++*submissionBinding_.completedPresentOrdinal;
          perf::countCompletedPresentOrdinal();
        }
      }
      // TLA+: PresentFrameLatency / PresentCompletionSafety.
      DXMT_ASSERT(*presentCompletedSeqId <= completedSeqIdValue);
    }
  });
  if (submissionBinding_.schedulingProgressWatchdog) {
    submissionBinding_.schedulingProgressWatchdog->noteReleased(
        seqId, presentSettled);
  }
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

bool QueueLifecycleController::drainCompletedArenaGroupSettlementsLocked(
    u64 completedSeqId) noexcept {
  // The completion watcher appends a group result before publishing its
  // source sequence to completedSeqQueue. Keep the value-owned result private
  // until the finish thread has consumed the group's tail, so the event
  // waterline cannot lead the source FIFO.
  for (;;) {
    const auto* front = completedArenaGroupSettlements_.front();
    if (!front || front->tailSeqId > completedSeqId) {
      return true;
    }
    CpuReadyTape::ArenaGroupSettlement settlement;
    if (completedEventSettlementCount_ ==
            std::numeric_limits<std::uint64_t>::max() ||
        !completedArenaGroupSettlements_.consume(settlement) ||
        settlement.tailSeqId <= completedEventTailSeqId_) {
      return false;
    }
    completedEventTailSeqId_ = settlement.tailSeqId;
    lastCompletedEventSettlement_ = settlement;
    if (completedEventSettlementHistoryCount_ <
        completedEventSettlementHistory_.size()) {
      completedEventSettlementHistory_[
          (completedEventSettlementHistoryHead_ +
           completedEventSettlementHistoryCount_) %
          completedEventSettlementHistory_.size()] = settlement;
      ++completedEventSettlementHistoryCount_;
    } else {
      completedEventSettlementHistory_[completedEventSettlementHistoryHead_] =
          settlement;
      completedEventSettlementHistoryHead_ =
          (completedEventSettlementHistoryHead_ + 1u) %
          completedEventSettlementHistory_.size();
    }
    ++completedEventSettlementCount_;
  }
}

bool QueueLifecycleController::hasCompletedArenaGroupSettlement(
    u64 rawOrdinal, u64 buildGeneration, u64 firstSourceOrdinal,
    u64 tailSeqId, std::uint32_t sourceCount) const noexcept {
  for (std::size_t i = 0; i < completedEventSettlementHistoryCount_; ++i) {
    const auto& value = completedEventSettlementHistory_[
        (completedEventSettlementHistoryHead_ + i) %
        completedEventSettlementHistory_.size()];
    if (value.rawOrdinal == rawOrdinal &&
        value.buildGeneration == buildGeneration &&
        value.firstSourceOrdinal == firstSourceOrdinal &&
        value.tailSeqId == tailSeqId && value.sourceCount == sourceCount) {
      return true;
    }
  }
  return false;
}

bool QueueLifecycleController::runFinishIteration(std::unique_lock<std::mutex>& lock,
                                                  const std::function<void(u64)>& onAfterFinish) {
  // TLA+: FinishDequeue followed by ReclaimFree.
  u64 seqId = 0;
  if (!drainCompletedSequence(lock, seqId)) {
    return false;
  }
  if (!drainCompletedArenaGroupSettlementsLocked(seqId)) {
    poisonTapeFailureLocked();
    return false;
  }
  // SEGMENT-HOLD: bracket only this function's OWN bookkeeping -- the head
  // check before reclaimCompletedTapeHead(), and the onAfterFinish callback
  // after it returns. reclaimCompletedTapeHead() has its own interior
  // unlock/relock and records its own "run_finish_loop/retire" samples
  // around it (see above); wrapping this whole function span would double
  // count that function's held time.
  const bool qmxEnabled = dxmt9::queueMutexSplitEnabled();
  auto qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  const auto head = submissionBinding_.cpuReadyTape->oldestResident();
  if (head && head->seqId < seqId) {
    poisonTapeFailureLocked();
    dxmt9::noteQueueMutexSegmentIfEnabled("run_finish_loop/retire", qmxEnabled,
                                          qmxSegStart);
    return false;
  }
  dxmt9::noteQueueMutexSegmentIfEnabled("run_finish_loop/retire", qmxEnabled,
                                        qmxSegStart);
  // Inline completion already performed its two-phase reclaim before making
  // this sequence visible. An absent head or a newer head therefore requires
  // no second reclaim; an equal GPU-completed head is reclaimed here.
  if (head && head->seqId == seqId &&
      !reclaimCompletedTapeHead(lock, seqId)) {
    return false;
  }
  qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
  if (onAfterFinish) {
    onAfterFinish(seqId);
  }
  dxmt9::noteQueueMutexSegmentIfEnabled("run_finish_loop/retire", qmxEnabled,
                                        qmxSegStart);
  return true;
}

bool QueueLifecycleController::reclaimCompletedTapeHead(
    std::unique_lock<std::mutex>& lock,
    u64 seqId) {
  // TLA+: QueueLifecycleRefinement / ReclaimFree.
  DXMT_ASSERT(lock.owns_lock());
  // SEGMENT-HOLD: this function has exactly one interior unlock/relock pair
  // (around resource destruction below). The "run_finish_loop/retire" tag
  // brackets both disjoint held halves -- the begin-reclaim bookkeeping
  // before the unlock, and the finish-reclaim bookkeeping after the relock
  // -- as two separate samples of the same site, matching the module
  // comment's "run_finish_loop/retire" example. Rare early-return error
  // paths (poisonTapeFailureLocked()) are left unbracketed: they are
  // fail-stop paths, not steady-state per-present traffic.
  const bool qmxEnabled = dxmt9::queueMutexSplitEnabled();
  auto qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  std::vector<DrawShaderLayoutContext> deferredReleases;
  std::optional<CpuReadyTape::DetachedArenaOwner> arenaOwner;
  const auto head = submissionBinding_.cpuReadyTape->oldestResident();
  if (!head || head->seqId != seqId ||
      head->state != CpuReadyTape::State::Completed) {
    poisonTapeFailureLocked();
    return false;
  }
  if (!submissionBinding_.cpuReadyTape->beginReclaim(
          head->source.id, head->source.storage)) {
    poisonTapeFailureLocked();
    return false;
  }
  const auto payloadKind = submissionBinding_.cpuReadyTape->payloadKind(
      head->source.id, head->source.storage, CpuReadyTape::State::Reclaiming);
  if (!payloadKind) {
    poisonTapeFailureLocked();
    return false;
  }
  if (*payloadKind == CpuReadyTape::PayloadKind::Legacy) {
    auto* payload = submissionBinding_.cpuReadyTape->reclaimingPayload(
        head->source.id, head->source.storage);
    if (!payload || payload->seqId != seqId) {
      poisonTapeFailureLocked();
      return false;
    }
    deferredReleases = payload->detachResourceOwners();
    payload->clearCommands();
  } else {
    auto detached =
        submissionBinding_.cpuReadyTape->detachReclaimingArenaOwner(
            head->source.id, head->source.storage);
    if (!detached) {
      poisonTapeFailureLocked();
      return false;
    }
    arenaOwner.emplace(std::move(*detached));
  }

  dxmt9::noteQueueMutexSegmentIfEnabled("run_finish_loop/retire", qmxEnabled,
                                        qmxSegStart);
  lock.unlock();
  deferredReleases.clear();
  if (arenaOwner) {
    arenaOwner->destroy();
  }
  lock.lock();
  qmxSegStart = qmxEnabled ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};

  bool sourceReclaimed = false;
  if (*payloadKind == CpuReadyTape::PayloadKind::Legacy) {
    auto* payload = submissionBinding_.cpuReadyTape->reclaimingPayload(
        head->source.id, head->source.storage);
    if (!payload || payload->seqId != seqId) {
      poisonTapeFailureLocked();
      return false;
    }
    payload->seqId = 0;
    sourceReclaimed = submissionBinding_.cpuReadyTape->finishReclaim(
        head->source.id, head->source.storage);
  } else {
    sourceReclaimed = arenaOwner &&
        submissionBinding_.cpuReadyTape->finishArenaReclaim(
            head->source.id, head->source.storage, std::move(*arenaOwner));
  }
  DXMT_ASSERT(sourceReclaimed);
  if (!sourceReclaimed) {
    poisonTapeFailureLocked();
    dxmt9::noteQueueMutexSegmentIfEnabled("run_finish_loop/retire", qmxEnabled,
                                          qmxSegStart);
    return false;
  }
  recordCpuReadyTapeStats(*submissionBinding_.cpuReadyTape);
  perf::countCpuReadyTapeReclaimWakeup();
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  noteCpuReadyCapacityProgress();
  dxmt9::noteQueueMutexSegmentIfEnabled("run_finish_loop/retire", qmxEnabled,
                                        qmxSegStart);
  return true;
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
  // Relaxed throughout: the caller holds `lock`, and the sole writer takes the
  // same mutex, so these loads are exact.
  if (completedSeqId->load(std::memory_order_relaxed) < targetSeqId) {
    observeWaitForSequence(targetSeqId);
  }
  const bool waitNeeded =
      completedSeqId->load(std::memory_order_relaxed) < targetSeqId;
  const auto waitStarted = std::chrono::steady_clock::now();
  if (waitNeeded) {
    auto* encodeCv = submissionBinding_.encodeCv;
    {
      std::lock_guard pendingLock(pendingCompletionMutex_);
      ++producerSequenceWaitDepth_;
    }
    if (encodeCv) {
      encodeCv->notify_one();
    }
    finishCv->wait(lock, [&] {
      return *stop ||
             completedSeqId->load(std::memory_order_relaxed) >= targetSeqId;
    });
    {
      std::lock_guard pendingLock(pendingCompletionMutex_);
      DXMT_ASSERT(producerSequenceWaitDepth_ > 0);
      if (producerSequenceWaitDepth_ > 0) {
        --producerSequenceWaitDepth_;
      }
    }
    if (encodeCv) {
      encodeCv->notify_one();
    }
  }
  // TLA+: QueueLifecycleRefinement / WaitForSequenceSafety.
  DXMT_ASSERT(*stop ||
              completedSeqId->load(std::memory_order_relaxed) >= targetSeqId);
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

bool QueueLifecycleController::enqueueSubmission(QueueSubmissionRecord& record) {
  return submit(record);
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

  if ((beforeState == ChunkSlot::State::Encoding ||
       beforeState == ChunkSlot::State::Retiring) &&
      afterState == ChunkSlot::State::Free) {
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

  const u64 completedSeqId =
      binding.completedSeqId
          ? binding.completedSeqId->load(std::memory_order_relaxed)
          : 0;
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
        DXMT_ASSERT(!slot.sourceId.valid());
        DXMT_ASSERT(!slot.storage.valid());
        DXMT_ASSERT(slot.payload == nullptr);
        break;
      case ChunkSlot::State::Writing:
        DXMT_ASSERT(slot.seqId == 0);
        DXMT_ASSERT(binding.cpuReadyTape);
        {
          const auto kind = binding.cpuReadyTape->payloadKind(
              slot.sourceId, slot.storage, CpuReadyTape::State::Writing);
          DXMT_ASSERT(kind.has_value());
          if (kind == CpuReadyTape::PayloadKind::Legacy) {
            DXMT_ASSERT(binding.cpuReadyTape->resolveForWrite(
                            CpuReadyPublicationTicket{
                                .id = slot.sourceId,
                                .storage = slot.storage,
                            }) == slot.payload);
          } else {
            DXMT_ASSERT(slot.payload == nullptr);
          }
        }
        break;
      case ChunkSlot::State::Pending:
        DXMT_ASSERT(binding.cpuReadyTape);
        {
          const auto tapeState = binding.cpuReadyTape->state(
              slot.sourceId, slot.storage);
          DXMT_ASSERT(tapeState == CpuReadyTape::State::Ready ||
                      tapeState ==
                          CpuReadyTape::State::TentativeRepresented);
          const auto expectedTapeState =
              tapeState == CpuReadyTape::State::TentativeRepresented
                  ? CpuReadyTape::State::TentativeRepresented
                  : CpuReadyTape::State::Ready;
          const auto payload = binding.cpuReadyTape->resolveSourcePayload(
              slot.sourceId, slot.storage, expectedTapeState);
          DXMT_ASSERT(payload.valid());
          DXMT_ASSERT((payload.isLegacy() &&
                       payload.legacyPayload() == slot.payload) ||
                      (payload.isArena() && slot.payload == nullptr));
        }
        [[fallthrough]];
      case ChunkSlot::State::Encoding:
        if (slot.state == ChunkSlot::State::Encoding) {
          DXMT_ASSERT(binding.cpuReadyTape);
          const auto payload = binding.cpuReadyTape->resolveSourcePayload(
              slot.sourceId, slot.storage, CpuReadyTape::State::Encoding);
          DXMT_ASSERT(payload.valid());
          DXMT_ASSERT((payload.isLegacy() &&
                       payload.legacyPayload() == slot.payload) ||
                      (payload.isArena() && slot.payload == nullptr));
        }
        DXMT_ASSERT(slot.seqId > 0);
        DXMT_ASSERT(slot.seqId <= lastCommittedSeqId);
        if (slot.payload) {
          DXMT_ASSERT(slot.payload->seqId == slot.seqId);
        }
        ++abstractInflight;
        break;
      case ChunkSlot::State::Retiring:
        DXMT_ASSERT(binding.cpuReadyTape);
        DXMT_ASSERT(binding.cpuReadyTape->state(
                        slot.sourceId, slot.storage) ==
                    CpuReadyTape::State::Reclaiming);
        DXMT_ASSERT(slot.seqId > 0);
        DXMT_ASSERT(slot.seqId <= lastCommittedSeqId);
        ++abstractInflight;
        break;
      case ChunkSlot::State::GPU:
        DXMT_ASSERT(false &&
                    "Submitted/Completed Tape sources own no live control");
        break;
    }
  }
  const size_t effectiveInflightLimit =
      binding.cpuReadyTape ? binding.cpuReadyTape->capacity()
                           : (inflightLimit != 0 ? inflightLimit : slots.size());
  if (binding.inflightCount) {
    DXMT_ASSERT(*binding.inflightCount <= effectiveInflightLimit);
    DXMT_ASSERT(abstractInflight <= *binding.inflightCount);
  }

  if (binding.cpuReadyTape) {
    std::array<CpuReadyTape::ReadyEntry, kMaxEncodeSessionSources> ready{};
    DXMT_ASSERT(binding.cpuReadyTape->readyCount() <= ready.size());
    const size_t readyCount = binding.cpuReadyTape->copyReadyPrefix(
        std::span<CpuReadyTape::ReadyEntry>(
            ready.data(), binding.cpuReadyTape->readyCount()));
    DXMT_ASSERT(readyCount == binding.cpuReadyTape->readyCount());
    for (size_t i = 0; i < readyCount; ++i) {
      DXMT_ASSERT(ready[i].controlIndex < slots.size());
      const auto& control = slots[ready[i].controlIndex];
      DXMT_ASSERT(control.state == ChunkSlot::State::Pending);
      DXMT_ASSERT(control.sourceId == ready[i].source.id);
      DXMT_ASSERT(control.storage == ready[i].source.storage);
      DXMT_ASSERT(control.seqId == ready[i].seqId);
      for (size_t j = i + 1; j < readyCount; ++j) {
        DXMT_ASSERT(ready[i].controlIndex != ready[j].controlIndex);
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
    const auto completionSources = pending.explicitCompletionSourceSpan();
    DXMT_ASSERT(!completionSources.empty());
    for (const auto& source : completionSources) {
      DXMT_ASSERT(source.completionIdentityValid());
      if (source.receiptBacked()) {
        DXMT_ASSERT(postEncodeCompletionLedger_.matches(
            source.receipt, PostEncodeReceiptState::Submitted,
            source.hasPresent));
      } else {
        DXMT_ASSERT(binding.cpuReadyTape);
        DXMT_ASSERT(binding.cpuReadyTape->state(
                        source.source.id, source.source.storage) ==
                    CpuReadyTape::State::Submitted);
      }
      DXMT_ASSERT(source.seqId > 0);
      DXMT_ASSERT(source.seqId <= lastCommittedSeqId);
      DXMT_ASSERT(previousSeqId == 0 || source.seqId > previousSeqId);
      previousSeqId = source.seqId;
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

bool QueueLifecycleController::submit(QueueSubmissionRecord& record) {
  if (!record.commandBuffer && !record.testOnlyAllowNullCommandBuffer) {
    poisonTapeFailureLocked();
    return false;
  }
  const auto completionSources = record.explicitCompletionSourceSpan();
  if (completionSources.empty() ||
      completionSources.size() > kMaxEncodeSessionSources) {
    poisonTapeFailureLocked();
    return false;
  }
  if (!record.completionSpanShadowMatchesSources()) {
    poisonTapeFailureLocked();
    return false;
  }
  if (record.commandBuffer &&
      completionSources.size() >
          std::numeric_limits<std::uint64_t>::max() -
              gpuOutstandingCompletionSourceCount_) {
    poisonTapeFailureLocked();
    return false;
  }

  std::array<CpuReadyTape::SourceRef, kMaxEncodeSessionSources> tapeSources{};
  std::size_t tapeSourceCount = 0;
  for (std::size_t i = 0; i < completionSources.size(); ++i) {
    const auto& source = completionSources[i];
    if (!source.completionIdentityValid()) {
      poisonTapeFailureLocked();
      return false;
    }
    if (source.receiptBacked()) {
      if (!postEncodeCompletionLedger_.matches(
              source.receipt, PostEncodeReceiptState::Active,
              source.hasPresent)) {
        poisonTapeFailureLocked();
        return false;
      }
      continue;
    }
    if (source.slotIndex >= submissionBinding_.slots.size()) {
      poisonTapeFailureLocked();
      return false;
    }
    auto& sourceSlot = submissionBinding_.slots[source.slotIndex];
    const auto sourcePayload =
        submissionBinding_.cpuReadyTape->resolveSourcePayload(
        source.source.id, source.source.storage,
        CpuReadyTape::State::Represented);
    if (sourceSlot.state != ChunkSlot::State::Encoding ||
        sourceSlot.seqId != source.seqId || source.seqId == 0 ||
        sourceSlot.sourceId != source.source.id ||
        sourceSlot.storage != source.source.storage ||
        !submissionBinding_.cpuReadyTape->matches(
            source.source, source.seqId, CpuReadyTape::State::Represented) ||
        !sourcePayload.valid() ||
        (sourcePayload.isLegacy() &&
         sourcePayload.legacyPayload() != sourceSlot.payload) ||
        (sourcePayload.isArena() && sourceSlot.payload != nullptr) ||
        !commandRangeWithinSource(sourcePayload,
                                  source.commandBegin,
                                  source.commandCount) ||
        commandRangeHasPresent(sourcePayload,
                               source.commandBegin,
                               source.commandCount) != source.hasPresent) {
      poisonTapeFailureLocked();
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (completionSources[j].locatorBacked() &&
          (completionSources[j].slotIndex == source.slotIndex ||
           completionSources[j].source == source.source)) {
        poisonTapeFailureLocked();
        return false;
      }
    }
    tapeSources[tapeSourceCount++] = source.source;
  }

  const CommandBufferDiagnostics diagnostics =
      summarizeSubmissionSources(record, completionSources);

  const auto beforeCommitState = currentState();
  const bool transitioned = tapeSourceCount == 0u ||
      submissionBinding_.cpuReadyTape->transitionAll(
          std::span<const CpuReadyTape::SourceRef>(tapeSources.data(),
                                                   tapeSourceCount),
          CpuReadyTape::State::Encoding,
          CpuReadyTape::State::GPU);
  DXMT_ASSERT(transitioned);
  if (!transitioned) {
    poisonTapeFailureLocked();
    return false;
  }
  for (const auto& source : completionSources) {
    if (source.receiptBacked()) {
      const auto submitted = postEncodeCompletionLedger_.markSubmitted(
          source.receipt, source.hasPresent);
      DXMT_ASSERT(submitted == PostEncodeReceiptResult::Succeeded);
      if (submitted != PostEncodeReceiptResult::Succeeded) {
        perf::countPostEncodeReceiptFailure(
            static_cast<std::uint32_t>(submitted));
        poisonTapeFailureLocked();
        return false;
      }
      continue;
    }
    auto& sourceSlot = submissionBinding_.slots[source.slotIndex];
    sourceSlot.state = ChunkSlot::State::Free;
    sourceSlot.seqId = 0;
    sourceSlot.sourceId = {};
    sourceSlot.storage = {};
    sourceSlot.payload = nullptr;
  }
  const auto afterCommitState = currentState();
#ifndef NDEBUG
  assertQueueLifecycleInvariants();
#endif
  for (const auto& source : completionSources) {
    observeTransition(QueueTransitionRecord{
        .before = beforeCommitState,
        .after = afterCommitState,
        .slotIndex = source.slotIndex,
        .eventSeqId = source.seqId,
    });
  }
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  if (!record.commandBuffer) {
    return true;
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
    return true;
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
  if (binding.schedulingProgressWatchdog) {
    for (const auto& source : completionSources) {
      binding.schedulingProgressWatchdog->noteSubmitted(
          source.seqId, record.metalCapture.has_value() ||
              record.metalCaptureAlreadyStarted);
    }
  }
  gpuOutstandingCompletionSourceCount_ += completionSources.size();
  perf::recordGpuOutstandingCompletionSources(
      gpuOutstandingCompletionSourceCount_);

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
    pending.slotIndex = record.slotIndex;
    pending.seqId = record.seqId;
    pending.gpuOutstandingCounted = true;
    // submit preflight already validated this immutable pair before commit.
    // Copy both fixed owners without introducing a new fallible derivation
    // after the Metal effect; completion revalidates the projection before
    // any C++ callback or queue/Tape completion effect.
    pending.fixedCompletionSources = record.fixedCompletionSources;
    pending.completionSpanShadow = record.completionSpanShadow;
    const bool handoffShadowMatches =
        pending.completionSpanShadowMatchesSources();
    DXMT_ASSERT(handoffShadowMatches);
    static_cast<void>(handoffShadowMatches);
    pending.commandBufferChainLength = record.commandBufferChainLength;
    pending.enqueueTime = enqueueTime;
    pending.renderEncoderGpuSampleBuffer =
        std::move(record.renderEncoderGpuSampleBuffer);
    pending.renderEncoderGpuSamples =
        std::move(record.renderEncoderGpuSamples);
    pending.completionCallbacks = std::move(record.completionCallbacks);
    pending.retainedPayloads = std::move(record.retainedPayloads);
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
  return true;
}

bool QueueLifecycleController::processOnePendingCompletion() {
  PendingCompletion pending;
  size_t pendingDepthAfterPop = 0;
  {
    std::unique_lock<std::mutex> lock(pendingCompletionMutex_);
    if (!pendingCompletionStop_ && pendingCompletion_.empty() &&
        pendingCompletionWaitObserver_) {
      pendingCompletionWaitObserver_(pendingCompletionWaitObserverContext_);
    }
    pendingCompletionCv_.wait(lock, [this] {
      return pendingCompletionStop_ || !pendingCompletion_.empty();
    });
    if (pendingCompletionStop_ && pendingCompletion_.empty()) {
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
    std::condition_variable* encodeCv = nullptr;
    {
      std::lock_guard lock(pendingCompletionMutex_);
      completionWaitActive_ = true;
      completionWaitEnqueues_ = 0;
      completionWaitCommitPublishTime_ = {};
      completionWaitEncodeDequeueTime_ = {};
      encodeCv = submissionBinding_.encodeCv;
    }
    if (encodeCv) {
      encodeCv->notify_one();
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
      completionWaitCommitPublishTime_ = {};
      completionWaitEncodeDequeueTime_ = {};
      if (enqueuesDuringWait == 0) {
        lastNoEnqueueCompletionWaitEnd_ = completed;
        resetNoEnqueueGapProgressLocked();
      }
    }
    if (encodeCv) {
      encodeCv->notify_one();
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

  // The Metal tail has completed, but a corrupt shadow must fail-stop before
  // diagnostics, callbacks, waterline publication, or Tape completion.
  if (!pending.completionSpanShadowMatchesSources()) {
    poisonTapeFailure();
    return false;
  }

  const auto binding = submissionBinding_;
  const auto completionSources = pending.explicitCompletionSourceSpan();
  // Receipt validity is the locator-free retirement authority. Reject stale,
  // duplicate, or ABA receipt identities before diagnostics, callbacks,
  // waterlines, or any completion/release mutation. Legacy locator validation
  // retains its historical ordering below.
  if (completionSources.empty() ||
      completionSources.size() > kMaxEncodeSessionSources) {
    poisonTapeFailure();
    return false;
  }
  for (std::size_t i = 0; i < completionSources.size(); ++i) {
    const auto& source = completionSources[i];
    if (!source.receiptBacked()) {
      continue;
    }
    if (!postEncodeCompletionLedger_.matches(
            source.receipt, PostEncodeReceiptState::Submitted,
            source.hasPresent)) {
      perf::countPostEncodeReceiptFailure(
          static_cast<std::uint32_t>(PostEncodeReceiptResult::Stale));
      poisonTapeFailure();
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (completionSources[j].receiptBacked() &&
          completionSources[j].receipt == source.receipt) {
        perf::countPostEncodeReceiptFailure(
            static_cast<std::uint32_t>(PostEncodeReceiptResult::Duplicate));
        poisonTapeFailure();
        return false;
      }
    }
  }
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
    const u64 completedSeqId =
        binding.completedSeqId
            ? binding.completedSeqId->load(std::memory_order_relaxed)
            : 0;
    std::array<CpuReadyTape::SourceRef, kMaxEncodeSessionSources> tapeSources{};
    std::size_t tapeSourceCount = 0;
    u64 expectedSeqId = completedSeqId;
    if (binding.completedSeqQueue->size() >
        std::numeric_limits<u64>::max() - expectedSeqId) {
      poisonTapeFailureLocked();
      return false;
    }
    expectedSeqId += binding.completedSeqQueue->size();
    for (std::size_t i = 0; i < completionSources.size(); ++i) {
      const auto& source = completionSources[i];
      if (!source.completionIdentityValid() ||
          expectedSeqId == std::numeric_limits<u64>::max() ||
          source.seqId != expectedSeqId + 1u) {
        poisonTapeFailureLocked();
        return false;
      }
      if (source.receiptBacked()) {
        if (!postEncodeCompletionLedger_.matches(
                source.receipt, PostEncodeReceiptState::Submitted,
                source.hasPresent)) {
          poisonTapeFailureLocked();
          return false;
        }
      } else {
        if (!binding.cpuReadyTape->matches(
                source.source, source.seqId, CpuReadyTape::State::Submitted) ||
            !binding.cpuReadyTape->resolveSourcePayload(
                source.source.id, source.source.storage,
                CpuReadyTape::State::Submitted).valid()) {
          poisonTapeFailureLocked();
          return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
          if (completionSources[j].locatorBacked() &&
              completionSources[j].source == source.source) {
            poisonTapeFailureLocked();
            return false;
          }
        }
        tapeSources[tapeSourceCount++] = source.source;
      }
      expectedSeqId = source.seqId;
    }

    const QueueControllerState before = makeBoundQueueState(binding);
    const bool completed = tapeSourceCount == 0u ||
        binding.cpuReadyTape->completeAll(
            std::span<const CpuReadyTape::SourceRef>(tapeSources.data(),
                                                     tapeSourceCount));
    DXMT_ASSERT(completed);
    if (!completed) {
      poisonTapeFailureLocked();
      return false;
    }
    if (binding.completedArenaGroupSettlements) {
      for (const auto& source : completionSources) {
        if (source.receiptBacked()) {
          continue;
        }
        const auto settlement =
            binding.cpuReadyTape->takeCompletedArenaGroupSettlement(
                source.source);
        if (!settlement) {
          continue;
        }
        if (!binding.completedArenaGroupSettlements->append(*settlement)) {
          poisonTapeFailureLocked();
          return false;
        }
      }
    }
    for (const auto& source : completionSources) {
      if (!source.receiptBacked()) {
        continue;
      }
      const auto released = postEncodeCompletionLedger_.markCompleted(
          source.receipt, source.hasPresent);
      DXMT_ASSERT(released == PostEncodeReceiptResult::Succeeded);
      if (released != PostEncodeReceiptResult::Succeeded) {
        perf::countPostEncodeReceiptFailure(
            static_cast<std::uint32_t>(released));
        poisonTapeFailureLocked();
        return false;
      }
    }
    appendCompletionSourcesToQueues(
        *binding.completedSeqQueue,
        binding.completedPresentSeqQueue,
        completedSeqId,
        completionSources);
    if (binding.schedulingProgressWatchdog) {
      for (const auto& source : completionSources) {
        binding.schedulingProgressWatchdog->noteGpuSettled(source.seqId);
        binding.schedulingProgressWatchdog->noteCompletionExpanded(
            source.seqId);
      }
    }
    if (pending.gpuOutstandingCounted) {
      if (completionSources.size() > gpuOutstandingCompletionSourceCount_) {
        poisonTapeFailureLocked();
        return false;
      }
      gpuOutstandingCompletionSourceCount_ -= completionSources.size();
      perf::recordGpuOutstandingCompletionSources(
          gpuOutstandingCompletionSourceCount_);
    }
    const QueueControllerState after = makeBoundQueueState(binding);
#ifndef NDEBUG
    assertQueueLifecycleInvariants();
#endif
    for (const auto& source : completionSources) {
      observeTransition(QueueTransitionRecord{
          .before = before,
          .after = after,
          .slotIndex = source.slotIndex,
          .eventSeqId = source.seqId,
      });
    }
    if (binding.finishCv) {
      binding.finishCv->notify_all();
    }
  }
  // pending.commandBuffer is released when `pending` goes out of scope.
  return true;
}

void QueueLifecycleController::enqueuePendingCompletionForTest(
    PendingCompletion pending) {
  {
    std::lock_guard lock(pendingCompletionMutex_);
    pendingCompletion_.push_back(std::move(pending));
    // The injection hook deliberately accepts malformed completion metadata
    // so release/debug builds exercise processOnePendingCompletion's stale and
    // atomic-rejection paths. Production enqueue validates the invariant.
  }
  pendingCompletionCv_.notify_all();
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
  if (submissionBinding_.schedulingProgressWatchdog &&
      submissionBinding_.cpuReadyTape && slotIndex < state.slots.size()) {
    const auto& control = state.slots[slotIndex];
    const auto payload = submissionBinding_.cpuReadyTape->resolveSourcePayload(
        control.sourceId, control.storage, CpuReadyTape::State::Ready);
    submissionBinding_.schedulingProgressWatchdog->notePublished(
        eventSeqId, payload.valid() &&
            commandRangeHasPresent(payload, 0, payload.commandCount()));
  }
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::CommitPublish, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteEncodeDequeue(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  if (submissionBinding_.schedulingProgressWatchdog) {
    submissionBinding_.schedulingProgressWatchdog->noteEncodeOrOpenSession(
        eventSeqId);
  }
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
