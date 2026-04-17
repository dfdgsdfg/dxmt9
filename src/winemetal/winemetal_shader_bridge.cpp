#include "winemetal_shader_bridge.hpp"

#include "util/util_buffer.hpp"
#include "winemetal_service_abi.hpp"

#include <algorithm>
#include <string>

namespace {

thread_local std::string gShaderSourceScratch;
constexpr std::int32_t kStatusSuccess = 0;
constexpr std::int32_t kStatusInvalidParameter = static_cast<std::int32_t>(0xC000000Du);

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

}  // namespace

dxmt9_u64 dxmt9_winemetal_bridge_compile_shader(const WinemetalShaderCompileRequest* request) {
  if (!request) {
    return 0;
  }
  return dxmt9_winemetal_service_compile_shader(request);
}

const char* dxmt9_winemetal_bridge_shader_source(dxmt9_u64 shaderHandle) {
  const dxmt9_u64 sourceSize = dxmt9_winemetal_service_shader_source_size(shaderHandle);
  if (sourceSize == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  gShaderSourceScratch.resize(static_cast<size_t>(sourceSize) + 1u);
  const dxmt9_u64 bytesWritten = dxmt9_winemetal_service_shader_source_copy(
      shaderHandle, gShaderSourceScratch.data(), static_cast<dxmt9_u64>(gShaderSourceScratch.size()));
  if (bytesWritten == 0) {
    gShaderSourceScratch.clear();
    return nullptr;
  }

  gShaderSourceScratch.resize(static_cast<size_t>(bytesWritten));
  return gShaderSourceScratch.c_str();
}

dxmt9_u64 dxmt9_winemetal_bridge_shader_source_size(dxmt9_u64 shaderHandle) {
  return dxmt9_winemetal_service_shader_source_size(shaderHandle);
}

dxmt9_u64 dxmt9_winemetal_bridge_copy_shader_source(dxmt9_u64 shaderHandle,
                                                    char* buffer,
                                                    dxmt9_u64 bufferCapacity) {
  if (!buffer || bufferCapacity == 0) {
    return 0;
  }
  const char* source = dxmt9_winemetal_bridge_shader_source(shaderHandle);
  if (!source) {
    buffer[0] = '\0';
    return 0;
  }
  return dxmt9::util::copyStringToBuffer(source, buffer, bufferCapacity);
}

void dxmt9_winemetal_bridge_destroy_shader(dxmt9_u64 shaderHandle) {
  dxmt9_winemetal_service_destroy_shader(shaderHandle);
}

std::int32_t dxmt9_winemetal_bridge_compile_shader_params(Dxmt9WinemetalCompileShaderParams* params) {
  if (!params) {
    return kStatusInvalidParameter;
  }
  const auto request = decodeRequest(*params);
  params->ret = dxmt9_winemetal_bridge_compile_shader(&request);
  return kStatusSuccess;
}

std::int32_t dxmt9_winemetal_bridge_shader_source_size_params(Dxmt9WinemetalShaderSourceSizeParams* params) {
  if (!params) {
    return kStatusInvalidParameter;
  }
  params->ret = dxmt9_winemetal_bridge_shader_source_size(params->shader_handle);
  return kStatusSuccess;
}

std::int32_t dxmt9_winemetal_bridge_shader_source_copy_params(Dxmt9WinemetalShaderSourceCopyParams* params) {
  if (!params) {
    return kStatusInvalidParameter;
  }
  if (params->buffer_capacity == 0 || params->buffer_ptr == 0) {
    params->bytes_written = 0;
    return kStatusSuccess;
  }
  char* destination = dxmt9::util::u64ToPtr<char>(params->buffer_ptr);
  params->bytes_written = dxmt9_winemetal_bridge_copy_shader_source(
      params->shader_handle, destination, params->buffer_capacity);
  return kStatusSuccess;
}

std::int32_t dxmt9_winemetal_bridge_destroy_shader_params(Dxmt9WinemetalDestroyShaderParams* params) {
  if (!params) {
    return kStatusInvalidParameter;
  }
  dxmt9_winemetal_bridge_destroy_shader(params->shader_handle);
  return kStatusSuccess;
}

extern "C" dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request) {
  return dxmt9_winemetal_bridge_compile_shader(request);
}

extern "C" const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle) {
  return dxmt9_winemetal_bridge_shader_source(shaderHandle);
}

extern "C" dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle) {
  return dxmt9_winemetal_bridge_shader_source_size(shaderHandle);
}

extern "C" void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle) {
  dxmt9_winemetal_bridge_destroy_shader(shaderHandle);
}
