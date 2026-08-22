/* src/d3d9/d3d9_pe_device_com_cold.cpp — D3D9DeviceImpl cold COM surface.
 *
 * The IDirect3DDevice9Ex entry points an app calls at load, on a device
 * reset, or once per resource — not per draw and not per frame: every
 * Get* accessor, every Create* factory, the resource copy/fill operations
 * (UpdateSurface, UpdateTexture, StretchRect, ColorFill, ProcessVertices),
 * Reset/ResetEx and the device-lost queries, cursor/gamma/palette, the caps
 * and display-mode queries, and the state-block plumbing.
 *
 * All 96 are virtual overrides. Out-lining them here is only safe because
 * d3d9_pe_device.cpp pins an explicit key function (FlushPeRecorderForChild,
 * the first non-pure non-inline virtual in declaration order); without it the
 * first cold TU to out-line a virtual claims the vtable and the whole COM
 * surface with it. See the comment on FlushPeRecorderForChild in the class
 * header and agents/rules/codebase_conventions.rules.md.
 *
 * The two validator regions below are reached only from these definitions, so
 * they move out of the class header and become this TU's anonymous namespace.
 * The wrapper is what gives them internal linkage; the file bodies are
 * unchanged. */

#include "d3d9_pe_device_impl.hpp"

namespace {

#include "d3d9_pe_device_com_cold_helpers.inc.hpp"

#include "d3d9_pe_device_com_shader_validators.inc.hpp"

}  // namespace

