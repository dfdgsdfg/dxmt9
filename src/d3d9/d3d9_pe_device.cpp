/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex ABI owner.
 * QueryInterface deliberately anchors the vtable here alongside the hot
 * state/draw/present definitions. */

#include "d3d9_pe_device_impl.hpp"

// QueryInterface is the key function, so the retained inline hot entries
// (including D3D9DeviceImpl::Present) and the vtable are emitted by this TU.

#if defined(_WIN64)
static_assert(sizeof(D3D9DeviceImpl) == 106968);
static_assert(alignof(D3D9DeviceImpl) == 8);
#elif defined(_WIN32)
static_assert(sizeof(D3D9DeviceImpl) == 105752);
static_assert(alignof(D3D9DeviceImpl) == 8);
#endif

// Performance-critical state/draw definitions remain in this original device
// compiler unit. This preserves the baseline inlining and emission decisions
// for the hot COM surface and appendRecord specializations.
void D3D9DeviceImpl::assertRecorderThreadConfined() const noexcept {
    DXMT_ASSERT_OWNED_BY_OR_LOCKED(recorderState_.recorderOwnership,
                                  recorderState_.recorderLockRequired);
}

template<typename HotSetter>
HRESULT D3D9DeviceImpl::setRenderStateCore(D3DRENDERSTATETYPE state, DWORD value,
                           HotSetter& hotSetter) noexcept {
    using namespace dxmt9::d3d9::pe;
    dxmt9DeviceDebugLog("device_set_render_state device=%p state=%u value=0x%x",
                        this, (unsigned)state, (unsigned)value);
    // R-FORMAT-11 — RESZ MSAA depth-resolve trigger. The exact sentinel
    // write SetRenderState(D3DRS_POINTSIZE, 0x7FA05000) is a *command*,
    // not a point size: resolve the bound multisampled depth source
    // (the bound depth-stencil surface) into the bound INTZ depth
    // texture (stage 0). Intercept BEFORE any point-size shadow/record
    // so the sentinel never reaches the normal render-state path; a
    // non-sentinel D3DRS_POINTSIZE falls through and keeps its ordinary
    // point-size meaning. (A sentinel arriving during state-block
    // recording is likewise a command, not recordable state.)
    if (state == D3DRS_POINTSIZE && value == kReszDepthResolveSentinel) {
        hotSetter.markDirty();
        return requestReszDepthResolve();
    }
    const DWORD stateKey = static_cast<DWORD>(state);
    const RenderStateSlot renderKey = renderStateSlotKey(stateKey);
    std::uint32_t liveValue = 0u;
    const bool liveContains =
        recorderState_.peState.renderStateShadowTyped().get(renderKey, liveValue);
    const StateWritePlan plan = planRecorderStateWrite(StateWriteFacts{
        .phase = recorderState_.stateBlockTransaction.isRecording() ? RecorderPhase::Recording
                                      : RecorderPhase::Live,
        .origin = WriteOrigin::ExplicitSet,
        .liveContains = liveContains,
        .liveEquals = liveContains && liveValue == value,
        .pendingContains =
            recorderState_.peState.pendingRenderStatesTyped().contains(renderKey),
    });
    if (plan.kind() == StateWriteKind::NoOp ||
        plan.kind() == StateWriteKind::RetainPending) {
        return S_OK;
    }
    // Phase 31: cap check — if a NEW state would push the pending
    // table past the per-packet cap, drain pending state into the chunk
    // via chunkBarrierFlush() so the next packet starts fresh.
    if (plan.writePending() &&
        !recorderState_.peState.pendingRenderStatesTyped().contains(renderKey) &&
        recorderState_.peState.pendingRenderStatesTyped().size() >= D9C_DRAW_PACKET_MAX_RENDER_STATES) {
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
    }
    auto* const semanticTokens = scalarSemanticObserver();
    if (plan.writePending() && semanticTokens &&
        !semanticTokens->canRecord(
            ScalarSemanticCategory::RenderState, stateKey, 0u)) {
        return E_OUTOFMEMORY;
    }
    if (plan.directOrderedCall()) {
        const HRESULT hr = hr32(
            dxmt9c_device_set_render_state(dev_, stateKey, value));
        if (FAILED(hr)) return hr;
    }
    if (plan.writeRecorded()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept { writer.renderStates().set(renderKey, value); });
    }
    if (plan.writeLive() && plan.writePending()) {
        recorderState_.peState.transition().setRenderState(renderKey, value);
    } else if (plan.writeLive()) {
        recorderState_.peState.maintenance().renderStateShadowTyped().set(renderKey, value);
    } else if (plan.writePending()) {
        recorderState_.peState.maintenance().pendingRenderStatesTyped().set(renderKey, value);
        if (semanticTokens && !semanticTokens->record(
                ScalarSemanticCategory::RenderState, stateKey, 0u)) {
            return E_OUTOFMEMORY;
        }
    }
    if (plan.semanticTransition() || plan.directOrderedCall()) hotSetter.markDirty();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetRenderState(D3DRENDERSTATETYPE state,
                                          DWORD value) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!recorderState_.stateBlockTransaction.writeAllowed()) {
        return D3DERR_DEVICELOST;
    }
    PeDiagnosticsState* const diagnostics = diagnostics_.get();
    if (!diagnostics || !diagnostics->gates.hotSetterTimer) {
        return setRenderStateCore(state, value, peNullHotSetter_);
    }
    PeHotStateSetterTimer hotSetter(
        *this, *diagnostics, PeHotStateSetterFamily::RenderState,
        "SetRenderState",
        &PeDiagnosticsState::peEntryStateDecimatedStats_);
    return setRenderStateCore(state, value, hotSetter);
}

bool D3D9DeviceImpl::isValidTextureStageStateType(D3DTEXTURESTAGESTATETYPE type) noexcept {
    const uint32_t t = static_cast<uint32_t>(type);
    if (t == 0u || t > 32u) return false;
    // Valid D3DTSS_* IDs in d3d9types.h within [1..32]:
    //   1..11  COLOROP, COLORARG1, COLORARG2, ALPHAOP, ALPHAARG1,
    //          ALPHAARG2, BUMPENVMAT00..11, TEXCOORDINDEX
    //   22..24 BUMPENVLSCALE, BUMPENVLOFFSET, TEXTURETRANSFORMFLAGS
    //   26..28 COLORARG0, ALPHAARG0, RESULTARG
    //   32     CONSTANT
    // Reserved gaps (12..21, 25, 29..31) report INVALIDCALL on
    // native -- 0xdead is filtered by the t>32 check above; the
    // bit-mask below pins the in-range gaps as well.
    constexpr uint64_t kValid =
        (0x7FFull << 1) |   // 1..11
        (0x7ull << 22) |    // 22..24
        (0x7ull << 26) |    // 26..28
        (1ull << 32);       // 32
    return (kValid & (1ull << t)) != 0;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetTextureStageState(DWORD stage,
                                                D3DTEXTURESTAGESTATETYPE type,
                                                DWORD value) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::TextureStageSampler,
        "SetTextureStageState",
        &PeDiagnosticsState::peEntryStateDecimatedStats_, nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    using namespace dxmt9::d3d9::pe;
    dxmt9DeviceDebugLog("device_set_texture_stage_state device=%p stage=%u type=%u value=0x%x",
                        this, (unsigned)stage, (unsigned)type, (unsigned)value);
    // Wine d3d9 test_limits + test_texture_stage_states: reject
    // out-of-range stage (>= caps.MaxTextureBlendStages == 8) and
    // unrecognised D3DTSS_* type with D3DERR_INVALIDCALL at the
    // device-method boundary.
    if (stage >= kFragmentBlendStageCount) return D3DERR_INVALIDCALL;
    if (!isValidTextureStageStateType(type)) return D3DERR_INVALIDCALL;
    const TextureStageIndex stageKey = textureStageIndexKey(stage);
    const TextureStageStateType typeKey =
        textureStageStateTypeKey(static_cast<uint32_t>(type));
    std::uint32_t liveValue = 0u;
    const bool liveContains =
        recorderState_.peState.tssShadowTyped().get(stageKey, typeKey, liveValue);
    const StateWritePlan plan = planRecorderStateWrite(StateWriteFacts{
        .phase = recorderState_.stateBlockTransaction.isRecording() ? RecorderPhase::Recording
                                      : RecorderPhase::Live,
        .origin = WriteOrigin::ExplicitSet,
        .liveContains = liveContains,
        .liveEquals = liveContains && liveValue == value,
        .pendingContains =
            recorderState_.peState.pendingTssTyped().contains(stageKey, typeKey),
    });
    if (plan.kind() == StateWriteKind::NoOp ||
        plan.kind() == StateWriteKind::RetainPending) {
        return S_OK;
    }
    // Phase 34: cap-check uses chunkBarrierFlush so pending state is
    // encoded as APPLY_STATE record(s) + cleared before the new entry.
    if (plan.writePending() &&
        !recorderState_.peState.pendingTssTyped().contains(stageKey, typeKey) &&
        recorderState_.peState.pendingTssTyped().size() >= D9C_DRAW_PACKET_MAX_TSS) {
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
    }
    auto* const semanticTokens = scalarSemanticObserver();
    if (plan.writePending() && semanticTokens &&
        !semanticTokens->canRecord(
            ScalarSemanticCategory::TextureStageState, stage, type)) {
        return E_OUTOFMEMORY;
    }
    if (plan.directOrderedCall()) {
        const HRESULT hr = hr32(dxmt9c_device_set_texture_stage_state(
            dev_, stage, static_cast<std::uint32_t>(type), value));
        if (FAILED(hr)) return hr;
    }
    if (plan.writeRecorded()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.textureStageStates().set(stageKey, typeKey, value);
            });
    }
    if (plan.writeLive() && plan.writePending()) {
        recorderState_.peState.transition().setTextureStageState(
            stageKey, typeKey, value);
    } else if (plan.writeLive()) {
        recorderState_.peState.maintenance().tssShadowTyped().set(stageKey, typeKey, value);
    } else if (plan.writePending()) {
        recorderState_.peState.maintenance().pendingTssTyped().set(stageKey, typeKey, value);
        if (semanticTokens && !semanticTokens->record(
                ScalarSemanticCategory::TextureStageState, stage, type)) {
            return E_OUTOFMEMORY;
        }
    }
    if (plan.semanticTransition() || plan.directOrderedCall()) hotSetter.markDirty();
    return S_OK;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetSamplerState(DWORD sampler,
                                           D3DSAMPLERSTATETYPE type,
                                           DWORD value) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::TextureStageSampler, "SetSamplerState",
        &PeDiagnosticsState::peEntryStateDecimatedStats_, nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    using namespace dxmt9::d3d9::pe;
    dxmt9DeviceDebugLog("device_set_sampler_state device=%p sampler=%u type=%u value=0x%x",
                        this, (unsigned)sampler, (unsigned)type, (unsigned)value);
    SamplerIndex samplerIndexKeyVal{};
    if (!samplerIndexKey(sampler, samplerIndexKeyVal)) {
        return D3DERR_INVALIDCALL;
    }
    SamplerStateType stateTypeKeyVal{};
    if (!samplerStateTypeKey(static_cast<uint32_t>(type), stateTypeKeyVal)) {
        return S_OK;
    }
    std::uint32_t liveValue = 0u;
    const bool liveContains = recorderState_.peState.samplerStateShadowTyped().get(
        samplerIndexKeyVal, stateTypeKeyVal, liveValue);
    const StateWritePlan plan = planRecorderStateWrite(StateWriteFacts{
        .phase = recorderState_.stateBlockTransaction.isRecording() ? RecorderPhase::Recording
                                      : RecorderPhase::Live,
        .origin = WriteOrigin::ExplicitSet,
        .liveContains = liveContains,
        .liveEquals = liveContains && liveValue == value,
        .pendingContains = recorderState_.peState.pendingSamplerStatesTyped().contains(
            samplerIndexKeyVal, stateTypeKeyVal),
    });
    if (plan.kind() == StateWriteKind::NoOp ||
        plan.kind() == StateWriteKind::RetainPending) {
        return S_OK;
    }
    // Phase 34: cap-check uses chunkBarrierFlush.
    if (plan.writePending() &&
        !recorderState_.peState.pendingSamplerStatesTyped().contains(
            samplerIndexKeyVal, stateTypeKeyVal) &&
        recorderState_.peState.pendingSamplerStatesTyped().size() >= D9C_DRAW_PACKET_MAX_SAMPLER) {
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
    }
    auto* const semanticTokens = scalarSemanticObserver();
    if (plan.writePending() && semanticTokens &&
        !semanticTokens->canRecord(
            ScalarSemanticCategory::SamplerState,
            rawSlot(samplerIndexKeyVal),
            static_cast<std::uint32_t>(rawSlot(stateTypeKeyVal)))) {
        return E_OUTOFMEMORY;
    }
    if (plan.directOrderedCall()) {
        const HRESULT hr = hr32(dxmt9c_device_set_sampler_state(
            dev_, rawSlot(samplerIndexKeyVal),
            static_cast<std::uint32_t>(type), value));
        if (FAILED(hr)) return hr;
    }
    if (plan.writeRecorded()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.samplerStates().set(
                    samplerIndexKeyVal, stateTypeKeyVal, value);
            });
    }
    if (plan.writeLive() && plan.writePending()) {
        recorderState_.peState.transition().setSamplerState(
            samplerIndexKeyVal, stateTypeKeyVal, value);
    } else if (plan.writeLive()) {
        recorderState_.peState.maintenance().samplerStateShadowTyped().set(
            samplerIndexKeyVal, stateTypeKeyVal, value);
    } else if (plan.writePending()) {
        recorderState_.peState.maintenance().pendingSamplerStatesTyped().set(
            samplerIndexKeyVal, stateTypeKeyVal, value);
        if (semanticTokens && !semanticTokens->record(
                ScalarSemanticCategory::SamplerState,
                rawSlot(samplerIndexKeyVal),
                rawSlot(stateTypeKeyVal))) {
            return E_OUTOFMEMORY;
        }
    }
    if (plan.semanticTransition() || plan.directOrderedCall()) hotSetter.markDirty();
    return S_OK;
        });
}

