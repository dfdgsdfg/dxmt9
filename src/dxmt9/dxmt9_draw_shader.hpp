#pragma once

// Dispatches a ShaderSourceContext to the appropriate MSL source generator:
//   - Translated D3D bytecode → dxmt9::translator
//   - Fixed-function D3D9 pipeline → dxmt9::ffp
//   - Minimal passthrough (no user shader) → dxmt9::shaders
//
// Extracted from backend_metal.mm. Callers are the draw pipeline builder
// in dxmt9::pipeline::Cache + the WinemetalShaderCompileRequest service.

#include "dxmt9/core.hpp"
#include "dxmt9_shader_sources.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace dxmt9::drawshader {

// Minimal shader-source inputs. This intentionally omits draw payload,
// attachment handles, viewport state, and other encode-time data so pipeline
// construction does not need a full DrawDesc on the hot path.
struct ShaderSourceContext {
  core::VertexDeclSnapshot vertexDecl{};
  core::ShaderRef vertexShader{};
  core::ShaderRef pixelShader{};
  std::array<bool, core::kMaxTextures> textures{};
  std::array<core::TextureType, core::kMaxTextures> textureTypes{};
  std::uint32_t sampleCount = 1;
  std::uint32_t clipPlaneMask = 0;
  bool unboundTextureFallback = false;
  // R-BACK-12.22..12.26 — when true, the FFP and DXBC->MSL emitters route
  // per-stage uniform reads through `ArgbufLayout` at slot 30 instead of
  // dedicated slots 0/3 and individual texture/sampler slots. The vertex
  // stream (slot 1) and `DrawVolatile` (slot 5) stay direct (design.md
  // §11.4). This is set from `ShaderVariantKey::argbufHybridMode`.
  bool argbufHybridMode = false;
  // R-BACK-12.22..12.26 (resource-array sub-mode) — when true AND
  // argbufHybridMode is also true, the FFP and DXBC->MSL fragment/vertex
  // emitters additionally route the per-stage texture/sampler resources
  // through the slot-30 `ArgbufLayout` (texture/sampler array members at
  // [[id(4..)]]) instead of the direct `[[texture(N)]]` / `[[sampler(N)]]`
  // param lane. Default OFF — when off the emitted MSL is byte-identical
  // to the constants-only Stage 2 form. Set from
  // `ShaderVariantKey::argbufResourceArray`, which makeShaderVariantKey
  // computes from the `DXMT9_ARGBUF_RESOURCE_ARRAY` opt-in env flag. This
  // bit is meaningless unless argbufHybridMode is also set.
  bool argbufResourceArray = false;
  // D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3) PSO-variant gate. When true the FFP
  // and DXBC->MSL fragment emitters declare
  // `constant SamplerLodBias& samplerLodBias [[buffer(4)]]` and thread bias()
  // through every implicit-gradient sample; when false neither the param nor
  // bias() is emitted (the MSL is byte-identical to the pre-MIPMAPLODBIAS
  // plain-sample form). Set from `ShaderVariantKey::samplerLodBias`, which is
  // computed by makeShaderVariantKey from state::anySamplerLodBiasNonzero — the
  // same predicate gates the encoder's slot-4 bind, keeping declaration and
  // binding in lockstep.
  bool samplerLodBias = false;
  // R-BACK-13.1 — tile-FFP base-colour pass gate. When true the FFP pixel
  // emitter produces the *base-colour* variant: it computes the same FFP
  // texture-stage / vertex colour but emits NEITHER the fog blend NOR the
  // alpha-test `discard_fragment()`. Those effects move to the tile kernel
  // (`makeFfpTilePixelSource`), which post-processes the rasterized base
  // colour in the imageblock. Alpha-to-coverage is likewise stripped from
  // the base PSO (the descriptor's `alpha_to_coverage_enabled` is forced
  // off on the base-colour build). Only ever set alongside a tile-FFP draw
  // selection; default off keeps the portable FFP fragment byte-identical.
  bool stripFogAlphaTestForTileBase = false;
  // Pair-local VSOut layout selected from fragment-input liveness when
  // DXMT9_TRIM_UNUSED_VARYINGS is enabled. Full layout by default.
  shaders::VSOutLayout vsOutLayout{};
};

ShaderSourceContext makeShaderSourceContext(const core::DrawShaderLayoutContext& layout,
                                            const core::FlatDrawStateRecord& hot);
// Fixture bridge for tests and offline compile requests that still start from
// a DrawDesc-shaped state fixture. Hot pipeline code uses the flat overload.
ShaderSourceContext makeShaderSourceContext(const core::fixture::DrawDesc& desc);

// Fragment texture slots that the current pixel shader can actually read.
// For programmable shaders this is the bound fragment mask; for FFP we trim
// disabled texture stages so stale D3D texture bindings are not materialized as
// Metal shader params or encoder bindings.
std::uint32_t activeFragmentTextureMaskForShader(
    const core::ShaderRef& pixelShader,
    std::uint32_t textureMask);

// Returns the VSOut layout that the current VS/FS pair must share. With
// DXMT9_TRIM_UNUSED_VARYINGS disabled this is the full legacy layout. With it
// enabled, the layout keeps only fields that the emitted fragment MSL can read
// (plus texcoord0 as the helper fallback lane).
shaders::VSOutLayout resolveVSOutLayoutForShaderPair(const ShaderSourceContext& context);

// Returns a complete MSL translation unit for either the vertex or pixel
// shader corresponding to `context`. Also writes the source to
// $DXMT_DUMP_SHADER_DIR/<label>-shader-<shaderHash>-source-<sourceHash>.metal
// if the env var is set.
std::string makeDrawShaderSource(const ShaderSourceContext& context, bool vertex);

}  // namespace dxmt9::drawshader
