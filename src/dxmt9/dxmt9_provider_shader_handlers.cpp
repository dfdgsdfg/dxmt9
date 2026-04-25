#include "dxmt9_provider_service_abi.h"
#include "dxmt9_provider_shader_handlers.hpp"

#include <cstdint>

namespace {

constexpr std::int32_t kStatusSuccess = 0;
constexpr std::int32_t kStatusInvalidParameter = static_cast<std::int32_t>(0xC000000Du);

}  // namespace

extern "C" NTSTATUS dxmt9_winemetal_compile_shader_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalCompileShaderParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  params->ret = dxmt9_provider_service_compile_shader_marshaled(params);
  return kStatusSuccess;
}

extern "C" NTSTATUS dxmt9_winemetal_shader_source_size_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceSizeParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  params->ret = dxmt9_provider_service_shader_source_size(params->shader_handle);
  return kStatusSuccess;
}

extern "C" NTSTATUS dxmt9_winemetal_shader_source_copy_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceCopyParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  params->bytes_written = dxmt9_provider_service_shader_source_copy_marshaled(params);
  return kStatusSuccess;
}

extern "C" NTSTATUS dxmt9_winemetal_destroy_shader_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalDestroyShaderParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  dxmt9_provider_service_destroy_shader_marshaled(params);
  return kStatusSuccess;
}
