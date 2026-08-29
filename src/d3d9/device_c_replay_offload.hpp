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
#include "dxmt9/dxmt9_managed_mutation_lease.hpp"

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
struct BufferMutationTask;
struct StateBlockApplyTask;

class ReplayDrainLedger {
 public:
  std::shared_ptr<ReplayDrainTarget> targetForCoreBuffer(
      std::uint64_t handleValue);
  bool publishInline(RawCommandChunk& chunk) noexcept;
  void publishReplayed(const RawCommandChunk& chunk) noexcept;
  // R-BACK-44.5 — the mutation half of the same publication, so the existing
  // resource-scoped fence (R-BACK-2.51(d)(i)) covers pending mutations with no
  // change at all on the reader side.
  void publishMutationReplayed(const BufferMutationTask& task) noexcept;
  void publishStateBlockApplyAcceptedLocked(StateBlockApplyTask& task) noexcept;
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
  // R-BACK-44.2 step 3. Unlike the chunk publication this takes `max`, not a
  // plain assignment: the ordinal was fixed at reserve, and a chunk pushed
  // between reserve and commit may already have published a HIGHER queued
  // watermark against the same buffer. Overwriting it with the older ordinal
  // would tell a direct reader it was caught up while that chunk is still
  // queued.
  void publishMutationAcceptedLocked(BufferMutationTask& task) noexcept;

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
  // Zero in the normal renderer. A non-zero pair binds this raw FIFO item to
  // the exact PE Render Tape CommandChunk event that was accepted only after
  // this bridge call returned successfully.
  std::uint64_t renderTapeCaptureToken = 0;
  std::uint64_t renderTapeEventOrdinal = 0;
  bool bufferSnapshotsCaptured = false;
  // Admission-time cutover decision. The worker never re-reads the runtime
  // gate: false preserves the historical synchronous combined mark/capture
  // plus direct Legacy replay; true permits structural lane planning.
  bool cpuReadyTapePlanningEnabled = false;
  bool resourcesMarkedBeforeReplay = false;
};

// R-BACK-44.2a — one Managed writable unlock whose byte materialization was
// handed to the offload worker. It is the second alternative of the FIFO queue
// element; a chunk and a mutation are ordered against each other by nothing
// more than their position in the one deque, which is what keeps R-BACK-44.3's
// ordering argument to a single sentence.
//
// Everything the worker needs is a VALUE here, resolved on the producer thread
// before the unlock returned. Nothing is looked up again at apply time except
// the pool record itself, which the `coreBuffer` retention keeps alive.
struct BufferMutationTask {
  // R-BACK-44.2a retention. Holding the core buffer is what makes
  // `Buffer::invalidate()` -> `destroyBuffer` unreachable while the task is
  // queued, so the pool record cannot become `destroyPending` and `gcArena`
  // cannot reach it — the same pin obligation a chunk discharges by retaining
  // its wrappers.
  std::shared_ptr<dxmt9::core::Buffer> coreBuffer;
  // Owned rather than borrowed: the D9CBuffer wrapper may be released between
  // commit and apply, and the worker still has to publish completion against
  // this target (R-BACK-44.5).
  std::shared_ptr<ReplayDrainTarget> ledgerTarget;
  std::uint64_t bufferHandle = 0;
  dxmt9::resources::ManagedBufferMutationLease lease;
  std::vector<std::uint8_t> stagedBytes;
  std::uint64_t lockedOffset = 0;
  // The reserved FIFO ordinal, fixed at reserve (R-BACK-44.2 step 1) and
  // published against `ledgerTarget` at commit (step 3).
  ReplaySeq replaySeq = 0;
};

struct StateBlockApplyTask {
  std::shared_ptr<dxmt9::core::StateBlock> stateBlock;
  ReplaySeq replaySeq = 0;
};

