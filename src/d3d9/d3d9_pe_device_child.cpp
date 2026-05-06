/* src/d3d9/d3d9_pe_device_child.cpp — PE-side child COM wrappers. */

#include "d3d9_pe_device_child.hpp"

#include "util/com/com_private_data.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

static D3DFORMAT exposeAdapterDisplayFormat(D3DFORMAT fmt) {
  if (fmt == D3DFMT_A8R8G8B8)
    return D3DFMT_X8R8G8B8;
  return fmt;
}

static void dxmt9DeviceDebugLog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-device", fmt, args);
  va_end(args);
}

static bool dxmt9LeakStateBlocksEnabled() {
  static const bool enabled = dxmt9::util::getenvFlag("DXMT_LEAK_STATEBLOCKS");
  return enabled;
}

static HRESULT setPrivateData(dxmt9::util::ComPrivateData &storage,
                              REFGUID guid, const void *data, DWORD size,
                              DWORD flags, const char *label,
                              const void *self) {
  HRESULT hr = D3DERR_INVALIDCALL;
  if ((flags & D3DSPD_IUNKNOWN) != 0) {
    if (data && size == sizeof(IUnknown *)) {
      hr = storage.setInterface(guid, static_cast<const IUnknown *>(data));
    }
  } else {
    hr = storage.setData(guid, size, data);
  }
  dxmt9DeviceDebugLog(
      "%s_set_private_data this=%p size=%u flags=0x%x -> hr=0x%08x", label,
      self, (unsigned)size, (unsigned)flags, (unsigned)hr);
  return hr;
}

static HRESULT getPrivateData(dxmt9::util::ComPrivateData &storage,
                              REFGUID guid, void *data, DWORD *size,
                              const char *label, const void *self) {
  UINT localSize = size ? static_cast<UINT>(*size) : 0u;
  HRESULT hr = storage.getData(guid, &localSize, data);
  if (size && hr != D3DERR_NOTFOUND) {
    *size = static_cast<DWORD>(localSize);
  }
  dxmt9DeviceDebugLog(
      "%s_get_private_data this=%p data=%p size=%u -> hr=0x%08x", label, self,
      data, size ? (unsigned)*size : 0u, (unsigned)hr);
  return hr;
}

static HRESULT freePrivateData(dxmt9::util::ComPrivateData &storage,
                               REFGUID guid, const char *label,
                               const void *self) {
  HRESULT hr = storage.removeData(guid);
  dxmt9DeviceDebugLog("%s_free_private_data this=%p -> hr=0x%08x", label, self,
                      (unsigned)hr);
  return hr;
}

static D9CRect toR(const RECT &r) {
  D9CRect c;
  c.left = r.left;
  c.top = r.top;
  c.right = r.right;
  c.bottom = r.bottom;
  return c;
}

static D9CRect toR(const D3DBOX &b) {
  D9CRect c;
  c.left = static_cast<int32_t>(b.Left);
  c.top = static_cast<int32_t>(b.Top);
  c.right = static_cast<int32_t>(b.Right);
  c.bottom = static_cast<int32_t>(b.Bottom);
  return c;
}

static HRESULT flushChildRecorder(D3D9PeRecorderFlush *recorder) {
  return recorder ? recorder->FlushPeRecorderForChild() : S_OK;
}

static bool isChildStateBlockRecording(D3D9PeRecorderFlush *recorder) {
  return recorder && recorder->IsStateBlockRecordingForChild();
}

static HRESULT textureLevelDesc(D9CTexture *texture, UINT level,
                                D9CSurfaceDesc *desc) {
  if (!texture || !desc)
    return D3DERR_INVALIDCALL;
  return hr32(dxmt9c_texture_get_level_desc(texture, level, desc));
}

static bool textureIsManaged(D9CTexture *texture) {
  D9CSurfaceDesc desc{};
  return SUCCEEDED(textureLevelDesc(texture, 0, &desc)) &&
         desc.pool == D3DPOOL_MANAGED;
}

static DWORD setTextureLod(D9CTexture *texture, DWORD &lod, DWORD value) {
  if (!textureIsManaged(texture)) {
    return 0;
  }
  const DWORD previous = lod;
  const DWORD levelCount = dxmt9c_texture_get_level_count(texture);
  lod = std::min<DWORD>(value, levelCount > 0 ? levelCount - 1 : 0);
  return previous;
}

static HRESULT setAutoGenFilter(D3DTEXTUREFILTERTYPE &filter,
                                D3DTEXTUREFILTERTYPE value) {
  if (value == D3DTEXF_NONE) {
    return D3DERR_INVALIDCALL;
  }
  filter = value;
  return S_OK;
}

static HRESULT lockTextureBox(D9CTexture *texture, UINT level,
                              D3DLOCKED_BOX *locked, const D3DBOX *box,
                              DWORD flags, D3D9PeRecorderFlush *recorder) {
  if (!locked)
    return D3DERR_INVALIDCALL;
  const HRESULT flushHr = flushChildRecorder(recorder);
  if (FAILED(flushHr))
    return flushHr;

  D9CLockedRect lockedRect{};
  D9CRect rect{};
  if (box)
    rect = toR(*box);
  const HRESULT hr = hr32(dxmt9c_texture_lock_rect(
      texture, level, &lockedRect, box ? &rect : nullptr, flags));
  if (FAILED(hr))
    return hr;

  D9CSurfaceDesc desc{};
  UINT height = 1;
  if (box && box->Bottom > box->Top) {
    height = box->Bottom - box->Top;
  } else if (SUCCEEDED(textureLevelDesc(texture, level, &desc))) {
    height = std::max<UINT>(1, desc.height);
  }
  locked->RowPitch = lockedRect.pitch;
  locked->SlicePitch = lockedRect.pitch * static_cast<int>(height);
  locked->pBits = lockedRect.bits;
  return S_OK;
}

static HRESULT unlockTextureBox(D9CTexture *texture, UINT level,
                                D3D9PeRecorderFlush *recorder) {
  const HRESULT flushHr = flushChildRecorder(recorder);
  if (FAILED(flushHr))
    return flushHr;
  return hr32(dxmt9c_texture_unlock_rect(texture, level));
}

static bool surfaceIsDefaultPool(D9CSurface *surface) {
  if (!surface)
    return false;
  D9CSurfaceDesc desc{};
  return SUCCEEDED(hr32(dxmt9c_surface_get_desc(surface, &desc))) &&
         desc.pool == D3DPOOL_DEFAULT;
}

static bool textureIsDefaultPool(D9CTexture *texture) {
  if (!texture)
    return false;
  D9CSurfaceDesc desc{};
  return SUCCEEDED(textureLevelDesc(texture, 0, &desc)) &&
         desc.pool == D3DPOOL_DEFAULT;
}

