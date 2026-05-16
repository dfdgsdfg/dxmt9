#include "../../../src/dxmt9/dxmt9_debug_trace.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using u64 = std::uint64_t;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

dxmt9::debug::DrawSeqRange range(std::optional<u64> min, std::optional<u64> max) {
  return dxmt9::debug::makeDrawSeqRange(min, max);
}

dxmt9::debug::DrawOrdinalRange ordinalRange(std::optional<u64> min,
                                            std::optional<u64> max) {
  return dxmt9::debug::makeDrawOrdinalRange(min, max);
}

void testUnboundedRangeKeepsEverySeq() {
  const auto unbounded = range(std::nullopt, std::nullopt);
  check(!dxmt9::debug::drawSeqRangeEnabled(unbounded),
        "unbounded draw seq range is disabled");
  check(!dxmt9::debug::shouldSkipDrawSeq(0u, unbounded),
        "unbounded range keeps seq 0");
  check(!dxmt9::debug::shouldSkipDrawSeq(42u, unbounded),
        "unbounded range keeps normal seq");
}

void testMinBoundaryIsInclusive() {
  const auto minOnly = range(u64{10}, std::nullopt);
  check(dxmt9::debug::drawSeqRangeEnabled(minOnly),
        "min-only draw seq range is enabled");
  check(dxmt9::debug::shouldSkipDrawSeq(9u, minOnly),
        "min-only range skips seq below min");
  check(!dxmt9::debug::shouldSkipDrawSeq(10u, minOnly),
        "min-only range keeps seq at min");
  check(!dxmt9::debug::shouldSkipDrawSeq(11u, minOnly),
        "min-only range keeps seq above min");
}

void testMaxBoundaryIsInclusive() {
  const auto maxOnly = range(std::nullopt, u64{20});
  check(dxmt9::debug::drawSeqRangeEnabled(maxOnly),
        "max-only draw seq range is enabled");
  check(!dxmt9::debug::shouldSkipDrawSeq(19u, maxOnly),
        "max-only range keeps seq below max");
  check(!dxmt9::debug::shouldSkipDrawSeq(20u, maxOnly),
        "max-only range keeps seq at max");
  check(dxmt9::debug::shouldSkipDrawSeq(21u, maxOnly),
        "max-only range skips seq above max");
}

void testClosedRangeIsInclusive() {
  const auto closed = range(u64{10}, u64{20});
  check(dxmt9::debug::shouldSkipDrawSeq(9u, closed),
        "closed range skips seq below min");
  check(!dxmt9::debug::shouldSkipDrawSeq(10u, closed),
        "closed range keeps seq at min");
  check(!dxmt9::debug::shouldSkipDrawSeq(15u, closed),
        "closed range keeps seq inside range");
  check(!dxmt9::debug::shouldSkipDrawSeq(20u, closed),
        "closed range keeps seq at max");
  check(dxmt9::debug::shouldSkipDrawSeq(21u, closed),
        "closed range skips seq above max");
}

void testInvertedRangeSkipsEverySeq() {
  const auto inverted = range(u64{20}, u64{10});
  check(dxmt9::debug::shouldSkipDrawSeq(9u, inverted),
        "inverted range skips seq below both bounds");
  check(dxmt9::debug::shouldSkipDrawSeq(15u, inverted),
        "inverted range skips seq between inverted bounds");
  check(dxmt9::debug::shouldSkipDrawSeq(21u, inverted),
        "inverted range skips seq above both bounds");
}

void testDrawOrdinalRangeMatchesSeqRangeBoundaries() {
  const auto unbounded = ordinalRange(std::nullopt, std::nullopt);
  check(!dxmt9::debug::drawOrdinalRangeEnabled(unbounded),
        "unbounded draw ordinal range is disabled");
  check(!dxmt9::debug::shouldSkipDrawOrdinal(1u, unbounded),
        "unbounded draw ordinal range keeps ordinal 1");

  const auto closed = ordinalRange(u64{3}, u64{5});
  check(dxmt9::debug::drawOrdinalRangeEnabled(closed),
        "closed draw ordinal range is enabled");
  check(dxmt9::debug::shouldSkipDrawOrdinal(2u, closed),
        "closed draw ordinal range skips below min");
  check(!dxmt9::debug::shouldSkipDrawOrdinal(3u, closed),
        "closed draw ordinal range keeps min");
  check(!dxmt9::debug::shouldSkipDrawOrdinal(4u, closed),
        "closed draw ordinal range keeps inside");
  check(!dxmt9::debug::shouldSkipDrawOrdinal(5u, closed),
        "closed draw ordinal range keeps max");
  check(dxmt9::debug::shouldSkipDrawOrdinal(6u, closed),
        "closed draw ordinal range skips above max");

  const auto inverted = ordinalRange(u64{5}, u64{3});
  check(dxmt9::debug::shouldSkipDrawOrdinal(4u, inverted),
        "inverted draw ordinal range skips between inverted bounds");
}

}  // namespace

int main() {
  try {
    testUnboundedRangeKeepsEverySeq();
    testMinBoundaryIsInclusive();
    testMaxBoundaryIsInclusive();
    testClosedRangeIsInclusive();
    testInvertedRangeSkipsEverySeq();
    testDrawOrdinalRangeMatchesSeqRangeBoundaries();
  } catch (const TestFailure& failure) {
    std::cerr << "draw_seq_filter_spec failed: "
              << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "draw_seq_filter_spec unexpected exception: "
              << ex.what() << '\n';
    return 1;
  }

  std::cout << "draw_seq_filter_spec passed\n";
  return 0;
}
