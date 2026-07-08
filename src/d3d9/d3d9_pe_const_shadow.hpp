#pragma once

#include "d3d9_pe_stats_decimation.hpp"
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
