#pragma once

#include "dxmt9/winemetal.h"
#include "winemetal/winemetal_thunks.hpp"

#ifdef __cplusplus
extern "C" {
#endif

dxmt9_u64 dxmt9unix_service_compile_shader(const WinemetalShaderCompileRequest* request);
dxmt9_u64 dxmt9unix_service_compile_shader_marshaled(const Dxmt9WinemetalCompileShaderParams* params);
dxmt9_u64 dxmt9unix_service_shader_source_size(dxmt9_u64 shaderHandle);
dxmt9_u64 dxmt9unix_service_shader_source_copy(dxmt9_u64 shaderHandle,
                                               char* buffer,
                                               dxmt9_u64 bufferCapacity);
dxmt9_u64 dxmt9unix_service_shader_source_copy_marshaled(const Dxmt9WinemetalShaderSourceCopyParams* params);
void dxmt9unix_service_destroy_shader(dxmt9_u64 shaderHandle);
void dxmt9unix_service_destroy_shader_marshaled(const Dxmt9WinemetalDestroyShaderParams* params);

#ifdef __cplusplus
}
#endif
