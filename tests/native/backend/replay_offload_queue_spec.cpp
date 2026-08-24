// Pure-data spec for dxmt9::d3d9::ReplayOffloadQueue — bounded FIFO with a
// drain fence used by the commit-replay offload path.
#include "../../../src/d3d9/device_c_replay_offload.hpp"
#include "../../../src/d3d9/device_c_common.hpp"

#include <atomic>
#include <array>
#include <barrier>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace dxmt9::d3d9 {

struct ReplayDrainLedgerTestAccess {
  static std::size_t cacheSize(const ReplayDrainLedger& ledger) {
    std::lock_guard lock(ledger.mutex_);
    return ledger.bufferTargets_.size();
  }

  static std::size_t nextSweep(const ReplayDrainLedger& ledger) {
    std::lock_guard lock(ledger.mutex_);
    return ledger.nextBufferTargetSweep_;
  }
};

}  // namespace dxmt9::d3d9

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

void testFailureDispositionAndByteAdoption() {
  using namespace dxmt9::d3d9;
  ReplayQueueAllocationFailure allocationFailure;
  ReplayOffloadQueue q(64, 1u << 20, &allocationFailure);

  auto* retained = new D9CBuffer(nullptr);
  retained->refs.fetch_add(1u);
  auto before = makeChunk(17);
  before.retainedWrappers.push_back({
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .ptr = retained});
  q.failNextAllocationForTest();
  check(q.pushWithDisposition(std::move(before)) ==
            ReplayQueuePushDisposition::RejectedPreEffect,
        "actual deque allocator failure before adoption is retryable");
  check(before.recordBlob.size() == 17u && q.queuedBytesForTest() == 0u &&
            retained->refs.load() == 2u,
        "allocator failure preserves caller wrapper ownership and byte count");
  releaseRetainedWrappers(before);
  check(retained->refs.load() == 1u,
        "caller releases the allocation-rejected wrapper exactly once");
  delete retained;

  ReplayDrainLedger ledger;
  ReplayDrainTarget target;
  auto* adoptedRetained = new D9CBuffer(nullptr);
  adoptedRetained->refs.fetch_add(1u);
  auto after = makeChunk(23);
  after.ledgerTargets.push_back(&target);
  after.retainedWrappers.push_back({
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .ptr = adoptedRetained});
  q.failNextPushForTest(ReplayQueueFailurePoint::AfterAdoption);
  check(q.pushWithDisposition(std::move(after), &ledger) ==
            ReplayQueuePushDisposition::EffectUnknown,
        "synthetic post-adoption disposition is effect-unknown");
  check(q.queuedBytesForTest() == 23u && target.lastQueuedSeq != 0u,
        "post-adoption bytes and ledger publish exactly once");
  RawCommandChunk adopted;
  check(q.pop(adopted) && adopted.recordBytes == 23u,
        "effect-unknown queue retains adopted chunk ownership");
  check(q.queuedBytesForTest() == 0u,
        "queued byte count changes only at adoption and pop");
  releaseRetainedWrappers(adopted);
  check(adoptedRetained->refs.load() == 1u,
        "effect-unknown queue releases its adopted wrapper exactly once");
  delete adoptedRetained;
  q.markReplayDone();
}