bool D3D9DeviceImpl::fragmentTextureStageSlot(DWORD stage, uint32_t& slot) noexcept {
    if (stage < kFragmentTextureStageCount) {
        slot = static_cast<uint32_t>(stage);
        return true;
    }
    return vertexTextureSamplerSlot(stage, slot);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetTexture(DWORD stage,
                                      IDirect3DBaseTexture9* pTex) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::Texture, "SetTexture",
        &PeDiagnosticsState::peEntryStateDecimatedStats_, nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    dxmt9DeviceDebugLog("device_set_texture device=%p stage=%u tex=%p",
                        this, (unsigned)stage, pTex);
    // Wine d3d9 test_limits: all 16 pixel samplers are settable
    // (stages 0..15), independent of caps.MaxSimultaneousTextures.
    // fragmentTextureStageSlot is the single validator: fragment
    // stages 0..15 and vertex samplers 257..260 map to slots,
    // anything else is D3DERR_INVALIDCALL.
    uint32_t textureSlot = 0;
    if (!fragmentTextureStageSlot(stage, textureSlot)) return D3DERR_INVALIDCALL;
    D3D9PeValidatedTexture validatedTexture{};
    const HRESULT membershipHr = D3D9PeValidateTexture(
        pTex, static_cast<IDirect3DDevice9*>(this), &validatedTexture);
    if (FAILED(membershipHr)) return membershipHr;
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                setRecordedRef(
                    writer.textures(),
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::textures>(textureSlot),
                    validatedTexture);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    if (textures_[textureSlot] == pTex) {
        return S_OK;
    }
    recorderState_.peState.transition().bindTexture(textureSlot, [&]() noexcept {
        setRef(textures_[textureSlot], pTex);
        recorderState_.peBindingView.textures[textureSlot] =
            validatedTexture.wire();
        applyCurrentPaletteToTexture(textures_[textureSlot]);
    });
    hotSetter.markDirty();
    return S_OK;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetFVF(DWORD fvf) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::VertexInput, "SetFVF", nullptr, nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    dxmt9DeviceDebugLog("device_set_fvf device=%p fvf=0x%x", this, (unsigned)fvf);
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.fvf().set(
                    stateBlockFixedSlotKey<StateBlockApplyPhysicalStore::fvf>(0u),
                    fvf);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    if (fvf_ == fvf && vdecl_ != nullptr) {
        /* Same FVF, decl already mirrored. */
        return S_OK;
    }
    IDirect3DVertexDeclaration9* implicitDecl = nullptr;
    const HRESULT resolveHr =
        resolveImplicitDeclForFvf(fvf, &implicitDecl);
    if (FAILED(resolveHr)) return resolveHr;
    D3D9PeValidatedDeclaration validatedDeclaration{};
    const HRESULT membershipHr = D3D9PeValidateVertexDecl(
        implicitDecl, static_cast<IDirect3DDevice9*>(this),
        &validatedDeclaration);
    if (FAILED(membershipHr)) return membershipHr;
    recorderState_.peState.transition().bindVertexInput([&]() noexcept {
        fvf_ = fvf;
        vdecl_ = implicitDecl;
        recorderState_.peBindingView.fvf = fvf;
        recorderState_.peBindingView.vdecl = validatedDeclaration.wire();
    });
    hotSetter.markDirty();
    /* SetFVF shadows the vertex-declaration slot: GetVertexDeclaration
     * must return the implicit decl for this FVF (Wine
     * test_vertex_declaration_fvf_policy line ~702 and
     * test_fvf_decl_management). vdecl_ is borrowed (see
     * SetVertexDeclaration for Wine refcount semantics) — the
     * implicit decl is kept alive by fvfDeclCache_. */
    return S_OK;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetVertexDeclaration(
        IDirect3DVertexDeclaration9* pVD) noexcept {
    return withPeCallAndHotStateSetter(
        "SetVertexDeclaration", nullptr, nullptr,
        PeHotStateSetterFamily::VertexInput, nullptr,
        [&](auto& peCall, auto& hotSetter)
            __attribute__((always_inline)) noexcept -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("SetVertexDeclaration", hr);
    };
    dxmt9DeviceDebugLog("device_set_vertex_declaration device=%p decl=%p", this, pVD);
    D3D9PeValidatedDeclaration validatedDeclaration{};
    const HRESULT membershipHr = D3D9PeValidateVertexDecl(
        pVD, static_cast<IDirect3DDevice9*>(this), &validatedDeclaration);
    if (FAILED(membershipHr)) return finishPeCall(membershipHr);
    // PE-shadow stateblock support: remember that vdecl was touched
    // during BeginStateBlock/EndStateBlock so the resulting block's
    // tracked set includes the vdecl slot. The flag is consumed by
    // CaptureStateBlockShadowForChild and cleared in EndStateBlock.
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.setVertexDeclarationRecorded(true);
                setRecordedRef(
                    writer.vertexDeclaration(),
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::vertexDeclaration>(0u),
                    validatedDeclaration);
                writer.fvf().set(
                    stateBlockFixedSlotKey<StateBlockApplyPhysicalStore::fvf>(0u),
                    0u);
            });
        hotSetter.markDirty();
        return finishPeCall(S_OK);
    }
    if (vdecl_ == pVD) return finishPeCall(S_OK);
    /* Wine semantics (test_get_set_vertex_declaration, device.c:376):
     * SetVertexDeclaration must NOT touch the user-visible refcount
     * of either the previous or the new decl. The user is required
     * to keep the bound decl alive until they rebind or release the
     * device. Internally Wine's wined3d still holds its own ref via
     * wined3d_stateblock_set_vertex_declaration; the dxmt9 analogue
     * is the C-side D9CVertexDecl handle held by the PE wrapper. So
     * we store vdecl_ as a borrowed pointer and never AddRef/Release
     * through this slot. FVF-implicit decls are kept alive by
     * fvfDeclCache_ so they outlive any user-visible window. */
    /* Explicit decl resets FVF to 0 (Wine
     * test_vertex_declaration_fvf_policy line ~692). User-supplied
     * decls do not back-convert to an FVF in this PE shadow; that
     * mapping is intentionally lossy. */
    recorderState_.peState.transition().bindVertexInput([&]() noexcept {
        vdecl_ = pVD;
        fvf_ = 0;
        recorderState_.peBindingView.vdecl = validatedDeclaration.wire();
        recorderState_.peBindingView.fvf = 0u;
    });
    hotSetter.markDirty();
    return finishPeCall(S_OK);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetVertexShader(IDirect3DVertexShader9* pVS) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::Shader, "SetVertexShader", nullptr,
        nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    dxmt9DeviceDebugLog("device_set_vertex_shader device=%p shader=%p", this, pVS);
    D3D9PeValidatedVertexShader validatedShader{};
    const HRESULT membershipHr = D3D9PeValidateVertexShader(
        pVS, static_cast<IDirect3DDevice9*>(this), &validatedShader);
    if (FAILED(membershipHr)) return membershipHr;
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                setRecordedRef(
                    writer.vertexShader(),
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::vertexShader>(0u),
                    validatedShader);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    // Phase 12: PE-shadow-only when chunk recorder is active. The
    // packet built for the next draw carries vsValid=1 + the vs_
    // wire handle; server-side canonical state replay dispatches the
    // dxmt9c_device_set_vertex_shader call before the draw runs.
    if (vs_ == pVS) return S_OK;
    recorderState_.peState.transition().bindVertexShader([&]() noexcept {
        setRef(vs_, pVS);
        recorderState_.peBindingView.vs = validatedShader.wire();
    });
    hotSetter.markDirty();
    return S_OK;
        });
}


template <typename Scope>
HRESULT D3D9DeviceImpl::SetVertexShaderConstantFSlowBody(UINT start, const float* pData,
                                           UINT count,
                                           Scope& peCall) noexcept {
    DxmtPeDecimatedScopeGuard peEntryScope;
    dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
    const std::int64_t callEntryNs = dxmt9PeRecorderStatsEnabled()
        ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
        : 0;
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        if (SUCCEEDED(hr)) {
            recordPeConstSetterCpu(D9C_COMMAND_RECORD_SET_VS_CONST_F,
                                   callEntryNs, count);
        }
        return peCall.finish("SetVertexShaderConstantF", hr);
    };
    dxmt9DeviceDebugLog("device_set_vertex_shader_constant_f device=%p start=%u count=%u data=%p",
                        this, start, count, pData);
    const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
    if (FAILED(hr)) return finishPeCall(hr);
    if (dxmt9PerfVsConstSetterRangeEnabled()) {
        const VsConstRangeChange change = analyzeConstShadowChange(
            recorderState_.peConsts.vsConstF, start, count, pData, sizeof(float) * 4);
        recordVsConstSetterRange(VsConstSetterRangePhase::Call,
                                 currentVertexShaderHash(),
                                 currentPixelShaderHash(),
                                 start, count,
                                 change.changedRegs,
                                 change.changedSpanRegs);
    }
    // Shadow-only: defer the record until the next flushPendingConsts()
    // (called before each draw record + at chunk commit).
    return finishPeCall(applyConstStateWrite(
        recorderState_.peConsts.vsConstF, &PeStateBlockConstRecorded::vsConstF,
        start, count, pData, sizeof(float) * 4));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetVertexShaderConstantF(UINT start, const float* pData,
                                                    UINT count) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    if (dxmt9PeConstSetterSlowPathRequired()) {
        return SetVertexShaderConstantFSlow(start, pData, count);
    }
    const HRESULT hr = validateConstRangeFast(start, count, pData, kVsConstFMax);
    if (FAILED(hr)) return hr;
    // Shadow-only: defer the record until the next flushPendingConsts()
    // (called before each draw record + at chunk commit).
    return applyConstStateWrite(
        recorderState_.peConsts.vsConstF, &PeStateBlockConstRecorded::vsConstF,
        start, count, pData, sizeof(float) * 4);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetVertexShaderConstantI(UINT start, const INT* pData,
                                                    UINT count) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    if (dxmt9PeConstSetterSlowPathRequired()) {
        return SetVertexShaderConstantISlow(start, pData, count);
    }
    const HRESULT hr = validateConstRangeFast(start, count, pData, kVsConstIMax);
    if (FAILED(hr)) return hr;
    return applyConstStateWrite(
        recorderState_.peConsts.vsConstI, &PeStateBlockConstRecorded::vsConstI,
        start, count, pData, sizeof(int32_t) * 4);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                    UINT count) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    if (dxmt9PeConstSetterSlowPathRequired()) {
        return SetVertexShaderConstantBSlow(start, pData, count);
    }
    const HRESULT hr = validateConstRangeFast(start, count, pData, kVsConstBMax);
    if (FAILED(hr)) return hr;
    return applyConstStateWrite(
        recorderState_.peConsts.vsConstB, &PeStateBlockConstRecorded::vsConstB,
        start, count, pData, sizeof(uint32_t));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetStreamSourceFreq(UINT stream, UINT freq) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::VertexInput, "SetStreamSourceFreq",
        nullptr, nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    dxmt9DeviceDebugLog("device_set_stream_source_freq device=%p stream=%u freq=0x%x",
                        this, stream, (unsigned)freq);
    if (stream >= 16) return D3DERR_INVALIDCALL;
    // D3D9 SetStreamSourceFreq encoding (D3DSTREAMSOURCE_* in
    // d3d9types.h):
    //   - low 30 bits: frequency / divider value
    //   - bit 0x40000000 (INDEXEDDATA): stream 0 supplies the draw's
    //     instance count
    //   - bit 0x80000000 (INSTANCEDATA): a non-zero stream advances by
    //     instance_id / divider
    // Rules (Wine wined3d_device_set_stream_source_freq, matched by
    // test_stream_source_frequency_state):
    //   - Stream 0 may not carry INSTANCEDATA (it cannot be the
    //     index source stream).
    //   - INDEXEDDATA and INSTANCEDATA are mutually exclusive.
    //   - A freq value of 0 (divider 0 with no flag) is invalid.
    const UINT kIndexedData  = 0x40000000u;
    const UINT kInstanceData = 0x80000000u;
    const bool indexedFlag  = (freq & kIndexedData)  != 0;
    const bool instanceFlag = (freq & kInstanceData) != 0;
    if (indexedFlag && instanceFlag) return D3DERR_INVALIDCALL;
    if (stream == 0 && instanceFlag) return D3DERR_INVALIDCALL;
    if (!indexedFlag && !instanceFlag &&
        (freq & ~(kIndexedData | kInstanceData)) == 0) {
        return D3DERR_INVALIDCALL;
    }
    if (recorderState_.stateBlockTransaction.isRecording()) {
        // Frequency is an independent state-block aspect. Do not
        // implicitly capture or replay the source tuple.
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.streamFrequencies().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::streamFrequencies>(stream),
                    freq);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    const HRESULT flushHr = flushPeRecorder();
    if (FAILED(flushHr)) return flushHr;
    streamFreq_[stream] = freq;
    hotSetter.markDirty();
    return hr32(dxmt9c_device_set_stream_source_freq(dev_, stream, freq));
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetIndices(IDirect3DIndexBuffer9* pIBuf) noexcept {
    return withPeCallAndHotStateSetter(
        "SetIndices", nullptr, nullptr,
        PeHotStateSetterFamily::VertexInput, nullptr,
        [&](auto& peCall, auto& hotSetter)
            __attribute__((always_inline)) noexcept -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("SetIndices", hr);
    };
    dxmt9DeviceDebugLog("device_set_indices device=%p ib=%p", this, pIBuf);
    D3D9PeValidatedIndexBuffer validatedBuffer{};
    const HRESULT membershipHr = D3D9PeValidateIndexBuffer(
        pIBuf, static_cast<IDirect3DDevice9*>(this), &validatedBuffer);
    if (FAILED(membershipHr)) return finishPeCall(membershipHr);
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                setRecordedRef(
                    writer.indexBuffer(),
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::indexBuffer>(0u),
                    validatedBuffer);
            });
        hotSetter.markDirty();
        return finishPeCall(S_OK);
    }
    if (indexBuf_ == pIBuf) return finishPeCall(S_OK);
    recorderState_.peState.transition().bindIndexBuffer([&]() noexcept {
        setRef(indexBuf_, pIBuf);
        recorderState_.peBindingView.indexBuffer = validatedBuffer.wire();
    });
    hotSetter.markDirty();
    return finishPeCall(S_OK);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetPixelShader(IDirect3DPixelShader9* pPS) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::Shader, "SetPixelShader", nullptr,
        nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    dxmt9DeviceDebugLog("device_set_pixel_shader device=%p shader=%p", this, pPS);
    D3D9PeValidatedPixelShader validatedShader{};
    const HRESULT membershipHr = D3D9PeValidatePixelShader(
        pPS, static_cast<IDirect3DDevice9*>(this), &validatedShader);
    if (FAILED(membershipHr)) return membershipHr;
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                setRecordedRef(
                    writer.pixelShader(),
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::pixelShader>(0u),
                    validatedShader);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    if (ps_ == pPS) return S_OK;
    recorderState_.peState.transition().bindPixelShader([&]() noexcept {
        setRef(ps_, pPS);
        recorderState_.peBindingView.ps = validatedShader.wire();
    });
    hotSetter.markDirty();
    return S_OK;
        });
}

