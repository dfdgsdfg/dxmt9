// R-BACK-12.22..12.26 — Stage 2 argument-buffer hybrid runtime adopter.
//
// CPU-only spec covering the framework pieces of the Stage 2 hybrid:
//   - capability gate decision shape (R-BACK-12.22).
//   - per-pass selector decision shape (Stage1 vs Stage2).
//   - ShaderVariantKey carries the argbufHybridMode bit so Stage 1 and
//     Stage 2 PSOs hash to distinct cache entries (R-BACK-12.23).
//   - WMTArgumentDescriptor build covers the typed argbuf layout
//     (4 const buffers + 3 texture arrays + 1 sampler array) at the
//     [[id(N)]] positions the MSL ArgbufLayout pins.
//   - MSL prelude variant text contains the ArgbufLayout struct.
//
// Hardware shader-runner equality (R-BACK-12.26) is a follow-up — this
// spec runs on the native build with no Metal device.

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_argbuf_hybrid.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_shader_sources.hpp"
#include "../../../src/winemetal/winemetal.h"

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;

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
    std::ostringstream out;
    out << message << " (left != right)";
    fail(out.str());
  }
}

// ---------------------------------------------------------------------
// A1 — capability gate (R-BACK-12.22)

void testCapabilityGateRejectsTier1() {
  check(!dxmt9::argbuf_hybrid::computeCapabilityGate(WMTArgumentBuffersTier1, true),
        "Tier1 rejected even with Apple3");
  check(!dxmt9::argbuf_hybrid::computeCapabilityGate(WMTArgumentBuffersTier1, false),
        "Tier1 + non-Apple3 rejected");
}

void testCapabilityGateRejectsNonApple3() {
  check(!dxmt9::argbuf_hybrid::computeCapabilityGate(WMTArgumentBuffersTier2, false),
        "Tier2 alone is not enough — needs Apple3");
}

void testCapabilityGateAcceptsTier2Apple3() {
  check(dxmt9::argbuf_hybrid::computeCapabilityGate(WMTArgumentBuffersTier2, true),
        "Tier2 + Apple3 enables Stage 2");
}

// ---------------------------------------------------------------------
// A2 — per-pass selector (R-BACK-12.22 sentence 2: gate fail -> Stage 1)

void testSelectorPicksStage1WhenDisabled() {
  FlatDrawStateRecord hot{};
  DrawShaderLayoutContext shaderLayout{};
  FlatDrawStateView view{.hot = &hot, .shaderLayout = &shaderLayout};
  const auto decision =
      dxmt9::pipeline::selectArgbufHybridForPass(view, /*argbufHybridEnabled=*/false);
  checkEq(static_cast<int>(decision),
          static_cast<int>(dxmt9::pipeline::ArgbufHybridDecision::Stage1),
          "capability gate fail forces Stage 1");
}

void testSelectorPicksStage2WhenEnabled() {
  FlatDrawStateRecord hot{};
  DrawShaderLayoutContext shaderLayout{};
  FlatDrawStateView view{.hot = &hot, .shaderLayout = &shaderLayout};
  const auto decision =
      dxmt9::pipeline::selectArgbufHybridForPass(view, /*argbufHybridEnabled=*/true);
  checkEq(static_cast<int>(decision),
          static_cast<int>(dxmt9::pipeline::ArgbufHybridDecision::Stage2),
          "capability gate hold picks Stage 2");
}

void testSelectorFallsBackForTextureBoundDraws() {
  FlatDrawStateRecord hot{};
  hot.textures[0] = Handle{1};
  DrawShaderLayoutContext shaderLayout{};
  FlatDrawStateView view{.hot = &hot, .shaderLayout = &shaderLayout};
  const auto decision =
      dxmt9::pipeline::selectArgbufHybridForPass(view, /*argbufHybridEnabled=*/true);
  checkEq(static_cast<int>(decision),
          static_cast<int>(dxmt9::pipeline::ArgbufHybridDecision::Stage1),
          "texture-bound draws stay on Stage 1 until Stage 2 texture equality is stable");
}

// ---------------------------------------------------------------------
// A3 — variant key bit (R-BACK-12.23)

