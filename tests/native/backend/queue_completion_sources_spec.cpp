#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../../../src/dxmt9/dxmt9_queue.hpp"

namespace {

using dxmt9::core::metalqueue::QueueCompletionSource;
using dxmt9::core::metalqueue::QueueLifecycleController;
using dxmt9::core::metalqueue::QueueSubmissionRecord;
using dxmt9::core::metalqueue::ReadySlotSnapshot;
using dxmt9::core::metalqueue::appendCompletionSourcesToQueues;
using dxmt9::core::metalqueue::completionSourceForReadySlot;
using dxmt9::core::metalqueue::mergeEncodedPendingTailSubmission;
using dxmt9::core::metalqueue::mergeCommandBufferDiagnostics;
using dxmt9::core::ChunkSlot;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    fail(std::string(message));
  }
}

template <typename T>
std::span<const T> asSpan(const std::vector<T>& values) {
  return std::span<const T>(values.data(), values.size());
}

void appendsSingleLegacySource() {
  std::deque<std::uint64_t> completed;
  std::deque<std::uint64_t> presentCompleted;
  const std::vector<QueueCompletionSource> sources = {{
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};

  appendCompletionSourcesToQueues(completed, &presentCompleted, 0, asSpan(sources));

  checkEq(completed.size(), 1u, "single source appends one completed seq");
  checkEq(completed.front(), 1ull, "single source appends seq 1");
  check(presentCompleted.empty(), "non-present source skips present queue");
}

void appendsMultiSourceBatchInStrictSeqOrder() {
  std::deque<std::uint64_t> completed;
  std::deque<std::uint64_t> presentCompleted;
  const std::vector<QueueCompletionSource> sources = {
      {
          .slotIndex = 3,
          .seqId = 5,
          .hasPresent = false,
      },
      {
          .slotIndex = 4,
          .seqId = 6,
          .hasPresent = true,
      },
      {
          .slotIndex = 5,
          .seqId = 7,
          .hasPresent = false,
      },
  };

  appendCompletionSourcesToQueues(completed, &presentCompleted, 4, asSpan(sources));

  checkEq(completed.size(), 3u, "multi source appends every seq");
  checkEq(completed[0], 5ull, "multi source seq 5");
  checkEq(completed[1], 6ull, "multi source seq 6");
  checkEq(completed[2], 7ull, "multi source seq 7");
  checkEq(presentCompleted.size(), 1u, "only present source enters present queue");
  checkEq(presentCompleted.front(), 6ull, "present queue records source seq");
}

void respectsAlreadyQueuedCompletions() {
  std::deque<std::uint64_t> completed;
  std::deque<std::uint64_t> presentCompleted;
  completed.push_back(11);
  const std::vector<QueueCompletionSource> sources = {{
      .slotIndex = 2,
      .seqId = 12,
      .hasPresent = true,
  }};

  appendCompletionSourcesToQueues(completed, &presentCompleted, 10, asSpan(sources));

  checkEq(completed.size(), 2u, "existing completed entries are retained");
  checkEq(completed[0], 11ull, "existing seq remains first");
  checkEq(completed[1], 12ull, "new seq follows queued seq");
  checkEq(presentCompleted.size(), 1u, "present seq appends with existing completed queue");
  checkEq(presentCompleted.front(), 12ull, "present seq matches appended source");
}

void presentQueueMayBeAbsent() {
  std::deque<std::uint64_t> completed;
  const std::vector<QueueCompletionSource> sources = {{
      .slotIndex = 1,
      .seqId = 3,
      .hasPresent = true,
  }};

  appendCompletionSourcesToQueues(completed, nullptr, 2, asSpan(sources));

  checkEq(completed.size(), 1u, "completed queue appends without present queue");
  checkEq(completed.front(), 3ull, "completed seq is preserved without present queue");
}

void diagnosticsMergeKeepsTailIdentityAndAggregatesSourceShape() {
  dxmt9::core::metalqueue::CommandBufferDiagnostics aggregate{
      .seqId = 9,
      .slotIndex = 3,
      .hasDraw = true,
      .compatFlags = 0x01,
      .vertexShaderHash = 0x10,
      .pixelShaderHash = 0x20,
      .shaderVariantHash = 0x30,
  };
  const dxmt9::core::metalqueue::CommandBufferDiagnostics source{
      .seqId = 7,
      .slotIndex = 1,
      .hasPresent = true,
      .hasBlit = true,
      .hasStretchRect = true,
      .frame = 42,
      .compatFlags = 0x04,
      .vertexShaderHash = 0x11,
      .pixelShaderHash = 0x21,
      .shaderVariantHash = 0x31,
  };

  const auto merged = mergeCommandBufferDiagnostics(aggregate, source);

  checkEq(merged.seqId, 9ull, "merged diagnostics keep tail seq identity");
  checkEq(merged.slotIndex, 3u, "merged diagnostics keep tail slot identity");
  check(merged.hasDraw, "merged diagnostics retain draw flag");
  check(merged.hasPresent, "merged diagnostics aggregate present flag");
  check(merged.hasBlit, "merged diagnostics aggregate blit flag");
  check(merged.hasStretchRect, "merged diagnostics aggregate stretch flag");
  checkEq(merged.frame, 42u, "merged diagnostics pick first non-zero frame");
  checkEq(merged.compatFlags, 0x05u, "merged diagnostics OR compat flags");
  checkEq(merged.vertexShaderHash, 0x11ull, "merged diagnostics use latest VS hash");
  checkEq(merged.pixelShaderHash, 0x21ull, "merged diagnostics use latest PS hash");
  checkEq(merged.shaderVariantHash, 0x31ull,
          "merged diagnostics use latest shader variant hash");
}

struct QueueFixture {
  std::optional<std::size_t> writingSlot{};
  std::size_t writeIndex = 0;
  std::uint64_t nextSeqId = 1;
  std::deque<std::size_t> readySlots{};
  std::deque<std::uint64_t> completedSeqQueue{};
  std::deque<std::uint64_t> completedPresentSeqQueue{};
  std::size_t inflightCount = 0;
  std::uint64_t completedSeqId = 0;
  std::uint64_t presentCompletedSeqId = 0;
  std::uint64_t lastCommittedSeqId = 0;
  std::array<ChunkSlot, 4> slots{};
  std::mutex mutex{};
  std::condition_variable writeCv{};
  std::condition_variable encodeCv{};
  std::condition_variable finishCv{};
  std::condition_variable presentCompletedCv{};
  bool stop = false;
  QueueLifecycleController controller{};

  QueueFixture() {
    controller.bindTrackedSubmissionState(QueueLifecycleController::SubmissionBinding{
        .writingSlot = &writingSlot,
        .writeIndex = &writeIndex,
        .nextSeqId = &nextSeqId,
        .readySlots = &readySlots,
        .completedSeqQueue = &completedSeqQueue,
        .completedPresentSeqQueue = &completedPresentSeqQueue,
        .inflightCount = &inflightCount,
        .completedSeqId = &completedSeqId,
        .presentCompletedSeqId = &presentCompletedSeqId,
        .lastCommittedSeqId = &lastCommittedSeqId,
        .slots = std::span<ChunkSlot>(slots.data(), slots.size()),
        .mutex = &mutex,
        .writeCv = &writeCv,
        .encodeCv = &encodeCv,
        .finishCv = &finishCv,
        .presentCompletedCv = &presentCompletedCv,
        .stop = &stop,
    });
  }

  void addReadySlot(std::size_t slotIndex, std::uint64_t seqId) {
    slots[slotIndex].state = ChunkSlot::State::Pending;
    slots[slotIndex].seqId = seqId;
    readySlots.push_back(slotIndex);
    lastCommittedSeqId = std::max(lastCommittedSeqId, seqId);
    nextSeqId = std::max(nextSeqId, seqId + 1u);
    ++inflightCount;
  }
};

void dequeueReadySlotBatchMovesEveryDequeuedSlotToEncoding() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);

  std::array<ReadySlotSnapshot, 2> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(snapshots));

  checkEq(count, 2u, "batch dequeue returns every requested ready slot");
  check(fixture.readySlots.empty(), "batch dequeue drains ready queue up to capacity");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "first source slot moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Encoding,
        "second source slot moves to Encoding");
  checkEq(snapshots[0].slotIndex, 0u, "first snapshot records slot index");
  checkEq(snapshots[0].slot.seqId, 1ull, "first snapshot records seqId");
  checkEq(snapshots[1].slotIndex, 1u, "second snapshot records slot index");
  checkEq(snapshots[1].slot.seqId, 2ull, "second snapshot records seqId");
}

