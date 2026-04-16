#pragma once

#include "dxmt9/winemetal.h"

#include <cstdint>
#include <string>

namespace dxmt9::core::shader_service {

using u64 = std::uint64_t;

u64 compile(const WinemetalShaderCompileRequest& request);
std::string source(u64 shaderHandle);
u64 sourceSize(u64 shaderHandle);
void destroy(u64 shaderHandle);

}  // namespace dxmt9::core::shader_service

namespace dxmt9::core {

std::string makeShaderSourceFromRequest(const WinemetalShaderCompileRequest& request);

}  // namespace dxmt9::core

#ifndef DXMT9_SHADER_SERVICE_API
#define DXMT9_SHADER_SERVICE_API
#endif

extern "C" {

DXMT9_SHADER_SERVICE_API dxmt9_u64 dxmt9_shader_service_compile(const WinemetalShaderCompileRequest* request);
DXMT9_SHADER_SERVICE_API dxmt9_u64 dxmt9_shader_service_source_size(dxmt9_u64 shaderHandle);
DXMT9_SHADER_SERVICE_API dxmt9_u64 dxmt9_shader_service_source_copy(dxmt9_u64 shaderHandle,
                                                                    char* buffer,
                                                                    dxmt9_u64 bufferCapacity);
DXMT9_SHADER_SERVICE_API void dxmt9_shader_service_destroy(dxmt9_u64 shaderHandle);

}
