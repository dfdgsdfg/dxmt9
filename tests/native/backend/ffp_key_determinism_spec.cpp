// Determinism + sensitivity regression guard for the FFP variant keys
// `core::FfpVertexKey` / `core::FfpPixelKey`. The PSO cache and the
// shader-source cache both hash these keys; a same-state-different-key
// regression silently fragments those caches, and a different-state-same-key
// regression silently merges unrelated shader variants. Both directions
// must hold across `makeFfpVertexKey()` / `makeFfpPixelKey()` and across
// direct brace-init fixtures.
//
// See: `include/dxmt9/core_constants.hpp` (key definitions + lifecycle
// comment), `include/dxmt9/core_snapshots.hpp` (builder declarations),
// `src/d3d9/core_draw.cpp` (builder implementations).

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "dxmt9/core.hpp"

using namespace dxmt9::core;

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkNe(const A& left, const B& right, std::string_view message) {
  if (left == right) {
    fail(std::string(message));
  }
}

// Builds a DeviceState that exercises a representative cross-section of
// the FFP key inputs (lighting, fog, color material, alpha test, two
// active stages, texcoord transforms, vertex blend). Same state every
// call -- the determinism test relies on a stable fixture.
DeviceState makeRepresentativeFfpState() {
  DeviceState state;
  state.reset();

  // Vertex-side FFP state.
  state.renderStates[RS_LIGHTING] = 1;
  state.renderStates[RS_SPECULAR_ENABLE] = 1;
  state.renderStates[RS_NORMALIZE_NORMALS] = 1;
  state.renderStates[RS_EMISSIVE_MATERIAL_SOURCE] = 3;
  state.renderStates[RS_AMBIENT_MATERIAL_SOURCE] = 2;
  state.renderStates[RS_DIFFUSE_MATERIAL_SOURCE] = 1;
  state.renderStates[RS_SPECULAR_MATERIAL_SOURCE] = 0;
  state.renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  state.renderStates[RS_FOG_FROM_VERTEX] = 0;
  state.renderStates[RS_RANGE_FOG] = 0;
  state.renderStates[RS_VERTEX_BLEND] = 2;
  state.renderStates[RS_INDEXED_VERTEX_BLEND_ENABLE] = 0;
  state.renderStates[RS_CLIP_PLANE_ENABLE] = 0b1011;

  state.lightEnabled[0] = true;
  state.lights[0].type = LightType::Point;
  state.lightEnabled[1] = true;
  state.lights[1].type = LightType::Directional;

  // Pixel-side FFP state.
  state.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  state.renderStates[RS_ALPHA_FUNC] =
      static_cast<u32>(CompareFunc::GreaterEqual);

  state.textureStageStates[0][TSS_COLOR_OP] =
      static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[0][TSS_COLOR_ARG1] = 2u;
  state.textureStageStates[0][TSS_COLOR_ARG2] = 0u;
  state.textureStageStates[0][TSS_ALPHA_OP] =
      static_cast<u32>(TextureOp::Modulate);
  state.textureStageStates[0][TSS_ALPHA_ARG1] = 2u;
  state.textureStageStates[0][TSS_ALPHA_ARG2] = 1u;
  state.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4u;
  state.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7u;
  state.textureStageStates[0][TSS_TEXTURE_TYPE] = 2u;

  state.textureStageStates[1][TSS_COLOR_OP] =
      static_cast<u32>(TextureOp::Modulate);
  state.textureStageStates[1][TSS_COLOR_ARG1] = 2u;
  state.textureStageStates[1][TSS_COLOR_ARG2] = 1u;
  state.textureStageStates[1][TSS_ALPHA_OP] =
      static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[1][TSS_TEXCOORD_INDEX] = 1u;

  return state;
}

constexpr int kRepeatN = 100;

