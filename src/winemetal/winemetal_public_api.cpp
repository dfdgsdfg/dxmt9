#include "winemetal_shader_bridge_internal.hpp"

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
