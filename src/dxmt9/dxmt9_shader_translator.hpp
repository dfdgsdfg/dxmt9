#pragma once

// D3D9 shader bytecode → Metal Shading Language translator. Exposed as two
// stage-specific entry points; everything else (SPIR-V intermediate, opcode
// dispatch, register decoding) lives in the .cpp. Lifted out of
// backend_metal.mm so shader translation has a named module matching
// dxmt's split between dxmt_shader.cpp / dxmt_sm{1,2,3,4,5,6}.cpp.

#include "dxmt9/core.hpp"
#include "dxmt9_d3d9_bytecode.hpp"
#include "dxmt9_shader_sources.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace dxmt9::drawshader {
struct ShaderSourceContext;
}

namespace dxmt9::translator {

// Translate a D3D9 vertex shader (decl-first bytecode) into a complete
// standalone MSL source string. ShaderSourceContext supplies the small
// shader-relevant state slice needed to emit a specialised vertex variant.
std::string makeTranslatedVertexSource(const core::ShaderRef& shader,
                                       const drawshader::ShaderSourceContext& context);

// Translate a D3D9 pixel shader into MSL source. Mirrors the vertex entry.
std::string makeTranslatedFragmentSource(const core::ShaderRef& shader,
                                         const drawshader::ShaderSourceContext& context);

// Conservative liveness of VSOut fields referenced by the emitted translated
// fragment MSL for this pixel shader. Used to build pair-local VSOut layouts
// before the final VS/FS sources are generated.
shaders::VSOutLayout collectTranslatedFragmentVaryingLiveness(
    const core::ShaderRef& shader,
    const drawshader::ShaderSourceContext& context);

namespace test {

// Stable native-test seam for D3DBC decoding/classification without requiring
// Metal, Wine, GPU execution, or source-string assertions.
d3d9bc::SpirvModule decodeD3DBytecodeForTest(const core::ShaderRef& shader,
                                             bool vertex,
                                             const core::fixture::DrawDesc& desc);
d3d9bc::D3DRegisterRef decodeRegisterRefForTest(std::uint32_t token,
                                                d3d9bc::D3DShaderStage stage);
std::array<std::uint8_t, 4> decodeSwizzleForTest(std::uint32_t token);
std::uint32_t decodeSourceModifierForTest(std::uint32_t token);
std::uint32_t decodeDestModifierForTest(std::uint32_t token);
std::uint32_t decodeWriteMaskForTest(std::uint32_t token);
bool tokenHasRelativeAddressingForTest(std::uint32_t token);
std::string formatFloatLiteralBitsForTest(std::uint32_t bits);

}  // namespace test

}  // namespace dxmt9::translator
