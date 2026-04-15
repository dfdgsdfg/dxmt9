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
