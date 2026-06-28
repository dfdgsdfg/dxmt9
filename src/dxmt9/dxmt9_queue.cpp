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
#include <iterator>
#include <sstream>
#include <thread>
#include <utility>

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

std::array<QueueCompletionSource, 1> fallbackCompletionSource(
    size_t slotIndex,
    u64 seqId,
    bool hasPresent,
    size_t commandCount = 0,
    size_t commandBegin = 0) {
  return {QueueCompletionSource{
      .slotIndex = slotIndex,
      .seqId = seqId,
      .hasPresent = hasPresent,
      .commandBegin = commandBegin,
      .commandCount = commandCount,
  }};
}

std::span<const QueueCompletionSource> completionSourcesFor(
    std::span<const QueueCompletionSource> sources,
    const std::array<QueueCompletionSource, 1>& fallback) {
  if (!sources.empty()) {
    return sources;
  }
  return std::span<const QueueCompletionSource>(fallback.data(), fallback.size());
}

bool commandRangeWithinSlot(const ChunkSlot& slot,
                            size_t commandBegin,
                            size_t commandCount) noexcept {
  const size_t slotCommandCount = slot.commandCount();
  return commandBegin <= slotCommandCount &&
         commandCount <= slotCommandCount - commandBegin;
}

bool commandRangeHasPresent(const ChunkSlot& slot,
                            size_t commandBegin,
                            size_t commandCount) noexcept {
  if (!commandRangeWithinSlot(slot, commandBegin, commandCount)) {
    return false;
  }
  const size_t commandEnd = commandBegin + commandCount;
  for (size_t i = commandBegin; i < commandEnd; ++i) {
    if (slot.commandHeaders[i].kind == MetalCommandKind::Present) {
      return true;
    }
  }
  return false;
}

ReadySlotSnapshot makeReadySlotSnapshot(size_t slotIndex,
                                        ChunkSlot& slot) noexcept {
  return ReadySlotSnapshot{
      .slotIndex = slotIndex,
      .seqId = slot.seqId,
      .hasPresent = !slot.presentRecords.empty(),
      .commandBegin = 0,
      .commandCount = slot.commandCount(),
      .slot = &slot,
  };
}

}  // namespace

QueueCompletionSource completionSourceForReadySlot(
    const ReadySlotSnapshot& snapshot) noexcept {
  DXMT_ASSERT(snapshot.slot != nullptr);
  DXMT_ASSERT(!snapshot.slot || snapshot.slot->seqId == snapshot.seqId);
  DXMT_ASSERT(!snapshot.slot ||
              commandRangeWithinSlot(*snapshot.slot,
                                     snapshot.commandBegin,
                                     snapshot.commandCount));
  DXMT_ASSERT(!snapshot.slot ||
              commandRangeHasPresent(*snapshot.slot,
                                     snapshot.commandBegin,
                                     snapshot.commandCount) ==
                  snapshot.hasPresent);
  return QueueCompletionSource{
      .slotIndex = snapshot.slotIndex,
      .seqId = snapshot.seqId,
      .hasPresent = snapshot.hasPresent,
      .commandBegin = snapshot.commandBegin,
      .commandCount = snapshot.commandCount,
  };
}

std::size_t prepareBatchCompletionSources(
    QueueSubmissionRecord& submission,
    std::span<const ReadySlotSnapshot> sources) {
  if (sources.empty()) {
    return 0;
  }

  const auto existingSources = submission.explicitCompletionSourceSpan();
  if (!existingSources.empty()) {
    if (existingSources.size() > sources.size()) {
      return 0;
    }
    for (std::size_t i = 0; i < existingSources.size(); ++i) {
      const auto expected = completionSourceForReadySlot(sources[i]);
      const auto& actual = existingSources[i];
      if (actual.slotIndex != expected.slotIndex ||
          actual.seqId != expected.seqId ||
          actual.hasPresent != expected.hasPresent ||
          actual.commandBegin != expected.commandBegin ||
          actual.commandCount != expected.commandCount) {
        return 0;
      }
    }
    return existingSources.size();
  }
  if (sources.size() <= 1u) {
    return sources.size();
  }

  EncodeSessionSourceList completionSources;
  for (const auto& source : sources) {
    if (!completionSources.append(completionSourceForReadySlot(source))) {
      return 0;
    }
  }
  return submission.assignFixedCompletionSources(completionSources.span())
      ? sources.size()
      : 0;
}

