#pragma once

#include "dxmt9/winemetal.h"
#include "winemetal_thunks.hpp"

namespace dxmt9::winemetal {

WinemetalShaderCompileRequest decodeCompileShaderRequest(const Dxmt9WinemetalCompileShaderParams& params);
dxmt9_u64 compileShader(const WinemetalShaderCompileRequest* request);
const char* shaderSource(dxmt9_u64 shaderHandle);
dxmt9_u64 shaderSourceSize(dxmt9_u64 shaderHandle);
dxmt9_u64 copyShaderSource(dxmt9_u64 shaderHandle, char* buffer, dxmt9_u64 bufferCapacity);
void destroyShader(dxmt9_u64 shaderHandle);

}  // namespace dxmt9::winemetal
