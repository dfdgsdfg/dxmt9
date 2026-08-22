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
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include "d3d9_pe.hpp"
#if defined(_WIN32)
// tlhelp32.h requires windows.h (pulled in by d3d9_pe.hpp above) to already
// be visible for HANDLE / DWORD / SIZE_T / ULONG_PTR / WINBOOL.
#include <tlhelp32.h>
#endif
#include "d3d9_pe_device_child.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_decimated_scope.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_render_tape_publisher.hpp"
#include "d3d9_pe_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_descriptors.hpp"
#include "device_c_render_tape_first_access_locator.hpp"
#include "device_c_render_tape_identity.hpp"
#include "device_c_render_tape_origin_locator.hpp"
#include "d3d9_pe_process_vertices.hpp"
#include "d3d9_pe_recorder.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_stats_decimation.hpp"
#include "d3d9_pe_thread_sampler.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9/thread_ownership.hpp"
#include "dxmt9/d3d9_raster_status.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

using dxmt9::d3d9::pe::process_vertices::analyzeSimpleProcessVertexShader;
using dxmt9::d3d9::pe::process_vertices::Context;
using dxmt9::d3d9::pe::process_vertices::describeProcessDeclaration;
using dxmt9::d3d9::pe::process_vertices::describeProcessFvf;
using dxmt9::d3d9::pe::process_vertices::FvfProcessLayout;
using dxmt9::d3d9::pe::process_vertices::processFvfXyzbPosition;
using dxmt9::d3d9::pe::process_vertices::processVertices;
using dxmt9::d3d9::pe::process_vertices::ProcessShaderIo;
using dxmt9::d3d9::pe::process_vertices::vertexElementTypeSize;
using dxmt9::d3d9::RenderTapeQueryDescriptor;
using dxmt9::d3d9::RenderTapeShaderDescriptor;
using dxmt9::d3d9::RenderTapeInitialContentDisposition;
using dxmt9::d3d9::RenderTapeTextureDimension;
using dxmt9::d3d9::RenderTapeTextureDescriptorV2;
using dxmt9::d3d9::RenderTapeVertexDeclDescriptor;

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

#include "d3d9_pe_device_diag_log.inc.hpp"

static bool dxmt9PeRecorderStatsEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_RECORDER_STATS");
    return enabled;
}

// Single resolved-once gate for notePeDeviceCallAfterPresent's per-call
// diagnostic scaffold: dxmt9PeCurrentCallName plus the inter-append pair
// attribution (recordPeBetweenCallsEntry) and the pe_present_call_milestone /
// pe_present_next_call Info logs it feeds are all read only behind
// DXMT9_PE_RECORDER_STATS today. DXMT9_PE_STATS_DECIMATION and
// DXMT9_PE_THREAD_SAMPLER are independent diagnostics that read neither, so
// they must not be OR'd in here. If a future consumer needs this data
// without DXMT9_PE_RECORDER_STATS, extend this predicate rather than adding
// a second unguarded call site.
static bool dxmt9PeCallTrackingEnabled() {
    return dxmt9PeRecorderStatsEnabled();
}

static bool dxmt9PerfVsConstSetterRangeEnabled() {
    static const bool enabled =
        dxmt9::util::getenvFlag("DXMT9_PERF_VS_CONST_SETTER_RANGE");
    return enabled;
}

