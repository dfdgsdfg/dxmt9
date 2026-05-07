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

// Layout consumed by the generated draw shaders. Must match the
// declarations in dxmt9::shaders / dxmt9::drawshader shader source.
struct DrawUniforms {
  std::array<std::array<f32, 4>, core::kMaxVertexConstants> vsFloatConst{};
  std::array<std::array<i32, 4>, core::kMaxIntegerConstants> vsIntConst{};
  std::array<u32, core::kMaxBoolConstants> vsBoolConst{};
  std::array<std::array<f32, 4>, 4> ffpWorldViewProj{};
  std::array<std::array<std::array<f32, 4>, 4>, core::kMaxTextureStages> ffpTextureTransforms{};
  std::array<std::array<f32, 4>, core::kMaxPixelConstants> psFloatConst{};
  std::array<std::array<i32, 4>, core::kMaxIntegerConstants> psIntConst{};
  std::array<u32, core::kMaxBoolConstants> psBoolConst{};
  std::array<core::ClipPlane, core::kMaxClipPlanes> clipPlanes{};
  std::array<f32, 2> halfPixelFixup{};
  std::array<f32, 2> viewportOrigin{};
  std::array<f32, 2> viewportSize{};
  std::array<f32, 4> textureFactor{1.0f, 1.0f, 1.0f, 1.0f};
  f32 alphaRef = 0.0f;
  f32 fogStart = 1.0f;
  f32 fogEnd = 1.0f;
  f32 fogDensity = 1.0f;
  u32 vertexStreamOffset = 0;
  u32 vertexStreamStride = 0;
  i32 vertexBaseIndex = 0;
  u32 clipPlaneMask = 0;
  u32 alphaTestEnable = 0;
  u32 alphaTestFunc = static_cast<u32>(core::CompareFunc::Always);
  u32 fogMode = static_cast<u32>(core::FogMode::None);
};

// Per-stage split of DrawUniforms. VS-only constants. Sized so MSL
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
  std::array<std::array<std::array<f32, 4>, 4>, core::kMaxTextureStages> ffpTextureTransforms{};
  std::array<core::ClipPlane, core::kMaxClipPlanes> clipPlanes{};
  std::array<f32, 2> halfPixelFixup{};
  std::array<f32, 2> viewportOrigin{};
  std::array<f32, 2> viewportSize{};
  u32 clipPlaneMask = 0;
};
static_assert(sizeof(FfpVsConsts) == 700,
              "FfpVsConsts layout must match MSL prelude declaration");

struct FfpPsConsts {
  std::array<f32, 4> textureFactor{1.0f, 1.0f, 1.0f, 1.0f};
  f32 alphaRef = 0.0f;
  f32 fogStart = 1.0f;
  f32 fogEnd = 1.0f;
  f32 fogDensity = 1.0f;
  u32 alphaTestEnable = 0;
  u32 alphaTestFunc = static_cast<u32>(core::CompareFunc::Always);
  u32 fogMode = static_cast<u32>(core::FogMode::None);
};
static_assert(sizeof(FfpPsConsts) == 44,
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

// Translate D3D9 flat draw state into the uniforms consumed by MSL.
DrawUniforms buildDrawUniforms(core::FlatDrawStateView state);

// Per-stage transforms producing the split structs above. Pure value
// transforms over flat draw state — same field-to-category mapping as
// buildDrawUniforms; later tasks switch encoders over to these.
VsConsts buildVsConsts(core::FlatDrawStateView state);
PsConsts buildPsConsts(core::FlatDrawStateView state);
FfpVsConsts buildFfpVsConsts(core::FlatDrawStateView state);
FfpPsConsts buildFfpPsConsts(core::FlatDrawStateView state);
DrawVolatile buildDrawVolatile(i32 vertexBaseIndex, u32 vertexStreamOffset,
                               u32 vertexStreamStride);

// Compose a depth/stencil cache key from flat render-state storage.
pipeline::DepthStencilKey makeDepthStencilKey(core::FlatDrawStateView state);

}  // namespace dxmt9::state
