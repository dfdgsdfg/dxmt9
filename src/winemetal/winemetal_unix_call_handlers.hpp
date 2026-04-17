#pragma once

#include "dxmt9/wineunixlib.h"
#include "winemetal_thunks.hpp"

#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS dxmt9_winemetal_compile_shader_unix_call(void* args);
NTSTATUS dxmt9_winemetal_shader_source_size_unix_call(void* args);
NTSTATUS dxmt9_winemetal_shader_source_copy_unix_call(void* args);
NTSTATUS dxmt9_winemetal_destroy_shader_unix_call(void* args);

#ifdef __cplusplus
}
#endif
