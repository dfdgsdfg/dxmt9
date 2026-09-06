#pragma once

#include "dxmt9/copy_materialization_ledger.hpp"
#include "d3d9_pe_transition_algebra.hpp"

#include "d3d9_pe_stats_decimation.hpp"
#include "dxmt9/device_c.h"
#include "util/config/config.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

struct ConstShadow {
    std::vector<std::uint8_t> values;
    std::vector<std::uint8_t> dirtyElems;
    std::uint32_t dirtyStart = 0;
    std::uint32_t dirtyEnd = 0;

    bool dirty() const {
        return dirtyEnd > dirtyStart;
    }
    void clear() {
        dirtyStart = dirtyEnd = 0;
        std::fill(dirtyElems.begin(), dirtyElems.end(), std::uint8_t{0});
    }
    void reset() {
        values.clear();
        dirtyElems.clear();
        clear();
    }

    // Pre-allocate storage capacity (not size) to the caller-supplied
    // D3D9 register-file cap, without changing values.size(),
    // dirtyElems.size(), or dirty state.
    //
    // This must reserve() rather than resize(): values.size() is
    // load-bearing outside this file as "how many registers have ever
    // actually been touched" -- D3D9StateBlockShadow capture does
    // `out.vsConstF = peConsts_.vsConstF.values` wholesale
    // (d3d9_pe_device.cpp), and stateblock replay derives the restore
    // count directly from that vector's size
    // (`saved_.vsConstF.size() / kFloatVecSize` in the state-block Apply
    // preparation path) to call
    // `SetVertexShaderConstantF(0, data, count)`. If this pre-sized
    // values up to the full register-file cap, every stateblock capture
    // would restore the whole cap's worth of registers -- including ones
    // the app never set, as zero -- instead of only the registers the app
    // actually touched, which is an observable wire/behavior change.
    // touchConstShadow's existing `if (values.size() < needed)
    // values.resize(needed)` remains the sole path that grows size(); with
    // capacity already reserved here, that resize no longer reallocates or
    // copies for any span within the reserved cap, which is the actual hot
    // cost this removes. reset() -> values.clear() drops size() back to 0
    // but (per the standard) preserves capacity, so the reservation survives
    // a stateblock End/reset cycle without re-establishing it.
    void reserveCapacity(std::size_t byteCap, std::size_t elemCap) {
        values.reserve(byteCap);
        dirtyElems.reserve(elemCap);
    }
};

struct PeConstShadowBlock {
    ConstShadow vsConstF{};
    ConstShadow vsConstI{};
    ConstShadow vsConstB{};
    ConstShadow psConstF{};
    ConstShadow psConstI{};
    ConstShadow psConstB{};

    // D3D9 register-file caps used to bound the Set*Constant* entry points
    // in d3d9_pe_device.cpp (kVsConstFMax=256, kVsConstIMax=16,
    // kVsConstBMax=16, kPsConstFMax=224, kPsConstIMax=16, kPsConstBMax=16)
    // and mirrored by D9C_DRAW_PACKET_MAX_CONST_{VS,PS}_{F,I,B} in
    // include/dxmt9/device_c.h, which d9c_draw_packet_const_delta_section_
    // range_valid checks against. Both sources agree, so touchConstShadow
    // can never be asked to grow a shadow past what is reserved here.
    PeConstShadowBlock() {
        vsConstF.reserveCapacity(256u * sizeof(float) * 4u, 256u);
        vsConstI.reserveCapacity(16u * sizeof(std::int32_t) * 4u, 16u);
        vsConstB.reserveCapacity(16u * sizeof(std::uint32_t), 16u);
        psConstF.reserveCapacity(224u * sizeof(float) * 4u, 224u);
        psConstI.reserveCapacity(16u * sizeof(std::int32_t) * 4u, 16u);
        psConstB.reserveCapacity(16u * sizeof(std::uint32_t), 16u);
    }

    void reset() {
        vsConstF.reset();
        vsConstI.reset();
        vsConstB.reset();
        psConstF.reset();
        psConstI.reset();
        psConstB.reset();
    }
};

