#include "../../../src/d3d9/d3d9_stateblock_snapshot_effect_observer.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool value, std::string_view message) {
  if (!value) {
    throw TestFailure(std::string(message));
  }
}

void stateBlockSnapshotEffectSeparatesCanonicalDimensions() {
  dxmt9::core::DrawShaderLayoutContext layout;
  dxmt9::core::FlatDrawStateKey key;
  const dxmt9::d3d9::SnapshotMissUniformGenerations generations{
      .aggregate = 4, .vertexConstants = 5, .pixelConstants = 6};
  const auto compare = [&](const auto& previousLayout,
                           const auto& currentLayout,
                           const auto& previousKey, const auto& currentKey,
                           auto previousGenerations,
                           auto currentGenerations, auto previousPayload,
                           auto currentPayload) {
    return dxmt9::d3d9::compareStateBlockSnapshotEffect(
        previousLayout, currentLayout, previousKey, currentKey,
        previousGenerations, currentGenerations, previousPayload,
        currentPayload);
  };

  const auto unchanged =
      compare(layout, layout, key, key, generations, generations, 9, 9);
  check(unchanged.sameCanonical && !unchanged.keyChanged,
        "unchanged StateBlock snapshot is canonical");

  auto changedKey = key;
  changedKey.renderStateHash = 17;
  const auto keyEffect = compare(layout, layout, key, changedKey, generations,
                                 generations, 9, 9);
  check(!keyEffect.sameCanonical && keyEffect.keyChanged &&
            !keyEffect.resourceIdentityChanged,
        "render-state key effect is classified separately");

  auto changedResource = key;
  changedResource.textures[0] = dxmt9::core::Handle{42};
  const auto resourceEffect =
      compare(layout, layout, key, changedResource, generations, generations,
              9, 9);
  check(!resourceEffect.sameCanonical && resourceEffect.keyChanged &&
            resourceEffect.resourceIdentityChanged,
        "resource identity effect remains visible in canonical key");

  auto changedGeneration = generations;
  ++changedGeneration.vertexConstants;
  const auto generationEffect =
      compare(layout, layout, key, key, generations, changedGeneration, 9, 9);
  check(generationEffect.sameCanonical &&
            generationEffect.uniformGenerationChanged,
        "generation-only churn remains canonical");

  const auto payloadEffect =
      compare(layout, layout, key, key, generations, generations, 9, 10);
  check(!payloadEffect.sameCanonical && payloadEffect.uniformPayloadChanged,
        "uniform payload effect breaks canonical equality");

  auto changedLayout = layout;
  changedLayout.clipPlaneMask = 1;
  const auto layoutEffect = compare(layout, changedLayout, key, key,
                                    generations, generations, 9, 9);
  check(!layoutEffect.sameCanonical && layoutEffect.shaderLayoutChanged,
        "shader layout effect breaks canonical equality");
}

void stateBlockSnapshotEffectClearsBindingFields() {
  auto key = dxmt9::core::FlatDrawStateKey{};
  key.streamBuffers[0] = dxmt9::core::Handle{7};
  key.streamOffsets[0] = 8;
  key.streamStrides[0] = 9;
  key.streamMask = 1;
  key.indexBuffer = dxmt9::core::Handle{10};
  dxmt9::d3d9::clearStateBlockSnapshotBindingFields(key);
  check(key.streamBuffers[0] == dxmt9::core::Handle{} &&
            key.streamOffsets[0] == 0 && key.streamStrides[0] == 0 &&
            key.streamMask == 0 && key.indexBuffer == dxmt9::core::Handle{},
        "binding fields are normalized before comparison");
}

}  // namespace

int main() {
  try {
    stateBlockSnapshotEffectSeparatesCanonicalDimensions();
    stateBlockSnapshotEffectClearsBindingFields();
  } catch (const TestFailure& failure) {
    std::cerr << "stateblock_snapshot_effect_observer_spec: "
              << failure.what() << '\n';
    return 1;
  }
  std::cout << "stateblock_snapshot_effect_observer_spec: ok\n";
  return 0;
}
