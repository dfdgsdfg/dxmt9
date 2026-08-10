#include "dxmt9_encode_session_storage_internal.hpp"

#include "dxmt9/assert.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dxmt9::encoders::encode_session {

namespace {

constexpr std::uint32_t kMaxRenderEncoderGpuSamples = 8192;

}  // namespace

EncodeChunkSessionStorage* createStorage() {
  return new EncodeChunkSessionStorage{};
}

void destroyStorage(EncodeChunkSessionStorage* storage) noexcept {
  delete storage;
}

void resetStorage(EncodeChunkSessionStorage& storage) {
  DXMT_ASSERT(!storage.encoder.activeRenderEncoder);
  DXMT_ASSERT(!storage.encoder.activeBlitEncoder);
  DXMT_ASSERT(!storage.encoder.hasActiveRender);
  storage = EncodeChunkSessionStorage{};
}

bool storageHasActiveRender(
    const EncodeChunkSessionStorage& storage) noexcept {
  return static_cast<bool>(storage.encoder.activeRenderEncoder);
}

std::optional<ActiveRenderDependencySnapshot>
storageActiveRenderDependencySnapshot(
    const EncodeChunkSessionStorage& storage) noexcept {
  if (!storage.encoder.activeRenderEncoder ||
      !storage.encoder.hasActiveRender ||
      storage.pass.pendingClear.has_value()) {
    return std::nullopt;
  }

  ActiveRenderDependencySnapshot snapshot{};
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    snapshot.colorAttachments[i] =
        core::Handle{storage.pass.activeKey.colorHandles[i]};
  }
  snapshot.depthStencil = core::Handle{storage.pass.activeKey.depthHandle};
  snapshot.sampleCount = storage.pass.activeKey.sampleCount;
  snapshot.dependencyCount = static_cast<std::uint32_t>(
      storage.pass.activeWriteHazard.exact.count);
  if (storage.pass.activeWriteHazard.exact.count >
      snapshot.writeDependencies.size()) {
    return snapshot;
  }
  for (std::size_t i = 0;
       i < storage.pass.activeWriteHazard.exact.count; ++i) {
    snapshot.writeDependencies[i] =
        core::Handle{storage.pass.activeWriteHazard.exact.handles[i]};
  }
  auto dependencyContains = [&](core::Handle handle) {
    if (handle.value == 0) {
      return true;
    }
    for (std::size_t i = 0; i < snapshot.dependencyCount; ++i) {
      if (snapshot.writeDependencies[i] == handle) {
        return true;
      }
    }
    return false;
  };
  bool hasAttachment = snapshot.depthStencil.value != 0;
  bool coversAttachments = dependencyContains(snapshot.depthStencil);
  for (const core::Handle handle : snapshot.colorAttachments) {
    hasAttachment = hasAttachment || handle.value != 0;
    coversAttachments = coversAttachments && dependencyContains(handle);
  }
  snapshot.complete = snapshot.sampleCount != 0 && hasAttachment &&
                      coversAttachments;
  return snapshot;
}

std::optional<RenderPassInstanceToken> storageActiveRenderInstanceToken(
    const EncodeChunkSessionStorage& storage) noexcept {
  if (!storage.encoder.activeRenderEncoder ||
      !storage.encoder.hasActiveRender ||
      storage.pass.pendingClear.has_value() ||
      !storage.pass.activeInstance.valid()) {
    return std::nullopt;
  }
  return storage.pass.activeInstance;
}

EncodeSessionReplayFrontierState storageReplayFrontierState(
    const EncodeChunkSessionStorage& storage) noexcept {
  const bool hasActiveRender = storage.encoder.activeRenderEncoder ||
                               storage.encoder.hasActiveRender;
  const auto snapshot = hasActiveRender
                            ? storageActiveRenderDependencySnapshot(storage)
                            : std::nullopt;
  return replayFrontierStateForFacts({
      .hasPendingClearPayload = storage.pass.pendingClear.has_value(),
      .hasPendingClearCommand =
          storage.pass.pendingClearCommand !=
          core::metalqueue::PublishedCommandRef{},
      .hasActiveRenderEncoder =
          static_cast<bool>(storage.encoder.activeRenderEncoder),
      .hasActiveBlitEncoder =
          static_cast<bool>(storage.encoder.activeBlitEncoder),
      .hasActiveRender = storage.encoder.hasActiveRender,
      .activeRenderSnapshotComplete = snapshot && snapshot->complete,
  });
}

