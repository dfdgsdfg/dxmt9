/* src/win32/d3d9_entry.cpp — user-facing d3d9.dll entry points.
 *
 * d3d9.dll is intentionally thin: it exposes the PE D3D9 export surface and
 * forwards calls into the frontend-owned D3D9 PE export shim. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include "../d3d9/d3d9_pe_exports.hpp"

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdkVersion) {
  return dxmt9_pe_create9(sdkVersion);
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex **ppD3D) {
  return dxmt9_pe_create9_ex(sdkVersion, ppD3D);
}

extern "C" IDirect3D9 *WINAPI Direct3DCreate9On12(UINT sdkVersion,
                                                   void *d3d9On12Args,
                                                   UINT d3d9On12ArgsCount) {
  return dxmt9_pe_create9_on12(sdkVersion, d3d9On12Args, d3d9On12ArgsCount);
}

extern "C" IDirect3DShaderValidator9 *WINAPI Direct3DShaderValidatorCreate9(void) {
  return dxmt9_pe_create_shader_validator();
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, const WCHAR *name) {
  return dxmt9_pe_perf_begin_event(color, name);
}

extern "C" int WINAPI D3DPERF_EndEvent(void) {
  return dxmt9_pe_perf_end_event();
}

extern "C" DWORD WINAPI D3DPERF_GetStatus(void) {
  return dxmt9_pe_perf_get_status();
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
  return dxmt9_pe_perf_query_repeat_frame();
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR color, const WCHAR *name) {
  dxmt9_pe_perf_set_marker(color, name);
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD options) {
  dxmt9_pe_perf_set_options(options);
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR color, const WCHAR *name) {
  dxmt9_pe_perf_set_region(color, name);
}

extern "C" void WINAPI DebugSetMute(void) {
  dxmt9_pe_debug_set_mute();
}