// Diagnostics-off fast path for the six PE shader-constant setter entry
// points (SetVertexShaderConstantF/I/B, SetPixelShaderConstantF/I/B). GT2
// measures ~21,700 of these calls per present; with every diagnostic
// disabled, the per-call scaffolding these setters carried (a decimated
// scope guard, PeCallScope, a callEntryNs ternary, a finishPeCall lambda,
// notePeDeviceCallAfterPresent, and a debug-log call) costs ~35 ns/call --
// mostly branches and scope-object construction on the disabled path, which
// is expensive under Rosetta-translated x86. This folds every gate that
// scaffold depends on into ONE process-constant bool so the disabled path
// collapses to a single branch straight into validateConstRange +
// touchConstShadow. Every member is read once at process start (each
// accessor below caches its env read in a function-local static), so the
// composite is safe to cache the same way.
static bool dxmt9PeConstSetterSlowPathRequired() {
    static const bool required =
        // dxmt9PeCallTrackingEnabled() -- the gate for
        // notePeDeviceCallAfterPresent / PeCallScope, see the "Single
        // resolved-once gate" comment above -- is defined as exactly
        // dxmt9PeRecorderStatsEnabled(), so one check covers both.
        dxmt9PeRecorderStatsEnabled() ||
        // DXMT9_PE_STATS_DECIMATION: N != 0 arms the per-call decimated
        // scope guard (DxmtPeDecimatedScopeGuard / dxmt9PeArmDecimatedScope
        // against peEntryConstDecimatedStats_, phase 1 per
        // d3d9_pe_const_shadow.hpp).
        dxmt9PeStatsDecimationN() != 0 ||
        // DXMT9_PERF_VS_CONST_SETTER_RANGE.
        dxmt9PerfVsConstSetterRangeEnabled() ||
        // dxmt9DeviceDebugLog emits at LogLevel::Debug; DXMT_LOG_LEVEL is
        // read once into a function-local static by configuredLogLevel(),
        // so this admits-Debug check is itself process-constant.
        dxmt9::util::shouldLog(dxmt9::util::LogLevel::Debug);
    return required;
}

static bool dxmt9PeRecorderChunkLogEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_RECORDER_CHUNK_LOG");
    return enabled;
}

static bool dxmt9PeRenderTapeCaptureEnabled() {
    static const bool enabled =
        dxmt9::util::getenvFlag("DXMT9_RENDER_TAPE_CAPTURE") &&
        [] {
            const auto profile =
                dxmt9::util::getenvString("DXMT9_RENDER_TAPE_PROFILE");
            return dxmt9PeRenderTapeProfileFromText(profile) != 0u;
        }();
    return enabled;
}

static std::uint32_t dxmt9PeRenderTapeCaptureProfile() {
    if (!dxmt9PeRenderTapeCaptureEnabled()) {
        return dxmt9::d3d9::kRenderTapeProfileFrame;
    }
    static const std::uint32_t profile = [] {
        const auto value =
            dxmt9::util::getenvString("DXMT9_RENDER_TAPE_PROFILE");
        return dxmt9PeRenderTapeProfileFromText(value);
    }();
    return profile;
}

static std::uint32_t dxmt9PeRenderTapeCaptureSkipPresents() {
    if (!dxmt9PeRenderTapeCaptureEnabled()) {
        return 0u;
    }
    static const std::uint32_t skip =
        dxmt9::util::getenvU32("DXMT9_RENDER_TAPE_SKIP_PRESENTS").value_or(0u);
    return skip;
}

static dxmt9::d3d9::RenderTapeCaptureLimits
dxmt9PeRenderTapeCaptureLimits(bool captureEnabled) {
    dxmt9::d3d9::RenderTapeCaptureLimits limits{};
    if (captureEnabled) {
        limits.maxBlobBytes = dxmt9PeRenderTapeMaxBlobBytesFromText(
            dxmt9::util::getenvString("DXMT9_RENDER_TAPE_MAX_BLOB_BYTES"));
    }
    return limits;
}

static std::atomic<D3D9PeRenderTapeBootstrapProducer>
    dxmt9PeRenderTapeBootstrapProducer{nullptr};
static std::atomic<D3D9PeRenderTapeArtifactPublisher>
    dxmt9PeRenderTapeArtifactPublisher{nullptr};

void dxmt9PeSetRenderTapeBootstrapProducer(
    D3D9PeRenderTapeBootstrapProducer producer) noexcept {
    dxmt9PeRenderTapeBootstrapProducer.store(producer,
                                             std::memory_order_release);
}

void dxmt9PeSetRenderTapeArtifactPublisher(
    D3D9PeRenderTapeArtifactPublisher publisher) noexcept {
    dxmt9PeRenderTapeArtifactPublisher.store(publisher,
                                             std::memory_order_release);
}

