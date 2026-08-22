static bool isValidD3DStateBlockType(D3DSTATEBLOCKTYPE type) {
    return type == D3DSBT_ALL || type == D3DSBT_PIXELSTATE || type == D3DSBT_VERTEXSTATE;
}

static bool isUnknownFormat(D3DFORMAT fmt) {
    return fmt == D3DFMT_UNKNOWN;
}

// D3D9 mip-chain policy shared by CreateTexture / CreateVolumeTexture /
// CreateCubeTexture (Wine dlls/d3d9/tests/device.c test_cube_textures,
// scaffold test_create_cube_texture_dim_policy in
// tests/conformance/d3d9/d3d9_conformance_resource.c): a 0 dimension is
// always invalid, and an explicit Levels beyond floor(log2(maxDimension))+1
// is D3DERR_INVALIDCALL. Levels == 0 means "full chain" and is always
// accepted. This has to be rejected here, before crossing the C bridge --
// the unix provider's resolveMipLevelCount()/fullMipLevelCount()
// (device_c_resources.cpp) pass an explicit Levels through unclamped, and an
// over-long count reaches MTLTextureDescriptor, which asserts instead of
// failing gracefully.
static uint32_t peFullMipLevelCount(UINT maxDimension) {
    UINT dimension = maxDimension;
    uint32_t levels = 1u;
    while (dimension > 1u) {
        dimension >>= 1u;
        ++levels;
    }
    return levels;
}

[[nodiscard]] static HRESULT peTextureLevelCountHResult(UINT minDimension, UINT maxDimension,
                                                        UINT levels) {
    // Every axis must be nonzero: MTLTextureDescriptor asserts on a zero
    // width/height/depth just as it does on an over-long level count, so a
    // single zero axis must be rejected here even when another axis is large.
    if (minDimension == 0u) return D3DERR_INVALIDCALL;
    if (levels != 0u && levels > peFullMipLevelCount(maxDimension)) return D3DERR_INVALIDCALL;
    return S_OK;
}

// ── D3D9 present-parameter / Ex-mode / query validation ──────────────────
// Pure, PE-side validators that encode the Windows-runtime HRESULT
// contract for the device's swap-chain-shaped entry points (Reset,
// ResetEx, CreateAdditionalSwapChain) and IDirect3DQuery9::GetDataSize.
//
// Wine behavioral oracle (read, not modified):
//   * tests/conformance/d3d9/d3d9_conformance_swapchain.c:
//       test_present_parameter_validation, test_present_parameter_normalization
//   * tests/conformance/d3d9/d3d9_conformance_device.c:
//       test_ex_create_reset_mode_validation, test_query_get_data_size_policy
//   * tests/conformance/d3d9/d3d9_queries.cpp:
//       occlusion_query_public_sizes, timestamp_query_public_sizes
// (Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.)
//
// The native value-level pin is
// tests/native/core/core_d3d9_device_validation_spec.cpp — keep the two
// in lockstep (d3d9_pe_device.cpp is a Windows-only TU, so the native
// spec mirrors these by value rather than instantiating the device).
//
// NOTE: CreateDevice-time validation lives in d3d9_pe_factory.cpp
// (validatePresentParametersD3D, parallel-owned). These device-side
// validators apply the identical present-parameter rule at the device's
// own re-configuration entry points without crossing the C bridge first.
static bool isValidPresentationIntervalRaw(UINT interval) {
    return interval == D3DPRESENT_INTERVAL_DEFAULT ||
           interval == D3DPRESENT_INTERVAL_ONE ||
           interval == D3DPRESENT_INTERVAL_TWO ||
           interval == D3DPRESENT_INTERVAL_THREE ||
           interval == D3DPRESENT_INTERVAL_FOUR ||
           interval == D3DPRESENT_INTERVAL_IMMEDIATE;
}

