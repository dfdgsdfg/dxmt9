// R-BACK-12.8 .. R-BACK-12.12 — backend draw-uniforms DirtyMask asserts.
//
// These tests pin the contract of the C1-owned dirty tracker
// (`dxmt9::uniform::DirtyState` + `apply*` helpers in
// `src/dxmt9/dxmt9_uniform_dirty.hpp`) so that:
//   - the bitmask granularity matches §3.1 of the requirements
//   - each apply* helper ORs in only the matching bit
//   - constant-set helpers update the per-stage high-water counter
//     monotonically (max-of-prev-and-new) without leaking across stages
//   - clearBit removes only the targeted bit
//   - markAllDirty asserts the full 12-bit mask
//
// The dirty tracker is a pure value transform; no Metal / Wine / queue
// state is required, so this is a free-standing native unit test.
#include "../../../src/dxmt9/dxmt9_uniform_dirty.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

using dxmt9::uniform::DirtyBit;
using dxmt9::uniform::DirtyState;
using dxmt9::uniform::ShaderConstantUsageBounds;

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

template <typename T, typename U>
void checkEq(const T& actual, const U& expected, std::string_view message) {
  if (!(actual == expected)) {
    std::cerr << "FAIL: " << message
              << " actual=" << static_cast<std::uint64_t>(actual)
              << " expected=" << static_cast<std::uint64_t>(expected) << '\n';
    std::exit(1);
  }
}

constexpr std::uint16_t bitValue(DirtyBit bit) {
  return static_cast<std::uint16_t>(bit);
}

// R-BACK-12.8 — bitmask granularity. Each category must have a distinct
// power-of-two value matching design.md §4.
void testBitmaskGranularity() {
  checkEq(bitValue(DirtyBit::VsF), 1u << 0, "VsF == 1<<0");
  checkEq(bitValue(DirtyBit::VsI), 1u << 1, "VsI == 1<<1");
  checkEq(bitValue(DirtyBit::VsB), 1u << 2, "VsB == 1<<2");
  checkEq(bitValue(DirtyBit::PsF), 1u << 3, "PsF == 1<<3");
  checkEq(bitValue(DirtyBit::PsI), 1u << 4, "PsI == 1<<4");
  checkEq(bitValue(DirtyBit::PsB), 1u << 5, "PsB == 1<<5");
  checkEq(bitValue(DirtyBit::FfpVsTransforms), 1u << 6, "FfpVsTransforms == 1<<6");
  checkEq(bitValue(DirtyBit::FfpVsClip), 1u << 7, "FfpVsClip == 1<<7");
  checkEq(bitValue(DirtyBit::FfpVsViewport), 1u << 8, "FfpVsViewport == 1<<8");
  checkEq(bitValue(DirtyBit::FfpPsFog), 1u << 9, "FfpPsFog == 1<<9");
  checkEq(bitValue(DirtyBit::FfpPsAlpha), 1u << 10, "FfpPsAlpha == 1<<10");
  checkEq(bitValue(DirtyBit::FfpPsTexFactor), 1u << 11, "FfpPsTexFactor == 1<<11");

  // All twelve bits should be distinct and pack into the low 12 bits of u16.
  const std::uint16_t allBits = bitValue(DirtyBit::VsF) | bitValue(DirtyBit::VsI) |
                                bitValue(DirtyBit::VsB) | bitValue(DirtyBit::PsF) |
                                bitValue(DirtyBit::PsI) | bitValue(DirtyBit::PsB) |
                                bitValue(DirtyBit::FfpVsTransforms) |
                                bitValue(DirtyBit::FfpVsClip) |
                                bitValue(DirtyBit::FfpVsViewport) |
                                bitValue(DirtyBit::FfpPsFog) |
                                bitValue(DirtyBit::FfpPsAlpha) |
                                bitValue(DirtyBit::FfpPsTexFactor);
  checkEq(allBits, static_cast<std::uint16_t>(0x0FFFu), "all 12 bits pack as 0x0FFF");
}

