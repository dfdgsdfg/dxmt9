#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

namespace dxmt9::d3d9::pe {

// Visit valid mask positions in wire/canonical order without touching the
// inactive domain. A false callback result stops at the same first failing
// occurrence as an ascending slot loop.
// Keep the loop at the original call site: release PE32/PE64 otherwise outline
// each small visitor specialization and add calls even for an empty mask.
template <typename Visit>
  requires std::is_nothrow_invocable_r_v<bool, Visit&, std::uint32_t>
__attribute__((always_inline)) inline bool
forEachSetBitAscending(std::uint32_t mask, Visit&& visit) noexcept {
  while (mask != 0u) {
    const auto bit = static_cast<std::uint32_t>(std::countr_zero(mask));
    if (!visit(bit)) return false;
    mask &= mask - 1u;
  }
  return true;
}

}  // namespace dxmt9::d3d9::pe
