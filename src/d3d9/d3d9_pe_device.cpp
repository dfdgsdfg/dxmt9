/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex and resource COM wrappers.
 * All methods delegate to the dxmt9c_* C API from dxmt9/device_c.h. */

#include <atomic>
#include <algorithm>
#include <array>
#include <bit>
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

static D3DFORMAT exposeAdapterDisplayFormat(D3DFORMAT fmt) {
    if (fmt == D3DFMT_A8R8G8B8) return D3DFMT_X8R8G8B8;
    return fmt;
}

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
    // Phase 22: PE state shadow is now the DEFAULT path. Set* fast paths
    // shadow the value PE-side and embed it into the next draw packet
    // instead of bridging per-call. Opt-out via DXMT9_PE_STATE_SHADOW=0
    // for regression bisection. Same parsing convention as Phase 19's
    // DXMT9_PE_DRAW_CHUNK flip.
    static const bool enabled = []() {
        const auto value = dxmt9::util::getenvString("DXMT9_PE_STATE_SHADOW");
        if (value.empty()) return true;            // default ON
        if (value == "0") return false;            // explicit opt-out
        return true;
    }();
    return enabled;
}

static bool dxmt9PeDrawChunkEnabled() {
    // Phase 19: chunk recorder is now the DEFAULT path. The env var is
    // a regression-detection escape hatch — set DXMT9_PE_DRAW_CHUNK=0
    // to fall back to per-call bridge mode for bisecting issues.
    // getenvFlag returns false for unset / empty / "0", true otherwise.
    // We invert via DXMT9_PE_DRAW_CHUNK_DISABLE to match the explicit-
    // opt-out semantic without breaking existing scripts that set
    // DXMT9_PE_DRAW_CHUNK=1 (those keep working — getenvFlag still
    // returns true).
    static const bool enabled = []() {
        const auto value = dxmt9::util::getenvString("DXMT9_PE_DRAW_CHUNK");
        if (value.empty()) return true;            // default ON
        if (value == "0") return false;            // explicit opt-out
        return true;                               // any other value: ON
    }();
    return enabled;
}

// Phase 33: structural invariant — the DEFAULT chunk path (both
// DXMT9_PE_DRAW_CHUNK and DXMT9_PE_STATE_SHADOW unset, both default ON)
// has ZERO reachable code paths to dxmt9c_device_set_render_state /
// set_texture / set_stream_source / set_fvf / set_vs / set_ps /
// set_vertex_declaration / set_render_target / set_depth_stencil_surface
// / set_viewport / set_scissor_rect / set_texture_stage_state /
// set_sampler_state / set_material / set_clip_plane / set_transform /
// set_light / light_enable / set_indices unix-calls. Every Set* fast
// path is gated by `if (dxmt9PeStateShadowEnabled())` (early return)
// or `if (dxmt9PeDrawChunkEnabled())` (early return). The remaining
// hr32(dxmt9c_device_set_*) tail calls are reached only when the
// matching env var is set to "0" (regression escape hatch). Audited
// at Phase 33 commit; new Set*-style code MUST follow the same
// pattern (chunk-mode early return → PE shadow update only).
//
// Phase 16: full-snapshot mode. When set, every draw packet emitted in
// chunk-recorder mode carries the COMPLETE BaseDrawState snapshot (every
// field marked valid + populated from the PE shadow), not just the
// delta-since-last-packet. Wire size grows (typical packet jumps from
// ~100B to ~1KB) but the importer becomes idempotent — every packet is
// self-contained and can be replayed independently of prior packets.
// Off (default) keeps the delta optimization that makes run-coalescing
// detection cheap (packetHasNoStateDelta == "all valid bits zero").
static bool dxmt9PeFullSnapshotEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_DRAW_FULL_SNAPSHOT");
    return enabled;
}

static bool isValidD3DStateBlockType(D3DSTATEBLOCKTYPE type) {
    return type == D3DSBT_ALL || type == D3DSBT_PIXELSTATE || type == D3DSBT_VERTEXSTATE;
}

static bool isUnknownFormat(D3DFORMAT fmt) {
    return fmt == D3DFMT_UNKNOWN;
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
        if (data && size == sizeof(IUnknown*)) {
            hr = storage.setInterface(guid, static_cast<const IUnknown*>(data));
        }
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
    HRESULT hr = storage.getData(guid, &localSize, data);
    if (size && hr != D3DERR_NOTFOUND) {
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

static HRESULT validateSharedHandleForTexture(bool extended,
                                              HANDLE* sharedHandle,
                                              D3DPOOL pool,
                                              UINT levels,
                                              bool allowSystemMemUserMemory) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool == D3DPOOL_SYSTEMMEM) {
        if (!allowSystemMemUserMemory || levels != 1) return D3DERR_INVALIDCALL;
        return D3DERR_INVALIDCALL;
    }
    if (pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    return E_NOTIMPL;
}

static HRESULT validateSharedHandleForBuffer(bool extended,
                                             HANDLE* sharedHandle,
                                             D3DPOOL pool) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool != D3DPOOL_DEFAULT) return D3DERR_NOTAVAILABLE;
    return E_NOTIMPL;
}

static HRESULT validateSharedHandleForSurface(bool extended,
                                              HANDLE* sharedHandle,
                                              D3DPOOL pool,
                                              bool allowSystemMemUserMemory) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool == D3DPOOL_SYSTEMMEM) {
        if (!allowSystemMemUserMemory) return D3DERR_INVALIDCALL;
        return S_OK;
    }
    if (pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    return E_NOTIMPL;
}

static HRESULT validateSharedHandleForDefaultSurface(bool extended,
                                                     HANDLE* sharedHandle) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    return E_NOTIMPL;
}

static D9CRect toR(const RECT& r) {
    D9CRect c; c.left = r.left; c.top = r.top;
    c.right = r.right; c.bottom = r.bottom;
    return c;
}

struct D3D9PeRecorderFlush {
    virtual HRESULT FlushPeRecorderForChild() = 0;
    virtual bool IsStateBlockRecordingForChild() const = 0;
    // Phase 20: child COM wrappers (Query, Surface, Buffer) that want to
    // emit fire-and-forget records into the device's pending chunk go
    // through these. Returns true from IsChunkRecorderEnabledForChild()
    // when DXMT9_PE_DRAW_CHUNK is on; AppendRecordForChild forwards into
    // the device's appendCommandRecord (same pre-flush + size cap
    // handling as the device's own records).
    virtual bool IsChunkRecorderEnabledForChild() const = 0;
    virtual HRESULT AppendRecordForChild(const void* data, size_t bytes) = 0;

protected:
    ~D3D9PeRecorderFlush() = default;
};

static HRESULT flushChildRecorder(D3D9PeRecorderFlush* recorder) {
    return recorder ? recorder->FlushPeRecorderForChild() : S_OK;
}

