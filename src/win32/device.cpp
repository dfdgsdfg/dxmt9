/* src/win32/device.cpp — IDirect3DDevice9Ex and all resource COM wrappers.
 * All methods delegate to the dxmt9c_* C API from dxmt9/device_c.h. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstring>
#include "dxmt9/device_c.h"

/* declared in factory.cpp */
void FillD3DCaps9(const D9CCaps& src, D3DCAPS9* out);

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

static D9CRect toR(const RECT& r) {
    D9CRect c; c.left = r.left; c.top = r.top;
    c.right = r.right; c.bottom = r.bottom;
    return c;
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
    IDirect3DBaseTexture9* owner_; /* borrowed – no extra ref on owner */
public:
    D3D9SurfaceImpl(D9CSurface* s, IDirect3DBaseTexture9* owner)
        : s_(s), owner_(owner) {}
    ~D3D9SurfaceImpl() { dxmt9c_surface_release(s_); }

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
        *pp = nullptr; return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (!owner_) { *ppv = nullptr; return D3DERR_INVALIDCALL; }
        return owner_->QueryInterface(riid, ppv);
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
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* pLR, const RECT* pRect,
                                        DWORD flags) override {
        if (!pLR) return D3DERR_INVALIDCALL;
        D9CLockedRect lr{}; D9CRect cr{};
        if (pRect) cr = toR(*pRect);
        HRESULT hr = hr32(dxmt9c_surface_lock_rect(s_, &lr,
                          pRect ? &cr : nullptr, flags));
        if (SUCCEEDED(hr)) { pLR->Pitch = lr.pitch; pLR->pBits = lr.bits; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect() override {
        return hr32(dxmt9c_surface_unlock_rect(s_));
    }
    HRESULT STDMETHODCALLTYPE GetDC(HDC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override { return E_NOTIMPL; }
};

/* ── Texture2D ────────────────────────────────────────────────────────────── */

class D3D9TextureImpl final : public IDirect3DTexture9 {
    ULONG       refs_ = 1;
    D9CTexture* t_;
public:
    explicit D3D9TextureImpl(D9CTexture* t) : t_(t) {}
    ~D3D9TextureImpl() { dxmt9c_texture_release(t_); }

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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
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
        D9CSurface* s = dxmt9c_texture_get_surface_level(t_, level);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* pLR,
                                        const RECT* pRect, DWORD flags) override {
        if (!pLR) return D3DERR_INVALIDCALL;
        D9CLockedRect lr{}; D9CRect cr{};
        if (pRect) cr = toR(*pRect);
        HRESULT hr = hr32(dxmt9c_texture_lock_rect(t_, level, &lr,
                          pRect ? &cr : nullptr, flags));
        if (SUCCEEDED(hr)) { pLR->Pitch = lr.pitch; pLR->pBits = lr.bits; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override {
        return hr32(dxmt9c_texture_unlock_rect(t_, level));
    }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override { return S_OK; }
};

/* ── CubeTexture ──────────────────────────────────────────────────────────── */

class D3D9CubeTextureImpl final : public IDirect3DCubeTexture9 {
    ULONG       refs_ = 1;
    D9CTexture* t_;
public:
    explicit D3D9CubeTextureImpl(D9CTexture* t) : t_(t) {}
    ~D3D9CubeTextureImpl() { dxmt9c_texture_release(t_); }

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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
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
        UINT idx = (UINT)face * dxmt9c_texture_get_level_count(t_) + level;
        D9CSurface* s = dxmt9c_texture_get_surface_level(t_, idx);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level,
                                        D3DLOCKED_RECT* pLR, const RECT* pRect,
                                        DWORD flags) override {
        if (!pLR) return D3DERR_INVALIDCALL;
        UINT idx = (UINT)face * dxmt9c_texture_get_level_count(t_) + level;
        D9CLockedRect lr{}; D9CRect cr{};
        if (pRect) cr = toR(*pRect);
        HRESULT hr = hr32(dxmt9c_texture_lock_rect(t_, idx, &lr,
                          pRect ? &cr : nullptr, flags));
        if (SUCCEEDED(hr)) { pLR->Pitch = lr.pitch; pLR->pBits = lr.bits; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) override {
        UINT idx = (UINT)face * dxmt9c_texture_get_level_count(t_) + level;
        return hr32(dxmt9c_texture_unlock_rect(t_, idx));
    }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES, const RECT*) override { return S_OK; }
};

/* ── VolumeTexture ────────────────────────────────────────────────────────── */

class D3D9VolumeTextureImpl final : public IDirect3DVolumeTexture9 {
    ULONG       refs_ = 1;
    D9CTexture* t_;
public:
    explicit D3D9VolumeTextureImpl(D9CTexture* t) : t_(t) {}
    ~D3D9VolumeTextureImpl() { dxmt9c_texture_release(t_); }

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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
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
public:
    explicit D3D9VertexBufferImpl(D9CBuffer* b) : b_(b) {}
    ~D3D9VertexBufferImpl() { dxmt9c_buffer_release(b_); }

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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void** pp, DWORD flags) override {
        return hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { return hr32(dxmt9c_buffer_unlock(b_)); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC*) override { return E_NOTIMPL; }
};

/* ── IndexBuffer ──────────────────────────────────────────────────────────── */

class D3D9IndexBufferImpl final : public IDirect3DIndexBuffer9 {
    ULONG      refs_ = 1;
    D9CBuffer* b_;
public:
    explicit D3D9IndexBufferImpl(D9CBuffer* b) : b_(b) {}
    ~D3D9IndexBufferImpl() { dxmt9c_buffer_release(b_); }

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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void  STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void** pp, DWORD flags) override {
        return hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { return hr32(dxmt9c_buffer_unlock(b_)); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC*) override { return E_NOTIMPL; }
};

/* ── VertexShader ─────────────────────────────────────────────────────────── */

class D3D9VertexShaderImpl final : public IDirect3DVertexShader9 {
    ULONG      refs_ = 1;
    D9CShader* s_;
public:
    explicit D3D9VertexShaderImpl(D9CShader* s) : s_(s) {}
    ~D3D9VertexShaderImpl() { dxmt9c_shader_release(s_); }

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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
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
public:
    explicit D3D9PixelShaderImpl(D9CShader* s) : s_(s) {}
    ~D3D9PixelShaderImpl() { dxmt9c_shader_release(s_); }

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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
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
public:
    explicit D3D9VertexDeclImpl(D9CVertexDecl* d) : d_(d) {}
    ~D3D9VertexDeclImpl() { dxmt9c_vdecl_release(d_); }

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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
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
public:
    explicit D3D9QueryImpl(D9CQuery* q) : q_(q) {}
    ~D3D9QueryImpl() { dxmt9c_query_release(q_); }

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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
    }
    D3DQUERYTYPE STDMETHODCALLTYPE GetType()     override { return (D3DQUERYTYPE)dxmt9c_query_get_type(q_); }
    DWORD        STDMETHODCALLTYPE GetDataSize()  override { return dxmt9c_query_get_data_size(q_); }
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override { return hr32(dxmt9c_query_issue(q_, flags)); }
    HRESULT STDMETHODCALLTYPE GetData(void* pData, DWORD size, DWORD flags) override {
        return hr32(dxmt9c_query_get_data(q_, pData, size, flags));
    }
};

/* ── StateBlock ───────────────────────────────────────────────────────────── */

class D3D9StateBlockImpl final : public IDirect3DStateBlock9 {
    ULONG          refs_ = 1;
    D9CStateBlock* sb_;
public:
    explicit D3D9StateBlockImpl(D9CStateBlock* sb) : sb_(sb) {}
    ~D3D9StateBlockImpl() { dxmt9c_stateblock_release(sb_); }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_; if (!r) delete this; return r;
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
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE Capture() override { return hr32(dxmt9c_stateblock_capture(sb_)); }
    HRESULT STDMETHODCALLTYPE Apply()   override { return hr32(dxmt9c_stateblock_apply(sb_)); }
};

/* ── SwapChain ────────────────────────────────────────────────────────────── */

class D3D9SwapChainImpl final : public IDirect3DSwapChain9 {
    ULONG        refs_ = 1;
    D9CSwapChain* sc_;
public:
    explicit D3D9SwapChainImpl(D9CSwapChain* sc) : sc_(sc) {}
    ~D3D9SwapChainImpl() { dxmt9c_swapchain_release(sc_); }

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
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
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
        D9CSurface* s = dxmt9c_swapchain_get_back_buffer(sc_, idx, 0);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, nullptr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* p) override {
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* p) override {
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL; *pp = nullptr; return D3DERR_INVALIDCALL;
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

/* =========================================================================
 * D3D9DeviceImpl — IDirect3DDevice9Ex
 * ========================================================================= */

class D3D9DeviceImpl final : public IDirect3DDevice9Ex {
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

public:
    D3D9DeviceImpl(D9CDevice* dev, IDirect3D9Ex* factory,
                   UINT adapter, DWORD behaviorFlags, HWND window)
        : dev_(dev), factory_(factory)
        , adapter_(adapter), behaviorFlags_(behaviorFlags)
        , creationWindow_(window) {}

    ~D3D9DeviceImpl() {
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
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    /* ── device info ── */

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
        return hr32(dxmt9c_device_test_cooperative_level(dev_));
    }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return 0x80000000u; }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D) override {
        if (!ppD3D) return D3DERR_INVALIDCALL;
        factory_->AddRef();
        *ppD3D = static_cast<IDirect3D9*>(factory_);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override {
        if (!pCaps) return D3DERR_INVALIDCALL;
        D9CCaps cc{};
        HRESULT hr = hr32(dxmt9c_device_get_caps(dev_, &cc));
        if (SUCCEEDED(hr)) FillD3DCaps9(cc, pCaps);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT /*sc*/, D3DDISPLAYMODE* pMode) override {
        if (pMode) memset(pMode, 0, sizeof(*pMode)); return S_OK;
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
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT, UINT, IDirect3DSurface9*) override { return S_OK; }
    void    STDMETHODCALLTYPE SetCursorPosition(int, int, DWORD) override {}
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL) override { return FALSE; }

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
        *ppSC = new D3D9SwapChainImpl(sc);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index,
                                            IDirect3DSwapChain9** ppSC) override {
        if (!ppSC) return D3DERR_INVALIDCALL;
        D9CSwapChain* sc = dxmt9c_device_get_swap_chain(dev_, index);
        if (!sc) return D3DERR_INVALIDCALL;
        *ppSC = new D3D9SwapChainImpl(sc);
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
        releaseAllBound();
        return hr32(dxmt9c_device_reset(dev_, &cpp));
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty) override {
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        return hr32(dxmt9c_device_present(dev_,
            src ? &cs : nullptr, dst ? &cd : nullptr,
            (uint64_t)(uintptr_t)wnd, dirty, 0));
    }

    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT idx,
                                             D3DBACKBUFFER_TYPE,
                                             IDirect3DSurface9** ppS) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, sc);
        if (!chain) return D3DERR_INVALIDCALL;
        D9CSurface* s = dxmt9c_swapchain_get_back_buffer(chain, idx, 0);
        dxmt9c_swapchain_release(chain);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, nullptr);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT, D3DRASTER_STATUS* p) override {
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL) override { return S_OK; }
    void    STDMETHODCALLTYPE SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP*) override {}
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
        D9CTexture* t = dxmt9c_device_create_texture(dev_, w, h, levels,
                                                      usage, (uint32_t)fmt,
                                                      (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = new D3D9TextureImpl(t);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d,
                                                   UINT levels, DWORD usage,
                                                   D3DFORMAT fmt, D3DPOOL pool,
                                                   IDirect3DVolumeTexture9** ppTex,
                                                   HANDLE*) override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        D9CTexture* t = dxmt9c_device_create_volume_texture(dev_, w, h, d, levels,
                                                             usage, (uint32_t)fmt,
                                                             (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = new D3D9VolumeTextureImpl(t);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT size, UINT levels,
                                                 DWORD usage, D3DFORMAT fmt,
                                                 D3DPOOL pool,
                                                 IDirect3DCubeTexture9** ppTex,
                                                 HANDLE*) override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        D9CTexture* t = dxmt9c_device_create_cube_texture(dev_, size, levels,
                                                           usage, (uint32_t)fmt,
                                                           (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = new D3D9CubeTextureImpl(t);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage,
                                                  DWORD fvf, D3DPOOL pool,
                                                  IDirect3DVertexBuffer9** ppBuf,
                                                  HANDLE*) override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        D9CBuffer* b = dxmt9c_device_create_vertex_buffer(dev_, len, usage,
                                                           fvf, (uint32_t)pool);
        if (!b) return D3DERR_INVALIDCALL;
        *ppBuf = new D3D9VertexBufferImpl(b);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage,
                                                 D3DFORMAT fmt, D3DPOOL pool,
                                                 IDirect3DIndexBuffer9** ppBuf,
                                                 HANDLE*) override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        D9CBuffer* b = dxmt9c_device_create_index_buffer(dev_, len, usage,
                                                          (uint32_t)fmt,
                                                          (uint32_t)pool);
        if (!b) return D3DERR_INVALIDCALL;
        *ppBuf = new D3D9IndexBufferImpl(b);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt,
                                                  D3DMULTISAMPLE_TYPE ms,
                                                  DWORD msQual, BOOL lockable,
                                                  IDirect3DSurface9** ppS,
                                                  HANDLE* psh) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
        D9CSurface* s = dxmt9c_device_create_render_target(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)ms, msQual,
                                                            lockable ? 1u : 0u, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, nullptr);
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
        uint64_t sh = 0;
        D9CSurface* s = dxmt9c_device_create_depth_stencil(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)ms, msQual,
                                                            discard ? 1u : 0u, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, nullptr);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src,
                                             const RECT* srcRect,
                                             IDirect3DSurface9* dst,
                                             const POINT* dstPt) override {
        D9CRect cs{}, cd{};
        if (srcRect) cs = toR(*srcRect);
        if (dstPt) { cd.left = dstPt->x; cd.top = dstPt->y;
                     cd.right = dstPt->x; cd.bottom = dstPt->y; }
        return hr32(dxmt9c_device_update_surface(dev_,
            rawSurf(src), srcRect ? &cs : nullptr,
            rawSurf(dst), dstPt   ? &cd : nullptr));
    }

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,
                                             IDirect3DBaseTexture9* dst) override {
        return hr32(dxmt9c_device_update_texture(dev_,
                    rawTex(src), rawTex(dst)));
    }

    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt,
                                                   IDirect3DSurface9* dst) override {
        return hr32(dxmt9c_device_get_render_target_data(dev_,
                    rawSurf(rt), rawSurf(dst)));
    }

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT, IDirect3DSurface9*) override {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src,
                                           const RECT* srcRect,
                                           IDirect3DSurface9* dst,
                                           const RECT* dstRect,
                                           D3DTEXTUREFILTERTYPE filter) override {
        D9CRect cs{}, cd{};
        if (srcRect) cs = toR(*srcRect); if (dstRect) cd = toR(*dstRect);
        return hr32(dxmt9c_device_stretch_rect(dev_,
            rawSurf(src), srcRect ? &cs : nullptr,
            rawSurf(dst), dstRect ? &cd : nullptr,
            (uint32_t)filter));
    }

    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurf,
                                         const RECT* pRect,
                                         D3DCOLOR color) override {
        D9CRect cr{}; if (pRect) cr = toR(*pRect);
        return hr32(dxmt9c_device_color_fill(dev_, rawSurf(pSurf),
                    pRect ? &cr : nullptr, (uint32_t)color));
    }

    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DPOOL pool,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE*) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        uint64_t sh = 0;
        D9CSurface* s = dxmt9c_device_create_offscreen_surface(dev_, w, h,
                                                                (uint32_t)fmt,
                                                                (uint32_t)pool, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = new D3D9SurfaceImpl(s, nullptr);
        return S_OK;
    }

    /* ── render targets ── */

    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD idx,
                                               IDirect3DSurface9* pSurf) override {
        return hr32(dxmt9c_device_set_render_target(dev_, idx, rawSurf(pSurf)));
    }

    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx,
                                               IDirect3DSurface9** ppS) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        D9CSurface* s = dxmt9c_device_get_render_target(dev_, idx);
        *ppS = s ? new D3D9SurfaceImpl(s, nullptr) : nullptr;
        return s ? S_OK : D3DERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pSurf) override {
        return hr32(dxmt9c_device_set_depth_stencil(dev_, rawSurf(pSurf)));
    }

    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppS) override {
        if (!ppS) return D3DERR_INVALIDCALL;
        D9CSurface* s = dxmt9c_device_get_depth_stencil(dev_);
        *ppS = s ? new D3D9SurfaceImpl(s, nullptr) : nullptr;
        return s ? S_OK : S_FALSE;
    }

    /* ── scene ── */
    HRESULT STDMETHODCALLTYPE BeginScene() override { return hr32(dxmt9c_device_begin_scene(dev_)); }
    HRESULT STDMETHODCALLTYPE EndScene()   override { return hr32(dxmt9c_device_end_scene(dev_)); }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* pRects,
                                     DWORD flags, D3DCOLOR color,
                                     float z, DWORD stencil) override {
        return hr32(dxmt9c_device_clear(dev_, count,
            reinterpret_cast<const D9CRect*>(pRects),
            flags, (uint32_t)color, z, stencil));
    }

    /* ── transforms ── */
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* pM) override {
        if (!pM) return D3DERR_INVALIDCALL;
        return hr32(dxmt9c_device_set_transform(dev_, (uint32_t)state,
                    reinterpret_cast<const D9CMatrix*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state,
                                            D3DMATRIX* pM) override {
        if (!pM) return D3DERR_INVALIDCALL;
        return hr32(dxmt9c_device_get_transform(dev_, (uint32_t)state,
                    reinterpret_cast<D9CMatrix*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX* pM) override {
        if (!pM) return D3DERR_INVALIDCALL;
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
        D9CViewport vp{ pVP->X, pVP->Y, pVP->Width, pVP->Height,
                        pVP->MinZ, pVP->MaxZ };
        return hr32(dxmt9c_device_set_viewport(dev_, &vp));
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pVP) override {
        if (!pVP) return D3DERR_INVALIDCALL;
        D9CViewport vp{};
        dxmt9c_device_get_viewport(dev_, &vp);
        pVP->X = vp.x; pVP->Y = vp.y;
        pVP->Width = vp.width; pVP->Height = vp.height;
        pVP->MinZ = vp.minZ;   pVP->MaxZ   = vp.maxZ;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pR) override {
        if (!pR) return D3DERR_INVALIDCALL;
        D9CRect cr = toR(*pR);
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
        return hr32(dxmt9c_device_set_material(dev_,
                    reinterpret_cast<const D9CMaterial*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pM) override {
        if (!pM) return D3DERR_INVALIDCALL;
        return hr32(dxmt9c_device_get_material(dev_,
                    reinterpret_cast<D9CMaterial*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD idx, const D3DLIGHT9* pL) override {
        if (!pL) return D3DERR_INVALIDCALL;
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
        return hr32(dxmt9c_device_set_light(dev_, idx, &cl));
    }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD, D3DLIGHT9* pL) override {
        if (pL) memset(pL, 0, sizeof(*pL)); return S_OK; /* stub */
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) override {
        return hr32(dxmt9c_device_light_enable(dev_, idx, en ? 1u : 0u));
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD, BOOL* pEn) override {
        if (pEn) *pEn = FALSE; return S_OK; /* stub */
    }

    /* ── clip planes ── */
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* pPlane) override {
        return hr32(dxmt9c_device_set_clip_plane(dev_, idx, pPlane));
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* pPlane) override {
        return hr32(dxmt9c_device_get_clip_plane(dev_, idx, pPlane));
    }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* p) override {
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }

    /* ── render states ── */
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) override {
        return hr32(dxmt9c_device_set_render_state(dev_, (uint32_t)state, value));
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD* pValue) override {
        if (!pValue) return D3DERR_INVALIDCALL;
        *pValue = dxmt9c_device_get_render_state(dev_, (uint32_t)state);
        return S_OK;
    }

    /* ── state blocks ── */
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type,
                                                IDirect3DStateBlock9** ppSB) override {
        if (!ppSB) return D3DERR_INVALIDCALL;
        D9CStateBlock* sb = dxmt9c_device_create_state_block(dev_, (uint32_t)type);
        if (!sb) return D3DERR_INVALIDCALL;
        *ppSB = new D3D9StateBlockImpl(sb);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override {
        return hr32(dxmt9c_device_begin_state_block(dev_));
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override {
        if (!ppSB) return D3DERR_INVALIDCALL;
        D9CStateBlock* sb = nullptr;
        HRESULT hr = hr32(dxmt9c_device_end_state_block(dev_, &sb));
        if (SUCCEEDED(hr) && sb) *ppSB = new D3D9StateBlockImpl(sb);
        return hr;
    }

    /* ── texture stage / sampler states ── */
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD value) override {
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
        if (pPasses) *pPasses = 1; return S_OK;
    }

    /* ── palette (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT, const PALETTEENTRY*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT, PALETTEENTRY*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) override {
        if (p) *p = 0; return S_OK;
    }

    /* ── soft VP / NPatches (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL) override { return S_OK; }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float) override { return S_OK; }
    float   STDMETHODCALLTYPE GetNPatchMode() override { return 0.0f; }

    /* ── textures ── */
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage,
                                          IDirect3DBaseTexture9* pTex) override {
        setRef(textures_[stage < 16 ? stage : 0], pTex);
        return hr32(dxmt9c_device_set_texture(dev_, stage, rawTex(pTex)));
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage,
                                          IDirect3DBaseTexture9** ppTex) override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        IDirect3DBaseTexture9* t = textures_[stage < 16 ? stage : 0];
        if (t) t->AddRef();
        *ppTex = t;
        return S_OK;
    }

    /* ── FVF / vertex declaration ── */
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override {
        return hr32(dxmt9c_device_set_fvf(dev_, fvf));
    }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override {
        if (!pFVF) return D3DERR_INVALIDCALL;
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
        *ppVD = new D3D9VertexDeclImpl(d);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(
            IDirect3DVertexDeclaration9* pVD) override {
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
        D9CShader* s = dxmt9c_device_create_vertex_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
        if (!s) return D3DERR_INVALIDCALL;
        *ppVS = new D3D9VertexShaderImpl(s);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) override {
        setRef(vs_, pVS);
        return hr32(dxmt9c_device_set_vertex_shader(dev_, rawVS(pVS)));
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppVS) override {
        if (!ppVS) return D3DERR_INVALIDCALL;
        if (vs_) vs_->AddRef(); *ppVS = vs_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* pData,
                                                        UINT count) override {
        return hr32(dxmt9c_device_set_vs_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* pData,
                                                        UINT count) override {
        return hr32(dxmt9c_device_get_vs_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) override {
        return hr32(dxmt9c_device_set_vs_const_i(dev_, start,
                    reinterpret_cast<const int32_t*>(pData), count));
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, INT* pData,
                                                        UINT count) override {
        (void)start; (void)pData; (void)count; return S_OK; /* stub */
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) override {
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
        setRef(streamSrc_[stream < 16 ? stream : 0], pBuf);
        streamOff_[stream < 16 ? stream : 0] = offset;
        streamStr_[stream < 16 ? stream : 0] = stride;
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
        streamFreq_[stream < 16 ? stream : 0] = freq;
        return hr32(dxmt9c_device_set_stream_source_freq(dev_, stream, freq));
    }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* pFreq) override {
        if (pFreq) *pFreq = streamFreq_[stream < 16 ? stream : 0]; return S_OK;
    }

    /* ── indices ── */
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIBuf) override {
        setRef(indexBuf_, pIBuf);
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
        D9CShader* s = dxmt9c_device_create_pixel_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
        if (!s) return D3DERR_INVALIDCALL;
        *ppPS = new D3D9PixelShaderImpl(s);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pPS) override {
        setRef(ps_, pPS);
        return hr32(dxmt9c_device_set_pixel_shader(dev_, rawPS(pPS)));
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) override {
        if (!ppPS) return D3DERR_INVALIDCALL;
        if (ps_) ps_->AddRef(); *ppPS = ps_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) override {
        return hr32(dxmt9c_device_set_ps_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* pData,
                                                       UINT count) override {
        return hr32(dxmt9c_device_get_ps_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) override {
        return hr32(dxmt9c_device_set_ps_const_i(dev_, start,
                    reinterpret_cast<const int32_t*>(pData), count));
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, INT* pData,
                                                       UINT count) override {
        (void)start; (void)pData; (void)count; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) override {
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
        return hr32(dxmt9c_device_draw_primitive(dev_, (uint32_t)type,
                    startVertex, count));
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type,
                                                    INT baseVertex,
                                                    UINT minVertex, UINT numVertices,
                                                    UINT startIndex,
                                                    UINT count) override {
        return hr32(dxmt9c_device_draw_indexed_primitive(dev_, (uint32_t)type,
                    baseVertex, minVertex, numVertices, startIndex, count));
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                               UINT count,
                                               const void* pData,
                                               UINT stride) override {
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
        return hr32(dxmt9c_device_draw_indexed_primitive_up(dev_,
                    (uint32_t)type, minVertex, numVertices, count,
                    pIdxData, (uint32_t)idxFmt, pVtxData, stride));
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT,
                                               IDirect3DVertexBuffer9*,
                                               IDirect3DVertexDeclaration9*,
                                               DWORD) override {
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
        *ppQ = new D3D9QueryImpl(q);
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
        releaseAllBound();
        return hr32(dxmt9c_device_reset_ex(dev_, &cpp, pFsMode ? &cdme : nullptr));
    }

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT /*sc*/,
                                                D3DDISPLAYMODEEX* pMode,
                                                D3DDISPLAYROTATION* pRot) override {
        if (pMode) memset(pMode, 0, sizeof(*pMode));
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
