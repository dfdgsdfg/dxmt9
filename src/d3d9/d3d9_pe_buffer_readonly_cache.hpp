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
    return bytes_.size() == resourceSize && requestEnd <= resourceSize;
  }

  void *dataFor(std::uint32_t offset) noexcept {
    return bytes_.data() + offset;
  }

  // Managed-buffer READONLY locks are cached as a whole-resource snapshot.
  // Some D3D9 applications pass element counts rather than byte counts as the
  // lock size, then index the returned mapped backing using the real stride.
  // Native D3D9 and WineD3D expose a whole managed backing in that case. A
  // request-sized cache makes the same old application walk past the PE copy,
  // so partial cache entries are deliberately unrepresentable here.
  bool refresh(std::uint32_t resourceSize, const void *source) noexcept {
    if (!source || resourceSize == 0u)
      return false;
    try {
      bytes_.resize(resourceSize);
    } catch (const std::bad_alloc &) {
      valid_ = false;
      return false;
    }
    std::memcpy(bytes_.data(), source, resourceSize);
    cacheGeneration_ = contentGeneration_;
    valid_ = true;
    return true;
  }

 private:
  std::vector<std::uint8_t> bytes_{};
  std::uint64_t contentGeneration_ = 1;
  std::uint64_t cacheGeneration_ = 0;
  bool valid_ = false;
};
