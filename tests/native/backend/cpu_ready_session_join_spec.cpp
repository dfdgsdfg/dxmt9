// Tape-gated session join (DXMT9_CPU_READY_TAPE) — deterministic coverage
// for source-kind-neutral EncodeSession integration:
//   * the pure session-source policy classifies Legacy and Arena sources;
//   * those predicates admit multi-Arena and mixed Legacy/Arena FIFO prefixes;
//   * the production encodeChunk payload overload carries one EncodeSession
//     and one injected command buffer across consecutive Arena sources with
//     no artificial submission/session boundary at the source edge;
//   * the real queue seam (replayRawChunk publication -> neutral batch
//     dequeue -> whole-prefix retention -> one submitted record -> completion
//     -> ordered reclaim) attributes FIFO completion per source for a mixed
//     Arena/Legacy/Arena prefix through a single submission;
//   * the production runCpuReadySessionEncodeLoop consumes multi-Arena and
//     mixed prefixes, carries one pending session, and releases it at both a
//     deterministic shutdown drain and a fixed session-cap fence while a live
//     compatibility producer still has room to publish the cap candidate;
//   * a standalone Clear+Present followed by a Direct draw leaves the
//     production coordinator in the exact denied-lease wait, then resumes on
//     both GPU reclaim and inline reclaim without writer pressure or shutdown.

#include "device_c_common.hpp"
#include "device_c_cpu_ready_plan.hpp"
#include "device_c_replay_offload.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "d3d9_pe_wire_handle.hpp"

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_encode_session.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/dxmt9/dxmt9_ring_arena.hpp"
#include "../../../src/dxmt9/dxmt9_source_payload.hpp"
#include "../../../src/dxmt9/render/backend_interface.hpp"
#include "../../../src/dxmt9/render/deferred_terminal_suffix.hpp"
#include "../../../src/dxmt9/render/framegraph_backend.hpp"
#include "../../../src/dxmt9/render/session_source_policy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using OrderedControlReplayObserver = void (*)(
    void* userdata, std::uint32_t phase, std::uint32_t recordIndex,
    std::uint32_t recordType, std::int32_t result);
extern "C" void dxmt9_test_set_ordered_control_replay_observer(
    void* userdata, OrderedControlReplayObserver observer);

namespace dxmt9 {

std::size_t selectSessionSourceBatchPrefix(
    std::span<const core::metalqueue::ResolvedPublishedSource> candidates) {
  std::size_t sessionHeadPrefix = 0;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const core::SourcePayloadView payload = candidates[i].payload;
    if (!payload.valid()) {
      return sessionHeadPrefix;
    }
    if (render::sessionSourceHasFinalPresentTail(payload)) {
      return i == 0u ? 0u : i + 1u;
    }
    if (!render::sessionSourceCanBeHead(payload)) {
      return sessionHeadPrefix;
    }
    ++sessionHeadPrefix;
  }
  return sessionHeadPrefix;
}

struct CommandQueueArenaLeaseTestAccess {
  struct BatchResult {
    std::size_t dequeued = 0;
    std::size_t retained = 0;
    bool submitted = false;
    bool completed = false;
    std::size_t finishIterations = 0;
    std::vector<std::uint64_t> seqIds;
    std::vector<bool> arenaKinds;
  };

  static std::size_t readyCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.readyCount();
  }

  static std::size_t residentCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.residentCount();
  }

  static core::CpuReadyTape::Stats tapeStats(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.stats();
  }

  static core::CpuReadyTape::LeaseAcquisitionCapacitySnapshot
  leaseAcquisitionCapacitySnapshot(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.leaseAcquisitionCapacitySnapshot();
  }

  static std::uint64_t completedSeqId(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.completedSeqIdLocked();
  }

  static std::size_t capacityWaiterCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyCapacityWaiterCount_;
  }

  static std::uint32_t arenaAdmissionWaiterCount(CommandQueue& queue) {
    return queue.arenaAdmissionWaiterCount_.load(std::memory_order_acquire);
  }

  static void enableSchedulingWaitObservation(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlySchedulingWaitObservationEnabled_ = true;
    queue.testOnlyArenaAdmissionWaitEntries_ = 0;
    queue.testOnlyFirstLeaseWaitEntries_ = 0;
  }

  static bool waitForArenaAdmissionWaitEntries(
      CommandQueue& queue, std::uint64_t expected) {
    std::unique_lock lock(queue.mutex_);
    return queue.sessionReleaseCv_.wait_for(
        lock, std::chrono::seconds(2), [&] {
          return queue.testOnlyArenaAdmissionWaitEntries_ >= expected;
        });
  }

  static bool waitForFirstLeaseWaitEntries(
      CommandQueue& queue, std::uint64_t expected) {
    std::unique_lock lock(queue.mutex_);
    return queue.sessionReleaseCv_.wait_for(
        lock, std::chrono::seconds(2), [&] {
          return queue.testOnlyFirstLeaseWaitEntries_ >= expected;
        });
  }

  static void pauseAfterFirstLeaseRetry(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyPauseAfterFirstLeaseRetry_ = true;
    queue.testOnlyPausedAfterFirstLeaseRetry_ = false;
  }

  static bool waitForFirstLeaseRetryPause(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    return queue.sessionReleaseCv_.wait_for(
        lock, std::chrono::seconds(2), [&] {
          return queue.testOnlyPausedAfterFirstLeaseRetry_;
        });
  }

  static void resumeAfterFirstLeaseRetry(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyPausedAfterFirstLeaseRetry_ = false;
    queue.sessionReleaseCv_.notify_all();
  }

  static bool waitForArenaAdmission(
      CommandQueue& queue,
      const core::ArenaSourcePayloadLayout& layout) {
    return queue.waitForCpuReadyArenaAdmission(layout);
  }

  static core::CpuReadyTape::ReserveProbe probeArenaAdmission(
      CommandQueue& queue,
      const core::ArenaSourcePayloadLayout& layout) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.probeArenaReserve(layout);
  }

  static std::uint64_t capacityProgressGeneration(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.queueLifecycle_.cpuReadyCapacityProgressGeneration();
  }

  static bool readySourceIsArena(
      CommandQueue& queue,
      const core::metalqueue::QueueCompletionSource& source) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.payloadKind(
               source.source.id, source.source.storage,
               core::CpuReadyTape::State::Ready) ==
        core::CpuReadyTape::PayloadKind::Arena;
  }

  static bool sourceIsTentative(
      CommandQueue& queue,
      const core::metalqueue::QueueCompletionSource& source) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.state(source.source.id,
                                     source.source.storage) ==
        core::CpuReadyTape::State::TentativeRepresented;
  }

  static bool stopped(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.stop_;
  }

  static bool writerPressureActive(CommandQueue& queue) {
    return queue.queueLifecycle_.producerWriterPressureActive();
  }

  static std::vector<core::metalqueue::QueueCompletionSource>
  snapshotReadyCompletionSources(CommandQueue& queue) {
    using namespace core::metalqueue;
    std::lock_guard lock(queue.mutex_);
    std::array<core::CpuReadyTape::ReadyEntry, kCommandChunkCount> ready{};
    const std::size_t count = queue.cpuReadyTape_.copyReadyPrefix(
        std::span<core::CpuReadyTape::ReadyEntry>(
            ready.data(), queue.cpuReadyTape_.readyCount()));
    std::vector<QueueCompletionSource> sources;
    sources.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& entry = ready[i];
      if (entry.controlIndex >= queue.slots_.size()) {
        return {};
      }
      const auto& control = queue.slots_[entry.controlIndex];
      const auto payload = queue.cpuReadyTape_.resolveSourcePayload(
          entry.source.id, entry.source.storage,
          core::CpuReadyTape::State::Ready);
      if (!payload.valid() || control.state != core::ChunkSlot::State::Pending ||
          control.sourceId != entry.source.id ||
          control.storage != entry.source.storage ||
          control.seqId != entry.seqId) {
        return {};
      }
      sources.push_back(QueueCompletionSource{
          .source = entry.source,
          .slotIndex = entry.controlIndex,
          .seqId = entry.seqId,
          .hasPresent = payload.presentRecordCount() != 0,
          .commandBegin = 0,
          .commandCount = payload.commandCount(),
      });
    }
    return sources;
  }

  static void installBackend(
      CommandQueue& queue,
      std::unique_ptr<render::IRenderBackend> backend) {
    queue.backend_ = std::move(backend);
  }

  static void installDrawRecorder(
      CommandQueue& queue, encoders::EncodeDrawRecorder* recorder) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyDrawRecorder_ = recorder;
  }

  static void stopAndRunCpuReadySessionEncodeLoop(CommandQueue& queue) {
    {
      std::lock_guard lock(queue.mutex_);
      queue.stop_ = true;
    }
    queue.runCpuReadySessionEncodeLoop({});
  }

  static void runCpuReadySessionEncodeLoop(CommandQueue& queue) {
    queue.runCpuReadySessionEncodeLoop({});
  }

  static void enableCpuReadySessionReleaseLane(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.cpuReadySessionLaneEnabled_ = true;
  }

  static void restoreNextTentativePreflightAndReturn(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyRestoreNextCpuReadySessionPreflight_ = true;
  }

  static void pauseAfterStaleMultiSourcePlannerRestore(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyPauseAfterStaleMultiSourcePlannerRestore_ = true;
  }

  static bool pausedAfterStaleMultiSourcePlannerRestore(
      CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.testOnlyPausedAfterStaleMultiSourcePlannerRestore_;
  }

  static void resumeAfterStaleMultiSourcePlannerRestore(
      CommandQueue& queue) {
    {
      std::lock_guard lock(queue.mutex_);
      queue.testOnlyPausedAfterStaleMultiSourcePlannerRestore_ = false;
    }
    queue.sessionReleaseCv_.notify_all();
  }

  static void overrideLiveActiveRenderInstance(
      CommandQueue& queue, std::uint64_t seqId,
      std::uint64_t encoderIndex) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyOverrideLiveActiveRenderInstance_ = true;
    queue.testOnlyLiveActiveRenderSeqId_ = seqId;
    queue.testOnlyLiveActiveRenderEncoderIndex_ = encoderIndex;
  }

  static void pauseAfterNextSessionReleaseAck(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyPauseAfterNextSessionReleaseAck_ = true;
  }

  static bool pausedAfterSessionReleaseAck(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.testOnlyPausedAfterSessionReleaseAck_;
  }

  static void resumeAfterSessionReleaseAck(CommandQueue& queue) {
    {
      std::lock_guard lock(queue.mutex_);
      queue.testOnlyPausedAfterSessionReleaseAck_ = false;
    }
    queue.sessionReleaseCv_.notify_all();
  }

  static bool hasPendingSessionRelease(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.sessionReleaseState_.hasPending();
  }

  static std::uint64_t acknowledgedSessionReleaseOrdinal(
      CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.sessionReleaseState_.acknowledgedOrdinal();
  }

  static void requestStop(CommandQueue& queue) {
    {
      std::lock_guard lock(queue.mutex_);
      queue.stop_ = true;
    }
    queue.encodeCv_.notify_all();
    queue.writeCv_.notify_all();
  }

  static bool postOrderedSubmit(CommandQueue& queue,
                                std::uint64_t rawOrdinal,
                                std::uint64_t seqId) {
    bool accepted = false;
    {
      std::lock_guard lock(queue.mutex_);
      accepted = queue.sessionReleaseState_.tryPostOrdered(
          core::metalqueue::SessionReleaseReason::ExplicitFlush,
          core::metalqueue::SessionReleaseAction::SubmitSession,
          rawOrdinal, seqId).accepted();
    }
    queue.encodeCv_.notify_one();
    return accepted;
  }

  // Drive the real compatibility writer-acquire path, then discard the empty
  // reservation once capacity becomes available. This is the production wait
  // that a Legacy Present/query source reaches after Direct Arena sources have
  // occupied every control shell.
  static bool ensureAndAbortEmptyWriter(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    if (!queue.queueLifecycle_.ensureWriterSlot(lock, kMaxQueuedChunks)) {
      return false;
    }
    (void)queue.queueLifecycle_.commitCurrentChunk(lock, kMaxQueuedChunks);
    return !queue.writingSlot_.has_value();
  }

  static bool allSourcesSubmitted(
      CommandQueue& queue,
      std::span<const core::metalqueue::QueueCompletionSource> sources) {
    std::lock_guard lock(queue.mutex_);
    for (const auto& source : sources) {
      if (queue.cpuReadyTape_.state(source.source.id,
                                    source.source.storage) !=
              core::CpuReadyTape::State::GPU &&
          !queue.queueLifecycle_.postEncodeReceiptForTest(
              source.seqId,
              core::metalqueue::PostEncodeReceiptState::Submitted)) {
        return false;
      }
    }
    return true;
  }

  static bool hasActivePostEncodeReceipt(CommandQueue& queue,
                                         std::uint64_t seqId) {
    std::lock_guard lock(queue.mutex_);
    return queue.queueLifecycle_.postEncodeReceiptForTest(
               seqId,
               core::metalqueue::PostEncodeReceiptState::Active)
        .has_value();
  }

  static std::size_t completeAndFinish(
      CommandQueue& queue,
      std::span<const core::metalqueue::QueueCompletionSource> sources,
      std::function<void()> completionCallback = {},
      std::shared_ptr<void> retainedPayload = {}) {
    using namespace core::metalqueue;
    std::vector<QueueCompletionSource> completionSources(
        sources.begin(), sources.end());
    for (auto& source : completionSources) {
      const auto receipt = queue.queueLifecycle_.postEncodeReceiptForTest(
          source.seqId, PostEncodeReceiptState::Submitted);
      if (receipt) {
        source.source = {};
        source.receipt = *receipt;
      }
    }
    QueueLifecycleController::PendingCompletion pending{};
    pending.slotIndex = completionSources.back().slotIndex;
    pending.seqId = completionSources.back().seqId;
    if (!pending.assignFixedCompletionSources(completionSources)) {
      return 0;
    }
    if (completionCallback) {
      pending.completionCallbacks.push_back(std::move(completionCallback));
    }
    if (retainedPayload) {
      pending.retainedPayloads.push_back(std::move(retainedPayload));
    }
    queue.queueLifecycle_.enqueuePendingCompletionForTest(std::move(pending));
    if (!queue.queueLifecycle_.processOnePendingCompletion()) {
      return 0;
    }

    std::size_t iterations = 0;
    std::unique_lock lock(queue.mutex_);
    while (iterations < sources.size() &&
           queue.queueLifecycle_.runFinishIteration(lock)) {
      ++iterations;
    }
    return iterations;
  }

  // Publishes the current legacy writing slot through the compatibility
  // publication path (the lane used for oversize/TriangleFan/Present chunks
  // when the Tape gate is on).
  static bool publishLegacyWritingSlot(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    return queue.queueLifecycle_.commitCurrentChunk(lock, kMaxQueuedChunks);
  }

  static bool publishLegacyWritingSlotWithAdmissionPressure(
      CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    queue.arenaAdmissionWaiterCount_.store(1u, std::memory_order_release);
    return queue.queueLifecycle_.commitCurrentChunk(lock, kMaxQueuedChunks);
  }

  static void clearSyntheticAdmissionPressure(CommandQueue& queue) {
    queue.arenaAdmissionWaiterCount_.store(0u, std::memory_order_release);
    queue.encodeCv_.notify_all();
  }

  static bool publishLegacyClearPresent(CommandQueue& queue) {
    queue.submitClear(core::ClearDesc{});
    {
      std::unique_lock lock(queue.mutex_);
      queue.queueLifecycle_.presentAndCommit(
          lock, kMaxQueuedChunks, core::SwapDesc{}, core::Handle{0x92});
    }
    const auto ready = snapshotReadyCompletionSources(queue);
    return !ready.empty() && ready.back().hasPresent;
  }

  static std::optional<core::metalqueue::ReadySlotSnapshot>
  representReadyHead(CommandQueue& queue) {
    core::metalqueue::ReadySlotSnapshot source{};
    std::unique_lock lock(queue.mutex_);
    if (!queue.queueLifecycle_.dequeueReadySlot(lock, source)) {
      return std::nullopt;
    }
    return source;
  }

  static bool completeInline(
      CommandQueue& queue,
      const core::metalqueue::ReadySlotSnapshot& source) {
    std::unique_lock lock(queue.mutex_);
    return queue.queueLifecycle_.completeInlineChunk(
        lock, source.slotIndex, source.seqId);
  }

  static bool finishOne(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    return queue.queueLifecycle_.runFinishIteration(lock);
  }

  // Dequeues one maximal source-kind-neutral FIFO prefix, retains the whole
  // prefix, submits ONE record covering every source, then completes and
  // reclaims it, recording per-source FIFO identity and payload kind.
  static BatchResult consumeBatch(CommandQueue& queue,
                                  std::size_t maxSources) {
    using namespace core::metalqueue;
    BatchResult result{};
    std::array<ReadySlotSnapshot, kCommandChunkCount> scratch{};
    std::array<QueueCompletionSource, kCommandChunkCount> retained{};
    {
      std::unique_lock lock(queue.mutex_);
      const std::size_t count = queue.queueLifecycle_.dequeueReadySlotBatchPrefix(
          lock,
          std::span<ReadySlotSnapshot>(scratch.data(),
                                       std::min(maxSources, scratch.size())),
          [](std::span<const ResolvedPublishedSource> candidates) noexcept {
            return selectSessionSourceBatchPrefix(candidates);
          });
      result.dequeued = count;
      if (count == 0) {
        return result;
      }
      for (std::size_t i = 0; i < count; ++i) {
        const auto resolved =
            queue.queueLifecycle_.resolveRepresentedSource(scratch[i]);
        if (!resolved.valid()) {
          return result;
        }
        result.seqIds.push_back(resolved.seqId);
        result.arenaKinds.push_back(resolved.payload.isArena());
      }
      result.retained = queue.queueLifecycle_.retainEncodedSourcesForPendingTail(
          lock,
          std::span<const ReadySlotSnapshot>(scratch.data(), count),
          std::span<QueueCompletionSource>(retained.data(), count));
      if (result.retained != count) {
        return result;
      }

      QueueSubmissionRecord record{};
      record.testOnlyAllowNullCommandBuffer = true;
      record.slotIndex = retained[count - 1].slotIndex;
      record.seqId = retained[count - 1].seqId;
      if (!record.assignFixedCompletionSources(
              std::span<const QueueCompletionSource>(retained.data(), count))) {
        return result;
      }
      result.submitted =
          queue.queueLifecycle_.submitEncodedSubmission(lock, record);
      if (!result.submitted) {
        return result;
      }
    }

    core::metalqueue::QueueLifecycleController::PendingCompletion pending{};
    pending.slotIndex = retained[result.retained - 1].slotIndex;
    pending.seqId = retained[result.retained - 1].seqId;
    if (!pending.assignFixedCompletionSources(
            std::span<const QueueCompletionSource>(retained.data(),
                                                    result.retained))) {
      return result;
    }
    queue.queueLifecycle_.enqueuePendingCompletionForTest(std::move(pending));
    result.completed = queue.queueLifecycle_.processOnePendingCompletion();
    if (!result.completed) {
      return result;
    }
    {
      std::unique_lock lock(queue.mutex_);
      for (std::size_t i = 0; i < result.retained; ++i) {
        if (!queue.queueLifecycle_.runFinishIteration(lock)) {
          break;
        }
        ++result.finishIterations;
        if (queue.cpuReadyTape_.residentCount() == 0) {
          break;
        }
      }
    }
    return result;
  }
};

}  // namespace dxmt9

namespace {

using namespace dxmt9::core;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

template <typename WmtType>
WMT::Reference<WmtType> retainedToken(const char* label) {
  auto owner = WMT::MakeString(label, WMTUTF8StringEncoding);
  return WMT::Reference<WmtType>(WmtType{owner.handle});
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

struct RecordSpec {
  std::uint32_t type = 0;
  std::vector<std::byte> payload;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
};

struct WireFixture {
  std::vector<std::byte> bytes;
  dxmt9::d3d9::CommandChunkEnvelope envelope{};
};

WireFixture makeWireFixture(std::span<const RecordSpec> specs) {
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  std::vector<std::byte> payload;
  for (const auto& spec : specs) {
    const auto* rule = dxmt9::d3d9::recordRule(spec.type);
    check(rule != nullptr, "session join fixture record must be known");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back({
        .type = spec.type,
        .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
    });
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
  }

  D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  header.handleTableOffset = static_cast<std::uint32_t>(alignUp(
      header.recordTableOffset + records.size() * sizeof(records[0]),
      alignof(D9CCommandChunkWireHandleEntry)));
  header.payloadArenaOffset = static_cast<std::uint32_t>(alignUp(
      header.handleTableOffset + handles.size() * sizeof(handles[0]),
      alignof(std::uint32_t)));

  WireFixture fixture;
  fixture.bytes.resize(header.payloadArenaOffset + payload.size());
  std::memcpy(fixture.bytes.data(), &header, sizeof(header));
  std::memcpy(fixture.bytes.data() + header.recordTableOffset,
              records.data(), records.size() * sizeof(records[0]));
  if (!handles.empty()) {
    std::memcpy(fixture.bytes.data() + header.handleTableOffset,
                handles.data(), handles.size() * sizeof(handles[0]));
  }
  if (!payload.empty()) {
    std::memcpy(fixture.bytes.data() + header.payloadArenaOffset,
                payload.data(), payload.size());
  }
  fixture.envelope = {
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
  };
  return fixture;
}

RecordSpec clearRecord() {
  return {
      .type = D9C_COMMAND_RECORD_CLEAR,
      .payload = bytesOf(D9CCommandChunkWireClear{
          .flags = 1u,
          .colorARGB = 0xff654321u,
          .z = 1.0f,
          .stencil = 0,
          .rectCount = 0,
          .rectOffset = sizeof(D9CCommandChunkWireClear),
      }),
  };
}

RecordSpec stateOnlyRecord() {
  D9CCommandChunkWireDrawHeader draw{};
  draw.sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  draw.sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader);
  return {
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = bytesOf(draw),
  };
}

RecordSpec drawRecord(const D9CWireObjectIdentity& bufferIdentity,
                      std::uint32_t absoluteHandleIndex) {
  D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u,
      .primitiveCount = 1u,
      .sectionCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sizeof(D9CCommandChunkWireSectionDesc),
      alignof(D9CCommandChunkWireStreamBinding)));
  const D9CCommandChunkWireSectionDesc section{
      .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
      .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
      .count = 1u,
      .payloadOffset = draw.sectionPayloadOffset,
      .byteSize = sizeof(D9CCommandChunkWireStreamBinding),
  };
  const D9CCommandChunkWireStreamBinding stream{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = absoluteHandleIndex,
      .offset = 0u,
      .stride = 16u,
      .frequency = 1u,
  };
  std::vector<std::byte> payload(
      draw.sectionPayloadOffset + sizeof(stream));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset,
              &section, sizeof(section));
  std::memcpy(payload.data() + draw.sectionPayloadOffset,
              &stream, sizeof(stream));
  return {
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = std::move(payload),
      .handles = {dxmt9::d3d9::wireHandleEntry(bufferIdentity)},
  };
}

RecordSpec queryIssueRecord(const D9CWireObjectIdentity& queryIdentity,
                            std::uint32_t absoluteHandleIndex) {
  return {
      .type = D9C_COMMAND_RECORD_QUERY_ISSUE,
      .payload = bytesOf(D9CCommandChunkWireQueryIssue{
          .queryHandleIndex = absoluteHandleIndex,
          .flags = 2u,
      }),
      .handles = {dxmt9::d3d9::wireHandleEntry(queryIdentity)},
  };
}

dxmt9::d3d9::RawCommandChunk makeRaw(const WireFixture& fixture,
                                     std::uint64_t rawOrdinal) {
  dxmt9::d3d9::WireObjectRegistry registry;
  dxmt9::d3d9::RawCommandChunk raw;
  const bool prepared = dxmt9::d3d9::prepareOffloadChunk(
      fixture.bytes, fixture.envelope, registry,
      [](std::uint32_t, void*) noexcept {}, raw);
  check(prepared, "session join raw chunk must pass owned preflight");
  raw.replaySeq = rawOrdinal;
  raw.cpuReadyTapePlanningEnabled = true;
  return raw;
}

dxmt9::d3d9::RawCommandChunk makeRaw(
    const WireFixture& fixture, std::uint64_t rawOrdinal,
    const dxmt9::d3d9::WireObjectRegistry& registry) {
  dxmt9::d3d9::RawCommandChunk raw;
  const bool prepared = dxmt9::d3d9::prepareOffloadChunk(
      fixture.bytes, fixture.envelope, registry,
      [](std::uint32_t, void*) noexcept {}, raw);
  check(prepared,
        "resource-bearing session join chunk must pass owned preflight");
  raw.replaySeq = rawOrdinal;
  raw.cpuReadyTapePlanningEnabled = true;
  return raw;
}

struct SessionJoinDevice final : dxmt9::Device {
  explicit SessionJoinDevice(bool captureStreaming = false)
      : queue_(
            dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, limits_,
            retainedToken<WMT::CommandQueue>(
                "session-join-production-queue-token"),
            captureStreaming
                ? dxmt9::render::RenderPartitionConfig{
                      .sourceIdentity =
                          dxmt9::render::SourceIdentityConfig{
                              .requested = dxmt9::render::
                                  SourceIdentityModeRequest::Segment,
                              .resolved = dxmt9::render::
                                  SourceIdentityMode::SegmentSerial}}
                : dxmt9::render::RenderPartitionConfig{}) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return {}; }
  bool supportsCpuReadyArenaReplay() const noexcept override { return true; }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    return queue_.pool().createBuffer(WMT::Device{NULL_OBJECT_HANDLE}, desc);
  }

  SurfaceHandle createSurface(const SurfaceDesc&) override {
    return SurfaceHandle{nextHandle_++};
  }

  void submitClear(const ClearDesc& clear) override {
    queue_.submitClear(clear);
  }

  void submitDrawRun(CanonicalDrawState state,
                     const DrawUniformPayload& uniforms,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads) override {
    drawCalls.fetch_add(1, std::memory_order_relaxed);
    queue_.submitDrawRun(std::move(state), uniforms, draws, payloads);
  }

  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  std::atomic<std::uint32_t> drawCalls{0};
  std::uint64_t nextHandle_ = 1;
};

