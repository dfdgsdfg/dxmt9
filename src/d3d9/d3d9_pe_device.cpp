/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex owning TU.
 * The class and its file-local support live in d3d9_pe_device_impl.hpp;
 * this TU exists to define the factory entry point. */

#include "d3d9_pe_device_impl.hpp"

#include <new>

/* =========================================================================
 * Key function -- QueryInterface is declaration-only in the class header so
 * this hot TU owns the device vtable. Keep its COM behavior unchanged.
 * ========================================================================= */

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::QueryInterface(
    REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DDevice9)) {
        *ppv = static_cast<IDirect3DDevice9*>(this);
        dxmt9DeviceDebugLog("device_query_interface this=%p -> out=%p", this, *ppv);
        AddRef();
        return S_OK;
    }
    if (IsEqualGUID(riid, IID_IDirect3DDevice9Ex)) {
        if (!extended_) {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        *ppv = static_cast<IDirect3DDevice9Ex*>(this);
        dxmt9DeviceDebugLog("device_query_interface_ex this=%p -> out=%p", this, *ppv);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

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
