/* src/d3d9/d3d9_pe_device_child_buffer.cpp — PE-side child COM wrappers
 * for IDirect3DVertexBuffer9 and IDirect3DIndexBuffer9. */

#include "d3d9_pe_device_child.hpp"

#include "util/com/com_private_data.hpp"
#include "util/log/log.hpp"

#include <cstdarg>
#include <cstdint>

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

static bool bufferLockRequiresHazardFlush(DWORD flags) {
  if ((flags & D3DLOCK_READONLY) != 0)
    return false;
  // DISCARD may skip a GPU wait only when the implementation gives the app a
  // fresh backing allocation and already-recorded draws keep a concrete backing
  // snapshot. The flush keeps PE recorder ordering explicit; the backend rename
  // snapshot preserves the actual Metal buffer seen by each queued draw.
  if ((flags & D3DLOCK_DISCARD) != 0)
    return true;
  if ((flags & D3DLOCK_NOOVERWRITE) != 0)
    return false;
  return true;
}

[[nodiscard]] static HRESULT
flushChildRecorderForBufferLock(D3D9PeRecorderFlush *recorder, D9CBuffer *buffer,
                                DWORD flags) {
  if (!recorder || !bufferLockRequiresHazardFlush(flags))
    return S_OK;
  return recorder->FlushPeRecorderForBufferHazardForChild(buffer);
}

static bool loadBufferDesc(D9CBuffer *buffer, D9CBufferDesc &desc) {
  return buffer && SUCCEEDED(hr32(dxmt9c_buffer_get_desc(buffer, &desc)));
}

static bool bufferIsDefaultPool(const D9CBufferDesc &desc, bool valid) {
  return valid && desc.pool == D3DPOOL_DEFAULT;
}

// Wine d3d9 resource_priority_pool_policy: SetPriority only persists for
// D3DPOOL_MANAGED resources.
static bool bufferPriorityWriteable(const D9CBufferDesc &desc, bool valid) {
  return valid && desc.pool == D3DPOOL_MANAGED;
}

static HRESULT cachedBufferDescOrFetch(D9CBuffer *buffer,
                                        const D9CBufferDesc &cached,
                                        bool cachedValid, D9CBufferDesc &desc) {
  if (cachedValid) {
    desc = cached;
    return S_OK;
  }
  return hr32(dxmt9c_buffer_get_desc(buffer, &desc));
}

static void fillVertexBufferDesc(const D9CBufferDesc &desc,
                                 D3DVERTEXBUFFER_DESC &out) {
  out.Format = D3DFMT_VERTEXDATA;
  out.Type = D3DRTYPE_VERTEXBUFFER;
  out.Usage = desc.usage;
  out.Pool = static_cast<D3DPOOL>(desc.pool);
  out.Size = desc.size;
  out.FVF = desc.fvf;
}