SourcePayloadLayout maximal64PageSegment(SourcePayloadCapacity capacity) {
  constexpr std::size_t kPageSize = 4096u;
  constexpr std::size_t kMaxPages = 64u;
  std::size_t acceptedBytes = 0u;
  std::size_t rejectedBytes = kPageSize * kMaxPages + 1u;
  std::optional<SourcePayloadLayout> accepted;
  while (acceptedBytes + 1u < rejectedBytes) {
    const std::size_t candidateBytes =
        acceptedBytes + (rejectedBytes - acceptedBytes) / 2u;
    capacity.drawPayloadBytes = candidateBytes;
    const auto candidate =
        makeSourcePayloadLayout(capacity, kPageSize, kMaxPages);
    if (candidate) {
      acceptedBytes = candidateBytes;
      accepted = candidate;
    } else {
      rejectedBytes = candidateBytes;
    }
  }
  check(accepted.has_value() && accepted->pageCount == kMaxPages,
        "capture segment must maximize within the production 64-page bound");
  return *accepted;
}

struct RuntimeFixture {
  explicit RuntimeFixture(bool captureStreaming = false) {
    auto upper = std::make_unique<SessionJoinDevice>(captureStreaming);
    routing = upper.get();
    factory = dxmt9::com::Direct3DCreate9Ex(
        dxmt9::com::D3D_SDK_VERSION, std::move(upper));
    check(factory != nullptr, "session join factory must construct");

    PresentParameters params{};
    params.backBufferWidth = 16;
    params.backBufferHeight = 16;
    params.backBufferFormat = Format::A8R8G8B8;
    params.windowed = true;
    params.deviceWindow = Handle{92};
    params.presentationInterval = PresentInterval::Immediate;
    device = factory->CreateDeviceEx(0, params, nullptr);
    check(device != nullptr, "session join core device must construct");
    device->AddRef();
    cDevice = std::make_unique<D9CDevice>(device);
  }

  ~RuntimeFixture() {
    cDevice.reset();
    if (device) {
      (void)device->Release();
    }
    if (factory) {
      (void)factory->Release();
    }
  }

  void publishArenaClear(std::uint64_t rawOrdinal) {
    const std::array records{clearRecord()};
    auto raw = makeRaw(makeWireFixture(records), rawOrdinal);
    const auto hr = dxmt9::d3d9::replayRawChunk(cDevice.get(), raw);
    check(hr == D3D_OK, "arena clear source must replay and publish");
  }

  void publishArenaClearPages(std::uint64_t rawOrdinal,
                              std::size_t pageCount) {
    check(pageCount != 0u && pageCount <= 512u &&
              (pageCount <= 64u || pageCount % 64u == 0u),
          "sized Arena clear source must use bounded 64-page segments");
    const std::size_t segmentCount =
        pageCount <= 64u ? 1u : pageCount / 64u;
    std::array<SourcePayloadLayout, 8> segments{};
    for (std::size_t i = 0; i < segmentCount; ++i) {
      const std::size_t segmentPages = pageCount <= 64u ? pageCount : 64u;
      SourcePayloadCapacity capacity{};
      capacity.commandHeaders = 1;
      capacity.clearRecords = 1;
      capacity.drawPayloadBytes = (segmentPages - 1u) * 4096u;
      const auto segment = pageCount > 64u
          ? std::optional<SourcePayloadLayout>{
                maximal64PageSegment(capacity)}
          : makeSourcePayloadLayout(capacity, 4096, 64);
      check(segment.has_value() && segment->pageCount == segmentPages,
            "sized Arena clear segment must match its bounded page claim");
      segments[i] = *segment;
    }
    const auto layout = makeArenaSourcePayloadLayout(
        std::span(segments).first(segmentCount), 4096, pageCount);
    check(layout.has_value() && layout->pageCount == pageCount,
          "sized arena clear source must match its exact page claim");
    auto begin = routing->queue_.beginCpuReadyArenaSource(rawOrdinal, *layout);
    check(begin.has_value(), "sized arena clear admission must succeed");
    auto lease = std::move(*begin.lease);
    for (std::size_t i = 0; i < segmentCount; ++i) {
      check(lease.selectSegment(i),
            "sized Arena clear source selects each physical segment");
      routing->queue_.submitClear(ClearDesc{});
    }
    check(lease.publish(), "sized arena clear source must publish directly");
  }

  void publishArenaClearPresentPages(std::uint64_t rawOrdinal,
                                     std::size_t pageCount) {
    check(pageCount != 0u && pageCount % 64u == 0u && pageCount <= 512u,
          "sized Arena Present source must use bounded 64-page segments");
    const std::size_t segmentCount = pageCount / 64u;
    std::array<SourcePayloadLayout, 8> segments{};
    for (std::size_t i = 0; i < segmentCount; ++i) {
      SourcePayloadCapacity capacity{};
      capacity.commandHeaders = i + 1u == segmentCount ? 2u : 1u;
      capacity.clearRecords = 1;
      capacity.presentRecords = i + 1u == segmentCount ? 1u : 0u;
      const auto segment = maximal64PageSegment(capacity);
      check(segment.pageCount == 64u,
            "sized Arena Present segment must claim exactly 64 pages");
      segments[i] = segment;
    }
    const auto layout = makeArenaSourcePayloadLayout(
        std::span(segments).first(segmentCount), 4096, pageCount);
    check(layout.has_value() && layout->pageCount == pageCount,
          "sized Arena Present source must match its exact page claim");
    auto begin = routing->queue_.beginCpuReadyArenaSource(rawOrdinal, *layout);
    check(begin.has_value(), "sized Arena Present admission must succeed");
    auto lease = std::move(*begin.lease);
    for (std::size_t i = 0; i < segmentCount; ++i) {
      check(lease.selectSegment(i),
            "sized Arena Present source selects each physical segment");
      routing->queue_.submitClear(ClearDesc{});
      if (i + 1u == segmentCount) {
        check(routing->queue_.submitPresent(SwapDesc{}) != 0u,
              "sized Arena Present records one terminal Present");
      }
    }
    check(lease.publish(), "sized Arena Present source publishes directly");
  }

  void publishArenaDraw(std::uint64_t rawOrdinal) {
    auto buffer = device->CreateBuffer(BufferDesc{
        .size = 256u,
        .pool = Pool::Default,
        .usage = UsageVertexBuffer,
    });
    check(buffer != nullptr, "arena draw buffer must construct");
    D9CBuffer wireBuffer(buffer, cDevice.get());
    const std::array records{drawRecord(wireBuffer.wireIdentity, 0u)};
    auto raw = makeRaw(makeWireFixture(records), rawOrdinal,
                       cDevice->wireObjects);
    const auto hr = dxmt9::d3d9::replayRawChunk(cDevice.get(), raw);
    check(hr == D3D_OK, "arena Direct draw must replay and publish");
  }

  void replayStateOnly(std::uint64_t rawOrdinal) {
    const std::array records{stateOnlyRecord()};
    auto raw = makeRaw(makeWireFixture(records), rawOrdinal);
    const auto hr = dxmt9::d3d9::replayRawChunk(cDevice.get(), raw);
    check(hr == D3D_OK,
          "StateOnly raw entry must replay without publishing a source");
  }

  void publishLegacyClear() {
    routing->queue_.submitClear(ClearDesc{});
    check(dxmt9::CommandQueueArenaLeaseTestAccess::publishLegacyWritingSlot(
              routing->queue_),
          "legacy writing slot must publish through the compatibility lane");
  }

  void publishLegacyTargetDraw(std::uint64_t target) {
    publishTargetDraw(target);
    check(dxmt9::CommandQueueArenaLeaseTestAccess::publishLegacyWritingSlot(
              routing->queue_),
          "legacy target draw must publish through the Tape lane");
  }

  void beginLegacyTargetDraw(std::uint64_t target) {
    publishTargetDraw(target);
  }

  void publishArenaTargetDraw(std::uint64_t rawOrdinal,
                              std::uint64_t target) {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 1;
    capacity.drawHotStates = 1;
    capacity.drawShaderLayouts = 1;
    capacity.drawDebugSnapshots = 1;
    capacity.drawPsoSubviews = 1;
    capacity.drawUniformFixedPayloads = 1;
    capacity.drawUniformVertexConstants = 1;
    capacity.drawUniformVertexConstantBytes =
        sizeof(VertexShaderConstants);
    capacity.drawUniformPixelConstants = 1;
    capacity.drawUniformPixelConstantBytes = sizeof(PixelShaderConstants);
    capacity.drawUniformPayloads = 1;
    capacity.drawParams = 1;
    capacity.drawPayloadBytes = 4096;
    capacity.drawRunRecords = 1;
    const auto segment = makeSourcePayloadLayout(capacity, 4096, 64);
    check(segment.has_value(), "arena target segment layout must build");
    const std::array segments{*segment};
    const auto layout = makeArenaSourcePayloadLayout(segments, 4096, 64);
    check(layout.has_value(), "arena target source layout must build");
    auto begin = routing->queue_.beginCpuReadyArenaSource(rawOrdinal, *layout);
    check(begin.has_value(), "arena target source admission must succeed");
    auto lease = std::move(*begin.lease);
    publishTargetDraw(target);
    check(lease.publish(), "arena target source must publish directly");
  }

  dxmt9::CommandQueue::CpuReadyArenaBuildLease beginArenaTargetDraw(
      std::uint64_t rawOrdinal, std::uint64_t target) {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 1;
    capacity.drawHotStates = 1;
    capacity.drawShaderLayouts = 1;
    capacity.drawDebugSnapshots = 1;
    capacity.drawPsoSubviews = 1;
    capacity.drawUniformFixedPayloads = 1;
    capacity.drawUniformVertexConstants = 1;
    capacity.drawUniformVertexConstantBytes = sizeof(VertexShaderConstants);
    capacity.drawUniformPixelConstants = 1;
    capacity.drawUniformPixelConstantBytes = sizeof(PixelShaderConstants);
    capacity.drawUniformPayloads = 1;
    capacity.drawParams = 1;
    capacity.drawPayloadBytes = 4096;
    capacity.drawRunRecords = 1;
    const auto segment = makeSourcePayloadLayout(capacity, 4096, 64);
    check(segment.has_value(), "Writing target segment layout must build");
    const std::array segments{*segment};
    const auto layout = makeArenaSourcePayloadLayout(segments, 4096, 64);
    check(layout.has_value(), "Writing target source layout must build");
    auto begin = routing->queue_.beginCpuReadyArenaSource(rawOrdinal, *layout);
    check(begin.has_value(), "Writing target source admission must succeed");
    auto lease = std::move(*begin.lease);
    publishTargetDraw(target);
    return lease;
  }

  void publishArenaTerminalSuffix(std::uint64_t rawOrdinal,
                                  std::uint64_t targetA,
                                  std::uint64_t targetB) {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 3;
    capacity.clearRecords = 1;
    capacity.drawHotStates = 2;
    capacity.drawShaderLayouts = 2;
    capacity.drawDebugSnapshots = 2;
    capacity.drawPsoSubviews = 2;
    capacity.drawUniformFixedPayloads = 2;
    capacity.drawUniformVertexConstants = 2;
    capacity.drawUniformVertexConstantBytes =
        2 * sizeof(VertexShaderConstants);
    capacity.drawUniformPixelConstants = 2;
    capacity.drawUniformPixelConstantBytes =
        2 * sizeof(PixelShaderConstants);
    capacity.drawUniformPayloads = 2;
    capacity.drawParams = 2;
    capacity.drawPayloadBytes = 2 * 4096;
    capacity.drawRunRecords = 2;
    const auto segment = makeSourcePayloadLayout(capacity, 4096, 64);
    check(segment.has_value(), "terminal-suffix segment layout must build");
    const std::array segments{*segment};
    const auto layout = makeArenaSourcePayloadLayout(segments, 4096, 64);
    check(layout.has_value(), "terminal-suffix source layout must build");
    auto begin = routing->queue_.beginCpuReadyArenaSource(rawOrdinal, *layout);
    check(begin.has_value(), "terminal-suffix admission must succeed");
    auto lease = std::move(*begin.lease);

    publishTargetDraw(targetA);
    ClearDesc clear{};
    clear.colorAttachments[0] = RenderTargetAttachment{
        .handle = Handle{targetB},
        .sampleCount = 1u,
    };
    clear.clearColor = true;
    routing->queue_.submitClear(clear);
    publishTargetDraw(targetB);
    check(lease.publish(), "terminal-suffix source must publish directly");
  }

  void publishArenaMovedHeadReturn(std::uint64_t rawOrdinal,
                                   std::uint64_t targetA,
                                   std::uint64_t targetB) {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 3;
    capacity.drawHotStates = 3;
    capacity.drawShaderLayouts = 3;
    capacity.drawDebugSnapshots = 3;
    capacity.drawPsoSubviews = 3;
    capacity.drawUniformFixedPayloads = 3;
    capacity.drawUniformVertexConstants = 3;
    capacity.drawUniformVertexConstantBytes =
        3 * sizeof(VertexShaderConstants);
    capacity.drawUniformPixelConstants = 3;
    capacity.drawUniformPixelConstantBytes =
        3 * sizeof(PixelShaderConstants);
    capacity.drawUniformPayloads = 3;
    capacity.drawParams = 3;
    capacity.drawPayloadBytes = 3 * 4096;
    capacity.drawRunRecords = 3;
    const auto segment = makeSourcePayloadLayout(capacity, 4096, 64);
    check(segment.has_value(), "moved-head arena segment layout must build");
    const std::array segments{*segment};
    const auto layout = makeArenaSourcePayloadLayout(segments, 4096, 64);
    check(layout.has_value(), "moved-head arena source layout must build");
    auto begin = routing->queue_.beginCpuReadyArenaSource(rawOrdinal, *layout);
    check(begin.has_value(), "moved-head arena admission must succeed");
    auto lease = std::move(*begin.lease);
    publishTargetDraw(targetA);
    publishTargetDraw(targetB);
    publishTargetDraw(targetA, targetB);
    check(lease.publish(), "moved-head arena source must publish directly");
  }

  void publishArenaTargetPair(std::uint64_t rawOrdinal,
                              std::uint64_t firstTarget,
                              std::uint64_t secondTarget) {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 2;
    capacity.drawHotStates = 2;
    capacity.drawShaderLayouts = 2;
    capacity.drawDebugSnapshots = 2;
    capacity.drawPsoSubviews = 2;
    capacity.drawUniformFixedPayloads = 2;
    capacity.drawUniformVertexConstants = 2;
    capacity.drawUniformVertexConstantBytes =
        2 * sizeof(VertexShaderConstants);
    capacity.drawUniformPixelConstants = 2;
    capacity.drawUniformPixelConstantBytes =
        2 * sizeof(PixelShaderConstants);
    capacity.drawUniformPayloads = 2;
    capacity.drawParams = 2;
    capacity.drawPayloadBytes = 2 * 4096;
    capacity.drawRunRecords = 2;
    const auto segment = makeSourcePayloadLayout(capacity, 4096, 64);
    check(segment.has_value(), "arena pair segment layout must build");
    const std::array segments{*segment};
    const auto layout = makeArenaSourcePayloadLayout(segments, 4096, 64);
    check(layout.has_value(), "arena pair source layout must build");
    auto begin = routing->queue_.beginCpuReadyArenaSource(rawOrdinal, *layout);
    check(begin.has_value(), "arena pair admission must succeed");
    auto lease = std::move(*begin.lease);
    publishTargetDraw(firstTarget);
    publishTargetDraw(secondTarget);
    check(lease.publish(), "arena pair source must publish directly");
  }

  void publishArenaTargetSequence(
      std::uint64_t rawOrdinal,
      std::span<const std::uint64_t> targets) {
    check(!targets.empty(), "arena target sequence must not be empty");
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = targets.size();
    capacity.drawHotStates = targets.size();
    capacity.drawShaderLayouts = targets.size();
    capacity.drawDebugSnapshots = targets.size();
    capacity.drawPsoSubviews = targets.size();
    capacity.drawUniformFixedPayloads = targets.size();
    capacity.drawUniformVertexConstants = targets.size();
    capacity.drawUniformVertexConstantBytes =
        targets.size() * sizeof(VertexShaderConstants);
    capacity.drawUniformPixelConstants = targets.size();
    capacity.drawUniformPixelConstantBytes =
        targets.size() * sizeof(PixelShaderConstants);
    capacity.drawUniformPayloads = targets.size();
    capacity.drawParams = targets.size();
    capacity.drawPayloadBytes = targets.size() * 4096u;
    capacity.drawRunRecords = targets.size();
    const auto segment = makeSourcePayloadLayout(capacity, 4096, 64);
    check(segment.has_value(), "arena sequence segment layout must build");
    const std::array segments{*segment};
    const auto layout = makeArenaSourcePayloadLayout(segments, 4096, 64);
    check(layout.has_value(), "arena sequence source layout must build");
    auto begin = routing->queue_.beginCpuReadyArenaSource(rawOrdinal, *layout);
    check(begin.has_value(), "arena sequence source admission must succeed");
    auto lease = std::move(*begin.lease);
    for (const auto target : targets) {
      publishTargetDraw(target);
    }
    check(lease.publish(), "arena sequence source must publish directly");
  }

 private:
  void publishTargetDraw(std::uint64_t target,
                         std::uint64_t sampledTarget = 0) {
    CanonicalDrawState state{};
    state.hot.colorAttachments[0].handle = Handle{target};
    state.hot.colorAttachments[0].sampleCount = 1;
    state.hot.renderTargetMask = 1u;
    if (sampledTarget != 0) {
      state.hot.textures[0] = Handle{sampledTarget};
      state.hot.textureMask = 1u;
    }
    state.hot.streamStrides[0] = 4u;
    state.shaderLayout.vertexDecl.streams[0].stride = 4u;
    state.shaderLayout.vertexShader.kind =
        ShaderRef::Kind::Bytecode;
    state.shaderLayout.pixelShader.kind =
        ShaderRef::Kind::Bytecode;
    DrawParam draw{};
    draw.primitiveType = PrimitiveType::TriangleList;
    draw.primitiveCount = 1;
    draw.instanceCount = 1;
    const std::array draws{draw};
    const std::array<DrawParamPayloadView, 1> payloads{};
    routing->queue_.submitDrawRun(
        std::move(state), DrawUniformPayload{}, draws, payloads);
  }

 public:

  SessionJoinDevice* routing = nullptr;
  dxmt9::com::IDirect3D9Ex* factory = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* device = nullptr;
  std::unique_ptr<D9CDevice> cDevice;
};

struct ProductionLoopBackendCall {
  std::size_t slotIndex = 0;
  std::uint64_t seqId = 0;
  bool arena = false;
  std::uintptr_t session = 0;
  bool deferSessionFinalization = false;
  bool allowInjectedCommandBufferMidChunkCommits = false;
  std::optional<dxmt9::core::metalqueue::QueueCompletionSource>
      sessionSource;
  dxmt9::core::CpuReadyTape::SourceRef partitionSource{};
  std::size_t lookaheadCount = 0;
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource>
      lookaheadSources;
  dxmt9::encoders::ReplayWindowProvenance replayWindow{};
  dxmt9::encoders::ActiveSeedMergeTicketContext activeSeedMergeTicket{};
  std::vector<dxmt9::encoders::ActiveSeedMergeTargetWitness>
      activeSeedMergeTargets;
  obj_handle_t commandBuffer = NULL_OBJECT_HANDLE;
  bool createdCommandBuffer = false;
  obj_handle_t returnedCommandBuffer = NULL_OBJECT_HANDLE;
  std::uint64_t returnedCommandBufferChainLength = 0;
  std::size_t renderPassBeginsAfter = 0;
  std::size_t renderPassEndsAfter = 0;
  std::optional<dxmt9::encoders::PreRegisteredEncodeChunkFragment> fragment;
  bool skipBackendPlanning = false;
};

std::vector<dxmt9::core::metalqueue::QueueCompletionSource>
snapshotLookaheadSources(
    std::span<const dxmt9::core::metalqueue::ResolvedPublishedSource>
        sources) {
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> snapshots;
  snapshots.reserve(sources.size());
  for (const auto& source : sources) {
    snapshots.push_back(dxmt9::core::metalqueue::QueueCompletionSource{
        .source = source.source,
        .slotIndex = source.slotIndex,
        .seqId = source.seqId,
        .hasPresent = source.hasPresent,
        .commandBegin = source.commandBegin,
        .commandCount = source.commandCount,
    });
  }
  return snapshots;
}

struct ProductionLoopBackendState {
  std::vector<ProductionLoopBackendCall> calls;
  std::atomic<std::size_t> observedBackendCalls{0};
  std::atomic<bool> firstRecordPostCommitRan{false};
  std::atomic<std::size_t> backendCallCountAtFirstRecordSubmit{0};
};

class ProductionLoopBackend final : public dxmt9::render::IRenderBackend {
 public:
  explicit ProductionLoopBackend(
      std::shared_ptr<ProductionLoopBackendState> state)
      : state_(std::move(state)) {}

  std::optional<dxmt9::core::metalqueue::QueueSubmissionRecord> onChunkReady(
      dxmt9::encoders::EncodeContext&,
      std::size_t slotIndex,
      const dxmt9::core::ChunkSlot& slot,
      dxmt9::encoders::EncodeChunkOptions options) override {
    return record(slotIndex, dxmt9::core::SourcePayloadView(slot), slot.seqId,
                  std::move(options));
  }

  std::optional<dxmt9::core::metalqueue::QueueSubmissionRecord> onSourceReady(
      dxmt9::encoders::EncodeContext&,
      std::size_t slotIndex,
      dxmt9::core::SourcePayloadView payload,
      std::uint64_t seqId,
      dxmt9::encoders::EncodeChunkOptions options) override {
    return record(slotIndex, payload, seqId, std::move(options));
  }

  dxmt9::render::BackendMode mode() const override {
    return dxmt9::render::BackendMode::Traditional;
  }

 private:
  std::optional<dxmt9::core::metalqueue::QueueSubmissionRecord> record(
      std::size_t slotIndex,
      dxmt9::core::SourcePayloadView payload,
      std::uint64_t seqId,
      dxmt9::encoders::EncodeChunkOptions options) {
    state_->calls.push_back(ProductionLoopBackendCall{
        .slotIndex = slotIndex,
        .seqId = seqId,
        .arena = payload.isArena(),
        .session = reinterpret_cast<std::uintptr_t>(options.session),
        .deferSessionFinalization = options.deferSessionFinalization,
        .allowInjectedCommandBufferMidChunkCommits =
            options.allowInjectedCommandBufferMidChunkCommits,
        .sessionSource = options.sessionSource,
        .partitionSource = options.partitionSource,
        .lookaheadCount = options.sessionLookaheadSources.size(),
        .lookaheadSources =
            snapshotLookaheadSources(options.sessionLookaheadSources),
        .replayWindow = options.replayWindow,
        .activeSeedMergeTicket = options.activeSeedMergeTicket,
        .activeSeedMergeTargets = std::vector<
            dxmt9::encoders::ActiveSeedMergeTargetWitness>(
                options.activeSeedMergeTargets.begin(),
                options.activeSeedMergeTargets.end()),
        .commandBuffer = options.commandBuffer.handle,
        .fragment = options.preRegisteredFragment,
        .skipBackendPlanning = options.skipBackendPlanning,
    });
    state_->observedBackendCalls.store(state_->calls.size(),
                                       std::memory_order_release);

    dxmt9::core::metalqueue::QueueSubmissionRecord submission{};
    submission.testOnlyAllowNullCommandBuffer = true;
    submission.slotIndex = slotIndex;
    submission.seqId = seqId;
    if (state_->calls.size() == 1) {
      const auto state = state_;
      submission.postCommitCallbacks.push_back([state] {
        state->backendCallCountAtFirstRecordSubmit.store(
            state->calls.size(), std::memory_order_relaxed);
        state->firstRecordPostCommitRan.store(true, std::memory_order_release);
      });
    }
    return submission;
  }

  std::shared_ptr<ProductionLoopBackendState> state_;
};

struct PlannedProductionBackendState {
  std::vector<ProductionLoopBackendCall> calls;
  std::vector<std::uint64_t> encodedSeqIds;
  std::vector<std::size_t> encodedCommandIndices;
  std::vector<std::uint64_t> sourcePreambleSeqIds;
  std::vector<std::uint64_t> sourceEpilogueSeqIds;
  std::atomic<std::size_t> observedCalls{0};
  std::atomic<bool> firstRecordPostCommitRan{false};
  std::atomic<std::size_t> firstRecordPostCommitRuns{0};
  std::atomic<std::size_t> backendCallsAtFirstRecordSubmit{0};
  std::uint64_t currentSeqId = 0;
  std::size_t plannerCalls = 0;
  std::vector<dxmt9::encoders::EncodeSessionReplayFrontierState>
      plannerFrontierStates;
  std::vector<std::size_t> plannerSourceCounts;
  std::size_t compositeObserverCalls = 0;
  std::vector<std::uint64_t> compositeObservedSeqIds;
  std::size_t renderPassBegins = 0;
  std::size_t renderPassEnds = 0;
  std::vector<std::size_t> storeProofLookaheadCounts;
  std::size_t transactionPreambles = 0;
  std::size_t midChunkSplits = 0;
  bool observeFirstRecordSubmit = false;
  bool holdFirstReturn = false;
  bool holdSecondReturn = false;
  bool holdFirstPlanner = false;
  bool holdFirstObserver = false;
  bool disableMidChunkCommits = false;
  bool markFirstRecordCaptureBoundary = false;
  std::optional<dxmt9::framegraph::MultiSourceReplayPlan> forcedPlan;
  std::atomic<bool> firstCallEncoded{false};
  std::atomic<bool> releaseFirstReturn{false};
  std::atomic<bool> secondCallEncoded{false};
  std::atomic<bool> releaseSecondReturn{false};
  std::atomic<bool> firstPlannerEntered{false};
  std::atomic<bool> releaseFirstPlanner{false};
  std::atomic<bool> firstObserverEntered{false};
  std::atomic<bool> releaseFirstObserver{false};
  std::atomic<std::size_t> completedCalls{0};
};

void plannedBeginSourceFragmentPreamble(void* userdata,
                                        std::uint64_t seqId) {
  static_cast<PlannedProductionBackendState*>(userdata)
      ->sourcePreambleSeqIds.push_back(seqId);
}

void plannedBeginTransactionPreamble(void* userdata) {
  ++static_cast<PlannedProductionBackendState*>(userdata)
        ->transactionPreambles;
}

void plannedEndSourceFragmentEpilogue(void* userdata,
                                      std::uint64_t seqId,
                                      std::uint64_t) {
  static_cast<PlannedProductionBackendState*>(userdata)
      ->sourceEpilogueSeqIds.push_back(seqId);
}

void plannedBeginDrawRun(void* userdata, std::size_t commandIndex,
                         std::size_t) {
  auto* state = static_cast<PlannedProductionBackendState*>(userdata);
  state->encodedSeqIds.push_back(state->currentSeqId);
  state->encodedCommandIndices.push_back(commandIndex);
}

void plannedBeginRenderPass(void* userdata, std::size_t) {
  ++static_cast<PlannedProductionBackendState*>(userdata)->renderPassBegins;
}

