/* src/d3d9/d3d9_pe_device_child_buffer.cpp — PE-side child COM wrappers
 * for IDirect3DVertexBuffer9 and IDirect3DIndexBuffer9. */

#include "d3d9_pe_device_child.hpp"

#include "d3d9_pe_buffer_readonly_cache.hpp"
#include "d3d9_pe_buffer_hazard.hpp"
#include "util/com/com_private_data.hpp"
#include "util/log/log.hpp"

#include <cstdarg>
#include <cstdint>
#include <span>
#include <vector>

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

[[nodiscard]] static HRESULT
flushChildRecorderForBufferLock(D3D9PeRecorderFlush *recorder, D9CBuffer *buffer,
                                DWORD flags) {
  return dxmt9::d3d9::pe::sealBufferGenerationBeforeLock(
    recorder != nullptr, flags, static_cast<HRESULT>(S_OK),
    [&] { return recorder->FlushPeRecorderForBufferHazardForChild(buffer); });
}

static bool loadBufferDesc(D9CBuffer *buffer, D9CBufferDesc &desc) {
  return buffer && SUCCEEDED(hr32(dxmt9c_buffer_get_desc(buffer, &desc)));
}

static bool copyRenderTapeBufferMutation(D3D9PeRecorderFlush *recorder,
                                         const void *data, std::size_t bytes,
                                         std::vector<std::byte> &copy) noexcept {
  if (!recorder ||
      (!recorder->IsRenderTapeCaptureActiveForChild() &&
       !recorder->IsRenderTapeCaptureTrackingEnabledForChild()))
    return true;
  if (!data || bytes == 0u) {
    recorder->AbortRenderTapeCaptureForChild();
    return false;
  }
  try {
    copy.assign(static_cast<const std::byte *>(data),
                static_cast<const std::byte *>(data) + bytes);
    return true;
  } catch (...) {
    recorder->AbortRenderTapeCaptureForChild();
    return false;
  }
}

static bool bufferIsDefaultPool(const D9CBufferDesc &desc, bool valid) {
  return valid && desc.pool == D3DPOOL_DEFAULT;
}

// Wine d3d9 resource_priority_pool_policy: SetPriority only persists for
// D3DPOOL_MANAGED resources.
static bool bufferPriorityWriteable(const D9CBufferDesc &desc, bool valid) {
  return valid && desc.pool == D3DPOOL_MANAGED;
}