// The FIFO element. Exactly one of four states:
//   * placeholder (`reservationId != 0`)      — reserved, not yet committed;
//   * mutation    (`mutation != nullptr`)     — a committed BufferMutationTask;
//   * state block (`stateBlockApply != nullptr`) — a StateBlockApplyTask;
//   * chunk       (everything else)           — a RawCommandChunk.
// The mutation payload is behind a `unique_ptr` so a chunk-only queue keeps its
// element size, and so `commitMutation` is a pointer move — i.e. noexcept,
// which is what makes R-BACK-44.2 step 3 infallible.
struct ReplayQueueItem {
  RawCommandChunk chunk;
  std::unique_ptr<BufferMutationTask> mutation;
  std::unique_ptr<StateBlockApplyTask> stateBlockApply;
  std::uint64_t reservationId = 0;
  std::size_t chargedBytes = 0;

  bool placeholder() const noexcept { return reservationId != 0u; }
  bool isMutation() const noexcept { return mutation != nullptr; }
  bool isStateBlockApply() const noexcept { return stateBlockApply != nullptr; }
};

// Handle returned by `ReplayOffloadQueue::reserveMutation`. `replaySeq` is the
// FIFO ordinal the mutation will carry; it is allocated from the ledger under
// BOTH the queue and ledger mutexes so a concurrent chunk push — which takes
// the same two locks in the same order — can never be assigned an ordinal
// between this reservation and the queue position it already occupies.
struct ReplayQueueMutationReservation {
  std::uint64_t id = 0;
  ReplaySeq replaySeq = 0;

  bool valid() const noexcept { return id != 0u; }
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
bool replayOffloadObservabilityEnabled() noexcept;
void notePushWaitEnter();
void notePushWaitExit();
void noteReplayInflightRaw(bool inFlight);

// Raw-queue bounds (read-once): DXMT9_OFFLOAD_QUEUE_CHUNKS (default 64) and
// DXMT9_OFFLOAD_QUEUE_BYTES (default 8 MiB) — the backpressure tuning lever
// for the offload scouts.
std::size_t offloadQueueMaxChunks();
std::size_t offloadQueueMaxBytes();

enum class ReplayQueuePushDisposition : std::uint8_t {
  Accepted,
  RejectedPreEffect,
  EffectUnknown,
};

enum class ReplayQueueFailurePoint : std::uint8_t {
  None,
  BeforeAdoption,
  AfterAdoption,
};

// Host-test allocator control for exercising the real std::deque growth
// failure path. Production queues pass no control and use the same allocator
// as a zero-branch forwarding wrapper around std::allocator.
struct ReplayQueueAllocationFailure {
  std::atomic<bool> failNext{false};
};

template <typename T>
class ReplayQueueAllocator {
 public:
  using value_type = T;

  ReplayQueueAllocator() noexcept = default;
  explicit ReplayQueueAllocator(ReplayQueueAllocationFailure* failure) noexcept
      : failure_(failure) {}
  template <typename U>
  ReplayQueueAllocator(const ReplayQueueAllocator<U>& other) noexcept
      : failure_(other.failure_) {}

  T* allocate(std::size_t count) {
    if (failure_ && failure_->failNext.exchange(false)) {
      throw std::bad_alloc();
    }
    return std::allocator<T>{}.allocate(count);
  }
  void deallocate(T* value, std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(value, count);
  }

  template <typename U>
  bool operator==(const ReplayQueueAllocator<U>& other) const noexcept {
    return failure_ == other.failure_;
  }

 private:
  template <typename>
  friend class ReplayQueueAllocator;
  ReplayQueueAllocationFailure* failure_ = nullptr;
};

// Single-consumer queue: exactly one ReplayOffloadWorker thread is expected
// to call pop() / markReplayDone(); any number of producer threads may call
// push() concurrently. `inFlight_` is a plain bool (not a counter) because
// this contract guarantees at most one popped-but-not-yet-done chunk at a
// time -- a second concurrent consumer would make waitDrained()'s fence
// meaningless.
class ReplayOffloadQueue {
 public:
  ReplayOffloadQueue(std::size_t maxChunks, std::size_t maxBytes,
                     ReplayQueueAllocationFailure* allocationFailure = nullptr)
      : maxChunks_(maxChunks), maxBytes_(maxBytes),
        observabilityEnabled_(replayOffloadObservabilityEnabled()),
        queue_(ReplayQueueAllocator<ReplayQueueItem>(allocationFailure)),
        testOnlyAllocationFailure_(allocationFailure) {}