void plannedObserveRenderPassStoreProofLookahead(void* userdata,
                                                 std::size_t,
                                                 std::size_t sourceCount) {
  static_cast<PlannedProductionBackendState*>(userdata)
      ->storeProofLookaheadCounts.push_back(sourceCount);
}

void plannedEndRenderPass(void* userdata) {
  ++static_cast<PlannedProductionBackendState*>(userdata)->renderPassEnds;
}

void plannedDrawPrimitives(void*, WMTPrimitiveType, std::uint64_t,
                           std::uint64_t, std::uint32_t, std::uint32_t) {}

WMT::Reference<WMT::CommandBuffer> plannedSplitCommandBuffer(
    void* userdata, WMT::CommandBuffer) {
  auto* state = static_cast<PlannedProductionBackendState*>(userdata);
  ++state->midChunkSplits;
  return retainedToken<WMT::CommandBuffer>(
      "multi-source-production-split-command-buffer-token");
}

class PlannedProductionBackend final : public dxmt9::render::IRenderBackend {
 public:
  explicit PlannedProductionBackend(
      std::shared_ptr<PlannedProductionBackendState> state)
      : state_(std::move(state)),
        device_(retainedToken<WMT::Device>(
            "multi-source-production-device-token")),
        renderEncoderOwner_(retainedToken<WMT::RenderCommandEncoder>(
            "multi-source-production-render-encoder-token")),
        recorder_{
            .userdata = state_.get(),
            .suppressMetalCalls = true,
            .suppressBaseStateLookup = true,
            .renderPipelineState = WMT::RenderPipelineState{
                0x730000000000001ull},
            .depthStencilState = WMT::DepthStencilState{
                0x730000000000002ull},
            .renderCommandEncoder =
                WMT::RenderCommandEncoder{renderEncoderOwner_.handle},
            .beginSourceFragmentPreamble =
                plannedBeginSourceFragmentPreamble,
            .beginTransactionPreamble = plannedBeginTransactionPreamble,
            .endSourceFragmentEpilogue =
                plannedEndSourceFragmentEpilogue,
            .beginDrawRunCommand = plannedBeginDrawRun,
            .beginRenderPass = plannedBeginRenderPass,
            .observeRenderPassStoreProofLookahead =
                plannedObserveRenderPassStoreProofLookahead,
            .endRenderPass = plannedEndRenderPass,
            .splitCommandBufferForTest = plannedSplitCommandBuffer,
            .drawPrimitives = plannedDrawPrimitives,
        } {}

  std::optional<dxmt9::core::metalqueue::QueueSubmissionRecord> onSourceReady(
      dxmt9::encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      dxmt9::core::SourcePayloadView payload,
      std::uint64_t seqId,
      dxmt9::encoders::EncodeChunkOptions options) override {
    const bool createdCommandBuffer = !options.commandBuffer;
    if (createdCommandBuffer) {
      options.commandBuffer = retainedToken<WMT::CommandBuffer>(
          "multi-source-production-command-buffer-token");
    }
    state_->calls.push_back(ProductionLoopBackendCall{
        .slotIndex = slotIndex,
        .seqId = seqId,
        .arena = payload.isArena(),
        .session = reinterpret_cast<std::uintptr_t>(options.session),
        .deferSessionFinalization = options.deferSessionFinalization,
        .allowInjectedCommandBufferMidChunkCommits =
            options.allowInjectedCommandBufferMidChunkCommits,
        .sessionSource = options.sessionSource,
        .partitionSource = options.partitionSource,
        .lookaheadCount = options.sessionLookaheadSources.size(),
        .lookaheadSources =
            snapshotLookaheadSources(options.sessionLookaheadSources),
        .replayWindow = options.replayWindow,
        .activeSeedMergeTicket = options.activeSeedMergeTicket,
        .activeSeedMergeTargets = std::vector<
            dxmt9::encoders::ActiveSeedMergeTargetWitness>(
                options.activeSeedMergeTargets.begin(),
                options.activeSeedMergeTargets.end()),
        .commandBuffer = options.commandBuffer.handle,
        .createdCommandBuffer = createdCommandBuffer,
        .fragment = options.preRegisteredFragment,
        .skipBackendPlanning = options.skipBackendPlanning,
    });
    state_->currentSeqId = seqId;
    state_->observedCalls.store(state_->calls.size(),
                                std::memory_order_release);
    ctx.device = device_;
    ctx.drawRecorder = &recorder_;
    options.disableMidChunkCommits = state_->disableMidChunkCommits;
    auto submission = planner_.onSourceReady(ctx, slotIndex, payload, seqId,
                                             std::move(options));
    if (submission) {
      state_->calls.back().returnedCommandBuffer =
          submission->commandBuffer.handle;
      state_->calls.back().returnedCommandBufferChainLength =
          submission->commandBufferChainLength;
      submission->testOnlyAllowNullCommandBuffer = true;
      if (state_->markFirstRecordCaptureBoundary &&
          state_->calls.size() == 1u) {
        submission->metalCaptureAlreadyStarted = true;
      }
      if (state_->observeFirstRecordSubmit && state_->calls.size() == 1) {
        const auto state = state_;
        submission->postCommitCallbacks.emplace_back([state] {
          state->backendCallsAtFirstRecordSubmit.store(
              state->calls.size(), std::memory_order_relaxed);
          state->firstRecordPostCommitRuns.fetch_add(
              1u, std::memory_order_relaxed);
          state->firstRecordPostCommitRan.store(true,
                                                std::memory_order_release);
        });
      }
    }
    state_->calls.back().renderPassBeginsAfter = state_->renderPassBegins;
    state_->calls.back().renderPassEndsAfter = state_->renderPassEnds;
    state_->completedCalls.store(state_->calls.size(),
                                 std::memory_order_release);
    if (state_->calls.size() == 1 && state_->holdFirstReturn) {
      state_->firstCallEncoded.store(true, std::memory_order_release);
      while (!state_->releaseFirstReturn.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
    if (state_->calls.size() == 2 && state_->holdSecondReturn) {
      state_->secondCallEncoded.store(true, std::memory_order_release);
      while (!state_->releaseSecondReturn.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
    return submission;
  }

  std::optional<dxmt9::core::metalqueue::QueueSubmissionRecord> onChunkReady(
      dxmt9::encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      const dxmt9::core::ChunkSlot& slot,
      dxmt9::encoders::EncodeChunkOptions options) override {
    return onSourceReady(ctx, slotIndex,
                         dxmt9::core::SourcePayloadView(slot), slot.seqId,
                         std::move(options));
  }

  dxmt9::framegraph::MultiSourceReplayPlan planMultiSourceSessionReplay(
      const dxmt9::resources::Pool& pool,
      std::span<const dxmt9::core::metalqueue::ResolvedPublishedSource>
          sources,
      const dxmt9::render::MultiSourceSessionReplayFrontier& frontier) override {
    ++state_->plannerCalls;
    state_->plannerFrontierStates.push_back(frontier.state);
    state_->plannerSourceCounts.push_back(sources.size());
    if (state_->holdFirstPlanner && state_->plannerCalls == 1) {
      state_->firstPlannerEntered.store(true, std::memory_order_release);
      while (!state_->releaseFirstPlanner.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
    if (state_->forcedPlan.has_value()) {
      return *state_->forcedPlan;
    }
    return planner_.planMultiSourceSessionReplay(pool, sources, frontier);
  }

  void observeMultiSourceSessionReplay(
      const dxmt9::resources::Pool& pool,
      std::span<const dxmt9::core::metalqueue::ResolvedPublishedSource>
          sources) override {
    ++state_->compositeObserverCalls;
    for (const auto& source : sources) {
      state_->compositeObservedSeqIds.push_back(source.seqId);
    }
    if (state_->holdFirstObserver && state_->compositeObserverCalls == 1) {
      state_->firstObserverEntered.store(true, std::memory_order_release);
      while (!state_->releaseFirstObserver.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
    planner_.observeMultiSourceSessionReplay(pool, sources);
  }

  dxmt9::render::BackendMode mode() const override {
    return dxmt9::render::BackendMode::FrameGraph;
  }

  dxmt9::encoders::EncodeDrawRecorder* drawRecorder() {
    return &recorder_;
  }

 private:
  std::shared_ptr<PlannedProductionBackendState> state_;
  WMT::Reference<WMT::Device> device_;
  WMT::Reference<WMT::RenderCommandEncoder> renderEncoderOwner_;
  dxmt9::encoders::EncodeDrawRecorder recorder_;
  dxmt9::render::FrameGraphBackend planner_;
};

bool sameCompletionSource(
    const dxmt9::core::metalqueue::QueueCompletionSource& left,
    const dxmt9::core::metalqueue::QueueCompletionSource& right) {
  return left.source == right.source && left.slotIndex == right.slotIndex &&
      left.seqId == right.seqId && left.hasPresent == right.hasPresent &&
      left.commandBegin == right.commandBegin &&
      left.commandCount == right.commandCount;
}

void runProductionLoopStopDrainCase(std::vector<bool> expectedArenaKinds) {
  RuntimeFixture fixture;
  if (expectedArenaKinds == std::vector<bool>({true, true, true})) {
    fixture.publishArenaClear(1);
    fixture.publishArenaClear(2);
    fixture.publishArenaClear(3);
  } else {
    fixture.publishArenaClear(1);
    fixture.publishLegacyClear();
    fixture.publishArenaClear(2);
  }

  auto& queue = fixture.routing->queue_;
  const auto expectedSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(expectedSources.size() == expectedArenaKinds.size(),
        "production-loop fixture must snapshot every ready source");
  check(expectedSources.size() == 3 && expectedSources[0].seqId == 1 &&
            expectedSources[1].seqId == 2 &&
            expectedSources[2].seqId == 3,
        "production-loop completion snapshot must preserve FIFO seqIds");

  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));

  // All sources are published before stop is set. The production loop must
  // consume the ready prefix before its shutdown-drain fence submits it.
  dxmt9::CommandQueueArenaLeaseTestAccess::
      stopAndRunCpuReadySessionEncodeLoop(queue);

  check(backendState->calls.size() == expectedSources.size(),
        "production loop must hand every selected source to the backend");
  check(backendState->firstRecordPostCommitRan.load(std::memory_order_acquire),
        "the first record callback must run at physical submit");
  check(backendState->backendCallCountAtFirstRecordSubmit.load(
            std::memory_order_relaxed) ==
            expectedSources.size(),
        "the first record must remain pending until all sources merge into "
        "the shutdown-drain submit");

  const auto session = backendState->calls.front().session;
  check(session != 0, "production loop must create a session for the head");
  for (std::size_t i = 0; i < backendState->calls.size(); ++i) {
    const auto& call = backendState->calls[i];
    check(call.slotIndex == expectedSources[i].slotIndex &&
              call.seqId == expectedSources[i].seqId &&
              call.arena == expectedArenaKinds[i],
          "backend calls must preserve snapshotted FIFO source identity");
    check(call.session == session && call.deferSessionFinalization &&
              call.allowInjectedCommandBufferMidChunkCommits,
          "every compatible source must receive the same deferred session "
          "append options");
    check(call.sessionSource.has_value() &&
              sameCompletionSource(*call.sessionSource,
                                   expectedSources[i]),
          "session append options must carry the exact snapshotted source");
    check(call.partitionSource == expectedSources[i].source,
          "partition identity must follow the represented source");
    const std::size_t remaining = expectedSources.size() - i;
    check(call.lookaheadCount == (remaining > 1 ? remaining : 0),
          "production loop must expose the selected FIFO suffix as "
          "session lookahead");
  }

  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(queue) == 0,
        "production loop must consume the whole ready prefix");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "one shutdown-drain submit must publish every completion identity");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(queue) == 0,
        "eligible encoded sources retire before GPU completion");

  auto retainedCompletionOwner = std::make_shared<int>(37);
  std::weak_ptr<void> retainedCompletionWeak = retainedCompletionOwner;
  bool completionCallbackRan = false;
  bool callbackPrecededWaterline = false;
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources,
            [&] {
              completionCallbackRan = true;
              callbackPrecededWaterline =
                  !retainedCompletionWeak.expired() &&
                  dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(
                      queue) == 0u;
            },
            std::move(retainedCompletionOwner)) == expectedSources.size(),
        "test completion must expand and finish every merged source");
  check(completionCallbackRan && callbackPrecededWaterline &&
            retainedCompletionWeak.expired(),
        "receipt completion retains owners through the callback and runs it "
        "before publishing the completion waterline");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(queue) == 3,
        "merged completion must advance the FIFO waterline to the tail");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(queue) == 0,
        "finish iterations must reclaim all Arena and Legacy residency");
}

void productionLoopJoinsMultipleArenaSourcesOnStopDrain() {
  runProductionLoopStopDrainCase({true, true, true});
}

void productionLoopJoinsMixedSourcesOnStopDrain() {
  runProductionLoopStopDrainCase({true, false, true});
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate,
               std::chrono::milliseconds timeout =
                   std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void productionLoopPlansSeparateBThenASourcesIntoOneCarrier() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA510u;
  constexpr std::uint64_t kTargetB = 0xB510u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto headSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(headSource.size() == 1 && headSource.front().seqId == 1,
        "production planner fixture publishes head A alone");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffixSources;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "head A encodes before the deterministic suffix publication");
    check(backendState->plannerCalls == 0,
          "one Ready source starts naturally without waiting for a partner");

    fixture.replayStateOnly(100u);
    check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(queue) == 0,
          "StateOnly raw interposition publishes no CPU-ready source");
    fixture.publishArenaTargetDraw(101u, kTargetB);
    fixture.replayStateOnly(102u);
    check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(queue) == 1,
          "a second StateOnly raw gap leaves only B Ready");
    fixture.publishArenaTargetDraw(103u, kTargetA);
    suffixSources = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffixSources.size() == 2 && suffixSources[0].seqId == 2 &&
              suffixSources[1].seqId == 3,
          "B and A publish as one consecutive Ready window despite raw "
          "interposition");
    check(dxmt9::CommandQueueArenaLeaseTestAccess::readySourceIsArena(
              queue, suffixSources[0]) &&
              dxmt9::CommandQueueArenaLeaseTestAccess::readySourceIsArena(
                  queue, suffixSources[1]),
          "planned B and A suffix sources use direct Arena representation");
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 3;
          }),
          "multi-source planner encodes both selected fragments");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(backendState->plannerCalls == 1,
        "production loop invokes the bounded backend planning seam once");
  check(backendState->calls.size() == 3 &&
            backendState->calls[0].seqId == 1 &&
            backendState->calls[1].seqId == 3 &&
            backendState->calls[2].seqId == 2,
        "active A replays separate A before B with exact source seqIds");
  check(backendState->calls[1].fragment.has_value() &&
            backendState->calls[2].fragment.has_value() &&
            backendState->calls[1].fragment->commandBegin == 0 &&
            backendState->calls[1].fragment->commandCount == 1 &&
            backendState->calls[2].fragment->commandBegin == 0 &&
            backendState->calls[2].fragment->commandCount == 1 &&
            backendState->calls[1].skipBackendPlanning &&
            backendState->calls[2].skipBackendPlanning &&
            !backendState->calls[1].sessionSource.has_value() &&
            !backendState->calls[2].sessionSource.has_value(),
        "planned calls use whole-source pre-registered fragments exactly once");
  const obj_handle_t carrier = backendState->calls[0].commandBuffer;
  check(carrier != NULL_OBJECT_HANDLE &&
            backendState->calls[1].commandBuffer == carrier &&
            backendState->calls[2].commandBuffer == carrier,
        "source/window/run edges preserve one command-buffer carrier");
  check(backendState->renderPassBegins == 2 &&
            backendState->renderPassEnds == 2,
        "active A plus separate A|B uses two finalized passes, not three");
  check(backendState->encodedSeqIds ==
            std::vector<std::uint64_t>({1, 3, 2}),
        "draw callbacks preserve the planned source-command attribution");

  std::array expectedSources{
      headSource.front(), suffixSources[0], suffixSources[1]};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "one shutdown submission covers all planned sources");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size(),
        "planned carrier completion expands in natural FIFO order");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(queue) == 3,
        "planned completion advances the FIFO waterline to source A");
}

void productionLoopCanonicalizesNaturalCarrierBeforeReorderedComposite() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->observeFirstRecordSubmit = true;
  backendState->holdFirstReturn = true;
  backendState->holdSecondReturn = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA517u;
  constexpr std::uint64_t kTargetB = 0xB517u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto headSource = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(headSource.size() == 1u && headSource.front().seqId == 1u,
        "canonical carrier fixture publishes its A head alone");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> naturalSource;
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> plannedSources;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "canonical carrier head reaches its first natural encode");
    fixture.publishArenaTargetDraw(2u, kTargetA);
    naturalSource = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(naturalSource.size() == 1u && naturalSource.front().seqId == 2u,
          "second A is isolated as the natural source-local merge tail");
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return backendState->secondCallEncoded.load(
                std::memory_order_acquire);
          }),
          "second A enters natural source-local encoding before the suffix");
    check(backendState->plannerCalls == 0u,
          "two individually published A sources use no composite planner");

    fixture.publishArenaTargetDraw(3u, kTargetB);
    fixture.publishArenaTargetDraw(4u, kTargetA);
    plannedSources = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(plannedSources.size() == 2u &&
              plannedSources[0].seqId == 3u &&
              plannedSources[1].seqId == 4u,
          "B,A suffix is Ready together behind the natural merge");
    backendState->releaseSecondReturn.store(true,
                                            std::memory_order_release);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 4u;
          }),
          "canonicalized carrier admits both reordered suffix fragments");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    backendState->releaseSecondReturn.store(true,
                                            std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(backendState->plannerCalls == 1u &&
            backendState->compositeObserverCalls == 1u &&
            backendState->compositeObservedSeqIds ==
                std::vector<std::uint64_t>({3u, 4u}),
        "post-natural suffix executes one qualified composite transaction");
  check(backendState->calls.size() == 4u &&
            backendState->calls[0].seqId == 1u &&
            backendState->calls[1].seqId == 2u &&
            backendState->calls[2].seqId == 4u &&
            backendState->calls[3].seqId == 3u &&
            !backendState->calls[0].fragment.has_value() &&
            !backendState->calls[1].fragment.has_value() &&
            backendState->calls[2].fragment.has_value() &&
            backendState->calls[3].fragment.has_value() &&
            backendState->calls[2].skipBackendPlanning &&
            backendState->calls[3].skipBackendPlanning,
        "exact duplicate does not force Carrier fallback before A,B replay");
  const obj_handle_t carrier = backendState->calls.front().commandBuffer;
  check(carrier != NULL_OBJECT_HANDLE &&
            std::all_of(backendState->calls.begin(),
                        backendState->calls.end(),
                        [carrier](const auto& call) {
                          return call.commandBuffer == carrier;
                        }),
        "natural merge and reordered fragments retain one carrier owner");

  const std::array expectedSources{
      headSource.front(), naturalSource.front(), plannedSources[0],
      plannedSources[1]};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "finalization republishes all four canonical completion sources");
  check(backendState->firstRecordPostCommitRan.load(
            std::memory_order_acquire) &&
            backendState->backendCallsAtFirstRecordSubmit.load(
                std::memory_order_relaxed) == 4u,
        "initial carrier post-commit callback survives natural and fragment folds");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size(),
        "republished completion sources finish once in natural FIFO order");
}

void productionLoopPlansFreshRepeatedSourceWindow() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA512u;
  constexpr std::uint64_t kTargetB = 0xB512u;
  fixture.publishArenaTargetPair(10u, kTargetA, kTargetB);
  fixture.publishArenaTargetDraw(11u, kTargetA);
  const auto sources = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(sources.size() == 2 && sources[0].seqId == 1 &&
            sources[1].seqId == 2,
        "fresh repeated-source window is fully Ready before encoding");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  check(waitUntil([&] {
          return backendState->observedCalls.load(
                     std::memory_order_acquire) == 3;
        }),
        "fresh planner executes A(source0), A(source1), B(source0)");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  check(backendState->plannerCalls == 1 && backendState->calls.size() == 3,
        "fresh Ready prefix invokes one no-seed planner transaction");
  check(backendState->compositeObserverCalls == 1 &&
            backendState->compositeObservedSeqIds ==
                std::vector<std::uint64_t>({1u, 2u}),
        "fresh composite observes its natural FIFO sources exactly once");
  check(backendState->encodedSeqIds ==
            std::vector<std::uint64_t>({1u, 2u, 1u}) &&
            backendState->encodedCommandIndices ==
                std::vector<std::size_t>({0u, 0u, 1u}),
        "fresh no-seed replay executes the source-qualified repeated plan");
  check(backendState->calls[0].createdCommandBuffer &&
            !backendState->calls[1].createdCommandBuffer &&
            !backendState->calls[2].createdCommandBuffer,
        "first fresh fragment creates the carrier and later fragments inject it");
  check(backendState->calls[0].session != 0 &&
            backendState->calls[0].session == backendState->calls[1].session &&
            backendState->calls[0].session == backendState->calls[2].session &&
            backendState->calls[0].commandBuffer ==
                backendState->calls[1].commandBuffer &&
            backendState->calls[0].commandBuffer ==
                backendState->calls[2].commandBuffer,
        "fresh fragments share one new session and command buffer");
  check(backendState->midChunkSplits == 1 &&
            backendState->calls.back().returnedCommandBufferChainLength == 2,
        "fragment edges preserve the active split policy and produce one "
        "two-CB chain at the A-to-B pass edge");
  check(backendState->sourcePreambleSeqIds ==
            std::vector<std::uint64_t>({1u, 2u}) &&
            backendState->sourceEpilogueSeqIds ==
                std::vector<std::uint64_t>({2u, 1u}) &&
            backendState->transactionPreambles == 1,
        "fresh transaction runs source-wide hooks once and setup once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, sources),
        "fresh planned carrier submits both natural completion identities");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, sources) == sources.size(),
        "fresh planned completion expands in FIFO source order");
}

void productionLoopRetainsOneReadyHeadForExactWritingSuccessor() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5D0u;
  constexpr std::uint64_t kTargetB = 0xB5D0u;
  fixture.publishArenaTargetPair(60u, kTargetA, kTargetB);
  const auto older = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(older.size() == 1u && older.front().seqId == 1u,
        "retained-head fixture publishes A|B as the sole Ready source");
  fixture.beginLegacyTargetDraw(kTargetA);
  const auto before = dxmt9::CommandQueueArenaLeaseTestAccess::
      leaseAcquisitionCapacitySnapshot(queue);
  check(before.valid && before.olderUnavailable ==
            dxmt9::core::CpuReadyTape::LeaseCapacityClaim{} &&
            before.orderedTailWritingSuccessor.has_value(),
        "retained-head fixture exposes one exact ordered-tail writer");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  try {
    check(waitUntil([&] {
            return dxmt9::CommandQueueArenaLeaseTestAccess::sourceIsTentative(
                queue, older.front());
          }),
          "sole Ready head parks in pre-admission tentative state");
    const auto held = dxmt9::CommandQueueArenaLeaseTestAccess::
        leaseAcquisitionCapacitySnapshot(queue);
    check(held.valid && held.olderUnavailable.sources == 1u &&
              held.orderedTailWritingSuccessor.has_value() &&
              backendState->observedCalls.load(std::memory_order_acquire) ==
                  0u,
          "retained head owns no Metal work while the exact writer remains");

    check(dxmt9::CommandQueueArenaLeaseTestAccess::
              publishLegacyWritingSlotWithAdmissionPressure(queue),
          "exact Writing successor publishes with simultaneous pressure");
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 3u;
          }),
          "Ready successor dominates pressure and executes one A,A,B plan");
    dxmt9::CommandQueueArenaLeaseTestAccess::
        clearSyntheticAdmissionPressure(queue);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        clearSyntheticAdmissionPressure(queue);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(backendState->plannerCalls == 1u &&
            backendState->compositeObserverCalls == 1u &&
            backendState->encodedSeqIds ==
                std::vector<std::uint64_t>({1u, 2u, 1u}) &&
            backendState->encodedCommandIndices ==
                std::vector<std::size_t>({0u, 0u, 1u}),
        "whole-source retention exposes the cross-source A,A,B permutation");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, older),
        "retained head submits through the planned carrier");
}

void productionLoopConsumesOneReadyHeadBehindActiveSession() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5D1u;
  constexpr std::uint64_t kTargetB = 0xB5D1u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto head = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(head.size() == 1u && head.front().seqId == 1u,
        "active retained-head fixture publishes its initial A");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> retained;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "initial A enters the active session");
    fixture.publishArenaTargetPair(61u, kTargetA, kTargetB);
    retained = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(retained.size() == 1u && retained.front().seqId == 2u,
          "A|B is the sole Ready source behind active A");
    fixture.beginLegacyTargetDraw(kTargetA);
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);

    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 2u;
          }),
          "active session consumes the sole A|B head immediately");
    check(!dxmt9::CommandQueueArenaLeaseTestAccess::sourceIsTentative(
              queue, retained.front()),
          "active-session Ready head never enters the retained park state");

    check(dxmt9::CommandQueueArenaLeaseTestAccess::publishLegacyWritingSlot(
              queue),
          "later active-session Writing A publishes normally");
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 3u;
          }),
          "active carrier consumes the later A without retained lookahead");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(backendState->plannerCalls == 0u &&
            backendState->plannerFrontierStates.empty() &&
            backendState->plannerSourceCounts.empty() &&
            backendState->encodedSeqIds ==
                std::vector<std::uint64_t>({1u, 2u, 2u, 3u}) &&
            backendState->encodedCommandIndices ==
                std::vector<std::size_t>({0u, 0u, 1u, 0u}),
        "active Ready work preserves natural source order without a retained "
        "two-source planner window");
  const obj_handle_t carrier = backendState->calls.front().commandBuffer;
  check(carrier != NULL_OBJECT_HANDLE &&
            backendState->calls[1].commandBuffer == carrier,
        "immediate active head appends to the current command-buffer carrier");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, head) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
                queue, retained),
        "immediate active replay preserves FIFO completion authority");
}

void productionLoopRestoresRetainedHeadBeforeStopDrain() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));

  fixture.publishArenaClear(70u);
  const auto source = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(source.size() == 1u, "stop fallback fixture has one Ready head");
  queue.submitClear(ClearDesc{});

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::sourceIsTentative(
              queue, source.front());
        }),
        "stop fallback fixture reaches retained tentative state");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  check(backendState->observedBackendCalls.load(std::memory_order_acquire) ==
            1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
                queue, source),
        "shutdown restores and exactly replays the held head before drain");
}