static bool bufferIsDefaultPool(D9CBuffer *buffer) {
  if (!buffer)
    return false;
  D9CBufferDesc desc{};
  return SUCCEEDED(hr32(dxmt9c_buffer_get_desc(buffer, &desc))) &&
         desc.pool == D3DPOOL_DEFAULT;
}

static void trackDefaultPoolResource(D3D9PeRecorderFlush *recorder,
                                     bool &tracked, bool shouldTrack) {
  if (!recorder || !shouldTrack || tracked)
    return;
  tracked = true;
  recorder->AddDefaultPoolResourceRefForChild();
}

static void untrackDefaultPoolResource(D3D9PeRecorderFlush *recorder,
                                       bool &tracked) {
  if (!recorder || !tracked)
    return;
  tracked = false;
  recorder->ReleaseDefaultPoolResourceRefForChild();
}

/* =========================================================================
 * Resource COM wrappers
 * Each wrapper holds a D9C* handle (owns one refcount) and exposes raw()
 * for the device to extract the handle when binding the resource.
 * ========================================================================= */

/* ── Surface ────────────────────────────────────────────────────────────────
 */

class D3D9SurfaceImpl final : public IDirect3DSurface9 {
  ULONG refs_ = 1;
  D9CSurface *s_;
  IDirect3DDevice9 *device_;
  IUnknown *container_;
  D3D9PeRecorderFlush *recorder_;
  HDC dc_ = nullptr;
  bool defaultPoolTracked_ = false;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9SurfaceImpl(D9CSurface *s, IDirect3DDevice9 *device, IUnknown *container,
                  D3D9PeRecorderFlush *recorder = nullptr,
                  bool trackDefaultPool = true)
      : s_(s), device_(device), container_(container), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    if (container_)
      container_->AddRef();
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             trackDefaultPool && surfaceIsDefaultPool(s_));
  }
  ~D3D9SurfaceImpl() {
    untrackDefaultPoolResource(recorder_, defaultPoolTracked_);
    if (dc_)
      DeleteDC(dc_);
    dxmt9c_surface_release(s_);
    if (container_)
      container_->Release();
    if (device_)
      device_->Release();
  }

  D9CSurface *raw() const { return s_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DResource9) ||
        IsEqualGUID(riid, IID_IDirect3DSurface9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      dxmt9DeviceDebugLog("surface_get_device this=%p -> invalid (device=null)",
                          this);
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    dxmt9DeviceDebugLog("surface_get_device this=%p -> device=%p", this,
                        static_cast<void *>(device_));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data,
                                           DWORD size,
                                           DWORD flags) noexcept override {
    return setPrivateData(privateData_, guid, data, size, flags, "surface",
                          this);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data,
                                           DWORD *size) noexcept override {
    return getPrivateData(privateData_, guid, data, size, "surface", this);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
    return freePrivateData(privateData_, guid, "surface", this);
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
  void STDMETHODCALLTYPE PreLoad() noexcept override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override {
    return D3DRTYPE_SURFACE;
  }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid,
                                         void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (!container_) {
      *ppv = nullptr;
      return D3DERR_INVALIDCALL;
    }
    return container_->QueryInterface(riid, ppv);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pD) noexcept override {
    if (!pD)
      return D3DERR_INVALIDCALL;
    D9CSurfaceDesc sd{};
    HRESULT hr = hr32(dxmt9c_surface_get_desc(s_, &sd));
    if (SUCCEEDED(hr)) {
      pD->Format = (D3DFORMAT)sd.format;
      pD->Type = (D3DRESOURCETYPE)sd.resourceType;
      pD->Usage = sd.usage;
      pD->Pool = (D3DPOOL)sd.pool;
      pD->MultiSampleType = (D3DMULTISAMPLE_TYPE)sd.multiSampleType;
      pD->MultiSampleQuality = sd.multiSampleQuality;
      pD->Width = sd.width;
      pD->Height = sd.height;
      dxmt9DeviceDebugLog("surface_get_desc this=%p fmt=%u usage=0x%x pool=%u "
                          "msaa=%u/%u size=%ux%u",
                          this, (unsigned)pD->Format, (unsigned)pD->Usage,
                          (unsigned)pD->Pool, (unsigned)pD->MultiSampleType,
                          (unsigned)pD->MultiSampleQuality, (unsigned)pD->Width,
                          (unsigned)pD->Height);
    } else {
      dxmt9DeviceDebugLog("surface_get_desc this=%p -> hr=0x%08x", this,
                          (unsigned)hr);
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLR, const RECT *pRect,
                                     DWORD flags) noexcept override {
    if (!pLR)
      return D3DERR_INVALIDCALL;
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    dxmt9DeviceDebugLog("surface_lock_rect surface=%p flags=0x%x rect=%s", this,
                        (unsigned)flags, pRect ? "<custom>" : "<full>");
    D9CLockedRect lr{};
    D9CRect cr{};
    if (pRect)
      cr = toR(*pRect);
    HRESULT hr =
        hr32(dxmt9c_surface_lock_rect(s_, &lr, pRect ? &cr : nullptr, flags));
    if (SUCCEEDED(hr)) {
      pLR->Pitch = lr.pitch;
      pLR->pBits = lr.bits;
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
    if (FAILED(flushHr))
      return flushHr;
    return hr32(dxmt9c_surface_unlock_rect(s_));
  }
  HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) noexcept override {
    dxmt9DeviceDebugLog("surface_get_dc surface=%p phdc=%p", this, phdc);
    if (!phdc)
      return D3DERR_INVALIDCALL;
    if (dc_)
      return D3DERR_INVALIDCALL;
    dc_ = CreateCompatibleDC(nullptr);
    if (!dc_)
      return E_FAIL;
    *phdc = dc_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) noexcept override {
    dxmt9DeviceDebugLog("surface_release_dc surface=%p hdc=%p", this, hdc);
    if (!dc_ || hdc != dc_)
      return D3DERR_INVALIDCALL;
    DeleteDC(dc_);
    dc_ = nullptr;
    return S_OK;
  }
};

/* ── Texture2D ──────────────────────────────────────────────────────────────
 */

