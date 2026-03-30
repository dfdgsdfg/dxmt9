/* src/win32/d3d9_entry.cpp — user-facing d3d9.dll entry points.
 *
 * d3d9.dll is intentionally thin: it exposes Direct3DCreate9 / 9Ex and forwards
 * those calls into the internal dxmt9.dll PE bridge. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

extern "C" __declspec(dllimport) IDirect3D9 *WINAPI dxmt9_bridge_create9(UINT sdkVersion);
extern "C" __declspec(dllimport) HRESULT WINAPI dxmt9_bridge_create9_ex(UINT sdkVersion,
                                                                         IDirect3D9Ex **ppD3D);

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdkVersion) {
  return dxmt9_bridge_create9(sdkVersion);
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex **ppD3D) {
  return dxmt9_bridge_create9_ex(sdkVersion, ppD3D);
}
