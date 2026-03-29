#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t dxmt9_u32;
typedef uint64_t dxmt9_u64;

typedef struct WinemetalPresentParams {
  dxmt9_u32 width;
  dxmt9_u32 height;
  bool displaySyncEnabled;
  dxmt9_u32 sampleCount;
} WinemetalPresentParams;

typedef enum WinemetalShaderKind {
  WinemetalShaderKind_D3DBytecodeVertex = 0,
  WinemetalShaderKind_D3DBytecodePixel = 1,
  WinemetalShaderKind_FfpVertex = 2,
  WinemetalShaderKind_FfpPixel = 3,
} WinemetalShaderKind;

typedef struct WinemetalShaderCompileRequest {
  WinemetalShaderKind kind;
  const void* bytecode;
  dxmt9_u64 bytecodeSize;
  dxmt9_u64 bytecodeHash;
  const void* variantKey;
  bool textured;
  dxmt9_u32 clipPlaneMask;
  dxmt9_u32 sampleCount;
  dxmt9_u32 alphaTestEnable;
  dxmt9_u32 alphaTestFunc;
  float alphaRef;
  dxmt9_u32 fogMode;
} WinemetalShaderCompileRequest;

typedef struct WinemetalApi {
  dxmt9_u64 (*get_view_for_hwnd)(dxmt9_u64 hwnd);
  dxmt9_u64 (*create_metal_layer)(dxmt9_u64 viewHandle, dxmt9_u64 deviceHandle,
                                  const WinemetalPresentParams* params);
  void (*resize_metal_layer)(dxmt9_u64 layerHandle, dxmt9_u32 width, dxmt9_u32 height);
  void (*set_sync_enabled)(dxmt9_u64 layerHandle, bool enabled);
  void (*destroy_metal_layer)(dxmt9_u64 layerHandle);
  dxmt9_u64 (*next_drawable)(dxmt9_u64 layerHandle);
  void (*present_drawable)(dxmt9_u64 commandBufferHandle, dxmt9_u64 drawableHandle);
  dxmt9_u64 (*compile_shader)(const WinemetalShaderCompileRequest* request);
  const char* (*shader_source)(dxmt9_u64 shaderHandle);
  dxmt9_u64 (*shader_source_size)(dxmt9_u64 shaderHandle);
  void (*destroy_shader)(dxmt9_u64 shaderHandle);
} WinemetalApi;

const WinemetalApi* dxmt9_winemetal_get_api(void);
void dxmt9_winemetal_set_api(const WinemetalApi* api);

dxmt9_u64 dxmt9_winemetal_get_view_for_hwnd(dxmt9_u64 hwnd);
dxmt9_u64 dxmt9_winemetal_create_metal_layer(dxmt9_u64 viewHandle, dxmt9_u64 deviceHandle,
                                            const WinemetalPresentParams* params);
void dxmt9_winemetal_resize_metal_layer(dxmt9_u64 layerHandle, dxmt9_u32 width, dxmt9_u32 height);
void dxmt9_winemetal_set_sync_enabled(dxmt9_u64 layerHandle, bool enabled);
void dxmt9_winemetal_destroy_metal_layer(dxmt9_u64 layerHandle);
dxmt9_u64 dxmt9_winemetal_next_drawable(dxmt9_u64 layerHandle);
void dxmt9_winemetal_present_drawable(dxmt9_u64 commandBufferHandle, dxmt9_u64 drawableHandle);
dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request);
const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle);
dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle);
void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle);

#ifdef __cplusplus
}
#endif
