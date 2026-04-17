#include "winemetal_shader_bridge_internal.hpp"

#include "util/util_buffer.hpp"
#include "winemetal_service_abi.hpp"

#include <string>

namespace dxmt9::winemetal {

namespace {

thread_local std::string gShaderSourceScratch;

}  // namespace

WinemetalShaderCompileRequest decodeCompileShaderRequest(const Dxmt9WinemetalCompileShaderParams& params) {
  WinemetalShaderCompileRequest request{};
  request.kind = static_cast<WinemetalShaderKind>(params.kind);
  request.bytecode = dxmt9::util::u64ToPtr<const void>(params.bytecode_ptr);
  request.bytecodeSize = params.bytecode_size;
  request.bytecodeHash = params.bytecode_hash;
  request.variantKey = dxmt9::util::u64ToPtr<const void>(params.variant_key_ptr);
  request.textured = params.textured != 0;
  request.clipPlaneMask = params.clip_plane_mask;
  request.sampleCount = params.sample_count;
  request.alphaTestEnable = params.alpha_test_enable;
  request.alphaTestFunc = params.alpha_test_func;
  request.alphaRef = params.alpha_ref;
  request.fogMode = params.fog_mode;
  return request;
}

dxmt9_u64 compileShader(const WinemetalShaderCompileRequest* request) {
  if (!request) {
    return 0;
  }
  return dxmt9_winemetal_service_compile_shader(request);
}

const char* shaderSource(dxmt9_u64 shaderHandle) {
  const dxmt9_u64 sourceSize = dxmt9_winemetal_service_shader_source_size(shaderHandle);
  if (sourceSize == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  gShaderSourceScratch.resize(static_cast<size_t>(sourceSize) + 1u);
  const dxmt9_u64 bytesWritten = dxmt9_winemetal_service_shader_source_copy(
      shaderHandle, gShaderSourceScratch.data(), static_cast<dxmt9_u64>(gShaderSourceScratch.size()));
  if (bytesWritten == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  gShaderSourceScratch.resize(static_cast<size_t>(bytesWritten));
  return gShaderSourceScratch.c_str();
}

dxmt9_u64 shaderSourceSize(dxmt9_u64 shaderHandle) {
  return dxmt9_winemetal_service_shader_source_size(shaderHandle);
}

dxmt9_u64 copyShaderSource(dxmt9_u64 shaderHandle, char* buffer, dxmt9_u64 bufferCapacity) {
  if (!buffer || bufferCapacity == 0) {
    return 0;
  }
  const char* source = shaderSource(shaderHandle);
  if (!source) {
    buffer[0] = '\0';
    return 0;
  }
  return dxmt9::util::copyStringToBuffer(source, buffer, bufferCapacity);
}

void destroyShader(dxmt9_u64 shaderHandle) {
  dxmt9_winemetal_service_destroy_shader(shaderHandle);
}

}  // namespace dxmt9::winemetal