bool assignBatchCompletionSourcesIfNeeded(
    QueueSubmissionRecord& submission,
    std::span<const ReadySlotSnapshot> sources) {
  return prepareBatchCompletionSources(submission, sources) != 0;
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

  auto sourcesEqual = [](std::span<const QueueCompletionSource> left,
                         std::span<const QueueCompletionSource> right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
      if (left[i].slotIndex != right[i].slotIndex ||
          left[i].seqId != right[i].seqId ||
          left[i].hasPresent != right[i].hasPresent ||
          left[i].commandBegin != right[i].commandBegin ||
          left[i].commandCount != right[i].commandCount) {
        return false;
      }
    }
    return true;
  };

  std::array<QueueCompletionSource, 1> fallbackTailSources{tailSource};
  std::span<const QueueCompletionSource> tailSources(
      fallbackTailSources.data(), fallbackTailSources.size());
  const auto explicitTailSourceSpan = tail.explicitCompletionSourceSpan();
  if (!explicitTailSourceSpan.empty()) {
    std::span<const QueueCompletionSource> explicitTailSources =
        explicitTailSourceSpan;
    if (explicitTailSources.size() > encodedHeadSources.size() &&
        sourcesEqual(
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
  if (encodedHead.renderEncoderGpuSampleBuffer &&
      tail.renderEncoderGpuSampleBuffer &&
      encodedHead.renderEncoderGpuSampleBuffer.handle !=
          tail.renderEncoderGpuSampleBuffer.handle) {
    return false;
  }
  if (encodedHead.metalCapture.has_value() && tail.metalCapture.has_value()) {
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

  if (!tail.commandBuffer && encodedHead.commandBuffer) {
    tail.commandBuffer = std::move(encodedHead.commandBuffer);
  }
  if (!tail.renderEncoderGpuSampleBuffer &&
      encodedHead.renderEncoderGpuSampleBuffer) {
    tail.renderEncoderGpuSampleBuffer =
        std::move(encodedHead.renderEncoderGpuSampleBuffer);
  }

  if (!tail.assignFixedCompletionSources(mergedSourceSpan)) {
    return false;
  }
  if (mergedSourcesOut) {
    *mergedSourcesOut = mergedSourceList;
  }

  const u64 headChainLength = std::max<u64>(1, encodedHead.commandBufferChainLength);
  const u64 tailChainLength = std::max<u64>(1, tail.commandBufferChainLength);
  tail.commandBufferChainLength = headChainLength + tailChainLength - 1;

  CommandBufferDiagnostics headDiagnostics = encodedHead.diagnostics;
  if (headDiagnostics.seqId == 0) {
    headDiagnostics.seqId = encodedHead.seqId;
    headDiagnostics.slotIndex = encodedHead.slotIndex;
  }
  CommandBufferDiagnostics tailDiagnostics = tail.diagnostics;
  if (tailDiagnostics.seqId == 0) {
    tailDiagnostics.seqId = tail.seqId;
    tailDiagnostics.slotIndex = tail.slotIndex;
  }
  CommandBufferDiagnostics mergedDiagnostics{
      .seqId = tail.seqId,
      .slotIndex = tail.slotIndex,
  };
  mergedDiagnostics = mergeCommandBufferDiagnostics(
      mergedDiagnostics, headDiagnostics);
  mergedDiagnostics = mergeCommandBufferDiagnostics(
      mergedDiagnostics, tailDiagnostics);
  tail.diagnostics = mergedDiagnostics;

  prependMoved(tail.renderEncoderGpuSamples,
               std::move(encodedHead.renderEncoderGpuSamples));
  prependMoved(tail.postCommitCallbacks, std::move(encodedHead.postCommitCallbacks));
  prependMoved(tail.completionCallbacks, std::move(encodedHead.completionCallbacks));
  prependMoved(tail.retainedPayloads, std::move(encodedHead.retainedPayloads));

  if (encodedHead.metalCapture.has_value()) {
    tail.metalCaptureDevice = encodedHead.metalCaptureDevice;
    tail.metalCapture = std::move(encodedHead.metalCapture);
    tail.metalCaptureAlreadyStarted = encodedHead.metalCaptureAlreadyStarted;
  } else {
    tail.metalCaptureAlreadyStarted =
        tail.metalCaptureAlreadyStarted ||
        encodedHead.metalCaptureAlreadyStarted;
  }

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
  return summarizeCommands(seqId, slotIndex, submissionBinding_.slots[slotIndex], resolveSurfaceFlags);
}

CommandBufferDiagnostics QueueLifecycleController::summarizeSubmissionSources(
    const QueueSubmissionRecord& record,
    std::span<const QueueCompletionSource> sources) const {
  if (sources.empty()) {
    return record.diagnostics.seqId == 0
        ? summarizeSubmission(record.seqId, record.slotIndex)
        : record.diagnostics;
  }

  CommandBufferDiagnostics result{
      .seqId = record.seqId,
      .slotIndex = record.slotIndex,
  };
  for (const auto& source : sources) {
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
    if (encodeCv) {
      encodeCv->notify_one();
    }
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
  recordNoEnqueueFirstPublishSlotShapeBeforePublish(
      summarizeNoEnqueueFirstPublishSlotShape(slot));
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

bool QueueLifecycleController::dequeueReadySlot(
    std::unique_lock<std::mutex>& lock,
    ReadySlotSnapshot& out) {
  std::array<ReadySlotSnapshot, 1> snapshots{};
  const size_t count = dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(snapshots));
  if (count == 0) {
    return false;
  }
  DXMT_ASSERT(snapshots[0].slot != nullptr);
  out = snapshots[0];
  return true;
}

size_t QueueLifecycleController::dequeueReadySlotBatch(
    std::unique_lock<std::mutex>& lock,
    std::span<ReadySlotSnapshot> out,
    const ReadySlotBatchAppendPredicate& canAppend) {
  // TLA+: QueueLifecycleRefinement / EncodeDequeue.
  if (out.empty()) {
    return 0;
  }
  auto* readySlots = submissionBinding_.readySlots;
  auto* encodeCv = submissionBinding_.encodeCv;
  auto* stop = submissionBinding_.stop;
  if (!readySlots || !encodeCv || !stop) {
    return 0;
  }

  encodeCv->wait(lock, [&] { return *stop || !readySlots->empty(); });
  if (*stop && readySlots->empty()) {
    return 0;
  }

  perf::countEncodeDequeueReadyDepth(
      static_cast<std::uint64_t>(readySlots->size()));
  const size_t maxCount = std::min(out.size(), readySlots->size());
  size_t count = 0;
  for (size_t i = 0; i < maxCount; ++i) {
    const size_t slotIndex = readySlots->front();
    auto& slot = submissionBinding_.slots[slotIndex];
    if (i != 0 && canAppend &&
        !canAppend(std::span<const ReadySlotSnapshot>(out.data(), i),
                   slotIndex, slot)) {
      break;
    }
    traceEncodeIterationStage("dequeue.selected", slotIndex, slot);
    if (i == 0) {
      recordNoEnqueueWaitGapToEncodeDequeue();
    }
    encodeDequeue(slotIndex, slot.seqId, [&] {
      readySlots->pop_front();
      slot.state = ChunkSlot::State::Encoding;
    });
    traceEncodeIterationStage("dequeue.after-transition", slotIndex, slot);
    traceEncodeIterationStage("dequeue.before-slot-copy", slotIndex, slot);
    out[i] = makeReadySlotSnapshot(slotIndex, slot);
    traceEncodeIterationStage("dequeue.after-slot-copy", slotIndex, *out[i].slot);
    ++count;
  }
  return count;
}

size_t QueueLifecycleController::dequeueReadySlotBatchPrefix(
    std::unique_lock<std::mutex>& lock,
    std::span<ReadySlotSnapshot> out,
    const ReadySlotBatchPrefixSelector& selectPrefix) {
  // TLA+: QueueLifecycleRefinement / EncodeDequeue.
  if (out.empty()) {
    return 0;
  }
  auto* readySlots = submissionBinding_.readySlots;
  auto* encodeCv = submissionBinding_.encodeCv;
  auto* stop = submissionBinding_.stop;
  if (!readySlots || !encodeCv || !stop) {
    return 0;
  }

  encodeCv->wait(lock, [&] { return *stop || !readySlots->empty(); });
  if (*stop && readySlots->empty()) {
    return 0;
  }

  perf::countEncodeDequeueReadyDepth(
      static_cast<std::uint64_t>(readySlots->size()));
  const size_t maxCount = std::min(out.size(), readySlots->size());
  size_t count = 0;
  if (selectPrefix) {
    count = selectPrefix(
        *readySlots,
        std::span<const ChunkSlot>(
            submissionBinding_.slots.data(),
            submissionBinding_.slots.size()),
        maxCount);
    DXMT_ASSERT(count <= maxCount);
    count = std::min(count, maxCount);
  }
  if (count == 0) {
    count = 1u;
  }

  for (size_t i = 0; i < count; ++i) {
    const size_t slotIndex = readySlots->front();
    auto& slot = submissionBinding_.slots[slotIndex];
    traceEncodeIterationStage("dequeue.selected", slotIndex, slot);
    if (i == 0) {
      recordNoEnqueueWaitGapToEncodeDequeue();
    }
    encodeDequeue(slotIndex, slot.seqId, [&] {
      readySlots->pop_front();
      slot.state = ChunkSlot::State::Encoding;
    });
    traceEncodeIterationStage("dequeue.after-transition", slotIndex, slot);
    traceEncodeIterationStage("dequeue.before-slot-copy", slotIndex, slot);
    out[i] = makeReadySlotSnapshot(slotIndex, slot);
    traceEncodeIterationStage("dequeue.after-slot-copy", slotIndex, *out[i].slot);
  }
  return count;
}

bool QueueLifecycleController::stageLastReadySlot(
    std::unique_lock<std::mutex>& lock,
    std::deque<size_t>& stagedSlots,
    size_t expectedSlotIndex) {
  DXMT_ASSERT(lock.owns_lock());
  static_cast<void>(lock);
  auto* readySlots = submissionBinding_.readySlots;
  if (!readySlots || readySlots->empty()) {
    return false;
  }
  if (readySlots->back() != expectedSlotIndex) {
    return false;
  }

  stagedSlots.push_back(expectedSlotIndex);
  readySlots->pop_back();
#ifndef NDEBUG
  assertQueueLifecycleInvariants();
#endif
  return true;
}

size_t QueueLifecycleController::releaseStagedSlotsBeforeReadyTail(
    std::unique_lock<std::mutex>& lock,
    std::deque<size_t>& stagedSlots,
    size_t tailSlotIndex) {
  DXMT_ASSERT(lock.owns_lock());
  static_cast<void>(lock);
  if (stagedSlots.empty()) {
    return 0;
  }

  auto* readySlots = submissionBinding_.readySlots;
  if (!readySlots || readySlots->empty() || readySlots->back() != tailSlotIndex) {
    return 0;
  }

  readySlots->pop_back();
  const size_t releasedCount = stagedSlots.size();
  while (!stagedSlots.empty()) {
    readySlots->push_back(stagedSlots.front());
    stagedSlots.pop_front();
  }
  readySlots->push_back(tailSlotIndex);
#ifndef NDEBUG
  assertQueueLifecycleInvariants();
#endif
  if (submissionBinding_.encodeCv) {
    submissionBinding_.encodeCv->notify_one();
  }
  return releasedCount;
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
    if (!source.slot) {
      return 0;
    }
    const auto& liveSlot = submissionBinding_.slots[source.slotIndex];
    if (&liveSlot != source.slot ||
        liveSlot.state != ChunkSlot::State::Encoding ||
        liveSlot.seqId != source.seqId ||
        liveSlot.seqId == 0 ||
        !commandRangeWithinSlot(liveSlot,
                                source.commandBegin,
                                source.commandCount) ||
        commandRangeHasPresent(liveSlot,
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

size_t QueueLifecycleController::requeueUnsubmittedBatchSources(
    std::unique_lock<std::mutex>& lock,
    std::span<const ReadySlotSnapshot> sources) {
  DXMT_ASSERT(lock.owns_lock());
  static_cast<void>(lock);
  if (sources.empty()) {
    return 0;
  }

  auto* readySlots = submissionBinding_.readySlots;
  auto* encodeCv = submissionBinding_.encodeCv;
  if (!readySlots) {
    return 0;
  }

  for (const auto& source : sources) {
    if (!source.slot ||
        source.slotIndex >= submissionBinding_.slots.size()) {
      return 0;
    }
    auto& liveSlot = submissionBinding_.slots[source.slotIndex];
    if (&liveSlot != source.slot ||
        liveSlot.state != ChunkSlot::State::Encoding ||
        liveSlot.seqId != source.seqId ||
        !commandRangeWithinSlot(liveSlot,
                                source.commandBegin,
                                source.commandCount) ||
        commandRangeHasPresent(liveSlot,
                               source.commandBegin,
                               source.commandCount) != source.hasPresent) {
      return 0;
    }
  }

  for (auto it = sources.rbegin(); it != sources.rend(); ++it) {
    auto& slot = submissionBinding_.slots[it->slotIndex];
    slot.state = ChunkSlot::State::Pending;
    readySlots->push_front(it->slotIndex);
  }
#ifndef NDEBUG
  assertQueueLifecycleInvariants();
#endif
  if (encodeCv) {
    encodeCv->notify_one();
  }
  return sources.size();
}

bool QueueLifecycleController::runEncodeIteration(
    std::unique_lock<std::mutex>& lock,
    const std::function<std::optional<QueueSubmissionRecord>(size_t, ChunkSlot&)>& encodeFn,
    const std::function<void(u64)>& onInlineComplete) {
  // TLA+: EncodeDequeue followed by EncodeSubmitToGpu or EncodeCompleteInline.
  ReadySlotSnapshot source{};
  if (!dequeueReadySlot(lock, source)) {
    return false;
  }
  DXMT_ASSERT(source.slot != nullptr);

  traceEncodeIterationStage("iteration.after-dequeue",
                            source.slotIndex, *source.slot);
  traceEncodeIterationStage("iteration.before-unlock",
                            source.slotIndex, *source.slot);
  lock.unlock();
  traceEncodeIterationStage("iteration.after-unlock",
                            source.slotIndex, *source.slot);
  std::optional<QueueSubmissionRecord> submission;
  if (encodeFn) {
    traceEncodeIterationStage("iteration.before-encodefn",
                              source.slotIndex, *source.slot);
    submission = encodeFn(source.slotIndex, *source.slot);
    if (submission.has_value() && !submission->commandBuffer) {
      submission.reset();
    }
    traceEncodeIterationStage(submission.has_value()
                                  ? "iteration.after-encodefn-submission"
                                  : "iteration.after-encodefn-inline",
                              source.slotIndex,
                              *source.slot);
  } else {
    traceEncodeIterationStage("iteration.no-encodefn",
                              source.slotIndex, *source.slot);
  }
  traceEncodeIterationStage("iteration.before-relock",
                            source.slotIndex, *source.slot);
  lock.lock();
  traceEncodeIterationStage("iteration.after-relock",
                            source.slotIndex, *source.slot);

  if (submission.has_value()) {
    auto postCommitCallbacks = std::move(submission->postCommitCallbacks);
    traceEncodeIterationStage("iteration.before-submit-record",
                              source.slotIndex, *source.slot);
    enqueueSubmission(*submission);
    traceEncodeIterationStage("iteration.after-submit-record",
                              source.slotIndex, *source.slot);
    lock.unlock();
    for (auto& callback : postCommitCallbacks) {
      if (callback) {
        callback();
      }
    }
  } else {
    traceEncodeIterationStage("iteration.before-inline-complete",
                              source.slotIndex, *source.slot);
    completeInlineChunk(source.slotIndex, source.seqId);
    traceEncodeIterationStage("iteration.after-inline-complete",
                              source.slotIndex, *source.slot);
    if (onInlineComplete) {
      onInlineComplete(source.seqId);
    }
  }
  return true;
}

bool QueueLifecycleController::runEncodeBatchIteration(
    std::unique_lock<std::mutex>& lock,
    std::span<ReadySlotSnapshot> scratch,
    const std::function<std::optional<QueueSubmissionRecord>(
        std::span<ReadySlotSnapshot>)>& encodeFn,
    const std::function<void(u64)>& onInlineComplete,
    const ReadySlotBatchAppendPredicate& canAppend,
    const ReadySlotBatchPrefixSelector& selectPrefix) {
  const size_t count = selectPrefix
      ? dequeueReadySlotBatchPrefix(lock, scratch, selectPrefix)
      : dequeueReadySlotBatch(lock, scratch, canAppend);
  if (count == 0) {
    return false;
  }

  const auto sources = scratch.first(count);
  for (const auto& source : sources) {
    DXMT_ASSERT(source.slot != nullptr);
    traceEncodeIterationStage("batch.after-dequeue", source.slotIndex, *source.slot);
  }
  lock.unlock();

  std::optional<QueueSubmissionRecord> submission;
  if (encodeFn) {
    submission = encodeFn(sources);
    if (submission.has_value() && !submission->commandBuffer) {
      submission.reset();
    }
  }

  lock.lock();
  if (submission.has_value()) {
    const std::size_t submittedSourceCount =
        prepareBatchCompletionSources(*submission, sources);
    if (submittedSourceCount == 0 ||
        submittedSourceCount > sources.size()) {
      submission.reset();
    }
#ifndef NDEBUG
    if (submission.has_value() && sources.size() > 1) {
      DXMT_ASSERT(!submission->explicitCompletionSourceSpan().empty());
    }
#endif
    if (!submission.has_value()) {
      const std::size_t requeued =
          requeueUnsubmittedBatchSources(lock, sources);
      DXMT_ASSERT(requeued == sources.size());
      if (requeued == 0) {
        for (const auto& source : sources) {
          DXMT_ASSERT(source.slot != nullptr);
          const u64 seqId = source.seqId;
          completeInlineChunk(source.slotIndex, seqId);
          if (onInlineComplete) {
            onInlineComplete(seqId);
          }
        }
      }
      return true;
    }
    if (submittedSourceCount < sources.size()) {
      const auto suffix = sources.subspan(submittedSourceCount);
      const std::size_t requeued =
          requeueUnsubmittedBatchSources(lock, suffix);
      DXMT_ASSERT(requeued == suffix.size());
      if (requeued != suffix.size()) {
        submission.reset();
        return true;
      }
    }
    auto postCommitCallbacks = std::move(submission->postCommitCallbacks);
    enqueueSubmission(*submission);
    lock.unlock();
    for (auto& callback : postCommitCallbacks) {
      if (callback) {
        callback();
      }
    }
  } else {
    for (const auto& source : sources) {
      DXMT_ASSERT(source.slot != nullptr);
      const u64 seqId = source.seqId;
      completeInlineChunk(source.slotIndex, seqId);
      if (onInlineComplete) {
        onInlineComplete(seqId);
      }
    }
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

void QueueLifecycleController::submitEncodedSubmission(
    std::unique_lock<std::mutex>& lock,
    QueueSubmissionRecord& record) {
  DXMT_ASSERT(lock.owns_lock());
  static_cast<void>(lock);
  submit(record);
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
  if (waitNeeded) {
    auto* encodeCv = submissionBinding_.encodeCv;
    {
      std::lock_guard pendingLock(pendingCompletionMutex_);
      ++producerSequenceWaitDepth_;
    }
    if (encodeCv) {
      encodeCv->notify_one();
    }
    finishCv->wait(lock, [&] { return *stop || *completedSeqId >= targetSeqId; });
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
    const auto fallbackSources = fallbackCompletionSource(
        pending.slotIndex,
        pending.seqId,
        pending.diagnostics.hasPresent,
        pending.slotIndex < slots.size()
            ? slots[pending.slotIndex].commandCount()
            : std::size_t{0});
    const auto completionSources = completionSourcesFor(
        pending.explicitCompletionSourceSpan(),
        fallbackSources);
    for (const auto& source : completionSources) {
      DXMT_ASSERT(source.slotIndex < slots.size());
      DXMT_ASSERT(slots[source.slotIndex].state == ChunkSlot::State::GPU);
      DXMT_ASSERT(slots[source.slotIndex].seqId == source.seqId);
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

void QueueLifecycleController::submit(QueueSubmissionRecord& record) {
  const auto fallbackSources = fallbackCompletionSource(
      record.slotIndex,
      record.seqId,
      record.diagnostics.hasPresent,
      record.slotIndex < submissionBinding_.slots.size()
          ? submissionBinding_.slots[record.slotIndex].commandCount()
          : std::size_t{0});
  const auto completionSources = completionSourcesFor(
      record.explicitCompletionSourceSpan(), fallbackSources);
  const CommandBufferDiagnostics diagnostics =
      summarizeSubmissionSources(record, completionSources);
  for (const auto& source : completionSources) {
    const auto beforeCommitState = currentState();
#ifndef NDEBUG
    DXMT_ASSERT(source.slotIndex < submissionBinding_.slots.size());
    DXMT_ASSERT(submissionBinding_.slots[source.slotIndex].state ==
                ChunkSlot::State::Encoding);
    DXMT_ASSERT(submissionBinding_.slots[source.slotIndex].seqId == source.seqId);
#endif
    if (source.slotIndex < submissionBinding_.slots.size()) {
      submissionBinding_.slots[source.slotIndex].state = ChunkSlot::State::GPU;
    }
    const auto afterCommitState = currentState();
#ifndef NDEBUG
    assertQueueLifecycleInvariants();
#endif

    observeTransition(QueueTransitionRecord{
        .before = beforeCommitState,
        .after = afterCommitState,
        .slotIndex = source.slotIndex,
        .eventSeqId = source.seqId,
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
    pending.slotIndex = record.slotIndex;
    pending.seqId = record.seqId;
    pending.fixedCompletionSources = record.fixedCompletionSources;
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
    const u64 completedSeqId = binding.completedSeqId ? *binding.completedSeqId : 0;
    const auto fallbackSources = fallbackCompletionSource(
        pending.slotIndex,
        pending.seqId,
        pending.diagnostics.hasPresent,
        pending.slotIndex < binding.slots.size()
            ? binding.slots[pending.slotIndex].commandCount()
            : std::size_t{0});
    const auto completionSources = completionSourcesFor(
        pending.explicitCompletionSourceSpan(),
        fallbackSources);
    for (const auto& source : completionSources) {
      const QueueControllerState before = makeBoundQueueState(binding);
      appendCompletionSourcesToQueues(
          *binding.completedSeqQueue,
          binding.completedPresentSeqQueue,
          completedSeqId,
          std::span<const QueueCompletionSource>(&source, 1));
      const QueueControllerState after = makeBoundQueueState(binding);
#ifndef NDEBUG
      assertQueueLifecycleInvariants();
#endif
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
#ifndef NDEBUG
    assertPendingCompletionInvariantsLocked();
#endif
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