void productionLoopRestoresRetainedHeadBeforeOrderedRelease() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));

  fixture.publishArenaClear(75u);
  const auto source = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(source.size() == 1u,
        "release fallback fixture has one Ready head");
  queue.submitClear(ClearDesc{});

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  try {
    check(waitUntil([&] {
            return dxmt9::CommandQueueArenaLeaseTestAccess::sourceIsTentative(
                queue, source.front());
          }),
          "release fallback fixture reaches retained tentative state");
    check(dxmt9::CommandQueueArenaLeaseTestAccess::postOrderedSubmit(
              queue, 75u, source.front().seqId),
          "ordered release posts against the exact held source");
    check(waitUntil([&] {
            return backendState->observedBackendCalls.load(
                       std::memory_order_acquire) == 1u &&
                dxmt9::CommandQueueArenaLeaseTestAccess::
                        acknowledgedSessionReleaseOrdinal(queue) != 0u;
          }),
          "ordered release restores, replays, submits, and acknowledges head");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, source),
        "ordered-release fallback preserves held-head completion authority");
}

void plannerUnlockRestoresExactPrefixBeforeOrderedRelease() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstPlanner = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));
  dxmt9::CommandQueueArenaLeaseTestAccess::
      pauseAfterStaleMultiSourcePlannerRestore(queue);

  constexpr std::uint64_t kTargetA = 0xA5C0u;
  constexpr std::uint64_t kTargetB = 0xB5C0u;
  fixture.publishArenaTargetPair(20u, kTargetA, kTargetB);
  fixture.publishArenaTargetDraw(21u, kTargetA);
  const auto sources = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(sources.size() == 2,
        "stale-planner fixture publishes one exact two-source prefix");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::atomic<bool> postFinished{false};
  bool postAccepted = false;
  std::thread postThread;
  try {
    check(waitUntil([&] {
            return backendState->firstPlannerEntered.load(
                std::memory_order_acquire);
          }),
          "planner barrier is reached with the prefix tentative");
    check(backendState->calls.empty() &&
              backendState->compositeObserverCalls == 0,
          "tentative planning has no backend effect or observer side effect");

    postThread = std::thread([&] {
      postAccepted =
          dxmt9::CommandQueueArenaLeaseTestAccess::postOrderedSubmit(
              queue, 21u, sources.back().seqId);
      postFinished.store(true, std::memory_order_release);
    });
    check(waitUntil([&] {
            return postFinished.load(std::memory_order_acquire);
          }),
          "ordered release posts while the planner remains blocked");
    check(postAccepted,
          "ordered release is accepted during out-of-lock planning");

    backendState->releaseFirstPlanner.store(true,
                                            std::memory_order_release);
    check(waitUntil([&] {
            return dxmt9::CommandQueueArenaLeaseTestAccess::
                pausedAfterStaleMultiSourcePlannerRestore(queue);
          }),
          "release mismatch restores the tentative prefix before restart");
    check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(queue) == 2 &&
              backendState->calls.empty() &&
              backendState->compositeObserverCalls == 0 &&
              dxmt9::CommandQueueArenaLeaseTestAccess::
                  hasPendingSessionRelease(queue),
          "stale plan restores exact FIFO with zero effects and leaves the "
          "release pending");
    dxmt9::CommandQueueArenaLeaseTestAccess::
        resumeAfterStaleMultiSourcePlannerRestore(queue);
    check(waitUntil([&] {
            return dxmt9::CommandQueueArenaLeaseTestAccess::
                       acknowledgedSessionReleaseOrdinal(queue) != 0 &&
                backendState->observedCalls.load(
                    std::memory_order_acquire) == 2;
          }),
          "restored sources replay naturally before the ordered fence acks");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    postThread.join();
  } catch (...) {
    backendState->releaseFirstPlanner.store(true,
                                            std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::
        resumeAfterStaleMultiSourcePlannerRestore(queue);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    if (encodeThread.joinable()) encodeThread.join();
    if (postThread.joinable()) postThread.join();
    throw;
  }

  check(backendState->plannerCalls == 1 &&
            backendState->compositeObserverCalls == 0 &&
            backendState->calls.size() == 2 &&
            backendState->calls[0].seqId == 1 &&
            backendState->calls[1].seqId == 2 &&
            !backendState->calls[0].fragment.has_value() &&
            !backendState->calls[1].fragment.has_value(),
        "the stale transaction is discarded and exact FIFO replays once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, sources),
        "the ordered natural replay submits the restored prefix");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, sources) == sources.size(),
        "the restored prefix remains FIFO-completable");
}

void compositeObserverDoesNotOwnSchedulingMutex() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstObserver = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5D0u;
  constexpr std::uint64_t kTargetB = 0xB5D0u;
  fixture.publishArenaTargetPair(30u, kTargetA, kTargetB);
  fixture.publishArenaTargetDraw(31u, kTargetA);
  const auto sources = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(sources.size() == 2,
        "observer-lock fixture publishes a planned two-source prefix");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::atomic<bool> postFinished{false};
  bool postAccepted = false;
  std::thread postThread;
  try {
    check(waitUntil([&] {
            return backendState->firstObserverEntered.load(
                std::memory_order_acquire);
          }),
          "composite observer barrier is reached after fragment effects");
    check(backendState->calls.size() == 3,
          "all qualified fragment effects finish before observation");
    postThread = std::thread([&] {
      postAccepted =
          dxmt9::CommandQueueArenaLeaseTestAccess::postOrderedSubmit(
              queue, 31u, sources.back().seqId);
      postFinished.store(true, std::memory_order_release);
    });
    check(waitUntil([&] {
            return postFinished.load(std::memory_order_acquire);
          }),
          "ordered release posts while composite observation is blocked");
    check(postAccepted,
          "observer executes without owning the scheduling mutex");
    backendState->releaseFirstObserver.store(true,
                                             std::memory_order_release);
    check(waitUntil([&] {
            return dxmt9::CommandQueueArenaLeaseTestAccess::
                acknowledgedSessionReleaseOrdinal(queue) != 0;
          }),
          "post-observer carrier installs before ordered release ack");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    postThread.join();
  } catch (...) {
    backendState->releaseFirstObserver.store(true,
                                             std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    if (encodeThread.joinable()) encodeThread.join();
    if (postThread.joinable()) postThread.join();
    throw;
  }

  check(backendState->plannerCalls == 1 &&
            backendState->compositeObserverCalls == 1 &&
            backendState->encodedSeqIds ==
                std::vector<std::uint64_t>({1u, 2u, 1u}),
        "observer unlock preserves one planner, one observer, and one "
        "qualified replay transaction");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, sources),
        "observer-unlocked transaction submits both sources once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, sources) == sources.size(),
        "observer-unlocked transaction remains FIFO-completable");
}

void productionLoopNaturalSourceKeepsDefaultPassSplitBaseline() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5A0u;
  constexpr std::uint64_t kTargetB = 0xB5A0u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto head = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(head.size() == 1, "natural split baseline publishes one head");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffix;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "natural split baseline opens the A session first");
    fixture.publishArenaTargetPair(120u, kTargetA, kTargetB);
    suffix = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffix.size() == 1,
          "natural split baseline publishes one A-to-B source");
    backendState->releaseFirstReturn.store(true, std::memory_order_release);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 2;
          }),
          "natural A-to-B source encodes on the carried session");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true, std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(backendState->plannerCalls == 0 && backendState->calls.size() == 2,
        "one natural successor does not enter multi-source planning");
  check(backendState->midChunkSplits == 1 &&
            backendState->calls.back().returnedCommandBufferChainLength == 2,
        "default PerRenderPass naturally commits once and returns a two-CB "
        "chain for A-to-B");

  std::array sources{head.front(), suffix.front()};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, sources),
        "natural split baseline submits both completion sources");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, sources) == sources.size(),
        "natural split baseline remains FIFO-completable");
}

struct TerminalSuffixProductionResult {
  std::vector<ProductionLoopBackendCall> calls;
  std::vector<std::uint64_t> encodedSeqIds;
  std::vector<std::size_t> encodedCommandIndices;
  std::vector<std::uint64_t> sourcePreambleSeqIds;
  std::vector<std::uint64_t> sourceEpilogueSeqIds;
  std::vector<std::uint64_t> compositeObservedSeqIds;
  std::size_t plannerCalls = 0;
  std::size_t compositeObserverCalls = 0;
  std::size_t renderPassBegins = 0;
  std::size_t renderPassEnds = 0;
  std::size_t midChunkSplits = 0;
  std::size_t transactionPreambles = 0;
  std::size_t firstRecordPostCommitRuns = 0;
  std::vector<std::size_t> storeProofLookaheadCounts;
  bool bothSourcesRetiredBeforeSubmit = false;
  std::size_t residentSourcesAfterFinish = 0;
};

TerminalSuffixProductionResult runTerminalSuffixProductionCase(
    bool joinEnabled, bool admissionPressure = false) {
  setenv("DXMT9_RENDERER_COMPAT_PROFILE", "progressive", 1);
  setenv("DXMT9_RENDERER_FEATURES",
         joinEnabled ? "passcoalesce" : "0", 1);

  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = joinEnabled;
  backendState->observeFirstRecordSubmit = joinEnabled;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5F0u;
  constexpr std::uint64_t kTargetB = 0xB5F0u;
  fixture.publishArenaTerminalSuffix(160u, kTargetA, kTargetB);
  const auto current = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(current.size() == 1u && current.front().seqId == 1u,
        "terminal-suffix fixture publishes one current source");
  std::optional<dxmt9::CommandQueue::CpuReadyArenaBuildLease>
      successorLease;
  if (admissionPressure) {
    fixture.beginLegacyTargetDraw(kTargetA);
  } else {
    successorLease.emplace(
        fixture.beginArenaTargetDraw(161u, kTargetA));
  }
  const auto writing = dxmt9::CommandQueueArenaLeaseTestAccess::
      leaseAcquisitionCapacitySnapshot(queue);
  check(writing.valid && writing.orderedTailWritingSuccessor.has_value(),
        "terminal-suffix fixture owns one exact Writing successor");

  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> successor;
  if (!joinEnabled) {
    check(successorLease->publish(),
          "default-off successor publishes before natural replay");
    successor = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(successor.size() == 2u && successor[1].seqId == 2u,
          "default-off fixture exposes the natural two-source window");
    successor.erase(successor.begin());
  }

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  bool bothSourcesRetiredBeforeSubmit = false;
  try {
    if (joinEnabled) {
      check(waitUntil([&] {
              return backendState->firstCallEncoded.load(
                  std::memory_order_acquire);
            }),
            "terminal-suffix join encodes the current A prefix before Ready");
      check(backendState->calls.size() == 1u &&
                backendState->calls.front().seqId == 1u &&
                backendState->calls.front().fragment.has_value() &&
                backendState->calls.front().fragment->commandBegin == 0u &&
                backendState->calls.front().fragment->commandCount == 1u &&
                backendState->calls.front().renderPassBeginsAfter == 1u &&
                backendState->calls.front().renderPassEndsAfter == 0u &&
                backendState->calls.front().createdCommandBuffer &&
                backendState->calls.front().returnedCommandBuffer !=
                    NULL_OBJECT_HANDLE &&
                backendState->calls.front()
                        .returnedCommandBufferChainLength == 1u &&
                backendState->encodedSeqIds ==
                    std::vector<std::uint64_t>({1u}) &&
                backendState->encodedCommandIndices ==
                    std::vector<std::size_t>({0u}),
            "the held current source snapshots one exact active-A frontier");
      check(!backendState->firstRecordPostCommitRan.load(
                std::memory_order_acquire) &&
                !dxmt9::CommandQueueArenaLeaseTestAccess::
                    hasActivePostEncodeReceipt(queue, 1u),
            "the prefix edge emits no sidecar or completion receipt");
      const bool published = admissionPressure
          ? dxmt9::CommandQueueArenaLeaseTestAccess::
                publishLegacyWritingSlotWithAdmissionPressure(queue)
          : successorLease->publish();
      check(published,
            admissionPressure
                ? "exact Ready successor publishes with admission pressure"
                : "terminal-suffix successor publishes after the prefix effect");
      successor = dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
      check(successor.size() == 1u && successor.front().seqId == 2u,
            "the exact successor becomes the sole Ready source");
      backendState->releaseFirstReturn.store(true,
                                             std::memory_order_release);
    }
    check(waitUntil([&] {
            return backendState->completedCalls.load(
                       std::memory_order_acquire) ==
                (joinEnabled ? 3u : 2u);
          }),
          "terminal-suffix production replay reaches its exact call count");
    bothSourcesRetiredBeforeSubmit = joinEnabled &&
        waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::
                     hasActivePostEncodeReceipt(queue, 1u) &&
              dxmt9::CommandQueueArenaLeaseTestAccess::
                  hasActivePostEncodeReceipt(queue, 2u);
        });
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    if (admissionPressure) {
      dxmt9::CommandQueueArenaLeaseTestAccess::
          clearSyntheticAdmissionPressure(queue);
    }
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  if (admissionPressure) {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        clearSyntheticAdmissionPressure(queue);
  }

  const std::array expectedSources{current.front(), successor.front()};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "terminal-suffix submission covers current then successor");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size() &&
            dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(queue) ==
                2u,
        "terminal-suffix completion remains natural FIFO exactly once");
  const auto finishedStats =
      dxmt9::CommandQueueArenaLeaseTestAccess::tapeStats(queue);

  setenv("DXMT9_RENDERER_FEATURES", "passcoalesce", 1);
  return TerminalSuffixProductionResult{
      .calls = backendState->calls,
      .encodedSeqIds = backendState->encodedSeqIds,
      .encodedCommandIndices = backendState->encodedCommandIndices,
      .sourcePreambleSeqIds = backendState->sourcePreambleSeqIds,
      .sourceEpilogueSeqIds = backendState->sourceEpilogueSeqIds,
      .compositeObservedSeqIds = backendState->compositeObservedSeqIds,
      .plannerCalls = backendState->plannerCalls,
      .compositeObserverCalls = backendState->compositeObserverCalls,
      .renderPassBegins = backendState->renderPassBegins,
      .renderPassEnds = backendState->renderPassEnds,
      .midChunkSplits = backendState->midChunkSplits,
      .transactionPreambles = backendState->transactionPreambles,
      .firstRecordPostCommitRuns =
          backendState->firstRecordPostCommitRuns.load(
              std::memory_order_relaxed),
      .storeProofLookaheadCounts =
          backendState->storeProofLookaheadCounts,
      .bothSourcesRetiredBeforeSubmit =
          bothSourcesRetiredBeforeSubmit,
      .residentSourcesAfterFinish = finishedStats.residentSources,
  };
}

void productionLoopJoinsDeferredTerminalSuffix() {
  const auto baseline = runTerminalSuffixProductionCase(false);
  const auto joined = runTerminalSuffixProductionCase(true);
  const auto pressured = runTerminalSuffixProductionCase(true, true);

  check(baseline.calls.size() == 2u &&
            std::none_of(baseline.calls.begin(), baseline.calls.end(),
                         [](const auto& call) {
                           return call.fragment.has_value();
                         }) &&
            baseline.encodedSeqIds ==
                std::vector<std::uint64_t>({1u, 1u, 2u}) &&
            baseline.encodedCommandIndices ==
                std::vector<std::size_t>({0u, 2u, 0u}),
        "default-off replay remains natural A,Clear(B),B then A");
  check(joined.calls.size() == 3u && joined.plannerCalls == 0u,
        "qualified exact replay bypasses the universal planner");
  check(pressured.calls.size() == 3u &&
            pressured.encodedSeqIds == joined.encodedSeqIds &&
            pressured.encodedCommandIndices == joined.encodedCommandIndices,
        "exact Ready admission wins simultaneous admission pressure");
  check(joined.compositeObserverCalls == 1u &&
            joined.compositeObservedSeqIds ==
                std::vector<std::uint64_t>({1u, 2u}),
        "qualified replay observes the natural FIFO transaction once");
  check(joined.encodedSeqIds ==
            std::vector<std::uint64_t>({1u, 2u, 1u}) &&
            joined.encodedCommandIndices ==
                std::vector<std::size_t>({0u, 0u, 2u}) &&
            joined.calls[1].fragment.has_value() &&
            joined.calls[1].fragment->commandBegin == 0u &&
            joined.calls[1].fragment->commandCount == 1u &&
            joined.calls[2].fragment.has_value() &&
            joined.calls[2].fragment->commandBegin == 1u &&
            joined.calls[2].fragment->commandCount == 2u,
        "qualified replay is exactly A,A,Clear(B),B");
  check(baseline.renderPassBegins == 3u &&
            baseline.renderPassEnds == 3u &&
            baseline.midChunkSplits == 1u,
        "default-off replay preserves three passes and one split");
  check(joined.renderPassBegins == 2u && joined.renderPassEnds == 2u,
        "terminal-suffix join removes one A pass");
  check(joined.storeProofLookaheadCounts.size() == 2u &&
            baseline.storeProofLookaheadCounts.size() == 3u &&
            joined.firstRecordPostCommitRuns == 1u,
        "joined Clear resolves two pass actions and preserves one sidecar");
  check(joined.midChunkSplits == 0u,
        "terminal-suffix join removes one command-buffer split");
  check(joined.sourcePreambleSeqIds ==
            std::vector<std::uint64_t>({1u, 2u}) &&
            joined.sourceEpilogueSeqIds ==
                std::vector<std::uint64_t>({2u, 1u}) &&
            joined.transactionPreambles == 1u,
        "joined fragments run source and transaction hooks exactly once");
  check(joined.bothSourcesRetiredBeforeSubmit &&
            joined.residentSourcesAfterFinish == 0u &&
            baseline.residentSourcesAfterFinish == 0u,
        "current then successor retire to receipts and FIFO finish reclaims "
        "all source residency");
}

enum class TerminalSuffixFallbackEvent {
  StaleSuccessor,
  OrderedRelease,
  Stop,
  CaptureBoundary,
};

void productionLoopDrainsDeferredTerminalSuffixFallbacks() {
  constexpr std::array cases{
      TerminalSuffixFallbackEvent::StaleSuccessor,
      TerminalSuffixFallbackEvent::OrderedRelease,
      TerminalSuffixFallbackEvent::Stop,
      TerminalSuffixFallbackEvent::CaptureBoundary,
  };

  for (const auto event : cases) {
    setenv("DXMT9_RENDERER_COMPAT_PROFILE", "progressive", 1);
    setenv("DXMT9_RENDERER_FEATURES", "passcoalesce", 1);
    RuntimeFixture fixture;
    auto& queue = fixture.routing->queue_;
    auto backendState = std::make_shared<PlannedProductionBackendState>();
    backendState->holdFirstReturn = true;
    backendState->observeFirstRecordSubmit = true;
    backendState->markFirstRecordCaptureBoundary =
        event == TerminalSuffixFallbackEvent::CaptureBoundary;
    auto backend = std::make_unique<PlannedProductionBackend>(backendState);
    dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
        queue, backend->drawRecorder());
    dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
        queue, std::move(backend));

    constexpr std::uint64_t kTargetA = 0xA6F0u;
    constexpr std::uint64_t kTargetB = 0xB6F0u;
    fixture.publishArenaTerminalSuffix(170u, kTargetA, kTargetB);
    const auto current = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(current.size() == 1u,
          "fallback fixture publishes one current source");
    auto writingSuccessor = fixture.beginArenaTargetDraw(171u, kTargetA);
    std::optional<dxmt9::CommandQueue::CpuReadyArenaBuildLease>
        successorLease{std::move(writingSuccessor)};

    std::thread encodeThread([&] {
      dxmt9::CommandQueueArenaLeaseTestAccess::
          runCpuReadySessionEncodeLoop(queue);
    });
    std::vector<dxmt9::core::metalqueue::QueueCompletionSource> successor;
    try {
      check(waitUntil([&] {
              return backendState->firstCallEncoded.load(
                  std::memory_order_acquire);
            }),
            "fallback fixture reaches the held prefix edge");
      check(!backendState->firstRecordPostCommitRan.load(
                std::memory_order_acquire),
            "held prefix publishes no edge action, sidecar, or completion");

      if (event == TerminalSuffixFallbackEvent::StaleSuccessor) {
        check(successorLease->publish(),
              "stale fixture publishes the exact successor");
        successor = dxmt9::CommandQueueArenaLeaseTestAccess::
            snapshotReadyCompletionSources(queue);
        check(successor.size() == 1u && successor[0].seqId == 2u,
              "stale fixture exposes the successor alone");
        dxmt9::CommandQueueArenaLeaseTestAccess::
            restoreNextTentativePreflightAndReturn(queue);
      } else if (event == TerminalSuffixFallbackEvent::OrderedRelease) {
        check(dxmt9::CommandQueueArenaLeaseTestAccess::postOrderedSubmit(
                  queue, 170u, 1u),
              "ordered-release fixture posts the held-current fence");
      } else if (event == TerminalSuffixFallbackEvent::Stop) {
        dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
      }
      backendState->releaseFirstReturn.store(true,
                                             std::memory_order_release);

      const std::size_t expectedCalls =
          event == TerminalSuffixFallbackEvent::StaleSuccessor ? 3u : 2u;
      check(waitUntil([&] {
              return backendState->completedCalls.load(
                         std::memory_order_acquire) == expectedCalls;
            }),
            "fallback fixture drains the current suffix before younger "
            "effects");
      if (event != TerminalSuffixFallbackEvent::Stop) {
        dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
      }
      encodeThread.join();
    } catch (...) {
      backendState->releaseFirstReturn.store(true,
                                             std::memory_order_release);
      dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
      encodeThread.join();
      throw;
    }
    successorLease.reset();

    check(backendState->calls.size() >= 2u &&
              backendState->calls[0].fragment.has_value() &&
              backendState->calls[1].fragment.has_value() &&
              backendState->calls[0].fragment->transactionFragmentOrdinal ==
                  0u &&
              backendState->calls[0].fragment->transactionFragmentCount ==
                  3u &&
              backendState->calls[1].fragment->transactionFragmentOrdinal ==
                  1u &&
              backendState->calls[1].fragment->transactionFragmentCount ==
                  2u,
          "fallback pins the harmless provisional 3-to-natural-2 fragment "
          "notation");
    check(backendState->compositeObserverCalls == 0u &&
              backendState->encodedSeqIds.size() >= 2u &&
              backendState->encodedSeqIds[0] == 1u &&
              backendState->encodedSeqIds[1] == 1u &&
              backendState->transactionPreambles >= 1u,
          "fallback emits no joined observer and drains current naturally");

    if (event == TerminalSuffixFallbackEvent::StaleSuccessor) {
      check(backendState->calls.size() == 3u &&
                !backendState->calls[2].fragment.has_value() &&
                backendState->encodedSeqIds ==
                    std::vector<std::uint64_t>({1u, 1u, 2u}),
            "stale tentative successor restores before its natural replay");
      const std::array sources{current[0], successor[0]};
      check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
                queue, sources) &&
                dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
                    queue, sources) == sources.size(),
            "stale rollback completes current then successor exactly once");
    } else {
      const std::array sources{current[0]};
      check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
                queue, sources) &&
                dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
                    queue, sources) == sources.size(),
            "real drain completes the current source exactly once");
    }
    if (event == TerminalSuffixFallbackEvent::CaptureBoundary) {
      check(backendState->calls.size() == 2u &&
                backendState->compositeObserverCalls == 0u &&
                backendState->encodedSeqIds ==
                    std::vector<std::uint64_t>({1u, 1u}) &&
                backendState->encodedCommandIndices ==
                    std::vector<std::size_t>({0u, 2u}),
            "pending-record capture boundary revalidates and drains the "
            "natural Clear suffix");
    }
    check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(queue) == 0u,
          "fallback FIFO finish reclaims every published source");
  }
}

void productionLoopAttributesNaturalFallbackAbaWithinOneWindow() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  dxmt9::framegraph::MultiSourceReplayPlan forcedPlan{};
  forcedPlan.disposition =
      dxmt9::framegraph::MultiSourceReplayDisposition::NaturalFifo;
  forcedPlan.validation =
      dxmt9::framegraph::MultiSourceReplayValidation::Valid;
  forcedPlan.diagnostics.outcome =
      dxmt9::framegraph::MultiSourcePlannerOutcome::NaturalAfterMerge;
  forcedPlan.diagnostics.merge =
      dxmt9::framegraph::MultiSourceMergeDiagnostic::NonSeedOnly;
  backendState->forcedPlan = forcedPlan;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5E0u;
  constexpr std::uint64_t kTargetB = 0xB5E0u;
  fixture.publishArenaTargetDraw(140u, kTargetA);
  fixture.publishArenaTargetDraw(141u, kTargetB);
  fixture.publishArenaTargetDraw(142u, kTargetA);
  const auto sources = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(sources.size() == 3u,
        "natural attribution fixture publishes one complete A-B-A window");

  const auto fallbackBefore = dxmt9::perf::test::
      snapshotCpuReadyMultiSourceSourceLocalFallback();
  const auto passBefore = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  check(waitUntil([&] {
          return backendState->observedCalls.load(
                     std::memory_order_acquire) == 3u;
        }),
        "natural attribution fixture source-locally encodes every source");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  const auto fallbackAfter = dxmt9::perf::test::
      snapshotCpuReadyMultiSourceSourceLocalFallback();
  const auto passAfter = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();
  check(backendState->plannerCalls == 1u &&
            backendState->compositeObserverCalls == 0u &&
            backendState->calls.size() == 3u,
        "NaturalAfterMerge remains on the source-local fallback path");
  const std::uint64_t windowId =
      backendState->calls.front().replayWindow.windowId;
  check(windowId != 0u,
        "natural fallback provenance uses the first source ordinal");
  for (std::size_t i = 0; i < backendState->calls.size(); ++i) {
    const auto& call = backendState->calls[i];
    check(call.replayWindow.valid() &&
              call.replayWindow.disposition == dxmt9::encoders::
                  ReplayWindowDisposition::NaturalAfterMergeFallback &&
              call.replayWindow.windowId == windowId &&
              call.replayWindow.sourceIndex == i &&
              call.replayWindow.sourceCount == 3u &&
              !call.fragment.has_value() && !call.skipBackendPlanning,
          "every natural fallback source carries one stable window identity");
  }
  check(fallbackAfter.naturalStarted - fallbackBefore.naturalStarted == 1u &&
            fallbackAfter.naturalCompleted -
                    fallbackBefore.naturalCompleted ==
                1u &&
            fallbackAfter.naturalSources - fallbackBefore.naturalSources ==
                3u,
        "natural fallback started/completed/source counters conserve one window");
  check(passAfter.begins - passBefore.begins == 3u &&
            passAfter.sameWindowDistance1 -
                    passBefore.sameWindowDistance1 ==
                1u &&
            passAfter.sameWindowDistance2 == passBefore.sameWindowDistance2 &&
            passAfter.sameWindowDistance3To4 ==
                passBefore.sameWindowDistance3To4 &&
            passAfter.crossWindowDistance1 ==
                passBefore.crossWindowDistance1 &&
            passAfter.crossWindowDistance2 ==
                passBefore.crossWindowDistance2 &&
            passAfter.crossWindowDistance3To4 ==
                passBefore.crossWindowDistance3To4,
        "physical A-B-A is attributed once to its natural fallback window");

  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, sources),
        "natural attribution does not alter final session submission");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, sources) == sources.size(),
        "natural attribution preserves FIFO completion");
}

