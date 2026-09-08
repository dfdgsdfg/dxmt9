#pragma once

#include "d3d9_pe_chunk_builder.hpp"

namespace dxmt9::d3d9::pe {

// A live slot owns a strong COM reference. Its wire identity was populated by
// the full validator when the slot was installed and is immutable for the
// lifetime of that retained wrapper. This proof is intentionally narrower
// than membership validation: it can only certify the exact retained object,
// never null, a replacement, a foreign object, or a recording write. A
// malformed wire row is rejected by shape checks; `wire.valid` only rejects
// zero/invalid-shape fields. Nonzero identity consistency relies on the
// retained slot invariant. Only strongly retained texture/shader/vertex-buffer
// stream/index-buffer callers may use this helper; borrowed declarations keep
// full validation. Stream offset/stride changes still need their typed write.
struct PeSemanticRetainedBinding {
  const void* ownerDevice = nullptr;
  const void* publicIdentity = nullptr;
  PeWireObjectRef wire{};
};

static_assert(std::is_trivially_copyable_v<PeSemanticRetainedBinding>);

inline bool peSemanticCanReuseRetainedBinding(
    bool writeAllowed, bool recording, const void* candidate,
    const PeSemanticRetainedBinding& retained, const void* expectedOwner,
    std::uint32_t expectedWireKind) noexcept {
  return writeAllowed && !recording && candidate != nullptr &&
      retained.publicIdentity == candidate &&
      retained.ownerDevice == expectedOwner && retained.wire.valid(expectedWireKind);
}

}  // namespace dxmt9::d3d9::pe
