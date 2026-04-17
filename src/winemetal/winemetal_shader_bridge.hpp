#pragma once

#include "dxmt9/winemetal.h"
#include "winemetal_thunks.hpp"

#include <cstdint>

dxmt9_u64 dxmt9_winemetal_bridge_compile_shader(const WinemetalShaderCompileRequest* request);
const char* dxmt9_winemetal_bridge_shader_source(dxmt9_u64 shaderHandle);
dxmt9_u64 dxmt9_winemetal_bridge_shader_source_size(dxmt9_u64 shaderHandle);
dxmt9_u64 dxmt9_winemetal_bridge_copy_shader_source(dxmt9_u64 shaderHandle,
                                                    char* buffer,
                                                    dxmt9_u64 bufferCapacity);
void dxmt9_winemetal_bridge_destroy_shader(dxmt9_u64 shaderHandle);

std::int32_t dxmt9_winemetal_bridge_compile_shader_params(Dxmt9WinemetalCompileShaderParams* params);
std::int32_t dxmt9_winemetal_bridge_shader_source_size_params(Dxmt9WinemetalShaderSourceSizeParams* params);
std::int32_t dxmt9_winemetal_bridge_shader_source_copy_params(Dxmt9WinemetalShaderSourceCopyParams* params);
std::int32_t dxmt9_winemetal_bridge_destroy_shader_params(Dxmt9WinemetalDestroyShaderParams* params);
