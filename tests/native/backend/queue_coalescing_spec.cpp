#include "../../../src/dxmt9/dxmt9_queue.hpp"

#include <array>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dxmt9::core::metalqueue {

struct QueueLifecycleControllerTestPeer {
  static void enqueuePendingCompletion(QueueLifecycleController& lifecycle,
                                       QueueLifecycleController::PendingCompletion pending) {
    {
      std::lock_guard lock(lifecycle.pendingCompletionMutex_);
      lifecycle.pendingCompletion_.push_back(std::move(pending));
#ifndef NDEBUG
      lifecycle.assertPendingCompletionInvariantsLocked();
#endif
    }
    lifecycle.pendingCompletionCv_.notify_all();
  }
};

}  // namespace dxmt9::core::metalqueue

namespace {

using namespace dxmt9::core;
namespace metalqueue = dxmt9::core::metalqueue;

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

ClearDesc makeClear(Handle colorHandle) {
  ClearDesc clear{};
  clear.clearColor = true;
  clear.colorAttachments[0].handle = colorHandle;
  return clear;
}

SwapDesc makePresent() {
  SwapDesc present{};
  present.window = Handle{0x1000};
  present.sourceSurface = Handle{0x2000};
  present.width = 64;
  present.height = 64;
  return present;
}

void drainOne(metalqueue::QueueLifecycleController& lifecycle,
              std::unique_lock<std::mutex>& lock,
              u64 expectedSeqId) {
  u64 seqId = 0;
  check(lifecycle.drainCompletedSequence(lock, seqId),
        "completed sequence should drain");
  checkEq(seqId, expectedSeqId, "completed sequence order");
}

void testCoalescesNonPresentReadySlotsBeforePresent() {
  setenv("DXMT9_ENCODE_COALESCE_READY_SLOTS", "1", 1);
  setenv("DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT", "4", 1);

  std::array<ChunkSlot, 3> slots{};
  slots[0].state = ChunkSlot::State::Pending;
  slots[0].seqId = 1;
  slots[0].appendClear(makeClear(Handle{0x10}));
  slots[1].state = ChunkSlot::State::Pending;
  slots[1].seqId = 2;
  slots[1].appendClear(makeClear(Handle{0x20}));
  slots[2].state = ChunkSlot::State::Pending;
  slots[2].seqId = 3;
  slots[2].appendPresent(makePresent(), Handle{0x2000});

  std::optional<size_t> writingSlot{};
  size_t writeIndex = 0;
  u64 nextSeqId = 4;
  std::deque<size_t> readySlots{0, 1, 2};
  std::deque<u64> completedSeqQueue{};
  std::deque<u64> completedPresentSeqQueue{};
  size_t inflightCount = 3;
  u64 completedSeqId = 0;
  u64 presentCompletedSeqId = 0;
  u64 lastCommittedSeqId = 3;
  bool stop = false;
  std::mutex mutex;
  std::condition_variable writeCv;
  std::condition_variable encodeCv;
  std::condition_variable finishCv;
  std::condition_variable presentCompletedCv;

  metalqueue::QueueLifecycleController lifecycle;
  lifecycle.bindTrackedSubmissionState(metalqueue::QueueLifecycleController::SubmissionBinding{
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

  std::unique_lock lock(mutex);
  size_t encodeCalls = 0;
  const auto inlineEncode =
      [&](size_t slotIndex, ChunkSlot& slot)
          -> std::optional<metalqueue::QueueSubmissionRecord> {
    ++encodeCalls;
    if (encodeCalls == 1) {
      checkEq(slotIndex, size_t{0}, "first encode starts at slot 0");
      checkEq(slot.seqId, u64{2}, "coalesced non-present group uses tail seq");
      checkEq(slot.commandCount(), size_t{2},
              "two non-present commands should coalesce");
      checkEq(slot.commandAt(0).kind, MetalCommandKind::Clear,
              "first coalesced command preserved");
      checkEq(slot.commandAt(1).kind, MetalCommandKind::Clear,
              "second coalesced command preserved");
    } else if (encodeCalls == 2) {
      checkEq(slotIndex, size_t{2}, "present slot remains separate");
      checkEq(slot.seqId, u64{3}, "present slot keeps its own seq");
      checkEq(slot.commandCount(), size_t{1},
              "present slot must not coalesce");
      checkEq(slot.commandAt(0).kind, MetalCommandKind::Present,
              "present command preserved");
    } else {
      fail("unexpected encode call");
    }
    return std::nullopt;
  };

  check(lifecycle.runEncodeIteration(lock, inlineEncode),
        "first encode iteration should run");
  checkEq(readySlots.size(), size_t{1}, "present slot stays ready");
  checkEq(readySlots.front(), size_t{2}, "present slot remains at queue head");
  checkEq(completedSeqQueue.size(), size_t{2},
          "coalesced inline completion emits both seq IDs");
  drainOne(lifecycle, lock, 1);
  drainOne(lifecycle, lock, 2);
  checkEq(completedSeqId, u64{2}, "coalesced seqs drained");
  checkEq(inflightCount, size_t{1}, "two coalesced slots left inflight");

  check(lifecycle.runEncodeIteration(lock, inlineEncode),
        "present encode iteration should run");
  checkEq(readySlots.size(), size_t{0}, "ready queue drained");
  checkEq(completedSeqQueue.size(), size_t{1},
          "present inline completion emits one seq ID");
  drainOne(lifecycle, lock, 3);
  checkEq(completedSeqId, u64{3}, "present seq drained");
  checkEq(inflightCount, size_t{0}, "all slots retired");
}

void testPendingCompletionPushesCoalescedSeqsBeforeFinishReclaim() {
  std::array<ChunkSlot, 2> slots{};
  slots[0].state = ChunkSlot::State::GPU;
  slots[0].seqId = 1;
  slots[0].appendClear(makeClear(Handle{0x30}));
  slots[1].state = ChunkSlot::State::GPU;
  slots[1].seqId = 2;
  slots[1].appendClear(makeClear(Handle{0x40}));

  std::optional<size_t> writingSlot{};
  size_t writeIndex = 0;
  u64 nextSeqId = 3;
  std::deque<size_t> readySlots{};
  std::deque<u64> completedSeqQueue{};
  std::deque<u64> completedPresentSeqQueue{};
  size_t inflightCount = 2;
  u64 completedSeqId = 0;
  u64 presentCompletedSeqId = 0;
  u64 lastCommittedSeqId = 2;
  bool stop = false;
  std::mutex mutex;
  std::condition_variable writeCv;
  std::condition_variable encodeCv;
  std::condition_variable finishCv;
  std::condition_variable presentCompletedCv;

  metalqueue::QueueLifecycleController lifecycle;
  lifecycle.bindTrackedSubmissionState(metalqueue::QueueLifecycleController::SubmissionBinding{
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

  metalqueue::QueueLifecycleController::PendingCompletion pending;
  pending.slotIndex = 0;
  pending.seqId = 2;
  pending.coalescedSlotIndices = {0, 1};
  pending.coalescedSeqIds = {1, 2};
  pending.diagnostics.hasDraw = true;
  pending.contextValue = "queue-coalesced-test";

  metalqueue::QueueLifecycleControllerTestPeer::enqueuePendingCompletion(
      lifecycle, std::move(pending));
  check(lifecycle.processOnePendingCompletion(stop),
        "pending completion should process");
  checkEq(completedSeqQueue.size(), size_t{2},
          "coalesced async completion emits both seq IDs");
  checkEq(completedSeqQueue[0], u64{1},
          "first coalesced async seq emitted first");
  checkEq(completedSeqQueue[1], u64{2},
          "second coalesced async seq emitted second");
  checkEq(slots[0].state, ChunkSlot::State::GPU,
          "pending completion does not reclaim first slot");
  checkEq(slots[1].state, ChunkSlot::State::GPU,
          "pending completion does not reclaim second slot");

  std::unique_lock lock(mutex);
  std::vector<u64> finishedSeqs;
  check(lifecycle.runFinishIteration(lock, [&](u64 seqId) {
          finishedSeqs.push_back(seqId);
        }),
        "first async coalesced seq should finish");
  check(lifecycle.runFinishIteration(lock, [&](u64 seqId) {
          finishedSeqs.push_back(seqId);
        }),
        "second async coalesced seq should finish");
  checkEq(finishedSeqs.size(), size_t{2}, "two async seqs finished");
  checkEq(finishedSeqs[0], u64{1}, "first async seq finishes first");
  checkEq(finishedSeqs[1], u64{2}, "second async seq finishes second");
  checkEq(completedSeqId, u64{2}, "async coalesced seqs drained");
  checkEq(inflightCount, size_t{0}, "async coalesced slots retired");
  checkEq(slots[0].state, ChunkSlot::State::Free,
          "first async coalesced slot reclaimed");
  checkEq(slots[1].state, ChunkSlot::State::Free,
          "second async coalesced slot reclaimed");
}

}  // namespace

int main() {
  try {
    testCoalescesNonPresentReadySlotsBeforePresent();
    testPendingCompletionPushesCoalescedSeqsBeforeFinishReclaim();
  } catch (const TestFailure& failure) {
    std::cerr << "queue_coalescing_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "queue_coalescing_spec unexpected exception: " << ex.what()
              << '\n';
    return 1;
  }

  std::cout << "queue_coalescing_spec passed\n";
  return 0;
}
