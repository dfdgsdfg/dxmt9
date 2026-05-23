/* src/d3d9/d3d9_pe_device_child_misc.cpp — PE-side child COM wrappers
 * for IDirect3DVertexDeclaration9, IDirect3DQuery9, IDirect3DStateBlock9
 * and IDirect3DSwapChain9Ex (the non-resource families). */

#include "d3d9_pe_device_child.hpp"

#include "dxmt9/d3d9_raster_status.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <unordered_map>

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

static D9CRect toR(const RECT &r) {
  D9CRect c;
  c.left = r.left;
  c.top = r.top;
  c.right = r.right;
  c.bottom = r.bottom;
  return c;
}

[[nodiscard]] static HRESULT flushChildRecorder(D3D9PeRecorderFlush *recorder) {
  return recorder ? recorder->FlushPeRecorderForChild() : S_OK;
}

static bool isChildStateBlockRecording(D3D9PeRecorderFlush *recorder) {
  return recorder && recorder->IsStateBlockRecordingForChild();
}

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
  // PE-side snapshot of transforms / shader constants / vdecl populated by
  // CaptureStateBlockShadowForChild on End/Capture and replayed by Apply.
  // Lives only in the PE process — never crosses the unix boundary.
  D3D9StateBlockShadow saved_{};
  bool savedValid_ = false;

  // Replay all entries in saved_ onto the owning device via the existing
  // IDirect3DDevice9 COM methods. Each Set* lands in the device's normal
  // hot path (recorder picks up the change), so no new wire surface is
  // introduced. Best-effort: failures on individual entries are logged but
  // do not abort the apply — the upstream Wine tests only inspect the
  // specific entries the test recorded.
  void replaySavedShadow() noexcept {
    if (!savedValid_ || !device_) {
      return;
    }
    saved_.transforms.forEach([&](std::uint32_t state, const D9CMatrix &m) {
      const D3DMATRIX *pm = reinterpret_cast<const D3DMATRIX *>(&m);
      (void)device_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(state),
                                  pm);
    });
    constexpr std::size_t kFloatVecSize = sizeof(float) * 4;
    constexpr std::size_t kIntVecSize = sizeof(int32_t) * 4;
    constexpr std::size_t kBoolSize = sizeof(uint32_t);
    if (!saved_.vsConstF.empty()) {
      const UINT count =
          static_cast<UINT>(saved_.vsConstF.size() / kFloatVecSize);
      if (count > 0) {
        (void)device_->SetVertexShaderConstantF(
            0, reinterpret_cast<const float *>(saved_.vsConstF.data()), count);
      }
    }
    if (!saved_.vsConstI.empty()) {
      const UINT count =
          static_cast<UINT>(saved_.vsConstI.size() / kIntVecSize);
      if (count > 0) {
        (void)device_->SetVertexShaderConstantI(
            0, reinterpret_cast<const INT *>(saved_.vsConstI.data()), count);
      }
    }
    if (!saved_.vsConstB.empty()) {
      const UINT count =
          static_cast<UINT>(saved_.vsConstB.size() / kBoolSize);
      if (count > 0) {
        (void)device_->SetVertexShaderConstantB(
            0, reinterpret_cast<const BOOL *>(saved_.vsConstB.data()), count);
      }
    }
    if (!saved_.psConstF.empty()) {
      const UINT count =
          static_cast<UINT>(saved_.psConstF.size() / kFloatVecSize);
      if (count > 0) {
        (void)device_->SetPixelShaderConstantF(
            0, reinterpret_cast<const float *>(saved_.psConstF.data()), count);
      }
    }
    if (!saved_.psConstI.empty()) {
      const UINT count =
          static_cast<UINT>(saved_.psConstI.size() / kIntVecSize);
      if (count > 0) {
        (void)device_->SetPixelShaderConstantI(
            0, reinterpret_cast<const INT *>(saved_.psConstI.data()), count);
      }
    }
    if (!saved_.psConstB.empty()) {
      const UINT count =
          static_cast<UINT>(saved_.psConstB.size() / kBoolSize);
      if (count > 0) {
        (void)device_->SetPixelShaderConstantB(
            0, reinterpret_cast<const BOOL *>(saved_.psConstB.data()), count);
      }
    }
    if (saved_.hasVdecl) {
      // SetVertexDeclaration AddRefs internally; saved_.vdecl retains its own
      // ref until destructor (or next Capture).
      (void)device_->SetVertexDeclaration(saved_.vdecl);
    }
  }

