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
//   - markAllDirty asserts the full 13-bit mask
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
using dxmt9::uniform::DirectCbufPayloadCounts;
using dxmt9::uniform::DirectCbufPayloadSourceHashes;
using dxmt9::uniform::DrawBindingAbi;
using dxmt9::uniform::DrawBindingPath;
using dxmt9::uniform::DrawBindingPayloadIdentity;
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
// power-of-two value matching spec.md §4.
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
  checkEq(bitValue(DirtyBit::FfpPsTssConstant), 1u << 12,
          "FfpPsTssConstant == 1<<12");

  // All thirteen bits should be distinct and pack into the low 13 bits of u16.
  const std::uint16_t allBits = bitValue(DirtyBit::VsF) | bitValue(DirtyBit::VsI) |
                                bitValue(DirtyBit::VsB) | bitValue(DirtyBit::PsF) |
                                bitValue(DirtyBit::PsI) | bitValue(DirtyBit::PsB) |
                                bitValue(DirtyBit::FfpVsTransforms) |
                                bitValue(DirtyBit::FfpVsClip) |
                                bitValue(DirtyBit::FfpVsViewport) |
                                bitValue(DirtyBit::FfpPsFog) |
                                bitValue(DirtyBit::FfpPsAlpha) |
                                bitValue(DirtyBit::FfpPsTexFactor) |
                                bitValue(DirtyBit::FfpPsTssConstant);
  checkEq(allBits, static_cast<std::uint16_t>(0x1FFFu), "all 13 bits pack as 0x1FFF");
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
  {
    DirtyState s{};
    dxmt9::uniform::applyTextureStageConstant(s);
    checkEq(s.mask, bitValue(DirtyBit::FfpPsTssConstant),
            "applyTextureStageConstant sets only FfpPsTssConstant");
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

  // Range counters describe pending dirty work. Once the matching bit is
  // consumed, the high-water counter must reset so old high-register writes
  // do not inflate later unrelated range uploads.
  checkEq(s.maxChangedVsF, 0u, "clearBit resets consumed VS range counter");
  checkEq(s.maxChangedPsF, 8u, "clearBit leaves unrelated PS range counter");
}

void testClearBitsResetsOnlyConsumedRanges() {
  DirtyState s{};
  dxmt9::uniform::applyConstantSetVsF(s, 2, 5);
  dxmt9::uniform::applyConstantSetVsI(s, 1, 3);
  dxmt9::uniform::applyConstantSetPsF(s, 4, 6);
  dxmt9::uniform::applyConstantSetPsB(s, 3, 2);

  dxmt9::uniform::clearBits(s, dxmt9::uniform::kVsAny);
  checkEq(s.maxChangedVsF, 0u, "clearBits(kVsAny) resets VsF range");
  checkEq(s.maxChangedVsI, 0u, "clearBits(kVsAny) resets VsI range");
  checkEq(s.maxChangedVsB, 0u, "clearBits(kVsAny) keeps absent VsB at zero");
  checkEq(s.maxChangedPsF, 10u, "clearBits(kVsAny) does not reset PsF range");
  checkEq(s.maxChangedPsB, 5u, "clearBits(kVsAny) does not reset PsB range");

  dxmt9::uniform::clearBits(s, dxmt9::uniform::kPsAny);
  checkEq(s.maxChangedPsF, 0u, "clearBits(kPsAny) resets PsF range");
  checkEq(s.maxChangedPsB, 0u, "clearBits(kPsAny) resets PsB range");
}

