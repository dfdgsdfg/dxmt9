#pragma once

#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

inline std::uint32_t d3d9PeBufferLockActualSize(std::uint32_t offset,
                                                std::uint32_t size,
                                                std::uint32_t resourceSize) noexcept {
  if (offset > resourceSize)
    return 0;
  return size != 0u ? size : resourceSize - offset;
}

class D3D9PeBufferReadonlyCache {
 public:
  void invalidate() noexcept {
    ++contentGeneration_;
    valid_ = false;
  }

  bool canServe(std::uint32_t offset, std::uint32_t size,
                std::uint32_t resourceSize) const noexcept {
    const std::uint32_t actualSize =
        d3d9PeBufferLockActualSize(offset, size, resourceSize);
    if (actualSize == 0u || !valid_ ||
        cacheGeneration_ != contentGeneration_) {
      return false;
    }
    const std::uint64_t requestBegin = offset;
    const std::uint64_t requestEnd = requestBegin + actualSize;
    const std::uint64_t cacheBegin = offset_;
    const std::uint64_t cacheEnd = cacheBegin + size_;
    return requestBegin >= cacheBegin && requestEnd <= cacheEnd;
  }

  void *dataFor(std::uint32_t offset) noexcept {
    return bytes_.data() + (offset - offset_);
  }

  bool refresh(std::uint32_t offset, std::uint32_t size,
               std::uint32_t resourceSize, const void *source) noexcept {
    const std::uint32_t actualSize =
        d3d9PeBufferLockActualSize(offset, size, resourceSize);
    if (!source || actualSize == 0u)
      return false;
    try {
      bytes_.resize(actualSize);
    } catch (const std::bad_alloc &) {
      valid_ = false;
      return false;
    }
    std::memcpy(bytes_.data(), source, actualSize);
    offset_ = offset;
    size_ = actualSize;
    cacheGeneration_ = contentGeneration_;
    valid_ = true;
    return true;
  }

 private:
  std::vector<std::uint8_t> bytes_{};
  std::uint64_t contentGeneration_ = 1;
  std::uint64_t cacheGeneration_ = 0;
  std::uint32_t offset_ = 0;
  std::uint32_t size_ = 0;
  bool valid_ = false;
};
