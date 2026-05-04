#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

extern "C" IDirect3D9* WINAPI dxmt9_pe_create9(UINT sdkVersion);
extern "C" HRESULT WINAPI dxmt9_pe_create9_ex(UINT sdkVersion, IDirect3D9Ex** ppD3D);
extern "C" IDirect3D9* WINAPI dxmt9_pe_create9_on12(UINT sdkVersion,
                                                     void* d3d9On12Args,
                                                     UINT d3d9On12ArgsCount);
struct IDirect3DShaderValidator9;
extern "C" IDirect3DShaderValidator9* WINAPI dxmt9_pe_create_shader_validator(void);
extern "C" int WINAPI dxmt9_pe_perf_begin_event(D3DCOLOR color, const WCHAR* name);
extern "C" int WINAPI dxmt9_pe_perf_end_event(void);
extern "C" DWORD WINAPI dxmt9_pe_perf_get_status(void);
extern "C" BOOL WINAPI dxmt9_pe_perf_query_repeat_frame(void);
extern "C" void WINAPI dxmt9_pe_perf_set_marker(D3DCOLOR color, const WCHAR* name);
extern "C" void WINAPI dxmt9_pe_perf_set_options(DWORD options);
extern "C" void WINAPI dxmt9_pe_perf_set_region(D3DCOLOR color, const WCHAR* name);
extern "C" void WINAPI dxmt9_pe_debug_set_mute(void);
