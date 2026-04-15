#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wineunixlib.h"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason != DLL_PROCESS_ATTACH) {
    return TRUE;
  }
  DisableThreadLibraryCalls(instance);
  return __wine_init_unix_call() == DXMT9_STATUS_SUCCESS;
}
