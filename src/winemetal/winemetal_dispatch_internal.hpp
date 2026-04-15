#pragma once

#include "dxmt9/winemetal.h"

#ifdef __cplusplus
extern "C" {
#endif

dxmt9_u64 dxmt9_winemetal_default_compile_shader(const WinemetalShaderCompileRequest* request);
const char* dxmt9_winemetal_default_shader_source(dxmt9_u64 shaderHandle);
dxmt9_u64 dxmt9_winemetal_default_shader_source_size(dxmt9_u64 shaderHandle);
void dxmt9_winemetal_default_destroy_shader(dxmt9_u64 shaderHandle);

#ifdef __cplusplus
}
#endif