static bool bufferReadonlyCacheEligible(const D9CBufferDesc &desc, bool valid,
                                        DWORD flags) {
  return valid && desc.pool == D3DPOOL_MANAGED &&
         (flags & D3DLOCK_READONLY) != 0;
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

struct D3D9PeBufferLockState {
  bool locked = false;
  bool servedFromReadonlyCache = false;
  DWORD flags = 0;
  void *data = nullptr;
  UINT offset = 0;
  UINT size = 0;
  D3D9PeBufferReadonlyCache readonlyCache{};
};

[[nodiscard]] static HRESULT
validateBufferLockArgs(D9CBuffer *buffer, const D9CBufferDesc &cachedDesc,
                       bool cachedDescValid, bool locked, UINT off, UINT size,
                       void **pp, D9CBufferDesc &desc,
                       bool &descValid) noexcept {
  if (!pp)
    return D3DERR_INVALIDCALL;
  if (locked)
    return D3DERR_INVALIDCALL;
  descValid = false;
  if (SUCCEEDED(cachedBufferDescOrFetch(buffer, cachedDesc, cachedDescValid,
                                       desc))) {
    descValid = true;
    const UINT bufSize = desc.size;
    // Wine treats size==0 as "lock from off to end of buffer"; any non-zero
    // size must fit within [off, bufSize].
    if (off > bufSize)
      return D3DERR_INVALIDCALL;
    if (size != 0 && size > bufSize - off)
      return D3DERR_INVALIDCALL;
  }
  return S_OK;
}

[[nodiscard]] static HRESULT
lockPeBuffer(D9CBuffer *buffer, D3D9PeRecorderFlush *recorder,
             const D9CBufferDesc &cachedDesc, bool cachedDescValid,
             const dxmt9::d3d9::pe::PeWireObjectRef &wireObject,
             D3D9PeBufferLockState &state, UINT off, UINT size, void **pp,
             DWORD flags) noexcept {
  const auto notifyCpuRead = [&]() noexcept {
    if (!recorder || !recorder->IsRenderTapeCaptureActiveForChild() ||
        (flags & D3DLOCK_READONLY) == 0u) {
      return;
    }
    const dxmt9::d3d9::RenderTapeCpuReadControl payload{
        .copyCount = 1u,
        .bytesRead = static_cast<std::uint32_t>(
            std::min<UINT>(state.size, 0xffffffffu)),
    };
    recorder->NotifyRenderTapeOrderedControlForChild(
        dxmt9::d3d9::RenderTapeOrderedControlHeader{
            .identity = wireObject.identity,
            .kind = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeControlKind::CpuRead),
            .disposition = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeControlDisposition::Completed),
            .resultCode = static_cast<std::int32_t>(S_OK),
            .controlBytes = sizeof(payload)},
        std::as_bytes(std::span(&payload, 1u)));
  };
  D9CBufferDesc desc{};
  bool descValid = false;
  const HRESULT validationHr = validateBufferLockArgs(
      buffer, cachedDesc, cachedDescValid, state.locked, off, size, pp, desc,
      descValid);
  if (FAILED(validationHr))
    return validationHr;

  const bool cacheEligible = bufferReadonlyCacheEligible(desc, descValid, flags);
  if (cacheEligible &&
      state.readonlyCache.canServe(off, size, desc.size)) {
    *pp = state.readonlyCache.dataFor(off);
    state.locked = true;
    state.servedFromReadonlyCache = true;
    state.flags = flags;
    state.data = *pp;
    state.offset = off;
    state.size = size != 0u ? size : desc.size - off;
    notifyCpuRead();
    return S_OK;
  }

  const HRESULT flushHr = flushChildRecorderForBufferLock(recorder, buffer, flags);
  if (FAILED(flushHr))
    return flushHr;

  const HRESULT lockHr = hr32(dxmt9c_buffer_lock(buffer, off, size, pp, flags));
  if (SUCCEEDED(lockHr)) {
    state.locked = true;
    state.servedFromReadonlyCache = false;
    state.flags = flags;
    state.data = *pp;
    state.offset = off;
    state.size = size != 0u ? size : desc.size - off;
    if ((flags & D3DLOCK_READONLY) == 0)
      state.readonlyCache.invalidate();
    if (cacheEligible && *pp &&
        state.readonlyCache.refresh(off, size, desc.size, *pp)) {
      const HRESULT unlockHr = hr32(dxmt9c_buffer_unlock(buffer));
      if (SUCCEEDED(unlockHr)) {
        *pp = state.readonlyCache.dataFor(off);
        state.servedFromReadonlyCache = true;
      } else {
        state.readonlyCache.invalidate();
      }
    }
    notifyCpuRead();
  }
  return lockHr;
}