void testVariantKeyArgbufHybridBitDistinguishesStages() {
  dxmt9::pipeline::ShaderVariantKey k1{};
  k1.hash = 0xdeadbeefull;
  k1.argbufHybridMode = false;
  dxmt9::pipeline::ShaderVariantKey k2 = k1;
  k2.argbufHybridMode = true;

  // Equality operator uses defaulted member-wise compare; bit difference
  // must produce inequality so the cache stores distinct entries.
  check(!(k1 == k2), "argbufHybridMode bit makes keys unequal");

  // Hashes must also differ so the unordered_map probe lands in distinct
  // buckets in normal cases.
  dxmt9::pipeline::ShaderVariantKeyHash hasher{};
  check(hasher(k1) != hasher(k2),
        "argbufHybridMode bit changes ShaderVariantKey hash");
}

void testTileFfpAndArgbufHybridBitsAreIndependent() {
  // The four state combinations (tile off/on × argbuf off/on) must all
  // be distinguishable so the cache never aliases an argbuf-mode tile
  // PSO with a non-argbuf fragment-stage PSO.
  std::array<dxmt9::pipeline::ShaderVariantKey, 4> keys{};
  for (int i = 0; i < 4; ++i) {
    keys[i].hash = 0x1234ull;
    keys[i].tileFfpMode = (i & 1) != 0;
    keys[i].argbufHybridMode = (i & 2) != 0;
  }
  dxmt9::pipeline::ShaderVariantKeyHash hasher{};
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      check(!(keys[i] == keys[j]),
            "all (tileFfp, argbufHybrid) bit combos must be distinguishable");
      check(hasher(keys[i]) != hasher(keys[j]),
            "all (tileFfp, argbufHybrid) bit combos hash distinctly");
    }
  }
}

// ---------------------------------------------------------------------
// A4 — argument descriptor layout (R-BACK-12.23)

void testArgumentDescriptorCount() {
  const auto descriptors = dxmt9::argbuf_hybrid::buildArgumentDescriptors();
  checkEq(static_cast<std::uint64_t>(descriptors.count()),
          static_cast<std::uint64_t>(dxmt9::shaders::kArgbufHybridDescriptorCount),
          "descriptor table holds 4 cbuf + 3 texture arrays + 1 sampler array");
  checkEq(static_cast<std::uint64_t>(descriptors.count()),
          static_cast<std::uint64_t>(8),
          "descriptor count is 8");
}

void testArgumentDescriptorRoles() {
  const auto descriptors = dxmt9::argbuf_hybrid::buildArgumentDescriptors();
  // First 4 entries: constant buffers at id 0..3.
  for (std::uint32_t i = 0; i < 4; ++i) {
    const auto& d = descriptors.entries[i];
    checkEq(static_cast<std::uint32_t>(d.argumentType),
            static_cast<std::uint32_t>(WMTArgumentTypeBuffer),
            "first 4 descriptors are buffers");
    checkEq(static_cast<std::uint32_t>(d.index), i,
            "buffer descriptor indices are 0..3");
    check(d.constantBlockAlignment >= 16u,
          "buffer descriptors carry >=16 B alignment");
  }
  // Next 3 entries: 2D, cube, then 3D texture arrays.
  const std::uint32_t textureBases[] = {
      dxmt9::shaders::kArgbufHybridTexture2DBase,
      dxmt9::shaders::kArgbufHybridTextureCubeBase,
      dxmt9::shaders::kArgbufHybridTexture3DBase,
  };
  for (std::uint32_t i = 0; i < 3; ++i) {
    const auto& d = descriptors.entries[4u + i];
    checkEq(static_cast<std::uint32_t>(d.argumentType),
            static_cast<std::uint32_t>(WMTArgumentTypeTexture),
            "entries 4..6 are texture arrays");
    checkEq(static_cast<std::uint32_t>(d.index), textureBases[i],
            "texture array descriptor index is the MSL base id");
    checkEq(static_cast<std::uint32_t>(d.arrayLength),
            dxmt9::shaders::kArgbufHybridTextureSlotCount,
            "texture array descriptors have 8 elements");
  }
  // Final entry: sampler array at id 28 with 8 elements.
  const auto& sampler = descriptors.entries[7u];
  checkEq(static_cast<std::uint32_t>(sampler.argumentType),
          static_cast<std::uint32_t>(WMTArgumentTypeSampler),
          "entry 7 is the sampler array");
  checkEq(static_cast<std::uint32_t>(sampler.index),
          dxmt9::shaders::kArgbufHybridSamplerBase,
          "sampler array descriptor index is the MSL base id");
  checkEq(static_cast<std::uint32_t>(sampler.arrayLength),
          dxmt9::shaders::kArgbufHybridSamplerSlotCount,
          "sampler array descriptor has 8 elements");
}

