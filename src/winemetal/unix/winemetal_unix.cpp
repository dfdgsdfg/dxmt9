#include "../winemetal_thunks.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "dxmt9_shader_service.hpp"

#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#endif

#include "util/util_buffer.hpp"

namespace {

thread_local std::string gShaderSourceScratch;

#if defined(WINE_UNIX_LIB)
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
#endif

}  // namespace

dxmt9_u64 compileShaderViaService(const WinemetalShaderCompileRequest* request) {
  if (!request) {
    return 0;
  }
  return dxmt9_shader_service_compile(request);
}

const char* shaderSourceViaService(dxmt9_u64 shaderHandle) {
  const dxmt9_u64 sourceSize = dxmt9_shader_service_source_size(shaderHandle);
  if (sourceSize == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }
  gShaderSourceScratch.resize(static_cast<size_t>(sourceSize) + 1u);
  const dxmt9_u64 bytesWritten = dxmt9_shader_service_source_copy(
      shaderHandle, gShaderSourceScratch.data(), static_cast<dxmt9_u64>(gShaderSourceScratch.size()));
  if (bytesWritten == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }
  gShaderSourceScratch.resize(static_cast<size_t>(bytesWritten));
  return gShaderSourceScratch.c_str();
}

dxmt9_u64 shaderSourceSizeViaService(dxmt9_u64 shaderHandle) {
  return dxmt9_shader_service_source_size(shaderHandle);
}

void destroyShaderViaService(dxmt9_u64 shaderHandle) {
  dxmt9_shader_service_destroy(shaderHandle);
}

extern "C" dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request) {
  return compileShaderViaService(request);
}

extern "C" const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle) {
  return shaderSourceViaService(shaderHandle);
}

extern "C" dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle) {
  return shaderSourceSizeViaService(shaderHandle);
}

extern "C" void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle) {
  destroyShaderViaService(shaderHandle);
}

#if defined(WINE_UNIX_LIB)

namespace {

NTSTATUS compileShaderCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalCompileShaderParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  const auto request = decodeRequest(*params);
  params->ret = compileShaderViaService(&request);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS shaderSourceSizeCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceSizeParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  params->ret = shaderSourceSizeViaService(params->shader_handle);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS shaderSourceCopyCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceCopyParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  const char* source = shaderSourceViaService(params->shader_handle);
  if (!source || params->buffer_capacity == 0 || params->buffer_ptr == 0) {
    params->bytes_written = 0;
    return DXMT9_STATUS_SUCCESS;
  }

  char* destination = reinterpret_cast<char*>(static_cast<uintptr_t>(params->buffer_ptr));
  params->bytes_written = dxmt9::util::copyStringToBuffer(source, destination, params->buffer_capacity);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS destroyShaderCall(void* args) {
  auto* params = static_cast<Dxmt9WinemetalDestroyShaderParams*>(args);
  if (!params) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  destroyShaderViaService(params->shader_handle);
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
