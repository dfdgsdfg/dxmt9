#include "dxmt9_command_queue.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_resource_initializer.hpp"
#include "render/encode_session_admission.hpp"
#include "render/open_cb_carrier.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace dxmt9 {

namespace {

[[noreturn]] void abortCpuReadySessionFailOpen(const char* reason) {
  std::fprintf(stderr,
               "[dxmt9-queue] fatal: encoded CPU-ready session pending work "
               "could not fail-open by submit (%s)\n",
               reason ? reason : "unknown");
  std::abort();
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
  const std::uint32_t maxSessionSources = static_cast<std::uint32_t>(
      std::min<std::size_t>(core::metalqueue::kMaxEncodeSessionSources,
                            tapeValues.highWaterReady - 1u));
  const render::EncodeSessionLimits admissionLimits{
      .maxSources = maxSessionSources,
      .maxPages = static_cast<std::uint32_t>(sessionPages),
      .maxBytes = sessionBytes,
      .maxDraws = std::numeric_limits<std::uint32_t>::max(),
      .maxCommandBuffers = maxSessionSources,
  };
  const render::SessionCapacityPolicy capacityPolicy{
      .highWater = {
          .sources = tapeValues.highWaterReady,
          .pages = tapeValues.highWaterPages,
          .bytes = tapeValues.pageSize * tapeValues.highWaterPages,
          .draws = maxDrawCredits * 2u,
          .payloadBlocks = tapeValues.sourceSlotCount *
                           core::kMaxArenaSourcePayloadSegments,
          .readyEntries = tapeValues.highWaterReady,
          .retentionEntries = tapeValues.highWaterSources,
          .allocatorTickets = tapeValues.highWaterSources,
          .commandBuffers = tapeValues.highWaterReady,
      },
      .maxSession = {
          .sources = admissionLimits.maxSources,
          .pages = admissionLimits.maxPages,
          .bytes = admissionLimits.maxBytes,
          .draws = admissionLimits.maxDraws,
          .payloadBlocks = static_cast<std::uint64_t>(maxSessionSources) *
                           core::kMaxArenaSourcePayloadSegments,
          .readyEntries = maxSessionSources,
          .retentionEntries = maxSessionSources,
          .allocatorTickets = maxSessionSources,
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
    const bool released = capacityLeaseState.release(generation);
    DXMT_ASSERT(released);
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
      [&admissionKey](const ResolvedPublishedSource& source) {
        return render::SessionAdmissionCandidate{
            .key = admissionKey,
            .semantic = source.semantic,
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
          std::unique_lock<std::mutex>& lock) {
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
                ctx, *pendingSession, *pendingRecord);
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
          std::unique_lock<std::mutex>& lock) {
        if (!pendingRecord.has_value()) {
          return false;
        }
        if (!finalizePendingSessionForSubmitLocked(lock)) {
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
                   !submitPendingRecordLocked(lock)) {
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
      if (!submitPendingRecordLocked(lock)) {
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
        if (!submitPendingRecordLocked(lock)) {
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

    bool blockedByPendingCompatibility = false;
    bool leaseDenied = false;
    bool selectionAcquiredLease = false;
    std::uint64_t selectionLeaseGeneration = 0;
    std::array<render::SessionCapacityVector, kCommandChunkCount>
        selectedCapacityCharges{};
    std::array<bool, kCommandChunkCount> selectedCapacityCharged{};
    render::SessionCapacityDimension blockedCapDimension =
        render::SessionCapacityDimension::None;
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
                  const auto unavailable = cpuReadyTape_.unleasedCapacity();
                  const render::SessionCapacityVector unavailableCapacity{
                      .sources = unavailable.sources,
                      .pages = unavailable.pages,
                      .payloadBlocks = unavailable.payloadBlocks,
                      .retentionEntries = unavailable.retentionEntries,
                      .allocatorTickets = unavailable.allocatorTickets,
                  };
                  if (!capacityLeaseState.acquire(
                          capacityPolicy, unavailableCapacity, capacity)) {
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
                if (candidate.semantic.hasPresent()) {
                  break;
                }
              }
              return selected;
            });
    if (count == 0) {
      if (leaseDenied && !pendingRecord.has_value()) {
        // Older submitted/Writing residency may delay lease acquisition. The
        // finish/replay paths wake this CV after reclaim or publication; do not
        // spin and do not turn the occupancy observation into a release.
        if (stop_) {
          return;
        }
        encodeCv_.wait(lock);
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
            switch (blockedCapDimension) {
            case render::SessionCapacityDimension::Sources:
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
        if (!submitPendingRecordLocked(lock)) {
          abortCpuReadySessionFailOpen(
              "semantic compatibility release fallback");
        }
        continue;
      }
      if (pendingRecord.has_value()) {
        if (!submitPendingRecordLocked(lock)) {
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
    for (std::size_t i = 0; i < count; ++i) {
      sessionReleaseCoveredSeqId_ =
          std::max(sessionReleaseCoveredSeqId_, scratch[i].seqId);
      sessionReleaseCoveredRawOrdinal_ = std::max(
          sessionReleaseCoveredRawOrdinal_,
          scratch[i].metadata.rawOrdinal);
    }

    // H185 invariant (kind-neutral): validate + retain the whole selected
    // source prefix before exposing cross-source sessionLookaheadSources.
    bool selectedPrefixStartsSession = false;
    {
      const auto first = queueLifecycle_.resolveRepresentedSource(scratch[0]);
      if (!first.valid()) {
        queueLifecycle_.poisonTapeFailure();
        return;
      }
      selectedPrefixStartsSession =
          render::openCbCarrierSourceCanBeSessionHead(first.payload);
    }
    std::array<QueueCompletionSource, kCommandChunkCount>
        selectedCompletionSources{};
    bool selectedCompletionSourcesValid = false;
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
        const auto headDisposition = render::classifySessionAdmission(
            {}, sourceAdmission, admissionLimits);
        if (headDisposition ==
            render::SessionAdmissionDecision::ProcessCandidateIsolated) {
          perf::countCpuReadySessionDisposition(
              perf::CpuReadySessionDisposition::Isolated);
        } else if (headDisposition ==
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
        if (!submitPendingRecordLocked(lock)) {
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
        if (!submitPendingRecordLocked(lock)) {
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
          if (!submitPendingRecordLocked(lock)) {
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
          if (!submitPendingRecordLocked(lock)) {
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
          if (!submitPendingRecordLocked(lock)) {
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
          if (!submitPendingRecordLocked(lock)) {
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
          if (!submitPendingRecordLocked(lock)) {
            abortCpuReadySessionFailOpen(
                "initial session admission state rejected");
          }
          continue;
        }
        recordCapacityLeaseUsed();
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
            if (!submitPendingRecordLocked(lock)) {
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
  }
}

}  // namespace dxmt9
