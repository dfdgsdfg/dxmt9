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

#include "d3d9_pe_device_com_cold_helpers.inc.hpp"

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

// NOT an anonymous namespace. D3D9DeviceImpl has data members of the types
// declared below (std::optional<RenderTapeLiveRegistry> renderTapeRegistry_,
// std::vector<RenderTapeArmObjectSnapshot> renderTapeArmSnapshots_). In an
// anonymous namespace each including TU would get a distinct type, so the
// class would have a different type in each TU -- an ODR violation the
// compiler cannot diagnose. Keep these at namespace scope.

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
    // Native D3D9 semantics: the device lock is taken only when the app
    // passed D3DCREATE_MULTITHREADED; otherwise the app promises
    // single-threaded access and recorderMutex_ (below) is skipped on the
    // hot append path. Resolved once at construction — see
    // dxmt9PeRecorderLockRequired(). DXMT9_PE_FORCE_RECORDER_LOCK is the
    // rollback/insurance lane for apps that violate the contract.
    bool         recorderLockRequired_ = false;
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
    dxmt9::core::ThreadOwnershipToken recorderOwnership_{};
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

    PeHotStateShadow peState_{};
    PeConstShadowBlock peConsts_{};
    // Fixed-capacity DXMT9_PERF_VS_CONST_SETTER_RANGE diagnostic buckets.
    // Keeping them on the device avoids per-setter allocation; collection is
    // dormant unless the diagnostic gate is enabled.
    VsConstSetterRangePerf vsConstSetterRangePerf_{};
    IDirect3DSurface9* rtSlots_[4]{};
    bool rtSlotExplicit_[4]{};
    IDirect3DSurface9* dsSurface_ = nullptr;

    // Reused across draws so populateBindingView() does not zero ~850 bytes
    // per call. Mutable because the binding-view fill is a const method.
    mutable dxmt9::d3d9::pe::PeBindingView peBindingView_{};

    // Reused sparse-producer storage. The SparseStateInput spans point into
    // peSparseScratch_, so both must outlive the append that consumes them --
    // they are members for that reason as well as to avoid per-draw zeroing.
    mutable dxmt9::d3d9::pe::PeSparseScratch peSparseScratch_{};
    mutable dxmt9::d3d9::pe::SparseStateInput peSparseState_{};
    mutable D9CCommandChunkWireDrawHeader peSparseHeader_{};
    mutable dxmt9::d3d9::pe::PeDrawPayloads peSparsePayloads_{};
    bool dsSurfaceExplicit_ = false;
    dxmt9::d3d9::pe::CommandChunkBuilder commandChunk_{};
    bool commandChunkNegotiated_ = false;
    std::optional<dxmt9::d3d9::RenderTapeCaptureSession>
        renderTapeCapture_{};
    // Cold capture-only registry. It is disengaged for the normal renderer,
    // so capture-off draw/state paths retain their existing hot-path shape.
    std::optional<RenderTapeLiveRegistry> renderTapeRegistry_{};
    std::vector<dxmt9::d3d9::RenderTapeOracleAttachment>
        renderTapeCaptureOracle_{};
    std::optional<dxmt9::d3d9::RenderTapeDigest> renderTapeExpectedDigest_{};
    std::vector<std::byte> renderTapeExpectedPixels_{};
    std::vector<std::byte> renderTapeExpectedSourcePixels_{};
    std::optional<D9CSurfaceDesc> renderTapeOutputDesc_{};
    dxmt9::d3d9::RenderTapeArmBoundaryPhase renderTapeArmBoundaryPhase_ =
        dxmt9::d3d9::RenderTapeArmBoundaryPhase::Disabled;
    std::uint64_t renderTapeArmSnapshotOrdinal_ = 0u;
    std::uint64_t renderTapeNextCaptureToken_ = 0u;
    std::uint64_t renderTapeActiveCaptureToken_ = 0u;
    // Capture-only boundary selector. It is initialized once per device and
    // decremented only while the session is idle, so it cannot change command
    // recording, batching, or the selected interval after arming.
    std::uint32_t renderTapeArmPresentSkipRemaining_ = 0u;
    // Owned capture-interval overlay. It is generation- and arm-qualified,
    // never mutates the durable registry, and remains available to cold JIT
    // materialization until abort/retry/completion clears it.
    std::vector<RenderTapeArmObjectSnapshot> renderTapeArmSnapshots_{};
    // Exact identities admitted to the current tape. Pre-arm live objects
    // outside the bootstrap closure are materialized only on first reference.
    std::vector<D9CWireObjectIdentity> renderTapeAdmittedIdentities_{};
    // Capture-only observation state; this remains usable after capture aborts.
    dxmt9::d3d9::RenderTapeFirstAccessLedger renderTapeFirstAccessLedger_{};
    const char *renderTapeAbortReason_ = nullptr;
    std::uint64_t renderTapeCompletionOrdinal_ = 0u;
    std::uint64_t commandChunkCommits_ = 0;
    std::uint64_t commandChunkRecords_ = 0;
    std::uint64_t commandChunkBytes_ = 0;
    PeRecorderStats peRecorderStats_{};
    // DXMT9_PE_STATS_DECIMATION diagnostic accumulators (const_flush,
    // draw_packet, and canonical append scopes). The const_setter accumulator lives
    // next to its owner (touchConstShadow's function-local static) — see
    // d3d9_pe_stats_decimation.hpp and logPeStatsDecimation() below.
    // draw_packet's stats are mutable because the state build's forwarder
    // is a const method.
    PeDecimatedScopeStats peChunkAppendDecimatedStats_{};
    // Phase split of the record append, sampled on the same calls the
    // parent scope samples. CAVEAT: each phase costs one clock pair, so on a
    // sampled call the parent `append_sampled_ms` is inflated by roughly four
    // pairs. Read the phases against each other, not against the parent, and
    // do not quote the parent's absolute figure from a run with these enabled
    // -- a 2026-07-29 GT2 run read 4,180 ns for the parent here against
    // 2,851 ns with the phases off. Direct canonical emitters time only their builder
    // append as `encode`; capacity commits accumulate in the `flush` phase.
    // Per-record-type append census. Counting only -- no clock reads -- so it
    // can run alongside the decimated timers without adding to the bias that
    // 2026-07-29 showed dominates short scopes. Tells us which encode path in
    // the record encode is worth opening.
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

    PeDecimatedScopeStats peAppendPhaseEncode_{};
    PeDecimatedScopeStats peAppendPhaseFlush_{};
    PeDecimatedScopeStats peConstFlushDecimatedStats_{};
    // D3D9 ENTRY-level scopes. The four original scopes measure what the
    // recorder does; these measure the whole call the app made, so
    // `entry - (inner scopes it contains)` is the PE layer nothing has ever
    // measured: argument validation, DeviceState mutation and dirty
    // bookkeeping, and the finishPeCall tail. That residual is why
    // frame-lifecycle calls 15.1% a floor, and
    // present-pacing-post-defselect-cpu-attribution.05 established it cannot
    // be recovered by profiling from outside -- both the game and our PE
    // d3d9.dll are unattributed translated code to xctrace. Inside is the
    // only direction left.
    PeDecimatedScopeStats peEntryConstDecimatedStats_{};
    PeDecimatedScopeStats peEntryDrawDecimatedStats_{};
    PeDecimatedScopeStats peEntryStateDecimatedStats_{};
    // Sub-phases of the draw entry, which is 11.5us/call and 37% of the GT2
    // frame on its own (append-decomposition.08). swvp = the two SWVP fallback
    // probes plus the containers they fill; record = the actual draw-record
    // append. entry - swvp - record is the rest of the call.
    PeDecimatedScopeStats peDrawPhaseSwvpDecimatedStats_{};
    PeDecimatedScopeStats peDrawPhaseRecordDecimatedStats_{};
    // phaseOffset 2 (const_setter owns 1). buildSparseStateForRecord runs once
    // per draw record, so this scope's counter tracks peEntryDrawDecimatedStats_
    // to within 0.6% (1,944,455 vs 1,933,451 events on GT2 -- the drift is the
    // APPLY_STATE path). That is near-lockstep, the shape banned at the top of
    // d3d9_pe_stats_decimation.hpp: at phase 0 the coincidence is bursty rather
    // than total, which averages to only ~11ns of bias on entry_draw today, but
    // the drift rate is a property of the workload and nothing keeps it small.
    mutable PeDecimatedScopeStats peDrawPacketDecimatedStats_ = [] {
      PeDecimatedScopeStats s{};
      s.phaseOffset = 2;
      return s;
    }();
    std::uint64_t peStatsDecimationPresents_ = 0;
    // DXMT9_PE_THREAD_SAMPLER. Heap-owned and released through
    // PeThreadSampler::stopAndRelease(), which deliberately leaks the block
    // rather than freeing it under a sampler thread that did not join in time.
    dxmt9::d3d9::pe::PeThreadSampler* peThreadSampler_ = nullptr;
    std::uint64_t peThreadSamplerPresents_ = 0;
    bool peThreadSamplerPresentThreadChecked_ = false;
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

    DWORD renderStateValue(D3DRENDERSTATETYPE state) const {
        uint32_t shadowValue = 0;
        if (peState_.renderStateShadow.get(static_cast<DWORD>(state), shadowValue)) {
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
    // Not const: buildSparseState takes the const shadow by non-const
    // reference. It does not currently write to it -- see that function's
    // comment -- but the signature reserves the right, so this must not claim
    // const and cast it away.
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

    // The whole entry note, reached only with tracking on. It writes the entry
    // sample into `out`, which is always diagnostic-owned storage -- either a
    // call-scope slot or a throwaway local -- never a hot-path return value.
    void notePeDeviceCallAfterPresentTracked(const char* callName,
                                             const void* callerPc,
                                             PePresentCallSample& out) {
        dxmt9PeSetCurrentCallName(callName);
        dxmt9PeCurrentCallEntryNs =
            dxmt9SteadyClockNs(std::chrono::steady_clock::now());
        recordPeBetweenCallsEntry(callName, dxmt9PeCurrentCallEntryNs,
                                  callerPc);
        out = logPeCallMilestoneAfterPresent(callName, callerPc);
        if (!out.tracked) {
            out = PePresentCallSample{};
            out.entryNs = dxmt9PeCurrentCallEntryNs;
            out.callerPc = callerPc;
            out.threadId = dxmt9PeCurrentThreadId();
            dxmt9PeCaptureCallStack(out);
        }
        logPeFirstCallAfterPresent(callName, claimPeFirstCallAfterPresent(),
                                   out);
    }

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

    // RAII scope for a device entry point that pairs its entry note with a
    // return log. Off: one cached-bool test in the constructor, a two-word
    // object, and a destructor that does nothing. On: the ~96-byte entry sample
    // stays in the diagnostic's own slot storage. finish() is a no-op for an
    // untracked scope, and a return path that skips it leaks nothing because
    // the destructor releases the slot. See "Observer boundary" in
    // agents/rules/codebase_conventions.rules.md.
    class PeCallScope {
    public:
        PeCallScope(D3D9DeviceImpl& device, const char* callName,
                    const void* callerPc = nullptr) noexcept
            : device_(device),
              slot_(device.pushPeCallScope(callName, callerPc)) {}
        PeCallScope(const PeCallScope&) = delete;
        PeCallScope& operator=(const PeCallScope&) = delete;
        ~PeCallScope() noexcept { device_.popPeCallScope(slot_); }

        HRESULT finish(const char* callName, HRESULT hr) noexcept {
            device_.notePeCallScopeReturn(slot_, callName, hr);
            return hr;
        }

    private:
        D3D9DeviceImpl& device_;
        D3D9PePresentCallSlot slot_;
    };

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
            commandChunk_.recordCount() == 0) {
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
        const auto& appendStats = peChunkAppendDecimatedStats_;
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
            "append_encode_sampled=%llu append_encode_ms=%.3f "
            "append_flush_sampled=%llu append_flush_ms=%.3f "
            "entry_const_events=%llu entry_const_sampled=%llu entry_const_ms=%.3f "
            "entry_draw_events=%llu entry_draw_sampled=%llu entry_draw_ms=%.3f "
            "entry_state_events=%llu entry_state_sampled=%llu entry_state_ms=%.3f "
            "draw_swvp_sampled=%llu draw_swvp_ms=%.3f "
            "draw_record_sampled=%llu draw_record_ms=%.3f "
            "%s%s",
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
            static_cast<unsigned long long>(peAppendPhaseEncode_.sampled),
            static_cast<double>(peAppendPhaseEncode_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peAppendPhaseFlush_.sampled),
            static_cast<double>(peAppendPhaseFlush_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peEntryConstDecimatedStats_.events),
            static_cast<unsigned long long>(peEntryConstDecimatedStats_.sampled),
            static_cast<double>(peEntryConstDecimatedStats_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peEntryDrawDecimatedStats_.events),
            static_cast<unsigned long long>(peEntryDrawDecimatedStats_.sampled),
            static_cast<double>(peEntryDrawDecimatedStats_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peEntryStateDecimatedStats_.events),
            static_cast<unsigned long long>(peEntryStateDecimatedStats_.sampled),
            static_cast<double>(peEntryStateDecimatedStats_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peDrawPhaseSwvpDecimatedStats_.sampled),
            static_cast<double>(peDrawPhaseSwvpDecimatedStats_.sampledNs) / 1.0e6,
            static_cast<unsigned long long>(peDrawPhaseRecordDecimatedStats_.sampled),
            static_cast<double>(peDrawPhaseRecordDecimatedStats_.sampledNs) / 1.0e6,
            constSetterBucketText.c_str(),
            appendTypeText.c_str());
    }

    // DXMT9_PE_THREAD_SAMPLER: start one sampler targeting the thread that
    // created the device. That is the game thread by construction — D3D9
    // device creation is what the renderer thread does — and it is the thread
    // whose PE-side samples xctrace cannot attribute.
    void startPeThreadSamplerIfRequested() {
        if (!dxmt9PeThreadSamplerEnabled()) {
            return;
        }
        const std::uint32_t hz = dxmt9PeThreadSamplerHz();
        // A second device must not add a second thread stopping the same
        // target, so the sampler refuses; distinguish that from an OS failure
        // rather than reporting a stale GetLastError() for it.
        const bool alreadyLive =
            dxmt9::d3d9::pe::PeThreadSampler::processSamplerIsLive();
        peThreadSampler_ =
            dxmt9::d3d9::pe::PeThreadSampler::startForThread(GetCurrentThreadId(), hz);
        if (!peThreadSampler_) {
            if (alreadyLive) {
                dxmt9PeThreadSamplerInfoLog(
                    "not_started reason=already_running_for_process thread_id=0x%lx",
                    static_cast<unsigned long>(GetCurrentThreadId()));
            } else {
                dxmt9PeThreadSamplerInfoLog(
                    "not_started reason=start_failed thread_id=0x%lx hz=%u "
                    "error=0x%08lx",
                    static_cast<unsigned long>(GetCurrentThreadId()), hz,
                    static_cast<unsigned long>(GetLastError()));
            }
            return;
        }
        dxmt9PeThreadSamplerInfoLog(
            "started thread_id=0x%lx hz=%u interval_ms=%u",
            static_cast<unsigned long>(peThreadSampler_->targetThreadId()), hz,
            peThreadSampler_->intervalMs());
    }

    void stopPeThreadSampler() {
        if (!peThreadSampler_) {
            return;
        }
        dxmt9::d3d9::pe::PeThreadSampler::stopAndRelease(peThreadSampler_);
        peThreadSampler_ = nullptr;
    }

    // Emits ONE cumulative [dxmt9-pe-sampler] group: a header line, one line
    // per module with a nonzero count (top 20 by count), then the self-module
    // PC histogram's top buckets. Counters are cumulative and never reset, so
    // a consumer reads the LAST group in the log.
    void logPeThreadSampler() {
        if (!peThreadSampler_) {
            return;
        }
        dxmt9::d3d9::pe::PeSamplerSnapshot snap;
        peThreadSampler_->snapshot(snap);
        dxmt9PeThreadSamplerInfoLog(
            "presents=%llu samples=%llu suspend_failures=%llu "
            "ctx_failures=%llu resume_failures=%llu hz=%u module_table=%u",
            static_cast<unsigned long long>(peThreadSamplerPresents_),
            static_cast<unsigned long long>(snap.samples),
            static_cast<unsigned long long>(snap.suspendFailures),
            static_cast<unsigned long long>(snap.contextFailures),
            static_cast<unsigned long long>(snap.resumeFailures),
            snap.hz,
            snap.moduleTableReady ? 1u : 0u);
        for (std::size_t i = 0; i < snap.moduleRows; ++i) {
            dxmt9PeThreadSamplerInfoLog(
                "module=%s samples=%llu",
                snap.moduleNames[i],
                static_cast<unsigned long long>(snap.topModules[i].samples));
        }
        if (snap.selfModuleName[0] != '\0') {
            // Names the module the buckets below are relative to, so the join
            // tool subtracts the right [dxmt9-pe-module-map] base for the RVA.
            dxmt9PeThreadSamplerInfoLog("selfpc_module=%s", snap.selfModuleName);
        }
        for (std::size_t i = 0; i < snap.selfPcRows; ++i) {
            dxmt9PeThreadSamplerInfoLog(
                "selfpc bucket=0x%llx samples=%llu",
                static_cast<unsigned long long>(snap.topSelfPc[i].bucket),
                static_cast<unsigned long long>(snap.topSelfPc[i].samples));
        }
        dxmt9PeThreadSamplerInfoLog(
            "selfpc_overflow=%llu",
            static_cast<unsigned long long>(snap.selfPcOverflow));
    }

    // Present-cadence tick for the sampler dump, mirroring the decimation
    // cadence: cumulative line every 60 presents, plus a final line from the
    // destructor so the last partial interval is never lost.
    void notePeThreadSamplerPresent() {
        if (!peThreadSampler_) {
            return;
        }
        // The sampler targets the thread that created the device on the
        // assumption that it is also the thread that renders. If an app splits
        // those, every sample describes the wrong thread and nothing else in
        // the output would say so — the histogram would just look idle. Say it
        // once, loudly, instead of leaving a silently wrong answer.
        if (!peThreadSamplerPresentThreadChecked_) {
            peThreadSamplerPresentThreadChecked_ = true;
            const DWORD presentThread = GetCurrentThreadId();
            if (presentThread != peThreadSampler_->targetThreadId()) {
                dxmt9PeThreadSamplerInfoLog(
                    "target_thread_mismatch sampled=0x%lx present=0x%lx "
                    "note=samples_describe_the_device_creating_thread_not_the_present_thread",
                    static_cast<unsigned long>(peThreadSampler_->targetThreadId()),
                    static_cast<unsigned long>(presentThread));
            }
        }
        ++peThreadSamplerPresents_;
        if (peThreadSamplerPresents_ % 60 == 0) {
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

    RenderTapeLiveObject *findRenderTapeObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
        if (!renderTapeRegistry_) {
            return nullptr;
        }
        const auto it = std::find_if(
            renderTapeRegistry_->objects.begin(),
            renderTapeRegistry_->objects.end(),
            [&](const auto &entry) {
                return renderTapeSameIdentity(entry.identity, object.identity);
            });
        return it == renderTapeRegistry_->objects.end() ? nullptr : &*it;
    }

    const RenderTapeLiveObject *findRenderTapeObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) const noexcept {
        if (!renderTapeRegistry_) {
            return nullptr;
        }
        const auto it = std::find_if(
            renderTapeRegistry_->objects.begin(),
            renderTapeRegistry_->objects.end(),
            [&](const auto &entry) {
                return renderTapeSameIdentity(entry.identity, object.identity);
            });
        return it == renderTapeRegistry_->objects.end() ? nullptr : &*it;
    }

    bool hasRenderTapeDeadObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) const noexcept {
        return renderTapeRegistry_ &&
               std::any_of(renderTapeRegistry_->knownDead.begin(),
                           renderTapeRegistry_->knownDead.end(),
                           [&](const auto &identity) {
                               return renderTapeSameIdentity(identity,
                                                             object.identity);
                           });
    }

    void markRenderTapeInvalidOnce(
        const char *reason,
        const dxmt9::d3d9::pe::PeWireObjectRef *object = nullptr,
        std::uint32_t subresource =
            std::numeric_limits<std::uint32_t>::max(),
        const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic =
            {}) noexcept {
        if (!renderTapeRegistry_ || renderTapeRegistry_->invalid) {
            return;
        }
        renderTapeRegistry_->invalid = true;
        renderTapeRegistry_->invalidReason = reason;
        renderTapeRegistry_->invalidSubresource = subresource;
        renderTapeRegistry_->invalidLayout = diagnostic;
        if (object) {
            renderTapeRegistry_->invalidKind = object->identity.kind;
            renderTapeRegistry_->invalidGeneration = object->identity.generation;
            renderTapeRegistry_->invalidObjectId = object->identity.objectId;
        }
    }

    void abortRenderTapeCapture(const char *reason) noexcept {
        if (!renderTapeCapture_ || !renderTapeCapture_->enabled()) {
            return;
        }
        if (!renderTapeAbortReason_) {
            renderTapeAbortReason_ = reason;
            dxmt9DeviceInfoLog("render_tape_capture first_abort reason=%s",
                               reason);
        }
        renderTapeCapture_->abort();
        renderTapeArmBoundaryPhase_ =
            dxmt9::d3d9::RenderTapeArmBoundaryPhase::Disabled;
        renderTapeArmSnapshots_.clear();
        renderTapeExpectedDigest_.reset();
        renderTapeExpectedPixels_.clear();
        renderTapeExpectedSourcePixels_.clear();
        renderTapeOutputDesc_.reset();
        renderTapeActiveCaptureToken_ = 0u;
    }

    RenderTapeObjectRegistration registerRenderTapeObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::span<const std::byte> descriptor,
        std::span<const std::byte> immutablePayload,
        RenderTapeLiveObject::Role role = RenderTapeLiveObject::Role::Ordinary,
        std::uint32_t replacementRestart = 0u) noexcept {
        if (!renderTapeRegistry_) {
            return RenderTapeObjectRegistration::Rejected;
        }
        bool replacingRetainedAlias = false;
        try {
            if (!object.valid(object.identity.kind)) {
                markRenderTapeInvalidOnce("invalid_identity", &object);
                return RenderTapeObjectRegistration::Rejected;
            }
            if (descriptor.empty()) {
                markRenderTapeInvalidOnce("empty_descriptor", &object);
                return RenderTapeObjectRegistration::Rejected;
            }
            if (auto *existing = findRenderTapeObject(object)) {
                const bool descriptorMatches =
                    (existing->descriptor.size() == descriptor.size() &&
                     std::equal(existing->descriptor.begin(),
                                existing->descriptor.end(),
                                descriptor.begin())) ||
                    (object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
                     dxmt9::d3d9::renderTapeSurfaceAliasDescriptorsEqual(
                         existing->descriptor, descriptor));
                if (!descriptorMatches ||
                    existing->immutablePayload.size() !=
                        immutablePayload.size() ||
                    !std::equal(existing->immutablePayload.begin(),
                                existing->immutablePayload.end(),
                                immutablePayload.begin())) {
                    markRenderTapeInvalidOnce("duplicate_object_identity_conflict",
                                             &object);
                    return RenderTapeObjectRegistration::Rejected;
                }
                if (!existing->lifetime.acquire()) {
                    markRenderTapeInvalidOnce("duplicate_object_ref_overflow",
                                             &object);
                    return RenderTapeObjectRegistration::Rejected;
                }
                return RenderTapeObjectRegistration::Existing;
            }
            if (hasRenderTapeDeadObject(object)) {
                markRenderTapeInvalidOnce(
                    "duplicate_retired_object_identity", &object);
                if (IsRenderTapeCaptureActiveForChild())
                    abortRenderTapeCapture(
                        "duplicate_retired_object_identity");
                return RenderTapeObjectRegistration::Rejected;
            }
            const auto logicalSlot = dxmt9::d3d9::
                renderTapeLogicalObjectSlot(object.identity, descriptor);
            if (logicalSlot.malformedSurfaceDescriptor) {
                markRenderTapeInvalidOnce("surface_descriptor_v2", &object);
                if (IsRenderTapeCaptureActiveForChild())
                    abortRenderTapeCapture("surface_descriptor_v2");
                return RenderTapeObjectRegistration::Rejected;
            }
            const auto generationDoesNotAdvance =
                [&](const D9CWireObjectIdentity &prior) {
                    return dxmt9::d3d9::renderTapeSameWireObject(
                               prior, object.identity) &&
                           !dxmt9::d3d9::renderTapeWireGenerationAdvances(
                               prior, object.identity);
                };
            if (std::any_of(renderTapeRegistry_->objects.begin(),
                            renderTapeRegistry_->objects.end(),
                            [&](const auto &candidate) {
                                return generationDoesNotAdvance(
                                    candidate.identity);
                            }) ||
                std::any_of(renderTapeRegistry_->knownDead.begin(),
                            renderTapeRegistry_->knownDead.end(),
                            generationDoesNotAdvance)) {
                markRenderTapeInvalidOnce("non_monotone_generation", &object);
                if (IsRenderTapeCaptureActiveForChild())
                    abortRenderTapeCapture("non_monotone_generation");
                return RenderTapeObjectRegistration::Rejected;
            }
            const auto replacement = std::find_if(
                renderTapeRegistry_->objects.begin(),
                renderTapeRegistry_->objects.end(),
                [&](const auto &candidate) {
                    const auto candidateSlot = dxmt9::d3d9::
                        renderTapeLogicalObjectSlot(candidate.identity,
                                                    candidate.descriptor);
                    return dxmt9::d3d9::renderTapeLogicalSlotRelation(
                               candidateSlot, logicalSlot) !=
                           dxmt9::d3d9::
                               RenderTapeLogicalSlotRelation::Different;
                });
            std::size_t replacementIndex =
                std::numeric_limits<std::size_t>::max();
            if (replacement != renderTapeRegistry_->objects.end()) {
                const auto transition = dxmt9::d3d9::
                    renderTapeSurfaceAliasReplacementStatus(
                        replacement->identity, replacement->lifetime,
                        replacement->descriptor, object.identity, descriptor);
                if (transition == dxmt9::d3d9::
                                      RenderTapeSurfaceAliasReplacementStatus::
                                          PendingChunkRequiresFlush) {
                    const auto priorIdentity = replacement->identity;
                    const auto priorPending =
                        replacement->lifetime.pendingChunkRefs;
                    dxmt9DeviceInfoLog(
                        "render_tape_capture alias_pending_flush profile=%u "
                        "device=%p registry=%p restart=%u "
                        "old_kind=%u old_generation=%u old_object_id=%llu "
                        "new_kind=%u new_generation=%u new_object_id=%llu "
                        "pending=%u admitted=%d known_dead=%d",
                        dxmt9PeRenderTapeCaptureProfile(), this,
                        static_cast<void *>(&*renderTapeRegistry_),
                        replacementRestart, priorIdentity.kind,
                        priorIdentity.generation,
                        static_cast<unsigned long long>(priorIdentity.objectId),
                        object.identity.kind, object.identity.generation,
                        static_cast<unsigned long long>(object.identity.objectId),
                        priorPending,
                        renderTapeObjectAdmitted(priorIdentity) ? 1 : 0,
                        hasRenderTapeDeadObject(dxmt9::d3d9::pe::PeWireObjectRef{
                            .identity = priorIdentity}) ? 1 : 0);
                    if (replacementRestart != 0u) {
                        markRenderTapeInvalidOnce(
                            "alias_pending_flush_restart_exhausted", &object);
                        if (IsRenderTapeCaptureActiveForChild())
                            abortRenderTapeCapture(
                                "alias_pending_flush_restart_exhausted");
                        return RenderTapeObjectRegistration::Rejected;
                    }
                    const HRESULT flushHr = flushPendingCommandChunk(
                        PeRecorderFlushReason::Child);
                    if (FAILED(flushHr)) {
                        markRenderTapeInvalidOnce(
                            "alias_pending_flush_failed", &object);
                        if (IsRenderTapeCaptureActiveForChild())
                            abortRenderTapeCapture(
                                "alias_pending_flush_failed");
                        return RenderTapeObjectRegistration::Rejected;
                    }
                    return registerRenderTapeObject(
                        object, descriptor, immutablePayload, role,
                        replacementRestart + 1u);
                }
                if (transition != dxmt9::d3d9::
                                      RenderTapeSurfaceAliasReplacementStatus::
                                          Accepted) {
                    const char *reason = dxmt9::d3d9::
                        renderTapeSurfaceAliasReplacementStatusName(transition);
                    markRenderTapeInvalidOnce(reason, &object);
                    dxmt9DeviceInfoLog(
                        "render_tape_capture alias_generation rejected reason=%s "
                        "old_generation=%u new_generation=%u "
                        "old_object_id=%llu new_object_id=%llu",
                        reason, replacement->identity.generation,
                        object.identity.generation,
                        static_cast<unsigned long long>(
                            replacement->identity.objectId),
                        static_cast<unsigned long long>(
                            object.identity.objectId));
                    if (IsRenderTapeCaptureActiveForChild())
                        abortRenderTapeCapture(reason);
                    return RenderTapeObjectRegistration::Rejected;
                }
                replacingRetainedAlias = true;
                replacementIndex = static_cast<std::size_t>(
                    replacement - renderTapeRegistry_->objects.begin());
            }
            RenderTapeLiveObject entry{};
            entry.identity = object.identity;
            entry.descriptor.assign(descriptor.begin(), descriptor.end());
            entry.immutablePayload.assign(immutablePayload.begin(),
                                          immutablePayload.end());
            if (!entry.lifetime.acquire()) {
                markRenderTapeInvalidOnce("object_ref_overflow", &object);
                return RenderTapeObjectRegistration::Rejected;
            }
            entry.role = role;
            switch (object.identity.kind) {
            case D9C_CHUNK_HANDLE_KIND_BUFFER:
                if (entry.descriptor.size() != sizeof(D9CBufferDesc)) {
                    markRenderTapeInvalidOnce("buffer_descriptor_size", &object);
                    return RenderTapeObjectRegistration::Rejected;
                }
                entry.contentCount = 1u;
                break;
            case D9C_CHUNK_HANDLE_KIND_SURFACE: {
                dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
                if (!dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(
                        entry.descriptor, surface)) {
                    markRenderTapeInvalidOnce("surface_descriptor_v2",
                                              &object);
                    return RenderTapeObjectRegistration::Rejected;
                }
                if (surface.storage == static_cast<std::uint32_t>(
                                           dxmt9::d3d9::RenderTapeSurfaceStorage::
                                               TextureSubresource)) {
                    if (role == RenderTapeLiveObject::Role::PresentOutput) {
                        markRenderTapeInvalidOnce("surface_descriptor_role",
                                                  &object);
                        return RenderTapeObjectRegistration::Rejected;
                    }
                    entry.lifetime.textureAlias = true;
                    entry.aliasParentTexture = surface.parentTexture;
                    entry.contentCount = 0u;
                    break;
                }
                if ((role == RenderTapeLiveObject::Role::PresentOutput) !=
                    (surface.storage == static_cast<std::uint32_t>(
                         dxmt9::d3d9::RenderTapeSurfaceStorage::
                             SwapchainBackbuffer))) {
                    markRenderTapeInvalidOnce("surface_descriptor_role", &object);
                    return RenderTapeObjectRegistration::Rejected;
                }
                entry.contentCount =
                    role == RenderTapeLiveObject::Role::PresentOutput ? 0u
                                                                        : 1u;
                break;
            }
            case D9C_CHUNK_HANDLE_KIND_TEXTURE: {
                RenderTapeTextureDescriptorV2 texture{};
                if (!dxmt9::d3d9::renderTapeLoadTextureDescriptorV2(
                        entry.descriptor, texture) ||
                    texture.initialContentDisposition !=
                        static_cast<std::uint32_t>(dxmt9::d3d9::
                            RenderTapeInitialContentDisposition::CompleteSeed)) {
                    markRenderTapeInvalidOnce("texture_descriptor_v2", &object);
                    return RenderTapeObjectRegistration::Rejected;
                }
                entry.contentCount = texture.subresourceCount;
                break;
            }
            default:
                break;
            }
            entry.content.resize(entry.contentCount);
            if (replacingRetainedAlias) {
                auto &prior = renderTapeRegistry_->objects[replacementIndex];
                const bool priorAdmitted = renderTapeObjectAdmitted(prior.identity);
                renderTapeRegistry_->knownDead.reserve(
                    renderTapeRegistry_->knownDead.size() + 1u);
                if (priorAdmitted && IsRenderTapeCaptureActiveForChild()) {
                    const auto destroyStatus =
                        renderTapeCapture_->objectDestroy(prior.identity);
                    if (destroyStatus != dxmt9::d3d9::
                                             RenderTapeCaptureStatus::Accepted) {
                        markRenderTapeInvalidOnce(
                            "alias_generation_destroy_failed", &object);
                        abortRenderTapeCapture(
                            "alias_generation_destroy_failed");
                        return RenderTapeObjectRegistration::Rejected;
                    }
                    removeRenderTapeObjectAdmitted(prior.identity);
                }
                dxmt9DeviceInfoLog(
                    "render_tape_capture alias_generation replaced "
                    "old_generation=%u new_generation=%u object_id=%llu",
                    prior.identity.generation, object.identity.generation,
                    static_cast<unsigned long long>(object.identity.objectId));
                renderTapeRegistry_->knownDead.push_back(prior.identity);
                prior = std::move(entry);
                return RenderTapeObjectRegistration::New;
            }
            renderTapeRegistry_->objects.push_back(std::move(entry));
            return RenderTapeObjectRegistration::New;
        } catch (...) {
            markRenderTapeInvalidOnce("object_registration_exception", &object);
            if (replacingRetainedAlias && IsRenderTapeCaptureActiveForChild())
                abortRenderTapeCapture("object_registration_exception");
            return RenderTapeObjectRegistration::Rejected;
        }
    }

    // Hand the PresentOutput role back before a new admission names a holder,
    // and again whenever an arm attempt ends without an active interval. A
    // retained holder is the r6 GT2 failure: every retry re-admitted a fresh
    // back-buffer wrapper while the previous one stayed live and roled, so the
    // arm saw two through eight present outputs, and a recycled wire object id
    // then collided with the stale entry and marked the registry invalid for
    // the rest of the process.
    void releaseRenderTapePresentOutputRole(
        const D9CWireObjectIdentity *next) noexcept {
        if (!renderTapeRegistry_) {
            return;
        }
        // Only a pre-arm or aborted lifecycle may move the role. An armed or
        // capturing interval owns its holder until it terminates.
        if (renderTapeCapture_ &&
            renderTapeCapture_->state() !=
                dxmt9::d3d9::RenderTapeCaptureState::Disabled &&
            renderTapeCapture_->state() !=
                dxmt9::d3d9::RenderTapeCaptureState::Aborted) {
            return;
        }
        auto &role = renderTapeRegistry_->presentOutputRole;
        const dxmt9::d3d9::pe::PeWireObjectRef priorObject{
            .identity = role.identity,
        };
        auto *prior = role.held ? findRenderTapeObject(priorObject) : nullptr;
        const auto transition = dxmt9::d3d9::renderTapePresentOutputRoleTransition(
            role, next, prior != nullptr,
            prior ? prior->lifetime.wrapperRefs : 0u);
        if (transition == dxmt9::d3d9::RenderTapePresentOutputRoleTransition::
                              Retained) {
            return;
        }
        const auto identity = role.identity;
        // Only a capture-owned holder carries a wrapper reference this device
        // took; an app-owned entry was merely re-roled in place.
        const bool releaseAdmissionRef = role.captureOwned;
        const auto priorContentCount =
            renderTapeRegistry_->presentOutputPriorContentCount;
        auto priorDescriptor =
            std::move(renderTapeRegistry_->presentOutputPriorDescriptor);
        auto priorContent =
            std::move(renderTapeRegistry_->presentOutputPriorContent);
        role = {};
        renderTapeRegistry_->presentOutputPriorDescriptor.clear();
        renderTapeRegistry_->presentOutputPriorContentCount = 0u;
        renderTapeRegistry_->presentOutputPriorContent.clear();
        if (transition ==
            dxmt9::d3d9::RenderTapePresentOutputRoleTransition::None) {
            return;
        }
        if (prior->role != RenderTapeLiveObject::Role::PresentOutput) {
            // Something else already reclaimed the entry; leave it alone.
            return;
        }
        if (transition ==
            dxmt9::d3d9::RenderTapePresentOutputRoleTransition::Demote) {
            prior->role = RenderTapeLiveObject::Role::Ordinary;
            prior->descriptor = std::move(priorDescriptor);
            prior->contentCount = priorContentCount;
            prior->content = std::move(priorContent);
            if (releaseAdmissionRef)
                (void)prior->lifetime.releaseWrapper();
            dxmt9DeviceInfoLog(
                "render_tape_capture present_output released transition=%s "
                "kind=%u generation=%u object_id=%llu",
                dxmt9::d3d9::renderTapePresentOutputRoleTransitionName(
                    transition),
                identity.kind, identity.generation,
                static_cast<unsigned long long>(identity.objectId));
            return;
        }
        try {
            renderTapeRegistry_->knownDead.push_back(identity);
        } catch (...) {
            markRenderTapeInvalidOnce("present_output_tombstone_allocation",
                                      &priorObject);
            return;
        }
        if (releaseAdmissionRef)
            (void)prior->lifetime.releaseWrapper();
        renderTapeRegistry_->objects.erase(
            renderTapeRegistry_->objects.begin() +
            (prior - renderTapeRegistry_->objects.data()));
        dxmt9DeviceInfoLog(
            "render_tape_capture present_output released transition=%s kind=%u "
            "generation=%u object_id=%llu",
            dxmt9::d3d9::renderTapePresentOutputRoleTransitionName(transition),
            identity.kind, identity.generation,
            static_cast<unsigned long long>(identity.objectId));
    }

    bool admitRenderTapePresentOutput() noexcept {
        if (!renderTapeRegistry_) {
            return false;
        }
        // Use the stable cached PE backbuffer wrapper. Calling the C getter
        // directly would allocate a fresh D9CSurface and therefore a second
        // generation-qualified identity, while command chunks use the
        // wrapper-owned raw surface returned by the swap-chain cache.
        IDirect3DSwapChain9 *swapchain = nullptr;
        if (FAILED(GetSwapChain(0u, &swapchain)) || !swapchain) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=present_output_swapchain_missing");
            return false;
        }
        IDirect3DSurface9 *backBuffer = nullptr;
        const HRESULT backBufferHr = swapchain->GetBackBuffer(
            0u, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        swapchain->Release();
        // These getters only observe the post-Present call-cadence counters;
        // neither path calls Present or arms capture. The swap-chain cache
        // keeps its own backbuffer-wrapper reference, so releasing this
        // temporary COM reference cannot release the identity used by the
        // command chunks or the PresentOutput role.
        if (FAILED(backBufferHr) || !backBuffer) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=present_output_surface_missing");
            return false;
        }
        auto *surface = D3D9PeRawSurface(backBuffer);
        D9CWireObjectIdentity identity{};
        D9CWireObjectIdentity rawIdentity{};
        D9CSurfaceDesc descriptor{};
        const auto &cachedWireObject = D3D9PeWireSurface(backBuffer);
        const bool rawIdentityOk =
            dxmt9c_surface_get_wire_identity(surface, &rawIdentity) >= 0;
        identity = rawIdentity;
        const bool identityOk = cachedWireObject.valid(
                                    D9C_CHUNK_HANDLE_KIND_SURFACE) &&
            rawIdentityOk &&
            dxmt9::d3d9::renderTapePresentOutputIdentityMatchesCommand(
                cachedWireObject.identity, rawIdentity);
        const bool descriptorOk = dxmt9c_surface_get_desc(surface, &descriptor) >= 0;
        if (!identityOk || !descriptorOk ||
            identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
            identity.generation == 0u || identity.objectId == 0u) {
            backBuffer->Release();
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=present_output_identity_or_descriptor_invalid");
            return false;
        }
        renderTapeOutputDesc_ = descriptor;
        const dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 outputDescriptor{
            .schemaVersion =
                dxmt9::d3d9::kRenderTapeSurfaceDescriptorVersion2,
            .storage = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeSurfaceStorage::SwapchainBackbuffer),
            .initialContentDisposition = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeInitialContentDisposition::
                    ProducedPresentOutput),
            .surface = descriptor,
        };
        const dxmt9::d3d9::pe::PeWireObjectRef object{
            .identity = identity,
            .object = surface,
        };
        // The role must leave its previous holder before this identity is
        // registered: a recycled wire object id would otherwise meet the stale
        // entry in the logical-slot replacement scan, and a standalone surface
        // is deliberately not an alias replacement candidate there.
        releaseRenderTapePresentOutputRole(&identity);
        // A retained role already stashed this holder's displaced content on an
        // earlier admission; re-stashing would capture the cleared state.
        const bool roleRetained = renderTapeRegistry_->presentOutputRole.held;
        auto *existing = findRenderTapeObject(object);
        const bool captureOwned = existing == nullptr;
        if (existing) {
            if (!roleRetained) {
                renderTapeRegistry_->presentOutputPriorDescriptor =
                    existing->descriptor;
                renderTapeRegistry_->presentOutputPriorContentCount =
                    existing->contentCount;
                renderTapeRegistry_->presentOutputPriorContent =
                    std::move(existing->content);
            }
            existing->role = RenderTapeLiveObject::Role::PresentOutput;
            existing->descriptor.assign(
                reinterpret_cast<const std::byte *>(&outputDescriptor),
                reinterpret_cast<const std::byte *>(&outputDescriptor + 1u));
            existing->contentCount = 0u;
            existing->content.clear();
        } else {
            renderTapeRegistry_->presentOutputPriorDescriptor.clear();
            renderTapeRegistry_->presentOutputPriorContentCount = 0u;
            renderTapeRegistry_->presentOutputPriorContent.clear();
            registerRenderTapeObject(
                object,
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(&outputDescriptor),
                    sizeof(outputDescriptor)),
                {}, RenderTapeLiveObject::Role::PresentOutput);
        }
        backBuffer->Release();
        const auto *admitted = findRenderTapeObject(object);
        if (!admitted || admitted->role != RenderTapeLiveObject::Role::PresentOutput) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=present_output_admission_failed "
                "kind=%u generation=%u object_id=%llu",
                identity.kind, identity.generation,
                static_cast<unsigned long long>(identity.objectId));
            return false;
        }
        auto &role = renderTapeRegistry_->presentOutputRole;
        role.identity = identity;
        role.captureOwned = roleRetained ? role.captureOwned : captureOwned;
        role.held = true;
        dxmt9DeviceInfoLog(
            "render_tape_capture present_output admitted kind=%u generation=%u "
            "object_id=%llu descriptor=%zu initial_content=not_required "
            "capture_owned=%d",
            identity.kind, identity.generation,
            static_cast<unsigned long long>(identity.objectId),
            sizeof(outputDescriptor),
            renderTapeRegistry_->presentOutputRole.captureOwned ? 1 : 0);
        return true;
    }

    bool unregisterRenderTapeObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
        if (!renderTapeRegistry_) {
            return false;
        }
        const auto it = std::find_if(
            renderTapeRegistry_->objects.begin(),
            renderTapeRegistry_->objects.end(),
            [&](const auto &entry) {
                return renderTapeSameIdentity(entry.identity, object.identity);
            });
        if (it == renderTapeRegistry_->objects.end()) {
            // Swap-chain-owned surfaces can be destroyed through the common
            // child path even though they were never admitted to the
            // value-owned capture registry. They are not part of the
            // checkpoint closure and must not poison pre-arm capture state.
            // Once the interval is active, however, an unknown destroy is a
            // closure violation and remains fail-closed.
            if (hasRenderTapeDeadObject(object)) {
                markRenderTapeInvalidOnce("object_destroy_duplicate", &object);
                if (IsRenderTapeCaptureActiveForChild()) {
                    abortRenderTapeCapture("object_destroy_duplicate");
                }
            }
            return false;
        }
        // The parent texture owns aliased storage, so release-to-zero is a
        // retained state rather than tape retirement. A later wrapper can
        // reacquire the same live identity before the parent retires it.
        if (!it->lifetime.releaseWrapper()) {
            return false;
        }
        // The caller performs the shared ObjectDestroy/tombstone/erase step.
        // Keeping the live entry here makes immediate and pending retirement
        // use the same exact-once ordering.
        return true;
    }

    bool recordRenderTapeCpuBytes(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, std::uint64_t byteOffset,
        std::span<const std::byte> bytes) noexcept {
        auto *entry = findRenderTapeObject(object);
        if (!entry || object.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
            subresource != 0u || entry->contentCount != 1u ||
            entry->content.size() != 1u || bytes.empty() ||
            entry->descriptor.size() != sizeof(D9CBufferDesc)) {
            markRenderTapeInvalidOnce("mutation_unknown_or_empty", &object,
                                      subresource, {.bytes = bytes.size()});
            return false;
        }
        D9CBufferDesc desc{};
        std::memcpy(&desc, entry->descriptor.data(), sizeof(desc));
        auto &existing = entry->content[subresource];
        const auto status = dxmt9::d3d9::applyRenderTapeBufferMutation(
            desc.size, byteOffset, bytes, existing);
        if (status == dxmt9::d3d9::RenderTapeBlockMutationStatus::Accepted)
            return true;
        if (status ==
            dxmt9::d3d9::RenderTapeBlockMutationStatus::IncompleteSeed) {
            // A partial write before a complete CPU-owned seed is not enough
            // to establish initial contents. Leave it unknown and fail at arm.
            return true;
        }
        markRenderTapeInvalidOnce(
            status == dxmt9::d3d9::RenderTapeBlockMutationStatus::AllocationFailed
                ? "mutation_copy_exception"
                : "mutation_extent",
            &object, subresource,
            {.format = desc.format, .bytes = bytes.size()});
        return false;
    }

    bool renderTapeObjectSubresourceDesc(
        const RenderTapeLiveObject &entry,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, D9CSurfaceDesc &out) const noexcept {
        out = {};
        if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
            return renderTapeTextureSubresourceDescriptor(
                entry.descriptor, subresource, out);
        }
        if (object.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
            subresource != 0u) {
            return false;
        }
        dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
        if (!dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(
                entry.descriptor, surface) ||
            surface.storage != static_cast<std::uint32_t>(
                                   dxmt9::d3d9::RenderTapeSurfaceStorage::
                                       Standalone)) {
            return false;
        }
        out = surface.surface;
        return true;
    }

    dxmt9::d3d9::RenderTapeBlockMutationStatus recordRenderTapeBlockBytes(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
        std::span<const std::byte> bytes) noexcept {
        auto *entry = findRenderTapeObject(object);
        if (!entry || subresource >= entry->content.size()) {
            return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
        }
        D9CSurfaceDesc desc{};
        if (!renderTapeObjectSubresourceDesc(*entry, object, subresource, desc) ||
            !renderTapeFormatIsBlockCompressed(desc.format)) {
            return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
        }
        dxmt9::d3d9::RenderTapeBlockLockLayout fullLayout{};
        if (dxmt9::d3d9::renderTapeBlockLockLayout(
                desc, static_cast<std::int32_t>(layout.pitch), nullptr,
                fullLayout) !=
                dxmt9::d3d9::RenderTapeBlockLayoutStatus::Accepted ||
            layout.blockBytes != fullLayout.blockBytes ||
            layout.fullRowBytes != fullLayout.fullRowBytes ||
            layout.fullRows != fullLayout.fullRows) {
            return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
        }
        return dxmt9::d3d9::applyRenderTapeBlockMutation(
            layout, bytes, entry->content[subresource]);
    }

    dxmt9::d3d9::RenderTapeBlockMutationStatus recordRenderTapeLinearBytes(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
        std::span<const std::byte> bytes) noexcept {
        auto *entry = findRenderTapeObject(object);
        if (!entry || subresource >= entry->content.size()) {
            return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
        }
        D9CSurfaceDesc desc{};
        if (!renderTapeObjectSubresourceDesc(*entry, object, subresource, desc) ||
            renderTapeFormatIsBlockCompressed(desc.format)) {
            return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
        }
        dxmt9::d3d9::RenderTapeLinearLockLayout fullLayout{};
        if (dxmt9::d3d9::renderTapeLinearLockLayout(
                desc, static_cast<std::int32_t>(layout.pitch), nullptr,
                fullLayout) !=
                dxmt9::d3d9::RenderTapeLinearLayoutStatus::Accepted ||
            layout.bytesPerPixel != fullLayout.bytesPerPixel ||
            layout.fullRowBytes != fullLayout.fullRowBytes ||
            layout.fullRows != fullLayout.fullRows) {
            return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
        }
        return dxmt9::d3d9::applyRenderTapeLinearMutation(
            layout, bytes, entry->content[subresource]);
    }

    void logRenderTapeMutationFailure(
        const char *route,
        dxmt9::d3d9::RenderTapeBlockMutationStatus status,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, std::uint32_t fullRowBytes,
        std::uint32_t fullRows, std::uint32_t rowBytes,
        std::uint32_t rows, std::uint32_t pitch,
        std::span<const std::byte> bytes) const noexcept {
        if (!renderTapeRegistry_ || renderTapeRegistry_->invalid)
            return;
        const auto *entry = findRenderTapeObject(object);
        D9CSurfaceDesc desc{};
        const bool descValid =
            entry && renderTapeObjectSubresourceDesc(
                         *entry, object, subresource, desc);
        dxmt9::d3d9::RenderTapeTextureDescriptorV2 texture{};
        const bool textureValid =
            entry && object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
            dxmt9::d3d9::renderTapeLoadTextureDescriptorV2(
                entry->descriptor, texture);
        const std::size_t contentBytes =
            entry && subresource < entry->content.size()
                ? entry->content[subresource].size()
                : 0u;
        dxmt9DeviceInfoLog(
            "render_tape_capture mutation_rejected route=%s status=%u "
            "kind=%u generation=%u object_id=%llu subresource=%u admitted=%d "
            "entry=%d descriptor_bytes=%zu desc_valid=%d format=%u width=%u "
            "height=%u depth=%u texture_v2=%d dimension=%u mips=%u "
            "subresources=%u disposition=%u content_slots=%zu content_bytes=%zu "
            "full_row_bytes=%u full_rows=%u row_bytes=%u rows=%u pitch=%u "
            "mutation_bytes=%zu",
            route, static_cast<unsigned>(status), object.identity.kind,
            object.identity.generation,
            static_cast<unsigned long long>(object.identity.objectId),
            subresource, renderTapeObjectAdmitted(object.identity) ? 1 : 0,
            entry ? 1 : 0, entry ? entry->descriptor.size() : 0u,
            descValid ? 1 : 0, desc.format, desc.width, desc.height, desc.depth,
            textureValid ? 1 : 0, texture.dimension, texture.mipLevelCount,
            texture.subresourceCount, texture.initialContentDisposition,
            entry ? entry->content.size() : 0u, contentBytes, fullRowBytes,
            fullRows, rowBytes, rows, pitch, bytes.size());
    }

    bool produceRenderTapeBootstrap(
        dxmt9::d3d9::RenderTapeCaptureBootstrapSeed &seed) noexcept {
        if (!renderTapeRegistry_) {
            dxmt9DeviceInfoLog("render_tape_capture producer aborted reason=registry_missing");
            return false;
        }
        if (renderTapeRegistry_->invalid) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=registry_invalid "
                "detail=%s kind=%u generation=%u object_id=%llu subresource=%u "
                "format=%u width=%u height=%u pitch=%d bytes=%llu objects=%zu",
                renderTapeRegistry_->invalidReason
                    ? renderTapeRegistry_->invalidReason
                    : "unknown",
                renderTapeRegistry_->invalidKind,
                renderTapeRegistry_->invalidGeneration,
                static_cast<unsigned long long>(renderTapeRegistry_->invalidObjectId),
                renderTapeRegistry_->invalidSubresource,
                renderTapeRegistry_->invalidLayout.format,
                renderTapeRegistry_->invalidLayout.width,
                renderTapeRegistry_->invalidLayout.height,
                renderTapeRegistry_->invalidLayout.pitch,
                static_cast<unsigned long long>(
                    renderTapeRegistry_->invalidLayout.bytes),
                renderTapeRegistry_->objects.size());
            return false;
        }
        if (!admitRenderTapePresentOutput() || renderTapeRegistry_->invalid) {
            return false;
        }
        const auto findArmSnapshot = [&](const auto &identity) {
            return std::find_if(
                renderTapeArmSnapshots_.begin(), renderTapeArmSnapshots_.end(),
                [&](const auto &snapshot) {
                    return renderTapeSameIdentity(snapshot.identity, identity);
                });
        };
        try {
            dxmt9::d3d9::pe::CommandChunkBuilder builder({
                .records = 1u,
                .handles = 256u,
                .payloadBytes = 1024u * 1024u,
                .sealedBytes = 1024u * 1024u,
            });
            // The bootstrap is a checkpoint of the live PE shadow, not of the
            // last draw packet. Rebuild every binding slot immediately before
            // producing it so streams, null unbinds, and the current index
            // binding are authoritative even when no draw made them pending.
            populateBindingView(peBindingView_, true, true);
            const bool snapshotBuilt = dxmt9::d3d9::pe::buildFullSnapshotState(
                peState_, peConsts_, peBindingView_, peSparseScratch_,
                peSparseHeader_, peSparseState_);
            const bool snapshotAppended =
                snapshotBuilt && dxmt9::d3d9::pe::appendApplyState(
                                     builder, peSparseHeader_.flags,
                                     peSparseState_);
            if (!snapshotBuilt || !snapshotAppended) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture producer aborted reason=bootstrap_state "
                    "snapshot_built=%d snapshot_appended=%d",
                    snapshotBuilt ? 1 : 0, snapshotAppended ? 1 : 0);
                return false;
            }
            const auto overlay = builder.seal();
            if (!overlay.valid()) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture producer aborted reason=overlay_invalid");
                return false;
            }

            // Keep the bootstrap closure tied to the exact generation-qualified
            // handles emitted by the sealed overlay. The canonical validator is
            // the authority for wire bounds, record/section shape, and handle
            // references; this side table only answers whether a live object's
            // missing seed is actually reachable from the checkpoint.
            dxmt9::d3d9::ImportedChunkView bootstrapChunk{};
            dxmt9::d3d9::CommandChunkValidationScratch bootstrapScratch{};
            const auto bootstrapValidation =
                dxmt9::d3d9::validateCommandChunk(
                    overlay.blob,
                    dxmt9::d3d9::CommandChunkEnvelope{
                        .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                        .recordCount = overlay.recordCount,
                        .handleCount = overlay.handleCount,
                    },
                    &bootstrapChunk, bootstrapScratch);
            if (!bootstrapValidation.valid()) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture producer aborted reason=overlay_validation "
                    "status=%u record=%u section=%u handle=%u offset=%u",
                    static_cast<unsigned>(bootstrapValidation.status),
                    bootstrapValidation.failedRecordIndex,
                    bootstrapValidation.failedSectionIndex,
                    bootstrapValidation.failedHandleIndex,
                    bootstrapValidation.byteOffset);
                return false;
            }
            std::vector<D9CWireObjectIdentity> bootstrapHandles;
            bootstrapHandles.reserve(bootstrapChunk.handles.size());
            for (const auto& handle : bootstrapChunk.handles) {
                const D9CWireObjectIdentity identity{
                    .kind = handle.kind,
                    .generation = handle.generation,
                    .objectId = handle.objectId,
                };
                const auto alreadyPresent = std::find_if(
                    bootstrapHandles.begin(), bootstrapHandles.end(),
                    [&](const auto& candidate) {
                        return renderTapeSameIdentity(candidate, identity);
                    });
                if (alreadyPresent == bootstrapHandles.end()) {
                    bootstrapHandles.push_back(identity);
                }
            }
            seed.bootstrapOverlay.assign(overlay.blob.begin(), overlay.blob.end());

            std::vector<const RenderTapeLiveObject *> objects;
            objects.reserve(renderTapeRegistry_->objects.size());
            for (const auto &object : renderTapeRegistry_->objects) {
                objects.push_back(&object);
            }
            std::sort(objects.begin(), objects.end(), [](const auto *a, const auto *b) {
                return std::tie(a->identity.kind, a->identity.generation,
                                a->identity.objectId) <
                       std::tie(b->identity.kind, b->identity.generation,
                                b->identity.objectId);
            });
            std::vector<dxmt9::d3d9::RenderTapeBootstrapClosureObject>
                closureObjects;
            closureObjects.reserve(objects.size());
            D9CWireObjectIdentity presentOutput{};
            std::size_t presentOutputCount = 0u;
            bool staleArmSnapshot = false;
            for (const auto *object : objects) {
                if (object->role == RenderTapeLiveObject::Role::PresentOutput) {
                    presentOutput = object->identity;
                    ++presentOutputCount;
                }
                const auto armSnapshot = findArmSnapshot(object->identity);
                const auto armOverlay = dxmt9::d3d9::
                    renderTapeSelectArmObjectSnapshotOverlay(
                        object->descriptor, object->content,
                        armSnapshot != renderTapeArmSnapshots_.end()
                            ? std::span<const std::byte>(
                                  armSnapshot->descriptor)
                            : std::span<const std::byte>{},
                        armSnapshot != renderTapeArmSnapshots_.end()
                            ? std::span<const std::vector<std::byte>>(
                                  armSnapshot->content)
                            : std::span<const std::vector<std::byte>>{},
                        armSnapshot != renderTapeArmSnapshots_.end()
                            ? armSnapshot->armOrdinal
                            : 0u,
                        renderTapeArmSnapshotOrdinal_,
                        object->role == RenderTapeLiveObject::Role::PresentOutput
                            ? dxmt9::d3d9::
                                  RenderTapeArmObjectSnapshotOverlayPolicy::
                                      PresentOutput
                            : dxmt9::d3d9::
                                  RenderTapeArmObjectSnapshotOverlayPolicy::
                                      Ordinary);
                staleArmSnapshot |= armOverlay.source == dxmt9::d3d9::
                    RenderTapeArmSnapshotOverlaySource::StaleArm;
                const auto overlayPolicy =
                    object->role == RenderTapeLiveObject::Role::PresentOutput
                        ? dxmt9::d3d9::
                              RenderTapeArmObjectSnapshotOverlayPolicy::
                                  PresentOutput
                        : dxmt9::d3d9::
                              RenderTapeArmObjectSnapshotOverlayPolicy::Ordinary;
                const bool complete = dxmt9::d3d9::
                    renderTapeArmObjectSnapshotContentComplete(
                        object->contentCount, object->lifetime.textureAlias,
                        overlayPolicy, armOverlay.source, armOverlay.content);
                bool producedByCapturedPassCandidate = false;
                if (!complete &&
                    object->identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
                    RenderTapeTextureDescriptorV2 texture{};
                    if (renderTapeLoadTextureDescriptorV2(object->descriptor,
                                                          texture) &&
                        texture.dimension == static_cast<std::uint32_t>(
                            RenderTapeTextureDimension::Texture2D) &&
                        texture.mipLevelCount == 1u &&
                        texture.subresourceCount == 1u) {
                        D9CSurfaceDesc desc{};
                        producedByCapturedPassCandidate =
                            renderTapeTextureSubresourceDescriptor(
                                object->descriptor, 0u, desc) &&
                            (desc.usage & 1u) != 0u;
                    }
                } else if (!complete &&
                           object->identity.kind ==
                               D9C_CHUNK_HANDLE_KIND_SURFACE) {
                    dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
                    producedByCapturedPassCandidate =
                        renderTapeLoadSurfaceDescriptorV2(object->descriptor,
                                                          surface) &&
                        surface.storage == static_cast<std::uint32_t>(
                            dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
                        dxmt9::d3d9::renderTapeProducedStandaloneSurfaceSupported(
                            surface.surface);
                }
                closureObjects.push_back({
                    .identity = object->identity,
                    .complete = complete,
                    .producedByCapturedPassCandidate =
                        producedByCapturedPassCandidate,
                    .hasDescriptorDependency = object->lifetime.textureAlias,
                    .descriptorDependency = object->aliasParentTexture,
                });
            }
            if (staleArmSnapshot) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture producer aborted reason=stale_arm_snapshot");
                return false;
            }
            if (presentOutputCount != 1u) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture producer aborted reason=present_output_count "
                    "count=%zu",
                    presentOutputCount);
                return false;
            }
            if (dxmt9::d3d9::renderTapeBootstrapRequiresAllLiveObjects(
                    dxmt9PeRenderTapeCaptureProfile())) {
                // Sequence tapes cannot define a pre-arm object after their
                // first PresentComplete. Preserve the complete arm snapshot
                // for that profile; exact JIT closure is a frame-tape policy.
                for (const auto *object : objects)
                    bootstrapHandles.push_back(object->identity);
            }
            std::vector<D9CWireObjectIdentity> closure;
            const auto closureResult =
                dxmt9::d3d9::renderTapeBuildBootstrapClosureAttributed(
                    bootstrapHandles, presentOutput,
                    closureObjects, closure);
            const auto closureStatus = closureResult.status;
            if (closureStatus !=
                dxmt9::d3d9::RenderTapeBootstrapClosureStatus::Accepted) {
                const char *reason =
                    closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::ReferencedObjectIncomplete
                        ? "bootstrap_referenced_incomplete_seed"
                        : closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::DescriptorDependencyIncomplete
                            ? "bootstrap_descriptor_dependency_incomplete_seed"
                            : closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::DescriptorDependencyMissing
                                ? "bootstrap_descriptor_dependency_missing"
                                : closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::DuplicateObjectIdentity
                                    ? "bootstrap_duplicate_object_identity"
                                    : closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::InvalidDescriptorDependency
                                        ? "bootstrap_invalid_descriptor_dependency"
                                        : "bootstrap_referenced_object_missing";
                const auto &offending = closureResult.offendingIdentity;
                const auto &dependency = closureResult.dependencyIdentity;
                const auto offendingObject = std::find_if(
                    objects.begin(), objects.end(), [&](const auto *candidate) {
                        return closureResult.hasOffendingIdentity &&
                            renderTapeSameIdentity(candidate->identity, offending);
                    });
                const auto missingSubresource =
                    offendingObject != objects.end()
                    ? static_cast<std::uint32_t>(std::find_if(
                          (*offendingObject)->content.begin(),
                          (*offendingObject)->content.end(),
                          [](const auto &bytes) { return bytes.empty(); }) -
                      (*offendingObject)->content.begin())
                    : 0u;
                const auto missing = offendingObject != objects.end()
                    ? dxmt9::d3d9::renderTapeDescribeMissingSeed(
                          offending, (*offendingObject)->descriptor,
                          missingSubresource,
                          {.handleIndex =
                               std::numeric_limits<std::uint32_t>::max(),
                           .recordIndex = 0u,
                           .recordType = D9C_COMMAND_RECORD_APPLY_STATE})
                    : dxmt9::d3d9::RenderTapeMissingSeedDescriptor{};
                dxmt9DeviceInfoLog(
                    "render_tape_capture producer aborted reason=%s "
                    "closure_status=%u offending_present=%d "
                    "offending_kind=%u offending_generation=%u "
                    "offending_object_id=%llu dependency_present=%d "
                    "dependency_kind=%u dependency_generation=%u "
                    "dependency_object_id=%llu bootstrap_handles=%zu "
                    "live_objects=%zu descriptor_status=%s expected_status=%s "
                    "missing_subresource=%u format=%u width=%u height=%u depth=%u "
                    "multisample_type=%u usage=%u resource_type=%u pool=%u "
                    "expected_tight_bytes=%llu expected_tight_bytes_valid=%d",
                    reason, static_cast<unsigned>(closureStatus),
                    closureResult.hasOffendingIdentity ? 1 : 0,
                    offending.kind, offending.generation,
                    static_cast<unsigned long long>(offending.objectId),
                    closureResult.hasDependencyIdentity ? 1 : 0,
                    dependency.kind, dependency.generation,
                    static_cast<unsigned long long>(dependency.objectId),
                    bootstrapHandles.size(), objects.size(),
                    dxmt9::d3d9::renderTapeMissingSeedDescriptorStatusName(
                        missing.descriptorStatus),
                    dxmt9::d3d9::renderTapeExpectedContentStatusName(
                        missing.expectedContentStatus),
                    missing.missingSubresource, missing.missingSurface.format,
                    missing.missingSurface.width, missing.missingSurface.height,
                    missing.missingSurface.depth,
                    missing.missingSurface.multiSampleType,
                    missing.missingSurface.usage,
                    missing.missingSurface.resourceType,
                    missing.missingSurface.pool,
                    static_cast<unsigned long long>(missing.expectedTightBytes),
                    missing.expectedTightBytesValid ? 1 : 0);
                return false;
            }
            for (const auto *object : objects) {
                if (!dxmt9::d3d9::renderTapeBootstrapClosureContains(
                        closure, object->identity)) {
                    continue;
                }
                const auto closureObject = std::find_if(
                    closureObjects.begin(), closureObjects.end(),
                    [&](const auto &candidate) {
                        return renderTapeSameIdentity(candidate.identity,
                                                      object->identity);
                    });
                const auto dependencyObject =
                    closureObject != closureObjects.end() &&
                            closureObject->hasDescriptorDependency
                        ? std::find_if(
                              closureObjects.begin(), closureObjects.end(),
                              [&](const auto &candidate) {
                                return renderTapeSameIdentity(
                                    candidate.identity,
                                    closureObject->descriptorDependency);
                              })
                        : closureObjects.end();
                if (closureObject != closureObjects.end() &&
                    ((!closureObject->complete &&
                      closureObject->producedByCapturedPassCandidate) ||
                     (dependencyObject != closureObjects.end() &&
                      !dependencyObject->complete &&
                      dependencyObject->producedByCapturedPassCandidate))) {
                    // The generation-qualified storage is defined only after
                    // the current command chunk proves its first terminal
                    // access is the matching unrestricted attachment Clear.
                    continue;
                }
                dxmt9::d3d9::RenderTapeCaptureObjectSeed objectSeed{};
                objectSeed.identity = object->identity;
                objectSeed.descriptorKind = static_cast<std::uint32_t>(
                    dxmt9::d3d9::renderTapeDescriptorKindForObject(
                        object->identity.kind));
                const auto armSnapshot = findArmSnapshot(object->identity);
                const auto armOverlay = dxmt9::d3d9::
                    renderTapeSelectArmObjectSnapshotOverlay(
                        object->descriptor, object->content,
                        armSnapshot != renderTapeArmSnapshots_.end()
                            ? std::span<const std::byte>(
                                  armSnapshot->descriptor)
                            : std::span<const std::byte>{},
                        armSnapshot != renderTapeArmSnapshots_.end()
                            ? std::span<const std::vector<std::byte>>(
                                  armSnapshot->content)
                            : std::span<const std::vector<std::byte>>{},
                        armSnapshot != renderTapeArmSnapshots_.end()
                            ? armSnapshot->armOrdinal
                            : 0u,
                        renderTapeArmSnapshotOrdinal_,
                        object->role == RenderTapeLiveObject::Role::PresentOutput
                            ? dxmt9::d3d9::
                                  RenderTapeArmObjectSnapshotOverlayPolicy::
                                      PresentOutput
                            : dxmt9::d3d9::
                                  RenderTapeArmObjectSnapshotOverlayPolicy::
                                      Ordinary);
                if (armOverlay.source == dxmt9::d3d9::
                        RenderTapeArmSnapshotOverlaySource::StaleArm) {
                    return false;
                }
                const auto effectiveDescriptor = armOverlay.descriptor;
                const auto effectiveContent = armOverlay.content;
                objectSeed.descriptor.assign(effectiveDescriptor.begin(),
                                             effectiveDescriptor.end());
                dxmt9::d3d9::RenderTapeExpectedContentContract contentContract{};
                if (!renderTapeValidateExpectedContent(
                        object->identity, effectiveDescriptor,
                        effectiveContent,
                        contentContract)) {
                    dxmt9DeviceInfoLog(
                        "render_tape_capture producer aborted reason=expected_content_contract "
                        "status=%s kind=%u generation=%u object_id=%llu expected_bytes=%llu "
                        "expected_count=%u actual_count=%zu",
                        dxmt9::d3d9::renderTapeExpectedContentStatusName(
                            contentContract.status),
                        object->identity.kind, object->identity.generation,
                        static_cast<unsigned long long>(object->identity.objectId),
                        static_cast<unsigned long long>(contentContract.bytes),
                        contentContract.count, effectiveContent.size());
                    RejectRenderTapeCaptureForChild(
                        dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                            ExpectedContentContract,
                        dxmt9::d3d9::pe::PeWireObjectRef{
                            .identity = object->identity},
                        std::numeric_limits<std::uint32_t>::max(), {});
                    return false;
                }
                objectSeed.expectedContentBytes = contentContract.bytes;
                objectSeed.expectedContentCount = contentContract.count;
                if (!object->immutablePayload.empty()) {
                    objectSeed.immutableBytes = object->immutablePayload.size();
                    objectSeed.immutableDigest =
                        dxmt9::d3d9::RenderTapeCaptureSession::sha256(
                            object->immutablePayload);
                    seed.blobs.push_back({.bytes = object->immutablePayload});
                }
                if (!effectiveContent.empty()) {
                    for (std::uint32_t subresource = 0u;
                         subresource < effectiveContent.size(); ++subresource) {
                        const auto &bytes = effectiveContent[subresource];
                        if (bytes.empty()) {
                            if (object->identity.kind ==
                                D9C_CHUNK_HANDLE_KIND_TEXTURE) {
                                RenderTapeTextureDescriptorV2 texture{};
                                std::uint32_t textureVersion = 0u;
                                std::uint32_t levels = 0u;
                                std::uint32_t count = 0u;
                                if (object->descriptor.size() >=
                                    sizeof(texture)) {
                                    std::memcpy(&texture, object->descriptor.data(),
                                                sizeof(texture));
                                    if (texture.schemaVersion ==
                                        dxmt9::d3d9::
                                            kRenderTapeTextureDescriptorVersion2) {
                                        textureVersion = texture.schemaVersion;
                                        levels = texture.mipLevelCount;
                                        count = texture.subresourceCount;
                                    }
                                }
                                D9CSurfaceDesc desc{};
                                const bool hasDesc =
                                    renderTapeTextureSubresourceDescriptor(
                                        object->descriptor, subresource, desc);
                                dxmt9DeviceInfoLog(
                                    "render_tape_capture missing_seed identity_kind=%u "
                                    "generation=%u object_id=%llu subresource=%u "
                                    "bootstrap_referenced=%d texture_version=%u "
                                    "levels=%u count=%u desc_valid=%d format=%u "
                                    "width=%u height=%u depth=%u usage=%u pool=%u "
                                    "resource_type=%u",
                                    object->identity.kind,
                                    object->identity.generation,
                                    static_cast<unsigned long long>(
                                        object->identity.objectId),
                                    subresource, 1,
                                    textureVersion, levels, count, hasDesc ? 1 : 0,
                                    desc.format, desc.width, desc.height, desc.depth,
                                    desc.usage, desc.pool, desc.resourceType);
                            } else if (object->identity.kind ==
                                       D9C_CHUNK_HANDLE_KIND_SURFACE) {
                                D9CSurfaceDesc desc{};
                                dxmt9::d3d9::RenderTapeSurfaceDescriptorV2
                                    surface{};
                                const bool hasDesc = dxmt9::d3d9::
                                    renderTapeLoadSurfaceDescriptorV2(
                                        object->descriptor, surface);
                                if (hasDesc)
                                    desc = surface.surface;
                                dxmt9DeviceInfoLog(
                                    "render_tape_capture missing_seed identity_kind=%u "
                                    "generation=%u object_id=%llu subresource=%u "
                                    "bootstrap_referenced=%d surface_desc_valid=%d "
                                    "format=%u width=%u height=%u depth=%u usage=%u "
                                    "pool=%u resource_type=%u",
                                    object->identity.kind,
                                    object->identity.generation,
                                    static_cast<unsigned long long>(
                                        object->identity.objectId),
                                    subresource, 1,
                                    hasDesc ? 1 : 0, desc.format, desc.width,
                                    desc.height, desc.depth, desc.usage, desc.pool,
                                    desc.resourceType);
                            } else if (object->identity.kind ==
                                       D9C_CHUNK_HANDLE_KIND_BUFFER) {
                                D9CBufferDesc desc{};
                                const bool hasDesc =
                                    object->descriptor.size() == sizeof(desc);
                                if (hasDesc) {
                                    std::memcpy(&desc, object->descriptor.data(),
                                                sizeof(desc));
                                }
                                dxmt9DeviceInfoLog(
                                    "render_tape_capture missing_seed identity_kind=%u "
                                    "generation=%u object_id=%llu subresource=%u "
                                    "bootstrap_referenced=%d buffer_desc_valid=%d "
                                    "size=%u usage=%u pool=%u format=%u fvf=%u",
                                    object->identity.kind,
                                    object->identity.generation,
                                    static_cast<unsigned long long>(
                                        object->identity.objectId),
                                    subresource, 1,
                                    hasDesc ? 1 : 0, desc.size, desc.usage,
                                    desc.pool, desc.format, desc.fvf);
                            }
                            D9CSurfaceDesc rejectionDesc{};
                            const dxmt9::d3d9::pe::PeWireObjectRef reference{
                                .identity = object->identity,
                            };
                            (void)renderTapeObjectSubresourceDesc(
                                *object, reference, subresource, rejectionDesc);
                            RejectRenderTapeCaptureForChild(
                                dxmt9::d3d9::
                                    RenderTapeCaptureRejectionReason::
                                        IncompleteSubresourceSeed,
                                reference, subresource,
                                {.format = rejectionDesc.format,
                                 .width = rejectionDesc.width,
                                 .height = rejectionDesc.height,
                                 .pitch = 0,
                                 .bytes = 0u});
                            return false;
                        }
                        const auto digest =
                            dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes);
                        seed.blobs.push_back({.bytes = bytes});
                        seed.mutations.push_back({
                            .identity = object->identity,
                            .kind = dxmt9::d3d9::RenderTapeMutationKind::Upload,
                            .subresource = subresource,
                            .byteOffset = 0u,
                            .byteSize = bytes.size(),
                            .digest = digest,
                        });
                    }
                }
                seed.objects.push_back(std::move(objectSeed));
                if (object->role == RenderTapeLiveObject::Role::PresentOutput) {
                    seed.oracleAttachments.push_back({
                        .identity = object->identity,
                        .descriptorKind = static_cast<std::uint32_t>(
                            dxmt9::d3d9::RenderTapeDescriptorKind::Surface),
                    });
                }
            }
            if (seed.oracleAttachments.size() != 1u) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture producer aborted reason=present_output_oracle_count "
                    "count=%zu",
                    seed.oracleAttachments.size());
                return false;
            }
            return true;
        } catch (...) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=exception");
            return false;
        }
    }

    bool advanceRenderTapeArmBoundary(
        dxmt9::d3d9::RenderTapeArmBoundaryPhase requested) noexcept {
        const auto transition = dxmt9::d3d9::renderTapeAdvanceArmBoundary(
            renderTapeArmBoundaryPhase_, requested);
        if (!transition.accepted) {
            dxmt9DeviceInfoLog(
                "render_tape_capture arm_boundary rejected current=%u requested=%u",
                static_cast<unsigned>(renderTapeArmBoundaryPhase_),
                static_cast<unsigned>(requested));
            return false;
        }
        renderTapeArmBoundaryPhase_ = transition.next;
        return true;
    }

    bool snapshotRenderTapeResourcesAtArmBoundary() noexcept {
        if (!renderTapeRegistry_ || renderTapeRegistry_->invalid)
            return false;
        try {
            renderTapeArmSnapshots_.clear();
            const auto epoch = dxmt9::d3d9::renderTapeNextArmSnapshotEpoch(
                renderTapeArmSnapshotOrdinal_);
            if (!epoch.valid) {
                return false;
            }
            renderTapeArmSnapshotOrdinal_ = epoch.ordinal;
            for (std::size_t index = 0u;
                 index < renderTapeRegistry_->objects.size(); ++index) {
                const auto &object = renderTapeRegistry_->objects[index];
                if (object.lifetime.textureAlias) {
                    continue;
                }
                dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
                const bool standaloneD24 =
                    object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
                    object.contentCount == 1u && object.content.size() == 1u &&
                    dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(
                        object.descriptor, surface) &&
                    surface.storage == static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
                    dxmt9::d3d9::renderTapeSnapshotStandaloneD24X8Supported(
                        surface.surface);
                const bool colorTexture =
                    object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
                    dxmt9::d3d9::renderTapeArmColorSnapshotTextureSupported(
                        object.descriptor);
                const bool standaloneColor =
                    object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
                    object.contentCount == 1u && object.content.size() == 1u &&
                    surface.storage == static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
                    dxmt9::d3d9::
                        renderTapeArmColorSnapshotStandaloneSurfaceSupported(
                            surface.surface);
                const bool presentOutputColor =
                    object.role == RenderTapeLiveObject::Role::PresentOutput &&
                    object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
                    object.contentCount == 0u &&
                    dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(
                        object.descriptor, surface) &&
                    surface.storage == static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeSurfaceStorage::SwapchainBackbuffer) &&
                    dxmt9::d3d9::renderTapeArmColorSnapshotSwapchainSurfaceSupported(
                        surface.surface);
                if (!standaloneD24 && !standaloneColor && !colorTexture &&
                    !presentOutputColor)
                    continue;

                RenderTapeArmObjectSnapshot snapshot{
                    .objectIndex = index,
                    .armOrdinal = epoch.ordinal,
                    .identity = object.identity,
                    .descriptor = object.descriptor,
                    .content = std::vector<std::vector<std::byte>>(
                        standaloneD24 || standaloneColor || presentOutputColor
                            ? 1u
                            : object.contentCount),
                };
                if (standaloneD24) {
                    if (surface.surface.width >
                            std::numeric_limits<std::uint32_t>::max() / 4u ||
                        surface.surface.height >
                            std::numeric_limits<std::uint64_t>::max() /
                                (surface.surface.width * 4u)) {
                        return false;
                    }
                    const std::uint64_t byteCount =
                        static_cast<std::uint64_t>(surface.surface.width) *
                        surface.surface.height * 4u;
                    if (byteCount == 0u ||
                        byteCount > std::numeric_limits<std::size_t>::max()) {
                        return false;
                    }
                    snapshot.content[0].resize(
                        static_cast<std::size_t>(byteCount));
                    const D9CRenderTapeD24X8SnapshotRequest request{
                        .identity = object.identity,
                        .surface = surface.surface,
                        .encodingVersion =
                            D9C_RENDER_TAPE_D24X8_ENCODING_FLOAT32_LE_V1,
                        .reserved0 = 0u,
                    };
                    D9CRenderTapeD24X8SnapshotResult result{};
                    const HRESULT hr = hr32(
                        dxmt9c_device_capture_render_tape_d24x8_snapshot(
                            dev_, &request, &result,
                            snapshot.content[0].data(),
                            snapshot.content[0].size()));
                    const std::uint32_t expectedPitch =
                        surface.surface.width * 4u;
                    if (FAILED(hr) || result.status !=
                            D9C_RENDER_TAPE_D24X8_SNAPSHOT_COMPLETE ||
                        result.encodingVersion !=
                            D9C_RENDER_TAPE_D24X8_ENCODING_FLOAT32_LE_V1 ||
                        result.width != surface.surface.width ||
                        result.height != surface.surface.height ||
                        result.pitch != expectedPitch ||
                        result.byteCount != snapshot.content[0].size() ||
                        result.physicalFormat == 0u) {
                        dxmt9DeviceInfoLog(
                            "render_tape_capture d24x8_snapshot rejected "
                            "reason=provider_result hr=0x%08x status=%u "
                            "generation=%u object_id=%llu",
                            static_cast<unsigned>(hr), result.status,
                            object.identity.generation,
                            static_cast<unsigned long long>(
                                object.identity.objectId));
                        return false;
                    }
                    surface.initialContentDisposition =
                        static_cast<std::uint32_t>(dxmt9::d3d9::
                            RenderTapeInitialContentDisposition::
                                CompleteDepthFloat32V1);
                    snapshot.descriptor.assign(
                        std::as_bytes(std::span(&surface, 1u)).begin(),
                        std::as_bytes(std::span(&surface, 1u)).end());
                    dxmt9DeviceInfoLog(
                        "render_tape_capture d24x8_snapshot complete kind=%u "
                        "generation=%u object_id=%llu encoding=1 width=%u "
                        "height=%u pitch=%u bytes=%zu physical_format=%u",
                        snapshot.identity.kind, snapshot.identity.generation,
                        static_cast<unsigned long long>(snapshot.identity.objectId),
                        surface.surface.width, surface.surface.height,
                        result.pitch, snapshot.content[0].size(),
                        result.physicalFormat);
                } else {
                    RenderTapeTextureDescriptorV2 texture{};
                    std::uint32_t subresourceCount = 1u;
                    if (standaloneColor || presentOutputColor) {
                        surface.initialContentDisposition =
                            static_cast<std::uint32_t>(dxmt9::d3d9::
                                RenderTapeInitialContentDisposition::
                                    CompleteSeed);
                        snapshot.descriptor.assign(
                            std::as_bytes(std::span(&surface, 1u)).begin(),
                            std::as_bytes(std::span(&surface, 1u)).end());
                    } else {
                        if (!renderTapeLoadTextureDescriptorV2(
                                snapshot.descriptor, texture) ||
                            texture.subresourceCount !=
                                snapshot.content.size()) {
                            return false;
                        }
                        subresourceCount = texture.subresourceCount;
                    }
                    for (std::uint32_t subresource = 0u;
                         subresource < subresourceCount;
                         ++subresource) {
                        D9CSurfaceDesc desc = surface.surface;
                        if (!standaloneColor && !presentOutputColor &&
                            !renderTapeTextureSubresourceDescriptor(
                                snapshot.descriptor, subresource, desc)) {
                            return false;
                        }
                        const auto expected = dxmt9::d3d9::
                            renderTapeDeriveExpectedSurfaceContent(desc);
                        if (expected.status != dxmt9::d3d9::
                                RenderTapeExpectedContentStatus::Accepted ||
                            expected.bytes == 0u ||
                            expected.bytes >
                                std::numeric_limits<std::size_t>::max()) {
                            return false;
                        }
                        snapshot.content[subresource].resize(
                            static_cast<std::size_t>(expected.bytes));
                        const D9CRenderTapeColorSnapshotRequest request{
                            .identity = object.identity,
                            .surface = desc,
                            .subresource = subresource,
                            .encodingVersion =
                                D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1,
                        };
                        D9CRenderTapeColorSnapshotResult result{};
                        const HRESULT hr = hr32(
                            dxmt9c_device_capture_render_tape_color_snapshot(
                                dev_, &request, &result,
                                snapshot.content[subresource].data(),
                                snapshot.content[subresource].size()));
                        const std::uint32_t expectedPitch = desc.width * 4u;
                        if (FAILED(hr) || result.status !=
                                D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE ||
                            result.encodingVersion !=
                                D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1 ||
                            result.subresource != subresource ||
                            result.width != desc.width ||
                            result.height != desc.height ||
                            result.pitch != expectedPitch ||
                            result.format != desc.format ||
                            result.reserved0 != 0u ||
                            result.byteCount !=
                                snapshot.content[subresource].size()) {
                            dxmt9DeviceInfoLog(
                                "render_tape_capture color_snapshot rejected "
                                "reason=provider_result hr=0x%08x status=%u "
                                "generation=%u object_id=%llu subresource=%u",
                                static_cast<unsigned>(hr), result.status,
                                object.identity.generation,
                                static_cast<unsigned long long>(
                                    object.identity.objectId),
                                subresource);
                            return false;
                        }
                        dxmt9DeviceInfoLog(
                            "render_tape_capture color_snapshot complete kind=%u "
                            "generation=%u object_id=%llu subresource=%u "
                            "encoding=1 format=%u width=%u height=%u pitch=%u "
                            "bytes=%zu",
                            snapshot.identity.kind,
                            snapshot.identity.generation,
                            static_cast<unsigned long long>(
                                snapshot.identity.objectId),
                            subresource, desc.format, desc.width, desc.height,
                            result.pitch,
                            snapshot.content[subresource].size());
                    }
                }
                renderTapeArmSnapshots_.push_back(std::move(snapshot));
            }
            return advanceRenderTapeArmBoundary(dxmt9::d3d9::
                RenderTapeArmBoundaryPhase::SnapshotComplete);
        } catch (...) {
            dxmt9DeviceInfoLog(
                "render_tape_capture arm_snapshot rejected reason=exception");
            return false;
        }
    }

    // An arm attempt that does not reach an active interval must hand the
    // PresentOutput role back immediately. Deferring it to the next attempt
    // leaves a stale live entry across the window in which the C-side wire
    // registry can recycle its object id at a newer generation.
    bool armRenderTapeCaptureAtPresentBoundary() {
        if (armRenderTapeCaptureAtPresentBoundaryInterval()) {
            return true;
        }
        releaseRenderTapePresentOutputRole(nullptr);
        return false;
    }

    bool armRenderTapeCaptureAtPresentBoundaryInterval() {
        if (!renderTapeCapture_ ||
            !renderTapeCapture_->enabled() ||
            (renderTapeCapture_->state() !=
                 dxmt9::d3d9::RenderTapeCaptureState::Disabled &&
             renderTapeCapture_->state() !=
                 dxmt9::d3d9::RenderTapeCaptureState::Aborted)) {
            return false;
        }
        if (renderTapeArmPresentSkipRemaining_ != 0u) {
            --renderTapeArmPresentSkipRemaining_;
            return false;
        }
        // An interval that aborted after arming still holds the role; release
        // it here so a retry starts from exactly one live present output.
        releaseRenderTapePresentOutputRole(nullptr);
        // Keep the first-abort marker sticky only for this arm/interval
        // lifecycle; a retry must get independent attribution.
        renderTapeAbortReason_ = nullptr;
        renderTapeAdmittedIdentities_.clear();
        renderTapeExpectedDigest_.reset();
        renderTapeExpectedPixels_.clear();
        renderTapeExpectedSourcePixels_.clear();
        renderTapeOutputDesc_.reset();
        renderTapeFirstAccessLedger_ = {};
        const auto producer = dxmt9PeRenderTapeBootstrapProducer.load(
            std::memory_order_acquire);
        auto publisher = dxmt9PeRenderTapeArtifactPublisher.load(
            std::memory_order_acquire);
        if (!publisher) {
            publisher = dxmt9PeDefaultRenderTapeArtifactPublisher();
        }
        dxmt9DeviceInfoLog(
            "render_tape_capture arm enabled=1 producer=%d publisher=%d",
            producer != nullptr ? 1 : 0, publisher != nullptr ? 1 : 0);
        if (!dxmt9PeRenderTapeCaptureCallbacksInstalled(
                renderTapeCapture_->enabled(), producer, publisher)) {
            dxmt9DeviceInfoLog(
                "render_tape_capture requested without artifact publisher; "
                "capture remains off");
            return false;
        }
        // Both callers reach this point only after the Present bridge call or
        // Present-record chunk commit returned success.
        renderTapeArmBoundaryPhase_ =
            dxmt9::d3d9::RenderTapeArmBoundaryPhase::Disabled;
        if (!advanceRenderTapeArmBoundary(dxmt9::d3d9::
                RenderTapeArmBoundaryPhase::PresentFlushed)) {
            return false;
        }
        // Admit the just-presented swap-chain backbuffer before taking the arm
        // snapshot. The backbuffer is the only capture identity whose role is
        // assigned lazily by the bootstrap producer; without this ordering its
        // actual post-arm bytes cannot be captured as starting content.
        if (!admitRenderTapePresentOutput() || renderTapeRegistry_->invalid) {
            dxmt9DeviceInfoLog(
                "render_tape_capture arm aborted reason=present_output_admission");
            abortRenderTapeCapture("present_output_admission");
            return false;
        }
        if (!snapshotRenderTapeResourcesAtArmBoundary()) {
            dxmt9DeviceInfoLog(
                "render_tape_capture arm aborted reason=arm_snapshot");
            abortRenderTapeCapture("arm_snapshot");
            return false;
        }
        dxmt9::d3d9::RenderTapeCaptureBootstrapSeed seed{};
        bool produced = false;
        try {
            // A non-null injected producer is an explicit test override. The
            // production path always snapshots this device's value-owned PE
            // shadow and live-object store at the arm Present boundary.
            produced = producer ? producer(seed)
                                : produceRenderTapeBootstrap(seed);
            // Gamma is PE-owned persistent state and therefore part of the
            // bootstrap checkpoint, not an implicit host default. Keep the
            // bytes in the seed so injected producers can override the
            // complete snapshot in native tests.
            if (produced && seed.gammaRamp.empty()) {
                seed.gammaRamp.resize(dxmt9::d3d9::kRenderTapeGammaRampBytes);
                std::memcpy(seed.gammaRamp.data(), &gammaRamp_,
                            seed.gammaRamp.size());
            }
        } catch (...) {
            produced = false;
        }
        auto armStatus = dxmt9::d3d9::RenderTapeCaptureStatus::InvalidInput;
        auto intervalStatus = dxmt9::d3d9::RenderTapeCaptureStatus::InvalidState;
        if (produced) {
            armStatus = renderTapeCapture_->armWithBlobs(
                seed.bootstrapOverlay, seed.blobs, seed.gammaRamp);
            if (armStatus == dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                intervalStatus = renderTapeCapture_->beginPresentInterval();
            }
        }
        if (!produced ||
            armStatus != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted ||
            intervalStatus != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            dxmt9DeviceInfoLog(
                "render_tape_capture arm aborted produced=%d arm_status=%u "
                "interval_status=%u objects=%zu mutations=%zu blobs=%zu",
                produced ? 1 : 0, static_cast<unsigned>(armStatus),
                static_cast<unsigned>(intervalStatus), seed.objects.size(),
                seed.mutations.size(), seed.blobs.size());
            abortRenderTapeCapture("arm_validation");
            return false;
        }
        for (const auto& object : seed.objects) {
            dxmt9::d3d9::RenderTapeObjectDefineDisposition disposition{};
            const auto status = renderTapeCapture_->objectDefine(
                    object.identity, object.descriptorKind, object.descriptor,
                    object.immutableBytes, object.immutableDigest,
                    object.expectedContentBytes,
                    object.expectedContentCount, &disposition);
            if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture seed_object_define status=%u reason=%s kind=%u "
                    "generation=%u object_id=%llu descriptor=%zu immutable=%llu "
                    "expected_bytes=%llu expected_count=%u",
                    static_cast<unsigned>(status),
                    dxmt9::d3d9::renderTapeObjectDefineDispositionName(
                        disposition),
                    object.identity.kind,
                    object.identity.generation,
                    static_cast<unsigned long long>(object.identity.objectId),
                    object.descriptor.size(),
                    static_cast<unsigned long long>(object.immutableBytes),
                    static_cast<unsigned long long>(object.expectedContentBytes),
                    object.expectedContentCount);
                abortRenderTapeCapture("seed_object_define");
                return false;
            }
            try {
                renderTapeAdmittedIdentities_.push_back(object.identity);
            } catch (...) {
                abortRenderTapeCapture("seed_identity_allocation");
                return false;
            }
        }
        for (const auto& mutation : seed.mutations) {
            const auto status = renderTapeCapture_->resourceMutation(
                mutation.identity, mutation.kind, mutation.subresource,
                mutation.byteOffset, mutation.byteSize, mutation.digest,
                mutation.bufferDisposition);
            if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture seed_resource_mutation status=%u kind=%u "
                    "generation=%u object_id=%llu subresource=%u bytes=%llu",
                    static_cast<unsigned>(status), mutation.identity.kind,
                    mutation.identity.generation,
                    static_cast<unsigned long long>(mutation.identity.objectId),
                    mutation.subresource,
                    static_cast<unsigned long long>(mutation.byteSize));
                abortRenderTapeCapture("seed_resource_mutation");
                return false;
            }
        }
        renderTapeCaptureOracle_ = std::move(seed.oracleAttachments);
        if (!advanceRenderTapeArmBoundary(dxmt9::d3d9::
                RenderTapeArmBoundaryPhase::Armed)) {
            abortRenderTapeCapture("arm_boundary_order");
            return false;
        }
        if (renderTapeNextCaptureToken_ ==
            std::numeric_limits<std::uint64_t>::max()) {
            renderTapeNextCaptureToken_ = 1u;
        } else {
            ++renderTapeNextCaptureToken_;
        }
        renderTapeActiveCaptureToken_ = renderTapeNextCaptureToken_;
        return true;
    }

    // Bounded capture-rejection attribution shared by the CpuUnlock append
    // branches. Every field is already-owned session state; nothing here
    // relaxes a predicate or raises a capacity.
    void logRenderTapeMutationRejection(
        const char *reason, const char *detail,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, std::uint64_t bytes,
        dxmt9::d3d9::RenderTapeCaptureStatus status) noexcept {
        const auto &limits = renderTapeCapture_->limits();
        dxmt9DeviceInfoLog(
            "render_tape_capture mutation_reject reason=%s detail=%s status=%u "
            "capture_state=%u kind=%u generation=%u object_id=%llu "
            "subresource=%u bytes=%llu live_object=%d event_count=%u/%u "
            "buffered_bytes=%llu/%llu owned_blob_bytes=%llu/%llu "
            "owned_blob_entries=%u/%u",
            reason, detail, static_cast<unsigned>(status),
            static_cast<unsigned>(renderTapeCapture_->state()),
            object.identity.kind, object.identity.generation,
            static_cast<unsigned long long>(object.identity.objectId),
            subresource, static_cast<unsigned long long>(bytes),
            renderTapeCapture_->hasLiveObject(object.identity) ? 1 : 0,
            renderTapeCapture_->eventCount(), limits.maxEvents,
            static_cast<unsigned long long>(renderTapeCapture_->bufferedBytes()),
            static_cast<unsigned long long>(limits.maxEventBytes),
            static_cast<unsigned long long>(
                renderTapeCapture_->ownedBlobBytes()),
            static_cast<unsigned long long>(limits.maxBlobBytes),
            renderTapeCapture_->ownedBlobEntries(), limits.maxBlobEntries);
    }

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
        std::uint32_t subresource, const char *reason) noexcept {
        const auto *entry = findRenderTapeObject(object);
        if (!entry || subresource >= entry->content.size()) {
            dxmt9DeviceInfoLog(
                "render_tape_capture mutation_reject reason=%s detail=%s kind=%u "
                "generation=%u object_id=%llu subresource=%u content=%zu",
                reason,
                entry ? "subresource_out_of_range" : "registry_entry_missing",
                object.identity.kind, object.identity.generation,
                static_cast<unsigned long long>(object.identity.objectId),
                subresource, entry ? entry->content.size() : 0u);
            return false;
        }
        const auto &bytes = entry->content[subresource];
        dxmt9::d3d9::RenderTapeDigest digest{};
        const auto blobStatus =
            renderTapeCapture_->registerBlobBytes(bytes, &digest);
        if (blobStatus != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            logRenderTapeMutationRejection(reason, "blob_register", object,
                                           subresource, bytes.size(),
                                           blobStatus);
            return false;
        }
        const auto status = renderTapeCapture_->resourceMutation(
            object.identity, dxmt9::d3d9::RenderTapeMutationKind::CpuUnlock,
            subresource, 0u, bytes.size(),
            std::span<const std::byte, dxmt9::d3d9::kRenderTapeDigestSize>(
                digest));
        if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            logRenderTapeMutationRejection(reason, "mutation_event", object,
                                           subresource, bytes.size(), status);
            return false;
        }
        return true;
    }

    bool renderTapeObjectAdmitted(
        const D9CWireObjectIdentity &identity) const noexcept {
        return dxmt9::d3d9::renderTapeBootstrapClosureContains(
            renderTapeAdmittedIdentities_, identity);
    }

    void removeRenderTapeObjectAdmitted(
        const D9CWireObjectIdentity &identity) noexcept {
        const auto it = std::find_if(
            renderTapeAdmittedIdentities_.begin(),
            renderTapeAdmittedIdentities_.end(), [&](const auto &candidate) {
                return renderTapeSameIdentity(candidate, identity);
            });
        if (it != renderTapeAdmittedIdentities_.end())
            renderTapeAdmittedIdentities_.erase(it);
    }

    bool materializeRenderTapeObjectForReference(
        const D9CWireObjectIdentity &identity,
        std::uint32_t handleIndex =
            std::numeric_limits<std::uint32_t>::max(),
        std::uint32_t recordIndex =
            std::numeric_limits<std::uint32_t>::max(),
        std::uint32_t recordType = 0u,
        const dxmt9::d3d9::RenderTapeOriginLocator *originLocator =
            nullptr,
        const dxmt9::d3d9::ImportedChunkView *currentChunk = nullptr) noexcept {
        if (renderTapeObjectAdmitted(identity))
            return true;
        if (!renderTapeRegistry_) {
            abortRenderTapeCapture("jit_materialize_registry_missing");
            return false;
        }
        if (!renderTapeCapture_ ||
            renderTapeCapture_->state() !=
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            return false;
        }
        const auto reject = [&](dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
                                std::uint32_t subresource =
                                    std::numeric_limits<std::uint32_t>::max()) {
            const dxmt9::d3d9::pe::PeWireObjectRef object{.identity = identity};
            RejectRenderTapeCaptureForChild(reason, object, subresource, {});
            return false;
        };
        const auto object = std::find_if(
            renderTapeRegistry_->objects.begin(),
            renderTapeRegistry_->objects.end(), [&](const auto &candidate) {
                return renderTapeSameIdentity(candidate.identity, identity);
            });
        if (object == renderTapeRegistry_->objects.end()) {
            const dxmt9::d3d9::pe::PeWireObjectRef reference{
                .identity = identity};
            dxmt9DeviceInfoLog(
                "render_tape_capture materialize_miss profile=%u device=%p "
                "registry=%p kind=%u generation=%u object_id=%llu live=0 "
                "pending=0 admitted=%d known_dead=%d handle_index=%u "
                "record_index=%u record_type=%u",
                dxmt9PeRenderTapeCaptureProfile(), this,
                static_cast<void *>(&*renderTapeRegistry_), identity.kind,
                identity.generation,
                static_cast<unsigned long long>(identity.objectId),
                renderTapeObjectAdmitted(identity) ? 1 : 0,
                hasRenderTapeDeadObject(reference) ? 1 : 0, handleIndex,
                recordIndex, recordType);
            return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                              UnmaterializedPreArmObject);
        }
        if (dxmt9::d3d9::renderTapeBootstrapRequiresAllLiveObjects(
                dxmt9PeRenderTapeCaptureProfile())) {
            // An unadmitted pre-arm identity is impossible after the sequence
            // profile's complete arm snapshot. Reject before emitting an
            // ObjectDefine that the second interval grammar cannot accept.
            return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                              UnmaterializedPreArmObject);
        }
        const auto armSnapshot = std::find_if(
            renderTapeArmSnapshots_.begin(), renderTapeArmSnapshots_.end(),
            [&](const auto &candidate) {
                return renderTapeSameIdentity(candidate.identity, identity);
            });
        const auto armOverlay = dxmt9::d3d9::
            renderTapeSelectArmObjectSnapshotOverlay(
                object->descriptor, object->content,
                armSnapshot != renderTapeArmSnapshots_.end()
                    ? std::span<const std::byte>(armSnapshot->descriptor)
                    : std::span<const std::byte>{},
                armSnapshot != renderTapeArmSnapshots_.end()
                    ? std::span<const std::vector<std::byte>>(
                          armSnapshot->content)
                    : std::span<const std::vector<std::byte>>{},
                armSnapshot != renderTapeArmSnapshots_.end()
                    ? armSnapshot->armOrdinal
                    : 0u,
                renderTapeArmSnapshotOrdinal_,
                object->role == RenderTapeLiveObject::Role::PresentOutput
                    ? dxmt9::d3d9::
                          RenderTapeArmObjectSnapshotOverlayPolicy::PresentOutput
                    : dxmt9::d3d9::
                          RenderTapeArmObjectSnapshotOverlayPolicy::Ordinary);
        if (armOverlay.source == dxmt9::d3d9::
                RenderTapeArmSnapshotOverlaySource::StaleArm) {
            abortRenderTapeCapture("jit_stale_arm_snapshot");
            return false;
        }
        const auto effectiveDescriptor = armOverlay.descriptor;
        const auto effectiveContent = armOverlay.content;
        if (object->lifetime.textureAlias &&
            !materializeRenderTapeObjectForReference(
                object->aliasParentTexture, handleIndex, recordIndex,
                recordType, originLocator, currentChunk)) {
            return false;
        }
        const bool incomplete =
            object->contentCount != effectiveContent.size() ||
            std::any_of(effectiveContent.begin(), effectiveContent.end(),
                        [](const auto &bytes) { return bytes.empty(); });
        const auto firstMissing = std::find_if(
            effectiveContent.begin(), effectiveContent.end(),
            [](const auto &bytes) { return bytes.empty(); });
        const auto firstMissingSubresource = static_cast<std::uint32_t>(
            firstMissing - effectiveContent.begin());
        bool producedByCapturedPass = false;
        if (incomplete && object->lifetime.textureAlias) {
            // A texture-derived surface owns no independent seed. Its parent
            // was resolved above; preserve the alias descriptor as Unavailable.
        } else if (incomplete && currentChunk && originLocator) {
            RenderTapeTextureDescriptorV2 texture{};
            dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 alias{};
            const auto aliasObject = std::find_if(
                renderTapeRegistry_->objects.begin(),
                renderTapeRegistry_->objects.end(), [&](const auto &candidate) {
                    return renderTapeSameIdentity(
                        candidate.identity, originLocator->originIdentity);
                });
            const bool exactTexture =
                renderTapeLoadTextureDescriptorV2(effectiveDescriptor, texture) &&
                dxmt9::d3d9::renderTapeProducedTextureShapeSupported(texture) &&
                aliasObject != renderTapeRegistry_->objects.end() &&
                aliasObject->lifetime.textureAlias &&
                renderTapeSameIdentity(aliasObject->aliasParentTexture,
                                       object->identity) &&
                renderTapeLoadSurfaceDescriptorV2(aliasObject->descriptor,
                                                   alias) &&
                dxmt9::d3d9::renderTapeSurfaceAliasMatchesTextureSubresource(
                    effectiveDescriptor, object->identity, alias) &&
                alias.subresource < effectiveContent.size() &&
                effectiveContent[alias.subresource].empty() &&
                ((texture.dimension == static_cast<std::uint32_t>(
                      RenderTapeTextureDimension::Texture2D) &&
                  alias.subresource == firstMissingSubresource &&
                  alias.subresource == 0u) ||
                 (texture.dimension == static_cast<std::uint32_t>(
                      RenderTapeTextureDimension::Cube) &&
                  alias.subresource < 6u));
            dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
            const bool exactStandaloneSurface =
                renderTapeLoadSurfaceDescriptorV2(effectiveDescriptor, surface) &&
                surface.storage == static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
                dxmt9::d3d9::renderTapeProducedStandaloneSurfaceSupported(
                    surface.surface);
            producedByCapturedPass =
                (exactTexture &&
                 dxmt9::d3d9::renderTapeProveProducedByCapturedPass(
                     *currentChunk, originLocator->originIdentity,
                     object->identity)) ||
                (exactStandaloneSurface &&
                 dxmt9::d3d9::
                     renderTapeClassifyProducedStandaloneSurfaceByCapturedPass(
                         *currentChunk, object->identity, surface.surface)
                         .accepted());
        } else if (incomplete) {
            // Keep the diagnostic index identical to the existing rejection
            // below: a count mismatch with no empty slot reports content.size().
            const auto missingSubresource = static_cast<std::uint32_t>(
                firstMissing - object->content.begin());
            dxmt9::d3d9::RenderTapeOriginLocator locator{};
            locator.originIdentity = identity;
            locator.resolvedIdentity = identity;
            locator.recordIndex = recordIndex;
            locator.recordType = recordType;
            locator.handleIndex = handleIndex;
            if (originLocator) {
                locator = *originLocator;
                locator.resolvedIdentity = identity;
                locator.aliasOrigin =
                    !dxmt9::d3d9::renderTapeSameWireObject(
                        locator.originIdentity, identity);
            }
            const auto missingSeed = dxmt9::d3d9::renderTapeDescribeMissingSeed(
                object->identity, effectiveDescriptor, missingSubresource,
                dxmt9::d3d9::RenderTapeReferenceProvenance{
                    .handleIndex = locator.handleIndex,
                    .recordIndex = locator.recordIndex,
                    .recordType = locator.recordType,
                });
            dxmt9::d3d9::renderTapeFirstAccessArm(
                renderTapeFirstAccessLedger_, locator.originIdentity,
                object->identity);
            dxmt9DeviceInfoLog(
                "render_tape_capture missing_seed identity_kind=%u "
                "generation=%u object_id=%llu descriptor_status=%s "
                "expected_status=%s texture_dimension=%u mip_levels=%u "
                "subresources=%u missing_subresource=%u usage=%u "
                "resource_type=%u pool=%u format=%u width=%u height=%u "
                "multisample_type=%u multisample_quality=%u "
                "expected_tight_bytes=%llu expected_tight_bytes_valid=%d "
                "handle_index=%u record_index=%u record_type=%u "
                "origin_kind=%u origin_generation=%u origin_object_id=%llu "
                "resolved_kind=%u resolved_generation=%u "
                "resolved_object_id=%llu section_kind=%u binding_slot=%u "
                "alias_origin=%d command_role=%s storage_role=%s "
                "locator_status=%s",
                missingSeed.identity.kind, missingSeed.identity.generation,
                static_cast<unsigned long long>(missingSeed.identity.objectId),
                dxmt9::d3d9::renderTapeMissingSeedDescriptorStatusName(
                    missingSeed.descriptorStatus),
                dxmt9::d3d9::renderTapeExpectedContentStatusName(
                    missingSeed.expectedContentStatus),
                static_cast<unsigned>(missingSeed.textureDimension),
                missingSeed.mipLevelCount, missingSeed.subresourceCount,
                missingSeed.missingSubresource, missingSeed.missingSurface.usage,
                missingSeed.missingSurface.resourceType,
                missingSeed.missingSurface.pool, missingSeed.missingSurface.format,
                missingSeed.missingSurface.width, missingSeed.missingSurface.height,
                missingSeed.missingSurface.multiSampleType,
                missingSeed.missingSurface.multiSampleQuality,
                static_cast<unsigned long long>(missingSeed.expectedTightBytes),
                missingSeed.expectedTightBytesValid ? 1 : 0,
                missingSeed.provenance.handleIndex,
                missingSeed.provenance.recordIndex,
                missingSeed.provenance.recordType, locator.originIdentity.kind,
                locator.originIdentity.generation,
                static_cast<unsigned long long>(locator.originIdentity.objectId),
                locator.resolvedIdentity.kind, locator.resolvedIdentity.generation,
                static_cast<unsigned long long>(locator.resolvedIdentity.objectId),
                locator.sectionKind, locator.bindingSlot,
                locator.aliasOrigin ? 1 : 0,
                dxmt9::d3d9::renderTapeCommandRoleName(locator.role),
                dxmt9::d3d9::renderTapeStorageRoleName(locator.storageRole),
                dxmt9::d3d9::renderTapeOriginLocatorStatusName(
                    locator.status));
            if (recordType == D9C_COMMAND_RECORD_UPDATE_TEXTURE &&
                !renderTapeObjectAdmitted(identity)) {
                // UpdateTexture's destination initial bytes must precede the
                // command. Do not poison the long-lived registry when that
                // proof is absent; the post-append closure can retain a
                // complete source copy for the next arm attempt.
                abortRenderTapeCapture("update_texture_destination_unadmitted");
                return false;
            }
            return reject(
                dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                    IncompleteSubresourceSeed,
                firstMissingSubresource);
        }
        dxmt9::d3d9::RenderTapeExpectedContentContract contentContract{};
        if (!producedByCapturedPass && !object->lifetime.textureAlias &&
            !renderTapeValidateExpectedContent(
                object->identity, effectiveDescriptor, effectiveContent,
                contentContract)) {
            dxmt9DeviceInfoLog(
                "render_tape_capture materialize rejected reason=expected_content_contract "
                "status=%s kind=%u generation=%u object_id=%llu expected_bytes=%llu "
                "expected_count=%u actual_count=%zu",
                dxmt9::d3d9::renderTapeExpectedContentStatusName(
                    contentContract.status),
                object->identity.kind, object->identity.generation,
                static_cast<unsigned long long>(object->identity.objectId),
                static_cast<unsigned long long>(contentContract.bytes),
                contentContract.count, effectiveContent.size());
            return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                              ExpectedContentContract);
        }
        dxmt9::d3d9::RenderTapeDigest immutableDigest{};
        std::uint64_t immutableBytes = 0u;
        if (!object->immutablePayload.empty()) {
            if (renderTapeCapture_->registerBlobBytes(object->immutablePayload,
                                                      &immutableDigest) !=
                dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                abortRenderTapeCapture("jit_object_define_blob");
                return false;
            }
            immutableBytes = object->immutablePayload.size();
        }
        const auto descriptorKind = static_cast<std::uint32_t>(
            dxmt9::d3d9::renderTapeDescriptorKindForObject(identity.kind));
        std::vector<std::byte> descriptor(effectiveDescriptor.begin(),
                                          effectiveDescriptor.end());
        if (producedByCapturedPass) {
            if (identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
                RenderTapeTextureDescriptorV2 texture{};
                if (!renderTapeLoadTextureDescriptorV2(descriptor, texture) ||
                    texture.initialContentDisposition !=
                        static_cast<std::uint32_t>(
                            RenderTapeInitialContentDisposition::CompleteSeed)) {
                    return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                                       DescriptorMismatch);
                }
                texture.initialContentDisposition = static_cast<std::uint32_t>(
                    RenderTapeInitialContentDisposition::ProducedByCapturedPass);
                std::memcpy(descriptor.data(), &texture, sizeof(texture));
            } else {
                dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
                if (!renderTapeLoadSurfaceDescriptorV2(descriptor, surface) ||
                    surface.storage != static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) ||
                    !dxmt9::d3d9::renderTapeProducedStandaloneSurfaceSupported(
                        surface.surface)) {
                    return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                                       DescriptorMismatch);
                }
                surface.initialContentDisposition = static_cast<std::uint32_t>(
                    RenderTapeInitialContentDisposition::ProducedByCapturedPass);
                std::memcpy(descriptor.data(), &surface, sizeof(surface));
            }
        }
        if (renderTapeCapture_->objectDefine(
                identity, descriptorKind, descriptor, immutableBytes,
                immutableDigest,
                producedByCapturedPass || object->lifetime.textureAlias
                    ? 0u
                    : contentContract.bytes,
                producedByCapturedPass || object->lifetime.textureAlias
                    ? 0u
                    : contentContract.count) !=
            dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            abortRenderTapeCapture("jit_object_define");
            return false;
        }
        if (producedByCapturedPass || object->lifetime.textureAlias) {
            try {
                renderTapeAdmittedIdentities_.push_back(identity);
            } catch (...) {
                abortRenderTapeCapture("jit_identity_allocation");
                return false;
            }
            return true;
        }
        for (std::uint32_t subresource = 0u;
             subresource < effectiveContent.size(); ++subresource) {
            if (renderTapeCapture_->resourceMutationBytes(
                    identity, dxmt9::d3d9::RenderTapeMutationKind::Upload,
                    subresource, 0u, effectiveContent[subresource]) !=
                dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                abortRenderTapeCapture("jit_resource_mutation");
                return false;
            }
        }
        try {
            renderTapeAdmittedIdentities_.push_back(identity);
        } catch (...) {
            abortRenderTapeCapture("jit_identity_allocation");
            return false;
        }
        return true;
    }

    bool admitRenderTapeChunkHandles(
        const D9CCommandChunk &chunk,
        const PeCommandChunkCommitInfo &info) noexcept {
        (void)info;
        if (chunk.recordBytes < sizeof(D9CCommandChunkWireHeader) ||
            d9cWireHandleValue(chunk.records) == 0u) {
            abortRenderTapeCapture("command_chunk_invalid");
            return false;
        }
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(
                d9cWireHandleValue(chunk.records))),
            chunk.recordBytes);
        dxmt9::d3d9::ImportedChunkView imported{};
        dxmt9::d3d9::CommandChunkValidationScratch scratch{};
        const auto validation = dxmt9::d3d9::validateCommandChunk(
            bytes, dxmt9::d3d9::CommandChunkEnvelope{
                       .version = chunk.version,
                       .recordCount = info.recordCount,
                       .handleCount = info.handleCount},
            &imported, scratch);
        if (!validation.valid()) {
            abortRenderTapeCapture("command_chunk_validation");
            return false;
        }
        struct PreflightAttribution {
            dxmt9::d3d9::RenderTapeCommandAdmissionResult admission{};
            dxmt9::d3d9::RenderTapeOriginLocator locator{};
            dxmt9::d3d9::RenderTapeProducedProofResult producedProof{};
            dxmt9::d3d9::RenderTapeFirstAccessObservation firstAccess{};
            dxmt9::d3d9::RenderTapeMissingSeedDescriptor missingSeed{};
            D9CWireObjectIdentity resolvedIdentity{};
            D9CWireObjectIdentity dependencyIdentity{};
            dxmt9::d3d9::RenderTapeCommandAdmissionStatus
                dependencyAdmissionStatus =
                    dxmt9::d3d9::RenderTapeCommandAdmissionStatus::OriginRejected;
            dxmt9::d3d9::RenderTapeProducedProofStatus
                dependencyProducedProofStatus =
                    dxmt9::d3d9::RenderTapeProducedProofStatus::NoTerminalAccess;
            dxmt9::d3d9::RenderTapeFirstAccessStatus
                dependencyFirstAccessStatus =
                    dxmt9::d3d9::RenderTapeFirstAccessStatus::Idle;
            bool hasDependency = false;
            bool registryPresent = false;
            bool admitted = false;
            bool live = false;
            bool dead = false;
            bool contentComplete = false;
            bool armSnapshotPresent = false;
            bool armSnapshotCurrent = false;
            bool textureAlias = false;
            bool producedDescriptorSupported = false;
            bool producedAliasPresent = false;
            bool producedAliasDescriptorAccepted = false;
            bool producedAliasParentMatched = false;
            bool producedAliasShapeMatched = false;
            std::uint32_t producedAliasSubresource =
                std::numeric_limits<std::uint32_t>::max();
            D9CSurfaceDesc producedAliasSurface{};
            std::size_t descriptorBytes = 0u;
            std::uint32_t expectedContentCount = 0u;
            std::size_t actualContentCount = 0u;
        };
        const auto canMaterialize = [&](const auto &self,
                                        const D9CWireObjectIdentity &identity,
                                        const dxmt9::d3d9::RenderTapeOriginLocator
                                            &originLocator,
                                        std::optional<D9CWireObjectIdentity>
                                            &producedIdentity)
            -> PreflightAttribution {
            PreflightAttribution attribution{};
            attribution.locator = originLocator;
            attribution.locator.resolvedIdentity = identity;
            attribution.locator.aliasOrigin = !renderTapeSameIdentity(
                originLocator.originIdentity, identity);
            attribution.resolvedIdentity = identity;
            attribution.registryPresent = renderTapeRegistry_.has_value();
            attribution.admitted = renderTapeObjectAdmitted(identity);
            if (attribution.admitted) {
                attribution.admission =
                    dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                        .originAccepted = originLocator.status ==
                            dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                        .registryPresent = attribution.registryPresent,
                        .admitted = true,
                    });
                return attribution;
            }
            if (!renderTapeRegistry_) {
                attribution.admission =
                    dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                        .originAccepted = originLocator.status ==
                            dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                    });
                return attribution;
            }
            const auto object = std::find_if(
                renderTapeRegistry_->objects.begin(),
                renderTapeRegistry_->objects.end(), [&](const auto &candidate) {
                    return renderTapeSameIdentity(candidate.identity, identity);
                });
            attribution.live = object != renderTapeRegistry_->objects.end();
            if (!attribution.live) {
                const dxmt9::d3d9::pe::PeWireObjectRef reference{
                    .identity = identity};
                attribution.dead = hasRenderTapeDeadObject(reference);
                attribution.admission =
                    dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                        .originAccepted = originLocator.status ==
                            dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                        .registryPresent = true,
                        .deadObject = attribution.dead,
                    });
                return attribution;
            }
            attribution.textureAlias = object->lifetime.textureAlias;
            const auto armSnapshot = std::find_if(
                renderTapeArmSnapshots_.begin(), renderTapeArmSnapshots_.end(),
                [&](const auto &candidate) {
                    return renderTapeSameIdentity(candidate.identity, identity);
                });
            attribution.armSnapshotPresent =
                armSnapshot != renderTapeArmSnapshots_.end();
            const auto armOverlay = dxmt9::d3d9::
                renderTapeSelectArmObjectSnapshotOverlay(
                    object->descriptor, object->content,
                    attribution.armSnapshotPresent
                        ? std::span<const std::byte>(armSnapshot->descriptor)
                        : std::span<const std::byte>{},
                    attribution.armSnapshotPresent
                        ? std::span<const std::vector<std::byte>>(
                              armSnapshot->content)
                        : std::span<const std::vector<std::byte>>{},
                    attribution.armSnapshotPresent
                        ? armSnapshot->armOrdinal
                        : 0u,
                    renderTapeArmSnapshotOrdinal_,
                    object->role == RenderTapeLiveObject::Role::PresentOutput
                        ? dxmt9::d3d9::
                              RenderTapeArmObjectSnapshotOverlayPolicy::
                                  PresentOutput
                        : dxmt9::d3d9::
                              RenderTapeArmObjectSnapshotOverlayPolicy::
                                  Ordinary);
            attribution.armSnapshotCurrent = armOverlay.source == dxmt9::d3d9::
                RenderTapeArmSnapshotOverlaySource::CurrentArm;
            attribution.descriptorBytes = armOverlay.descriptor.size();
            attribution.expectedContentCount = object->contentCount;
            attribution.actualContentCount = armOverlay.content.size();
            if (armOverlay.source == dxmt9::d3d9::
                    RenderTapeArmSnapshotOverlaySource::StaleArm) {
                attribution.admission =
                    dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                        .originAccepted = originLocator.status ==
                            dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                        .registryPresent = true,
                        .liveObject = true,
                    });
                return attribution;
            }
            const auto effectiveDescriptor = armOverlay.descriptor;
            const auto effectiveContent = armOverlay.content;
            if (attribution.textureAlias) {
                auto dependency = self(self, object->aliasParentTexture,
                                       originLocator, producedIdentity);
                if (!dependency.admission.accepted()) {
                    dependency.hasDependency = true;
                    dependency.dependencyIdentity = object->aliasParentTexture;
                    dependency.dependencyAdmissionStatus =
                        dependency.admission.status;
                    dependency.dependencyProducedProofStatus =
                        dependency.producedProof.status;
                    dependency.dependencyFirstAccessStatus =
                        dependency.firstAccess.status;
                    dependency.admission =
                        dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                            .originAccepted = originLocator.status ==
                                dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                            .registryPresent = true,
                            .liveObject = true,
                            .aliasDependencyAccepted = false,
                            .textureAlias = true,
                        });
                    return dependency;
                }
            }
            const bool incomplete =
                object->contentCount != effectiveContent.size() ||
                std::any_of(effectiveContent.begin(), effectiveContent.end(),
                            [](const auto &bytes) { return bytes.empty(); });
            attribution.contentComplete = !incomplete;
            if (!incomplete || attribution.textureAlias) {
                attribution.admission =
                    dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                        .originAccepted = originLocator.status ==
                            dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                        .registryPresent = true,
                        .liveObject = true,
                        .aliasDependencyAccepted = true,
                        .contentComplete = !incomplete,
                        .textureAlias = attribution.textureAlias,
                    });
                return attribution;
            }
            const auto missing = std::find_if(
                effectiveContent.begin(), effectiveContent.end(),
                [](const auto &bytes) { return bytes.empty(); });
            const auto missingSubresource = static_cast<std::uint32_t>(
                missing - effectiveContent.begin());
            attribution.missingSeed = dxmt9::d3d9::renderTapeDescribeMissingSeed(
                identity, effectiveDescriptor, missingSubresource,
                {.handleIndex = originLocator.handleIndex,
                 .recordIndex = originLocator.recordIndex,
                 .recordType = originLocator.recordType});
            RenderTapeTextureDescriptorV2 texture{};
            dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
            dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 alias{};
            const auto aliasObject = std::find_if(
                renderTapeRegistry_->objects.begin(),
                renderTapeRegistry_->objects.end(), [&](const auto &candidate) {
                    return renderTapeSameIdentity(
                        candidate.identity, originLocator.originIdentity);
                });
            attribution.producedAliasPresent =
                aliasObject != renderTapeRegistry_->objects.end();
            attribution.producedAliasDescriptorAccepted =
                attribution.producedAliasPresent &&
                renderTapeLoadSurfaceDescriptorV2(aliasObject->descriptor,
                                                   alias);
            if (attribution.producedAliasDescriptorAccepted) {
                attribution.producedAliasSubresource = alias.subresource;
                attribution.producedAliasSurface = alias.surface;
                attribution.producedAliasParentMatched =
                    renderTapeSameIdentity(alias.parentTexture,
                                           object->identity);
                attribution.producedAliasShapeMatched = dxmt9::d3d9::
                    renderTapeSurfaceAliasMatchesTextureSubresource(
                        effectiveDescriptor, object->identity, alias);
            }
            const bool producedTexture =
                renderTapeLoadTextureDescriptorV2(effectiveDescriptor, texture) &&
                dxmt9::d3d9::renderTapeProducedTextureShapeSupported(texture) &&
                attribution.producedAliasPresent &&
                aliasObject->lifetime.textureAlias &&
                renderTapeSameIdentity(aliasObject->aliasParentTexture,
                                       object->identity) &&
                attribution.producedAliasDescriptorAccepted &&
                attribution.producedAliasShapeMatched &&
                alias.subresource < effectiveContent.size() &&
                effectiveContent[alias.subresource].empty() &&
                ((texture.dimension == static_cast<std::uint32_t>(
                      RenderTapeTextureDimension::Texture2D) &&
                  alias.subresource == missingSubresource &&
                  alias.subresource == 0u) ||
                 (texture.dimension == static_cast<std::uint32_t>(
                      RenderTapeTextureDimension::Cube) &&
                  alias.subresource < 6u));
            const bool producedStandaloneSurface =
                renderTapeLoadSurfaceDescriptorV2(effectiveDescriptor, surface) &&
                surface.storage == static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
                dxmt9::d3d9::renderTapeProducedStandaloneSurfaceSupported(
                    surface.surface);
            attribution.producedDescriptorSupported =
                producedTexture || producedStandaloneSurface;
            if (producedTexture) {
                attribution.producedProof = dxmt9::d3d9::
                    renderTapeClassifyProducedByCapturedPass(
                        imported, originLocator.originIdentity, identity);
                attribution.firstAccess =
                    attribution.producedProof.observation;
            } else if (producedStandaloneSurface) {
                attribution.producedProof = dxmt9::d3d9::
                    renderTapeClassifyProducedStandaloneSurfaceByCapturedPass(
                        imported, identity, surface.surface);
                attribution.firstAccess = attribution.producedProof.observation;
            } else {
                // Diagnostic-only generic observation: unlike the production
                // ProducedByCapturedPass grammar, this deliberately permits a
                // direct standalone surface identity so r29 can prove whether
                // the D24X8 binding is overwritten before any read.
                dxmt9::d3d9::RenderTapeFirstAccessLedger ledger{};
                dxmt9::d3d9::renderTapeFirstAccessArm(
                    ledger, originLocator.originIdentity, identity);
                attribution.firstAccess =
                    dxmt9::d3d9::renderTapeFirstAccessObserve(ledger, imported);
            }
            attribution.admission =
                dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                    .originAccepted = originLocator.status ==
                        dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                    .registryPresent = true,
                    .liveObject = true,
                    .aliasDependencyAccepted = true,
                    .contentComplete = false,
                    .textureAlias = false,
                    .producedDescriptorSupported =
                        attribution.producedDescriptorSupported,
                    .producedProofAccepted =
                        attribution.producedProof.accepted(),
                });
            if (attribution.admission.accepted())
                producedIdentity = identity;
            return attribution;
        };
        std::optional<D9CWireObjectIdentity> producedIdentity;
        for (std::size_t handleIndex = 0u;
             handleIndex < imported.handles.size(); ++handleIndex) {
            const auto &handle = imported.handles[handleIndex];
            const D9CWireObjectIdentity identity{
                .kind = handle.kind,
                .generation = handle.generation,
                .objectId = handle.objectId};
            const auto originLocator = dxmt9::d3d9::renderTapeLocateOrigin(
                imported, static_cast<std::uint32_t>(handleIndex), identity);
            const auto attribution = canMaterialize(
                canMaterialize, identity, originLocator, producedIdentity);
            if (!attribution.admission.accepted()) {
                const auto &locator = attribution.locator;
                const auto &missing = attribution.missingSeed;
                const auto &observation = attribution.firstAccess;
                dxmt9DeviceInfoLog(
                    "render_tape_capture command_chunk_preflight "
                    "status=%s handle_index=%u record_index=%u record_type=%u "
                    "section_kind=%u binding_slot=%u command_role=%s "
                    "storage_role=%s locator_status=%s origin_kind=%u "
                    "origin_generation=%u origin_object_id=%llu "
                    "resolved_kind=%u resolved_generation=%u "
                    "resolved_object_id=%llu alias_origin=%d registry=%d "
                    "admitted=%d live=%d dead=%d content_complete=%d "
                    "arm_snapshot_present=%d arm_snapshot_current=%d "
                    "content_expected=%u content_actual=%zu descriptor_bytes=%zu "
                    "descriptor_status=%s expected_status=%s dimension=%u "
                    "mips=%u subresources=%u missing_subresource=%u format=%u "
                    "width=%u height=%u multisample_type=%u usage=%u "
                    "produced_descriptor=%d produced_proof=%s "
                    "first_access_status=%s first_access_class=%s "
                    "produced_alias_present=%d produced_alias_descriptor=%d "
                    "produced_alias_parent=%d produced_alias_shape=%d "
                    "produced_alias_subresource=%u produced_alias_format=%u "
                    "produced_alias_width=%u produced_alias_height=%u "
                    "produced_alias_usage=%u "
                    "dependency_present=%d dependency_kind=%u "
                    "dependency_generation=%u dependency_object_id=%llu "
                    "dependency_status=%s dependency_produced_proof=%s "
                    "dependency_first_access_status=%s "
                    "produced_candidate_present=%d produced_candidate_kind=%u "
                    "produced_candidate_generation=%u "
                    "produced_candidate_object_id=%llu",
                    dxmt9::d3d9::renderTapeCommandAdmissionStatusName(
                        attribution.admission.status),
                    locator.handleIndex, locator.recordIndex,
                    locator.recordType, locator.sectionKind,
                    locator.bindingSlot,
                    dxmt9::d3d9::renderTapeCommandRoleName(locator.role),
                    dxmt9::d3d9::renderTapeStorageRoleName(locator.storageRole),
                    dxmt9::d3d9::renderTapeOriginLocatorStatusName(locator.status),
                    locator.originIdentity.kind,
                    locator.originIdentity.generation,
                    static_cast<unsigned long long>(
                        locator.originIdentity.objectId),
                    attribution.resolvedIdentity.kind,
                    attribution.resolvedIdentity.generation,
                    static_cast<unsigned long long>(
                        attribution.resolvedIdentity.objectId),
                    locator.aliasOrigin ? 1 : 0,
                    attribution.registryPresent ? 1 : 0,
                    attribution.admitted ? 1 : 0,
                    attribution.live ? 1 : 0,
                    attribution.dead ? 1 : 0,
                    attribution.contentComplete ? 1 : 0,
                    attribution.armSnapshotPresent ? 1 : 0,
                    attribution.armSnapshotCurrent ? 1 : 0,
                    attribution.expectedContentCount,
                    attribution.actualContentCount,
                    attribution.descriptorBytes,
                    dxmt9::d3d9::renderTapeMissingSeedDescriptorStatusName(
                        missing.descriptorStatus),
                    dxmt9::d3d9::renderTapeExpectedContentStatusName(
                        missing.expectedContentStatus),
                    static_cast<unsigned>(missing.textureDimension),
                    missing.mipLevelCount, missing.subresourceCount,
                    missing.missingSubresource, missing.missingSurface.format,
                    missing.missingSurface.width, missing.missingSurface.height,
                    missing.missingSurface.multiSampleType,
                    missing.missingSurface.usage,
                    attribution.producedDescriptorSupported ? 1 : 0,
                    dxmt9::d3d9::renderTapeProducedProofStatusName(
                        attribution.producedProof.status),
                    dxmt9::d3d9::renderTapeFirstAccessStatusName(
                        observation.status),
                    dxmt9::d3d9::renderTapeFirstAccessClassName(
                        observation.classification),
                    attribution.producedAliasPresent ? 1 : 0,
                    attribution.producedAliasDescriptorAccepted ? 1 : 0,
                    attribution.producedAliasParentMatched ? 1 : 0,
                    attribution.producedAliasShapeMatched ? 1 : 0,
                    attribution.producedAliasSubresource,
                    attribution.producedAliasSurface.format,
                    attribution.producedAliasSurface.width,
                    attribution.producedAliasSurface.height,
                    attribution.producedAliasSurface.usage,
                    attribution.hasDependency ? 1 : 0,
                    attribution.dependencyIdentity.kind,
                    attribution.dependencyIdentity.generation,
                    static_cast<unsigned long long>(
                        attribution.dependencyIdentity.objectId),
                    dxmt9::d3d9::renderTapeCommandAdmissionStatusName(
                        attribution.dependencyAdmissionStatus),
                    dxmt9::d3d9::renderTapeProducedProofStatusName(
                        attribution.dependencyProducedProofStatus),
                    dxmt9::d3d9::renderTapeFirstAccessStatusName(
                        attribution.dependencyFirstAccessStatus),
                    producedIdentity ? 1 : 0,
                    producedIdentity ? producedIdentity->kind : 0u,
                    producedIdentity ? producedIdentity->generation : 0u,
                    static_cast<unsigned long long>(
                        producedIdentity ? producedIdentity->objectId : 0u));
                abortRenderTapeCapture("command_chunk_produced_pass_preflight");
                return false;
            }
        }
        for (std::size_t handleIndex = 0u;
             handleIndex < imported.handles.size(); ++handleIndex) {
            const auto &handle = imported.handles[handleIndex];
            const D9CWireObjectIdentity identity{
                .kind = handle.kind,
                .generation = handle.generation,
                .objectId = handle.objectId};
            const auto originLocator = dxmt9::d3d9::renderTapeLocateOrigin(
                imported, static_cast<std::uint32_t>(handleIndex), identity);
            if (!materializeRenderTapeObjectForReference(
                identity, originLocator.handleIndex,
                originLocator.recordIndex, originLocator.recordType,
                &originLocator, &imported)) {
                return false;
            }
        }
        return true;
    }

    void observeRenderTapeFirstAccessChunk(
        const D9CCommandChunk &chunk,
        const PeCommandChunkCommitInfo &info) noexcept {
        if (!renderTapeFirstAccessLedger_.armed ||
            chunk.recordBytes < sizeof(D9CCommandChunkWireHeader) ||
            d9cWireHandleValue(chunk.records) == 0u) {
            return;
        }
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(
                d9cWireHandleValue(chunk.records))),
            chunk.recordBytes);
        dxmt9::d3d9::ImportedChunkView imported{};
        dxmt9::d3d9::CommandChunkValidationScratch scratch{};
        const auto validation = dxmt9::d3d9::validateCommandChunk(
            bytes, dxmt9::d3d9::CommandChunkEnvelope{
                       .version = chunk.version,
                       .recordCount = info.recordCount,
                       .handleCount = info.handleCount},
            &imported, scratch);
        if (!validation.valid()) {
            if (!renderTapeFirstAccessLedger_.terminal) {
                renderTapeFirstAccessLedger_.terminal = true;
                dxmt9DeviceInfoLog(
                    "render_tape_capture first_access status=malformed "
                    "class=unknown reason=chunk_validation origin_kind=%u "
                    "origin_generation=%u origin_object_id=%llu "
                    "resolved_kind=%u resolved_generation=%u "
                    "resolved_object_id=%llu",
                    renderTapeFirstAccessLedger_.originIdentity.kind,
                    renderTapeFirstAccessLedger_.originIdentity.generation,
                    static_cast<unsigned long long>(
                        renderTapeFirstAccessLedger_.originIdentity.objectId),
                    renderTapeFirstAccessLedger_.resolvedIdentity.kind,
                    renderTapeFirstAccessLedger_.resolvedIdentity.generation,
                    static_cast<unsigned long long>(
                        renderTapeFirstAccessLedger_.resolvedIdentity.objectId));
            }
            return;
        }
        const auto observation = dxmt9::d3d9::renderTapeFirstAccessObserve(
            renderTapeFirstAccessLedger_, imported);
        if (observation.status !=
                dxmt9::d3d9::RenderTapeFirstAccessStatus::Terminal &&
            observation.status !=
                dxmt9::d3d9::RenderTapeFirstAccessStatus::Malformed) {
            return;
        }
        dxmt9DeviceInfoLog(
            "render_tape_capture first_access status=%s class=%s "
            "origin_kind=%u origin_generation=%u origin_object_id=%llu "
            "resolved_kind=%u resolved_generation=%u resolved_object_id=%llu "
            "observed_kind=%u observed_generation=%u observed_object_id=%llu "
            "record_index=%u record_type=%u handle_index=%u section_kind=%u "
            "binding_slot=%u alias_origin=%d",
            dxmt9::d3d9::renderTapeFirstAccessStatusName(observation.status),
            dxmt9::d3d9::renderTapeFirstAccessClassName(
                observation.classification),
            observation.originIdentity.kind, observation.originIdentity.generation,
            static_cast<unsigned long long>(observation.originIdentity.objectId),
            observation.resolvedIdentity.kind,
            observation.resolvedIdentity.generation,
            static_cast<unsigned long long>(observation.resolvedIdentity.objectId),
            observation.observedIdentity.kind,
            observation.observedIdentity.generation,
            static_cast<unsigned long long>(observation.observedIdentity.objectId),
            observation.recordIndex, observation.recordType,
            observation.handleIndex, observation.sectionKind,
            observation.bindingSlot, observation.aliasOrigin ? 1 : 0);
    }

    bool prepareRenderTapeChunkCapture(
        const D9CCommandChunk& chunk,
        const PeCommandChunkCommitInfo& info) noexcept {
        if (!renderTapeCapture_ ||
            renderTapeCapture_->state() !=
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            return false;
        }
        if (renderTapeArmBoundaryPhase_ ==
                dxmt9::d3d9::RenderTapeArmBoundaryPhase::Armed &&
            !advanceRenderTapeArmBoundary(dxmt9::d3d9::
                RenderTapeArmBoundaryPhase::FirstCapturedChunk)) {
            abortRenderTapeCapture("arm_boundary_order");
            return false;
        }
        if (renderTapeArmBoundaryPhase_ != dxmt9::d3d9::
                RenderTapeArmBoundaryPhase::FirstCapturedChunk) {
            abortRenderTapeCapture("arm_boundary_order");
            return false;
        }
        if (!admitRenderTapeChunkHandles(chunk, info)) {
            // The missing-seed arm can happen while admitting this very
            // chunk. Re-scan it now that the exact target is known.
            observeRenderTapeFirstAccessChunk(chunk, info);
            return false;
        }
        return true;
    }

    void captureCommittedRenderTapeChunk(
        const D9CCommandChunk& chunk,
        const PeCommandChunkCommitInfo& info) noexcept {
        if (!renderTapeCapture_ ||
            renderTapeCapture_->state() !=
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            return;
        }
        const auto status = renderTapeCapture_->commandChunk(
            dxmt9::d3d9::CommandChunkEnvelope{
                .version = chunk.version,
                .recordCount = info.recordCount,
                .handleCount = info.handleCount,
            },
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(
                    d9cWireHandleValue(chunk.records))),
                chunk.recordBytes));
        if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            dxmt9DeviceInfoLog(
                "render_tape_capture command_chunk aborted status=%u records=%u "
                "handles=%u",
                static_cast<unsigned>(status), info.recordCount, info.handleCount);
            abortRenderTapeCapture("command_chunk");
        }
    }

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

    void finishRenderTapeCaptureAtPresentBoundary() noexcept {
        if (!renderTapeCapture_ ||
            renderTapeCapture_->state() !=
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            return;
        }
        if (!renderTapeExpectedDigest_) {
            abortRenderTapeCapture("present_output_capture_missing");
            return;
        }
        const std::uint64_t capturedPresentOrdinal =
            renderTapeCapture_->eventCount();
        const auto status = renderTapeCapture_->completePresent(
            capturedPresentOrdinal,
            ++renderTapeCompletionOrdinal_,
            dxmt9::d3d9::RenderTapeDigestValidity::Sha256,
            *renderTapeExpectedDigest_,
            std::as_bytes(std::span(renderTapeCaptureOracle_)),
            renderTapeExpectedPixels_, renderTapeExpectedSourcePixels_);
        if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted &&
            status != dxmt9::d3d9::RenderTapeCaptureStatus::Complete) {
            dxmt9DeviceInfoLog(
                "render_tape_capture completion aborted status=%u events=%u "
                "chunks=%llu present_chunk_seen=%d oracle_bytes=%zu validation=%u",
                static_cast<unsigned>(status), renderTapeCapture_->eventCount(),
                static_cast<unsigned long long>(commandChunkCommits_),
                renderTapeCapture_->presentChunkSeen() ? 1 : 0,
                std::as_bytes(std::span(renderTapeCaptureOracle_)).size(),
                static_cast<unsigned>(renderTapeCapture_->validationStatus()));
            const auto &validation = renderTapeCapture_->validationResult();
            dxmt9DeviceInfoLog(
                "render_tape_capture validation_failure status=%u name=%s "
                "failed_event_index=%u failed_event_type=%u chunk_status=%u "
                "incomplete_reason=%s offending_present=%d offending_kind=%u "
                "offending_generation=%u offending_object_id=%llu",
                static_cast<unsigned>(validation.status),
                dxmt9::d3d9::renderTapeValidationStatusName(validation.status),
                validation.failedEventIndex, validation.failedEventType,
                static_cast<unsigned>(validation.chunkStatus),
                dxmt9::d3d9::renderTapeIncompleteFrameReasonName(
                    validation.incompleteFrameReason),
                validation.hasOffendingIdentity ? 1 : 0,
                validation.offendingIdentity.kind,
                validation.offendingIdentity.generation,
                static_cast<unsigned long long>(
                    validation.offendingIdentity.objectId));
            if (validation.objectDefine.valid()) {
                const auto &detail = validation.objectDefine;
                dxmt9DeviceInfoLog(
                    "render_tape_capture object_define_detail subreason=%u "
                    "name=%s kind=%u generation=%u object_id=%llu "
                    "descriptor_kind=%u descriptor_bytes=%u "
                    "descriptor_payload_bytes=%u payload_validity=%u "
                    "immutable_bytes=%llu expected_bytes=%llu "
                    "expected_count=%u schema=%u dimension=%u mips=%u "
                    "subresources=%u storage=%u disposition=%u subresource=%u "
                    "descriptor_extent=%llu/%llu parent_kind=%u "
                    "parent_generation=%u parent_object_id=%llu",
                    static_cast<unsigned>(detail.subreason),
                    dxmt9::d3d9::renderTapeObjectDefineValidationSubreasonName(
                        detail.subreason),
                    detail.identity.kind, detail.identity.generation,
                    static_cast<unsigned long long>(detail.identity.objectId),
                    detail.descriptorKind, detail.descriptorBytes,
                    detail.descriptorPayloadBytes, detail.payloadValidity,
                    static_cast<unsigned long long>(detail.immutablePayloadBytes),
                    static_cast<unsigned long long>(detail.expectedContentBytes),
                    detail.expectedContentCount, detail.descriptorSchemaVersion,
                    detail.descriptorDimension, detail.descriptorMipLevelCount,
                    detail.descriptorSubresourceCount, detail.descriptorStorage,
                    detail.descriptorDisposition, detail.descriptorSubresource,
                    static_cast<unsigned long long>(detail.descriptorExtentBytes),
                    static_cast<unsigned long long>(
                        detail.descriptorExpectedExtentBytes),
                    detail.parentTexture.kind, detail.parentTexture.generation,
                    static_cast<unsigned long long>(
                        detail.parentTexture.objectId));
            }
            abortRenderTapeCapture("completion");
            return;
        }
        // Sequence profile keeps the first interval journaled but unsealed;
        // publication is deliberately deferred until the second Present has
        // passed final validation.
        if (status == dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            if (dxmt9::d3d9::renderTapeArmSnapshotCompletionAction(false) !=
                dxmt9::d3d9::RenderTapeArmSnapshotCompletionAction::
                    RetainForNextInterval) {
                abortRenderTapeCapture("snapshot_completion_policy");
                return;
            }
            renderTapeExpectedDigest_.reset();
            renderTapeExpectedPixels_.clear();
            renderTapeExpectedSourcePixels_.clear();
            return;
        }
        D9CRenderTapeIdentityCaptureResult identityResult{};
        if (renderTapeActiveCaptureToken_ == 0u ||
            FAILED(hr32(dxmt9c_device_finish_render_tape_identity_capture(
                dev_, renderTapeActiveCaptureToken_, &identityResult,
                nullptr, 0u))) ||
            identityResult.status !=
                D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE ||
            identityResult.captureToken != renderTapeActiveCaptureToken_ ||
            identityResult.sourceCount == 0u ||
            identityResult.rangeCount == 0u ||
            identityResult.reserved0 != 0u ||
            identityResult.byteCount >
                std::numeric_limits<std::size_t>::max()) {
            abortRenderTapeCapture("identity_query");
            return;
        }
        std::vector<std::byte> identityBytes;
        try {
            identityBytes.resize(
                static_cast<std::size_t>(identityResult.byteCount));
        } catch (...) {
            abortRenderTapeCapture("identity_allocation");
            return;
        }
        D9CRenderTapeIdentityCaptureResult copiedIdentity{};
        if (FAILED(hr32(dxmt9c_device_finish_render_tape_identity_capture(
                dev_, renderTapeActiveCaptureToken_, &copiedIdentity,
                identityBytes.data(), identityBytes.size()))) ||
            copiedIdentity.status !=
                D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE ||
            copiedIdentity.sourceCount != identityResult.sourceCount ||
            copiedIdentity.rangeCount != identityResult.rangeCount ||
            copiedIdentity.captureToken != identityResult.captureToken ||
            copiedIdentity.byteCount != identityResult.byteCount) {
            abortRenderTapeCapture("identity_copy");
            return;
        }
        if (static_cast<std::size_t>(copiedIdentity.sourceCount) >
                std::numeric_limits<std::size_t>::max() /
                    sizeof(D9CRenderTapeIdentitySourceEntry) ||
            static_cast<std::size_t>(copiedIdentity.rangeCount) >
                std::numeric_limits<std::size_t>::max() /
                    sizeof(D9CRenderTapeIdentityRangeEntry)) {
            abortRenderTapeCapture("identity_layout");
            return;
        }
        const std::size_t sourceBytes =
            static_cast<std::size_t>(copiedIdentity.sourceCount) *
            sizeof(D9CRenderTapeIdentitySourceEntry);
        const std::size_t rangeBytes =
            static_cast<std::size_t>(copiedIdentity.rangeCount) *
            sizeof(D9CRenderTapeIdentityRangeEntry);
        if (sourceBytes > identityBytes.size() ||
            rangeBytes != identityBytes.size() - sourceBytes) {
            abortRenderTapeCapture("identity_layout");
            return;
        }
        std::vector<dxmt9::d3d9::RenderTapeIdentitySource> identitySources;
        std::vector<dxmt9::d3d9::RenderTapeIdentityRange> identityRanges;
        try {
            identitySources.resize(copiedIdentity.sourceCount);
            identityRanges.resize(copiedIdentity.rangeCount);
        } catch (...) {
            abortRenderTapeCapture("identity_allocation");
            return;
        }
        static_assert(sizeof(D9CRenderTapeIdentitySourceEntry) ==
                      sizeof(dxmt9::d3d9::RenderTapeIdentitySource));
        static_assert(sizeof(D9CRenderTapeIdentityRangeEntry) ==
                      sizeof(dxmt9::d3d9::RenderTapeIdentityRange));
        std::memcpy(identitySources.data(), identityBytes.data(), sourceBytes);
        std::memcpy(identityRanges.data(), identityBytes.data() + sourceBytes,
                    rangeBytes);
        if (renderTapeCapture_->attachCaptureIdentity(
                renderTapeActiveCaptureToken_, capturedPresentOrdinal,
                identitySources, identityRanges) !=
            dxmt9::d3d9::RenderTapeCaptureStatus::Complete) {
            const auto& identityValidation =
                renderTapeCapture_->identityValidationResult();
            dxmt9DeviceInfoLog(
                "render_tape_capture identity_attach_failure status=%u name=%s "
                "failed_source=%u failed_range=%u sources=%zu ranges=%zu",
                static_cast<unsigned>(identityValidation.status),
                dxmt9::d3d9::renderTapeIdentityStatusName(
                    identityValidation.status),
                identityValidation.failedSource,
                identityValidation.failedRange, identitySources.size(),
                identityRanges.size());
            abortRenderTapeCapture("identity_attach");
            return;
        }
        auto publisher = dxmt9PeRenderTapeArtifactPublisher.load(
            std::memory_order_acquire);
        if (!publisher) {
            publisher = dxmt9PeDefaultRenderTapeArtifactPublisher();
        }
        const bool published =
            publisher && publisher(renderTapeCapture_->publicationBundle());
        dxmt9DeviceInfoLog("render_tape_capture publication published=%d",
                           published ? 1 : 0);
        if (!published) {
            abortRenderTapeCapture("publication");
        }
        if (dxmt9::d3d9::renderTapeArmSnapshotCompletionAction(true) ==
            dxmt9::d3d9::RenderTapeArmSnapshotCompletionAction::Clear) {
            renderTapeArmSnapshots_.clear();
        }
        renderTapeExpectedDigest_.reset();
        renderTapeExpectedPixels_.clear();
        renderTapeExpectedSourcePixels_.clear();
        renderTapeActiveCaptureToken_ = 0u;
    }

    HRESULT commitPendingCommandChunk(PeRecorderFlushReason commitReason,
                                      const D9CCommandChunk& chunk,
                                      const PeCommandChunkCommitInfo& info) {
                const bool capturePresent = renderTapeCapture_ &&
                    renderTapeCapture_->state() ==
                        dxmt9::d3d9::RenderTapeCaptureState::Capturing &&
                    chunkHasPresentRecord(chunk);
                bool presentMirrorReserved = false;
                if (capturePresent) {
                    const HRESULT reserveHr = hr32(
                        dxmt9c_device_reserve_render_tape_present_capture(dev_));
                    if (SUCCEEDED(reserveHr)) {
                        presentMirrorReserved = true;
                    } else {
                        abortRenderTapeCapture("present_output_reserve");
                    }
                }
                auto chunkCadence = claimPeFirstChunkAfterPresent();
                const std::int64_t entryNs =
                    dxmt9SteadyClockNs(std::chrono::steady_clock::now());
                const std::int64_t priorReturnNs = peRecorderLastChunkReturnNs_;
                const bool captureWasActive = renderTapeCapture_ &&
                    renderTapeCapture_->state() ==
                        dxmt9::d3d9::RenderTapeCaptureState::Capturing;
                const bool captureChunkPrepared = captureWasActive &&
                    prepareRenderTapeChunkCapture(chunk, info);
                D9CCommandChunk submittedChunk = chunk;
                if (captureChunkPrepared && renderTapeCapture_ &&
                    renderTapeCapture_->state() ==
                        dxmt9::d3d9::RenderTapeCaptureState::Capturing &&
                    renderTapeActiveCaptureToken_ != 0u) {
                    submittedChunk.renderTapeCaptureToken =
                        renderTapeActiveCaptureToken_;
                    submittedChunk.renderTapeEventOrdinal =
                        static_cast<std::uint64_t>(
                            renderTapeCapture_->eventCount()) + 1u;
                }
                const HRESULT hr = hr32(
                    dxmt9c_device_commit_chunk(dev_, &submittedChunk));
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
                    // Copy the exact sealed canonical bytes only after the
                    // bridge accepted them. The source remains valid until
                    // flushPendingCommandChunk resets its builder below.
                    if (captureChunkPrepared) {
                        captureCommittedRenderTapeChunk(chunk, info);
                    } else if (!captureWasActive) {
                        observeRenderTapeFirstAccessChunk(chunk, info);
                    }
                } else if (renderTapeCapture_ &&
                           renderTapeCapture_->state() ==
                               dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
                    abortRenderTapeCapture("bridge_commit");
                }
                if (presentMirrorReserved) {
                    if (FAILED(hr)) {
                        dxmt9c_device_cancel_render_tape_present_capture(dev_);
                    } else if (renderTapeCapture_ &&
                               renderTapeCapture_->state() ==
                                   dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
                        const auto outputBpp = renderTapeOutputDesc_
                            ? dxmt9::d3d9::renderTapeLinearBytesPerPixel(
                                  renderTapeOutputDesc_->format)
                            : 0u;
                        const bool outputExtentFits = renderTapeOutputDesc_ &&
                            outputBpp != 0u &&
                            renderTapeOutputDesc_->height != 0u &&
                            renderTapeOutputDesc_->width <=
                                std::numeric_limits<std::uint64_t>::max() /
                                    renderTapeOutputDesc_->height / outputBpp;
                        const auto outputBytes = outputExtentFits
                            ? static_cast<std::uint64_t>(
                                  renderTapeOutputDesc_->width) *
                                  renderTapeOutputDesc_->height * outputBpp
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
                            output.width == renderTapeOutputDesc_->width &&
                            output.height == renderTapeOutputDesc_->height &&
                            output.format == renderTapeOutputDesc_->format &&
                            output.byteCount == outputBytes &&
                            SUCCEEDED(sourceFinishHr) &&
                            source.status ==
                                D9C_RENDER_TAPE_PRESENT_SOURCE_CAPTURE_COMPLETE &&
                            source.width == renderTapeOutputDesc_->width &&
                            source.height == renderTapeOutputDesc_->height &&
                            source.format == renderTapeOutputDesc_->format &&
                            source.byteCount == outputBytes &&
                            sourceDigestMatches) {
                            dxmt9::d3d9::RenderTapeDigest digest{};
                            std::memcpy(digest.data(), output.sha256,
                                        digest.size());
                            renderTapeExpectedDigest_ = digest;
                            renderTapeExpectedPixels_ = std::move(outputPixels);
                            renderTapeExpectedSourcePixels_ =
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
            const bool recordDestroy =
                renderTapeCapture_ &&
                renderTapeCapture_->state() ==
                    dxmt9::d3d9::RenderTapeCaptureState::Capturing;
            // commitPendingCommandChunk has first copied/materialized the
            // command into the capture session. Only now may a deferred last
            // wrapper become an ObjectDestroy event. Bridge failure leaves
            // both the builder and pending refs intact for retry.
            drainPendingRenderTapeChunk(recordDestroy);
            commandChunk_.reset();
        }
        return hr;
    }

    // Phase timer handed to an appendRecord emitter. The envelope owns the
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
                peChunkAppendDecimatedStats_, decimationN)) {
            appendDecimatedScope.stats = &peChunkAppendDecimatedStats_;
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
            // The emitter records peAppendPhaseEncode_ itself around the direct
            // canonical builder append. Timing the whole callable here would include
            // context/retention preparation and silently redefine `encode`,
            // the figure the migration is measured against -- see
            // docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.01.md.
            hr = emit(commandChunk_, AppendPhaseTimer{phaseSampled});
        }
        if (SUCCEEDED(hr) &&
            (commandChunk_.recordCount() >= maxRecords ||
             commandChunk_.payloadBytes() >= maxBytes)) {
            const auto t0 = phaseNow();
            hr = flushPendingCommandChunk(PeRecorderFlushReason::CapacityPost);
            phaseRecord(peAppendPhaseFlush_, t0);
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
        Dxmt9PeAppendFamilyScope appendFamily(PeInterAppendCallFamily::Draw);
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
        // Under inlineConstDelta the const shadows are still dirty here and the
        // producer drains them into the record's constant-range sections.
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
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendSparseRecord(
                    builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, peSparseHeader_,
                    peSparseState_);
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
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
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendSparseRecord(
                    builder, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
                    peSparseHeader_, peSparseState_);
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        if (SUCCEEDED(hr)) {
            if (indexSectionEmitted) {
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
        Dxmt9PeAppendFamilyScope appendFamily(PeInterAppendCallFamily::Draw);
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
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendSparseRecord(
                    builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                    peSparseHeader_, peSparseState_);
                phase.record(peAppendPhaseEncode_, t0);
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
        Dxmt9PeAppendFamilyScope appendFamily(PeInterAppendCallFamily::Draw);
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
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendSparseRecord(
                    builder, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                    peSparseHeader_, peSparseState_);
                phase.record(peAppendPhaseEncode_, t0);
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
                                 const void* data, std::size_t elemSize) {
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
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendSetConstants(
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

    // Emit one record covering the pending merged dirty range, then clear it.
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
        // and updates the flush counters above.
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
        emitRun(shadow.dirtyStart, shadow.dirtyEnd);
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
        Dxmt9PeAppendFamilyScope appendFamily(PeInterAppendCallFamily::Barrier);
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
                    const auto t0 = AppendPhaseTimer::now();
                    const bool ok = dxmt9::d3d9::pe::appendApplyState(
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

    // One APPLY_STATE record carrying exactly one category's batch. The
    // sizeHint stays kLegacyApplyStateSizeHint, which is what the
    // capacity precheck saw when this drained legacy records, so seal cadence is
    // unchanged. flags stay 0: these are batches, never snapshots.
    template <typename Fill>
    HRESULT appendSingleCategoryApplyState(Fill fill) {
        peSparseState_ = dxmt9::d3d9::pe::SparseStateInput{};
        fill();
        return appendRecord(
            D9C_COMMAND_RECORD_APPLY_STATE,
            kLegacyApplyStateSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendApplyState(
                    builder, /*flags=*/0u, peSparseState_);
                phase.record(peAppendPhaseEncode_, t0);
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
        // Section order is ascending by construction: every popFirst walks its
        // bitmap from the lowest set bit and its rows in order. appendPlainSection
        // does not enforce ordering for these four categories, but emitting them
        // out of order would still be a change in wire shape for no reason.
        auto drainTable = [&](auto& pendingTable, std::uint32_t cap,
                              auto fillEntry) -> HRESULT {
            while (!pendingTable.empty()) {
                std::uint32_t n = 0;
                std::uint32_t key = 0;
                std::uint32_t value = 0;
                const HRESULT hr = appendSingleCategoryApplyState([&] {
                    while (n < cap && pendingTable.popFirst(key, value)) {
                        fillEntry(n, key, value);
                        ++n;
                    }
                });
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        auto drainMatrix = [&](auto& pendingTable, std::uint32_t cap,
                               auto fillEntry) -> HRESULT {
            while (!pendingTable.empty()) {
                std::uint32_t n = 0;
                std::uint32_t row = 0;
                std::uint32_t key = 0;
                std::uint32_t value = 0;
                const HRESULT hr = appendSingleCategoryApplyState([&] {
                    while (n < cap && pendingTable.popFirst(row, key, value)) {
                        fillEntry(n, row, key, value);
                        ++n;
                    }
                });
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        if (auto hr = drainTable(
                peState_.pendingRenderStates,
                (std::uint32_t)D9C_DRAW_PACKET_MAX_RENDER_STATES,
                [&](std::uint32_t i, std::uint32_t k, std::uint32_t v) {
                    peSparseScratch_.renderStates[i] =
                        D9CCommandChunkWireRenderState{k, v};
                    peSparseState_.renderStates =
                        std::span(peSparseScratch_.renderStates).first(i + 1u);
                });
            FAILED(hr)) return hr;
        if (auto hr = drainMatrix(
                peState_.pendingTss, (std::uint32_t)D9C_DRAW_PACKET_MAX_TSS,
                [&](std::uint32_t i, std::uint32_t row, std::uint32_t k,
                    std::uint32_t v) {
                    peSparseScratch_.textureStageStates[i] =
                        D9CDrawPacketTextureStageState{row, k, v};
                    peSparseState_.textureStageStates =
                        std::span(peSparseScratch_.textureStageStates)
                            .first(i + 1u);
                });
            FAILED(hr)) return hr;
        if (auto hr = drainMatrix(
                peState_.pendingSamplerStates,
                (std::uint32_t)D9C_DRAW_PACKET_MAX_SAMPLER,
                [&](std::uint32_t i, std::uint32_t row, std::uint32_t k,
                    std::uint32_t v) {
                    peSparseScratch_.samplerStates[i] =
                        D9CDrawPacketSamplerState{row, k, v};
                    peSparseState_.samplerStates =
                        std::span(peSparseScratch_.samplerStates).first(i + 1u);
                });
            FAILED(hr)) return hr;
        while (!peState_.pendingTransforms.empty()) {
            std::uint32_t n = 0;
            std::uint32_t key = 0;
            D9CMatrix value{};
            const HRESULT hr = appendSingleCategoryApplyState([&] {
                while (n < (std::uint32_t)D9C_DRAW_PACKET_MAX_TRANSFORMS &&
                       peState_.pendingTransforms.popFirst(key, value)) {
                    peSparseScratch_.transforms[n] =
                        D9CDrawPacketTransform{key, 0u, value};
                    peSparseState_.transforms =
                        std::span(peSparseScratch_.transforms).first(n + 1u);
                    ++n;
                }
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
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendApplyState(
                    builder, peSparseHeader_.flags, peSparseState_);
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
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
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        const bool referenced = commandChunk_.referencesObject(buffer);
        if (!referenced) {
            return S_OK;
        }
        return flushPeRecorder(PeRecorderFlushReason::Child);
    }
    void NotifyPeFirstCallAfterPresentForChild(
        const char* callName, const void* callerPc = nullptr) noexcept override {
        notePeDeviceCallAfterPresent(callName, callerPc);
    }
    D3D9PePresentCallSlot PushPeCallScopeForChild(
        const char* callName, const void* callerPc) noexcept override {
        return pushPeCallScope(callName, callerPc);
    }
    void NotifyPeCallScopeReturnForChild(D3D9PePresentCallSlot slot,
                                         const char* callName,
                                         HRESULT hr) noexcept override {
        notePeCallScopeReturn(slot, callName, hr);
    }
    void PopPeCallScopeForChild(D3D9PePresentCallSlot slot) noexcept override {
        popPeCallScope(slot);
    }
    void NotifyRenderTapeObjectDefineForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::span<const std::byte> descriptor,
        std::span<const std::byte> immutablePayload = {}) noexcept override {
        if (!renderTapeCapture_ ||
            renderTapeCapture_->state() !=
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            return;
        }
        try {
            dxmt9::d3d9::RenderTapeDigest digest{};
            std::uint64_t bytes = 0u;
            if (!immutablePayload.empty()) {
                const auto status = renderTapeCapture_->registerBlobBytes(
                    immutablePayload, &digest);
                if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                    abortRenderTapeCapture("object_define_blob");
                    return;
                }
                bytes = immutablePayload.size();
            }
            const auto descriptorKind =
                dxmt9::d3d9::renderTapeDescriptorKindForObject(
                    object.identity.kind);
            if (descriptorKind ==
                dxmt9::d3d9::RenderTapeDescriptorKind::Invalid) {
                markRenderTapeInvalidOnce("descriptor_kind_invalid", &object);
                abortRenderTapeCapture("object_define_descriptor_kind");
                return;
            }
            dxmt9::d3d9::RenderTapeObjectDefineDisposition disposition{};
            const auto status = renderTapeCapture_->objectDefine(
                object.identity, static_cast<std::uint32_t>(descriptorKind),
                descriptor, bytes, digest, 0u, 0u, &disposition);
            if (status == dxmt9::d3d9::RenderTapeCaptureStatus::Accepted &&
                disposition == dxmt9::d3d9::
                                   RenderTapeObjectDefineDisposition::
                                       IdempotentSurfaceAlias) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture object_define reason=%s kind=%u "
                    "generation=%u object_id=%llu descriptor=%zu",
                    dxmt9::d3d9::renderTapeObjectDefineDispositionName(
                        disposition),
                    object.identity.kind, object.identity.generation,
                    static_cast<unsigned long long>(object.identity.objectId),
                    descriptor.size());
            }
            if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture object_define rejected status=%u reason=%s "
                    "kind=%u generation=%u object_id=%llu descriptor=%zu "
                    "immutable=%llu",
                    static_cast<unsigned>(status),
                    dxmt9::d3d9::renderTapeObjectDefineDispositionName(
                        disposition),
                    object.identity.kind,
                    object.identity.generation,
                    static_cast<unsigned long long>(object.identity.objectId),
                    descriptor.size(),
                    static_cast<unsigned long long>(bytes));
                abortRenderTapeCapture("object_define");
            }
        } catch (...) {
            abortRenderTapeCapture("object_define_exception");
        }
    }
    bool IsRenderTapeCaptureActiveForChild() const noexcept override {
        return renderTapeCapture_ &&
               renderTapeCapture_->state() ==
                   dxmt9::d3d9::RenderTapeCaptureState::Capturing;
    }
    bool IsRenderTapeCaptureTrackingEnabledForChild() const noexcept override {
        return renderTapeRegistry_.has_value();
    }
    void AbortRenderTapeCaptureForChild() noexcept override {
        markRenderTapeInvalidOnce("child_abort");
        if (IsRenderTapeCaptureActiveForChild())
            abortRenderTapeCapture("child_abort");
    }
    void RejectRenderTapeCaptureForChild(
        dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic)
        noexcept override {
        const char *name =
            dxmt9::d3d9::renderTapeCaptureRejectionReasonName(reason);
        const bool first = renderTapeRegistry_ && !renderTapeRegistry_->invalid;
        markRenderTapeInvalidOnce(name, &object, subresource, diagnostic);
        if (first) {
            dxmt9DeviceInfoLog(
                "render_tape_capture first_rejection reason=%s kind=%u "
                "generation=%u object_id=%llu subresource=%u format=%u "
                "width=%u height=%u pitch=%d bytes=%llu",
                name, object.identity.kind, object.identity.generation,
                static_cast<unsigned long long>(object.identity.objectId),
                subresource, diagnostic.format, diagnostic.width,
                diagnostic.height, diagnostic.pitch,
                static_cast<unsigned long long>(diagnostic.bytes));
        }
        if (IsRenderTapeCaptureActiveForChild())
            abortRenderTapeCapture(name);
    }
    dxmt9::d3d9::RenderTapeFullSnapshotStatus
    RenderTapeFullSnapshotStatusForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource, std::uint32_t fullRowBytes,
        std::uint32_t fullRows, std::uint64_t fullBytes) const noexcept override {
        using Status = dxmt9::d3d9::RenderTapeFullSnapshotStatus;
        if (!renderTapeRegistry_ ||
            (object.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE &&
             object.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER)) {
            return Status::NotRequired;
        }
        const auto *entry = findRenderTapeObject(object);
        if (!entry || subresource >= entry->content.size()) {
            return Status::InvalidIdentity;
        }
        if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
            if (entry->descriptor.size() != sizeof(D9CBufferDesc) ||
                fullRowBytes == 0u || fullRows != 1u ||
                fullBytes != fullRowBytes) {
                return Status::InvalidExtent;
            }
            D9CBufferDesc desc{};
            std::memcpy(&desc, entry->descriptor.data(), sizeof(desc));
            if (fullRowBytes != desc.size || fullBytes != desc.size) {
                return Status::InvalidExtent;
            }
            return dxmt9::d3d9::renderTapeClassifyBufferSnapshot(
                true, true, true, true, entry->content[subresource].size(),
                desc.size);
        }
        D9CSurfaceDesc desc{};
        if (!renderTapeObjectSubresourceDesc(*entry, object, subresource, desc))
            return Status::InvalidIdentity;
        if (fullRowBytes == 0u || fullRows == 0u || fullBytes == 0u)
            return Status::InvalidExtent;
        std::uint64_t expectedBytes = 0u;
        if (renderTapeFormatIsBlockCompressed(desc.format)) {
            dxmt9::d3d9::RenderTapeBlockLockLayout expected{};
            if (dxmt9::d3d9::renderTapeBlockLockLayout(
                    desc, static_cast<std::int32_t>(fullRowBytes), nullptr,
                    expected) !=
                    dxmt9::d3d9::RenderTapeBlockLayoutStatus::Accepted ||
                !expected.fullSubresource ||
                expected.fullRowBytes != fullRowBytes ||
                expected.fullRows != fullRows) {
                return Status::InvalidExtent;
            }
            expectedBytes = expected.tightBytes;
        } else {
            dxmt9::d3d9::RenderTapeLinearLockLayout expected{};
            if (dxmt9::d3d9::renderTapeLinearLockLayout(
                    desc, static_cast<std::int32_t>(fullRowBytes), nullptr,
                    expected) !=
                    dxmt9::d3d9::RenderTapeLinearLayoutStatus::Accepted ||
                !expected.fullSubresource ||
                expected.fullRowBytes != fullRowBytes ||
                expected.fullRows != fullRows) {
                return Status::InvalidExtent;
            }
            expectedBytes = expected.tightBytes;
        }
        if (expectedBytes != fullBytes)
            return Status::InvalidExtent;
        const auto &content = entry->content[subresource];
        return dxmt9::d3d9::renderTapeClassifySnapshot(
            true, true, true, true, content.size(), expectedBytes);
    }
    void NotifyRenderTapeBlockMutationForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
        std::span<const std::byte> bytes) noexcept override {
        const auto status =
            recordRenderTapeBlockBytes(object, subresource, layout, bytes);
        if (status !=
            dxmt9::d3d9::RenderTapeBlockMutationStatus::Accepted) {
            if (status == dxmt9::d3d9::RenderTapeBlockMutationStatus::IncompleteSeed &&
                !renderTapeObjectAdmitted(object.identity)) {
                return;
            }
            const auto reason =
                status == dxmt9::d3d9::
                              RenderTapeBlockMutationStatus::IncompleteSeed
                    ? dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                          IncompleteSubresourceSeed
                    : dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                          DescriptorMismatch;
            D9CSurfaceDesc desc{};
            const auto *entry = findRenderTapeObject(object);
            if (entry && subresource < entry->content.size()) {
                (void)renderTapeObjectSubresourceDesc(
                    *entry, object, subresource, desc);
            }
            logRenderTapeMutationFailure(
                "block", status, object, subresource, layout.fullRowBytes,
                layout.fullRows, layout.rowBytes, layout.rows, layout.pitch,
                bytes);
            RejectRenderTapeCaptureForChild(
                reason, object, subresource,
                {.format = desc.format,
                 .width = desc.width,
                 .height = desc.height,
                 .pitch = static_cast<std::int32_t>(layout.pitch),
                 .bytes = bytes.size()});
            return;
        }
        if (!IsRenderTapeCaptureActiveForChild())
            return;
        if (!renderTapeObjectAdmitted(object.identity))
            return;
        if (!appendRenderTapeUnlockMutation(object, subresource,
                                            "block_resource_mutation")) {
            abortRenderTapeCapture("block_resource_mutation");
        }
    }
    void NotifyRenderTapeLinearMutationForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::uint32_t subresource,
        const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
        std::span<const std::byte> bytes) noexcept override {
        const auto status =
            recordRenderTapeLinearBytes(object, subresource, layout, bytes);
        if (status != dxmt9::d3d9::RenderTapeBlockMutationStatus::Accepted) {
            if (status == dxmt9::d3d9::RenderTapeBlockMutationStatus::IncompleteSeed &&
                !renderTapeObjectAdmitted(object.identity)) {
                return;
            }
            const auto reason =
                status == dxmt9::d3d9::RenderTapeBlockMutationStatus::IncompleteSeed
                    ? dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                          IncompleteSubresourceSeed
                    : dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                          DescriptorMismatch;
            D9CSurfaceDesc desc{};
            const auto *entry = findRenderTapeObject(object);
            if (entry && subresource < entry->content.size()) {
                (void)renderTapeObjectSubresourceDesc(
                    *entry, object, subresource, desc);
            }
            logRenderTapeMutationFailure(
                "linear", status, object, subresource, layout.fullRowBytes,
                layout.fullRows, layout.rowBytes, layout.rows, layout.pitch,
                bytes);
            RejectRenderTapeCaptureForChild(
                reason, object, subresource,
                {.format = desc.format,
                 .width = desc.width,
                 .height = desc.height,
                 .pitch = static_cast<std::int32_t>(layout.pitch),
                 .bytes = bytes.size()});
            return;
        }
        if (!IsRenderTapeCaptureActiveForChild())
            return;
        if (!renderTapeObjectAdmitted(object.identity))
            return;
        if (!appendRenderTapeUnlockMutation(object, subresource,
                                            "linear_resource_mutation")) {
            abortRenderTapeCapture("linear_resource_mutation");
        }
    }

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
        const dxmt9::d3d9::pe::PeWireObjectRef &destination) noexcept {
        if (!renderTapeRegistry_ ||
            source.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE ||
            destination.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE) {
            return;
        }
        if (renderTapeSameIdentity(source.identity, destination.identity)) {
            if (IsRenderTapeCaptureActiveForChild())
                abortRenderTapeCapture("update_texture_self_copy");
            return;
        }
        auto *sourceEntry = findRenderTapeObject(source);
        auto *destinationEntry = findRenderTapeObject(destination);
        const bool active = IsRenderTapeCaptureActiveForChild();
        if (!sourceEntry || !destinationEntry) {
            if (active)
                abortRenderTapeCapture("update_texture_registry_missing");
            return;
        }
        const bool destinationAdmitted =
            renderTapeObjectAdmitted(destination.identity);
        if (active && !destinationAdmitted) {
            // The destination's pre-copy bytes are not in the session. Keep
            // this interval fail-closed rather than defining it with the
            // post-copy source bytes before the UpdateTexture command.
            abortRenderTapeCapture("update_texture_destination_unadmitted");
        }
        const auto status = dxmt9::d3d9::applyRenderTapeUpdateTextureClosure(
            sourceEntry->descriptor, sourceEntry->content,
            destinationEntry->descriptor, destinationEntry->content);
        if (status == dxmt9::d3d9::RenderTapeUpdateTextureStatus::Accepted)
            return;
        const auto reason =
            status == dxmt9::d3d9::RenderTapeUpdateTextureStatus::IncompleteSource
                ? dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                      IncompleteSubresourceSeed
                : dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                      DescriptorMismatch;
        if (active)
            abortRenderTapeCapture(
                reason == dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                              IncompleteSubresourceSeed
                    ? "update_texture_source_incomplete"
                    : "update_texture_descriptor_mismatch");
        dxmt9DeviceInfoLog(
            "render_tape_capture update_texture_closure status=%u active=%d "
            "source_generation=%u source_object_id=%llu "
            "destination_generation=%u destination_object_id=%llu",
            static_cast<unsigned>(status), active ? 1 : 0,
            source.identity.generation,
            static_cast<unsigned long long>(source.identity.objectId),
            destination.identity.generation,
            static_cast<unsigned long long>(destination.identity.objectId));
    }

    void NotifyRenderTapeSurfaceAliasForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &surface,
        const dxmt9::d3d9::pe::PeWireObjectRef &parentTexture,
        std::uint32_t subresource,
        const D9CSurfaceDesc &descriptor) noexcept override {
        if (!IsRenderTapeCaptureTrackingEnabledForChild())
            return;
        const dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 alias{
            .schemaVersion =
                dxmt9::d3d9::kRenderTapeSurfaceDescriptorVersion2,
            .storage = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeSurfaceStorage::TextureSubresource),
            .initialContentDisposition = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeInitialContentDisposition::Unavailable),
            .subresource = subresource,
            .parentTexture = parentTexture.identity,
            .surface = descriptor,
        };
        notifyRenderTapeCreatedObject(
            surface,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(&alias), sizeof(alias)));
    }
    void NotifyRenderTapeStandaloneSurfaceForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &surface,
        const D9CSurfaceDesc &descriptor) noexcept override {
        if (!IsRenderTapeCaptureTrackingEnabledForChild())
            return;
        const dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 standalone{
            .schemaVersion =
                dxmt9::d3d9::kRenderTapeSurfaceDescriptorVersion2,
            .storage = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone),
            .initialContentDisposition = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeInitialContentDisposition::CompleteSeed),
            .surface = descriptor,
        };
        notifyRenderTapeCreatedObject(
            surface,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(&standalone),
                sizeof(standalone)));
    }

    bool retireRenderTapeObject(
        const D9CWireObjectIdentity &identity, bool recordDestroy,
        const char *failureReason) noexcept {
        if (!renderTapeRegistry_)
            return false;
        const auto it = std::find_if(
            renderTapeRegistry_->objects.begin(),
            renderTapeRegistry_->objects.end(), [&](const auto &entry) {
                return renderTapeSameIdentity(entry.identity, identity);
            });
        if (it == renderTapeRegistry_->objects.end())
            return false;
        const bool admitted = renderTapeObjectAdmitted(identity);
        if (recordDestroy && admitted && renderTapeCapture_ &&
            renderTapeCapture_->state() ==
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            const auto status = renderTapeCapture_->objectDestroy(identity);
            dxmt9DeviceInfoLog(
                "render_tape_capture object_destroy status=%u kind=%u "
                "generation=%u object_id=%llu",
                static_cast<unsigned>(status), identity.kind,
                identity.generation,
                static_cast<unsigned long long>(identity.objectId));
            if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                abortRenderTapeCapture(failureReason);
            } else {
                removeRenderTapeObjectAdmitted(identity);
            }
        }
        try {
            renderTapeRegistry_->knownDead.push_back(identity);
        } catch (...) {
            const dxmt9::d3d9::pe::PeWireObjectRef object{.identity = identity};
            markRenderTapeInvalidOnce("object_destroy_tombstone_allocation",
                                     &object);
            return false;
        }
        renderTapeRegistry_->objects.erase(it);
        return true;
    }

    void retireRenderTapeAliasesForParent(
        const D9CWireObjectIdentity &parent, bool recordDestroy = true) noexcept {
        if (!renderTapeRegistry_) {
            return;
        }
        for (auto it = renderTapeRegistry_->objects.begin();
             it != renderTapeRegistry_->objects.end();) {
            if (!it->lifetime.textureAlias ||
                !renderTapeSameIdentity(it->aliasParentTexture, parent)) {
                ++it;
                continue;
            }
            if (it->lifetime.wrapperRefs != 0u) {
                const dxmt9::d3d9::pe::PeWireObjectRef aliasObject{
                    .identity = it->identity};
                markRenderTapeInvalidOnce("alias_parent_destroy_live_wrapper",
                                         &aliasObject);
                if (IsRenderTapeCaptureActiveForChild()) {
                    abortRenderTapeCapture("alias_parent_destroy_live_wrapper");
                }
                ++it;
                continue;
            }
            const auto identity = it->identity;
            if (!it->lifetime.retireParent()) {
                ++it;
                continue;
            }
            (void)retireRenderTapeObject(identity, recordDestroy,
                                          "alias_object_destroy");
            // The helper erases the identity. Restarting from a value lookup
            // keeps iterator invalidation out of this bounded registry walk.
            it = renderTapeRegistry_->objects.begin();
        }
    }

    void drainPendingRenderTapeChunk(bool recordDestroy) noexcept {
        if (!renderTapeRegistry_)
            return;
        // Handles are intentionally walked after the command has been
        // materialized. Duplicate handles across records are harmless because
        // the bounded lifetime ref reaches zero on the first visit.
        for (const auto &handle : commandChunk_.handles()) {
            const D9CWireObjectIdentity identity{
                .kind = handle.kind,
                .generation = handle.generation,
                .objectId = handle.objectId,
            };
            auto *entry = findRenderTapeObject(
                dxmt9::d3d9::pe::PeWireObjectRef{.identity = identity});
            if (!entry || entry->lifetime.pendingChunkRefs == 0u)
                continue;
            if (!entry->lifetime.releasePendingChunk())
                continue;
            const bool isTexture = identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE;
            if (isTexture)
                retireRenderTapeAliasesForParent(identity, recordDestroy);
            // Preserve the established alias-before-parent event order. The
            // parent entry remains in the registry until the alias scan has
            // completed, so the scan is safe for both immediate and pending
            // retirement.
            retireRenderTapeObject(identity, recordDestroy, "object_destroy");
        }
    }

    void NotifyRenderTapeObjectDestroyForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept override {
        if (renderTapeRegistry_) {
            auto *entry = findRenderTapeObject(object);
            // The PE wrapper destructor has already delivered this callback.
            // Transfer the logical lifetime to the bounded pending chunk ref;
            // drain it after command materialization and before raw D9C
            // retainer reset.
            if (entry && entry->lifetime.wrapperRefs == 1u &&
                entry->lifetime.pendingChunkRefs == 0u &&
                commandChunk_.referencesObject(object.object) &&
                entry->lifetime.retainPendingChunk()) {
                (void)entry->lifetime.releaseWrapper();
                dxmt9DeviceInfoLog(
                    "render_tape_capture object_destroy deferred kind=%u "
                    "generation=%u object_id=%llu pending=%u",
                    object.identity.kind, object.identity.generation,
                    static_cast<unsigned long long>(object.identity.objectId),
                    entry->lifetime.pendingChunkRefs);
                return;
            }
        }
        const bool retired = unregisterRenderTapeObject(object);
        if (!retired) {
            return;
        }
        if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
            retireRenderTapeAliasesForParent(object.identity);
        }
        retireRenderTapeObject(object.identity, true, "object_destroy");
    }
    void NotifyRenderTapeResourceMutationForChild(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        dxmt9::d3d9::RenderTapeMutationKind kind, std::uint32_t subresource,
        std::uint64_t byteOffset,
        std::span<const std::byte> bytes,
        dxmt9::d3d9::RenderTapeBufferMutationDisposition bufferDisposition)
        noexcept override {
        const bool registryAccepted =
            recordRenderTapeCpuBytes(object, subresource, byteOffset, bytes);
        if (!registryAccepted) {
            if (IsRenderTapeCaptureActiveForChild())
                abortRenderTapeCapture("resource_mutation_registry");
            return;
        }
        if (!renderTapeCapture_ ||
            renderTapeCapture_->state() !=
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            return;
        }
        if (!renderTapeObjectAdmitted(object.identity))
            return;
        try {
            if (renderTapeCapture_->resourceMutationBytes(
                    object.identity, kind, subresource, byteOffset, bytes,
                    bufferDisposition) !=
                dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                abortRenderTapeCapture("resource_mutation");
            }
        } catch (...) {
            abortRenderTapeCapture("resource_mutation_exception");
        }
    }
    void NotifyRenderTapeOrderedControlForChild(
        const dxmt9::d3d9::RenderTapeOrderedControlHeader &fixed,
        std::span<const std::byte> payload) noexcept override {
        if (!renderTapeCapture_ ||
            renderTapeCapture_->state() !=
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            return;
        }
        if (fixed.identity.objectId != 0u &&
            !materializeRenderTapeObjectForReference(fixed.identity)) {
            return;
        }
        auto recorded = fixed;
        recorded.completionOrdinal = ++renderTapeCompletionOrdinal_;
        if (renderTapeCapture_->orderedControl(recorded, payload) !=
            dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            abortRenderTapeCapture("ordered_control");
        }
    }
    void notifyRenderTapeCreatedObject(
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::span<const std::byte> descriptor,
        std::span<const std::byte> immutablePayload = {}) noexcept {
        if (!IsRenderTapeCaptureTrackingEnabledForChild())
            return;
        if (!object.valid(object.identity.kind) || descriptor.empty()) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        const auto registration =
            registerRenderTapeObject(object, descriptor, immutablePayload);
        if (registration != RenderTapeObjectRegistration::New) {
            // A repeated COM wrapper for the same underlying identity is a
            // lifetime alias, not a second tape ObjectDefine. Conflicting
            // descriptors are already recorded as a registry rejection.
            return;
        }
        // A newly created frame-tape identity is materialized immediately
        // before its first command/control reference. That cold JIT point can
        // require complete seeds and emit the exact descriptor plus mutations
        // transactionally; creation alone cannot claim initialized bytes.
        // Sequence-tape deliberately rejects such post-arm identities.
    }
    void notifyRenderTapeCreatedBuffer(
        D9CBuffer *buffer,
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
        if (!IsRenderTapeCaptureTrackingEnabledForChild())
            return;
        D9CBufferDesc descriptor{};
        if (!buffer || FAILED(hr32(dxmt9c_buffer_get_desc(buffer, &descriptor)))) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        notifyRenderTapeCreatedObject(
            object,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(&descriptor),
                sizeof(descriptor)));
    }
    void notifyRenderTapeCreatedTexture(
        D9CTexture *texture,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        dxmt9::d3d9::RenderTapeTextureDimension dimension) noexcept {
        if (!IsRenderTapeCaptureTrackingEnabledForChild())
            return;
        if (!texture) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        const std::uint32_t mipLevelCount =
            dxmt9c_texture_get_level_count(texture);
        if (mipLevelCount == 0u) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        std::uint32_t subresourceCount = mipLevelCount;
        if (dimension == dxmt9::d3d9::RenderTapeTextureDimension::Cube) {
            if (mipLevelCount > std::numeric_limits<std::uint32_t>::max() / 6u) {
                AbortRenderTapeCaptureForChild();
                return;
            }
            subresourceCount = mipLevelCount * 6u;
        }
        if (!renderTapeDescriptorSubresourceCountFits(
                subresourceCount, sizeof(RenderTapeTextureDescriptorV2))) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        const RenderTapeTextureDescriptorV2 descriptor{
            .schemaVersion =
                dxmt9::d3d9::kRenderTapeTextureDescriptorVersion2,
            .dimension = static_cast<std::uint32_t>(dimension),
            .mipLevelCount = mipLevelCount,
            .subresourceCount = subresourceCount,
            .initialContentDisposition = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeInitialContentDisposition::CompleteSeed),
        };
        std::vector<std::byte> descriptorBytes;
        try {
            descriptorBytes.resize(
                sizeof(descriptor) +
                static_cast<std::size_t>(subresourceCount) *
                    sizeof(D9CSurfaceDesc));
        } catch (...) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        std::memcpy(descriptorBytes.data(), &descriptor, sizeof(descriptor));
        for (std::uint32_t subresource = 0u;
             subresource < subresourceCount; ++subresource) {
            D9CSurfaceDesc subresourceDesc{};
            const auto mipLevel =
                dxmt9::d3d9::renderTapeTextureDescriptorMipLevel(
                    dimension, mipLevelCount, subresource);
            if (FAILED(hr32(dxmt9c_texture_get_level_desc(
                    texture, mipLevel, &subresourceDesc)))) {
                AbortRenderTapeCaptureForChild();
                return;
            }
            std::memcpy(descriptorBytes.data() + sizeof(descriptor) +
                            static_cast<std::size_t>(subresource) *
                                sizeof(D9CSurfaceDesc),
                        &subresourceDesc, sizeof(subresourceDesc));
        }
        notifyRenderTapeCreatedObject(
            object,
            descriptorBytes);
    }
    void notifyRenderTapeCreatedVertexDecl(
        D9CVertexDecl *decl,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        std::span<const std::byte> elements, std::size_t elementCount) noexcept {
        if (!IsRenderTapeCaptureTrackingEnabledForChild())
            return;
        if (!decl || elements.empty() ||
            elementCount > std::numeric_limits<uint32_t>::max() ||
            elements.size() > std::numeric_limits<uint32_t>::max()) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        const RenderTapeVertexDeclDescriptor descriptor{
            static_cast<uint32_t>(elementCount),
            static_cast<uint32_t>(elements.size())};
        notifyRenderTapeCreatedObject(
            object,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(&descriptor),
                sizeof(descriptor)),
            elements);
    }
    void notifyRenderTapeCreatedQuery(
        D9CQuery *query,
        const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
        if (!IsRenderTapeCaptureTrackingEnabledForChild())
            return;
        if (!query) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        const RenderTapeQueryDescriptor descriptor{
            dxmt9c_query_get_type(query), dxmt9c_query_get_data_size(query)};
        if (descriptor.dataBytes == 0u) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        notifyRenderTapeCreatedObject(
            object,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(&descriptor),
                sizeof(descriptor)));
    }
    void notifyRenderTapeCreatedShader(
        D9CShader *shader,
        const dxmt9::d3d9::pe::PeWireObjectRef &object,
        uint32_t stage) noexcept {
        if (!IsRenderTapeCaptureTrackingEnabledForChild())
            return;
        if (!shader || !object.valid(object.identity.kind)) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        uint32_t bytecodeBytes = 0u;
        if (FAILED(hr32(dxmt9c_shader_get_bytecode(shader, nullptr,
                                                   &bytecodeBytes))) ||
            bytecodeBytes == 0u || bytecodeBytes % sizeof(uint32_t) != 0u) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        std::vector<std::byte> bytecode;
        try {
            bytecode.resize(bytecodeBytes);
        } catch (...) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        uint32_t availableBytes = bytecodeBytes;
        if (FAILED(hr32(dxmt9c_shader_get_bytecode(
                shader, bytecode.data(), &availableBytes))) ||
            availableBytes != bytecodeBytes) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        const RenderTapeShaderDescriptor descriptor{stage, bytecodeBytes};
        notifyRenderTapeCreatedObject(
            object,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(&descriptor),
                sizeof(descriptor)),
            bytecode);
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
        return appendRecord(
            D9C_COMMAND_RECORD_QUERY_ISSUE,
            kLegacyQueryIssueSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendQueryIssue(
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
        , recorderLockRequired_(dxmt9PeRecorderLockRequired(
              behaviorFlags, dxmt9PeForceRecorderLockEnabled()))
        // recorderOwnership_ binds to this thread through its default member
        // initializer, so it needs no entry here.
        , softwareVertexProcessing_((behaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? TRUE : FALSE)
        , extended_(extended)
        , renderTapeCapture_(
              std::in_place, dxmt9PeRenderTapeCaptureEnabled(),
              dxmt9PeRenderTapeCaptureLimits(
                  dxmt9PeRenderTapeCaptureEnabled()),
              dxmt9PeRenderTapeCaptureProfile())
        , renderTapeRegistry_(dxmt9PeRenderTapeCaptureEnabled()
                                  ? std::optional<RenderTapeLiveRegistry>{
                                        std::in_place}
                                  : std::nullopt)
        , renderTapeArmPresentSkipRemaining_(
              dxmt9PeRenderTapeCaptureSkipPresents())
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
        D9CSwapChain* chain = borrowSwapChainHandle(sc);
        if (!chain) {
            return D3DERR_INVALIDCALL;
        }
        D9CPresentParams cpp{};
        const HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(chain, &cpp));
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
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
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
        if (renderTapeCapture_ &&
            renderTapeCapture_->state() ==
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            const dxmt9::d3d9::RenderTapeResetControl payload{
                .reclaimedGeneration = 0u, .terminal = 1u};
            NotifyRenderTapeOrderedControlForChild(
                dxmt9::d3d9::RenderTapeOrderedControlHeader{
                    .kind = static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeControlKind::Reset),
                    .disposition = static_cast<std::uint32_t>(
                        SUCCEEDED(hr)
                            ? dxmt9::d3d9::RenderTapeControlDisposition::Terminal
                            : dxmt9::d3d9::RenderTapeControlDisposition::Failed),
                    .resultCode = static_cast<std::int32_t>(hr),
                    .controlBytes = sizeof(payload)},
                std::as_bytes(std::span(&payload, 1u)));
            abortRenderTapeCapture("reset");
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty) noexcept override {
        dxmt9PeSetCurrentCallName("Present");
        const bool recordPresentTiming = dxmt9PeRecorderStatsEnabled();
        const std::uint32_t presentThreadId = dxmt9PeCurrentThreadId();
        const auto presentTimingEnter = std::chrono::steady_clock::now();
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        const bool renderTapeCaptureWasActive =
            renderTapeCapture_ &&
            renderTapeCapture_->state() ==
                dxmt9::d3d9::RenderTapeCaptureState::Capturing;
        const auto presentTimingStart = std::chrono::steady_clock::now();
        auto presentTimingBarrierEnd = presentTimingStart;
        auto presentTimingAppendEnd = presentTimingStart;
        auto presentTimingFlushEnd = presentTimingStart;
        // T2 device-lost gate: render-path methods must early-return
        // D3DERR_DEVICELOST while the device awaits Reset.
        if (deviceNotReset_) {
            if (renderTapeCapture_)
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
        if (!presentSourceWire.valid(D9C_CHUNK_HANDLE_KIND_SURFACE)) {
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
                const auto t0 = AppendPhaseTimer::now();
                const bool ok =
                    dxmt9::d3d9::pe::appendPresent(
                        builder, presentWire, presentSourceWire);
                phase.record(peAppendPhaseEncode_, t0);
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
            } else {
                // The first successful Present is only the arm boundary;
                // the next Present owns the one captured interval.
                (void)armRenderTapeCaptureAtPresentBoundary();
            }
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
        D9CSwapChain* chain = borrowSwapChainHandle(sc);
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
                return D3DERR_INVALIDCALL;
            }
        }
        // The former dxmt9c_swapchain_get_back_buffer + surface_release probe
        // that stood here is gone: it allocated and immediately dropped a unix
        // surface handle purely to map "no such back buffer" to
        // D3DERR_INVALIDCALL, which the swap-chain wrapper's own
        // GetBackBuffer below does with the identical null check and the
        // identical HRESULT, without disturbing *ppS (already NULL).
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
        D9CSwapChain* chain = borrowSwapChainHandle(swapChain);
        if (chain) {
            D9CPresentParams cpp{};
            if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(chain, &cpp)))) {
                displayHeight = cpp.backBufferHeight;
            }
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
        const bool captureGamma =
            renderTapeCapture_ &&
            renderTapeCapture_->state() ==
                dxmt9::d3d9::RenderTapeCaptureState::Capturing;
        if (captureGamma) {
            // A direct state mutation is an ordering boundary. Seal any
            // pending draw/state chunk before journaling GammaRampSet so the
            // provider cannot apply the new LUT ahead of older work.
            const HRESULT flushHr =
                flushPeRecorder(PeRecorderFlushReason::Barrier);
            if (FAILED(flushHr)) {
                abortRenderTapeCapture("gamma_ramp_barrier");
            }
        }
        // Byte-copy: D3DGAMMARAMP is a POD (3 * 256 * WORD). sizeof
        // is the safe shape regardless of any future struct growth.
        std::memcpy(&gammaRamp_, ramp, sizeof(D3DGAMMARAMP));
        if (dev_) {
            dxmt9c_device_set_gamma_ramp(dev_, reinterpret_cast<const uint16_t*>(ramp));
        }
        if (captureGamma && renderTapeCapture_ &&
            renderTapeCapture_->state() ==
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            const auto *bytes = reinterpret_cast<const std::byte *>(ramp);
            NotifyRenderTapeOrderedControlForChild(
                dxmt9::d3d9::RenderTapeOrderedControlHeader{
                    .identity = {},
                    .kind = static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeControlKind::GammaRampSet),
                    .disposition = static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeControlDisposition::Completed),
                    .resultCode = 0,
                    .controlBytes = dxmt9::d3d9::kRenderTapeGammaRampBytes,
                },
                std::span<const std::byte>(
                    bytes, dxmt9::d3d9::kRenderTapeGammaRampBytes));
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
        const HRESULT levelHr = peTextureLevelCountHResult(std::min(w, h), std::max(w, h), levels);
        if (FAILED(levelHr)) return levelHr;
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
        notifyRenderTapeCreatedTexture(
            t, D3D9PeWireTexture(*ppTex),
            dxmt9::d3d9::RenderTapeTextureDimension::Texture2D);
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
        const HRESULT levelHr =
            peTextureLevelCountHResult(std::min({w, h, d}), std::max({w, h, d}), levels);
        if (FAILED(levelHr)) return levelHr;
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
        notifyRenderTapeCreatedTexture(
            t, D3D9PeWireTexture(*ppTex),
            dxmt9::d3d9::RenderTapeTextureDimension::Volume);
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
        const HRESULT levelHr = peTextureLevelCountHResult(size, size, levels);
        if (FAILED(levelHr)) return levelHr;
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
        notifyRenderTapeCreatedTexture(
            t, D3D9PeWireTexture(*ppTex),
            dxmt9::d3d9::RenderTapeTextureDimension::Cube);
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
        notifyRenderTapeCreatedBuffer(b, D3D9PeWireVertexBuffer(*ppBuf));
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
        notifyRenderTapeCreatedBuffer(b, D3D9PeWireIndexBuffer(*ppBuf));
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
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
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
        // Handle indices are assigned by appendUpdateSurface as it appends
        // the refs, so they stay zero here.
        const D9CCommandChunkWireUpdateSurface wire{
            .srcHandleIndex = 0u,
            .dstHandleIndex = 0u,
            .hasSrcRect = srcRect ? 1u : 0u,
            .hasDstPoint = dstPt ? 1u : 0u,
            .srcRect = cs,
            .dstPoint = cd,
        };
        return appendRecord(
            D9C_COMMAND_RECORD_UPDATE_SURFACE,
            kLegacyUpdateSurfaceSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendUpdateSurface(
                    builder, wire, D3D9PeWireSurface(src),
                    D3D9PeWireSurface(dst));
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
    }

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,
                                             IDirect3DBaseTexture9* dst) noexcept override {
        dxmt9PeSetCurrentCallName("UpdateTexture");
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
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
        const auto sourceWire = D3D9PeWireTexture(src);
        const auto destinationWire = D3D9PeWireTexture(dst);
        // Wine d3d9 UpdateTexture: both args non-NULL; src in SYSTEMMEM;
        // dst not SYSTEMMEM/SCRATCH. test_update_texture_pool_copy_2d.
        const HRESULT appendHr = appendRecord(
            D9C_COMMAND_RECORD_UPDATE_TEXTURE,
            kLegacyUpdateTextureSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendUpdateTexture(
                    builder, sourceWire, destinationWire);
                phase.record(peAppendPhaseEncode_, t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        // The registry shadow is committed only after appendRecord accepts the
        // record. If the append's capacity flush or emitter fails, the
        // destination remains unchanged; the normal command bytes are also
        // left on their existing failure path.
        if (SUCCEEDED(appendHr)) {
            applyRenderTapeUpdateTextureClosure(sourceWire, destinationWire);
        }
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
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
        // Decomposed on purpose: the cadence claim above must be taken before
        // the recorder lock and the milestone log after it, so this site cannot
        // use notePeDeviceCallAfterPresent. Gating the pair keeps the sample
        // inside the branch, so the disabled path constructs nothing. When
        // tracking is off both calls were already no-ops -- the claim is
        // unclaimed and the milestone sample untracked -- so skipping them
        // changes no emission.
        if (dxmt9PeCallTrackingEnabled()) {
            const auto peCall =
                logPeCallMilestoneAfterPresent("GetRenderTargetData", callerPc);
            logPeFirstCallAfterPresent("GetRenderTargetData", peCadence,
                                       peCall);
        }
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
        const HRESULT appendHr = appendRecord(
            D9C_COMMAND_RECORD_READBACK, kLegacyReadbackSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendReadback(
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
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
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
        const D9CCommandChunkWireStretchRect wire{
            .srcHandleIndex = 0u,
            .dstHandleIndex = 0u,
            .hasSrcRect = srcRect ? 1u : 0u,
            .hasDstRect = dstRect ? 1u : 0u,
            .filter = (uint32_t)filter,
            .reserved0 = 0u,
            .srcRect = srcRect ? cs : D9CRect{},
            .dstRect = dstRect ? cd : D9CRect{},
        };
        return appendRecord(
            D9C_COMMAND_RECORD_STRETCH_RECT,
            kLegacyStretchRectSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendStretchRect(
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
        assertRecorderThreadConfined();
        PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
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
        const D9CCommandChunkWireColorFill wire{
            .surfaceHandleIndex = 0u,
            .colorARGB = (uint32_t)color,
            .hasRect = pRect ? 1u : 0u,
            .reserved0 = 0u,
            .rect = pRect ? cr : D9CRect{},
        };
        return appendRecord(
            D9C_COMMAND_RECORD_COLOR_FILL, kLegacyColorFillSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendColorFill(
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
        PeCallScope peCall(*this, "SetRenderTarget", DXMT9_PE_CALLSITE_PC());
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::RenderTarget);
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
    }

    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx,
                                               IDirect3DSurface9** ppS) noexcept override {
        PeCallScope peCall(*this, "GetRenderTarget", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("GetRenderTarget", hr);
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
        PeCallScope peCall(*this, "BeginScene", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("BeginScene", hr);
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_begin_scene device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_scene(dev_));
        dxmt9DeviceDebugLog("device_begin_scene -> hr=0x%08x", (unsigned)hr);
        return finishPeCall(hr);
    }
    HRESULT STDMETHODCALLTYPE EndScene()   noexcept override {
        PeCallScope peCall(*this, "EndScene", DXMT9_PE_CALLSITE_PC());
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
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* pRects,
                                     DWORD flags, D3DCOLOR color,
                                     float z, DWORD stencil) noexcept override {
        PeCallScope peCall(*this, "Clear", DXMT9_PE_CALLSITE_PC());
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
                const auto t0 = AppendPhaseTimer::now();
                const bool ok =
                    dxmt9::d3d9::pe::appendClear(builder, clearWire, rects);
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
        // server-side canonical state replay dispatches set_transform per
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
        // shadow snapshot; server-side canonical state replay dispatches
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
        PeCallScope peCall(*this, "GetViewport", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("GetViewport", hr);
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
        PeCallScope peCall(*this, "GetScissorRect", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("GetScissorRect", hr);
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
                const auto t0 = AppendPhaseTimer::now();
                const bool ok = dxmt9::d3d9::pe::appendReszDepthResolve(
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
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryStateDecimatedStats_);
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
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryStateDecimatedStats_);
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
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryStateDecimatedStats_);
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
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryStateDecimatedStats_);
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
        *ppVD = CreatePeVertexDecl(d, this, this);
        notifyRenderTapeCreatedVertexDecl(
            d, D3D9PeWireVertexDecl(*ppVD),
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(tmp),
                n * sizeof(tmp[0])), n);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(
            IDirect3DVertexDeclaration9* pVD) noexcept override {
        PeCallScope peCall(*this, "SetVertexDeclaration");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::VertexInput);
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("SetVertexDeclaration", hr);
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
        *ppVS = CreatePeVertexShader(s, this, hashValidatedShaderBytecode(pFn), this);
        notifyRenderTapeCreatedShader(s, D3D9PeWireVertexShader(*ppVS), 0u);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) noexcept override {
        notePeDeviceCallAfterPresent("SetVertexShader");
        PeHotStateSetterTimer hotSetter(*this, PeHotStateSetterFamily::Shader);
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

    // Diagnostics-on body for SetVertexShaderConstantF, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantFSlow(UINT start, const float* pData,
                                  UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryConstDecimatedStats_);
        PeCallScope peCall(*this, "SetVertexShaderConstantF",
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
        touchConstShadow(peConsts_.vsConstF, start, count, pData, sizeof(float) * 4);
        return finishPeCall(S_OK);
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
        touchConstShadow(peConsts_.vsConstF, start, count, pData, sizeof(float) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* pData,
                                                        UINT count) noexcept override {
        notePeDeviceCallAfterPresent("GetVertexShaderConstantF");
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.vsConstF, start, pData, count, sizeof(float) * 4);
        return S_OK;    }
    // Diagnostics-on body for SetVertexShaderConstantI, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantISlow(UINT start, const INT* pData,
                                  UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryConstDecimatedStats_);
        notePeDeviceCallAfterPresent("SetVertexShaderConstantI");
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.vsConstI, start, count, pData, sizeof(int32_t) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetVertexShaderConstantISlow(start, pData, count);
        }
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
    // Diagnostics-on body for SetVertexShaderConstantB, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetVertexShaderConstantBSlow(UINT start, const BOOL* pData,
                                  UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryConstDecimatedStats_);
        notePeDeviceCallAfterPresent("SetVertexShaderConstantB");
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.vsConstB, start, count, pData, sizeof(uint32_t));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetVertexShaderConstantBSlow(start, pData, count);
        }
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
        PeCallScope peCall(*this, "SetStreamSource");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::VertexInput);
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
        PeCallScope peCall(*this, "SetIndices");
        PeHotStateSetterTimer hotSetter(
            *this, PeHotStateSetterFamily::VertexInput);
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("SetIndices", hr);
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
        *ppPS = CreatePePixelShader(s, this, hashValidatedShaderBytecode(pFn), this);
        notifyRenderTapeCreatedShader(s, D3D9PeWirePixelShader(*ppPS), 1u);
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
    // Diagnostics-on body for SetPixelShaderConstantF, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantFSlow(UINT start, const float* pData,
                                 UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryConstDecimatedStats_);
        PeCallScope peCall(*this, "SetPixelShaderConstantF",
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
        touchConstShadow(peConsts_.psConstF, start, count, pData, sizeof(float) * 4);
        return finishPeCall(S_OK);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetPixelShaderConstantFSlow(start, pData, count);
        }
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.psConstF, start, count, pData, sizeof(float) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* pData,
                                                       UINT count) noexcept override {
        notePeDeviceCallAfterPresent("GetPixelShaderConstantF");
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
        if (FAILED(hr)) return hr;
        readConstShadow(peConsts_.psConstF, start, pData, count, sizeof(float) * 4);
        return S_OK;    }
    // Diagnostics-on body for SetPixelShaderConstantI, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantISlow(UINT start, const INT* pData,
                                 UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryConstDecimatedStats_);
        notePeDeviceCallAfterPresent("SetPixelShaderConstantI");
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.psConstI, start, count, pData, sizeof(int32_t) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetPixelShaderConstantISlow(start, pData, count);
        }
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
    // Diagnostics-on body for SetPixelShaderConstantB, unchanged from
    // before the fast-path split. Reached only when
    // dxmt9PeConstSetterSlowPathRequired() is true.
    HRESULT __attribute__((noinline))
    SetPixelShaderConstantBSlow(UINT start, const BOOL* pData,
                                 UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryConstDecimatedStats_);
        notePeDeviceCallAfterPresent("SetPixelShaderConstantB");
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
        if (FAILED(hr)) return hr;
        touchConstShadow(peConsts_.psConstB, start, count, pData, sizeof(uint32_t));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) noexcept override {
        if (dxmt9PeConstSetterSlowPathRequired()) {
            return SetPixelShaderConstantBSlow(start, pData, count);
        }
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
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryDrawDecimatedStats_);
        PeCallScope peCall(*this, "DrawPrimitive", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("DrawPrimitive", hr);
        };
        // T2 device-lost gate.
        if (deviceNotReset_) return finishPeCall(D3DERR_DEVICELOST);
        dxmt9DeviceDebugLog("device_draw_primitive device=%p type=%u startVertex=%u count=%u",
                            this, (unsigned)type, startVertex, count);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return finishPeCall(barrierHr);
        }
        DxmtPeDecimatedPhaseTimer drawSwvpPhase(
            peEntryScope.stats != nullptr, peDrawPhaseSwvpDecimatedStats_);
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
            DxmtPeDecimatedPhaseTimer drawRecordPhase(
                peEntryScope.stats != nullptr, peDrawPhaseRecordDecimatedStats_);
            hr = appendDrawPrimitiveRecord(type, startVertex, count);
            drawRecordPhase.stop();
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
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryDrawDecimatedStats_);
        PeCallScope peCall(*this, "DrawIndexedPrimitive", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("DrawIndexedPrimitive", hr);
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
        DxmtPeDecimatedPhaseTimer drawSwvpPhase(
            peEntryScope.stats != nullptr, peDrawPhaseSwvpDecimatedStats_);
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
            DxmtPeDecimatedPhaseTimer drawRecordPhase(
                peEntryScope.stats != nullptr, peDrawPhaseRecordDecimatedStats_);
            hr = appendDrawIndexedPrimitiveRecord(type, baseVertex, minVertex,
                                                 numVertices, startIndex, count);
            drawRecordPhase.stop();
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
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryDrawDecimatedStats_);
        PeCallScope peCall(*this, "DrawPrimitiveUP", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("DrawPrimitiveUP", hr);
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
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, peEntryDrawDecimatedStats_);
        PeCallScope peCall(*this, "DrawIndexedPrimitiveUP", DXMT9_PE_CALLSITE_PC());
        const auto finishPeCall = [&](HRESULT hr) noexcept {
            return peCall.finish("DrawIndexedPrimitiveUP", hr);
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
        const Context context{
            .device = dev_,
            .deviceIdentity = this,
            .fvf = fvf_,
            .vertexDeclaration = vdecl_,
            .vertexShader = vs_,
            .streamSources = std::span<
                IDirect3DVertexBuffer9* const,
                D9C_DRAW_PACKET_MAX_STREAMS>{streamSrc_},
            .streamOffsets = std::span<
                const UINT, D9C_DRAW_PACKET_MAX_STREAMS>{streamOff_},
            .streamStrides = std::span<
                const UINT, D9C_DRAW_PACKET_MAX_STREAMS>{streamStr_},
            .streamFrequencies = std::span<
                const UINT, D9C_DRAW_PACKET_MAX_STREAMS>{streamFreq_},
            .textures = std::span<
                IDirect3DBaseTexture9* const,
                D9C_DRAW_PACKET_MAX_TEXTURES>{textures_},
            .state = peState_,
            .constants = peConsts_,
        };
        return processVertices(
            context, srcStart, dstIndex, vertexCount,
            dstBuffer, declaration, flags);
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
        notifyRenderTapeCreatedQuery(q, D3D9PeWireQuery(*ppQ));
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
        if (deviceNotReset_) {
            if (renderTapeCapture_)
                abortRenderTapeCapture("device_lost");
            return D3DERR_DEVICELOST;
        }
        const bool renderTapeCaptureWasActive =
            renderTapeCapture_ &&
            renderTapeCapture_->state() ==
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
            else
                (void)armRenderTapeCaptureAtPresentBoundary();
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
        const HRESULT hr = hr32(dxmt9c_device_wait_for_vblank(dev_, sc));
        const dxmt9::d3d9::RenderTapeFlushWaitControl payload{.waitedSeqId = 0u};
        NotifyRenderTapeOrderedControlForChild(
            dxmt9::d3d9::RenderTapeOrderedControlHeader{
                .kind = static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeControlKind::FlushWait),
                .disposition = static_cast<std::uint32_t>(
                    SUCCEEDED(hr)
                        ? dxmt9::d3d9::RenderTapeControlDisposition::Completed
                        : dxmt9::d3d9::RenderTapeControlDisposition::Failed),
                .resultCode = static_cast<std::int32_t>(hr),
                .controlBytes = sizeof(payload)},
            std::as_bytes(std::span(&payload, 1u)));
        return hr;
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
        if (renderTapeCapture_ &&
            renderTapeCapture_->state() ==
                dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
            const dxmt9::d3d9::RenderTapeResetControl payload{
                .reclaimedGeneration = 0u, .terminal = 1u};
            NotifyRenderTapeOrderedControlForChild(
                dxmt9::d3d9::RenderTapeOrderedControlHeader{
                    .kind = static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeControlKind::Reset),
                    .disposition = static_cast<std::uint32_t>(
                        SUCCEEDED(hr)
                            ? dxmt9::d3d9::RenderTapeControlDisposition::Terminal
                            : dxmt9::d3d9::RenderTapeControlDisposition::Failed),
                    .resultCode = static_cast<std::int32_t>(hr),
                    .controlBytes = sizeof(payload)},
                std::as_bytes(std::span(&payload, 1u)));
            abortRenderTapeCapture("reset_ex");
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
