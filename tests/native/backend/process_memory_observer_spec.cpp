#include "dxmt9/dxmt9_process_memory_observer.hpp"

#include <cstdio>
#include <cstdlib>

using namespace dxmt9::perf;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

void clipsCoalescesAndFindsLargestGap() {
  LowAddressMapSummary state{};
  state = addLowAddressMappedInterval(state, 0x1000, 0x3000);
  state = addLowAddressMappedInterval(state, 0x2000, 0x5000);
  state = addLowAddressMappedInterval(state, 0x100000000ull - 0x2000,
                                      0x100000000ull + 0x4000);
  state = finishLowAddressMapSummary(state);
  check(state.valid && state.mappedBytes == 0x6000,
        "overlap is counted once and the final interval clips at 4 GiB");
  check(state.largestGapBytes == 0x100000000ull - 0x7000,
        "the largest low-address gap is exact");
}

void containedAndEndClippedRangesRemainExact() {
  LowAddressMapSummary state{};
  state = addLowAddressMappedInterval(state, 0x1000, 0x5000);
  state = addLowAddressMappedInterval(state, 0x2000, 0x3000);
  state = addLowAddressMappedInterval(state, kLow4GiBDomainEnd - 0x100,
                                      kLow4GiBDomainEnd + 0x100);
  state = finishLowAddressMapSummary(state);
  check(state.valid && state.mappedBytes == 0x4100 &&
            state.largestGapBytes == kLow4GiBDomainEnd - 0x100 - 0x5000,
        "contained overlap and high-end clipping preserve mapped bytes");
}

void emptyAndMalformedDomainsAreExplicit() {
  const auto empty = finishLowAddressMapSummary({});
  check(empty.valid && empty.mappedBytes == 0 &&
            empty.largestGapBytes == kLow4GiBDomainEnd,
        "an empty map reports the whole low-4-GiB domain as free");
  const auto malformed = addLowAddressMappedInterval({}, 9, 4);
  check(!malformed.valid,
        "a reversed Mach interval fails closed instead of inventing evidence");
}

}  // namespace

int main() {
  clipsCoalescesAndFindsLargestGap();
  containedAndEndClippedRangesRemainExact();
  emptyAndMalformedDomainsAreExplicit();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("process memory observer spec ok\n");
  return EXIT_SUCCESS;
}
