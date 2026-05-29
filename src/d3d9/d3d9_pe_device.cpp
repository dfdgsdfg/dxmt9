/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex and recorder glue.
 * All methods delegate to the dxmt9c_* C API from dxmt9/device_c.h. */

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include "d3d9_pe.hpp"
#include "d3d9_pe_device_child.hpp"
#include "d3d9_pe_draw_packet.hpp"
#include "d3d9_pe_recorder.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "dxmt9/d3d9_raster_status.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

static D3DFORMAT exposeAdapterDisplayFormat(D3DFORMAT fmt) {
    if (fmt == D3DFMT_A8R8G8B8) return D3DFMT_X8R8G8B8;
    return fmt;
}

static void dxmt9DeviceDebugLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-device", fmt, args);
    va_end(args);
}

static void dxmt9DeviceInfoLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-device", fmt, args);
    va_end(args);
}

static bool dxmt9PeRecorderStatsEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_RECORDER_STATS");
    return enabled;
}

static bool dxmt9PeRecorderChunkLogEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_RECORDER_CHUNK_LOG");
    return enabled;
}

// Structural invariant: the chunk recorder and PE state shadow are the
// production path. Draw / Set* hot paths have no runtime env opt-out to
// per-call bridge mode for dxmt9c_device_set_render_state /
// set_texture / set_stream_source / set_fvf / set_vs / set_ps /
// set_vertex_declaration / set_render_target / set_depth_stencil_surface
// / set_viewport / set_scissor_rect / set_texture_stage_state /
// set_sampler_state / set_material / set_clip_plane / set_transform /
// set_light / light_enable / set_indices unix-calls. New Set*-style code
// should update PE shadow state and encode it into command records.
//
// Phase 16: full-snapshot mode. When set, every draw packet emitted in
// chunk-recorder mode carries the COMPLETE BaseDrawState snapshot (every
// field marked valid + populated from the PE shadow), not just the
// delta-since-last-packet. Wire size grows (typical packet jumps from
// ~100B to ~1KB) but the importer becomes idempotent — every packet is
// self-contained and can be replayed independently of prior packets.
// Off (default) keeps the delta optimization that makes run-coalescing
// detection cheap (packetHasNoStateDelta == "all valid bits zero").
//
// SOLE APPLICATION SITE: buildDrawPrimitivePacket() below, after the
// delta block populates valid/mask fields from pending-* PE state. When
// enabled, the snapshot block overrides every delta field with the full
// shadow contents (render-state table, every populated texture slot,
// every populated stream, every shadow-driven scalar, all sampler/TSS
// tables, full clip-plane mask 0x3F, every transform/light slot). Both
// modes share the same wire layout (D9CDrawPrimitivePacket); only the
// valid/mask population policy differs.
//
// Equivalence guarantee: applying a delta-mode packet sequence vs the
// matching full-snapshot sequence through device_c_chunk_replay's
// applyDrawPacketStateDirect() yields identical effective state. The
// regression guard is tests/native/bridge/
// pe_full_snapshot_equivalence_spec.cpp.
static bool dxmt9PeFullSnapshotEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_DRAW_FULL_SNAPSHOT");
    return enabled;
}

static bool isValidD3DStateBlockType(D3DSTATEBLOCKTYPE type) {
    return type == D3DSBT_ALL || type == D3DSBT_PIXELSTATE || type == D3DSBT_VERTEXSTATE;
}

static bool isUnknownFormat(D3DFORMAT fmt) {
    return fmt == D3DFMT_UNKNOWN;
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
// DEFAULT-pool shared-handle contract (Wine d3d9 d3d9_device_CreateTexture
// / CreateCubeTexture / CreateVolumeTexture, commit
// 6e073d28dee3af7f4c965daec94644e0f9f92727): on an extended (D3D9Ex) device,
// a non-NULL pSharedHandle in D3DPOOL_DEFAULT logs a FIXME and then *proceeds
// to create the resource normally* — the handle is ignored, not rejected.
// dxmt9 mirrors that observable HRESULT (S_OK + a real DEFAULT-pool resource)
// even though cross-process sharing is not wired; an actual shared backing
// would need an IOSurface / MTLSharedTexture winemetal bridge. The earlier
// placeholder returned E_NOTIMPL here, which diverges from the oracle.
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
    // Extended + DEFAULT: Wine proceeds (handle ignored). Create normally.
    return S_OK;
}

// VB/IB shared-handle contract (Wine d3d9 d3d9_device_CreateVertexBuffer /
// CreateIndexBuffer): non-extended -> E_NOTIMPL; extended + non-DEFAULT pool
// -> D3DERR_NOTAVAILABLE; extended + DEFAULT -> FIXME then proceed normally.
[[nodiscard]] static HRESULT validateSharedHandleForBuffer(bool extended,
                                             HANDLE* sharedHandle,
                                             D3DPOOL pool) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool != D3DPOOL_DEFAULT) return D3DERR_NOTAVAILABLE;
    // Extended + DEFAULT: Wine proceeds (handle ignored). Create normally.
    return S_OK;
}

// Offscreen plain surface shared-handle contract (Wine d3d9
// d3d9_device_CreateOffscreenPlainSurface):
//   - SYSTEMMEM -> S_OK; user pointer aliased
//   - SCRATCH   -> D3DERR_INVALIDCALL
//   - DEFAULT   -> FIXME then proceed normally (handle ignored)
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
    // Extended + DEFAULT: Wine proceeds (handle ignored). Create normally.
    return S_OK;
}

// Render-target / depth-stencil surfaces are DEFAULT-pool only. Wine d3d9
// d3d9_device_CreateRenderTarget / CreateDepthStencilSurface: non-extended
// -> E_NOTIMPL; extended -> FIXME then proceed normally (handle ignored).
[[nodiscard]] static HRESULT validateSharedHandleForDefaultSurface(bool extended,
                                                     HANDLE* sharedHandle) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    // Extended: Wine proceeds (handle ignored). Create normally.
    return S_OK;
}

static D9CRect toR(const RECT& r) {
    D9CRect c; c.left = r.left; c.top = r.top;
    c.right = r.right; c.bottom = r.bottom;
    return c;
}

// T4 (D3D9Ex shared-handle, SYSTEMMEM partial): format byte size for the
// formats the SYSTEMMEM user-memory paths actually exercise. The PE side
// is intentionally walled off from dxmt9::core helpers — keeping a tiny
// table here avoids dragging core_format into the PE TU. Returns 0 for
// unknown/unsupported formats; the caller must fall through to the
// normal create path on 0.
static uint32_t userMemoryBytesPerPixel(D3DFORMAT fmt) {
    switch (fmt) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_A2R10G10B10:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_G16R16:
        case D3DFMT_D32:
        case D3DFMT_D24S8:
        case D3DFMT_D24X8:
            return 4;
        case D3DFMT_R8G8B8:
            return 3;
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A8L8:
        case D3DFMT_A8P8:
        case D3DFMT_L16:
        case D3DFMT_D16:
        case D3DFMT_D15S1:
            return 2;
        case D3DFMT_A8:
        case D3DFMT_L8:
        case D3DFMT_R3G3B2:
        case D3DFMT_A4L4:
        case D3DFMT_P8:
            return 1;
        default:
            return 0;
    }
}

/* =========================================================================
 * Shader bytecode + vertex declaration validators.
 *
 * Wine semantics (dlls/d3d9/tests/device.c::test_unsupported_shaders,
 * test_unused_declaration_type, test_vertex_declaration):
 *
 *   - CreateVertexShader / CreatePixelShader reject NULL bytecode and any
 *     header whose top 16 bits do not match the expected stage (0xFFFE for
 *     VS, 0xFFFF for PS), or whose major version exceeds 3 (the highest
 *     real D3D9 shader model). Both Device9 and Device9Ex apply identical
 *     validation.
 *   - CreateVertexDeclaration rejects misaligned offsets (must be a
 *     multiple of 4 for FLOAT/COLOR/UBYTE4 etc.) and any in-band element
 *     with D3DDECLTYPE_UNUSED — only the explicit D3DDECL_END() sentinel
 *     ({stream=0xFF, type=UNUSED}) is allowed to carry UNUSED. Wine
 *     returns E_FAIL for in-band UNUSED (not D3DERR_INVALIDCALL).
 * ========================================================================= */

namespace {

/// D3D9 shader-version token form: ((stageHi << 16) | (major << 8) | minor).
/// stageHi == 0xFFFE for vertex shaders, 0xFFFF for pixel shaders.
constexpr uint32_t kShaderHeaderVS = 0xFFFEu;
constexpr uint32_t kShaderHeaderPS = 0xFFFFu;
constexpr uint32_t kShaderMaxMajor = 3u; /* vs_3_0 / ps_3_0 are the cap */
constexpr uint32_t kShaderEndToken = 0x0000FFFFu;

[[nodiscard]] HRESULT validateShaderBytecodeForStage(const DWORD* code,
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
     * supply. */
    constexpr size_t kBoundedScan = 1u << 16;
    bool seenEnd = false;
    for (size_t i = 1; i < kBoundedScan; ++i) {
        const uint32_t t = static_cast<uint32_t>(code[i]);
        if (t == kShaderEndToken) {
            seenEnd = true;
            break;
        }
        /* Treat any 0xFFFFFFFF (NULL bytecode runaway sentinel some
         * fuzzers use) as truncated. */
        if (t == 0xFFFFFFFFu) {
            return D3DERR_INVALIDCALL;
        }
    }
    if (!seenEnd) return D3DERR_INVALIDCALL;
    return S_OK;
}

/// Returns >0 when the type encodes a known D3DDECLTYPE_* with that byte
/// size, or 0 when the type is unknown / UNUSED.
[[nodiscard]] uint32_t vertexElementTypeSize(uint8_t type) {
    switch (type) {
        case D3DDECLTYPE_FLOAT1:    return 4;
        case D3DDECLTYPE_FLOAT2:    return 8;
        case D3DDECLTYPE_FLOAT3:    return 12;
        case D3DDECLTYPE_FLOAT4:    return 16;
        case D3DDECLTYPE_D3DCOLOR:  return 4;
        case D3DDECLTYPE_UBYTE4:    return 4;
        case D3DDECLTYPE_SHORT2:    return 4;
        case D3DDECLTYPE_SHORT4:    return 8;
        case D3DDECLTYPE_UBYTE4N:   return 4;
        case D3DDECLTYPE_SHORT2N:   return 4;
        case D3DDECLTYPE_SHORT4N:   return 8;
        case D3DDECLTYPE_USHORT2N:  return 4;
        case D3DDECLTYPE_USHORT4N:  return 8;
        case D3DDECLTYPE_UDEC3:     return 4;
        case D3DDECLTYPE_DEC3N:     return 4;
        case D3DDECLTYPE_FLOAT16_2: return 4;
        case D3DDECLTYPE_FLOAT16_4: return 8;
        case D3DDECLTYPE_UNUSED:    return 0;
        default:                    return 0;
    }
}

/// Returns S_OK if a user-supplied D3DVERTEXELEMENT9 array is well-formed:
///   - bounded length (<= MAXD3DDECLLENGTH)
///   - terminated by D3DDECL_END (stream=0xFF, type=UNUSED)
///   - no in-band UNUSED elements (Wine returns E_FAIL for those)
///   - each offset is naturally aligned to the element's word size when the
///     type is FLOAT-like / 32-bit-aligned (multiples of 4). Misaligned
///     offsets are surfaced as E_FAIL per Wine, not D3DERR_INVALIDCALL.
[[nodiscard]] HRESULT validateVertexElements(const D3DVERTEXELEMENT9* elems) {
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

/// Convert a small set of FVF combinations to a Wine-compatible
/// D3DVERTEXELEMENT9 array. Only the subset exercised by Wine's
/// test_fvf_decl_management is required to be lossless; unsupported
/// combinations fall through with an "XYZ FLOAT3 + END" minimum so the
/// resulting decl is still a valid object (which is what Wine produces
/// for arbitrary user FVFs).
inline void fvfToVertexElements(DWORD fvf,
                                std::vector<D3DVERTEXELEMENT9>& out) {
    out.clear();
    uint16_t offset = 0;
    const DWORD posMask = fvf & D3DFVF_POSITION_MASK;
    if (posMask == D3DFVF_XYZ) {
        out.push_back({0, offset, D3DDECLTYPE_FLOAT3,
                       D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_POSITION, 0});
        offset += 12;
    } else if (posMask == D3DFVF_XYZRHW) {
        out.push_back({0, offset, D3DDECLTYPE_FLOAT4,
                       D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_POSITIONT, 0});
        offset += 16;
    } else if (posMask == D3DFVF_XYZW) {
        out.push_back({0, offset, D3DDECLTYPE_FLOAT4,
                       D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_POSITION, 0});
        offset += 16;
    } else if (posMask == D3DFVF_XYZB1 || posMask == D3DFVF_XYZB2 ||
               posMask == D3DFVF_XYZB3 || posMask == D3DFVF_XYZB4 ||
               posMask == D3DFVF_XYZB5) {
        out.push_back({0, offset, D3DDECLTYPE_FLOAT3,
                       D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_POSITION, 0});
        offset += 12;
        const uint32_t blend = (posMask - D3DFVF_XYZB1) / 2u + 1u;
        const bool lastBetaUbyte4 = (fvf & D3DFVF_LASTBETA_UBYTE4) != 0;
        const bool lastBetaD3dcolor = (fvf & D3DFVF_LASTBETA_D3DCOLOR) != 0;
        const uint32_t weightCount =
            (lastBetaUbyte4 || lastBetaD3dcolor) ? blend - 1u : blend;
        if (weightCount >= 1) {
            const uint8_t blendType = weightCount == 1
                ? D3DDECLTYPE_FLOAT1
                : (weightCount == 2 ? D3DDECLTYPE_FLOAT2
                                    : (weightCount == 3 ? D3DDECLTYPE_FLOAT3
                                                        : D3DDECLTYPE_FLOAT4));
            out.push_back({0, offset, blendType,
                           D3DDECLMETHOD_DEFAULT,
                           D3DDECLUSAGE_BLENDWEIGHT, 0});
            offset += vertexElementTypeSize(blendType);
        }
        if (lastBetaUbyte4 || lastBetaD3dcolor) {
            out.push_back({0, offset,
                           static_cast<uint8_t>(lastBetaD3dcolor
                               ? D3DDECLTYPE_D3DCOLOR : D3DDECLTYPE_UBYTE4),
                           D3DDECLMETHOD_DEFAULT,
                           D3DDECLUSAGE_BLENDINDICES, 0});
            offset += 4;
        } else if (blend > 4u) {
            offset += 4;
        }
    }
    if (fvf & D3DFVF_NORMAL) {
        out.push_back({0, offset, D3DDECLTYPE_FLOAT3,
                       D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_NORMAL, 0});
        offset += 12;
    }
    if (fvf & D3DFVF_PSIZE) {
        out.push_back({0, offset, D3DDECLTYPE_FLOAT1,
                       D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_PSIZE, 0});
        offset += 4;
    }
    if (fvf & D3DFVF_DIFFUSE) {
        out.push_back({0, offset, D3DDECLTYPE_D3DCOLOR,
                       D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_COLOR, 0});
        offset += 4;
    }
    if (fvf & D3DFVF_SPECULAR) {
        out.push_back({0, offset, D3DDECLTYPE_D3DCOLOR,
                       D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_COLOR, 1});
        offset += 4;
    }
    const uint32_t texCount = (fvf & D3DFVF_TEXCOUNT_MASK)
                              >> D3DFVF_TEXCOUNT_SHIFT;
    for (uint32_t i = 0; i < texCount && i < 8; ++i) {
        const uint32_t shift = i * 2u + 16u;
        const uint32_t sizeBits = (fvf >> shift) & 0x3u;
        /* TEXTUREFORMAT mapping:
         *   FORMAT2 (0) -> FLOAT2, FORMAT3 (1) -> FLOAT3,
         *   FORMAT4 (2) -> FLOAT4, FORMAT1 (3) -> FLOAT1. */
        uint8_t type = D3DDECLTYPE_FLOAT2;
        uint16_t size = 8;
        if (sizeBits == 1) { type = D3DDECLTYPE_FLOAT3; size = 12; }
        else if (sizeBits == 2) { type = D3DDECLTYPE_FLOAT4; size = 16; }
        else if (sizeBits == 3) { type = D3DDECLTYPE_FLOAT1; size = 4; }
        out.push_back({0, offset, type, D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_TEXCOORD,
                       static_cast<uint8_t>(i)});
        offset += size;
    }
    /* If no recognized position bit was set, emit a minimum (no-op) decl
     * so the resulting object is still well-formed. */
    if (out.empty()) {
        out.push_back({0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_POSITION, 0});
    }
    out.push_back({0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0});
}

struct FvfProcessLayout {
    UINT stride = 0;
    UINT streamStride[16]{};
    UINT positionStream = 0;
    UINT positionOffset = 0;
    UINT positionBytes = 0;
    UINT normalStream = 0;
    UINT normalOffset = 0;
    UINT normalType = D3DDECLTYPE_FLOAT3;
    UINT normalBytes = 12u;
    UINT tangentStream = 0;
    UINT tangentOffset = 0;
    UINT tangentType = D3DDECLTYPE_FLOAT3;
    UINT tangentBytes = 12u;
    UINT binormalStream = 0;
    UINT binormalOffset = 0;
    UINT binormalType = D3DDECLTYPE_FLOAT3;
    UINT binormalBytes = 12u;
    UINT blendWeightStream = 0;
    UINT blendWeightOffset = 0;
    UINT blendWeightBytes = 0;
    UINT blendIndicesStream = 0;
    UINT blendIndicesOffset = 0;
    UINT blendIndicesBytes = 0;
    UINT psizeStream = 0;
    UINT psizeOffset = 0;
    UINT diffuseStream = 0;
    UINT diffuseOffset = 0;
    UINT specularStream = 0;
    UINT specularOffset = 0;
    UINT texStream[8]{};
    UINT texOffset[8]{};
    UINT texBytes[8]{};
    UINT texType[8]{};
    UINT texCount = 0;
    struct GenericInput {
        UINT usage = 0;
        UINT usageIndex = 0;
        UINT stream = 0;
        UINT offset = 0;
        UINT type = D3DDECLTYPE_FLOAT4;
        UINT bytes = 0;
    };
    GenericInput genericInput[16]{};
    UINT genericInputCount = 0;
    bool normal = false;
    bool tangent = false;
    bool binormal = false;
    bool blendWeight = false;
    bool blendIndices = false;
    bool psize = false;
    bool diffuse = false;
    bool specular = false;
};

struct ProcessShaderReg {
    UINT type = 0;
    UINT index = 0;
};

struct ProcessShaderIo {
    int inputPosition = -1;
    int inputNormal = -1;
    int inputTangent = -1;
    int inputBinormal = -1;
    int inputBlendWeight = -1;
    int inputBlendIndices = -1;
    int inputPSize = -1;
    int inputDiffuse = -1;
    int inputSpecular = -1;
    int inputTex[8]{-1, -1, -1, -1, -1, -1, -1, -1};
    struct GenericInput {
        UINT usage = 0;
        UINT usageIndex = 0;
        int reg = -1;
    };
    GenericInput inputGeneric[16]{};
    UINT inputGenericCount = 0;
    bool inputGenericOverflow = false;
    ProcessShaderReg outputPosition{};
    ProcessShaderReg outputPSize{};
    ProcessShaderReg outputDiffuse{};
    ProcessShaderReg outputSpecular{};
    ProcessShaderReg outputTex[8]{};
    bool hasOutputPosition = false;
    bool hasOutputPSize = false;
    bool hasOutputDiffuse = false;
    bool hasOutputSpecular = false;
    bool hasOutputTex[8]{};
    UINT major = 0;
};

static UINT fvfTexcoordBytes(DWORD fvf, UINT index) {
    const DWORD sizeBits = (fvf >> (index * 2u + 16u)) & 0x3u;
    if (sizeBits == 1u) return 12u;
    if (sizeBits == 2u) return 16u;
    if (sizeBits == 3u) return 4u;
    return 8u;
}

static bool processFvfXyzbPosition(DWORD positionMask) {
    return positionMask == D3DFVF_XYZB1 ||
           positionMask == D3DFVF_XYZB2 ||
           positionMask == D3DFVF_XYZB3 ||
           positionMask == D3DFVF_XYZB4 ||
           positionMask == D3DFVF_XYZB5;
}

static UINT processTexDeclBytes(UINT type, bool destination) {
    switch (type) {
        case D3DDECLTYPE_FLOAT1:
            return 4u;
        case D3DDECLTYPE_FLOAT2:
            return 8u;
        case D3DDECLTYPE_FLOAT3:
            return 12u;
        case D3DDECLTYPE_FLOAT4:
            return 16u;
        case D3DDECLTYPE_UBYTE4:
        case D3DDECLTYPE_SHORT2:
        case D3DDECLTYPE_UBYTE4N:
        case D3DDECLTYPE_UDEC3:
        case D3DDECLTYPE_SHORT2N:
        case D3DDECLTYPE_USHORT2N:
        case D3DDECLTYPE_FLOAT16_2:
            return destination ? 0u : 4u;
        case D3DDECLTYPE_SHORT4:
        case D3DDECLTYPE_SHORT4N:
        case D3DDECLTYPE_USHORT4N:
        case D3DDECLTYPE_FLOAT16_4:
            return destination ? 0u : 8u;
        default:
            return 0u;
    }
}

static UINT processFloatVectorDeclBytes(UINT type, bool allowTwoComponent) {
    switch (type) {
        case D3DDECLTYPE_FLOAT1:
            return allowTwoComponent ? 4u : 0u;
        case D3DDECLTYPE_FLOAT2:
            return allowTwoComponent ? 8u : 0u;
        case D3DDECLTYPE_FLOAT3:
            return 12u;
        case D3DDECLTYPE_FLOAT4:
            return 16u;
        case D3DDECLTYPE_UBYTE4:
            return 4u;
        case D3DDECLTYPE_SHORT4:
            return 8u;
        case D3DDECLTYPE_DEC3N:
        case D3DDECLTYPE_UDEC3:
        case D3DDECLTYPE_UBYTE4N:
            return 4u;
        case D3DDECLTYPE_SHORT2N:
        case D3DDECLTYPE_USHORT2N:
        case D3DDECLTYPE_FLOAT16_2:
            return allowTwoComponent ? 4u : 0u;
        case D3DDECLTYPE_SHORT4N:
        case D3DDECLTYPE_USHORT4N:
        case D3DDECLTYPE_FLOAT16_4:
            return 8u;
        default:
            return 0u;
    }
}

static bool describeProcessFvf(DWORD fvf, FvfProcessLayout& layout) {
    layout = {};
    switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZ:
            layout.positionOffset = 0u;
            layout.positionBytes = 12u;
            break;
        case D3DFVF_XYZB1:
        case D3DFVF_XYZB2:
        case D3DFVF_XYZB3:
        case D3DFVF_XYZB4:
        case D3DFVF_XYZB5:
            layout.positionOffset = 0u;
            layout.positionBytes = 12u;
            break;
        case D3DFVF_XYZRHW:
        case D3DFVF_XYZW:
            layout.positionOffset = 0u;
            layout.positionBytes = 16u;
            break;
        default:
            return false;
    }
    UINT offset = layout.positionBytes;
    if (processFvfXyzbPosition(fvf & D3DFVF_POSITION_MASK)) {
        const UINT betaCount =
            ((fvf & D3DFVF_POSITION_MASK) - D3DFVF_XYZB1) / 2u + 1u;
        const bool lastBetaUbyte4 = (fvf & D3DFVF_LASTBETA_UBYTE4) != 0;
        const bool lastBetaD3dcolor = (fvf & D3DFVF_LASTBETA_D3DCOLOR) != 0;
        if (lastBetaUbyte4 && lastBetaD3dcolor) return false;
        if (lastBetaUbyte4 || lastBetaD3dcolor) {
            const UINT weightCount = betaCount - 1u;
            if (weightCount > 4u) return false;
            if (weightCount != 0u) {
                layout.blendWeight = true;
                layout.blendWeightOffset = offset;
                layout.blendWeightBytes = weightCount * sizeof(float);
                offset += layout.blendWeightBytes;
            }
            layout.blendIndices = true;
            layout.blendIndicesOffset = offset;
            layout.blendIndicesBytes = 4u;
            offset += 4u;
        } else {
            if (betaCount > 4u) return false;
            layout.blendWeight = true;
            layout.blendWeightOffset = offset;
            layout.blendWeightBytes = betaCount * sizeof(float);
            offset += layout.blendWeightBytes;
        }
    }
    if (fvf & D3DFVF_NORMAL) {
        layout.normal = true;
        layout.normalOffset = offset;
        layout.normalType = D3DDECLTYPE_FLOAT3;
        layout.normalBytes = 12u;
        offset += 12u;
    }
    if (fvf & D3DFVF_PSIZE) {
        layout.psize = true;
        layout.psizeOffset = offset;
        offset += 4u;
    }
    if (fvf & D3DFVF_DIFFUSE) {
        layout.diffuse = true;
        layout.diffuseOffset = offset;
        offset += 4u;
    }
    if (fvf & D3DFVF_SPECULAR) {
        layout.specular = true;
        layout.specularOffset = offset;
        offset += 4u;
    }
    layout.texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (layout.texCount > 8u) return false;
    for (UINT i = 0; i < layout.texCount; ++i) {
        layout.texOffset[i] = offset;
        layout.texBytes[i] = fvfTexcoordBytes(fvf, i);
        layout.texType[i] = layout.texBytes[i] == 4u ? D3DDECLTYPE_FLOAT1
                          : layout.texBytes[i] == 12u ? D3DDECLTYPE_FLOAT3
                          : layout.texBytes[i] == 16u ? D3DDECLTYPE_FLOAT4
                          : D3DDECLTYPE_FLOAT2;
        offset += layout.texBytes[i];
    }
    layout.stride = offset;
    layout.streamStride[0] = offset;
    return layout.stride != 0u;
}

static bool describeProcessDeclaration(IDirect3DVertexDeclaration9* declaration,
                                       FvfProcessLayout& layout,
                                       bool destination) {
    layout = {};
    if (!declaration) return false;
    D9CVertexElement elements[MAXD3DDECLLENGTH + 1]{};
    uint32_t count = MAXD3DDECLLENGTH + 1;
    if (FAILED(hr32(dxmt9c_vdecl_get_declaration(
            D3D9PeRawVertexDecl(declaration), elements, &count)))) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const D9CVertexElement& e = elements[i];
        if (e.stream == 0xff && e.type == D3DDECLTYPE_UNUSED) {
            break;
        }
        if ((destination && e.stream != 0) ||
            (!destination && e.stream >= D9C_DRAW_PACKET_MAX_STREAMS) ||
            e.method != D3DDECLMETHOD_DEFAULT) {
            return false;
        }
        const UINT elementBytes = vertexElementTypeSize(e.type);
        if (elementBytes == 0u) return false;
        layout.stride = std::max<UINT>(layout.stride, e.offset + elementBytes);
        layout.streamStride[e.stream] =
            std::max<UINT>(layout.streamStride[e.stream], e.offset + elementBytes);
        const bool expectedPosition =
            e.usageIndex == 0 &&
            ((destination && e.usage == D3DDECLUSAGE_POSITIONT) ||
             (!destination && e.usage == D3DDECLUSAGE_POSITION));
        if (expectedPosition) {
            if (layout.positionBytes != 0u) return false;
            if (destination) {
                if (e.type != D3DDECLTYPE_FLOAT4) return false;
                layout.positionBytes = 16u;
            } else {
                if (e.type == D3DDECLTYPE_FLOAT3) {
                    layout.positionBytes = 12u;
                } else if (e.type == D3DDECLTYPE_FLOAT4) {
                    layout.positionBytes = 16u;
                } else {
                    return false;
                }
            }
            layout.positionStream = e.stream;
            layout.positionOffset = e.offset;
        } else if (e.usage == D3DDECLUSAGE_COLOR && e.usageIndex == 0) {
            if (e.type != D3DDECLTYPE_D3DCOLOR || layout.diffuse) return false;
            layout.diffuse = true;
            layout.diffuseStream = e.stream;
            layout.diffuseOffset = e.offset;
        } else if (!destination && e.usage == D3DDECLUSAGE_NORMAL &&
                   e.usageIndex == 0) {
            const UINT bytes = processFloatVectorDeclBytes(e.type, false);
            if (bytes == 0u || layout.normal) return false;
            layout.normal = true;
            layout.normalStream = e.stream;
            layout.normalOffset = e.offset;
            layout.normalType = e.type;
            layout.normalBytes = bytes;
        } else if (!destination && e.usage == D3DDECLUSAGE_TANGENT &&
                   e.usageIndex == 0) {
            const UINT bytes = processFloatVectorDeclBytes(e.type, false);
            if (bytes == 0u || layout.tangent) return false;
            layout.tangent = true;
            layout.tangentStream = e.stream;
            layout.tangentOffset = e.offset;
            layout.tangentType = e.type;
            layout.tangentBytes = bytes;
        } else if (!destination && e.usage == D3DDECLUSAGE_BINORMAL &&
                   e.usageIndex == 0) {
            const UINT bytes = processFloatVectorDeclBytes(e.type, false);
            if (bytes == 0u || layout.binormal) return false;
            layout.binormal = true;
            layout.binormalStream = e.stream;
            layout.binormalOffset = e.offset;
            layout.binormalType = e.type;
            layout.binormalBytes = bytes;
        } else if (!destination && e.usage == D3DDECLUSAGE_BLENDWEIGHT &&
                   e.usageIndex == 0) {
            UINT blendBytes = 0u;
            if (e.type == D3DDECLTYPE_FLOAT1) blendBytes = 4u;
            else if (e.type == D3DDECLTYPE_FLOAT2) blendBytes = 8u;
            else if (e.type == D3DDECLTYPE_FLOAT3) blendBytes = 12u;
            else if (e.type == D3DDECLTYPE_FLOAT4) blendBytes = 16u;
            else return false;
            if (layout.blendWeight) return false;
            layout.blendWeight = true;
            layout.blendWeightStream = e.stream;
            layout.blendWeightOffset = e.offset;
            layout.blendWeightBytes = blendBytes;
        } else if (!destination && e.usage == D3DDECLUSAGE_BLENDINDICES &&
                   e.usageIndex == 0) {
            if (e.type != D3DDECLTYPE_UBYTE4 || layout.blendIndices) return false;
            layout.blendIndices = true;
            layout.blendIndicesStream = e.stream;
            layout.blendIndicesOffset = e.offset;
            layout.blendIndicesBytes = 4u;
        } else if (!destination && e.usage == D3DDECLUSAGE_PSIZE &&
                   e.usageIndex == 0) {
            if (e.type != D3DDECLTYPE_FLOAT1 || layout.psize) return false;
            layout.psize = true;
            layout.psizeStream = e.stream;
            layout.psizeOffset = e.offset;
        } else if (destination && e.usage == D3DDECLUSAGE_PSIZE &&
                   e.usageIndex == 0) {
            if (e.type != D3DDECLTYPE_FLOAT1 || layout.psize) return false;
            layout.psize = true;
            layout.psizeStream = e.stream;
            layout.psizeOffset = e.offset;
        } else if (e.usage == D3DDECLUSAGE_COLOR && e.usageIndex == 1) {
            if (e.type != D3DDECLTYPE_D3DCOLOR || layout.specular) return false;
            layout.specular = true;
            layout.specularStream = e.stream;
            layout.specularOffset = e.offset;
        } else if (e.usage == D3DDECLUSAGE_TEXCOORD && e.usageIndex < 8) {
            const UINT texBytes = processTexDeclBytes(e.type, destination);
            if (texBytes == 0u) return false;
            layout.texCount = std::max<UINT>(layout.texCount, e.usageIndex + 1u);
            layout.texStream[e.usageIndex] = e.stream;
            layout.texOffset[e.usageIndex] = e.offset;
            layout.texBytes[e.usageIndex] = texBytes;
            layout.texType[e.usageIndex] = e.type;
        } else {
            if (destination) return false;
            const UINT genericBytes = processFloatVectorDeclBytes(e.type, true);
            if (genericBytes == 0u ||
                layout.genericInputCount >= std::size(layout.genericInput)) {
                return false;
            }
            for (UINT generic = 0; generic < layout.genericInputCount; ++generic) {
                if (layout.genericInput[generic].usage == e.usage &&
                    layout.genericInput[generic].usageIndex == e.usageIndex) {
                    return false;
                }
            }
            layout.genericInput[layout.genericInputCount++] = {
                e.usage,
                e.usageIndex,
                e.stream,
                e.offset,
                e.type,
                genericBytes,
            };
        }
    }
    return (destination ? layout.positionBytes == 16u
                        : (layout.positionBytes == 12u ||
                           layout.positionBytes == 16u)) &&
           layout.stride != 0u;
}