// Case 1: makeFfpVertexKey() is bit-stable across N invocations on a
// fixed DeviceState.
void testVertexKeyDeterministicAcrossRepeatedBuilds() {
  const auto state = makeRepresentativeFfpState();
  const auto baseline = makeFfpVertexKey(state);
  check(baseline.hash != 0, "vertex key baseline hash is nonzero");

  for (int i = 0; i < kRepeatN; ++i) {
    const auto k = makeFfpVertexKey(state);
    checkEq(k, baseline, "vertex key drifted across repeated builds");
    checkEq(k.hash, baseline.hash,
            "vertex key hash drifted across repeated builds");
  }
}

// Case 2: makeFfpPixelKey() is bit-stable across N invocations on a
// fixed DeviceState.
void testPixelKeyDeterministicAcrossRepeatedBuilds() {
  const auto state = makeRepresentativeFfpState();
  const auto baseline = makeFfpPixelKey(state);
  check(baseline.hash != 0, "pixel key baseline hash is nonzero");

  for (int i = 0; i < kRepeatN; ++i) {
    const auto k = makeFfpPixelKey(state);
    checkEq(k, baseline, "pixel key drifted across repeated builds");
    checkEq(k.hash, baseline.hash,
            "pixel key hash drifted across repeated builds");
  }
}

// Case 3: every TSS field that the pixel key reads must be sensitive --
// a single-bit perturbation MUST flip the key (operator==) and the hash.
void testPixelKeySensitiveToTssPerturbations() {
  const auto base = makeRepresentativeFfpState();
  const auto baseKey = makeFfpPixelKey(base);

  struct PerturbationCase {
    std::string_view name;
    u32 tss;
    u32 delta;
  };
  const std::array<PerturbationCase, 8> cases{{
      {"TSS_COLOR_OP@stage0", TSS_COLOR_OP, 1u},
      {"TSS_COLOR_ARG1@stage0", TSS_COLOR_ARG1, 1u},
      {"TSS_COLOR_ARG2@stage0", TSS_COLOR_ARG2, 1u},
      {"TSS_ALPHA_OP@stage0", TSS_ALPHA_OP, 1u},
      {"TSS_ALPHA_ARG1@stage0", TSS_ALPHA_ARG1, 1u},
      {"TSS_RESULT_ARG@stage0", TSS_RESULT_ARG, 1u},
      {"TSS_TEXTURE_TYPE@stage0", TSS_TEXTURE_TYPE, 1u},
      {"TSS_TEXCOORD_INDEX@stage0", TSS_TEXCOORD_INDEX, 1u},
  }};
  for (const auto& c : cases) {
    auto perturbed = base;
    const u32 prior =
        static_cast<u32>(perturbed.textureStageStates[0][c.tss]);
    perturbed.textureStageStates[0][c.tss] = prior ^ c.delta;
    const auto k = makeFfpPixelKey(perturbed);
    checkNe(k, baseKey, "pixel key did not change under TSS perturbation");
    checkNe(k.hash, baseKey.hash,
            "pixel key hash did not change under TSS perturbation");
  }

  // Stage-2 perturbation also flips the key (proves cross-stage
  // sensitivity, not just stage-0 coverage).
  auto stage2 = base;
  stage2.textureStageStates[2][TSS_COLOR_OP] =
      static_cast<u32>(TextureOp::Add);
  const auto stage2Key = makeFfpPixelKey(stage2);
  checkNe(stage2Key, baseKey,
          "pixel key insensitive to stage-2 TSS_COLOR_OP");

  // Alpha-test toggles flip the key too (RS-side, but still pixel-key).
  auto alphaOff = base;
  alphaOff.renderStates[RS_ALPHA_TEST_ENABLE] = 0;
  const auto alphaOffKey = makeFfpPixelKey(alphaOff);
  checkNe(alphaOffKey, baseKey,
          "pixel key insensitive to RS_ALPHA_TEST_ENABLE toggle");

  auto alphaFunc = base;
  alphaFunc.renderStates[RS_ALPHA_FUNC] =
      static_cast<u32>(CompareFunc::Less);
  const auto alphaFuncKey = makeFfpPixelKey(alphaFunc);
  checkNe(alphaFuncKey, baseKey,
          "pixel key insensitive to RS_ALPHA_FUNC change");
}

