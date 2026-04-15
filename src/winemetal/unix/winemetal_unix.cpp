#include "winemetal_dispatch_internal.hpp"
#include "../winemetal_thunks.hpp"

#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#else
#include "dxmt9_shader_service.hpp"
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

#if !defined(WINE_UNIX_LIB)
thread_local std::string gShaderSourceScratch;
#endif

WinemetalShaderCompileRequest decodeRequest(const Dxmt9WinemetalCompileShaderParams& params) {
  WinemetalShaderCompileRequest request{};
  request.kind = static_cast<WinemetalShaderKind>(params.kind);
  request.bytecode = reinterpret_cast<const void*>(static_cast<uintptr_t>(params.bytecode_ptr));
  request.bytecodeSize = params.bytecode_size;
  request.bytecodeHash = params.bytecode_hash;
  request.variantKey = reinterpret_cast<const void*>(static_cast<uintptr_t>(params.variant_key_ptr));
  request.textured = params.textured != 0;
  request.clipPlaneMask = params.clip_plane_mask;
  request.sampleCount = params.sample_count;
  request.alphaTestEnable = params.alpha_test_enable;
  request.alphaTestFunc = params.alpha_test_func;
  request.alphaRef = params.alpha_ref;
  request.fogMode = params.fog_mode;
  return request;
}

}  // namespace

extern "C" dxmt9_u64 dxmt9_winemetal_default_compile_shader(const WinemetalShaderCompileRequest* request) {
#if defined(WINE_UNIX_LIB)
  (void)request;
  return 0;
#else
  if (!request) {
    return 0;
  }
  return dxmt9::core::shader_service::compile(*request);
#endif
}

extern "C" const char* dxmt9_winemetal_default_shader_source(dxmt9_u64 shaderHandle) {
#if defined(WINE_UNIX_LIB)
  (void)shaderHandle;
  return nullptr;
#else
  gShaderSourceScratch = dxmt9::core::shader_service::source(shaderHandle);
  return gShaderSourceScratch.empty() ? nullptr : gShaderSourceScratch.c_str();
#endif
}

extern "C" dxmt9_u64 dxmt9_winemetal_default_shader_source_size(dxmt9_u64 shaderHandle) {
#if defined(WINE_UNIX_LIB)
  (void)shaderHandle;
  return 0;
#else
  return dxmt9::core::shader_service::sourceSize(shaderHandle);
#endif
}

extern "C" void dxmt9_winemetal_default_destroy_shader(dxmt9_u64 shaderHandle) {
#if defined(WINE_UNIX_LIB)
  (void)shaderHandle;
#else
  dxmt9::core::shader_service::destroy(shaderHandle);
#endif
}

#if defined(WINE_UNIX_LIB)

namespace {

NTSTATUS compileShaderCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalCompileShaderParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  const auto request = decodeRequest(*params);
  params->ret = dxmt9_winemetal_default_compile_shader(&request);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS shaderSourceSizeCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceSizeParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  params->ret = dxmt9_winemetal_default_shader_source_size(params->shader_handle);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS shaderSourceCopyCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceCopyParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  const char* source = dxmt9_winemetal_default_shader_source(params->shader_handle);
  if (!source || params->buffer_capacity == 0 || params->buffer_ptr == 0) {
    params->bytes_written = 0;
    return DXMT9_STATUS_SUCCESS;
  }

  char* destination = reinterpret_cast<char*>(static_cast<uintptr_t>(params->buffer_ptr));
  const size_t sourceSize = std::strlen(source);
  const size_t bytesToCopy = std::min<size_t>(sourceSize, static_cast<size_t>(params->buffer_capacity - 1u));
  std::memcpy(destination, source, bytesToCopy);
  destination[bytesToCopy] = '\0';
  params->bytes_written = static_cast<uint64_t>(bytesToCopy);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS destroyShaderCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalDestroyShaderParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  dxmt9_winemetal_default_destroy_shader(params->shader_handle);
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
