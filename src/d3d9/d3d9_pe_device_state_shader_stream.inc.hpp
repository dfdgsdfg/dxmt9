// Ordered in-class fragment: FVF, shader constants, streams, indices, and pixel shaders.
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) noexcept override;
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
            const D3DVERTEXELEMENT9* pElems,
            IDirect3DVertexDeclaration9** ppVD) noexcept override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(
            IDirect3DVertexDeclaration9* pVD) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(
            IDirect3DVertexDeclaration9** ppVD) noexcept override;

    /* ── vertex shaders ── */
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFn,
                                                  IDirect3DVertexShader9** ppVS) noexcept override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppVS) noexcept override;
    /* Constant register-file caps. The Wine constant-roundtrip tests use
     * D3DCAPS9::MaxVertexShaderConst as the F-register cap (256 for the
     * sm3-class caps we report); I/B are fixed at 16 across all real
     * D3D9 hardware. */
    static constexpr UINT kVsConstFMax = 256;
    static constexpr UINT kVsConstIMax = 16;
    static constexpr UINT kVsConstBMax = 16;
    static constexpr UINT kPsConstFMax = 224;
    static constexpr UINT kPsConstIMax = 16;
    static constexpr UINT kPsConstBMax = 16;

    /// Common range-validity check. count==0 short-circuits to S_OK (a
    /// documented no-op); pData==NULL with count>0 fails INVALIDCALL.
    /// Overflow-safe.
    [[nodiscard]] static HRESULT validateConstRange(UINT start, UINT count,
                                                    const void* pData,
                                                    UINT maxRegisters) {
        if (count == 0) return S_OK;
        if (!pData) return D3DERR_INVALIDCALL;
        const uint64_t end = static_cast<uint64_t>(start) + count;
        if (end > maxRegisters) return D3DERR_INVALIDCALL;
        return S_OK;
    }

    /// Read a contiguous range out of a PE const shadow.
    /// If the shadow has not been grown to cover [start, start+count),
    /// the missing tail is zero-filled (matching the post-Reset default
    /// register state).
    static void readConstShadow(const ConstShadow& shadow,
                                UINT start, void* pData, UINT count,
                                std::size_t elemSize) {
        if (count == 0 || !pData) return;
        std::memset(pData, 0, count * elemSize);
        const std::size_t base = static_cast<std::size_t>(start) * elemSize;
        const std::size_t want = static_cast<std::size_t>(count) * elemSize;
        if (shadow.values.size() <= base) return;
        const std::size_t avail =
            std::min<std::size_t>(want, shadow.values.size() - base);
        std::memcpy(pData, shadow.values.data() + base, avail);
    }

    template <typename Scope>
    HRESULT SetVertexShaderConstantFSlowBody(UINT start, const float* pData,
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
    // Diagnostics-on body for SetVertexShaderConstantF, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantFSlow(UINT start, const float* pData,
                                  UINT count) noexcept {
        if (diagnostics_->gates.callScope) {
            PeCallScope peCall(*diagnostics_, "SetVertexShaderConstantF",
                               DXMT9_PE_CALLSITE_PC());
            return SetVertexShaderConstantFSlowBody(start, pData, count,
                                                    peCall);
        }
        return SetVertexShaderConstantFSlowBody(start, pData, count,
                                                peNullCallScope_);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* pData,
                                                        UINT count) noexcept override {
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
        if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetVertexShaderConstantFSlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
        if (FAILED(hr)) return hr;
        // Shadow-only: defer the record until the next flushPendingConsts()
        // (called before each draw record + at chunk commit).
        return applyConstStateWrite(
            recorderState_.peConsts.vsConstF, &PeStateBlockConstRecorded::vsConstF,
            start, count, pData, sizeof(float) * 4);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* pData,
                                                        UINT count) noexcept override;
    // Diagnostics-on body for SetVertexShaderConstantI, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantISlow(UINT start, const INT* pData,
                                  UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        notePeDeviceCallAfterPresent("SetVertexShaderConstantI");
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.vsConstI, &PeStateBlockConstRecorded::vsConstI,
            start, count, pData, sizeof(int32_t) * 4);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) noexcept override {
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
        if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetVertexShaderConstantISlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.vsConstI, &PeStateBlockConstRecorded::vsConstI,
            start, count, pData, sizeof(int32_t) * 4);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, INT* pData,
                                                        UINT count) noexcept override;
    // Diagnostics-on body for SetVertexShaderConstantB, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantBSlow(UINT start, const BOOL* pData,
                                  UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        notePeDeviceCallAfterPresent("SetVertexShaderConstantB");
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.vsConstB, &PeStateBlockConstRecorded::vsConstB,
            start, count, pData, sizeof(uint32_t));
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) noexcept override {
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
        if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetVertexShaderConstantBSlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.vsConstB, &PeStateBlockConstRecorded::vsConstB,
            start, count, pData, sizeof(uint32_t));
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, BOOL* pData,
                                                        UINT count) noexcept override;

    /* ── stream sources ── */
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9* pBuf,
                                               UINT offset, UINT stride) noexcept override {
        return withPeCallAndHotStateSetter(
            "SetStreamSource", nullptr, nullptr,
            PeHotStateSetterFamily::VertexInput, nullptr,
            [&](auto& peCall, auto& hotSetter)
                __attribute__((always_inline)) noexcept -> HRESULT {
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("SetStreamSource", hr);
        };
        dxmt9DeviceDebugLog("device_set_stream_source device=%p stream=%u buf=%p offset=%u stride=%u",
                            this, stream, pBuf, offset, stride);
        if (stream >= 16) return finishPeCall(D3DERR_INVALIDCALL);
        D3D9PeValidatedVertexBuffer validatedBuffer{};
        const HRESULT membershipHr = D3D9PeValidateVertexBuffer(
            pBuf, static_cast<IDirect3DDevice9*>(this), &validatedBuffer);
        if (FAILED(membershipHr)) return finishPeCall(membershipHr);
        // Wine d3d9 deactivate-stream idiom: SetStreamSource(NULL, 0, 0)
        // detaches the buffer while preserving the previously cached
        // offset/stride for the stream slot — verified in
        // test_stream_source_null_layout_policy at line ~2375 where
        // (vb, 4, 32) followed by (NULL, 0, 0) yields a Get of
        // (NULL, 4, 32). Other null calls (NULL with non-zero offset
        // or non-zero stride) flow through the regular store-as-given
        // path.
        if (pBuf == nullptr && offset == 0 && stride == 0) {
            if (recorderState_.stateBlockTransaction.isRecording()) {
                StateBlockStreamSourceValue value{};
                value.buffer = {};
                value.offset = streamOff_[stream];
                value.stride = streamStr_[stream];
                const auto recordedStream = stateBlockFixedSlotKey<
                    StateBlockApplyPhysicalStore::streamSources>(stream);
                recorderState_.stateBlockTransaction.withRecordingWriter(
                    [&](auto& writer) noexcept {
                        setRecordedStreamRef(
                            writer.streamSources(), recordedStream,
                            validatedBuffer);
                        writer.streamSources().set(recordedStream, value);
                    });
                hotSetter.markDirty();
                return finishPeCall(S_OK);
            }
            if (streamSrc_[stream] == nullptr) {
                return finishPeCall(S_OK);
            }
            recorderState_.peState.transition().bindStream(stream, [&]() noexcept {
                setRef(streamSrc_[stream],
                       static_cast<IDirect3DVertexBuffer9*>(nullptr));
                recorderState_.peBindingView.streams[stream].buffer = {};
            });
            hotSetter.markDirty();
            return finishPeCall(S_OK);
        }
        if (recorderState_.stateBlockTransaction.isRecording()) {
            StateBlockStreamSourceValue value{
                .buffer = StateBlockBufferRefFactory::fromValidated(
                    validatedBuffer.stateBlockBufferCapability()),
                .offset = offset,
                .stride = stride,
            };
            const auto recordedStream = stateBlockFixedSlotKey<
                StateBlockApplyPhysicalStore::streamSources>(stream);
            StateBlockStreamSourceValue prior{};
            recorderState_.stateBlockTransaction.withRecordingWriter(
                [&](auto& writer) noexcept {
                    setRecordedStreamRef(
                        writer.streamSources(), recordedStream,
                        validatedBuffer);
                    (void)writer.streamSources().get(recordedStream, prior);
                    value.buffer = prior.buffer;
                    writer.streamSources().set(recordedStream, value);
                });
            hotSetter.markDirty();
            return finishPeCall(S_OK);
        }
        if (shadowedStreamSourceEquals(stream, pBuf, offset, stride)) {
            return finishPeCall(S_OK);
        }
        recorderState_.peState.transition().bindStream(stream, [&]() noexcept {
            setRef(streamSrc_[stream], pBuf);
            streamOff_[stream] = offset;
            streamStr_[stream] = stride;
            recorderState_.peBindingView.streams[stream] = {
                .buffer = validatedBuffer.wire(),
                .offset = offset,
                .stride = stride,
            };
        });
        hotSetter.markDirty();
        return finishPeCall(S_OK);
            });
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9** ppBuf,
                                               UINT* pOffset, UINT* pStride) noexcept override;
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT freq) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* pFreq) noexcept override;

    /* ── indices ── */
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIBuf) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIBuf) noexcept override;

    /* ── pixel shaders ── */
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFn,
                                                 IDirect3DPixelShader9** ppPS) noexcept override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pPS) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) noexcept override;
    template <typename Scope>
    HRESULT SetPixelShaderConstantFSlowBody(UINT start, const float* pData,
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
    // Diagnostics-on body for SetPixelShaderConstantF, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantFSlow(UINT start, const float* pData,
                                 UINT count) noexcept {
        if (diagnostics_->gates.callScope) {
            PeCallScope peCall(*diagnostics_, "SetPixelShaderConstantF",
                               DXMT9_PE_CALLSITE_PC());
            return SetPixelShaderConstantFSlowBody(start, pData, count,
                                                   peCall);
        }
        return SetPixelShaderConstantFSlowBody(start, pData, count,
                                               peNullCallScope_);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) noexcept override {
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
        if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetPixelShaderConstantFSlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.psConstF, &PeStateBlockConstRecorded::psConstF,
            start, count, pData, sizeof(float) * 4);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* pData,
                                                       UINT count) noexcept override;
    // Diagnostics-on body for SetPixelShaderConstantI, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantISlow(UINT start, const INT* pData,
                                 UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        notePeDeviceCallAfterPresent("SetPixelShaderConstantI");
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.psConstI, &PeStateBlockConstRecorded::psConstI,
            start, count, pData, sizeof(int32_t) * 4);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) noexcept override {
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
        if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetPixelShaderConstantISlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.psConstI, &PeStateBlockConstRecorded::psConstI,
            start, count, pData, sizeof(int32_t) * 4);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, INT* pData,
                                                       UINT count) noexcept override;
    // Diagnostics-on body for SetPixelShaderConstantB, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantBSlow(UINT start, const BOOL* pData,
                                 UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        notePeDeviceCallAfterPresent("SetPixelShaderConstantB");
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.psConstB, &PeStateBlockConstRecorded::psConstB,
            start, count, pData, sizeof(uint32_t));
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) noexcept override {
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
        if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetPixelShaderConstantBSlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.psConstB, &PeStateBlockConstRecorded::psConstB,
            start, count, pData, sizeof(uint32_t));
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, BOOL* pData,
                                                       UINT count) noexcept override;
