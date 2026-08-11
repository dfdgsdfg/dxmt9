#include "dxmt9_parallel_render_pass.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt9::encoders {
namespace {

using Fallback = SealedParallelPassSnapshotFallback;
using Kind = core::MetalCommandKind;

constexpr std::size_t fallbackIndex(Fallback fallback) noexcept {
  return static_cast<std::size_t>(fallback);
}

SealedParallelPassSnapshotBatchResult rejectBatch(
    Fallback fallback,
    SealedParallelPassSnapshotBatch& snapshots) noexcept {
  snapshots.reset();
  SealedParallelPassSnapshotBatchResult result{
      .fallback = fallback,
      .considered = true,
  };
  if (fallback != Fallback::None && fallback != Fallback::Count) {
    result.rejectionCounts[fallbackIndex(fallback)] = 1u;
  }
  return result;
}

void noteRejection(SealedParallelPassSnapshotBatchResult& result,
                   Fallback fallback) noexcept {
  if (fallback == Fallback::None || fallback == Fallback::Count) {
    return;
  }
  ++result.rejectionCounts[fallbackIndex(fallback)];
  if (result.fallback == Fallback::None) {
    result.fallback = fallback;
  }
}

bool appendResources(core::ExactResourceSet& destination,
                     const core::ExactResourceSet& source) noexcept {
  if (!destination.complete() || !source.complete() ||
      !source.canonicalized()) {
    destination.flags &= ~core::ExactResourceSetComplete;
    return false;
  }
  destination.flags |= core::ExactResourceSetCanonicalized;
  for (std::uint32_t i = 0; i < source.count; ++i) {
    if (!destination.add(source.handles[i])) {
      return false;
    }
  }
  return destination.complete();
}

bool canonicalizeResources(
    const core::ExactResourceSet& raw,
    const ParallelPassResourceIdentityProof& proof,
    core::ExactResourceSet& canonical) noexcept {
  canonical = {};
  if (!raw.complete() || !proof.valid()) {
    canonical.flags &= ~core::ExactResourceSetComplete;
    return false;
  }
  for (std::uint32_t i = 0; i < raw.count; ++i) {
    std::uint64_t resolved = 0;
    if (!proof.resolve(proof.context, raw.handles[i], resolved) ||
        resolved == 0u || !canonical.add(resolved)) {
      canonical.flags &= ~core::ExactResourceSetComplete;
      return false;
    }
  }
  canonical.flags |= core::ExactResourceSetCanonicalized;
  return canonical.complete();
}

core::RenderAttachmentKey attachmentKeyForClear(
    const core::ClearCommandView& clear) noexcept {
  core::RenderAttachmentKey key{
      .color = clear.colorAttachments,
      .depthStencil = clear.depthStencil,
  };
  for (const auto& attachment : key.color) {
    key.sampleCount = std::max(key.sampleCount, attachment.sampleCount);
  }
  key.sampleCount = std::max(key.sampleCount,
                             key.depthStencil.sampleCount);
  return key;
}

ParallelPassCommandLocator commandLocator(
    const EncodePartitionReplayStream& stream,
    std::size_t replayOrdinal,
    std::uint32_t commandIndex,
    Kind kind) noexcept {
  if (replayOrdinal > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  return ParallelPassCommandLocator{
      .source = RetainedEncodeSourceLocator{
          .tapeSource = stream.source.source,
          .retainedSourceIndex = 0u,
          .slotIndex = static_cast<std::uint32_t>(stream.source.slotIndex),
          .seqId = stream.source.seqId,
      },
      .replayOrdinal = static_cast<std::uint32_t>(replayOrdinal),
      .commandIndex = commandIndex,
      .kind = kind,
      .valid = stream.source.source.valid() && stream.source.seqId != 0u &&
          stream.source.slotIndex <= std::numeric_limits<std::uint32_t>::max(),
  };
}

bool isAllowedCoordinatorBoundary(Kind kind) noexcept {
  return kind == Kind::Clear || kind == Kind::Present;
}

}  // namespace

SealedParallelPassSnapshotBatchResult produceSealedParallelPassSnapshots(
    const SealedParallelPassSnapshotInput& input,
    SealedParallelPassSnapshotBatch& snapshots) noexcept {
  snapshots.reset();
  if (!input.stream) {
    return rejectBatch(Fallback::PlanMissing, snapshots);
  }
  if (!input.planValidated) {
    return rejectBatch(Fallback::PlanNotValidated, snapshots);
  }
  const auto& stream = *input.stream;
  if (!stream.valid || !stream.source.source.valid() ||
      stream.source.seqId == 0u || !stream.source.payload.valid() ||
      stream.replayOrdinalCount() == 0u ||
      stream.replayOrdinalCount() >
          std::numeric_limits<std::uint32_t>::max()) {
    return rejectBatch(Fallback::ReplayInvalid, snapshots);
  }
  // Query, UpdateTexture, and ordered controls are normally excluded before a
  // SourcePayloadView exists. Any other helper command that does reach this
  // stream is still a source-wide fail-closed observation input. Clear and
  // Present are the two coordinator-owned pass boundaries this producer can
  // reason about without executing them.
  for (std::size_t ordinal = 0;
       ordinal < stream.replayOrdinalCount(); ++ordinal) {
    std::uint32_t commandIndex = 0;
    if (!stream.commandIndexAt(ordinal, commandIndex)) {
      return rejectBatch(Fallback::ReplayInvalid, snapshots);
    }
    const Kind kind = stream.source.payload.commandAt(commandIndex).kind();
    if (kind != Kind::DrawRun && !isAllowedCoordinatorBoundary(kind)) {
      return rejectBatch(Fallback::CoordinatorCommand, snapshots);
    }
  }

  SealedParallelPassSnapshotBatchResult result{
      .fallback = Fallback::None,
      .considered = true,
  };
  SealedParallelPassSnapshot current{};
  ParallelPassCommandLocator pendingClear{};
  core::RenderAttachmentKey pendingClearAttachments{};
  std::uint64_t passActionEpoch =
      input.proofs.coordinator.firstPassActionEpoch;
  bool passActionEpochValid = true;
  bool active = false;
  bool startProven = input.sourceStartsPass;
  bool attachmentKnown = false;
  bool candidateRejected = false;
  std::array<bool, static_cast<std::size_t>(Fallback::Count)>
      candidateRejections{};
  core::RenderRoute candidateRoute = core::RenderRoute::Unknown;

  auto rejectCandidate = [&](Fallback fallback) {
    if (fallback == Fallback::None || fallback == Fallback::Count) {
      return;
    }
    candidateRejections[fallbackIndex(fallback)] = true;
    if (!candidateRejected) {
      candidateRejected = true;
    }
  };
  auto advancePassActionEpoch = [&]() {
    if (!passActionEpochValid) {
      return;
    }
    if (passActionEpoch == std::numeric_limits<std::uint64_t>::max()) {
      passActionEpochValid = false;
      return;
    }
    ++passActionEpoch;
  };
  auto resetCandidate = [&]() {
    current.reset();
    active = false;
    attachmentKnown = false;
    candidateRejected = false;
    candidateRejections.fill(false);
    candidateRoute = core::RenderRoute::Unknown;
  };
  auto startCandidate = [&](std::size_t ordinal,
                            std::uint32_t commandIndex,
                            const core::RenderAttachmentKey& attachments) {
    resetCandidate();
    active = true;
    attachmentKnown = true;
    current.source = stream.source.source;
    current.seqId = stream.source.seqId;
    const bool leadingClearMatches =
        pendingClear.valid && pendingClearAttachments == attachments;
    if (pendingClear.valid && !leadingClearMatches) {
      advancePassActionEpoch();
    }
    current.passActionEpoch = passActionEpoch;
    current.replayOrdinalBegin = static_cast<std::uint32_t>(ordinal);
    current.attachments = attachments;
    current.firstDraw = commandLocator(
        stream, ordinal, commandIndex, Kind::DrawRun);
    if (leadingClearMatches) {
      current.leadingClear = pendingClear;
    }
    pendingClear = {};
    if (!startProven) {
      rejectCandidate(Fallback::UnsealedStart);
    }
    if (!passActionEpochValid || current.passActionEpoch == 0u) {
      rejectCandidate(Fallback::PassActionEpoch);
    }
    if (!input.proofs.coordinator.proves(ParallelPassQueryAbsent)) {
      rejectCandidate(Fallback::QueryState);
    }
    if (!input.proofs.coordinator.proves(ParallelPassUpdateTextureAbsent)) {
      rejectCandidate(Fallback::UpdateTextureState);
    }
    if (!input.proofs.coordinator.proves(ParallelPassCaptureInactive)) {
      rejectCandidate(Fallback::CaptureState);
    }
    if (!input.proofs.coordinator.proves(
            ParallelPassInitializerIndependent)) {
      rejectCandidate(Fallback::InitializerState);
    }
    if (!input.proofs.coordinator.proves(ParallelPassOrderedControlAbsent)) {
      rejectCandidate(Fallback::OrderedControlState);
    }
    if (!input.proofs.coordinator.proves(
            ParallelPassSidecarObservationAbsent)) {
      rejectCandidate(Fallback::SidecarState);
    }
    if (!input.proofs.resources.valid()) {
      rejectCandidate(Fallback::ResourceIdentityProof);
    }
    if (!input.proofs.route.valid()) {
      rejectCandidate(Fallback::RenderRoute);
    }
  };
  auto appendChild = [&](std::uint32_t replayOrdinalBegin,
                         std::uint32_t replayOrdinalCount,
                         std::uint32_t drawBegin,
                         std::uint32_t drawCount,
                         bool completeCommands) {
    if (current.childCount >= kParallelRenderPassChildCapacity ||
        replayOrdinalCount == 0u || drawCount == 0u) {
      rejectCandidate(Fallback::ChildCapacity);
      return false;
    }
    std::uint32_t commandIndex = 0u;
    if (!stream.commandIndexAt(replayOrdinalBegin, commandIndex)) {
      rejectCandidate(Fallback::FirstDrawSnapshot);
      return false;
    }
    const auto source = stream.source.payload.commandAt(commandIndex);
    if (source.kind() != Kind::DrawRun ||
        !source.command.drawState.hot ||
        !source.command.drawState.shaderLayout ||
        !source.command.drawState.debug ||
        !source.command.drawRunRecord || source.command.drawParams.empty() ||
        drawBegin >= source.command.drawParams.size() ||
        drawCount > source.command.drawParams.size() - drawBegin) {
      rejectCandidate(Fallback::FirstDrawSnapshot);
      return false;
    }
    EncodePartitionEntrySnapshot entry{};
    if (drawBegin > UINT32_MAX - source.command.drawRunRecord->firstParam) {
      rejectCandidate(Fallback::FirstDrawSnapshot);
      return false;
    }
    if (!buildEncodePartitionEntrySnapshot(
            stream, replayOrdinalBegin,
            source.command.drawRunRecord->firstParam + drawBegin, entry)) {
      rejectCandidate(Fallback::FirstDrawSnapshot);
      return false;
    }
    const auto rawReads = core::makeDrawEntryReadSet(
        source.command.drawState);
    core::ExactResourceSet reads{};
    if (!canonicalizeResources(rawReads, input.proofs.resources, reads)) {
      rejectCandidate(rawReads.complete()
                          ? Fallback::ResourceIdentityProof
                          : Fallback::ResourceSetIncomplete);
      return false;
    }
    const core::RenderRoute route = input.proofs.route.valid()
        ? input.proofs.route.resolve(input.proofs.route.context,
                                     source.command.drawState)
        : core::RenderRoute::Unknown;
    if (route == core::RenderRoute::Unknown ||
        (candidateRoute != core::RenderRoute::Unknown &&
         route != candidateRoute)) {
      rejectCandidate(Fallback::RenderRoute);
      return false;
    }
    const std::size_t child = current.childCount++;
    current.ranges[child] = EncodePartitionRangeSnapshot{
        .kind = EncodePartitionRangeKind::DrawRunEntries,
        .replayOrdinalBegin = replayOrdinalBegin,
        .replayOrdinalCount = 1u,
        .drawEntryCount = drawCount,
        .entry = entry,
    };
    current.firstDraws[child] = ParallelFirstDrawSnapshot{
        .provenance = entry,
        .entryRender = core::RenderContinuationKey{
            .attachments = current.attachments,
            .entryReads = reads,
            .route = route,
            .passActionEpoch = current.passActionEpoch,
            .flags = core::RenderContinuationKeyValid |
                     core::RenderContinuationEntryStateComplete,
        },
        .generation = stream.source.seqId,
        .complete = stream.source.seqId != 0u && reads.complete() &&
            reads.canonicalized(),
    };
    current.childReplayOrdinalBegins[child] = replayOrdinalBegin;
    current.childReplayOrdinalCounts[child] = replayOrdinalCount;
    current.childrenCoverCompleteCommands = completeCommands;
    return true;
  };
  auto buildCandidateChildren = [&](std::uint32_t replayOrdinalEnd) {
    current.childCount = 0u;
    if (replayOrdinalEnd <= current.replayOrdinalBegin ||
        current.drawCount < kProductionPartitionDrawThreshold) {
      return;
    }
    const std::uint32_t commandCount =
        replayOrdinalEnd - current.replayOrdinalBegin;
    if (commandCount == 1u) {
      std::uint32_t commandIndex = 0u;
      if (!stream.commandIndexAt(current.replayOrdinalBegin, commandIndex)) {
        rejectCandidate(Fallback::FirstDrawSnapshot);
        return;
      }
      const auto source = stream.source.payload.commandAt(commandIndex);
      if (source.kind() != Kind::DrawRun ||
          source.command.drawParams.size() > UINT32_MAX) {
        rejectCandidate(Fallback::FirstDrawSnapshot);
        return;
      }
      std::uint32_t drawBegin = 0u;
      std::uint32_t remaining =
          static_cast<std::uint32_t>(source.command.drawParams.size());
      while (remaining != 0u) {
        std::uint32_t count = std::min(
            remaining, kProductionPartitionTargetDraws);
        if (remaining > count &&
            remaining - count < kProductionPartitionMinimumDraws) {
          count = remaining;
        }
        if (!appendChild(current.replayOrdinalBegin, 1u, drawBegin,
                         count, false)) {
          return;
        }
        drawBegin += count;
        remaining -= count;
      }
      return;
    }

    const std::uint64_t targetChildren64 =
        (current.drawCount + kProductionPartitionTargetDraws - 1u) /
        kProductionPartitionTargetDraws;
    const std::uint32_t targetChildren = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(
            kParallelRenderPassChildCapacity,
            std::max<std::uint64_t>(2u, targetChildren64)));
    const std::uint32_t childCount = std::min(commandCount, targetChildren);
    std::uint32_t ordinal = current.replayOrdinalBegin;
    std::uint64_t remainingDraws = current.drawCount;
    for (std::uint32_t child = 0u; child < childCount; ++child) {
      const std::uint32_t childBegin = ordinal;
      const std::uint32_t groupsRemaining = childCount - child;
      const std::uint64_t target =
          (remainingDraws + groupsRemaining - 1u) / groupsRemaining;
      std::uint64_t childDraws = 0u;
      do {
        std::uint32_t commandIndex = 0u;
        if (!stream.commandIndexAt(ordinal, commandIndex)) {
          rejectCandidate(Fallback::FirstDrawSnapshot);
          return;
        }
        const auto source = stream.source.payload.commandAt(commandIndex);
        if (source.kind() != Kind::DrawRun ||
            source.command.drawParams.empty()) {
          rejectCandidate(Fallback::NonChildDrawRun);
          return;
        }
        childDraws += source.command.drawParams.size();
        ++ordinal;
      } while (ordinal < replayOrdinalEnd && childDraws < target &&
               replayOrdinalEnd - ordinal > groupsRemaining - 1u);
      std::uint32_t firstCommandIndex = 0u;
      if (!stream.commandIndexAt(childBegin, firstCommandIndex)) {
        rejectCandidate(Fallback::FirstDrawSnapshot);
        return;
      }
      const auto firstSource =
          stream.source.payload.commandAt(firstCommandIndex);
      if (firstSource.command.drawParams.size() > UINT32_MAX ||
          !appendChild(childBegin, ordinal - childBegin, 0u,
                       static_cast<std::uint32_t>(
                           firstSource.command.drawParams.size()), true)) {
        return;
      }
      remainingDraws -= childDraws;
    }
    if (ordinal != replayOrdinalEnd || remainingDraws != 0u) {
      rejectCandidate(Fallback::FirstDrawSnapshot);
    }
  };
  auto sealCandidate = [&](std::uint32_t replayOrdinalEnd,
                           ParallelPassCommandLocator sealingCommand,
                           bool sealedAtSourceEnd) {
    if (!active) {
      return;
    }
    ++result.candidateCount;
    current.replayOrdinalEnd = replayOrdinalEnd;
    current.sealingCommand = sealingCommand;
    current.sealedAtSourceEnd = sealedAtSourceEnd;
    if (!startProven) {
      rejectCandidate(Fallback::UnsealedStart);
    }
    const bool endProven = sealingCommand.valid || sealedAtSourceEnd;
    if (!endProven) {
      rejectCandidate(Fallback::UnsealedEnd);
    }
    if (replayOrdinalEnd <= current.replayOrdinalBegin) {
      rejectCandidate(Fallback::UnsealedEnd);
    }
    const bool boundaryComplete = startProven &&
        endProven && replayOrdinalEnd > current.replayOrdinalBegin;
    if (boundaryComplete) {
      ++result.sealedCount;
    }
    buildCandidateChildren(replayOrdinalEnd);
    if (current.childCount < 2u) {
      rejectCandidate(Fallback::TooFewChildren);
    } else if (current.childCount > kParallelRenderPassChildCapacity) {
      rejectCandidate(Fallback::ChildCapacity);
    }
    if (!candidateRejected &&
        snapshots.count >= snapshots.passes.size()) {
      rejectCandidate(Fallback::PassCapacity);
    }
    if (candidateRejected) {
      for (std::size_t i = 1u; i < candidateRejections.size(); ++i) {
        if (candidateRejections[i]) {
          noteRejection(result, static_cast<Fallback>(i));
        }
      }
    } else {
      snapshots.passes[snapshots.count++] = current;
      ++result.eligibleCount;
      result.childCount += current.childCount;
      result.drawCount += current.drawCount;
      result.childCountMax = std::max(result.childCountMax,
                                      current.childCount);
      result.drawCountMax = std::max(result.drawCountMax,
                                     current.drawCount);
    }
    resetCandidate();
  };

