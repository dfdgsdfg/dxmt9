/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex and resource COM wrappers.
 * All methods delegate to the dxmt9c_* C API from dxmt9/device_c.h. */

#include <atomic>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "d3d9_pe.hpp"
#include "util/com/com_private_data.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

static void dxmt9DeviceDebugLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-device", fmt, args);
    va_end(args);
}

static bool dxmt9LeakStateBlocksEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT_LEAK_STATEBLOCKS");
    return enabled;
}

static bool dxmt9PeStateShadowEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_STATE_SHADOW");
    return enabled;
}

static bool dxmt9PeDrawChunkEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_DRAW_CHUNK");
    return enabled;
}

static HRESULT setPrivateData(dxmt9::util::ComPrivateData& storage,
                              REFGUID guid,
                              const void* data,
                              DWORD size,
                              DWORD flags,
                              const char* label,
                              const void* self) {
    HRESULT hr = D3DERR_INVALIDCALL;
    if ((flags & D3DSPD_IUNKNOWN) != 0) {
        const auto* ifacePtr = reinterpret_cast<IUnknown* const*>(data);
        hr = ifacePtr ? storage.setInterface(guid, *ifacePtr) : D3DERR_INVALIDCALL;
    } else {
        hr = storage.setData(guid, size, data);
    }
    dxmt9DeviceDebugLog("%s_set_private_data this=%p size=%u flags=0x%x -> hr=0x%08x",
                        label, self, (unsigned)size, (unsigned)flags, (unsigned)hr);
    return hr;
}

static HRESULT getPrivateData(dxmt9::util::ComPrivateData& storage,
                              REFGUID guid,
                              void* data,
                              DWORD* size,
                              const char* label,
                              const void* self) {
    UINT localSize = size ? static_cast<UINT>(*size) : 0u;
    HRESULT hr = storage.getData(guid, size ? &localSize : nullptr, data);
    if (size) {
        *size = static_cast<DWORD>(localSize);
    }
    dxmt9DeviceDebugLog("%s_get_private_data this=%p data=%p size=%u -> hr=0x%08x",
                        label, self, data, size ? (unsigned)*size : 0u, (unsigned)hr);
    return hr;
}

static HRESULT freePrivateData(dxmt9::util::ComPrivateData& storage,
                               REFGUID guid,
                               const char* label,
                               const void* self) {
    HRESULT hr = storage.removeData(guid);
    dxmt9DeviceDebugLog("%s_free_private_data this=%p -> hr=0x%08x",
                        label, self, (unsigned)hr);
    return hr;
}

static D9CRect toR(const RECT& r) {
    D9CRect c; c.left = r.left; c.top = r.top;
    c.right = r.right; c.bottom = r.bottom;
    return c;
}

struct D3D9PeRecorderFlush {
    virtual HRESULT FlushPeRecorderForChild() = 0;

protected:
    ~D3D9PeRecorderFlush() = default;
};

static HRESULT flushChildRecorder(D3D9PeRecorderFlush* recorder) {
    return recorder ? recorder->FlushPeRecorderForChild() : S_OK;
}

/* =========================================================================
 * Resource COM wrappers
 * Each wrapper holds a D9C* handle (owns one refcount) and exposes raw()
 * for the device to extract the handle when binding the resource.
 * ========================================================================= */

/* ── Surface ──────────────────────────────────────────────────────────────── */

class D3D9SurfaceImpl final : public IDirect3DSurface9 {
    ULONG               refs_ = 1;
    D9CSurface*         s_;
    IDirect3DDevice9*   device_;
    IUnknown*           container_;
    D3D9PeRecorderFlush* recorder_;
    dxmt9::util::ComPrivateData privateData_{};
public:
    D3D9SurfaceImpl(D9CSurface* s,
                    IDirect3DDevice9* device,
                    IUnknown* container,
                    D3D9PeRecorderFlush* recorder = nullptr)
        : s_(s), device_(device), container_(container), recorder_(recorder) {
        if (device_) device_->AddRef();
        if (container_) container_->AddRef();
    }
    ~D3D9SurfaceImpl() {
        dxmt9c_surface_release(s_);
        if (container_) container_->Release();
        if (device_) device_->Release();
    }