template <typename Scope>
HRESULT D3D9DeviceImpl::SetPixelShaderConstantFSlowBody(UINT start, const float* pData,
                                          UINT count,
                                          Scope& peCall) noexcept {
    DxmtPeDecimatedScopeGuard peEntryScope;
    dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
    const std::int64_t callEntryNs = dxmt9PeRecorderStatsEnabled()
        ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
        : 0;
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        if (SUCCEEDED(hr)) {
            recordPeConstSetterCpu(D9C_COMMAND_RECORD_SET_PS_CONST_F,
                                   callEntryNs, count);
        }
        return peCall.finish("SetPixelShaderConstantF", hr);
    };
    dxmt9DeviceDebugLog("device_set_pixel_shader_constant_f device=%p start=%u count=%u data=%p",
                        this, start, count, pData);
    const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
    if (FAILED(hr)) return finishPeCall(hr);
    return finishPeCall(applyConstStateWrite(
        recorderState_.peConsts.psConstF, &PeStateBlockConstRecorded::psConstF,
        start, count, pData, sizeof(float) * 4));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetPixelShaderConstantF(UINT start, const float* pData,
                                                   UINT count) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    if (dxmt9PeConstSetterSlowPathRequired()) {
        return SetPixelShaderConstantFSlow(start, pData, count);
    }
    const HRESULT hr = validateConstRangeFast(start, count, pData, kPsConstFMax);
    if (FAILED(hr)) return hr;
    return applyConstStateWrite(
        recorderState_.peConsts.psConstF, &PeStateBlockConstRecorded::psConstF,
        start, count, pData, sizeof(float) * 4);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetPixelShaderConstantI(UINT start, const INT* pData,
                                                   UINT count) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    if (dxmt9PeConstSetterSlowPathRequired()) {
        return SetPixelShaderConstantISlow(start, pData, count);
    }
    const HRESULT hr = validateConstRangeFast(start, count, pData, kPsConstIMax);
    if (FAILED(hr)) return hr;
    return applyConstStateWrite(
        recorderState_.peConsts.psConstI, &PeStateBlockConstRecorded::psConstI,
        start, count, pData, sizeof(int32_t) * 4);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                   UINT count) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    if (dxmt9PeConstSetterSlowPathRequired()) {
        return SetPixelShaderConstantBSlow(start, pData, count);
    }
    const HRESULT hr = validateConstRangeFast(start, count, pData, kPsConstBMax);
    if (FAILED(hr)) return hr;
    return applyConstStateWrite(
        recorderState_.peConsts.psConstB, &PeStateBlockConstRecorded::psConstB,
        start, count, pData, sizeof(uint32_t));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::BeginScene() noexcept {
    return withPeCallScope(
        "BeginScene", DXMT9_PE_CALLSITE_PC(), nullptr,
        [&](auto& peCall) __attribute__((always_inline)) noexcept
            -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("BeginScene", hr);
    };
    // T2 device-lost gate.
    if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
    dxmt9DeviceDebugLog("device_begin_scene device=%p", this);
    const HRESULT hr = hr32(dxmt9c_device_begin_scene(dev_));
    dxmt9DeviceDebugLog("device_begin_scene -> hr=0x%08x", (unsigned)hr);
    return finishPeCall(hr);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::EndScene()   noexcept {
    return withPeCallScope(
        "EndScene", DXMT9_PE_CALLSITE_PC(), nullptr,
        [&](auto& peCall) __attribute__((always_inline)) noexcept
            -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("EndScene", hr);
    };
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    // T2 device-lost gate.
    if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
    dxmt9DeviceDebugLog("device_end_scene device=%p", this);
    const HRESULT flushHr = flushPeRecorder();
    if (FAILED(flushHr)) return finishPeCall(flushHr);
    const HRESULT hr = hr32(dxmt9c_device_end_scene(dev_));
    dxmt9DeviceDebugLog("device_end_scene -> hr=0x%08x", (unsigned)hr);
    return finishPeCall(hr);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::Clear(DWORD count, const D3DRECT* pRects,
                                 DWORD flags, D3DCOLOR color,
                                 float z, DWORD stencil) noexcept {
    return withPeCallScope(
        "Clear", DXMT9_PE_CALLSITE_PC(), nullptr,
        [&](auto& peCall) __attribute__((always_inline)) noexcept
            -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("Clear", hr);
    };
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    // T2 device-lost gate.
    if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
    // Wine d3d9: Clear count/pRects must agree, and Z clears require
    // a bound depth-stencil. visual_clear_color_only_policy /
    // visual_depth_buffer_clear_policy.
    if (count == 0 && pRects != nullptr) {
        return finishPeCall(D3DERR_INVALIDCALL);
    }
    if (count > 0 && pRects == nullptr) {
        return finishPeCall(D3DERR_INVALIDCALL);
    }
    if ((flags & D3DCLEAR_ZBUFFER) && !dsSurfaceExplicit_) {
        D9CSurface* s = dxmt9c_device_get_depth_stencil(dev_);
        if (!s) return finishPeCall(D3DERR_INVALIDCALL);
        dxmt9c_surface_release(s);
    } else if ((flags & D3DCLEAR_ZBUFFER) && dsSurfaceExplicit_ && !dsSurface_) {
        return finishPeCall(D3DERR_INVALIDCALL);
    }
    dxmt9DeviceDebugLog("device_clear device=%p count=%u flags=0x%x color=0x%08x z=%f stencil=%u",
                        this, (unsigned)count, (unsigned)flags, (unsigned)color, z,
                        (unsigned)stencil);
    // Per recorder design: Clear is a standalone ordering record
    // inside the chunk — drains pending hot state + const dirty
    // ranges first so the chunk replays in API order, then
    // appends a CLEAR record carrying flags + color + z + stencil
    // + the optional rect array as a tail payload.
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return finishPeCall(barrierHr);

    const std::uint32_t rectBytes = static_cast<std::uint32_t>(count) * sizeof(D9CRect);
    // rectCount / rectOffset are computed by appendClear from the span,
    // so they stay zero here. sizeHint keeps the legacy header+payload size
    // the capacity precheck saw before, so seal cadence is unchanged.
    const D9CCommandChunkWireClear clearWire{
        .flags = (uint32_t)flags,
        .colorARGB = (uint32_t)color,
        .z = z,
        .stencil = (uint32_t)stencil,
        .rectCount = 0u,
        .rectOffset = 0u,
    };
    const std::span<const D9CRect> rects(
        reinterpret_cast<const D9CRect*>(pRects),
        pRects ? static_cast<std::size_t>(count) : 0u);
    const HRESULT hr = appendRecord(
        D9C_COMMAND_RECORD_CLEAR,
        kLegacyClearSizeHint + rectBytes,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok =
                dxmt9::d3d9::pe::appendClear(builder, clearWire, rects);
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    return finishPeCall(hr);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::DrawPrimitive(D3DPRIMITIVETYPE type,
                                         UINT startVertex,
                                         UINT count) noexcept {
    PeDiagnosticsState* const diagnostics = diagnostics_.get();
    if (!diagnostics || !diagnostics->gates.callScope) {
        return drawPrimitiveCore(
            type, startVertex, count, peNullCallScope_);
    }
    PeCallScope peCall(
        *diagnostics, "DrawPrimitive", DXMT9_PE_CALLSITE_PC(),
        &PeDiagnosticsState::peEntryDrawDecimatedStats_);
    return drawPrimitiveCore(type, startVertex, count, peCall);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::DrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                           UINT count,
                                           const void* pData,
                                           UINT stride) noexcept {
    return withPeCallScope(
        "DrawPrimitiveUP", DXMT9_PE_CALLSITE_PC(),
        &PeDiagnosticsState::peEntryDrawDecimatedStats_,
        [&](auto& peCall) __attribute__((always_inline)) noexcept
            -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("DrawPrimitiveUP", hr);
    };
    // T2 device-lost gate.
    if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
    dxmt9DeviceDebugLog("device_draw_primitive_up device=%p type=%u count=%u data=%p stride=%u",
                        this, (unsigned)type, count, pData, stride);
    if (recorderState_.peState.pendingRenderStatesTyped().size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return finishPeCall(barrierHr);
    }
    SoftwareFfpDrawData swvpDraw{};
    HRESULT hr = prepareSoftwareDrawCandidate([&] {
        HRESULT candidateHr = trySoftwareFfpDrawPrimitiveUP(
            type, count, pData, stride, swvpDraw);
        if (candidateHr == S_FALSE) {
            candidateHr = trySoftwareProgrammableDrawPrimitiveUP(
                type, count, pData, stride, swvpDraw);
        }
        if (candidateHr == S_OK) {
            candidateHr = filterSoftwareDrawOutsideClipPrimitives(swvpDraw);
        }
        return candidateHr;
    });
    bool appendedDraw = false;
    if (hr == S_OK) {
        dxmt9DeviceDebugLog("device_draw_primitive_up swvp_fallback device=%p fvf=0x%x stride=%u bytes=%zu",
                            this, (unsigned)swvpDraw.fvf, swvpDraw.stride,
                            swvpDraw.vertices.size());
        const UINT swvpPrimitiveCount = swvpDraw.primitiveCount
            ? swvpDraw.primitiveCount
            : count;
        if (swvpPrimitiveCount != 0u && !swvpDraw.vertices.empty()) {
            hr = appendDrawPrimitiveUPRecordWithFvf(
                swvpDraw.primitiveType, swvpPrimitiveCount,
                swvpDraw.vertices.data(), swvpDraw.stride,
                true, swvpDraw.fvf, swvpDraw.bypassVertexShader, true);
            appendedDraw = SUCCEEDED(hr);
        }
    } else if (hr == S_FALSE) {
        hr = appendDrawPrimitiveUPRecord(type, count, pData, stride);
        appendedDraw = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr) && appendedDraw) {
        if (!swvpDraw.vertices.empty()) {
            clearPendingHotState();
            recorderState_.peState.maintenance().pendingFvf() = true;
            recorderState_.peState.maintenance().pendingVdecl() = true;
            if (swvpDraw.bypassVertexShader) recorderState_.peState.maintenance().pendingVs() = true;
        }
    }
    return finishPeCall(hr);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                  UINT minVertex,
                                                  UINT numVertices,
                                                  UINT count,
                                                  const void* pIdxData,
                                                  D3DFORMAT idxFmt,
                                                  const void* pVtxData,
                                                  UINT stride) noexcept {
    return withPeCallScope(
        "DrawIndexedPrimitiveUP", DXMT9_PE_CALLSITE_PC(),
        &PeDiagnosticsState::peEntryDrawDecimatedStats_,
        [&](auto& peCall) __attribute__((always_inline)) noexcept
            -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("DrawIndexedPrimitiveUP", hr);
    };
    // T2 device-lost gate.
    if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
    dxmt9DeviceDebugLog("device_draw_indexed_primitive_up device=%p type=%u min=%u num=%u count=%u idx=%p idxFmt=%u vtx=%p stride=%u",
                        this, (unsigned)type, minVertex, numVertices, count,
                        pIdxData, (unsigned)idxFmt, pVtxData, stride);
    if (recorderState_.peState.pendingRenderStatesTyped().size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return finishPeCall(barrierHr);
    }
    SoftwareFfpDrawData swvpDraw{};
    std::vector<std::uint8_t> swvpIndices{};
    D3DFORMAT swvpIndexFormat = idxFmt;
    bool useSwvpIndices = false;
    HRESULT hr = prepareSoftwareDrawCandidate([&] {
        HRESULT candidateHr = trySoftwareFfpDrawIndexedPrimitiveUP(
            type, minVertex, numVertices, count, pVtxData, stride, swvpDraw);
        if (candidateHr == S_FALSE) {
            candidateHr = trySoftwareProgrammableDrawIndexedPrimitiveUP(
                type, minVertex, numVertices, count, pVtxData, stride,
                swvpDraw);
        }
        if (candidateHr == S_OK &&
            renderStateValue(D3DRS_CLIPPING) != FALSE) {
            const UINT indexSize = idxFmt == D3DFMT_INDEX32 ? 4u : 2u;
            std::uint32_t indexBytes = 0;
            if (!checkedByteCount(primitiveVertexCount(type, count), indexSize,
                                  indexBytes) ||
                (indexBytes != 0u && !pIdxData)) {
                return D3DERR_INVALIDCALL;
            }
            const auto preparation = dxmt9::d3d9::pe::prepareSwvpIndices(
                swvpIndices, pIdxData, indexBytes,
                [&](auto& candidate) {
                    candidateHr = filterSoftwareIndexedDrawOutsideClipPrimitives(
                        swvpDraw, candidate, swvpIndexFormat);
                    return SUCCEEDED(candidateHr);
                });
            if (preparation ==
                dxmt9::d3d9::pe::PublicAllocationResult::OutOfMemory) {
                return E_OUTOFMEMORY;
            }
            if (preparation ==
                    dxmt9::d3d9::pe::PublicAllocationResult::Rejected &&
                SUCCEEDED(candidateHr)) {
                return D3DERR_INVALIDCALL;
            }
            useSwvpIndices = preparation ==
                dxmt9::d3d9::pe::PublicAllocationResult::Completed;
        }
        return candidateHr;
    });
    bool appendedDraw = false;
    if (hr == S_OK) {
        dxmt9DeviceDebugLog("device_draw_indexed_primitive_up swvp_fallback device=%p fvf=0x%x stride=%u bytes=%zu",
                            this, (unsigned)swvpDraw.fvf, swvpDraw.stride,
                            swvpDraw.vertices.size());
        const UINT swvpPrimitiveCount = swvpDraw.primitiveCount
            ? swvpDraw.primitiveCount
            : count;
        const void* indexData = useSwvpIndices ? swvpIndices.data() : pIdxData;
        const D3DFORMAT indexFormat =
            useSwvpIndices ? swvpIndexFormat : idxFmt;
        const UINT swvpMinVertex = useSwvpIndices ? 0u : minVertex;
        const UINT swvpNumVertices =
            useSwvpIndices && swvpDraw.stride != 0u
                ? static_cast<UINT>(swvpDraw.vertices.size() / swvpDraw.stride)
                : numVertices;
        if (swvpPrimitiveCount != 0u && !swvpDraw.vertices.empty() &&
            indexData) {
            hr = appendDrawIndexedPrimitiveUPRecordWithFvf(
                swvpDraw.primitiveType, swvpMinVertex, swvpNumVertices,
                swvpPrimitiveCount, indexData, indexFormat,
                swvpDraw.vertices.data(), swvpDraw.stride, true, swvpDraw.fvf,
                swvpDraw.bypassVertexShader, true);
            appendedDraw = SUCCEEDED(hr);
        }
    } else if (hr == S_FALSE) {
        hr = appendDrawIndexedPrimitiveUPRecord(type, minVertex, numVertices,
                                               count, pIdxData, idxFmt,
                                               pVtxData, stride);
        appendedDraw = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr) && appendedDraw) {
        if (!swvpDraw.vertices.empty()) {
            clearPendingHotState();
            recorderState_.peState.maintenance().pendingFvf() = true;
            recorderState_.peState.maintenance().pendingVdecl() = true;
            if (swvpDraw.bypassVertexShader) recorderState_.peState.maintenance().pendingVs() = true;
        }
    }
    return finishPeCall(hr);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::PresentEx(const RECT* src, const RECT* dst,
                                     HWND wnd, const RGNDATA* dirty,
                                     DWORD flags) noexcept {
    dxmt9PeSetCurrentCallName("PresentEx");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    // T2 device-lost gate.
    if (deviceNotReset_) {
        if (peCaptureState_)
            abortRenderTapeCapture("device_lost");
        return D3DERR_DEVICELOST;
    }
    const bool renderTapeCaptureWasActive =
        peCaptureState_ &&
        peCaptureState_->renderTapeCapture.state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing;
    D9CRect cs{}, cd{};
    if (src) cs = toR(*src); if (dst) cd = toR(*dst);
    const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::Present);
    if (FAILED(flushHr)) return flushHr;
    const HRESULT hr = hr32(dxmt9c_device_present(dev_,
        src ? &cs : nullptr, dst ? &cd : nullptr,
        (uint64_t)(uintptr_t)wnd, dirty, flags));
    if (SUCCEEDED(hr)) {
        logPeRecorderStats("present_ex");
        markPePresentReturnedForCadence();
        if (renderTapeCaptureWasActive)
            finishRenderTapeCaptureAtPresentBoundary();
        else if (peCaptureState_)
            (void)armRenderTapeCaptureAtPresentBoundary();
    }
    return hr;
}



void D3D9DeviceImpl::initGammaRampIdentity() noexcept {
    for (UINT i = 0; i < 256; ++i) {
        const WORD v = static_cast<WORD>(i << 8);
        gammaRamp_.red[i]   = v;
        gammaRamp_.green[i] = v;
        gammaRamp_.blue[i]  = v;
    }
}

StateBlockTextureRef D3D9DeviceImpl::stateBlockTextureRef(
    IDirect3DBaseTexture9* value) const noexcept {
    D3D9PeValidatedTexture validated{};
    if (FAILED(D3D9PeValidateTexture(
            value, static_cast<const IDirect3DDevice9*>(this), &validated)))
        return {};
    return StateBlockComRefFactory<StateBlockTextureTag>::fromValidated(
        validated.template stateBlockCapability<StateBlockTextureTag>());
}

StateBlockStreamSourceValue::BufferRef D3D9DeviceImpl::stateBlockBufferRef(
    IDirect3DVertexBuffer9* value) const noexcept {
    D3D9PeValidatedVertexBuffer validated{};
    if (FAILED(D3D9PeValidateVertexBuffer(
            value, static_cast<const IDirect3DDevice9*>(this), &validated)))
        return {};
    return StateBlockBufferRefFactory::fromValidated(
        validated.stateBlockBufferCapability());
}

StateBlockVertexShaderRef D3D9DeviceImpl::stateBlockVertexShaderRef(
    IDirect3DVertexShader9* value) const noexcept {
    D3D9PeValidatedVertexShader validated{};
    if (FAILED(D3D9PeValidateVertexShader(
            value, static_cast<const IDirect3DDevice9*>(this), &validated)))
        return {};
    return StateBlockComRefFactory<StateBlockVertexShaderTag>::fromValidated(
        validated.template stateBlockCapability<StateBlockVertexShaderTag>());
}

StateBlockPixelShaderRef D3D9DeviceImpl::stateBlockPixelShaderRef(
    IDirect3DPixelShader9* value) const noexcept {
    D3D9PeValidatedPixelShader validated{};
    if (FAILED(D3D9PeValidatePixelShader(
            value, static_cast<const IDirect3DDevice9*>(this), &validated)))
        return {};
    return StateBlockComRefFactory<StateBlockPixelShaderTag>::fromValidated(
        validated.template stateBlockCapability<StateBlockPixelShaderTag>());
}

StateBlockVertexDeclarationRef D3D9DeviceImpl::stateBlockVertexDeclarationRef(
    IDirect3DVertexDeclaration9* value) const noexcept {
    D3D9PeValidatedDeclaration validated{};
    if (FAILED(D3D9PeValidateVertexDecl(
            value, static_cast<const IDirect3DDevice9*>(this), &validated)))
        return {};
    return StateBlockComRefFactory<StateBlockVertexDeclarationTag>::fromValidated(
        validated.template stateBlockCapability<StateBlockVertexDeclarationTag>());
}

StateBlockIndexBufferRef D3D9DeviceImpl::stateBlockIndexBufferRef(
    IDirect3DIndexBuffer9* value) const noexcept {
    D3D9PeValidatedIndexBuffer validated{};
    if (FAILED(D3D9PeValidateIndexBuffer(
            value, static_cast<const IDirect3DDevice9*>(this), &validated)))
        return {};
    return StateBlockComRefFactory<StateBlockIndexBufferTag>::fromValidated(
        validated.template stateBlockCapability<StateBlockIndexBufferTag>());
}

StateBlockRenderTargetRef D3D9DeviceImpl::stateBlockSurfaceRef(
    IDirect3DSurface9* value) const noexcept {
    D3D9PeValidatedSurface validated{};
    if (FAILED(D3D9PeValidateSurface(
            value, static_cast<const IDirect3DDevice9*>(this), &validated)))
        return {};
    return StateBlockComRefFactory<StateBlockRenderTargetTag>::fromValidated(
        validated.template stateBlockCapability<StateBlockRenderTargetTag>());
}

StateBlockDepthStencilRef D3D9DeviceImpl::stateBlockDepthStencilRef(
    IDirect3DSurface9* value) const noexcept {
    D3D9PeValidatedSurface validated{};
    if (FAILED(D3D9PeValidateSurface(
            value, static_cast<const IDirect3DDevice9*>(this), &validated)))
        return {};
    return StateBlockComRefFactory<StateBlockDepthStencilTag>::fromValidated(
        validated.template stateBlockCapability<StateBlockDepthStencilTag>());
}

void D3D9DeviceImpl::discardPreparedStateBlockApply() noexcept {
    if (recorderState_.stateBlockTransaction.isApplyPrepared()) {
        recorderState_.stateBlockTransaction.failPreparedApply(
            d3d9PeReleaseStateBlockRef);
    } else {
        recorderState_.stateBlockTransaction.discardPrepared(
            d3d9PeReleaseStateBlockRef);
    }
}

void D3D9DeviceImpl::poisonStateBlockTransaction() noexcept {
    recorderState_.stateBlockTransaction.poison(
        d3d9PeReleaseStateBlockRef);
}

void D3D9DeviceImpl::releaseRecordedStateBlockRefs() noexcept {
    recorderState_.stateBlockTransaction.abandonRecording(
        d3d9PeReleaseStateBlockRef);
}

D9CTexture* D3D9DeviceImpl::validatedRawTexture(IDirect3DBaseTexture9* texture) const noexcept {
    D3D9PeValidatedTexture validated{};
    return SUCCEEDED(D3D9PeValidateTexture(
        texture, static_cast<const IDirect3DDevice9*>(this), &validated))
        ? validated.raw() : nullptr;
}

D9CSurface* D3D9DeviceImpl::validatedRawSurface(IDirect3DSurface9* surface) const noexcept {
    D3D9PeValidatedSurface validated{};
    return SUCCEEDED(D3D9PeValidateSurface(
        surface, static_cast<const IDirect3DDevice9*>(this), &validated))
        ? validated.raw() : nullptr;
}

bool D3D9DeviceImpl::applyCurrentPaletteToTexture(IDirect3DBaseTexture9* texture) {
    if (!texture || !currentPaletteSet_) return false;
    const auto it = palettes_.find(currentPaletteIndex_);
    if (it == palettes_.end()) return false;
    D9CTexture* raw = validatedRawTexture(texture);
    if (!raw) return false;
    std::array<uint32_t, 256> argb{};
    for (UINT i = 0; i < 256; ++i) {
        const PALETTEENTRY& entry = it->second[i];
        argb[i] = (static_cast<uint32_t>(entry.peFlags) << 24) |
                  (static_cast<uint32_t>(entry.peRed) << 16) |
                  (static_cast<uint32_t>(entry.peGreen) << 8) |
                  static_cast<uint32_t>(entry.peBlue);
    }
    return SUCCEEDED(hr32(dxmt9c_texture_set_palette(
        raw, argb.data(), static_cast<uint32_t>(argb.size()))));
}

void D3D9DeviceImpl::applyCurrentPaletteToBoundTextures() {
    for (auto* texture : textures_) {
        applyCurrentPaletteToTexture(texture);
    }
}

DWORD D3D9DeviceImpl::renderStateValue(D3DRENDERSTATETYPE state) const {
    uint32_t shadowValue = 0;
    if (recorderState_.peState.renderStateShadowTyped().get(
            renderStateSlotKey(static_cast<uint32_t>(state)), shadowValue)) {
        return shadowValue;
    }
    return dxmt9c_device_get_render_state(dev_, static_cast<uint32_t>(state));
}

D9CSwapChain* D3D9DeviceImpl::borrowSwapChainHandle(UINT index) {
    if (const auto it = swapchainHandles_.find(index);
        it != swapchainHandles_.end()) {
        return it->second;
    }
    D9CSwapChain* chain =
        dev_ ? dxmt9c_device_get_swap_chain(dev_, index) : nullptr;
    if (!chain) {
        return nullptr;
    }
    try {
        const auto [it, inserted] = swapchainHandles_.emplace(index, chain);
        if (!inserted) {
            dxmt9c_swapchain_release(chain);
            return it->second;
        }
    } catch (...) {
        dxmt9c_swapchain_release(chain);
        return nullptr;
    }
    return chain;
}

void D3D9DeviceImpl::releaseAllBound() {
    for (auto& t : textures_)   setRef(t, (IDirect3DBaseTexture9*)nullptr);
    setRef(vs_, (IDirect3DVertexShader9*)nullptr);
    setRef(ps_, (IDirect3DPixelShader9*)nullptr);
    for (auto& s : streamSrc_)  setRef(s, (IDirect3DVertexBuffer9*)nullptr);
    setRef(indexBuf_, (IDirect3DIndexBuffer9*)nullptr);
    /* vdecl_ is a borrowed pointer (Wine refcount semantics — see
     * SetVertexDeclaration), so no Release here. The underlying
     * decl is either user-owned (user keeps it alive while bound)
     * or implicit-FVF and released via fvfDeclCache_ below. */
    vdecl_ = nullptr;
    /* Drop the FVF→decl shadow cache. The map owns one ref per entry
     * (held since the cache miss in SetFVF created the decl). */
    for (auto& [fvf, decl] : fvfDeclCache_) {
        if (decl) decl->Release();
    }
    fvfDeclCache_.clear();
    for (auto& rt : rtSlots_)   setRef(rt, (IDirect3DSurface9*)nullptr);
    for (auto& explicitRt : rtSlotExplicit_) explicitRt = false;
    setRef(dsSurface_, (IDirect3DSurface9*)nullptr);
    dsSurfaceExplicit_ = false;
    setRef(cachedBackBuffer0_, (IDirect3DSurface9*)nullptr);
    for (auto& [idx, sc] : swapchainWrappers_) {
        if (sc) sc->Release();
    }
    swapchainWrappers_.clear();
    for (auto& [idx, chain] : swapchainHandles_) {
        if (chain) dxmt9c_swapchain_release(chain);
    }
    swapchainHandles_.clear();
    // T2 device-lost: explicitly nullify the device's primary RT slot
    // and depth-stencil on the C side so no stale Metal surface handle
    // survives a Reset(). The PE shadow's pendingRtMask/pendingDs is
    // cleared via clearPendingHotState() in clearPeStateTracking, but
    // the server-side core::Device state must also lose the prior
    // attachment references — invalidateDefaultPoolResources() clears
    // the resource itself but not the bound-slot pointer.
    if (dev_) {
        (void)dxmt9c_device_set_render_target(dev_, 0, nullptr);
        (void)dxmt9c_device_set_depth_stencil(dev_, nullptr);
    }
}

dxmt9::d3d9::pe::PeScalarSemanticTokenLedger*
D3D9DeviceImpl::scalarSemanticObserver() noexcept {
    return diagnostics_ ? diagnostics_->scalarSemanticTokens.get() : nullptr;
}

void D3D9DeviceImpl::clearPeStateTracking() {
    recorderState_.peState.maintenance().clearServerShadowTables();
    recorderState_.peState.consume().clearPendingHotState();
    recorderState_.peConsts.reset();
    recorderState_.peBindingView = {};
    if (auto* tokens = scalarSemanticObserver()) tokens->clear();
    recorderState_.stateBlockTransaction.clearRecordedCandidateForReset();
    clearPendingCommandChunk(
        dxmt9::d3d9::pe::RecorderCommitEvent::DeviceReset);
    submittedIndexBufferWireValue_ = 0;
    submittedIndexBufferKnown_ = false;
    fvf_ = 0;
    std::memset(streamOff_, 0, sizeof(streamOff_));
    std::memset(streamStr_, 0, sizeof(streamStr_));
    // D3D9 default stream-source frequency divider is 1 (not 0,
    // which encodes an invalid value -- a zero divider with no
    // INDEXED/INSTANCE flag is rejected at Set time). Restoring 1
    // here keeps Reset()-then-Get round-trips consistent with
    // test_stream_source_frequency_state.
    for (UINT& freq : streamFreq_) {
        freq = 1;
    }
}

bool D3D9DeviceImpl::hasPendingHotState() const {
    return recorderState_.peState.hasPendingHotState();
}

void D3D9DeviceImpl::clearPendingHotState() {
    recorderState_.peState.consume().clearPendingHotState();
}

bool D3D9DeviceImpl::shadowedRenderStateEquals(DWORD state, DWORD value) const {
    return recorderState_.peState.renderStateEqualsTyped(renderStateSlotKey(state), value);
}

dxmt9::d3d9::pe::PeRtExplicitMask D3D9DeviceImpl::currentRtExplicitMask() const {
    dxmt9::d3d9::pe::PeRtExplicitMask explicitMask{};
    for (DWORD slot = 0; slot < 4; ++slot) {
        explicitMask[slot] = rtSlotExplicit_[slot];
    }
    return explicitMask;
}

std::uint64_t D3D9DeviceImpl::currentVertexShaderHash() const noexcept {
    if (!diagnostics_) return 0u;
    D3D9PeValidatedVertexShader validated{};
    return SUCCEEDED(D3D9PeValidateVertexShader(
        vs_, static_cast<const IDirect3DDevice9*>(this), &validated))
        ? validated.localMetadata() : 0u;
}

std::uint64_t D3D9DeviceImpl::currentPixelShaderHash() const noexcept {
    if (!diagnostics_) return 0u;
    D3D9PeValidatedPixelShader validated{};
    return SUCCEEDED(D3D9PeValidatePixelShader(
        ps_, static_cast<const IDirect3DDevice9*>(this), &validated))
        ? validated.localMetadata() : 0u;
}

void D3D9DeviceImpl::populateBindingView(dxmt9::d3d9::pe::PeBindingView& view,
                         bool needAllSlots,
                         bool allStreams) const {
    // Object refs are admitted and cached only through the kind-qualified
    // validation capabilities used by their public setters.  This helper
    // therefore refreshes scalar binding metadata only; it never recovers
    // a wire ref by convention-casting a public COM pointer.
    (void)needAllSlots;
    for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
        if (!needAllSlots && !allStreams &&
            (recorderState_.peState.pendingStreamMask() & (1u << slot)) == 0) {
            continue;
        }
        view.streams[slot].offset = streamOff_[slot];
        view.streams[slot].stride = streamStr_[slot];
    }
    view.rtExplicitMask = currentRtExplicitMask();
    view.fvf = fvf_;
}

dxmt9::d3d9::pe::PeChunkContext D3D9DeviceImpl::currentChunkContext() const {
    dxmt9::d3d9::pe::PeChunkContext chunk{};
    for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
        if (pendingChunkReferencesBuffer(recorderState_.peBindingView.streams[slot].buffer)) {
            chunk.retainedStreamMask |= 1u << slot;
        }
    }
    chunk.indexBufferKnown = submittedIndexBufferKnown_;
    chunk.submittedIndexBufferWire = submittedIndexBufferWireValue_;
    chunk.indexBufferRetained =
        pendingChunkReferencesBuffer(recorderState_.peBindingView.indexBuffer);
    return chunk;
}

