// R-BACK-5.8 — D3DUSAGE_DYNAMIC rename ring behavior.
//
// This is a CPU-only spec for the per-buffer-handle rename ring carried
// on `BufferRecord` when a buffer is created as `D3DPOOL_DEFAULT` +
// `D3DUSAGE_DYNAMIC`. Draw submissions now snapshot the concrete Metal backing
// for dynamic buffers, so default DISCARD can rotate the ring instead of
// waiting for the logical BufferHandle to drain. This spec exercises:
//
//   * Create-time tagging (`isDynamicRename`) and ring seeding with the
//     create-time allocation entry.
//   * D3DLOCK_DISCARD rotation reusing an idle ring entry whose
//     lastUsedSeqId is at or below the GPU completion watermark.
//   * D3DLOCK_DISCARD growing the ring with a fresh allocation when no
//     idle entry exists rather than blocking on prior GPU completion.
//   * Ring capacity never shrinks during a session.
//
// The spec runs without a Metal device — `WMT::Device{NULL_OBJECT_HANDLE}`
// makes `MTLDevice_newBuffer` return NULL_OBJECT_HANDLE, so the ring's
// underlying `WMT::Reference<WMT::Buffer>` entries are empty handles.
// This is enough to verify the ring's structural shape: count, active
// index, and lastUsedSeqId watermarks per entry. Live shared-mode
// allocation behavior is covered by the runtime integration suite via
// `shader_runner_dxmt9` once a real device is available.
//
// Pairs with R-BACK-5.7 storage-mode mapping (DEFAULT+DYNAMIC →
// MTLStorageModeShared), already enforced at create time in
// `Pool::createBuffer`.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/winemetal/winemetal.h"

namespace {

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
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

dxmt9::core::BufferDesc dynamicBufferDesc(std::uint64_t size = 64u) {
  dxmt9::core::BufferDesc desc{};
  desc.size = size;
  desc.pool = dxmt9::core::Pool::Default;
  desc.usage = dxmt9::core::UsageDynamic;
  return desc;
}

dxmt9::core::BufferDesc staticBufferDesc(std::uint64_t size = 64u) {
  dxmt9::core::BufferDesc desc{};
  desc.size = size;
  desc.pool = dxmt9::core::Pool::Default;
  desc.usage = 0u;  // no UsageDynamic
  return desc;
}

void testDynamicCreateSeedsRingWithSingleEntry() {
  using namespace dxmt9::resources;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, dynamicBufferDesc());
  check(static_cast<bool>(handle), "createBuffer returns a valid handle");
  auto* record = resourcePool.findBuffer(handle.value);
  check(record != nullptr, "createBuffer record is reachable");
  check(record->isDynamicRename,
        "DEFAULT + UsageDynamic must select the rename-ring policy");
  checkEq(record->renameRing.size(), std::size_t{1u},
          "rename ring is seeded with the create-time allocation");
  checkEq(record->renameActiveIndex, 0u,
          "create-time entry is the initial active slot");
}

void testNonDynamicSkipsRingTagging() {
  using namespace dxmt9::resources;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, staticBufferDesc());
  auto* record = resourcePool.findBuffer(handle.value);
  check(record != nullptr, "createBuffer returns a record for static buffers");
  check(!record->isDynamicRename,
        "non-dynamic DEFAULT buffer must not arm the rename ring");
  check(record->renameRing.empty(),
        "non-dynamic buffer carries no rename-ring spares");
}

void testDiscardWithoutInflightUseRotatesInPlace() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, dynamicBufferDesc());
  // No use at all — completedSeqId is 0 and the only ring entry is
  // already idle. The DISCARD path must not grow the ring just
  // because there is no other slot to rotate into.
  resourcePool.finalizeBufferMap(dev, handle, UsageDiscard, /*completedSeqId=*/0u);
  auto* record = resourcePool.findBuffer(handle.value);
  check(record != nullptr, "record survives a DISCARD lock");
  checkEq(record->renameRing.size(), std::size_t{1u},
          "DISCARD on an already-idle solo entry must not grow the ring");
  checkEq(record->renameActiveIndex, 0u,
          "DISCARD on a solo idle ring keeps the active slot at 0");
}

