#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

extern "C" IDirect3D9* WINAPI dxmt9_pe_create9(UINT sdkVersion);
extern "C" HRESULT WINAPI dxmt9_pe_create9_ex(UINT sdkVersion, IDirect3D9Ex** ppD3D);
