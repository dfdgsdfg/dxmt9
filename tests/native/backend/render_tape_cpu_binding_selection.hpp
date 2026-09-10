#pragma once

#include <cstdint>

namespace dxmt9::test {

// Cold projection of the encoder's resource/UP selection, not a new
// production binding policy. Inline bytes are observed separately.
inline constexpr bool cpuTapeObservesStreamBuffer(
    unsigned stream, std::uint32_t mask, bool hasBuffer,
    bool hasInlineVertices) noexcept {
  return stream < 16u && (mask & (1u << stream)) && hasBuffer &&
         !(stream == 0u && hasInlineVertices);
}

inline constexpr bool cpuTapeObservesIndexBuffer(
    bool indexed, bool hasBuffer, bool hasInlineIndices) noexcept {
  return indexed && hasBuffer && !hasInlineIndices;
}

} // namespace dxmt9::test
