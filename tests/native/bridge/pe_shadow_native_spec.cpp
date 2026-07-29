// pe_shadow_native_spec
//
// Proves d3d9_pe_state_shadow.hpp compiles and behaves natively, with no
// windows.h / d3d9.h in its transitive include set, and pins the mirrored
// D3D9 constants against the literal values they replaced.
//
// The header declares everything at global scope -- there is no namespace to
// alias here.

#include "d3d9_pe_state_shadow.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

struct TestFailure : std::runtime_error {
  explicit TestFailure(std::string message)
      : std::runtime_error(std::move(message)) {}
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void transformSlotsMatchD3DConstants() {
  std::uint32_t slot = 0;

  // D3DTS_VIEW == 2, D3DTS_PROJECTION == 3 (d3d9types.h:1201-1202).
  check(FixedTransformTable::slotForState(2u, slot), "D3DTS_VIEW must map");
  check(slot == 0u, "D3DTS_VIEW is slot 0");
  check(FixedTransformTable::slotForState(3u, slot),
        "D3DTS_PROJECTION must map");
  check(slot == 1u, "D3DTS_PROJECTION is slot 1");

  // D3DTS_TEXTURE0 == 16 .. D3DTS_TEXTURE7 == 23 (d3d9types.h:1203-1210).
  check(FixedTransformTable::slotForState(16u, slot),
        "D3DTS_TEXTURE0 must map");
  check(slot == kPeTransformTextureBaseSlot,
        "D3DTS_TEXTURE0 is the texture transform base slot");
  check(FixedTransformTable::slotForState(23u, slot),
        "D3DTS_TEXTURE7 must map");
  check(slot == kPeTransformTextureBaseSlot + 7u,
        "texture transform slots are contiguous");

  // D3DTS_WORLD == D3DTS_WORLDMATRIX(0) == 256 (d3d9types.h:102).
  check(FixedTransformTable::slotForState(256u, slot), "D3DTS_WORLD must map");
  check(slot == kPeTransformWorldBaseSlot,
        "D3DTS_WORLD is the world matrix base slot");
  check(FixedTransformTable::slotForState(259u, slot),
        "D3DTS_WORLDMATRIX(3) must map");
  check(slot == kPeTransformWorldBaseSlot + 3u,
        "world matrix slots are contiguous");

  for (const std::uint32_t state : {2u, 3u, 16u, 23u, 256u, 259u}) {
    check(FixedTransformTable::slotForState(state, slot), "state must map");
    check(FixedTransformTable::stateForSlot(slot) == state,
          "transform slot must round-trip to its state id");
  }
}

void vertexTextureSamplerSlotsMatchD3DConstants() {
  std::uint32_t slot = 0;

  // Vertex texture samplers land ABOVE the fragment sampler block:
  // slot = kPeFragmentSamplerSlots + (sampler - D3DVERTEXTEXTURESAMPLER0).
  // See d3d9_pe_state_shadow.hpp's vertexTextureSamplerSlot(). They are NOT
  // slots 0..3.
  check(!vertexTextureSamplerSlot(256u, slot),
        "D3DDMAPSAMPLER (256) is not a vertex texture sampler");
  check(vertexTextureSamplerSlot(257u, slot),
        "D3DVERTEXTEXTURESAMPLER0 must map");
  check(slot == kPeFragmentSamplerSlots,
        "D3DVERTEXTEXTURESAMPLER0 sits just above the fragment sampler block");
  check(vertexTextureSamplerSlot(260u, slot),
        "D3DVERTEXTEXTURESAMPLER3 must map");
  check(slot == kPeFragmentSamplerSlots + 3u,
        "D3DVERTEXTEXTURESAMPLER3 is three above it");
  check(!vertexTextureSamplerSlot(261u, slot),
        "261 is past D3DVERTEXTEXTURESAMPLER3");
}

void pendingMasksAreThirtyTwoBitUnsigned() {
  PeHotStateShadow shadow{};
  check(!shadow.hasPendingHotState(), "a fresh shadow has nothing pending");

  // The pending masks were DWORD. After the change they must still hold all
  // 32 bits without sign extension.
  shadow.pendingTextureMask = 0xFFFFFFFFu;
  check(shadow.pendingTextureMask == 0xFFFFFFFFu,
        "pendingTextureMask must hold 32 bits unsigned");
  check(shadow.hasPendingHotState(), "a set mask must be pending");

  shadow.clearPendingHotState();
  check(!shadow.hasPendingHotState(),
        "clearPendingHotState must clear the mask");
  check(shadow.pendingTextureMask == 0u, "mask must be zero after clear");
}

int main() {
  try {
    transformSlotsMatchD3DConstants();
    vertexTextureSamplerSlotsMatchD3DConstants();
    pendingMasksAreThirtyTwoBitUnsigned();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_shadow_native_spec FAILED: " << failure.what() << "\n";
    return 1;
  }
  std::cout << "pe_shadow_native_spec OK\n";
  return 0;
}
