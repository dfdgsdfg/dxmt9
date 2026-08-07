#include "dxmt9_command_queue.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_resource_initializer.hpp"
#include "framegraph/fg_multi_source_planner.hpp"
#include "render/backend_interface.hpp"
#include "render/encode_session_admission.hpp"
#include "render/open_cb_carrier.hpp"

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
  // Tape-gated session join lane (DXMT9_CPU_READY_TAPE). Reuses the H229
  // carrier mechanics — one pending QueueSubmissionRecord owning the open
  // Metal command buffer plus the retained per-source completion list, an
  // EncodeChunkSession carried across source boundaries, and one submitted
  // tail expanding to per-source seqId completion — but with two deliberate
  // differences (see runCpuReadySessionEncodeLoop declaration):
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
  using core::metalqueue::SessionReleaseAckResult;
  using core::metalqueue::SessionReleaseAction;
  using core::metalqueue::SessionReleaseCompletion;

  std::array<ReadySlotSnapshot, kCommandChunkCount> scratch{};
  std::optional<QueueSubmissionRecord> pendingRecord;
  EncodeSessionSourceList pendingSources;
  encoders::EncodeChunkSession pendingSession;
  render::EncodeSessionAdmissionState pendingAdmission{};
  render::SessionCapacityLeaseState capacityLeaseState{};
  bool exactReplaySingleSource = false;
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
          const ResolvedPublishedSource& source) {
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
        releaseCapacityLease();
        return true;
      };

  auto processNextReleaseLocked =
      [this, &pendingRecord, &pendingSession,
       &submitPendingRecordLocked](std::unique_lock<std::mutex>& lock) {
        const auto release = sessionReleaseState_.peekNext();
        if (!release ||
            release->event.fenceSeqId > sessionReleaseCoveredSeqId_) {
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
        return stop_ || !cpuReadyTape_.readyEmpty() ||
               sessionReleaseState_.hasPending() ||
               queueLifecycle_.producerSequenceWaitActive();
      });
      continue;
    }

    // A fresh one-source Ready frontier cannot expose an A|B + A replay
    // window to the bounded planner. When that exact frontier has one already-
    // reserved ordered-tail Writing successor, retain the whole Ready source
    // tentatively until the successor publishes. An active session consumes
    // its Ready head immediately; R15 showed that parking it provided no
    // cross-source return and failed strict locality.
    const bool freshRetainedHeadFrontier =
        !pendingRecord.has_value() && !pendingSession &&
        !pendingAdmission.valid() && !capacityLeaseState.lease().valid();
    if (!exactReplaySingleSource &&
        freshRetainedHeadFrontier &&
        cpuReadyTape_.readyCount() == 1u &&
        !sessionReleaseState_.hasPending() &&
        !queueLifecycle_.producerSequenceWaitActive() &&
        arenaAdmissionWaiterCount_.load(std::memory_order_acquire) == 0u &&
        !queueLifecycle_.producerWriterPressureActive() &&
        !(initializer_ && initializer_->hasPendingUploadsUnlocked())) {
      perf::countCpuReadyRetainedHeadAttempt();
      core::CpuReadyTape::SourceRef writingSuccessor{};
      const std::size_t retainedCount =
          queueLifecycle_.reserveReadySlotBatchPrefix(
              lock, std::span<ReadySlotSnapshot>(scratch.data(), 1u),
              [&](std::span<const ResolvedPublishedSource> candidates)
                  noexcept {
                if (candidates.size() != 1u) {
                  return std::size_t{0};
                }
                const auto& candidate = candidates.front();
                const auto admission = admissionCandidateFor(candidate);
                if (candidate.semantic.hasPresent() ||
                    !render::openCbCarrierSourceCanBeSessionHead(
                        candidate.payload) ||
                    render::classifySessionAdmission(
                        render::EncodeSessionAdmissionState{},
                        admission,
                        admissionLimits) !=
                        render::SessionAdmissionDecision::Admit) {
                  return std::size_t{0};
                }
                const auto capacity =
                    cpuReadyTape_.leaseAcquisitionCapacitySnapshot();
                if (!capacity.valid ||
                    !capacity.orderedTailWritingSuccessor.has_value() ||
                    !capacity.orderedTailWritingSuccessor->valid()) {
                  return std::size_t{0};
                }
                writingSuccessor =
                    capacity.orderedTailWritingSuccessor->source;
                return std::size_t{1};
              });
      if (retainedCount == 1u) {
        perf::countCpuReadyRetainedHeadHeld();
        const auto waitStarted = perf::enabled()
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        std::uint64_t observedCapacityProgress =
            queueLifecycle_.cpuReadyCapacityProgressGeneration();
        std::optional<core::CpuReadyTape::ReadyEntry> readySuccessor;

        auto successorReady = [&]() {
          std::array<core::CpuReadyTape::ReadyEntry, 1> ready{};
          if (cpuReadyTape_.copyReadyPrefix(ready) == 1u &&
              ready.front().source == writingSuccessor) {
            readySuccessor = ready.front();
            return true;
          }
          readySuccessor.reset();
          return false;
        };
        auto writingSuccessorStillExact = [&]() {
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
            return stop_ || !cpuReadyTape_.readyEmpty() ||
                sessionReleaseState_.hasPending() ||
                queueLifecycle_.producerSequenceWaitActive() ||
                arenaAdmissionWaiterCount_.load(
                    std::memory_order_acquire) != 0u ||
                queueLifecycle_.producerWriterPressureActive() ||
                (initializer_ &&
                 initializer_->hasPendingUploadsUnlocked()) ||
                queueLifecycle_.cpuReadyCapacityProgressGeneration() !=
                    observedCapacityProgress;
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
              queueLifecycle_.poisonTapeFailure();
              return;
            }
            if (fallback.has_value()) {
              perf::countCpuReadyRetainedHeadFallback(*fallback);
              exactReplaySingleSource = true;
            } else {
              DXMT_ASSERT(readySuccessor.has_value());
              perf::countCpuReadyRetainedHeadSuccessorReady();
              exactReplaySingleSource = false;
            }
            break;
          }
          observedCapacityProgress =
              queueLifecycle_.cpuReadyCapacityProgressGeneration();
        }
        continue;
      }
    }

    bool blockedByPendingCompatibility = false;
    bool leaseDenied = false;
    bool invalidCapacitySnapshot = false;
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
                std::span<const ResolvedPublishedSource> candidates) noexcept {
              render::EncodeSessionAdmissionState planned = pendingAdmission;
              std::size_t selected = 0;
              for (const auto& candidate : candidates) {
                if (releaseFence &&
                    ((releaseFence->event.fenceSeqId != 0 &&
                      candidate.seqId > releaseFence->event.fenceSeqId) ||
                     (releaseFence->event.fenceRawOrdinal != 0 &&
                      candidate.metadata.rawOrdinal != 0 &&
                      candidate.metadata.rawOrdinal >
                          releaseFence->event.fenceRawOrdinal))) {
                  break;
                }
                if (exactReplaySingleSource) {
                  return std::size_t{1};
                }
                if (selected >= 2u && candidate.semantic.hasPresent()) {
                  // Present is a hard replay-window/logical-pass boundary,
                  // but a final Present tail may still close and submit the
                  // same EncodeSession/CB. Leave it Ready for the next natural
                  // iteration without invalidating the complete pre-Present
                  // planning prefix.
                  break;
                }

                const auto admission = admissionCandidateFor(candidate);
                const auto result = render::classifySessionAdmissionDetailed(
                    planned, admission, admissionLimits);
                const auto decision = result.decision;
                const bool hasPlannedSession = planned.valid();
                const bool exactCompatible = hasPlannedSession
                    ? render::openCbCarrierSourceCanAppendToPending(
                          candidate.payload, true)
                    : render::openCbCarrierSourceCanBeSessionHead(
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
                    return std::size_t{1};
                  }
                  break;
                }
                if (!render::appendSessionAdmission(
                        planned, admission, admissionLimits)) {
                  if (selected == 0 && pendingRecord.has_value()) {
                    blockedByPendingCompatibility = true;
                    blockedReleaseReason =
                        core::metalqueue::SessionReleaseReason::SessionCap;
                  }
                  break;
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
                    break;
                  }
                  if (!capacityLeaseState.acquire(
                          capacityPolicy, *unavailable, capacity)) {
                    perf::countCpuReadySessionLeaseDenied();
                    leaseDenied = true;
                    break;
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
                if (planned.valid() && selected >=
                        framegraph::kMaxMultiSourcePlanningSources) {
                  // Keep the represented transaction within the bounded
                  // planner's complete proof window. A ninth compatible
                  // source remains Ready and is reconsidered after this
                  // planned prefix installs its carried state.
                  break;
                }
                if (candidate.semantic.hasPresent()) {
                  break;
                }
              }
              return selected;
            });
    if (count == 0) {
      if (invalidCapacitySnapshot) {
        // Invalid Tape identity/arithmetic is structural corruption, not a
        // capacity transition. Poison synchronously so it cannot enter the
        // generation wait and wedge without a possible releasing event.
        queueLifecycle_.poisonTapeFailure();
        return;
      }
      if (leaseDenied && !pendingRecord.has_value()) {
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
        ++cpuReadyCapacityWaiterCount_;
        encodeCv_.wait(lock, [this, observedCapacityProgress] {
          return stop_ ||
              queueLifecycle_.cpuReadyCapacityProgressGeneration() !=
                  observedCapacityProgress;
        });
        DXMT_ASSERT(cpuReadyCapacityWaiterCount_ > 0);
        --cpuReadyCapacityWaiterCount_;
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
        queueLifecycle_.poisonTapeFailure();
        return;
      }
      exactReplaySingleSource = true;
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
            render::openCbCarrierSourceCanBeSessionHead(
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
      const bool captureBoundary =
          (pendingRecord &&
           (pendingRecord->metalCapture.has_value() ||
            pendingRecord->metalCaptureAlreadyStarted)) ||
          metalCaptureEnabled();
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
        lock.unlock();
        auto replayPlan = backend_->planMultiSourceSessionReplay(
            pool_, std::span<const ResolvedPublishedSource>(
                       resolvedWindow.data(), count),
            replayFrontier);
        std::optional<framegraph::MultiSourceReplayRuns> replayRuns;
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
            // Materialize replay-order ranges while the represented payloads
            // are pinned and scheduling is unlocked. Each synchronous
            // fragment call receives its exact current..tail suffix; a
            // repeated source therefore appears once per disjoint run.
            std::vector<ResolvedPublishedSource> replayLookaheadSources;
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
            if (replayLookaheadSources.size() == replayRuns->runs.size()) {
              preplannedReplayLookaheadSources =
                  std::move(replayLookaheadSources);
            }
          }
        }
        lock.lock();

        bool snapshotStillValid =
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
            ((pendingRecord &&
              (pendingRecord->metalCapture.has_value() ||
               pendingRecord->metalCaptureAlreadyStarted)) ||
             metalCaptureEnabled()) == captureBoundary;
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
            queueLifecycle_.poisonTapeFailure();
            return;
          }
          if (testOnlyPauseAfterStaleMultiSourcePlannerRestore_) {
            testOnlyPauseAfterStaleMultiSourcePlannerRestore_ = false;
            testOnlyPausedAfterStaleMultiSourcePlannerRestore_ = true;
            sessionReleaseCv_.notify_all();
            sessionReleaseCv_.wait(lock, [this] {
              return !testOnlyPausedAfterStaleMultiSourcePlannerRestore_;
            });
          }
          exactReplaySingleSource = false;
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
        queueLifecycle_.poisonTapeFailure();
        return;
      }
      exactReplaySingleSource = true;
      continue;
    }
    exactReplaySingleSource = false;
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
    // source prefix before exposing cross-source sessionLookaheadSources.
    if (!selectedPrefixStartsSession) {
      const auto first = queueLifecycle_.resolveRepresentedSource(scratch[0]);
      if (!first.valid()) {
        queueLifecycle_.poisonTapeFailure();
        return;
      }
      selectedPrefixStartsSession =
          (pendingRecord.has_value() && pendingSession) ||
          render::openCbCarrierSourceCanBeSessionHead(first.payload);
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
                  options.sessionLookaheadSources =
                      std::span<const ResolvedPublishedSource>(
                          replayLookaheadSources.data(),
                          replayLookaheadSources.size())
                          .subspan(runIndex);
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

                  lock.unlock();
                  auto submission = encodeCpuReadySessionSource(
                      replaySource, std::move(options));
                  lock.lock();
                  if (!submission.has_value()) {
                    perf::countCpuReadyMultiSourcePostEffectFatal(
                        perf::CpuReadyMultiSourceFatalReason::
                            EncodeReturnedNull);
                    abortCpuReadySessionFailOpen(
                        "multi-source fragment encode returned null");
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
                lock.unlock();
                backend_->observeMultiSourceSessionReplay(
                    pool_, std::span<const ResolvedPublishedSource>(
                               resolvedWindow.data(), count));
                lock.lock();
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
        queueLifecycle_.poisonTapeFailure();
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
      const bool sourceHasFinalPresentTail =
          render::openCbCarrierSourceHasFinalPresentTail(commonSource.payload);
      const bool sourceCanStartSession =
          render::openCbCarrierSourceCanBeSessionHead(commonSource.payload);
      bool sourceCanAppendToPending =
          render::openCbCarrierSourceCanAppendToPending(
              commonSource.payload, static_cast<bool>(pendingSession));

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
          render::openCbCarrierShouldSubmitBeforeInitializerWait(
              sourceCanAppendToPending,
              pendingSession &&
                  encoders::encodeChunkSessionHasActiveRender(
                      *pendingSession),
              initializer_ &&
                  initializer_->hasPendingUploadsUnlocked())) {
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
      bool startPending = !pendingRecord.has_value() && sourceCanStartSession &&
                          capacityLeaseState.lease().valid();
      QueueCompletionSource appendRetained{};
      bool appendRetainedValid = false;
      if (appendToPending) {
        if (!retainedSourceForIndex(lock, sourceIndex, source,
                                    appendRetained)) {
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
            queueLifecycle_.poisonTapeFailure();
            return;
          }
        }
        if (resolvedCount > 1u) {
          options.sessionLookaheadSources =
              std::span<const ResolvedPublishedSource>(
                  resolvedLookahead.data(), resolvedCount);
        }
        lock.unlock();
        submission = encodeCpuReadySessionSource(
            resolvedLookahead[0], std::move(options));
      }
      lock.lock();

      if (!submission.has_value()) {
        if (appendToPending) {
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
        if (!pendingSources.append(startRetained)) {
          DXMT_ASSERT(false && "first encoded session source must be valid");
          pendingRecord = std::move(*submission);
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
          abortCpuReadySessionFailOpen("merged tail session retain failed");
        }
        if (!submitRecordLocked(lock, *submission)) {
          abortCpuReadySessionFailOpen("merged tail submit failed");
        }
        perf::countCpuReadySessionTailSubmitted();
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
