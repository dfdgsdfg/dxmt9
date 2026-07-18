#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "device_c_chunk_v2_registry.hpp"
#include "device_c_chunk_v2_validate.hpp"
#include "dxmt9/core.hpp"

struct D9CDevice;     // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CBuffer;     // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CSwapChain;  // fwd (global-namespace struct; see device_c_common.hpp)

namespace dxmt9::d3d9 {

// Wrapper pointer retained across the offload queue boundary (see
// retainWrappersForOffload / releaseRetainedWrappers in
// device_c_chunk_replay.cpp). `kind` is a D9C_CHUNK_HANDLE_KIND_* value;
// Query wrappers participate in the same validated wire handle table.
struct RetainedWireHandle {
  uint32_t kind = 0;
  void* ptr = nullptr;
};

struct RawCommandChunk {
  std::vector<dxmt9::core::u8> recordBlob;
  uint32_t wireVersion = D9C_COMMAND_CHUNK_VERSION;
  uint32_t recordCount = 0;
  uint32_t recordBytes = 0;
  uint32_t handleCount = 0;
  bool preflightValidated = false;
  bool hasPresent = false;
  // Wow64 pointer-decode semantics are carried by a thread_local
  // (g_wow64ClientCallDepth) on the committing app thread. The deferred
  // replay must reproduce that context on the worker or wireValuePtr's
  // final reinterpret_cast fallback treats unregistered 32-bit tokens as
  // raw pointers (garbage vtable -> jump to 0, which wedges Wine's
  // signal handling on a non-Wine thread).
  bool wow64ClientCall = false;
  // V2 objects are resolved and retained synchronously before enqueue.
  // The worker consumes these pointers directly and never looks a stable
  // registry ID up after the app-thread commit returns.
  std::vector<void*> resolvedObjects;
  std::vector<RetainedWireHandle> retainedWrappers;
  std::chrono::steady_clock::time_point bridgeCommitStart{};
};

bool prepareV2OffloadChunk(
    std::span<const std::byte> blob,
    const V2ChunkEnvelope& envelope,
    const WireObjectRegistry& registry,
    WireObjectRegistry::RetainFn retain,
    RawCommandChunk& out) noexcept;

// Perf hooks (defined in device_c_replay_offload.cpp) — invoked only when a
// wait actually occurred, so the uncontended queue paths stay a predicate
// check with no counter traffic.
void notePushBackpressureWait(std::uint64_t nanoseconds);
void noteWorkerIdleWait(std::uint64_t nanoseconds);

// Raw-queue bounds (read-once): DXMT9_OFFLOAD_QUEUE_CHUNKS (default 64) and
// DXMT9_OFFLOAD_QUEUE_BYTES (default 8 MiB) — the backpressure tuning lever
// for the offload scouts.
std::size_t offloadQueueMaxChunks();
std::size_t offloadQueueMaxBytes();

// Single-consumer queue: exactly one ReplayOffloadWorker thread is expected
// to call pop() / markReplayDone(); any number of producer threads may call
// push() concurrently. `inFlight_` is a plain bool (not a counter) because
// this contract guarantees at most one popped-but-not-yet-done chunk at a
// time -- a second concurrent consumer would make waitDrained()'s fence
// meaningless.
class ReplayOffloadQueue {
 public:
  ReplayOffloadQueue(std::size_t maxChunks, std::size_t maxBytes)
      : maxChunks_(maxChunks), maxBytes_(maxBytes) {}

