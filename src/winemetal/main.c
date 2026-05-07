#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wineunixlib.h"
#include "winemetal_abi_check.hpp"

typedef NTSTATUS (WINAPI *WineInitUnixCallFn)(void);

static WineInitUnixCallFn resolve_init_unix_call(void) {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) {
    return NULL;
  }
  return (WineInitUnixCallFn)GetProcAddress(ntdll, "__wine_init_unix_call");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason != DLL_PROCESS_ATTACH) {
    return TRUE;
  }
  DisableThreadLibraryCalls(instance);
  WineInitUnixCallFn init_unix_call = resolve_init_unix_call();
  if (init_unix_call) {
    (void)init_unix_call();
  }
  /* ABI handshake — call the reserved DXMT9_WINEMETAL_CALL_ABI_HASH slot
   * in winemetal.so and compare with the codegen-time hash baked into
   * this TU's dxmt9_bridge_ops.generated.h. On mismatch
   * dxmt9_winemetal_check_abi_handshake logs the skew and returns 0;
   * we propagate that as DllMain failure so the loader rejects the DLL
   * before any bridge call can dispatch into a misaligned slot table. */
  if (!dxmt9_winemetal_check_abi_handshake()) {
    return FALSE;
  }
  return TRUE;
}
