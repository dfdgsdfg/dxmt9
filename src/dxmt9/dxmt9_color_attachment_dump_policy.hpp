#pragma once

#include "winemetal/winemetal.h"

#include <cstdint>
#include <limits>

namespace dxmt9::encoders {

// A necessary readback qualification, not proof that every pixel/channel was
// initialized or live. Resolve-only does not preserve the source attachment.
constexpr bool colorAttachmentDumpHasStoredContents(
    bool witnessKnown, WMTStoreAction action, std::uint32_t sampleCount) noexcept {
  return witnessKnown && sampleCount == 1u &&
      (action == WMTStoreActionStore || action == WMTStoreActionStoreAndMultisampleResolve);
}

// Diagnostic-local cursor for one queue's dedicated encode thread. This is an
// encoded Present ordinal, not PE capture authority or GPU completion evidence.
struct ColorAttachmentDumpFrameCursor {
  std::uintptr_t owner = 0u;
  std::uint64_t lastSeq = 0u;
  std::uint64_t frame = 1u;

  constexpr std::uint64_t observe(std::uintptr_t currentOwner,
                                  std::uint64_t seq) noexcept {
    if (owner != currentOwner || seq < lastSeq) {
      owner = currentOwner;
      frame = 1u;
    }
    lastSeq = seq;
    return frame;
  }

  constexpr void present(std::uintptr_t currentOwner,
                         std::uint64_t seq) noexcept {
    observe(currentOwner, seq);
    if (frame != std::numeric_limits<std::uint64_t>::max()) ++frame;
  }
};

constexpr bool colorAttachmentDumpFramePolicyValid(
    bool hasFrame, std::uint64_t frame, bool hasMaxDumps,
    std::uint64_t maxDumps, bool hashOnly = false) noexcept {
  return (!hasFrame || (frame != 0u && hasMaxDumps)) &&
      (!hashOnly || hasMaxDumps) &&
      (!hasMaxDumps || (maxDumps != 0u &&
                       maxDumps <= (hashOnly ? 16384u : 256u)));
}

constexpr bool colorAttachmentDepthPairPolicyValid(
    bool enabled, bool hashOnly, bool afterDraw, std::uint64_t maxDumps) noexcept {
  return !enabled || (hashOnly && !afterDraw && maxDumps != 0u && maxDumps <= 256u);
}

constexpr bool colorAttachmentDepthPairHasStoredContents(
    bool known, WMTStoreAction action, std::uint32_t sampleCount,
    bool supportedFloatDepth, std::uint64_t width, std::uint64_t height,
    std::uint64_t colorWidth, std::uint64_t colorHeight) noexcept {
  return known && action == WMTStoreActionStore && sampleCount == 1u &&
      supportedFloatDepth && width != 0u && height != 0u &&
      width == colorWidth && height == colorHeight &&
      width <= std::numeric_limits<std::uint32_t>::max() / 4u &&
      height <= std::numeric_limits<std::uint32_t>::max() / (width * 4u);
}

}  // namespace dxmt9::encoders
