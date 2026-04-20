#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"

// winemetal.so no longer hosts the device_c generated unix bridge.
// WMT C functions are exposed as direct symbol exports from
// winemetal_private_api.mm; the PE-side bridge dlsym-resolves them
// via the builtin-dispatcher fallback in winemetal_bridge.cpp.
extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {};
extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {};

#endif
