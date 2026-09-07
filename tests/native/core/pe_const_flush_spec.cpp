#include "../../../src/d3d9/d3d9_pe_const_shadow.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

// R-CORE-REC-7.43 — the empty-dispatch policy is a four-cell value transform.
void testDispatchPredicateTruthTable() {
  check(!peConstFlushDispatchRequired(false, false),
        "clean shadow skips dispatch when empty observation is off");
  check(peConstFlushDispatchRequired(false, true),
        "dirty shadow dispatches when empty observation is off");
  check(peConstFlushDispatchRequired(true, false),
        "clean shadow dispatches when empty observation is on");
  check(peConstFlushDispatchRequired(true, true),
        "dirty shadow dispatches when empty observation is on");
}

// R-CORE-REC-7.43 — all 64 dirty-family masks are checked with observation
// both off and on, including the six empty observer-on callbacks.
void testDispatchSequenceExhaustiveMasks() {
  for (const bool observeEmpty : {false, true}) {
    for (unsigned mask = 0u; mask < (1u << 6u); ++mask) {
      PeConstShadowBlock shadows{};
      const std::array<ConstShadow*, 6> shadowList = {
          &shadows.vsConstF, &shadows.vsConstI, &shadows.vsConstB,
          &shadows.psConstF, &shadows.psConstI, &shadows.psConstB};
      for (std::size_t family = 0u; family < shadowList.size(); ++family) {
        if ((mask & (1u << family)) != 0u) {
          shadowList[family]->dirtyStart = 1u;
          shadowList[family]->dirtyEnd = 2u;
        }
      }

      std::array<std::size_t, 6> calls{};
      std::size_t callCount = 0u;
      const int result = dispatchPeConstFlushes(
          shadows, observeEmpty,
          [&](ConstShadow&, std::size_t family) noexcept {
            check(callCount < calls.size(), "flush dispatch does not overrun six families");
            calls[callCount++] = family;
            return 0;
          },
          [](int status) noexcept { return status < 0; });
      check(result == 0, "successful flush sequence returns success");

      std::size_t expectedCount = 0u;
      for (std::size_t family = 0u; family < shadowList.size(); ++family) {
        const bool expected = observeEmpty ||
            ((mask & (1u << family)) != 0u);
        if (!expected) continue;
        check(expectedCount < callCount, "expected flush family was dispatched");
        check(calls[expectedCount] == family,
              "flush families retain VS/PS float/int/bool ordering");
        ++expectedCount;
      }
      check(callCount == expectedCount,
            "dispatch count matches the observation/dirty truth table");
    }
  }
}

// R-CORE-REC-7.42/7.43 — production dispatch must check each family at the
// point it is reached, preserve the first failure, and leave later dirty work
// for a retry rather than snapshotting all six families up front.
void testDispatchStopsAtFirstFailure() {
  for (const bool observeEmpty : {false, true}) {
    for (std::size_t failureFamily = 0u; failureFamily < 6u; ++failureFamily) {
      PeConstShadowBlock shadows{};
      const std::array<ConstShadow*, 6> shadowList = {
          &shadows.vsConstF, &shadows.vsConstI, &shadows.vsConstB,
          &shadows.psConstF, &shadows.psConstI, &shadows.psConstB};
      for (ConstShadow* shadow : shadowList) {
        shadow->dirtyStart = 1u;
        shadow->dirtyEnd = 2u;
      }

      std::array<std::size_t, 6> calls{};
      std::size_t callCount = 0u;
      const int result = dispatchPeConstFlushes(
          shadows, observeEmpty,
          [&](ConstShadow&, std::size_t family) noexcept {
            calls[callCount++] = family;
            return family == failureFamily ? -1 : 0;
          },
          [](int status) noexcept { return status < 0; });
      check(result == -1, "first failed flush result propagates");
      check(callCount == failureFamily + 1u,
            "flush sequence stops immediately at first failure");
      for (std::size_t index = 0u; index < callCount; ++index) {
        check(calls[index] == index,
              "failure propagation does not reorder preceding flushes");
      }
    }
  }
}

void testDispatchSeesLaterMutationAndRetry() {
  PeConstShadowBlock shadows{};
  shadows.vsConstF.dirtyStart = 1u;
  shadows.vsConstF.dirtyEnd = 2u;

  std::array<std::size_t, 6> calls{};
  std::size_t callCount = 0u;
  const int firstResult = dispatchPeConstFlushes(
      shadows, false,
      [&](ConstShadow&, std::size_t family) noexcept {
        calls[callCount++] = family;
        shadows.psConstI.dirtyStart = 3u;
        shadows.psConstI.dirtyEnd = 4u;
        return -1;
      },
      [](int status) noexcept { return status < 0; });
  check(firstResult == -1, "later-family mutation keeps the first append failure");
  check(callCount == 1u && calls[0] == 0u,
        "first failure prevents dispatch past the failing family");

  callCount = 0u;
  const int retryResult = dispatchPeConstFlushes(
      shadows, false,
      [&](ConstShadow&, std::size_t family) noexcept {
        calls[callCount++] = family;
        return 0;
      },
      [](int status) noexcept { return status < 0; });
  check(retryResult == 0, "retry drains the failed and newly dirty families");
  check(callCount == 2u && calls[0] == 0u && calls[1] == 4u,
        "retry sees dirtiness at each family dispatch point");
}

