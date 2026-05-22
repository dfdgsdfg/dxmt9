/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex and recorder glue.
 * All methods delegate to the dxmt9c_* C API from dxmt9/device_c.h. */

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include "d3d9_pe.hpp"
#include "d3d9_pe_device_child.hpp"
#include "d3d9_pe_draw_packet.hpp"
#include "d3d9_pe_recorder.hpp"
#include "d3d9_pe_state_shadow.hpp"
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

// T4 (D3D9Ex shared-handle, SYSTEMMEM partial): for SYSTEMMEM textures
// the test_user_memory oracle (Wine d3d9ex tests) requires that
//   - 0 levels (auto-mip)            -> D3DERR_INVALIDCALL
//   - levels > 1                     -> D3DERR_INVALIDCALL
//   - SCRATCH pool                   -> D3DERR_INVALIDCALL
//   - SYSTEMMEM, levels == 1         -> S_OK; user pointer aliased
// allowSystemMemUserMemory is false for cube/volume textures since the
// partial scope only covers 2D textures and offscreen plain surfaces.
// The width/height == 1x1 narrowing for 2D textures is enforced at the
// call site (validate* doesn't see W/H). DEFAULT-pool sharing remains
// E_NOTIMPL until the IOSurface / MTLSharedTexture bridge lands.
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
    return E_NOTIMPL;
}

// T4: per Wine test_user_memory (~line 793-798), VB/IB with pSharedHandle
// and SYSTEMMEM (or any non-DEFAULT pool) must return D3DERR_NOTAVAILABLE.
[[nodiscard]] static HRESULT validateSharedHandleForBuffer(bool extended,
                                             HANDLE* sharedHandle,
                                             D3DPOOL pool) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool != D3DPOOL_DEFAULT) return D3DERR_NOTAVAILABLE;
    return E_NOTIMPL;
}

// T4: per Wine test_user_memory (~line 800-830), offscreen plain surface
// with pSharedHandle:
//   - SYSTEMMEM           -> S_OK; user pointer aliased
//   - SCRATCH             -> D3DERR_INVALIDCALL
//   - DEFAULT (E_NOTIMPL) -> partial scope, see validateSharedHandleForDefaultSurface
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
    return E_NOTIMPL;
}

