#include "../winemetal_thunks.hpp"
#include "../winemetal_shader_bridge.hpp"
#include "util/util_buffer.hpp"

#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#endif

namespace {

#if defined(WINE_UNIX_LIB)
WinemetalShaderCompileRequest decodeRequest(const Dxmt9WinemetalCompileShaderParams& params) {
  WinemetalShaderCompileRequest request{};
  request.kind = static_cast<WinemetalShaderKind>(params.kind);
  request.bytecode = dxmt9::util::u64ToPtr<const void>(params.bytecode_ptr);
  request.bytecodeSize = params.bytecode_size;
  request.bytecodeHash = params.bytecode_hash;
  request.variantKey = dxmt9::util::u64ToPtr<const void>(params.variant_key_ptr);
  request.textured = params.textured != 0;
  request.clipPlaneMask = params.clip_plane_mask;
  request.sampleCount = params.sample_count;
  request.alphaTestEnable = params.alpha_test_enable;
  request.alphaTestFunc = params.alpha_test_func;
  request.alphaRef = params.alpha_ref;
  request.fogMode = params.fog_mode;
  return request;
}
#endif

}  // namespace

#if defined(WINE_UNIX_LIB)

namespace {

NTSTATUS compileShaderCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalCompileShaderParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  const auto request = decodeRequest(*params);
  params->ret = dxmt9::winemetal::compileShader(&request);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS shaderSourceSizeCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceSizeParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  params->ret = dxmt9::winemetal::shaderSourceSize(params->shader_handle);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS shaderSourceCopyCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceCopyParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  if (params->buffer_capacity == 0 || params->buffer_ptr == 0) {
    params->bytes_written = 0;
    return DXMT9_STATUS_SUCCESS;
  }

  char* destination = dxmt9::util::u64ToPtr<char>(params->buffer_ptr);
  params->bytes_written = dxmt9::winemetal::copyShaderSource(
      params->shader_handle, destination, params->buffer_capacity);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS destroyShaderCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalDestroyShaderParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  dxmt9::winemetal::destroyShader(params->shader_handle);
  return DXMT9_STATUS_SUCCESS;
}

}  // namespace

extern "C" DECLSPEC_EXPORT NTSTATUS __wine_unix_lib_init(void) {
  return DXMT9_STATUS_SUCCESS;
}

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {
    compileShaderCall,
    shaderSourceSizeCall,
    shaderSourceCopyCall,
    destroyShaderCall,
};

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    compileShaderCall,
    shaderSourceSizeCall,
    shaderSourceCopyCall,
    destroyShaderCall,
};

#endif
