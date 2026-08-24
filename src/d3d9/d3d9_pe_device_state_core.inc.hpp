// Ordered in-class fragment: render-state, state-block, texture-stage, and sampler state.
    /* ── render states ── */
    template<typename HotSetter>
    HRESULT setRenderStateCore(D3DRENDERSTATETYPE state, DWORD value,
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
                [&](auto& writer) { writer.renderStates().set(renderKey, value); });
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

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD* pValue) noexcept override;

    /* ── state blocks ── */
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type,
                                                IDirect3DStateBlock9** ppSB) noexcept override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() noexcept override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) noexcept override;

    /* ── texture stage / sampler states ── */
    // Wine d3d9 texture-stage-state input validation. Tested by
    // test_texture_stage_states (line ~1535): the stage must be within
    // [0..MaxTextureBlendStages-1] and the type id must be a defined
    // D3DTSS_*. dxmt9 reports MaxTextureBlendStages=8.
    static constexpr DWORD kFragmentBlendStageCount = 8;
    static bool isValidTextureStageStateType(D3DTEXTURESTAGESTATETYPE type) noexcept {
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
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD value) noexcept override {
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
                [&](auto& writer) {
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
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD* pValue) noexcept override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD value) noexcept override {
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
                [&](auto& writer) {
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
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD* pValue) noexcept override;
