// R-BACK-5.8 / 5.11 — DYNAMIC rename + MANAGED backing-version behavior.
//
// This is a CPU-only spec for the per-buffer-handle backing ring carried on
// `BufferRecord`. Draw submissions snapshot the concrete Metal backing so
// DEFAULT+DYNAMIC DISCARD and MANAGED writable unlock can rotate without
// waiting for the logical BufferHandle to drain. This spec exercises:
//
//   * Create-time tagging (`isDynamicRename`) and ring seeding with the
//     create-time allocation entry.
//   * D3DLOCK_DISCARD rotation reusing an idle ring entry whose
//     lastUsedSeqId is at or below the GPU completion watermark.
//   * D3DLOCK_DISCARD growing the ring with a fresh allocation when no
//     idle entry exists rather than blocking on prior GPU completion.
//   * Ring capacity never shrinks during a session.
//   * MANAGED maps never wait, rotation happens at upload rather than map,
//     and the complete CPU shadow is copied into the selected backing.
//   * Logical destruction keeps the aggregate sequence watermark while
//     backing entries use their own watermarks for reuse.
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

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

dxmt9::core::BufferDesc managedBufferDesc(std::uint64_t size = 64u) {
  dxmt9::core::BufferDesc desc{};
  desc.size = size;
  desc.pool = dxmt9::core::Pool::Managed;
  desc.usage = 0u;
  return desc;
}

struct SyntheticBufferHandleGuard {
  dxmt9::resources::BufferRecord* record = nullptr;

  ~SyntheticBufferHandleGuard() {
    if (!record) {
      return;
    }
    record->buffer.handle = NULL_OBJECT_HANDLE;
    for (auto& entry : record->renameRing) {
      entry.buffer.handle = NULL_OBJECT_HANDLE;
    }
  }
};

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
  checkEq(record->lastUsedSeqId, std::uint64_t{5u},
          "logical record keeps its aggregate destruction watermark");
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

void testConcreteSnapshotMarkProtectsRotatedBacking() {
  using namespace dxmt9::resources;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, dynamicBufferDesc());
  auto* record = resourcePool.findBuffer(handle.value);
  check(record != nullptr, "dynamic buffer is reachable");
  checkEq(record->renameRing.size(), std::size_t{1u},
          "dynamic buffer seeds one rename entry");

  // A NULL device cannot allocate real Metal buffers. Add a second empty ring
  // entry, then install synthetic non-owning handles so the production
  // snapshot matcher can distinguish them. The guard clears the handles
  // before their destructors can release them.
  record->renameRing.emplace_back();
  SyntheticBufferHandleGuard handleGuard{record};
  constexpr obj_handle_t firstMetalHandle = 0x1234u;
  constexpr obj_handle_t secondMetalHandle = 0x5678u;
  record->renameRing[0].buffer.handle = firstMetalHandle;
  record->renameRing[1].buffer.handle = secondMetalHandle;
  dxmt9::core::DrawBufferBindingSnapshot snapshot{};
  snapshot.metalHandle = secondMetalHandle;

  resourcePool.markBufferSnapshotUse(handle, snapshot, /*seqId=*/7u);
  checkEq(record->renameRing[0].lastUsedSeqId, std::uint64_t{0u},
          "snapshot mark does not stamp a different rename entry");
  checkEq(record->renameRing[1].lastUsedSeqId, std::uint64_t{7u},
          "snapshot mark stamps the matching concrete ring entry");
  checkEq(record->lastUsedSeqId, std::uint64_t{7u},
          "snapshot mark also advances the logical buffer watermark");
}

void testManagedCreateSeedsVersionRing() {
  using namespace dxmt9::resources;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, managedBufferDesc());
  auto* record = resourcePool.findBuffer(handle.value);
  check(record != nullptr, "MANAGED create returns a record");
  check(record->isManagedVersioned,
        "MANAGED buffer selects writable-unlock backing versioning");
  check(!record->isDynamicRename,
        "MANAGED versioning is distinct from DEFAULT+DYNAMIC DISCARD");
  checkEq(record->renameRing.size(), std::size_t{1u},
          "MANAGED buffer seeds the create-time backing version");
}