bool D3D9DeviceImpl::buildSparseStateForRecord(
    const dxmt9::d3d9::pe::PeDrawParams& params,
    bool forceFullSnapshot,
    bool inlineConstDelta) {
    const std::uint32_t recordType = params.recordType;
    // Choke point for all three callers. addChunkContextSections carries the
    // same guard and the full story of what an unstamped recordType did, but
    // it is only reached by draws -- the chunkBarrierFlush APPLY_STATE path
    // would slip past it and be misclassified as a draw by isDraw below.
    // Returning false surfaces as a failed HRESULT, not silent corruption.
    if (recordType == 0u) {
        return false;
    }
    DxmtPeDecimatedScopeGuard decimatedScope;
    const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
    if (decimationN != 0 &&
        PeDecimatedScopeTimer::shouldSample(
            diagnostics_->peDrawPacketDecimatedStats_, decimationN)) {
        decimatedScope.stats = &diagnostics_->peDrawPacketDecimatedStats_;
        {
            const auto n0 = std::chrono::steady_clock::now();
            const auto n1 = std::chrono::steady_clock::now();
            PeDecimatedScopeTimer::recordSample(
                peDecimatedNullScopeStats(),
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(n1 - n0).count()));
        }
        decimatedScope.t0 = std::chrono::steady_clock::now();
    }
    const bool needAllSlots =
        forceFullSnapshot || dxmt9::d3d9::pe::dxmt9PeFullSnapshotEnabled();
    const bool isDraw =
        recordType != D9C_COMMAND_RECORD_APPLY_STATE;
    populateBindingView(recorderState_.peBindingView, needAllSlots, isDraw);
    return dxmt9::d3d9::pe::buildSparseState(
        recorderState_.peState, recorderState_.peConsts, recorderState_.peBindingView, recorderState_.peSparsePayloads, params,
        forceFullSnapshot, inlineConstDelta, recorderState_.peSparseScratch,
        recorderState_.peSparseHeader, recorderState_.peSparseState);
}

