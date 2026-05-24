#pragma once

// Dispatches a ShaderSourceContext to the appropriate MSL source generator:
//   - Translated D3D bytecode → dxmt9::translator
//   - Fixed-function D3D9 pipeline → dxmt9::ffp
//   - Minimal passthrough (no user shader) → dxmt9::shaders
//
// Extracted from backend_metal.mm. Callers are the draw pipeline builder
// in dxmt9::pipeline::Cache + the WinemetalShaderCompileRequest service.

#include "dxmt9/core.hpp"

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
};

ShaderSourceContext makeShaderSourceContext(const core::DrawShaderLayoutContext& layout,
                                            const core::FlatDrawStateRecord& hot);
// Fixture bridge for tests and offline compile requests that still start from
// a DrawDesc-shaped state fixture. Hot pipeline code uses the flat overload.
ShaderSourceContext makeShaderSourceContext(const core::fixture::DrawDesc& desc);

// Returns a complete MSL translation unit for either the vertex or pixel
// shader corresponding to `context`. Also writes the source to
// $DXMT_DUMP_SHADER_DIR/<label>-<hash>.metal if the env var is set.
std::string makeDrawShaderSource(const ShaderSourceContext& context, bool vertex);

}  // namespace dxmt9::drawshader
