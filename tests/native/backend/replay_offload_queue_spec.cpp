// Pure-data spec for dxmt9::d3d9::ReplayOffloadQueue — bounded FIFO with a
// drain fence used by the commit-replay offload path.
#include "../../../src/d3d9/device_c_replay_offload.hpp"

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
}  // namespace

int main() {
  try {
    testFifoPushPop();
    testDrainWaitsForInFlight();
    testBoundedPushBlocksUntilPop();
    testStopReleasesEverything();
  } catch (const TestFailure& e) {
    std::cerr << "replay_offload_queue_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "replay_offload_queue_spec passed\n";
  return 0;
}
