#pragma once

// Per-encoder draw-uniforms dirty tracking. Pure value-transform header
// shared by C1 (chunk-record import — sets bits + range counters) and
// C2 (encoder consumption — reads + clears bits and decides which
// uniform sub-allocations to bind for the next draw).
//
// DirtyState is intentionally POD-like (16-bit mask + six 16-bit range
// counters); the apply* helpers below are pure value transforms with
// no side effects beyond mutating the passed DirtyState&. They live in
// the matching .cpp.

#include "dxmt9/core_constants.hpp"

#include <cstdint>

namespace dxmt9::uniform {

// Single-bit dirty markers, one per uniform category the encoder may
// sub-allocate / bind. Values are uint16_t so DirtyState::mask packs all
// of them in a single field. Ordering matches design.md §4.
enum class DirtyBit : std::uint16_t {
  VsF             = 1u << 0,
  VsI             = 1u << 1,
  VsB             = 1u << 2,
  PsF             = 1u << 3,
  PsI             = 1u << 4,
  PsB             = 1u << 5,
  FfpVsTransforms = 1u << 6,
  FfpVsClip       = 1u << 7,
  FfpVsViewport   = 1u << 8,
  FfpPsFog        = 1u << 9,
  FfpPsAlpha      = 1u << 10,
  FfpPsTexFactor  = 1u << 11,
};

// Dirty state carried by EncodeContext. Default-constructed = nothing
// dirty. C2 calls markAllDirty(...) at encoder init per R-BACK-12.12.
//
// The maxChanged* counters track the highest (startReg + count) seen
// per shader-constant category since the last clear. They are retained
// for a future shader-usage-aware range-upload path; the production
// encoder still binds full VsConsts/PsConsts blocks because the current
// MSL ABI exposes each category as one contiguous struct.
struct DirtyState {
  std::uint16_t mask = 0;
  std::uint16_t maxChangedVsF = 0;
  std::uint16_t maxChangedVsI = 0;
  std::uint16_t maxChangedVsB = 0;
  std::uint16_t maxChangedPsF = 0;
  std::uint16_t maxChangedPsI = 0;
  std::uint16_t maxChangedPsB = 0;
};

struct ShaderConstantUsageBounds {
  std::uint16_t floatCount = 0;
  std::uint16_t intCount = 0;
  std::uint16_t boolCount = 0;
  bool indexedFloat = false;
  bool indexedInt = false;
  bool indexedBool = false;
  bool unknown = true;
};

struct ShaderConstantUploadPlan {
  std::uint16_t floatCount = 0;
  std::uint16_t intCount = 0;
  std::uint16_t boolCount = 0;
  bool fullStructRequired = true;
};

// Composite per-frequency masks. The encoder consumes the per-frequency
// UBOs as a unit (one sub-allocation, one bind, one clear), so it
// checks/clears all bits in a category in one shot. Defining these once
// here keeps the call sites readable and matches the C2 mental model.
inline constexpr std::uint16_t kVsAny =
    static_cast<std::uint16_t>(DirtyBit::VsF) |
    static_cast<std::uint16_t>(DirtyBit::VsI) |
    static_cast<std::uint16_t>(DirtyBit::VsB);
inline constexpr std::uint16_t kPsAny =
    static_cast<std::uint16_t>(DirtyBit::PsF) |
    static_cast<std::uint16_t>(DirtyBit::PsI) |
    static_cast<std::uint16_t>(DirtyBit::PsB);
inline constexpr std::uint16_t kFfpVsAny =
    static_cast<std::uint16_t>(DirtyBit::FfpVsTransforms) |
    static_cast<std::uint16_t>(DirtyBit::FfpVsClip) |
    static_cast<std::uint16_t>(DirtyBit::FfpVsViewport);
inline constexpr std::uint16_t kFfpPsAny =
    static_cast<std::uint16_t>(DirtyBit::FfpPsFog) |
    static_cast<std::uint16_t>(DirtyBit::FfpPsAlpha) |
    static_cast<std::uint16_t>(DirtyBit::FfpPsTexFactor);

// Bit-level helpers — pure value transforms.
void markAllDirty(DirtyState& state);
void clearBit(DirtyState& state, DirtyBit bit);
void clearBits(DirtyState& state, std::uint16_t mask);
void setBit(DirtyState& state, DirtyBit bit);
bool isDirty(const DirtyState& state, DirtyBit bit);
bool anyDirty(const DirtyState& state, std::uint16_t mask);

// Per-record apply helpers. The chunk-record importer calls these as
// it processes records; each ORs in the matching DirtyBit and (for
// constant-set categories) bumps the high-water counter via saturating
// add so a malformed (startReg + count) overflow can never wrap below
// the previous max. Counts are clamped at u16 max — the actual D3D9
// limits (kMaxVertex/PixelConstants <= 256) are well within range, and
// the saturate is purely a safety guard against malformed records.
void applyConstantSetVsF(DirtyState& state, std::uint32_t startReg, std::uint32_t count);
void applyConstantSetVsI(DirtyState& state, std::uint32_t startReg, std::uint32_t count);
void applyConstantSetVsB(DirtyState& state, std::uint32_t startReg, std::uint32_t count);
void applyConstantSetPsF(DirtyState& state, std::uint32_t startReg, std::uint32_t count);
void applyConstantSetPsI(DirtyState& state, std::uint32_t startReg, std::uint32_t count);
void applyConstantSetPsB(DirtyState& state, std::uint32_t startReg, std::uint32_t count);

// Coarse FFP / fixed-uniform-block apply helpers. These OR the matching
// DirtyBit; they do not carry per-element ranges because the FFP
// uniform sub-blocks are uploaded whole when any contributor changes.
void applyTransformChange(DirtyState& state);    // ANY world/view/proj/tex transform
void applyClipPlaneChange(DirtyState& state);
void applyViewportChange(DirtyState& state);
void applyRenderStateFog(DirtyState& state);     // RS_FOG* family
void applyRenderStateAlpha(DirtyState& state);   // RS_ALPHA_TEST_ENABLE/FUNC/REF
void applyRenderStateTexFactor(DirtyState& state); // RS_TEXTURE_FACTOR

// Conservative planning helper for the shader-usage-aware upload path.
// The current MSL ABI still binds full VsConsts/PsConsts structs; these
// pure transforms expose the future range boundary without changing live
// binding lifetime or buffer layout.
ShaderConstantUploadPlan makeVsConstantUploadPlan(
    const DirtyState& state,
    ShaderConstantUsageBounds usage);
ShaderConstantUploadPlan makePsConstantUploadPlan(
    const DirtyState& state,
    ShaderConstantUsageBounds usage);

// Byte prefix required by the current MSL-visible VsConsts/PsConsts ABI.
// Fixed-range plans may upload only the prefix that reaches the last used
// member. Unknown or indexed plans return the full host-struct size.
std::uint64_t vsConstantUploadBytes(ShaderConstantUploadPlan plan) noexcept;
std::uint64_t psConstantUploadBytes(ShaderConstantUploadPlan plan) noexcept;

}  // namespace dxmt9::uniform
