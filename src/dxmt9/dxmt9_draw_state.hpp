#pragma once

// Per-draw uniform buffer + depth/stencil key builder. Previously lived
// in backend_metal.mm's anonymous namespace. Pure data transforms over
// the public DrawDesc — no dependency on backend state.

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

// Translate D3D9 DrawDesc state into the uniforms consumed by MSL.
DrawUniforms buildDrawUniforms(const core::DrawDesc& desc);

// Compose a depth/stencil cache key from a DrawDesc's render-state map.
pipeline::DepthStencilKey makeDepthStencilKey(const core::DrawDesc& desc);

}  // namespace dxmt9::state