static bool isChildStateBlockRecording(D3D9PeRecorderFlush* recorder) {
    return recorder && recorder->IsStateBlockRecordingForChild();
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)          ||
            IsEqualGUID(riid, IID_IDirect3DResource9)||
            IsEqualGUID(riid, IID_IDirect3DSurface9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) noexcept override {
        return setPrivateData(privateData_, guid, data, size, flags, "surface", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) noexcept override {
        return getPrivateData(privateData_, guid, data, size, "surface", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
        return freePrivateData(privateData_, guid, "surface", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() noexcept override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override { return D3DRTYPE_SURFACE; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (!container_) { *ppv = nullptr; return D3DERR_INVALIDCALL; }
        return container_->QueryInterface(riid, ppv);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* pD) noexcept override {
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
                                        DWORD flags) noexcept override {
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
    HRESULT STDMETHODCALLTYPE UnlockRect() noexcept override {
        dxmt9DeviceDebugLog("surface_unlock_rect surface=%p", this);
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_surface_unlock_rect(s_));
    }
    HRESULT STDMETHODCALLTYPE GetDC(HDC* phdc) noexcept override {
        dxmt9DeviceDebugLog("surface_get_dc surface=%p phdc=%p", this, phdc);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) noexcept override {
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)              ||
            IsEqualGUID(riid, IID_IDirect3DResource9)    ||
            IsEqualGUID(riid, IID_IDirect3DBaseTexture9) ||
            IsEqualGUID(riid, IID_IDirect3DTexture9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) noexcept override {
        return setPrivateData(privateData_, guid, data, size, flags, "texture", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) noexcept override {
        return getPrivateData(privateData_, guid, data, size, "texture", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
        return freePrivateData(privateData_, guid, "texture", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() noexcept override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override { return D3DRTYPE_TEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD()      noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() noexcept override {
        return dxmt9c_texture_get_level_count(t_);
    }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) noexcept override { return S_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() noexcept override { return D3DTEXF_LINEAR; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return;
        dxmt9c_texture_generate_mip_sublevels(t_);
    }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* pD) noexcept override {
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
                                               IDirect3DSurface9** ppS) noexcept override {
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
                                        const RECT* pRect, DWORD flags) noexcept override {
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
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_texture_unlock_rect(t_, level));
    }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) noexcept override { return S_OK; }
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

    bool subresourceIndex(D3DCUBEMAP_FACES face, UINT level, UINT& out) const {
        const UINT mipCount = dxmt9c_texture_get_level_count(t_);
        if (static_cast<UINT>(face) >= 6u || level >= mipCount) {
            return false;
        }
        out = static_cast<UINT>(face) * mipCount + level;
        return true;
    }

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)               ||
            IsEqualGUID(riid, IID_IDirect3DResource9)     ||
            IsEqualGUID(riid, IID_IDirect3DBaseTexture9)  ||
            IsEqualGUID(riid, IID_IDirect3DCubeTexture9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) noexcept override {
        return setPrivateData(privateData_, guid, data, size, flags, "cube", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) noexcept override {
        return getPrivateData(privateData_, guid, data, size, "cube", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
        return freePrivateData(privateData_, guid, "cube", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() noexcept override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override { return D3DRTYPE_CUBETEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD()      noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() noexcept override {
        return dxmt9c_texture_get_level_count(t_);
    }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) noexcept override { return S_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() noexcept override { return D3DTEXF_LINEAR; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return;
        dxmt9c_texture_generate_mip_sublevels(t_);
    }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* pD) noexcept override {
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
                                                 IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("cube_get_surface_level this=%p face=%u level=%u",
                            this, static_cast<unsigned>(face), level);
        UINT idx = 0;
        if (!subresourceIndex(face, level, idx)) {
            dxmt9DeviceDebugLog("cube_get_surface_level this=%p face=%u level=%u -> invalid",
                                this, static_cast<unsigned>(face), level);
            return D3DERR_INVALIDCALL;
        }
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
                                        DWORD flags) noexcept override {
        if (!pLR) return D3DERR_INVALIDCALL;
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        UINT idx = 0;
        if (!subresourceIndex(face, level, idx)) return D3DERR_INVALIDCALL;
        D9CLockedRect lr{}; D9CRect cr{};
        if (pRect) cr = toR(*pRect);
        HRESULT hr = hr32(dxmt9c_texture_lock_rect(t_, idx, &lr,
                          pRect ? &cr : nullptr, flags));
        if (SUCCEEDED(hr)) { pLR->Pitch = lr.pitch; pLR->pBits = lr.bits; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        UINT idx = 0;
        if (!subresourceIndex(face, level, idx)) return D3DERR_INVALIDCALL;
        return hr32(dxmt9c_texture_unlock_rect(t_, idx));
    }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES, const RECT*) noexcept override { return S_OK; }
};

/* ── Volume ───────────────────────────────────────────────────────────────── */

class D3D9VolumeImpl final : public IDirect3DVolume9 {
    ULONG refs_ = 1;
    D9CTexture* t_;
    IDirect3DDevice9* device_;
    IUnknown* container_;
    D3D9PeRecorderFlush* recorder_;
    UINT level_;
    dxmt9::util::ComPrivateData privateData_{};
public:
    D3D9VolumeImpl(D9CTexture* t,
                   IDirect3DDevice9* device,
                   IUnknown* container,
                   D3D9PeRecorderFlush* recorder,
                   UINT level)
        : t_(t), device_(device), container_(container), recorder_(recorder), level_(level) {
        if (t_) dxmt9c_texture_addref(t_);
        if (device_) device_->AddRef();
        if (container_) container_->AddRef();
    }
    ~D3D9VolumeImpl() {
        if (t_) dxmt9c_texture_release(t_);
        if (container_) container_->Release();
        if (device_) device_->Release();
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DVolume9)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data,
                                             DWORD size, DWORD flags) noexcept override {
        return setPrivateData(privateData_, guid, data, size, flags, "volume-level", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) noexcept override {
        return getPrivateData(privateData_, guid, data, size, "volume-level", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
        return freePrivateData(privateData_, guid, "volume-level", this);
    }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (!container_) {
            *ppv = nullptr;
            return D3DERR_INVALIDCALL;
        }
        return container_->QueryInterface(riid, ppv);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC* pD) noexcept override {
        if (!pD) return D3DERR_INVALIDCALL;
        D9CSurfaceDesc sd{};
        const HRESULT hr = hr32(dxmt9c_texture_get_level_desc(t_, level_, &sd));
        if (FAILED(hr)) return hr;
        pD->Format = static_cast<D3DFORMAT>(sd.format);
        pD->Type = D3DRTYPE_VOLUME;
        pD->Usage = sd.usage;
        pD->Pool = static_cast<D3DPOOL>(sd.pool);
        pD->Width = sd.width;
        pD->Height = sd.height;
        pD->Depth = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX*, const D3DBOX*, DWORD) noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE UnlockBox() noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return E_NOTIMPL;
    }
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)                ||
            IsEqualGUID(riid, IID_IDirect3DResource9)      ||
            IsEqualGUID(riid, IID_IDirect3DBaseTexture9)   ||
            IsEqualGUID(riid, IID_IDirect3DVolumeTexture9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) noexcept override {
        return setPrivateData(privateData_, guid, data, size, flags, "volume", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) noexcept override {
        return getPrivateData(privateData_, guid, data, size, "volume", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
        return freePrivateData(privateData_, guid, "volume", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() noexcept override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override { return D3DRTYPE_VOLUMETEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD()      noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() noexcept override {
        return dxmt9c_texture_get_level_count(t_);
    }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) noexcept override { return S_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() noexcept override { return D3DTEXF_LINEAR; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return;
        dxmt9c_texture_generate_mip_sublevels(t_);
    }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT, D3DVOLUME_DESC* pD) noexcept override {
        if (pD) memset(pD, 0, sizeof(*pD)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT level, IDirect3DVolume9** ppVolume) noexcept override {
        if (!ppVolume) return D3DERR_INVALIDCALL;
        *ppVolume = nullptr;
        if (level >= dxmt9c_texture_get_level_count(t_)) return D3DERR_INVALIDCALL;
        *ppVolume = new D3D9VolumeImpl(t_, device_, static_cast<IDirect3DBaseTexture9*>(this), recorder_, level);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockBox(UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX*) noexcept override { return S_OK; }
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)              ||
            IsEqualGUID(riid, IID_IDirect3DResource9)    ||
            IsEqualGUID(riid, IID_IDirect3DVertexBuffer9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) noexcept override {
        return setPrivateData(privateData_, guid, data, size, flags, "vb", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) noexcept override {
        return getPrivateData(privateData_, guid, data, size, "vb", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
        return freePrivateData(privateData_, guid, "vb", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() noexcept override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override { return D3DRTYPE_VERTEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void** pp, DWORD flags) noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
    }
    HRESULT STDMETHODCALLTYPE Unlock() noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_buffer_unlock(b_));
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* pDesc) noexcept override {
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)             ||
            IsEqualGUID(riid, IID_IDirect3DResource9)   ||
            IsEqualGUID(riid, IID_IDirect3DIndexBuffer9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void* data,DWORD size,DWORD flags) noexcept override {
        return setPrivateData(privateData_, guid, data, size, flags, "ib", this);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void* data,DWORD* size) noexcept override {
        return getPrivateData(privateData_, guid, data, size, "ib", this);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
        return freePrivateData(privateData_, guid, "ib", this);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() noexcept override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override { return D3DRTYPE_INDEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void** pp, DWORD flags) noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
    }
    HRESULT STDMETHODCALLTYPE Unlock() noexcept override {
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_buffer_unlock(b_));
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* pDesc) noexcept override {
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DVertexShader9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFunction(void* pData, UINT* pSize) noexcept override {
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DPixelShader9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFunction(void* pData, UINT* pSize) noexcept override {
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DVertexDeclaration9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
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
                                              UINT* pCount) noexcept override {
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DQuery9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    D3DQUERYTYPE STDMETHODCALLTYPE GetType()     noexcept override { return (D3DQUERYTYPE)dxmt9c_query_get_type(q_); }
    DWORD        STDMETHODCALLTYPE GetDataSize()  noexcept override { return dxmt9c_query_get_data_size(q_); }
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) noexcept override {
        // Phase 20: Query::Issue (D3DISSUE_BEGIN / D3DISSUE_END) is
        // fire-and-forget — server records it into the query object,
        // PE caller doesn't wait. Chunk-record path keeps it ordered
        // with surrounding draws within the same chunk; legacy path
        // falls back to flush+bridge.
        if (recorder_ && recorder_->IsChunkRecorderEnabledForChild()) {
            D9CCommandRecordQueryIssue record{};
            record.header.type = D9C_COMMAND_RECORD_QUERY_ISSUE;
            record.header.size = sizeof(record);
            record.queryWire = reinterpret_cast<uint64_t>(q_);
            record.flags = static_cast<uint32_t>(flags);
            return recorder_->AppendRecordForChild(&record, sizeof(record));
        }
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_query_issue(q_, flags));
    }
    HRESULT STDMETHODCALLTYPE GetData(void* pData, DWORD size, DWORD flags) noexcept override {
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

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override {
        const ULONG refs = refs_.fetch_add(1) + 1;
        dxmt9DeviceDebugLog("stateblock_addref this=%p sb=%p refs=%u",
                            this, static_cast<void*>(sb_), (unsigned)refs);
        return refs;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG refs = refs_.fetch_sub(1) - 1;
        dxmt9DeviceDebugLog("stateblock_release this=%p sb=%p refs=%u",
                            this, static_cast<void*>(sb_), (unsigned)refs);
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DStateBlock9)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Capture() noexcept override {
        dxmt9DeviceDebugLog("stateblock_capture sb=%p", this);
        if (isChildStateBlockRecording(recorder_)) {
            return D3DERR_INVALIDCALL;
        }
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_stateblock_capture(sb_));
        dxmt9DeviceDebugLog("stateblock_capture -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Apply() noexcept override {
        dxmt9DeviceDebugLog("stateblock_apply sb=%p", this);
        if (isChildStateBlockRecording(recorder_)) {
            return D3DERR_INVALIDCALL;
        }
        const HRESULT flushHr = flushChildRecorder(recorder_);
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_stateblock_apply(sb_));
        dxmt9DeviceDebugLog("stateblock_apply -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
};

/* ── SwapChain ────────────────────────────────────────────────────────────── */

class D3D9SwapChainImpl final : public IDirect3DSwapChain9Ex {
    ULONG        refs_ = 1;
    D9CSwapChain* sc_;
    IDirect3DDevice9* device_;
    D3D9PeRecorderFlush* recorder_;
    bool extended_ = false;
public:
    D3D9SwapChainImpl(D9CSwapChain* sc,
                      IDirect3DDevice9* device,
                      D3D9PeRecorderFlush* recorder = nullptr,
                      bool extended = false)
        : sc_(sc), device_(device), recorder_(recorder), extended_(extended) {
        if (device_) device_->AddRef();
    }
    ~D3D9SwapChainImpl() {
        dxmt9c_swapchain_release(sc_);
        if (device_) device_->Release();
    }

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IDirect3DSwapChain9)) {
            *ppv = static_cast<IDirect3DSwapChain9*>(this); AddRef(); return S_OK;
        }
        if (IsEqualGUID(riid, IID_IDirect3DSwapChain9Ex)) {
            if (!extended_) {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            *ppv = static_cast<IDirect3DSwapChain9Ex*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty,
                                       DWORD flags) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9*) noexcept override {
        return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT idx, D3DBACKBUFFER_TYPE,
                                             IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("swapchain_get_back_buffer sc=%p idx=%u", this, idx);
        D9CSurface* s = dxmt9c_swapchain_get_back_buffer(sc_, idx, 0);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, device_, static_cast<IDirect3DSwapChain9*>(this), recorder_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* p) noexcept override {
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* p) noexcept override {
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
        p->Format = exposeAdapterDisplayFormat(static_cast<D3DFORMAT>(cpp.backBufferFormat));
        dxmt9DeviceDebugLog("swapchain_get_display_mode -> %ux%u fmt=%u hz=%u",
                            p->Width, p->Height, (unsigned)p->Format, p->RefreshRate);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) noexcept override {
        if (!pp) return D3DERR_INVALIDCALL;
        if (!device_) {
            *pp = nullptr;
            return D3DERR_INVALIDCALL;
        }
        device_->AddRef();
        *pp = device_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* pPP) noexcept override {
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

    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) noexcept override {
        if (pLastPresentCount) *pLastPresentCount = 0u;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS* pPresentationStatistics) noexcept override {
        if (pPresentationStatistics) {
            memset(pPresentationStatistics, 0, sizeof(*pPresentationStatistics));
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX* pMode,
                                                D3DDISPLAYROTATION* pRotation) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        if (pMode->Size != sizeof(D3DDISPLAYMODEEX)) return D3DERR_INVALIDCALL;
        D3DDISPLAYMODE mode{};
        const HRESULT hr = GetDisplayMode(&mode);
        if (FAILED(hr)) return hr;
        pMode->Size = sizeof(D3DDISPLAYMODEEX);
        pMode->Width = mode.Width;
        pMode->Height = mode.Height;
        pMode->RefreshRate = mode.RefreshRate;
        pMode->Format = mode.Format;
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
        if (pRotation) *pRotation = D3DDISPLAYROTATION_IDENTITY;
        return S_OK;
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

static uint64_t d9cWireHandleValue(const D9CWireHandle& handle) {
    return static_cast<uint64_t>(handle.lo) |
           (static_cast<uint64_t>(handle.hi) << 32);
}

/* =========================================================================
 * D3D9DeviceImpl — IDirect3DDevice9Ex
 * ========================================================================= */

class D3D9DeviceImpl final : public IDirect3DDevice9Ex, public D3D9PeRecorderFlush {
    static_assert(sizeof(D9CCommandChunkWireHeader) ==
                  D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE);
    static_assert(sizeof(D9CCommandChunkWireRecordHeader) ==
                  D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE);
    static_assert(sizeof(D9CCommandChunkWireHandleEntry) ==
                  D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE);

    // Phase 21: chunk-flush thresholds. Defaults match what the PE
    // recorder has been tuned around since Phase 5 (64 records = a few
    // dozen draws + their state setters; 256 KB ≈ one full vertex
    // upload for a complex draw + headers). Both are env-overridable
    // via DXMT9_PE_CHUNK_MAX_RECORDS / DXMT9_PE_CHUNK_MAX_BYTES; the
    // helpers below cap the env values to prevent pathological inputs
    // from blowing chunk-side allocations.
    static constexpr UINT kDefaultMaxPendingCommandRecords = 64;
    static constexpr size_t kDefaultMaxPendingCommandBytes = 256 * 1024;
    static constexpr UINT kAbsoluteMaxPendingCommandRecords = 4096;
    static constexpr size_t kAbsoluteMaxPendingCommandBytes = 16 * 1024 * 1024;
    static UINT maxPendingCommandRecords() {
        static const UINT cached = []() -> UINT {
            const auto envValue = dxmt9::util::getenvU32("DXMT9_PE_CHUNK_MAX_RECORDS");
            if (!envValue || *envValue == 0) return kDefaultMaxPendingCommandRecords;
            return std::min<UINT>(*envValue, kAbsoluteMaxPendingCommandRecords);
        }();
        return cached;
    }
    static size_t maxPendingCommandBytes() {
        static const size_t cached = []() -> size_t {
            const auto envValue = dxmt9::util::getenvU64("DXMT9_PE_CHUNK_MAX_BYTES");
            if (!envValue || *envValue == 0) return kDefaultMaxPendingCommandBytes;
            return std::min<size_t>(*envValue, kAbsoluteMaxPendingCommandBytes);
        }();
        return cached;
    }

    static constexpr uint32_t kPeRenderStateSlots = 256;
    static constexpr uint32_t kPeTextureStageSlots = 8;
    static constexpr uint32_t kPeTextureStageStateSlots = 64;
    static constexpr uint32_t kPeSamplerSlots = 16;
    static constexpr uint32_t kPeSamplerStateSlots = 64;

    template<std::size_t Slots>
    struct FixedStateTable {
        std::array<uint32_t, Slots> values{};
        std::array<uint64_t, (Slots + 63u) / 64u> occupied{};
        uint32_t count = 0;

        static constexpr bool valid(uint32_t slot) noexcept {
            return slot < Slots;
        }
        static constexpr uint64_t bit(uint32_t slot) noexcept {
            return 1ull << (slot & 63u);
        }
        static constexpr std::size_t word(uint32_t slot) noexcept {
            return slot >> 6;
        }
        bool contains(uint32_t slot) const noexcept {
            return valid(slot) && (occupied[word(slot)] & bit(slot)) != 0;
        }
        bool empty() const noexcept {
            return count == 0;
        }
        uint32_t size() const noexcept {
            return count;
        }
        bool get(uint32_t slot, uint32_t& value) const noexcept {
            if (!contains(slot)) {
                return false;
            }
            value = values[slot];
            return true;
        }
        void set(uint32_t slot, uint32_t value) noexcept {
            if (!valid(slot)) {
                return;
            }
            const auto w = word(slot);
            const auto b = bit(slot);
            if ((occupied[w] & b) == 0) {
                occupied[w] |= b;
                ++count;
            }
            values[slot] = value;
        }
        void erase(uint32_t slot) noexcept {
            if (!contains(slot)) {
                return;
            }
            occupied[word(slot)] &= ~bit(slot);
            --count;
        }
        void clear() noexcept {
            occupied = {};
            count = 0;
        }
        template<typename Fn>
        void forEach(Fn&& fn) const {
            for (std::size_t w = 0; w < occupied.size(); ++w) {
                uint64_t bits = occupied[w];
                while (bits != 0) {
                    const auto b = static_cast<uint32_t>(std::countr_zero(bits));
                    const auto slot = static_cast<uint32_t>(w * 64u + b);
                    if (slot < Slots) {
                        fn(slot, values[slot]);
                    }
                    bits &= bits - 1u;
                }
            }
        }
        bool popFirst(uint32_t& slot, uint32_t& value) noexcept {
            for (std::size_t w = 0; w < occupied.size(); ++w) {
                uint64_t bits = occupied[w];
                if (bits == 0) {
                    continue;
                }
                const auto b = static_cast<uint32_t>(std::countr_zero(bits));
                slot = static_cast<uint32_t>(w * 64u + b);
                value = values[slot];
                occupied[w] &= ~bit(slot);
                --count;
                return true;
            }
            return false;
        }
    };

    template<std::size_t Rows, std::size_t Slots>
    struct FixedStateMatrix {
        std::array<FixedStateTable<Slots>, Rows> rows{};
        uint32_t count = 0;

        static constexpr bool valid(uint32_t row, uint32_t slot) noexcept {
            return row < Rows && slot < Slots;
        }
        bool contains(uint32_t row, uint32_t slot) const noexcept {
            return valid(row, slot) && rows[row].contains(slot);
        }
        bool empty() const noexcept {
            return count == 0;
        }
        uint32_t size() const noexcept {
            return count;
        }
        bool get(uint32_t row, uint32_t slot, uint32_t& value) const noexcept {
            return valid(row, slot) && rows[row].get(slot, value);
        }
        void set(uint32_t row, uint32_t slot, uint32_t value) noexcept {
            if (!valid(row, slot)) {
                return;
            }
            if (!rows[row].contains(slot)) {
                ++count;
            }
            rows[row].set(slot, value);
        }
        void clear() noexcept {
            for (auto& row : rows) {
                row.clear();
            }
            count = 0;
        }
        template<typename Fn>
        void forEach(Fn&& fn) const {
            for (uint32_t row = 0; row < Rows; ++row) {
                rows[row].forEach([&](uint32_t slot, uint32_t value) {
                    fn(row, slot, value);
                });
            }
        }
        bool popFirst(uint32_t& row, uint32_t& slot, uint32_t& value) noexcept {
            for (uint32_t r = 0; r < Rows; ++r) {
                if (rows[r].popFirst(slot, value)) {
                    row = r;
                    --count;
                    return true;
                }
            }
            return false;
        }
    };

    static uint32_t textureStageSlot(DWORD stage) noexcept {
        return std::min<uint32_t>(stage, kPeTextureStageSlots - 1u);
    }
    static uint32_t textureStageStateSlot(D3DTEXTURESTAGESTATETYPE type) noexcept {
        return std::min<uint32_t>(static_cast<uint32_t>(type),
                                  kPeTextureStageStateSlots - 1u);
    }
    static bool samplerSlot(DWORD sampler, uint32_t& slot) noexcept {
        if (sampler >= kPeSamplerSlots) {
            return false;
        }
        slot = sampler;
        return true;
    }
    static bool samplerStateSlot(D3DSAMPLERSTATETYPE type, uint32_t& slot) noexcept {
        slot = static_cast<uint32_t>(type);
        return slot < kPeSamplerStateSlots;
    }

    ULONG        refs_    = 1;
    D9CDevice*   dev_;
    IDirect3D9Ex* factory_;
    UINT         adapter_ = 0;
    D3DDEVTYPE   deviceType_ = D3DDEVTYPE_HAL;
    DWORD        behaviorFlags_ = 0;
    bool         extended_ = false;
    bool         stateBlockRecording_ = false;

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

    FixedStateTable<kPeRenderStateSlots> renderStateShadow_{};
    FixedStateTable<kPeRenderStateSlots> pendingRenderStates_{};
    FixedStateTable<kPeRenderStateSlots> stateBlockRenderStateRestore_{};
    DWORD pendingTextureMask_ = 0;
    DWORD pendingStreamMask_ = 0;
    bool pendingFvf_ = false;
    // Phase 12: shader-handle delta flags. PE-side SetVS/SetPS update the
    // shadow + set the matching pending bit; the next built draw packet
    // ships vsValid=1 / psValid=1 with vs_/ps_ snapshot, then clears.
    bool pendingVs_ = false;
    bool pendingPs_ = false;
    // Phase 12: vertex-decl handle delta (alternative to fvf).
    bool pendingVdecl_ = false;
    // Phase 12: index buffer handle delta (rides on indexed packets).
    bool pendingIb_ = false;
    // Phase 12: render-target / depth-stencil deltas. RT mask covers up to
    // 4 slots; rt[0] storage is rt0_ (legacy), rt[1..3] in rtSlots_.
    DWORD pendingRtMask_ = 0;
    bool pendingDs_ = false;
    IDirect3DSurface9* rtSlots_[4]{};
    IDirect3DSurface9* dsSurface_ = nullptr;
    // Phase 12: viewport / scissor shadow + pending flags.
    bool pendingViewport_ = false;
    bool pendingScissor_ = false;
    D9CViewport viewportShadow_{};
    D9CRect scissorShadow_{};
    // Phase 12: TSS + SamplerState deltas. These are fixed row x state
    // tables with occupancy masks; pending tables are the dirty masks
    // drained into the next draw/apply packet.
    FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots> pendingTss_{};
    FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots> pendingSamplerStates_{};
    // Identity-no-op shadow: last value sent for each row/state tuple.
    FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots> tssShadow_{};
    FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots> samplerStateShadow_{};
    // Phase 12: Material + ClipPlane shadow.
    bool pendingMaterial_ = false;
    D9CMaterial materialShadow_{};
    DWORD pendingClipPlaneMask_ = 0;
    float clipPlaneShadow_[6 * 4]{};
    // Phase 12: Transform delta — keyed by D3DTRANSFORMSTATETYPE
    // (variable enum range, so map keyed instead of fixed array).
    std::unordered_map<uint32_t, D9CMatrix> pendingTransforms_{};
    std::unordered_map<uint32_t, D9CMatrix> transformShadow_{};
    // Phase 12: Light delta — bit i ⇒ lightShadow_[i] is fresh.
    DWORD pendingLightSlotMask_ = 0;
    D9CLight lightShadow_[D9C_DRAW_PACKET_MAX_LIGHTS]{};
    // Phase 12: LightEnable delta. ValidMask = which slots have a fresh
    // enable value this packet; pendingLightEnableMask_ holds the enable
    // bit per slot. lightEnableShadow_ tracks the most recently set value
    // per slot (for identity-no-op detection).
    DWORD pendingLightEnableValidMask_ = 0;
    DWORD pendingLightEnableMask_ = 0;
    DWORD lightEnableShadow_ = 0;
    std::vector<std::uint8_t> pendingCommandBytes_{};
    std::vector<D9CCommandChunkWireRecordHeader> pendingCommandWireRecords_{};
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

    // Legacy append-time retention notes, one dedup'd container per kind.
    // The DOD wire blob now gets per-record ranges from flush-time payload
    // decoding; these sets are kept for existing noteChunkHandle callers
    // and cleared with the chunk.
    std::unordered_set<uint64_t> chunkHandlesByKind_[5]{};
    std::vector<D9CCommandChunkWireHandleEntry> chunkHandlesPayload_{};
    std::vector<std::uint8_t> pendingCommandWireBlob_{};

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
        for (auto& rt : rtSlots_)   setRef(rt, (IDirect3DSurface9*)nullptr);
        setRef(dsSurface_, (IDirect3DSurface9*)nullptr);
    }

    void clearPeStateTracking() {
        renderStateShadow_.clear();
        tssShadow_.clear();
        samplerStateShadow_.clear();
        clearPendingHotState();
        pendingCommandBytes_.clear();
        pendingCommandWireRecords_.clear();
        pendingCommandWireBlob_.clear();
        chunkHandlesPayload_.clear();
        pendingCommandRecordCount_ = 0;
        fvf_ = 0;
        std::memset(streamOff_, 0, sizeof(streamOff_));
        std::memset(streamStr_, 0, sizeof(streamStr_));
        std::memset(streamFreq_, 0, sizeof(streamFreq_));
    }

    bool hasPendingHotState() const {
        return !pendingRenderStates_.empty() || pendingTextureMask_ != 0 ||
               pendingStreamMask_ != 0 || pendingFvf_ ||
               pendingVs_ || pendingPs_ || pendingVdecl_ ||
               pendingIb_ || pendingRtMask_ != 0 || pendingDs_ ||
               pendingViewport_ || pendingScissor_ ||
               !pendingTss_.empty() || !pendingSamplerStates_.empty() ||
               pendingMaterial_ || pendingClipPlaneMask_ != 0 ||
               !pendingTransforms_.empty() ||
               pendingLightSlotMask_ != 0 ||
               pendingLightEnableValidMask_ != 0;
    }

    void clearPendingHotState() {
        pendingRenderStates_.clear();
        pendingTextureMask_ = 0;
        pendingStreamMask_ = 0;
        pendingFvf_ = false;
        pendingVs_ = false;
        pendingPs_ = false;
        pendingVdecl_ = false;
        pendingIb_ = false;
        pendingRtMask_ = 0;
        pendingDs_ = false;
        pendingViewport_ = false;
        pendingScissor_ = false;
        pendingTss_.clear();
        pendingSamplerStates_.clear();
        pendingMaterial_ = false;
        pendingClipPlaneMask_ = 0;
        pendingTransforms_.clear();
        pendingLightSlotMask_ = 0;
        pendingLightEnableValidMask_ = 0;
        pendingLightEnableMask_ = 0;
    }

    bool shadowedRenderStateEquals(DWORD state, DWORD value) const {
        uint32_t shadowValue = 0;
        return renderStateShadow_.get(state, shadowValue) && shadowValue == value;
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
        pendingRenderStates_.forEach([&](uint32_t state, uint32_t value) {
            auto& entry = packet.renderStates[packet.renderStateCount++];
            entry.state = state;
            entry.value = value;
        });

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
        // Phase 12: shader-handle delta. Server-side applyDrawPacketState
        // dispatches dxmt9c_device_set_vertex_shader / set_pixel_shader
        // when valid=1, mirroring the renderState/texture/stream pattern.
        packet.vsValid = pendingVs_ ? 1u : 0u;
        packet.vsHandle = toWireHandle(rawVS(vs_));
        packet.psValid = pendingPs_ ? 1u : 0u;
        packet.psHandle = toWireHandle(rawPS(ps_));
        packet.vdeclValid = pendingVdecl_ ? 1u : 0u;
        packet.vdeclHandle = toWireHandle(rawVD(vdecl_));
        // RT delta — emit handle for every set bit. Slot 0 is rt0_ if
        // ever populated; slots 1..3 are rtSlots_[i]. The legacy SetRT
        // path doesn't populate rt0_ separately, so always use rtSlots_.
        packet.rtMask = pendingRtMask_;
        for (DWORD slot = 0; slot < 4; ++slot) {
            packet.rtHandles[slot] = (pendingRtMask_ & (1u << slot))
                                          ? toWireHandle(rawSurf(rtSlots_[slot]))
                                          : D9CWireHandle{};
        }
        packet.dsValid = pendingDs_ ? 1u : 0u;
        packet.dsHandle = toWireHandle(rawSurf(dsSurface_));
        packet.viewportValid = pendingViewport_ ? 1u : 0u;
        packet.viewport = viewportShadow_;
        packet.scissorValid = pendingScissor_ ? 1u : 0u;
        packet.scissor = scissorShadow_;
        // Phase 12: drain TSS / SamplerState pending tables into packet
        // delta arrays. The cap check inside Set* already flushes the
        // chunk if a single Set would push beyond the per-packet limit;
        // here we just emit what's pending.
        if (pendingTss_.size() > D9C_DRAW_PACKET_MAX_TSS ||
            pendingSamplerStates_.size() > D9C_DRAW_PACKET_MAX_SAMPLER) {
            return false;
        }
        packet.tssCount = static_cast<uint32_t>(pendingTss_.size());
        uint32_t tssIdx = 0;
        pendingTss_.forEach([&](uint32_t stage, uint32_t state, uint32_t value) {
            packet.tss[tssIdx].stage = stage;
            packet.tss[tssIdx].type = state;
            packet.tss[tssIdx].value = value;
            ++tssIdx;
        });
        packet.samplerStateCount = static_cast<uint32_t>(pendingSamplerStates_.size());
        uint32_t ssIdx = 0;
        pendingSamplerStates_.forEach([&](uint32_t sampler, uint32_t state, uint32_t value) {
            packet.samplerStates[ssIdx].sampler = sampler;
            packet.samplerStates[ssIdx].type = state;
            packet.samplerStates[ssIdx].value = value;
            ++ssIdx;
        });
        // Phase 12: material + clip-plane deltas. Material rides as a
        // single struct + valid flag; clip planes ride as a 6-bit mask
        // + flat 6×4 float array (only set bits' slots are
        // semantically meaningful, but the array is fixed-size so the
        // packet layout stays simple).
        packet.materialValid = pendingMaterial_ ? 1u : 0u;
        packet.material = materialShadow_;
        packet.clipPlaneMask = pendingClipPlaneMask_;
        std::memcpy(packet.clipPlanes, clipPlaneShadow_, sizeof(packet.clipPlanes));
        // Phase 12: Transform delta — drain pending map (per-frame typically
        // a handful: View, Projection, a few World/Texture transforms).
        // Cap check: > MAX_TRANSFORMS forces chunk seal upstream.
        if (pendingTransforms_.size() > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
            return false;
        }
        packet.transformCount = static_cast<uint32_t>(pendingTransforms_.size());
        uint32_t txIdx = 0;
        for (const auto& [state, matrix] : pendingTransforms_) {
            packet.transforms[txIdx].state = state;
            packet.transforms[txIdx].reserved = 0;
            packet.transforms[txIdx].matrix = matrix;
            ++txIdx;
        }
        // Phase 12: Light + LightEnable deltas. Light slot mask carries
        // the per-slot full D9CLight payload (set bit ⇒ lights[slot] is
        // semantically meaningful). LightEnable delta is two parallel
        // masks: ValidMask says "this slot has a fresh enable" and
        // LightEnableMask carries the new value.
        packet.lightSlotMask = pendingLightSlotMask_;
        for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
            if ((pendingLightSlotMask_ & (1u << slot)) != 0) {
                packet.lights[slot] = lightShadow_[slot];
            }
        }
        packet.lightEnableValidMask = pendingLightEnableValidMask_;
        packet.lightEnableMask = pendingLightEnableMask_;
        // Phase 16: full-snapshot mode — override every delta field with
        // the complete shadow snapshot. The importer applies whatever
        // valid bits are set, so flipping every bit + populating from
        // the existing PE shadow gives a self-contained packet without
        // requiring any importer changes. We respect the per-array caps;
        // a shadow that overflows (e.g. > 64 distinct render states)
        // returns false to force the chunk to seal.
        if (dxmt9PeFullSnapshotEnabled()) {
            // Render states: drain the entire shadow table.
            if (renderStateShadow_.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
                return false;
            }
            packet.renderStateCount = 0;
            renderStateShadow_.forEach([&](uint32_t state, uint32_t value) {
                auto& entry = packet.renderStates[packet.renderStateCount++];
                entry.state = state;
                entry.value = value;
            });
            // Texture / RT / Stream — set mask bits for every populated slot.
            packet.textureMask = 0;
            for (DWORD stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
                if (textures_[stage] != nullptr) {
                    packet.textureMask |= 1u << stage;
                    packet.textures[stage] = toWireHandle(rawTex(textures_[stage]));
                }
            }
            packet.streamSourceMask = 0;
            for (DWORD stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
                if (streamSrc_[stream] != nullptr) {
                    packet.streamSourceMask |= 1u << stream;
                    auto& s = packet.streamSources[stream];
                    s.buffer = toWireHandle(rawVBuf(streamSrc_[stream]));
                    s.offset = streamOff_[stream];
                    s.stride = streamStr_[stream];
                }
            }
            packet.rtMask = 0;
            for (DWORD slot = 0; slot < 4; ++slot) {
                if (rtSlots_[slot] != nullptr) {
                    packet.rtMask |= 1u << slot;
                    packet.rtHandles[slot] = toWireHandle(rawSurf(rtSlots_[slot]));
                }
            }
            // Scalar valid bits: emit shadow contents unconditionally.
            packet.fvfValid = 1u;
            packet.fvf = fvf_;
            packet.vsValid = 1u;
            packet.vsHandle = toWireHandle(rawVS(vs_));
            packet.psValid = 1u;
            packet.psHandle = toWireHandle(rawPS(ps_));
            packet.vdeclValid = 1u;
            packet.vdeclHandle = toWireHandle(rawVD(vdecl_));
            packet.dsValid = 1u;
            packet.dsHandle = toWireHandle(rawSurf(dsSurface_));
            packet.viewportValid = 1u;
            packet.viewport = viewportShadow_;
            packet.scissorValid = 1u;
            packet.scissor = scissorShadow_;
            // TSS / SamplerState — drain shadow tables fully.
            if (tssShadow_.size() > D9C_DRAW_PACKET_MAX_TSS ||
                samplerStateShadow_.size() > D9C_DRAW_PACKET_MAX_SAMPLER ||
                transformShadow_.size() > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
                return false;
            }
            packet.tssCount = 0;
            tssShadow_.forEach([&](uint32_t stage, uint32_t state, uint32_t value) {
                auto& e = packet.tss[packet.tssCount++];
                e.stage = stage;
                e.type = state;
                e.value = value;
            });
            packet.samplerStateCount = 0;
            samplerStateShadow_.forEach([&](uint32_t sampler, uint32_t state, uint32_t value) {
                auto& e = packet.samplerStates[packet.samplerStateCount++];
                e.sampler = sampler;
                e.type = state;
                e.value = value;
            });
            packet.materialValid = 1u;
            packet.material = materialShadow_;
            // Clip planes: emit every slot with mask = 0x3F (all 6).
            packet.clipPlaneMask = 0x3Fu;
            std::memcpy(packet.clipPlanes, clipPlaneShadow_,
                        sizeof(packet.clipPlanes));
            // Transforms: drain shadow.
            packet.transformCount = 0;
            for (const auto& [state, matrix] : transformShadow_) {
                auto& t = packet.transforms[packet.transformCount++];
                t.state = state;
                t.reserved = 0;
                t.matrix = matrix;
            }
            // Lights: emit every slot.
            packet.lightSlotMask = (1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u;
            for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
                packet.lights[i] = lightShadow_[i];
            }
            packet.lightEnableValidMask = (1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u;
            packet.lightEnableMask = lightEnableShadow_;
        }
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

    // Preserve existing append-time handle notes for code paths that
    // still call them. Wire serialization now derives record ranges from
    // the payload arena at flush time.
    void noteChunkHandle(uint32_t kind, uint64_t handle) {
        if (!dxmt9PeDrawChunkEnabled() || handle == 0 || kind > 4) {
            return;
        }
        chunkHandlesByKind_[kind].insert(handle);
    }

    using WireHandleEntryList = std::vector<D9CCommandChunkWireHandleEntry>;

    static bool appendRecordWireHandle(WireHandleEntryList& handles,
                                       uint32_t kind,
                                       uint64_t handle) {
        if (handle == 0) {
            return true;
        }
        if (kind > D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
            return false;
        }
        for (const auto& existing : handles) {
            if (existing.kind == kind && existing.opaqueHandle == handle) {
                return true;
            }
        }
        handles.push_back(D9CCommandChunkWireHandleEntry{
            .kind = kind,
            .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
            .opaqueHandle = handle,
            .reserved0 = 0,
            .reserved1 = 0,
        });
        return true;
    }

    static bool appendDrawPacketWireHandles(const D9CDrawPrimitivePacket& packet,
                                            WireHandleEntryList& handles) {
        for (uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
            if ((packet.textureMask & (1u << stage)) != 0 &&
                !appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                        d9cWireHandleValue(packet.textures[stage]))) {
                return false;
            }
        }
        for (uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            if ((packet.streamSourceMask & (1u << stream)) != 0 &&
                !appendRecordWireHandle(
                    handles, D9C_CHUNK_HANDLE_KIND_BUFFER,
                    d9cWireHandleValue(packet.streamSources[stream].buffer))) {
                return false;
            }
        }
        for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
            if ((packet.rtMask & (1u << slot)) != 0 &&
                !appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                        d9cWireHandleValue(packet.rtHandles[slot]))) {
                return false;
            }
        }
        if (packet.dsValid != 0 &&
            !appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                    d9cWireHandleValue(packet.dsHandle))) {
            return false;
        }
        return true;
    }

    static bool appendIndexedDrawPacketWireHandles(
        const D9CDrawIndexedPrimitivePacket& packet,
        WireHandleEntryList& handles) {
        if (!appendDrawPacketWireHandles(packet.state, handles)) {
            return false;
        }
        if (packet.ibValid != 0 &&
            !appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                    d9cWireHandleValue(packet.ibHandle))) {
            return false;
        }
        return true;
    }

    // Draw records consume the effective server state, not only the
    // handles present in their delta packet. Set* fast paths flush the
    // current chunk before mutating these shadows, so the flush-time
    // bindings are a conservative per-draw retention subset.
    bool appendCurrentlyBoundDrawHandles(WireHandleEntryList& handles) {
        for (auto* tex : textures_) {
            if (auto* raw = rawTex(tex); raw != nullptr) {
                if (!appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                            reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        for (auto* vb : streamSrc_) {
            if (auto* raw = rawVBuf(vb); raw != nullptr) {
                if (!appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                            reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        if (auto* raw = rawIBuf(indexBuf_); raw != nullptr) {
            if (!appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                        reinterpret_cast<uint64_t>(raw))) {
                return false;
            }
        }
        for (auto* surf : rtSlots_) {
            if (auto* raw = rawSurf(surf); raw != nullptr) {
                if (!appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                            reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        if (auto* raw = rawSurf(dsSurface_); raw != nullptr) {
            if (!appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                        reinterpret_cast<uint64_t>(raw))) {
                return false;
            }
        }
        // VS/PS/Vdecl have no pool retention table on the server side
        // (importer's markChunkResources skips SHADER / VERTEX_DECL
        // kinds), so emitting them here would be inert. Leaving them
        // out keeps the wire payload tight.
        return true;
    }

    bool appendCurrentlyBoundClearHandles(WireHandleEntryList& handles,
                                          uint32_t flags) {
        if ((flags & D3DCLEAR_TARGET) != 0) {
            for (auto* surf : rtSlots_) {
                if (auto* raw = rawSurf(surf); raw != nullptr) {
                    if (!appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                                reinterpret_cast<uint64_t>(raw))) {
                        return false;
                    }
                }
            }
        }
        if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0) {
            if (auto* raw = rawSurf(dsSurface_); raw != nullptr) {
                if (!appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                            reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        return true;
    }

    bool collectRecordWireHandles(const D9CCommandChunkWireRecordHeader& wireRecord,
                                  WireHandleEntryList& handles) {
        const auto payloadArenaSize =
            static_cast<uint32_t>(pendingCommandBytes_.size());
        if (!d9c_command_chunk_wire_payload_range_valid(
                payloadArenaSize, wireRecord.payloadOffset,
                wireRecord.payloadSize)) {
            return false;
        }
        const auto* payload = wireRecord.payloadSize == 0
            ? nullptr
            : pendingCommandBytes_.data() + wireRecord.payloadOffset;

        switch (wireRecord.type) {
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawPrimitive)) {
                return false;
            }
            D9CCommandRecordDrawPrimitive decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendDrawPacketWireHandles(decoded.packet, handles) &&
                   appendCurrentlyBoundDrawHandles(handles);
        }
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawIndexedPrimitive)) {
                return false;
            }
            D9CCommandRecordDrawIndexedPrimitive decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendIndexedDrawPacketWireHandles(decoded.packet, handles) &&
                   appendCurrentlyBoundDrawHandles(handles);
        }
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawPrimitiveUP)) {
                return false;
            }
            D9CCommandRecordDrawPrimitiveUP decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendDrawPacketWireHandles(decoded.packet.state, handles) &&
                   appendCurrentlyBoundDrawHandles(handles);
        }
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawIndexedPrimitiveUP)) {
                return false;
            }
            D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendDrawPacketWireHandles(decoded.packet.state, handles) &&
                   appendCurrentlyBoundDrawHandles(handles);
        }
        case D9C_COMMAND_RECORD_APPLY_STATE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordApplyState)) {
                return false;
            }
            D9CCommandRecordApplyState decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendDrawPacketWireHandles(decoded.packet, handles);
        }
        case D9C_COMMAND_RECORD_CLEAR: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordClear)) {
                return false;
            }
            D9CCommandRecordClear decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendCurrentlyBoundClearHandles(handles, decoded.flags);
        }
        case D9C_COMMAND_RECORD_STRETCH_RECT: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordStretchRect)) {
                return false;
            }
            D9CCommandRecordStretchRect decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.srcWire) &&
                   appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.dstWire);
        }
        case D9C_COMMAND_RECORD_COLOR_FILL: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordColorFill)) {
                return false;
            }
            D9CCommandRecordColorFill decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.surfaceWire);
        }
        case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordUpdateTexture)) {
                return false;
            }
            D9CCommandRecordUpdateTexture decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                          decoded.srcWire) &&
                   appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                          decoded.dstWire);
        }
        case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordUpdateSurface)) {
                return false;
            }
            D9CCommandRecordUpdateSurface decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.srcWire) &&
                   appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.dstWire);
        }
        case D9C_COMMAND_RECORD_READBACK: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordReadback)) {
                return false;
            }
            D9CCommandRecordReadback decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.srcWire) &&
                   appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.dstWire);
        }
        default:
            return true;
        }
    }

    HRESULT flushPendingCommandChunk() {
        if (!dxmt9PeDrawChunkEnabled() || pendingCommandRecordCount_ == 0) {
            return S_OK;
        }
        if (pendingCommandWireRecords_.size() != pendingCommandRecordCount_) {
            return D3DERR_INVALIDCALL;
        }
        const auto wireRecordCount =
            static_cast<std::uint32_t>(pendingCommandWireRecords_.size());
        const auto payloadArenaSize =
            static_cast<std::uint32_t>(pendingCommandBytes_.size());

        // Build per-record contiguous handle ranges. Entries are deduped
        // within a record; duplicates across records are allowed so each
        // header can point at exactly the subset it needs without falling
        // back to the full chunk table.
        chunkHandlesPayload_.clear();
        for (auto& record : pendingCommandWireRecords_) {
            WireHandleEntryList recordHandles;
            if (!collectRecordWireHandles(record, recordHandles)) {
                return D3DERR_INVALIDCALL;
            }
            if (chunkHandlesPayload_.size() >
                static_cast<size_t>(0xffffffffull) - recordHandles.size()) {
                return D3DERR_INVALIDCALL;
            }
            record.firstHandle =
                static_cast<std::uint32_t>(chunkHandlesPayload_.size());
            record.handleCount =
                static_cast<std::uint32_t>(recordHandles.size());
            chunkHandlesPayload_.insert(chunkHandlesPayload_.end(),
                                        recordHandles.begin(),
                                        recordHandles.end());
        }
        const auto wireHandleCount =
            static_cast<std::uint32_t>(chunkHandlesPayload_.size());

        const auto recordTableBytes =
            static_cast<std::uint64_t>(wireRecordCount) *
            D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
        const auto handleTableBytes =
            static_cast<std::uint64_t>(wireHandleCount) *
            D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
        const auto payloadArenaOffset =
            static_cast<std::uint64_t>(D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE) +
            recordTableBytes + handleTableBytes;
        const auto wireBlobSize =
            payloadArenaOffset + static_cast<std::uint64_t>(payloadArenaSize);
        if (recordTableBytes > 0xffffffffull ||
            handleTableBytes > 0xffffffffull ||
            payloadArenaOffset > 0xffffffffull ||
            wireBlobSize > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }

        pendingCommandWireBlob_.assign(
            static_cast<size_t>(wireBlobSize), std::uint8_t{0});
        D9CCommandChunkWireHeader wireHeader{};
        wireHeader.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
        wireHeader.headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
        wireHeader.recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
        wireHeader.handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
        wireHeader.recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
        wireHeader.recordCount = wireRecordCount;
        wireHeader.handleTableOffset =
            wireHeader.recordTableOffset + static_cast<std::uint32_t>(recordTableBytes);
        wireHeader.handleCount = wireHandleCount;
        wireHeader.payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset);
        wireHeader.payloadArenaSize = payloadArenaSize;

        std::memcpy(pendingCommandWireBlob_.data(), &wireHeader, sizeof(wireHeader));
        if (recordTableBytes != 0) {
            std::memcpy(
                pendingCommandWireBlob_.data() + wireHeader.recordTableOffset,
                pendingCommandWireRecords_.data(),
                static_cast<size_t>(recordTableBytes));
        }
        if (handleTableBytes != 0) {
            std::memcpy(
                pendingCommandWireBlob_.data() + wireHeader.handleTableOffset,
                chunkHandlesPayload_.data(),
                static_cast<size_t>(handleTableBytes));
        }
        if (payloadArenaSize != 0) {
            std::memcpy(
                pendingCommandWireBlob_.data() + wireHeader.payloadArenaOffset,
                pendingCommandBytes_.data(),
                payloadArenaSize);
        }

        D9CCommandChunk chunk{};
        chunk.version = D9C_COMMAND_CHUNK_VERSION;
        chunk.recordCount = wireRecordCount;
        chunk.recordBytes = static_cast<std::uint32_t>(pendingCommandWireBlob_.size());
        chunk.records = toWireHandle(pendingCommandWireBlob_.data());
        chunk.handleCount = wireHandleCount;
        chunk.handles = D9CWireHandle{};

        const HRESULT hr = hr32(dxmt9c_device_commit_chunk(dev_, &chunk));
        if (SUCCEEDED(hr)) {
            pendingCommandBytes_.clear();
            pendingCommandWireRecords_.clear();
            pendingCommandWireBlob_.clear();
            chunkHandlesPayload_.clear();
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
            (pendingCommandRecordCount_ >= maxPendingCommandRecords() ||
             pendingCommandBytes_.size() + bytes > maxPendingCommandBytes())) {
            const HRESULT flushHr = flushPendingCommandChunk();
            if (FAILED(flushHr)) return flushHr;
        }

        const auto* raw = static_cast<const std::uint8_t*>(data);
        const auto payloadOffset = pendingCommandBytes_.size();
        if (payloadOffset > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        pendingCommandBytes_.insert(pendingCommandBytes_.end(), raw, raw + bytes);
        D9CCommandChunkWireRecordHeader wireRecord{};
        if (bytes >= sizeof(D9CCommandRecordHeader)) {
            D9CCommandRecordHeader legacyHeader{};
            std::memcpy(&legacyHeader, data, sizeof(legacyHeader));
            wireRecord.type = legacyHeader.type;
        }
        wireRecord.flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE;
        wireRecord.payloadOffset = static_cast<std::uint32_t>(payloadOffset);
        wireRecord.payloadSize = static_cast<std::uint32_t>(bytes);
        wireRecord.firstHandle = 0;
        wireRecord.handleCount = 0;
        pendingCommandWireRecords_.push_back(wireRecord);
        ++pendingCommandRecordCount_;

        if (pendingCommandRecordCount_ >= maxPendingCommandRecords() ||
            pendingCommandBytes_.size() >= maxPendingCommandBytes()) {
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
        // Phase 12: index buffer delta. Server applies before
        // dxmt9c_device_draw_indexed_primitive.
        record.packet.ibValid = pendingIb_ ? 1u : 0u;
        record.packet.ibHandle = toWireHandle(rawIBuf(indexBuf_));
        pendingIb_ = false;
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
        // Phase 28: mode-aware. Chunk mode drains pending state into the
        // chunk via chunkBarrierFlush() (records, never bridge calls),
        // then seals. Legacy mode keeps the bridge-emit pattern.
        if (dxmt9PeDrawChunkEnabled()) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
            return flushPendingCommandChunk();
        }
        // Legacy fallback: drain consts as records, seal, bridge-emit
        // hot state so the upcoming per-call bridge sees current
        // server state.
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

    // Phase 28: chunk-mode barrier flush. Replaces flushPendingHotState's
    // bridge-emit path with a chunk-record path that preserves the
    // "Set* never crosses PE/unix in default chunk mode" invariant.
    //
    // Drains pending consts (existing per-record stream) THEN, if hot
    // state is pending, packages the delta into a D9C_COMMAND_RECORD_
    // APPLY_STATE record + appends to the chunk + clears the pending
    // bits. Server importer dispatches APPLY_STATE via the same
    // applyDrawPacketState() that draw records use, so the server
    // shadow is updated before the upcoming barrier record runs.
    //
    // Caller still appends the actual barrier record afterwards;
    // chunk-commit flushes everything in the recorded order.
    HRESULT chunkBarrierFlush() {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        if (!hasPendingHotState()) {
            return S_OK;
        }
        D9CCommandRecordApplyState record{};
        record.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
        record.header.size = sizeof(record);
        // Fast path: single APPLY_STATE record covers all pending
        // state. After Phase 31 cap-checks at every Set* fast path,
        // this is the only path that runs in practice.
        if (buildDrawPrimitivePacket(D3DPT_POINTLIST, 0, 0, record.packet)) {
            const HRESULT appendHr = appendCommandRecord(&record, sizeof(record));
            if (FAILED(appendHr)) return appendHr;
            clearPendingHotState();
            return S_OK;
        }
        // Over-cap slow path: a Set* somewhere bypassed the cap check
        // (regression). Drain pending oversized collections in batches
        // of cap-size records. Critical safety property: every pending
        // state bit MUST be represented in the chunk before the caller
        // appends a barrier record. Sealing-and-deferring (the prior
        // behavior) lets the barrier observe stale server state.
        return drainOversizedPendingStateAsApplyStateRecords();
    }

    HRESULT drainOversizedPendingStateAsApplyStateRecords() {
        // Drain the four cappable collections (renderStates, tss,
        // samplerStates, transforms) in batches of cap-size. Each batch
        // becomes one APPLY_STATE record carrying ONLY that collection's
        // batch (other fields zero / unset). Server's applyDrawPacketState
        // is idempotent for unset fields so empty validX/maskX are safe.
        auto drainMap = [&](auto& pendingMap, auto cap, auto fillEntry,
                            auto packetCountField) -> HRESULT {
            while (!pendingMap.empty()) {
                D9CCommandRecordApplyState rec{};
                rec.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
                rec.header.size = sizeof(rec);
                std::uint32_t n = 0;
                while (!pendingMap.empty() && n < cap) {
                    auto it = pendingMap.begin();
                    fillEntry(rec.packet, n, it->first, it->second);
                    ++n;
                    pendingMap.erase(it);
                }
                packetCountField(rec.packet) = n;
                const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        auto drainTable = [&](auto& pendingTable, auto cap, auto fillEntry,
                              auto packetCountField) -> HRESULT {
            while (!pendingTable.empty()) {
                D9CCommandRecordApplyState rec{};
                rec.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
                rec.header.size = sizeof(rec);
                std::uint32_t n = 0;
                uint32_t key = 0;
                uint32_t value = 0;
                while (n < cap && pendingTable.popFirst(key, value)) {
                    fillEntry(rec.packet, n, key, value);
                    ++n;
                }
                packetCountField(rec.packet) = n;
                const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        auto drainMatrix = [&](auto& pendingMatrix, auto cap, auto fillEntry,
                               auto packetCountField) -> HRESULT {
            while (!pendingMatrix.empty()) {
                D9CCommandRecordApplyState rec{};
                rec.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
                rec.header.size = sizeof(rec);
                std::uint32_t n = 0;
                uint32_t row = 0;
                uint32_t key = 0;
                uint32_t value = 0;
                while (n < cap && pendingMatrix.popFirst(row, key, value)) {
                    fillEntry(rec.packet, n, row, key, value);
                    ++n;
                }
                packetCountField(rec.packet) = n;
                const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        if (auto hr = drainTable(pendingRenderStates_,
                                 (uint32_t)D9C_DRAW_PACKET_MAX_RENDER_STATES,
                                 [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                    uint32_t k, uint32_t v) {
                                     p.renderStates[i].state = k;
                                     p.renderStates[i].value = v;
                                 },
                                 [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                     return p.renderStateCount;
                                 });
            FAILED(hr)) return hr;
        if (auto hr = drainMatrix(pendingTss_,
                                  (uint32_t)D9C_DRAW_PACKET_MAX_TSS,
                                  [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                     uint32_t row, uint32_t k, uint32_t v) {
                                      p.tss[i].stage = row;
                                      p.tss[i].type = k;
                                      p.tss[i].value = v;
                                  },
                                  [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                      return p.tssCount;
                                  });
            FAILED(hr)) return hr;
        if (auto hr = drainMatrix(pendingSamplerStates_,
                                  (uint32_t)D9C_DRAW_PACKET_MAX_SAMPLER,
                                  [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                     uint32_t row, uint32_t k, uint32_t v) {
                                      p.samplerStates[i].sampler = row;
                                      p.samplerStates[i].type = k;
                                      p.samplerStates[i].value = v;
                                  },
                                  [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                      return p.samplerStateCount;
                                  });
            FAILED(hr)) return hr;
        if (auto hr = drainMap(pendingTransforms_,
                               (uint32_t)D9C_DRAW_PACKET_MAX_TRANSFORMS,
                               [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                  uint32_t k, const D9CMatrix& v) {
                                   p.transforms[i].state = k;
                                   p.transforms[i].reserved = 0;
                                   p.transforms[i].matrix = v;
                               },
                               [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                   return p.transformCount;
                               });
            FAILED(hr)) return hr;
        // Remaining scalar pending bits (texture / stream / vs / ps /
        // vdecl / RT / DS / viewport / scissor / fvf / material / clip
        // / lights / lightEnable) all fit in one packet. After draining
        // the four cappable collections above, buildDrawPrimitivePacket
        // succeeds.
        if (!hasPendingHotState()) {
            return S_OK;
        }
        D9CCommandRecordApplyState tail{};
        tail.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
        tail.header.size = sizeof(tail);
        if (!buildDrawPrimitivePacket(D3DPT_POINTLIST, 0, 0, tail.packet)) {
            // Truly should never happen — the four cappable collections
            // are now empty. Defensive: log + return failure rather than
            // silently leaving pending state dirty (which would let the
            // upcoming barrier observe stale server state).
            dxmt9DeviceDebugLog(
                "ERR: drainOversizedPendingStateAsApplyStateRecords could "
                "not build tail APPLY_STATE — pending state lost. Caller "
                "should treat as recorder failure.");
            return D3DERR_INVALIDCALL;
        }
        const HRESULT hr = appendCommandRecord(&tail, sizeof(tail));
        if (FAILED(hr)) return hr;
        clearPendingHotState();
        return S_OK;
    }

    // Phase 28: bridge-emit pending hot state via per-call setter unix-
    // calls. LEGACY FALLBACK ONLY — chunk mode must not reach the
    // bridge-emit branch below; chunkBarrierFlush() is the chunk-mode
    // equivalent that emits an APPLY_STATE record into the chunk
    // instead. Callers that follow up with a per-call bridge use
    // flushPeRecorder() (which routes mode-aware: chunkBarrierFlush
    // for chunk, this function for legacy).
    HRESULT flushPendingHotState() {
        const HRESULT chunkHr = flushPendingCommandChunk();
        if (FAILED(chunkHr)) {
            return chunkHr;
        }
        if (!dxmt9PeStateShadowEnabled() || !hasPendingHotState()) {
            return S_OK;
        }
        // Defensive: log if anyone manages to enter the bridge-emit
        // branch under chunk mode. Should never fire after Phase 28
        // (every chunk-mode call site routes via chunkBarrierFlush or
        // mode-aware flushPeRecorder); a hit means a regression slipped
        // a Set* bridge call back into the chunk path.
        if (dxmt9PeDrawChunkEnabled()) {
            dxmt9DeviceDebugLog(
                "WARN: flushPendingHotState bridge-emit reached in chunk "
                "mode — Set* invariant violation. Caller bypassed "
                "chunkBarrierFlush / flushPeRecorder.");
        }

        HRESULT pendingHr = S_OK;
        pendingRenderStates_.forEach([&](uint32_t state, uint32_t value) {
            if (FAILED(pendingHr)) {
                return;
            }
            pendingHr = hr32(dxmt9c_device_set_render_state(dev_, state, value));
        });
        if (FAILED(pendingHr)) {
            return pendingHr;
        }
        pendingRenderStates_.clear();

        pendingTss_.forEach([&](uint32_t stage, uint32_t state, uint32_t value) {
            if (FAILED(pendingHr)) {
                return;
            }
            pendingHr = hr32(dxmt9c_device_set_texture_stage_state(dev_, stage, state, value));
        });
        if (FAILED(pendingHr)) {
            return pendingHr;
        }
        pendingTss_.clear();

        pendingSamplerStates_.forEach([&](uint32_t sampler, uint32_t state, uint32_t value) {
            if (FAILED(pendingHr)) {
                return;
            }
            pendingHr = hr32(dxmt9c_device_set_sampler_state(dev_, sampler, state, value));
        });
        if (FAILED(pendingHr)) {
            return pendingHr;
        }
        pendingSamplerStates_.clear();

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
    HRESULT FlushPeRecorderForChild() noexcept override {
        return flushPeRecorder();
    }
    bool IsStateBlockRecordingForChild() const noexcept override {
        return stateBlockRecording_;
    }
    bool IsChunkRecorderEnabledForChild() const override {
        return dxmt9PeDrawChunkEnabled();
    }
    HRESULT AppendRecordForChild(const void* data, size_t bytes) noexcept override {
        return appendCommandRecord(data, bytes);
    }

    D3D9DeviceImpl(D9CDevice* dev, IDirect3D9Ex* factory,
                   UINT adapter, D3DDEVTYPE deviceType, DWORD behaviorFlags,
                   HWND window, bool extended)
        : dev_(dev), factory_(factory)
        , adapter_(adapter), deviceType_(deviceType), behaviorFlags_(behaviorFlags)
        , extended_(extended)
        , creationWindow_(window) {
        if (factory_) factory_->AddRef();
        for (UINT& freq : streamFreq_) {
            freq = 1;
        }
        dxmt9DeviceDebugLog("device_ctor this=%p dev=%p factory=%p adapter=%u devType=%u behavior=0x%x window=%p extended=%u",
                            this, static_cast<void*>(dev_), static_cast<void*>(factory_),
                            adapter_, (unsigned)deviceType_, (unsigned)behaviorFlags_, window, extended_ ? 1u : 0u);
    }

    ~D3D9DeviceImpl() {
        (void)flushPeRecorder();
        releaseAllBound();
        dxmt9c_device_release(dev_);
        if (factory_) factory_->Release();
    }

    /* ── IUnknown ── */

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)          ||
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
        *ppv = nullptr; return E_NOINTERFACE;
    }

    /* ── device info ── */

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() noexcept override {
        dxmt9DeviceDebugLog("device_test_cooperative_level device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_test_cooperative_level(dev_));
        dxmt9DeviceDebugLog("device_test_cooperative_level -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() noexcept override {
        dxmt9DeviceDebugLog("device_get_available_texture_mem device=%p", this);
        const UINT value = 0x80000000u;
        dxmt9DeviceDebugLog("device_get_available_texture_mem -> %u (0x%x)",
                            value, (unsigned)value);
        return value;
    }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D) noexcept override {
        if (!ppD3D) return D3DERR_INVALIDCALL;
        factory_->AddRef();
        *ppD3D = static_cast<IDirect3D9*>(factory_);
        dxmt9DeviceDebugLog("device_get_direct3d this=%p -> factory=%p", this, static_cast<void*>(*ppD3D));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) noexcept override {
        if (!pCaps) return D3DERR_INVALIDCALL;
        D9CCaps cc{};
        HRESULT hr = hr32(dxmt9c_device_get_caps(dev_, &cc));
        if (SUCCEEDED(hr)) {
            FillD3DCaps9(cc, pCaps);
            pCaps->DeviceType = deviceType_;
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

    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT sc, D3DDISPLAYMODE* pMode) noexcept override {
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
        pMode->Format = exposeAdapterDisplayFormat(static_cast<D3DFORMAT>(cpp.backBufferFormat));
        dxmt9DeviceDebugLog("device_get_display_mode -> %ux%u fmt=%u hz=%u",
                            pMode->Width, pMode->Height, (unsigned)pMode->Format, pMode->RefreshRate);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCreationParameters(
            D3DDEVICE_CREATION_PARAMETERS* pParams) noexcept override {
        if (!pParams) return D3DERR_INVALIDCALL;
        pParams->AdapterOrdinal  = adapter_;
        pParams->DeviceType      = deviceType_;
        pParams->hFocusWindow    = creationWindow_;
        pParams->BehaviorFlags   = behaviorFlags_;
        return S_OK;
    }

    /* ── cursor (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* surface) noexcept override {
        dxmt9DeviceDebugLog("device_set_cursor_properties device=%p x=%u y=%u surface=%p",
                            this, x, y, surface);
        return S_OK;
    }
    void    STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) noexcept override {
        dxmt9DeviceDebugLog("device_set_cursor_position device=%p x=%d y=%d flags=0x%x",
                            this, x, y, (unsigned)flags);
    }
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL show) noexcept override {
        dxmt9DeviceDebugLog("device_show_cursor device=%p show=%u", this, (unsigned)show);
        return FALSE;
    }

    /* ── swap chains ── */

    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(
            D3DPRESENT_PARAMETERS* pPP, IDirect3DSwapChain9** ppSC) noexcept override {
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
        *ppSC = new D3D9SwapChainImpl(sc, this, this, extended_);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index,
                                            IDirect3DSwapChain9** ppSC) noexcept override {
        if (!ppSC) return D3DERR_INVALIDCALL;
        D9CSwapChain* sc = dxmt9c_device_get_swap_chain(dev_, index);
        if (!sc) return D3DERR_INVALIDCALL;
        *ppSC = new D3D9SwapChainImpl(sc, this, this, extended_);
        return S_OK;
    }

    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() noexcept override {
        return dxmt9c_device_get_swap_chain_count(dev_);
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPP) noexcept override {
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
        stateBlockRecording_ = false;
        stateBlockRenderStateRestore_.clear();
        return hr32(dxmt9c_device_reset(dev_, &cpp));
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty) noexcept override {
        dxmt9DeviceDebugLog("device_present device=%p wnd=%p src=%s dst=%s dirty=%p",
                            this, wnd,
                            src ? "<custom>" : "<full>",
                            dst ? "<custom>" : "<full>",
                            dirty);
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        // Recorder-design Present: append a PRESENT record to the
        // current chunk after draining hot state + const dirty ranges,
        // then commit the chunk synchronously. The server-side
        // importer dispatches dxmt9c_device_present after replaying
        // every preceding draw / clear / state in the chunk — so
        // ordering is preserved and Present serves as the natural
        // chunk boundary. Dirty-region payload is dropped (the
        // backend present path doesn't consume it).
        if (dxmt9PeDrawChunkEnabled()) {
            // Phase 28: chunk-mode barrier — flush pending hot state
            // + consts as records into the chunk, never as bridge calls.
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;

            D9CCommandRecordPresent record{};
            record.header.type = D9C_COMMAND_RECORD_PRESENT;
            record.header.size = sizeof(record);
            record.hwnd = (uint64_t)(uintptr_t)wnd;
            record.flags = 0;
            record.hasSrc = src ? 1u : 0u;
            record.hasDst = dst ? 1u : 0u;
            if (src) record.src = cs;
            if (dst) record.dst = cd;
            const HRESULT appendHr = appendCommandRecord(&record, sizeof(record));
            if (FAILED(appendHr)) return appendHr;
            // Force-commit so Present runs at the bridge boundary even
            // if the chunk is below the byte/record threshold.
            return flushPendingCommandChunk();
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_present(dev_,
            src ? &cs : nullptr, dst ? &cd : nullptr,
            (uint64_t)(uintptr_t)wnd, dirty, 0));
    }

    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT idx,
                                             D3DBACKBUFFER_TYPE,
                                             IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_back_buffer device=%p sc=%u idx=%u", this, sc, idx);
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, sc);
        if (!chain) return D3DERR_INVALIDCALL;
        D9CSurface* s = dxmt9c_swapchain_get_back_buffer(chain, idx, 0);
        if (!s) {
            dxmt9c_swapchain_release(chain);
            return D3DERR_INVALIDCALL;
        }
        auto* swapchain = new D3D9SwapChainImpl(chain, this, this, extended_);
        *ppS = new D3D9SurfaceImpl(s, this, static_cast<IDirect3DSwapChain9*>(swapchain), this);
        swapchain->Release();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT, D3DRASTER_STATUS* p) noexcept override {
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enableDialogs) noexcept override {
        dxmt9DeviceDebugLog("device_set_dialog_box_mode device=%p enable=%u", this, (unsigned)enableDialogs);
        return S_OK;
    }
    void    STDMETHODCALLTYPE SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP*) noexcept override {
        dxmt9DeviceDebugLog("device_set_gamma_ramp device=%p swapChain=%u flags=0x%x",
                            this, swapChain, (unsigned)flags);
    }
    void    STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP* p) noexcept override {
        if (p) memset(p, 0, sizeof(*p));
    }

    /* ── resource creation ── */

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels,
                                             DWORD usage, D3DFORMAT fmt,
                                             D3DPOOL pool,
                                             IDirect3DTexture9** ppTex,
                                             HANDLE* psh) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        *ppTex = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, true);
        if (FAILED(sharedHr)) return sharedHr;
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
                                                   HANDLE* psh) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        *ppTex = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
        if (FAILED(sharedHr)) return sharedHr;
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
                                                 HANDLE* psh) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        *ppTex = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
        if (FAILED(sharedHr)) return sharedHr;
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
                                                  HANDLE* psh) noexcept override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        *ppBuf = nullptr;
        const HRESULT sharedHr = validateSharedHandleForBuffer(extended_, psh, pool);
        if (FAILED(sharedHr)) return sharedHr;
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
                                                 HANDLE* psh) noexcept override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        *ppBuf = nullptr;
        const HRESULT sharedHr = validateSharedHandleForBuffer(extended_, psh, pool);
        if (FAILED(sharedHr)) return sharedHr;
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
                                                  HANDLE* psh) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForDefaultSurface(extended_, psh);
        if (FAILED(sharedHr)) return sharedHr;
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
                                                         HANDLE* psh) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForDefaultSurface(extended_, psh);
        if (FAILED(sharedHr)) return sharedHr;
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
                                             const POINT* dstPt) noexcept override {
        dxmt9DeviceDebugLog("device_update_surface device=%p src=%p dst=%p srcRect=%s dstPt=%s",
                            this, src, dst,
                            srcRect ? "<custom>" : "<full>",
                            dstPt ? "<custom>" : "<origin>");
        D9CRect cs{}, cd{};
        if (srcRect) cs = toR(*srcRect);
        if (dstPt) { cd.left = dstPt->x; cd.top = dstPt->y;
                     cd.right = dstPt->x; cd.bottom = dstPt->y; }
        // Phase 15: chunk-recorder fast path — UpdateSurface is fire-and-
        // forget (no return data the PE caller waits on), so it rides as
        // a chunk record. Both surfaces are bulk-retained against the
        // chunk seqId to survive until the GPU consumes the copy.
        if (dxmt9PeDrawChunkEnabled()) {
            // Phase 28: chunk-mode barrier — flush pending hot state
            // + consts as records into the chunk, never as bridge calls.
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
            if (auto* raw = rawSurf(src); raw)
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                reinterpret_cast<uint64_t>(raw));
            if (auto* raw = rawSurf(dst); raw)
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                reinterpret_cast<uint64_t>(raw));
            D9CCommandRecordUpdateSurface record{};
            record.header.type = D9C_COMMAND_RECORD_UPDATE_SURFACE;
            record.header.size = sizeof(record);
            record.srcWire = reinterpret_cast<uint64_t>(rawSurf(src));
            record.dstWire = reinterpret_cast<uint64_t>(rawSurf(dst));
            record.hasSrcRect = srcRect ? 1u : 0u;
            record.hasDstPoint = dstPt ? 1u : 0u;
            record.srcRect = cs;
            record.dstPoint = cd;
            return appendCommandRecord(&record, sizeof(record));
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_update_surface(dev_,
            rawSurf(src), srcRect ? &cs : nullptr,
            rawSurf(dst), dstPt   ? &cd : nullptr));
    }

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,
                                             IDirect3DBaseTexture9* dst) noexcept override {
        if (dxmt9PeDrawChunkEnabled()) {
            // Phase 28: chunk-mode barrier — flush pending hot state
            // + consts as records into the chunk, never as bridge calls.
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
            if (auto* raw = rawTex(src); raw)
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                reinterpret_cast<uint64_t>(raw));
            if (auto* raw = rawTex(dst); raw)
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                reinterpret_cast<uint64_t>(raw));
            D9CCommandRecordUpdateTexture record{};
            record.header.type = D9C_COMMAND_RECORD_UPDATE_TEXTURE;
            record.header.size = sizeof(record);
            record.srcWire = reinterpret_cast<uint64_t>(rawTex(src));
            record.dstWire = reinterpret_cast<uint64_t>(rawTex(dst));
            return appendCommandRecord(&record, sizeof(record));
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_update_texture(dev_,
                    rawTex(src), rawTex(dst)));
    }

    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt,
                                                   IDirect3DSurface9* dst) noexcept override {
        dxmt9DeviceDebugLog("device_get_render_target_data device=%p rt=%p dst=%p",
                            this, rt, dst);
        // Phase 24: chunk-recorder path. The PE caller is synchronous —
        // the call doesn't return until the data is in dst — but
        // routing through the chunk record stream keeps ordering atomic
        // with surrounding draws/clears in the SAME chunk. We append a
        // READBACK record then commit the chunk synchronously (Present
        // pattern); commit_chunk's per-record short-circuit propagates
        // the actual readback HRESULT back to PE.
        if (dxmt9PeDrawChunkEnabled()) {
            // Phase 28: chunk-mode barrier — flush pending hot state
            // + consts as records into the chunk, never as bridge calls.
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
            if (auto* raw = rawSurf(rt); raw)
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                reinterpret_cast<uint64_t>(raw));
            if (auto* raw = rawSurf(dst); raw)
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                reinterpret_cast<uint64_t>(raw));
            D9CCommandRecordReadback record{};
            record.header.type = D9C_COMMAND_RECORD_READBACK;
            record.header.size = sizeof(record);
            record.srcWire = reinterpret_cast<uint64_t>(rawSurf(rt));
            record.dstWire = reinterpret_cast<uint64_t>(rawSurf(dst));
            const HRESULT appendHr = appendCommandRecord(&record, sizeof(record));
            if (FAILED(appendHr)) return appendHr;
            // Sync semantics: commit the chunk now and wait for
            // completion. flushPendingCommandChunk routes through
            // commit_chunk → server's record dispatcher → readback
            // record handler → dxmt9c_device_get_render_target_data
            // (which encodes + waits internally).
            return flushPendingCommandChunk();
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_get_render_target_data(dev_,
                    rawSurf(rt), rawSurf(dst)));
    }

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sc, IDirect3DSurface9* surface) noexcept override {
        dxmt9DeviceDebugLog("device_get_front_buffer_data device=%p sc=%u surface=%p",
                            this, sc, surface);
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src,
                                           const RECT* srcRect,
                                           IDirect3DSurface9* dst,
                                           const RECT* dstRect,
                                           D3DTEXTUREFILTERTYPE filter) noexcept override {
        dxmt9DeviceDebugLog("device_stretch_rect device=%p src=%p dst=%p filter=%u srcRect=%s dstRect=%s",
                            this, src, dst, (unsigned)filter,
                            srcRect ? "<custom>" : "<full>",
                            dstRect ? "<custom>" : "<full>");
        D9CRect cs{}, cd{};
        if (srcRect) cs = toR(*srcRect); if (dstRect) cd = toR(*dstRect);
        if (dxmt9PeDrawChunkEnabled()) {
            // Phase 28: chunk-mode barrier — flush pending hot state
            // + consts as records into the chunk, never as bridge calls.
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
            // Retain both surfaces against the chunk seqId so they
            // survive until the GPU consumes the blit.
            if (auto* raw = rawSurf(src); raw)
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                reinterpret_cast<uint64_t>(raw));
            if (auto* raw = rawSurf(dst); raw)
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                reinterpret_cast<uint64_t>(raw));
            D9CCommandRecordStretchRect record{};
            record.header.type = D9C_COMMAND_RECORD_STRETCH_RECT;
            record.header.size = sizeof(record);
            record.srcWire = reinterpret_cast<uint64_t>(rawSurf(src));
            record.dstWire = reinterpret_cast<uint64_t>(rawSurf(dst));
            record.hasSrcRect = srcRect ? 1u : 0u;
            record.hasDstRect = dstRect ? 1u : 0u;
            record.filter = (uint32_t)filter;
            if (srcRect) record.srcRect = cs;
            if (dstRect) record.dstRect = cd;
            return appendCommandRecord(&record, sizeof(record));
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_stretch_rect(dev_,
            rawSurf(src), srcRect ? &cs : nullptr,
            rawSurf(dst), dstRect ? &cd : nullptr,
            (uint32_t)filter));
    }

    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurf,
                                         const RECT* pRect,
                                         D3DCOLOR color) noexcept override {
        dxmt9DeviceDebugLog("device_color_fill device=%p surf=%p rect=%s color=0x%08x",
                            this, pSurf, pRect ? "<custom>" : "<full>", (unsigned)color);
        D9CRect cr{}; if (pRect) cr = toR(*pRect);
        if (dxmt9PeDrawChunkEnabled()) {
            // Phase 28: chunk-mode barrier — flush pending hot state
            // + consts as records into the chunk, never as bridge calls.
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
            if (auto* raw = rawSurf(pSurf); raw)
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                reinterpret_cast<uint64_t>(raw));
            D9CCommandRecordColorFill record{};
            record.header.type = D9C_COMMAND_RECORD_COLOR_FILL;
            record.header.size = sizeof(record);
            record.surfaceWire = reinterpret_cast<uint64_t>(rawSurf(pSurf));
            record.colorARGB = (uint32_t)color;
            record.hasRect = pRect ? 1u : 0u;
            if (pRect) record.rect = cr;
            return appendCommandRecord(&record, sizeof(record));
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_color_fill(dev_, rawSurf(pSurf),
                    pRect ? &cr : nullptr, (uint32_t)color));
    }

    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DPOOL pool,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE* psh) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForSurface(extended_, psh, pool, true);
        if (FAILED(sharedHr)) return sharedHr;
        dxmt9DeviceDebugLog("device_create_offscreen_surface device=%p size=%ux%u fmt=%u pool=%u",
                            this, w, h, (unsigned)fmt, (unsigned)pool);
        uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
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
                                               IDirect3DSurface9* pSurf) noexcept override {
        dxmt9DeviceDebugLog("device_set_render_target device=%p idx=%u surf=%p",
                            this, (unsigned)idx, pSurf);
        if (idx >= 4) return D3DERR_INVALIDCALL;
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled()) {
            if (rtSlots_[idx] == pSurf) return S_OK;     // shadow no-op
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            setRef(rtSlots_[idx], pSurf);
            pendingRtMask_ |= 1u << idx;
            if (auto* raw = rawSurf(pSurf); raw != nullptr) {
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                reinterpret_cast<uint64_t>(raw));
            }
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        if (auto* raw = rawSurf(pSurf); raw != nullptr) {
            noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                            reinterpret_cast<uint64_t>(raw));
        }
        return hr32(dxmt9c_device_set_render_target(dev_, idx, rawSurf(pSurf)));
    }

    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx,
                                               IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u",
                            this, (unsigned)idx);
        D9CSurface* s = dxmt9c_device_get_render_target(dev_, idx);
        *ppS = s ? new D3D9SurfaceImpl(s, this, nullptr, this) : nullptr;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> surface=%p",
                            this, (unsigned)idx, ppS ? static_cast<void*>(*ppS) : nullptr);
        return s ? S_OK : D3DERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pSurf) noexcept override {
        dxmt9DeviceDebugLog("device_set_depth_stencil device=%p surf=%p", this, pSurf);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled()) {
            if (dsSurface_ == pSurf) return S_OK;     // shadow no-op
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            setRef(dsSurface_, pSurf);
            pendingDs_ = true;
            if (auto* raw = rawSurf(pSurf); raw != nullptr) {
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                reinterpret_cast<uint64_t>(raw));
            }
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        if (auto* raw = rawSurf(pSurf); raw != nullptr) {
            noteChunkHandle(D9C_CHUNK_HANDLE_KIND_SURFACE,
                            reinterpret_cast<uint64_t>(raw));
        }
        return hr32(dxmt9c_device_set_depth_stencil(dev_, rawSurf(pSurf)));
    }

    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        D9CSurface* s = dxmt9c_device_get_depth_stencil(dev_);
        *ppS = s ? new D3D9SurfaceImpl(s, this, nullptr, this) : nullptr;
        dxmt9DeviceDebugLog("device_get_depth_stencil_surface device=%p -> surface=%p",
                            this, ppS ? static_cast<void*>(*ppS) : nullptr);
        return s ? S_OK : S_FALSE;
    }

    /* ── scene ── */
    HRESULT STDMETHODCALLTYPE BeginScene() noexcept override {
        dxmt9DeviceDebugLog("device_begin_scene device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_scene(dev_));
        dxmt9DeviceDebugLog("device_begin_scene -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndScene()   noexcept override {
        dxmt9DeviceDebugLog("device_end_scene device=%p", this);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_device_end_scene(dev_));
        dxmt9DeviceDebugLog("device_end_scene -> hr=0x%08x", (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* pRects,
                                     DWORD flags, D3DCOLOR color,
                                     float z, DWORD stencil) noexcept override {
        dxmt9DeviceDebugLog("device_clear device=%p count=%u flags=0x%x color=0x%08x z=%f stencil=%u",
                            this, (unsigned)count, (unsigned)flags, (unsigned)color, z,
                            (unsigned)stencil);
        // Per recorder design: Clear is a standalone ordering record
        // inside the chunk — drains pending hot state + const dirty
        // ranges first so the chunk replays in API order, then
        // appends a CLEAR record carrying flags + color + z + stencil
        // + the optional rect array as a tail payload.
        if (dxmt9PeDrawChunkEnabled()) {
            // Phase 28: chunk-mode barrier — flush pending hot state
            // + consts as records into the chunk, never as bridge calls.
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;

            const std::uint32_t rectBytes = static_cast<std::uint32_t>(count) * sizeof(D9CRect);
            D9CCommandRecordClear header{};
            header.header.type = D9C_COMMAND_RECORD_CLEAR;
            header.header.size = static_cast<std::uint32_t>(sizeof(header) + rectBytes);
            header.flags = (uint32_t)flags;
            header.colorARGB = (uint32_t)color;
            header.z = z;
            header.stencil = (uint32_t)stencil;
            header.rectCount = (uint32_t)count;
            header.rectOffset = sizeof(header);

            std::vector<std::uint8_t> record(header.header.size);
            std::memcpy(record.data(), &header, sizeof(header));
            if (rectBytes != 0 && pRects) {
                std::memcpy(record.data() + header.rectOffset, pRects, rectBytes);
            }
            return appendCommandRecord(record.data(), record.size());
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_clear(dev_, count,
            reinterpret_cast<const D9CRect*>(pRects),
            flags, (uint32_t)color, z, stencil));
    }

    /* ── transforms ── */
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog(
            "device_set_transform device=%p state=%u "
            "m=[[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g]]",
            this, (unsigned)state,
            pM->m[0][0], pM->m[0][1], pM->m[0][2], pM->m[0][3],
            pM->m[1][0], pM->m[1][1], pM->m[1][2], pM->m[1][3],
            pM->m[2][0], pM->m[2][1], pM->m[2][2], pM->m[2][3],
            pM->m[3][0], pM->m[3][1], pM->m[3][2], pM->m[3][3]);
        // Phase 12: PE-shadow-only when chunk recorder is active. Pending
        // transforms ride on the next draw packet's transforms[] array;
        // server-side applyDrawPacketState dispatches set_transform per
        // entry before the draw runs.
        const D9CMatrix& wireM = *reinterpret_cast<const D9CMatrix*>(pM);
        if (dxmt9PeDrawChunkEnabled()) {
            const auto shadowIt = transformShadow_.find((uint32_t)state);
            const bool shadowMatches = shadowIt != transformShadow_.end() &&
                std::memcmp(&shadowIt->second, &wireM, sizeof(D9CMatrix)) == 0;
            const bool alreadyPending =
                pendingTransforms_.find((uint32_t)state) != pendingTransforms_.end();
            if (!alreadyPending && shadowMatches) {
                return S_OK;            // identity no-op
            }
            // Phase 34: cap-check uses chunkBarrierFlush, not bare
            // flushPendingCommandChunk. The latter only seals existing
            // records — pending hot state would remain DIRTY across
            // the seal, leaving the next Draw* / barrier observing
            // stale server state. chunkBarrierFlush encodes pending
            // state as APPLY_STATE record(s) + clears the pending
            // maps, so the new entry below starts with a fresh delta
            // budget AND the server has already received the prior
            // delta when the next chunk-record runs.
            if (!alreadyPending &&
                pendingTransforms_.size() >= D9C_DRAW_PACKET_MAX_TRANSFORMS) {
                const HRESULT barrierHr = chunkBarrierFlush();
                if (FAILED(barrierHr)) return barrierHr;
            }
            pendingTransforms_[(uint32_t)state] = wireM;
            transformShadow_[(uint32_t)state] = wireM;
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_device_set_transform(dev_, (uint32_t)state,
                    reinterpret_cast<const D9CMatrix*>(pM)));
        dxmt9DeviceDebugLog("device_set_transform -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state,
                                            D3DMATRIX* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_transform device=%p state=%u", this, (unsigned)state);
        return hr32(dxmt9c_device_get_transform(dev_, (uint32_t)state,
                    reinterpret_cast<D9CMatrix*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX* pM) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pVP) noexcept override {
        if (!pVP) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_viewport device=%p x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                            this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
        D9CViewport vp{ pVP->X, pVP->Y, pVP->Width, pVP->Height,
                        pVP->MinZ, pVP->MaxZ };
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // packet built for the next draw carries viewportValid=1 + the
        // shadow snapshot; server-side applyDrawPacketState dispatches
        // dxmt9c_device_set_viewport before the draw runs.
        if (dxmt9PeDrawChunkEnabled()) {
            if (std::memcmp(&viewportShadow_, &vp, sizeof(vp)) == 0) {
                return S_OK;            // identity no-op
            }
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            viewportShadow_ = vp;
            pendingViewport_ = true;
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_viewport(dev_, &vp));
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pVP) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pR) noexcept override {
        if (!pR) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_scissor_rect device=%p rect=%ld,%ld-%ld,%ld",
                            this, (long)pR->left, (long)pR->top, (long)pR->right, (long)pR->bottom);
        D9CRect cr = toR(*pR);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled()) {
            if (std::memcmp(&scissorShadow_, &cr, sizeof(cr)) == 0) {
                return S_OK;            // identity no-op
            }
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            scissorShadow_ = cr;
            pendingScissor_ = true;
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_scissor_rect(dev_, &cr));
    }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pR) noexcept override {
        if (!pR) return D3DERR_INVALIDCALL;
        D9CRect cr{};
        dxmt9c_device_get_scissor_rect(dev_, &cr);
        pR->left = cr.left; pR->top = cr.top;
        pR->right = cr.right; pR->bottom = cr.bottom;
        return S_OK;
    }

    /* ── material / lights ── */
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_material device=%p", this);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled()) {
            if (std::memcmp(&materialShadow_, pM, sizeof(D9CMaterial)) == 0) {
                return S_OK;            // identity no-op
            }
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            std::memcpy(&materialShadow_, pM, sizeof(D9CMaterial));
            pendingMaterial_ = true;
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_material(dev_,
                    reinterpret_cast<const D9CMaterial*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_material device=%p", this);
        return hr32(dxmt9c_device_get_material(dev_,
                    reinterpret_cast<D9CMaterial*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD idx, const D3DLIGHT9* pL) noexcept override {
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
        // Phase 12: PE-shadow-only when chunk recorder is active. Up to
        // D9C_DRAW_PACKET_MAX_LIGHTS (8) light slots ride on a single
        // packet via lightSlotMask + lights[8]. Out-of-range idx falls
        // back to legacy unix-call (rare, and the backend may also
        // refuse).
        if (dxmt9PeDrawChunkEnabled() && idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
            if ((pendingLightSlotMask_ & (1u << idx)) == 0 &&
                std::memcmp(&lightShadow_[idx], &cl, sizeof(D9CLight)) == 0) {
                return S_OK;            // identity no-op
            }
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            lightShadow_[idx] = cl;
            pendingLightSlotMask_ |= 1u << idx;
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_light(dev_, idx, &cl));
    }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD, D3DLIGHT9* pL) noexcept override {
        dxmt9DeviceDebugLog("device_get_light device=%p", this);
        if (pL) memset(pL, 0, sizeof(*pL)); return S_OK; /* stub */
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) noexcept override {
        dxmt9DeviceDebugLog("device_light_enable device=%p idx=%u enable=%u", this, (unsigned)idx, (unsigned)en);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled() && idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
            const DWORD bit = 1u << idx;
            const bool wantEnabled = en != 0;
            const bool shadowEnabled = (lightEnableShadow_ & bit) != 0;
            if ((pendingLightEnableValidMask_ & bit) == 0 &&
                wantEnabled == shadowEnabled) {
                return S_OK;            // identity no-op
            }
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            pendingLightEnableValidMask_ |= bit;
            if (wantEnabled) {
                pendingLightEnableMask_ |= bit;
                lightEnableShadow_ |= bit;
            } else {
                pendingLightEnableMask_ &= ~bit;
                lightEnableShadow_ &= ~bit;
            }
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_light_enable(dev_, idx, en ? 1u : 0u));
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD, BOOL* pEn) noexcept override {
        dxmt9DeviceDebugLog("device_get_light_enable device=%p", this);
        if (pEn) *pEn = FALSE; return S_OK; /* stub */
    }

    /* ── clip planes ── */
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* pPlane) noexcept override {
        dxmt9DeviceDebugLog("device_set_clip_plane device=%p idx=%u plane=%p", this, (unsigned)idx, pPlane);
        if (!pPlane) return D3DERR_INVALIDCALL;
        if (idx >= 6) return D3DERR_INVALIDCALL;
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled()) {
            const std::size_t off = static_cast<std::size_t>(idx) * 4u;
            if ((pendingClipPlaneMask_ & (1u << idx)) == 0 &&
                std::memcmp(&clipPlaneShadow_[off], pPlane, sizeof(float) * 4) == 0) {
                return S_OK;            // identity no-op
            }
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            std::memcpy(&clipPlaneShadow_[off], pPlane, sizeof(float) * 4);
            pendingClipPlaneMask_ |= 1u << idx;
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_clip_plane(dev_, idx, pPlane));
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* pPlane) noexcept override {
        dxmt9DeviceDebugLog("device_get_clip_plane device=%p idx=%u", this, (unsigned)idx);
        return hr32(dxmt9c_device_get_clip_plane(dev_, idx, pPlane));
    }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9*) noexcept override {
        dxmt9DeviceDebugLog("device_set_clip_status device=%p", this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* p) noexcept override {
        dxmt9DeviceDebugLog("device_get_clip_status device=%p", this);
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }

    /* ── render states ── */
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) noexcept override {
        dxmt9DeviceDebugLog("device_set_render_state device=%p state=%u value=0x%x",
                            this, (unsigned)state, (unsigned)value);
        if (stateBlockRecording_) {
            const DWORD stateKey = static_cast<DWORD>(state);
            if (!stateBlockRenderStateRestore_.contains(stateKey)) {
                DWORD previous = dxmt9c_device_get_render_state(dev_, stateKey);
                uint32_t shadowValue = 0;
                if (dxmt9PeStateShadowEnabled() &&
                    renderStateShadow_.get(stateKey, shadowValue)) {
                    previous = shadowValue;
                }
                stateBlockRenderStateRestore_.set(stateKey, previous);
            }
            return hr32(dxmt9c_device_set_render_state(dev_, (uint32_t)state, value));
        }
        const DWORD stateKey = static_cast<DWORD>(state);
        if (dxmt9PeStateShadowEnabled() && shadowedRenderStateEquals(stateKey, value)) {
            return S_OK;
        }
        const HRESULT chunkHr = flushPendingCommandChunk();
        if (FAILED(chunkHr)) return chunkHr;
        if (dxmt9PeStateShadowEnabled()) {
            // Phase 31: cap check — if a NEW state would push the
            // pending table past the per-packet cap, drain pending state
            // into the chunk via chunkBarrierFlush() so the next packet
            // starts with a fresh delta budget. Mirrors the TSS /
            // SamplerState / Transform fast-path patterns and prevents
            // the over-cap edge from leaking into the Draw fallback
            // path (which would bridge-emit Set* calls).
            if (!pendingRenderStates_.contains(stateKey) &&
                pendingRenderStates_.size() >= D9C_DRAW_PACKET_MAX_RENDER_STATES) {
                const HRESULT barrierHr = chunkBarrierFlush();
                if (FAILED(barrierHr)) return barrierHr;
            }
            renderStateShadow_.set(stateKey, value);
            pendingRenderStates_.set(stateKey, value);
            return S_OK;
        }
        return hr32(dxmt9c_device_set_render_state(dev_, (uint32_t)state, value));
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD* pValue) noexcept override {
        if (!pValue) return D3DERR_INVALIDCALL;
        if (dxmt9PeStateShadowEnabled()) {
            uint32_t shadowValue = 0;
            if (renderStateShadow_.get(static_cast<DWORD>(state), shadowValue)) {
                *pValue = shadowValue;
                return S_OK;
            }
        }
        *pValue = dxmt9c_device_get_render_state(dev_, (uint32_t)state);
        return S_OK;
    }

    /* ── state blocks ── */
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type,
                                                IDirect3DStateBlock9** ppSB) noexcept override {
        if (!ppSB) return D3DERR_INVALIDCALL;
        if (!isValidD3DStateBlockType(type) || stateBlockRecording_) {
            return D3DERR_INVALIDCALL;
        }
        // Phase 28: state-block creation needs current server state.
        // flushPeRecorder() routes through chunkBarrierFlush + chunk
        // commit in chunk mode, bridge-emit in legacy mode.
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        dxmt9DeviceDebugLog("device_create_state_block device=%p type=%u", this, (unsigned)type);
        D9CStateBlock* sb = dxmt9c_device_create_state_block(dev_, (uint32_t)type);
        if (!sb) return D3DERR_INVALIDCALL;
        *ppSB = new D3D9StateBlockImpl(sb, this, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() noexcept override {
        if (stateBlockRecording_) {
            return D3DERR_INVALIDCALL;
        }
        const HRESULT flushHr = flushPeRecorder();   // Phase 28: mode-aware
        if (FAILED(flushHr)) return flushHr;
        dxmt9DeviceDebugLog("device_begin_state_block device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_state_block(dev_));
        if (SUCCEEDED(hr)) {
            stateBlockRecording_ = true;
            stateBlockRenderStateRestore_.clear();
        }
        dxmt9DeviceDebugLog("device_begin_state_block -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) noexcept override {
        if (!ppSB) return D3DERR_INVALIDCALL;
        if (!stateBlockRecording_) {
            return D3DERR_INVALIDCALL;
        }
        const HRESULT flushHr = flushPeRecorder();   // Phase 28: mode-aware
        if (FAILED(flushHr)) return flushHr;
        dxmt9DeviceDebugLog("device_end_state_block device=%p", this);
        D9CStateBlock* sb = nullptr;
        HRESULT hr = hr32(dxmt9c_device_end_state_block(dev_, &sb));
        if (SUCCEEDED(hr)) {
            stateBlockRecording_ = false;
            stateBlockRenderStateRestore_.forEach([&](uint32_t state, uint32_t value) {
                (void)dxmt9c_device_set_render_state(dev_, state, value);
                renderStateShadow_.set(state, value);
                pendingRenderStates_.erase(state);
            });
            stateBlockRenderStateRestore_.clear();
            if (sb) {
                *ppSB = new D3D9StateBlockImpl(sb, this, this);
            }
        }
        dxmt9DeviceDebugLog("device_end_state_block -> hr=0x%08x sb=%p out=%p",
                            (unsigned)hr, static_cast<void*>(sb), *ppSB);
        return hr;
    }

    /* ── texture stage / sampler states ── */
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD value) noexcept override {
        dxmt9DeviceDebugLog("device_set_texture_stage_state device=%p stage=%u type=%u value=0x%x",
                            this, (unsigned)stage, (unsigned)type, (unsigned)value);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled()) {
            const uint32_t stageSlot = textureStageSlot(stage);
            const uint32_t stateSlot = textureStageStateSlot(type);
            uint32_t shadowValue = 0;
            if (tssShadow_.get(stageSlot, stateSlot, shadowValue) &&
                shadowValue == value) {
                return S_OK;            // identity no-op
            }
            // Phase 34: cap-check uses chunkBarrierFlush so pending
            // state is encoded as APPLY_STATE record(s) + cleared
            // before the new entry. Bare flushPendingCommandChunk
            // would leave the pending TSS table dirty across the seal.
            if (!pendingTss_.contains(stageSlot, stateSlot) &&
                pendingTss_.size() >= D9C_DRAW_PACKET_MAX_TSS) {
                const HRESULT barrierHr = chunkBarrierFlush();
                if (FAILED(barrierHr)) return barrierHr;
            }
            tssShadow_.set(stageSlot, stateSlot, value);
            pendingTss_.set(stageSlot, stateSlot, value);
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_texture_stage_state(dev_, stage,
                    (uint32_t)type, value));
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD* pValue) noexcept override {
        if (!pValue) return D3DERR_INVALIDCALL;
        *pValue = dxmt9c_device_get_texture_stage_state(dev_, stage, (uint32_t)type);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD value) noexcept override {
        dxmt9DeviceDebugLog("device_set_sampler_state device=%p sampler=%u type=%u value=0x%x",
                            this, (unsigned)sampler, (unsigned)type, (unsigned)value);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled()) {
            uint32_t samplerIndex = 0;
            if (!samplerSlot(sampler, samplerIndex)) {
                return D3DERR_INVALIDCALL;
            }
            uint32_t stateSlot = 0;
            if (!samplerStateSlot(type, stateSlot)) {
                return S_OK;
            }
            uint32_t shadowValue = 0;
            if (samplerStateShadow_.get(samplerIndex, stateSlot, shadowValue) &&
                shadowValue == value) {
                return S_OK;            // identity no-op
            }
            // Phase 34: cap-check uses chunkBarrierFlush — see Phase
            // 31a / SetTextureStageState for the rationale.
            if (!pendingSamplerStates_.contains(samplerIndex, stateSlot) &&
                pendingSamplerStates_.size() >= D9C_DRAW_PACKET_MAX_SAMPLER) {
                const HRESULT barrierHr = chunkBarrierFlush();
                if (FAILED(barrierHr)) return barrierHr;
            }
            samplerStateShadow_.set(samplerIndex, stateSlot, value);
            pendingSamplerStates_.set(samplerIndex, stateSlot, value);
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_sampler_state(dev_, sampler,
                    (uint32_t)type, value));
    }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD* pValue) noexcept override {
        if (!pValue) return D3DERR_INVALIDCALL;
        *pValue = dxmt9c_device_get_sampler_state(dev_, sampler, (uint32_t)type);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pPasses) noexcept override {
        dxmt9DeviceDebugLog("device_validate_device device=%p", this);
        if (pPasses) *pPasses = 1; return S_OK;
    }

    /* ── palette (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY*) noexcept override {
        dxmt9DeviceDebugLog("device_set_palette_entries device=%p palette=%u", this, palette);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY*) noexcept override {
        dxmt9DeviceDebugLog("device_get_palette_entries device=%p palette=%u", this, palette);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) noexcept override {
        dxmt9DeviceDebugLog("device_set_current_texture_palette device=%p palette=%u", this, palette);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) noexcept override {
        dxmt9DeviceDebugLog("device_get_current_texture_palette device=%p", this);
        if (p) *p = 0; return S_OK;
    }

    /* ── soft VP / NPatches (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL enable) noexcept override {
        dxmt9DeviceDebugLog("device_set_software_vertex_processing device=%p enable=%u", this, (unsigned)enable);
        return S_OK;
    }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() noexcept override {
        dxmt9DeviceDebugLog("device_get_software_vertex_processing device=%p", this);
        return FALSE;
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) noexcept override {
        dxmt9DeviceDebugLog("device_set_npatch_mode device=%p segments=%f", this, segments);
        return S_OK;
    }
    float   STDMETHODCALLTYPE GetNPatchMode() noexcept override { return 0.0f; }

    /* ── textures ── */
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage,
                                          IDirect3DBaseTexture9* pTex) noexcept override {
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
                                          IDirect3DBaseTexture9** ppTex) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        IDirect3DBaseTexture9* t = textures_[stage < 16 ? stage : 0];
        if (t) t->AddRef();
        *ppTex = t;
        dxmt9DeviceDebugLog("device_get_texture device=%p stage=%u -> tex=%p",
                            this, (unsigned)stage, static_cast<void*>(t));
        return S_OK;
    }

    /* ── FVF / vertex declaration ── */
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) noexcept override {
        if (!pFVF) return D3DERR_INVALIDCALL;
        if (dxmt9PeStateShadowEnabled()) {
            *pFVF = fvf_;
            return S_OK;
        }
        *pFVF = dxmt9c_device_get_fvf(dev_); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
            const D3DVERTEXELEMENT9* pElems,
            IDirect3DVertexDeclaration9** ppVD) noexcept override {
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
            IDirect3DVertexDeclaration9* pVD) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_declaration device=%p decl=%p", this, pVD);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled()) {
            if (vdecl_ == pVD) return S_OK;     // shadow no-op
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            setRef(vdecl_, pVD);
            pendingVdecl_ = true;
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        setRef(vdecl_, pVD);
        return hr32(dxmt9c_device_set_vertex_declaration(dev_, rawVD(pVD)));
    }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(
            IDirect3DVertexDeclaration9** ppVD) noexcept override {
        if (!ppVD) return D3DERR_INVALIDCALL;
        if (vdecl_) vdecl_->AddRef();
        *ppVD = vdecl_; return S_OK;
    }

    /* ── vertex shaders ── */
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFn,
                                                  IDirect3DVertexShader9** ppVS) noexcept override {
        if (!pFn || !ppVS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_vertex_shader device=%p code=%p", this, pFn);
        D9CShader* s = dxmt9c_device_create_vertex_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
        if (!s) return D3DERR_INVALIDCALL;
        *ppVS = new D3D9VertexShaderImpl(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_shader device=%p shader=%p", this, pVS);
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // packet built for the next draw carries vsValid=1 + the vs_
        // wire handle; server-side applyDrawPacketState dispatches the
        // dxmt9c_device_set_vertex_shader call before the draw runs.
        if (dxmt9PeDrawChunkEnabled()) {
            if (vs_ == pVS) return S_OK;     // shadow no-op
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            setRef(vs_, pVS);
            pendingVs_ = true;
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        setRef(vs_, pVS);
        return hr32(dxmt9c_device_set_vertex_shader(dev_, rawVS(pVS)));
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppVS) noexcept override {
        if (!ppVS) return D3DERR_INVALIDCALL;
        if (vs_) vs_->AddRef(); *ppVS = vs_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* pData,
                                                        UINT count) noexcept override {
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
                                                        UINT count) noexcept override {
        return hr32(dxmt9c_device_get_vs_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) noexcept override {
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
                                                        UINT count) noexcept override {
        (void)start; (void)pData; (void)count; return S_OK; /* stub */
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) noexcept override {
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
                                                        UINT count) noexcept override {
        (void)start; (void)pData; (void)count; return S_OK; /* stub */
    }

    /* ── stream sources ── */
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9* pBuf,
                                               UINT offset, UINT stride) noexcept override {
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
                                               UINT* pOffset, UINT* pStride) noexcept override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        IDirect3DVertexBuffer9* b = streamSrc_[stream < 16 ? stream : 0];
        if (b) b->AddRef();
        *ppBuf = b;
        if (pOffset) *pOffset = streamOff_[stream < 16 ? stream : 0];
        if (pStride) *pStride = streamStr_[stream < 16 ? stream : 0];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT freq) noexcept override {
        dxmt9DeviceDebugLog("device_set_stream_source_freq device=%p stream=%u freq=0x%x",
                            this, stream, (unsigned)freq);
        const HRESULT flushHr = flushPeRecorder();   // Phase 28: mode-aware
        if (FAILED(flushHr)) return flushHr;
        streamFreq_[stream < 16 ? stream : 0] = freq;
        return hr32(dxmt9c_device_set_stream_source_freq(dev_, stream, freq));
    }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* pFreq) noexcept override {
        const UINT freq = streamFreq_[stream < 16 ? stream : 0];
        if (pFreq) *pFreq = freq;
        dxmt9DeviceDebugLog("device_get_stream_source_freq device=%p stream=%u -> freq=0x%x",
                            this, stream, (unsigned)freq);
        return S_OK;
    }

    /* ── indices ── */
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIBuf) noexcept override {
        dxmt9DeviceDebugLog("device_set_indices device=%p ib=%p", this, pIBuf);
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // index buffer rides on D9CDrawIndexedPrimitivePacket via
        // ibValid + ibHandle (only consumed by indexed draws).
        if (dxmt9PeDrawChunkEnabled()) {
            if (indexBuf_ == pIBuf) return S_OK;     // shadow no-op
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            setRef(indexBuf_, pIBuf);
            pendingIb_ = true;
            if (auto* raw = rawIBuf(pIBuf); raw != nullptr) {
                noteChunkHandle(D9C_CHUNK_HANDLE_KIND_BUFFER,
                                reinterpret_cast<uint64_t>(raw));
            }
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        setRef(indexBuf_, pIBuf);
        if (auto* raw = rawIBuf(pIBuf); raw != nullptr) {
            noteChunkHandle(D9C_CHUNK_HANDLE_KIND_BUFFER,
                            reinterpret_cast<uint64_t>(raw));
        }
        return hr32(dxmt9c_device_set_indices(dev_, rawIBuf(pIBuf)));
    }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIBuf) noexcept override {
        if (!ppIBuf) return D3DERR_INVALIDCALL;
        if (indexBuf_) indexBuf_->AddRef(); *ppIBuf = indexBuf_; return S_OK;
    }

    /* ── pixel shaders ── */
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFn,
                                                 IDirect3DPixelShader9** ppPS) noexcept override {
        if (!pFn || !ppPS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_pixel_shader device=%p code=%p", this, pFn);
        D9CShader* s = dxmt9c_device_create_pixel_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
        if (!s) return D3DERR_INVALIDCALL;
        *ppPS = new D3D9PixelShaderImpl(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pPS) noexcept override {
        dxmt9DeviceDebugLog("device_set_pixel_shader device=%p shader=%p", this, pPS);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (dxmt9PeDrawChunkEnabled()) {
            if (ps_ == pPS) return S_OK;     // shadow no-op
            const HRESULT chunkHr = flushPendingCommandChunk();
            if (FAILED(chunkHr)) return chunkHr;
            setRef(ps_, pPS);
            pendingPs_ = true;
            return S_OK;
        }
        const HRESULT flushHr = flushPendingHotState();
        if (FAILED(flushHr)) return flushHr;
        setRef(ps_, pPS);
        return hr32(dxmt9c_device_set_pixel_shader(dev_, rawPS(pPS)));
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) noexcept override {
        if (!ppPS) return D3DERR_INVALIDCALL;
        if (ps_) ps_->AddRef(); *ppPS = ps_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) noexcept override {
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
                                                       UINT count) noexcept override {
        return hr32(dxmt9c_device_get_ps_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) noexcept override {
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
                                                       UINT count) noexcept override {
        (void)start; (void)pData; (void)count; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) noexcept override {
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
                                                       UINT count) noexcept override {
        (void)start; (void)pData; (void)count; return S_OK;
    }

    /* ── draw calls ── */
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type,
                                             UINT startVertex,
                                             UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_draw_primitive device=%p type=%u startVertex=%u count=%u",
                            this, (unsigned)type, startVertex, count);
        // Phase 32: chunk mode never falls through to per-call bridge.
        // If pending state somehow exceeds packet cap (shouldn't happen
        // post-Phase-31 cap-checks at every Set* fast path), drain via
        // chunkBarrierFlush — which splits oversized collections into
        // multiple APPLY_STATE records — then append the draw record.
        if (dxmt9PeDrawChunkEnabled()) {
            if (pendingRenderStates_.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
                const HRESULT barrierHr = chunkBarrierFlush();
                if (FAILED(barrierHr)) return barrierHr;
            }
            const HRESULT hr = appendDrawPrimitiveRecord(type, startVertex, count);
            if (SUCCEEDED(hr)) {
                clearPendingHotState();
            }
            return hr;
        }
        // Legacy fallback (DXMT9_PE_DRAW_CHUNK=0): packet-bridge first,
        // then per-call bridge.
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
                                                    UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_draw_indexed_primitive device=%p type=%u base=%d min=%u num=%u startIndex=%u count=%u",
                            this, (unsigned)type, baseVertex, minVertex, numVertices,
                            startIndex, count);
        // Phase 32: chunk mode never falls through to per-call bridge.
        if (dxmt9PeDrawChunkEnabled()) {
            if (pendingRenderStates_.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
                const HRESULT barrierHr = chunkBarrierFlush();
                if (FAILED(barrierHr)) return barrierHr;
            }
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
                                               UINT stride) noexcept override {
        dxmt9DeviceDebugLog("device_draw_primitive_up device=%p type=%u count=%u data=%p stride=%u",
                            this, (unsigned)type, count, pData, stride);
        // Phase 32: chunk mode never falls through to per-call bridge.
        if (dxmt9PeDrawChunkEnabled()) {
            if (pendingRenderStates_.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
                const HRESULT barrierHr = chunkBarrierFlush();
                if (FAILED(barrierHr)) return barrierHr;
            }
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
                                                      UINT stride) noexcept override {
        dxmt9DeviceDebugLog("device_draw_indexed_primitive_up device=%p type=%u min=%u num=%u count=%u idx=%p idxFmt=%u vtx=%p stride=%u",
                            this, (unsigned)type, minVertex, numVertices, count,
                            pIdxData, (unsigned)idxFmt, pVtxData, stride);
        // Phase 32: chunk mode never falls through to per-call bridge.
        if (dxmt9PeDrawChunkEnabled()) {
            if (pendingRenderStates_.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
                const HRESULT barrierHr = chunkBarrierFlush();
                if (FAILED(barrierHr)) return barrierHr;
            }
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
                                               DWORD) noexcept override {
        dxmt9DeviceDebugLog("device_process_vertices device=%p", this);
        return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) noexcept override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) noexcept override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) noexcept override { return S_OK; }

    /* ── query ── */
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type,
                                           IDirect3DQuery9** ppQ) noexcept override {
        if (!ppQ) { /* just checking support */ return S_OK; }
        D9CQuery* q = dxmt9c_device_create_query(dev_, (uint32_t)type);
        if (!q) return D3DERR_NOTAVAILABLE;
        *ppQ = new D3D9QueryImpl(q, this, this);
        return S_OK;
    }

    /* ── IDirect3DDevice9Ex ── */

    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT,UINT,float*,float*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9*,IDirect3DSurface9*,
                                            IDirect3DVertexBuffer9*,UINT,
                                            IDirect3DVertexBuffer9*,
                                            D3DCOMPOSERECTSOP,int,int) noexcept override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE PresentEx(const RECT* src, const RECT* dst,
                                         HWND wnd, const RGNDATA* dirty,
                                         DWORD flags) noexcept override {
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_present(dev_,
            src ? &cs : nullptr, dst ? &cd : nullptr,
            (uint64_t)(uintptr_t)wnd, dirty, flags));
    }

    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* p) noexcept override { if (p) *p = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT sc) noexcept override {
        return hr32(dxmt9c_device_wait_for_vblank(dev_, sc));
    }

    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9**,
                                                      UINT32) noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT maxLatency) noexcept override {
        return hr32(dxmt9c_device_set_maximum_frame_latency(dev_, maxLatency));
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* p) noexcept override {
        if (!p) return D3DERR_INVALIDCALL;
        *p = dxmt9c_device_get_maximum_frame_latency(dev_); return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND wnd) noexcept override {
        return hr32(dxmt9c_device_check_device_state(dev_,
                    (uint64_t)(uintptr_t)wnd));
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT w, UINT h,
                                                    D3DFORMAT fmt,
                                                    D3DMULTISAMPLE_TYPE ms,
                                                    DWORD msQual, BOOL lockable,
                                                    IDirect3DSurface9** ppS,
                                                    HANDLE* psh,
                                                    DWORD usage) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        return CreateRenderTarget(w, h, fmt, ms, msQual, lockable, ppS, psh);
    }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT w, UINT h,
                                                             D3DFORMAT fmt,
                                                             D3DPOOL pool,
                                                             IDirect3DSurface9** ppS,
                                                             HANDLE* psh,
                                                             DWORD usage) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        return CreateOffscreenPlainSurface(w, h, fmt, pool, ppS, psh);
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DMULTISAMPLE_TYPE ms,
                                                           DWORD msQual,
                                                           BOOL discard,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE* psh,
                                                           DWORD usage) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        return CreateDepthStencilSurface(w, h, fmt, ms, msQual, discard, ppS, psh);
    }

    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS* pPP,
                                       D3DDISPLAYMODEEX* pFsMode) noexcept override {
        if (!pPP) return D3DERR_INVALIDCALL;
        if (pFsMode && pFsMode->Size != sizeof(D3DDISPLAYMODEEX)) return D3DERR_INVALIDCALL;
        if (pPP->Windowed ? pFsMode != nullptr : pFsMode == nullptr) return D3DERR_INVALIDCALL;
        if (pFsMode && (pFsMode->Width != pPP->BackBufferWidth
                || pFsMode->Height != pPP->BackBufferHeight)) {
            return D3DERR_INVALIDCALL;
        }
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
        stateBlockRecording_ = false;
        stateBlockRenderStateRestore_.clear();
        return hr32(dxmt9c_device_reset_ex(dev_, &cpp, pFsMode ? &cdme : nullptr));
    }

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT sc,
                                                D3DDISPLAYMODEEX* pMode,
                                                D3DDISPLAYROTATION* pRot) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        if (pMode->Size != sizeof(D3DDISPLAYMODEEX)) return D3DERR_INVALIDCALL;
        D3DDISPLAYMODE mode{};
        const HRESULT hr = GetDisplayMode(sc, &mode);
        if (FAILED(hr)) return hr;
        pMode->Size = sizeof(D3DDISPLAYMODEEX);
        pMode->Width = mode.Width;
        pMode->Height = mode.Height;
        pMode->RefreshRate = mode.RefreshRate;
        pMode->Format = mode.Format;
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
        if (pRot)  *pRot = D3DDISPLAYROTATION_IDENTITY;
        return S_OK;
    }
};

/* =========================================================================
 * Factory function (called from factory.cpp)
 * ========================================================================= */

IDirect3DDevice9Ex* CreateDeviceImpl(D9CDevice* dev, IDirect3D9Ex* pFactory,
                                     UINT adapter, D3DDEVTYPE deviceType,
                                     DWORD behaviorFlags,
                                     HWND window, bool extended) {
    return new D3D9DeviceImpl(dev, pFactory, adapter, deviceType,
                              behaviorFlags, window, extended);
}