// R-BACK-2.52 (Inline Const Delta, opt-in): read once at first use. Off
// (default/unset) keeps every Draw* record on the pre-existing standalone
// D9C_COMMAND_RECORD_SET_*_CONST_* + fixed-size record path verbatim
// (R-BACK-2.52(a)). On, appendDrawPrimitiveRecord /
// appendDrawIndexedPrimitiveRecord drain the six pending const shadows' merged
// dirty ranges directly into canonical draw-record constant sections (R-BACK-2.52(b)).
// Non-draw const consumers (ProcessVertices, chunkBarrierFlush's drain, the
// UP draw variants — see appendDrawPrimitiveUPRecordWithFvf /
// appendDrawIndexedPrimitiveUPRecordWithFvf) are untouched by this flag and
// always use the standalone flush (R-BACK-2.52(e)).
static bool dxmt9PeInlineConstDeltaEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_INLINE_CONST_DELTA");
    return enabled;
}

// DXMT9_PE_MODULE_MAP (diagnostic, Tier 1 of PE 32-bit symbolication): dump
// the process's loaded-module base/size map and one validation probe address
// so xctrace `time-profile` samples of the opaque 32-bit game thread can be
// joined against real module ranges by
// scripts/tools/symbolicate_xctrace_pe.py. Read once at first use; zero cost
// when unset. Windows-only (Toolhelp32) — this translation unit is compiled
// only in the win32 PE lanes (src/d3d9/meson.build), but the implementation
// is still guarded with #ifdef _WIN32 in case this ever gets pulled into a
// shared header compiled on the host/unix lane.
static bool dxmt9PeModuleMapEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_MODULE_MAP");
    return enabled;
}

static void dxmt9PeModuleMapInfoLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-pe-module-map", fmt, args);
    va_end(args);
}

#ifdef _WIN32
// Marker function used purely as the module-map validation probe: its own
// runtime address, logged alongside the module map, must fall inside our own
// d3d9.dll's [base, base+size) range. The join script asserts this to verify
// its address space matches the Win32 VA space it is joining against.
static void dxmt9PeModuleMapProbeMarker() {}

static void dxmt9PeDumpModuleMap() {
    if (!dxmt9PeModuleMapEnabled()) {
        return;
    }

    const DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        dxmt9PeModuleMapInfoLog(
            "snapshot_failed error=0x%08lx", static_cast<unsigned long>(GetLastError()));
        return;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    const auto probeAddr =
        reinterpret_cast<std::uintptr_t>(&dxmt9PeModuleMapProbeMarker);
    bool probeContained = false;

    if (Module32FirstW(snapshot, &entry)) {
        do {
            char baseName[MAX_PATH]{};
            const int written = WideCharToMultiByte(
                CP_UTF8, 0, entry.szModule, -1, baseName, sizeof(baseName),
                nullptr, nullptr);
            if (written <= 0) {
                std::snprintf(baseName, sizeof(baseName), "<unnamed>");
            }

            const auto base =
                reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            const auto size = static_cast<std::uintptr_t>(entry.modBaseSize);

            dxmt9PeModuleMapInfoLog(
                "module=%s base=0x%llx size=0x%llx",
                baseName,
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(size));

            if (probeAddr >= base && probeAddr < base + size) {
                probeContained = true;
            }
        } while (Module32NextW(snapshot, &entry));
    } else {
        dxmt9PeModuleMapInfoLog(
            "module_enum_failed error=0x%08lx", static_cast<unsigned long>(GetLastError()));
    }

    CloseHandle(snapshot);

    dxmt9PeModuleMapInfoLog(
        "probe=dxmt9PeModuleMapProbeMarker addr=0x%llx contained=%u",
        static_cast<unsigned long long>(probeAddr),
        probeContained ? 1u : 0u);

    // The probe address must land inside our own d3d9.dll's logged module
    // range: if it does not, the enumerated module list does not describe
    // this process's own address space and the join in
    // symbolicate_xctrace_pe.py cannot be trusted.
    DXMT_ASSERT(probeContained);
}
#else
static void dxmt9PeDumpModuleMap() {
    // Non-Windows build of this translation unit: no Toolhelp32 API
    // available. Should not be reachable — src/d3d9/meson.build only
    // compiles d3d9_pe_device.cpp on the windows host_machine lane — but
    // kept as a safe no-op guard in case that build wiring ever changes.
    if (dxmt9PeModuleMapEnabled()) {
        dxmt9PeModuleMapInfoLog("unsupported_platform");
    }
}
#endif