void dequeueReadySlotBatchRespectsOutputCapacity() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  std::array<ReadySlotSnapshot, 2> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(snapshots));

  checkEq(count, 2u, "batch dequeue is capped by caller storage");
  checkEq(fixture.readySlots.size(), 1u, "capacity-limited batch leaves remaining ready slot");
  checkEq(fixture.readySlots.front(), 2u, "remaining ready slot keeps FIFO order");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "first capacity slot moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Encoding,
        "second capacity slot moves to Encoding");
  check(fixture.slots[2].state == ChunkSlot::State::Pending,
        "overflow ready slot remains Pending");
}

void dequeueReadySlotBatchHonorsAppendPredicate() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  std::array<ReadySlotSnapshot, 3> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatch(
          lock,
          std::span<ReadySlotSnapshot>(snapshots),
          [](std::span<const ReadySlotSnapshot> selected,
             std::size_t candidateSlotIndex,
             const ChunkSlot& candidateSlot) {
            checkEq(selected.size(), 1u,
                    "predicate sees the already-selected source");
            checkEq(candidateSlotIndex, 1u,
                    "predicate sees the next FIFO candidate");
            checkEq(candidateSlot.seqId, 2ull,
                    "predicate sees the candidate slot payload");
            return false;
          });

  checkEq(count, 1u, "batch predicate stops after the first source");
  checkEq(fixture.readySlots.size(), 2u,
          "rejected candidates remain ready for later encode iterations");
  checkEq(fixture.readySlots.front(), 1u,
          "first rejected candidate keeps FIFO position");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "accepted source moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Pending,
        "rejected candidate remains Pending");
  check(fixture.slots[2].state == ChunkSlot::State::Pending,
        "later candidate remains Pending");
}

