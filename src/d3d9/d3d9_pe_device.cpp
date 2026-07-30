/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex and recorder glue.
 * All methods delegate to the dxmt9c_* C API from dxmt9/device_c.h. */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include "d3d9_pe.hpp"
#include "d3d9_pe_device_child.hpp"
#include "d3d9_pe_chunk_v2_builder.hpp"
#include "d3d9_pe_decimated_scope.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_draw_packet.hpp"
#include "d3d9_pe_recorder.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_stats_decimation.hpp"
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

static void dxmt9WriteStderrLineAtomic(const char* line, std::size_t len) noexcept {
    if (!line || len == 0u) return;
#if defined(_WIN32)
    (void)_write(_fileno(stderr), line, static_cast<unsigned int>(len));
#else
    (void)::write(STDERR_FILENO, line, len);
#endif
}

static void dxmt9PerfLogStderrAtomic(const char* fmt, ...) noexcept {
    char line[512]{};
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (written <= 0) return;
    std::size_t len = static_cast<std::size_t>(written);
    if (len >= sizeof(line)) {
        len = sizeof(line) - 1u;
        line[len - 1u] = '\n';
    }
    dxmt9WriteStderrLineAtomic(line, len);
}

static bool dxmt9PeRecorderStatsEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_RECORDER_STATS");
    return enabled;
}

static bool dxmt9SplitSparseConstRecordsEnabled() {
    static const bool enabled =
        dxmt9::util::getenvFlag("DXMT9_SPLIT_SPARSE_CONST_RECORDS");
    return enabled;
}

static bool dxmt9PerfVsConstSetterRangeEnabled() {
    static const bool enabled =
        dxmt9::util::getenvFlag("DXMT9_PERF_VS_CONST_SETTER_RANGE");
    return enabled;
}

static bool dxmt9PeRecorderChunkLogEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_RECORDER_CHUNK_LOG");
    return enabled;
}

// R-BACK-2.52 (Inline Const Delta, opt-in): read once at first use. Off
// (default/unset) keeps every Draw* record on the pre-existing standalone
// D9C_COMMAND_RECORD_SET_*_CONST_* + fixed-size record path verbatim
// (R-BACK-2.52(a)). On, appendDrawPrimitiveRecord / appendDrawIndexedPrimitiveRecord
// fold the six pending const shadows' merged dirty ranges into the draw
// packet's constDeltaSections + trailing payload instead (R-BACK-2.52(b)).
// Non-draw const consumers (ProcessVertices, chunkBarrierFlush's drain, the
// UP draw variants — see appendDrawPrimitiveUPRecordWithFvf /
// appendDrawIndexedPrimitiveUPRecordWithFvf) are untouched by this flag and
// always use the standalone flush (R-BACK-2.52(e)).
static bool dxmt9PeInlineConstDeltaEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_INLINE_CONST_DELTA");
    return enabled;
}

static double dxmt9ElapsedMs(std::chrono::steady_clock::time_point start,
                             std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static std::int64_t dxmt9SteadyClockNs(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        t.time_since_epoch()).count();
}

// dxmt9PeStatsDecimationN() now lives in d3d9_pe_decimated_scope.hpp so the
// natively-built producer TU can time its own scope.

namespace {
// Shared RAII guard for the two decimated-timing sites owned directly by
// D3D9DeviceImpl (const_flush, draw_packet). Covers every exit path of the
// guarded function, including early returns, by recording the elapsed time
// in its destructor. `stats` stays null (no-op destructor) unless
// PeDecimatedScopeTimer::shouldSample() selected this call for timing.
// DxmtPeDecimatedScopeGuard now lives in d3d9_pe_decimated_scope.hpp.
}  // namespace

static std::uint32_t dxmt9PeCurrentThreadId() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentThreadId());
#else
    return 0;
#endif
}

static thread_local const char* dxmt9PeCurrentCallName = nullptr;
static thread_local PeInterAppendCallFamily dxmt9PeCurrentAppendFamily =
    PeInterAppendCallFamily::Unknown;
static thread_local std::int64_t dxmt9PeCurrentCallEntryNs = 0;

static void dxmt9PeSetCurrentCallName(const char* callName) noexcept {
    dxmt9PeCurrentCallName = callName;
}

class Dxmt9PeAppendFamilyScope {
public:
    explicit Dxmt9PeAppendFamilyScope(PeInterAppendCallFamily family) noexcept
        : previous_(dxmt9PeCurrentAppendFamily) {
        dxmt9PeCurrentAppendFamily = family;
    }

    ~Dxmt9PeAppendFamilyScope() noexcept {
        dxmt9PeCurrentAppendFamily = previous_;
    }

    Dxmt9PeAppendFamilyScope(const Dxmt9PeAppendFamilyScope&) = delete;
    Dxmt9PeAppendFamilyScope& operator=(
        const Dxmt9PeAppendFamilyScope&) = delete;

private:
    PeInterAppendCallFamily previous_;
};

struct PeInterAppendCallSiteLocalKey {
    std::uint32_t prevCallName = 0;
    std::uint32_t nextCallName = 0;
    const void* callerPc = nullptr;

    bool operator==(const PeInterAppendCallSiteLocalKey& other)
        const noexcept {
        return prevCallName == other.prevCallName &&
               nextCallName == other.nextCallName &&
               callerPc == other.callerPc;
    }
};

struct PeInterAppendCallSiteKey {
    std::uint32_t focusPair = 0;
    std::uint32_t prevCallName = 0;
    std::uint32_t nextCallName = 0;
    const void* callerPc = nullptr;

    bool operator==(const PeInterAppendCallSiteKey& other)
        const noexcept {
        return focusPair == other.focusPair &&
               prevCallName == other.prevCallName &&
               nextCallName == other.nextCallName &&
               callerPc == other.callerPc;
    }
};

struct PeInterAppendCallSiteLocalKeyHash {
    std::size_t operator()(const PeInterAppendCallSiteLocalKey& key)
        const noexcept {
        std::size_t h = static_cast<std::size_t>(key.prevCallName);
        h = h * 1315423911u + static_cast<std::size_t>(key.nextCallName);
        h ^= reinterpret_cast<std::uintptr_t>(key.callerPc) +
             0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
};

struct PeInterAppendCallSiteKeyHash {
    std::size_t operator()(const PeInterAppendCallSiteKey& key)
        const noexcept {
        std::size_t h = static_cast<std::size_t>(key.focusPair);
        h = h * 1315423911u + static_cast<std::size_t>(key.prevCallName);
        h = h * 1315423911u + static_cast<std::size_t>(key.nextCallName);
        h ^= reinterpret_cast<std::uintptr_t>(key.callerPc) +
             0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
};

struct PeInterAppendCallSiteStats {
    std::uint64_t samples = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxNs = 0;
};

struct PeInterAppendCallSiteSummary {
    std::uint32_t prevCallName = 0;
    std::uint32_t nextCallName = 0;
    const void* callerPc = nullptr;
    std::uint64_t samples = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxNs = 0;
};

struct Dxmt9PeCallerModuleInfo {
    const void* base = nullptr;
    std::uintptr_t rva = 0;
    std::array<char, 260> path{};
};

static const char* dxmt9PeCallerModuleLeaf(const Dxmt9PeCallerModuleInfo& info) {
    const char* leaf = info.path.data();
    for (const char* p = info.path.data(); *p; ++p) {
        if (*p == '\\' || *p == '/') {
            leaf = p + 1;
        }
    }
    return *leaf ? leaf : "unknown";
}

static Dxmt9PeCallerModuleInfo dxmt9PeResolveCallerModule(
    const void* callerPc) {
    Dxmt9PeCallerModuleInfo info{};
    if (!callerPc) {
        return info;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(callerPc, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        return info;
    }
    info.base = mbi.AllocationBase;
    info.rva = reinterpret_cast<std::uintptr_t>(callerPc) -
               reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
    const DWORD written = GetModuleFileNameA(
        reinterpret_cast<HMODULE>(mbi.AllocationBase),
        info.path.data(), static_cast<DWORD>(info.path.size()));
    if (written == 0) {
        std::strncpy(info.path.data(), "unknown", info.path.size() - 1);
    } else {
        info.path.back() = '\0';
    }
    return info;
}

static void dxmt9PeCaptureCallStack(D3D9PePresentCallToken& sample) {
#if defined(_WIN32)
    void* frames[D3D9PePresentCallStackDepth]{};
    const USHORT count = RtlCaptureStackBackTrace(
        0, static_cast<DWORD>(D3D9PePresentCallStackDepth), frames, nullptr);
    sample.callerStackCount = static_cast<std::uint8_t>(
        std::min<std::size_t>(count, sample.callerStack.size()));
    for (std::size_t i = 0; i < sample.callerStackCount; ++i) {
        sample.callerStack[i] = frames[i];
    }
#else
    (void)sample;
#endif
}

static std::array<char, 2048> dxmt9PeFormatCallerStack(
    const D3D9PePresentCallToken& sample) {
    std::array<char, 2048> out{};
    std::size_t used = 0;
    if (sample.callerStackCount == 0) {
        std::snprintf(out.data(), out.size(), "empty");
        return out;
    }
    for (std::size_t i = 0; i < sample.callerStackCount && used < out.size(); ++i) {
        const auto frameInfo = dxmt9PeResolveCallerModule(sample.callerStack[i]);
        const int written = std::snprintf(
            out.data() + used, out.size() - used, "%s%u:%s+0x%llx@%p",
            i == 0 ? "" : ";", static_cast<unsigned>(i),
            dxmt9PeCallerModuleLeaf(frameInfo),
            static_cast<unsigned long long>(frameInfo.rva),
            sample.callerStack[i]);
        if (written <= 0) {
            break;
        }
        used += std::min<std::size_t>(static_cast<std::size_t>(written),
                                      out.size() - used);
    }
    out.back() = '\0';
    return out;
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
// SOLE APPLICATION SITE: buildDrawPacketFromViews() in
// d3d9_pe_producer.cpp, after the
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
// dxmt9PeFullSnapshotEnabled() now lives in d3d9_pe_producer.hpp beside the
// producer that reads it. Keeping a second copy here would be exactly the
// drift hazard that motivated sharing toWireHandle.

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
constexpr size_t kShaderBoundedScan = 1u << 16;

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
    bool seenEnd = false;
    for (size_t i = 1; i < kShaderBoundedScan; ++i) {
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

[[nodiscard]] std::uint64_t hashValidatedShaderBytecode(const DWORD* code) {
    size_t wordCount = 0;
    for (size_t i = 0; i < kShaderBoundedScan; ++i) {
        if (static_cast<uint32_t>(code[i]) == kShaderEndToken) {
            wordCount = i + 1u;
            break;
        }
    }
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
    UINT positionType = D3DDECLTYPE_FLOAT3;
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
    UINT blendWeightType = D3DDECLTYPE_FLOAT4;
    UINT blendWeightBytes = 0;
    UINT blendIndicesStream = 0;
    UINT blendIndicesOffset = 0;
    UINT blendIndicesType = D3DDECLTYPE_UBYTE4;
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
    // vs_1_x: shader does not need to DCL inputs because the v# registers have
    // fixed FFP semantics. We still default-map v0=POSITION/v3=NORMAL/v5=DIFFUSE
    // /v6=SPECULAR/v7..14=TEXCOORD0..7 so that a shader which reads those
    // registers without a DCL still binds the right stream — but we must NOT
    // require streams for inputs the shader never reads. This bitmask tracks
    // which v# registers actually appear as a source operand in the parsed
    // instructions; the SWVP-programmable validator gates require*Read() on it.
    std::uint32_t usedInputMask = 0;
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

static UINT processTexDeclBytes(UINT type, bool /*destination*/) {
    switch (type) {
        case D3DDECLTYPE_FLOAT1:
            return 4u;
        case D3DDECLTYPE_FLOAT2:
            return 8u;
        case D3DDECLTYPE_FLOAT3:
            return 12u;
        case D3DDECLTYPE_FLOAT4:
            return 16u;
        case D3DDECLTYPE_D3DCOLOR:
            return 4u;
        case D3DDECLTYPE_UBYTE4:
        case D3DDECLTYPE_SHORT2:
        case D3DDECLTYPE_UBYTE4N:
        case D3DDECLTYPE_UDEC3:
        case D3DDECLTYPE_DEC3N:
        case D3DDECLTYPE_SHORT2N:
        case D3DDECLTYPE_USHORT2N:
        case D3DDECLTYPE_FLOAT16_2:
            return 4u;
        case D3DDECLTYPE_SHORT4:
        case D3DDECLTYPE_SHORT4N:
        case D3DDECLTYPE_USHORT4N:
        case D3DDECLTYPE_FLOAT16_4:
            return 8u;
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
        case D3DDECLTYPE_SHORT2:
            return allowTwoComponent ? 4u : 0u;
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

static UINT processGenericDeclBytes(UINT type) {
    if (type == D3DDECLTYPE_D3DCOLOR) return 4u;
    return processFloatVectorDeclBytes(type, true);
}

static bool describeProcessFvf(DWORD fvf, FvfProcessLayout& layout) {
    layout = {};
    switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZ:
            layout.positionOffset = 0u;
            layout.positionType = D3DDECLTYPE_FLOAT3;
            layout.positionBytes = 12u;
            break;
        case D3DFVF_XYZB1:
        case D3DFVF_XYZB2:
        case D3DFVF_XYZB3:
        case D3DFVF_XYZB4:
        case D3DFVF_XYZB5:
            layout.positionOffset = 0u;
            layout.positionType = D3DDECLTYPE_FLOAT3;
            layout.positionBytes = 12u;
            break;
        case D3DFVF_XYZRHW:
        case D3DFVF_XYZW:
            layout.positionOffset = 0u;
            layout.positionType = D3DDECLTYPE_FLOAT4;
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
                layout.blendWeightType =
                    weightCount == 1u ? D3DDECLTYPE_FLOAT1 :
                    weightCount == 2u ? D3DDECLTYPE_FLOAT2 :
                    weightCount == 3u ? D3DDECLTYPE_FLOAT3 :
                                        D3DDECLTYPE_FLOAT4;
                layout.blendWeightBytes = weightCount * sizeof(float);
                offset += layout.blendWeightBytes;
            }
            layout.blendIndices = true;
            layout.blendIndicesOffset = offset;
            layout.blendIndicesType =
                lastBetaD3dcolor ? D3DDECLTYPE_D3DCOLOR : D3DDECLTYPE_UBYTE4;
            layout.blendIndicesBytes = 4u;
            offset += 4u;
        } else {
            if (betaCount > 4u) return false;
            layout.blendWeight = true;
            layout.blendWeightOffset = offset;
            layout.blendWeightType =
                betaCount == 1u ? D3DDECLTYPE_FLOAT1 :
                betaCount == 2u ? D3DDECLTYPE_FLOAT2 :
                betaCount == 3u ? D3DDECLTYPE_FLOAT3 :
                                  D3DDECLTYPE_FLOAT4;
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
                layout.positionType = D3DDECLTYPE_FLOAT4;
                layout.positionBytes = 16u;
            } else {
                const UINT bytes = processFloatVectorDeclBytes(e.type, true);
                if (bytes == 0u) return false;
                layout.positionType = e.type;
                layout.positionBytes = bytes;
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
            const UINT blendBytes = processFloatVectorDeclBytes(e.type, true);
            if (blendBytes == 0u) return false;
            if (layout.blendWeight) return false;
            layout.blendWeight = true;
            layout.blendWeightStream = e.stream;
            layout.blendWeightOffset = e.offset;
            layout.blendWeightType = e.type;
            layout.blendWeightBytes = blendBytes;
        } else if (!destination && e.usage == D3DDECLUSAGE_BLENDINDICES &&
                   e.usageIndex == 0) {
            if ((e.type != D3DDECLTYPE_UBYTE4 &&
                 e.type != D3DDECLTYPE_D3DCOLOR) ||
                layout.blendIndices) return false;
            layout.blendIndices = true;
            layout.blendIndicesStream = e.stream;
            layout.blendIndicesOffset = e.offset;
            layout.blendIndicesType = e.type;
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
            const UINT genericBytes = processGenericDeclBytes(e.type);
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
                        : layout.positionBytes != 0u) &&
           layout.stride != 0u;
}

static UINT shaderRegType(DWORD token) {
    const UINT low = (token >> D3DSP_REGTYPE_SHIFT) & 0x7u;
    const UINT officialHigh = (token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2;
    if (officialHigh != 0u) {
        return low | officialHigh;
    }
    // The local PE ProcessVertices fixtures build a few SM3 tokens with the
    // secondary register-type bits packed at 8..9 instead of D3D's 11..12.
    // Accept that encoding for simple CPU execution while preserving the
    // official decode for normal bytecode.
    return low | (((token >> 8u) & 0x3u) << 3u);
}

static UINT shaderRegIndex(DWORD token) {
    UINT index = token & D3DSP_REGNUM_MASK;
    if ((token & D3DSP_REGTYPE_MASK2) == 0u &&
        ((token >> 8u) & 0x3u) != 0u) {
        index &= ~0x300u;
    }
    return index;
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
        if (opcode == D3DSIO_DCL) {
            const UINT usage = (operands[0] & D3DSP_DCL_USAGE_MASK) >> D3DSP_DCL_USAGE_SHIFT;
            const UINT usageIndex =
                (operands[0] & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT;
            const ProcessShaderReg reg{shaderRegType(operands[1]),
                                       shaderRegIndex(operands[1])};
            if (reg.type == D3DSPR_INPUT) {
                noteProcessShaderInput(io, usage, usageIndex, reg.index);
                if (reg.index < 32u) io.usedInputMask |= (1u << reg.index);
            } else if (reg.type == D3DSPR_OUTPUT || reg.type == D3DSPR_TEXCRDOUT) {
                noteProcessShaderOutput(io, usage, usageIndex, reg);
            } else if (reg.type == D3DSPR_RASTOUT || reg.type == D3DSPR_ATTROUT) {
                noteProcessShaderOutput(io, usage, usageIndex, reg);
            }
            continue;
        }
        // Non-DCL: track v# registers actually used as source operands. For
        // vs_1_x this is the only signal we have that DIFFUSE/SPECULAR/TEXCOORD
        // slots are read (DCL is not required there). Operand 0 is the
        // destination for the ALU/CTRL ops we care about; operands [1..count) are
        // sources.
        for (UINT i = 1; i < parsedOperands.count; ++i) {
            if (shaderRegType(operands[i]) != D3DSPR_INPUT) continue;
            const UINT regIdx = shaderRegIndex(operands[i]);
            if (regIdx < 32u) {
                io.usedInputMask |= (1u << regIdx);
            }
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

static uint16_t floatToHalf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16u) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23u) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;

    if (((bits >> 23u) & 0xffu) == 0xffu) {
        if (mantissa == 0u) return static_cast<uint16_t>(sign | 0x7c00u);
        return static_cast<uint16_t>(sign | 0x7e00u);
    }
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t halfMantissa = mantissa >> shift;
        if ((mantissa >> (shift - 1u)) & 1u) ++halfMantissa;
        return static_cast<uint16_t>(sign | halfMantissa);
    }

    uint32_t halfMantissa = mantissa >> 13u;
    if (mantissa & 0x1000u) {
        ++halfMantissa;
        if (halfMantissa == 0x400u) {
            halfMantissa = 0u;
            ++exponent;
            if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
        }
    }
    return static_cast<uint16_t>(sign |
                                 (static_cast<uint32_t>(exponent) << 10u) |
                                 halfMantissa);
}

static int16_t floatToSnorm16(float value) {
    if (value <= -1.0f) return static_cast<int16_t>(-32768);
    if (value >= 1.0f) return static_cast<int16_t>(32767);
    return static_cast<int16_t>(std::lround(value * 32767.0f));
}

static uint16_t floatToUnorm16(float value) {
    return static_cast<uint16_t>(std::lround(clamp01(value) * 65535.0f));
}

static int32_t floatToSnorm10Bits(float value) {
    if (value <= -1.0f) return 0x200;
    if (value >= 1.0f) return 0x1ff;
    return static_cast<int32_t>(std::lround(value * 511.0f)) & 0x3ff;
}

static bool encodeProcessDeclVector(const float in[4],
                                    UINT type,
                                    uint8_t* destination) {
    switch (type) {
    case D3DDECLTYPE_FLOAT1:
    case D3DDECLTYPE_FLOAT2:
    case D3DDECLTYPE_FLOAT3:
    case D3DDECLTYPE_FLOAT4: {
        const UINT bytes = processTexDeclBytes(type, true);
        std::memcpy(destination, in, bytes);
        return true;
    }
    case D3DDECLTYPE_D3DCOLOR: {
        const DWORD color = packD3DColor(in);
        std::memcpy(destination, &color, sizeof(color));
        return true;
    }
    case D3DDECLTYPE_SHORT2: {
        int16_t out[2] = {
            static_cast<int16_t>(std::clamp<long>(std::lround(in[0]), -32768, 32767)),
            static_cast<int16_t>(std::clamp<long>(std::lround(in[1]), -32768, 32767)),
        };
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_SHORT4: {
        int16_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) {
            out[c] = static_cast<int16_t>(
                std::clamp<long>(std::lround(in[c]), -32768, 32767));
        }
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_UBYTE4: {
        uint8_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) {
            out[c] = static_cast<uint8_t>(
                std::clamp<long>(std::lround(in[c]), 0, 255));
        }
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_SHORT2N: {
        int16_t out[2] = {floatToSnorm16(in[0]), floatToSnorm16(in[1])};
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_SHORT4N: {
        int16_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) out[c] = floatToSnorm16(in[c]);
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_USHORT2N: {
        uint16_t out[2] = {floatToUnorm16(in[0]), floatToUnorm16(in[1])};
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_USHORT4N: {
        uint16_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) out[c] = floatToUnorm16(in[c]);
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_UBYTE4N: {
        uint8_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) out[c] = floatColorByte(in[c]);
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_UDEC3: {
        const DWORD packed =
            (static_cast<DWORD>(std::clamp<long>(std::lround(in[0]), 0, 1023)) & 0x3ffu) |
            ((static_cast<DWORD>(std::clamp<long>(std::lround(in[1]), 0, 1023)) & 0x3ffu) << 10u) |
            ((static_cast<DWORD>(std::clamp<long>(std::lround(in[2]), 0, 1023)) & 0x3ffu) << 20u);
        std::memcpy(destination, &packed, sizeof(packed));
        return true;
    }
    case D3DDECLTYPE_DEC3N: {
        const DWORD packed =
            (static_cast<DWORD>(floatToSnorm10Bits(in[0])) & 0x3ffu) |
            ((static_cast<DWORD>(floatToSnorm10Bits(in[1])) & 0x3ffu) << 10u) |
            ((static_cast<DWORD>(floatToSnorm10Bits(in[2])) & 0x3ffu) << 20u);
        std::memcpy(destination, &packed, sizeof(packed));
        return true;
    }
    case D3DDECLTYPE_FLOAT16_2: {
        uint16_t out[2] = {floatToHalf(in[0]), floatToHalf(in[1])};
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_FLOAT16_4: {
        uint16_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) out[c] = floatToHalf(in[c]);
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    default:
        return false;
    }
}

static bool decodeProcessDeclVector(const uint8_t* source,
                                    UINT type,
                                    UINT bytes,
                                    std::array<float, 4>& out) {
    out = {0.0f, 0.0f, 0.0f, 1.0f};
    switch (type) {
    case D3DDECLTYPE_FLOAT1:
    case D3DDECLTYPE_FLOAT2:
    case D3DDECLTYPE_FLOAT3:
    case D3DDECLTYPE_FLOAT4: {
        if (bytes == 0u || bytes > sizeof(float) * 4u ||
            (bytes % sizeof(float)) != 0u) {
            return false;
        }
        const UINT components = bytes / sizeof(float);
        std::memcpy(out.data(), source,
                    std::min<UINT>(components, 4u) * sizeof(float));
        return true;
    }
    case D3DDECLTYPE_SHORT4: {
        int16_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = static_cast<float>(in[c]);
        return true;
    }
    case D3DDECLTYPE_SHORT2: {
        int16_t in[2]{};
        std::memcpy(in, source, sizeof(in));
        out[0] = static_cast<float>(in[0]);
        out[1] = static_cast<float>(in[1]);
        return true;
    }
    case D3DDECLTYPE_UBYTE4: {
        uint8_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = static_cast<float>(in[c]);
        return true;
    }
    case D3DDECLTYPE_SHORT2N: {
        int16_t in[2]{};
        std::memcpy(in, source, sizeof(in));
        out[0] = snorm16ToFloat(in[0]);
        out[1] = snorm16ToFloat(in[1]);
        return true;
    }
    case D3DDECLTYPE_SHORT4N: {
        int16_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = snorm16ToFloat(in[c]);
        return true;
    }
    case D3DDECLTYPE_USHORT2N: {
        uint16_t in[2]{};
        std::memcpy(in, source, sizeof(in));
        out[0] = unorm16ToFloat(in[0]);
        out[1] = unorm16ToFloat(in[1]);
        return true;
    }
    case D3DDECLTYPE_USHORT4N: {
        uint16_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = unorm16ToFloat(in[c]);
        return true;
    }
    case D3DDECLTYPE_UBYTE4N: {
        uint8_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) {
            out[c] = static_cast<float>(in[c]) / 255.0f;
        }
        return true;
    }
    case D3DDECLTYPE_DEC3N: {
        uint32_t packed = 0;
        std::memcpy(&packed, source, sizeof(packed));
        out[0] = snorm10ToFloat(packed);
        out[1] = snorm10ToFloat(packed >> 10u);
        out[2] = snorm10ToFloat(packed >> 20u);
        return true;
    }
    case D3DDECLTYPE_UDEC3: {
        uint32_t packed = 0;
        std::memcpy(&packed, source, sizeof(packed));
        out[0] = static_cast<float>(packed & 0x3ffu);
        out[1] = static_cast<float>((packed >> 10u) & 0x3ffu);
        out[2] = static_cast<float>((packed >> 20u) & 0x3ffu);
        return true;
    }
    case D3DDECLTYPE_FLOAT16_2: {
        uint16_t in[2]{};
        std::memcpy(in, source, sizeof(in));
        out[0] = halfToFloat(in[0]);
        out[1] = halfToFloat(in[1]);
        return true;
    }
    case D3DDECLTYPE_FLOAT16_4: {
        uint16_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = halfToFloat(in[c]);
        return true;
    }
    default:
        return false;
    }
}

struct SimpleVsRegisters {
    std::array<std::array<float, 4>, 32> temp{};
    std::array<std::array<float, 4>, 16> input{};
    std::array<std::array<float, 4>, 256> constant{};
    std::array<std::array<int32_t, 4>, 16> constantInt{};
    std::array<uint32_t, 16> constantBool{};
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
        case D3DSPR_CONSTBOOL:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.constantBool.size()), index)) {
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
    } else if (type == D3DSPR_CONSTBOOL) {
        const float value = regs.constantBool[index] != 0u ? 1.0f : 0.0f;
        out[0] = out[1] = out[2] = out[3] = value;
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

static bool simpleVsConstantMatrixBase(const SimpleVsRegisters& regs,
                                       UINT major,
                                       DWORD token,
                                       DWORD relAddrToken,
                                       UINT rowCount,
                                       UINT& base) {
    if (shaderRegType(token) != D3DSPR_CONST) return false;
    if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                             static_cast<UINT>(regs.constant.size()), base)) {
        return false;
    }
    return rowCount != 0u && base <= regs.constant.size() - rowCount;
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
            if (!shaderSkipComment(words, index, token)) {
                return SimpleVsExecResult::Fail;
            }
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
        const bool predicatedInstruction = ((token >> 28u) & 0x1u) != 0u;
        const bool predicateAllows =
            !predicatedInstruction || regs.predicate[0][0] != 0.0f;
        const bool flowInstruction =
            opcode == D3DSIO_IF || opcode == D3DSIO_IFC ||
            opcode == D3DSIO_ELSE || opcode == D3DSIO_ENDIF ||
            opcode == D3DSIO_REP || opcode == D3DSIO_ENDREP ||
            opcode == D3DSIO_LOOP || opcode == D3DSIO_ENDLOOP ||
            opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ ||
            opcode == D3DSIO_LABEL || opcode == D3DSIO_RET ||
            opcode == D3DSIO_BREAK || opcode == D3DSIO_BREAKC ||
            opcode == D3DSIO_BREAKP;
        if (predicatedInstruction && !flowInstruction) {
            if (!predicateAllows) {
                continue;
            }
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
            bool takeCall = predicateAllows;
            if (opcode == D3DSIO_CALLNZ) {
                if (operandCount < 2u) return SimpleVsExecResult::Fail;
                float condition[4]{};
                if (!simpleVsReadSource(regs, io.major, operands[1], condition,
                                        relAddrOperands[1])) {
                    return SimpleVsExecResult::Fail;
                }
                takeCall = takeCall && condition[0] != 0.0f;
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
            if (!predicateAllows) continue;
            return SimpleVsExecResult::Break;
        }
        if (opcode == D3DSIO_BREAKP) {
            if (!predicateAllows) continue;
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
            if (!predicateAllows) continue;
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
            const UINT mask = shaderWriteMask(operands[0]);
            for (UINT component = 0; component < 4u; ++component) {
                if (mask & (1u << component)) {
                    (*dst)[component] = static_cast<float>(std::lround(value[component]));
                }
            }
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
            if (!predicateAllows) {
                index = afterEnd;
                continue;
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
            if (!predicateAllows) {
                if (!simpleVsSkipControlBlock(words, index, false)) {
                    return SimpleVsExecResult::Fail;
                }
                continue;
            }
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
            if (shaderRegType(operands[0]) != D3DSPR_CONSTINT) {
                return SimpleVsExecResult::Fail;
            }
            const UINT indexConst = shaderRegIndex(operands[0]);
            if (indexConst >= regs.constantInt.size()) {
                return SimpleVsExecResult::Fail;
            }
            for (UINT i = 0; i < 4; ++i) {
                regs.constantInt[indexConst][i] = static_cast<int32_t>(operands[i + 1u]);
            }
            continue;
        }
        if (opcode == D3DSIO_DEFB) {
            if (operandCount < 2u) return SimpleVsExecResult::Fail;
            if (shaderRegType(operands[0]) != D3DSPR_CONSTBOOL) {
                return SimpleVsExecResult::Fail;
            }
            UINT indexConst = shaderRegIndex(operands[0]);
            if (simpleProcessShaderTokenHasRelAddr(operands[0])) {
                if (!simpleVsSourceIndex(regs, io.major, operands[0],
                                         relAddrOperands[0],
                                         static_cast<UINT>(regs.constantBool.size()),
                                         indexConst)) {
                    return SimpleVsExecResult::Fail;
                }
            } else if (indexConst >= regs.constantBool.size()) {
                return SimpleVsExecResult::Fail;
            }
            regs.constantBool[indexConst] = operands[1] != 0u ? 1u : 0u;
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
            case D3DSIO_CND:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] > 0.5f ? b[i] : c[i];
                break;
            case D3DSIO_CMP:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] >= 0.0f ? b[i] : c[i];
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
            case D3DSIO_DP2ADD: {
                const float value = a[0] * b[0] + a[1] * b[1] + c[0];
                out[0] = out[1] = out[2] = out[3] = value;
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
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 4u, base)) {
                    return SimpleVsExecResult::Fail;
                }
                for (UINT row = 0; row < 4; ++row) {
                    const auto& c = regs.constant[base + row];
                    out[row] = a[0] * c[0] + a[1] * c[1] +
                               a[2] * c[2] + a[3] * c[3];
                }
                break;
            }
            case D3DSIO_M4x3: {
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 3u, base)) {
                    return SimpleVsExecResult::Fail;
                }
                for (UINT row = 0; row < 3; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] +
                               a[2] * k[2] + a[3] * k[3];
                }
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_M3x4: {
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 4u, base)) {
                    return SimpleVsExecResult::Fail;
                }
                for (UINT row = 0; row < 4; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] + a[2] * k[2];
                }
                break;
            }
            case D3DSIO_M3x3: {
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 3u, base)) {
                    return SimpleVsExecResult::Fail;
                }
                for (UINT row = 0; row < 3; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] + a[2] * k[2];
                }
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_M3x2: {
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 2u, base)) {
                    return SimpleVsExecResult::Fail;
                }
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
static D9CTexture*   rawTex(IDirect3DBaseTexture9* p)       { return D3D9PeRawTexture(p); }

/* R-FORMAT-11 — RESZ MSAA depth-resolve trigger. RESZ is a *command*, not
 * storage: an app requests a multisample depth resolve into the bound INTZ
 * depth texture by writing this exact sentinel to D3DRS_POINTSIZE while the
 * multisampled depth surface is bound as a texture. Any other D3DRS_POINTSIZE
 * value keeps its ordinary point-size meaning. The value-level classification
 * is unit-pinned by core::isReszDepthResolveSentinel (core_constants.hpp); it
 * is duplicated here as a literal because this PE translation unit speaks the
 * C ABI (device_c.h), not the C++ core header. See
 * specs/d3d9/formats/{requirements,spec}.md. */
static constexpr DWORD kReszDepthResolveSentinel = 0x7FA05000u;

/* =========================================================================
 * D3D9DeviceImpl — IDirect3DDevice9Ex
 * ========================================================================= */

class D3D9DeviceImpl final : public IDirect3DDevice9Ex, public D3D9PeRecorderFlush {
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

    enum class VsConstSetterRangePhase : std::uint32_t {
        Call = 1,
        Flush = 2,
    };

    struct VsConstRangeChange {
        std::uint32_t changedRegs = 0;
        std::uint32_t changedSpanRegs = 0;
    };

    struct VsConstSetterRangeBucket {
        bool used = false;
        VsConstSetterRangePhase phase = VsConstSetterRangePhase::Call;
        std::uint64_t vsHash = 0;
        std::uint64_t psHash = 0;
        std::uint32_t start = 0;
        std::uint32_t count = 0;
        std::uint64_t events = 0;
        std::uint64_t rangeRegs = 0;
        std::uint64_t changedRegs = 0;
        std::uint64_t changedSpanRegs = 0;
        std::uint64_t fullRangeEvents = 0;
        std::uint64_t fullChangedEvents = 0;
    };

    struct VsConstSetterRangeOverflow {
        std::uint64_t events = 0;
        std::uint64_t rangeRegs = 0;
        std::uint64_t changedRegs = 0;
        std::uint64_t changedSpanRegs = 0;
        std::uint64_t fullRangeEvents = 0;
        std::uint64_t fullChangedEvents = 0;
    };

    struct VsConstSetterRangePerf {
        static constexpr std::size_t kBucketCount = 256;
        std::array<VsConstSetterRangeBucket, kBucketCount> buckets{};
        std::array<VsConstSetterRangeOverflow, 3> overflow{};
    };

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
    std::uint64_t              submittedIndexBufferWireValue_ = 0;
    bool                       submittedIndexBufferKnown_ = false;
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
    // R-BACK-2.52 (Inline Const Delta): reusable scratch for
    // foldPendingConstsIntoDrawPacket()'s trailing const-delta payload
    // bytes, sized to the sum of every section kind's register-file cap
    // (VS_F 256*16 + VS_I 16*16 + VS_B 16*4 + PS_F 224*16 + PS_I 16*16 +
    // PS_B 16*4 = 8320 bytes) so a fully-populated draw never overflows it.
    // A fixed member array (not a per-draw heap allocation) keeps the fold
    // path allocation-free on the hot path per the DOD conventions; it only
    // participates in memory traffic when DXMT9_PE_INLINE_CONST_DELTA=1.
    static constexpr std::size_t kMaxInlineConstDeltaPayloadBytes =
        static_cast<std::size_t>(D9C_DRAW_PACKET_MAX_CONST_VS_F) * 16u +
        static_cast<std::size_t>(D9C_DRAW_PACKET_MAX_CONST_VS_I) * 16u +
        static_cast<std::size_t>(D9C_DRAW_PACKET_MAX_CONST_VS_B) * 4u +
        static_cast<std::size_t>(D9C_DRAW_PACKET_MAX_CONST_PS_F) * 16u +
        static_cast<std::size_t>(D9C_DRAW_PACKET_MAX_CONST_PS_I) * 16u +
        static_cast<std::size_t>(D9C_DRAW_PACKET_MAX_CONST_PS_B) * 4u;
    std::array<std::uint8_t, kMaxInlineConstDeltaPayloadBytes>
        constDeltaPayloadScratch_{};
    VsConstSetterRangePerf vsConstSetterRangePerf_{};
    IDirect3DSurface9* rtSlots_[4]{};
    bool rtSlotExplicit_[4]{};
    IDirect3DSurface9* dsSurface_ = nullptr;

    // Reused across draws so populateBindingView() does not zero ~850 bytes
    // per call. Mutable because buildDrawPrimitivePacket() is const.
    mutable dxmt9::d3d9::pe::PeBindingView peBindingView_{};

    // Reused sparse-producer storage. The SparseStateV2Input spans point into
    // peSparseScratch_, so both must outlive the append that consumes them --
    // they are members for that reason as well as to avoid per-draw zeroing.
    mutable dxmt9::d3d9::pe::PeSparseScratch peSparseScratch_{};
    mutable dxmt9::d3d9::pe::SparseStateV2Input peSparseState_{};
    mutable D9CCommandChunkWireDrawHeaderV2 peSparseHeader_{};
    mutable dxmt9::d3d9::pe::PeDrawPayloads peSparsePayloads_{};
    bool dsSurfaceExplicit_ = false;
    dxmt9::d3d9::pe::CommandChunkV2Builder commandChunkV2_{};
    std::vector<std::byte> legacyV2RecordScratch_{};
    bool commandChunkNegotiated_ = false;
    std::uint64_t commandChunkCommits_ = 0;
    std::uint64_t commandChunkRecords_ = 0;
    std::uint64_t commandChunkBytes_ = 0;
    PeRecorderStats peRecorderStats_{};
    // DXMT9_PE_STATS_DECIMATION diagnostic accumulators (const_flush,
    // draw_packet, and V2 append scopes). The const_setter accumulator lives
    // next to its owner (touchConstShadow's function-local static) — see
    // d3d9_pe_stats_decimation.hpp and logPeStatsDecimation() below.
    // draw_packet's stats are mutable because buildDrawPrimitivePacket()
    // is a const method.
    PeDecimatedScopeStats peV2AppendDecimatedStats_{};
    // Phase split of appendCommandRecordDirect, sampled on the same calls the
    // parent scope samples. CAVEAT: each phase costs one clock pair, so on a
    // sampled call the parent `append_sampled_ms` is inflated by roughly four
    // pairs. Read the phases against each other, not against the parent, and
    // do not quote the parent's absolute figure from a run with these enabled
    // -- a 2026-07-29 GT2 run read 4,180 ns for the parent here against
    // 2,851 ns with the phases off. The record is built in the legacy wire format and
    // then re-parsed and re-encoded into V2, so "build" and "encode" are two
    // halves of a serialize -> parse -> re-serialize round trip; this tells us
    // what that round trip actually costs.
    // Per-record-type append census. Counting only -- no clock reads -- so it
    // can run alongside the decimated timers without adding to the bias that
    // 2026-07-29 showed dominates short scopes. Tells us which encode path in
    // appendLegacyCommandRecordAsV2 is worth opening.
    static constexpr std::size_t kPeAppendTypeBuckets = 8;
    std::uint64_t peAppendTypeCounts_[kPeAppendTypeBuckets]{};
    std::uint64_t peAppendTypeBytes_[kPeAppendTypeBuckets]{};
    static std::size_t peAppendTypeBucket(std::uint32_t type) {
        switch (type) {
            case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: return 0;
            case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: return 1;
            case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
            case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: return 2;
            case D9C_COMMAND_RECORD_APPLY_STATE: return 3;
            case D9C_COMMAND_RECORD_SET_VS_CONST_F:
            case D9C_COMMAND_RECORD_SET_VS_CONST_I:
            case D9C_COMMAND_RECORD_SET_VS_CONST_B: return 4;
            case D9C_COMMAND_RECORD_SET_PS_CONST_F:
            case D9C_COMMAND_RECORD_SET_PS_CONST_I:
            case D9C_COMMAND_RECORD_SET_PS_CONST_B: return 5;
            case D9C_COMMAND_RECORD_CLEAR: return 6;
            default: return 7;
        }
    }

    PeDecimatedScopeStats peAppendPhaseResize_{};
    PeDecimatedScopeStats peAppendPhaseWrite_{};
    PeDecimatedScopeStats peAppendPhaseEncode_{};
    PeDecimatedScopeStats peAppendPhaseFlush_{};
    PeDecimatedScopeStats peConstFlushDecimatedStats_{};
    mutable PeDecimatedScopeStats peDrawPacketDecimatedStats_{};
    std::uint64_t peStatsDecimationPresents_ = 0;
    std::uint64_t peRecorderStatsLastLoggedCommitCount_ = 0;
    std::int64_t peRecorderLastChunkReturnNs_ = 0;
    std::int64_t peRecorderCurrentChunkFirstAppendNs_ = 0;
    std::int64_t peRecorderLastAppendReturnNs_ = 0;
    std::int64_t peRecorderLastAppendCallEntryNs_ = 0;
    std::int64_t peRecorderLastAppendCallExitNs_ = 0;
    std::uint32_t peRecorderLastAppendRecordType_ = 0;
    bool peRecorderBetweenCallsActive_ = false;
    std::int64_t peRecorderBetweenCallsStartNs_ = 0;
    std::array<std::uint64_t, kPeInterAppendCallFamilyCount>
        peRecorderBetweenCallFamilySamples_{};
    std::array<std::uint64_t, kPeInterAppendCallNameCount>
        peRecorderBetweenCallNameSamples_{};
    std::array<std::uint64_t, kPeInterAppendCallNameCount>
        peRecorderBetweenCallNameCpuNsTotal_{};
    std::array<std::uint64_t, kPeInterAppendCallNameCount>
        peRecorderBetweenCallNameCpuNsMax_{};
    PeInterAppendCallFamily peRecorderBetweenLastCallFamily_ =
        PeInterAppendCallFamily::Unknown;
    PeInterAppendCallName peRecorderBetweenLastCallName_ =
        PeInterAppendCallName::Unknown;
    std::int64_t peRecorderBetweenLastCallExitNs_ = 0;
    std::array<std::uint64_t,
               kPeInterAppendCallFamilyCount *
                   kPeInterAppendCallFamilyCount>
        peRecorderBetweenCallTransitionSamples_{};
    std::array<std::uint64_t,
               kPeInterAppendCallFamilyCount *
                   kPeInterAppendCallFamilyCount>
        peRecorderBetweenCallTransitionNsTotal_{};
    std::array<std::uint64_t,
               kPeInterAppendCallFamilyCount *
                   kPeInterAppendCallFamilyCount>
        peRecorderBetweenCallTransitionNsMax_{};
    std::array<std::uint64_t,
               kPeInterAppendCallNameCount *
                   kPeInterAppendCallNameCount>
        peRecorderBetweenCallNameTransitionSamples_{};
    std::array<std::uint64_t,
               kPeInterAppendCallNameCount *
                   kPeInterAppendCallNameCount>
        peRecorderBetweenCallNameTransitionNsTotal_{};
    std::array<std::uint64_t,
               kPeInterAppendCallNameCount *
                   kPeInterAppendCallNameCount>
        peRecorderBetweenCallNameTransitionNsMax_{};
    std::unordered_map<
        PeInterAppendCallSiteLocalKey,
        PeInterAppendCallSiteStats,
        PeInterAppendCallSiteLocalKeyHash>
        peRecorderBetweenCallNameTransitionSites_{};
    std::unordered_map<
        PeInterAppendCallSiteKey,
        PeInterAppendCallSiteStats,
        PeInterAppendCallSiteKeyHash>
        peRecorderFocusBetweenCallNameTransitionSites_{};
    std::uint64_t peRecorderBetweenCallBodyCalls_ = 0;
    std::uint64_t peRecorderBetweenCallBodyCpuNsTotal_ = 0;
    std::uint64_t peRecorderBetweenCallBodyCpuNsMax_ = 0;
    std::atomic<std::uint64_t> pePresentCadenceOrdinal_{0};
    std::atomic<std::uint64_t> pePresentCadencePendingOrdinal_{0};
    std::atomic<std::uint64_t> pePresentCallMilestonePendingOrdinal_{0};
    std::atomic<std::uint32_t> pePresentCallCount_{0};
    std::atomic<std::uint32_t> pePresentCallMilestoneMask_{0};
    std::atomic<std::uint64_t> pePresentChunkPendingOrdinal_{0};
    std::atomic<std::uint64_t> pePresentRecordPendingOrdinal_{0};
    std::atomic<std::uint32_t> pePresentRecordMilestoneMask_{0};
    std::atomic<std::int64_t> pePresentCadenceReturnNs_{0};

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

    DWORD renderStateValue(D3DRENDERSTATETYPE state) const {
        uint32_t shadowValue = 0;
        if (peState_.renderStateShadow.get(static_cast<DWORD>(state), shadowValue)) {
            return shadowValue;
        }
        return dxmt9c_device_get_render_state(dev_, static_cast<uint32_t>(state));
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
        commandChunkV2_.reset();
    }

    void clearPeStateTracking() {
        peState_.clearServerShadowTables();
        peState_.clearPendingHotState();
        peConsts_.reset();
        clearPendingCommandChunk();
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

    dxmt9::d3d9::pe::PeStreamSources currentDrawStreamSources() const {
        dxmt9::d3d9::pe::PeStreamSources sources{};
        for (DWORD slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
            sources[slot].buffer = toWireHandle(rawVBuf(streamSrc_[slot]));
            sources[slot].offset = streamOff_[slot];
            sources[slot].stride = streamStr_[slot];
        }
        return sources;
    }

    std::uint64_t currentVertexShaderHash() const noexcept {
        return D3D9PeVertexShaderHash(vs_);
    }

    std::uint64_t currentPixelShaderHash() const noexcept {
        return D3D9PePixelShaderHash(ps_);
    }

    static const char* vsConstSetterRangePhaseName(
        VsConstSetterRangePhase phase) noexcept {
        switch (phase) {
        case VsConstSetterRangePhase::Call:
            return "call";
        case VsConstSetterRangePhase::Flush:
            return "flush";
        }
        return "unknown";
    }

    static std::size_t vsConstSetterRangePhaseIndex(
        VsConstSetterRangePhase phase) noexcept {
        return phase == VsConstSetterRangePhase::Flush ? 2u : 1u;
    }

    void recordVsConstSetterRange(VsConstSetterRangePhase phase,
                                  std::uint64_t vsHash,
                                  std::uint64_t psHash,
                                  std::uint32_t start,
                                  std::uint32_t count,
                                  std::uint32_t changedRegs,
                                  std::uint32_t changedSpanRegs) noexcept {
        if (!dxmt9PerfVsConstSetterRangeEnabled() || count == 0u) {
            return;
        }
        VsConstSetterRangeBucket* bucket = nullptr;
        for (auto& candidate : vsConstSetterRangePerf_.buckets) {
            if (candidate.used) {
                if (candidate.phase == phase &&
                    candidate.vsHash == vsHash &&
                    candidate.psHash == psHash &&
                    candidate.start == start &&
                    candidate.count == count) {
                    bucket = &candidate;
                    break;
                }
                continue;
            }
            candidate.used = true;
            candidate.phase = phase;
            candidate.vsHash = vsHash;
            candidate.psHash = psHash;
            candidate.start = start;
            candidate.count = count;
            bucket = &candidate;
            break;
        }

        const bool fullRange = start == 0u && count >= kVsConstFMax;
        const bool fullChanged = start == 0u && changedSpanRegs >= kVsConstFMax;
        if (!bucket) {
            auto& overflow = vsConstSetterRangePerf_.overflow[
                vsConstSetterRangePhaseIndex(phase)];
            ++overflow.events;
            overflow.rangeRegs += count;
            overflow.changedRegs += changedRegs;
            overflow.changedSpanRegs += changedSpanRegs;
            overflow.fullRangeEvents += fullRange ? 1u : 0u;
            overflow.fullChangedEvents += fullChanged ? 1u : 0u;
            return;
        }

        ++bucket->events;
        bucket->rangeRegs += count;
        bucket->changedRegs += changedRegs;
        bucket->changedSpanRegs += changedSpanRegs;
        bucket->fullRangeEvents += fullRange ? 1u : 0u;
        bucket->fullChangedEvents += fullChanged ? 1u : 0u;
    }

    void logVsConstSetterRangePerf(const char* event) {
        if (!dxmt9PerfVsConstSetterRangeEnabled()) {
            return;
        }
        for (const auto& bucket : vsConstSetterRangePerf_.buckets) {
            if (!bucket.used || bucket.events == 0u) {
                continue;
            }
            dxmt9PerfLogStderrAtomic(
                "[dxmt9-perf-vs-const-setter-range event=%s overflow=0 "
                "phase=%s vs_hash=0x%llx ps_hash=0x%llx "
                "start=%u count=%u events=%llu range_regs=%llu "
                "changed_regs=%llu changed_span_regs=%llu "
                "full_range_events=%llu full_changed_events=%llu]\n",
                event ? event : "unknown",
                vsConstSetterRangePhaseName(bucket.phase),
                static_cast<unsigned long long>(bucket.vsHash),
                static_cast<unsigned long long>(bucket.psHash),
                bucket.start, bucket.count,
                static_cast<unsigned long long>(bucket.events),
                static_cast<unsigned long long>(bucket.rangeRegs),
                static_cast<unsigned long long>(bucket.changedRegs),
                static_cast<unsigned long long>(bucket.changedSpanRegs),
                static_cast<unsigned long long>(bucket.fullRangeEvents),
                static_cast<unsigned long long>(bucket.fullChangedEvents));
        }
        for (std::size_t phaseIndex = 1u;
             phaseIndex < vsConstSetterRangePerf_.overflow.size();
             ++phaseIndex) {
            const auto& overflow = vsConstSetterRangePerf_.overflow[phaseIndex];
            if (overflow.events == 0u) {
                continue;
            }
            const auto phase = phaseIndex == 2u
                ? VsConstSetterRangePhase::Flush
                : VsConstSetterRangePhase::Call;
            dxmt9PerfLogStderrAtomic(
                "[dxmt9-perf-vs-const-setter-range event=%s overflow=1 "
                "phase=%s events=%llu range_regs=%llu changed_regs=%llu "
                "changed_span_regs=%llu full_range_events=%llu "
                "full_changed_events=%llu]\n",
                event ? event : "unknown",
                vsConstSetterRangePhaseName(phase),
                static_cast<unsigned long long>(overflow.events),
                static_cast<unsigned long long>(overflow.rangeRegs),
                static_cast<unsigned long long>(overflow.changedRegs),
                static_cast<unsigned long long>(overflow.changedSpanRegs),
                static_cast<unsigned long long>(overflow.fullRangeEvents),
                static_cast<unsigned long long>(overflow.fullChangedEvents));
        }
        std::fflush(stderr);
        vsConstSetterRangePerf_ = VsConstSetterRangePerf{};
    }

    // Fill the producer's binding view from the COM members. Reuses a member
    // rather than a local so the ~850-byte view is not zero-initialized on
    // every draw.
    //
    // `needAllSlots` mirrors the producer's own snapshot gate. It matters for
    // cost, not correctness: rawTex() is not a cast -- D3D9PeRawTexture makes a
    // virtual GetType() call per bound texture -- so translating all 20 texture
    // and 16 stream slots unconditionally would do roughly 45 raw* calls per
    // draw where the delta path historically did about 9. At GT2's ~1,700
    // packet builds per present that is tens of thousands of extra virtual
    // calls, on the same hot path aefde46f had just cleaned up. So the delta
    // fill translates exactly the pending slots the old inline code did, and
    // only the snapshot path drains everything.
    //
    // Fills the FULL PeWireObjectRef -- identity as well as object -- via the
    // D3D9PeWire* accessors, which return each wrapper's cached ref.
    //
    // Identity is not optional: CommandChunkV2Builder::appendHandle rejects a
    // ref whose identity is zero (PeWireObjectRef::valid checks kind,
    // generation and objectId) and fails the whole record through
    // failActiveRecord() with NO log line. An earlier revision filled only
    // `object`, on the reasoning that nothing read identity yet; the moment
    // APPLY_STATE started going through appendApplyStateV2 that turned into
    // "IDirect3DDevice9::Clear failed: Invalid call" with nothing explaining
    // why, and the harness still reported status=pass because it does not gate
    // on the rendered image.
    //
    // These accessors are also cheaper than the raw* pair they replaced for
    // surfaces and buffers: wireObject() is a member read where rawSurf() went
    // through a cast and rawTex()/D3D9PeRawTexture made a virtual GetType()
    // call. D3D9PeWireTexture still switches on GetType(), so the texture loop
    // keeps its pending-mask guard.
    void populateBindingView(dxmt9::d3d9::pe::PeBindingView& view,
                             bool needAllSlots) const {
        for (std::uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
            if (needAllSlots ||
                (peState_.pendingTextureMask & (1u << stage)) != 0) {
                view.textures[stage] = D3D9PeWireTexture(textures_[stage]);
            }
        }
        for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
            if (!needAllSlots &&
                (peState_.pendingStreamMask & (1u << slot)) == 0) {
                continue;
            }
            view.streams[slot].buffer = D3D9PeWireVertexBuffer(streamSrc_[slot]);
            view.streams[slot].offset = streamOff_[slot];
            view.streams[slot].stride = streamStr_[slot];
        }
        // These were read unconditionally by the old inline code on both paths.
        view.vs = D3D9PeWireVertexShader(vs_);
        view.ps = D3D9PeWirePixelShader(ps_);
        view.vdecl = D3D9PeWireVertexDecl(vdecl_);
        view.depthStencil = D3D9PeWireSurface(dsSurface_);
        for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
            view.renderTargets[slot] = D3D9PeWireSurface(rtSlots_[slot]);
        }
        view.rtExplicitMask = currentRtExplicitMask();
        view.fvf = fvf_;
        // indexBuffer is filled by the indexed-draw sites, which are the only
        // consumers; buildSparseStateV2 emits the index section only for the
        // indexed record types.
    }

    // Forwards to the rehosted producer in d3d9_pe_producer.cpp. The signature
    // is deliberately unchanged so all six call sites -- which differ only in
    // which packet field they pass, whether they thread a live
    // forceFullSnapshot, and what they do on failure -- keep working verbatim.
    // Rewriting them individually would have been six hand edits in a
    // translation unit no meson test can compile; this is one function.
    //
    // The DXMT9_PE_STATS_DECIMATION `draw_packet` scope lives HERE, not in the
    // producer, because it historically covered the COM-to-wire binding
    // translation as well as the packet fill. Timing only the producer would
    // have made the figure read lower while the work moved outside it, and that
    // figure is what docs/perfomance/ cites for PE recording cost.
    // Sparse-state counterpart to buildDrawPrimitivePacket. Fills the reused
    // peSparseState_ / peSparseHeader_ from the shadows and the binding view --
    // no fat packet in between. The decimated draw_packet scope lives here for
    // the same reason it does on the fat-packet forwarder: it has to cover the
    // COM-to-wire binding translation, not just the section fill.
    // Not const: buildSparseStateV2 takes the const shadow by non-const
    // reference because the inline-delta path drains dirty ranges from it.
    // Claiming const here and casting it away would hide that.
    bool buildSparseStateForRecord(
        std::uint32_t recordType,
        dxmt9::d3d9::pe::PeDrawParams params,
        bool forceFullSnapshot = false) {
        DxmtPeDecimatedScopeGuard decimatedScope;
        const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
        if (decimationN != 0 &&
            PeDecimatedScopeTimer::shouldSample(
                peDrawPacketDecimatedStats_, decimationN)) {
            decimatedScope.stats = &peDrawPacketDecimatedStats_;
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
        params.recordType = recordType;
        const bool needAllSlots =
            forceFullSnapshot || dxmt9::d3d9::pe::dxmt9PeFullSnapshotEnabled();
        populateBindingView(peBindingView_, needAllSlots);
        return dxmt9::d3d9::pe::buildSparseStateV2(
            peState_, peConsts_, peBindingView_, peSparsePayloads_, params,
            forceFullSnapshot, peSparseScratch_, peSparseHeader_,
            peSparseState_);
    }

    bool buildDrawPrimitivePacket(D3DPRIMITIVETYPE type,
                                  UINT startVertex,
                                  UINT count,
                                  D9CDrawPrimitivePacket& packet,
                                  bool forceFullSnapshot = false) const {
        DxmtPeDecimatedScopeGuard decimatedScope;
        const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
        if (decimationN != 0 &&
            PeDecimatedScopeTimer::shouldSample(
                peDrawPacketDecimatedStats_, decimationN)) {
            decimatedScope.stats = &peDrawPacketDecimatedStats_;
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
        populateBindingView(peBindingView_, needAllSlots);
        return dxmt9::d3d9::pe::buildDrawPacketFromViews(
            peState_, peBindingView_, static_cast<std::uint32_t>(type),
            startVertex, count, forceFullSnapshot, packet);
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

    struct SoftwareFfpDrawData {
        std::vector<std::uint8_t> vertices;
        DWORD fvf = 0;
        UINT stride = 0;
        UINT primitiveCount = 0;
        D3DPRIMITIVETYPE primitiveType = D3DPT_POINTLIST;
        bool bypassVertexShader = false;
    };

    enum : std::uint32_t {
        kSwvpClipOutsideEye = 1u << 0,
        kSwvpClipOutsideLeft = 1u << 1,
        kSwvpClipOutsideRight = 1u << 2,
        kSwvpClipOutsideTop = 1u << 3,
        kSwvpClipOutsideBottom = 1u << 4,
        kSwvpClipOutsideNear = 1u << 5,
        kSwvpClipOutsideFar = 1u << 6,
        kSwvpClipOutsideUserPlane0 = 1u << 7,
        kSwvpClipOutsideAll = kSwvpClipOutsideEye |
                              kSwvpClipOutsideLeft |
                              kSwvpClipOutsideRight |
                              kSwvpClipOutsideTop |
                              kSwvpClipOutsideBottom |
                              kSwvpClipOutsideNear |
                              kSwvpClipOutsideFar |
                              (0x3fu << 7),
    };

    std::uint32_t transformedSwvpVertexClipFlags(
            const std::vector<std::uint8_t>& vertices,
            UINT stride,
            UINT index) const {
        if (stride < 16u) return 0u;
        const std::uint64_t offset = static_cast<std::uint64_t>(index) * stride;
        if (offset > vertices.size() ||
            16u > vertices.size() - static_cast<size_t>(offset)) {
            return kSwvpClipOutsideAll;
        }
        float x = 0.0f, y = 0.0f, z = 0.0f, rhw = 0.0f;
        std::memcpy(&x, vertices.data() + static_cast<size_t>(offset),
                    sizeof(x));
        std::memcpy(&y, vertices.data() + static_cast<size_t>(offset) + 4u,
                    sizeof(y));
        std::memcpy(&z, vertices.data() + static_cast<size_t>(offset) + 8u,
                    sizeof(z));
        std::memcpy(&rhw,
                    vertices.data() + static_cast<size_t>(offset) + 12u,
                    sizeof(rhw));
        if (!std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(z) || !std::isfinite(rhw)) {
            return kSwvpClipOutsideAll;
        }

        std::uint32_t flags = 0u;
        if (rhw <= 0.0f) flags |= kSwvpClipOutsideEye;

        const auto& vp = peState_.viewportShadow;
        const float left = static_cast<float>(vp.x);
        const float right = left + static_cast<float>(vp.width);
        const float top = static_cast<float>(vp.y);
        const float bottom = top + static_cast<float>(vp.height);
        if (vp.width != 0u) {
            if (x < left) flags |= kSwvpClipOutsideLeft;
            if (x > right) flags |= kSwvpClipOutsideRight;
        }
        if (vp.height != 0u) {
            if (y < top) flags |= kSwvpClipOutsideTop;
            if (y > bottom) flags |= kSwvpClipOutsideBottom;
        }
        const float nearZ = std::min(vp.minZ, vp.maxZ);
        const float farZ = std::max(vp.minZ, vp.maxZ);
        if (z < nearZ) flags |= kSwvpClipOutsideNear;
        if (z > farZ) flags |= kSwvpClipOutsideFar;
        const DWORD userClipMask =
            renderStateValue(D3DRS_CLIPPLANEENABLE) & 0x3fu;
        const float zScale = vp.maxZ - vp.minZ;
        if (userClipMask != 0u && rhw != 0.0f && vp.width != 0u &&
            vp.height != 0u && zScale != 0.0f) {
            const float scaleX = static_cast<float>(vp.width) * 0.5f;
            const float scaleY = static_cast<float>(vp.height) * 0.5f;
            const float offsetX = static_cast<float>(vp.x) + scaleX;
            const float offsetY = static_cast<float>(vp.y) + scaleY;
            const float w = 1.0f / rhw;
            const float clip[4] = {
                ((x - offsetX) / scaleX) * w,
                (-(y - offsetY) / scaleY) * w,
                ((z - vp.minZ) / zScale) * w,
                w,
            };
            for (UINT i = 0; i < 6u; ++i) {
                if ((userClipMask & (1u << i)) == 0u) continue;
                const float* plane = &peState_.clipPlaneShadow[i * 4u];
                const float distance = plane[0] * clip[0] +
                                       plane[1] * clip[1] +
                                       plane[2] * clip[2] +
                                       plane[3] * clip[3];
                if (!std::isfinite(distance) || distance < 0.0f) {
                    flags |= kSwvpClipOutsideUserPlane0 << i;
                }
            }
        }
        return flags;
    }

	    static HRESULT appendTransformedSwvpVertex(
	            const std::vector<std::uint8_t>& source,
	            UINT stride,
	            UINT index,
	            std::vector<std::uint8_t>& out) {
        if (stride == 0u) return D3DERR_INVALIDCALL;
        const std::uint64_t offset =
            static_cast<std::uint64_t>(index) * stride;
        if (offset > source.size() ||
            stride > source.size() - static_cast<size_t>(offset)) {
            return D3DERR_INVALIDCALL;
        }
        out.insert(out.end(),
                   source.begin() + static_cast<size_t>(offset),
                   source.begin() + static_cast<size_t>(offset) + stride);
	        return S_OK;
	    }

	    struct SwvpClippedVertex {
	        std::vector<std::uint8_t> bytes;
	    };

	    static HRESULT copyTransformedSwvpVertex(
	            const std::vector<std::uint8_t>& source,
	            UINT stride,
	            UINT index,
	            SwvpClippedVertex& out) {
	        if (stride == 0u) return D3DERR_INVALIDCALL;
	        const std::uint64_t offset =
	            static_cast<std::uint64_t>(index) * stride;
	        if (offset > source.size() ||
	            stride > source.size() - static_cast<size_t>(offset)) {
	            return D3DERR_INVALIDCALL;
	        }
	        out.bytes.assign(source.begin() + static_cast<size_t>(offset),
	                         source.begin() + static_cast<size_t>(offset) + stride);
	        return S_OK;
	    }

	    static float swvpReadFloat(const std::vector<std::uint8_t>& bytes,
	                               UINT offset) {
	        float value = 0.0f;
	        if (offset + sizeof(value) <= bytes.size()) {
	            std::memcpy(&value, bytes.data() + offset, sizeof(value));
	        }
	        return value;
	    }

	    static void swvpWriteFloat(std::vector<std::uint8_t>& bytes,
	                               UINT offset,
	                               float value) {
	        if (offset + sizeof(value) <= bytes.size()) {
	            std::memcpy(bytes.data() + offset, &value, sizeof(value));
	        }
	    }

	    static DWORD swvpReadDword(const std::vector<std::uint8_t>& bytes,
	                               UINT offset) {
	        DWORD value = 0;
	        if (offset + sizeof(value) <= bytes.size()) {
	            std::memcpy(&value, bytes.data() + offset, sizeof(value));
	        }
	        return value;
	    }

	    static void swvpWriteDword(std::vector<std::uint8_t>& bytes,
	                               UINT offset,
	                               DWORD value) {
	        if (offset + sizeof(value) <= bytes.size()) {
	            std::memcpy(bytes.data() + offset, &value, sizeof(value));
	        }
	    }

	    static DWORD interpolateD3dColor(DWORD a, DWORD b, float t) {
	        auto channel = [&](UINT shift) -> DWORD {
	            const float av = static_cast<float>((a >> shift) & 0xffu);
	            const float bv = static_cast<float>((b >> shift) & 0xffu);
	            const float v = av + (bv - av) * t;
	            return static_cast<DWORD>(
	                std::clamp<int>(static_cast<int>(std::lround(v)), 0, 255));
	        };
	        return (channel(24u) << 24u) |
	               (channel(16u) << 16u) |
	               (channel(8u) << 8u) |
	               channel(0u);
	    }

	    static SwvpClippedVertex interpolateTransformedSwvpVertex(
	            const SwvpClippedVertex& a,
	            const SwvpClippedVertex& b,
	            DWORD fvf,
	            UINT stride,
	            float t) {
	        SwvpClippedVertex out{a.bytes};
	        if (out.bytes.size() != stride) out.bytes.resize(stride);
	        t = std::clamp(t, 0.0f, 1.0f);

	        FvfProcessLayout layout{};
	        if (!describeProcessFvf(fvf, layout) || layout.stride > stride ||
	            layout.positionBytes < 16u) {
	            for (UINT offset = 0; offset + sizeof(float) <= stride; offset += 4u) {
	                const float av = swvpReadFloat(a.bytes, offset);
	                const float bv = swvpReadFloat(b.bytes, offset);
	                swvpWriteFloat(out.bytes, offset, av + (bv - av) * t);
	            }
	            return out;
	        }

	        auto lerpFloat = [&](UINT offset) {
	            const float av = swvpReadFloat(a.bytes, offset);
	            const float bv = swvpReadFloat(b.bytes, offset);
	            swvpWriteFloat(out.bytes, offset, av + (bv - av) * t);
	        };
	        for (UINT offset = layout.positionOffset;
	             offset < layout.positionOffset + layout.positionBytes;
	             offset += 4u) {
	            lerpFloat(offset);
	        }
	        if (layout.psize) lerpFloat(layout.psizeOffset);
	        if (layout.diffuse) {
	            swvpWriteDword(out.bytes, layout.diffuseOffset,
	                interpolateD3dColor(swvpReadDword(a.bytes, layout.diffuseOffset),
	                                    swvpReadDword(b.bytes, layout.diffuseOffset),
	                                    t));
	        }
	        if (layout.specular) {
	            swvpWriteDword(out.bytes, layout.specularOffset,
	                interpolateD3dColor(swvpReadDword(a.bytes, layout.specularOffset),
	                                    swvpReadDword(b.bytes, layout.specularOffset),
	                                    t));
	        }
	        for (UINT tex = 0; tex < layout.texCount; ++tex) {
	            for (UINT offset = layout.texOffset[tex];
	                 offset < layout.texOffset[tex] + layout.texBytes[tex];
	                 offset += 4u) {
	                lerpFloat(offset);
	            }
	        }
	        return out;
	    }

	    std::vector<std::uint32_t> transformedSwvpActiveClipPlanes() const {
	        std::vector<std::uint32_t> planes;
	        planes.reserve(13u);
	        planes.push_back(kSwvpClipOutsideEye);
	        const auto& vp = peState_.viewportShadow;
	        if (vp.width != 0u) {
	            planes.push_back(kSwvpClipOutsideLeft);
	            planes.push_back(kSwvpClipOutsideRight);
	        }
	        if (vp.height != 0u) {
	            planes.push_back(kSwvpClipOutsideTop);
	            planes.push_back(kSwvpClipOutsideBottom);
	        }
	        planes.push_back(kSwvpClipOutsideNear);
	        planes.push_back(kSwvpClipOutsideFar);
	        const DWORD userClipMask =
	            renderStateValue(D3DRS_CLIPPLANEENABLE) & 0x3fu;
	        for (UINT i = 0; i < 6u; ++i) {
	            if (userClipMask & (1u << i)) {
	                planes.push_back(kSwvpClipOutsideUserPlane0 << i);
	            }
	        }
	        return planes;
	    }

	    float transformedSwvpVertexPlaneDistance(
	            const SwvpClippedVertex& vertex,
	            std::uint32_t planeFlag) const {
	        const float x = swvpReadFloat(vertex.bytes, 0u);
	        const float y = swvpReadFloat(vertex.bytes, 4u);
	        const float z = swvpReadFloat(vertex.bytes, 8u);
	        const float rhw = swvpReadFloat(vertex.bytes, 12u);
	        if (!std::isfinite(x) || !std::isfinite(y) ||
	            !std::isfinite(z) || !std::isfinite(rhw)) {
	            return -1.0f;
	        }
	        const auto& vp = peState_.viewportShadow;
	        const float left = static_cast<float>(vp.x);
	        const float right = left + static_cast<float>(vp.width);
	        const float top = static_cast<float>(vp.y);
	        const float bottom = top + static_cast<float>(vp.height);
	        switch (planeFlag) {
	            case kSwvpClipOutsideEye:
	                return rhw - 1.0e-6f;
	            case kSwvpClipOutsideLeft:
	                return x - left;
	            case kSwvpClipOutsideRight:
	                return right - x;
	            case kSwvpClipOutsideTop:
	                return y - top;
	            case kSwvpClipOutsideBottom:
	                return bottom - y;
	            case kSwvpClipOutsideNear:
	                return z - std::min(vp.minZ, vp.maxZ);
	            case kSwvpClipOutsideFar:
	                return std::max(vp.minZ, vp.maxZ) - z;
	            default:
	                break;
	        }
	        if (planeFlag >= kSwvpClipOutsideUserPlane0 &&
	            planeFlag < (kSwvpClipOutsideUserPlane0 << 6u) && rhw != 0.0f &&
	            vp.width != 0u && vp.height != 0u &&
	            vp.maxZ != vp.minZ) {
	            UINT userPlane = 0u;
	            for (; userPlane < 6u; ++userPlane) {
	                if (planeFlag == (kSwvpClipOutsideUserPlane0 << userPlane)) break;
	            }
	            if (userPlane < 6u) {
	                const float scaleX = static_cast<float>(vp.width) * 0.5f;
	                const float scaleY = static_cast<float>(vp.height) * 0.5f;
	                const float offsetX = static_cast<float>(vp.x) + scaleX;
	                const float offsetY = static_cast<float>(vp.y) + scaleY;
	                const float w = 1.0f / rhw;
	                const float clip[4] = {
	                    ((x - offsetX) / scaleX) * w,
	                    (-(y - offsetY) / scaleY) * w,
	                    ((z - vp.minZ) / (vp.maxZ - vp.minZ)) * w,
	                    w,
	                };
	                const float* plane = &peState_.clipPlaneShadow[userPlane * 4u];
	                const float distance = plane[0] * clip[0] +
	                                       plane[1] * clip[1] +
	                                       plane[2] * clip[2] +
	                                       plane[3] * clip[3];
	                return std::isfinite(distance) ? distance : -1.0f;
	            }
	        }
	        return 1.0f;
	    }

	    HRESULT clipTransformedSwvpTriangle(
	            DWORD fvf,
	            UINT stride,
	            const SwvpClippedVertex& a,
	            const SwvpClippedVertex& b,
	            const SwvpClippedVertex& c,
	            std::vector<std::uint8_t>& out,
	            UINT& primitiveCount) const {
	        if (stride < 16u || a.bytes.size() != stride ||
	            b.bytes.size() != stride || c.bytes.size() != stride) {
	            return D3DERR_INVALIDCALL;
	        }
	        std::vector<SwvpClippedVertex> polygon{a, b, c};
	        std::vector<SwvpClippedVertex> clipped;
	        const auto planes = transformedSwvpActiveClipPlanes();
	        for (std::uint32_t plane : planes) {
	            if (polygon.empty()) break;
	            clipped.clear();
	            clipped.reserve(polygon.size() + 1u);
	            SwvpClippedVertex previous = polygon.back();
	            float previousDistance =
	                transformedSwvpVertexPlaneDistance(previous, plane);
	            bool previousInside = previousDistance >= -1.0e-5f;
	            for (const auto& current : polygon) {
	                const float currentDistance =
	                    transformedSwvpVertexPlaneDistance(current, plane);
	                const bool currentInside = currentDistance >= -1.0e-5f;
	                if (currentInside != previousInside) {
	                    const float denom = previousDistance - currentDistance;
	                    const float t = std::fabs(denom) > 1.0e-12f
	                        ? previousDistance / denom : 0.0f;
	                    clipped.push_back(interpolateTransformedSwvpVertex(
	                        previous, current, fvf, stride, t));
	                }
	                if (currentInside) clipped.push_back(current);
	                previous = current;
	                previousDistance = currentDistance;
	                previousInside = currentInside;
	            }
	            polygon = clipped;
	        }
	        if (polygon.size() < 3u) return S_OK;
	        for (size_t i = 1; i + 1u < polygon.size(); ++i) {
	            out.insert(out.end(), polygon[0].bytes.begin(), polygon[0].bytes.end());
	            out.insert(out.end(), polygon[i].bytes.begin(), polygon[i].bytes.end());
	            out.insert(out.end(), polygon[i + 1u].bytes.begin(),
	                       polygon[i + 1u].bytes.end());
	            ++primitiveCount;
	        }
	        return S_OK;
	    }

	    HRESULT clipTransformedSwvpLine(
	            DWORD fvf,
	            UINT stride,
	            const SwvpClippedVertex& a,
	            const SwvpClippedVertex& b,
	            std::vector<std::uint8_t>& out,
	            UINT& primitiveCount) const {
	        if (stride < 16u || a.bytes.size() != stride ||
	            b.bytes.size() != stride) {
	            return D3DERR_INVALIDCALL;
	        }
	        SwvpClippedVertex start = a;
	        SwvpClippedVertex end = b;
	        const auto planes = transformedSwvpActiveClipPlanes();
	        for (std::uint32_t plane : planes) {
	            const float startDistance =
	                transformedSwvpVertexPlaneDistance(start, plane);
	            const float endDistance =
	                transformedSwvpVertexPlaneDistance(end, plane);
	            const bool startInside = startDistance >= -1.0e-5f;
	            const bool endInside = endDistance >= -1.0e-5f;
	            if (!startInside && !endInside) return S_OK;
	            if (startInside && endInside) continue;

	            const float denom = startDistance - endDistance;
	            const float t = std::fabs(denom) > 1.0e-12f
	                ? startDistance / denom : 0.0f;
	            SwvpClippedVertex intersection =
	                interpolateTransformedSwvpVertex(start, end, fvf, stride, t);
	            if (!startInside) {
	                start = std::move(intersection);
	            } else {
	                end = std::move(intersection);
	            }
	        }
	        out.insert(out.end(), start.bytes.begin(), start.bytes.end());
	        out.insert(out.end(), end.bytes.begin(), end.bytes.end());
	        ++primitiveCount;
	        return S_OK;
	    }

    static DWORD processFfpDeclarationOutputFvf(
            const FvfProcessLayout& srcLayout,
            bool lighting,
            bool specularLighting,
            bool allowBlendAttributes) {
        if (srcLayout.positionBytes == 0u) {
            return 0u;
        }
        if (!allowBlendAttributes &&
            (srcLayout.blendWeight || srcLayout.blendIndices)) {
            return 0u;
        }
        DWORD outputFvf = D3DFVF_XYZRHW;
        if (srcLayout.psize) outputFvf |= D3DFVF_PSIZE;
        if (lighting || srcLayout.diffuse) outputFvf |= D3DFVF_DIFFUSE;
        if (specularLighting || srcLayout.specular) outputFvf |= D3DFVF_SPECULAR;
        if (srcLayout.texCount > 8u) return 0u;
        for (UINT i = 0; i < srcLayout.texCount; ++i) {
            switch (srcLayout.texBytes[i]) {
                case 4u:
                    outputFvf |= D3DFVF_TEXCOORDSIZE1(i);
                    break;
                case 8u:
                    outputFvf |= D3DFVF_TEXCOORDSIZE2(i);
                    break;
                case 12u:
                    outputFvf |= D3DFVF_TEXCOORDSIZE3(i);
                    break;
                case 16u:
                    outputFvf |= D3DFVF_TEXCOORDSIZE4(i);
                    break;
                default:
                    return 0u;
            }
        }
        outputFvf |= srcLayout.texCount << D3DFVF_TEXCOUNT_SHIFT;
        return outputFvf;
    }

    HRESULT describeSoftwareFfpDrawTarget(DWORD& outputFvf,
                                          FvfProcessLayout& srcLayout,
                                          FvfProcessLayout& dstLayout) {
        outputFvf = 0;
        srcLayout = {};
        dstLayout = {};
        if (!softwareVertexProcessing_ || vs_ != nullptr) {
            return S_FALSE;
        }
        const bool lighting = renderStateValue(D3DRS_LIGHTING) != FALSE;
        const bool specularLighting =
            lighting && renderStateValue(D3DRS_SPECULARENABLE) != FALSE;
        if (fvf_ != 0) {
            const DWORD positionMask = fvf_ & D3DFVF_POSITION_MASK;
            if ((positionMask != D3DFVF_XYZ &&
                 !processFvfXyzbPosition(positionMask)) ||
                !describeProcessFvf(fvf_, srcLayout)) {
                return S_FALSE;
            }
            outputFvf = processFfpDeclarationOutputFvf(
                srcLayout, lighting, specularLighting, true);
            if (outputFvf == 0u) return S_FALSE;
        } else if (vdecl_) {
            if (!describeProcessDeclaration(vdecl_, srcLayout, false)) {
                return S_FALSE;
            }
            outputFvf = processFfpDeclarationOutputFvf(
                srcLayout, lighting, specularLighting, true);
            if (outputFvf == 0u) return S_FALSE;
        } else {
            return S_FALSE;
        }
        if (lighting && !srcLayout.normal) {
            return S_FALSE;
        }
        if (!describeProcessFvf(outputFvf, dstLayout) ||
            dstLayout.positionBytes != 16u) {
            return S_FALSE;
        }
        return S_OK;
    }

    static DWORD processProgrammableOutputFvf(const ProcessShaderIo& shaderIo) {
        DWORD outputFvf = D3DFVF_XYZRHW;
        if (shaderIo.hasOutputPSize) outputFvf |= D3DFVF_PSIZE;
        if (shaderIo.hasOutputDiffuse) outputFvf |= D3DFVF_DIFFUSE;
        if (shaderIo.hasOutputSpecular) outputFvf |= D3DFVF_SPECULAR;
        int highestTex = -1;
        for (UINT i = 0; i < 8u; ++i) {
            if (shaderIo.hasOutputTex[i]) highestTex = static_cast<int>(i);
        }
        if (highestTex >= 0) {
            for (int i = 0; i <= highestTex; ++i) {
                if (!shaderIo.hasOutputTex[i]) return 0u;
                outputFvf |= D3DFVF_TEXCOORDSIZE4(i);
            }
            outputFvf |= static_cast<DWORD>(highestTex + 1)
                         << D3DFVF_TEXCOUNT_SHIFT;
        }
        return outputFvf;
    }

    static bool processLayoutUsesOnlyStream0(const FvfProcessLayout& layout) {
        for (UINT stream = 1; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            if (layout.streamStride[stream] != 0u) return false;
        }
        return true;
    }

    static bool softwareDrawCanConcatenateInstances(D3DPRIMITIVETYPE type) {
        return type == D3DPT_POINTLIST ||
               type == D3DPT_LINELIST ||
               type == D3DPT_TRIANGLELIST;
    }

    static bool softwareDrawCanExpandInstances(D3DPRIMITIVETYPE type) {
        return softwareDrawCanConcatenateInstances(type) ||
               type == D3DPT_LINESTRIP ||
               type == D3DPT_TRIANGLESTRIP ||
               type == D3DPT_TRIANGLEFAN;
    }

    static D3DPRIMITIVETYPE softwareDrawExpandedPrimitiveType(
            D3DPRIMITIVETYPE type) {
        if (type == D3DPT_LINESTRIP) return D3DPT_LINELIST;
        if (type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN) {
            return D3DPT_TRIANGLELIST;
        }
        return type;
    }

    static HRESULT appendSoftwarePrimitiveVertices(
            const std::vector<std::uint8_t>& source,
            UINT stride,
            D3DPRIMITIVETYPE type,
            UINT primitiveCount,
            std::vector<std::uint8_t>& out) {
        if (stride == 0u) return D3DERR_INVALIDCALL;
        auto appendVertex = [&](UINT index) -> HRESULT {
            const std::uint64_t offset =
                static_cast<std::uint64_t>(index) * stride;
            if (offset > source.size() ||
                stride > source.size() - static_cast<size_t>(offset)) {
                return D3DERR_INVALIDCALL;
            }
            out.insert(out.end(),
                       source.begin() + static_cast<size_t>(offset),
                       source.begin() + static_cast<size_t>(offset) + stride);
            return S_OK;
        };
        switch (type) {
            case D3DPT_POINTLIST:
            case D3DPT_LINELIST:
            case D3DPT_TRIANGLELIST:
                out.insert(out.end(), source.begin(), source.end());
                return S_OK;
            case D3DPT_LINESTRIP:
                for (UINT i = 0; i < primitiveCount; ++i) {
                    HRESULT hr = appendVertex(i);
                    if (FAILED(hr)) return hr;
                    hr = appendVertex(i + 1u);
                    if (FAILED(hr)) return hr;
                }
                return S_OK;
            case D3DPT_TRIANGLESTRIP:
                for (UINT i = 0; i < primitiveCount; ++i) {
                    const UINT a = (i & 1u) ? i + 1u : i;
                    const UINT b = (i & 1u) ? i : i + 1u;
                    HRESULT hr = appendVertex(a);
                    if (FAILED(hr)) return hr;
                    hr = appendVertex(b);
                    if (FAILED(hr)) return hr;
                    hr = appendVertex(i + 2u);
                    if (FAILED(hr)) return hr;
                }
                return S_OK;
            case D3DPT_TRIANGLEFAN:
                for (UINT i = 0; i < primitiveCount; ++i) {
                    HRESULT hr = appendVertex(0u);
                    if (FAILED(hr)) return hr;
                    hr = appendVertex(i + 1u);
                    if (FAILED(hr)) return hr;
                    hr = appendVertex(i + 2u);
                    if (FAILED(hr)) return hr;
                }
                return S_OK;
	    default:
                return D3DERR_INVALIDCALL;
        }
    }

    HRESULT filterSoftwareDrawOutsideClipPrimitives(SoftwareFfpDrawData& draw) {
        if (renderStateValue(D3DRS_CLIPPING) == FALSE ||
            draw.vertices.empty() || draw.stride < 16u ||
            draw.primitiveCount == 0u) {
            return S_OK;
        }
        if (draw.vertices.size() % draw.stride != 0u) {
            return D3DERR_INVALIDCALL;
        }
        const UINT vertexCount =
            static_cast<UINT>(draw.vertices.size() / draw.stride);
        const UINT expectedVertexCount =
            primitiveVertexCount(draw.primitiveType, draw.primitiveCount);
        if (expectedVertexCount > vertexCount) {
            return D3DERR_INVALIDCALL;
        }

	        std::vector<std::uint8_t> filtered;
	        filtered.reserve(draw.vertices.size());
	        UINT keptPrimitiveCount = 0u;
	        bool droppedAny = false;
	        bool clippedTriangles = false;
	        bool clippedLines = false;

        auto clipFlags = [&](UINT index) {
            return transformedSwvpVertexClipFlags(
                draw.vertices, draw.stride, index);
        };
        auto vertexInside = [&](UINT index) {
            return clipFlags(index) == 0u;
        };
	        auto appendVertex = [&](UINT index) {
	            return appendTransformedSwvpVertex(
	                draw.vertices, draw.stride, index, filtered);
        };

        switch (draw.primitiveType) {
            case D3DPT_POINTLIST:
                for (UINT i = 0; i < draw.primitiveCount; ++i) {
                    if (!vertexInside(i)) {
                        droppedAny = true;
                        continue;
                    }
                    HRESULT hr = appendVertex(i);
                    if (FAILED(hr)) return hr;
                    ++keptPrimitiveCount;
                }
                break;
	            case D3DPT_LINELIST:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    const UINT a = i * 2u;
	                    const UINT b = a + 1u;
	                    SwvpClippedVertex av{}, bv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpLine(
	                        draw.fvf, draw.stride, av, bv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b)) != 0u) {
	                        clippedLines = true;
	                    }
	                }
	                break;
	            case D3DPT_LINESTRIP:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    SwvpClippedVertex av{}, bv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, i, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, i + 1u, bv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpLine(
	                        draw.fvf, draw.stride, av, bv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(i) | clipFlags(i + 1u)) != 0u) {
	                        clippedLines = true;
	                    }
	                }
	                draw.primitiveType = D3DPT_LINELIST;
	                clippedLines = true;
	                break;
	            case D3DPT_TRIANGLELIST:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    const UINT a = i * 3u;
	                    const UINT b = a + 1u;
	                    const UINT c = a + 2u;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b) | clipFlags(c)) != 0u) {
	                        clippedTriangles = true;
	                    }
	                }
	                clippedTriangles = clippedTriangles || droppedAny;
	                break;
	            case D3DPT_TRIANGLESTRIP:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    const UINT a = (i & 1u) ? i + 1u : i;
	                    const UINT b = (i & 1u) ? i : i + 1u;
	                    const UINT c = i + 2u;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b) | clipFlags(c)) != 0u) {
	                        clippedTriangles = true;
	                    }
	                }
	                draw.primitiveType = D3DPT_TRIANGLELIST;
	                clippedTriangles = true;
	                break;
	            case D3DPT_TRIANGLEFAN:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    const UINT a = 0u;
	                    const UINT b = i + 1u;
	                    const UINT c = i + 2u;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b) | clipFlags(c)) != 0u) {
	                        clippedTriangles = true;
	                    }
	                }
	                draw.primitiveType = D3DPT_TRIANGLELIST;
	                clippedTriangles = true;
	                break;
            default:
                return D3DERR_INVALIDCALL;
        }

	        if (!droppedAny && !clippedTriangles && !clippedLines) return S_OK;
	        if (clippedTriangles) draw.primitiveType = D3DPT_TRIANGLELIST;
	        if (clippedLines) draw.primitiveType = D3DPT_LINELIST;
	        draw.vertices = std::move(filtered);
	        draw.primitiveCount = keptPrimitiveCount;
	        return S_OK;
    }

    static HRESULT readSoftwareIndexValue(const std::vector<std::uint8_t>& indices,
                                          D3DFORMAT indexFormat,
                                          UINT ordinal,
                                          DWORD& out) {
        if (indexFormat != D3DFMT_INDEX16 && indexFormat != D3DFMT_INDEX32) {
            return D3DERR_INVALIDCALL;
        }
        const UINT indexSize = indexFormat == D3DFMT_INDEX32 ? 4u : 2u;
        const std::uint64_t offset =
            static_cast<std::uint64_t>(ordinal) * indexSize;
        if (offset > indices.size() ||
            indexSize > indices.size() - static_cast<size_t>(offset)) {
            return D3DERR_INVALIDCALL;
        }
        if (indexFormat == D3DFMT_INDEX32) {
            std::memcpy(&out, indices.data() + static_cast<size_t>(offset),
                        sizeof(out));
        } else {
            WORD index16 = 0;
            std::memcpy(&index16, indices.data() + static_cast<size_t>(offset),
                        sizeof(index16));
            out = index16;
        }
        return S_OK;
    }

    static HRESULT appendSoftwareIndex32(std::vector<std::uint8_t>& indices,
                                         DWORD index) {
        const auto oldSize = indices.size();
        indices.resize(oldSize + sizeof(index));
        std::memcpy(indices.data() + oldSize, &index, sizeof(index));
        return S_OK;
    }

    HRESULT filterSoftwareIndexedDrawOutsideClipPrimitives(
            SoftwareFfpDrawData& draw,
            std::vector<std::uint8_t>& indices,
            D3DFORMAT& indexFormat) {
        if (renderStateValue(D3DRS_CLIPPING) == FALSE ||
            draw.vertices.empty() || draw.stride < 16u ||
            draw.primitiveCount == 0u) {
            return S_OK;
        }
        if (draw.vertices.size() % draw.stride != 0u) {
            return D3DERR_INVALIDCALL;
        }
        const UINT vertexCount =
            static_cast<UINT>(draw.vertices.size() / draw.stride);
        const UINT sourceIndexCount =
            primitiveVertexCount(draw.primitiveType, draw.primitiveCount);
        const UINT sourceIndexSize = indexFormat == D3DFMT_INDEX32 ? 4u : 2u;
        if ((indexFormat != D3DFMT_INDEX16 && indexFormat != D3DFMT_INDEX32) ||
            static_cast<std::uint64_t>(sourceIndexCount) * sourceIndexSize >
                indices.size()) {
            return D3DERR_INVALIDCALL;
        }

	        std::vector<std::uint8_t> filtered;
	        filtered.reserve(static_cast<size_t>(sourceIndexCount) * sizeof(DWORD));
	        std::vector<std::uint8_t> clippedVertices;
	        UINT keptPrimitiveCount = 0u;
	        bool clippedTriangles = false;
	        bool clippedLines = false;

        auto readIndex = [&](UINT ordinal, DWORD& outIndex) -> HRESULT {
            HRESULT hr = readSoftwareIndexValue(indices, indexFormat, ordinal,
                                                outIndex);
            if (FAILED(hr)) return hr;
            if (outIndex >= vertexCount) return D3DERR_INVALIDCALL;
            return S_OK;
        };
        auto clipFlags = [&](DWORD index) {
            return transformedSwvpVertexClipFlags(
                draw.vertices, draw.stride, index);
        };
        auto vertexInside = [&](DWORD index) {
            return clipFlags(index) == 0u;
        };
        auto appendIndex = [&](DWORD index) -> HRESULT {
            return appendSoftwareIndex32(filtered, index);
        };
	        switch (draw.primitiveType) {
            case D3DPT_POINTLIST:
                for (UINT i = 0; i < draw.primitiveCount; ++i) {
                    DWORD a = 0;
                    HRESULT hr = readIndex(i, a);
                    if (FAILED(hr)) return hr;
                    if (!vertexInside(a)) continue;
                    hr = appendIndex(a);
                    if (FAILED(hr)) return hr;
                    ++keptPrimitiveCount;
                }
                break;
	            case D3DPT_LINELIST:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    DWORD a = 0, b = 0;
	                    HRESULT hr = readIndex(i * 2u, a);
	                    if (FAILED(hr)) return hr;
	                    hr = readIndex(i * 2u + 1u, b);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpLine(
	                        draw.fvf, draw.stride, av, bv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b)) != 0u) {
	                        clippedLines = true;
	                    }
	                }
	                clippedLines = true;
	                break;
	            case D3DPT_LINESTRIP:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    DWORD a = 0, b = 0;
	                    HRESULT hr = readIndex(i, a);
	                    if (FAILED(hr)) return hr;
	                    hr = readIndex(i + 1u, b);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = clipTransformedSwvpLine(
	                        draw.fvf, draw.stride, av, bv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                }
	                draw.primitiveType = D3DPT_LINELIST;
	                clippedLines = true;
	                break;
	            case D3DPT_TRIANGLELIST:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    DWORD a = 0, b = 0, c = 0;
	                    HRESULT hr = readIndex(i * 3u, a);
                    if (FAILED(hr)) return hr;
                    hr = readIndex(i * 3u + 1u, b);
                    if (FAILED(hr)) return hr;
	                    hr = readIndex(i * 3u + 2u, c);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b) | clipFlags(c)) != 0u) {
	                        clippedTriangles = true;
	                    }
	                }
	                clippedTriangles = true;
	                break;
	            case D3DPT_TRIANGLESTRIP:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
                    DWORD a = 0, b = 0, c = 0;
                    HRESULT hr = readIndex((i & 1u) ? i + 1u : i, a);
                    if (FAILED(hr)) return hr;
                    hr = readIndex((i & 1u) ? i : i + 1u, b);
                    if (FAILED(hr)) return hr;
	                    hr = readIndex(i + 2u, c);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                }
	                draw.primitiveType = D3DPT_TRIANGLELIST;
	                clippedTriangles = true;
	                break;
	            case D3DPT_TRIANGLEFAN:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
                    DWORD a = 0, b = 0, c = 0;
                    HRESULT hr = readIndex(0u, a);
                    if (FAILED(hr)) return hr;
                    hr = readIndex(i + 1u, b);
                    if (FAILED(hr)) return hr;
	                    hr = readIndex(i + 2u, c);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                }
	                draw.primitiveType = D3DPT_TRIANGLELIST;
	                clippedTriangles = true;
	                break;
            default:
                return D3DERR_INVALIDCALL;
        }

	        if (clippedLines || clippedTriangles) {
	            const UINT vertexCount =
	                draw.stride != 0u
	                    ? static_cast<UINT>(clippedVertices.size() / draw.stride)
	                    : 0u;
	            filtered.clear();
	            filtered.reserve(static_cast<size_t>(vertexCount) * sizeof(DWORD));
	            for (UINT i = 0; i < vertexCount; ++i) {
	                HRESULT hr = appendSoftwareIndex32(filtered, i);
	                if (FAILED(hr)) return hr;
	            }
	            draw.vertices = std::move(clippedVertices);
	            draw.primitiveType = clippedTriangles
	                ? D3DPT_TRIANGLELIST : D3DPT_LINELIST;
	        }
	        indices = std::move(filtered);
	        indexFormat = D3DFMT_INDEX32;
	        draw.primitiveCount = keptPrimitiveCount;
	        return S_OK;
	    }

    UINT softwareDrawInstanceCount() const {
        if ((streamFreq_[0] & D3DSTREAMSOURCE_INDEXEDDATA) == 0u) {
            return 1u;
        }
        const UINT count = streamFreq_[0] & 0x3fffffffu;
        return count ? count : 1u;
    }

    HRESULT applySoftwareInstanceStreamOffsets(UINT instance,
                                               UINT savedOffsets[16]) {
        for (UINT stream = 0; stream < 16u; ++stream) {
            savedOffsets[stream] = streamOff_[stream];
        }
        for (UINT stream = 1; stream < 16u; ++stream) {
            if ((streamFreq_[stream] & D3DSTREAMSOURCE_INSTANCEDATA) == 0u) {
                continue;
            }
            const UINT divider = std::max<UINT>(streamFreq_[stream] & 0x3fffffffu, 1u);
            const UINT element = instance / divider;
            const std::uint64_t offset =
                static_cast<std::uint64_t>(streamOff_[stream]) +
                static_cast<std::uint64_t>(element) * streamStr_[stream];
            if (offset > 0xffffffffull) {
                return D3DERR_INVALIDCALL;
            }
            streamOff_[stream] = static_cast<UINT>(offset);
        }
        return S_OK;
    }

    void restoreSoftwareInstanceStreamOffsets(const UINT savedOffsets[16]) {
        for (UINT stream = 0; stream < 16u; ++stream) {
            streamOff_[stream] = savedOffsets[stream];
        }
    }

    HRESULT describeSoftwareProgrammableDrawTarget(
        DWORD& outputFvf,
        FvfProcessLayout& srcLayout,
        FvfProcessLayout& dstLayout) {
        outputFvf = 0;
        srcLayout = {};
        dstLayout = {};
        if (!softwareVertexProcessing_ || !vs_) {
            return S_FALSE;
        }
        if (fvf_ != 0) {
            const DWORD positionMask = fvf_ & D3DFVF_POSITION_MASK;
            if ((positionMask != D3DFVF_XYZ &&
                 positionMask != D3DFVF_XYZW &&
                 !processFvfXyzbPosition(positionMask)) ||
                !describeProcessFvf(fvf_, srcLayout)) {
                return S_FALSE;
            }
        } else if (vdecl_) {
            if (!describeProcessDeclaration(vdecl_, srcLayout, false)) {
                return S_FALSE;
            }
        } else {
            return S_FALSE;
        }
        UINT shaderBytes = 0;
        HRESULT hr = vs_->GetFunction(nullptr, &shaderBytes);
        if (FAILED(hr) || shaderBytes == 0u ||
            (shaderBytes % sizeof(DWORD)) != 0u) {
            return S_FALSE;
        }
        std::vector<DWORD> shaderWords(shaderBytes / sizeof(DWORD));
        hr = vs_->GetFunction(shaderWords.data(), &shaderBytes);
        ProcessShaderIo shaderIo{};
        if (FAILED(hr) ||
            !analyzeSimpleProcessVertexShader(shaderWords, shaderIo)) {
            return S_FALSE;
        }
        outputFvf = processProgrammableOutputFvf(shaderIo);
        if (outputFvf == 0u ||
            !describeProcessFvf(outputFvf, dstLayout) ||
            dstLayout.positionBytes != 16u) {
            outputFvf = 0;
            return S_FALSE;
        }
        return S_OK;
    }

    HRESULT readTransformedVertexBuffer(IDirect3DVertexBuffer9* dstBuffer,
                                        UINT bytes,
                                        SoftwareFfpDrawData& out,
                                        DWORD outputFvf,
                                        UINT outputStride) {
        out = {};
        if (bytes == 0u) return S_FALSE;
        void* mapped = nullptr;
        HRESULT hr = dstBuffer->Lock(0, bytes, &mapped, D3DLOCK_READONLY);
        if (FAILED(hr)) return hr;
        if (!mapped) {
            (void)dstBuffer->Unlock();
            return D3DERR_INVALIDCALL;
        }
        out.vertices.resize(bytes);
        std::memcpy(out.vertices.data(), mapped, bytes);
        hr = dstBuffer->Unlock();
        if (FAILED(hr)) return hr;
        out.fvf = outputFvf;
        out.stride = outputStride;
        return S_OK;
    }

    HRESULT trySoftwareProgrammableTransformBoundVertices(
        UINT startVertex,
        UINT vertexCount,
        SoftwareFfpDrawData& out) {
        out = {};
        if (vertexCount == 0u) return S_FALSE;
        DWORD outputFvf = 0;
        FvfProcessLayout srcLayout{};
        FvfProcessLayout dstLayout{};
        HRESULT hr = describeSoftwareProgrammableDrawTarget(
            outputFvf, srcLayout, dstLayout);
        if (hr != S_OK) return hr;
        std::uint32_t outputBytes = 0;
        if (!checkedByteCount(vertexCount, dstLayout.stride, outputBytes)) {
            return D3DERR_INVALIDCALL;
        }
        IDirect3DVertexBuffer9* dstBuffer = nullptr;
        hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                                &dstBuffer, nullptr);
        if (FAILED(hr)) return hr;
        hr = ProcessVertices(startVertex, 0, vertexCount, dstBuffer, nullptr, 0);
        if (SUCCEEDED(hr)) {
            hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                             outputFvf, dstLayout.stride);
            if (SUCCEEDED(hr)) out.bypassVertexShader = true;
        }
        dstBuffer->Release();
        return hr;
    }

    HRESULT trySoftwareFfpTransformBoundVertices(UINT startVertex,
                                                 UINT vertexCount,
                                                 SoftwareFfpDrawData& out) {
        out = {};
        if (vertexCount == 0u) return S_FALSE;
        DWORD outputFvf = 0;
        FvfProcessLayout srcLayout{};
        FvfProcessLayout dstLayout{};
        HRESULT hr = describeSoftwareFfpDrawTarget(outputFvf, srcLayout, dstLayout);
        if (hr != S_OK) return hr;
        if (!streamSrc_[0] || streamStr_[0] < srcLayout.stride) {
            return S_FALSE;
        }
        std::uint32_t outputBytes = 0;
        if (!checkedByteCount(vertexCount, dstLayout.stride, outputBytes)) {
            return D3DERR_INVALIDCALL;
        }
        IDirect3DVertexBuffer9* dstBuffer = nullptr;
        hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                                &dstBuffer, nullptr);
        if (FAILED(hr)) return hr;
        hr = ProcessVertices(startVertex, 0, vertexCount, dstBuffer, nullptr, 0);
        if (SUCCEEDED(hr)) {
            hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                             outputFvf, dstLayout.stride);
        }
        dstBuffer->Release();
        return hr;
    }

    HRESULT trySoftwareFfpDrawPrimitive(D3DPRIMITIVETYPE type,
                                        UINT startVertex,
                                        UINT primitiveCount,
                                        SoftwareFfpDrawData& out) {
        out = {};
        const UINT vertexCount = primitiveVertexCount(type, primitiveCount);
        const UINT instanceCount = softwareDrawInstanceCount();
        if (instanceCount <= 1u) {
            HRESULT hr = trySoftwareFfpTransformBoundVertices(startVertex, vertexCount, out);
            if (SUCCEEDED(hr) && hr != S_FALSE) {
                out.primitiveType = type;
                out.primitiveCount = primitiveCount;
            }
            return hr;
        }
        if (!softwareDrawCanExpandInstances(type) ||
            primitiveCount > 0xffffffffu / instanceCount) {
            return S_FALSE;
        }
        out.primitiveType = softwareDrawExpandedPrimitiveType(type);
        for (UINT instance = 0; instance < instanceCount; ++instance) {
            UINT savedOffsets[16]{};
            HRESULT hr = applySoftwareInstanceStreamOffsets(instance, savedOffsets);
            if (SUCCEEDED(hr)) {
                SoftwareFfpDrawData instanceDraw{};
                hr = trySoftwareFfpTransformBoundVertices(startVertex, vertexCount,
                                                          instanceDraw);
                if (SUCCEEDED(hr) && hr != S_FALSE) {
                    if (out.vertices.empty()) {
                        out.fvf = instanceDraw.fvf;
                        out.stride = instanceDraw.stride;
                        out.bypassVertexShader = instanceDraw.bypassVertexShader;
                    } else if (out.fvf != instanceDraw.fvf ||
                               out.stride != instanceDraw.stride ||
                               out.bypassVertexShader != instanceDraw.bypassVertexShader) {
                        hr = D3DERR_INVALIDCALL;
                    }
                    if (SUCCEEDED(hr)) {
                        hr = appendSoftwarePrimitiveVertices(
                            instanceDraw.vertices, instanceDraw.stride, type,
                            primitiveCount, out.vertices);
                    }
                }
            }
            restoreSoftwareInstanceStreamOffsets(savedOffsets);
            if (FAILED(hr) || hr == S_FALSE) {
                out = {};
                return hr;
            }
        }
        out.primitiveCount = primitiveCount * instanceCount;
        return S_OK;
    }

    HRESULT trySoftwareProgrammableDrawPrimitive(D3DPRIMITIVETYPE type,
                                                 UINT startVertex,
                                                 UINT primitiveCount,
                                                 SoftwareFfpDrawData& out) {
        out = {};
        const UINT vertexCount = primitiveVertexCount(type, primitiveCount);
        const UINT instanceCount = softwareDrawInstanceCount();
        if (instanceCount <= 1u) {
            HRESULT hr = trySoftwareProgrammableTransformBoundVertices(
                startVertex, vertexCount, out);
            if (SUCCEEDED(hr) && hr != S_FALSE) {
                out.primitiveType = type;
                out.primitiveCount = primitiveCount;
            }
            return hr;
        }
        if (!softwareDrawCanExpandInstances(type) ||
            primitiveCount > 0xffffffffu / instanceCount) {
            return S_FALSE;
        }
        out.primitiveType = softwareDrawExpandedPrimitiveType(type);
        for (UINT instance = 0; instance < instanceCount; ++instance) {
            UINT savedOffsets[16]{};
            HRESULT hr = applySoftwareInstanceStreamOffsets(instance, savedOffsets);
            if (SUCCEEDED(hr)) {
                SoftwareFfpDrawData instanceDraw{};
                hr = trySoftwareProgrammableTransformBoundVertices(
                    startVertex, vertexCount, instanceDraw);
                if (SUCCEEDED(hr) && hr != S_FALSE) {
                    if (out.vertices.empty()) {
                        out.fvf = instanceDraw.fvf;
                        out.stride = instanceDraw.stride;
                        out.bypassVertexShader = instanceDraw.bypassVertexShader;
                    } else if (out.fvf != instanceDraw.fvf ||
                               out.stride != instanceDraw.stride ||
                               out.bypassVertexShader != instanceDraw.bypassVertexShader) {
                        hr = D3DERR_INVALIDCALL;
                    }
                    if (SUCCEEDED(hr)) {
                        hr = appendSoftwarePrimitiveVertices(
                            instanceDraw.vertices, instanceDraw.stride, type,
                            primitiveCount, out.vertices);
                    }
                }
            }
            restoreSoftwareInstanceStreamOffsets(savedOffsets);
            if (FAILED(hr) || hr == S_FALSE) {
                out = {};
                return hr;
            }
        }
        out.primitiveCount = primitiveCount * instanceCount;
        return S_OK;
    }

    HRESULT readSoftwareFfpAdjustedIndices(UINT startIndex,
                                           UINT indexCount,
                                           UINT minVertex,
                                           UINT numVertices,
                                           std::vector<std::uint8_t>& out,
                                           D3DFORMAT& indexFormat) {
        out.clear();
        indexFormat = D3DFMT_UNKNOWN;
        if (!indexBuf_ || indexCount == 0u || numVertices == 0u) return S_FALSE;
        D3DINDEXBUFFER_DESC desc{};
        HRESULT hr = indexBuf_->GetDesc(&desc);
        if (FAILED(hr)) return S_FALSE;
        if (desc.Format != D3DFMT_INDEX16 && desc.Format != D3DFMT_INDEX32) {
            return S_FALSE;
        }
        const UINT indexSize = desc.Format == D3DFMT_INDEX32 ? 4u : 2u;
        std::uint32_t indexBytes = 0;
        if (!checkedByteCount(indexCount, indexSize, indexBytes)) {
            return D3DERR_INVALIDCALL;
        }
        const std::uint64_t byteOffset =
            static_cast<std::uint64_t>(startIndex) * indexSize;
        if (byteOffset > 0xffffffffull ||
            indexBytes > desc.Size ||
            byteOffset > desc.Size - indexBytes) {
            return D3DERR_INVALIDCALL;
        }
        void* mapped = nullptr;
        hr = indexBuf_->Lock(static_cast<UINT>(byteOffset), indexBytes,
                             &mapped, D3DLOCK_READONLY);
        if (FAILED(hr)) return S_FALSE;
        if (!mapped) {
            (void)indexBuf_->Unlock();
            return D3DERR_INVALIDCALL;
        }
        out.resize(indexBytes);
        bool supportedRange = true;
        if (desc.Format == D3DFMT_INDEX16) {
            const auto* src = static_cast<const std::uint8_t*>(mapped);
            for (UINT i = 0; i < indexCount; ++i) {
                WORD index = 0;
                std::memcpy(&index, src + i * indexSize, sizeof(index));
                if (index < minVertex || index - minVertex >= numVertices) {
                    supportedRange = false;
                    break;
                }
                const WORD adjusted = static_cast<WORD>(index - minVertex);
                std::memcpy(out.data() + i * indexSize, &adjusted, sizeof(adjusted));
            }
        } else {
            const auto* src = static_cast<const std::uint8_t*>(mapped);
            for (UINT i = 0; i < indexCount; ++i) {
                DWORD index = 0;
                std::memcpy(&index, src + i * indexSize, sizeof(index));
                if (index < minVertex || index - minVertex >= numVertices) {
                    supportedRange = false;
                    break;
                }
                const DWORD adjusted = index - minVertex;
                std::memcpy(out.data() + i * indexSize, &adjusted, sizeof(adjusted));
            }
        }
        hr = indexBuf_->Unlock();
        if (FAILED(hr)) return hr;
        if (!supportedRange) {
            out.clear();
            return S_FALSE;
        }
        indexFormat = desc.Format;
        return S_OK;
    }

    static HRESULT appendSoftwareIndicesWithBase32(
        const std::vector<std::uint8_t>& source,
        D3DFORMAT sourceFormat,
        UINT indexCount,
        UINT baseVertex,
        std::vector<std::uint8_t>& out) {
        if (sourceFormat != D3DFMT_INDEX16 && sourceFormat != D3DFMT_INDEX32) {
            return D3DERR_INVALIDCALL;
        }
        const UINT sourceIndexSize = sourceFormat == D3DFMT_INDEX32 ? 4u : 2u;
        const std::uint64_t required =
            static_cast<std::uint64_t>(indexCount) * sourceIndexSize;
        if (required > source.size()) {
            return D3DERR_INVALIDCALL;
        }
        const auto* bytes = source.data();
        for (UINT i = 0; i < indexCount; ++i) {
            DWORD index = 0;
            if (sourceFormat == D3DFMT_INDEX32) {
                std::memcpy(&index, bytes + i * sourceIndexSize, sizeof(index));
            } else {
                WORD index16 = 0;
                std::memcpy(&index16, bytes + i * sourceIndexSize, sizeof(index16));
                index = index16;
            }
            const std::uint64_t adjusted =
                static_cast<std::uint64_t>(index) + baseVertex;
            if (adjusted > 0xffffffffull) {
                return D3DERR_INVALIDCALL;
            }
            const DWORD adjusted32 = static_cast<DWORD>(adjusted);
            const auto oldSize = out.size();
            out.resize(oldSize + sizeof(adjusted32));
            std::memcpy(out.data() + oldSize, &adjusted32, sizeof(adjusted32));
        }
        return S_OK;
    }

    static HRESULT appendSoftwarePrimitiveIndicesWithBase32(
        const std::vector<std::uint8_t>& source,
        D3DFORMAT sourceFormat,
        D3DPRIMITIVETYPE type,
        UINT primitiveCount,
        UINT baseVertex,
        std::vector<std::uint8_t>& out) {
        if (sourceFormat != D3DFMT_INDEX16 && sourceFormat != D3DFMT_INDEX32) {
            return D3DERR_INVALIDCALL;
        }
        const UINT sourceIndexSize = sourceFormat == D3DFMT_INDEX32 ? 4u : 2u;
        const UINT sourceIndexCount = primitiveVertexCount(type, primitiveCount);
        const std::uint64_t required =
            static_cast<std::uint64_t>(sourceIndexCount) * sourceIndexSize;
        if (required > source.size()) {
            return D3DERR_INVALIDCALL;
        }
        auto readIndex = [&](UINT index, DWORD& outIndex) -> HRESULT {
            if (index >= sourceIndexCount) return D3DERR_INVALIDCALL;
            const auto* bytes = source.data() +
                static_cast<size_t>(index) * sourceIndexSize;
            if (sourceFormat == D3DFMT_INDEX32) {
                std::memcpy(&outIndex, bytes, sizeof(outIndex));
            } else {
                WORD index16 = 0;
                std::memcpy(&index16, bytes, sizeof(index16));
                outIndex = index16;
            }
            return S_OK;
        };
        auto appendIndex = [&](UINT index) -> HRESULT {
            DWORD value = 0;
            HRESULT hr = readIndex(index, value);
            if (FAILED(hr)) return hr;
            const std::uint64_t adjusted =
                static_cast<std::uint64_t>(value) + baseVertex;
            if (adjusted > 0xffffffffull) return D3DERR_INVALIDCALL;
            const DWORD adjusted32 = static_cast<DWORD>(adjusted);
            const auto oldSize = out.size();
            out.resize(oldSize + sizeof(adjusted32));
            std::memcpy(out.data() + oldSize, &adjusted32, sizeof(adjusted32));
            return S_OK;
        };
        switch (type) {
            case D3DPT_POINTLIST:
            case D3DPT_LINELIST:
            case D3DPT_TRIANGLELIST:
                return appendSoftwareIndicesWithBase32(
                    source, sourceFormat, sourceIndexCount, baseVertex, out);
            case D3DPT_LINESTRIP:
                for (UINT i = 0; i < primitiveCount; ++i) {
                    HRESULT hr = appendIndex(i);
                    if (FAILED(hr)) return hr;
                    hr = appendIndex(i + 1u);
                    if (FAILED(hr)) return hr;
                }
                return S_OK;
            case D3DPT_TRIANGLESTRIP:
                for (UINT i = 0; i < primitiveCount; ++i) {
                    const UINT a = (i & 1u) ? i + 1u : i;
                    const UINT b = (i & 1u) ? i : i + 1u;
                    HRESULT hr = appendIndex(a);
                    if (FAILED(hr)) return hr;
                    hr = appendIndex(b);
                    if (FAILED(hr)) return hr;
                    hr = appendIndex(i + 2u);
                    if (FAILED(hr)) return hr;
                }
                return S_OK;
            case D3DPT_TRIANGLEFAN:
                for (UINT i = 0; i < primitiveCount; ++i) {
                    HRESULT hr = appendIndex(0u);
                    if (FAILED(hr)) return hr;
                    hr = appendIndex(i + 1u);
                    if (FAILED(hr)) return hr;
                    hr = appendIndex(i + 2u);
                    if (FAILED(hr)) return hr;
                }
                return S_OK;
            default:
                return D3DERR_INVALIDCALL;
        }
    }

    HRESULT trySoftwareFfpDrawIndexedPrimitive(D3DPRIMITIVETYPE type,
                                               INT baseVertex,
                                               UINT minVertex,
                                               UINT numVertices,
                                               UINT startIndex,
                                               UINT primitiveCount,
                                               SoftwareFfpDrawData& out,
                                               std::vector<std::uint8_t>& indices,
                                               D3DFORMAT& indexFormat) {
        out = {};
        indices.clear();
        indexFormat = D3DFMT_UNKNOWN;
        const UINT indexCount = primitiveVertexCount(type, primitiveCount);
        if (indexCount == 0u || numVertices == 0u) return S_FALSE;
        const std::int64_t srcStart =
            static_cast<std::int64_t>(baseVertex) +
            static_cast<std::int64_t>(minVertex);
        if (srcStart < 0 || srcStart > 0xffffffffll) return S_FALSE;
        HRESULT hr = readSoftwareFfpAdjustedIndices(
            startIndex, indexCount, minVertex, numVertices, indices, indexFormat);
        if (hr != S_OK) return hr;
        const UINT instanceCount = softwareDrawInstanceCount();
        if (instanceCount <= 1u) {
            hr = trySoftwareFfpTransformBoundVertices(
                static_cast<UINT>(srcStart), numVertices, out);
            if (SUCCEEDED(hr) && hr != S_FALSE) {
                out.primitiveType = type;
                out.primitiveCount = primitiveCount;
            }
        } else {
            if (!softwareDrawCanExpandInstances(type) ||
                primitiveCount > 0xffffffffu / instanceCount ||
                indexCount > 0xffffffffu / instanceCount ||
                numVertices > 0xffffffffu / instanceCount) {
                indices.clear();
                return S_FALSE;
            }
            const std::vector<std::uint8_t> sourceIndices = indices;
            const D3DFORMAT sourceIndexFormat = indexFormat;
            indices.clear();
            indexFormat = D3DFMT_INDEX32;
            out.primitiveType = softwareDrawExpandedPrimitiveType(type);
            for (UINT instance = 0; instance < instanceCount; ++instance) {
                UINT savedOffsets[16]{};
                hr = applySoftwareInstanceStreamOffsets(instance, savedOffsets);
                if (SUCCEEDED(hr)) {
                    SoftwareFfpDrawData instanceDraw{};
                    hr = trySoftwareFfpTransformBoundVertices(
                        static_cast<UINT>(srcStart), numVertices, instanceDraw);
                    if (SUCCEEDED(hr) && hr != S_FALSE) {
                        if (out.vertices.empty()) {
                            out.fvf = instanceDraw.fvf;
                            out.stride = instanceDraw.stride;
                            out.bypassVertexShader = instanceDraw.bypassVertexShader;
                        } else if (out.fvf != instanceDraw.fvf ||
                                   out.stride != instanceDraw.stride ||
                                   out.bypassVertexShader != instanceDraw.bypassVertexShader) {
                            hr = D3DERR_INVALIDCALL;
                        }
                        if (SUCCEEDED(hr)) {
                            out.vertices.insert(out.vertices.end(),
                                                instanceDraw.vertices.begin(),
                                                instanceDraw.vertices.end());
                            hr = appendSoftwarePrimitiveIndicesWithBase32(
                                sourceIndices, sourceIndexFormat, type, primitiveCount,
                                instance * numVertices, indices);
                        }
                    }
                }
                restoreSoftwareInstanceStreamOffsets(savedOffsets);
                if (FAILED(hr) || hr == S_FALSE) {
                    break;
                }
            }
            if (SUCCEEDED(hr) && hr != S_FALSE) {
                out.primitiveCount = primitiveCount * instanceCount;
            }
        }
        if (FAILED(hr) || hr == S_FALSE) {
            out = {};
            indices.clear();
        }
        return hr;
    }

    HRESULT trySoftwareProgrammableDrawIndexedPrimitive(
        D3DPRIMITIVETYPE type,
        INT baseVertex,
        UINT minVertex,
        UINT numVertices,
        UINT startIndex,
        UINT primitiveCount,
        SoftwareFfpDrawData& out,
        std::vector<std::uint8_t>& indices,
        D3DFORMAT& indexFormat) {
        out = {};
        indices.clear();
        indexFormat = D3DFMT_UNKNOWN;
        const UINT indexCount = primitiveVertexCount(type, primitiveCount);
        if (indexCount == 0u || numVertices == 0u) return S_FALSE;
        const std::int64_t srcStart =
            static_cast<std::int64_t>(baseVertex) +
            static_cast<std::int64_t>(minVertex);
        if (srcStart < 0 || srcStart > 0xffffffffll) return S_FALSE;
        HRESULT hr = readSoftwareFfpAdjustedIndices(
            startIndex, indexCount, minVertex, numVertices, indices, indexFormat);
        if (hr != S_OK) return hr;
        const UINT instanceCount = softwareDrawInstanceCount();
        if (instanceCount <= 1u) {
            hr = trySoftwareProgrammableTransformBoundVertices(
                static_cast<UINT>(srcStart), numVertices, out);
            if (SUCCEEDED(hr) && hr != S_FALSE) {
                out.primitiveType = type;
                out.primitiveCount = primitiveCount;
            }
        } else {
            if (!softwareDrawCanExpandInstances(type) ||
                primitiveCount > 0xffffffffu / instanceCount ||
                indexCount > 0xffffffffu / instanceCount ||
                numVertices > 0xffffffffu / instanceCount) {
                indices.clear();
                return S_FALSE;
            }
            const std::vector<std::uint8_t> sourceIndices = indices;
            const D3DFORMAT sourceIndexFormat = indexFormat;
            indices.clear();
            indexFormat = D3DFMT_INDEX32;
            out.primitiveType = softwareDrawExpandedPrimitiveType(type);
            for (UINT instance = 0; instance < instanceCount; ++instance) {
                UINT savedOffsets[16]{};
                hr = applySoftwareInstanceStreamOffsets(instance, savedOffsets);
                if (SUCCEEDED(hr)) {
                    SoftwareFfpDrawData instanceDraw{};
                    hr = trySoftwareProgrammableTransformBoundVertices(
                        static_cast<UINT>(srcStart), numVertices, instanceDraw);
                    if (SUCCEEDED(hr) && hr != S_FALSE) {
                        if (out.vertices.empty()) {
                            out.fvf = instanceDraw.fvf;
                            out.stride = instanceDraw.stride;
                            out.bypassVertexShader = instanceDraw.bypassVertexShader;
                        } else if (out.fvf != instanceDraw.fvf ||
                                   out.stride != instanceDraw.stride ||
                                   out.bypassVertexShader != instanceDraw.bypassVertexShader) {
                            hr = D3DERR_INVALIDCALL;
                        }
                        if (SUCCEEDED(hr)) {
                            out.vertices.insert(out.vertices.end(),
                                                instanceDraw.vertices.begin(),
                                                instanceDraw.vertices.end());
                            hr = appendSoftwarePrimitiveIndicesWithBase32(
                                sourceIndices, sourceIndexFormat, type, primitiveCount,
                                instance * numVertices, indices);
                        }
                    }
                }
                restoreSoftwareInstanceStreamOffsets(savedOffsets);
                if (FAILED(hr) || hr == S_FALSE) {
                    break;
                }
            }
            if (SUCCEEDED(hr) && hr != S_FALSE) {
                out.primitiveCount = primitiveCount * instanceCount;
            }
        }
        if (FAILED(hr) || hr == S_FALSE) {
            out = {};
            indices.clear();
        }
        return hr;
    }

    HRESULT trySoftwareFfpDrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                          UINT primitiveCount,
                                          const void* data,
                                          UINT stride,
                                          SoftwareFfpDrawData& out) {
        out = {};
        const UINT vertexCount = primitiveVertexCount(type, primitiveCount);
        if (vertexCount == 0u) return S_FALSE;
        DWORD outputFvf = 0;
        FvfProcessLayout srcLayout{};
        FvfProcessLayout dstLayout{};
        HRESULT hr = describeSoftwareFfpDrawTarget(outputFvf, srcLayout, dstLayout);
        if (hr != S_OK) return hr;
        if (stride < srcLayout.stride) return S_FALSE;
        std::uint32_t inputBytes = 0;
        std::uint32_t outputBytes = 0;
        if (!checkedByteCount(vertexCount, stride, inputBytes) ||
            !checkedByteCount(vertexCount, dstLayout.stride, outputBytes) ||
            (inputBytes != 0u && !data)) {
            return D3DERR_INVALIDCALL;
        }
        IDirect3DVertexBuffer9* srcBuffer = nullptr;
        IDirect3DVertexBuffer9* dstBuffer = nullptr;
        hr = CreateVertexBuffer(inputBytes, 0, fvf_, D3DPOOL_SYSTEMMEM,
                                &srcBuffer, nullptr);
        if (FAILED(hr)) return hr;
        void* mapped = nullptr;
        hr = srcBuffer->Lock(0, inputBytes, &mapped, 0);
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(mapped, data, inputBytes);
            hr = srcBuffer->Unlock();
        } else if (SUCCEEDED(hr)) {
            hr = D3DERR_INVALIDCALL;
        }
        if (FAILED(hr)) {
            srcBuffer->Release();
            return hr;
        }
        hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                                &dstBuffer, nullptr);
        if (FAILED(hr)) {
            srcBuffer->Release();
            return hr;
        }

        IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
        if (savedStream0) savedStream0->AddRef();
        const UINT savedOffset0 = streamOff_[0];
        const UINT savedStride0 = streamStr_[0];
        setRef(streamSrc_[0], srcBuffer);
        streamOff_[0] = 0;
        streamStr_[0] = stride;
        hr = ProcessVertices(0, 0, vertexCount, dstBuffer, nullptr, 0);
        setRef(streamSrc_[0], savedStream0);
        streamOff_[0] = savedOffset0;
        streamStr_[0] = savedStride0;
        if (savedStream0) savedStream0->Release();

        if (SUCCEEDED(hr)) {
            hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                             outputFvf, dstLayout.stride);
            if (SUCCEEDED(hr)) {
                out.primitiveType = type;
                out.primitiveCount = primitiveCount;
            }
        }
        dstBuffer->Release();
        srcBuffer->Release();
        return hr;
    }

    HRESULT trySoftwareProgrammableDrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                                   UINT primitiveCount,
                                                   const void* data,
                                                   UINT stride,
                                                   SoftwareFfpDrawData& out) {
        out = {};
        const UINT vertexCount = primitiveVertexCount(type, primitiveCount);
        if (vertexCount == 0u) return S_FALSE;
        DWORD outputFvf = 0;
        FvfProcessLayout srcLayout{};
        FvfProcessLayout dstLayout{};
        HRESULT hr = describeSoftwareProgrammableDrawTarget(
            outputFvf, srcLayout, dstLayout);
        if (hr != S_OK) return hr;
        if (!processLayoutUsesOnlyStream0(srcLayout) || stride < srcLayout.stride) {
            return S_FALSE;
        }
        std::uint32_t inputBytes = 0;
        std::uint32_t outputBytes = 0;
        if (!checkedByteCount(vertexCount, stride, inputBytes) ||
            !checkedByteCount(vertexCount, dstLayout.stride, outputBytes) ||
            (inputBytes != 0u && !data)) {
            return D3DERR_INVALIDCALL;
        }
        IDirect3DVertexBuffer9* srcBuffer = nullptr;
        IDirect3DVertexBuffer9* dstBuffer = nullptr;
        hr = CreateVertexBuffer(inputBytes, 0, fvf_, D3DPOOL_SYSTEMMEM,
                                &srcBuffer, nullptr);
        if (FAILED(hr)) return hr;
        void* mapped = nullptr;
        hr = srcBuffer->Lock(0, inputBytes, &mapped, 0);
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(mapped, data, inputBytes);
            hr = srcBuffer->Unlock();
        } else if (SUCCEEDED(hr)) {
            hr = D3DERR_INVALIDCALL;
        }
        if (FAILED(hr)) {
            srcBuffer->Release();
            return hr;
        }
        hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                                &dstBuffer, nullptr);
        if (FAILED(hr)) {
            srcBuffer->Release();
            return hr;
        }

        IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
        if (savedStream0) savedStream0->AddRef();
        const UINT savedOffset0 = streamOff_[0];
        const UINT savedStride0 = streamStr_[0];
        setRef(streamSrc_[0], srcBuffer);
        streamOff_[0] = 0;
        streamStr_[0] = stride;
        hr = ProcessVertices(0, 0, vertexCount, dstBuffer, nullptr, 0);
        setRef(streamSrc_[0], savedStream0);
        streamOff_[0] = savedOffset0;
        streamStr_[0] = savedStride0;
        if (savedStream0) savedStream0->Release();

        if (SUCCEEDED(hr)) {
            hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                             outputFvf, dstLayout.stride);
            if (SUCCEEDED(hr)) {
                out.primitiveType = type;
                out.primitiveCount = primitiveCount;
                out.bypassVertexShader = true;
            }
        }
        dstBuffer->Release();
        srcBuffer->Release();
        return hr;
    }

    HRESULT trySoftwareFfpDrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                 UINT minVertex,
                                                 UINT numVertices,
                                                 UINT primitiveCount,
                                                 const void* vertexData,
                                                 UINT stride,
                                                 SoftwareFfpDrawData& out) {
        out = {};
        if (primitiveVertexCount(type, primitiveCount) == 0u || numVertices == 0u) {
            return S_FALSE;
        }
        if (minVertex > 0xffffffffu - numVertices) {
            return D3DERR_INVALIDCALL;
        }
        DWORD outputFvf = 0;
        FvfProcessLayout srcLayout{};
        FvfProcessLayout dstLayout{};
        HRESULT hr = describeSoftwareFfpDrawTarget(outputFvf, srcLayout, dstLayout);
        if (hr != S_OK) return hr;
        if (stride < srcLayout.stride) return S_FALSE;
        const UINT vertexCount = minVertex + numVertices;
        std::uint32_t inputBytes = 0;
        std::uint32_t outputBytes = 0;
        if (!checkedByteCount(vertexCount, stride, inputBytes) ||
            !checkedByteCount(vertexCount, dstLayout.stride, outputBytes) ||
            (inputBytes != 0u && !vertexData)) {
            return D3DERR_INVALIDCALL;
        }
        IDirect3DVertexBuffer9* srcBuffer = nullptr;
        IDirect3DVertexBuffer9* dstBuffer = nullptr;
        hr = CreateVertexBuffer(inputBytes, 0, fvf_, D3DPOOL_SYSTEMMEM,
                                &srcBuffer, nullptr);
        if (FAILED(hr)) return hr;
        void* mapped = nullptr;
        hr = srcBuffer->Lock(0, inputBytes, &mapped, 0);
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(mapped, vertexData, inputBytes);
            hr = srcBuffer->Unlock();
        } else if (SUCCEEDED(hr)) {
            hr = D3DERR_INVALIDCALL;
        }
        if (FAILED(hr)) {
            srcBuffer->Release();
            return hr;
        }
        hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                                &dstBuffer, nullptr);
        if (FAILED(hr)) {
            srcBuffer->Release();
            return hr;
        }

        IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
        if (savedStream0) savedStream0->AddRef();
        const UINT savedOffset0 = streamOff_[0];
        const UINT savedStride0 = streamStr_[0];
        setRef(streamSrc_[0], srcBuffer);
        streamOff_[0] = 0;
        streamStr_[0] = stride;
        hr = ProcessVertices(0, 0, vertexCount, dstBuffer, nullptr, 0);
        setRef(streamSrc_[0], savedStream0);
        streamOff_[0] = savedOffset0;
        streamStr_[0] = savedStride0;
        if (savedStream0) savedStream0->Release();

        if (SUCCEEDED(hr)) {
            hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                             outputFvf, dstLayout.stride);
            if (SUCCEEDED(hr)) {
                out.primitiveType = type;
                out.primitiveCount = primitiveCount;
            }
        }
        dstBuffer->Release();
        srcBuffer->Release();
        return hr;
    }

    HRESULT trySoftwareProgrammableDrawIndexedPrimitiveUP(
        D3DPRIMITIVETYPE type,
        UINT minVertex,
        UINT numVertices,
        UINT primitiveCount,
        const void* vertexData,
        UINT stride,
        SoftwareFfpDrawData& out) {
        out = {};
        if (primitiveVertexCount(type, primitiveCount) == 0u || numVertices == 0u) {
            return S_FALSE;
        }
        if (minVertex > 0xffffffffu - numVertices) {
            return D3DERR_INVALIDCALL;
        }
        DWORD outputFvf = 0;
        FvfProcessLayout srcLayout{};
        FvfProcessLayout dstLayout{};
        HRESULT hr = describeSoftwareProgrammableDrawTarget(
            outputFvf, srcLayout, dstLayout);
        if (hr != S_OK) return hr;
        if (!processLayoutUsesOnlyStream0(srcLayout) || stride < srcLayout.stride) {
            return S_FALSE;
        }
        const UINT vertexCount = minVertex + numVertices;
        std::uint32_t inputBytes = 0;
        std::uint32_t outputBytes = 0;
        if (!checkedByteCount(vertexCount, stride, inputBytes) ||
            !checkedByteCount(vertexCount, dstLayout.stride, outputBytes) ||
            (inputBytes != 0u && !vertexData)) {
            return D3DERR_INVALIDCALL;
        }
        IDirect3DVertexBuffer9* srcBuffer = nullptr;
        IDirect3DVertexBuffer9* dstBuffer = nullptr;
        hr = CreateVertexBuffer(inputBytes, 0, fvf_, D3DPOOL_SYSTEMMEM,
                                &srcBuffer, nullptr);
        if (FAILED(hr)) return hr;
        void* mapped = nullptr;
        hr = srcBuffer->Lock(0, inputBytes, &mapped, 0);
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(mapped, vertexData, inputBytes);
            hr = srcBuffer->Unlock();
        } else if (SUCCEEDED(hr)) {
            hr = D3DERR_INVALIDCALL;
        }
        if (FAILED(hr)) {
            srcBuffer->Release();
            return hr;
        }
        hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                                &dstBuffer, nullptr);
        if (FAILED(hr)) {
            srcBuffer->Release();
            return hr;
        }

        IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
        if (savedStream0) savedStream0->AddRef();
        const UINT savedOffset0 = streamOff_[0];
        const UINT savedStride0 = streamStr_[0];
        setRef(streamSrc_[0], srcBuffer);
        streamOff_[0] = 0;
        streamStr_[0] = stride;
        hr = ProcessVertices(0, 0, vertexCount, dstBuffer, nullptr, 0);
        setRef(streamSrc_[0], savedStream0);
        streamOff_[0] = savedOffset0;
        streamStr_[0] = savedStride0;
        if (savedStream0) savedStream0->Release();

        if (SUCCEEDED(hr)) {
            hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                             outputFvf, dstLayout.stride);
            if (SUCCEEDED(hr)) {
                out.primitiveType = type;
                out.primitiveCount = primitiveCount;
                out.bypassVertexShader = true;
            }
        }
        dstBuffer->Release();
        srcBuffer->Release();
        return hr;
    }

    struct PePresentCadenceClaim {
        bool claimed = false;
        std::uint64_t ordinal = 0;
        std::int64_t returnNs = 0;
        std::int64_t entryNs = 0;
    };

    using PePresentCallSample = D3D9PePresentCallToken;

    PePresentCadenceClaim claimPeFirstCallAfterPresent() {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return {};
        }
        std::uint64_t ordinal =
            pePresentCadencePendingOrdinal_.load(std::memory_order_acquire);
        if (ordinal == 0) {
            return {};
        }
        const auto entry = std::chrono::steady_clock::now();
        if (!pePresentCadencePendingOrdinal_.compare_exchange_strong(
                ordinal, 0, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return {};
        }
        return PePresentCadenceClaim{
            true, ordinal,
            pePresentCadenceReturnNs_.load(std::memory_order_acquire),
            dxmt9SteadyClockNs(entry)};
    }

    void logPeFirstCallAfterPresent(const char* callName,
                                    const PePresentCadenceClaim& claim,
                                    const PePresentCallSample& sample) {
        if (!claim.claimed) {
            return;
        }
        const std::int64_t observedNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        const auto callerInfo = dxmt9PeResolveCallerModule(sample.callerPc);
        const auto callerStack = dxmt9PeFormatCallerStack(sample);
        dxmt9DeviceInfoLog(
            "pe_present_next_call device=%p ordinal=%llu call=%s "
            "thread_id=0x%lx "
            "entry_delta_ms=%.3f observed_delta_ms=%.3f "
            "observed_wait_ms=%.3f caller_pc=%p caller_module=%s "
            "caller_base=%p caller_rva=0x%llx caller_stack=%s",
            this, static_cast<unsigned long long>(claim.ordinal),
            callName ? callName : "unknown",
            static_cast<unsigned long>(sample.threadId),
            static_cast<double>(claim.entryNs - claim.returnNs) / 1000000.0,
            static_cast<double>(observedNs - claim.returnNs) / 1000000.0,
            static_cast<double>(observedNs - claim.entryNs) / 1000000.0,
            sample.callerPc, dxmt9PeCallerModuleLeaf(callerInfo),
            callerInfo.base, static_cast<unsigned long long>(callerInfo.rva),
            callerStack.data());
    }

    PePresentCallSample notePeDeviceCallAfterPresent(const char* callName,
                                                     const void* callerPc = nullptr) {
        dxmt9PeSetCurrentCallName(callName);
        dxmt9PeCurrentCallEntryNs = dxmt9PeRecorderStatsEnabled()
            ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
            : 0;
        recordPeBetweenCallsEntry(callName, dxmt9PeCurrentCallEntryNs,
                                  callerPc);
        const PePresentCallSample sample =
            logPeCallMilestoneAfterPresent(callName, callerPc);
        if (!sample.tracked && dxmt9PeRecorderStatsEnabled()) {
            PePresentCallSample untracked{};
            untracked.entryNs = dxmt9PeCurrentCallEntryNs;
            untracked.callerPc = callerPc;
            untracked.threadId = dxmt9PeCurrentThreadId();
            dxmt9PeCaptureCallStack(untracked);
            logPeFirstCallAfterPresent(callName, claimPeFirstCallAfterPresent(),
                                       untracked);
            return untracked;
        }
        logPeFirstCallAfterPresent(callName, claimPeFirstCallAfterPresent(),
                                   sample);
        return sample;
    }

    void markPePresentReturnedForCadence() {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t ordinal =
            pePresentCadenceOrdinal_.fetch_add(1, std::memory_order_relaxed) + 1;
        pePresentCadenceReturnNs_.store(dxmt9SteadyClockNs(now),
                                        std::memory_order_release);
        pePresentCadencePendingOrdinal_.store(ordinal, std::memory_order_release);
        pePresentCallCount_.store(0, std::memory_order_release);
        pePresentCallMilestoneMask_.store(0, std::memory_order_release);
        pePresentCallMilestonePendingOrdinal_.store(ordinal,
                                                   std::memory_order_release);
        pePresentChunkPendingOrdinal_.store(ordinal, std::memory_order_release);
        pePresentRecordMilestoneMask_.store(0, std::memory_order_release);
        pePresentRecordPendingOrdinal_.store(ordinal, std::memory_order_release);
    }

    static bool peCallMilestoneBit(std::uint32_t callCount,
                                   std::uint32_t& bit) noexcept {
        switch (callCount) {
        case 1: bit = 1u << 0; return true;
        case 2: bit = 1u << 1; return true;
        case 3: bit = 1u << 2; return true;
        case 4: bit = 1u << 3; return true;
        case 5: bit = 1u << 4; return true;
        case 6: bit = 1u << 5; return true;
        case 7: bit = 1u << 6; return true;
        case 8: bit = 1u << 7; return true;
        case 16: bit = 1u << 8; return true;
        case 32: bit = 1u << 9; return true;
        case 64: bit = 1u << 10; return true;
        default:
            bit = 0;
            return false;
        }
    }

    PePresentCallSample logPeCallMilestoneAfterPresent(const char* callName,
                                                       const void* callerPc) {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return {};
        }
        const std::uint64_t ordinal =
            pePresentCallMilestonePendingOrdinal_.load(std::memory_order_acquire);
        if (ordinal == 0) {
            return {};
        }
        const auto entry = std::chrono::steady_clock::now();
        const std::int64_t entryNs = dxmt9SteadyClockNs(entry);
        const std::uint32_t callCount =
            pePresentCallCount_.fetch_add(1, std::memory_order_acq_rel) + 1;
        const std::int64_t returnNs =
            pePresentCadenceReturnNs_.load(std::memory_order_acquire);
        std::uint32_t milestoneBit = 0;
        const bool milestone = peCallMilestoneBit(callCount, milestoneBit);
        PePresentCallSample sample{
            true, ordinal, callCount, returnNs, entryNs, callerPc};
        sample.threadId = dxmt9PeCurrentThreadId();
        if (callCount <= 8 || milestone) {
            dxmt9PeCaptureCallStack(sample);
        }
        if (!milestone) {
            return sample;
        }
        std::uint32_t mask =
            pePresentCallMilestoneMask_.load(std::memory_order_acquire);
        while ((mask & milestoneBit) == 0) {
            if (pePresentCallMilestoneMask_.compare_exchange_weak(
                    mask, mask | milestoneBit, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                const auto callerInfo = dxmt9PeResolveCallerModule(callerPc);
                const auto callerStack = dxmt9PeFormatCallerStack(sample);
                dxmt9DeviceInfoLog(
                    "pe_present_call_milestone device=%p ordinal=%llu "
                    "milestone=%u call=%s thread_id=0x%lx "
                    "entry_delta_ms=%.3f caller_pc=%p "
                    "caller_module=%s caller_base=%p caller_rva=0x%llx "
                    "caller_stack=%s",
                    this, static_cast<unsigned long long>(ordinal), callCount,
                    callName ? callName : "unknown",
                    static_cast<unsigned long>(sample.threadId),
                    static_cast<double>(entryNs - returnNs) / 1000000.0,
                    callerPc, dxmt9PeCallerModuleLeaf(callerInfo),
                    callerInfo.base,
                    static_cast<unsigned long long>(callerInfo.rva),
                    callerStack.data());
                return sample;
            }
        }
        return sample;
    }

    void logPeCallReturnAfterPresent(const PePresentCallSample& sample,
                                     const char* callName,
                                     HRESULT hr) {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return;
        }
        const std::int64_t exitNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        if (!sample.tracked) {
            if (hr != D3DERR_INVALIDCALL) {
                return;
            }
            if (sample.entryNs != 0) {
                recordPeBetweenCallsReturn(callName, sample.entryNs, exitNs);
            }
            const auto callerInfo = dxmt9PeResolveCallerModule(sample.callerPc);
            const auto callerStack = dxmt9PeFormatCallerStack(sample);
            dxmt9DeviceInfoLog(
                "pe_call_return_untracked_failure device=%p call=%s "
                "thread_id=0x%lx hr=0x%08x duration_ms=%.3f "
                "caller_pc=%p caller_module=%s caller_base=%p "
                "caller_rva=0x%llx caller_stack=%s",
                this, callName ? callName : "unknown",
                static_cast<unsigned long>(sample.threadId),
                static_cast<unsigned>(hr),
                sample.entryNs != 0
                    ? static_cast<double>(exitNs - sample.entryNs) / 1000000.0
                    : 0.0,
                sample.callerPc, dxmt9PeCallerModuleLeaf(callerInfo),
                callerInfo.base,
                static_cast<unsigned long long>(callerInfo.rva),
                callerStack.data());
            return;
        }
        recordPeBetweenCallsReturn(callName, sample.entryNs, exitNs);
        if (sample.callCount > 8 && SUCCEEDED(hr)) {
            return;
        }
        const auto callerInfo = dxmt9PeResolveCallerModule(sample.callerPc);
        const auto callerStack = dxmt9PeFormatCallerStack(sample);
        dxmt9DeviceInfoLog(
            "pe_present_call_return device=%p ordinal=%llu milestone=%u "
            "call=%s thread_id=0x%lx hr=0x%08x "
            "return_delta_ms=%.3f duration_ms=%.3f "
            "caller_pc=%p caller_module=%s caller_base=%p caller_rva=0x%llx "
            "caller_stack=%s",
            this, static_cast<unsigned long long>(sample.ordinal),
            sample.callCount, callName ? callName : "unknown",
            static_cast<unsigned long>(sample.threadId),
            static_cast<unsigned>(hr),
            static_cast<double>(exitNs - sample.returnNs) / 1000000.0,
            static_cast<double>(exitNs - sample.entryNs) / 1000000.0,
            sample.callerPc, dxmt9PeCallerModuleLeaf(callerInfo),
            callerInfo.base,
            static_cast<unsigned long long>(callerInfo.rva),
            callerStack.data());
    }

    static const char* peCommandRecordTypeName(std::uint32_t type) noexcept {
        switch (type) {
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
            return "draw";
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
            return "draw_indexed";
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
            return "draw_up";
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
            return "draw_indexed_up";
        case D9C_COMMAND_RECORD_SET_VS_CONST_F:
            return "set_vs_const_f";
        case D9C_COMMAND_RECORD_SET_VS_CONST_I:
            return "set_vs_const_i";
        case D9C_COMMAND_RECORD_SET_VS_CONST_B:
            return "set_vs_const_b";
        case D9C_COMMAND_RECORD_SET_PS_CONST_F:
            return "set_ps_const_f";
        case D9C_COMMAND_RECORD_SET_PS_CONST_I:
            return "set_ps_const_i";
        case D9C_COMMAND_RECORD_SET_PS_CONST_B:
            return "set_ps_const_b";
        case D9C_COMMAND_RECORD_CLEAR:
            return "clear";
        case D9C_COMMAND_RECORD_PRESENT:
            return "present";
        case D9C_COMMAND_RECORD_STRETCH_RECT:
            return "stretch_rect";
        case D9C_COMMAND_RECORD_COLOR_FILL:
            return "color_fill";
        case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
            return "update_texture";
        case D9C_COMMAND_RECORD_UPDATE_SURFACE:
            return "update_surface";
        case D9C_COMMAND_RECORD_QUERY_ISSUE:
            return "query_issue";
        case D9C_COMMAND_RECORD_READBACK:
            return "readback";
        case D9C_COMMAND_RECORD_APPLY_STATE:
            return "apply_state";
        case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
            return "resz_depth_resolve";
        default:
            return "unknown";
        }
    }

    static std::uint32_t peCommandRecordTypeBucket(std::uint32_t type) noexcept {
        return type < kPeCommandRecordTypeBucketCount ? type : 0u;
    }

    static const char*
    peInterAppendCallFamilyName(std::uint32_t family) noexcept {
        switch (static_cast<PeInterAppendCallFamily>(family)) {
        case PeInterAppendCallFamily::Unknown: return "unknown";
        case PeInterAppendCallFamily::RenderTarget: return "render_target";
        case PeInterAppendCallFamily::DepthStencil: return "depth_stencil";
        case PeInterAppendCallFamily::ViewportScissor:
            return "viewport_scissor";
        case PeInterAppendCallFamily::Transform: return "transform";
        case PeInterAppendCallFamily::MaterialLightClip:
            return "material_light_clip";
        case PeInterAppendCallFamily::RenderState: return "render_state";
        case PeInterAppendCallFamily::TextureStageSampler:
            return "tss_sampler";
        case PeInterAppendCallFamily::Texture: return "texture";
        case PeInterAppendCallFamily::VertexInput: return "vertex_input";
        case PeInterAppendCallFamily::Shader: return "shader";
        case PeInterAppendCallFamily::VsConst: return "vs_const";
        case PeInterAppendCallFamily::PsConst: return "ps_const";
        case PeInterAppendCallFamily::OtherConst: return "other_const";
        case PeInterAppendCallFamily::Draw: return "draw";
        case PeInterAppendCallFamily::Barrier: return "barrier";
        case PeInterAppendCallFamily::ScenePresent: return "scene_present";
        case PeInterAppendCallFamily::Resource: return "resource";
        case PeInterAppendCallFamily::Count: break;
        }
        return "unknown";
    }

    static PeInterAppendCallFamily
    peInterAppendCallFamilyFromName(const char* callName) noexcept {
        if (!callName) {
            return PeInterAppendCallFamily::Unknown;
        }
        if (std::strcmp(callName, "SetRenderTarget") == 0 ||
            std::strcmp(callName, "GetRenderTarget") == 0) {
            return PeInterAppendCallFamily::RenderTarget;
        }
        if (std::strcmp(callName, "SetDepthStencilSurface") == 0 ||
            std::strcmp(callName, "GetDepthStencilSurface") == 0) {
            return PeInterAppendCallFamily::DepthStencil;
        }
        if (std::strcmp(callName, "SetViewport") == 0 ||
            std::strcmp(callName, "GetViewport") == 0 ||
            std::strcmp(callName, "SetScissorRect") == 0 ||
            std::strcmp(callName, "GetScissorRect") == 0) {
            return PeInterAppendCallFamily::ViewportScissor;
        }
        if (std::strcmp(callName, "SetTransform") == 0 ||
            std::strcmp(callName, "GetTransform") == 0 ||
            std::strcmp(callName, "MultiplyTransform") == 0) {
            return PeInterAppendCallFamily::Transform;
        }
        if (std::strcmp(callName, "SetMaterial") == 0 ||
            std::strcmp(callName, "GetMaterial") == 0 ||
            std::strcmp(callName, "SetLight") == 0 ||
            std::strcmp(callName, "GetLight") == 0 ||
            std::strcmp(callName, "LightEnable") == 0 ||
            std::strcmp(callName, "GetLightEnable") == 0 ||
            std::strcmp(callName, "SetClipPlane") == 0 ||
            std::strcmp(callName, "GetClipPlane") == 0 ||
            std::strcmp(callName, "SetClipStatus") == 0 ||
            std::strcmp(callName, "GetClipStatus") == 0) {
            return PeInterAppendCallFamily::MaterialLightClip;
        }
        if (std::strcmp(callName, "SetRenderState") == 0 ||
            std::strcmp(callName, "GetRenderState") == 0) {
            return PeInterAppendCallFamily::RenderState;
        }
        if (std::strcmp(callName, "SetTextureStageState") == 0 ||
            std::strcmp(callName, "GetTextureStageState") == 0 ||
            std::strcmp(callName, "SetSamplerState") == 0 ||
            std::strcmp(callName, "GetSamplerState") == 0) {
            return PeInterAppendCallFamily::TextureStageSampler;
        }
        if (std::strcmp(callName, "SetTexture") == 0 ||
            std::strcmp(callName, "GetTexture") == 0) {
            return PeInterAppendCallFamily::Texture;
        }
        if (std::strcmp(callName, "SetFVF") == 0 ||
            std::strcmp(callName, "GetFVF") == 0 ||
            std::strcmp(callName, "SetVertexDeclaration") == 0 ||
            std::strcmp(callName, "GetVertexDeclaration") == 0 ||
            std::strcmp(callName, "SetStreamSource") == 0 ||
            std::strcmp(callName, "GetStreamSource") == 0 ||
            std::strcmp(callName, "SetStreamSourceFreq") == 0 ||
            std::strcmp(callName, "GetStreamSourceFreq") == 0 ||
            std::strcmp(callName, "SetIndices") == 0 ||
            std::strcmp(callName, "GetIndices") == 0) {
            return PeInterAppendCallFamily::VertexInput;
        }
        if (std::strcmp(callName, "SetVertexShader") == 0 ||
            std::strcmp(callName, "GetVertexShader") == 0 ||
            std::strcmp(callName, "SetPixelShader") == 0 ||
            std::strcmp(callName, "GetPixelShader") == 0) {
            return PeInterAppendCallFamily::Shader;
        }
        if (std::strcmp(callName, "SetVertexShaderConstantF") == 0 ||
            std::strcmp(callName, "GetVertexShaderConstantF") == 0 ||
            std::strcmp(callName, "SetVertexShaderConstantI") == 0 ||
            std::strcmp(callName, "GetVertexShaderConstantI") == 0 ||
            std::strcmp(callName, "SetVertexShaderConstantB") == 0 ||
            std::strcmp(callName, "GetVertexShaderConstantB") == 0) {
            return PeInterAppendCallFamily::VsConst;
        }
        if (std::strcmp(callName, "SetPixelShaderConstantF") == 0 ||
            std::strcmp(callName, "GetPixelShaderConstantF") == 0 ||
            std::strcmp(callName, "SetPixelShaderConstantI") == 0 ||
            std::strcmp(callName, "GetPixelShaderConstantI") == 0 ||
            std::strcmp(callName, "SetPixelShaderConstantB") == 0 ||
            std::strcmp(callName, "GetPixelShaderConstantB") == 0) {
            return PeInterAppendCallFamily::PsConst;
        }
        if (std::strcmp(callName, "DrawPrimitive") == 0 ||
            std::strcmp(callName, "DrawIndexedPrimitive") == 0 ||
            std::strcmp(callName, "DrawPrimitiveUP") == 0 ||
            std::strcmp(callName, "DrawIndexedPrimitiveUP") == 0) {
            return PeInterAppendCallFamily::Draw;
        }
        if (std::strcmp(callName, "Clear") == 0 ||
            std::strcmp(callName, "StretchRect") == 0 ||
            std::strcmp(callName, "ColorFill") == 0 ||
            std::strcmp(callName, "UpdateTexture") == 0 ||
            std::strcmp(callName, "UpdateSurface") == 0 ||
            std::strcmp(callName, "ProcessVertices") == 0) {
            return PeInterAppendCallFamily::Barrier;
        }
        if (std::strcmp(callName, "BeginScene") == 0 ||
            std::strcmp(callName, "EndScene") == 0 ||
            std::strcmp(callName, "Present") == 0 ||
            std::strcmp(callName, "PresentEx") == 0 ||
            std::strcmp(callName, "Reset") == 0 ||
            std::strcmp(callName, "ResetEx") == 0) {
            return PeInterAppendCallFamily::ScenePresent;
        }
        if (std::strncmp(callName, "Create", 6) == 0 ||
            std::strncmp(callName, "Get", 3) == 0 ||
            std::strcmp(callName, "ValidateDevice") == 0 ||
            std::strcmp(callName, "SetPaletteEntries") == 0 ||
            std::strcmp(callName, "GetPaletteEntries") == 0 ||
            std::strcmp(callName, "SetCurrentTexturePalette") == 0 ||
            std::strcmp(callName, "GetCurrentTexturePalette") == 0) {
            return PeInterAppendCallFamily::Resource;
        }
        return PeInterAppendCallFamily::Unknown;
    }

    static const char*
    peInterAppendCallNameName(std::uint32_t callName) noexcept {
        switch (static_cast<PeInterAppendCallName>(callName)) {
        case PeInterAppendCallName::Unknown: return "unknown";
        case PeInterAppendCallName::BeginScene: return "BeginScene";
        case PeInterAppendCallName::EndScene: return "EndScene";
        case PeInterAppendCallName::Clear: return "Clear";
        case PeInterAppendCallName::SetRenderTarget: return "SetRenderTarget";
        case PeInterAppendCallName::GetRenderTarget: return "GetRenderTarget";
        case PeInterAppendCallName::SetDepthStencilSurface:
            return "SetDepthStencilSurface";
        case PeInterAppendCallName::GetDepthStencilSurface:
            return "GetDepthStencilSurface";
        case PeInterAppendCallName::SetViewport: return "SetViewport";
        case PeInterAppendCallName::GetViewport: return "GetViewport";
        case PeInterAppendCallName::SetScissorRect: return "SetScissorRect";
        case PeInterAppendCallName::GetScissorRect: return "GetScissorRect";
        case PeInterAppendCallName::SetRenderState: return "SetRenderState";
        case PeInterAppendCallName::SetTextureStageState:
            return "SetTextureStageState";
        case PeInterAppendCallName::SetSamplerState: return "SetSamplerState";
        case PeInterAppendCallName::SetTexture: return "SetTexture";
        case PeInterAppendCallName::SetFVF: return "SetFVF";
        case PeInterAppendCallName::SetVertexDeclaration:
            return "SetVertexDeclaration";
        case PeInterAppendCallName::SetStreamSource:
            return "SetStreamSource";
        case PeInterAppendCallName::SetStreamSourceFreq:
            return "SetStreamSourceFreq";
        case PeInterAppendCallName::SetIndices: return "SetIndices";
        case PeInterAppendCallName::SetVertexShader:
            return "SetVertexShader";
        case PeInterAppendCallName::SetPixelShader: return "SetPixelShader";
        case PeInterAppendCallName::SetVertexShaderConstantF:
            return "SetVertexShaderConstantF";
        case PeInterAppendCallName::SetVertexShaderConstantI:
            return "SetVertexShaderConstantI";
        case PeInterAppendCallName::SetVertexShaderConstantB:
            return "SetVertexShaderConstantB";
        case PeInterAppendCallName::SetPixelShaderConstantF:
            return "SetPixelShaderConstantF";
        case PeInterAppendCallName::SetPixelShaderConstantI:
            return "SetPixelShaderConstantI";
        case PeInterAppendCallName::SetPixelShaderConstantB:
            return "SetPixelShaderConstantB";
        case PeInterAppendCallName::DrawPrimitive: return "DrawPrimitive";
        case PeInterAppendCallName::DrawIndexedPrimitive:
            return "DrawIndexedPrimitive";
        case PeInterAppendCallName::DrawPrimitiveUP:
            return "DrawPrimitiveUP";
        case PeInterAppendCallName::DrawIndexedPrimitiveUP:
            return "DrawIndexedPrimitiveUP";
        case PeInterAppendCallName::ProcessVertices: return "ProcessVertices";
        case PeInterAppendCallName::GetBackBuffer: return "GetBackBuffer";
        case PeInterAppendCallName::GetSwapChain: return "GetSwapChain";
        case PeInterAppendCallName::GetRasterStatus: return "GetRasterStatus";
        case PeInterAppendCallName::ValidateDevice: return "ValidateDevice";
        case PeInterAppendCallName::SetSoftwareVertexProcessing:
            return "SetSoftwareVertexProcessing";
        case PeInterAppendCallName::SetNPatchMode: return "SetNPatchMode";
        case PeInterAppendCallName::SurfaceGetDesc:
            return "Surface::GetDesc";
        case PeInterAppendCallName::SurfaceLockRect:
            return "Surface::LockRect";
        case PeInterAppendCallName::TextureGetSurfaceLevel:
            return "Texture::GetSurfaceLevel";
        case PeInterAppendCallName::CubeTextureGetCubeMapSurface:
            return "CubeTexture::GetCubeMapSurface";
        case PeInterAppendCallName::VertexBufferLock:
            return "VertexBuffer::Lock";
        case PeInterAppendCallName::VertexBufferGetDesc:
            return "VertexBuffer::GetDesc";
        case PeInterAppendCallName::IndexBufferLock:
            return "IndexBuffer::Lock";
        case PeInterAppendCallName::IndexBufferGetDesc:
            return "IndexBuffer::GetDesc";
        case PeInterAppendCallName::QueryIssue: return "Query::Issue";
        case PeInterAppendCallName::QueryGetData: return "Query::GetData";
        case PeInterAppendCallName::StateBlockCapture:
            return "StateBlock::Capture";
        case PeInterAppendCallName::StateBlockApply:
            return "StateBlock::Apply";
        case PeInterAppendCallName::OtherChild: return "other_child";
        case PeInterAppendCallName::OtherGet: return "other_get";
        case PeInterAppendCallName::OtherSet: return "other_set";
        case PeInterAppendCallName::OtherCreate: return "other_create";
        case PeInterAppendCallName::Other: return "other";
        case PeInterAppendCallName::Count: break;
        }
        return "unknown";
    }

    static PeInterAppendCallName
    peInterAppendCallNameFromName(const char* callName) noexcept {
        if (!callName) {
            return PeInterAppendCallName::Unknown;
        }
        if (std::strcmp(callName, "BeginScene") == 0) {
            return PeInterAppendCallName::BeginScene;
        }
        if (std::strcmp(callName, "EndScene") == 0) {
            return PeInterAppendCallName::EndScene;
        }
        if (std::strcmp(callName, "Clear") == 0) {
            return PeInterAppendCallName::Clear;
        }
        if (std::strcmp(callName, "SetRenderTarget") == 0) {
            return PeInterAppendCallName::SetRenderTarget;
        }
        if (std::strcmp(callName, "GetRenderTarget") == 0) {
            return PeInterAppendCallName::GetRenderTarget;
        }
        if (std::strcmp(callName, "SetDepthStencilSurface") == 0) {
            return PeInterAppendCallName::SetDepthStencilSurface;
        }
        if (std::strcmp(callName, "GetDepthStencilSurface") == 0) {
            return PeInterAppendCallName::GetDepthStencilSurface;
        }
        if (std::strcmp(callName, "SetViewport") == 0) {
            return PeInterAppendCallName::SetViewport;
        }
        if (std::strcmp(callName, "GetViewport") == 0) {
            return PeInterAppendCallName::GetViewport;
        }
        if (std::strcmp(callName, "SetScissorRect") == 0) {
            return PeInterAppendCallName::SetScissorRect;
        }
        if (std::strcmp(callName, "GetScissorRect") == 0) {
            return PeInterAppendCallName::GetScissorRect;
        }
        if (std::strcmp(callName, "SetRenderState") == 0) {
            return PeInterAppendCallName::SetRenderState;
        }
        if (std::strcmp(callName, "SetTextureStageState") == 0) {
            return PeInterAppendCallName::SetTextureStageState;
        }
        if (std::strcmp(callName, "SetSamplerState") == 0) {
            return PeInterAppendCallName::SetSamplerState;
        }
        if (std::strcmp(callName, "SetTexture") == 0) {
            return PeInterAppendCallName::SetTexture;
        }
        if (std::strcmp(callName, "SetFVF") == 0) {
            return PeInterAppendCallName::SetFVF;
        }
        if (std::strcmp(callName, "SetVertexDeclaration") == 0) {
            return PeInterAppendCallName::SetVertexDeclaration;
        }
        if (std::strcmp(callName, "SetStreamSource") == 0) {
            return PeInterAppendCallName::SetStreamSource;
        }
        if (std::strcmp(callName, "SetStreamSourceFreq") == 0) {
            return PeInterAppendCallName::SetStreamSourceFreq;
        }
        if (std::strcmp(callName, "SetIndices") == 0) {
            return PeInterAppendCallName::SetIndices;
        }
        if (std::strcmp(callName, "SetVertexShader") == 0) {
            return PeInterAppendCallName::SetVertexShader;
        }
        if (std::strcmp(callName, "SetPixelShader") == 0) {
            return PeInterAppendCallName::SetPixelShader;
        }
        if (std::strcmp(callName, "SetVertexShaderConstantF") == 0) {
            return PeInterAppendCallName::SetVertexShaderConstantF;
        }
        if (std::strcmp(callName, "SetVertexShaderConstantI") == 0) {
            return PeInterAppendCallName::SetVertexShaderConstantI;
        }
        if (std::strcmp(callName, "SetVertexShaderConstantB") == 0) {
            return PeInterAppendCallName::SetVertexShaderConstantB;
        }
        if (std::strcmp(callName, "SetPixelShaderConstantF") == 0) {
            return PeInterAppendCallName::SetPixelShaderConstantF;
        }
        if (std::strcmp(callName, "SetPixelShaderConstantI") == 0) {
            return PeInterAppendCallName::SetPixelShaderConstantI;
        }
        if (std::strcmp(callName, "SetPixelShaderConstantB") == 0) {
            return PeInterAppendCallName::SetPixelShaderConstantB;
        }
        if (std::strcmp(callName, "DrawPrimitive") == 0) {
            return PeInterAppendCallName::DrawPrimitive;
        }
        if (std::strcmp(callName, "DrawIndexedPrimitive") == 0) {
            return PeInterAppendCallName::DrawIndexedPrimitive;
        }
        if (std::strcmp(callName, "DrawPrimitiveUP") == 0) {
            return PeInterAppendCallName::DrawPrimitiveUP;
        }
        if (std::strcmp(callName, "DrawIndexedPrimitiveUP") == 0) {
            return PeInterAppendCallName::DrawIndexedPrimitiveUP;
        }
        if (std::strcmp(callName, "ProcessVertices") == 0) {
            return PeInterAppendCallName::ProcessVertices;
        }
        if (std::strcmp(callName, "GetBackBuffer") == 0) {
            return PeInterAppendCallName::GetBackBuffer;
        }
        if (std::strcmp(callName, "GetSwapChain") == 0) {
            return PeInterAppendCallName::GetSwapChain;
        }
        if (std::strcmp(callName, "GetRasterStatus") == 0) {
            return PeInterAppendCallName::GetRasterStatus;
        }
        if (std::strcmp(callName, "ValidateDevice") == 0) {
            return PeInterAppendCallName::ValidateDevice;
        }
        if (std::strcmp(callName, "SetSoftwareVertexProcessing") == 0) {
            return PeInterAppendCallName::SetSoftwareVertexProcessing;
        }
        if (std::strcmp(callName, "SetNPatchMode") == 0) {
            return PeInterAppendCallName::SetNPatchMode;
        }
        if (std::strcmp(callName, "Surface::GetDesc") == 0) {
            return PeInterAppendCallName::SurfaceGetDesc;
        }
        if (std::strcmp(callName, "Surface::LockRect") == 0) {
            return PeInterAppendCallName::SurfaceLockRect;
        }
        if (std::strcmp(callName, "Texture::GetSurfaceLevel") == 0) {
            return PeInterAppendCallName::TextureGetSurfaceLevel;
        }
        if (std::strcmp(callName, "CubeTexture::GetCubeMapSurface") == 0) {
            return PeInterAppendCallName::CubeTextureGetCubeMapSurface;
        }
        if (std::strcmp(callName, "VertexBuffer::Lock") == 0) {
            return PeInterAppendCallName::VertexBufferLock;
        }
        if (std::strcmp(callName, "VertexBuffer::GetDesc") == 0) {
            return PeInterAppendCallName::VertexBufferGetDesc;
        }
        if (std::strcmp(callName, "IndexBuffer::Lock") == 0) {
            return PeInterAppendCallName::IndexBufferLock;
        }
        if (std::strcmp(callName, "IndexBuffer::GetDesc") == 0) {
            return PeInterAppendCallName::IndexBufferGetDesc;
        }
        if (std::strcmp(callName, "Query::Issue") == 0) {
            return PeInterAppendCallName::QueryIssue;
        }
        if (std::strcmp(callName, "Query::GetData") == 0) {
            return PeInterAppendCallName::QueryGetData;
        }
        if (std::strcmp(callName, "StateBlock::Capture") == 0) {
            return PeInterAppendCallName::StateBlockCapture;
        }
        if (std::strcmp(callName, "StateBlock::Apply") == 0) {
            return PeInterAppendCallName::StateBlockApply;
        }
        if (std::strstr(callName, "::") != nullptr) {
            return PeInterAppendCallName::OtherChild;
        }
        if (std::strncmp(callName, "Get", 3) == 0) {
            return PeInterAppendCallName::OtherGet;
        }
        if (std::strncmp(callName, "Set", 3) == 0) {
            return PeInterAppendCallName::OtherSet;
        }
        if (std::strncmp(callName, "Create", 6) == 0) {
            return PeInterAppendCallName::OtherCreate;
        }
        return PeInterAppendCallName::Other;
    }

    static std::size_t peInterAppendFocusPairIndex(std::uint32_t prevType,
                                                   std::uint32_t nextType) noexcept {
        if (prevType != D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
            return kPeInterAppendFocusPairCount;
        }
        switch (nextType) {
        case D9C_COMMAND_RECORD_SET_VS_CONST_F:
            return static_cast<std::size_t>(
                PeInterAppendFocusPair::DrawIndexedToVsConstF);
        case D9C_COMMAND_RECORD_APPLY_STATE:
            return static_cast<std::size_t>(
                PeInterAppendFocusPair::DrawIndexedToApplyState);
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
            return static_cast<std::size_t>(
                PeInterAppendFocusPair::DrawIndexedToDrawIndexed);
        case D9C_COMMAND_RECORD_SET_PS_CONST_F:
            return static_cast<std::size_t>(
                PeInterAppendFocusPair::DrawIndexedToPsConstF);
        default:
            return kPeInterAppendFocusPairCount;
        }
    }

    static std::size_t peInterAppendFocusCallFamilyIndex(
        std::size_t focusPair,
        PeInterAppendCallFamily family) noexcept {
        return focusPair * kPeInterAppendCallFamilyCount +
               static_cast<std::size_t>(family);
    }

    static std::size_t peInterAppendFocusCallNameIndex(
        std::size_t focusPair,
        PeInterAppendCallName callName) noexcept {
        return focusPair * kPeInterAppendCallNameCount +
               static_cast<std::size_t>(callName);
    }

    static std::size_t peInterAppendFocusCallTransitionIndex(
        std::size_t focusPair,
        PeInterAppendCallFamily prevFamily,
        PeInterAppendCallFamily nextFamily) noexcept {
        return (focusPair * kPeInterAppendCallFamilyCount +
                static_cast<std::size_t>(prevFamily)) *
                   kPeInterAppendCallFamilyCount +
               static_cast<std::size_t>(nextFamily);
    }

    static std::size_t peInterAppendFocusCallNameTransitionIndex(
        std::size_t focusPair,
        PeInterAppendCallName prevCallName,
        PeInterAppendCallName nextCallName) noexcept {
        return (focusPair * kPeInterAppendCallNameCount +
                static_cast<std::size_t>(prevCallName)) *
                   kPeInterAppendCallNameCount +
               static_cast<std::size_t>(nextCallName);
    }

    static std::size_t peInterAppendCallTransitionIndex(
        PeInterAppendCallFamily prevFamily,
        PeInterAppendCallFamily nextFamily) noexcept {
        return static_cast<std::size_t>(prevFamily) *
                   kPeInterAppendCallFamilyCount +
               static_cast<std::size_t>(nextFamily);
    }

    static std::size_t peInterAppendCallNameTransitionIndex(
        PeInterAppendCallName prevCallName,
        PeInterAppendCallName nextCallName) noexcept {
        return static_cast<std::size_t>(prevCallName) *
                   kPeInterAppendCallNameCount +
               static_cast<std::size_t>(nextCallName);
    }

    static std::size_t peInterAppendPairIndex(std::uint32_t prevType,
                                              std::uint32_t nextType) noexcept {
        return static_cast<std::size_t>(
                   peCommandRecordTypeBucket(prevType)) *
                   kPeCommandRecordTypeBucketCount +
               peCommandRecordTypeBucket(nextType);
    }

    struct PeInterAppendPairSummary {
        std::uint32_t prevType = 0;
        std::uint32_t nextType = 0;
        std::uint64_t samples = 0;
        std::uint64_t totalNs = 0;
        std::uint64_t maxNs = 0;
    };

    struct PeInterAppendCallFamilySummary {
        std::uint32_t family = 0;
        std::uint64_t samples = 0;
        std::uint64_t totalNs = 0;
        std::uint64_t maxNs = 0;
    };

    struct PeInterAppendCallNameSummary {
        std::uint32_t callName = 0;
        std::uint64_t samples = 0;
        std::uint64_t totalNs = 0;
        std::uint64_t maxNs = 0;
    };

    struct PeInterAppendCallTransitionSummary {
        std::uint32_t prevFamily = 0;
        std::uint32_t nextFamily = 0;
        std::uint64_t samples = 0;
        std::uint64_t totalNs = 0;
        std::uint64_t maxNs = 0;
    };

    struct PeInterAppendCallNameTransitionSummary {
        std::uint32_t prevCallName = 0;
        std::uint32_t nextCallName = 0;
        std::uint64_t samples = 0;
        std::uint64_t totalNs = 0;
        std::uint64_t maxNs = 0;
    };

    std::array<PeInterAppendPairSummary, kPeRecorderInterAppendTopPairCount>
    topPeInterAppendPairs() const noexcept {
        std::array<PeInterAppendPairSummary, kPeRecorderInterAppendTopPairCount>
            top{};
        for (std::size_t prev = 0; prev < kPeCommandRecordTypeBucketCount;
             ++prev) {
            for (std::size_t next = 0; next < kPeCommandRecordTypeBucketCount;
                 ++next) {
                const std::size_t index =
                    prev * kPeCommandRecordTypeBucketCount + next;
                const std::uint64_t totalNs =
                    peRecorderStats_.chunkInterAppendPairNsTotal[index];
                if (totalNs == 0) {
                    continue;
                }
                PeInterAppendPairSummary candidate{
                    static_cast<std::uint32_t>(prev),
                    static_cast<std::uint32_t>(next),
                    peRecorderStats_.chunkInterAppendPairSamples[index],
                    totalNs,
                    peRecorderStats_.chunkInterAppendPairNsMax[index]};
                for (auto& slot : top) {
                    if (candidate.totalNs <= slot.totalNs) {
                        continue;
                    }
                    std::swap(candidate, slot);
                }
            }
        }
        return top;
    }

    std::array<PeInterAppendCallFamilySummary,
               kPeRecorderInterAppendTopCallFamilyCount>
    topPeInterAppendFocusCallFamilies(std::size_t focusPair) const noexcept {
        std::array<PeInterAppendCallFamilySummary,
                   kPeRecorderInterAppendTopCallFamilyCount>
            top{};
        if (focusPair >= kPeInterAppendFocusPairCount) {
            return top;
        }
        for (std::size_t family = 0; family < kPeInterAppendCallFamilyCount;
             ++family) {
            const std::size_t index =
                focusPair * kPeInterAppendCallFamilyCount + family;
            const std::uint64_t totalNs =
                peRecorderStats_
                    .chunkInterAppendFocusCallFamilyNsTotal[index];
            if (totalNs == 0) {
                continue;
            }
            PeInterAppendCallFamilySummary candidate{
                static_cast<std::uint32_t>(family),
                peRecorderStats_
                    .chunkInterAppendFocusCallFamilySamples[index],
                totalNs,
                peRecorderStats_
                    .chunkInterAppendFocusCallFamilyNsMax[index]};
            for (auto& slot : top) {
                if (candidate.totalNs <= slot.totalNs) {
                    continue;
                }
                std::swap(candidate, slot);
            }
        }
        return top;
    }

    std::array<PeInterAppendCallFamilySummary,
               kPeRecorderInterAppendTopCallFamilyCount>
    topPeInterAppendFocusBetweenCallFamilies(std::size_t focusPair) const noexcept {
        std::array<PeInterAppendCallFamilySummary,
                   kPeRecorderInterAppendTopCallFamilyCount>
            top{};
        if (focusPair >= kPeInterAppendFocusPairCount) {
            return top;
        }
        for (std::size_t family = 0; family < kPeInterAppendCallFamilyCount;
             ++family) {
            const std::size_t index =
                focusPair * kPeInterAppendCallFamilyCount + family;
            const std::uint64_t samples =
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallFamilySamples[index];
            if (samples == 0) {
                continue;
            }
            PeInterAppendCallFamilySummary candidate{
                static_cast<std::uint32_t>(family), samples, samples, 0};
            for (auto& slot : top) {
                if (candidate.samples <= slot.samples) {
                    continue;
                }
                std::swap(candidate, slot);
            }
        }
        return top;
    }

    std::array<PeInterAppendCallNameSummary,
               kPeRecorderInterAppendTopCallNameCount>
    topPeInterAppendFocusBetweenCallNames(std::size_t focusPair) const noexcept {
        std::array<PeInterAppendCallNameSummary,
                   kPeRecorderInterAppendTopCallNameCount>
            top{};
        if (focusPair >= kPeInterAppendFocusPairCount) {
            return top;
        }
        for (std::size_t callName = 0; callName < kPeInterAppendCallNameCount;
             ++callName) {
            const std::size_t index =
                focusPair * kPeInterAppendCallNameCount + callName;
            const std::uint64_t samples =
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameSamples[index];
            if (samples == 0) {
                continue;
            }
            const auto cpuNs =
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameCpuNsTotal[index];
            PeInterAppendCallNameSummary candidate{
                static_cast<std::uint32_t>(callName), samples, cpuNs,
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameCpuNsMax[index]};
            for (auto& slot : top) {
                if (candidate.samples <= slot.samples) {
                    continue;
                }
                std::swap(candidate, slot);
            }
        }
        return top;
    }

    std::array<PeInterAppendCallTransitionSummary,
               kPeRecorderInterAppendTopCallTransitionCount>
    topPeInterAppendFocusBetweenCallTransitions(
        std::size_t focusPair) const noexcept {
        std::array<PeInterAppendCallTransitionSummary,
                   kPeRecorderInterAppendTopCallTransitionCount>
            top{};
        if (focusPair >= kPeInterAppendFocusPairCount) {
            return top;
        }
        for (std::size_t prevFamily = 0;
             prevFamily < kPeInterAppendCallFamilyCount; ++prevFamily) {
            for (std::size_t nextFamily = 0;
                 nextFamily < kPeInterAppendCallFamilyCount; ++nextFamily) {
                const std::size_t index =
                    peInterAppendFocusCallTransitionIndex(
                        focusPair,
                        static_cast<PeInterAppendCallFamily>(prevFamily),
                        static_cast<PeInterAppendCallFamily>(nextFamily));
                const std::uint64_t totalNs =
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallTransitionNsTotal[
                            index];
                if (totalNs == 0) {
                    continue;
                }
                PeInterAppendCallTransitionSummary candidate{
                    static_cast<std::uint32_t>(prevFamily),
                    static_cast<std::uint32_t>(nextFamily),
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallTransitionSamples[
                            index],
                    totalNs,
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallTransitionNsMax[
                            index]};
                for (auto& slot : top) {
                    if (candidate.totalNs <= slot.totalNs) {
                        continue;
                    }
                    std::swap(candidate, slot);
                }
            }
        }
        return top;
    }

    std::array<PeInterAppendCallNameTransitionSummary,
               kPeRecorderInterAppendTopCallTransitionCount>
    topPeInterAppendFocusBetweenCallNameTransitions(
        std::size_t focusPair) const noexcept {
        std::array<PeInterAppendCallNameTransitionSummary,
                   kPeRecorderInterAppendTopCallTransitionCount>
            top{};
        if (focusPair >= kPeInterAppendFocusPairCount) {
            return top;
        }
        for (std::size_t prevCallName = 0;
             prevCallName < kPeInterAppendCallNameCount; ++prevCallName) {
            for (std::size_t nextCallName = 0;
                 nextCallName < kPeInterAppendCallNameCount; ++nextCallName) {
                const std::size_t index =
                    peInterAppendFocusCallNameTransitionIndex(
                        focusPair,
                        static_cast<PeInterAppendCallName>(prevCallName),
                        static_cast<PeInterAppendCallName>(nextCallName));
                const std::uint64_t totalNs =
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallNameTransitionNsTotal[
                            index];
                if (totalNs == 0) {
                    continue;
                }
                PeInterAppendCallNameTransitionSummary candidate{
                    static_cast<std::uint32_t>(prevCallName),
                    static_cast<std::uint32_t>(nextCallName),
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallNameTransitionSamples[
                            index],
                    totalNs,
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallNameTransitionNsMax[
                            index]};
                for (auto& slot : top) {
                    if (candidate.totalNs <= slot.totalNs) {
                        continue;
                    }
                    std::swap(candidate, slot);
                }
            }
        }
        return top;
    }

    std::array<PeInterAppendCallSiteSummary,
               kPeRecorderInterAppendTopCallTransitionCount>
    topPeInterAppendFocusBetweenCallNameTransitionSites(
        std::size_t focusPair) const {
        std::array<PeInterAppendCallSiteSummary,
                   kPeRecorderInterAppendTopCallTransitionCount>
            top{};
        if (focusPair >= kPeInterAppendFocusPairCount) {
            return top;
        }
        const auto wantedFocusPair = static_cast<std::uint32_t>(focusPair);
        for (const auto& entry :
             peRecorderFocusBetweenCallNameTransitionSites_) {
            const auto& key = entry.first;
            const auto& stats = entry.second;
            if (key.focusPair != wantedFocusPair || stats.totalNs == 0) {
                continue;
            }
            PeInterAppendCallSiteSummary candidate{
                key.prevCallName, key.nextCallName, key.callerPc,
                stats.samples, stats.totalNs, stats.maxNs};
            for (auto& slot : top) {
                if (candidate.totalNs <= slot.totalNs) {
                    continue;
                }
                std::swap(candidate, slot);
            }
        }
        return top;
    }

    static bool peRecordMilestoneBit(std::uint32_t recordCount,
                                     std::uint32_t& bit) noexcept {
        switch (recordCount) {
        case 1: bit = 1u << 0; return true;
        case 4: bit = 1u << 1; return true;
        case 8: bit = 1u << 2; return true;
        case 16: bit = 1u << 3; return true;
        case 32: bit = 1u << 4; return true;
        case 64: bit = 1u << 5; return true;
        default:
            bit = 0;
            return false;
        }
    }

    void logPeRecordMilestoneAfterPresent(std::uint32_t type,
                                          std::uint32_t recordCount,
                                          std::uint32_t payloadBytes,
                                          std::int64_t entryNs) {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return;
        }
        std::uint32_t milestoneBit = 0;
        if (!peRecordMilestoneBit(recordCount, milestoneBit)) {
            return;
        }
        const std::uint64_t ordinal =
            pePresentRecordPendingOrdinal_.load(std::memory_order_acquire);
        if (ordinal == 0) {
            return;
        }
        std::uint32_t mask =
            pePresentRecordMilestoneMask_.load(std::memory_order_acquire);
        while ((mask & milestoneBit) == 0) {
            if (pePresentRecordMilestoneMask_.compare_exchange_weak(
                    mask, mask | milestoneBit, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                const std::int64_t returnNs =
                    pePresentCadenceReturnNs_.load(std::memory_order_acquire);
                dxmt9DeviceInfoLog(
                    "pe_present_record_milestone device=%p ordinal=%llu "
                    "milestone=%u type=%s typeId=%u call=%s "
                    "thread_id=0x%lx "
                    "entry_delta_ms=%.3f recordCount=%u payloadBytes=%u",
                    this, static_cast<unsigned long long>(ordinal), recordCount,
                    peCommandRecordTypeName(type), type,
                    dxmt9PeCurrentCallName ? dxmt9PeCurrentCallName : "unknown",
                    static_cast<unsigned long>(dxmt9PeCurrentThreadId()),
                    static_cast<double>(entryNs - returnNs) / 1000000.0,
                    recordCount, payloadBytes);
                return;
            }
        }
    }

    PePresentCadenceClaim claimPeFirstChunkAfterPresent() {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return {};
        }
        std::uint64_t ordinal =
            pePresentChunkPendingOrdinal_.load(std::memory_order_acquire);
        if (ordinal == 0) {
            return {};
        }
        const auto entry = std::chrono::steady_clock::now();
        if (!pePresentChunkPendingOrdinal_.compare_exchange_strong(
                ordinal, 0, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return {};
        }
        return PePresentCadenceClaim{
            true, ordinal,
            pePresentCadenceReturnNs_.load(std::memory_order_acquire),
            dxmt9SteadyClockNs(entry)};
    }

    void logPeFirstChunkAfterPresent(PeRecorderFlushReason reason,
                                     const PePresentCadenceClaim& claim,
                                     HRESULT hr,
                                     const PeCommandChunkCommitInfo& info) {
        if (!claim.claimed) {
            return;
        }
        const std::int64_t observedNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        dxmt9DeviceInfoLog(
            "pe_present_next_chunk device=%p ordinal=%llu reason=%s "
            "thread_id=0x%lx hr=0x%08x "
            "entry_delta_ms=%.3f return_delta_ms=%.3f "
            "bridge_ms=%.3f recordCount=%u payloadBytes=%u "
            "handleCount=%u wireBytes=%u",
            this, static_cast<unsigned long long>(claim.ordinal),
            peRecorderFlushReasonName(reason),
            static_cast<unsigned long>(dxmt9PeCurrentThreadId()),
            static_cast<unsigned>(hr),
            static_cast<double>(claim.entryNs - claim.returnNs) / 1000000.0,
            static_cast<double>(observedNs - claim.returnNs) / 1000000.0,
            static_cast<double>(observedNs - claim.entryNs) / 1000000.0,
            info.recordCount, info.payloadBytes, info.handleCount,
            info.wireBytes);
    }

    void recordPeChunkCommit(PeRecorderFlushReason reason,
                             std::uint32_t recordCount,
                             std::uint32_t payloadBytes,
                             std::uint32_t handleCount,
                             std::uint32_t wireBytes,
                             std::uint64_t fillGapNs,
                             std::uint64_t activeFillNs,
                             std::uint64_t bridgeNs) {
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
        if (fillGapNs > 0) {
            ++peRecorderStats_.chunkFillGapSamples;
            peRecorderStats_.chunkFillGapNsTotal += fillGapNs;
            peRecorderStats_.chunkFillGapNsMax =
                std::max(peRecorderStats_.chunkFillGapNsMax, fillGapNs);
        }
        if (activeFillNs > 0) {
            ++peRecorderStats_.chunkActiveFillSamples;
            peRecorderStats_.chunkActiveFillNsTotal += activeFillNs;
            peRecorderStats_.chunkActiveFillNsMax =
                std::max(peRecorderStats_.chunkActiveFillNsMax, activeFillNs);
        }
        ++peRecorderStats_.chunkBridgeSamples;
        peRecorderStats_.chunkBridgeNsTotal += bridgeNs;
        peRecorderStats_.chunkBridgeNsMax =
            std::max(peRecorderStats_.chunkBridgeNsMax, bridgeNs);
        const auto reasonIndex = static_cast<std::size_t>(reason);
        if (reasonIndex < peRecorderStats_.flushReasons.size()) {
            ++peRecorderStats_.flushReasons[reasonIndex];
        }
        if (dxmt9PeRecorderChunkLogEnabled()) {
            dxmt9DeviceInfoLog(
                "pe_recorder_chunk device=%p reason=%s commitCount=%llu "
                "recordCount=%u payloadBytes=%u handleCount=%u wireBytes=%u "
                "fillGapMs=%.3f activeFillMs=%.3f bridgeMs=%.3f",
                this, peRecorderFlushReasonName(reason),
                static_cast<unsigned long long>(peRecorderStats_.commitCount),
                recordCount, payloadBytes, handleCount, wireBytes,
                static_cast<double>(fillGapNs) / 1000000.0,
                static_cast<double>(activeFillNs) / 1000000.0,
                static_cast<double>(bridgeNs) / 1000000.0);
        }
    }

    void recordPeChunkInterAppendGap(std::int64_t appendEntryNs,
                                     std::uint32_t recordCountBefore,
                                     std::uint32_t nextType) {
        if (!dxmt9PeRecorderStatsEnabled() ||
            recordCountBefore == 0 ||
            peRecorderLastAppendReturnNs_ <= 0 ||
            appendEntryNs <= peRecorderLastAppendReturnNs_) {
            return;
        }
        const auto interAppendGapNs =
            static_cast<std::uint64_t>(
                appendEntryNs - peRecorderLastAppendReturnNs_);
        ++peRecorderStats_.chunkInterAppendGapSamples;
        peRecorderStats_.chunkInterAppendGapNsTotal += interAppendGapNs;
        peRecorderStats_.chunkInterAppendGapNsMax =
            std::max(peRecorderStats_.chunkInterAppendGapNsMax,
                     interAppendGapNs);
        const std::size_t pairIndex =
            peInterAppendPairIndex(peRecorderLastAppendRecordType_, nextType);
        ++peRecorderStats_.chunkInterAppendPairSamples[pairIndex];
        peRecorderStats_.chunkInterAppendPairNsTotal[pairIndex] +=
            interAppendGapNs;
        peRecorderStats_.chunkInterAppendPairNsMax[pairIndex] =
            std::max(peRecorderStats_.chunkInterAppendPairNsMax[pairIndex],
                     interAppendGapNs);
        const std::uint32_t prevType = peRecorderLastAppendRecordType_;
        const std::uint32_t nextBucket = peCommandRecordTypeBucket(nextType);
        const std::size_t focusPair =
            peInterAppendFocusPairIndex(prevType, nextBucket);
        if (focusPair < kPeInterAppendFocusPairCount) {
            PeInterAppendCallFamily callFamily = dxmt9PeCurrentAppendFamily;
            if (callFamily == PeInterAppendCallFamily::Unknown) {
                callFamily =
                    peInterAppendCallFamilyFromName(dxmt9PeCurrentCallName);
            }
            const std::size_t focusIndex =
                peInterAppendFocusCallFamilyIndex(focusPair, callFamily);
            ++peRecorderStats_
                .chunkInterAppendFocusCallFamilySamples[focusIndex];
            peRecorderStats_
                .chunkInterAppendFocusCallFamilyNsTotal[focusIndex] +=
                interAppendGapNs;
            peRecorderStats_
                .chunkInterAppendFocusCallFamilyNsMax[focusIndex] =
                std::max(
                    peRecorderStats_
                        .chunkInterAppendFocusCallFamilyNsMax[focusIndex],
                    interAppendGapNs);
            recordPeChunkInterAppendFocusPhaseSplit(focusPair, appendEntryNs);
        }
        if (peRecorderBetweenCallsActive_) {
            resetPeBetweenCallsWindow();
        }
    }

    void recordPeChunkInterAppendFocusPhaseSplit(std::size_t focusPair,
                                                 std::int64_t appendEntryNs) {
        if (focusPair >= kPeInterAppendFocusPairCount ||
            peRecorderLastAppendReturnNs_ <= 0 ||
            dxmt9PeCurrentCallEntryNs <= peRecorderLastAppendReturnNs_ ||
            dxmt9PeCurrentCallEntryNs > appendEntryNs) {
            return;
        }
        const auto preCallNs = static_cast<std::uint64_t>(
            dxmt9PeCurrentCallEntryNs - peRecorderLastAppendReturnNs_);
        const auto insideCallNs = static_cast<std::uint64_t>(
            appendEntryNs - dxmt9PeCurrentCallEntryNs);
        ++peRecorderStats_.chunkInterAppendFocusPhaseSamples[focusPair];
        peRecorderStats_.chunkInterAppendFocusPreCallNsTotal[focusPair] +=
            preCallNs;
        peRecorderStats_.chunkInterAppendFocusPreCallNsMax[focusPair] =
            std::max(
                peRecorderStats_.chunkInterAppendFocusPreCallNsMax[focusPair],
                preCallNs);
        peRecorderStats_.chunkInterAppendFocusInsideCallNsTotal[focusPair] +=
            insideCallNs;
        peRecorderStats_.chunkInterAppendFocusInsideCallNsMax[focusPair] =
            std::max(
                peRecorderStats_.chunkInterAppendFocusInsideCallNsMax[focusPair],
                insideCallNs);
        recordPeChunkInterAppendFocusTailSplit(focusPair);
    }

    void recordPeChunkInterAppendFocusTailSplit(std::size_t focusPair) {
        if (focusPair >= kPeInterAppendFocusPairCount ||
            peRecorderLastAppendReturnNs_ <= 0 ||
            peRecorderLastAppendCallExitNs_ <= peRecorderLastAppendReturnNs_ ||
            dxmt9PeCurrentCallEntryNs < peRecorderLastAppendCallExitNs_) {
            return;
        }
        const auto prevCallTailNs = static_cast<std::uint64_t>(
            peRecorderLastAppendCallExitNs_ - peRecorderLastAppendReturnNs_);
        const auto betweenCallsNs = static_cast<std::uint64_t>(
            dxmt9PeCurrentCallEntryNs - peRecorderLastAppendCallExitNs_);
        ++peRecorderStats_.chunkInterAppendFocusTailSplitSamples[focusPair];
        peRecorderStats_
            .chunkInterAppendFocusPrevCallTailNsTotal[focusPair] +=
            prevCallTailNs;
        peRecorderStats_.chunkInterAppendFocusPrevCallTailNsMax[focusPair] =
            std::max(
                peRecorderStats_
                    .chunkInterAppendFocusPrevCallTailNsMax[focusPair],
                prevCallTailNs);
        peRecorderStats_.chunkInterAppendFocusBetweenCallsNsTotal[focusPair] +=
            betweenCallsNs;
        peRecorderStats_.chunkInterAppendFocusBetweenCallsNsMax[focusPair] =
            std::max(
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallsNsMax[focusPair],
                betweenCallsNs);
        recordPeChunkInterAppendFocusBetweenCallFamilies(focusPair);
    }

    void recordPeBetweenCallsEntry(const char* callName,
                                   std::int64_t entryNs,
                                   const void* callerPc) {
        if (!dxmt9PeRecorderStatsEnabled() ||
            !peRecorderBetweenCallsActive_ ||
            entryNs <= peRecorderBetweenCallsStartNs_) {
            return;
        }
        const auto family = peInterAppendCallFamilyFromName(callName);
        const auto callNameBucket = peInterAppendCallNameFromName(callName);
        if (peRecorderBetweenLastCallExitNs_ >=
                peRecorderBetweenCallsStartNs_ &&
            entryNs > peRecorderBetweenLastCallExitNs_) {
            const std::size_t transitionIndex =
                peInterAppendCallTransitionIndex(
                    peRecorderBetweenLastCallFamily_, family);
            const auto gapNs = static_cast<std::uint64_t>(
                entryNs - peRecorderBetweenLastCallExitNs_);
            ++peRecorderBetweenCallTransitionSamples_[transitionIndex];
            peRecorderBetweenCallTransitionNsTotal_[transitionIndex] += gapNs;
            peRecorderBetweenCallTransitionNsMax_[transitionIndex] =
                std::max(
                    peRecorderBetweenCallTransitionNsMax_[transitionIndex],
                    gapNs);
            const std::size_t nameTransitionIndex =
                peInterAppendCallNameTransitionIndex(
                    peRecorderBetweenLastCallName_, callNameBucket);
            ++peRecorderBetweenCallNameTransitionSamples_[nameTransitionIndex];
            peRecorderBetweenCallNameTransitionNsTotal_[nameTransitionIndex] +=
                gapNs;
            peRecorderBetweenCallNameTransitionNsMax_[nameTransitionIndex] =
                std::max(
                    peRecorderBetweenCallNameTransitionNsMax_[
                        nameTransitionIndex],
                    gapNs);
            PeInterAppendCallSiteLocalKey siteKey{
                static_cast<std::uint32_t>(peRecorderBetweenLastCallName_),
                static_cast<std::uint32_t>(callNameBucket),
                callerPc};
            auto& siteStats =
                peRecorderBetweenCallNameTransitionSites_[siteKey];
            ++siteStats.samples;
            siteStats.totalNs += gapNs;
            siteStats.maxNs = std::max(siteStats.maxNs, gapNs);
        }
        peRecorderBetweenLastCallExitNs_ = 0;
        peRecorderBetweenLastCallFamily_ = family;
        peRecorderBetweenLastCallName_ = callNameBucket;
        ++peRecorderBetweenCallFamilySamples_[
            static_cast<std::size_t>(family)];
        ++peRecorderBetweenCallNameSamples_[
            static_cast<std::size_t>(callNameBucket)];
    }

    void recordPeBetweenCallsReturn(const char* callName,
                                    std::int64_t entryNs,
                                    std::int64_t exitNs) {
        if (!dxmt9PeRecorderStatsEnabled() ||
            !peRecorderBetweenCallsActive_ ||
            entryNs <= peRecorderBetweenCallsStartNs_ ||
            exitNs <= entryNs) {
            return;
        }
        const auto callNameBucket = peInterAppendCallNameFromName(callName);
        const auto index = static_cast<std::size_t>(callNameBucket);
        const auto cpuNs = static_cast<std::uint64_t>(exitNs - entryNs);
        peRecorderBetweenLastCallFamily_ =
            peInterAppendCallFamilyFromName(callName);
        peRecorderBetweenLastCallName_ = callNameBucket;
        peRecorderBetweenLastCallExitNs_ = exitNs;
        ++peRecorderBetweenCallBodyCalls_;
        peRecorderBetweenCallBodyCpuNsTotal_ += cpuNs;
        peRecorderBetweenCallBodyCpuNsMax_ =
            std::max(peRecorderBetweenCallBodyCpuNsMax_, cpuNs);
        peRecorderBetweenCallNameCpuNsTotal_[index] += cpuNs;
        peRecorderBetweenCallNameCpuNsMax_[index] =
            std::max(peRecorderBetweenCallNameCpuNsMax_[index], cpuNs);
    }

    void resetPeBetweenCallsWindow() {
        peRecorderBetweenCallsActive_ = false;
        peRecorderBetweenCallsStartNs_ = 0;
        peRecorderBetweenCallFamilySamples_.fill(0);
        peRecorderBetweenCallNameSamples_.fill(0);
        peRecorderBetweenCallNameCpuNsTotal_.fill(0);
        peRecorderBetweenCallNameCpuNsMax_.fill(0);
        peRecorderBetweenLastCallFamily_ = PeInterAppendCallFamily::Unknown;
        peRecorderBetweenLastCallName_ = PeInterAppendCallName::Unknown;
        peRecorderBetweenLastCallExitNs_ = 0;
        peRecorderBetweenCallTransitionSamples_.fill(0);
        peRecorderBetweenCallTransitionNsTotal_.fill(0);
        peRecorderBetweenCallTransitionNsMax_.fill(0);
        peRecorderBetweenCallNameTransitionSamples_.fill(0);
        peRecorderBetweenCallNameTransitionNsTotal_.fill(0);
        peRecorderBetweenCallNameTransitionNsMax_.fill(0);
        peRecorderBetweenCallNameTransitionSites_.clear();
        peRecorderBetweenCallBodyCalls_ = 0;
        peRecorderBetweenCallBodyCpuNsTotal_ = 0;
        peRecorderBetweenCallBodyCpuNsMax_ = 0;
    }

    void recordPeChunkInterAppendFocusBetweenCallFamilies(std::size_t focusPair) {
        if (focusPair >= kPeInterAppendFocusPairCount) {
            resetPeBetweenCallsWindow();
            return;
        }
        auto samples = peRecorderBetweenCallFamilySamples_;
        const auto terminalFamily =
            peRecorderBetweenCallsActive_
            ? peInterAppendCallFamilyFromName(dxmt9PeCurrentCallName)
            : PeInterAppendCallFamily::Unknown;
        auto& terminalSamples =
            samples[static_cast<std::size_t>(terminalFamily)];
        if (terminalSamples != 0) {
            --terminalSamples;
        }
        auto callNameSamples = peRecorderBetweenCallNameSamples_;
        auto callNameCpuNsTotal = peRecorderBetweenCallNameCpuNsTotal_;
        auto callNameCpuNsMax = peRecorderBetweenCallNameCpuNsMax_;
        auto transitionSamples = peRecorderBetweenCallTransitionSamples_;
        auto transitionNsTotal = peRecorderBetweenCallTransitionNsTotal_;
        auto transitionNsMax = peRecorderBetweenCallTransitionNsMax_;
        auto nameTransitionSamples =
            peRecorderBetweenCallNameTransitionSamples_;
        auto nameTransitionNsTotal =
            peRecorderBetweenCallNameTransitionNsTotal_;
        auto nameTransitionNsMax = peRecorderBetweenCallNameTransitionNsMax_;
        const std::uint64_t bodyCalls = peRecorderBetweenCallBodyCalls_;
        const std::uint64_t bodyCpuNsTotal =
            peRecorderBetweenCallBodyCpuNsTotal_;
        const std::uint64_t bodyCpuNsMax = peRecorderBetweenCallBodyCpuNsMax_;
        const auto terminalCallName =
            peRecorderBetweenCallsActive_
            ? peInterAppendCallNameFromName(dxmt9PeCurrentCallName)
            : PeInterAppendCallName::Unknown;
        auto& terminalCallNameSamples =
            callNameSamples[static_cast<std::size_t>(terminalCallName)];
        if (terminalCallNameSamples != 0) {
            --terminalCallNameSamples;
        }
        for (std::size_t family = 0; family < kPeInterAppendCallFamilyCount;
             ++family) {
            const std::uint64_t count = samples[family];
            if (count == 0) {
                continue;
            }
            const std::size_t index =
                focusPair * kPeInterAppendCallFamilyCount + family;
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallFamilySamples[index] += count;
        }
        for (std::size_t callName = 0; callName < kPeInterAppendCallNameCount;
             ++callName) {
            const std::uint64_t count = callNameSamples[callName];
            if (count == 0) {
                continue;
            }
            const std::size_t index =
                focusPair * kPeInterAppendCallNameCount + callName;
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallNameSamples[index] += count;
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallNameCpuNsTotal[index] +=
                callNameCpuNsTotal[callName];
            peRecorderStats_.chunkInterAppendFocusBetweenCallNameCpuNsMax[index] =
                std::max(
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallNameCpuNsMax[index],
                callNameCpuNsMax[callName]);
        }
        for (std::size_t prevFamily = 0;
             prevFamily < kPeInterAppendCallFamilyCount; ++prevFamily) {
            for (std::size_t nextFamily = 0;
                 nextFamily < kPeInterAppendCallFamilyCount; ++nextFamily) {
                const std::size_t localIndex =
                    prevFamily * kPeInterAppendCallFamilyCount + nextFamily;
                const std::uint64_t count = transitionSamples[localIndex];
                if (count == 0) {
                    continue;
                }
                const std::size_t index =
                    peInterAppendFocusCallTransitionIndex(
                        focusPair,
                        static_cast<PeInterAppendCallFamily>(prevFamily),
                        static_cast<PeInterAppendCallFamily>(nextFamily));
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallTransitionSamples[
                        index] += count;
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallTransitionNsTotal[
                        index] += transitionNsTotal[localIndex];
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallTransitionNsMax[index] =
                    std::max(
                        peRecorderStats_
                            .chunkInterAppendFocusBetweenCallTransitionNsMax[
                                index],
                        transitionNsMax[localIndex]);
            }
        }
        for (std::size_t prevCallName = 0;
             prevCallName < kPeInterAppendCallNameCount; ++prevCallName) {
            for (std::size_t nextCallName = 0;
                 nextCallName < kPeInterAppendCallNameCount; ++nextCallName) {
                const std::size_t localIndex =
                    prevCallName * kPeInterAppendCallNameCount + nextCallName;
                const std::uint64_t count =
                    nameTransitionSamples[localIndex];
                if (count == 0) {
                    continue;
                }
                const std::size_t index =
                    peInterAppendFocusCallNameTransitionIndex(
                        focusPair,
                        static_cast<PeInterAppendCallName>(prevCallName),
                        static_cast<PeInterAppendCallName>(nextCallName));
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameTransitionSamples[
                        index] += count;
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameTransitionNsTotal[
                        index] += nameTransitionNsTotal[localIndex];
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameTransitionNsMax[
                        index] =
                    std::max(
                        peRecorderStats_
                            .chunkInterAppendFocusBetweenCallNameTransitionNsMax[
                                index],
                        nameTransitionNsMax[localIndex]);
            }
        }
        const auto focusPairValue = static_cast<std::uint32_t>(focusPair);
        for (const auto& entry : peRecorderBetweenCallNameTransitionSites_) {
            const auto& key = entry.first;
            const auto& stats = entry.second;
            PeInterAppendCallSiteKey siteKey{
                focusPairValue, key.prevCallName, key.nextCallName,
                key.callerPc};
            auto& siteStats =
                peRecorderFocusBetweenCallNameTransitionSites_[siteKey];
            siteStats.samples += stats.samples;
            siteStats.totalNs += stats.totalNs;
            siteStats.maxNs = std::max(siteStats.maxNs, stats.maxNs);
        }
        peRecorderStats_.chunkInterAppendFocusBetweenCallBodyCalls[focusPair] +=
            bodyCalls;
        peRecorderStats_
            .chunkInterAppendFocusBetweenCallBodyCpuNsTotal[focusPair] +=
            bodyCpuNsTotal;
        peRecorderStats_.chunkInterAppendFocusBetweenCallBodyCpuNsMax[focusPair] =
            std::max(
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallBodyCpuNsMax[focusPair],
                bodyCpuNsMax);
        resetPeBetweenCallsWindow();
    }

    void notePeCurrentCallReturnForInterAppendSplit() {
        if (!dxmt9PeRecorderStatsEnabled() ||
            peRecorderLastAppendCallEntryNs_ <= 0 ||
            peRecorderLastAppendCallEntryNs_ != dxmt9PeCurrentCallEntryNs ||
            peRecorderLastAppendRecordType_ !=
                D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
            return;
        }
        peRecorderLastAppendCallExitNs_ =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        peRecorderBetweenCallsActive_ = true;
        peRecorderBetweenCallsStartNs_ = peRecorderLastAppendCallExitNs_;
        peRecorderBetweenCallFamilySamples_.fill(0);
        peRecorderBetweenCallNameSamples_.fill(0);
        peRecorderBetweenCallNameCpuNsTotal_.fill(0);
        peRecorderBetweenCallNameCpuNsMax_.fill(0);
        peRecorderBetweenLastCallFamily_ = PeInterAppendCallFamily::Draw;
        peRecorderBetweenLastCallName_ =
            PeInterAppendCallName::DrawIndexedPrimitive;
        peRecorderBetweenLastCallExitNs_ = peRecorderBetweenCallsStartNs_;
        peRecorderBetweenCallTransitionSamples_.fill(0);
        peRecorderBetweenCallTransitionNsTotal_.fill(0);
        peRecorderBetweenCallTransitionNsMax_.fill(0);
        peRecorderBetweenCallNameTransitionSamples_.fill(0);
        peRecorderBetweenCallNameTransitionNsTotal_.fill(0);
        peRecorderBetweenCallNameTransitionNsMax_.fill(0);
        peRecorderBetweenCallNameTransitionSites_.clear();
        peRecorderBetweenCallBodyCalls_ = 0;
        peRecorderBetweenCallBodyCpuNsTotal_ = 0;
        peRecorderBetweenCallBodyCpuNsMax_ = 0;
    }

    void recordPeAppendCpu(std::uint64_t appendCpuNs, bool noFlushAppend) {
        if (!dxmt9PeRecorderStatsEnabled() || appendCpuNs == 0) {
            return;
        }
        ++peRecorderStats_.recordAppendCalls;
        peRecorderStats_.recordAppendCpuNsTotal += appendCpuNs;
        peRecorderStats_.recordAppendCpuNsMax =
            std::max(peRecorderStats_.recordAppendCpuNsMax, appendCpuNs);
        if (!noFlushAppend) {
            return;
        }
        ++peRecorderStats_.recordAppendNoFlushCalls;
        peRecorderStats_.recordAppendNoFlushCpuNsTotal += appendCpuNs;
        peRecorderStats_.recordAppendNoFlushCpuNsMax =
            std::max(peRecorderStats_.recordAppendNoFlushCpuNsMax,
                     appendCpuNs);
    }

    void recordPeConstSetterCpu(std::uint32_t recordType,
                                std::int64_t entryNs,
                                std::uint32_t regs) {
        if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0) {
            return;
        }
        const std::int64_t returnNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        if (returnNs <= entryNs) {
            return;
        }
        const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
        if (recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F) {
            ++peRecorderStats_.vsConstFSetterCalls;
            peRecorderStats_.vsConstFSetterRegs += regs;
            peRecorderStats_.vsConstFSetterCpuNsTotal += cpuNs;
            peRecorderStats_.vsConstFSetterCpuNsMax =
                std::max(peRecorderStats_.vsConstFSetterCpuNsMax, cpuNs);
        } else if (recordType == D9C_COMMAND_RECORD_SET_PS_CONST_F) {
            ++peRecorderStats_.psConstFSetterCalls;
            peRecorderStats_.psConstFSetterRegs += regs;
            peRecorderStats_.psConstFSetterCpuNsTotal += cpuNs;
            peRecorderStats_.psConstFSetterCpuNsMax =
                std::max(peRecorderStats_.psConstFSetterCpuNsMax, cpuNs);
        }
    }

    void recordPeConstFlushCpu(std::uint32_t recordType,
                               std::int64_t entryNs,
                               std::uint32_t records,
                               std::uint32_t regs) {
        if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0 || records == 0u) {
            return;
        }
        const std::int64_t returnNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        if (returnNs <= entryNs) {
            return;
        }
        const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
        ++peRecorderStats_.constFlushCalls;
        peRecorderStats_.constFlushRecords += records;
        peRecorderStats_.constFlushRegs += regs;
        peRecorderStats_.constFlushCpuNsTotal += cpuNs;
        peRecorderStats_.constFlushCpuNsMax =
            std::max(peRecorderStats_.constFlushCpuNsMax, cpuNs);
        if (recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F) {
            peRecorderStats_.vsConstFFlushRecords += records;
            peRecorderStats_.vsConstFFlushRegs += regs;
            peRecorderStats_.vsConstFFlushCpuNsTotal += cpuNs;
        } else if (recordType == D9C_COMMAND_RECORD_SET_PS_CONST_F) {
            peRecorderStats_.psConstFFlushRecords += records;
            peRecorderStats_.psConstFFlushRegs += regs;
            peRecorderStats_.psConstFFlushCpuNsTotal += cpuNs;
        }
    }

    void recordPeChunkBarrierConstCpu(std::int64_t entryNs) {
        if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0) {
            return;
        }
        const std::int64_t returnNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        if (returnNs <= entryNs) {
            return;
        }
        const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
        ++peRecorderStats_.chunkBarrierFlushCalls;
        peRecorderStats_.chunkBarrierConstCpuNsTotal += cpuNs;
        peRecorderStats_.chunkBarrierConstCpuNsMax =
            std::max(peRecorderStats_.chunkBarrierConstCpuNsMax, cpuNs);
    }

    void recordPeApplyStateBuildCpu(std::int64_t entryNs) {
        if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0) {
            return;
        }
        const std::int64_t returnNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        if (returnNs <= entryNs) {
            return;
        }
        const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
        ++peRecorderStats_.applyStateBuildCalls;
        peRecorderStats_.applyStateBuildCpuNsTotal += cpuNs;
        peRecorderStats_.applyStateBuildCpuNsMax =
            std::max(peRecorderStats_.applyStateBuildCpuNsMax, cpuNs);
    }

    void recordPeHotStateSetterCpu(PeHotStateSetterFamily family,
                                   std::int64_t entryNs,
                                   bool dirty) {
        if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0) {
            return;
        }
        const auto index = static_cast<std::size_t>(family);
        if (index >= kPeHotStateSetterFamilyCount) {
            return;
        }
        ++peRecorderStats_.hotStateSetterCalls[index];
        if (dirty) {
            ++peRecorderStats_.hotStateSetterDirty[index];
        }
        const std::int64_t returnNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        if (returnNs <= entryNs) {
            return;
        }
        const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
        peRecorderStats_.hotStateSetterCpuNsTotal[index] += cpuNs;
        peRecorderStats_.hotStateSetterCpuNsMax[index] =
            std::max(peRecorderStats_.hotStateSetterCpuNsMax[index], cpuNs);
    }

    class PeHotStateSetterTimer {
    public:
        PeHotStateSetterTimer(D3D9DeviceImpl& device,
                              PeHotStateSetterFamily family) noexcept
        : device_(device), family_(family),
          entryNs_(dxmt9PeRecorderStatsEnabled()
              ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
              : 0) {
        }

        ~PeHotStateSetterTimer() noexcept {
            device_.recordPeHotStateSetterCpu(family_, entryNs_, dirty_);
        }

        void markDirty() noexcept {
            dirty_ = true;
        }

    private:
        D3D9DeviceImpl& device_;
        PeHotStateSetterFamily family_;
        std::int64_t entryNs_ = 0;
        bool dirty_ = false;
    };

    void notePeChunkAppendBoundary(std::int64_t appendReturnNs,
                                   std::uint32_t type) {
        if (!dxmt9PeRecorderStatsEnabled() ||
            commandChunkV2_.recordCount() == 0) {
            return;
        }
        if (peRecorderCurrentChunkFirstAppendNs_ == 0) {
            peRecorderCurrentChunkFirstAppendNs_ = appendReturnNs;
            const std::int64_t priorReturnNs = peRecorderLastChunkReturnNs_;
            if (priorReturnNs > 0 && appendReturnNs > priorReturnNs) {
                const auto firstRecordGapNs =
                    static_cast<std::uint64_t>(
                        appendReturnNs - priorReturnNs);
                ++peRecorderStats_.chunkFirstRecordGapSamples;
                peRecorderStats_.chunkFirstRecordGapNsTotal += firstRecordGapNs;
                peRecorderStats_.chunkFirstRecordGapNsMax =
                    std::max(peRecorderStats_.chunkFirstRecordGapNsMax,
                             firstRecordGapNs);
            }
        }
        peRecorderLastAppendReturnNs_ = appendReturnNs;
        peRecorderLastAppendCallEntryNs_ = dxmt9PeCurrentCallEntryNs;
        peRecorderLastAppendCallExitNs_ = 0;
        peRecorderLastAppendRecordType_ = peCommandRecordTypeBucket(type);
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
        const auto topInterAppendPairs = topPeInterAppendPairs();
        const auto hotRt =
            static_cast<std::size_t>(PeHotStateSetterFamily::RenderTarget);
        const auto hotDs =
            static_cast<std::size_t>(PeHotStateSetterFamily::DepthStencil);
        const auto hotViewportScissor =
            static_cast<std::size_t>(PeHotStateSetterFamily::ViewportScissor);
        const auto hotTransform =
            static_cast<std::size_t>(PeHotStateSetterFamily::Transform);
        const auto hotMaterialLightClip =
            static_cast<std::size_t>(PeHotStateSetterFamily::MaterialLightClip);
        const auto hotRenderState =
            static_cast<std::size_t>(PeHotStateSetterFamily::RenderState);
        const auto hotTssSampler =
            static_cast<std::size_t>(
                PeHotStateSetterFamily::TextureStageSampler);
        const auto hotTexture =
            static_cast<std::size_t>(PeHotStateSetterFamily::Texture);
        const auto hotVertexInput =
            static_cast<std::size_t>(PeHotStateSetterFamily::VertexInput);
        const auto hotShader =
            static_cast<std::size_t>(PeHotStateSetterFamily::Shader);
        const auto gapVsConstFocus = static_cast<std::size_t>(
            PeInterAppendFocusPair::DrawIndexedToVsConstF);
        const auto gapApplyFocus = static_cast<std::size_t>(
            PeInterAppendFocusPair::DrawIndexedToApplyState);
        const auto gapDrawFocus = static_cast<std::size_t>(
            PeInterAppendFocusPair::DrawIndexedToDrawIndexed);
        const auto gapPsConstFocus = static_cast<std::size_t>(
            PeInterAppendFocusPair::DrawIndexedToPsConstF);
        const auto gapVsConstFamilies =
            topPeInterAppendFocusCallFamilies(gapVsConstFocus);
        const auto gapApplyFamilies =
            topPeInterAppendFocusCallFamilies(gapApplyFocus);
        const auto gapDrawFamilies =
            topPeInterAppendFocusCallFamilies(gapDrawFocus);
        const auto gapPsConstFamilies =
            topPeInterAppendFocusCallFamilies(gapPsConstFocus);
        const auto gapVsConstBetweenFamilies =
            topPeInterAppendFocusBetweenCallFamilies(gapVsConstFocus);
        const auto gapApplyBetweenFamilies =
            topPeInterAppendFocusBetweenCallFamilies(gapApplyFocus);
        const auto gapDrawBetweenFamilies =
            topPeInterAppendFocusBetweenCallFamilies(gapDrawFocus);
        const auto gapPsConstBetweenFamilies =
            topPeInterAppendFocusBetweenCallFamilies(gapPsConstFocus);
        const auto gapVsConstBetweenCallNames =
            topPeInterAppendFocusBetweenCallNames(gapVsConstFocus);
        const auto gapApplyBetweenCallNames =
            topPeInterAppendFocusBetweenCallNames(gapApplyFocus);
        const auto gapDrawBetweenCallNames =
            topPeInterAppendFocusBetweenCallNames(gapDrawFocus);
        const auto gapPsConstBetweenCallNames =
            topPeInterAppendFocusBetweenCallNames(gapPsConstFocus);
        const auto gapVsConstBetweenTransitions =
            topPeInterAppendFocusBetweenCallTransitions(gapVsConstFocus);
        const auto gapApplyBetweenTransitions =
            topPeInterAppendFocusBetweenCallTransitions(gapApplyFocus);
        const auto gapDrawBetweenTransitions =
            topPeInterAppendFocusBetweenCallTransitions(gapDrawFocus);
        const auto gapPsConstBetweenTransitions =
            topPeInterAppendFocusBetweenCallTransitions(gapPsConstFocus);
        const auto gapVsConstBetweenNameTransitions =
            topPeInterAppendFocusBetweenCallNameTransitions(gapVsConstFocus);
        const auto gapApplyBetweenNameTransitions =
            topPeInterAppendFocusBetweenCallNameTransitions(gapApplyFocus);
        const auto gapDrawBetweenNameTransitions =
            topPeInterAppendFocusBetweenCallNameTransitions(gapDrawFocus);
        const auto gapPsConstBetweenNameTransitions =
            topPeInterAppendFocusBetweenCallNameTransitions(gapPsConstFocus);
        const auto gapVsConstBetweenCallSites =
            topPeInterAppendFocusBetweenCallNameTransitionSites(gapVsConstFocus);
        const auto gapApplyBetweenCallSites =
            topPeInterAppendFocusBetweenCallNameTransitionSites(gapApplyFocus);
        const auto gapDrawBetweenCallSites =
            topPeInterAppendFocusBetweenCallNameTransitionSites(gapDrawFocus);
        const auto gapPsConstBetweenCallSites =
            topPeInterAppendFocusBetweenCallNameTransitionSites(gapPsConstFocus);
        const auto phaseSamples = [this](std::size_t focus) noexcept {
            return peRecorderStats_.chunkInterAppendFocusPhaseSamples[focus];
        };
        const auto preCallMs = [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusPreCallNsTotal[focus]) /
                1000000.0;
        };
        const auto preCallMaxMs = [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusPreCallNsMax[focus]) /
                1000000.0;
        };
        const auto insideCallMs = [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusInsideCallNsTotal[focus]) /
                1000000.0;
        };
        const auto insideCallMaxMs = [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusInsideCallNsMax[focus]) /
                1000000.0;
        };
        const auto tailSplitSamples = [this](std::size_t focus) noexcept {
            return peRecorderStats_
                .chunkInterAppendFocusTailSplitSamples[focus];
        };
        const auto prevCallTailMs = [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusPrevCallTailNsTotal[focus]) /
                1000000.0;
        };
        const auto prevCallTailMaxMs = [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusPrevCallTailNsMax[focus]) /
                1000000.0;
        };
        const auto betweenCallsMs = [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallsNsTotal[focus]) /
                1000000.0;
        };
        const auto betweenCallsMaxMs = [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallsNsMax[focus]) /
                1000000.0;
        };
        const auto betweenCallBodyCalls = [this](std::size_t focus) noexcept {
            return peRecorderStats_
                .chunkInterAppendFocusBetweenCallBodyCalls[focus];
        };
        const auto betweenCallBodyCpuMs = [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallBodyCpuNsTotal[focus]) /
                1000000.0;
        };
        const auto betweenCallBodyCpuMaxMs =
            [this](std::size_t focus) noexcept {
                return static_cast<double>(
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallBodyCpuNsMax[focus]) /
                    1000000.0;
            };
        dxmt9DeviceInfoLog(
            "pe_recorder_stats event=%s device=%p commitCount=%llu "
            "recordCountTotal=%llu recordCountMax=%llu "
            "payloadBytesTotal=%llu payloadBytesMax=%llu "
            "handleCountTotal=%llu handleCountMax=%llu "
            "chunkFillGapSamples=%llu chunkFillGapMs=%.3f "
            "chunkFillGapMaxMs=%.3f "
            "chunkFirstRecordGapSamples=%llu chunkFirstRecordGapMs=%.3f "
            "chunkFirstRecordGapMaxMs=%.3f "
            "chunkActiveFillSamples=%llu chunkActiveFillMs=%.3f "
            "chunkActiveFillMaxMs=%.3f "
            "chunkInterAppendGapSamples=%llu chunkInterAppendGapMs=%.3f "
            "chunkInterAppendGapMaxMs=%.3f "
            "chunkBridgeSamples=%llu chunkBridgeMs=%.3f "
            "chunkBridgeMaxMs=%.3f "
            "recordAppendCalls=%llu recordAppendCpuMs=%.3f "
            "recordAppendCpuMaxMs=%.3f "
            "recordAppendNoFlushCalls=%llu recordAppendNoFlushCpuMs=%.3f "
            "recordAppendNoFlushCpuMaxMs=%.3f "
            "interAppendTop1PrevType=%u interAppendTop1Prev=%s "
            "interAppendTop1NextType=%u interAppendTop1Next=%s "
            "interAppendTop1Samples=%llu interAppendTop1Ms=%.3f "
            "interAppendTop1MaxMs=%.3f "
            "interAppendTop2PrevType=%u interAppendTop2Prev=%s "
            "interAppendTop2NextType=%u interAppendTop2Next=%s "
            "interAppendTop2Samples=%llu interAppendTop2Ms=%.3f "
            "interAppendTop2MaxMs=%.3f "
            "interAppendTop3PrevType=%u interAppendTop3Prev=%s "
            "interAppendTop3NextType=%u interAppendTop3Next=%s "
            "interAppendTop3Samples=%llu interAppendTop3Ms=%.3f "
            "interAppendTop3MaxMs=%.3f "
            "interAppendTop4PrevType=%u interAppendTop4Prev=%s "
            "interAppendTop4NextType=%u interAppendTop4Next=%s "
            "interAppendTop4Samples=%llu interAppendTop4Ms=%.3f "
            "interAppendTop4MaxMs=%.3f "
            "vsConstFSetterCalls=%llu vsConstFSetterRegs=%llu "
            "vsConstFSetterCpuMs=%.3f vsConstFSetterCpuMaxMs=%.3f "
            "psConstFSetterCalls=%llu psConstFSetterRegs=%llu "
            "psConstFSetterCpuMs=%.3f psConstFSetterCpuMaxMs=%.3f "
            "constFlushCalls=%llu constFlushRecords=%llu "
            "constFlushRegs=%llu constFlushCpuMs=%.3f "
            "constFlushCpuMaxMs=%.3f "
            "vsConstFFlushRecords=%llu vsConstFFlushRegs=%llu "
            "vsConstFFlushCpuMs=%.3f "
            "psConstFFlushRecords=%llu psConstFFlushRegs=%llu "
            "psConstFFlushCpuMs=%.3f "
            "chunkBarrierFlushCalls=%llu chunkBarrierConstCpuMs=%.3f "
            "chunkBarrierConstCpuMaxMs=%.3f "
            "applyStateBuildCalls=%llu applyStateBuildCpuMs=%.3f "
            "applyStateBuildCpuMaxMs=%.3f "
            "hotSetterRtCalls=%llu hotSetterRtDirty=%llu "
            "hotSetterRtCpuMs=%.3f hotSetterRtCpuMaxMs=%.3f "
            "hotSetterDsCalls=%llu hotSetterDsDirty=%llu "
            "hotSetterDsCpuMs=%.3f hotSetterDsCpuMaxMs=%.3f "
            "hotSetterViewportScissorCalls=%llu "
            "hotSetterViewportScissorDirty=%llu "
            "hotSetterViewportScissorCpuMs=%.3f "
            "hotSetterViewportScissorCpuMaxMs=%.3f "
            "hotSetterTransformCalls=%llu hotSetterTransformDirty=%llu "
            "hotSetterTransformCpuMs=%.3f "
            "hotSetterTransformCpuMaxMs=%.3f "
            "hotSetterMaterialLightClipCalls=%llu "
            "hotSetterMaterialLightClipDirty=%llu "
            "hotSetterMaterialLightClipCpuMs=%.3f "
            "hotSetterMaterialLightClipCpuMaxMs=%.3f "
            "hotSetterRenderStateCalls=%llu "
            "hotSetterRenderStateDirty=%llu "
            "hotSetterRenderStateCpuMs=%.3f "
            "hotSetterRenderStateCpuMaxMs=%.3f "
            "hotSetterTssSamplerCalls=%llu "
            "hotSetterTssSamplerDirty=%llu "
            "hotSetterTssSamplerCpuMs=%.3f "
            "hotSetterTssSamplerCpuMaxMs=%.3f "
            "hotSetterTextureCalls=%llu hotSetterTextureDirty=%llu "
            "hotSetterTextureCpuMs=%.3f "
            "hotSetterTextureCpuMaxMs=%.3f "
            "hotSetterVertexInputCalls=%llu "
            "hotSetterVertexInputDirty=%llu "
            "hotSetterVertexInputCpuMs=%.3f "
            "hotSetterVertexInputCpuMaxMs=%.3f "
            "hotSetterShaderCalls=%llu hotSetterShaderDirty=%llu "
            "hotSetterShaderCpuMs=%.3f "
            "hotSetterShaderCpuMaxMs=%.3f "
            "gapDrawIndexedVsConstFTop1CallFamily=%s "
            "gapDrawIndexedVsConstFTop1Samples=%llu "
            "gapDrawIndexedVsConstFTop1Ms=%.3f "
            "gapDrawIndexedVsConstFTop1MaxMs=%.3f "
            "gapDrawIndexedVsConstFTop2CallFamily=%s "
            "gapDrawIndexedVsConstFTop2Samples=%llu "
            "gapDrawIndexedVsConstFTop2Ms=%.3f "
            "gapDrawIndexedVsConstFTop2MaxMs=%.3f "
            "gapDrawIndexedApplyStateTop1CallFamily=%s "
            "gapDrawIndexedApplyStateTop1Samples=%llu "
            "gapDrawIndexedApplyStateTop1Ms=%.3f "
            "gapDrawIndexedApplyStateTop1MaxMs=%.3f "
            "gapDrawIndexedApplyStateTop2CallFamily=%s "
            "gapDrawIndexedApplyStateTop2Samples=%llu "
            "gapDrawIndexedApplyStateTop2Ms=%.3f "
            "gapDrawIndexedApplyStateTop2MaxMs=%.3f "
            "gapDrawIndexedDrawIndexedTop1CallFamily=%s "
            "gapDrawIndexedDrawIndexedTop1Samples=%llu "
            "gapDrawIndexedDrawIndexedTop1Ms=%.3f "
            "gapDrawIndexedDrawIndexedTop1MaxMs=%.3f "
            "gapDrawIndexedDrawIndexedTop2CallFamily=%s "
            "gapDrawIndexedDrawIndexedTop2Samples=%llu "
            "gapDrawIndexedDrawIndexedTop2Ms=%.3f "
            "gapDrawIndexedDrawIndexedTop2MaxMs=%.3f "
            "gapDrawIndexedPsConstFTop1CallFamily=%s "
            "gapDrawIndexedPsConstFTop1Samples=%llu "
            "gapDrawIndexedPsConstFTop1Ms=%.3f "
            "gapDrawIndexedPsConstFTop1MaxMs=%.3f "
            "gapDrawIndexedPsConstFTop2CallFamily=%s "
            "gapDrawIndexedPsConstFTop2Samples=%llu "
            "gapDrawIndexedPsConstFTop2Ms=%.3f "
            "gapDrawIndexedPsConstFTop2MaxMs=%.3f "
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
                peRecorderStats_.chunkFillGapSamples),
            static_cast<double>(peRecorderStats_.chunkFillGapNsTotal) /
                1000000.0,
            static_cast<double>(peRecorderStats_.chunkFillGapNsMax) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.chunkFirstRecordGapSamples),
            static_cast<double>(
                peRecorderStats_.chunkFirstRecordGapNsTotal) / 1000000.0,
            static_cast<double>(
                peRecorderStats_.chunkFirstRecordGapNsMax) / 1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.chunkActiveFillSamples),
            static_cast<double>(peRecorderStats_.chunkActiveFillNsTotal) /
                1000000.0,
            static_cast<double>(peRecorderStats_.chunkActiveFillNsMax) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.chunkInterAppendGapSamples),
            static_cast<double>(
                peRecorderStats_.chunkInterAppendGapNsTotal) / 1000000.0,
            static_cast<double>(
                peRecorderStats_.chunkInterAppendGapNsMax) / 1000000.0,
            static_cast<unsigned long long>(peRecorderStats_.chunkBridgeSamples),
            static_cast<double>(peRecorderStats_.chunkBridgeNsTotal) /
                1000000.0,
            static_cast<double>(peRecorderStats_.chunkBridgeNsMax) /
                1000000.0,
            static_cast<unsigned long long>(peRecorderStats_.recordAppendCalls),
            static_cast<double>(peRecorderStats_.recordAppendCpuNsTotal) /
                1000000.0,
            static_cast<double>(peRecorderStats_.recordAppendCpuNsMax) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.recordAppendNoFlushCalls),
            static_cast<double>(
                peRecorderStats_.recordAppendNoFlushCpuNsTotal) / 1000000.0,
            static_cast<double>(
                peRecorderStats_.recordAppendNoFlushCpuNsMax) / 1000000.0,
            topInterAppendPairs[0].prevType,
            peCommandRecordTypeName(topInterAppendPairs[0].prevType),
            topInterAppendPairs[0].nextType,
            peCommandRecordTypeName(topInterAppendPairs[0].nextType),
            static_cast<unsigned long long>(topInterAppendPairs[0].samples),
            static_cast<double>(topInterAppendPairs[0].totalNs) / 1000000.0,
            static_cast<double>(topInterAppendPairs[0].maxNs) / 1000000.0,
            topInterAppendPairs[1].prevType,
            peCommandRecordTypeName(topInterAppendPairs[1].prevType),
            topInterAppendPairs[1].nextType,
            peCommandRecordTypeName(topInterAppendPairs[1].nextType),
            static_cast<unsigned long long>(topInterAppendPairs[1].samples),
            static_cast<double>(topInterAppendPairs[1].totalNs) / 1000000.0,
            static_cast<double>(topInterAppendPairs[1].maxNs) / 1000000.0,
            topInterAppendPairs[2].prevType,
            peCommandRecordTypeName(topInterAppendPairs[2].prevType),
            topInterAppendPairs[2].nextType,
            peCommandRecordTypeName(topInterAppendPairs[2].nextType),
            static_cast<unsigned long long>(topInterAppendPairs[2].samples),
            static_cast<double>(topInterAppendPairs[2].totalNs) / 1000000.0,
            static_cast<double>(topInterAppendPairs[2].maxNs) / 1000000.0,
            topInterAppendPairs[3].prevType,
            peCommandRecordTypeName(topInterAppendPairs[3].prevType),
            topInterAppendPairs[3].nextType,
            peCommandRecordTypeName(topInterAppendPairs[3].nextType),
            static_cast<unsigned long long>(topInterAppendPairs[3].samples),
            static_cast<double>(topInterAppendPairs[3].totalNs) / 1000000.0,
            static_cast<double>(topInterAppendPairs[3].maxNs) / 1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.vsConstFSetterCalls),
            static_cast<unsigned long long>(
                peRecorderStats_.vsConstFSetterRegs),
            static_cast<double>(
                peRecorderStats_.vsConstFSetterCpuNsTotal) / 1000000.0,
            static_cast<double>(
                peRecorderStats_.vsConstFSetterCpuNsMax) / 1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.psConstFSetterCalls),
            static_cast<unsigned long long>(
                peRecorderStats_.psConstFSetterRegs),
            static_cast<double>(
                peRecorderStats_.psConstFSetterCpuNsTotal) / 1000000.0,
            static_cast<double>(
                peRecorderStats_.psConstFSetterCpuNsMax) / 1000000.0,
            static_cast<unsigned long long>(peRecorderStats_.constFlushCalls),
            static_cast<unsigned long long>(peRecorderStats_.constFlushRecords),
            static_cast<unsigned long long>(peRecorderStats_.constFlushRegs),
            static_cast<double>(peRecorderStats_.constFlushCpuNsTotal) /
                1000000.0,
            static_cast<double>(peRecorderStats_.constFlushCpuNsMax) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.vsConstFFlushRecords),
            static_cast<unsigned long long>(peRecorderStats_.vsConstFFlushRegs),
            static_cast<double>(
                peRecorderStats_.vsConstFFlushCpuNsTotal) / 1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.psConstFFlushRecords),
            static_cast<unsigned long long>(peRecorderStats_.psConstFFlushRegs),
            static_cast<double>(
                peRecorderStats_.psConstFFlushCpuNsTotal) / 1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.chunkBarrierFlushCalls),
            static_cast<double>(
                peRecorderStats_.chunkBarrierConstCpuNsTotal) / 1000000.0,
            static_cast<double>(
                peRecorderStats_.chunkBarrierConstCpuNsMax) / 1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.applyStateBuildCalls),
            static_cast<double>(
                peRecorderStats_.applyStateBuildCpuNsTotal) / 1000000.0,
            static_cast<double>(
                peRecorderStats_.applyStateBuildCpuNsMax) / 1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotRt]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotRt]),
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsTotal[hotRt]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsMax[hotRt]) / 1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotDs]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotDs]),
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsTotal[hotDs]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsMax[hotDs]) / 1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotViewportScissor]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotViewportScissor]),
            static_cast<double>(
                peRecorderStats_
                    .hotStateSetterCpuNsTotal[hotViewportScissor]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsMax[hotViewportScissor]) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotTransform]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotTransform]),
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsTotal[hotTransform]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsMax[hotTransform]) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotMaterialLightClip]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotMaterialLightClip]),
            static_cast<double>(
                peRecorderStats_
                    .hotStateSetterCpuNsTotal[hotMaterialLightClip]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_
                    .hotStateSetterCpuNsMax[hotMaterialLightClip]) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotRenderState]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotRenderState]),
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsTotal[hotRenderState]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsMax[hotRenderState]) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotTssSampler]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotTssSampler]),
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsTotal[hotTssSampler]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsMax[hotTssSampler]) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotTexture]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotTexture]),
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsTotal[hotTexture]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsMax[hotTexture]) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotVertexInput]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotVertexInput]),
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsTotal[hotVertexInput]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsMax[hotVertexInput]) /
                1000000.0,
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterCalls[hotShader]),
            static_cast<unsigned long long>(
                peRecorderStats_.hotStateSetterDirty[hotShader]),
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsTotal[hotShader]) /
                1000000.0,
            static_cast<double>(
                peRecorderStats_.hotStateSetterCpuNsMax[hotShader]) /
                1000000.0,
            peInterAppendCallFamilyName(gapVsConstFamilies[0].family),
            static_cast<unsigned long long>(gapVsConstFamilies[0].samples),
            static_cast<double>(gapVsConstFamilies[0].totalNs) / 1000000.0,
            static_cast<double>(gapVsConstFamilies[0].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapVsConstFamilies[1].family),
            static_cast<unsigned long long>(gapVsConstFamilies[1].samples),
            static_cast<double>(gapVsConstFamilies[1].totalNs) / 1000000.0,
            static_cast<double>(gapVsConstFamilies[1].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapApplyFamilies[0].family),
            static_cast<unsigned long long>(gapApplyFamilies[0].samples),
            static_cast<double>(gapApplyFamilies[0].totalNs) / 1000000.0,
            static_cast<double>(gapApplyFamilies[0].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapApplyFamilies[1].family),
            static_cast<unsigned long long>(gapApplyFamilies[1].samples),
            static_cast<double>(gapApplyFamilies[1].totalNs) / 1000000.0,
            static_cast<double>(gapApplyFamilies[1].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapDrawFamilies[0].family),
            static_cast<unsigned long long>(gapDrawFamilies[0].samples),
            static_cast<double>(gapDrawFamilies[0].totalNs) / 1000000.0,
            static_cast<double>(gapDrawFamilies[0].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapDrawFamilies[1].family),
            static_cast<unsigned long long>(gapDrawFamilies[1].samples),
            static_cast<double>(gapDrawFamilies[1].totalNs) / 1000000.0,
            static_cast<double>(gapDrawFamilies[1].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapPsConstFamilies[0].family),
            static_cast<unsigned long long>(gapPsConstFamilies[0].samples),
            static_cast<double>(gapPsConstFamilies[0].totalNs) / 1000000.0,
            static_cast<double>(gapPsConstFamilies[0].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapPsConstFamilies[1].family),
            static_cast<unsigned long long>(gapPsConstFamilies[1].samples),
            static_cast<double>(gapPsConstFamilies[1].totalNs) / 1000000.0,
            static_cast<double>(gapPsConstFamilies[1].maxNs) / 1000000.0,
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
        dxmt9DeviceInfoLog(
            "pe_recorder_gap_call_stats event=%s device=%p "
            "gapDrawIndexedVsConstFTop1CallFamily=%s "
            "gapDrawIndexedVsConstFTop1Samples=%llu "
            "gapDrawIndexedVsConstFTop1Ms=%.3f "
            "gapDrawIndexedVsConstFTop1MaxMs=%.3f "
            "gapDrawIndexedVsConstFTop2CallFamily=%s "
            "gapDrawIndexedVsConstFTop2Samples=%llu "
            "gapDrawIndexedVsConstFTop2Ms=%.3f "
            "gapDrawIndexedVsConstFTop2MaxMs=%.3f "
            "gapDrawIndexedApplyStateTop1CallFamily=%s "
            "gapDrawIndexedApplyStateTop1Samples=%llu "
            "gapDrawIndexedApplyStateTop1Ms=%.3f "
            "gapDrawIndexedApplyStateTop1MaxMs=%.3f "
            "gapDrawIndexedApplyStateTop2CallFamily=%s "
            "gapDrawIndexedApplyStateTop2Samples=%llu "
            "gapDrawIndexedApplyStateTop2Ms=%.3f "
            "gapDrawIndexedApplyStateTop2MaxMs=%.3f "
            "gapDrawIndexedDrawIndexedTop1CallFamily=%s "
            "gapDrawIndexedDrawIndexedTop1Samples=%llu "
            "gapDrawIndexedDrawIndexedTop1Ms=%.3f "
            "gapDrawIndexedDrawIndexedTop1MaxMs=%.3f "
            "gapDrawIndexedDrawIndexedTop2CallFamily=%s "
            "gapDrawIndexedDrawIndexedTop2Samples=%llu "
            "gapDrawIndexedDrawIndexedTop2Ms=%.3f "
            "gapDrawIndexedDrawIndexedTop2MaxMs=%.3f "
            "gapDrawIndexedPsConstFTop1CallFamily=%s "
            "gapDrawIndexedPsConstFTop1Samples=%llu "
            "gapDrawIndexedPsConstFTop1Ms=%.3f "
            "gapDrawIndexedPsConstFTop1MaxMs=%.3f "
            "gapDrawIndexedPsConstFTop2CallFamily=%s "
            "gapDrawIndexedPsConstFTop2Samples=%llu "
            "gapDrawIndexedPsConstFTop2Ms=%.3f "
            "gapDrawIndexedPsConstFTop2MaxMs=%.3f "
            "gapDrawIndexedVsConstFPhaseSamples=%llu "
            "gapDrawIndexedVsConstFPreCallMs=%.3f "
            "gapDrawIndexedVsConstFPreCallMaxMs=%.3f "
            "gapDrawIndexedVsConstFInsideCallMs=%.3f "
            "gapDrawIndexedVsConstFInsideCallMaxMs=%.3f "
            "gapDrawIndexedApplyStatePhaseSamples=%llu "
            "gapDrawIndexedApplyStatePreCallMs=%.3f "
            "gapDrawIndexedApplyStatePreCallMaxMs=%.3f "
            "gapDrawIndexedApplyStateInsideCallMs=%.3f "
            "gapDrawIndexedApplyStateInsideCallMaxMs=%.3f "
            "gapDrawIndexedDrawIndexedPhaseSamples=%llu "
            "gapDrawIndexedDrawIndexedPreCallMs=%.3f "
            "gapDrawIndexedDrawIndexedPreCallMaxMs=%.3f "
            "gapDrawIndexedDrawIndexedInsideCallMs=%.3f "
            "gapDrawIndexedDrawIndexedInsideCallMaxMs=%.3f "
            "gapDrawIndexedPsConstFPhaseSamples=%llu "
            "gapDrawIndexedPsConstFPreCallMs=%.3f "
            "gapDrawIndexedPsConstFPreCallMaxMs=%.3f "
            "gapDrawIndexedPsConstFInsideCallMs=%.3f "
            "gapDrawIndexedPsConstFInsideCallMaxMs=%.3f",
            event ? event : "unknown", this,
            peInterAppendCallFamilyName(gapVsConstFamilies[0].family),
            static_cast<unsigned long long>(gapVsConstFamilies[0].samples),
            static_cast<double>(gapVsConstFamilies[0].totalNs) / 1000000.0,
            static_cast<double>(gapVsConstFamilies[0].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapVsConstFamilies[1].family),
            static_cast<unsigned long long>(gapVsConstFamilies[1].samples),
            static_cast<double>(gapVsConstFamilies[1].totalNs) / 1000000.0,
            static_cast<double>(gapVsConstFamilies[1].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapApplyFamilies[0].family),
            static_cast<unsigned long long>(gapApplyFamilies[0].samples),
            static_cast<double>(gapApplyFamilies[0].totalNs) / 1000000.0,
            static_cast<double>(gapApplyFamilies[0].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapApplyFamilies[1].family),
            static_cast<unsigned long long>(gapApplyFamilies[1].samples),
            static_cast<double>(gapApplyFamilies[1].totalNs) / 1000000.0,
            static_cast<double>(gapApplyFamilies[1].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapDrawFamilies[0].family),
            static_cast<unsigned long long>(gapDrawFamilies[0].samples),
            static_cast<double>(gapDrawFamilies[0].totalNs) / 1000000.0,
            static_cast<double>(gapDrawFamilies[0].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapDrawFamilies[1].family),
            static_cast<unsigned long long>(gapDrawFamilies[1].samples),
            static_cast<double>(gapDrawFamilies[1].totalNs) / 1000000.0,
            static_cast<double>(gapDrawFamilies[1].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapPsConstFamilies[0].family),
            static_cast<unsigned long long>(gapPsConstFamilies[0].samples),
            static_cast<double>(gapPsConstFamilies[0].totalNs) / 1000000.0,
            static_cast<double>(gapPsConstFamilies[0].maxNs) / 1000000.0,
            peInterAppendCallFamilyName(gapPsConstFamilies[1].family),
            static_cast<unsigned long long>(gapPsConstFamilies[1].samples),
            static_cast<double>(gapPsConstFamilies[1].totalNs) / 1000000.0,
            static_cast<double>(gapPsConstFamilies[1].maxNs) / 1000000.0,
            static_cast<unsigned long long>(phaseSamples(gapVsConstFocus)),
            preCallMs(gapVsConstFocus),
            preCallMaxMs(gapVsConstFocus),
            insideCallMs(gapVsConstFocus),
            insideCallMaxMs(gapVsConstFocus),
            static_cast<unsigned long long>(phaseSamples(gapApplyFocus)),
            preCallMs(gapApplyFocus),
            preCallMaxMs(gapApplyFocus),
            insideCallMs(gapApplyFocus),
            insideCallMaxMs(gapApplyFocus),
            static_cast<unsigned long long>(phaseSamples(gapDrawFocus)),
            preCallMs(gapDrawFocus),
            preCallMaxMs(gapDrawFocus),
            insideCallMs(gapDrawFocus),
            insideCallMaxMs(gapDrawFocus),
            static_cast<unsigned long long>(phaseSamples(gapPsConstFocus)),
            preCallMs(gapPsConstFocus),
            preCallMaxMs(gapPsConstFocus),
            insideCallMs(gapPsConstFocus),
            insideCallMaxMs(gapPsConstFocus));
        dxmt9DeviceInfoLog(
            "pe_recorder_gap_body_stats event=%s device=%p "
            "gapDrawIndexedVsConstFBetweenCallBodyCalls=%llu "
            "gapDrawIndexedVsConstFBetweenCallBodyCpuMs=%.3f "
            "gapDrawIndexedVsConstFBetweenCallBodyCpuMaxMs=%.3f "
            "gapDrawIndexedApplyStateBetweenCallBodyCalls=%llu "
            "gapDrawIndexedApplyStateBetweenCallBodyCpuMs=%.3f "
            "gapDrawIndexedApplyStateBetweenCallBodyCpuMaxMs=%.3f "
            "gapDrawIndexedDrawIndexedBetweenCallBodyCalls=%llu "
            "gapDrawIndexedDrawIndexedBetweenCallBodyCpuMs=%.3f "
            "gapDrawIndexedDrawIndexedBetweenCallBodyCpuMaxMs=%.3f "
            "gapDrawIndexedPsConstFBetweenCallBodyCalls=%llu "
            "gapDrawIndexedPsConstFBetweenCallBodyCpuMs=%.3f "
            "gapDrawIndexedPsConstFBetweenCallBodyCpuMaxMs=%.3f",
            event ? event : "unknown", this,
            static_cast<unsigned long long>(
                betweenCallBodyCalls(gapVsConstFocus)),
            betweenCallBodyCpuMs(gapVsConstFocus),
            betweenCallBodyCpuMaxMs(gapVsConstFocus),
            static_cast<unsigned long long>(
                betweenCallBodyCalls(gapApplyFocus)),
            betweenCallBodyCpuMs(gapApplyFocus),
            betweenCallBodyCpuMaxMs(gapApplyFocus),
            static_cast<unsigned long long>(
                betweenCallBodyCalls(gapDrawFocus)),
            betweenCallBodyCpuMs(gapDrawFocus),
            betweenCallBodyCpuMaxMs(gapDrawFocus),
            static_cast<unsigned long long>(
                betweenCallBodyCalls(gapPsConstFocus)),
            betweenCallBodyCpuMs(gapPsConstFocus),
            betweenCallBodyCpuMaxMs(gapPsConstFocus));
        dxmt9DeviceInfoLog(
            "pe_recorder_gap_tail_stats event=%s device=%p "
            "gapDrawIndexedVsConstFTailSplitSamples=%llu "
            "gapDrawIndexedVsConstFPrevCallTailMs=%.3f "
            "gapDrawIndexedVsConstFPrevCallTailMaxMs=%.3f "
            "gapDrawIndexedVsConstFBetweenCallsMs=%.3f "
            "gapDrawIndexedVsConstFBetweenCallsMaxMs=%.3f "
            "gapDrawIndexedApplyStateTailSplitSamples=%llu "
            "gapDrawIndexedApplyStatePrevCallTailMs=%.3f "
            "gapDrawIndexedApplyStatePrevCallTailMaxMs=%.3f "
            "gapDrawIndexedApplyStateBetweenCallsMs=%.3f "
            "gapDrawIndexedApplyStateBetweenCallsMaxMs=%.3f "
            "gapDrawIndexedDrawIndexedTailSplitSamples=%llu "
            "gapDrawIndexedDrawIndexedPrevCallTailMs=%.3f "
            "gapDrawIndexedDrawIndexedPrevCallTailMaxMs=%.3f "
            "gapDrawIndexedDrawIndexedBetweenCallsMs=%.3f "
            "gapDrawIndexedDrawIndexedBetweenCallsMaxMs=%.3f "
            "gapDrawIndexedPsConstFTailSplitSamples=%llu "
            "gapDrawIndexedPsConstFPrevCallTailMs=%.3f "
            "gapDrawIndexedPsConstFPrevCallTailMaxMs=%.3f "
            "gapDrawIndexedPsConstFBetweenCallsMs=%.3f "
            "gapDrawIndexedPsConstFBetweenCallsMaxMs=%.3f",
            event ? event : "unknown", this,
            static_cast<unsigned long long>(
                tailSplitSamples(gapVsConstFocus)),
            prevCallTailMs(gapVsConstFocus),
            prevCallTailMaxMs(gapVsConstFocus),
            betweenCallsMs(gapVsConstFocus),
            betweenCallsMaxMs(gapVsConstFocus),
            static_cast<unsigned long long>(tailSplitSamples(gapApplyFocus)),
            prevCallTailMs(gapApplyFocus),
            prevCallTailMaxMs(gapApplyFocus),
            betweenCallsMs(gapApplyFocus),
            betweenCallsMaxMs(gapApplyFocus),
            static_cast<unsigned long long>(tailSplitSamples(gapDrawFocus)),
            prevCallTailMs(gapDrawFocus),
            prevCallTailMaxMs(gapDrawFocus),
            betweenCallsMs(gapDrawFocus),
            betweenCallsMaxMs(gapDrawFocus),
            static_cast<unsigned long long>(tailSplitSamples(gapPsConstFocus)),
            prevCallTailMs(gapPsConstFocus),
            prevCallTailMaxMs(gapPsConstFocus),
            betweenCallsMs(gapPsConstFocus),
            betweenCallsMaxMs(gapPsConstFocus));
        dxmt9DeviceInfoLog(
            "pe_recorder_gap_between_call_stats event=%s device=%p "
            "gapDrawIndexedVsConstFBetweenTop1CallFamily=%s "
            "gapDrawIndexedVsConstFBetweenTop1Samples=%llu "
            "gapDrawIndexedVsConstFBetweenTop2CallFamily=%s "
            "gapDrawIndexedVsConstFBetweenTop2Samples=%llu "
            "gapDrawIndexedApplyStateBetweenTop1CallFamily=%s "
            "gapDrawIndexedApplyStateBetweenTop1Samples=%llu "
            "gapDrawIndexedApplyStateBetweenTop2CallFamily=%s "
            "gapDrawIndexedApplyStateBetweenTop2Samples=%llu "
            "gapDrawIndexedDrawIndexedBetweenTop1CallFamily=%s "
            "gapDrawIndexedDrawIndexedBetweenTop1Samples=%llu "
            "gapDrawIndexedDrawIndexedBetweenTop2CallFamily=%s "
            "gapDrawIndexedDrawIndexedBetweenTop2Samples=%llu "
            "gapDrawIndexedPsConstFBetweenTop1CallFamily=%s "
            "gapDrawIndexedPsConstFBetweenTop1Samples=%llu "
            "gapDrawIndexedPsConstFBetweenTop2CallFamily=%s "
            "gapDrawIndexedPsConstFBetweenTop2Samples=%llu "
            "gapDrawIndexedVsConstFBetweenTop1CallName=%s "
            "gapDrawIndexedVsConstFBetweenTop1CallNameSamples=%llu "
            "gapDrawIndexedVsConstFBetweenTop1CallNameCpuMs=%.3f "
            "gapDrawIndexedVsConstFBetweenTop1CallNameCpuMaxMs=%.3f "
            "gapDrawIndexedVsConstFBetweenTop2CallName=%s "
            "gapDrawIndexedVsConstFBetweenTop2CallNameSamples=%llu "
            "gapDrawIndexedVsConstFBetweenTop2CallNameCpuMs=%.3f "
            "gapDrawIndexedVsConstFBetweenTop2CallNameCpuMaxMs=%.3f "
            "gapDrawIndexedApplyStateBetweenTop1CallName=%s "
            "gapDrawIndexedApplyStateBetweenTop1CallNameSamples=%llu "
            "gapDrawIndexedApplyStateBetweenTop1CallNameCpuMs=%.3f "
            "gapDrawIndexedApplyStateBetweenTop1CallNameCpuMaxMs=%.3f "
            "gapDrawIndexedApplyStateBetweenTop2CallName=%s "
            "gapDrawIndexedApplyStateBetweenTop2CallNameSamples=%llu "
            "gapDrawIndexedApplyStateBetweenTop2CallNameCpuMs=%.3f "
            "gapDrawIndexedApplyStateBetweenTop2CallNameCpuMaxMs=%.3f "
            "gapDrawIndexedDrawIndexedBetweenTop1CallName=%s "
            "gapDrawIndexedDrawIndexedBetweenTop1CallNameSamples=%llu "
            "gapDrawIndexedDrawIndexedBetweenTop1CallNameCpuMs=%.3f "
            "gapDrawIndexedDrawIndexedBetweenTop1CallNameCpuMaxMs=%.3f "
            "gapDrawIndexedDrawIndexedBetweenTop2CallName=%s "
            "gapDrawIndexedDrawIndexedBetweenTop2CallNameSamples=%llu "
            "gapDrawIndexedDrawIndexedBetweenTop2CallNameCpuMs=%.3f "
            "gapDrawIndexedDrawIndexedBetweenTop2CallNameCpuMaxMs=%.3f "
            "gapDrawIndexedPsConstFBetweenTop1CallName=%s "
            "gapDrawIndexedPsConstFBetweenTop1CallNameSamples=%llu "
            "gapDrawIndexedPsConstFBetweenTop1CallNameCpuMs=%.3f "
            "gapDrawIndexedPsConstFBetweenTop1CallNameCpuMaxMs=%.3f "
            "gapDrawIndexedPsConstFBetweenTop2CallName=%s "
            "gapDrawIndexedPsConstFBetweenTop2CallNameSamples=%llu "
            "gapDrawIndexedPsConstFBetweenTop2CallNameCpuMs=%.3f "
            "gapDrawIndexedPsConstFBetweenTop2CallNameCpuMaxMs=%.3f",
            event ? event : "unknown", this,
            peInterAppendCallFamilyName(
                gapVsConstBetweenFamilies[0].family),
            static_cast<unsigned long long>(
                gapVsConstBetweenFamilies[0].samples),
            peInterAppendCallFamilyName(
                gapVsConstBetweenFamilies[1].family),
            static_cast<unsigned long long>(
                gapVsConstBetweenFamilies[1].samples),
            peInterAppendCallFamilyName(gapApplyBetweenFamilies[0].family),
            static_cast<unsigned long long>(
                gapApplyBetweenFamilies[0].samples),
            peInterAppendCallFamilyName(gapApplyBetweenFamilies[1].family),
            static_cast<unsigned long long>(
                gapApplyBetweenFamilies[1].samples),
            peInterAppendCallFamilyName(gapDrawBetweenFamilies[0].family),
            static_cast<unsigned long long>(
                gapDrawBetweenFamilies[0].samples),
            peInterAppendCallFamilyName(gapDrawBetweenFamilies[1].family),
            static_cast<unsigned long long>(
                gapDrawBetweenFamilies[1].samples),
            peInterAppendCallFamilyName(gapPsConstBetweenFamilies[0].family),
            static_cast<unsigned long long>(
                gapPsConstBetweenFamilies[0].samples),
            peInterAppendCallFamilyName(gapPsConstBetweenFamilies[1].family),
            static_cast<unsigned long long>(
                gapPsConstBetweenFamilies[1].samples),
            peInterAppendCallNameName(
                gapVsConstBetweenCallNames[0].callName),
            static_cast<unsigned long long>(
                gapVsConstBetweenCallNames[0].samples),
            static_cast<double>(gapVsConstBetweenCallNames[0].totalNs) /
                1000000.0,
            static_cast<double>(gapVsConstBetweenCallNames[0].maxNs) /
                1000000.0,
            peInterAppendCallNameName(
                gapVsConstBetweenCallNames[1].callName),
            static_cast<unsigned long long>(
                gapVsConstBetweenCallNames[1].samples),
            static_cast<double>(gapVsConstBetweenCallNames[1].totalNs) /
                1000000.0,
            static_cast<double>(gapVsConstBetweenCallNames[1].maxNs) /
                1000000.0,
            peInterAppendCallNameName(gapApplyBetweenCallNames[0].callName),
            static_cast<unsigned long long>(
                gapApplyBetweenCallNames[0].samples),
            static_cast<double>(gapApplyBetweenCallNames[0].totalNs) /
                1000000.0,
            static_cast<double>(gapApplyBetweenCallNames[0].maxNs) /
                1000000.0,
            peInterAppendCallNameName(gapApplyBetweenCallNames[1].callName),
            static_cast<unsigned long long>(
                gapApplyBetweenCallNames[1].samples),
            static_cast<double>(gapApplyBetweenCallNames[1].totalNs) /
                1000000.0,
            static_cast<double>(gapApplyBetweenCallNames[1].maxNs) /
                1000000.0,
            peInterAppendCallNameName(gapDrawBetweenCallNames[0].callName),
            static_cast<unsigned long long>(
                gapDrawBetweenCallNames[0].samples),
            static_cast<double>(gapDrawBetweenCallNames[0].totalNs) /
                1000000.0,
            static_cast<double>(gapDrawBetweenCallNames[0].maxNs) /
                1000000.0,
            peInterAppendCallNameName(gapDrawBetweenCallNames[1].callName),
            static_cast<unsigned long long>(
                gapDrawBetweenCallNames[1].samples),
            static_cast<double>(gapDrawBetweenCallNames[1].totalNs) /
                1000000.0,
            static_cast<double>(gapDrawBetweenCallNames[1].maxNs) /
                1000000.0,
            peInterAppendCallNameName(gapPsConstBetweenCallNames[0].callName),
            static_cast<unsigned long long>(
                gapPsConstBetweenCallNames[0].samples),
            static_cast<double>(gapPsConstBetweenCallNames[0].totalNs) /
                1000000.0,
            static_cast<double>(gapPsConstBetweenCallNames[0].maxNs) /
                1000000.0,
            peInterAppendCallNameName(gapPsConstBetweenCallNames[1].callName),
            static_cast<unsigned long long>(
                gapPsConstBetweenCallNames[1].samples),
            static_cast<double>(gapPsConstBetweenCallNames[1].totalNs) /
                1000000.0,
            static_cast<double>(gapPsConstBetweenCallNames[1].maxNs) /
                1000000.0);
        const auto logTransitionStats =
            [this, event](const char* prefix, const auto& transitions) {
                dxmt9DeviceInfoLog(
                    "pe_recorder_gap_transition_stats event=%s device=%p "
                    "%sBetweenGapTop1PrevFamily=%s "
                    "%sBetweenGapTop1NextFamily=%s "
                    "%sBetweenGapTop1Samples=%llu "
                    "%sBetweenGapTop1Ms=%.3f "
                    "%sBetweenGapTop1MaxMs=%.3f "
                    "%sBetweenGapTop2PrevFamily=%s "
                    "%sBetweenGapTop2NextFamily=%s "
                    "%sBetweenGapTop2Samples=%llu "
                    "%sBetweenGapTop2Ms=%.3f "
                    "%sBetweenGapTop2MaxMs=%.3f",
                    event ? event : "unknown", this,
                    prefix,
                    peInterAppendCallFamilyName(
                        transitions[0].prevFamily),
                    prefix,
                    peInterAppendCallFamilyName(
                        transitions[0].nextFamily),
                    prefix,
                    static_cast<unsigned long long>(
                        transitions[0].samples),
                    prefix,
                    static_cast<double>(transitions[0].totalNs) / 1000000.0,
                    prefix,
                    static_cast<double>(transitions[0].maxNs) / 1000000.0,
                    prefix,
                    peInterAppendCallFamilyName(
                        transitions[1].prevFamily),
                    prefix,
                    peInterAppendCallFamilyName(
                        transitions[1].nextFamily),
                    prefix,
                    static_cast<unsigned long long>(
                        transitions[1].samples),
                    prefix,
                    static_cast<double>(transitions[1].totalNs) / 1000000.0,
                    prefix,
                    static_cast<double>(transitions[1].maxNs) / 1000000.0);
            };
        logTransitionStats("gapDrawIndexedVsConstF",
                           gapVsConstBetweenTransitions);
        logTransitionStats("gapDrawIndexedApplyState",
                           gapApplyBetweenTransitions);
        logTransitionStats("gapDrawIndexedDrawIndexed",
                           gapDrawBetweenTransitions);
        logTransitionStats("gapDrawIndexedPsConstF",
                           gapPsConstBetweenTransitions);
        const auto logNameTransitionStats =
            [this, event](const char* prefix, const auto& transitions) {
                dxmt9DeviceInfoLog(
                    "pe_recorder_gap_name_transition_stats event=%s device=%p "
                    "%sBetweenGapTop1PrevCallName=%s "
                    "%sBetweenGapTop1NextCallName=%s "
                    "%sBetweenGapTop1NameSamples=%llu "
                    "%sBetweenGapTop1NameMs=%.3f "
                    "%sBetweenGapTop1NameMaxMs=%.3f "
                    "%sBetweenGapTop2PrevCallName=%s "
                    "%sBetweenGapTop2NextCallName=%s "
                    "%sBetweenGapTop2NameSamples=%llu "
                    "%sBetweenGapTop2NameMs=%.3f "
                    "%sBetweenGapTop2NameMaxMs=%.3f",
                    event ? event : "unknown", this,
                    prefix,
                    peInterAppendCallNameName(
                        transitions[0].prevCallName),
                    prefix,
                    peInterAppendCallNameName(
                        transitions[0].nextCallName),
                    prefix,
                    static_cast<unsigned long long>(
                        transitions[0].samples),
                    prefix,
                    static_cast<double>(transitions[0].totalNs) / 1000000.0,
                    prefix,
                    static_cast<double>(transitions[0].maxNs) / 1000000.0,
                    prefix,
                    peInterAppendCallNameName(
                        transitions[1].prevCallName),
                    prefix,
                    peInterAppendCallNameName(
                        transitions[1].nextCallName),
                    prefix,
                    static_cast<unsigned long long>(
                        transitions[1].samples),
                    prefix,
                    static_cast<double>(transitions[1].totalNs) / 1000000.0,
                    prefix,
                    static_cast<double>(transitions[1].maxNs) / 1000000.0);
            };
        logNameTransitionStats("gapDrawIndexedVsConstF",
                               gapVsConstBetweenNameTransitions);
        logNameTransitionStats("gapDrawIndexedApplyState",
                               gapApplyBetweenNameTransitions);
        logNameTransitionStats("gapDrawIndexedDrawIndexed",
                               gapDrawBetweenNameTransitions);
        logNameTransitionStats("gapDrawIndexedPsConstF",
                               gapPsConstBetweenNameTransitions);
        const auto logCallSiteStats =
            [this, event](const char* prefix, const auto& sites) {
                const auto callerInfo0 =
                    dxmt9PeResolveCallerModule(sites[0].callerPc);
                const auto callerInfo1 =
                    dxmt9PeResolveCallerModule(sites[1].callerPc);
                dxmt9DeviceInfoLog(
                    "pe_recorder_gap_callsite_stats event=%s device=%p "
                    "%sBetweenGapSiteTop1PrevCallName=%s "
                    "%sBetweenGapSiteTop1NextCallName=%s "
                    "%sBetweenGapSiteTop1CallerModule=%s "
                    "%sBetweenGapSiteTop1CallerRva=0x%llx "
                    "%sBetweenGapSiteTop1Samples=%llu "
                    "%sBetweenGapSiteTop1Ms=%.3f "
                    "%sBetweenGapSiteTop1MaxMs=%.3f "
                    "%sBetweenGapSiteTop2PrevCallName=%s "
                    "%sBetweenGapSiteTop2NextCallName=%s "
                    "%sBetweenGapSiteTop2CallerModule=%s "
                    "%sBetweenGapSiteTop2CallerRva=0x%llx "
                    "%sBetweenGapSiteTop2Samples=%llu "
                    "%sBetweenGapSiteTop2Ms=%.3f "
                    "%sBetweenGapSiteTop2MaxMs=%.3f",
                    event ? event : "unknown", this,
                    prefix,
                    peInterAppendCallNameName(sites[0].prevCallName),
                    prefix,
                    peInterAppendCallNameName(sites[0].nextCallName),
                    prefix,
                    dxmt9PeCallerModuleLeaf(callerInfo0),
                    prefix,
                    static_cast<unsigned long long>(callerInfo0.rva),
                    prefix,
                    static_cast<unsigned long long>(sites[0].samples),
                    prefix,
                    static_cast<double>(sites[0].totalNs) / 1000000.0,
                    prefix,
                    static_cast<double>(sites[0].maxNs) / 1000000.0,
                    prefix,
                    peInterAppendCallNameName(sites[1].prevCallName),
                    prefix,
                    peInterAppendCallNameName(sites[1].nextCallName),
                    prefix,
                    dxmt9PeCallerModuleLeaf(callerInfo1),
                    prefix,
                    static_cast<unsigned long long>(callerInfo1.rva),
                    prefix,
                    static_cast<unsigned long long>(sites[1].samples),
                    prefix,
                    static_cast<double>(sites[1].totalNs) / 1000000.0,
                    prefix,
                    static_cast<double>(sites[1].maxNs) / 1000000.0);
            };
        logCallSiteStats("gapDrawIndexedVsConstF",
                         gapVsConstBetweenCallSites);
        logCallSiteStats("gapDrawIndexedApplyState",
                         gapApplyBetweenCallSites);
        logCallSiteStats("gapDrawIndexedDrawIndexed",
                         gapDrawBetweenCallSites);
        logCallSiteStats("gapDrawIndexedPsConstF",
                         gapPsConstBetweenCallSites);
    }

    // DXMT9_PE_STATS_DECIMATION: emit ONE cumulative [dxmt9-pe-decimated]
    // line covering all four decimated hot-scope accumulators (append,
    // const_setter, const_flush, draw_packet). Counters are cumulative
    // (never reset) — estimation (sampled_ms * N / presents) is done
    // offline, not here. No-op when the knob is unset/0/unparseable.
    void logPeStatsDecimation() {
        const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
        if (decimationN == 0) {
            return;
        }
        const auto& appendStats = peV2AppendDecimatedStats_;
        const auto& constSetterStats = peConstSetterDecimatedStats();
        // Per-call register-count split of the const_setter scope. A flat
        // ns/sample across buckets means the fixed per-call entry cost
        // dominates; a slope means the per-element compare loop does.
        std::string constSetterBucketText;
        {
            const auto& buckets = peConstSetterDecimatedBuckets();
            static const char* const kNames[PeDecimatedBucketStats::kBuckets] = {
                "1", "2", "3_4", "5_8", "9_16", "gt16"};
            for (int i = 0; i < PeDecimatedBucketStats::kBuckets; ++i) {
                const auto& b = buckets.bucket[i];
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              " const_setter_n%s_events=%llu"
                              " const_setter_n%s_sampled=%llu"
                              " const_setter_n%s_sampled_ms=%.3f",
                              kNames[i], static_cast<unsigned long long>(b.events),
                              kNames[i], static_cast<unsigned long long>(b.sampled),
                              kNames[i], static_cast<double>(b.sampledNs) / 1.0e6);
                constSetterBucketText += buf;
            }
        }
        std::string appendTypeText;
        {
            static const char* const kTypeNames[kPeAppendTypeBuckets] = {
                "draw", "drawidx", "drawup", "applystate",
                "vsconst", "psconst", "clear", "other"};
            for (std::size_t i = 0; i < kPeAppendTypeBuckets; ++i) {
                if (peAppendTypeCounts_[i] == 0) continue;
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              " append_%s_calls=%llu append_%s_bytes=%llu",
                              kTypeNames[i],
                              static_cast<unsigned long long>(peAppendTypeCounts_[i]),
                              kTypeNames[i],
                              static_cast<unsigned long long>(peAppendTypeBytes_[i]));
                appendTypeText += buf;
            }
        }
        dxmt9DeviceInfoLog(
            "[dxmt9-pe-decimated] presents=%llu decimation=%u "
            "append_events=%llu append_sampled=%llu append_sampled_ms=%.3f "
            "const_setter_events=%llu const_setter_sampled=%llu const_setter_sampled_ms=%.3f "
            "const_flush_events=%llu const_flush_sampled=%llu const_flush_sampled_ms=%.3f "
            "draw_packet_events=%llu draw_packet_sampled=%llu draw_packet_sampled_ms=%.3f "
            "identity_getter_calls=%llu null_scope_sampled=%llu null_scope_ms=%.3f "
            "append_resize_sampled=%llu append_resize_ms=%.3f "
            "append_write_sampled=%llu append_write_ms=%.3f "
            "append_encode_sampled=%llu append_encode_ms=%.3f "
            "append_flush_sampled=%llu append_flush_ms=%.3f%s%s",
            static_cast<unsigned long long>(peStatsDecimationPresents_),
            decimationN,
            static_cast<unsigned long long>(appendStats.events),
            static_cast<unsigned long long>(appendStats.sampled),
            static_cast<double>(appendStats.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(constSetterStats.events),
            static_cast<unsigned long long>(constSetterStats.sampled),
            static_cast<double>(constSetterStats.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peConstFlushDecimatedStats_.events),
            static_cast<unsigned long long>(peConstFlushDecimatedStats_.sampled),
            static_cast<double>(peConstFlushDecimatedStats_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peDrawPacketDecimatedStats_.events),
            static_cast<unsigned long long>(peDrawPacketDecimatedStats_.sampled),
            static_cast<double>(peDrawPacketDecimatedStats_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(
                dxmt9::d3d9::pe::wireIdentityGetterCallCount()),
            static_cast<unsigned long long>(peDecimatedNullScopeStats().sampled),
            static_cast<double>(peDecimatedNullScopeStats().sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peAppendPhaseResize_.sampled),
            static_cast<double>(peAppendPhaseResize_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peAppendPhaseWrite_.sampled),
            static_cast<double>(peAppendPhaseWrite_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peAppendPhaseEncode_.sampled),
            static_cast<double>(peAppendPhaseEncode_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peAppendPhaseFlush_.sampled),
            static_cast<double>(peAppendPhaseFlush_.sampledNs) / 1.0e6,
            constSetterBucketText.c_str(),
            appendTypeText.c_str());
    }

    // Present-cadence tick for the decimated dump: increments a cumulative
    // present counter and emits the cumulative line every 60 presents. A
    // final line is also emitted unconditionally from the destructor so the
    // last partial interval is never lost. No-op when decimation is off.
    void notePeStatsDecimationPresent() {
        if (dxmt9PeStatsDecimationN() == 0) {
            return;
        }
        ++peStatsDecimationPresents_;
        if (peStatsDecimationPresents_ % 60 == 0) {
            logPeStatsDecimation();
        }
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

    HRESULT commitPendingCommandChunk(PeRecorderFlushReason commitReason,
                                      const D9CCommandChunk& chunk,
                                      const PeCommandChunkCommitInfo& info) {
                auto chunkCadence = claimPeFirstChunkAfterPresent();
                const std::int64_t entryNs =
                    dxmt9SteadyClockNs(std::chrono::steady_clock::now());
                const std::int64_t priorReturnNs = peRecorderLastChunkReturnNs_;
                const HRESULT hr = hr32(dxmt9c_device_commit_chunk(dev_, &chunk));
                const std::int64_t returnNs =
                    dxmt9SteadyClockNs(std::chrono::steady_clock::now());
                peRecorderLastChunkReturnNs_ = returnNs;
                const std::uint64_t fillGapNs =
                    priorReturnNs > 0 && entryNs > priorReturnNs
                    ? static_cast<std::uint64_t>(entryNs - priorReturnNs)
                    : 0;
                const std::uint64_t activeFillNs =
                    peRecorderCurrentChunkFirstAppendNs_ > 0 &&
                    entryNs > peRecorderCurrentChunkFirstAppendNs_
                    ? static_cast<std::uint64_t>(
                        entryNs - peRecorderCurrentChunkFirstAppendNs_)
                    : 0;
                const std::uint64_t bridgeNs =
                    returnNs > entryNs
                    ? static_cast<std::uint64_t>(returnNs - entryNs)
                    : 0;
                if (SUCCEEDED(hr)) {
                    peRecorderCurrentChunkFirstAppendNs_ = 0;
                    peRecorderLastAppendReturnNs_ = 0;
                    peRecorderLastAppendCallEntryNs_ = 0;
                    peRecorderLastAppendCallExitNs_ = 0;
                    peRecorderLastAppendRecordType_ = 0;
                    resetPeBetweenCallsWindow();
                }
                logPeFirstChunkAfterPresent(commitReason, chunkCadence, hr, info);
                if (SUCCEEDED(hr)) {
                    ++commandChunkCommits_;
                    commandChunkRecords_ += info.recordCount;
                    commandChunkBytes_ += info.wireBytes;
                    recordPeChunkCommit(commitReason, info.recordCount,
                                        info.payloadBytes, info.handleCount,
                                        info.wireBytes, fillGapNs,
                                        activeFillNs, bridgeNs);
                }
                return hr;
    }

    HRESULT flushPendingCommandChunk(
        PeRecorderFlushReason reason = PeRecorderFlushReason::Explicit) {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        if (!commandChunkNegotiated_) {
            return D3DERR_NOTAVAILABLE;
        }
        if (commandChunkV2_.recordCount() == 0u) {
            return S_OK;
        }
        const auto payloadBytes = commandChunkV2_.payloadBytes();
        const auto sealed = commandChunkV2_.seal();
        if (!sealed.valid() || sealed.blob.size() > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        D9CCommandChunk chunk{};
        chunk.version = D9C_COMMAND_CHUNK_VERSION_V2;
        chunk.recordCount = sealed.recordCount;
        chunk.recordBytes = static_cast<std::uint32_t>(sealed.blob.size());
        chunk.records = toWireHandle(sealed.blob.data());
        chunk.handleCount = sealed.handleCount;
        const PeCommandChunkCommitInfo info{
            .recordCount = sealed.recordCount,
            .payloadBytes = static_cast<std::uint32_t>(std::min<std::size_t>(
                payloadBytes, std::numeric_limits<std::uint32_t>::max())),
            .handleCount = sealed.handleCount,
            .wireBytes = chunk.recordBytes,
        };
        const HRESULT hr = commitPendingCommandChunk(reason, chunk, info);
        if (SUCCEEDED(hr)) {
            commandChunkV2_.reset();
        }
        return hr;
    }

    // Phase timer handed to an appendRecordV2 emitter. The envelope owns the
    // decimation sampling decision, so an emitter that has its own phases to
    // attribute (the legacy adapter's resize and write) records them through
    // this rather than re-deriving `phaseSampled`.
    struct AppendPhaseTimer {
        bool sampled = false;

        static std::chrono::steady_clock::time_point now() noexcept {
            return std::chrono::steady_clock::now();
        }
        void record(PeDecimatedScopeStats& stats,
                    std::chrono::steady_clock::time_point t0) const noexcept {
            if (!sampled) {
                return;
            }
            PeDecimatedScopeTimer::recordSample(
                stats, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now() - t0).count()));
        }
    };

    // Append envelope. Owns the recorder mutex, the negotiation gate, the
    // CapacityPre and CapacityPost flushes, and every append telemetry site;
    // `emit` supplies only the record itself.
    //
    // `sizeHint` feeds the capacity precheck and therefore decides where
    // chunks are cut. A caller that no longer builds a legacy record must
    // still pass a value on the same scale as the legacy record it replaced,
    // or chunk seal cadence shifts -- which no per-record test can observe.
    //
    // `emit` is invoked as HRESULT(CommandChunkV2Builder&, const AppendPhaseTimer&).
    // It returns an HRESULT rather than bool so an emitter can distinguish
    // E_OUTOFMEMORY from a malformed record.
    //
    // The return type is asserted, not merely documented, because HRESULT is
    // LONG: an emitter that returned the V2 emitters' natural `bool` would
    // convert `false` to 0 == S_OK, so a failed append would be reported as a
    // success. CapacityPost would run, notePeChunkAppendBoundary would count a
    // record that was never appended, and the record would vanish with no
    // error. Emitters must spell out `? S_OK : D3DERR_INVALIDCALL`.
    template<typename EmitFn>
    HRESULT appendRecordV2(uint32_t type, size_t sizeHint, EmitFn emit) {
        static_assert(
            std::is_same_v<
                decltype(emit(std::declval<dxmt9::d3d9::pe::CommandChunkV2Builder&>(),
                              std::declval<const AppendPhaseTimer&>())),
                HRESULT>,
            "appendRecordV2 emitters must return HRESULT, not bool: a bool "
            "false would silently convert to S_OK");
        const size_t bytes = sizeHint;
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        if (!commandChunkNegotiated_ || bytes == 0u || bytes > 0xffffffffull) {
            return commandChunkNegotiated_ ? D3DERR_INVALIDCALL
                                           : D3DERR_NOTAVAILABLE;
        }
        const auto maxRecords = maxPendingCommandRecords();
        const auto maxBytes = maxPendingCommandBytes();
        const auto recordCountBefore = commandChunkV2_.recordCount();
        const auto payloadBytesBefore = commandChunkV2_.payloadBytes();
        const bool willFlushBeforeAppend = recordCountBefore != 0u &&
            (recordCountBefore >= maxRecords ||
             payloadBytesBefore + bytes > maxBytes);
        const std::uint32_t appendedRecordCount =
            willFlushBeforeAppend ? 1u : recordCountBefore + 1u;
        const auto appendedPayloadBytes =
            willFlushBeforeAppend ? bytes : payloadBytesBefore + bytes;
        const bool willFlushAfterAppend =
            appendedRecordCount >= maxRecords || appendedPayloadBytes >= maxBytes;
        const bool noFlushAppend =
            !willFlushBeforeAppend && !willFlushAfterAppend;
        const auto appendEntryNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        recordPeChunkInterAppendGap(appendEntryNs, recordCountBefore, type);
        HRESULT hr = S_OK;
        DxmtPeDecimatedScopeGuard appendDecimatedScope;
        const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
        if (decimationN != 0 &&
            PeDecimatedScopeTimer::shouldSample(
                peV2AppendDecimatedStats_, decimationN)) {
            appendDecimatedScope.stats = &peV2AppendDecimatedStats_;
            {
                const auto n0 = std::chrono::steady_clock::now();
                const auto n1 = std::chrono::steady_clock::now();
                PeDecimatedScopeTimer::recordSample(
                    peDecimatedNullScopeStats(),
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(n1 - n0).count()));
            }
            appendDecimatedScope.t0 = std::chrono::steady_clock::now();
        }
        if (decimationN != 0) {
            const auto typeBucket = peAppendTypeBucket(type);
            ++peAppendTypeCounts_[typeBucket];
            peAppendTypeBytes_[typeBucket] += bytes;
        }
        const bool phaseSampled = appendDecimatedScope.stats != nullptr;
        const auto phaseNow = [] { return std::chrono::steady_clock::now(); };
        const auto phaseRecord = [&](PeDecimatedScopeStats& stats,
                                     std::chrono::steady_clock::time_point t0) {
            if (!phaseSampled) return;
            PeDecimatedScopeTimer::recordSample(
                stats, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        phaseNow() - t0).count()));
        };
        if (willFlushBeforeAppend) {
            const auto t0 = phaseNow();
            hr = flushPendingCommandChunk(PeRecorderFlushReason::CapacityPre);
            phaseRecord(peAppendPhaseFlush_, t0);
        }
        if (SUCCEEDED(hr)) {
            // The emitter records peAppendPhaseEncode_ itself, around its own
            // record emission only. Timing the whole callable here instead
            // would silently redefine `encode` to include the legacy adapter's
            // resize and write, and `encode` is the figure the migration is
            // measured against -- see
            // docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.01.md.
            hr = emit(commandChunkV2_, AppendPhaseTimer{phaseSampled});
        }
        if (SUCCEEDED(hr) &&
            (commandChunkV2_.recordCount() >= maxRecords ||
             commandChunkV2_.payloadBytes() >= maxBytes)) {
            const auto t0 = phaseNow();
            hr = flushPendingCommandChunk(PeRecorderFlushReason::CapacityPost);
            phaseRecord(peAppendPhaseFlush_, t0);
        }
        const auto appendReturnNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        if (appendReturnNs > appendEntryNs) {
            recordPeAppendCpu(
                static_cast<std::uint64_t>(appendReturnNs - appendEntryNs),
                noFlushAppend);
        }
        if (SUCCEEDED(hr)) {
            notePeChunkAppendBoundary(appendReturnNs, type);
            logPeRecordMilestoneAfterPresent(
                type, appendedRecordCount,
                static_cast<std::uint32_t>(std::min<std::size_t>(
                    appendedPayloadBytes, std::numeric_limits<std::uint32_t>::max())),
                appendEntryNs);
        }
        return hr;
    }

    // Legacy adapter over appendRecordV2: build the record into the scratch
    // buffer in the legacy wire format, then re-parse and re-encode it as V2.
    //
    // All three phase timers stay exactly where they were before the envelope
    // was extracted -- resize around the scratch grow, write around the
    // caller's writer, encode around appendLegacyCommandRecordAsV2 alone. A
    // migrated emitter records encode around its direct V2 call, so `encode`
    // keeps one meaning across the migration and the before/after numbers stay
    // comparable.
    //
    // Every caller of this is a record family not yet migrated to a direct V2
    // emitter. When the last one moves, this and appendLegacyCommandRecordAsV2
    // go away together.
    template<typename WriteFn>
    HRESULT appendCommandRecordDirect(uint32_t type, size_t bytes, WriteFn write) {
        return appendRecordV2(
            type, bytes,
            [this, bytes, &write](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                                  const AppendPhaseTimer& phase) -> HRESULT {
                {
                    const auto t0 = AppendPhaseTimer::now();
                    try {
                        legacyV2RecordScratch_.resize(bytes);
                    } catch (...) {
                        phase.record(peAppendPhaseResize_, t0);
                        return E_OUTOFMEMORY;
                    }
                    phase.record(peAppendPhaseResize_, t0);
                }
                {
                    const auto t0 = AppendPhaseTimer::now();
                    write(reinterpret_cast<std::uint8_t*>(
                        legacyV2RecordScratch_.data()));
                    phase.record(peAppendPhaseWrite_, t0);
                }
                const auto t0 = AppendPhaseTimer::now();
                const bool encoded = dxmt9::d3d9::pe::appendLegacyCommandRecordAsV2(
                    builder, legacyV2RecordScratch_);
                phase.record(peAppendPhaseEncode_, t0);
                return encoded ? S_OK : D3DERR_INVALIDCALL;
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


    HRESULT appendDrawPrimitiveRecord(D3DPRIMITIVETYPE type, UINT startVertex, UINT count) {
        Dxmt9PeAppendFamilyScope appendFamily(PeInterAppendCallFamily::Draw);
        // Hold the recorder lock across the const-flush/fold + draw-record
        // append pair: recorderMutex_ is recursive, so the nested per-append
        // acquisitions below become cheap re-entries instead of repeated
        // cold lock/unlock cycles on this hot path.
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const bool inlineConstDelta = dxmt9PeInlineConstDeltaEnabled();
        if (!inlineConstDelta) {
            // Drain any accumulated const dirty ranges into chunk records
            // FIRST, so the chunk replays "consts → draw" in API order.
            const HRESULT constHr = flushPendingConsts();
            if (FAILED(constHr)) return constHr;
        }
        D9CCommandRecordDrawPrimitive record{};
        record.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
        if (!buildDrawPrimitivePacket(type, startVertex, count, record.packet)) {
            return D3DERR_INVALIDCALL;
        }
        const auto streamSources = currentDrawStreamSources();
        HRESULT hr;
        if (inlineConstDelta) {
            // R-BACK-2.52(b): fold the pending const shadows into
            // record.packet.constDeltaSections + a trailing payload region
            // instead of standalone records. Must run AFTER
            // buildDrawPrimitivePacket, which resets `record.packet`
            // (clobbering constDeltaSections) before filling it in.
            std::uint32_t payloadBytes = 0;
            const HRESULT foldHr =
                foldPendingConstsIntoDrawPacket(record.packet, payloadBytes);
            if (FAILED(foldHr)) return foldHr;
            record.header.size =
                d9c_command_record_draw_primitive_total_size(&record.packet);
            hr = appendCommandRecordDirect(
                record.header.type, record.header.size,
                [this, &record, &streamSources, payloadBytes](std::uint8_t* dst) {
                    populatePendingChunkDrawStreamDependencies(
                        record.packet, streamSources);
                    std::memcpy(dst, &record, sizeof(record));
                    if (payloadBytes != 0) {
                        std::memcpy(dst + sizeof(record),
                                    constDeltaPayloadScratch_.data(),
                                    payloadBytes);
                    }
                });
        } else {
            record.header.size = sizeof(record);
            hr = appendCommandRecordDirect(
                record.header.type, record.header.size,
                [this, &record, &streamSources](std::uint8_t* dst) {
                    populatePendingChunkDrawStreamDependencies(
                        record.packet, streamSources);
                    std::memcpy(dst, &record, sizeof(record));
                });
        }
        return hr;
    }

    HRESULT appendDrawIndexedPrimitiveRecord(D3DPRIMITIVETYPE type,
                                             INT baseVertex,
                                             UINT minVertex,
                                             UINT numVertices,
                                             UINT startIndex,
                                             UINT count) {
        Dxmt9PeAppendFamilyScope appendFamily(PeInterAppendCallFamily::Draw);
        // See appendDrawPrimitiveRecord: recursive re-entry on an
        // already-held recorderMutex_ is cheaper than the repeated cold
        // acquisitions the nested const-flush + draw appends would do.
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const bool inlineConstDelta = dxmt9PeInlineConstDeltaEnabled();
        if (!inlineConstDelta) {
            const HRESULT constHr = flushPendingConsts();
            if (FAILED(constHr)) return constHr;
        }
        D9CCommandRecordDrawIndexedPrimitive record{};
        record.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
        if (!buildDrawPrimitivePacket(type, 0, count, record.packet.state)) {
            return D3DERR_INVALIDCALL;
        }
        const auto streamSources = currentDrawStreamSources();
        record.packet.baseVertex = baseVertex;
        record.packet.minVertex = minVertex;
        record.packet.numVertices = numVertices;
        record.packet.startIndex = startIndex;
        record.packet.primitiveCount = count;
        // Preserve the semantic IB delta. The append-time dependency
        // checkpoint below additionally emits an unchanged IB when the new
        // chunk has not retained it yet.
        record.packet.ibHandle = toWireHandle(rawIBuf(indexBuf_));
        const std::uint64_t ibWireValue =
            d9cWireHandleValue(record.packet.ibHandle);
        record.packet.ibValid =
            (peState_.pendingIb || !submittedIndexBufferKnown_ ||
             submittedIndexBufferWireValue_ != ibWireValue) ? 1u : 0u;
        HRESULT hr;
        if (inlineConstDelta) {
            // R-BACK-2.52(b): see appendDrawPrimitiveRecord. Folds into
            // record.packet.state.constDeltaSections — the shared
            // D9CDrawPrimitivePacket embedded in the indexed packet.
            std::uint32_t payloadBytes = 0;
            const HRESULT foldHr = foldPendingConstsIntoDrawPacket(
                record.packet.state, payloadBytes);
            if (FAILED(foldHr)) return foldHr;
            record.header.size =
                d9c_command_record_draw_indexed_primitive_total_size(
                    &record.packet.state);
            hr = appendCommandRecordDirect(
                record.header.type, record.header.size,
                [this, &record, &streamSources, payloadBytes](std::uint8_t* dst) {
                    populatePendingChunkDrawStreamDependencies(
                        record.packet.state, streamSources);
                    populatePendingChunkDrawIndexDependency(record.packet);
                    std::memcpy(dst, &record, sizeof(record));
                    if (payloadBytes != 0) {
                        std::memcpy(dst + sizeof(record),
                                    constDeltaPayloadScratch_.data(),
                                    payloadBytes);
                    }
                });
        } else {
            record.header.size = sizeof(record);
            hr = appendCommandRecordDirect(
                record.header.type, record.header.size,
                [this, &record, &streamSources](std::uint8_t* dst) {
                    populatePendingChunkDrawStreamDependencies(
                        record.packet.state, streamSources);
                    populatePendingChunkDrawIndexDependency(record.packet);
                    std::memcpy(dst, &record, sizeof(record));
                });
        }
        if (SUCCEEDED(hr)) {
            if (record.packet.ibValid != 0) {
                submittedIndexBufferWireValue_ = ibWireValue;
                submittedIndexBufferKnown_ = true;
            }
            peState_.pendingIb = false;
        }
        return hr;
    }

    bool pendingChunkReferencesBuffer(D9CWireHandle handle) const {
        const std::uint64_t value = d9cWireHandleValue(handle);
        if (value == 0u) {
            return false;
        }
        return commandChunkV2_.referencesObject(reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(value)));
    }

    void populatePendingChunkDrawStreamDependencies(
        D9CDrawPrimitivePacket& packet,
        const dxmt9::d3d9::pe::PeStreamSources& streamSources) const {
        std::uint32_t retainedStreamMask = 0u;
        for (DWORD slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
            if (pendingChunkReferencesBuffer(streamSources[slot].buffer)) {
                retainedStreamMask |= 1u << slot;
            }
        }
        // CapacityPre has already sealed the old chunk before the writer is
        // called. Therefore this deterministic transform checkpoints only
        // streams absent from the actual destination chunk. The serialized
        // packet remains the sole source of retention semantics
        // (R-CORE-11.17), while later draws can still coalesce.
        dxmt9::d3d9::pe::populateDrawPacketStreamDependencies(
            packet, streamSources, retainedStreamMask);
    }

    void populatePendingChunkDrawIndexDependency(
        D9CDrawIndexedPrimitivePacket& packet) const {
        dxmt9::d3d9::pe::populateDrawPacketIndexDependency(
            packet, pendingChunkReferencesBuffer(packet.ibHandle));
    }

    HRESULT appendDrawPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                        UINT count,
                                        const void* data,
                                        UINT stride) {
        return appendDrawPrimitiveUPRecordWithFvf(type, count, data, stride,
                                                  false, 0);
    }

    HRESULT appendDrawPrimitiveUPRecordWithFvf(D3DPRIMITIVETYPE type,
                                               UINT count,
                                               const void* data,
                                               UINT stride,
                                               bool overrideFvf,
                                               DWORD packetFvf,
                                               bool overrideVertexShaderNull = false,
                                               bool forceFullSnapshot = false) {
        Dxmt9PeAppendFamilyScope appendFamily(PeInterAppendCallFamily::Draw);
        // See appendDrawPrimitiveRecord: recursive re-entry on an
        // already-held recorderMutex_ is cheaper than the repeated cold
        // acquisitions the nested const-flush + draw appends would do.
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawPrimitiveUP header{};
        header.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
        const DWORD savedFvf = fvf_;
        IDirect3DVertexDeclaration9* savedVdecl = vdecl_;
        IDirect3DVertexShader9* savedVs = vs_;
        const bool savedPendingFvf = peState_.pendingFvf;
        const bool savedPendingVdecl = peState_.pendingVdecl;
        const bool savedPendingVs = peState_.pendingVs;
        if (overrideFvf) {
            fvf_ = packetFvf;
            vdecl_ = implicitDeclForFvf(packetFvf);
            peState_.pendingFvf = true;
            peState_.pendingVdecl = true;
        }
        if (overrideVertexShaderNull) {
            vs_ = nullptr;
            peState_.pendingVs = true;
        }
        if (!buildDrawPrimitivePacket(type, 0, count, header.packet.state,
                forceFullSnapshot)) {
            if (overrideFvf) {
                fvf_ = savedFvf;
                vdecl_ = savedVdecl;
                peState_.pendingFvf = savedPendingFvf;
                peState_.pendingVdecl = savedPendingVdecl;
            }
            if (overrideVertexShaderNull) {
                vs_ = savedVs;
                peState_.pendingVs = savedPendingVs;
            }
            return D3DERR_INVALIDCALL;
        }
        if (overrideFvf) {
            fvf_ = savedFvf;
            vdecl_ = savedVdecl;
            peState_.pendingFvf = savedPendingFvf;
            peState_.pendingVdecl = savedPendingVdecl;
        }
        if (overrideVertexShaderNull) {
            vs_ = savedVs;
            peState_.pendingVs = savedPendingVs;
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
        return appendDrawIndexedPrimitiveUPRecordWithFvf(
            type, minVertex, numVertices, count, indexData, indexFormat,
            vertexData, stride, false, 0);
    }

    HRESULT appendDrawIndexedPrimitiveUPRecordWithFvf(D3DPRIMITIVETYPE type,
                                                      UINT minVertex,
                                                      UINT numVertices,
                                                      UINT count,
                                                      const void* indexData,
                                                      D3DFORMAT indexFormat,
                                                      const void* vertexData,
                                                      UINT stride,
                                                      bool overrideFvf,
                                                      DWORD packetFvf,
                                                      bool overrideVertexShaderNull = false,
                                                      bool forceFullSnapshot = false) {
        Dxmt9PeAppendFamilyScope appendFamily(PeInterAppendCallFamily::Draw);
        // See appendDrawPrimitiveRecord: recursive re-entry on an
        // already-held recorderMutex_ is cheaper than the repeated cold
        // acquisitions the nested const-flush + draw appends would do.
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawIndexedPrimitiveUP header{};
        header.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
        const DWORD savedFvf = fvf_;
        IDirect3DVertexDeclaration9* savedVdecl = vdecl_;
        IDirect3DVertexShader9* savedVs = vs_;
        const bool savedPendingFvf = peState_.pendingFvf;
        const bool savedPendingVdecl = peState_.pendingVdecl;
        const bool savedPendingVs = peState_.pendingVs;
        if (overrideFvf) {
            fvf_ = packetFvf;
            vdecl_ = implicitDeclForFvf(packetFvf);
            peState_.pendingFvf = true;
            peState_.pendingVdecl = true;
        }
        if (overrideVertexShaderNull) {
            vs_ = nullptr;
            peState_.pendingVs = true;
        }
        if (!buildDrawPrimitivePacket(type, 0, count, header.packet.state,
                forceFullSnapshot)) {
            if (overrideFvf) {
                fvf_ = savedFvf;
                vdecl_ = savedVdecl;
                peState_.pendingFvf = savedPendingFvf;
                peState_.pendingVdecl = savedPendingVdecl;
            }
            if (overrideVertexShaderNull) {
                vs_ = savedVs;
                peState_.pendingVs = savedPendingVs;
            }
            return D3DERR_INVALIDCALL;
        }
        if (overrideFvf) {
            fvf_ = savedFvf;
            vdecl_ = savedVdecl;
            peState_.pendingFvf = savedPendingFvf;
            peState_.pendingVdecl = savedPendingVdecl;
        }
        if (overrideVertexShaderNull) {
            vs_ = savedVs;
            peState_.pendingVs = savedPendingVs;
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

        // sizeHint keeps the legacy header+payload size the capacity precheck
        // saw before, so chunk seal cadence is unchanged. Both guards above are
        // untouched, which is why flushConstShadow's DXMT9_SPLIT_SPARSE_CONST_
        // RECORDS diagnostic path and its telemetry need no changes: this is
        // the single emitter behind all six VS/PS constant kinds.
        return appendRecordV2(
            recordType,
            sizeof(D9CCommandRecordSetConst) + payloadBytes,
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendSetConstantsV2(
                    builder, recordType, start, count,
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(data), payloadBytes));
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
    }

    static bool constShadowElemEquals(const ConstShadow& shadow,
                                      std::uint32_t reg,
                                      const std::uint8_t* src,
                                      std::size_t elemSize) {
        const std::size_t offset = static_cast<std::size_t>(reg) * elemSize;
        if (shadow.values.size() >= offset + elemSize) {
            return std::memcmp(shadow.values.data() + offset, src, elemSize) == 0;
        }
        for (std::size_t i = 0; i < elemSize; ++i) {
            if (src[i] != 0u) {
                return false;
            }
        }
        return true;
    }

    static VsConstRangeChange analyzeConstShadowChange(
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

    static std::uint32_t countDirtyConstRegs(const ConstShadow& shadow,
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

    static std::vector<std::pair<uint32_t, uint32_t>>
    collectConstDirtyRuns(const ConstShadow& shadow) {
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        if (!shadow.dirty()) return runs;
        uint32_t runStart = 0;
        bool inRun = false;
        const uint32_t dirtyEnd = std::min<uint32_t>(
            shadow.dirtyEnd, static_cast<uint32_t>(shadow.dirtyElems.size()));
        for (uint32_t reg = shadow.dirtyStart; reg < dirtyEnd; ++reg) {
            const bool dirty = shadow.dirtyElems[reg] != 0;
            if (dirty && !inRun) {
                runStart = reg;
                inRun = true;
            } else if (!dirty && inRun) {
                runs.emplace_back(runStart, reg);
                inRun = false;
            }
        }
        if (inRun) {
            runs.emplace_back(runStart, dirtyEnd);
        }
        return runs;
    }

    // Emit records covering pending dirty constants, then clear them. Default
    // behavior keeps the historical merged range. DXMT9_SPLIT_SPARSE_CONST_RECORDS
    // is an opt-in perf experiment for sparse constant updates.
    HRESULT flushConstShadow(ConstShadow& shadow, uint32_t recordType, std::size_t elemSize) {
        // Decimated timing (const_flush scope): sample only every Nth call
        // so the CPU cost of measuring is itself negligible. Independent of
        // DXMT9_PE_RECORDER_STATS. Guard covers every exit path (including
        // the early "not dirty" return below) via RAII.
        DxmtPeDecimatedScopeGuard decimatedScope;
        const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
        if (decimationN != 0 &&
            PeDecimatedScopeTimer::shouldSample(peConstFlushDecimatedStats_, decimationN)) {
            decimatedScope.stats = &peConstFlushDecimatedStats_;
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
        if (!shadow.dirty()) return S_OK;
        const std::int64_t flushEntryNs = dxmt9PeRecorderStatsEnabled()
            ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
            : 0;
        HRESULT hr = S_OK;
        std::uint32_t flushedRecords = 0u;
        std::uint32_t flushedRegs = 0u;
        // Emits one D9C_COMMAND_RECORD_SET_*_CONST_* record for [start, end)
        // and updates the flush counters above; shared by both branches
        // below so their per-run bodies stay byte-for-byte identical.
        const auto emitRun = [&](uint32_t start, uint32_t end) {
            if (end <= start) return;
            const uint32_t count = end - start;
            const auto* data =
                shadow.values.data() + static_cast<std::size_t>(start) * elemSize;
            hr = appendSetConstRecord(recordType, start, count, data, elemSize);
            if (SUCCEEDED(hr)) {
                ++flushedRecords;
                flushedRegs += count;
            }
            if (SUCCEEDED(hr) && recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F) {
                const std::uint32_t dirtyRegs =
                    countDirtyConstRegs(shadow, start, end);
                recordVsConstSetterRange(VsConstSetterRangePhase::Flush,
                                         currentVertexShaderHash(),
                                         currentPixelShaderHash(),
                                         start, count, dirtyRegs, count);
            }
        };
        if (dxmt9SplitSparseConstRecordsEnabled()) {
            // Diagnostic path: unchanged vector-of-runs logic.
            std::vector<std::pair<uint32_t, uint32_t>> runs =
                collectConstDirtyRuns(shadow);
            if (runs.empty()) {
                runs.emplace_back(shadow.dirtyStart, shadow.dirtyEnd);
            }
            for (const auto& [start, end] : runs) {
                emitRun(start, end);
                if (FAILED(hr)) break;
            }
        } else {
            // Default path: the historical merged range is always exactly
            // one run, so emit it directly without building a std::vector.
            emitRun(shadow.dirtyStart, shadow.dirtyEnd);
        }
        recordPeConstFlushCpu(recordType, flushEntryNs, flushedRecords,
                              flushedRegs);
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

    // R-BACK-2.52 (Inline Const Delta fold, DXMT9_PE_INLINE_CONST_DELTA=1
    // only): fold all six const shadows' merged dirty ranges into
    // `packet.constDeltaSections` plus this call's payload bytes in
    // constDeltaPayloadScratch_, instead of appending standalone
    // D9C_COMMAND_RECORD_SET_*_CONST_* records for them. Called by
    // appendDrawPrimitiveRecord / appendDrawIndexedPrimitiveRecord AFTER
    // buildDrawPrimitivePacket() has already reset `packet` — folding
    // earlier would be clobbered by that reset.
    //
    // Each shadow's fold uses foldConstShadowIntoDeltaSection
    // (d3d9_pe_const_shadow.hpp), which is required to reproduce the exact
    // merged [dirtyStart, dirtyEnd) range and element size that
    // flushConstShadow's default path would have emitted as a standalone
    // record — this is what preserves R-BACK-2.52(d) replay equivalence.
    // If a shadow's fold fails (defensive-only: the range check should be
    // unreachable because Set* fast paths already bound ranges to the
    // D3D9 register-file limit before touchConstShadow runs), that single
    // shadow falls back to flushConstShadow so its bytes still reach the
    // chunk as a standalone record placed before the draw record — the
    // draw record is not appended by this function, so chunk order still
    // ends up "consts → draw" for the fallback case.
    HRESULT foldPendingConstsIntoDrawPacket(D9CDrawPrimitivePacket& packet,
                                            std::uint32_t& payloadBytes) {
        payloadBytes = 0;
        struct FoldEntry {
            ConstShadow* shadow;
            uint32_t kind;
            uint32_t recordType;
            std::size_t elemSize;
        };
        const std::array<FoldEntry, D9C_DRAW_PACKET_CONST_DELTA_COUNT> entries{{
            {&peConsts_.vsConstF, D9C_DRAW_PACKET_CONST_DELTA_VS_F,
             D9C_COMMAND_RECORD_SET_VS_CONST_F, sizeof(float) * 4},
            {&peConsts_.vsConstI, D9C_DRAW_PACKET_CONST_DELTA_VS_I,
             D9C_COMMAND_RECORD_SET_VS_CONST_I, sizeof(int32_t) * 4},
            {&peConsts_.vsConstB, D9C_DRAW_PACKET_CONST_DELTA_VS_B,
             D9C_COMMAND_RECORD_SET_VS_CONST_B, sizeof(uint32_t)},
            {&peConsts_.psConstF, D9C_DRAW_PACKET_CONST_DELTA_PS_F,
             D9C_COMMAND_RECORD_SET_PS_CONST_F, sizeof(float) * 4},
            {&peConsts_.psConstI, D9C_DRAW_PACKET_CONST_DELTA_PS_I,
             D9C_COMMAND_RECORD_SET_PS_CONST_I, sizeof(int32_t) * 4},
            {&peConsts_.psConstB, D9C_DRAW_PACKET_CONST_DELTA_PS_B,
             D9C_COMMAND_RECORD_SET_PS_CONST_B, sizeof(uint32_t)},
        }};
        for (const auto& entry : entries) {
            auto& section = packet.constDeltaSections[entry.kind];
            const bool folded = foldConstShadowIntoDeltaSection(
                *entry.shadow, entry.kind, entry.elemSize, section,
                constDeltaPayloadScratch_.data(),
                constDeltaPayloadScratch_.size(), payloadBytes);
            if (!folded) {
                const HRESULT hr = flushConstShadow(
                    *entry.shadow, entry.recordType, entry.elemSize);
                if (FAILED(hr)) return hr;
            }
        }
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
        Dxmt9PeAppendFamilyScope appendFamily(PeInterAppendCallFamily::Barrier);
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const std::int64_t constEntryNs = dxmt9PeRecorderStatsEnabled()
            ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
            : 0;
        const HRESULT constHr = flushPendingConsts();
        recordPeChunkBarrierConstCpu(constEntryNs);
        if (FAILED(constHr)) return constHr;
        if (!hasPendingHotState()) {
            return S_OK;
        }
        // Fast path: single APPLY_STATE record covers all pending
        // state. After Phase 31 cap-checks at every Set* fast path,
        // this is the only path that runs in practice.
        const std::int64_t buildEntryNs = dxmt9PeRecorderStatsEnabled()
            ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
            : 0;
        if (buildSparseStateForRecord(D9C_COMMAND_RECORD_APPLY_STATE,
                                      dxmt9::d3d9::pe::PeDrawParams{})) {
            recordPeApplyStateBuildCpu(buildEntryNs);
            // sizeHint stays sizeof(D9CCommandRecordApplyState): it is what the
            // capacity precheck saw before, so seal cadence is unchanged.
            const HRESULT appendHr = appendRecordV2(
                D9C_COMMAND_RECORD_APPLY_STATE,
                sizeof(D9CCommandRecordApplyState),
                [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                    const AppendPhaseTimer& phase) -> HRESULT {
                    const auto t0 = AppendPhaseTimer::now();
                    const bool ok = dxmt9::d3d9::pe::appendApplyStateV2(
                        builder, peSparseHeader_.flags, peSparseState_);
                    phase.record(peAppendPhaseEncode_, t0);
                    return ok ? S_OK : D3DERR_INVALIDCALL;
                });
            if (FAILED(appendHr)) return appendHr;
            clearPendingHotState();
            return S_OK;
        }
        recordPeApplyStateBuildCpu(buildEntryNs);
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
    HRESULT FlushPeRecorderForBufferHazardForChild(D9CBuffer *buffer) noexcept override {
        if (!buffer) {
            return S_OK;
        }
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const bool referenced = commandChunkV2_.referencesObject(buffer);
        if (!referenced) {
            return S_OK;
        }
        return flushPeRecorder(PeRecorderFlushReason::Child);
    }
    D3D9PePresentCallToken NotifyPeFirstCallAfterPresentForChild(
        const char* callName, const void* callerPc = nullptr) noexcept override {
        return notePeDeviceCallAfterPresent(callName, callerPc);
    }
    void NotifyPeCallReturnAfterPresentForChild(
        const D3D9PePresentCallToken& token,
        const char* callName, HRESULT hr) noexcept override {
        logPeCallReturnAfterPresent(token, callName, hr);
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

    HRESULT AppendQueryIssueForChild(
        std::uint32_t flags,
        const dxmt9::d3d9::pe::PeWireObjectRef& query) noexcept override {
        return appendRecordV2(
            D9C_COMMAND_RECORD_QUERY_ISSUE,
            sizeof(D9CCommandRecordQueryIssue),
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendQueryIssueV2(
                    builder, flags, query);
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
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
        if (dev_) {
            D9CCommandChunkNegotiation negotiation{};
            negotiation.peSupportedVersions = D9C_COMMAND_CHUNK_CAP_VERSION_2;
            negotiation.pePreferredVersion = D9C_COMMAND_CHUNK_VERSION_V2;
            const HRESULT negotiationHr =
                hr32(dxmt9c_device_negotiate_command_chunk(
                    dev_, &negotiation));
            commandChunkNegotiated_ = SUCCEEDED(negotiationHr) &&
                negotiation.selectedVersion == D9C_COMMAND_CHUNK_VERSION_V2;
            if (commandChunkNegotiated_) {
                dxmt9DeviceInfoLog(
                    "command chunk negotiation selected v2 pe_caps=0x%x unix_caps=0x%x",
                    negotiation.peSupportedVersions,
                    negotiation.unixSupportedVersions);
            } else {
                dxmt9DeviceInfoLog(
                    "command chunk negotiation failed hr=0x%08x preferred=v2 selected=v%u unix_caps=0x%x",
                    static_cast<unsigned>(negotiationHr),
                    negotiation.selectedVersion,
                    negotiation.unixSupportedVersions);
            }
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

    bool commandChunkReady() const noexcept {
        return commandChunkNegotiated_;
    }

    ~D3D9DeviceImpl() {
        (void)flushPeRecorder(PeRecorderFlushReason::Destructor);
        dxmt9DeviceInfoLog(
            "command chunk totals selected=v2 chunks=%llu records=%llu bytes=%llu identity_getter_calls=%llu",
            static_cast<unsigned long long>(commandChunkCommits_),
            static_cast<unsigned long long>(commandChunkRecords_),
            static_cast<unsigned long long>(commandChunkBytes_),
            static_cast<unsigned long long>(
                dxmt9::d3d9::pe::wireIdentityGetterCallCount()));
        logVsConstSetterRangePerf("destructor");
        logPeRecorderStats("destructor", true);
        logPeStatsDecimation();
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

    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() noexcept override {
        return dxmt9c_device_get_swap_chain_count(dev_);
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPP) noexcept override {
        dxmt9PeSetCurrentCallName("Reset");
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
        dxmt9PeSetCurrentCallName("Present");
        const bool recordPresentTiming = dxmt9PeRecorderStatsEnabled();
        const std::uint32_t presentThreadId = dxmt9PeCurrentThreadId();
        const auto presentTimingEnter = std::chrono::steady_clock::now();
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const auto presentTimingStart = std::chrono::steady_clock::now();
        auto presentTimingBarrierEnd = presentTimingStart;
        auto presentTimingAppendEnd = presentTimingStart;
        auto presentTimingFlushEnd = presentTimingStart;
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
        if (recordPresentTiming) {
            presentTimingBarrierEnd = std::chrono::steady_clock::now();
            presentTimingAppendEnd = presentTimingBarrierEnd;
            presentTimingFlushEnd = presentTimingBarrierEnd;
        }
        if (FAILED(barrierHr)) {
            if (recordPresentTiming) {
                dxmt9DeviceInfoLog(
                    "pe_present_timing device=%p thread_id=0x%lx "
                    "hr=0x%08x total_ms=%.3f "
                    "lock_wait_ms=%.3f barrier_ms=%.3f append_ms=0.000 "
                    "flush_ms=0.000",
                    this, static_cast<unsigned long>(presentThreadId),
                    static_cast<unsigned>(barrierHr),
                    dxmt9ElapsedMs(presentTimingEnter, presentTimingBarrierEnd),
                    dxmt9ElapsedMs(presentTimingEnter, presentTimingStart),
                    dxmt9ElapsedMs(presentTimingStart, presentTimingBarrierEnd));
            }
            return barrierHr;
        }

        // sizeHint stays sizeof(D9CCommandRecordPresent) even though no legacy
        // record is built: it is what the capacity precheck saw before, so
        // chunk seal cadence is unchanged.
        const D9CCommandChunkWirePresentV2 presentWire{
            .hwnd = (uint64_t)(uintptr_t)wnd,
            .flags = 0,
            .hasSrc = src ? 1u : 0u,
            .hasDst = dst ? 1u : 0u,
            .reserved0 = 0u,
            .src = src ? cs : D9CRect{},
            .dst = dst ? cd : D9CRect{},
        };
        const HRESULT appendHr = appendRecordV2(
            D9C_COMMAND_RECORD_PRESENT, sizeof(D9CCommandRecordPresent),
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok =
                    dxmt9::d3d9::pe::appendPresentV2(builder, presentWire);
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        if (recordPresentTiming) {
            presentTimingAppendEnd = std::chrono::steady_clock::now();
            presentTimingFlushEnd = presentTimingAppendEnd;
        }
        if (FAILED(appendHr)) {
            if (recordPresentTiming) {
                dxmt9DeviceInfoLog(
                    "pe_present_timing device=%p thread_id=0x%lx "
                    "hr=0x%08x total_ms=%.3f "
                    "lock_wait_ms=%.3f barrier_ms=%.3f append_ms=%.3f "
                    "flush_ms=0.000",
                    this, static_cast<unsigned long>(presentThreadId),
                    static_cast<unsigned>(appendHr),
                    dxmt9ElapsedMs(presentTimingEnter, presentTimingAppendEnd),
                    dxmt9ElapsedMs(presentTimingEnter, presentTimingStart),
                    dxmt9ElapsedMs(presentTimingStart, presentTimingBarrierEnd),
                    dxmt9ElapsedMs(presentTimingBarrierEnd, presentTimingAppendEnd));
            }
            return appendHr;
        }
        // Force-commit so Present runs at the bridge boundary even
        // if the chunk is below the byte/record threshold.
        const HRESULT flushHr = flushPendingCommandChunk(PeRecorderFlushReason::Present);
        if (recordPresentTiming) {
            presentTimingFlushEnd = std::chrono::steady_clock::now();
            dxmt9DeviceInfoLog(
                "pe_present_timing device=%p thread_id=0x%lx "
                "hr=0x%08x total_ms=%.3f "
                "lock_wait_ms=%.3f barrier_ms=%.3f append_ms=%.3f "
                "flush_ms=%.3f",
                this, static_cast<unsigned long>(presentThreadId),
                static_cast<unsigned>(flushHr),
                dxmt9ElapsedMs(presentTimingEnter, presentTimingFlushEnd),
                dxmt9ElapsedMs(presentTimingEnter, presentTimingStart),
                dxmt9ElapsedMs(presentTimingStart, presentTimingBarrierEnd),
                dxmt9ElapsedMs(presentTimingBarrierEnd, presentTimingAppendEnd),
                dxmt9ElapsedMs(presentTimingAppendEnd, presentTimingFlushEnd));
        }
        if (SUCCEEDED(flushHr)) {
            logVsConstSetterRangePerf("present");
            logPeRecorderStats("present");
            markPePresentReturnedForCadence();
            notePeStatsDecimationPresent();
        }
        return flushHr;
    }

    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT idx,
                                             D3DBACKBUFFER_TYPE type,
                                             IDirect3DSurface9** ppS) noexcept override {
        notePeDeviceCallAfterPresent("GetBackBuffer", DXMT9_PE_CALLSITE_PC());
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
        notePeDeviceCallAfterPresent("GetRasterStatus");
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
        uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
        uint64_t* providerShared =
            extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
        D9CBuffer* b = dxmt9c_device_create_vertex_buffer_shared(dev_, len, usage,
                                                           fvf, (uint32_t)pool,
                                                           providerShared);
        if (!b) return D3DERR_INVALIDCALL;
        if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
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

    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src,
                                             const RECT* srcRect,
                                             IDirect3DSurface9* dst,
                                             const POINT* dstPt) noexcept override {
        dxmt9PeSetCurrentCallName("UpdateSurface");
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
        // Handle indices are assigned by appendUpdateSurfaceV2 as it appends
        // the refs, so they stay zero here.
        const D9CCommandChunkWireUpdateSurfaceV2 wire{
            .srcHandleIndex = 0u,
            .dstHandleIndex = 0u,
            .hasSrcRect = srcRect ? 1u : 0u,
            .hasDstPoint = dstPt ? 1u : 0u,
            .srcRect = cs,
            .dstPoint = cd,
        };
        return appendRecordV2(
            D9C_COMMAND_RECORD_UPDATE_SURFACE,
            sizeof(D9CCommandRecordUpdateSurface),
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendUpdateSurfaceV2(
                    builder, wire, D3D9PeWireSurface(src),
                    D3D9PeWireSurface(dst));
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
    }

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,
                                             IDirect3DBaseTexture9* dst) noexcept override {
        dxmt9PeSetCurrentCallName("UpdateTexture");
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
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
        // Wine d3d9 UpdateTexture: both args non-NULL; src in SYSTEMMEM;
        // dst not SYSTEMMEM/SCRATCH. test_update_texture_pool_copy_2d.
        const HRESULT appendHr = appendRecordV2(
            D9C_COMMAND_RECORD_UPDATE_TEXTURE,
            sizeof(D9CCommandRecordUpdateTexture),
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendUpdateTextureV2(
                    builder, D3D9PeWireTexture(src), D3D9PeWireTexture(dst));
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
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

    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt,
                                                   IDirect3DSurface9* dst) noexcept override {
        dxmt9PeSetCurrentCallName("GetRenderTargetData");
        auto peCadence = claimPeFirstCallAfterPresent();
        const void* callerPc = DXMT9_PE_CALLSITE_PC();
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const auto peCall =
            logPeCallMilestoneAfterPresent("GetRenderTargetData", callerPc);
        logPeFirstCallAfterPresent("GetRenderTargetData", peCadence, peCall);
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
        const HRESULT appendHr = appendRecordV2(
            D9C_COMMAND_RECORD_READBACK, sizeof(D9CCommandRecordReadback),
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendReadbackV2(
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

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sc, IDirect3DSurface9* surface) noexcept override {
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

    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src,
                                           const RECT* srcRect,
                                           IDirect3DSurface9* dst,
                                           const RECT* dstRect,
                                           D3DTEXTUREFILTERTYPE filter) noexcept override {
        dxmt9PeSetCurrentCallName("StretchRect");
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
        const D9CCommandChunkWireStretchRectV2 wire{
            .srcHandleIndex = 0u,
            .dstHandleIndex = 0u,
            .hasSrcRect = srcRect ? 1u : 0u,
            .hasDstRect = dstRect ? 1u : 0u,
            .filter = (uint32_t)filter,
            .reserved0 = 0u,
            .srcRect = srcRect ? cs : D9CRect{},
            .dstRect = dstRect ? cd : D9CRect{},
        };
        return appendRecordV2(
            D9C_COMMAND_RECORD_STRETCH_RECT,
            sizeof(D9CCommandRecordStretchRect),
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendStretchRectV2(
                    builder, wire, D3D9PeWireSurface(src),
                    D3D9PeWireSurface(dst));
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
    }

    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurf,
                                         const RECT* pRect,
                                         D3DCOLOR color) noexcept override {
        dxmt9PeSetCurrentCallName("ColorFill");
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
        const D9CCommandChunkWireColorFillV2 wire{
            .surfaceHandleIndex = 0u,
            .colorARGB = (uint32_t)color,
            .hasRect = pRect ? 1u : 0u,
            .reserved0 = 0u,
            .rect = pRect ? cr : D9CRect{},
        };
        return appendRecordV2(
            D9C_COMMAND_RECORD_COLOR_FILL, sizeof(D9CCommandRecordColorFill),
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendColorFillV2(
                    builder, wire, D3D9PeWireSurface(pSurf));
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
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

    /* ── render targets ── */

    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD idx,
                                               IDirect3DSurface9* pSurf) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "SetRenderTarget", DXMT9_PE_CALLSITE_PC());
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::RenderTarget);
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "SetRenderTarget", hr);
            return hr;
        };
        dxmt9DeviceDebugLog("device_set_render_target device=%p idx=%u surf=%p",
                            this, (unsigned)idx, pSurf);
        if (idx >= 4) return finishPeCall(D3DERR_INVALIDCALL);
        // render_target_device_mismatch: a surface created by a different
        // device cannot be bound. Compare via GetDevice; it AddRef's, so
        // Release the borrowed pointer immediately.
        if (pSurf) {
            IDirect3DDevice9* owner = nullptr;
            if (SUCCEEDED(pSurf->GetDevice(&owner)) && owner) {
                const bool foreign = owner != static_cast<IDirect3DDevice9*>(this);
                owner->Release();
                if (foreign) return finishPeCall(D3DERR_INVALIDCALL);
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
            hotSetter.markDirty();
        }
        if (idx == 0 && pSurf) {
            D3DSURFACE_DESC desc{};
            const HRESULT descHr = pSurf->GetDesc(&desc);
            if (FAILED(descHr)) return finishPeCall(descHr);
            const uint32_t w = std::max<uint32_t>(1u, desc.Width);
            const uint32_t h = std::max<uint32_t>(1u, desc.Height);
            peState_.viewportShadow = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
            peState_.scissorShadow = D9CRect{0, 0, static_cast<int32_t>(w),
                                             static_cast<int32_t>(h)};
            peState_.pendingViewport = true;
            peState_.pendingScissor = true;
            hotSetter.markDirty();
        }
        return finishPeCall(S_OK);
    }

    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx,
                                               IDirect3DSurface9** ppS) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "GetRenderTarget", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "GetRenderTarget", hr);
            return hr;
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

    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pSurf) noexcept override {
        notePeDeviceCallAfterPresent("SetDepthStencilSurface");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::DepthStencil);
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
            hotSetter.markDirty();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppS) noexcept override {
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

    /* ── scene ── */
    HRESULT STDMETHODCALLTYPE BeginScene() noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "BeginScene", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "BeginScene", hr);
            return hr;
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_begin_scene device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_scene(dev_));
        dxmt9DeviceDebugLog("device_begin_scene -> hr=0x%08x", (unsigned)hr);
        return finishPeCall(hr);
    }
    HRESULT STDMETHODCALLTYPE EndScene()   noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "EndScene", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "EndScene", hr);
            return hr;
        };
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_end_scene device=%p", this);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return finishPeCall(flushHr);
        const HRESULT hr = hr32(dxmt9c_device_end_scene(dev_));
        dxmt9DeviceDebugLog("device_end_scene -> hr=0x%08x", (unsigned)hr);
        return finishPeCall(hr);
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* pRects,
                                     DWORD flags, D3DCOLOR color,
                                     float z, DWORD stencil) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "Clear", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "Clear", hr);
            return hr;
        };
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
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
        // rectCount / rectOffset are computed by appendClearV2 from the span,
        // so they stay zero here. sizeHint keeps the legacy header+payload size
        // the capacity precheck saw before, so seal cadence is unchanged.
        const D9CCommandChunkWireClearV2 clearWire{
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
        const HRESULT hr = appendRecordV2(
            D9C_COMMAND_RECORD_CLEAR,
            sizeof(D9CCommandRecordClear) + rectBytes,
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok =
                    dxmt9::d3d9::pe::appendClearV2(builder, clearWire, rects);
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        return finishPeCall(hr);
    }

    /* ── transforms ── */
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* pM) noexcept override {
        notePeDeviceCallAfterPresent("SetTransform");
        PeHotStateSetterTimer hotSetter(*this, PeHotStateSetterFamily::Transform);
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
            hotSetter.markDirty();
            return hr32(dxmt9c_device_set_transform(dev_, stateKey, &wireM));
        }
        uint32_t transformSlotIndex = 0;
        if (!FixedTransformTable::slotForState(stateKey, transformSlotIndex)) {
            const HRESULT flushHr = flushPeRecorder();
            if (FAILED(flushHr)) return flushHr;
            hotSetter.markDirty();
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
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state,
                                            D3DMATRIX* pM) noexcept override {
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
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX* pM) noexcept override {
        notePeDeviceCallAfterPresent("MultiplyTransform");
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
        notePeDeviceCallAfterPresent("SetViewport", DXMT9_PE_CALLSITE_PC());
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::ViewportScissor);
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
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pVP) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "GetViewport", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "GetViewport", hr);
            return hr;
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
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pR) noexcept override {
        notePeDeviceCallAfterPresent("SetScissorRect", DXMT9_PE_CALLSITE_PC());
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::ViewportScissor);
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
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pR) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "GetScissorRect", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "GetScissorRect", hr);
            return hr;
        };
        if (!pR) return finishPeCall(D3DERR_INVALIDCALL);
        // Phase 12: PE shadow is the source of truth (see GetViewport).
        const D9CRect& cr = peState_.scissorShadow;
        pR->left = cr.left; pR->top = cr.top;
        pR->right = cr.right; pR->bottom = cr.bottom;
        return finishPeCall(S_OK);
    }

    /* ── material / lights ── */
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pM) noexcept override {
        notePeDeviceCallAfterPresent("SetMaterial");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::MaterialLightClip);
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_material device=%p", this);
        if (std::memcmp(&peState_.materialShadow, pM, sizeof(D9CMaterial)) == 0) {
            return S_OK;
        }
        std::memcpy(&peState_.materialShadow, pM, sizeof(D9CMaterial));
        peState_.pendingMaterial = true;
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pM) noexcept override {
        notePeDeviceCallAfterPresent("GetMaterial");
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_material device=%p", this);
        // PE-shadow is the source of truth: SetMaterial only writes the
        // shadow, never the C-side state. Reading from C would return the
        // default-constructed value instead of the last Set value.
        std::memcpy(pM, &peState_.materialShadow, sizeof(D3DMATERIAL9));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD idx, const D3DLIGHT9* pL) noexcept override {
        notePeDeviceCallAfterPresent("SetLight");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::MaterialLightClip);
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
            hotSetter.markDirty();
            return S_OK;
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        hotSetter.markDirty();
        return hr32(dxmt9c_device_set_light(dev_, idx, &cl));
    }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD idx, D3DLIGHT9* pL) noexcept override {
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
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) noexcept override {
        notePeDeviceCallAfterPresent("LightEnable");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::MaterialLightClip);
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
            hotSetter.markDirty();
            return S_OK;
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        hotSetter.markDirty();
        return hr32(dxmt9c_device_light_enable(dev_, idx, en ? 1u : 0u));
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD idx, BOOL* pEn) noexcept override {
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

    /* ── clip planes ── */
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* pPlane) noexcept override {
        notePeDeviceCallAfterPresent("SetClipPlane");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::MaterialLightClip);
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
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* pPlane) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* p) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* p) noexcept override {
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
        const HRESULT appendHr = appendRecordV2(
            D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
            sizeof(D9CCommandRecordReszDepthResolve),
            [&](dxmt9::d3d9::pe::CommandChunkV2Builder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendReszDepthResolveV2(
                    builder, D3D9PeWireSurface(dsSurface_),
                    D3D9PeWireTexture(textures_[0]));
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        if (FAILED(appendHr)) return appendHr;
        return S_OK;
    }

    /* ── render states ── */
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) noexcept override {
        notePeDeviceCallAfterPresent("SetRenderState");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::RenderState);
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
            hotSetter.markDirty();
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
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD* pValue) noexcept override {
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

    /* ── state blocks ── */
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type,
                                                IDirect3DStateBlock9** ppSB) noexcept override {
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
    HRESULT STDMETHODCALLTYPE BeginStateBlock() noexcept override {
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
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) noexcept override {
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
        notePeDeviceCallAfterPresent("SetTextureStageState");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::TextureStageSampler);
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
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD* pValue) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD value) noexcept override {
        notePeDeviceCallAfterPresent("SetSamplerState");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::TextureStageSampler);
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
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD* pValue) noexcept override {
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
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pPasses) noexcept override {
        notePeDeviceCallAfterPresent("ValidateDevice");
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
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY* out) noexcept override {
        notePeDeviceCallAfterPresent("GetPaletteEntries");
        dxmt9DeviceDebugLog("device_get_palette_entries device=%p palette=%u out=%p",
                            this, palette, static_cast<void*>(out));
        if (!out) return D3DERR_INVALIDCALL;
        const auto it = palettes_.find(palette);
        if (it == palettes_.end()) return D3DERR_INVALIDCALL;
        std::memcpy(out, it->second.data(), sizeof(PALETTEENTRY) * 256);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) noexcept override {
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
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) noexcept override {
        notePeDeviceCallAfterPresent("GetCurrentTexturePalette");
        dxmt9DeviceDebugLog("device_get_current_texture_palette device=%p out=%p",
                            this, static_cast<void*>(p));
        if (!p) return D3DERR_INVALIDCALL;
        if (!currentPaletteSet_) return D3DERR_INVALIDCALL;
        *p = currentPaletteIndex_;
        return S_OK;
    }

    /* ── soft VP / NPatches ── */
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL enable) noexcept override {
        notePeDeviceCallAfterPresent("SetSoftwareVertexProcessing");
        dxmt9DeviceDebugLog("device_set_software_vertex_processing device=%p enable=%u", this, (unsigned)enable);
        softwareVertexProcessing_ = enable ? TRUE : FALSE;
        return S_OK;
    }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() noexcept override {
        notePeDeviceCallAfterPresent("GetSoftwareVertexProcessing");
        dxmt9DeviceDebugLog("device_get_software_vertex_processing device=%p", this);
        return softwareVertexProcessing_;
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) noexcept override {
        notePeDeviceCallAfterPresent("SetNPatchMode");
        dxmt9DeviceDebugLog("device_set_npatch_mode device=%p segments=%f", this, segments);
        // stub: Wine returns S_OK; N-Patch tessellation was removed in D3D10, legacy
        // apps tolerate a no-op.
        return S_OK;
    }
    float   STDMETHODCALLTYPE GetNPatchMode() noexcept override {
        notePeDeviceCallAfterPresent("GetNPatchMode");
        // stub: Wine returns 0.0f; N-Patch tessellation removed in D3D10, legacy apps
        // tolerate a no-op.
        return 0.0f;
    }

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
        notePeDeviceCallAfterPresent("SetTexture");
        PeHotStateSetterTimer hotSetter(*this, PeHotStateSetterFamily::Texture);
        dxmt9DeviceDebugLog("device_set_texture device=%p stage=%u tex=%p",
                            this, (unsigned)stage, pTex);
        // Wine d3d9 test_limits: all 16 pixel samplers are settable
        // (stages 0..15), independent of caps.MaxSimultaneousTextures.
        // fragmentTextureStageSlot is the single validator: fragment
        // stages 0..15 and vertex samplers 257..260 map to slots,
        // anything else is D3DERR_INVALIDCALL.
        uint32_t textureSlot = 0;
        if (!fragmentTextureStageSlot(stage, textureSlot)) return D3DERR_INVALIDCALL;
        if (textures_[textureSlot] == pTex) {
            return S_OK;
        }
        setRef(textures_[textureSlot], pTex);
        applyCurrentPaletteToTexture(textures_[textureSlot]);
        peState_.pendingTextureMask |= 1u << textureSlot;
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage,
                                          IDirect3DBaseTexture9** ppTex) noexcept override {
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
        notePeDeviceCallAfterPresent("SetFVF");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::VertexInput);
        dxmt9DeviceDebugLog("device_set_fvf device=%p fvf=0x%x", this, (unsigned)fvf);
        if (fvf_ == fvf && vdecl_ != nullptr) {
            /* Same FVF, decl already mirrored. */
            return S_OK;
        }
        fvf_ = fvf;
        peState_.pendingFvf = true;
        peState_.pendingVdecl = true;
        hotSetter.markDirty();
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
        notePeDeviceCallAfterPresent("GetFVF");
        if (!pFVF) return D3DERR_INVALIDCALL;
        *pFVF = fvf_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
            const D3DVERTEXELEMENT9* pElems,
            IDirect3DVertexDeclaration9** ppVD) noexcept override {
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
        *ppVD = CreatePeVertexDecl(d, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(
            IDirect3DVertexDeclaration9* pVD) noexcept override {
        const auto peCall =
            notePeDeviceCallAfterPresent("SetVertexDeclaration");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::VertexInput);
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "SetVertexDeclaration", hr);
            return hr;
        };
        dxmt9DeviceDebugLog("device_set_vertex_declaration device=%p decl=%p", this, pVD);
        // PE-shadow stateblock support: remember that vdecl was touched
        // during BeginStateBlock/EndStateBlock so the resulting block's
        // tracked set includes the vdecl slot. The flag is consumed by
        // CaptureStateBlockShadowForChild and cleared in EndStateBlock.
        if (stateBlockRecording_) {
            peState_.stateBlockVdeclRecorded = true;
            hotSetter.markDirty();
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
        vdecl_ = pVD;
        /* Explicit decl resets FVF to 0 (Wine
         * test_vertex_declaration_fvf_policy line ~692). User-supplied
         * decls do not back-convert to an FVF in this PE shadow; that
         * mapping is intentionally lossy. */
        fvf_ = 0;
        peState_.pendingVdecl = true;
        peState_.pendingFvf = true;
        hotSetter.markDirty();
        return finishPeCall(S_OK);
    }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(
            IDirect3DVertexDeclaration9** ppVD) noexcept override {
        notePeDeviceCallAfterPresent("GetVertexDeclaration");
        if (!ppVD) return D3DERR_INVALIDCALL;
        if (vdecl_) vdecl_->AddRef();
        *ppVD = vdecl_; return S_OK;
    }

    /* ── vertex shaders ── */
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFn,
                                                  IDirect3DVertexShader9** ppVS) noexcept override {
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
        *ppVS = CreatePeVertexShader(s, this, hashValidatedShaderBytecode(pFn));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) noexcept override {
        notePeDeviceCallAfterPresent("SetVertexShader");
        PeHotStateSetterTimer hotSetter(*this, PeHotStateSetterFamily::Shader);
        dxmt9DeviceDebugLog("device_set_vertex_shader device=%p shader=%p", this, pVS);
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // packet built for the next draw carries vsValid=1 + the vs_
        // wire handle; server-side applyDrawPacketState dispatches the
        // dxmt9c_device_set_vertex_shader call before the draw runs.
        if (vs_ == pVS) return S_OK;
        setRef(vs_, pVS);
        peState_.pendingVs = true;
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppVS) noexcept override {
        notePeDeviceCallAfterPresent("GetVertexShader");
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
        const auto peCall =
            notePeDeviceCallAfterPresent(
                "SetVertexShaderConstantF", DXMT9_PE_CALLSITE_PC());
        const std::int64_t callEntryNs = dxmt9PeRecorderStatsEnabled()
            ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
            : 0;
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            if (SUCCEEDED(hr)) {
                recordPeConstSetterCpu(D9C_COMMAND_RECORD_SET_VS_CONST_F,
                                       callEntryNs, count);
            }
            logPeCallReturnAfterPresent(peCall, "SetVertexShaderConstantF", hr);
            return hr;
        };
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_f device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
        if (FAILED(hr)) return finishPeCall(hr);
        if (dxmt9PerfVsConstSetterRangeEnabled()) {
            const VsConstRangeChange change = analyzeConstShadowChange(
                peConsts_.vsConstF, start, count, pData, sizeof(float) * 4);
            recordVsConstSetterRange(VsConstSetterRangePhase::Call,
                                     currentVertexShaderHash(),
                                     currentPixelShaderHash(),
                                     start, count,
                                     change.changedRegs,
                                     change.changedSpanRegs);
        }
        // Shadow-only: defer the record until the next flushPendingConsts()
        // (called before each draw record + at chunk commit).
        touchConstShadow(peConsts_.vsConstF, start, count, pData, sizeof(float) * 4);
        return finishPeCall(S_OK);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* pData,
                                                        UINT count) noexcept override {
        notePeDeviceCallAfterPresent("GetVertexShaderConstantF");
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.vsConstF, start, pData, count, sizeof(float) * 4);
        return S_OK;    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) noexcept override {
        notePeDeviceCallAfterPresent("SetVertexShaderConstantI");
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.vsConstI, start, count, pData, sizeof(int32_t) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, INT* pData,
                                                        UINT count) noexcept override {
        notePeDeviceCallAfterPresent("GetVertexShaderConstantI");
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.vsConstI, start, pData, count, sizeof(int32_t) * 4);        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) noexcept override {
        notePeDeviceCallAfterPresent("SetVertexShaderConstantB");
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.vsConstB, start, count, pData, sizeof(uint32_t));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, BOOL* pData,
                                                        UINT count) noexcept override {
        notePeDeviceCallAfterPresent("GetVertexShaderConstantB");
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.vsConstB, start, pData, count, sizeof(uint32_t));        return S_OK;
    }

    /* ── stream sources ── */
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9* pBuf,
                                               UINT offset, UINT stride) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent("SetStreamSource");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::VertexInput);
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "SetStreamSource", hr);
            return hr;
        };
        dxmt9DeviceDebugLog("device_set_stream_source device=%p stream=%u buf=%p offset=%u stride=%u",
                            this, stream, pBuf, offset, stride);
        if (stream >= 16) return finishPeCall(D3DERR_INVALIDCALL);
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
                return finishPeCall(S_OK);
            }
            setRef(streamSrc_[stream], (IDirect3DVertexBuffer9*)nullptr);
            peState_.pendingStreamMask |= 1u << stream;
            hotSetter.markDirty();
            return finishPeCall(S_OK);
        }
        if (shadowedStreamSourceEquals(stream, pBuf, offset, stride)) {
            return finishPeCall(S_OK);
        }
        setRef(streamSrc_[stream], pBuf);
        streamOff_[stream] = offset;
        streamStr_[stream] = stride;
        peState_.pendingStreamMask |= 1u << stream;
        hotSetter.markDirty();
        return finishPeCall(S_OK);
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9** ppBuf,
                                               UINT* pOffset, UINT* pStride) noexcept override {
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
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT freq) noexcept override {
        notePeDeviceCallAfterPresent("SetStreamSourceFreq");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::VertexInput);
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
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        streamFreq_[stream] = freq;
        hotSetter.markDirty();
        return hr32(dxmt9c_device_set_stream_source_freq(dev_, stream, freq));
    }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* pFreq) noexcept override {
        notePeDeviceCallAfterPresent("GetStreamSourceFreq");
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
        const auto peCall = notePeDeviceCallAfterPresent("SetIndices");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::VertexInput);
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "SetIndices", hr);
            return hr;
        };
        dxmt9DeviceDebugLog("device_set_indices device=%p ib=%p", this, pIBuf);
        if (indexBuf_ == pIBuf) return finishPeCall(S_OK);
        setRef(indexBuf_, pIBuf);
        peState_.pendingIb = true;
        hotSetter.markDirty();
        return finishPeCall(S_OK);
    }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIBuf) noexcept override {
        notePeDeviceCallAfterPresent("GetIndices");
        if (!ppIBuf) return D3DERR_INVALIDCALL;
        if (indexBuf_) indexBuf_->AddRef(); *ppIBuf = indexBuf_; return S_OK;
    }

    /* ── pixel shaders ── */
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFn,
                                                 IDirect3DPixelShader9** ppPS) noexcept override {
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
        *ppPS = CreatePePixelShader(s, this, hashValidatedShaderBytecode(pFn));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pPS) noexcept override {
        notePeDeviceCallAfterPresent("SetPixelShader");
        PeHotStateSetterTimer hotSetter(*this, PeHotStateSetterFamily::Shader);
        dxmt9DeviceDebugLog("device_set_pixel_shader device=%p shader=%p", this, pPS);
        if (ps_ == pPS) return S_OK;
        setRef(ps_, pPS);
        peState_.pendingPs = true;
        hotSetter.markDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) noexcept override {
        notePeDeviceCallAfterPresent("GetPixelShader");
        if (!ppPS) return D3DERR_INVALIDCALL;
        if (ps_) ps_->AddRef(); *ppPS = ps_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) noexcept override {
        const auto peCall =
            notePeDeviceCallAfterPresent(
                "SetPixelShaderConstantF", DXMT9_PE_CALLSITE_PC());
        const std::int64_t callEntryNs = dxmt9PeRecorderStatsEnabled()
            ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
            : 0;
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            if (SUCCEEDED(hr)) {
                recordPeConstSetterCpu(D9C_COMMAND_RECORD_SET_PS_CONST_F,
                                       callEntryNs, count);
            }
            logPeCallReturnAfterPresent(peCall, "SetPixelShaderConstantF", hr);
            return hr;
        };
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_f device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
        if (FAILED(hr)) return finishPeCall(hr);
        touchConstShadow(peConsts_.psConstF, start, count, pData, sizeof(float) * 4);
        return finishPeCall(S_OK);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* pData,
                                                       UINT count) noexcept override {
        notePeDeviceCallAfterPresent("GetPixelShaderConstantF");
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.psConstF, start, pData, count, sizeof(float) * 4);
        return S_OK;    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) noexcept override {
        notePeDeviceCallAfterPresent("SetPixelShaderConstantI");
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.psConstI, start, count, pData, sizeof(int32_t) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, INT* pData,
                                                       UINT count) noexcept override {
        notePeDeviceCallAfterPresent("GetPixelShaderConstantI");
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.psConstI, start, pData, count, sizeof(int32_t) * 4);        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) noexcept override {
        notePeDeviceCallAfterPresent("SetPixelShaderConstantB");
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.psConstB, start, count, pData, sizeof(uint32_t));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, BOOL* pData,
                                                       UINT count) noexcept override {
        notePeDeviceCallAfterPresent("GetPixelShaderConstantB");
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.psConstB, start, pData, count, sizeof(uint32_t));        return S_OK;
    }

    /* ── draw calls ── */
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type,
                                             UINT startVertex,
                                             UINT count) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "DrawPrimitive", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "DrawPrimitive", hr);
            return hr;
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_primitive device=%p type=%u startVertex=%u count=%u",
                            this, (unsigned)type, startVertex, count);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return finishPeCall(barrierHr);
        }
        SoftwareFfpDrawData swvpDraw{};
        HRESULT hr = trySoftwareFfpDrawPrimitive(type, startVertex, count, swvpDraw);
        if (hr == S_FALSE) {
            hr = trySoftwareProgrammableDrawPrimitive(
                type, startVertex, count, swvpDraw);
        }
        bool appendedDraw = false;
        if (hr == S_OK) {
            hr = filterSoftwareDrawOutsideClipPrimitives(swvpDraw);
        }
        if (hr == S_OK) {
            dxmt9DeviceDebugLog("device_draw_primitive swvp_fallback device=%p fvf=0x%x stride=%u bytes=%zu",
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
            hr = appendDrawPrimitiveRecord(type, startVertex, count);
            appendedDraw = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr) && appendedDraw) {
            clearPendingHotState();
            if (!swvpDraw.vertices.empty()) {
                peState_.pendingFvf = true;
                peState_.pendingVdecl = true;
                if (swvpDraw.bypassVertexShader) peState_.pendingVs = true;
            }
        }
        notePeCurrentCallReturnForInterAppendSplit();
        return finishPeCall(hr);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type,
                                                    INT baseVertex,
                                                    UINT minVertex, UINT numVertices,
                                                    UINT startIndex,
                                                    UINT count) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "DrawIndexedPrimitive", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "DrawIndexedPrimitive", hr);
            return hr;
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_indexed_primitive device=%p type=%u base=%d min=%u num=%u startIndex=%u count=%u",
                            this, (unsigned)type, baseVertex, minVertex, numVertices,
                            startIndex, count);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return finishPeCall(barrierHr);
        }
        SoftwareFfpDrawData swvpDraw{};
        std::vector<std::uint8_t> swvpIndices{};
        D3DFORMAT swvpIndexFormat = D3DFMT_UNKNOWN;
        HRESULT hr = trySoftwareFfpDrawIndexedPrimitive(
            type, baseVertex, minVertex, numVertices, startIndex, count,
            swvpDraw, swvpIndices, swvpIndexFormat);
        if (hr == S_FALSE) {
            hr = trySoftwareProgrammableDrawIndexedPrimitive(
                type, baseVertex, minVertex, numVertices, startIndex, count,
                swvpDraw, swvpIndices, swvpIndexFormat);
        }
        bool appendedDraw = false;
        if (hr == S_OK) {
            hr = filterSoftwareIndexedDrawOutsideClipPrimitives(
                swvpDraw, swvpIndices, swvpIndexFormat);
        }
        if (hr == S_OK) {
            dxmt9DeviceDebugLog("device_draw_indexed_primitive swvp_fallback device=%p fvf=0x%x stride=%u vertexBytes=%zu indexBytes=%zu",
                                this, (unsigned)swvpDraw.fvf, swvpDraw.stride,
                                swvpDraw.vertices.size(), swvpIndices.size());
            const UINT swvpNumVertices = swvpDraw.stride
                ? static_cast<UINT>(swvpDraw.vertices.size() / swvpDraw.stride)
                : numVertices;
            const UINT swvpPrimitiveCount = swvpDraw.primitiveCount
                ? swvpDraw.primitiveCount
                : count;
            if (swvpPrimitiveCount != 0u && !swvpDraw.vertices.empty() &&
                !swvpIndices.empty()) {
                hr = appendDrawIndexedPrimitiveUPRecordWithFvf(
                    swvpDraw.primitiveType, 0, swvpNumVertices, swvpPrimitiveCount,
                    swvpIndices.data(), swvpIndexFormat, swvpDraw.vertices.data(),
                    swvpDraw.stride, true, swvpDraw.fvf,
                    swvpDraw.bypassVertexShader, true);
                appendedDraw = SUCCEEDED(hr);
            }
        } else if (hr == S_FALSE) {
            hr = appendDrawIndexedPrimitiveRecord(type, baseVertex, minVertex,
                                                 numVertices, startIndex, count);
            appendedDraw = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr) && appendedDraw) {
            clearPendingHotState();
            if (!swvpDraw.vertices.empty()) {
                peState_.pendingFvf = true;
                peState_.pendingVdecl = true;
                peState_.pendingIb = indexBuf_ != nullptr;
                if (swvpDraw.bypassVertexShader) peState_.pendingVs = true;
            }
        }
        notePeCurrentCallReturnForInterAppendSplit();
        return finishPeCall(hr);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                               UINT count,
                                               const void* pData,
                                               UINT stride) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "DrawPrimitiveUP", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(peCall, "DrawPrimitiveUP", hr);
            return hr;
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_primitive_up device=%p type=%u count=%u data=%p stride=%u",
                            this, (unsigned)type, count, pData, stride);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return finishPeCall(barrierHr);
        }
        SoftwareFfpDrawData swvpDraw{};
        HRESULT hr = trySoftwareFfpDrawPrimitiveUP(type, count, pData, stride, swvpDraw);
        if (hr == S_FALSE) {
            hr = trySoftwareProgrammableDrawPrimitiveUP(
                type, count, pData, stride, swvpDraw);
        }
        bool appendedDraw = false;
        if (hr == S_OK) {
            hr = filterSoftwareDrawOutsideClipPrimitives(swvpDraw);
        }
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
            clearPendingHotState();
            if (!swvpDraw.vertices.empty()) {
                peState_.pendingFvf = true;
                peState_.pendingVdecl = true;
                if (swvpDraw.bypassVertexShader) peState_.pendingVs = true;
            }
        }
        return finishPeCall(hr);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                      UINT minVertex,
                                                      UINT numVertices,
                                                      UINT count,
                                                      const void* pIdxData,
                                                      D3DFORMAT idxFmt,
                                                      const void* pVtxData,
                                                      UINT stride) noexcept override {
        const auto peCall = notePeDeviceCallAfterPresent(
            "DrawIndexedPrimitiveUP", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            logPeCallReturnAfterPresent(
                peCall, "DrawIndexedPrimitiveUP", hr);
            return hr;
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_indexed_primitive_up device=%p type=%u min=%u num=%u count=%u idx=%p idxFmt=%u vtx=%p stride=%u",
                            this, (unsigned)type, minVertex, numVertices, count,
                            pIdxData, (unsigned)idxFmt, pVtxData, stride);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return finishPeCall(barrierHr);
        }
        SoftwareFfpDrawData swvpDraw{};
        std::vector<std::uint8_t> swvpIndices{};
        D3DFORMAT swvpIndexFormat = idxFmt;
        HRESULT hr = trySoftwareFfpDrawIndexedPrimitiveUP(
            type, minVertex, numVertices, count, pVtxData, stride, swvpDraw);
        if (hr == S_FALSE) {
            hr = trySoftwareProgrammableDrawIndexedPrimitiveUP(
                type, minVertex, numVertices, count, pVtxData, stride, swvpDraw);
        }
        bool appendedDraw = false;
        bool useSwvpIndices = false;
        if (hr == S_OK && renderStateValue(D3DRS_CLIPPING) != FALSE) {
            const UINT indexSize = idxFmt == D3DFMT_INDEX32 ? 4u : 2u;
            std::uint32_t indexBytes = 0;
            if (!checkedByteCount(primitiveVertexCount(type, count), indexSize,
                                  indexBytes) ||
                (indexBytes != 0u && !pIdxData)) {
                hr = D3DERR_INVALIDCALL;
            } else {
                swvpIndices.resize(indexBytes);
                if (indexBytes != 0u) {
                    std::memcpy(swvpIndices.data(), pIdxData, indexBytes);
                }
                hr = filterSoftwareIndexedDrawOutsideClipPrimitives(
                    swvpDraw, swvpIndices, swvpIndexFormat);
                useSwvpIndices = SUCCEEDED(hr);
            }
        }
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
            clearPendingHotState();
            if (!swvpDraw.vertices.empty()) {
                peState_.pendingFvf = true;
                peState_.pendingVdecl = true;
                if (swvpDraw.bypassVertexShader) peState_.pendingVs = true;
            }
        }
        return finishPeCall(hr);
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT srcStart, UINT dstIndex,
                                               UINT vertexCount,
                                               IDirect3DVertexBuffer9* dstBuffer,
                                               IDirect3DVertexDeclaration9* declaration,
                                               DWORD flags) noexcept override {
        notePeDeviceCallAfterPresent("ProcessVertices");
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
        bool sourceLayoutFromDeclaration = false;
        if (fvf_ != 0) {
            const DWORD positionMask = fvf_ & D3DFVF_POSITION_MASK;
            if ((positionMask != D3DFVF_XYZ &&
                 (programmable
                      ? (positionMask != D3DFVF_XYZW &&
                         !processFvfXyzbPosition(positionMask))
                      : !processFvfXyzbPosition(positionMask))) ||
                !describeProcessFvf(fvf_, srcLayout)) {
                return invalid("source FVF unsupported");
            }
        } else if (vdecl_) {
            sourceLayoutFromDeclaration = true;
            if (!describeProcessDeclaration(vdecl_, srcLayout, false)) {
                return invalid("source declaration unsupported");
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
        UINT fixedBlendWeightCount = 0;
        bool fixedIndexedVertexBlend = false;
        const DWORD vertexBlendState =
            programmable ? D3DVBF_DISABLE : renderStateValue(D3DRS_VERTEXBLEND);
        if (!programmable && vertexBlendState != D3DVBF_DISABLE) {
            fixedIndexedVertexBlend =
                renderStateValue(D3DRS_INDEXEDVERTEXBLENDENABLE) != FALSE;
            switch (vertexBlendState) {
                case D3DVBF_1WEIGHTS:
                    fixedBlendWeightCount = 1;
                    break;
                case D3DVBF_2WEIGHTS:
                    fixedBlendWeightCount = 2;
                    break;
                case D3DVBF_3WEIGHTS:
                    fixedBlendWeightCount = 3;
                    break;
                default:
                    return invalid("vertex blending mode unsupported");
            }
        }
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
        auto inferTrailingTexcoord0Read = [&]() -> bool {
            if (!sourceLayoutFromDeclaration || srcLayout.texBytes[0] != 0u ||
                srcLayout.streamStride[0] == 0u) {
                return false;
            }
            const UINT offset = srcLayout.streamStride[0];
            const UINT bytes = 2u * sizeof(float);
            if (streamStr_[0] < offset + bytes) return false;
            // Windows accepts ProcessVertices content that leaves TEXCOORD0
            // out of a narrow explicit declaration while still carrying the
            // legacy FVF float2 tail in stream 0. Keep this compatibility
            // path limited to TEXCOORD0 and only when the bound stride proves
            // the tail exists.
            srcLayout.texCount = std::max<UINT>(srcLayout.texCount, 1u);
            srcLayout.texStream[0] = 0u;
            srcLayout.texOffset[0] = offset;
            srcLayout.texBytes[0] = bytes;
            srcLayout.texType[0] = D3DDECLTYPE_FLOAT2;
            srcReadBytes[0] = std::max(srcReadBytes[0], offset + bytes);
            return true;
        };
        auto requireTexRead = [&](UINT i, bool requireMatchingBytes) -> bool {
            const bool hasTex =
                i < srcLayout.texCount && srcLayout.texBytes[i] != 0u;
            if (!hasTex && i == 0u && !requireMatchingBytes &&
                inferTrailingTexcoord0Read()) {
                return true;
            }
            if (!hasTex ||
                (requireMatchingBytes &&
                 (dstLayout.texBytes[i] != srcLayout.texBytes[i] ||
                  dstLayout.texType[i] != srcLayout.texType[i]))) {
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
            // vs_1_x maps every v# to a fixed FFP semantic by default. We must
            // only require streams for v# that the shader actually reads as a
            // source operand (or DCL'd, for vs_2.0+/3.0). usedInputMask was
            // populated by the operand scan in analyzeSimpleProcessVertexShader.
            auto inputUsed = [&](int regIdx) {
                if (regIdx < 0 || regIdx >= 32) return false;
                if (shaderIo.major < 3u) {
                    return (shaderIo.usedInputMask & (1u << regIdx)) != 0u;
                }
                return true;  // sm3 requires DCL — presence implies use
            };
            if (inputUsed(shaderIo.inputPosition)) requirePositionRead();
            if (inputUsed(shaderIo.inputNormal) && !requireNormalRead()) return invalid("shader normal input missing");
            if (inputUsed(shaderIo.inputTangent) && !requireTangentRead()) return invalid("shader tangent input missing");
            if (inputUsed(shaderIo.inputBinormal) && !requireBinormalRead()) return invalid("shader binormal input missing");
            if (inputUsed(shaderIo.inputBlendWeight) && !requireBlendWeightRead()) return invalid("shader blendweight input missing");
            if (inputUsed(shaderIo.inputBlendIndices) && !requireBlendIndicesRead()) return invalid("shader blendindices input missing");
            if (inputUsed(shaderIo.inputPSize) && !requirePSizeRead()) return invalid("shader psize input missing");
            if (inputUsed(shaderIo.inputDiffuse) && !requireDiffuseRead()) return invalid("shader diffuse input missing");
            if (inputUsed(shaderIo.inputSpecular) && !requireSpecularRead()) return invalid("shader specular input missing");
            for (UINT i = 0; i < 8; ++i) {
                if (inputUsed(shaderIo.inputTex[i]) && !requireTexRead(i, false)) {
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
            if (fixedBlendWeightCount != 0u && !requireBlendWeightRead()) {
                return invalid("vertex blending weight input missing");
            }
            if (fixedIndexedVertexBlend && !requireBlendIndicesRead()) {
                return invalid("indexed vertex blending indices missing");
            }
            if (dstLayout.diffuse) {
                if (processLighting) {
                    if (!requireNormalRead()) return invalid("lighting normal input missing");
                } else if (!requireDiffuseRead()) {
                    return invalid("diffuse passthrough missing");
                }
            }
            if (dstLayout.specular && processSpecularLighting) {
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
        D3D9PeInvalidateVertexBufferReadonlyCache(dstBuffer);

        const auto& vp = peState_.viewportShadow;
        const float scaleX = static_cast<float>(vp.width) * 0.5f;
        const float scaleY = static_cast<float>(vp.height) * 0.5f;
        const float offsetX = static_cast<float>(vp.x) + scaleX;
        const float offsetY = static_cast<float>(vp.y) + scaleY;
        const float zScale = vp.maxZ - vp.minZ;
        const D9CMatrix wvp = worldViewProjectionTransform();
        const D9CMatrix viewProjection = multiplyTransformMatrix(
            transformOrIdentity(D3DTS_VIEW),
            transformOrIdentity(D3DTS_PROJECTION));
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
        std::array<uint32_t, 16> shaderConstB{};
        if (programmable && !peConsts_.vsConstB.values.empty()) {
            const size_t bytes = std::min(peConsts_.vsConstB.values.size(),
                                          shaderConstB.size() * sizeof(shaderConstB[0]));
            std::memcpy(shaderConstB.data(), peConsts_.vsConstB.values.data(), bytes);
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
            auto transformPoint = [](const float position[4],
                                     const D9CMatrix& matrix,
                                     float out[4]) {
                for (UINT col = 0; col < 4; ++col) {
                    out[col] = position[0] * matrix.m[col] +
                               position[1] * matrix.m[4 + col] +
                               position[2] * matrix.m[8 + col] +
                               position[3] * matrix.m[12 + col];
                }
            };
            if (programmable) {
                SimpleVsRegisters regs{};
                regs.constant = shaderConstF;
                regs.constantInt = shaderConstI;
                regs.constantBool = shaderConstB;
                auto loadPositionInput = [&](int reg) {
                    if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                    const auto* positionSource =
                        srcBase[srcLayout.positionStream] +
                        sourceOffset(srcLayout.positionStream, i) +
                        srcLayout.positionOffset;
                    return decodeProcessDeclVector(positionSource,
                                                   srcLayout.positionType,
                                                   srcLayout.positionBytes,
                                                   regs.input[reg]);
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
                        case D3DDECLTYPE_SHORT2: {
                            int16_t in[2]{};
                            std::memcpy(in, source, sizeof(in));
                            regs.input[reg][0] = static_cast<float>(in[0]);
                            regs.input[reg][1] = static_cast<float>(in[1]);
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
                        case D3DDECLTYPE_D3DCOLOR: {
                            DWORD color = 0;
                            std::memcpy(&color, source, sizeof(color));
                            unpackD3DColor(color, regs.input[reg].data());
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
                auto loadBlendIndicesInput =
                    [&](int reg, UINT stream, UINT offset, UINT type) {
                        if (type == D3DDECLTYPE_UBYTE4) {
                            return loadUbyte4Input(reg, stream, offset);
                        }
                        if (type != D3DDECLTYPE_D3DCOLOR ||
                            reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) {
                            return false;
                        }
                        DWORD color = 0;
                        const auto* source =
                            srcBase[stream] + sourceOffset(stream, i) + offset;
                        std::memcpy(&color, source, sizeof(color));
                        unpackD3DColor(color, regs.input[reg].data());
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
                        case D3DDECLTYPE_D3DCOLOR: {
                            DWORD color = 0;
                            std::memcpy(&color, texSource, sizeof(color));
                            unpackD3DColor(color, regs.input[reg].data());
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
                        case D3DDECLTYPE_DEC3N: {
                            uint32_t packed = 0;
                            std::memcpy(&packed, texSource, sizeof(packed));
                            regs.input[reg][0] = snorm10ToFloat(packed);
                            regs.input[reg][1] = snorm10ToFloat(packed >> 10u);
                            regs.input[reg][2] = snorm10ToFloat(packed >> 20u);
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
                // Mirror the validator's usedInputMask gating so we only load
                // v# the shader actually reads. vs_1_x maps every v# to a fixed
                // FFP semantic by default, but issuing a load for a v# that the
                // shader never references would dereference srcBase[stream]
                // for a stream the caller deliberately left unbound.
                auto inputLoadGate = [&](int regIdx) {
                    if (regIdx < 0 || regIdx >= 32) return false;
                    return (shaderIo.usedInputMask & (1u << regIdx)) != 0u;
                };
                if (inputLoadGate(shaderIo.inputPosition) && !loadPositionInput(shaderIo.inputPosition)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (inputLoadGate(shaderIo.inputNormal) &&
                    !loadDeclVectorInput(shaderIo.inputNormal, srcLayout.normalStream,
                                         srcLayout.normalOffset, srcLayout.normalType,
                                         srcLayout.normalBytes)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (inputLoadGate(shaderIo.inputTangent) &&
                    !loadDeclVectorInput(shaderIo.inputTangent, srcLayout.tangentStream,
                                         srcLayout.tangentOffset, srcLayout.tangentType,
                                         srcLayout.tangentBytes)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (inputLoadGate(shaderIo.inputBinormal) &&
                    !loadDeclVectorInput(shaderIo.inputBinormal, srcLayout.binormalStream,
                                         srcLayout.binormalOffset, srcLayout.binormalType,
                                         srcLayout.binormalBytes)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (inputLoadGate(shaderIo.inputBlendWeight) &&
                    !loadDeclVectorInput(shaderIo.inputBlendWeight,
                                         srcLayout.blendWeightStream,
                                         srcLayout.blendWeightOffset,
                                         srcLayout.blendWeightType,
                                         srcLayout.blendWeightBytes)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (inputLoadGate(shaderIo.inputBlendIndices) &&
                    !loadBlendIndicesInput(shaderIo.inputBlendIndices,
                                           srcLayout.blendIndicesStream,
                                           srcLayout.blendIndicesOffset,
                                           srcLayout.blendIndicesType)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (inputLoadGate(shaderIo.inputPSize) &&
                    !loadFloatVectorInput(shaderIo.inputPSize,
                                          srcLayout.psizeStream,
                                          srcLayout.psizeOffset, 4u)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (inputLoadGate(shaderIo.inputDiffuse) &&
                    !loadColorInput(shaderIo.inputDiffuse, srcLayout.diffuseStream,
                                    srcLayout.diffuseOffset)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (inputLoadGate(shaderIo.inputSpecular) &&
                    !loadColorInput(shaderIo.inputSpecular, srcLayout.specularStream,
                                    srcLayout.specularOffset)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                for (UINT tex = 0; tex < 8; ++tex) {
                    if (inputLoadGate(shaderIo.inputTex[tex]) &&
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
                std::array<float, 4> in{};
                const auto* positionSource =
                    srcBase[srcLayout.positionStream] +
                    sourceOffset(srcLayout.positionStream, i) +
                    srcLayout.positionOffset;
                if (!decodeProcessDeclVector(positionSource,
                                             srcLayout.positionType,
                                             srcLayout.positionBytes,
                                             in)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
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
                if (fixedBlendWeightCount != 0u) {
                    std::array<float, 4> blendWeights{0.0f, 0.0f, 0.0f, 0.0f};
                    std::array<UINT, 4> blendIndices{0u, 1u, 2u, 3u};
                    const auto* blendSource =
                        srcBase[srcLayout.blendWeightStream] +
                        sourceOffset(srcLayout.blendWeightStream, i) +
                        srcLayout.blendWeightOffset;
                    if (!decodeProcessDeclVector(blendSource,
                                                 srcLayout.blendWeightType,
                                                 srcLayout.blendWeightBytes,
                                                 blendWeights)) {
                        hr = D3DERR_INVALIDCALL;
                        break;
                    }
                    if (fixedIndexedVertexBlend) {
                        const auto* indicesSource =
                            srcBase[srcLayout.blendIndicesStream] +
                            sourceOffset(srcLayout.blendIndicesStream, i) +
                            srcLayout.blendIndicesOffset;
                        if (srcLayout.blendIndicesType == D3DDECLTYPE_UBYTE4) {
                            uint8_t indices[4]{};
                            std::memcpy(indices, indicesSource, sizeof(indices));
                            for (UINT c = 0; c < 4u; ++c) {
                                blendIndices[c] = indices[c];
                            }
                        } else if (srcLayout.blendIndicesType == D3DDECLTYPE_D3DCOLOR) {
                            float color[4]{};
                            DWORD packed = 0;
                            std::memcpy(&packed, indicesSource, sizeof(packed));
                            unpackD3DColor(packed, color);
                            for (UINT c = 0; c < 4u; ++c) {
                                blendIndices[c] = static_cast<UINT>(
                                    std::lround(std::clamp(color[c], 0.0f, 1.0f) * 255.0f));
                            }
                        } else {
                            hr = D3DERR_INVALIDCALL;
                            break;
                        }
                    }
                    float worldPosition[4]{};
                    float explicitWeightSum = 0.0f;
                    for (UINT weightIndex = 0;
                         weightIndex < fixedBlendWeightCount; ++weightIndex) {
                        const float weight = blendWeights[weightIndex];
                        explicitWeightSum += weight;
                        float transformed[4]{};
                        transformPoint(position,
                                       transformOrIdentity(D3DTS_WORLDMATRIX(
                                           blendIndices[weightIndex])),
                                       transformed);
                        for (UINT c = 0; c < 4; ++c) {
                            worldPosition[c] += transformed[c] * weight;
                        }
                    }
                    const float implicitWeight = 1.0f - explicitWeightSum;
                    float transformed[4]{};
                    transformPoint(position,
                                   transformOrIdentity(D3DTS_WORLDMATRIX(
                                       blendIndices[fixedBlendWeightCount])),
                                   transformed);
                    for (UINT c = 0; c < 4; ++c) {
                        worldPosition[c] += transformed[c] * implicitWeight;
                    }
                    fixedPosition[0] = worldPosition[0];
                    fixedPosition[1] = worldPosition[1];
                    fixedPosition[2] = worldPosition[2];
                    transformPoint(worldPosition, viewProjection, clip);
                } else {
                    transformPoint(position, wvp, clip);
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
                    std::array<float, 4> normalIn{};
                    const auto* normalSource =
                        srcBase[srcLayout.normalStream] +
                        sourceOffset(srcLayout.normalStream, i) +
                        srcLayout.normalOffset;
                    if (!decodeProcessDeclVector(normalSource,
                                                 srcLayout.normalType,
                                                 srcLayout.normalBytes,
                                                 normalIn)) {
                        lightingColors = {};
                        lightingColorsReady = true;
                        return lightingColors;
                    }
                    float normal[3]{normalIn[0], normalIn[1], normalIn[2]};
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
                    if (!encodeProcessDeclVector(
                            texOut[tex], dstLayout.texType[tex],
                            dstVertex + dstLayout.texOffset[tex])) {
                        hr = D3DERR_INVALIDCALL;
                        break;
                    }
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
        dxmt9PeSetCurrentCallName("PresentEx");
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
            markPePresentReturnedForCadence();
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* p) noexcept override {
        notePeDeviceCallAfterPresent("GetGPUThreadPriority");
        // stub: Wine returns S_OK; GPU thread priority is not exposed by Metal.
        if (p) *p = 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) noexcept override {
        // stub: Wine returns S_OK; GPU thread priority is not exposed by Metal.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT sc) noexcept override {
        notePeDeviceCallAfterPresent("WaitForVBlank");
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
        notePeDeviceCallAfterPresent("GetMaximumFrameLatency");
        if (!p) return D3DERR_INVALIDCALL;
        // PE-shadow: return value previously set or the default of 3.
        *p = maxFrameLatencyShadow_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND wnd) noexcept override {
        notePeDeviceCallAfterPresent("CheckDeviceState");
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
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT sc,
                                                D3DDISPLAYMODEEX* pMode,
                                                D3DDISPLAYROTATION* pRot) noexcept override {
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
};

/* =========================================================================
 * Factory function (called from factory.cpp)
 * ========================================================================= */

IDirect3DDevice9Ex* CreateDeviceImpl(D9CDevice* dev, IDirect3D9Ex* pFactory,
                                     UINT adapter, D3DDEVTYPE deviceType,
                                     DWORD behaviorFlags,
                                     HWND window, bool extended,
                                     DWORD implicitSwapchainFlags) {
    auto* device = new D3D9DeviceImpl(
        dev, pFactory, adapter, deviceType, behaviorFlags, window, extended,
        implicitSwapchainFlags);
    if (!device->commandChunkReady()) {
        delete device;
        return nullptr;
    }
    return device;
}