void testManagedWritableMapNeverWaitsButDefaultPlainDoes() {
  using namespace dxmt9::core;
  using namespace dxmt9::resources;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto managed = resourcePool.createBuffer(dev, managedBufferDesc());
  resourcePool.markBufferUse(managed, /*seqId=*/9u);
  checkEq(resourcePool.mapWaitSeqId(managed, /*flags=*/0u), std::uint64_t{0u},
          "writable MANAGED plain map reads CPU shadow without waiting");
  checkEq(resourcePool.mapWaitSeqId(managed, UsageReadOnly), std::uint64_t{0u},
          "read-only MANAGED map also reads CPU shadow without waiting");
  checkEq(resourcePool.mapWaitSeqId(managed, UsageDiscard), std::uint64_t{0u},
          "MANAGED DISCARD map does not wait for the live Metal backing");

  const auto plain = resourcePool.createBuffer(dev, staticBufferDesc());
  resourcePool.markBufferUse(plain, /*seqId=*/7u);
  checkEq(resourcePool.mapWaitSeqId(plain, /*flags=*/0u), std::uint64_t{7u},
          "non-versioned DEFAULT plain map retains the sequence wait contract");
}

void testManagedUploadRotatesAfterMapAndCopiesFullShadow() {
  using namespace dxmt9::resources;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, managedBufferDesc(16u));
  auto* record = resourcePool.findBuffer(handle.value);
  check(record != nullptr, "MANAGED upload test record exists");

  std::array<std::uint8_t, 16> oldBacking{};
  oldBacking.fill(0x44u);
  std::array<std::uint8_t, 16> idleBacking{};
  idleBacking.fill(0xccu);
  record->renameRing[0].contents = oldBacking.data();
  record->contents = oldBacking.data();
  record->renameRing.emplace_back();
  record->renameRing[1].contents = idleBacking.data();

  resourcePool.markBufferUse(handle, /*seqId=*/5u);
  resourcePool.finalizeBufferMap(dev, handle, /*flags=*/0u,
                                 /*completedSeqId=*/4u);
  checkEq(record->renameActiveIndex, 0u,
          "MANAGED map itself does not rotate the backing");

  const std::array<std::uint8_t, 16> fullShadow{
      0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  check(resourcePool.uploadBufferData(dev, handle.value,
                                      fullShadow.data(), fullShadow.size(),
                                      /*completedSeqId=*/4u),
        "MANAGED writable unlock upload resolves the buffer");
  checkEq(record->renameActiveIndex, 1u,
          "upload rotates away from the in-flight backing");
  check(oldBacking == std::array<std::uint8_t, 16>{
                          0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
                          0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44},
        "upload never overwrites the in-flight old backing");
  check(idleBacking == fullShadow,
        "upload copies the complete CPU shadow into the selected backing");
  check(record->shadow ==
            std::vector<std::uint8_t>(fullShadow.begin(), fullShadow.end()),
        "pool shadow retains the complete uploaded buffer contents");
  checkEq(record->lastUsedSeqId, std::uint64_t{5u},
          "rotation does not lower the logical destruction watermark");
}

void testManagedUploadGrowsThenReusesOnlyCompletedBacking() {
  using namespace dxmt9::resources;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, managedBufferDesc(4u));
  const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};

  resourcePool.markBufferUse(handle, /*seqId=*/5u);
  check(resourcePool.uploadBufferData(dev, handle.value, bytes.data(), bytes.size(),
                                      /*completedSeqId=*/4u),
        "MANAGED upload grows when the sole backing is in flight");
  auto* record = resourcePool.findBuffer(handle.value);
  checkEq(record->renameRing.size(), std::size_t{2u},
          "no-idle MANAGED upload appends a fresh backing");
  checkEq(record->renameActiveIndex, 1u,
          "fresh MANAGED backing becomes active");
  checkEq(record->renameRing[0].lastUsedSeqId, std::uint64_t{5u},
          "retired backing keeps the sequence that references it");

  resourcePool.markBufferUse(handle, /*seqId=*/7u);
  check(resourcePool.uploadBufferData(dev, handle.value, bytes.data(), bytes.size(),
                                      /*completedSeqId=*/5u),
        "later MANAGED upload resolves the existing buffer");
  checkEq(record->renameRing.size(), std::size_t{2u},
          "completed old backing is reused instead of growing again");
  checkEq(record->renameActiveIndex, 0u,
          "reuse selects the backing whose sequence reached completion");
  checkEq(record->renameRing[1].lastUsedSeqId, std::uint64_t{7u},
          "still-in-flight backing is not selected for overwrite");
}