void runProductionLoopAttributesExactActiveSeedBridge(bool exactTarget) {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  dxmt9::framegraph::MultiSourceReplayPlan forcedPlan{};
  forcedPlan.disposition =
      dxmt9::framegraph::MultiSourceReplayDisposition::NaturalFifo;
  forcedPlan.validation =
      dxmt9::framegraph::MultiSourceReplayValidation::Valid;
  forcedPlan.diagnostics.outcome =
      dxmt9::framegraph::MultiSourcePlannerOutcome::NaturalAfterMerge;
  forcedPlan.diagnostics.merge =
      dxmt9::framegraph::MultiSourceMergeDiagnostic::SeedMerged;
  forcedPlan.diagnostics.activeSeedMergeCount = 1u;
  forcedPlan.diagnostics.activeSeedMergeWitnesses.push_back(
      dxmt9::encoders::ActiveSeedMergeTargetWitness{
          .retainedSourceIndex = exactTarget ? 1u : 0u,
          .commandIndex = 0u,
          .mergeOrdinal = 0u,
          .mergeDistance = 1u,
      });
  backendState->forcedPlan = forcedPlan;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5E2u;
  constexpr std::uint64_t kTargetB = 0xB5E2u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto head = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(head.size() == 1u, "seed bridge fixture publishes active A first");
  const auto before = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffix;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "active A is encoded before publishing the B,A suffix");
    fixture.publishArenaTargetDraw(145u, kTargetB);
    fixture.publishArenaTargetDraw(146u, kTargetA);
    suffix = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffix.size() == 2u, "seed bridge fixture publishes B,A suffix");
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 3u;
          }),
          "active seed fallback encodes A then B,A");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  const auto after = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();
  check(backendState->plannerCalls == 1u &&
            backendState->calls.size() == 3u,
        "active seed bridge uses one bounded suffix planner window");
  const auto& ticketCall = backendState->calls[exactTarget ? 2u : 1u];
  check(ticketCall.activeSeedMergeTicket.valid() &&
            ticketCall.activeSeedMergeTargets.size() == 1u &&
            ticketCall.activeSeedMergeTargets[0].retainedSourceIndex ==
                (exactTarget ? 1u : 0u),
        "queue hands the ticket only to its exact retained source");
  check(after.seedTicketsIssued - before.seedTicketsIssued == 1u,
        "one planner witness issues exactly one encode ticket");
  if (exactTarget) {
    check(after.seedTicketsMatched - before.seedTicketsMatched == 1u &&
              after.seedTicketsContinued == before.seedTicketsContinued &&
              after.seedTicketsMismatch == before.seedTicketsMismatch &&
              after.seedTicketsUnconsumed == before.seedTicketsUnconsumed &&
              after.seedBridgeDistance1 - before.seedBridgeDistance1 == 1u,
          "exact A|B,A physical token joins one d1 seed bridge");
  } else {
    check(after.seedTicketsMatched == before.seedTicketsMatched &&
              after.seedTicketsContinued == before.seedTicketsContinued &&
              after.seedTicketsMismatch - before.seedTicketsMismatch == 1u &&
              after.seedTicketsUnconsumed == before.seedTicketsUnconsumed &&
              after.seedBridgeDistance1 == before.seedBridgeDistance1,
          "wrong target pass fails closed as one consumed ticket mismatch");
  }
  std::array allSources{head.front(), suffix[0], suffix[1]};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, allSources),
        "seed attribution does not alter submission ownership");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, allSources) == allSources.size(),
        "seed attribution preserves FIFO completion");
}

void productionLoopAttributesExactActiveSeedBridge() {
  runProductionLoopAttributesExactActiveSeedBridge(true);
}

void productionLoopRejectsWrongActiveSeedBridgeTarget() {
  runProductionLoopAttributesExactActiveSeedBridge(false);
}

void productionLoopAttributesExactActiveSeedContinuation() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  dxmt9::framegraph::MultiSourceReplayPlan forcedPlan{};
  forcedPlan.disposition =
      dxmt9::framegraph::MultiSourceReplayDisposition::NaturalFifo;
  forcedPlan.validation =
      dxmt9::framegraph::MultiSourceReplayValidation::Valid;
  forcedPlan.diagnostics.outcome =
      dxmt9::framegraph::MultiSourcePlannerOutcome::NaturalAfterMerge;
  forcedPlan.diagnostics.merge =
      dxmt9::framegraph::MultiSourceMergeDiagnostic::SeedMerged;
  forcedPlan.diagnostics.activeSeedMergeCount = 1u;
  forcedPlan.diagnostics.activeSeedMergeWitnesses.push_back(
      dxmt9::encoders::ActiveSeedMergeTargetWitness{
          .retainedSourceIndex = 0u,
          .commandIndex = 0u,
          .mergeOrdinal = 0u,
          .mergeDistance = 1u,
      });
  backendState->forcedPlan = forcedPlan;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5E3u;
  constexpr std::uint64_t kTargetB = 0xB5E3u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto head = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  const auto before = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffix;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "continuation fixture encodes active A first");
    fixture.publishArenaTargetDraw(147u, kTargetA);
    fixture.publishArenaTargetDraw(148u, kTargetB);
    suffix = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffix.size() == 2u,
          "continuation fixture publishes immediate A then B");
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 3u;
          }),
          "continuation fixture consumes both suffix sources");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }
  const auto after = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();
  check(backendState->calls.size() == 3u &&
            backendState->calls[1].activeSeedMergeTicket.valid() &&
            backendState->calls[1].activeSeedMergeTargets.size() == 1u,
        "queue hands the adjacent target to the immediate A source");
  check(after.seedTicketsIssued - before.seedTicketsIssued == 1u &&
            after.seedTicketsContinued - before.seedTicketsContinued == 1u &&
            after.seedTicketsMatched == before.seedTicketsMatched &&
            after.seedTicketsMismatch == before.seedTicketsMismatch &&
            after.seedTicketsUnconsumed == before.seedTicketsUnconsumed &&
            after.seedBridgeDistance1 == before.seedBridgeDistance1,
        "active A plus immediate A conserves as one continued ticket");
  std::array allSources{head.front(), suffix[0], suffix[1]};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, allSources),
        "continuation attribution preserves submission ownership");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, allSources) == allSources.size(),
        "continuation attribution preserves FIFO completion");
}

void productionLoopDropsOnlyTicketWhenActiveSeedInstanceTurnsStale() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  backendState->holdFirstPlanner = true;
  dxmt9::framegraph::MultiSourceReplayPlan forcedPlan{};
  forcedPlan.disposition =
      dxmt9::framegraph::MultiSourceReplayDisposition::NaturalFifo;
  forcedPlan.validation =
      dxmt9::framegraph::MultiSourceReplayValidation::Valid;
  forcedPlan.diagnostics.outcome =
      dxmt9::framegraph::MultiSourcePlannerOutcome::NaturalAfterMerge;
  forcedPlan.diagnostics.merge =
      dxmt9::framegraph::MultiSourceMergeDiagnostic::SeedMerged;
  forcedPlan.diagnostics.activeSeedMergeCount = 1u;
  forcedPlan.diagnostics.activeSeedMergeWitnesses.push_back(
      dxmt9::encoders::ActiveSeedMergeTargetWitness{
          .retainedSourceIndex = 1u,
          .commandIndex = 0u,
          .mergeOrdinal = 0u,
          .mergeDistance = 1u,
      });
  backendState->forcedPlan = forcedPlan;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5E4u;
  constexpr std::uint64_t kTargetB = 0xB5E4u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto head = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  const auto before = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffix;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "stale token fixture encodes active A first");
    fixture.publishArenaTargetDraw(149u, kTargetB);
    fixture.publishArenaTargetDraw(150u, kTargetA);
    suffix = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffix.size() == 2u,
          "stale token fixture publishes one B,A suffix window");
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return backendState->firstPlannerEntered.load(
                std::memory_order_acquire);
          }),
          "stale token fixture pauses outside the scheduling lock");
    dxmt9::CommandQueueArenaLeaseTestAccess::
        overrideLiveActiveRenderInstance(queue, 999u, 777u);
    backendState->releaseFirstPlanner.store(true,
                                            std::memory_order_release);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 3u;
          }),
          "token-only mismatch preserves source-local replay progress");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    backendState->releaseFirstPlanner.store(true,
                                            std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }
  const auto after = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();
  check(backendState->plannerCalls == 1u &&
            backendState->calls.size() == 3u &&
            !backendState->calls[1].activeSeedMergeTicket.valid() &&
            !backendState->calls[2].activeSeedMergeTicket.valid(),
        "token-only mismatch keeps the accepted plan but publishes no ticket");
  check(after.seedTicketsIssued == before.seedTicketsIssued &&
            after.seedInstanceStale - before.seedInstanceStale == 1u &&
            after.seedInstanceUnavailable == before.seedInstanceUnavailable,
        "perf-on stale token is visible without issuing terminal ownership");
  std::array allSources{head.front(), suffix[0], suffix[1]};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, allSources),
        "stale diagnostic token does not restore or lose the selected prefix");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, allSources) == allSources.size(),
        "stale diagnostic token preserves FIFO completion");
}

void productionLoopPerfOffKeepsSeedPlanWithoutTicketWork() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  dxmt9::framegraph::MultiSourceReplayPlan forcedPlan{};
  forcedPlan.disposition =
      dxmt9::framegraph::MultiSourceReplayDisposition::NaturalFifo;
  forcedPlan.validation =
      dxmt9::framegraph::MultiSourceReplayValidation::Valid;
  forcedPlan.diagnostics.outcome =
      dxmt9::framegraph::MultiSourcePlannerOutcome::NaturalAfterMerge;
  forcedPlan.diagnostics.merge =
      dxmt9::framegraph::MultiSourceMergeDiagnostic::SeedMerged;
  forcedPlan.diagnostics.activeSeedMergeCount = 1u;
  forcedPlan.diagnostics.activeSeedMergeWitnesses.push_back(
      dxmt9::encoders::ActiveSeedMergeTargetWitness{
          .retainedSourceIndex = 1u,
          .commandIndex = 0u,
          .mergeOrdinal = 0u,
          .mergeDistance = 1u,
      });
  backendState->forcedPlan = forcedPlan;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA5E5u;
  constexpr std::uint64_t kTargetB = 0xB5E5u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto head = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffix;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "perf-off fixture encodes active A first");
    fixture.publishArenaTargetDraw(151u, kTargetB);
    fixture.publishArenaTargetDraw(152u, kTargetA);
    suffix = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffix.size() == 2u,
          "perf-off fixture publishes one B,A suffix window");
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 3u;
          }),
          "perf-off fixture preserves source-local planner progress");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }
  check(backendState->plannerCalls == 1u &&
            backendState->calls.size() == 3u,
        "perf-off keeps the same accepted NaturalAfterMerge plan");
  for (const auto& call : backendState->calls) {
    check(!call.activeSeedMergeTicket.valid() &&
              call.activeSeedMergeTargets.empty(),
          "perf-off publishes no ticket context or target span");
  }
  std::array allSources{head.front(), suffix[0], suffix[1]};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, allSources),
        "perf-off attribution leaves the selected prefix committed");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, allSources) == allSources.size(),
        "perf-off attribution preserves FIFO completion");
}

void productionLoopAttributesPermutationRejectedFallbackWindow() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  dxmt9::framegraph::MultiSourceReplayPlan forcedPlan{};
  forcedPlan.disposition =
      dxmt9::framegraph::MultiSourceReplayDisposition::NaturalFifo;
  forcedPlan.validation =
      dxmt9::framegraph::MultiSourceReplayValidation::Valid;
  forcedPlan.diagnostics.outcome =
      dxmt9::framegraph::MultiSourcePlannerOutcome::PermutationRejected;
  backendState->forcedPlan = forcedPlan;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  fixture.publishArenaTargetDraw(143u, 0xA5E1u);
  fixture.publishArenaTargetDraw(144u, 0xB5E1u);
  const auto sources = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(sources.size() == 2u,
        "permutation-rejected fixture publishes one complete window");

  const auto before = dxmt9::perf::test::
      snapshotCpuReadyMultiSourceSourceLocalFallback();
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  check(waitUntil([&] {
          return backendState->observedCalls.load(
                     std::memory_order_acquire) == 2u;
        }),
        "permutation-rejected fallback encodes every source");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  const auto after = dxmt9::perf::test::
      snapshotCpuReadyMultiSourceSourceLocalFallback();
  check(backendState->plannerCalls == 1u &&
            backendState->compositeObserverCalls == 0u &&
            backendState->calls.size() == 2u,
        "PermutationRejected remains on the source-local fallback path");
  const std::uint64_t windowId =
      backendState->calls.front().replayWindow.windowId;
  for (std::size_t i = 0; i < backendState->calls.size(); ++i) {
    const auto& provenance = backendState->calls[i].replayWindow;
    check(provenance.valid() &&
              provenance.disposition == dxmt9::encoders::
                  ReplayWindowDisposition::PermutationRejectedFallback &&
              provenance.windowId == windowId &&
              provenance.sourceIndex == i && provenance.sourceCount == 2u,
          "permutation-rejected sources carry one stable window identity");
  }
  check(windowId != 0u &&
            after.permutationStarted - before.permutationStarted == 1u &&
            after.permutationCompleted - before.permutationCompleted == 1u &&
            after.permutationSources - before.permutationSources == 2u,
        "permutation-rejected fallback counters conserve one window");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, sources) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
                queue, sources) == sources.size(),
        "permutation-rejected attribution preserves FIFO completion");
}

void productionLoopBoundsFreshNineReadySources() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA612u;
  constexpr std::uint64_t kTargetB = 0xB612u;
  fixture.publishArenaTargetDraw(1u, kTargetA);
  fixture.publishArenaTargetDraw(2u, kTargetB);
  fixture.publishArenaTargetDraw(3u, kTargetA);
  for (std::uint64_t i = 4; i <= 9; ++i) {
    fixture.publishArenaTargetDraw(i, 0xC612u + i);
  }
  const auto sources = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(sources.size() == 9,
        "fresh bounded fixture publishes nine Ready sources");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  check(waitUntil([&] {
          return backendState->observedCalls.load(
                     std::memory_order_acquire) == 9;
        }),
        "fresh bounded transaction and ninth suffix both encode");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  check(backendState->plannerCalls == 1 &&
            backendState->calls[0].seqId == 1 &&
            backendState->calls[1].seqId == 3 &&
            backendState->calls[2].seqId == 2 &&
            backendState->calls.back().seqId == 9,
        "fresh planner reorders only the bounded first eight-source prefix");
  for (std::size_t i = 0; i < 8; ++i) {
    check(backendState->calls[i].fragment.has_value(),
          "each source in the bounded fresh prefix uses a planned fragment");
  }
  check(!backendState->calls[8].fragment.has_value(),
        "the ninth source remains outside the proof window and replays naturally");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, sources),
        "bounded fresh plan and suffix submit every source once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, sources) == sources.size(),
        "bounded fresh completion remains natural FIFO");
}

void productionLoopRevisitsOneSourceWithoutRepeatingItsPreamble() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA515u;
  constexpr std::uint64_t kTargetB = 0xB515u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto headSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(headSource.size() == 1 && headSource.front().seqId == 1,
        "repeated-source fixture publishes its active A head alone");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffixSources;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "repeated-source fixture opens the active A session");
    fixture.publishArenaTargetPair(110u, kTargetA, kTargetB);
    fixture.publishArenaTargetDraw(111u, kTargetA);
    suffixSources = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffixSources.size() == 2 && suffixSources[0].seqId == 2 &&
              suffixSources[1].seqId == 3,
          "A,B and returning A publish as two Ready sources");
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 4;
          }),
          "qualified A|A|B replay revisits the older source");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(backendState->plannerCalls == 1,
        "repeated-source window invokes one bounded planner transaction");
  check(backendState->compositeObserverCalls == 1 &&
            backendState->compositeObservedSeqIds ==
                std::vector<std::uint64_t>({2u, 3u}),
        "repeated-source composite observation runs once in natural order");
  check(backendState->calls.size() == 4 &&
            backendState->calls[0].seqId == 1 &&
            backendState->calls[1].seqId == 2 &&
            backendState->calls[2].seqId == 3 &&
            backendState->calls[3].seqId == 2,
        "planned replay is head A, older A, newer A, older B");
  check(backendState->calls[1].fragment.has_value() &&
            backendState->calls[2].fragment.has_value() &&
            backendState->calls[3].fragment.has_value() &&
            backendState->calls[1].fragment->firstSourceFragment() &&
            !backendState->calls[1].fragment->lastSourceFragment() &&
            backendState->calls[3].fragment->lastSourceFragment() &&
            backendState->calls[1].fragment->firstTransactionFragment() &&
            !backendState->calls[2].fragment->firstTransactionFragment(),
        "fragment phases identify one repeated source transaction exactly");
  check(backendState->calls[1].lookaheadCount == 3u &&
            backendState->calls[2].lookaheadCount == 2u &&
            backendState->calls[3].lookaheadCount == 1u,
        "planned fragments receive the exact replay-order lookahead suffix");
  const auto& firstLookahead = backendState->calls[1].lookaheadSources;
  const auto& secondLookahead = backendState->calls[2].lookaheadSources;
  const auto& thirdLookahead = backendState->calls[3].lookaheadSources;
  check(firstLookahead.size() == 3u &&
            firstLookahead[0].seqId == 2u &&
            firstLookahead[0].commandBegin == 0u &&
            firstLookahead[0].commandCount == 1u &&
            firstLookahead[1].seqId == 3u &&
            firstLookahead[1].commandBegin == 0u &&
            firstLookahead[1].commandCount == 1u &&
            firstLookahead[2].seqId == 2u &&
            firstLookahead[2].commandBegin == 1u &&
            firstLookahead[2].commandCount == 1u &&
            secondLookahead.size() == 2u &&
            secondLookahead[0].seqId == 3u &&
            secondLookahead[1].seqId == 2u &&
            secondLookahead[1].commandBegin == 1u &&
            thirdLookahead.size() == 1u &&
            thirdLookahead[0].seqId == 2u &&
            thirdLookahead[0].commandBegin == 1u,
        "lookahead ranges follow A0|A1|B0 replay rather than natural full "
        "sources");
  const auto lookaheadCommandCount = [](const auto& sources) {
    std::size_t count = 0;
    for (const auto& source : sources) {
      count += source.commandCount;
    }
    return count;
  };
  check(lookaheadCommandCount(firstLookahead) == 3u &&
            lookaheadCommandCount(secondLookahead) == 2u &&
            lookaheadCommandCount(thirdLookahead) == 1u,
        "GPU sampling sees each replay command exactly once in the suffix");
  check(backendState->sourcePreambleSeqIds ==
            std::vector<std::uint64_t>({1u, 2u, 3u}),
        "capture/source preamble executes once per source, not per fragment");
  check(backendState->sourceEpilogueSeqIds ==
            std::vector<std::uint64_t>({1u, 3u, 2u}),
        "source epilogue emits once at each source's final fragment");
  check(backendState->transactionPreambles == 2,
        "ordinary head and one composite window each flush initializer setup once");
  check(backendState->encodedSeqIds ==
            std::vector<std::uint64_t>({1u, 2u, 3u, 2u}) &&
            backendState->encodedCommandIndices ==
                std::vector<std::size_t>({0u, 0u, 0u, 1u}),
        "draw attribution preserves the source-qualified A,A,B permutation");
  const obj_handle_t carrier = backendState->calls[0].commandBuffer;
  check(carrier != NULL_OBJECT_HANDLE &&
            backendState->calls[1].commandBuffer == carrier &&
            backendState->calls[2].commandBuffer == carrier &&
            backendState->calls[3].commandBuffer == carrier,
        "repeated source runs retain one command-buffer carrier");

  std::array expectedSources{headSource.front(), suffixSources[0],
                             suffixSources[1]};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "one final submission covers every repeated-window source once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size(),
        "repeated replay still expands completion in natural FIFO order");
}

void productionLoopStoreProofLookaheadOverflowFailsClosed() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  dxmt9::framegraph::MultiSourceReplayPlan forcedPlan{};
  forcedPlan.disposition =
      dxmt9::framegraph::MultiSourceReplayDisposition::Planned;
  forcedPlan.validation =
      dxmt9::framegraph::MultiSourceReplayValidation::Valid;
  forcedPlan.diagnostics.outcome =
      dxmt9::framegraph::MultiSourcePlannerOutcome::Planned;
  constexpr std::size_t kSourceCount = 8u;
  constexpr std::size_t kCommandsPerSource = 5u;
  constexpr std::size_t kTotalCommands =
      kSourceCount * kCommandsPerSource;
  forcedPlan.commands.reserve(kTotalCommands);
  for (std::uint32_t command = 0; command < kCommandsPerSource; ++command) {
    for (std::uint32_t source = 0; source < kSourceCount; ++source) {
      forcedPlan.commands.push_back({.retainedSourceIndex = source,
                                     .commandIndex = command});
    }
  }
  backendState->forcedPlan = std::move(forcedPlan);
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  for (std::size_t source = 0; source < kSourceCount; ++source) {
    std::array<std::uint64_t, kCommandsPerSource> targets{};
    targets.fill(0xA516u + source);
    fixture.publishArenaTargetSequence(130u + source, targets);
  }
  const auto sources = dxmt9::CommandQueueArenaLeaseTestAccess::
      snapshotReadyCompletionSources(queue);
  check(sources.size() == kSourceCount,
        "overflow fixture publishes one complete bounded Ready window");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  check(waitUntil([&] {
          return backendState->observedCalls.load(
                     std::memory_order_acquire) ==
              kTotalCommands;
        }),
        "overflow fixture executes every source-qualified fragment");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  check(backendState->calls.size() == kTotalCommands &&
            backendState->calls[0].lookaheadCount == 40u &&
            backendState->calls[1].lookaheadCount == 39u &&
            backendState->calls[8].lookaheadCount == 32u &&
            backendState->calls.back().lookaheadCount == 1u,
        "fragment calls receive complete replay suffixes across the fixed "
        "store-proof bound");
  std::size_t firstSuffixCommands = 0;
  for (const auto& source : backendState->calls[0].lookaheadSources) {
    firstSuffixCommands += source.commandCount;
  }
  check(firstSuffixCommands == kTotalCommands,
        "GPU sampling command sizing counts every fragmented command once");
  check(backendState->storeProofLookaheadCounts.size() ==
            kTotalCommands &&
            backendState->storeProofLookaheadCounts[0] == 1u &&
            backendState->storeProofLookaheadCounts[1] == 1u &&
            backendState->storeProofLookaheadCounts[7] == 1u &&
            backendState->storeProofLookaheadCounts[8] == 32u,
        "store proof rejects an oversized cross-source suffix but preserves "
        "the current-source next-touch proof");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, sources),
        "overflow fallback still submits both sources exactly once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, sources) == sources.size(),
        "overflow fallback preserves FIFO completion");
}

void productionLoopOrderedReleaseFencesRawInterposition() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->observeFirstRecordSubmit = true;
  backendState->holdFirstReturn = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA520u;
  constexpr std::uint64_t kTargetB = 0xB520u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto headSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(headSource.size() == 1 && headSource.front().seqId == 1,
        "ordered-release fixture publishes one Legacy head");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffixSources;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "ordered-release fixture encodes the Legacy head first");

    fixture.replayStateOnly(200u);
    check(dxmt9::CommandQueueArenaLeaseTestAccess::postOrderedSubmit(
              queue, 200u, headSource.front().seqId),
          "ordered SessionRelease posts at the StateOnly raw fence");
    fixture.publishArenaTargetDraw(201u, kTargetB);
    fixture.replayStateOnly(202u);
    fixture.publishArenaTargetDraw(203u, kTargetA);
    suffixSources = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffixSources.size() == 2 && suffixSources[0].seqId == 2 &&
              suffixSources[1].seqId == 3,
          "B and A remain Ready beyond the ordered release fence");

    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return backendState->firstRecordPostCommitRan.load(
                std::memory_order_acquire);
          }),
          "ordered release submits the head before admitting its suffix");
    check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
              queue, headSource) == headSource.size(),
          "fenced head completion releases capacity for the younger suffix");
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 3;
          }),
          "fenced head and younger natural suffix both encode");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(backendState->plannerCalls == 1,
        "only the clean suffix plans after the ordered head fence is acknowledged");
  check(backendState->calls.size() == 3 &&
            backendState->calls[0].seqId == 1 &&
            backendState->calls[1].seqId == 2 &&
            backendState->calls[2].seqId == 3 &&
            !backendState->calls[0].fragment.has_value() &&
            !backendState->calls[1].fragment.has_value() &&
            !backendState->calls[2].fragment.has_value(),
        "ordered release excludes the head and an unmergeable clean B|A "
        "suffix fails open in natural order");
  check(backendState->firstRecordPostCommitRan.load(
            std::memory_order_acquire) &&
            backendState->backendCallsAtFirstRecordSubmit.load(
                std::memory_order_relaxed) == 1,
        "ordered release submits the head before encoding its suffix");

  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, suffixSources),
        "shutdown submits the younger post-fence suffix");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, suffixSources) == suffixSources.size(),
        "post-fence suffix completion remains FIFO-reclaimable");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(queue) == 3,
        "ordered release plus suffix completion reaches the FIFO tail");
}