static UINT shaderRegType(DWORD token) {
    return ((token >> D3DSP_REGTYPE_SHIFT) & 0x7u) |
           (((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2) << 3u);
}

static UINT shaderRegIndex(DWORD token) {
    return token & D3DSP_REGNUM_MASK;
}

static UINT shaderWriteMask(DWORD token) {
    return (token & D3DSP_WRITEMASK_ALL) >> 16u;
}

static UINT shaderSwizzle(DWORD token) {
    return (token & D3DSP_SWIZZLE_MASK) >> D3DSP_SWIZZLE_SHIFT;
}

static UINT simpleProcessShaderOperandCount(UINT opcode, DWORD token) {
    switch (opcode) {
        case D3DSIO_NOP:
        case D3DSIO_RET:
        case D3DSIO_PHASE:
        case D3DSIO_ELSE:
        case D3DSIO_ENDIF:
        case D3DSIO_ENDLOOP:
        case D3DSIO_ENDREP:
        case D3DSIO_BREAK:
            return 0;
        case D3DSIO_MOV:
        case D3DSIO_MOVA:
        case D3DSIO_RCP:
        case D3DSIO_RSQ:
        case D3DSIO_FRC:
        case D3DSIO_ABS:
        case D3DSIO_EXP:
        case D3DSIO_LOG:
        case D3DSIO_LIT:
        case D3DSIO_EXPP:
        case D3DSIO_LOGP:
        case D3DSIO_SGN:
        case D3DSIO_SINCOS:
        case D3DSIO_NRM:
        case D3DSIO_SETP:
        case D3DSIO_BREAKP:
            return 2;
        case D3DSIO_MAD:
        case D3DSIO_LRP:
            return 4;
        case D3DSIO_ADD:
        case D3DSIO_SUB:
        case D3DSIO_MUL:
        case D3DSIO_DP3:
        case D3DSIO_DP4:
        case D3DSIO_SLT:
        case D3DSIO_SGE:
        case D3DSIO_MIN:
        case D3DSIO_MAX:
        case D3DSIO_POW:
        case D3DSIO_CRS:
        case D3DSIO_DST:
        case D3DSIO_M4x4:
        case D3DSIO_M4x3:
        case D3DSIO_M3x4:
        case D3DSIO_M3x3:
        case D3DSIO_M3x2:
            return 3;
        case D3DSIO_DCL:
            return 2;
        case D3DSIO_IF:
        case D3DSIO_REP:
        case D3DSIO_LABEL:
        case D3DSIO_CALL:
            return 1;
        case D3DSIO_IFC:
        case D3DSIO_BREAKC:
        case D3DSIO_CALLNZ:
            return 2;
        case D3DSIO_LOOP:
            return (token >> D3DSI_INSTLENGTH_SHIFT) & 0xfu;
        case D3DSIO_DEF:
        case D3DSIO_DEFI:
            return 5;
        default:
            return (token >> D3DSI_INSTLENGTH_SHIFT) & 0xfu;
    }
}

struct SimpleProcessShaderOperands {
    UINT count = 0;
    std::array<DWORD, 8> operands{};
    std::array<DWORD, 8> relAddrOperands{};
};

static bool simpleProcessShaderOperandCarriesRelAddr(UINT opcode, UINT operandIndex) {
    switch (opcode) {
        case D3DSIO_DEF:
        case D3DSIO_DEFI:
            return operandIndex == 0u;
        case D3DSIO_DCL:
            return operandIndex == 1u;
        case D3DSIO_LABEL:
        case D3DSIO_CALL:
            return false;
        case D3DSIO_CALLNZ:
            return operandIndex == 1u;
        default:
            return true;
    }
}

static bool simpleProcessShaderTokenHasRelAddr(DWORD token) {
    return (token & D3DSHADER_ADDRESSMODE_MASK) != 0u;
}

static bool simpleProcessShaderReadOperands(const std::vector<DWORD>& words,
                                            size_t& index,
                                            UINT opcode,
                                            DWORD token,
                                            SimpleProcessShaderOperands& out) {
    out = {};
    out.count = simpleProcessShaderOperandCount(opcode, token);
    if (out.count > out.operands.size()) return false;
    if (out.count > words.size() - index) return false;
    for (UINT i = 0; i < out.count; ++i) {
        const DWORD operand = words[index++];
        out.operands[i] = operand;
        if (!simpleProcessShaderOperandCarriesRelAddr(opcode, i) ||
            !simpleProcessShaderTokenHasRelAddr(operand)) {
            continue;
        }
        if (index >= words.size()) return false;
        out.relAddrOperands[i] = words[index++];
    }
    return true;
}

static bool shaderSkipComment(const std::vector<DWORD>& words, size_t& index,
                              DWORD token) {
    const size_t commentWords = (token >> 16u) & 0x7fffu;
    if (commentWords > words.size() - index) return false;
    index += commentWords;
    return true;
}

static void noteProcessShaderInput(ProcessShaderIo& io, UINT usage,
                                   UINT usageIndex, UINT reg) {
    if (usage == D3DDECLUSAGE_POSITION && usageIndex == 0) {
        io.inputPosition = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_NORMAL && usageIndex == 0) {
        io.inputNormal = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_TANGENT && usageIndex == 0) {
        io.inputTangent = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_BINORMAL && usageIndex == 0) {
        io.inputBinormal = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_BLENDWEIGHT && usageIndex == 0) {
        io.inputBlendWeight = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_BLENDINDICES && usageIndex == 0) {
        io.inputBlendIndices = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_PSIZE && usageIndex == 0) {
        io.inputPSize = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_COLOR && usageIndex == 0) {
        io.inputDiffuse = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_COLOR && usageIndex == 1) {
        io.inputSpecular = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_TEXCOORD && usageIndex < 8) {
        io.inputTex[usageIndex] = static_cast<int>(reg);
    } else if (io.inputGenericCount < std::size(io.inputGeneric)) {
        io.inputGeneric[io.inputGenericCount++] = {
            usage,
            usageIndex,
            static_cast<int>(reg),
        };
    } else {
        io.inputGenericOverflow = true;
    }
}

static void noteProcessShaderOutput(ProcessShaderIo& io, UINT usage,
                                    UINT usageIndex, ProcessShaderReg reg) {
    if (usage == D3DDECLUSAGE_POSITION && usageIndex == 0) {
        io.outputPosition = reg;
        io.hasOutputPosition = true;
    } else if (usage == D3DDECLUSAGE_PSIZE && usageIndex == 0) {
        io.outputPSize = reg;
        io.hasOutputPSize = true;
    } else if (usage == D3DDECLUSAGE_COLOR && usageIndex == 0) {
        io.outputDiffuse = reg;
        io.hasOutputDiffuse = true;
    } else if (usage == D3DDECLUSAGE_COLOR && usageIndex == 1) {
        io.outputSpecular = reg;
        io.hasOutputSpecular = true;
    } else if (usage == D3DDECLUSAGE_TEXCOORD && usageIndex < 8) {
        io.outputTex[usageIndex] = reg;
        io.hasOutputTex[usageIndex] = true;
    }
}

static bool analyzeSimpleProcessVertexShader(const std::vector<DWORD>& words,
                                             ProcessShaderIo& io) {
    io = {};
    for (int& tex : io.inputTex) tex = -1;
    if (words.empty() || (words[0] >> 16u) != kShaderHeaderVS) return false;
    io.major = (words[0] >> 8u) & 0xffu;
    if (io.major < 3u) {
        io.inputPosition = 0;
        io.inputDiffuse = 5;
        io.inputSpecular = 6;
        for (UINT i = 0; i < 8; ++i) io.inputTex[i] = static_cast<int>(7u + i);
        io.outputPosition = {D3DSPR_RASTOUT, D3DSRO_POSITION};
        io.outputDiffuse = {D3DSPR_ATTROUT, 0};
        io.outputSpecular = {D3DSPR_ATTROUT, 1};
        io.hasOutputPosition = true;
        io.hasOutputDiffuse = true;
        io.hasOutputSpecular = true;
        for (UINT i = 0; i < 8; ++i) {
            io.outputTex[i] = {D3DSPR_TEXCRDOUT, i};
            io.hasOutputTex[i] = true;
        }
    }

    for (size_t index = 1; index < words.size();) {
        const DWORD token = words[index++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) {
            return io.hasOutputPosition && !io.inputGenericOverflow;
        }
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, index, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, index, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        const DWORD* operands = parsedOperands.operands.data();
        if (opcode != D3DSIO_DCL) continue;
        const UINT usage = (operands[0] & D3DSP_DCL_USAGE_MASK) >> D3DSP_DCL_USAGE_SHIFT;
        const UINT usageIndex =
            (operands[0] & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT;
        const ProcessShaderReg reg{shaderRegType(operands[1]),
                                   shaderRegIndex(operands[1])};
        if (reg.type == D3DSPR_INPUT) {
            noteProcessShaderInput(io, usage, usageIndex, reg.index);
        } else if (reg.type == D3DSPR_OUTPUT || reg.type == D3DSPR_TEXCRDOUT) {
            noteProcessShaderOutput(io, usage, usageIndex, reg);
        } else if (reg.type == D3DSPR_RASTOUT || reg.type == D3DSPR_ATTROUT) {
            noteProcessShaderOutput(io, usage, usageIndex, reg);
        }
    }
    return false;
}

static float clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static uint8_t floatColorByte(float value) {
    return static_cast<uint8_t>(clamp01(value) * 255.0f + 0.5f);
}

static void unpackD3DColor(DWORD color, float out[4]) {
    out[0] = static_cast<float>((color >> 16u) & 0xffu) / 255.0f;
    out[1] = static_cast<float>((color >> 8u) & 0xffu) / 255.0f;
    out[2] = static_cast<float>(color & 0xffu) / 255.0f;
    out[3] = static_cast<float>((color >> 24u) & 0xffu) / 255.0f;
}

static DWORD packD3DColor(const float in[4]) {
    return (static_cast<DWORD>(floatColorByte(in[3])) << 24u) |
           (static_cast<DWORD>(floatColorByte(in[0])) << 16u) |
           (static_cast<DWORD>(floatColorByte(in[1])) << 8u) |
           static_cast<DWORD>(floatColorByte(in[2]));
}

static D9CColorRGBA d3dColorToRgba(DWORD color) {
    float rgba[4]{};
    unpackD3DColor(color, rgba);
    return {rgba[0], rgba[1], rgba[2], rgba[3]};
}

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool normalize3(float v[3]) {
    const float lenSq = dot3(v, v);
    if (lenSq <= 0.0f) return false;
    const float invLen = 1.0f / std::sqrt(lenSq);
    v[0] *= invLen;
    v[1] *= invLen;
    v[2] *= invLen;
    return true;
}

static void addColorProduct(float out[4], const D9CColorRGBA& a,
                            const D9CColorRGBA& b, float scale) {
    out[0] += a.r * b.r * scale;
    out[1] += a.g * b.g * scale;
    out[2] += a.b * b.b * scale;
}

struct ProcessFixedFunctionLightingColors {
    DWORD diffuse;
    DWORD specular;
};

static ProcessFixedFunctionLightingColors processFixedFunctionLightingColors(
    const float position[3],
    const float normalIn[3],
    const D9CMaterial& material,
    DWORD ambient,
    const D9CLight lights[8],
    DWORD lightEnableMask,
    bool specularEnabled) {
    float normal[3]{normalIn[0], normalIn[1], normalIn[2]};
    normalize3(normal);

    float ambientColor[4]{};
    unpackD3DColor(ambient, ambientColor);
    float lit[4]{
        material.emissive.r + material.ambient.r * ambientColor[0],
        material.emissive.g + material.ambient.g * ambientColor[1],
        material.emissive.b + material.ambient.b * ambientColor[2],
        material.diffuse.a,
    };
    float specular[4]{0.0f, 0.0f, 0.0f, 0.0f};

    for (UINT i = 0; i < 8u; ++i) {
        if ((lightEnableMask & (1u << i)) == 0) continue;
        const D9CLight& light = lights[i];

        float toLight[3]{};
        float attenuation = 1.0f;
        bool directional = false;
        if (light.type == D3DLIGHT_DIRECTIONAL) {
            directional = true;
            toLight[0] = -light.direction[0];
            toLight[1] = -light.direction[1];
            toLight[2] = -light.direction[2];
            if (!normalize3(toLight)) continue;
        } else if (light.type == D3DLIGHT_POINT || light.type == D3DLIGHT_SPOT) {
            toLight[0] = light.position[0] - position[0];
            toLight[1] = light.position[1] - position[1];
            toLight[2] = light.position[2] - position[2];
            const float distanceSq = dot3(toLight, toLight);
            if (distanceSq <= 0.0f) continue;
            const float distance = std::sqrt(distanceSq);
            if (light.range > 0.0f && distance > light.range) continue;
            const float denom = light.attenuation0 +
                                light.attenuation1 * distance +
                                light.attenuation2 * distanceSq;
            if (denom > 0.0f) attenuation = clamp01(1.0f / denom);
            toLight[0] /= distance;
            toLight[1] /= distance;
            toLight[2] /= distance;
            if (light.type == D3DLIGHT_SPOT) {
                float spotDirection[3]{
                    -light.direction[0],
                    -light.direction[1],
                    -light.direction[2],
                };
                if (!normalize3(spotDirection)) continue;
                const float rho = dot3(spotDirection, toLight);
                const float cosInner = std::cos(0.5f * light.theta);
                const float cosOuter = std::cos(0.5f * light.phi);
                float spotFactor = 0.0f;
                if (rho >= cosInner) {
                    spotFactor = 1.0f;
                } else if (rho > cosOuter) {
                    const float denom = std::max(cosInner - cosOuter, 1.0e-6f);
                    const float cone = clamp01((rho - cosOuter) / denom);
                    spotFactor = std::pow(cone, std::max(light.falloff, 0.0f));
                }
                attenuation *= spotFactor;
            }
        } else {
            continue;
        }
        addColorProduct(lit, material.ambient, light.ambient,
                        directional ? 1.0f : attenuation);
        const float ndotl = std::max(0.0f, dot3(normal, toLight));
        const float diffuse = ndotl * attenuation;
        addColorProduct(lit, material.diffuse, light.diffuse, diffuse);
        if (specularEnabled && ndotl > 0.0f) {
            float halfVec[3]{toLight[0], toLight[1], toLight[2] + 1.0f};
            if (normalize3(halfVec)) {
                const float shininess = std::max(material.power, 1.0f);
                const float factor = std::pow(std::max(0.0f, dot3(normal, halfVec)),
                                              shininess) * attenuation;
                addColorProduct(specular, material.specular, light.specular, factor);
            }
        }
    }
    return {packD3DColor(lit), packD3DColor(specular)};
}

static float snorm16ToFloat(int16_t value) {
    if (value <= -32768) return -1.0f;
    return static_cast<float>(value) / 32767.0f;
}

static float unorm16ToFloat(uint16_t value) {
    return static_cast<float>(value) / 65535.0f;
}

static float snorm10ToFloat(uint32_t value) {
    int32_t signedValue = static_cast<int32_t>(value & 0x3ffu);
    if (signedValue & 0x200) signedValue |= ~0x3ff;
    if (signedValue <= -512) return -1.0f;
    return static_cast<float>(signedValue) / 511.0f;
}

static float halfToFloat(uint16_t value) {
    const uint32_t sign = value & 0x8000u;
    const uint32_t exponent = (value >> 10u) & 0x1fu;
    const uint32_t mantissa = value & 0x3ffu;
    float result = 0.0f;

    if (exponent == 0u) {
        result = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 0x1fu) {
        result = mantissa == 0u
               ? std::numeric_limits<float>::infinity()
               : std::numeric_limits<float>::quiet_NaN();
    } else {
        result = std::ldexp(static_cast<float>(1024u + mantissa),
                            static_cast<int>(exponent) - 25);
    }
    return sign ? -result : result;
}

struct SimpleVsRegisters {
    std::array<std::array<float, 4>, 32> temp{};
    std::array<std::array<float, 4>, 16> input{};
    std::array<std::array<float, 4>, 256> constant{};
    std::array<std::array<int32_t, 4>, 16> constantInt{};
    std::array<std::array<float, 4>, 16> predicate{};
    std::array<float, 4> address{};
    std::array<float, 4> loop{};
    std::array<std::array<float, 4>, 16> output{};
    std::array<std::array<float, 4>, 3> rastOut{};
    std::array<std::array<float, 4>, 2> attrOut{};
    std::array<std::array<float, 4>, 8> texOut{};
};

struct SimpleVsTextureState {
    std::array<D9CTexture*, kPeVertexTextureSamplerSlots> vertexTextures{};
    std::array<DWORD, kPeVertexTextureSamplerSlots> addressU{};
    std::array<DWORD, kPeVertexTextureSamplerSlots> addressV{};
    std::array<DWORD, kPeVertexTextureSamplerSlots> borderColor{};
    std::array<DWORD, kPeVertexTextureSamplerSlots> minMipLevel{};
};

static std::array<float, 4>* simpleVsRegister(SimpleVsRegisters& regs,
                                              UINT major,
                                              UINT type,
                                              UINT index) {
    switch (type) {
        case D3DSPR_TEMP:
            return index < regs.temp.size() ? &regs.temp[index] : nullptr;
        case D3DSPR_INPUT:
            return index < regs.input.size() ? &regs.input[index] : nullptr;
        case D3DSPR_CONST:
            return index < regs.constant.size() ? &regs.constant[index] : nullptr;
        case D3DSPR_ADDR:
            return index == 0u ? &regs.address : nullptr;
        case D3DSPR_RASTOUT:
            return index < regs.rastOut.size() ? &regs.rastOut[index] : nullptr;
        case D3DSPR_ATTROUT:
            return index < regs.attrOut.size() ? &regs.attrOut[index] : nullptr;
        case D3DSPR_TEXCRDOUT:
            if (major >= 3u) {
                return index < regs.output.size() ? &regs.output[index] : nullptr;
            }
            return index < regs.texOut.size() ? &regs.texOut[index] : nullptr;
        case D3DSPR_PREDICATE:
            return index < regs.predicate.size() ? &regs.predicate[index] : nullptr;
        case D3DSPR_LOOP:
            return index == 0u ? &regs.loop : nullptr;
        default:
            return nullptr;
    }
}

static const std::array<float, 4>* simpleVsRegisterConst(
        const SimpleVsRegisters& regs, UINT major, UINT type, UINT index) {
    switch (type) {
        case D3DSPR_TEMP:
            return index < regs.temp.size() ? &regs.temp[index] : nullptr;
        case D3DSPR_INPUT:
            return index < regs.input.size() ? &regs.input[index] : nullptr;
        case D3DSPR_CONST:
            return index < regs.constant.size() ? &regs.constant[index] : nullptr;
        case D3DSPR_ADDR:
            return index == 0u ? &regs.address : nullptr;
        case D3DSPR_RASTOUT:
            return index < regs.rastOut.size() ? &regs.rastOut[index] : nullptr;
        case D3DSPR_ATTROUT:
            return index < regs.attrOut.size() ? &regs.attrOut[index] : nullptr;
        case D3DSPR_TEXCRDOUT:
            if (major >= 3u) {
                return index < regs.output.size() ? &regs.output[index] : nullptr;
            }
            return index < regs.texOut.size() ? &regs.texOut[index] : nullptr;
        case D3DSPR_PREDICATE:
            return index < regs.predicate.size() ? &regs.predicate[index] : nullptr;
        case D3DSPR_LOOP:
            return index == 0u ? &regs.loop : nullptr;
        default:
            return nullptr;
    }
}

static bool simpleVsRelAddrOffset(const SimpleVsRegisters& regs,
                                  UINT major,
                                  DWORD token,
                                  int32_t& offset) {
    const UINT type = shaderRegType(token);
    if (type != D3DSPR_ADDR && type != D3DSPR_LOOP) return false;
    const auto* reg = simpleVsRegisterConst(regs, major, type,
                                            shaderRegIndex(token));
    if (!reg) return false;
    const UINT component = shaderSwizzle(token) & 0x3u;
    offset = static_cast<int32_t>(std::lround((*reg)[component]));
    return true;
}

static bool simpleVsSourceIndex(const SimpleVsRegisters& regs,
                                UINT major,
                                DWORD token,
                                DWORD relAddrToken,
                                UINT maxCount,
                                UINT& index) {
    long effective = static_cast<long>(shaderRegIndex(token));
    if (simpleProcessShaderTokenHasRelAddr(token)) {
        if (relAddrToken == 0u) return false;
        int32_t relOffset = 0;
        if (!simpleVsRelAddrOffset(regs, major, relAddrToken, relOffset)) {
            return false;
        }
        effective += relOffset;
        if (effective < 0) effective = 0;
        const long maxIndex = maxCount > 0u ? static_cast<long>(maxCount - 1u) : 0;
        if (effective > maxIndex) effective = maxIndex;
    }
    if (effective < 0 || static_cast<UINT>(effective) >= maxCount) return false;
    index = static_cast<UINT>(effective);
    return true;
}

static bool simpleVsReadSource(const SimpleVsRegisters& regs,
                               UINT major,
                               DWORD token,
                               float out[4],
                               DWORD relAddrToken = 0u) {
    const UINT type = shaderRegType(token);
    UINT index = 0;
    switch (type) {
        case D3DSPR_TEMP:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.temp.size()), index)) {
                return false;
            }
            break;
        case D3DSPR_CONST:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.constant.size()), index)) {
                return false;
            }
            break;
        case D3DSPR_CONSTINT:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.constantInt.size()), index)) {
                return false;
            }
            break;
        case D3DSPR_INPUT:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.input.size()), index)) {
                return false;
            }
            break;
        default:
            if (simpleProcessShaderTokenHasRelAddr(token)) return false;
            index = shaderRegIndex(token);
            break;
    }
    const auto* reg = simpleVsRegisterConst(regs, major, type, index);
    const UINT swizzle = shaderSwizzle(token);
    if (type == D3DSPR_CONSTINT) {
        const auto& intReg = regs.constantInt[index];
        for (UINT i = 0; i < 4; ++i) {
            out[i] = static_cast<float>(intReg[(swizzle >> (i * 2u)) & 0x3u]);
        }
    } else {
        if (!reg) return false;
        for (UINT i = 0; i < 4; ++i) {
            out[i] = (*reg)[(swizzle >> (i * 2u)) & 0x3u];
        }
    }
    const UINT modifier = (token & D3DSP_SRCMOD_MASK) >> D3DSP_SRCMOD_SHIFT;
    switch (modifier) {
        case 0: /* D3DSPSM_NONE */
            return true;
        case 1: /* D3DSPSM_NEG */
            for (UINT i = 0; i < 4; ++i) out[i] = -out[i];
            return true;
        case 2: /* D3DSPSM_BIAS */
            for (UINT i = 0; i < 4; ++i) out[i] -= 0.5f;
            return true;
        case 3: /* D3DSPSM_BIASNEG */
            for (UINT i = 0; i < 4; ++i) out[i] = -(out[i] - 0.5f);
            return true;
        case 4: /* D3DSPSM_SIGN */
            for (UINT i = 0; i < 4; ++i) out[i] = out[i] * 2.0f - 1.0f;
            return true;
        case 5: /* D3DSPSM_SIGNNEG */
            for (UINT i = 0; i < 4; ++i) out[i] = -(out[i] * 2.0f - 1.0f);
            return true;
        case 6: /* D3DSPSM_COMP */
            for (UINT i = 0; i < 4; ++i) out[i] = 1.0f - out[i];
            return true;
        case 7: /* D3DSPSM_X2 */
            for (UINT i = 0; i < 4; ++i) out[i] *= 2.0f;
            return true;
        case 8: /* D3DSPSM_X2NEG */
            for (UINT i = 0; i < 4; ++i) out[i] *= -2.0f;
            return true;
        case 9: { /* D3DSPSM_DZ */
            const float z = out[2];
            for (UINT i = 0; i < 4; ++i) out[i] = z != 0.0f ? out[i] / z : 0.0f;
            return true;
        }
        case 10: { /* D3DSPSM_DW */
            const float w = out[3];
            for (UINT i = 0; i < 4; ++i) out[i] = w != 0.0f ? out[i] / w : 0.0f;
            return true;
        }
        case 11: /* D3DSPSM_ABS */
            for (UINT i = 0; i < 4; ++i) if (out[i] < 0.0f) out[i] = -out[i];
            return true;
        case 12: /* D3DSPSM_ABSNEG */
            for (UINT i = 0; i < 4; ++i) {
                if (out[i] < 0.0f) out[i] = -out[i];
                out[i] = -out[i];
            }
            return true;
        case 13: /* D3DSPSM_NOT */
            for (UINT i = 0; i < 4; ++i) out[i] = out[i] != 0.0f ? 0.0f : 1.0f;
            return true;
        default:
            return false;
    }
}

static bool simpleVsSampleTexture2D(const SimpleVsTextureState* textures,
                                    UINT sampler,
                                    const float coord[4],
                                    float out[4]) {
    if (!textures || sampler >= textures->vertexTextures.size()) return false;
    auto* texture = textures->vertexTextures[sampler];
    if (!texture) return false;
    const UINT levels = dxmt9c_texture_get_level_count(texture);
    if (levels == 0u) return false;
    const long requestedLevel = std::lround(coord[3]);
    const long minMipLevel = static_cast<long>(textures->minMipLevel[sampler]);
    const UINT level = static_cast<UINT>(
        std::clamp<long>(std::max(requestedLevel, minMipLevel), 0,
                         static_cast<long>(levels - 1u)));
    bool border = false;
    const auto addressCoord = [&](float value, DWORD mode) -> float {
        if (!std::isfinite(value)) value = 0.0f;
        switch (mode) {
            case D3DTADDRESS_WRAP: {
                float wrapped = std::fmod(value, 1.0f);
                if (wrapped < 0.0f) wrapped += 1.0f;
                return wrapped;
            }
            case D3DTADDRESS_MIRROR: {
                float mirrored = std::fmod(value, 2.0f);
                if (mirrored < 0.0f) mirrored += 2.0f;
                return mirrored <= 1.0f ? mirrored : 2.0f - mirrored;
            }
            case D3DTADDRESS_BORDER:
                if (value < 0.0f || value > 1.0f) border = true;
                return std::clamp(value, 0.0f, 1.0f);
            case D3DTADDRESS_MIRRORONCE:
                return std::clamp(std::fabs(value), 0.0f, 1.0f);
            case D3DTADDRESS_CLAMP:
            default:
                return std::clamp(value, 0.0f, 1.0f);
        }
    };
    const float u = addressCoord(coord[0], textures->addressU[sampler]);
    const float v = addressCoord(coord[1], textures->addressV[sampler]);
    if (border) {
        const DWORD color = textures->borderColor[sampler];
        out[0] = static_cast<float>((color >> 16) & 0xffu) / 255.0f;
        out[1] = static_cast<float>((color >> 8) & 0xffu) / 255.0f;
        out[2] = static_cast<float>(color & 0xffu) / 255.0f;
        out[3] = static_cast<float>((color >> 24) & 0xffu) / 255.0f;
        return true;
    }
    return SUCCEEDED(hr32(dxmt9c_texture_sample_2d(
        texture, level, u, v, out)));
}

static bool simpleVsWriteDest(SimpleVsRegisters& regs,
                              UINT major,
                              DWORD token,
                              const float in[4],
                              DWORD relAddrToken = 0u) {
    const UINT type = shaderRegType(token);
    UINT index = shaderRegIndex(token);
    if (simpleProcessShaderTokenHasRelAddr(token)) {
        UINT maxCount = 0;
        switch (type) {
            case D3DSPR_TEMP:
                maxCount = static_cast<UINT>(regs.temp.size());
                break;
            case D3DSPR_CONST:
                maxCount = static_cast<UINT>(regs.constant.size());
                break;
            case D3DSPR_TEXCRDOUT:
                maxCount = major >= 3u
                               ? static_cast<UINT>(regs.output.size())
                               : static_cast<UINT>(regs.texOut.size());
                break;
            default:
                return false;
        }
        if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                 maxCount, index)) {
            return false;
        }
    }
    auto* reg = simpleVsRegister(regs, major, type, index);
    if (!reg) return false;
    float value[4] = {in[0], in[1], in[2], in[3]};
    const UINT modifier = (token & D3DSP_DSTMOD_MASK) >> D3DSP_DSTMOD_SHIFT;
    if (modifier & 0x1u) {
        for (float& v : value) v = clamp01(v);
    }
    if ((modifier & ~0x3u) != 0u) return false;
    const UINT mask = shaderWriteMask(token);
    for (UINT i = 0; i < 4; ++i) {
        if (mask & (1u << i)) {
            (*reg)[i] = value[i];
        }
    }
    return true;
}

