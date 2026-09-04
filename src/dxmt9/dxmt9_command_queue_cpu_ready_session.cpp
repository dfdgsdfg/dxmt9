#include "dxmt9_command_queue.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_resource_initializer.hpp"
#include "framegraph/fg_multi_source_planner.hpp"
#include "render/backend_interface.hpp"
#include "render/dag_observer.hpp"
#include "render/deferred_terminal_suffix.hpp"
#include "render/encode_session_admission.hpp"
#include "render/framegraph_backend.hpp"
#include "render/session_source_policy.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace dxmt9 {

namespace {

class ScopedQueueUnlock final {
 public:
  explicit ScopedQueueUnlock(std::unique_lock<std::mutex>& lock) noexcept
      : lock_(lock) {
    DXMT_ASSERT(lock_.owns_lock());
    lock_.unlock();
  }
  ScopedQueueUnlock(const ScopedQueueUnlock&) = delete;
  ScopedQueueUnlock& operator=(const ScopedQueueUnlock&) = delete;
  ~ScopedQueueUnlock() {
    if (!lock_.owns_lock()) {
      lock_.lock();
    }
  }

 private:
  std::unique_lock<std::mutex>& lock_;
};

[[noreturn]] void abortCpuReadySessionFailOpen(const char* reason) {
  std::fprintf(stderr,
               "[dxmt9-queue] fatal: encoded CPU-ready session pending work "
               "could not fail-open by submit (%s)\n",
               reason ? reason : "unknown");
  std::abort();
}

perf::CpuReadyMultiSourcePlannerOutcome plannerPerfOutcome(
    framegraph::MultiSourcePlannerOutcome outcome) noexcept {
  using Source = framegraph::MultiSourcePlannerOutcome;
  using Target = perf::CpuReadyMultiSourcePlannerOutcome;
  switch (outcome) {
  case Source::InvalidInput:
    return Target::InvalidInput;
  case Source::SeedRejected:
    return Target::SeedRejected;
  case Source::NoActiveTargetMatch:
    return Target::NoActiveTargetMatch;
  case Source::NoMerge:
    return Target::NoMerge;
  case Source::NaturalAfterMerge:
    return Target::NaturalAfterMerge;
  case Source::PermutationRejected:
    return Target::PermutationRejected;
  case Source::MovedHeadUnproved:
    return Target::MovedHeadUnproved;
  case Source::Planned:
    return Target::Planned;
  }
  return Target::InvalidInput;
}

perf::CpuReadyMultiSourcePlannerMerge plannerPerfMerge(
    framegraph::MultiSourceMergeDiagnostic merge) noexcept {
  using Source = framegraph::MultiSourceMergeDiagnostic;
  using Target = perf::CpuReadyMultiSourcePlannerMerge;
  switch (merge) {
  case Source::None:
    return Target::None;
  case Source::SeedMerged:
    return Target::Seed;
  case Source::NonSeedOnly:
    return Target::NonSeedOnly;
  }
  return Target::None;
}

bool sameResolvedSourceIdentity(
    const core::metalqueue::ResolvedPublishedSource& left,
    const core::metalqueue::ResolvedPublishedSource& right) noexcept {
  return left.source == right.source && left.slotIndex == right.slotIndex &&
      left.seqId == right.seqId && left.metadata == right.metadata &&
      left.semantic == right.semantic && left.sourceId == right.sourceId &&
      left.storage == right.storage && left.slot == right.slot &&
      left.hasPresent == right.hasPresent &&
      left.commandBegin == right.commandBegin &&
      left.commandCount == right.commandCount;
}

bool deferredTerminalSuffixJoinEnabled(
    const render::IRenderBackend* backend) {
  if (!backend || backend->mode() != render::BackendMode::FrameGraph) {
    return false;
  }
  const auto profile = render::resolveRendererCompatProfile(
      std::getenv("DXMT9_RENDERER_COMPAT_PROFILE"));
  const auto features = render::resolveRendererFeatures(
      std::getenv("DXMT9_RENDERER_FEATURES"), profile);
  return profile == render::RendererCompatProfile::Progressive &&
      features.passcoalesce && !features.dce;
}

bool isDeferredTerminalSuffixCandidate(
    const core::SourcePayloadView& payload) noexcept {
  return payload.valid() && payload.commandCount() == 3u &&
      payload.commandAt(0u).kind() == core::MetalCommandKind::DrawRun &&
      payload.commandAt(1u).kind() == core::MetalCommandKind::Clear &&
      payload.commandAt(2u).kind() == core::MetalCommandKind::DrawRun;
}

render::DeferredTerminalSuffixSourceIdentity deferredSuffixIdentity(
    const core::metalqueue::ResolvedPublishedSource& source) noexcept {
  return {
      .source = source.source,
      .rawOrdinal = source.metadata.rawOrdinal,
      .sourceOrdinal = source.metadata.sourceOrdinal,
      .seqId = source.seqId,
  };
}

framegraph::ActiveRenderPlanningSeed deferredSuffixActiveSeed(
    const encoders::ActiveRenderDependencySnapshot& active) noexcept {
  framegraph::ActiveRenderPlanningSeed seed{};
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    seed.targets.color[i] =
        framegraph::TextureHandle{active.colorAttachments[i].value};
    if (active.colorAttachments[i].value != 0) {
      seed.targets.color_count = static_cast<framegraph::u32>(i) + 1u;
    }
  }
  seed.targets.depth =
      framegraph::TextureHandle{active.depthStencil.value};
  seed.targets.sample_count = active.sampleCount;
  seed.dependency_count = active.dependencyCount;
  seed.complete = active.complete;
  const std::size_t count = std::min<std::size_t>(
      active.dependencyCount, seed.write_dependencies.size());
  for (std::size_t i = 0; i < count; ++i) {
    seed.write_dependencies[i] =
        framegraph::ResourceHandle{active.writeDependencies[i].value};
  }
  return seed;
}

core::metalqueue::PostEncodeRetirementIneligibility
postEncodeRetirementIneligibility(
    const core::metalqueue::ResolvedPublishedSource& source,
    std::optional<core::metalqueue::PublishedCommandRef> pendingClear) {
  using Reason =
      core::metalqueue::PostEncodeRetirementIneligibility;
  if (!source.valid()) {
    return Reason::Invalid;
  }
  if (pendingClear.has_value()) {
    if (!pendingClear->valid()) {
      return Reason::RemainingPayloadBorrow;
    }
    if (pendingClear->seqId == source.seqId) {
      return Reason::PendingClear;
    }
  }
  if (source.hasPresent || source.semantic.hasPresent()) {
    return Reason::Present;
  }
  for (std::size_t i = source.commandBegin;
       i < source.commandBegin + source.commandCount; ++i) {
    switch (source.payload.commandAt(i).kind()) {
    case core::MetalCommandKind::DrawRun:
    case core::MetalCommandKind::Clear:
      break;
    case core::MetalCommandKind::Readback:
      return Reason::Readback;
    case core::MetalCommandKind::Present:
      return Reason::Present;
    case core::MetalCommandKind::SurfaceCopy:
    case core::MetalCommandKind::StretchRect:
    case core::MetalCommandKind::ColorFill:
    case core::MetalCommandKind::DepthResolve:
    case core::MetalCommandKind::GenerateMipmaps:
      return Reason::UpdateOrSurfaceOperation;
    }
  }
  return Reason::None;
}

encoders::SessionFinalizeCause sessionFinalizeCauseForRelease(
    core::metalqueue::SessionReleaseReason reason) noexcept {
  using Reason = core::metalqueue::SessionReleaseReason;
  using Cause = encoders::SessionFinalizeCause;
  switch (reason) {
  case Reason::SessionCap:
    return Cause::SessionCap;
  case Reason::IndependentSubmission:
    return Cause::Independent;
  case Reason::InitializerWait:
    return Cause::Initializer;
  case Reason::ProducerSequenceWait:
    return Cause::ProducerWait;
  case Reason::Shutdown:
    return Cause::Drain;
  case Reason::Present:
  case Reason::ExplicitFlush:
  case Reason::DirectObservation:
  case Reason::DeviceLoss:
    return Cause::FailOrOther;
  }
  return Cause::FailOrOther;
}

}  // namespace

