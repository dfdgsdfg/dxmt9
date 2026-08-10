#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include "device_c_chunk_registry.hpp"
#include "device_c_chunk_validate.hpp"
#include "dxmt9/core.hpp"

struct D9CDevice;     // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CBuffer;     // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CSwapChain;  // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CTexture;    // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CSurface;    // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CShader;     // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CVertexDecl; // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CQuery;      // fwd (global-namespace struct; see device_c_common.hpp)
struct D9CStateBlock; // fwd (global-namespace struct; see device_c_common.hpp)

namespace dxmt9::d3d9 {

// Wrapper pointer retained across the canonical offload queue boundary. `kind` is a
// D9C_CHUNK_HANDLE_KIND_* value; Query wrappers participate in the same
// validated wire handle table.
struct RetainedWireHandle {
  uint32_t kind = 0;
  void* ptr = nullptr;
};

using ReplaySeq = std::uint64_t;
using RawOrdinal = ReplaySeq;

// One decision covers the complete immutable raw canonical chunk. Records are never
// split between lanes, and RawCommandChunk::replaySeq is the rawOrdinal used by
// this decision.
enum class ReplayLane : std::uint8_t {
  DirectArenaCandidate,
  StateOnly,
  Legacy,  // Existing ReplayOffloadWorker path, including paced Present.
  Inline,  // Synchronous-read boundary; currently Readback only.
  Reject,
};

enum class ReplayReason : std::uint8_t {
  Eligible,
  Query,
  Readback,
  UpdateTexture,
  Present,
  TriangleFan,
  UnknownRecord,
  InvalidImportedView,
  StructuralOverflow,
  Oversize,
};

struct ReplayDrainTarget {
  ReplaySeq lastQueuedSeq = 0;
  ReplaySeq lastReplayedSeq = 0;
};

enum class ReplayDrainResult : std::uint8_t {
  CaughtUp,
  Stopped,
  Poisoned,
};

enum class ReplayTerminalState : std::uint8_t {
  Running,
  Stopped,
  Failed,
};

// Bridge functions expose several C return shapes. A terminal replay worker
// maps HRESULT-bearing calls to DEVICELOST, value getters to zero, and object
// factories/getters to null without entering the provider.
struct ReplayDrainFailure {
  operator std::int32_t() const noexcept {
    return dxmt9::core::D3DERR_DEVICELOST;
  }
  operator std::uint32_t() const noexcept { return 0u; }
  template <typename T>
  operator T*() const noexcept {
    return nullptr;
  }
};

struct RawCommandChunk;

class ReplayDrainLedger {
 public:
  std::shared_ptr<ReplayDrainTarget> targetForCoreBuffer(
      std::uint64_t handleValue);
  bool publishInline(RawCommandChunk& chunk) noexcept;
  void publishReplayed(const RawCommandChunk& chunk) noexcept;
  ReplayDrainResult wait(ReplayDrainTarget& target) noexcept;
  bool pending(const ReplayDrainTarget& target) const noexcept;
  void stop() noexcept;
  void publishFailure() noexcept;
  void poison() noexcept;
  ReplayTerminalState terminalState() const noexcept;
  bool terminal() const noexcept;
  bool stopped() const noexcept;
  bool poisoned() const noexcept;

 private:
  friend class ReplayOffloadQueue;
  friend struct ReplayDrainLedgerTestAccess;

  void publishAcceptedLocked(RawCommandChunk& chunk) noexcept;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  ReplaySeq nextSeq_ = 1;
  bool accepting_ = true;
  bool stopping_ = false;
  bool poisoned_ = false;
  std::atomic<ReplayTerminalState> terminalState_{
      ReplayTerminalState::Running};
  std::unordered_map<std::uint64_t, std::weak_ptr<ReplayDrainTarget>>
      bufferTargets_;
  std::size_t nextBufferTargetSweep_ = 64u;
};

struct RawCommandChunk {
  std::vector<dxmt9::core::u8> recordBlob;
  uint32_t wireVersion = D9C_COMMAND_CHUNK_VERSION;
  uint32_t recordCount = 0;
  uint32_t recordBytes = 0;
  uint32_t handleCount = 0;
  bool preflightValidated = false;
  bool hasPresent = false;
  // canonical objects are resolved and retained synchronously before enqueue.
  // The worker consumes these pointers directly and never looks a stable
  // registry ID up after the app-thread commit returns.
  std::vector<void*> resolvedObjects;
  std::vector<RetainedWireHandle> retainedWrappers;
  std::vector<ReplayDrainTarget*> ledgerTargets;
  // Validated, deduplicated unix-side resource identities. The app thread
  // persists these before handoff. Legacy admission may have already stamped
  // the current queue sequence; Direct publication stamps its exact reserved
  // ticket sequence after strict admission.
  std::vector<dxmt9::core::ChunkHandleEntry> resourceEntries;
  std::vector<dxmt9::core::ChunkBufferBindingSnapshot> bufferSnapshots;
  // Raw FIFO identity. For canonical planning/admission, replaySeq == rawOrdinal.
  ReplaySeq replaySeq = 0;
  bool bufferSnapshotsCaptured = false;
  // Admission-time cutover decision. The worker never re-reads the runtime
  // gate: false preserves the historical synchronous combined mark/capture
  // plus direct Legacy replay; true permits structural lane planning.
  bool cpuReadyTapePlanningEnabled = false;
  bool resourcesMarkedBeforeReplay = false;
};

class ReplayBufferSnapshotResolver {
 public:
  enum class BindingClass : std::uint8_t {
    Missing,
    Live,
    Captured,
  };

