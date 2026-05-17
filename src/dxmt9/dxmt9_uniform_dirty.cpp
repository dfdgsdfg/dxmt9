#include "dxmt9_uniform_dirty.hpp"

#include <algorithm>
#include <limits>

namespace dxmt9::uniform {

namespace {

// Saturating add into u16. The constant-set counter must never wrap
// past u16 max even if a malformed record claims a giant count: that
// would re-enter the "low" range and mask a real high-water mark.
std::uint16_t saturatingAddU16(std::uint32_t startReg, std::uint32_t count) {
  const std::uint64_t end = static_cast<std::uint64_t>(startReg) +
                            static_cast<std::uint64_t>(count);
  if (end > static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max())) {
    return std::numeric_limits<std::uint16_t>::max();
  }
  return static_cast<std::uint16_t>(end);
}

void bumpRange(std::uint16_t& slot, std::uint32_t startReg, std::uint32_t count) {
  const std::uint16_t candidate = saturatingAddU16(startReg, count);
  slot = std::max(slot, candidate);
}

constexpr std::uint16_t kAllBits =
    static_cast<std::uint16_t>(DirtyBit::VsF) |
    static_cast<std::uint16_t>(DirtyBit::VsI) |
    static_cast<std::uint16_t>(DirtyBit::VsB) |
    static_cast<std::uint16_t>(DirtyBit::PsF) |
    static_cast<std::uint16_t>(DirtyBit::PsI) |
    static_cast<std::uint16_t>(DirtyBit::PsB) |
    static_cast<std::uint16_t>(DirtyBit::FfpVsTransforms) |
    static_cast<std::uint16_t>(DirtyBit::FfpVsClip) |
    static_cast<std::uint16_t>(DirtyBit::FfpVsViewport) |
    static_cast<std::uint16_t>(DirtyBit::FfpPsFog) |
    static_cast<std::uint16_t>(DirtyBit::FfpPsAlpha) |
    static_cast<std::uint16_t>(DirtyBit::FfpPsTexFactor);

std::uint16_t clampCount(std::uint16_t value, std::uint16_t limit) {
  return std::min(value, limit);
}

ShaderConstantUploadPlan makeConstantUploadPlan(
    std::uint16_t dirtyFloatCount,
    std::uint16_t dirtyIntCount,
    std::uint16_t dirtyBoolCount,
    ShaderConstantUsageBounds usage,
    std::uint16_t maxFloatCount,
    std::uint16_t maxIntCount,
    std::uint16_t maxBoolCount) {
  if (usage.unknown || usage.indexedFloat || usage.indexedInt || usage.indexedBool) {
    return ShaderConstantUploadPlan{
        .floatCount = maxFloatCount,
        .intCount = maxIntCount,
        .boolCount = maxBoolCount,
        .fullStructRequired = true,
    };
  }

  return ShaderConstantUploadPlan{
      .floatCount = clampCount(std::max(dirtyFloatCount, usage.floatCount), maxFloatCount),
      .intCount = clampCount(std::max(dirtyIntCount, usage.intCount), maxIntCount),
      .boolCount = clampCount(std::max(dirtyBoolCount, usage.boolCount), maxBoolCount),
      .fullStructRequired = false,
  };
}

}  // namespace

void markAllDirty(DirtyState& state) {
  state.mask = kAllBits;
  // Keep range counters intact across all-dirty transitions. They are
  // diagnostic/future-planning metadata today; zeroing them would still
  // lose the only high-water evidence collected from incoming state
  // records.
}

void clearBit(DirtyState& state, DirtyBit bit) {
  state.mask &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(bit));
}

void clearBits(DirtyState& state, std::uint16_t mask) {
  state.mask = static_cast<std::uint16_t>(state.mask & ~mask);
}

void setBit(DirtyState& state, DirtyBit bit) {
  state.mask |= static_cast<std::uint16_t>(bit);
}

bool isDirty(const DirtyState& state, DirtyBit bit) {
  return (state.mask & static_cast<std::uint16_t>(bit)) != 0;
}

bool anyDirty(const DirtyState& state, std::uint16_t mask) {
  return (state.mask & mask) != 0;
}

void applyConstantSetVsF(DirtyState& state, std::uint32_t startReg, std::uint32_t count) {
  setBit(state, DirtyBit::VsF);
  bumpRange(state.maxChangedVsF, startReg, count);
}

void applyConstantSetVsI(DirtyState& state, std::uint32_t startReg, std::uint32_t count) {
  setBit(state, DirtyBit::VsI);
  bumpRange(state.maxChangedVsI, startReg, count);
}

void applyConstantSetVsB(DirtyState& state, std::uint32_t startReg, std::uint32_t count) {
  setBit(state, DirtyBit::VsB);
  bumpRange(state.maxChangedVsB, startReg, count);
}

void applyConstantSetPsF(DirtyState& state, std::uint32_t startReg, std::uint32_t count) {
  setBit(state, DirtyBit::PsF);
  bumpRange(state.maxChangedPsF, startReg, count);
}

void applyConstantSetPsI(DirtyState& state, std::uint32_t startReg, std::uint32_t count) {
  setBit(state, DirtyBit::PsI);
  bumpRange(state.maxChangedPsI, startReg, count);
}

void applyConstantSetPsB(DirtyState& state, std::uint32_t startReg, std::uint32_t count) {
  setBit(state, DirtyBit::PsB);
  bumpRange(state.maxChangedPsB, startReg, count);
}

void applyTransformChange(DirtyState& state) {
  setBit(state, DirtyBit::FfpVsTransforms);
}

void applyClipPlaneChange(DirtyState& state) {
  setBit(state, DirtyBit::FfpVsClip);
}

void applyViewportChange(DirtyState& state) {
  setBit(state, DirtyBit::FfpVsViewport);
}

void applyRenderStateFog(DirtyState& state) {
  setBit(state, DirtyBit::FfpPsFog);
}

void applyRenderStateAlpha(DirtyState& state) {
  setBit(state, DirtyBit::FfpPsAlpha);
}

void applyRenderStateTexFactor(DirtyState& state) {
  setBit(state, DirtyBit::FfpPsTexFactor);
}

ShaderConstantUploadPlan makeVsConstantUploadPlan(
    const DirtyState& state,
    ShaderConstantUsageBounds usage) {
  return makeConstantUploadPlan(
      state.maxChangedVsF,
      state.maxChangedVsI,
      state.maxChangedVsB,
      usage,
      static_cast<std::uint16_t>(core::kMaxVertexConstants),
      static_cast<std::uint16_t>(core::kMaxIntegerConstants),
      static_cast<std::uint16_t>(core::kMaxBoolConstants));
}

ShaderConstantUploadPlan makePsConstantUploadPlan(
    const DirtyState& state,
    ShaderConstantUsageBounds usage) {
  return makeConstantUploadPlan(
      state.maxChangedPsF,
      state.maxChangedPsI,
      state.maxChangedPsB,
      usage,
      static_cast<std::uint16_t>(core::kMaxPixelConstants),
      static_cast<std::uint16_t>(core::kMaxIntegerConstants),
      static_cast<std::uint16_t>(core::kMaxBoolConstants));
}

}  // namespace dxmt9::uniform