void dequeueReadySlotBatchPrefixUsesCompleteSelectorCount() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  bool selectorCalled = false;
  std::array<ReadySlotSnapshot, 3> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatchPrefix(
          lock,
          std::span<ReadySlotSnapshot>(snapshots),
          [&](const std::deque<std::size_t>& readySlots,
              std::span<const ChunkSlot> slots,
              std::size_t maxCount) {
            selectorCalled = true;
            checkEq(maxCount, 3u, "selector sees caller capacity");
            checkEq(readySlots.size(), 3u, "selector sees ready depth");
            checkEq(readySlots[0], 0u, "selector sees first FIFO source");
            checkEq(readySlots[1], 1u, "selector sees second FIFO source");
            checkEq(readySlots[2], 2u, "selector sees third FIFO source");
            checkEq(slots[2].seqId, 3ull, "selector can inspect slot payloads");
            return 3u;
          });

  check(selectorCalled, "prefix selector is invoked");
  checkEq(count, 3u, "prefix selector controls dequeue count");
  check(fixture.readySlots.empty(), "complete prefix drains selected sources");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "first prefix source moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Encoding,
        "second prefix source moves to Encoding");
  check(fixture.slots[2].state == ChunkSlot::State::Encoding,
        "third prefix source moves to Encoding");
  checkEq(snapshots[2].slotIndex, 2u, "third snapshot records slot index");
  checkEq(snapshots[2].slot.seqId, 3ull, "third snapshot records seqId");
}

void dequeueReadySlotBatchPrefixFallsBackToSingleWhenSelectorRejects() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  std::array<ReadySlotSnapshot, 3> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatchPrefix(
          lock,
          std::span<ReadySlotSnapshot>(snapshots),
          [](const std::deque<std::size_t>&,
             std::span<const ChunkSlot>,
             std::size_t) { return 0u; });

  checkEq(count, 1u, "rejected prefix falls back to one source");
  checkEq(fixture.readySlots.size(), 2u,
          "fallback leaves later ready sources pending");
  checkEq(fixture.readySlots.front(), 1u,
          "fallback keeps the first rejected source FIFO-visible");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "fallback source moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Pending,
        "first rejected source remains Pending");
  check(fixture.slots[2].state == ChunkSlot::State::Pending,
        "later rejected source remains Pending");
  checkEq(snapshots[0].slotIndex, 0u, "fallback snapshot records first source");
  checkEq(snapshots[0].slot.seqId, 1ull, "fallback snapshot records first seq");
}

