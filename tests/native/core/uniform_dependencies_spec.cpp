// R-BACK-2.15.5: bounded value/dependency and temporal refinement. No GPU,
// allocator, scheduling, constant-usage or whole-program proof is claimed.
#include "dxmt9/core.hpp"
#include "../../../src/d3d9/d3d9_uniform_dependencies.hpp"
#include "../../../src/d3d9/d3d9_uniform_dependency_observer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::d3d9;

void check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "uniform dependency spec: %s\n", message);
    std::exit(1);
  }
}

bool sameFixed(const DrawUniformFixedPayload& a, const DrawUniformPayload& b) {
  const auto groups = compareUniformNonconstantValues(a, b);
  return std::all_of(groups.begin(), groups.end(), [](bool same) { return same; });
}

void observerValues() {
  DrawUniformPayload a{};
  const auto fixed = makeDrawUniformFixedPayload(a);
  check(sameFixed(fixed, a), "identical payload groups compare equal");
  for (unsigned group = 0; group < 5; ++group) {
    auto b = a;
    switch (group) {
    case 0: b.ffpBlendNormalMatrix[3].m[7] = -0.0f; break;
    case 1: b.material.power = -0.0f; break;
    case 2: b.lights[7].theta = -0.0f; break;
    case 3: b.textureTransforms[7].m[8] = -0.0f; break;
    case 4: b.clipPlanes[5][3] = -0.0f; break;
    }
    const auto same = compareUniformNonconstantValues(fixed, b);
    for (unsigned i = 0; i < same.size(); ++i)
      check(same[i] == (i != group), "only changed group differs; signed zero matters");
  }
  a.material.power = std::bit_cast<float>(0x7fc00001u);
  auto b = a;
  check(sameFixed(makeDrawUniformFixedPayload(a), b), "same NaN bits compare equal");
  b.material.power = std::bit_cast<float>(0x7fc00002u);
  check(!sameFixed(makeDrawUniformFixedPayload(a), b), "different NaN bits differ");
  Light light{};
  auto other = light;
  auto* bytes = reinterpret_cast<unsigned char*>(&other);
  for (auto i = offsetof(Light, enabled) + sizeof(light.enabled);
       i < offsetof(Light, diffuse); ++i) bytes[i] ^= 0xff;
  check(uniformLightEqual(light, other), "padding is not a semantic light value");
  other.enabled = !light.enabled;
  check(!uniformLightEqual(light, other), "light enable remains semantic");
}

void keyDependencyTable() {
  unsigned dependent = 0;
  for (u32 key = 0; key < kMaxStateSlots; ++key)
    dependent += renderStateAffectsNonconstantUniform(key);
  check(dependent == 2 && renderStateAffectsNonconstantUniform(RS_CLIPPING) &&
        renderStateAffectsNonconstantUniform(RS_CLIP_PLANE_ENABLE),
        "exactly two supported render-state dependencies");
  check(!renderStateAffectsNonconstantUniform(kMaxStateSlots) &&
        !renderStateAffectsNonconstantUniform(0xffffffffu),
        "out-of-table known setters cannot change the fixed payload");

  // Fresh construction is independent of cached invalidation. Cover FFP and
  // programmable clip spaces, invertible and singular matrix fallback.
  for (bool programmable : {false, true}) for (bool singular : {false, true}) {
    DeviceState initial{};
    Matrix4x4 transform{};
    transform.m = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 5, 6, 7, 1};
    initial.transforms[XFORM_WORLD_BASE] = transform;
    initial.transforms[XFORM_VIEW] = transform;
    if (singular) transform.m[0] = 0;
    initial.transforms[XFORM_PROJECTION] = transform;
    initial.transforms[XFORM_TEXTURE_BASE] = transform;
    initial.material.power = 3;
    initial.lights[0].position = {3, 5, 7};
    initial.lights[0].enabled = true;
    initial.clipPlanes[0] = {2, 3, 5, 7};
    initial.renderStates[RS_CLIPPING] = 1;
    initial.renderStates[RS_CLIP_PLANE_ENABLE] = 1;
    if (programmable) initial.vertexShader.kind = ShaderRef::Kind::Bytecode;
    const auto baseline = makeDrawUniformFixedPayload(
        makeDrawUniformPayload(makeDrawDescFromState(initial, {})));
    for (u32 key = 0; key < kMaxStateSlots; ++key) {
      bool witnessedChange = false;
      for (u32 value : {0u, 1u, 0xffu, 0x80000000u}) {
        auto changed = initial;
        changed.renderStates[key] = value;
        const auto fresh = makeDrawUniformPayload(makeDrawDescFromState(changed, {}));
        const bool same = sameFixed(baseline, fresh);
        witnessedChange |= !same;
        check(renderStateAffectsNonconstantUniform(key) || same,
              "nondependent key cannot change freshly constructed fixed values");
      }
      check(witnessedChange == renderStateAffectsNonconstantUniform(key),
            "both classified dependencies have a distinguishing fresh value");
    }
  }
}

