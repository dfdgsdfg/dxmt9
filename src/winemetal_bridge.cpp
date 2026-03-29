#include "dxmt9/winemetal.h"

namespace {

struct DispatchState {
  WinemetalApi api{};
};

DispatchState& dispatchState() {
  static DispatchState state;
  return state;
}

#if defined(__APPLE__)
extern "C" {
dxmt9_u64 winemetal_compile_shader(const WinemetalShaderCompileRequest* request);
const char* winemetal_shader_source(dxmt9_u64 shaderHandle);
dxmt9_u64 winemetal_shader_source_size(dxmt9_u64 shaderHandle);
void winemetal_destroy_shader(dxmt9_u64 shaderHandle);
}
#endif

#if !defined(__APPLE__)
static dxmt9_u64 stubCompileShader(const WinemetalShaderCompileRequest*) { return 0; }
static const char* stubShaderSource(dxmt9_u64) { return nullptr; }
static dxmt9_u64 stubShaderSourceSize(dxmt9_u64) { return 0; }
static void stubDestroyShader(dxmt9_u64) {}
#endif

}  // namespace

extern "C" const WinemetalApi* dxmt9_winemetal_get_api(void) {
  auto& state = dispatchState();
  if (state.api.compile_shader) {
    return &state.api;
  }
#if defined(__APPLE__)
  state.api.compile_shader       = winemetal_compile_shader;
  state.api.shader_source        = winemetal_shader_source;
  state.api.shader_source_size   = winemetal_shader_source_size;
  state.api.destroy_shader       = winemetal_destroy_shader;
#else
  state.api.compile_shader       = stubCompileShader;
  state.api.shader_source        = stubShaderSource;
  state.api.shader_source_size   = stubShaderSourceSize;
  state.api.destroy_shader       = stubDestroyShader;
#endif
  return &state.api;
}

extern "C" void dxmt9_winemetal_set_api(const WinemetalApi* api) {
  auto& state = dispatchState();
  if (api) {
    /* Only override non-null entries so a partial override (e.g. only
     * compile_shader) does not clobber the native fallbacks. */
    if (api->compile_shader)     state.api.compile_shader     = api->compile_shader;
    if (api->shader_source)      state.api.shader_source      = api->shader_source;
    if (api->shader_source_size) state.api.shader_source_size = api->shader_source_size;
    if (api->destroy_shader)     state.api.destroy_shader     = api->destroy_shader;
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
