#include "winemetal_unix_call_handlers.hpp"

#include "util/unixcall_marshal.hpp"
#include "winemetal_service_abi.h"

#include <cstdint>

namespace {

constexpr std::int32_t kStatusSuccess = 0;
constexpr std::int32_t kStatusInvalidParameter = static_cast<std::int32_t>(0xC000000Du);

WinemetalShaderCompileRequest decodeCompileShaderRequest(
    const Dxmt9WinemetalCompileShaderParams& params) {
  return WinemetalShaderCompileRequest{
      .kind = static_cast<WinemetalShaderKind>(params.kind),
      .bytecode = dxmt9::util::marshal::decodePtr<const void>(params.bytecode_ptr),
      .bytecodeSize = params.bytecode_size,
      .bytecodeHash = params.bytecode_hash,
      .variantKey = dxmt9::util::marshal::decodePtr<const void>(params.variant_key_ptr),
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

extern "C" NTSTATUS dxmt9_winemetal_compile_shader_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalCompileShaderParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  const auto request = decodeCompileShaderRequest(*params);
  params->ret = dxmt9_winemetal_service_compile_shader(&request);
  return kStatusSuccess;
}

extern "C" NTSTATUS dxmt9_winemetal_shader_source_size_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceSizeParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  params->ret = dxmt9_winemetal_service_shader_source_size(params->shader_handle);
  return kStatusSuccess;
}

extern "C" NTSTATUS dxmt9_winemetal_shader_source_copy_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceCopyParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  if (params->buffer_capacity == 0 || params->buffer_ptr == 0) {
    params->bytes_written = 0;
    return kStatusSuccess;
  }
  char* destination = dxmt9::util::marshal::decodePtr<char>(params->buffer_ptr);
  params->bytes_written = dxmt9_winemetal_service_shader_source_copy(
      params->shader_handle, destination, params->buffer_capacity);
  return kStatusSuccess;
}

extern "C" NTSTATUS dxmt9_winemetal_destroy_shader_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalDestroyShaderParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  dxmt9_winemetal_service_destroy_shader(params->shader_handle);
  return kStatusSuccess;
}
