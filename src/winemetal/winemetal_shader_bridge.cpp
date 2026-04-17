#include "winemetal_shader_bridge.hpp"

#include "util/util_buffer.hpp"
#include "winemetal_service_abi.hpp"

#include <string>

namespace dxmt9::winemetal {

namespace {

thread_local std::string gShaderSourceScratch;

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

extern "C" dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request) {
  return dxmt9::winemetal::compileShader(request);
}

extern "C" const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle) {
  return dxmt9::winemetal::shaderSource(shaderHandle);
}

extern "C" dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle) {
  return dxmt9::winemetal::shaderSourceSize(shaderHandle);
}

extern "C" void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle) {
  dxmt9::winemetal::destroyShader(shaderHandle);
}