static bool simpleVsIfcCompare(DWORD token, float a, float b) {
    switch ((token >> 16u) & 0xfu) {
        case 1: return a > b;   /* D3DSPC_GT */
        case 2: return a == b;  /* D3DSPC_EQ */
        case 3: return a >= b;  /* D3DSPC_GE */
        case 4: return a < b;   /* D3DSPC_LT */
        case 5: return a != b;  /* D3DSPC_NE */
        case 6: return a <= b;  /* D3DSPC_LE */
        default: return a == b;
    }
}

static bool simpleVsSkipControlBlock(const std::vector<DWORD>& words,
                                     size_t& index,
                                     bool stopAtElse) {
    UINT depth = 0;
    for (size_t scan = index; scan < words.size();) {
        const DWORD token = words[scan++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return false;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, scan, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, scan, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode == D3DSIO_IF || opcode == D3DSIO_IFC) {
            ++depth;
        } else if (opcode == D3DSIO_ENDIF) {
            if (depth == 0u) {
                index = scan;
                return true;
            }
            --depth;
        } else if (opcode == D3DSIO_ELSE && stopAtElse && depth == 0u) {
            index = scan;
            return true;
        }
    }
    return false;
}

static bool simpleVsFindLoopEnd(const std::vector<DWORD>& words,
                                size_t bodyBegin,
                                UINT endOpcode,
                                size_t& bodyEnd,
                                size_t& afterEnd) {
    UINT depth = 0;
    for (size_t scan = bodyBegin; scan < words.size();) {
        const size_t tokenIndex = scan;
        const DWORD token = words[scan++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return false;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, scan, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, scan, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode == D3DSIO_LOOP || opcode == D3DSIO_REP) {
            ++depth;
        } else if (opcode == D3DSIO_ENDLOOP || opcode == D3DSIO_ENDREP) {
            if (depth == 0u) {
                if (opcode != endOpcode) return false;
                bodyEnd = tokenIndex;
                afterEnd = scan;
                return true;
            }
            --depth;
        }
    }
    return false;
}

static bool simpleVsLoopCount(const SimpleVsRegisters& regs,
                              const ProcessShaderIo& io,
                              DWORD source,
                              UINT& count,
                              DWORD relAddrToken = 0u) {
    float value[4]{};
    if (!simpleVsReadSource(regs, io.major, source, value, relAddrToken)) return false;
    const long rounded = std::lround(value[0]);
    if (rounded <= 0) {
        count = 0;
        return true;
    }
    if (rounded > 1024) return false;
    count = static_cast<UINT>(rounded);
    return true;
}

static bool simpleVsLoopControl(const SimpleVsRegisters& regs,
                                const ProcessShaderIo& io,
                                DWORD source,
                                UINT& count,
                                int32_t& initial,
                                int32_t& step,
                                DWORD relAddrToken = 0u) {
    float value[4]{};
    if (!simpleVsReadSource(regs, io.major, source, value, relAddrToken)) return false;
    const long roundedCount = std::lround(value[0]);
    if (roundedCount <= 0) {
        count = 0;
    } else {
        if (roundedCount > 1024) return false;
        count = static_cast<UINT>(roundedCount);
    }
    initial = static_cast<int32_t>(std::lround(value[1]));
    step = static_cast<int32_t>(std::lround(value[2]));
    return true;
}

static UINT simpleVsLabelIndex(DWORD token) {
    return token & D3DSP_REGNUM_MASK;
}

static bool simpleVsFindRet(const std::vector<DWORD>& words,
                            size_t bodyBegin,
                            size_t& retIndex,
                            size_t& afterRet) {
    UINT depth = 0;
    for (size_t scan = bodyBegin; scan < words.size();) {
        const size_t tokenIndex = scan;
        const DWORD token = words[scan++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return false;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, scan, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, scan, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode == D3DSIO_IF || opcode == D3DSIO_IFC ||
            opcode == D3DSIO_LOOP || opcode == D3DSIO_REP) {
            ++depth;
        } else if (opcode == D3DSIO_ENDIF || opcode == D3DSIO_ENDLOOP ||
                   opcode == D3DSIO_ENDREP) {
            if (depth == 0u) return false;
            --depth;
        } else if (opcode == D3DSIO_RET && depth == 0u) {
            retIndex = tokenIndex;
            afterRet = scan;
            return true;
        } else if (opcode == D3DSIO_LABEL && depth == 0u) {
            return false;
        }
    }
    return false;
}

static bool simpleVsSkipLabelBody(const std::vector<DWORD>& words,
                                  size_t& index) {
    while (index < words.size()) {
        const size_t tokenIndex = index;
        const DWORD token = words[index++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, index, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, index, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode != D3DSIO_LABEL) {
            index = tokenIndex;
            break;
        }
    }
    size_t retIndex = 0;
    size_t afterRet = 0;
    if (!simpleVsFindRet(words, index, retIndex, afterRet)) return false;
    index = afterRet;
    return true;
}

static bool simpleVsFindLabelRange(const std::vector<DWORD>& words,
                                   UINT targetLabel,
                                   size_t& bodyBegin,
                                   size_t& retIndex) {
    for (size_t scan = 1; scan < words.size();) {
        const DWORD token = words[scan++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return false;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, scan, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, scan, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode != D3DSIO_LABEL) {
            continue;
        }

        bool found = false;
        for (;;) {
            if (parsedOperands.count < 1u) return false;
            if (simpleVsLabelIndex(parsedOperands.operands[0]) == targetLabel) {
                found = true;
            }
            if (scan >= words.size()) return false;
            const DWORD nextToken = words[scan];
            if ((nextToken & D3DSI_OPCODE_MASK) != D3DSIO_LABEL) break;
            ++scan;
            if (!simpleProcessShaderReadOperands(
                    words, scan, D3DSIO_LABEL, nextToken, parsedOperands)) {
                return false;
            }
        }

        size_t afterRet = 0;
        if (!simpleVsFindRet(words, scan, retIndex, afterRet)) return false;
        if (found) {
            bodyBegin = scan;
            return true;
        }
        scan = afterRet;
    }
    return false;
}

enum class SimpleVsExecResult {
    Ok,
    Fail,
    Break,
    Ret,
};

static SimpleVsExecResult executeSimpleProcessVertexShaderRange(
        const std::vector<DWORD>& words,
        const ProcessShaderIo& io,
        SimpleVsRegisters& regs,
        const SimpleVsTextureState* textures,
        size_t begin,
        size_t end,
        UINT recursionDepth) {
    if (recursionDepth > 32u) return SimpleVsExecResult::Fail;
    for (size_t index = begin; index < end;) {
        const DWORD token = words[index++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return SimpleVsExecResult::Ok;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, index, token)) return SimpleVsExecResult::Fail;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, index, opcode, token,
                                             parsedOperands)) {
            return SimpleVsExecResult::Fail;
        }
        const UINT operandCount = parsedOperands.count;
        const DWORD* operands = parsedOperands.operands.data();
        const DWORD* relAddrOperands = parsedOperands.relAddrOperands.data();
        if (opcode == D3DSIO_NOP || opcode == D3DSIO_DCL || opcode == D3DSIO_PHASE) {
            continue;
        }
        if (opcode == D3DSIO_RET) {
            return SimpleVsExecResult::Ret;
        }
        if (opcode == D3DSIO_LABEL) {
            if (!simpleVsSkipLabelBody(words, index)) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }
        if (opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ) {
            if (operandCount < 1u) return SimpleVsExecResult::Fail;
            bool takeCall = true;
            if (opcode == D3DSIO_CALLNZ) {
                if (operandCount < 2u) return SimpleVsExecResult::Fail;
                float condition[4]{};
                if (!simpleVsReadSource(regs, io.major, operands[1], condition,
                                        relAddrOperands[1])) {
                    return SimpleVsExecResult::Fail;
                }
                takeCall = condition[0] != 0.0f;
            }
            if (!takeCall) continue;
            size_t bodyBegin = 0;
            size_t retIndex = 0;
            if (!simpleVsFindLabelRange(
                    words, simpleVsLabelIndex(operands[0]), bodyBegin, retIndex)) {
                return SimpleVsExecResult::Fail;
            }
            const SimpleVsExecResult callResult =
                executeSimpleProcessVertexShaderRange(
                    words, io, regs, textures, bodyBegin, retIndex + 1u,
                    recursionDepth + 1u);
            if (callResult == SimpleVsExecResult::Fail ||
                callResult == SimpleVsExecResult::Break) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }
        if (opcode == D3DSIO_BREAK) {
            return SimpleVsExecResult::Break;
        }
        if (opcode == D3DSIO_BREAKP) {
            float predicate[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[0], predicate,
                                    relAddrOperands[0])) {
                return SimpleVsExecResult::Fail;
            }
            if (predicate[0] != 0.0f) {
                return SimpleVsExecResult::Break;
            }
            continue;
        }
        if (opcode == D3DSIO_BREAKC) {
            float condition[4]{};
            float rhs[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[0], condition,
                                    relAddrOperands[0]) ||
                !simpleVsReadSource(regs, io.major, operands[1], rhs,
                                    relAddrOperands[1])) {
                return SimpleVsExecResult::Fail;
            }
            if (simpleVsIfcCompare(token, condition[0], rhs[0])) {
                return SimpleVsExecResult::Break;
            }
            continue;
        }
        if (opcode == D3DSIO_MOVA) {
            const UINT dstType = shaderRegType(operands[0]);
            if (dstType != D3DSPR_ADDR && dstType != D3DSPR_LOOP) {
                return SimpleVsExecResult::Fail;
            }
            auto* dst = simpleVsRegister(regs, io.major, dstType,
                                         shaderRegIndex(operands[0]));
            if (!dst) return SimpleVsExecResult::Fail;
            float value[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[1], value,
                                    relAddrOperands[1])) {
                return SimpleVsExecResult::Fail;
            }
            const float rounded = static_cast<float>(std::lround(value[0]));
            *dst = {rounded, rounded, rounded, rounded};
            continue;
        }
        if (opcode == D3DSIO_SETP) {
            if (shaderRegType(operands[0]) != D3DSPR_PREDICATE) {
                return SimpleVsExecResult::Fail;
            }
            auto* predicate = simpleVsRegister(
                regs, io.major, D3DSPR_PREDICATE, shaderRegIndex(operands[0]));
            if (!predicate) return SimpleVsExecResult::Fail;
            float value[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[1], value,
                                    relAddrOperands[1])) {
                return SimpleVsExecResult::Fail;
            }
            (*predicate)[0] = value[0] != 0.0f ? 1.0f : 0.0f;
            (*predicate)[1] = (*predicate)[2] = (*predicate)[3] = (*predicate)[0];
            continue;
        }
        if (opcode == D3DSIO_REP || opcode == D3DSIO_LOOP) {
            if (operandCount == 0u) return SimpleVsExecResult::Fail;
            size_t bodyEnd = 0;
            size_t afterEnd = 0;
            const UINT endOpcode = opcode == D3DSIO_REP ? D3DSIO_ENDREP : D3DSIO_ENDLOOP;
            if (!simpleVsFindLoopEnd(words, index, endOpcode, bodyEnd, afterEnd)) {
                return SimpleVsExecResult::Fail;
            }
            const DWORD countSource = opcode == D3DSIO_LOOP && operandCount > 1u
                                          ? operands[1]
                                          : operands[0];
            UINT count = 0;
            int32_t loopValue = 0;
            int32_t loopStep = 0;
            const std::array<float, 4> savedLoop = regs.loop;
            if (opcode == D3DSIO_LOOP) {
                if (operandCount < 2u || shaderRegType(operands[0]) != D3DSPR_LOOP) {
                    return SimpleVsExecResult::Fail;
                }
                const DWORD countRelAddr = opcode == D3DSIO_LOOP && operandCount > 1u
                                               ? relAddrOperands[1]
                                               : relAddrOperands[0];
                if (!simpleVsLoopControl(
                        regs, io, countSource, count, loopValue, loopStep,
                        countRelAddr)) {
                    return SimpleVsExecResult::Fail;
                }
            } else if (!simpleVsLoopCount(
                           regs, io, countSource, count, relAddrOperands[0])) {
                return SimpleVsExecResult::Fail;
            }
            for (UINT iteration = 0; iteration < count; ++iteration) {
                if (opcode == D3DSIO_LOOP) {
                    const float loopFloat = static_cast<float>(loopValue);
                    regs.loop = {loopFloat, loopFloat, loopFloat, loopFloat};
                }
                const SimpleVsExecResult loopResult =
                    executeSimpleProcessVertexShaderRange(
                        words, io, regs, textures, index, bodyEnd,
                        recursionDepth + 1u);
                if (loopResult == SimpleVsExecResult::Fail) {
                    if (opcode == D3DSIO_LOOP) regs.loop = savedLoop;
                    return SimpleVsExecResult::Fail;
                }
                if (loopResult == SimpleVsExecResult::Break) {
                    break;
                }
                if (loopResult == SimpleVsExecResult::Ret) {
                    if (opcode == D3DSIO_LOOP) regs.loop = savedLoop;
                    return SimpleVsExecResult::Ret;
                }
                loopValue += loopStep;
            }
            if (opcode == D3DSIO_LOOP) regs.loop = savedLoop;
            index = afterEnd;
            continue;
        }
        if (opcode == D3DSIO_ENDREP || opcode == D3DSIO_ENDLOOP) {
            return SimpleVsExecResult::Fail;
        }
        if (opcode == D3DSIO_IF || opcode == D3DSIO_IFC) {
            float condition[4]{};
            float rhs[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[0], condition,
                                    relAddrOperands[0])) {
                return SimpleVsExecResult::Fail;
            }
            bool takeBranch = condition[0] != 0.0f;
            if (opcode == D3DSIO_IFC) {
                if (!simpleVsReadSource(regs, io.major, operands[1], rhs,
                                        relAddrOperands[1])) {
                    return SimpleVsExecResult::Fail;
                }
                takeBranch = simpleVsIfcCompare(token, condition[0], rhs[0]);
            }
            if (!takeBranch &&
                !simpleVsSkipControlBlock(words, index, true)) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }
        if (opcode == D3DSIO_ELSE) {
            if (!simpleVsSkipControlBlock(words, index, false)) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }
        if (opcode == D3DSIO_ENDIF) {
            continue;
        }
        if (opcode == D3DSIO_DEF) {
            if (shaderRegType(operands[0]) != D3DSPR_CONST) return SimpleVsExecResult::Fail;
            auto* dst = simpleVsRegister(regs, io.major, D3DSPR_CONST,
                                         shaderRegIndex(operands[0]));
            if (!dst) return SimpleVsExecResult::Fail;
            std::memcpy(dst->data(), operands + 1, sizeof(float) * 4u);
            continue;
        }
        if (opcode == D3DSIO_DEFI) {
            if (shaderRegType(operands[0]) != D3DSPR_CONSTINT) return SimpleVsExecResult::Fail;
            const UINT indexConst = shaderRegIndex(operands[0]);
            if (indexConst >= regs.constantInt.size()) return SimpleVsExecResult::Fail;
            for (UINT i = 0; i < 4; ++i) {
                regs.constantInt[indexConst][i] = static_cast<int32_t>(operands[i + 1u]);
            }
            continue;
        }
        if (opcode == D3DSIO_TEXLDL) {
            if (operandCount < 3u ||
                shaderRegType(operands[2]) != D3DSPR_SAMPLER) {
                return SimpleVsExecResult::Fail;
            }
            float coord[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[1], coord,
                                    relAddrOperands[1]) ||
                !simpleVsSampleTexture2D(textures, shaderRegIndex(operands[2]),
                                         coord, coord) ||
                !simpleVsWriteDest(regs, io.major, operands[0], coord,
                                   relAddrOperands[0])) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }

        float a[4]{};
        float b[4]{};
        float c[4]{};
        float out[4]{};
        if (operandCount >= 2 &&
            !simpleVsReadSource(regs, io.major, operands[1], a,
                                relAddrOperands[1])) {
            return SimpleVsExecResult::Fail;
        }
        if (operandCount >= 3 &&
            !simpleVsReadSource(regs, io.major, operands[2], b,
                                relAddrOperands[2])) {
            return SimpleVsExecResult::Fail;
        }
        if (operandCount >= 4 &&
            !simpleVsReadSource(regs, io.major, operands[3], c,
                                relAddrOperands[3])) {
            return SimpleVsExecResult::Fail;
        }
        switch (opcode) {
            case D3DSIO_MOV:
                std::memcpy(out, a, sizeof(out));
                break;
            case D3DSIO_RCP: {
                const float value = a[0] != 0.0f ? 1.0f / a[0] : 0.0f;
                out[0] = out[1] = out[2] = out[3] = value;
                break;
            }
            case D3DSIO_RSQ: {
                const float value = a[0] > 0.0f ? 1.0f / std::sqrt(a[0]) : 0.0f;
                out[0] = out[1] = out[2] = out[3] = value;
                break;
            }
            case D3DSIO_FRC:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] - std::floor(a[i]);
                break;
            case D3DSIO_ABS:
                for (UINT i = 0; i < 4; ++i) out[i] = std::fabs(a[i]);
                break;
            case D3DSIO_SGN:
                for (UINT i = 0; i < 4; ++i) {
                    out[i] = a[i] > 0.0f ? 1.0f : (a[i] < 0.0f ? -1.0f : 0.0f);
                }
                break;
            case D3DSIO_SINCOS:
                out[0] = std::sin(a[0]);
                out[1] = std::cos(a[0]);
                out[2] = 0.0f;
                out[3] = 0.0f;
                break;
            case D3DSIO_EXP:
            case D3DSIO_EXPP:
                for (UINT i = 0; i < 4; ++i) out[i] = std::exp2(a[i]);
                break;
            case D3DSIO_LOG:
            case D3DSIO_LOGP:
                for (UINT i = 0; i < 4; ++i) out[i] = std::log2(std::fabs(a[i]));
                break;
            case D3DSIO_LIT: {
                const float x = a[0];
                const float y = a[1];
                const float w = std::max(-128.0f, std::min(128.0f, a[3]));
                out[0] = 1.0f;
                out[1] = std::max(x, 0.0f);
                out[2] = x > 0.0f ? std::pow(std::max(y, 0.0f), w) : 0.0f;
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_ADD:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] + b[i];
                break;
            case D3DSIO_SUB:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] - b[i];
                break;
            case D3DSIO_MUL:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] * b[i];
                break;
            case D3DSIO_MAD:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] * b[i] + c[i];
                break;
            case D3DSIO_LRP:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] * b[i] + (1.0f - a[i]) * c[i];
                break;
            case D3DSIO_SLT:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] < b[i] ? 1.0f : 0.0f;
                break;
            case D3DSIO_SGE:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] >= b[i] ? 1.0f : 0.0f;
                break;
            case D3DSIO_MIN:
                for (UINT i = 0; i < 4; ++i) out[i] = std::min(a[i], b[i]);
                break;
            case D3DSIO_MAX:
                for (UINT i = 0; i < 4; ++i) out[i] = std::max(a[i], b[i]);
                break;
            case D3DSIO_DP3: {
                const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
                out[0] = out[1] = out[2] = out[3] = dot;
                break;
            }
            case D3DSIO_DP4: {
                const float dot = a[0] * b[0] + a[1] * b[1] +
                                  a[2] * b[2] + a[3] * b[3];
                out[0] = out[1] = out[2] = out[3] = dot;
                break;
            }
            case D3DSIO_POW: {
                const float value = std::pow(a[0], b[0]);
                out[0] = out[1] = out[2] = out[3] = value;
                break;
            }
            case D3DSIO_CRS:
                out[0] = a[1] * b[2] - a[2] * b[1];
                out[1] = a[2] * b[0] - a[0] * b[2];
                out[2] = a[0] * b[1] - a[1] * b[0];
                out[3] = 1.0f;
                break;
            case D3DSIO_DST:
                out[0] = 1.0f;
                out[1] = a[1] * b[1];
                out[2] = a[2];
                out[3] = b[3];
                break;
            case D3DSIO_NRM: {
                const float lenSq = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
                const float invLen = lenSq > 0.0f ? 1.0f / std::sqrt(lenSq) : 0.0f;
                out[0] = a[0] * invLen;
                out[1] = a[1] * invLen;
                out[2] = a[2] * invLen;
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_M4x4: {
                if (shaderRegType(operands[2]) != D3DSPR_CONST) return SimpleVsExecResult::Fail;
                const UINT base = shaderRegIndex(operands[2]);
                if (base + 3u >= regs.constant.size()) return SimpleVsExecResult::Fail;
                for (UINT row = 0; row < 4; ++row) {
                    const auto& c = regs.constant[base + row];
                    out[row] = a[0] * c[0] + a[1] * c[1] +
                               a[2] * c[2] + a[3] * c[3];
                }
                break;
            }
            case D3DSIO_M4x3: {
                if (shaderRegType(operands[2]) != D3DSPR_CONST) return SimpleVsExecResult::Fail;
                const UINT base = shaderRegIndex(operands[2]);
                if (base + 2u >= regs.constant.size()) return SimpleVsExecResult::Fail;
                for (UINT row = 0; row < 3; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] +
                               a[2] * k[2] + a[3] * k[3];
                }
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_M3x4: {
                if (shaderRegType(operands[2]) != D3DSPR_CONST) return SimpleVsExecResult::Fail;
                const UINT base = shaderRegIndex(operands[2]);
                if (base + 3u >= regs.constant.size()) return SimpleVsExecResult::Fail;
                for (UINT row = 0; row < 4; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] + a[2] * k[2];
                }
                break;
            }
            case D3DSIO_M3x3: {
                if (shaderRegType(operands[2]) != D3DSPR_CONST) return SimpleVsExecResult::Fail;
                const UINT base = shaderRegIndex(operands[2]);
                if (base + 2u >= regs.constant.size()) return SimpleVsExecResult::Fail;
                for (UINT row = 0; row < 3; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] + a[2] * k[2];
                }
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_M3x2: {
                if (shaderRegType(operands[2]) != D3DSPR_CONST) return SimpleVsExecResult::Fail;
                const UINT base = shaderRegIndex(operands[2]);
                if (base + 1u >= regs.constant.size()) return SimpleVsExecResult::Fail;
                for (UINT row = 0; row < 2; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] + a[2] * k[2];
                }
                out[2] = 0.0f;
                out[3] = 0.0f;
                break;
            }
            default:
                return SimpleVsExecResult::Fail;
        }
        if (!simpleVsWriteDest(regs, io.major, operands[0], out,
                               relAddrOperands[0])) {
            return SimpleVsExecResult::Fail;
        }
    }
    return SimpleVsExecResult::Ok;
}

static bool executeSimpleProcessVertexShader(const std::vector<DWORD>& words,
                                             const ProcessShaderIo& io,
                                             SimpleVsRegisters& regs,
                                             const SimpleVsTextureState* textures) {
    const SimpleVsExecResult result =
        executeSimpleProcessVertexShaderRange(
            words, io, regs, textures, 1, words.size(), 0);
    return result == SimpleVsExecResult::Ok || result == SimpleVsExecResult::Ret;
}

}  // namespace

/* =========================================================================
 * Raw-handle extractors — safe because only our device creates these objects.
 * ========================================================================= */

static D9CSurface*   rawSurf(IDirect3DSurface9* p)          { return D3D9PeRawSurface(p); }
static D9CBuffer*    rawVBuf(IDirect3DVertexBuffer9* p)     { return D3D9PeRawVertexBuffer(p); }
static D9CBuffer*    rawIBuf(IDirect3DIndexBuffer9* p)      { return D3D9PeRawIndexBuffer(p); }
static D9CShader*    rawVS(IDirect3DVertexShader9* p)       { return D3D9PeRawVertexShader(p); }
static D9CShader*    rawPS(IDirect3DPixelShader9* p)        { return D3D9PeRawPixelShader(p); }
static D9CVertexDecl* rawVD(IDirect3DVertexDeclaration9* p) { return D3D9PeRawVertexDecl(p); }
static D9CTexture*   rawTex(IDirect3DBaseTexture9* p)       { return D3D9PeRawTexture(p); }

/* R-FORMAT-11 — RESZ MSAA depth-resolve trigger. RESZ is a *command*, not
 * storage: an app requests a multisample depth resolve into the bound INTZ
 * depth texture by writing this exact sentinel to D3DRS_POINTSIZE while the
 * multisampled depth surface is bound as a texture. Any other D3DRS_POINTSIZE
 * value keeps its ordinary point-size meaning. The value-level classification
 * is unit-pinned by core::isReszDepthResolveSentinel (core_constants.hpp); it
 * is duplicated here as a literal because this PE translation unit speaks the
 * C ABI (device_c.h), not the C++ core header. See
 * specs/d3d9/formats/{requirements,design}.md. */
static constexpr DWORD kReszDepthResolveSentinel = 0x7FA05000u;

/* =========================================================================
 * D3D9DeviceImpl — IDirect3DDevice9Ex
 * ========================================================================= */

class D3D9DeviceImpl final : public IDirect3DDevice9Ex, public D3D9PeRecorderFlush {
    static_assert(sizeof(D9CCommandChunkWireHeader) ==
                  D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE);
    static_assert(sizeof(D9CCommandChunkWireRecordHeader) ==
                  D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE);
    static_assert(sizeof(D9CCommandChunkWireHandleEntry) ==
                  D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE);

    // Phase 21: chunk-flush thresholds. Defaults match what the PE
    // recorder has been tuned around since Phase 5 (64 records = a few
    // dozen draws + their state setters; 256 KB ≈ one full vertex
    // upload for a complex draw + headers). Both are env-overridable
    // via DXMT9_PE_CHUNK_MAX_RECORDS / DXMT9_PE_CHUNK_MAX_BYTES; the
    // helpers below cap the env values to prevent pathological inputs
    // from blowing chunk-side allocations.
    static constexpr UINT kDefaultMaxPendingCommandRecords = 64;
    static constexpr size_t kDefaultMaxPendingCommandBytes = 256 * 1024;
    static constexpr UINT kAbsoluteMaxPendingCommandRecords = 4096;
    static constexpr size_t kAbsoluteMaxPendingCommandBytes = 16 * 1024 * 1024;
    static UINT maxPendingCommandRecords() {
        static const UINT cached = []() -> UINT {
            const auto envValue = dxmt9::util::getenvU32("DXMT9_PE_CHUNK_MAX_RECORDS");
            if (!envValue || *envValue == 0) return kDefaultMaxPendingCommandRecords;
            return std::min<UINT>(*envValue, kAbsoluteMaxPendingCommandRecords);
        }();
        return cached;
    }
    static size_t maxPendingCommandBytes() {
        static const size_t cached = []() -> size_t {
            const auto envValue = dxmt9::util::getenvU64("DXMT9_PE_CHUNK_MAX_BYTES");
            if (!envValue || *envValue == 0) return kDefaultMaxPendingCommandBytes;
            return std::min<size_t>(*envValue, kAbsoluteMaxPendingCommandBytes);
        }();
        return cached;
    }

    using WireHandleEntryList = D3D9PeWireHandleEntryList;

    ULONG        refs_    = 1;
    D9CDevice*   dev_;
    IDirect3D9Ex* factory_;
    UINT         adapter_ = 0;
    D3DDEVTYPE   deviceType_ = D3DDEVTYPE_HAL;
    DWORD        behaviorFlags_ = 0;
    BOOL         softwareVertexProcessing_ = FALSE;
    // Wine d3d9ex test_frame_latency: default Ex device latency is 3.
    UINT         maxFrameLatencyShadow_ = 3;
    // Wine d3d9 base_vidmem_accounting_policy + ex_vidmem_accounting_policy:
    // GetAvailableTextureMem must report a value that strictly decreases as
    // large RT-pool textures are allocated. Monotonic accounting only; we
    // never grow it back on Release because the test does not require it
    // and tracking release would couple the device to every child wrapper.
    uint64_t     vidmemBytesUsedShadow_ = 0;
    bool         extended_ = false;
    bool         cursorSurfaceSet_ = false;
    bool         cursorVisible_ = false;
    bool         deviceNotReset_ = false;
    uint32_t     defaultPoolResourceRefs_ = 0;
    bool         stateBlockRecording_ = false;
    // Internal guard: when true, SetTransform during recording will NOT
    // append to stateBlockTransformRecorded. MultiplyTransform raises this
    // around its internal SetTransform so its product does not get recorded
    // into the stateblock — matches wined3d semantics that
    // MultiplyTransform inside BeginStateBlock/EndStateBlock is invisible
    // to the resulting stateblock (the device state itself IS modified,
    // and is NOT reverted at EndStateBlock).
    bool         suppressStateBlockTransformRecord_ = false;
    // True only inside EndStateBlock's CreatePeStateBlock call. Tells the
    // freshly-constructed D3D9StateBlockImpl that its initial transform
    // tracked-keys set should be EXACTLY stateBlockTransformRecorded —
    // even if that table is empty (a Begin/End block where everything was
    // MultiplyTransform). Without this flag, an empty recorded set would
    // be indistinguishable from a CreateStateBlock(D3DSBT_ALL) call,
    // which legitimately needs every populated transform captured.
    bool         insideEndStateBlock_ = false;
    std::recursive_mutex recorderMutex_{};

    /* bound resource tracking (AddRef'd) */
    IDirect3DBaseTexture9*     textures_[D9C_DRAW_PACKET_MAX_TEXTURES] = {};
    IDirect3DVertexShader9*    vs_              = nullptr;
    IDirect3DPixelShader9*     ps_              = nullptr;
    IDirect3DVertexBuffer9*    streamSrc_[16]   = {};
    UINT                       streamOff_[16]   = {};
    UINT                       streamStr_[16]   = {};
    UINT                       streamFreq_[16]  = {};
    IDirect3DIndexBuffer9*     indexBuf_        = nullptr;
    IDirect3DVertexDeclaration9* vdecl_         = nullptr;
    DWORD                      fvf_             = 0;
    /* Cache of implicit FVF→decl shadow objects. Wine's
     * test_fvf_decl_management requires that two SetFVF(F) calls return
     * the SAME IDirect3DVertexDeclaration9* via GetVertexDeclaration, so
     * we memoize by FVF. The map owns one reference per entry; entries
     * are released in releaseAllBound(). */
    std::unordered_map<DWORD, IDirect3DVertexDeclaration9*> fvfDeclCache_{};
    IDirect3DSurface9*         cachedBackBuffer0_ = nullptr;
    /* Per-swap-chain wrapper cache: Wine d3d9 contract
     * (test_swapchain_backbuffer_getter_policy) is that
     * IDirect3DDevice9::GetBackBuffer(sc, idx, …) returns the same COM
     * pointer as the prior IDirect3DSwapChain9::GetBackBuffer(idx, …)
     * called against the same swap-chain. Achieve this by routing both
     * device-level GetBackBuffer and GetSwapChain through a stable
     * per-sc wrapper that owns the back-buffer cache. */
    std::unordered_map<UINT, IDirect3DSwapChain9*> swapchainWrappers_{};

