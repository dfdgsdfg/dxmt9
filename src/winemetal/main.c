#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wineunixlib.h"

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
  return TRUE;
}
