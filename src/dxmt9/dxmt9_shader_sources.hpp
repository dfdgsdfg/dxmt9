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
// When withClipDistances is true, VSOut gets a clipDistance[6] array.
std::string makeShaderPrelude(bool withClipDistances);

// DXMT9_TRIM_UNUSED_VARYINGS — opt-in trimming of VSOut fields that the
// configured app's fragment shaders never read. Inspected once at first
// call, cached. When enabled, drops texcoord5/6/7 + fogFactor + pointSize
// from both the VSOut struct emit and every VS-body write to those
// fields, shrinking the inter-stage parameter buffer by 56 bytes/vertex.
// Default off — the trim is workload-specific (verified for
// `street-fighter-iv-benchmark`'s 15 dumped FS); apps that DO sample
// texcoord5..7 or read fogFactor/pointSize will produce wrong pixels.
//
// vsoutMaxTexcoord: 8 by default, 5 when trim is active. Loops over
// texture stages in the VS emitters and the
// `dxmt9_select_texcoord(in, N)` helper switch read this.
// vsoutEmitFogFactor / vsoutEmitPointSize: true by default, false when
// trim is active. Guards prelude declaration + VS body writes.
std::size_t vsoutMaxTexcoord();
bool vsoutEmitFogFactor();
bool vsoutEmitPointSize();

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
// then declares an `ArgbufLayout` MSL struct that wraps the per-stage
// constant pointers + texture/sampler descriptors at the layout offsets
// described in design.md §11.2:
//
//   struct ArgbufLayout {
//     constant VsConsts* vsConsts    [[id(0)]];
//     constant FfpVsConsts* ffpVs    [[id(1)]];
//     constant PsConsts* psConsts    [[id(2)]];
//     constant FfpPsConsts* ffpPs    [[id(3)]];
//     array<texture2d<float>, 8> textures2d     [[id(4)]];
//     array<texturecube<float>, 8> texturesCube [[id(12)]];
//     array<texture3d<float>, 8> textures3d     [[id(20)]];
//     array<sampler, 8> samplers                [[id(28)]];
//   };
//
// Shaders compiled with this prelude bind a single argument buffer at
// MTL slot 30 (vertex + fragment) and dereference each member instead
// of reading dedicated slot-0/3 buffers. `DrawVolatile` (slot 5,
// setVertexBytes) and the vertex stream (slot 1) stay on direct
// binding — see R-BACK-12.23 / design.md §11.2.
std::string makeShaderPreludeArgbufHybrid(bool withClipDistances);

// R-BACK-12.22 — argbuf bind slot. Mirrors DXMT's slot-30 convention so
// frame captures and existing tooling stay consistent.
inline constexpr std::uint32_t kArgbufHybridBindSlot = 30u;

// R-BACK-12.23 — argbuf descriptor field counts. The ArgbufLayout
// struct holds 4 constant-buffer pointers, three texture arrays, and
// one sampler array. The array fields expose element ids 4..35, but
// each array is represented by one MTLArgumentDescriptor with
// arrayLength=8.
inline constexpr std::uint32_t kArgbufHybridConstantBufferCount = 4u;
inline constexpr std::uint32_t kArgbufHybridTextureSlotCount = 8u;
inline constexpr std::uint32_t kArgbufHybridTexture2DBase =
    kArgbufHybridConstantBufferCount;
inline constexpr std::uint32_t kArgbufHybridTextureCubeBase =
    kArgbufHybridTexture2DBase + kArgbufHybridTextureSlotCount;
inline constexpr std::uint32_t kArgbufHybridTexture3DBase =
    kArgbufHybridTextureCubeBase + kArgbufHybridTextureSlotCount;
inline constexpr std::uint32_t kArgbufHybridSamplerBase =
    kArgbufHybridTexture3DBase + kArgbufHybridTextureSlotCount;
inline constexpr std::uint32_t kArgbufHybridSamplerSlotCount = 8u;
inline constexpr std::uint32_t kArgbufHybridDescriptorCount =
    kArgbufHybridConstantBufferCount + 3u + 1u;

}  // namespace dxmt9::shaders