void stagedReadySlotIsHiddenUntilReadyTailRelease() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  std::deque<std::size_t> stagedSlots;
  std::unique_lock lock(fixture.mutex);
  const bool staged = fixture.controller.stageLastReadySlot(
      lock, stagedSlots, /*expectedSlotIndex=*/0);

  check(staged, "last ready slot can move to staging");
  check(fixture.readySlots.empty(), "staged slot is not encode-visible");
  checkEq(stagedSlots.size(), 1u, "staged queue records one source");
  checkEq(stagedSlots.front(), 0u, "staged source keeps slot index");
  check(fixture.slots[0].state == ChunkSlot::State::Pending,
        "staged source remains Pending");

  fixture.addReadySlot(1, 2);
  const std::size_t released = fixture.controller.releaseStagedSlotsBeforeReadyTail(
      lock, stagedSlots, /*tailSlotIndex=*/1);

  checkEq(released, 1u, "release returns staged source count");
  check(stagedSlots.empty(), "release drains staged queue");
  checkEq(fixture.readySlots.size(), 2u,
          "release makes staged source and tail encode-visible");
  checkEq(fixture.readySlots[0], 0u, "staged source is released before tail");
  checkEq(fixture.readySlots[1], 1u, "tail remains last ready source");
  check(fixture.slots[0].state == ChunkSlot::State::Pending,
        "released source remains Pending until dequeue");
  check(fixture.slots[1].state == ChunkSlot::State::Pending,
        "tail source remains Pending until dequeue");
}

void severalStagedReadySlotsReleaseBeforeReadyTailInFifoOrder() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  std::deque<std::size_t> stagedSlots;
  std::unique_lock lock(fixture.mutex);
  check(fixture.controller.stageLastReadySlot(
            lock, stagedSlots, /*expectedSlotIndex=*/0),
        "test setup stages first source");
  fixture.addReadySlot(1, 2);
  check(fixture.controller.stageLastReadySlot(
            lock, stagedSlots, /*expectedSlotIndex=*/1),
        "test setup stages second source");
  check(fixture.readySlots.empty(), "all staged sources are hidden");

  fixture.addReadySlot(2, 3);
  const std::size_t released = fixture.controller.releaseStagedSlotsBeforeReadyTail(
      lock, stagedSlots, /*tailSlotIndex=*/2);

  checkEq(released, 2u, "release returns every staged source");
  check(stagedSlots.empty(), "release drains all staged sources");
  checkEq(fixture.readySlots.size(), 3u,
          "release makes staged sources plus tail encode-visible");
  checkEq(fixture.readySlots[0], 0u,
          "release preserves staged FIFO insertion order");
  checkEq(fixture.readySlots[1], 1u,
          "release preserves the second staged source order");
  checkEq(fixture.readySlots[2], 2u, "tail remains after staged sources");
}

void stagedReadySlotReleaseRequiresMatchingTail() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  std::deque<std::size_t> stagedSlots;
  std::unique_lock lock(fixture.mutex);
  check(fixture.controller.stageLastReadySlot(
            lock, stagedSlots, /*expectedSlotIndex=*/0),
        "test setup stages one source");

  fixture.addReadySlot(1, 2);
  const std::size_t released = fixture.controller.releaseStagedSlotsBeforeReadyTail(
      lock, stagedSlots, /*tailSlotIndex=*/2);

  checkEq(released, 0u, "wrong tail index does not release staged slots");
  checkEq(stagedSlots.size(), 1u, "staged source is retained after failed release");
  checkEq(fixture.readySlots.size(), 1u, "ready tail stays visible");
  checkEq(fixture.readySlots.front(), 1u, "ready queue remains unchanged");
  check(fixture.slots[0].state == ChunkSlot::State::Pending,
        "failed release keeps staged source Pending");
  check(fixture.slots[1].state == ChunkSlot::State::Pending,
        "failed release keeps tail Pending");
}

void completionSourceForReadySlotPreservesPresentMetadata() {
  ReadySlotSnapshot snapshot{};
  snapshot.slotIndex = 3;
  snapshot.slot.seqId = 7;
  snapshot.slot.presentRecords.push_back({});

  const auto source = completionSourceForReadySlot(snapshot);

  checkEq(source.slotIndex, 3u, "completion source preserves slot index");
  checkEq(source.seqId, 7ull, "completion source preserves seqId");
  check(source.hasPresent, "completion source derives present metadata");
}