    PeHotStateShadow peState_{};
    PeConstShadowBlock peConsts_{};
    IDirect3DSurface9* rtSlots_[4]{};
    bool rtSlotExplicit_[4]{};
    IDirect3DSurface9* dsSurface_ = nullptr;
    bool dsSurfaceExplicit_ = false;
    PeCommandChunkBuilder commandChunk_{};
    PeRecorderStats peRecorderStats_{};
    std::uint64_t peRecorderStatsLastLoggedCommitCount_ = 0;

    /* present params copy for GetCreationParameters */
    HWND creationWindow_ = nullptr;
    // Wine reset_lockable_backbuffer_policy: the implicit swap-chain's
    // PresentParameters.Flags is captured at CreateDevice and re-captured at
    // Reset because the C ABI does not yet round-trip the field.
    DWORD implicitSwapchainFlagsShadow_ = 0;

    /* Palette shadow for Wine conformance round-trip and active P8/A8P8
     * texture expansion. Set/Get remain PE-owned state; when a current
     * palette is selected, bound palettized textures push expanded
     * A8R8G8B8 texels to the backend for sampling. */
    std::unordered_map<UINT, std::array<PALETTEENTRY, 256>> palettes_{};
    UINT currentPaletteIndex_ = 0;
    bool currentPaletteSet_ = false;

    /* gamma ramp shadow — Set/GetGammaRamp round-trip (G1-4 audit
     * dispatch, Option B). CAMetalLayer has no per-channel gamma LUT
     * and Quartz Display Services mutates the entire display, so the
     * present-time apply pass is a follow-up track. For now we shadow
     * Set/Get so apps that probe the ramp round-trip see byte-equal
     * values, and SetGammaRamp no longer silently drops the ramp.
     *
     * Default is the identity ramp (ramp[i] = i << 8 per channel),
     * which is what wined3d also reports as the "original" gamma at
     * swap-chain creation when the platform exposes no real ramp.
     *
     * Wire-side apply (present-pass fullscreen-quad MSL gather with a
     * 256-entry LUT uniform) is intentionally deferred — it requires
     * a new Presenter pipeline variant + a 1.5 KB uniform-buffer
     * transport across the PE/unix boundary, both of which would
     * widen the winemetal ABI and are out of scope for this track.
     * The shadow is therefore sufficient for the Wine round-trip
     * conformance shape but does NOT calibrate the displayed image.
     *
     * Option A follow-up: `dxmt9c_device_set_gamma_ramp` now pushes
     * the same payload across the PE/unix boundary into core::Device
     * on every SetGammaRamp, so the present-pass actually applies the
     * ramp; this shadow remains the source of truth for Get reads. */
    D3DGAMMARAMP gammaRamp_{};

    // Initialize the gamma ramp shadow to the identity ramp. Called
    // from the constructor; the all-zero default-constructed state is
    // observably different from identity and would surprise apps that
    // never call SetGammaRamp.
    void initGammaRampIdentity() noexcept {
        for (UINT i = 0; i < 256; ++i) {
            const WORD v = static_cast<WORD>(i << 8);
            gammaRamp_.red[i]   = v;
            gammaRamp_.green[i] = v;
            gammaRamp_.blue[i]  = v;
        }
    }

    template<typename T>
    static void setRef(T*& slot, T* newVal) {
        if (newVal) newVal->AddRef();
        if (slot)   slot->Release();
        slot = newVal;
    }

