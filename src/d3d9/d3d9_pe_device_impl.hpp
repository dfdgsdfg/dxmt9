#pragma once

/* src/d3d9/d3d9_pe_device_impl.hpp — PE-side IDirect3DDevice9Ex and recorder glue.
 * All methods delegate to the dxmt9c_* C API from dxmt9/device_c.h.
 *
 * This is a header rather than a TU so the cold subsystems (SWVP, PE
 * diagnostics, Render Tape capture, cold COM) can be compiled separately
 * while the hot recorder/append path stays visible for inlining. Everything
 * at namespace scope here is `inline`: a `static` would give each including
 * TU its own copy, and for the mutable state (the Render Tape producer /
 * publisher atomics, the PE call-tracking thread_locals) separate copies
 * would be a correctness bug, not just duplication. */

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
#include <memory>
#include <mutex>
#include <new>
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
#include "d3d9_pe_child_context.hpp"
#include "d3d9_pe_child_factories.hpp"
#include "d3d9_pe_child_scopes.hpp"
#include "d3d9_pe_child_validation.hpp"
#include "d3d9_pe_diagnostic_observer.hpp"
#include "d3d9_pe_stateblock_shadow.hpp"
#include "d3d9_pe_com_cache.hpp"
#include "d3d9_pe_capture_state.hpp"
#include "d3d9_pe_commit_transition.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_decimated_scope.hpp"
#include "d3d9_pe_diagnostics_state.hpp"
#include "d3d9_pe_fvf_transaction.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_render_tape_publisher.hpp"
#include "d3d9_pe_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_descriptors.hpp"
#include "device_c_render_tape_first_access_locator.hpp"
#include "device_c_render_tape_identity.hpp"
#include "device_c_render_tape_origin_locator.hpp"
#include "d3d9_pe_process_vertices.hpp"
#include "d3d9_pe_public_allocation.hpp"
#include "d3d9_pe_recorder.hpp"
#include "d3d9_pe_recorder_settlement.hpp"
#include "d3d9_pe_recorder_state.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_stateblock_transaction.hpp"
#include "d3d9_pe_stats_decimation.hpp"
#include "d3d9_pe_thread_sampler.hpp"
#include "d3d9_pe_wire_handle.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9/pe_recorder_lock.hpp"
#include "dxmt9/thread_ownership.hpp"
#include "dxmt9/d3d9_raster_status.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

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

inline D3DFORMAT exposeAdapterDisplayFormat(D3DFORMAT fmt) {
    if (fmt == D3DFMT_A8R8G8B8) return D3DFMT_X8R8G8B8;
    return fmt;
}

inline void dxmt9DeviceDebugLogImpl(const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    try {
        dxmt9::util::vlogf(
            dxmt9::util::LogLevel::Debug, "dxmt9-device", fmt, args);
    } catch (...) {
        // Logging is diagnostic-only and may allocate internally.  No COM/
        // Wine entry point may terminate because that allocation failed.
    }
    va_end(args);
}

inline bool dxmt9DeviceDebugLogEnabled() noexcept {
    static const bool enabled =
        dxmt9::util::shouldLog(dxmt9::util::LogLevel::Debug);
    return enabled;
}

// Keep disabled debug calls at one cached branch: arguments are not evaluated,
// and no varargs frame/function call is emitted on the hot PE path.
#define dxmt9DeviceDebugLog(...) \
    do { \
        if (dxmt9DeviceDebugLogEnabled()) { \
            dxmt9DeviceDebugLogImpl(__VA_ARGS__); \
        } \
    } while (false)

inline void dxmt9DeviceInfoLog(const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    try {
        dxmt9::util::vlogf(
            dxmt9::util::LogLevel::Info, "dxmt9-device", fmt, args);
    } catch (...) {
        // Best-effort diagnostics cannot escape a noexcept COM boundary.
    }
    va_end(args);
}