void retainEncodedSourcesRejectsPendingSources() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  ReadySlotSnapshot snapshot{};
  snapshot.slotIndex = 0;
  snapshot.slot = fixture.slots[0];
  std::array<QueueCompletionSource, 1> retained{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count = fixture.controller.retainEncodedSourcesForPendingTail(
      lock,
      std::span<const ReadySlotSnapshot>(&snapshot, 1),
      std::span<QueueCompletionSource>(retained));

  checkEq(count, 0u, "pending sources are not retained as encoded heads");
  checkEq(fixture.readySlots.size(), 1u,
          "rejected pending source remains ready-visible");
  check(fixture.slots[0].state == ChunkSlot::State::Pending,
        "rejected pending source keeps Pending state");
}

void retainedEncodedHeadCompletesOnlyWithTailCarrier() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  std::array<ReadySlotSnapshot, 1> head{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t headCount =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(head));
  checkEq(headCount, 1u, "test setup dequeues one head source");
  check(fixture.readySlots.empty(), "encoded head is no longer ready-visible");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "head source enters Encoding before retention");

  std::array<QueueCompletionSource, 1> retainedHeads{};
  const std::size_t retainedCount = fixture.controller.retainEncodedSourcesForPendingTail(
      lock,
      std::span<const ReadySlotSnapshot>(head.data(), headCount),
      std::span<QueueCompletionSource>(retainedHeads));
  checkEq(retainedCount, 1u, "encoded head is retained for the pending tail");
  checkEq(retainedHeads[0].slotIndex, 0u, "retained head keeps slot index");
  checkEq(retainedHeads[0].seqId, 1ull, "retained head keeps seqId");
  check(!retainedHeads[0].hasPresent, "retained head is not a present source");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "retaining head does not make it GPU-visible");

  fixture.addReadySlot(1, 2);
  fixture.slots[1].presentRecords.push_back({});
  std::array<ReadySlotSnapshot, 1> tail{};
  const std::size_t tailCount =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(tail));
  checkEq(tailCount, 1u, "test setup dequeues the present tail");
  check(tail[0].slot.presentRecords.size() == 1u,
        "tail snapshot carries present metadata");

  QueueSubmissionRecord record;
  record.slotIndex = tail[0].slotIndex;
  record.seqId = tail[0].slot.seqId;
  record.completionSources.reserve(retainedCount + tailCount);
  record.completionSources.push_back(retainedHeads[0]);
  record.completionSources.push_back(completionSourceForReadySlot(tail[0]));

  fixture.controller.submitEncodedSubmission(lock, record);
  check(fixture.slots[0].state == ChunkSlot::State::GPU,
        "retained head enters GPU state with the tail carrier");
  check(fixture.slots[1].state == ChunkSlot::State::GPU,
        "tail source enters GPU state with the same carrier");
  check(fixture.completedSeqQueue.empty(),
        "carrier submission alone does not mark sources completed");

  appendCompletionSourcesToQueues(
      fixture.completedSeqQueue,
      &fixture.completedPresentSeqQueue,
      fixture.completedSeqId,
      asSpan(record.completionSources));

  std::uint64_t finishedSeq = 0;
  const bool finishedHead = fixture.controller.runFinishIteration(
      lock, [&](std::uint64_t seqId) { finishedSeq = seqId; });
  check(finishedHead, "head completion drains first");
  checkEq(finishedSeq, 1ull, "head seq completes first");
  check(fixture.slots[0].state == ChunkSlot::State::Free,
        "head is freed only after tail-carrier completion");
  check(fixture.slots[1].state == ChunkSlot::State::GPU,
        "tail remains GPU-visible until its own completion drains");
  checkEq(fixture.presentCompletedSeqId, 0ull,
          "present completion waits for the present tail seq");

  const bool finishedTail = fixture.controller.runFinishIteration(
      lock, [&](std::uint64_t seqId) { finishedSeq = seqId; });
  check(finishedTail, "tail completion drains second");
  checkEq(finishedSeq, 2ull, "tail seq completes second");
  check(fixture.slots[1].state == ChunkSlot::State::Free,
        "tail is freed after its completion drains");
  checkEq(fixture.completedSeqId, 2ull, "completed seq advances through tail");
  checkEq(fixture.presentCompletedSeqId, 2ull,
          "present completion advances at the tail seq");
}

QueueSubmissionRecord::RenderEncoderGpuSample makeGpuSample(
    std::uint32_t commandIndex,
    std::uint64_t seqId) {
  return QueueSubmissionRecord::RenderEncoderGpuSample{
      .startIndex = commandIndex * 2u,
      .endIndex = commandIndex * 2u + 1u,
      .seqId = seqId,
      .commandIndex = commandIndex,
  };
}

