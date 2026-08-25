// R-BACK-44.x — the PRODUCTION half of the Managed buffer mutation offload:
// the pool's rotate/apply split, the offload queue's reserve/commit/release
// transaction, the worker's FIFO dispatch across chunks and mutations, and the
// end-to-end unlock transaction through a real `core::Buffer` + `D9CBuffer`.
//
// `buffer_mutation_offload_spec.cpp` is the sibling that binds
// `specs/verification/tla/BufferMutationOffload.tla` to the pure predicates in
// `src/dxmt9/dxmt9_mutation_offload_predicates.hpp`. This file does not repeat
// those truth tables; it exercises the code that CALLS them.
//
// The binary is registered TWICE in `meson.build`: once with the mode off (the
// rollback lane, which must resolve to the synchronous path) and once with
// `DXMT9_MANAGED_MUTATION_OFFLOAD=1`. The mode resolver is read-once-and-cached
// by design, so a single process can only ever observe one of the two answers —
// running the same tests under both environments is what covers the pair.
//
// What this file deliberately does NOT prove: Metal behavior (every WMT::Device
// here is `NULL_OBJECT_HANDLE`, so `BufferRenameRingEntry::contents` is
// test-owned memory), GPU-visible bytes, or the wild-run promotion gates in
// R-BACK-44.8.

#include "../../../src/d3d9/device_c_common.hpp"
#include "../../../src/d3d9/device_c_replay_offload.hpp"
#include "../../../src/dxmt9/dxmt9_device.hpp"
#include "../../../src/dxmt9/dxmt9_mutation_offload_predicates.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

constexpr std::uint32_t kBufferBytes = 64u;
constexpr std::uint64_t kPatchOffset = 16u;
constexpr std::uint64_t kPatchLength = 8u;

dxmt9::core::BufferDesc managedBufferDesc() {
  dxmt9::core::BufferDesc desc{};
  desc.size = kBufferBytes;
  desc.pool = dxmt9::core::Pool::Managed;
  return desc;
}

// The pre-mutation content every fixture starts from, and the post-mutation
// core `storage_` an app produces by writing only `[kPatchOffset, +kPatchLength)`.
std::vector<std::uint8_t> preMutationBytes() {
  std::vector<std::uint8_t> bytes(kBufferBytes);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(0x10u + i);
  }
  return bytes;
}

