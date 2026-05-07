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

}  // namespace

void markAllDirty(DirtyState& state) {
  state.mask = kAllBits;
  // C2 reads the range counters when VsF/VsI/... is dirty. After a
  // markAllDirty (encoder init / per R-BACK-12.12), the counters carry
  // the previously-observed high-water marks, which are still valid as
  // an upper bound on slots that were ever written. We do NOT zero
  // them: zeroing would tell C2 "no slots were ever written", which is
  // a missed-dirty bug.
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

}  // namespace dxmt9::uniform
