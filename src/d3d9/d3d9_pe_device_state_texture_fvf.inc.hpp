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
        DWORD fvf, IDirect3DVertexDeclaration9** out) noexcept {
        if (!out) return D3DERR_INVALIDCALL;
        *out = nullptr;
        if (fvf == 0) return S_OK;
        if (auto it = fvfDeclCache_.find(fvf); it != fvfDeclCache_.end()) {
            *out = it->second;
            return S_OK;
        }
        try {
            std::vector<D3DVERTEXELEMENT9> elements;
            fvfToVertexElements(fvf, elements);
            D9CVertexElement tmp[MAXD3DDECLLENGTH + 1]{};
            const size_t n = elements.size();
            if (n > MAXD3DDECLLENGTH + 1) return D3DERR_INVALIDCALL;
            for (size_t i = 0; i < n; ++i) {
                tmp[i].stream     = elements[i].Stream;
                tmp[i].offset     = elements[i].Offset;
                tmp[i].type       = elements[i].Type;
                tmp[i].method     = elements[i].Method;
                tmp[i].usage      = elements[i].Usage;
                tmp[i].usageIndex = elements[i].UsageIndex;
            }
            const auto result =
                dxmt9::d3d9::pe::createImplicitFvfDeclTransaction<
                    D9CVertexDecl*, IDirect3DVertexDeclaration9*>(
                    [&]() {
                        return dxmt9c_device_create_vertex_declaration(dev_, tmp);
                    },
                    [](D9CVertexDecl* decl) noexcept {
                        dxmt9c_vdecl_release(decl);
                    },
                    [&](D9CVertexDecl* decl) {
                        IDirect3DVertexDeclaration9* wrapper =
                            CreatePeVertexDecl(decl, this, shaderDeclarationContext());
                        if (wrapper) {
                            // Create must precede any failure cleanup because
                            // the wrapper destructor emits ObjectDestroy.
                            D3D9PeValidatedDeclaration validated{};
                            if (SUCCEEDED(D3D9PeValidateVertexDecl(
                                    wrapper,
                                    static_cast<IDirect3DDevice9*>(this),
                                    &validated))) {
                                notifyRenderTapeCreatedVertexDecl(
                                    decl, validated.wire(),
                                    std::span<const std::byte>(
                                        reinterpret_cast<const std::byte *>(tmp),
                                        n * sizeof(tmp[0])), n);
                            }
                        }
                        return wrapper;
                    },
                    [](IDirect3DVertexDeclaration9* decl) noexcept {
                        decl->Release();
                    },
                    [&](IDirect3DVertexDeclaration9* decl) {
                        return fvfDeclCache_.emplace(fvf, decl).second;
                    });
            if (!result) {
                return result.failure ==
                        dxmt9::d3d9::pe::ImplicitFvfDeclFailure::Backend
                    ? D3DERR_INVALIDCALL
                    : E_OUTOFMEMORY;
            }
            *out = result.wrapper;
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT PrepareStateBlockApplyForChild(
        const D3D9StateBlockShadow& shadow) noexcept {
        if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
        const auto snapshot = shadow.snapshot();
        discardPreparedStateBlockApply();
        const auto prepareConst = [&](ConstShadow& live,
                                      const StateBlockConstShadow& recorded,
                                      std::size_t elemSize,
                                      std::size_t maxElems) -> HRESULT {
            if (recorded.valueSize > StateBlockConstShadow::kMaxBytes ||
                recorded.trackedSize > StateBlockConstShadow::kMaxElems ||
                recorded.trackedSize > maxElems ||
                recorded.valueSize <
                    static_cast<std::size_t>(recorded.trackedSize) * elemSize) {
                return D3DERR_INVALIDCALL;
            }
            for (std::size_t i = 0; i < recorded.trackedSize; ++i) {
                if (recorded.trackedElems[i] != 0u &&
                    (i + 1u) * elemSize > recorded.valueSize) {
                    return D3DERR_INVALIDCALL;
                }
            }
            try {
                // Reserve only: preserving live values/dirty bits makes this
                // phase invisible if backend Apply subsequently fails.
                live.reserveCapacity(recorded.valueSize,
                                      recorded.trackedSize);
            } catch (const std::bad_alloc&) {
                return E_OUTOFMEMORY;
            }
            return S_OK;
        };
        HRESULT hr = prepareConst(recorderState_.peConsts.vsConstF, snapshot.constants().vsConstF,
                                  sizeof(float) * 4u, kVsConstFMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.vsConstI, snapshot.constants().vsConstI,
                          sizeof(std::int32_t) * 4u, kVsConstIMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.vsConstB, snapshot.constants().vsConstB,
                          sizeof(std::uint32_t), kVsConstBMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.psConstF, snapshot.constants().psConstF,
                          sizeof(float) * 4u, kPsConstFMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.psConstI, snapshot.constants().psConstI,
                          sizeof(std::int32_t) * 4u, kPsConstIMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.psConstB, snapshot.constants().psConstB,
                          sizeof(std::uint32_t), kPsConstBMax);
        if (FAILED(hr)) return hr;

        // Resolve/cache and retain by walking the authoritative 26-store
        // APPLY PHYSICAL inventory. Resolution stays before every retain so a failing implicit
        // FVF lookup leaves no staged ownership. Commit can then stay
        // allocation-free after backend mutation; a tracked null remains
        // represented by its occupancy mask.
        HRESULT categoryHr = S_OK;
        snapshot.categories().forEachApplyPhysical(
            [&]<StateBlockApplyPhysicalStore store>(const auto& values) {
                constexpr auto descriptor =
                    stateBlockApplyPhysicalDescriptor<store>();
                constexpr auto role = descriptor.role;
                if constexpr (role ==
                              StateBlockApplyCategoryRole::ImplicitFvf) {
                    values.forEach([&](std::size_t, DWORD fvf) {
                        IDirect3DVertexDeclaration9* decl = nullptr;
                        if (SUCCEEDED(categoryHr)) {
                            categoryHr = resolveImplicitDeclForFvf(fvf, &decl);
                        }
                    });
                } else if constexpr (
                    descriptor.kind == StateBlockApplyPhysicalKind::Fixed) {
                    static_assert(
                        role == StateBlockApplyCategoryRole::Value ||
                        role == StateBlockApplyCategoryRole::
                                    CandidateOwnedVertexDeclaration ||
                        role >= StateBlockApplyCategoryRole::StagedTexture,
                        "StateBlock resolution role is not handled");
                } else if constexpr (
                    descriptor.kind == StateBlockApplyPhysicalKind::Keyed) {
                    static_assert(
                        store == StateBlockApplyPhysicalStore::renderStates ||
                        store == StateBlockApplyPhysicalStore::textureStageStates ||
                        store == StateBlockApplyPhysicalStore::samplerStates ||
                        store == StateBlockApplyPhysicalStore::transforms,
                        "StateBlock Prepare keyed store is not bound");
                } else {
                    static_assert(
                        store == StateBlockApplyPhysicalStore::vsConstF ||
                        store == StateBlockApplyPhysicalStore::vsConstI ||
                        store == StateBlockApplyPhysicalStore::vsConstB ||
                        store == StateBlockApplyPhysicalStore::psConstF ||
                        store == StateBlockApplyPhysicalStore::psConstI ||
                        store == StateBlockApplyPhysicalStore::psConstB,
                        "StateBlock Prepare constant store is not bound");
                }
            });
        if (FAILED(categoryHr)) return categoryHr;
        snapshot.categories().forEachApplyPhysical(
            [&]<StateBlockApplyPhysicalStore store>(const auto& values) {
                constexpr auto descriptor =
                    stateBlockApplyPhysicalDescriptor<store>();
                constexpr auto role = descriptor.role;
                if constexpr (
                    role == StateBlockApplyCategoryRole::StagedTexture) {
                    values.forEach([&](std::size_t slot, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageTexture(
                                stateBlockTextureSlotKey(slot), value,
                                d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedStreamSource) {
                    values.forEach([&](std::size_t slot, const auto& value) {
                        if (!recorderState_.stateBlockTransaction.stageStream(
                                stateBlockStreamSlotKey(slot), value,
                                d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedVertexShader) {
                    values.forEach([&](std::size_t, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageVertexShader(
                                value, d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedPixelShader) {
                    values.forEach([&](std::size_t, auto value) {
                        if (!recorderState_.stateBlockTransaction.stagePixelShader(
                                value, d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedIndexBuffer) {
                    values.forEach([&](std::size_t, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageIndexBuffer(
                                value, d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedRenderTarget) {
                    values.forEach([&](std::size_t slot, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageRenderTarget(
                                stateBlockRenderTargetSlotKey(slot), value,
                                d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedDepthStencil) {
                    values.forEach([&](std::size_t, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageDepthStencil(
                                value, d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else {
                    if constexpr (descriptor.kind ==
                                  StateBlockApplyPhysicalKind::Keyed) {
                        static_assert(
                            store == StateBlockApplyPhysicalStore::renderStates ||
                            store == StateBlockApplyPhysicalStore::textureStageStates ||
                            store == StateBlockApplyPhysicalStore::samplerStates ||
                            store == StateBlockApplyPhysicalStore::transforms,
                            "StateBlock Prepare keyed store is not bound");
                    } else if constexpr (descriptor.kind ==
                                         StateBlockApplyPhysicalKind::Constant) {
                        static_assert(
                            store == StateBlockApplyPhysicalStore::vsConstF ||
                            store == StateBlockApplyPhysicalStore::vsConstI ||
                            store == StateBlockApplyPhysicalStore::vsConstB ||
                            store == StateBlockApplyPhysicalStore::psConstF ||
                            store == StateBlockApplyPhysicalStore::psConstI ||
                            store == StateBlockApplyPhysicalStore::psConstB,
                            "StateBlock Prepare constant store is not bound");
                    } else {
                        static_assert(
                            role == StateBlockApplyCategoryRole::Value ||
                            role == StateBlockApplyCategoryRole::ImplicitFvf ||
                            role == StateBlockApplyCategoryRole::
                                        CandidateOwnedVertexDeclaration,
                            "StateBlock Prepare role is not handled");
                    }
                }
            });
        if (FAILED(categoryHr)) {
            recorderState_.stateBlockTransaction.discardPrepared(
                d3d9PeReleaseStateBlockRef);
            return categoryHr;
        }
        recorderState_.stateBlockTransaction.markApplyPrepared();
        return categoryHr;
    }

    void CommitStateBlockApplyForChild(
        const D3D9StateBlockShadow& shadow) noexcept {
        if (recorderState_.stateBlockTransaction.isPoisoned()) return;
        const auto snapshot = shadow.snapshot();
        auto copyConst = [](ConstShadow& live,
                            const StateBlockConstShadow& recorded,
                            std::size_t elemSize) noexcept {
            (void)recorded.forEachRange(
                elemSize, [&](std::uint32_t start, std::uint32_t count,
                              const std::uint8_t* bytes) {
                    const std::size_t begin =
                        static_cast<std::size_t>(start) * elemSize;
                    const std::size_t end =
                        begin + static_cast<std::size_t>(count) * elemSize;
                    // PrepareStateBlockApplyForChild reserved this capacity;
                    // resize therefore cannot allocate in the commit phase.
                    if (live.values.size() < end) live.values.resize(end);
                    if (live.dirtyElems.size() <
                        static_cast<std::size_t>(start) + count) {
                        live.dirtyElems.resize(
                            static_cast<std::size_t>(start) + count);
                    }
                    std::memcpy(live.values.data() + begin, bytes, end - begin);
                    std::fill(live.dirtyElems.begin(), live.dirtyElems.end(),
                              std::uint8_t{0});
                    return true;
                });
            live.dirtyStart = live.dirtyEnd = 0u;
        };

        auto* const semanticTokens = scalarSemanticObserver();
        snapshot.renderStates().forEach([&](RenderStateSlot key, DWORD value) {
            recorderState_.peState.maintenance().renderStateShadowTyped().set(key, value);
            recorderState_.peState.maintenance().pendingRenderStatesTyped().erase(key);
            if (semanticTokens) {
                (void)semanticTokens->eraseSuperseded(
                    dxmt9::d3d9::pe::ScalarSemanticCategory::RenderState,
                    rawSlot(key));
            }
        });
        snapshot.textureStageStates().forEach(
            [&](TextureStageIndex stage, TextureStageStateType type,
                DWORD value) {
                recorderState_.peState.maintenance().tssShadowTyped().set(stage, type, value);
                recorderState_.peState.maintenance().pendingTssTyped().erase(stage, type);
                if (semanticTokens) {
                    (void)semanticTokens->eraseSuperseded(
                        dxmt9::d3d9::pe::ScalarSemanticCategory::
                            TextureStageState,
                        rawSlot(stage), rawSlot(type));
                }
            });
        snapshot.samplerStates().forEach(
            [&](SamplerIndex sampler, SamplerStateType type, DWORD value) {
                recorderState_.peState.maintenance().samplerStateShadowTyped().set(sampler, type, value);
                recorderState_.peState.maintenance().pendingSamplerStatesTyped().erase(sampler, type);
                if (semanticTokens) {
                    (void)semanticTokens->eraseSuperseded(
                        dxmt9::d3d9::pe::ScalarSemanticCategory::SamplerState,
                        rawSlot(sampler), rawSlot(type));
                }
            });
        snapshot.transforms().forEach([&](TransformState key, const D9CMatrix& value) {
            recorderState_.peState.maintenance().transformShadowTyped().set(key, value);
            recorderState_.peState.maintenance().pendingTransformsTyped().erase(key);
        });

        // This visitor is deliberately compile-time-only binding for the
        // manually specialized keyed/constant commit code surrounding the
        // generated fixed-category visitor below. A new physical row must be
        // classified here before this PE COM TU can compile.
        snapshot.categories().forEachApplyPhysical(
            []<StateBlockApplyPhysicalStore store>(const auto&) {
                constexpr auto descriptor =
                    stateBlockApplyPhysicalDescriptor<store>();
                if constexpr (descriptor.kind ==
                              StateBlockApplyPhysicalKind::Keyed) {
                    static_assert(
                        store == StateBlockApplyPhysicalStore::renderStates ||
                        store == StateBlockApplyPhysicalStore::textureStageStates ||
                        store == StateBlockApplyPhysicalStore::samplerStates ||
                        store == StateBlockApplyPhysicalStore::transforms,
                        "StateBlock Commit keyed store is not bound");
                } else if constexpr (descriptor.kind ==
                                     StateBlockApplyPhysicalKind::Constant) {
                    static_assert(
                        store == StateBlockApplyPhysicalStore::vsConstF ||
                        store == StateBlockApplyPhysicalStore::vsConstI ||
                        store == StateBlockApplyPhysicalStore::vsConstB ||
                        store == StateBlockApplyPhysicalStore::psConstF ||
                        store == StateBlockApplyPhysicalStore::psConstI ||
                        store == StateBlockApplyPhysicalStore::psConstB,
                        "StateBlock Commit constant store is not bound");
                } else {
                    static_assert(
                        descriptor.role == StateBlockApplyCategoryRole::Value ||
                        descriptor.role == StateBlockApplyCategoryRole::ImplicitFvf ||
                        descriptor.role == StateBlockApplyCategoryRole::
                                    CandidateOwnedVertexDeclaration ||
                        descriptor.role >= StateBlockApplyCategoryRole::
                                    StagedTexture,
                        "StateBlock Commit fixed store is not bound");
                }
            });

        using Category = StateBlockRecorded::Category;
        snapshot.categories().forEachTypedCategory(
            [&]<Category category>(const auto& values) {
                if constexpr (category == Category::textures) {
                    values.forEach([&](std::size_t slot, auto) {
                        if (textures_[slot]) textures_[slot]->Release();
                        textures_[slot] =
                            recorderState_.stateBlockTransaction.takeTexture(
                                stateBlockTextureSlotKey(slot)).raw();
                        D3D9PeValidatedTexture validated{};
                        DXMT_ASSERT(SUCCEEDED(D3D9PeValidateTexture(
                            textures_[slot], static_cast<IDirect3DDevice9*>(this),
                            &validated)));
                        recorderState_.peBindingView.textures[slot] =
                            validated.wire();
                        recorderState_.peState.maintenance().pendingTextureMask() &= ~(1u << slot);
                    });
                } else if constexpr (category == Category::streamSources) {
                    values.forEach([&](std::size_t slot, const auto&) {
                        if (streamSrc_[slot]) streamSrc_[slot]->Release();
                        const auto staged =
                            recorderState_.stateBlockTransaction.takeStream(
                                stateBlockStreamSlotKey(slot));
                        streamSrc_[slot] = staged.buffer.raw();
                        streamOff_[slot] = staged.offset;
                        streamStr_[slot] = staged.stride;
                        D3D9PeValidatedVertexBuffer validated{};
                        DXMT_ASSERT(SUCCEEDED(D3D9PeValidateVertexBuffer(
                            streamSrc_[slot],
                            static_cast<IDirect3DDevice9*>(this), &validated)));
                        recorderState_.peBindingView.streams[slot] = {
                            .buffer = validated.wire(),
                            .offset = staged.offset,
                            .stride = staged.stride,
                        };
                        recorderState_.peState.maintenance().pendingStreamMask() &= ~(1u << slot);
                    });
                } else if constexpr (category ==
                                     Category::streamFrequencies) {
                    values.forEach([&](std::size_t slot, std::uint32_t value) {
                        streamFreq_[slot] = value;
                    });
                } else if constexpr (category == Category::vertexShader) {
                    values.forEach([&](std::size_t, auto) {
                        if (vs_) vs_->Release();
                        vs_ = recorderState_.stateBlockTransaction
                                  .takeVertexShader().raw();
                        D3D9PeValidatedVertexShader validated{};
                        DXMT_ASSERT(SUCCEEDED(D3D9PeValidateVertexShader(
                            vs_, static_cast<IDirect3DDevice9*>(this),
                            &validated)));
                        recorderState_.peBindingView.vs = validated.wire();
                        recorderState_.peState.maintenance().pendingVs() = false;
                    });
                } else if constexpr (category == Category::pixelShader) {
                    values.forEach([&](std::size_t, auto) {
                        if (ps_) ps_->Release();
                        ps_ = recorderState_.stateBlockTransaction
                                  .takePixelShader().raw();
                        D3D9PeValidatedPixelShader validated{};
                        DXMT_ASSERT(SUCCEEDED(D3D9PeValidatePixelShader(
                            ps_, static_cast<IDirect3DDevice9*>(this),
                            &validated)));
                        recorderState_.peBindingView.ps = validated.wire();
                        recorderState_.peState.maintenance().pendingPs() = false;
                    });
                } else if constexpr (category == Category::fvf) {
                    values.forEach([&](std::size_t, DWORD value) {
                        fvf_ = value;
                        recorderState_.peState.maintenance().pendingFvf() = false;
                        recorderState_.peState.maintenance().pendingVdecl() = false;
                        if (value == 0u) {
                            vdecl_ = nullptr;
                        } else {
                            const auto it = fvfDeclCache_.find(value);
                            DXMT_ASSERT(it != fvfDeclCache_.end());
                            vdecl_ = it == fvfDeclCache_.end()
                                ? nullptr : it->second;
                        }
                        D3D9PeValidatedDeclaration validated{};
                        DXMT_ASSERT(SUCCEEDED(D3D9PeValidateVertexDecl(
                            vdecl_, static_cast<IDirect3DDevice9*>(this),
                            &validated)));
                        recorderState_.peBindingView.fvf = value;
                        recorderState_.peBindingView.vdecl = validated.wire();
                    });
                } else if constexpr (category ==
                                     Category::vertexDeclaration) {
                    values.forEach([&](std::size_t, auto value) {
                        vdecl_ = value.raw();
                        D3D9PeValidatedDeclaration validated{};
                        DXMT_ASSERT(SUCCEEDED(D3D9PeValidateVertexDecl(
                            vdecl_, static_cast<IDirect3DDevice9*>(this),
                            &validated)));
                        recorderState_.peBindingView.vdecl = validated.wire();
                        recorderState_.peState.maintenance().pendingVdecl() = false;
                    });
                } else if constexpr (category == Category::indexBuffer) {
                    values.forEach([&](std::size_t, auto) {
                        if (indexBuf_) indexBuf_->Release();
                        indexBuf_ = recorderState_.stateBlockTransaction
                                        .takeIndexBuffer().raw();
                        D3D9PeValidatedIndexBuffer validated{};
                        DXMT_ASSERT(SUCCEEDED(D3D9PeValidateIndexBuffer(
                            indexBuf_, static_cast<IDirect3DDevice9*>(this),
                            &validated)));
                        recorderState_.peBindingView.indexBuffer =
                            validated.wire();
                        recorderState_.peState.maintenance().pendingIb() = false;
                    });
                } else if constexpr (category == Category::renderTargets) {
                    values.forEach([&](std::size_t slot, auto) {
                        if (rtSlots_[slot]) rtSlots_[slot]->Release();
                        rtSlots_[slot] = recorderState_.stateBlockTransaction
                            .takeRenderTarget(
                                stateBlockRenderTargetSlotKey(slot)).raw();
                        D3D9PeValidatedSurface validated{};
                        DXMT_ASSERT(SUCCEEDED(D3D9PeValidateSurface(
                            rtSlots_[slot], static_cast<IDirect3DDevice9*>(this),
                            &validated)));
                        recorderState_.peBindingView.renderTargets[slot] =
                            validated.wire();
                        rtSlotExplicit_[slot] = true;
                        recorderState_.peBindingView.rtExplicitMask =
                            currentRtExplicitMask();
                        recorderState_.peState.maintenance().pendingRtMask() &= ~(1u << slot);
                    });
                } else if constexpr (category == Category::depthStencil) {
                    values.forEach([&](std::size_t, auto) {
                        if (dsSurface_) dsSurface_->Release();
                        dsSurface_ = recorderState_.stateBlockTransaction
                                         .takeDepthStencil().raw();
                        D3D9PeValidatedSurface validated{};
                        DXMT_ASSERT(SUCCEEDED(D3D9PeValidateSurface(
                            dsSurface_, static_cast<IDirect3DDevice9*>(this),
                            &validated)));
                        recorderState_.peBindingView.depthStencil =
                            validated.wire();
                        dsSurfaceExplicit_ = true;
                        recorderState_.peState.maintenance().pendingDs() = false;
                    });
                } else if constexpr (category == Category::viewport) {
                    values.forEach([&](std::size_t, const D9CViewport& value) {
                        recorderState_.peState.maintenance().viewportShadow() = value;
                        recorderState_.peState.maintenance().pendingViewport() = false;
                    });
                } else if constexpr (category == Category::scissor) {
                    values.forEach([&](std::size_t, const D9CRect& value) {
                        recorderState_.peState.maintenance().scissorShadow() = value;
                        recorderState_.peState.maintenance().pendingScissor() = false;
                    });
                } else if constexpr (category == Category::material) {
                    values.forEach([&](std::size_t, const D9CMaterial& value) {
                        recorderState_.peState.maintenance().materialShadow() = value;
                        recorderState_.peState.maintenance().pendingMaterial() = false;
                    });
                } else if constexpr (category == Category::clipPlanes) {
                    values.forEach([&](std::size_t idx, const auto& value) {
                        std::memcpy(recorderState_.peState.maintenance().clipPlaneShadow() + idx * 4u,
                                    value.data(), sizeof(value));
                        recorderState_.peState.maintenance().pendingClipPlaneMask() &= ~(1u << idx);
                    });
                } else if constexpr (category == Category::lights) {
                    values.forEach([&](std::size_t idx,
                                       const D9CLight& value) {
                        recorderState_.peState.maintenance().lightShadow()[idx] = value;
                        recorderState_.peState.maintenance().pendingLightSlotMask() &= ~(1u << idx);
                    });
                } else if constexpr (category == Category::lightEnables) {
                    values.forEach([&](std::size_t idx,
                                       std::uint32_t value) {
                        const std::uint32_t bit = 1u << idx;
                        if (value) recorderState_.peState.maintenance().lightEnableShadow() |= bit;
                        else recorderState_.peState.maintenance().lightEnableShadow() &= ~bit;
                        recorderState_.peState.maintenance().pendingLightEnableValidMask() &= ~bit;
                        recorderState_.peState.maintenance().pendingLightEnableMask() &= ~bit;
                    });
                } else {
                    []<bool handled = false>() {
                        static_assert(handled,
                                      "StateBlock Commit category omitted");
                    }();
                }
            });
        if (snapshot.hasVdecl() && !snapshot.categories().vertexDeclaration().contains(0u)) {
            vdecl_ = snapshot.vdecl();
            D3D9PeValidatedDeclaration validated{};
            DXMT_ASSERT(SUCCEEDED(D3D9PeValidateVertexDecl(
                vdecl_, static_cast<IDirect3DDevice9*>(this), &validated)));
            recorderState_.peBindingView.vdecl = validated.wire();
            recorderState_.peState.maintenance().pendingVdecl() = false;
        }
        copyConst(recorderState_.peConsts.vsConstF, snapshot.constants().vsConstF,
                  sizeof(float) * 4u);
        copyConst(recorderState_.peConsts.vsConstI, snapshot.constants().vsConstI,
                  sizeof(std::int32_t) * 4u);
        copyConst(recorderState_.peConsts.vsConstB, snapshot.constants().vsConstB,
                  sizeof(std::uint32_t));
        copyConst(recorderState_.peConsts.psConstF, snapshot.constants().psConstF,
                  sizeof(float) * 4u);
        copyConst(recorderState_.peConsts.psConstI, snapshot.constants().psConstI,
                  sizeof(std::int32_t) * 4u);
        copyConst(recorderState_.peConsts.psConstB, snapshot.constants().psConstB,
                  sizeof(std::uint32_t));
        DXMT_ASSERT(!recorderState_.stateBlockTransaction.hasPreparedApply());
        recorderState_.stateBlockTransaction.finishPreparedApply();
    }