// Case 4: VS-side perturbations (light enable, light type, vertex blend
// shape, clip plane mask, texcoord transform flags) flip the vertex key.
void testVertexKeySensitiveToVsStatePerturbations() {
  const auto base = makeRepresentativeFfpState();
  const auto baseKey = makeFfpVertexKey(base);

  // Light-enable toggle.
  auto lightOff = base;
  lightOff.lightEnabled[0] = false;
  checkNe(makeFfpVertexKey(lightOff), baseKey,
          "vertex key insensitive to lightEnabled[0] toggle");

  // Light-type change.
  auto lightType = base;
  lightType.lights[0].type = LightType::Spot;
  checkNe(makeFfpVertexKey(lightType), baseKey,
          "vertex key insensitive to lights[0].type change");

  // Vertex blend change.
  auto vertexBlend = base;
  vertexBlend.renderStates[RS_VERTEX_BLEND] = 3u;
  checkNe(makeFfpVertexKey(vertexBlend), baseKey,
          "vertex key insensitive to RS_VERTEX_BLEND change");

  // Indexed-vertex-blend toggle.
  auto indexedBlend = base;
  indexedBlend.renderStates[RS_INDEXED_VERTEX_BLEND_ENABLE] = 1u;
  checkNe(makeFfpVertexKey(indexedBlend), baseKey,
          "vertex key insensitive to RS_INDEXED_VERTEX_BLEND_ENABLE");

  // Clip plane mask change.
  auto clip = base;
  clip.renderStates[RS_CLIP_PLANE_ENABLE] = 0xffu;
  checkNe(makeFfpVertexKey(clip), baseKey,
          "vertex key insensitive to RS_CLIP_PLANE_ENABLE change");

  // Texcoord transform flag change.
  auto transform = base;
  transform.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 3u;
  checkNe(makeFfpVertexKey(transform), baseKey,
          "vertex key insensitive to TSS_TEXTURE_TRANSFORM_FLAGS change");

  // Color-material source change.
  auto materialSource = base;
  materialSource.renderStates[RS_EMISSIVE_MATERIAL_SOURCE] = 0u;
  checkNe(makeFfpVertexKey(materialSource), baseKey,
          "vertex key insensitive to RS_EMISSIVE_MATERIAL_SOURCE change");

  // COLORVERTEX gates all vertex-color material sources back to material.
  auto colorVertexOff = base;
  colorVertexOff.renderStates[141u /*RS_COLORVERTEX*/] = 0u;
  const auto colorVertexOffKey = makeFfpVertexKey(colorVertexOff);
  checkNe(colorVertexOffKey, baseKey,
          "vertex key insensitive to RS_COLORVERTEX toggle");
  check(!colorVertexOffKey.colorVertexEnabled,
        "RS_COLORVERTEX=false is preserved in the FFP vertex key");
  for (u32 mode : colorVertexOffKey.colorMaterialMode) {
    checkEq(mode, 0u,
            "RS_COLORVERTEX=false forces material-source modes to material");
  }

  // Lighting toggle.
  auto lightingOff = base;
  lightingOff.renderStates[RS_LIGHTING] = 0u;
  checkNe(makeFfpVertexKey(lightingOff), baseKey,
          "vertex key insensitive to RS_LIGHTING toggle");

  // Normalize-normals toggle.
  auto normalize = base;
  normalize.renderStates[RS_NORMALIZE_NORMALS] = 0u;
  checkNe(makeFfpVertexKey(normalize), baseKey,
          "vertex key insensitive to RS_NORMALIZE_NORMALS toggle");
}