class D3D9TextureImpl final : public IDirect3DTexture9 {
  ULONG refs_ = 1;
  D9CTexture *t_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  DWORD lod_ = 0;
  D3DTEXTUREFILTERTYPE autoGenFilter_ = D3DTEXF_LINEAR;
  bool defaultPoolTracked_ = false;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9TextureImpl(D9CTexture *t, IDirect3DDevice9 *device,
                  D3D9PeRecorderFlush *recorder = nullptr)
      : t_(t), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             textureIsDefaultPool(t_));
  }
  ~D3D9TextureImpl() {
    untrackDefaultPoolResource(recorder_, defaultPoolTracked_);
    dxmt9c_texture_release(t_);
    if (device_)
      device_->Release();
  }

  D9CTexture *raw() const { return t_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DResource9) ||
        IsEqualGUID(riid, IID_IDirect3DBaseTexture9) ||
        IsEqualGUID(riid, IID_IDirect3DTexture9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      dxmt9DeviceDebugLog("texture_get_device this=%p -> invalid (device=null)",
                          this);
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    dxmt9DeviceDebugLog("texture_get_device this=%p -> device=%p", this,
                        static_cast<void *>(device_));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data,
                                           DWORD size,
                                           DWORD flags) noexcept override {
    return setPrivateData(privateData_, guid, data, size, flags, "texture",
                          this);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data,
                                           DWORD *size) noexcept override {
    return getPrivateData(privateData_, guid, data, size, "texture", this);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
    return freePrivateData(privateData_, guid, "texture", this);
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
  void STDMETHODCALLTYPE PreLoad() noexcept override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override {
    return D3DRTYPE_TEXTURE;
  }
  DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) noexcept override {
    return setTextureLod(t_, lod_, lod);
  }
  DWORD STDMETHODCALLTYPE GetLOD() noexcept override {
    return textureIsManaged(t_) ? lod_ : 0;
  }
  DWORD STDMETHODCALLTYPE GetLevelCount() noexcept override {
    return dxmt9c_texture_get_level_count(t_);
  }
  HRESULT STDMETHODCALLTYPE
  SetAutoGenFilterType(D3DTEXTUREFILTERTYPE filter) noexcept override {
    return setAutoGenFilter(autoGenFilter_, filter);
  }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
  GetAutoGenFilterType() noexcept override {
    return autoGenFilter_;
  }
  void STDMETHODCALLTYPE GenerateMipSubLevels() noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return;
    dxmt9c_texture_generate_mip_sublevels(t_);
  }
  HRESULT STDMETHODCALLTYPE
  GetLevelDesc(UINT level, D3DSURFACE_DESC *pD) noexcept override {
    if (!pD)
      return D3DERR_INVALIDCALL;
    D9CSurfaceDesc sd{};
    HRESULT hr = hr32(dxmt9c_texture_get_level_desc(t_, level, &sd));
    if (SUCCEEDED(hr)) {
      pD->Format = (D3DFORMAT)sd.format;
      pD->Type = D3DRTYPE_TEXTURE;
      pD->Usage = sd.usage;
      pD->Pool = (D3DPOOL)sd.pool;
      pD->MultiSampleType = (D3DMULTISAMPLE_TYPE)sd.multiSampleType;
      pD->MultiSampleQuality = sd.multiSampleQuality;
      pD->Width = sd.width;
      pD->Height = sd.height;
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE
  GetSurfaceLevel(UINT level, IDirect3DSurface9 **ppS) noexcept override {
    if (!ppS)
      return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    dxmt9DeviceDebugLog("texture_get_surface_level this=%p level=%u", this,
                        level);
    D9CSurface *s = dxmt9c_texture_get_surface_level(t_, level);
    if (!s) {
      dxmt9DeviceDebugLog(
          "texture_get_surface_level this=%p level=%u -> invalid", this, level);
      return D3DERR_INVALIDCALL;
    }
    *ppS = new D3D9SurfaceImpl(
        s, device_, static_cast<IDirect3DBaseTexture9 *>(this), recorder_);
    dxmt9DeviceDebugLog(
        "texture_get_surface_level this=%p level=%u -> surface=%p", this, level,
        *ppS);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT *pLR,
                                     const RECT *pRect,
                                     DWORD flags) noexcept override {
    if (!pLR)
      return D3DERR_INVALIDCALL;
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    dxmt9DeviceDebugLog(
        "texture_lock_rect texture=%p level=%u flags=0x%x rect=%s", this,
        (unsigned)level, (unsigned)flags, pRect ? "<custom>" : "<full>");
    D9CLockedRect lr{};
    D9CRect cr{};
    if (pRect)
      cr = toR(*pRect);
    HRESULT hr = hr32(
        dxmt9c_texture_lock_rect(t_, level, &lr, pRect ? &cr : nullptr, flags));
    if (SUCCEEDED(hr)) {
      pLR->Pitch = lr.pitch;
      pLR->pBits = lr.bits;
      dxmt9DeviceDebugLog("texture_lock_rect -> pitch=%ld bits=%p",
                          (long)pLR->Pitch, pLR->pBits);
    } else {
      dxmt9DeviceDebugLog("texture_lock_rect -> hr=0x%08x", (unsigned)hr);
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    dxmt9DeviceDebugLog("texture_unlock_rect texture=%p level=%u", this,
                        (unsigned)level);
    const HRESULT hr = hr32(dxmt9c_texture_unlock_rect(t_, level));
    if (FAILED(hr)) {
      dxmt9DeviceDebugLog("texture_unlock_rect -> hr=0x%08x", (unsigned)hr);
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *) noexcept override {
    return S_OK;
  }
};

/* ── CubeTexture ────────────────────────────────────────────────────────────
 */

class D3D9CubeTextureImpl final : public IDirect3DCubeTexture9 {
  ULONG refs_ = 1;
  D9CTexture *t_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  DWORD lod_ = 0;
  D3DTEXTUREFILTERTYPE autoGenFilter_ = D3DTEXF_LINEAR;
  bool defaultPoolTracked_ = false;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9CubeTextureImpl(D9CTexture *t, IDirect3DDevice9 *device,
                      D3D9PeRecorderFlush *recorder = nullptr)
      : t_(t), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             textureIsDefaultPool(t_));
  }
  ~D3D9CubeTextureImpl() {
    untrackDefaultPoolResource(recorder_, defaultPoolTracked_);
    dxmt9c_texture_release(t_);
    if (device_)
      device_->Release();
  }

  D9CTexture *raw() const { return t_; }

  bool subresourceIndex(D3DCUBEMAP_FACES face, UINT level, UINT &out) const {
    const UINT mipCount = dxmt9c_texture_get_level_count(t_);
    if (static_cast<UINT>(face) >= 6u || level >= mipCount) {
      return false;
    }
    out = static_cast<UINT>(face) * mipCount + level;
    return true;
  }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DResource9) ||
        IsEqualGUID(riid, IID_IDirect3DBaseTexture9) ||
        IsEqualGUID(riid, IID_IDirect3DCubeTexture9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      dxmt9DeviceDebugLog(
          "swapchain_get_device this=%p -> invalid (device=null)", this);
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    dxmt9DeviceDebugLog("swapchain_get_device this=%p -> device=%p", this,
                        static_cast<void *>(device_));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data,
                                           DWORD size,
                                           DWORD flags) noexcept override {
    return setPrivateData(privateData_, guid, data, size, flags, "cube", this);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data,
                                           DWORD *size) noexcept override {
    return getPrivateData(privateData_, guid, data, size, "cube", this);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
    return freePrivateData(privateData_, guid, "cube", this);
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
  void STDMETHODCALLTYPE PreLoad() noexcept override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override {
    return D3DRTYPE_CUBETEXTURE;
  }
  DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) noexcept override {
    return setTextureLod(t_, lod_, lod);
  }
  DWORD STDMETHODCALLTYPE GetLOD() noexcept override {
    return textureIsManaged(t_) ? lod_ : 0;
  }
  DWORD STDMETHODCALLTYPE GetLevelCount() noexcept override {
    return dxmt9c_texture_get_level_count(t_);
  }
  HRESULT STDMETHODCALLTYPE
  SetAutoGenFilterType(D3DTEXTUREFILTERTYPE filter) noexcept override {
    return setAutoGenFilter(autoGenFilter_, filter);
  }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
  GetAutoGenFilterType() noexcept override {
    return autoGenFilter_;
  }
  void STDMETHODCALLTYPE GenerateMipSubLevels() noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return;
    dxmt9c_texture_generate_mip_sublevels(t_);
  }
  HRESULT STDMETHODCALLTYPE
  GetLevelDesc(UINT level, D3DSURFACE_DESC *pD) noexcept override {
    if (!pD)
      return D3DERR_INVALIDCALL;
    D9CSurfaceDesc sd{};
    HRESULT hr = hr32(dxmt9c_texture_get_level_desc(t_, level, &sd));
    if (SUCCEEDED(hr)) {
      pD->Format = (D3DFORMAT)sd.format;
      pD->Type = D3DRTYPE_CUBETEXTURE;
      pD->Usage = sd.usage;
      pD->Pool = (D3DPOOL)sd.pool;
      pD->MultiSampleType = (D3DMULTISAMPLE_TYPE)sd.multiSampleType;
      pD->MultiSampleQuality = sd.multiSampleQuality;
      pD->Width = sd.width;
      pD->Height = sd.height;
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE
  GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level,
                    IDirect3DSurface9 **ppS) noexcept override {
    if (!ppS)
      return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    dxmt9DeviceDebugLog("cube_get_surface_level this=%p face=%u level=%u", this,
                        static_cast<unsigned>(face), level);
    UINT idx = 0;
    if (!subresourceIndex(face, level, idx)) {
      dxmt9DeviceDebugLog(
          "cube_get_surface_level this=%p face=%u level=%u -> invalid", this,
          static_cast<unsigned>(face), level);
      return D3DERR_INVALIDCALL;
    }
    D9CSurface *s = dxmt9c_texture_get_surface_level(t_, idx);
    if (!s) {
      dxmt9DeviceDebugLog(
          "cube_get_surface_level this=%p face=%u level=%u -> invalid", this,
          static_cast<unsigned>(face), level);
      return D3DERR_INVALIDCALL;
    }
    *ppS = new D3D9SurfaceImpl(
        s, device_, static_cast<IDirect3DBaseTexture9 *>(this), recorder_);
    dxmt9DeviceDebugLog(
        "cube_get_surface_level this=%p face=%u level=%u -> surface=%p", this,
        static_cast<unsigned>(face), level, *ppS);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level,
                                     D3DLOCKED_RECT *pLR, const RECT *pRect,
                                     DWORD flags) noexcept override {
    if (!pLR)
      return D3DERR_INVALIDCALL;
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    UINT idx = 0;
    if (!subresourceIndex(face, level, idx))
      return D3DERR_INVALIDCALL;
    D9CLockedRect lr{};
    D9CRect cr{};
    if (pRect)
      cr = toR(*pRect);
    HRESULT hr = hr32(
        dxmt9c_texture_lock_rect(t_, idx, &lr, pRect ? &cr : nullptr, flags));
    if (SUCCEEDED(hr)) {
      pLR->Pitch = lr.pitch;
      pLR->pBits = lr.bits;
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face,
                                       UINT level) noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    UINT idx = 0;
    if (!subresourceIndex(face, level, idx))
      return D3DERR_INVALIDCALL;
    return hr32(dxmt9c_texture_unlock_rect(t_, idx));
  }
  HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES,
                                         const RECT *) noexcept override {
    return S_OK;
  }
};

