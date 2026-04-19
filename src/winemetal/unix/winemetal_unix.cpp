#include "../winemetal_thunks.hpp"

#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#endif

#if defined(WINE_UNIX_LIB)

extern "C" NTSTATUS dxmt9_winemetal_compile_shader_unix_call(void* opaque);
extern "C" NTSTATUS dxmt9_winemetal_shader_source_size_unix_call(void* opaque);
extern "C" NTSTATUS dxmt9_winemetal_shader_source_copy_unix_call(void* opaque);
extern "C" NTSTATUS dxmt9_winemetal_destroy_shader_unix_call(void* opaque);

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {
    dxmt9_winemetal_compile_shader_unix_call,
    dxmt9_winemetal_shader_source_size_unix_call,
    dxmt9_winemetal_shader_source_copy_unix_call,
    dxmt9_winemetal_destroy_shader_unix_call,
};

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    dxmt9_winemetal_compile_shader_unix_call,
    dxmt9_winemetal_shader_source_size_unix_call,
    dxmt9_winemetal_shader_source_copy_unix_call,
    dxmt9_winemetal_destroy_shader_unix_call,
};

#endif