// Case 7 (P1-4): point-sprite + point-scale enable bits are key bits on
// both the vertex and pixel sides; the six size/scale uniforms must NOT
// flip the key (they ride the uniform record). Combined enables produce
// keys distinct from either single-enable variant.
void testPointSpriteAndScaleKeyBitsAreKeyOnly() {
  const auto base = makeRepresentativeFfpState();
  const auto baseVk = makeFfpVertexKey(base);
  const auto basePk = makeFfpPixelKey(base);

  // POINTSPRITEENABLE flips both vertex AND pixel keys.
  auto sprite = base;
  sprite.renderStates[RS_POINT_SPRITE_ENABLE] = 1u;
  const auto spriteVk = makeFfpVertexKey(sprite);
  const auto spritePk = makeFfpPixelKey(sprite);
  checkNe(spriteVk, baseVk,
          "vertex key insensitive to RS_POINT_SPRITE_ENABLE toggle");
  checkNe(spritePk, basePk,
          "pixel key insensitive to RS_POINT_SPRITE_ENABLE toggle");

  // POINTSCALEENABLE flips only the vertex key (FS does not branch on it).
  auto scale = base;
  scale.renderStates[RS_POINT_SCALE_ENABLE] = 1u;
  const auto scaleVk = makeFfpVertexKey(scale);
  const auto scalePk = makeFfpPixelKey(scale);
  checkNe(scaleVk, baseVk,
          "vertex key insensitive to RS_POINT_SCALE_ENABLE toggle");
  checkEq(scalePk, basePk,
          "pixel key incorrectly sensitive to RS_POINT_SCALE_ENABLE");

  // Both enabled produces a vertex key distinct from either single-enable.
  auto both = base;
  both.renderStates[RS_POINT_SPRITE_ENABLE] = 1u;
  both.renderStates[RS_POINT_SCALE_ENABLE] = 1u;
  const auto bothVk = makeFfpVertexKey(both);
  checkNe(bothVk, baseVk,
          "vertex key insensitive to combined POINTSPRITE+POINTSCALE");
  checkNe(bothVk, spriteVk,
          "vertex key sprite==sprite+scale (POINTSCALE absorbed)");
  checkNe(bothVk, scaleVk,
          "vertex key scale==sprite+scale (POINTSPRITE absorbed)");

  // The six size/scale uniforms (POINTSIZE, _MIN, _MAX, SCALE_A/B/C) are
  // uniform-only and MUST NOT flip the variant key. They participate in
  // the FfpVsConsts record instead -- buildFfpVsConsts coverage is
  // exercised by the backend-side ffp uniform fixtures.
  const std::array<u32, 6> uniformRs{
      RS_POINTSIZE,     RS_POINTSIZE_MIN, RS_POINTSIZE_MAX,
      RS_POINTSCALE_A, RS_POINTSCALE_B, RS_POINTSCALE_C,
  };
  for (u32 rs : uniformRs) {
    auto perturbed = base;
    // 0x3fc00000 = 1.5f bit pattern; any non-default value works.
    perturbed.renderStates[rs] = 0x3fc00000u;
    checkEq(makeFfpVertexKey(perturbed), baseVk,
            "vertex key incorrectly sensitive to a size/scale uniform RS");
    checkEq(makeFfpPixelKey(perturbed), basePk,
            "pixel key incorrectly sensitive to a size/scale uniform RS");
  }
}