void productionLoopBoundsNineReadySourcesToFirstPlanningWindow() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA610u;
  constexpr std::uint64_t kTargetB = 0xB610u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto headSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  check(waitUntil([&] {
          return backendState->firstCallEncoded.load(
              std::memory_order_acquire);
        }),
        "nine-source fixture encodes active head before releasing it");

  fixture.publishArenaTargetDraw(1u, kTargetB);
  fixture.publishArenaTargetDraw(2u, kTargetA);
  for (std::uint64_t i = 3; i <= 9; ++i) {
    fixture.publishArenaTargetDraw(i, 0xC610u + i);
  }
  const auto suffixSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(suffixSources.size() == 9,
        "all nine compatible successors accumulate before selection");
  for (const auto& source : suffixSources) {
    check(dxmt9::CommandQueueArenaLeaseTestAccess::readySourceIsArena(
              queue, source),
          "nine-source bounded window keeps every successor Arena-backed");
  }
  backendState->releaseFirstReturn.store(true, std::memory_order_release);
  check(waitUntil([&] {
          return backendState->observedCalls.load(
                     std::memory_order_acquire) == 10;
        }),
        "bounded first window and Ready suffix both encode");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  check(backendState->plannerCalls == 1,
        "nine Ready successors invoke one bounded eight-source plan");
  check(backendState->calls.size() == 10 &&
            backendState->calls[0].seqId == 1 &&
            backendState->calls[1].seqId == 3 &&
            backendState->calls[2].seqId == 2 &&
            backendState->calls.back().seqId == 10,
        "first bounded window reorders A before B and leaves source nine for "
        "the next natural selection");
  check(backendState->calls[1].fragment.has_value() &&
            backendState->calls[8].fragment.has_value() &&
            !backendState->calls[9].fragment.has_value(),
        "exactly eight successors use planned fragments; the ninth remains "
        "outside that proof window");

  std::array<dxmt9::core::metalqueue::QueueCompletionSource, 10>
      expectedSources{};
  expectedSources[0] = headSource.front();
  std::copy(suffixSources.begin(), suffixSources.end(),
            expectedSources.begin() + 1);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "bounded planning plus natural suffix submits every source once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size(),
        "ten-source carrier completion remains FIFO after bounded planning");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(queue) == 10,
        "bounded-window completion reaches the ninth successor");
}

void productionLoopCarriesActivePassAcrossBoundedWindowEdge() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  backendState->observeFirstRecordSubmit = true;
  dxmt9::framegraph::MultiSourceReplayPlan forcedPlan{};
  forcedPlan.disposition =
      dxmt9::framegraph::MultiSourceReplayDisposition::Planned;
  forcedPlan.validation =
      dxmt9::framegraph::MultiSourceReplayValidation::Valid;
  forcedPlan.diagnostics.outcome =
      dxmt9::framegraph::MultiSourcePlannerOutcome::Planned;
  for (std::uint32_t source = 0; source < 8u; ++source) {
    forcedPlan.commands.push_back({.retainedSourceIndex = source,
                                   .commandIndex = 0u});
  }
  backendState->forcedPlan = std::move(forcedPlan);
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA611u;
  constexpr std::uint64_t kTargetB = 0xB611u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto headSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(headSource.size() == 1u && headSource.front().seqId == 1u,
        "bounded-edge fixture publishes its active head alone");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffixSources;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "bounded-edge fixture opens the initial A pass first");
    for (std::uint64_t source = 1u; source <= 7u; ++source) {
      fixture.publishArenaTargetDraw(source, kTargetB);
    }
    fixture.publishArenaTargetDraw(8u, kTargetA);
    fixture.publishArenaTargetDraw(9u, kTargetA);
    suffixSources = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffixSources.size() == 9u,
          "bounded-edge fixture publishes one full window plus a successor");

    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return backendState->completedCalls.load(
                       std::memory_order_acquire) == 10u;
          }),
          "bounded window and its ninth successor both finish encoding");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(backendState->plannerCalls == 1u &&
            backendState->calls.size() == 10u,
        "only the bounded eight-source prefix enters planning");
  for (std::size_t i = 1u; i <= 8u; ++i) {
    check(backendState->calls[i].fragment.has_value(),
          "every source in the first bounded window uses its planned fragment");
  }
  check(!backendState->calls[9].fragment.has_value(),
        "the ninth successor remains outside the first planning window");

  const auto& windowTail = backendState->calls[8];
  const auto& ninthSource = backendState->calls[9];
  check(windowTail.seqId == 9u && ninthSource.seqId == 10u &&
            windowTail.session != 0u &&
            windowTail.session == ninthSource.session,
        "the ninth source appends to the session left active by window zero");
  check(windowTail.returnedCommandBuffer != NULL_OBJECT_HANDLE &&
            ninthSource.commandBuffer == windowTail.returnedCommandBuffer &&
            ninthSource.returnedCommandBuffer ==
                windowTail.returnedCommandBuffer,
        "the window tail passes its exact command-buffer carrier to source nine");
  check(windowTail.renderPassBeginsAfter == 3u &&
            windowTail.renderPassEndsAfter == 2u &&
            ninthSource.renderPassBeginsAfter ==
                windowTail.renderPassBeginsAfter &&
            ninthSource.renderPassEndsAfter ==
                windowTail.renderPassEndsAfter,
        "matching A draws cross the planning-window edge without another "
        "render-pass begin or end");
  check(backendState->renderPassBegins ==
                ninthSource.renderPassBeginsAfter &&
            backendState->renderPassEnds ==
                ninthSource.renderPassEndsAfter + 1u,
        "the explicit stop drain closes the carried render pass exactly once");
  check(backendState->firstRecordPostCommitRan.load(
            std::memory_order_acquire) &&
            backendState->backendCallsAtFirstRecordSubmit.load(
                std::memory_order_relaxed) == 10u,
        "the carried session submits only after all ten sources encode");

  std::array<dxmt9::core::metalqueue::QueueCompletionSource, 10u>
      expectedSources{};
  expectedSources[0] = headSource.front();
  std::copy(suffixSources.begin(), suffixSources.end(),
            expectedSources.begin() + 1);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "one final submission covers both sides of the bounded window edge");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size() &&
            dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(queue) ==
                10u,
        "window-edge carry preserves natural FIFO completion through the tail");
}

void productionLoopPlansPrefixBeforePresentBoundary() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->observeFirstRecordSubmit = true;
  backendState->holdFirstReturn = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));

  constexpr std::uint64_t kTargetA = 0xA710u;
  constexpr std::uint64_t kTargetB = 0xB710u;
  fixture.publishLegacyTargetDraw(kTargetA);
  const auto headSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffixSources;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "Present-prefix fixture encodes active head before release");

    fixture.publishArenaTargetDraw(1u, kTargetB);
    fixture.publishArenaTargetDraw(2u, kTargetA);
    check(dxmt9::CommandQueueArenaLeaseTestAccess::publishLegacyClearPresent(
              queue),
          "Present-prefix fixture publishes the hard boundary source");
    suffixSources = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffixSources.size() == 3 && suffixSources[0].seqId == 2 &&
              suffixSources[1].seqId == 3 && suffixSources[2].seqId == 4 &&
              suffixSources[2].hasPresent,
          "B, A, Present accumulate as one Ready batch");

    backendState->releaseFirstReturn.store(true, std::memory_order_release);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 4;
          }),
          "planned pre-Present prefix and natural Present both encode");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true, std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    throw;
  }

  check(backendState->plannerCalls == 1,
        "B and A invoke exactly one bounded planner window");
  check(backendState->calls.size() == 4 &&
            backendState->calls[0].seqId == 1 &&
            backendState->calls[1].seqId == 3 &&
            backendState->calls[2].seqId == 2 &&
            backendState->calls[3].seqId == 4,
        "Present remains outside the A-before-B replay permutation");
  check(backendState->calls[1].fragment.has_value() &&
            backendState->calls[2].fragment.has_value() &&
            !backendState->calls[3].fragment.has_value(),
        "only B and A use pre-registered planned fragments");
  check(backendState->calls[3].session ==
                backendState->calls[0].session &&
            backendState->calls[3].commandBuffer ==
                backendState->calls[2].returnedCommandBuffer &&
            backendState->calls[2].returnedCommandBufferChainLength == 2 &&
            backendState->calls[3].commandBuffer !=
                backendState->calls[0].commandBuffer &&
            !backendState->calls[3].deferSessionFinalization,
        "the natural Present tail uses the same session and post-split tail "
        "CB, then requests immediate finalization");
  check(backendState->renderPassBegins == 2 &&
            backendState->renderPassEnds == 2,
        "the Present tail closes the active planned pass without opening a "
        "third render pass");
  check(backendState->firstRecordPostCommitRan.load(
            std::memory_order_acquire) &&
            backendState->backendCallsAtFirstRecordSubmit.load(
                std::memory_order_relaxed) == 4,
        "the natural Present boundary closes and submits the carried planned "
        "prefix after its own non-planned encode");

  std::array expectedSources{headSource.front(), suffixSources[0],
                             suffixSources[1], suffixSources[2]};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "pre-Present and Present submissions cover every source once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size(),
        "split submissions still complete sources in natural FIFO order");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(queue) == 4,
        "Present completion advances the FIFO waterline after the prefix");
}

void productionLoopLeaseWaitResumesAfterGpuReclaim() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  check(dxmt9::CommandQueueArenaLeaseTestAccess::publishLegacyClearPresent(
            queue),
        "GPU-reclaim fixture publishes one standalone Clear+Present");
  const auto presentSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(presentSource.size() == 1 && presentSource.front().hasPresent,
        "GPU-reclaim fixture snapshots the standalone Present source");

  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });

  const bool presentSubmitted = waitUntil([&] {
    return backendState->firstRecordPostCommitRan.load(
        std::memory_order_acquire);
  });
  if (!presentSubmitted) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    check(false, "standalone Clear+Present must submit before Direct replay");
  }

  fixture.publishArenaDraw(2);
  const auto directSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(directSource.size() == 1 && !directSource.front().hasPresent,
        "GPU-reclaim fixture leaves exactly one Direct draw Ready");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readySourceIsArena(
            queue, directSource.front()),
        "GPU-reclaim successor must use the production Direct Arena route");
  const bool leaseWaitEntered = waitUntil([&] {
    return dxmt9::CommandQueueArenaLeaseTestAccess::capacityWaiterCount(
               queue) == 1;
  });
  if (!leaseWaitEntered) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    check(false,
          "submitted Present residency must reach the exact denied-lease "
          "capacity wait");
  }
  check(backendState->observedBackendCalls.load(std::memory_order_acquire) ==
            1,
        "Direct draw stays unrepresented while the startup lease is denied");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::stopped(queue),
        "GPU-reclaim denied-lease wait must not use the stop escape");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::writerPressureActive(queue),
        "GPU-reclaim denied-lease wait must not use writer pressure");

  const std::uint64_t generationBeforeReclaim =
      dxmt9::CommandQueueArenaLeaseTestAccess::capacityProgressGeneration(
          queue);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, presentSource) == 1,
        "GPU completion and finish reclaim the standalone Present source");
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::
                         capacityProgressGeneration(queue) !=
                     generationBeforeReclaim &&
              backendState->observedBackendCalls.load(
                  std::memory_order_acquire) == 2;
        }),
        "GPU reclaim advances capacity progress and starts the Direct draw");

  check(dxmt9::CommandQueueArenaLeaseTestAccess::postOrderedSubmit(
            queue, 2, directSource.front().seqId),
        "ordered Flush submits the Direct session after startup");
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
              queue, directSource);
        }),
        "Direct draw submits without shutdown or writer-pressure release");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, directSource) == 1,
        "GPU-reclaim Direct source completes and reclaims normally");
}

struct P0OffloadAdmissionContext {
  dxmt9::CommandQueue* queue = nullptr;
  const dxmt9::core::ArenaSourcePayloadLayout* layout = nullptr;
  std::atomic<bool> admitted{false};
};

std::atomic<P0OffloadAdmissionContext*> p0OffloadAdmissionContext{nullptr};

std::int32_t replayP0OffloadAdmission(
    D9CDevice*, dxmt9::d3d9::RawCommandChunk&) {
  auto* context = p0OffloadAdmissionContext.load(std::memory_order_acquire);
  if (!context || !context->queue || !context->layout) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const bool admitted =
      dxmt9::CommandQueueArenaLeaseTestAccess::waitForArenaAdmission(
          *context->queue, *context->layout);
  context->admitted.store(admitted, std::memory_order_release);
  dxmt9::perf::recordOffloadReplayStage(
      dxmt9::perf::OffloadReplayStage::Done);
  return D3D_OK;
}

void productionLoopPressureEscapesDeniedFirstLeaseOnce() {
  RuntimeFixture fixture(/*captureStreaming=*/true);
  auto& queue = fixture.routing->queue_;
  const auto frontierBefore =
      dxmt9::perf::snapshotSchedulingProgressFrontier();
  fixture.publishArenaClearPresentPages(1u, 512u);
  for (std::uint64_t rawOrdinal = 2; rawOrdinal <= 4; ++rawOrdinal) {
    fixture.publishArenaClearPages(rawOrdinal, 512u);
  }

  const auto sourcesBefore =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  const auto statsBefore =
      dxmt9::CommandQueueArenaLeaseTestAccess::tapeStats(queue);
  check(sourcesBefore.size() == 4u && sourcesBefore.front().hasPresent &&
            statsBefore.residentSources == 4u &&
            statsBefore.residentPages == 2048u,
        "pressure fixture fills the capture-streaming Tape with one older "
        "Present and a three-source Ready suffix");

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  const auto segment = makeSourcePayloadLayout(capacity, 4096, 64);
  check(segment.has_value() && segment->pageCount == 1u,
        "pressure fixture builds one exact ordinary Direct admission");
  const std::array segments{*segment};
  const auto layout = makeArenaSourcePayloadLayout(segments, 4096, 64);
  check(layout.has_value() && layout->pageCount == 1u,
        "pressure fixture owns a valid one-page admission request");

  dxmt9::CommandQueueArenaLeaseTestAccess::
      enableSchedulingWaitObservation(queue);
  dxmt9::CommandQueueArenaLeaseTestAccess::
      pauseAfterFirstLeaseRetry(queue);
  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  check(dxmt9::CommandQueueArenaLeaseTestAccess::
            waitForFirstLeaseWaitEntries(queue, 1u),
        "first lease denial parks before Arena admission pressure appears");

  fixture.cDevice->replayOffload =
      std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>(
          replayP0OffloadAdmission);
  P0OffloadAdmissionContext offloadContext{
      .queue = &queue,
      .layout = &*layout,
  };
  p0OffloadAdmissionContext.store(&offloadContext,
                                   std::memory_order_release);
  fixture.cDevice->replayOffload->queue().
      enableDrainWaitObservationForTest();
  fixture.cDevice->replayOffload->start(fixture.cDevice.get());
  const std::array offloadRecords{clearRecord()};
  auto offloadRaw = makeRaw(makeWireFixture(offloadRecords), 5u);
  check(fixture.cDevice->replayOffload->queue().push(
            std::move(offloadRaw)),
        "composition enqueues one real offload raw item");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::
            waitForArenaAdmissionWaitEntries(queue, 1u) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::
                    arenaAdmissionWaiterCount(queue) == 1u,
        "the real offload item parks in Arena admission under full-Tape "
        "pressure while encode remains in its first-lease wait");

  check(dxmt9::CommandQueueArenaLeaseTestAccess::
            waitForFirstLeaseWaitEntries(queue, 2u),
        "pressure escape performs one serial source then parks the next "
        "denied lease until capacity progress");
  check(backendState->calls.size() == 2u &&
            backendState->calls[0].seqId == sourcesBefore[0].seqId &&
            backendState->calls[1].seqId == sourcesBefore[1].seqId &&
            backendState->calls[1].session == 0u &&
            !backendState->calls[1].deferSessionFinalization,
        "production coordinator executes exactly the FIFO head as one "
        "bounded serial standalone source");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::
              hasPendingSessionRelease(queue) &&
            backendState->firstRecordPostCommitRan.load(
                std::memory_order_acquire) &&
            backendState->backendCallCountAtFirstRecordSubmit.load(
                std::memory_order_relaxed) == 1u,
        "admission pressure creates no SessionReleaseEvent and does not fold "
        "the escape into the older semantic submission");

  const auto readyAfterEscape =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(readyAfterEscape.size() == sourcesBefore.size() - 2u,
        "one older standalone and one pressure escape leave the exact suffix "
        "Ready");
  for (std::size_t i = 0; i < readyAfterEscape.size(); ++i) {
    check(sameCompletionSource(readyAfterEscape[i], sourcesBefore[i + 2u]),
          "pressure escape preserves every unselected FIFO source identity");
  }
  const std::array progressed{sourcesBefore[0], sourcesBefore[1]};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, progressed),
        "older unavailable residency and the exact escaped head both remain "
        "completion-owned");

  const auto frontierAtSecondWait =
      dxmt9::perf::snapshotSchedulingProgressFrontier();
  check(frontierAtSecondWait.cpuReadyFirstLeaseHeadSeq ==
                sourcesBefore[2].seqId &&
            frontierAtSecondWait.cpuReadyFirstLeaseHeadSourceOrdinal == 3u &&
            frontierAtSecondWait.cpuReadyFirstLeaseWaitCurrent == 1u &&
            frontierAtSecondWait.cpuReadyArenaAdmissionWaitCurrent == 1u &&
            frontierAtSecondWait.offloadReplayInflightRaw == 1u &&
            frontierAtSecondWait.offloadReplayStage ==
                static_cast<std::uint64_t>(
                    dxmt9::perf::OffloadReplayStage::ArenaAdmission),
        "frontier gauges expose the exact next head, raw replay, and Arena "
        "admission stage without waiting for another Present");

  std::atomic<bool> captureReturned{false};
  std::atomic<std::int32_t> captureResult{D3D_OK};
  std::mutex captureReturnMutex;
  std::condition_variable captureReturnCv;
  std::thread captureThread([&] {
    captureResult.store(
        dxmt9c_device_reserve_render_tape_present_capture(
            fixture.cDevice.get()),
        std::memory_order_release);
    {
      std::lock_guard lock(captureReturnMutex);
      captureReturned.store(true, std::memory_order_release);
    }
    captureReturnCv.notify_all();
  });
  check(fixture.cDevice->replayOffload->queue().
            waitForDrainWaitEntriesForTest(1u) &&
            !captureReturned.load(std::memory_order_acquire) &&
            dxmt9::perf::snapshotSchedulingProgressFrontier().
                    offloadDrainWaitCurrent == 1u,
        "capture reserve deterministically waits for the real in-flight raw "
        "item without polling");

  const std::uint64_t generationBeforeReclaim =
      dxmt9::CommandQueueArenaLeaseTestAccess::capacityProgressGeneration(
          queue);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, std::span<const dxmt9::core::metalqueue::
                                 QueueCompletionSource>(&progressed[0], 1u)) ==
                1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
                queue,
                std::span<const dxmt9::core::metalqueue::QueueCompletionSource>(
                    &progressed[1], 1u)) == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::
                    capacityProgressGeneration(queue) !=
                generationBeforeReclaim,
        "older and escaped completion advance the explicit capacity "
        "generation and free the real replay admission");
  const auto statsAfterProgress =
      dxmt9::CommandQueueArenaLeaseTestAccess::tapeStats(queue);
  check(statsAfterProgress.residentSources == 2u &&
            statsAfterProgress.residentPages == 1024u &&
            statsAfterProgress.readyFifoEntries == 2u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::probeArenaAdmission(
                queue, *layout) ==
                dxmt9::core::CpuReadyTape::ReserveProbe::Ready,
        "two exact 8x64-segment releases reach capture page low-water and "
        "make the production Arena predicate Ready");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::
            waitForFirstLeaseRetryPause(queue),
        "the production generation predicate retries before another lease");
  const bool replayDrained = fixture.cDevice->replayOffload->queue().
      waitForDrainedForTest();
  const bool replayAdmitted =
      offloadContext.admitted.load(std::memory_order_acquire);
  bool captureReturnedBeforeStop = false;
  {
    std::unique_lock lock(captureReturnMutex);
    captureReturnedBeforeStop = captureReturnCv.wait_for(
        lock, std::chrono::seconds(2), [&] {
          return captureReturned.load(std::memory_order_acquire);
        });
  }
  const auto captureResultBeforeStop =
      captureResult.load(std::memory_order_acquire);
  const bool eligibleProgressReturnedDrain =
      replayDrained && replayAdmitted && captureReturnedBeforeStop &&
      captureResultBeforeStop == dxmt9::core::D3DERR_NOTAVAILABLE;

  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  dxmt9::CommandQueueArenaLeaseTestAccess::
      resumeAfterFirstLeaseRetry(queue);
  captureThread.join();
  encodeThread.join();
  fixture.cDevice->replayOffload->stop();
  p0OffloadAdmissionContext.store(nullptr, std::memory_order_release);
  const auto frontierAfter =
      dxmt9::perf::snapshotSchedulingProgressFrontier();
  const auto waitEnters = frontierAfter.cpuReadyFirstLeaseWaitEnter -
      frontierBefore.cpuReadyFirstLeaseWaitEnter;
  const auto waitExits =
      frontierAfter.cpuReadyFirstLeaseActionRetryGeneration -
          frontierBefore.cpuReadyFirstLeaseActionRetryGeneration +
      frontierAfter.cpuReadyFirstLeaseActionPressureSerial -
          frontierBefore.cpuReadyFirstLeaseActionPressureSerial +
      frontierAfter.cpuReadyFirstLeaseActionStop -
          frontierBefore.cpuReadyFirstLeaseActionStop;
  const auto admissionEnters =
      frontierAfter.cpuReadyArenaAdmissionWaitEnter -
      frontierBefore.cpuReadyArenaAdmissionWaitEnter;
  const auto admissionExits =
      frontierAfter.cpuReadyArenaAdmissionExitRetry -
          frontierBefore.cpuReadyArenaAdmissionExitRetry +
      frontierAfter.cpuReadyArenaAdmissionExitStop -
          frontierBefore.cpuReadyArenaAdmissionExitStop;
  const auto admissionRetryExits =
      frontierAfter.cpuReadyArenaAdmissionExitRetry -
          frontierBefore.cpuReadyArenaAdmissionExitRetry;
  const auto admissionStopExits =
      frontierAfter.cpuReadyArenaAdmissionExitStop -
          frontierBefore.cpuReadyArenaAdmissionExitStop;
  check(eligibleProgressReturnedDrain && admissionRetryExits == 1u &&
            admissionStopExits == 0u,
        "eligible standalone progress must admit replay and return capture "
        "before teardown (drained=" + std::to_string(replayDrained) +
        ", admitted=" + std::to_string(replayAdmitted) +
        ", capture_returned=" +
        std::to_string(captureReturnedBeforeStop) +
        ", capture_result=" + std::to_string(captureResultBeforeStop) +
        ", admission_retry_exits=" +
        std::to_string(admissionRetryExits) +
        ", admission_stop_exits=" + std::to_string(admissionStopExits) +
        ")");
  check(waitEnters == waitExits && waitEnters == 2u &&
            admissionEnters == admissionExits && admissionEnters == 1u &&
            admissionRetryExits == 1u && admissionStopExits == 0u &&
            frontierAfter.cpuReadyFirstLeaseActionPressureSerial -
                    frontierBefore.cpuReadyFirstLeaseActionPressureSerial ==
                1u &&
            frontierAfter.cpuReadyFirstLeaseActionRetryGeneration -
                    frontierBefore.cpuReadyFirstLeaseActionRetryGeneration ==
                1u &&
            frontierAfter.cpuReadyFirstLeaseActionStop -
                    frontierBefore.cpuReadyFirstLeaseActionStop ==
                0u &&
            frontierAfter.cpuReadyFirstLeaseCreditRearmed -
                    frontierBefore.cpuReadyFirstLeaseCreditRearmed ==
                1u &&
            frontierAfter.cpuReadyFirstLeaseWaitCurrent == 0u &&
            frontierAfter.cpuReadyArenaAdmissionWaitCurrent == 0u &&
            frontierAfter.offloadDrainWaitCurrent == 0u &&
            frontierAfter.offloadPushWaitCurrent == 0u &&
            frontierAfter.offloadReplayInflightRaw == 0u &&
            frontierAfter.offloadReplayStage ==
                static_cast<std::uint64_t>(
                    dxmt9::perf::OffloadReplayStage::Done),
        "production frontier counters conserve every wait/action pair, one "
        "credit, retry, drain, raw, and replay-stage transition");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(queue) ==
                progressed.back().seqId &&
            dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
                queue, readyAfterEscape) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(queue) ==
                0u,
        "completion reclaims the progressed prefix while the terminal drain "
        "submits and post-encode retires every untouched suffix identity");
}

void productionLoopLeaseWaitResumesAfterInlineReclaim() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  check(dxmt9::CommandQueueArenaLeaseTestAccess::publishLegacyClearPresent(
            queue),
        "inline-reclaim fixture publishes one standalone Clear+Present");
  const auto presentSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::representReadyHead(queue);
  check(presentSource.has_value() && presentSource->hasPresent,
        "inline-reclaim fixture represents the standalone Present head");
  fixture.publishArenaDraw(2);
  const auto directSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(directSource.size() == 1 && !directSource.front().hasPresent,
        "inline-reclaim fixture leaves exactly one Direct draw Ready");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readySourceIsArena(
            queue, directSource.front()),
        "inline-reclaim successor must use the production Direct Arena route");

  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  const bool leaseWaitEntered = waitUntil([&] {
    return dxmt9::CommandQueueArenaLeaseTestAccess::capacityWaiterCount(
               queue) == 1;
  });
  if (!leaseWaitEntered) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    check(false,
          "represented Present residency must reach the exact denied-lease "
          "capacity wait");
  }
  check(backendState->observedBackendCalls.load(std::memory_order_acquire) ==
            0,
        "Direct draw stays unrepresented before inline capacity reclaim");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::stopped(queue),
        "inline-reclaim denied-lease wait must not use the stop escape");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::writerPressureActive(queue),
        "inline-reclaim denied-lease wait must not use writer pressure");

  const std::uint64_t generationBeforeReclaim =
      dxmt9::CommandQueueArenaLeaseTestAccess::capacityProgressGeneration(
          queue);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeInline(
            queue, *presentSource),
        "production inline completion reclaims the standalone Present head");
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::
                         capacityProgressGeneration(queue) !=
                     generationBeforeReclaim &&
              backendState->observedBackendCalls.load(
                  std::memory_order_acquire) == 1;
        }),
        "inline reclaim advances capacity progress and starts the Direct draw");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::finishOne(queue),
        "finish consumes the already inline-reclaimed Present sequence");

  check(dxmt9::CommandQueueArenaLeaseTestAccess::postOrderedSubmit(
            queue, 2, directSource.front().seqId),
        "ordered Flush submits the Direct session after inline startup");
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
              queue, directSource);
        }),
        "inline schedule submits Direct draw without shutdown or writer "
        "pressure");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, directSource) == 1,
        "inline-reclaim Direct source completes and reclaims normally");
}