void mergeEncodedPendingTailSubmissionPreservesHeadThenTailOrder() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;
  head.commandBufferChainLength = 3;
  head.diagnostics = dxmt9::core::metalqueue::CommandBufferDiagnostics{
      .seqId = 1,
      .slotIndex = 0,
      .hasDraw = true,
      .vertexShaderHash = 0x10,
      .pixelShaderHash = 0x20,
  };
  head.renderEncoderGpuSamples.push_back(makeGpuSample(1, 1));
  head.postCommitCallbacks.push_back([] {});
  head.completionCallbacks.push_back([] {});

  QueueSubmissionRecord tail;
  tail.slotIndex = 1;
  tail.seqId = 2;
  tail.commandBufferChainLength = 2;
  tail.diagnostics = dxmt9::core::metalqueue::CommandBufferDiagnostics{
      .seqId = 2,
      .slotIndex = 1,
      .hasPresent = true,
      .hasBlit = true,
      .vertexShaderHash = 0x30,
      .pixelShaderHash = 0x40,
  };
  tail.renderEncoderGpuSamples.push_back(makeGpuSample(2, 2));
  tail.postCommitCallbacks.push_back([] {});
  tail.completionCallbacks.push_back([] {});

  const std::array<QueueCompletionSource, 1> headSources{QueueCompletionSource{
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};
  const QueueCompletionSource tailSource{
      .slotIndex = 1,
      .seqId = 2,
      .hasPresent = true,
  };

  const bool merged = mergeEncodedPendingTailSubmission(
      tail,
      std::move(head),
      std::span<const QueueCompletionSource>(
          headSources.data(), headSources.size()),
      tailSource);

  check(merged, "encoded head submission merges into the tail record");
  checkEq(tail.slotIndex, 1u, "merged record keeps tail slot identity");
  checkEq(tail.seqId, 2ull, "merged record keeps tail seq identity");
  checkEq(tail.commandBufferChainLength, 4ull,
          "chain length counts head sub-CBs plus one final tail commit");
  checkEq(tail.completionSources.size(), 2u,
          "merged record carries head and tail completion sources");
  checkEq(tail.completionSources[0].seqId, 1ull,
          "head completion source stays first");
  checkEq(tail.completionSources[1].seqId, 2ull,
          "tail completion source stays second");
  check(tail.completionSources[1].hasPresent,
        "tail completion source carries present metadata");
  check(tail.diagnostics.hasDraw, "merged diagnostics include head draw work");
  check(tail.diagnostics.hasPresent,
        "merged diagnostics include tail present work");
  check(tail.diagnostics.hasBlit, "merged diagnostics include tail blit work");
  checkEq(tail.diagnostics.seqId, 2ull,
          "merged diagnostics keep tail seq identity");
  checkEq(tail.diagnostics.vertexShaderHash, 0x30ull,
          "tail shader hash wins as the latest source");
  checkEq(tail.renderEncoderGpuSamples.size(), 2u,
          "render encoder samples are merged");
  checkEq(tail.renderEncoderGpuSamples[0].seqId, 1ull,
          "head render sample stays before tail sample");
  checkEq(tail.renderEncoderGpuSamples[1].seqId, 2ull,
          "tail render sample stays after head sample");
  checkEq(tail.postCommitCallbacks.size(), 2u,
          "post-commit callbacks are merged");
  checkEq(tail.completionCallbacks.size(), 2u,
          "completion callbacks are merged");
}

void mergeEncodedPendingTailSubmissionRejectsSequenceGaps() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;

  QueueSubmissionRecord tail;
  tail.slotIndex = 2;
  tail.seqId = 3;
  tail.commandBufferChainLength = 9;

  const std::array<QueueCompletionSource, 1> headSources{QueueCompletionSource{
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};
  const QueueCompletionSource tailSource{
      .slotIndex = 2,
      .seqId = 3,
      .hasPresent = true,
  };

  const bool merged = mergeEncodedPendingTailSubmission(
      tail,
      std::move(head),
      std::span<const QueueCompletionSource>(
          headSources.data(), headSources.size()),
      tailSource);

  check(!merged, "sequence gaps are rejected");
  check(tail.completionSources.empty(),
        "failed merge leaves tail completion sources untouched");
  checkEq(tail.commandBufferChainLength, 9ull,
          "failed merge leaves tail chain length untouched");
}

