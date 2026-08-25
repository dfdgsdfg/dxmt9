#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include "dxmt9/device_c.h"
#include "dxmt9/wsi_surface_protocol.hpp"

using D3D9PeWsiBinding = dxmt9::wsi::SurfaceBindingState;

IDirect3D9* CreateFactoryImpl(D9CFactory* f) noexcept;
IDirect3D9Ex* CreateFactoryExImpl(D9CFactory* f) noexcept;
IDirect3DDevice9Ex* CreateDeviceImpl(D9CDevice* dev,
                                     IDirect3D9Ex* pFactory,
                                     UINT adapter,
                                     D3DDEVTYPE deviceType,
                                     DWORD behaviorFlags,
                                     HWND window,
                                     bool extended,
                                     DWORD implicitSwapchainFlags,
                                     dxmt9::wsi::SurfaceBindingState wsiBinding,
                                     HRESULT* failureReason) noexcept;
void FillD3DCaps9(const D9CCaps& src, D3DCAPS9* out);