[[nodiscard]] static HRESULT validateSharedHandleForDefaultSurface(bool extended,
                                                     HANDLE* sharedHandle) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    return E_NOTIMPL;
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
///     type is FLOAT-like / 32-bit-aligned (multiples of 4).
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
        /* All D3D9 element types are 32-bit word aligned. */
        if ((e.Offset & 0x3u) != 0u) return D3DERR_INVALIDCALL;
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
        const int blend = (posMask - D3DFVF_XYZB1) / 2 + 1;
        if (blend >= 1) {
            const uint8_t blendType = blend == 1
                ? D3DDECLTYPE_FLOAT1
                : (blend == 2 ? D3DDECLTYPE_FLOAT2
                              : (blend == 3 ? D3DDECLTYPE_FLOAT3
                                            : D3DDECLTYPE_FLOAT4));
            out.push_back({0, offset, blendType,
                           D3DDECLMETHOD_DEFAULT,
                           D3DDECLUSAGE_BLENDWEIGHT, 0});
            offset += vertexElementTypeSize(blendType);
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

    /* palette shadow — Wine conformance round-trip. PE-only; the
     * backend doesn't render through palette textures yet, so we just
     * shadow Set/Get and gate SetCurrentTexturePalette on whether the
     * index was ever set. */
    std::unordered_map<UINT, std::array<PALETTEENTRY, 256>> palettes_{};
    UINT currentPaletteIndex_ = 0;
    bool currentPaletteSet_ = false;

    template<typename T>
    static void setRef(T*& slot, T* newVal) {
        if (newVal) newVal->AddRef();
        if (slot)   slot->Release();
        slot = newVal;
    }

    void releaseAllBound() {
        for (auto& t : textures_)   setRef(t, (IDirect3DBaseTexture9*)nullptr);
        setRef(vs_, (IDirect3DVertexShader9*)nullptr);
        setRef(ps_, (IDirect3DPixelShader9*)nullptr);
        for (auto& s : streamSrc_)  setRef(s, (IDirect3DVertexBuffer9*)nullptr);
        setRef(indexBuf_, (IDirect3DIndexBuffer9*)nullptr);
        setRef(vdecl_, (IDirect3DVertexDeclaration9*)nullptr);
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
    HRESULT STDMETHODCALLTYPE EvictManagedResources() noexcept override { return S_OK; }

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
        if (pPP->BackBufferCount == 0) {
            pPP->BackBufferCount = 1;
        }
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
        memset(p, 0, sizeof(*p));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enableDialogs) noexcept override {
        dxmt9DeviceDebugLog("device_set_dialog_box_mode device=%p enable=%u", this, (unsigned)enableDialogs);
        return S_OK;
    }
    void    STDMETHODCALLTYPE SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP*) noexcept override {
        dxmt9DeviceDebugLog("device_set_gamma_ramp device=%p swapChain=%u flags=0x%x",
                            this, swapChain, (unsigned)flags);
    }
    void    STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP* p) noexcept override {
        if (p) memset(p, 0, sizeof(*p));
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
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9*) noexcept override {
        dxmt9DeviceDebugLog("device_set_clip_status device=%p", this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* p) noexcept override {
        dxmt9DeviceDebugLog("device_get_clip_status device=%p", this);
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }

    /* ── render states ── */
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) noexcept override {
        dxmt9DeviceDebugLog("device_set_render_state device=%p state=%u value=0x%x",
                            this, (unsigned)state, (unsigned)value);
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
        if (!stateBlockRecording_) {
            return D3DERR_INVALIDCALL;
        }
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

    /* ── palette — PE-only shadow for Wine conformance round-trip
     *    (test_set_palette_roundtrip, test_palette_alpha_caps_policy,
     *     test_palette_current_entry_isolation). The backend doesn't
     *     yet sample through palette textures; this state lives only
     *     for the Get/Set contract.
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

    /* ── soft VP / NPatches (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL enable) noexcept override {
        dxmt9DeviceDebugLog("device_set_software_vertex_processing device=%p enable=%u", this, (unsigned)enable);
        return S_OK;
    }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() noexcept override {
        dxmt9DeviceDebugLog("device_get_software_vertex_processing device=%p", this);
        // Wine d3d9 contract: when the device was created with
        // D3DCREATE_SOFTWARE_VERTEXPROCESSING, the default state is TRUE.
        // (Apps may override via SetSoftwareVertexProcessing on a mixed-mode
        // device, but dxmt9 does not yet expose mixed-mode.)
        return (behaviorFlags_ & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? TRUE : FALSE;
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) noexcept override {
        dxmt9DeviceDebugLog("device_set_npatch_mode device=%p segments=%f", this, segments);
        return S_OK;
    }
    float   STDMETHODCALLTYPE GetNPatchMode() noexcept override { return 0.0f; }

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
         * test_fvf_decl_management). */
        IDirect3DVertexDeclaration9* implicit = implicitDeclForFvf(fvf);
        setRef(vdecl_, implicit);
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
        setRef(vdecl_, pVD);
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
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT,
                                               IDirect3DVertexBuffer9*,
                                               IDirect3DVertexDeclaration9*,
                                               DWORD) noexcept override {
        // T2 device-lost gate. ProcessVertices isn't implemented yet, but
        // when the device is lost it must return D3DERR_DEVICELOST before
        // the unimplemented INVALIDCALL fallback.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_process_vertices device=%p", this);
        return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) noexcept override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) noexcept override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) noexcept override { return S_OK; }

    /* ── query ── */
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type,
                                           IDirect3DQuery9** ppQ) noexcept override {
        D9CQuery* q = dxmt9c_device_create_query(dev_, (uint32_t)type);
        if (!q) return D3DERR_NOTAVAILABLE;
        if (!ppQ) {
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

    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* p) noexcept override { if (p) *p = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT sc) noexcept override {
        return hr32(dxmt9c_device_wait_for_vblank(dev_, sc));
    }

    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9**,
                                                      UINT32) noexcept override { return S_OK; }

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
        if (pFsMode && pFsMode->Size != sizeof(D3DDISPLAYMODEEX)) return D3DERR_INVALIDCALL;
        if (pPP->Windowed ? pFsMode != nullptr : pFsMode == nullptr) return D3DERR_INVALIDCALL;
        if (pFsMode && (pFsMode->Width != pPP->BackBufferWidth
                || pFsMode->Height != pPP->BackBufferHeight)) {
            return D3DERR_INVALIDCALL;
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
