// makeShaderSourceFromRequest: translate a winemetal shader compile request
// into MSL source. Moved out of backend_metal.mm in Step 3e as the final
// content there after MetalBackendDevice was dissolved into DeviceImpl.

#include "dxmt9/core.hpp"
#include "dxmt9/winemetal.h"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_ffp_shaders.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace dxmt9::core {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

using dxmt9::drawshader::makeDrawShaderSource;

std::string makeShaderSourceFromRequestInternal(const WinemetalShaderCompileRequest& request) {
  DrawDesc desc;
  desc.rts.color[0].sampleCount = std::max<u32>(1u, request.sampleCount);
  desc.clipPlaneMask = request.clipPlaneMask;
  desc.textures[0].handle = request.textured ? Handle{1} : Handle{};
  if (request.alphaTestEnable != 0) {
    desc.rs.values[RS_ALPHA_TEST_ENABLE] = request.alphaTestEnable;
    desc.rs.values[RS_ALPHA_FUNC] = request.alphaTestFunc;
    desc.rs.values[RS_ALPHA_REF] = static_cast<u32>(std::clamp(request.alphaRef, 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  if (request.fogMode != static_cast<u32>(FogMode::None)) {
    desc.rs.values[RS_FOG_TABLE_MODE] = request.fogMode;
  }

  std::vector<u8> bytecode;
  if (request.bytecode && request.bytecodeSize > 0) {
    const auto* bytes = static_cast<const u8*>(request.bytecode);
    bytecode.assign(bytes, bytes + request.bytecodeSize);
  }

  switch (request.kind) {
    case WinemetalShaderKind_D3DBytecodeVertex:
      desc.vertexShader.kind = ShaderRef::Kind::Bytecode;
      desc.vertexShader.bytecode.bytes = std::move(bytecode);
      desc.vertexShader.bytecode.hash = request.bytecodeHash;
      return makeDrawShaderSource(desc, true);
    case WinemetalShaderKind_D3DBytecodePixel:
      desc.pixelShader.kind = ShaderRef::Kind::Bytecode;
      desc.pixelShader.bytecode.bytes = std::move(bytecode);
      desc.pixelShader.bytecode.hash = request.bytecodeHash;
      return makeDrawShaderSource(desc, false);
    case WinemetalShaderKind_FfpVertex:
      desc.vertexShader.kind = ShaderRef::Kind::FixedFunctionVertex;
      if (request.variantKey) {
        desc.vertexShader.vertexKey = *reinterpret_cast<const FfpVertexKey*>(request.variantKey);
      }
      return makeDrawShaderSource(desc, true);
    case WinemetalShaderKind_FfpPixel:
      desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
      if (request.variantKey) {
        desc.pixelShader.pixelKey = *reinterpret_cast<const FfpPixelKey*>(request.variantKey);
      }
      return makeDrawShaderSource(desc, false);
  }
  return {};
}

}  // namespace

std::string makeShaderSourceFromRequest(const WinemetalShaderCompileRequest& request) {
  return makeShaderSourceFromRequestInternal(request);
}

}  // namespace dxmt9::core
