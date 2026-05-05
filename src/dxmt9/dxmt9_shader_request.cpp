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

using dxmt9::drawshader::ShaderSourceContext;
using dxmt9::drawshader::makeDrawShaderSource;

std::string makeShaderSourceFromRequestInternal(const WinemetalShaderCompileRequest& request) {
  ShaderSourceContext context{};
  context.sampleCount = std::max<u32>(1u, request.sampleCount);
  context.clipPlaneMask = request.clipPlaneMask;
  context.textures[0] = request.textured != 0;

  std::vector<u8> bytecode;
  if (request.bytecode && request.bytecodeSize > 0) {
    const auto* bytes = static_cast<const u8*>(request.bytecode);
    bytecode.assign(bytes, bytes + request.bytecodeSize);
  }

  switch (request.kind) {
    case WinemetalShaderKind_D3DBytecodeVertex:
      context.vertexShader.kind = ShaderRef::Kind::Bytecode;
      context.vertexShader.bytecode.bytes = std::move(bytecode);
      context.vertexShader.bytecode.hash = request.bytecodeHash;
      return makeDrawShaderSource(context, true);
    case WinemetalShaderKind_D3DBytecodePixel:
      context.pixelShader.kind = ShaderRef::Kind::Bytecode;
      context.pixelShader.bytecode.bytes = std::move(bytecode);
      context.pixelShader.bytecode.hash = request.bytecodeHash;
      return makeDrawShaderSource(context, false);
    case WinemetalShaderKind_FfpVertex:
      context.vertexShader.kind = ShaderRef::Kind::FixedFunctionVertex;
      if (request.variantKey) {
        context.vertexShader.vertexKey = *reinterpret_cast<const FfpVertexKey*>(request.variantKey);
      }
      return makeDrawShaderSource(context, true);
    case WinemetalShaderKind_FfpPixel:
      context.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
      if (request.variantKey) {
        context.pixelShader.pixelKey = *reinterpret_cast<const FfpPixelKey*>(request.variantKey);
      }
      return makeDrawShaderSource(context, false);
  }
  return {};
}

}  // namespace

std::string makeShaderSourceFromRequest(const WinemetalShaderCompileRequest& request) {
  return makeShaderSourceFromRequestInternal(request);
}

}  // namespace dxmt9::core
