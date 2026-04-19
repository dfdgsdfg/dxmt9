#include "dxmt9_shader_service.hpp"
#include "dxmt9_provider_service_abi.h"
#include "util/unixcall_marshal.hpp"
#include "util/util_buffer.hpp"

namespace {

WinemetalShaderCompileRequest decodeCompileShaderRequest(
    const Dxmt9WinemetalCompileShaderParams& params) {
  return WinemetalShaderCompileRequest{
      .kind = static_cast<WinemetalShaderKind>(params.kind),
      .bytecode = dxmt9::util::marshal::decodeNullablePtr<const void>(params.bytecode_ptr),
      .bytecodeSize = params.bytecode_size,
      .bytecodeHash = params.bytecode_hash,
      .variantKey = dxmt9::util::marshal::decodeNullablePtr<const void>(params.variant_key_ptr),
      .textured = params.textured != 0,
      .clipPlaneMask = params.clip_plane_mask,
      .sampleCount = params.sample_count,
      .alphaTestEnable = params.alpha_test_enable,
      .alphaTestFunc = params.alpha_test_func,
      .alphaRef = params.alpha_ref,
      .fogMode = params.fog_mode,
  };
}

}  // namespace

extern "C" dxmt9_u64
dxmt9_provider_service_compile_shader(const WinemetalShaderCompileRequest* request) {
  if (!request) {
    return 0;
  }
  return dxmt9::core::shader_service::compile(*request);
}

extern "C" dxmt9_u64
dxmt9_provider_service_compile_shader_marshaled(const Dxmt9WinemetalCompileShaderParams* params) {
  if (!params) {
    return 0;
  }
  const auto request = decodeCompileShaderRequest(*params);
  return dxmt9_provider_service_compile_shader(&request);
}

extern "C" dxmt9_u64 dxmt9_provider_service_shader_source_size(dxmt9_u64 shaderHandle) {
  return dxmt9::core::shader_service::sourceSize(shaderHandle);
}

extern "C" dxmt9_u64
dxmt9_provider_service_shader_source_copy(dxmt9_u64 shaderHandle,
                                          char* buffer,
                                          dxmt9_u64 bufferCapacity) {
  if (!buffer || bufferCapacity == 0) {
    return 0;
  }
  const std::string source = dxmt9::core::shader_service::source(shaderHandle);
  if (source.empty()) {
    buffer[0] = '\0';
    return 0;
  }
  return dxmt9::util::copyStringToBuffer(source, buffer, bufferCapacity);
}

extern "C" dxmt9_u64
dxmt9_provider_service_shader_source_copy_marshaled(const Dxmt9WinemetalShaderSourceCopyParams* params) {
  if (!params) {
    return 0;
  }
  char* destination = dxmt9::util::marshal::decodeCharBuffer(
      params->buffer_ptr, params->buffer_capacity);
  if (!destination) {
    return 0;
  }
  return dxmt9_provider_service_shader_source_copy(
      params->shader_handle, destination, params->buffer_capacity);
}

extern "C" void dxmt9_provider_service_destroy_shader(dxmt9_u64 shaderHandle) {
  dxmt9::core::shader_service::destroy(shaderHandle);
}

extern "C" void dxmt9_provider_service_destroy_shader_marshaled(
    const Dxmt9WinemetalDestroyShaderParams* params) {
  if (!params) {
    return;
  }
  dxmt9_provider_service_destroy_shader(params->shader_handle);
}