// Case 8 (P1-4): determinism — repeated builds with point-sprite or
// point-scale enabled produce byte-identical keys (including hash).
void testPointStateKeysDeterministic() {
  auto state = makeRepresentativeFfpState();
  state.renderStates[RS_POINT_SPRITE_ENABLE] = 1u;
  state.renderStates[RS_POINT_SCALE_ENABLE] = 1u;
  state.renderStates[RS_POINTSIZE] = 0x40400000u;       // 3.0f
  state.renderStates[RS_POINTSIZE_MIN] = 0x3f800000u;   // 1.0f
  state.renderStates[RS_POINTSIZE_MAX] = 0x42800000u;   // 64.0f
  state.renderStates[RS_POINTSCALE_A] = 0x3f800000u;
  state.renderStates[RS_POINTSCALE_B] = 0x3dcccccdu;    // 0.1f
  state.renderStates[RS_POINTSCALE_C] = 0x3c23d70au;    // 0.01f

  const auto baselineVk = makeFfpVertexKey(state);
  const auto baselinePk = makeFfpPixelKey(state);
  check(baselineVk.hash != 0,
        "point-state vertex key baseline hash is nonzero");
  check(baselinePk.hash != 0,
        "point-state pixel key baseline hash is nonzero");
  for (int i = 0; i < kRepeatN; ++i) {
    checkEq(makeFfpVertexKey(state), baselineVk,
            "point-state vertex key drifted across repeated builds");
    checkEq(makeFfpPixelKey(state), baselinePk,
            "point-state pixel key drifted across repeated builds");
  }
}

// Case 4b (G2-C): per-light Type byte participates in the FFP vertex key
// (D3D9 §B.5 — Directional / Point / Spot lower to different MSL
// bodies). Three independent fixtures, identical except for the slot-0
// light type, must produce three distinct keys and three distinct
// hashes.
void testVertexKeySensitiveToPerLightType() {
  auto baseState = makeRepresentativeFfpState();
  // Pin all lights so only slot 0's type varies in the test below.
  for (size_t i = 0; i < kMaxLights; ++i) {
    baseState.lightEnabled[i] = (i < 2);
  }
  baseState.lights[1].type = LightType::Directional;

  baseState.lights[0].type = LightType::Directional;
  const auto k1 = makeFfpVertexKey(baseState);
  baseState.lights[0].type = LightType::Point;
  const auto k2 = makeFfpVertexKey(baseState);
  baseState.lights[0].type = LightType::Spot;
  const auto k3 = makeFfpVertexKey(baseState);

  checkEq(k1.lightType[0], static_cast<u32>(LightType::Directional),
          "directional fixture light type");
  checkEq(k2.lightType[0], static_cast<u32>(LightType::Point),
          "point fixture light type");
  checkEq(k3.lightType[0], static_cast<u32>(LightType::Spot),
          "spot fixture light type");
  checkNe(k1, k2, "Directional/Point keys must differ on slot 0 type");
  checkNe(k1, k3, "Directional/Spot keys must differ on slot 0 type");
  checkNe(k2, k3, "Point/Spot keys must differ on slot 0 type");
  checkNe(k1.hash, k2.hash, "Directional/Point hashes must differ");
  checkNe(k1.hash, k3.hash, "Directional/Spot hashes must differ");
  checkNe(k2.hash, k3.hash, "Point/Spot hashes must differ");

  // Slot-N sensitivity: flipping slot 1 also flips the key (per-slot
  // hashing, not "any slot changed").
  auto slot1 = baseState;
  slot1.lights[0].type = LightType::Directional;
  slot1.lights[1].type = LightType::Spot;
  const auto k1Slot1Spot = makeFfpVertexKey(slot1);
  checkNe(k1Slot1Spot, k1,
          "Slot-1 type change must flip vertex key independently");
}

