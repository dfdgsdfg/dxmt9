#pragma once

#include "dxmt9/winemetal.h"

#ifdef __cplusplus
extern "C" {
#endif

dxmt9_u64 dxmt9_provider_service_compile_shader(const WinemetalShaderCompileRequest* request);
dxmt9_u64 dxmt9_provider_service_shader_source_size(dxmt9_u64 shaderHandle);
dxmt9_u64 dxmt9_provider_service_shader_source_copy(dxmt9_u64 shaderHandle,
                                                    char* buffer,
                                                    dxmt9_u64 bufferCapacity);
void dxmt9_provider_service_destroy_shader(dxmt9_u64 shaderHandle);

#ifdef __cplusplus
}
#endif