// R-BACK-12.9 — apply* helpers OR the matching bit and only the matching
// bit. Each helper is exercised on a fresh zero-initialized state.
void testApplyHelpersSetMatchingBit() {
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetVsF(s, 0, 4);
    checkEq(s.mask, bitValue(DirtyBit::VsF), "applyConstantSetVsF sets only VsF");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetVsI(s, 0, 4);
    checkEq(s.mask, bitValue(DirtyBit::VsI), "applyConstantSetVsI sets only VsI");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetVsB(s, 0, 4);
    checkEq(s.mask, bitValue(DirtyBit::VsB), "applyConstantSetVsB sets only VsB");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetPsF(s, 0, 4);
    checkEq(s.mask, bitValue(DirtyBit::PsF), "applyConstantSetPsF sets only PsF");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetPsI(s, 0, 4);
    checkEq(s.mask, bitValue(DirtyBit::PsI), "applyConstantSetPsI sets only PsI");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetPsB(s, 0, 4);
    checkEq(s.mask, bitValue(DirtyBit::PsB), "applyConstantSetPsB sets only PsB");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyTransformChange(s);
    checkEq(s.mask, bitValue(DirtyBit::FfpVsTransforms),
            "applyTransformChange sets only FfpVsTransforms");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyClipPlaneChange(s);
    checkEq(s.mask, bitValue(DirtyBit::FfpVsClip),
            "applyClipPlaneChange sets only FfpVsClip");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyViewportChange(s);
    checkEq(s.mask, bitValue(DirtyBit::FfpVsViewport),
            "applyViewportChange sets only FfpVsViewport");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyRenderStateFog(s);
    checkEq(s.mask, bitValue(DirtyBit::FfpPsFog),
            "applyRenderStateFog sets only FfpPsFog");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyRenderStateAlpha(s);
    checkEq(s.mask, bitValue(DirtyBit::FfpPsAlpha),
            "applyRenderStateAlpha sets only FfpPsAlpha");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyRenderStateTexFactor(s);
    checkEq(s.mask, bitValue(DirtyBit::FfpPsTexFactor),
            "applyRenderStateTexFactor sets only FfpPsTexFactor");
  }
}

// R-BACK-12.10 — range counters. Each constant-set helper bumps the
// matching maxChanged* counter to `max(prev, start + count)` and never
// regresses. Counters from one stage do not leak into another.
void testRangeCounters() {
  // VsF: grow from 0 -> 4 -> 12; a smaller (8, 1) write must not regress.
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetVsF(s, 0, 4);
    checkEq(s.maxChangedVsF, 4u, "VsF maxChanged after (0,4)");
    dxmt9::uniform::applyConstantSetVsF(s, 10, 2);
    checkEq(s.maxChangedVsF, 12u, "VsF maxChanged after (10,2) is max(prev, 12)");
    dxmt9::uniform::applyConstantSetVsF(s, 8, 1);
    checkEq(s.maxChangedVsF, 12u, "VsF maxChanged after (8,1) does not regress");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetVsI(s, 0, 2);
    checkEq(s.maxChangedVsI, 2u, "VsI maxChanged after (0,2)");
    dxmt9::uniform::applyConstantSetVsI(s, 5, 3);
    checkEq(s.maxChangedVsI, 8u, "VsI maxChanged after (5,3) is max(prev, 8)");
    dxmt9::uniform::applyConstantSetVsI(s, 1, 1);
    checkEq(s.maxChangedVsI, 8u, "VsI maxChanged after (1,1) does not regress");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetVsB(s, 0, 1);
    checkEq(s.maxChangedVsB, 1u, "VsB maxChanged after (0,1)");
    dxmt9::uniform::applyConstantSetVsB(s, 8, 4);
    checkEq(s.maxChangedVsB, 12u, "VsB maxChanged after (8,4) is max(prev, 12)");
    dxmt9::uniform::applyConstantSetVsB(s, 3, 2);
    checkEq(s.maxChangedVsB, 12u, "VsB maxChanged after (3,2) does not regress");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetPsF(s, 0, 4);
    checkEq(s.maxChangedPsF, 4u, "PsF maxChanged after (0,4)");
    dxmt9::uniform::applyConstantSetPsF(s, 16, 8);
    checkEq(s.maxChangedPsF, 24u, "PsF maxChanged after (16,8) is max(prev, 24)");
    dxmt9::uniform::applyConstantSetPsF(s, 12, 4);
    checkEq(s.maxChangedPsF, 24u, "PsF maxChanged after (12,4) does not regress");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetPsI(s, 0, 2);
    checkEq(s.maxChangedPsI, 2u, "PsI maxChanged after (0,2)");
    dxmt9::uniform::applyConstantSetPsI(s, 6, 4);
    checkEq(s.maxChangedPsI, 10u, "PsI maxChanged after (6,4) is max(prev, 10)");
    dxmt9::uniform::applyConstantSetPsI(s, 4, 1);
    checkEq(s.maxChangedPsI, 10u, "PsI maxChanged after (4,1) does not regress");
  }
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetPsB(s, 0, 1);
    checkEq(s.maxChangedPsB, 1u, "PsB maxChanged after (0,1)");
    dxmt9::uniform::applyConstantSetPsB(s, 9, 7);
    checkEq(s.maxChangedPsB, 16u, "PsB maxChanged after (9,7) is max(prev, 16)");
    dxmt9::uniform::applyConstantSetPsB(s, 5, 2);
    checkEq(s.maxChangedPsB, 16u, "PsB maxChanged after (5,2) does not regress");
  }

  // Cross-stage isolation — a VsF write must never bump PsF (or any
  // other category) counters.
  {
    DirtyState s{};
    dxmt9::uniform::applyConstantSetVsF(s, 0, 100);
    checkEq(s.maxChangedVsF, 100u, "VsF counter advanced");
    checkEq(s.maxChangedVsI, 0u, "VsF write does not leak into VsI counter");
    checkEq(s.maxChangedVsB, 0u, "VsF write does not leak into VsB counter");
    checkEq(s.maxChangedPsF, 0u, "VsF write does not leak into PsF counter");
    checkEq(s.maxChangedPsI, 0u, "VsF write does not leak into PsI counter");
    checkEq(s.maxChangedPsB, 0u, "VsF write does not leak into PsB counter");
  }
}