void testWorkerStartFailureRollsBack() {
  using namespace dxmt9::d3d9;
  D9CDevice device(nullptr);
  ReplayOffloadWorker worker;
  worker.failNextStartForTest();
  check(!worker.start(&device),
        "injected thread startup failure is reported");
  check(!worker.startedForTest(),
        "failed thread startup leaves no owner or joinable thread");
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

void testAcceptedPushPublishesLedgerBeforePop() {
  using namespace dxmt9::d3d9;
  ReplayOffloadQueue q(4, 1 << 20);
  ReplayDrainLedger ledger;
  ReplayDrainTarget target;
  auto chunk = makeChunk(8);
  chunk.ledgerTargets.push_back(&target);
  check(q.push(std::move(chunk), &ledger), "ledger push accepted");
  check(target.lastQueuedSeq == 1 && target.lastReplayedSeq == 0,
        "accepted push publishes queued watermark");

  RawCommandChunk out;
  check(q.pop(out), "published entry pops");
  check(out.replaySeq == 1, "owned entry carries published replay sequence");
  ledger.publishReplayed(out);
  q.markReplayDone();
  check(target.lastReplayedSeq == 1, "replay completion catches target up");
}

void testConcurrentPopSeesPublishedMonotonicSequences() {
  using namespace dxmt9::d3d9;
  ReplayOffloadQueue q(4, 1 << 20);
  ReplayDrainLedger ledger;
  ReplayDrainTarget target;
  std::array<ReplaySeq, 2> poppedSequences{};
  std::barrier start{2};
  std::thread consumer([&] {
    start.arrive_and_wait();
    for (std::size_t index = 0; index < poppedSequences.size(); ++index) {
      RawCommandChunk out;
      check(q.pop(out), "concurrent consumer pops accepted entry");
      poppedSequences[index] = out.replaySeq;
      check(out.replaySeq == index + 1u,
            "consumer cannot pop before replay-sequence publication");
      ledger.publishReplayed(out);
      q.markReplayDone();
    }
  });
  start.arrive_and_wait();
  for (int i = 0; i < 2; ++i) {
    auto chunk = makeChunk(8);
    chunk.ledgerTargets.push_back(&target);
    check(q.push(std::move(chunk), &ledger),
          "concurrent producer accepts sequenced entry");
  }
  consumer.join();
  check(poppedSequences[0] == 1 && poppedSequences[1] == 2,
        "accepted entries receive monotonic FIFO replay sequences");
  check(target.lastQueuedSeq == 2 && target.lastReplayedSeq == 2,
        "multi-chunk target finishes at the latest monotonic sequence");
}

void testLedgerRefusalDoesNotMoveOrPublish() {
  using namespace dxmt9::d3d9;
  ReplayOffloadQueue q(4, 1 << 20);
  ReplayDrainLedger ledger;
  ReplayDrainTarget target;
  ledger.stop();
  auto chunk = makeChunk(8);
  chunk.recordBlob.assign({1, 2, 3, 4, 5, 6, 7, 8});
  chunk.ledgerTargets.push_back(&target);
  check(!q.push(std::move(chunk), &ledger), "stopped ledger refuses push");
  check(chunk.recordBlob.size() == 8 && chunk.replaySeq == 0,
        "ledger refusal leaves caller entry unmoved and unsequenced");
  check(target.lastQueuedSeq == 0 && target.lastReplayedSeq == 0,
        "ledger refusal leaks no watermark");
  check(q.depth() == 0, "ledger-refused entry never enters queue");
}

void testScopedWaitIsResourceLocal() {
  using namespace dxmt9::d3d9;
  ReplayOffloadQueue q(4, 1 << 20);
  ReplayDrainLedger ledger;
  ReplayDrainTarget targetA;
  ReplayDrainTarget targetB;
  auto chunk = makeChunk(8);
  chunk.ledgerTargets.push_back(&targetA);
  check(q.push(std::move(chunk), &ledger), "resource A push");

  check(ledger.wait(targetB) == ReplayDrainResult::CaughtUp,
        "unrelated resource does not wait");
  std::atomic<bool> returned{false};
  std::barrier start{2};
  std::thread waiter([&] {
    start.arrive_and_wait();
    check(ledger.wait(targetA) == ReplayDrainResult::CaughtUp,
          "resource A wait catches up");
    returned.store(true, std::memory_order_release);
  });
  start.arrive_and_wait();

  RawCommandChunk out;
  check(q.pop(out), "resource A pop");
  ledger.publishReplayed(out);
  q.markReplayDone();
  waiter.join();
  check(returned.load(std::memory_order_acquire),
        "target wait wakes after its replay watermark");
}

void testCoreBufferAliasesShareLedgerTarget() {
  using namespace dxmt9::d3d9;
  ReplayDrainLedger ledger;
  auto aliasA = ledger.targetForCoreBuffer(0x1234u);
  auto aliasB = ledger.targetForCoreBuffer(0x1234u);
  auto unrelated = ledger.targetForCoreBuffer(0x5678u);
  check(aliasA == aliasB,
        "one core buffer identity owns one shared ledger target");
  check(aliasA != unrelated,
        "different core buffers keep independent ledger targets");

  RawCommandChunk chunk = makeChunk(8);
  chunk.ledgerTargets.push_back(aliasA.get());
  check(ledger.publishInline(chunk), "alias fixture publishes through A");
  check(ledger.pending(*aliasB),
        "alias B observes work admitted through alias A");
  check(!ledger.pending(*unrelated),
        "unrelated core identity remains clear");
  ledger.publishReplayed(chunk);
  check(!ledger.pending(*aliasB),
        "replay completion catches every alias up together");
}

void testCoreBufferTargetCacheReclaimsExpiredGenerations() {
  using namespace dxmt9::d3d9;
  ReplayDrainLedger ledger;
  auto liveA = ledger.targetForCoreBuffer(0x100000001ull);
  auto liveAlias = ledger.targetForCoreBuffer(0x100000001ull);
  check(liveA == liveAlias, "live aliases share one target before sweeps");

  for (std::uint64_t generation = 2u; generation < 2u + 3u * 64u;
       ++generation) {
    auto transient = ledger.targetForCoreBuffer(
        (generation << 32u) | 1u);
    check(transient != liveA,
          "new handle generation never aliases the live old generation");
  }

  const auto cacheSize = ReplayDrainLedgerTestAccess::cacheSize(ledger);
  const auto nextSweep = ReplayDrainLedgerTestAccess::nextSweep(ledger);
  check(cacheSize < nextSweep,
        "expired target nodes stay below the next amortized sweep threshold");
  check(cacheSize <= 64u,
        "continued generation churn reclaims expired weak-cache nodes");
  check(ledger.targetForCoreBuffer(0x100000001ull) == liveA,
        "sweeping preserves the canonical live alias target");
}

void testDiscardBypassRequiresRuntimeRenameCapability() {
  using namespace dxmt9::d3d9;
  D9CBufferDesc desc{};
  desc.pool = 0u;
  desc.usage = 0x00000200u;
  constexpr std::uint32_t discard = 0x00002000u;
  constexpr std::uint32_t noOverwrite = 0x00001000u;
  check(bufferLockClassBypassesReplay(desc, discard, true),
        "DEFAULT+DYNAMIC DISCARD bypasses when runtime rename is enabled");
  check(!bufferLockClassBypassesReplay(desc, discard, false),
        "rename-disabled DISCARD falls back to the scoped ledger wait");
  check(bufferLockClassBypassesReplay(desc, noOverwrite, false),
        "NOOVERWRITE keeps its caller-owned bypass without rename support");
}

void testGlobalDrainReportsTerminalOutcome() {
  using namespace dxmt9::d3d9;
  for (const bool poison : {false, true}) {
    D9CDevice device(nullptr);
    if (poison) {
      device.replayDrainLedger.poison();
    } else {
      device.replayDrainLedger.stop();
    }
    check(!drainDeferredReplay(&device, "terminal-global-drain"),
          "global drain rejects provider entry after stop or poison");
    check(dxmt9c_device_reset(&device, nullptr) ==
              dxmt9::core::D3DERR_DEVICELOST,
          "Reset propagates terminal replay outcome without provider entry");
    check(dxmt9c_device_present(&device, nullptr, nullptr, 0u, nullptr, 0u) ==
              dxmt9::core::D3DERR_DEVICELOST,
          "Present propagates terminal replay outcome without provider entry");
    check(dxmt9c_device_begin_scene(&device) ==
              dxmt9::core::D3DERR_DEVICELOST,
          "BeginScene keeps its no-drain lane but rejects terminal replay");
    check(dxmt9c_device_end_scene(&device) ==
              dxmt9::core::D3DERR_DEVICELOST,
          "EndScene keeps its no-drain lane but rejects terminal replay");
  }
}

void testNoDrainProviderEntrySpiesRejectTerminal() {
  for (const bool poison : {false, true}) {
    D9CDevice device(nullptr);
    D9CShader shader(&device);
    shader.bytecodeWords = {0xfffe0101u, 0x0000ffffu};
    D9CVertexDecl declaration(&device);
    declaration.raw.push_back({0xffu, 0u, 17u, 0u, 0u, 0u});
    D9CTexture texture(std::shared_ptr<dxmt9::core::Texture>{}, &device);
    D9CBuffer buffer(std::shared_ptr<dxmt9::core::Buffer>{}, &device);
    buffer.desc.size = 0x11223344u;
    D9CSurface surface(std::shared_ptr<dxmt9::core::Surface>{},
                       &texture, 0u, &device);
    D9CSwapChain swapchain(nullptr);
    swapchain.owner = &device;
    D9CQuery query(std::shared_ptr<dxmt9::core::Query>{}, &device);

    if (poison) {
      device.replayDrainLedger.poison();
    } else {
      device.replayDrainLedger.stop();
    }

    constexpr std::uint32_t kSentinel = 0xa5a5a5a5u;
    const std::uint32_t shaderBytecode[] = {0xfffe0101u, 0x0000ffffu};
    check(dxmt9c_device_create_vertex_shader(&device, shaderBytecode) == nullptr,
          "terminal no-drain shader creation does not enter provider");
    std::uint32_t shaderBytes = kSentinel;
    check(dxmt9c_shader_get_bytecode(&shader, nullptr, &shaderBytes) ==
              dxmt9::core::D3DERR_DEVICELOST &&
              shaderBytes == kSentinel,
          "terminal shader metadata leaves provider output sentinel untouched");
    std::uint32_t declarationCount = kSentinel;
    check(dxmt9c_vdecl_get_declaration(
              &declaration, nullptr, &declarationCount) ==
              dxmt9::core::D3DERR_DEVICELOST &&
              declarationCount == kSentinel,
          "terminal vdecl metadata leaves provider output sentinel untouched");

    check(dxmt9c_texture_get_surface_level(&texture, 0u) == nullptr &&
              dxmt9c_texture_get_level_count(&texture) == 0u,
          "terminal texture getters do not enter provider");
    D9CBufferDesc bufferDesc{};
    bufferDesc.size = kSentinel;
    check(dxmt9c_buffer_get_desc(&buffer, &bufferDesc) ==
              dxmt9::core::D3DERR_DEVICELOST &&
              bufferDesc.size == kSentinel,
          "terminal buffer metadata leaves provider output sentinel untouched");
    check(dxmt9c_surface_get_container_texture(&surface) == nullptr,
          "terminal surface getter does not return its cached provider object");

    check(dxmt9c_swapchain_get_back_buffer(&swapchain, 0u, 0u) == nullptr,
          "terminal swapchain getter does not enter provider");
    check(dxmt9c_query_get_data_size(&query) == 0u &&
              dxmt9c_query_get_type(&query) == 0u,
          "terminal query metadata getters do not enter provider");

    const auto refsBefore = shader.refs.load();
    dxmt9c_shader_addref(&shader);
    check(shader.refs.load() == refsBefore + 1u &&
              dxmt9c_shader_release(&shader) == refsBefore,
          "lifetime-only addref/release remain reachable after terminal");
  }
}

struct FailureBarrierContext {
  std::barrier<> published{2};
  std::barrier<> release{2};
};

int32_t injectedReplayFailure(D9CDevice*,
                              dxmt9::d3d9::RawCommandChunk&) {
  return dxmt9::core::D3DERR_INVALIDCALL;
}

int32_t injectedReplayAllocationException(
    D9CDevice*, dxmt9::d3d9::RawCommandChunk&) {
  throw std::bad_alloc();
}

int32_t injectedReplayException(
    D9CDevice*, dxmt9::d3d9::RawCommandChunk&) {
  throw std::runtime_error("injected replay exception");
}

void waitAtPublishedFailure(void* opaque) {
  auto& context = *static_cast<FailureBarrierContext*>(opaque);
  context.published.arrive_and_wait();
  context.release.arrive_and_wait();
}

void testActualReplayFailurePublishesTerminalBeforeCompletion() {
  using namespace dxmt9::d3d9;
  D9CDevice device(nullptr);
  ReplayDrainTarget failedTarget;
  FailureBarrierContext barriers;
  device.replayOffload = std::make_unique<ReplayOffloadWorker>(
      injectedReplayFailure, waitAtPublishedFailure, &barriers);
  device.replayOffload->start(&device);

  auto failedChunk = makeChunk(8);
  failedChunk.ledgerTargets.push_back(&failedTarget);
  check(device.replayOffload->queue().push(
            std::move(failedChunk), &device.replayDrainLedger),
        "failure fixture enqueues through production admission");
  barriers.published.arrive_and_wait();

  check(device.replayOffload->failed(),
        "negative replay publishes worker failure before completion");
  check(device.replayDrainLedger.poisoned(),
        "negative replay poisons the device ledger before completion");
  check(device.replayOffload->queue().stopped(),
        "negative replay stops queue admission before completion");
  check(device.replayOffload->queue().depth() == 1u,
        "failure barrier is reached while the failed chunk remains in flight");
  check(!drainDeferredReplay(&device, "injected-replay-failure"),
        "global drain cannot report success after replay failure publication");

  ReplayDrainTarget refusedTarget;
  auto refusedChunk = makeChunk(8);
  refusedChunk.ledgerTargets.push_back(&refusedTarget);
  check(!device.replayOffload->queue().push(
            std::move(refusedChunk), &device.replayDrainLedger),
        "enqueue after replay failure publication is refused");
  check(refusedChunk.replaySeq == 0u && refusedChunk.recordBlob.size() == 8u,
        "failure-refused enqueue remains unmoved and unsequenced");
  check(refusedTarget.lastQueuedSeq == 0u,
        "failure-refused enqueue publishes no target watermark");

  barriers.release.arrive_and_wait();
  device.replayOffload->stop();
  check(device.replayOffload->queue().depth() == 0u,
        "failed chunk completion is published only after terminal barrier");
  check(failedTarget.lastReplayedSeq == 0u,
        "failed replay never advances its target replay watermark");
}

void testThrowingReplaySettlesEveryOwnedWrapperExactlyOnce() {
  using namespace dxmt9::d3d9;
  constexpr std::array<ReplayOffloadWorker::ReplayFn, 2> throwingReplay{
      injectedReplayAllocationException, injectedReplayException};
  for (const auto replay : throwingReplay) {
    D9CDevice device(nullptr);
    ReplayDrainTarget failedTarget;
    ReplayDrainTarget queuedTarget;
    FailureBarrierContext barriers;
    device.replayOffload = std::make_unique<ReplayOffloadWorker>(
        replay, waitAtPublishedFailure, &barriers);
    check(device.replayOffload->start(&device),
          "throwing replay worker starts");

    auto* retained = new D9CBuffer(nullptr);
    retained->refs.fetch_add(2u);
    auto failedChunk = makeChunk(8);
    failedChunk.ledgerTargets.push_back(&failedTarget);
    failedChunk.retainedWrappers.push_back({
        .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .ptr = retained});
    auto queuedChunk = makeChunk(8);
    queuedChunk.ledgerTargets.push_back(&queuedTarget);
    queuedChunk.retainedWrappers.push_back({
        .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .ptr = retained});
    check(device.replayOffload->queue().push(
              std::move(failedChunk), &device.replayDrainLedger) &&
              device.replayOffload->queue().push(
                  std::move(queuedChunk), &device.replayDrainLedger),
          "throwing replay fixture owns one retain per queued chunk");

    barriers.published.arrive_and_wait();
    check(device.replayOffload->failed() &&
              device.replayDrainLedger.poisoned() &&
              retained->refs.load() == 3u,
          "exception is contained and terminal state precedes ownership release");
    barriers.release.arrive_and_wait();
    device.replayOffload->stop();
    check(retained->refs.load() == 1u &&
              failedTarget.lastReplayedSeq == 0u &&
              queuedTarget.lastReplayedSeq == 0u &&
              device.replayOffload->queue().depth() == 0u,
          "failed and unreplayed queued chunks each release once without ledger catch-up");
    delete retained;
  }
}

void testLedgerTerminalStatesWakeWithoutSuccess() {
  using namespace dxmt9::d3d9;
  for (const bool poison : {false, true}) {
    ReplayDrainLedger ledger;
    ReplayDrainTarget target;
    RawCommandChunk chunk = makeChunk(8);
    chunk.ledgerTargets.push_back(&target);
    check(ledger.publishInline(chunk), "inline ledger publish");
    std::atomic<ReplayDrainResult> result{ReplayDrainResult::CaughtUp};
    std::barrier start{2};
    std::thread waiter([&] {
      start.arrive_and_wait();
      result.store(ledger.wait(target));
    });
    start.arrive_and_wait();
    if (poison) {
      ledger.poison();
    } else {
      ledger.stop();
    }
    waiter.join();
    check(result.load() == (poison ? ReplayDrainResult::Poisoned
                                  : ReplayDrainResult::Stopped),
          "terminal ledger wake must not report catch-up");
    check(target.lastReplayedSeq == 0,
          "terminal wake does not acknowledge unreplayed work");
  }
}

void testBackpressuredStopLeaksNoSecondWatermark() {
  using namespace dxmt9::d3d9;
  ReplayOffloadQueue q(1, 1 << 20);
  ReplayDrainLedger ledger;
  ReplayDrainTarget first;
  ReplayDrainTarget second;
  auto firstChunk = makeChunk(8);
  firstChunk.ledgerTargets.push_back(&first);
  check(q.push(std::move(firstChunk), &ledger), "first push fills queue");
  auto secondChunk = makeChunk(8);
  secondChunk.ledgerTargets.push_back(&second);
  std::atomic<bool> pushed{true};
  std::barrier start{2};
  std::thread producer([&] {
    start.arrive_and_wait();
    pushed.store(q.push(std::move(secondChunk), &ledger));
  });
  start.arrive_and_wait();
  q.stop();
  ledger.stop();
  producer.join();
  check(!pushed.load(), "backpressured push is refused by stop");
  check(second.lastQueuedSeq == 0 && second.lastReplayedSeq == 0,
        "backpressured refusal publishes no second watermark");
  check(secondChunk.replaySeq == 0 && secondChunk.recordBlob.size() == 8,
        "backpressured refusal leaves second entry unmoved");
}

void testCleanWorkerStopAndDestructorWakeScopedWaiters() {
  using namespace dxmt9::d3d9;
  for (const bool explicitDoubleStop : {false, true}) {
    D9CDevice device(nullptr);
    ReplayDrainTarget target;
    RawCommandChunk chunk = makeChunk(8);
    chunk.ledgerTargets.push_back(&target);
    check(device.replayDrainLedger.publishInline(chunk),
          "clean-stop fixture publishes pending target");
    auto worker = std::make_unique<ReplayOffloadWorker>();
    worker->start(&device);
    std::atomic<ReplayDrainResult> result{ReplayDrainResult::CaughtUp};
    std::barrier start{2};
    std::thread waiter([&] {
      start.arrive_and_wait();
      result.store(device.replayDrainLedger.wait(target));
    });
    start.arrive_and_wait();
    if (explicitDoubleStop) {
      worker->stop();
      worker->stop();
    }
    worker.reset();
    waiter.join();
    check(result.load() == ReplayDrainResult::Stopped,
          "clean worker stop/destructor wakes without false catch-up");
    check(target.lastReplayedSeq == 0,
          "clean worker stop never acknowledges pending replay");
  }
}

void testCleanStopWithInflightEntryWakesScopedWaiter() {
  using namespace dxmt9::d3d9;
  ReplayOffloadQueue q(4, 1 << 20);
  ReplayDrainLedger ledger;
  ReplayDrainTarget target;
  auto chunk = makeChunk(8);
  chunk.ledgerTargets.push_back(&target);
  check(q.push(std::move(chunk), &ledger), "in-flight stop push");
  RawCommandChunk inFlight;
  check(q.pop(inFlight), "in-flight stop pop");
  std::atomic<ReplayDrainResult> result{ReplayDrainResult::CaughtUp};
  std::barrier start{2};
  std::thread waiter([&] {
    start.arrive_and_wait();
    result.store(ledger.wait(target));
  });
  start.arrive_and_wait();
  q.stop();
  ledger.stop();
  waiter.join();
  check(result.load() == ReplayDrainResult::Stopped,
        "stop with in-flight entry wakes scoped waiter as stopped");
  check(target.lastReplayedSeq == 0,
        "in-flight clean stop does not publish replay completion");
}

void testTerminalScopedFenceRejectsProviderCall() {
  using namespace dxmt9::d3d9;
  for (const bool poison : {false, true}) {
    for (const std::uint32_t flags : {0u, 0x00001000u, 0x00002000u}) {
      D9CDevice device(nullptr);
      D9CBuffer buffer(nullptr, &device);
      buffer.desc.pool = 0u;
      buffer.desc.usage = 0x00000200u;
      buffer.lastLockSucceeded = true;
      buffer.lastLockFlags = flags;
      RawCommandChunk chunk = makeChunk(8);
      chunk.ledgerTargets.push_back(buffer.replayDrainTarget.get());
      check(device.replayDrainLedger.publishInline(chunk),
            "terminal-fence pending publish");
      if (poison) {
        device.replayDrainLedger.poison();
      } else {
        device.replayDrainLedger.stop();
      }
      check(!drainDeferredReplayForBufferLock(&buffer, flags),
            "terminal lock fence rejects provider call");
      check(!drainDeferredReplayForBufferUnlock(&buffer),
            "terminal unlock fence rejects provider call");
    }
  }
}
}  // namespace

