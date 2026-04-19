#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#include "dxmt9_provider_shader_handlers.hpp"
#endif

#if defined(WINE_UNIX_LIB)

#define DXMT9_BRIDGE_UNIX_ENTRY(native, wow64) \
  extern NTSTATUS native(void* opaque);        \
  extern NTSTATUS wow64(void* opaque);
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {
#define DXMT9_BRIDGE_UNIX_ENTRY(native, wow64) native,
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY
    dxmt9_winemetal_compile_shader_unix_call,
    dxmt9_winemetal_shader_source_size_unix_call,
    dxmt9_winemetal_shader_source_copy_unix_call,
    dxmt9_winemetal_destroy_shader_unix_call,
};

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
#define DXMT9_BRIDGE_UNIX_ENTRY(native, wow64) wow64,
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY
    dxmt9_winemetal_compile_shader_unix_call,
    dxmt9_winemetal_shader_source_size_unix_call,
    dxmt9_winemetal_shader_source_copy_unix_call,
    dxmt9_winemetal_destroy_shader_unix_call,
};

#endif
