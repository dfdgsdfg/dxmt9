#pragma once

// Dispatches a DrawDesc to the appropriate MSL source generator:
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
  std::uint32_t sampleCount = 1;
  std::uint32_t clipPlaneMask = 0;
};

ShaderSourceContext makeShaderSourceContext(const core::DrawShaderLayoutContext& layout,
                                            const core::FlatDrawStateRecord& hot);
ShaderSourceContext makeShaderSourceContext(const core::DrawDesc& desc);

// Returns a complete MSL translation unit for either the vertex or pixel
// shader corresponding to `context`. Also writes the source to
// $DXMT_DUMP_SHADER_DIR/<label>-<hash>.metal if the env var is set.
std::string makeDrawShaderSource(const ShaderSourceContext& context, bool vertex);

// Compatibility wrapper for callers/tests that still provide DrawDesc.
std::string makeDrawShaderSource(const core::DrawDesc& desc, bool vertex);

}  // namespace dxmt9::drawshader
