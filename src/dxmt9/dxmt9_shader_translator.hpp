#pragma once

// D3D9 shader bytecode → Metal Shading Language translator. Exposed as two
// stage-specific entry points; everything else (SPIR-V intermediate, opcode
// dispatch, register decoding) lives in the .cpp. Lifted out of
// backend_metal.mm so shader translation has a named module matching
// dxmt's split between dxmt_shader.cpp / dxmt_sm{1,2,3,4,5,6}.cpp.

#include "dxmt9/core.hpp"
#include "dxmt9_d3d9_bytecode.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace dxmt9::translator {

// Translate a D3D9 vertex shader (decl-first bytecode) into a complete
// standalone MSL source string. The DrawDesc supplies state context
// (clip planes, texture declarations, etc.) needed to emit a specialised
// vertex shader variant.
std::string makeTranslatedVertexSource(const core::ShaderRef& shader, const core::DrawDesc& desc);

// Translate a D3D9 pixel shader into MSL source. Mirrors the vertex entry.
std::string makeTranslatedFragmentSource(const core::ShaderRef& shader, const core::DrawDesc& desc);

namespace test {

// Stable native-test seam for D3DBC decoding/classification without requiring
// Metal, Wine, GPU execution, or source-string assertions.
d3d9bc::SpirvModule decodeD3DBytecodeForTest(const core::ShaderRef& shader,
                                             bool vertex,
                                             const core::DrawDesc& desc);
d3d9bc::D3DRegisterRef decodeRegisterRefForTest(std::uint32_t token,
                                                d3d9bc::D3DShaderStage stage);
std::array<std::uint8_t, 4> decodeSwizzleForTest(std::uint32_t token);
std::uint32_t decodeSourceModifierForTest(std::uint32_t token);
std::uint32_t decodeDestModifierForTest(std::uint32_t token);
std::uint32_t decodeWriteMaskForTest(std::uint32_t token);
bool tokenHasRelativeAddressingForTest(std::uint32_t token);

}  // namespace test

}  // namespace dxmt9::translator
