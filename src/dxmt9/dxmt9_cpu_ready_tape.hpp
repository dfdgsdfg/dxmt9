#pragma once

#include "dxmt9_backend_types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace dxmt9::core {

// Migration name for the existing data-oriented command payload. Queue
// lifecycle code uses SourcePayloadBlock; encoder APIs continue to accept the
// historical ChunkSlot spelling until that wider signature migration is
// useful on its own.
using SourcePayloadBlock = ChunkSlot;

// Generation-checked locator for a payload block owned by CpuReadyTape. The
// queue may copy this value into ready/encode snapshots, but it must resolve
// the locator again before retaining or reclaiming the source.
struct CpuReadySourceId {
  std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
  std::uint64_t generation = 0;

  constexpr bool valid() const noexcept {
    return generation != 0 &&
           index != std::numeric_limits<std::uint32_t>::max();
  }

  friend constexpr bool operator==(CpuReadySourceId,
                                   CpuReadySourceId) noexcept = default;
};

// Queue-ring control shell. Command payload is deliberately not embedded:
// CpuReadyTape owns it and this shell only carries lifecycle identity plus a
// call-local convenience pointer validated by sourceId.
struct ChunkSlotControl {
  using State = ChunkSlot::State;

  State state = State::Free;
  std::uint64_t seqId = 0;
  CpuReadySourceId sourceId{};
  SourcePayloadBlock* payload = nullptr;

  bool commandsEmpty() const noexcept {
    return !payload || payload->commandsEmpty();
  }

  std::size_t commandCount() const noexcept {
    return payload ? payload->commandCount() : 0;
  }
};

// Fixed-capacity CPU-ready source store. Storage is allocated once when the
// command queue is created; reserve/publish/encode/reclaim perform no heap
// allocation. Payload vector growth remains payload-local and is intentionally
// unchanged by this ownership refactor. All methods are called under the
// owning queue's lifecycle mutex.
class CpuReadyTape {
 public:
  enum class State { Free, Writing, Ready, Encoding, GPU };

  struct Reservation {
    CpuReadySourceId id{};
    SourcePayloadBlock* payload = nullptr;

    explicit operator bool() const noexcept {
      return id.valid() && payload;
    }
  };

  explicit CpuReadyTape(std::size_t capacity)
      : capacity_(capacity), entries_(std::make_unique<Entry[]>(capacity)) {}

  CpuReadyTape(const CpuReadyTape&) = delete;
  CpuReadyTape& operator=(const CpuReadyTape&) = delete;

  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t residentCount() const noexcept { return residentCount_; }

  std::optional<Reservation> reserve() noexcept {
    if (residentCount_ == capacity_) {
      return std::nullopt;
    }
    for (std::size_t offset = 0; offset < capacity_; ++offset) {
      const std::size_t index = (nextIndex_ + offset) % capacity_;
      auto& entry = entries_[index];
      if (entry.state != State::Free) {
        continue;
      }
      ++entry.generation;
      if (entry.generation == 0) {
        ++entry.generation;
      }
      entry.payload.clearCommands();
      entry.payload.seqId = 0;
      entry.state = State::Writing;
      ++residentCount_;
      nextIndex_ = (index + 1) % capacity_;
      return Reservation{
          .id = CpuReadySourceId{
              .index = static_cast<std::uint32_t>(index),
              .generation = entry.generation,
          },
          .payload = &entry.payload,
      };
    }
    return std::nullopt;
  }

  SourcePayloadBlock* resolve(CpuReadySourceId id, State expected) noexcept {
    auto* entry = resolveEntry(id);
    return entry && entry->state == expected ? &entry->payload : nullptr;
  }

  const SourcePayloadBlock* resolve(CpuReadySourceId id,
                                    State expected) const noexcept {
    const auto* entry = resolveEntry(id);
    return entry && entry->state == expected ? &entry->payload : nullptr;
  }

  bool transition(CpuReadySourceId id, State before, State after) noexcept {
    auto* entry = resolveEntry(id);
    if (!entry || entry->state != before ||
        !validTransition(before, after)) {
      return false;
    }
    entry->state = after;
    return true;
  }

  bool reclaim(CpuReadySourceId id) noexcept {
    auto* entry = resolveEntry(id);
    if (!entry || (entry->state != State::GPU &&
                   entry->state != State::Encoding &&
                   entry->state != State::Writing)) {
      return false;
    }
    entry->payload.clearCommands();
    entry->payload.seqId = 0;
    entry->state = State::Free;
    --residentCount_;
    return true;
  }

 private:
  static constexpr bool validTransition(State before, State after) noexcept {
    return (before == State::Writing && after == State::Ready) ||
           (before == State::Ready && after == State::Encoding) ||
           (before == State::Encoding && after == State::GPU);
  }

  struct Entry {
    SourcePayloadBlock payload{};
    std::uint64_t generation = 0;
    State state = State::Free;
  };

  Entry* resolveEntry(CpuReadySourceId id) noexcept {
    if (!id.valid() || id.index >= capacity_) {
      return nullptr;
    }
    auto& entry = entries_[id.index];
    return entry.generation == id.generation ? &entry : nullptr;
  }

  const Entry* resolveEntry(CpuReadySourceId id) const noexcept {
    if (!id.valid() || id.index >= capacity_) {
      return nullptr;
    }
    const auto& entry = entries_[id.index];
    return entry.generation == id.generation ? &entry : nullptr;
  }

  std::size_t capacity_ = 0;
  std::unique_ptr<Entry[]> entries_{};
  std::size_t residentCount_ = 0;
  std::size_t nextIndex_ = 0;
};

}  // namespace dxmt9::core
