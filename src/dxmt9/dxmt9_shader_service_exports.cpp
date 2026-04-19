#include "dxmt9_shader_service.hpp"
#include "dxmt9unix_service_abi.h"
#include "util/util_buffer.hpp"

extern "C" dxmt9_u64
dxmt9unix_service_compile_shader(const WinemetalShaderCompileRequest* request) {
  if (!request) {
    return 0;
  }
  return dxmt9::core::shader_service::compile(*request);
}

extern "C" dxmt9_u64 dxmt9unix_service_shader_source_size(dxmt9_u64 shaderHandle) {
  return dxmt9::core::shader_service::sourceSize(shaderHandle);
}

extern "C" dxmt9_u64
dxmt9unix_service_shader_source_copy(dxmt9_u64 shaderHandle,
                                     char* buffer,
                                     dxmt9_u64 bufferCapacity) {
  if (!buffer || bufferCapacity == 0) {
    return 0;
  }
  const std::string source = dxmt9::core::shader_service::source(shaderHandle);
  if (source.empty()) {
    buffer[0] = '\0';
    return 0;
  }
  return dxmt9::util::copyStringToBuffer(source, buffer, bufferCapacity);
}

extern "C" void dxmt9unix_service_destroy_shader(dxmt9_u64 shaderHandle) {
  dxmt9::core::shader_service::destroy(shaderHandle);
}
