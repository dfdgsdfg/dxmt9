#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include "dxmt9/device_c.h"

IDirect3D9* CreateFactoryImpl(D9CFactory* f);
IDirect3D9Ex* CreateFactoryExImpl(D9CFactory* f);
IDirect3DDevice9Ex* CreateDeviceImpl(D9CDevice* dev,
                                     IDirect3D9Ex* pFactory,
                                     UINT adapter,
                                     D3DDEVTYPE deviceType,
                                     DWORD behaviorFlags,
                                     HWND window,
                                     bool extended,
                                     DWORD implicitSwapchainFlags);
void FillD3DCaps9(const D9CCaps& src, D3DCAPS9* out);
