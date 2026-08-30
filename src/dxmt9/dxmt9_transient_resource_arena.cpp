#include "dxmt9_transient_resource_arena.hpp"

#include "dxmt9/copy_materialization_ledger.hpp"
#include "dxmt9_perf_counters.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace dxmt9::transient {

namespace {

constexpr std::size_t kInitialSlabCapacity = 8ull << 20;

std::atomic_uint64_t gTransientLabelCounter{0};

template <std::size_t Cap = 96>
WMT::String makeLabelStringFmt(const char* fmt, ...) {
  char buf[Cap];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return WMT::String::string(buf, WMTUTF8StringEncoding);
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  return (value + alignment - 1) & ~(alignment - 1);
}

std::size_t nextPowerOfTwo(std::size_t value) {
  if (value <= 1) {
    return 1;
  }
  --value;
  for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
    value |= value >> shift;
  }
  return value + 1;
}

bool forceDedicatedTransientUploads() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT_DEBUG_FORCE_TRANSIENT_DEDICATED");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return value;
}

}  // namespace

void ResourceArena::init(WMT::Device device) noexcept {
  device_ = device;
}

void ResourceArena::reclaimUnlocked(std::uint64_t completedSeqId) {
  while (!slabAllocations_.empty() &&
         slabAllocations_.front().seqId <= completedSeqId) {
    slabAllocations_.pop_front();
  }
  if (slabAllocations_.empty()) {
    slabCursor_ = 0;
  }

  while (!retainedBuffers_.empty() &&
         retainedBuffers_.front().seqId <= completedSeqId) {
    retainedBuffers_.pop_front();
  }

  while (!retainedSamplers_.empty() &&
         retainedSamplers_.front().seqId <= completedSeqId) {
    retainedSamplers_.pop_front();
  }
}

void ResourceArena::reclaim(std::uint64_t completedSeqId) {
  std::lock_guard lock(mutex_);
  reclaimUnlocked(completedSeqId);
}

bool ResourceArena::ensureSlabUnlocked(std::size_t minimumCapacity) {
  if (slab_) {
    return slabCapacity_ >= minimumCapacity && slabContents_ != nullptr;
  }

  const std::size_t capacity =
      nextPowerOfTwo(std::max(kInitialSlabCapacity, minimumCapacity));
  WMTBufferInfo info{};
  info.length = capacity;
  info.options = WMTResourceStorageModeShared;
  auto buffer = device_.newBuffer(info);
  if (!buffer || !info.memory.ptr) {
    return false;
  }

  perf::countMetalBuffer(capacity);
  {
    const auto id = gTransientLabelCounter.fetch_add(1, std::memory_order_relaxed);
    WMT::Buffer view{buffer.handle};
    view.setLabel(makeLabelStringFmt("dxmt9-transient-slab-%llu-cap%zu",
                                     static_cast<unsigned long long>(id),
                                     capacity));
  }
  slab_ = std::move(buffer);
  slabContents_ = static_cast<std::byte*>(info.memory.ptr);
  slabCapacity_ = capacity;
  slabCursor_ = 0;
  return true;
}

bool ResourceArena::rotateSlabUnlocked(std::size_t minimumCapacity, std::uint64_t seqId) {
  if (slab_) {
    if (!slabAllocations_.empty()) {
      retainedBuffers_.push_back(RetainedBuffer{
          .buffer = std::move(slab_),
          .seqId = seqId,
      });
    } else {
      slab_ = nullptr;
    }
  }
  slabContents_ = nullptr;
  slabCapacity_ = 0;
  slabCursor_ = 0;
  slabAllocations_.clear();
  return ensureSlabUnlocked(minimumCapacity);
}