  for (std::size_t ordinal = 0;
       ordinal < stream.replayOrdinalCount(); ++ordinal) {
    std::uint32_t commandIndex = 0;
    if (!stream.commandIndexAt(ordinal, commandIndex)) {
      return rejectBatch(Fallback::ReplayInvalid, snapshots);
    }
    const auto source = stream.source.payload.commandAt(commandIndex);
    const Kind kind = source.kind();
    const auto locator = commandLocator(stream, ordinal, commandIndex, kind);

    if (kind == Kind::Clear) {
      const bool endedPass = active;
      sealCandidate(static_cast<std::uint32_t>(ordinal), locator, false);
      if (!source.clear.has_value() || !locator.valid) {
        return rejectBatch(Fallback::ReplayInvalid, snapshots);
      }
      if (endedPass || pendingClear.valid) {
        advancePassActionEpoch();
      }
      if (source.clear->rects.empty()) {
        pendingClear = locator;
        pendingClearAttachments = attachmentKeyForClear(*source.clear);
      } else {
        pendingClear = {};
        advancePassActionEpoch();
      }
      startProven = true;
      continue;
    }
    if (kind == Kind::Present) {
      sealCandidate(static_cast<std::uint32_t>(ordinal), locator, false);
      pendingClear = {};
      startProven = true;
      advancePassActionEpoch();
      continue;
    }
    if (kind != Kind::DrawRun || !source.command.drawState.hot ||
        !source.command.drawState.shaderLayout ||
        !source.command.drawState.debug ||
        !source.command.drawRunRecord || source.command.drawParams.empty() ||
        !locator.valid) {
      return rejectBatch(Fallback::ReplayInvalid, snapshots);
    }

    const auto attachments =
        core::makeRenderAttachmentKey(*source.command.drawState.hot);
    if (active && attachmentKnown && attachments != current.attachments) {
      sealCandidate(static_cast<std::uint32_t>(ordinal), locator, false);
      startProven = true;
      advancePassActionEpoch();
    }
    if (!active) {
      startCandidate(ordinal, commandIndex, attachments);
    }

    const auto rawWrites =
        core::makeRenderAttachmentWriteSet(*source.command.drawState.hot);
    const auto rawReads = core::makeDrawEntryReadSet(source.command.drawState);
    core::ExactResourceSet writes{};
    core::ExactResourceSet reads{};
    const bool writesCanonical = canonicalizeResources(
        rawWrites, input.proofs.resources, writes);
    const bool readsCanonical = canonicalizeResources(
        rawReads, input.proofs.resources, reads);
    if (!input.proofs.resources.valid()) {
      rejectCandidate(Fallback::ResourceIdentityProof);
    } else if (!writesCanonical || !readsCanonical) {
      rejectCandidate(rawWrites.complete() && rawReads.complete()
                          ? Fallback::ResourceIdentityProof
                          : Fallback::ResourceSetIncomplete);
    }
    if (writesCanonical && readsCanonical) {
      if (current.attachmentWrites.count != 0u &&
          writes != current.attachmentWrites) {
        rejectCandidate(Fallback::AttachmentMismatch);
      }
      if (current.attachmentWrites.count == 0u) {
        current.attachmentWrites = writes;
      }
      if (!appendResources(current.resourceReads, reads)) {
        rejectCandidate(Fallback::ResourceSetIncomplete);
      }
      if (current.attachmentWrites.overlaps(reads)) {
        rejectCandidate(Fallback::ResourceHazard);
      }
    }
    core::RenderRoute drawRoute = core::RenderRoute::Unknown;
    if (input.proofs.route.valid()) {
      drawRoute = input.proofs.route.resolve(
          input.proofs.route.context, source.command.drawState);
    }
    if (drawRoute == core::RenderRoute::Unknown) {
      rejectCandidate(Fallback::RenderRoute);
    } else if (candidateRoute == core::RenderRoute::Unknown) {
      candidateRoute = drawRoute;
    } else if (candidateRoute != drawRoute) {
      rejectCandidate(Fallback::RenderRoute);
    }
    if (source.command.drawParams.size() >
        std::numeric_limits<std::uint64_t>::max() - current.drawCount) {
      rejectCandidate(Fallback::FirstDrawSnapshot);
    } else {
      current.drawCount += source.command.drawParams.size();
    }

  }

  if (active) {
    if (input.sourceEndsPass) {
      sealCandidate(static_cast<std::uint32_t>(stream.replayOrdinalCount()),
                    {}, true);
    } else {
      rejectCandidate(Fallback::UnsealedEnd);
      sealCandidate(static_cast<std::uint32_t>(stream.replayOrdinalCount()),
                    {}, false);
    }
  }
  result.eligibleCountMax = result.eligibleCount;
  return result;
}

}  // namespace dxmt9::encoders