int main() {
  try {
    testFifoPushPop();
    testDrainWaitsForInFlight();
    testBoundedPushBlocksUntilPop();
    testStopReleasesEverything();
    testFailureDispositionAndByteAdoption();
    testWorkerStartFailureRollsBack();
    testWaitDrainedIsStopAware();
    testOversizedChunkAdmittedWhenQueueEmpty();
    testPopDrainsQueuedItemsAfterStop();
    testPushDoesNotConsumeChunkOnStopFailure();
    testAcceptedPushPublishesLedgerBeforePop();
    testConcurrentPopSeesPublishedMonotonicSequences();
    testLedgerRefusalDoesNotMoveOrPublish();
    testScopedWaitIsResourceLocal();
    testCoreBufferAliasesShareLedgerTarget();
    testCoreBufferTargetCacheReclaimsExpiredGenerations();
    testDiscardBypassRequiresRuntimeRenameCapability();
    testGlobalDrainReportsTerminalOutcome();
    testNoDrainProviderEntrySpiesRejectTerminal();
    testActualReplayFailurePublishesTerminalBeforeCompletion();
    testThrowingReplaySettlesEveryOwnedWrapperExactlyOnce();
    testLedgerTerminalStatesWakeWithoutSuccess();
    testBackpressuredStopLeaksNoSecondWatermark();
    testCleanWorkerStopAndDestructorWakeScopedWaiters();
    testCleanStopWithInflightEntryWakesScopedWaiter();
    testTerminalScopedFenceRejectsProviderCall();
  } catch (const TestFailure& e) {
    std::cerr << "replay_offload_queue_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "replay_offload_queue_spec passed\n";
  return 0;
}