static void fillIndexBufferDesc(const D9CBufferDesc &desc,
                                D3DINDEXBUFFER_DESC &out) {
  out.Format = static_cast<D3DFORMAT>(desc.format);
  out.Type = D3DRTYPE_INDEXBUFFER;
  out.Usage = desc.usage;
  out.Pool = static_cast<D3DPOOL>(desc.pool);
  out.Size = desc.size;
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

/* ── VertexBuffer ───────────────────────────────────────────────────────────
 */

class D3D9VertexBufferImpl final : public IDirect3DVertexBuffer9 {
  ULONG refs_ = 1;
  D9CBuffer *b_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  D9CBufferDesc desc_{};
  bool descValid_ = false;
  bool defaultPoolTracked_ = false;
  bool locked_ = false;
  DWORD lockFlags_ = 0;
  DWORD priorityShadow_ = 0;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9VertexBufferImpl(D9CBuffer *b, IDirect3DDevice9 *device,
                       D3D9PeRecorderFlush *recorder = nullptr)
      : b_(b), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    descValid_ = loadBufferDesc(b_, desc_);
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             bufferIsDefaultPool(desc_, descValid_));
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
  DWORD STDMETHODCALLTYPE SetPriority(DWORD newPriority) noexcept override {
    const DWORD previous = priorityShadow_;
    if (bufferPriorityWriteable(desc_, descValid_))
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
    return D3DRTYPE_VERTEXBUFFER;
  }
  HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void **pp,
                                 DWORD flags) noexcept override {
    if (recorder_)
      recorder_->NotifyPeFirstCallAfterPresentForChild(
          "VertexBuffer::Lock", DXMT9_PE_CALLSITE_PC());
    // Wine d3d9 conformance: validate the argument shape before the
    // recorder flush so a bogus Lock never causes pending work to
    // commit. The four invariants below mirror dlls/d3d9/buffer.c
    // (test_vb_lock_flags / wined3d_resource_check_box_dimensions).
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (locked_)
      return D3DERR_INVALIDCALL;
    {
      D9CBufferDesc desc{};
      if (SUCCEEDED(cachedBufferDescOrFetch(b_, desc_, descValid_, desc))) {
        const UINT bufSize = desc.size;
        // Wine treats size==0 as "lock from off to end of buffer";
        // any non-zero size must fit within [off, bufSize].
        if (off > bufSize)
          return D3DERR_INVALIDCALL;
        if (size != 0 && size > bufSize - off)
          return D3DERR_INVALIDCALL;
      }
    }
    const HRESULT flushHr =
        flushChildRecorderForBufferLock(recorder_, b_, flags);
    if (FAILED(flushHr))
      return flushHr;
    const HRESULT lockHr = hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
    if (SUCCEEDED(lockHr)) {
      locked_ = true;
      lockFlags_ = flags;
    }
    return lockHr;
  }
  HRESULT STDMETHODCALLTYPE Unlock() noexcept override {
    if (!locked_)
      return D3DERR_INVALIDCALL;
    const HRESULT flushHr =
        flushChildRecorderForBufferLock(recorder_, b_, lockFlags_);
    if (FAILED(flushHr))
      return flushHr;
    const HRESULT unlockHr = hr32(dxmt9c_buffer_unlock(b_));
    if (SUCCEEDED(unlockHr)) {
      locked_ = false;
      lockFlags_ = 0;
    }
    return unlockHr;
  }
  HRESULT STDMETHODCALLTYPE
  GetDesc(D3DVERTEXBUFFER_DESC *pDesc) noexcept override {
    const auto peCall = recorder_
        ? recorder_->NotifyPeFirstCallAfterPresentForChild(
              "VertexBuffer::GetDesc", DXMT9_PE_CALLSITE_PC())
        : D3D9PePresentCallToken{};
    const auto finishPeCall = [&](HRESULT hr) noexcept {
      if (recorder_)
        recorder_->NotifyPeCallReturnAfterPresentForChild(
            peCall, "VertexBuffer::GetDesc", hr);
      return hr;
    };
    if (!pDesc)
      return finishPeCall(D3DERR_INVALIDCALL);
    D9CBufferDesc desc{};
    const HRESULT hr = cachedBufferDescOrFetch(b_, desc_, descValid_, desc);
    if (FAILED(hr)) {
      dxmt9DeviceDebugLog("vb_get_desc vb=%p -> hr=0x%08x", this,
                          (unsigned)hr);
      return finishPeCall(hr);
    }
    fillVertexBufferDesc(desc, *pDesc);
    dxmt9DeviceDebugLog(
        "vb_get_desc vb=%p -> size=%u usage=0x%x pool=%u fvf=0x%x", this,
        desc.size, desc.usage, desc.pool, desc.fvf);
    return finishPeCall(S_OK);
  }
};

/* ── IndexBuffer ────────────────────────────────────────────────────────────
 */

