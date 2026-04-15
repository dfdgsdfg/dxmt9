#pragma once

#include "dxmt9/winemetal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dxmt9_winemetal_call_id {
  DXMT9_WINEMETAL_CALL_COMPILE_SHADER = 0,
  DXMT9_WINEMETAL_CALL_SHADER_SOURCE_SIZE = 1,
  DXMT9_WINEMETAL_CALL_SHADER_SOURCE_COPY = 2,
  DXMT9_WINEMETAL_CALL_DESTROY_SHADER = 3,
};

typedef struct Dxmt9WinemetalCompileShaderParams {
  uint32_t kind;
  uint64_t bytecode_ptr;
  uint64_t bytecode_size;
  uint64_t bytecode_hash;
  uint64_t variant_key_ptr;
  uint32_t textured;
  uint32_t clip_plane_mask;
  uint32_t sample_count;
  uint32_t alpha_test_enable;
  uint32_t alpha_test_func;
  float alpha_ref;
  uint32_t fog_mode;
  uint64_t ret;
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
}
#endif
