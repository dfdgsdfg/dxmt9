#pragma once

#include "dxmt9/core_snapshots.hpp"

#include <cstdint>

namespace dxmt9::d3d9 {

struct SnapshotMissSemanticComparison {
  bool shaderLayoutChanged = false;
  bool uniformGenerationChanged = false;
  bool uniformPayloadChanged = false;
  bool resourceIdentityChanged = false;

  constexpr bool sameSemantic() const noexcept {
    // Generation churn is a cache/versioning event, not a semantic change by
    // itself. Payload is compared separately so callers can distinguish a
    // generation-only rebuild from a changed uniform value.
    return !shaderLayoutChanged && !uniformPayloadChanged &&
           !resourceIdentityChanged;
  }
};

struct SnapshotMissUniformGenerations {
  std::uint64_t aggregate = 0;
  std::uint64_t vertexConstants = 0;
  std::uint64_t pixelConstants = 0;

  friend constexpr bool operator==(const SnapshotMissUniformGenerations&,
                                   const SnapshotMissUniformGenerations&) =
      default;
};

// This is deliberately a value-only classifier. It is called only by the
// opt-in miss observer, after the normal cache rebuild, and therefore does not
// participate in cache validity or rendering decisions. Resource identity is
// limited to handles and attachment identity available in FlatDrawStateKey.
inline SnapshotMissSemanticComparison compareSnapshotMissSemantics(
    const core::DrawShaderLayoutContext& previousLayout,
    const core::DrawShaderLayoutContext& currentLayout,
    const core::FlatDrawStateKey& previousKey,
    const core::FlatDrawStateKey& currentKey,
    SnapshotMissUniformGenerations previousGenerations,
    SnapshotMissUniformGenerations currentGenerations,
    std::uint64_t previousUniformPayloadHash,
    std::uint64_t currentUniformPayloadHash) noexcept {
  const auto resourcesEqual = [&] {
    return previousKey.streamBuffers == currentKey.streamBuffers &&
           previousKey.indexBuffer == currentKey.indexBuffer &&
           previousKey.textures == currentKey.textures &&
           previousKey.textureLods == currentKey.textureLods &&
           previousKey.textureMask == currentKey.textureMask &&
           previousKey.colorAttachments == currentKey.colorAttachments &&
           previousKey.depthStencil == currentKey.depthStencil &&
           previousKey.renderTargetMask == currentKey.renderTargetMask;
  };
  return {
      .shaderLayoutChanged = previousLayout != currentLayout,
      .uniformGenerationChanged = previousGenerations != currentGenerations,
      .uniformPayloadChanged =
          previousUniformPayloadHash != currentUniformPayloadHash,
      .resourceIdentityChanged = !resourcesEqual(),
  };
}

}  // namespace dxmt9::d3d9
