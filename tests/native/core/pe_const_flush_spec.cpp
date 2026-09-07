#include "../../../src/d3d9/d3d9_pe_const_shadow.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
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

void clearConstShadowReference(ConstShadow& shadow) {
  shadow.dirtyStart = shadow.dirtyEnd = 0u;
  std::fill(shadow.dirtyElems.begin(), shadow.dirtyElems.end(),
            std::uint8_t{0});
}

std::vector<std::uint8_t> constFlushTraceBytes(std::size_t elemSize,
                                             std::uint32_t seed,
                                             std::uint32_t count) {
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count) * elemSize);
  for (std::size_t i = 0u; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(seed + i * 13u + 1u);
  }
  return bytes;
}

std::size_t constFlushTraceElementSize(std::size_t family) {
  return family == 2u || family == 5u ? 4u : 16u;
}

std::uint32_t constFlushTraceHighRegister(std::size_t family) {
  if (family == 0u) return 240u;
  if (family == 3u) return 208u;
  return 12u;
}

ConstShadow& constFlushTraceFamily(PeConstShadowBlock& shadows,
                                   std::size_t family) {
  switch (family) {
  case 0u: return shadows.vsConstF;
  case 1u: return shadows.vsConstI;
  case 2u: return shadows.vsConstB;
  case 3u: return shadows.psConstF;
  case 4u: return shadows.psConstI;
  default: return shadows.psConstB;
  }
}

const ConstShadow& constFlushTraceFamily(const PeConstShadowBlock& shadows,
                                         std::size_t family) {
  switch (family) {
  case 0u: return shadows.vsConstF;
  case 1u: return shadows.vsConstI;
  case 2u: return shadows.vsConstB;
  case 3u: return shadows.psConstF;
  case 4u: return shadows.psConstI;
  default: return shadows.psConstB;
  }
}

struct ConstFlushTraceRecord {
  std::size_t family = 0u;
  std::uint32_t start = 0u;
  std::uint32_t count = 0u;
  std::vector<std::uint8_t> bytes;

  bool operator==(const ConstFlushTraceRecord&) const = default;
};

struct ConstFlushDifferentialTrace {
  PeConstShadowBlock candidate;
  PeConstShadowBlock reference;
  std::vector<ConstFlushTraceRecord> candidateRecords;
  std::vector<ConstFlushTraceRecord> referenceRecords;
  std::vector<std::size_t> candidateCallbacks;
  std::vector<std::size_t> referenceCallbacks;
  std::size_t family = 0u;
  std::size_t elemSize = 0u;

  void compare() const {
    for (std::size_t index = 0u; index < 6u; ++index) {
      const auto& actual = constFlushTraceFamily(candidate, index);
      const auto& expected = constFlushTraceFamily(reference, index);
      check(actual.dirtyStart <= actual.dirtyEnd &&
                actual.dirtyEnd <= actual.dirtyElems.size(),
            "production writes preserve bounded dirty interval");
      for (std::size_t reg = 0u; reg < actual.dirtyElems.size(); ++reg) {
        check(actual.dirtyElems[reg] == 0u ||
                  (actual.dirtyStart <= reg && reg < actual.dirtyEnd),
              "production dirty interval contains every pending bit");
      }
      check(actual.values == expected.values,
            "clear differential preserves materialized values");
      check(actual.dirtyElems == expected.dirtyElems,
            "clear differential preserves dirty bitmap membership");
      check(actual.dirtyStart == expected.dirtyStart &&
                actual.dirtyEnd == expected.dirtyEnd,
            "clear differential preserves dirty interval");
      check(actual.values.size() == expected.values.size() &&
                actual.dirtyElems.size() == expected.dirtyElems.size(),
            "clear differential preserves materialized extents");
      check(actual.values.capacity() == expected.values.capacity() &&
                actual.dirtyElems.capacity() == expected.dirtyElems.capacity(),
            "clear differential preserves capacity");
    }
    check(candidateRecords == referenceRecords,
          "clear differential preserves accepted flush payloads");
    check(candidateCallbacks == referenceCallbacks,
          "clear differential preserves flush diagnostic membership");
  }

  void apply(ConstShadow& shadow, std::uint32_t start, std::uint32_t count,
             const std::vector<std::uint8_t>& bytes, bool fixedWidth) {
    if (!fixedWidth) {
      touchConstShadow(shadow, start, count, bytes.data(), elemSize);
      return;
    }
    switch (elemSize) {
    case 4u:
      applyConstShadowLiveFixedWidth<4u>(shadow, start, count, bytes.data());
      return;
    case 16u:
      applyConstShadowLiveFixedWidth<16u>(shadow, start, count, bytes.data());
      return;
    default:
      check(false, "clear differential uses a supported constant width");
    }
  }

  void write(std::uint32_t start, std::uint32_t count,
             const std::vector<std::uint8_t>& bytes, bool fixedWidth) {
    apply(constFlushTraceFamily(candidate, family), start, count, bytes,
          fixedWidth);
    apply(constFlushTraceFamily(reference, family), start, count, bytes,
          fixedWidth);
    compare();
  }