void CommandQueue::runCpuReadySessionEncodeLoop(OnSubmittedFn onSubmitted) {
  // Tape-gated session join lane (DXMT9_CPU_READY_TAPE). One pending
  // QueueSubmissionRecord owns the open Metal command buffer plus the retained
  // per-source completion list, an EncodeChunkSession crosses source
  // boundaries, and one submitted tail expands to per-source seqId completion.
  // The lane has two defining policies (see the declaration):
  //   * source-kind-neutral admission: compatible FIFO prefixes accept both
  //     Legacy ChunkSlot and Arena Tape sources, so an Arena source appends
  //     to the pending session instead of forcing a standalone submission;
  //   * ordered-fence release only: a parked pending session never releases
  //     on completion-wait or producer-quiescence observations. It finalizes
  //     on Present tails, non-appendable sources, session caps/preflight
  //     failures, producer sequence waits, initializer-wait boundaries, and
  //     shutdown drain. Admission and writer pressure only wake re-evaluation.
  using core::metalqueue::EncodeSessionSourceList;
  using core::metalqueue::QueueCompletionSource;
  using core::metalqueue::QueueSubmissionRecord;
  using core::metalqueue::ReadySlotSnapshot;
  using core::metalqueue::ResolvedPublishedSource;
  using core::metalqueue::GenerationQualifiedSourceBorrow;
  using core::metalqueue::SynchronousSourceBorrowBatch;
  using core::metalqueue::SynchronousSourcePayloadBorrow;
  using core::metalqueue::SessionReleaseAckResult;
  using core::metalqueue::SessionReleaseAction;
  using core::metalqueue::SessionReleaseCompletion;
  const bool schedulingObservabilityEnabled = perf::enabled();

  // Keep every session encode under a queue-issued generation witness.  The
  // backend may derive call-local payload views from this batch, but no raw
  // lookahead span is allowed to cross the synchronous callback boundary.
  const auto encodeWithBorrows =
      [this](std::unique_lock<std::mutex>& lock,
             std::span<const ResolvedPublishedSource> sources,
             const ResolvedPublishedSource& source,
             encoders::EncodeChunkOptions options,
             bool exposeLookahead)
      -> std::optional<QueueSubmissionRecord> {
    std::array<ResolvedPublishedSource,
               core::metalqueue::kMaxReadyPrefixSources>
        representedSources{};
    std::size_t representedCount = 0;
    for (const auto& candidate : sources) {
      auto represented = std::find_if(
          representedSources.begin(),
          representedSources.begin() + representedCount,
          [&](const ResolvedPublishedSource& existing) {
            return existing.source == candidate.source;
          });
      if (represented != representedSources.begin() + representedCount) {
        if (represented->slotIndex != candidate.slotIndex ||
            represented->seqId != candidate.seqId ||
            represented->metadata != candidate.metadata ||
            represented->semantic != candidate.semantic ||
            represented->payload != candidate.payload ||
            represented->sourceId != candidate.sourceId ||
            represented->storage != candidate.storage ||
            represented->hasPresent != candidate.hasPresent) {
          return std::nullopt;
        }
        continue;
      }
      if (representedCount == representedSources.size()) {
        return std::nullopt;
      }
      representedSources[representedCount++] = candidate;
    }
    if (exposeLookahead) {
      options.sessionLookaheadSources = sources;
    }
    std::optional<QueueSubmissionRecord> result;
    const bool visited = queueLifecycle_.visitRepresentedSourceBorrows(
        lock,
        std::span<const ResolvedPublishedSource>(representedSources.data(),
                                                 representedCount),
        [&](const SynchronousSourceBorrowBatch& sourceBorrows) noexcept {
          if (exposeLookahead) {
            options.sessionLookaheadBorrows = &sourceBorrows;
          }
          ScopedQueueUnlock queueUnlocked(lock);
          try {
            result = encodeCpuReadySessionSource(source, std::move(options));
            return true;
          } catch (...) {
            return false;
          }
        });
    return visited ? std::move(result) : std::nullopt;
  };

  std::array<ReadySlotSnapshot, kCommandChunkCount> scratch{};
  std::optional<QueueSubmissionRecord> pendingRecord;
  const auto fullCaptureBoundary = [this, &pendingRecord]() noexcept {
    return (pendingRecord &&
            (pendingRecord->metalCapture.has_value() ||
             pendingRecord->metalCaptureAlreadyStarted)) ||
        metalCaptureEnabled();
  };
  EncodeSessionSourceList pendingSources;
  encoders::EncodeChunkSession pendingSession;
  render::EncodeSessionAdmissionState pendingAdmission{};
  bool pendingEarlyPrefix = false;
  render::SessionCapacityLeaseState capacityLeaseState{};
  bool exactReplaySingleSource = false;
  bool activeRetainedHeadSingleSourceFallback = false;
  render::FirstLeaseReadyHeadIdentity firstLeaseStandaloneConsumedHead{};
  render::FirstLeaseReadyHeadIdentity firstLeaseCreditRearmObservedHead{};
  render::FirstLeaseReadyHeadIdentity firstLeaseStandalonePendingHead{};
  std::optional<QueueCompletionSource> standaloneSerialSource;
  const bool terminalSuffixJoinEnabled =
      deferredTerminalSuffixJoinEnabled(backend_.get());
  const auto& tapeValues = cpuReadyTape_.config().values();
  const std::uint64_t successorPages =
      render::worstCaseNonWrappingReservationPages(
          tapeValues.maxPagesPerSource);
  const std::uint64_t sessionPages =
      tapeValues.highWaterPages - successorPages;
  const std::uint64_t ordinaryBytes =
      tapeValues.pageSize * tapeValues.maxPagesPerSource;
  const std::uint64_t sessionBytes = tapeValues.pageSize * sessionPages;
  const std::uint64_t maxDrawCredits =
      std::numeric_limits<std::uint32_t>::max();
  // Compatibility publication stops at kMaxQueuedChunks, one below the
  // Tape's Ready FIFO high-water so a Writing control shell remains. Reserve
  // successor headroom against that effective ceiling; otherwise a session
  // can occupy all publishable sources and wait forever for the Ready
  // successor that would trigger its fixed-cap release.
  const std::uint32_t publishedSourceHighWater =
      static_cast<std::uint32_t>(std::min<std::size_t>(
          kMaxQueuedChunks, tapeValues.highWaterReady));
  DXMT_ASSERT(publishedSourceHighWater > 1u);
  const std::uint32_t maxSessionResidentSources =
      publishedSourceHighWater - 1u;
  const std::uint32_t maxSessionWorkSources = static_cast<std::uint32_t>(
      core::metalqueue::kMaxEncodeSessionSources);
  const render::EncodeSessionLimits admissionLimits{
      .maxSources = maxSessionWorkSources,
      .maxPages = static_cast<std::uint32_t>(sessionPages),
      .maxBytes = sessionBytes,
      .maxDraws = std::numeric_limits<std::uint32_t>::max(),
      .maxCommandBuffers = maxSessionWorkSources,
  };
  const render::SessionCapacityPolicy capacityPolicy{
      .highWater = {
          .sources = publishedSourceHighWater,
          .pages = tapeValues.highWaterPages,
          .bytes = tapeValues.pageSize * tapeValues.highWaterPages,
          .draws = maxDrawCredits * 2u,
          .payloadBlocks = tapeValues.sourceSlotCount *
                           core::kMaxArenaSourcePayloadSegments,
          .readyEntries = publishedSourceHighWater,
          .retentionEntries = tapeValues.highWaterSources,
          .allocatorTickets = tapeValues.highWaterSources,
          .commandBuffers =
              static_cast<std::uint64_t>(maxSessionWorkSources) + 1u,
      },
      .maxSession = {
          .sources = maxSessionResidentSources,
          .pages = admissionLimits.maxPages,
          .bytes = admissionLimits.maxBytes,
          .draws = admissionLimits.maxDraws,
          .payloadBlocks =
              static_cast<std::uint64_t>(maxSessionResidentSources) *
                           core::kMaxArenaSourcePayloadSegments,
          .readyEntries = maxSessionResidentSources,
          .retentionEntries = maxSessionResidentSources,
          .allocatorTickets = maxSessionResidentSources,
          .commandBuffers = admissionLimits.maxCommandBuffers,
      },
      .successorHeadroom = {
          .sources = 1,
          .pages = successorPages,
          .bytes = ordinaryBytes,
          .draws = maxDrawCredits,
          .payloadBlocks = core::kMaxArenaSourcePayloadSegments,
          .readyEntries = 1,
          .retentionEntries = 1,
          .allocatorTickets = 1,
          .commandBuffers = 1,
      },
      .ordinaryDirect = {
          .sources = 1,
          .pages = tapeValues.maxPagesPerSource,
          .bytes = ordinaryBytes,
          .draws = maxDrawCredits,
          .payloadBlocks = core::kMaxArenaSourcePayloadSegments,
          .readyEntries = 1,
          .retentionEntries = 1,
          .allocatorTickets = 1,
          .commandBuffers = 1,
      },
  };
  DXMT_ASSERT(capacityPolicy.valid());
  const auto releaseCapacityLease = [&capacityLeaseState]() {
    const std::uint64_t generation =
        capacityLeaseState.lease().generation;
    if (generation == 0) {
      return;
    }
    if (!capacityLeaseState.release(generation)) {
      DXMT_ASSERT(false && "live session capacity lease must release");
      return;
    }
    perf::countCpuReadySessionLeaseReleased();
  };
  const auto recordCapacityLeaseUsed = [&capacityLeaseState]() {
    const auto& stats = capacityLeaseState.stats();
    perf::recordCpuReadySessionLeaseUsed(
        stats.used.sources, stats.used.pages, stats.used.bytes,
        stats.used.draws, stats.slack.sources, stats.slack.pages,
        stats.slack.bytes, stats.slack.draws);
  };
  const render::SessionAdmissionKey admissionKey{
      .queueLifetimeEpoch = 1,
      .allocatorPolicyEpoch = 1,
      .lane = render::EncodeQueueLane::CpuReadySerial,
      .captureMode = render::EncodeCaptureMode::Disabled,
      .completionMode = queue_
          ? render::EncodeCompletionMode::Metal
          : render::EncodeCompletionMode::TestNullCommandBuffer,
  };

  const auto admissionCandidateFor =
      [&admissionKey, pageSize = tapeValues.pageSize](
          const ResolvedPublishedSource& source) noexcept {
        // Arena usedBytes names the exact constructed Tape extent. Legacy
        // usedBytes names its logical replay extent, so the session Tape-byte
        // charge is the compatibility source's reserved Tape page. ChunkSlot
        // heap bytes remain outside this axis and are bounded indirectly by
        // the compatibility source/slot limits.
        const auto residencyBytes = render::sessionTapeByteCharge(
            source.payload.isArena(), source.metadata.usedBytes,
            source.metadata.pageCount, pageSize);
        return render::SessionAdmissionCandidate{
            .key = admissionKey,
            .semantic = source.semantic,
            .residencyBytes = residencyBytes,
            .rawOrdinal = source.metadata.rawOrdinal,
            .sourceOrdinal = source.metadata.sourceOrdinal,
            .seqId = source.seqId,
            .predictedCommandBuffers = 1,
            .reservationPages = static_cast<std::uint32_t>(
                source.metadata.pageCount +
                source.metadata.paddingPagesBefore),
            .payloadBlocks = static_cast<std::uint32_t>(
                std::max<std::size_t>(1, source.payload.arenaSegmentCount())),
            .retentionEntries = 1,
            .allocatorTickets = 1,
        };
      };

  auto completeInlineSnapshot = [this, &onSubmitted](
                                    std::unique_lock<std::mutex>& lock,
                                    const ReadySlotSnapshot& source) {
    const std::uint64_t seqId = source.seqId;
    if (!queueLifecycle_.completeInlineChunk(lock, source.slotIndex, seqId)) {
      return;
    }
    if (onSubmitted) {
      onSubmitted(seqId);
    }
  };

  auto retainSource = [this](std::unique_lock<std::mutex>& lock,
                             const ReadySlotSnapshot& source,
                             QueueCompletionSource& retained) {
    std::array<QueueCompletionSource, 1> out{};
    const std::span<const ReadySlotSnapshot> sources(&source, 1u);
    const std::size_t count =
        queueLifecycle_.retainEncodedSourcesForPendingTail(
            lock, sources, std::span<QueueCompletionSource>(out));
    if (count != 1u) {
      return false;
    }
    retained = out.front();
    return true;
  };

  auto submitRecordLocked = [this](std::unique_lock<std::mutex>& lock,
                                   QueueSubmissionRecord& record) {
    if (record.testOnlyAllowNullCommandBuffer) {
      record.commandBuffer = {};
    }
    if (!queueLifecycle_.submitEncodedSubmission(lock, record)) {
      return false;
    }
    auto callbacks = std::move(record.postCommitCallbacks);
    lock.unlock();
    for (auto& callback : callbacks) {
      if (callback) {
        callback();
      }
    }
    return true;
  };

  auto finalizePendingSessionForSubmitLocked =
      [this, &pendingRecord, &pendingSession](
          std::unique_lock<std::mutex>& lock,
          encoders::SessionFinalizeCause cause) {
        if (!pendingSession) {
          return true;
        }
        if (!encoders::encodeChunkSessionHasDeferredSubmissionPayload(
                *pendingSession) &&
            encoders::encodeChunkSessionSources(*pendingSession).empty()) {
          pendingSession.reset();
          return true;
        }
        if (!pendingRecord.has_value()) {
          return false;
        }

        lock.unlock();
        auto ctx = makeEncodeContext();
        const bool finalized =
            encoders::finalizeEncodeChunkSessionIntoSubmission(
                ctx, *pendingSession, *pendingRecord, cause);
        lock.lock();
        if (finalized) {
          if (!encoders::retainEncodeChunkSessionUntilSubmissionComplete(
                  std::move(pendingSession), *pendingRecord)) {
            return false;
          }
        }
        return finalized;
      };

  auto submitPendingRecordLocked =
      [&pendingRecord, &pendingSources, &pendingSession, &pendingAdmission,
       &pendingEarlyPrefix,
       &releaseCapacityLease,
       &finalizePendingSessionForSubmitLocked, &submitRecordLocked](
          std::unique_lock<std::mutex>& lock,
          encoders::SessionFinalizeCause cause) {
        if (!pendingRecord.has_value()) {
          return false;
        }
        if (!finalizePendingSessionForSubmitLocked(lock, cause)) {
          return false;
        }
        if (pendingRecord->explicitCompletionSourceSpan().empty() &&
            !pendingSources.empty() &&
            !pendingRecord->assignFixedCompletionSources(
                pendingSources.span())) {
          return false;
        }
        if (!submitRecordLocked(lock, *pendingRecord)) {
          return false;
        }
        pendingRecord.reset();
        pendingSources.clear();
        pendingSession.reset();
        pendingAdmission = {};
        pendingEarlyPrefix = false;
        releaseCapacityLease();
        return true;
      };

  auto abandonEarlyPrefixPreEffectLocked =
      [this, &pendingRecord, &pendingSources, &pendingSession,
       &pendingAdmission, &pendingEarlyPrefix,
       &releaseCapacityLease]() noexcept {
        DXMT_ASSERT(pendingEarlyPrefix);
        perf::countCpuReadyEarlyPrefixSessionJoinFailedPreEffect();
        // Destroying the unsubmitted carrier/session is the rollback edge.
        // The retained source identities intentionally remain terminally
        // owned by the stopped queue; they must never be reported complete
        // without GPU execution.
        pendingRecord.reset();
        pendingSources.clear();
        pendingSession.reset();
        pendingAdmission = {};
        pendingEarlyPrefix = false;
        releaseCapacityLease();
        requestSchedulingStopLocked();
      };

  auto processNextReleaseLocked =
      [this, &pendingRecord, &pendingSession,
       &submitPendingRecordLocked](std::unique_lock<std::mutex>& lock) {
        const auto release = sessionReleaseState_.peekNext();
        if (!release || !core::metalqueue::sessionReleaseActionReady(
                            *release, sessionReleaseCoveredRawOrdinal_,
                            sessionReleaseCoveredSeqId_)) {
          return false;
        }

        SessionReleaseCompletion completion =
            SessionReleaseCompletion::SessionSubmitted;
        if (release->event.action == SessionReleaseAction::ClosePass) {
          completion = SessionReleaseCompletion::PassClosed;
          if (pendingSession && pendingRecord) {
            lock.unlock();
            auto ctx = makeEncodeContext();
            const auto closed =
                encoders::closeEncodeChunkSessionRenderPass(
                    ctx, *pendingSession, *pendingRecord);
            lock.lock();
            if (closed == encoders::EncodeChunkSessionPassCloseResult::
                              InvalidCommandBufferCarrier) {
              abortCpuReadySessionFailOpen(
                  "ordered ClosePass command-buffer carrier");
            }
          }
        } else if (pendingRecord &&
                   !submitPendingRecordLocked(
                       lock, sessionFinalizeCauseForRelease(
                                 release->event.reason))) {
          abortCpuReadySessionFailOpen("ordered session release submit");
        }
        if (!lock.owns_lock()) {
          lock.lock();
        }

        // The posting thread reached the raw control before publishing the
        // event. Once its fixed sequence prefix is represented and the action
        // above has executed, that ordered-control identity covers the raw
        // fence even when the control itself does not become a Tape source.
        sessionReleaseCoveredRawOrdinal_ = std::max(
            sessionReleaseCoveredRawOrdinal_,
            release->event.fenceRawOrdinal);
        const auto acknowledged = sessionReleaseState_.acknowledge(
            *release, completion, sessionReleaseCoveredRawOrdinal_,
            sessionReleaseCoveredSeqId_);
        if (acknowledged != SessionReleaseAckResult::Acknowledged) {
          abortCpuReadySessionFailOpen("ordered release acknowledgement");
        }
        switch (release->event.reason) {
        case core::metalqueue::SessionReleaseReason::ProducerSequenceWait:
          perf::countCpuReadySessionReleased(
              perf::CpuReadySessionReleaseReason::ProducerWait);
          break;
        default:
          break;
        }
        sessionReleaseCv_.notify_all();
        return true;
      };

  // A producer sequence wait is an ordered D3D-visible progress obligation.
  // Admission and raw-writer pressure only wake this loop; fixed lease/cap
  // policy, never live occupancy, selects the predecessor submission fence.
  auto pendingOrderedReleaseReason =
      [this, &pendingRecord]()
      -> std::optional<perf::CpuReadySessionReleaseReason> {
    if (!pendingRecord.has_value()) {
      return std::nullopt;
    }
    if (queueLifecycle_.producerSequenceWaitActive()) {
      return perf::CpuReadySessionReleaseReason::ProducerWait;
    }
    return std::nullopt;
  };

  while (true) {
    std::unique_lock lock(mutex_);

    const auto earlyWakeAction =
        core::metalqueue::decideCpuReadyEarlySessionAction(
            pendingEarlyPrefix, stop_,
            sessionReleaseState_.hasPending() ||
                queueLifecycle_.producerSequenceWaitActive(),
            !cpuReadyTape_.readyEmpty(), /*nextAppendable=*/true,
            /*nextHasFinalPresentTail=*/false);
    if (earlyWakeAction ==
        core::metalqueue::CpuReadyEarlySessionAction::FailPreEffect) {
      abandonEarlyPrefixPreEffectLocked();
      return;
    }

    if (processNextReleaseLocked(lock)) {
      if (testOnlyPauseAfterNextSessionReleaseAck_) {
        testOnlyPauseAfterNextSessionReleaseAck_ = false;
        testOnlyPausedAfterSessionReleaseAck_ = true;
        sessionReleaseCv_.notify_all();
        sessionReleaseCv_.wait(lock, [this] {
          return stop_ || !testOnlyPausedAfterSessionReleaseAck_;
        });
      }
      if (stop_) {
        return;
      }
      continue;
    }

    if (const auto reason = pendingOrderedReleaseReason()) {
      const auto posted = sessionReleaseState_.tryPostOrdered(
          core::metalqueue::SessionReleaseReason::ProducerSequenceWait,
          SessionReleaseAction::SubmitSession,
          pendingAdmission.lastRawOrdinal,
          pendingAdmission.lastSeqId);
      if (posted.accepted()) {
        encodeCv_.notify_one();
        continue;
      }
      // A younger semantic event can already own the transport. Submitting the
      // covered predecessor remains an ordered producer-wait action.
      if (!submitPendingRecordLocked(
              lock, encoders::SessionFinalizeCause::ProducerWait)) {
          abortCpuReadySessionFailOpen("ordered producer-wait release");
      }
      perf::countCpuReadySessionReleased(*reason);
      if (stop_) {
        return;
      }
      continue;
    }

    if (pendingRecord.has_value() && cpuReadyTape_.readyEmpty()) {
      if (stop_) {
        if (pendingEarlyPrefix) {
          abandonEarlyPrefixPreEffectLocked();
          return;
        }
        if (!submitPendingRecordLocked(
                lock, encoders::SessionFinalizeCause::Drain)) {
          abortCpuReadySessionFailOpen("shutdown drain release");
        }
        perf::countCpuReadySessionReleased(
            perf::CpuReadySessionReleaseReason::Drain);
        return;
      }
      // Park with the session open. Producer quiescence and completion waits
      // are deliberately NOT wake-to-release conditions; only new ready work,
      // shutdown, or an ordered semantic/fixed-cap fence re-evaluates it.
      encodeCv_.wait(lock, [this] {
        return render::openSessionWaitDone({
            .stopped = stop_,
            .ready = !cpuReadyTape_.readyEmpty(),
            .orderedRelease = sessionReleaseState_.hasPending(),
            .producerSequenceWait =
                queueLifecycle_.producerSequenceWaitActive(),
        });
      });
      continue;
    }

    if (!pendingRecord.has_value() && cpuReadyTape_.readyEmpty()) {
      // An idle coordinator still owns release acknowledgement. In
      // particular, final WSI teardown can post a terminal fence after the
      // last source has already submitted and reclaimed. The generic Ready
      // dequeue wait observes only Ready/stop, so waiting there would lose
      // this wake and leave the posting thread blocked forever. Wake only for
      // a covered release: an uncovered fence still needs its Ready source to
      // publish before the coordinator may acknowledge it.
      if (testOnlySchedulingWaitObservationEnabled_) {
        ++testOnlyIdleSessionWaitEntries_;
        sessionReleaseCv_.notify_all();
      }
      encodeCv_.wait(lock, [this] {
        const auto release = sessionReleaseState_.peekNext();
        const bool coveredRelease = release &&
            core::metalqueue::sessionReleaseActionReady(
                *release, sessionReleaseCoveredRawOrdinal_,
                sessionReleaseCoveredSeqId_);
        return stop_ || !cpuReadyTape_.readyEmpty() || coveredRelease;
      });
      if (stop_) {
        return;
      }
      continue;
    }

    // A one-source Ready frontier cannot expose an A|B + A replay window to
    // the bounded planner. When that exact frontier has one already-reserved
    // ordered-tail Writing successor, retain the whole Ready source
    // tentatively until the successor publishes. The original fresh-frontier
    // form prevents startup fragmentation. An active session may now use the
    // same physical hold only when its render frontier is complete: unlike the
    // retired seedless R15 observer, the current planner receives the active
    // render seed and can prove a cross-window return before any effect.
    const bool freshRetainedHeadFrontier =
        !pendingRecord.has_value() && !pendingSession &&
        !pendingAdmission.valid() && !capacityLeaseState.lease().valid();
    const bool activeRetainedHeadFrontier =
        pendingRecord.has_value() && pendingSession &&
        pendingAdmission.valid() && capacityLeaseState.lease().valid() &&
        encoders::encodeChunkSessionReplayFrontierState(*pendingSession) ==
            encoders::EncodeSessionReplayFrontierState::ActiveRenderComplete;
    const bool retainedHeadFrontier =
        freshRetainedHeadFrontier || activeRetainedHeadFrontier;
    if (!exactReplaySingleSource &&
        !activeRetainedHeadSingleSourceFallback &&
        !stop_ &&
        retainedHeadFrontier &&
        cpuReadyTape_.readyCount() == 1u &&
        !sessionReleaseState_.hasPending() &&
        !queueLifecycle_.producerSequenceWaitActive() &&
        arenaAdmissionWaiterCount_.load(std::memory_order_acquire) == 0u &&
        !queueLifecycle_.producerWriterPressureActive() &&
        !(initializer_ && initializer_->hasPendingUploadsUnlocked())) {
      const auto retainedHeadKind = activeRetainedHeadFrontier
          ? perf::CpuReadyRetainedHeadFrontier::ActiveRender
          : perf::CpuReadyRetainedHeadFrontier::Fresh;
      perf::countCpuReadyRetainedHeadAttempt(retainedHeadKind);
      core::CpuReadyTape::SourceRef writingSuccessor{};
      std::optional<core::metalqueue::CpuReadyNextSourceIntent>
          nextSourceIntent;
      auto retainedHeadRejection =
          perf::CpuReadyRetainedHeadRejectReason::ReservationRace;
      const std::size_t retainedCount =
          queueLifecycle_.reserveReadySlotBatchPrefix(
              lock, std::span<ReadySlotSnapshot>(scratch.data(), 1u),
              [&](const SynchronousSourceBorrowBatch& candidates) noexcept {
                if (candidates.size() != 1u) {
                  retainedHeadRejection =
                      perf::CpuReadyRetainedHeadRejectReason::BorrowShape;
                  return std::size_t{0};
                }
                std::size_t selected = 0;
                const bool valid = candidates.visitAt(
                    0u, [&](const GenerationQualifiedSourceBorrow& borrow) noexcept {
                  return borrow.visitPayload(
                      [&](const SynchronousSourcePayloadBorrow& payloadBorrow)
                          noexcept {
                    const auto payload = payloadBorrow.checkedView();
                    if (!payload.valid()) {
                      retainedHeadRejection = perf::
                          CpuReadyRetainedHeadRejectReason::PayloadInvalid;
                      return false;
                    }
                    if (terminalSuffixJoinEnabled &&
                        isDeferredTerminalSuffixCandidate(payload)) {
                      retainedHeadRejection = perf::
                          CpuReadyRetainedHeadRejectReason::
                              TerminalSuffixOwned;
                      return false;
                    }
                    if (const auto* legacy = payload.legacyPayload();
                        legacy && legacy->publishReason ==
                            perf::ChunkPublishReason::EarlyPrefix) {
                      // The experiment exists to encode and park this source
                      // before its reserved Writing successor publishes.
                      // Retaining the head would erase the intended overlap.
                      retainedHeadRejection = perf::
                          CpuReadyRetainedHeadRejectReason::SourceCompatibility;
                      return false;
                    }
                    ResolvedPublishedSource candidate{
                        .source = borrow.source(),
                        .slotIndex = borrow.slotIndex(),
                        .seqId = borrow.seqId(),
                        .metadata = borrow.metadata(),
                        .semantic = borrow.semantic(),
                        .payload = payload,
                    };
                    const auto admission = admissionCandidateFor(candidate);
                    const bool sourceCompatible = activeRetainedHeadFrontier
                        ? render::sessionSourceCanAppendToPending(payload, true)
                        : render::sessionSourceCanBeHead(payload);
                    const auto admissionState = activeRetainedHeadFrontier
                        ? pendingAdmission
                        : render::EncodeSessionAdmissionState{};
                    if (borrow.hasPresent()) {
                      retainedHeadRejection =
                          perf::CpuReadyRetainedHeadRejectReason::Present;
                      return false;
                    }
                    if (!sourceCompatible) {
                      retainedHeadRejection = perf::
                          CpuReadyRetainedHeadRejectReason::
                              SourceCompatibility;
                      return false;
                    }
                    if (render::classifySessionAdmission(
                            admissionState, admission, admissionLimits) !=
                        render::SessionAdmissionDecision::Admit) {
                      retainedHeadRejection =
                          perf::CpuReadyRetainedHeadRejectReason::Admission;
                      return false;
                    }
                    const auto capacity =
                        cpuReadyTape_.leaseAcquisitionCapacitySnapshot();
                    if (!capacity.valid) {
                      retainedHeadRejection = perf::
                          CpuReadyRetainedHeadRejectReason::CapacitySnapshot;
                      return false;
                    }
                    if (capacity.orderedTailWritingSuccessor.has_value()) {
                      if (!capacity.orderedTailWritingSuccessor->valid()) {
                        retainedHeadRejection = perf::
                            CpuReadyRetainedHeadRejectReason::
                                WritingSuccessorInvalid;
                        return false;
                      }
                      writingSuccessor =
                          capacity.orderedTailWritingSuccessor->source;
                    } else if (core::metalqueue::
                                   cpuReadyNextSourceIntentMatches(
                                       nextSourceIntent_,
                                       candidate.metadata.sourceOrdinal,
                                       candidate.seqId)) {
                      nextSourceIntent = nextSourceIntent_;
                    } else {
                      retainedHeadRejection = perf::
                          CpuReadyRetainedHeadRejectReason::
                              WritingSuccessorMissing;
                      return false;
                    }
                    selected = 1u;
                    return true;
                  });
                });
                if (!valid || selected == 0u) {
                  // The terminal-suffix lane represents and encodes only the
                  // current A prefix while the exact successor is Writing.
                  // Whole-head retention would hide that effect boundary.
                  return std::size_t{0};
                }
                return std::size_t{1};
              });
      if (retainedCount == 1u) {
        perf::countCpuReadyRetainedHeadHeld(retainedHeadKind);
        if (nextSourceIntent.has_value()) {
          perf::countCpuReadyRetainedHeadIntentSelected();
        }
        const auto waitStarted = perf::enabled()
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        std::uint64_t observedCapacityProgress =
            queueLifecycle_.cpuReadyCapacityProgressGeneration();
        std::optional<core::CpuReadyTape::ReadyEntry> readySuccessor;

        auto successorReady = [&]() {
          std::array<core::CpuReadyTape::ReadyEntry, 1> ready{};
          const bool hasReady = cpuReadyTape_.copyReadyPrefix(ready) == 1u;
          const bool exactReady = hasReady &&
              (nextSourceIntent.has_value()
                   ? ready.front().metadata.sourceOrdinal ==
                             nextSourceIntent->sourceOrdinal &&
                         ready.front().seqId == nextSourceIntent->seqId
                   : ready.front().source == writingSuccessor);
          if (exactReady) {
            readySuccessor = ready.front();
            return true;
          }
          readySuccessor.reset();
          return false;
        };
        auto writingSuccessorStillExact = [&]() {
          if (nextSourceIntent.has_value()) {
            return nextSourceIntent_ == *nextSourceIntent;
          }
          const auto capacity =
              cpuReadyTape_.leaseAcquisitionCapacitySnapshot();
          return capacity.valid &&
              capacity.orderedTailWritingSuccessor.has_value() &&
              capacity.orderedTailWritingSuccessor->valid() &&
              capacity.orderedTailWritingSuccessor->source ==
                  writingSuccessor;
        };

        while (true) {
          encodeCv_.wait(lock, [&] {
            return render::retainedOrDeferredSessionWaitDone({
                .stopped = stop_,
                .ready = !cpuReadyTape_.readyEmpty(),
                .orderedRelease = sessionReleaseState_.hasPending(),
                .producerSequenceWait =
                    queueLifecycle_.producerSequenceWaitActive(),
                .admissionPressure = arenaAdmissionWaiterCount_.load(
                    std::memory_order_acquire) != 0u,
                .writerPressure =
                    queueLifecycle_.producerWriterPressureActive(),
                .initializerPending = initializer_ &&
                    initializer_->hasPendingUploadsUnlocked(),
                .capacityProgress =
                    queueLifecycle_.cpuReadyCapacityProgressGeneration() !=
                        observedCapacityProgress,
                .sourceIntentProgress = nextSourceIntent.has_value() &&
                    nextSourceIntent_ != *nextSourceIntent,
            });
          });

          const bool releasePending = sessionReleaseState_.hasPending();
          const bool producerWait =
              queueLifecycle_.producerSequenceWaitActive();
          const bool initializerPending =
              initializer_ && initializer_->hasPendingUploadsUnlocked();
          const bool pressure =
              arenaAdmissionWaiterCount_.load(std::memory_order_acquire) !=
                  0u ||
              queueLifecycle_.producerWriterPressureActive();
          const bool ready = successorReady();
          const bool writerExact = ready || writingSuccessorStillExact();
          if (stop_ || releasePending || producerWait ||
              initializerPending || pressure || ready || !writerExact) {
            std::optional<perf::CpuReadyRetainedHeadFallbackReason>
                fallback;
            if (stop_) {
              fallback = perf::CpuReadyRetainedHeadFallbackReason::Stop;
            } else if (releasePending) {
              fallback = perf::CpuReadyRetainedHeadFallbackReason::Release;
            } else if (producerWait) {
              fallback =
                  perf::CpuReadyRetainedHeadFallbackReason::ProducerWait;
            } else if (initializerPending) {
              fallback =
                  perf::CpuReadyRetainedHeadFallbackReason::Initializer;
            } else if (!ready && pressure) {
              fallback = perf::CpuReadyRetainedHeadFallbackReason::Pressure;
            } else if (!ready) {
              fallback =
                  perf::CpuReadyRetainedHeadFallbackReason::WriterGone;
            }

            const std::uint64_t waitedNanoseconds = perf::enabled()
                ? static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - waitStarted)
                          .count())
                : 0u;
            perf::recordCpuReadyRetainedHeadWait(waitedNanoseconds);
            if (!queueLifecycle_.restoreReservedReadySlotBatch(
                    lock, std::span<const ReadySlotSnapshot>(scratch.data(),
                                                             retainedCount))) {
              perf::countCpuReadyRetainedHeadRestoreFailure();
              queueLifecycle_.poisonTapeFailureLocked();
              return;
            }
            if (fallback.has_value()) {
              perf::countCpuReadyRetainedHeadFallback(*fallback);
              exactReplaySingleSource = !activeRetainedHeadFrontier;
              activeRetainedHeadSingleSourceFallback =
                  activeRetainedHeadFrontier;
            } else {
              DXMT_ASSERT(readySuccessor.has_value());
              perf::countCpuReadyRetainedHeadSuccessorReady(
                  retainedHeadKind);
              if (nextSourceIntent.has_value()) {
                perf::countCpuReadyRetainedHeadIntentSuccessorReady();
              }
              exactReplaySingleSource = false;
              activeRetainedHeadSingleSourceFallback = false;
            }
            break;
          }
          observedCapacityProgress =
              queueLifecycle_.cpuReadyCapacityProgressGeneration();
        }
        continue;
      }
      perf::countCpuReadyRetainedHeadRejected(retainedHeadRejection);
    }

    bool blockedByPendingCompatibility = false;
    bool leaseDenied = false;
    std::uint64_t leaseDeniedSeqId = 0;
    QueueCompletionSource leaseDeniedReadySource{};
    render::FirstLeaseReadyHeadEligibility leaseDeniedReadyHeadEligibility =
        render::FirstLeaseReadyHeadEligibility::NonArena;
    std::uint64_t leaseDeniedReadyHeadSourceOrdinal = 0;
    bool invalidCapacitySnapshot = false;
    bool invalidStandaloneSerialSource = false;
    bool selectionAcquiredLease = false;
    std::uint64_t selectionLeaseGeneration = 0;
    std::array<render::SessionCapacityVector, kCommandChunkCount>
        selectedCapacityCharges{};
    std::array<bool, kCommandChunkCount> selectedCapacityCharged{};
    render::SessionCapacityDimension blockedCapDimension =
        render::SessionCapacityDimension::None;
    render::SessionCapacityRejectionObservation blockedCapRequirement{};
    core::metalqueue::SessionReleaseReason blockedReleaseReason =
        core::metalqueue::SessionReleaseReason::IndependentSubmission;
    const auto releaseFence = sessionReleaseState_.peekNext();
    const std::size_t count =
        queueLifecycle_.reserveReadySlotBatchPrefix(
            lock, std::span<ReadySlotSnapshot>(scratch),
            [&, releaseFence](
                const SynchronousSourceBorrowBatch& candidates) noexcept {
              render::EncodeSessionAdmissionState planned = pendingAdmission;
              std::size_t selected = 0;
              bool selectionInvalid = false;
              candidates.visit([&](const GenerationQualifiedSourceBorrow& borrow,
                                   std::size_t) noexcept {
                ResolvedPublishedSource candidate{};
                if (!borrow.visitPayload(
                        [&](const SynchronousSourcePayloadBorrow& payloadBorrow)
                            noexcept {
                          const auto payload = payloadBorrow.checkedView();
                          candidate = ResolvedPublishedSource{
                              .source = borrow.source(),
                              .slotIndex = borrow.slotIndex(),
                              .seqId = borrow.seqId(),
                              .metadata = borrow.metadata(),
                              .semantic = borrow.semantic(),
                              .payload = payload,
                              .sourceId = borrow.source().id,
                              .storage = borrow.source().storage,
                              .hasPresent = borrow.hasPresent(),
                              .commandBegin = borrow.commandBegin(),
                              .commandCount = borrow.commandCount(),
                          };
                        })) {
                  selectionInvalid = true;
                  return false;
                }
                if (releaseFence &&
                    ((releaseFence->event.fenceSeqId != 0 &&
                      candidate.seqId > releaseFence->event.fenceSeqId) ||
                     (releaseFence->event.fenceRawOrdinal != 0 &&
                      candidate.metadata.rawOrdinal != 0 &&
                      candidate.metadata.rawOrdinal >
                          releaseFence->event.fenceRawOrdinal))) {
                  return false;
                }
                if (exactReplaySingleSource) {
                  if (standaloneSerialSource.has_value() &&
                      (candidate.source != standaloneSerialSource->source ||
                       candidate.slotIndex != standaloneSerialSource->slotIndex ||
                       candidate.seqId != standaloneSerialSource->seqId)) {
                    invalidStandaloneSerialSource = true;
                    selectionInvalid = true;
                    return false;
                  }
                  selected = 1u;
                  return false;
                }
                if (selected >= 2u && candidate.semantic.hasPresent()) {
                  // Present is a hard replay-window/logical-pass boundary,
                  // but a final Present tail may still close and submit the
                  // same EncodeSession/CB. Leave it Ready for the next natural
                  // iteration without invalidating the complete pre-Present
                  // planning prefix.
                  return false;
                }

                const auto admission = admissionCandidateFor(candidate);
                const auto result = render::classifySessionAdmissionDetailed(
                    planned, admission, admissionLimits);
                const auto decision = result.decision;
                const bool hasPlannedSession = planned.valid();
                const bool exactCompatible = hasPlannedSession
                    ? render::sessionSourceCanAppendToPending(
                          candidate.payload, true)
                    : render::sessionSourceCanBeHead(
                          candidate.payload);
                const bool initializerBoundary =
                    hasPlannedSession && pendingSession &&
                    encoders::encodeChunkSessionHasActiveRender(
                        *pendingSession) &&
                    initializer_ &&
                    initializer_->hasPendingUploadsUnlocked();

                if (decision != render::SessionAdmissionDecision::Admit ||
                    !exactCompatible || initializerBoundary) {
                  if (selected == 0 && pendingRecord.has_value()) {
                    blockedByPendingCompatibility = true;
                    blockedReleaseReason = initializerBoundary
                        ? core::metalqueue::SessionReleaseReason::
                              InitializerWait
                        : decision == render::SessionAdmissionDecision::
                                         SubmitPrefixBeforeCandidate &&
                                  result.limitingDimension !=
                                      render::SessionCapacityDimension::None
                            ? core::metalqueue::SessionReleaseReason::SessionCap
                            : core::metalqueue::SessionReleaseReason::
                                  IndependentSubmission;
                    if (blockedReleaseReason ==
                        core::metalqueue::SessionReleaseReason::SessionCap) {
                      blockedCapDimension = result.limitingDimension;
                      blockedCapRequirement = result.capacityRejection;
                    }
                  }
                  if (selected == 0 && !pendingRecord.has_value()) {
                    // Invalid/isolated heads still execute through the exact
                    // serial encoder; they simply cannot start a joined
                    // session based on this preflight.
                    selected = 1u;
                    return false;
                  }
                  return false;
                }
                if (!render::appendSessionAdmission(
                        planned, admission, admissionLimits)) {
                  if (selected == 0 && pendingRecord.has_value()) {
                    blockedByPendingCompatibility = true;
                    blockedReleaseReason =
                        core::metalqueue::SessionReleaseReason::SessionCap;
                  }
                  return false;
                }
                const auto capacity = render::sessionCapacityFor(admission);
                if (!capacityLeaseState.lease().valid()) {
                  const auto tapeCapacity =
                      cpuReadyTape_.leaseAcquisitionCapacitySnapshot();
                  render::SessionCapacityLeaseAcquisitionSnapshot snapshot{
                      .olderUnavailable = {
                          .sources = tapeCapacity.olderUnavailable.sources,
                          .pages = tapeCapacity.olderUnavailable.pages,
                          .bytes = tapeCapacity.olderUnavailable.bytes,
                          .payloadBlocks =
                              tapeCapacity.olderUnavailable.payloadBlocks,
                          .readyEntries =
                              tapeCapacity.olderUnavailable.readyEntries,
                          .retentionEntries =
                              tapeCapacity.olderUnavailable.retentionEntries,
                          .allocatorTickets =
                              tapeCapacity.olderUnavailable.allocatorTickets,
                      },
                      .valid = tapeCapacity.valid,
                  };
                  if (tapeCapacity.orderedTailWritingSuccessor.has_value()) {
                    const auto& writing =
                        tapeCapacity.orderedTailWritingSuccessor->claim;
                    snapshot.orderedTailWritingSuccessor = {
                        .sources = writing.sources,
                        .pages = writing.pages,
                        .bytes = writing.bytes,
                        .payloadBlocks = writing.payloadBlocks,
                        .readyEntries = writing.readyEntries,
                        .retentionEntries = writing.retentionEntries,
                        .allocatorTickets = writing.allocatorTickets,
                    };
                  }
                  const auto unavailable =
                      render::sessionCapacityUnavailableForFirstLease(
                          snapshot, capacityPolicy.successorHeadroom);
                  if (!snapshot.valid || !unavailable.has_value()) {
                    invalidCapacitySnapshot = true;
                    return false;
                  }
                  if (!capacityLeaseState.acquire(
                          capacityPolicy, *unavailable, capacity)) {
                    perf::countCpuReadySessionLeaseDenied();
                    leaseDenied = true;
                    leaseDeniedSeqId = candidate.seqId;
                    leaseDeniedReadySource = {
                        .source = candidate.source,
                        .slotIndex = candidate.slotIndex,
                        .seqId = candidate.seqId,
                        .hasPresent = candidate.hasPresent,
                        .commandBegin = candidate.commandBegin,
                        .commandCount = candidate.commandCount,
                    };
                    leaseDeniedReadyHeadSourceOrdinal =
                        candidate.metadata.sourceOrdinal;
                    const auto readyHeadCapacity =
                        render::firstLeaseReadyHeadCapacityViewFor(admission);
                    leaseDeniedReadyHeadEligibility =
                        render::classifyFirstLeaseReadyHeadEligibility({
                            .arena = candidate.payload.isArena(),
                            .present = candidate.hasPresent,
                            .fitsOrdinaryCapacity =
                                render::sessionCapacityFitsWithin(
                                    readyHeadCapacity.ordinaryShape,
                                    capacityPolicy.ordinaryDirect),
                            .fitsHighWater =
                                render::sessionCapacityFitsWithin(
                                    readyHeadCapacity.fullReservation,
                                    capacityPolicy.highWater),
                        });
                    if (schedulingObservabilityEnabled) {
                      perf::recordCpuReadyFirstLeaseEligibility(
                          leaseDeniedReadyHeadEligibility);
                    }
                    return false;
                  }
                  selectionAcquiredLease = true;
                  selectionLeaseGeneration =
                      capacityLeaseState.lease().generation;
                  const auto& lease = capacityLeaseState.lease();
                  perf::countCpuReadySessionLeaseAcquired(
                      lease.reserved.sources, lease.reserved.pages,
                      lease.reserved.bytes, lease.reserved.draws,
                      capacityPolicy.successorHeadroom.pages);
                } else {
                  selectionLeaseGeneration =
                      capacityLeaseState.lease().generation;
                  if (!capacityLeaseState.charge(selectionLeaseGeneration,
                                                 capacity)) {
                    abortCpuReadySessionFailOpen(
                        "capacity lease pre-representation charge");
                  }
                }
                selectedCapacityCharges[selected] = capacity;
                selectedCapacityCharged[selected] = true;
                ++selected;
                if (activeRetainedHeadSingleSourceFallback) {
                  // The active hold already owns a live lease. Its fallback
                  // must still pass through admission/charge before selecting
                  // exactly the restored older source; the fresh/pressure
                  // exact-replay shortcut intentionally bypasses that path.
                  return false;
                }
                if (planned.valid() && selected >=
                        framegraph::kMaxMultiSourcePlanningSources) {
                  // Keep the represented transaction within the bounded
                  // planner's complete proof window. A ninth compatible
                  // source remains Ready and is reconsidered after this
                  // planned prefix installs its carried state.
                  return false;
                }
                if (candidate.semantic.hasPresent()) {
                  return false;
                }
                return true;
              });
              return selectionInvalid ? std::size_t{0} : selected;
            });
    if (count == 0) {
      if (invalidCapacitySnapshot) {
        // Invalid Tape identity/arithmetic is structural corruption, not a
        // capacity transition. Poison synchronously so it cannot enter the
        // generation wait with an unrepairable predicate.
        queueLifecycle_.poisonTapeFailureLocked();
        return;
      }
      if (invalidStandaloneSerialSource) {
        // The standalone escape requires the exact Ready identity observed at
        // denial. Identity drift is structural corruption, not another
        // capacity transition or permission to select a different head.
        queueLifecycle_.poisonTapeFailureLocked();
        return;
      }
      if (leaseDenied && !pendingRecord.has_value()) {
        schedulingProgressWatchdog_.noteLeaseWait(leaseDeniedSeqId);
        // Older submitted/represented/retiring residency or a valid Writing
        // claim beyond successor headroom may delay lease acquisition. One
        // eligible ordered-tail Writing successor is already owned by the
        // fixed successor headroom and never enters this wait. Invalid Tape
        // identity or arithmetic has already poisoned and returned above.
        // Wait for an explicit capacity-releasing transition, not an
        // unqualified notification: a wake from unrelated queue activity is
        // not proof that the denied lease predicate changed.
        if (stop_) {
          return;
        }
        const std::uint64_t observedCapacityProgress =
            queueLifecycle_.cpuReadyCapacityProgressGeneration();
        const render::FirstLeaseReadyHeadIdentity leaseDeniedReadyHead{
                .seqId = leaseDeniedSeqId,
                .sourceOrdinal = leaseDeniedReadyHeadSourceOrdinal,
            };
        const bool firstLeaseStandaloneProgressAvailable =
            leaseDeniedReadyHead.valid() &&
            leaseDeniedReadyHead != firstLeaseStandaloneConsumedHead;
        if (schedulingObservabilityEnabled &&
            arenaAdmissionWaiterCount_.load(std::memory_order_acquire) != 0u &&
            leaseDeniedReadyHeadEligibility ==
                render::FirstLeaseReadyHeadEligibility::Eligible &&
            firstLeaseStandaloneConsumedHead.valid() &&
            firstLeaseStandaloneProgressAvailable &&
            firstLeaseCreditRearmObservedHead != leaseDeniedReadyHead) {
          firstLeaseCreditRearmObservedHead = leaseDeniedReadyHead;
          perf::countCpuReadyFirstLeaseCreditRearmed();
        }
        render::FirstLeaseCapacityWaitAction waitAction =
            render::FirstLeaseCapacityWaitAction::Wait;
        const auto classifyWait = [&] {
          const auto currentGeneration =
              queueLifecycle_.cpuReadyCapacityProgressGeneration();
          if (schedulingObservabilityEnabled) {
            perf::updateCpuReadyFirstLeaseWaitGenerations(
                observedCapacityProgress, currentGeneration);
          }
          return render::classifyFirstLeaseCapacityWait({
              .stopped = stop_,
              .admissionPressure = arenaAdmissionWaiterCount_.load(
                  std::memory_order_acquire) != 0u,
              .producerSequenceWaitTargetSeqId =
                  queueLifecycle_.producerSequenceWaitTargetSeqId(),
              .readyHeadOwnsOrdinaryDirectCapacity =
                  leaseDeniedReadyHeadEligibility ==
                  render::FirstLeaseReadyHeadEligibility::Eligible,
              .readyHead = leaseDeniedReadyHead,
              .lastSerialProgressHead =
                  firstLeaseStandaloneConsumedHead,
              .observedGeneration = observedCapacityProgress,
              .currentGeneration = currentGeneration,
          });
        };
        if (schedulingObservabilityEnabled) {
          perf::enterCpuReadyFirstLeaseWait(
              arenaAdmissionWaiterCount_.load(std::memory_order_acquire) != 0u,
              firstLeaseStandaloneProgressAvailable,
              observedCapacityProgress,
              queueLifecycle_.cpuReadyCapacityProgressGeneration(),
              leaseDeniedSeqId, leaseDeniedReadyHeadSourceOrdinal);
        }
        ++cpuReadyCapacityWaiterCount_;
        if (testOnlySchedulingWaitObservationEnabled_) {
          ++testOnlyFirstLeaseWaitEntries_;
          sessionReleaseCv_.notify_all();
        }
        encodeCv_.wait(lock, [&] {
          waitAction = classifyWait();
          return waitAction != render::FirstLeaseCapacityWaitAction::Wait;
        });
        DXMT_ASSERT(cpuReadyCapacityWaiterCount_ > 0);
        --cpuReadyCapacityWaiterCount_;
        if (schedulingObservabilityEnabled) {
          perf::exitCpuReadyFirstLeaseWait(waitAction);
        }
        if (waitAction ==
                render::FirstLeaseCapacityWaitAction::RetryLease &&
            testOnlyPauseAfterFirstLeaseRetry_) {
          testOnlyPauseAfterFirstLeaseRetry_ = false;
          testOnlyPausedAfterFirstLeaseRetry_ = true;
          sessionReleaseCv_.notify_all();
          sessionReleaseCv_.wait(lock, [this] {
            return stop_ || !testOnlyPausedAfterFirstLeaseRetry_;
          });
        }
        if (waitAction == render::FirstLeaseCapacityWaitAction::Stop) {
          return;
        }
        if (waitAction == render::FirstLeaseCapacityWaitAction::
                              ExecuteOneSourceSerialForAdmissionPressure ||
            waitAction == render::FirstLeaseCapacityWaitAction::
                              ExecuteOneSourceSerialForProducerSequenceWait) {
          DXMT_ASSERT(leaseDeniedReadySource.source.valid());
          standaloneSerialSource = leaseDeniedReadySource;
          exactReplaySingleSource = true;
          firstLeaseStandalonePendingHead = leaseDeniedReadyHead;
        } else {
          DXMT_ASSERT(waitAction ==
                      render::FirstLeaseCapacityWaitAction::RetryLease);
        }
        continue;
      }
      if (blockedByPendingCompatibility && pendingRecord.has_value()) {
        const auto posted = sessionReleaseState_.tryPostOrdered(
            blockedReleaseReason, SessionReleaseAction::SubmitSession,
            pendingAdmission.lastRawOrdinal,
            pendingAdmission.lastSeqId);
        if (posted.accepted()) {
          if (blockedReleaseReason ==
              core::metalqueue::SessionReleaseReason::SessionCap) {
            if (blockedCapRequirement.valid()) {
              const auto axes = blockedCapRequirement.sourcesExceeded()
                  ? blockedCapRequirement.pagesExceeded()
                      ? perf::CpuReadySessionCapRequirementAxes::
                            SourcesAndPages
                      : perf::CpuReadySessionCapRequirementAxes::SourcesOnly
                  : perf::CpuReadySessionCapRequirementAxes::PagesOnly;
              perf::recordCpuReadySessionCapRequirement(
                  axes, blockedCapRequirement.predecessorSources,
                  blockedCapRequirement.predecessorPages,
                  blockedCapRequirement.candidatePayloadPages,
                  blockedCapRequirement.candidateWrapPaddingPages,
                  blockedCapRequirement.candidateRequiredPages,
                  blockedCapRequirement.requiredTotalSources,
                  blockedCapRequirement.requiredTotalPages);
            }
            switch (blockedCapDimension) {
            case render::SessionCapacityDimension::Sources:
              if (pendingAdmission.sources >= maxSessionWorkSources) {
                perf::countPostEncodeWorkCapClose();
              }
              perf::countCpuReadySessionCapRelease(
                  perf::CpuReadySessionCapDimension::Sources);
              break;
            case render::SessionCapacityDimension::Pages:
              perf::countCpuReadySessionCapRelease(
                  perf::CpuReadySessionCapDimension::Pages);
              break;
            case render::SessionCapacityDimension::Bytes:
              perf::countCpuReadySessionCapRelease(
                  perf::CpuReadySessionCapDimension::Bytes);
              break;
            case render::SessionCapacityDimension::Draws:
              perf::countCpuReadySessionCapRelease(
                  perf::CpuReadySessionCapDimension::Draws);
              break;
            case render::SessionCapacityDimension::CommandBuffers:
              perf::countCpuReadySessionCapRelease(
                  perf::CpuReadySessionCapDimension::CommandBuffers);
              break;
            case render::SessionCapacityDimension::None:
              break;
            }
          }
          encodeCv_.notify_one();
          continue;
        }
        // A younger externally posted fence can make this predecessor-fence
        // post regress. Submit the already-encoded older prefix directly so
        // the incompatible candidate can become covered by that event; merely
        // spinning on the younger event would deadlock both sides.
        if (!submitPendingRecordLocked(
                lock, sessionFinalizeCauseForRelease(
                          blockedReleaseReason))) {
          abortCpuReadySessionFailOpen(
              "semantic compatibility release fallback");
        }
        continue;
      }
      if (pendingRecord.has_value()) {
        if (!submitPendingRecordLocked(
                lock, encoders::SessionFinalizeCause::Drain)) {
          abortCpuReadySessionFailOpen("queue drained");
        }
        perf::countCpuReadySessionReleased(
            perf::CpuReadySessionReleaseReason::Drain);
      }
      return;
    }

    bool tentativePreflightValid = true;
    for (std::size_t i = 0; i < count; ++i) {
      const auto resolved =
          queueLifecycle_.resolveTentativeSource(lock, scratch[i]);
      if (!resolved.valid() || resolved.metadata != scratch[i].metadata ||
          resolved.semantic != scratch[i].semantic) {
        tentativePreflightValid = false;
        break;
      }
    }
    const bool testOnlyRestore =
        testOnlyRestoreNextCpuReadySessionPreflight_;
    testOnlyRestoreNextCpuReadySessionPreflight_ = false;
    if (!tentativePreflightValid || testOnlyRestore) {
      if (selectionAcquiredLease) {
        releaseCapacityLease();
      } else {
        for (std::size_t i = 0; i < count; ++i) {
          if (selectedCapacityCharged[i] &&
              !capacityLeaseState.uncharge(
                  selectionLeaseGeneration, selectedCapacityCharges[i])) {
            abortCpuReadySessionFailOpen(
                "capacity lease tentative rollback");
          }
        }
      }
      if (!queueLifecycle_.restoreReservedReadySlotBatch(
              lock, std::span<const ReadySlotSnapshot>(scratch.data(), count))) {
        queueLifecycle_.poisonTapeFailureLocked();
        return;
      }
      const bool abandonedStandaloneSerial =
          firstLeaseStandalonePendingHead.valid();
      standaloneSerialSource.reset();
      firstLeaseStandalonePendingHead = {};
      exactReplaySingleSource = !abandonedStandaloneSerial;
      activeRetainedHeadSingleSourceFallback = false;
      if (testOnlyRestore) {
        return;
      }
      continue;
    }

    const bool freshPlanningFrontier =
        !pendingRecord.has_value() && !pendingSession &&
        !pendingAdmission.valid();
    std::array<ResolvedPublishedSource,
               framegraph::kMaxMultiSourcePlanningSources>
        resolvedWindow{};
    std::array<render::SessionAdmissionCandidate,
               framegraph::kMaxMultiSourcePlanningSources>
        admissionWindow{};
    std::array<QueueCompletionSource, kCommandChunkCount>
        selectedCompletionSources{};
    bool selectedPrefixStartsSession = false;
    bool selectedCompletionSourcesValid = false;
    std::optional<render::MultiSourceSessionWindowPreflight>
        preplannedWindowPreflight;
    std::optional<framegraph::MultiSourceReplayPlan> preplannedReplayPlan;
    std::optional<framegraph::MultiSourceReplayRuns> preplannedReplayRuns;
    std::optional<std::vector<ResolvedPublishedSource>>
        preplannedReplayLookaheadSources;
    std::optional<encoders::ActiveRenderDependencySnapshot>
        revalidatedActiveRender;
    std::optional<encoders::RenderPassInstanceToken>
        revalidatedActiveRenderInstance;
    encoders::ActiveSeedInstanceRevalidation activeInstanceRevalidation =
        encoders::ActiveSeedInstanceRevalidation::Unavailable;
    bool replayWindowAttributionEnabled = false;
    encoders::ReplayWindowDisposition sourceLocalReplayDisposition =
        encoders::ReplayWindowDisposition::Ordinary;
    std::uint64_t sourceLocalReplayWindowId = 0;

    // Planner inputs are immutable while the exact selected prefix remains
    // TentativeRepresented. Do not publish Encoding state or advance release
    // coverage until slow FrameGraph/resource work has run without the queue
    // scheduling mutex and the snapshot has been revalidated.
    if (((pendingRecord.has_value() && pendingSession) ||
         freshPlanningFrontier) && count >= 2u &&
        count <= framegraph::kMaxMultiSourcePlanningSources) {
      replayWindowAttributionEnabled = perf::enabled();
      bool planningWindowValid = true;
      for (std::size_t i = 0; i < count && planningWindowValid; ++i) {
        resolvedWindow[i] =
            queueLifecycle_.resolveTentativeSource(lock, scratch[i]);
        const auto& resolved = resolvedWindow[i];
        planningWindowValid =
            resolved.valid() && resolved.metadata == scratch[i].metadata &&
            resolved.semantic == scratch[i].semantic &&
            resolved.slotIndex == scratch[i].slotIndex &&
            resolved.seqId == scratch[i].seqId &&
            resolved.commandBegin == 0u &&
            resolved.commandCount == resolved.payload.commandCount();
        if (planningWindowValid) {
          admissionWindow[i] = admissionCandidateFor(resolved);
          selectedCompletionSources[i] = QueueCompletionSource{
              .source = resolved.source,
              .slotIndex = resolved.slotIndex,
              .seqId = resolved.seqId,
              .hasPresent = resolved.hasPresent,
              .commandBegin = resolved.commandBegin,
              .commandCount = resolved.commandCount,
          };
        }
      }
      if (planningWindowValid) {
        selectedPrefixStartsSession =
            (pendingRecord.has_value() && pendingSession) ||
            render::sessionSourceCanBeHead(
                resolvedWindow[0].payload);
        selectedCompletionSourcesValid = selectedPrefixStartsSession;
      }

      const auto replayFrontierState = pendingSession
          ? encoders::encodeChunkSessionReplayFrontierState(*pendingSession)
          : encoders::EncodeSessionReplayFrontierState::
                CleanClosedEncoderNoPendingClear;
      const auto active = pendingSession
          ? encoders::encodeChunkSessionActiveRenderDependencySnapshot(
                *pendingSession)
          : std::nullopt;
      const auto plannedActiveInstance =
          replayWindowAttributionEnabled && pendingSession
          ? encoders::encodeChunkSessionActiveRenderInstanceToken(
                *pendingSession)
          : std::nullopt;
      const render::MultiSourceSessionWindowFrontier preflightFrontier =
          freshPlanningFrontier
          ? render::MultiSourceSessionWindowFrontier::FreshClean
          : replayFrontierState == encoders::EncodeSessionReplayFrontierState::
                                       CleanClosedEncoderNoPendingClear
              ? render::MultiSourceSessionWindowFrontier::CleanClosed
              : replayFrontierState ==
                            encoders::EncodeSessionReplayFrontierState::
                                ActiveRenderComplete &&
                        active && active->complete
                    ? render::MultiSourceSessionWindowFrontier::
                          ActiveRenderComplete
                    : render::MultiSourceSessionWindowFrontier::Unsupported;
      const bool captureBoundary = fullCaptureBoundary();
      const bool initializerBoundary =
          initializer_ && initializer_->hasPendingUploadsUnlocked();
      if (planningWindowValid && selectedCompletionSourcesValid) {
        preplannedWindowPreflight =
            render::preflightMultiSourceSessionWindow(
                pendingAdmission,
                std::span<const render::SessionAdmissionCandidate>(
                    admissionWindow.data(), count),
                admissionLimits, preflightFrontier, captureBoundary,
                initializerBoundary, releaseFence.has_value());
      }

      if (preplannedWindowPreflight &&
          preplannedWindowPreflight->eligible()) {
        perf::countCpuReadyMultiSourceWindowAttempted();
        const render::MultiSourceSessionReplayFrontier replayFrontier{
            .state = replayFrontierState,
            .activeRender = active,
            .collectActiveSeedMergeWitnesses =
                replayWindowAttributionEnabled,
        };
        const auto releaseSnapshot = releaseFence;
        const std::uint64_t leaseGenerationSnapshot =
            capacityLeaseState.lease().generation;
        framegraph::MultiSourceReplayPlan replayPlan{};
        std::optional<framegraph::MultiSourceReplayRuns> replayRuns;
        const bool borrowWindowValid =
            queueLifecycle_.visitTentativeSourceBorrows(
                lock,
                std::span<const ResolvedPublishedSource>(
                    resolvedWindow.data(), count),
                [&](const SynchronousSourceBorrowBatch& sourceBorrows) noexcept {
                  try {
                    ScopedQueueUnlock queueUnlocked(lock);
                    replayPlan = backend_->planMultiSourceSessionReplay(
                        pool_, sourceBorrows, replayFrontier);
                    if (replayPlan.valid() && replayPlan.reordered()) {
                      std::array<framegraph::MultiSourcePlanningSource,
                                 framegraph::kMaxMultiSourcePlanningSources>
                          planningSources{};
                      for (std::size_t i = 0; i < count; ++i) {
                        planningSources[i].payload = resolvedWindow[i].payload;
                      }
                      replayRuns = framegraph::buildMultiSourceReplayRuns(
                          std::span<const framegraph::MultiSourcePlanningSource>(
                              planningSources.data(), count),
                          replayPlan);
                      if (replayRuns->valid()) {
                        // Materialize replay-order ranges while the represented
                        // payloads are pinned and scheduling is unlocked.
                        std::vector<ResolvedPublishedSource>
                            replayLookaheadSources;
                        replayLookaheadSources.reserve(replayRuns->runs.size());
                        for (const auto& run : replayRuns->runs) {
                          if (run.retainedSourceIndex >= count) {
                            replayLookaheadSources.clear();
                            break;
                          }
                          auto source = resolvedWindow[run.retainedSourceIndex];
                          source.commandBegin = run.commandBegin;
                          source.commandCount = run.commandCount;
                          replayLookaheadSources.push_back(source);
                        }
                        if (replayLookaheadSources.size() ==
                            replayRuns->runs.size()) {
                          preplannedReplayLookaheadSources =
                              std::move(replayLookaheadSources);
                        }
                      }
                    }
                    return true;
                  } catch (...) {
                    return false;
                  }
                });

        bool snapshotStillValid =
            borrowWindowValid &&
            sessionReleaseState_.peekNext() == releaseSnapshot &&
            capacityLeaseState.lease().generation ==
                leaseGenerationSnapshot;
        for (std::size_t i = 0; i < count && snapshotStillValid; ++i) {
          const auto live =
              queueLifecycle_.resolveTentativeSource(lock, scratch[i]);
          snapshotStillValid = live.valid() &&
              sameResolvedSourceIdentity(live, resolvedWindow[i]);
        }
        const bool captureStillMatches =
            fullCaptureBoundary() == captureBoundary;
        const bool initializerStillMatches =
            (initializer_ && initializer_->hasPendingUploadsUnlocked()) ==
            initializerBoundary;
        const auto liveReplayFrontierState = pendingSession
            ? encoders::encodeChunkSessionReplayFrontierState(*pendingSession)
            : encoders::EncodeSessionReplayFrontierState::
                  CleanClosedEncoderNoPendingClear;
        const auto liveActive = pendingSession
            ? encoders::encodeChunkSessionActiveRenderDependencySnapshot(
                  *pendingSession)
            : std::nullopt;
        auto liveActiveInstance =
            replayWindowAttributionEnabled && pendingSession
            ? encoders::encodeChunkSessionActiveRenderInstanceToken(
                  *pendingSession)
            : std::nullopt;
        if (testOnlyOverrideLiveActiveRenderInstance_) {
          testOnlyOverrideLiveActiveRenderInstance_ = false;
          liveActiveInstance = encoders::RenderPassInstanceToken{
              .seqId = testOnlyLiveActiveRenderSeqId_,
              .encoderIndex = testOnlyLiveActiveRenderEncoderIndex_,
          };
        }
        snapshotStillValid = snapshotStillValid && captureStillMatches &&
            initializerStillMatches &&
            liveReplayFrontierState == replayFrontierState &&
            liveActive == active;

        if (!snapshotStillValid) {
          if (selectionAcquiredLease) {
            releaseCapacityLease();
          } else {
            for (std::size_t i = 0; i < count; ++i) {
              if (selectedCapacityCharged[i] &&
                  !capacityLeaseState.uncharge(
                      selectionLeaseGeneration,
                      selectedCapacityCharges[i])) {
                abortCpuReadySessionFailOpen(
                    "capacity lease stale-planner rollback");
              }
            }
          }
          if (!queueLifecycle_.restoreReservedReadySlotBatch(
                  lock, std::span<const ReadySlotSnapshot>(scratch.data(),
                                                           count))) {
            queueLifecycle_.poisonTapeFailureLocked();
            return;
          }
          standaloneSerialSource.reset();
          firstLeaseStandalonePendingHead = {};
          if (testOnlyPauseAfterStaleMultiSourcePlannerRestore_) {
            testOnlyPauseAfterStaleMultiSourcePlannerRestore_ = false;
            testOnlyPausedAfterStaleMultiSourcePlannerRestore_ = true;
            sessionReleaseCv_.notify_all();
            sessionReleaseCv_.wait(lock, [this] {
              return !testOnlyPausedAfterStaleMultiSourcePlannerRestore_;
            });
          }
          exactReplaySingleSource = false;
          activeRetainedHeadSingleSourceFallback = false;
          continue;
        }
        revalidatedActiveRender = active;
        if (replayWindowAttributionEnabled && active) {
          activeInstanceRevalidation =
              encoders::classifyActiveSeedInstanceRevalidation(
                  plannedActiveInstance, liveActiveInstance);
          if (activeInstanceRevalidation == encoders::
                  ActiveSeedInstanceRevalidation::Available) {
            revalidatedActiveRenderInstance = plannedActiveInstance;
          }
        }
        preplannedReplayPlan = std::move(replayPlan);
        preplannedReplayRuns = std::move(replayRuns);
      }
    }
    if (!queueLifecycle_.commitReservedReadySlotBatch(
            lock,
            std::span<const ReadySlotSnapshot>(scratch.data(), count))) {
      if (selectionAcquiredLease) {
        releaseCapacityLease();
      } else {
        for (std::size_t i = 0; i < count; ++i) {
          if (selectedCapacityCharged[i] &&
              !capacityLeaseState.uncharge(
                  selectionLeaseGeneration, selectedCapacityCharges[i])) {
            abortCpuReadySessionFailOpen(
                "capacity lease commit rollback");
          }
        }
      }
      if (!queueLifecycle_.restoreReservedReadySlotBatch(
              lock, std::span<const ReadySlotSnapshot>(scratch.data(), count))) {
        queueLifecycle_.poisonTapeFailureLocked();
        return;
      }
      const bool abandonedStandaloneSerial =
          firstLeaseStandalonePendingHead.valid();
      standaloneSerialSource.reset();
      firstLeaseStandalonePendingHead = {};
      exactReplaySingleSource = !abandonedStandaloneSerial;
      activeRetainedHeadSingleSourceFallback = false;
      continue;
    }
    if (firstLeaseStandalonePendingHead.valid()) {
      firstLeaseStandaloneConsumedHead = firstLeaseStandalonePendingHead;
      firstLeaseStandalonePendingHead = {};
    }
    exactReplaySingleSource = false;
    activeRetainedHeadSingleSourceFallback = false;
    standaloneSerialSource.reset();
    const auto unchargeSelectedCapacity =
        [&](std::size_t sourceIndex) {
          if (sourceIndex >= count ||
              !selectedCapacityCharged[sourceIndex] ||
              !capacityLeaseState.lease().valid()) {
            return;
          }
          if (!capacityLeaseState.uncharge(
                  selectionLeaseGeneration,
                  selectedCapacityCharges[sourceIndex])) {
            abortCpuReadySessionFailOpen(
                "capacity lease represented-source rollback");
          }
          selectedCapacityCharged[sourceIndex] = false;
          if (capacityLeaseState.lease().used.sources == 0 &&
              !pendingRecord.has_value()) {
            releaseCapacityLease();
          } else {
            recordCapacityLeaseUsed();
          }
        };
    const auto tryRetireEncodedSource =
        [&](std::size_t sourceIndex,
            const ResolvedPublishedSource& resolved,
            const render::SessionAdmissionCandidate& admission,
            QueueCompletionSource& retained) {
          if (!pendingRecord || !pendingSession) {
            return false;
          }
          perf::countPostEncodeRetireAttempt();
          const auto ineligible = postEncodeRetirementIneligibility(
              resolved,
              encoders::encodeChunkSessionPendingClearCommand(
                  *pendingSession));
          if (ineligible !=
              core::metalqueue::PostEncodeRetirementIneligibility::None) {
            perf::countPostEncodeRetireIneligible(
                static_cast<std::uint32_t>(ineligible));
            return false;
          }
          const bool arena = resolved.payload.isArena();

          const QueueCompletionSource locatorBacked = retained;
          const auto retired =
              queueLifecycle_.retireEncodedSourcePayload(lock, retained);
          if (retired ==
              core::metalqueue::PostEncodeReceiptResult::WrongState) {
            perf::countPostEncodeRetireIneligible(static_cast<std::uint32_t>(
                core::metalqueue::PostEncodeRetirementIneligibility::
                    NotOldestResident));
            return false;
          }
          if (retired ==
              core::metalqueue::PostEncodeReceiptResult::Capacity) {
            perf::countPostEncodeRetireIneligible(static_cast<std::uint32_t>(
                core::metalqueue::PostEncodeRetirementIneligibility::
                    ReceiptCapacity));
            return false;
          }
          if (retired !=
              core::metalqueue::PostEncodeReceiptResult::Succeeded) {
            abortCpuReadySessionFailOpen(
                "post-encode receipt activation");
          }

          if (!pendingSources.replaceIdentity(locatorBacked, retained)) {
            abortCpuReadySessionFailOpen(
                "post-encode pending completion identity replacement");
          }
          if (!encoders::encodeChunkSessionSources(*pendingSession).empty() &&
              !encoders::replaceEncodeChunkSessionSourceIdentity(
                  *pendingSession, locatorBacked, retained)) {
            abortCpuReadySessionFailOpen(
                "post-encode session completion identity replacement");
          }
          if (!pendingRecord->explicitCompletionSourceSpan().empty() &&
              !pendingRecord->assignFixedCompletionSources(
                  pendingSources.span())) {
            abortCpuReadySessionFailOpen(
                "post-encode submission receipt publication");
          }
          if (!render::retireSessionAdmissionResidency(
                  pendingAdmission, admission)) {
            abortCpuReadySessionFailOpen(
                "post-encode admission residency release");
          }
          const auto physical =
              render::sessionPhysicalResidencyCapacityFor(admission);
          if (!capacityLeaseState.uncharge(
                  capacityLeaseState.lease().generation, physical)) {
            abortCpuReadySessionFailOpen(
                "post-encode lease residency release");
          }
          selectedCapacityCharges[sourceIndex] =
              render::SessionCapacityVector{
                  .draws = admission.semantic.drawCount,
                  .commandBuffers = admission.predictedCommandBuffers,
              };
          perf::countPostEncodeRetireSuccess(arena);
          perf::countPostEncodeResidencyCreditReleased(
              physical.pages, physical.bytes);
          recordCapacityLeaseUsed();
          return true;
        };
    for (std::size_t i = 0; i < count; ++i) {
      sessionReleaseCoveredSeqId_ =
          std::max(sessionReleaseCoveredSeqId_, scratch[i].seqId);
      sessionReleaseCoveredRawOrdinal_ = std::max(
          sessionReleaseCoveredRawOrdinal_,
          scratch[i].metadata.rawOrdinal);
    }

    // H185 invariant (kind-neutral): validate + retain the whole selected
    // source prefix before issuing the cross-source borrow batch.
    if (!selectedPrefixStartsSession) {
      const auto first = queueLifecycle_.resolveRepresentedSource(scratch[0]);
      if (!first.valid()) {
        queueLifecycle_.poisonTapeFailureLocked();
        return;
      }
      selectedPrefixStartsSession =
          (pendingRecord.has_value() && pendingSession) ||
          render::sessionSourceCanBeHead(first.payload);
    }
    if (selectedPrefixStartsSession) {
      const std::size_t retainedCount =
          queueLifecycle_.retainEncodedSourcesForPendingTail(
              lock,
              std::span<const ReadySlotSnapshot>(scratch.data(), count),
              std::span<QueueCompletionSource>(
                  selectedCompletionSources.data(), count));
      selectedCompletionSourcesValid = retainedCount == count;
      DXMT_ASSERT(selectedCompletionSourcesValid);
    }
    auto retainedSourceForIndex =
        [&retainSource, &selectedCompletionSources,
         selectedCompletionSourcesValid](
            std::unique_lock<std::mutex>& lock,
            std::size_t sourceIndex,
            const ReadySlotSnapshot& source,
            QueueCompletionSource& retained) {
          if (selectedCompletionSourcesValid) {
            retained = selectedCompletionSources[sourceIndex];
            return true;
          }
          return retainSource(lock, source, retained);
        };

    // A bounded opt-in lane may encode the current source's leading A draw
    // while its exact FIFO successor is still Writing, then prove the only
    // legal replay A(successor), Clear(B), B(current). The current source is
    // already Represented and fully charged here; no borrowed payload enters
    // the held state.
    bool deferredSuffixHandled = false;
    const bool activeTerminalSuffixFrontier =
        pendingRecord.has_value() && pendingSession &&
        pendingAdmission.valid() && capacityLeaseState.lease().valid() &&
        encoders::encodeChunkSessionReplayFrontierState(*pendingSession) ==
            encoders::EncodeSessionReplayFrontierState::ActiveRenderComplete;
    const auto terminalSuffixFrontier = activeTerminalSuffixFrontier
        ? perf::CpuReadyTerminalSuffixFrontier::ActiveRender
        : perf::CpuReadyTerminalSuffixFrontier::Fresh;
    if (terminalSuffixJoinEnabled &&
        (freshPlanningFrontier || activeTerminalSuffixFrontier) &&
        count == 1u &&
        selectedPrefixStartsSession && selectedCompletionSourcesValid &&
        !fullCaptureBoundary()) {
      ResolvedPublishedSource current =
          queueLifecycle_.resolveRepresentedSource(scratch[0]);
      const auto writingCapacity =
          cpuReadyTape_.leaseAcquisitionCapacitySnapshot();
      const bool exactWriting = current.valid() &&
          isDeferredTerminalSuffixCandidate(current.payload) &&
          writingCapacity.valid &&
          writingCapacity.orderedTailWritingSuccessor.has_value() &&
          writingCapacity.orderedTailWritingSuccessor->valid() &&
          current.metadata.sourceOrdinal !=
              std::numeric_limits<std::uint64_t>::max() &&
          current.seqId != std::numeric_limits<std::uint64_t>::max();
      if (exactWriting) {
        const auto& writing =
            *writingCapacity.orderedTailWritingSuccessor;
        const auto currentIdentity = deferredSuffixIdentity(current);
        const auto currentAdmission = admissionCandidateFor(current);
        const framegraph::SourceCommandRange currentPrefix{
            .source = current.source,
            .sourceOrdinal = current.metadata.sourceOrdinal,
            .seqId = current.seqId,
            .commandBegin = 0u,
            .commandCount = 1u,
        };
        const framegraph::SourceCommandRange currentSuffix{
            .source = current.source,
            .sourceOrdinal = current.metadata.sourceOrdinal,
            .seqId = current.seqId,
            .commandBegin = 1u,
            .commandCount = 2u,
        };
        const auto releaseSnapshot =
            sessionReleaseState_.peekNext().value_or(
                core::metalqueue::SessionReleaseSnapshot{});

        EncodeSessionSourceList stagedSources = pendingSources;
        render::EncodeSessionAdmissionState stagedAdmission =
            pendingAdmission;
        encoders::EncodeChunkSession stagedSession;
        if (freshPlanningFrontier) {
          stagedSession = encoders::makeEncodeChunkSession();
        }
        encoders::EncodeChunkSessionState* registrationSession =
            freshPlanningFrontier ? stagedSession.get()
                                  : pendingSession.get();
        const bool registrationValid = registrationSession &&
            stagedSources.append(selectedCompletionSources[0]) &&
            render::appendSessionAdmission(
                stagedAdmission, currentAdmission, admissionLimits) &&
            encoders::appendEncodeChunkSessionSources(
                *registrationSession,
                std::span<const QueueCompletionSource>(
                    selectedCompletionSources.data(), 1u));
        if (registrationValid) {
          pendingSources = stagedSources;
          pendingAdmission = stagedAdmission;
          if (freshPlanningFrontier) {
            pendingSession = std::move(stagedSession);
          }

          encoders::PreRegisteredEncodeSourceFragmentAccumulator
              currentFragments{};
          ResolvedPublishedSource prefixSource = current;
          prefixSource.commandBegin = 0u;
          prefixSource.commandCount = 1u;
          encoders::EncodeChunkOptions prefixOptions{};
          prefixOptions.allowInjectedCommandBufferMidChunkCommits = true;
          const obj_handle_t injectedCommandBuffer = pendingRecord
              ? pendingRecord->commandBuffer.handle
              : NULL_OBJECT_HANDLE;
          if (pendingRecord) {
            prefixOptions.commandBuffer = pendingRecord->commandBuffer;
          }
          prefixOptions.session = pendingSession.get();
          prefixOptions.deferSessionFinalization = true;
          prefixOptions.partitionSource = current.source;
          prefixOptions.preRegisteredFragment =
              encoders::PreRegisteredEncodeChunkFragment{
                  .commandBegin = 0u,
                  .commandCount = 1u,
                  .sourceFragmentOrdinal = 0u,
                  .sourceFragmentCount = 2u,
                  .transactionFragmentOrdinal = 0u,
                  .transactionFragmentCount = 3u,
              };
          prefixOptions.preRegisteredSourceAccumulator =
              &currentFragments;
          prefixOptions.skipBackendPlanning = true;

          lock.unlock();
          auto prefixSubmission = encodeCpuReadySessionSource(
              prefixSource, std::move(prefixOptions));
          lock.lock();
          prefixSource = {};
          if (!prefixSubmission) {
            abortCpuReadySessionFailOpen(
                "deferred terminal-suffix prefix encode");
          }
          if (pendingRecord &&
              !core::metalqueue::foldEncodedSessionFragmentCarrier(
                  *prefixSubmission, *pendingRecord,
                  injectedCommandBuffer)) {
            abortCpuReadySessionFailOpen(
                "deferred terminal-suffix prefix carrier fold");
          }
          pendingRecord = std::move(*prefixSubmission);
          const auto heldReplayFrontier =
              encoders::encodeChunkSessionReplayFrontierState(
                  *pendingSession);
          const auto heldActive =
              encoders::encodeChunkSessionActiveRenderDependencySnapshot(
                  *pendingSession);
          const auto heldActiveInstance =
              encoders::encodeChunkSessionActiveRenderInstanceToken(
                  *pendingSession);
          const bool heldCaptureBoundary = fullCaptureBoundary();

          auto prefixEncoded =
              render::makeDeferredTerminalSuffixPrefixEncoded(
                  currentIdentity, scratch[0], selectedCompletionSources[0],
                  currentAdmission, selectedCapacityCharges[0],
                  currentPrefix, currentSuffix, writing.source,
                  writing.claim, capacityLeaseState.lease(),
                  capacityPolicy.successorHeadroom, releaseSnapshot);
          if (!prefixEncoded) {
            abortCpuReadySessionFailOpen(
                "deferred terminal-suffix prefix state");
          }
          prefixEncoded->fragments = currentFragments;
          auto held = render::holdDeferredTerminalSuffix(*prefixEncoded);
          if (!held) {
            abortCpuReadySessionFailOpen(
                "deferred terminal-suffix held state");
          }
          perf::countCpuReadyTerminalSuffixPrefix(terminalSuffixFrontier);
          // SourcePayloadView is a synchronous borrow. The park below retains
          // only the value-only held state and re-resolves the represented
          // current source for each later call-local use.
          current = {};

          const auto heldBoundaryStillMatches = [&]() noexcept {
            return pendingSession &&
                encoders::encodeChunkSessionReplayFrontierState(
                    *pendingSession) == heldReplayFrontier &&
                encoders::encodeChunkSessionActiveRenderDependencySnapshot(
                    *pendingSession) == heldActive &&
                encoders::encodeChunkSessionActiveRenderInstanceToken(
                    *pendingSession) == heldActiveInstance &&
                fullCaptureBoundary() == heldCaptureBoundary;
          };
          const auto preEffectValuesStillMatch =
              [&](const render::SessionCapacityVector& expectedUsed)
                  noexcept {
                const auto& liveLease = capacityLeaseState.lease();
                return heldBoundaryStillMatches() &&
                    liveLease.generation == held->leaseGeneration &&
                    liveLease.reserved == held->leaseReserved &&
                    liveLease.used == expectedUsed &&
                    liveLease.successorRemaining ==
                        held->leaseSuccessorRemaining &&
                    sessionReleaseState_.peekNext().value_or(
                        core::metalqueue::SessionReleaseSnapshot{}) ==
                        held->release &&
                    !sessionReleaseState_.hasPending() &&
                    !queueLifecycle_.producerSequenceWaitActive() &&
                    !(initializer_ &&
                      initializer_->hasPendingUploadsUnlocked()) &&
                    !stop_ && !heldCaptureBoundary;
              };

          const auto drainCurrentSuffix = [&]() {
            ResolvedPublishedSource suffixSource =
                queueLifecycle_.resolveRepresentedSource(scratch[0]);
            if (!suffixSource.valid() ||
                deferredSuffixIdentity(suffixSource) !=
                    held->currentIdentity) {
              abortCpuReadySessionFailOpen(
                  "deferred terminal-suffix drain current identity");
            }
            suffixSource.commandBegin = 1u;
            suffixSource.commandCount = 2u;
            encoders::EncodeChunkOptions suffixOptions{};
            const obj_handle_t injectedCommandBuffer =
                pendingRecord ? pendingRecord->commandBuffer.handle
                              : NULL_OBJECT_HANDLE;
            if (pendingRecord) {
              suffixOptions.commandBuffer = pendingRecord->commandBuffer;
            }
            suffixOptions.allowInjectedCommandBufferMidChunkCommits = true;
            suffixOptions.session = pendingSession.get();
            suffixOptions.deferSessionFinalization = true;
            suffixOptions.partitionSource = held->currentIdentity.source;
            suffixOptions.preRegisteredFragment =
                encoders::PreRegisteredEncodeChunkFragment{
                    .commandBegin = 1u,
                    .commandCount = 2u,
                    .sourceFragmentOrdinal = 1u,
                    .sourceFragmentCount = 2u,
                    .transactionFragmentOrdinal = 1u,
                    .transactionFragmentCount = 2u,
                };
            suffixOptions.preRegisteredSourceAccumulator =
                &held->fragments;
            suffixOptions.skipBackendPlanning = true;
            lock.unlock();
            auto suffixSubmission = encodeCpuReadySessionSource(
                suffixSource, std::move(suffixOptions));
            lock.lock();
            if (!suffixSubmission || !pendingRecord ||
                !core::metalqueue::foldEncodedSessionFragmentCarrier(
                    *suffixSubmission, *pendingRecord,
                    injectedCommandBuffer)) {
              abortCpuReadySessionFailOpen(
                  "deferred terminal-suffix natural drain");
            }
            pendingRecord = std::move(*suffixSubmission);
            pendingRecord->seqId = held->currentIdentity.seqId;
            pendingRecord->slotIndex = held->current.slotIndex;
            pendingRecord->diagnostics.seqId = held->currentIdentity.seqId;
            pendingRecord->diagnostics.slotIndex = held->current.slotIndex;
            QueueCompletionSource currentCompletion =
                selectedCompletionSources[0];
            (void)tryRetireEncodedSource(
                0u, suffixSource, currentAdmission, currentCompletion);
            recordCapacityLeaseUsed();
            perf::countCpuReadyTerminalSuffixNaturalDrain(
                terminalSuffixFrontier);
            deferredSuffixHandled = true;
          };

          if (heldReplayFrontier != encoders::
                  EncodeSessionReplayFrontierState::ActiveRenderComplete ||
              !heldActive || !heldActive->complete ||
              !heldActiveInstance || heldCaptureBoundary) {
            drainCurrentSuffix();
          }

          std::array<core::CpuReadyTape::ReadyEntry, 1u> ready{};
          bool exactReady = false;
          while (!deferredSuffixHandled && !exactReady) {
            exactReady = cpuReadyTape_.copyReadyPrefix(ready) == 1u &&
                ready[0].valid() &&
                ready[0].source == held->expectedWritingSuccessor.source &&
                ready[0].seqId == held->expectedWritingSuccessor.seqId &&
                ready[0].metadata.sourceOrdinal ==
                    held->expectedWritingSuccessor.sourceOrdinal;

            render::DeferredTerminalSuffixObservation observation{
                .currentIdentity = held->currentIdentity,
                .currentAdmission = held->admission,
                .lease = capacityLeaseState.lease(),
                .release = sessionReleaseState_.peekNext().value_or(
                    core::metalqueue::SessionReleaseSnapshot{}),
            };
            if (exactReady) {
              const auto readyPayload = cpuReadyTape_.resolveSourcePayload(
                  ready[0].source.id, ready[0].source.storage,
                  core::CpuReadyTape::State::Ready);
              if (!readyPayload.valid()) {
                exactReady = false;
              } else {
                const ResolvedPublishedSource readySource{
                    .source = ready[0].source,
                    .slotIndex = ready[0].controlIndex,
                    .seqId = ready[0].seqId,
                    .metadata = ready[0].metadata,
                    .semantic = ready[0].semantic,
                    .payload = readyPayload,
                    .sourceId = ready[0].source.id,
                    .storage = ready[0].source.storage,
                    .slot = readyPayload.legacyPayload(),
                    .hasPresent = readyPayload.presentRecordCount() != 0u,
                    .commandBegin = 0u,
                    .commandCount = readyPayload.commandCount(),
                };
                observation.successor = deferredSuffixIdentity(readySource);
                observation.successorAdmission =
                    admissionCandidateFor(readySource);
                observation.wakeFlags |= render::
                    DeferredTerminalSuffixWakeExactSuccessorReady;
              }
            }
            if (sessionReleaseState_.hasPending()) {
              observation.wakeFlags |=
                  render::DeferredTerminalSuffixWakeOrderedRelease;
            }
            if (queueLifecycle_.producerSequenceWaitActive()) {
              observation.wakeFlags |=
                  render::DeferredTerminalSuffixWakeProducerWait;
            }
            if (initializer_ && initializer_->hasPendingUploadsUnlocked()) {
              observation.wakeFlags |=
                  render::DeferredTerminalSuffixWakeInitializer;
            }
            if (stop_) {
              observation.wakeFlags |=
                  render::DeferredTerminalSuffixWakeStop;
            }
            if (arenaAdmissionWaiterCount_.load(
                    std::memory_order_acquire) != 0u) {
              observation.wakeFlags |=
                  render::DeferredTerminalSuffixWakeAdmissionPressure;
            }
            if (queueLifecycle_.producerWriterPressureActive()) {
              observation.wakeFlags |=
                  render::DeferredTerminalSuffixWakeWriterPressure;
            }
            if (!exactReady) {
              const auto liveWriting =
                  cpuReadyTape_.leaseAcquisitionCapacitySnapshot();
              const bool writerStillExact = liveWriting.valid &&
                  liveWriting.orderedTailWritingSuccessor.has_value() &&
                  liveWriting.orderedTailWritingSuccessor->valid() &&
                  liveWriting.orderedTailWritingSuccessor->source ==
                      held->expectedWritingSuccessor.source &&
                  liveWriting.orderedTailWritingSuccessor->claim ==
                      held->successorWritingClaim;
              if (!writerStillExact) {
                observation.wakeFlags |=
                    render::DeferredTerminalSuffixWakeWriterLost;
              }
            }

            lock.unlock();
            const auto decision =
                render::classifyDeferredTerminalSuffix(*held, observation);
            lock.lock();
            if (decision.decision ==
                render::DeferredTerminalSuffixDecision::NaturalDrain) {
              drainCurrentSuffix();
              break;
            }
            if (exactReady) {
              break;
            }
            if (decision.decision !=
                render::DeferredTerminalSuffixDecision::WaitUnlocked) {
              abortCpuReadySessionFailOpen(
                  "deferred terminal-suffix wait classification");
            }
            const std::uint64_t observedCapacityProgress =
                queueLifecycle_.cpuReadyCapacityProgressGeneration();
            encodeCv_.wait(lock, [this, observedCapacityProgress] {
              return render::retainedOrDeferredSessionWaitDone({
                  .stopped = stop_,
                  .ready = !cpuReadyTape_.readyEmpty(),
                  .orderedRelease = sessionReleaseState_.hasPending(),
                  .producerSequenceWait =
                      queueLifecycle_.producerSequenceWaitActive(),
                  .admissionPressure = arenaAdmissionWaiterCount_.load(
                      std::memory_order_acquire) != 0u,
                  .writerPressure =
                      queueLifecycle_.producerWriterPressureActive(),
                  .initializerPending = initializer_ &&
                      initializer_->hasPendingUploadsUnlocked(),
                  .capacityProgress =
                      queueLifecycle_.cpuReadyCapacityProgressGeneration() !=
                          observedCapacityProgress,
              });
            });
          }

          if (!deferredSuffixHandled && exactReady &&
              !preEffectValuesStillMatch(
                  held->leaseUsedBeforeSuccessor)) {
            drainCurrentSuffix();
          }

          if (!deferredSuffixHandled && exactReady) {
            const std::size_t successorCount =
                queueLifecycle_.reserveReadySlotBatchPrefix(
                    lock,
                    std::span<ReadySlotSnapshot>(scratch.data() + 1u, 1u),
                    [&](const SynchronousSourceBorrowBatch& candidates)
                        noexcept {
                      if (candidates.empty()) {
                        return std::size_t{0};
                      }
                      bool matches = false;
                      candidates.visitAt(
                          0u, [&](const GenerationQualifiedSourceBorrow& candidate)
                              noexcept {
                            matches =
                                candidate.source() ==
                                    held->expectedWritingSuccessor.source &&
                                candidate.seqId() ==
                                    held->expectedWritingSuccessor.seqId &&
                                candidate.metadata().sourceOrdinal ==
                                    held->expectedWritingSuccessor.sourceOrdinal;
                          });
                      return matches ? std::size_t{1} : std::size_t{0};
                    });
            if (successorCount != 1u) {
              drainCurrentSuffix();
            } else {
              const ResolvedPublishedSource successor =
                  queueLifecycle_.resolveTentativeSource(lock, scratch[1]);
              const auto successorAdmission =
                  admissionCandidateFor(successor);
              render::DeferredTerminalSuffixObservation observation{
                  .currentIdentity = held->currentIdentity,
                  .currentAdmission = held->admission,
                  .successor = deferredSuffixIdentity(successor),
                  .successorAdmission = successorAdmission,
                  .lease = capacityLeaseState.lease(),
                  .release = sessionReleaseState_.peekNext().value_or(
                      core::metalqueue::SessionReleaseSnapshot{}),
                  .wakeFlags = render::
                      DeferredTerminalSuffixWakeExactSuccessorReady,
              };
              lock.unlock();
              const auto reserveDecision =
                  render::classifyDeferredTerminalSuffix(*held,
                                                         observation);
              lock.lock();

              bool successorCharged = false;
              auto restoreSuccessor = [&]() {
                if (successorCharged) {
                  if (!capacityLeaseState.uncharge(
                          capacityLeaseState.lease().generation,
                          render::sessionCapacityFor(successorAdmission))) {
                    abortCpuReadySessionFailOpen(
                        "deferred terminal-suffix successor uncharge");
                  }
                  successorCharged = false;
                }
                if (!queueLifecycle_.restoreReservedReadySlotBatch(
                        lock, std::span<const ReadySlotSnapshot>(
                                  scratch.data() + 1u, 1u))) {
                  queueLifecycle_.poisonTapeFailureLocked();
                  return false;
                }
                return true;
              };

              bool joinAccepted = successor.valid() &&
                  reserveDecision.decision == render::
                      DeferredTerminalSuffixDecision::ReserveExactSuccessor &&
                  preEffectValuesStillMatch(
                      held->leaseUsedBeforeSuccessor);
              const ResolvedPublishedSource currentForPlanning =
                  queueLifecycle_.resolveRepresentedSource(scratch[0]);
              joinAccepted = joinAccepted && currentForPlanning.valid() &&
                  heldActive && heldActive->complete &&
                  heldActiveInstance.has_value();

              framegraph::DeferredTerminalSuffixPlan exactPlan{};
              if (joinAccepted) {
                const auto currentView = framegraph::
                    DeferredTerminalSuffixPlanningSourceView{
                        .payload = currentForPlanning.payload,
                        .source = currentForPlanning.source,
                        .sourceOrdinal =
                            currentForPlanning.metadata.sourceOrdinal,
                        .seqId = currentForPlanning.seqId,
                    };
                const auto successorView = framegraph::
                    DeferredTerminalSuffixPlanningSourceView{
                        .payload = successor.payload,
                        .source = successor.source,
                        .sourceOrdinal = successor.metadata.sourceOrdinal,
                        .seqId = successor.seqId,
                    };
                const auto activeSeed = deferredSuffixActiveSeed(*heldActive);
                lock.unlock();
                exactPlan = framegraph::planDeferredTerminalSuffixReplay(
                    currentView, successorView, activeSeed,
                    render::makeResourceAliasResolver(pool_));
                const auto validation = exactPlan.qualified()
                    ? framegraph::validateDeferredTerminalSuffixReplay(
                          currentView, successorView, activeSeed,
                          exactPlan.proof, exactPlan.proof.joinedReplay,
                          render::makeResourceAliasResolver(pool_))
                    : framegraph::
                          DeferredTerminalSuffixReplayValidation::
                              InvalidProof;
                lock.lock();
                joinAccepted = exactPlan.qualified() &&
                    validation == framegraph::
                        DeferredTerminalSuffixReplayValidation::Valid;
              }

              const auto liveCurrent =
                  queueLifecycle_.resolveRepresentedSource(scratch[0]);
              const auto liveSuccessor =
                  queueLifecycle_.resolveTentativeSource(lock, scratch[1]);
              joinAccepted = joinAccepted &&
                  sameResolvedSourceIdentity(liveCurrent,
                                             currentForPlanning) &&
                  sameResolvedSourceIdentity(liveSuccessor, successor) &&
                  preEffectValuesStillMatch(
                      held->leaseUsedBeforeSuccessor);
              if (testOnlyRestoreNextCpuReadySessionPreflight_) {
                testOnlyRestoreNextCpuReadySessionPreflight_ = false;
                joinAccepted = false;
              }

              render::EncodeSessionAdmissionState joinedAdmission =
                  pendingAdmission;
              QueueCompletionSource successorCompletion =
                  core::metalqueue::completionSourceForReadySlot(scratch[1]);
              joinAccepted = joinAccepted &&
                  pendingSources.canAppend(successorCompletion) &&
                  encoders::canAppendEncodeChunkSessionSource(
                      *pendingSession, successorCompletion) &&
                  render::appendSessionAdmission(
                      joinedAdmission, successorAdmission, admissionLimits);
              if (joinAccepted) {
                successorCharged = capacityLeaseState.charge(
                    held->leaseGeneration,
                    render::sessionCapacityFor(successorAdmission));
                joinAccepted = successorCharged;
              }

              const auto chargedUsed = render::addSessionCapacity(
                  held->leaseUsedBeforeSuccessor,
                  render::sessionCapacityFor(successorAdmission));
              joinAccepted = joinAccepted && chargedUsed.has_value() &&
                  preEffectValuesStillMatch(*chargedUsed);

              std::optional<render::DeferredTerminalSuffixState> tentative;
              if (joinAccepted) {
                observation.lease = capacityLeaseState.lease();
                observation.release = held->release;
                observation.proof = exactPlan.proof;
                observation.proofValidated = true;
                lock.unlock();
                tentative =
                    render::markDeferredTerminalSuffixSuccessorTentative(
                        *held, observation);
                const auto effectDecision = tentative
                    ? render::classifyDeferredTerminalSuffix(
                          *tentative, observation)
                    : render::DeferredTerminalSuffixDecisionResult{};
                lock.lock();
                const auto effectActive = pendingSession
                    ? encoders::
                          encodeChunkSessionActiveRenderDependencySnapshot(
                              *pendingSession)
                    : std::nullopt;
                const auto effectActiveInstance = pendingSession
                    ? encoders::encodeChunkSessionActiveRenderInstanceToken(
                          *pendingSession)
                    : std::nullopt;
                joinAccepted = tentative.has_value() &&
                    effectDecision.decision == render::
                        DeferredTerminalSuffixDecision::JoinExactSuccessor &&
                    effectActive == heldActive &&
                    effectActiveInstance == heldActiveInstance &&
                    preEffectValuesStillMatch(*chargedUsed);
              }

              if (!joinAccepted) {
                if (restoreSuccessor()) {
                  drainCurrentSuffix();
                }
              } else {
                const ResolvedPublishedSource currentForEffects =
                    queueLifecycle_.resolveRepresentedSource(scratch[0]);
                const ResolvedPublishedSource successorForEffects =
                    queueLifecycle_.resolveTentativeSource(lock, scratch[1]);
                joinAccepted =
                    sameResolvedSourceIdentity(currentForEffects,
                                               currentForPlanning) &&
                    sameResolvedSourceIdentity(successorForEffects,
                                               successor) &&
                    pendingSession &&
                    encoders::encodeChunkSessionActiveRenderDependencySnapshot(
                        *pendingSession) == heldActive &&
                    encoders::encodeChunkSessionActiveRenderInstanceToken(
                        *pendingSession) == heldActiveInstance &&
                    preEffectValuesStillMatch(*chargedUsed);
                if (!joinAccepted) {
                  if (restoreSuccessor()) {
                    drainCurrentSuffix();
                  }
                } else if (!queueLifecycle_.commitReservedReadySlotBatch(
                             lock, std::span<const ReadySlotSnapshot>(
                                       scratch.data() + 1u, 1u))) {
                  if (restoreSuccessor()) {
                    drainCurrentSuffix();
                  }
                } else {
                  successorCharged = false;
                  sessionReleaseCoveredSeqId_ = std::max(
                      sessionReleaseCoveredSeqId_, successor.seqId);
                  sessionReleaseCoveredRawOrdinal_ = std::max(
                      sessionReleaseCoveredRawOrdinal_,
                      successor.metadata.rawOrdinal);
                  if (!retainSource(lock, scratch[1], successorCompletion) ||
                      !pendingSources.append(successorCompletion) ||
                      !encoders::appendEncodeChunkSessionSources(
                          *pendingSession,
                          std::span<const QueueCompletionSource>(
                              &successorCompletion, 1u))) {
                    abortCpuReadySessionFailOpen(
                        "deferred terminal-suffix successor registration");
                  }
                  pendingAdmission = joinedAdmission;

                  std::array<ResolvedPublishedSource, 2u> replaySources{
                      successorForEffects, currentForEffects};
                  replaySources[0].commandBegin = 0u;
                  replaySources[0].commandCount = 1u;
                  replaySources[1].commandBegin = 1u;
                  replaySources[1].commandCount = 2u;
                  std::array<ResolvedPublishedSource, 2u> naturalSources{
                      currentForEffects, successorForEffects};
                  encoders::PreRegisteredEncodeSourceFragmentAccumulator
                      successorFragments{};
                  std::optional<QueueSubmissionRecord> carrier =
                      std::move(pendingRecord);
                  pendingRecord.reset();

                  for (std::size_t run = 0; run < replaySources.size();
                       ++run) {
                    const bool successorRun = run == 0u;
                    encoders::EncodeChunkOptions options{};
                    const obj_handle_t injectedCommandBuffer = carrier
                        ? carrier->commandBuffer.handle
                        : NULL_OBJECT_HANDLE;
                    if (carrier) {
                      options.commandBuffer = carrier->commandBuffer;
                    }
                    options.allowInjectedCommandBufferMidChunkCommits = true;
                    options.session = pendingSession.get();
                    options.deferSessionFinalization = true;
                    options.partitionSource = replaySources[run].source;
                    options.preRegisteredFragment =
                        encoders::PreRegisteredEncodeChunkFragment{
                            .commandBegin = replaySources[run].commandBegin,
                            .commandCount = replaySources[run].commandCount,
                            .sourceFragmentOrdinal = successorRun ? 0u : 1u,
                            .sourceFragmentCount = successorRun ? 1u : 2u,
                            .transactionFragmentOrdinal =
                                static_cast<std::uint32_t>(run + 1u),
                            .transactionFragmentCount = 3u,
                        };
                    options.preRegisteredSourceAccumulator = successorRun
                        ? &successorFragments
                        : &held->fragments;
                    options.skipBackendPlanning = true;
                    auto submission = encodeWithBorrows(
                        lock,
                        std::span<const ResolvedPublishedSource>(
                            replaySources.data() + run,
                            replaySources.size() - run),
                        replaySources[run], std::move(options), true);
                    if (!submission ||
                        (carrier &&
                         !core::metalqueue::
                             foldEncodedSessionFragmentCarrier(
                                 *submission, *carrier,
                                 injectedCommandBuffer))) {
                      abortCpuReadySessionFailOpen(
                          "deferred terminal-suffix joined effect");
                    }
                    if (!pendingSources.setOwner(replaySources[run].seqId,
                                                 submission->pipelineOwner) ||
                        !pendingSession ||
                        !encoders::setEncodeChunkSessionSourceOwner(
                            *pendingSession, replaySources[run].seqId,
                            submission->pipelineOwner)) {
                      abortCpuReadySessionFailOpen(
                          "deferred terminal-suffix owner attribution");
                    }
                    carrier = std::move(*submission);
                  }

                  const bool observed =
                      queueLifecycle_.visitRepresentedSourceBorrows(
                          lock,
                          std::span<const ResolvedPublishedSource>(
                              naturalSources),
                          [&](const SynchronousSourceBorrowBatch& sourceBorrows)
                              noexcept {
                            try {
                              ScopedQueueUnlock queueUnlocked(lock);
                              backend_->observeMultiSourceSessionReplay(
                                  pool_, sourceBorrows);
                              return true;
                            } catch (...) {
                              return false;
                            }
                          });
                  if (!observed) {
                    abortCpuReadySessionFailOpen(
                        "deferred terminal-suffix observation borrow");
                  }
                  pendingRecord = std::move(carrier);
                  pendingRecord->seqId = successor.seqId;
                  pendingRecord->slotIndex = successor.slotIndex;
                  pendingRecord->diagnostics.seqId = successor.seqId;
                  pendingRecord->diagnostics.slotIndex =
                      successorForEffects.slotIndex;
                  QueueCompletionSource currentCompletion =
                      selectedCompletionSources[0];
                  (void)tryRetireEncodedSource(
                      0u, currentForEffects, currentAdmission,
                      currentCompletion);
                  selectedCapacityCharges[1] =
                      render::sessionCapacityFor(successorAdmission);
                  selectedCapacityCharged[1] = true;
                  (void)tryRetireEncodedSource(
                      1u, successorForEffects, successorAdmission,
                      successorCompletion);
                  recordCapacityLeaseUsed();
                  perf::countCpuReadyTerminalSuffixJoined(
                      terminalSuffixFrontier);
                  deferredSuffixHandled = true;
                }
              }
            }
          }
        }
      }
    }
    if (deferredSuffixHandled) {
      continue;
    }

    // The production cross-source planner consumes only a fully retained
    // 2..8-source window. It may start from an active/clean carried session or
    // from the implicit clean frontier before the first source creates a CB.
    // Natural/invalid outcomes leave the existing source-local loop untouched.
    // Once FIFO completion sources are pre-registered and the first fragment
    // enters encodeChunk, every later failure is fail-stop: restoring source
    // order would duplicate Metal effects.
    if (preplannedWindowPreflight.has_value()) {
      bool resolvedWindowValid = selectedCompletionSourcesValid;
      for (std::size_t i = 0; i < count && resolvedWindowValid; ++i) {
        resolvedWindow[i] =
            queueLifecycle_.resolveRepresentedSource(scratch[i]);
        resolvedWindowValid =
            resolvedWindow[i].valid() &&
            resolvedWindow[i].metadata == scratch[i].metadata &&
            resolvedWindow[i].semantic == scratch[i].semantic &&
            resolvedWindow[i].source == selectedCompletionSources[i].source &&
            resolvedWindow[i].slotIndex ==
                selectedCompletionSources[i].slotIndex &&
            resolvedWindow[i].seqId == selectedCompletionSources[i].seqId &&
            resolvedWindow[i].commandBegin == 0u &&
            resolvedWindow[i].commandCount ==
                resolvedWindow[i].payload.commandCount();
        if (resolvedWindowValid) {
          admissionWindow[i] = admissionCandidateFor(resolvedWindow[i]);
        }
      }
      if (!resolvedWindowValid) {
        perf::countCpuReadyMultiSourceWindowFallback(
            selectedCompletionSourcesValid
                ? perf::CpuReadyMultiSourceFallbackReason::ResolvedSource
                : perf::CpuReadyMultiSourceFallbackReason::CompletionSource);
      } else {
        const auto& windowPreflight = *preplannedWindowPreflight;
        if (!windowPreflight.eligible()) {
          perf::countCpuReadyMultiSourceWindowFallback(
              perf::CpuReadyMultiSourceFallbackReason::Eligibility);
          using PreflightReason =
              render::MultiSourceSessionWindowPreflightReason;
          using EligibilityReason =
              perf::CpuReadyMultiSourceEligibilityReason;
          switch (windowPreflight.reason) {
          case PreflightReason::ActiveRenderIncomplete:
            perf::countCpuReadyMultiSourceEligibilityFallback(
                EligibilityReason::ActiveRenderIncomplete);
            break;
          case PreflightReason::PresentBoundary:
            perf::countCpuReadyMultiSourceEligibilityFallback(
                EligibilityReason::PresentBoundary);
            if (replayWindowAttributionEnabled) {
              sourceLocalReplayDisposition = encoders::
                  ReplayWindowDisposition::EligibilityPresent;
            }
            break;
          case PreflightReason::NonConsecutiveIdentity:
            perf::countCpuReadyMultiSourceEligibilityFallback(
                EligibilityReason::NonConsecutiveIdentity);
            break;
          default:
            perf::countCpuReadyMultiSourceEligibilityFallback(
                EligibilityReason::OtherBoundary);
            if (replayWindowAttributionEnabled) {
              sourceLocalReplayDisposition = encoders::
                  ReplayWindowDisposition::EligibilityOther;
            }
            break;
          }
          if (replayWindowAttributionEnabled &&
              sourceLocalReplayDisposition ==
              encoders::ReplayWindowDisposition::Ordinary) {
            sourceLocalReplayDisposition = encoders::
                ReplayWindowDisposition::EligibilityOther;
          }
          if (replayWindowAttributionEnabled) {
            sourceLocalReplayWindowId =
                resolvedWindow[0].metadata.sourceOrdinal;
          }
        } else {
          DXMT_ASSERT(preplannedReplayPlan.has_value());
          const framegraph::MultiSourceReplayPlan& replayPlan =
              *preplannedReplayPlan;
          perf::recordCpuReadyMultiSourcePlannerOutcome(
              plannerPerfOutcome(replayPlan.diagnostics.outcome),
              plannerPerfMerge(replayPlan.diagnostics.merge),
              replayPlan.diagnostics.firstMatchingPassDistance,
              perf::CpuReadyMultiSourceSeedMergeAttribution{
                  .optimizerMergeCount =
                      replayPlan.diagnostics.optimizerMergeCount,
                  .seedMergeCount =
                      replayPlan.diagnostics.activeSeedMergeCount,
                  .seedMergeDistanceTotal =
                      replayPlan.diagnostics.activeSeedMergeDistanceTotal,
                  .seedMergeDistanceMax =
                      replayPlan.diagnostics.activeSeedMergeDistanceMax,
                  .commandBefore =
                      replayPlan.diagnostics.activeSeedCommandBefore,
                  .commandAfter =
                      replayPlan.diagnostics.activeSeedCommandAfter,
                  .emptyIntervening =
                      replayPlan.diagnostics.activeSeedEmptyIntervening,
                  .missing = replayPlan.diagnostics
                      .activeSeedMergeAttributionMissing,
              },
              replayPlan.diagnostics.seedSecondNonDraw,
              replayPlan.diagnostics.seedBlockedCycle);

          if (!replayPlan.valid()) {
            perf::countCpuReadyMultiSourceWindowFallback(
                perf::CpuReadyMultiSourceFallbackReason::InvalidPlan);
          } else if (!replayPlan.reordered()) {
            perf::countCpuReadyMultiSourceWindowFallback(
                perf::CpuReadyMultiSourceFallbackReason::NaturalPlan);
            if (replayWindowAttributionEnabled &&
                replayPlan.diagnostics.outcome ==
                framegraph::MultiSourcePlannerOutcome::NaturalAfterMerge) {
              sourceLocalReplayDisposition = encoders::
                  ReplayWindowDisposition::NaturalAfterMergeFallback;
            } else if (replayWindowAttributionEnabled &&
                       replayPlan.diagnostics.outcome ==
                       framegraph::MultiSourcePlannerOutcome::
                           PermutationRejected) {
              sourceLocalReplayDisposition = encoders::
                  ReplayWindowDisposition::PermutationRejectedFallback;
            }
            if (replayWindowAttributionEnabled) {
              sourceLocalReplayWindowId =
                  resolvedWindow[0].metadata.sourceOrdinal;
            }
          } else {
            DXMT_ASSERT(preplannedReplayRuns.has_value());
            const auto& replayRuns = *preplannedReplayRuns;
            if (replayRuns.validation == framegraph::
                    MultiSourceReplayRunValidation::RepeatedSourceRun) {
              perf::countCpuReadyMultiSourceWindowFallback(
                  perf::CpuReadyMultiSourceFallbackReason::RepeatedSource);
            } else if (!replayRuns.valid() ||
                       !preplannedReplayLookaheadSources.has_value() ||
                       preplannedReplayLookaheadSources->size() !=
                           replayRuns.runs.size()) {
              perf::countCpuReadyMultiSourceWindowFallback(
                  perf::CpuReadyMultiSourceFallbackReason::InvalidPlan);
            } else {
              const bool repeatedSourceRun = std::any_of(
                  replayRuns.runs.begin(), replayRuns.runs.end(),
                  [](const framegraph::MultiSourceReplayRun& run) {
                    return run.sourceFragmentCount > 1u;
                  });
              if (repeatedSourceRun &&
                  !encoders::encodeChunkAllowsRepeatedSourceFragments()) {
                perf::countCpuReadyMultiSourceWindowFallback(
                    perf::CpuReadyMultiSourceFallbackReason::RepeatedSource);
                goto multi_source_window_complete;
              }
              EncodeSessionSourceList stagedSources = pendingSources;
              const auto carriedSessionSources = pendingSession
                  ? encoders::encodeChunkSessionSources(*pendingSession)
                  : std::span<const QueueCompletionSource>{};
              const bool exactRedundantFixedSources =
                  !freshPlanningFrontier && pendingRecord.has_value() &&
                  pendingSession &&
                  (pendingRecord->commandBuffer ||
                   pendingRecord->testOnlyAllowNullCommandBuffer) &&
                  core::metalqueue::hasExactRedundantFixedCompletionSources(
                      *pendingRecord, pendingSources.span(),
                      carriedSessionSources);
              bool completionPreflightValid =
                  freshPlanningFrontier
                  ? !pendingRecord.has_value() && !pendingSession &&
                        pendingSources.empty()
                  : pendingRecord.has_value() && pendingSession &&
                        (pendingRecord->explicitCompletionSourceSpan().empty() ||
                         exactRedundantFixedSources) &&
                        (pendingRecord->commandBuffer ||
                         pendingRecord->testOnlyAllowNullCommandBuffer);
              std::array<bool, framegraph::kMaxMultiSourcePlanningSources>
                  sourceSeen{};
              std::array<std::size_t,
                         framegraph::kMaxMultiSourcePlanningSources>
                  sourceCoveredCommands{};
              for (std::size_t runIndex = 0;
                   runIndex < replayRuns.runs.size(); ++runIndex) {
                const auto& run = replayRuns.runs[runIndex];
                if (!completionPreflightValid ||
                    run.retainedSourceIndex >= count ||
                    run.sourceFragmentCount == 0u ||
                    run.sourceFragmentOrdinal >= run.sourceFragmentCount ||
                    run.transactionFragmentCount != replayRuns.runs.size() ||
                    run.transactionFragmentOrdinal != runIndex) {
                  completionPreflightValid = false;
                  break;
                }
                sourceSeen[run.retainedSourceIndex] = true;
                const auto& resolved =
                    resolvedWindow[run.retainedSourceIndex];
                completionPreflightValid =
                    run.commandBegin <= resolved.payload.commandCount() &&
                    run.commandCount != 0u &&
                    run.commandCount <=
                        resolved.payload.commandCount() - run.commandBegin &&
                    sourceCoveredCommands[run.retainedSourceIndex] <=
                        resolved.payload.commandCount() - run.commandCount;
                if (completionPreflightValid) {
                  sourceCoveredCommands[run.retainedSourceIndex] +=
                      run.commandCount;
                }
              }
              for (std::size_t i = 0;
                   i < count && completionPreflightValid; ++i) {
                completionPreflightValid = sourceSeen[i] &&
                    sourceCoveredCommands[i] ==
                        resolvedWindow[i].payload.commandCount() &&
                    stagedSources.append(selectedCompletionSources[i]);
              }
              if (completionPreflightValid && freshPlanningFrontier) {
                pendingSession = encoders::makeEncodeChunkSession();
              }
              const auto sessionSources = pendingSession
                  ? encoders::encodeChunkSessionSources(*pendingSession)
                  : std::span<const QueueCompletionSource>{};
              completionPreflightValid = completionPreflightValid &&
                  sessionSources.size() == pendingSources.size() &&
                  core::metalqueue::queueCompletionSourceSpansExactlyEqual(
                      sessionSources, pendingSources.span());
              if (!completionPreflightValid) {
                if (freshPlanningFrontier) {
                  pendingSession.reset();
                }
                perf::countCpuReadyMultiSourceWindowFallback(
                    perf::CpuReadyMultiSourceFallbackReason::Carrier);
              } else if (!encoders::appendEncodeChunkSessionSources(
                             *pendingSession,
                             std::span<const QueueCompletionSource>(
                             selectedCompletionSources.data(), count))) {
                // appendEncodeChunkSessionSources stages internally, so this
                // rejection precedes both completion mutation and encoding.
                perf::countCpuReadyMultiSourceWindowFallback(
                    perf::CpuReadyMultiSourceFallbackReason::
                        CompletionSource);
                if (freshPlanningFrontier) {
                  pendingSession.reset();
                }
              } else {
                if (exactRedundantFixedSources) {
                  // Natural source-local merging may materialize the same
                  // complete FIFO list in record, queue, and session owners.
                  // Before the first fragment effect, normalize that exact
                  // duplicate to the canonical empty record form. The
                  // session finalizer republishes it after all folds.
                  pendingRecord->clearFixedCompletionSources();
                }
                const auto& replayLookaheadSources =
                    *preplannedReplayLookaheadSources;
                // The session list is encode-thread-local and append is an
                // all-or-nothing staged assignment. The exact FIFO list was
                // validated above before mutation, so registration cannot
                // introduce a new post-effect failure mode.
                perf::recordCpuReadyMultiSourceCompletionRegistration(
                    count, true);

                std::optional<QueueSubmissionRecord> fragmentCarrier;
                if (pendingRecord) {
                  fragmentCarrier = std::move(*pendingRecord);
                  pendingRecord.reset();
                }
                std::array<
                    encoders::PreRegisteredEncodeSourceFragmentAccumulator,
                    framegraph::kMaxMultiSourcePlanningSources>
                    sourceFragmentAccumulators{};
                for (std::size_t runIndex = 0;
                     runIndex < replayRuns.runs.size(); ++runIndex) {
                  const auto& run = replayRuns.runs[runIndex];
                  const std::size_t replaySourceIndex =
                      run.retainedSourceIndex;
                  const auto& replaySource =
                      resolvedWindow[replaySourceIndex];
                  encoders::EncodeChunkOptions options{};
                  const obj_handle_t injectedCommandBuffer =
                      fragmentCarrier
                      ? fragmentCarrier->commandBuffer.handle
                      : NULL_OBJECT_HANDLE;
                  if (fragmentCarrier) {
                    options.commandBuffer =
                        std::move(fragmentCarrier->commandBuffer);
                  }
                  options.allowInjectedCommandBufferMidChunkCommits = true;
                  options.session = pendingSession.get();
                  options.deferSessionFinalization = true;
                  options.partitionSource = replaySource.source;
                  options.preRegisteredFragment =
                      encoders::PreRegisteredEncodeChunkFragment{
                          .commandBegin = run.commandBegin,
                          .commandCount = run.commandCount,
                          .sourceFragmentOrdinal =
                              run.sourceFragmentOrdinal,
                          .sourceFragmentCount = run.sourceFragmentCount,
                          .transactionFragmentOrdinal =
                              run.transactionFragmentOrdinal,
                          .transactionFragmentCount =
                              run.transactionFragmentCount,
                      };
                  options.preRegisteredSourceAccumulator =
                      &sourceFragmentAccumulators[replaySourceIndex];
                  options.skipBackendPlanning = true;
                  if (replayWindowAttributionEnabled) {
                    options.replayWindow = encoders::ReplayWindowProvenance{
                        .disposition = encoders::ReplayWindowDisposition::
                            PlannedComposite,
                        .windowId =
                            resolvedWindow[0].metadata.sourceOrdinal,
                        .sourceIndex = static_cast<std::uint32_t>(
                            replaySourceIndex),
                        .sourceCount = static_cast<std::uint32_t>(count),
                    };
                  }

                  auto submission = encodeWithBorrows(
                      lock,
                      std::span<const ResolvedPublishedSource>(
                          replayLookaheadSources.data(),
                          replayLookaheadSources.size())
                          .subspan(runIndex),
                      replaySource, std::move(options), true);
                  if (!submission.has_value()) {
                    perf::countCpuReadyMultiSourcePostEffectFatal(
                        perf::CpuReadyMultiSourceFatalReason::
                            EncodeReturnedNull);
                    abortCpuReadySessionFailOpen(
                        "multi-source fragment encode returned null");
                  }
                  const auto fragmentOwner = submission->pipelineOwner;
                  if (!stagedSources.setOwner(replaySource.seqId,
                                              fragmentOwner) ||
                      !pendingSession ||
                      !encoders::setEncodeChunkSessionSourceOwner(
                          *pendingSession, replaySource.seqId,
                          fragmentOwner)) {
                    abortCpuReadySessionFailOpen(
                        "multi-source fragment owner attribution");
                  }
                  if (fragmentCarrier) {
                    if (!core::metalqueue::foldEncodedSessionFragmentCarrier(
                            *submission, *fragmentCarrier,
                            injectedCommandBuffer)) {
                      perf::countCpuReadyMultiSourcePostEffectFatal(
                          perf::CpuReadyMultiSourceFatalReason::CarrierFold);
                      abortCpuReadySessionFailOpen(
                          "multi-source fragment carrier fold");
                    }
                  }
                  fragmentCarrier = std::move(*submission);
                }

                if (!fragmentCarrier) {
                  abortCpuReadySessionFailOpen(
                      "multi-source fragment transaction empty");
                }

                // Fragment calls deliberately bypass per-source backend
                // planning. Observe once only after every Metal effect and
                // carrier fold succeeded, so stale tentative retries cannot
                // duplicate the observer's frame state. The retained source
                // payloads stay live while the scheduling mutex is released.
                const bool observed =
                    queueLifecycle_.visitRepresentedSourceBorrows(
                        lock,
                        std::span<const ResolvedPublishedSource>(
                            resolvedWindow.data(), count),
                        [&](const SynchronousSourceBorrowBatch& sourceBorrows)
                            noexcept {
                          try {
                            ScopedQueueUnlock queueUnlocked(lock);
                            backend_->observeMultiSourceSessionReplay(
                                pool_, sourceBorrows);
                            return true;
                          } catch (...) {
                            return false;
                          }
                        });
                if (!observed) {
                  abortCpuReadySessionFailOpen(
                      "multi-source observation borrow");
                }
                pendingRecord = std::move(*fragmentCarrier);
                pendingSources = stagedSources;
                pendingAdmission = windowPreflight.stagedAdmission;
                const QueueCompletionSource& fifoTail =
                    stagedSources.entries[stagedSources.count - 1u];
                pendingRecord->seqId = fifoTail.seqId;
                pendingRecord->slotIndex = fifoTail.slotIndex;
                pendingRecord->diagnostics.seqId = fifoTail.seqId;
                pendingRecord->diagnostics.slotIndex = fifoTail.slotIndex;
                for (std::size_t i = 0; i < count; ++i) {
                  perf::countCpuReadySessionHeadAppended(
                      resolvedWindow[i].payload.isArena());
                }
                for (std::size_t i = 0; i < count; ++i) {
                  QueueCompletionSource retained =
                      selectedCompletionSources[i];
                  (void)tryRetireEncodedSource(
                      i, resolvedWindow[i], admissionWindow[i], retained);
                }
                recordCapacityLeaseUsed();
                perf::countCpuReadyMultiSourceWindowPlanned(
                    count, replayPlan.commands.size(),
                    replayRuns.runs.size());
                continue;
              }
            }
          }
        }
      }
    }