// Mirror: core_d3d9_device_validation_spec.cpp::mirrorPresentParamsHResult.
//
// multiSampleType / multiSampleQuality encode the Windows D3D9
// multisample-vs-swap-effect contract validated by Wine's
// wined3d_swapchain_state_init: multisampling is only legal with
// D3DSWAPEFFECT_DISCARD, and a non-zero MultiSampleQuality requires a
// non-NONE MultiSampleType. (test_swapchain_multisample_reset resets
// with DISCARD + 2_SAMPLES, which stays valid under this rule.)
[[nodiscard]] static HRESULT pePresentParamsHResult(D3DSWAPEFFECT swapEffect,
                                                    UINT backBufferCount,
                                                    UINT presentationInterval,
                                                    D3DMULTISAMPLE_TYPE multiSampleType,
                                                    UINT multiSampleQuality,
                                                    bool extended) {
    switch (swapEffect) {
    case D3DSWAPEFFECT_DISCARD:
    case D3DSWAPEFFECT_FLIP:
    case D3DSWAPEFFECT_COPY:
        break;
    case D3DSWAPEFFECT_FLIPEX:
        if (extended) break;
        return D3DERR_INVALIDCALL;
    default:
        return D3DERR_INVALIDCALL;
    }

    const UINT maxBackBufferCount = extended ? 30u : 3u;
    if (backBufferCount > maxBackBufferCount) {
        return D3DERR_INVALIDCALL;
    }
    // COPY swap effect supports at most a single back buffer.
    if (swapEffect == D3DSWAPEFFECT_COPY && backBufferCount > 1u) {
        return D3DERR_INVALIDCALL;
    }
    if (!isValidPresentationIntervalRaw(presentationInterval)) {
        return D3DERR_INVALIDCALL;
    }
    // Multisampled swap chains require D3DSWAPEFFECT_DISCARD; a quality
    // level cannot be requested without a sample type.
    if (multiSampleType != D3DMULTISAMPLE_NONE &&
        swapEffect != D3DSWAPEFFECT_DISCARD) {
        return D3DERR_INVALIDCALL;
    }
    if (multiSampleType == D3DMULTISAMPLE_NONE && multiSampleQuality != 0u) {
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

// Mirror: core_d3d9_device_validation_spec.cpp::mirrorResetExModeHResult.
// hasMode == (pFsMode != nullptr); when hasMode is false modeSize/modeW/
// modeH are ignored.
[[nodiscard]] static HRESULT peResetExModeHResult(bool windowed, bool hasMode,
                                                  UINT modeSize, UINT modeW,
                                                  UINT modeH, UINT ppW, UINT ppH) {
    if (hasMode && modeSize != sizeof(D3DDISPLAYMODEEX)) {
        return D3DERR_INVALIDCALL;
    }
    // Windowed ResetEx must pass a NULL mode; fullscreen must pass a mode.
    if (windowed ? hasMode : !hasMode) {
        return D3DERR_INVALIDCALL;
    }
    // Fullscreen mode dimensions must match the requested back-buffer size
    // (this also rejects a zero-dimension mode when the back buffer is sized).
    if (hasMode && (modeW != ppW || modeH != ppH)) {
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

// Mirror: core_d3d9_device_validation_spec.cpp::mirrorNormalizeBackBufferCount.
// BackBufferCount == 0 normalizes to 1 (the documented minimum the
// swap-chain reports back through GetPresentParameters).
[[nodiscard]] static UINT peNormalizeBackBufferCount(UINT count) {
    return count == 0u ? 1u : count;
}

// Mirror: core_d3d9_device_validation_spec.cpp::mirrorQueryDataSizeForType.
// Per-type IDirect3DQuery9::GetDataSize byte size (0 = unsupported type).
[[nodiscard]] static DWORD peQueryDataSizeForType(D3DQUERYTYPE type) {
    switch (type) {
    case D3DQUERYTYPE_EVENT:             return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION:         return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP:         return sizeof(UINT64);
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: return sizeof(BOOL);
    case D3DQUERYTYPE_TIMESTAMPFREQ:     return sizeof(UINT64);
    default:                             return 0u;
    }
}

// T4 (D3D9Ex shared-handle, SYSTEMMEM partial): for SYSTEMMEM textures
// the test_user_memory oracle (Wine d3d9ex tests) requires that
//   - 0 levels (auto-mip)            -> D3DERR_INVALIDCALL
//   - levels > 1                     -> D3DERR_INVALIDCALL
//   - SCRATCH pool                   -> D3DERR_INVALIDCALL
//   - SYSTEMMEM, levels == 1         -> S_OK; user pointer aliased
// allowSystemMemUserMemory is false for cube/volume textures since the
// partial scope only covers 2D textures and offscreen plain surfaces.
// The width/height == 1x1 narrowing for 2D textures is enforced at the
// call site (validate* doesn't see W/H).
//
// DEFAULT-pool shared-handle contract: Wine establishes that an Ex device
// proceeds rather than returning the old E_NOTIMPL placeholder. dxmt9 now
// forwards the in/out value to the unix provider: zero creates a process-local
// shared token, a known token opens the same Metal backing, and an unknown
// nonzero value retains Wine's permissive create behavior. Cross-process
// Win32-handle transport still requires IOSurface / MTLSharedTextureHandle.
[[nodiscard]] static HRESULT validateSharedHandleForTexture(bool extended,
                                              HANDLE* sharedHandle,
                                              D3DPOOL pool,
                                              UINT levels,
                                              bool allowSystemMemUserMemory) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool == D3DPOOL_SYSTEMMEM) {
        if (!allowSystemMemUserMemory) return D3DERR_INVALIDCALL;
        if (levels != 1) return D3DERR_INVALIDCALL;
        return S_OK;
    }
    if (pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    // Extended + DEFAULT: provider handles create/open-existing semantics.
    return S_OK;
}

// VB/IB shared-handle contract (Wine d3d9 d3d9_device_CreateVertexBuffer /
// CreateIndexBuffer): non-extended -> E_NOTIMPL; extended + non-DEFAULT pool
// -> D3DERR_NOTAVAILABLE; extended + DEFAULT -> provider shared path.
[[nodiscard]] static HRESULT validateSharedHandleForBuffer(bool extended,
                                             HANDLE* sharedHandle,
                                             D3DPOOL pool) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool != D3DPOOL_DEFAULT) return D3DERR_NOTAVAILABLE;
    // Extended + DEFAULT: provider handles create/open-existing semantics.
    return S_OK;
}

// Offscreen plain surface shared-handle contract (Wine d3d9
// d3d9_device_CreateOffscreenPlainSurface):
//   - SYSTEMMEM -> S_OK; user pointer aliased
//   - SCRATCH   -> D3DERR_INVALIDCALL
//   - DEFAULT   -> provider shared path
[[nodiscard]] static HRESULT validateSharedHandleForSurface(bool extended,
                                              HANDLE* sharedHandle,
                                              D3DPOOL pool,
                                              bool allowSystemMemUserMemory) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool == D3DPOOL_SYSTEMMEM) {
        if (!allowSystemMemUserMemory) return D3DERR_INVALIDCALL;
        return S_OK;
    }
    if (pool == D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    if (pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    // Extended + DEFAULT: provider handles create/open-existing semantics.
    return S_OK;
}

// Render-target / depth-stencil surfaces are DEFAULT-pool only. Wine d3d9
// d3d9_device_CreateRenderTarget / CreateDepthStencilSurface: non-extended
// -> E_NOTIMPL; extended -> provider shared path.
[[nodiscard]] static HRESULT validateSharedHandleForDefaultSurface(bool extended,
                                                     HANDLE* sharedHandle) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    // Extended: provider handles create/open-existing semantics.
    return S_OK;
}