// R-BACK-12.11 — clearBit removes only the targeted bit; siblings remain
// dirty until they too are cleared (or markAllDirty is called).
void testClearBit() {
  DirtyState s{};
  dxmt9::uniform::applyConstantSetVsF(s, 0, 4);
  dxmt9::uniform::applyConstantSetPsF(s, 0, 8);
  dxmt9::uniform::applyTransformChange(s);

  const std::uint16_t before = bitValue(DirtyBit::VsF) | bitValue(DirtyBit::PsF) |
                               bitValue(DirtyBit::FfpVsTransforms);
  checkEq(s.mask, before, "before clearBit, VsF | PsF | FfpVsTransforms set");

  dxmt9::uniform::clearBit(s, DirtyBit::VsF);
  check((s.mask & bitValue(DirtyBit::VsF)) == 0, "VsF cleared");
  check((s.mask & bitValue(DirtyBit::PsF)) != 0, "PsF still dirty after clearing VsF");
  check((s.mask & bitValue(DirtyBit::FfpVsTransforms)) != 0,
        "FfpVsTransforms still dirty after clearing VsF");

  // Range counters are not zeroed by clearBit — they remain valid as
  // upper bounds on slots that were ever written.
  checkEq(s.maxChangedVsF, 4u, "clearBit does not reset range counter");
}

// R-BACK-12.12 — markAllDirty asserts every category bit at once. Used by
// C2 at encoder init / pass start.
void testMarkAllDirty() {
  DirtyState s{};
  dxmt9::uniform::markAllDirty(s);
  checkEq(s.mask, static_cast<std::uint16_t>(0x0FFFu),
          "markAllDirty sets all 12 category bits");

  // Every named bit must be present.
  check(dxmt9::uniform::isDirty(s, DirtyBit::VsF), "markAllDirty: VsF dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::VsI), "markAllDirty: VsI dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::VsB), "markAllDirty: VsB dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::PsF), "markAllDirty: PsF dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::PsI), "markAllDirty: PsI dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::PsB), "markAllDirty: PsB dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::FfpVsTransforms),
        "markAllDirty: FfpVsTransforms dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::FfpVsClip),
        "markAllDirty: FfpVsClip dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::FfpVsViewport),
        "markAllDirty: FfpVsViewport dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::FfpPsFog),
        "markAllDirty: FfpPsFog dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::FfpPsAlpha),
        "markAllDirty: FfpPsAlpha dirty");
  check(dxmt9::uniform::isDirty(s, DirtyBit::FfpPsTexFactor),
        "markAllDirty: FfpPsTexFactor dirty");
}

// Bonus — `isDirty` consistency. setBit toggles the predicate true,
// clearBit toggles it back to false, and unrelated bits are unaffected.
void testIsDirtyConsistency() {
  DirtyState s{};
  check(!dxmt9::uniform::isDirty(s, DirtyBit::VsF), "fresh state: VsF not dirty");
  check(!dxmt9::uniform::isDirty(s, DirtyBit::PsF), "fresh state: PsF not dirty");

  dxmt9::uniform::setBit(s, DirtyBit::VsF);
  check(dxmt9::uniform::isDirty(s, DirtyBit::VsF), "after setBit: VsF dirty");
  check(!dxmt9::uniform::isDirty(s, DirtyBit::PsF),
        "after setBit(VsF): PsF still not dirty");

  dxmt9::uniform::clearBit(s, DirtyBit::VsF);
  check(!dxmt9::uniform::isDirty(s, DirtyBit::VsF), "after clearBit: VsF not dirty");
}

