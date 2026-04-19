#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#endif

#if defined(WINE_UNIX_LIB)

extern "C" NTSTATUS dxmt9_winemetal_unused_unix_call(void*) {
  return DXMT9_STATUS_NOT_SUPPORTED;
}

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {
    dxmt9_winemetal_unused_unix_call,
};

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    dxmt9_winemetal_unused_unix_call,
};

#endif