// DXMT9_PE_THREAD_SAMPLER (diagnostic, Tier 2 of PE 32-bit symbolication):
// suspend the game thread `HZ` times a second and classify its true Win32
// program counter against the module map. Read once at first use; zero cost
// when unset. See src/d3d9/d3d9_pe_thread_sampler.hpp for the suspend-window
// safety contract, and agents/rules/environment_variables_bridge.rules.md for
// why a run with this enabled is not a valid performance sample.
static bool dxmt9PeThreadSamplerEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_THREAD_SAMPLER");
    return enabled;
}

static std::uint32_t dxmt9PeThreadSamplerHz() {
    static const std::uint32_t hz = [] {
        // Unset, unparseable, and 0 all fall back to the default rather than
        // clamping to the floor, so a typo reads as "default", not "50".
        const auto parsed = dxmt9::util::getenvU32("DXMT9_PE_THREAD_SAMPLER_HZ");
        const std::uint32_t requested =
            (parsed.has_value() && *parsed != 0)
                ? *parsed
                : dxmt9::d3d9::pe::kPeSamplerDefaultHz;
        return dxmt9::d3d9::pe::clampPeSamplerHz(requested);
    }();
    return hz;
}

static void dxmt9PeThreadSamplerInfoLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-pe-sampler", fmt, args);
    va_end(args);
}

// DXMT9_PE_FORCE_RECORDER_LOCK: rollback/insurance lane for wild apps that
// release resources from loader threads despite not passing
// D3DCREATE_MULTITHREADED. See dxmt9PeRecorderLockRequired() below.
static bool dxmt9PeForceRecorderLockEnabled() {
    static const bool enabled =
        dxmt9::util::getenvFlag("DXMT9_PE_FORCE_RECORDER_LOCK");
    return enabled;
}

// Native D3D9 semantics: the device lock is taken only when the app passed
// D3DCREATE_MULTITHREADED in CreateDevice's BehaviorFlags; without it, the
// app promises single-threaded access and dxmt9 must not pay a
// recursive-mutex lock/unlock on every PE recorder append (measured ~9.6%
// of hot d3d9.dll self-PC on GT2, which does not pass the flag). Pure so it
// is host-testable without a device: flag set -> locked; flag clear + env
// set -> locked (rollback lane); flag clear + env clear -> unlocked.
static bool dxmt9PeRecorderLockRequired(DWORD behaviorFlags,
                                        bool forceLockEnv) noexcept {
    return (behaviorFlags & D3DCREATE_MULTITHREADED) != 0 || forceLockEnv;
}

// Conditional guard for D3D9DeviceImpl::recorderMutex_. When
// recorderLockRequired_ is false (the common case: the app did not pass
// D3DCREATE_MULTITHREADED), this costs exactly one branch on construction
// and one on destruction — no atomic, no clock, no syscall. When true, it
// behaves exactly like the std::lock_guard it replaces.
struct PeRecorderGuard {
    PeRecorderGuard(std::recursive_mutex& mutex, bool locked) noexcept
        : mutex_(mutex), locked_(locked) {
        if (locked_) {
            mutex_.lock();
        }
    }

    ~PeRecorderGuard() {
        if (locked_) {
            mutex_.unlock();
        }
    }

    PeRecorderGuard(const PeRecorderGuard&) = delete;
    PeRecorderGuard& operator=(const PeRecorderGuard&) = delete;

private:
    std::recursive_mutex& mutex_;
    bool locked_;
};

static double dxmt9ElapsedMs(std::chrono::steady_clock::time_point start,
                             std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static std::int64_t dxmt9SteadyClockNs(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        t.time_since_epoch()).count();
}

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