// R-CORE-REC-7.43: compare the production dispatch with the old unconditional
// sequence. The shared fake append models payload acceptance, dirty settlement,
// first failure and callback mutation; it is not a Wine/Metal oracle.
void testSerialRefinement() {
  struct Run {
    PeConstShadowBlock shadows;
    PeDecimatedScopeStats stats;
    std::vector<std::uint8_t> records;
    int failureFamily = -1;
    std::uint32_t decimation = 0u;
    unsigned mutation = 0u;

    std::array<ConstShadow*, 6u> families() {
      return {&shadows.vsConstF, &shadows.vsConstI, &shadows.vsConstB,
              &shadows.psConstF, &shadows.psConstI, &shadows.psConstB};
    }

    int append(ConstShadow& shadow, std::size_t family) {
      if (decimation != 0u &&
          PeDecimatedScopeTimer::shouldSample(stats, decimation))
        PeDecimatedScopeTimer::recordSample(stats, 1u);
      if (!shadow.dirty()) return 0;
      if (static_cast<int>(family) == failureFamily) return -9;
      records.push_back(static_cast<std::uint8_t>(family));
      records.push_back(static_cast<std::uint8_t>(shadow.dirtyStart));
      records.push_back(static_cast<std::uint8_t>(shadow.dirtyEnd));
      records.insert(records.end(), shadow.values.begin(), shadow.values.end());
      shadow.clear();
      if (family == 0u && mutation != 0u) {
        auto& later = shadows.psConstI;
        if (mutation == 1u) {
          later.dirtyStart = 1u;
          later.dirtyEnd = 3u;
          later.dirtyElems[1] = 1u;
        } else {
          later.clear();
        }
      }
      return 0;
    }

    int execute(bool candidate) {
      if (candidate)
        return dispatchPeConstFlushes(
            shadows, decimation != 0u,
            [&](ConstShadow& s, std::size_t family) { return append(s, family); },
            [](int status) noexcept { return status < 0; });
      auto list = families();
      for (std::size_t family = 0u; family < list.size(); ++family) {
        const int status = append(*list[family], family);
        if (status < 0) return status;
      }
      return 0;
    }
  };
  const auto same = [](Run& reference, Run& candidate) {
    check(reference.records == candidate.records,
          "serial refinement preserves accepted payload and order");
    auto a = reference.families();
    auto b = candidate.families();
    for (std::size_t family = 0u; family < a.size(); ++family) {
      check(a[family]->values == b[family]->values &&
                a[family]->dirtyElems == b[family]->dirtyElems &&
                a[family]->dirtyStart == b[family]->dirtyStart &&
                a[family]->dirtyEnd == b[family]->dirtyEnd,
            "serial refinement preserves residual values and dirty state");
    }
    check(reference.stats.events == candidate.stats.events &&
              reference.stats.sampled == candidate.stats.sampled,
          "serial refinement preserves N=0/1/127 event and sample populations");
  };
  for (const auto n : {0u, 1u, 127u}) {
    for (unsigned mask = 0u; mask < 64u; ++mask) {
      for (int failure = -1; failure < 6; ++failure) {
        for (unsigned mutation = 0u; mutation < 3u; ++mutation) {
          Run reference;
          reference.decimation = n;
          reference.failureFamily = failure;
          reference.mutation = mutation;
          // Start near a sample boundary so N=127 is not a zero-sample pin.
          reference.stats.events = n == 127u ? 125u : 0u;
          auto list = reference.families();
          for (std::size_t family = 0u; family < list.size(); ++family) {
            auto& s = *list[family];
            const auto width = family == 2u || family == 5u ? 4u : 16u;
            s.values.resize(3u * width, static_cast<std::uint8_t>(family + 1u));
            s.dirtyElems.resize(3u, 0u);
            if ((mask & (1u << family)) != 0u) {
              s.dirtyStart = 1u;
              s.dirtyEnd = 3u;
              s.dirtyElems[1] = 1u;
            }
          }
          Run candidate = reference;
          check(reference.execute(false) == candidate.execute(true),
                "serial refinement preserves the first HRESULT failure");
          same(reference, candidate);
          reference.failureFamily = candidate.failureFamily = -1;
          check(reference.execute(false) == candidate.execute(true),
                "serial refinement preserves retry disposition");
          same(reference, candidate);
        }
      }
    }
  }
}

void testExceptionPropagation() {
  PeConstShadowBlock shadows;
  shadows.vsConstF.dirtyEnd = 1u;
  bool caught = false;
  try {
    (void)dispatchPeConstFlushes(
        shadows, false,
        [](ConstShadow&, std::size_t) -> int { throw std::runtime_error("append"); },
        [](int status) noexcept { return status < 0; });
  } catch (const std::runtime_error&) {
    caught = true;
  }
  check(caught && shadows.vsConstF.dirty(),
        "dispatch does not convert the previous throwing path to termination");
}

}  // namespace

int main() {
  testDispatchPredicateTruthTable();
  testDispatchSequenceExhaustiveMasks();
  testDispatchStopsAtFirstFailure();
  testDispatchSeesLaterMutationAndRetry();
  testSerialRefinement();
  testExceptionPropagation();
  return 0;
}