struct Model {
  bool clipping = true, mask = true, matrix = false, programmable = false;
  std::array<bool, 2> dirty{true, true};
  std::array<unsigned, 2> cached{};

  unsigned serialValue() const {
    return unsigned(matrix) | (clipping && mask ? (programmable ? 4u : 2u) : 0u);
  }
};

// 8 actions, two independently refreshed lanes, all traces of length <= 6.
// Broad mutation/reset is separate from the known-key predicate. A shader
// change conservatively dirties the abstraction; concrete coordinate-space
// and generation/usage witnesses are tested by the production-cache fixture.
bool step(Model& m, unsigned action, u32 omittedKey) {
  bool invalidate = false;
  switch (action) {
  case 0: invalidate = renderStateAffectsNonconstantUniform(RS_Z_ENABLE); break;
  case 1:
    m.clipping = !m.clipping;
    invalidate = RS_CLIPPING != omittedKey && renderStateAffectsNonconstantUniform(RS_CLIPPING);
    break;
  case 2:
    m.mask = !m.mask;
    invalidate = RS_CLIP_PLANE_ENABLE != omittedKey && renderStateAffectsNonconstantUniform(RS_CLIP_PLANE_ENABLE);
    break;
  case 3: m.matrix = !m.matrix; invalidate = true; break;
  case 4:
    m.clipping = m.mask = true; m.matrix = m.programmable = false;
    invalidate = true; break;
  case 5: m.programmable = !m.programmable; invalidate = true; break;
  default: {
    const auto lane = action - 6;
    if (m.dirty[lane]) { m.cached[lane] = m.serialValue(); m.dirty[lane] = false; }
    return m.cached[lane] == m.serialValue();
  }
  }
  if (invalidate) m.dirty.fill(true);
  return true;
}

bool explore(Model m, unsigned depth, u32 omittedKey, std::uint64_t& edges,
             unsigned trace = 0) {
  if (!depth) return true;
  for (unsigned action = 0; action < 8; ++action) {
    auto next = m;
    ++edges;
    if (!step(next, action, omittedKey)) {
      std::printf("expected stale-value counterexample: omitted=%u trace(octal)=%o\n",
                  omittedKey, trace * 8 + action);
      return false;
    }
    if (!explore(next, depth - 1, omittedKey, edges, trace * 8 + action)) return false;
  }
  return true;
}

} // namespace

int main() {
  observerValues();
  keyDependencyTable();
  std::uint64_t edges = 0;
  check(explore({}, 6, 0xffffffffu, edges), "all bounded traces refine fresh values");
  check(edges == 299592, "all action prefixes are explored");
  std::printf("bounded refinement: %llu edges, 262144 length-six traces\n",
              static_cast<unsigned long long>(edges));
  for (auto omitted : {RS_CLIPPING, RS_CLIP_PLANE_ENABLE}) {
    edges = 0;
    check(!explore({}, 3, omitted, edges), "omitting either dependency fails in three steps");
  }
}
