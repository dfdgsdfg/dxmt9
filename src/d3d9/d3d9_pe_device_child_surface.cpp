/* src/d3d9/d3d9_pe_device_child_surface.cpp — PE-side child COM wrappers
 * for IDirect3DSurface9, IDirect3DTexture9, IDirect3DCubeTexture9,
 * IDirect3DVolumeTexture9 (and the inner IDirect3DVolume9 helper). */

#include "d3d9_pe_device_child.hpp"

#include "util/com/com_private_data.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdint>

// Wine's d3dkmthk.h is not in the llvm-mingw SDK; inline the minimum
// surface needed for the D3DKMTCreateDCFromMemory call path used by
// `ex_user_memory_getdc_dib_identity`. Layout matches Wine's
// `include/ddk/d3dkmthk.h`.
extern "C" {
  typedef enum _D3DDDIFORMAT { D3DDDIFMT_A8R8G8B8 = 21 } D3DDDIFORMAT;
  typedef struct _D3DKMT_CREATEDCFROMMEMORY {
    void *pMemory;
    D3DDDIFORMAT Format;
    UINT Width;
    UINT Height;
    UINT Pitch;
    HDC hDeviceDc;
    PALETTEENTRY *pColorTable;
    HDC hDc;
    HANDLE hBitmap;
  } D3DKMT_CREATEDCFROMMEMORY;
  typedef struct _D3DKMT_DESTROYDCFROMMEMORY {
    HDC hDc;
    HANDLE hBitmap;
  } D3DKMT_DESTROYDCFROMMEMORY;
}

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

static void dxmt9DeviceDebugLog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-device", fmt, args);
  va_end(args);
}

