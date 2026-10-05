#pragma once

#include "dxmt9/device_c.h"

#include <cstdint>

namespace dxmt9::d3d9 {

// Call-local Unix metadata only: these pointers are not wire identities and
// must never be serialized or retained as a classification cache. Production
// supplies the exact registry-retained facade and shared core Surface backing.
// Backing::valid() observes host lifetime state, not pixels or queue completion.
template <typename Backing>
[[nodiscard]] bool renderTapeClassifyPrimaryBackbuffer(
    const D9CWireObjectIdentity& requestedIdentity,
    const D9CWireObjectIdentity& retainedIdentity,
    const void* requestedDevice, const void* retainedDevice,
    const Backing* retainedBacking, const Backing* primaryBacking,
    bool terminal, std::uint32_t& outIsPrimary) noexcept {
  outIsPrimary = 0u;
  if (!requestedDevice || retainedDevice != requestedDevice || terminal ||
      requestedIdentity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
      requestedIdentity.generation == 0u || requestedIdentity.objectId == 0u ||
      retainedIdentity.kind != requestedIdentity.kind ||
      retainedIdentity.generation != requestedIdentity.generation ||
      retainedIdentity.objectId != requestedIdentity.objectId ||
      !retainedBacking || !primaryBacking || !retainedBacking->valid() ||
      !primaryBacking->valid()) {
    return false;
  }
  outIsPrimary = retainedBacking == primaryBacking ? 1u : 0u;
  return true;
}

} // namespace dxmt9::d3d9