  explicit ReplayBufferSnapshotResolver(
      std::span<const dxmt9::core::ChunkBufferBindingSnapshot> entries)
      : entries_(entries) {}

  bool resolve(
      const std::array<dxmt9::core::Handle, dxmt9::core::kMaxStreams>& streams,
      const std::array<dxmt9::core::u32, dxmt9::core::kMaxStreams>& offsets,
      const std::array<dxmt9::core::u32, dxmt9::core::kMaxStreams>& strides,
      dxmt9::core::Handle indexBuffer,
      dxmt9::core::IndexType indexType,
      bool indexed,
      dxmt9::core::DrawBindingSnapshot& out,
      bool* usedCapturedBacking = nullptr) const noexcept {
    bool initialized = false;
    const auto initialize = [&] {
      if (initialized) {
        return;
      }
      // DrawBindingSnapshot is serialized byte-for-byte. Clear padding only
      // when this draw actually needs a captured backing and will attach the
      // payload; live-only draws never materialize the 832-byte object.
      std::memset(&out, 0, sizeof(out));
      initialized = true;
    };
    for (dxmt9::core::u32 stream = 0;
         stream < dxmt9::core::kMaxStreams; ++stream) {
      if (!streams[stream]) {
        continue;
      }
      const auto* capture = find(streams[stream]);
      if (!capture) {
        return false;
      }
      if (!capture->requiresCapturedBacking) {
        continue;
      }
      if (!capture->snapshot.valid()) {
        return false;
      }
      initialize();
      out.streamMask |= 1u << stream;
      out.streams[stream] = dxmt9::core::DrawStreamBindingSnapshot{
          .buffer = streams[stream],
          .offset = offsets[stream],
          .stride = strides[stream],
          .snapshot = capture->snapshot,
      };
    }
    if (indexed && indexBuffer) {
      const auto* capture = find(indexBuffer);
      if (!capture) {
        return false;
      }
      if (capture->requiresCapturedBacking) {
        if (!capture->snapshot.valid()) {
          return false;
        }
        initialize();
        out.indexBuffer = indexBuffer;
        out.indexType = indexType;
        out.indexSnapshot = capture->snapshot;
        out.indexSnapshotValid = true;
      }
    }
    if (usedCapturedBacking) {
      *usedCapturedBacking = initialized;
    } else if (!initialized) {
      // Preserve the historical value-result API for non-replay callers that
      // do not request the sparse-payload signal.
      std::memset(&out, 0, sizeof(out));
    }
    return true;
  }

  bool hasCapturedBackings() const noexcept {
    return std::any_of(entries_.begin(), entries_.end(),
                       [](const auto& entry) {
                         return entry.requiresCapturedBacking;
                       });
  }

  BindingClass classify(dxmt9::core::Handle handle) const noexcept {
    if (!handle) {
      return BindingClass::Live;
    }
    const auto* capture = find(handle);
    if (!capture) {
      return BindingClass::Missing;
    }
    return capture->requiresCapturedBacking
               ? BindingClass::Captured
               : BindingClass::Live;
  }

 private:
  const dxmt9::core::ChunkBufferBindingSnapshot* find(
      dxmt9::core::Handle handle) const noexcept {
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), handle.value,
        [](const auto& entry, std::uint64_t value) {
          return entry.buffer.value < value;
        });
    return found == entries_.end() || found->buffer != handle
               ? nullptr
               : &*found;
  }

  std::span<const dxmt9::core::ChunkBufferBindingSnapshot> entries_{};
};

