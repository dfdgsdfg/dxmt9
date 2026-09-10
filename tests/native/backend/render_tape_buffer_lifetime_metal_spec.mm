// R-BACK-5.8 / R-BACK-43.4 — real Metal backing lifetime oracle.
//
// This fixture exercises Pool's actual Shared MTLBuffer allocation and
// rename-ring path without submitting a command buffer. It deliberately
// proves CPU-visible backing identity, contents, revision, and residency
// leases only; it is not a GPU execution or pixel oracle.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../../../src/d3d9/device_c_render_tape_completion.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/winemetal/Metal.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view message) {
  throw TestFailure(std::string(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

using dxmt9::core::BufferDesc;
using dxmt9::core::Handle;
using dxmt9::core::Pool;
using dxmt9::core::UsageDynamic;
using dxmt9::core::UsageDiscard;
using dxmt9::d3d9::RenderTapeCompletionAction;
using dxmt9::d3d9::RenderTapeCompletionKey;
using dxmt9::d3d9::RenderTapeCompletionSchedule;
using ResourcePool = dxmt9::resources::Pool;

id<MTLBuffer> metalBuffer(std::uint64_t handle) {
  return (id<MTLBuffer>)(uintptr_t)handle;
}

std::vector<std::uint8_t> readBytes(std::uint64_t handle, std::size_t size) {
  id<MTLBuffer> buffer = metalBuffer(handle);
  check(buffer != nil, "captured backing must be a valid MTLBuffer");
  void* contents = [buffer contents];
  check(contents != nullptr, "Shared backing must expose CPU contents");
  const auto* bytes = static_cast<const std::uint8_t*>(contents);
  return {bytes, bytes + size};
}

bool ringResident(const ResourcePool& pool, Handle handle,
                  std::uint64_t metalHandle) {
  const auto* record = pool.findBuffer(handle.value);
  if (!record) return false;
  for (const auto& entry : record->renameRing) {
    if (entry.buffer && entry.buffer.handle == metalHandle) {
      return entry.replayResident();
    }
  }
  return false;
}

std::uint64_t activeMetalHandle(const ResourcePool& pool, Handle handle) {
  const auto* record = pool.findBuffer(handle.value);
  check(record != nullptr, "buffer record must remain live");
  check(record->renameActiveIndex < record->renameRing.size(),
        "active rename index must be in range");
  const auto& entry = record->renameRing[record->renameActiveIndex];
  check(static_cast<bool>(entry.buffer),
        "active rename entry must hold a valid MTLBuffer");
  return entry.buffer.handle;
}

void runLifetimeOracle(id<MTLDevice> metalDevice) {
  const WMT::Device device{
      static_cast<obj_handle_t>(reinterpret_cast<uintptr_t>(metalDevice))};
  ResourcePool pool;

  constexpr std::size_t kBytes = 64u;
  BufferDesc desc{};
  desc.size = kBytes;
  desc.pool = Pool::Default;
  desc.usage = UsageDynamic;

  const auto handle = pool.createBuffer(device, desc);
  check(static_cast<bool>(handle),
        "Pool must allocate the dynamic buffer on the real device");
  check(pool.bufferHasVersionedBacking(handle.value),
        "DEFAULT+DYNAMIC buffer must carry a rename ring");

  const std::vector<std::uint8_t> bytesA(kBytes, 0xa1u);
  const std::vector<std::uint8_t> bytesB(kBytes, 0xb2u);
  const std::vector<std::uint8_t> bytesD(kBytes, 0xd4u);
  check(pool.uploadBufferData(device, handle.value, bytesA.data(), bytesA.size(),
                              /*completedSeqId=*/0u),
        "initial Pool upload must resolve the buffer");

  auto capturedA = pool.captureChunkBufferBinding(handle);
  check(capturedA.requiresCapturedBacking,
        "versioned backing must require a commit-time capture");
  check(capturedA.snapshot.valid(), "capture A must contain a real MTL handle");
  check(capturedA.snapshot.contentRevision != 0u,
        "capture A must carry its content revision");
  check(readBytes(capturedA.snapshot.metalHandle, kBytes) == bytesA,
        "capture A must expose the uploaded bytes");
  check(ringResident(pool, handle, capturedA.snapshot.metalHandle),
        "capture A must hold a replay residency lease");

  pool.markBufferSnapshotUse(handle, capturedA.snapshot, /*seqId=*/1u);

  RenderTapeCompletionSchedule schedule;
  const RenderTapeCompletionAction actionA{
      RenderTapeCompletionKey{.iteration = 1u,
                              .submission = 1u,
                              .source = 17u,
                              .actionOrdinal = 0u,
                              .sourceGeneration = 3u}};
  const RenderTapeCompletionAction actionB{
      RenderTapeCompletionKey{.iteration = 1u,
                              .submission = 2u,
                              .source = 17u,
                              .actionOrdinal = 1u,
                              .sourceGeneration = 3u}};
  // These source/generation values are qualified reducer model keys, not
  // claims about an actual queue source identity; this fixture drives Pool
  // directly and submits no queue source.
  const RenderTapeCompletionAction actionC{
      RenderTapeCompletionKey{.iteration = 1u,
                              .submission = 3u,
                              .source = 17u,
                              .actionOrdinal = 2u,
                              .sourceGeneration = 3u}};
  check(schedule.append(actionA).accepted(), "completion action A must append");
  check(schedule.append(actionB).accepted(), "completion action B must append");
  check(schedule.append(actionC).accepted(), "completion action C must append");
  check(schedule.beginIteration(1u).accepted(),
        "completion iteration must begin");
  check(schedule.submit(actionA.key).accepted(), "submission A must admit");

  // A is now captured and in flight. DISCARD must not overwrite it, even
  // though the completion watermark is still zero.
  check(pool.finalizeBufferMap(device, handle, UsageDiscard,
                               /*completedSeqId=*/0u) ==
            pool.findBuffer(handle.value)->contents,
        "DISCARD map must expose the newly active shared backing");
  const auto backingB = activeMetalHandle(pool, handle);
  check(backingB != capturedA.snapshot.metalHandle,
        "in-flight captured A must rotate to a distinct backing B");
  check(pool.uploadBufferData(device, handle.value, bytesB.data(), bytesB.size(),
                             /*completedSeqId=*/0u),
        "upload B must resolve the rotated buffer");
  auto capturedB = pool.captureChunkBufferBinding(handle);
  check(capturedB.snapshot.valid(), "capture B must contain a real MTL handle");
  check(capturedB.snapshot.metalHandle == backingB,
        "capture B must name the active backing");
  check(capturedB.snapshot.contentRevision > capturedA.snapshot.contentRevision,
        "mutation B must advance content revision");
  check(readBytes(capturedB.snapshot.metalHandle, kBytes) == bytesB,
        "capture B must expose bytes distinct from capture A");
  check(readBytes(capturedA.snapshot.metalHandle, kBytes) == bytesA,
        "rotating B must preserve captured A contents");
  pool.markBufferSnapshotUse(handle, capturedB.snapshot, /*seqId=*/2u);
  check(schedule.submit(actionB.key).accepted(), "submission B must admit");

  // Completion advances the shared reducer and the logical pool waterline,
  // but it does not release the captured backing lease held by capturedA.
  check(schedule.release(actionA.key, [&] {
          pool.reclaimCompleted(/*completedSeqId=*/1u);
          return true;
        }).accepted(),
        "completion A must release in order");
  check(ringResident(pool, handle, capturedA.snapshot.metalHandle),
        "completion alone must not unpin captured A");

  const auto beforeGrowth = pool.findBuffer(handle.value)->renameRing.size();
  (void)pool.finalizeBufferMap(device, handle, UsageDiscard,
                               /*completedSeqId=*/1u);
  check(pool.findBuffer(handle.value)->renameRing.size() == beforeGrowth + 1u,
        "pinned A and in-flight B must force a fresh C backing");

  // The binding snapshot owns the only external residency reference. Releasing
  // it is the explicit capture-drain event; no completion callback may fake it.
  capturedA.backingResidency.reset();
  check(!ringResident(pool, handle, capturedA.snapshot.metalHandle),
        "released capture A must no longer hold residency");

  const auto beforeReuse = pool.findBuffer(handle.value)->renameRing.size();
  pool.markBufferUse(handle, /*seqId=*/3u);
  (void)pool.finalizeBufferMap(device, handle, UsageDiscard,
                               /*completedSeqId=*/1u);
  check(pool.findBuffer(handle.value)->renameRing.size() == beforeReuse,
        "released capture A must permit ring reuse");
  check(activeMetalHandle(pool, handle) == capturedA.snapshot.metalHandle,
        "released capture A must be selected as the reusable backing");
  check(pool.uploadBufferData(device, handle.value, bytesD.data(), bytesD.size(),
                             /*completedSeqId=*/1u),
        "reused A upload must resolve the buffer");
  check(readBytes(capturedA.snapshot.metalHandle, kBytes) == bytesD,
        "reused A backing must contain the new bytes");
  check(schedule.submit(actionC.key).accepted(), "submission C must admit");

  check(schedule.release(actionB.key, [&] {
          pool.reclaimCompleted(/*completedSeqId=*/2u);
          capturedB.backingResidency.reset();
          return true;
        }).accepted(),
        "completion B must release in order");

  check(pool.markBufferDestroyAndGc(handle.value, /*completedSeqId=*/2u),
        "destroy must mark the logical buffer pending");
  check(pool.findBuffer(handle.value) != nullptr,
        "destroy before the last use watermark must retain the record");
  check(schedule.release(actionC.key, [&] {
          pool.reclaimCompleted(/*completedSeqId=*/3u);
          return true;
        }).accepted(),
        "completion C must release in order");
  check(schedule.finishIteration().accepted(),
        "completion schedule must finish after all three releases");
  check(pool.findBuffer(handle.value) == nullptr,
        "completed destroy must reclaim the logical buffer and ring");
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::cerr << "SKIP: no Metal device\n";
      return 77;
    }
    try {
      runLifetimeOracle(device);
    } catch (const TestFailure& failure) {
      std::cerr << "render_tape_buffer_lifetime_metal_spec failed: "
                << failure.what() << '\n';
      return 1;
    } catch (const std::exception& failure) {
      std::cerr << "render_tape_buffer_lifetime_metal_spec unexpected exception: "
                << failure.what() << '\n';
      return 1;
    }
  }
  return 0;
}