class D3D9IndexBufferImpl final : public IDirect3DIndexBuffer9 {
  ULONG refs_ = 1;
  D9CBuffer *b_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  D9CBufferDesc desc_{};
  bool descValid_ = false;
  bool defaultPoolTracked_ = false;
  bool locked_ = false;
  DWORD lockFlags_ = 0;
  DWORD priorityShadow_ = 0;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9IndexBufferImpl(D9CBuffer *b, IDirect3DDevice9 *device,
                      D3D9PeRecorderFlush *recorder = nullptr)
      : b_(b), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    descValid_ = loadBufferDesc(b_, desc_);
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             bufferIsDefaultPool(desc_, descValid_));
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
  DWORD STDMETHODCALLTYPE SetPriority(DWORD newPriority) noexcept override {
    const DWORD previous = priorityShadow_;
    if (bufferPriorityWriteable(desc_, descValid_))
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
    return D3DRTYPE_INDEXBUFFER;
  }
  HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void **pp,
                                 DWORD flags) noexcept override {
    if (recorder_)
      recorder_->NotifyPeFirstCallAfterPresentForChild(
          "IndexBuffer::Lock", DXMT9_PE_CALLSITE_PC());
    const HRESULT flushHr =
        flushChildRecorderForBufferLock(recorder_, b_, flags);
    if (FAILED(flushHr))
      return flushHr;
    const HRESULT lockHr = hr32(dxmt9c_buffer_lock(b_, off, size, pp, flags));
    if (SUCCEEDED(lockHr)) {
      locked_ = true;
      lockFlags_ = flags;
    }
    return lockHr;
  }
  HRESULT STDMETHODCALLTYPE Unlock() noexcept override {
    const HRESULT flushHr =
        locked_ ? flushChildRecorderForBufferLock(recorder_, b_, lockFlags_)
                : S_OK;
    if (FAILED(flushHr))
      return flushHr;
    const HRESULT unlockHr = hr32(dxmt9c_buffer_unlock(b_));
    if (SUCCEEDED(unlockHr)) {
      locked_ = false;
      lockFlags_ = 0;
    }
    return unlockHr;
  }
  HRESULT STDMETHODCALLTYPE
  GetDesc(D3DINDEXBUFFER_DESC *pDesc) noexcept override {
    const auto peCall = recorder_
        ? recorder_->NotifyPeFirstCallAfterPresentForChild(
              "IndexBuffer::GetDesc", DXMT9_PE_CALLSITE_PC())
        : D3D9PePresentCallToken{};
    const auto finishPeCall = [&](HRESULT hr) noexcept {
      if (recorder_)
        recorder_->NotifyPeCallReturnAfterPresentForChild(
            peCall, "IndexBuffer::GetDesc", hr);
      return hr;
    };
    if (!pDesc)
      return finishPeCall(D3DERR_INVALIDCALL);
    D9CBufferDesc desc{};
    const HRESULT hr = cachedBufferDescOrFetch(b_, desc_, descValid_, desc);
    if (FAILED(hr)) {
      dxmt9DeviceDebugLog("ib_get_desc ib=%p -> hr=0x%08x", this,
                          (unsigned)hr);
      return finishPeCall(hr);
    }
    fillIndexBufferDesc(desc, *pDesc);
    dxmt9DeviceDebugLog(
        "ib_get_desc ib=%p -> size=%u usage=0x%x pool=%u fmt=%u", this,
        desc.size, desc.usage, desc.pool, desc.format);
    return finishPeCall(S_OK);
  }
};

/* =========================================================================
 * Public factory + raw-handle extractors for buffer family.
 * ========================================================================= */

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

D9CBuffer *D3D9PeRawVertexBuffer(IDirect3DVertexBuffer9 *buffer) {
  return buffer ? static_cast<D3D9VertexBufferImpl *>(buffer)->raw() : nullptr;
}

D9CBuffer *D3D9PeRawIndexBuffer(IDirect3DIndexBuffer9 *buffer) {
  return buffer ? static_cast<D3D9IndexBufferImpl *>(buffer)->raw() : nullptr;
}
