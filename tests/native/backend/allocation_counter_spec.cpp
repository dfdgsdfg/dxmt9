#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void requirePerfCountersEnabled() {
  if (!dxmt9::perf::enabled()) {
    throw TestFailure("DXMT_PERF_COUNTERS must be enabled by the test harness");
  }
}

}  // namespace

int main() {
  try {
    requirePerfCountersEnabled();
    dxmt9::perf::countMetalBuffer(256);
    dxmt9::perf::countMetalBuffer(512);
    dxmt9::perf::countDrawGeometryDiagnostics(
        true,   // fixed-function path
        true,   // indexed
        false,  // UInt16 index buffer
        false,  // not direct
        true,   // UP payload
        false,  // not expanded
        true,   // non-zero base vertex
        true,   // non-zero start index
        true,   // non-zero stream0 offset
        32,
        0x1234);
  } catch (const TestFailure& e) {
    std::cerr << "allocation_counter_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "allocation_counter_spec unexpected exception: " << e.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "allocation_counter_spec passed\n";
  return EXIT_SUCCESS;
}