/* ── Volume ─────────────────────────────────────────────────────────────────
 */

class D3D9VolumeImpl final : public IDirect3DVolume9 {
  ULONG refs_ = 1;
  D9CTexture *t_;
  IDirect3DDevice9 *device_;
  IUnknown *container_;
  D3D9PeRecorderFlush *recorder_;
  UINT level_;
  bool defaultPoolTracked_ = false;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9VolumeImpl(D9CTexture *t, IDirect3DDevice9 *device, IUnknown *container,
                 D3D9PeRecorderFlush *recorder, UINT level)
      : t_(t), device_(device), container_(container), recorder_(recorder),
        level_(level) {
    if (t_)
      dxmt9c_texture_addref(t_);
    if (device_)
      device_->AddRef();
    if (container_)
      container_->AddRef();
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             textureIsDefaultPool(t_));
  }
  ~D3D9VolumeImpl() {
    untrackDefaultPoolResource(recorder_, defaultPoolTracked_);
    if (t_)
      dxmt9c_texture_release(t_);
    if (container_)
      container_->Release();
    if (device_)
      device_->Release();
  }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DVolume9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data,
                                           DWORD size,
                                           DWORD flags) noexcept override {
    return setPrivateData(privateData_, guid, data, size, flags, "volume-level",
                          this);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data,
                                           DWORD *size) noexcept override {
    return getPrivateData(privateData_, guid, data, size, "volume-level", this);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
    return freePrivateData(privateData_, guid, "volume-level", this);
  }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid,
                                         void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (!container_) {
      *ppv = nullptr;
      return D3DERR_INVALIDCALL;
    }
    return container_->QueryInterface(riid, ppv);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC *pD) noexcept override {
    if (!pD)
      return D3DERR_INVALIDCALL;
    D9CSurfaceDesc sd{};
    const HRESULT hr = hr32(dxmt9c_texture_get_level_desc(t_, level_, &sd));
    if (FAILED(hr))
      return hr;
    pD->Format = static_cast<D3DFORMAT>(sd.format);
    pD->Type = D3DRTYPE_VOLUME;
    pD->Usage = sd.usage;
    pD->Pool = static_cast<D3DPOOL>(sd.pool);
    pD->Width = sd.width;
    pD->Height = sd.height;
    pD->Depth = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX *locked, const D3DBOX *box,
                                    DWORD flags) noexcept override {
    return lockTextureBox(t_, level_, locked, box, flags, recorder_);
  }
  HRESULT STDMETHODCALLTYPE UnlockBox() noexcept override {
    return unlockTextureBox(t_, level_, recorder_);
  }
};