void testDiscardWithInflightUseGrowsTheRing() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, dynamicBufferDesc());
  // Stamp the active entry as in-flight at seqId = 5 and call DISCARD
  // with completedSeqId = 4 — the ring's only entry is still in use,
  // so a fresh allocation must be appended rather than waiting on GPU.
  resourcePool.markBufferUse(handle, /*seqId=*/5u);
  resourcePool.finalizeBufferMap(dev, handle, UsageDiscard, /*completedSeqId=*/4u);
  auto* record = resourcePool.findBuffer(handle.value);
  check(record != nullptr, "record survives a growth-rename");
  checkEq(record->renameRing.size(), std::size_t{2u},
          "DISCARD with no idle entry must grow the rename ring");
  checkEq(record->renameActiveIndex, 1u,
          "fresh allocation becomes the new active slot");
  checkEq(record->renameRing[0].lastUsedSeqId, std::uint64_t{5u},
          "rotated-out entry retains its last-used watermark");
  checkEq(record->renameRing[1].lastUsedSeqId, std::uint64_t{0u},
          "freshly appended entry starts idle (lastUsedSeqId == 0)");
  checkEq(record->lastUsedSeqId, std::uint64_t{0u},
          "active record watermark resets when rotating to a fresh entry");
}

void testDiscardReusesIdleEntryAcrossRing() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, dynamicBufferDesc());
  // Force a growth-rename: active(0) is in-flight at seqId 5, so
  // DISCARD must allocate a fresh entry as active(1).
  resourcePool.markBufferUse(handle, /*seqId=*/5u);
  resourcePool.finalizeBufferMap(dev, handle, UsageDiscard, /*completedSeqId=*/4u);
  auto* record = resourcePool.findBuffer(handle.value);
  checkEq(record->renameRing.size(), std::size_t{2u},
          "preconditions: ring grew to two entries");
  // Now stamp active(1) as in-flight at seqId 7. Then DISCARD with
  // completedSeqId == 6 — entry(0)'s watermark is 5 (idle), entry(1)
  // is 7 (in-flight). The rotation should reuse entry(0) instead of
  // growing the ring.
  resourcePool.markBufferUse(handle, /*seqId=*/7u);
  resourcePool.finalizeBufferMap(dev, handle, UsageDiscard, /*completedSeqId=*/6u);
  checkEq(record->renameRing.size(), std::size_t{2u},
          "DISCARD must reuse an idle entry rather than grow the ring");
  checkEq(record->renameActiveIndex, 0u,
          "rotation lands on the idle (entry-0) slot");
  checkEq(record->renameRing[1].lastUsedSeqId, std::uint64_t{7u},
          "newly-rotated-out entry stamps its own last-used watermark");
}

void testRingNeverShrinksOnSubsequentDiscards() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, dynamicBufferDesc());
  // Three forced growths: each DISCARD finds every existing entry
  // in-flight, so the ring must grow to four entries total (the
  // create-time seed plus three appends).
  resourcePool.markBufferUse(handle, /*seqId=*/1u);
  resourcePool.finalizeBufferMap(dev, handle, UsageDiscard, /*completedSeqId=*/0u);
  resourcePool.markBufferUse(handle, /*seqId=*/2u);
  resourcePool.finalizeBufferMap(dev, handle, UsageDiscard, /*completedSeqId=*/0u);
  resourcePool.markBufferUse(handle, /*seqId=*/3u);
  resourcePool.finalizeBufferMap(dev, handle, UsageDiscard, /*completedSeqId=*/0u);
  auto* record = resourcePool.findBuffer(handle.value);
  checkEq(record->renameRing.size(), std::size_t{4u},
          "three growths bring ring capacity to four entries");
  // A subsequent DISCARD with every entry retired must NOT shrink the
  // ring — the spec mandates grow-only behavior. The rotation simply
  // picks an idle slot and reuses it; the underlying vector keeps its
  // capacity.
  resourcePool.finalizeBufferMap(dev, handle, UsageDiscard, /*completedSeqId=*/100u);
  checkEq(record->renameRing.size(), std::size_t{4u},
          "rename ring must not shrink after every entry retires");
}

void testNonDiscardLockSkipsRotation() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, dynamicBufferDesc());
  resourcePool.markBufferUse(handle, /*seqId=*/5u);
  // Plain map (no DISCARD, no NoOverwrite) must not touch the ring —
  // the queue's wait-for-sequence path is what serializes the access.
  resourcePool.finalizeBufferMap(dev, handle, /*flags=*/0u, /*completedSeqId=*/0u);
  auto* record = resourcePool.findBuffer(handle.value);
  checkEq(record->renameRing.size(), std::size_t{1u},
          "non-DISCARD lock must not grow the rename ring");
  checkEq(record->renameActiveIndex, 0u,
          "non-DISCARD lock must not rotate the active slot");
}

}  // namespace

int main() {
  try {
    testDynamicCreateSeedsRingWithSingleEntry();
    testNonDynamicSkipsRingTagging();
    testDiscardWithoutInflightUseRotatesInPlace();
    testDiscardWithInflightUseGrowsTheRing();
    testDiscardReusesIdleEntryAcrossRing();
    testRingNeverShrinksOnSubsequentDiscards();
    testNonDiscardLockSkipsRotation();
  } catch (const TestFailure& e) {
    std::cerr << "dynamic_rename_ring_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "dynamic_rename_ring_spec unexpected exception: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
