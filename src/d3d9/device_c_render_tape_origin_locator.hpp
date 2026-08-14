#pragma once

#include "device_c_chunk_validate.hpp"

#include <cstdint>
#include <limits>

namespace dxmt9::d3d9 {

enum class RenderTapeCommandRole : std::uint32_t {
  Unknown = 0u,
  BindingOnly,
  ShaderReadCandidate,
  RenderTargetBinding,
  DepthStencilBinding,
  CopySource,
  CopyDestination,
  ReadbackSource,
};

enum class RenderTapeOriginLocatorStatus : std::uint32_t {
  InvalidHandle = 0u,
  NotReferenced,
  MalformedRecord,
  Accepted,
};

inline constexpr std::uint32_t kRenderTapeOriginSentinel =
    std::numeric_limits<std::uint32_t>::max();

// Capture-only, value-owned provenance for one handle in an already validated
// command chunk. `originIdentity` is always copied from the chunk handle table;
// `resolvedIdentity` is supplied by the materializer after alias-parent
// recursion. No field claims that a binding was actually read or written by
// the GPU.
struct RenderTapeOriginLocator {
  RenderTapeOriginLocatorStatus status =
      RenderTapeOriginLocatorStatus::InvalidHandle;
  D9CWireObjectIdentity originIdentity{};
  D9CWireObjectIdentity resolvedIdentity{};
  std::uint32_t recordIndex = kRenderTapeOriginSentinel;
  std::uint32_t recordType = 0u;
  std::uint32_t handleIndex = kRenderTapeOriginSentinel;
  std::uint32_t sectionKind = kRenderTapeOriginSentinel;
  std::uint32_t bindingSlot = kRenderTapeOriginSentinel;
  RenderTapeCommandRole role = RenderTapeCommandRole::Unknown;
  bool aliasOrigin = false;
};

RenderTapeOriginLocator renderTapeLocateOrigin(
    const ImportedChunkView& chunk, std::uint32_t handleIndex,
    const D9CWireObjectIdentity& resolvedIdentity) noexcept;

const char* renderTapeCommandRoleName(RenderTapeCommandRole role) noexcept;
const char* renderTapeOriginLocatorStatusName(
    RenderTapeOriginLocatorStatus status) noexcept;

} // namespace dxmt9::d3d9