[[nodiscard]] static HRESULT setPrivateData(dxmt9::util::ComPrivateData &storage,
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

[[nodiscard]] static HRESULT getPrivateData(dxmt9::util::ComPrivateData &storage,
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

[[nodiscard]] static HRESULT freePrivateData(dxmt9::util::ComPrivateData &storage,
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

[[nodiscard]] static HRESULT flushChildRecorder(D3D9PeRecorderFlush *recorder) {
  return recorder ? recorder->FlushPeRecorderForChild() : S_OK;
}

[[nodiscard]] static HRESULT textureLevelDesc(D9CTexture *texture, UINT level,
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
  dxmt9c_texture_set_lod(texture, lod);
  return previous;
}

[[nodiscard]] static HRESULT setAutoGenFilter(D3DTEXTUREFILTERTYPE &filter,
                                D3DTEXTUREFILTERTYPE value) {
  if (value == D3DTEXF_NONE) {
    return D3DERR_INVALIDCALL;
  }
  filter = value;
  return S_OK;
}

// Wine d3d9 conformance: rect/box bounds check shared by surface and
// texture/cube/volume Lock paths. Returns false when the rect is
// degenerate (right<=left, bottom<=top), has negative origins, or
// exceeds the surface dimensions. Matches wined3d's
// wined3d_resource_check_box_dimensions for the 2D case.
static bool rectWithinExtents(const RECT *r, UINT width, UINT height) {
  if (!r)
    return true;
  if (r->left < 0 || r->top < 0)
    return false;
  if (r->right <= r->left || r->bottom <= r->top)
    return false;
  if (static_cast<UINT>(r->right) > width ||
      static_cast<UINT>(r->bottom) > height) {
    return false;
  }
  return true;
}

static bool boxWithinExtents(const D3DBOX *b, UINT width, UINT height,
                             UINT depth) {
  if (!b)
    return true;
  if (b->Right <= b->Left || b->Bottom <= b->Top || b->Back <= b->Front)
    return false;
  if (b->Right > width || b->Bottom > height || b->Back > depth)
    return false;
  return true;
}

// D3DLOCK_DISCARD is only valid on textures created with D3DUSAGE_DYNAMIC.
static bool lockDiscardIsValid(DWORD flags, DWORD usage) {
  if ((flags & D3DLOCK_DISCARD) == 0)
    return true;
  return (usage & D3DUSAGE_DYNAMIC) != 0;
}

// Wine volume_block_lock_layout: block-compressed (DXT*/ATI*) volume
// textures require the lock box to be 4-pixel aligned on Left/Top.
// Non-aligned boxes return D3DERR_INVALIDCALL before any backend work.
static bool formatIsBlockCompressed(uint32_t fmt) {
  switch (fmt) {
  case D3DFMT_DXT1:
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return true;
  default:
    return false;
  }
}

// Bytes-per-pixel for the small set of formats the conformance lock-box
// tests exercise. Returns 0 for unknown / block-compressed formats — the
// caller must skip the offset adjustment in that case.
static uint32_t formatBytesPerPixel(uint32_t fmt) {
  switch (fmt) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8B8G8R8:
  case D3DFMT_X8B8G8R8:
    return 4;
  case D3DFMT_R8G8B8:
    return 3;
  case D3DFMT_A1R5G5B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_R5G6B5:
  case D3DFMT_A4R4G4B4:
  case D3DFMT_X4R4G4B4:
  case D3DFMT_A8L8:
  case D3DFMT_V8U8:
    return 2;
  case D3DFMT_A8:
  case D3DFMT_L8:
  case D3DFMT_P8:
    return 1;
  default:
    return 0;
  }
}

[[nodiscard]] static HRESULT lockTextureBox(D9CTexture *texture, UINT level,
                              D3DLOCKED_BOX *locked, const D3DBOX *box,
                              DWORD flags, D3D9PeRecorderFlush *recorder) {
  if (!locked)
    return D3DERR_INVALIDCALL;
  // Reject misaligned boxes on block-compressed volumes BEFORE the
  // recorder flush so a malformed Lock never commits pending records.
  D9CSurfaceDesc preDesc{};
  const bool descOk =
      SUCCEEDED(textureLevelDesc(texture, level, &preDesc));
  if (box && descOk && formatIsBlockCompressed(preDesc.format)) {
    if ((box->Left & 3u) != 0u || (box->Top & 3u) != 0u
        || ((box->Right & 3u) != 0u && box->Right != preDesc.width)
        || ((box->Bottom & 3u) != 0u && box->Bottom != preDesc.height)) {
      return D3DERR_INVALIDCALL;
    }
  }
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

  // Wine D3D9 contract (check_volume_lock_policy line 660): SlicePitch
  // reflects the full slice stride of the underlying volume level, not
  // the locked sub-box height. Take the level desc as the source of
  // truth and fall back to the box height only if the desc lookup
  // fails.
  D9CSurfaceDesc desc{};
  UINT height = 1;
  uint32_t format = preDesc.format;
  if (SUCCEEDED(textureLevelDesc(texture, level, &desc))) {
    height = std::max<UINT>(1, desc.height);
    format = desc.format;
  } else if (box && box->Bottom > box->Top) {
    height = box->Bottom - box->Top;
  }
  locked->RowPitch = lockedRect.pitch;
  locked->SlicePitch = lockedRect.pitch * static_cast<int>(height);
  locked->pBits = lockedRect.bits;
  // volume_lockbox_bounds_offset_policy: D9CRect only carries 2D
  // (left/top/right/bottom) so the C side already advanced pBits by the
  // top/left offset but cannot apply the Front/Back Z component. Add the
  // Z component here so the final pointer matches the box origin.
  if (box && lockedRect.bits && box->Front != 0) {
    auto *base = static_cast<uint8_t *>(lockedRect.bits);
    locked->pBits = base
        + static_cast<ptrdiff_t>(box->Front) * locked->SlicePitch;
  }
  return S_OK;
}

[[nodiscard]] static HRESULT unlockTextureBox(D9CTexture *texture, UINT level,
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

// Wine d3d9 resource_priority_pool_policy: SetPriority only stores the
// value on D3DPOOL_MANAGED resources; DEFAULT / SYSTEMMEM / SCRATCH ignore
// the new value but still return the previous shadow.
static bool surfacePriorityWriteable(D9CSurface *surface) {
  if (!surface)
    return false;
  D9CSurfaceDesc desc{};
  return SUCCEEDED(hr32(dxmt9c_surface_get_desc(surface, &desc))) &&
         desc.pool == D3DPOOL_MANAGED;
}

static bool texturePriorityWriteable(D9CTexture *texture) {
  if (!texture)
    return false;
  D9CSurfaceDesc desc{};
  return SUCCEEDED(textureLevelDesc(texture, 0, &desc)) &&
         desc.pool == D3DPOOL_MANAGED;
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
  // ex_user_memory_getdc_dib_identity: for user-memory-aliased surfaces,
  // GetDC must return a DC whose selected bitmap is a DIB section whose
  // dsBm.bmBits literally equals the caller's data pointer. Wine's
  // gdi32 exposes `D3DKMTCreateDCFromMemory` (forwards to
  // win32u!NtGdiDdDDICreateDCFromMemory) which builds exactly such a
  // DC/bitmap pair. The Win32-public CreateDIBSection path always
  // allocates fresh bits and cannot satisfy the identity assertion.
  HBITMAP dibBitmap_ = nullptr;
  HDC kmtDc_ = nullptr;
  bool defaultPoolTracked_ = false;
  // Wine d3d9 conformance: PE-side lock-state mirror used to enforce
  // double-Lock / Unlock-without-Lock invariants before the C-side
  // recorder flush. Distinct from the C-side authority but kept in
  // sync via the C-side return values.
  bool locked_ = false;
  // Wine d3d9 conformance: surfaces obtained through
  // IDirect3DTexture9::GetSurfaceLevel share their lock state with the
  // owning texture and accept a redundant Unlock with S_OK (cube and
  // volume containers do not — see test_texture_level_surface_unlock_policy).
  bool ownerIsTexture2D_ = false;
  // T4 (D3D9Ex shared-handle, SYSTEMMEM partial): when non-null this
  // surface aliases caller-owned memory; LockRect short-circuits the
  // bridge path and returns userMemory_ + userMemoryPitch_ directly.
  void *userMemory_ = nullptr;
  int32_t userMemoryPitch_ = 0;
  DWORD priorityShadow_ = 0;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9SurfaceImpl(D9CSurface *s, IDirect3DDevice9 *device, IUnknown *container,
                  D3D9PeRecorderFlush *recorder = nullptr,
                  bool trackDefaultPool = true,
                  void *userMemory = nullptr,
                  int32_t userMemoryPitch = 0)
      : s_(s), device_(device), container_(container), recorder_(recorder),
        userMemory_(userMemory), userMemoryPitch_(userMemoryPitch) {
    if (device_)
      device_->AddRef();
    if (container_)
      container_->AddRef();
    if (container_) {
      IDirect3DBaseTexture9 *base = nullptr;
      if (SUCCEEDED(container_->QueryInterface(IID_IDirect3DBaseTexture9,
                                               reinterpret_cast<void **>(&base))) &&
          base) {
        ownerIsTexture2D_ = (base->GetType() == D3DRTYPE_TEXTURE);
        base->Release();
      }
    }
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             trackDefaultPool && surfaceIsDefaultPool(s_));
  }

  bool peLocked() const { return locked_; }

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
  DWORD STDMETHODCALLTYPE SetPriority(DWORD newPriority) noexcept override {
    const DWORD previous = priorityShadow_;
    // Surfaces obtained from a parent texture / cube / volume container are
    // never priority-writeable — the parent owns the priority. Standalone
    // surfaces (CreateOffscreenPlainSurface, CreateRenderTarget) honor the
    // pool rule (only MANAGED stores).
    if (!container_ && surfacePriorityWriteable(s_))
      priorityShadow_ = newPriority;
    return previous;
  }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override {
    return priorityShadow_;
  }
  // stub: Wine returns S_OK; PreLoad is a hint to copy into VRAM, dxmt9 backing
  // is already Metal GPU-resident.
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
      // Wine d3d9: any D3D9Surface always reports D3DRTYPE_SURFACE
      // regardless of the parent texture's resource type.
      // cube_texture_level_surface_policy.
      pD->Type = D3DRTYPE_SURFACE;
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
    // Wine lockable_backbuffer_lock_policy / nonlockable_backbuffer_getdc_policy:
    // a backbuffer surface is only lockable if its owning swap chain was
    // created with D3DPRESENTFLAG_LOCKABLE_BACKBUFFER. Check via the
    // container (the swap-chain wrapper) — non-backbuffer surfaces have
    // either no container or a texture container.
    if (container_) {
      IDirect3DSwapChain9 *containerSwap = nullptr;
      if (SUCCEEDED(container_->QueryInterface(IID_IDirect3DSwapChain9,
                                               reinterpret_cast<void **>(&containerSwap)))
          && containerSwap) {
        D3DPRESENT_PARAMETERS pp{};
        const HRESULT ppHr = containerSwap->GetPresentParameters(&pp);
        containerSwap->Release();
        if (SUCCEEDED(ppHr)
            && (pp.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) == 0) {
          return D3DERR_INVALIDCALL;
        }
      }
    }
    // Wine d3d9 conformance (test_resource_lock_error_policy): reject a
    // double-Lock and an out-of-bounds / inverted / negative rect before
    // the recorder flush. The C-side already tracks lock state but it
    // is reached only AFTER the recorder flush; mirroring the check here
    // keeps a malformed Lock from committing pending records.
    if (locked_)
      return D3DERR_INVALIDCALL;
    if (pRect) {
      D9CSurfaceDesc desc{};
      if (SUCCEEDED(hr32(dxmt9c_surface_get_desc(s_, &desc)))) {
        if (!rectWithinExtents(pRect, desc.width, desc.height))
          return D3DERR_INVALIDCALL;
      }
    }
    // T4: if this surface aliases caller-owned memory (SYSTEMMEM
    // shared-handle path), short-circuit the bridge. The caller's
    // pointer is the lock target; pRect is ignored (Wine reports the
    // base pBits + format pitch even for partial rects in the
    // user-memory path, see test_user_memory).
    if (userMemory_) {
      pLR->Pitch = userMemoryPitch_;
      pLR->pBits = userMemory_;
      locked_ = true;
      dxmt9DeviceDebugLog(
          "surface_lock_rect (user-memory) surface=%p pitch=%d bits=%p", this,
          userMemoryPitch_, userMemory_);
      return S_OK;
    }
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
      locked_ = true;
      dxmt9DeviceDebugLog("surface_lock_rect -> pitch=%ld bits=%p",
                          (long)pLR->Pitch, pLR->pBits);
    } else {
      dxmt9DeviceDebugLog("surface_lock_rect -> hr=0x%08x", (unsigned)hr);
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect() noexcept override {
    dxmt9DeviceDebugLog("surface_unlock_rect surface=%p", this);
    // T4: user-memory aliasing has no GPU staging to flush; treat the
    // lock as a pure CPU op so unlock is a no-op success.
    if (userMemory_) {
      if (!locked_)
        return ownerIsTexture2D_ ? S_OK : D3DERR_INVALIDCALL;
      locked_ = false;
      return S_OK;
    }
    // Wine d3d9 conformance: Unlock-without-Lock returns INVALIDCALL
    // for standalone and cube-derived surfaces (the C side enforces
    // this for both PE-tracked and texture-shared cases). Doing the
    // check here too skips a wasteful recorder flush on the failing
    // call. 2D-texture-derived surfaces forward unconditionally
    // because Wine treats redundant level-Unlock as idempotent and we
    // must keep the C-side lockedLevels in sync if the parent texture
    // performed the Lock.
    if (!locked_ && !ownerIsTexture2D_) {
      return D3DERR_INVALIDCALL;
    }
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return flushHr;
    HRESULT hr = hr32(dxmt9c_surface_unlock_rect(s_));
    // Wine d3d9 conformance: 2D-texture-derived surfaces accept a
    // redundant Unlock with S_OK (wined3d treats the per-level Unlock
    // as idempotent on mipmap chains). Standalone and cube-derived
    // surfaces propagate INVALIDCALL. See
    // test_texture_level_surface_unlock_policy lines 1166 vs 1191.
    if (hr == D3DERR_INVALIDCALL && ownerIsTexture2D_) {
      hr = S_OK;
    }
    if (SUCCEEDED(hr))
      locked_ = false;
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) noexcept override {
    dxmt9DeviceDebugLog("surface_get_dc surface=%p phdc=%p", this, phdc);
    if (!phdc)
      return D3DERR_INVALIDCALL;
    // nonlockable_backbuffer_getdc_policy: GetDC on a non-lockable
    // backbuffer must return INVALIDCALL AND leave *phdc untouched. Check
    // the swap chain's present flags before any other work.
    if (container_) {
      IDirect3DSwapChain9 *containerSwap = nullptr;
      if (SUCCEEDED(container_->QueryInterface(IID_IDirect3DSwapChain9,
                                               reinterpret_cast<void **>(&containerSwap)))
          && containerSwap) {
        D3DPRESENT_PARAMETERS pp{};
        const HRESULT ppHr = containerSwap->GetPresentParameters(&pp);
        containerSwap->Release();
        if (SUCCEEDED(ppHr)
            && (pp.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) == 0) {
          // Leave *phdc unchanged.
          return D3DERR_INVALIDCALL;
        }
      }
    }
    if (dc_)
      return D3DERR_INVALIDCALL;
    // ex_user_memory_getdc_dib_identity: when this surface aliases a
    // caller-supplied user-memory buffer (D3D9Ex SYSTEMMEM
    // shared-handle path), use D3DKMTCreateDCFromMemory — Wine's
    // gdi32 entry point that builds a DC + DIB-section pair whose
    // bmBits is the supplied pMemory pointer. This is exactly the
    // mechanism Wine's wined3d uses for the same case.
    if (userMemory_) {
      D9CSurfaceDesc desc{};
      if (SUCCEEDED(hr32(dxmt9c_surface_get_desc(s_, &desc)))
          && desc.width != 0 && desc.height != 0) {
        const uint32_t bpp = formatBytesPerPixel(desc.format);
        if (bpp != 0) {
          using PFN_D3DKMTCreateDCFromMemory =
              LONG (WINAPI *)(D3DKMT_CREATEDCFROMMEMORY *);
          static auto pCreate = []() -> PFN_D3DKMTCreateDCFromMemory {
            HMODULE gdi = GetModuleHandleA("gdi32.dll");
            if (!gdi) gdi = LoadLibraryA("gdi32.dll");
            if (!gdi) return nullptr;
            return reinterpret_cast<PFN_D3DKMTCreateDCFromMemory>(
                reinterpret_cast<void *>(
                    GetProcAddress(gdi, "D3DKMTCreateDCFromMemory")));
          }();
          if (pCreate) {
            D3DKMT_CREATEDCFROMMEMORY req{};
            req.pMemory = userMemory_;
            // D3D9 D3DFMT_A8R8G8B8 maps to D3DDDIFMT_A8R8G8B8 (value 21).
            req.Format = static_cast<D3DDDIFORMAT>(desc.format);
            req.Width = desc.width;
            req.Height = desc.height;
            req.Pitch = static_cast<UINT>(userMemoryPitch_ != 0
                ? userMemoryPitch_
                : static_cast<int32_t>(desc.width * bpp));
            HDC deviceDc = ::GetDC(nullptr);  // screen DC as device hint
            req.hDeviceDc = deviceDc;
            if (pCreate(&req) == 0 /* STATUS_SUCCESS */) {
              kmtDc_ = req.hDc;
              dibBitmap_ = static_cast<HBITMAP>(req.hBitmap);
              dc_ = kmtDc_;
            }
            if (deviceDc) ::ReleaseDC(nullptr, deviceDc);
          }
        }
      }
    }
    if (!dc_) {
      dc_ = CreateCompatibleDC(nullptr);
      if (!dc_)
        return E_FAIL;
    }
    *phdc = dc_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) noexcept override {
    dxmt9DeviceDebugLog("surface_release_dc surface=%p hdc=%p", this, hdc);
    if (!dc_ || hdc != dc_)
      return D3DERR_INVALIDCALL;
    if (kmtDc_) {
      using PFN_D3DKMTDestroyDCFromMemory =
          LONG (WINAPI *)(D3DKMT_DESTROYDCFROMMEMORY *);
      static auto pDestroy = []() -> PFN_D3DKMTDestroyDCFromMemory {
        HMODULE gdi = GetModuleHandleA("gdi32.dll");
        if (!gdi) return nullptr;
        return reinterpret_cast<PFN_D3DKMTDestroyDCFromMemory>(
            reinterpret_cast<void *>(
                GetProcAddress(gdi, "D3DKMTDestroyDCFromMemory")));
      }();
      if (pDestroy) {
        D3DKMT_DESTROYDCFROMMEMORY req{};
        req.hDc = kmtDc_;
        req.hBitmap = dibBitmap_;
        pDestroy(&req);
      }
      kmtDc_ = nullptr;
      dibBitmap_ = nullptr;
      dc_ = nullptr;
      return S_OK;
    }
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
  // T4 (D3D9Ex shared-handle, SYSTEMMEM partial): when non-null this
  // texture aliases caller-owned memory; LockRect (level 0) returns the
  // user pointer directly. Only level 0 is supported here because the
  // partial scope restricts SYSTEMMEM-shared textures to levels == 1.
  void *userMemory_ = nullptr;
  int32_t userMemoryPitch_ = 0;
  DWORD priorityShadow_ = 0;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9TextureImpl(D9CTexture *t, IDirect3DDevice9 *device,
                  D3D9PeRecorderFlush *recorder = nullptr,
                  void *userMemory = nullptr,
                  int32_t userMemoryPitch = 0)
      : t_(t), device_(device), recorder_(recorder),
        userMemory_(userMemory), userMemoryPitch_(userMemoryPitch) {
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
  DWORD STDMETHODCALLTYPE SetPriority(DWORD newPriority) noexcept override {
    const DWORD previous = priorityShadow_;
    if (texturePriorityWriteable(t_))
      priorityShadow_ = newPriority;
    return previous;
  }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override {
    return priorityShadow_;
  }
  // stub: Wine returns S_OK; PreLoad is a hint to copy into VRAM, dxmt9 backing
  // is already Metal GPU-resident.
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
    // Why: Wine oracle dlls/d3d9/texture.c::d3d9_texture_2d_GenerateMipSubLevels
    // -> d3d9_texture_gen_auto_mipmap only regenerates mips when the texture
    // carries D3DUSAGE_AUTOGENMIPMAP (gated via D3D9_TEXTURE_MIPMAP_DIRTY,
    // which is itself set only on AUTOGENMIPMAP textures by
    // d3d9_texture_flag_auto_gen_mipmap). Non-AUTOGENMIPMAP textures are a
    // no-op (Wine emits no WARN here, just returns). The backend
    // dxmt9c_texture_generate_mip_sublevels does not enforce this flag for
    // its internal unlockRect-driven path, so we gate at the COM vtable.
    D9CSurfaceDesc sd{};
    if (FAILED(hr32(dxmt9c_texture_get_level_desc(t_, 0, &sd))))
      return;
    if ((sd.usage & D3DUSAGE_AUTOGENMIPMAP) == 0)
      return;
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return;
    static_cast<void>(dxmt9c_texture_generate_mip_sublevels(t_));
  }
  HRESULT STDMETHODCALLTYPE
  GetLevelDesc(UINT level, D3DSURFACE_DESC *pD) noexcept override {
    if (!pD)
      return D3DERR_INVALIDCALL;
    D9CSurfaceDesc sd{};
    HRESULT hr = hr32(dxmt9c_texture_get_level_desc(t_, level, &sd));
    if (SUCCEEDED(hr)) {
      pD->Format = (D3DFORMAT)sd.format;
      // Wine d3d9: level descriptor is a surface.
      pD->Type = D3DRTYPE_SURFACE;
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
    // Wine d3d9 conformance (test_resource_lock_error_policy /
    // check_texture_lock_policy): reject an out-of-range level, an
    // out-of-bounds / inverted rect, and a D3DLOCK_DISCARD on a
    // non-D3DUSAGE_DYNAMIC texture before reaching the recorder
    // flush. The C-side double-lock check (lockedLevels) remains
    // authoritative for the in-flight state.
    if (level >= dxmt9c_texture_get_level_count(t_))
      return D3DERR_INVALIDCALL;
    {
      D9CSurfaceDesc desc{};
      if (SUCCEEDED(textureLevelDesc(t_, level, &desc))) {
        if (!rectWithinExtents(pRect, desc.width, desc.height))
          return D3DERR_INVALIDCALL;
        if (!lockDiscardIsValid(flags, desc.usage))
          return D3DERR_INVALIDCALL;
      }
    }
    // T4: user-memory aliasing path (SYSTEMMEM shared-handle). The
    // partial scope only creates level-1 textures, so level == 0 is the
    // only valid lock target.
    if (userMemory_ && level == 0) {
      pLR->Pitch = userMemoryPitch_;
      pLR->pBits = userMemory_;
      dxmt9DeviceDebugLog(
          "texture_lock_rect (user-memory) texture=%p pitch=%d bits=%p", this,
          userMemoryPitch_, userMemory_);
      return S_OK;
    }
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
    // Wine d3d9 conformance: Unlock-with-invalid-level returns
    // INVALIDCALL before reaching the recorder flush.
    if (level >= dxmt9c_texture_get_level_count(t_))
      return D3DERR_INVALIDCALL;
    // T4: user-memory aliasing has no staging copy to flush.
    if (userMemory_ && level == 0) {
      return S_OK;
    }
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
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *pRect) noexcept override {
    // visual_add_dirty_rect_policy: AddDirtyRect is only valid for
    // D3DPOOL_MANAGED textures (and SYSTEMMEM-backed Ex-defaultpool). For
    // DEFAULT-pool textures, Wine returns D3DERR_INVALIDCALL.
    D9CSurfaceDesc desc{};
    if (SUCCEEDED(textureLevelDesc(t_, 0, &desc))
        && desc.pool != D3DPOOL_MANAGED
        && desc.pool != D3DPOOL_SYSTEMMEM) {
      return D3DERR_INVALIDCALL;
    }
    // Inverted / empty rect rejection (Wine d3d9: AddDirtyRect validates
    // rect bounds before recording).
    if (pRect) {
      if (pRect->left < 0 || pRect->top < 0
          || pRect->right <= pRect->left
          || pRect->bottom <= pRect->top) {
        return D3DERR_INVALIDCALL;
      }
    }
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
  DWORD priorityShadow_ = 0;
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
  DWORD STDMETHODCALLTYPE SetPriority(DWORD newPriority) noexcept override {
    const DWORD previous = priorityShadow_;
    if (texturePriorityWriteable(t_))
      priorityShadow_ = newPriority;
    return previous;
  }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override {
    return priorityShadow_;
  }
  // stub: Wine returns S_OK; PreLoad is a hint to copy into VRAM, dxmt9 backing
  // is already Metal GPU-resident.
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
    // Why: Wine oracle dlls/d3d9/texture.c::d3d9_texture_cube_GenerateMipSubLevels
    // shares d3d9_texture_gen_auto_mipmap with the 2D path; the dirty-flag
    // gating only fires for D3DUSAGE_AUTOGENMIPMAP textures. Mirror that
    // contract here before forwarding to the C ABI (which is type-agnostic
    // and would otherwise rebuild mips for any cube with mipLevels > 1).
    D9CSurfaceDesc sd{};
    if (FAILED(hr32(dxmt9c_texture_get_level_desc(t_, 0, &sd))))
      return;
    if ((sd.usage & D3DUSAGE_AUTOGENMIPMAP) == 0)
      return;
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return;
    static_cast<void>(dxmt9c_texture_generate_mip_sublevels(t_));
  }
  HRESULT STDMETHODCALLTYPE
  GetLevelDesc(UINT level, D3DSURFACE_DESC *pD) noexcept override {
    if (!pD)
      return D3DERR_INVALIDCALL;
    D9CSurfaceDesc sd{};
    HRESULT hr = hr32(dxmt9c_texture_get_level_desc(t_, level, &sd));
    if (SUCCEEDED(hr)) {
      pD->Format = (D3DFORMAT)sd.format;
      // Wine d3d9: cube level descriptor is a surface.
      // cube_texture_level_surface_policy.
      pD->Type = D3DRTYPE_SURFACE;
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
    pD->Depth = sd.depth;
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
  DWORD priorityShadow_ = 0;
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
  DWORD STDMETHODCALLTYPE SetPriority(DWORD newPriority) noexcept override {
    const DWORD previous = priorityShadow_;
    if (texturePriorityWriteable(t_))
      priorityShadow_ = newPriority;
    return previous;
  }
  DWORD STDMETHODCALLTYPE GetPriority() noexcept override {
    return priorityShadow_;
  }
  // stub: Wine returns S_OK; PreLoad is a hint to copy into VRAM, dxmt9 backing
  // is already Metal GPU-resident.
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
    // Why: Wine oracle dlls/d3d9/texture.c::d3d9_texture_volume_GenerateMipSubLevels
    // shares the same d3d9_texture_gen_auto_mipmap path as the 2D and cube
    // variants. Volume textures honour the D3DUSAGE_AUTOGENMIPMAP gate
    // identically; non-AUTOGENMIPMAP volume textures are a no-op.
    D9CSurfaceDesc sd{};
    if (FAILED(hr32(dxmt9c_texture_get_level_desc(t_, 0, &sd))))
      return;
    if ((sd.usage & D3DUSAGE_AUTOGENMIPMAP) == 0)
      return;
    const HRESULT flushHr = flushChildRecorder(recorder_);
    if (FAILED(flushHr))
      return;
    static_cast<void>(dxmt9c_texture_generate_mip_sublevels(t_));
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
    pD->Depth = sd.depth;
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
    if (!locked)
      return D3DERR_INVALIDCALL;
    if (level >= dxmt9c_texture_get_level_count(t_))
      return D3DERR_INVALIDCALL;
    // Wine d3d9 conformance:
    //  - check_volume_lock_policy: reject inverted/out-of-bounds box
    //    and D3DLOCK_DISCARD on non-dynamic volume.
    //  - volume_block_lock_layout: D3DLOCK_READONLY on DEFAULT pool
    //    volume is rejected with INVALIDCALL.
    {
      D9CSurfaceDesc desc{};
      if (SUCCEEDED(textureLevelDesc(t_, level, &desc))) {
        if (!boxWithinExtents(box, desc.width, desc.height, desc.depth))
          return D3DERR_INVALIDCALL;
        if (!lockDiscardIsValid(flags, desc.usage))
          return D3DERR_INVALIDCALL;
        if ((flags & D3DLOCK_READONLY) &&
            (D3DPOOL)desc.pool == D3DPOOL_DEFAULT)
          return D3DERR_INVALIDCALL;
      }
    }
    return lockTextureBox(t_, level, locked, box, flags, recorder_);
  }
  HRESULT STDMETHODCALLTYPE UnlockBox(UINT level) noexcept override {
    if (level >= dxmt9c_texture_get_level_count(t_))
      return D3DERR_INVALIDCALL;
    return unlockTextureBox(t_, level, recorder_);
  }
  HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX *) noexcept override {
    // stub: Wine returns S_OK; dxmt9 uploads dirty regions via the chunk recorder,
    // AddDirtyBox is an optimization hint.
    return S_OK;
  }
};

/* =========================================================================
 * Public factory + raw-handle extractors for surface/texture family.
 * ========================================================================= */

IDirect3DSurface9 *CreatePeSurface(D9CSurface *surface,
                                   IDirect3DDevice9 *device,
                                   IUnknown *container,
                                   D3D9PeRecorderFlush *recorder,
                                   bool trackDefaultPool,
                                   void *userMemory,
                                   int32_t userMemoryPitch) {
  return new D3D9SurfaceImpl(surface, device, container, recorder,
                             trackDefaultPool, userMemory, userMemoryPitch);
}

IDirect3DTexture9 *CreatePeTexture(D9CTexture *texture,
                                   IDirect3DDevice9 *device,
                                   D3D9PeRecorderFlush *recorder,
                                   void *userMemory,
                                   int32_t userMemoryPitch) {
  return new D3D9TextureImpl(texture, device, recorder, userMemory,
                             userMemoryPitch);
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

D9CSurface *D3D9PeRawSurface(IDirect3DSurface9 *surface) {
  return surface ? static_cast<D3D9SurfaceImpl *>(surface)->raw() : nullptr;
}

bool D3D9PeSurfaceIsLocked(IDirect3DSurface9 *surface) {
  return surface ? static_cast<D3D9SurfaceImpl *>(surface)->peLocked() : false;
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