// ---------------------------------------------------------------------
// A5 — MSL prelude argbuf variant (R-BACK-12.23)

void testArgbufHybridPreludeContainsArgbufLayout() {
  const auto prelude = dxmt9::shaders::makeShaderPreludeArgbufHybrid(/*withClipDistances=*/false);
  check(prelude.find("struct ArgbufLayout") != std::string::npos,
        "argbuf-hybrid prelude declares struct ArgbufLayout");
  check(prelude.find("[[id(0)]]") != std::string::npos,
        "argbuf-hybrid prelude pins descriptor id 0");
  check(prelude.find("[[id(4)]]") != std::string::npos,
        "argbuf-hybrid prelude pins texture base id 4");
  check(prelude.find("[[id(28)]]") != std::string::npos,
        "argbuf-hybrid prelude pins sampler base id 28");
  check(prelude.find("array<texture2d<float>, 8> textures2d") != std::string::npos,
        "argbuf-hybrid prelude declares 8 2D textures");
  check(prelude.find("array<texturecube<float>, 8> texturesCube") != std::string::npos,
        "argbuf-hybrid prelude declares 8 cube textures");
  check(prelude.find("array<texture3d<float>, 8> textures3d") != std::string::npos,
        "argbuf-hybrid prelude declares 8 3D textures");
  check(prelude.find("array<sampler, 8> samplers") != std::string::npos,
        "argbuf-hybrid prelude declares 8 samplers");
}

void testArgbufHybridPreludeIncludesStage1Structs() {
  // R-BACK-12.4 — single source of truth for each field. Stage 2 reuses
  // VsConsts/PsConsts/FfpVsConsts/FfpPsConsts/DrawVolatile from Stage 1
  // verbatim; the prelude variant must emit them so helper inlines that
  // take `constant FfpVsConsts&` (e.g., dxmt9_apply_texture_transform)
  // still compile.
  const auto prelude = dxmt9::shaders::makeShaderPreludeArgbufHybrid(/*withClipDistances=*/false);
  check(prelude.find("struct VsConsts") != std::string::npos,
        "argbuf-hybrid prelude includes VsConsts");
  check(prelude.find("struct PsConsts") != std::string::npos,
        "argbuf-hybrid prelude includes PsConsts");
  check(prelude.find("struct FfpVsConsts") != std::string::npos,
        "argbuf-hybrid prelude includes FfpVsConsts");
  check(prelude.find("struct FfpPsConsts") != std::string::npos,
        "argbuf-hybrid prelude includes FfpPsConsts");
  check(prelude.find("struct DrawVolatile") != std::string::npos,
        "argbuf-hybrid prelude includes DrawVolatile");
}

void testArgbufHybridPreludeWithClipDistances() {
  const auto prelude = dxmt9::shaders::makeShaderPreludeArgbufHybrid(/*withClipDistances=*/true);
  check(prelude.find("clipDistance") != std::string::npos,
        "argbuf-hybrid prelude with clip distances declares clipDistance[6]");
}

// ---------------------------------------------------------------------
// A6 — bind-slot constant matches DXMT mirror (R-BACK-12.23)

void testArgbufBindSlotConstant() {
  // Slot 30 mirrors DXMT's argbuf bind slot. A drift here would alias
  // with a future DrawVolatile/stream slot extension, so pin it.
  checkEq(dxmt9::shaders::kArgbufHybridBindSlot, 30u,
          "argbuf bind slot is 30 (DXMT mirror)");
}

}  // namespace

int main() {
  try {
    testCapabilityGateRejectsTier1();
    testCapabilityGateRejectsNonApple3();
    testCapabilityGateAcceptsTier2Apple3();
    testSelectorPicksStage1WhenDisabled();
    testSelectorPicksStage2WhenEnabled();
    testSelectorFallsBackForTextureBoundDraws();
    testVariantKeyArgbufHybridBitDistinguishesStages();
    testTileFfpAndArgbufHybridBitsAreIndependent();
    testArgumentDescriptorCount();
    testArgumentDescriptorRoles();
    testArgbufHybridPreludeContainsArgbufLayout();
    testArgbufHybridPreludeIncludesStage1Structs();
    testArgbufHybridPreludeWithClipDistances();
    testArgbufBindSlotConstant();
  } catch (const TestFailure& failure) {
    std::cerr << "argbuf_hybrid_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "argbuf_hybrid_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }

  std::cout << "argbuf_hybrid_spec passed\n";
  return 0;
}
