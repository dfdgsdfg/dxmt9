#include "dxmt9/winemetal.h"

namespace {

using GetViewForHwndFn = dxmt9_u64 (*)(dxmt9_u64);
using CreateLayerFn = dxmt9_u64 (*)(dxmt9_u64, dxmt9_u64, const WinemetalPresentParams*);
using ResizeLayerFn = void (*)(dxmt9_u64, dxmt9_u32, dxmt9_u32);
using SetSyncFn = void (*)(dxmt9_u64, bool);
using DestroyLayerFn = void (*)(dxmt9_u64);
using NextDrawableFn = dxmt9_u64 (*)(dxmt9_u64);
using PresentDrawableFn = void (*)(dxmt9_u64, dxmt9_u64);
using CompileShaderFn = dxmt9_u64 (*)(const WinemetalShaderCompileRequest*);
using ShaderSourceFn = const char* (*)(dxmt9_u64);
using ShaderSourceSizeFn = dxmt9_u64 (*)(dxmt9_u64);
using DestroyShaderFn = void (*)(dxmt9_u64);

struct DispatchState {
  WinemetalApi api{};
};

DispatchState& dispatchState() {
  static DispatchState state;
  return state;
}

#if defined(__APPLE__)
extern "C" {
dxmt9_u64 winemetal_get_view_for_hwnd(dxmt9_u64 hwnd);
dxmt9_u64 winemetal_create_metal_layer(dxmt9_u64 viewHandle, dxmt9_u64 deviceHandle,
                                       const WinemetalPresentParams* params);
void winemetal_resize_metal_layer(dxmt9_u64 layerHandle, dxmt9_u32 width, dxmt9_u32 height);
void winemetal_set_sync_enabled(dxmt9_u64 layerHandle, bool enabled);
void winemetal_destroy_metal_layer(dxmt9_u64 layerHandle);
dxmt9_u64 winemetal_next_drawable(dxmt9_u64 layerHandle);
void winemetal_present_drawable(dxmt9_u64 commandBufferHandle, dxmt9_u64 drawableHandle);
dxmt9_u64 winemetal_compile_shader(const WinemetalShaderCompileRequest* request);
const char* winemetal_shader_source(dxmt9_u64 shaderHandle);
dxmt9_u64 winemetal_shader_source_size(dxmt9_u64 shaderHandle);
void winemetal_destroy_shader(dxmt9_u64 shaderHandle);
}
#endif

#if !defined(__APPLE__)
dxmt9_u64 stubGetViewForHwnd(dxmt9_u64 hwnd) {
  return hwnd;
}

dxmt9_u64 stubCreateLayer(dxmt9_u64, dxmt9_u64, const WinemetalPresentParams*) {
  return 0;
}

void stubResizeLayer(dxmt9_u64, dxmt9_u32, dxmt9_u32) {}
void stubSetSync(dxmt9_u64, bool) {}
void stubDestroyLayer(dxmt9_u64) {}
dxmt9_u64 stubNextDrawable(dxmt9_u64) {
  return 0;
}
void stubPresentDrawable(dxmt9_u64, dxmt9_u64) {}
dxmt9_u64 stubCompileShader(const WinemetalShaderCompileRequest*) {
  return 0;
}
const char* stubShaderSource(dxmt9_u64) {
  return nullptr;
}
dxmt9_u64 stubShaderSourceSize(dxmt9_u64) {
  return 0;
}
void stubDestroyShader(dxmt9_u64) {}
#endif

}  // namespace

extern "C" const WinemetalApi* dxmt9_winemetal_get_api(void) {
  auto& state = dispatchState();
  if (state.api.get_view_for_hwnd) {
    return &state.api;
  }
#if defined(__APPLE__)
  state.api.get_view_for_hwnd = winemetal_get_view_for_hwnd;
  state.api.create_metal_layer = winemetal_create_metal_layer;
  state.api.resize_metal_layer = winemetal_resize_metal_layer;
  state.api.set_sync_enabled = winemetal_set_sync_enabled;
  state.api.destroy_metal_layer = winemetal_destroy_metal_layer;
  state.api.next_drawable = winemetal_next_drawable;
  state.api.present_drawable = winemetal_present_drawable;
  state.api.compile_shader = winemetal_compile_shader;
  state.api.shader_source = winemetal_shader_source;
  state.api.shader_source_size = winemetal_shader_source_size;
  state.api.destroy_shader = winemetal_destroy_shader;
#else
  state.api.get_view_for_hwnd = stubGetViewForHwnd;
  state.api.create_metal_layer = stubCreateLayer;
  state.api.resize_metal_layer = stubResizeLayer;
  state.api.set_sync_enabled = stubSetSync;
  state.api.destroy_metal_layer = stubDestroyLayer;
  state.api.next_drawable = stubNextDrawable;
  state.api.present_drawable = stubPresentDrawable;
  state.api.compile_shader = stubCompileShader;
  state.api.shader_source = stubShaderSource;
  state.api.shader_source_size = stubShaderSourceSize;
  state.api.destroy_shader = stubDestroyShader;
#endif
  return &state.api;
}

extern "C" void dxmt9_winemetal_set_api(const WinemetalApi* api) {
  auto& state = dispatchState();
  if (api) {
    state.api = *api;
    return;
  }
  state.api = {};
}

extern "C" dxmt9_u64 dxmt9_winemetal_get_view_for_hwnd(dxmt9_u64 hwnd) {
  const auto* api = dxmt9_winemetal_get_api();
  return api && api->get_view_for_hwnd ? api->get_view_for_hwnd(hwnd) : 0;
}

extern "C" dxmt9_u64 dxmt9_winemetal_create_metal_layer(dxmt9_u64 viewHandle, dxmt9_u64 deviceHandle,
                                                         const WinemetalPresentParams* params) {
  const auto* api = dxmt9_winemetal_get_api();
  return api && api->create_metal_layer ? api->create_metal_layer(viewHandle, deviceHandle, params) : 0;
}

extern "C" void dxmt9_winemetal_resize_metal_layer(dxmt9_u64 layerHandle, dxmt9_u32 width, dxmt9_u32 height) {
  const auto* api = dxmt9_winemetal_get_api();
  if (api && api->resize_metal_layer) {
    api->resize_metal_layer(layerHandle, width, height);
  }
}

extern "C" void dxmt9_winemetal_set_sync_enabled(dxmt9_u64 layerHandle, bool enabled) {
  const auto* api = dxmt9_winemetal_get_api();
  if (api && api->set_sync_enabled) {
    api->set_sync_enabled(layerHandle, enabled);
  }
}

extern "C" void dxmt9_winemetal_destroy_metal_layer(dxmt9_u64 layerHandle) {
  const auto* api = dxmt9_winemetal_get_api();
  if (api && api->destroy_metal_layer) {
    api->destroy_metal_layer(layerHandle);
  }
}

extern "C" dxmt9_u64 dxmt9_winemetal_next_drawable(dxmt9_u64 layerHandle) {
  const auto* api = dxmt9_winemetal_get_api();
  return api && api->next_drawable ? api->next_drawable(layerHandle) : 0;
}

extern "C" void dxmt9_winemetal_present_drawable(dxmt9_u64 commandBufferHandle, dxmt9_u64 drawableHandle) {
  const auto* api = dxmt9_winemetal_get_api();
  if (api && api->present_drawable) {
    api->present_drawable(commandBufferHandle, drawableHandle);
  }
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