  // No-move-on-failure guarantee: `chunk` is only ever std::move()'d into
  // the internal deque on the success (post-stop-check) path below. If this
  // returns false, `chunk` is left completely untouched (its recordBytes is
  // only read, never consumed, by the wait predicate) -- callers may safely
  // inspect or release it (e.g. releaseRetainedWrappers()) afterwards
  // without any moved-from concerns. This is load-bearing for the
  // commit_chunk offload-queue-stopped path in device_c_chunk_replay.cpp,
  // which retains wrapper refs before calling push() and must release them
  // itself when push() refuses the chunk.
  bool push(RawCommandChunk&& chunk) {
    std::unique_lock lock(mutex_);
    const auto admissible = [&] {
      // Oversized-chunk admission: a chunk bigger than maxBytes_ must still
      // be admitted once the queue is empty, or it could never be pushed
      // and would deadlock the producer forever. The count bound
      // (maxChunks_) still applies unconditionally.
      return stop_ || (queue_.size() < maxChunks_ &&
                       (queue_.empty() ||
                        queuedBytes_ + chunk.recordBytes <= maxBytes_));
    };
    if (!admissible()) {
      // Producer backpressure: counted only when the bound actually blocks.
      const auto waitStart = std::chrono::steady_clock::now();
      spaceCv_.wait(lock, admissible);
      notePushBackpressureWait(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - waitStart).count()));
    }
    if (stop_) return false;  // chunk not touched -- see guarantee above.
    queuedBytes_ += chunk.recordBytes;
    queue_.push_back(std::move(chunk));
    workCv_.notify_one();
    return true;
  }

  bool pop(RawCommandChunk& out) {
    std::unique_lock lock(mutex_);
    if (!stop_ && queue_.empty()) {
      // Worker idle: counted only when the queue is actually empty.
      const auto waitStart = std::chrono::steady_clock::now();
      workCv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      noteWorkerIdleWait(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - waitStart).count()));
    }
    if (queue_.empty()) return false;  // stop_ with empty queue
    out = std::move(queue_.front());
    queue_.pop_front();
    queuedBytes_ -= out.recordBytes;
    inFlight_ = true;
    spaceCv_.notify_all();
    return true;
  }

  void markReplayDone() {
    std::lock_guard lock(mutex_);
    inFlight_ = false;
    drainCv_.notify_all();
  }

  // Stop-aware: once stop() has been called, waitDrained() returns even if
  // the queue is not actually empty / a chunk is still in flight, because a
  // stopped queue's worker will never call markReplayDone() again. Callers
  // that need to distinguish "drained" from "gave up because of stop" must
  // check stopped() / the worker's failed() after this returns.
  void waitDrained() {
    std::unique_lock lock(mutex_);
    drainCv_.wait(lock, [&] { return stop_ || (queue_.empty() && !inFlight_); });
  }

  void stop() {
    std::lock_guard lock(mutex_);
    stop_ = true;
    workCv_.notify_all();
    spaceCv_.notify_all();
    drainCv_.notify_all();
  }

  bool stopped() const {
    std::lock_guard lock(mutex_);
    return stop_;
  }

  std::size_t depth() const {
    std::lock_guard lock(mutex_);
    return queue_.size() + (inFlight_ ? 1 : 0);
  }

 private:
  const std::size_t maxChunks_;
  const std::size_t maxBytes_;
  mutable std::mutex mutex_;
  std::condition_variable workCv_;
  std::condition_variable spaceCv_;
  std::condition_variable drainCv_;
  std::deque<RawCommandChunk> queue_;
  std::size_t queuedBytes_ = 0;
  bool inFlight_ = false;
  bool stop_ = false;
};

// getenv("DXMT9_OFFLOAD_COMMIT_REPLAY"), read once. Defined in
// device_c_replay_offload.cpp. dxmt9_command_queue.cpp has its own
// independent, TU-local resolver of the same env var by design (see that
// file); do not try to share a single definition across both TUs.
bool offloadCommitReplayEnabled();

// Drain-fence prologue for every direct (non-commit_chunk) dxmt9c_device_*
// bridge call: if this device has a live offload worker with a non-empty
// queue, block until it has drained so the direct call observes
// offload-replayed state in program order. A null `d` or a device that
// never spun up a worker (offload disabled, or `d->replayOffload` never
// constructed) already encodes "nothing to drain" -- do not re-check
// offloadCommitReplayEnabled() here. An empty queue (depth() == 0) is also
// a plain no-op return: no counter touch, no wait. Cheap on the common/off
// path: one pointer test plus one mutex-guarded depth() read.
void drainDeferredReplay(D9CDevice* d);

// Buffer Lock is also a direct bridge call. Resolve its owning device before
// entering the provider so deferred draws cannot leave Lock waiting on a
// sequence that the replay worker has not appended yet.
void drainDeferredReplay(D9CBuffer* b);