// Wrapper-owned state-block constant snapshot.  `trackedElems` fixes the
// register set at EndStateBlock; Capture refreshes bytes for those registers
// only and never grows the set.  Values are byte-shaped so all six D3D9
// constant kinds share one fixed-capacity, allocation-free adapter.
struct StateBlockConstShadow {
    static constexpr std::size_t kMaxBytes = 4096u;
    static constexpr std::size_t kMaxElems = 256u;
    std::array<std::uint8_t, kMaxBytes> values{};
    std::array<std::uint8_t, kMaxElems> trackedElems{};
    std::uint32_t valueSize = 0u;
    std::uint32_t trackedSize = 0u;

    void reserveCapacity(std::size_t, std::size_t) noexcept {}

    void clear() noexcept {
        valueSize = 0u;
        trackedSize = 0u;
        values.fill(0u);
        trackedElems.fill(0u);
    }

    bool empty() const noexcept {
        return std::none_of(trackedElems.begin(), trackedElems.begin() + trackedSize,
                            [](std::uint8_t value) { return value != 0u; });
    }

    std::size_t size() const noexcept { return valueSize; }

    bool contains(std::uint32_t reg) const noexcept {
        return reg < trackedSize && trackedElems[reg] != 0u;
    }

    void record(std::uint32_t start, std::uint32_t count, const void* data,
                std::size_t elemSize) {
        if (count == 0u) {
            return;
        }
        const std::size_t end = static_cast<std::size_t>(start) + count;
        const std::size_t byteEnd = end * elemSize;
        if (byteEnd > kMaxBytes || end > kMaxElems) return;
        valueSize = std::max<std::uint32_t>(valueSize,
                                            static_cast<std::uint32_t>(byteEnd));
        trackedSize = std::max<std::uint32_t>(trackedSize,
                                              static_cast<std::uint32_t>(end));
        const auto byteCount = static_cast<std::size_t>(count) * elemSize;
        auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
            dxmt9::core::CopyMaterializationOwner::Pe);
        if (ledger) {
            dxmt9::core::CopyMaterializationEvent copy(
                ledger, dxmt9::core::CopyMaterializationClass::PeStateShadow,
                byteCount);
            std::memcpy(values.data() + static_cast<std::size_t>(start) * elemSize,
                        data, byteCount);
        } else {
            std::memcpy(values.data() + static_cast<std::size_t>(start) * elemSize,
                        data, byteCount);
        }
        std::fill(trackedElems.begin() + start, trackedElems.begin() + end,
                  std::uint8_t{1});
    }

    void refreshFrom(const ConstShadow& live, std::size_t elemSize) {
        for (std::size_t reg = 0; reg < trackedSize; ++reg) {
            if (trackedElems[reg] == 0u) {
                continue;
            }
            const std::size_t offset = reg * elemSize;
            if (valueSize < offset + elemSize) {
                valueSize = static_cast<std::uint32_t>(offset + elemSize);
            }
            if (live.values.size() >= offset + elemSize) {
                auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
                    dxmt9::core::CopyMaterializationOwner::Pe);
                if (ledger) {
                    dxmt9::core::CopyMaterializationEvent copy(
                        ledger,
                        dxmt9::core::CopyMaterializationClass::PeStateShadow,
                        elemSize);
                    std::memcpy(values.data() + offset,
                                live.values.data() + offset, elemSize);
                } else {
                    std::memcpy(values.data() + offset,
                                live.values.data() + offset, elemSize);
                }
            } else {
                std::memset(values.data() + offset, 0, elemSize);
            }
        }
    }

    template<typename Fn>
    bool forEachRange(std::size_t elemSize, Fn&& fn) const {
        std::size_t begin = 0u;
        while (begin < trackedSize) {
            while (begin < trackedSize && trackedElems[begin] == 0u) {
                ++begin;
            }
            if (begin == trackedSize) {
                break;
            }
            std::size_t end = begin + 1u;
            while (end < trackedSize && trackedElems[end] != 0u) {
                ++end;
            }
            if (!fn(static_cast<std::uint32_t>(begin),
                    static_cast<std::uint32_t>(end - begin),
                    values.data() + begin * elemSize)) {
                return false;
            }
            begin = end;
        }
        return true;
    }
};

