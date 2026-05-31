#pragma once

#include "../winemetal/Metal.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace dxmt9::transient {

struct BufferSlice {
  WMT::Buffer buffer{};
  std::uint64_t offset = 0;
  std::size_t size = 0;

  explicit operator bool() const noexcept { return static_cast<bool>(buffer); }
};

struct BufferReservation {
  BufferSlice slice{};
  std::byte* contents = nullptr;

  explicit operator bool() const noexcept {
    return contents != nullptr && static_cast<bool>(slice);
  }
};

// Queue-local arena for all short-lived Metal resources whose lifetime is
// bounded by a command chunk seqId. This keeps shared transient slabs,
// dedicated transient buffers, argbuf reservations, and argbuf sampler
// retention under the same reclaim(completedSeqId) policy.
class ResourceArena {
 public:
  ResourceArena() = default;
  ResourceArena(const ResourceArena&) = delete;
  ResourceArena& operator=(const ResourceArena&) = delete;

  void init(WMT::Device device) noexcept;

  BufferSlice uploadBuffer(std::span<const std::byte> bytes,
                           std::size_t alignment,
                           std::uint64_t seqId,
                           std::uint64_t completedSeqId);
  std::vector<BufferSlice> uploadBufferBatch(
      std::span<const std::span<const std::byte>> payloads,
      std::size_t alignment,
      std::uint64_t seqId,
      std::uint64_t completedSeqId);
  BufferReservation reserveBuffer(std::size_t size,
                                  std::size_t alignment,
                                  std::uint64_t seqId,
                                  std::uint64_t completedSeqId);

  void retainSamplerForSeq(WMT::Reference<WMT::SamplerState> sampler,
                           std::uint64_t seqId);
  void reclaim(std::uint64_t completedSeqId);

 private:
  struct BufferAllocation {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::uint64_t seqId = 0;
  };

  struct RetainedBuffer {
    WMT::Reference<WMT::Buffer> buffer{};
    std::uint64_t seqId = 0;
  };

  struct RetainedSampler {
    WMT::Reference<WMT::SamplerState> sampler{};
    std::uint64_t seqId = 0;
  };

  bool ensureSlabUnlocked(std::size_t minimumCapacity);
  bool rotateSlabUnlocked(std::size_t minimumCapacity, std::uint64_t seqId);
  void reclaimUnlocked(std::uint64_t completedSeqId);

  WMT::Device device_{};
  std::mutex mutex_{};
  WMT::Reference<WMT::Buffer> slab_{};
  std::byte* slabContents_ = nullptr;
  std::size_t slabCapacity_ = 0;
  std::size_t slabCursor_ = 0;
  std::deque<BufferAllocation> slabAllocations_{};
  std::deque<RetainedBuffer> retainedBuffers_{};
  std::deque<RetainedSampler> retainedSamplers_{};
};

}  // namespace dxmt9::transient
