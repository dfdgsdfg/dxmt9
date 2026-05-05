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