multi_source_window_complete:
    const bool naturalSourceLocalFallback =
        sourceLocalReplayDisposition == encoders::ReplayWindowDisposition::
            NaturalAfterMergeFallback;
    const bool permutationSourceLocalFallback =
        sourceLocalReplayDisposition == encoders::ReplayWindowDisposition::
            PermutationRejectedFallback;
    if (naturalSourceLocalFallback || permutationSourceLocalFallback) {
      perf::countCpuReadyMultiSourceSourceLocalFallbackStarted(
          naturalSourceLocalFallback
              ? perf::CpuReadyMultiSourceSourceLocalFallback::
                    NaturalAfterMerge
              : perf::CpuReadyMultiSourceSourceLocalFallback::
                    PermutationRejected,
          count);
    }
    encoders::ActiveSeedMergeTicketContext activeSeedMergeTicket{};
    std::span<const encoders::ActiveSeedMergeTargetWitness>
        activeSeedMergeTargets{};
    if (replayWindowAttributionEnabled && naturalSourceLocalFallback &&
        preplannedReplayPlan.has_value()) {
      const auto& diagnostics = preplannedReplayPlan->diagnostics;
      const bool seedMerge = diagnostics.merge ==
          framegraph::MultiSourceMergeDiagnostic::SeedMerged;
      if (seedMerge && revalidatedActiveRender.has_value() &&
          revalidatedActiveRender->complete &&
          revalidatedActiveRenderInstance.has_value()) {
        bool orderedWitnessSet = sourceLocalReplayWindowId != 0u;
        for (std::size_t i = 0u;
             i < diagnostics.activeSeedMergeWitnesses.size() &&
                 orderedWitnessSet;
             ++i) {
          const auto& witness = diagnostics.activeSeedMergeWitnesses[i];
          orderedWitnessSet = witness.valid() &&
              witness.retainedSourceIndex < count &&
              witness.commandIndex <
                  resolvedWindow[witness.retainedSourceIndex]
                      .payload.commandCount();
          if (i != 0u) {
            const auto& previous =
                diagnostics.activeSeedMergeWitnesses[i - 1u];
            orderedWitnessSet = orderedWitnessSet &&
                (previous.retainedSourceIndex < witness.retainedSourceIndex ||
                 (previous.retainedSourceIndex ==
                      witness.retainedSourceIndex &&
                  previous.commandIndex < witness.commandIndex));
          }
        }
        const bool witnessSetValid =
            !diagnostics.activeSeedMergeWitnessOverflow &&
            !diagnostics.activeSeedMergeWitnessMismatch &&
            orderedWitnessSet &&
            !diagnostics.activeSeedMergeWitnesses.empty() &&
            diagnostics.activeSeedMergeWitnesses.size() ==
                diagnostics.activeSeedMergeCount;
        if (witnessSetValid) {
          activeSeedMergeTicket = encoders::ActiveSeedMergeTicketContext{
              .seed = *revalidatedActiveRenderInstance,
              .windowId = sourceLocalReplayWindowId,
              .sourceCount = static_cast<std::uint32_t>(count),
          };
          activeSeedMergeTargets =
              diagnostics.activeSeedMergeWitnesses;
        } else if (diagnostics.activeSeedMergeWitnessOverflow) {
          perf::countActiveSeedMergeWitnessOverflow(
              std::max<std::uint64_t>(
                  diagnostics.activeSeedMergeCount, 1u));
        } else {
          perf::countActiveSeedMergeWitnessMismatch(
              std::max<std::uint64_t>(
                  diagnostics.activeSeedMergeCount, 1u));
        }
      } else if (seedMerge && revalidatedActiveRender.has_value() &&
                 revalidatedActiveRender->complete) {
        if (activeInstanceRevalidation == encoders::
                ActiveSeedInstanceRevalidation::Stale) {
          perf::countActiveSeedInstanceStale();
        } else {
          perf::countActiveSeedInstanceUnavailable();
        }
      }
    }
    for (std::size_t sourceIndex = 0; sourceIndex < count; ++sourceIndex) {
      if (!lock.owns_lock()) {
        lock.lock();
      }
      const ReadySlotSnapshot source = scratch[sourceIndex];
      const auto commonSource =
          queueLifecycle_.resolveRepresentedSource(source);
      if (!commonSource.valid()) {
        queueLifecycle_.poisonTapeFailureLocked();
        return;
      }
      const auto sourceAdmission = admissionCandidateFor(commonSource);
      if (!pendingRecord.has_value()) {
        const auto headAdmission = render::classifySessionAdmissionDetailed(
            {}, sourceAdmission, admissionLimits);
        if (headAdmission.decision ==
            render::SessionAdmissionDecision::ProcessCandidateIsolated) {
          perf::countCpuReadySessionDisposition(
              perf::CpuReadySessionDisposition::Isolated);
          auto isolationReason =
              perf::CpuReadySessionIsolationReason::Other;
          if (headAdmission.limitingDimension ==
              render::SessionCapacityDimension::Bytes) {
            isolationReason =
                perf::CpuReadySessionIsolationReason::CapacityBytes;
          } else if (sourceAdmission.semantic.hasPresent()) {
            isolationReason = perf::CpuReadySessionIsolationReason::Present;
          }
          perf::countCpuReadySessionIsolation(isolationReason);
        } else if (headAdmission.decision ==
                   render::SessionAdmissionDecision::RejectInvalid) {
          perf::countCpuReadySessionDisposition(
              perf::CpuReadySessionDisposition::Invalid);
        }
      }
      const bool sourceIsArena = commonSource.payload.isArena();
      const bool sourceIsEarlyPrefix =
          commonSource.payload.legacyPayload() &&
          commonSource.payload.legacyPayload()->publishReason ==
              perf::ChunkPublishReason::EarlyPrefix;
      const bool sourceHasFinalPresentTail =
          render::sessionSourceHasFinalPresentTail(commonSource.payload);
      const bool sourceCanStartSession =
          render::sessionSourceCanBeHead(commonSource.payload);
      bool sourceCanAppendToPending =
          render::sessionSourceCanAppendToPending(
              commonSource.payload, static_cast<bool>(pendingSession));
      const auto earlySourceAction =
          core::metalqueue::decideCpuReadyEarlySessionAction(
              pendingEarlyPrefix, stop_, /*orderedRelease=*/false,
              /*hasNextSource=*/true, sourceCanAppendToPending,
              sourceHasFinalPresentTail);
      if (earlySourceAction ==
          core::metalqueue::CpuReadyEarlySessionAction::FailPreEffect) {
        abandonEarlyPrefixPreEffectLocked();
        return;
      }

      if (pendingRecord.has_value() && !sourceCanAppendToPending) {
        perf::countCpuReadySessionReleased(
            perf::CpuReadySessionReleaseReason::NonAppendable);
        if (!submitPendingRecordLocked(
                lock, encoders::SessionFinalizeCause::Independent)) {
          abortCpuReadySessionFailOpen("non-appendable source");
        }
        if (!lock.owns_lock()) {
          lock.lock();
        }
      }
      if (pendingRecord.has_value() &&
          render::sessionShouldSubmitBeforeInitializerWait(
              sourceCanAppendToPending,
              pendingSession &&
                  encoders::encodeChunkSessionHasActiveRender(
                      *pendingSession),
              initializer_ &&
                  initializer_->hasPendingUploadsUnlocked())) {
        if (pendingEarlyPrefix) {
          abandonEarlyPrefixPreEffectLocked();
          return;
        }
        perf::countCpuReadySessionReleased(
            perf::CpuReadySessionReleaseReason::InitializerWait);
        if (!submitPendingRecordLocked(
                lock, encoders::SessionFinalizeCause::Initializer)) {
          abortCpuReadySessionFailOpen("initializer wait boundary");
        }
        if (!lock.owns_lock()) {
          lock.lock();
        }
        sourceCanAppendToPending = false;
      }

      bool appendToPending =
          pendingRecord.has_value() && sourceCanAppendToPending;
      if (appendToPending) {
        const QueueCompletionSource candidate = selectedCompletionSourcesValid
            ? selectedCompletionSources[sourceIndex]
            : core::metalqueue::completionSourceForReadySlot(source);
        const bool queueSourcesCanAppend =
            pendingSources.canAppend(candidate);
        const bool sessionSourcesCanAppend =
            !pendingSession ||
            encoders::canAppendEncodeChunkSessionSource(
                *pendingSession, candidate);
        if (!queueSourcesCanAppend || !sessionSourcesCanAppend) {
          if (pendingEarlyPrefix) {
            abandonEarlyPrefixPreEffectLocked();
            return;
          }
          perf::countCpuReadySessionReleased(
              perf::CpuReadySessionReleaseReason::FailPath);
          if (!submitPendingRecordLocked(
                  lock, encoders::SessionFinalizeCause::FailOrOther)) {
            abortCpuReadySessionFailOpen("session source preflight failed");
          }
          if (!lock.owns_lock()) {
            lock.lock();
          }
          appendToPending = false;
        }
      }
      // An early-prefix source is normally required to start the one
      // unsubmitted experimental carrier.  If an ordinary compatible session
      // is already pending, however, this source has not had any Metal effect
      // yet and can safely fold into that carrier.  Treating this as a join
      // failure would stop the queue merely because the producer published the
      // prefix after an older FIFO source.
      const bool foldEarlyPrefixIntoPending =
          sourceIsEarlyPrefix && !pendingEarlyPrefix && appendToPending;
      bool startPending = !pendingRecord.has_value() && sourceCanStartSession &&
                          capacityLeaseState.lease().valid();
      if (sourceIsEarlyPrefix && !foldEarlyPrefixIntoPending &&
          (!startPending || pendingEarlyPrefix)) {
        // No Metal encoding has occurred for this source yet. A published
        // experimental prefix may never take the standalone path.
        if (pendingEarlyPrefix) {
          abandonEarlyPrefixPreEffectLocked();
          return;
        }
        perf::countCpuReadyEarlyPrefixSessionJoinFailedPreEffect();
        requestSchedulingStopLocked();
        return;
      }
      QueueCompletionSource appendRetained{};
      bool appendRetainedValid = false;
      if (appendToPending) {
        if (!retainedSourceForIndex(lock, sourceIndex, source,
                                    appendRetained)) {
          if (pendingEarlyPrefix) {
            abandonEarlyPrefixPreEffectLocked();
            return;
          }
          perf::countCpuReadySessionReleased(
              perf::CpuReadySessionReleaseReason::FailPath);
          if (!submitPendingRecordLocked(
                  lock, encoders::SessionFinalizeCause::FailOrOther)) {
            abortCpuReadySessionFailOpen("append source retain failed");
          }
          if (!lock.owns_lock()) {
            lock.lock();
          }
          appendToPending = false;
          startPending = false;
        } else {
          appendRetainedValid = true;
        }
      }
      QueueCompletionSource startRetained{};
      bool startRetainedValid = false;
      if (startPending) {
        if (!retainedSourceForIndex(lock, sourceIndex, source,
                                    startRetained)) {
          startPending = false;
        } else {
          startRetainedValid = true;
        }
      }
      if (sourceIsEarlyPrefix && !foldEarlyPrefixIntoPending &&
          !startPending) {
        perf::countCpuReadyEarlyPrefixSessionJoinFailedPreEffect();
        requestSchedulingStopLocked();
        return;
      }

      encoders::EncodeChunkOptions options{};
      if (replayWindowAttributionEnabled) {
        options.replayWindow = encoders::ReplayWindowProvenance{
            .disposition = sourceLocalReplayDisposition,
            .windowId = sourceLocalReplayWindowId,
            .sourceIndex = static_cast<std::uint32_t>(sourceIndex),
            .sourceCount = static_cast<std::uint32_t>(count),
        };
      }
      if (activeSeedMergeTicket.valid() &&
          !activeSeedMergeTargets.empty()) {
        std::size_t first = 0u;
        while (first < activeSeedMergeTargets.size() &&
               activeSeedMergeTargets[first].retainedSourceIndex <
                   sourceIndex) {
          ++first;
        }
        std::size_t end = first;
        while (end < activeSeedMergeTargets.size() &&
               activeSeedMergeTargets[end].retainedSourceIndex ==
                   sourceIndex) {
          ++end;
        }
        if (end != first) {
          options.activeSeedMergeTicket = activeSeedMergeTicket;
          options.activeSeedMergeTargets =
              activeSeedMergeTargets.subspan(first, end - first);
        }
      }
      options.partitionSource = core::CpuReadyTape::SourceRef{
          .id = source.sourceId,
          .storage = source.storage,
      };
      if (appendToPending || startPending) {
        // Source boundaries must not become extra command buffers, but
        // semantic pass/barrier boundaries keep the normal mid-chunk
        // chain policy inside the carried CB.
        options.allowInjectedCommandBufferMidChunkCommits = true;
        if (!pendingSession) {
          pendingSession = encoders::makeEncodeChunkSession();
        }
        options.session = pendingSession.get();
        options.deferSessionFinalization = !sourceHasFinalPresentTail;
        options.sessionSource =
            appendToPending ? appendRetained : startRetained;
      }
      if (appendToPending) {
        options.commandBuffer = pendingRecord->commandBuffer;
      }

      std::optional<QueueSubmissionRecord> submission;
      {
        std::array<ResolvedPublishedSource, kCommandChunkCount>
            resolvedLookahead{};
        const std::size_t resolvedCount =
            selectedPrefixStartsSession && selectedCompletionSourcesValid
            ? count - sourceIndex
            : 1u;
        for (std::size_t i = 0; i < resolvedCount; ++i) {
          resolvedLookahead[i] = queueLifecycle_.resolveRepresentedSource(
              scratch[sourceIndex + i]);
          if (!resolvedLookahead[i].valid()) {
            queueLifecycle_.poisonTapeFailureLocked();
            return;
          }
        }
        submission = encodeWithBorrows(
            lock,
            std::span<const ResolvedPublishedSource>(resolvedLookahead.data(),
                                                     resolvedCount),
            resolvedLookahead[0], std::move(options), resolvedCount > 1u);
      }
      if (!submission.has_value()) {
        if (sourceIsEarlyPrefix && !foldEarlyPrefixIntoPending) {
          perf::countCpuReadyEarlyPrefixSessionJoinFailedPreEffect();
          pendingSession.reset();
          releaseCapacityLease();
          requestSchedulingStopLocked();
          return;
        }
        if (appendToPending) {
          if (pendingEarlyPrefix) {
            abandonEarlyPrefixPreEffectLocked();
            return;
          }
          perf::countCpuReadySessionReleased(
              perf::CpuReadySessionReleaseReason::FailPath);
          if (!submitPendingRecordLocked(
                  lock, encoders::SessionFinalizeCause::FailOrOther)) {
            abortCpuReadySessionFailOpen("append encode returned null");
          }
          if (!lock.owns_lock()) {
            lock.lock();
          }
        }
        if (startPending) {
          pendingSession.reset();
        }
        unchargeSelectedCapacity(sourceIndex);
        completeInlineSnapshot(lock, source);
        continue;
      }

      // EncodeChunk decides the owner only after replay has run. Refresh every
      // carried ledger that already contains this source before any merge or
      // receipt replacement; the record-wide tail owner is insufficient for a
      // mixed serial/selected-parallel session.
      if (appendToPending || startPending) {
        const auto owner = submission->pipelineOwner;
        const std::uint64_t sourceSeqId = source.seqId;
        if (appendRetainedValid) {
          appendRetained.pipelineOwner = owner;
        }
        if (startRetainedValid) {
          startRetained.pipelineOwner = owner;
        }
        // The queue list contains the current source only after the merge;
        // appendRetained carries its owner into that staged list below.
        if (pendingRecord && !appendToPending &&
            !pendingSources.setOwner(sourceSeqId, owner)) {
          abortCpuReadySessionFailOpen(
              "carried session owner attribution missing queue source");
        }
        if (pendingSession &&
            !encoders::setEncodeChunkSessionSourceOwner(
                *pendingSession, sourceSeqId, owner)) {
          abortCpuReadySessionFailOpen(
              "carried session owner attribution missing session source");
        }
      }

      if (startPending) {
        if (!startRetainedValid) {
          if (!core::metalqueue::assignOrValidateSingleCompletionSource(
                  *submission, source)) {
            abortCpuReadySessionFailOpen(
                "unretained session completion source retention failed");
          }
          if (!submitRecordLocked(lock, *submission)) {
            abortCpuReadySessionFailOpen("unretained session submit failed");
          }
          pendingSession.reset();
          unchargeSelectedCapacity(sourceIndex);
          continue;
        }
        pendingSources.clear();
        pendingEarlyPrefix = sourceIsEarlyPrefix;
        if (!pendingSources.append(startRetained)) {
          DXMT_ASSERT(false && "first encoded session source must be valid");
          pendingRecord = std::move(*submission);
          if (pendingEarlyPrefix) {
            abandonEarlyPrefixPreEffectLocked();
            return;
          }
          perf::countCpuReadySessionReleased(
              perf::CpuReadySessionReleaseReason::FailPath);
          if (!submitPendingRecordLocked(
                  lock, encoders::SessionFinalizeCause::FailOrOther)) {
            abortCpuReadySessionFailOpen("initial session source rejected");
          }
          continue;
        }
        pendingRecord = std::move(*submission);
        pendingAdmission = {};
        if (!render::appendSessionAdmission(
                pendingAdmission, sourceAdmission, admissionLimits)) {
          if (pendingEarlyPrefix) {
            abandonEarlyPrefixPreEffectLocked();
            return;
          }
          perf::countCpuReadySessionReleased(
              perf::CpuReadySessionReleaseReason::FailPath);
          if (!submitPendingRecordLocked(
                  lock, encoders::SessionFinalizeCause::FailOrOther)) {
            abortCpuReadySessionFailOpen(
                "initial session admission state rejected");
          }
          continue;
        }
        recordCapacityLeaseUsed();
        (void)tryRetireEncodedSource(
            sourceIndex, commonSource, sourceAdmission, startRetained);
        perf::countCpuReadySessionPendingStarted();
        if (sourceIsEarlyPrefix) {
          perf::countCpuReadyEarlyPrefixSessionParked();
        }
        continue;
      }

      if (appendToPending) {
        const std::uint64_t appendChainLength =
            std::max<std::uint64_t>(
                1, submission->commandBufferChainLength);
        const bool appendCommittedPendingTail =
            appendRetainedValid &&
            pendingRecord.has_value() &&
            pendingRecord->commandBuffer &&
            submission->commandBuffer &&
            pendingRecord->commandBuffer.handle !=
                submission->commandBuffer.handle &&
            appendChainLength > 1u;
        EncodeSessionSourceList mergedSources;
        const bool merged =
            appendRetainedValid &&
            core::metalqueue::mergeEncodedPendingTailSubmission(
                *submission, *pendingRecord, pendingSources.span(),
                appendRetained, appendCommittedPendingTail,
                sourceHasFinalPresentTail ? nullptr : &mergedSources);
        if (!merged) {
          if (pendingEarlyPrefix) {
            abandonEarlyPrefixPreEffectLocked();
            return;
          }
          perf::countCpuReadySessionReleased(
              perf::CpuReadySessionReleaseReason::FailPath);
          if (!appendRetainedValid || !submission->commandBuffer) {
            if (!submitPendingRecordLocked(
                    lock, encoders::SessionFinalizeCause::FailOrOther)) {
              abortCpuReadySessionFailOpen("merge failed before append submit");
            }
            if (!lock.owns_lock()) {
              lock.lock();
            }
            if (submission->commandBuffer) {
              if (!core::metalqueue::assignOrValidateSingleCompletionSource(
                      *submission, source)) {
                abortCpuReadySessionFailOpen(
                    "merge fallback completion source retention failed");
              }
              if (!submitRecordLocked(lock, *submission)) {
                abortCpuReadySessionFailOpen("merge fallback submit failed");
              }
            } else {
              completeInlineSnapshot(lock, source);
            }
            continue;
          }
          abortCpuReadySessionFailOpen("pending append metadata merge failed");
        }

        pendingRecord.reset();
        pendingSources.clear();
        if (!sourceHasFinalPresentTail) {
          if (!render::appendSessionAdmission(
                  pendingAdmission, sourceAdmission, admissionLimits)) {
            abortCpuReadySessionFailOpen(
                "appended session admission state rejected");
          }
          recordCapacityLeaseUsed();
          pendingSources = mergedSources;
          pendingRecord = std::move(*submission);
          (void)tryRetireEncodedSource(
              sourceIndex, commonSource, sourceAdmission, appendRetained);
          perf::countCpuReadySessionHeadAppended(sourceIsArena);
          continue;
        }

        if (pendingSession &&
            !encoders::retainEncodeChunkSessionUntilSubmissionComplete(
                std::move(pendingSession), *submission)) {
          if (pendingEarlyPrefix) {
            perf::countCpuReadyEarlyPrefixSessionJoinFailedPreEffect();
          }
          abortCpuReadySessionFailOpen("merged tail session retain failed");
        }
        if (!submitRecordLocked(lock, *submission)) {
          if (pendingEarlyPrefix) {
            perf::countCpuReadyEarlyPrefixSessionJoinFailedPreEffect();
          }
          abortCpuReadySessionFailOpen("merged tail submit failed");
        }
        perf::countCpuReadySessionTailSubmitted();
        if (pendingEarlyPrefix) {
          perf::countCpuReadyEarlyPrefixSessionJoined();
        }
        pendingEarlyPrefix = false;
        pendingSession.reset();
        pendingAdmission = {};
        releaseCapacityLease();
        continue;
      }

      if (!core::metalqueue::assignOrValidateSingleCompletionSource(
              *submission, source)) {
        abortCpuReadySessionFailOpen(
            "standalone completion source retention failed");
      }
      if (!submitRecordLocked(lock, *submission)) {
        abortCpuReadySessionFailOpen("standalone submit failed");
      }
      unchargeSelectedCapacity(sourceIndex);
    }
    if (naturalSourceLocalFallback || permutationSourceLocalFallback) {
      perf::countCpuReadyMultiSourceSourceLocalFallbackCompleted(
          naturalSourceLocalFallback
              ? perf::CpuReadyMultiSourceSourceLocalFallback::
                    NaturalAfterMerge
              : perf::CpuReadyMultiSourceSourceLocalFallback::
                    PermutationRejected);
    }
  }
}

}  // namespace dxmt9