void D3D9DeviceImpl::CaptureStateBlockShadowForChild(D3D9StateBlockShadow& out) {
    const bool initialSnapshot = !out.initialized;
    if (initialSnapshot) {
        // Called from D3D9StateBlockImpl ctor.
        // Begin/End path (insideEndStateBlock_): take the recorded
        // set verbatim — even if it is empty (the block tracked no
        // transforms; Apply is a no-op for transforms).
        // CreateStateBlock path: capture the entire live shadow.
        if (insideEndStateBlock_) {
            out.transforms = peState_.stateBlockTransformRecorded;
        } else {
            out.transforms = peState_.transformShadow;
        }
        out.initialized = true;
    } else {
        // Refresh mode (mid-game D3D9StateBlockImpl::Capture()).
        // The set of tracked transform keys is FIXED at ctor;
        // Capture() only refreshes their values from the live
        // shadow. Keys absent from out.transforms (e.g. a block
        // whose End-time recorded set was empty because everything
        // was MultiplyTransform) stay absent — Apply will not
        // touch them.
        FixedTransformTable refreshed{};
        out.transforms.forEach([&](uint32_t state, const D9CMatrix& /*old*/) {
            D9CMatrix latest{};
            if (peState_.transformShadow.get(state, latest)) {
                refreshed.set(state, latest);
            } else {
                // Server lost the binding (e.g. Reset); keep the
                // previously-captured value as a best-effort fallback.
                D9CMatrix prior{};
                out.transforms.get(state, prior);
                refreshed.set(state, prior);
            }
        });
        out.transforms = refreshed;
    }
    out.vsConstF   = peConsts_.vsConstF.values;
    out.vsConstI   = peConsts_.vsConstI.values;
    out.vsConstB   = peConsts_.vsConstB.values;
    out.psConstF   = peConsts_.psConstF.values;
    out.psConstI   = peConsts_.psConstI.values;
    out.psConstB   = peConsts_.psConstB.values;
    // vdecl tracking:
    //   - Initial snapshot, Begin/End path → track only if
    //     SetVertexDeclaration was called during recording.
    //   - Initial snapshot, CreateStateBlock path → always track.
    //   - Mid-game refresh → preserve the initial-time decision
    //     (out.hasVdecl set at ctor time).
    bool shouldTrackVdecl;
    if (initialSnapshot) {
        shouldTrackVdecl =
            insideEndStateBlock_ ? peState_.stateBlockVdeclRecorded : true;
    } else {
        shouldTrackVdecl = out.hasVdecl;
    }
    if (out.vdecl) {
        out.vdecl->Release();
        out.vdecl = nullptr;
    }
    out.hasVdecl = shouldTrackVdecl && (vdecl_ != nullptr);
    if (shouldTrackVdecl && vdecl_) {
        vdecl_->AddRef();
        out.vdecl = vdecl_;
    }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::TestCooperativeLevel() noexcept {
    dxmt9DeviceDebugLog("device_test_cooperative_level device=%p", this);
    if (deviceNotReset_) {
        dxmt9DeviceDebugLog("device_test_cooperative_level -> device not reset");
        return D3DERR_DEVICENOTRESET;
    }
    const HRESULT hr = hr32(dxmt9c_device_test_cooperative_level(dev_));
    dxmt9DeviceDebugLog("device_test_cooperative_level -> hr=0x%08x", (unsigned)hr);
    return hr;
}

UINT STDMETHODCALLTYPE D3D9DeviceImpl::GetAvailableTextureMem() noexcept {
    dxmt9DeviceDebugLog("device_get_available_texture_mem device=%p", this);
    // Wine base_vidmem_accounting_policy: report a pseudo-budget that
    // decreases with each large allocation. The actual GPU has its own
    // budget machinery; this PE-side accounting only needs to expose
    // the strictly-decreasing property the conformance test asserts.
    constexpr uint64_t kBudget = 0x80000000ull;  // 2 GiB sentinel
    const uint64_t used = vidmemBytesUsedShadow_;
    const uint64_t remaining = used >= kBudget ? 0ull : (kBudget - used);
    const UINT value = static_cast<UINT>(remaining);
    dxmt9DeviceDebugLog("device_get_available_texture_mem -> %u (0x%x) used=%llu",
                        value, (unsigned)value, (unsigned long long)used);
    return value;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::EvictManagedResources() noexcept {
    // stub: Wine returns S_OK; Apple GPUs have unified memory, manual eviction is not exposed.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDirect3D(IDirect3D9** ppD3D) noexcept {
    if (!ppD3D) return D3DERR_INVALIDCALL;
    factory_->AddRef();
    *ppD3D = static_cast<IDirect3D9*>(factory_);
    dxmt9DeviceDebugLog("device_get_direct3d this=%p -> factory=%p", this, static_cast<void*>(*ppD3D));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDeviceCaps(D3DCAPS9* pCaps) noexcept {
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

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDisplayMode(UINT sc, D3DDISPLAYMODE* pMode) noexcept {
    if (!pMode) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_get_display_mode device=%p sc=%u", this, sc);
    D9CSwapChain* chain = borrowSwapChainHandle(sc);
    if (!chain) {
        return D3DERR_INVALIDCALL;
    }
    D9CPresentParams cpp{};
    const HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(chain, &cpp));
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

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetCreationParameters(
        D3DDEVICE_CREATION_PARAMETERS* pParams) noexcept {
    if (!pParams) return D3DERR_INVALIDCALL;
    pParams->AdapterOrdinal  = adapter_;
    pParams->DeviceType      = deviceType_;
    pParams->hFocusWindow    = creationWindow_;
    pParams->BehaviorFlags   = behaviorFlags_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* surface) noexcept {
    dxmt9DeviceDebugLog("device_set_cursor_properties device=%p x=%u y=%u surface=%p",
                        this, x, y, surface);
    if (!surface) {
        return D3DERR_INVALIDCALL;
    }
    D3DSURFACE_DESC desc{};
    const HRESULT hr = surface->GetDesc(&desc);
    if (FAILED(hr)) {
        return hr;
    }
    const auto isPowerOfTwo = [](UINT value) noexcept -> bool {
        return value != 0 && (value & (value - 1u)) == 0;
    };
    if (desc.Format != D3DFMT_A8R8G8B8 ||
        !isPowerOfTwo(desc.Width) ||
        !isPowerOfTwo(desc.Height)) {
        return D3DERR_INVALIDCALL;
    }
    cursorSurfaceSet_ = true;
    return S_OK;
}

void    STDMETHODCALLTYPE D3D9DeviceImpl::SetCursorPosition(int x, int y, DWORD flags) noexcept {
    // stub: Wine returns S_OK; cursor positioning belongs to the WindowServer / window manager,
    // the app's hint is informational.
    dxmt9DeviceDebugLog("device_set_cursor_position device=%p x=%d y=%d flags=0x%x",
                        this, x, y, (unsigned)flags);
}

BOOL    STDMETHODCALLTYPE D3D9DeviceImpl::ShowCursor(BOOL show) noexcept {
    dxmt9DeviceDebugLog("device_show_cursor device=%p show=%u", this, (unsigned)show);
    if (!cursorSurfaceSet_) {
        return FALSE;
    }
    const BOOL previous = cursorVisible_ ? TRUE : FALSE;
    cursorVisible_ = show ? true : false;
    return previous;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateAdditionalSwapChain(
        D3DPRESENT_PARAMETERS* pPP, IDirect3DSwapChain9** ppSC) noexcept {
    if (!pPP || !ppSC) return D3DERR_INVALIDCALL;
    *ppSC = nullptr;
    // Present-parameter validation (same rule as CreateDevice / Reset):
    // invalid swap effect, BackBufferCount over the cap, COPY with > 1
    // back buffer, and undocumented presentation intervals are rejected
    // with D3DERR_INVALIDCALL before any swap chain is created.
    if (const HRESULT vhr = pePresentParamsHResult(pPP->SwapEffect,
            pPP->BackBufferCount, pPP->PresentationInterval,
            pPP->MultiSampleType, pPP->MultiSampleQuality, extended_);
        FAILED(vhr)) {
        return vhr;
    }
    D9CPresentParams cpp{};
    // minimal fill
    cpp.backBufferWidth  = pPP->BackBufferWidth;
    cpp.backBufferHeight = pPP->BackBufferHeight;
    cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
    cpp.backBufferCount  = pPP->BackBufferCount;
    cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
    // Wine d3d9: when the caller leaves hDeviceWindow NULL the
    // swap-chain inherits the device's focus window (the one that
    // owns the device). IDirect3DSwapChain9::GetPresentParameters
    // then reports the resolved window — see
    // test_additional_swapchain_backbuffer_bounds line 475.
    HWND effectiveDeviceWindow = pPP->hDeviceWindow ? pPP->hDeviceWindow
                                                   : creationWindow_;
    cpp.deviceWindow     = (uint64_t)(uintptr_t)effectiveDeviceWindow;
    cpp.windowed         = pPP->Windowed ? 1u : 0u;
    cpp.presentationInterval = pPP->PresentationInterval;
    D9CSwapChain* sc = dxmt9c_device_create_additional_swap_chain(dev_, &cpp);
    if (!sc) return D3DERR_INVALIDCALL;
    *ppSC = CreatePeSwapChain(sc, this, this, extended_, pPP->Flags);
    // Wine d3d9 post-create mutation of pPP (additional swapchain only):
    //   * BackBufferCount=0 normalises to 1 (clamped to a documented
    //     minimum) and is written back to the caller's struct.
    //   * hDeviceWindow is cleared on the additional swapchain's view
    //     of the present parameters — the swapchain's internal record
    //     still stores the device's focus window (which
    //     IDirect3DSwapChain9::GetPresentParameters reports), but the
    //     mutated pPP struct visible to the caller has hDeviceWindow
    //     zeroed.
    // Tests: test_additional_swapchain_backbuffer_bounds at
    // tests/conformance/d3d9/d3d9_conformance_swapchain.c:443-444.
    pPP->BackBufferCount = peNormalizeBackBufferCount(pPP->BackBufferCount);
    pPP->hDeviceWindow = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetSwapChain(UINT index,
                                                    IDirect3DSwapChain9** ppSC) noexcept {
    notePeDeviceCallAfterPresent("GetSwapChain");
    if (!ppSC) return D3DERR_INVALIDCALL;
    *ppSC = nullptr;
    if (auto it = swapchainWrappers_.find(index);
        it != swapchainWrappers_.end()) {
        it->second->AddRef();
        *ppSC = it->second;
        return S_OK;
    }
    D9CSwapChain* sc = dxmt9c_device_get_swap_chain(dev_, index);
    if (!sc) return D3DERR_INVALIDCALL;
    // index 0 = implicit swap chain; additional swap chains are
    // created with their own flags through CreateAdditionalSwapChain
    // and that path already sets the shadow. For lazy-created index-0
    // wrappers fall back to the device's captured implicit flags.
    const DWORD wrapperFlags = (index == 0) ? implicitSwapchainFlagsShadow_ : 0;
    auto* wrapper = CreatePeSwapChain(sc, this, this, extended_, wrapperFlags);
    wrapper->AddRef();  // device retains one ref in the cache
    swapchainWrappers_.emplace(index,
            static_cast<IDirect3DSwapChain9*>(wrapper));
    *ppSC = static_cast<IDirect3DSwapChain9*>(wrapper);
    return S_OK;
}

UINT STDMETHODCALLTYPE D3D9DeviceImpl::GetNumberOfSwapChains() noexcept {
    return dxmt9c_device_get_swap_chain_count(dev_);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::Reset(D3DPRESENT_PARAMETERS* pPP) noexcept {
    dxmt9PeSetCurrentCallName("Reset");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
    if (!pPP) return D3DERR_INVALIDCALL;
    // Present-parameter validation (same rule as CreateDevice): invalid
    // swap effect, BackBufferCount over the cap, COPY with > 1 back
    // buffer, and undocumented presentation intervals are rejected with
    // D3DERR_INVALIDCALL before any device state is torn down.
    if (const HRESULT vhr = pePresentParamsHResult(pPP->SwapEffect,
            pPP->BackBufferCount, pPP->PresentationInterval,
            pPP->MultiSampleType, pPP->MultiSampleQuality, extended_);
        FAILED(vhr)) {
        return vhr;
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
    const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::Reset);
    if (FAILED(flushHr)) return flushHr;
    releaseAllBound();
    if (defaultPoolResourceRefs_ != 0) {
        clearPeStateTracking();
        stateBlockRecording_ = false;
        peState_.stateBlockRenderStateRestore.clear();
        peState_.stateBlockTransformRestore.clear();
        peState_.stateBlockTransformRecorded.clear();
        peState_.stateBlockVdeclRecorded = false;
        deviceNotReset_ = true;
        return D3DERR_INVALIDCALL;
    }
    clearPeStateTracking();
    stateBlockRecording_ = false;
    peState_.stateBlockRenderStateRestore.clear();
    peState_.stateBlockTransformRestore.clear();
    peState_.stateBlockTransformRecorded.clear();
    peState_.stateBlockVdeclRecorded = false;
    const HRESULT hr = hr32(dxmt9c_device_reset(dev_, &cpp));
    if (SUCCEEDED(hr)) {
        deviceNotReset_ = false;
        // reset_lockable_backbuffer_policy: capture the new
        // PresentParameters.Flags so future GetSwapChain wrapper
        // creations see the updated value.
        implicitSwapchainFlagsShadow_ = pPP->Flags;
        // T2: per Wine d3d9_device_Reset, viewport and scissor must
        // be set to {0, 0, BackBufferWidth, BackBufferHeight, 0, 1}
        // after a successful Reset. The core::Device already sets
        // its server-side viewport in resetValidated() — mirror it
        // into the PE shadow so the next draw packet carries fresh
        // viewport/scissor instead of stale pre-reset values.
        const uint32_t w = std::max<uint32_t>(1u, cpp.backBufferWidth);
        const uint32_t h = std::max<uint32_t>(1u, cpp.backBufferHeight);
        peState_.viewportShadow = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
        peState_.scissorShadow  = D9CRect{0, 0, (int32_t)w, (int32_t)h};
        peState_.pendingViewport = false;
        peState_.pendingScissor  = false;
    }
    if (renderTapeCapture_ &&
        renderTapeCapture_->state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        const dxmt9::d3d9::RenderTapeResetControl payload{
            .reclaimedGeneration = 0u, .terminal = 1u};
        NotifyRenderTapeOrderedControlForChild(
            dxmt9::d3d9::RenderTapeOrderedControlHeader{
                .kind = static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeControlKind::Reset),
                .disposition = static_cast<std::uint32_t>(
                    SUCCEEDED(hr)
                        ? dxmt9::d3d9::RenderTapeControlDisposition::Terminal
                        : dxmt9::d3d9::RenderTapeControlDisposition::Failed),
                .resultCode = static_cast<std::int32_t>(hr),
                .controlBytes = sizeof(payload)},
            std::as_bytes(std::span(&payload, 1u)));
        abortRenderTapeCapture("reset");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetBackBuffer(UINT sc, UINT idx,
                                                     D3DBACKBUFFER_TYPE type,
                                                     IDirect3DSurface9** ppS) noexcept {
    notePeDeviceCallAfterPresent("GetBackBuffer", DXMT9_PE_CALLSITE_PC());
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    dxmt9DeviceDebugLog("device_get_back_buffer device=%p sc=%u idx=%u", this, sc, idx);
    D9CSwapChain* chain = borrowSwapChainHandle(sc);
    if (!chain) return D3DERR_INVALIDCALL;
    // Wine d3d9 test_swapchain_parameters: GetBackBuffer with an
    // index meeting or exceeding the swapchain's BackBufferCount
    // returns D3DERR_INVALIDCALL — *ppS must remain NULL on
    // failure, but cppcheck'd test asserts the pointer is left
    // untouched at the deadbeef sentinel only on the swapchain
    // path; the device path's spec resets it to NULL above and
    // expects NULL back.
    D9CPresentParams cppGuard{};
    if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(chain, &cppGuard)))) {
        if (idx >= cppGuard.backBufferCount) {
            return D3DERR_INVALIDCALL;
        }
    }
    // The former dxmt9c_swapchain_get_back_buffer + surface_release probe
    // that stood here is gone: it allocated and immediately dropped a unix
    // surface handle purely to map "no such back buffer" to
    // D3DERR_INVALIDCALL, which the swap-chain wrapper's own
    // GetBackBuffer below does with the identical null check and the
    // identical HRESULT, without disturbing *ppS (already NULL).
    // Wine d3d9 contract: device-level GetBackBuffer must return
    // the same COM wrapper as the matching swap-chain GetBackBuffer
    // for any (sc, idx). Route through the cached swap-chain
    // wrapper so its per-idx back-buffer cache is the single source
    // of truth. See test_swapchain_backbuffer_getter_policy lines
    // 371/379 — the assertion typed == backbuffer requires this
    // identity across the device-level and swap-chain-level calls.
    IDirect3DSwapChain9* swapchain = nullptr;
    const HRESULT swapHr = GetSwapChain(sc, &swapchain);
    if (FAILED(swapHr) || !swapchain) return swapHr;
    const HRESULT bbHr = swapchain->GetBackBuffer(idx, type, ppS);
    swapchain->Release();
    if (sc == 0 && idx == 0 && SUCCEEDED(bbHr) && *ppS) {
        // Preserve the pre-existing cachedBackBuffer0_ alias used by
        // the rest of the device implementation (e.g. resetState
        // explicit-clear paths) without holding an extra reference
        // beyond the swap-chain cache.
        setRef(cachedBackBuffer0_, *ppS);
    }
    return bbHr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetRasterStatus(UINT swapChain, D3DRASTER_STATUS* p) noexcept {
    notePeDeviceCallAfterPresent("GetRasterStatus");
    if (!p || swapChain != 0) {
        return D3DERR_INVALIDCALL;
    }
    // Synthesize a monotonically-advancing ScanLine so apps that VBlank-poll do
    // not spin forever. dxmt9 has no real per-line vblank signal from Metal;
    // the helper takes a per-call counter and the current backbuffer height.
    static std::atomic<uint64_t> rasterTick{0};
    uint32_t displayHeight = 0;
    D9CSwapChain* chain = borrowSwapChainHandle(swapChain);
    if (chain) {
        D9CPresentParams cpp{};
        if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(chain, &cpp)))) {
            displayHeight = cpp.backBufferHeight;
        }
    }
    const auto tick = rasterTick.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto est = ::dxmt9::d3d9::computeRasterStatusEstimate(tick, displayHeight);
    memset(p, 0, sizeof(*p));
    p->ScanLine = est.scanLine;
    p->InVBlank = est.inVBlank ? TRUE : FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetDialogBoxMode(BOOL enableDialogs) noexcept {
    dxmt9DeviceDebugLog("device_set_dialog_box_mode device=%p enable=%u", this, (unsigned)enableDialogs);
    // stub: Wine `wined3d_device_set_dialog_box_mode` returns WINED3D_OK
    // unconditionally — dialog-box mode requires Win32 user32/dwm primitives
    // that don't exist on macOS, but matching the Wine S_OK contract keeps
    // the conformance manifest aligned. Toggling has no observable effect.
    return S_OK;
}

void    STDMETHODCALLTYPE D3D9DeviceImpl::SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp) noexcept {
    dxmt9DeviceDebugLog("device_set_gamma_ramp device=%p swapChain=%u flags=0x%x ramp=%p",
                        this, swapChain, (unsigned)flags,
                        static_cast<const void*>(ramp));
    if (!ramp) return;
    const bool captureGamma =
        renderTapeCapture_ &&
        renderTapeCapture_->state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing;
    if (captureGamma) {
        // A direct state mutation is an ordering boundary. Seal any
        // pending draw/state chunk before journaling GammaRampSet so the
        // provider cannot apply the new LUT ahead of older work.
        const HRESULT flushHr =
            flushPeRecorder(PeRecorderFlushReason::Barrier);
        if (FAILED(flushHr)) {
            abortRenderTapeCapture("gamma_ramp_barrier");
        }
    }
    // Byte-copy: D3DGAMMARAMP is a POD (3 * 256 * WORD). sizeof
    // is the safe shape regardless of any future struct growth.
    std::memcpy(&gammaRamp_, ramp, sizeof(D3DGAMMARAMP));
    if (dev_) {
        dxmt9c_device_set_gamma_ramp(dev_, reinterpret_cast<const uint16_t*>(ramp));
    }
    if (captureGamma && renderTapeCapture_ &&
        renderTapeCapture_->state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        const auto *bytes = reinterpret_cast<const std::byte *>(ramp);
        NotifyRenderTapeOrderedControlForChild(
            dxmt9::d3d9::RenderTapeOrderedControlHeader{
                .identity = {},
                .kind = static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeControlKind::GammaRampSet),
                .disposition = static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeControlDisposition::Completed),
                .resultCode = 0,
                .controlBytes = dxmt9::d3d9::kRenderTapeGammaRampBytes,
            },
            std::span<const std::byte>(
                bytes, dxmt9::d3d9::kRenderTapeGammaRampBytes));
    }
}

void    STDMETHODCALLTYPE D3D9DeviceImpl::GetGammaRamp(UINT swapChain, D3DGAMMARAMP* p) noexcept {
    dxmt9DeviceDebugLog("device_get_gamma_ramp device=%p swapChain=%u out=%p",
                        this, swapChain, static_cast<void*>(p));
    if (!p) return;
    std::memcpy(p, &gammaRamp_, sizeof(D3DGAMMARAMP));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateTexture(UINT w, UINT h, UINT levels,
                                                     DWORD usage, D3DFORMAT fmt,
                                                     D3DPOOL pool,
                                                     IDirect3DTexture9** ppTex,
                                                     HANDLE* psh) noexcept {
    if (!ppTex) return D3DERR_INVALIDCALL;
    *ppTex = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    const HRESULT levelHr = peTextureLevelCountHResult(std::min(w, h), std::max(w, h), levels);
    if (FAILED(levelHr)) return levelHr;
    // Wine D3D9Ex contract from dlls/d3d9/tests/d3d9ex.c
    // test_resource_access: D3DPOOL_MANAGED is rejected outright on
    // Ex devices (every MANAGED row in the matrix is marked
    // valid=FALSE, regardless of usage / format). Scaffold:
    // tests/conformance/d3d9/d3d9_conformance_device.c
    // test_resource_access_ex_pool_policy.
    if (extended_ && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, true);
    if (FAILED(sharedHr)) return sharedHr;
    // T4 (D3D9Ex shared-handle, SYSTEMMEM partial): SYSTEMMEM 1-mip
    // 2D texture with pSharedHandle aliases caller-supplied memory.
    // Wine d3d9ex test_user_memory line 769-778 accepts arbitrary
    // widths/heights for this path; the only constraint is single
    // mip level (validateSharedHandleForTexture already enforced).
    // bytesPerPixel == 0 means "format we cannot alias" — reject.
    const bool useUserMemory =
        extended_ && psh && pool == D3DPOOL_SYSTEMMEM && levels == 1;
    void* userPtr = nullptr;
    int32_t userPitch = 0;
    if (useUserMemory) {
        const uint32_t bpp = userMemoryBytesPerPixel(fmt);
        if (bpp == 0) return D3DERR_INVALIDCALL;
        userPtr = *psh;
        userPitch = static_cast<int32_t>(bpp * w);
    }
    dxmt9DeviceDebugLog("device_create_texture device=%p size=%ux%u levels=%u usage=0x%x fmt=%u pool=%u user=%p",
                        this, w, h, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool,
                        userPtr);
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CTexture* t = dxmt9c_device_create_texture_shared(dev_, w, h, levels,
                                                  usage, (uint32_t)fmt,
                                                  (uint32_t)pool,
                                                  providerShared);
    if (!t) return D3DERR_INVALIDCALL;
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    *ppTex = CreatePeTexture(t, this, this, userPtr, userPitch);
    notifyRenderTapeCreatedTexture(
        t, D3D9PeWireTexture(*ppTex),
        dxmt9::d3d9::RenderTapeTextureDimension::Texture2D);
    // Wine base_vidmem_accounting_policy expects a strictly-decreasing
    // budget on non-Ex devices; ex_vidmem_accounting_policy expects
    // the value to stay roughly constant for D3D9Ex devices (the spec
    // reports an "unlimited" budget for Ex). Only charge non-Ex.
    if (!extended_) {
        vidmemBytesUsedShadow_ += static_cast<uint64_t>(w) * h
            * std::max<uint32_t>(1u, levels) * 4u;
    }
    dxmt9DeviceDebugLog("device_create_texture -> texture=%p", *ppTex);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateVolumeTexture(UINT w, UINT h, UINT d,
                                                           UINT levels, DWORD usage,
                                                           D3DFORMAT fmt, D3DPOOL pool,
                                                           IDirect3DVolumeTexture9** ppTex,
                                                           HANDLE* psh) noexcept {
    if (!ppTex) return D3DERR_INVALIDCALL;
    *ppTex = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    const HRESULT levelHr =
        peTextureLevelCountHResult(std::min({w, h, d}), std::max({w, h, d}), levels);
    if (FAILED(levelHr)) return levelHr;
    // Wine D3D9Ex contract: see CreateTexture above — MANAGED pool
    // is rejected outright on Ex devices.
    if (extended_ && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_volume_texture device=%p size=%ux%ux%u levels=%u usage=0x%x fmt=%u pool=%u",
                        this, w, h, d, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CTexture* t = dxmt9c_device_create_volume_texture_shared(dev_, w, h, d, levels,
                                                         usage, (uint32_t)fmt,
                                                         (uint32_t)pool,
                                                         providerShared);
    if (!t) return D3DERR_INVALIDCALL;
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    *ppTex = CreatePeVolumeTexture(t, this, this);
    notifyRenderTapeCreatedTexture(
        t, D3D9PeWireTexture(*ppTex),
        dxmt9::d3d9::RenderTapeTextureDimension::Volume);
    dxmt9DeviceDebugLog("device_create_volume_texture -> texture=%p", *ppTex);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateCubeTexture(UINT size, UINT levels,
                                                         DWORD usage, D3DFORMAT fmt,
                                                         D3DPOOL pool,
                                                         IDirect3DCubeTexture9** ppTex,
                                                         HANDLE* psh) noexcept {
    if (!ppTex) return D3DERR_INVALIDCALL;
    *ppTex = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    const HRESULT levelHr = peTextureLevelCountHResult(size, size, levels);
    if (FAILED(levelHr)) return levelHr;
    // Wine D3D9Ex contract: see CreateTexture above — MANAGED pool
    // is rejected outright on Ex devices.
    if (extended_ && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_cube_texture device=%p size=%u levels=%u usage=0x%x fmt=%u pool=%u",
                        this, size, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CTexture* t = dxmt9c_device_create_cube_texture_shared(dev_, size, levels,
                                                       usage, (uint32_t)fmt,
                                                       (uint32_t)pool,
                                                       providerShared);
    if (!t) return D3DERR_INVALIDCALL;
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    *ppTex = CreatePeCubeTexture(t, this, this);
    notifyRenderTapeCreatedTexture(
        t, D3D9PeWireTexture(*ppTex),
        dxmt9::d3d9::RenderTapeTextureDimension::Cube);
    dxmt9DeviceDebugLog("device_create_cube_texture -> texture=%p", *ppTex);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateVertexBuffer(UINT len, DWORD usage,
                                                          DWORD fvf, D3DPOOL pool,
                                                          IDirect3DVertexBuffer9** ppBuf,
                                                          HANDLE* psh) noexcept {
    if (!ppBuf) return D3DERR_INVALIDCALL;
    *ppBuf = nullptr;
    // Wine D3D9 (test_vertex_buffer_desc_binding_policy line 2887):
    // SCRATCH pool is invalid for vertex buffers; Wine returns
    // D3DERR_INVALIDCALL.
    if (pool == D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForBuffer(extended_, psh, pool);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_vertex_buffer device=%p len=%u usage=0x%x fvf=0x%x pool=%u",
                        this, len, (unsigned)usage, (unsigned)fvf, (unsigned)pool);
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CBuffer* b = dxmt9c_device_create_vertex_buffer_shared(dev_, len, usage,
                                                       fvf, (uint32_t)pool,
                                                       providerShared);
    if (!b) return D3DERR_INVALIDCALL;
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    *ppBuf = CreatePeVertexBuffer(b, this, this);
    notifyRenderTapeCreatedBuffer(b, D3D9PeWireVertexBuffer(*ppBuf));
    dxmt9DeviceDebugLog("device_create_vertex_buffer -> buffer=%p", *ppBuf);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateIndexBuffer(UINT len, DWORD usage,
                                                         D3DFORMAT fmt, D3DPOOL pool,
                                                         IDirect3DIndexBuffer9** ppBuf,
                                                         HANDLE* psh) noexcept {
    if (!ppBuf) return D3DERR_INVALIDCALL;
    *ppBuf = nullptr;
    // Wine D3D9 (test_index_buffer_desc_binding_policy line 2768):
    // SCRATCH pool is invalid for index buffers; Wine returns
    // D3DERR_INVALIDCALL.
    if (pool == D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForBuffer(extended_, psh, pool);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_index_buffer device=%p len=%u usage=0x%x fmt=%u pool=%u",
                        this, len, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CBuffer* b = dxmt9c_device_create_index_buffer_shared(dev_, len, usage,
                                                      (uint32_t)fmt,
                                                      (uint32_t)pool,
                                                      providerShared);
    if (!b) return D3DERR_INVALIDCALL;
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    *ppBuf = CreatePeIndexBuffer(b, this, this);
    notifyRenderTapeCreatedBuffer(b, D3D9PeWireIndexBuffer(*ppBuf));
    dxmt9DeviceDebugLog("device_create_index_buffer -> buffer=%p", *ppBuf);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt,
                                                          D3DMULTISAMPLE_TYPE ms,
                                                          DWORD msQual, BOOL lockable,
                                                          IDirect3DSurface9** ppS,
                                                          HANDLE* psh) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    // create_rt_ds_failure_policy: depth-stencil formats cannot be RTs.
    if (fmt == D3DFMT_D16 || fmt == D3DFMT_D24X8 || fmt == D3DFMT_D24S8
            || fmt == D3DFMT_D32 || fmt == D3DFMT_D15S1
            || fmt == D3DFMT_D24X4S4 || fmt == D3DFMT_D24FS8
            || fmt == D3DFMT_D32F_LOCKABLE || fmt == D3DFMT_D16_LOCKABLE
            || fmt == D3DFMT_D32_LOCKABLE) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT sharedHr = validateSharedHandleForDefaultSurface(extended_, psh);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_render_target device=%p size=%ux%u fmt=%u ms=%u msQual=%u lockable=%u",
                        this, w, h, (unsigned)fmt, (unsigned)ms, (unsigned)msQual, (unsigned)lockable);
    // Wine d3d9 test_invalid_multisample: CreateRenderTarget probes
    // CheckDeviceMultiSampleType to validate the (sampleCount,
    // quality) pair before dispatching the allocation. Mapping:
    //   - D3DMULTISAMPLE_NONE + quality>=1 → INVALIDCALL
    //   - CheckDeviceMultiSampleType returns NOTAVAILABLE → INVALIDCALL
    //   - quality >= reportedLevels → INVALIDCALL
    // The backend allocator silently dropped quality, so the
    // quality-vs-reported check has to live at the PE boundary.
    if (ms == D3DMULTISAMPLE_NONE && msQual != 0) {
        return D3DERR_INVALIDCALL;
    }
    if (ms != D3DMULTISAMPLE_NONE && factory_) {
        DWORD reportedQuality = 0;
        const HRESULT msHr = factory_->CheckDeviceMultiSampleType(
            adapter_, deviceType_, fmt, /*windowed=*/FALSE, ms,
            &reportedQuality);
        if (FAILED(msHr)) {
            return D3DERR_INVALIDCALL;
        }
        if (msQual >= reportedQuality) {
            return D3DERR_INVALIDCALL;
        }
    }
    uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared = extended_ && psh ? &sh : nullptr;
    D9CSurface* s = dxmt9c_device_create_render_target(dev_, w, h,
                                                        (uint32_t)fmt,
                                                        (uint32_t)ms, msQual,
                                                        lockable ? 1u : 0u,
                                                        providerShared);
    if (!s) return D3DERR_INVALIDCALL;
    if (providerShared) *psh = (HANDLE)(uintptr_t)sh;
    *ppS = CreatePeSurface(s, this, nullptr, this);
    dxmt9DeviceDebugLog("device_create_render_target -> surface=%p", *ppS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateDepthStencilSurface(UINT w, UINT h,
                                                                 D3DFORMAT fmt,
                                                                 D3DMULTISAMPLE_TYPE ms,
                                                                 DWORD msQual,
                                                                 BOOL discard,
                                                                 IDirect3DSurface9** ppS,
                                                                 HANDLE* psh) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    // create_rt_ds_failure_policy: only depth-stencil formats are valid
    // as a DS surface. Colour formats must reject with INVALIDCALL.
    const bool isDepthFormat =
        fmt == D3DFMT_D16 || fmt == D3DFMT_D24X8 || fmt == D3DFMT_D24S8
        || fmt == D3DFMT_D32 || fmt == D3DFMT_D15S1
        || fmt == D3DFMT_D24X4S4 || fmt == D3DFMT_D24FS8
        || fmt == D3DFMT_D32F_LOCKABLE || fmt == D3DFMT_D16_LOCKABLE
        || fmt == D3DFMT_D32_LOCKABLE;
    if (!isDepthFormat) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForDefaultSurface(extended_, psh);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_depth_stencil_surface device=%p size=%ux%u fmt=%u ms=%u msQual=%u discard=%u",
                        this, w, h, (unsigned)fmt, (unsigned)ms, (unsigned)msQual, (unsigned)discard);
    uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared = extended_ && psh ? &sh : nullptr;
    D9CSurface* s = dxmt9c_device_create_depth_stencil(dev_, w, h,
                                                        (uint32_t)fmt,
                                                        (uint32_t)ms, msQual,
                                                        discard ? 1u : 0u,
                                                        providerShared);
    if (!s) return D3DERR_INVALIDCALL;
    if (providerShared) *psh = (HANDLE)(uintptr_t)sh;
    *ppS = CreatePeSurface(s, this, nullptr, this);
    dxmt9DeviceDebugLog("device_create_depth_stencil_surface -> surface=%p", *ppS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::UpdateSurface(IDirect3DSurface9* src,
                                                     const RECT* srcRect,
                                                     IDirect3DSurface9* dst,
                                                     const POINT* dstPt) noexcept {
    dxmt9PeSetCurrentCallName("UpdateSurface");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
    dxmt9DeviceDebugLog("device_update_surface device=%p src=%p dst=%p srcRect=%s dstPt=%s",
                        this, src, dst,
                        srcRect ? "<custom>" : "<full>",
                        dstPt ? "<custom>" : "<origin>");
    // Wine d3d9 conformance (test_mipmap_surface_update_lock_policy):
    // UpdateSurface requires the source to be SYSTEMMEM, the
    // destination to be DEFAULT, matching formats, and the source
    // not be currently locked. wined3d's
    // device_update_surface validates all four invariants before
    // initiating the copy; mirroring them here keeps a malformed
    // call from generating an upload record.
    if (!src || !dst) {
        return D3DERR_INVALIDCALL;
    }
    D3DSURFACE_DESC srcDesc{};
    D3DSURFACE_DESC dstDesc{};
    if (FAILED(src->GetDesc(&srcDesc)) || FAILED(dst->GetDesc(&dstDesc))) {
        return D3DERR_INVALIDCALL;
    }
    if (srcDesc.Pool != D3DPOOL_SYSTEMMEM ||
        dstDesc.Pool != D3DPOOL_DEFAULT) {
        return D3DERR_INVALIDCALL;
    }
    if (srcDesc.Format != dstDesc.Format) {
        return D3DERR_INVALIDCALL;
    }
    if (D3D9PeSurfaceIsLocked(src) || D3D9PeSurfaceIsLocked(dst)) {
        return D3DERR_INVALIDCALL;
    }
    if (srcRect) {
        if (srcRect->left < 0 || srcRect->top < 0 ||
            srcRect->right <= srcRect->left ||
            srcRect->bottom <= srcRect->top ||
            static_cast<UINT>(srcRect->right) > srcDesc.Width ||
            static_cast<UINT>(srcRect->bottom) > srcDesc.Height) {
            return D3DERR_INVALIDCALL;
        }
    }
    {
        const UINT copyW = srcRect ? static_cast<UINT>(srcRect->right - srcRect->left)
                                   : srcDesc.Width;
        const UINT copyH = srcRect ? static_cast<UINT>(srcRect->bottom - srcRect->top)
                                   : srcDesc.Height;
        if (dstPt) {
            if (dstPt->x < 0 || dstPt->y < 0 ||
                static_cast<UINT>(dstPt->x) + copyW > dstDesc.Width ||
                static_cast<UINT>(dstPt->y) + copyH > dstDesc.Height) {
                return D3DERR_INVALIDCALL;
            }
        } else if (copyW > dstDesc.Width || copyH > dstDesc.Height) {
            return D3DERR_INVALIDCALL;
        }
    }
    D9CRect cs{}, cd{};
    if (srcRect) cs = toR(*srcRect);
    if (dstPt) { cd.left = dstPt->x; cd.top = dstPt->y;
                 cd.right = dstPt->x; cd.bottom = dstPt->y; }
    // Fire-and-forget copy records stay queued until the normal chunk
    // boundary. The raw D9C wrappers are AddRef'd by the pending chunk
    // so callers may release their D3D9 wrappers immediately.
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    // Handle indices are assigned by appendUpdateSurface as it appends
    // the refs, so they stay zero here.
    const D9CCommandChunkWireUpdateSurface wire{
        .srcHandleIndex = 0u,
        .dstHandleIndex = 0u,
        .hasSrcRect = srcRect ? 1u : 0u,
        .hasDstPoint = dstPt ? 1u : 0u,
        .srcRect = cs,
        .dstPoint = cd,
    };
    return appendRecord(
        D9C_COMMAND_RECORD_UPDATE_SURFACE,
        kLegacyUpdateSurfaceSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = AppendPhaseTimer::now();
            const bool ok = dxmt9::d3d9::pe::appendUpdateSurface(
                builder, wire, D3D9PeWireSurface(src),
                D3D9PeWireSurface(dst));
            phase.record(peAppendPhaseEncode_, t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::UpdateTexture(IDirect3DBaseTexture9* src,
                                                     IDirect3DBaseTexture9* dst) noexcept {
    dxmt9PeSetCurrentCallName("UpdateTexture");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
    // Wine d3d9 IDirect3DDevice9::UpdateTexture: both args non-NULL;
    // src must be SYSTEMMEM; dst must NOT be SYSTEMMEM/SCRATCH. See
    // test_update_texture_pool_copy_2d in d3d9_conformance_resource.c.
    if (!src || !dst) return D3DERR_INVALIDCALL;
    bool palettizedUpdate = false;
    if (src->GetType() == D3DRTYPE_TEXTURE && dst->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC sd{}, dd{};
        ((IDirect3DTexture9*)src)->GetLevelDesc(0, &sd);
        ((IDirect3DTexture9*)dst)->GetLevelDesc(0, &dd);
        if (sd.Pool != D3DPOOL_SYSTEMMEM) return D3DERR_INVALIDCALL;
        if (dd.Pool == D3DPOOL_SYSTEMMEM || dd.Pool == D3DPOOL_SCRATCH)
            return D3DERR_INVALIDCALL;
        palettizedUpdate =
            (sd.Format == D3DFMT_P8 || sd.Format == D3DFMT_A8P8) &&
            sd.Format == dd.Format;
    }
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    const auto sourceWire = D3D9PeWireTexture(src);
    const auto destinationWire = D3D9PeWireTexture(dst);
    // Wine d3d9 UpdateTexture: both args non-NULL; src in SYSTEMMEM;
    // dst not SYSTEMMEM/SCRATCH. test_update_texture_pool_copy_2d.
    const HRESULT appendHr = appendRecord(
        D9C_COMMAND_RECORD_UPDATE_TEXTURE,
        kLegacyUpdateTextureSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = AppendPhaseTimer::now();
            const bool ok = dxmt9::d3d9::pe::appendUpdateTexture(
                builder, sourceWire, destinationWire);
            phase.record(peAppendPhaseEncode_, t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    // The registry shadow is committed only after appendRecord accepts the
    // record. If the append's capacity flush or emitter fails, the
    // destination remains unchanged; the normal command bytes are also
    // left on their existing failure path.
    if (SUCCEEDED(appendHr)) {
        applyRenderTapeUpdateTextureClosure(sourceWire, destinationWire);
    }
    if (FAILED(appendHr) || !palettizedUpdate) {
        return appendHr;
    }
    // P8/A8P8 resources keep CPU-visible index/alpha shadow state while
    // sampling through an expanded A8R8G8B8 backing. Make the shadow copy
    // visible immediately so CPU ProcessVertices vertex-texture TEXLDL can
    // observe a preceding UpdateTexture without waiting for a later draw.
    const HRESULT flushHr = flushPendingCommandChunk(PeRecorderFlushReason::Readback);
    if (FAILED(flushHr)) return flushHr;
    // The immediate commit may run before a later SetTexture applies the
    // device-current palette to this destination. Re-expand now as well,
    // so fixed-function/programmable draws and CPU samplers see the same
    // palette if they sample the destination right after UpdateTexture.
    applyCurrentPaletteToTexture(dst);
    return appendHr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetRenderTargetData(IDirect3DSurface9* rt,
                                                           IDirect3DSurface9* dst) noexcept {
    dxmt9PeSetCurrentCallName("GetRenderTargetData");
    auto peCadence = claimPeFirstCallAfterPresent();
    const void* callerPc = DXMT9_PE_CALLSITE_PC();
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
    // Decomposed on purpose: the cadence claim above must be taken before
    // the recorder lock and the milestone log after it, so this site cannot
    // use notePeDeviceCallAfterPresent. Gating the pair keeps the sample
    // inside the branch, so the disabled path constructs nothing. When
    // tracking is off both calls were already no-ops -- the claim is
    // unclaimed and the milestone sample untracked -- so skipping them
    // changes no emission.
    if (dxmt9PeCallTrackingEnabled()) {
        const auto peCall =
            logPeCallMilestoneAfterPresent("GetRenderTargetData", callerPc);
        logPeFirstCallAfterPresent("GetRenderTargetData", peCadence,
                                   peCall);
    }
    dxmt9DeviceDebugLog("device_get_render_target_data device=%p rt=%p dst=%p",
                        this, rt, dst);
    // get_render_target_data_policy: both args must be non-NULL. Wine
    // rejects NULL with INVALIDCALL before any backend work.
    if (!rt || !dst) return D3DERR_INVALIDCALL;
    // Wine d3d9 multisample_get_rtdata_test (visual.c:17106) contract:
    // GetRenderTargetData cannot copy from a multisampled render target
    // into a SYSTEMMEM offscreen surface — there is no way to express
    // per-sample data in a single sysmem surface. The legal readback
    // path is StretchRect (MSAA -> non-MSAA resolve) followed by
    // GetRenderTargetData on the resolved RT.
    // get_render_target_data_msaa_policy.
    {
        D3DSURFACE_DESC sdSrc{};
        if (SUCCEEDED(rt->GetDesc(&sdSrc))) {
            if (sdSrc.MultiSampleType != D3DMULTISAMPLE_NONE) {
                return D3DERR_INVALIDCALL;
            }
        }
    }
    // Phase 24: chunk-recorder path. The PE caller is synchronous —
    // the call doesn't return until the data is in dst — but
    // routing through the chunk record stream keeps ordering atomic
    // with surrounding draws/clears in the SAME chunk. We append a
    // READBACK record then commit the chunk synchronously (Present
    // pattern); commit_chunk's per-record short-circuit propagates
    // the actual readback HRESULT back to PE.
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    const HRESULT appendHr = appendRecord(
        D9C_COMMAND_RECORD_READBACK, kLegacyReadbackSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = AppendPhaseTimer::now();
            const bool ok = dxmt9::d3d9::pe::appendReadback(
                builder, D3D9PeWireSurface(rt), D3D9PeWireSurface(dst));
            phase.record(peAppendPhaseEncode_, t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (FAILED(appendHr)) return appendHr;
    // Sync semantics: commit the chunk now and wait for completion.
    // flushPendingCommandChunk routes through commit_chunk -> server's
    // record dispatcher -> readback record handler.
    return flushPendingCommandChunk(PeRecorderFlushReason::Readback);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetFrontBufferData(UINT sc, IDirect3DSurface9* surface) noexcept {
    dxmt9DeviceDebugLog("device_get_front_buffer_data device=%p sc=%u surface=%p",
                        this, sc, surface);
    if (!surface) return D3DERR_INVALIDCALL;
    IDirect3DSwapChain9* swapchain = nullptr;
    const HRESULT swapHr = GetSwapChain(sc, &swapchain);
    if (FAILED(swapHr) || !swapchain) return swapHr;
    const HRESULT hr = swapchain->GetFrontBufferData(surface);
    swapchain->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::StretchRect(IDirect3DSurface9* src,
                                                   const RECT* srcRect,
                                                   IDirect3DSurface9* dst,
                                                   const RECT* dstRect,
                                                   D3DTEXTUREFILTERTYPE filter) noexcept {
    dxmt9PeSetCurrentCallName("StretchRect");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
    dxmt9DeviceDebugLog("device_stretch_rect device=%p src=%p dst=%p filter=%u srcRect=%s dstRect=%s",
                        this, src, dst, (unsigned)filter,
                        srcRect ? "<custom>" : "<full>",
                        dstRect ? "<custom>" : "<full>");
    // Wine d3d9 test_stretch_rect contract: NULL src or NULL dst, and
    // degenerate / negative-extent src or dst rects, are rejected with
    // D3DERR_INVALIDCALL before any wire-record dispatch
    // (stretch_rect_null_and_degenerate_policy).
    if (!src || !dst) return D3DERR_INVALIDCALL;
    if (srcRect && (srcRect->right <= srcRect->left ||
                    srcRect->bottom <= srcRect->top)) {
        return D3DERR_INVALIDCALL;
    }
    if (dstRect && (dstRect->right <= dstRect->left ||
                    dstRect->bottom <= dstRect->top)) {
        return D3DERR_INVALIDCALL;
    }
    // Wine d3d9 depth_blit_test (visual.c:14713) contract: when either
    // surface is a depth-stencil format, the other must be the same
    // depth-stencil format. Cross-format depth blits (e.g. D24S8 -> D16)
    // and depth -> color blits are rejected with INVALIDCALL.
    // stretch_rect_depth_stencil_policy.
    {
        D3DSURFACE_DESC sdSrc{};
        D3DSURFACE_DESC sdDst{};
        const bool gotSrc = SUCCEEDED(src->GetDesc(&sdSrc));
        const bool gotDst = SUCCEEDED(dst->GetDesc(&sdDst));
        auto isDepth = [](D3DFORMAT f) {
            return f == D3DFMT_D16 || f == D3DFMT_D24X8 ||
                   f == D3DFMT_D24S8 || f == D3DFMT_D32 ||
                   f == D3DFMT_D15S1 || f == D3DFMT_D24X4S4 ||
                   f == D3DFMT_D24FS8 || f == D3DFMT_D32F_LOCKABLE ||
                   f == D3DFMT_D16_LOCKABLE || f == D3DFMT_D32_LOCKABLE;
        };
        if (gotSrc && gotDst) {
            const bool srcIsDepth = isDepth(sdSrc.Format);
            const bool dstIsDepth = isDepth(sdDst.Format);
            if ((srcIsDepth || dstIsDepth) &&
                sdSrc.Format != sdDst.Format) {
                return D3DERR_INVALIDCALL;
            }
        }
        // Wine d3d9 test_format_conversion (visual.c:27960) contract:
        // StretchRect does not perform block-compressed <-> linear
        // conversion. If either surface is a DXT* compressed format,
        // the other surface must have the same compressed format.
        // (DXT1 -> A8R8G8B8 must fail with D3DERR_INVALIDCALL.)
        // stretch_rect_format_conversion_policy.
        if (gotSrc && gotDst) {
            auto isCompressed = [](D3DFORMAT f) {
                return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 ||
                       f == D3DFMT_DXT3 || f == D3DFMT_DXT4 ||
                       f == D3DFMT_DXT5;
            };
            const bool srcIsCompressed = isCompressed(sdSrc.Format);
            const bool dstIsCompressed = isCompressed(sdDst.Format);
            if ((srcIsCompressed || dstIsCompressed) &&
                sdSrc.Format != sdDst.Format) {
                return D3DERR_INVALIDCALL;
            }
        }
        // Wine d3d9 test_multisample_stretch_rect (visual.c:4494)
        // contract: the destination of a StretchRect must not be
        // multisampled. MSAA -> non-MSAA is the D3D9 resolve idiom
        // and is allowed; non-MSAA -> MSAA and MSAA -> MSAA blits
        // are rejected with D3DERR_INVALIDCALL.
        // stretch_rect_multisample_resolve_policy.
        if (gotSrc && gotDst) {
            if (sdDst.MultiSampleType != D3DMULTISAMPLE_NONE) {
                return D3DERR_INVALIDCALL;
            }
        }
    }
    D9CRect cs{}, cd{};
    if (srcRect) cs = toR(*srcRect); if (dstRect) cd = toR(*dstRect);
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    const D9CCommandChunkWireStretchRect wire{
        .srcHandleIndex = 0u,
        .dstHandleIndex = 0u,
        .hasSrcRect = srcRect ? 1u : 0u,
        .hasDstRect = dstRect ? 1u : 0u,
        .filter = (uint32_t)filter,
        .reserved0 = 0u,
        .srcRect = srcRect ? cs : D9CRect{},
        .dstRect = dstRect ? cd : D9CRect{},
    };
    return appendRecord(
        D9C_COMMAND_RECORD_STRETCH_RECT,
        kLegacyStretchRectSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = AppendPhaseTimer::now();
            const bool ok = dxmt9::d3d9::pe::appendStretchRect(
                builder, wire, D3D9PeWireSurface(src),
                D3D9PeWireSurface(dst));
            phase.record(peAppendPhaseEncode_, t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ColorFill(IDirect3DSurface9* pSurf,
                                                 const RECT* pRect,
                                                 D3DCOLOR color) noexcept {
    dxmt9PeSetCurrentCallName("ColorFill");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
    // Wine d3d9 ColorFill: DXT-compressed and SYSTEMMEM surfaces are
    // rejected. visual_colorfill_format_policy.
    if (!pSurf) return D3DERR_INVALIDCALL;
    {
        D3DSURFACE_DESC sd{};
        if (SUCCEEDED(pSurf->GetDesc(&sd))) {
            if (sd.Pool == D3DPOOL_SYSTEMMEM) return D3DERR_INVALIDCALL;
            const D3DFORMAT f = sd.Format;
            if (f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 ||
                f == D3DFMT_DXT4 || f == D3DFMT_DXT5)
                return D3DERR_INVALIDCALL;
        }
    }
    dxmt9DeviceDebugLog("device_color_fill device=%p surf=%p rect=%s color=0x%08x",
                        this, pSurf, pRect ? "<custom>" : "<full>", (unsigned)color);
    D9CRect cr{}; if (pRect) cr = toR(*pRect);
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    const D9CCommandChunkWireColorFill wire{
        .surfaceHandleIndex = 0u,
        .colorARGB = (uint32_t)color,
        .hasRect = pRect ? 1u : 0u,
        .reserved0 = 0u,
        .rect = pRect ? cr : D9CRect{},
    };
    return appendRecord(
        D9C_COMMAND_RECORD_COLOR_FILL, kLegacyColorFillSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = AppendPhaseTimer::now();
            const bool ok = dxmt9::d3d9::pe::appendColorFill(
                builder, wire, D3D9PeWireSurface(pSurf));
            phase.record(peAppendPhaseEncode_, t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateOffscreenPlainSurface(UINT w, UINT h,
                                                                   D3DFORMAT fmt,
                                                                   D3DPOOL pool,
                                                                   IDirect3DSurface9** ppS,
                                                                   HANDLE* psh) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    // Wine D3D9 contract from test_surface_dimensions: width or
    // height of zero is rejected with D3DERR_INVALIDCALL.
    if (w == 0 || h == 0) return D3DERR_INVALIDCALL;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    // visual_offscreen_surface_creation_policy: only DEFAULT, SYSTEMMEM
    // and SCRATCH are valid pools for offscreen-plain surfaces. MANAGED
    // is rejected with INVALIDCALL.
    if (pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForSurface(extended_, psh, pool, true);
    if (FAILED(sharedHr)) return sharedHr;
    // T4 (D3D9Ex shared-handle, SYSTEMMEM partial): SYSTEMMEM
    // offscreen surfaces accept arbitrary W/H per Wine's
    // test_user_memory (~line 800). The user pointer becomes the
    // entire surface storage; pitch == bpp * width.
    const bool useUserMemory =
        extended_ && psh && pool == D3DPOOL_SYSTEMMEM;
    void* userPtr = nullptr;
    int32_t userPitch = 0;
    if (useUserMemory) {
        const uint32_t bpp = userMemoryBytesPerPixel(fmt);
        if (bpp == 0) return D3DERR_INVALIDCALL;
        userPtr = *psh;
        userPitch = static_cast<int32_t>(bpp * w);
    }
    dxmt9DeviceDebugLog("device_create_offscreen_surface device=%p size=%ux%u fmt=%u pool=%u user=%p",
                        this, w, h, (unsigned)fmt, (unsigned)pool, userPtr);
    uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sh : nullptr;
    D9CSurface* s = dxmt9c_device_create_offscreen_surface(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)pool,
                                                            providerShared);
    if (!s) return D3DERR_INVALIDCALL;
    if (providerShared) *psh = (HANDLE)(uintptr_t)sh;
    *ppS = CreatePeSurface(s, this, nullptr, this, true, userPtr, userPitch);
    dxmt9DeviceDebugLog("device_create_offscreen_surface -> surface=%p", *ppS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetRenderTarget(DWORD idx,
                                                       IDirect3DSurface9** ppS) noexcept {
    PeCallScope peCall(*this, "GetRenderTarget", DXMT9_PE_CALLSITE_PC());
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("GetRenderTarget", hr);
    };
    if (!ppS) return finishPeCall(D3DERR_INVALIDCALL);
    dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u",
                        this, (unsigned)idx);
    if (idx < 4 && rtSlotExplicit_[idx]) {
        if (!rtSlots_[idx]) {
            *ppS = nullptr;
            dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> explicit null",
                                this, (unsigned)idx);
            return finishPeCall(D3DERR_NOTFOUND);
        }
        rtSlots_[idx]->AddRef();
        *ppS = rtSlots_[idx];
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> cached rt=%p",
                            this, (unsigned)idx, static_cast<void*>(*ppS));
        return finishPeCall(S_OK);
    }
    if (idx == 0 && cachedBackBuffer0_) {
        cachedBackBuffer0_->AddRef();
        *ppS = cachedBackBuffer0_;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> cached backbuffer=%p",
                            this, (unsigned)idx, static_cast<void*>(*ppS));
        return finishPeCall(S_OK);
    }
    D9CSurface* s = dxmt9c_device_get_render_target(dev_, idx);
    *ppS = s ? CreatePeSurface(s, this, nullptr, this, false) : nullptr;
    dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> surface=%p",
                        this, (unsigned)idx, ppS ? static_cast<void*>(*ppS) : nullptr);
    return finishPeCall(s ? S_OK : D3DERR_NOTFOUND);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDepthStencilSurface(IDirect3DSurface9** ppS) noexcept {
    notePeDeviceCallAfterPresent("GetDepthStencilSurface");
    if (!ppS) return D3DERR_INVALIDCALL;
    // Wine d3d9 contract: when there is no depth-stencil surface bound,
    // the return code is D3DERR_NOTFOUND (not S_FALSE) and *ppS is NULL.
    // visual_depth_buffer_reset_policy + visual_depth_stencil_init_policy.
    if (dsSurfaceExplicit_) {
        if (!dsSurface_) {
            *ppS = nullptr;
            return D3DERR_NOTFOUND;
        }
        dsSurface_->AddRef();
        *ppS = dsSurface_;
        dxmt9DeviceDebugLog("device_get_depth_stencil_surface device=%p -> cached surface=%p",
                            this, static_cast<void*>(*ppS));
        return S_OK;
    }
    D9CSurface* s = dxmt9c_device_get_depth_stencil(dev_);
    *ppS = s ? CreatePeSurface(s, this, nullptr, this) : nullptr;
    dxmt9DeviceDebugLog("device_get_depth_stencil_surface device=%p -> surface=%p",
                        this, ppS ? static_cast<void*>(*ppS) : nullptr);
    return s ? S_OK : D3DERR_NOTFOUND;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetTransform(D3DTRANSFORMSTATETYPE state,
                                                    D3DMATRIX* pM) noexcept {
    notePeDeviceCallAfterPresent("GetTransform");
    if (!pM) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_get_transform device=%p state=%u", this, (unsigned)state);
    const uint32_t stateKey = static_cast<uint32_t>(state);
    D9CMatrix wireM{};
    if (peState_.transformShadow.get(stateKey, wireM)) {
        std::memcpy(pM, &wireM, sizeof(wireM));
        return S_OK;
    }
    return hr32(dxmt9c_device_get_transform(dev_, stateKey,
                reinterpret_cast<D9CMatrix*>(pM)));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetViewport(D3DVIEWPORT9* pVP) noexcept {
    PeCallScope peCall(*this, "GetViewport", DXMT9_PE_CALLSITE_PC());
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("GetViewport", hr);
    };
    if (!pVP) return finishPeCall(D3DERR_INVALIDCALL);
    // Phase 12: PE shadow is the source of truth. SetViewport writes
    // only into peState_.viewportShadow (recorder-active path);
    // round-trip the same value.
    const D9CViewport& vp = peState_.viewportShadow;
    pVP->X = vp.x; pVP->Y = vp.y;
    pVP->Width = vp.width; pVP->Height = vp.height;
    pVP->MinZ = vp.minZ;   pVP->MaxZ   = vp.maxZ;
    dxmt9DeviceDebugLog("device_get_viewport device=%p -> x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                        this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
    return finishPeCall(S_OK);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetScissorRect(RECT* pR) noexcept {
    PeCallScope peCall(*this, "GetScissorRect", DXMT9_PE_CALLSITE_PC());
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("GetScissorRect", hr);
    };
    if (!pR) return finishPeCall(D3DERR_INVALIDCALL);
    // Phase 12: PE shadow is the source of truth (see GetViewport).
    const D9CRect& cr = peState_.scissorShadow;
    pR->left = cr.left; pR->top = cr.top;
    pR->right = cr.right; pR->bottom = cr.bottom;
    return finishPeCall(S_OK);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetMaterial(D3DMATERIAL9* pM) noexcept {
    notePeDeviceCallAfterPresent("GetMaterial");
    if (!pM) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_get_material device=%p", this);
    // PE-shadow is the source of truth: SetMaterial only writes the
    // shadow, never the C-side state. Reading from C would return the
    // default-constructed value instead of the last Set value.
    std::memcpy(pM, &peState_.materialShadow, sizeof(D3DMATERIAL9));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetLight(DWORD idx, D3DLIGHT9* pL) noexcept {
    notePeDeviceCallAfterPresent("GetLight");
    dxmt9DeviceDebugLog("device_get_light device=%p idx=%u", this, (unsigned)idx);
    if (!pL) return D3DERR_INVALIDCALL;
    if (idx >= D9C_DRAW_PACKET_MAX_LIGHTS) {
        // Unset slot — Wine returns INVALIDCALL for never-Set indices.
        return D3DERR_INVALIDCALL;
    }
    const D9CLight& cl = peState_.lightShadow[idx];
    pL->Type = (D3DLIGHTTYPE)cl.type;
    std::memcpy(&pL->Diffuse,  &cl.diffuse,  sizeof(D3DCOLORVALUE));
    std::memcpy(&pL->Specular, &cl.specular, sizeof(D3DCOLORVALUE));
    std::memcpy(&pL->Ambient,  &cl.ambient,  sizeof(D3DCOLORVALUE));
    pL->Position.x = cl.position[0];
    pL->Position.y = cl.position[1];
    pL->Position.z = cl.position[2];
    pL->Direction.x = cl.direction[0];
    pL->Direction.y = cl.direction[1];
    pL->Direction.z = cl.direction[2];
    pL->Range = cl.range;
    pL->Falloff = cl.falloff;
    pL->Attenuation0 = cl.attenuation0;
    pL->Attenuation1 = cl.attenuation1;
    pL->Attenuation2 = cl.attenuation2;
    pL->Theta = cl.theta;
    pL->Phi = cl.phi;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetLightEnable(DWORD idx, BOOL* pEn) noexcept {
    notePeDeviceCallAfterPresent("GetLightEnable");
    dxmt9DeviceDebugLog("device_get_light_enable device=%p idx=%u", this, (unsigned)idx);
    if (!pEn) return D3DERR_INVALIDCALL;
    // Phase 12: LightEnable shadow is the source of truth for
    // idx < D9C_DRAW_PACKET_MAX_LIGHTS (the setter writes into
    // peState_.lightEnableShadow exclusively in the recorder-active
    // path). High indices fall through to the legacy unix-call —
    // mirror the boundary so idx out of shadow range stays FALSE
    // by default (no easy bridge read here without an extra ABI).
    if (idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
        const DWORD bit = 1u << idx;
        *pEn = (peState_.lightEnableShadow & bit) ? TRUE : FALSE;
    } else {
        *pEn = FALSE;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetClipPlane(DWORD idx, float* pPlane) noexcept {
    notePeDeviceCallAfterPresent("GetClipPlane");
    dxmt9DeviceDebugLog("device_get_clip_plane device=%p idx=%u", this, (unsigned)idx);
    if (!pPlane) return D3DERR_INVALIDCALL;
    if (idx >= 6) return D3DERR_INVALIDCALL;
    // Phase 12: PE shadow is the source of truth — SetClipPlane
    // writes into peState_.clipPlaneShadow exclusively (the array
    // is zero-initialized by PeHotStateShadow default ctor, which
    // matches the D3D9 post-CreateDevice all-zero contract).
    const std::size_t off = static_cast<std::size_t>(idx) * 4u;
    std::memcpy(pPlane, &peState_.clipPlaneShadow[off], sizeof(float) * 4);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetClipStatus(const D3DCLIPSTATUS9* p) noexcept {
    notePeDeviceCallAfterPresent("SetClipStatus");
    dxmt9DeviceDebugLog("device_set_clip_status device=%p", this);
    // gap_d3d9 B.8: dxmt9 does not track per-primitive clip status — no hardware
    // path exposes per-vertex clip-flag accumulation, exactly like wined3d's
    // storage-free stub. Reject null (the one real wined3d contract) and
    // otherwise accept without storing; echoing the seed back would be a
    // meaningless fake value, so GetClipStatus returns a defined default instead.
    if (!p) return D3DERR_INVALIDCALL;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetClipStatus(D3DCLIPSTATUS9* p) noexcept {
    notePeDeviceCallAfterPresent("GetClipStatus");
    dxmt9DeviceDebugLog("device_get_clip_status device=%p", this);
    if (!p) return D3DERR_INVALIDCALL;
    // Defined "everything visible / nothing clipped" default rather than echoing
    // a meaningless seed (no real clip accumulation exists on the HW path) or
    // leaving the caller's buffer untouched.
    p->ClipUnion = 0u;
    p->ClipIntersection = 0xFFFFFFFFu;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetRenderState(D3DRENDERSTATETYPE state,
                                                      DWORD* pValue) noexcept {
    notePeDeviceCallAfterPresent("GetRenderState");
    if (!pValue) return D3DERR_INVALIDCALL;
    uint32_t shadowValue = 0;
    if (peState_.renderStateShadow.get(static_cast<DWORD>(state), shadowValue)) {
        *pValue = shadowValue;
        return S_OK;
    }
    *pValue = dxmt9c_device_get_render_state(dev_, (uint32_t)state);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateStateBlock(D3DSTATEBLOCKTYPE type,
                                                        IDirect3DStateBlock9** ppSB) noexcept {
    notePeDeviceCallAfterPresent("CreateStateBlock");
    if (!ppSB) return D3DERR_INVALIDCALL;
    // D3D9 creation contract: a failed create must leave the out-pointer
    // NULL before the error HRESULT is returned.
    *ppSB = nullptr;
    if (!isValidD3DStateBlockType(type) || stateBlockRecording_) {
        return D3DERR_INVALIDCALL;
    }
    // State-block creation needs current server state.
    // flushPeRecorder() routes pending PE state through chunk records.
    const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::StateBlock);
    if (FAILED(flushHr)) return flushHr;
    dxmt9DeviceDebugLog("device_create_state_block device=%p type=%u", this, (unsigned)type);
    D9CStateBlock* sb = dxmt9c_device_create_state_block(dev_, (uint32_t)type);
    if (!sb) return D3DERR_INVALIDCALL;
    *ppSB = CreatePeStateBlock(sb, this, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::BeginStateBlock() noexcept {
    notePeDeviceCallAfterPresent("BeginStateBlock");
    if (stateBlockRecording_) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::StateBlock);
    if (FAILED(flushHr)) return flushHr;
    dxmt9DeviceDebugLog("device_begin_state_block device=%p", this);
    const HRESULT hr = hr32(dxmt9c_device_begin_state_block(dev_));
    if (SUCCEEDED(hr)) {
        stateBlockRecording_ = true;
        peState_.stateBlockRenderStateRestore.clear();
        peState_.stateBlockTransformRestore.clear();
        peState_.stateBlockTransformRecorded.clear();
        peState_.stateBlockVdeclRecorded = false;
    }
    dxmt9DeviceDebugLog("device_begin_state_block -> hr=0x%08x", (unsigned)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::EndStateBlock(IDirect3DStateBlock9** ppSB) noexcept {
    notePeDeviceCallAfterPresent("EndStateBlock");
    if (!ppSB) return D3DERR_INVALIDCALL;
    // EndStateBlock without a matching BeginStateBlock returns INVALIDCALL
    // and MUST leave the out-pointer UNTOUCHED — the Wine d3d9 oracle checks
    // `stateblock == sentinel` here (begin_end_state_block_policy /
    // stateblock_invalid_type_recording_invalid_calls). So null the
    // out-pointer only once we are actually attempting the create.
    if (!stateBlockRecording_) {
        return D3DERR_INVALIDCALL;
    }
    *ppSB = nullptr;
    const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::StateBlock);
    if (FAILED(flushHr)) return flushHr;
    dxmt9DeviceDebugLog("device_end_state_block device=%p", this);
    D9CStateBlock* sb = nullptr;
    HRESULT hr = hr32(dxmt9c_device_end_state_block(dev_, &sb));
    if (SUCCEEDED(hr)) {
        stateBlockRecording_ = false;
        peState_.stateBlockRenderStateRestore.forEach([&](uint32_t state, uint32_t value) {
            (void)dxmt9c_device_set_render_state(dev_, state, value);
            peState_.renderStateShadow.set(state, value);
            peState_.pendingRenderStates.erase(state);
        });
        peState_.stateBlockRenderStateRestore.clear();
        // wined3d semantics: state set / multiplied inside Begin/End
        // remains on the device after End. The PE shadow was updated
        // along with the server during the recording branch of
        // SetTransform, so no revert loop is needed here. Just drop the
        // (now-stale) *Restore entries.
        peState_.stateBlockTransformRestore.forEach([&](uint32_t state, const D9CMatrix& /*old*/) {
            // Drain any pending Sets that were re-routed during recording
            // — they have already been forwarded to the server.
            peState_.pendingTransforms.erase(state);
        });
        peState_.stateBlockTransformRestore.clear();
        if (sb) {
            // Mark Begin/End context so the new stateblock's ctor
            // takes its tracked-keys set from
            // stateBlockTransformRecorded (which may be empty if all
            // recording was MultiplyTransform) instead of falling
            // back to a full transformShadow capture.
            insideEndStateBlock_ = true;
            *ppSB = CreatePeStateBlock(sb, this, this);
            insideEndStateBlock_ = false;
        }
        // Clear AFTER CreatePeStateBlock so the new stateblock's ctor
        // can read stateBlockTransformRecorded via
        // CaptureStateBlockShadowForChild.
        peState_.stateBlockTransformRecorded.clear();
        peState_.stateBlockVdeclRecorded = false;
    }
    dxmt9DeviceDebugLog("device_end_state_block -> hr=0x%08x sb=%p out=%p",
                        (unsigned)hr, static_cast<void*>(sb), *ppSB);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetTextureStageState(DWORD stage,
                                                            D3DTEXTURESTAGESTATETYPE type,
                                                            DWORD* pValue) noexcept {
    notePeDeviceCallAfterPresent("GetTextureStageState");
    if (!pValue) return D3DERR_INVALIDCALL;
    if (stage >= kFragmentBlendStageCount) return D3DERR_INVALIDCALL;
    if (!isValidTextureStageStateType(type)) return D3DERR_INVALIDCALL;
    const uint32_t stageSlot = textureStageSlot(stage);
    const uint32_t stateSlot = textureStageStateSlot(type);
    uint32_t shadowValue = 0;
    if (peState_.tssShadow.get(stageSlot, stateSlot, shadowValue)) {
        *pValue = shadowValue;
        return S_OK;
    }
    *pValue = dxmt9c_device_get_texture_stage_state(dev_, stageSlot, stateSlot);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetSamplerState(DWORD sampler,
                                                       D3DSAMPLERSTATETYPE type,
                                                       DWORD* pValue) noexcept {
    notePeDeviceCallAfterPresent("GetSamplerState");
    if (!pValue) return D3DERR_INVALIDCALL;
    uint32_t samplerIndex = 0;
    if (!samplerSlot(sampler, samplerIndex)) {
        *pValue = 0;
        return S_OK;
    }
    // Phase 34: serve from the PE-side shadow so a Set/Get round-trip
    // is observable without forcing a recorder flush. Mirrors the
    // GetTextureStageState pattern (see above): shadow first, fall
    // back to the core-side read for slots the app has never written
    // (those return the resetState() defaults).
    uint32_t stateSlot = 0;
    if (samplerStateSlot(type, stateSlot)) {
        uint32_t shadowValue = 0;
        if (peState_.samplerStateShadow.get(samplerIndex, stateSlot, shadowValue)) {
            *pValue = shadowValue;
            return S_OK;
        }
    }
    *pValue = dxmt9c_device_get_sampler_state(dev_, samplerIndex, (uint32_t)type);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ValidateDevice(DWORD* pPasses) noexcept {
    notePeDeviceCallAfterPresent("ValidateDevice");
    dxmt9DeviceDebugLog("device_validate_device device=%p", this);
    if (pPasses) *pPasses = 1; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) noexcept {
    notePeDeviceCallAfterPresent("SetPaletteEntries");
    dxmt9DeviceDebugLog("device_set_palette_entries device=%p palette=%u entries=%p",
                        this, palette, static_cast<const void*>(entries));
    if (!entries) return D3DERR_INVALIDCALL;
    // D3DPTEXTURECAPS_ALPHAPALETTE policy: when the cap is NOT set,
    // any entry with non-trivial alpha (peFlags != 0xff) is rejected
    // and the previous palette must be preserved.
    D9CCaps cc{};
    bool alphaPaletteCap = false;
    if (SUCCEEDED(hr32(dxmt9c_device_get_caps(dev_, &cc)))) {
        D3DCAPS9 dcaps{};
        FillD3DCaps9(cc, &dcaps);
        alphaPaletteCap = (dcaps.TextureCaps & D3DPTEXTURECAPS_ALPHAPALETTE) != 0;
    }
    if (!alphaPaletteCap) {
        for (UINT i = 0; i < 256; ++i) {
            if (entries[i].peFlags != 0xff) {
                return D3DERR_INVALIDCALL;
            }
        }
    }
    auto& slot = palettes_[palette];
    std::memcpy(slot.data(), entries, sizeof(PALETTEENTRY) * 256);
    if (currentPaletteSet_ && currentPaletteIndex_ == palette) {
        applyCurrentPaletteToBoundTextures();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPaletteEntries(UINT palette, PALETTEENTRY* out) noexcept {
    notePeDeviceCallAfterPresent("GetPaletteEntries");
    dxmt9DeviceDebugLog("device_get_palette_entries device=%p palette=%u out=%p",
                        this, palette, static_cast<void*>(out));
    if (!out) return D3DERR_INVALIDCALL;
    const auto it = palettes_.find(palette);
    if (it == palettes_.end()) return D3DERR_INVALIDCALL;
    std::memcpy(out, it->second.data(), sizeof(PALETTEENTRY) * 256);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetCurrentTexturePalette(UINT palette) noexcept {
    notePeDeviceCallAfterPresent("SetCurrentTexturePalette");
    dxmt9DeviceDebugLog("device_set_current_texture_palette device=%p palette=%u", this, palette);
    if (palettes_.find(palette) == palettes_.end()) {
        return D3DERR_INVALIDCALL;
    }
    currentPaletteIndex_ = palette;
    currentPaletteSet_ = true;
    applyCurrentPaletteToBoundTextures();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetCurrentTexturePalette(UINT* p) noexcept {
    notePeDeviceCallAfterPresent("GetCurrentTexturePalette");
    dxmt9DeviceDebugLog("device_get_current_texture_palette device=%p out=%p",
                        this, static_cast<void*>(p));
    if (!p) return D3DERR_INVALIDCALL;
    if (!currentPaletteSet_) return D3DERR_INVALIDCALL;
    *p = currentPaletteIndex_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetSoftwareVertexProcessing(BOOL enable) noexcept {
    notePeDeviceCallAfterPresent("SetSoftwareVertexProcessing");
    dxmt9DeviceDebugLog("device_set_software_vertex_processing device=%p enable=%u", this, (unsigned)enable);
    softwareVertexProcessing_ = enable ? TRUE : FALSE;
    return S_OK;
}

BOOL    STDMETHODCALLTYPE D3D9DeviceImpl::GetSoftwareVertexProcessing() noexcept {
    notePeDeviceCallAfterPresent("GetSoftwareVertexProcessing");
    dxmt9DeviceDebugLog("device_get_software_vertex_processing device=%p", this);
    return softwareVertexProcessing_;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetNPatchMode(float segments) noexcept {
    notePeDeviceCallAfterPresent("SetNPatchMode");
    dxmt9DeviceDebugLog("device_set_npatch_mode device=%p segments=%f", this, segments);
    // stub: Wine returns S_OK; N-Patch tessellation was removed in D3D10, legacy
    // apps tolerate a no-op.
    return S_OK;
}

float   STDMETHODCALLTYPE D3D9DeviceImpl::GetNPatchMode() noexcept {
    notePeDeviceCallAfterPresent("GetNPatchMode");
    // stub: Wine returns 0.0f; N-Patch tessellation removed in D3D10, legacy apps
    // tolerate a no-op.
    return 0.0f;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetTexture(DWORD stage,
                                                  IDirect3DBaseTexture9** ppTex) noexcept {
    notePeDeviceCallAfterPresent("GetTexture");
    if (!ppTex) return D3DERR_INVALIDCALL;
    uint32_t textureSlot = 0;
    if (!fragmentTextureStageSlot(stage, textureSlot)) {
        // Wine leaves the caller's out-pointer untouched on
        // INVALIDCALL -- test_get_set_texture asserts the sentinel
        // value (0xdeadbeef) survives the failed call.
        return D3DERR_INVALIDCALL;
    }
    IDirect3DBaseTexture9* t = textures_[textureSlot];
    if (t) t->AddRef();
    *ppTex = t;
    dxmt9DeviceDebugLog("device_get_texture device=%p stage=%u -> tex=%p",
                        this, (unsigned)stage, static_cast<void*>(t));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetFVF(DWORD* pFVF) noexcept {
    notePeDeviceCallAfterPresent("GetFVF");
    if (!pFVF) return D3DERR_INVALIDCALL;
    *pFVF = fvf_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateVertexDeclaration(
        const D3DVERTEXELEMENT9* pElems,
        IDirect3DVertexDeclaration9** ppVD) noexcept {
    notePeDeviceCallAfterPresent("CreateVertexDeclaration");
    if (!ppVD) return D3DERR_INVALIDCALL;
    /* Wine returns INVALIDCALL with *ppVD == NULL on bad input. */
    const HRESULT validationHr = validateVertexElements(pElems);
    if (FAILED(validationHr)) {
        *ppVD = nullptr;
        return validationHr;
    }
    /* count elements until D3DDECL_END() */
    size_t n = 0;
    while (pElems[n].Stream != 0xFF) ++n;
    ++n; /* include D3DDECL_END */
    if (n > MAXD3DDECLLENGTH + 1) {
        *ppVD = nullptr;
        return D3DERR_INVALIDCALL;
    }
    D9CVertexElement tmp[MAXD3DDECLLENGTH + 1]{};
    for (size_t i = 0; i < n; ++i) {
        tmp[i].stream = pElems[i].Stream; tmp[i].offset = pElems[i].Offset;
        tmp[i].type   = pElems[i].Type;   tmp[i].method = pElems[i].Method;
        tmp[i].usage  = pElems[i].Usage;  tmp[i].usageIndex = pElems[i].UsageIndex;
    }
    D9CVertexDecl* d = dxmt9c_device_create_vertex_declaration(dev_, tmp);
    if (!d) {
        *ppVD = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *ppVD = CreatePeVertexDecl(d, this, this);
    notifyRenderTapeCreatedVertexDecl(
        d, D3D9PeWireVertexDecl(*ppVD),
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(tmp),
            n * sizeof(tmp[0])), n);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexDeclaration(
        IDirect3DVertexDeclaration9** ppVD) noexcept {
    notePeDeviceCallAfterPresent("GetVertexDeclaration");
    if (!ppVD) return D3DERR_INVALIDCALL;
    if (vdecl_) vdecl_->AddRef();
    *ppVD = vdecl_; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateVertexShader(const DWORD* pFn,
                                                          IDirect3DVertexShader9** ppVS) noexcept {
    notePeDeviceCallAfterPresent("CreateVertexShader");
    if (!ppVS) return D3DERR_INVALIDCALL;
    /* Wine semantics: leave *ppVS as NULL on validation failure. */
    const HRESULT validationHr = validateShaderBytecodeForStage(pFn,
                                                                /*vertexStage=*/true);
    if (FAILED(validationHr)) {
        *ppVS = nullptr;
        return validationHr;
    }
    dxmt9DeviceDebugLog("device_create_vertex_shader device=%p code=%p", this, pFn);
    D9CShader* s = dxmt9c_device_create_vertex_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
    if (!s) {
        *ppVS = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *ppVS = CreatePeVertexShader(s, this, hashValidatedShaderBytecode(pFn), this);
    notifyRenderTapeCreatedShader(s, D3D9PeWireVertexShader(*ppVS), 0u);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexShader(IDirect3DVertexShader9** ppVS) noexcept {
    notePeDeviceCallAfterPresent("GetVertexShader");
    if (!ppVS) return D3DERR_INVALIDCALL;
    if (vs_) vs_->AddRef(); *ppVS = vs_; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexShaderConstantF(UINT start, float* pData,
                                                                UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetVertexShaderConstantF");
    const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
    if (FAILED(hr)) return hr;
    readConstShadow(peConsts_.vsConstF, start, pData, count, sizeof(float) * 4);
    return S_OK;    }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexShaderConstantI(UINT start, INT* pData,
                                                                UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetVertexShaderConstantI");
    const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
    if (FAILED(hr)) return hr;
    readConstShadow(peConsts_.vsConstI, start, pData, count, sizeof(int32_t) * 4);        return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexShaderConstantB(UINT start, BOOL* pData,
                                                                UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetVertexShaderConstantB");
    const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
    if (FAILED(hr)) return hr;
    readConstShadow(peConsts_.vsConstB, start, pData, count, sizeof(uint32_t));        return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetStreamSource(UINT stream,
                                                       IDirect3DVertexBuffer9** ppBuf,
                                                       UINT* pOffset, UINT* pStride) noexcept {
    notePeDeviceCallAfterPresent("GetStreamSource");
    if (!ppBuf) return D3DERR_INVALIDCALL;
    if (stream >= 16) return D3DERR_INVALIDCALL;
    IDirect3DVertexBuffer9* b = streamSrc_[stream];
    if (b) b->AddRef();
    *ppBuf = b;
    if (pOffset) *pOffset = streamOff_[stream];
    if (pStride) *pStride = streamStr_[stream];
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetStreamSourceFreq(UINT stream, UINT* pFreq) noexcept {
    notePeDeviceCallAfterPresent("GetStreamSourceFreq");
    if (!pFreq) return D3DERR_INVALIDCALL;
    if (stream >= 16) return D3DERR_INVALIDCALL;
    const UINT freq = streamFreq_[stream];
    *pFreq = freq;
    dxmt9DeviceDebugLog("device_get_stream_source_freq device=%p stream=%u -> freq=0x%x",
                        this, stream, (unsigned)freq);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetIndices(IDirect3DIndexBuffer9** ppIBuf) noexcept {
    notePeDeviceCallAfterPresent("GetIndices");
    if (!ppIBuf) return D3DERR_INVALIDCALL;
    if (indexBuf_) indexBuf_->AddRef(); *ppIBuf = indexBuf_; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreatePixelShader(const DWORD* pFn,
                                                         IDirect3DPixelShader9** ppPS) noexcept {
    notePeDeviceCallAfterPresent("CreatePixelShader");
    if (!ppPS) return D3DERR_INVALIDCALL;
    const HRESULT validationHr = validateShaderBytecodeForStage(pFn,
                                                                /*vertexStage=*/false);
    if (FAILED(validationHr)) {
        *ppPS = nullptr;
        return validationHr;
    }
    dxmt9DeviceDebugLog("device_create_pixel_shader device=%p code=%p", this, pFn);
    D9CShader* s = dxmt9c_device_create_pixel_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
    if (!s) {
        *ppPS = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *ppPS = CreatePePixelShader(s, this, hashValidatedShaderBytecode(pFn), this);
    notifyRenderTapeCreatedShader(s, D3D9PeWirePixelShader(*ppPS), 1u);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPixelShader(IDirect3DPixelShader9** ppPS) noexcept {
    notePeDeviceCallAfterPresent("GetPixelShader");
    if (!ppPS) return D3DERR_INVALIDCALL;
    if (ps_) ps_->AddRef(); *ppPS = ps_; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPixelShaderConstantF(UINT start, float* pData,
                                                               UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetPixelShaderConstantF");
    const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
    if (FAILED(hr)) return hr;
    readConstShadow(peConsts_.psConstF, start, pData, count, sizeof(float) * 4);
    return S_OK;    }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPixelShaderConstantI(UINT start, INT* pData,
                                                               UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetPixelShaderConstantI");
    const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
    if (FAILED(hr)) return hr;
    readConstShadow(peConsts_.psConstI, start, pData, count, sizeof(int32_t) * 4);        return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPixelShaderConstantB(UINT start, BOOL* pData,
                                                               UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetPixelShaderConstantB");
    const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
    if (FAILED(hr)) return hr;
    readConstShadow(peConsts_.psConstB, start, pData, count, sizeof(uint32_t));        return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ProcessVertices(UINT srcStart, UINT dstIndex,
                                                       UINT vertexCount,
                                                       IDirect3DVertexBuffer9* dstBuffer,
                                                       IDirect3DVertexDeclaration9* declaration,
                                                       DWORD flags) noexcept {
    notePeDeviceCallAfterPresent("ProcessVertices");
    // T2 device-lost gate: lost devices must report DEVICELOST before
    // any ProcessVertices validation or unsupported-path rejection.
    if (deviceNotReset_) return D3DERR_DEVICELOST;
    const Context context{
        .device = dev_,
        .deviceIdentity = this,
        .fvf = fvf_,
        .vertexDeclaration = vdecl_,
        .vertexShader = vs_,
        .streamSources = std::span<
            IDirect3DVertexBuffer9* const,
            D9C_DRAW_PACKET_MAX_STREAMS>{streamSrc_},
        .streamOffsets = std::span<
            const UINT, D9C_DRAW_PACKET_MAX_STREAMS>{streamOff_},
        .streamStrides = std::span<
            const UINT, D9C_DRAW_PACKET_MAX_STREAMS>{streamStr_},
        .streamFrequencies = std::span<
            const UINT, D9C_DRAW_PACKET_MAX_STREAMS>{streamFreq_},
        .textures = std::span<
            IDirect3DBaseTexture9* const,
            D9C_DRAW_PACKET_MAX_TEXTURES>{textures_},
        .state = peState_,
        .constants = peConsts_,
    };
    return processVertices(
        context, srcStart, dstIndex, vertexCount,
        dstBuffer, declaration, flags);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) noexcept { return D3DERR_INVALIDCALL; }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) noexcept { return D3DERR_INVALIDCALL; }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::DeletePatch(UINT) noexcept {
    // stub: Wine returns S_OK; patch primitives unused on Metal.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateQuery(D3DQUERYTYPE type,
                                                   IDirect3DQuery9** ppQ) noexcept {
    notePeDeviceCallAfterPresent("CreateQuery");
    // Query-type support gate. Unsupported / out-of-range types return
    // D3DERR_NOTAVAILABLE — including the support-probe form
    // CreateQuery(type, NULL), which must NOT mutate *ppQ (it is NULL).
    // peQueryDataSizeForType reports a non-zero size for exactly the
    // supported set {EVENT, OCCLUSION, TIMESTAMP, TIMESTAMPDISJOINT,
    // TIMESTAMPFREQ}; everything else (e.g. 0xdeadbeef) reports 0.
    // Oracle: test_query_get_data_size_policy, query_support_probe.
    if (peQueryDataSizeForType(type) == 0u) return D3DERR_NOTAVAILABLE;
    D9CQuery* q = dxmt9c_device_create_query(dev_, (uint32_t)type);
    if (!q) return D3DERR_NOTAVAILABLE;
    if (!ppQ) {
        // Support-probe form: the type is supported, report S_OK but
        // create no object (caller passed a NULL out pointer).
        dxmt9c_query_release(q);
        return S_OK;
    }
    *ppQ = CreatePeQuery(q, this, this);
    notifyRenderTapeCreatedQuery(q, D3D9PeWireQuery(*ppQ));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetConvolutionMonoKernel(UINT,UINT,float*,float*) noexcept { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ComposeRects(IDirect3DSurface9*,IDirect3DSurface9*,
                                                    IDirect3DVertexBuffer9*,UINT,
                                                    IDirect3DVertexBuffer9*,
                                                    D3DCOMPOSERECTSOP,int,int) noexcept { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetGPUThreadPriority(INT* p) noexcept {
    notePeDeviceCallAfterPresent("GetGPUThreadPriority");
    // stub: Wine returns S_OK; GPU thread priority is not exposed by Metal.
    if (p) *p = 0; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetGPUThreadPriority(INT) noexcept {
    // stub: Wine returns S_OK; GPU thread priority is not exposed by Metal.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::WaitForVBlank(UINT sc) noexcept {
    notePeDeviceCallAfterPresent("WaitForVBlank");
    const HRESULT hr = hr32(dxmt9c_device_wait_for_vblank(dev_, sc));
    const dxmt9::d3d9::RenderTapeFlushWaitControl payload{.waitedSeqId = 0u};
    NotifyRenderTapeOrderedControlForChild(
        dxmt9::d3d9::RenderTapeOrderedControlHeader{
            .kind = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeControlKind::FlushWait),
            .disposition = static_cast<std::uint32_t>(
                SUCCEEDED(hr)
                    ? dxmt9::d3d9::RenderTapeControlDisposition::Completed
                    : dxmt9::d3d9::RenderTapeControlDisposition::Failed),
            .resultCode = static_cast<std::int32_t>(hr),
            .controlBytes = sizeof(payload)},
        std::as_bytes(std::span(&payload, 1u)));
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CheckResourceResidency(IDirect3DResource9**,
                                                              UINT32) noexcept {
    // stub: Wine returns S_OK; unified memory on Apple Silicon — all resources
    // are resident.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetMaximumFrameLatency(UINT maxLatency) noexcept {
    // Wine d3d9ex test_frame_latency contract: valid range is 1..30.
    // 0 or >= 31 must return D3DERR_INVALIDCALL.
    if (maxLatency == 0 || maxLatency >= 31)
        return D3DERR_INVALIDCALL;
    maxFrameLatencyShadow_ = maxLatency;
    return hr32(dxmt9c_device_set_maximum_frame_latency(dev_, maxLatency));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetMaximumFrameLatency(UINT* p) noexcept {
    notePeDeviceCallAfterPresent("GetMaximumFrameLatency");
    if (!p) return D3DERR_INVALIDCALL;
    // PE-shadow: return value previously set or the default of 3.
    *p = maxFrameLatencyShadow_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CheckDeviceState(HWND wnd) noexcept {
    notePeDeviceCallAfterPresent("CheckDeviceState");
    return hr32(dxmt9c_device_check_device_state(dev_,
                (uint64_t)(uintptr_t)wnd));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateRenderTargetEx(UINT w, UINT h,
                                                            D3DFORMAT fmt,
                                                            D3DMULTISAMPLE_TYPE ms,
                                                            DWORD msQual, BOOL lockable,
                                                            IDirect3DSurface9** ppS,
                                                            HANDLE* psh,
                                                            DWORD usage) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
    return CreateRenderTarget(w, h, fmt, ms, msQual, lockable, ppS, psh);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateOffscreenPlainSurfaceEx(UINT w, UINT h,
                                                                     D3DFORMAT fmt,
                                                                     D3DPOOL pool,
                                                                     IDirect3DSurface9** ppS,
                                                                     HANDLE* psh,
                                                                     DWORD usage) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
    return CreateOffscreenPlainSurface(w, h, fmt, pool, ppS, psh);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateDepthStencilSurfaceEx(UINT w, UINT h,
                                                                   D3DFORMAT fmt,
                                                                   D3DMULTISAMPLE_TYPE ms,
                                                                   DWORD msQual,
                                                                   BOOL discard,
                                                                   IDirect3DSurface9** ppS,
                                                                   HANDLE* psh,
                                                                   DWORD usage) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
    return CreateDepthStencilSurface(w, h, fmt, ms, msQual, discard, ppS, psh);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ResetEx(D3DPRESENT_PARAMETERS* pPP,
                                               D3DDISPLAYMODEEX* pFsMode) noexcept {
    dxmt9PeSetCurrentCallName("ResetEx");
    if (!pPP) return D3DERR_INVALIDCALL;
    // ResetEx windowed/fullscreen mode rules: wrong mode Size, a mode
    // supplied for a windowed reset (or missing for a fullscreen reset),
    // and a fullscreen mode whose dimensions do not match the requested
    // back-buffer size are all rejected with D3DERR_INVALIDCALL.
    if (const HRESULT mhr = peResetExModeHResult(pPP->Windowed != FALSE,
            pFsMode != nullptr, pFsMode ? pFsMode->Size : 0u,
            pFsMode ? pFsMode->Width : 0u, pFsMode ? pFsMode->Height : 0u,
            pPP->BackBufferWidth, pPP->BackBufferHeight);
        FAILED(mhr)) {
        return mhr;
    }
    // Present-parameter validation (same rule as Reset / CreateDevice),
    // evaluated on the extended lane (FLIPEX allowed, cap 30).
    if (const HRESULT vhr = pePresentParamsHResult(pPP->SwapEffect,
            pPP->BackBufferCount, pPP->PresentationInterval,
            pPP->MultiSampleType, pPP->MultiSampleQuality, extended_);
        FAILED(vhr)) {
        return vhr;
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
    const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::Reset);
    if (FAILED(flushHr)) return flushHr;
    releaseAllBound();
    clearPeStateTracking();
    stateBlockRecording_ = false;
    peState_.stateBlockRenderStateRestore.clear();
    peState_.stateBlockTransformRestore.clear();
    peState_.stateBlockTransformRecorded.clear();
    peState_.stateBlockVdeclRecorded = false;
    const HRESULT hr = hr32(dxmt9c_device_reset_ex(dev_, &cpp,
        pFsMode ? &cdme : nullptr));
    if (SUCCEEDED(hr)) {
        deviceNotReset_ = false;
        // Same flags-capture as Reset().
        implicitSwapchainFlagsShadow_ = pPP->Flags;
        // T2: same viewport/scissor reset semantics as Reset().
        const uint32_t w = std::max<uint32_t>(1u, cpp.backBufferWidth);
        const uint32_t h = std::max<uint32_t>(1u, cpp.backBufferHeight);
        peState_.viewportShadow = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
        peState_.scissorShadow  = D9CRect{0, 0, (int32_t)w, (int32_t)h};
        peState_.pendingViewport = false;
        peState_.pendingScissor  = false;
    }
    if (renderTapeCapture_ &&
        renderTapeCapture_->state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        const dxmt9::d3d9::RenderTapeResetControl payload{
            .reclaimedGeneration = 0u, .terminal = 1u};
        NotifyRenderTapeOrderedControlForChild(
            dxmt9::d3d9::RenderTapeOrderedControlHeader{
                .kind = static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeControlKind::Reset),
                .disposition = static_cast<std::uint32_t>(
                    SUCCEEDED(hr)
                        ? dxmt9::d3d9::RenderTapeControlDisposition::Terminal
                        : dxmt9::d3d9::RenderTapeControlDisposition::Failed),
                .resultCode = static_cast<std::int32_t>(hr),
                .controlBytes = sizeof(payload)},
            std::as_bytes(std::span(&payload, 1u)));
        abortRenderTapeCapture("reset_ex");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDisplayModeEx(UINT sc,
                                                        D3DDISPLAYMODEEX* pMode,
                                                        D3DDISPLAYROTATION* pRot) noexcept {
    notePeDeviceCallAfterPresent("GetDisplayModeEx");
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