inline bool dxmt9PeRecorderStatsEnabled() {
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
inline bool dxmt9PeCallTrackingEnabled() {
    return dxmt9PeRecorderStatsEnabled();
}

inline bool dxmt9PerfVsConstSetterRangeEnabled() {
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
inline bool dxmt9PeConstSetterSlowPathRequired() {
    static const bool required =
        // dxmt9PeCallTrackingEnabled() -- the gate for
        // notePeDeviceCallAfterPresent / PeCallScope, see the "Single
        // resolved-once gate" comment above -- is defined as exactly
        // dxmt9PeRecorderStatsEnabled(), so one check covers both.
        dxmt9PeRecorderStatsEnabled() ||
        // DXMT9_PE_STATS_DECIMATION: N != 0 arms the per-call decimated
        // scope guard (DxmtPeDecimatedScopeGuard / dxmt9PeArmDecimatedScope
        // against diagnostics_->peEntryConstDecimatedStats_, phase 1 per
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

inline bool dxmt9PeRecorderChunkLogEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_RECORDER_CHUNK_LOG");
    return enabled;
}

inline bool dxmt9PeRenderTapeCaptureEnabled() {
    static const bool enabled =
        dxmt9::util::getenvFlag("DXMT9_RENDER_TAPE_CAPTURE") &&
        [] {
            const auto profile =
                dxmt9::util::getenvString("DXMT9_RENDER_TAPE_PROFILE");
            return dxmt9PeRenderTapeProfileFromText(profile) != 0u;
        }();
    return enabled;
}

inline std::uint32_t dxmt9PeRenderTapeCaptureProfile() {
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

inline std::uint32_t dxmt9PeRenderTapeCaptureSkipPresents() {
    if (!dxmt9PeRenderTapeCaptureEnabled()) {
        return 0u;
    }
    static const std::uint32_t skip =
        dxmt9::util::getenvU32("DXMT9_RENDER_TAPE_SKIP_PRESENTS").value_or(0u);
    return skip;
}

inline dxmt9::d3d9::RenderTapeCaptureLimits
dxmt9PeRenderTapeCaptureLimits(bool captureEnabled) {
    dxmt9::d3d9::RenderTapeCaptureLimits limits{};
    if (captureEnabled) {
        limits.maxBlobBytes = dxmt9PeRenderTapeMaxBlobBytesFromText(
            dxmt9::util::getenvString("DXMT9_RENDER_TAPE_MAX_BLOB_BYTES"));
    }
    return limits;
}

inline std::atomic<D3D9PeRenderTapeBootstrapProducer>
    dxmt9PeRenderTapeBootstrapProducer{nullptr};
inline std::atomic<D3D9PeRenderTapeArtifactPublisher>
    dxmt9PeRenderTapeArtifactPublisher{nullptr};

inline void dxmt9PeSetRenderTapeBootstrapProducer(
    D3D9PeRenderTapeBootstrapProducer producer) noexcept {
    dxmt9PeRenderTapeBootstrapProducer.store(producer,
                                             std::memory_order_release);
}

inline void dxmt9PeSetRenderTapeArtifactPublisher(
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
inline bool dxmt9PeInlineConstDeltaEnabled() {
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
inline bool dxmt9PeModuleMapEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_MODULE_MAP");
    return enabled;
}

inline void dxmt9PeModuleMapInfoLog(const char* fmt, ...) {
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
inline void dxmt9PeModuleMapProbeMarker() {}

inline void dxmt9PeDumpModuleMap() {
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
inline void dxmt9PeDumpModuleMap() {
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
inline bool dxmt9PeThreadSamplerEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_THREAD_SAMPLER");
    return enabled;
}

// Optional exact source-ordinal witness for formal/native projection audits.
// It is deliberately cold and default-off: ordinary correctness continues to
// use the allocation-free typed PendingDelta/value preflight.
inline bool dxmt9PeScalarSemanticObserverEnabled() {
    static const bool enabled =
        dxmt9::util::getenvFlag("DXMT9_PE_SCALAR_SEMANTIC_OBSERVER");
    return enabled;
}

inline std::uint32_t dxmt9PeThreadSamplerHz() {
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

inline PeDiagnosticsConfig dxmt9PeResolvedDiagnosticsConfig() {
    return PeDiagnosticsConfig{
        .recorderStats = dxmt9PeRecorderStatsEnabled(),
        .recorderChunkLog = dxmt9PeRecorderChunkLogEnabled(),
        .statsDecimationN = dxmt9PeStatsDecimationN(),
        .vsConstSetterRange = dxmt9PerfVsConstSetterRangeEnabled(),
        .moduleMap = dxmt9PeModuleMapEnabled(),
        .threadSampler = dxmt9PeThreadSamplerEnabled(),
        .debugLog = dxmt9::util::shouldLog(dxmt9::util::LogLevel::Debug),
        .scalarSemanticObserver = dxmt9PeScalarSemanticObserverEnabled(),
        .copyMaterializationLedger =
            dxmt9::core::copyMaterializationLedgerEnabled(),
        .threadSamplerHz = dxmt9PeThreadSamplerHz(),
    };
}

inline bool dxmt9PeChunkCommitDiagnosticsEnabled() {
    static const bool enabled =
        dxmt9PeRecorderStatsEnabled() || dxmt9PeRecorderChunkLogEnabled();
    return enabled;
}

inline void dxmt9PeThreadSamplerInfoLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-pe-sampler", fmt, args);
    va_end(args);
}

// DXMT9_PE_FORCE_RECORDER_LOCK: rollback/insurance lane for wild apps that
// release resources from loader threads despite not passing
// D3DCREATE_MULTITHREADED. See dxmt9PeRecorderLockRequired() below.
inline bool dxmt9PeForceRecorderLockEnabled() {
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
inline bool dxmt9PeRecorderLockRequired(DWORD behaviorFlags,
                                        bool forceLockEnv) noexcept {
    return dxmt9::d3d9::pe::recorderLockRequired(
        static_cast<std::uint32_t>(behaviorFlags), forceLockEnv);
}

// Conditional guard for D3D9DeviceImpl::recorderState_.recorderMutex. When
// recorderState_.recorderLockRequired is false (the common case: the app did not pass
// D3DCREATE_MULTITHREADED), this costs exactly one branch on construction
// and one on destruction — no atomic, no clock, no syscall. When true, it
// behaves exactly like the std::lock_guard it replaces.
using PeRecorderGuard = dxmt9::d3d9::pe::RecorderLockGuard;

inline double dxmt9ElapsedMs(std::chrono::steady_clock::time_point start,
                             std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

inline std::int64_t dxmt9SteadyClockNs(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        t.time_since_epoch()).count();
}

inline std::uint32_t dxmt9PeCurrentThreadId() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentThreadId());
#else
    return 0;
#endif
}

inline thread_local const char* dxmt9PeCurrentCallName = nullptr;
inline thread_local PeInterAppendCallFamily dxmt9PeCurrentAppendFamily =
    PeInterAppendCallFamily::Unknown;
inline thread_local std::int64_t dxmt9PeCurrentCallEntryNs = 0;

inline void dxmt9PeSetCurrentCallName(const char* callName) noexcept {
    if (dxmt9PeCallTrackingEnabled()) {
        dxmt9PeCurrentCallName = callName;
    }
}

class Dxmt9PeAppendFamilyScope {
public:
    Dxmt9PeAppendFamilyScope(PeDiagnosticsState* diagnostics,
                            PeInterAppendCallFamily family) noexcept
        : active_(diagnostics && diagnostics->config.recorderStats) {
        if (active_) {
            previous_ = dxmt9PeCurrentAppendFamily;
            dxmt9PeCurrentAppendFamily = family;
        }
    }

    ~Dxmt9PeAppendFamilyScope() noexcept {
        if (active_) {
            dxmt9PeCurrentAppendFamily = previous_;
        }
    }

    Dxmt9PeAppendFamilyScope(const Dxmt9PeAppendFamilyScope&) = delete;
    Dxmt9PeAppendFamilyScope& operator=(
        const Dxmt9PeAppendFamilyScope&) = delete;

private:
    bool active_ = false;
    PeInterAppendCallFamily previous_ = PeInterAppendCallFamily::Unknown;
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
inline thread_local D3D9PePresentCallToken
    dxmt9PeCallScopeSlots[kPeCallScopeSlots];
inline thread_local std::size_t dxmt9PeCallScopeDepth = 0;

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

inline D9CRect toR(const RECT& r) {
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

/// D3D9 shader-version token form: ((stageHi << 16) | (major << 8) | minor).
/// stageHi == 0xFFFE for vertex shaders, 0xFFFF for pixel shaders.
constexpr uint32_t kShaderHeaderVS = 0xFFFEu;
constexpr uint32_t kShaderHeaderPS = 0xFFFFu;
constexpr uint32_t kShaderMaxMajor = 3u; /* vs_3_0 / ps_3_0 are the cap */
constexpr uint32_t kShaderEndToken = 0x0000FFFFu;
constexpr size_t kShaderBoundedScan = 1u << 16;

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

// (end of the formerly-anonymous namespace; see the note above it)

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

class D3D9DeviceImpl final : public IDirect3DDevice9Ex {
    friend class D3D9PeDiagnosticObserver;
    friend struct D3D9PeStateBlockContext;
    friend struct D3D9PeBufferContext;
    friend struct D3D9PeSurfaceTextureContext;
    friend struct D3D9PeQueryContext;
    friend struct D3D9PePresentationContext;
    friend struct D3D9PeShaderDeclarationContext;
    // Chunk-flush thresholds. The historical defaults (64 records /
    // 256 KiB, tuned around Phase 5) were promoted to 4x on 2026-08-19
    // after the measured GT2 evidence in
    // docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.21.md:
    // fewer, larger chunk seals return ~1.2 ms/present of game-thread
    // CPU (44 -> 8 seals/present) for a reproducible median fps win
    // (GT2 +2.0% non-overlapping, GT1 +3.5%, GT3 +2.5%, SFIV
    // percentile-equal) with byte-identical locality (CB/pass/subCB
    // per present) and conserved encode pacing. Both remain
    // env-overridable via DXMT9_PE_CHUNK_MAX_RECORDS /
    // DXMT9_PE_CHUNK_MAX_BYTES — the pre-promotion cadence is
    // DXMT9_PE_CHUNK_MAX_RECORDS=64 DXMT9_PE_CHUNK_MAX_BYTES=262144 —
    // and the helpers below cap env values to prevent pathological
    // inputs from blowing chunk-side allocations. Note the two move
    // together: the frozen legacy sizeHints make the byte precheck
    // bind near 53 draw records at the old byte cap, so raising only
    // the record cap is inert.
    static constexpr UINT kDefaultMaxPendingCommandRecords = 256;
    static constexpr size_t kDefaultMaxPendingCommandBytes = 1310720;
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

    ULONG        refs_    = 1;
    D9CDevice*   dev_;
    IDirect3D9Ex* factory_;
    UINT         adapter_ = 0;
    D3DDEVTYPE   deviceType_ = D3DDEVTYPE_HAL;
    DWORD        behaviorFlags_ = 0;
    // Native D3D9 semantics: the device lock is taken only when the app
    // passed D3DCREATE_MULTITHREADED; otherwise the app promises
    // single-threaded access and recorderState_.recorderMutex is skipped on the
    // hot append path. Resolved once at construction — see
    // dxmt9PeRecorderLockRequired(). DXMT9_PE_FORCE_RECORDER_LOCK is the
    // rollback/insurance lane for apps that violate the contract.
    dxmt9::d3d9::pe::PeRecorderState recorderState_{};
    D3D9PeStateBlockContext stateBlockContext_{};
    D3D9PeBufferContext bufferContext_{};
    D3D9PeSurfaceTextureContext surfaceTextureContext_{};
    D3D9PeQueryContext queryContext_{};
    D3D9PePresentationContext presentationContext_{};
    D3D9PeShaderDeclarationContext shaderDeclarationContext_{};
    // R-BACK-43.4 `producer-owned` (PE game thread) — the PE recorder, the
    // chunk builder it owns, and their retainer are written and read only on
    // the thread that constructed this device, EXCEPT under the documented
    // R-BACK-43.5 shape-(c) exception: with D3DCREATE_MULTITHREADED (or the
    // DXMT9_PE_FORCE_RECORDER_LOCK rollback lane) recorderState_.recorderMutex
    // covers cross-thread access instead.
    //
    // Debug-only thread-confinement companion to the conditional lock: this
    // token binds to the constructing thread and assertRecorderThreadConfined()
    // DXMT_ASSERTs that no other thread calls a recorder-guarded path while the
    // lock is skipped — catching app UB (calling cross-thread without
    // D3DCREATE_MULTITHREADED) loudly in debug builds instead of silently
    // corrupting the recorder. Shared helper per R-BACK-43.5; this site is the
    // reference shape the helper was extracted from.
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
    /* Per-index unix swap-chain handle, owned for exactly as long as
     * swapchainWrappers_ above (both are dropped by releaseAllBound(), which
     * runs on the device destructor, Reset and ResetEx). Every call to
     * dxmt9c_device_get_swap_chain allocates a fresh D9CSwapChain wrapper AND
     * its bridge entry carries a deferred-replay drain fence — GT2 measured it
     * at 617.9 us/call, the single most expensive getter crossing in the run —
     * while the object it wraps is stable: core::Device::swapChains_ is only
     * appended to (CreateAdditionalSwapChain) and resized in place
     * (resetValidated), never erased or replaced. Resolve one handle per index
     * and borrow it. */
    std::unordered_map<UINT, D9CSwapChain*> swapchainHandles_{};

    IDirect3DSurface9* rtSlots_[4]{};
    bool rtSlotExplicit_[4]{};
    IDirect3DSurface9* dsSurface_ = nullptr;

    // Section-sized sparse compatibility storage lives in recorderState_ and
    // is reserved for oversized, Render Tape, and SWVP override projections.
    // Ordinary draws/APPLY_STATE use a compact call-local plan.
    bool dsSurfaceExplicit_ = false;
    // The complete Render Tape lifecycle is cold. The nullable owner leaves
    // only one pointer in the normal renderer and constructs no capture
    // storage when capture/tracking is disabled.
    std::unique_ptr<PeCaptureState> peCaptureState_{};
    // Every optional PE diagnostic payload lives behind this one pointer.
    // Capture correctness state remains in peCaptureState_, and recorder
    // protocol accounting remains in recorderState_.
    std::unique_ptr<PeDiagnosticsState> diagnostics_{};

    /* present params copy for GetCreationParameters */
    HWND creationWindow_ = nullptr;
    D3D9PeWsiBinding implicitWsiBinding_{};
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
    void initGammaRampIdentity() noexcept;

    template<typename T>
    static void setRef(T*& slot, T* newVal) noexcept {
        if (newVal) newVal->AddRef();
        if (slot)   slot->Release();
        slot = newVal;
    }

    StateBlockTextureRef stateBlockTextureRef(
        IDirect3DBaseTexture9* value) const noexcept;
    StateBlockStreamSourceValue::BufferRef stateBlockBufferRef(
        IDirect3DVertexBuffer9* value) const noexcept;
    StateBlockVertexShaderRef stateBlockVertexShaderRef(
        IDirect3DVertexShader9* value) const noexcept;
    StateBlockPixelShaderRef stateBlockPixelShaderRef(
        IDirect3DPixelShader9* value) const noexcept;
    StateBlockVertexDeclarationRef stateBlockVertexDeclarationRef(
        IDirect3DVertexDeclaration9* value) const noexcept;
    StateBlockIndexBufferRef stateBlockIndexBufferRef(
        IDirect3DIndexBuffer9* value) const noexcept;
    StateBlockRenderTargetRef stateBlockSurfaceRef(
        IDirect3DSurface9* value) const noexcept;
    StateBlockDepthStencilRef stateBlockDepthStencilRef(
        IDirect3DSurface9* value) const noexcept;

    void discardPreparedStateBlockApply() noexcept;

    void poisonStateBlockTransaction() noexcept;

    template<typename Table, typename Key, typename Validated>
    static void setRecordedRef(Table table, Key slot,
                                const Validated& validated) noexcept {
        // Invalid capabilities are rejected before either mutation or retain.
        if (!slot.valid()) return;
        using Ref = typename Table::value_type;
        Ref prior{};
        (void)table.get(slot, prior);
        using Tag = typename StateBlockComRefTagFor<Ref>::type;
        if (prior.raw() == validated.publicIdentity()) return;
        const Ref next = StateBlockComRefFactory<Tag>::fromValidated(
            validated.template stateBlockCapability<Tag>());
        d3d9PeRetainStateBlockRef(next);
        d3d9PeReleaseStateBlockRef(prior);
        table.set(slot, next);
    }

    template<typename Table, typename Key, typename Validated>
    static void setRecordedStreamRef(
        Table table, Key slot, const Validated& validated) noexcept {
        if (!slot.valid()) return;
        StateBlockStreamSourceValue prior{};
        (void)table.get(slot, prior);
        if (prior.buffer == validated.publicIdentity()) return;
        const auto next = StateBlockBufferRefFactory::fromValidated(
            validated.stateBlockBufferCapability());
        d3d9PeRetainStateBlockRef(next);
        d3d9PeReleaseStateBlockRef(prior.buffer);
        prior.buffer = next;
        table.set(slot, prior);
    }

    void releaseRecordedStateBlockRefs() noexcept;

    D9CTexture* validatedRawTexture(IDirect3DBaseTexture9* texture) const noexcept;

    D9CSurface* validatedRawSurface(IDirect3DSurface9* surface) const noexcept;

    bool applyCurrentPaletteToTexture(IDirect3DBaseTexture9* texture);

    void applyCurrentPaletteToBoundTextures();

    DWORD renderStateValue(D3DRENDERSTATETYPE state) const;

    /* Borrowed per-index unix swap-chain handle; see swapchainHandles_.
     * The returned pointer must NOT be released by the caller — it is dropped
     * with the rest of the swap-chain cache in releaseAllBound(). */
    D9CSwapChain* borrowSwapChainHandle(UINT index);

    void releaseAllBound();

    void clearPendingCommandChunk(
        dxmt9::d3d9::pe::RecorderCommitEvent discardEvent =
            dxmt9::d3d9::pe::RecorderCommitEvent::ExplicitDiscard);

    // The semantic ledgers are cold, nullable owners. Keep these accessors
    // visible to the in-class hot setters/append envelope so the disabled
    // path pays only the cached diagnostics/null branch; an out-of-line
    // accessor call would be unconditional even when the pointer is null.
    dxmt9::d3d9::pe::PeScalarSemanticTokenLedger*
    scalarSemanticObserver() noexcept {
        return diagnostics_ ? diagnostics_->scalarSemanticTokens.get() : nullptr;
    }

    dxmt9::d3d9::pe::PeAllFamilySemanticTokenLedger*
    allFamilySemanticObserver() noexcept {
        return diagnostics_ ? diagnostics_->allFamilySemanticTokens.get() : nullptr;
    }

    bool observeCommittedSemanticRecord(
        std::uint32_t recordType, std::uint64_t sourceOrdinal,
        const dxmt9::d3d9::pe::CommandChunkBuilder& builder) noexcept;

    void clearPeStateTracking();

    bool hasPendingHotState() const;

    void clearPendingHotState();

    bool shadowedRenderStateEquals(DWORD state, DWORD value) const;

    bool shadowedStreamSourceEquals(UINT stream,
                                    IDirect3DVertexBuffer9* buffer,
                                    UINT offset,
                                    UINT stride) const {
        return stream < 16 && streamSrc_[stream] == buffer &&
               streamOff_[stream] == offset && streamStr_[stream] == stride;
    }

    dxmt9::d3d9::pe::PeRtExplicitMask currentRtExplicitMask() const;

    std::uint64_t currentVertexShaderHash() const noexcept;

    std::uint64_t currentPixelShaderHash() const noexcept;

    static const char* vsConstSetterRangePhaseName(
        VsConstSetterRangePhase phase) noexcept;

    static std::size_t vsConstSetterRangePhaseIndex(
        VsConstSetterRangePhase phase) noexcept;

    void recordVsConstSetterRange(VsConstSetterRangePhase phase,
                                  std::uint64_t vsHash,
                                  std::uint64_t psHash,
                                  std::uint32_t start,
                                  std::uint32_t count,
                                  std::uint32_t changedRegs,
                                  std::uint32_t changedSpanRegs) noexcept;

    void logVsConstSetterRangePerf(const char* event);

    // Fill the producer's binding view from the COM members. Reuses a member
    // rather than a local so the ~850-byte view is not zero-initialized on
    // every draw.
    //
    // `needAllSlots` mirrors the producer's own snapshot gate. It matters for
    // cost, not correctness: textureHandle() is not a cast -- the trusted
    // virtual GetType() call per bound texture -- so translating all 20 texture
    // and 16 stream slots unconditionally would do roughly 45 raw* calls per
    // draw where the delta path historically did about 9. At GT2's ~1,700
    // packet builds per present that is tens of thousands of extra virtual
    // calls, on the same hot path aefde46f had just cleaned up. So the delta
    // fill translates exactly the pending slots the old inline code did, and
    // only the snapshot path drains everything.
    //
    // Fills the FULL PeWireObjectRef -- identity as well as object -- via the
    // ref accessors return each wrapper's cached reference.
    //
    // Identity is not optional: CommandChunkBuilder::appendHandle rejects a
    // ref whose identity is zero (PeWireObjectRef::valid checks kind,
    // generation and objectId) and fails the whole record through
    // failActiveRecord() with NO log line. An earlier revision filled only
    // `object`, on the reasoning that nothing read identity yet; the moment
    // APPLY_STATE started going through appendApplyState that turned into
    // "IDirect3DDevice9::Clear failed: Invalid call" with nothing explaining
    // why, and the harness still reported status=pass because it does not gate
    // on the rendered image.
    //
    // These accessors are also cheaper than the raw* pair they replaced for
    // surfaces and buffers: wireObject() is a member read where rawSurf() went
    // through a cast and textureHandle() made a virtual GetType()
    // call. The texture ref accessor still switches on GetType(), so the texture loop
    // keeps its pending-mask guard.
    // `allStreams` makes the stream half authoritative for EVERY slot, not just
    // pending ones. Draw sites need that: chunk-context re-emission asks "what
    // is bound in slot N", and a non-pending slot otherwise holds whatever the
    // last build that touched it left behind -- stale after
    // clearPeStateTracking()/Reset, which clears streamOff_/streamStr_ and the
    // shadow but not this view. It costs what production already paid: the draw
    // sites called currentDrawStreamSources(), which reads all 16 slots.
    void populateBindingView(dxmt9::d3d9::pe::PeBindingView& view,
                             bool needAllSlots,
                             bool allStreams = false) const;

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
    // Destination-chunk retention, as data. MUST be read after any CapacityPre
    // flush has resealed the chunk -- that is why the draw sites build it inside
    // the append emitter and not before.
    dxmt9::d3d9::pe::PeChunkContext currentChunkContext() const;

    // Compatibility producer for Render Tape, oversized batches, and SWVP
    // override packets. Ordinary APPLY_STATE and non-UP draws use the compact
    // two-pass SparseStatePlan path below and never write section-sized scratch.
    // The decimated draw_packet scope lives here for
    // the same reason it does on the fat-packet forwarder: it has to cover the
    // COM-to-wire binding translation, not just the section fill.
    // The shadow is const through this producer boundary. Preparation is
    // intentionally non-consuming; acceptInlineConstantDelta is the only
    // operation that settles represented constant ranges after append
    // acceptance. The recorder lock/producer ownership contract makes this
    // prepare/accept interval non-reentrant.
    // params.recordType is the ONLY record-type input, deliberately: this used
    // to take a separate recordType argument and stamp it onto params. Because
    // params was taken by value, the stamp landed on the local copy only, so
    // every caller that later handed its own params to addChunkContextSections
    // passed recordType == 0. That reads as "not an indexed draw", and the
    // context step does not merely skip the index section -- it rebuilds the
    // span as first(0), wiping the one buildSparseState had already emitted
    // for pendingIb. SetIndices records nothing standalone in chunk mode, so
    // every indexed draw then replayed against a stale index buffer: GT1 came
    // out as sliver triangles with garbled HUD digits. Two sources of truth for
    // one value, one of them silently write-only. Now there is one.
    bool buildSparseStateForRecord(
        const dxmt9::d3d9::pe::PeDrawParams& params,
        bool forceFullSnapshot = false,
        bool inlineConstDelta = false);

    bool buildSparseStatePlanForRecord(
        const dxmt9::d3d9::pe::RecorderLockCapability& access,
        const dxmt9::d3d9::pe::PeDrawParams& params,
        const dxmt9::d3d9::pe::PeDrawPayloads& payloads,
        dxmt9::d3d9::pe::SparseStatePlan& plan,
        bool forceFullSnapshot = false,
        bool inlineConstDelta = false);
    static UINT primitiveVertexCount(D3DPRIMITIVETYPE type, UINT primitiveCount);

    static bool checkedByteCount(UINT count, UINT stride, std::uint32_t& bytes);

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
            UINT index) const;

	    static HRESULT appendTransformedSwvpVertex(
	            const std::vector<std::uint8_t>& source,
	            UINT stride,
	            UINT index,
	            std::vector<std::uint8_t>& out);

	    struct SwvpClippedVertex {
	        std::vector<std::uint8_t> bytes;
	    };

	    static HRESULT copyTransformedSwvpVertex(
	            const std::vector<std::uint8_t>& source,
	            UINT stride,
	            UINT index,
	            SwvpClippedVertex& out);

	    static float swvpReadFloat(const std::vector<std::uint8_t>& bytes,
	                               UINT offset);

	    static void swvpWriteFloat(std::vector<std::uint8_t>& bytes,
	                               UINT offset,
	                               float value);

	    static DWORD swvpReadDword(const std::vector<std::uint8_t>& bytes,
	                               UINT offset);

	    static void swvpWriteDword(std::vector<std::uint8_t>& bytes,
	                               UINT offset,
	                               DWORD value);

	    static DWORD interpolateD3dColor(DWORD a, DWORD b, float t);

	    static SwvpClippedVertex interpolateTransformedSwvpVertex(
	            const SwvpClippedVertex& a,
	            const SwvpClippedVertex& b,
	            DWORD fvf,
	            UINT stride,
	            float t);

	    std::vector<std::uint32_t> transformedSwvpActiveClipPlanes() const;

	    float transformedSwvpVertexPlaneDistance(
	            const SwvpClippedVertex& vertex,
	            std::uint32_t planeFlag) const;

	    HRESULT clipTransformedSwvpTriangle(
	            DWORD fvf,
	            UINT stride,
	            const SwvpClippedVertex& a,
	            const SwvpClippedVertex& b,
	            const SwvpClippedVertex& c,
	            std::vector<std::uint8_t>& out,
	            UINT& primitiveCount) const;

	    HRESULT clipTransformedSwvpLine(
	            DWORD fvf,
	            UINT stride,
	            const SwvpClippedVertex& a,
	            const SwvpClippedVertex& b,
	            std::vector<std::uint8_t>& out,
	            UINT& primitiveCount) const;

    static DWORD processFfpDeclarationOutputFvf(
            const FvfProcessLayout& srcLayout,
            bool lighting,
            bool specularLighting,
            bool allowBlendAttributes);

    HRESULT describeSoftwareFfpDrawTarget(DWORD& outputFvf,
                                          FvfProcessLayout& srcLayout,
                                          FvfProcessLayout& dstLayout);

    static DWORD processProgrammableOutputFvf(const ProcessShaderIo& shaderIo);

    static bool processLayoutUsesOnlyStream0(const FvfProcessLayout& layout);

    static bool softwareDrawCanConcatenateInstances(D3DPRIMITIVETYPE type);

    static bool softwareDrawCanExpandInstances(D3DPRIMITIVETYPE type);

    static D3DPRIMITIVETYPE softwareDrawExpandedPrimitiveType(
            D3DPRIMITIVETYPE type);

    static HRESULT appendSoftwarePrimitiveVertices(
            const std::vector<std::uint8_t>& source,
            UINT stride,
            D3DPRIMITIVETYPE type,
            UINT primitiveCount,
            std::vector<std::uint8_t>& out);

    HRESULT filterSoftwareDrawOutsideClipPrimitives(SoftwareFfpDrawData& draw);

    static HRESULT readSoftwareIndexValue(const std::vector<std::uint8_t>& indices,
                                          D3DFORMAT indexFormat,
                                          UINT ordinal,
                                          DWORD& out);

    static HRESULT appendSoftwareIndex32(std::vector<std::uint8_t>& indices,
                                         DWORD index);

    HRESULT filterSoftwareIndexedDrawOutsideClipPrimitives(
            SoftwareFfpDrawData& draw,
            std::vector<std::uint8_t>& indices,
            D3DFORMAT& indexFormat);

    UINT softwareDrawInstanceCount() const;

    HRESULT applySoftwareInstanceStreamOffsets(UINT instance,
                                               UINT savedOffsets[16]);

    void restoreSoftwareInstanceStreamOffsets(const UINT savedOffsets[16]);

    // SWVP draw preparation owns all candidate vectors until the public draw
    // has accepted the candidate. Keep the temporary UP stream override and
    // instancing offsets reversible even when vector growth throws.
    template <typename Prepare>
    __attribute__((noinline))
    HRESULT prepareSoftwareDrawCandidate(Prepare&& prepare) noexcept;

    HRESULT describeSoftwareProgrammableDrawTarget(
        DWORD& outputFvf,
        FvfProcessLayout& srcLayout,
        FvfProcessLayout& dstLayout);

    HRESULT readTransformedVertexBuffer(IDirect3DVertexBuffer9* dstBuffer,
                                        UINT bytes,
                                        SoftwareFfpDrawData& out,
                                        DWORD outputFvf,
                                        UINT outputStride);

    HRESULT trySoftwareProgrammableTransformBoundVertices(
        UINT startVertex,
        UINT vertexCount,
        SoftwareFfpDrawData& out);

    HRESULT trySoftwareFfpTransformBoundVertices(UINT startVertex,
                                                 UINT vertexCount,
                                                 SoftwareFfpDrawData& out);

    HRESULT trySoftwareFfpDrawPrimitive(D3DPRIMITIVETYPE type,
                                        UINT startVertex,
                                        UINT primitiveCount,
                                        SoftwareFfpDrawData& out);

    HRESULT trySoftwareProgrammableDrawPrimitive(D3DPRIMITIVETYPE type,
                                                 UINT startVertex,
                                                 UINT primitiveCount,
                                                 SoftwareFfpDrawData& out);

    HRESULT readSoftwareFfpAdjustedIndices(UINT startIndex,
                                           UINT indexCount,
                                           UINT minVertex,
                                           UINT numVertices,
                                           std::vector<std::uint8_t>& out,
                                           D3DFORMAT& indexFormat);

    static HRESULT appendSoftwareIndicesWithBase32(
        const std::vector<std::uint8_t>& source,
        D3DFORMAT sourceFormat,
        UINT indexCount,
        UINT baseVertex,
        std::vector<std::uint8_t>& out);

    static HRESULT appendSoftwarePrimitiveIndicesWithBase32(
        const std::vector<std::uint8_t>& source,
        D3DFORMAT sourceFormat,
        D3DPRIMITIVETYPE type,
        UINT primitiveCount,
        UINT baseVertex,
        std::vector<std::uint8_t>& out);

    HRESULT trySoftwareFfpDrawIndexedPrimitive(D3DPRIMITIVETYPE type,
                                               INT baseVertex,
                                               UINT minVertex,
                                               UINT numVertices,
                                               UINT startIndex,
                                               UINT primitiveCount,
                                               SoftwareFfpDrawData& out,
                                               std::vector<std::uint8_t>& indices,
                                               D3DFORMAT& indexFormat);

    HRESULT trySoftwareProgrammableDrawIndexedPrimitive(
        D3DPRIMITIVETYPE type,
        INT baseVertex,
        UINT minVertex,
        UINT numVertices,
        UINT startIndex,
        UINT primitiveCount,
        SoftwareFfpDrawData& out,
        std::vector<std::uint8_t>& indices,
        D3DFORMAT& indexFormat);

    HRESULT trySoftwareFfpDrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                          UINT primitiveCount,
                                          const void* data,
                                          UINT stride,
                                          SoftwareFfpDrawData& out);

    HRESULT trySoftwareProgrammableDrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                                   UINT primitiveCount,
                                                   const void* data,
                                                   UINT stride,
                                                   SoftwareFfpDrawData& out);

    HRESULT trySoftwareFfpDrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                 UINT minVertex,
                                                 UINT numVertices,
                                                 UINT primitiveCount,
                                                 const void* vertexData,
                                                 UINT stride,
                                                 SoftwareFfpDrawData& out);

    HRESULT trySoftwareProgrammableDrawIndexedPrimitiveUP(
        D3DPRIMITIVETYPE type,
        UINT minVertex,
        UINT numVertices,
        UINT primitiveCount,
        const void* vertexData,
        UINT stride,
        SoftwareFfpDrawData& out);

    struct PePresentCadenceClaim {
        bool claimed = false;
        std::uint64_t ordinal = 0;
        std::int64_t returnNs = 0;
        std::int64_t entryNs = 0;
    };

    using PePresentCallSample = D3D9PePresentCallToken;

    PePresentCadenceClaim claimPeFirstCallAfterPresent();

    void logPeFirstCallAfterPresent(const char* callName,
                                    const PePresentCadenceClaim& claim,
                                    const PePresentCallSample& sample);

    // The whole entry note, reached only with tracking on. It writes the entry
    // sample into `out`, which is always diagnostic-owned storage -- either a
    // call-scope slot or a throwaway local -- never a hot-path return value.
    void notePeDeviceCallAfterPresentTracked(const char* callName,
                                             const void* callerPc,
                                             PePresentCallSample& out);

    // Hot path: this is called on every D3D9 device entry. With no diagnostic
    // consumer enabled (see dxmt9PeCallTrackingEnabled) it is one cached-bool
    // test and nothing else -- no sample is constructed and none crosses the
    // call boundary. Entry points that also emit the paired return log use
    // PeCallScope below. See "Observer boundary" in
    // agents/rules/codebase_conventions.rules.md.
    void notePeDeviceCallAfterPresent(const char* callName,
                                      const void* callerPc = nullptr) {
        if (!dxmt9PeCallTrackingEnabled()) {
            return;
        }
        PePresentCallSample sample;
        notePeDeviceCallAfterPresentTracked(callName, callerPc, sample);
    }

    // Slot API behind PeCallScope / D3D9PeChildCallScope. Push answers
    // kD3D9PePresentCallSlotNone when nothing was tracked, which is the single
    // cached-bool test on the disabled path.
    D3D9PePresentCallSlot pushPeCallScope(const char* callName,
                                          const void* callerPc) noexcept {
        if (!dxmt9PeCallTrackingEnabled()) {
            return kD3D9PePresentCallSlotNone;
        }
        if (dxmt9PeCallScopeDepth >= kPeCallScopeSlots) {
            return kD3D9PePresentCallSlotNone;
        }
        const std::size_t slot = dxmt9PeCallScopeDepth++;
        notePeDeviceCallAfterPresentTracked(callName, callerPc,
                                            dxmt9PeCallScopeSlots[slot]);
        return static_cast<D3D9PePresentCallSlot>(slot);
    }

    void notePeCallScopeReturn(D3D9PePresentCallSlot slot,
                               const char* callName, HRESULT hr) noexcept {
        if (slot == kD3D9PePresentCallSlotNone) {
            return;
        }
        logPeCallReturnAfterPresent(dxmt9PeCallScopeSlots[slot], callName, hr);
    }

    void popPeCallScope(D3D9PePresentCallSlot slot) noexcept {
        // Scopes nest, so the slot being released is always the top one.
        if (slot != kD3D9PePresentCallSlotNone &&
            dxmt9PeCallScopeDepth == static_cast<std::size_t>(slot) + 1u) {
            dxmt9PeCallScopeDepth = slot;
        }
    }

    // Enabled-only RAII scope for a device entry point that pairs its entry
    // note with a return log. Callers branch on the nullable diagnostics owner
    // before this object's lifetime begins, so the disabled edge constructs no
    // scope and performs no clock, TLS, sample-storage, or observer work. The
    // ~96-byte entry sample stays in diagnostic-owned slot storage. A return
    // path that skips finish() leaks nothing because the destructor releases
    // the slot. See "Observer boundary" in
    // agents/rules/codebase_conventions.rules.md.
    class PeCallScope {
    public:
        __attribute__((always_inline))
        PeCallScope(PeDiagnosticsState& diagnostics,
                    const char* callName,
                    const void* callerPc = nullptr,
                    PeDecimatedScopeStats PeDiagnosticsState::*
                        entryStats = nullptr) noexcept
            : diagnostics_(&diagnostics) {
            startEnabled(callName, callerPc, entryStats);
        }
        PeCallScope(const PeCallScope&) = delete;
        PeCallScope& operator=(const PeCallScope&) = delete;
        __attribute__((always_inline))
        ~PeCallScope() noexcept {
            finishEnabled();
        }

        __attribute__((always_inline))
        HRESULT finish(const char* callName, HRESULT hr) noexcept {
            if (observer_) {
                observer_->notifyCallScopeReturn(slot_, callName, hr);
            }
            return hr;
        }

        PeDecimatedScopeStats* decimatedStats() const noexcept {
            return decimatedStats_;
        }

        DxmtPeDecimatedPhaseTimer phase(
            PeDecimatedScopeStats PeDiagnosticsState::* stats) const noexcept {
            return DxmtPeDecimatedPhaseTimer(
                decimatedStats_ != nullptr, &(diagnostics_->*stats));
        }

    private:
        __attribute__((noinline))
        void startEnabled(
            const char* callName, const void* callerPc,
            PeDecimatedScopeStats PeDiagnosticsState::* entryStats) noexcept {
            const std::uint32_t decimationN =
                diagnostics_->config.statsDecimationN;
            if (entryStats && decimationN != 0) {
                auto& stats = diagnostics_->*entryStats;
                if (PeDecimatedScopeTimer::shouldSample(stats, decimationN)) {
                    decimatedStats_ = &stats;
                    const auto n0 = std::chrono::steady_clock::now();
                    const auto n1 = std::chrono::steady_clock::now();
                    PeDecimatedScopeTimer::recordSample(
                        peDecimatedNullScopeStats(),
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                n1 - n0).count()));
                    decimatedEntry_ = std::chrono::steady_clock::now();
                }
            }
            if (diagnostics_->config.recorderStats) {
                observer_ = &diagnostics_->childObserver;
                slot_ = observer_->pushCallScope(callName, callerPc);
            }
        }

        __attribute__((noinline))
        void finishEnabled() noexcept {
            if (observer_) {
                observer_->popCallScope(slot_);
            }
            if (decimatedStats_) {
                PeDecimatedScopeTimer::recordSample(
                    *decimatedStats_,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() -
                            decimatedEntry_).count()));
            }
        }

        PeDiagnosticsState* diagnostics_ = nullptr;
        D3D9PeDiagnosticObserver* observer_ = nullptr;
        D3D9PePresentCallSlot slot_ = kD3D9PePresentCallSlotNone;
        PeDecimatedScopeStats* decimatedStats_ = nullptr;
        std::chrono::steady_clock::time_point decimatedEntry_{};
    };

    void markPePresentReturnedForCadence();

    static bool peCallMilestoneBit(std::uint32_t callCount,
                                   std::uint32_t& bit) noexcept;

    PePresentCallSample logPeCallMilestoneAfterPresent(const char* callName,
                                                       const void* callerPc);

    void logPeCallReturnAfterPresent(const PePresentCallSample& sample,
                                     const char* callName,
                                     HRESULT hr);

    static const char* peCommandRecordTypeName(std::uint32_t type) noexcept;

    static std::uint32_t peCommandRecordTypeBucket(std::uint32_t type) noexcept;

    static const char*
    peInterAppendCallFamilyName(std::uint32_t family) noexcept;

    static PeInterAppendCallFamily
    peInterAppendCallFamilyFromName(const char* callName) noexcept;

    static const char*
    peInterAppendCallNameName(std::uint32_t callName) noexcept;

    static PeInterAppendCallName
    peInterAppendCallNameFromName(const char* callName) noexcept;

    static std::size_t peInterAppendFocusPairIndex(std::uint32_t prevType,
                                                   std::uint32_t nextType) noexcept;

    static std::size_t peInterAppendFocusCallFamilyIndex(
        std::size_t focusPair,
        PeInterAppendCallFamily family) noexcept;

    static std::size_t peInterAppendFocusCallTransitionIndex(
        std::size_t focusPair,
        PeInterAppendCallFamily prevFamily,
        PeInterAppendCallFamily nextFamily) noexcept;

    static std::size_t peInterAppendFocusCallNameTransitionIndex(
        std::size_t focusPair,
        PeInterAppendCallName prevCallName,
        PeInterAppendCallName nextCallName) noexcept;

    static std::size_t peInterAppendCallTransitionIndex(
        PeInterAppendCallFamily prevFamily,
        PeInterAppendCallFamily nextFamily) noexcept;

    static std::size_t peInterAppendCallNameTransitionIndex(
        PeInterAppendCallName prevCallName,
        PeInterAppendCallName nextCallName) noexcept;

    static std::size_t peInterAppendPairIndex(std::uint32_t prevType,
                                              std::uint32_t nextType) noexcept;

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
    topPeInterAppendPairs() const noexcept;

    std::array<PeInterAppendCallFamilySummary,
               kPeRecorderInterAppendTopCallFamilyCount>
    topPeInterAppendFocusCallFamilies(std::size_t focusPair) const noexcept;

    std::array<PeInterAppendCallFamilySummary,
               kPeRecorderInterAppendTopCallFamilyCount>
    topPeInterAppendFocusBetweenCallFamilies(std::size_t focusPair) const noexcept;

    std::array<PeInterAppendCallNameSummary,
               kPeRecorderInterAppendTopCallNameCount>
    topPeInterAppendFocusBetweenCallNames(std::size_t focusPair) const noexcept;

    std::array<PeInterAppendCallTransitionSummary,
               kPeRecorderInterAppendTopCallTransitionCount>
    topPeInterAppendFocusBetweenCallTransitions(
        std::size_t focusPair) const noexcept;

    std::array<PeInterAppendCallNameTransitionSummary,
               kPeRecorderInterAppendTopCallTransitionCount>
    topPeInterAppendFocusBetweenCallNameTransitions(
        std::size_t focusPair) const noexcept;

    std::array<PeInterAppendCallSiteSummary,
               kPeRecorderInterAppendTopCallTransitionCount>
    topPeInterAppendFocusBetweenCallNameTransitionSites(
        std::size_t focusPair) const;

    static bool peRecordMilestoneBit(std::uint32_t recordCount,
                                     std::uint32_t& bit) noexcept;

    void logPeRecordMilestoneAfterPresent(std::uint32_t type,
                                          std::uint32_t recordCount,
                                          std::uint32_t payloadBytes,
                                          std::int64_t entryNs);

    PePresentCadenceClaim claimPeFirstChunkAfterPresent();

    void logPeFirstChunkAfterPresent(PeRecorderFlushReason reason,
                                     const PePresentCadenceClaim& claim,
                                     HRESULT hr,
                                     const PeCommandChunkCommitInfo& info);

    void recordPeChunkCommit(PeRecorderFlushReason reason,
                             std::uint32_t recordCount,
                             std::uint32_t payloadBytes,
                             std::uint32_t handleCount,
                             std::uint32_t wireBytes,
                             std::uint64_t fillGapNs,
                             std::uint64_t activeFillNs,
                             std::uint64_t bridgeNs);

    void recordPeStateBlockFault(bool entered, std::int32_t hr) noexcept;

    void recordPeChunkInterAppendGap(std::int64_t appendEntryNs,
                                     std::uint32_t recordCountBefore,
                                     std::uint32_t nextType);

    void recordPeChunkInterAppendFocusPhaseSplit(std::size_t focusPair,
                                                 std::int64_t appendEntryNs);

    void recordPeChunkInterAppendFocusTailSplit(std::size_t focusPair);

    void recordPeBetweenCallsEntry(const char* callName,
                                   std::int64_t entryNs,
                                   const void* callerPc);

    void recordPeBetweenCallsReturn(const char* callName,
                                    std::int64_t entryNs,
                                    std::int64_t exitNs);

    void resetPeBetweenCallsWindow();

    void recordPeChunkInterAppendFocusBetweenCallFamilies(std::size_t focusPair);

    void notePeCurrentCallReturnForInterAppendSplit();

    void recordPeAppendCpu(std::uint64_t appendCpuNs, bool noFlushAppend);

    void recordPeConstSetterCpu(std::uint32_t recordType,
                                std::int64_t entryNs,
                                std::uint32_t regs);

    void recordPeConstFlushCpu(std::uint32_t recordType,
                               std::int64_t entryNs,
                               std::uint32_t records,
                               std::uint32_t regs);

    void recordPeChunkBarrierConstCpu(std::int64_t entryNs);

    void recordPeApplyStateBuildCpu(std::int64_t entryNs);

    void recordPeHotStateSetterCpu(PeHotStateSetterFamily family,
                                   std::int64_t entryNs,
                                   bool dirty);

    class PeHotStateSetterTimer {
    public:
        __attribute__((always_inline))
        PeHotStateSetterTimer(D3D9DeviceImpl& device,
                              PeDiagnosticsState& diagnostics,
                              PeHotStateSetterFamily family,
                              const char* callName = nullptr,
                              PeDecimatedScopeStats PeDiagnosticsState::*
                                  entryStats = nullptr,
                              const void* callerPc = nullptr) noexcept
            : device_(&device), diagnostics_(&diagnostics),
              family_(family) {
            startEnabled(callName, entryStats, callerPc);
        }

        __attribute__((always_inline))
        ~PeHotStateSetterTimer() noexcept {
            finishEnabled();
        }

        __attribute__((always_inline))
        void markDirty() noexcept {
            dirty_ = true;
        }

    private:
        __attribute__((noinline))
        void startEnabled(
            const char* callName,
            PeDecimatedScopeStats PeDiagnosticsState::* entryStats,
            const void* callerPc) noexcept {
            const std::uint32_t decimationN =
                diagnostics_->config.statsDecimationN;
            if (entryStats && decimationN != 0) {
                auto& stats = diagnostics_->*entryStats;
                if (PeDecimatedScopeTimer::shouldSample(stats, decimationN)) {
                    decimatedStats_ = &stats;
                    const auto n0 = std::chrono::steady_clock::now();
                    const auto n1 = std::chrono::steady_clock::now();
                    PeDecimatedScopeTimer::recordSample(
                        peDecimatedNullScopeStats(),
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                n1 - n0).count()));
                    decimatedEntry_ = std::chrono::steady_clock::now();
                }
            }
            if (callName && diagnostics_->config.recorderStats) {
                PePresentCallSample sample;
                device_->notePeDeviceCallAfterPresentTracked(
                    callName, callerPc, sample);
            }
            if (diagnostics_->config.recorderStats) {
                entryNs_ = dxmt9SteadyClockNs(
                    std::chrono::steady_clock::now());
            }
        }

        __attribute__((noinline))
        void finishEnabled() noexcept {
            if (entryNs_ > 0) {
                device_->recordPeHotStateSetterCpu(family_, entryNs_, dirty_);
            }
            if (decimatedStats_) {
                PeDecimatedScopeTimer::recordSample(
                    *decimatedStats_,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() -
                            decimatedEntry_).count()));
            }
        }

        D3D9DeviceImpl* device_ = nullptr;
        PeDiagnosticsState* diagnostics_ = nullptr;
        PeHotStateSetterFamily family_;
        std::int64_t entryNs_ = 0;
        PeDecimatedScopeStats* decimatedStats_ = nullptr;
        std::chrono::steady_clock::time_point decimatedEntry_{};
        bool dirty_ = false;
    };

    struct PeNullPhaseTimer {
        void stop() const noexcept {}
    };

    struct PeNullCallScope {
        HRESULT finish(const char*, HRESULT hr) const noexcept {
            return hr;
        }

        PeDecimatedScopeStats* decimatedStats() const noexcept {
            return nullptr;
        }

        PeNullPhaseTimer phase(
            PeDecimatedScopeStats PeDiagnosticsState::*) const noexcept {
            return {};
        }
    };

    struct PeNullHotStateSetterTimer {
        void markDirty() const noexcept {}
    };

    inline static constexpr PeNullCallScope peNullCallScope_{};
    inline static constexpr PeNullHotStateSetterTimer peNullHotSetter_{};

    template<typename Body>
    __attribute__((always_inline))
    HRESULT withPeCallScope(
        const char* callName, const void* callerPc,
        PeDecimatedScopeStats PeDiagnosticsState::* entryStats,
        Body&& body) noexcept {
        PeDiagnosticsState* const diagnostics = diagnostics_.get();
        if (!diagnostics) {
            return std::forward<Body>(body)(peNullCallScope_);
        }
        if (!diagnostics->gates.callScope) {
            return std::forward<Body>(body)(peNullCallScope_);
        }
        PeCallScope peCall(
            *diagnostics, callName, callerPc, entryStats);
        return std::forward<Body>(body)(peCall);
    }

    template<typename Body>
    __attribute__((always_inline))
    HRESULT withPeHotStateSetter(
        PeHotStateSetterFamily family, const char* callName,
        PeDecimatedScopeStats PeDiagnosticsState::* entryStats,
        const void* callerPc, Body&& body) noexcept;

    template<typename Body>
    __attribute__((always_inline))
    HRESULT withPeCallAndHotStateSetter(
        const char* callName, const void* callerPc,
        PeDecimatedScopeStats PeDiagnosticsState::* callEntryStats,
        PeHotStateSetterFamily family,
        PeDecimatedScopeStats PeDiagnosticsState::* hotEntryStats,
        Body&& body) noexcept {
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
        if (!recorderState_.stateBlockTransaction.writeAllowed()) {
            return D3DERR_DEVICELOST;
        }
        PeDiagnosticsState* const diagnostics = diagnostics_.get();
        if (!diagnostics) {
            return std::forward<Body>(body)(
                peNullCallScope_, peNullHotSetter_);
        }
        if (!diagnostics->gates.callScope) {
            return std::forward<Body>(body)(
                peNullCallScope_, peNullHotSetter_);
        }
        PeCallScope peCall(
            *diagnostics, callName, callerPc, callEntryStats);
        PeHotStateSetterTimer hotSetter(
            *this, *diagnostics, family, nullptr, hotEntryStats, nullptr);
        return std::forward<Body>(body)(peCall, hotSetter);
    }

    void notePeChunkAppendBoundary(std::int64_t appendReturnNs,
                                   std::uint32_t type);

    void logPeRecorderStats(const char* event, bool force = false);

    // DXMT9_PE_STATS_DECIMATION: emit ONE cumulative [dxmt9-pe-decimated]
    // line covering all four decimated hot-scope accumulators (append,
    // const_setter, const_flush, draw_packet). Counters are cumulative
    // (never reset) — estimation (sampled_ms * N / presents) is done
    // offline, not here. No-op when the knob is unset/0/unparseable.
    void logPeStatsDecimation();

    // DXMT9_PE_THREAD_SAMPLER: start one sampler targeting the thread that
    // created the device. That is the game thread by construction — D3D9
    // device creation is what the renderer thread does — and it is the thread
    // whose PE-side samples xctrace cannot attribute.
    void startPeThreadSamplerIfRequested();

    void stopPeThreadSampler();

    // Emits ONE cumulative [dxmt9-pe-sampler] group: a header line, one line
    // per module with a nonzero count (top 20 by count), then the self-module
    // PC histogram's top buckets. Counters are cumulative and never reset, so
    // a consumer reads the LAST group in the log.
    void logPeThreadSampler();

    // Present-cadence tick for the sampler dump, mirroring the decimation
    // cadence: cumulative line every 60 presents, plus a final line from the
    // destructor so the last partial interval is never lost.
    void notePeThreadSamplerPresent();

    // Present-cadence tick for the decimated dump: increments a cumulative
    // present counter and emits the cumulative line every 60 presents. A
    // final line is also emitted unconditionally from the destructor so the
    // last partial interval is never lost. No-op when decimation is off.
    void notePeStatsDecimationPresent();
    void notePeCopyMaterializationPresent();
    void logPeCopyMaterializationLedger();
    void recordDrawPrimitiveUPCopy(std::uint32_t vertexBytes);

    void recordDrawIndexedPrimitiveUPCopy(std::uint32_t vertexBytes,
                                          std::uint32_t indexBytes);

    RenderTapeLiveObject *findRenderTapeObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept;

    const RenderTapeLiveObject *findRenderTapeObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) const noexcept;

    bool hasRenderTapeDeadObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) const noexcept;

    void markRenderTapeInvalidOnce(
        const char *reason,
        const dxmt9::d3d9::pe::PeWireObjectRef *object = nullptr,
        std::uint32_t subresource =
            std::numeric_limits<std::uint32_t>::max(),
        const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic =
            {}) noexcept;

    void abortRenderTapeCapture(const char *reason) noexcept;

    RenderTapeObjectRegistration registerRenderTapeObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::span<const std::byte> descriptor,
        std::span<const std::byte> immutablePayload,
        RenderTapeLiveObject::Role role = RenderTapeLiveObject::Role::Ordinary,
        std::uint32_t replacementRestart = 0u) noexcept;

    // Hand the PresentOutput role back before a new admission names a holder,
    // and again whenever an arm attempt ends without an active interval. A
    // retained holder is the r6 GT2 failure: every retry re-admitted a fresh
    // back-buffer wrapper while the previous one stayed live and roled, so the
    // arm saw two through eight present outputs, and a recycled wire object id
    // then collided with the stale entry and marked the registry invalid for
    // the rest of the process.
    void releaseRenderTapePresentOutputRole(
        const D9CWireObjectIdentity *next) noexcept;

    bool admitRenderTapePresentOutput() noexcept;

    bool unregisterRenderTapeObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept;

    bool recordRenderTapeCpuBytes(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, std::uint64_t byteOffset,
        std::span<const std::byte> bytes) noexcept;

    bool renderTapeObjectSubresourceDesc(
        const RenderTapeLiveObject &entry,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, D9CSurfaceDesc &out) const noexcept;

    dxmt9::d3d9::RenderTapeBlockMutationStatus recordRenderTapeBlockBytes(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
        std::span<const std::byte> bytes) noexcept;

    dxmt9::d3d9::RenderTapeBlockMutationStatus recordRenderTapeLinearBytes(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
        std::span<const std::byte> bytes) noexcept;

    void logRenderTapeMutationFailure(
        const char *route,
        dxmt9::d3d9::RenderTapeBlockMutationStatus status,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, std::uint32_t fullRowBytes,
        std::uint32_t fullRows, std::uint32_t rowBytes,
        std::uint32_t rows, std::uint32_t pitch,
        std::span<const std::byte> bytes) const noexcept;

    bool produceRenderTapeBootstrap(
        dxmt9::d3d9::RenderTapeCaptureBootstrapSeed &seed) noexcept;

    bool advanceRenderTapeArmBoundary(
        dxmt9::d3d9::RenderTapeArmBoundaryPhase requested) noexcept;

    bool snapshotRenderTapeResourcesAtArmBoundary() noexcept;

    // An arm attempt that does not reach an active interval must hand the
    // PresentOutput role back immediately. Deferring it to the next attempt
    // leaves a stale live entry across the window in which the C-side wire
    // registry can recycle its object id at a newer generation.
    bool armRenderTapeCaptureAtPresentBoundary();

    bool armRenderTapeCaptureAtPresentBoundaryInterval();

    // Bounded capture-rejection attribution shared by the CpuUnlock append
    // branches. Every field is already-owned session state; nothing here
    // relaxes a predicate or raises a capacity.
    void logRenderTapeMutationRejection(
        const char *reason, const char *detail,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, std::uint64_t bytes,
        dxmt9::d3d9::RenderTapeCaptureStatus status) noexcept;

    // Append one CpuUnlock mutation event for an already-admitted object and
    // attribute the exact failing branch. The r6 GT2 log reported only
    // `first_abort reason=block_resource_mutation`, which cannot distinguish a
    // missing registry entry, a subresource outside the admitted content
    // shape, blob admission, and the mutation event itself. The two session
    // steps are therefore driven separately here — `resourceMutationBytes`
    // performs exactly this blob-register-then-mutate pair, so splitting it
    // narrows attribution without changing what is admitted or when the tape
    // fails closed.
    bool appendRenderTapeUnlockMutation(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, const char *reason) noexcept;

    bool renderTapeObjectAdmitted(
        const D9CWireObjectIdentity &identity) const noexcept;

    void removeRenderTapeObjectAdmitted(
        const D9CWireObjectIdentity &identity) noexcept;

    bool materializeRenderTapeObjectForReference(
        const D9CWireObjectIdentity &identity,
        std::uint32_t handleIndex =
            std::numeric_limits<std::uint32_t>::max(),
        std::uint32_t recordIndex =
            std::numeric_limits<std::uint32_t>::max(),
        std::uint32_t recordType = 0u,
        const dxmt9::d3d9::RenderTapeOriginLocator *originLocator =
            nullptr,
        const dxmt9::d3d9::ImportedChunkView *currentChunk = nullptr) noexcept;

    bool admitRenderTapeChunkHandles(
        const D9CCommandChunk &chunk,
        const PeCommandChunkCommitInfo &info) noexcept;

    void observeRenderTapeFirstAccessChunk(
        const D9CCommandChunk &chunk,
        const PeCommandChunkCommitInfo &info) noexcept;

    bool prepareRenderTapeChunkCapture(
        const D9CCommandChunk& chunk,
        const PeCommandChunkCommitInfo& info) noexcept;

    void captureCommittedRenderTapeChunk(
        const D9CCommandChunk& chunk,
        const PeCommandChunkCommitInfo& info) noexcept;

    static bool chunkHasPresentRecord(const D9CCommandChunk& chunk) noexcept;

    void finishRenderTapeCaptureAtPresentBoundary() noexcept;

    HRESULT commitPendingCommandChunk(
        PeRecorderFlushReason commitReason, const D9CCommandChunk& chunk,
        const PeCommandChunkCommitInfo& info,
        dxmt9::d3d9::pe::RecorderCommitPhase* settledPhase = nullptr);

    HRESULT flushPendingCommandChunk(
        PeRecorderFlushReason reason,
        PeRecorderFlushDisposition disposition =
            PeRecorderFlushDisposition::Submit);

    using AppendPhaseTimer = PeAppendPhaseTimer;
    // Append envelope. Owns the recorder mutex, the negotiation gate, the
    // CapacityPre and CapacityPost flushes, and every append telemetry site;
    // `emit` supplies only the record itself.
    //
    // `sizeHint` feeds the capacity precheck and therefore decides where
    // chunks are cut. A caller that no longer builds a legacy record must
    // still pass a value on the same scale as the legacy record it replaced,
    // or chunk seal cadence shifts -- which no per-record test can observe.
    //
    // `emit` is invoked as HRESULT(CommandChunkBuilder&, const AppendPhaseTimer&).
    // It returns an HRESULT rather than bool so an emitter can distinguish
    // E_OUTOFMEMORY from a malformed record.
    //
    // The return type is asserted, not merely documented, because HRESULT is
    // LONG: an emitter that returned the canonical emitters' natural `bool` would
    // convert `false` to 0 == S_OK, so a failed append would be reported as a
    // success. CapacityPost would run, notePeChunkAppendBoundary would count a
    // record that was never appended, and the record would vanish with no
    // error. Emitters must spell out `? S_OK : D3DERR_INVALIDCALL`.
    // Chunk seal cadence is a behavioural contract, not a size. appendRecord's
    // sizeHint is what the capacity precheck compares against, so changing it
    // moves where chunks seal and therefore which draws share a chunk. These are
    // the sizes of the legacy records each site used to build, preserved verbatim
    // so cadence stays bit-identical to the pre-migration recorder.
    //
    // Frozen deliberately. Their origin -- sizeof() of a legacy record struct --
    // is deleted by this commit, so nothing regenerates them and nothing should:
    // the number IS the contract now. Every sparse record is far smaller than its
    // legacy counterpart, so each of these is an over-estimate, which is the safe
    // direction (seal earlier, never overrun a chunk).
    //
    // Worth revisiting, but NOT here: now that the legacy format is gone, cadence
    // no longer has to match the legacy era -- it only has to be sensible. Using
    // true sparse sizes would seal chunks far less often, which is a perf change
    // needing paired GT1/GT2 evidence, not a side effect of a deletion commit.
    static constexpr std::size_t kLegacyApplyStateSizeHint = 4888u;
    static constexpr std::size_t kLegacyClearSizeHint = 32u;
    static constexpr std::size_t kLegacyColorFillSizeHint = 40u;
    static constexpr std::size_t kLegacyDrawIndexedPrimitiveSizeHint = 4920u;
    static constexpr std::size_t kLegacyDrawIndexedPrimitiveUPSizeHint = 4924u;
    static constexpr std::size_t kLegacyDrawPrimitiveSizeHint = 4888u;
    static constexpr std::size_t kLegacyDrawPrimitiveUPSizeHint = 4904u;
    static constexpr std::size_t kLegacyPresentSizeHint = 64u;
    static constexpr std::size_t kLegacyQueryIssueSizeHint = 24u;
    static constexpr std::size_t kLegacyReadbackSizeHint = 24u;
    static constexpr std::size_t kLegacyReszDepthResolveSizeHint = 24u;
    static constexpr std::size_t kLegacySetConstSizeHint = 16u;
    static constexpr std::size_t kLegacyStretchRectSizeHint = 72u;
    static constexpr std::size_t kLegacyUpdateSurfaceSizeHint = 64u;
    static constexpr std::size_t kLegacyUpdateTextureSizeHint = 24u;

    template<typename EmitFn>
    HRESULT appendRecord(uint32_t type, size_t sizeHint, EmitFn emit) {
        static_assert(
            std::is_same_v<
                decltype(emit(std::declval<dxmt9::d3d9::pe::CommandChunkBuilder&>(),
                              std::declval<const AppendPhaseTimer&>())),
                HRESULT>,
            "appendRecord emitters must return HRESULT, not bool: a bool "
            "false would silently convert to S_OK");
        const size_t bytes = sizeHint;
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
        // The common append boundary owns the fail-stop gate so child writers
        // such as Query::Issue cannot bypass the setter-specific checks. This
        // precedes negotiation, capacity accounting, and either flush edge;
        // Reset recovery does not append and remains able to clear poison.
        if (recorderState_.stateBlockTransaction.isPoisoned()) {
            return D3DERR_DEVICELOST;
        }
        if (!recorderState_.commandChunkNegotiated || bytes == 0u || bytes > 0xffffffffull) {
            return recorderState_.commandChunkNegotiated ? D3DERR_INVALIDCALL
                                           : D3DERR_NOTAVAILABLE;
        }
        auto* const allFamilyTokens = allFamilySemanticObserver();
        const std::uint64_t semanticSourceOrdinal = allFamilyTokens
            ? allFamilyTokens->beginSource(type)
            : 0u;
        if (allFamilyTokens && semanticSourceOrdinal == 0u) {
            return D3DERR_INVALIDCALL;
        }
        const auto maxRecords = maxPendingCommandRecords();
        const auto maxBytes = maxPendingCommandBytes();
        const auto recordCountBefore = recorderState_.commandChunk.recordCount();
        const auto payloadBytesBefore = recorderState_.commandChunk.payloadBytes();
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
        // Gate the clock reads, not just their consumers. All four users of
        // these two timestamps (recordPeChunkInterAppendGap, recordPeAppendCpu,
        // notePeChunkAppendBoundary, logPeRecordMilestoneAfterPresent) early-out
        // on this same flag, which is off in every perf/production run -- so the
        // two steady_clock::now() calls were being paid per append and thrown
        // away. At GT2's 2,707 appends/present and the ~187ns/call this codebase
        // measured for its own null scope, that is ~1.0ms/present, about 1.9% of
        // the frame. The flag is a cached static bool, so the branch is free.
        const bool peStatsEnabled = dxmt9PeRecorderStatsEnabled();
        const std::int64_t appendEntryNs =
            peStatsEnabled ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
                           : 0;
        if (peStatsEnabled) {
            recordPeChunkInterAppendGap(appendEntryNs, recordCountBefore, type);
        }
        HRESULT hr = S_OK;
        DxmtPeDecimatedScopeGuard appendDecimatedScope;
        const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
        if (decimationN != 0 &&
            PeDecimatedScopeTimer::shouldSample(
                diagnostics_->peChunkAppendDecimatedStats_, decimationN)) {
            appendDecimatedScope.stats = &diagnostics_->peChunkAppendDecimatedStats_;
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
            const auto typeBucket = PeDiagnosticsState::peAppendTypeBucket(type);
            ++diagnostics_->peAppendTypeCounts_[typeBucket];
            diagnostics_->peAppendTypeBytes_[typeBucket] += bytes;
        }
        const bool phaseSampled = appendDecimatedScope.stats != nullptr;
        const AppendPhaseTimer phase{
            phaseSampled ? diagnostics_.get() : nullptr};
        if (willFlushBeforeAppend) {
            const auto t0 = phase.begin();
            hr = flushPendingCommandChunk(PeRecorderFlushReason::CapacityPre);
            phase.recordFlush(t0);
            const auto plan = dxmt9::d3d9::pe::planRecorderSettlement({
                .point = dxmt9::d3d9::pe::RecorderSettlementPoint::CapacityPre,
                .result = dxmt9::d3d9::pe::recorderFlushSettlementResult(
                    SUCCEEDED(hr),
                    recorderState_.stateBlockTransaction.isPoisoned()),
            });
            DXMT_ASSERT(plan.valid() &&
                        (SUCCEEDED(hr) ||
                         ((plan.retryable() || plan.poison()) &&
                          !plan.acceptedRecord())));
            (void)plan;
            if (FAILED(hr) && allFamilyTokens) {
                allFamilyTokens->preserveForRetry(semanticSourceOrdinal);
            }
        }
        if (SUCCEEDED(hr)) {
            // The emitter records diagnostics_->peAppendPhaseEncode_ itself around the direct
            // canonical builder append. Timing the whole callable here would include
            // context/retention preparation and silently redefine `encode`,
            // the figure the migration is measured against -- see
            // docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.01.md.
            if (allFamilyTokens) {
                const auto recordOrdinalBefore =
                    recorderState_.commandChunk.lastCommittedRecordOrdinal();
                hr = emit(recorderState_.commandChunk, phase);
                const bool recordAccepted =
                    recorderState_.commandChunk.lastCommittedRecordOrdinal() !=
                        recordOrdinalBefore;
                if (recordAccepted) {
                    if (!observeCommittedSemanticRecord(
                            type, semanticSourceOrdinal,
                            recorderState_.commandChunk)) {
                        poisonStateBlockTransaction();
                        hr = D3DERR_DEVICELOST;
                    }
                } else if (FAILED(hr)) {
                    allFamilyTokens->preserveForRetry(semanticSourceOrdinal);
                } else {
                    poisonStateBlockTransaction();
                    hr = D3DERR_DEVICELOST;
                }
                const auto plan = dxmt9::d3d9::pe::planRecorderSettlement({
                    .point = dxmt9::d3d9::pe::RecorderSettlementPoint::Emitter,
                    .result = recordAccepted
                        ? dxmt9::d3d9::pe::RecorderSettlementResult::Succeeded
                        : dxmt9::d3d9::pe::RecorderSettlementResult::FailedPreEffect,
                });
                DXMT_ASSERT(plan.valid() &&
                            (recordAccepted ? plan.acceptedRecord()
                                            : plan.rollbackEmitter()));
                (void)plan;
            } else {
                hr = emit(recorderState_.commandChunk, phase);
                const auto plan = dxmt9::d3d9::pe::planRecorderSettlement({
                    .point = dxmt9::d3d9::pe::RecorderSettlementPoint::Emitter,
                    .result = SUCCEEDED(hr)
                        ? dxmt9::d3d9::pe::RecorderSettlementResult::Succeeded
                        : dxmt9::d3d9::pe::RecorderSettlementResult::FailedPreEffect,
                });
                DXMT_ASSERT(plan.valid() &&
                            (SUCCEEDED(hr) ? plan.acceptedRecord()
                                           : plan.rollbackEmitter()));
                (void)plan;
            }
        }
        if (SUCCEEDED(hr) &&
            (recorderState_.commandChunk.recordCount() >= maxRecords ||
             recorderState_.commandChunk.payloadBytes() >= maxBytes)) {
            const auto t0 = phase.begin();
            hr = flushPendingCommandChunk(PeRecorderFlushReason::CapacityPost);
            phase.recordFlush(t0);
            const auto plan = dxmt9::d3d9::pe::planRecorderSettlement({
                .point = dxmt9::d3d9::pe::RecorderSettlementPoint::CapacityPost,
                .result = dxmt9::d3d9::pe::recorderFlushSettlementResult(
                    SUCCEEDED(hr),
                    recorderState_.stateBlockTransaction.isPoisoned()),
            });
            DXMT_ASSERT(plan.valid() && plan.acceptedRecord() &&
                        (SUCCEEDED(hr) || plan.retryable() || plan.poison()));
            (void)plan;
        }
        if (peStatsEnabled) {
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
                        appendedPayloadBytes,
                        std::numeric_limits<std::uint32_t>::max())),
                    appendEntryNs);
            }
        }
        return hr;
    }

    // Non-indexed draw: fold or flush pending constants, then emit one sparse
    // canonical draw record through the appendRecord envelope.
    HRESULT appendDrawPrimitiveRecord(D3DPRIMITIVETYPE type, UINT startVertex, UINT count);

    HRESULT appendDrawIndexedPrimitiveRecord(D3DPRIMITIVETYPE type,
                                             INT baseVertex,
                                             UINT minVertex,
                                             UINT numVertices,
                                             UINT startIndex,
                                             UINT count);

    bool pendingChunkReferencesBuffer(
        const dxmt9::d3d9::pe::BufferRef &buffer) const;

    HRESULT appendDrawPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                        UINT count,
                                        const void* data,
                                        UINT stride);

    HRESULT appendDrawPrimitiveUPRecordWithFvf(D3DPRIMITIVETYPE type,
                                               UINT count,
                                               const void* data,
                                               UINT stride,
                                               bool overrideFvf,
                                               DWORD packetFvf,
                                               bool overrideVertexShaderNull = false,
                                               bool forceFullSnapshot = false);

    HRESULT appendDrawIndexedPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                               UINT minVertex,
                                               UINT numVertices,
                                               UINT count,
                                               const void* indexData,
                                               D3DFORMAT indexFormat,
                                               const void* vertexData,
                                               UINT stride);

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
                                                      bool forceFullSnapshot = false);

    HRESULT flushPeRecorder(
        PeRecorderFlushReason reason = PeRecorderFlushReason::Barrier,
        PeRecorderFlushDisposition disposition =
            PeRecorderFlushDisposition::Submit);

    // Variable-size const-array record append. The record is
    // header + (start, count) + count * elemSize bytes of payload.
    // Caller must supply the matching D9C_COMMAND_RECORD_SET_*_CONST_*
    // type tag; decoder will validate header.size against count*elemSize.
    HRESULT appendSetConstRecord(uint32_t recordType, UINT start, UINT count,
                                 const void* data, std::size_t elemSize,
                                 ConstShadow& settlementOwner);

    static bool constShadowElemEquals(const ConstShadow& shadow,
                                      std::uint32_t reg,
                                      const std::uint8_t* src,
                                      std::size_t elemSize);

    HRESULT applyConstStateWrite(ConstShadow& live,
                                 StateBlockConstShadow PeStateBlockConstRecorded::*
                                     recordedMember,
                                 std::uint32_t start, std::uint32_t count,
                                 const void* data,
                                 std::size_t elemSize) noexcept {
        using namespace dxmt9::d3d9::pe;
        if (!recorderState_.stateBlockTransaction.writeAllowed()) {
            return D3DERR_DEVICELOST;
        }
        if (count == 0u) return S_OK;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        if (recorderState_.stateBlockTransaction.isRecording()) {
            HRESULT recordingHr = D3DERR_INVALIDCALL;
            const bool entered =
                recorderState_.stateBlockTransaction.withRecordingWriter(
                    [&](auto& writer) noexcept {
                StateBlockConstShadow& recorded =
                    writer.constants().*recordedMember;
                try {
                    for (std::uint32_t i = 0u; i < count; ++i) {
                        const std::uint32_t reg = start + i;
                        const std::size_t offset =
                            static_cast<std::size_t>(reg) * elemSize;
                        const bool liveContains =
                            live.values.size() >= offset + elemSize;
                        const bool pendingContains =
                            reg < live.dirtyElems.size() &&
                            live.dirtyElems[reg] != 0u;
                        const StateWritePlan plan = planRecorderStateWrite(
                            StateWriteFacts{
                                .phase = RecorderPhase::Recording,
                                .origin = WriteOrigin::ExplicitSet,
                                .liveContains = liveContains,
                                .liveEquals =
                                    liveContains && constShadowElemEquals(
                                        live, reg, bytes +
                                            static_cast<std::size_t>(i) * elemSize,
                                        elemSize),
                                .pendingContains = pendingContains,
                            });
                        if (!plan.writeRecorded() || plan.writeLive() ||
                            plan.writePending()) {
                            recordingHr = D3DERR_INVALIDCALL;
                            return;
                        }
                    }
                    recorded.record(start, count, data, elemSize);
                    recordingHr = S_OK;
                } catch (const std::bad_alloc&) {
                    recordingHr = E_OUTOFMEMORY;
                }
            });
            return entered ? recordingHr : D3DERR_INVALIDCALL;
        }

        bool touch = false;
        for (std::uint32_t i = 0u; i < count; ++i) {
            const std::uint32_t reg = start + i;
            const std::size_t offset =
                static_cast<std::size_t>(reg) * elemSize;
            const bool liveContains = live.values.size() >= offset + elemSize;
            const bool pendingContains =
                reg < live.dirtyElems.size() && live.dirtyElems[reg] != 0u;
            const StateWritePlan plan = planRecorderStateWrite(StateWriteFacts{
                .phase = RecorderPhase::Live,
                .origin = WriteOrigin::ExplicitSet,
                .liveContains = liveContains,
                .liveEquals = liveContains && constShadowElemEquals(
                    live, reg,
                    bytes + static_cast<std::size_t>(i) * elemSize, elemSize),
                .pendingContains = pendingContains,
            });
            touch = touch || plan.writePending();
        }
        if (touch) touchConstShadow(live, start, count, data, elemSize);
        return S_OK;
    }

    static VsConstRangeChange analyzeConstShadowChange(
        const ConstShadow& shadow,
        std::uint32_t start,
        std::uint32_t count,
        const void* data,
        std::size_t elemSize);

    static std::uint32_t countDirtyConstRegs(const ConstShadow& shadow,
                                             std::uint32_t start,
                                             std::uint32_t end);

    // Emit one record covering the pending merged dirty range, then clear it.
    HRESULT flushConstShadow(ConstShadow& shadow, uint32_t recordType, std::size_t elemSize);

    // Drain all 6 const shadows. Called before each appended Draw record
    // and at chunk flush so the chunk's record stream replays
    // constants → draw in API order.
    HRESULT flushPendingConsts();

    // Phase 28: chunk-mode barrier flush. Replaces flushPendingHotState's
    // bridge-emit path with a chunk-record path that preserves the
    // "Set* never crosses PE/unix in default chunk mode" invariant.
    //
    // Drains pending consts (existing per-record stream) THEN, if hot
    // state is pending, packages the delta into a D9C_COMMAND_RECORD_
    // APPLY_STATE record + appends to the chunk + clears the pending
    // bits. Server importer dispatches APPLY_STATE via the same
    // canonical state replay that draw records use, so the server
    // shadow is updated before the upcoming barrier record runs.
    //
    // Caller still appends the actual barrier record afterwards;
    // chunk-commit flushes everything in the recorded order.
    HRESULT chunkBarrierFlush();

    // One APPLY_STATE record carrying exactly one category's batch. The
    // sizeHint stays kLegacyApplyStateSizeHint, which is what the
    // capacity precheck saw when this drained legacy records, so seal cadence is
    // unchanged. flags stay 0: these are batches, never snapshots.
    template <typename Fill, typename Accept>
    HRESULT appendSingleCategoryApplyState(Fill fill, Accept accept);

    HRESULT drainOversizedPendingStateAsApplyStateRecords(
        const dxmt9::d3d9::pe::RecorderLockCapability& access);