    D9CSurface* raw() const { return s_; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)          ||
            IsEqualGUID(riid, IID_IDirect3DResource9)||
            IsEqualGUID(riid, IID_IDirect3DSurface9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            dxmt9DeviceDebugLog("surface_get_device this=%p -> invalid (device=null)", this);
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        dxmt9DeviceDebugLog("surface_get_device this=%p -> device=%p", this, static_cast<void*>(device_));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) override {
        return setPrivateData(privateData_, guid, data, size, flags, "surface", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) override {
        return getPrivateData(privateData_, guid, data, size, "surface", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        return freePrivateData(privateData_, guid, "surface", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (!container_) { *ppv = nullptr; return D3DERR_INVALIDCALL; }
        return container_->QueryInterface(riid, ppv);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* pD) override {
        if (!pD) return D3DERR_INVALIDCALL;
        D9CSurfaceDesc sd{};
        HRESULT hr = hr32(dxmt9c_surface_get_desc(s_, &sd));
        if (SUCCEEDED(hr)) {
            pD->Format = (D3DFORMAT)sd.format; pD->Type = (D3DRESOURCETYPE)sd.resourceType;
            pD->Usage  = sd.usage;             pD->Pool = (D3DPOOL)sd.pool;
            pD->MultiSampleType    = (D3DMULTISAMPLE_TYPE)sd.multiSampleType;
            pD->MultiSampleQuality = sd.multiSampleQuality;
            pD->Width  = sd.width;             pD->Height = sd.height;
            dxmt9DeviceDebugLog("surface_get_desc this=%p fmt=%u usage=0x%x pool=%u msaa=%u/%u size=%ux%u",
                                this,
                                (unsigned)pD->Format,
                                (unsigned)pD->Usage,
                                (unsigned)pD->Pool,
                                (unsigned)pD->MultiSampleType,
                                (unsigned)pD->MultiSampleQuality,
                                (unsigned)pD->Width,
                                (unsigned)pD->Height);
        } else {
            dxmt9DeviceDebugLog("surface_get_desc this=%p -> hr=0x%08x", this, (unsigned)hr);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* pLR, const RECT* pRect,
                                        DWORD flags) override {
        if (!pLR) return D3DERR_INVALIDCALL;
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        dxmt9DeviceDebugLog("surface_lock_rect surface=%p flags=0x%x rect=%s",
                            this,
                            (unsigned)flags,
                            pRect ? "<custom>" : "<full>");
        D9CLockedRect lr{}; D9CRect cr{};
        if (pRect) cr = toR(*pRect);
        HRESULT hr = hr32(dxmt9c_surface_lock_rect(s_, &lr,
                          pRect ? &cr : nullptr, flags));
        if (SUCCEEDED(hr)) {
            pLR->Pitch = lr.pitch; pLR->pBits = lr.bits;
            dxmt9DeviceDebugLog("surface_lock_rect -> pitch=%ld bits=%p",
                                (long)pLR->Pitch, pLR->pBits);
        } else {
            dxmt9DeviceDebugLog("surface_lock_rect -> hr=0x%08x", (unsigned)hr);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect() override {
        dxmt9DeviceDebugLog("surface_unlock_rect surface=%p", this);
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_surface_unlock_rect(s_));
    }
    HRESULT STDMETHODCALLTYPE GetDC(HDC* phdc) override {
        dxmt9DeviceDebugLog("surface_get_dc surface=%p phdc=%p", this, phdc);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override {
        dxmt9DeviceDebugLog("surface_release_dc surface=%p hdc=%p", this, hdc);
        return E_NOTIMPL;
    }
};

/* ── Texture2D ────────────────────────────────────────────────────────────── */

class D3D9TextureImpl final : public IDirect3DTexture9 {
    ULONG       refs_ = 1;
    D9CTexture* t_;
    IDirect3DDevice9* device_;
    D3D9PeRecorderFlush* recorder_;
    dxmt9::util::ComPrivateData privateData_{};
public:
    D3D9TextureImpl(D9CTexture* t,
                    IDirect3DDevice9* device,
                    D3D9PeRecorderFlush* recorder = nullptr)
        : t_(t), device_(device), recorder_(recorder) {
        if (device_) device_->AddRef();
    }
    ~D3D9TextureImpl() {
        dxmt9c_texture_release(t_);
        if (device_) device_->Release();
    }

    D9CTexture* raw() const { return t_; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)              ||
            IsEqualGUID(riid, IID_IDirect3DResource9)    ||
            IsEqualGUID(riid, IID_IDirect3DBaseTexture9) ||
            IsEqualGUID(riid, IID_IDirect3DTexture9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            dxmt9DeviceDebugLog("texture_get_device this=%p -> invalid (device=null)", this);
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        dxmt9DeviceDebugLog("texture_get_device this=%p -> device=%p", this, static_cast<void*>(device_));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) override {
        return setPrivateData(privateData_, guid, data, size, flags, "texture", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) override {
        return getPrivateData(privateData_, guid, data, size, "texture", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        return freePrivateData(privateData_, guid, "texture", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD()      override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override {
        return dxmt9c_texture_get_level_count(t_);
    }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override { return S_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_LINEAR; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return;
        dxmt9c_texture_generate_mip_sublevels(t_);
    }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* pD) override {
        if (!pD) return D3DERR_INVALIDCALL;
        D9CSurfaceDesc sd{};
        HRESULT hr = hr32(dxmt9c_texture_get_level_desc(t_, level, &sd));
        if (SUCCEEDED(hr)) {
            pD->Format = (D3DFORMAT)sd.format; pD->Type = D3DRTYPE_TEXTURE;
            pD->Usage  = sd.usage;             pD->Pool = (D3DPOOL)sd.pool;
            pD->MultiSampleType    = (D3DMULTISAMPLE_TYPE)sd.multiSampleType;
            pD->MultiSampleQuality = sd.multiSampleQuality;
            pD->Width = sd.width; pD->Height = sd.height;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level,
                                               IDirect3DSurface9** ppS) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("texture_get_surface_level this=%p level=%u", this, level);
        D9CSurface* s = dxmt9c_texture_get_surface_level(t_, level);
        if (!s) {
            dxmt9DeviceDebugLog("texture_get_surface_level this=%p level=%u -> invalid", this, level);
            return D3DERR_INVALIDCALL;
        }
        *ppS = new D3D9SurfaceImpl(s, device_, static_cast<IDirect3DBaseTexture9*>(this), recorder_);
        dxmt9DeviceDebugLog("texture_get_surface_level this=%p level=%u -> surface=%p", this, level, *ppS);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* pLR,
                                        const RECT* pRect, DWORD flags) override {
        if (!pLR) return D3DERR_INVALIDCALL;
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        D9CLockedRect lr{}; D9CRect cr{};
        if (pRect) cr = toR(*pRect);
        HRESULT hr = hr32(dxmt9c_texture_lock_rect(t_, level, &lr,
                          pRect ? &cr : nullptr, flags));
        if (SUCCEEDED(hr)) {
            pLR->Pitch = lr.pitch;
            pLR->pBits = lr.bits;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_texture_unlock_rect(t_, level));
    }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override { return S_OK; }
};

/* ── CubeTexture ──────────────────────────────────────────────────────────── */

class D3D9CubeTextureImpl final : public IDirect3DCubeTexture9 {
    ULONG       refs_ = 1;
    D9CTexture* t_;
    IDirect3DDevice9* device_;
    D3D9PeRecorderFlush* recorder_;
    dxmt9::util::ComPrivateData privateData_{};
public:
    D3D9CubeTextureImpl(D9CTexture* t,
                        IDirect3DDevice9* device,
                        D3D9PeRecorderFlush* recorder = nullptr)
        : t_(t), device_(device), recorder_(recorder) {
        if (device_) device_->AddRef();
    }
    ~D3D9CubeTextureImpl() {
        dxmt9c_texture_release(t_);
        if (device_) device_->Release();
    }

    D9CTexture* raw() const { return t_; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)               ||
            IsEqualGUID(riid, IID_IDirect3DResource9)     ||
            IsEqualGUID(riid, IID_IDirect3DBaseTexture9)  ||
            IsEqualGUID(riid, IID_IDirect3DCubeTexture9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            dxmt9DeviceDebugLog("swapchain_get_device this=%p -> invalid (device=null)", this);
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        dxmt9DeviceDebugLog("swapchain_get_device this=%p -> device=%p", this, static_cast<void*>(device_));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) override {
        return setPrivateData(privateData_, guid, data, size, flags, "cube", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) override {
        return getPrivateData(privateData_, guid, data, size, "cube", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        return freePrivateData(privateData_, guid, "cube", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_CUBETEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD()      override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override {
        return dxmt9c_texture_get_level_count(t_);
    }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override { return S_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_LINEAR; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return;
        dxmt9c_texture_generate_mip_sublevels(t_);
    }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* pD) override {
        if (!pD) return D3DERR_INVALIDCALL;
        D9CSurfaceDesc sd{};
        HRESULT hr = hr32(dxmt9c_texture_get_level_desc(t_, level, &sd));
        if (SUCCEEDED(hr)) {
            pD->Format = (D3DFORMAT)sd.format; pD->Type = D3DRTYPE_CUBETEXTURE;
            pD->Usage  = sd.usage;             pD->Pool = (D3DPOOL)sd.pool;
            pD->MultiSampleType    = (D3DMULTISAMPLE_TYPE)sd.multiSampleType;
            pD->MultiSampleQuality = sd.multiSampleQuality;
            pD->Width = sd.width; pD->Height = sd.height;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level,
                                                 IDirect3DSurface9** ppS) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("cube_get_surface_level this=%p face=%u level=%u",
                            this, static_cast<unsigned>(face), level);
        UINT idx = (UINT)face * dxmt9c_texture_get_level_count(t_) + level;
        D9CSurface* s = dxmt9c_texture_get_surface_level(t_, idx);
        if (!s) {
            dxmt9DeviceDebugLog("cube_get_surface_level this=%p face=%u level=%u -> invalid",
                                this, static_cast<unsigned>(face), level);
            return D3DERR_INVALIDCALL;
        }
        *ppS = new D3D9SurfaceImpl(s, device_, static_cast<IDirect3DBaseTexture9*>(this), recorder_);
        dxmt9DeviceDebugLog("cube_get_surface_level this=%p face=%u level=%u -> surface=%p",
                            this, static_cast<unsigned>(face), level, *ppS);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level,
                                        D3DLOCKED_RECT* pLR, const RECT* pRect,
                                        DWORD flags) override {
        if (!pLR) return D3DERR_INVALIDCALL;
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        UINT idx = (UINT)face * dxmt9c_texture_get_level_count(t_) + level;
        D9CLockedRect lr{}; D9CRect cr{};
        if (pRect) cr = toR(*pRect);
        HRESULT hr = hr32(dxmt9c_texture_lock_rect(t_, idx, &lr,
                          pRect ? &cr : nullptr, flags));
        if (SUCCEEDED(hr)) { pLR->Pitch = lr.pitch; pLR->pBits = lr.bits; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        UINT idx = (UINT)face * dxmt9c_texture_get_level_count(t_) + level;
        return hr32(dxmt9c_texture_unlock_rect(t_, idx));
    }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES, const RECT*) override { return S_OK; }
};

/* ── VolumeTexture ────────────────────────────────────────────────────────── */

class D3D9VolumeTextureImpl final : public IDirect3DVolumeTexture9 {
    ULONG       refs_ = 1;
    D9CTexture* t_;
    IDirect3DDevice9* device_;
    D3D9PeRecorderFlush* recorder_;
    dxmt9::util::ComPrivateData privateData_{};
public:
    D3D9VolumeTextureImpl(D9CTexture* t,
                          IDirect3DDevice9* device,
                          D3D9PeRecorderFlush* recorder = nullptr)
        : t_(t), device_(device), recorder_(recorder) {
        if (device_) device_->AddRef();
    }
    ~D3D9VolumeTextureImpl() {
        dxmt9c_texture_release(t_);
        if (device_) device_->Release();
    }

    D9CTexture* raw() const { return t_; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)                ||
            IsEqualGUID(riid, IID_IDirect3DResource9)      ||
            IsEqualGUID(riid, IID_IDirect3DBaseTexture9)   ||
            IsEqualGUID(riid, IID_IDirect3DVolumeTexture9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            dxmt9DeviceDebugLog("stateblock_get_device this=%p -> invalid (device=null)", this);
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        dxmt9DeviceDebugLog("stateblock_get_device this=%p -> device=%p", this, static_cast<void*>(device_));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) override {
        return setPrivateData(privateData_, guid, data, size, flags, "volume", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) override {
        return getPrivateData(privateData_, guid, data, size, "volume", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        return freePrivateData(privateData_, guid, "volume", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD()      override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override {
        return dxmt9c_texture_get_level_count(t_);
    }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override { return S_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_LINEAR; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return;
        dxmt9c_texture_generate_mip_sublevels(t_);
    }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT, D3DVOLUME_DESC* pD) override {
        if (pD) memset(pD, 0, sizeof(*pD)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT, IDirect3DVolume9**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockBox(UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX*) override { return S_OK; }
};

/* ── VertexBuffer ─────────────────────────────────────────────────────────── */

class D3D9VertexBufferImpl final : public IDirect3DVertexBuffer9 {
    ULONG      refs_ = 1;
    D9CBuffer* b_;
    IDirect3DDevice9* device_;
    D3D9PeRecorderFlush* recorder_;
    dxmt9::util::ComPrivateData privateData_{};
public:
    D3D9VertexBufferImpl(D9CBuffer* b,
                         IDirect3DDevice9* device,
                         D3D9PeRecorderFlush* recorder = nullptr)
        : b_(b), device_(device), recorder_(recorder) {
        if (device_) device_->AddRef();
    }
    ~D3D9VertexBufferImpl() {
        dxmt9c_buffer_release(b_);
        if (device_) device_->Release();
    }

    D9CBuffer* raw() const { return b_; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)              ||
            IsEqualGUID(riid, IID_IDirect3DResource9)    ||
            IsEqualGUID(riid, IID_IDirect3DVertexBuffer9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) override {
        return setPrivateData(privateData_, guid, data, size, flags, "vb", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) override {
        return getPrivateData(privateData_, guid, data, size, "vb", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        return freePrivateData(privateData_, guid, "vb", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void** pp, DWORD flags) override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
    }
    HRESULT STDMETHODCALLTYPE Unlock() override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_buffer_unlock(b_));
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* pDesc) override {
        if (!pDesc) return D3DERR_INVALIDCALL;
        D9CBufferDesc desc{};
        const HRESULT hr = hr32(dxmt9c_buffer_get_desc(b_, &desc));
        if (FAILED(hr)) {
            dxmt9DeviceDebugLog("vb_get_desc vb=%p -> hr=0x%08x", this, (unsigned)hr);
            return hr;
        }
        pDesc->Format = D3DFMT_VERTEXDATA;
        pDesc->Type = D3DRTYPE_VERTEXBUFFER;
        pDesc->Usage = desc.usage;
        pDesc->Pool = static_cast<D3DPOOL>(desc.pool);
        pDesc->Size = desc.size;
        pDesc->FVF = desc.fvf;
        dxmt9DeviceDebugLog("vb_get_desc vb=%p -> size=%u usage=0x%x pool=%u fvf=0x%x",
                            this, desc.size, desc.usage, desc.pool, desc.fvf);
        return S_OK;
    }
};

/* ── IndexBuffer ──────────────────────────────────────────────────────────── */

class D3D9IndexBufferImpl final : public IDirect3DIndexBuffer9 {
    ULONG      refs_ = 1;
    D9CBuffer* b_;
    IDirect3DDevice9* device_;
    D3D9PeRecorderFlush* recorder_;
    dxmt9::util::ComPrivateData privateData_{};
public:
    D3D9IndexBufferImpl(D9CBuffer* b,
                        IDirect3DDevice9* device,
                        D3D9PeRecorderFlush* recorder = nullptr)
        : b_(b), device_(device), recorder_(recorder) {
        if (device_) device_->AddRef();
    }
    ~D3D9IndexBufferImpl() {
        dxmt9c_buffer_release(b_);
        if (device_) device_->Release();
    }

    D9CBuffer* raw() const { return b_; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)             ||
            IsEqualGUID(riid, IID_IDirect3DResource9)   ||
            IsEqualGUID(riid, IID_IDirect3DIndexBuffer9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) override {
        return setPrivateData(privateData_, guid, data, size, flags, "ib", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) override {
        return getPrivateData(privateData_, guid, data, size, "ib", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        return freePrivateData(privateData_, guid, "ib", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void** pp, DWORD flags) override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
    }
    HRESULT STDMETHODCALLTYPE Unlock() override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_buffer_unlock(b_));
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* pDesc) override {
        if (!pDesc) return D3DERR_INVALIDCALL;
        D9CBufferDesc desc{};
        const HRESULT hr = hr32(dxmt9c_buffer_get_desc(b_, &desc));
        if (FAILED(hr)) {
            dxmt9DeviceDebugLog("ib_get_desc ib=%p -> hr=0x%08x", this, (unsigned)hr);
            return hr;
        }
        pDesc->Format = static_cast<D3DFORMAT>(desc.format);
        pDesc->Type = D3DRTYPE_INDEXBUFFER;
        pDesc->Usage = desc.usage;
        pDesc->Pool = static_cast<D3DPOOL>(desc.pool);
        pDesc->Size = desc.size;
        dxmt9DeviceDebugLog("ib_get_desc ib=%p -> size=%u usage=0x%x pool=%u fmt=%u",
                            this, desc.size, desc.usage, desc.pool, desc.format);
        return S_OK;
    }
};

/* ── VertexShader ─────────────────────────────────────────────────────────── */

class D3D9VertexShaderImpl final : public IDirect3DVertexShader9 {
    ULONG      refs_ = 1;
    D9CShader* s_;
    IDirect3DDevice9* device_;
public:
    D3D9VertexShaderImpl(D9CShader* s, IDirect3DDevice9* device) : s_(s), device_(device) {
        if (device_) device_->AddRef();
    }
    ~D3D9VertexShaderImpl() {
        dxmt9c_shader_release(s_);
        if (device_) device_->Release();
    }

    D9CShader* raw() const { return s_; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DVertexShader9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFunction(void* pData, UINT* pSize) override {
        if (!pSize) return D3DERR_INVALIDCALL;
        return hr32(dxmt9c_shader_get_bytecode(s_, pData, pSize));
    }
};

/* ── PixelShader ──────────────────────────────────────────────────────────── */

class D3D9PixelShaderImpl final : public IDirect3DPixelShader9 {
    ULONG      refs_ = 1;
    D9CShader* s_;
    IDirect3DDevice9* device_;
public:
    D3D9PixelShaderImpl(D9CShader* s, IDirect3DDevice9* device) : s_(s), device_(device) {
        if (device_) device_->AddRef();
    }
    ~D3D9PixelShaderImpl() {
        dxmt9c_shader_release(s_);
        if (device_) device_->Release();
    }

    D9CShader* raw() const { return s_; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DPixelShader9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFunction(void* pData, UINT* pSize) override {
        if (!pSize) return D3DERR_INVALIDCALL;
        return hr32(dxmt9c_shader_get_bytecode(s_, pData, pSize));
    }
};

/* ── VertexDeclaration ────────────────────────────────────────────────────── */

class D3D9VertexDeclImpl final : public IDirect3DVertexDeclaration9 {
    ULONG          refs_ = 1;
    D9CVertexDecl* d_;
    IDirect3DDevice9* device_;
public:
    D3D9VertexDeclImpl(D9CVertexDecl* d, IDirect3DDevice9* device) : d_(d), device_(device) {
        if (device_) device_->AddRef();
    }
    ~D3D9VertexDeclImpl() {
        dxmt9c_vdecl_release(d_);
        if (device_) device_->Release();
    }

    D9CVertexDecl* raw() const { return d_; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DVertexDeclaration9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9* pE,
                                              UINT* pCount) override {
        if (!pCount) return D3DERR_INVALIDCALL;
        D9CVertexElement tmp[64]{};
        HRESULT hr = hr32(dxmt9c_vdecl_get_declaration(d_, tmp, pCount));
        if (SUCCEEDED(hr) && pE) {
            for (UINT i = 0; i < *pCount; ++i) {
                pE[i].Stream = tmp[i].stream; pE[i].Offset = tmp[i].offset;
                pE[i].Type   = tmp[i].type;   pE[i].Method = tmp[i].method;
                pE[i].Usage  = tmp[i].usage;  pE[i].UsageIndex = tmp[i].usageIndex;
            }
        }
        return hr;
    }
};

/* ── Query ────────────────────────────────────────────────────────────────── */

class D3D9QueryImpl final : public IDirect3DQuery9 {
    ULONG    refs_ = 1;
    D9CQuery* q_;
    IDirect3DDevice9* device_;
    D3D9PeRecorderFlush* recorder_;
public:
    D3D9QueryImpl(D9CQuery* q,
                  IDirect3DDevice9* device,
                  D3D9PeRecorderFlush* recorder = nullptr)
        : q_(q), device_(device), recorder_(recorder) {
        if (device_) device_->AddRef();
    }
    ~D3D9QueryImpl() {
        dxmt9c_query_release(q_);
        if (device_) device_->Release();
    }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DQuery9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    D3DQUERYTYPE STDMETHODCALLTYPE GetType()     override { return (D3DQUERYTYPE)dxmt9c_query_get_type(q_); }
    DWORD        STDMETHODCALLTYPE GetDataSize()  override { return dxmt9c_query_get_data_size(q_); }
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_query_issue(q_, flags));
    }
    HRESULT STDMETHODCALLTYPE GetData(void* pData, DWORD size, DWORD flags) override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_query_get_data(q_, pData, size, flags));
    }
};

/* ── StateBlock ───────────────────────────────────────────────────────────── */

class D3D9StateBlockImpl final : public IDirect3DStateBlock9 {
    std::atomic<ULONG> refs_{1};
    D9CStateBlock* sb_;
    IDirect3DDevice9* device_;
    D3D9PeRecorderFlush* recorder_;
public:
    D3D9StateBlockImpl(D9CStateBlock* sb,
                       IDirect3DDevice9* device,
                       D3D9PeRecorderFlush* recorder = nullptr)
        : sb_(sb), device_(device), recorder_(recorder) {
        if (device_) device_->AddRef();
        dxmt9DeviceDebugLog("stateblock_ctor this=%p sb=%p device=%p refs=%u",
                            this, static_cast<void*>(sb_), static_cast<void*>(device_),
                            (unsigned)refs_.load());
    }
    ~D3D9StateBlockImpl() {
        dxmt9DeviceDebugLog("stateblock_dtor this=%p sb=%p device=%p leak=%u",
                            this, static_cast<void*>(sb_), static_cast<void*>(device_),
                            dxmt9LeakStateBlocksEnabled() ? 1u : 0u);
        if (sb_ && !dxmt9LeakStateBlocksEnabled()) {
            dxmt9c_stateblock_release(sb_);
        }
        sb_ = nullptr;
        if (device_) device_->Release();
        device_ = nullptr;
    }

    ULONG STDMETHODCALLTYPE AddRef()  override {
        const ULONG refs = refs_.fetch_add(1) + 1;
        dxmt9DeviceDebugLog("stateblock_addref this=%p sb=%p refs=%u",
                            this, static_cast<void*>(sb_), (unsigned)refs);
        return refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = refs_.fetch_sub(1) - 1;
        dxmt9DeviceDebugLog("stateblock_release this=%p sb=%p refs=%u",
                            this, static_cast<void*>(sb_), (unsigned)refs);
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DStateBlock9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Capture() override {
        dxmt9DeviceDebugLog("stateblock_capture sb=%p", this);
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_stateblock_capture(sb_));
        dxmt9DeviceDebugLog("stateblock_capture -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Apply() override {
        dxmt9DeviceDebugLog("stateblock_apply sb=%p", this);
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_stateblock_apply(sb_));
        dxmt9DeviceDebugLog("stateblock_apply -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
};

/* ── SwapChain ────────────────────────────────────────────────────────────── */

class D3D9SwapChainImpl final : public IDirect3DSwapChain9 {
    ULONG        refs_ = 1;
    D9CSwapChain* sc_;
    IDirect3DDevice9* device_;
    D3D9PeRecorderFlush* recorder_;
public:
    D3D9SwapChainImpl(D9CSwapChain* sc,
                      IDirect3DDevice9* device,
                      D3D9PeRecorderFlush* recorder = nullptr)
        : sc_(sc), device_(device), recorder_(recorder) {
        if (device_) device_->AddRef();
    }
    ~D3D9SwapChainImpl() {
        dxmt9c_swapchain_release(sc_);
        if (device_) device_->Release();
    }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DSwapChain9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty,
                                       DWORD flags) override {
        dxmt9DeviceDebugLog("swapchain_present sc=%p wnd=%p flags=0x%x src=%s dst=%s dirty=%p",
                            this, wnd, (unsigned)flags,
                            src ? "<custom>" : "<full>",
                            dst ? "<custom>" : "<full>",
                            dirty);
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_swapchain_present(sc_,
            src ? &cs : nullptr, dst ? &cd : nullptr,
            (uint64_t)(uintptr_t)wnd, dirty, flags));
    }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9*) override {
        return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT idx, D3DBACKBUFFER_TYPE,
                                             IDirect3DSurface9** ppS) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("swapchain_get_back_buffer sc=%p idx=%u", this, idx);
        D9CSurface* s = dxmt9c_swapchain_get_back_buffer(sc_, idx, 0);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, device_, static_cast<IDirect3DSwapChain9*>(this), recorder_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* p) override {
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* p) override {
        if (!p) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("swapchain_get_display_mode sc=%p", this);
        D9CPresentParams cpp{};
        const HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(sc_, &cpp));
        if (FAILED(hr)) {
            dxmt9DeviceDebugLog("swapchain_get_display_mode -> hr=0x%08x", (unsigned)hr);
            return hr;
        }
        p->Width = cpp.backBufferWidth;
        p->Height = cpp.backBufferHeight;
        p->RefreshRate = cpp.fullScreenRefreshRateHz;
        p->Format = static_cast<D3DFORMAT>(cpp.backBufferFormat);
        dxmt9DeviceDebugLog("swapchain_get_display_mode -> %ux%u fmt=%u hz=%u",
                            p->Width, p->Height, (unsigned)p->Format, p->RefreshRate);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* pPP) override {
        if (!pPP) return D3DERR_INVALIDCALL;
        D9CPresentParams cpp{};
        HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(sc_, &cpp));
        if (SUCCEEDED(hr)) {
            pPP->BackBufferWidth        = cpp.backBufferWidth;
            pPP->BackBufferHeight       = cpp.backBufferHeight;
            pPP->BackBufferFormat       = (D3DFORMAT)cpp.backBufferFormat;
            pPP->BackBufferCount        = cpp.backBufferCount;
            pPP->MultiSampleType        = (D3DMULTISAMPLE_TYPE)cpp.multiSampleType;
            pPP->MultiSampleQuality     = cpp.multiSampleQuality;
            pPP->SwapEffect             = (D3DSWAPEFFECT)cpp.swapEffect;
            pPP->hDeviceWindow          = (HWND)(uintptr_t)cpp.deviceWindow;
            pPP->Windowed               = cpp.windowed ? TRUE : FALSE;
            pPP->EnableAutoDepthStencil = cpp.enableAutoDepthStencil ? TRUE : FALSE;
            pPP->AutoDepthStencilFormat = (D3DFORMAT)cpp.autoDepthStencilFormat;
            pPP->Flags                  = cpp.flags;
            pPP->FullScreen_RefreshRateInHz = cpp.fullScreenRefreshRateHz;
            pPP->PresentationInterval   = cpp.presentationInterval;
        }
        return hr;
    }
};

/* =========================================================================
 * Raw-handle extractors — safe because only our device creates these objects.
 * ========================================================================= */

static D9CSurface*   rawSurf(IDirect3DSurface9* p)          { return p ? static_cast<D3D9SurfaceImpl*>(p)->raw()     : nullptr; }
static D9CBuffer*    rawVBuf(IDirect3DVertexBuffer9* p)      { return p ? static_cast<D3D9VertexBufferImpl*>(p)->raw() : nullptr; }
static D9CBuffer*    rawIBuf(IDirect3DIndexBuffer9* p)       { return p ? static_cast<D3D9IndexBufferImpl*>(p)->raw()  : nullptr; }
static D9CShader*    rawVS(IDirect3DVertexShader9* p)        { return p ? static_cast<D3D9VertexShaderImpl*>(p)->raw() : nullptr; }
static D9CShader*    rawPS(IDirect3DPixelShader9* p)         { return p ? static_cast<D3D9PixelShaderImpl*>(p)->raw()  : nullptr; }
static D9CVertexDecl* rawVD(IDirect3DVertexDeclaration9* p)  { return p ? static_cast<D3D9VertexDeclImpl*>(p)->raw()  : nullptr; }

static D9CTexture* rawTex(IDirect3DBaseTexture9* p) {
    if (!p) return nullptr;
    switch (p->GetType()) {
    case D3DRTYPE_TEXTURE:       return static_cast<D3D9TextureImpl*>(p)->raw();
    case D3DRTYPE_CUBETEXTURE:   return static_cast<D3D9CubeTextureImpl*>(p)->raw();
    case D3DRTYPE_VOLUMETEXTURE: return static_cast<D3D9VolumeTextureImpl*>(p)->raw();
    default: return nullptr;
    }
}

static D9CWireHandle toWireHandle(const void* handle) {
    const auto value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    return D9CWireHandle{
        static_cast<uint32_t>(value & 0xffffffffull),
        static_cast<uint32_t>(value >> 32),
    };
}

/* =========================================================================
 * D3D9DeviceImpl — IDirect3DDevice9Ex
 * ========================================================================= */

class D3D9DeviceImpl final : public IDirect3DDevice9Ex, public D3D9PeRecorderFlush {
    static constexpr UINT kMaxPendingCommandRecords = 64;
    static constexpr size_t kMaxPendingCommandBytes = 256 * 1024;

    ULONG        refs_    = 1;
    D9CDevice*   dev_;
    IDirect3D9Ex* factory_; /* borrowed, no AddRef — factory owns device */
    UINT         adapter_ = 0;
    DWORD        behaviorFlags_ = 0;

    /* bound resource tracking (AddRef'd) */
    IDirect3DBaseTexture9*     textures_[16]    = {};
    IDirect3DVertexShader9*    vs_              = nullptr;
    IDirect3DPixelShader9*     ps_              = nullptr;
    IDirect3DVertexBuffer9*    streamSrc_[16]   = {};
    UINT                       streamOff_[16]   = {};
    UINT                       streamStr_[16]   = {};
    UINT                       streamFreq_[16]  = {};
    IDirect3DIndexBuffer9*     indexBuf_        = nullptr;
    IDirect3DVertexDeclaration9* vdecl_         = nullptr;
    DWORD                      fvf_             = 0;

    std::unordered_map<DWORD, DWORD> renderStateShadow_{};
    std::unordered_map<DWORD, DWORD> pendingRenderStates_{};
    DWORD pendingTextureMask_ = 0;
    DWORD pendingStreamMask_ = 0;
    bool pendingFvf_ = false;
    std::vector<std::uint8_t> pendingCommandBytes_{};
    UINT pendingCommandRecordCount_ = 0;

    // Shader-constant shadow + dirty range per (stage, type). Per the
    // recorder design, Set*ShaderConstant* updates the shadow + extends
    // the dirty range; the chunk does not get a record per Set call.
    // Just before each appended Draw record (and at chunk-flush) the
    // dirty range for each (stage, type) is emitted as ONE
    // D9C_COMMAND_RECORD_SET_*_CONST_* covering [start, end) — so a
    // shader pushing 30 individual SetVsConstF calls between two draws
    // costs 1 record (merged range), not 30.
    struct ConstShadow {
      std::vector<std::uint8_t> values; // raw bytes; size = (slotsTouched * elemSize)
      uint32_t dirtyStart = 0;
      uint32_t dirtyEnd = 0;             // [start, end), end > start ⇒ dirty
      bool dirty() const { return dirtyEnd > dirtyStart; }
      void clear() { dirtyStart = dirtyEnd = 0; }
    };
    ConstShadow vsConstF_{};
    ConstShadow vsConstI_{};
    ConstShadow vsConstB_{};
    ConstShadow psConstF_{};
    ConstShadow psConstI_{};
    ConstShadow psConstB_{};

    // Per-chunk resource retention set, one dedup'd container per kind.
    // Populated by the PE-side Set{Texture,StreamSource,Indices,
    // RenderTarget,DepthStencil} fast paths; serialized into
    // D9CCommandChunk.handles[] at commit_chunk; cleared after a
    // successful commit.
    std::unordered_set<uint64_t> chunkHandlesByKind_[5]{};
    std::vector<D9CChunkHandleEntry> chunkHandlesPayload_{};

    /* present params copy for GetCreationParameters */
    HWND creationWindow_ = nullptr;

    template<typename T>
    static void setRef(T*& slot, T* newVal) {
        if (newVal) newVal->AddRef();
        if (slot)   slot->Release();
        slot = newVal;
    }

    void releaseAllBound() {
        for (auto& t : textures_)   setRef(t, (IDirect3DBaseTexture9*)nullptr);
        setRef(vs_, (IDirect3DVertexShader9*)nullptr);
        setRef(ps_, (IDirect3DPixelShader9*)nullptr);
        for (auto& s : streamSrc_)  setRef(s, (IDirect3DVertexBuffer9*)nullptr);
        setRef(indexBuf_, (IDirect3DIndexBuffer9*)nullptr);
        setRef(vdecl_, (IDirect3DVertexDeclaration9*)nullptr);
    }

    void clearPeStateTracking() {
        renderStateShadow_.clear();
        clearPendingHotState();
        pendingCommandBytes_.clear();
        pendingCommandRecordCount_ = 0;
        fvf_ = 0;
        std::memset(streamOff_, 0, sizeof(streamOff_));
        std::memset(streamStr_, 0, sizeof(streamStr_));
        std::memset(streamFreq_, 0, sizeof(streamFreq_));
    }

    bool hasPendingHotState() const {
        return !pendingRenderStates_.empty() || pendingTextureMask_ != 0 ||
               pendingStreamMask_ != 0 || pendingFvf_;
    }

    void clearPendingHotState() {
        pendingRenderStates_.clear();
        pendingTextureMask_ = 0;
        pendingStreamMask_ = 0;
        pendingFvf_ = false;
    }

    bool shadowedRenderStateEquals(DWORD state, DWORD value) const {
        const auto it = renderStateShadow_.find(state);
        return it != renderStateShadow_.end() && it->second == value;
    }

    bool shadowedTextureEquals(DWORD stage, IDirect3DBaseTexture9* texture) const {
        return stage < 16 && textures_[stage] == texture;
    }

    bool shadowedStreamSourceEquals(UINT stream,
                                    IDirect3DVertexBuffer9* buffer,
                                    UINT offset,
                                    UINT stride) const {
        return stream < 16 && streamSrc_[stream] == buffer &&
               streamOff_[stream] == offset && streamStr_[stream] == stride;
    }

    bool buildDrawPrimitivePacket(D3DPRIMITIVETYPE type,
                                  UINT startVertex,
                                  UINT count,
                                  D9CDrawPrimitivePacket& packet) const {
        if (pendingRenderStates_.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            return false;
        }

        packet = D9CDrawPrimitivePacket{};
        for (const auto& [state, value] : pendingRenderStates_) {
            auto& entry = packet.renderStates[packet.renderStateCount++];
            entry.state = state;
            entry.value = value;
        }

        packet.textureMask = pendingTextureMask_;
        for (DWORD stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
            if ((pendingTextureMask_ & (1u << stage)) != 0) {
                packet.textures[stage] = toWireHandle(rawTex(textures_[stage]));
            }
        }

        packet.streamSourceMask = pendingStreamMask_;
        for (DWORD stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            if ((pendingStreamMask_ & (1u << stream)) == 0) {
                continue;
            }
            auto& source = packet.streamSources[stream];
            source.buffer = toWireHandle(rawVBuf(streamSrc_[stream]));
            source.offset = streamOff_[stream];
            source.stride = streamStr_[stream];
        }

        packet.fvfValid = pendingFvf_ ? 1u : 0u;
        packet.fvf = fvf_;
        packet.primitiveType = static_cast<uint32_t>(type);
        packet.startVertex = startVertex;
        packet.primitiveCount = count;
        return true;
    }

    static UINT primitiveVertexCount(D3DPRIMITIVETYPE type, UINT primitiveCount) {
        switch (type) {
        case D3DPT_POINTLIST: return primitiveCount;
        case D3DPT_LINELIST: return primitiveCount * 2u;
        case D3DPT_LINESTRIP: return primitiveCount + 1u;
        case D3DPT_TRIANGLELIST: return primitiveCount * 3u;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return primitiveCount + 2u;
        default: return 0;
        }
    }

    static bool checkedByteCount(UINT count, UINT stride, std::uint32_t& bytes) {
        const auto value = static_cast<std::uint64_t>(count) * stride;
        if (value > 0xffffffffull) {
            return false;
        }
        bytes = static_cast<std::uint32_t>(value);
        return true;
    }

    // Add a (kind, handle) to the per-chunk dedup'd retention set. Called
    // from Set{Texture,StreamSource,Indices,RenderTarget,DepthStencil,
    // VertexShader,PixelShader,VertexDeclaration} fast paths whenever a
    // resource handle is bound. Cheap O(1) hash insert; serialization
    // happens once per chunk at flush time.
    void noteChunkHandle(uint32_t kind, uint64_t handle) {
        if (!dxmt9PeDrawChunkEnabled() || handle == 0 || kind > 4) {
            return;
        }
        chunkHandlesByKind_[kind].insert(handle);
    }

    HRESULT flushPendingCommandChunk() {
        if (!dxmt9PeDrawChunkEnabled() || pendingCommandRecordCount_ == 0) {
            return S_OK;
        }
        // Serialize the deduped retention set into a packed payload the
        // server-side importer can iterate in one pass. Order isn't
        // semantically meaningful — server treats the list as an
        // unordered set of (kind, handle) pairs.
        chunkHandlesPayload_.clear();
        std::uint32_t totalHandles = 0;
        for (uint32_t kind = 0; kind < 5; ++kind) {
            totalHandles += static_cast<std::uint32_t>(chunkHandlesByKind_[kind].size());
        }
        chunkHandlesPayload_.reserve(totalHandles);
        for (uint32_t kind = 0; kind < 5; ++kind) {
            for (auto handle : chunkHandlesByKind_[kind]) {
                chunkHandlesPayload_.push_back(D9CChunkHandleEntry{
                    .kind = kind,
                    .reserved = 0,
                    .handle = handle,
                });
            }
        }

        D9CCommandChunk chunk{};
        chunk.version = D9C_COMMAND_CHUNK_VERSION;
        chunk.recordCount = pendingCommandRecordCount_;
        chunk.recordBytes = static_cast<std::uint32_t>(pendingCommandBytes_.size());
        chunk.records = toWireHandle(pendingCommandBytes_.data());
        chunk.handleCount = static_cast<std::uint32_t>(chunkHandlesPayload_.size());
        chunk.handles = chunk.handleCount != 0
                            ? toWireHandle(chunkHandlesPayload_.data())
                            : D9CWireHandle{};

        const HRESULT hr = hr32(dxmt9c_device_commit_chunk(dev_, &chunk));
        if (SUCCEEDED(hr)) {
            pendingCommandBytes_.clear();
            pendingCommandRecordCount_ = 0;
            for (auto& set : chunkHandlesByKind_) {
                set.clear();
            }
        }
        return hr;
    }

    HRESULT appendCommandRecord(const void* data, size_t bytes) {
        if (!dxmt9PeDrawChunkEnabled() || !data || bytes == 0 || bytes > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        if (pendingCommandRecordCount_ != 0 &&
            (pendingCommandRecordCount_ >= kMaxPendingCommandRecords ||
             pendingCommandBytes_.size() + bytes > kMaxPendingCommandBytes)) {
            const HRESULT flushHr = flushPendingCommandChunk();
            if (FAILED(flushHr)) return flushHr;
        }

        const auto* raw = static_cast<const std::uint8_t*>(data);
        pendingCommandBytes_.insert(pendingCommandBytes_.end(), raw, raw + bytes);
        ++pendingCommandRecordCount_;

        if (pendingCommandRecordCount_ >= kMaxPendingCommandRecords ||
            pendingCommandBytes_.size() >= kMaxPendingCommandBytes) {
            return flushPendingCommandChunk();
        }
        return S_OK;
    }

    HRESULT appendDrawPrimitiveRecord(D3DPRIMITIVETYPE type, UINT startVertex, UINT count) {
        // Drain any accumulated const dirty ranges into chunk records FIRST,
        // so the chunk replays "consts → draw" in API order.
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawPrimitive record{};
        record.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
        record.header.size = sizeof(record);
        if (!buildDrawPrimitivePacket(type, startVertex, count, record.packet)) {
            return D3DERR_INVALIDCALL;
        }
        return appendCommandRecord(&record, sizeof(record));
    }

    HRESULT appendDrawIndexedPrimitiveRecord(D3DPRIMITIVETYPE type,
                                             INT baseVertex,
                                             UINT minVertex,
                                             UINT numVertices,
                                             UINT startIndex,
                                             UINT count) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawIndexedPrimitive record{};
        record.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
        record.header.size = sizeof(record);
        if (!buildDrawPrimitivePacket(type, 0, count, record.packet.state)) {
            return D3DERR_INVALIDCALL;
        }
        record.packet.baseVertex = baseVertex;
        record.packet.minVertex = minVertex;
        record.packet.numVertices = numVertices;
        record.packet.startIndex = startIndex;
        record.packet.primitiveCount = count;
        return appendCommandRecord(&record, sizeof(record));
    }

    HRESULT appendDrawPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                        UINT count,
                                        const void* data,
                                        UINT stride) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawPrimitiveUP header{};
        header.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
        if (!buildDrawPrimitivePacket(type, 0, count, header.packet.state)) {
            return D3DERR_INVALIDCALL;
        }

        std::uint32_t vertexBytes = 0;
        if (!checkedByteCount(primitiveVertexCount(type, count), stride, vertexBytes) ||
            (vertexBytes != 0 && !data)) {
            return D3DERR_INVALIDCALL;
        }
        header.packet.primitiveCount = count;
        header.packet.stride = stride;
        header.packet.vertexDataOffset = sizeof(D9CCommandRecordDrawPrimitiveUP);
        header.packet.vertexDataSize = vertexBytes;
        header.header.size = sizeof(D9CCommandRecordDrawPrimitiveUP) + vertexBytes;

        std::vector<std::uint8_t> record(header.header.size);
        std::memcpy(record.data(), &header, sizeof(header));
        if (vertexBytes != 0) {
            std::memcpy(record.data() + header.packet.vertexDataOffset, data, vertexBytes);
        }
        return appendCommandRecord(record.data(), record.size());
    }

    HRESULT appendDrawIndexedPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                               UINT minVertex,
                                               UINT numVertices,
                                               UINT count,
                                               const void* indexData,
                                               D3DFORMAT indexFormat,
                                               const void* vertexData,
                                               UINT stride) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawIndexedPrimitiveUP header{};
        header.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
        if (!buildDrawPrimitivePacket(type, 0, count, header.packet.state)) {
            return D3DERR_INVALIDCALL;
        }

        const UINT indexSize = indexFormat == D3DFMT_INDEX32 ? 4u : 2u;
        std::uint32_t indexBytes = 0;
        std::uint32_t vertexBytes = 0;
        if (minVertex > 0xffffffffu - numVertices) {
            return D3DERR_INVALIDCALL;
        }
        if (!checkedByteCount(primitiveVertexCount(type, count), indexSize, indexBytes) ||
            !checkedByteCount(minVertex + numVertices, stride, vertexBytes) ||
            (indexBytes != 0 && !indexData) ||
            (vertexBytes != 0 && !vertexData)) {
            return D3DERR_INVALIDCALL;
        }

        header.packet.minVertex = minVertex;
        header.packet.numVertices = numVertices;
        header.packet.primitiveCount = count;
        header.packet.indexFormat = static_cast<std::uint32_t>(indexFormat);
        header.packet.stride = stride;
        header.packet.indexDataOffset = sizeof(D9CCommandRecordDrawIndexedPrimitiveUP);
        header.packet.indexDataSize = indexBytes;
        header.packet.vertexDataOffset = header.packet.indexDataOffset + indexBytes;
        header.packet.vertexDataSize = vertexBytes;
        header.header.size = sizeof(D9CCommandRecordDrawIndexedPrimitiveUP) +
                             indexBytes + vertexBytes;

        std::vector<std::uint8_t> record(header.header.size);
        std::memcpy(record.data(), &header, sizeof(header));
        if (indexBytes != 0) {
            std::memcpy(record.data() + header.packet.indexDataOffset, indexData, indexBytes);
        }
        if (vertexBytes != 0) {
            std::memcpy(record.data() + header.packet.vertexDataOffset, vertexData, vertexBytes);
        }
        return appendCommandRecord(record.data(), record.size());
    }

    HRESULT flushPeRecorder() {
        // Drain const dirty ranges first so they make it into the chunk
        // before we commit it to the bridge; otherwise they'd carry
        // across to the next chunk's first draw with stale ordering.
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        const HRESULT chunkHr = flushPendingCommandChunk();
        if (FAILED(chunkHr)) {
            return chunkHr;
        }
        return flushPendingHotState();
    }

    // Variable-size const-array record append. The record is
    // header + (start, count) + count * elemSize bytes of payload.
    // Caller must supply the matching D9C_COMMAND_RECORD_SET_*_CONST_*
    // type tag; decoder will validate header.size against count*elemSize.
    HRESULT appendSetConstRecord(uint32_t recordType, UINT start, UINT count,
                                 const void* data, std::size_t elemSize) {
        const std::uint64_t payload64 = static_cast<std::uint64_t>(count) * elemSize;
        if (payload64 > 0xffffffffull - sizeof(D9CCommandRecordSetConst)) {
            return D3DERR_INVALIDCALL;
        }
        const std::uint32_t payloadBytes = static_cast<std::uint32_t>(payload64);
        if (payloadBytes != 0 && !data) {
            return D3DERR_INVALIDCALL;
        }

        D9CCommandRecordSetConst header{};
        header.header.type = recordType;
        header.header.size = static_cast<std::uint32_t>(sizeof(header) + payloadBytes);
        header.start = start;
        header.count = count;

        std::vector<std::uint8_t> record(header.header.size);
        std::memcpy(record.data(), &header, sizeof(header));
        if (payloadBytes != 0) {
            std::memcpy(record.data() + sizeof(header), data, payloadBytes);
        }
        return appendCommandRecord(record.data(), record.size());
    }

    // Update a const-shadow with new values + extend the dirty range.
    // No record is appended yet — flushed at the next draw / chunk flush.
    static void touchConstShadow(ConstShadow& shadow, UINT start, UINT count,
                                 const void* data, std::size_t elemSize) {
        const std::uint64_t needed64 = (static_cast<std::uint64_t>(start) + count) * elemSize;
        if (needed64 > 0xffffffffull) {
            return;  // out of range — falls through to legacy path elsewhere
        }
        const std::size_t needed = static_cast<std::size_t>(needed64);
        if (shadow.values.size() < needed) {
            shadow.values.resize(needed);
        }
        if (count > 0 && data) {
            std::memcpy(shadow.values.data() + start * elemSize, data, count * elemSize);
        }
        const uint32_t end = start + count;
        if (!shadow.dirty()) {
            shadow.dirtyStart = start;
            shadow.dirtyEnd = end;
        } else {
            shadow.dirtyStart = std::min<uint32_t>(shadow.dirtyStart, start);
            shadow.dirtyEnd = std::max<uint32_t>(shadow.dirtyEnd, end);
        }
    }

    // Emit one record covering the merged dirty range, then clear it.
    HRESULT flushConstShadow(ConstShadow& shadow, uint32_t recordType, std::size_t elemSize) {
        if (!shadow.dirty()) return S_OK;
        const uint32_t start = shadow.dirtyStart;
        const uint32_t count = shadow.dirtyEnd - shadow.dirtyStart;
        const auto* data = shadow.values.data() + static_cast<std::size_t>(start) * elemSize;
        const HRESULT hr = appendSetConstRecord(recordType, start, count, data, elemSize);
        shadow.clear();
        return hr;
    }

    // Drain all 6 const shadows. Called before each appended Draw record
    // and at chunk flush so the chunk's record stream replays
    // constants → draw in API order.
    HRESULT flushPendingConsts() {
        HRESULT hr = flushConstShadow(vsConstF_, D9C_COMMAND_RECORD_SET_VS_CONST_F, sizeof(float) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(vsConstI_, D9C_COMMAND_RECORD_SET_VS_CONST_I, sizeof(int32_t) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(vsConstB_, D9C_COMMAND_RECORD_SET_VS_CONST_B, sizeof(uint32_t));
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(psConstF_, D9C_COMMAND_RECORD_SET_PS_CONST_F, sizeof(float) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(psConstI_, D9C_COMMAND_RECORD_SET_PS_CONST_I, sizeof(int32_t) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(psConstB_, D9C_COMMAND_RECORD_SET_PS_CONST_B, sizeof(uint32_t));
        if (FAILED(hr)) return hr;
        return S_OK;
    }

    HRESULT flushPendingHotState() {
        const HRESULT chunkHr = flushPendingCommandChunk();
        if (FAILED(chunkHr)) {
            return chunkHr;
        }
        if (!dxmt9PeStateShadowEnabled() || !hasPendingHotState()) {
            return S_OK;
        }

        for (const auto& [state, value] : pendingRenderStates_) {
            const HRESULT hr = hr32(dxmt9c_device_set_render_state(dev_, state, value));
            if (FAILED(hr)) {
                return hr;
            }
        }
        pendingRenderStates_.clear();

        for (DWORD stage = 0; stage < 16; ++stage) {
            if ((pendingTextureMask_ & (1u << stage)) == 0) {
                continue;
            }
            const HRESULT hr = hr32(dxmt9c_device_set_texture(dev_, stage, rawTex(textures_[stage])));
            if (FAILED(hr)) {
                return hr;
            }
        }
        pendingTextureMask_ = 0;

        for (DWORD stream = 0; stream < 16; ++stream) {
            if ((pendingStreamMask_ & (1u << stream)) == 0) {
                continue;
            }
            const HRESULT hr = hr32(dxmt9c_device_set_stream_source(dev_,
                                                                    stream,
                                                                    rawVBuf(streamSrc_[stream]),
                                                                    streamOff_[stream],
                                                                    streamStr_[stream]));
            if (FAILED(hr)) {
                return hr;
            }
        }
        pendingStreamMask_ = 0;

        if (pendingFvf_) {
            const HRESULT hr = hr32(dxmt9c_device_set_fvf(dev_, fvf_));
            if (FAILED(hr)) {
                return hr;
            }
            pendingFvf_ = false;
        }

        return S_OK;
    }

public:
    HRESULT FlushPeRecorderForChild() override {
        return flushPeRecorder();
    }

    D3D9DeviceImpl(D9CDevice* dev, IDirect3D9Ex* factory,
                   UINT adapter, DWORD behaviorFlags, HWND window)
        : dev_(dev), factory_(factory)
        , adapter_(adapter), behaviorFlags_(behaviorFlags)
        , creationWindow_(window) {
        for (UINT& freq : streamFreq_) {
            freq = 1;
        }
        dxmt9DeviceDebugLog("device_ctor this=%p dev=%p factory=%p adapter=%u behavior=0x%x window=%p",
                            this, static_cast<void*>(dev_), static_cast<void*>(factory_),
                            adapter_, (unsigned)behaviorFlags_, window);
    }

    ~D3D9DeviceImpl() {
        (void)flushPeRecorder();
        releaseAllBound();
        dxmt9c_device_release(dev_);
    }

    /* ── IUnknown ── */

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)          ||
            IsEqualGUID(riid, IID_IDirect3DDevice9)  ||
            IsEqualGUID(riid, IID_IDirect3DDevice9Ex)) {
            *ppv = this;
            dxmt9DeviceDebugLog("device_query_interface this=%p -> out=%p", this, *ppv);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    /* ── device info ── */

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
        dxmt9DeviceDebugLog("device_test_cooperative_level device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_test_cooperative_level(dev_));
        dxmt9DeviceDebugLog("device_test_cooperative_level -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override {
        dxmt9DeviceDebugLog("device_get_available_texture_mem device=%p", this);
        const UINT value = 0x80000000u;
        dxmt9DeviceDebugLog("device_get_available_texture_mem -> %u (0x%x)",
                            value, (unsigned)value);
        return value;
    }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D) override {
        if (!ppD3D) return D3DERR_INVALIDCALL;
        factory_->AddRef();
        *ppD3D = static_cast<IDirect3D9*>(factory_);
        dxmt9DeviceDebugLog("device_get_direct3d this=%p -> factory=%p", this, static_cast<void*>(*ppD3D));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override {
        if (!pCaps) return D3DERR_INVALIDCALL;
        D9CCaps cc{};
        HRESULT hr = hr32(dxmt9c_device_get_caps(dev_, &cc));
        if (SUCCEEDED(hr)) {
            FillD3DCaps9(cc, pCaps);
            dxmt9DeviceDebugLog("device_get_caps -> vs=0x%08x ps=0x%08x maxTex=%ux%u maxRT=%u maxLights=%u maxStreams=%u maxAniso=%u intervals=0x%x devCaps=0x%x rasterCaps=0x%x texCaps=0x%x textureOpCaps=0x%x",
                                (unsigned)pCaps->VertexShaderVersion,
                                (unsigned)pCaps->PixelShaderVersion,
                                (unsigned)pCaps->MaxTextureWidth,
                                (unsigned)pCaps->MaxTextureHeight,
                                (unsigned)pCaps->NumSimultaneousRTs,
                                (unsigned)pCaps->MaxActiveLights,
                                (unsigned)pCaps->MaxStreams,
                                (unsigned)pCaps->MaxAnisotropy,
                                (unsigned)pCaps->PresentationIntervals,
                                (unsigned)pCaps->DevCaps,
                                (unsigned)pCaps->RasterCaps,
                                (unsigned)pCaps->TextureCaps,
                                (unsigned)pCaps->TextureOpCaps);
        } else {
            dxmt9DeviceDebugLog("device_get_caps -> hr=0x%08x", (unsigned)hr);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT sc, D3DDISPLAYMODE* pMode) override {
        if (!pMode) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_display_mode device=%p sc=%u", this, sc);
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, sc);
        if (!chain) {
            return D3DERR_INVALIDCALL;
        }
        D9CPresentParams cpp{};
        const HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(chain, &cpp));
        dxmt9c_swapchain_release(chain);
        if (FAILED(hr)) {
            dxmt9DeviceDebugLog("device_get_display_mode -> hr=0x%08x", (unsigned)hr);
            return hr;
        }
        pMode->Width = cpp.backBufferWidth;
        pMode->Height = cpp.backBufferHeight;
        pMode->RefreshRate = cpp.fullScreenRefreshRateHz;
        pMode->Format = static_cast<D3DFORMAT>(cpp.backBufferFormat);
        dxmt9DeviceDebugLog("device_get_display_mode -> %ux%u fmt=%u hz=%u",
                            pMode->Width, pMode->Height, (unsigned)pMode->Format, pMode->RefreshRate);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCreationParameters(
            D3DDEVICE_CREATION_PARAMETERS* pParams) override {
        if (!pParams) return D3DERR_INVALIDCALL;
        pParams->AdapterOrdinal  = adapter_;
        pParams->DeviceType      = D3DDEVTYPE_HAL;
        pParams->hFocusWindow    = creationWindow_;
        pParams->BehaviorFlags   = behaviorFlags_;
        return S_OK;
    }

    /* ── cursor (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* surface) override {
        dxmt9DeviceDebugLog("device_set_cursor_properties device=%p x=%u y=%u surface=%p",
                            this, x, y, surface);
        return S_OK;
    }
    void    STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) override {
        dxmt9DeviceDebugLog("device_set_cursor_position device=%p x=%d y=%d flags=0x%x",
                            this, x, y, (unsigned)flags);
    }
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL show) override {
        dxmt9DeviceDebugLog("device_show_cursor device=%p show=%u", this, (unsigned)show);
        return FALSE;
    }

    /* ── swap chains ── */

    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(
            D3DPRESENT_PARAMETERS* pPP, IDirect3DSwapChain9** ppSC) override {
        if (!pPP || !ppSC) return D3DERR_INVALIDCALL;
        D9CPresentParams cpp{};
        // minimal fill
        cpp.backBufferWidth  = pPP->BackBufferWidth;
        cpp.backBufferHeight = pPP->BackBufferHeight;
        cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
        cpp.backBufferCount  = pPP->BackBufferCount;
        cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
        cpp.deviceWindow     = (uint64_t)(uintptr_t)pPP->hDeviceWindow;
        cpp.windowed         = pPP->Windowed ? 1u : 0u;
        cpp.presentationInterval = pPP->PresentationInterval;
        D9CSwapChain* sc = dxmt9c_device_create_additional_swap_chain(dev_, &cpp);
        if (!sc) return D3DERR_INVALIDCALL;
        *ppSC = new D3D9SwapChainImpl(sc, this, this);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index,
                                            IDirect3DSwapChain9** ppSC) override {
        if (!ppSC) return D3DERR_INVALIDCALL;
        D9CSwapChain* sc = dxmt9c_device_get_swap_chain(dev_, index);
        if (!sc) return D3DERR_INVALIDCALL;
        *ppSC = new D3D9SwapChainImpl(sc, this, this);
        return S_OK;
    }

    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override {
        return dxmt9c_device_get_swap_chain_count(dev_);
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPP) override {
        if (!pPP) return D3DERR_INVALIDCALL;
        D9CPresentParams cpp{};
        cpp.backBufferWidth  = pPP->BackBufferWidth;
        cpp.backBufferHeight = pPP->BackBufferHeight;
        cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
        cpp.backBufferCount  = pPP->BackBufferCount;
        cpp.multiSampleType  = (uint32_t)pPP->MultiSampleType;
        cpp.multiSampleQuality = pPP->MultiSampleQuality;
        cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
        cpp.deviceWindow     = (uint64_t)(uintptr_t)pPP->hDeviceWindow;
        cpp.windowed         = pPP->Windowed ? 1u : 0u;
        cpp.enableAutoDepthStencil = pPP->EnableAutoDepthStencil ? 1u : 0u;
        cpp.autoDepthStencilFormat = (uint32_t)pPP->AutoDepthStencilFormat;
        cpp.flags            = pPP->Flags;
        cpp.fullScreenRefreshRateHz = pPP->FullScreen_RefreshRateInHz;
        cpp.presentationInterval = pPP->PresentationInterval;
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        releaseAllBound();
        clearPeStateTracking();
        return hr32(dxmt9c_device_reset(dev_, &cpp));
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty) override {
        dxmt9DeviceDebugLog("device_present device=%p wnd=%p src=%s dst=%s dirty=%p",
                            this, wnd,
                            src ? "<custom>" : "<full>",
                            dst ? "<custom>" : "<full>",
                            dirty);
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_present(dev_,
            src ? &cs : nullptr, dst ? &cd : nullptr,
            (uint64_t)(uintptr_t)wnd, dirty, 0));
    }

    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT idx,
                                             D3DBACKBUFFER_TYPE,
                                             IDirect3DSurface9** ppS) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_back_buffer device=%p sc=%u idx=%u", this, sc, idx);
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, sc);
        if (!chain) return D3DERR_INVALIDCALL;
        D9CSurface* s = dxmt9c_swapchain_get_back_buffer(chain, idx, 0);
        if (!s) {
            dxmt9c_swapchain_release(chain);
            return D3DERR_INVALIDCALL;
        }
        auto* swapchain = new D3D9SwapChainImpl(chain, this, this);
        *ppS = new D3D9SurfaceImpl(s, this, static_cast<IDirect3DSwapChain9*>(swapchain), this);
        swapchain->Release();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT, D3DRASTER_STATUS* p) override {
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enableDialogs) override {
        dxmt9DeviceDebugLog("device_set_dialog_box_mode device=%p enable=%u", this, (unsigned)enableDialogs);
        return S_OK;
    }
    void    STDMETHODCALLTYPE SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP*) override {
        dxmt9DeviceDebugLog("device_set_gamma_ramp device=%p swapChain=%u flags=0x%x",
                            this, swapChain, (unsigned)flags);
    }
    void    STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP* p) override {
        if (p) memset(p, 0, sizeof(*p));
    }

    /* ── resource creation ── */

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels,
                                             DWORD usage, D3DFORMAT fmt,
                                             D3DPOOL pool,
                                             IDirect3DTexture9** ppTex,
                                             HANDLE*) override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_texture device=%p size=%ux%u levels=%u usage=0x%x fmt=%u pool=%u",
                            this, w, h, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
        D9CTexture* t = dxmt9c_device_create_texture(dev_, w, h, levels,
                                                      usage, (uint32_t)fmt,
                                                      (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = new D3D9TextureImpl(t, this, this);
        dxmt9DeviceDebugLog("device_create_texture -> texture=%p", *ppTex);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d,
                                                   UINT levels, DWORD usage,
                                                   D3DFORMAT fmt, D3DPOOL pool,
                                                   IDirect3DVolumeTexture9** ppTex,
                                                   HANDLE*) override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_volume_texture device=%p size=%ux%ux%u levels=%u usage=0x%x fmt=%u pool=%u",
                            this, w, h, d, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
        D9CTexture* t = dxmt9c_device_create_volume_texture(dev_, w, h, d, levels,
                                                             usage, (uint32_t)fmt,
                                                             (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = new D3D9VolumeTextureImpl(t, this, this);
        dxmt9DeviceDebugLog("device_create_volume_texture -> texture=%p", *ppTex);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT size, UINT levels,
                                                 DWORD usage, D3DFORMAT fmt,
                                                 D3DPOOL pool,
                                                 IDirect3DCubeTexture9** ppTex,
                                                 HANDLE*) override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_cube_texture device=%p size=%u levels=%u usage=0x%x fmt=%u pool=%u",
                            this, size, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
        D9CTexture* t = dxmt9c_device_create_cube_texture(dev_, size, levels,
                                                           usage, (uint32_t)fmt,
                                                           (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = new D3D9CubeTextureImpl(t, this, this);
        dxmt9DeviceDebugLog("device_create_cube_texture -> texture=%p", *ppTex);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage,
                                                  DWORD fvf, D3DPOOL pool,
                                                  IDirect3DVertexBuffer9** ppBuf,
                                                  HANDLE*) override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_vertex_buffer device=%p len=%u usage=0x%x fvf=0x%x pool=%u",
                            this, len, (unsigned)usage, (unsigned)fvf, (unsigned)pool);
        D9CBuffer* b = dxmt9c_device_create_vertex_buffer(dev_, len, usage,
                                                           fvf, (uint32_t)pool);
        if (!b) return D3DERR_INVALIDCALL;
        *ppBuf = new D3D9VertexBufferImpl(b, this, this);
        dxmt9DeviceDebugLog("device_create_vertex_buffer -> buffer=%p", *ppBuf);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage,
                                                 D3DFORMAT fmt, D3DPOOL pool,
                                                 IDirect3DIndexBuffer9** ppBuf,
                                                 HANDLE*) override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_index_buffer device=%p len=%u usage=0x%x fmt=%u pool=%u",
                            this, len, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
        D9CBuffer* b = dxmt9c_device_create_index_buffer(dev_, len, usage,
                                                          (uint32_t)fmt,
                                                          (uint32_t)pool);
        if (!b) return D3DERR_INVALIDCALL;
        *ppBuf = new D3D9IndexBufferImpl(b, this, this);
        dxmt9DeviceDebugLog("device_create_index_buffer -> buffer=%p", *ppBuf);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt,
                                                  D3DMULTISAMPLE_TYPE ms,
                                                  DWORD msQual, BOOL lockable,
                                                  IDirect3DSurface9** ppS,
                                                  HANDLE* psh) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_render_target device=%p size=%ux%u fmt=%u ms=%u msQual=%u lockable=%u",
                            this, w, h, (unsigned)fmt, (unsigned)ms, (unsigned)msQual, (unsigned)lockable);
        uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
        D9CSurface* s = dxmt9c_device_create_render_target(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)ms, msQual,
                                                            lockable ? 1u : 0u, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, this, nullptr, this);
        dxmt9DeviceDebugLog("device_create_render_target -> surface=%p", *ppS);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h,
                                                         D3DFORMAT fmt,
                                                         D3DMULTISAMPLE_TYPE ms,
                                                         DWORD msQual,
                                                         BOOL discard,
                                                         IDirect3DSurface9** ppS,
                                                         HANDLE* psh) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_depth_stencil_surface device=%p size=%ux%u fmt=%u ms=%u msQual=%u discard=%u",
                            this, w, h, (unsigned)fmt, (unsigned)ms, (unsigned)msQual, (unsigned)discard);
        uint64_t sh = 0;
        D9CSurface* s = dxmt9c_device_create_depth_stencil(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)ms, msQual,
                                                            discard ? 1u : 0u, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, this, nullptr, this);
        dxmt9DeviceDebugLog("device_create_depth_stencil_surface -> surface=%p", *ppS);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src,
                                             const RECT* srcRect,
                                             IDirect3DSurface9* dst,
                                             const POINT* dstPt) override {
        dxmt9DeviceDebugLog("device_update_surface device=%p src=%p dst=%p srcRect=%s dstPt=%s",
                            this, src, dst,
                            srcRect ? "<custom>" : "<full>",
                            dstPt ? "<custom>" : "<origin>");
        D9CRect cs{}, cd{};
        if (srcRect) cs = toR(*srcRect);
        if (dstPt) { cd.left = dstPt->x; cd.top = dstPt->y;
                     cd.right = dstPt->x; cd.bottom = dstPt->y; }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_update_surface(dev_,
            rawSurf(src), srcRect ? &cs : nullptr,
            rawSurf(dst), dstPt   ? &cd : nullptr));
    }

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,
                                             IDirect3DBaseTexture9* dst) override {
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_update_texture(dev_,
                    rawTex(src), rawTex(dst)));
    }

    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt,
                                                   IDirect3DSurface9* dst) override {
        dxmt9DeviceDebugLog("device_get_render_target_data device=%p rt=%p dst=%p",
                            this, rt, dst);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_get_render_target_data(dev_,
                    rawSurf(rt), rawSurf(dst)));
    }

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sc, IDirect3DSurface9* surface) override {
        dxmt9DeviceDebugLog("device_get_front_buffer_data device=%p sc=%u surface=%p",
                            this, sc, surface);
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src,
                                           const RECT* srcRect,
                                           IDirect3DSurface9* dst,
                                           const RECT* dstRect,
                                           D3DTEXTUREFILTERTYPE filter) override {
        dxmt9DeviceDebugLog("device_stretch_rect device=%p src=%p dst=%p filter=%u srcRect=%s dstRect=%s",
                            this, src, dst, (unsigned)filter,
                            srcRect ? "<custom>" : "<full>",
                            dstRect ? "<custom>" : "<full>");
        D9CRect cs{}, cd{};
        if (srcRect) cs = toR(*srcRect); if (dstRect) cd = toR(*dstRect);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_stretch_rect(dev_,
            rawSurf(src), srcRect ? &cs : nullptr,
            rawSurf(dst), dstRect ? &cd : nullptr,
            (uint32_t)filter));
    }

    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurf,
                                         const RECT* pRect,
                                         D3DCOLOR color) override {
        dxmt9DeviceDebugLog("device_color_fill device=%p surf=%p rect=%s color=0x%08x",
                            this, pSurf, pRect ? "<custom>" : "<full>", (unsigned)color);
        D9CRect cr{}; if (pRect) cr = toR(*pRect);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_color_fill(dev_, rawSurf(pSurf),
                    pRect ? &cr : nullptr, (uint32_t)color));
    }

    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DPOOL pool,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE*) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_offscreen_surface device=%p size=%ux%u fmt=%u pool=%u",
                            this, w, h, (unsigned)fmt, (unsigned)pool);
        uint64_t sh = 0;
        D9CSurface* s = dxmt9c_device_create_offscreen_surface(dev_, w, h,
                                                                (uint32_t)fmt,
                                                                (uint32_t)pool, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, this, nullptr, this);
        dxmt9DeviceDebugLog("device_create_offscreen_surface -> surface=%p", *ppS);
        return S_OK;
    }

    /* ── render targets ── */

    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD idx,
                                               IDirect3DSurface9* pSurf) override {
        dxmt9DeviceDebugLog("device_set_render_target device=%p idx=%u surf=%p",
                            this, (unsigned)idx, pSurf);
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        if (auto* raw = rawSurf(pSurf); raw != nullptr) {
            noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                            reinterpret_cast<uint64_t>(raw));
        }
        return hr32(dxmt9c_device_set_render_target(dev_, idx, rawSurf(pSurf)));
    }

    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx,
                                               IDirect3DSurface9** ppS) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u",
                            this, (unsigned)idx);
        D9CSurface* s = dxmt9c_device_get_render_target(dev_, idx);
        *ppS = s ? new D3D9SurfaceImpl(s, this, nullptr, this) : nullptr;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> surface=%p",
                            this, (unsigned)idx, ppS ? static_cast<void*>(*ppS) : nullptr);
        return s ? S_OK : D3DERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pSurf) override {
        dxmt9DeviceDebugLog("device_set_depth_stencil device=%p surf=%p", this, pSurf);
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        if (auto* raw = rawSurf(pSurf); raw != nullptr) {
            noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                            reinterpret_cast<uint64_t>(raw));
        }
        return hr32(dxmt9c_device_set_depth_stencil(dev_, rawSurf(pSurf)));
    }

    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppS) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        D9CSurface* s = dxmt9c_device_get_depth_stencil(dev_);
        *ppS = s ? new D3D9SurfaceImpl(s, this, nullptr, this) : nullptr;
        dxmt9DeviceDebugLog("device_get_depth_stencil_surface device=%p -> surface=%p",
                            this, ppS ? static_cast<void*>(*ppS) : nullptr);
        return s ? S_OK : S_FALSE;
    }

    /* ── scene ── */
    HRESULT STDMETHODCALLTYPE BeginScene() override {
        dxmt9DeviceDebugLog("device_begin_scene device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_scene(dev_));
        dxmt9DeviceDebugLog("device_begin_scene -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndScene()   override {
        dxmt9DeviceDebugLog("device_end_scene device=%p", this);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_device_end_scene(dev_));
        dxmt9DeviceDebugLog("device_end_scene -> hr=0x%08x", (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* pRects,
                                     DWORD flags, D3DCOLOR color,
                                     float z, DWORD stencil) override {
        dxmt9DeviceDebugLog("device_clear device=%p count=%u flags=0x%x color=0x%08x z=%f stencil=%u",
                            this, (unsigned)count, (unsigned)flags, (unsigned)color, z,
                            (unsigned)stencil);
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_clear(dev_, count,
            reinterpret_cast<const D9CRect*>(pRects),
            flags, (uint32_t)color, z, stencil));
    }

    /* ── transforms ── */
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* pM) override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog(
            "device_set_transform device=%p state=%u "
            "m=[[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g]]",
            this, (unsigned)state,
            pM->m[0][0], pM->m[0][1], pM->m[0][2], pM->m[0][3],
            pM->m[1][0], pM->m[1][1], pM->m[1][2], pM->m[1][3],
            pM->m[2][0], pM->m[2][1], pM->m[2][2], pM->m[2][3],
            pM->m[3][0], pM->m[3][1], pM->m[3][2], pM->m[3][3]);
        // Per recorder design: Set* state setters are PE-shadow-only and
        // belong in the next DrawRecord's snapshot, not as standalone
        // backend records. DrawRecord doesn't yet carry transforms, so
        // fall back to legacy unix-call until that extension lands.
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_device_set_transform(dev_, (uint32_t)state,
                    reinterpret_cast<const D9CMatrix*>(pM)));
        dxmt9DeviceDebugLog("device_set_transform -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state,
                                            D3DMATRIX* pM) override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_transform device=%p state=%u", this, (unsigned)state);
        return hr32(dxmt9c_device_get_transform(dev_, (uint32_t)state,
                    reinterpret_cast<D9CMatrix*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX* pM) override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_multiply_transform device=%p state=%u", this, (unsigned)state);
        D3DMATRIX cur{};
        GetTransform(state, &cur);
        /* multiply 4x4 */
        D3DMATRIX result{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                float s = 0;
                for (int k = 0; k < 4; ++k)
                    s += cur.m[r][k] * pM->m[k][c];
                result.m[r][c] = s;
            }
        return SetTransform(state, &result);
    }

    /* ── viewport / scissor ── */
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pVP) override {
        if (!pVP) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_viewport device=%p x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                            this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
        D9CViewport vp{ pVP->X, pVP->Y, pVP->Width, pVP->Height,
                        pVP->MinZ, pVP->MaxZ };
        // Set* setter — PE-shadow-only per recorder design; falls back to
        // legacy unix-call until DrawRecord carries the snapshot.
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_viewport(dev_, &vp));
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pVP) override {
        if (!pVP) return D3DERR_INVALIDCALL;
        D9CViewport vp{};
        dxmt9c_device_get_viewport(dev_, &vp);
        pVP->X = vp.x; pVP->Y = vp.y;
        pVP->Width = vp.width; pVP->Height = vp.height;
        pVP->MinZ = vp.minZ;   pVP->MaxZ   = vp.maxZ;
        dxmt9DeviceDebugLog("device_get_viewport device=%p -> x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                            this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pR) override {
        if (!pR) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_scissor_rect device=%p rect=%ld,%ld-%ld,%ld",
                            this, (long)pR->left, (long)pR->top, (long)pR->right, (long)pR->bottom);
        D9CRect cr = toR(*pR);
        // Set* setter — PE-shadow-only per recorder design.
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_scissor_rect(dev_, &cr));
    }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pR) override {
        if (!pR) return D3DERR_INVALIDCALL;
        D9CRect cr{};
        dxmt9c_device_get_scissor_rect(dev_, &cr);
        pR->left = cr.left; pR->top = cr.top;
        pR->right = cr.right; pR->bottom = cr.bottom;
        return S_OK;
    }

    /* ── material / lights ── */
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pM) override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_material device=%p", this);
        // Set* setter — PE-shadow-only per recorder design.
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_material(dev_,
                    reinterpret_cast<const D9CMaterial*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pM) override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_material device=%p", this);
        return hr32(dxmt9c_device_get_material(dev_,
                    reinterpret_cast<D9CMaterial*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD idx, const D3DLIGHT9* pL) override {
        if (!pL) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_light device=%p idx=%u type=%u", this, (unsigned)idx, (unsigned)pL->Type);
        D9CLight cl{};
        cl.type = (uint32_t)pL->Type;
        memcpy(&cl.diffuse,  &pL->Diffuse,  sizeof(D9CColorRGBA));
        memcpy(&cl.specular, &pL->Specular, sizeof(D9CColorRGBA));
        memcpy(&cl.ambient,  &pL->Ambient,  sizeof(D9CColorRGBA));
        cl.position[0] = pL->Position.x;
        cl.position[1] = pL->Position.y;
        cl.position[2] = pL->Position.z;
        cl.direction[0] = pL->Direction.x;
        cl.direction[1] = pL->Direction.y;
        cl.direction[2] = pL->Direction.z;
        cl.range  = pL->Range;  cl.falloff = pL->Falloff;
        cl.attenuation0 = pL->Attenuation0;
        cl.attenuation1 = pL->Attenuation1;
        cl.attenuation2 = pL->Attenuation2;
        cl.theta = pL->Theta; cl.phi = pL->Phi;
        // Set* setter — PE-shadow-only per recorder design.
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_light(dev_, idx, &cl));
    }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD, D3DLIGHT9* pL) override {
        dxmt9DeviceDebugLog("device_get_light device=%p", this);
        if (pL) memset(pL, 0, sizeof(*pL)); return S_OK; /* stub */
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) override {
        dxmt9DeviceDebugLog("device_light_enable device=%p idx=%u enable=%u", this, (unsigned)idx, (unsigned)en);
        // Set* setter — PE-shadow-only per recorder design.
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_light_enable(dev_, idx, en ? 1u : 0u));
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD, BOOL* pEn) override {
        dxmt9DeviceDebugLog("device_get_light_enable device=%p", this);
        if (pEn) *pEn = FALSE; return S_OK; /* stub */
    }

    /* ── clip planes ── */
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* pPlane) override {
        dxmt9DeviceDebugLog("device_set_clip_plane device=%p idx=%u plane=%p", this, (unsigned)idx, pPlane);
        if (!pPlane) return D3DERR_INVALIDCALL;
        // Set* setter — PE-shadow-only per recorder design.
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_clip_plane(dev_, idx, pPlane));
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* pPlane) override {
        dxmt9DeviceDebugLog("device_get_clip_plane device=%p idx=%u", this, (unsigned)idx);
        return hr32(dxmt9c_device_get_clip_plane(dev_, idx, pPlane));
    }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9*) override {
        dxmt9DeviceDebugLog("device_set_clip_status device=%p", this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* p) override {
        dxmt9DeviceDebugLog("device_get_clip_status device=%p", this);
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }

    /* ── render states ── */
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) override {
        dxmt9DeviceDebugLog("device_set_render_state device=%p state=%u value=0x%x",
                            this, (unsigned)state, (unsigned)value);
        const DWORD stateKey = static_cast<DWORD>(state);
        if (dxmt9PeStateShadowEnabled() && shadowedRenderStateEquals(stateKey, value)) {
            return S_OK;
        }
        const HRESULT chunkHr = flushPendingCommandChunk();
        if (FAILED(chunkHr)) return chunkHr;
        if (dxmt9PeStateShadowEnabled()) {
            renderStateShadow_[stateKey] = value;
            pendingRenderStates_[stateKey] = value;
            return S_OK;
        }
        return hr32(dxmt9c_device_set_render_state(dev_, (uint32_t)state, value));
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD* pValue) override {
        if (!pValue) return D3DERR_INVALIDCALL;
        if (dxmt9PeStateShadowEnabled()) {
            const auto it = renderStateShadow_.find(static_cast<DWORD>(state));
            if (it != renderStateShadow_.end()) {
                *pValue = it->second;
                return S_OK;
            }
        }
        *pValue = dxmt9c_device_get_render_state(dev_, (uint32_t)state);
        return S_OK;
    }

    /* ── state blocks ── */
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type,
                                                IDirect3DStateBlock9** ppSB) override {
        if (!ppSB) return D3DERR_INVALIDCALL;
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        dxmt9DeviceDebugLog("device_create_state_block device=%p type=%u", this, (unsigned)type);
        D9CStateBlock* sb = dxmt9c_device_create_state_block(dev_, (uint32_t)type);
        if (!sb) return D3DERR_INVALIDCALL;
        *ppSB = new D3D9StateBlockImpl(sb, this, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override {
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        dxmt9DeviceDebugLog("device_begin_state_block device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_state_block(dev_));
        dxmt9DeviceDebugLog("device_begin_state_block -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override {
        if (!ppSB) return D3DERR_INVALIDCALL;
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        *ppSB = nullptr;
        dxmt9DeviceDebugLog("device_end_state_block device=%p", this);
        D9CStateBlock* sb = nullptr;
        HRESULT hr = hr32(dxmt9c_device_end_state_block(dev_, &sb));
        if (SUCCEEDED(hr) && sb) *ppSB = new D3D9StateBlockImpl(sb, this, this);
        dxmt9DeviceDebugLog("device_end_state_block -> hr=0x%08x sb=%p out=%p",
                            (unsigned)hr, static_cast<void*>(sb), *ppSB);
        return hr;
    }

    /* ── texture stage / sampler states ── */
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD value) override {
        dxmt9DeviceDebugLog("device_set_texture_stage_state device=%p stage=%u type=%u value=0x%x",
                            this, (unsigned)stage, (unsigned)type, (unsigned)value);
        // Set* setter — PE-shadow-only per recorder design.
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_texture_stage_state(dev_, stage,
                    (uint32_t)type, value));
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD* pValue) override {
        if (!pValue) return D3DERR_INVALIDCALL;
        *pValue = dxmt9c_device_get_texture_stage_state(dev_, stage, (uint32_t)type);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD value) override {
        dxmt9DeviceDebugLog("device_set_sampler_state device=%p sampler=%u type=%u value=0x%x",
                            this, (unsigned)sampler, (unsigned)type, (unsigned)value);
        // Set* setter — PE-shadow-only per recorder design.
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_sampler_state(dev_, sampler,
                    (uint32_t)type, value));
    }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD* pValue) override {
        if (!pValue) return D3DERR_INVALIDCALL;
        *pValue = dxmt9c_device_get_sampler_state(dev_, sampler, (uint32_t)type);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pPasses) override {
        dxmt9DeviceDebugLog("device_validate_device device=%p", this);
        if (pPasses) *pPasses = 1; return S_OK;
    }

    /* ── palette (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY*) override {
        dxmt9DeviceDebugLog("device_set_palette_entries device=%p palette=%u", this, palette);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY*) override {
        dxmt9DeviceDebugLog("device_get_palette_entries device=%p palette=%u", this, palette);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) override {
        dxmt9DeviceDebugLog("device_set_current_texture_palette device=%p palette=%u", this, palette);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) override {
        dxmt9DeviceDebugLog("device_get_current_texture_palette device=%p", this);
        if (p) *p = 0; return S_OK;
    }

    /* ── soft VP / NPatches (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL enable) override {
        dxmt9DeviceDebugLog("device_set_software_vertex_processing device=%p enable=%u", this, (unsigned)enable);
        return S_OK;
    }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() override {
        dxmt9DeviceDebugLog("device_get_software_vertex_processing device=%p", this);
        return FALSE;
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) override {
        dxmt9DeviceDebugLog("device_set_npatch_mode device=%p segments=%f", this, segments);
        return S_OK;
    }
    float   STDMETHODCALLTYPE GetNPatchMode() override { return 0.0f; }

    /* ── textures ── */
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage,
                                          IDirect3DBaseTexture9* pTex) override {
        dxmt9DeviceDebugLog("device_set_texture device=%p stage=%u tex=%p",
                            this, (unsigned)stage, pTex);
        if (stage >= 16) return D3DERR_INVALIDCALL;
        if (dxmt9PeStateShadowEnabled() && shadowedTextureEquals(stage, pTex)) {
            return S_OK;
        }
        const HRESULT chunkHr = flushPendingCommandChunk();
        if (FAILED(chunkHr)) return chunkHr;
        setRef(textures_[stage], pTex);
        // Phase 4: track every bound texture handle in the chunk
        // retention set so the server-side importer can mark all of
        // them at chunk seqId in one bulk pass.
        if (auto* raw = rawTex(pTex); raw != nullptr) {
            noteChunkHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE,
                            reinterpret_cast<uint64_t>(raw));
        }
        if (dxmt9PeStateShadowEnabled()) {
            pendingTextureMask_ |= 1u << stage;
            return S_OK;
        }
        return hr32(dxmt9c_device_set_texture(dev_, stage, rawTex(pTex)));
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage,
                                          IDirect3DBaseTexture9** ppTex) override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        IDirect3DBaseTexture9* t = textures_[stage < 16 ? stage : 0];
        if (t) t->AddRef();
        *ppTex = t;
        dxmt9DeviceDebugLog("device_get_texture device=%p stage=%u -> tex=%p",
                            this, (unsigned)stage, static_cast<void*>(t));
        return S_OK;
    }

    /* ── FVF / vertex declaration ── */
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override {
        dxmt9DeviceDebugLog("device_set_fvf device=%p fvf=0x%x", this, (unsigned)fvf);
        if (dxmt9PeStateShadowEnabled() && fvf_ == fvf) {
            return S_OK;
        }
        const HRESULT chunkHr = flushPendingCommandChunk();
        if (FAILED(chunkHr)) return chunkHr;
        fvf_ = fvf;
        if (dxmt9PeStateShadowEnabled()) {
            pendingFvf_ = true;
            return S_OK;
        }
        return hr32(dxmt9c_device_set_fvf(dev_, fvf));
    }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override {
        if (!pFVF) return D3DERR_INVALIDCALL;
        if (dxmt9PeStateShadowEnabled()) {
            *pFVF = fvf_;
            return S_OK;
        }
        *pFVF = dxmt9c_device_get_fvf(dev_); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
            const D3DVERTEXELEMENT9* pElems,
            IDirect3DVertexDeclaration9** ppVD) override {
        if (!pElems || !ppVD) return D3DERR_INVALIDCALL;
        /* count elements until D3DDECL_END() */
        int n = 0;
        while (pElems[n].Stream != 0xFF) ++n;
        ++n; /* include D3DDECL_END */
        D9CVertexElement tmp[64]{};
        if (n > 64) return D3DERR_INVALIDCALL;
        for (int i = 0; i < n; ++i) {
            tmp[i].stream = pElems[i].Stream; tmp[i].offset = pElems[i].Offset;
            tmp[i].type   = pElems[i].Type;   tmp[i].method = pElems[i].Method;
            tmp[i].usage  = pElems[i].Usage;  tmp[i].usageIndex = pElems[i].UsageIndex;
        }
        D9CVertexDecl* d = dxmt9c_device_create_vertex_declaration(dev_, tmp);
        if (!d) return D3DERR_INVALIDCALL;
        *ppVD = new D3D9VertexDeclImpl(d, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(
            IDirect3DVertexDeclaration9* pVD) override {
        dxmt9DeviceDebugLog("device_set_vertex_declaration device=%p decl=%p", this, pVD);
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        setRef(vdecl_, pVD);
        return hr32(dxmt9c_device_set_vertex_declaration(dev_, rawVD(pVD)));
    }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(
            IDirect3DVertexDeclaration9** ppVD) override {
        if (!ppVD) return D3DERR_INVALIDCALL;
        if (vdecl_) vdecl_->AddRef();
        *ppVD = vdecl_; return S_OK;
    }

    /* ── vertex shaders ── */
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFn,
                                                  IDirect3DVertexShader9** ppVS) override {
        if (!pFn || !ppVS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_vertex_shader device=%p code=%p", this, pFn);
        D9CShader* s = dxmt9c_device_create_vertex_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
        if (!s) return D3DERR_INVALIDCALL;
        *ppVS = new D3D9VertexShaderImpl(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) override {
        dxmt9DeviceDebugLog("device_set_vertex_shader device=%p shader=%p", this, pVS);
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        setRef(vs_, pVS);
        return hr32(dxmt9c_device_set_vertex_shader(dev_, rawVS(pVS)));
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppVS) override {
        if (!ppVS) return D3DERR_INVALIDCALL;
        if (vs_) vs_->AddRef(); *ppVS = vs_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* pData,
                                                        UINT count) override {
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_f device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        if (dxmt9PeDrawChunkEnabled()) {
            // Shadow-only: defer the record until the next flushPendingConsts()
            // (called before each draw record + at chunk commit). Merging
            // dozens of overlapping/contiguous Set calls into one record.
            touchConstShadow(vsConstF_, start, count, pData, sizeof(float) * 4);
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_vs_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* pData,
                                                        UINT count) override {
        return hr32(dxmt9c_device_get_vs_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) override {
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        if (dxmt9PeDrawChunkEnabled()) {
            touchConstShadow(vsConstI_, start, count, pData, sizeof(int32_t) * 4);
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_vs_const_i(dev_, start,
                    reinterpret_cast<const int32_t*>(pData), count));
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, INT* pData,
                                                        UINT count) override {
        (void)start; (void)pData; (void)count; return S_OK; /* stub */
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) override {
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        if (dxmt9PeDrawChunkEnabled()) {
            touchConstShadow(vsConstB_, start, count, pData, sizeof(uint32_t));
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_vs_const_b(dev_, start,
                    reinterpret_cast<const uint32_t*>(pData), count));
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, BOOL* pData,
                                                        UINT count) override {
        (void)start; (void)pData; (void)count; return S_OK; /* stub */
    }

    /* ── stream sources ── */
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9* pBuf,
                                               UINT offset, UINT stride) override {
        dxmt9DeviceDebugLog("device_set_stream_source device=%p stream=%u buf=%p offset=%u stride=%u",
                            this, stream, pBuf, offset, stride);
        if (stream >= 16) return D3DERR_INVALIDCALL;
        if (dxmt9PeStateShadowEnabled() && shadowedStreamSourceEquals(stream, pBuf, offset, stride)) {
            return S_OK;
        }
        const HRESULT chunkHr = flushPendingCommandChunk();
        if (FAILED(chunkHr)) return chunkHr;
        setRef(streamSrc_[stream], pBuf);
        streamOff_[stream] = offset;
        streamStr_[stream] = stride;
        if (auto* raw = rawVBuf(pBuf); raw != nullptr) {
            noteChunkHandle(D9C_CHUNK_HANDLE_KIND_BUFFER,
                            reinterpret_cast<uint64_t>(raw));
        }
        if (dxmt9PeStateShadowEnabled()) {
            pendingStreamMask_ |= 1u << stream;
            return S_OK;
        }
        return hr32(dxmt9c_device_set_stream_source(dev_, stream,
                    rawVBuf(pBuf), offset, stride));
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9** ppBuf,
                                               UINT* pOffset, UINT* pStride) override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        IDirect3DVertexBuffer9* b = streamSrc_[stream < 16 ? stream : 0];
        if (b) b->AddRef();
        *ppBuf = b;
        if (pOffset) *pOffset = streamOff_[stream < 16 ? stream : 0];
        if (pStride) *pStride = streamStr_[stream < 16 ? stream : 0];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT freq) override {
        dxmt9DeviceDebugLog("device_set_stream_source_freq device=%p stream=%u freq=0x%x",
                            this, stream, (unsigned)freq);
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        streamFreq_[stream < 16 ? stream : 0] = freq;
        return hr32(dxmt9c_device_set_stream_source_freq(dev_, stream, freq));
    }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* pFreq) override {
        const UINT freq = streamFreq_[stream < 16 ? stream : 0];
        if (pFreq) *pFreq = freq;
        dxmt9DeviceDebugLog("device_get_stream_source_freq device=%p stream=%u -> freq=0x%x",
                            this, stream, (unsigned)freq);
        return S_OK;
    }

    /* ── indices ── */
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIBuf) override {
        dxmt9DeviceDebugLog("device_set_indices device=%p ib=%p", this, pIBuf);
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        setRef(indexBuf_, pIBuf);
        if (auto* raw = rawIBuf(pIBuf); raw != nullptr) {
            noteChunkHandle(D9C_CHUNK_HANDLE_KIND_BUFFER,
                            reinterpret_cast<uint64_t>(raw));
        }
        return hr32(dxmt9c_device_set_indices(dev_, rawIBuf(pIBuf)));
    }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIBuf) override {
        if (!ppIBuf) return D3DERR_INVALIDCALL;
        if (indexBuf_) indexBuf_->AddRef(); *ppIBuf = indexBuf_; return S_OK;
    }

    /* ── pixel shaders ── */
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFn,
                                                 IDirect3DPixelShader9** ppPS) override {
        if (!pFn || !ppPS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_pixel_shader device=%p code=%p", this, pFn);
        D9CShader* s = dxmt9c_device_create_pixel_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
        if (!s) return D3DERR_INVALIDCALL;
        *ppPS = new D3D9PixelShaderImpl(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pPS) override {
        dxmt9DeviceDebugLog("device_set_pixel_shader device=%p shader=%p", this, pPS);
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        setRef(ps_, pPS);
        return hr32(dxmt9c_device_set_pixel_shader(dev_, rawPS(pPS)));
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) override {
        if (!ppPS) return D3DERR_INVALIDCALL;
        if (ps_) ps_->AddRef(); *ppPS = ps_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) override {
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_f device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        if (dxmt9PeDrawChunkEnabled()) {
            touchConstShadow(psConstF_, start, count, pData, sizeof(float) * 4);
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_ps_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* pData,
                                                       UINT count) override {
        return hr32(dxmt9c_device_get_ps_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) override {
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        if (dxmt9PeDrawChunkEnabled()) {
            touchConstShadow(psConstI_, start, count, pData, sizeof(int32_t) * 4);
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_ps_const_i(dev_, start,
                    reinterpret_cast<const int32_t*>(pData), count));
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, INT* pData,
                                                       UINT count) override {
        (void)start; (void)pData; (void)count; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) override {
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        if (dxmt9PeDrawChunkEnabled()) {
            touchConstShadow(psConstB_, start, count, pData, sizeof(uint32_t));
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_ps_const_b(dev_, start,
                    reinterpret_cast<const uint32_t*>(pData), count));
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, BOOL* pData,
                                                       UINT count) override {
        (void)start; (void)pData; (void)count; return S_OK;
    }

    /* ── draw calls ── */
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type,
                                             UINT startVertex,
                                             UINT count) override {
        dxmt9DeviceDebugLog("device_draw_primitive device=%p type=%u startVertex=%u count=%u",
                            this, (unsigned)type, startVertex, count);
        if (dxmt9PeDrawChunkEnabled() &&
            pendingRenderStates_.size() <= D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT hr = appendDrawPrimitiveRecord(type, startVertex, count);
            if (SUCCEEDED(hr)) {
                clearPendingHotState();
            }
            return hr;
        }
        if (dxmt9PeStateShadowEnabled() && hasPendingHotState()) {
            D9CDrawPrimitivePacket packet{};
            if (buildDrawPrimitivePacket(type, startVertex, count, packet)) {
                const HRESULT hr = hr32(dxmt9c_device_draw_primitive_packet(dev_, &packet));
                if (SUCCEEDED(hr)) {
                    clearPendingHotState();
                }
                return hr;
            }
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_draw_primitive(dev_, (uint32_t)type,
                    startVertex, count));
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type,
                                                    INT baseVertex,
                                                    UINT minVertex, UINT numVertices,
                                                    UINT startIndex,
                                                    UINT count) override {
        dxmt9DeviceDebugLog("device_draw_indexed_primitive device=%p type=%u base=%d min=%u num=%u startIndex=%u count=%u",
                            this, (unsigned)type, baseVertex, minVertex, numVertices,
                            startIndex, count);
        if (dxmt9PeDrawChunkEnabled() &&
            pendingRenderStates_.size() <= D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT hr = appendDrawIndexedPrimitiveRecord(type, baseVertex, minVertex,
                                                                numVertices, startIndex, count);
            if (SUCCEEDED(hr)) {
                clearPendingHotState();
            }
            return hr;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_draw_indexed_primitive(dev_, (uint32_t)type,
                    baseVertex, minVertex, numVertices, startIndex, count));
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                               UINT count,
                                               const void* pData,
                                               UINT stride) override {
        dxmt9DeviceDebugLog("device_draw_primitive_up device=%p type=%u count=%u data=%p stride=%u",
                            this, (unsigned)type, count, pData, stride);
        if (dxmt9PeDrawChunkEnabled() &&
            pendingRenderStates_.size() <= D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT hr = appendDrawPrimitiveUPRecord(type, count, pData, stride);
            if (SUCCEEDED(hr)) {
                clearPendingHotState();
            }
            return hr;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_draw_primitive_up(dev_, (uint32_t)type,
                    count, pData, stride));
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                      UINT minVertex,
                                                      UINT numVertices,
                                                      UINT count,
                                                      const void* pIdxData,
                                                      D3DFORMAT idxFmt,
                                                      const void* pVtxData,
                                                      UINT stride) override {
        dxmt9DeviceDebugLog("device_draw_indexed_primitive_up device=%p type=%u min=%u num=%u count=%u idx=%p idxFmt=%u vtx=%p stride=%u",
                            this, (unsigned)type, minVertex, numVertices, count,
                            pIdxData, (unsigned)idxFmt, pVtxData, stride);
        if (dxmt9PeDrawChunkEnabled() &&
            pendingRenderStates_.size() <= D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT hr = appendDrawIndexedPrimitiveUPRecord(type, minVertex, numVertices,
                                                                  count, pIdxData, idxFmt,
                                                                  pVtxData, stride);
            if (SUCCEEDED(hr)) {
                clearPendingHotState();
            }
            return hr;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_draw_indexed_primitive_up(dev_,
                    (uint32_t)type, minVertex, numVertices, count,
                    pIdxData, (uint32_t)idxFmt, pVtxData, stride));
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT,
                                               IDirect3DVertexBuffer9*,
                                               IDirect3DVertexDeclaration9*,
                                               DWORD) override {
        dxmt9DeviceDebugLog("device_process_vertices device=%p", this);
        return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) override { return S_OK; }

    /* ── query ── */
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type,
                                           IDirect3DQuery9** ppQ) override {
        if (!ppQ) { /* just checking support */ return S_OK; }
        D9CQuery* q = dxmt9c_device_create_query(dev_, (uint32_t)type);
        if (!q) return D3DERR_NOTAVAILABLE;
        *ppQ = new D3D9QueryImpl(q, this, this);
        return S_OK;
    }

    /* ── IDirect3DDevice9Ex ── */

    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT,UINT,float*,float*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9*,IDirect3DSurface9*,
                                            IDirect3DVertexBuffer9*,UINT,
                                            IDirect3DVertexBuffer9*,
                                            D3DCOMPOSERECTSOP,int,int) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE PresentEx(const RECT* src, const RECT* dst,
                                         HWND wnd, const RGNDATA* dirty,
                                         DWORD flags) override {
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_present(dev_,
            src ? &cs : nullptr, dst ? &cd : nullptr,
            (uint64_t)(uintptr_t)wnd, dirty, flags));
    }

    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* p) override { if (p) *p = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT sc) override {
        return hr32(dxmt9c_device_wait_for_vblank(dev_, sc));
    }

    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9**,
                                                      UINT32) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT maxLatency) override {
        return hr32(dxmt9c_device_set_maximum_frame_latency(dev_, maxLatency));
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* p) override {
        if (!p) return D3DERR_INVALIDCALL;
        *p = dxmt9c_device_get_maximum_frame_latency(dev_); return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND wnd) override {
        return hr32(dxmt9c_device_check_device_state(dev_,
                    (uint64_t)(uintptr_t)wnd));
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT w, UINT h,
                                                    D3DFORMAT fmt,
                                                    D3DMULTISAMPLE_TYPE ms,
                                                    DWORD msQual, BOOL lockable,
                                                    IDirect3DSurface9** ppS,
                                                    HANDLE* psh,
                                                    DWORD) override {
        return CreateRenderTarget(w, h, fmt, ms, msQual, lockable, ppS, psh);
    }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT w, UINT h,
                                                             D3DFORMAT fmt,
                                                             D3DPOOL pool,
                                                             IDirect3DSurface9** ppS,
                                                             HANDLE* psh,
                                                             DWORD) override {
        return CreateOffscreenPlainSurface(w, h, fmt, pool, ppS, psh);
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DMULTISAMPLE_TYPE ms,
                                                           DWORD msQual,
                                                           BOOL discard,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE* psh,
                                                           DWORD) override {
        return CreateDepthStencilSurface(w, h, fmt, ms, msQual, discard, ppS, psh);
    }

    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS* pPP,
                                       D3DDISPLAYMODEEX* pFsMode) override {
        if (!pPP) return D3DERR_INVALIDCALL;
        D9CPresentParams cpp{};
        cpp.backBufferWidth  = pPP->BackBufferWidth;
        cpp.backBufferHeight = pPP->BackBufferHeight;
        cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
        cpp.backBufferCount  = pPP->BackBufferCount;
        cpp.multiSampleType  = (uint32_t)pPP->MultiSampleType;
        cpp.multiSampleQuality = pPP->MultiSampleQuality;
        cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
        cpp.deviceWindow     = (uint64_t)(uintptr_t)pPP->hDeviceWindow;
        cpp.windowed         = pPP->Windowed ? 1u : 0u;
        cpp.flags            = pPP->Flags;
        cpp.fullScreenRefreshRateHz = pPP->FullScreen_RefreshRateInHz;
        cpp.presentationInterval = pPP->PresentationInterval;
        D9CDisplayModeEx cdme{};
        if (pFsMode) {
            cdme.width  = pFsMode->Width; cdme.height = pFsMode->Height;
            cdme.refreshRate = pFsMode->RefreshRate;
            cdme.format = (uint32_t)pFsMode->Format;
            cdme.scanLineOrdering = (uint32_t)pFsMode->ScanLineOrdering;
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        releaseAllBound();
        clearPeStateTracking();
        return hr32(dxmt9c_device_reset_ex(dev_, &cpp, pFsMode ? &cdme : nullptr));
    }

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT /*sc*/,
                                                D3DDISPLAYMODEEX* pMode,
                                                D3DDISPLAYROTATION* pRot) override {
        if (pMode) {
            D3DDISPLAYMODE mode{};
            if (SUCCEEDED(GetDisplayMode(0, &mode))) {
                pMode->Size = sizeof(*pMode);
                pMode->Width = mode.Width;
                pMode->Height = mode.Height;
                pMode->RefreshRate = mode.RefreshRate;
                pMode->Format = mode.Format;
                pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            } else {
                memset(pMode, 0, sizeof(*pMode));
            }
        }
        if (pRot)  *pRot = D3DDISPLAYROTATION_IDENTITY;
        return S_OK;
    }
};

/* =========================================================================
 * Factory function (called from factory.cpp)
 * ========================================================================= */

IDirect3DDevice9Ex* CreateDeviceImpl(D9CDevice* dev, IDirect3D9Ex* pFactory,
                                     UINT adapter, DWORD behaviorFlags,
                                     HWND window) {
    return new D3D9DeviceImpl(dev, pFactory, adapter, behaviorFlags, window);
}
