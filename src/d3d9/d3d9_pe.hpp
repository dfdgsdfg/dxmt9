#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include "dxmt9/device_c.h"
#include "dxmt9/wsi_surface_protocol.hpp"

#include <utility>

struct D3D9PeWindowDcCapability {
  D3D9PeWindowDcCapability() = default;
  D3D9PeWindowDcCapability(const D3D9PeWindowDcCapability&) = delete;
  D3D9PeWindowDcCapability& operator=(const D3D9PeWindowDcCapability&) = delete;
  D3D9PeWindowDcCapability(D3D9PeWindowDcCapability&& other) noexcept
      : hdc(other.hdc) {
    other.hdc = 0u;
  }
  D3D9PeWindowDcCapability& operator=(
      D3D9PeWindowDcCapability&& other) noexcept {
    if (this != &other) {
      std::swap(hdc, other.hdc);
    }
    return *this;
  }

  bool retained() const noexcept { return hdc != 0u; }
  std::uintptr_t hdc = 0u;
};

// PE-only owner: protocol/token state remains platform-neutral while the
// retained acquisition HDC is a move-only capability that never enters the
// C wire record or a CommandChunk.
struct D3D9PeWsiBinding : dxmt9::wsi::SurfaceBindingState {
  D3D9PeWsiBinding() = default;
  D3D9PeWsiBinding(const D3D9PeWsiBinding&) = delete;
  D3D9PeWsiBinding& operator=(const D3D9PeWsiBinding&) = delete;
  D3D9PeWsiBinding(D3D9PeWsiBinding&& other) noexcept {
    swap(other);
  }
  D3D9PeWsiBinding& operator=(D3D9PeWsiBinding&& other) noexcept {
    if (this != &other) {
      swap(other);
    }
    return *this;
  }

  void swap(D3D9PeWsiBinding& other) noexcept {
    std::swap(protocol, other.protocol);
    std::swap(hwnd, other.hwnd);
    std::swap(surfaceToken, other.surfaceToken);
    std::swap(layerToken, other.layerToken);
    std::swap(unixAdopted, other.unixAdopted);
    std::swap(releaseAttempted, other.releaseAttempted);
    std::swap(releaseCapability.hdc, other.releaseCapability.hdc);
  }

  D3D9PeWindowDcCapability releaseCapability{};
};

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
                                     D3D9PeWsiBinding wsiBinding,
                                     HRESULT* failureReason) noexcept;
void FillD3DCaps9(const D9CCaps& src, D3DCAPS9* out);