public:
    // KEY FUNCTION. QueryInterface is deliberately declaration-only and its
    // unchanged body is owned by d3d9_pe_device.cpp. It is the first
    // non-pure, non-inline virtual in declaration order, so the Itanium ABI
    // anchors this device's vtable -- and the out-of-line copies of its inline
    // COM surface -- in the hot owning TU. Keep AddRef/Release inline.
private:
    HRESULT FlushPeRecorderForChild() noexcept;

public:
    void LockStateBlockOperationForChild() noexcept;
    void UnlockStateBlockOperationForChild() noexcept;
    bool IsStateBlockRecorderPoisonedForChild() const noexcept;
    void DiscardPreparedStateBlockApplyForChild() noexcept;
    void PoisonStateBlockRecorderForChild() noexcept;
    HRESULT FlushPeRecorderForBufferHazardForChild(D9CBuffer *buffer) noexcept;
    void NotifyRenderTapeObjectDefineForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::span<const std::byte> descriptor,
        std::span<const std::byte> immutablePayload = {}) noexcept;
    bool IsRenderTapeCaptureActiveForChild() const noexcept;
    bool IsRenderTapeCaptureTrackingEnabledForChild() const noexcept;
    void AbortRenderTapeCaptureForChild() noexcept;
    void RejectRenderTapeCaptureForChild(
        dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic)
        noexcept;
    dxmt9::d3d9::RenderTapeFullSnapshotStatus
    RenderTapeFullSnapshotStatusForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, std::uint32_t fullRowBytes,
        std::uint32_t fullRows, std::uint64_t fullBytes) const noexcept;
    void NotifyRenderTapeBlockMutationForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
        std::span<const std::byte> bytes) noexcept;
    void NotifyRenderTapeLinearMutationForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
        std::span<const std::byte> bytes) noexcept;

    // UpdateTexture is a full-resource SYSTEMMEM -> DEFAULT copy. Keep the
    // normal command record untouched, but use its successful append as the
    // capture-only registry commit point. Before an interval, the resulting
    // bytes become the destination's future exact seed. During an interval,
    // only an already-admitted destination is safe: materializing an unseen
    // destination with post-copy bytes would move the initial state across the
    // command boundary and make replay depend on an unproven refinement. The
    // successful copy is still retained for a later retry.
    void applyRenderTapeUpdateTextureClosure(
        const dxmt9::d3d9::pe::PeWireObjectRef &source,
        const dxmt9::d3d9::pe::PeWireObjectRef &destination) noexcept;

    void NotifyRenderTapeSurfaceAliasForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &surface,
        const dxmt9::d3d9::pe::PeWireObjectRef &parentTexture,
        std::uint32_t subresource,
        const D9CSurfaceDesc &descriptor) noexcept;
    void NotifyRenderTapeStandaloneSurfaceForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &surface,
        const D9CSurfaceDesc &descriptor) noexcept;

    bool retireRenderTapeObject(
        const D9CWireObjectIdentity &identity, bool recordDestroy,
        const char *failureReason) noexcept;

    void retireRenderTapeAliasesForParent(
        const D9CWireObjectIdentity &parent, bool recordDestroy = true) noexcept;

    void drainPendingRenderTapeChunk(bool recordDestroy) noexcept;

    void NotifyRenderTapeObjectDestroyForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept;
    void NotifyRenderTapeResourceMutationForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        dxmt9::d3d9::RenderTapeMutationKind kind, std::uint32_t subresource,
        std::uint64_t byteOffset,
        std::span<const std::byte> bytes,
        dxmt9::d3d9::RenderTapeBufferMutationDisposition bufferDisposition)
        noexcept;
    void NotifyRenderTapeOrderedControlForChild(
        const dxmt9::d3d9::RenderTapeOrderedControlHeader &fixed,
        std::span<const std::byte> payload) noexcept;
    void notifyRenderTapeCreatedObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::span<const std::byte> descriptor,
        std::span<const std::byte> immutablePayload = {}) noexcept;
    void notifyRenderTapeCreatedBuffer(
        D9CBuffer *buffer,
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept;
    void notifyRenderTapeCreatedTexture(
        D9CTexture *texture,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        dxmt9::d3d9::RenderTapeTextureDimension dimension) noexcept;
    void notifyRenderTapeCreatedVertexDecl(
        D9CVertexDecl *decl,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::span<const std::byte> elements, std::size_t elementCount) noexcept;
    void notifyRenderTapeCreatedQuery(
        D9CQuery *query,
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept;
    void notifyRenderTapeCreatedShader(
        D9CShader *shader,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        uint32_t stage) noexcept;
    bool IsStateBlockRecordingForChild() const noexcept;
    void InvalidateStateBlockShadowForChild() noexcept;
    void AddDefaultPoolResourceRefForChild() noexcept;
    void ReleaseDefaultPoolResourceRefForChild() noexcept;
    bool IsChunkRecorderEnabledForChild() const noexcept;

    HRESULT AppendQueryIssueForChild(
        std::uint32_t flags,
        const dxmt9::d3d9::pe::QueryRef& query) noexcept;

    // PE-shadow stateblock support.
    //
    // Initial snapshot mode (called from D3D9StateBlockImpl ctor inside
    // EndStateBlock, when `stateBlockTransformRecorded` is still populated):
    // copy the recorded transform set into `out.transforms` to fix the
    // *tracked keys* — those will be the only transforms the stateblock
    // replays on Apply. Keys touched only by MultiplyTransform are absent
    // from this table (wined3d quirk), so they will not be captured or
    // replayed. Shader constants and the bound vdecl are captured from the
    // matching fixed recorded set.
    //
    // Refresh mode (called from D3D9StateBlockImpl::Capture, post-End,
    // when `stateBlockTransformRecorded` is empty): re-read the value of
    // each already-tracked key from the live `transformShadow`. The set
    // of tracked keys is FIXED at End; mid-game Captures only refresh
    // values, never add keys. Shader constants and the vdecl likewise refresh
    // only keys already tracked by the stateblock.
    HRESULT CaptureStateBlockShadowForChild(
        D3D9StateBlockShadow& out,
        StateBlockCaptureDisposition disposition) noexcept;

    D3D9DeviceImpl(D9CDevice* dev, IDirect3D9Ex* factory,
                   UINT adapter, D3DDEVTYPE deviceType, DWORD behaviorFlags,
                   HWND window, bool extended,
                   DWORD implicitSwapchainFlags);
    void takeImplicitWsiBinding(D3D9PeWsiBinding& binding) noexcept;
    bool commandChunkReady() const noexcept;

    D3D9PeStateBlockContext *stateBlockContext() noexcept;
    D3D9PeBufferContext *bufferContext() noexcept;
    D3D9PeSurfaceTextureContext *surfaceTextureContext() noexcept;
    D3D9PeQueryContext *queryContext() noexcept;
    D3D9PePresentationContext *presentationContext() noexcept;
    D3D9PeShaderDeclarationContext *shaderDeclarationContext() noexcept;

    D3D9PeDiagnosticObserver* diagnosticObserverForChild() noexcept;

    // Debug-only companion to recorderState_.recorderLockRequired: when the recorder lock
    // is being skipped (the app did not pass D3DCREATE_MULTITHREADED and
    // DXMT9_PE_FORCE_RECORDER_LOCK is not set), assert that the calling
    // thread is the thread that created this device. A real cross-thread
    // call here is app UB per the D3D9 contract; catch it loudly in debug
    // builds instead of racing the unlocked recorder state. Compiles out
    // under NDEBUG (release), same as every other DXMT_ASSERT.
    //
    // R-BACK-43.5 shape (c): `recorderState_.recorderLockRequired` is the lock witness —
    // when it is set, recorderState_.recorderMutex (not thread confinement) is what makes
    // the recorder safe, so any thread may proceed.
    void assertRecorderThreadConfined() const noexcept;

    ~D3D9DeviceImpl();

    /* ── IUnknown ── */

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override;

    /* ── device info ── */

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() noexcept override;
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() noexcept override;
    HRESULT STDMETHODCALLTYPE EvictManagedResources() noexcept override;

    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D) noexcept override;

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) noexcept override;

    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT sc, D3DDISPLAYMODE* pMode) noexcept override;

    HRESULT STDMETHODCALLTYPE GetCreationParameters(
            D3DDEVICE_CREATION_PARAMETERS* pParams) noexcept override;

    /* ── cursor (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* surface) noexcept override;
    void    STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) noexcept override;
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL show) noexcept override;

    /* ── swap chains ── */

    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(
            D3DPRESENT_PARAMETERS* pPP, IDirect3DSwapChain9** ppSC) noexcept override;

    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index,
                                            IDirect3DSwapChain9** ppSC) noexcept override;

    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() noexcept override;

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPP) noexcept override;

    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty) noexcept override {
        dxmt9PeSetCurrentCallName("Present");
        const bool recordPresentTiming = dxmt9PeRecorderStatsEnabled();
        const std::uint32_t presentThreadId =
            recordPresentTiming ? dxmt9PeCurrentThreadId() : 0u;
        const auto presentTimingEnter = recordPresentTiming
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
        const bool renderTapeCaptureWasActive =
            peCaptureState_ &&
            peCaptureState_->renderTapeCapture.state() ==
                dxmt9::d3d9::RenderTapeCaptureState::Capturing;
        const auto presentTimingStart = recordPresentTiming
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        auto presentTimingBarrierEnd = presentTimingStart;
        auto presentTimingAppendEnd = presentTimingStart;
        auto presentTimingFlushEnd = presentTimingStart;
        if (deviceNotReset_) {
            if (peCaptureState_)
                abortRenderTapeCapture("device_lost");
            return D3DERR_DEVICELOST;
        }
        dxmt9DeviceDebugLog("device_present device=%p wnd=%p src=%s dst=%s dirty=%p",
                            this, wnd,
                            src ? "<custom>" : "<full>",
                            dst ? "<custom>" : "<full>",
                            dirty);
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
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
        IDirect3DSurface9* presentSource = nullptr;
        const HRESULT presentSourceHr = GetBackBuffer(
            0u, 0u, D3DBACKBUFFER_TYPE_MONO, &presentSource);
        if (FAILED(presentSourceHr) || !presentSource) {
            return FAILED(presentSourceHr) ? presentSourceHr
                                           : D3DERR_INVALIDCALL;
        }
        D3D9PeValidatedSurface validatedPresentSource{};
        const HRESULT presentValidationHr = D3D9PeValidateSurface(
            presentSource, static_cast<IDirect3DDevice9*>(this),
            &validatedPresentSource);
        if (FAILED(presentValidationHr) || !validatedPresentSource.wire().valid()) {
            presentSource->Release();
            return D3DERR_INVALIDCALL;
        }
        const auto presentSourceWire = validatedPresentSource.wire();
        const dxmt9::d3d9::pe::PePresentBatch presentBatch{
            .command = D9CCommandChunkWirePresent{
                .hwnd = (uint64_t)(uintptr_t)wnd, .flags = 0,
                .hasSrc = src ? 1u : 0u, .hasDst = dst ? 1u : 0u,
                .sourceHandleIndex = 0u, .src = src ? cs : D9CRect{},
                .dst = dst ? cd : D9CRect{}},
            .source = presentSourceWire,
        };
        const HRESULT appendHr = appendRecord(
            D9C_COMMAND_RECORD_PRESENT, kLegacyPresentSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendPeExactPresentSingleton(
                    builder, presentBatch);
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        presentSource->Release();
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
        const HRESULT flushHr = flushPendingCommandChunk(PeRecorderFlushReason::Present);
        if (SUCCEEDED(flushHr)) (void)recorderState_.commandChunk.returnToLegacyFinalLayout();
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
            notePeThreadSamplerPresent();
            notePeCopyMaterializationPresent();
            if (renderTapeCaptureWasActive) {
                finishRenderTapeCaptureAtPresentBoundary();
            } else if (peCaptureState_) {
                (void)armRenderTapeCaptureAtPresentBoundary();
            }
        }
        return flushHr;
    }

    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT idx,
                                             D3DBACKBUFFER_TYPE type,
                                             IDirect3DSurface9** ppS) noexcept override;

    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT swapChain, D3DRASTER_STATUS* p) noexcept override;
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enableDialogs) noexcept override;
    // SetGammaRamp / GetGammaRamp — G2-B PE shadow + Option A unix push.
    // D3D9 returns void; Wine wined3d ignores unknown flag bits ("FIXME:
    // Ignoring flags") so we accept any flags value as opaque. iSwapChain
    // is unused for the shadow since there is no error channel on the
    // void-return D3D9 contract — the per-output forwarding in wined3d
    // is observationally invisible to the caller. SetGammaRamp also
    // pushes the payload through the D9C bridge to core::Device so the
    // unix-side Presenter can apply it on the next Present without a
    // second ABI roundtrip per frame.
    void    STDMETHODCALLTYPE SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp) noexcept override;
    void    STDMETHODCALLTYPE GetGammaRamp(UINT swapChain, D3DGAMMARAMP* p) noexcept override;

    /* ── resource creation ── */

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels,
                                             DWORD usage, D3DFORMAT fmt,
                                             D3DPOOL pool,
                                             IDirect3DTexture9** ppTex,
                                             HANDLE* psh) noexcept override;

    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d,
                                                   UINT levels, DWORD usage,
                                                   D3DFORMAT fmt, D3DPOOL pool,
                                                   IDirect3DVolumeTexture9** ppTex,
                                                   HANDLE* psh) noexcept override;

    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT size, UINT levels,
                                                 DWORD usage, D3DFORMAT fmt,
                                                 D3DPOOL pool,
                                                 IDirect3DCubeTexture9** ppTex,
                                                 HANDLE* psh) noexcept override;

    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage,
                                                  DWORD fvf, D3DPOOL pool,
                                                  IDirect3DVertexBuffer9** ppBuf,
                                                  HANDLE* psh) noexcept override;

    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage,
                                                 D3DFORMAT fmt, D3DPOOL pool,
                                                 IDirect3DIndexBuffer9** ppBuf,
                                                 HANDLE* psh) noexcept override;

    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt,
                                                  D3DMULTISAMPLE_TYPE ms,
                                                  DWORD msQual, BOOL lockable,
                                                  IDirect3DSurface9** ppS,
                                                  HANDLE* psh) noexcept override;

    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h,
                                                         D3DFORMAT fmt,
                                                         D3DMULTISAMPLE_TYPE ms,
                                                         DWORD msQual,
                                                         BOOL discard,
                                                         IDirect3DSurface9** ppS,
                                                         HANDLE* psh) noexcept override;

    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src,
                                             const RECT* srcRect,
                                             IDirect3DSurface9* dst,
                                             const POINT* dstPt) noexcept override;

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,
                                             IDirect3DBaseTexture9* dst) noexcept override;

    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt,
                                                   IDirect3DSurface9* dst) noexcept override;

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sc, IDirect3DSurface9* surface) noexcept override;

    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src,
                                           const RECT* srcRect,
                                           IDirect3DSurface9* dst,
                                           const RECT* dstRect,
                                           D3DTEXTUREFILTERTYPE filter) noexcept override;

    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurf,
                                         const RECT* pRect,
                                         D3DCOLOR color) noexcept override;

    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DPOOL pool,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE* psh) noexcept override;

    /* ── render targets ── */

    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD idx,
                                               IDirect3DSurface9* pSurf) noexcept override;

    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx,
                                               IDirect3DSurface9** ppS) noexcept override;

    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pSurf) noexcept override;

    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppS) noexcept override;

    /* ── scene ── */
    HRESULT STDMETHODCALLTYPE BeginScene() noexcept override;
    HRESULT STDMETHODCALLTYPE EndScene()   noexcept override;

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* pRects,
                                     DWORD flags, D3DCOLOR color,
                                     float z, DWORD stencil) noexcept override;

    /* ── transforms ── */
    template<typename HotSetter>
    HRESULT setTransformNormalized(D3DTRANSFORMSTATETYPE state,
                                   const D9CMatrix& wireM,
                                   dxmt9::d3d9::pe::WriteOrigin origin,
                                   HotSetter& hotSetter) noexcept;

    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* pM) noexcept override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state,
                                            D3DMATRIX* pM) noexcept override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX* pM) noexcept override;

    /* ── viewport / scissor ── */
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pVP) noexcept override;
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pVP) noexcept override;
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pR) noexcept override;
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pR) noexcept override;

    /* ── material / lights ── */
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pM) noexcept override;
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pM) noexcept override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD idx, const D3DLIGHT9* pL) noexcept override;
    HRESULT STDMETHODCALLTYPE GetLight(DWORD idx, D3DLIGHT9* pL) noexcept override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) noexcept override;
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD idx, BOOL* pEn) noexcept override;

    /* ── clip planes ── */
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* pPlane) noexcept override;
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* pPlane) noexcept override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* p) noexcept override;
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* p) noexcept override;

    HRESULT requestReszDepthResolve() noexcept;

    // Wine's AUTOGEN contract is a small recorder-thread-confined state
    // machine: rendering/clearing a texture-backed RT marks its hidden mip
    // pyramid dirty; the next draw that samples it first drains earlier
    // replay, generates the pyramid, and only then records the draw.
    HRESULT generateBoundAutogenMipmaps() noexcept;
    void markBoundRenderTargetsAutogenDirty() noexcept;

    /* ── render states ── */
    template<typename HotSetter>
    HRESULT setRenderStateCore(D3DRENDERSTATETYPE state, DWORD value,
                               HotSetter& hotSetter) noexcept;

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) noexcept override;
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
    static bool isValidTextureStageStateType(D3DTEXTURESTAGESTATETYPE type) noexcept;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD value) noexcept override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD* pValue) noexcept override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD value) noexcept override;
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD* pValue) noexcept override;
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
    static bool fragmentTextureStageSlot(DWORD stage, uint32_t& slot) noexcept;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage,
                                          IDirect3DBaseTexture9* pTex) noexcept override;
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
        const D3D9StateBlockShadow& shadow,
        StateBlockCaptureDisposition disposition) noexcept;

    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) noexcept override;
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) noexcept override;
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
            const D3DVERTEXELEMENT9* pElems,
            IDirect3DVertexDeclaration9** ppVD) noexcept override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(
            IDirect3DVertexDeclaration9* pVD) noexcept override;
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(
            IDirect3DVertexDeclaration9** ppVD) noexcept override;

    /* ── vertex shaders ── */
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFn,
                                                  IDirect3DVertexShader9** ppVS) noexcept override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) noexcept override;
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
                                                    UINT maxRegisters);

    // Keep the default-off virtual setter branch self-contained. The full
    // validator above is a cold owner for diagnostics/getters; this identical
    // checked kernel preserves the audited fast-path codegen.
    [[nodiscard]] static inline HRESULT validateConstRangeFast(
        UINT start, UINT count, const void* pData, UINT maxRegisters) noexcept {
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
                                std::size_t elemSize);

    template <typename Scope>
    HRESULT SetVertexShaderConstantFSlowBody(UINT start, const float* pData,
                                               UINT count,
                                               Scope& peCall) noexcept;
    // Diagnostics-on body for SetVertexShaderConstantF, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantFSlow(UINT start, const float* pData,
                                  UINT count) noexcept;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* pData,
                                                        UINT count) noexcept override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* pData,
                                                        UINT count) noexcept override;
    // Diagnostics-on body for SetVertexShaderConstantI, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantISlow(UINT start, const INT* pData,
                                  UINT count) noexcept;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) noexcept override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, INT* pData,
                                                        UINT count) noexcept override;
    // Diagnostics-on body for SetVertexShaderConstantB, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantBSlow(UINT start, const BOOL* pData,
                                  UINT count) noexcept;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) noexcept override;
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
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT freq) noexcept override;
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* pFreq) noexcept override;

    /* ── indices ── */
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIBuf) noexcept override;
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIBuf) noexcept override;

    /* ── pixel shaders ── */
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFn,
                                                 IDirect3DPixelShader9** ppPS) noexcept override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pPS) noexcept override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) noexcept override;
    template <typename Scope>
    HRESULT SetPixelShaderConstantFSlowBody(UINT start, const float* pData,
                                              UINT count,
                                              Scope& peCall) noexcept;
    // Diagnostics-on body for SetPixelShaderConstantF, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantFSlow(UINT start, const float* pData,
                                 UINT count) noexcept;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) noexcept override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* pData,
                                                       UINT count) noexcept override;
    // Diagnostics-on body for SetPixelShaderConstantI, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantISlow(UINT start, const INT* pData,
                                 UINT count) noexcept;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) noexcept override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, INT* pData,
                                                       UINT count) noexcept override;
    // Diagnostics-on body for SetPixelShaderConstantB, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantBSlow(UINT start, const BOOL* pData,
                                 UINT count) noexcept;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) noexcept override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, BOOL* pData,
                                                       UINT count) noexcept override;

    /* ── draw calls ── */
    template<typename CallScope>
    __attribute__((always_inline))
    HRESULT drawPrimitiveCore(D3DPRIMITIVETYPE type, UINT startVertex,
                              UINT count, CallScope& peCall) noexcept {
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("DrawPrimitive", hr);
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_primitive device=%p type=%u startVertex=%u count=%u",
                            this, (unsigned)type, startVertex, count);
        if (recorderState_.peState.pendingRenderStatesTyped().size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return finishPeCall(barrierHr);
        }
        auto drawSwvpPhase = peCall.phase(
            &PeDiagnosticsState::peDrawPhaseSwvpDecimatedStats_);
        SoftwareFfpDrawData swvpDraw{};
        HRESULT hr = prepareSoftwareDrawCandidate([&] {
            HRESULT candidateHr = trySoftwareFfpDrawPrimitive(
                type, startVertex, count, swvpDraw);
            if (candidateHr == S_FALSE) {
                candidateHr = trySoftwareProgrammableDrawPrimitive(
                    type, startVertex, count, swvpDraw);
            }
            if (candidateHr == S_OK) {
                candidateHr = filterSoftwareDrawOutsideClipPrimitives(swvpDraw);
            }
            return candidateHr;
        });
        drawSwvpPhase.stop();
        if (SUCCEEDED(hr)) {
            const HRESULT mipHr = generateBoundAutogenMipmaps();
            if (FAILED(mipHr)) return finishPeCall(mipHr);
        }
        bool appendedDraw = false;
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
            auto drawRecordPhase = peCall.phase(
                &PeDiagnosticsState::peDrawPhaseRecordDecimatedStats_);
            hr = appendDrawPrimitiveRecord(type, startVertex, count);
            drawRecordPhase.stop();
            appendedDraw = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr) && appendedDraw) {
            markBoundRenderTargetsAutogenDirty();
            if (!swvpDraw.vertices.empty()) {
                clearPendingHotState();
                recorderState_.peState.maintenance().pendingFvf() = true;
                recorderState_.peState.maintenance().pendingVdecl() = true;
                if (swvpDraw.bypassVertexShader) recorderState_.peState.maintenance().pendingVs() = true;
            }
        }
        notePeCurrentCallReturnForInterAppendSplit();
        return finishPeCall(hr);
    }

    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type,
                                             UINT startVertex,
                                             UINT count) noexcept override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type,
                                                    INT baseVertex,
                                                    UINT minVertex, UINT numVertices,
                                                    UINT startIndex,
                                                    UINT count) noexcept override {
        return withPeCallScope(
            "DrawIndexedPrimitive", DXMT9_PE_CALLSITE_PC(),
            &PeDiagnosticsState::peEntryDrawDecimatedStats_,
            [&](auto& peCall) __attribute__((always_inline)) noexcept
                -> HRESULT {
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("DrawIndexedPrimitive", hr);
        };
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_indexed_primitive device=%p type=%u base=%d min=%u num=%u startIndex=%u count=%u",
                            this, (unsigned)type, baseVertex, minVertex, numVertices,
                            startIndex, count);
        if (recorderState_.peState.pendingRenderStatesTyped().size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return finishPeCall(barrierHr);
        }
        auto drawSwvpPhase = peCall.phase(
            &PeDiagnosticsState::peDrawPhaseSwvpDecimatedStats_);
        SoftwareFfpDrawData swvpDraw{};
        std::vector<std::uint8_t> swvpIndices{};
        D3DFORMAT swvpIndexFormat = D3DFMT_UNKNOWN;
        HRESULT hr = prepareSoftwareDrawCandidate([&] {
            HRESULT candidateHr = trySoftwareFfpDrawIndexedPrimitive(
                type, baseVertex, minVertex, numVertices, startIndex, count,
                swvpDraw, swvpIndices, swvpIndexFormat);
            if (candidateHr == S_FALSE) {
                candidateHr = trySoftwareProgrammableDrawIndexedPrimitive(
                    type, baseVertex, minVertex, numVertices, startIndex, count,
                    swvpDraw, swvpIndices, swvpIndexFormat);
            }
            if (candidateHr == S_OK) {
                candidateHr = filterSoftwareIndexedDrawOutsideClipPrimitives(
                    swvpDraw, swvpIndices, swvpIndexFormat);
            }
            return candidateHr;
        });
        drawSwvpPhase.stop();
        if (SUCCEEDED(hr)) {
            const HRESULT mipHr = generateBoundAutogenMipmaps();
            if (FAILED(mipHr)) return finishPeCall(mipHr);
        }
        bool appendedDraw = false;
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
            auto drawRecordPhase = peCall.phase(
                &PeDiagnosticsState::peDrawPhaseRecordDecimatedStats_);
            hr = appendDrawIndexedPrimitiveRecord(type, baseVertex, minVertex,
                                                 numVertices, startIndex, count);
            drawRecordPhase.stop();
            appendedDraw = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr) && appendedDraw) {
            markBoundRenderTargetsAutogenDirty();
            if (!swvpDraw.vertices.empty()) {
                clearPendingHotState();
                recorderState_.peState.maintenance().pendingFvf() = true;
                recorderState_.peState.maintenance().pendingVdecl() = true;
                recorderState_.peState.maintenance().pendingIb() = indexBuf_ != nullptr;
                if (swvpDraw.bypassVertexShader) recorderState_.peState.maintenance().pendingVs() = true;
            }
        }
        notePeCurrentCallReturnForInterAppendSplit();
        return finishPeCall(hr);
            });
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                               UINT count,
                                               const void* pData,
                                               UINT stride) noexcept override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                      UINT minVertex,
                                                      UINT numVertices,
                                                      UINT count,
                                                      const void* pIdxData,
                                                      D3DFORMAT idxFmt,
                                                      const void* pVtxData,
                                                      UINT stride) noexcept override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT srcStart, UINT dstIndex,
                                               UINT vertexCount,
                                               IDirect3DVertexBuffer9* dstBuffer,
                                               IDirect3DVertexDeclaration9* declaration,
                                               DWORD flags) noexcept override;
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) noexcept override;
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) noexcept override;
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) noexcept override;

    /* ── query ── */
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type,
                                           IDirect3DQuery9** ppQ) noexcept override;

    /* ── IDirect3DDevice9Ex ── */

    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT,UINT,float*,float*) noexcept override;
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9*,IDirect3DSurface9*,
                                            IDirect3DVertexBuffer9*,UINT,
                                            IDirect3DVertexBuffer9*,
                                            D3DCOMPOSERECTSOP,int,int) noexcept override;

    HRESULT STDMETHODCALLTYPE PresentEx(const RECT* src, const RECT* dst,
                                         HWND wnd, const RGNDATA* dirty,
                                         DWORD flags) noexcept override;

    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* p) noexcept override;
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) noexcept override;

    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT sc) noexcept override;

    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9**,
                                                      UINT32) noexcept override;

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT maxLatency) noexcept override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* p) noexcept override;

    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND wnd) noexcept override;

    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT w, UINT h,
                                                    D3DFORMAT fmt,
                                                    D3DMULTISAMPLE_TYPE ms,
                                                    DWORD msQual, BOOL lockable,
                                                    IDirect3DSurface9** ppS,
                                                    HANDLE* psh,
                                                    DWORD usage) noexcept override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT w, UINT h,
                                                             D3DFORMAT fmt,
                                                             D3DPOOL pool,
                                                             IDirect3DSurface9** ppS,
                                                             HANDLE* psh,
                                                             DWORD usage) noexcept override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DMULTISAMPLE_TYPE ms,
                                                           DWORD msQual,
                                                           BOOL discard,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE* psh,
                                                           DWORD usage) noexcept override;

    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS* pPP,
                                       D3DDISPLAYMODEEX* pFsMode) noexcept override;

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT sc,
                                                D3DDISPLAYMODEEX* pMode,
                                                D3DDISPLAYROTATION* pRot) noexcept override;
};

#include "d3d9_pe_device_swvp_templates.inc.hpp"