BufferSlice ResourceArena::uploadBuffer(std::span<const std::byte> bytes,
                                        std::size_t alignment,
                                        std::uint64_t seqId,
                                        std::uint64_t completedSeqId) {
  if (!device_ || bytes.empty()) {
    return {};
  }
  const auto uploadStarted = std::chrono::steady_clock::now();
  const auto recordUploadTime = [&] {
    const auto elapsed = std::chrono::steady_clock::now() - uploadStarted;
    perf::countTransientUploadCpuTime(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        bytes.size());
  };

  std::lock_guard lock(mutex_);
  reclaimUnlocked(completedSeqId);

  auto uploadDedicated = [&]() -> BufferSlice {
    WMTBufferInfo info{};
    info.length = bytes.size();
    info.options = WMTResourceStorageModeShared;
    info.memory.set((void*)bytes.data());
    auto buffer = device_.newBuffer(info);
    if (!buffer) {
      recordUploadTime();
      return {};
    }
    // `newBuffer(info)` performs the implicit byte transfer, but its
    // duration also includes Metal allocation/driver work. Record bytes and
    // calls without attributing that API latency to copy_ns.
    if (auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
            dxmt9::core::CopyMaterializationOwner::Unix)) {
      ledger->record(dxmt9::core::CopyMaterializationClass::GpuUploadCopy,
                     bytes.size());
    }
    perf::countMetalBuffer(static_cast<std::size_t>(info.length));
    {
      WMT::Buffer view{buffer.handle};
      view.setLabel(makeLabelStringFmt("dxmt9-transient-dedicated-seq%llu-bytes%zu",
                                       static_cast<unsigned long long>(seqId),
                                       bytes.size()));
    }
    retainedBuffers_.push_back(RetainedBuffer{
        .buffer = std::move(buffer),
        .seqId = seqId,
    });
    auto slice = BufferSlice{
        .buffer = WMT::Buffer{retainedBuffers_.back().buffer.handle},
        .offset = 0,
        .size = bytes.size(),
    };
    recordUploadTime();
    return slice;
  };

  alignment = std::max<std::size_t>(alignment, 1);
  if (forceDedicatedTransientUploads()) {
    return uploadDedicated();
  }
  const std::size_t alignedSize = alignUp(bytes.size(), alignment);
  if (!ensureSlabUnlocked(alignedSize)) {
    return uploadDedicated();
  }

  auto canPlace = [&](std::size_t offset) {
    if (offset + alignedSize > slabCapacity_) {
      return false;
    }
    for (const auto& allocation : slabAllocations_) {
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
  auto canAppendWithoutScan = [&](std::size_t offset) {
    if (offset + alignedSize > slabCapacity_) {
      return false;
    }
    // In the common non-wrapped state, slabCursor_ is past every live
    // allocation, so appending cannot overlap. Only fall back to the O(n)
    // overlap scan after the ring has wrapped and live high-offset
    // allocations may still exist.
    return slabAllocations_.empty() ||
           slabAllocations_.front().offset <= slabAllocations_.back().offset;
  };

  std::size_t offset = alignUp(slabCursor_, alignment);
  if (!canAppendWithoutScan(offset) && !canPlace(offset)) {
    offset = 0;
    if (!canPlace(offset)) {
      if (!rotateSlabUnlocked(alignedSize, seqId)) {
        return uploadDedicated();
      }
      offset = alignUp(slabCursor_, alignment);
      if (!canPlace(offset)) {
        return uploadDedicated();
      }
    }
  }

  if (auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
          dxmt9::core::CopyMaterializationOwner::Unix)) {
    dxmt9::core::CopyMaterializationEvent uploadEvent(
        ledger, dxmt9::core::CopyMaterializationClass::GpuUploadCopy,
        bytes.size());
    std::memcpy(slabContents_ + offset, bytes.data(), bytes.size());
  } else {
    std::memcpy(slabContents_ + offset, bytes.data(), bytes.size());
  }
  slabAllocations_.push_back(BufferAllocation{
      .offset = offset,
      .size = alignedSize,
      .seqId = seqId,
  });
  slabCursor_ = offset + alignedSize;

  auto slice = BufferSlice{
      .buffer = WMT::Buffer{slab_.handle},
      .offset = offset,
      .size = bytes.size(),
  };
  recordUploadTime();
  return slice;
}

