#pragma once

// Metal shader-source helpers shared across pipeline builders. Lifted out of
// backend_metal.mm's anonymous namespace so PipelineCache + Presenter + any
// other consumer can call them without pulling in the full backend TU.

#include "../winemetal/Metal.hpp"

#include <cstdint>
#include <string>

namespace dxmt9::core {
struct ColorRGBA;  // forward — defined in core.hpp
}

namespace dxmt9::shaders {

using u64 = std::uint64_t;

// FNV-1a hash of a shader source string, used for variant keying.
u64 makeHash(const std::string& source);

// Generic fullscreen-triangle vertex shader (no attributes). Used by
// clear/fill pipelines that render a solid color across the target.
std::string makeGenericVertexSource(u64 variantHash);

// Generic solid-color fragment shader. Color is baked into the source.
std::string makeGenericFragmentSource(const core::ColorRGBA& color, u64 variantHash);

// Textured fullscreen-triangle vertex shader — generates UVs for a present
// or stretch blit.
std::string makeTexturedVertexSource(u64 variantHash);

// Textured sampling fragment shader. forceOpaqueAlpha=true clamps alpha to
// 1.0 (used for X8R8G8B8 / X8B8G8R8 present paths).
std::string makeTexturedFragmentSource(u64 variantHash, bool forceOpaqueAlpha = false);

// Gamma-ramp-apply fragment shader. Same VS as makeTexturedVertexSource —
// shares the fullscreen-triangle UV emitter. The FS samples the source
// texture and quantizes each channel to a 0..255 index into a 256-entry
// ushort LUT bound at buffer(0). forceOpaqueAlpha=true clamps alpha to
// 1.0 to match the existing X8R8G8B8 / X8B8G8R8 present path. The 1.5 KB
// LUT payload rides as setFragmentBytes (well under Metal's 4 KB inline
// limit) so no MTLBuffer allocation is needed per frame.
std::string makeGammaApplyFragmentSource(u64 variantHash, bool forceOpaqueAlpha = false);

// Compile a source string into a WMT::Library (Metal Shading Language).
// Returns an empty reference on failure.
WMT::Reference<WMT::Library> makeLibrary(WMT::Device& device, const std::string& source);

// Open the on-disk shader binary archive; archiveOut is assigned the result
// (possibly empty on failure).
void initShaderArchive(WMT::Device& device, const std::string& path,
                       WMT::Reference<WMT::BinaryArchive>& archiveOut);

// Value-returning overload for in-place member initialization.
WMT::Reference<WMT::BinaryArchive> initShaderArchive(WMT::Device device, const std::string& path);

// Serialize the archive to disk. No-op if archive is empty or path is empty.
void persistShaderArchive(WMT::BinaryArchive& archive, const std::string& path);

// Shared MSL prelude used by the draw / FFP shader generators: defines
// the per-category uniform structs (VsConsts, PsConsts, FfpVsConsts,
// FfpPsConsts, DrawVolatile), VSOut, and a library of helper inline
// functions (dxmt9_load_*, dxmt9_apply_texture_*, dxmt9_select_*, etc).
// When withClipDistances is true, VSOut gets one clipDistance0
// [[clip_distance]] scalar. Enabled D3D9 clip planes are min-folded into that
// single Apple-supported slot.
struct ShaderPreludeOptions {
  bool withClipDistances = false;
  bool centroidColor = false;
  bool centroidSecondaryColor = false;
  std::uint32_t centroidTexcoordMask = 0;
  bool centroidFogFactor = false;
  // Diagnostic only: request half-precision user varyings in VSOut. Position,
  // point_size, and clip_distance stay float.
  bool halfVSOut = false;
  struct VSOutLayout {
    std::uint32_t texcoordMask = 0xffu;
    bool color = true;
    bool secondaryColor = true;
    bool fogFactor = true;
    bool pointSize = true;
  } vsOutLayout{};
};

std::string makeShaderPrelude(const ShaderPreludeOptions& options);
std::string makeShaderPrelude(bool withClipDistances);

using VSOutLayout = ShaderPreludeOptions::VSOutLayout;

// DXMT9_TRIM_UNUSED_VARYINGS — opt-in pair-local VSOut trimming. The env
// flag only enables the optimization; the actual layout must come from the
// VS/FS pair's fragment-input liveness. This prevents SFIV-style global
// trimming from deleting GT1 inputs such as texcoord5..7 or fogFactor.
bool vsoutTrimEnabled();
bool vsoutProbeDropPointSizeEnabled();
bool vsoutProbePositionOnlyEnabled();
bool vsoutProbeHalfEnabled();
constexpr VSOutLayout fullVSOutLayout() { return VSOutLayout{}; }
VSOutLayout minimalVSOutLayout();
VSOutLayout positionOnlyVSOutLayout();
VSOutLayout applyVSOutProbeOverrides(VSOutLayout layout);
std::uint32_t vsoutLayoutKey(const VSOutLayout& layout);
bool vsoutEmitTexcoord(const VSOutLayout& layout, std::size_t index);
bool vsoutEmitColor(const VSOutLayout& layout);
bool vsoutEmitSecondaryColor(const VSOutLayout& layout);
bool vsoutEmitFogFactor(const VSOutLayout& layout);
bool vsoutEmitPointSize(const VSOutLayout& layout);

// DXMT9_FS_HALF_PRECISION — **EXPERIMENTAL — NOT FUNCTIONAL** opt-in
// half (fp16) emission for the DXBC translator's fragment shader path.
//
// Rationale: Apple Silicon GPUs (M1+) have ~2× FP16 ALU throughput vs
// FP32; SFIV is 98% fragment-bound (verified via
// DXMT_DEBUG_FORCE_FRAGMENT_COLOR A/B in task 115), so half-precision
// is the only dxmt9-side lever that meaningfully moves per-frame GPU
// time without violating D3D9 transparency (D3D9 PS1.x/2.x permit
// `_pp` partial-precision hints, making internal half use contract-
// compatible).
//
// Current status (2026-05-12): the text post-pass rewrite at
// `applyFsHalfPrecisionRewrite` (in dxmt9_shader_metal_ir.cpp) only
// compiles ~33% of SFIV's translated FS sources because MSL's type
// system catches a cascade of float/half boundary mismatches that
// regex substitution cannot resolve:
//   * `texture2d<half>::sample(sampler, float2 coord)` — texcoord must
//     stay float at the sample site even when the body is half-typed.
//   * `dxmt9_merge` / `dxmt9_select_texcoord` helpers in the shared
//     prelude are float-typed; this header emits half overloads but
//     the rewrite doesn't always know whether to dispatch the half
//     variant (depends on argument expression types).
//   * MSL constructor matching for `half4(float3, half)` rejects
//     mixed-precision args. Constructor / literal coercion at boundary
//     sites requires IR-level type tracking, not text substitution.
//
// Path forward: proper implementation requires threading a precision
// flag through the SPIR-V → MSL emit pipeline (~50 emit sites) plus
// an explicit cast-insertion pass at every float↔half boundary. See
// task 119 investigation notes for the full enumeration.
//
// Until then: keep the env-var path off in production. Setting it
// makes ~33% of SFIV's shaders compile and the remainder fail at
// PSO build (silent fallback to a default pipeline → wrong pixels).
bool fsHalfPrecisionEnabled();

// R-BACK-12.22..12.26 — Stage 2 argument-buffer hybrid prelude. Emits
// the same per-category uniform struct definitions as makeShaderPrelude,
// then declares an `ArgbufLayout` MSL struct that wraps the four
// per-frequency constant-buffer pointers at the layout offsets
// described in spec.md §11.2:
//
//   struct ArgbufLayout {
//     constant VsConsts*    vsConsts [[id(0)]];
//     constant FfpVsConsts* ffpVs    [[id(1)]];
//     constant PsConsts*    psConsts [[id(2)]];
//     constant FfpPsConsts* ffpPs    [[id(3)]];
//   };
//
// Shaders compiled with this prelude bind a single argument buffer at
// MTL slot 30 (vertex + fragment) and dereference each constant pointer
// instead of reading dedicated slot-0/3 buffers. Texture/sampler
// resources stay on direct `[[texture(N)]]` / `[[sampler(N)]]`
// binding — the validated Stage 1 lane is retained for resource binds.
// `DrawVolatile` (slot 5, setVertexBytes) and the vertex stream (slot 1)
// also stay direct. See R-BACK-12.23 / spec.md §11.2.
std::string makeShaderPreludeArgbufHybrid(bool withClipDistances);
std::string makeShaderPreludeArgbufHybrid(const ShaderPreludeOptions& options);

// R-BACK-12.22..12.26 (resource-array sub-mode) — opt-in env gate. True
// iff `DXMT9_ARGBUF_RESOURCE_ARRAY` is set to a non-empty, non-"0" value.
// Read once (static init). The Stage 2 hybrid carries texture/sampler
// resources through the slot-30 argbuf only when this is set; default OFF
// keeps the constants-only lane byte-identical. The gate is meaningful
// only when the device-level Stage 2 capability gate already holds.
bool argbufResourceArrayEnabled();

// R-BACK-12.22..12.26 (resource-array sub-mode) — extended ArgbufLayout
// prelude. Same five per-category uniform structs + the four
// constant-buffer pointers at [[id(0..3)]] as makeShaderPreludeArgbufHybrid,
// PLUS a per-stage texture/sampler resource array:
//
//   struct ArgbufLayout {
//     constant VsConsts*    vsConsts [[id(0)]];
//     constant FfpVsConsts* ffpVs    [[id(1)]];
//     constant PsConsts*    psConsts [[id(2)]];
//     constant FfpPsConsts* ffpPs    [[id(3)]];
//     texture2d<float> textures[8] [[id(4)]];
//     sampler          samplers[8] [[id(12)]];
//   };
//
// The texture array is homogeneously typed `texture2d<float>` — cube /
// volume sampling reinterprets the same `[[id]]` slot in the shader via
// an `as_type`-free typed alias emitted by the per-shader emitter (the
// gpuResourceID written by MTLArgumentEncoder_setTexture is type-agnostic
// at the ABI level). The host descriptor table mirrors these [[id]]
// positions (see argbuf_hybrid::buildResourceArrayArgumentDescriptors).
//
// Texture/sampler array sizes are kArgbufResourceArrayStageCount (8 — the
// fixed-function texture-stage count; matches the DXBC s0..s7 fragment
// sampler range the FFP/IR emitters bind).
std::string makeShaderPreludeArgbufResourceArray(bool withClipDistances);
std::string makeShaderPreludeArgbufResourceArray(const ShaderPreludeOptions& options);

// Stage 1 / Stage 2b direct constant-buffer slots. Pinned next to the
// Stage 2 argbuf slot so host binding and emitted MSL stay in lockstep.
inline constexpr std::uint32_t kDirectVsConstsBindSlot = 0u;
inline constexpr std::uint32_t kDirectPsConstsBindSlot = 0u;
inline constexpr std::uint32_t kDirectFfpVsConstsBindSlot = 3u;
inline constexpr std::uint32_t kDirectFfpPsConstsBindSlot = 3u;
inline constexpr std::uint32_t kDirectVertexStreamBindSlot = 1u;
inline constexpr std::uint32_t kDirectDrawVolatileBindSlot = 5u;

// R-BACK-12.22 — argbuf bind slot. Mirrors DXMT's slot-30 convention so
// frame captures and existing tooling stay consistent.
inline constexpr std::uint32_t kArgbufHybridBindSlot = 30u;

// R-BACK-12.23 — argbuf descriptor field counts. The ArgbufLayout
// struct holds exactly four constant-buffer pointers; texture and
// sampler resources are bound directly on the render encoder, not
// through this argument buffer.
inline constexpr std::uint32_t kArgbufHybridConstantBufferCount = 4u;
inline constexpr std::uint32_t kArgbufHybridDescriptorCount =
    kArgbufHybridConstantBufferCount;

// R-BACK-12.22..12.26 (resource-array sub-mode) — argbuf texture/sampler
// array layout. The four constant-buffer pointers keep [[id(0..3)]]; the
// texture array occupies the next kArgbufResourceArrayStageCount ids and
// the sampler array the ids after that. Pinned here so the MSL ArgbufLayout
// struct (emitted by makeShaderPreludeArgbufResourceArray) and the host
// MTLArgumentEncoder descriptor table (buildResourceArrayArgumentDescriptors)
// agree on every [[id(N)]] without a magic number on either side.
//
// 8 stages = the fixed-function texture-stage count (kMaxTextureStages) and
// the DXBC s0..s7 fragment sampler range the FFP/IR emitters bind. Vertex
// texture samplers (s8..) stay on the direct lane in this sub-mode.
inline constexpr std::uint32_t kArgbufResourceArrayStageCount = 8u;
inline constexpr std::uint32_t kArgbufResourceArrayTextureBaseId =
    kArgbufHybridConstantBufferCount;  // id 4
inline constexpr std::uint32_t kArgbufResourceArraySamplerBaseId =
    kArgbufResourceArrayTextureBaseId + kArgbufResourceArrayStageCount;  // id 12
inline constexpr std::uint32_t kArgbufResourceArrayDescriptorCount =
    kArgbufHybridConstantBufferCount + 2u * kArgbufResourceArrayStageCount;  // 4 + 8 + 8 = 20

}  // namespace dxmt9::shaders