/* ── VolumeTexture ──────────────────────────────────────────────────────────
 */

class D3D9VolumeTextureImpl final : public IDirect3DVolumeTexture9 {
  ULONG refs_ = 1;
  D9CTexture *t_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  DWORD lod_ = 0;
  D3DTEXTUREFILTERTYPE autoGenFilter_ = D3DTEXF_LINEAR;
  bool defaultPoolTracked_ = false;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9VolumeTextureImpl(D9CTexture *t, IDirect3DDevice9 *device,
                        D3D9PeRecorderFlush *recorder = nullptr)
      : t_(t), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             textureIsDefaultPool(t_));
  }
  ~D3D9VolumeTextureImpl() {
    untrackDefaultPoolResource(recorder_, defaultPoolTracked_);
    dxmt9c_texture_release(t_);
    if (device_)
      device_->Release();
  }

  D9CTexture *raw() const { return t_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DResource9) ||
        IsEqualGUID(riid, IID_IDirect3DBaseTexture9) ||
        IsEqualGUID(riid, IID_IDirect3DVolumeTexture9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      dxmt9DeviceDebugLog(
          "stateblock_get_device this=%p -> invalid (device=null)", this);
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    dxmt9DeviceDebugLog("stateblock_get_device this=%p -> device=%p", this,
                        static_cast<void *>(device_));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data,
                                           DWORD size,
                                           DWORD flags) noexcept override {
    return setPrivateData(privateData_, guid, data, size, flags, "volume",
                          this);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data,
                                           DWORD *size) noexcept override {
    return getPrivateData(privateData_, guid, data, size, "volume", this);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
    return freePrivateData(privateData_, guid, "volume", this);
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
  void STDMETHODCALLTYPE PreLoad() noexcept override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override {
    return D3DRTYPE_VOLUMETEXTURE;
  }
  DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) noexcept override {
    return setTextureLod(t_, lod_, lod);
  }
  DWORD STDMETHODCALLTYPE GetLOD() noexcept override {
    return textureIsManaged(t_) ? lod_ : 0;
  }
  DWORD STDMETHODCALLTYPE GetLevelCount() noexcept override {
    return dxmt9c_texture_get_level_count(t_);
  }
  HRESULT STDMETHODCALLTYPE
  SetAutoGenFilterType(D3DTEXTUREFILTERTYPE filter) noexcept override {
    return setAutoGenFilter(autoGenFilter_, filter);
  }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
  GetAutoGenFilterType() noexcept override {
    return autoGenFilter_;
  }
  void STDMETHODCALLTYPE GenerateMipSubLevels() noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return;
    dxmt9c_texture_generate_mip_sublevels(t_);
  }
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level,
                                         D3DVOLUME_DESC *pD) noexcept override {
    if (!pD)
      return D3DERR_INVALIDCALL;
    D9CSurfaceDesc sd{};
    const HRESULT hr = hr32(dxmt9c_texture_get_level_desc(t_, level, &sd));
    if (FAILED(hr))
      return hr;
    pD->Format = static_cast<D3DFORMAT>(sd.format);
    pD->Type = D3DRTYPE_VOLUME;
    pD->Usage = sd.usage;
    pD->Pool = static_cast<D3DPOOL>(sd.pool);
    pD->Width = sd.width;
    pD->Height = sd.height;
    pD->Depth = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetVolumeLevel(UINT level, IDirect3DVolume9 **ppVolume) noexcept override {
    if (!ppVolume)
      return D3DERR_INVALIDCALL;
    *ppVolume = nullptr;
    if (level >= dxmt9c_texture_get_level_count(t_))
      return D3DERR_INVALIDCALL;
    *ppVolume = new D3D9VolumeImpl(t_, device_,
                                   static_cast<IDirect3DBaseTexture9 *>(this),
                                   recorder_, level);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE LockBox(UINT level, D3DLOCKED_BOX *locked,
                                    const D3DBOX *box,
                                    DWORD flags) noexcept override {
    if (level >= dxmt9c_texture_get_level_count(t_))
      return D3DERR_INVALIDCALL;
    return lockTextureBox(t_, level, locked, box, flags, recorder_);
  }
  HRESULT STDMETHODCALLTYPE UnlockBox(UINT level) noexcept override {
    if (level >= dxmt9c_texture_get_level_count(t_))
      return D3DERR_INVALIDCALL;
    return unlockTextureBox(t_, level, recorder_);
  }
  HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX *) noexcept override {
    return S_OK;
  }
};

/* ── VertexBuffer ───────────────────────────────────────────────────────────
 */

class D3D9VertexBufferImpl final : public IDirect3DVertexBuffer9 {
  ULONG refs_ = 1;
  D9CBuffer *b_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  bool defaultPoolTracked_ = false;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9VertexBufferImpl(D9CBuffer *b, IDirect3DDevice9 *device,
                       D3D9PeRecorderFlush *recorder = nullptr)
      : b_(b), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             bufferIsDefaultPool(b_));
  }
  ~D3D9VertexBufferImpl() {
    untrackDefaultPoolResource(recorder_, defaultPoolTracked_);
    dxmt9c_buffer_release(b_);
    if (device_)
      device_->Release();
  }

  D9CBuffer *raw() const { return b_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DResource9) ||
        IsEqualGUID(riid, IID_IDirect3DVertexBuffer9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data,
                                           DWORD size,
                                           DWORD flags) noexcept override {
    return setPrivateData(privateData_, guid, data, size, flags, "vb", this);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data,
                                           DWORD *size) noexcept override {
    return getPrivateData(privateData_, guid, data, size, "vb", this);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
    return freePrivateData(privateData_, guid, "vb", this);
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
  void STDMETHODCALLTYPE PreLoad() noexcept override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override {
    return D3DRTYPE_VERTEXBUFFER;
  }
  HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void **pp,
                                 DWORD flags) noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    return hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
  }
  HRESULT STDMETHODCALLTYPE Unlock() noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    return hr32(dxmt9c_buffer_unlock(b_));
  }
  HRESULT STDMETHODCALLTYPE
  GetDesc(D3DVERTEXBUFFER_DESC *pDesc) noexcept override {
    if (!pDesc)
      return D3DERR_INVALIDCALL;
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
    dxmt9DeviceDebugLog(
        "vb_get_desc vb=%p -> size=%u usage=0x%x pool=%u fvf=0x%x", this,
        desc.size, desc.usage, desc.pool, desc.fvf);
    return S_OK;
  }
};