bool prepareOffloadChunk(
    std::span<const std::byte> blob,
    const CommandChunkEnvelope& envelope,
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
  bool push(RawCommandChunk&& chunk, ReplayDrainLedger* ledger = nullptr) {
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
    if (ledger) {
      // The only nested order in this subsystem is queue -> ledger. Check
      // terminal state before moving the caller's entry, then publish the
      // owned deque entry before either lock can be released.
      std::unique_lock ledgerLock(ledger->mutex_);
      if (!ledger->accepting_ || ledger->terminal()) {
        return false;
      }
      queuedBytes_ += chunk.recordBytes;
      queue_.push_back(std::move(chunk));
      ledger->publishAcceptedLocked(queue_.back());
    } else {
      queuedBytes_ += chunk.recordBytes;
      queue_.push_back(std::move(chunk));
    }
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
// offload-replayed state in program order. Return false on any ledger/worker
// terminal state so the bridge returns ReplayDrainFailure without entering the
// provider. A null `d`, or a healthy device that never spun up a worker
// (offload disabled, or `d->replayOffload` never constructed), already encodes
// "nothing to drain" -- do not re-check offloadCommitReplayEnabled() here. A
// healthy empty queue is a no-wait return: no timing-counter touch. Cheap on the
// common/off path: one ledger predicate plus one pointer test.
// `site` is a diagnostic tag naming the bridge entry point, used only when the
// drain actually blocks (DXMT9_PERF_DRAIN_FENCE_SITES). It must be a string
// literal with static storage duration -- the sink buckets on pointer identity,
// not on string content, so it costs a pointer compare and never a strcmp.
// Defaulted so a caller that has not been tagged still compiles and is simply
// attributed to "untagged".
bool drainDeferredReplay(D9CDevice* d, const char* site = nullptr);

// Non-blocking fail-stop check for deliberately drain-free provider calls,
// including scene markers and wrapper metadata. This never takes the queue or
// ledger mutex.
bool replayTerminal(D9CDevice* d) noexcept;
bool replayTerminal(D9CBuffer* b) noexcept;
bool replayTerminal(D9CSwapChain* s) noexcept;
bool replayTerminal(D9CTexture* t) noexcept;
bool replayTerminal(D9CSurface* s) noexcept;
bool replayTerminal(D9CShader* s) noexcept;
bool replayTerminal(D9CVertexDecl* d) noexcept;
bool replayTerminal(D9CQuery* q) noexcept;

// Buffer Lock is also a direct bridge call. Resolve its owning device before
// entering the provider so deferred draws cannot leave Lock waiting on a
// sequence that the replay worker has not appended yet.
bool drainDeferredReplay(D9CBuffer* b, const char* site = nullptr);

// dxmt9c_swapchain_present overload: D9CSwapChain is an opaque forward
// declaration in the bridge TUs (they only see the ABI-facing
// dxmt9/device_c.h, not device_c_common.hpp), so this overload -- defined in
// device_c_replay_offload.cpp where the full D9CSwapChain definition is
// visible -- resolves `s->owner` (the backpointer set at swapchain creation,
// see device_c_common.hpp) and forwards to the D9CDevice* overload above.
bool drainDeferredReplay(D9CSwapChain* s, const char* site = nullptr);
// Texture/Surface overloads exist for the same reason the Buffer one does: the
// bridge shims hold only forward declarations of the wrapper structs, so they
// cannot reach `->device` themselves. Null-safe on the wrapper.
bool drainDeferredReplay(D9CTexture* t, const char* site = nullptr);
bool drainDeferredReplay(D9CSurface* s, const char* site = nullptr);
bool drainDeferredReplay(D9CQuery* q, const char* site = nullptr);
bool drainDeferredReplay(D9CStateBlock* sb, const char* site = nullptr);

// Emits one [dxmt9-drain-site] line per blocking entry point at Info level.
// No-op unless DXMT9_PERF_DRAIN_FENCE_SITES is set. `presents` is the
// denominator for the per-present columns.
void logDrainFenceSites(std::uint64_t presents);

// buffer_lock's drain, with the lock's class recorded when — and only when —
// the drain actually blocks. `drainDeferredReplay` can see that a lock blocked
// but not what kind it was, and the kind is what decides how much of the fence
// is recoverable: a READONLY MANAGED lock waits on nothing and could be exempted
// outright, while a DISCARD on a hot per-frame buffer will alias queued draws
// and keep blocking under any narrowing. Same early-outs as the plain overload.
bool drainDeferredReplayForBufferLock(D9CBuffer* b,
                                      std::uint32_t lockFlags);
bool drainDeferredReplayForBufferUnlock(D9CBuffer* b);
bool bufferLockClassBypassesReplay(const D9CBufferDesc& desc,
                                   std::uint32_t lockFlags,
                                   bool dynamicRenameEnabled) noexcept;

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
  using ReplayFn = int32_t (*)(D9CDevice*, RawCommandChunk&);
  using FailurePublishedHook = void (*)(void*);

  // Queue bound: 64 chunks / 8 MiB ~= 2+ frames of GT1 chunks (about
  // 14 chunks/present, ~200 KB/present).
  explicit ReplayOffloadWorker(ReplayFn replay = nullptr,
                               FailurePublishedHook failureHook = nullptr,
                               void* failureHookContext = nullptr)
      : queue_(offloadQueueMaxChunks(), offloadQueueMaxBytes()),
        replay_(replay),
        failureHook_(failureHook),
        failureHookContext_(failureHookContext) {}
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
  D9CDevice* owner_ = nullptr;
  ReplayFn replay_ = nullptr;
  FailurePublishedHook failureHook_ = nullptr;
  void* failureHookContext_ = nullptr;
};

// Replays a prevalidated, resolved canonical chunk. The worker publishes ledger
// completion before releasing retained wrappers so its target pointers remain
// alive through the publication.
int32_t replayRawChunk(D9CDevice* d, RawCommandChunk& chunk);

// Releases every wrapper retained during canonical admission and clears the list.
// Namespace linkage allows both the commit push-failure path and the offload
// worker's fail-stop drain to call it.
void releaseRetainedWrappers(RawCommandChunk& chunk);

}  // namespace dxmt9::d3d9
