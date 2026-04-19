#include "winemetal_thunks.hpp"
#include "util/util_buffer.hpp"
#include "dxmt9/wineunixlib.h"
#include "util/log/log.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

thread_local std::string gShaderSourceScratch;
extern "C" NTSTATUS dxmt9_bridge_unix_call(unsigned int code, void* args);

NTSTATUS providerUnixCall(unsigned int code, void* args) {
  dxmt9::util::logf(dxmt9::util::LogLevel::Debug, "winemetal-thunks",
                    "unix_call code=%u args=%p", code, args);
  return dxmt9_bridge_unix_call(code, args);
}

dxmt9_u64 compileShaderViaUnixCall(const WinemetalShaderCompileRequest* request) {
  if (!request) {
    return 0;
  }

  Dxmt9WinemetalCompileShaderParams params{};
  params.kind = static_cast<uint32_t>(request->kind);
  params.bytecode_ptr = dxmt9::util::ptrToU64(request->bytecode);
  params.bytecode_size = request->bytecodeSize;
  params.bytecode_hash = request->bytecodeHash;
  params.variant_key_ptr = dxmt9::util::ptrToU64(request->variantKey);
  params.textured = request->textured ? 1u : 0u;
  params.clip_plane_mask = request->clipPlaneMask;
  params.sample_count = request->sampleCount;
  params.alpha_test_enable = request->alphaTestEnable;
  params.alpha_test_func = request->alphaTestFunc;
  params.alpha_ref = request->alphaRef;
  params.fog_mode = request->fogMode;

  if (providerUnixCall(DXMT9_WINEMETAL_CALL_COMPILE_SHADER, &params) != DXMT9_STATUS_SUCCESS) {
    return 0;
  }
  return params.ret;
}

dxmt9_u64 shaderSourceSizeViaUnixCall(dxmt9_u64 shaderHandle) {
  Dxmt9WinemetalShaderSourceSizeParams params{};
  params.shader_handle = shaderHandle;
  if (providerUnixCall(DXMT9_WINEMETAL_CALL_SHADER_SOURCE_SIZE, &params) != DXMT9_STATUS_SUCCESS) {
    return 0;
  }
  return params.ret;
}

const char* shaderSourceViaUnixCall(dxmt9_u64 shaderHandle) {
  const dxmt9_u64 size = shaderSourceSizeViaUnixCall(shaderHandle);
  if (size == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  gShaderSourceScratch.assign(static_cast<size_t>(size) + 1u, '\0');

  Dxmt9WinemetalShaderSourceCopyParams params{};
  params.shader_handle = shaderHandle;
  params.buffer_ptr = dxmt9::util::ptrToU64(gShaderSourceScratch.data());
  params.buffer_capacity = static_cast<uint64_t>(gShaderSourceScratch.size());
  if (providerUnixCall(DXMT9_WINEMETAL_CALL_SHADER_SOURCE_COPY, &params) != DXMT9_STATUS_SUCCESS ||
      params.bytes_written == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  const size_t written = static_cast<size_t>(std::min<uint64_t>(params.bytes_written, size));
  gShaderSourceScratch.resize(written);
  return gShaderSourceScratch.c_str();
}

void destroyShaderViaUnixCall(dxmt9_u64 shaderHandle) {
  Dxmt9WinemetalDestroyShaderParams params{};
  params.shader_handle = shaderHandle;
  (void)providerUnixCall(DXMT9_WINEMETAL_CALL_DESTROY_SHADER, &params);
}

}  // namespace

extern "C" dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request) {
  return compileShaderViaUnixCall(request);
}

extern "C" const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle) {
  return shaderSourceViaUnixCall(shaderHandle);
}

extern "C" dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle) {
  return shaderSourceSizeViaUnixCall(shaderHandle);
}

extern "C" void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle) {
  destroyShaderViaUnixCall(shaderHandle);
}
