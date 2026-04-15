#include "winemetal_dispatch_internal.hpp"
#include "winemetal_thunks.hpp"
#include "wineunixlib.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

thread_local std::string gShaderSourceScratch;

}  // namespace

extern "C" dxmt9_u64 dxmt9_winemetal_default_compile_shader(const WinemetalShaderCompileRequest* request) {
  if (!request) {
    return 0;
  }

  Dxmt9WinemetalCompileShaderParams params{};
  params.kind = static_cast<uint32_t>(request->kind);
  params.bytecode_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(request->bytecode));
  params.bytecode_size = request->bytecodeSize;
  params.bytecode_hash = request->bytecodeHash;
  params.variant_key_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(request->variantKey));
  params.textured = request->textured ? 1u : 0u;
  params.clip_plane_mask = request->clipPlaneMask;
  params.sample_count = request->sampleCount;
  params.alpha_test_enable = request->alphaTestEnable;
  params.alpha_test_func = request->alphaTestFunc;
  params.alpha_ref = request->alphaRef;
  params.fog_mode = request->fogMode;

  if (WINE_UNIX_CALL(DXMT9_WINEMETAL_CALL_COMPILE_SHADER, &params) != DXMT9_STATUS_SUCCESS) {
    return 0;
  }
  return params.ret;
}

extern "C" dxmt9_u64 dxmt9_winemetal_default_shader_source_size(dxmt9_u64 shaderHandle) {
  Dxmt9WinemetalShaderSourceSizeParams params{};
  params.shader_handle = shaderHandle;
  if (WINE_UNIX_CALL(DXMT9_WINEMETAL_CALL_SHADER_SOURCE_SIZE, &params) != DXMT9_STATUS_SUCCESS) {
    return 0;
  }
  return params.ret;
}

extern "C" const char* dxmt9_winemetal_default_shader_source(dxmt9_u64 shaderHandle) {
  const dxmt9_u64 size = dxmt9_winemetal_default_shader_source_size(shaderHandle);
  if (size == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  gShaderSourceScratch.assign(static_cast<size_t>(size) + 1u, '\0');

  Dxmt9WinemetalShaderSourceCopyParams params{};
  params.shader_handle = shaderHandle;
  params.buffer_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(gShaderSourceScratch.data()));
  params.buffer_capacity = static_cast<uint64_t>(gShaderSourceScratch.size());
  if (WINE_UNIX_CALL(DXMT9_WINEMETAL_CALL_SHADER_SOURCE_COPY, &params) != DXMT9_STATUS_SUCCESS ||
      params.bytes_written == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  const size_t written = static_cast<size_t>(std::min<uint64_t>(params.bytes_written, size));
  gShaderSourceScratch.resize(written);
  return gShaderSourceScratch.c_str();
}

extern "C" void dxmt9_winemetal_default_destroy_shader(dxmt9_u64 shaderHandle) {
  Dxmt9WinemetalDestroyShaderParams params{};
  params.shader_handle = shaderHandle;
  (void)WINE_UNIX_CALL(DXMT9_WINEMETAL_CALL_DESTROY_SHADER, &params);
}