void productionLoopCreditsExactReadyAndWritingSuccessor() {
  RuntimeFixture fixture;
  for (std::uint64_t rawOrdinal = 1; rawOrdinal <= 7; ++rawOrdinal) {
    fixture.publishArenaClearPages(rawOrdinal, 64);
  }
  fixture.publishArenaClearPages(8, 40);
  for (std::uint64_t rawOrdinal = 9; rawOrdinal <= 31; ++rawOrdinal) {
    fixture.publishArenaClearPages(rawOrdinal, 1);
  }

  auto& queue = fixture.routing->queue_;
  const auto readyBeforeWriter =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  const auto statsBeforeWriter =
      dxmt9::CommandQueueArenaLeaseTestAccess::tapeStats(queue);
  check(readyBeforeWriter.size() == 31u &&
            statsBeforeWriter.residentSources == 31u &&
            statsBeforeWriter.residentPages == 511u,
        "production fixture constructs the failed 31 Ready / 511-page "
        "prefix exactly");

  queue.submitClear(ClearDesc{});
  const auto writingSnapshot =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          leaseAcquisitionCapacitySnapshot(queue);
  const auto statsWithWriter =
      dxmt9::CommandQueueArenaLeaseTestAccess::tapeStats(queue);
  check(writingSnapshot.valid &&
            writingSnapshot.olderUnavailable ==
                dxmt9::core::CpuReadyTape::LeaseCapacityClaim{} &&
            writingSnapshot.orderedTailWritingSuccessor.has_value() &&
            writingSnapshot.orderedTailWritingSuccessor->claim.pages == 1u &&
            writingSnapshot.orderedTailWritingSuccessor->claim.readyEntries ==
                1u &&
            statsWithWriter.residentSources == 32u &&
            statsWithWriter.residentPages == 512u &&
            statsWithWriter.readyFifoEntries == 31u &&
            statsWithWriter.readyPublicationReservations == 32u,
        "production fixture exposes exactly 31 Ready plus one eligible "
        "ordered-tail Writing successor");

  std::atomic<bool> writerReturned{false};
  std::atomic<bool> writerPublished{false};
  std::thread writerThread([&] {
    writerPublished.store(
        dxmt9::CommandQueueArenaLeaseTestAccess::
            publishLegacyWritingSlot(queue),
        std::memory_order_release);
    writerReturned.store(true, std::memory_order_release);
  });
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::
              writerPressureActive(queue);
        }) && !writerReturned.load(std::memory_order_acquire),
        "compatibility writer blocks in commitCurrentChunk at the physical "
        "31-source inflight limit");

  auto backendState = std::make_shared<ProductionLoopBackendState>();
  auto backend = std::make_unique<ProductionLoopBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(backend));
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  struct ThreadGuard {
    dxmt9::CommandQueue& queue;
    std::thread& writer;
    std::thread& encode;
    ~ThreadGuard() {
      if (encode.joinable() || writer.joinable()) {
        dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
      }
      if (writer.joinable()) {
        writer.join();
      }
      if (encode.joinable()) {
        encode.join();
      }
    }
  } guard{queue, writerThread, encodeThread};

  check(waitUntil([&] {
          return backendState->observedBackendCalls.load(
                     std::memory_order_acquire) >= 1u &&
              writerReturned.load(std::memory_order_acquire);
        }) && writerPublished.load(std::memory_order_acquire),
        "first lease acquisition and encode retire the credited head so the "
        "blocked writer publishes");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::stopped(queue) &&
            !backendState->firstRecordPostCommitRan.load(
                std::memory_order_acquire),
        "Ready/Writing progress uses neither stop nor a pressure-created "
        "session submission");
  check(waitUntil([&] {
          return backendState->observedBackendCalls.load(
                     std::memory_order_acquire) == 32u;
        }),
        "all 31 Ready sources and the published Writing successor encode");

  check(!backendState->calls.empty(),
        "Ready/Writing regression records production encode calls");
  const auto session = backendState->calls.front().session;
  check(session != 0 &&
            std::all_of(
                backendState->calls.begin(), backendState->calls.end(),
                [session](const ProductionLoopBackendCall& call) {
                  return call.session == session &&
                      call.deferSessionFinalization;
                }),
        "credited successor remains on one deferred EncodeSession without "
        "an artificial command-buffer or pass boundary");

  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> allSources;
  allSources.reserve(backendState->calls.size());
  for (const auto& call : backendState->calls) {
    check(call.sessionSource.has_value(),
          "credited regression retains every completion source");
    allSources.push_back(*call.sessionSource);
  }
  check(dxmt9::CommandQueueArenaLeaseTestAccess::postOrderedSubmit(
            queue, 31, allSources.back().seqId),
        "explicit ordered fence submits the progressed session");
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
              queue, allSources);
        }) &&
            backendState->backendCallCountAtFirstRecordSubmit.load(
                std::memory_order_relaxed) == 32u,
        "only the explicit fence submits the complete 32-source session");

  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  writerThread.join();
  encodeThread.join();
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, allSources) == allSources.size(),
        "credited session completes every source in FIFO order");
}

void productionLoopReleasesAtDeterministicCapBeforeWriterPressure() {
  RuntimeFixture fixture;
  for (std::uint64_t rawOrdinal = 1;
       rawOrdinal < dxmt9::kMaxQueuedChunks; ++rawOrdinal) {
    fixture.publishArenaClear(rawOrdinal);
  }

  auto& queue = fixture.routing->queue_;
  const auto predecessorSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(predecessorSources.size() == dxmt9::kMaxQueuedChunks - 1u,
        "fixed-cap fixture leaves one compatibility publication slot");

  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });

  const bool pendingParked = waitUntil([&] {
    return backendState->observedBackendCalls.load(std::memory_order_acquire) ==
           predecessorSources.size();
  });
  if (!pendingParked) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    check(false,
          "production loop must encode the fixed predecessor prefix before "
          "parking the Ready suffix behind submitted residency");
  }

  constexpr std::size_t kSuccessorCount = 10u;
  for (std::size_t i = 0; i < kSuccessorCount; ++i) {
    queue.submitClear(ClearDesc{});
    check(dxmt9::CommandQueueArenaLeaseTestAccess::publishLegacyWritingSlot(
              queue),
          "post-encode retirement must reopen compatibility publication");
    check(waitUntil([&] {
            return backendState->observedBackendCalls.load(
                       std::memory_order_acquire) ==
                predecessorSources.size() + i + 1u;
          }),
          "each successor must join the still-open encoded session");
  }
  const std::size_t expectedCount =
      predecessorSources.size() + kSuccessorCount;
  check(expectedCount > 30u && backendState->calls.size() == expectedCount,
        "one live session must encode more than the former 30-source cap");
  check(!backendState->firstRecordPostCommitRan.load(
            std::memory_order_acquire),
        "physical residency release must not submit the open session");
  const auto session = backendState->calls.front().session;
  check(std::all_of(
            backendState->calls.begin(), backendState->calls.end(),
            [session](const ProductionLoopBackendCall& call) {
              return call.session == session;
            }),
        "all post-retirement successors stay on one EncodeSession");

  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> allSources;
  allSources.reserve(backendState->calls.size());
  for (const auto& call : backendState->calls) {
    check(call.sessionSource.has_value(),
          "every encoded source retains completion attribution");
    allSources.push_back(*call.sessionSource);
  }
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, allSources),
        "shutdown submits the locator-free completion ledger");
  const std::size_t finishedSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
          queue, allSources);
  check(backendState->backendCallCountAtFirstRecordSubmit.load(
            std::memory_order_relaxed) == expectedCount,
        "the former residency cap no longer creates a submission boundary");
  check(finishedSources == expectedCount,
        "the expanded session completes every receipt in FIFO order");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(queue) == 0,
        "post-encode retirement leaves no payload residency");
}

void productionLoopAttributesSessionCapCloseToSameKeyReopen(
    bool expectAttribution) {
  RuntimeFixture fixture;
  constexpr std::uint64_t kTargetA = 0xCA900u;
  for (std::uint64_t rawOrdinal = 1;
       rawOrdinal < dxmt9::kMaxQueuedChunks; ++rawOrdinal) {
    fixture.publishArenaTargetDraw(rawOrdinal, kTargetA);
  }

  auto& queue = fixture.routing->queue_;
  const auto predecessorSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(predecessorSources.size() == dxmt9::kMaxQueuedChunks - 1u,
        "close-attribution fixture fills the 30-source session prefix");

  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->observeFirstRecordSubmit = true;
  backendState->disableMidChunkCommits = true;
  auto backend = std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, backend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(backend));
  const auto before =
      dxmt9::perf::test::snapshotRenderPassCloseAttribution();

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  struct EncodeThreadGuard {
    dxmt9::CommandQueue& queue;
    std::thread& thread;
    ~EncodeThreadGuard() {
      if (thread.joinable()) {
        dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
        thread.join();
      }
    }
  } encodeThreadGuard{queue, encodeThread};
  if (!waitUntil([&] {
        return backendState->observedCalls.load(std::memory_order_acquire) ==
            predecessorSources.size();
      })) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    check(false, "close-attribution prefix must park as one open pass");
  }

  constexpr std::size_t kWorkCap =
      dxmt9::core::metalqueue::kMaxEncodeSessionSources;
  for (std::size_t sourceCount = predecessorSources.size();
       sourceCount < kWorkCap; ++sourceCount) {
    try {
      fixture.publishLegacyTargetDraw(kTargetA);
    } catch (...) {
      dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
      encodeThread.join();
      check(false,
            "post-encode credit must publish every source through work cap");
    }
    if (!waitUntil([&] {
          return backendState->observedCalls.load(
                     std::memory_order_acquire) == sourceCount + 1u;
        })) {
      dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
      encodeThread.join();
      check(false, "eligible source must join through the fixed work cap");
    }
    check(!backendState->firstRecordPostCommitRan.load(
              std::memory_order_acquire),
          "residency reuse must not close below the encoded-work cap");
  }

  try {
    fixture.publishLegacyTargetDraw(kTargetA);
  } catch (...) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    check(false, "work-cap successor must publish after receipt retirement");
  }
  if (!waitUntil([&] {
        return backendState->observedCalls.load(std::memory_order_acquire) ==
                   kWorkCap + 1u &&
            backendState->firstRecordPostCommitRan.load(
                std::memory_order_acquire);
      })) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    check(false, "the 129th source must close the deterministic work cap");
  }

  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> prefix;
  prefix.reserve(kWorkCap);
  for (std::size_t i = 0; i < kWorkCap; ++i) {
    check(backendState->calls[i].sessionSource.has_value(),
          "work-cap predecessor keeps completion attribution");
    prefix.push_back(*backendState->calls[i].sessionSource);
  }
  check(backendState->calls.back().sessionSource.has_value(),
        "work-cap successor keeps completion attribution");
  const std::array suffix{*backendState->calls.back().sessionSource};
  const auto predecessorSession = backendState->calls.front().session;
  check(std::all_of(
            backendState->calls.begin(),
            backendState->calls.begin() + kWorkCap,
            [predecessorSession](const ProductionLoopBackendCall& call) {
              return call.session == predecessorSession;
            }) &&
            backendState->calls.front().createdCommandBuffer &&
            backendState->calls.back().createdCommandBuffer,
        "exactly 128 sources share the predecessor session before reopen");
  check(backendState->backendCallsAtFirstRecordSubmit.load(
            std::memory_order_relaxed) == kWorkCap,
        "work-cap submission fences exactly the bounded predecessor");

  const auto afterReopen =
      dxmt9::perf::test::snapshotRenderPassCloseAttribution();
  check(expectAttribution
            ? afterReopen.finalSessionCap == before.finalSessionCap + 1u &&
                  afterReopen.adjacentSessionCap ==
                      before.adjacentSessionCap + 1u &&
                  afterReopen.finalRecorded == before.finalRecorded + 1u
            : afterReopen.finalSessionCap == before.finalSessionCap &&
                  afterReopen.adjacentSessionCap ==
                      before.adjacentSessionCap &&
                  afterReopen.recorded == before.recorded &&
                  afterReopen.finalRecorded == before.finalRecorded,
        expectAttribution
            ? "the work-cap close token owns the exact same-key reopen"
            : "perf-off work-cap finalization performs no attribution work");
  check(backendState->renderPassBegins == 2u,
        "the deterministic cap creates exactly one same-key reopen");

  const std::size_t finishedPrefix =
      dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(queue, prefix);
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, suffix),
        "shutdown submits the one-source work-cap successor");
  const std::size_t finishedSuffix =
      dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(queue, suffix);
  check(finishedPrefix == kWorkCap && finishedSuffix == 1u &&
            backendState->renderPassEnds == 2u,
        "both work-cap pass instances complete once in FIFO order");
}

void orderedClosePassKeepsFencedSuffixReadyAndPreservesSession() {
  RuntimeFixture fixture;
  fixture.publishArenaClear(1);
  fixture.publishArenaClear(2);
  auto& queue = fixture.routing->queue_;
  dxmt9::CommandQueueArenaLeaseTestAccess::
      enableCpuReadySessionReleaseLane(queue);

  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));
  dxmt9::CommandQueueArenaLeaseTestAccess::
      pauseAfterNextSessionReleaseAck(queue);

  std::atomic<bool> releaseResult{false};
  std::thread releaseThread([&] {
    releaseResult.store(
        queue.releaseCpuReadySessionBeforeOrderedControl(
            dxmt9::core::metalqueue::SessionReleaseReason::DirectObservation,
            dxmt9::core::metalqueue::SessionReleaseAction::ClosePass, 2),
        std::memory_order_release);
  });
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::
              hasPendingSessionRelease(queue);
        }),
        "Query-style ClosePass must publish its ordered fence before wait");

  // This publication is deliberately younger than the already-fixed event.
  // It must remain Ready while the older prefix closes its pass.
  fixture.publishArenaClear(3);
  const auto expectedSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(expectedSources.size() == 3,
        "ClosePass fixture publishes two fenced sources and one suffix");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::
              pausedAfterSessionReleaseAck(queue);
        }),
        "encode coordinator must acknowledge ClosePass after its action");
  releaseThread.join();

  check(releaseResult.load(std::memory_order_acquire),
        "ordered ClosePass poster returns only after acknowledgement");
  check(backendState->calls.size() == 2,
        "release fence represents only the older compatibility prefix");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(queue) == 1,
        "source younger than the release fence stays Ready");
  check(!backendState->firstRecordPostCommitRan.load(
            std::memory_order_acquire),
        "ClosePass must not submit the pending non-present session");
  check(backendState->calls[0].session != 0 &&
            backendState->calls[0].session == backendState->calls[1].session,
        "ClosePass keeps one queue-owned EncodeSession for the fenced prefix");

  dxmt9::CommandQueueArenaLeaseTestAccess::
      resumeAfterSessionReleaseAck(queue);
  check(waitUntil([&] {
          return backendState->observedBackendCalls.load(
                     std::memory_order_acquire) == 3;
        }),
        "resumed coordinator appends the younger suffix");
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  check(backendState->calls[2].session == backendState->calls[0].session,
        "Query-style ClosePass preserves the session across the suffix");
  check(backendState->firstRecordPostCommitRan.load(
            std::memory_order_acquire) &&
            backendState->backendCallCountAtFirstRecordSubmit.load(
                std::memory_order_relaxed) == 3,
        "only the later shutdown drain submits the preserved session");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size(),
        "ClosePass fixture completes every merged source after final submit");
}

void orderedClosePassEnablesYoungerMovedHeadOnSameSession() {
  RuntimeFixture fixture;
  auto& queue = fixture.routing->queue_;
  dxmt9::CommandQueueArenaLeaseTestAccess::
      enableCpuReadySessionReleaseLane(queue);
  auto backendState = std::make_shared<PlannedProductionBackendState>();
  backendState->holdFirstReturn = true;
  backendState->disableMidChunkCommits = true;
  auto plannedBackend =
      std::make_unique<PlannedProductionBackend>(backendState);
  dxmt9::CommandQueueArenaLeaseTestAccess::installDrawRecorder(
      queue, plannedBackend->drawRecorder());
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::move(plannedBackend));
  dxmt9::CommandQueueArenaLeaseTestAccess::
      pauseAfterNextSessionReleaseAck(queue);

  constexpr std::uint64_t kTargetA = 0xA540u;
  constexpr std::uint64_t kTargetB = 0xB540u;
  fixture.publishArenaTargetDraw(1u, kTargetA);
  const auto headSource =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(headSource.size() == 1 && headSource.front().seqId == 1,
        "closed-frontier queue fixture publishes head A alone");

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  std::atomic<bool> releaseResult{false};
  std::thread releaseThread;
  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> suffixSources;
  try {
    check(waitUntil([&] {
            return backendState->firstCallEncoded.load(
                std::memory_order_acquire);
          }),
          "head A opens its real production session pass");

    releaseThread = std::thread([&] {
      releaseResult.store(
          queue.releaseCpuReadySessionBeforeOrderedControl(
              dxmt9::core::metalqueue::SessionReleaseReason::
                  DirectObservation,
              dxmt9::core::metalqueue::SessionReleaseAction::ClosePass, 1u),
          std::memory_order_release);
    });
    check(waitUntil([&] {
            return dxmt9::CommandQueueArenaLeaseTestAccess::
                hasPendingSessionRelease(queue);
          }),
          "ordered ClosePass publishes the head fence");
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    check(waitUntil([&] {
            return dxmt9::CommandQueueArenaLeaseTestAccess::
                pausedAfterSessionReleaseAck(queue);
          }),
          "coordinator closes head A before admitting a younger source");
    releaseThread.join();
    check(releaseResult.load(std::memory_order_acquire),
          "ordered ClosePass acknowledges its exact head fence");
    check(backendState->observedCalls.load(std::memory_order_acquire) == 1,
          "ClosePass itself does not create a backend replay call");

    fixture.publishArenaMovedHeadReturn(2u, kTargetA, kTargetB);
    suffixSources = dxmt9::CommandQueueArenaLeaseTestAccess::
        snapshotReadyCompletionSources(queue);
    check(suffixSources.size() == 1 && suffixSources.front().seqId == 2,
          "younger source-local A-B-A stays Ready behind the close fence");
    check(backendState->observedCalls.load(std::memory_order_acquire) == 1,
          "paused acknowledgement prevents early younger replay");

    dxmt9::CommandQueueArenaLeaseTestAccess::
        resumeAfterSessionReleaseAck(queue);
    check(waitUntil([&] {
            return backendState->observedCalls.load(
                       std::memory_order_acquire) == 2;
          }),
          "younger moved-head source appends after ordered close");
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
  } catch (...) {
    backendState->releaseFirstReturn.store(true,
                                           std::memory_order_release);
    dxmt9::CommandQueueArenaLeaseTestAccess::
        resumeAfterSessionReleaseAck(queue);
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    if (releaseThread.joinable()) {
      releaseThread.join();
    }
    encodeThread.join();
    throw;
  }

  check(backendState->calls.size() == 2 &&
            backendState->calls[0].seqId == 1 &&
            backendState->calls[1].seqId == 2,
        "queue path encodes the natural source sequence exactly once");
  check(backendState->calls[0].session != 0 &&
            backendState->calls[0].session == backendState->calls[1].session,
        "ordered ClosePass retains one EncodeSession");
  check(backendState->calls[0].commandBuffer != NULL_OBJECT_HANDLE &&
            backendState->calls[0].commandBuffer ==
                backendState->calls[1].commandBuffer,
        "closed frontier retains the same command-buffer carrier");
  check(backendState->encodedSeqIds ==
            std::vector<std::uint64_t>({1u, 2u, 2u, 2u}) &&
            backendState->encodedCommandIndices ==
                std::vector<std::size_t>({0u, 1u, 0u, 2u}),
        "queue path replays prior A then younger B,A1,A2");
  check(backendState->renderPassBegins == 3 &&
            backendState->renderPassEnds == 3,
        "queue path closes prior A and finalizes younger B,A without extras");

  std::array expectedSources{headSource.front(), suffixSources.front()};
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "one final session submission covers head and younger source");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size(),
        "moved replay completion expands in natural FIFO order");
}

void orderedSubmitAcknowledgesAfterNonPresentPrefixSubmission() {
  RuntimeFixture fixture;
  fixture.publishArenaClear(1);
  fixture.publishArenaClear(2);
  auto& queue = fixture.routing->queue_;
  dxmt9::CommandQueueArenaLeaseTestAccess::
      enableCpuReadySessionReleaseLane(queue);
  const auto expectedSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(expectedSources.size() == 2 && !expectedSources[0].hasPresent &&
            !expectedSources[1].hasPresent,
        "SubmitSession fixture is a non-present prefix");

  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));
  std::atomic<bool> releaseResult{false};
  std::thread releaseThread([&] {
    releaseResult.store(
        queue.releaseCpuReadySessionBeforeOrderedControl(
            dxmt9::core::metalqueue::SessionReleaseReason::
                IndependentSubmission,
            dxmt9::core::metalqueue::SessionReleaseAction::SubmitSession, 2),
        std::memory_order_release);
  });
  check(waitUntil([&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::
              hasPendingSessionRelease(queue);
        }),
        "SubmitSession event must be visible before coordinator start");
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  releaseThread.join();

  check(releaseResult.load(std::memory_order_acquire),
        "SubmitSession poster observes coordinator acknowledgement");
  check(backendState->firstRecordPostCommitRan.load(
            std::memory_order_acquire),
        "acknowledgement follows physical non-present prefix submission");
  check(backendState->backendCallCountAtFirstRecordSubmit.load(
            std::memory_order_relaxed) == 2,
        "ordered submit covers the complete fixed prefix");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::
            acknowledgedSessionReleaseOrdinal(queue) != 0,
        "queue-owned release state records the acknowledged action");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "SubmitSession transitions every fenced source to GPU");

  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size(),
        "submitted prefix completes and reclaims normally");
}

void tentativeCoordinatorPreflightRestoresExactFifoOrder() {
  RuntimeFixture fixture;
  fixture.publishArenaClear(1);
  fixture.publishArenaClear(2);
  fixture.publishArenaClear(3);
  auto& queue = fixture.routing->queue_;
  const auto before =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));
  dxmt9::CommandQueueArenaLeaseTestAccess::
      restoreNextTentativePreflightAndReturn(queue);

  dxmt9::CommandQueueArenaLeaseTestAccess::
      runCpuReadySessionEncodeLoop(queue);
  const auto after =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(after.size() == before.size() && after.size() == 3,
        "tentative preflight restore returns the complete prefix to Ready");
  for (std::size_t i = 0; i < before.size(); ++i) {
    check(sameCompletionSource(before[i], after[i]),
          "tentative restore preserves exact FIFO identity and order");
  }
  check(backendState->calls.empty(),
        "restored preflight emits no backend or Metal effects");

  dxmt9::CommandQueueArenaLeaseTestAccess::
      stopAndRunCpuReadySessionEncodeLoop(queue);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, before) == before.size(),
      "restored sources remain consumable through the normal session path");
}

struct OrderedControlReplayEvent {
  std::uint32_t phase = 0;
  std::uint32_t recordIndex = 0;
  std::uint32_t recordType = 0;
  std::int32_t result = 0;
  std::uint32_t drawCalls = 0;
  std::uint64_t queryIssuedSequence = 0;
  std::size_t backendCalls = 0;
};

struct OrderedControlReplayTrace {
  SessionJoinDevice* routing = nullptr;
  dxmt9::core::Query* query = nullptr;
  ProductionLoopBackendState* backend = nullptr;
  std::vector<OrderedControlReplayEvent> events;
};

void recordOrderedControlReplayEvent(void* userdata, std::uint32_t phase,
                                     std::uint32_t recordIndex,
                                     std::uint32_t recordType,
                                     std::int32_t result) {
  auto& trace = *static_cast<OrderedControlReplayTrace*>(userdata);
  trace.events.push_back(OrderedControlReplayEvent{
      .phase = phase,
      .recordIndex = recordIndex,
      .recordType = recordType,
      .result = result,
      .drawCalls = trace.routing->drawCalls.load(std::memory_order_relaxed),
      .queryIssuedSequence = trace.query->issuedSequenceId(),
      .backendCalls = trace.backend
          ? trace.backend->observedBackendCalls.load(std::memory_order_acquire)
          : 0,
  });
}

void productionReplayFencesQueryBetweenOlderAndYoungerDraws() {
  RuntimeFixture fixture;
  auto buffer = fixture.device->CreateBuffer(BufferDesc{
      .size = 256u,
      .pool = Pool::Default,
      .usage = UsageVertexBuffer,
  });
  auto query = fixture.device->CreateQuery(QueryType::Occlusion);
  check(buffer != nullptr && query != nullptr,
        "ordered-control production resources must construct");
  D9CBuffer wireBuffer(buffer, fixture.cDevice.get());
  D9CQuery wireQuery(query, fixture.cDevice.get());
  const std::array records{
      drawRecord(wireBuffer.wireIdentity, 0u),
      queryIssueRecord(wireQuery.wireIdentity, 1u),
      drawRecord(wireBuffer.wireIdentity, 2u),
  };
  auto raw = makeRaw(makeWireFixture(records), 77,
                     fixture.cDevice->wireObjects);

  auto& queue = fixture.routing->queue_;
  dxmt9::CommandQueueArenaLeaseTestAccess::
      enableCpuReadySessionReleaseLane(queue);
  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));
  dxmt9::CommandQueueArenaLeaseTestAccess::
      pauseAfterNextSessionReleaseAck(queue);
  OrderedControlReplayTrace trace{
      .routing = fixture.routing,
      .query = query.get(),
      .backend = backendState.get(),
  };
  dxmt9_test_set_ordered_control_replay_observer(
      &trace, recordOrderedControlReplayEvent);

  std::atomic<std::int32_t> replayResult{D3DERR_INVALIDCALL};
  std::thread replayThread([&] {
    replayResult.store(
        dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw),
        std::memory_order_release);
  });
  const bool releasePublished = waitUntil([&] {
    return dxmt9::CommandQueueArenaLeaseTestAccess::
        hasPendingSessionRelease(queue);
  });
  const auto olderSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  replayThread.join();
  dxmt9_test_set_ordered_control_replay_observer(nullptr, nullptr);

  const bool coordinatorPaused = waitUntil([&] {
    return dxmt9::CommandQueueArenaLeaseTestAccess::
        pausedAfterSessionReleaseAck(queue);
  });
  const bool youngerPublished =
      dxmt9::CommandQueueArenaLeaseTestAccess::publishLegacyWritingSlot(queue);
  const auto youngerSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  dxmt9::CommandQueueArenaLeaseTestAccess::
      resumeAfterSessionReleaseAck(queue);
  const bool youngerEncoded = waitUntil([&] {
    return backendState->observedBackendCalls.load(
               std::memory_order_acquire) == 2;
  });
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> allSources;
  allSources.insert(allSources.end(), olderSources.begin(), olderSources.end());
  allSources.insert(allSources.end(), youngerSources.begin(),
                    youngerSources.end());

  check(releasePublished && replayResult.load(std::memory_order_acquire) ==
                                D3D_OK,
        "mixed draw/query/draw replay must publish and satisfy its ordered "
        "release");
  check(olderSources.size() == 1 && youngerPublished &&
            coordinatorPaused && youngerSources.size() == 1,
        "Query fence must split exactly one older and one younger source");
  check(youngerEncoded && backendState->calls.size() == 2,
        "production coordinator must encode both sides exactly once");
  check(trace.events.size() == 4,
        "one Query record must emit one complete release/dispatch trace");
  for (std::size_t i = 0; i < trace.events.size(); ++i) {
    const auto& event = trace.events[i];
    check(event.phase == i + 1u && event.recordIndex == 1u &&
              event.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE,
          "ordered replay trace must identify exact Query record and phase");
  }
  check(trace.events[0].drawCalls == 1u &&
            trace.events[0].queryIssuedSequence == 0u &&
            trace.events[0].backendCalls == 0u,
        "older draw must replay before release while Query remains untouched");
  check(trace.events[1].drawCalls == 1u &&
            trace.events[1].queryIssuedSequence == 0u &&
            trace.events[1].backendCalls == 1u,
        "release acknowledgement must follow older-source session encode");
  check(trace.events[2].drawCalls == 1u &&
            trace.events[2].queryIssuedSequence == 0u &&
            trace.events[2].backendCalls == 1u,
        "Query dispatch must begin only after the older release completes");
  check(trace.events[3].result == D3D_OK &&
            trace.events[3].drawCalls == 1u &&
            trace.events[3].queryIssuedSequence == 2u &&
            trace.events[3].backendCalls == 1u,
        "real Query side effect must occur exactly once before younger draw");
  check(fixture.routing->drawCalls.load(std::memory_order_relaxed) == 2u &&
            query->issuedSequenceId() == 2u,
        "mixed raw replay must dispatch both draws and the Query exactly "
        "once with no fallback replay");
  check(allSources.size() == 2 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
                queue, allSources),
        "shutdown must submit the preserved session containing both sides");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, allSources) == allSources.size(),
        "both fenced sources must retain exact completion attribution");
}

