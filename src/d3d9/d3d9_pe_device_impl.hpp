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
#include "d3d9_pe_device_child.hpp"
#include "d3d9_pe_capture_state.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_decimated_scope.hpp"
#include "d3d9_pe_diagnostics_state.hpp"
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
#include "d3d9_pe_recorder_state.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_stats_decimation.hpp"
#include "d3d9_pe_thread_sampler.hpp"
#include "d3d9_pe_wire_handle.hpp"
#include "dxmt9/assert.hpp"
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

inline void dxmt9DeviceDebugLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-device", fmt, args);
    va_end(args);
}

inline void dxmt9DeviceInfoLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-device", fmt, args);
    va_end(args);
}

#include "d3d9_pe_device_diag_log.inc.hpp"

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
inline thread_local D3D9PePresentCallToken
    dxmt9PeCallScopeSlots[kPeCallScopeSlots];
inline thread_local std::size_t dxmt9PeCallScopeDepth = 0;

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
inline uint32_t userMemoryBytesPerPixel(D3DFORMAT fmt) {
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

/// D3D9 shader-version token form: ((stageHi << 16) | (major << 8) | minor).
/// stageHi == 0xFFFE for vertex shaders, 0xFFFF for pixel shaders.
constexpr uint32_t kShaderHeaderVS = 0xFFFEu;
constexpr uint32_t kShaderHeaderPS = 0xFFFFu;
constexpr uint32_t kShaderMaxMajor = 3u; /* vs_3_0 / ps_3_0 are the cap */
constexpr uint32_t kShaderEndToken = 0x0000FFFFu;
constexpr size_t kShaderBoundedScan = 1u << 16;

#include "d3d9_pe_device_tape_helpers.inc.hpp"

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

/* =========================================================================
 * Raw-handle extractors — safe because only our device creates these objects.
 * ========================================================================= */

inline D9CSurface*   rawSurf(IDirect3DSurface9* p)          { return D3D9PeRawSurface(p); }
inline D9CTexture*   rawTex(IDirect3DBaseTexture9* p)       { return D3D9PeRawTexture(p); }

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
    friend class D3D9PeDiagnosticObserver;
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
    // single-threaded access and recorderMutex_ (below) is skipped on the
    // hot append path. Resolved once at construction — see
    // dxmt9PeRecorderLockRequired(). DXMT9_PE_FORCE_RECORDER_LOCK is the
    // rollback/insurance lane for apps that violate the contract.
    dxmt9::d3d9::pe::PeRecorderState recorderState_{};
    bool&        recorderLockRequired_ = recorderState_.recorderLockRequired;
    // R-BACK-43.4 `producer-owned` (PE game thread) — the PE recorder, the
    // chunk builder it owns, and their retainer are written and read only on
    // the thread that constructed this device, EXCEPT under the documented
    // R-BACK-43.5 shape-(c) exception: with D3DCREATE_MULTITHREADED (or the
    // DXMT9_PE_FORCE_RECORDER_LOCK rollback lane) recorderMutex_ covers
    // cross-thread access instead, which is exactly what recorderLockRequired_
    // above witnesses.
    //
    // Debug-only thread-confinement companion to recorderLockRequired_: this
    // token binds to the constructing thread and assertRecorderThreadConfined()
    // DXMT_ASSERTs that no other thread calls a recorder-guarded path while the
    // lock is skipped — catching app UB (calling cross-thread without
    // D3DCREATE_MULTITHREADED) loudly in debug builds instead of silently
    // corrupting the recorder. Shared helper per R-BACK-43.5; this site is the
    // reference shape the helper was extracted from.
    dxmt9::core::ThreadOwnershipToken& recorderOwnership_ =
        recorderState_.recorderOwnership;
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
    // True only inside EndStateBlock's CreatePeStateBlock call. Tells the
    // freshly-constructed D3D9StateBlockImpl that its initial transform
    // tracked-keys set should be EXACTLY stateBlockTransformRecorded —
    // even if that table is empty (a Begin/End block where everything was
    // MultiplyTransform). Without this flag, an empty recorded set would
    // be indistinguishable from a CreateStateBlock(D3DSBT_ALL) call,
    // which legitimately needs every populated transform captured.
    bool         insideEndStateBlock_ = false;
    std::recursive_mutex& recorderMutex_ = recorderState_.recorderMutex;

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

    PeHotStateShadow& peState_ = recorderState_.peState;
    PeConstShadowBlock& peConsts_ = recorderState_.peConsts;
    StateBlockRecorded& stateBlockState_ = recorderState_.stateBlock;
    PeStateBlockConstRecorded& stateBlockConsts_ =
        recorderState_.stateBlockConsts;
    IDirect3DSurface9* rtSlots_[4]{};
    bool rtSlotExplicit_[4]{};
    IDirect3DSurface9* dsSurface_ = nullptr;

    // Reused across draws so populateBindingView() does not zero ~850 bytes
    // per call. Mutable because the binding-view fill is a const method.
    dxmt9::d3d9::pe::PeBindingView& peBindingView_ =
        recorderState_.peBindingView;

    // Reused sparse-producer storage. The SparseStateInput spans point into
    // peSparseScratch_, so both must outlive the append that consumes them --
    // they are members for that reason as well as to avoid per-draw zeroing.
    dxmt9::d3d9::pe::PeSparseScratch& peSparseScratch_ =
        recorderState_.peSparseScratch;
    dxmt9::d3d9::pe::SparseStateInput& peSparseState_ =
        recorderState_.peSparseState;
    D9CCommandChunkWireDrawHeader& peSparseHeader_ =
        recorderState_.peSparseHeader;
    dxmt9::d3d9::pe::PeDrawPayloads& peSparsePayloads_ =
        recorderState_.peSparsePayloads;
    bool dsSurfaceExplicit_ = false;
    dxmt9::d3d9::pe::CommandChunkBuilder& commandChunk_ =
        recorderState_.commandChunk;
    bool& commandChunkNegotiated_ = recorderState_.commandChunkNegotiated;
    // The complete Render Tape lifecycle is cold. The nullable owner leaves
    // only one pointer in the normal renderer and constructs no capture
    // storage when capture/tracking is disabled.
    std::unique_ptr<PeCaptureState> peCaptureState_{};
    std::uint64_t& commandChunkCommits_ = recorderState_.commandChunkCommits;
    std::uint64_t& commandChunkRecords_ = recorderState_.commandChunkRecords;
    std::uint64_t& commandChunkBytes_ = recorderState_.commandChunkBytes;
    // Every optional PE diagnostic payload lives behind this one pointer.
    // Capture correctness state remains in peCaptureState_, and recorder
    // protocol accounting remains in recorderState_.
    std::unique_ptr<PeDiagnosticsState> diagnostics_{};

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

    DWORD renderStateValue(D3DRENDERSTATETYPE state) const {
        uint32_t shadowValue = 0;
        if (peState_.renderStateShadowTyped().get(
                renderStateSlotKey(static_cast<uint32_t>(state)), shadowValue)) {
            return shadowValue;
        }
        return dxmt9c_device_get_render_state(dev_, static_cast<uint32_t>(state));
    }

    /* Borrowed per-index unix swap-chain handle; see swapchainHandles_.
     * The returned pointer must NOT be released by the caller — it is dropped
     * with the rest of the swap-chain cache in releaseAllBound(). */
    D9CSwapChain* borrowSwapChainHandle(UINT index) {
        if (const auto it = swapchainHandles_.find(index);
            it != swapchainHandles_.end()) {
            return it->second;
        }
        D9CSwapChain* chain =
            dev_ ? dxmt9c_device_get_swap_chain(dev_, index) : nullptr;
        if (!chain) {
            return nullptr;
        }
        try {
            swapchainHandles_.emplace(index, chain);
        } catch (...) {
            dxmt9c_swapchain_release(chain);
            return nullptr;
        }
        return chain;
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
        for (auto& [idx, chain] : swapchainHandles_) {
            if (chain) dxmt9c_swapchain_release(chain);
        }
        swapchainHandles_.clear();
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
        // Discarded chunks never acquire a tape ObjectDestroy event. Drain
        // the logical pending refs before raw D9C retainer reset.
        drainPendingRenderTapeChunk(false);
        // Discard path (device teardown, Reset, ResetEx): release the warm
        // retainer pins too, so nothing is still holding a unix object when
        // dxmt9c_device_reset* / dxmt9c_device_release runs.
        commandChunk_.resetAndReleaseRetained();
    }

    void clearPeStateTracking() {
        peState_.clearServerShadowTables();
        peState_.clearPendingHotState();
        peConsts_.reset();
        stateBlockConsts_.clearForBegin();
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
        return peState_.renderStateEqualsTyped(renderStateSlotKey(state), value);
    }

    bool shadowedStreamSourceEquals(UINT stream,
                                    IDirect3DVertexBuffer9* buffer,
                                    UINT offset,
                                    UINT stride) const {
        return stream < 16 && streamSrc_[stream] == buffer &&
               streamOff_[stream] == offset && streamStr_[stream] == stride;
    }

    dxmt9::d3d9::pe::PeRtExplicitMask currentRtExplicitMask() const {
        dxmt9::d3d9::pe::PeRtExplicitMask explicitMask{};
        for (DWORD slot = 0; slot < 4; ++slot) {
            explicitMask[slot] = rtSlotExplicit_[slot];
        }
        return explicitMask;
    }

    std::uint64_t currentVertexShaderHash() const noexcept {
        return D3D9PeVertexShaderHash(vs_);
    }

    std::uint64_t currentPixelShaderHash() const noexcept {
        return D3D9PePixelShaderHash(ps_);
    }

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
    // through a cast and rawTex()/D3D9PeRawTexture made a virtual GetType()
    // call. D3D9PeWireTexture still switches on GetType(), so the texture loop
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
                             bool allStreams = false) const {
        for (std::uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
            if (needAllSlots ||
                (peState_.pendingTextureMask & (1u << stage)) != 0) {
                view.textures[stage] = D3D9PeWireTexture(textures_[stage]);
            }
        }
        for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
            if (!needAllSlots && !allStreams &&
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
        // Filled unconditionally now: addChunkContextSections needs it to answer
        // the retention question, and buildSparseState emits the index section
        // only for the indexed record types anyway.
        view.indexBuffer = D3D9PeWireIndexBuffer(indexBuf_);
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
    // Destination-chunk retention, as data. MUST be read after any CapacityPre
    // flush has resealed the chunk -- that is why the draw sites build it inside
    // the append emitter and not before.
    dxmt9::d3d9::pe::PeChunkContext currentChunkContext() const {
        dxmt9::d3d9::pe::PeChunkContext chunk{};
        for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
            if (pendingChunkReferencesBuffer(
                    toWireHandle(peBindingView_.streams[slot].buffer.object))) {
                chunk.retainedStreamMask |= 1u << slot;
            }
        }
        chunk.indexBufferKnown = submittedIndexBufferKnown_;
        chunk.submittedIndexBufferWire = submittedIndexBufferWireValue_;
        chunk.indexBufferRetained = pendingChunkReferencesBuffer(
            toWireHandle(peBindingView_.indexBuffer.object));
        return chunk;
    }

    // Bytes the drained constant ranges contribute, so the capacity precheck
    // sees a value on the same scale as the legacy record's
    // d9c_command_record_draw_*_total_size(), which included the folded const
    // payload. Without this, enabling DXMT9_PE_INLINE_CONST_DELTA would move
    // chunk boundaries.
    std::size_t sparseConstPayloadBytes() const {
        const dxmt9::d3d9::pe::SparseConstantRangeInput* ranges[] = {
            &peSparseState_.vsFloatConstants, &peSparseState_.vsIntConstants,
            &peSparseState_.vsBoolConstants,  &peSparseState_.psFloatConstants,
            &peSparseState_.psIntConstants,   &peSparseState_.psBoolConstants,
        };
        std::size_t total = 0;
        for (const auto* range : ranges) {
            total += range->registerBytes.size();
        }
        return total;
    }

    // The recorder's sole state producer. Fills the reused
    // peSparseState_ / peSparseHeader_ from the shadows and the binding view --
    // no fat packet in between. The decimated draw_packet scope lives here for
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
        bool inlineConstDelta = false) {
        const std::uint32_t recordType = params.recordType;
        // Choke point for all three callers. addChunkContextSections carries the
        // same guard and the full story of what an unstamped recordType did, but
        // it is only reached by draws -- the chunkBarrierFlush APPLY_STATE path
        // would slip past it and be misclassified as a draw by isDraw below.
        // Returning false surfaces as a failed HRESULT, not silent corruption.
        if (recordType == 0u) {
            return false;
        }
        DxmtPeDecimatedScopeGuard decimatedScope;
        const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
        if (decimationN != 0 &&
            PeDecimatedScopeTimer::shouldSample(
                diagnostics_->peDrawPacketDecimatedStats_, decimationN)) {
            decimatedScope.stats = &diagnostics_->peDrawPacketDecimatedStats_;
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
        const bool isDraw =
            recordType != D9C_COMMAND_RECORD_APPLY_STATE;
        populateBindingView(peBindingView_, needAllSlots, isDraw);
        return dxmt9::d3d9::pe::buildSparseState(
            peState_, peConsts_, peBindingView_, peSparsePayloads_, params,
            forceFullSnapshot, inlineConstDelta, peSparseScratch_,
            peSparseHeader_, peSparseState_);
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

    PePresentCadenceClaim claimPeFirstCallAfterPresent() {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return {};
        }
        std::uint64_t ordinal =
            diagnostics_->pePresentCadencePendingOrdinal_.load(std::memory_order_acquire);
        if (ordinal == 0) {
            return {};
        }
        const auto entry = std::chrono::steady_clock::now();
        if (!diagnostics_->pePresentCadencePendingOrdinal_.compare_exchange_strong(
                ordinal, 0, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return {};
        }
        return PePresentCadenceClaim{
            true, ordinal,
            diagnostics_->pePresentCadenceReturnNs_.load(std::memory_order_acquire),
            dxmt9SteadyClockNs(entry)};
    }

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

    void markPePresentReturnedForCadence() {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t ordinal =
            diagnostics_->pePresentCadenceOrdinal_.fetch_add(1, std::memory_order_relaxed) + 1;
        diagnostics_->pePresentCadenceReturnNs_.store(dxmt9SteadyClockNs(now),
                                        std::memory_order_release);
        diagnostics_->pePresentCadencePendingOrdinal_.store(ordinal, std::memory_order_release);
        diagnostics_->pePresentCallCount_.store(0, std::memory_order_release);
        diagnostics_->pePresentCallMilestoneMask_.store(0, std::memory_order_release);
        diagnostics_->pePresentCallMilestonePendingOrdinal_.store(ordinal,
                                                   std::memory_order_release);
        diagnostics_->pePresentChunkPendingOrdinal_.store(ordinal, std::memory_order_release);
        diagnostics_->pePresentRecordMilestoneMask_.store(0, std::memory_order_release);
        diagnostics_->pePresentRecordPendingOrdinal_.store(ordinal, std::memory_order_release);
    }

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

    PePresentCadenceClaim claimPeFirstChunkAfterPresent() {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return {};
        }
        std::uint64_t ordinal =
            diagnostics_->pePresentChunkPendingOrdinal_.load(std::memory_order_acquire);
        if (ordinal == 0) {
            return {};
        }
        const auto entry = std::chrono::steady_clock::now();
        if (!diagnostics_->pePresentChunkPendingOrdinal_.compare_exchange_strong(
                ordinal, 0, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return {};
        }
        return PePresentCadenceClaim{
            true, ordinal,
            diagnostics_->pePresentCadenceReturnNs_.load(std::memory_order_acquire),
            dxmt9SteadyClockNs(entry)};
    }

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

    void resetPeBetweenCallsWindow() {
        diagnostics_->peRecorderBetweenCallsActive_ = false;
        diagnostics_->peRecorderBetweenCallsStartNs_ = 0;
        diagnostics_->peRecorderBetweenCallFamilySamples_.fill(0);
        diagnostics_->peRecorderBetweenCallNameSamples_.fill(0);
        diagnostics_->peRecorderBetweenCallNameCpuNsTotal_.fill(0);
        diagnostics_->peRecorderBetweenCallNameCpuNsMax_.fill(0);
        diagnostics_->peRecorderBetweenLastCallFamily_ = PeInterAppendCallFamily::Unknown;
        diagnostics_->peRecorderBetweenLastCallName_ = PeInterAppendCallName::Unknown;
        diagnostics_->peRecorderBetweenLastCallExitNs_ = 0;
        diagnostics_->peRecorderBetweenCallTransitionSamples_.fill(0);
        diagnostics_->peRecorderBetweenCallTransitionNsTotal_.fill(0);
        diagnostics_->peRecorderBetweenCallTransitionNsMax_.fill(0);
        diagnostics_->peRecorderBetweenCallNameTransitionSamples_.fill(0);
        diagnostics_->peRecorderBetweenCallNameTransitionNsTotal_.fill(0);
        diagnostics_->peRecorderBetweenCallNameTransitionNsMax_.fill(0);
        diagnostics_->peRecorderBetweenCallNameTransitionSites_.clear();
        diagnostics_->peRecorderBetweenCallBodyCalls_ = 0;
        diagnostics_->peRecorderBetweenCallBodyCpuNsTotal_ = 0;
        diagnostics_->peRecorderBetweenCallBodyCpuNsMax_ = 0;
    }

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
        PeCallScope peCall(
            *diagnostics, callName, callerPc, entryStats);
        return std::forward<Body>(body)(peCall);
    }

    template<typename Body>
    __attribute__((always_inline))
    HRESULT withPeHotStateSetter(
        PeHotStateSetterFamily family, const char* callName,
        PeDecimatedScopeStats PeDiagnosticsState::* entryStats,
        const void* callerPc, Body&& body) noexcept {
        PeDiagnosticsState* const diagnostics = diagnostics_.get();
        if (!diagnostics) {
            return std::forward<Body>(body)(peNullHotSetter_);
        }
        PeHotStateSetterTimer hotSetter(
            *this, *diagnostics, family, callName, entryStats, callerPc);
        return std::forward<Body>(body)(hotSetter);
    }

    template<typename Body>
    __attribute__((always_inline))
    HRESULT withPeCallAndHotStateSetter(
        const char* callName, const void* callerPc,
        PeDecimatedScopeStats PeDiagnosticsState::* callEntryStats,
        PeHotStateSetterFamily family,
        PeDecimatedScopeStats PeDiagnosticsState::* hotEntryStats,
        Body&& body) noexcept {
        PeDiagnosticsState* const diagnostics = diagnostics_.get();
        if (!diagnostics) {
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

    void stopPeThreadSampler() {
        if (!diagnostics_ || !diagnostics_->peThreadSampler_) {
            return;
        }
        dxmt9::d3d9::pe::PeThreadSampler::stopAndRelease(diagnostics_->peThreadSampler_);
        diagnostics_->peThreadSampler_ = nullptr;
    }

    // Emits ONE cumulative [dxmt9-pe-sampler] group: a header line, one line
    // per module with a nonzero count (top 20 by count), then the self-module
    // PC histogram's top buckets. Counters are cumulative and never reset, so
    // a consumer reads the LAST group in the log.
    void logPeThreadSampler();

    // Present-cadence tick for the sampler dump, mirroring the decimation
    // cadence: cumulative line every 60 presents, plus a final line from the
    // destructor so the last partial interval is never lost.
    void notePeThreadSamplerPresent() {
        if (!diagnostics_ || !diagnostics_->peThreadSampler_) {
            return;
        }
        // The sampler targets the thread that created the device on the
        // assumption that it is also the thread that renders. If an app splits
        // those, every sample describes the wrong thread and nothing else in
        // the output would say so — the histogram would just look idle. Say it
        // once, loudly, instead of leaving a silently wrong answer.
        if (!diagnostics_->peThreadSamplerPresentThreadChecked_) {
            diagnostics_->peThreadSamplerPresentThreadChecked_ = true;
            const DWORD presentThread = GetCurrentThreadId();
            if (presentThread != diagnostics_->peThreadSampler_->targetThreadId()) {
                dxmt9PeThreadSamplerInfoLog(
                    "target_thread_mismatch sampled=0x%lx present=0x%lx "
                    "note=samples_describe_the_device_creating_thread_not_the_present_thread",
                    static_cast<unsigned long>(diagnostics_->peThreadSampler_->targetThreadId()),
                    static_cast<unsigned long>(presentThread));
            }
        }
        ++diagnostics_->peThreadSamplerPresents_;
        if (diagnostics_->peThreadSamplerPresents_ % 60 == 0) {
            logPeThreadSampler();
        }
    }

    // Present-cadence tick for the decimated dump: increments a cumulative
    // present counter and emits the cumulative line every 60 presents. A
    // final line is also emitted unconditionally from the destructor so the
    // last partial interval is never lost. No-op when decimation is off.
    void notePeStatsDecimationPresent() {
        if (dxmt9PeStatsDecimationN() == 0) {
            return;
        }
        ++diagnostics_->peStatsDecimationPresents_;
        if (diagnostics_->peStatsDecimationPresents_ % 60 == 0) {
            logPeStatsDecimation();
        }
    }

    void recordDrawPrimitiveUPCopy(std::uint32_t vertexBytes) {
        peDiagnosticsCall(diagnostics_.get(),
            [vertexBytes](PeDiagnosticsState& diagnostics) noexcept {
                if (!diagnostics.config.recorderStats) {
                    return;
                }
                ++diagnostics.peRecorderStats_.drawPrimitiveUPCalls;
                diagnostics.peRecorderStats_.upVertexBytes += vertexBytes;
            });
    }

    void recordDrawIndexedPrimitiveUPCopy(std::uint32_t vertexBytes,
                                          std::uint32_t indexBytes) {
        peDiagnosticsCall(diagnostics_.get(),
            [vertexBytes, indexBytes](PeDiagnosticsState& diagnostics) noexcept {
                if (!diagnostics.config.recorderStats) {
                    return;
                }
                ++diagnostics.peRecorderStats_.drawIndexedPrimitiveUPCalls;
                diagnostics.peRecorderStats_.upVertexBytes += vertexBytes;
                diagnostics.peRecorderStats_.upIndexBytes += indexBytes;
            });
    }

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

    static bool chunkHasPresentRecord(const D9CCommandChunk& chunk) noexcept {
        if (chunk.recordBytes < sizeof(D9CCommandChunkWireHeader) ||
            d9cWireHandleValue(chunk.records) == 0u) {
            return false;
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(
            static_cast<std::uintptr_t>(d9cWireHandleValue(chunk.records)));
        D9CCommandChunkWireHeader header{};
        std::memcpy(&header, bytes, sizeof(header));
        if (header.recordCount != chunk.recordCount ||
            header.recordHeaderSize != sizeof(D9CCommandChunkWireRecordHeader) ||
            header.recordTableOffset > chunk.recordBytes ||
            header.recordCount >
                (chunk.recordBytes - header.recordTableOffset) /
                    sizeof(D9CCommandChunkWireRecordHeader)) {
            return false;
        }
        for (std::uint32_t index = 0u; index < header.recordCount; ++index) {
            D9CCommandChunkWireRecordHeader record{};
            std::memcpy(&record,
                        bytes + header.recordTableOffset +
                            static_cast<std::size_t>(index) * sizeof(record),
                        sizeof(record));
            if (record.type == D9C_COMMAND_RECORD_PRESENT) {
                return true;
            }
        }
        return false;
    }

    void finishRenderTapeCaptureAtPresentBoundary() noexcept;

    HRESULT commitPendingCommandChunk(PeRecorderFlushReason commitReason,
                                      const D9CCommandChunk& chunk,
                                      const PeCommandChunkCommitInfo& info) {
                PeCaptureState *const captureState =
                    peCaptureState_ ? &*peCaptureState_ : nullptr;
                const bool capturePresent = captureState &&
                    captureState->renderTapeCapture.state() ==
                        dxmt9::d3d9::RenderTapeCaptureState::Capturing &&
                    chunkHasPresentRecord(chunk);
                bool presentMirrorReserved = false;
                if (capturePresent) {
                    dxmt9DeviceInfoLog(
                        "render_tape_capture captured_present_reserve_begin "
                        "token=%llu",
                        static_cast<unsigned long long>(
                            captureState->renderTapeActiveCaptureToken));
                    const HRESULT reserveHr = hr32(
                        dxmt9c_device_reserve_render_tape_present_capture(dev_));
                    dxmt9DeviceInfoLog(
                        "render_tape_capture captured_present_reserve_end "
                        "token=%llu hr=0x%08x disposition=%s",
                        static_cast<unsigned long long>(
                            captureState->renderTapeActiveCaptureToken),
                        static_cast<unsigned>(reserveHr),
                        SUCCEEDED(reserveHr) ? "reserved" : "failed");
                    if (SUCCEEDED(reserveHr)) {
                        presentMirrorReserved = true;
                    } else {
                        abortRenderTapeCapture("present_output_reserve");
                    }
                }
                PeDiagnosticsState *const chunkDiagnostics =
                    dxmt9PeChunkCommitDiagnosticsEnabled()
                        ? diagnostics_.get()
                        : nullptr;
                PePresentCadenceClaim chunkCadence{};
                std::int64_t entryNs = 0;
                std::int64_t priorReturnNs = 0;
                if (chunkDiagnostics) {
                    if (chunkDiagnostics->config.recorderStats) {
                        chunkCadence = claimPeFirstChunkAfterPresent();
                    }
                    entryNs = peDiagnosticsRead(
                        chunkDiagnostics, [](PeDiagnosticsState&) noexcept {
                            return dxmt9SteadyClockNs(
                                std::chrono::steady_clock::now());
                        });
                    priorReturnNs =
                        chunkDiagnostics->peRecorderLastChunkReturnNs_;
                }
                const bool captureWasActive = captureState &&
                    captureState->renderTapeCapture.state() ==
                        dxmt9::d3d9::RenderTapeCaptureState::Capturing;
                const bool captureChunkPrepared = captureWasActive &&
                    prepareRenderTapeChunkCapture(chunk, info);
                D9CCommandChunk submittedChunk = chunk;
                if (captureChunkPrepared && captureState &&
                    captureState->renderTapeCapture.state() ==
                        dxmt9::d3d9::RenderTapeCaptureState::Capturing &&
                    captureState->renderTapeActiveCaptureToken != 0u) {
                    submittedChunk.renderTapeCaptureToken =
                        captureState->renderTapeActiveCaptureToken;
                    submittedChunk.renderTapeEventOrdinal =
                        static_cast<std::uint64_t>(
                            captureState->renderTapeCapture.eventCount()) + 1u;
                }
                const HRESULT hr = hr32(
                    dxmt9c_device_commit_chunk(dev_, &submittedChunk));
                const std::int64_t returnNs = peDiagnosticsRead(
                    chunkDiagnostics, [](PeDiagnosticsState&) noexcept {
                        return dxmt9SteadyClockNs(
                            std::chrono::steady_clock::now());
                    });
                if (chunkDiagnostics) {
                    chunkDiagnostics->peRecorderLastChunkReturnNs_ = returnNs;
                }
                const std::uint64_t fillGapNs =
                    priorReturnNs > 0 && entryNs > priorReturnNs
                    ? static_cast<std::uint64_t>(entryNs - priorReturnNs)
                    : 0;
                const std::uint64_t activeFillNs =
                    chunkDiagnostics &&
                    chunkDiagnostics->peRecorderCurrentChunkFirstAppendNs_ > 0 &&
                    entryNs > chunkDiagnostics->peRecorderCurrentChunkFirstAppendNs_
                    ? static_cast<std::uint64_t>(
                        entryNs -
                            chunkDiagnostics->peRecorderCurrentChunkFirstAppendNs_)
                    : 0;
                const std::uint64_t bridgeNs =
                    returnNs > entryNs
                    ? static_cast<std::uint64_t>(returnNs - entryNs)
                    : 0;
                if (SUCCEEDED(hr) && chunkDiagnostics) {
                    chunkDiagnostics->peRecorderCurrentChunkFirstAppendNs_ = 0;
                    chunkDiagnostics->peRecorderLastAppendReturnNs_ = 0;
                    chunkDiagnostics->peRecorderLastAppendCallEntryNs_ = 0;
                    chunkDiagnostics->peRecorderLastAppendCallExitNs_ = 0;
                    chunkDiagnostics->peRecorderLastAppendRecordType_ = 0;
                    if (chunkDiagnostics->config.recorderStats) {
                        resetPeBetweenCallsWindow();
                    }
                }
                if (chunkDiagnostics &&
                    chunkDiagnostics->config.recorderStats) {
                    logPeFirstChunkAfterPresent(commitReason, chunkCadence, hr,
                                                info);
                }
                if (SUCCEEDED(hr)) {
                    ++commandChunkCommits_;
                    commandChunkRecords_ += info.recordCount;
                    commandChunkBytes_ += info.wireBytes;
                    if (chunkDiagnostics) {
                        recordPeChunkCommit(commitReason, info.recordCount,
                                            info.payloadBytes,
                                            info.handleCount, info.wireBytes,
                                            fillGapNs, activeFillNs, bridgeNs);
                    }
                    // Copy the exact sealed canonical bytes only after the
                    // bridge accepted them. The source remains valid until
                    // flushPendingCommandChunk resets its builder below.
                    if (captureChunkPrepared) {
                        captureCommittedRenderTapeChunk(chunk, info);
                    } else if (captureState && !captureWasActive) {
                        observeRenderTapeFirstAccessChunk(chunk, info);
                    }
                } else if (captureState &&
                           captureState->renderTapeCapture.state() ==
                               dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
                    abortRenderTapeCapture("bridge_commit");
                }
                if (presentMirrorReserved) {
                    if (FAILED(hr)) {
                        dxmt9c_device_cancel_render_tape_present_capture(dev_);
                    } else if (captureState &&
                               captureState->renderTapeCapture.state() ==
                                   dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
                        const auto outputBpp = captureState->renderTapeOutputDesc
                            ? dxmt9::d3d9::renderTapeLinearBytesPerPixel(
                                  captureState->renderTapeOutputDesc->format)
                            : 0u;
                        const bool outputExtentFits = captureState->renderTapeOutputDesc &&
                            outputBpp != 0u &&
                            captureState->renderTapeOutputDesc->height != 0u &&
                            captureState->renderTapeOutputDesc->width <=
                                std::numeric_limits<std::uint64_t>::max() /
                                    captureState->renderTapeOutputDesc->height / outputBpp;
                        const auto outputBytes = outputExtentFits
                            ? static_cast<std::uint64_t>(
                                  captureState->renderTapeOutputDesc->width) *
                                  captureState->renderTapeOutputDesc->height * outputBpp
                            : 0u;
                        bool outputBufferReady = outputExtentFits &&
                            outputBytes <=
                                std::numeric_limits<std::size_t>::max();
                        std::vector<std::byte> outputPixels;
                        if (outputBufferReady) {
                            try {
                                outputPixels.resize(
                                    static_cast<std::size_t>(outputBytes));
                            } catch (...) {
                                outputBufferReady = false;
                            }
                        }
                        if (!outputBufferReady) {
                            dxmt9c_device_cancel_render_tape_present_capture(dev_);
                            abortRenderTapeCapture("present_output_buffer");
                            return hr;
                        }
                        D9CRenderTapePresentCaptureResult output{};
                        const HRESULT finishHr = hr32(
                            dxmt9c_device_finish_render_tape_present_capture(
                                dev_, &output, outputPixels.data(),
                                outputPixels.size()));
                        std::vector<std::byte> sourcePixels;
                        D9CRenderTapePresentSourceCaptureResult source{};
                        HRESULT sourceFinishHr = D3DERR_NOTAVAILABLE;
                        if (SUCCEEDED(finishHr) && outputBufferReady) {
                            try {
                                sourcePixels.resize(
                                    static_cast<std::size_t>(outputBytes));
                                sourceFinishHr = hr32(
                                    dxmt9c_device_finish_render_tape_present_source_capture(
                                        dev_, &source, sourcePixels.data(),
                                        sourcePixels.size()));
                            } catch (...) {
                                sourceFinishHr = D3DERR_OUTOFVIDEOMEMORY;
                            }
                        }
                        bool sourceDigestMatches = false;
                        if (SUCCEEDED(sourceFinishHr) &&
                            source.status ==
                                D9C_RENDER_TAPE_PRESENT_SOURCE_CAPTURE_COMPLETE) {
                            dxmt9::d3d9::RenderTapeDigest sourceDigest{};
                            std::memcpy(sourceDigest.data(), source.sha256,
                                        sourceDigest.size());
                            sourceDigestMatches =
                                dxmt9::d3d9::RenderTapeCaptureSession::sha256(
                                    sourcePixels) == sourceDigest;
                        }
                        if (SUCCEEDED(finishHr) &&
                            output.status ==
                                D9C_RENDER_TAPE_PRESENT_CAPTURE_COMPLETE &&
                            outputExtentFits &&
                            output.width == captureState->renderTapeOutputDesc->width &&
                            output.height == captureState->renderTapeOutputDesc->height &&
                            output.format == captureState->renderTapeOutputDesc->format &&
                            output.byteCount == outputBytes &&
                            SUCCEEDED(sourceFinishHr) &&
                            source.status ==
                                D9C_RENDER_TAPE_PRESENT_SOURCE_CAPTURE_COMPLETE &&
                            source.width == captureState->renderTapeOutputDesc->width &&
                            source.height == captureState->renderTapeOutputDesc->height &&
                            source.format == captureState->renderTapeOutputDesc->format &&
                            source.byteCount == outputBytes &&
                            sourceDigestMatches) {
                            dxmt9::d3d9::RenderTapeDigest digest{};
                            std::memcpy(digest.data(), output.sha256,
                                        digest.size());
                            captureState->renderTapeExpectedDigest = digest;
                            captureState->renderTapeExpectedPixels = std::move(outputPixels);
                            captureState->renderTapeExpectedSourcePixels =
                                std::move(sourcePixels);
                        } else {
                            dxmt9c_device_cancel_render_tape_present_capture(dev_);
                            abortRenderTapeCapture("present_output_or_source_finish");
                        }
                    } else {
                        dxmt9c_device_cancel_render_tape_present_capture(dev_);
                    }
                }
                return hr;
    }

    HRESULT flushPendingCommandChunk(PeRecorderFlushReason reason) {
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        if (!commandChunkNegotiated_) {
            return D3DERR_NOTAVAILABLE;
        }
        if (commandChunk_.recordCount() == 0u) {
            return S_OK;
        }
        const auto payloadBytes = commandChunk_.payloadBytes();
        const auto sealed = commandChunk_.seal();
        if (!sealed.valid() || sealed.blob.size() > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        D9CCommandChunk chunk{};
        chunk.version = D9C_COMMAND_CHUNK_VERSION;
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
            if (peCaptureState_) {
                const bool recordDestroy =
                    peCaptureState_->renderTapeCapture.state() ==
                        dxmt9::d3d9::RenderTapeCaptureState::Capturing;
                // commitPendingCommandChunk has first copied/materialized the
                // command into the capture session. Only now may a deferred
                // last wrapper become an ObjectDestroy event. Bridge failure
                // leaves both the builder and pending refs intact for retry.
                drainPendingRenderTapeChunk(recordDestroy);
            }
            commandChunk_.reset();
        }
        return hr;
    }

    // Phase timer handed to an appendRecord emitter. The envelope owns the
    // decimation sampling decision, so an emitter that has its own phases to
    // attribute (the legacy adapter's resize and write) records them through
    // this rather than re-deriving `phaseSampled`.
    struct AppendPhaseTimer {
        PeDiagnosticsState* diagnostics = nullptr;

        std::chrono::steady_clock::time_point begin() const noexcept {
            return peDiagnosticsRead(
                diagnostics, [](PeDiagnosticsState&) noexcept {
                    return std::chrono::steady_clock::now();
                });
        }
        void recordEncode(
            std::chrono::steady_clock::time_point t0) const noexcept {
            if (!diagnostics) {
                return;
            }
            PeDecimatedScopeTimer::recordSample(
                diagnostics->peAppendPhaseEncode_,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count()));
        }
        void recordFlush(
            std::chrono::steady_clock::time_point t0) const noexcept {
            if (!diagnostics) {
                return;
            }
            PeDecimatedScopeTimer::recordSample(
                diagnostics->peAppendPhaseFlush_,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count()));
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
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        if (!commandChunkNegotiated_ || bytes == 0u || bytes > 0xffffffffull) {
            return commandChunkNegotiated_ ? D3DERR_INVALIDCALL
                                           : D3DERR_NOTAVAILABLE;
        }
        const auto maxRecords = maxPendingCommandRecords();
        const auto maxBytes = maxPendingCommandBytes();
        const auto recordCountBefore = commandChunk_.recordCount();
        const auto payloadBytesBefore = commandChunk_.payloadBytes();
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
        }
        if (SUCCEEDED(hr)) {
            // The emitter records diagnostics_->peAppendPhaseEncode_ itself around the direct
            // canonical builder append. Timing the whole callable here would include
            // context/retention preparation and silently redefine `encode`,
            // the figure the migration is measured against -- see
            // docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.01.md.
            hr = emit(commandChunk_, phase);
        }
        if (SUCCEEDED(hr) &&
            (commandChunk_.recordCount() >= maxRecords ||
             commandChunk_.payloadBytes() >= maxBytes)) {
            const auto t0 = phase.begin();
            hr = flushPendingCommandChunk(PeRecorderFlushReason::CapacityPost);
            phase.recordFlush(t0);
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
    HRESULT appendDrawPrimitiveRecord(D3DPRIMITIVETYPE type, UINT startVertex, UINT count) {
        Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Draw);
        // Hold the recorder lock across the const-flush/fold + draw-record
        // append pair: recorderMutex_ is recursive, so the nested per-append
        // acquisitions below become cheap re-entries instead of repeated
        // cold lock/unlock cycles on this hot path.
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        const bool inlineConstDelta = dxmt9PeInlineConstDeltaEnabled();
        if (!inlineConstDelta) {
            // Drain any accumulated const dirty ranges into chunk records
            // FIRST, so the chunk replays "consts → draw" in API order.
            const HRESULT constHr = flushPendingConsts();
            if (FAILED(constHr)) return constHr;
        }
        dxmt9::d3d9::pe::PeDrawParams params{};
        params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
        params.primitiveType = static_cast<std::uint32_t>(type);
        params.startVertex = startVertex;
        params.primitiveCount = count;
        // Under inlineConstDelta the const shadows are still dirty here. The
        // producer prepares their constant-range sections; the emitter settles
        // them only after appendSparseRecord accepts the record.
        if (!buildSparseStateForRecord(params, /*forceFullSnapshot=*/false,
                                       inlineConstDelta)) {
            return D3DERR_INVALIDCALL;
        }
        return appendRecord(
            D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
            kLegacyDrawPrimitiveSizeHint + sparseConstPayloadBytes(),
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                // Inside the emitter on purpose: CapacityPre may have sealed the
                // old chunk, and the retention answers are about the chunk this
                // record actually lands in.
                if (!dxmt9::d3d9::pe::addChunkContextSections(
                        currentChunkContext(), peState_, peBindingView_, params,
                        /*forceFullSnapshot=*/false, peSparseScratch_,
                        peSparseState_)) {
                    return D3DERR_INVALIDCALL;
                }
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendSparseRecord(
                    builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, peSparseHeader_,
                    peSparseState_);
                const auto settlement =
                    dxmt9::d3d9::pe::settleRecorderAppend({
                        .phase =
                            dxmt9::d3d9::pe::AppendSettlement::Prepared,
                        .appendSucceeded = ok,
                    });
                const bool settled =
                    dxmt9::d3d9::pe::acceptPreparedSparseState(
                        peState_, peConsts_, peSparseState_, settlement);
                DXMT_ASSERT(!ok || settled);
                (void)settled;
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
    }

    HRESULT appendDrawIndexedPrimitiveRecord(D3DPRIMITIVETYPE type,
                                             INT baseVertex,
                                             UINT minVertex,
                                             UINT numVertices,
                                             UINT startIndex,
                                             UINT count) {
        Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Draw);
        // See appendDrawPrimitiveRecord: recursive re-entry on an
        // already-held recorderMutex_ is cheaper than the repeated cold
        // acquisitions the nested const-flush + draw appends would do.
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        const bool inlineConstDelta = dxmt9PeInlineConstDeltaEnabled();
        if (!inlineConstDelta) {
            const HRESULT constHr = flushPendingConsts();
            if (FAILED(constHr)) return constHr;
        }
        dxmt9::d3d9::pe::PeDrawParams params{};
        params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
        params.primitiveType = static_cast<std::uint32_t>(type);
        params.baseVertex = baseVertex;
        params.minVertex = minVertex;
        params.numVertices = numVertices;
        params.startIndex = startIndex;
        params.primitiveCount = count;
        if (!buildSparseStateForRecord(params, /*forceFullSnapshot=*/false,
                                       inlineConstDelta)) {
            return D3DERR_INVALIDCALL;
        }
        const std::uint64_t ibWireValue =
            d9cWireHandleValue(toWireHandle(peBindingView_.indexBuffer.object));
        // Whether the index section was actually emitted decides the tracking
        // update, exactly as the legacy code keyed it on the final ibValid --
        // which the append-time dependency checkpoint could itself set.
        bool indexSectionEmitted = false;
        const HRESULT hr = appendRecord(
            D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
            kLegacyDrawIndexedPrimitiveSizeHint +
                sparseConstPayloadBytes(),
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                if (!dxmt9::d3d9::pe::addChunkContextSections(
                        currentChunkContext(), peState_, peBindingView_, params,
                        /*forceFullSnapshot=*/false, peSparseScratch_,
                        peSparseState_)) {
                    return D3DERR_INVALIDCALL;
                }
                indexSectionEmitted = !peSparseState_.indexBuffers.empty();
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendSparseRecord(
                    builder, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
                    peSparseHeader_, peSparseState_);
                const auto settlement =
                    dxmt9::d3d9::pe::settleRecorderAppend({
                        .phase =
                            dxmt9::d3d9::pe::AppendSettlement::Prepared,
                        .appendSucceeded = ok,
                    });
                const bool settled =
                    dxmt9::d3d9::pe::acceptPreparedSparseState(
                        peState_, peConsts_, peSparseState_, settlement);
                DXMT_ASSERT(!ok || settled);
                (void)settled;
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        if (SUCCEEDED(hr)) {
            if (indexSectionEmitted) {
                submittedIndexBufferWireValue_ = ibWireValue;
                submittedIndexBufferKnown_ = true;
            }
        }
        return hr;
    }

    bool pendingChunkReferencesBuffer(D9CWireHandle handle) const {
        const std::uint64_t value = d9cWireHandleValue(handle);
        if (value == 0u) {
            return false;
        }
        return commandChunk_.referencesObject(reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(value)));
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
        Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Draw);
        // See appendDrawPrimitiveRecord: recursive re-entry on an
        // already-held recorderMutex_ is cheaper than the repeated cold
        // acquisitions the nested const-flush + draw appends would do.
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        // Payload sizing moved AHEAD of the state build, because the producer
        // reads the payload spans while building. It is pure input validation --
        // type/count/stride/data only, nothing the state build produces -- and
        // consumes no pending state, so failing here instead of after the build
        // returns the same D3DERR_INVALIDCALL from the same inputs.
        std::uint32_t vertexBytes = 0;
        if (!checkedByteCount(primitiveVertexCount(type, count), stride, vertexBytes) ||
            (vertexBytes != 0 && !data)) {
            return D3DERR_INVALIDCALL;
        }
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
        // The override window has to cover populateBindingView, which reads
        // fvf_ / vdecl_ / vs_, so it wraps the whole state build exactly as it
        // wrapped the fat-packet build before.
        dxmt9::d3d9::pe::PeDrawParams params{};
        params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
        params.primitiveType = static_cast<std::uint32_t>(type);
        params.primitiveCount = count;
        params.stride = stride;
        // Borrowed for this call only; cleared below so no later build can read
        // a dangling span. Nothing else assigns peSparsePayloads_, so empty is
        // the invariant every non-UP record relies on.
        peSparsePayloads_.upVertex = std::span<const std::byte>(
            static_cast<const std::byte*>(data), vertexBytes);
        const bool built =
            buildSparseStateForRecord(params, forceFullSnapshot);
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
        if (!built) {
            peSparsePayloads_ = dxmt9::d3d9::pe::PeDrawPayloads{};
            return D3DERR_INVALIDCALL;
        }

        // sizeHint stays the legacy header+payload size the capacity precheck
        // saw before, so chunk seal cadence is unchanged. No chunk-context step:
        // a UP draw binds no app buffer, so there is nothing for the destination
        // chunk to have retained -- matching both legacy UP call sites, which ran
        // neither dependency checkpoint.
        const HRESULT hr = appendRecord(
            D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
            kLegacyDrawPrimitiveUPSizeHint + vertexBytes,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendSparseRecord(
                    builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                    peSparseHeader_, peSparseState_);
                if (!overrideFvf && !overrideVertexShaderNull &&
                    !forceFullSnapshot) {
                    const auto settlement =
                        dxmt9::d3d9::pe::settleRecorderAppend({
                            .phase =
                                dxmt9::d3d9::pe::AppendSettlement::Prepared,
                            .appendSucceeded = ok,
                        });
                    const bool settled =
                        dxmt9::d3d9::pe::acceptPreparedSparseState(
                            peState_, peConsts_, peSparseState_, settlement);
                    DXMT_ASSERT(!ok || settled);
                    (void)settled;
                }
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        peSparsePayloads_ = dxmt9::d3d9::pe::PeDrawPayloads{};
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
        Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Draw);
        // See appendDrawPrimitiveRecord: recursive re-entry on an
        // already-held recorderMutex_ is cheaper than the repeated cold
        // acquisitions the nested const-flush + draw appends would do.
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        // Payload sizing moved AHEAD of the state build; see the non-indexed UP
        // site for why that is behaviour-preserving.
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
        dxmt9::d3d9::pe::PeDrawParams params{};
        params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
        params.primitiveType = static_cast<std::uint32_t>(type);
        params.minVertex = minVertex;
        params.numVertices = numVertices;
        params.primitiveCount = count;
        params.stride = stride;
        params.indexFormat = static_cast<std::uint32_t>(indexFormat);
        // Borrowed for this call only; cleared below. Index bytes precede vertex
        // bytes in the record, and appendSparseRecord lays them out in that
        // order from these two spans.
        peSparsePayloads_.upIndex = std::span<const std::byte>(
            static_cast<const std::byte*>(indexData), indexBytes);
        peSparsePayloads_.upVertex = std::span<const std::byte>(
            static_cast<const std::byte*>(vertexData), vertexBytes);
        const bool built =
            buildSparseStateForRecord(params, forceFullSnapshot);
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
        if (!built) {
            peSparsePayloads_ = dxmt9::d3d9::pe::PeDrawPayloads{};
            return D3DERR_INVALIDCALL;
        }

        // No chunk-context step and, critically, no index-buffer section: an
        // indexed UP draw carries its indices inline and binds no index buffer.
        // dxmt9_pe_producer.cpp's indexedDraw predicate excludes _UP for exactly
        // this reason.
        const HRESULT hr = appendRecord(
            D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
            kLegacyDrawIndexedPrimitiveUPSizeHint + indexBytes +
                vertexBytes,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendSparseRecord(
                    builder, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                    peSparseHeader_, peSparseState_);
                if (!overrideFvf && !overrideVertexShaderNull &&
                    !forceFullSnapshot) {
                    const auto settlement =
                        dxmt9::d3d9::pe::settleRecorderAppend({
                            .phase =
                                dxmt9::d3d9::pe::AppendSettlement::Prepared,
                            .appendSucceeded = ok,
                        });
                    const bool settled =
                        dxmt9::d3d9::pe::acceptPreparedSparseState(
                            peState_, peConsts_, peSparseState_, settlement);
                    DXMT_ASSERT(!ok || settled);
                    (void)settled;
                }
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        peSparsePayloads_ = dxmt9::d3d9::pe::PeDrawPayloads{};
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
                                 const void* data, std::size_t elemSize,
                                 ConstShadow& settlementOwner) {
        const std::uint64_t payload64 = static_cast<std::uint64_t>(count) * elemSize;
        if (payload64 > 0xffffffffull - kLegacySetConstSizeHint) {
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
        return appendRecord(
            recordType,
            kLegacySetConstSizeHint + payloadBytes,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendSetConstants(
                    builder, recordType, start, count,
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(data), payloadBytes));
                const auto settlement =
                    dxmt9::d3d9::pe::settleRecorderAppend({
                        .phase = dxmt9::d3d9::pe::AppendSettlement::Prepared,
                        .appendSucceeded = ok,
                    });
                if (settlement.consumeRepresentedPending &&
                    settlement.recordDurable) {
                    settlementOwner.clear();
                }
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
    }

    static bool constShadowElemEquals(const ConstShadow& shadow,
                                      std::uint32_t reg,
                                      const std::uint8_t* src,
                                      std::size_t elemSize);

    HRESULT applyConstStateWrite(ConstShadow& live,
                                 StateBlockConstShadow& recorded,
                                 std::uint32_t start, std::uint32_t count,
                                 const void* data,
                                 std::size_t elemSize) noexcept {
        using namespace dxmt9::d3d9::pe;
        if (count == 0u) return S_OK;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        if (stateBlockRecording_) {
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
                            .recordedContains = recorded.contains(reg),
                        });
                    if (!plan.writeRecorded || plan.writeLive ||
                        plan.writePending) {
                        return D3DERR_INVALIDCALL;
                    }
                }
                recorded.record(start, count, data, elemSize);
                return S_OK;
            } catch (const std::bad_alloc&) {
                return E_OUTOFMEMORY;
            }
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
            touch = touch || plan.writePending;
        }
        if (touch) touchConstShadow(live, start, count, data, elemSize);
        return S_OK;
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

    // Emit one record covering the pending merged dirty range, then clear it.
    HRESULT flushConstShadow(ConstShadow& shadow, uint32_t recordType, std::size_t elemSize) {
        // Decimated timing (const_flush scope): sample only every Nth call
        // so the CPU cost of measuring is itself negligible. Independent of
        // DXMT9_PE_RECORDER_STATS. Guard covers every exit path (including
        // the early "not dirty" return below) via RAII.
        DxmtPeDecimatedScopeGuard decimatedScope;
        const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
        if (decimationN != 0 &&
            PeDecimatedScopeTimer::shouldSample(diagnostics_->peConstFlushDecimatedStats_, decimationN)) {
            decimatedScope.stats = &diagnostics_->peConstFlushDecimatedStats_;
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
        // and updates the flush counters above.
        const auto emitRun = [&](uint32_t start, uint32_t end) {
            if (end <= start) return;
            const uint32_t count = end - start;
            const auto* data =
                shadow.values.data() + static_cast<std::size_t>(start) * elemSize;
            const std::uint32_t dirtyRegs =
                recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F
                    ? countDirtyConstRegs(shadow, start, end)
                    : 0u;
            hr = appendSetConstRecord(
                recordType, start, count, data, elemSize, shadow);
            if (SUCCEEDED(hr)) {
                ++flushedRecords;
                flushedRegs += count;
            }
            if (SUCCEEDED(hr) && recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F) {
                recordVsConstSetterRange(VsConstSetterRangePhase::Flush,
                                         currentVertexShaderHash(),
                                         currentPixelShaderHash(),
                                         start, count, dirtyRegs, count);
            }
        };
        emitRun(shadow.dirtyStart, shadow.dirtyEnd);
        recordPeConstFlushCpu(recordType, flushEntryNs, flushedRecords,
                              flushedRegs);
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
    // canonical state replay that draw records use, so the server
    // shadow is updated before the upcoming barrier record runs.
    //
    // Caller still appends the actual barrier record afterwards;
    // chunk-commit flushes everything in the recorded order.
    HRESULT chunkBarrierFlush() {
        Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Barrier);
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
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
        dxmt9::d3d9::pe::PeDrawParams applyParams{};
        applyParams.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
        if (buildSparseStateForRecord(applyParams)) {
            recordPeApplyStateBuildCpu(buildEntryNs);
            // sizeHint stays kLegacyApplyStateSizeHint: it is what the
            // capacity precheck saw before, so seal cadence is unchanged.
            const HRESULT appendHr = appendRecord(
                D9C_COMMAND_RECORD_APPLY_STATE,
                kLegacyApplyStateSizeHint,
                [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                    const AppendPhaseTimer& phase) -> HRESULT {
                    const auto t0 = phase.begin();
                    const bool ok = dxmt9::d3d9::pe::appendApplyState(
                        builder, peSparseHeader_.flags, peSparseState_);
                    const auto settlement =
                        dxmt9::d3d9::pe::settleRecorderAppend({
                            .phase =
                                dxmt9::d3d9::pe::AppendSettlement::Prepared,
                            .appendSucceeded = ok,
                        });
                    const bool settled =
                        dxmt9::d3d9::pe::acceptPreparedSparseState(
                            peState_, peConsts_, peSparseState_, settlement);
                    DXMT_ASSERT(!ok || settled);
                    (void)settled;
                    phase.recordEncode(t0);
                    return ok ? S_OK : D3DERR_INVALIDCALL;
                });
            if (FAILED(appendHr)) return appendHr;
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

    // One APPLY_STATE record carrying exactly one category's batch. The
    // sizeHint stays kLegacyApplyStateSizeHint, which is what the
    // capacity precheck saw when this drained legacy records, so seal cadence is
    // unchanged. flags stay 0: these are batches, never snapshots.
    template <typename Fill, typename Accept>
    HRESULT appendSingleCategoryApplyState(Fill fill, Accept accept) {
        peSparseState_ = dxmt9::d3d9::pe::SparseStateInput{};
        fill();
        return appendRecord(
            D9C_COMMAND_RECORD_APPLY_STATE,
            kLegacyApplyStateSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendApplyState(
                    builder, /*flags=*/0u, peSparseState_);
                const auto settlement =
                    dxmt9::d3d9::pe::settleRecorderAppend({
                        .phase =
                            dxmt9::d3d9::pe::AppendSettlement::Prepared,
                        .appendSucceeded = ok,
                    });
                accept(settlement);
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
    }

    HRESULT drainOversizedPendingStateAsApplyStateRecords() {
        // Drain the four cappable collections (renderStates, tss,
        // samplerStates, transforms) in batches of cap-size. Each batch becomes
        // one APPLY_STATE record carrying ONLY that collection's batch; the
        // server applies unset categories idempotently, so an otherwise-empty
        // sparse record is safe.
        //
        // Section order is ascending by construction: every typed prepare walk
        // visits its bitmap from the lowest set bit and its rows in order.
        // appendPlainSection does not enforce ordering for these four categories,
        // but emitting them out of order would still change the wire shape.
        auto renderStates = peState_.pendingRenderStatesTyped();
        while (!renderStates.empty()) {
            const std::size_t n = peState_.prepareRenderStateBatch(
                peSparseScratch_.renderStates);
            const HRESULT hr = appendSingleCategoryApplyState(
                [&] {
                    peSparseState_.renderStates =
                        std::span(peSparseScratch_.renderStates).first(n);
                },
                [&](const dxmt9::d3d9::pe::AppendPlan& settlement) {
                    peState_.acceptRenderStateBatch(
                        std::span(peSparseScratch_.renderStates).first(n),
                        settlement);
                });
            if (FAILED(hr)) return hr;
        }
        auto textureStageStates = peState_.pendingTssTyped();
        while (!textureStageStates.empty()) {
            const std::size_t n = peState_.prepareTextureStageStateBatch(
                peSparseScratch_.textureStageStates);
            const HRESULT hr = appendSingleCategoryApplyState(
                [&] {
                    peSparseState_.textureStageStates =
                        std::span(peSparseScratch_.textureStageStates).first(n);
                },
                [&](const dxmt9::d3d9::pe::AppendPlan& settlement) {
                    peState_.acceptTextureStageStateBatch(
                        std::span(peSparseScratch_.textureStageStates).first(n),
                        settlement);
                });
            if (FAILED(hr)) return hr;
        }
        auto samplerStates = peState_.pendingSamplerStatesTyped();
        while (!samplerStates.empty()) {
            const std::size_t n = peState_.prepareSamplerStateBatch(
                peSparseScratch_.samplerStates);
            const HRESULT hr = appendSingleCategoryApplyState(
                [&] {
                    peSparseState_.samplerStates =
                        std::span(peSparseScratch_.samplerStates).first(n);
                },
                [&](const dxmt9::d3d9::pe::AppendPlan& settlement) {
                    peState_.acceptSamplerStateBatch(
                        std::span(peSparseScratch_.samplerStates).first(n),
                        settlement);
                });
            if (FAILED(hr)) return hr;
        }
        auto transforms = peState_.pendingTransformsTyped();
        while (!transforms.empty()) {
            const std::size_t n = peState_.prepareTransformBatch(
                peSparseScratch_.transforms);
            const HRESULT hr = appendSingleCategoryApplyState(
                [&] {
                    peSparseState_.transforms =
                        std::span(peSparseScratch_.transforms).first(n);
                },
                [&](const dxmt9::d3d9::pe::AppendPlan& settlement) {
                    peState_.acceptTransformBatch(
                        std::span(peSparseScratch_.transforms).first(n),
                        settlement);
                });
            if (FAILED(hr)) return hr;
        }
        // Remaining scalar pending bits (texture / stream / vs / ps / vdecl / RT
        // / DS / viewport / scissor / fvf / material / clip / lights /
        // lightEnable) all fit in one record. After draining the four cappable
        // collections above, the sparse build succeeds.
        if (!hasPendingHotState()) {
            return S_OK;
        }
        dxmt9::d3d9::pe::PeDrawParams tailParams{};
        tailParams.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
        if (!buildSparseStateForRecord(tailParams)) {
            // Truly should never happen -- the four cappable collections are now
            // empty. Defensive: log + return failure rather than silently leaving
            // pending state dirty (which would let the upcoming barrier observe
            // stale server state).
            dxmt9DeviceDebugLog(
                "ERR: drainOversizedPendingStateAsApplyStateRecords could "
                "not build tail APPLY_STATE - pending state lost. Caller "
                "should treat as recorder failure.");
            return D3DERR_INVALIDCALL;
        }
        const HRESULT hr = appendRecord(
            D9C_COMMAND_RECORD_APPLY_STATE,
            kLegacyApplyStateSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendApplyState(
                    builder, peSparseHeader_.flags, peSparseState_);
                const auto settlement =
                    dxmt9::d3d9::pe::settleRecorderAppend({
                        .phase =
                            dxmt9::d3d9::pe::AppendSettlement::Prepared,
                        .appendSucceeded = ok,
                    });
                const bool settled =
                    dxmt9::d3d9::pe::acceptPreparedSparseState(
                        peState_, peConsts_, peSparseState_, settlement);
                DXMT_ASSERT(!ok || settled);
                (void)settled;
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        if (FAILED(hr)) return hr;
        return S_OK;
    }

public:
    // KEY FUNCTION. Deliberately the one virtual whose body is NOT in this
    // header, and deliberately the FIRST virtual in declaration order, so the
    // Itanium ABI's "first non-pure, non-inline virtual" rule resolves to it.
    // That pins the vtable -- and with it the out-of-line copies of every
    // inline virtual, i.e. the whole IDirect3DDevice9Ex COM surface Wine calls
    // through -- into d3d9_pe_device.cpp, the hot TU, where it belongs.
    // Without this the class has NO key function, the vtable is comdat in
    // every TU, and the first cold TU to out-line any virtual silently claims
    // it: step 8 did exactly that by accident and moved DrawPrimitive into the
    // diagnostics object (744 KB there, 146 KB left in the hot TU).
    // Defined in d3d9_pe_device.cpp. Do not give it an in-class body.
    HRESULT FlushPeRecorderForChild() noexcept override;
    HRESULT FlushPeRecorderForBufferHazardForChild(D9CBuffer *buffer) noexcept override {
        if (!buffer) {
            return S_OK;
        }
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        const bool referenced = commandChunk_.referencesObject(buffer);
        if (!referenced) {
            return S_OK;
        }
        return flushPeRecorder(PeRecorderFlushReason::Child);
    }
    void NotifyRenderTapeObjectDefineForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::span<const std::byte> descriptor,
        std::span<const std::byte> immutablePayload = {}) noexcept override;
    bool IsRenderTapeCaptureActiveForChild() const noexcept override;
    bool IsRenderTapeCaptureTrackingEnabledForChild() const noexcept override;
    void AbortRenderTapeCaptureForChild() noexcept override;
    void RejectRenderTapeCaptureForChild(
        dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic)
        noexcept override;
    dxmt9::d3d9::RenderTapeFullSnapshotStatus
    RenderTapeFullSnapshotStatusForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, std::uint32_t fullRowBytes,
        std::uint32_t fullRows, std::uint64_t fullBytes) const noexcept override;
    void NotifyRenderTapeBlockMutationForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
        std::span<const std::byte> bytes) noexcept override;
    void NotifyRenderTapeLinearMutationForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
        std::span<const std::byte> bytes) noexcept override;

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
        const D9CSurfaceDesc &descriptor) noexcept override;
    void NotifyRenderTapeStandaloneSurfaceForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &surface,
        const D9CSurfaceDesc &descriptor) noexcept override;

    bool retireRenderTapeObject(
        const D9CWireObjectIdentity &identity, bool recordDestroy,
        const char *failureReason) noexcept;

    void retireRenderTapeAliasesForParent(
        const D9CWireObjectIdentity &parent, bool recordDestroy = true) noexcept;

    void drainPendingRenderTapeChunk(bool recordDestroy) noexcept;

    void NotifyRenderTapeObjectDestroyForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept override;
    void NotifyRenderTapeResourceMutationForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        dxmt9::d3d9::RenderTapeMutationKind kind, std::uint32_t subresource,
        std::uint64_t byteOffset,
        std::span<const std::byte> bytes,
        dxmt9::d3d9::RenderTapeBufferMutationDisposition bufferDisposition)
        noexcept override;
    void NotifyRenderTapeOrderedControlForChild(
        const dxmt9::d3d9::RenderTapeOrderedControlHeader &fixed,
        std::span<const std::byte> payload) noexcept override;
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
    bool IsStateBlockRecordingForChild() const noexcept override {
        return stateBlockRecording_;
    }
    void InvalidateStateBlockShadowForChild() noexcept override {
        peState_.clearServerShadowTables();
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
        const dxmt9::d3d9::pe::QueryRef& query) noexcept override {
        return appendRecord(
            D9C_COMMAND_RECORD_QUERY_ISSUE,
            kLegacyQueryIssueSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendQueryIssue(
                    builder, flags, query);
                phase.recordEncode(t0);
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
        D3D9StateBlockShadow& out) override;

    D3D9DeviceImpl(D9CDevice* dev, IDirect3D9Ex* factory,
                   UINT adapter, D3DDEVTYPE deviceType, DWORD behaviorFlags,
                   HWND window, bool extended,
                   DWORD implicitSwapchainFlags)
        : dev_(dev), factory_(factory)
        , adapter_(adapter), deviceType_(deviceType), behaviorFlags_(behaviorFlags)
        , recorderState_(dxmt9PeRecorderLockRequired(
              behaviorFlags, dxmt9PeForceRecorderLockEnabled()))
        // recorderOwnership_ binds to this thread through its default member
        // initializer, so it needs no entry here.
        , softwareVertexProcessing_((behaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? TRUE : FALSE)
        , extended_(extended)
        , peCaptureState_(makePeCaptureState(
              dxmt9PeRenderTapeCaptureEnabled(),
              dxmt9PeRenderTapeCaptureLimits(
                  dxmt9PeRenderTapeCaptureEnabled()),
              dxmt9PeRenderTapeCaptureProfile(),
              dxmt9PeRenderTapeCaptureSkipPresents()))
        , diagnostics_(makePeDiagnosticsState(
              this, dxmt9PeResolvedDiagnosticsConfig()))
        , creationWindow_(window)
        , implicitSwapchainFlagsShadow_(implicitSwapchainFlags) {
        if (factory_) factory_->AddRef();
        for (UINT& freq : streamFreq_) {
            freq = 1;
        }
        if (dev_) {
            D9CCommandChunkNegotiation negotiation{};
            negotiation.peSupportedVersions = D9C_COMMAND_CHUNK_CAP_CURRENT;
            negotiation.pePreferredVersion = D9C_COMMAND_CHUNK_VERSION;
            const HRESULT negotiationHr =
                hr32(dxmt9c_device_negotiate_command_chunk(
                    dev_, &negotiation));
            commandChunkNegotiated_ = SUCCEEDED(negotiationHr) &&
                negotiation.selectedVersion == D9C_COMMAND_CHUNK_VERSION;
            if (commandChunkNegotiated_) {
                dxmt9DeviceInfoLog(
                    "command chunk negotiation selected canonical pe_caps=0x%x unix_caps=0x%x",
                    negotiation.peSupportedVersions,
                    negotiation.unixSupportedVersions);
            } else {
                dxmt9DeviceInfoLog(
                    "command chunk negotiation failed hr=0x%08x preferred=canonical selected=v%u unix_caps=0x%x",
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
        // DXMT9_PE_MODULE_MAP: dump the loaded-module map after all modules
        // (game exe, our PE d3d9.dll/winemetal.dll, Wine DLLs) are loaded.
        // Device creation happens well after process/module init, so this is
        // a safe, one-time-per-device site for the Tier 1 PE symbolication
        // diagnostic.
        dxmt9PeDumpModuleMap();
        // DXMT9_PE_THREAD_SAMPLER: same site, same reasoning — every module is
        // loaded by now, and the creating thread is the game thread.
        startPeThreadSamplerIfRequested();
        dxmt9DeviceDebugLog("device_ctor this=%p dev=%p factory=%p adapter=%u devType=%u behavior=0x%x window=%p extended=%u",
                            this, static_cast<void*>(dev_), static_cast<void*>(factory_),
                            adapter_, (unsigned)deviceType_, (unsigned)behaviorFlags_, window, extended_ ? 1u : 0u);
    }

    bool commandChunkReady() const noexcept {
        return commandChunkNegotiated_;
    }

    D3D9PeDiagnosticObserver* diagnosticObserverForChild() noexcept {
        if (!diagnostics_ || !diagnostics_->config.recorderStats) {
            return nullptr;
        }
        return &diagnostics_->childObserver;
    }

    // Debug-only companion to recorderLockRequired_: when the recorder lock
    // is being skipped (the app did not pass D3DCREATE_MULTITHREADED and
    // DXMT9_PE_FORCE_RECORDER_LOCK is not set), assert that the calling
    // thread is the thread that created this device. A real cross-thread
    // call here is app UB per the D3D9 contract; catch it loudly in debug
    // builds instead of racing the unlocked recorder state. Compiles out
    // under NDEBUG (release), same as every other DXMT_ASSERT.
    //
    // R-BACK-43.5 shape (c): `recorderLockRequired_` is the lock witness —
    // when it is set, recorderMutex_ (not thread confinement) is what makes
    // the recorder safe, so any thread may proceed.
    void assertRecorderThreadConfined() const noexcept {
        DXMT_ASSERT_OWNED_BY_OR_LOCKED(recorderOwnership_,
                                        recorderLockRequired_);
    }

    ~D3D9DeviceImpl() {
        (void)flushPeRecorder(PeRecorderFlushReason::Destructor);
        dxmt9DeviceInfoLog(
            "command chunk totals selected=canonical chunks=%llu records=%llu bytes=%llu identity_getter_calls=%llu",
            static_cast<unsigned long long>(commandChunkCommits_),
            static_cast<unsigned long long>(commandChunkRecords_),
            static_cast<unsigned long long>(commandChunkBytes_),
            static_cast<unsigned long long>(
                dxmt9::d3d9::pe::wireIdentityGetterCallCount()));
        logVsConstSetterRangePerf("destructor");
        logPeRecorderStats("destructor", true);
        logPeStatsDecimation();
        // Emit the last partial interval before the sampler stops, then stop
        // it: the sampler thread must not outlive the state it reads.
        logPeThreadSampler();
        stopPeThreadSampler();
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
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
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
        // T2 device-lost gate: render-path methods must early-return
        // D3DERR_DEVICELOST while the device awaits Reset.
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

        IDirect3DSurface9* presentSource = nullptr;
        const HRESULT presentSourceHr = GetBackBuffer(
            0u, 0u, D3DBACKBUFFER_TYPE_MONO, &presentSource);
        if (FAILED(presentSourceHr) || !presentSource) {
            return FAILED(presentSourceHr) ? presentSourceHr
                                           : D3DERR_INVALIDCALL;
        }
        const auto presentSourceWire = D3D9PeWireSurface(presentSource);
        if (!presentSourceWire.valid()) {
            presentSource->Release();
            return D3DERR_INVALIDCALL;
        }

        // sizeHint stays kLegacyPresentSizeHint even though no legacy
        // record is built: it is what the capacity precheck saw before, so
        // chunk seal cadence is unchanged.
        const D9CCommandChunkWirePresent presentWire{
            .hwnd = (uint64_t)(uintptr_t)wnd,
            .flags = 0,
            .hasSrc = src ? 1u : 0u,
            .hasDst = dst ? 1u : 0u,
            .sourceHandleIndex = 0u,
            .src = src ? cs : D9CRect{},
            .dst = dst ? cd : D9CRect{},
        };
        const HRESULT appendHr = appendRecord(
            D9C_COMMAND_RECORD_PRESENT, kLegacyPresentSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok =
                    dxmt9::d3d9::pe::appendPresent(
                        builder, presentWire, presentSourceWire);
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
            notePeThreadSamplerPresent();
            if (renderTapeCaptureWasActive) {
                finishRenderTapeCaptureAtPresentBoundary();
            } else if (peCaptureState_) {
                // The first successful Present is only the arm boundary;
                // the next Present owns the one captured interval.
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
                                               IDirect3DSurface9* pSurf) noexcept override {
        return withPeCallAndHotStateSetter(
            "SetRenderTarget", DXMT9_PE_CALLSITE_PC(), nullptr,
            PeHotStateSetterFamily::RenderTarget, nullptr,
            [&](auto& peCall, auto& hotSetter)
                __attribute__((always_inline)) noexcept -> HRESULT {
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("SetRenderTarget", hr);
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
            });
    }

    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx,
                                               IDirect3DSurface9** ppS) noexcept override;

    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pSurf) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::DepthStencil,
            "SetDepthStencilSurface", nullptr, nullptr,
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
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
            });
    }

    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppS) noexcept override;

    /* ── scene ── */
    HRESULT STDMETHODCALLTYPE BeginScene() noexcept override {
        return withPeCallScope(
            "BeginScene", DXMT9_PE_CALLSITE_PC(), nullptr,
            [&](auto& peCall) __attribute__((always_inline)) noexcept
                -> HRESULT {
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("BeginScene", hr);
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_begin_scene device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_scene(dev_));
        dxmt9DeviceDebugLog("device_begin_scene -> hr=0x%08x", (unsigned)hr);
        return finishPeCall(hr);
            });
    }
    HRESULT STDMETHODCALLTYPE EndScene()   noexcept override {
        return withPeCallScope(
            "EndScene", DXMT9_PE_CALLSITE_PC(), nullptr,
            [&](auto& peCall) __attribute__((always_inline)) noexcept
                -> HRESULT {
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("EndScene", hr);
        };
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_end_scene device=%p", this);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return finishPeCall(flushHr);
        const HRESULT hr = hr32(dxmt9c_device_end_scene(dev_));
        dxmt9DeviceDebugLog("device_end_scene -> hr=0x%08x", (unsigned)hr);
        return finishPeCall(hr);
            });
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* pRects,
                                     DWORD flags, D3DCOLOR color,
                                     float z, DWORD stencil) noexcept override {
        return withPeCallScope(
            "Clear", DXMT9_PE_CALLSITE_PC(), nullptr,
            [&](auto& peCall) __attribute__((always_inline)) noexcept
                -> HRESULT {
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("Clear", hr);
        };
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
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
        // rectCount / rectOffset are computed by appendClear from the span,
        // so they stay zero here. sizeHint keeps the legacy header+payload size
        // the capacity precheck saw before, so seal cadence is unchanged.
        const D9CCommandChunkWireClear clearWire{
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
        const HRESULT hr = appendRecord(
            D9C_COMMAND_RECORD_CLEAR,
            kLegacyClearSizeHint + rectBytes,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok =
                    dxmt9::d3d9::pe::appendClear(builder, clearWire, rects);
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        return finishPeCall(hr);
            });
    }

    /* ── transforms ── */
    template<typename HotSetter>
    HRESULT setTransformNormalized(D3DTRANSFORMSTATETYPE state,
                                   const D9CMatrix& wireM,
                                   dxmt9::d3d9::pe::WriteOrigin origin,
                                   HotSetter& hotSetter) noexcept {
        using namespace dxmt9::d3d9::pe;
        const std::uint32_t stateKey = static_cast<std::uint32_t>(state);
        std::uint32_t transformSlotIndex = 0u;
        if (!FixedTransformTable::slotForState(stateKey, transformSlotIndex)) {
            const HRESULT flushHr = flushPeRecorder();
            if (FAILED(flushHr)) return flushHr;
            const HRESULT hr = hr32(dxmt9c_device_set_transform(dev_, stateKey,
                                                                 &wireM));
            if (SUCCEEDED(hr)) hotSetter.markDirty();
            return hr;
        }

        const TransformState key = transformStateKey(stateKey);
        D9CMatrix liveValue{};
        const bool liveContains =
            peState_.transformShadowTyped().get(key, liveValue);
        const bool liveEquals = liveContains && matrixEquals(liveValue, wireM);
        const StateWritePlan plan = planRecorderStateWrite(StateWriteFacts{
            .phase = stateBlockRecording_ ? RecorderPhase::Recording
                                          : RecorderPhase::Live,
            .origin = origin,
            .liveContains = liveContains,
            .liveEquals = liveEquals,
            .pendingContains = peState_.pendingTransformsTyped().contains(key),
            .recordedContains =
                stateBlockState_.transforms().contains(key),
        });

        if (plan.kind == StateWriteKind::NoOp ||
            plan.kind == StateWriteKind::RetainPending) {
            return S_OK;
        }
        if (plan.writePending &&
            !peState_.pendingTransformsTyped().contains(key) &&
            peState_.pendingTransformsTyped().size() >=
                D9C_DRAW_PACKET_MAX_TRANSFORMS) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }

        // BeginStateBlock flushes all pending recorder state before entering
        // Recording. A direct prior-value operation is therefore ordered only
        // when no older transform delta survives. Fail closed if that
        // lifecycle premise is ever violated instead of allowing a later
        // pending replay to overwrite the direct result.
        if (plan.directOrderedCall &&
            peState_.pendingTransformsTyped().contains(key)) {
            return D3DERR_INVALIDCALL;
        }
        if (plan.directOrderedCall) {
            const HRESULT hr = hr32(
                dxmt9c_device_set_transform(dev_, stateKey, &wireM));
            if (FAILED(hr)) return hr;
        }
        if (plan.writeRecorded) {
            stateBlockState_.transforms().set(key, wireM);
        }
        if (plan.writeLive) {
            peState_.transformShadowTyped().set(key, wireM);
        }
        if (plan.writePending) {
            peState_.pendingTransformsTyped().set(key, wireM);
        }
        if (plan.semanticTransition || plan.directOrderedCall) {
            hotSetter.markDirty();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* pM) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::Transform, "SetTransform", nullptr,
            nullptr,
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog(
            "device_set_transform device=%p state=%u "
            "m=[[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g]]",
            this, (unsigned)state,
            pM->m[0][0], pM->m[0][1], pM->m[0][2], pM->m[0][3],
            pM->m[1][0], pM->m[1][1], pM->m[1][2], pM->m[1][3],
            pM->m[2][0], pM->m[2][1], pM->m[2][2], pM->m[2][3],
            pM->m[3][0], pM->m[3][1], pM->m[3][2], pM->m[3][3]);
        const D9CMatrix& wireM = *reinterpret_cast<const D9CMatrix*>(pM);
        return setTransformNormalized(
            state, wireM, dxmt9::d3d9::pe::WriteOrigin::ExplicitSet,
            hotSetter);
            });
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state,
                                            D3DMATRIX* pM) noexcept override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX* pM) noexcept override {
        notePeDeviceCallAfterPresent("MultiplyTransform");
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_multiply_transform device=%p state=%u", this, (unsigned)state);
        D3DMATRIX cur{};
        // GetTransform reads the primary PE shadow. Explicit SetTransform
        // calls made while recording update only the recording value, while
        // MultiplyTransform remains a prior-value operation on the primary
        // live value (matching Wine's stateblock behavior).
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
        const D9CMatrix& wireResult =
            *reinterpret_cast<const D9CMatrix*>(&result);
        return setTransformNormalized(
            state, wireResult,
            dxmt9::d3d9::pe::WriteOrigin::PriorValueOperation,
            peNullHotSetter_);
    }

    /* ── viewport / scissor ── */
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pVP) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::ViewportScissor, "SetViewport", nullptr,
            DXMT9_PE_CALLSITE_PC(),
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
        if (!pVP) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_viewport device=%p x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                            this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
        D9CViewport vp{ pVP->X, pVP->Y, pVP->Width, pVP->Height,
                        pVP->MinZ, pVP->MaxZ };
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // packet built for the next draw carries viewportValid=1 + the
        // shadow snapshot; server-side canonical state replay dispatches
        // dxmt9c_device_set_viewport before the draw runs.
        if (std::memcmp(&peState_.viewportShadow, &vp, sizeof(vp)) == 0) {
            return S_OK;
        }
        peState_.viewportShadow = vp;
        peState_.pendingViewport = true;
        hotSetter.markDirty();
        return S_OK;
            });
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pVP) noexcept override;
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pR) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::ViewportScissor, "SetScissorRect",
            nullptr, DXMT9_PE_CALLSITE_PC(),
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
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
            });
    }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pR) noexcept override;

    /* ── material / lights ── */
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pM) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::MaterialLightClip, "SetMaterial", nullptr,
            nullptr,
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_material device=%p", this);
        if (std::memcmp(&peState_.materialShadow, pM, sizeof(D9CMaterial)) == 0) {
            return S_OK;
        }
        std::memcpy(&peState_.materialShadow, pM, sizeof(D9CMaterial));
        peState_.pendingMaterial = true;
        hotSetter.markDirty();
        return S_OK;
            });
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pM) noexcept override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD idx, const D3DLIGHT9* pL) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::MaterialLightClip, "SetLight", nullptr,
            nullptr,
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
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
            });
    }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD idx, D3DLIGHT9* pL) noexcept override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::MaterialLightClip, "LightEnable", nullptr,
            nullptr,
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
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
            });
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD idx, BOOL* pEn) noexcept override;

    /* ── clip planes ── */
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* pPlane) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::MaterialLightClip, "SetClipPlane", nullptr,
            nullptr,
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
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
            });
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* pPlane) noexcept override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* p) noexcept override;
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* p) noexcept override;

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
     * dxmt9_draw_encoder_chunk.mm's surface-op Kind switch. The record is emitted
     * exactly like StretchRect/ColorFill (chunkBarrierFlush, then append with
     * source/dest retained), so it orders atomically with the surrounding
     * draws/clears in the same chunk. */
    HRESULT requestReszDepthResolve() noexcept {
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
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
        const HRESULT appendHr = appendRecord(
            D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
            kLegacyReszDepthResolveSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendReszDepthResolve(
                    builder, D3D9PeWireSurface(dsSurface_),
                    D3D9PeWireTexture(textures_[0]));
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        if (FAILED(appendHr)) return appendHr;
        return S_OK;
    }

    /* ── render states ── */
    template<typename HotSetter>
    HRESULT setRenderStateCore(D3DRENDERSTATETYPE state, DWORD value,
                               HotSetter& hotSetter) noexcept {
        using namespace dxmt9::d3d9::pe;
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
        const DWORD stateKey = static_cast<DWORD>(state);
        const RenderStateSlot renderKey = renderStateSlotKey(stateKey);
        std::uint32_t liveValue = 0u;
        const bool liveContains =
            peState_.renderStateShadowTyped().get(renderKey, liveValue);
        const StateWritePlan plan = planRecorderStateWrite(StateWriteFacts{
            .phase = stateBlockRecording_ ? RecorderPhase::Recording
                                          : RecorderPhase::Live,
            .origin = WriteOrigin::ExplicitSet,
            .liveContains = liveContains,
            .liveEquals = liveContains && liveValue == value,
            .pendingContains =
                peState_.pendingRenderStatesTyped().contains(renderKey),
            .recordedContains =
                stateBlockState_.renderStates().contains(renderKey),
        });
        if (plan.kind == StateWriteKind::NoOp ||
            plan.kind == StateWriteKind::RetainPending) {
            return S_OK;
        }
        // Phase 31: cap check — if a NEW state would push the pending
        // table past the per-packet cap, drain pending state into the chunk
        // via chunkBarrierFlush() so the next packet starts fresh.
        if (plan.writePending &&
            !peState_.pendingRenderStatesTyped().contains(renderKey) &&
            peState_.pendingRenderStatesTyped().size() >= D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        if (plan.directOrderedCall) {
            const HRESULT hr = hr32(
                dxmt9c_device_set_render_state(dev_, stateKey, value));
            if (FAILED(hr)) return hr;
        }
        if (plan.writeRecorded) {
            stateBlockState_.renderStates().set(renderKey, value);
        }
        if (plan.writeLive) {
            peState_.renderStateShadowTyped().set(renderKey, value);
        }
        if (plan.writePending) {
            peState_.pendingRenderStatesTyped().set(renderKey, value);
        }
        if (plan.semanticTransition || plan.directOrderedCall) hotSetter.markDirty();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) noexcept override {
        PeDiagnosticsState* const diagnostics = diagnostics_.get();
        if (!diagnostics) {
            return setRenderStateCore(state, value, peNullHotSetter_);
        }
        PeHotStateSetterTimer hotSetter(
            *this, *diagnostics, PeHotStateSetterFamily::RenderState,
            "SetRenderState",
            &PeDiagnosticsState::peEntryStateDecimatedStats_);
        return setRenderStateCore(state, value, hotSetter);
    }
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
        return withPeHotStateSetter(
            PeHotStateSetterFamily::TextureStageSampler,
            "SetTextureStageState",
            &PeDiagnosticsState::peEntryStateDecimatedStats_, nullptr,
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
        using namespace dxmt9::d3d9::pe;
        dxmt9DeviceDebugLog("device_set_texture_stage_state device=%p stage=%u type=%u value=0x%x",
                            this, (unsigned)stage, (unsigned)type, (unsigned)value);
        // Wine d3d9 test_limits + test_texture_stage_states: reject
        // out-of-range stage (>= caps.MaxTextureBlendStages == 8) and
        // unrecognised D3DTSS_* type with D3DERR_INVALIDCALL at the
        // device-method boundary.
        if (stage >= kFragmentBlendStageCount) return D3DERR_INVALIDCALL;
        if (!isValidTextureStageStateType(type)) return D3DERR_INVALIDCALL;
        const TextureStageIndex stageKey = textureStageIndexKey(stage);
        const TextureStageStateType typeKey =
            textureStageStateTypeKey(static_cast<uint32_t>(type));
        std::uint32_t liveValue = 0u;
        const bool liveContains =
            peState_.tssShadowTyped().get(stageKey, typeKey, liveValue);
        const StateWritePlan plan = planRecorderStateWrite(StateWriteFacts{
            .phase = stateBlockRecording_ ? RecorderPhase::Recording
                                          : RecorderPhase::Live,
            .origin = WriteOrigin::ExplicitSet,
            .liveContains = liveContains,
            .liveEquals = liveContains && liveValue == value,
            .pendingContains =
                peState_.pendingTssTyped().contains(stageKey, typeKey),
            .recordedContains =
                stateBlockState_.textureStageStates().contains(stageKey, typeKey),
        });
        if (plan.kind == StateWriteKind::NoOp ||
            plan.kind == StateWriteKind::RetainPending) {
            return S_OK;
        }
        // Phase 34: cap-check uses chunkBarrierFlush so pending state is
        // encoded as APPLY_STATE record(s) + cleared before the new entry.
        if (plan.writePending &&
            !peState_.pendingTssTyped().contains(stageKey, typeKey) &&
            peState_.pendingTssTyped().size() >= D9C_DRAW_PACKET_MAX_TSS) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        if (plan.directOrderedCall) {
            const HRESULT hr = hr32(dxmt9c_device_set_texture_stage_state(
                dev_, stage, static_cast<std::uint32_t>(type), value));
            if (FAILED(hr)) return hr;
        }
        if (plan.writeRecorded) {
            stateBlockState_.textureStageStates().set(stageKey, typeKey, value);
        }
        if (plan.writeLive) {
            peState_.tssShadowTyped().set(stageKey, typeKey, value);
        }
        if (plan.writePending) {
            peState_.pendingTssTyped().set(stageKey, typeKey, value);
        }
        if (plan.semanticTransition || plan.directOrderedCall) hotSetter.markDirty();
        return S_OK;
            });
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD* pValue) noexcept override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD value) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::TextureStageSampler, "SetSamplerState",
            &PeDiagnosticsState::peEntryStateDecimatedStats_, nullptr,
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
        using namespace dxmt9::d3d9::pe;
        dxmt9DeviceDebugLog("device_set_sampler_state device=%p sampler=%u type=%u value=0x%x",
                            this, (unsigned)sampler, (unsigned)type, (unsigned)value);
        SamplerIndex samplerIndexKeyVal{};
        if (!samplerIndexKey(sampler, samplerIndexKeyVal)) {
            return D3DERR_INVALIDCALL;
        }
        SamplerStateType stateTypeKeyVal{};
        if (!samplerStateTypeKey(static_cast<uint32_t>(type), stateTypeKeyVal)) {
            return S_OK;
        }
        std::uint32_t liveValue = 0u;
        const bool liveContains = peState_.samplerStateShadowTyped().get(
            samplerIndexKeyVal, stateTypeKeyVal, liveValue);
        const StateWritePlan plan = planRecorderStateWrite(StateWriteFacts{
            .phase = stateBlockRecording_ ? RecorderPhase::Recording
                                          : RecorderPhase::Live,
            .origin = WriteOrigin::ExplicitSet,
            .liveContains = liveContains,
            .liveEquals = liveContains && liveValue == value,
            .pendingContains = peState_.pendingSamplerStatesTyped().contains(
                samplerIndexKeyVal, stateTypeKeyVal),
            .recordedContains = stateBlockState_.samplerStates().contains(
                samplerIndexKeyVal, stateTypeKeyVal),
        });
        if (plan.kind == StateWriteKind::NoOp ||
            plan.kind == StateWriteKind::RetainPending) {
            return S_OK;
        }
        // Phase 34: cap-check uses chunkBarrierFlush.
        if (plan.writePending &&
            !peState_.pendingSamplerStatesTyped().contains(
                samplerIndexKeyVal, stateTypeKeyVal) &&
            peState_.pendingSamplerStatesTyped().size() >= D9C_DRAW_PACKET_MAX_SAMPLER) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        if (plan.directOrderedCall) {
            const HRESULT hr = hr32(dxmt9c_device_set_sampler_state(
                dev_, rawSlot(samplerIndexKeyVal),
                static_cast<std::uint32_t>(type), value));
            if (FAILED(hr)) return hr;
        }
        if (plan.writeRecorded) {
            stateBlockState_.samplerStates().set(
                samplerIndexKeyVal, stateTypeKeyVal, value);
        }
        if (plan.writeLive) {
            peState_.samplerStateShadowTyped().set(
                samplerIndexKeyVal, stateTypeKeyVal, value);
        }
        if (plan.writePending) {
            peState_.pendingSamplerStatesTyped().set(
                samplerIndexKeyVal, stateTypeKeyVal, value);
        }
        if (plan.semanticTransition || plan.directOrderedCall) hotSetter.markDirty();
        return S_OK;
            });
    }
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
        if (textures_[textureSlot] == pTex) {
            return S_OK;
        }
        setRef(textures_[textureSlot], pTex);
        applyCurrentPaletteToTexture(textures_[textureSlot]);
        peState_.pendingTextureMask |= 1u << textureSlot;
        hotSetter.markDirty();
        return S_OK;
            });
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage,
                                          IDirect3DBaseTexture9** ppTex) noexcept override;

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
        IDirect3DVertexDeclaration9* decl = CreatePeVertexDecl(d, this, this);
        if (!decl) return nullptr;
        notifyRenderTapeCreatedVertexDecl(
            d, D3D9PeWireVertexDecl(decl),
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(tmp),
                n * sizeof(tmp[0])), n);
        /* Cache holds one ref. */
        fvfDeclCache_.emplace(fvf, decl);
        return decl;
    }

    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) noexcept override {
        return withPeHotStateSetter(
            PeHotStateSetterFamily::VertexInput, "SetFVF", nullptr, nullptr,
            [&](auto& hotSetter) __attribute__((always_inline)) noexcept
                -> HRESULT {
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
        // PE-shadow stateblock support: remember that vdecl was touched
        // during BeginStateBlock/EndStateBlock so the resulting block's
        // tracked set includes the vdecl slot. The flag is consumed by
        // CaptureStateBlockShadowForChild and cleared in EndStateBlock.
        if (stateBlockRecording_) {
            stateBlockState_.vertexDeclarationRecorded = true;
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
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // packet built for the next draw carries vsValid=1 + the vs_
        // wire handle; server-side canonical state replay dispatches the
        // dxmt9c_device_set_vertex_shader call before the draw runs.
        if (vs_ == pVS) return S_OK;
        setRef(vs_, pVS);
        peState_.pendingVs = true;
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

    // Diagnostics-on body for SetVertexShaderConstantF, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantFSlow(UINT start, const float* pData,
                                  UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        PeCallScope peCall(*diagnostics_, "SetVertexShaderConstantF",
                           DXMT9_PE_CALLSITE_PC());
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
        return finishPeCall(applyConstStateWrite(
            peConsts_.vsConstF, stateBlockConsts_.vsConstF,
            start, count, pData, sizeof(float) * 4));
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* pData,
                                                        UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetVertexShaderConstantFSlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
        if (FAILED(hr)) return hr;
        // Shadow-only: defer the record until the next flushPendingConsts()
        // (called before each draw record + at chunk commit).
        return applyConstStateWrite(
            peConsts_.vsConstF, stateBlockConsts_.vsConstF,
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
            peConsts_.vsConstI, stateBlockConsts_.vsConstI,
            start, count, pData, sizeof(int32_t) * 4);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetVertexShaderConstantISlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            peConsts_.vsConstI, stateBlockConsts_.vsConstI,
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
            peConsts_.vsConstB, stateBlockConsts_.vsConstB,
            start, count, pData, sizeof(uint32_t));
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetVertexShaderConstantBSlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            peConsts_.vsConstB, stateBlockConsts_.vsConstB,
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
        if (indexBuf_ == pIBuf) return finishPeCall(S_OK);
        setRef(indexBuf_, pIBuf);
        peState_.pendingIb = true;
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
        if (ps_ == pPS) return S_OK;
        setRef(ps_, pPS);
        peState_.pendingPs = true;
        hotSetter.markDirty();
        return S_OK;
            });
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) noexcept override;
    // Diagnostics-on body for SetPixelShaderConstantF, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantFSlow(UINT start, const float* pData,
                                 UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        PeCallScope peCall(*diagnostics_, "SetPixelShaderConstantF",
                           DXMT9_PE_CALLSITE_PC());
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
            peConsts_.psConstF, stateBlockConsts_.psConstF,
            start, count, pData, sizeof(float) * 4));
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetPixelShaderConstantFSlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            peConsts_.psConstF, stateBlockConsts_.psConstF,
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
            peConsts_.psConstI, stateBlockConsts_.psConstI,
            start, count, pData, sizeof(int32_t) * 4);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetPixelShaderConstantISlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            peConsts_.psConstI, stateBlockConsts_.psConstI,
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
            peConsts_.psConstB, stateBlockConsts_.psConstB,
            start, count, pData, sizeof(uint32_t));
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetPixelShaderConstantBSlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            peConsts_.psConstB, stateBlockConsts_.psConstB,
            start, count, pData, sizeof(uint32_t));
    }
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
        if (peState_.pendingRenderStatesTyped().size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return finishPeCall(barrierHr);
        }
        auto drawSwvpPhase = peCall.phase(
            &PeDiagnosticsState::peDrawPhaseSwvpDecimatedStats_);
        SoftwareFfpDrawData swvpDraw{};
        HRESULT hr = trySoftwareFfpDrawPrimitive(type, startVertex, count, swvpDraw);
        if (hr == S_FALSE) {
            hr = trySoftwareProgrammableDrawPrimitive(
                type, startVertex, count, swvpDraw);
        }
        drawSwvpPhase.stop();
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
            auto drawRecordPhase = peCall.phase(
                &PeDiagnosticsState::peDrawPhaseRecordDecimatedStats_);
            hr = appendDrawPrimitiveRecord(type, startVertex, count);
            drawRecordPhase.stop();
            appendedDraw = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr) && appendedDraw) {
            if (!swvpDraw.vertices.empty()) {
                clearPendingHotState();
                peState_.pendingFvf = true;
                peState_.pendingVdecl = true;
                if (swvpDraw.bypassVertexShader) peState_.pendingVs = true;
            }
        }
        notePeCurrentCallReturnForInterAppendSplit();
        return finishPeCall(hr);
    }

    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type,
                                             UINT startVertex,
                                             UINT count) noexcept override {
        PeDiagnosticsState* const diagnostics = diagnostics_.get();
        if (!diagnostics) {
            return drawPrimitiveCore(
                type, startVertex, count, peNullCallScope_);
        }
        PeCallScope peCall(
            *diagnostics, "DrawPrimitive", DXMT9_PE_CALLSITE_PC(),
            &PeDiagnosticsState::peEntryDrawDecimatedStats_);
        return drawPrimitiveCore(type, startVertex, count, peCall);
    }
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
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_indexed_primitive device=%p type=%u base=%d min=%u num=%u startIndex=%u count=%u",
                            this, (unsigned)type, baseVertex, minVertex, numVertices,
                            startIndex, count);
        if (peState_.pendingRenderStatesTyped().size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return finishPeCall(barrierHr);
        }
        auto drawSwvpPhase = peCall.phase(
            &PeDiagnosticsState::peDrawPhaseSwvpDecimatedStats_);
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
        drawSwvpPhase.stop();
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
            auto drawRecordPhase = peCall.phase(
                &PeDiagnosticsState::peDrawPhaseRecordDecimatedStats_);
            hr = appendDrawIndexedPrimitiveRecord(type, baseVertex, minVertex,
                                                 numVertices, startIndex, count);
            drawRecordPhase.stop();
            appendedDraw = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr) && appendedDraw) {
            if (!swvpDraw.vertices.empty()) {
                clearPendingHotState();
                peState_.pendingFvf = true;
                peState_.pendingVdecl = true;
                peState_.pendingIb = indexBuf_ != nullptr;
                if (swvpDraw.bypassVertexShader) peState_.pendingVs = true;
            }
        }
        notePeCurrentCallReturnForInterAppendSplit();
        return finishPeCall(hr);
            });
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                               UINT count,
                                               const void* pData,
                                               UINT stride) noexcept override {
        return withPeCallScope(
            "DrawPrimitiveUP", DXMT9_PE_CALLSITE_PC(),
            &PeDiagnosticsState::peEntryDrawDecimatedStats_,
            [&](auto& peCall) __attribute__((always_inline)) noexcept
                -> HRESULT {
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("DrawPrimitiveUP", hr);
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_primitive_up device=%p type=%u count=%u data=%p stride=%u",
                            this, (unsigned)type, count, pData, stride);
        if (peState_.pendingRenderStatesTyped().size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
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
            if (!swvpDraw.vertices.empty()) {
                clearPendingHotState();
                peState_.pendingFvf = true;
                peState_.pendingVdecl = true;
                if (swvpDraw.bypassVertexShader) peState_.pendingVs = true;
            }
        }
        return finishPeCall(hr);
            });
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                      UINT minVertex,
                                                      UINT numVertices,
                                                      UINT count,
                                                      const void* pIdxData,
                                                      D3DFORMAT idxFmt,
                                                      const void* pVtxData,
                                                      UINT stride) noexcept override {
        return withPeCallScope(
            "DrawIndexedPrimitiveUP", DXMT9_PE_CALLSITE_PC(),
            &PeDiagnosticsState::peEntryDrawDecimatedStats_,
            [&](auto& peCall) __attribute__((always_inline)) noexcept
                -> HRESULT {
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("DrawIndexedPrimitiveUP", hr);
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_indexed_primitive_up device=%p type=%u min=%u num=%u count=%u idx=%p idxFmt=%u vtx=%p stride=%u",
                            this, (unsigned)type, minVertex, numVertices, count,
                            pIdxData, (unsigned)idxFmt, pVtxData, stride);
        if (peState_.pendingRenderStatesTyped().size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
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
            if (!swvpDraw.vertices.empty()) {
                clearPendingHotState();
                peState_.pendingFvf = true;
                peState_.pendingVdecl = true;
                if (swvpDraw.bypassVertexShader) peState_.pendingVs = true;
            }
        }
        return finishPeCall(hr);
            });
    }
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
                                         DWORD flags) noexcept override {
        dxmt9PeSetCurrentCallName("PresentEx");
        // T2 device-lost gate.
        if (deviceNotReset_) {
            if (peCaptureState_)
                abortRenderTapeCapture("device_lost");
            return D3DERR_DEVICELOST;
        }
        const bool renderTapeCaptureWasActive =
            peCaptureState_ &&
            peCaptureState_->renderTapeCapture.state() ==
                dxmt9::d3d9::RenderTapeCaptureState::Capturing;
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
            if (renderTapeCaptureWasActive)
                finishRenderTapeCaptureAtPresentBoundary();
            else if (peCaptureState_)
                (void)armRenderTapeCaptureAtPresentBoundary();
        }
        return hr;
    }

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
