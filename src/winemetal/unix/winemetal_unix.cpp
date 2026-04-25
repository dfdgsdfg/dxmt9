#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#include "../../dxmt9/dxmt9_provider_shader_handlers.hpp"
#include "../winemetal_thunks.hpp"

// __wine_unix_call dispatch table — indexed by Dxmt9WinemetalCallId from
// winemetal_thunks.hpp. The PE-side winemetal.dll passes its handle +
// these IDs through __wine_unix_call; ntdll routes the call to the
// matching slot here. WMT (Metal API C wrapper) functions still ship as
// direct symbol exports from winemetal_private_api.mm and are resolved
// via dlsym fallback in winemetal_bridge.cpp — the table below covers
// only the marshaled call paths.
extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {
    [DXMT9_WINEMETAL_CALL_COMPILE_SHADER] = &dxmt9_winemetal_compile_shader_unix_call,
    [DXMT9_WINEMETAL_CALL_SHADER_SOURCE_SIZE] = &dxmt9_winemetal_shader_source_size_unix_call,
    [DXMT9_WINEMETAL_CALL_SHADER_SOURCE_COPY] = &dxmt9_winemetal_shader_source_copy_unix_call,
    [DXMT9_WINEMETAL_CALL_DESTROY_SHADER] = &dxmt9_winemetal_destroy_shader_unix_call,
};
extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    [DXMT9_WINEMETAL_CALL_COMPILE_SHADER] = &dxmt9_winemetal_compile_shader_unix_call,
    [DXMT9_WINEMETAL_CALL_SHADER_SOURCE_SIZE] = &dxmt9_winemetal_shader_source_size_unix_call,
    [DXMT9_WINEMETAL_CALL_SHADER_SOURCE_COPY] = &dxmt9_winemetal_shader_source_copy_unix_call,
    [DXMT9_WINEMETAL_CALL_DESTROY_SHADER] = &dxmt9_winemetal_destroy_shader_unix_call,
};

#endif