// dxmt9c_swapchain_present overload: D9CSwapChain is an opaque forward
// declaration in the bridge TUs (they only see the ABI-facing
// dxmt9/device_c.h, not device_c_common.hpp), so this overload -- defined in
// device_c_replay_offload.cpp where the full D9CSwapChain definition is
// visible -- resolves `s->owner` (the backpointer set at swapchain creation,
// see device_c_common.hpp) and forwards to the D9CDevice* overload above.
void drainDeferredReplay(D9CSwapChain* s);

// Device-owned background thread that drains a ReplayOffloadQueue by
// calling replayRawChunk() for each popped chunk. Fail-stop: a replay
// failure sets failed_, DXMT_ASSERTs (debug abort), stops the queue so
// every subsequent commit_chunk on this device short-circuits with
// commitChunkFail("offload-worker-failed") instead of silently dropping
// work, and (release-build safety net, since DXMT_ASSERT does not abort
// outside debug builds) calls dxmt9::Device::abortPresentOrdinalWaits() so
// an app thread already parked in waitPresentOrdinalBoundary on an ordinal
// this now-dead worker can never retire does not hang forever.
//
// Queue drain: any chunks still sitting in the queue when run()'s pop loop
// exits (via the fail-stop break, or -- defensively -- a race with an
// external stop()) are drained and have releaseRetainedWrappers() called on
// them without ever being replayed, once in run()'s epilogue and again
// (normally a no-op) after join() in stop(). Both drains are single-threaded
// by construction: the run()-epilogue drain still runs on the worker thread
// itself before it exits, and the stop()-epilogue drain only runs after
// thread_.join() has returned, i.e. after the worker thread has already
// fully exited -- so neither can race a live pop()/markReplayDone() caller.
class ReplayOffloadWorker {
 public:
  // Queue bound: 64 chunks / 8 MiB ~= 2+ frames of GT1 chunks (about
  // 14 chunks/present, ~200 KB/present).
  ReplayOffloadWorker() : queue_(offloadQueueMaxChunks(), offloadQueueMaxBytes()) {}
  ~ReplayOffloadWorker() { stop(); }

  ReplayOffloadWorker(const ReplayOffloadWorker&) = delete;
  ReplayOffloadWorker& operator=(const ReplayOffloadWorker&) = delete;

  void start(D9CDevice* device);   // spawns thread_ running run(device)
  void stop();                     // queue_.stop(); join; drain; idempotent
  ReplayOffloadQueue& queue() { return queue_; }
  bool failed() const { return failed_.load(std::memory_order_acquire); }

 private:
  void run(D9CDevice* device);     // pop loop -> replayRawChunk -> markReplayDone, then drain
  ReplayOffloadQueue queue_;
  std::thread thread_;
  std::atomic<bool> failed_{false};
};

// Rebuilds an ImportedWireChunkView over chunk.recordBlob, replays it via
// the same file-local machinery dxmt9c_device_commit_chunk uses, and
// releases chunk.retainedWrappers (success or failure). Implemented in
// device_c_chunk_replay.cpp because it needs that TU's file-local
// replayImportedChunk / makeImportedWireChunkBlobView / commitChunkFail.
int32_t replayRawChunk(D9CDevice* d, RawCommandChunk& chunk);

// Releases every wrapper addref'd by retainWrappersForOffload() for this
// chunk (dxmt9c_texture_release / dxmt9c_surface_release / etc., dispatched
// by RetainedWireHandle::kind) and clears retainedWrappers. Given
// namespace-level (not file-local) linkage -- unlike its sibling
// retainWrappersForOffload(), which never leaves device_c_chunk_replay.cpp
// -- specifically so it can be called from device_c_replay_offload.cpp's
// ReplayOffloadWorker: both the commit-branch push() failure path
// (device_c_chunk_replay.cpp) and the worker's fail-stop/teardown drain
// (device_c_replay_offload.cpp) need to release a chunk's wrappers without
// ever having replayed it. Defined in device_c_chunk_replay.cpp.
void releaseRetainedWrappers(RawCommandChunk& chunk);

}  // namespace dxmt9::d3d9