struct PeStateBlockConstRecorded {
    StateBlockConstShadow vsConstF{};
    StateBlockConstShadow vsConstI{};
    StateBlockConstShadow vsConstB{};
    StateBlockConstShadow psConstF{};
    StateBlockConstShadow psConstI{};
    StateBlockConstShadow psConstB{};

    void clearForBegin() noexcept {
        vsConstF.clear();
        vsConstI.clear();
        vsConstB.clear();
        psConstF.clear();
        psConstI.clear();
        psConstB.clear();
    }
};

// DXMT9_PE_STATS_DECIMATION diagnostic (const_setter scope): the fused
// constant transition reducer is shared by all six VS/PS const-shadow call sites in
// d3d9_pe_device.cpp, so the accumulator and its cached env value live as
// function-local statics here (a single instance per process, guaranteed by
// the ODR for this inline function) rather than as a device-class member.
// D3D9DeviceImpl::logPeStatsDecimation() reaches it via
// peConstSetterDecimatedStats().
//
// phaseOffset is 1, not 0: valid nonzero live setters now include redundant
// writes in this scope (the former touch-only population excluded them).
// Sharing phase 0 could make both scopes sample the same calls and bury this
// scope's entire instrument inside the entry
// scope's span -- the failure documented at the top of
// d3d9_pe_stats_decimation.hpp.
inline PeDecimatedScopeStats& peConstSetterDecimatedStats() {
    static PeDecimatedScopeStats stats = [] {
        PeDecimatedScopeStats s{};
        s.phaseOffset = 1;
        return s;
    }();
    return stats;
}

// Companion to peConstSetterDecimatedStats(): the same samples split by the
// call's register count, to separate fixed per-call overhead from the
// per-element compare loop. Same ODR-single-instance reasoning as above.
inline PeDecimatedBucketStats& peConstSetterDecimatedBuckets() {
    static PeDecimatedBucketStats buckets{};
    return buckets;
}

// Calibration for the two accumulators above. On every sampled call we also
// time an immediately-adjacent empty region with the identical clock pair, so
// its mean is the instrument's own cost per sample. Subtracting it from the
// scope's mean is what turns a decimated reading into an absolute one:
// N-variation cannot do that, because the extrapolation
// `sampled_ms * N / presents` reduces to `events * (true + bias) / presents`
// and is independent of N -- which is why N=64 and N=16 agreeing proved
// stability, not accuracy.
inline std::uint32_t peConstSetterDecimationN() {
    static const std::uint32_t n = []() -> std::uint32_t {
        const auto envValue =
            dxmt9::util::getenvU32("DXMT9_PE_STATS_DECIMATION");
        return envValue.value_or(0);
    }();
    return n;
}

enum class ConstShadowTransitionMode : std::uint8_t {
    Observe,
    Apply,
};

struct ConstShadowTransitionResult {
    // Aggregate model permissions, not changed-byte counters. In particular
    // an absent zero register permits materialization without becoming dirty.
    bool valid = true;
    bool writeLive = false;
    bool writePending = false;
    bool writeRecorded = false;

    bool writesLive() const noexcept { return writeLive; }
    bool writesPending() const noexcept { return writePending; }
    bool writesRecorded() const noexcept { return writeRecorded; }
};

inline bool constShadowTransitionElemEquals(
    const ConstShadow& shadow, std::size_t offset, bool liveContains,
    const std::uint8_t* source, std::size_t elemSize) noexcept {
    if (liveContains) {
        return std::memcmp(shadow.values.data() + offset, source, elemSize) == 0;
    }
    for (std::size_t i = 0; i < elemSize; ++i) {
        if (source[i] != 0u) return false;
    }
    return true;
}

