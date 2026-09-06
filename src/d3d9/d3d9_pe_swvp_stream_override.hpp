#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dxmt9::d3d9::pe {

// SWVP temporarily changes only stream 0's complete binding and the offset
// of any instanced stream. Keeping this snapshot bounded and typed prevents a
// temporary draw from copying/restoring unrelated stream object identities or
// strides.
template <typename Binding, std::size_t Count>
class PeSwvpStreamOverrideGuard final {
  static_assert(Count != 0u);
  static_assert(std::is_nothrow_copy_constructible_v<Binding> &&
         std::is_nothrow_copy_assignable_v<Binding>);

public:
  explicit PeSwvpStreamOverrideGuard(
    std::array<Binding, Count>& bindings) noexcept
    : bindings_(bindings), stream0_(bindings[0]) {
    for (std::size_t stream = 0; stream < Count; ++stream) {
      offsets_[stream] = bindings[stream].offset;
    }
  }

  ~PeSwvpStreamOverrideGuard() noexcept { restore(); }

  PeSwvpStreamOverrideGuard(const PeSwvpStreamOverrideGuard&) = delete;
  PeSwvpStreamOverrideGuard& operator=(const PeSwvpStreamOverrideGuard&) = delete;

  void restore() noexcept {
    if (!active_) return;
    bindings_[0] = stream0_;
    for (std::size_t stream = 1; stream < Count; ++stream) {
      bindings_[stream].offset = offsets_[stream];
    }
    active_ = false;
  }

private:
  std::array<Binding, Count>& bindings_;
  Binding stream0_{};
  std::array<std::uint32_t, Count> offsets_{};
  bool active_ = true;
};

constexpr bool peSwvpCheckedInstanceOffset(
  std::uint32_t base,
  std::uint32_t element,
  std::uint32_t stride,
  std::uint32_t& result) noexcept {
  const std::uint64_t offset =
    static_cast<std::uint64_t>(base) +
    static_cast<std::uint64_t>(element) * stride;
  if (offset > std::numeric_limits<std::uint32_t>::max()) return false;
  result = static_cast<std::uint32_t>(offset);
  return true;
}

}  // namespace dxmt9::d3d9::pe