  void restore(StateBlockConstShadow& recorded) {
    const auto publish = [&](ConstShadow& shadow) {
      (void)recorded.forEachRange(
          elemSize, [&](std::uint32_t start, std::uint32_t count,
                        const std::uint8_t* bytes) {
            const std::size_t begin = static_cast<std::size_t>(start) * elemSize;
            const std::size_t end = begin + static_cast<std::size_t>(count) * elemSize;
            if (shadow.values.size() < end) shadow.values.resize(end);
            if (shadow.dirtyElems.size() <
                static_cast<std::size_t>(start) + count) {
              shadow.dirtyElems.resize(static_cast<std::size_t>(start) + count);
            }
            // This is the production StateBlockApply commit shape: restored
            // ranges publish bytes first, then mark the complete tracked
            // range pending even when a byte equals the live value.
            std::memcpy(shadow.values.data() + begin, bytes, end - begin);
            std::fill(shadow.dirtyElems.begin() + start,
                      shadow.dirtyElems.begin() + start + count,
                      std::uint8_t{1});
            if (!shadow.dirty()) {
              shadow.dirtyStart = start;
              shadow.dirtyEnd = start + count;
            } else {
              shadow.dirtyStart = std::min(shadow.dirtyStart, start);
              shadow.dirtyEnd = std::max(shadow.dirtyEnd, start + count);
            }
            return true;
          });
    };
    publish(constFlushTraceFamily(candidate, family));
    publish(constFlushTraceFamily(reference, family));
    compare();
  }

  int flush(bool candidatePath, bool observeEmpty, int failureFamily) {
    auto& shadows = candidatePath ? candidate : reference;
    auto& callbacks = candidatePath ? candidateCallbacks : referenceCallbacks;
    auto& records = candidatePath ? candidateRecords : referenceRecords;
    return dispatchPeConstFlushes(
        shadows, observeEmpty,
        [&](ConstShadow& shadow, std::size_t flushFamily) {
          callbacks.push_back(flushFamily);
          if (!shadow.dirty()) return 0;
          if (static_cast<int>(flushFamily) == failureFamily) return -9;
          records.push_back(ConstFlushTraceRecord{
              flushFamily, shadow.dirtyStart,
              shadow.dirtyEnd - shadow.dirtyStart,
              std::vector<std::uint8_t>(
                  shadow.values.begin() +
                      static_cast<std::size_t>(shadow.dirtyStart) *
                          constFlushTraceElementSize(flushFamily),
                  shadow.values.begin() +
                      static_cast<std::size_t>(shadow.dirtyEnd) *
                          constFlushTraceElementSize(flushFamily))});
          if (candidatePath) {
            shadow.clear();
          } else {
            clearConstShadowReference(shadow);
          }
          return 0;
        },
        [](int status) noexcept { return status < 0; });
  }
};

