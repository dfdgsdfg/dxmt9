#pragma once

// Per-frame bump-ring allocator for transient scratch buffers used by the
// encode thread. Each allocation is tagged with the sequence id that owns
// it; `reclaim(completedSeqId)` frees storage for allocations at or below
// that watermark.
//
// Lifted out of backend_metal.mm so other encoder modules (future
// Renderer / PipelineCache builders) can use the same allocator without
// pulling in the backend TU.

#include "dxmt9/assert.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace dxmt9::scratch {

using u64 = std::uint64_t;

class RingArena {
 public:
  explicit RingArena(std::size_t capacity = 1 << 20) : storage_(capacity) {}

  void reclaim(u64 completedSeqId) {
    while (!allocations_.empty() && allocations_.front().seqId <= completedSeqId) {
      cursor_ = std::max(cursor_, allocations_.front().offset + allocations_.front().size);
      allocations_.pop_front();
    }
    if (allocations_.empty()) {
      cursor_ = 0;
    }
  }

  std::byte* allocateBytes(std::size_t size, std::size_t alignment, u64 seqId) {
    if (size == 0 || storage_.empty()) {
      return nullptr;
    }
    const std::size_t alignedSize = alignUp(size, alignment);
    if (alignedSize > storage_.size()) {
      return nullptr;
    }

    auto canPlace = [&](std::size_t offset) {
      if (offset + alignedSize > storage_.size()) {
        return false;
      }
      for (const auto& allocation : allocations_) {
        const std::size_t begin = allocation.offset;
        const std::size_t end = allocation.offset + allocation.size;
        const std::size_t newBegin = offset;
        const std::size_t newEnd = offset + alignedSize;
        if (!(newEnd <= begin || newBegin >= end)) {
          return false;
        }
      }
      return true;
    };

    std::size_t offset = alignUp(cursor_, alignment);
    if (!canPlace(offset)) {
      offset = 0;
      if (!canPlace(offset)) {
        // TLA+: RingSafety. Exhaustion is recoverable: callers that need
        // transient upload memory fall back to one-shot Metal buffers.
        return nullptr;
      }
    }

    allocations_.push_back({offset, alignedSize, seqId});
    cursor_ = offset + alignedSize;
    return storage_.data() + offset;
  }

  template <typename T>
  T* allocate(u64 seqId, std::size_t count = 1) {
    return reinterpret_cast<T*>(allocateBytes(sizeof(T) * count, alignof(T), seqId));
  }

 private:
  struct Allocation {
    std::size_t offset = 0;
    std::size_t size = 0;
    u64 seqId = 0;
  };

  static std::size_t alignUp(std::size_t value, std::size_t alignment) {
    if (alignment <= 1) {
      return value;
    }
    return (value + alignment - 1) & ~(alignment - 1);
  }

  std::vector<std::byte> storage_;
  std::size_t cursor_ = 0;
  std::deque<Allocation> allocations_;
};

// Per-backend set of ring arenas. All four are reclaimed in lockstep on
// the finish path keyed by completedSeqId.
struct FrameAllocators {
  RingArena argbuf{1 << 20};
  RingArena lambdaStore{1 << 18};
  RingArena staging{1 << 20};
  RingArena copyTemp{1 << 20};

  void reclaim(u64 completedSeqId) {
    argbuf.reclaim(completedSeqId);
    lambdaStore.reclaim(completedSeqId);
    staging.reclaim(completedSeqId);
    copyTemp.reclaim(completedSeqId);
  }
};

}  // namespace dxmt9::scratch