[[nodiscard]] static HRESULT
unlockPeBuffer(D9CBuffer *buffer, D3D9PeRecorderFlush *recorder,
               const dxmt9::d3d9::pe::PeWireObjectRef &wireObject,
               D3D9PeBufferLockState &state) noexcept {
  if (!state.locked)
    return D3DERR_INVALIDCALL;
  if (state.servedFromReadonlyCache) {
    state.locked = false;
    state.servedFromReadonlyCache = false;
    state.flags = 0;
    state.data = nullptr;
    state.offset = 0;
    state.size = 0;
    return S_OK;
  }
  const HRESULT flushHr =
      flushChildRecorderForBufferLock(recorder, buffer, state.flags);
  if (FAILED(flushHr))
    return flushHr;
  std::vector<std::byte> mutationCopy;
  const bool mutationReady =
      (state.flags & D3DLOCK_READONLY) != 0u ||
      copyRenderTapeBufferMutation(recorder, state.data, state.size,
                                    mutationCopy);
  const HRESULT unlockHr = hr32(dxmt9c_buffer_unlock(buffer));
  if (SUCCEEDED(unlockHr)) {
    if (mutationReady && !mutationCopy.empty()) {
      recorder->NotifyRenderTapeResourceMutationForChild(
          wireObject, dxmt9::d3d9::RenderTapeMutationKind::CpuUnlock, 0u,
          state.offset,
          mutationCopy);
    }
    state.locked = false;
    state.servedFromReadonlyCache = false;
    state.flags = 0;
    state.data = nullptr;
    state.offset = 0;
    state.size = 0;
  }
  return unlockHr;
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
  D3D9PeBufferLockState lockState_{};
  dxmt9::d3d9::pe::PeWireObjectRef wireObject_{};
  DWORD priorityShadow_ = 0;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9VertexBufferImpl(D9CBuffer *b, IDirect3DDevice9 *device,
                       D3D9PeRecorderFlush *recorder = nullptr)
      : b_(b), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        b_, D9C_CHUNK_HANDLE_KIND_BUFFER,
        dxmt9c_buffer_get_wire_identity, wireObject_);
    descValid_ = loadBufferDesc(b_, desc_);
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             bufferIsDefaultPool(desc_, descValid_));
  }
  ~D3D9VertexBufferImpl() {
    if (recorder_)
      recorder_->NotifyRenderTapeObjectDestroyForChild(wireObject_);
    untrackDefaultPoolResource(recorder_, defaultPoolTracked_);
    dxmt9::d3d9::pe::unpublishCachedWireObjectRef(wireObject_);
    dxmt9c_buffer_release(b_);
    if (device_)
      device_->Release();
  }

  D9CBuffer *raw() const { return b_; }
  const dxmt9::d3d9::pe::PeWireObjectRef &wireObject() const {
    return wireObject_;
  }
  void invalidateReadonlyCache() noexcept {
    lockState_.readonlyCache.invalidate();
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
    return lockPeBuffer(b_, recorder_, desc_, descValid_, wireObject_,
                        lockState_, off, size, pp, flags);
  }
  HRESULT STDMETHODCALLTYPE Unlock() noexcept override {
    return unlockPeBuffer(b_, recorder_, wireObject_, lockState_);
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
  D3D9PeBufferLockState lockState_{};
  dxmt9::d3d9::pe::PeWireObjectRef wireObject_{};
  DWORD priorityShadow_ = 0;
  dxmt9::util::ComPrivateData privateData_{};

public:
  D3D9IndexBufferImpl(D9CBuffer *b, IDirect3DDevice9 *device,
                      D3D9PeRecorderFlush *recorder = nullptr)
      : b_(b), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        b_, D9C_CHUNK_HANDLE_KIND_BUFFER,
        dxmt9c_buffer_get_wire_identity, wireObject_);
    descValid_ = loadBufferDesc(b_, desc_);
    trackDefaultPoolResource(recorder_, defaultPoolTracked_,
                             bufferIsDefaultPool(desc_, descValid_));
  }
  ~D3D9IndexBufferImpl() {
    if (recorder_)
      recorder_->NotifyRenderTapeObjectDestroyForChild(wireObject_);
    untrackDefaultPoolResource(recorder_, defaultPoolTracked_);
    dxmt9::d3d9::pe::unpublishCachedWireObjectRef(wireObject_);
    dxmt9c_buffer_release(b_);
    if (device_)
      device_->Release();
  }

  D9CBuffer *raw() const { return b_; }
  const dxmt9::d3d9::pe::PeWireObjectRef &wireObject() const {
    return wireObject_;
  }
  void invalidateReadonlyCache() noexcept {
    lockState_.readonlyCache.invalidate();
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
    return lockPeBuffer(b_, recorder_, desc_, descValid_, wireObject_,
                        lockState_, off, size, pp, flags);
  }
  HRESULT STDMETHODCALLTYPE Unlock() noexcept override {
    return unlockPeBuffer(b_, recorder_, wireObject_, lockState_);
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

const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireVertexBuffer(IDirect3DVertexBuffer9 *buffer) {
  static const dxmt9::d3d9::pe::PeWireObjectRef empty{};
  return buffer ? static_cast<D3D9VertexBufferImpl *>(buffer)->wireObject()
                : empty;
}

const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireIndexBuffer(IDirect3DIndexBuffer9 *buffer) {
  static const dxmt9::d3d9::pe::PeWireObjectRef empty{};
  return buffer ? static_cast<D3D9IndexBufferImpl *>(buffer)->wireObject()
                : empty;
}

void D3D9PeInvalidateVertexBufferReadonlyCache(IDirect3DVertexBuffer9 *buffer) {
  if (buffer)
    static_cast<D3D9VertexBufferImpl *>(buffer)->invalidateReadonlyCache();
}
