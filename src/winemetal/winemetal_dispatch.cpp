#include "dxmt9/winemetal.h"

#include "winemetal_dispatch_internal.hpp"

namespace {

struct DispatchState {
  WinemetalApi api{};
};

DispatchState& dispatchState() {
  static DispatchState state;
  return state;
}

}  // namespace

extern "C" const WinemetalApi* dxmt9_winemetal_get_api(void) {
  auto& state = dispatchState();
  if (state.api.compile_shader) {
    return &state.api;
  }
  state.api.compile_shader = dxmt9_winemetal_default_compile_shader;
  state.api.shader_source = dxmt9_winemetal_default_shader_source;
  state.api.shader_source_size = dxmt9_winemetal_default_shader_source_size;
  state.api.destroy_shader = dxmt9_winemetal_default_destroy_shader;
  return &state.api;
}

extern "C" void dxmt9_winemetal_set_api(const WinemetalApi* api) {
  auto& state = dispatchState();
  if (api) {
    if (api->compile_shader) {
      state.api.compile_shader = api->compile_shader;
    }
    if (api->shader_source) {
      state.api.shader_source = api->shader_source;
    }
    if (api->shader_source_size) {
      state.api.shader_source_size = api->shader_source_size;
    }
    if (api->destroy_shader) {
      state.api.destroy_shader = api->destroy_shader;
    }
    return;
  }
  state.api = {};
}

extern "C" dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request) {
  const auto* api = dxmt9_winemetal_get_api();
  return api && api->compile_shader ? api->compile_shader(request) : 0;
}

extern "C" const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle) {
  const auto* api = dxmt9_winemetal_get_api();
  return api && api->shader_source ? api->shader_source(shaderHandle) : nullptr;
}

extern "C" dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle) {
  const auto* api = dxmt9_winemetal_get_api();
  return api && api->shader_source_size ? api->shader_source_size(shaderHandle) : 0;
}

extern "C" void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle) {
  const auto* api = dxmt9_winemetal_get_api();
  if (api && api->destroy_shader) {
    api->destroy_shader(shaderHandle);
  }
}