void runEncodeBatchIterationCompletesEmptySubmissionInline() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);

  std::array<ReadySlotSnapshot, 2> snapshots{};
  std::vector<QueueCompletionSource> completedSources;
  std::vector<std::uint64_t> inlineCompleted;
  std::unique_lock lock(fixture.mutex);
  const bool encoded = fixture.controller.runEncodeBatchIteration(
      lock,
      std::span<ReadySlotSnapshot>(snapshots),
      [&](std::span<ReadySlotSnapshot> sources) {
        checkEq(sources.size(), 2u, "batch encode receives both source slots");
        completedSources.reserve(sources.size());
        for (const auto& source : sources) {
          completedSources.push_back(completionSourceForReadySlot(source));
        }

        dxmt9::core::metalqueue::QueueSubmissionRecord record;
        record.slotIndex = sources.back().slotIndex;
        record.seqId = sources.back().slot.seqId;
        return std::optional<dxmt9::core::metalqueue::QueueSubmissionRecord>(
            std::move(record));
      },
      [&](std::uint64_t seqId) { inlineCompleted.push_back(seqId); });

  check(encoded, "batch iteration encodes when ready slots are available");
  checkEq(completedSources.size(), 2u, "test captured every completion source");
  check(!completedSources[0].hasPresent, "first source is non-present");
  check(!completedSources[1].hasPresent, "second source is non-present");
  checkEq(inlineCompleted.size(), 2u,
          "empty submission completes every source inline");
  checkEq(inlineCompleted[0], 1ull, "first source inline completion");
  checkEq(inlineCompleted[1], 2ull, "second source inline completion");
  checkEq(fixture.completedSeqQueue.size(), 2u,
          "inline completion queues every source seq");
  check(fixture.completedPresentSeqQueue.empty(),
        "inline completion has no present source seq");

  std::uint64_t finishedSeq = 0;
  const bool finishedFirst = fixture.controller.runFinishIteration(
      lock, [&](std::uint64_t seqId) { finishedSeq = seqId; });
  check(finishedFirst, "first source finish iteration succeeds");
  checkEq(finishedSeq, 1ull, "first source finishes first");
  checkEq(fixture.completedSeqId, 1ull, "completed seq advances to first source");
  check(fixture.slots[0].state == ChunkSlot::State::Free,
        "first source is reclaimed after first finish");
  check(fixture.slots[1].state == ChunkSlot::State::Free,
        "second inline source is already free");

  const bool finishedSecond = fixture.controller.runFinishIteration(
      lock, [&](std::uint64_t seqId) { finishedSeq = seqId; });
  check(finishedSecond, "second source finish iteration succeeds");
  checkEq(finishedSeq, 2ull, "tail source finishes second");
  checkEq(fixture.completedSeqId, 2ull, "completed seq advances to tail source");
  checkEq(fixture.presentCompletedSeqId, 0ull,
          "present completion does not advance for non-present inline sources");
}

}  // namespace

int main() {
  try {
    appendsSingleLegacySource();
    appendsMultiSourceBatchInStrictSeqOrder();
    respectsAlreadyQueuedCompletions();
    presentQueueMayBeAbsent();
    diagnosticsMergeKeepsTailIdentityAndAggregatesSourceShape();
    dequeueReadySlotBatchMovesEveryDequeuedSlotToEncoding();
    dequeueReadySlotBatchRespectsOutputCapacity();
    dequeueReadySlotBatchHonorsAppendPredicate();
    dequeueReadySlotBatchPrefixUsesCompleteSelectorCount();
    dequeueReadySlotBatchPrefixFallsBackToSingleWhenSelectorRejects();
    stagedReadySlotIsHiddenUntilReadyTailRelease();
    severalStagedReadySlotsReleaseBeforeReadyTailInFifoOrder();
    stagedReadySlotReleaseRequiresMatchingTail();
    completionSourceForReadySlotPreservesPresentMetadata();
    retainEncodedSourcesRejectsPendingSources();
    retainedEncodedHeadCompletesOnlyWithTailCarrier();
    mergeEncodedPendingTailSubmissionPreservesHeadThenTailOrder();
    mergeEncodedPendingTailSubmissionRejectsSequenceGaps();
    runEncodeBatchIterationCompletesEmptySubmissionInline();
  } catch (const TestFailure& error) {
    std::cerr << "queue_completion_sources_spec failed: " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "queue_completion_sources_spec unexpected exception: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}
