#pragma once

// Per-draw uniform buffer + depth/stencil key builder. Previously lived
// in backend_metal.mm's anonymous namespace. Pure data transforms over
// flat draw state — no dependency on backend state.

#include "dxmt9/core.hpp"
#include "dxmt9_pipeline_cache.hpp"

#include <array>
#include <cstdint>

namespace dxmt9::state {

using u32 = std::uint32_t;
using i32 = std::int32_t;
using f32 = float;

// Per-frequency draw uniforms split by stage and update cadence (see
// specs/backend/draw-uniforms). VS-only constants. Sized so MSL
// `float4`/`int4`/`uint` arrays match the host layout byte-for-byte.
struct VsConsts {
  std::array<std::array<f32, 4>, core::kMaxVertexConstants> vsFloatConst{};
  std::array<std::array<i32, 4>, core::kMaxIntegerConstants> vsIntConst{};
  std::array<u32, core::kMaxBoolConstants> vsBoolConst{};
};
static_assert(sizeof(VsConsts) == 4416,
              "VsConsts layout must match MSL prelude declaration");

struct PsConsts {
  std::array<std::array<f32, 4>, core::kMaxPixelConstants> psFloatConst{};
  std::array<std::array<i32, 4>, core::kMaxIntegerConstants> psIntConst{};
  std::array<u32, core::kMaxBoolConstants> psBoolConst{};
};
static_assert(sizeof(PsConsts) == 3904,
              "PsConsts layout must match MSL prelude declaration");

struct FfpVsConsts {
  std::array<std::array<f32, 4>, 4> ffpWorldViewProj{};
  std::array<std::array<f32, 4>, 4> ffpWorldView{};
  std::array<std::array<f32, 4>, 4> ffpNormalMatrix{};
  std::array<f32, 4> materialEmissive{};
  std::array<f32, 4> materialAmbient{};
  std::array<f32, 4> materialDiffuse{};
  std::array<f32, 4> materialSpecular{};
  std::array<f32, 4> globalAmbient{};
  std::array<f32, 4> materialPower{};
  std::array<std::array<f32, 4>, core::kMaxLights> lightDiffuse{};
  std::array<std::array<f32, 4>, core::kMaxLights> lightSpecular{};
  std::array<std::array<f32, 4>, core::kMaxLights> lightAmbient{};
  std::array<std::array<f32, 4>, core::kMaxLights> lightDirection{};
  std::array<std::array<std::array<f32, 4>, 4>, 4> ffpBlendWorldViewProj{};
  std::array<std::array<std::array<f32, 4>, 4>, core::kMaxTextureStages> ffpTextureTransforms{};
  std::array<core::ClipPlane, core::kMaxClipPlanes> clipPlanes{};
  std::array<f32, 2> halfPixelFixup{};
  std::array<f32, 2> viewportOrigin{};
  std::array<f32, 2> viewportSize{};
  f32 fogStart = 1.0f;
  f32 fogEnd = 1.0f;
  f32 fogDensity = 1.0f;
  u32 fogMode = 0;
  u32 rangeFog = 0;
  u32 clipPlaneMask = 0;
  f32 pointSize = 1.0f;
  f32 pointSizeMin = 1.0f;
  f32 pointSizeMax = 64.0f;
  f32 pointScaleA = 1.0f;
  f32 pointScaleB = 0.0f;
  f32 pointScaleC = 0.0f;
};
static_assert(sizeof(FfpVsConsts) == 1736,
              "FfpVsConsts layout must match MSL prelude declaration");

struct FfpPsConsts {
  std::array<f32, 4> textureFactor{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<std::array<f32, 4>, core::kMaxTextureStages> stageConstants{};
  f32 alphaRef = 0.0f;
  f32 fogStart = 1.0f;
  f32 fogEnd = 1.0f;
  f32 fogDensity = 1.0f;
  u32 alphaTestEnable = 0;
  u32 alphaTestFunc = static_cast<u32>(core::CompareFunc::Always);
  u32 fogMode = static_cast<u32>(core::FogMode::None);
  u32 fogSource = 0;
  std::array<std::array<f32, 4>, core::kMaxTextureStages> bumpEnvMat{};
  std::array<std::array<f32, 2>, core::kMaxTextureStages> bumpEnvLum{};
  std::array<f32, 4> fogColor{};
};
static_assert(sizeof(FfpPsConsts) == 384,
              "FfpPsConsts layout must match MSL prelude declaration");

// Per-draw push constants. Padded to 16 B so Metal setVertexBytes
// requirements (4 B alignment minimum, 16 B convention for uniforms) hold.
struct DrawVolatile {
  i32 vertexBaseIndex = 0;
  u32 vertexStreamOffset = 0;
  u32 vertexStreamStride = 0;
  u32 _pad = 0;
};
static_assert(sizeof(DrawVolatile) == 16,
              "DrawVolatile layout must match MSL prelude declaration");

// Per-stage transforms producing the split structs above. Pure value
// transforms over flat draw state.
VsConsts buildVsConsts(core::FlatDrawStateView state);
PsConsts buildPsConsts(core::FlatDrawStateView state);
FfpVsConsts buildFfpVsConsts(core::FlatDrawStateView state);
FfpPsConsts buildFfpPsConsts(core::FlatDrawStateView state);
DrawVolatile buildDrawVolatile(i32 vertexBaseIndex, u32 vertexStreamOffset,
                               u32 vertexStreamStride);

// Compose a depth/stencil cache key from flat render-state storage.
pipeline::DepthStencilKey makeDepthStencilKey(core::FlatDrawStateView state);

// Extract the D3D9 D3DRS_STENCILREF byte (0..255). D3D9 only carries a
// single stencil ref (no D3DRS_CCW_STENCILREF — see
// `~/workspaces/wine/include/d3d9types.h:1029-1033`), so the value is
// applied to both faces; Metal's `setStencilReferenceValue` mirrors that.
std::uint8_t computeStencilRef(core::FlatDrawStateView state);

}  // namespace dxmt9::state