std::vector<BufferSlice> ResourceArena::uploadBufferBatch(
    std::span<const std::span<const std::byte>> payloads,
    std::size_t alignment,
    std::uint64_t seqId,
    std::uint64_t completedSeqId) {
  std::vector<BufferSlice> result;
  if (!device_ || payloads.empty()) {
    return result;
  }
  result.reserve(payloads.size());

  const auto uploadStarted = std::chrono::steady_clock::now();
  std::size_t totalBytes = 0;
  for (const auto& p : payloads) totalBytes += p.size();

  std::lock_guard lock(mutex_);
  reclaimUnlocked(completedSeqId);

  alignment = std::max<std::size_t>(alignment, 1);

  for (const auto& bytes : payloads) {
    if (bytes.empty()) {
      result.push_back(BufferSlice{});
      continue;
    }
    const std::size_t alignedSize = alignUp(bytes.size(), alignment);

    auto canPlace = [&](std::size_t offset) {
      if (offset + alignedSize > slabCapacity_) return false;
      for (const auto& a : slabAllocations_) {
        const std::size_t begin = a.offset;
        const std::size_t end = a.offset + a.size;
        const std::size_t newBegin = offset;
        const std::size_t newEnd = offset + alignedSize;
        if (!(newEnd <= begin || newBegin >= end)) return false;
      }
      return true;
    };
    auto canAppendWithoutScan = [&](std::size_t offset) {
      if (offset + alignedSize > slabCapacity_) return false;
      return slabAllocations_.empty() ||
             slabAllocations_.front().offset <= slabAllocations_.back().offset;
    };

    auto uploadDedicated = [&]() -> BufferSlice {
      WMTBufferInfo info{};
      info.length = bytes.size();
      info.options = WMTResourceStorageModeShared;
      info.memory.set((void*)bytes.data());
      auto buffer = device_.newBuffer(info);
      if (!buffer) return {};
      // See the single-upload path: the implicit Metal transfer is counted,
      // but allocation/driver latency is deliberately excluded from copy_ns.
      if (auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
              dxmt9::core::CopyMaterializationOwner::Unix)) {
        ledger->record(dxmt9::core::CopyMaterializationClass::GpuUploadCopy,
                       bytes.size());
      }
      perf::countMetalBuffer(static_cast<std::size_t>(info.length));
      {
        WMT::Buffer view{buffer.handle};
        view.setLabel(makeLabelStringFmt("dxmt9-transient-dedicated-seq%llu-bytes%zu",
                                         static_cast<unsigned long long>(seqId),
                                         bytes.size()));
      }
      retainedBuffers_.push_back(RetainedBuffer{
          .buffer = std::move(buffer), .seqId = seqId});
      return BufferSlice{
          .buffer = WMT::Buffer{retainedBuffers_.back().buffer.handle},
          .offset = 0,
          .size = bytes.size(),
      };
    };

    if (forceDedicatedTransientUploads()) {
      result.push_back(uploadDedicated());
      continue;
    }

    if (!ensureSlabUnlocked(alignedSize)) {
      result.push_back(uploadDedicated());
      continue;
    }
    std::size_t offset = alignUp(slabCursor_, alignment);
    if (!canAppendWithoutScan(offset) && !canPlace(offset)) {
      offset = 0;
      if (!canPlace(offset)) {
        if (!rotateSlabUnlocked(alignedSize, seqId)) {
          result.push_back(uploadDedicated());
          continue;
        }
        offset = alignUp(slabCursor_, alignment);
        if (!canPlace(offset)) {
          result.push_back(uploadDedicated());
          continue;
        }
      }
    }
    if (auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
            dxmt9::core::CopyMaterializationOwner::Unix)) {
      dxmt9::core::CopyMaterializationEvent uploadEvent(
          ledger, dxmt9::core::CopyMaterializationClass::GpuUploadCopy,
          bytes.size());
      std::memcpy(slabContents_ + offset, bytes.data(), bytes.size());
    } else {
      std::memcpy(slabContents_ + offset, bytes.data(), bytes.size());
    }
    slabAllocations_.push_back(BufferAllocation{
        .offset = offset, .size = alignedSize, .seqId = seqId});
    slabCursor_ = offset + alignedSize;
    result.push_back(BufferSlice{
        .buffer = WMT::Buffer{slab_.handle},
        .offset = offset,
        .size = bytes.size(),
    });
  }

  const auto elapsed = std::chrono::steady_clock::now() - uploadStarted;
  perf::countTransientUploadCpuTime(
      static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
      totalBytes);
  return result;
}

