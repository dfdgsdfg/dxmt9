/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex owning TU.
 * The class and its file-local support live in d3d9_pe_device_impl.hpp;
 * this TU exists to define the factory entry point. */

#include "d3d9_pe_device_impl.hpp"

#include <new>

/* =========================================================================
 * Key function -- see the declaration comment in d3d9_pe_device_impl.hpp.
 * Its only job here is to be the first non-pure, non-inline virtual in
 * declaration order, which anchors the vtable in this translation unit.
 * ========================================================================= */

HRESULT D3D9DeviceImpl::FlushPeRecorderForChild() noexcept {
    return flushPeRecorder(PeRecorderFlushReason::Child);
}

/* =========================================================================
 * Factory function (called from factory.cpp)
 * ========================================================================= */

IDirect3DDevice9Ex* CreateDeviceImpl(D9CDevice* dev, IDirect3D9Ex* pFactory,
                                     UINT adapter, D3DDEVTYPE deviceType,
                                     DWORD behaviorFlags,
                                     HWND window, bool extended,
                                     DWORD implicitSwapchainFlags,
                                     HRESULT* failureReason) noexcept {
    if (failureReason) *failureReason = D3DERR_NOTAVAILABLE;
    D3D9DeviceImpl* device = nullptr;
    try {
        device = new (std::nothrow) D3D9DeviceImpl(
            dev, pFactory, adapter, deviceType, behaviorFlags, window, extended,
            implicitSwapchainFlags);
    } catch (const std::bad_alloc&) {
        if (dev) dxmt9c_device_release(dev);
        if (failureReason) *failureReason = E_OUTOFMEMORY;
        return nullptr;
    } catch (...) {
        if (dev) dxmt9c_device_release(dev);
        if (failureReason) *failureReason = D3DERR_INVALIDCALL;
        return nullptr;
    }
    if (!device) {
        if (dev) dxmt9c_device_release(dev);
        if (failureReason) *failureReason = E_OUTOFMEMORY;
        return nullptr;
    }
    if (!device->commandChunkReady()) {
        delete device;
        return nullptr;
    }
    return device;
}