// Case 5: directly brace-initialized keys round-trip through operator==
// and copy-construction. Covers test fixtures that bypass `DeviceState`
// and the canonical builders. We do NOT compare a brace-init key against
// a builder output here -- the builder picks up `DeviceState::reset()`
// defaults (e.g. `RS_DIFFUSE_MATERIAL_SOURCE = 1`, `RS_FOG_FROM_VERTEX =
// 1`, `Light::type = Directional` on every entry) that a minimal
// brace-init would not mirror; that lockstep is the brace-init author's
// responsibility, not the builder's. The invariant guarded here is just
// "two structurally-equal keys are operator==-equal across N rounds."
void testDirectInitKeyOperatorEqualityIsStable() {
  FfpVertexKey vk{};
  vk.lightingEnabled = true;
  vk.specularEnabled = true;
  vk.lightEnabled[0] = true;
  vk.lightType[0] = static_cast<u32>(LightType::Point);
  vk.fogMode = FogMode::Exp2;
  vk.texCoordGen[0] = 4u;
  vk.texTransformFlags[0] = 7u;
  vk.vertexBlend = 2u;
  vk.clipPlaneMask = 0b1011u;
  vk.hash = 0xdeadbeefcafebabeull;

  FfpPixelKey pk{};
  pk.stages[0].colorOp = static_cast<u32>(TextureOp::SelectArg1);
  pk.stages[0].alphaOp = static_cast<u32>(TextureOp::Modulate);
  pk.stages[0].texCoordIndex = 4u;
  pk.fogMode = FogMode::Exp2;
  pk.alphaTestEnable = true;
  pk.alphaTestFunc = static_cast<u32>(CompareFunc::GreaterEqual);
  pk.hash = 0x12345678abcdef00ull;

  for (int i = 0; i < kRepeatN; ++i) {
    FfpVertexKey vkCopy = vk;
    FfpPixelKey pkCopy = pk;
    checkEq(vkCopy, vk, "direct-init vertex key copy diverged");
    checkEq(pkCopy, pk, "direct-init pixel key copy diverged");
  }

  // Mutating the cached hash field alone is enough to break operator==
  // (the comparison is whole-struct including `hash`).
  FfpVertexKey vkMutated = vk;
  vkMutated.hash ^= 1ull;
  checkNe(vkMutated, vk, "vertex key operator== ignores hash mutation");
  FfpPixelKey pkMutated = pk;
  pkMutated.hash ^= 1ull;
  checkNe(pkMutated, pk, "pixel key operator== ignores hash mutation");
}

// Case 6: equivalent DeviceState payloads expressed two ways (e.g. with
// the same map entries inserted in a different order, or with the
// implicit-default 0 left absent vs. explicitly set to 0) produce the
// same key. Guards against accidental order-dependence in the builder.
void testBuilderIgnoresMapInsertionOrder() {
  DeviceState a;
  a.reset();
  a.renderStates[RS_LIGHTING] = 1u;
  a.renderStates[RS_SPECULAR_ENABLE] = 1u;
  a.renderStates[RS_ALPHA_TEST_ENABLE] = 1u;
  a.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::Less);
  a.textureStageStates[0][TSS_COLOR_OP] =
      static_cast<u32>(TextureOp::Modulate);
  a.textureStageStates[0][TSS_ALPHA_OP] =
      static_cast<u32>(TextureOp::SelectArg1);

  DeviceState b;
  b.reset();
  // Same final state values, inserted in a different order.
  b.textureStageStates[0][TSS_ALPHA_OP] =
      static_cast<u32>(TextureOp::SelectArg1);
  b.textureStageStates[0][TSS_COLOR_OP] =
      static_cast<u32>(TextureOp::Modulate);
  b.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::Less);
  b.renderStates[RS_ALPHA_TEST_ENABLE] = 1u;
  b.renderStates[RS_SPECULAR_ENABLE] = 1u;
  b.renderStates[RS_LIGHTING] = 1u;

  checkEq(makeFfpVertexKey(a), makeFfpVertexKey(b),
          "vertex builder is order-dependent on render-state insertion");
  checkEq(makeFfpPixelKey(a), makeFfpPixelKey(b),
          "pixel builder is order-dependent on texture-stage insertion");
}

}  // namespace

int main() {
  try {
    testVertexKeyDeterministicAcrossRepeatedBuilds();
    testPixelKeyDeterministicAcrossRepeatedBuilds();
    testPixelKeySensitiveToTssPerturbations();
    testVertexKeySensitiveToVsStatePerturbations();
    testVertexKeySensitiveToPerLightType();
    testDirectInitKeyOperatorEqualityIsStable();
    testBuilderIgnoresMapInsertionOrder();
    testPointSpriteAndScaleKeyBitsAreKeyOnly();
    testPointStateKeysDeterministic();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