bool D3D9DeviceImpl::buildSparseStatePlanForRecord(
    const dxmt9::d3d9::pe::PeDrawParams& params,
    const dxmt9::d3d9::pe::PeDrawPayloads& payloads,
    dxmt9::d3d9::pe::SparseStatePlan& plan,
    bool forceFullSnapshot,
    bool inlineConstDelta) {
    if (params.recordType == 0u) {
        return false;
    }
    DxmtPeDecimatedScopeGuard decimatedScope;
    const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
    if (decimationN != 0 &&
        PeDecimatedScopeTimer::shouldSample(
            diagnostics_->peDrawPacketDecimatedStats_, decimationN)) {
        decimatedScope.stats = &diagnostics_->peDrawPacketDecimatedStats_;
        {
            const auto n0 = std::chrono::steady_clock::now();
            const auto n1 = std::chrono::steady_clock::now();
            PeDecimatedScopeTimer::recordSample(
                peDecimatedNullScopeStats(),
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(n1 - n0).count()));
        }
        decimatedScope.t0 = std::chrono::steady_clock::now();
    }
    const bool needAllSlots =
        forceFullSnapshot || dxmt9::d3d9::pe::dxmt9PeFullSnapshotEnabled();
    const bool isDraw =
        params.recordType != D9C_COMMAND_RECORD_APPLY_STATE;
    populateBindingView(recorderState_.peBindingView, needAllSlots, isDraw);
    return dxmt9::d3d9::pe::buildSparseStatePlan(
        recorderState_.peState, recorderState_.peConsts,
        recorderState_.peBindingView, payloads, params, forceFullSnapshot,
        inlineConstDelta, plan);
}