  // RejectedPreEffect preserves caller ownership. Accepted adopts the chunk
  // and publishes its byte count (and optional ledger sequence) exactly once.
  // EffectUnknown means adoption completed but a later bridge-visible effect
  // could not be proven; callers must poison/fail-stop and must not release the
  // moved wrapper refs themselves.
  ReplayQueuePushDisposition pushWithDisposition(
      RawCommandChunk&& chunk, ReplayDrainLedger* ledger = nullptr) noexcept try {
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
      if (observabilityEnabled_) {
        notePushWaitEnter();
      }
      spaceCv_.wait(lock, admissible);
      if (observabilityEnabled_) {
        notePushWaitExit();
      }
      notePushBackpressureWait(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - waitStart).count()));
    }
    if (stop_) return ReplayQueuePushDisposition::RejectedPreEffect;
    if (testOnlyFailurePoint_ == ReplayQueueFailurePoint::BeforeAdoption) {
      testOnlyFailurePoint_ = ReplayQueueFailurePoint::None;
      return ReplayQueuePushDisposition::RejectedPreEffect;
    }
    if (ledger) {
      // The only nested order in this subsystem is queue -> ledger. Check
      // terminal state before moving the caller's entry, then publish the
      // owned deque entry before either lock can be released.
      std::unique_lock ledgerLock(ledger->mutex_);
      if (!ledger->accepting_ || ledger->terminal()) {
        return ReplayQueuePushDisposition::RejectedPreEffect;
      }
      try {
        queue_.emplace_back();
      } catch (...) {
        return ReplayQueuePushDisposition::RejectedPreEffect;
      }
      // Element construction is the only throwing step; the chunk's own move
      // assignment is noexcept, so the caller keeps ownership on any failure.
      queue_.back().chunk = std::move(chunk);
      queue_.back().chargedBytes = queue_.back().chunk.recordBytes;
      queuedBytes_ += queue_.back().chargedBytes;
      ledger->publishAcceptedLocked(queue_.back().chunk);
    } else {
      try {
        queue_.emplace_back();
      } catch (...) {
        return ReplayQueuePushDisposition::RejectedPreEffect;
      }
      queue_.back().chunk = std::move(chunk);
      queue_.back().chargedBytes = queue_.back().chunk.recordBytes;
      queuedBytes_ += queue_.back().chargedBytes;
    }
    if (testOnlyFailurePoint_ == ReplayQueueFailurePoint::AfterAdoption) {
      testOnlyFailurePoint_ = ReplayQueueFailurePoint::None;
      stop_ = true;
      workCv_.notify_all();
      spaceCv_.notify_all();
      drainCv_.notify_all();
      return ReplayQueuePushDisposition::EffectUnknown;
    }
    workCv_.notify_one();
    return ReplayQueuePushDisposition::Accepted;
  } catch (...) {
    // Every potentially throwing operation precedes deque adoption; push_back
    // itself is caught at the adoption site. Later publication/accounting and
    // notifications are noexcept, so an outer failure preserves caller
    // ownership and is explicitly pre-effect.
    return ReplayQueuePushDisposition::RejectedPreEffect;
  }

  bool push(RawCommandChunk&& chunk,
            ReplayDrainLedger* ledger = nullptr) noexcept {
    return pushWithDisposition(std::move(chunk), ledger) ==
           ReplayQueuePushDisposition::Accepted;
  }

  // Appends one immutable StateBlock apply after all items currently in the
  // FIFO. The caller retains ownership on pre-effect rejection.
  ReplayQueuePushDisposition pushStateBlockApply(
      std::unique_ptr<StateBlockApplyTask>& task,
      ReplayDrainLedger* ledger = nullptr) noexcept try {
    if (!task) {
      return ReplayQueuePushDisposition::RejectedPreEffect;
    }
    std::unique_lock lock(mutex_);
    const auto admissible = [&] {
      return stop_ || queue_.size() < maxChunks_;
    };
    if (!admissible()) {
      const auto waitStart = std::chrono::steady_clock::now();
      if (observabilityEnabled_) {
        notePushWaitEnter();
      }
      spaceCv_.wait(lock, admissible);
      if (observabilityEnabled_) {
        notePushWaitExit();
      }
      notePushBackpressureWait(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - waitStart).count()));
    }
    if (stop_) return ReplayQueuePushDisposition::RejectedPreEffect;
    if (testOnlyFailurePoint_ == ReplayQueueFailurePoint::BeforeAdoption) {
      testOnlyFailurePoint_ = ReplayQueueFailurePoint::None;
      return ReplayQueuePushDisposition::RejectedPreEffect;
    }
    if (ledger) {
      std::unique_lock ledgerLock(ledger->mutex_);
      if (!ledger->accepting_ || ledger->terminal()) {
        return ReplayQueuePushDisposition::RejectedPreEffect;
      }
      try {
        queue_.emplace_back();
      } catch (...) {
        return ReplayQueuePushDisposition::RejectedPreEffect;
      }
      queue_.back().stateBlockApply = std::move(task);
      ledger->publishStateBlockApplyAcceptedLocked(
          *queue_.back().stateBlockApply);
    } else {
      try {
        queue_.emplace_back();
      } catch (...) {
        return ReplayQueuePushDisposition::RejectedPreEffect;
      }
      queue_.back().stateBlockApply = std::move(task);
    }
    if (testOnlyFailurePoint_ == ReplayQueueFailurePoint::AfterAdoption) {
      testOnlyFailurePoint_ = ReplayQueueFailurePoint::None;
      stop_ = true;
      workCv_.notify_all();
      spaceCv_.notify_all();
      drainCv_.notify_all();
      return ReplayQueuePushDisposition::EffectUnknown;
    }
    workCv_.notify_one();
    return ReplayQueuePushDisposition::Accepted;
  } catch (...) {
    return ReplayQueuePushDisposition::RejectedPreEffect;
  }

  // R-BACK-44.2's reservation, half one: fix the FIFO ordinal and charge the
  // staged-byte budget with NO externally visible effect. The reservation is a
  // real deque element from this instant, which is the whole scheme — see the
  // `placeholder()` note on `pop` below for why the worker cannot walk past it.
  // Returns an invalid handle on stop/poison or allocation failure; the caller
  // then rejects the unlock pre-effect, with every layer's lock state intact.
  ReplayQueueMutationReservation reserveMutation(
      std::size_t bytes, ReplayDrainLedger& ledger) noexcept try {
    std::unique_lock lock(mutex_);
    const auto admissible = [&] {
      // Same oversize escape hatch the chunk path has: a staged span larger
      // than the byte bound must still be admitted once the queue is empty.
      return stop_ || (queue_.size() < maxChunks_ &&
                       (queue_.empty() || queuedBytes_ + bytes <= maxBytes_));
    };
    if (!admissible()) {
      const auto waitStart = std::chrono::steady_clock::now();
      if (observabilityEnabled_) {
        notePushWaitEnter();
      }
      spaceCv_.wait(lock, admissible);
      if (observabilityEnabled_) {
        notePushWaitExit();
      }
      notePushBackpressureWait(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - waitStart).count()));
    }
    if (stop_) return {};
    ReplaySeq reserved = 0;
    {
      // queue -> ledger, the subsystem's only nested order. Taking the ordinal
      // here — under both locks, with the placeholder already appended — is
      // what makes "a chunk pushed between reserve and commit lands after the
      // mutation" true of the ordinal as well as of the queue position.
      std::unique_lock ledgerLock(ledger.mutex_);
      if (!ledger.accepting_ || ledger.terminal()) {
        return {};
      }
      queue_.emplace_back();
      reserved = ledger.nextSeq_++;
    }
    auto& slot = queue_.back();
    slot.reservationId = nextReservationId_++;
    slot.chargedBytes = bytes;
    queuedBytes_ += bytes;
    return {slot.reservationId, reserved};
  } catch (...) {
    // `emplace_back` is the only throwing step and it precedes every visible
    // effect (ordinal, byte charge), so a failure here is pre-effect.
    return {};
  }

  // R-BACK-44.2 step 3, and infallible by construction: the deque element
  // already exists and the payload is a pointer move. A vanished reservation
  // is only reachable through a teardown drain that cleared the queue after
  // this reservation was taken; the task is then dropped (and counted as
  // discarded by the caller), which is exactly the disposition R-BACK-44.7
  // gives every other pending task on that path.
  bool commitMutation(const ReplayQueueMutationReservation& reservation,
                      std::unique_ptr<BufferMutationTask> task,
                      ReplayDrainLedger& ledger) noexcept {
    if (!reservation.valid() || !task) {
      return false;
    }
    std::unique_lock lock(mutex_);
    const auto found = findReservationLocked(reservation.id);
    if (found == queue_.end()) {
      return false;
    }
    task->replaySeq = reservation.replaySeq;
    found->mutation = std::move(task);
    found->reservationId = 0u;
    {
      std::lock_guard ledgerLock(ledger.mutex_);
      ledger.publishMutationAcceptedLocked(*found->mutation);
    }
    workCv_.notify_one();
    return true;
  }

  // Every abandon path: staging failure, an invalid lease (i.e. no rotation
  // happened), or any other pre-effect rejection.
  void releaseMutation(
      const ReplayQueueMutationReservation& reservation) noexcept {
    if (!reservation.valid()) {
      return;
    }
    std::lock_guard lock(mutex_);
    const auto found = findReservationLocked(reservation.id);
    if (found == queue_.end()) {
      return;
    }
    queuedBytes_ -= found->chargedBytes;
    queue_.erase(found);
    spaceCv_.notify_all();
    // The head may have just become poppable, and the queue may have just
    // become empty.
    workCv_.notify_all();
    drainCv_.notify_all();
  }

  // Single-consumer. Returns false when the queue is empty, or when the head
  // is an UNCOMMITTED RESERVATION: the worker must not run past a reserved
  // ordinal, and it cannot, because the reservation physically occupies that
  // deque position and nothing pops from the middle. The window is producer-
  // side straight-line code between `reserveMutation` and `commitMutation`
  // with no waits in it, so head-blocking here is microseconds.
  bool pop(ReplayQueueItem& out) {
    std::unique_lock lock(mutex_);
    const auto ready = [&] {
      return stop_ || (!queue_.empty() && !queue_.front().placeholder());
    };
    if (!ready()) {
      // Worker idle: counted only when there is nothing it may consume.
      const auto waitStart = std::chrono::steady_clock::now();
      workCv_.wait(lock, ready);
      noteWorkerIdleWait(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - waitStart).count()));
    }
    if (queue_.empty() || queue_.front().placeholder()) {
      return false;  // stop_ with an empty queue or an unresolved reservation
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    queuedBytes_ -= out.chargedBytes;
    inFlight_ = true;
    if (observabilityEnabled_) {
      noteReplayInflightRaw(true);
    }
    spaceCv_.notify_all();
    return true;
  }

  // Teardown drain (R-BACK-44.7): move every committed item out in FIFO order
  // so the caller can release chunk wrapper retention and mutation leases
  // without replaying them. Placeholders own nothing and are simply dropped;
  // their producer's `commitMutation` / `releaseMutation` then no-ops. Never
  // waits, so it cannot deadlock against a producer mid-transaction.
  void drainRemaining(std::vector<ReplayQueueItem>& out) {
    std::lock_guard lock(mutex_);
    for (auto& item : queue_) {
      if (item.placeholder()) {
        continue;
      }
      out.push_back(std::move(item));
    }
    queue_.clear();
    queuedBytes_ = 0u;
    spaceCv_.notify_all();
    drainCv_.notify_all();
  }

  void markReplayDone() {
    std::lock_guard lock(mutex_);
    inFlight_ = false;
    if (observabilityEnabled_) {
      noteReplayInflightRaw(false);
    }
    drainCv_.notify_all();
  }

  // Stop-aware: once stop() has been called, waitDrained() returns even if
  // the queue is not actually empty / a chunk is still in flight, because a
  // stopped queue's worker will never call markReplayDone() again. Callers
  // that need to distinguish "drained" from "gave up because of stop" must
  // check stopped() / the worker's failed() after this returns.
  void waitDrained() {
    std::unique_lock lock(mutex_);
    if (testOnlyDrainWaitObservationEnabled_ &&
        !stop_ && (!queue_.empty() || inFlight_)) {
      ++testOnlyDrainWaitEntries_;
      drainCv_.notify_all();
    }
    drainCv_.wait(lock, [&] { return stop_ || (queue_.empty() && !inFlight_); });
  }

  void enableDrainWaitObservationForTest() {
    std::lock_guard lock(mutex_);
    testOnlyDrainWaitObservationEnabled_ = true;
    testOnlyDrainWaitEntries_ = 0;
  }

  bool waitForDrainWaitEntriesForTest(std::uint64_t expected) {
    std::unique_lock lock(mutex_);
    drainCv_.wait(lock, [&] {
      return testOnlyDrainWaitEntries_ >= expected;
    });
    return true;
  }

  bool waitForDrainedForTest() {
    std::unique_lock lock(mutex_);
    drainCv_.wait(lock, [&] {
      return queue_.empty() && !inFlight_;
    });
    return true;
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

  std::size_t queuedBytesForTest() const {
    std::lock_guard lock(mutex_);
    return queuedBytes_;
  }

  void failNextPushForTest(ReplayQueueFailurePoint point) {
    std::lock_guard lock(mutex_);
    testOnlyFailurePoint_ = point;
  }

  void failNextAllocationForTest() noexcept {
    if (testOnlyAllocationFailure_) {
      testOnlyAllocationFailure_->failNext.store(true);
    }
  }

 private:
  using ItemDeque =
      std::deque<ReplayQueueItem, ReplayQueueAllocator<ReplayQueueItem>>;

  ItemDeque::iterator findReservationLocked(std::uint64_t id) noexcept {
    return std::find_if(queue_.begin(), queue_.end(),
                        [id](const ReplayQueueItem& item) {
                          return item.reservationId == id;
                        });
  }

  const std::size_t maxChunks_;
  const std::size_t maxBytes_;
  const bool observabilityEnabled_;
  mutable std::mutex mutex_;
  std::condition_variable workCv_;
  std::condition_variable spaceCv_;
  std::condition_variable drainCv_;
  ItemDeque queue_;
  std::uint64_t nextReservationId_ = 1;
  std::size_t queuedBytes_ = 0;
  bool inFlight_ = false;
  bool stop_ = false;
  bool testOnlyDrainWaitObservationEnabled_ = false;
  std::uint64_t testOnlyDrainWaitEntries_ = 0;
  ReplayQueueFailurePoint testOnlyFailurePoint_ =
      ReplayQueueFailurePoint::None;
  ReplayQueueAllocationFailure* testOnlyAllocationFailure_ = nullptr;
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
  using ApplyMutationFn = int32_t (*)(D9CDevice*, BufferMutationTask&);
  using ApplyStateBlockFn = int32_t (*)(D9CDevice*, StateBlockApplyTask&);
  using FailurePublishedHook = void (*)(void*);

  // Queue bound: 64 chunks / 8 MiB ~= 2+ frames of GT1 chunks (about
  // 14 chunks/present, ~200 KB/present).
  explicit ReplayOffloadWorker(ReplayFn replay = nullptr,
                               FailurePublishedHook failureHook = nullptr,
                               void* failureHookContext = nullptr,
                               ApplyMutationFn applyMutation = nullptr,
                               ApplyStateBlockFn applyStateBlock = nullptr)
      : queue_(offloadQueueMaxChunks(), offloadQueueMaxBytes()),
        replay_(replay),
        applyMutation_(applyMutation),
        applyStateBlock_(applyStateBlock),
        failureHook_(failureHook),
        failureHookContext_(failureHookContext) {}
  ~ReplayOffloadWorker() { stop(); }

  ReplayOffloadWorker(const ReplayOffloadWorker&) = delete;
  ReplayOffloadWorker& operator=(const ReplayOffloadWorker&) = delete;

  bool start(D9CDevice* device) noexcept;  // rolls owner back if spawn fails
  void stop();                     // queue_.stop(); join; drain; idempotent
  ReplayOffloadQueue& queue() { return queue_; }
  bool failed() const { return failed_.load(std::memory_order_acquire); }
  void failNextStartForTest() noexcept { testOnlyFailStart_ = true; }
  bool startedForTest() const noexcept {
    return owner_ != nullptr || thread_.joinable();
  }

 private:
  void run(D9CDevice* device);     // pop loop -> replayRawChunk -> markReplayDone, then drain
  ReplayOffloadQueue queue_;
  std::thread thread_;
  std::atomic<bool> failed_{false};
  D9CDevice* owner_ = nullptr;
  ReplayFn replay_ = nullptr;
  ApplyMutationFn applyMutation_ = nullptr;
  ApplyStateBlockFn applyStateBlock_ = nullptr;
  FailurePublishedHook failureHook_ = nullptr;
  void* failureHookContext_ = nullptr;
  bool testOnlyFailStart_ = false;
};

