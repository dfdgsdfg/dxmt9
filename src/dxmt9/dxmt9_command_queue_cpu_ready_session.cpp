#include "dxmt9_command_queue.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_resource_initializer.hpp"
#include "render/open_cb_carrier.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
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
  //     failures, producer sequence waits, arena admission pressure,
  //     compatibility control/Tape or inflight-cap writer pressure,
  //     initializer-wait boundaries, and shutdown drain.
  using core::metalqueue::EncodeSessionSourceList;
  using core::metalqueue::QueueCompletionSource;
  using core::metalqueue::QueueSubmissionRecord;
  using core::metalqueue::ReadySlotSnapshot;
  using core::metalqueue::ResolvedPublishedSource;

  std::array<ReadySlotSnapshot, kCommandChunkCount> scratch{};
  std::optional<QueueSubmissionRecord> pendingRecord;
  EncodeSessionSourceList pendingSources;
  encoders::EncodeChunkSession pendingSession;

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
                *pendingSession)) {
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
      [&pendingRecord, &pendingSources, &pendingSession,
       &finalizePendingSessionForSubmitLocked, &submitRecordLocked](
          std::unique_lock<std::mutex>& lock) {
        if (!pendingRecord.has_value()) {
          return false;
        }
        if (!finalizePendingSessionForSubmitLocked(lock)) {
          return false;
        }
        if (!submitRecordLocked(lock, *pendingRecord)) {
          return false;
        }
        pendingRecord.reset();
        pendingSources.clear();
        pendingSession.reset();
        return true;
      };

  // Ordered pressure fences observable under the queue mutex. Each names a
  // producer/replay-worker wait whose resolution can require completion of
  // work held by the pending unsubmitted session.
  auto pendingPressureReleaseReason =
      [this, &pendingRecord]()
      -> std::optional<perf::CpuReadySessionReleaseReason> {
    if (!pendingRecord.has_value()) {
      return std::nullopt;
    }
    if (queueLifecycle_.producerSequenceWaitActive()) {
      return perf::CpuReadySessionReleaseReason::ProducerWait;
    }
    if (arenaAdmissionWaiterCount_.load(std::memory_order_acquire) > 0) {
      return perf::CpuReadySessionReleaseReason::AdmissionPressure;
    }
    if (queueLifecycle_.producerWriterPressureActive()) {
      return perf::CpuReadySessionReleaseReason::WriterPressure;
    }
    return std::nullopt;
  };

  while (true) {
    std::unique_lock lock(mutex_);

    if (const auto reason = pendingPressureReleaseReason()) {
      if (!submitPendingRecordLocked(lock)) {
        abortCpuReadySessionFailOpen("ordered pressure fence release");
      }
      perf::countCpuReadySessionReleased(*reason);
      if (!lock.owns_lock()) {
        lock.lock();
      }
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
      // shutdown, or an ordered pressure fence re-evaluates the pending work.
      encodeCv_.wait(lock, [this] {
        return stop_ || !cpuReadyTape_.readyEmpty() ||
               queueLifecycle_.producerSequenceWaitActive() ||
               arenaAdmissionWaiterCount_.load(std::memory_order_acquire) >
                   0 ||
               queueLifecycle_.producerWriterPressureActive();
      });
      continue;
    }

    const std::size_t count =
        queueLifecycle_.dequeueReadySlotBatchPrefix(
            lock,
            std::span<ReadySlotSnapshot>(scratch),
            [](std::span<const ResolvedPublishedSource> candidates) noexcept {
              return render::selectCpuReadySessionBatchPrefix(candidates);
            });
    if (count == 0) {
      if (pendingRecord.has_value()) {
        if (!submitPendingRecordLocked(lock)) {
          abortCpuReadySessionFailOpen("queue drained");
        }
        perf::countCpuReadySessionReleased(
            perf::CpuReadySessionReleaseReason::Drain);
      }
      return;
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
      bool startPending = !pendingRecord.has_value() && sourceCanStartSession;
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
    }
  }
}

}  // namespace dxmt9
