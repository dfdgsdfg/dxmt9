#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t dxmt9_u32;
typedef uint64_t dxmt9_u64;

/* WinemetalShaderCompileRequest — passed to compile_shader for ahead-of-time
 * MSL generation.  An optional Wine build with the Apple shader converter can
 * provide a faster compile_shader implementation; the default falls back to
 * the built-in D3DBC→MSL translator in dxmt9.so. */

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

/* WinemetalApi — ABI contract between the PE bridge and dxmt9.so.
 *
 * Window/layer management (HWND→NSView lookup, CAMetalLayer lifecycle,
 * drawable vending) is handled directly inside dxmt9.so via
 * macdrv_get_cocoa_view() from Wine's winemac.drv — no Wine fork required.
 *
 * The only optional override is compile_shader, which a Wine build that
 * includes the Apple Metal shader converter can provide for faster PSO
 * compilation.  All pointers may be null; dxmt9.so falls back to its
 * built-in translator when compile_shader is null. */
typedef struct WinemetalApi {
  dxmt9_u64 (*compile_shader)(const WinemetalShaderCompileRequest* request);
  const char* (*shader_source)(dxmt9_u64 shaderHandle);
  dxmt9_u64 (*shader_source_size)(dxmt9_u64 shaderHandle);
  void (*destroy_shader)(dxmt9_u64 shaderHandle);
} WinemetalApi;

const WinemetalApi* dxmt9_winemetal_get_api(void);
void dxmt9_winemetal_set_api(const WinemetalApi* api);

dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request);
const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle);
dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle);
void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle);

#ifdef __cplusplus
}
#endif