UINT D3D9DeviceImpl::primitiveVertexCount(D3DPRIMITIVETYPE type, UINT primitiveCount) {
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

bool D3D9DeviceImpl::checkedByteCount(UINT count, UINT stride, std::uint32_t& bytes) {
    const auto value = static_cast<std::uint64_t>(count) * stride;
    if (value > 0xffffffffull) {
        return false;
    }
    bytes = static_cast<std::uint32_t>(value);
    return true;
}

D3D9DeviceImpl::PePresentCadenceClaim
D3D9DeviceImpl::claimPeFirstCallAfterPresent() {
    if (!dxmt9PeRecorderStatsEnabled()) {
        return {};
    }
    std::uint64_t ordinal =
        diagnostics_->pePresentCadencePendingOrdinal_.load(std::memory_order_acquire);
    if (ordinal == 0) {
        return {};
    }
    const auto entry = std::chrono::steady_clock::now();
    if (!diagnostics_->pePresentCadencePendingOrdinal_.compare_exchange_strong(
            ordinal, 0, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return {};
    }
    return PePresentCadenceClaim{
        true, ordinal,
        diagnostics_->pePresentCadenceReturnNs_.load(std::memory_order_acquire),
        dxmt9SteadyClockNs(entry)};
}

void D3D9DeviceImpl::markPePresentReturnedForCadence() {
    if (!dxmt9PeRecorderStatsEnabled()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t ordinal =
        diagnostics_->pePresentCadenceOrdinal_.fetch_add(1, std::memory_order_relaxed) + 1;
    diagnostics_->pePresentCadenceReturnNs_.store(dxmt9SteadyClockNs(now),
                                    std::memory_order_release);
    diagnostics_->pePresentCadencePendingOrdinal_.store(ordinal, std::memory_order_release);
    diagnostics_->pePresentCallCount_.store(0, std::memory_order_release);
    diagnostics_->pePresentCallMilestoneMask_.store(0, std::memory_order_release);
    diagnostics_->pePresentCallMilestonePendingOrdinal_.store(ordinal,
                                               std::memory_order_release);
    diagnostics_->pePresentChunkPendingOrdinal_.store(ordinal, std::memory_order_release);
    diagnostics_->pePresentRecordMilestoneMask_.store(0, std::memory_order_release);
    diagnostics_->pePresentRecordPendingOrdinal_.store(ordinal, std::memory_order_release);
}

D3D9DeviceImpl::PePresentCadenceClaim
D3D9DeviceImpl::claimPeFirstChunkAfterPresent() {
    if (!dxmt9PeRecorderStatsEnabled()) {
        return {};
    }
    std::uint64_t ordinal =
        diagnostics_->pePresentChunkPendingOrdinal_.load(std::memory_order_acquire);
    if (ordinal == 0) {
        return {};
    }
    const auto entry = std::chrono::steady_clock::now();
    if (!diagnostics_->pePresentChunkPendingOrdinal_.compare_exchange_strong(
            ordinal, 0, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return {};
    }
    return PePresentCadenceClaim{
        true, ordinal,
        diagnostics_->pePresentCadenceReturnNs_.load(std::memory_order_acquire),
        dxmt9SteadyClockNs(entry)};
}

void D3D9DeviceImpl::resetPeBetweenCallsWindow() {
    diagnostics_->peRecorderBetweenCallsActive_ = false;
    diagnostics_->peRecorderBetweenCallsStartNs_ = 0;
    diagnostics_->peRecorderBetweenCallFamilySamples_.fill(0);
    diagnostics_->peRecorderBetweenCallNameSamples_.fill(0);
    diagnostics_->peRecorderBetweenCallNameCpuNsTotal_.fill(0);
    diagnostics_->peRecorderBetweenCallNameCpuNsMax_.fill(0);
    diagnostics_->peRecorderBetweenLastCallFamily_ = PeInterAppendCallFamily::Unknown;
    diagnostics_->peRecorderBetweenLastCallName_ = PeInterAppendCallName::Unknown;
    diagnostics_->peRecorderBetweenLastCallExitNs_ = 0;
    diagnostics_->peRecorderBetweenCallTransitionSamples_.fill(0);
    diagnostics_->peRecorderBetweenCallTransitionNsTotal_.fill(0);
    diagnostics_->peRecorderBetweenCallTransitionNsMax_.fill(0);
    diagnostics_->peRecorderBetweenCallNameTransitionSamples_.fill(0);
    diagnostics_->peRecorderBetweenCallNameTransitionNsTotal_.fill(0);
    diagnostics_->peRecorderBetweenCallNameTransitionNsMax_.fill(0);
    diagnostics_->peRecorderBetweenCallNameTransitionSites_.clear();
    diagnostics_->peRecorderBetweenCallBodyCalls_ = 0;
    diagnostics_->peRecorderBetweenCallBodyCpuNsTotal_ = 0;
    diagnostics_->peRecorderBetweenCallBodyCpuNsMax_ = 0;
}

void D3D9DeviceImpl::stopPeThreadSampler() {
    if (!diagnostics_ || !diagnostics_->peThreadSampler_) {
        return;
    }
    dxmt9::d3d9::pe::PeThreadSampler::stopAndRelease(diagnostics_->peThreadSampler_);
    diagnostics_->peThreadSampler_ = nullptr;
}

void D3D9DeviceImpl::notePeThreadSamplerPresent() {
    if (!diagnostics_ || !diagnostics_->peThreadSampler_) {
        return;
    }
    // The sampler targets the thread that created the device on the
    // assumption that it is also the thread that renders. If an app splits
    // those, every sample describes the wrong thread and nothing else in
    // the output would say so — the histogram would just look idle. Say it
    // once, loudly, instead of leaving a silently wrong answer.
    if (!diagnostics_->peThreadSamplerPresentThreadChecked_) {
        diagnostics_->peThreadSamplerPresentThreadChecked_ = true;
        const DWORD presentThread = GetCurrentThreadId();
        if (presentThread != diagnostics_->peThreadSampler_->targetThreadId()) {
            dxmt9PeThreadSamplerInfoLog(
                "target_thread_mismatch sampled=0x%lx present=0x%lx "
                "note=samples_describe_the_device_creating_thread_not_the_present_thread",
                static_cast<unsigned long>(diagnostics_->peThreadSampler_->targetThreadId()),
                static_cast<unsigned long>(presentThread));
        }
    }
    ++diagnostics_->peThreadSamplerPresents_;
    if (diagnostics_->peThreadSamplerPresents_ % 60 == 0) {
        logPeThreadSampler();
    }
}

void D3D9DeviceImpl::notePeStatsDecimationPresent() {
    if (dxmt9PeStatsDecimationN() == 0) {
        return;
    }
    ++diagnostics_->peStatsDecimationPresents_;
    if (diagnostics_->peStatsDecimationPresents_ % 60 == 0) {
        logPeStatsDecimation();
    }
}

void D3D9DeviceImpl::recordDrawPrimitiveUPCopy(std::uint32_t vertexBytes) {
    peDiagnosticsCall(diagnostics_.get(),
        [vertexBytes](PeDiagnosticsState& diagnostics) noexcept {
            if (!diagnostics.config.recorderStats) {
                return;
            }
            ++diagnostics.peRecorderStats_.drawPrimitiveUPCalls;
            diagnostics.peRecorderStats_.upVertexBytes += vertexBytes;
        });
}

void D3D9DeviceImpl::recordDrawIndexedPrimitiveUPCopy(std::uint32_t vertexBytes,
                                      std::uint32_t indexBytes) {
    peDiagnosticsCall(diagnostics_.get(),
        [vertexBytes, indexBytes](PeDiagnosticsState& diagnostics) noexcept {
            if (!diagnostics.config.recorderStats) {
                return;
            }
            ++diagnostics.peRecorderStats_.drawIndexedPrimitiveUPCalls;
            diagnostics.peRecorderStats_.upVertexBytes += vertexBytes;
            diagnostics.peRecorderStats_.upIndexBytes += indexBytes;
        });
}

bool D3D9DeviceImpl::chunkHasPresentRecord(const D9CCommandChunk& chunk) noexcept {
    if (chunk.recordBytes < sizeof(D9CCommandChunkWireHeader) ||
        d9cWireHandleValue(chunk.records) == 0u) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(
        static_cast<std::uintptr_t>(d9cWireHandleValue(chunk.records)));
    D9CCommandChunkWireHeader header{};
    std::memcpy(&header, bytes, sizeof(header));
    if (header.recordCount != chunk.recordCount ||
        header.recordHeaderSize != sizeof(D9CCommandChunkWireRecordHeader) ||
        header.recordTableOffset > chunk.recordBytes ||
        header.recordCount >
            (chunk.recordBytes - header.recordTableOffset) /
                sizeof(D9CCommandChunkWireRecordHeader)) {
        return false;
    }
    for (std::uint32_t index = 0u; index < header.recordCount; ++index) {
        D9CCommandChunkWireRecordHeader record{};
        std::memcpy(&record,
                    bytes + header.recordTableOffset +
                        static_cast<std::size_t>(index) * sizeof(record),
                    sizeof(record));
        if (record.type == D9C_COMMAND_RECORD_PRESENT) {
            return true;
        }
    }
    return false;
}

HRESULT D3D9DeviceImpl::appendDrawPrimitiveRecord(D3DPRIMITIVETYPE type, UINT startVertex, UINT count) {
    Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Draw);
    // Hold the recorder lock across the const-flush/fold + draw-record
    // append pair: recorderState_.recorderMutex is recursive, so the nested per-append
    // acquisitions below become cheap re-entries instead of repeated
    // cold lock/unlock cycles on this hot path.
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const bool inlineConstDelta = dxmt9PeInlineConstDeltaEnabled();
    if (!inlineConstDelta) {
        // Drain any accumulated const dirty ranges into chunk records
        // FIRST, so the chunk replays "consts → draw" in API order.
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
    }
    dxmt9::d3d9::pe::PeDrawParams params{};
    params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
    params.primitiveType = static_cast<std::uint32_t>(type);
    params.startVertex = startVertex;
    params.primitiveCount = count;
    // Under inlineConstDelta the const shadows are still dirty here. The
    // producer prepares their constant-range sections; the emitter settles
    // them only after appendSparseRecord accepts the record.
    dxmt9::d3d9::pe::SparseStatePlan sparsePlan{};
    if (!buildSparseStatePlanForRecord(
            params, dxmt9::d3d9::pe::PeDrawPayloads{}, sparsePlan,
            /*forceFullSnapshot=*/false, inlineConstDelta)) {
        return D3DERR_INVALIDCALL;
    }
    return appendRecord(
        D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
        kLegacyDrawPrimitiveSizeHint +
            dxmt9::d3d9::pe::sparseStatePlanConstantPayloadBytes(sparsePlan),
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            // Inside the emitter on purpose: CapacityPre may have sealed the
            // old chunk, and the retention answers are about the chunk this
            // record actually lands in.
            if (!dxmt9::d3d9::pe::finalizeSparseStatePlanChunkContext(
                    currentChunkContext(), sparsePlan)) {
                return D3DERR_INVALIDCALL;
            }
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendSparseStatePlan(
                builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, sparsePlan);
            const auto settlement =
                dxmt9::d3d9::pe::settleRecorderAppend({
                    .phase =
                        dxmt9::d3d9::pe::AppendSettlement::Prepared,
                    .appendSucceeded = ok,
                });
            const bool settled =
                dxmt9::d3d9::pe::acceptSparseStatePlan(
                    recorderState_.peState, recorderState_.peConsts,
                    sparsePlan, settlement,
                    scalarSemanticObserver(),
                    builder.activeRecordOrdinal());
            phase.recordEncode(t0);
            if (ok && !settled) {
                // The record is already durable in the builder, but its
                // semantic settlement failed.  Retrying could duplicate
                // the accepted wire record, so enter the existing
                // recorder fail-stop state rather than returning S_OK
                // with stale PendingDelta.
                poisonStateBlockTransaction();
                return D3DERR_DEVICELOST;
            }
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
}

HRESULT D3D9DeviceImpl::appendDrawIndexedPrimitiveRecord(D3DPRIMITIVETYPE type,
                                         INT baseVertex,
                                         UINT minVertex,
                                         UINT numVertices,
                                         UINT startIndex,
                                         UINT count) {
    Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Draw);
    // See appendDrawPrimitiveRecord: recursive re-entry on an
    // already-held recorderState_.recorderMutex is cheaper than the repeated cold
    // acquisitions the nested const-flush + draw appends would do.
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const bool inlineConstDelta = dxmt9PeInlineConstDeltaEnabled();
    if (!inlineConstDelta) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
    }
    dxmt9::d3d9::pe::PeDrawParams params{};
    params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
    params.primitiveType = static_cast<std::uint32_t>(type);
    params.baseVertex = baseVertex;
    params.minVertex = minVertex;
    params.numVertices = numVertices;
    params.startIndex = startIndex;
    params.primitiveCount = count;
    dxmt9::d3d9::pe::SparseStatePlan sparsePlan{};
    if (!buildSparseStatePlanForRecord(
            params, dxmt9::d3d9::pe::PeDrawPayloads{}, sparsePlan,
            /*forceFullSnapshot=*/false, inlineConstDelta)) {
        return D3DERR_INVALIDCALL;
    }
    const std::uint64_t ibWireValue =
        d9cWireHandleValue(toWireHandle(recorderState_.peBindingView.indexBuffer.object));
    // Whether the index section was actually emitted decides the tracking
    // update, exactly as the legacy code keyed it on the final ibValid --
    // which the append-time dependency checkpoint could itself set.
    bool indexSectionEmitted = false;
    const HRESULT hr = appendRecord(
        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
        kLegacyDrawIndexedPrimitiveSizeHint +
            dxmt9::d3d9::pe::sparseStatePlanConstantPayloadBytes(sparsePlan),
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            if (!dxmt9::d3d9::pe::finalizeSparseStatePlanChunkContext(
                    currentChunkContext(), sparsePlan)) {
                return D3DERR_INVALIDCALL;
            }
            indexSectionEmitted = sparsePlan.indexBuffer;
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendSparseStatePlan(
                builder, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
                sparsePlan);
            const auto settlement =
                dxmt9::d3d9::pe::settleRecorderAppend({
                    .phase =
                        dxmt9::d3d9::pe::AppendSettlement::Prepared,
                    .appendSucceeded = ok,
                });
            const bool settled =
                dxmt9::d3d9::pe::acceptSparseStatePlan(
                    recorderState_.peState, recorderState_.peConsts,
                    sparsePlan, settlement,
                    scalarSemanticObserver(),
                    builder.activeRecordOrdinal());
            phase.recordEncode(t0);
            if (ok && !settled) {
                poisonStateBlockTransaction();
                return D3DERR_DEVICELOST;
            }
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (SUCCEEDED(hr)) {
        if (indexSectionEmitted) {
            submittedIndexBufferWireValue_ = ibWireValue;
            submittedIndexBufferKnown_ = true;
        }
    }
    return hr;
}

bool D3D9DeviceImpl::pendingChunkReferencesBuffer(
    const dxmt9::d3d9::pe::BufferRef &buffer) const {
    if (!buffer.object) {
        return false;
    }
    return recorderState_.commandChunk.referencesObject(
        dxmt9::d3d9::pe::localIdentity(buffer));
}

HRESULT D3D9DeviceImpl::appendDrawPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                    UINT count,
                                    const void* data,
                                    UINT stride) {
    return appendDrawPrimitiveUPRecordWithFvf(type, count, data, stride,
                                              false, 0);
}

HRESULT D3D9DeviceImpl::appendDrawPrimitiveUPRecordWithFvf(D3DPRIMITIVETYPE type,
                                           UINT count,
                                           const void* data,
                                           UINT stride,
                                           bool overrideFvf,
                                           DWORD packetFvf,
                                           bool overrideVertexShaderNull,
                                           bool forceFullSnapshot) {
    Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Draw);
    // See appendDrawPrimitiveRecord: recursive re-entry on an
    // already-held recorderState_.recorderMutex is cheaper than the repeated cold
    // acquisitions the nested const-flush + draw appends would do.
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const HRESULT constHr = flushPendingConsts();
    if (FAILED(constHr)) return constHr;
    // Payload sizing moved AHEAD of the state build, because the producer
    // reads the payload spans while building. It is pure input validation --
    // type/count/stride/data only, nothing the state build produces -- and
    // consumes no pending state, so failing here instead of after the build
    // returns the same D3DERR_INVALIDCALL from the same inputs.
    std::uint32_t vertexBytes = 0;
    if (!checkedByteCount(primitiveVertexCount(type, count), stride, vertexBytes) ||
        (vertexBytes != 0 && !data)) {
        return D3DERR_INVALIDCALL;
    }
    IDirect3DVertexDeclaration9* overrideDecl = nullptr;
    if (overrideFvf) {
        const HRESULT fvfHr =
            resolveImplicitDeclForFvf(packetFvf, &overrideDecl);
        if (FAILED(fvfHr)) return fvfHr;
    }
    const DWORD savedFvf = fvf_;
    IDirect3DVertexDeclaration9* savedVdecl = vdecl_;
    IDirect3DVertexShader9* savedVs = vs_;
    const auto savedBindingFvf = recorderState_.peBindingView.fvf;
    const auto savedBindingVdecl = recorderState_.peBindingView.vdecl;
    const auto savedBindingVs = recorderState_.peBindingView.vs;
    const bool savedPendingFvf = recorderState_.peState.pendingFvf();
    const bool savedPendingVdecl = recorderState_.peState.pendingVdecl();
    const bool savedPendingVs = recorderState_.peState.pendingVs();
    if (overrideFvf) {
        D3D9PeValidatedDeclaration validatedOverride{};
        const HRESULT validationHr = D3D9PeValidateVertexDecl(
            overrideDecl, static_cast<IDirect3DDevice9*>(this),
            &validatedOverride);
        if (FAILED(validationHr)) return validationHr;
        fvf_ = packetFvf;
        vdecl_ = overrideDecl;
        recorderState_.peBindingView.fvf = packetFvf;
        recorderState_.peBindingView.vdecl = validatedOverride.wire();
        recorderState_.peState.maintenance().pendingFvf() = true;
        recorderState_.peState.maintenance().pendingVdecl() = true;
    }
    if (overrideVertexShaderNull) {
        vs_ = nullptr;
        recorderState_.peBindingView.vs = {};
        recorderState_.peState.maintenance().pendingVs() = true;
    }
    // The override window has to cover populateBindingView, which reads
    // fvf_ / vdecl_ / vs_, so it wraps the whole state build exactly as it
    // wrapped the fat-packet build before.
    dxmt9::d3d9::pe::PeDrawParams params{};
    params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
    params.primitiveType = static_cast<std::uint32_t>(type);
    params.primitiveCount = count;
    params.stride = stride;
    const dxmt9::d3d9::pe::PeDrawPayloads payloads{
        .upVertex = std::span<const std::byte>(
            static_cast<const std::byte*>(data), vertexBytes),
    };
    const bool compatibilityOverride =
        overrideFvf || overrideVertexShaderNull;
    dxmt9::d3d9::pe::SparseStatePlan sparsePlan{};
    if (compatibilityOverride) {
        // The SWVP override window is restored before append, so its borrowed
        // binding values need the value-owned compatibility projection.
        recorderState_.peSparsePayloads = payloads;
    }
    const bool built = compatibilityOverride
        ? buildSparseStateForRecord(params, forceFullSnapshot)
        : buildSparseStatePlanForRecord(
              params, payloads, sparsePlan, forceFullSnapshot);
    if (overrideFvf) {
        fvf_ = savedFvf;
        vdecl_ = savedVdecl;
        recorderState_.peBindingView.fvf = savedBindingFvf;
        recorderState_.peBindingView.vdecl = savedBindingVdecl;
        recorderState_.peState.maintenance().pendingFvf() = savedPendingFvf;
        recorderState_.peState.maintenance().pendingVdecl() = savedPendingVdecl;
    }
    if (overrideVertexShaderNull) {
        vs_ = savedVs;
        recorderState_.peBindingView.vs = savedBindingVs;
        recorderState_.peState.maintenance().pendingVs() = savedPendingVs;
    }
    if (!built) {
        if (compatibilityOverride) {
            recorderState_.peSparsePayloads =
                dxmt9::d3d9::pe::PeDrawPayloads{};
        }
        return D3DERR_INVALIDCALL;
    }

    // sizeHint stays the legacy header+payload size the capacity precheck
    // saw before, so chunk seal cadence is unchanged. No chunk-context step:
    // a UP draw binds no app buffer, so there is nothing for the destination
    // chunk to have retained -- matching both legacy UP call sites, which ran
    // neither dependency checkpoint.
    const HRESULT hr = appendRecord(
        D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
        kLegacyDrawPrimitiveUPSizeHint + vertexBytes,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = compatibilityOverride
                ? dxmt9::d3d9::pe::appendSparseRecord(
                      builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                      recorderState_.peSparseHeader,
                      recorderState_.peSparseState)
                : dxmt9::d3d9::pe::appendSparseStatePlan(
                      builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                      sparsePlan);
            if (!compatibilityOverride) {
                const auto settlement =
                    dxmt9::d3d9::pe::settleRecorderAppend({
                        .phase =
                            dxmt9::d3d9::pe::AppendSettlement::Prepared,
                        .appendSucceeded = ok,
                    });
                const bool settled =
                    dxmt9::d3d9::pe::acceptSparseStatePlan(
                        recorderState_.peState, recorderState_.peConsts,
                        sparsePlan, settlement,
                        scalarSemanticObserver(),
                        builder.activeRecordOrdinal());
                if (ok && !settled) {
                    poisonStateBlockTransaction();
                    phase.recordEncode(t0);
                    return D3DERR_DEVICELOST;
                }
            }
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (compatibilityOverride) {
        recorderState_.peSparsePayloads =
            dxmt9::d3d9::pe::PeDrawPayloads{};
    }
    if (SUCCEEDED(hr)) {
        recordDrawPrimitiveUPCopy(vertexBytes);
    }
    return hr;
}

HRESULT D3D9DeviceImpl::appendDrawIndexedPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                           UINT minVertex,
                                           UINT numVertices,
                                           UINT count,
                                           const void* indexData,
                                           D3DFORMAT indexFormat,
                                           const void* vertexData,
                                           UINT stride) {
    return appendDrawIndexedPrimitiveUPRecordWithFvf(
        type, minVertex, numVertices, count, indexData, indexFormat,
        vertexData, stride, false, 0);
}

HRESULT D3D9DeviceImpl::appendDrawIndexedPrimitiveUPRecordWithFvf(D3DPRIMITIVETYPE type,
                                                  UINT minVertex,
                                                  UINT numVertices,
                                                  UINT count,
                                                  const void* indexData,
                                                  D3DFORMAT indexFormat,
                                                  const void* vertexData,
                                                  UINT stride,
                                                  bool overrideFvf,
                                                  DWORD packetFvf,
                                                  bool overrideVertexShaderNull,
                                                  bool forceFullSnapshot) {
    Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Draw);
    // See appendDrawPrimitiveRecord: recursive re-entry on an
    // already-held recorderState_.recorderMutex is cheaper than the repeated cold
    // acquisitions the nested const-flush + draw appends would do.
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const HRESULT constHr = flushPendingConsts();
    if (FAILED(constHr)) return constHr;
    // Payload sizing moved AHEAD of the state build; see the non-indexed UP
    // site for why that is behaviour-preserving.
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
    IDirect3DVertexDeclaration9* overrideDecl = nullptr;
    if (overrideFvf) {
        const HRESULT fvfHr =
            resolveImplicitDeclForFvf(packetFvf, &overrideDecl);
        if (FAILED(fvfHr)) return fvfHr;
    }
    const DWORD savedFvf = fvf_;
    IDirect3DVertexDeclaration9* savedVdecl = vdecl_;
    IDirect3DVertexShader9* savedVs = vs_;
    const auto savedBindingFvf = recorderState_.peBindingView.fvf;
    const auto savedBindingVdecl = recorderState_.peBindingView.vdecl;
    const auto savedBindingVs = recorderState_.peBindingView.vs;
    const bool savedPendingFvf = recorderState_.peState.pendingFvf();
    const bool savedPendingVdecl = recorderState_.peState.pendingVdecl();
    const bool savedPendingVs = recorderState_.peState.pendingVs();
    if (overrideFvf) {
        D3D9PeValidatedDeclaration validatedOverride{};
        const HRESULT validationHr = D3D9PeValidateVertexDecl(
            overrideDecl, static_cast<IDirect3DDevice9*>(this),
            &validatedOverride);
        if (FAILED(validationHr)) return validationHr;
        fvf_ = packetFvf;
        vdecl_ = overrideDecl;
        recorderState_.peBindingView.fvf = packetFvf;
        recorderState_.peBindingView.vdecl = validatedOverride.wire();
        recorderState_.peState.maintenance().pendingFvf() = true;
        recorderState_.peState.maintenance().pendingVdecl() = true;
    }
    if (overrideVertexShaderNull) {
        vs_ = nullptr;
        recorderState_.peBindingView.vs = {};
        recorderState_.peState.maintenance().pendingVs() = true;
    }
    dxmt9::d3d9::pe::PeDrawParams params{};
    params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
    params.primitiveType = static_cast<std::uint32_t>(type);
    params.minVertex = minVertex;
    params.numVertices = numVertices;
    params.primitiveCount = count;
    params.stride = stride;
    params.indexFormat = static_cast<std::uint32_t>(indexFormat);
    const dxmt9::d3d9::pe::PeDrawPayloads payloads{
        .upIndex = std::span<const std::byte>(
            static_cast<const std::byte*>(indexData), indexBytes),
        .upVertex = std::span<const std::byte>(
            static_cast<const std::byte*>(vertexData), vertexBytes),
    };
    const bool compatibilityOverride =
        overrideFvf || overrideVertexShaderNull;
    dxmt9::d3d9::pe::SparseStatePlan sparsePlan{};
    if (compatibilityOverride) {
        recorderState_.peSparsePayloads = payloads;
    }
    const bool built = compatibilityOverride
        ? buildSparseStateForRecord(params, forceFullSnapshot)
        : buildSparseStatePlanForRecord(
              params, payloads, sparsePlan, forceFullSnapshot);
    if (overrideFvf) {
        fvf_ = savedFvf;
        vdecl_ = savedVdecl;
        recorderState_.peBindingView.fvf = savedBindingFvf;
        recorderState_.peBindingView.vdecl = savedBindingVdecl;
        recorderState_.peState.maintenance().pendingFvf() = savedPendingFvf;
        recorderState_.peState.maintenance().pendingVdecl() = savedPendingVdecl;
    }
    if (overrideVertexShaderNull) {
        vs_ = savedVs;
        recorderState_.peBindingView.vs = savedBindingVs;
        recorderState_.peState.maintenance().pendingVs() = savedPendingVs;
    }
    if (!built) {
        if (compatibilityOverride) {
            recorderState_.peSparsePayloads =
                dxmt9::d3d9::pe::PeDrawPayloads{};
        }
        return D3DERR_INVALIDCALL;
    }

    // No chunk-context step and, critically, no index-buffer section: an
    // indexed UP draw carries its indices inline and binds no index buffer.
    // dxmt9_pe_producer.cpp's indexedDraw predicate excludes _UP for exactly
    // this reason.
    const HRESULT hr = appendRecord(
        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
        kLegacyDrawIndexedPrimitiveUPSizeHint + indexBytes +
            vertexBytes,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = compatibilityOverride
                ? dxmt9::d3d9::pe::appendSparseRecord(
                      builder,
                      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                      recorderState_.peSparseHeader,
                      recorderState_.peSparseState)
                : dxmt9::d3d9::pe::appendSparseStatePlan(
                      builder,
                      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                      sparsePlan);
            if (!compatibilityOverride) {
                const auto settlement =
                    dxmt9::d3d9::pe::settleRecorderAppend({
                        .phase =
                            dxmt9::d3d9::pe::AppendSettlement::Prepared,
                        .appendSucceeded = ok,
                    });
                const bool settled =
                    dxmt9::d3d9::pe::acceptSparseStatePlan(
                        recorderState_.peState, recorderState_.peConsts,
                        sparsePlan, settlement,
                        scalarSemanticObserver(),
                        builder.activeRecordOrdinal());
                if (ok && !settled) {
                    poisonStateBlockTransaction();
                    phase.recordEncode(t0);
                    return D3DERR_DEVICELOST;
                }
            }
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (compatibilityOverride) {
        recorderState_.peSparsePayloads =
            dxmt9::d3d9::pe::PeDrawPayloads{};
    }
    if (SUCCEEDED(hr)) {
        recordDrawIndexedPrimitiveUPCopy(vertexBytes, indexBytes);
    }
    return hr;
}

VsConstRangeChange D3D9DeviceImpl::analyzeConstShadowChange(
    const ConstShadow& shadow,
    std::uint32_t start,
    std::uint32_t count,
    const void* data,
    std::size_t elemSize) {
    VsConstRangeChange change{};
    if (count == 0u || !data) {
        return change;
    }
    const auto* src = static_cast<const std::uint8_t*>(data);
    std::uint32_t firstChanged = count;
    std::uint32_t lastChanged = 0u;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* elem = src + static_cast<std::size_t>(i) * elemSize;
        if (constShadowElemEquals(shadow, start + i, elem, elemSize)) {
            continue;
        }
        ++change.changedRegs;
        firstChanged = std::min<std::uint32_t>(firstChanged, i);
        lastChanged = i + 1u;
    }
    if (change.changedRegs != 0u) {
        change.changedSpanRegs = lastChanged - firstChanged;
    }
    return change;
}

