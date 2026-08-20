// Host coverage for the pure part of the wow64 low-4GB shadow-lock block
// pool (`src/d3d9/device_c_low4gb_pool.hpp`). The real OS-backed allocator
// (`allocateLow4GB`/`freeLow4GB`) lives in `device_c_marshal.cpp` and needs
// __APPLE__/Wine symbol resolution, so only this policy core — bucket
// selection, bounded capacity, and hit/miss/eviction accounting — is
// host-testable here; the wiring into `acquireLow4GB`/`releaseLow4GB` is
// covered by `dxmt9-core-device-com-spec` and the wild-run counters.

#include "device_c_low4gb_pool.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
}

// Mirrors the shape of Low4GBAllocation just enough to exercise the pool
// without pulling in device_c_common.hpp (Wine/core headers).
struct FakeBlock {
  void* ptr = nullptr;
  size_t size = 0;

  constexpr explicit operator bool() const noexcept { return ptr != nullptr; }
};

using Pool = dxmt9::d3d9::devicec::Low4GBBlockPool<FakeBlock>;

void testBucketCapacityRoundsUpToPowerOfTwo() {
  check(Pool::bucketCapacityFor(1) == Pool::kMinBucketBytes,
        "tiny request rounds up to the minimum bucket");
  check(Pool::bucketCapacityFor(Pool::kMinBucketBytes) == Pool::kMinBucketBytes,
        "exact minimum bucket size stays in that bucket");
  check(Pool::bucketCapacityFor(Pool::kMinBucketBytes + 1) == Pool::kMinBucketBytes * 2,
        "one byte over a bucket boundary rounds to the next bucket");
  check(Pool::bucketCapacityFor(Pool::kMaxBucketBytes) == Pool::kMaxBucketBytes,
        "exact maximum bucket size is still poolable");
  check(Pool::bucketCapacityFor(Pool::kMaxBucketBytes + 1) == 0,
        "a request above the largest bucket is not poolable");
  check(Pool::bucketCapacityFor(0) == 0, "a zero-size request is not poolable");
}

void testEmptyPoolMissesAndOversizedNeverCountsAsMiss() {
  Pool pool;
  check(!pool.tryAcquire(4096).has_value(), "empty pool misses a poolable request");
  check(pool.misses() == 1, "the poolable miss is counted");
  check(!pool.tryAcquire(Pool::kMaxBucketBytes + 1).has_value(),
        "an oversized request also returns nullopt");
  check(pool.misses() == 1, "an oversized request never touches the pool's miss count");
  check(pool.hits() == 0, "no hits yet");
}

void testReleaseThenAcquireIsAHit() {
  Pool pool;
  const size_t capacity = Pool::bucketCapacityFor(200u * 1024u);
  int dummy = 0;
  FakeBlock block{&dummy, capacity};
  check(pool.tryRelease(capacity, block), "releasing a bucket-sized block is pooled");
  check(pool.totalBytes() == capacity, "pooled bytes are tracked");
  auto acquired = pool.tryAcquire(200u * 1024u);
  check(acquired.has_value(), "a request that fits the pooled bucket hits");
  check(acquired.has_value() && acquired->ptr == &dummy, "the exact pooled block comes back");
  check(pool.hits() == 1, "the hit is counted");
  check(pool.totalBytes() == 0, "pooled bytes drop back to zero after the block is taken");
  check(!pool.tryAcquire(200u * 1024u).has_value(), "the bucket is empty again after the hit");
}

void testAcquireDoesNotCrossBuckets() {
  // A pool miss always allocates at the *bucket* capacity, so a later
  // request rounding to a *different*, smaller bucket must not be
  // satisfied out of a larger bucket's blocks — each bucket only serves
  // requests that round to its own exact capacity.
  Pool pool;
  const size_t capacity = Pool::bucketCapacityFor(600u * 1024u);  // -> 1 MiB
  int dummy = 0;
  check(pool.tryRelease(capacity, FakeBlock{&dummy, capacity}), "release into the 1 MiB bucket");
  auto acquired = pool.tryAcquire(10u * 1024u);  // rounds to 64 KiB bucket != 1 MiB bucket
  check(!acquired.has_value(),
        "a request for a smaller bucket does not reach into a larger bucket");
}

void testPerBucketCapIsEnforced() {
  Pool pool;
  const size_t capacity = Pool::bucketCapacityFor(64u * 1024u);
  std::vector<int> dummies(Pool::kMaxBlocksPerBucket + 2, 0);
  size_t pooled = 0;
  size_t evicted = 0;
  for (auto& dummy : dummies) {
    if (pool.tryRelease(capacity, FakeBlock{&dummy, capacity})) {
      ++pooled;
    } else {
      ++evicted;
    }
  }
  check(pooled == Pool::kMaxBlocksPerBucket, "bucket accepts exactly its cap of blocks");
  check(evicted == dummies.size() - Pool::kMaxBlocksPerBucket,
        "blocks beyond the per-bucket cap are evicted");
  check(pool.evictions() == evicted, "evictions are counted");
}

void testTotalByteCapIsEnforced() {
  Pool pool;
  // Fill enough large buckets to approach the total-byte cap, then prove
  // one more release evicts even though its own bucket has room.
  const size_t bigCapacity = Pool::kMaxBucketBytes;  // 8 MiB
  int dummy = 0;
  size_t pooledBlocks = 0;
  while (pool.totalBytes() + bigCapacity <= Pool::kMaxTotalBytes &&
         pooledBlocks < Pool::kMaxBlocksPerBucket) {
    check(pool.tryRelease(bigCapacity, FakeBlock{&dummy, bigCapacity}),
          "large block pools while under the byte cap");
    ++pooledBlocks;
  }
  const bool overflowed = !pool.tryRelease(bigCapacity, FakeBlock{&dummy, bigCapacity});
  check(overflowed, "a release that would exceed the total byte cap is evicted");
  check(pool.totalBytes() <= Pool::kMaxTotalBytes, "pool never exceeds its total byte cap");
}

void testReleaseAllVisitsEveryPooledBlockOnce() {
  Pool pool;
  const size_t capacityA = Pool::bucketCapacityFor(64u * 1024u);
  const size_t capacityB = Pool::bucketCapacityFor(1024u * 1024u);
  int a1 = 0, a2 = 0, b1 = 0;
  check(pool.tryRelease(capacityA, FakeBlock{&a1, capacityA}), "pool block a1");
  check(pool.tryRelease(capacityA, FakeBlock{&a2, capacityA}), "pool block a2");
  check(pool.tryRelease(capacityB, FakeBlock{&b1, capacityB}), "pool block b1");

  int visited = 0;
  pool.releaseAll([&](FakeBlock& block) {
    check(block.ptr != nullptr, "releaseAll visits a real block");
    ++visited;
  });
  check(visited == 3, "releaseAll visits every pooled block exactly once");
  check(pool.totalBytes() == 0, "releaseAll drains the byte total");
  check(!pool.tryAcquire(64u * 1024u).has_value(), "pool is empty after releaseAll");
}

}  // namespace

int main() {
  testBucketCapacityRoundsUpToPowerOfTwo();
  testEmptyPoolMissesAndOversizedNeverCountsAsMiss();
  testReleaseThenAcquireIsAHit();
  testAcquireDoesNotCrossBuckets();
  testPerBucketCapIsEnforced();
  testTotalByteCapIsEnforced();
  testReleaseAllVisitsEveryPooledBlockOnce();

  if (failures != 0) {
    std::cerr << failures << " low4gb block pool check(s) failed\n";
    return 1;
  }
  std::cout << "device_c low4gb block pool spec ok\n";
  return 0;
}
