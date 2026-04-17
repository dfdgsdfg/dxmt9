#pragma once

#include "winemetal_thunks.hpp"

#include <cstdint>

extern "C" std::int32_t
dxmt9_winemetal_bridge_compile_shader_params(Dxmt9WinemetalCompileShaderParams* params);
extern "C" std::int32_t
dxmt9_winemetal_bridge_shader_source_size_params(Dxmt9WinemetalShaderSourceSizeParams* params);
extern "C" std::int32_t
dxmt9_winemetal_bridge_shader_source_copy_params(Dxmt9WinemetalShaderSourceCopyParams* params);
extern "C" std::int32_t
dxmt9_winemetal_bridge_destroy_shader_params(Dxmt9WinemetalDestroyShaderParams* params);
