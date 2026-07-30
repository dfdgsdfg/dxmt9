#pragma once

// Clock-reading half of the decimated PE scope timers.
//
// d3d9_pe_stats_decimation.hpp is deliberately clock-free -- it says so in its
// own header comment, and that is what makes it unit-testable without a clock.
// The RAII guard below has to read std::chrono, and the decimation rate comes
// from the environment, so both live here instead of being pushed into that
// header. Anything that needs to *time* a decimated scope includes this;
// anything that only needs the accumulator shape includes the other.
//
// Extracted from d3d9_pe_device.cpp so d3d9_pe_producer.cpp can time the
// draw-packet scope without pulling in the device translation unit (which is
// Windows-only and therefore not natively buildable).

#include "d3d9_pe_stats_decimation.hpp"
#include "util/config/config.hpp"

#include <chrono>
#include <cstdint>

// DXMT9_PE_STATS_DECIMATION: numeric N for the decimated (every-Nth-event)
// PE-recorder scope timers. Fully independent of DXMT9_PE_RECORDER_STATS /
// dxmt9PeRecorderStatsEnabled() -- this works whether or not that flag is set.
// Unset / "0" / unparseable = off.
inline std::uint32_t dxmt9PeStatsDecimationN() {
    static const std::uint32_t n = []() -> std::uint32_t {
        const auto envValue =
            dxmt9::util::getenvU32("DXMT9_PE_STATS_DECIMATION");
        return envValue.value_or(0);
    }();
    return n;
}

// RAII scope timer for the decimated PE scopes. Covers every exit path of the
// guarded function, including early returns, by recording the elapsed time in
// its destructor. `stats` stays null (no-op destructor) unless
// PeDecimatedScopeTimer::shouldSample() selected this call for timing.
struct DxmtPeDecimatedScopeGuard {
    PeDecimatedScopeStats* stats = nullptr;
    std::chrono::steady_clock::time_point t0{};
    ~DxmtPeDecimatedScopeGuard() {
        if (stats) {
            const auto elapsedNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
            PeDecimatedScopeTimer::recordSample(
                *stats, static_cast<std::uint64_t>(elapsedNs));
        }
    }
};
