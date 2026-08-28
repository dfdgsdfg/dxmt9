#pragma once

// Append-only publication table for handles whose numeric ABI is fixed.  A
// writer allocates each block once and publishes one immutable value at a
// time; readers never take the writer's lock and never observe a partially
// initialized value.  The owner must keep the table alive until all readers
// have stopped (Cache supplies that lifetime contract).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace dxmt9::detail {

template <typename T, std::size_t BlockSize = 64,
          typename Index = std::uint16_t>
class SegmentedImmutableSlotTable {
  static_assert(BlockSize != 0, "slot table blocks must not be empty");
  static_assert(std::is_unsigned_v<Index>, "slot indices must be unsigned");

  struct Slot {
    T value{};
    std::atomic<bool> published{false};
  };

  struct Block {
    std::array<Slot, BlockSize> slots{};
  };

 public:
  static constexpr Index kInvalidIndex = std::numeric_limits<Index>::max();
  static constexpr std::size_t kCapacity = static_cast<std::size_t>(kInvalidIndex);
  static constexpr std::size_t kSegmentSize = BlockSize;
  static constexpr std::size_t kBlockCount =
      (kCapacity + BlockSize - 1u) / BlockSize;

  static constexpr std::size_t segmentStorageBytes() noexcept {
    return sizeof(Block);
  }

  SegmentedImmutableSlotTable() noexcept {
    for (auto& block : blocks_) {
      block.store(nullptr, std::memory_order_relaxed);
    }
  }

  SegmentedImmutableSlotTable(const SegmentedImmutableSlotTable&) = delete;
  SegmentedImmutableSlotTable& operator=(const SegmentedImmutableSlotTable&) = delete;

  // Append is single-writer only.  The caller serializes it with the cache's
  // mutex; readers may call lookup()/size() concurrently.  A returned index
  // is permanently occupied and its value is never modified or recycled.
  std::optional<Index> append(T value) {
    const std::size_t index = publishedSize_.load(std::memory_order_relaxed);
    if (index >= kCapacity) {
      return std::nullopt;
    }

    const std::size_t blockIndex = index / BlockSize;
    const std::size_t slotIndex = index % BlockSize;
    Block* block = blocks_[blockIndex].load(std::memory_order_acquire);
    if (!block) {
      auto owned = std::make_unique<Block>();
      block = owned.get();
      ownedBlocks_.push_back(std::move(owned));
      blocks_[blockIndex].store(block, std::memory_order_release);
    }

    Slot& slot = block->slots[slotIndex];
    slot.value = std::move(value);
    slot.published.store(true, std::memory_order_release);
    publishedSize_.store(index + 1u, std::memory_order_release);
    return static_cast<Index>(index);
  }

  const T* lookup(Index index) const noexcept {
    const std::size_t numericIndex = static_cast<std::size_t>(index);
    if (index == kInvalidIndex ||
        numericIndex >= publishedSize_.load(std::memory_order_acquire)) {
      return nullptr;
    }
    const Block* block = blocks_[numericIndex / BlockSize].load(
        std::memory_order_acquire);
    if (!block) {
      return nullptr;
    }
    const Slot& slot = block->slots[numericIndex % BlockSize];
    if (!slot.published.load(std::memory_order_acquire)) {
      return nullptr;
    }
    return &slot.value;
  }

  std::size_t size() const noexcept {
    return publishedSize_.load(std::memory_order_acquire);
  }

 private:
  std::array<std::atomic<Block*>, kBlockCount> blocks_{};
  // This vector is writer-owned.  It is deliberately not used by readers;
  // its only purpose is to retain blocks for the table's lifetime.
  std::vector<std::unique_ptr<Block>> ownedBlocks_{};
  std::atomic<std::size_t> publishedSize_{0};
};

}  // namespace dxmt9::detail
