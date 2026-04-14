#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dxmt9/wineunixlib.h"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);

  (void)reserved;
  return TRUE;
}