// Reduce and, in Apply mode, commit one constant-setter transition. Observe
// mode is used by state-block recording: it evaluates the same model facts
// without touching the live shadow. Apply mode materializes the requested
// extent first (resize is allocation-free after PeConstShadowBlock's bounded
// warm reservation), then copies only differing QueueDelta elements while
// reducing. This generic helper may allocate for an unreserved test shadow;
// keep the former touch helper's exception contract, not an implicit terminate.
inline ConstShadowTransitionResult reduceConstShadowTransitionUnobserved(
    ConstShadow& shadow, dxmt9::d3d9::pe::RecorderPhase phase,
    ConstShadowTransitionMode mode, std::uint32_t start,
    std::uint32_t count, const void* data, std::size_t elemSize) {
    ConstShadowTransitionResult result{};
    if (count == 0u) return result;
    if (!data || elemSize == 0u) {
        result.valid = false;
        return result;
    }
    if (mode == ConstShadowTransitionMode::Apply &&
        phase != dxmt9::d3d9::pe::RecorderPhase::Live) {
        result.valid = false;
        return result;
    }

    const auto* source = static_cast<const std::uint8_t*>(data);
    const std::uint64_t end64 = static_cast<std::uint64_t>(start) + count;
    if (end64 > 0xffffffffull || elemSize > 0xffffffffull / end64) {
        result.valid = false;
        return result;
    }
    const std::uint64_t needed64 = end64 * elemSize;
    const std::size_t oldValueSize = shadow.values.size();
    const std::size_t oldElemSize = shadow.dirtyElems.size();
    const std::size_t needed = static_cast<std::size_t>(needed64);
    const std::size_t neededElems = static_cast<std::size_t>(end64);
    if (mode == ConstShadowTransitionMode::Apply) {
        if (shadow.values.size() < needed) shadow.values.resize(needed);
        if (shadow.dirtyElems.size() < neededElems)
            shadow.dirtyElems.resize(neededElems);
    }

    bool changed = false;
    std::uint32_t firstChanged = count;
    std::uint32_t lastChanged = 0u;
    for (std::uint32_t i = 0u; i < count; ++i) {
        const std::uint32_t reg = start + i;
        const std::size_t offset = static_cast<std::size_t>(reg) * elemSize;
        const bool liveContains = oldValueSize >= offset + elemSize;
        // One comparison serves both the old wrapper's permission decision
        // and touch's byte update. An absent zero register requests extent
        // materialization through the model, but matches resize's zero fill
        // and must not gain a pending dirty bit.
        const bool equals = constShadowTransitionElemEquals(
            shadow, offset,
            mode == ConstShadowTransitionMode::Apply || liveContains,
            source + static_cast<std::size_t>(i) * elemSize, elemSize);
        const auto plan = dxmt9::d3d9::pe::planRecorderStateWrite(
            dxmt9::d3d9::pe::StateWriteFacts{
                .phase = phase,
                .origin = dxmt9::d3d9::pe::WriteOrigin::ExplicitSet,
                .liveContains = liveContains,
                .liveEquals = equals,
                .pendingContains =
                    reg < oldElemSize && shadow.dirtyElems[reg] != 0u,
            });
        result.valid = result.valid && plan.valid();
        result.writeLive = result.writeLive || plan.writeLive();
        result.writePending = result.writePending || plan.writePending();
        result.writeRecorded = result.writeRecorded || plan.writeRecorded();

        if (mode != ConstShadowTransitionMode::Apply ||
            !plan.writePending() || equals) {
            continue;
        }
        const auto* element = source + static_cast<std::size_t>(i) * elemSize;
        std::memcpy(shadow.values.data() + offset, element, elemSize);
        shadow.dirtyElems[reg] = 1u;
        changed = true;
        firstChanged = std::min(firstChanged, i);
        lastChanged = i + 1u;
    }

    if (mode == ConstShadowTransitionMode::Apply && changed) {
        const std::uint32_t changedStart = start + firstChanged;
        const std::uint32_t changedEnd = start + lastChanged;
        if (!shadow.dirty()) {
            shadow.dirtyStart = changedStart;
            shadow.dirtyEnd = changedEnd;
        } else {
            shadow.dirtyStart =
                std::min(shadow.dirtyStart, changedStart);
            shadow.dirtyEnd = std::max(shadow.dirtyEnd, changedEnd);
        }
    }
    return result;
}

