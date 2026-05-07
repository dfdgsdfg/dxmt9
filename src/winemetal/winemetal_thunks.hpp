#pragma once

#include "dxmt9/winemetal.h"

#include <stdint.h>
#ifdef __cplusplus
#include <cstddef>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Shader-service unix-call IDs. Owned by winemetal.so's __wine_unix_call
// dispatch table at slots 0..3. The generated device_c bridge entries
// (BridgeOpcode in dxmt9_bridge_ops.generated.h) are renumbered to start
// at DXMT9_WINEMETAL_BRIDGE_OP_BASE so the two ID spaces don't collide
// in the same table.
//
// Slot DXMT9_WINEMETAL_CALL_ABI_HASH (= SHADER_COUNT, immediately after the
// shader handlers and immediately before the generated device_c bridge
// entries) is reserved at a fixed positional index so it does not drift
// when dxmt9c_* prototypes are added or removed. Its handler returns the
// unix-side dxmt9::bridge::kBridgeAbiHash so the PE-side DllMain can
// detect a winemetal.dll/winemetal.so version skew at load time.
enum dxmt9_winemetal_call_id {
  DXMT9_WINEMETAL_CALL_COMPILE_SHADER = 0,
  DXMT9_WINEMETAL_CALL_SHADER_SOURCE_SIZE = 1,
  DXMT9_WINEMETAL_CALL_SHADER_SOURCE_COPY = 2,
  DXMT9_WINEMETAL_CALL_DESTROY_SHADER = 3,
  DXMT9_WINEMETAL_CALL_SHADER_COUNT = 4,
  DXMT9_WINEMETAL_CALL_ABI_HASH = 4,
  DXMT9_WINEMETAL_CALL_RESERVED_COUNT = 5,
};

// First slot consumed by the generated device_c bridge entries — read by
// gen_wine_bridge.py when writing dxmt9_bridge_ops.generated.h. Equals
// the count of explicit shader IDs (4) plus the reserved ABI-hash slot
// (1) so the unified __wine_unix_call dispatch table indexes correctly.
#define DXMT9_WINEMETAL_BRIDGE_OP_BASE DXMT9_WINEMETAL_CALL_RESERVED_COUNT

// Layout for the params struct passed to slot DXMT9_WINEMETAL_CALL_ABI_HASH.
// Pointer-free POD so the same struct works for native, PE→ELF, and wow64
// dispatch paths without translation.
typedef struct Dxmt9WinemetalAbiHashParams {
  uint64_t hash;
} Dxmt9WinemetalAbiHashParams;

typedef struct Dxmt9WinemetalCompileShaderParams {
  uint64_t bytecode_ptr;
  uint64_t bytecode_size;
  uint64_t bytecode_hash;
  uint64_t variant_key_ptr;
  uint64_t ret;
  uint32_t kind;
  uint32_t textured;
  uint32_t clip_plane_mask;
  uint32_t sample_count;
  uint32_t alpha_test_enable;
  uint32_t alpha_test_func;
  uint32_t fog_mode;
  float alpha_ref;
} Dxmt9WinemetalCompileShaderParams;

typedef struct Dxmt9WinemetalShaderSourceSizeParams {
  uint64_t shader_handle;
  uint64_t ret;
} Dxmt9WinemetalShaderSourceSizeParams;

typedef struct Dxmt9WinemetalShaderSourceCopyParams {
  uint64_t shader_handle;
  uint64_t buffer_ptr;
  uint64_t buffer_capacity;
  uint64_t bytes_written;
} Dxmt9WinemetalShaderSourceCopyParams;

typedef struct Dxmt9WinemetalDestroyShaderParams {
  uint64_t shader_handle;
} Dxmt9WinemetalDestroyShaderParams;

#ifdef __cplusplus
static_assert(sizeof(Dxmt9WinemetalCompileShaderParams) == 72);
static_assert(offsetof(Dxmt9WinemetalCompileShaderParams, bytecode_ptr) == 0);
static_assert(offsetof(Dxmt9WinemetalCompileShaderParams, variant_key_ptr) == 24);
static_assert(offsetof(Dxmt9WinemetalCompileShaderParams, ret) == 32);
static_assert(offsetof(Dxmt9WinemetalCompileShaderParams, kind) == 40);
static_assert(offsetof(Dxmt9WinemetalCompileShaderParams, fog_mode) == 64);
static_assert(offsetof(Dxmt9WinemetalCompileShaderParams, alpha_ref) == 68);
#endif

#ifdef __cplusplus
}
#endif