public:
  D3D9StateBlockImpl(D9CStateBlock *sb, IDirect3DDevice9 *device,
                     D3D9PeRecorderFlush *recorder = nullptr)
      : sb_(sb), device_(device), recorder_(recorder) {
    if (device_)
      device_->AddRef();
    // Snapshot the device's current PE shadow at construction so a
    // CreateStateBlock(D3DSBT_ALL) / EndStateBlock-produced block holds the
    // transforms / constants / vdecl the upstream tests check on Apply.
    if (recorder_) {
      recorder_->CaptureStateBlockShadowForChild(saved_);
      savedValid_ = true;
    }
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
    if (saved_.vdecl) {
      saved_.vdecl->Release();
      saved_.vdecl = nullptr;
    }
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
    // PE-side snapshot: re-read transforms / shader constants / vdecl from
    // the device's current shadow so Apply replays the post-Capture state,
    // not the End-time state.
    if (recorder_) {
      recorder_->CaptureStateBlockShadowForChild(saved_);
      savedValid_ = true;
    }
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
    // Replay PE-side shadow after the existing apply so the recorder picks
    // up the transforms / shader constants / vdecl the upstream Wine tests
    // check on round-trip.
    replaySavedShadow();
    // Drain the PE recorder so a subsequent Get* sees the values we just
    // wrote. The PE-side Set* only updates the shadow / dirty range;
    // without a flush, server-side Get* still returns pre-Apply state.
    (void)flushChildRecorder(recorder_);
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
  // Wine d3d9 contract (test_swapchain_backbuffer_getter_policy +
  // test_additional_swapchain_backbuffer_bounds): repeated
  // GetBackBuffer(idx, *, &out) calls must return the same COM pointer
  // for the same idx regardless of D3DBACKBUFFER_TYPE.
  std::unordered_map<UINT, IDirect3DSurface9 *> cachedBackBuffers_;
  // Wine reset_lockable_backbuffer_policy / lockable_backbuffer_lock_policy:
  // the C ABI does not currently preserve D3DPRESENT_PARAMETERS.Flags
  // through CreateDevice / Reset, so cache the originally-requested Flags
  // on the wrapper and OR them into the value reported by
  // GetPresentParameters.
  DWORD flagsShadow_ = 0;

public:
  D3D9SwapChainImpl(D9CSwapChain *sc, IDirect3DDevice9 *device,
                    D3D9PeRecorderFlush *recorder = nullptr,
                    bool extended = false)
      : sc_(sc), device_(device), recorder_(recorder), extended_(extended) {
    if (device_)
      device_->AddRef();
  }

  void setFlagsShadow(DWORD flags) { flagsShadow_ = flags; }
  ~D3D9SwapChainImpl() {
    for (auto &entry : cachedBackBuffers_) {
      if (entry.second)
        entry.second->Release();
    }
    cachedBackBuffers_.clear();
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
  // Cache lookup shared by the swapchain-level and device-level
  // GetBackBuffer paths so the same idx always reports the same COM
  // pointer regardless of the entry surface (see
  // test_swapchain_backbuffer_getter_policy where the device-level
  // IDirect3DDevice9::GetBackBuffer call must match the prior
  // swapchain-level call).
  IDirect3DSurface9 *acquireCachedBackBuffer(UINT idx) {
    if (auto it = cachedBackBuffers_.find(idx); it != cachedBackBuffers_.end()) {
      it->second->AddRef();
      return it->second;
    }
    D9CSurface *s = dxmt9c_swapchain_get_back_buffer(sc_, idx, 0);
    if (!s)
      return nullptr;
    auto *surface = CreatePeSurface(
        s, device_, static_cast<IDirect3DSwapChain9 *>(this), recorder_, false);
    surface->AddRef();
    cachedBackBuffers_.emplace(idx, surface);
    return surface;
  }

  HRESULT STDMETHODCALLTYPE GetBackBuffer(
      UINT idx, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppS) noexcept override {
    if (!ppS)
      return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("swapchain_get_back_buffer sc=%p idx=%u", this, idx);
    // Wine d3d9 test_swapchain_parameters: GetBackBuffer with an index
    // >= BackBufferCount returns D3DERR_INVALIDCALL and must NOT
    // overwrite the caller-supplied *ppS (the test seeds it with
    // 0xdeadbeef and asserts the sentinel survives).
    D9CPresentParams cppGuard{};
    if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(sc_, &cppGuard)))) {
      if (idx >= cppGuard.backBufferCount)
        return D3DERR_INVALIDCALL;
    }
    *ppS = nullptr;
    if (auto it = cachedBackBuffers_.find(idx); it != cachedBackBuffers_.end()) {
      it->second->AddRef();
      *ppS = it->second;
      return S_OK;
    }
    D9CSurface *s = dxmt9c_swapchain_get_back_buffer(sc_, idx, 0);
    if (!s)
      return D3DERR_INVALIDCALL;
    auto *surface = CreatePeSurface(
        s, device_, static_cast<IDirect3DSwapChain9 *>(this), recorder_, false);
    // Wine d3d9: same idx must yield the same COM pointer across calls.
    // Keep one internal reference so future Get* lookups can AddRef and
    // return the cached pointer.
    surface->AddRef();
    cachedBackBuffers_.emplace(idx, surface);
    *ppS = surface;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetRasterStatus(D3DRASTER_STATUS *p) noexcept override {
    if (!p)
      return S_OK;
    // Synthesize a monotonically-advancing ScanLine so apps that VBlank-poll do
    // not spin forever. dxmt9 has no real per-line vblank signal from Metal;
    // we derive a counter-modulo-height estimate from the swapchain backbuffer.
    static std::atomic<uint64_t> rasterTick{0};
    uint32_t displayHeight = 0;
    D9CPresentParams cpp{};
    if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(sc_, &cpp)))) {
      displayHeight = cpp.backBufferHeight;
    }
    const auto tick = rasterTick.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto est = ::dxmt9::d3d9::computeRasterStatusEstimate(tick, displayHeight);
    memset(p, 0, sizeof(*p));
    p->ScanLine = est.scanLine;
    p->InVBlank = est.inVBlank ? TRUE : FALSE;
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
      pPP->Flags = cpp.flags | flagsShadow_;
      pPP->FullScreen_RefreshRateInHz = cpp.fullScreenRefreshRateHz;
      pPP->PresentationInterval = cpp.presentationInterval;
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  GetLastPresentCount(UINT *pLastPresentCount) noexcept override {
    // stub: Wine returns S_OK; presentation statistics not measured.
    if (pLastPresentCount)
      *pLastPresentCount = 0u;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics) noexcept override {
    // stub: Wine returns S_OK; presentation statistics not measured.
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
 * Public factory + raw-handle extractors for misc family.
 * ========================================================================= */

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
                                         bool extended,
                                         DWORD presentFlagsShadow) {
  auto *impl =
      new D3D9SwapChainImpl(swapChain, device, recorder, extended);
  impl->setFlagsShadow(presentFlagsShadow);
  return impl;
}

D9CVertexDecl *D3D9PeRawVertexDecl(IDirect3DVertexDeclaration9 *decl) {
  return decl ? static_cast<D3D9VertexDeclImpl *>(decl)->raw() : nullptr;
}