/* ── IndexBuffer ────────────────────────────────────────────────────────────
 */

class D3D9IndexBufferImpl final : public IDirect3DIndexBuffer9 {
  ULONG refs_ = 1;
  D9CBuffer *b_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  bool defaultPoolTracked_ = false;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9IndexBufferImpl(D9CBuffer *b, IDirect3DDevice9 *device,
                      D3D9PeRecorderFlush *recorder = nullptr)
      : b_(b), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             bufferIsDefaultPool(b_));
  }
  ~D3D9IndexBufferImpl() {
    untrackDefaultPoolResource(recorder_, defaultPoolTracked_);
    dxmt9c_buffer_release(b_);
    if (device_)
      device_->Release();
  }

  D9CBuffer *raw() const { return b_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DResource9) ||
        IsEqualGUID(riid, IID_IDirect3DIndexBuffer9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data,
                                           DWORD size,
                                           DWORD flags) noexcept override {
    return setPrivateData(privateData_, guid, data, size, flags, "ib", this);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data,
                                           DWORD *size) noexcept override {
    return getPrivateData(privateData_, guid, data, size, "ib", this);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) noexcept override {
    return freePrivateData(privateData_, guid, "ib", this);
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) noexcept override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override { return 0; }
  void STDMETHODCALLTYPE PreLoad() noexcept override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() noexcept override {
    return D3DRTYPE_INDEXBUFFER;
  }
  HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void **pp,
                                 DWORD flags) noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    return hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
  }
  HRESULT STDMETHODCALLTYPE Unlock() noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    return hr32(dxmt9c_buffer_unlock(b_));
  }
  HRESULT STDMETHODCALLTYPE
  GetDesc(D3DINDEXBUFFER_DESC *pDesc) noexcept override {
    if (!pDesc)
      return D3DERR_INVALIDCALL;
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
    dxmt9DeviceDebugLog(
        "ib_get_desc ib=%p -> size=%u usage=0x%x pool=%u fmt=%u", this,
        desc.size, desc.usage, desc.pool, desc.format);
    return S_OK;
  }
};

/* ── VertexShader ───────────────────────────────────────────────────────────
 */

class D3D9VertexShaderImpl final : public IDirect3DVertexShader9 {
  ULONG refs_ = 1;
  D9CShader *s_;
  IDirect3DDevice9 *device_;

public:
  D3D9VertexShaderImpl(D9CShader *s, IDirect3DDevice9 *device)
      : s_(s), device_(device) {
    if (device_)
      device_->AddRef();
  }
  ~D3D9VertexShaderImpl() {
    dxmt9c_shader_release(s_);
    if (device_)
      device_->Release();
  }

  D9CShader *raw() const { return s_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DVertexShader9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFunction(void *pData,
                                        UINT *pSize) noexcept override {
    if (!pSize)
      return D3DERR_INVALIDCALL;
    return hr32(dxmt9c_shader_get_bytecode(s_, pData, pSize));
  }
};

/* ── PixelShader ────────────────────────────────────────────────────────────
 */

class D3D9PixelShaderImpl final : public IDirect3DPixelShader9 {
  ULONG refs_ = 1;
  D9CShader *s_;
  IDirect3DDevice9 *device_;

public:
  D3D9PixelShaderImpl(D9CShader *s, IDirect3DDevice9 *device)
      : s_(s), device_(device) {
    if (device_)
      device_->AddRef();
  }
  ~D3D9PixelShaderImpl() {
    dxmt9c_shader_release(s_);
    if (device_)
      device_->Release();
  }

  D9CShader *raw() const { return s_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DPixelShader9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFunction(void *pData,
                                        UINT *pSize) noexcept override {
    if (!pSize)
      return D3DERR_INVALIDCALL;
    return hr32(dxmt9c_shader_get_bytecode(s_, pData, pSize));
  }
};

/* ── VertexDeclaration ──────────────────────────────────────────────────────
 */

class D3D9VertexDeclImpl final : public IDirect3DVertexDeclaration9 {
  ULONG refs_ = 1;
  D9CVertexDecl *d_;
  IDirect3DDevice9 *device_;

public:
  D3D9VertexDeclImpl(D9CVertexDecl *d, IDirect3DDevice9 *device)
      : d_(d), device_(device) {
    if (device_)
      device_->AddRef();
  }
  ~D3D9VertexDeclImpl() {
    dxmt9c_vdecl_release(d_);
    if (device_)
      device_->Release();
  }

  D9CVertexDecl *raw() const { return d_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DVertexDeclaration9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *pE,
                                           UINT *pCount) noexcept override {
    if (!pCount)
      return D3DERR_INVALIDCALL;
    D9CVertexElement tmp[64]{};
    HRESULT hr = hr32(dxmt9c_vdecl_get_declaration(d_, tmp, pCount));
    if (SUCCEEDED(hr) && pE) {
      for (UINT i = 0; i < *pCount; ++i) {
        pE[i].Stream = tmp[i].stream;
        pE[i].Offset = tmp[i].offset;
        pE[i].Type = tmp[i].type;
        pE[i].Method = tmp[i].method;
        pE[i].Usage = tmp[i].usage;
        pE[i].UsageIndex = tmp[i].usageIndex;
      }
    }
    return hr;
  }
};

/* ── Query ──────────────────────────────────────────────────────────────────
 */

class D3D9QueryImpl final : public IDirect3DQuery9 {
  ULONG refs_ = 1;
  D9CQuery *q_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;

public:
  D3D9QueryImpl(D9CQuery *q, IDirect3DDevice9 *device,
                D3D9PeRecorderFlush *recorder = nullptr)
      : q_(q), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
  }
  ~D3D9QueryImpl() {
    dxmt9c_query_release(q_);
    if (device_)
      device_->Release();
  }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DQuery9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() noexcept override {
    return (D3DQUERYTYPE)dxmt9c_query_get_type(q_);
  }
  DWORD STDMETHODCALLTYPE GetDataSize() noexcept override {
    return dxmt9c_query_get_data_size(q_);
  }
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
    if (FAILED(flushHr))
      return flushHr;
    return hr32(dxmt9c_query_issue(q_, flags));
  }
  HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD size,
                                    DWORD flags) noexcept override {
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    return hr32(dxmt9c_query_get_data(q_, pData, size, flags));
  }
};

/* ── StateBlock ─────────────────────────────────────────────────────────────
 */