#include "d3d9_pe_device_diag_module.inc.hpp"

// PE call-tracking diagnostic sample (DXMT9_PE_RECORDER_STATS only). This used
// to live in d3d9_pe_device_child.hpp and be returned by value from every noted
// D3D9 entry point; it is file-local now because no hot-path signature -- not
// the device's own entry note, not the child recorder interface -- names it any
// more. See "Observer boundary" in agents/rules/codebase_conventions.rules.md.
static constexpr std::size_t D3D9PePresentCallStackDepth = 12;

struct D3D9PePresentCallToken {
    bool tracked = false;
    std::uint64_t ordinal = 0;
    std::uint32_t callCount = 0;
    std::int64_t returnNs = 0;
    std::int64_t entryNs = 0;
    const void* callerPc = nullptr;
    std::uint32_t threadId = 0;
    std::uint8_t callerStackCount = 0;
    std::array<const void*, D3D9PePresentCallStackDepth> callerStack{};
};

// Diagnostic-owned storage for the entry samples of calls that also emit a
// paired return log. Every slot is owned by an RAII scope whose destructor
// releases it, so `depth` is exactly the live nesting of noted D3D9 entry
// points on this thread (a device method that internally calls another noted
// method). 16 is far above any real chain; a would-be overflow declines to
// track rather than aliasing a live slot.
static constexpr std::size_t kPeCallScopeSlots = 16;
static thread_local D3D9PePresentCallToken
    dxmt9PeCallScopeSlots[kPeCallScopeSlots];
static thread_local std::size_t dxmt9PeCallScopeDepth = 0;

#include "d3d9_pe_device_diag_callstack.inc.hpp"

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
// Phase 16: full-snapshot mode. When set, every draw record carries the
// COMPLETE state -- every category populated from the PE shadow, including null
// unbinds -- instead of the delta since the previous record. Wire size grows
// (typical record ~100B to ~1KB) but each record is replayable independently of
// the ones before it, which is what makes isolated and out-of-order replay
// meaningful for debug and stress.
//
// APPLICATION SITE: buildSparseState() in d3d9_pe_producer.cpp, which per
// category selects the shadow table instead of the pending set (`snapshot ?
// shadow.renderStateShadow : shadow.pendingRenderStates` and its four siblings)
// and emits every bound slot rather than only the dirty ones.
// addChunkContextSections() takes the same verdict and then leaves the stream
// set alone: a delta-shaped rebuild there silently dropped the very sections
// snapshot mode exists to emit, which is what c3e18446 fixed.
//
// One caveat the name oversells: the index-buffer section is still subject to
// chunk retention under snapshot, so a snapshot record is not literally
// self-contained for the IB. That matches what the legacy format did, and the
// goldens pin it.
//
// Equivalence guard: tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp
// runs the real producer in both modes over identical inputs and asserts the
// snapshot record is self-contained and the delta record is a subset of it.
// dxmt9PeFullSnapshotEnabled() now lives in d3d9_pe_producer.hpp beside the
// producer that reads it. Keeping a second copy here would be exactly the
// drift hazard that motivated sharing toWireHandle.

#include "d3d9_pe_device_com_cold_helpers.inc.hpp"

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

static_assert(
    dxmt9::d3d9::render_tape_d3d_format::X4R4G4B4 ==
    static_cast<std::uint32_t>(D3DFMT_X4R4G4B4));
static_assert(
    dxmt9::d3d9::render_tape_d3d_format::R32F ==
    static_cast<std::uint32_t>(D3DFMT_R32F));

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

#include "d3d9_pe_device_tape_types.inc.hpp"

#include "d3d9_pe_device_tape_helpers.inc.hpp"

#include "d3d9_pe_device_com_shader_validators.inc.hpp"

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

}  // namespace

/* =========================================================================
 * Raw-handle extractors — safe because only our device creates these objects.
 * ========================================================================= */

static D9CSurface*   rawSurf(IDirect3DSurface9* p)          { return D3D9PeRawSurface(p); }
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

#include "d3d9_pe_device_impl.hpp"

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
