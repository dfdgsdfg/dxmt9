#pragma once

#include "dxmt9/core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace dxmt9::resources {

// A generation-qualified logical buffer handle plus an optional concrete
// rename-ring backing.  backing == 0 means "the logical/current backing";
// otherwise it is the captured WMT::Buffer handle from a valid snapshot.
// The complete core::Handle value (including its generation) is part of the
// identity.  Never reduce it to a slot/index when retaining a stamp.
struct BufferUseStamp {
  core::Handle handle{};
  std::uint64_t backing = 0;

  friend constexpr bool operator==(const BufferUseStamp&, const BufferUseStamp&) =
      default;
};

static_assert(std::is_trivially_copyable_v<BufferUseStamp>);

// This is deliberately a fixed-size, call-local block.  It is intended to be
// filled while walking a captured resource closure and flushed with one final
// sequence ticket.  Only captured (backing != 0) stamps belong in this block;
// a logical/current-backing mark is a flush fence because the active backing
// may rotate between two marks.  Captured stamps are deduplicated only against
// the exact same full handle/backing pair in this block.
class BufferUseStampBatch {
 public:
  static constexpr std::size_t kCapacity = 64;

  // Returns false when stamp is logical (backing == 0), or when the block is
  // full and stamp is a new entry.  An invalid captured handle is accepted as
  // a no-op so callers can append the result of a bounded resource walk
  // without a side branch.  Logical marks must be flushed through the scalar
  // Pool API before continuing a captured batch.
  bool append(BufferUseStamp stamp) noexcept {
    if (!stamp.handle) {
      return true;
    }
    if (stamp.backing == 0) {
      return false;
    }
    for (const auto existing :
         std::span<const BufferUseStamp>(stamps_.data(), count_)) {
      if (existing == stamp) {
        return true;
      }
    }
    if (count_ == kCapacity) {
      return false;
    }
    stamps_[count_++] = stamp;
    return true;
  }

  bool append(core::Handle handle, std::uint64_t backing) noexcept {
    return append(BufferUseStamp{.handle = handle, .backing = backing});
  }

  void clear() noexcept { count_ = 0; }

  constexpr bool empty() const noexcept { return count_ == 0; }
  constexpr bool full() const noexcept { return count_ == kCapacity; }
  constexpr std::size_t size() const noexcept { return count_; }

  std::span<const BufferUseStamp> span() const noexcept {
    return std::span<const BufferUseStamp>(stamps_.data(), count_);
  }

 private:
  std::array<BufferUseStamp, kCapacity> stamps_{};
  std::size_t count_ = 0;
};

}  // namespace dxmt9::resources
