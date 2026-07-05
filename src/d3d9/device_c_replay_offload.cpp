// ReplayOffloadWorker: device-owned background thread draining the Task 2
// ReplayOffloadQueue. Kept in its own TU so device_c_common.hpp only needs a
// forward declaration of ReplayOffloadWorker for the D9CDevice member (the
// full definition here, plus D9CDevice's own destructor defined out-of-line
// in device_c_state.cpp, is what actually needs ReplayOffloadWorker
// complete).

#include "device_c_replay_offload.hpp"

#include "device_c_common.hpp"
// Need the full dxmt9::Device type to call abortPresentOrdinalWaits() on
// the upperDevice shared_ptr from the fail-stop path below (mirrors the
// same include-for-the-same-reason comment in device_c_chunk_replay.cpp).
#include "dxmt9/dxmt9_device.hpp"

#include "dxmt9/assert.hpp"

#include <cstdlib>

namespace dxmt9::d3d9 {

bool offloadCommitReplayEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_OFFLOAD_COMMIT_REPLAY");
    return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

void ReplayOffloadWorker::start(D9CDevice* device) {
  thread_ = std::thread([this, device] { run(device); });
}

void ReplayOffloadWorker::stop() {
  queue_.stop();
  if (thread_.joinable()) {
    thread_.join();
  }
  // Defensive drain: by the time join() returns, run() has already fully
  // exited (including its own epilogue drain below), so this is
  // single-threaded and normally finds nothing left. Kept here in case a
  // future change manages to leave a chunk queued across a stop() that
  // did not go through the fail-stop path.
  RawCommandChunk drained;
  while (queue_.pop(drained)) {
    releaseRetainedWrappers(drained);
    queue_.markReplayDone();
  }
}

void ReplayOffloadWorker::run(D9CDevice* device) {
  RawCommandChunk chunk;
  while (queue_.pop(chunk)) {
    const int32_t hr = replayRawChunk(device, chunk);
    queue_.markReplayDone();
    if (hr < 0) {
      failed_.store(true, std::memory_order_release);
      DXMT_ASSERT(false && "deferred commit replay failed");
      queue_.stop();
      // Release-build safety net: DXMT_ASSERT above does not abort outside
      // debug builds, so without this an app thread already parked in
      // CommandQueue::waitPresentOrdinalBoundary on an ordinal this now-dead
      // worker can never retire would hang forever. abortPresentOrdinalWaits()
      // wakes any such waiter so it observes the abort and returns instead.
      if (auto upper = device->dev().upperDevice()) {
        upper->abortPresentOrdinalWaits();
      }
      break;
    }
  }
  // Drain epilogue: release wrapper retention for any chunks left queued
  // when the loop above exited. Reached either via the fail-stop `break`
  // (the remaining queued chunks were never popped/replayed, so their
  // retained wrappers would otherwise leak) or via a normal stop()-with-
  // empty-queue exit (a no-op drain, since ReplayOffloadQueue::pop() already
  // returns queued items after stop() -- only returning false once stopped
  // *and* empty -- so the loop above already replayed everything in that
  // case). Single-threaded: this is still the worker thread itself, and no
  // other thread calls pop()/markReplayDone() on this queue.
  RawCommandChunk drained;
  while (queue_.pop(drained)) {
    releaseRetainedWrappers(drained);
    queue_.markReplayDone();
  }
}

}  // namespace dxmt9::d3d9