std::uint32_t D3D9DeviceImpl::countDirtyConstRegs(const ConstShadow& shadow,
                                         std::uint32_t start,
                                         std::uint32_t end) {
    std::uint32_t count = 0u;
    const std::uint32_t dirtyEnd = std::min<std::uint32_t>(
        end, static_cast<std::uint32_t>(shadow.dirtyElems.size()));
    for (std::uint32_t reg = start; reg < dirtyEnd; ++reg) {
        count += shadow.dirtyElems[reg] != 0u ? 1u : 0u;
    }
    return count;
}

void D3D9DeviceImpl::LockStateBlockOperationForChild() noexcept {
    if (recorderState_.recorderLockRequired) recorderState_.recorderMutex.lock();
}

void D3D9DeviceImpl::UnlockStateBlockOperationForChild() noexcept {
    if (recorderState_.recorderLockRequired) recorderState_.recorderMutex.unlock();
}

bool D3D9DeviceImpl::IsStateBlockRecorderPoisonedForChild() const noexcept {
    return recorderState_.stateBlockTransaction.isPoisoned();
}

void D3D9DeviceImpl::DiscardPreparedStateBlockApplyForChild() noexcept {
    discardPreparedStateBlockApply();
}

void D3D9DeviceImpl::PoisonStateBlockRecorderForChild() noexcept {
    poisonStateBlockTransaction();
}

HRESULT D3D9DeviceImpl::FlushPeRecorderForBufferHazardForChild(D9CBuffer *buffer) noexcept {
    if (!buffer) {
        return S_OK;
    }
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const bool referenced = recorderState_.commandChunk.referencesObject(
        dxmt9::d3d9::pe::PeLocalObjectIdentity{
            .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .object = buffer});
    if (!referenced) {
        return S_OK;
    }
    return flushPeRecorder(PeRecorderFlushReason::Child);
}

bool D3D9DeviceImpl::IsStateBlockRecordingForChild() const noexcept {
    return recorderState_.stateBlockTransaction.isRecording();
}

void D3D9DeviceImpl::InvalidateStateBlockShadowForChild() noexcept {
    recorderState_.peState.maintenance().clearServerShadowTables();
    clearPendingHotState();
}

void D3D9DeviceImpl::AddDefaultPoolResourceRefForChild() noexcept {
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    ++defaultPoolResourceRefs_;
}

void D3D9DeviceImpl::ReleaseDefaultPoolResourceRefForChild() noexcept {
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (defaultPoolResourceRefs_ != 0) {
        --defaultPoolResourceRefs_;
    }
}

bool D3D9DeviceImpl::IsChunkRecorderEnabledForChild() const noexcept {
    return true;
}

HRESULT D3D9DeviceImpl::AppendQueryIssueForChild(
    std::uint32_t flags,
    const dxmt9::d3d9::pe::QueryRef& query) noexcept {
    return appendRecord(
        D9C_COMMAND_RECORD_QUERY_ISSUE,
        kLegacyQueryIssueSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendQueryIssue(
                builder, flags, query);
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
}

bool D3D9DeviceImpl::commandChunkReady() const noexcept {
    return recorderState_.commandChunkNegotiated;
}

D3D9PeStateBlockContext *D3D9DeviceImpl::stateBlockContext() noexcept {
    return &stateBlockContext_;
}

D3D9PeBufferContext *D3D9DeviceImpl::bufferContext() noexcept { return &bufferContext_; }

D3D9PeSurfaceTextureContext *D3D9DeviceImpl::surfaceTextureContext() noexcept {
    return &surfaceTextureContext_;
}

D3D9PeQueryContext *D3D9DeviceImpl::queryContext() noexcept { return &queryContext_; }

D3D9PePresentationContext *D3D9DeviceImpl::presentationContext() noexcept {
    return &presentationContext_;
}

D3D9PeShaderDeclarationContext *D3D9DeviceImpl::shaderDeclarationContext() noexcept {
    return &shaderDeclarationContext_;
}

D3D9PeDiagnosticObserver* D3D9DeviceImpl::diagnosticObserverForChild() noexcept {
    if (!diagnostics_ || !diagnostics_->config.recorderStats) {
        return nullptr;
    }
    return &diagnostics_->childObserver;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetRenderTarget(DWORD idx,
                                           IDirect3DSurface9* pSurf) noexcept {
    return withPeCallAndHotStateSetter(
        "SetRenderTarget", DXMT9_PE_CALLSITE_PC(), nullptr,
        PeHotStateSetterFamily::RenderTarget, nullptr,
        [&](auto& peCall, auto& hotSetter)
            __attribute__((always_inline)) noexcept -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("SetRenderTarget", hr);
    };
    dxmt9DeviceDebugLog("device_set_render_target device=%p idx=%u surf=%p",
                        this, (unsigned)idx, pSurf);
    if (idx >= 4) return finishPeCall(D3DERR_INVALIDCALL);
    D3D9PeValidatedSurface validatedSurface{};
    const HRESULT membershipHr = D3D9PeValidateSurface(
        pSurf, static_cast<IDirect3DDevice9*>(this), &validatedSurface);
    if (FAILED(membershipHr)) return finishPeCall(membershipHr);
    D3DSURFACE_DESC primaryDesc{};
    if (idx == 0 && pSurf) {
        const HRESULT descHr = pSurf->GetDesc(&primaryDesc);
        if (FAILED(descHr)) return finishPeCall(descHr);
    }
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                setRecordedRef(
                    writer.renderTargets(),
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::renderTargets>(idx),
                    validatedSurface);
            });
        hotSetter.markDirty();
        return finishPeCall(S_OK);
    }
    if (idx == 0) {
        setRef(cachedBackBuffer0_, (IDirect3DSurface9*)nullptr);
    }
    const bool wasExplicit = rtSlotExplicit_[idx];
    const bool valueChanged = rtSlots_[idx] != pSurf;
    if (valueChanged || !wasExplicit) {
        recorderState_.peState.transition().bindRenderTarget(idx, [&]() noexcept {
            rtSlotExplicit_[idx] = true;
            if (valueChanged) setRef(rtSlots_[idx], pSurf);
            recorderState_.peBindingView.renderTargets[idx] =
                validatedSurface.wire();
            recorderState_.peBindingView.rtExplicitMask =
                currentRtExplicitMask();
        });
        hotSetter.markDirty();
    } else {
        rtSlotExplicit_[idx] = true;
    }
    if (idx == 0 && pSurf) {
        const uint32_t w = std::max<uint32_t>(1u, primaryDesc.Width);
        const uint32_t h = std::max<uint32_t>(1u, primaryDesc.Height);
        recorderState_.peState.transition().setViewport(
            D9CViewport{0, 0, w, h, 0.0f, 1.0f});
        recorderState_.peState.transition().setScissor(
            D9CRect{0, 0, static_cast<int32_t>(w),
                    static_cast<int32_t>(h)});
        hotSetter.markDirty();
    }
    return finishPeCall(S_OK);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetDepthStencilSurface(IDirect3DSurface9* pSurf) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::DepthStencil,
        "SetDepthStencilSurface", nullptr, nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    dxmt9DeviceDebugLog("device_set_depth_stencil device=%p surf=%p", this, pSurf);
    D3D9PeValidatedSurface validatedSurface{};
    const HRESULT membershipHr = D3D9PeValidateSurface(
        pSurf, static_cast<IDirect3DDevice9*>(this), &validatedSurface);
    if (FAILED(membershipHr)) return membershipHr;
    if (pSurf) {
        // visual_multisample_rt_ds_mismatch_policy: the DS multisample
        // type must match the bound RT[0] multisample type. Query both
        // descs (via the C ABI helpers) and reject the mismatch.
        if (rtSlots_[0]) {
            D9CSurface* dsRaw = validatedSurface.raw();
            D9CSurface* rtRaw = validatedRawSurface(rtSlots_[0]);
            if (dsRaw && rtRaw) {
                D9CSurfaceDesc dsDesc{};
                D9CSurfaceDesc rtDesc{};
                if (SUCCEEDED(hr32(dxmt9c_surface_get_desc(dsRaw, &dsDesc)))
                        && SUCCEEDED(hr32(dxmt9c_surface_get_desc(rtRaw, &rtDesc)))
                        && dsDesc.multiSampleType != rtDesc.multiSampleType) {
                    return D3DERR_INVALIDCALL;
                }
            }
        }
    }
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                setRecordedRef(
                    writer.depthStencil(),
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::depthStencil>(0u),
                    validatedSurface);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    const bool wasExplicit = dsSurfaceExplicit_;
    const bool valueChanged = dsSurface_ != pSurf;
    if (valueChanged || !wasExplicit) {
        recorderState_.peState.transition().bindDepthStencil([&]() noexcept {
            dsSurfaceExplicit_ = true;
            if (valueChanged) setRef(dsSurface_, pSurf);
            recorderState_.peBindingView.depthStencil =
                validatedSurface.wire();
        });
        hotSetter.markDirty();
    } else {
        dsSurfaceExplicit_ = true;
    }
    return S_OK;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetTransform(D3DTRANSFORMSTATETYPE state,
                                        const D3DMATRIX* pM) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::Transform, "SetTransform", nullptr,
        nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    if (!pM) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog(
        "device_set_transform device=%p state=%u "
        "m=[[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g]]",
        this, (unsigned)state,
        pM->m[0][0], pM->m[0][1], pM->m[0][2], pM->m[0][3],
        pM->m[1][0], pM->m[1][1], pM->m[1][2], pM->m[1][3],
        pM->m[2][0], pM->m[2][1], pM->m[2][2], pM->m[2][3],
        pM->m[3][0], pM->m[3][1], pM->m[3][2], pM->m[3][3]);
    const D9CMatrix& wireM = *reinterpret_cast<const D9CMatrix*>(pM);
    return setTransformNormalized(
        state, wireM, dxmt9::d3d9::pe::WriteOrigin::ExplicitSet,
        hotSetter);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                             const D3DMATRIX* pM) noexcept {
    notePeDeviceCallAfterPresent("MultiplyTransform");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    if (!pM) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_multiply_transform device=%p state=%u", this, (unsigned)state);
    D3DMATRIX cur{};
    // GetTransform reads the primary PE shadow. Explicit SetTransform
    // calls made while recording update only the recording value, while
    // MultiplyTransform remains a prior-value operation on the primary
    // live value (matching Wine's stateblock behavior).
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
    const D9CMatrix& wireResult =
        *reinterpret_cast<const D9CMatrix*>(&result);
    return setTransformNormalized(
        state, wireResult,
        dxmt9::d3d9::pe::WriteOrigin::PriorValueOperation,
        peNullHotSetter_);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetViewport(const D3DVIEWPORT9* pVP) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::ViewportScissor, "SetViewport", nullptr,
        DXMT9_PE_CALLSITE_PC(),
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    if (!pVP) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_set_viewport device=%p x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                        this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
    D9CViewport vp{ pVP->X, pVP->Y, pVP->Width, pVP->Height,
                    pVP->MinZ, pVP->MaxZ };
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.viewport().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::viewport>(0u), vp);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    // Phase 12: PE-shadow-only when chunk recorder is active. The
    // packet built for the next draw carries viewportValid=1 + the
    // shadow snapshot; server-side canonical state replay dispatches
    // dxmt9c_device_set_viewport before the draw runs.
    const auto viewportPlan = dxmt9::d3d9::pe::planRecorderStateWrite({
        .phase = dxmt9::d3d9::pe::RecorderPhase::Live,
        .origin = dxmt9::d3d9::pe::WriteOrigin::ExplicitSet,
        .liveContains = true,
        .liveEquals = std::memcmp(
            &recorderState_.peState.viewportShadow(), &vp, sizeof(vp)) == 0,
        .pendingContains = recorderState_.peState.pendingViewport(),
    });
    if (!viewportPlan.valid()) return D3DERR_INVALIDCALL;
    if (!viewportPlan.writeLive()) {
        return S_OK;
    }
    recorderState_.peState.transition().setViewport(vp);
    hotSetter.markDirty();
    return S_OK;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetScissorRect(const RECT* pR) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::ViewportScissor, "SetScissorRect",
        nullptr, DXMT9_PE_CALLSITE_PC(),
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    if (!pR) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_set_scissor_rect device=%p rect=%ld,%ld-%ld,%ld",
                        this, (long)pR->left, (long)pR->top, (long)pR->right, (long)pR->bottom);
    D9CRect cr = toR(*pR);
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.scissor().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::scissor>(0u), cr);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    // Phase 12: PE-shadow-only when chunk recorder is active.
    const auto scissorPlan = dxmt9::d3d9::pe::planRecorderStateWrite({
        .phase = dxmt9::d3d9::pe::RecorderPhase::Live,
        .origin = dxmt9::d3d9::pe::WriteOrigin::ExplicitSet,
        .liveContains = true,
        .liveEquals = std::memcmp(
            &recorderState_.peState.scissorShadow(), &cr, sizeof(cr)) == 0,
        .pendingContains = recorderState_.peState.pendingScissor(),
    });
    if (!scissorPlan.valid()) return D3DERR_INVALIDCALL;
    if (!scissorPlan.writeLive()) {
        return S_OK;
    }
    recorderState_.peState.transition().setScissor(cr);
    hotSetter.markDirty();
    return S_OK;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetMaterial(const D3DMATERIAL9* pM) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::MaterialLightClip, "SetMaterial", nullptr,
        nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    if (!pM) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_set_material device=%p", this);
    if (recorderState_.stateBlockTransaction.isRecording()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.material().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::material>(0u),
                    *reinterpret_cast<const D9CMaterial*>(pM));
            });
        hotSetter.markDirty();
        return S_OK;
    }
    const auto materialPlan = dxmt9::d3d9::pe::planRecorderStateWrite({
        .phase = dxmt9::d3d9::pe::RecorderPhase::Live,
        .origin = dxmt9::d3d9::pe::WriteOrigin::ExplicitSet,
        .liveContains = true,
        .liveEquals = std::memcmp(
            &recorderState_.peState.materialShadow(), pM,
            sizeof(D9CMaterial)) == 0,
        .pendingContains = recorderState_.peState.pendingMaterial(),
    });
    if (!materialPlan.valid()) return D3DERR_INVALIDCALL;
    if (!materialPlan.writeLive()) {
        return S_OK;
    }
    recorderState_.peState.transition().setMaterial(
        *reinterpret_cast<const D9CMaterial*>(pM));
    hotSetter.markDirty();
    return S_OK;
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetLight(DWORD idx, const D3DLIGHT9* pL) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::MaterialLightClip, "SetLight", nullptr,
        nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
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
    if (recorderState_.stateBlockTransaction.isRecording()) {
        if (idx >= D9C_DRAW_PACKET_MAX_LIGHTS) return D3DERR_INVALIDCALL;
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.lights().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::lights>(idx), cl);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    // Phase 12: PE-shadow-only when chunk recorder is active. Up to
    // D9C_DRAW_PACKET_MAX_LIGHTS (8) light slots ride on a single
    // packet via lightSlotMask + lights[8]. Out-of-range idx falls
    // back to legacy unix-call (rare, and the backend may also
    // refuse).
    if (idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
        if ((recorderState_.peState.pendingLightSlotMask() & (1u << idx)) == 0 &&
            std::memcmp(&recorderState_.peState.lightShadow()[idx], &cl, sizeof(D9CLight)) == 0) {
            return S_OK;
        }
        recorderState_.peState.transition().setLight(idx, cl);
        hotSetter.markDirty();
        return S_OK;
    }
    const HRESULT flushHr = flushPeRecorder();
    if (FAILED(flushHr)) return flushHr;
    hotSetter.markDirty();
    return hr32(dxmt9c_device_set_light(dev_, idx, &cl));
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::LightEnable(DWORD idx, BOOL en) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::MaterialLightClip, "LightEnable", nullptr,
        nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    dxmt9DeviceDebugLog("device_light_enable device=%p idx=%u enable=%u", this, (unsigned)idx, (unsigned)en);
    if (recorderState_.stateBlockTransaction.isRecording()) {
        if (idx >= D9C_DRAW_PACKET_MAX_LIGHTS) return D3DERR_INVALIDCALL;
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.lightEnables().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::lightEnables>(idx),
                    en ? 1u : 0u);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    // Phase 12: PE-shadow-only when chunk recorder is active.
    if (idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
        const DWORD bit = 1u << idx;
        const bool wantEnabled = en != 0;
        const bool shadowEnabled = (recorderState_.peState.lightEnableShadow() & bit) != 0;
        if ((recorderState_.peState.pendingLightEnableValidMask() & bit) == 0 &&
            wantEnabled == shadowEnabled) {
            return S_OK;
        }
        recorderState_.peState.transition().setLightEnable(
            idx, wantEnabled);
        hotSetter.markDirty();
        return S_OK;
    }
    const HRESULT flushHr = flushPeRecorder();
    if (FAILED(flushHr)) return flushHr;
    hotSetter.markDirty();
    return hr32(dxmt9c_device_light_enable(dev_, idx, en ? 1u : 0u));
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetClipPlane(DWORD idx, const float* pPlane) noexcept {
    return withPeHotStateSetter(
        PeHotStateSetterFamily::MaterialLightClip, "SetClipPlane", nullptr,
        nullptr,
        [&](auto& hotSetter) __attribute__((always_inline)) noexcept
            -> HRESULT {
    dxmt9DeviceDebugLog("device_set_clip_plane device=%p idx=%u plane=%p", this, (unsigned)idx, pPlane);
    if (!pPlane) return D3DERR_INVALIDCALL;
    if (idx >= 6) return D3DERR_INVALIDCALL;
    if (recorderState_.stateBlockTransaction.isRecording()) {
        std::array<float, 4> plane{};
        std::memcpy(plane.data(), pPlane, sizeof(plane));
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept {
                writer.clipPlanes().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::clipPlanes>(idx), plane);
            });
        hotSetter.markDirty();
        return S_OK;
    }
    const std::size_t off = static_cast<std::size_t>(idx) * 4u;
    if ((recorderState_.peState.pendingClipPlaneMask() & (1u << idx)) == 0 &&
        std::memcmp(&recorderState_.peState.clipPlaneShadow()[off], pPlane, sizeof(float) * 4) == 0) {
        return S_OK;
    }
    recorderState_.peState.transition().setClipPlane(idx, pPlane);
    hotSetter.markDirty();
    return S_OK;
        });
}

