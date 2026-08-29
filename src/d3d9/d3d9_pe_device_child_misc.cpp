/* src/d3d9/d3d9_pe_device_child_misc.cpp — PE-side child COM wrappers
 * for IDirect3DVertexDeclaration9, IDirect3DQuery9, IDirect3DStateBlock9
 * and IDirect3DSwapChain9Ex (the non-resource families). */

#include "d3d9_pe_device_child.hpp"
#include "d3d9_pe_validated_object_writer.hpp"
#include "d3d9_pe_com_cache.hpp"
#include "d3d9_pe_stateblock_fault.hpp"
#include "d3d9_pe_stateblock_transaction.hpp"
#include "d3d9_pe_stateblock_value.hpp"
#include "d3d9_pe_wsi.hpp"

#include "dxmt9/d3d9_raster_status.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <new>
#include <unordered_map>

template <typename T, typename... Args>
T* peNewNoexcept(Args&&... args) noexcept {
  try {
    return new (std::nothrow) T(std::forward<Args>(args)...);
  } catch (...) {
    return nullptr;
  }
}

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

template<typename Context>
[[nodiscard]] static HRESULT flushChildRecorder(Context *recorder) {
  return recorder ? recorder->FlushPeRecorderForChild() : S_OK;
}

static bool isChildStateBlockRecording(D3D9PeStateBlockContext *recorder) {
  return recorder && recorder->IsStateBlockRecordingForChild();
}

/* ── VertexDeclaration ──────────────────────────────────────────────────────
 */

class D3D9VertexDeclImpl final : public IDirect3DVertexDeclaration9 {
  ULONG refs_ = 1;
  D9CVertexDecl *d_;
  IDirect3DDevice9 *device_;
  D3D9PeShaderDeclarationContext *context_;
  dxmt9::d3d9::pe::DeclarationRef wireObject_{};

public:
  D3D9VertexDeclImpl(D9CVertexDecl *d, IDirect3DDevice9 *device,
                     D3D9PeShaderDeclarationContext *recorder)
      : d_(d), device_(device), context_(recorder) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        d_,
        dxmt9c_vdecl_get_wire_identity, wireObject_);
  }
  ~D3D9VertexDeclImpl() {
    if (context_)
      context_->NotifyRenderTapeObjectDestroyForChild(wireObject_);
    dxmt9c_vdecl_release(d_);
    if (device_)
      device_->Release();
  }

  D9CVertexDecl *raw() const { return d_; }
  IDirect3DDevice9 *ownerDevice() const { return device_; }
  const dxmt9::d3d9::pe::DeclarationRef &wireObject() const {
    return wireObject_;
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
  D3D9PeQueryContext *context_;
  D3D9PeDiagnosticObserver *diagnostics_;
  dxmt9::d3d9::pe::QueryRef wireObject_{};

public:
  D3D9QueryImpl(D9CQuery *q, IDirect3DDevice9 *device,
                D3D9PeQueryContext *recorder = nullptr,
                D3D9PeDiagnosticObserver *diagnostics = nullptr)
      : q_(q), device_(device), context_(recorder),
        diagnostics_(diagnostics) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        q_,
        dxmt9c_query_get_wire_identity, wireObject_);
  }
  ~D3D9QueryImpl() {
    if (context_)
      context_->NotifyRenderTapeObjectDestroyForChild(wireObject_);
    dxmt9c_query_release(q_);
    if (device_)
      device_->Release();
  }

  D9CQuery *raw() const { return q_; }
  IDirect3DDevice9 *ownerDevice() const { return device_; }
  const dxmt9::d3d9::pe::QueryRef &wireObject() const {
    return wireObject_;
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
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "Query::GetType", DXMT9_PE_CALLSITE_PC());
    return (D3DQUERYTYPE)dxmt9c_query_get_type(q_);
  }
  DWORD STDMETHODCALLTYPE GetDataSize() noexcept override {
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "Query::GetDataSize", DXMT9_PE_CALLSITE_PC());
    return dxmt9c_query_get_data_size(q_);
  }
  HRESULT STDMETHODCALLTYPE Issue(DWORD flags) noexcept override {
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "Query::Issue", DXMT9_PE_CALLSITE_PC());
    // Phase 20: Query::Issue (D3DISSUE_BEGIN / D3DISSUE_END) is
    // fire-and-forget — server records it into the query object,
    // PE caller doesn't wait. Chunk-record path keeps it ordered
    // with surrounding draws within the same chunk; legacy path
    // falls back to flush+bridge.
    if (context_ && context_->IsChunkRecorderEnabledForChild()) {
      return context_->AppendQueryIssueForChild(
          static_cast<std::uint32_t>(flags), wireObject());
    }
    const HRESULT flushHr = flushChildRecorder(context_);
    if (FAILED(flushHr)) {
      (void)planPeStateBlockTransition(
          {PeStateBlockPhase::Idle,
           PeStateBlockEvent::CapturePreEffectFailed});
      return flushHr;
    }
    return hr32(dxmt9c_query_issue(q_, flags));
  }
  HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD size,
                                    DWORD flags) noexcept override {
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "Query::GetData", DXMT9_PE_CALLSITE_PC());
    const HRESULT flushHr = flushChildRecorder(context_);
    if (FAILED(flushHr))
      return flushHr;
    const HRESULT hr = hr32(dxmt9c_query_get_data(q_, pData, size, flags));
    if (context_) {
      const auto disposition =
          hr == S_FALSE
              ? dxmt9::d3d9::RenderTapeControlDisposition::Pending
              : SUCCEEDED(hr)
                    ? dxmt9::d3d9::RenderTapeControlDisposition::Completed
                    : dxmt9::d3d9::RenderTapeControlDisposition::Failed;
      const dxmt9::d3d9::RenderTapeQueryGetDataControl payload{
          .dataSize = size, .seqId = 0u};
      context_->NotifyRenderTapeOrderedControlForChild(
          dxmt9::d3d9::RenderTapeOrderedControlHeader{
              .identity = wireObject_.identity,
              .kind = static_cast<std::uint32_t>(
                  dxmt9::d3d9::RenderTapeControlKind::QueryGetData),
              .disposition = static_cast<std::uint32_t>(disposition),
              .resultCode = static_cast<std::int32_t>(hr),
              .controlBytes = sizeof(payload)},
          std::as_bytes(std::span(&payload, 1u)));
    }
    return hr;
  }
};

