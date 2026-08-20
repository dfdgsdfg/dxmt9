#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace dxmt9::d3d9::devicec {

// Bounded, size-classed free-block pool for wow64 low-4GB shadow-lock
// allocations.
//
// `allocateLow4GB`'s slow path is an OS-level low-address scan
// (NtAllocateVirtualMemory / mach_vm_allocate probing upward from a fixed
// base until it lands a sub-4GB region) that costs on the order of a
// millisecond once most of address space above the scan start is already
// claimed (a syscall per failed attempt). `ShadowLock` already keeps a
// block alive across repeated locks of the *same* D3D9 object, but a
// workload that locks a stream of *distinct* surfaces (a fresh object
// every time) never benefits from that reuse — every first lock frees the
// previous object's shadow at release time and pays the full scan again
// on the next object's first lock, even though the blocks being churned
// are all well under a few MB.
//
// This pool sits between that per-object reuse cache and the OS
// allocator: a released block is kept here instead of being freed
// immediately, and a fresh allocation checks here before falling through
// to the OS scan.
//
// This class is the pure, allocation-bounded bookkeeping: it owns no OS
// resources and makes no syscalls. The caller does the real OS
// alloc/free and only asks this pool where to look/park a `Block` value.
// `Block` must be default-constructible (representing "no allocation"),
// contextually convertible to `bool` (false == empty, matching
// `Low4GBAllocation::operator bool()`), and expose a `size_t size` member
// naming the block's real usable capacity in bytes.
//
// R-BACK-43.4: this type carries NO internal synchronization. Its single
// production instance (`low4GBPool()` in `device_c_marshal.cpp`) is
// `arena-protected` by `low4GBPoolMutex()` there — the component's own lock,
// explicitly not `CommandQueue::mutex_`, because the game thread allocates
// shadows while the replay offload worker can drive their release. A new
// caller must take that mutex; nothing else covers this state.
template <typename Block>
class Low4GBBlockPool {
 public:
  // Size classes are power-of-two buckets. A block the pool itself hands
  // out always has a size exactly equal to one of these bucket
  // capacities (see `bucketCapacityFor`), so bucket membership is a
  // simple index computed from `size` — never a search over a range of
  // sizes.
  static constexpr size_t kMinBucketBytes = 64u * 1024u;         // 64 KiB
  static constexpr size_t kMaxBucketBytes = 8u * 1024u * 1024u;  // 8 MiB
  static constexpr size_t kNumBuckets = 8;  // 64K,128K,256K,512K,1M,2M,4M,8M

  // Per-bucket block cap. A workload that locks many same-size fresh
  // surfaces in a burst should not be starved after a handful of
  // releases, but this is a recycle pool, not a cache of everything ever
  // freed — bound it.
  static constexpr size_t kMaxBlocksPerBucket = 8;

  // Total resident-byte cap across every bucket. At 8 buckets x 8 blocks
  // the theoretical maximum is far above this; the byte cap is the real
  // limit so the pool cannot hoard a large fraction of the scarce
  // sub-4GB address range even under a pathological mix of large
  // buckets. `tryRelease` treats exceeding this as an eviction.
  static constexpr size_t kMaxTotalBytes = 64u * 1024u * 1024u;  // 64 MiB

  // Bucket capacity (exact size the pool will ask the OS for) for a
  // request of `size` bytes, or 0 when `size` is 0 or exceeds
  // `kMaxBucketBytes` — such a request is not poolable and the caller
  // must alloc/free it directly. Pooling multi-megabyte-and-up blocks
  // indefinitely would hoard low-4GB address space for shadow locks that
  // are already rare; the OS scan cost they pay is a small fraction of
  // overall churn compared to the small, frequent, fresh-surface case
  // this pool targets.
  static constexpr size_t bucketCapacityFor(size_t size) noexcept {
    if (size == 0 || size > kMaxBucketBytes) {
      return 0;
    }
    size_t cap = kMinBucketBytes;
    while (cap < size) {
      cap <<= 1;
    }
    return cap;
  }

  static constexpr int bucketIndexForCapacity(size_t capacity) noexcept {
    size_t cap = kMinBucketBytes;
    for (int idx = 0; idx < static_cast<int>(kNumBuckets); ++idx) {
      if (cap == capacity) {
        return idx;
      }
      cap <<= 1;
    }
    return -1;
  }

  // Attempts to take a pooled block that satisfies a request of `size`
  // bytes. On a hit, pops and returns the block (increments `hits_`). On
  // a miss — `size` is not poolable, or the matching bucket is empty —
  // returns `std::nullopt`. `misses_` is incremented only when `size`
  // was itself poolable (an oversized request never touches the pool at
  // all, so it is not a pool "miss").
  std::optional<Block> tryAcquire(size_t size) {
    const size_t capacity = bucketCapacityFor(size);
    if (capacity == 0) {
      return std::nullopt;
    }
    const int idx = bucketIndexForCapacity(capacity);
    auto& bucket = buckets_[static_cast<size_t>(idx)];
    if (bucket.empty()) {
      ++misses_;
      return std::nullopt;
    }
    Block block = std::move(bucket.back());
    bucket.pop_back();
    totalBytes_ -= capacity;
    ++hits_;
    return block;
  }

  // Offers `block` (whose real capacity is `blockCapacity`) back to the
  // pool. Returns true when the block was pooled. Returns false — and
  // bumps `evictions_` — when `blockCapacity` is not one of the pool's
  // bucket sizes, the matching bucket is already at
  // `kMaxBlocksPerBucket`, or accepting it would exceed
  // `kMaxTotalBytes`; the caller must free such a block directly.
  bool tryRelease(size_t blockCapacity, Block block) {
    const int idx = bucketIndexForCapacity(blockCapacity);
    if (idx < 0) {
      ++evictions_;
      return false;
    }
    auto& bucket = buckets_[static_cast<size_t>(idx)];
    if (bucket.size() >= kMaxBlocksPerBucket ||
        totalBytes_ + blockCapacity > kMaxTotalBytes) {
      ++evictions_;
      return false;
    }
    bucket.push_back(std::move(block));
    totalBytes_ += blockCapacity;
    return true;
  }

  size_t hits() const noexcept { return hits_; }
  size_t misses() const noexcept { return misses_; }
  size_t evictions() const noexcept { return evictions_; }
  size_t totalBytes() const noexcept { return totalBytes_; }

  size_t blockCountForTest(size_t capacity) const {
    const int idx = bucketIndexForCapacity(capacity);
    if (idx < 0) {
      return 0;
    }
    return buckets_[static_cast<size_t>(idx)].size();
  }

  // Drops every stored block via `freeFn(Block&)`, for pool teardown /
  // tests. Not required in production (the pool lives for the process
  // lifetime) but useful to assert no leaks in a host test.
  template <typename FreeFn>
  void releaseAll(FreeFn&& freeFn) {
    for (auto& bucket : buckets_) {
      for (auto& block : bucket) {
        freeFn(block);
      }
      bucket.clear();
    }
    totalBytes_ = 0;
  }

 private:
  std::array<std::vector<Block>, kNumBuckets> buckets_{};
  size_t totalBytes_ = 0;
  size_t hits_ = 0;
  size_t misses_ = 0;
  size_t evictions_ = 0;
};

}  // namespace dxmt9::d3d9::devicec