void testShaderUsageAwareUploadPlanIsConservative() {
  DirtyState s{};
  dxmt9::uniform::applyConstantSetVsF(s, 6, 2);
  dxmt9::uniform::applyConstantSetVsI(s, 1, 1);
  dxmt9::uniform::applyConstantSetVsB(s, 0, 1);

  ShaderConstantUsageBounds usage{};
  usage.unknown = false;
  usage.floatCount = 4;
  usage.intCount = 3;
  usage.boolCount = 0;

  const auto plan = dxmt9::uniform::makeVsConstantUploadPlan(s, usage);
  check(!plan.fullStructRequired,
        "known non-indexed VS constants can use a range upload plan");
  checkEq(plan.floatCount, 8u,
          "VS upload float count covers max(dirty high-water, shader usage)");
  checkEq(plan.intCount, 3u,
          "VS upload int count covers shader usage when it exceeds dirty range");
  checkEq(plan.boolCount, 1u,
          "VS upload bool count covers dirty range when usage is lower");
  checkEq(dxmt9::uniform::vsConstantUploadBytes(plan), 4356u,
          "VS bool usage requires a prefix reaching the last used bool slot");

  DirtyState floatOnlyDirty{};
  dxmt9::uniform::applyConstantSetVsF(floatOnlyDirty, 2, 2);
  ShaderConstantUsageBounds floatOnlyUsage{};
  floatOnlyUsage.unknown = false;
  floatOnlyUsage.floatCount = 6;
  const auto floatOnlyPlan =
      dxmt9::uniform::makeVsConstantUploadPlan(floatOnlyDirty, floatOnlyUsage);
  checkEq(dxmt9::uniform::vsConstantUploadBytes(floatOnlyPlan), 96u,
          "VS float-only fixed usage uploads only the float4 prefix");
}

void testShaderUsageAwareUploadPlanFallsBackForUnknownOrIndexedUse() {
  DirtyState s{};
  dxmt9::uniform::applyConstantSetPsF(s, 1, 2);
  dxmt9::uniform::applyConstantSetPsI(s, 1, 2);
  dxmt9::uniform::applyConstantSetPsB(s, 1, 2);

  ShaderConstantUsageBounds unknownUsage{};
  const auto unknownPlan = dxmt9::uniform::makePsConstantUploadPlan(s, unknownUsage);
  check(unknownPlan.fullStructRequired,
        "unknown PS constant usage requires the current full struct ABI");
  checkEq(unknownPlan.floatCount, dxmt9::core::kMaxPixelConstants,
          "unknown PS float usage expands to full pixel constant capacity");
  checkEq(unknownPlan.intCount, dxmt9::core::kMaxIntegerConstants,
          "unknown PS int usage expands to full integer constant capacity");
  checkEq(unknownPlan.boolCount, dxmt9::core::kMaxBoolConstants,
          "unknown PS bool usage expands to full bool constant capacity");

  ShaderConstantUsageBounds indexedUsage{};
  indexedUsage.unknown = false;
  indexedUsage.floatCount = 4;
  indexedUsage.indexedFloat = true;

  const auto indexedPlan = dxmt9::uniform::makePsConstantUploadPlan(s, indexedUsage);
  check(indexedPlan.fullStructRequired,
        "relative/indexed constant reads require full struct backing");
  checkEq(indexedPlan.floatCount, dxmt9::core::kMaxPixelConstants,
          "indexed PS float usage expands to full pixel constant capacity");
  checkEq(dxmt9::uniform::psConstantUploadBytes(indexedPlan), 3904u,
          "indexed PS usage uploads the full PsConsts struct");
}

}  // namespace

int main() {
  testBitmaskGranularity();
  testApplyHelpersSetMatchingBit();
  testRangeCounters();
  testClearBit();
  testMarkAllDirty();
  testIsDirtyConsistency();
  testShaderUsageAwareUploadPlanIsConservative();
  testShaderUsageAwareUploadPlanFallsBackForUnknownOrIndexedUse();
  return 0;
}