// Keep timer state and calibration in a cold outlined adapter. The disabled
// constant path constructs no diagnostic scope, even on a redundant write.
__attribute__((noinline)) inline ConstShadowTransitionResult
reduceConstShadowTransitionObserved(
    ConstShadow& shadow, std::uint32_t start, std::uint32_t count,
    const void* data, std::size_t elemSize) {
    struct DecimatedScopeGuard {
        PeDecimatedScopeStats* stats = nullptr;
        std::uint32_t bucketCount = 0u;
        std::chrono::steady_clock::time_point t0{};
        ~DecimatedScopeGuard() {
            if (!stats) return;
            const auto elapsedNs = std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
            PeDecimatedScopeTimer::recordSample(
                *stats, static_cast<std::uint64_t>(elapsedNs));
            peConstSetterDecimatedBuckets().record(
                bucketCount, static_cast<std::uint64_t>(elapsedNs));
        }
    } scope;
    const auto n = peConstSetterDecimationN();
    if (PeDecimatedScopeTimer::shouldSample(peConstSetterDecimatedStats(), n)) {
        scope.stats = &peConstSetterDecimatedStats();
        scope.bucketCount = count;
        const auto n0 = std::chrono::steady_clock::now();
        const auto n1 = std::chrono::steady_clock::now();
        PeDecimatedScopeTimer::recordSample(
            peDecimatedNullScopeStats(), static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(n1 - n0).count()));
        scope.t0 = std::chrono::steady_clock::now();
    }
    peConstSetterDecimatedBuckets().countEvent(count);
    return reduceConstShadowTransitionUnobserved(
        shadow, dxmt9::d3d9::pe::RecorderPhase::Live,
        ConstShadowTransitionMode::Apply, start, count, data, elemSize);
}

inline ConstShadowTransitionResult reduceConstShadowTransition(
    ConstShadow& shadow, dxmt9::d3d9::pe::RecorderPhase phase,
    ConstShadowTransitionMode mode, std::uint32_t start,
    std::uint32_t count, const void* data, std::size_t elemSize) {
    if (mode == ConstShadowTransitionMode::Apply &&
        phase == dxmt9::d3d9::pe::RecorderPhase::Live &&
        count != 0u && data && elemSize != 0u &&
        peConstSetterDecimationN() != 0u) {
        return reduceConstShadowTransitionObserved(
            shadow, start, count, data, elemSize);
    }
    return reduceConstShadowTransitionUnobserved(
        shadow, phase, mode, start, count, data, elemSize);
}

inline void touchConstShadow(ConstShadow& shadow,
                             std::uint32_t start,
                             std::uint32_t count,
                             const void* data,
                             std::size_t elemSize) {
    (void)reduceConstShadowTransition(
        shadow, dxmt9::d3d9::pe::RecorderPhase::Live,
        ConstShadowTransitionMode::Apply, start, count, data, elemSize);
}

// R-BACK-2.52(b),(d) (DXMT9_PE_INLINE_CONST_DELTA fold): consume `shadow`'s
// merged dirty range directly into a draw packet's inline const-delta
// section header plus the caller-owned trailing payload buffer, instead of
// emitting a standalone D9C_COMMAND_RECORD_SET_*_CONST_* record. This MUST
// produce EXACTLY the same [dirtyStart, dirtyEnd) merged range and the same
// per-register `elemSize` that flushConstShadow would have emitted as one
// record —
// that equivalence is what makes the fold path's server-side effective
// state match the standalone-record wire (R-BACK-2.52(d)).
//
// On success (returns true): if `shadow` was clean, `section` is left fully
// zeroed (valid=0) and `payloadBytesInOut` is untouched — nothing to fold.
// If `shadow` was dirty, `section` is populated with the merged range,
// `count * elemSize` bytes are copied from `shadow.values` into `payload`
// starting at the current `payloadBytesInOut` offset, `payloadBytesInOut`
// is advanced by that many bytes, and `shadow` is cleared (dirty range
// reset) exactly like a successful flushConstShadow.
//
// On failure (returns false): `section` is left at valid=0 and `shadow` is
// left UNTOUCHED (still dirty) so the caller can fall back to the
// standalone flushConstShadow for this shadow instead of silently dropping
// the pending bytes. Failure happens when the merged range fails
// d9c_draw_packet_const_delta_section_range_valid for `kind` (should be
// unreachable in production — the Set* fast paths already bound ranges to
// the D3D9 register-file limit before touchConstShadow runs) or when the
// payload wouldn't fit in the caller-supplied `payloadCapacity` (defensive;
// callers size their scratch buffer to the sum of every section's
// register-file cap, so this should also be unreachable).
