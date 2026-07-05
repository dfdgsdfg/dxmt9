// Pure-data spec for dxmt9::d3d9::ReplayOffloadQueue — bounded FIFO with a
// drain fence used by the commit-replay offload path.
#include "../../../src/d3d9/device_c_replay_offload.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };
void check(bool c, std::string_view m) { if (!c) throw TestFailure(std::string(m)); }

dxmt9::d3d9::RawCommandChunk makeChunk(uint32_t bytes) {
  dxmt9::d3d9::RawCommandChunk c;
  c.recordBlob.resize(bytes);
  c.recordBytes = bytes;
  c.recordCount = 1;
  return c;
}

void testFifoPushPop() {
  dxmt9::d3d9::ReplayOffloadQueue q(4, 1 << 20);
  check(q.push(makeChunk(16)), "push 1");
  check(q.push(makeChunk(32)), "push 2");
  dxmt9::d3d9::RawCommandChunk out;
  check(q.pop(out) && out.recordBytes == 16, "fifo order 1");
  q.markReplayDone();
  check(q.pop(out) && out.recordBytes == 32, "fifo order 2");
  q.markReplayDone();
  check(q.depth() == 0, "drained depth");
}

void testDrainWaitsForInFlight() {
  dxmt9::d3d9::ReplayOffloadQueue q(4, 1 << 20);
  check(q.push(makeChunk(8)), "push");
  dxmt9::d3d9::RawCommandChunk out;
  check(q.pop(out), "pop");
  bool drained = false;
  std::thread waiter([&] { q.waitDrained(); drained = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!drained, "drain must wait for in-flight chunk");
  q.markReplayDone();
  waiter.join();
  check(drained, "drain released after markReplayDone");
}

void testBoundedPushBlocksUntilPop() {
  dxmt9::d3d9::ReplayOffloadQueue q(1, 1 << 20);
  check(q.push(makeChunk(8)), "push fills bound");
  bool pushed = false;
  std::thread producer([&] { pushed = q.push(makeChunk(8)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!pushed, "push blocks while full");
  dxmt9::d3d9::RawCommandChunk out;
  check(q.pop(out), "pop frees slot");
  q.markReplayDone();
  producer.join();
  check(pushed, "blocked push completes");
}

void testStopReleasesEverything() {
  dxmt9::d3d9::ReplayOffloadQueue q(1, 1 << 20);
  std::thread popper([&] {
    dxmt9::d3d9::RawCommandChunk out;
    check(!q.pop(out), "pop returns false after stop with empty queue");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  q.stop();
  popper.join();
  check(!q.push(makeChunk(8)), "push refused after stop");
}

// waitDrained() must be stop-aware: once stop() has been called, a waiter
// must be released even if a chunk is still (permanently) in flight,
// because a stopped queue's worker is gone and will never call
// markReplayDone(). Without this, a clean-shutdown waitDrained() call
// racing a worker fail-stop would block forever.
void testWaitDrainedIsStopAware() {
  dxmt9::d3d9::ReplayOffloadQueue q(4, 1 << 20);
  check(q.push(makeChunk(8)), "push");
  dxmt9::d3d9::RawCommandChunk out;
  check(q.pop(out), "pop leaves one chunk in flight");
  std::atomic<bool> drained{false};
  std::thread waiter([&] {
    q.waitDrained();
    drained.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!drained.load(), "drain must wait while a chunk is in flight and not stopped");
  q.stop();  // Note: markReplayDone() is deliberately never called.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const bool releasedByStop = drained.load();
  waiter.detach();  // detach before check() so a failing check can't leak a
                    // joinable std::thread into std::terminate on unwind.
  check(releasedByStop,
        "waitDrained must return once stop() is called even with an "
        "undrained in-flight chunk");
}

// Oversized-chunk admission: a single chunk whose recordBytes exceeds the
// queue's byte bound must still be admitted when the queue is empty. The
// count bound (maxChunks) still applies; only the byte bound gets this
// escape hatch, otherwise a chunk larger than maxBytes could never be
// pushed and would deadlock the producer forever.
void testOversizedChunkAdmittedWhenQueueEmpty() {
  dxmt9::d3d9::ReplayOffloadQueue q(4, 16);  // byte bound well under one chunk.
  std::atomic<bool> pushed{false};
  std::thread pusher([&] { pushed.store(q.push(makeChunk(64))); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const bool admitted = pushed.load();
  if (!admitted) {
    pusher.detach();  // avoid std::terminate from an unjoined thread below.
  } else {
    pusher.join();
  }
  check(admitted,
        "an oversized chunk must be admitted immediately when the queue is "
        "empty, not blocked forever on the byte bound");
  dxmt9::d3d9::RawCommandChunk out;
  check(q.pop(out) && out.recordBytes == 64, "oversized chunk pops back out intact");
  q.markReplayDone();
}

// pop() must keep returning already-queued items after stop() -- it only
// returns false once stopped *and* empty. This is what lets a worker's
// fail-stop drain epilogue (device_c_replay_offload.cpp) release wrapper
// retention for every chunk left in the queue instead of leaking whatever
// was still enqueued when the failure happened.
void testPopDrainsQueuedItemsAfterStop() {
  dxmt9::d3d9::ReplayOffloadQueue q(4, 1 << 20);
  check(q.push(makeChunk(8)), "push 1");
  check(q.push(makeChunk(16)), "push 2");
  q.stop();
  dxmt9::d3d9::RawCommandChunk out;
  check(q.pop(out) && out.recordBytes == 8, "pop after stop returns first queued item");
  q.markReplayDone();
  check(q.pop(out) && out.recordBytes == 16, "pop after stop returns second queued item");
  q.markReplayDone();
  check(!q.pop(out), "pop returns false once stopped and empty");
  check(q.depth() == 0, "depth reaches 0 after both markReplayDone calls");
}

// push()'s no-move-on-failure guarantee (see the doc comment on push() in
// device_c_replay_offload.hpp): on the stop_ early-return path, `chunk` must
// not have been consumed by std::move, so the caller can still release any
// wrappers it had already retained into the chunk before calling push().
void testPushDoesNotConsumeChunkOnStopFailure() {
  dxmt9::d3d9::ReplayOffloadQueue q(4, 1 << 20);
  q.stop();
  dxmt9::d3d9::RawCommandChunk chunk = makeChunk(8);
  chunk.recordBlob.assign({1, 2, 3, 4, 5, 6, 7, 8});
  check(!q.push(std::move(chunk)), "push refused once the queue is stopped");
  check(chunk.recordBytes == 8, "recordBytes must survive a refused push untouched");
  check(chunk.recordBlob.size() == 8, "recordBlob size must survive a refused push untouched");
  check(chunk.recordBlob ==
            std::vector<dxmt9::core::u8>({1, 2, 3, 4, 5, 6, 7, 8}),
        "recordBlob contents must survive a refused push untouched");
}
}  // namespace

int main() {
  try {
    testFifoPushPop();
    testDrainWaitsForInFlight();
    testBoundedPushBlocksUntilPop();
    testStopReleasesEverything();
    testWaitDrainedIsStopAware();
    testOversizedChunkAdmittedWhenQueueEmpty();
    testPopDrainsQueuedItemsAfterStop();
    testPushDoesNotConsumeChunkOnStopFailure();
  } catch (const TestFailure& e) {
    std::cerr << "replay_offload_queue_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "replay_offload_queue_spec passed\n";
  return 0;
}
