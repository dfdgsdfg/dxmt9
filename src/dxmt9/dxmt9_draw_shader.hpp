#pragma once

// Dispatches a DrawDesc to the appropriate MSL source generator:
//   - Translated D3D bytecode → dxmt9::translator
//   - Fixed-function D3D9 pipeline → dxmt9::ffp
//   - Minimal passthrough (no user shader) → dxmt9::shaders
//
// Extracted from backend_metal.mm. Callers are the draw pipeline builder
// in dxmt9::pipeline::Cache + the WinemetalShaderCompileRequest service.

#include "dxmt9/core.hpp"

#include <string>

namespace dxmt9::drawshader {

// Returns a complete MSL translation unit for either the vertex or pixel
// shader corresponding to `desc`. Also writes the source to
// $DXMT_DUMP_SHADER_DIR/<label>-<hash>.metal if the env var is set.
std::string makeDrawShaderSource(const core::DrawDesc& desc, bool vertex);

}  // namespace dxmt9::drawshader