std::vector<std::uint8_t> postMutationBytes() {
  auto bytes = preMutationBytes();
  for (std::size_t i = 0; i < kPatchLength; ++i) {
    bytes[kPatchOffset + i] = static_cast<std::uint8_t>(0xa0u + i);
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Pool-level: the rotate/apply split must land byte-for-byte where the
// synchronous `uploadBufferData` lands.
// ---------------------------------------------------------------------------

struct PoolFixture {
  dxmt9::resources::Pool pool;
  dxmt9::core::BufferHandle handle{};
  // Test-owned stand-in for the Metal shared-mode `contents` pointer. A null
  // WMT::Device hands back no mapped memory, so the fixture supplies it.
  std::vector<std::uint8_t> backing0;
  std::vector<std::uint8_t> backing1;

  PoolFixture() : backing0(preMutationBytes()), backing1(kBufferBytes, 0u) {
    WMT::Device device{NULL_OBJECT_HANDLE};
    handle = pool.createBuffer(device, managedBufferDesc());
    auto* record = pool.findBuffer(handle.value);
    check(record != nullptr, "managed buffer record exists");
    check(record->isManagedVersioned,
          "D3DPOOL_MANAGED selects the versioned-backing policy");
    check(record->renameRing.size() == 1u,
          "create seeds exactly one ring entry");
    // Publish the pointers only after `backing0` has its final storage: a
    // later vector assignment would move-replace the buffer and leave the
    // record pointing at freed memory.
    record->renameRing[0].contents = backing0.data();
    record->contents = backing0.data();
    record->shadow = preMutationBytes();
  }

  dxmt9::resources::BufferRecord& record() {
    auto* found = pool.findBuffer(handle.value);
    check(found != nullptr, "record still reachable");
    return *found;
  }
};

void testDeferredApplyMatchesSynchronousUploadByteForByte() {
  const auto post = postMutationBytes();

  // Reference: today's path — the whole core `storage_` handed to the pool.
  PoolFixture reference;
  check(reference.pool.uploadBufferData(WMT::Device{NULL_OBJECT_HANDLE},
                                        reference.handle.value, post.data(),
                                        post.size(), /*completedSeqId=*/0u),
        "synchronous upload resolves the handle");

  // Candidate: rotate now, apply the staged dirty span later.
  PoolFixture candidate;
  const auto lease = candidate.pool.rotateManagedBufferForMutation(
      WMT::Device{NULL_OBJECT_HANDLE}, candidate.handle.value,
      /*completedSeqId=*/0u);
  check(lease.valid, "rotation produces a lease");
  check(lease.contents == candidate.backing0.data(),
        "an idle active entry is leased in place");
  check(lease.contentRevision == candidate.record().contentRevision,
        "the lease carries the revision the rotation published");
  check(candidate.record().contentRevision ==
            reference.record().contentRevision,
        "deferring the byte copy does not change how often the revision moves");
  // Nothing has been materialized yet: the leased backing still holds
  // pre-mutation bytes, which is exactly what makes the ordering argument
  // load-bearing rather than decorative.
  check(candidate.backing0 == preMutationBytes(),
        "rotation alone publishes no bytes");

  const auto applied = candidate.pool.applyManagedBufferMutation(
      candidate.handle.value, lease, kPatchOffset,
      post.data() + kPatchOffset, kPatchLength);
  check(applied.applied, "apply accepts a matching lease");
  check(applied.patchBytes == kPatchLength, "patch bytes are the dirty span");
  check(applied.copyForwardBytes == kBufferBytes - kPatchLength,
        "copy-forward covers exactly the untouched region");

  check(candidate.backing0 == reference.backing0,
        "deferred apply writes the leased backing byte-identically");
  check(candidate.record().shadow == reference.record().shadow,
        "deferred apply writes the pool CPU shadow byte-identically");
  check(candidate.record().shadow == post,
        "the shadow ends at the post-mutation core storage content");
}

void testApplyRejectsALeaseThatNoLongerNamesItsEntry() {
  PoolFixture fixture;
  const auto lease = fixture.pool.rotateManagedBufferForMutation(
      WMT::Device{NULL_OBJECT_HANDLE}, fixture.handle.value, 0u);
  check(lease.valid, "rotation produces a lease");

  // Simulate the ring entry having been re-pointed at a different allocation.
  fixture.record().renameRing[lease.renameIndex].contents =
      fixture.backing1.data();
  const auto post = postMutationBytes();
  const auto applied = fixture.pool.applyManagedBufferMutation(
      fixture.handle.value, lease, kPatchOffset, post.data() + kPatchOffset,
      kPatchLength);
  check(!applied.applied,
        "a lease that stopped naming its entry must not be retargeted");
  check(fixture.backing1 == std::vector<std::uint8_t>(kBufferBytes, 0u),
        "a rejected apply writes nothing anywhere");
  check(fixture.record().shadow == preMutationBytes(),
        "a rejected apply leaves the pool shadow untouched");
}

void testHeldLeaseKeepsTheNextRotationOffTheSameEntry() {
  PoolFixture fixture;
  const auto lease = fixture.pool.rotateManagedBufferForMutation(
      WMT::Device{NULL_OBJECT_HANDLE}, fixture.handle.value, 0u);
  check(lease.valid && lease.renameIndex == 0u, "first rotation leases entry 0");
  check(fixture.record().renameRing.size() == 1u,
        "an idle solo ring does not grow on the first rotation");

  // R-BACK-44.2a: the lease holds the entry's `replayResidency`, so the entry
  // is NOT idle for the next rotation even though its seq watermark is 0.
  const auto second = fixture.pool.rotateManagedBufferForMutation(
      WMT::Device{NULL_OBJECT_HANDLE}, fixture.handle.value, 0u);
  check(second.valid, "second rotation succeeds");
  check(second.renameIndex != lease.renameIndex,
        "a leased entry is ineligible for the next rotation");
  check(fixture.record().renameRing.size() == 2u,
        "the ring grew rather than overwriting bytes a queued task owns");
}

void testVersionedBackingQueryMatchesTheRecordPolicy() {
  PoolFixture fixture;
  check(fixture.pool.bufferHasVersionedBacking(fixture.handle.value),
        "a Managed record reports versioned backing");
  check(!fixture.pool.bufferHasVersionedBacking(0u),
        "a missing handle reports no versioned backing");

  dxmt9::resources::Pool plainPool;
  dxmt9::core::BufferDesc desc{};
  desc.size = kBufferBytes;
  desc.pool = dxmt9::core::Pool::Default;
  const auto plain =
      plainPool.createBuffer(WMT::Device{NULL_OBJECT_HANDLE}, desc);
  check(!plainPool.bufferHasVersionedBacking(plain.value),
        "a non-dynamic DEFAULT record has no ring to rotate");
}

// ---------------------------------------------------------------------------
// Queue-level: reserve fixes the FIFO position AND the ordinal.
// ---------------------------------------------------------------------------

std::unique_ptr<dxmt9::d3d9::BufferMutationTask> makeTask(
    std::shared_ptr<dxmt9::d3d9::ReplayDrainTarget> target,
    std::size_t bytes) {
  auto task = std::make_unique<dxmt9::d3d9::BufferMutationTask>();
  task->ledgerTarget = std::move(target);
  task->stagedBytes.assign(bytes, 0u);
  task->lease.valid = true;
  return task;
}

dxmt9::d3d9::RawCommandChunk makeChunk(std::uint32_t bytes) {
  dxmt9::d3d9::RawCommandChunk chunk;
  chunk.recordBlob.resize(bytes);
  chunk.recordBytes = bytes;
  chunk.recordCount = 1;
  return chunk;
}

void testReservationHoldsItsPositionAgainstAConcurrentChunkPush() {
  using namespace dxmt9::d3d9;
  ReplayOffloadQueue queue(8, 1u << 20);
  ReplayDrainLedger ledger;
  auto target = ledger.targetForCoreBuffer(0x1234u);

  const auto reservation = queue.reserveMutation(24u, ledger);
  check(reservation.valid(), "reservation is granted");
  check(reservation.replaySeq == 1u,
        "the reservation takes the next FIFO ordinal");
  check(queue.queuedBytesForTest() == 24u,
        "the staged-byte budget is charged at reserve");
  check(queue.depth() == 1u, "the reservation occupies its queue position");
  check(target->lastQueuedSeq == 0u,
        "reserve has no externally visible effect: nothing is published yet");

  // The worker must not be able to run past an uncommitted reservation.
  std::atomic<bool> popped{false};
  ReplayQueueItem head;
  std::thread consumer([&] {
    if (queue.pop(head)) {
      popped.store(true, std::memory_order_release);
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!popped.load(std::memory_order_acquire),
        "pop blocks while the head is an uncommitted reservation");

  auto chunk = makeChunk(8);
  chunk.ledgerTargets.push_back(target.get());
  check(queue.push(std::move(chunk), &ledger),
        "a concurrent chunk push is admitted behind the reservation");
  check(target->lastQueuedSeq == 2u,
        "the interleaved chunk takes the LATER ordinal");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!popped.load(std::memory_order_acquire),
        "the interleaved chunk cannot be popped ahead of the reservation");

  check(queue.commitMutation(reservation, makeTask(target, 24u), ledger),
        "commit fills the reserved slot");
  check(target->lastQueuedSeq == 2u,
        "commit publishes with max(), never regressing the chunk's watermark");
  consumer.join();
  check(popped.load(std::memory_order_acquire) && head.isMutation() &&
            head.mutation->replaySeq == 1u,
        "the mutation pops first, carrying its reserved ordinal");
  queue.markReplayDone();

  ReplayQueueItem next;
  check(queue.pop(next) && !next.isMutation() && next.chunk.replaySeq == 2u,
        "the interleaved chunk pops second");
  queue.markReplayDone();
  check(queue.queuedBytesForTest() == 0u && queue.depth() == 0u,
        "both charges are released exactly once");
}

void testReservationReleaseFreesPositionBudgetAndPublishesNothing() {
  using namespace dxmt9::d3d9;
  ReplayOffloadQueue queue(8, 1u << 20);
  ReplayDrainLedger ledger;
  auto target = ledger.targetForCoreBuffer(0x99u);

  const auto reservation = queue.reserveMutation(40u, ledger);
  check(reservation.valid() && queue.queuedBytesForTest() == 40u,
        "reservation charges the budget");
  auto chunk = makeChunk(8);
  chunk.ledgerTargets.push_back(target.get());
  check(queue.push(std::move(chunk), &ledger), "chunk queued behind it");

  queue.releaseMutation(reservation);
  check(queue.queuedBytesForTest() == 8u,
        "release refunds exactly the reserved charge");
  check(queue.depth() == 1u, "release removes the reservation's position");
  check(target->lastQueuedSeq == 2u && target->lastReplayedSeq == 0u,
        "a released reservation publishes no watermark of its own");

  ReplayQueueItem head;
  check(queue.pop(head) && !head.isMutation(),
        "the chunk becomes poppable once the reservation is released");
  queue.markReplayDone();

  // The burned ordinal is a gap, not a stall: the target's queued watermark
  // came from the chunk, and the chunk's replay catches it up.
  ledger.publishReplayed(head.chunk);
  check(!ledger.pending(*target),
        "a released reservation cannot leave a resource permanently fenced");
}

void testReservationBudgetBoundsBlockAndAdmitOversize() {
  using namespace dxmt9::d3d9;
  ReplayDrainLedger ledger;
  {
    ReplayOffloadQueue queue(1, 1u << 20);
    const auto first = queue.reserveMutation(8u, ledger);
    check(first.valid(), "first reservation fits the count bound");
    std::atomic<bool> granted{false};
    std::thread producer([&] {
      const auto second = queue.reserveMutation(8u, ledger);
      granted.store(second.valid(), std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!granted.load(std::memory_order_acquire),
          "a reservation blocks on the same count bound a chunk push does");
    queue.releaseMutation(first);
    producer.join();
    check(granted.load(std::memory_order_acquire),
          "the blocked reservation completes once space frees");
  }
  {
    // Oversize escape hatch: a staged span larger than the byte bound must
    // still be admitted on an empty queue, or the producer would hang forever.
    ReplayOffloadQueue queue(4, 16);
    const auto oversize = queue.reserveMutation(4096u, ledger);
    check(oversize.valid(),
          "an oversize staged span is admitted when the queue is empty");
    queue.releaseMutation(oversize);
  }
}

void testStoppedQueueRejectsReservationPreEffect() {
  using namespace dxmt9::d3d9;
  ReplayOffloadQueue queue(4, 1u << 20);
  ReplayDrainLedger ledger;
  queue.stop();
  check(!queue.reserveMutation(8u, ledger).valid(),
        "a stopped queue rejects the reservation");
  check(queue.queuedBytesForTest() == 0u && queue.depth() == 0u,
        "a rejected reservation charges nothing");

  ReplayOffloadQueue live(4, 1u << 20);
  ReplayDrainLedger poisoned;
  poisoned.poison();
  check(!live.reserveMutation(8u, poisoned).valid(),
        "a poisoned ledger rejects the reservation");
  check(live.depth() == 0u, "a ledger-rejected reservation leaves no slot");
}

// ---------------------------------------------------------------------------
// Worker-level: one FIFO order across both alternatives.
// ---------------------------------------------------------------------------

struct WorkerOrderLog {
  std::mutex mutex;
  std::vector<std::string> entries;

  void note(std::string entry) {
    std::lock_guard lock(mutex);
    entries.push_back(std::move(entry));
  }
  std::vector<std::string> snapshot() {
    std::lock_guard lock(mutex);
    return entries;
  }
};

WorkerOrderLog* gWorkerOrderLog = nullptr;

std::int32_t recordingReplay(D9CDevice*, dxmt9::d3d9::RawCommandChunk& chunk) {
  gWorkerOrderLog->note("chunk:" + std::to_string(chunk.replaySeq));
  return dxmt9::core::D3D_OK;
}

std::int32_t recordingApply(D9CDevice*,
                            dxmt9::d3d9::BufferMutationTask& task) {
  gWorkerOrderLog->note("mutation:" + std::to_string(task.replaySeq));
  return dxmt9::core::D3D_OK;
}

std::int32_t failingApply(D9CDevice*, dxmt9::d3d9::BufferMutationTask&) {
  return dxmt9::core::D3DERR_INVALIDCALL;
}

void testWorkerInterleavesMutationsAndChunksInOneFifoOrder() {
  using namespace dxmt9::d3d9;
  WorkerOrderLog log;
  gWorkerOrderLog = &log;
  D9CDevice device(nullptr);
  device.replayOffload = std::make_unique<ReplayOffloadWorker>(
      recordingReplay, nullptr, nullptr, recordingApply);
  auto& queue = device.replayOffload->queue();
  auto& ledger = device.replayDrainLedger;
  auto target = ledger.targetForCoreBuffer(0x4242u);

  auto first = makeChunk(8);
  first.ledgerTargets.push_back(target.get());
  check(queue.push(std::move(first), &ledger), "chunk A queued");
  const auto reservation = queue.reserveMutation(8u, ledger);
  check(reservation.valid(), "mutation reserved between the two chunks");
  check(queue.commitMutation(reservation, makeTask(target, 8u), ledger),
        "mutation committed");
  auto second = makeChunk(8);
  second.ledgerTargets.push_back(target.get());
  check(queue.push(std::move(second), &ledger), "chunk B queued");

  check(device.replayOffload->start(&device), "worker starts");
  queue.waitDrained();
  device.replayOffload->stop();
  gWorkerOrderLog = nullptr;

  const auto order = log.snapshot();
  check(order.size() == 3u, "every queued item ran exactly once");
  check(order[0] == "chunk:1" && order[1] == "mutation:2" &&
            order[2] == "chunk:3",
        "a mutation enqueued after A and before B runs strictly between them");
  check(target->lastReplayedSeq == 3u,
        "mutation completion publishes against the same per-buffer target");
}

void testWorkerApplyFailureFailStopsRatherThanSkipping() {
  using namespace dxmt9::d3d9;
  D9CDevice device(nullptr);
  device.replayOffload = std::make_unique<ReplayOffloadWorker>(
      recordingReplay, nullptr, nullptr, failingApply);
  auto& queue = device.replayOffload->queue();
  auto& ledger = device.replayDrainLedger;
  auto target = ledger.targetForCoreBuffer(0x777u);

  const auto reservation = queue.reserveMutation(8u, ledger);
  check(reservation.valid(), "reservation granted");
  check(queue.commitMutation(reservation, makeTask(target, 8u), ledger),
        "mutation committed");
  check(device.replayOffload->start(&device), "worker starts");
  queue.waitDrained();
  device.replayOffload->stop();

  check(device.replayOffload->failed(),
        "a failed apply fail-stops the worker");
  check(ledger.poisoned(), "a failed apply poisons the device ledger");
  check(target->lastReplayedSeq == 0u,
        "a failed apply never acknowledges its ordinal");
}

void testTeardownDiscardsPendingMutationsReleasingBudgetAndRetention() {
  using namespace dxmt9::d3d9;
  D9CDevice device(nullptr);
  device.replayOffload = std::make_unique<ReplayOffloadWorker>();
  auto& queue = device.replayOffload->queue();
  auto& ledger = device.replayDrainLedger;
  auto target = ledger.targetForCoreBuffer(0x5150u);

  // A retention stand-in with the same ownership shape the real task's
  // `shared_ptr<core::Buffer>` has: the task holding it is what keeps the pool
  // record alive, so releasing it is the observable teardown effect.
  bool retentionReleased = false;
  std::shared_ptr<dxmt9::core::Buffer> retention(
      static_cast<dxmt9::core::Buffer*>(nullptr),
      [&](dxmt9::core::Buffer*) { retentionReleased = true; });

  const auto reservation = queue.reserveMutation(32u, ledger);
  check(reservation.valid(), "reservation granted");
  auto task = makeTask(target, 32u);
  task->coreBuffer = retention;
  check(queue.commitMutation(reservation, std::move(task), ledger),
        "mutation committed");
  retention.reset();
  check(!retentionReleased, "the queued task owns the retention");
  check(queue.queuedBytesForTest() == 32u, "the charge is live while queued");

  // Never started: `stop()` runs the teardown drain on the calling thread.
  device.replayOffload->stop();
  check(retentionReleased,
        "a discarded mutation task releases its retention");
  check(queue.queuedBytesForTest() == 0u && queue.depth() == 0u,
        "a discarded mutation task releases its byte-budget charge");
  check(target->lastReplayedSeq == 0u,
        "a discarded task never claims to have been applied");
}

// ---------------------------------------------------------------------------
// End-to-end: the unlock transaction through real core/pool objects.
// ---------------------------------------------------------------------------

class MutationOffloadStubDevice final : public dxmt9::Device {
 public:
  MutationOffloadStubDevice()
      : queue_(WMT::Device{NULL_OBJECT_HANDLE}, limits_, false) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const dxmt9::core::BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<dxmt9::core::BackendDevice> backend() override { return {}; }

  dxmt9::core::BufferHandle createBuffer(
      const dxmt9::core::BufferDesc& desc) override {
    return queue_.pool().createBuffer(WMT::Device{NULL_OBJECT_HANDLE}, desc);
  }
  void destroyBuffer(dxmt9::core::BufferHandle handle) override {
    destroyedBuffers.fetch_add(1u, std::memory_order_relaxed);
    queue_.pool().markBufferDestroyAndGc(handle.value, 0u);
  }
  void uploadBufferData(dxmt9::core::BufferHandle handle,
                        std::span<const std::uint8_t> bytes) override {
    synchronousUploads.fetch_add(1u, std::memory_order_relaxed);
    queue_.pool().uploadBufferData(WMT::Device{NULL_OBJECT_HANDLE},
                                   handle.value, bytes.data(), bytes.size(),
                                   0u);
  }
  bool bufferHasVersionedBacking(dxmt9::core::BufferHandle handle) override {
    return queue_.pool().bufferHasVersionedBacking(handle.value);
  }
  dxmt9::resources::ManagedBufferMutationLease rotateManagedBufferForMutation(
      dxmt9::core::BufferHandle handle) override {
    return queue_.pool().rotateManagedBufferForMutation(
        WMT::Device{NULL_OBJECT_HANDLE}, handle.value, 0u);
  }
  dxmt9::resources::ManagedBufferMutationApplyResult
  applyManagedBufferMutation(
      dxmt9::core::BufferHandle handle,
      const dxmt9::resources::ManagedBufferMutationLease& lease,
      std::uint64_t offset,
      std::span<const std::uint8_t> bytes) override {
    return queue_.pool().applyManagedBufferMutation(
        handle.value, lease, offset, bytes.data(), bytes.size());
  }

  dxmt9::resources::Pool* pool() override { return &queue_.pool(); }
  dxmt9::resources::Pool& poolRef() noexcept { return queue_.pool(); }

  std::atomic<std::uint32_t> synchronousUploads{0};
  std::atomic<std::uint32_t> destroyedBuffers{0};

 private:
  dxmt9::core::BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
};

struct UnlockFixture {
  std::shared_ptr<MutationOffloadStubDevice> upper =
      std::make_shared<MutationOffloadStubDevice>();
  std::shared_ptr<dxmt9::core::Device> coreDevice;
  std::shared_ptr<dxmt9::core::Buffer> buffer;
  std::unique_ptr<D9CDevice> cDevice;
  std::unique_ptr<D9CBuffer> wrapper;
  std::vector<std::uint8_t> backing;

  UnlockFixture() : backing(kBufferBytes, 0u) {
    coreDevice = std::make_shared<dxmt9::core::Device>(
        dxmt9::core::AdapterInfo{}, dxmt9::core::BackendLimits{},
        dxmt9::core::PresentParameters{}, 0u, upper);
    buffer = coreDevice->createBuffer(managedBufferDesc());
    check(static_cast<bool>(buffer), "core buffer created");

    auto* record = upper->poolRef().findBuffer(buffer->handle().value);
    check(record != nullptr, "pool record created");
    backing = preMutationBytes();
    record->renameRing[0].contents = backing.data();
    record->contents = backing.data();
    record->shadow = preMutationBytes();

    cDevice = std::make_unique<D9CDevice>(nullptr);
    cDevice->replayOffload =
        std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>();
    wrapper = std::make_unique<D9CBuffer>(buffer, cDevice.get());
    wrapper->desc.size = kBufferBytes;
    wrapper->desc.pool = 1u;  // D3DPOOL_MANAGED
  }

  ~UnlockFixture() {
    // Break the leases before the test-owned `backing` vector dies.
    if (cDevice && cDevice->replayOffload) {
      cDevice->replayOffload->stop();
    }
    wrapper.reset();
    buffer.reset();
  }

  // Everything `dxmt9c_buffer_lock` records for a plain writable lock, plus
  // the app's own write into the mapped span.
  void plainWritableLockAndWrite() {
    const auto region = buffer->lock(kPatchOffset, kPatchLength, 0u);
    check(region.data != nullptr, "plain writable lock returns a span");
    const auto post = postMutationBytes();
    std::memcpy(region.data, post.data() + kPatchOffset, kPatchLength);
    wrapper->lastLockReadOnly = false;
    wrapper->lastLockOffset = static_cast<std::uint32_t>(kPatchOffset);
    wrapper->lastLockSize = static_cast<std::uint32_t>(kPatchLength);
    wrapper->lastLockFlags = 0u;
    wrapper->lastLockSucceeded = true;
  }
};

void testUnlockTransactionUnderModeOff() {
  using namespace dxmt9::d3d9;
  check(!managedMutationOffloadEnabled(),
        "this lane runs with DXMT9_MANAGED_MUTATION_OFFLOAD unset");
  UnlockFixture fixture;
  fixture.plainWritableLockAndWrite();
  check(!admitsManagedMutationOffload(fixture.wrapper.get()),
        "mode off rejects an otherwise perfectly admissible unlock");
  check(fixture.cDevice->replayOffload->queue().depth() == 0u,
        "mode off enqueues nothing");
}

void testUnlockTransactionUnderModeOn() {
  using namespace dxmt9::d3d9;
  check(managedMutationOffloadEnabled(),
        "this lane runs with DXMT9_MANAGED_MUTATION_OFFLOAD=1");
  check(offloadCommitReplayEnabled(),
        "the mode requires the commit-replay offload, which is engine-default");

  UnlockFixture fixture;
  fixture.plainWritableLockAndWrite();
  check(admitsManagedMutationOffload(fixture.wrapper.get()),
        "a plain writable Managed unlock with a versioned record is admitted");

  const auto revisionBefore =
      fixture.upper->poolRef().findBuffer(fixture.buffer->handle().value)
          ->contentRevision;
  const auto result = offloadManagedBufferMutation(fixture.wrapper.get());
  check(result == BufferMutationOffloadResult::Committed,
        "the transaction commits");
  auto* record =
      fixture.upper->poolRef().findBuffer(fixture.buffer->handle().value);
  check(record->contentRevision == revisionBefore + 1u,
        "the revision is bumped synchronously, at unlock");
  check(fixture.upper->synchronousUploads.load() == 0u,
        "no synchronous full upload ran");
  check(fixture.cDevice->replayOffload->queue().depth() == 1u,
        "exactly one mutation task is queued");
  check(fixture.wrapper->replayDrainTarget->lastQueuedSeq == 1u,
        "the task is published against the buffer's resource-scoped target");
  check(fixture.backing == preMutationBytes(),
        "the bytes have NOT landed yet — that is the point of the mode");
  check(fixture.buffer->locked(),
        "core lock state survives until the caller clears it after commit");

  check(fixture.cDevice->replayOffload->start(fixture.cDevice.get()),
        "worker starts");
  fixture.cDevice->replayOffload->queue().waitDrained();
  check(fixture.wrapper->replayDrainTarget->lastReplayedSeq == 1u,
        "the worker publishes completion against the same target");

  const auto post = postMutationBytes();
  check(fixture.backing == post,
        "the applied backing equals what the synchronous upload would write");
  check(record->shadow == post,
        "the applied pool shadow equals what the synchronous upload would write");
  fixture.buffer->finishDeferredUnlock();
}

void testRepeatedUnlocksComposeThroughTheShadow() {
  using namespace dxmt9::d3d9;
  if (!managedMutationOffloadEnabled()) {
    return;
  }
  // Two offloaded unlocks writing DISJOINT spans. The second task's
  // copy-forward reads the shadow the first task wrote, which is the induction
  // step R-BACK-44.3's ordering argument rests on: get it wrong and the second
  // apply silently reverts the first.
  UnlockFixture fixture;
  auto expected = preMutationBytes();

  const auto writeSpan = [&](std::uint64_t offset, std::uint64_t length,
                             std::uint8_t seed) {
    const auto region = fixture.buffer->lock(offset, length, 0u);
    check(region.data != nullptr, "writable lock returns a span");
    for (std::uint64_t i = 0; i < length; ++i) {
      const auto value = static_cast<std::uint8_t>(seed + i);
      static_cast<std::uint8_t*>(region.data)[i] = value;
      expected[offset + i] = value;
    }
    fixture.wrapper->lastLockReadOnly = false;
    fixture.wrapper->lastLockOffset = static_cast<std::uint32_t>(offset);
    fixture.wrapper->lastLockSize = static_cast<std::uint32_t>(length);
    fixture.wrapper->lastLockFlags = 0u;
    fixture.wrapper->lastLockSucceeded = true;
    check(admitsManagedMutationOffload(fixture.wrapper.get()), "admitted");
    check(offloadManagedBufferMutation(fixture.wrapper.get()) ==
              BufferMutationOffloadResult::Committed,
          "committed");
    fixture.buffer->finishDeferredUnlock();
  };

  writeSpan(0u, 8u, 0xc0u);
  writeSpan(48u, 8u, 0xd0u);
  check(fixture.cDevice->replayOffload->queue().depth() == 2u,
        "both unlocks are queued before either is applied");

  check(fixture.cDevice->replayOffload->start(fixture.cDevice.get()),
        "worker starts");
  fixture.cDevice->replayOffload->queue().waitDrained();

  auto* record =
      fixture.upper->poolRef().findBuffer(fixture.buffer->handle().value);
  // THE induction step: the second task's copy-forward read the shadow the
  // first task had already written, so both disjoint spans survive. Get the
  // FIFO order wrong and the second apply silently reverts the first.
  check(record->shadow == expected,
        "FIFO application composes both dirty spans into the shadow");

  // R-BACK-44.2a again, now on the live path: the first task still held entry
  // 0's residency when the second unlock rotated, so the ring had to grow
  // rather than let the second mutation overwrite bytes the first one owned.
  check(record->renameRing.size() == 2u,
        "the second rotation could not reuse the first task's leased entry");
  auto expectedFirstOnly = preMutationBytes();
  for (std::uint64_t i = 0; i < 8u; ++i) {
    expectedFirstOnly[i] = static_cast<std::uint8_t>(0xc0u + i);
  }
  check(fixture.backing == expectedFirstOnly,
        "the first task's leased backing carries exactly its own mutation");
  // The second task leased a freshly allocated entry, and a NULL_OBJECT_HANDLE
  // WMT::Device hands back no mapped memory for it — so this fixture can prove
  // the shadow composition and the lease separation, but not the second
  // entry's bytes. That is a limitation of the null-device harness, not of the
  // path: `applyManagedBufferMutation` writes `contents` whenever it is
  // non-null, which `testDeferredApplyMatchesSynchronousUploadByteForByte`
  // covers byte-for-byte.
  check(record->renameRing[1].contents == nullptr,
        "the null-device harness cannot map the freshly grown ring entry");
}

}  // namespace

int main() {
  try {
    testDeferredApplyMatchesSynchronousUploadByteForByte();
    testApplyRejectsALeaseThatNoLongerNamesItsEntry();
    testHeldLeaseKeepsTheNextRotationOffTheSameEntry();
    testVersionedBackingQueryMatchesTheRecordPolicy();
    testReservationHoldsItsPositionAgainstAConcurrentChunkPush();
    testReservationReleaseFreesPositionBudgetAndPublishesNothing();
    testReservationBudgetBoundsBlockAndAdmitOversize();
    testStoppedQueueRejectsReservationPreEffect();
    testWorkerInterleavesMutationsAndChunksInOneFifoOrder();
    testWorkerApplyFailureFailStopsRatherThanSkipping();
    testTeardownDiscardsPendingMutationsReleasingBudgetAndRetention();
    if (dxmt9::d3d9::managedMutationOffloadEnabled()) {
      testUnlockTransactionUnderModeOn();
      testRepeatedUnlocksComposeThroughTheShadow();
    } else {
      testUnlockTransactionUnderModeOff();
    }
  } catch (const TestFailure& failure) {
    std::cerr << "managed_mutation_offload_transaction_spec failed: "
              << failure.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "managed_mutation_offload_transaction_spec unexpected "
                 "exception: " << error.what() << '\n';
    return 1;
  }
  std::cout << "managed_mutation_offload_transaction_spec passed\n";
  return 0;
}
