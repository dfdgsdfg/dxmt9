#include "winemetal_unix_call_handlers.hpp"

#include "util/util_buffer.hpp"
#include "winemetal_shader_bridge_internal.hpp"

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
  const auto request = dxmt9::winemetal::decodeCompileShaderRequest(*params);
  params->ret = dxmt9::winemetal::compileShader(&request);
  return kStatusSuccess;
}

extern "C" NTSTATUS dxmt9_winemetal_shader_source_size_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalShaderSourceSizeParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  params->ret = dxmt9::winemetal::shaderSourceSize(params->shader_handle);
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
  char* destination = dxmt9::util::u64ToPtr<char>(params->buffer_ptr);
  params->bytes_written =
      dxmt9::winemetal::copyShaderSource(params->shader_handle, destination, params->buffer_capacity);
  return kStatusSuccess;
}

extern "C" NTSTATUS dxmt9_winemetal_destroy_shader_unix_call(void* opaque) {
  auto* params = static_cast<Dxmt9WinemetalDestroyShaderParams*>(opaque);
  if (!params) {
    return kStatusInvalidParameter;
  }
  dxmt9::winemetal::destroyShader(params->shader_handle);
  return kStatusSuccess;
}
