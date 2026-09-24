#pragma once

#include <array>
#include <cstdint>

namespace dxmt9::encoders {
inline constexpr std::uint64_t bloomMix64(std::uint64_t value, std::uint64_t salt) {
  auto x = value + salt + 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}
inline constexpr void addHazardBloom(std::array<std::uint64_t, 2>& bits,
                                    std::uint64_t value) noexcept {
  if (!value) return;
  bits[0] |= 1ull << (bloomMix64(value, 0x4d595df4d0f33173ull) & 63u);
  bits[1] |= 1ull << (bloomMix64(value, 0x9e3779b97f4a7c15ull) & 63u);
}
}  // namespace dxmt9::encoders
