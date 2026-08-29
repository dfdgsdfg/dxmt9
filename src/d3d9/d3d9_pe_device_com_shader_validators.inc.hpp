[[nodiscard]] inline HRESULT validateShaderBytecodeForStage(const DWORD* code,
                                                     bool vertexStage) {
    if (!code) return D3DERR_INVALIDCALL;
    const uint32_t token = static_cast<uint32_t>(code[0]);
    const uint32_t stageHi = token >> 16;
    const uint32_t expectedStage =
        vertexStage ? kShaderHeaderVS : kShaderHeaderPS;
    if (stageHi != expectedStage) return D3DERR_INVALIDCALL;
    const uint32_t major = (token >> 8) & 0xffu;
    if (major == 0u || major > kShaderMaxMajor) return D3DERR_INVALIDCALL;
    /* Minimal "is there an END token within a sane window?" check. The
     * full token walker lives in computeShaderBytecodeWordCount on the C
     * side; we only need to reject truncated bytecode where the END
     * marker is absent in the first few words the test harness can
     * supply. The comment-skipping scan itself lives in
     * d3d9_pe_shader_bytecode_scan.hpp (host-testable; see its header
     * comment for the S.T.A.L.K.E.R. CoP debug-info incident). */
    if (dxmt9::d3d9::scanShaderBytecodeForEnd(
            reinterpret_cast<const uint32_t*>(code), kShaderBoundedScan) !=
        dxmt9::d3d9::ShaderBytecodeScanResult::EndFound) {
        return D3DERR_INVALIDCALL;
    }
    return S_OK;
}

[[nodiscard]] inline std::uint64_t hashValidatedShaderBytecode(const DWORD* code) {
    /* Comment blocks are skipped exactly as in validateShaderBytecodeForStage:
     * their payload may contain 0x0000FFFF, and a raw scan would truncate the
     * hashed range there, disagreeing with the unix-side full token walk. */
    size_t wordCount = 0;
    (void)dxmt9::d3d9::scanShaderBytecodeForEnd(
        reinterpret_cast<const uint32_t*>(code), kShaderBoundedScan, &wordCount);
    // Match dxmt9::core::hashBytes. This is intentionally not
    // dxmt9::util::fnv1a64; the core shader hash uses the historical
    // truncated FNV offset basis.
    constexpr std::uint64_t kFnvOffsetCore = 1469598103934665603ull;
    constexpr std::uint64_t kFnvPrimeCore = 1099511628211ull;
    std::uint64_t hash = kFnvOffsetCore;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(code);
    const size_t byteCount = wordCount * sizeof(uint32_t);
    for (size_t i = 0; i < byteCount; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= kFnvPrimeCore;
    }
    return hash;
}

/// Returns S_OK if a user-supplied D3DVERTEXELEMENT9 array is well-formed:
///   - bounded length (<= MAXD3DDECLLENGTH)
///   - terminated by D3DDECL_END (stream=0xFF, type=UNUSED)
///   - no in-band UNUSED elements (Wine returns E_FAIL for those)
///   - each offset is naturally aligned to the element's word size when the
///     type is FLOAT-like / 32-bit-aligned (multiples of 4). Misaligned
///     offsets are surfaced as E_FAIL per Wine, not D3DERR_INVALIDCALL.
[[nodiscard]] inline HRESULT validateVertexElements(const D3DVERTEXELEMENT9* elems) {
    if (!elems) return D3DERR_INVALIDCALL;
    constexpr size_t kMaxLen = MAXD3DDECLLENGTH + 1; /* +END */
    for (size_t i = 0; i < kMaxLen; ++i) {
        const D3DVERTEXELEMENT9& e = elems[i];
        if (e.Stream == 0xFF) {
            /* Anything with stream==0xFF must be the END sentinel. */
            if (e.Type != D3DDECLTYPE_UNUSED) return D3DERR_INVALIDCALL;
            return S_OK;
        }
        if (e.Type == D3DDECLTYPE_UNUSED) {
            /* In-band UNUSED — Wine surfaces this as E_FAIL. */
            return E_FAIL;
        }
        if (vertexElementTypeSize(e.Type) == 0) {
            /* Unknown type (non-UNUSED, non-recognized). */
            return D3DERR_INVALIDCALL;
        }
        /* All D3D9 element types are 32-bit word aligned.
         * Wine's CreateVertexDeclaration surfaces misaligned offsets as
         * E_FAIL (dlls/d3d9/tests/device.c test_vertex_declaration_alignment),
         * not D3DERR_INVALIDCALL — mirror that here for parity. */
        if ((e.Offset & 0x3u) != 0u) return E_FAIL;
    }
    /* Ran off the end without seeing an END marker. */
    return D3DERR_INVALIDCALL;
}
