#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
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
};

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
#define DXMT9_BRIDGE_UNIX_ENTRY(native, wow64) wow64,
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY
};

#endif