BufferReservation ResourceArena::reserveBuffer(std::size_t size,
                                               std::size_t alignment,
                                               std::uint64_t seqId,
                                               std::uint64_t completedSeqId) {
  BufferReservation reservation{};
  if (!device_ || size == 0) {
    return reservation;
  }
  const auto uploadStarted = std::chrono::steady_clock::now();
  const auto recordUploadTime = [&] {
    const auto elapsed = std::chrono::steady_clock::now() - uploadStarted;
    perf::countTransientUploadCpuTime(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        size);
  };

  std::lock_guard lock(mutex_);
  reclaimUnlocked(completedSeqId);

  alignment = std::max<std::size_t>(alignment, 1);
  const std::size_t alignedSize = alignUp(size, alignment);

  auto reserveDedicated = [&]() -> BufferReservation {
    WMTBufferInfo info{};
    info.length = size;
    info.options = WMTResourceStorageModeShared;
    auto buffer = device_.newBuffer(info);
    if (!buffer || !info.memory.ptr) {
      recordUploadTime();
      return {};
    }
    perf::countMetalBuffer(static_cast<std::size_t>(info.length));
    {
      WMT::Buffer view{buffer.handle};
      view.setLabel(makeLabelStringFmt("dxmt9-transient-reserved-seq%llu-bytes%zu",
                                       static_cast<unsigned long long>(seqId),
                                       size));
    }
    retainedBuffers_.push_back(RetainedBuffer{
        .buffer = std::move(buffer),
        .seqId = seqId,
    });
    BufferReservation r{};
    r.slice = BufferSlice{
        .buffer = WMT::Buffer{retainedBuffers_.back().buffer.handle},
        .offset = 0,
        .size = size,
    };
    r.contents = static_cast<std::byte*>(info.memory.ptr);
    recordUploadTime();
    return r;
  };

  if (forceDedicatedTransientUploads()) {
    return reserveDedicated();
  }
  if (!ensureSlabUnlocked(alignedSize)) {
    return reserveDedicated();
  }

  auto canPlace = [&](std::size_t offset) {
    if (offset + alignedSize > slabCapacity_) {
      return false;
    }
    for (const auto& a : slabAllocations_) {
      const std::size_t begin = a.offset;
      const std::size_t end = a.offset + a.size;
      const std::size_t newBegin = offset;
      const std::size_t newEnd = offset + alignedSize;
      if (!(newEnd <= begin || newBegin >= end)) {
        return false;
      }
    }
    return true;
  };
  auto canAppendWithoutScan = [&](std::size_t offset) {
    if (offset + alignedSize > slabCapacity_) {
      return false;
    }
    return slabAllocations_.empty() ||
           slabAllocations_.front().offset <= slabAllocations_.back().offset;
  };

  std::size_t offset = alignUp(slabCursor_, alignment);
  if (!canAppendWithoutScan(offset) && !canPlace(offset)) {
    offset = 0;
    if (!canPlace(offset)) {
      if (!rotateSlabUnlocked(alignedSize, seqId)) {
        return reserveDedicated();
      }
      offset = alignUp(slabCursor_, alignment);
      if (!canPlace(offset)) {
        return reserveDedicated();
      }
    }
  }

  slabAllocations_.push_back(BufferAllocation{
      .offset = offset,
      .size = alignedSize,
      .seqId = seqId,
  });
  slabCursor_ = offset + alignedSize;

  reservation.slice = BufferSlice{
      .buffer = WMT::Buffer{slab_.handle},
      .offset = offset,
      .size = size,
  };
  reservation.contents = slabContents_ + offset;
  recordUploadTime();
  return reservation;
}

void ResourceArena::retainSamplerForSeq(WMT::Reference<WMT::SamplerState> sampler,
                                        std::uint64_t seqId) {
  if (!sampler) {
    return;
  }
  std::lock_guard lock(mutex_);
  retainedSamplers_.push_back(RetainedSampler{
      .sampler = std::move(sampler),
      .seqId = seqId,
  });
}

}  // namespace dxmt9::transient
