#include "dxmt9_provider_service_abi.h"

#include <string>

namespace {

thread_local std::string gShaderSourceScratch;

const char* shaderSourceLocal(dxmt9_u64 shaderHandle) {
  const dxmt9_u64 size = dxmt9_provider_service_shader_source_size(shaderHandle);
  if (size == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  gShaderSourceScratch.assign(static_cast<size_t>(size) + 1u, '\0');
  const dxmt9_u64 bytesWritten = dxmt9_provider_service_shader_source_copy(
      shaderHandle,
      gShaderSourceScratch.data(),
      static_cast<dxmt9_u64>(gShaderSourceScratch.size()));
  if (bytesWritten == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  gShaderSourceScratch.resize(static_cast<size_t>(bytesWritten));
  return gShaderSourceScratch.c_str();
}

}  // namespace

extern "C" dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request) {
  return dxmt9_provider_service_compile_shader(request);
}

extern "C" const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle) {
  return shaderSourceLocal(shaderHandle);
}

extern "C" dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle) {
  return dxmt9_provider_service_shader_source_size(shaderHandle);
}

extern "C" void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle) {
  dxmt9_provider_service_destroy_shader(shaderHandle);
}
