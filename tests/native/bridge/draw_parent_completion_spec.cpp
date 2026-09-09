#include "../../conformance/d3d9/draw_parent_completion_policy.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using dxmt9::draw_parent_completion::shouldDrain;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "draw_parent_completion_spec failed: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void check(bool condition, const std::string& message) {
  if (!condition) fail(message);
}

// Independent phase scheduler used as the native oracle. It intentionally
// does not call the production predicate or repeat its modulo expression:
// each phase buffers groups, flushes at a full window or phase end, then
// resets the buffered count.
void checkPhase(std::uint32_t phaseCount, std::uint32_t window) {
  check(!shouldDrain(0u, phaseCount, window), "zero ordinal has no boundary");
  check(!shouldDrain(phaseCount + 1u, phaseCount, window),
        "past-end ordinal has no boundary");
  std::uint32_t buffered = 0u;
  std::uint32_t observedDrains = 0u;
  for (std::uint32_t ordinal = 1u; ordinal <= phaseCount; ++ordinal) {
    ++buffered;
    const bool expected = buffered == window || ordinal == phaseCount;
    const bool observed = shouldDrain(ordinal, phaseCount, window);
    check(observed == expected,
          "phase decision differs from independent scheduler");
    if (expected) buffered = 0u;
    if (observed) ++observedDrains;
  }
  check(buffered == 0u, "final partial window must drain");
  check(observedDrains == (phaseCount + window - 1u) / window,
        "phase drain count differs from independent scheduler");
}

void testWindowBounds() {
  check(!shouldDrain(1u, 1u, 0u), "zero window fails closed");
  check(!shouldDrain(1u, 1u, 65u), "window above 64 fails closed");
  check(!shouldDrain(0u, 1u, 1u), "zero ordinal fails closed");
  check(!shouldDrain(1u, 0u, 1u), "empty phase has no drain");
  check(!shouldDrain(2u, 1u, 1u), "ordinal past phase fails closed");
}

void testExhaustiveBoundedPhases() {
  for (std::uint32_t window = 1u; window <= 64u; ++window) {
    for (std::uint32_t phaseCount = 0u; phaseCount <= 128u;
         ++phaseCount) {
      checkPhase(phaseCount, window);
    }
  }
}

void testLargePartialAndBoundaryPhases() {
  constexpr std::array<std::uint32_t, 8u> phaseCounts{{
      129u, 255u, 256u, 1023u, 1024u, 4095u, 4096u, 8192u}};
  for (const std::uint32_t window : {1u, 2u, 3u, 7u, 31u, 32u, 63u,
                                     64u}) {
    for (const std::uint32_t phaseCount : phaseCounts) {
      checkPhase(phaseCount, window);
    }
  }
}

void testWarmAndMeasuredPhaseReset() {
  // The measured phase must restart at local ordinal one, regardless of the
  // warm phase length. This covers empty warm phases and non-divisible tails.
  constexpr std::array<std::uint32_t, 7u> warmCounts{{0u, 1u, 2u, 7u,
                                                       63u, 64u, 128u}};
  constexpr std::array<std::uint32_t, 8u> measuredCounts{{
      1u, 2u, 3u, 63u, 64u, 65u, 127u, 8192u}};
  for (const std::uint32_t window : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
    for (const std::uint32_t warm : warmCounts) {
      for (const std::uint32_t measured : measuredCounts) {
        checkPhase(warm, window);
        checkPhase(measured, window);
      }
    }
  }
}

}  // namespace

int main() {
  testWindowBounds();
  testExhaustiveBoundedPhases();
  testLargePartialAndBoundaryPhases();
  testWarmAndMeasuredPhaseReset();
  std::cout << "draw_parent_completion_spec passed\n";
  return EXIT_SUCCESS;
}