// R-BACK-12.12 — markAllDirty asserts every category bit at once. Used by
// C2 at encoder init / pass start.
void testMarkAllDirty() {
  DirtyState s{};
  dxmt9::uniform::markAllDirty(s);
  checkEq(s.mask, static_cast<std::uint16_t>(0x1FFFu),
          "markAllDirty sets all 13 category bits");

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
  check(dxmt9::uniform::isDirty(s, DirtyBit::FfpPsTssConstant),
        "markAllDirty: FfpPsTssConstant dirty");
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

// R-BACK-12.24 / R-BACK-12.26 — direct-cbuf must recreate the argbuf-table
// repoint edge when a DrawRun changes compact uniform payload sources.
void testDirectCbufPayloadSourceDirtyRebind() {
  const DirectCbufPayloadSourceHashes sourceA{
      .vertexConstants = 0x1111u,
      .pixelConstants = 0x2222u,
  };
  const DirectCbufPayloadSourceHashes sourceB{
      .vertexConstants = 0x3333u,
      .pixelConstants = 0x4444u,
  };
  const DirectCbufPayloadCounts countsA{
      .vertexFloat = 2,
      .vertexInt = 1,
      .pixelFloat = 3,
      .pixelBool = 4,
  };
  const DirectCbufPayloadCounts countsB{
      .vertexFloat = 5,
      .vertexBool = 6,
      .pixelInt = 7,
  };

  const auto initial = dxmt9::uniform::classifyDirectCbufPayloadSourceChange(
      false, {}, sourceA);
  check(!initial.vertex && !initial.pixel,
        "first payload relies on pass-initial dirty state");

  const auto unchanged =
      dxmt9::uniform::classifyDirectCbufPayloadSourceChange(
          true, sourceA, sourceA);
  DirtyState dirty{};
  dxmt9::uniform::applyDirectCbufPayloadSourceChange(
      dirty, unchanged, countsA);
  checkEq(dirty.mask, 0u, "unchanged payload source does not dirty cbufs");

  const auto aToB = dxmt9::uniform::classifyDirectCbufPayloadSourceChange(
      true, sourceA, sourceB);
  dxmt9::uniform::applyDirectCbufPayloadSourceChange(
      dirty, aToB, countsB);
  checkEq(dirty.mask,
          bitValue(DirtyBit::VsF) | bitValue(DirtyBit::VsB) |
              bitValue(DirtyBit::PsI),
          "A to B dirties only B's live VS/PS categories");
  checkEq(dirty.maxChangedVsF, 5u, "A to B uses B VS float range");
  checkEq(dirty.maxChangedVsB, 6u, "A to B uses B VS bool range");
  checkEq(dirty.maxChangedPsI, 7u, "A to B uses B PS int range");

  dxmt9::uniform::clearBits(
      dirty, dxmt9::uniform::kVsAny | dxmt9::uniform::kPsAny);
  const auto bToA = dxmt9::uniform::classifyDirectCbufPayloadSourceChange(
      true, sourceB, sourceA);
  dxmt9::uniform::applyDirectCbufPayloadSourceChange(
      dirty, bToA, countsA);
  checkEq(dirty.mask,
          bitValue(DirtyBit::VsF) | bitValue(DirtyBit::VsI) |
              bitValue(DirtyBit::PsF) | bitValue(DirtyBit::PsB),
          "B to A re-dirties A instead of treating its old binding as current");
  checkEq(dirty.maxChangedVsF, 2u, "B to A restores A VS float range");
  checkEq(dirty.maxChangedVsI, 1u, "B to A restores A VS int range");
  checkEq(dirty.maxChangedPsF, 3u, "B to A restores A PS float range");
  checkEq(dirty.maxChangedPsB, 4u, "B to A restores A PS bool range");

  dxmt9::uniform::clearBits(
      dirty, dxmt9::uniform::kVsAny | dxmt9::uniform::kPsAny);
  const auto aToEmpty =
      dxmt9::uniform::classifyDirectCbufPayloadSourceChange(
          true, sourceA, {});
  dxmt9::uniform::applyDirectCbufPayloadSourceChange(
      dirty, aToEmpty, {});
  checkEq(dirty.mask,
          bitValue(DirtyBit::VsF) | bitValue(DirtyBit::PsF),
          "empty payload source keeps sentinel dirty bits for minimum slabs");
  checkEq(dirty.maxChangedVsF, 0u,
          "empty VS payload does not invent a live register range");
  checkEq(dirty.maxChangedPsF, 0u,
          "empty PS payload does not invent a live register range");

  dxmt9::uniform::clearBits(
      dirty, dxmt9::uniform::kVsAny | dxmt9::uniform::kPsAny);
  const auto vertexOnly =
      dxmt9::uniform::classifyDirectCbufPayloadSourceChange(
          true,
          sourceA,
          DirectCbufPayloadSourceHashes{
              .vertexConstants = sourceB.vertexConstants,
              .pixelConstants = sourceA.pixelConstants,
          });
  dxmt9::uniform::applyDirectCbufPayloadSourceChange(
      dirty, vertexOnly, countsB);
  check(dxmt9::uniform::anyDirty(dirty, dxmt9::uniform::kVsAny),
        "vertex-only source change dirties VS");
  check(!dxmt9::uniform::anyDirty(dirty, dxmt9::uniform::kPsAny),
        "vertex-only source change leaves PS clean");
}

// R-VERIF-2.15 — truth table shared with ParallelDrawBinding.tla. Each child
// owns its previous identity and dirty state; A -> B -> A must rebind B and
// then A, while an independent sibling remains untouched.
void testDrawBindingTransitionTruthTable() {
  const DrawBindingPayloadIdentity a{
      .vertexConstants = 0x10u,
      .pixelConstants = 0x20u,
      .fixedFunction = 0x30u,
  };
  const DrawBindingPayloadIdentity b{
      .vertexConstants = 0x11u,
      .pixelConstants = 0x21u,
      .fixedFunction = 0x31u,
  };
  const DirectCbufPayloadCounts counts{
      .vertexFloat = 3,
      .vertexInt = 2,
      .vertexBool = 1,
      .pixelFloat = 4,
      .pixelInt = 2,
      .pixelBool = 1,
  };

  DirtyState child0{};
  DirtyState child1{};
  auto first = dxmt9::uniform::planDrawBindingTransition(
      false, {}, a, DrawBindingAbi::Stage2DirectCbuf,
      DrawBindingPath::Direct);
  check(first.psoBindingAbiCompatible,
        "Stage 2b PSO matches direct child binding");
  check(!first.constantSourceChange.vertex &&
            !first.constantSourceChange.pixel &&
            !first.fixedFunctionChanged,
        "first draw relies on the child-open all-dirty transition");

  const auto same = dxmt9::uniform::planDrawBindingTransition(
      true, a, a, DrawBindingAbi::Stage2DirectCbuf,
      DrawBindingPath::Direct);
  check(dxmt9::uniform::applyDrawBindingTransition(child0, same, counts),
        "Stage 2b direct-cbuf PSO is direct-binding ABI compatible");
  checkEq(child0.mask, 0u, "A to A does not create dirty work");

  const auto aToB = dxmt9::uniform::planDrawBindingTransition(
      true, a, b, DrawBindingAbi::Stage2DirectCbuf,
      DrawBindingPath::Direct);
  check(dxmt9::uniform::applyDrawBindingTransition(child0, aToB, counts),
        "A to B transition applies");
  check(dxmt9::uniform::anyDirty(child0, dxmt9::uniform::kVsAny),
        "A to B dirties VS constants");
  check(dxmt9::uniform::anyDirty(child0, dxmt9::uniform::kPsAny),
        "A to B dirties PS constants");
  check(dxmt9::uniform::anyDirty(child0, dxmt9::uniform::kFfpVsAny),
        "A to B dirties FFP VS payload");
  check(dxmt9::uniform::anyDirty(child0, dxmt9::uniform::kFfpPsAny),
        "A to B dirties FFP PS payload");
  checkEq(child1.mask, 0u,
          "child binding shadows stay isolated across sibling transition");

  dxmt9::uniform::clearBits(child0, 0x1fffu);
  const auto bToA = dxmt9::uniform::planDrawBindingTransition(
      true, b, a, DrawBindingAbi::Stage2DirectCbuf,
      DrawBindingPath::Direct);
  check(dxmt9::uniform::applyDrawBindingTransition(child0, bToA, counts),
        "Stage 2b B to A transition applies instead of reusing stale A history");
  check(dxmt9::uniform::anyDirty(child0, dxmt9::uniform::kVsAny) &&
            dxmt9::uniform::anyDirty(child0, dxmt9::uniform::kPsAny),
        "B to A re-dirties both programmable stages");

  DirtyState stageOnly{};
  auto vsOnly = b;
  vsOnly.pixelConstants = a.pixelConstants;
  vsOnly.fixedFunction = a.fixedFunction;
  const auto vsPlan = dxmt9::uniform::planDrawBindingTransition(
      true, a, vsOnly, DrawBindingAbi::Stage2DirectCbuf,
      DrawBindingPath::Direct);
  dxmt9::uniform::applyDrawBindingTransition(stageOnly, vsPlan, counts);
  check(dxmt9::uniform::anyDirty(stageOnly, dxmt9::uniform::kVsAny) &&
            !dxmt9::uniform::anyDirty(stageOnly, dxmt9::uniform::kPsAny),
        "VS-only identity change leaves PS clean");

  stageOnly = {};
  auto psOnly = a;
  psOnly.pixelConstants = b.pixelConstants;
  const auto psPlan = dxmt9::uniform::planDrawBindingTransition(
      true, a, psOnly, DrawBindingAbi::Stage2DirectCbuf,
      DrawBindingPath::Direct);
  dxmt9::uniform::applyDrawBindingTransition(stageOnly, psPlan, counts);
  check(!dxmt9::uniform::anyDirty(stageOnly, dxmt9::uniform::kVsAny) &&
            dxmt9::uniform::anyDirty(stageOnly, dxmt9::uniform::kPsAny),
        "PS-only identity change leaves VS clean");

  stageOnly = {};
  auto ffpOnly = a;
  ffpOnly.fixedFunction = b.fixedFunction;
  const auto ffpPlan = dxmt9::uniform::planDrawBindingTransition(
      true, a, ffpOnly, DrawBindingAbi::Stage2DirectCbuf,
      DrawBindingPath::Direct);
  dxmt9::uniform::applyDrawBindingTransition(stageOnly, ffpPlan, counts);
  check(!dxmt9::uniform::anyDirty(
            stageOnly, dxmt9::uniform::kVsAny | dxmt9::uniform::kPsAny),
        "FFP-only identity change leaves programmable constants clean");
  check(dxmt9::uniform::anyDirty(stageOnly, dxmt9::uniform::kFfpVsAny) &&
            dxmt9::uniform::anyDirty(stageOnly, dxmt9::uniform::kFfpPsAny),
        "FFP-only identity change dirties both fixed-function payloads");

  DirtyState incompatible{};
  const auto wrongAbi = dxmt9::uniform::planDrawBindingTransition(
      true, a, b, DrawBindingAbi::Stage2ArgumentTable,
      DrawBindingPath::Direct);
  check(!dxmt9::uniform::applyDrawBindingTransition(
            incompatible, wrongAbi, counts),
        "slot-30 PSO fails closed before a direct child binding");
  checkEq(incompatible.mask, 0u,
          "ABI rejection has no partial dirty-state side effect");

  DirtyState grow{};
  const DirectCbufPayloadCounts grown{
      .vertexFloat = 12,
      .pixelFloat = 9,
  };
  dxmt9::uniform::applyDrawBindingTransition(grow, aToB, grown);
  checkEq(grow.maxChangedVsF, 12u,
          "constant-count growth uses the current payload range");
  checkEq(grow.maxChangedPsF, 9u,
          "pixel constant-count growth uses the current payload range");
  dxmt9::uniform::clearBits(
      grow, dxmt9::uniform::kVsAny | dxmt9::uniform::kPsAny);
  const DirectCbufPayloadCounts shrunk{
      .vertexFloat = 1,
      .pixelFloat = 2,
  };
  dxmt9::uniform::applyDrawBindingTransition(grow, bToA, shrunk);
  checkEq(grow.maxChangedVsF, 1u,
          "constant-count shrink binds the smaller current payload range");
  checkEq(grow.maxChangedPsF, 2u,
          "pixel constant-count shrink binds the smaller current range");
}

}  // namespace

int main() {
  testBitmaskGranularity();
  testApplyHelpersSetMatchingBit();
  testRangeCounters();
  testClearBit();
  testClearBitsResetsOnlyConsumedRanges();
  testMarkAllDirty();
  testIsDirtyConsistency();
  testShaderUsageAwareUploadPlanIsConservative();
  testShaderUsageAwareUploadPlanFallsBackForUnknownOrIndexedUse();
  testDirectCbufPayloadSourceDirtyRebind();
  testDrawBindingTransitionTruthTable();
  return 0;
}