// Replays a prevalidated, resolved canonical chunk. The worker publishes ledger
// completion before releasing retained wrappers so its target pointers remain
// alive through the publication.
int32_t replayRawChunk(D9CDevice* d, RawCommandChunk& chunk);

// Applies one committed mutation task at its FIFO position (R-BACK-44.3).
// A negative return fail-stops the worker under the existing poison
// discipline; there is no "skip" disposition, because a skipped mutation is a
// silently wrong buffer.
int32_t applyBufferMutationTask(D9CDevice* d, BufferMutationTask& task);

int32_t applyStateBlockObject(
    D9CDevice* d,
    const std::shared_ptr<dxmt9::core::StateBlock>& stateBlock);
int32_t applyStateBlockApplyTask(D9CDevice* d, StateBlockApplyTask& task);

// Ordered StateBlock apply bridge. Accepted calls return after the task is
// appended to the single replay FIFO; pre-effect admission failures fall back
// to the synchronous provider after draining prior FIFO work.
int32_t enqueueStateBlockApply(D9CStateBlock* stateBlock) noexcept;

// getenv("DXMT9_MANAGED_MUTATION_OFFLOAD"), read once. Unset, empty, and "0"
// select the byte-identical synchronous upload path (R-BACK-44.1).
bool managedMutationOffloadEnabled() noexcept;

