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

dxmt9_u64 dxmt9_winemetal_compile_shader(const WinemetalShaderCompileRequest* request);
const char* dxmt9_winemetal_shader_source(dxmt9_u64 shaderHandle);
dxmt9_u64 dxmt9_winemetal_shader_source_size(dxmt9_u64 shaderHandle);
void dxmt9_winemetal_destroy_shader(dxmt9_u64 shaderHandle);

#ifdef __cplusplus
}
#endif