class D3D9StateBlockImpl final : public IDirect3DStateBlock9 {
  std::atomic<ULONG> refs_{1};
  D9CStateBlock *sb_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;

public:
  D3D9StateBlockImpl(D9CStateBlock *sb, IDirect3DDevice9 *device,
                     D3D9PeRecorderFlush *recorder = nullptr)
      : sb_(sb), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    dxmt9DeviceDebugLog("stateblock_ctor this=%p sb=%p device=%p refs=%u", this,
                        static_cast<void *>(sb_), static_cast<void *>(device_),
                        (unsigned)refs_.load());
  }
  ~D3D9StateBlockImpl() {
    dxmt9DeviceDebugLog("stateblock_dtor this=%p sb=%p device=%p leak=%u", this,
                        static_cast<void *>(sb_), static_cast<void *>(device_),
                        dxmt9LeakStateBlocksEnabled() ? 1u : 0u);
    if (sb_ && !dxmt9LeakStateBlocksEnabled()) {
      dxmt9c_stateblock_release(sb_);
    }
    sb_ = nullptr;
    if (device_)
      device_->Release();
    device_ = nullptr;
  }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override {
    const ULONG refs = refs_.fetch_add(1) + 1;
    dxmt9DeviceDebugLog("stateblock_addref this=%p sb=%p refs=%u", this,
                        static_cast<void *>(sb_), (unsigned)refs);
    return refs;
  }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    const ULONG refs = refs_.fetch_sub(1) - 1;
    dxmt9DeviceDebugLog("stateblock_release this=%p sb=%p refs=%u", this,
                        static_cast<void *>(sb_), (unsigned)refs);
    if (!refs)
      delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DStateBlock9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
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
    if (FAILED(flushHr))
      return flushHr;
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
    if (FAILED(flushHr))
      return flushHr;
    const HRESULT hr = hr32(dxmt9c_stateblock_apply(sb_));
    if (SUCCEEDED(hr) && recorder_) {
      recorder_->InvalidateStateBlockShadowForChild();
    }
    dxmt9DeviceDebugLog("stateblock_apply -> hr=0x%08x", (unsigned)hr);
    return hr;
  }
};

/* ── SwapChain ──────────────────────────────────────────────────────────────
 */