    bool applyCurrentPaletteToTexture(IDirect3DBaseTexture9* texture) {
        if (!texture || !currentPaletteSet_) return false;
        const auto it = palettes_.find(currentPaletteIndex_);
        if (it == palettes_.end()) return false;
        D9CTexture* raw = rawTex(texture);
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

    void applyCurrentPaletteToBoundTextures() {
        for (auto* texture : textures_) {
            applyCurrentPaletteToTexture(texture);
        }
    }

    D9CMatrix transformOrIdentity(D3DTRANSFORMSTATETYPE state) const {
        D9CMatrix matrix = identityTransformMatrix();
        (void)peState_.transformShadow.get(static_cast<uint32_t>(state), matrix);
        return matrix;
    }

    static D9CMatrix multiplyTransformMatrix(const D9CMatrix& left,
                                             const D9CMatrix& right) {
        D9CMatrix result{};
        for (UINT row = 0; row < 4; ++row) {
            for (UINT col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (UINT k = 0; k < 4; ++k) {
                    sum += left.m[row * 4 + k] * right.m[k * 4 + col];
                }
                result.m[row * 4 + col] = sum;
            }
        }
        return result;
    }

    D9CMatrix worldViewProjectionTransform() const {
        const D9CMatrix world = transformOrIdentity(D3DTS_WORLD);
        const D9CMatrix view = transformOrIdentity(D3DTS_VIEW);
        const D9CMatrix projection = transformOrIdentity(D3DTS_PROJECTION);
        return multiplyTransformMatrix(multiplyTransformMatrix(world, view),
                                       projection);
    }

    void releaseAllBound() {
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

    void clearPendingCommandChunk() {
        commandChunk_.clear();
    }

    void clearPeStateTracking() {
        peState_.clearServerShadowTables();
        peState_.clearPendingHotState();
        peConsts_.reset();
        clearPendingCommandChunk();
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

    bool hasPendingHotState() const {
        return peState_.hasPendingHotState();
    }

    void clearPendingHotState() {
        peState_.clearPendingHotState();
    }

    bool shadowedRenderStateEquals(DWORD state, DWORD value) const {
        return peState_.renderStateEquals(state, value);
    }

    bool shadowedTextureEquals(DWORD stage, IDirect3DBaseTexture9* texture) const {
        uint32_t slot = 0;
        return textureBindingSlot(stage, slot) && textures_[slot] == texture;
    }

    bool shadowedStreamSourceEquals(UINT stream,
                                    IDirect3DVertexBuffer9* buffer,
                                    UINT offset,
                                    UINT stride) const {
        return stream < 16 && streamSrc_[stream] == buffer &&
               streamOff_[stream] == offset && streamStr_[stream] == stride;
    }

    dxmt9::d3d9::pe::PeRtWireHandles currentRtWireHandles() const {
        dxmt9::d3d9::pe::PeRtWireHandles handles{};
        for (DWORD slot = 0; slot < 4; ++slot) {
            handles[slot] = toWireHandle(rawSurf(rtSlots_[slot]));
        }
        return handles;
    }

    dxmt9::d3d9::pe::PeRtExplicitMask currentRtExplicitMask() const {
        dxmt9::d3d9::pe::PeRtExplicitMask explicitMask{};
        for (DWORD slot = 0; slot < 4; ++slot) {
            explicitMask[slot] = rtSlotExplicit_[slot];
        }
        return explicitMask;
    }

    bool buildDrawPrimitivePacket(D3DPRIMITIVETYPE type,
                                  UINT startVertex,
                                  UINT count,
                                  D9CDrawPrimitivePacket& packet) const {
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            return false;
        }

        packet = D9CDrawPrimitivePacket{};
        peState_.pendingRenderStates.forEach([&](uint32_t state, uint32_t value) {
            auto& entry = packet.renderStates[packet.renderStateCount++];
            entry.state = state;
            entry.value = value;
        });

        packet.textureMask = peState_.pendingTextureMask;
        for (DWORD stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
            if ((peState_.pendingTextureMask & (1u << stage)) != 0) {
                packet.textures[stage] = toWireHandle(rawTex(textures_[stage]));
            }
        }

        packet.streamSourceMask = peState_.pendingStreamMask;
        for (DWORD stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            if ((peState_.pendingStreamMask & (1u << stream)) == 0) {
                continue;
            }
            auto& source = packet.streamSources[stream];
            source.buffer = toWireHandle(rawVBuf(streamSrc_[stream]));
            source.offset = streamOff_[stream];
            source.stride = streamStr_[stream];
        }

        packet.fvfValid = peState_.pendingFvf ? 1u : 0u;
        packet.fvf = fvf_;
        // Phase 12: shader-handle delta. Server-side applyDrawPacketState
        // dispatches dxmt9c_device_set_vertex_shader / set_pixel_shader
        // when valid=1, mirroring the renderState/texture/stream pattern.
        packet.vsValid = peState_.pendingVs ? 1u : 0u;
        packet.vsHandle = toWireHandle(rawVS(vs_));
        packet.psValid = peState_.pendingPs ? 1u : 0u;
        packet.psHandle = toWireHandle(rawPS(ps_));
        packet.vdeclValid = peState_.pendingVdecl ? 1u : 0u;
        packet.vdeclHandle = toWireHandle(rawVD(vdecl_));
        // RT / DS delta — emit a handle for every pending bit. A set bit
        // with a zero wire handle is a deliberate detach.
        dxmt9::d3d9::pe::populateDrawPacketAttachmentDelta(
            packet, peState_.pendingRtMask, currentRtWireHandles(),
            peState_.pendingDs, toWireHandle(rawSurf(dsSurface_)));
        packet.viewportValid = peState_.pendingViewport ? 1u : 0u;
        packet.viewport = peState_.viewportShadow;
        packet.scissorValid = peState_.pendingScissor ? 1u : 0u;
        packet.scissor = peState_.scissorShadow;
        // Phase 12: drain TSS / SamplerState pending tables into packet
        // delta arrays. The cap check inside Set* already flushes the
        // chunk if a single Set would push beyond the per-packet limit;
        // here we just emit what's pending.
        if (peState_.pendingTss.size() > D9C_DRAW_PACKET_MAX_TSS ||
            peState_.pendingSamplerStates.size() > D9C_DRAW_PACKET_MAX_SAMPLER) {
            return false;
        }
        packet.tssCount = static_cast<uint32_t>(peState_.pendingTss.size());
        uint32_t tssIdx = 0;
        peState_.pendingTss.forEach([&](uint32_t stage, uint32_t state, uint32_t value) {
            packet.tss[tssIdx].stage = stage;
            packet.tss[tssIdx].type = state;
            packet.tss[tssIdx].value = value;
            ++tssIdx;
        });
        packet.samplerStateCount = static_cast<uint32_t>(peState_.pendingSamplerStates.size());
        uint32_t ssIdx = 0;
        peState_.pendingSamplerStates.forEach([&](uint32_t sampler, uint32_t state, uint32_t value) {
            packet.samplerStates[ssIdx].sampler = sampler;
            packet.samplerStates[ssIdx].type = state;
            packet.samplerStates[ssIdx].value = value;
            ++ssIdx;
        });
        // Phase 12: material + clip-plane deltas. Material rides as a
        // single struct + valid flag; clip planes ride as a 6-bit mask
        // + flat 6×4 float array (only set bits' slots are
        // semantically meaningful, but the array is fixed-size so the
        // packet layout stays simple).
        packet.materialValid = peState_.pendingMaterial ? 1u : 0u;
        packet.material = peState_.materialShadow;
        packet.clipPlaneMask = peState_.pendingClipPlaneMask;
        std::memcpy(packet.clipPlanes, peState_.clipPlaneShadow, sizeof(packet.clipPlanes));
        // Phase 12: Transform delta — drain pending transform table
        // (per-frame typically a handful: View, Projection, a few
        // World/Texture transforms). Cap check: > MAX_TRANSFORMS forces
        // chunk seal upstream.
        if (peState_.pendingTransforms.size() > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
            return false;
        }
        packet.transformCount = static_cast<uint32_t>(peState_.pendingTransforms.size());
        uint32_t txIdx = 0;
        peState_.pendingTransforms.forEach([&](uint32_t state, const D9CMatrix& matrix) {
            packet.transforms[txIdx].state = state;
            packet.transforms[txIdx].reserved = 0;
            packet.transforms[txIdx].matrix = matrix;
            ++txIdx;
        });
        // Phase 12: Light + LightEnable deltas. Light slot mask carries
        // the per-slot full D9CLight payload (set bit ⇒ lights[slot] is
        // semantically meaningful). LightEnable delta is two parallel
        // masks: ValidMask says "this slot has a fresh enable" and
        // LightEnableMask carries the new value.
        packet.lightSlotMask = peState_.pendingLightSlotMask;
        for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
            if ((peState_.pendingLightSlotMask & (1u << slot)) != 0) {
                packet.lights[slot] = peState_.lightShadow[slot];
            }
        }
        packet.lightEnableValidMask = peState_.pendingLightEnableValidMask;
        packet.lightEnableMask = peState_.pendingLightEnableMask;
        // Phase 16: full-snapshot mode — override every delta field with
        // the complete shadow snapshot. The importer applies whatever
        // valid bits are set, so flipping every bit + populating from
        // the existing PE shadow gives a self-contained packet without
        // requiring any importer changes. We respect the per-array caps;
        // a shadow that overflows (e.g. > 64 distinct render states)
        // returns false to force the chunk to seal.
        //
        // Triggered exclusively by DXMT9_PE_DRAW_FULL_SNAPSHOT=1 (see
        // dxmt9PeFullSnapshotEnabled() above for the env-flag contract
        // and equivalence guarantee). Branch is delta-vs-snapshot only —
        // both produce a D9CDrawPrimitivePacket with the same wire layout
        // (no schema change), and the unix-side applier in
        // device_c_chunk_replay.cpp::applyDrawPacketStateDirect() applies
        // either packet by the same valid/mask iteration.
        if (dxmt9PeFullSnapshotEnabled()) {
            // Render states: drain the entire shadow table.
            if (peState_.renderStateShadow.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
                return false;
            }
            packet.renderStateCount = 0;
            peState_.renderStateShadow.forEach([&](uint32_t state, uint32_t value) {
                auto& entry = packet.renderStates[packet.renderStateCount++];
                entry.state = state;
                entry.value = value;
            });
            // Texture / RT / Stream — set mask bits for every populated slot.
            packet.textureMask = 0;
            for (DWORD stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
                if (textures_[stage] != nullptr) {
                    packet.textureMask |= 1u << stage;
                    packet.textures[stage] = toWireHandle(rawTex(textures_[stage]));
                }
            }
            packet.streamSourceMask = 0;
            for (DWORD stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
                if (streamSrc_[stream] != nullptr) {
                    packet.streamSourceMask |= 1u << stream;
                    auto& s = packet.streamSources[stream];
                    s.buffer = toWireHandle(rawVBuf(streamSrc_[stream]));
                    s.offset = streamOff_[stream];
                    s.stride = streamStr_[stream];
                }
            }
            dxmt9::d3d9::pe::populateDrawPacketAttachmentSnapshot(
                packet, currentRtWireHandles(), currentRtExplicitMask(), true,
                toWireHandle(rawSurf(dsSurface_)));
            // Scalar valid bits: emit shadow contents unconditionally.
            packet.fvfValid = 1u;
            packet.fvf = fvf_;
            packet.vsValid = 1u;
            packet.vsHandle = toWireHandle(rawVS(vs_));
            packet.psValid = 1u;
            packet.psHandle = toWireHandle(rawPS(ps_));
            packet.vdeclValid = 1u;
            packet.vdeclHandle = toWireHandle(rawVD(vdecl_));
            packet.viewportValid = 1u;
            packet.viewport = peState_.viewportShadow;
            packet.scissorValid = 1u;
            packet.scissor = peState_.scissorShadow;
            // TSS / SamplerState — drain shadow tables fully.
            if (peState_.tssShadow.size() > D9C_DRAW_PACKET_MAX_TSS ||
                peState_.samplerStateShadow.size() > D9C_DRAW_PACKET_MAX_SAMPLER ||
                peState_.transformShadow.size() > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
                return false;
            }
            packet.tssCount = 0;
            peState_.tssShadow.forEach([&](uint32_t stage, uint32_t state, uint32_t value) {
                auto& e = packet.tss[packet.tssCount++];
                e.stage = stage;
                e.type = state;
                e.value = value;
            });
            packet.samplerStateCount = 0;
            peState_.samplerStateShadow.forEach([&](uint32_t sampler, uint32_t state, uint32_t value) {
                auto& e = packet.samplerStates[packet.samplerStateCount++];
                e.sampler = sampler;
                e.type = state;
                e.value = value;
            });
            packet.materialValid = 1u;
            packet.material = peState_.materialShadow;
            // Clip planes: emit every slot with mask = 0x3F (all 6).
            packet.clipPlaneMask = 0x3Fu;
            std::memcpy(packet.clipPlanes, peState_.clipPlaneShadow,
                        sizeof(packet.clipPlanes));
            // Transforms: drain shadow.
            packet.transformCount = 0;
            peState_.transformShadow.forEach([&](uint32_t state, const D9CMatrix& matrix) {
                auto& t = packet.transforms[packet.transformCount++];
                t.state = state;
                t.reserved = 0;
                t.matrix = matrix;
            });
            // Lights: emit every slot.
            packet.lightSlotMask = (1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u;
            for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
                packet.lights[i] = peState_.lightShadow[i];
            }
            packet.lightEnableValidMask = (1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u;
            packet.lightEnableMask = peState_.lightEnableShadow;
        }
        packet.primitiveType = static_cast<uint32_t>(type);
        packet.startVertex = startVertex;
        packet.primitiveCount = count;
        return true;
    }

    static UINT primitiveVertexCount(D3DPRIMITIVETYPE type, UINT primitiveCount) {
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

    static bool checkedByteCount(UINT count, UINT stride, std::uint32_t& bytes) {
        const auto value = static_cast<std::uint64_t>(count) * stride;
        if (value > 0xffffffffull) {
            return false;
        }
        bytes = static_cast<std::uint32_t>(value);
        return true;
    }

    // Draw records consume the effective server state, not only the handles
    // present in their delta packet. Capture these at append time so coarser
    // chunks can survive later Set* mutations and wrapper releases.
    bool appendCurrentlyBoundDrawHandles(WireHandleEntryList& handles,
                                         std::size_t firstHandle) {
        for (auto* tex : textures_) {
            if (auto* raw = rawTex(tex); raw != nullptr) {
                if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                        handles, firstHandle, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                        reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        for (auto* vb : streamSrc_) {
            if (auto* raw = rawVBuf(vb); raw != nullptr) {
                if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                        handles, firstHandle, D9C_CHUNK_HANDLE_KIND_BUFFER,
                        reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        if (auto* raw = rawIBuf(indexBuf_); raw != nullptr) {
            if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                    handles, firstHandle, D9C_CHUNK_HANDLE_KIND_BUFFER,
                    reinterpret_cast<uint64_t>(raw))) {
                return false;
            }
        }
        for (auto* surf : rtSlots_) {
            if (auto* raw = rawSurf(surf); raw != nullptr) {
                if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                        handles, firstHandle, D9C_CHUNK_HANDLE_KIND_SURFACE,
                        reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        if (auto* raw = rawSurf(dsSurface_); raw != nullptr) {
            if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                    handles, firstHandle, D9C_CHUNK_HANDLE_KIND_SURFACE,
                    reinterpret_cast<uint64_t>(raw))) {
                return false;
            }
        }
        // VS/PS/Vdecl have no pool retention table on the server side
        // (importer's markChunkResources skips SHADER / VERTEX_DECL
        // kinds), so emitting them here would be inert. Leaving them
        // out keeps the wire payload tight.
        return true;
    }

    bool appendCurrentlyBoundClearHandles(WireHandleEntryList& handles,
                                          std::size_t firstHandle,
                                          uint32_t flags) {
        if ((flags & D3DCLEAR_TARGET) != 0) {
            for (auto* surf : rtSlots_) {
                if (auto* raw = rawSurf(surf); raw != nullptr) {
                    if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                            handles, firstHandle, D9C_CHUNK_HANDLE_KIND_SURFACE,
                            reinterpret_cast<uint64_t>(raw))) {
                        return false;
                    }
                }
            }
        }
        if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0) {
            if (auto* raw = rawSurf(dsSurface_); raw != nullptr) {
                if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                        handles, firstHandle, D9C_CHUNK_HANDLE_KIND_SURFACE,
                        reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        return true;
    }

    bool collectAppendTimeExtraWireHandles(
        const D9CCommandChunkWireRecordHeader& wireRecord,
        const std::uint8_t* payload,
        WireHandleEntryList& handles,
        std::size_t firstHandle) {
        switch (wireRecord.type) {
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
            return appendCurrentlyBoundDrawHandles(handles, firstHandle);
        case D9C_COMMAND_RECORD_CLEAR: {
            if (!payload || wireRecord.payloadSize < sizeof(D9CCommandRecordClear)) {
                return false;
            }
            D9CCommandRecordClear decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendCurrentlyBoundClearHandles(
                handles, firstHandle, decoded.flags);
        }
        default:
            return true;
        }
    }

    void recordPeChunkCommit(PeRecorderFlushReason reason,
                             std::uint32_t recordCount,
                             std::uint32_t payloadBytes,
                             std::uint32_t handleCount,
                             std::uint32_t wireBytes) {
        ++peRecorderStats_.commitCount;
        peRecorderStats_.recordCountTotal += recordCount;
        peRecorderStats_.recordCountMax =
            std::max<std::uint64_t>(peRecorderStats_.recordCountMax, recordCount);
        peRecorderStats_.payloadBytesTotal += payloadBytes;
        peRecorderStats_.payloadBytesMax =
            std::max<std::uint64_t>(peRecorderStats_.payloadBytesMax, payloadBytes);
        peRecorderStats_.handleCountTotal += handleCount;
        peRecorderStats_.handleCountMax =
            std::max<std::uint64_t>(peRecorderStats_.handleCountMax, handleCount);
        const auto reasonIndex = static_cast<std::size_t>(reason);
        if (reasonIndex < peRecorderStats_.flushReasons.size()) {
            ++peRecorderStats_.flushReasons[reasonIndex];
        }
        if (dxmt9PeRecorderChunkLogEnabled()) {
            dxmt9DeviceInfoLog(
                "pe_recorder_chunk device=%p reason=%s commitCount=%llu "
                "recordCount=%u payloadBytes=%u handleCount=%u wireBytes=%u",
                this, peRecorderFlushReasonName(reason),
                static_cast<unsigned long long>(peRecorderStats_.commitCount),
                recordCount, payloadBytes, handleCount, wireBytes);
        }
    }

    void logPeRecorderStats(const char* event, bool force = false) {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return;
        }
        if (!force &&
            peRecorderStatsLastLoggedCommitCount_ == peRecorderStats_.commitCount) {
            return;
        }
        peRecorderStatsLastLoggedCommitCount_ = peRecorderStats_.commitCount;
        dxmt9DeviceInfoLog(
            "pe_recorder_stats event=%s device=%p commitCount=%llu "
            "recordCountTotal=%llu recordCountMax=%llu "
            "payloadBytesTotal=%llu payloadBytesMax=%llu "
            "handleCountTotal=%llu handleCountMax=%llu "
            "flushReasons{explicit=%llu capacityPre=%llu capacityPost=%llu "
            "barrier=%llu present=%llu readback=%llu reset=%llu "
            "stateblock=%llu child=%llu destructor=%llu stateMutation=%llu} "
            "up{drawPrimitiveUPCalls=%llu drawIndexedPrimitiveUPCalls=%llu "
            "vertexBytes=%llu indexBytes=%llu}",
            event ? event : "unknown", this,
            static_cast<unsigned long long>(peRecorderStats_.commitCount),
            static_cast<unsigned long long>(peRecorderStats_.recordCountTotal),
            static_cast<unsigned long long>(peRecorderStats_.recordCountMax),
            static_cast<unsigned long long>(peRecorderStats_.payloadBytesTotal),
            static_cast<unsigned long long>(peRecorderStats_.payloadBytesMax),
            static_cast<unsigned long long>(peRecorderStats_.handleCountTotal),
            static_cast<unsigned long long>(peRecorderStats_.handleCountMax),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Explicit)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::CapacityPre)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::CapacityPost)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Barrier)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Present)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Readback)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Reset)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::StateBlock)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Child)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Destructor)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::StateMutation)]),
            static_cast<unsigned long long>(peRecorderStats_.drawPrimitiveUPCalls),
            static_cast<unsigned long long>(peRecorderStats_.drawIndexedPrimitiveUPCalls),
            static_cast<unsigned long long>(peRecorderStats_.upVertexBytes),
            static_cast<unsigned long long>(peRecorderStats_.upIndexBytes));
    }

    void recordDrawPrimitiveUPCopy(std::uint32_t vertexBytes) {
        ++peRecorderStats_.drawPrimitiveUPCalls;
        peRecorderStats_.upVertexBytes += vertexBytes;
    }

    void recordDrawIndexedPrimitiveUPCopy(std::uint32_t vertexBytes,
                                          std::uint32_t indexBytes) {
        ++peRecorderStats_.drawIndexedPrimitiveUPCalls;
        peRecorderStats_.upVertexBytes += vertexBytes;
        peRecorderStats_.upIndexBytes += indexBytes;
    }

    HRESULT flushPendingCommandChunk(
        PeRecorderFlushReason reason = PeRecorderFlushReason::Explicit) {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        return commandChunk_.flush(
            reason,
            [this](PeRecorderFlushReason commitReason,
                   const D9CCommandChunk& chunk,
                   const PeCommandChunkCommitInfo& info) {
                const HRESULT hr = hr32(dxmt9c_device_commit_chunk(dev_, &chunk));
                if (SUCCEEDED(hr)) {
                    recordPeChunkCommit(commitReason, info.recordCount,
                                        info.payloadBytes, info.handleCount,
                                        info.wireBytes);
                }
                return hr;
            });
    }

    template<typename WriteFn>
    HRESULT appendCommandRecordDirect(uint32_t type, size_t bytes, WriteFn write) {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        return commandChunk_.appendRecordDirect(
            type, bytes, maxPendingCommandRecords(), maxPendingCommandBytes(),
            std::forward<WriteFn>(write),
            [this](const D9CCommandChunkWireRecordHeader& wireRecord,
                   const std::uint8_t* payload,
                   WireHandleEntryList& extraHandles,
                   std::size_t firstHandle) {
                return collectAppendTimeExtraWireHandles(
                    wireRecord, payload, extraHandles, firstHandle);
            },
            [this](PeRecorderFlushReason flushReason) {
                return flushPendingCommandChunk(flushReason);
            });
    }

    HRESULT appendCommandRecord(const void* data, size_t bytes) {
        if (!data) {
            return D3DERR_INVALIDCALL;
        }
        uint32_t type = 0;
        if (bytes >= sizeof(D9CCommandRecordHeader)) {
            D9CCommandRecordHeader legacyHeader{};
            std::memcpy(&legacyHeader, data, sizeof(legacyHeader));
            type = legacyHeader.type;
        }
        return appendCommandRecordDirect(
            type, bytes, [data, bytes](std::uint8_t* dst) {
                std::memcpy(dst, data, bytes);
            });
    }

    HRESULT appendCommandRecordRetained(const void* data,
                                        size_t bytes,
                                        D9CSurface* surface0 = nullptr,
                                        D9CSurface* surface1 = nullptr,
                                        D9CTexture* texture0 = nullptr,
                                        D9CTexture* texture1 = nullptr) {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        if (!data || bytes == 0 || bytes > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        if (commandChunk_.shouldFlushBeforeAppend(
                bytes, maxPendingCommandRecords(), maxPendingCommandBytes())) {
            const HRESULT flushHr =
                flushPendingCommandChunk(PeRecorderFlushReason::CapacityPre);
            if (FAILED(flushHr)) return flushHr;
        }

        D3D9PePendingCommandRetainer::Acquired acquired{};
        auto& retainer = commandChunk_.retainer();
        retainer.retainSurface(surface0, acquired);
        retainer.retainSurface(surface1, acquired);
        retainer.retainTexture(texture0, acquired);
        retainer.retainTexture(texture1, acquired);

        const auto recordCountBefore = commandChunk_.recordCount();
        const HRESULT hr = appendCommandRecord(data, bytes);
        if (FAILED(hr) && commandChunk_.recordCount() == recordCountBefore) {
            retainer.rollback(acquired);
        }
        return hr;
    }

    HRESULT appendDrawPrimitiveRecord(D3DPRIMITIVETYPE type, UINT startVertex, UINT count) {
        // Drain any accumulated const dirty ranges into chunk records FIRST,
        // so the chunk replays "consts → draw" in API order.
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawPrimitive record{};
        record.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
        record.header.size = sizeof(record);
        if (!buildDrawPrimitivePacket(type, startVertex, count, record.packet)) {
            return D3DERR_INVALIDCALL;
        }
        return appendCommandRecord(&record, sizeof(record));
    }

    HRESULT appendDrawIndexedPrimitiveRecord(D3DPRIMITIVETYPE type,
                                             INT baseVertex,
                                             UINT minVertex,
                                             UINT numVertices,
                                             UINT startIndex,
                                             UINT count) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawIndexedPrimitive record{};
        record.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
        record.header.size = sizeof(record);
        if (!buildDrawPrimitivePacket(type, 0, count, record.packet.state)) {
            return D3DERR_INVALIDCALL;
        }
        record.packet.baseVertex = baseVertex;
        record.packet.minVertex = minVertex;
        record.packet.numVertices = numVertices;
        record.packet.startIndex = startIndex;
        record.packet.primitiveCount = count;
        // Phase 12: index buffer delta. Server applies before
        // dxmt9c_device_draw_indexed_primitive.
        record.packet.ibValid = peState_.pendingIb ? 1u : 0u;
        record.packet.ibHandle = toWireHandle(rawIBuf(indexBuf_));
        peState_.pendingIb = false;
        return appendCommandRecord(&record, sizeof(record));
    }

    HRESULT appendDrawPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                        UINT count,
                                        const void* data,
                                        UINT stride) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawPrimitiveUP header{};
        header.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
        if (!buildDrawPrimitivePacket(type, 0, count, header.packet.state)) {
            return D3DERR_INVALIDCALL;
        }

        std::uint32_t vertexBytes = 0;
        if (!checkedByteCount(primitiveVertexCount(type, count), stride, vertexBytes) ||
            (vertexBytes != 0 && !data)) {
            return D3DERR_INVALIDCALL;
        }
        header.packet.primitiveCount = count;
        header.packet.stride = stride;
        header.packet.vertexDataOffset = sizeof(D9CCommandRecordDrawPrimitiveUP);
        header.packet.vertexDataSize = vertexBytes;
        header.header.size = sizeof(D9CCommandRecordDrawPrimitiveUP) + vertexBytes;

        const HRESULT hr = appendCommandRecordDirect(
            header.header.type, header.header.size,
            [&header, data, vertexBytes](std::uint8_t* record) {
                std::memcpy(record, &header, sizeof(header));
                if (vertexBytes != 0) {
                    std::memcpy(record + header.packet.vertexDataOffset, data, vertexBytes);
                }
            });
        if (SUCCEEDED(hr)) {
            recordDrawPrimitiveUPCopy(vertexBytes);
        }
        return hr;
    }

    HRESULT appendDrawIndexedPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                               UINT minVertex,
                                               UINT numVertices,
                                               UINT count,
                                               const void* indexData,
                                               D3DFORMAT indexFormat,
                                               const void* vertexData,
                                               UINT stride) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawIndexedPrimitiveUP header{};
        header.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
        if (!buildDrawPrimitivePacket(type, 0, count, header.packet.state)) {
            return D3DERR_INVALIDCALL;
        }

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

        header.packet.minVertex = minVertex;
        header.packet.numVertices = numVertices;
        header.packet.primitiveCount = count;
        header.packet.indexFormat = static_cast<std::uint32_t>(indexFormat);
        header.packet.stride = stride;
        header.packet.indexDataOffset = sizeof(D9CCommandRecordDrawIndexedPrimitiveUP);
        header.packet.indexDataSize = indexBytes;
        header.packet.vertexDataOffset = header.packet.indexDataOffset + indexBytes;
        header.packet.vertexDataSize = vertexBytes;
        header.header.size = sizeof(D9CCommandRecordDrawIndexedPrimitiveUP) +
                             indexBytes + vertexBytes;

        const HRESULT hr = appendCommandRecordDirect(
            header.header.type, header.header.size,
            [&header, indexData, indexBytes, vertexData, vertexBytes](std::uint8_t* record) {
                std::memcpy(record, &header, sizeof(header));
                if (indexBytes != 0) {
                    std::memcpy(record + header.packet.indexDataOffset, indexData, indexBytes);
                }
                if (vertexBytes != 0) {
                    std::memcpy(record + header.packet.vertexDataOffset, vertexData, vertexBytes);
                }
            });
        if (SUCCEEDED(hr)) {
            recordDrawIndexedPrimitiveUPCopy(vertexBytes, indexBytes);
        }
        return hr;
    }

    HRESULT flushPeRecorder(
        PeRecorderFlushReason reason = PeRecorderFlushReason::Barrier) {
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
        return flushPendingCommandChunk(reason);
    }

    // Variable-size const-array record append. The record is
    // header + (start, count) + count * elemSize bytes of payload.
    // Caller must supply the matching D9C_COMMAND_RECORD_SET_*_CONST_*
    // type tag; decoder will validate header.size against count*elemSize.
    HRESULT appendSetConstRecord(uint32_t recordType, UINT start, UINT count,
                                 const void* data, std::size_t elemSize) {
        const std::uint64_t payload64 = static_cast<std::uint64_t>(count) * elemSize;
        if (payload64 > 0xffffffffull - sizeof(D9CCommandRecordSetConst)) {
            return D3DERR_INVALIDCALL;
        }
        const std::uint32_t payloadBytes = static_cast<std::uint32_t>(payload64);
        if (payloadBytes != 0 && !data) {
            return D3DERR_INVALIDCALL;
        }

        D9CCommandRecordSetConst header{};
        header.header.type = recordType;
        header.header.size = static_cast<std::uint32_t>(sizeof(header) + payloadBytes);
        header.start = start;
        header.count = count;

        return appendCommandRecordDirect(
            header.header.type, header.header.size,
            [&header, data, payloadBytes](std::uint8_t* record) {
                std::memcpy(record, &header, sizeof(header));
                if (payloadBytes != 0) {
                    std::memcpy(record + sizeof(header), data, payloadBytes);
                }
            });
    }

    // Emit one record covering the merged dirty range, then clear it.
    HRESULT flushConstShadow(ConstShadow& shadow, uint32_t recordType, std::size_t elemSize) {
        if (!shadow.dirty()) return S_OK;
        const uint32_t start = shadow.dirtyStart;
        const uint32_t count = shadow.dirtyEnd - shadow.dirtyStart;
        const auto* data = shadow.values.data() + static_cast<std::size_t>(start) * elemSize;
        const HRESULT hr = appendSetConstRecord(recordType, start, count, data, elemSize);
        if (SUCCEEDED(hr)) {
            shadow.clear();
        }
        return hr;
    }

    // Drain all 6 const shadows. Called before each appended Draw record
    // and at chunk flush so the chunk's record stream replays
    // constants → draw in API order.
    HRESULT flushPendingConsts() {
        HRESULT hr = flushConstShadow(peConsts_.vsConstF, D9C_COMMAND_RECORD_SET_VS_CONST_F, sizeof(float) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.vsConstI, D9C_COMMAND_RECORD_SET_VS_CONST_I, sizeof(int32_t) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.vsConstB, D9C_COMMAND_RECORD_SET_VS_CONST_B, sizeof(uint32_t));
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.psConstF, D9C_COMMAND_RECORD_SET_PS_CONST_F, sizeof(float) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.psConstI, D9C_COMMAND_RECORD_SET_PS_CONST_I, sizeof(int32_t) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.psConstB, D9C_COMMAND_RECORD_SET_PS_CONST_B, sizeof(uint32_t));
        if (FAILED(hr)) return hr;
        return S_OK;
    }

    // Phase 28: chunk-mode barrier flush. Replaces flushPendingHotState's
    // bridge-emit path with a chunk-record path that preserves the
    // "Set* never crosses PE/unix in default chunk mode" invariant.
    //
    // Drains pending consts (existing per-record stream) THEN, if hot
    // state is pending, packages the delta into a D9C_COMMAND_RECORD_
    // APPLY_STATE record + appends to the chunk + clears the pending
    // bits. Server importer dispatches APPLY_STATE via the same
    // applyDrawPacketState() that draw records use, so the server
    // shadow is updated before the upcoming barrier record runs.
    //
    // Caller still appends the actual barrier record afterwards;
    // chunk-commit flushes everything in the recorded order.
    HRESULT chunkBarrierFlush() {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        if (!hasPendingHotState()) {
            return S_OK;
        }
        D9CCommandRecordApplyState record{};
        record.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
        record.header.size = sizeof(record);
        // Fast path: single APPLY_STATE record covers all pending
        // state. After Phase 31 cap-checks at every Set* fast path,
        // this is the only path that runs in practice.
        if (buildDrawPrimitivePacket(D3DPT_POINTLIST, 0, 0, record.packet)) {
            const HRESULT appendHr = appendCommandRecord(&record, sizeof(record));
            if (FAILED(appendHr)) return appendHr;
            clearPendingHotState();
            return S_OK;
        }
        // Over-cap slow path: a Set* somewhere bypassed the cap check
        // (regression). Drain pending oversized collections in batches
        // of cap-size records. Critical safety property: every pending
        // state bit MUST be represented in the chunk before the caller
        // appends a barrier record. Sealing-and-deferring (the prior
        // behavior) lets the barrier observe stale server state.
        return drainOversizedPendingStateAsApplyStateRecords();
    }

    HRESULT drainOversizedPendingStateAsApplyStateRecords() {
        // Drain the four cappable collections (renderStates, tss,
        // samplerStates, transforms) in batches of cap-size. Each batch
        // becomes one APPLY_STATE record carrying ONLY that collection's
        // batch (other fields zero / unset). Server's applyDrawPacketState
        // is idempotent for unset fields so empty validX/maskX are safe.
        auto drainTable = [&](auto& pendingTable, auto cap, auto fillEntry,
                              auto packetCountField) -> HRESULT {
            while (!pendingTable.empty()) {
                D9CCommandRecordApplyState rec{};
                rec.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
                rec.header.size = sizeof(rec);
                std::uint32_t n = 0;
                uint32_t key = 0;
                uint32_t value = 0;
                while (n < cap && pendingTable.popFirst(key, value)) {
                    fillEntry(rec.packet, n, key, value);
                    ++n;
                }
                packetCountField(rec.packet) = n;
                const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        auto drainTransformTable = [&](auto& pendingTable, auto cap, auto fillEntry,
                                       auto packetCountField) -> HRESULT {
            while (!pendingTable.empty()) {
                D9CCommandRecordApplyState rec{};
                rec.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
                rec.header.size = sizeof(rec);
                std::uint32_t n = 0;
                uint32_t key = 0;
                D9CMatrix value{};
                while (n < cap && pendingTable.popFirst(key, value)) {
                    fillEntry(rec.packet, n, key, value);
                    ++n;
                }
                packetCountField(rec.packet) = n;
                const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        auto drainMatrix = [&](auto& pendingMatrix, auto cap, auto fillEntry,
                               auto packetCountField) -> HRESULT {
            while (!pendingMatrix.empty()) {
                D9CCommandRecordApplyState rec{};
                rec.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
                rec.header.size = sizeof(rec);
                std::uint32_t n = 0;
                uint32_t row = 0;
                uint32_t key = 0;
                uint32_t value = 0;
                while (n < cap && pendingMatrix.popFirst(row, key, value)) {
                    fillEntry(rec.packet, n, row, key, value);
                    ++n;
                }
                packetCountField(rec.packet) = n;
                const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        if (auto hr = drainTable(peState_.pendingRenderStates,
                                 (uint32_t)D9C_DRAW_PACKET_MAX_RENDER_STATES,
                                 [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                    uint32_t k, uint32_t v) {
                                     p.renderStates[i].state = k;
                                     p.renderStates[i].value = v;
                                 },
                                 [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                     return p.renderStateCount;
                                 });
            FAILED(hr)) return hr;
        if (auto hr = drainMatrix(peState_.pendingTss,
                                  (uint32_t)D9C_DRAW_PACKET_MAX_TSS,
                                  [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                     uint32_t row, uint32_t k, uint32_t v) {
                                      p.tss[i].stage = row;
                                      p.tss[i].type = k;
                                      p.tss[i].value = v;
                                  },
                                  [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                      return p.tssCount;
                                  });
            FAILED(hr)) return hr;
        if (auto hr = drainMatrix(peState_.pendingSamplerStates,
                                  (uint32_t)D9C_DRAW_PACKET_MAX_SAMPLER,
                                  [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                     uint32_t row, uint32_t k, uint32_t v) {
                                      p.samplerStates[i].sampler = row;
                                      p.samplerStates[i].type = k;
                                      p.samplerStates[i].value = v;
                                  },
                                  [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                      return p.samplerStateCount;
                                  });
            FAILED(hr)) return hr;
        if (auto hr = drainTransformTable(peState_.pendingTransforms,
                                          (uint32_t)D9C_DRAW_PACKET_MAX_TRANSFORMS,
                                          [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                             uint32_t k, const D9CMatrix& v) {
                                              p.transforms[i].state = k;
                                              p.transforms[i].reserved = 0;
                                              p.transforms[i].matrix = v;
                                          },
                                          [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                              return p.transformCount;
                                          });
            FAILED(hr)) return hr;
        // Remaining scalar pending bits (texture / stream / vs / ps /
        // vdecl / RT / DS / viewport / scissor / fvf / material / clip
        // / lights / lightEnable) all fit in one packet. After draining
        // the four cappable collections above, buildDrawPrimitivePacket
        // succeeds.
        if (!hasPendingHotState()) {
            return S_OK;
        }
        D9CCommandRecordApplyState tail{};
        tail.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
        tail.header.size = sizeof(tail);
        if (!buildDrawPrimitivePacket(D3DPT_POINTLIST, 0, 0, tail.packet)) {
            // Truly should never happen — the four cappable collections
            // are now empty. Defensive: log + return failure rather than
            // silently leaving pending state dirty (which would let the
            // upcoming barrier observe stale server state).
            dxmt9DeviceDebugLog(
                "ERR: drainOversizedPendingStateAsApplyStateRecords could "
                "not build tail APPLY_STATE — pending state lost. Caller "
                "should treat as recorder failure.");
            return D3DERR_INVALIDCALL;
        }
        const HRESULT hr = appendCommandRecord(&tail, sizeof(tail));
        if (FAILED(hr)) return hr;
        clearPendingHotState();
        return S_OK;
    }

public:
    HRESULT FlushPeRecorderForChild() noexcept override {
        return flushPeRecorder(PeRecorderFlushReason::Child);
    }
    bool IsStateBlockRecordingForChild() const noexcept override {
        return stateBlockRecording_;
    }
    void InvalidateStateBlockShadowForChild() noexcept override {
        peState_.renderStateShadow.clear();
        peState_.transformShadow.clear();
        clearPendingHotState();
    }
    void AddDefaultPoolResourceRefForChild() noexcept override {
        ++defaultPoolResourceRefs_;
    }
    void ReleaseDefaultPoolResourceRefForChild() noexcept override {
        if (defaultPoolResourceRefs_ != 0) {
            --defaultPoolResourceRefs_;
        }
    }
    bool IsChunkRecorderEnabledForChild() const override {
        return true;
    }
    HRESULT AppendRecordForChild(const void* data, size_t bytes) noexcept override {
        return appendCommandRecord(data, bytes);
    }

    // PE-shadow stateblock support.
    //
    // Initial snapshot mode (called from D3D9StateBlockImpl ctor inside
    // EndStateBlock, when `stateBlockTransformRecorded` is still populated):
    // copy the recorded transform set into `out.transforms` to fix the
    // *tracked keys* — those will be the only transforms the stateblock
    // replays on Apply. Keys touched only by MultiplyTransform are absent
    // from this table (wined3d quirk), so they will not be captured or
    // replayed. The shader-constant register files and the bound vdecl
    // mirror whatever the device shadow currently holds.
    //
    // Refresh mode (called from D3D9StateBlockImpl::Capture, post-End,
    // when `stateBlockTransformRecorded` is empty): re-read the value of
    // each already-tracked key from the live `transformShadow`. The set
    // of tracked keys is FIXED at End; mid-game Captures only refresh
    // values, never add keys. Shader-constant register files and vdecl
    // are re-snapshotted in full.
    void CaptureStateBlockShadowForChild(D3D9StateBlockShadow& out) override {
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

    D3D9DeviceImpl(D9CDevice* dev, IDirect3D9Ex* factory,
                   UINT adapter, D3DDEVTYPE deviceType, DWORD behaviorFlags,
                   HWND window, bool extended,
                   DWORD implicitSwapchainFlags)
        : dev_(dev), factory_(factory)
        , adapter_(adapter), deviceType_(deviceType), behaviorFlags_(behaviorFlags)
        , softwareVertexProcessing_((behaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? TRUE : FALSE)
        , extended_(extended)
        , creationWindow_(window)
        , implicitSwapchainFlagsShadow_(implicitSwapchainFlags) {
        if (factory_) factory_->AddRef();
        for (UINT& freq : streamFreq_) {
            freq = 1;
        }
        // T2: Initialize viewport/scissor PE shadow from the implicit
        // swapchain's back-buffer rect so GetViewport/GetScissorRect
        // round-trip correctly before any Set call (Wine conformance:
        // test_viewport_scissor_state_getters). Mirrors the Reset()
        // / ResetEx() block at lines ~1601 / ~3154.
        if (dev_) {
            D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, 0);
            if (chain) {
                D9CPresentParams cpp{};
                if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(chain, &cpp)))) {
                    const uint32_t w = std::max<uint32_t>(1u, cpp.backBufferWidth);
                    const uint32_t h = std::max<uint32_t>(1u, cpp.backBufferHeight);
                    peState_.viewportShadow = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
                    peState_.scissorShadow  = D9CRect{0, 0, (int32_t)w, (int32_t)h};
                }
                dxmt9c_swapchain_release(chain);
            }
        }
        initGammaRampIdentity();
        dxmt9DeviceDebugLog("device_ctor this=%p dev=%p factory=%p adapter=%u devType=%u behavior=0x%x window=%p extended=%u",
                            this, static_cast<void*>(dev_), static_cast<void*>(factory_),
                            adapter_, (unsigned)deviceType_, (unsigned)behaviorFlags_, window, extended_ ? 1u : 0u);
    }

    ~D3D9DeviceImpl() {
        (void)flushPeRecorder(PeRecorderFlushReason::Destructor);
        logPeRecorderStats("destructor", true);
        clearPendingCommandChunk();
        releaseAllBound();
        dxmt9c_device_release(dev_);
        if (factory_) factory_->Release();
    }

    /* ── IUnknown ── */

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)          ||
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
        *ppv = nullptr; return E_NOINTERFACE;
    }

    /* ── device info ── */

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() noexcept override {
        dxmt9DeviceDebugLog("device_test_cooperative_level device=%p", this);
        if (deviceNotReset_) {
            dxmt9DeviceDebugLog("device_test_cooperative_level -> device not reset");
            return D3DERR_DEVICENOTRESET;
        }
        const HRESULT hr = hr32(dxmt9c_device_test_cooperative_level(dev_));
        dxmt9DeviceDebugLog("device_test_cooperative_level -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() noexcept override {
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
    HRESULT STDMETHODCALLTYPE EvictManagedResources() noexcept override {
        // stub: Wine returns S_OK; Apple GPUs have unified memory, manual eviction is not exposed.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D) noexcept override {
        if (!ppD3D) return D3DERR_INVALIDCALL;
        factory_->AddRef();
        *ppD3D = static_cast<IDirect3D9*>(factory_);
        dxmt9DeviceDebugLog("device_get_direct3d this=%p -> factory=%p", this, static_cast<void*>(*ppD3D));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) noexcept override {
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

    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT sc, D3DDISPLAYMODE* pMode) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_display_mode device=%p sc=%u", this, sc);
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, sc);
        if (!chain) {
            return D3DERR_INVALIDCALL;
        }
        D9CPresentParams cpp{};
        const HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(chain, &cpp));
        dxmt9c_swapchain_release(chain);
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

    HRESULT STDMETHODCALLTYPE GetCreationParameters(
            D3DDEVICE_CREATION_PARAMETERS* pParams) noexcept override {
        if (!pParams) return D3DERR_INVALIDCALL;
        pParams->AdapterOrdinal  = adapter_;
        pParams->DeviceType      = deviceType_;
        pParams->hFocusWindow    = creationWindow_;
        pParams->BehaviorFlags   = behaviorFlags_;
        return S_OK;
    }

    /* ── cursor (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* surface) noexcept override {
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
    void    STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) noexcept override {
        // stub: Wine returns S_OK; cursor positioning belongs to the WindowServer / window manager,
        // the app's hint is informational.
        dxmt9DeviceDebugLog("device_set_cursor_position device=%p x=%d y=%d flags=0x%x",
                            this, x, y, (unsigned)flags);
    }
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL show) noexcept override {
        dxmt9DeviceDebugLog("device_show_cursor device=%p show=%u", this, (unsigned)show);
        if (!cursorSurfaceSet_) {
            return FALSE;
        }
        const BOOL previous = cursorVisible_ ? TRUE : FALSE;
        cursorVisible_ = show ? true : false;
        return previous;
    }

    /* ── swap chains ── */

    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(
            D3DPRESENT_PARAMETERS* pPP, IDirect3DSwapChain9** ppSC) noexcept override {
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

    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index,
                                            IDirect3DSwapChain9** ppSC) noexcept override {
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

    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() noexcept override {
        return dxmt9c_device_get_swap_chain_count(dev_);
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPP) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
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
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        // T2 device-lost gate: render-path methods must early-return
        // D3DERR_DEVICELOST while the device awaits Reset.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_present device=%p wnd=%p src=%s dst=%s dirty=%p",
                            this, wnd,
                            src ? "<custom>" : "<full>",
                            dst ? "<custom>" : "<full>",
                            dirty);
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        // Recorder-design Present: append a PRESENT record to the
        // current chunk after draining hot state + const dirty ranges,
        // then commit the chunk synchronously. The server-side
        // importer dispatches dxmt9c_device_present after replaying
        // every preceding draw / clear / state in the chunk — so
        // ordering is preserved and Present serves as the natural
        // chunk boundary. Dirty-region payload is dropped (the
        // backend present path doesn't consume it).
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;

        D9CCommandRecordPresent record{};
        record.header.type = D9C_COMMAND_RECORD_PRESENT;
        record.header.size = sizeof(record);
        record.hwnd = (uint64_t)(uintptr_t)wnd;
        record.flags = 0;
        record.hasSrc = src ? 1u : 0u;
        record.hasDst = dst ? 1u : 0u;
        if (src) record.src = cs;
        if (dst) record.dst = cd;
        const HRESULT appendHr = appendCommandRecord(&record, sizeof(record));
        if (FAILED(appendHr)) return appendHr;
        // Force-commit so Present runs at the bridge boundary even
        // if the chunk is below the byte/record threshold.
        const HRESULT flushHr = flushPendingCommandChunk(PeRecorderFlushReason::Present);
        if (SUCCEEDED(flushHr)) {
            logPeRecorderStats("present");
        }
        return flushHr;
    }

    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT idx,
                                             D3DBACKBUFFER_TYPE type,
                                             IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        dxmt9DeviceDebugLog("device_get_back_buffer device=%p sc=%u idx=%u", this, sc, idx);
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, sc);
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
                dxmt9c_swapchain_release(chain);
                return D3DERR_INVALIDCALL;
            }
        }
        D9CSurface* s = dxmt9c_swapchain_get_back_buffer(chain, idx, 0);
        if (!s) {
            dxmt9c_swapchain_release(chain);
            return D3DERR_INVALIDCALL;
        }
        // Release the C-side handles we hold; the cached swap-chain
        // wrapper owns its own retained references and the eventual
        // PE surface wrapper holds its own ref via CreatePeSurface.
        dxmt9c_surface_release(s);
        dxmt9c_swapchain_release(chain);
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

    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT swapChain, D3DRASTER_STATUS* p) noexcept override {
        if (!p || swapChain != 0) {
            return D3DERR_INVALIDCALL;
        }
        // Synthesize a monotonically-advancing ScanLine so apps that VBlank-poll do
        // not spin forever. dxmt9 has no real per-line vblank signal from Metal;
        // the helper takes a per-call counter and the current backbuffer height.
        static std::atomic<uint64_t> rasterTick{0};
        uint32_t displayHeight = 0;
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, swapChain);
        if (chain) {
            D9CPresentParams cpp{};
            if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(chain, &cpp)))) {
                displayHeight = cpp.backBufferHeight;
            }
            dxmt9c_swapchain_release(chain);
        }
        const auto tick = rasterTick.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto est = ::dxmt9::d3d9::computeRasterStatusEstimate(tick, displayHeight);
        memset(p, 0, sizeof(*p));
        p->ScanLine = est.scanLine;
        p->InVBlank = est.inVBlank ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enableDialogs) noexcept override {
        dxmt9DeviceDebugLog("device_set_dialog_box_mode device=%p enable=%u", this, (unsigned)enableDialogs);
        // stub: Wine `wined3d_device_set_dialog_box_mode` returns WINED3D_OK
        // unconditionally — dialog-box mode requires Win32 user32/dwm primitives
        // that don't exist on macOS, but matching the Wine S_OK contract keeps
        // the conformance manifest aligned. Toggling has no observable effect.
        return S_OK;
    }
    // SetGammaRamp / GetGammaRamp — G2-B PE shadow + Option A unix push.
    // D3D9 returns void; Wine wined3d ignores unknown flag bits ("FIXME:
    // Ignoring flags") so we accept any flags value as opaque. iSwapChain
    // is unused for the shadow since there is no error channel on the
    // void-return D3D9 contract — the per-output forwarding in wined3d
    // is observationally invisible to the caller. SetGammaRamp also
    // pushes the payload through the D9C bridge to core::Device so the
    // unix-side Presenter can apply it on the next Present without a
    // second ABI roundtrip per frame.
    void    STDMETHODCALLTYPE SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp) noexcept override {
        dxmt9DeviceDebugLog("device_set_gamma_ramp device=%p swapChain=%u flags=0x%x ramp=%p",
                            this, swapChain, (unsigned)flags,
                            static_cast<const void*>(ramp));
        if (!ramp) return;
        // Byte-copy: D3DGAMMARAMP is a POD (3 * 256 * WORD). sizeof
        // is the safe shape regardless of any future struct growth.
        std::memcpy(&gammaRamp_, ramp, sizeof(D3DGAMMARAMP));
        if (dev_) {
            dxmt9c_device_set_gamma_ramp(dev_, reinterpret_cast<const uint16_t*>(ramp));
        }
    }
    void    STDMETHODCALLTYPE GetGammaRamp(UINT swapChain, D3DGAMMARAMP* p) noexcept override {
        dxmt9DeviceDebugLog("device_get_gamma_ramp device=%p swapChain=%u out=%p",
                            this, swapChain, static_cast<void*>(p));
        if (!p) return;
        std::memcpy(p, &gammaRamp_, sizeof(D3DGAMMARAMP));
    }

    /* ── resource creation ── */

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels,
                                             DWORD usage, D3DFORMAT fmt,
                                             D3DPOOL pool,
                                             IDirect3DTexture9** ppTex,
                                             HANDLE* psh) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        *ppTex = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
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
        D9CTexture* t = dxmt9c_device_create_texture(dev_, w, h, levels,
                                                      usage, (uint32_t)fmt,
                                                      (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = CreatePeTexture(t, this, this, userPtr, userPitch);
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

    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d,
                                                   UINT levels, DWORD usage,
                                                   D3DFORMAT fmt, D3DPOOL pool,
                                                   IDirect3DVolumeTexture9** ppTex,
                                                   HANDLE* psh) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        *ppTex = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        // Wine D3D9Ex contract: see CreateTexture above — MANAGED pool
        // is rejected outright on Ex devices.
        if (extended_ && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
        if (FAILED(sharedHr)) return sharedHr;
        dxmt9DeviceDebugLog("device_create_volume_texture device=%p size=%ux%ux%u levels=%u usage=0x%x fmt=%u pool=%u",
                            this, w, h, d, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
        D9CTexture* t = dxmt9c_device_create_volume_texture(dev_, w, h, d, levels,
                                                             usage, (uint32_t)fmt,
                                                             (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = CreatePeVolumeTexture(t, this, this);
        dxmt9DeviceDebugLog("device_create_volume_texture -> texture=%p", *ppTex);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT size, UINT levels,
                                                 DWORD usage, D3DFORMAT fmt,
                                                 D3DPOOL pool,
                                                 IDirect3DCubeTexture9** ppTex,
                                                 HANDLE* psh) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        *ppTex = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        // Wine D3D9Ex contract: see CreateTexture above — MANAGED pool
        // is rejected outright on Ex devices.
        if (extended_ && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
        if (FAILED(sharedHr)) return sharedHr;
        dxmt9DeviceDebugLog("device_create_cube_texture device=%p size=%u levels=%u usage=0x%x fmt=%u pool=%u",
                            this, size, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
        D9CTexture* t = dxmt9c_device_create_cube_texture(dev_, size, levels,
                                                           usage, (uint32_t)fmt,
                                                           (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = CreatePeCubeTexture(t, this, this);
        dxmt9DeviceDebugLog("device_create_cube_texture -> texture=%p", *ppTex);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage,
                                                  DWORD fvf, D3DPOOL pool,
                                                  IDirect3DVertexBuffer9** ppBuf,
                                                  HANDLE* psh) noexcept override {
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
        D9CBuffer* b = dxmt9c_device_create_vertex_buffer(dev_, len, usage,
                                                           fvf, (uint32_t)pool);
        if (!b) return D3DERR_INVALIDCALL;
        *ppBuf = CreatePeVertexBuffer(b, this, this);
        dxmt9DeviceDebugLog("device_create_vertex_buffer -> buffer=%p", *ppBuf);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage,
                                                 D3DFORMAT fmt, D3DPOOL pool,
                                                 IDirect3DIndexBuffer9** ppBuf,
                                                 HANDLE* psh) noexcept override {
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
        D9CBuffer* b = dxmt9c_device_create_index_buffer(dev_, len, usage,
                                                          (uint32_t)fmt,
                                                          (uint32_t)pool);
        if (!b) return D3DERR_INVALIDCALL;
        *ppBuf = CreatePeIndexBuffer(b, this, this);
        dxmt9DeviceDebugLog("device_create_index_buffer -> buffer=%p", *ppBuf);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt,
                                                  D3DMULTISAMPLE_TYPE ms,
                                                  DWORD msQual, BOOL lockable,
                                                  IDirect3DSurface9** ppS,
                                                  HANDLE* psh) noexcept override {
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
        D9CSurface* s = dxmt9c_device_create_render_target(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)ms, msQual,
                                                            lockable ? 1u : 0u, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = CreatePeSurface(s, this, nullptr, this);
        dxmt9DeviceDebugLog("device_create_render_target -> surface=%p", *ppS);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h,
                                                         D3DFORMAT fmt,
                                                         D3DMULTISAMPLE_TYPE ms,
                                                         DWORD msQual,
                                                         BOOL discard,
                                                         IDirect3DSurface9** ppS,
                                                         HANDLE* psh) noexcept override {
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
        uint64_t sh = 0;
        D9CSurface* s = dxmt9c_device_create_depth_stencil(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)ms, msQual,
                                                            discard ? 1u : 0u, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = CreatePeSurface(s, this, nullptr, this);
        dxmt9DeviceDebugLog("device_create_depth_stencil_surface -> surface=%p", *ppS);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src,
                                             const RECT* srcRect,
                                             IDirect3DSurface9* dst,
                                             const POINT* dstPt) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
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
        auto* const srcRaw = rawSurf(src);
        auto* const dstRaw = rawSurf(dst);
        D9CCommandRecordUpdateSurface record{};
        record.header.type = D9C_COMMAND_RECORD_UPDATE_SURFACE;
        record.header.size = sizeof(record);
        record.srcWire = reinterpret_cast<uint64_t>(srcRaw);
        record.dstWire = reinterpret_cast<uint64_t>(dstRaw);
        record.hasSrcRect = srcRect ? 1u : 0u;
        record.hasDstPoint = dstPt ? 1u : 0u;
        record.srcRect = cs;
        record.dstPoint = cd;
        return appendCommandRecordRetained(&record, sizeof(record),
                                           srcRaw, dstRaw);
    }

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,
                                             IDirect3DBaseTexture9* dst) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        // Wine d3d9 IDirect3DDevice9::UpdateTexture: both args non-NULL;
        // src must be SYSTEMMEM; dst must NOT be SYSTEMMEM/SCRATCH. See
        // test_update_texture_pool_copy_2d in d3d9_conformance_resource.c.
        if (!src || !dst) return D3DERR_INVALIDCALL;
        if (src->GetType() == D3DRTYPE_TEXTURE && dst->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC sd{}, dd{};
            ((IDirect3DTexture9*)src)->GetLevelDesc(0, &sd);
            ((IDirect3DTexture9*)dst)->GetLevelDesc(0, &dd);
            if (sd.Pool != D3DPOOL_SYSTEMMEM) return D3DERR_INVALIDCALL;
            if (dd.Pool == D3DPOOL_SYSTEMMEM || dd.Pool == D3DPOOL_SCRATCH)
                return D3DERR_INVALIDCALL;
        }
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
        auto* const srcRaw = rawTex(src);
        auto* const dstRaw = rawTex(dst);
        D9CCommandRecordUpdateTexture record{};
        record.header.type = D9C_COMMAND_RECORD_UPDATE_TEXTURE;
        record.header.size = sizeof(record);
        record.srcWire = reinterpret_cast<uint64_t>(srcRaw);
        record.dstWire = reinterpret_cast<uint64_t>(dstRaw);
        // Wine d3d9 UpdateTexture: both args non-NULL; src in SYSTEMMEM;
        // dst not SYSTEMMEM/SCRATCH. test_update_texture_pool_copy_2d.
        (void)0;
        return appendCommandRecordRetained(&record, sizeof(record),
                                           nullptr, nullptr, srcRaw, dstRaw);
    }

    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt,
                                                   IDirect3DSurface9* dst) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
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
        auto* const rtRaw = rawSurf(rt);
        auto* const dstRaw = rawSurf(dst);
        D9CCommandRecordReadback record{};
        record.header.type = D9C_COMMAND_RECORD_READBACK;
        record.header.size = sizeof(record);
        record.srcWire = reinterpret_cast<uint64_t>(rtRaw);
        record.dstWire = reinterpret_cast<uint64_t>(dstRaw);
        const HRESULT appendHr = appendCommandRecordRetained(&record, sizeof(record),
                                                             rtRaw, dstRaw);
        if (FAILED(appendHr)) return appendHr;
        // Sync semantics: commit the chunk now and wait for completion.
        // flushPendingCommandChunk routes through commit_chunk -> server's
        // record dispatcher -> readback record handler.
        return flushPendingCommandChunk(PeRecorderFlushReason::Readback);
    }

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sc, IDirect3DSurface9* surface) noexcept override {
        dxmt9DeviceDebugLog("device_get_front_buffer_data device=%p sc=%u surface=%p",
                            this, sc, surface);
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src,
                                           const RECT* srcRect,
                                           IDirect3DSurface9* dst,
                                           const RECT* dstRect,
                                           D3DTEXTUREFILTERTYPE filter) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
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
        auto* const srcRaw = rawSurf(src);
        auto* const dstRaw = rawSurf(dst);
        D9CCommandRecordStretchRect record{};
        record.header.type = D9C_COMMAND_RECORD_STRETCH_RECT;
        record.header.size = sizeof(record);
        record.srcWire = reinterpret_cast<uint64_t>(srcRaw);
        record.dstWire = reinterpret_cast<uint64_t>(dstRaw);
        record.hasSrcRect = srcRect ? 1u : 0u;
        record.hasDstRect = dstRect ? 1u : 0u;
        record.filter = (uint32_t)filter;
        if (srcRect) record.srcRect = cs;
        if (dstRect) record.dstRect = cd;
        return appendCommandRecordRetained(&record, sizeof(record),
                                           srcRaw, dstRaw);
    }

    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurf,
                                         const RECT* pRect,
                                         D3DCOLOR color) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
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
        auto* const surfRaw = rawSurf(pSurf);
        D9CCommandRecordColorFill record{};
        record.header.type = D9C_COMMAND_RECORD_COLOR_FILL;
        record.header.size = sizeof(record);
        record.surfaceWire = reinterpret_cast<uint64_t>(surfRaw);
        record.colorARGB = (uint32_t)color;
        record.hasRect = pRect ? 1u : 0u;
        if (pRect) record.rect = cr;
        return appendCommandRecordRetained(&record, sizeof(record), surfRaw);
    }

    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DPOOL pool,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE* psh) noexcept override {
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
        D9CSurface* s = dxmt9c_device_create_offscreen_surface(dev_, w, h,
                                                                (uint32_t)fmt,
                                                                (uint32_t)pool, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = CreatePeSurface(s, this, nullptr, this, true, userPtr, userPitch);
        dxmt9DeviceDebugLog("device_create_offscreen_surface -> surface=%p", *ppS);
        return S_OK;
    }

    /* ── render targets ── */

    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD idx,
                                               IDirect3DSurface9* pSurf) noexcept override {
        dxmt9DeviceDebugLog("device_set_render_target device=%p idx=%u surf=%p",
                            this, (unsigned)idx, pSurf);
        if (idx >= 4) return D3DERR_INVALIDCALL;
        // render_target_device_mismatch: a surface created by a different
        // device cannot be bound. Compare via GetDevice; it AddRef's, so
        // Release the borrowed pointer immediately.
        if (pSurf) {
            IDirect3DDevice9* owner = nullptr;
            if (SUCCEEDED(pSurf->GetDevice(&owner)) && owner) {
                const bool foreign = owner != static_cast<IDirect3DDevice9*>(this);
                owner->Release();
                if (foreign) return D3DERR_INVALIDCALL;
            }
        }
        if (idx == 0) {
            setRef(cachedBackBuffer0_, (IDirect3DSurface9*)nullptr);
        }
        const bool wasExplicit = rtSlotExplicit_[idx];
        const bool valueChanged = rtSlots_[idx] != pSurf;
        rtSlotExplicit_[idx] = true;
        if (valueChanged) {
            setRef(rtSlots_[idx], pSurf);
        }
        if (valueChanged || !wasExplicit) {
            peState_.pendingRtMask |= 1u << idx;
        }
        if (idx == 0 && pSurf) {
            D3DSURFACE_DESC desc{};
            const HRESULT descHr = pSurf->GetDesc(&desc);
            if (FAILED(descHr)) return descHr;
            const uint32_t w = std::max<uint32_t>(1u, desc.Width);
            const uint32_t h = std::max<uint32_t>(1u, desc.Height);
            peState_.viewportShadow = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
            peState_.scissorShadow = D9CRect{0, 0, static_cast<int32_t>(w),
                                             static_cast<int32_t>(h)};
            peState_.pendingViewport = true;
            peState_.pendingScissor = true;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx,
                                               IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u",
                            this, (unsigned)idx);
        if (idx < 4 && rtSlotExplicit_[idx]) {
            if (!rtSlots_[idx]) {
                *ppS = nullptr;
                dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> explicit null",
                                    this, (unsigned)idx);
                return D3DERR_NOTFOUND;
            }
            rtSlots_[idx]->AddRef();
            *ppS = rtSlots_[idx];
            dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> cached rt=%p",
                                this, (unsigned)idx, static_cast<void*>(*ppS));
            return S_OK;
        }
        if (idx == 0 && cachedBackBuffer0_) {
            cachedBackBuffer0_->AddRef();
            *ppS = cachedBackBuffer0_;
            dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> cached backbuffer=%p",
                                this, (unsigned)idx, static_cast<void*>(*ppS));
            return S_OK;
        }
        D9CSurface* s = dxmt9c_device_get_render_target(dev_, idx);
        *ppS = s ? CreatePeSurface(s, this, nullptr, this, false) : nullptr;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> surface=%p",
                            this, (unsigned)idx, ppS ? static_cast<void*>(*ppS) : nullptr);
        return s ? S_OK : D3DERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pSurf) noexcept override {
        dxmt9DeviceDebugLog("device_set_depth_stencil device=%p surf=%p", this, pSurf);
        // render_target_device_mismatch: reject foreign-device surfaces.
        if (pSurf) {
            IDirect3DDevice9* owner = nullptr;
            if (SUCCEEDED(pSurf->GetDevice(&owner)) && owner) {
                const bool foreign = owner != static_cast<IDirect3DDevice9*>(this);
                owner->Release();
                if (foreign) return D3DERR_INVALIDCALL;
            }
            // visual_multisample_rt_ds_mismatch_policy: the DS multisample
            // type must match the bound RT[0] multisample type. Query both
            // descs (via the C ABI helpers) and reject the mismatch.
            if (rtSlots_[0]) {
                D9CSurface* dsRaw = rawSurf(pSurf);
                D9CSurface* rtRaw = rawSurf(rtSlots_[0]);
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
        const bool wasExplicit = dsSurfaceExplicit_;
        const bool valueChanged = dsSurface_ != pSurf;
        dsSurfaceExplicit_ = true;
        if (valueChanged) {
            setRef(dsSurface_, pSurf);
        }
        if (valueChanged || !wasExplicit) {
            peState_.pendingDs = true;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppS) noexcept override {
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

    /* ── scene ── */
    HRESULT STDMETHODCALLTYPE BeginScene() noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_begin_scene device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_scene(dev_));
        dxmt9DeviceDebugLog("device_begin_scene -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndScene()   noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_end_scene device=%p", this);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_device_end_scene(dev_));
        dxmt9DeviceDebugLog("device_end_scene -> hr=0x%08x", (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* pRects,
                                     DWORD flags, D3DCOLOR color,
                                     float z, DWORD stencil) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        // Wine d3d9: Clear count/pRects must agree, and Z clears require
        // a bound depth-stencil. visual_clear_color_only_policy /
        // visual_depth_buffer_clear_policy.
        if (count == 0 && pRects != nullptr) return D3DERR_INVALIDCALL;
        if (count > 0 && pRects == nullptr) return D3DERR_INVALIDCALL;
        if ((flags & D3DCLEAR_ZBUFFER) && !dsSurfaceExplicit_) {
            D9CSurface* s = dxmt9c_device_get_depth_stencil(dev_);
            if (!s) return D3DERR_INVALIDCALL;
            dxmt9c_surface_release(s);
        } else if ((flags & D3DCLEAR_ZBUFFER) && dsSurfaceExplicit_ && !dsSurface_) {
            return D3DERR_INVALIDCALL;
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
        if (FAILED(barrierHr)) return barrierHr;

        const std::uint32_t rectBytes = static_cast<std::uint32_t>(count) * sizeof(D9CRect);
        D9CCommandRecordClear header{};
        header.header.type = D9C_COMMAND_RECORD_CLEAR;
        header.header.size = static_cast<std::uint32_t>(sizeof(header) + rectBytes);
        header.flags = (uint32_t)flags;
        header.colorARGB = (uint32_t)color;
        header.z = z;
        header.stencil = (uint32_t)stencil;
        header.rectCount = (uint32_t)count;
        header.rectOffset = sizeof(header);

        return appendCommandRecordDirect(
            header.header.type, header.header.size,
            [&header, pRects, rectBytes](std::uint8_t* record) {
                std::memcpy(record, &header, sizeof(header));
                if (rectBytes != 0 && pRects) {
                    std::memcpy(record + header.rectOffset, pRects, rectBytes);
                }
            });
    }

    /* ── transforms ── */
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog(
            "device_set_transform device=%p state=%u "
            "m=[[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g]]",
            this, (unsigned)state,
            pM->m[0][0], pM->m[0][1], pM->m[0][2], pM->m[0][3],
            pM->m[1][0], pM->m[1][1], pM->m[1][2], pM->m[1][3],
            pM->m[2][0], pM->m[2][1], pM->m[2][2], pM->m[2][3],
            pM->m[3][0], pM->m[3][1], pM->m[3][2], pM->m[3][3]);
        // Phase 12: PE-shadow-only when chunk recorder is active. Pending
        // transforms ride on the next draw packet's transforms[] array;
        // server-side applyDrawPacketState dispatches set_transform per
        // entry before the draw runs.
        const D9CMatrix& wireM = *reinterpret_cast<const D9CMatrix*>(pM);
        const uint32_t stateKey = static_cast<uint32_t>(state);
        if (stateBlockRecording_) {
            if (!peState_.stateBlockTransformRestore.contains(stateKey)) {
                D9CMatrix previous = identityTransformMatrix();
                (void)peState_.transformShadow.get(stateKey, previous);
                peState_.stateBlockTransformRestore.set(stateKey, previous);
            }
            // PE-shadow stateblock support. The NEW value being set inside
            // BeginStateBlock/EndStateBlock is what the resulting stateblock
            // must replay on Apply. MultiplyTransform sets
            // suppressStateBlockTransformRecord_ around its internal
            // SetTransform call so the multiply does NOT land in this table —
            // wined3d's "MultiplyTransform during recording is ignored by the
            // stateblock" quirk.
            if (!suppressStateBlockTransformRecord_) {
                peState_.stateBlockTransformRecorded.set(stateKey, wireM);
            }
            // Keep the PE shadow in sync with the server during recording so
            // a subsequent MultiplyTransform / GetTransform on the same key
            // observes the in-flight value, not pre-Begin state. EndStateBlock
            // is responsible for reverting shadow entries that should not
            // survive the block (see the *Restore loop).
            peState_.transformShadow.set(stateKey, wireM);
            return hr32(dxmt9c_device_set_transform(dev_, stateKey, &wireM));
        }
        uint32_t transformSlotIndex = 0;
        if (!FixedTransformTable::slotForState(stateKey, transformSlotIndex)) {
            const HRESULT flushHr = flushPeRecorder();
            if (FAILED(flushHr)) return flushHr;
            return hr32(dxmt9c_device_set_transform(dev_, stateKey, &wireM));
        }
        D9CMatrix shadowMatrix{};
        const bool shadowMatches = peState_.transformShadow.get(stateKey, shadowMatrix) &&
                                   matrixEquals(shadowMatrix, wireM);
        const bool alreadyPending = peState_.pendingTransforms.contains(stateKey);
        if (!alreadyPending && shadowMatches) {
            return S_OK;
        }
        // Phase 34: cap-check uses chunkBarrierFlush, not bare
        // flushPendingCommandChunk. The latter only seals existing
        // records — pending hot state would remain DIRTY across
        // the seal, leaving the next Draw* / barrier observing
        // stale server state. chunkBarrierFlush encodes pending
        // state as APPLY_STATE record(s) + clears the pending
        // maps, so the new entry below starts with a fresh delta
        // budget AND the server has already received the prior
        // delta when the next chunk-record runs.
        if (!alreadyPending &&
            peState_.pendingTransforms.size() >= D9C_DRAW_PACKET_MAX_TRANSFORMS) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        peState_.pendingTransforms.set(stateKey, wireM);
        peState_.transformShadow.set(stateKey, wireM);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state,
                                            D3DMATRIX* pM) noexcept override {
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
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_multiply_transform device=%p state=%u", this, (unsigned)state);
        D3DMATRIX cur{};
        // GetTransform reads the PE shadow (which is also updated during
        // recording — see SetTransform's recording branch), so we observe
        // the in-flight multiplicand whether we are inside Begin/End or
        // not.
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
        // wined3d quirk: MultiplyTransform inside BeginStateBlock/
        // EndStateBlock updates the device state but does NOT contribute
        // to the resulting stateblock's tracked entries — guard the
        // recursive SetTransform call so it skips stateBlockTransformRecorded
        // (the *Restore branch still runs so the device shadow stays in
        // sync, but the resulting stateblock will not replay this state).
        const bool wasSuppressed = suppressStateBlockTransformRecord_;
        suppressStateBlockTransformRecord_ = true;
        const HRESULT hr = SetTransform(state, &result);
        suppressStateBlockTransformRecord_ = wasSuppressed;
        return hr;
    }

    /* ── viewport / scissor ── */
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pVP) noexcept override {
        if (!pVP) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_viewport device=%p x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                            this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
        D9CViewport vp{ pVP->X, pVP->Y, pVP->Width, pVP->Height,
                        pVP->MinZ, pVP->MaxZ };
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // packet built for the next draw carries viewportValid=1 + the
        // shadow snapshot; server-side applyDrawPacketState dispatches
        // dxmt9c_device_set_viewport before the draw runs.
        if (std::memcmp(&peState_.viewportShadow, &vp, sizeof(vp)) == 0) {
            return S_OK;
        }
        peState_.viewportShadow = vp;
        peState_.pendingViewport = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pVP) noexcept override {
        if (!pVP) return D3DERR_INVALIDCALL;
        // Phase 12: PE shadow is the source of truth. SetViewport writes
        // only into peState_.viewportShadow (recorder-active path);
        // round-trip the same value.
        const D9CViewport& vp = peState_.viewportShadow;
        pVP->X = vp.x; pVP->Y = vp.y;
        pVP->Width = vp.width; pVP->Height = vp.height;
        pVP->MinZ = vp.minZ;   pVP->MaxZ   = vp.maxZ;
        dxmt9DeviceDebugLog("device_get_viewport device=%p -> x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                            this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pR) noexcept override {
        if (!pR) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_scissor_rect device=%p rect=%ld,%ld-%ld,%ld",
                            this, (long)pR->left, (long)pR->top, (long)pR->right, (long)pR->bottom);
        D9CRect cr = toR(*pR);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (std::memcmp(&peState_.scissorShadow, &cr, sizeof(cr)) == 0) {
            return S_OK;
        }
        peState_.scissorShadow = cr;
        peState_.pendingScissor = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pR) noexcept override {
        if (!pR) return D3DERR_INVALIDCALL;
        // Phase 12: PE shadow is the source of truth (see GetViewport).
        const D9CRect& cr = peState_.scissorShadow;
        pR->left = cr.left; pR->top = cr.top;
        pR->right = cr.right; pR->bottom = cr.bottom;
        return S_OK;
    }

    /* ── material / lights ── */
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_material device=%p", this);
        if (std::memcmp(&peState_.materialShadow, pM, sizeof(D9CMaterial)) == 0) {
            return S_OK;
        }
        std::memcpy(&peState_.materialShadow, pM, sizeof(D9CMaterial));
        peState_.pendingMaterial = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_material device=%p", this);
        // PE-shadow is the source of truth: SetMaterial only writes the
        // shadow, never the C-side state. Reading from C would return the
        // default-constructed value instead of the last Set value.
        std::memcpy(pM, &peState_.materialShadow, sizeof(D3DMATERIAL9));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD idx, const D3DLIGHT9* pL) noexcept override {
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
        // Phase 12: PE-shadow-only when chunk recorder is active. Up to
        // D9C_DRAW_PACKET_MAX_LIGHTS (8) light slots ride on a single
        // packet via lightSlotMask + lights[8]. Out-of-range idx falls
        // back to legacy unix-call (rare, and the backend may also
        // refuse).
        if (idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
            if ((peState_.pendingLightSlotMask & (1u << idx)) == 0 &&
                std::memcmp(&peState_.lightShadow[idx], &cl, sizeof(D9CLight)) == 0) {
                return S_OK;
            }
            peState_.lightShadow[idx] = cl;
            peState_.pendingLightSlotMask |= 1u << idx;
            return S_OK;
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_light(dev_, idx, &cl));
    }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD idx, D3DLIGHT9* pL) noexcept override {
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
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) noexcept override {
        dxmt9DeviceDebugLog("device_light_enable device=%p idx=%u enable=%u", this, (unsigned)idx, (unsigned)en);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
            const DWORD bit = 1u << idx;
            const bool wantEnabled = en != 0;
            const bool shadowEnabled = (peState_.lightEnableShadow & bit) != 0;
            if ((peState_.pendingLightEnableValidMask & bit) == 0 &&
                wantEnabled == shadowEnabled) {
                return S_OK;
            }
            peState_.pendingLightEnableValidMask |= bit;
            if (wantEnabled) {
                peState_.pendingLightEnableMask |= bit;
                peState_.lightEnableShadow |= bit;
            } else {
                peState_.pendingLightEnableMask &= ~bit;
                peState_.lightEnableShadow &= ~bit;
            }
            return S_OK;
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_light_enable(dev_, idx, en ? 1u : 0u));
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD idx, BOOL* pEn) noexcept override {
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

    /* ── clip planes ── */
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* pPlane) noexcept override {
        dxmt9DeviceDebugLog("device_set_clip_plane device=%p idx=%u plane=%p", this, (unsigned)idx, pPlane);
        if (!pPlane) return D3DERR_INVALIDCALL;
        if (idx >= 6) return D3DERR_INVALIDCALL;
        const std::size_t off = static_cast<std::size_t>(idx) * 4u;
        if ((peState_.pendingClipPlaneMask & (1u << idx)) == 0 &&
            std::memcmp(&peState_.clipPlaneShadow[off], pPlane, sizeof(float) * 4) == 0) {
            return S_OK;
        }
        std::memcpy(&peState_.clipPlaneShadow[off], pPlane, sizeof(float) * 4);
        peState_.pendingClipPlaneMask |= 1u << idx;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* pPlane) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* p) noexcept override {
        dxmt9DeviceDebugLog("device_set_clip_status device=%p", this);
        // gap_d3d9 B.8: dxmt9 does not track per-primitive clip status — no hardware
        // path exposes per-vertex clip-flag accumulation, exactly like wined3d's
        // storage-free stub. Reject null (the one real wined3d contract) and
        // otherwise accept without storing; echoing the seed back would be a
        // meaningless fake value, so GetClipStatus returns a defined default instead.
        if (!p) return D3DERR_INVALIDCALL;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* p) noexcept override {
        dxmt9DeviceDebugLog("device_get_clip_status device=%p", this);
        if (!p) return D3DERR_INVALIDCALL;
        // Defined "everything visible / nothing clipped" default rather than echoing
        // a meaningless seed (no real clip accumulation exists on the HW path) or
        // leaving the caller's buffer untouched.
        p->ClipUnion = 0u;
        p->ClipIntersection = 0xFFFFFFFFu;
        return S_OK;
    }

    /* R-FORMAT-11 — service a RESZ depth-resolve sentinel write. Resolves the
     * bound multisampled depth source (the bound depth-stencil surface) into
     * the bound INTZ depth destination (the stage-0 texture). Returns S_OK
     * unconditionally: D3D9 SetRenderState has no failure contract for a
     * point-size write, and the RESZ idiom is fire-and-forget — a missing
     * source/destination binding is a benign no-op on real hardware too.
     *
     * BACKEND STATUS: the winemetal ABI + backend primitive now exist.
     * WMTDepthAttachmentInfo carries resolve_texture + resolve_filter
     * (WMTMultisampleDepthResolveFilter) — the DEPTH twin of the color resolve
     * that rides on WMTColorAttachmentInfo.resolve_texture +
     * WMTStoreActionMultisampleResolve. The unix importer wires
     * MTLRenderPassDescriptor.depthAttachment.resolveTexture /
     * .depthResolveFilter (src/winemetal/unix/winemetal_private_api.mm), and
     * the backend resolve is implemented as
     * dxmt9::encoders::encodeDepthResolve(cmdbuf, pool, msaaDepthSrc, intzDst)
     * in src/dxmt9/dxmt9_blit_encoders.cpp — a render pass whose depth
     * attachment uses store=MultisampleResolve + resolve_texture +
     * filter=Sample.
     *
     * PE->unix DISPATCH (this change): the request is carried across the
     * bridge as a D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE chunk record
     * (mirroring D9CCommandRecordReadback's two-handle shape) — msaaDepthHandle
     * = the bound depth-stencil surface, intzDestHandle = the stage-0 INTZ
     * texture. It is validated in device_c_record_validate.cpp, classified as
     * a SurfaceOp ordering barrier in device_c_record_replay.cpp, and
     * dispatched to dxmt9::encoders::encodeDepthResolve via
     * dxmt9_draw_encoder.mm's surface-op Kind switch. The record is emitted
     * exactly like StretchRect/ColorFill (chunkBarrierFlush, then append with
     * source/dest retained), so it orders atomically with the surrounding
     * draws/clears in the same chunk. */
    HRESULT requestReszDepthResolve() noexcept {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        // MSAA depth source = the currently bound depth-stencil surface.
        D9CSurface* const depthSrcRaw = rawSurf(dsSurface_);
        // INTZ depth destination = the texture bound at fragment stage 0.
        D9CTexture* const intzDstRaw = rawTex(textures_[0]);
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
        D9CCommandRecordReszDepthResolve record{};
        record.header.type = D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE;
        record.header.size = sizeof(record);
        record.msaaDepthHandle = reinterpret_cast<uint64_t>(depthSrcRaw);
        record.intzDestHandle = reinterpret_cast<uint64_t>(intzDstRaw);
        const HRESULT appendHr = appendCommandRecordRetained(
            &record, sizeof(record), depthSrcRaw, /*surface1=*/nullptr,
            intzDstRaw);
        if (FAILED(appendHr)) return appendHr;
        return S_OK;
    }

    /* ── render states ── */
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) noexcept override {
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
            return requestReszDepthResolve();
        }
        if (stateBlockRecording_) {
            const DWORD stateKey = static_cast<DWORD>(state);
            if (!peState_.stateBlockRenderStateRestore.contains(stateKey)) {
                DWORD previous = dxmt9c_device_get_render_state(dev_, stateKey);
                uint32_t shadowValue = 0;
                if (peState_.renderStateShadow.get(stateKey, shadowValue)) {
                    previous = shadowValue;
                }
                peState_.stateBlockRenderStateRestore.set(stateKey, previous);
            }
            return hr32(dxmt9c_device_set_render_state(dev_, (uint32_t)state, value));
        }
        const DWORD stateKey = static_cast<DWORD>(state);
        if (shadowedRenderStateEquals(stateKey, value)) {
            return S_OK;
        }
        // Phase 31: cap check — if a NEW state would push the pending
        // table past the per-packet cap, drain pending state into the chunk
        // via chunkBarrierFlush() so the next packet starts fresh.
        if (!peState_.pendingRenderStates.contains(stateKey) &&
            peState_.pendingRenderStates.size() >= D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        peState_.renderStateShadow.set(stateKey, value);
        peState_.pendingRenderStates.set(stateKey, value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD* pValue) noexcept override {
        if (!pValue) return D3DERR_INVALIDCALL;
        uint32_t shadowValue = 0;
        if (peState_.renderStateShadow.get(static_cast<DWORD>(state), shadowValue)) {
            *pValue = shadowValue;
            return S_OK;
        }
        *pValue = dxmt9c_device_get_render_state(dev_, (uint32_t)state);
        return S_OK;
    }

    /* ── state blocks ── */
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type,
                                                IDirect3DStateBlock9** ppSB) noexcept override {
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
    HRESULT STDMETHODCALLTYPE BeginStateBlock() noexcept override {
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
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) noexcept override {
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
        dxmt9DeviceDebugLog("device_set_texture_stage_state device=%p stage=%u type=%u value=0x%x",
                            this, (unsigned)stage, (unsigned)type, (unsigned)value);
        // Wine d3d9 test_limits + test_texture_stage_states: reject
        // out-of-range stage (>= caps.MaxTextureBlendStages == 8) and
        // unrecognised D3DTSS_* type with D3DERR_INVALIDCALL at the
        // device-method boundary.
        if (stage >= kFragmentBlendStageCount) return D3DERR_INVALIDCALL;
        if (!isValidTextureStageStateType(type)) return D3DERR_INVALIDCALL;
        const uint32_t stageSlot = textureStageSlot(stage);
        const uint32_t stateSlot = textureStageStateSlot(type);
        uint32_t shadowValue = 0;
        if (peState_.tssShadow.get(stageSlot, stateSlot, shadowValue) &&
            shadowValue == value) {
            return S_OK;
        }
        // Phase 34: cap-check uses chunkBarrierFlush so pending state is
        // encoded as APPLY_STATE record(s) + cleared before the new entry.
        if (!peState_.pendingTss.contains(stageSlot, stateSlot) &&
            peState_.pendingTss.size() >= D9C_DRAW_PACKET_MAX_TSS) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        peState_.tssShadow.set(stageSlot, stateSlot, value);
        peState_.pendingTss.set(stageSlot, stateSlot, value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD* pValue) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD value) noexcept override {
        dxmt9DeviceDebugLog("device_set_sampler_state device=%p sampler=%u type=%u value=0x%x",
                            this, (unsigned)sampler, (unsigned)type, (unsigned)value);
        uint32_t samplerIndex = 0;
        if (!samplerSlot(sampler, samplerIndex)) {
            return D3DERR_INVALIDCALL;
        }
        uint32_t stateSlot = 0;
        if (!samplerStateSlot(type, stateSlot)) {
            return S_OK;
        }
        uint32_t shadowValue = 0;
        if (peState_.samplerStateShadow.get(samplerIndex, stateSlot, shadowValue) &&
            shadowValue == value) {
            return S_OK;
        }
        // Phase 34: cap-check uses chunkBarrierFlush.
        if (!peState_.pendingSamplerStates.contains(samplerIndex, stateSlot) &&
            peState_.pendingSamplerStates.size() >= D9C_DRAW_PACKET_MAX_SAMPLER) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        peState_.samplerStateShadow.set(samplerIndex, stateSlot, value);
        peState_.pendingSamplerStates.set(samplerIndex, stateSlot, value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD* pValue) noexcept override {
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
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pPasses) noexcept override {
        dxmt9DeviceDebugLog("device_validate_device device=%p", this);
        if (pPasses) *pPasses = 1; return S_OK;
    }

    /* ── palette — PE shadow plus P8/A8P8 backend expansion
     *    (test_set_palette_roundtrip, test_palette_alpha_caps_policy,
     *     test_palette_current_entry_isolation, dxmt9-core-device-com-spec).
     *     P8 resources keep index data PE/C-side and re-expand through
     *     the active palette into the backend A8R8G8B8 backing texture.
     * ─────────────────────────────────────────────────────────────── */
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY* out) noexcept override {
        dxmt9DeviceDebugLog("device_get_palette_entries device=%p palette=%u out=%p",
                            this, palette, static_cast<void*>(out));
        if (!out) return D3DERR_INVALIDCALL;
        const auto it = palettes_.find(palette);
        if (it == palettes_.end()) return D3DERR_INVALIDCALL;
        std::memcpy(out, it->second.data(), sizeof(PALETTEENTRY) * 256);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) noexcept override {
        dxmt9DeviceDebugLog("device_set_current_texture_palette device=%p palette=%u", this, palette);
        if (palettes_.find(palette) == palettes_.end()) {
            return D3DERR_INVALIDCALL;
        }
        currentPaletteIndex_ = palette;
        currentPaletteSet_ = true;
        applyCurrentPaletteToBoundTextures();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) noexcept override {
        dxmt9DeviceDebugLog("device_get_current_texture_palette device=%p out=%p",
                            this, static_cast<void*>(p));
        if (!p) return D3DERR_INVALIDCALL;
        if (!currentPaletteSet_) return D3DERR_INVALIDCALL;
        *p = currentPaletteIndex_;
        return S_OK;
    }

    /* ── soft VP / NPatches ── */
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL enable) noexcept override {
        dxmt9DeviceDebugLog("device_set_software_vertex_processing device=%p enable=%u", this, (unsigned)enable);
        softwareVertexProcessing_ = enable ? TRUE : FALSE;
        return S_OK;
    }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() noexcept override {
        dxmt9DeviceDebugLog("device_get_software_vertex_processing device=%p", this);
        return softwareVertexProcessing_;
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) noexcept override {
        dxmt9DeviceDebugLog("device_set_npatch_mode device=%p segments=%f", this, segments);
        // stub: Wine returns S_OK; N-Patch tessellation was removed in D3D10, legacy
        // apps tolerate a no-op.
        return S_OK;
    }
    float   STDMETHODCALLTYPE GetNPatchMode() noexcept override {
        // stub: Wine returns 0.0f; N-Patch tessellation removed in D3D10, legacy apps
        // tolerate a no-op.
        return 0.0f;
    }

    /* ── textures ── */
    // Wine d3d9 texture-stage validation. Valid stages are the FFP
    // fragment sampler range [0..MaxSimultaneousTextures-1] (=[0..7]
    // under the caps dxmt9 reports) plus the vertex texture sampler
    // range D3DVERTEXTEXTURESAMPLER0..D3DVERTEXTEXTURESAMPLER3.
    // Anything else is D3DERR_INVALIDCALL (see test_get_set_texture
    // around line 2693: SetTexture(MaxSimultaneousTextures, ...) must
    // fail and GetTexture must leave the caller's out-pointer
    // untouched).
    static constexpr DWORD kFragmentTextureStageCount = 8;
    static bool fragmentTextureStageSlot(DWORD stage, uint32_t& slot) noexcept {
        if (stage < kFragmentTextureStageCount) {
            slot = static_cast<uint32_t>(stage);
            return true;
        }
        return vertexTextureSamplerSlot(stage, slot);
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage,
                                          IDirect3DBaseTexture9* pTex) noexcept override {
        dxmt9DeviceDebugLog("device_set_texture device=%p stage=%u tex=%p",
                            this, (unsigned)stage, pTex);
        // Wine d3d9 test_limits: SetTexture with a stage at or beyond
        // caps.MaxSimultaneousTextures (8) — but not in the vertex
        // texture sampler range D3DVERTEXTEXTURESAMPLER0..3 (257..260)
        // — returns D3DERR_INVALIDCALL. textureBindingSlot accepts
        // 0..kPeFragmentSamplerSlots-1 (=15) which over-promises the
        // exposed cap; tighten the front-end guard here so the device
        // reports the same surface area as makeDefaultCaps advertises.
        constexpr DWORD kMaxFragmentTextureStage = 8;
        if (stage >= kMaxFragmentTextureStage &&
            (stage < D3DVERTEXTEXTURESAMPLER0 || stage > D3DVERTEXTEXTURESAMPLER3)) {
            return D3DERR_INVALIDCALL;
        }
        uint32_t textureSlot = 0;
        if (!fragmentTextureStageSlot(stage, textureSlot)) return D3DERR_INVALIDCALL;
        if (textures_[textureSlot] == pTex) {
            return S_OK;
        }
        setRef(textures_[textureSlot], pTex);
        applyCurrentPaletteToTexture(textures_[textureSlot]);
        peState_.pendingTextureMask |= 1u << textureSlot;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage,
                                          IDirect3DBaseTexture9** ppTex) noexcept override {
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

    /* ── FVF / vertex declaration ── */
    /// Resolve (and cache) the implicit IDirect3DVertexDeclaration9 for an
    /// FVF. The cache owns one ref per entry; this function does NOT add a
    /// new reference for the caller — call AddRef yourself before handing
    /// the pointer out. Returns nullptr only on allocation failure.
    IDirect3DVertexDeclaration9* implicitDeclForFvf(DWORD fvf) {
        if (fvf == 0) return nullptr;
        if (auto it = fvfDeclCache_.find(fvf); it != fvfDeclCache_.end()) {
            return it->second;
        }
        std::vector<D3DVERTEXELEMENT9> elements;
        fvfToVertexElements(fvf, elements);
        D9CVertexElement tmp[MAXD3DDECLLENGTH + 1]{};
        const size_t n = elements.size();
        if (n > MAXD3DDECLLENGTH + 1) return nullptr;
        for (size_t i = 0; i < n; ++i) {
            tmp[i].stream     = elements[i].Stream;
            tmp[i].offset     = elements[i].Offset;
            tmp[i].type       = elements[i].Type;
            tmp[i].method     = elements[i].Method;
            tmp[i].usage      = elements[i].Usage;
            tmp[i].usageIndex = elements[i].UsageIndex;
        }
        D9CVertexDecl* d = dxmt9c_device_create_vertex_declaration(dev_, tmp);
        if (!d) return nullptr;
        IDirect3DVertexDeclaration9* decl = CreatePeVertexDecl(d, this);
        if (!decl) return nullptr;
        /* Cache holds one ref. */
        fvfDeclCache_.emplace(fvf, decl);
        return decl;
    }

    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) noexcept override {
        dxmt9DeviceDebugLog("device_set_fvf device=%p fvf=0x%x", this, (unsigned)fvf);
        if (fvf_ == fvf && vdecl_ != nullptr) {
            /* Same FVF, decl already mirrored. */
            return S_OK;
        }
        fvf_ = fvf;
        peState_.pendingFvf = true;
        peState_.pendingVdecl = true;
        /* SetFVF shadows the vertex-declaration slot: GetVertexDeclaration
         * must return the implicit decl for this FVF (Wine
         * test_vertex_declaration_fvf_policy line ~702 and
         * test_fvf_decl_management). vdecl_ is borrowed (see
         * SetVertexDeclaration for Wine refcount semantics) — the
         * implicit decl is kept alive by fvfDeclCache_. */
        vdecl_ = implicitDeclForFvf(fvf);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) noexcept override {
        if (!pFVF) return D3DERR_INVALIDCALL;
        *pFVF = fvf_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
            const D3DVERTEXELEMENT9* pElems,
            IDirect3DVertexDeclaration9** ppVD) noexcept override {
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
        *ppVD = CreatePeVertexDecl(d, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(
            IDirect3DVertexDeclaration9* pVD) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_declaration device=%p decl=%p", this, pVD);
        // PE-shadow stateblock support: remember that vdecl was touched
        // during BeginStateBlock/EndStateBlock so the resulting block's
        // tracked set includes the vdecl slot. The flag is consumed by
        // CaptureStateBlockShadowForChild and cleared in EndStateBlock.
        if (stateBlockRecording_) {
            peState_.stateBlockVdeclRecorded = true;
        }
        if (vdecl_ == pVD) return S_OK;
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
        vdecl_ = pVD;
        /* Explicit decl resets FVF to 0 (Wine
         * test_vertex_declaration_fvf_policy line ~692). User-supplied
         * decls do not back-convert to an FVF in this PE shadow; that
         * mapping is intentionally lossy. */
        fvf_ = 0;
        peState_.pendingVdecl = true;
        peState_.pendingFvf = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(
            IDirect3DVertexDeclaration9** ppVD) noexcept override {
        if (!ppVD) return D3DERR_INVALIDCALL;
        if (vdecl_) vdecl_->AddRef();
        *ppVD = vdecl_; return S_OK;
    }

    /* ── vertex shaders ── */
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFn,
                                                  IDirect3DVertexShader9** ppVS) noexcept override {
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
        *ppVS = CreatePeVertexShader(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_shader device=%p shader=%p", this, pVS);
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // packet built for the next draw carries vsValid=1 + the vs_
        // wire handle; server-side applyDrawPacketState dispatches the
        // dxmt9c_device_set_vertex_shader call before the draw runs.
        if (vs_ == pVS) return S_OK;
        setRef(vs_, pVS);
        peState_.pendingVs = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppVS) noexcept override {
        if (!ppVS) return D3DERR_INVALIDCALL;
        if (vs_) vs_->AddRef(); *ppVS = vs_; return S_OK;
    }
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

    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* pData,
                                                        UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_f device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
        if (FAILED(hr)) return hr;
        // Shadow-only: defer the record until the next flushPendingConsts()
        // (called before each draw record + at chunk commit).
        touchConstShadow(peConsts_.vsConstF, start, count, pData, sizeof(float) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* pData,
                                                        UINT count) noexcept override {
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.vsConstF, start, pData, count, sizeof(float) * 4);
        return S_OK;    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.vsConstI, start, count, pData, sizeof(int32_t) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, INT* pData,
                                                        UINT count) noexcept override {
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.vsConstI, start, pData, count, sizeof(int32_t) * 4);        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.vsConstB, start, count, pData, sizeof(uint32_t));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, BOOL* pData,
                                                        UINT count) noexcept override {
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.vsConstB, start, pData, count, sizeof(uint32_t));        return S_OK;
    }

    /* ── stream sources ── */
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9* pBuf,
                                               UINT offset, UINT stride) noexcept override {
        dxmt9DeviceDebugLog("device_set_stream_source device=%p stream=%u buf=%p offset=%u stride=%u",
                            this, stream, pBuf, offset, stride);
        if (stream >= 16) return D3DERR_INVALIDCALL;
        // Wine d3d9 deactivate-stream idiom: SetStreamSource(NULL, 0, 0)
        // detaches the buffer while preserving the previously cached
        // offset/stride for the stream slot — verified in
        // test_stream_source_null_layout_policy at line ~2375 where
        // (vb, 4, 32) followed by (NULL, 0, 0) yields a Get of
        // (NULL, 4, 32). Other null calls (NULL with non-zero offset
        // or non-zero stride) flow through the regular store-as-given
        // path.
        if (pBuf == nullptr && offset == 0 && stride == 0) {
            if (streamSrc_[stream] == nullptr) {
                return S_OK;
            }
            setRef(streamSrc_[stream], (IDirect3DVertexBuffer9*)nullptr);
            peState_.pendingStreamMask |= 1u << stream;
            return S_OK;
        }
        if (shadowedStreamSourceEquals(stream, pBuf, offset, stride)) {
            return S_OK;
        }
        setRef(streamSrc_[stream], pBuf);
        streamOff_[stream] = offset;
        streamStr_[stream] = stride;
        peState_.pendingStreamMask |= 1u << stream;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9** ppBuf,
                                               UINT* pOffset, UINT* pStride) noexcept override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        if (stream >= 16) return D3DERR_INVALIDCALL;
        IDirect3DVertexBuffer9* b = streamSrc_[stream];
        if (b) b->AddRef();
        *ppBuf = b;
        if (pOffset) *pOffset = streamOff_[stream];
        if (pStride) *pStride = streamStr_[stream];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT freq) noexcept override {
        dxmt9DeviceDebugLog("device_set_stream_source_freq device=%p stream=%u freq=0x%x",
                            this, stream, (unsigned)freq);
        if (stream >= 16) return D3DERR_INVALIDCALL;
        // D3D9 SetStreamSourceFreq encoding (D3DSTREAMSOURCE_* in
        // d3d9types.h):
        //   - low 24 bits: divider value
        //   - bit 0x40000000 (INDEXEDDATA): per-instance source stream
        //   - bit 0x80000000 (INSTANCEDATA): the index source stream
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
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        streamFreq_[stream] = freq;
        return hr32(dxmt9c_device_set_stream_source_freq(dev_, stream, freq));
    }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* pFreq) noexcept override {
        if (!pFreq) return D3DERR_INVALIDCALL;
        if (stream >= 16) return D3DERR_INVALIDCALL;
        const UINT freq = streamFreq_[stream];
        *pFreq = freq;
        dxmt9DeviceDebugLog("device_get_stream_source_freq device=%p stream=%u -> freq=0x%x",
                            this, stream, (unsigned)freq);
        return S_OK;
    }

    /* ── indices ── */
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIBuf) noexcept override {
        dxmt9DeviceDebugLog("device_set_indices device=%p ib=%p", this, pIBuf);
        if (indexBuf_ == pIBuf) return S_OK;
        setRef(indexBuf_, pIBuf);
        peState_.pendingIb = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIBuf) noexcept override {
        if (!ppIBuf) return D3DERR_INVALIDCALL;
        if (indexBuf_) indexBuf_->AddRef(); *ppIBuf = indexBuf_; return S_OK;
    }

    /* ── pixel shaders ── */
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFn,
                                                 IDirect3DPixelShader9** ppPS) noexcept override {
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
        *ppPS = CreatePePixelShader(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pPS) noexcept override {
        dxmt9DeviceDebugLog("device_set_pixel_shader device=%p shader=%p", this, pPS);
        if (ps_ == pPS) return S_OK;
        setRef(ps_, pPS);
        peState_.pendingPs = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) noexcept override {
        if (!ppPS) return D3DERR_INVALIDCALL;
        if (ps_) ps_->AddRef(); *ppPS = ps_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_f device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.psConstF, start, count, pData, sizeof(float) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* pData,
                                                       UINT count) noexcept override {
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.psConstF, start, pData, count, sizeof(float) * 4);
        return S_OK;    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.psConstI, start, count, pData, sizeof(int32_t) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, INT* pData,
                                                       UINT count) noexcept override {
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.psConstI, start, pData, count, sizeof(int32_t) * 4);        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.psConstB, start, count, pData, sizeof(uint32_t));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, BOOL* pData,
                                                       UINT count) noexcept override {
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.psConstB, start, pData, count, sizeof(uint32_t));        return S_OK;
    }

    /* ── draw calls ── */
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type,
                                             UINT startVertex,
                                             UINT count) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_draw_primitive device=%p type=%u startVertex=%u count=%u",
                            this, (unsigned)type, startVertex, count);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        const HRESULT hr = appendDrawPrimitiveRecord(type, startVertex, count);
        if (SUCCEEDED(hr)) {
            clearPendingHotState();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type,
                                                    INT baseVertex,
                                                    UINT minVertex, UINT numVertices,
                                                    UINT startIndex,
                                                    UINT count) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_draw_indexed_primitive device=%p type=%u base=%d min=%u num=%u startIndex=%u count=%u",
                            this, (unsigned)type, baseVertex, minVertex, numVertices,
                            startIndex, count);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        const HRESULT hr = appendDrawIndexedPrimitiveRecord(type, baseVertex, minVertex,
                                                            numVertices, startIndex, count);
        if (SUCCEEDED(hr)) {
            clearPendingHotState();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                               UINT count,
                                               const void* pData,
                                               UINT stride) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_draw_primitive_up device=%p type=%u count=%u data=%p stride=%u",
                            this, (unsigned)type, count, pData, stride);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        const HRESULT hr = appendDrawPrimitiveUPRecord(type, count, pData, stride);
        if (SUCCEEDED(hr)) {
            clearPendingHotState();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                      UINT minVertex,
                                                      UINT numVertices,
                                                      UINT count,
                                                      const void* pIdxData,
                                                      D3DFORMAT idxFmt,
                                                      const void* pVtxData,
                                                      UINT stride) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_draw_indexed_primitive_up device=%p type=%u min=%u num=%u count=%u idx=%p idxFmt=%u vtx=%p stride=%u",
                            this, (unsigned)type, minVertex, numVertices, count,
                            pIdxData, (unsigned)idxFmt, pVtxData, stride);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        const HRESULT hr = appendDrawIndexedPrimitiveUPRecord(type, minVertex, numVertices,
                                                              count, pIdxData, idxFmt,
                                                              pVtxData, stride);
        if (SUCCEEDED(hr)) {
            clearPendingHotState();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT srcStart, UINT dstIndex,
                                               UINT vertexCount,
                                               IDirect3DVertexBuffer9* dstBuffer,
                                               IDirect3DVertexDeclaration9* declaration,
                                               DWORD flags) noexcept override {
        // T2 device-lost gate: lost devices must report DEVICELOST before
        // any ProcessVertices validation or unsupported-path rejection.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_process_vertices device=%p srcStart=%u dstIndex=%u vertexCount=%u dst=%p decl=%p flags=0x%x",
                            this, srcStart, dstIndex, vertexCount, dstBuffer,
                            declaration, (unsigned)flags);
        auto invalid = [&](const char* reason) {
            dxmt9DeviceDebugLog("device_process_vertices invalid: %s", reason);
            return D3DERR_INVALIDCALL;
        };
        if (!dstBuffer) return invalid("null destination buffer");
        if (vertexCount == 0) return S_OK;
        if (flags & ~D3DPV_DONOTCOPYDATA) return invalid("flags unsupported");
        const bool programmable = vs_ != nullptr;
        std::vector<DWORD> shaderWords;
        ProcessShaderIo shaderIo{};
        if (programmable) {
            UINT shaderBytes = 0;
            HRESULT shaderHr = vs_->GetFunction(nullptr, &shaderBytes);
            if (FAILED(shaderHr) || shaderBytes == 0 ||
                (shaderBytes % sizeof(DWORD)) != 0) {
                return invalid("shader bytecode query failed");
            }
            shaderWords.resize(shaderBytes / sizeof(DWORD));
            shaderHr = vs_->GetFunction(shaderWords.data(), &shaderBytes);
            if (FAILED(shaderHr) ||
                !analyzeSimpleProcessVertexShader(shaderWords, shaderIo)) {
                return invalid("shader analysis failed");
            }
        }
        D9CBuffer* dstRaw = rawVBuf(dstBuffer);
        if (!dstRaw) return invalid("raw destination buffer missing");
        D9CBufferDesc dstDesc{};
        if (FAILED(hr32(dxmt9c_buffer_get_desc(dstRaw, &dstDesc)))) {
            return invalid("destination desc failed");
        }
        FvfProcessLayout srcLayout{};
        FvfProcessLayout dstLayout{};
        if (fvf_ != 0) {
            const DWORD positionMask = fvf_ & D3DFVF_POSITION_MASK;
            if ((positionMask != D3DFVF_XYZ &&
                 (!programmable ||
                  (positionMask != D3DFVF_XYZW &&
                   !processFvfXyzbPosition(positionMask)))) ||
                !describeProcessFvf(fvf_, srcLayout)) {
                return invalid("source FVF unsupported");
            }
        } else if (vdecl_) {
            if (!describeProcessDeclaration(vdecl_, srcLayout, false)) {
                return invalid("source declaration unsupported");
            }
            if (!programmable && srcLayout.positionBytes != 12u) {
                return invalid("fixed-function source declaration position unsupported");
            }
        } else {
            return invalid("no source layout");
        }
        if (declaration) {
            if (!describeProcessDeclaration(declaration, dstLayout, true)) {
                return invalid("destination declaration unsupported");
            }
        } else {
            if ((dstDesc.fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW ||
                (dstDesc.fvf & D3DFVF_NORMAL) != 0 ||
                !describeProcessFvf(dstDesc.fvf, dstLayout)) {
                return invalid("destination FVF unsupported");
            }
        }
        if (dstLayout.positionBytes != 16u) {
            return invalid("destination lacks POSITIONT");
        }
        auto renderStateValue = [&](D3DRENDERSTATETYPE state) -> DWORD {
            uint32_t shadowValue = 0;
            if (peState_.renderStateShadow.get(static_cast<DWORD>(state), shadowValue)) {
                return shadowValue;
            }
            return dxmt9c_device_get_render_state(dev_, static_cast<uint32_t>(state));
        };
        const bool processLighting =
            !programmable && srcLayout.normal && renderStateValue(D3DRS_LIGHTING) != 0;
        const bool processSpecularLighting =
            processLighting && renderStateValue(D3DRS_SPECULARENABLE) != 0;
        const bool processColorVertex =
            processLighting && renderStateValue(D3DRS_COLORVERTEX) != 0;
        auto processStreamInstanced = [&](UINT stream) {
            return (streamFreq_[stream] & D3DSTREAMSOURCE_INSTANCEDATA) != 0u;
        };
        UINT srcReadBytes[D9C_DRAW_PACKET_MAX_STREAMS]{};
        auto requirePositionRead = [&]() {
            srcReadBytes[srcLayout.positionStream] =
                std::max(srcReadBytes[srcLayout.positionStream],
                         srcLayout.positionOffset + srcLayout.positionBytes);
        };
        auto requireNormalRead = [&]() -> bool {
            if (!srcLayout.normal) return false;
            srcReadBytes[srcLayout.normalStream] =
                std::max(srcReadBytes[srcLayout.normalStream],
                         srcLayout.normalOffset + srcLayout.normalBytes);
            return true;
        };
        auto requireTangentRead = [&]() -> bool {
            if (!srcLayout.tangent) return false;
            srcReadBytes[srcLayout.tangentStream] =
                std::max(srcReadBytes[srcLayout.tangentStream],
                         srcLayout.tangentOffset + srcLayout.tangentBytes);
            return true;
        };
        auto requireBinormalRead = [&]() -> bool {
            if (!srcLayout.binormal) return false;
            srcReadBytes[srcLayout.binormalStream] =
                std::max(srcReadBytes[srcLayout.binormalStream],
                         srcLayout.binormalOffset + srcLayout.binormalBytes);
            return true;
        };
        auto requireBlendWeightRead = [&]() -> bool {
            if (!srcLayout.blendWeight) return false;
            srcReadBytes[srcLayout.blendWeightStream] =
                std::max(srcReadBytes[srcLayout.blendWeightStream],
                         srcLayout.blendWeightOffset + srcLayout.blendWeightBytes);
            return true;
        };
        auto requireBlendIndicesRead = [&]() -> bool {
            if (!srcLayout.blendIndices) return false;
            srcReadBytes[srcLayout.blendIndicesStream] =
                std::max(srcReadBytes[srcLayout.blendIndicesStream],
                         srcLayout.blendIndicesOffset + srcLayout.blendIndicesBytes);
            return true;
        };
        auto requirePSizeRead = [&]() -> bool {
            if (!srcLayout.psize) return false;
            srcReadBytes[srcLayout.psizeStream] =
                std::max(srcReadBytes[srcLayout.psizeStream],
                         srcLayout.psizeOffset + 4u);
            return true;
        };
        auto requireDiffuseRead = [&]() -> bool {
            if (!srcLayout.diffuse) return false;
            srcReadBytes[srcLayout.diffuseStream] =
                std::max(srcReadBytes[srcLayout.diffuseStream],
                         srcLayout.diffuseOffset + 4u);
            return true;
        };
        auto requireSpecularRead = [&]() -> bool {
            if (!srcLayout.specular) return false;
            srcReadBytes[srcLayout.specularStream] =
                std::max(srcReadBytes[srcLayout.specularStream],
                         srcLayout.specularOffset + 4u);
            return true;
        };
        auto requireMaterialColorRead = [&](DWORD source) -> bool {
            if (!processColorVertex) return true;
            if (source == D3DMCS_COLOR1) return requireDiffuseRead();
            if (source == D3DMCS_COLOR2) return requireSpecularRead();
            return source == D3DMCS_MATERIAL;
        };
        auto requireTexRead = [&](UINT i, bool requireMatchingBytes) -> bool {
            if (i >= srcLayout.texCount || srcLayout.texBytes[i] == 0u ||
                (requireMatchingBytes && dstLayout.texBytes[i] != srcLayout.texBytes[i])) {
                return false;
            }
            srcReadBytes[srcLayout.texStream[i]] =
                std::max(srcReadBytes[srcLayout.texStream[i]],
                         srcLayout.texOffset[i] + srcLayout.texBytes[i]);
            return true;
        };
        auto findGenericInput = [&](UINT usage, UINT usageIndex)
            -> const FvfProcessLayout::GenericInput* {
            for (UINT i = 0; i < srcLayout.genericInputCount; ++i) {
                const auto& generic = srcLayout.genericInput[i];
                if (generic.usage == usage &&
                    generic.usageIndex == usageIndex) {
                    return &generic;
                }
            }
            return nullptr;
        };
        auto requireGenericRead = [&](UINT usage, UINT usageIndex) -> bool {
            const auto* generic = findGenericInput(usage, usageIndex);
            if (!generic) return false;
            srcReadBytes[generic->stream] =
                std::max(srcReadBytes[generic->stream],
                         generic->offset + generic->bytes);
            return true;
        };
        if (programmable) {
            if (!shaderIo.hasOutputPosition) return invalid("shader lacks position output");
            if (dstLayout.psize && !shaderIo.hasOutputPSize) return invalid("shader lacks psize output");
            if (dstLayout.diffuse && !shaderIo.hasOutputDiffuse) return invalid("shader lacks diffuse output");
            if (dstLayout.specular && !shaderIo.hasOutputSpecular) return invalid("shader lacks specular output");
            for (UINT i = 0; i < dstLayout.texCount; ++i) {
                if (dstLayout.texBytes[i] == 0u) continue;
                if (!shaderIo.hasOutputTex[i]) return invalid("shader lacks texcoord output");
            }
            if (shaderIo.inputPosition >= 0) requirePositionRead();
            if (shaderIo.inputNormal >= 0 && !requireNormalRead()) return invalid("shader normal input missing");
            if (shaderIo.inputTangent >= 0 && !requireTangentRead()) return invalid("shader tangent input missing");
            if (shaderIo.inputBinormal >= 0 && !requireBinormalRead()) return invalid("shader binormal input missing");
            if (shaderIo.inputBlendWeight >= 0 && !requireBlendWeightRead()) return invalid("shader blendweight input missing");
            if (shaderIo.inputBlendIndices >= 0 && !requireBlendIndicesRead()) return invalid("shader blendindices input missing");
            if (shaderIo.inputPSize >= 0 && !requirePSizeRead()) return invalid("shader psize input missing");
            if (shaderIo.inputDiffuse >= 0 && !requireDiffuseRead()) return invalid("shader diffuse input missing");
            if (shaderIo.inputSpecular >= 0 && !requireSpecularRead()) return invalid("shader specular input missing");
            for (UINT i = 0; i < 8; ++i) {
                if (shaderIo.inputTex[i] >= 0 && !requireTexRead(i, false)) {
                    return invalid("shader texcoord input missing");
                }
            }
            for (UINT i = 0; i < shaderIo.inputGenericCount; ++i) {
                const auto& generic = shaderIo.inputGeneric[i];
                if (!requireGenericRead(generic.usage, generic.usageIndex)) {
                    return invalid("shader generic input missing");
                }
            }
        } else {
            requirePositionRead();
            if (dstLayout.diffuse) {
                if (processLighting) {
                    if (srcLayout.normalType != D3DDECLTYPE_FLOAT3) {
                        return invalid("fixed-function lighting normal type unsupported");
                    }
                    if (!requireNormalRead()) return invalid("lighting normal input missing");
                } else if (!requireDiffuseRead()) {
                    return invalid("diffuse passthrough missing");
                }
            }
            if (dstLayout.specular && processSpecularLighting) {
                if (srcLayout.normalType != D3DDECLTYPE_FLOAT3) {
                    return invalid("fixed-function specular normal type unsupported");
                }
                if (!requireNormalRead()) return invalid("specular lighting normal input missing");
            } else if (dstLayout.specular && !requireSpecularRead()) {
                return invalid("specular passthrough missing");
            }
            if (dstLayout.psize && !requirePSizeRead()) {
                return invalid("psize passthrough missing");
            }
            if (processLighting) {
                if (!requireMaterialColorRead(renderStateValue(D3DRS_DIFFUSEMATERIALSOURCE))) {
                    return invalid("diffuse material source color missing");
                }
                if (!requireMaterialColorRead(renderStateValue(D3DRS_AMBIENTMATERIALSOURCE))) {
                    return invalid("ambient material source color missing");
                }
                if (!requireMaterialColorRead(renderStateValue(D3DRS_EMISSIVEMATERIALSOURCE))) {
                    return invalid("emissive material source color missing");
                }
                if (processSpecularLighting &&
                    !requireMaterialColorRead(renderStateValue(D3DRS_SPECULARMATERIALSOURCE))) {
                    return invalid("specular material source color missing");
                }
            }
            for (UINT i = 0; i < dstLayout.texCount; ++i) {
                if (dstLayout.texBytes[i] == 0u) continue;
                if (!requireTexRead(i, true)) return invalid("texcoord passthrough mismatch");
            }
        }
        D9CBuffer* srcRaw[D9C_DRAW_PACKET_MAX_STREAMS]{};
        D9CBufferDesc srcDesc[D9C_DRAW_PACKET_MAX_STREAMS]{};
        uint64_t srcByteStart[D9C_DRAW_PACKET_MAX_STREAMS]{};
        uint64_t srcByteEnd[D9C_DRAW_PACKET_MAX_STREAMS]{};
        D9CBuffer* uniqueSrcRaw[D9C_DRAW_PACKET_MAX_STREAMS]{};
        uint64_t uniqueSrcLockSize[D9C_DRAW_PACKET_MAX_STREAMS]{};
        void* uniqueSrcBytes[D9C_DRAW_PACKET_MAX_STREAMS]{};
        UINT uniqueSrcCount = 0;
        for (UINT stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            if (srcReadBytes[stream] == 0u) continue;
            if (!streamSrc_[stream] || streamStr_[stream] < srcLayout.streamStride[stream]) {
                return invalid("source stream missing or stride too small");
            }
            srcRaw[stream] = rawVBuf(streamSrc_[stream]);
            if (!srcRaw[stream] ||
                FAILED(hr32(dxmt9c_buffer_get_desc(srcRaw[stream], &srcDesc[stream])))) {
                return invalid("source stream desc failed");
            }
            const bool instancedStream = processStreamInstanced(stream);
            const uint64_t firstElement = instancedStream ? 0u : srcStart;
            const uint64_t lastElement =
                firstElement + (instancedStream ? 0u : vertexCount - 1u);
            srcByteStart[stream] =
                static_cast<uint64_t>(streamOff_[stream]) +
                firstElement * streamStr_[stream];
            srcByteEnd[stream] =
                srcByteStart[stream] +
                (lastElement - firstElement) * streamStr_[stream] +
                srcReadBytes[stream];
            if (srcByteEnd[stream] > srcDesc[stream].size ||
                srcByteEnd[stream] > UINT32_MAX) {
                return invalid("source range out of bounds");
            }
            UINT unique = 0;
            for (; unique < uniqueSrcCount; ++unique) {
                if (uniqueSrcRaw[unique] == srcRaw[stream]) break;
            }
            if (unique == uniqueSrcCount) {
                uniqueSrcRaw[uniqueSrcCount++] = srcRaw[stream];
            }
            uniqueSrcLockSize[unique] =
                std::max(uniqueSrcLockSize[unique], srcByteEnd[stream]);
        }
        const uint64_t dstByteStart =
            static_cast<uint64_t>(dstIndex) * dstLayout.stride;
        const uint64_t dstByteEnd =
            dstByteStart + static_cast<uint64_t>(vertexCount) * dstLayout.stride;
        if (dstByteEnd > dstDesc.size || dstByteEnd > UINT32_MAX) {
            return invalid("destination range out of bounds");
        }

        void* dstBytes = nullptr;
        const uint32_t dstLockOffset = static_cast<uint32_t>(dstByteStart);
        const uint32_t dstLockSize = static_cast<uint32_t>(dstByteEnd - dstByteStart);
        HRESULT hr = D3D_OK;
        for (UINT unique = 0; unique < uniqueSrcCount; ++unique) {
            hr = hr32(dxmt9c_buffer_lock(
                uniqueSrcRaw[unique], 0,
                static_cast<uint32_t>(uniqueSrcLockSize[unique]),
                &uniqueSrcBytes[unique], D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE));
            if (FAILED(hr) || !uniqueSrcBytes[unique]) {
                for (UINT unlock = 0; unlock < unique; ++unlock) {
                    (void)dxmt9c_buffer_unlock(uniqueSrcRaw[unlock]);
                }
                return FAILED(hr) ? hr : D3DERR_INVALIDCALL;
            }
        }
        void* srcBytes[D9C_DRAW_PACKET_MAX_STREAMS]{};
        for (UINT stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            if (srcReadBytes[stream] == 0u) continue;
            for (UINT unique = 0; unique < uniqueSrcCount; ++unique) {
                if (uniqueSrcRaw[unique] == srcRaw[stream]) {
                    srcBytes[stream] = uniqueSrcBytes[unique];
                    break;
                }
            }
        }
        hr = hr32(dxmt9c_buffer_lock(dstRaw, dstLockOffset, dstLockSize, &dstBytes, 0));
        if (FAILED(hr) || !dstBytes) {
            for (UINT unique = 0; unique < uniqueSrcCount; ++unique) {
                (void)dxmt9c_buffer_unlock(uniqueSrcRaw[unique]);
            }
            return FAILED(hr) ? hr : D3DERR_INVALIDCALL;
        }

        const auto& vp = peState_.viewportShadow;
        const float scaleX = static_cast<float>(vp.width) * 0.5f;
        const float scaleY = static_cast<float>(vp.height) * 0.5f;
        const float offsetX = static_cast<float>(vp.x) + scaleX;
        const float offsetY = static_cast<float>(vp.y) + scaleY;
        const float zScale = vp.maxZ - vp.minZ;
        const D9CMatrix wvp = worldViewProjectionTransform();
        const DWORD processAmbient = processLighting ? renderStateValue(D3DRS_AMBIENT) : 0u;
        const uint8_t* srcBase[D9C_DRAW_PACKET_MAX_STREAMS]{};
        for (UINT stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            srcBase[stream] = static_cast<const uint8_t*>(srcBytes[stream]);
        }
        auto sourceOffset = [&](UINT stream, UINT vertex) {
            const uint64_t element = processStreamInstanced(stream) ? 0u : vertex;
            return srcByteStart[stream] + element * streamStr_[stream];
        };
        std::array<std::array<float, 4>, 256> shaderConstF{};
        if (programmable && !peConsts_.vsConstF.values.empty()) {
            const size_t bytes = std::min(peConsts_.vsConstF.values.size(),
                                          shaderConstF.size() * sizeof(shaderConstF[0]));
            std::memcpy(shaderConstF.data(), peConsts_.vsConstF.values.data(), bytes);
        }
        std::array<std::array<int32_t, 4>, 16> shaderConstI{};
        if (programmable && !peConsts_.vsConstI.values.empty()) {
            const size_t bytes = std::min(peConsts_.vsConstI.values.size(),
                                          shaderConstI.size() * sizeof(shaderConstI[0]));
            std::memcpy(shaderConstI.data(), peConsts_.vsConstI.values.data(), bytes);
        }
        SimpleVsTextureState shaderTextures{};
        if (programmable) {
            for (UINT sampler = 0; sampler < shaderTextures.vertexTextures.size(); ++sampler) {
                const UINT samplerSlot = kPeFragmentSamplerSlots + sampler;
                const auto samplerStateValue =
                    [&](D3DSAMPLERSTATETYPE type, DWORD fallback) -> DWORD {
                        uint32_t stateSlot = 0;
                        uint32_t value = 0;
                        if (!samplerStateSlot(type, stateSlot)) {
                            return fallback;
                        }
                        if (peState_.samplerStateShadow.get(
                                samplerSlot, stateSlot, value)) {
                            return value;
                        }
                        return dxmt9c_device_get_sampler_state(
                            dev_, samplerSlot, static_cast<uint32_t>(type));
                    };
                shaderTextures.vertexTextures[sampler] =
                    rawTex(textures_[samplerSlot]);
                shaderTextures.addressU[sampler] =
                    samplerStateValue(D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                shaderTextures.addressV[sampler] =
                    samplerStateValue(D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                shaderTextures.borderColor[sampler] =
                    samplerStateValue(D3DSAMP_BORDERCOLOR, 0u);
                shaderTextures.minMipLevel[sampler] =
                    std::max<DWORD>(
                        textures_[samplerSlot] ? textures_[samplerSlot]->GetLOD() : 0u,
                        samplerStateValue(D3DSAMP_MAXMIPLEVEL, 0u));
            }
        }
        auto* dstBase = static_cast<uint8_t*>(dstBytes);
        for (UINT i = 0; i < vertexCount; ++i) {
            auto* dstVertex = dstBase + static_cast<size_t>(i) * dstLayout.stride;
            float clip[4]{};
            float psizeOut = 0.0f;
            float diffuseOut[4]{};
            float specularOut[4]{};
            float texOut[8][4]{};
            float fixedPosition[3]{};
            if (programmable) {
                SimpleVsRegisters regs{};
                regs.constant = shaderConstF;
                regs.constantInt = shaderConstI;
                auto loadPositionInput = [&](int reg) {
                    if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                    float in[4]{0.0f, 0.0f, 0.0f, 1.0f};
                    const auto* positionSource =
                        srcBase[srcLayout.positionStream] +
                        sourceOffset(srcLayout.positionStream, i) +
                        srcLayout.positionOffset;
                    std::memcpy(in, positionSource,
                                std::min<UINT>(srcLayout.positionBytes, sizeof(in)));
                    regs.input[reg] = {in[0], in[1], in[2], in[3]};
                    return true;
                };
                auto loadColorInput = [&](int reg, UINT stream, UINT offset) {
                    if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                    DWORD color = 0;
                    const auto* colorSource =
                        srcBase[stream] + sourceOffset(stream, i) + offset;
                    std::memcpy(&color, colorSource, sizeof(color));
                    unpackD3DColor(color, regs.input[reg].data());
                    return true;
                };
                auto loadDeclVectorInput =
                    [&](int reg, UINT stream, UINT offset, UINT type, UINT bytes) {
                    if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                    const auto* source =
                        srcBase[stream] + sourceOffset(stream, i) + offset;
                    regs.input[reg] = {0.0f, 0.0f, 0.0f, 1.0f};
                    switch (type) {
                        case D3DDECLTYPE_FLOAT1:
                        case D3DDECLTYPE_FLOAT2:
                        case D3DDECLTYPE_FLOAT3:
                        case D3DDECLTYPE_FLOAT4: {
                            if (bytes == 0u || bytes > sizeof(float) * 4u ||
                                (bytes % sizeof(float)) != 0u) return false;
                            const UINT components = bytes / sizeof(float);
                            std::memcpy(regs.input[reg].data(), source,
                                        std::min<UINT>(components, 4u) * sizeof(float));
                            return true;
                        }
                        case D3DDECLTYPE_SHORT4: {
                            int16_t in[4]{};
                            std::memcpy(in, source, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = static_cast<float>(in[c]);
                            }
                            return true;
                        }
                        case D3DDECLTYPE_UBYTE4: {
                            uint8_t in[4]{};
                            std::memcpy(in, source, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = static_cast<float>(in[c]);
                            }
                            return true;
                        }
                        case D3DDECLTYPE_SHORT2N: {
                            int16_t in[2]{};
                            std::memcpy(in, source, sizeof(in));
                            regs.input[reg][0] = snorm16ToFloat(in[0]);
                            regs.input[reg][1] = snorm16ToFloat(in[1]);
                            return true;
                        }
                        case D3DDECLTYPE_SHORT4N: {
                            int16_t in[4]{};
                            std::memcpy(in, source, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = snorm16ToFloat(in[c]);
                            }
                            return true;
                        }
                        case D3DDECLTYPE_USHORT2N: {
                            uint16_t in[2]{};
                            std::memcpy(in, source, sizeof(in));
                            regs.input[reg][0] = unorm16ToFloat(in[0]);
                            regs.input[reg][1] = unorm16ToFloat(in[1]);
                            return true;
                        }
                        case D3DDECLTYPE_USHORT4N: {
                            uint16_t in[4]{};
                            std::memcpy(in, source, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = unorm16ToFloat(in[c]);
                            }
                            return true;
                        }
                        case D3DDECLTYPE_UBYTE4N: {
                            uint8_t in[4]{};
                            std::memcpy(in, source, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = static_cast<float>(in[c]) / 255.0f;
                            }
                            return true;
                        }
                        case D3DDECLTYPE_DEC3N: {
                            uint32_t packed = 0;
                            std::memcpy(&packed, source, sizeof(packed));
                            regs.input[reg][0] = snorm10ToFloat(packed);
                            regs.input[reg][1] = snorm10ToFloat(packed >> 10u);
                            regs.input[reg][2] = snorm10ToFloat(packed >> 20u);
                            return true;
                        }
                        case D3DDECLTYPE_UDEC3: {
                            uint32_t packed = 0;
                            std::memcpy(&packed, source, sizeof(packed));
                            regs.input[reg][0] = static_cast<float>(packed & 0x3ffu);
                            regs.input[reg][1] = static_cast<float>((packed >> 10u) & 0x3ffu);
                            regs.input[reg][2] = static_cast<float>((packed >> 20u) & 0x3ffu);
                            return true;
                        }
                        case D3DDECLTYPE_FLOAT16_2: {
                            uint16_t in[2]{};
                            std::memcpy(in, source, sizeof(in));
                            regs.input[reg][0] = halfToFloat(in[0]);
                            regs.input[reg][1] = halfToFloat(in[1]);
                            return true;
                        }
                        case D3DDECLTYPE_FLOAT16_4: {
                            uint16_t in[4]{};
                            std::memcpy(in, source, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = halfToFloat(in[c]);
                            }
                            return true;
                        }
                        default:
                            return false;
                    }
                };
                auto loadFloatVectorInput =
                    [&](int reg, UINT stream, UINT offset, UINT bytes) {
                        if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                        if (bytes == 0u || bytes > sizeof(float) * 4u ||
                            (bytes % sizeof(float)) != 0u) return false;
                        float in[4]{0.0f, 0.0f, 0.0f, 1.0f};
                        const auto* source =
                            srcBase[stream] + sourceOffset(stream, i) + offset;
                        std::memcpy(in, source, bytes);
                        regs.input[reg] = {in[0], in[1], in[2], in[3]};
                        return true;
                    };
                auto loadUbyte4Input = [&](int reg, UINT stream, UINT offset) {
                    if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                    uint8_t in[4]{};
                    const auto* source =
                        srcBase[stream] + sourceOffset(stream, i) + offset;
                    std::memcpy(in, source, sizeof(in));
                    regs.input[reg] = {
                        static_cast<float>(in[0]),
                        static_cast<float>(in[1]),
                        static_cast<float>(in[2]),
                        static_cast<float>(in[3]),
                    };
                    return true;
                };
                auto loadTexInput = [&](int reg, UINT tex) {
                    if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                    const auto* texSource =
                        srcBase[srcLayout.texStream[tex]] +
                        sourceOffset(srcLayout.texStream[tex], i) +
                        srcLayout.texOffset[tex];
                    regs.input[reg] = {0.0f, 0.0f, 0.0f, 1.0f};
                    switch (srcLayout.texType[tex]) {
                        case D3DDECLTYPE_FLOAT1:
                        case D3DDECLTYPE_FLOAT2:
                        case D3DDECLTYPE_FLOAT3:
                        case D3DDECLTYPE_FLOAT4: {
                            const UINT components = srcLayout.texBytes[tex] / sizeof(float);
                            std::memcpy(regs.input[reg].data(), texSource,
                                        std::min<UINT>(components, 4u) * sizeof(float));
                            return true;
                        }
                        case D3DDECLTYPE_SHORT2: {
                            int16_t in[2]{};
                            std::memcpy(in, texSource, sizeof(in));
                            regs.input[reg][0] = static_cast<float>(in[0]);
                            regs.input[reg][1] = static_cast<float>(in[1]);
                            return true;
                        }
                        case D3DDECLTYPE_SHORT4: {
                            int16_t in[4]{};
                            std::memcpy(in, texSource, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = static_cast<float>(in[c]);
                            }
                            return true;
                        }
                        case D3DDECLTYPE_UBYTE4: {
                            uint8_t in[4]{};
                            std::memcpy(in, texSource, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = static_cast<float>(in[c]);
                            }
                            return true;
                        }
                        case D3DDECLTYPE_SHORT2N: {
                            int16_t in[2]{};
                            std::memcpy(in, texSource, sizeof(in));
                            regs.input[reg][0] = snorm16ToFloat(in[0]);
                            regs.input[reg][1] = snorm16ToFloat(in[1]);
                            return true;
                        }
                        case D3DDECLTYPE_SHORT4N: {
                            int16_t in[4]{};
                            std::memcpy(in, texSource, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = snorm16ToFloat(in[c]);
                            }
                            return true;
                        }
                        case D3DDECLTYPE_USHORT2N: {
                            uint16_t in[2]{};
                            std::memcpy(in, texSource, sizeof(in));
                            regs.input[reg][0] = unorm16ToFloat(in[0]);
                            regs.input[reg][1] = unorm16ToFloat(in[1]);
                            return true;
                        }
                        case D3DDECLTYPE_USHORT4N: {
                            uint16_t in[4]{};
                            std::memcpy(in, texSource, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = unorm16ToFloat(in[c]);
                            }
                            return true;
                        }
                        case D3DDECLTYPE_UBYTE4N: {
                            uint8_t in[4]{};
                            std::memcpy(in, texSource, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = static_cast<float>(in[c]) / 255.0f;
                            }
                            return true;
                        }
                        case D3DDECLTYPE_UDEC3: {
                            uint32_t packed = 0;
                            std::memcpy(&packed, texSource, sizeof(packed));
                            regs.input[reg][0] = static_cast<float>(packed & 0x3ffu);
                            regs.input[reg][1] = static_cast<float>((packed >> 10u) & 0x3ffu);
                            regs.input[reg][2] = static_cast<float>((packed >> 20u) & 0x3ffu);
                            return true;
                        }
                        case D3DDECLTYPE_FLOAT16_2: {
                            uint16_t in[2]{};
                            std::memcpy(in, texSource, sizeof(in));
                            regs.input[reg][0] = halfToFloat(in[0]);
                            regs.input[reg][1] = halfToFloat(in[1]);
                            return true;
                        }
                        case D3DDECLTYPE_FLOAT16_4: {
                            uint16_t in[4]{};
                            std::memcpy(in, texSource, sizeof(in));
                            for (UINT c = 0; c < 4u; ++c) {
                                regs.input[reg][c] = halfToFloat(in[c]);
                            }
                            return true;
                        }
                        default:
                            return false;
                    }
                };
                if (shaderIo.inputPosition >= 0 && !loadPositionInput(shaderIo.inputPosition)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (shaderIo.inputNormal >= 0 &&
                    !loadDeclVectorInput(shaderIo.inputNormal, srcLayout.normalStream,
                                         srcLayout.normalOffset, srcLayout.normalType,
                                         srcLayout.normalBytes)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (shaderIo.inputTangent >= 0 &&
                    !loadDeclVectorInput(shaderIo.inputTangent, srcLayout.tangentStream,
                                         srcLayout.tangentOffset, srcLayout.tangentType,
                                         srcLayout.tangentBytes)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (shaderIo.inputBinormal >= 0 &&
                    !loadDeclVectorInput(shaderIo.inputBinormal, srcLayout.binormalStream,
                                         srcLayout.binormalOffset, srcLayout.binormalType,
                                         srcLayout.binormalBytes)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (shaderIo.inputBlendWeight >= 0 &&
                    !loadFloatVectorInput(shaderIo.inputBlendWeight,
                                          srcLayout.blendWeightStream,
                                          srcLayout.blendWeightOffset,
                                          srcLayout.blendWeightBytes)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (shaderIo.inputBlendIndices >= 0 &&
                    !loadUbyte4Input(shaderIo.inputBlendIndices,
                                     srcLayout.blendIndicesStream,
                                     srcLayout.blendIndicesOffset)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (shaderIo.inputPSize >= 0 &&
                    !loadFloatVectorInput(shaderIo.inputPSize,
                                          srcLayout.psizeStream,
                                          srcLayout.psizeOffset, 4u)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (shaderIo.inputDiffuse >= 0 &&
                    !loadColorInput(shaderIo.inputDiffuse, srcLayout.diffuseStream,
                                    srcLayout.diffuseOffset)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (shaderIo.inputSpecular >= 0 &&
                    !loadColorInput(shaderIo.inputSpecular, srcLayout.specularStream,
                                    srcLayout.specularOffset)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                for (UINT tex = 0; tex < 8; ++tex) {
                    if (shaderIo.inputTex[tex] >= 0 &&
                        !loadTexInput(shaderIo.inputTex[tex], tex)) {
                        hr = D3DERR_INVALIDCALL;
                        break;
                    }
                }
                if (FAILED(hr)) break;
                for (UINT genericIndex = 0;
                     genericIndex < shaderIo.inputGenericCount; ++genericIndex) {
                    const auto& shaderGeneric = shaderIo.inputGeneric[genericIndex];
                    const auto* generic = findGenericInput(
                        shaderGeneric.usage, shaderGeneric.usageIndex);
                    if (!generic ||
                        !loadDeclVectorInput(shaderGeneric.reg,
                                             generic->stream,
                                             generic->offset,
                                             generic->type,
                                             generic->bytes)) {
                        hr = D3DERR_INVALIDCALL;
                        break;
                    }
                }
                if (FAILED(hr)) break;
                if (!executeSimpleProcessVertexShader(
                        shaderWords, shaderIo, regs, &shaderTextures)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                const auto* positionReg =
                    simpleVsRegister(regs, shaderIo.major, shaderIo.outputPosition.type,
                                     shaderIo.outputPosition.index);
                if (!positionReg) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                std::memcpy(clip, positionReg->data(), sizeof(clip));
                if (dstLayout.psize) {
                    const auto* psizeReg =
                        simpleVsRegister(regs, shaderIo.major, shaderIo.outputPSize.type,
                                         shaderIo.outputPSize.index);
                    if (!psizeReg) {
                        hr = D3DERR_INVALIDCALL;
                        break;
                    }
                    psizeOut = (*psizeReg)[0];
                }
                if (dstLayout.diffuse) {
                    const auto* colorReg =
                        simpleVsRegister(regs, shaderIo.major, shaderIo.outputDiffuse.type,
                                         shaderIo.outputDiffuse.index);
                    if (!colorReg) {
                        hr = D3DERR_INVALIDCALL;
                        break;
                    }
                    std::memcpy(diffuseOut, colorReg->data(), sizeof(diffuseOut));
                }
                if (dstLayout.specular) {
                    const auto* colorReg =
                        simpleVsRegister(regs, shaderIo.major, shaderIo.outputSpecular.type,
                                         shaderIo.outputSpecular.index);
                    if (!colorReg) {
                        hr = D3DERR_INVALIDCALL;
                        break;
                    }
                    std::memcpy(specularOut, colorReg->data(), sizeof(specularOut));
                }
                for (UINT tex = 0; tex < dstLayout.texCount; ++tex) {
                    if (dstLayout.texBytes[tex] == 0u) continue;
                    const auto* texReg =
                        simpleVsRegister(regs, shaderIo.major, shaderIo.outputTex[tex].type,
                                         shaderIo.outputTex[tex].index);
                    if (!texReg) {
                        hr = D3DERR_INVALIDCALL;
                        break;
                    }
                    std::memcpy(texOut[tex], texReg->data(), sizeof(texOut[tex]));
                }
                if (FAILED(hr)) break;
            } else {
                float in[3]{};
                const auto* positionSource =
                    srcBase[srcLayout.positionStream] +
                    sourceOffset(srcLayout.positionStream, i) +
                    srcLayout.positionOffset;
                std::memcpy(in, positionSource, sizeof(in));
                fixedPosition[0] = in[0];
                fixedPosition[1] = in[1];
                fixedPosition[2] = in[2];
                if (dstLayout.psize) {
                    const auto* psizeSource =
                        srcBase[srcLayout.psizeStream] +
                        sourceOffset(srcLayout.psizeStream, i) +
                        srcLayout.psizeOffset;
                    std::memcpy(&psizeOut, psizeSource, sizeof(psizeOut));
                }
                const float position[4] = {in[0], in[1], in[2], 1.0f};
                for (UINT col = 0; col < 4; ++col) {
                    clip[col] = position[0] * wvp.m[col] +
                                position[1] * wvp.m[4 + col] +
                                position[2] * wvp.m[8 + col] +
                                position[3] * wvp.m[12 + col];
                }
            }
            const float invW = clip[3] != 0.0f ? 1.0f / clip[3] : 1.0f;
            const float ndcX = clip[0] * invW;
            const float ndcY = clip[1] * invW;
            const float ndcZ = clip[2] * invW;
            float viewportZ = vp.minZ + ndcZ * zScale;
            if (renderStateValue(D3DRS_CLIPPING) == 0u) {
                const float minDepth = std::min(vp.minZ, vp.maxZ);
                const float maxDepth = std::max(vp.minZ, vp.maxZ);
                viewportZ = std::clamp(viewportZ, minDepth, maxDepth);
            }
            float out[4] = {
                ndcX * scaleX + offsetX,
                -ndcY * scaleY + offsetY,
                viewportZ,
                invW,
            };
            std::memcpy(dstVertex + dstLayout.positionOffset, out, sizeof(out));
            if (dstLayout.psize) {
                std::memcpy(dstVertex + dstLayout.psizeOffset,
                            &psizeOut, sizeof(psizeOut));
            }
            ProcessFixedFunctionLightingColors lightingColors{};
            bool lightingColorsReady = false;
            auto fixedFunctionLightingColors = [&]() -> const ProcessFixedFunctionLightingColors& {
                if (!lightingColorsReady) {
                    float normal[3]{};
                    const auto* normalSource =
                        srcBase[srcLayout.normalStream] +
                        sourceOffset(srcLayout.normalStream, i) +
                        srcLayout.normalOffset;
                    std::memcpy(normal, normalSource, sizeof(normal));
                    D9CMaterial material = peState_.materialShadow;
                    auto readMaterialColor = [&](DWORD source,
                                                 D9CColorRGBA& target) {
                        if (!processColorVertex || source == D3DMCS_MATERIAL) {
                            return true;
                        }
                        UINT stream = 0;
                        UINT offset = 0;
                        if (source == D3DMCS_COLOR1 && srcLayout.diffuse) {
                            stream = srcLayout.diffuseStream;
                            offset = srcLayout.diffuseOffset;
                        } else if (source == D3DMCS_COLOR2 && srcLayout.specular) {
                            stream = srcLayout.specularStream;
                            offset = srcLayout.specularOffset;
                        } else {
                            return false;
                        }
                        DWORD color = 0;
                        const auto* colorSource =
                            srcBase[stream] + sourceOffset(stream, i) +
                            offset;
                        std::memcpy(&color, colorSource, sizeof(color));
                        target = d3dColorToRgba(color);
                        return true;
                    };
                    if (!readMaterialColor(renderStateValue(D3DRS_DIFFUSEMATERIALSOURCE),
                                           material.diffuse) ||
                        !readMaterialColor(renderStateValue(D3DRS_AMBIENTMATERIALSOURCE),
                                           material.ambient) ||
                        !readMaterialColor(renderStateValue(D3DRS_EMISSIVEMATERIALSOURCE),
                                           material.emissive) ||
                        (processSpecularLighting &&
                         !readMaterialColor(renderStateValue(D3DRS_SPECULARMATERIALSOURCE),
                                            material.specular))) {
                        lightingColors = {};
                        lightingColorsReady = true;
                        return lightingColors;
                    }
                    lightingColors = processFixedFunctionLightingColors(
                        fixedPosition, normal, material, processAmbient,
                        peState_.lightShadow, peState_.lightEnableShadow,
                        processSpecularLighting);
                    lightingColorsReady = true;
                }
                return lightingColors;
            };
            if (dstLayout.diffuse) {
                if (programmable) {
                    const DWORD color = packD3DColor(diffuseOut);
                    std::memcpy(dstVertex + dstLayout.diffuseOffset, &color, sizeof(color));
                } else if (processLighting) {
                    const DWORD color = fixedFunctionLightingColors().diffuse;
                    std::memcpy(dstVertex + dstLayout.diffuseOffset, &color, sizeof(color));
                } else {
                    const auto* diffuseSource =
                        srcBase[srcLayout.diffuseStream] + sourceOffset(srcLayout.diffuseStream, i) +
                        srcLayout.diffuseOffset;
                    std::memcpy(dstVertex + dstLayout.diffuseOffset,
                                diffuseSource, 4u);
                }
            }
            if (dstLayout.specular) {
                if (programmable) {
                    const DWORD color = packD3DColor(specularOut);
                    std::memcpy(dstVertex + dstLayout.specularOffset, &color, sizeof(color));
                } else if (processSpecularLighting) {
                    const DWORD color = fixedFunctionLightingColors().specular;
                    std::memcpy(dstVertex + dstLayout.specularOffset, &color, sizeof(color));
                } else {
                    const auto* specularSource =
                        srcBase[srcLayout.specularStream] +
                        sourceOffset(srcLayout.specularStream, i) +
                        srcLayout.specularOffset;
                    std::memcpy(dstVertex + dstLayout.specularOffset,
                                specularSource, 4u);
                }
            }
            for (UINT tex = 0; tex < dstLayout.texCount; ++tex) {
                if (dstLayout.texBytes[tex] == 0u) continue;
                if (programmable) {
                    std::memcpy(dstVertex + dstLayout.texOffset[tex],
                                texOut[tex], dstLayout.texBytes[tex]);
                } else {
                    const auto* texSource =
                        srcBase[srcLayout.texStream[tex]] + sourceOffset(srcLayout.texStream[tex], i) +
                        srcLayout.texOffset[tex];
                    std::memcpy(dstVertex + dstLayout.texOffset[tex],
                                texSource, dstLayout.texBytes[tex]);
                }
            }
        }
        const HRESULT dstUnlockHr = hr32(dxmt9c_buffer_unlock(dstRaw));
        HRESULT srcUnlockHr = D3D_OK;
        for (UINT unique = 0; unique < uniqueSrcCount; ++unique) {
            const HRESULT oneHr = hr32(dxmt9c_buffer_unlock(uniqueSrcRaw[unique]));
            if (SUCCEEDED(srcUnlockHr) && FAILED(oneHr)) srcUnlockHr = oneHr;
        }
        if (FAILED(dstUnlockHr)) return dstUnlockHr;
        if (FAILED(srcUnlockHr)) return srcUnlockHr;
        if (FAILED(hr)) return hr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) noexcept override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) noexcept override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) noexcept override {
        // stub: Wine returns S_OK; patch primitives unused on Metal.
        return S_OK;
    }

    /* ── query ── */
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type,
                                           IDirect3DQuery9** ppQ) noexcept override {
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
        return S_OK;
    }

    /* ── IDirect3DDevice9Ex ── */

    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT,UINT,float*,float*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9*,IDirect3DSurface9*,
                                            IDirect3DVertexBuffer9*,UINT,
                                            IDirect3DVertexBuffer9*,
                                            D3DCOMPOSERECTSOP,int,int) noexcept override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE PresentEx(const RECT* src, const RECT* dst,
                                         HWND wnd, const RGNDATA* dirty,
                                         DWORD flags) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::Present);
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_device_present(dev_,
            src ? &cs : nullptr, dst ? &cd : nullptr,
            (uint64_t)(uintptr_t)wnd, dirty, flags));
        if (SUCCEEDED(hr)) {
            logPeRecorderStats("present_ex");
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* p) noexcept override {
        // stub: Wine returns S_OK; GPU thread priority is not exposed by Metal.
        if (p) *p = 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) noexcept override {
        // stub: Wine returns S_OK; GPU thread priority is not exposed by Metal.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT sc) noexcept override {
        return hr32(dxmt9c_device_wait_for_vblank(dev_, sc));
    }

    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9**,
                                                      UINT32) noexcept override {
        // stub: Wine returns S_OK; unified memory on Apple Silicon — all resources
        // are resident.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT maxLatency) noexcept override {
        // Wine d3d9ex test_frame_latency contract: valid range is 1..30.
        // 0 or >= 31 must return D3DERR_INVALIDCALL.
        if (maxLatency == 0 || maxLatency >= 31)
            return D3DERR_INVALIDCALL;
        maxFrameLatencyShadow_ = maxLatency;
        return hr32(dxmt9c_device_set_maximum_frame_latency(dev_, maxLatency));
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* p) noexcept override {
        if (!p) return D3DERR_INVALIDCALL;
        // PE-shadow: return value previously set or the default of 3.
        *p = maxFrameLatencyShadow_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND wnd) noexcept override {
        return hr32(dxmt9c_device_check_device_state(dev_,
                    (uint64_t)(uintptr_t)wnd));
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT w, UINT h,
                                                    D3DFORMAT fmt,
                                                    D3DMULTISAMPLE_TYPE ms,
                                                    DWORD msQual, BOOL lockable,
                                                    IDirect3DSurface9** ppS,
                                                    HANDLE* psh,
                                                    DWORD usage) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        return CreateRenderTarget(w, h, fmt, ms, msQual, lockable, ppS, psh);
    }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT w, UINT h,
                                                             D3DFORMAT fmt,
                                                             D3DPOOL pool,
                                                             IDirect3DSurface9** ppS,
                                                             HANDLE* psh,
                                                             DWORD usage) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        return CreateOffscreenPlainSurface(w, h, fmt, pool, ppS, psh);
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DMULTISAMPLE_TYPE ms,
                                                           DWORD msQual,
                                                           BOOL discard,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE* psh,
                                                           DWORD usage) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        return CreateDepthStencilSurface(w, h, fmt, ms, msQual, discard, ppS, psh);
    }

    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS* pPP,
                                       D3DDISPLAYMODEEX* pFsMode) noexcept override {
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
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT sc,
                                                D3DDISPLAYMODEEX* pMode,
                                                D3DDISPLAYROTATION* pRot) noexcept override {
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
};

/* =========================================================================
 * Factory function (called from factory.cpp)
 * ========================================================================= */

IDirect3DDevice9Ex* CreateDeviceImpl(D9CDevice* dev, IDirect3D9Ex* pFactory,
                                     UINT adapter, D3DDEVTYPE deviceType,
                                     DWORD behaviorFlags,
                                     HWND window, bool extended,
                                     DWORD implicitSwapchainFlags) {
    return new D3D9DeviceImpl(dev, pFactory, adapter, deviceType,
                              behaviorFlags, window, extended,
                              implicitSwapchainFlags);
}