bool storageHasDeferredSubmissionPayload(
    const EncodeChunkSessionStorage& storage) noexcept {
  return static_cast<bool>(storage.encoder.activeRenderEncoder) ||
         static_cast<bool>(storage.encoder.activeBlitEncoder) ||
         storage.encoder.hasActiveRender ||
         storage.pass.pendingClear.has_value() ||
         !storage.completion.postCommitCallbacks.empty() ||
         !storage.completion.completionCallbacks.empty() ||
         storage.diagnostics.metalCaptureRequest.has_value() ||
         static_cast<bool>(storage.diagnostics.renderEncoderGpuSampleBuffer) ||
         !storage.diagnostics.renderEncoderGpuSamples.empty();
}

std::optional<core::metalqueue::PublishedCommandRef>
storagePendingClearCommand(
    const EncodeChunkSessionStorage& storage) noexcept {
  if (!storage.pass.pendingClear.has_value()) {
    return std::nullopt;
  }
  return storage.pass.pendingClearCommand;
}

void initializeStorage(EncodeChunkSessionStorage& storage,
                       const uniform::DirtyState& dirty) {
  if (storage.binding.initialized) {
    return;
  }
  storage.binding.uniformDirty = dirty;
  storage.binding.initialized = true;
}

EncodeChunkSessionStorage makeStorage(const uniform::DirtyState& dirty) {
  EncodeChunkSessionStorage storage{};
  initializeStorage(storage, dirty);
  return storage;
}

void initializeGpuSamplingStorage(EncodeChunkSessionStorage& storage,
                                  WMT::Device device,
                                  std::size_t commandCount) {
  auto& diagnostics = storage.diagnostics;
  if (diagnostics.requestedRenderEncoderGpuSamples != 0) {
    return;
  }
  if (!renderEncoderGpuTimeEnabled() ||
      !device.supportsCounterSampling(WMTCounterSamplingPointAtStageBoundary)) {
    return;
  }
  diagnostics.requestedRenderEncoderGpuSamples =
      static_cast<std::uint32_t>(std::min<std::size_t>(
          kMaxRenderEncoderGpuSamples,
          std::max<std::size_t>(2u, commandCount * 2u + 16u)));
  diagnostics.renderEncoderGpuSampleBuffer =
      device.newCounterSampleBuffer(
          diagnostics.requestedRenderEncoderGpuSamples,
                                    /*shared=*/true);
  if (!diagnostics.renderEncoderGpuSampleBuffer) {
    diagnostics.requestedRenderEncoderGpuSamples = 0;
  }
}

std::size_t gpuSamplingCommandCount(
    core::SourcePayloadView payload,
    std::uint64_t sourceSeqId,
    std::size_t currentCommandCount,
    std::span<const core::metalqueue::ResolvedPublishedSource>
        lookaheadSources) noexcept {
  if (lookaheadSources.empty()) {
    return currentCommandCount;
  }

  constexpr std::size_t kMaxSampledCommands =
      (kMaxRenderEncoderGpuSamples - 16u) / 2u;
  std::size_t total = 0;
  bool includesCurrentSlot = false;
  auto addCommands = [&](std::size_t count) noexcept {
    if (total >= kMaxSampledCommands) {
      return;
    }
    const std::size_t remaining = kMaxSampledCommands - total;
    total += std::min(count, remaining);
  };
  for (const auto& source : lookaheadSources) {
    if (!source.payload.valid() && source.seqId == 0) {
      continue;
    }
    includesCurrentSlot =
        includesCurrentSlot || source.payload == payload ||
        source.seqId == sourceSeqId;
    addCommands(source.commandCount);
  }
  if (!includesCurrentSlot) {
    addCommands(currentCommandCount);
  }
  return std::min(kMaxSampledCommands,
                  std::max<std::size_t>(currentCommandCount, total));
}

}  // namespace dxmt9::encoders::encode_session