void productionReplayFencesEveryQueryInOneRaw() {
  RuntimeFixture fixture;
  auto buffer = fixture.device->CreateBuffer(BufferDesc{
      .size = 256u,
      .pool = Pool::Default,
      .usage = UsageVertexBuffer,
  });
  auto query = fixture.device->CreateQuery(QueryType::Occlusion);
  check(buffer != nullptr && query != nullptr,
        "multi-control production resources must construct");
  D9CBuffer wireBuffer(buffer, fixture.cDevice.get());
  D9CQuery wireQuery(query, fixture.cDevice.get());
  const std::array records{
      drawRecord(wireBuffer.wireIdentity, 0u),
      queryIssueRecord(wireQuery.wireIdentity, 1u),
      drawRecord(wireBuffer.wireIdentity, 2u),
      queryIssueRecord(wireQuery.wireIdentity, 3u),
      drawRecord(wireBuffer.wireIdentity, 4u),
  };
  auto raw = makeRaw(makeWireFixture(records), 79,
                     fixture.cDevice->wireObjects);

  auto& queue = fixture.routing->queue_;
  dxmt9::CommandQueueArenaLeaseTestAccess::
      enableCpuReadySessionReleaseLane(queue);
  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));
  OrderedControlReplayTrace trace{
      .routing = fixture.routing,
      .query = query.get(),
      .backend = backendState.get(),
  };
  dxmt9_test_set_ordered_control_replay_observer(
      &trace, recordOrderedControlReplayEvent);

  std::atomic<std::int32_t> replayResult{D3DERR_INVALIDCALL};
  std::thread replayThread([&] {
    replayResult.store(
        dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw),
        std::memory_order_release);
  });
  const bool firstReleasePublished = waitUntil([&] {
    return dxmt9::CommandQueueArenaLeaseTestAccess::
        hasPendingSessionRelease(queue);
  });
  const auto firstSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });
  replayThread.join();
  dxmt9_test_set_ordered_control_replay_observer(nullptr, nullptr);

  const bool bothFencedPrefixesEncoded =
      backendState->observedBackendCalls.load(std::memory_order_acquire) == 2;
  const bool finalSourcePublished =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          publishLegacyWritingSlot(queue);
  const bool finalSourceEncoded = waitUntil([&] {
    return backendState->observedBackendCalls.load(
               std::memory_order_acquire) == 3;
  });
  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  encodeThread.join();

  std::vector<dxmt9::core::metalqueue::QueueCompletionSource> allSources;
  for (const auto& call : backendState->calls) {
    check(call.sessionSource.has_value(),
          "every fenced draw range retains an exact completion source");
    allSources.push_back(*call.sessionSource);
  }

  check(firstReleasePublished && firstSources.size() == 1 &&
            replayResult.load(std::memory_order_acquire) == D3D_OK,
        "the first Query publishes and satisfies exactly its older draw prefix");
  check(bothFencedPrefixesEncoded && finalSourcePublished &&
            finalSourceEncoded && backendState->calls.size() == 3,
        "both Query fences and the final drain encode three draw ranges once");
  check(trace.events.size() == 8,
        "both Query records emit complete release/dispatch traces");
  for (std::size_t i = 0; i < trace.events.size(); ++i) {
    const auto& event = trace.events[i];
    check(event.phase == i % 4u + 1u &&
              event.recordIndex == (i < 4u ? 1u : 3u) &&
              event.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE,
          "each trace phase belongs to its exact Query record");
  }
  check(trace.events[1].backendCalls == 1u &&
            trace.events[3].queryIssuedSequence == 2u &&
            trace.events[4].drawCalls == 2u &&
            trace.events[4].backendCalls == 1u &&
            trace.events[5].backendCalls == 2u &&
            trace.events[7].result == D3D_OK &&
            trace.events[7].queryIssuedSequence == 4u,
        "the second Query independently releases the interstitial draw before "
        "its side effect");
  check(fixture.routing->drawCalls.load(std::memory_order_relaxed) == 3u &&
            query->issuedSequenceId() == 4u,
        "multi-control raw replay dispatches all records exactly once");
  check(allSources.size() == 3 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
                queue, allSources),
        "one preserved session submits all three ordered draw ranges");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, allSources) == allSources.size(),
        "all three ranges retain exact FIFO completion attribution");
}

void productionReplayGateOffLeavesQueryInCompatibilityWriter() {
  RuntimeFixture fixture;
  auto buffer = fixture.device->CreateBuffer(BufferDesc{
      .size = 256u,
      .pool = Pool::Default,
      .usage = UsageVertexBuffer,
  });
  auto query = fixture.device->CreateQuery(QueryType::Occlusion);
  check(buffer != nullptr && query != nullptr,
        "gate-off ordered-control resources must construct");
  D9CBuffer wireBuffer(buffer, fixture.cDevice.get());
  D9CQuery wireQuery(query, fixture.cDevice.get());
  const std::array records{
      drawRecord(wireBuffer.wireIdentity, 0u),
      queryIssueRecord(wireQuery.wireIdentity, 1u),
      drawRecord(wireBuffer.wireIdentity, 2u),
  };
  auto raw = makeRaw(makeWireFixture(records), 78,
                     fixture.cDevice->wireObjects);

  OrderedControlReplayTrace trace{
      .routing = fixture.routing,
      .query = query.get(),
  };
  dxmt9_test_set_ordered_control_replay_observer(
      &trace, recordOrderedControlReplayEvent);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  dxmt9_test_set_ordered_control_replay_observer(nullptr, nullptr);

  auto& queue = fixture.routing->queue_;
  check(hr == D3D_OK && trace.events.size() == 4,
        "gate-off mixed replay must retain historical dispatch behavior");
  check(fixture.routing->drawCalls.load(std::memory_order_relaxed) == 2u &&
            query->issuedSequenceId() == 2u,
        "gate-off path must dispatch both draws and Query exactly once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(queue) == 0 &&
            !dxmt9::CommandQueueArenaLeaseTestAccess::
                hasPendingSessionRelease(queue) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::
                acknowledgedSessionReleaseOrdinal(queue) == 0,
        "disabled release lane must not publish, fence, or acknowledge at "
        "the Query edge");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::
            publishLegacyWritingSlot(queue),
        "gate-off draws remain publishable as one compatibility source");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(queue) == 1,
        "gate-off replay must leave one combined compatibility source");
  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeBatch(queue, 1);
  check(completion.dequeued == 1 && completion.retained == 1 &&
            completion.submitted && completion.completed,
        "gate-off compatibility source must remain normally consumable");
}

// ---------------------------------------------------------------------------
// Arena payload block fixtures for the pure predicate and encoder-seam tests.

struct ArenaClearBlockFixture {
  std::vector<std::max_align_t> backing;
  SourcePayloadLayout layout{};
  ArenaSourcePayloadBlock block;

  explicit ArenaClearBlockFixture(bool presentTail = false) {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 1;
    if (presentTail) {
      capacity.presentRecords = 1;
    } else {
      capacity.clearRecords = 1;
    }
    const auto built = makeSourcePayloadLayout(capacity, 4096, 64);
    check(built.has_value(), "arena fixture layout must build");
    layout = *built;
    backing.resize((layout.usedBytes + sizeof(std::max_align_t) - 1) /
                   sizeof(std::max_align_t));
    std::span<std::byte> memory{
        reinterpret_cast<std::byte*>(backing.data()),
        backing.size() * sizeof(std::max_align_t)};
    ArenaSourcePayloadBuilder builder(block, layout,
                                      memory.first(layout.usedBytes));
    check(builder.good(), "arena fixture builder must bind");
    if (presentTail) {
      check(builder.tryAppendPresentCommand(PresentCommandRecord{}),
            "arena fixture present record must append");
    } else {
      ClearDesc clear{};
      clear.clearColor = true;
      check(builder.tryAppendClearCommand(clear),
            "arena fixture clear record must append");
    }
    check(builder.publish(), "arena fixture block must publish");
  }

  SourcePayloadView view() const { return SourcePayloadView(block); }
};

void sessionSourcePolicyClassifiesLegacyAndArena() {
  // Legacy shapes exercise the same pure policy used by the Tape coordinator.
  ChunkSlot head{};
  head.appendClear({});
  ChunkSlot presentTail{};
  presentTail.appendPresent({}, Handle{0x21});
  ChunkSlot midPresent{};
  midPresent.appendPresent({}, Handle{0x22});
  midPresent.appendClear({});

  const auto checkShape = [](const ChunkSlot& slot,
                             bool finalPresentTail,
                             bool sessionHead,
                             std::string_view what) {
    const SourcePayloadView view(slot);
    check(dxmt9::render::sessionSourceHasFinalPresentTail(view) ==
              finalPresentTail,
          std::string("present-tail classification: ") + std::string(what));
    check(dxmt9::render::sessionSourceCanBeHead(view) == sessionHead,
          std::string("session-head classification: ") + std::string(what));
    for (const bool pending : {false, true}) {
      check(dxmt9::render::sessionSourceCanAppendToPending(view, pending) ==
                (finalPresentTail || (pending && sessionHead)),
            std::string("append classification: ") + std::string(what));
    }
  };
  checkShape(head, false, true, "clear head");
  checkShape(presentTail, true, false, "present tail");
  checkShape(midPresent, false, false, "mid present");
  checkShape(ChunkSlot{}, false, false, "empty");

  // Arena shapes: a published clear block is a session head; a present-only
  // block is a final present tail.
  ArenaClearBlockFixture arenaClear;
  const auto arenaClearView = arenaClear.view();
  check(arenaClearView.isArena() && arenaClearView.commandCount() == 1,
        "arena clear fixture must expose one arena command");
  check(dxmt9::render::sessionSourceCanBeHead(arenaClearView),
        "arena clear source must be a session head");
  check(!dxmt9::render::sessionSourceHasFinalPresentTail(arenaClearView),
        "arena clear source is not a present tail");
  check(dxmt9::render::sessionSourceCanAppendToPending(arenaClearView, true),
        "arena clear source must append to an active session");
  check(!dxmt9::render::sessionSourceCanAppendToPending(arenaClearView, false),
        "arena clear source must not append without a session");

  ArenaClearBlockFixture arenaPresent(/*presentTail=*/true);
  const auto arenaPresentView = arenaPresent.view();
  check(arenaPresentView.presentRecordCount() == 1,
        "arena present fixture must expose one present record");
  check(dxmt9::render::sessionSourceHasFinalPresentTail(arenaPresentView),
        "arena present-only source is a final present tail");
  check(!dxmt9::render::sessionSourceCanBeHead(arenaPresentView),
        "arena present tail cannot open a session");

  check(dxmt9::render::sessionShouldSubmitBeforeInitializerWait(
            true, true, true),
        "initializer wait must submit an appendable active-render session");
  check(!dxmt9::render::sessionShouldSubmitBeforeInitializerWait(
            false, true, true),
        "non-appendable source is handled before initializer policy");
  check(!dxmt9::render::sessionShouldSubmitBeforeInitializerWait(
            true, false, true),
        "closed render session does not block initializer uploads");
  check(!dxmt9::render::sessionShouldSubmitBeforeInitializerWait(
            true, true, false),
        "no pending upload means no initializer submission boundary");
}

void neutralPrefixSelectorAdmitsMixedCandidates() {
  ChunkSlot legacyHead{};
  legacyHead.appendClear({});
  ChunkSlot legacyEmpty{};
  ChunkSlot legacyNonHead{};
  legacyNonHead.appendPresent({}, Handle{0x30});
  legacyNonHead.appendClear({});
  ChunkSlot legacyPresentTail{};
  legacyPresentTail.appendPresent({}, Handle{0x31});
  ArenaClearBlockFixture arenaClear;

  auto candidate = [](SourcePayloadView payload, std::uint64_t seqId) {
    dxmt9::core::metalqueue::ResolvedPublishedSource source{};
    source.payload = payload;
    source.seqId = seqId;
    source.slot = payload.legacyPayload();
    return source;
  };

  const std::array mixed{
      candidate(arenaClear.view(), 1),
      candidate(SourcePayloadView(legacyHead), 2),
      candidate(SourcePayloadView(legacyPresentTail), 3),
  };
  check(dxmt9::selectSessionSourceBatchPrefix(mixed) == 3,
        "neutral selector must admit arena+legacy heads through the tail");

  const std::array arenaOnly{
      candidate(arenaClear.view(), 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::selectSessionSourceBatchPrefix(arenaOnly) == 2,
        "neutral selector must admit a multi-arena head run");

  const std::array tailFirst{
      candidate(SourcePayloadView(legacyPresentTail), 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::selectSessionSourceBatchPrefix(tailFirst) == 0,
        "a leading present tail still falls back to single-source dequeue");

  const std::array invalidAfterHeads{
      candidate(arenaClear.view(), 1),
      candidate(SourcePayloadView(legacyHead), 2),
      candidate(SourcePayloadView{}, 3),
  };
  check(dxmt9::selectSessionSourceBatchPrefix(invalidAfterHeads) ==
            2,
        "a later invalid source must preserve the maximal valid head prefix");

  const std::array emptyAfterHeads{
      candidate(arenaClear.view(), 1),
      candidate(SourcePayloadView(legacyHead), 2),
      candidate(SourcePayloadView(legacyEmpty), 3),
  };
  check(dxmt9::selectSessionSourceBatchPrefix(emptyAfterHeads) == 2,
        "a later empty source must preserve the maximal valid head prefix");

  const std::array nonHeadAfterHead{
      candidate(arenaClear.view(), 1),
      candidate(SourcePayloadView(legacyNonHead), 2),
  };
  check(dxmt9::selectSessionSourceBatchPrefix(nonHeadAfterHead) == 1,
        "a later non-head source must preserve the maximal valid head prefix");

  const std::array leadingInvalid{
      candidate(SourcePayloadView{}, 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::selectSessionSourceBatchPrefix(leadingInvalid) == 0,
        "a leading invalid source must still reject the batch prefix");

  const std::array leadingEmpty{
      candidate(SourcePayloadView(legacyEmpty), 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::selectSessionSourceBatchPrefix(leadingEmpty) == 0,
        "a leading empty source must still reject the batch prefix");

  const std::array leadingNonHead{
      candidate(SourcePayloadView(legacyNonHead), 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::selectSessionSourceBatchPrefix(leadingNonHead) == 0,
        "a leading non-head source must still reject the batch prefix");
}

// ---------------------------------------------------------------------------
// Encoder seam: two Arena sources share one session and one command buffer.

struct EncoderHarness {
  dxmt9::core::BackendLimits limits{};
  dxmt9::resources::Pool pool{};
  dxmt9::pipeline::Cache cache{};
  dxmt9::scratch::FrameAllocators allocators{};
  WMT::Reference<WMT::Device> device =
      retainedToken<WMT::Device>("session-join-device-token");
  dxmt9::CommandQueue queue;

  EncoderHarness()
      : queue(dxmt9::CommandQueue::InertTestQueueTag{},
              retainedToken<WMT::CommandQueue>("session-join-queue-token"),
              limits) {}

  dxmt9::encoders::EncodeContext makeContext() {
    return dxmt9::encoders::EncodeContext{
        device,
        limits,
        pool,
        cache,
        allocators,
        nullptr,
        nullptr,
        queue,
    };
  }
};

dxmt9::core::metalqueue::QueueCompletionSource arenaSessionSource(
    std::size_t slotIndex,
    std::uint64_t seqId) {
  return dxmt9::core::metalqueue::QueueCompletionSource{
      .source = {
          .id = {
              .index = static_cast<std::uint32_t>(slotIndex),
              .generation = seqId,
          },
          .storage = {
              .firstPage = static_cast<std::uint32_t>(slotIndex),
              .pageCount = 1,
              .generation = seqId,
          },
      },
      .slotIndex = slotIndex,
      .seqId = seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 1,
  };
}

void arenaSessionCarryAcrossSourcesSharesOneCommandBuffer() {
  EncoderHarness harness;
  auto ctx = harness.makeContext();
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  check(static_cast<bool>(session), "session owner is created");

  ArenaClearBlockFixture headBlock;
  ArenaClearBlockFixture tailBlock;
  const auto headSource = arenaSessionSource(2, 41);
  const auto tailSource = arenaSessionSource(3, 42);

  auto commandBuffer =
      retainedToken<WMT::CommandBuffer>("session-join-cb-token");
  const obj_handle_t commandBufferHandle = commandBuffer.handle;

  auto makeOptions = [&session](
                         WMT::Reference<WMT::CommandBuffer> cb,
                         const dxmt9::core::metalqueue::QueueCompletionSource&
                             source) {
    dxmt9::encoders::EncodeChunkOptions options{};
    options.commandBuffer = std::move(cb);
    options.allowInjectedCommandBufferMidChunkCommits = true;
    options.session = session.get();
    options.deferSessionFinalization = true;
    options.sessionSource = source;
    options.partitionSource = source.source;
    return options;
  };

  auto head = dxmt9::encoders::encodeChunk(
      ctx, headSource.slotIndex, headBlock.view(), headSource.seqId,
      makeOptions(std::move(commandBuffer), headSource));
  check(head.has_value(),
        "arena head encodeChunk must return a deferred submission");
  check(head->commandBuffer.handle == commandBufferHandle,
        "arena head submission carries the injected command buffer");
  check(head->explicitCompletionSourceSpan().empty(),
        "deferred arena head does not publish session sources");
  check(dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "arena head clear remains deferred session payload");

  auto tail = dxmt9::encoders::encodeChunk(
      ctx, tailSource.slotIndex, tailBlock.view(), tailSource.seqId,
      makeOptions(std::move(head->commandBuffer), tailSource));
  check(tail.has_value(),
        "arena tail encodeChunk must return a deferred submission");
  check(tail->commandBuffer.handle == commandBufferHandle,
        "arena source boundary must not open a new command buffer");

  const auto sources = dxmt9::encoders::encodeChunkSessionSources(*session);
  check(sources.size() == 2 && sources[0].seqId == headSource.seqId &&
            sources[1].seqId == tailSource.seqId,
        "session must retain both arena sources in FIFO order");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "finalizer publishes the deferred arena session");
  const auto published = tail->explicitCompletionSourceSpan();
  check(published.size() == 2 && published[0].seqId == headSource.seqId &&
            published[1].seqId == tailSource.seqId &&
            tail->commandBuffer.handle == commandBufferHandle,
        "one submission covers both arena sources with FIFO attribution");
}

// ---------------------------------------------------------------------------
// Queue seam: mixed Arena/Legacy prefix through one submission.

void multipleArenaSourcesCompleteFifoThroughOneSubmission() {
  RuntimeFixture fixture;
  fixture.publishArenaClear(1);
  fixture.publishArenaClear(2);
  fixture.publishArenaClear(3);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 3,
        "three arena sources must be ready");

  const auto batch = dxmt9::CommandQueueArenaLeaseTestAccess::consumeBatch(
      fixture.routing->queue_, 8);
  check(batch.dequeued == 3 && batch.retained == 3,
        "the neutral prefix must represent all three arena sources at once");
  check(batch.arenaKinds == std::vector<bool>({true, true, true}),
        "every represented source must resolve as an arena payload");
  check(batch.seqIds == std::vector<std::uint64_t>({1, 2, 3}),
        "represented arena sources must retain FIFO seqId order");
  check(batch.submitted && batch.completed,
        "one submitted record must cover the whole arena prefix");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(
            fixture.routing->queue_) == 3,
        "completion must expand all three sources in FIFO order");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
            fixture.routing->queue_) == 0,
        "ordered reclaim must release every arena source");
}

void mixedLegacyAndArenaSourcesShareOneSubmission() {
  RuntimeFixture fixture;
  fixture.publishArenaClear(1);
  fixture.publishLegacyClear();
  fixture.publishArenaClear(2);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 3,
        "arena/legacy/arena sources must all be ready");

  const auto batch = dxmt9::CommandQueueArenaLeaseTestAccess::consumeBatch(
      fixture.routing->queue_, 8);
  check(batch.dequeued == 3 && batch.retained == 3,
        "the neutral prefix must admit the mixed arena/legacy run");
  check(batch.arenaKinds == std::vector<bool>({true, false, true}),
        "represented kinds must interleave arena and legacy sources");
  check(batch.seqIds == std::vector<std::uint64_t>({1, 2, 3}),
        "mixed sources must retain FIFO seqId order");
  check(batch.submitted && batch.completed,
        "one submitted record must cover the mixed prefix without an "
        "artificial submission boundary");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(
            fixture.routing->queue_) == 3,
        "mixed completion must expand per source in FIFO order");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
            fixture.routing->queue_) == 0,
        "mixed ordered reclaim must release every source");
}

}  // namespace

int main() {
  const bool perfOffCase =
      std::getenv("DXMT9_SESSION_JOIN_PERF_OFF_CASE") != nullptr;
  if (perfOffCase) {
    unsetenv("DXMT_PERF_COUNTERS");
  } else {
    setenv("DXMT_PERF_COUNTERS", "1", 1);
  }
  setenv("DXMT9_RENDERER_COMPAT_PROFILE", "progressive", 1);
  setenv("DXMT9_RENDERER_FEATURES", "passcoalesce", 1);
  try {
    if (perfOffCase) {
      productionLoopPerfOffKeepsSeedPlanWithoutTicketWork();
      productionLoopAttributesSessionCapCloseToSameKeyReopen(false);
      return 0;
    }
    if (const char* splitCase =
            std::getenv("DXMT9_SESSION_JOIN_SPLIT_POLICY_CASE");
        splitCase && std::strcmp(splitCase, "per-n-records") == 0) {
      productionLoopPlansFreshRepeatedSourceWindow();
      return 0;
    }
    if (std::getenv("DXMT9_SESSION_JOIN_P0_COMPOSITION_CASE")) {
      productionLoopPressureEscapesDeniedFirstLeaseOnce();
      return 0;
    }
    sessionSourcePolicyClassifiesLegacyAndArena();
    neutralPrefixSelectorAdmitsMixedCandidates();
    arenaSessionCarryAcrossSourcesSharesOneCommandBuffer();
    multipleArenaSourcesCompleteFifoThroughOneSubmission();
    mixedLegacyAndArenaSourcesShareOneSubmission();
    productionLoopJoinsMultipleArenaSourcesOnStopDrain();
    productionLoopJoinsMixedSourcesOnStopDrain();
    productionLoopPlansSeparateBThenASourcesIntoOneCarrier();
    productionLoopCanonicalizesNaturalCarrierBeforeReorderedComposite();
    productionLoopPlansFreshRepeatedSourceWindow();
    productionLoopRetainsOneReadyHeadForExactWritingSuccessor();
    productionLoopConsumesOneReadyHeadBehindActiveSession();
    productionLoopRestoresRetainedHeadBeforeStopDrain();
    productionLoopRestoresRetainedHeadBeforeOrderedRelease();
    plannerUnlockRestoresExactPrefixBeforeOrderedRelease();
    compositeObserverDoesNotOwnSchedulingMutex();
    productionLoopNaturalSourceKeepsDefaultPassSplitBaseline();
    productionLoopJoinsDeferredTerminalSuffix();
    productionLoopDrainsDeferredTerminalSuffixFallbacks();
    productionLoopAttributesNaturalFallbackAbaWithinOneWindow();
    productionLoopAttributesExactActiveSeedBridge();
    productionLoopRejectsWrongActiveSeedBridgeTarget();
    productionLoopAttributesExactActiveSeedContinuation();
    productionLoopDropsOnlyTicketWhenActiveSeedInstanceTurnsStale();
    productionLoopAttributesPermutationRejectedFallbackWindow();
    productionLoopBoundsFreshNineReadySources();
    productionLoopRevisitsOneSourceWithoutRepeatingItsPreamble();
    productionLoopStoreProofLookaheadOverflowFailsClosed();
    productionLoopOrderedReleaseFencesRawInterposition();
    productionLoopBoundsNineReadySourcesToFirstPlanningWindow();
    productionLoopCarriesActivePassAcrossBoundedWindowEdge();
    productionLoopPlansPrefixBeforePresentBoundary();
    productionLoopPressureEscapesDeniedFirstLeaseOnce();
    productionLoopLeaseWaitResumesAfterGpuReclaim();
    productionLoopLeaseWaitResumesAfterInlineReclaim();
    productionLoopCreditsExactReadyAndWritingSuccessor();
    productionLoopReleasesAtDeterministicCapBeforeWriterPressure();
    productionLoopAttributesSessionCapCloseToSameKeyReopen(true);
    orderedClosePassKeepsFencedSuffixReadyAndPreservesSession();
    orderedClosePassEnablesYoungerMovedHeadOnSameSession();
    orderedSubmitAcknowledgesAfterNonPresentPrefixSubmission();
    tentativeCoordinatorPreflightRestoresExactFifoOrder();
    productionReplayFencesQueryBetweenOlderAndYoungerDraws();
    productionReplayFencesEveryQueryInOneRaw();
    productionReplayGateOffLeavesQueryInCompatibilityWriter();
  } catch (const TestFailure& error) {
    std::cerr << "cpu_ready_session_join_spec failed: " << error.what()
              << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "cpu_ready_session_join_spec unexpected error: "
              << error.what() << '\n';
    return 1;
  }
  std::cout << "cpu_ready_session_join_spec passed\n";
  return 0;
}
