#include "winemetal_bridge_service_abi.h"

extern "C" dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request) {
  return dxmt9_winemetal_bridge_compile_shader(request);
}

extern "C" const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle) {
  return dxmt9_winemetal_bridge_shader_source(shaderHandle);
}

extern "C" dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle) {
  return dxmt9_winemetal_bridge_shader_source_size(shaderHandle);
}

extern "C" void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle) {
  dxmt9_winemetal_bridge_destroy_shader(shaderHandle);
}
