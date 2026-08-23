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

// Explicit-stop phase timer for sub-scopes of an already-armed scope.
//
// Two rules it enforces. It only runs when the PARENT scope was sampled, so
// phases are comparable to each other and to their parent rather than being
// three independently-sampled populations. And it stops explicitly rather than
// in a destructor, because a phase usually ends before its variables leave
// scope -- a destructor would time the wrong span.
//
// Phases are NOT null-calibrated individually: each phase pays one clock pair
// out of the parent's already-calibrated span, so subtracting the parent's null
// again per phase would double-count. Phases are comparable to each other; the
// parent total is the calibrated figure.
struct DxmtPeDecimatedPhaseTimer {
  PeDecimatedScopeStats* stats = nullptr;
  std::chrono::steady_clock::time_point t0{};

  DxmtPeDecimatedPhaseTimer(bool parentSampled,
                            PeDecimatedScopeStats* target) {
    if (!parentSampled || !target) {
      return;
    }
    stats = target;
    t0 = std::chrono::steady_clock::now();
  }

  void stop() {
    if (!stats) {
      return;
    }
    PeDecimatedScopeTimer::recordSample(
        *stats, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count()));
    stats = nullptr;
  }
};

// Arms a decimated scope in one statement: sampling decision, null-scope
// calibration, and t0, in the order the existing hand-written call sites use.
//
// The calibration read is not optional and not decoration -- the instrument
// costs ~186ns per sample, which is 6% of appendRecordDirect's reading and 92%
// of touchConstShadow's, so an uncalibrated number is wrong by more than the
// thing it measures for any short scope
// (docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.04.md).
// Every scope must therefore pay the same null read, which is why this is a
// helper rather than a comment telling people to remember.
inline void dxmt9PeArmDecimatedScope(DxmtPeDecimatedScopeGuard& guard,
                                     PeDecimatedScopeStats* stats) {
  if (!stats) {
    return;
  }
  const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
  if (decimationN == 0 ||
      !PeDecimatedScopeTimer::shouldSample(*stats, decimationN)) {
    return;
  }
  guard.stats = stats;
  {
    const auto n0 = std::chrono::steady_clock::now();
    const auto n1 = std::chrono::steady_clock::now();
    PeDecimatedScopeTimer::recordSample(
        peDecimatedNullScopeStats(),
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(n1 - n0).count()));
  }
  guard.t0 = std::chrono::steady_clock::now();
}
