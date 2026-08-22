/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex owning TU.
 * The class and its file-local support live in d3d9_pe_device_impl.hpp;
 * this TU exists to define the factory entry point. */

#include "d3d9_pe_device_impl.hpp"

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
                                     DWORD implicitSwapchainFlags) {
    auto* device = new D3D9DeviceImpl(
        dev, pFactory, adapter, deviceType, behaviorFlags, window, extended,
        implicitSwapchainFlags);
    if (!device->commandChunkReady()) {
        delete device;
        return nullptr;
    }
    return device;
}
