#pragma once

#include "query_fence.hpp"
#include <atomic>
#include <memory>
#include <limits>

namespace dxmt9::query {

// Unix/core-only ownership. Never serialized into the PE bridge or stored in
// a Metal callback. Source generations disambiguate recycled physical Slots.
struct WriterIdentity {
  std::uint64_t queue = 0;
  std::uint32_t index = 0;
  std::uint64_t generation = 0;
  std::uint64_t storageGeneration = 0;
  constexpr bool valid() const noexcept {
    return queue && index != std::numeric_limits<std::uint32_t>::max() &&
        generation && storageGeneration;
  }
  friend constexpr bool operator==(WriterIdentity, WriterIdentity) = default;
};

// QueryEndCoalescing / Publish: only the admitted owner may bind, once,
// using the actual successful publication sequence, never a reservation.
constexpr bool canBindEnd(WriterIdentity admitted, WriterIdentity published,
                          std::uint64_t bound, std::uint64_t sequence) noexcept {
  return admitted.valid() && admitted == published && !bound && sequence;
}

class EndMarker {
 public:
  explicit EndMarker(WriterIdentity owner) noexcept : owner_(owner) {}
  WriterIdentity owner() const noexcept { return owner_; }
  std::uint64_t sequence() const noexcept {
    return sequence_.load(std::memory_order_acquire);
  }
  // Called only under the owning queue mutex after successful sealAndPublish.
  bool bind(WriterIdentity owner, std::uint64_t sequence) noexcept {
    auto expected = std::uint64_t{0};
    return canBindEnd(owner_, owner, expected, sequence) &&
        sequence_.compare_exchange_strong(expected, sequence,
                                          std::memory_order_release,
                                          std::memory_order_relaxed);
  }
 private:
  const WriterIdentity owner_;
  std::atomic<std::uint64_t> sequence_{0};
};

// Keep the scalar Fence/classify algebra separate from lifetime ownership.
// One allocation per nonempty writer group, not per END. Empty/eager receipts
// carry no marker; null shared_ptr copies have no refcount operation.
struct EndReceipt {
  Fence prefix{};
  std::shared_ptr<const EndMarker> marker{};
  EndReceipt() = default;
  EndReceipt(std::uint64_t queue, std::uint64_t sequence,
             std::uint64_t initializer = 0) noexcept
      : prefix{queue, sequence, initializer} {}
  bool valid() const noexcept {
    return prefix.valid() && (!marker ||
        (marker->owner().valid() && marker->owner().queue == prefix.queue));
  }
  Fence snapshot() const noexcept {
    auto value = prefix;
    if (marker) value.sequence = marker->sequence();
    return value;
  }
};

// Production and native truth-table binding. Unbound is NOT an empty prefix.
constexpr Poll classifyEnd(Fence snapshot, Progress progress, bool deferred,
                            bool ownerMatches) noexcept {
  if (!ownerMatches) return Poll::Invalid;
  const auto result = classify(snapshot, progress);
  return deferred && snapshot.sequence == 0 && result == Poll::Complete
      ? Poll::Pending : result;
}
inline Poll classifyEnd(const EndReceipt& receipt, Progress progress) noexcept {
  return classifyEnd(receipt.snapshot(), progress, bool(receipt.marker),
                     receipt.valid());
}
} // namespace dxmt9::query