// R-BACK-44.1 scope gate for one pending unlock, evaluated through the shared
// predicate `dxmt9::resources::mutation_offload::admitsManagedMutationOffload`.
// Reads only state the caller already owns plus one buffer-arena probe for the
// record's `hasVersionedBacking`, and costs a single cached bool when the mode
// is off.
bool admitsManagedMutationOffload(D9CBuffer* b) noexcept;

enum class BufferMutationOffloadResult : std::uint8_t {
  // Nothing was attempted; the caller must run the synchronous upload path.
  NotAdmitted,
  // Reserved, staged, rotated, committed and published. The caller may now —
  // and only now — clear lock state on every layer.
  Committed,
  // R-BACK-44.2 step-1 failure: no rotation, no revision bump, no enqueue.
  // The caller must leave every layer's lock state intact and return a
  // retryable failure so the unlock can be attempted again.
  RejectedPreEffect,
};

// The R-BACK-44.2 reserve -> stage -> rotate -> commit transaction. Called
// only when `admitsManagedMutationOffload` already held for this unlock (the
// bridge fence evaluated it and skipped the pre-mutation drain on the strength
// of it), so it never re-derives admission — re-deriving it would open a window
// where the fence was skipped and the synchronous path ran anyway.
BufferMutationOffloadResult offloadManagedBufferMutation(D9CBuffer* b) noexcept;

// Releases every wrapper retained during canonical admission and clears the list.
// Namespace linkage allows both the commit push-failure path and the offload
// worker's fail-stop drain to call it.
void releaseRetainedWrappers(RawCommandChunk& chunk);

}  // namespace dxmt9::d3d9