void testManagedDiscardMapDoesNotOverwriteInflightBacking() {
  using namespace dxmt9::core;
  using namespace dxmt9::resources;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, managedBufferDesc(8u));
  auto* record = resourcePool.findBuffer(handle.value);
  std::array<std::uint8_t, 8> liveBacking{};
  liveBacking.fill(0xa5u);
  record->renameRing[0].contents = liveBacking.data();
  record->contents = liveBacking.data();
  resourcePool.markBufferUse(handle, /*seqId=*/3u);

  resourcePool.finalizeBufferMap(dev, handle, UsageDiscard,
                                 /*completedSeqId=*/0u);
  std::array<std::uint8_t, 8> expected{};
  expected.fill(0xa5u);
  check(liveBacking == expected,
        "MANAGED DISCARD map leaves the in-flight Metal contents untouched");
}

void testManagedSnapshotMarksConcreteBackingAndPinsDestroy() {
  using namespace dxmt9::resources;
  dxmt9::resources::Pool resourcePool;
  WMT::Device dev{NULL_OBJECT_HANDLE};
  const auto handle = resourcePool.createBuffer(dev, managedBufferDesc());
  auto* record = resourcePool.findBuffer(handle.value);
  SyntheticBufferHandleGuard handleGuard{record};
  constexpr obj_handle_t metalHandle = 0x9abcu;
  record->buffer.handle = metalHandle;
  record->renameRing[0].buffer.handle = metalHandle;

  const auto snapshot = resourcePool.snapshotBufferBinding(handle);
  checkEq(snapshot.metalHandle, metalHandle,
          "MANAGED draw snapshot captures the concrete active backing");
  resourcePool.markBufferSnapshotUse(handle, snapshot, /*seqId=*/11u);
  checkEq(record->renameRing[0].lastUsedSeqId, std::uint64_t{11u},
          "MANAGED snapshot stamps the matching backing watermark");
  checkEq(record->lastUsedSeqId, std::uint64_t{11u},
          "MANAGED snapshot advances the logical destruction watermark");

  // Clear the synthetic handles before testing arena reclamation so
  // WMT::Reference destruction never forwards them to NSObject_release.
  record->buffer.handle = NULL_OBJECT_HANDLE;
  record->renameRing[0].buffer.handle = NULL_OBJECT_HANDLE;
  check(resourcePool.markBufferDestroyAndGc(handle.value,
                                             /*completedSeqId=*/10u),
        "destroy-pending MANAGED buffer is found");
  check(resourcePool.findBuffer(handle.value) != nullptr,
        "logical record and all backing references survive before seq 11");
  resourcePool.reclaimCompleted(/*completedSeqId=*/11u);
  check(resourcePool.findBuffer(handle.value) == nullptr,
        "logical record is reclaimed only after its snapshot sequence completes");
  handleGuard.record = nullptr;
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
    testConcreteSnapshotMarkProtectsRotatedBacking();
    testManagedCreateSeedsVersionRing();
    testManagedWritableMapNeverWaitsButDefaultPlainDoes();
    testManagedUploadRotatesAfterMapAndCopiesFullShadow();
    testManagedUploadGrowsThenReusesOnlyCompletedBacking();
    testManagedDiscardMapDoesNotOverwriteInflightBacking();
    testManagedSnapshotMarksConcreteBackingAndPinsDestroy();
  } catch (const TestFailure& e) {
    std::cerr << "dynamic_rename_ring_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "dynamic_rename_ring_spec unexpected exception: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
