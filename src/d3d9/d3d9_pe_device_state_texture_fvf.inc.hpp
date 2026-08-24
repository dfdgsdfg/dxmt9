// Ordered in-class fragment: palette, software VP, texture, and vertex-input state.
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pPasses) noexcept override;

    /* ── palette — PE shadow plus P8/A8P8 backend expansion
     *    (test_set_palette_roundtrip, test_palette_alpha_caps_policy,
     *     test_palette_current_entry_isolation, dxmt9-core-device-com-spec).
     *     P8 resources keep index data PE/C-side and re-expand through
     *     the active palette into the backend A8R8G8B8 backing texture.
     * ─────────────────────────────────────────────────────────────── */
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) noexcept override;
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY* out) noexcept override;
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) noexcept override;
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) noexcept override;

    /* ── soft VP / NPatches ── */
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL enable) noexcept override;
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() noexcept override;
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) noexcept override;
    float   STDMETHODCALLTYPE GetNPatchMode() noexcept override;

    /* ── textures ── */
    // Wine d3d9 texture-stage validation (test_limits: "There are 16
    // pixel samplers. We should be able to access all of them" —
    // SetTexture/SetSamplerState succeed for stages 0..15 regardless of
    // caps.MaxSimultaneousTextures, which only describes the FFP blend
    // stage count). Valid stages are the ps_2_0+ fragment sampler range
    // [0..15] plus the vertex texture sampler range
    // D3DVERTEXTEXTURESAMPLER0..D3DVERTEXTEXTURESAMPLER3. A former
    // guard capped this at 8 citing a nonexistent test_get_set_texture
    // assertion; that dropped s8+ bindings (3DMark05 GT3 binds the
    // water reflection at stage 8) and the translator's unbound-texture
    // fallback then sampled constant black.
    static constexpr DWORD kFragmentTextureStageCount = kPeFragmentSamplerSlots;
    static bool fragmentTextureStageSlot(DWORD stage, uint32_t& slot) noexcept {
        if (stage < kFragmentTextureStageCount) {
            slot = static_cast<uint32_t>(stage);
            return true;
        }
        return vertexTextureSamplerSlot(stage, slot);
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage,
                                          IDirect3DBaseTexture9* pTex) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage,
                                          IDirect3DBaseTexture9** ppTex) noexcept override;

    /* ── FVF / vertex declaration ── */
    /// Resolve (and cache) the implicit IDirect3DVertexDeclaration9 for an
    /// FVF. The cache owns one ref per entry; this function does NOT add a
    /// new reference for the caller. All allocation/backend/wrapper failures
    /// are translated to HRESULT so noexcept COM entries cannot terminate.
    HRESULT resolveImplicitDeclForFvf(
        DWORD fvf, IDirect3DVertexDeclaration9** out) noexcept;

    HRESULT PrepareStateBlockApplyForChild(
        const D3D9StateBlockShadow& shadow) noexcept;

    void CommitStateBlockApplyForChild(
        const D3D9StateBlockShadow& shadow) noexcept;
