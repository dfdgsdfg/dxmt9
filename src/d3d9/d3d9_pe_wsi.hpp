#pragma once

#include "d3d9_pe.hpp"
#include "dxmt9/wsi_surface_protocol.hpp"

D3D9PeWsiBinding dxmt9PeAcquireWsiBinding(HWND hwnd) noexcept;
HRESULT dxmt9PeAdoptWsiBinding(
    D9CSwapChain* swapChain, D3D9PeWsiBinding& binding) noexcept;
HRESULT dxmt9PeAdoptDeviceWsiBinding(
    D9CDevice* device, D3D9PeWsiBinding& binding) noexcept;
HRESULT dxmt9PeTeardownAndReleaseWsiBinding(
    D9CSwapChain* swapChain, D3D9PeWsiBinding& binding) noexcept;
HRESULT dxmt9PeTeardownDeviceAndReleaseWsiBinding(
    D9CDevice* device, D3D9PeWsiBinding& binding) noexcept;
void dxmt9PeFinalizeAndReleaseWsiBinding(
    D9CSwapChain* swapChain, D3D9PeWsiBinding& binding) noexcept;
void dxmt9PeFinalizeDeviceAndReleaseWsiBinding(
    D9CDevice* device, D3D9PeWsiBinding& binding) noexcept;
void dxmt9PeReleaseWsiBindingAfterQuiescence(
    D3D9PeWsiBinding& binding) noexcept;
