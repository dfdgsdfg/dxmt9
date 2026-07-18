#pragma once

// Per-draw uniform buffer + depth/stencil key builder. Previously lived
// in backend_metal.mm's anonymous namespace. Pure data transforms over
// flat draw state — no dependency on backend state.

#include "dxmt9/core.hpp"
#include "dxmt9_pipeline_cache.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dxmt9::uniform {
struct ShaderConstantUploadPlan;
}  // namespace dxmt9::uniform

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
  // Point/Spot lighting (D3D9 §B.5). lightPosition.xyz and lightDirection.xyz
  // are transformed from D3D world space into camera space before upload;
  // lightPosition.w carries Range. lightAttenuation packs
  // (atten0, atten1, atten2, falloff) used by the D3D9 attenuation /
  // spot-factor equations. lightSpotCone packs (cos(theta/2),
  // cos(phi/2), 1.0, 0.0); the trailing scalars are reserved for future
  // packing and keep the float4 array element aligned. Type per-slot is
  // already in FfpVertexKey.lightType[] (Directional/Point/Spot).
  std::array<std::array<f32, 4>, core::kMaxLights> lightPosition{};
  std::array<std::array<f32, 4>, core::kMaxLights> lightAttenuation{};
  std::array<std::array<f32, 4>, core::kMaxLights> lightSpotCone{};
  std::array<std::array<std::array<f32, 4>, 4>, 4> ffpBlendWorldViewProj{};
  std::array<std::array<std::array<f32, 4>, 4>, 4> ffpBlendWorldView{};
  std::array<std::array<std::array<f32, 4>, 4>, 4> ffpBlendNormalMatrix{};
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
static_assert(sizeof(FfpVsConsts) == 2632,
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

// Per-draw push constants. Stream divisors are zero for per-vertex streams and
// non-zero for D3DSTREAMSOURCE_INSTANCEDATA streams.
struct DrawVolatile {
  i32 vertexBaseIndex = 0;
  u32 vertexStreamOffset = 0;
  u32 vertexStreamStride = 0;
  u32 _pad = 0;
  std::array<u32, core::kMaxStreams> streamInstanceDivisors{};
};
static_assert(sizeof(DrawVolatile) == 80,
              "DrawVolatile layout must match MSL prelude declaration");

// H228 — per-draw fragment immediate carrying the alpha-test state. Bound via
// setFragmentBytes at fragment buffer slot 5 (the fragment-stage mirror of the
// vertex DrawVolatile slot-5 immediate lane; fragment slots 0/3 are the direct
// cbufs, 4 is SamplerLodBias, 30 is the argbuf — 5 is free in every binding
// mode and, like DrawVolatile, stays direct under the argbuf hybrid). The
// generated fragment alpha-test tail is a SINGLE shader variant that reads
// this struct at runtime, so draw runs / submission batches can span per-draw
// alpha-test toggles without a PSO switch.
struct FsVolatile {
  u32 alphaTest = 0;   // 0 = alpha test off, else D3DCMPFUNC (1..8)
  f32 alphaRef = 0.0f; // RS_ALPHA_REF DWORD / 255.0f (matches fillFfpPsConsts)
  u32 sampleMask = 0xffffffffu;
  u32 _pad = 0;

  friend constexpr bool operator==(const FsVolatile&, const FsVolatile&) = default;
};
static_assert(sizeof(FsVolatile) == 16,
              "FsVolatile layout must match the MSL struct bound at fragment buffer 5");

// Per-sampler mip LOD bias (D3DSAMP_MIPMAPLODBIAS, gap_d3d9 B.3). Metal's
// MTLSamplerDescriptor has no LOD-bias field — the bias is applied at sample
// time in the shader via `texture.sample(sampler, coord, bias(b))`. This is a
// dedicated fragment uniform bound at buffer slot 4 (textures/samplers also
// stay direct, including under the argbuf-hybrid path, so slot 4 is consistent
// in both modes). 8 slots mirror the 8 FFP/translated texture stages; the
// emitters declare a byte-identical `struct SamplerLodBias` inline (the shader
// prelude is intentionally untouched). Default 0.0 makes every `bias(0.0)` a
// runtime no-op, so the common no-bias draw renders unchanged.
struct SamplerLodBias {
  std::array<f32, core::kMaxTextureStages> bias{};
};
static_assert(sizeof(SamplerLodBias) == core::kMaxTextureStages * sizeof(f32),
              "SamplerLodBias must be a tight float[8] for the MSL slot-4 binding");

// Per-stage transforms producing the split structs above. Pure value
// transforms over flat draw state.
VsConsts buildVsConsts(core::FlatDrawStateView state);
PsConsts buildPsConsts(core::FlatDrawStateView state);
void buildVsConstsUploadBytes(core::FlatDrawStateView state,
                              uniform::ShaderConstantUploadPlan plan,
                              std::span<std::byte> dst);
void buildPsConstsUploadBytes(core::FlatDrawStateView state,
                              uniform::ShaderConstantUploadPlan plan,
                              std::span<std::byte> dst);
FfpVsConsts buildFfpVsConsts(core::FlatDrawStateView state);
FfpPsConsts buildFfpPsConsts(core::FlatDrawStateView state);
void buildFfpPsConstsUploadBytes(core::FlatDrawStateView state,
                                 std::span<std::byte> dst);
SamplerLodBias buildSamplerLodBias(core::FlatDrawStateView state);
// PSO-variant gate predicate for D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3). True iff
// any active sampler stage carries a non-zero mip LOD bias. Single source of
// truth: the PSO key bit (ShaderVariantKey::samplerLodBias, set in
// makeShaderVariantKey) and the encoder's slot-4 SamplerLodBias bind both read
// this same predicate over the same flat draw state, so the shader's
// `[[buffer(4)]]` declaration and the bind can never drift apart.
bool anySamplerLodBiasNonzero(core::FlatDrawStateView state);

// Fragment tail gate predicates. Fog stays an H224 compile-time PSO variant:
// fragmentFogCouldApply is the single source of truth shared by the PSO key
// (ShaderVariantKey::fogActive, set in makeShaderVariantKey), the
// shader-source context (ShaderSourceContext::fogActive, set in
// makeShaderSourceContext), and the FfpPsConsts upload (fillFfpPsConsts).
// Alpha test is NOT a variant anymore (H228): the tail is always emitted and
// reads the per-draw FsVolatile immediate; fragmentAlphaTestEnabled now feeds
// only the FfpPsConsts upload, buildFsVolatile, and encoder diagnostics.
// resolveFragmentFog mirrors the historical
// buildFfpPsConsts resolution exactly: RS_FOG_ENABLE gates fog entirely, the
// table mode wins, and the vertex mode is the fallback (fogSource=1 marks the
// vertex-fog-factor lane). The RenderStateSnapshot overloads serve fixture /
// offline DrawDesc paths with identical semantics.
struct ResolvedFragmentFog {
  u32 fogMode = static_cast<u32>(core::FogMode::None);
  u32 fogSource = 0;  // 1 = interpolated vertex fog factor, 0 = table/depth fog
};
bool fragmentAlphaTestEnabled(const core::FlatRenderStateSet& renderStates);
bool fragmentAlphaTestEnabled(const core::RenderStateSnapshot& renderStates);
ResolvedFragmentFog resolveFragmentFog(const core::FlatRenderStateSet& renderStates);
ResolvedFragmentFog resolveFragmentFog(const core::RenderStateSnapshot& renderStates);
// True iff the resolved draw state can produce a non-zero ffpPs.fogMode at
// upload time — the coarse "could fog apply" signal the fog variant keys on.
bool fragmentFogCouldApply(const core::FlatRenderStateSet& renderStates);
bool fragmentFogCouldApply(const core::RenderStateSnapshot& renderStates);

DrawVolatile buildDrawVolatile(i32 vertexBaseIndex, u32 vertexStreamOffset,
                               u32 vertexStreamStride);
DrawVolatile buildDrawVolatile(
    i32 vertexBaseIndex, u32 vertexStreamOffset, u32 vertexStreamStride,
    const std::array<u32, core::kMaxStreams>& streamFrequencies);

// H228 — single-source conversion from raw D3DRS alpha-test values to the
// per-draw fragment immediate. Both the flat-state path (canonical/base
// draws) and the per-draw DrawBindingOverride path (run/batch draws) funnel
// through makeFsVolatile so the 0-255 -> float alphaRef conversion and the
// DXMT_DISABLE_ALPHA_TEST composition can never drift from the FfpPsConsts
// upload semantics in fillFfpPsConsts.
FsVolatile makeFsVolatile(u32 alphaTestEnable, u32 alphaTestFunc,
                          u32 alphaTestRefRaw,
                          u32 sampleMask = 0xffffffffu);
FsVolatile buildFsVolatile(core::FlatDrawStateView state);

// Compose a depth/stencil cache key from flat render-state storage.
pipeline::DepthStencilKey makeDepthStencilKey(core::FlatDrawStateView state);

// Extract the D3D9 D3DRS_STENCILREF byte (0..255). D3D9 only carries a
// single stencil ref (no D3DRS_CCW_STENCILREF — see
// `~/workspaces/wine/include/d3d9types.h:1029-1033`), so the value is
// applied to both faces; Metal's `setStencilReferenceValue` mirrors that.
std::uint8_t computeStencilRef(core::FlatDrawStateView state);

}  // namespace dxmt9::state
