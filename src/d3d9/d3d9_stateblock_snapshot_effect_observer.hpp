#pragma once

#include "d3d9_snapshot_miss_observer.hpp"

namespace dxmt9::d3d9 {

// The binding-agnostic cache clears these fields before publishing a snapshot.
// Keep the same normalization on the old side of an observation so a per-draw
// binding override cannot masquerade as a StateBlock semantic effect.
inline void clearStateBlockSnapshotBindingFields(
    core::FlatDrawStateKey& key) noexcept {
  key.streamBuffers = {};
  key.streamOffsets = {};
  key.streamStrides = {};
  key.streamMask = 0;
  key.indexBuffer = {};
}

struct StateBlockSnapshotEffectComparison {
  bool sameCanonical = false;
  bool shaderLayoutChanged = false;
  bool keyChanged = false;
  bool uniformGenerationChanged = false;
  bool uniformPayloadChanged = false;
  bool resourceIdentityChanged = false;
};

// This type is instantiated only in thread-local observer storage after the
// cached gate passes; it is never part of the cache object's hot state.
struct StateBlockSnapshotEffectBaseline {
  core::DrawShaderLayoutContext shaderLayout{};
  core::FlatDrawStateKey key{};
  SnapshotMissUniformGenerations uniformGenerations{};
  std::uint64_t uniformPayloadHash = 0;
};

// The baseline is cold observer state. Keeping it thread-local avoids putting
// the large layout/key copies in the cache method's stack frame when the
// observer is disabled; callers retain only this register-sized pointer.
inline StateBlockSnapshotEffectBaseline*
captureStateBlockSnapshotEffectBaseline(
    const core::DrawShaderLayoutContext& shaderLayout,
    const core::FlatDrawStateKey& key,
    SnapshotMissUniformGenerations uniformGenerations,
    std::uint64_t uniformPayloadHash) noexcept {
  thread_local StateBlockSnapshotEffectBaseline baseline;
  baseline.shaderLayout = shaderLayout;
  baseline.key = key;
  clearStateBlockSnapshotBindingFields(baseline.key);
  baseline.uniformGenerations = uniformGenerations;
  baseline.uniformPayloadHash = uniformPayloadHash;
  return &baseline;
}

// Value-only classification for the opt-in StateBlock miss observer. The
// cache has already rebuilt the current snapshot when this is called; this
// helper has no authority over validity, reuse, or invalidation. Generation
// changes alone deliberately remain same-canonical: they identify cache
// churn, while payload and layout/key/resource changes identify content.
inline StateBlockSnapshotEffectComparison compareStateBlockSnapshotEffect(
    const core::DrawShaderLayoutContext& previousLayout,
    const core::DrawShaderLayoutContext& currentLayout,
    const core::FlatDrawStateKey& previousKey,
    const core::FlatDrawStateKey& currentKey,
    SnapshotMissUniformGenerations previousGenerations,
    SnapshotMissUniformGenerations currentGenerations,
    std::uint64_t previousUniformPayloadHash,
    std::uint64_t currentUniformPayloadHash) noexcept {
  const auto semantic = compareSnapshotMissSemantics(
      previousLayout, currentLayout, previousKey, currentKey,
      previousGenerations, currentGenerations, previousUniformPayloadHash,
      currentUniformPayloadHash);
  const bool keyChanged = previousKey != currentKey;
  return {
      .sameCanonical = !semantic.shaderLayoutChanged && !keyChanged &&
                       !semantic.uniformPayloadChanged &&
                       !semantic.resourceIdentityChanged,
      .shaderLayoutChanged = semantic.shaderLayoutChanged,
      .keyChanged = keyChanged,
      .uniformGenerationChanged = semantic.uniformGenerationChanged,
      .uniformPayloadChanged = semantic.uniformPayloadChanged,
      .resourceIdentityChanged = semantic.resourceIdentityChanged,
  };
}

}  // namespace dxmt9::d3d9
