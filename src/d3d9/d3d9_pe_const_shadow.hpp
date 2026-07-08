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
};

struct PeConstShadowBlock {
    ConstShadow vsConstF{};
    ConstShadow vsConstI{};
    ConstShadow vsConstB{};
    ConstShadow psConstF{};
    ConstShadow psConstI{};
    ConstShadow psConstB{};

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
inline PeDecimatedScopeStats& peConstSetterDecimatedStats() {
    static PeDecimatedScopeStats stats{};
    return stats;
}

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
        std::chrono::steady_clock::time_point t0{};
        ~DecimatedScopeGuard() {
            if (stats) {
                const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                PeDecimatedScopeTimer::recordSample(
                    *stats, static_cast<std::uint64_t>(elapsedNs));
            }
        }
    } decimatedScope;
    const std::uint32_t decimationN = peConstSetterDecimationN();
    if (decimationN != 0 &&
        PeDecimatedScopeTimer::shouldSample(peConstSetterDecimatedStats(), decimationN)) {
        decimatedScope.stats = &peConstSetterDecimatedStats();
        decimatedScope.t0 = std::chrono::steady_clock::now();
    }
    const std::uint64_t needed64 =
        (static_cast<std::uint64_t>(start) + count) * elemSize;
    if (needed64 > 0xffffffffull) {
        return;
    }
    const auto needed = static_cast<std::size_t>(needed64);
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
// per-register `elemSize` that flushConstShadow's default (non-split, see
// DXMT9_SPLIT_SPARSE_CONST_RECORDS) path would have emitted as one record —
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
inline bool foldConstShadowIntoDeltaSection(
    ConstShadow& shadow,
    std::uint32_t kind,
    std::size_t elemSize,
    D9CDrawPacketConstDeltaSection& section,
    std::uint8_t* payload,
    std::size_t payloadCapacity,
    std::uint32_t& payloadBytesInOut) {
    section.valid = 0;
    section.startRegister = 0;
    section.registerCount = 0;
    if (!shadow.dirty()) {
        return true;
    }
    const std::uint32_t start = shadow.dirtyStart;
    const std::uint32_t count = shadow.dirtyEnd - shadow.dirtyStart;
    if (!d9c_draw_packet_const_delta_section_range_valid(kind, start, count)) {
        return false;
    }
    const std::uint32_t bytes = static_cast<std::uint32_t>(count * elemSize);
    if (payload == nullptr ||
        static_cast<std::size_t>(payloadBytesInOut) + bytes > payloadCapacity) {
        return false;
    }
    section.valid = 1;
    section.startRegister = start;
    section.registerCount = count;
    const auto offset = static_cast<std::size_t>(start) * elemSize;
    std::memcpy(payload + payloadBytesInOut, shadow.values.data() + offset, bytes);
    payloadBytesInOut += bytes;
    shadow.clear();
    return true;
}