/* ── StateBlock ─────────────────────────────────────────────────────────────
 */

class D3D9StateBlockImpl final : public IDirect3DStateBlock9 {
  std::atomic<ULONG> refs_{1};
  D9CStateBlock *sb_;
  IDirect3DDevice9 *device_;
  D3D9PeStateBlockContext *context_;
  D3D9PeDiagnosticObserver *diagnostics_;
  StateBlockCaptureDisposition captureDisposition_ =
      StateBlockCaptureDisposition::All;
  // PE-side snapshot of transforms / shader constants / vdecl populated by
  // CaptureStateBlockShadowForChild on End/Capture and replayed by Apply.
  // Lives only in the PE process — never crosses the unix boundary.
  D3D9StateBlockShadow saved_{};
  bool savedValid_ = false;


public:
  D3D9StateBlockImpl(D9CStateBlock *sb, IDirect3DDevice9 *device,
                     D3D9PeStateBlockContext *recorder = nullptr,
                     D3D9PeDiagnosticObserver *diagnostics = nullptr,
                     StateBlockCaptureDisposition disposition =
                         StateBlockCaptureDisposition::All)
      : sb_(sb), device_(device), context_(recorder),
        diagnostics_(diagnostics), captureDisposition_(disposition) {
    // Snapshot the device's current PE shadow at construction so a
    // CreateStateBlock(D3DSBT_ALL) / EndStateBlock-produced block holds the
    // transforms / constants / vdecl the upstream tests check on Apply.
    if (context_) {
      savedValid_ = SUCCEEDED(
          context_->CaptureStateBlockShadowForChild(saved_,
                                                     captureDisposition_));
    }
    if (device_)
      device_->AddRef();
    dxmt9DeviceDebugLog("stateblock_ctor this=%p sb=%p device=%p refs=%u", this,
                        static_cast<void *>(sb_), static_cast<void *>(device_),
                        (unsigned)refs_.load());
  }
  bool snapshotValid() const noexcept {
    return !context_ || savedValid_;
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
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent("StateBlock::Capture");
    dxmt9DeviceDebugLog("stateblock_capture sb=%p", this);
    D3D9PeChildOperationGuard operation(context_);
    if (isChildStateBlockRecording(context_)) {
      return D3DERR_INVALIDCALL;
    }
    if (context_ && context_->IsStateBlockRecorderPoisonedForChild()) {
      return D3DERR_DEVICELOST;
    }
    std::int32_t injectedHr = 0;
    if (dxmt9PeConsumeStateBlockFault(
            PeStateBlockFaultPoint::CapturePre, injectedHr)) {
      if (diagnostics_)
        diagnostics_->notifyStateBlockFault(false, injectedHr);
      return hr32(injectedHr);
    }
    const HRESULT flushHr = flushChildRecorder(context_);
    if (FAILED(flushHr))
      return flushHr;
    D3D9StateBlockShadow candidate{};
    if (context_) {
      try {
        if (savedValid_) {
          candidate = saved_;
          // Copy assignment retains vdecl; this candidate does not need that
          // separate owner because CaptureStateBlockShadowForChild refreshes
          // the fixed candidate and the original snapshot remains alive.
          candidate.writer().discardVdecl();
        }
      } catch (const std::bad_alloc &) {
        (void)planPeStateBlockTransition(
            {PeStateBlockPhase::Idle,
             PeStateBlockEvent::CapturePreEffectFailed});
        return E_OUTOFMEMORY;
      }
      const HRESULT captureHr =
          context_->CaptureStateBlockShadowForChild(candidate,
                                                      captureDisposition_);
      if (FAILED(captureHr)) {
        (void)planPeStateBlockTransition(
            {PeStateBlockPhase::Idle,
             PeStateBlockEvent::CapturePreEffectFailed});
        return captureHr;
      }
    }
    const HRESULT hr = hr32(dxmt9c_stateblock_capture(sb_));
    if (context_ && SUCCEEDED(hr) &&
        dxmt9PeConsumeStateBlockEnteredFault(
            PeStateBlockFaultPoint::CaptureEntered, injectedHr)) {
      if (diagnostics_)
        diagnostics_->notifyStateBlockFault(true, injectedHr);
      context_->PoisonStateBlockRecorderForChild();
      return hr32(injectedHr);
    }
    const auto valuePlan = planPeStateBlockValue(
        FAILED(hr) ? PeStateBlockValueEvent::CaptureBackendFailed
                   : PeStateBlockValueEvent::CaptureAccepted);
    const auto capturePlan = planPeStateBlockTransition(
        {PeStateBlockPhase::Idle,
         FAILED(hr) ? PeStateBlockEvent::CaptureBackendFailed
                    : PeStateBlockEvent::CapturePublished});
    if (capturePlan.action() == PeStateBlockAction::FailStop ||
        valuePlan.poison()) {
      if (context_) context_->PoisonStateBlockRecorderForChild();
      dxmt9DeviceDebugLog("stateblock_capture -> hr=0x%08x", (unsigned)hr);
      return hr;
    }
    if (context_ && valuePlan.refreshSnapshot()) {
      saved_ = std::move(candidate);
      savedValid_ = true;
    }
    dxmt9DeviceDebugLog("stateblock_capture -> hr=0x%08x", (unsigned)hr);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE Apply() noexcept override {
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent("StateBlock::Apply");
    dxmt9DeviceDebugLog("stateblock_apply sb=%p", this);
    D3D9PeChildOperationGuard operation(context_);
    if (isChildStateBlockRecording(context_)) {
      return D3DERR_INVALIDCALL;
    }
    if (context_ && context_->IsStateBlockRecorderPoisonedForChild()) {
      return D3DERR_DEVICELOST;
    }
    std::int32_t injectedHr = 0;
    if (dxmt9PeConsumeStateBlockFault(
            PeStateBlockFaultPoint::ApplyPre, injectedHr)) {
      if (diagnostics_)
        diagnostics_->notifyStateBlockFault(false, injectedHr);
      return hr32(injectedHr);
    }
    const HRESULT flushHr = flushChildRecorder(context_);
    if (FAILED(flushHr))
      return flushHr;
    if (context_) {
      const HRESULT prepareHr =
          context_->PrepareStateBlockApplyForChild(saved_);
      const auto valuePlan = planPeStateBlockValue(
          SUCCEEDED(prepareHr) ? PeStateBlockValueEvent::ApplyPrepared
                               : PeStateBlockValueEvent::ApplyPreEffectFailed);
      if (peStateBlockApplyTransition(
              PeStateBlockApplyPhase::Prepare, SUCCEEDED(prepareHr)) !=
              PeStateBlockApplyAction::Continue ||
          (!SUCCEEDED(prepareHr) && !valuePlan.preserveTrackedSet())) {
        return prepareHr;
      }
    }
    const HRESULT hr = hr32(dxmt9c_stateblock_apply(sb_));
    if (context_ && SUCCEEDED(hr) &&
        dxmt9PeConsumeStateBlockEnteredFault(
            PeStateBlockFaultPoint::ApplyEntered, injectedHr)) {
      if (diagnostics_)
        diagnostics_->notifyStateBlockFault(true, injectedHr);
      context_->DiscardPreparedStateBlockApplyForChild();
      context_->PoisonStateBlockRecorderForChild();
      return hr32(injectedHr);
    }
    const auto valuePlan = planPeStateBlockValue(
        FAILED(hr) ? PeStateBlockValueEvent::ApplyBackendFailed
                   : PeStateBlockValueEvent::ApplyAccepted);
    if (peStateBlockApplyTransition(
            PeStateBlockApplyPhase::Backend, SUCCEEDED(hr)) ==
            PeStateBlockApplyAction::Poison || valuePlan.poison()) {
      if (context_) {
        context_->DiscardPreparedStateBlockApplyForChild();
        context_->PoisonStateBlockRecorderForChild();
      }
      dxmt9DeviceDebugLog("stateblock_apply -> hr=0x%08x", (unsigned)hr);
      return hr;
    }
    if (context_ && valuePlan.publishLive())
      context_->CommitStateBlockApplyForChild(saved_, captureDisposition_);
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
  D3D9PePresentationContext *context_;
  D3D9PeDiagnosticObserver *diagnostics_;
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
  D3D9PeWsiBinding wsiBinding_{};

public:
  D3D9SwapChainImpl(D9CSwapChain *sc, IDirect3DDevice9 *device,
                    D3D9PePresentationContext *recorder = nullptr,
                    D3D9PeDiagnosticObserver *diagnostics = nullptr,
                    bool extended = false,
                    D3D9PeWsiBinding *wsiBinding = nullptr)
      : sc_(sc), device_(device), context_(recorder),
        diagnostics_(diagnostics), extended_(extended) {
    if (device_)
      device_->AddRef();
    if (wsiBinding)
      wsiBinding_ = std::move(*wsiBinding);
  }

  void setFlagsShadow(DWORD flags) { flagsShadow_ = flags; }
  ~D3D9SwapChainImpl() {
    for (auto &entry : cachedBackBuffers_) {
      if (entry.second)
        entry.second->Release();
    }
    cachedBackBuffers_.clear();
    dxmt9PeFinalizeAndReleaseWsiBinding(sc_, wsiBinding_);
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
    const HRESULT flushHr = flushChildRecorder(context_);
    if (FAILED(flushHr))
      return flushHr;
    return hr32(
        dxmt9c_swapchain_present(sc_, src ? &cs : nullptr, dst ? &cd : nullptr,
                                 (uint64_t)(uintptr_t)wnd, dirty, flags));
  }
  HRESULT STDMETHODCALLTYPE
  GetFrontBufferData(IDirect3DSurface9 *destination) noexcept override {
    if (!destination || !device_)
      return D3DERR_INVALIDCALL;

    D3DSURFACE_DESC destinationDesc{};
    if (FAILED(destination->GetDesc(&destinationDesc)) ||
        destinationDesc.Pool != D3DPOOL_SYSTEMMEM ||
        destinationDesc.Format != D3DFMT_A8R8G8B8)
      return D3DERR_INVALIDCALL;

    IDirect3DSurface9 *backBuffer = nullptr;
    HRESULT hr = GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(hr) || !backBuffer)
      return hr;

    D3DSURFACE_DESC backBufferDesc{};
    hr = backBuffer->GetDesc(&backBufferDesc);
    if (FAILED(hr)) {
      backBuffer->Release();
      return hr;
    }

    IDirect3DSurface9 *readbackSource = backBuffer;
    IDirect3DSurface9 *resolved = nullptr;
    if (backBufferDesc.MultiSampleType != D3DMULTISAMPLE_NONE) {
      hr = device_->CreateRenderTarget(
          backBufferDesc.Width, backBufferDesc.Height,
          backBufferDesc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
          &resolved, nullptr);
      if (SUCCEEDED(hr))
        hr = device_->StretchRect(backBuffer, nullptr, resolved, nullptr,
                                  D3DTEXF_NONE);
      if (SUCCEEDED(hr))
        readbackSource = resolved;
    }
    if (SUCCEEDED(hr))
      hr = device_->GetRenderTargetData(readbackSource, destination);

    if (resolved)
      resolved->Release();
    backBuffer->Release();
    return hr;
  }
  // Cache lookup shared by the swapchain-level and device-level
  // GetBackBuffer paths so the same idx always reports the same COM
  // pointer regardless of the entry surface (see
  // test_swapchain_backbuffer_getter_policy where the device-level
  // IDirect3DDevice9::GetBackBuffer call must match the prior
  // swapchain-level call).
  IDirect3DSurface9 *acquireCachedBackBuffer(UINT idx) {
    D3D9PeChildOperationGuard operation(context_);
    if (auto it = cachedBackBuffers_.find(idx); it != cachedBackBuffers_.end()) {
      it->second->AddRef();
      return it->second;
    }
    D9CSurface *s = dxmt9c_swapchain_get_back_buffer(sc_, idx, 0);
    if (!s)
      return nullptr;
    auto *surface = CreatePeSurface(
        s, device_, static_cast<IDirect3DSwapChain9 *>(this),
        context_->surfaceTextureContextForChild(),
        diagnostics_, false);
    if (!surface) {
      dxmt9c_surface_release(s);
      return nullptr;
    }
    IDirect3DSurface9* canonical = nullptr;
    const auto insertStatus = D3D9PeCanonicalizeComCacheInsertion(
        cachedBackBuffers_, idx, surface, &canonical);
    if (insertStatus == D3D9PeComCacheInsertStatus::OutOfMemory ||
        insertStatus == D3D9PeComCacheInsertStatus::Failed) {
      return nullptr;
    }
    return canonical;
  }

  HRESULT STDMETHODCALLTYPE GetBackBuffer(
      UINT idx, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppS) noexcept override {
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "SwapChain::GetBackBuffer", DXMT9_PE_CALLSITE_PC());
    if (!ppS)
      return D3DERR_INVALIDCALL;
    D3D9PeChildOperationGuard operation(context_);
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
        s, device_, static_cast<IDirect3DSwapChain9 *>(this),
        context_->surfaceTextureContextForChild(),
        diagnostics_, false);
    if (!surface) {
      dxmt9c_surface_release(s);
      return E_OUTOFMEMORY;
    }
    // Wine d3d9: same idx must yield the same COM pointer across calls.
    // Keep one internal reference so future Get* lookups can AddRef and
    // return the cached pointer.
    IDirect3DSurface9* canonical = nullptr;
    const auto insertStatus = D3D9PeCanonicalizeComCacheInsertion(
        cachedBackBuffers_, idx, surface, &canonical);
    if (insertStatus == D3D9PeComCacheInsertStatus::OutOfMemory)
      return E_OUTOFMEMORY;
    if (insertStatus == D3D9PeComCacheInsertStatus::Failed)
      return D3DERR_INVALIDCALL;
    *ppS = canonical;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetRasterStatus(D3DRASTER_STATUS *p) noexcept override {
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "SwapChain::GetRasterStatus", DXMT9_PE_CALLSITE_PC());
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
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "SwapChain::GetDisplayMode", DXMT9_PE_CALLSITE_PC());
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
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "SwapChain::GetPresentParameters", DXMT9_PE_CALLSITE_PC());
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
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "SwapChain::GetLastPresentCount", DXMT9_PE_CALLSITE_PC());
    // stub: Wine returns S_OK; presentation statistics not measured.
    if (pLastPresentCount)
      *pLastPresentCount = 0u;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics) noexcept override {
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "SwapChain::GetPresentStats", DXMT9_PE_CALLSITE_PC());
    // stub: Wine returns S_OK; presentation statistics not measured.
    if (pPresentationStatistics) {
      memset(pPresentationStatistics, 0, sizeof(*pPresentationStatistics));
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetDisplayModeEx(D3DDISPLAYMODEEX *pMode,
                   D3DDISPLAYROTATION *pRotation) noexcept override {
    if (diagnostics_)
      diagnostics_->notifyFirstCallAfterPresent(
          "SwapChain::GetDisplayModeEx", DXMT9_PE_CALLSITE_PC());
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
 * Public factory + trusted reference helpers for misc family.
 * ========================================================================= */

IDirect3DVertexDeclaration9 *CreatePeVertexDecl(D9CVertexDecl *decl,
                                                IDirect3DDevice9 *device,
                                                D3D9PeShaderDeclarationContext *recorder) noexcept {
  return peNewNoexcept<D3D9VertexDeclImpl>(decl, device, recorder);
}

IDirect3DQuery9 *CreatePeQuery(D9CQuery *query, IDirect3DDevice9 *device,
                               D3D9PeQueryContext *recorder,
                               D3D9PeDiagnosticObserver *diagnostics) noexcept {
  return peNewNoexcept<D3D9QueryImpl>(query, device, recorder, diagnostics);
}

IDirect3DStateBlock9 *CreatePeStateBlock(D9CStateBlock *stateBlock,
                                         IDirect3DDevice9 *device,
                                         D3D9PeStateBlockContext *recorder,
                                         D3D9PeDiagnosticObserver *diagnostics,
                                         StateBlockCaptureDisposition disposition) noexcept {
  D3D9StateBlockImpl *impl = nullptr;
  try {
    impl = new (std::nothrow)
        D3D9StateBlockImpl(stateBlock, device, recorder, diagnostics,
                           disposition);
  } catch (...) {
    if (stateBlock) dxmt9c_stateblock_release(stateBlock);
    return nullptr;
  }
  if (!impl) {
    if (stateBlock) dxmt9c_stateblock_release(stateBlock);
    return nullptr;
  }
  if (!impl->snapshotValid()) {
    delete impl;
    return nullptr;
  }
  return impl;
}

IDirect3DSwapChain9Ex *CreatePeSwapChain(D9CSwapChain *swapChain,
                                         IDirect3DDevice9 *device,
                                         D3D9PePresentationContext *recorder,
                                         D3D9PeDiagnosticObserver *diagnostics,
                                         bool extended,
                                         DWORD presentFlagsShadow,
                                         D3D9PeWsiBinding wsiBinding) noexcept {
  auto *impl = peNewNoexcept<D3D9SwapChainImpl>(
      swapChain, device, recorder, diagnostics, extended, &wsiBinding);
  if (!impl) {
    dxmt9PeFinalizeAndReleaseWsiBinding(swapChain, wsiBinding);
    return nullptr;
  }
  impl->setFlagsShadow(presentFlagsShadow);
  return impl;
}

HRESULT D3D9PeValidateVertexDecl(
    IDirect3DVertexDeclaration9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedDeclaration* out) noexcept {
  D3D9PeValidatedObjectWriter::clear(out);
  if (!object) return S_OK;
  auto* impl = dynamic_cast<D3D9VertexDeclImpl*>(object);
  if (!impl || static_cast<IDirect3DVertexDeclaration9*>(impl) != object) {
    return D3DERR_INVALIDCALL;
  }
  const auto& wire = impl->wireObject();
  const dxmt9::d3d9::pe::PeConcreteMemberIdentity identity{
      .kind = dxmt9::d3d9::pe::PeConcreteObjectKind::VertexDeclaration,
      .ownerDevice = impl->ownerDevice(),
      .publicIdentity = static_cast<IDirect3DVertexDeclaration9*>(impl),
      .wireIdentity = wire.identity,
  };
  if (!dxmt9::d3d9::pe::validateConcreteMemberIdentity(
          identity,
          dxmt9::d3d9::pe::PeConcreteObjectKind::VertexDeclaration,
          expectedOwnerDevice, object)) {
    return D3DERR_INVALIDCALL;
  }
  D3D9PeValidatedObjectWriter::assign(
      out, object, expectedOwnerDevice, impl->raw(), wire, 0u,
      dxmt9::d3d9::pe::PeConcreteObjectKind::VertexDeclaration);
  return S_OK;
}

HRESULT D3D9PeValidateQuery(
    IDirect3DQuery9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedQuery* out) noexcept {
  D3D9PeValidatedObjectWriter::clear(out);
  if (!object) return S_OK;
  auto* impl = dynamic_cast<D3D9QueryImpl*>(object);
  if (!impl || static_cast<IDirect3DQuery9*>(impl) != object) {
    return D3DERR_INVALIDCALL;
  }
  const auto& wire = impl->wireObject();
  const dxmt9::d3d9::pe::PeConcreteMemberIdentity identity{
      .kind = dxmt9::d3d9::pe::PeConcreteObjectKind::Query,
      .ownerDevice = impl->ownerDevice(),
      .publicIdentity = static_cast<IDirect3DQuery9*>(impl),
      .wireIdentity = wire.identity,
  };
  if (!dxmt9::d3d9::pe::validateConcreteMemberIdentity(
          identity, dxmt9::d3d9::pe::PeConcreteObjectKind::Query,
          expectedOwnerDevice, object)) {
    return D3DERR_INVALIDCALL;
  }
  D3D9PeValidatedObjectWriter::assign(
      out, object, expectedOwnerDevice, impl->raw(), wire, 0u,
      dxmt9::d3d9::pe::PeConcreteObjectKind::Query);
  return S_OK;
}