class D3D9SwapChainImpl final : public IDirect3DSwapChain9Ex {
  ULONG refs_ = 1;
  D9CSwapChain *sc_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  bool extended_ = false;

public:
  D3D9SwapChainImpl(D9CSwapChain *sc, IDirect3DDevice9 *device,
                    D3D9PeRecorderFlush *recorder = nullptr,
                    bool extended = false)
      : sc_(sc), device_(device), recorder_(recorder), extended_(extended) {
    if (device_)
      device_->AddRef();
  }
  ~D3D9SwapChainImpl() {
    dxmt9c_swapchain_release(sc_);
    if (device_)
      device_->Release();
  }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DSwapChain9)) {
      *ppv = static_cast<IDirect3DSwapChain9 *>(this);
      AddRef();
      return S_OK;
    }
    if (IsEqualGUID(riid, IID_IDirect3DSwapChain9Ex)) {
      if (!extended_) {
        *ppv = nullptr;
        return E_NOINTERFACE;
      }
      *ppv = static_cast<IDirect3DSwapChain9Ex *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Present(const RECT *src, const RECT *dst, HWND wnd,
                                    const RGNDATA *dirty,
                                    DWORD flags) noexcept override {
    dxmt9DeviceDebugLog(
        "swapchain_present sc=%p wnd=%p flags=0x%x src=%s dst=%s dirty=%p",
        this, wnd, (unsigned)flags, src ? "<custom>" : "<full>",
        dst ? "<custom>" : "<full>", dirty);
    D9CRect cs{}, cd{};
    if (src)
      cs = toR(*src);
    if (dst)
      cd = toR(*dst);
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    return hr32(
        dxmt9c_swapchain_present(sc_, src ? &cs : nullptr, dst ? &cd : nullptr,
                                 (uint64_t)(uintptr_t)wnd, dirty, flags));
  }
  HRESULT STDMETHODCALLTYPE
  GetFrontBufferData(IDirect3DSurface9 *) noexcept override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetBackBuffer(
      UINT idx, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppS) noexcept override {
    if (!ppS)
      return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    dxmt9DeviceDebugLog("swapchain_get_back_buffer sc=%p idx=%u", this, idx);
    D9CSurface *s = dxmt9c_swapchain_get_back_buffer(sc_, idx, 0);
    if (!s)
      return D3DERR_INVALIDCALL;
    *ppS = new D3D9SurfaceImpl(
        s, device_, static_cast<IDirect3DSwapChain9 *>(this), recorder_, false);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetRasterStatus(D3DRASTER_STATUS *p) noexcept override {
    if (p)
      memset(p, 0, sizeof(*p));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetDisplayMode(D3DDISPLAYMODE *p) noexcept override {
    if (!p)
      return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("swapchain_get_display_mode sc=%p", this);
    D9CPresentParams cpp{};
    const HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(sc_, &cpp));
    if (FAILED(hr)) {
      dxmt9DeviceDebugLog("swapchain_get_display_mode -> hr=0x%08x",
                          (unsigned)hr);
      return hr;
    }
    p->Width = cpp.backBufferWidth;
    p->Height = cpp.backBufferHeight;
    p->RefreshRate = cpp.fullScreenRefreshRateHz;
    p->Format = exposeAdapterDisplayFormat(
        static_cast<D3DFORMAT>(cpp.backBufferFormat));
    dxmt9DeviceDebugLog("swapchain_get_display_mode -> %ux%u fmt=%u hz=%u",
                        p->Width, p->Height, (unsigned)p->Format,
                        p->RefreshRate);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetPresentParameters(D3DPRESENT_PARAMETERS *pPP) noexcept override {
    if (!pPP)
      return D3DERR_INVALIDCALL;
    D9CPresentParams cpp{};
    HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(sc_, &cpp));
    if (SUCCEEDED(hr)) {
      pPP->BackBufferWidth = cpp.backBufferWidth;
      pPP->BackBufferHeight = cpp.backBufferHeight;
      pPP->BackBufferFormat = (D3DFORMAT)cpp.backBufferFormat;
      pPP->BackBufferCount = cpp.backBufferCount;
      pPP->MultiSampleType = (D3DMULTISAMPLE_TYPE)cpp.multiSampleType;
      pPP->MultiSampleQuality = cpp.multiSampleQuality;
      pPP->SwapEffect = (D3DSWAPEFFECT)cpp.swapEffect;
      pPP->hDeviceWindow = (HWND)(uintptr_t)cpp.deviceWindow;
      pPP->Windowed = cpp.windowed ? TRUE : FALSE;
      pPP->EnableAutoDepthStencil = cpp.enableAutoDepthStencil ? TRUE : FALSE;
      pPP->AutoDepthStencilFormat = (D3DFORMAT)cpp.autoDepthStencilFormat;
      pPP->Flags = cpp.flags;
      pPP->FullScreen_RefreshRateInHz = cpp.fullScreenRefreshRateHz;
      pPP->PresentationInterval = cpp.presentationInterval;
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  GetLastPresentCount(UINT *pLastPresentCount) noexcept override {
    if (pLastPresentCount)
      *pLastPresentCount = 0u;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics) noexcept override {
    if (pPresentationStatistics) {
      memset(pPresentationStatistics, 0, sizeof(*pPresentationStatistics));
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetDisplayModeEx(D3DDISPLAYMODEEX *pMode,
                   D3DDISPLAYROTATION *pRotation) noexcept override {
    if (!pMode)
      return D3DERR_INVALIDCALL;
    if (pMode->Size != sizeof(D3DDISPLAYMODEEX))
      return D3DERR_INVALIDCALL;
    D3DDISPLAYMODE mode{};
    const HRESULT hr = GetDisplayMode(&mode);
    if (FAILED(hr))
      return hr;
    pMode->Size = sizeof(D3DDISPLAYMODEEX);
    pMode->Width = mode.Width;
    pMode->Height = mode.Height;
    pMode->RefreshRate = mode.RefreshRate;
    pMode->Format = mode.Format;
    pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    if (pRotation)
      *pRotation = D3DDISPLAYROTATION_IDENTITY;
    return S_OK;
  }
};

/* =========================================================================
 * Raw-handle extractors — safe because only our device creates these objects.
 * ========================================================================= */

IDirect3DSurface9 *CreatePeSurface(D9CSurface *surface,
                                   IDirect3DDevice9 *device,
                                   IUnknown *container,
                                   D3D9PeRecorderFlush *recorder,
                                   bool trackDefaultPool) {
  return new D3D9SurfaceImpl(surface, device, container, recorder,
                             trackDefaultPool);
}

IDirect3DTexture9 *CreatePeTexture(D9CTexture *texture,
                                   IDirect3DDevice9 *device,
                                   D3D9PeRecorderFlush *recorder) {
  return new D3D9TextureImpl(texture, device, recorder);
}

IDirect3DVolumeTexture9 *CreatePeVolumeTexture(D9CTexture *texture,
                                               IDirect3DDevice9 *device,
                                               D3D9PeRecorderFlush *recorder) {
  return new D3D9VolumeTextureImpl(texture, device, recorder);
}

IDirect3DCubeTexture9 *CreatePeCubeTexture(D9CTexture *texture,
                                           IDirect3DDevice9 *device,
                                           D3D9PeRecorderFlush *recorder) {
  return new D3D9CubeTextureImpl(texture, device, recorder);
}

IDirect3DVertexBuffer9 *CreatePeVertexBuffer(D9CBuffer *buffer,
                                             IDirect3DDevice9 *device,
                                             D3D9PeRecorderFlush *recorder) {
  return new D3D9VertexBufferImpl(buffer, device, recorder);
}

IDirect3DIndexBuffer9 *CreatePeIndexBuffer(D9CBuffer *buffer,
                                           IDirect3DDevice9 *device,
                                           D3D9PeRecorderFlush *recorder) {
  return new D3D9IndexBufferImpl(buffer, device, recorder);
}

IDirect3DVertexShader9 *CreatePeVertexShader(D9CShader *shader,
                                             IDirect3DDevice9 *device) {
  return new D3D9VertexShaderImpl(shader, device);
}

IDirect3DPixelShader9 *CreatePePixelShader(D9CShader *shader,
                                           IDirect3DDevice9 *device) {
  return new D3D9PixelShaderImpl(shader, device);
}

IDirect3DVertexDeclaration9 *CreatePeVertexDecl(D9CVertexDecl *decl,
                                                IDirect3DDevice9 *device) {
  return new D3D9VertexDeclImpl(decl, device);
}

IDirect3DQuery9 *CreatePeQuery(D9CQuery *query, IDirect3DDevice9 *device,
                               D3D9PeRecorderFlush *recorder) {
  return new D3D9QueryImpl(query, device, recorder);
}

IDirect3DStateBlock9 *CreatePeStateBlock(D9CStateBlock *stateBlock,
                                         IDirect3DDevice9 *device,
                                         D3D9PeRecorderFlush *recorder) {
  return new D3D9StateBlockImpl(stateBlock, device, recorder);
}

IDirect3DSwapChain9Ex *CreatePeSwapChain(D9CSwapChain *swapChain,
                                         IDirect3DDevice9 *device,
                                         D3D9PeRecorderFlush *recorder,
                                         bool extended) {
  return new D3D9SwapChainImpl(swapChain, device, recorder, extended);
}

D9CSurface *D3D9PeRawSurface(IDirect3DSurface9 *surface) {
  return surface ? static_cast<D3D9SurfaceImpl *>(surface)->raw() : nullptr;
}

D9CTexture *D3D9PeRawTexture(IDirect3DBaseTexture9 *texture) {
  if (!texture)
    return nullptr;
  switch (texture->GetType()) {
  case D3DRTYPE_TEXTURE:
    return static_cast<D3D9TextureImpl *>(texture)->raw();
  case D3DRTYPE_CUBETEXTURE:
    return static_cast<D3D9CubeTextureImpl *>(texture)->raw();
  case D3DRTYPE_VOLUMETEXTURE:
    return static_cast<D3D9VolumeTextureImpl *>(texture)->raw();
  default:
    return nullptr;
  }
}

D9CBuffer *D3D9PeRawVertexBuffer(IDirect3DVertexBuffer9 *buffer) {
  return buffer ? static_cast<D3D9VertexBufferImpl *>(buffer)->raw() : nullptr;
}

D9CBuffer *D3D9PeRawIndexBuffer(IDirect3DIndexBuffer9 *buffer) {
  return buffer ? static_cast<D3D9IndexBufferImpl *>(buffer)->raw() : nullptr;
}

D9CShader *D3D9PeRawVertexShader(IDirect3DVertexShader9 *shader) {
  return shader ? static_cast<D3D9VertexShaderImpl *>(shader)->raw() : nullptr;
}

D9CShader *D3D9PeRawPixelShader(IDirect3DPixelShader9 *shader) {
  return shader ? static_cast<D3D9PixelShaderImpl *>(shader)->raw() : nullptr;
}

D9CVertexDecl *D3D9PeRawVertexDecl(IDirect3DVertexDeclaration9 *decl) {
  return decl ? static_cast<D3D9VertexDeclImpl *>(decl)->raw() : nullptr;
}
