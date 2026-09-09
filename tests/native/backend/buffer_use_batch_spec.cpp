// Native coverage for the bounded buffer-use stamp primitive.  The test uses
// synthetic WMT handles because the pool's arena and rename-ring policy are
// the subject under test; no Metal device is required.

#include "../../../src/dxmt9/dxmt9_buffer_use_batch.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/winemetal/winemetal.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool value, std::string_view message) {
  if (!value) {
    throw TestFailure(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, std::string_view message) {
  if (!(actual == expected)) {
    throw TestFailure(std::string(message));
  }
}

dxmt9::core::BufferDesc dynamicBufferDesc() {
  dxmt9::core::BufferDesc desc{};
  desc.size = 64;
  desc.pool = dxmt9::core::Pool::Default;
  desc.usage = dxmt9::core::UsageDynamic;
  return desc;
}

struct SyntheticHandleGuard {
  dxmt9::resources::BufferRecord* record = nullptr;

  ~SyntheticHandleGuard() {
    if (!record) {
      return;
    }
    record->buffer.handle = NULL_OBJECT_HANDLE;
    for (auto& entry : record->renameRing) {
      entry.buffer.handle = NULL_OBJECT_HANDLE;
    }
  }
};

void installRing(dxmt9::resources::Pool& pool,
                 dxmt9::core::BufferHandle handle,
                 std::uint64_t first,
                 std::uint64_t second) {
  auto* record = pool.findBuffer(handle.value);
  check(record != nullptr, "synthetic buffer resolves");
  record->renameRing.emplace_back();
  record->buffer.handle = first;
  record->renameRing[0].buffer.handle = first;
  record->renameRing[1].buffer.handle = second;
  record->renameActiveIndex = 0;
}

void testBatchDeduplicatesOnlyCapturedIdentity() {
  using dxmt9::resources::BufferUseStamp;
  using dxmt9::resources::BufferUseStampBatch;

  BufferUseStampBatch batch;
  const dxmt9::core::Handle handle{0x100000001ull};
  check(batch.append(BufferUseStamp{handle, 0xau}), "first stamp fits");
  check(batch.append(BufferUseStamp{handle, 0xau}),
        "captured duplicate is accepted as a no-op");
  checkEq(batch.size(), std::size_t{1}, "captured duplicate is deduplicated");

  check(!batch.append(BufferUseStamp{handle, 0}),
        "logical stamp is a flush fence, not a batch entry");
  checkEq(batch.size(), std::size_t{1},
          "logical active mark does not enter the captured batch");

  const dxmt9::core::Handle otherGeneration{0x200000001ull};
  check(batch.append(BufferUseStamp{otherGeneration, 0xau}),
        "different handle generation fits");
  checkEq(batch.size(), std::size_t{2},
          "generation is part of captured stamp identity");
}

void testCapacityBoundaryIsFixedAndAllocationFree() {
  using dxmt9::resources::BufferUseStamp;
  using dxmt9::resources::BufferUseStampBatch;

  BufferUseStampBatch batch;
  for (std::size_t i = 0; i != BufferUseStampBatch::kCapacity; ++i) {
    check(batch.append(BufferUseStamp{
              dxmt9::core::Handle{0x400000000ull + i + 1}, 0x1000ull + i}),
          "each distinct captured stamp fits in the fixed block");
  }
  check(batch.full(), "64 entries fill the fixed block");
  check(!batch.append(BufferUseStamp{
              dxmt9::core::Handle{0x400000100ull}, 0x2000ull}),
        "the 65th distinct stamp requests a flush");
  batch.clear();
  check(batch.empty(), "flush clear empties the block");
  check(batch.append(BufferUseStamp{
            dxmt9::core::Handle{0x400000100ull}, 0x2000ull}),
        "the post-flush stamp fits");
}

void testPoolBatchMatchesScalarAndPreservesMissingBackingLogicalMark() {
  using dxmt9::resources::BufferUseStamp;
  dxmt9::resources::Pool batchPool;
  dxmt9::resources::Pool scalarPool;
  WMT::Device device{NULL_OBJECT_HANDLE};
  const auto batchHandle = batchPool.createBuffer(device, dynamicBufferDesc());
  const auto scalarHandle = scalarPool.createBuffer(device, dynamicBufferDesc());
  installRing(batchPool, batchHandle, 0xa0, 0xb0);
  installRing(scalarPool, scalarHandle, 0xa0, 0xb0);
  SyntheticHandleGuard batchGuard{batchPool.findBuffer(batchHandle.value)};
  SyntheticHandleGuard scalarGuard{scalarPool.findBuffer(scalarHandle.value)};

  const std::array<BufferUseStamp, 4> stamps{{
      {batchHandle, 0xa0},
      {batchHandle, 0xb0},
      {batchHandle, 0xc0},  // missing backing: logical record still stamped
      {batchHandle, 0xa0},
  }};
  dxmt9::resources::BufferUseStampBatch block;
  for (const auto stamp : stamps) check(block.append(stamp), "captured stamp admitted");
  checkEq(block.size(), std::size_t{3}, "A-B-missing-A retains three exact keys");
  const auto acquisitionCount = [](dxmt9::UploadLockOperation operation) {
    dxmt9::UploadLockSnapshot snapshot;
    dxmt9::collectUploadArenaLockSnapshot(snapshot);
    std::uint64_t result = 0;
    for (const auto& row : snapshot.buckets) {
      if (row.valid && row.nativeThreadId == dxmt9::uploadArenaNativeThreadId() &&
          row.site == dxmt9::UploadLockSite::BufferArena && row.operation == operation) {
        result += row.count;
      }
    }
    return result;
  };
  const auto batchBefore = acquisitionCount(dxmt9::UploadLockOperation::UpdateMany);
  batchPool.markBufferSnapshotUsesBatch(block, /*seqId=*/17);
  if (dxmt9::uploadArenaLockSplitEnabled()) {
    checkEq(acquisitionCount(dxmt9::UploadLockOperation::UpdateMany) - batchBefore,
            std::uint64_t{1}, "three captured keys use one actual arena acquisition");
  }

  dxmt9::core::DrawBufferBindingSnapshot snapshotA{.metalHandle = 0xa0};
  dxmt9::core::DrawBufferBindingSnapshot snapshotB{.metalHandle = 0xb0};
  dxmt9::core::DrawBufferBindingSnapshot snapshotMissing{.metalHandle = 0xc0};
  scalarPool.markBufferSnapshotUse(scalarHandle, snapshotA, 17);
  scalarPool.markBufferSnapshotUse(scalarHandle, snapshotB, 17);
  scalarPool.markBufferSnapshotUse(scalarHandle, snapshotMissing, 17);
  scalarPool.markBufferSnapshotUse(scalarHandle, snapshotA, 17);

  const auto* batched = batchPool.findBuffer(batchHandle.value);
  const auto* scalar = scalarPool.findBuffer(scalarHandle.value);
  check(batched != nullptr && scalar != nullptr, "both records remain live");
  checkEq(batched->lastUsedSeqId, std::uint64_t{17}, "logical reference max is 17");
  checkEq(batched->renameRing[0].lastUsedSeqId, std::uint64_t{17},
          "backing A independently receives final sequence");
  checkEq(batched->renameRing[1].lastUsedSeqId, std::uint64_t{17},
          "backing B independently receives final sequence");
  checkEq(batched->lastUsedSeqId, scalar->lastUsedSeqId,
          "batch and scalar logical watermark agree");
  checkEq(batched->renameRing[0].lastUsedSeqId,
          scalar->renameRing[0].lastUsedSeqId,
          "batch and scalar agree for backing A");
  checkEq(batched->renameRing[1].lastUsedSeqId,
          scalar->renameRing[1].lastUsedSeqId,
          "batch and scalar agree for backing B");

  block.clear();
  check(block.append(stamps[0]), "lower-sequence captured stamp admitted");
  batchPool.markBufferSnapshotUsesBatch(block, /*seqId=*/9);
  checkEq(batchPool.findBuffer(batchHandle.value)->lastUsedSeqId,
          std::uint64_t{17}, "watermarks use the final sequence maximum");
}

void testStaleGenerationIsNoOp() {
  using dxmt9::resources::BufferUseStamp;
  dxmt9::resources::Pool pool;
  WMT::Device device{NULL_OBJECT_HANDLE};
  const auto stale = pool.createBuffer(device, dynamicBufferDesc());
  check(pool.markBufferDestroyAndGc(stale.value, /*completedSeqId=*/0),
        "old generation is destroyed");
  const auto current = pool.createBuffer(device, dynamicBufferDesc());
  check(stale.value != current.value, "slot reuse qualifies a new generation");
  SyntheticHandleGuard guard{pool.findBuffer(current.value)};
  dxmt9::resources::BufferUseStampBatch block;
  check(block.append(BufferUseStamp{stale, 0x1234}), "stale stamp admitted for resolution");
  pool.markBufferSnapshotUsesBatch(block, /*seqId=*/31);
  checkEq(pool.findBuffer(current.value)->lastUsedSeqId, std::uint64_t{0},
          "stale generation cannot stamp the replacement record");
}

}  // namespace

int main() {
  try {
    testBatchDeduplicatesOnlyCapturedIdentity();
    testCapacityBoundaryIsFixedAndAllocationFree();
    testPoolBatchMatchesScalarAndPreservesMissingBackingLogicalMark();
    testStaleGenerationIsNoOp();
  } catch (const TestFailure& error) {
    std::cerr << "buffer_use_batch_spec failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