void testConstShadowClearDifferential() {
  // Keep this trace bounded but cover every production family and both
  // fixed-width and generic reducer entry points. The reference differs only
  // in using the former full-vector settlement operation.
  for (const bool fixedWidth : {false, true}) {
    for (std::size_t family = 0u; family < 6u; ++family) {
      ConstFlushDifferentialTrace trace;
      trace.family = family;
      trace.elemSize = constFlushTraceElementSize(family);
      const auto high = constFlushTraceHighRegister(family);

      trace.write(high, 1u,
                  constFlushTraceBytes(trace.elemSize, 3u, 1u), fixedWidth);
      const auto candidateValuesCapacity =
          constFlushTraceFamily(trace.candidate, family).values.capacity();
      const auto candidateElemsCapacity =
          constFlushTraceFamily(trace.candidate, family).dirtyElems.capacity();
      check(constFlushTraceFamily(trace.candidate, family).dirty(),
            "reset probe starts with pending high-register write");
      trace.candidate.reset();
      for (std::size_t resetFamily = 0u; resetFamily < 6u; ++resetFamily) {
        auto& reference = constFlushTraceFamily(trace.reference, resetFamily);
        reference.values.clear();
        reference.dirtyElems.clear();
        clearConstShadowReference(reference);
      }
      trace.compare();
      check(constFlushTraceFamily(trace.candidate, family).values.size() == 0u &&
                constFlushTraceFamily(trace.candidate, family).dirtyElems.size() == 0u,
            "reset while dirty clears both materialized extents");
      check(constFlushTraceFamily(trace.candidate, family).values.capacity() ==
                candidateValuesCapacity &&
                constFlushTraceFamily(trace.candidate, family).dirtyElems.capacity() ==
                    candidateElemsCapacity,
            "reset while dirty retains capacity");
      trace.write(0u, 1u,
                  constFlushTraceBytes(trace.elemSize, 4u, 1u), fixedWidth);
      check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
            "post-reset low-write flush settles identically");
      trace.compare();

      trace.write(high, 1u,
                  constFlushTraceBytes(trace.elemSize, 5u, 1u), fixedWidth);
      check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
            "high-register flush settles identically");
      trace.compare();

      // Repeated low writes after a high-register materialization exercise the
      // grow-only bitmap extent without reintroducing historical dirtiness.
      for (std::uint32_t seed = 10u; seed < 14u; ++seed) {
        trace.write(0u, 1u,
                    constFlushTraceBytes(trace.elemSize, seed, 1u),
                    fixedWidth);
        check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
              "repeated low-write flush settles identically");
        trace.compare();
      }

      trace.write(2u, 1u,
                  constFlushTraceBytes(trace.elemSize, 21u, 1u), fixedWidth);
      trace.write(high - 1u, 1u,
                  constFlushTraceBytes(trace.elemSize, 22u, 1u), fixedWidth);
      check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
            "disjoint writes retain one merged flush range");
      trace.compare();

      const auto equal = constFlushTraceBytes(trace.elemSize, 31u, 2u);
      trace.write(4u, 2u, equal, fixedWidth);
      check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
            "equal-write seed flush settles identically");
      trace.write(4u, 2u, equal, fixedWidth);
      check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
            "equal writes do not create a flush record");
      trace.compare();

      const auto a = constFlushTraceBytes(trace.elemSize, 41u, 1u);
      const auto b = constFlushTraceBytes(trace.elemSize, 51u, 1u);
      trace.write(8u, 1u, a, fixedWidth);
      check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
            "A seed flush settles identically");
      trace.write(8u, 1u, b, fixedWidth);
      trace.write(8u, 1u, a, fixedWidth);
      check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
            "A/B/A flush settles identically");
      trace.compare();

      trace.write(9u, 2u,
                  constFlushTraceBytes(trace.elemSize, 61u, 2u), fixedWidth);
      check(trace.flush(true, false, static_cast<int>(family)) ==
                trace.flush(false, false, static_cast<int>(family)),
            "failed flush preserves pending settlement for retry");
      trace.compare();
      check(constFlushTraceFamily(trace.candidate, family).dirty() &&
                constFlushTraceFamily(trace.reference, family).dirty(),
            "failed flush leaves the dirty interval pending");
      check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
            "retry flush settles identically");
      trace.compare();

      StateBlockConstShadow recorded;
      const auto restoredA = constFlushTraceBytes(trace.elemSize, 71u, 1u);
      const auto restoredB = constFlushTraceBytes(trace.elemSize, 81u, 2u);
      recorded.record(3u, 1u, restoredA.data(), trace.elemSize);
      recorded.record(high - 3u, 2u, restoredB.data(), trace.elemSize);
      trace.restore(recorded);
      check(trace.flush(true, false, -1) == trace.flush(false, false, -1),
            "StateBlock-like restored ranges flush identically");
      trace.compare();
    }
  }
}

void testConstShadowClearValidMasks() {
  // Every mask is a valid dirty membership: dirtyStart/dirtyEnd encloses the
  // first and last pending bits, while holes inside that interval are clean.
  // This compares the candidate against the old full-clear reference without
  // manufacturing an impossible out-of-interval pending bit.
  for (unsigned mask = 0u; mask < (1u << 8u); ++mask) {
    ConstShadow candidate;
    candidate.values.resize(8u, 0x5au);
    candidate.dirtyElems.resize(8u, 0u);
    std::uint32_t first = 8u;
    std::uint32_t last = 0u;
    for (std::uint32_t reg = 0u; reg < 8u; ++reg) {
      if ((mask & (1u << reg)) == 0u) continue;
      candidate.dirtyElems[reg] = 1u;
      first = std::min(first, reg);
      last = reg + 1u;
    }
    candidate.dirtyStart = mask == 0u ? 0u : first;
    candidate.dirtyEnd = mask == 0u ? 0u : last;
    ConstShadow reference = candidate;
    const auto values = candidate.values;

    candidate.clear();
    clearConstShadowReference(reference);
    check(candidate.values == values && candidate.values == reference.values,
          "bounded clear preserves all materialized bytes");
    check(candidate.dirtyElems == reference.dirtyElems,
          "bounded clear matches full-clear bitmap membership");
    check(std::all_of(candidate.dirtyElems.begin(), candidate.dirtyElems.end(),
                      [](std::uint8_t value) { return value == 0u; }),
          "bounded clear settles every valid membership mask");
    check(candidate.dirtyStart == 0u && candidate.dirtyEnd == 0u &&
              reference.dirtyStart == 0u && reference.dirtyEnd == 0u,
          "bounded clear and reference reset zero bounds");
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
  testConstShadowClearDifferential();
  testConstShadowClearValidMasks();
  testDispatchPredicateTruthTable();
  testDispatchSequenceExhaustiveMasks();
  testDispatchStopsAtFirstFailure();
  testDispatchSeesLaterMutationAndRetry();
  testSerialRefinement();
  testExceptionPropagation();
  return 0;
}
