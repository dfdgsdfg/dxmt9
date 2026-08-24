#pragma once

#include "d3d9_pe.hpp"
#include "device_c_render_tape_capture.hpp"
#include "dxmt9/device_c.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// Render Tape-only value predicates.  Keeping these in the tape owner header
// prevents the default recorder/device include from importing capture helpers
// into every PE COM entry point.
inline bool renderTapeFormatIsBlockCompressed(std::uint32_t format) {
  switch (format) {
  case D3DFMT_DXT1:
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return true;
  default:
    return false;
  }
}

inline bool renderTapeDescriptorSubresourceCountFits(
    std::uint32_t count, std::size_t headerBytes) noexcept {
  if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
    return true;
  } else {
    return count <=
           (std::numeric_limits<std::size_t>::max() - headerBytes) /
               sizeof(D9CSurfaceDesc);
  }
}

inline bool renderTapeTextureSubresourceDescriptor(
    std::span<const std::byte> descriptor, std::uint32_t subresource,
    D9CSurfaceDesc &out) noexcept {
  return dxmt9::d3d9::renderTapeTextureSubresourceDescriptor(
      descriptor, subresource, out);
}

inline bool renderTapeValidateExpectedContent(
    const D9CWireObjectIdentity &identity,
    std::span<const std::byte> descriptor,
    std::span<const std::vector<std::byte>> content,
    dxmt9::d3d9::RenderTapeExpectedContentContract &contract) noexcept {
  try {
    std::vector<std::uint64_t> expected(content.size());
    contract = dxmt9::d3d9::renderTapeDeriveExpectedContentContract(
        identity.kind, descriptor, expected);
    if (contract.status ==
        dxmt9::d3d9::RenderTapeExpectedContentStatus::NotRequired)
      return contract.bytes == 0u && contract.count == 0u && content.empty();
    if (contract.status !=
            dxmt9::d3d9::RenderTapeExpectedContentStatus::Accepted ||
        contract.count != content.size())
      return false;
    std::uint64_t total = 0u;
    for (std::size_t index = 0u; index < content.size(); ++index) {
      if (expected[index] != content[index].size() ||
          total > std::numeric_limits<std::uint64_t>::max() - expected[index])
        return false;
      total += expected[index];
    }
    return total == contract.bytes;
  } catch (...) {
    contract = {};
    return false;
  }
}

inline bool renderTapeSameIdentity(const D9CWireObjectIdentity &a,
                                   const D9CWireObjectIdentity &b) noexcept {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}