HRESULT D3D9DeviceImpl::requestReszDepthResolve() noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    // MSAA depth source = the currently bound depth-stencil surface.
    D9CSurface* const depthSrcRaw = validatedRawSurface(dsSurface_);
    // INTZ depth destination = the texture bound at fragment stage 0.
    D9CTexture* const intzDstRaw = validatedRawTexture(textures_[0]);
    dxmt9DeviceDebugLog(
        "device_resz_depth_resolve device=%p depth_src=%p intz_dst=%p",
        this, static_cast<void*>(depthSrcRaw),
        static_cast<void*>(intzDstRaw));
    // No bound source or destination: nothing to resolve. RESZ is a
    // fire-and-forget idiom, so a missing binding is a benign no-op
    // (matching real-hardware behavior). Don't emit an empty record.
    if (!depthSrcRaw || !intzDstRaw) {
        return S_OK;
    }
    // Surface-op emit pattern (mirrors StretchRect / ColorFill): drain any
    // pending hot state to a barrier, then append the standalone record
    // with both endpoints retained for the chunk's lifetime. INTZ dest is
    // a texture handle, MSAA depth source a surface handle — canonicalized
    // the same way the neighboring surface ops resolve their endpoints
    // (rawSurf / rawTex → SERVER-SIDE wire cast), so the importer decodes
    // them via the same wireValuePtr path.
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    const HRESULT appendHr = appendRecord(
        D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
        kLegacyReszDepthResolveSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendReszDepthResolve(
                builder, recorderState_.peBindingView.depthStencil,
                recorderState_.peBindingView.textures[0]);
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (FAILED(appendHr)) return appendHr;
    return S_OK;
}



template<typename Body>
__attribute__((always_inline))
HRESULT D3D9DeviceImpl::withPeHotStateSetter(
    PeHotStateSetterFamily family, const char* callName,
    PeDecimatedScopeStats PeDiagnosticsState::* entryStats,
    const void* callerPc, Body&& body) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!recorderState_.stateBlockTransaction.writeAllowed()) {
        return D3DERR_DEVICELOST;
    }
    PeDiagnosticsState* const diagnostics = diagnostics_.get();
    if (!diagnostics) {
        return std::forward<Body>(body)(peNullHotSetter_);
    }
    if (!diagnostics->gates.hotSetterTimer) {
        return std::forward<Body>(body)(peNullHotSetter_);
    }
    PeHotStateSetterTimer hotSetter(
        *this, *diagnostics, family, callName, entryStats, callerPc);
    return std::forward<Body>(body)(hotSetter);
}


template<typename HotSetter>
HRESULT D3D9DeviceImpl::setTransformNormalized(D3DTRANSFORMSTATETYPE state,
                               const D9CMatrix& wireM,
                               dxmt9::d3d9::pe::WriteOrigin origin,
                               HotSetter& hotSetter) noexcept {
    using namespace dxmt9::d3d9::pe;
    const std::uint32_t stateKey = static_cast<std::uint32_t>(state);
    std::uint32_t transformSlotIndex = 0u;
    if (!FixedTransformTable::slotForState(stateKey, transformSlotIndex)) {
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_device_set_transform(dev_, stateKey,
                                                             &wireM));
        if (SUCCEEDED(hr)) hotSetter.markDirty();
        return hr;
    }

    const TransformState key = transformStateKey(stateKey);
    D9CMatrix liveValue{};
    const bool liveContains =
        recorderState_.peState.transformShadowTyped().get(key, liveValue);
    const bool liveEquals = liveContains && matrixEquals(liveValue, wireM);
    const StateWritePlan plan = planRecorderStateWrite(StateWriteFacts{
        .phase = recorderState_.stateBlockTransaction.isRecording() ? RecorderPhase::Recording
                                      : RecorderPhase::Live,
        .origin = origin,
        .liveContains = liveContains,
        .liveEquals = liveEquals,
        .pendingContains = recorderState_.peState.pendingTransformsTyped().contains(key),
    });

    if (plan.kind() == StateWriteKind::NoOp ||
        plan.kind() == StateWriteKind::RetainPending) {
        return S_OK;
    }
    if (plan.writePending() &&
        !recorderState_.peState.pendingTransformsTyped().contains(key) &&
        recorderState_.peState.pendingTransformsTyped().size() >=
            D9C_DRAW_PACKET_MAX_TRANSFORMS) {
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
    }

    // BeginStateBlock flushes all pending recorder state before entering
    // Recording. A direct prior-value operation is therefore ordered only
    // when no older transform delta survives. Fail closed if that
    // lifecycle premise is ever violated instead of allowing a later
    // pending replay to overwrite the direct result.
    if (plan.directOrderedCall() &&
        recorderState_.peState.pendingTransformsTyped().contains(key)) {
        return D3DERR_INVALIDCALL;
    }
    if (plan.directOrderedCall()) {
        const HRESULT hr = hr32(
            dxmt9c_device_set_transform(dev_, stateKey, &wireM));
        if (FAILED(hr)) return hr;
    }
    if (plan.writeRecorded()) {
        recorderState_.stateBlockTransaction.withRecordingWriter(
            [&](auto& writer) noexcept { writer.transforms().set(key, wireM); });
    }
    if (plan.writeLive() && plan.writePending()) {
        recorderState_.peState.transition().setTransform(key, wireM);
    } else if (plan.writeLive()) {
        recorderState_.peState.maintenance().transformShadowTyped().set(key, wireM);
    } else if (plan.writePending()) {
        recorderState_.peState.maintenance().pendingTransformsTyped().set(key, wireM);
    }
    if (plan.semanticTransition() || plan.directOrderedCall()) {
        hotSetter.markDirty();
    }
    return S_OK;
}





template HRESULT D3D9DeviceImpl::SetVertexShaderConstantFSlowBody<
    D3D9DeviceImpl::PeCallScope>(
        UINT, const float*, UINT, D3D9DeviceImpl::PeCallScope&) noexcept;
template HRESULT D3D9DeviceImpl::SetVertexShaderConstantFSlowBody<
    const D3D9DeviceImpl::PeNullCallScope>(
        UINT, const float*, UINT,
        const D3D9DeviceImpl::PeNullCallScope&) noexcept;
template HRESULT D3D9DeviceImpl::SetPixelShaderConstantFSlowBody<
    D3D9DeviceImpl::PeCallScope>(
        UINT, const float*, UINT, D3D9DeviceImpl::PeCallScope&) noexcept;
template HRESULT D3D9DeviceImpl::SetPixelShaderConstantFSlowBody<
    const D3D9DeviceImpl::PeNullCallScope>(
        UINT, const float*, UINT,
        const D3D9DeviceImpl::PeNullCallScope&) noexcept;


#include <new>

/* =========================================================================
 * Key function -- QueryInterface is declaration-only in the class header so
 * this hot TU owns the device vtable. Keep its COM behavior unchanged.
 * ========================================================================= */

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::QueryInterface(
    REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
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
    *ppv = nullptr;
    return E_NOINTERFACE;
}

HRESULT D3D9DeviceImpl::FlushPeRecorderForChild() noexcept {
    return flushPeRecorder(PeRecorderFlushReason::Child);
}

void D3D9DeviceImpl::takeImplicitWsiBinding(
        D3D9PeWsiBinding& binding) noexcept {
    implicitWsiBinding_ = binding;
    binding = {};
}

/* =========================================================================
 * Factory function (called from factory.cpp)
 * ========================================================================= */

IDirect3DDevice9Ex* CreateDeviceImpl(D9CDevice* dev, IDirect3D9Ex* pFactory,
                                     UINT adapter, D3DDEVTYPE deviceType,
                                     DWORD behaviorFlags,
                                     HWND window, bool extended,
                                     DWORD implicitSwapchainFlags,
                                     D3D9PeWsiBinding wsiBinding,
                                     HRESULT* failureReason) noexcept {
    if (failureReason) *failureReason = D3DERR_NOTAVAILABLE;
    D3D9DeviceImpl* device = nullptr;
    try {
        device = new (std::nothrow) D3D9DeviceImpl(
            dev, pFactory, adapter, deviceType, behaviorFlags, window, extended,
            implicitSwapchainFlags);
    } catch (const std::bad_alloc&) {
        (void)dxmt9PeTeardownDeviceAndReleaseWsiBinding(dev, wsiBinding);
        if (dev) dxmt9c_device_release(dev);
        if (failureReason) *failureReason = E_OUTOFMEMORY;
        return nullptr;
    } catch (...) {
        (void)dxmt9PeTeardownDeviceAndReleaseWsiBinding(dev, wsiBinding);
        if (dev) dxmt9c_device_release(dev);
        if (failureReason) *failureReason = D3DERR_INVALIDCALL;
        return nullptr;
    }
    if (!device) {
        (void)dxmt9PeTeardownDeviceAndReleaseWsiBinding(dev, wsiBinding);
        if (dev) dxmt9c_device_release(dev);
        if (failureReason) *failureReason = E_OUTOFMEMORY;
        return nullptr;
    }
    device->takeImplicitWsiBinding(wsiBinding);
    if (!device->commandChunkReady()) {
        delete device;
        return nullptr;
    }
    return device;
}
