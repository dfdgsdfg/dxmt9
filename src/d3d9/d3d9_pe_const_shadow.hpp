#pragma once

#include "d3d9_pe_stats_decimation.hpp"
#include "dxmt9/device_c.h"
#include "util/config/config.hpp"

#include <algorithm>
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
    // (`saved_.vsConstF.size() / kFloatVecSize` in
    // D3D9DeviceChildImpl::replaySavedShadow,
    // d3d9_pe_device_child_misc.cpp) to call
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

// DXMT9_PE_STATS_DECIMATION diagnostic (const_setter scope): touchConstShadow
// is a free function shared by all six VS/PS const-shadow call sites in
// d3d9_pe_device.cpp, so the accumulator and its cached env value live as
// function-local statics here (a single instance per process, guaranteed by
// the ODR for this inline function) rather than as a device-class member.
// D3D9DeviceImpl::logPeStatsDecimation() reaches it via
// peConstSetterDecimatedStats().
//
// phaseOffset is 1, not 0, and that is load-bearing: touchConstShadow is
// called exactly once per const-setter entry point, so this scope's event
// counter advances in lockstep with the entry scope's
// (peEntryConstDecimatedStats_). Sharing phase 0 would make both sample the
// same calls forever and bury this scope's entire instrument inside the entry
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

inline void touchConstShadow(ConstShadow& shadow,
                             std::uint32_t start,
                             std::uint32_t count,
                             const void* data,
                             std::size_t elemSize) {
    // Decimated timing: sample only every Nth call so the CPU cost of
    // measuring is itself negligible (see d3d9_pe_stats_decimation.hpp).
    // Independent of DXMT9_PE_RECORDER_STATS. Guard covers every exit path
    // (including the early returns below) via RAII.
    struct DecimatedScopeGuard {
        PeDecimatedScopeStats* stats = nullptr;
        std::uint32_t bucketCount = 0;
        std::chrono::steady_clock::time_point t0{};
        ~DecimatedScopeGuard() {
            if (stats) {
                const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                PeDecimatedScopeTimer::recordSample(
                    *stats, static_cast<std::uint64_t>(elapsedNs));
                peConstSetterDecimatedBuckets().record(
                    bucketCount, static_cast<std::uint64_t>(elapsedNs));
            }
        }
    } decimatedScope;
    const std::uint32_t decimationN = peConstSetterDecimationN();
    if (decimationN != 0 &&
        PeDecimatedScopeTimer::shouldSample(peConstSetterDecimatedStats(), decimationN)) {
        decimatedScope.stats = &peConstSetterDecimatedStats();
        decimatedScope.bucketCount = count;
        // Calibration sample: an empty region timed with the same clock pair.
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
    if (decimationN != 0) {
        peConstSetterDecimatedBuckets().countEvent(count);
    }
    const std::uint64_t needed64 =
        (static_cast<std::uint64_t>(start) + count) * elemSize;
    if (needed64 > 0xffffffffull) {
        return;
    }
    const auto needed = static_cast<std::size_t>(needed64);
    // Captured BEFORE the resize below: true only when this call's whole
    // span was already materialized by a previous call. A freshly-extended
    // region is zero-filled by resize() and must NOT take the bulk
    // early-out below -- an all-zero write into it has to fall through to
    // the per-element loop, which is what makes it alias the zero-fill and
    // stay clean (see the (d)/(e) cases in
    // testTouchConstShadowSemantics()).
    const bool alreadyCovered = shadow.values.size() >= needed;
    if (shadow.values.size() < needed) {
        shadow.values.resize(needed);
    }
    const std::size_t neededElems = static_cast<std::size_t>(start) + count;
    if (shadow.dirtyElems.size() < neededElems) {
        shadow.dirtyElems.resize(neededElems);
    }
    const auto offset = static_cast<std::size_t>(start) * elemSize;
    const auto bytes = static_cast<std::size_t>(count) * elemSize;
    bool changed = false;
    std::uint32_t firstChanged = count;
    std::uint32_t lastChanged = 0;
    if (bytes != 0 && data) {
        auto* dst = shadow.values.data() + offset;
        const auto* src = static_cast<const std::uint8_t*>(data);
        // Bulk-span early-out: one vectorized compare over the whole span
        // instead of per-element memcmp/memcpy calls. Only valid when the
        // span was already materialized (alreadyCovered) -- see the
        // comment above. When it fires and the span is unchanged, this is
        // observably identical to the per-element loop finding every
        // element equal: no dirtyElems writes, no merged-range update.
        if (alreadyCovered && std::memcmp(dst, src, bytes) == 0) {
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto elemOffset = static_cast<std::size_t>(i) * elemSize;
            if (std::memcmp(dst + elemOffset, src + elemOffset, elemSize) == 0) {
                continue;
            }
            std::memcpy(dst + elemOffset, src + elemOffset, elemSize);
            shadow.dirtyElems[static_cast<std::size_t>(start) + i] = 1;
            changed = true;
            firstChanged = std::min<std::uint32_t>(firstChanged, i);
            lastChanged = i + 1u;
        }
    }
    if (!changed) {
        return;
    }
    const std::uint32_t changedStart = start + firstChanged;
    const std::uint32_t end = start + lastChanged;
    if (!shadow.dirty()) {
        shadow.dirtyStart = changedStart;
        shadow.dirtyEnd = end;
    } else {
        shadow.dirtyStart = std::min<std::uint32_t>(shadow.dirtyStart, changedStart);
        shadow.dirtyEnd = std::max<std::uint32_t>(shadow.dirtyEnd, end);
    }
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
