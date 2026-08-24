#include "device_c_presence_table.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

template <typename T>
struct FailingAllocator {
  using value_type = T;
  bool* fail = nullptr;

  FailingAllocator() = default;
  explicit FailingAllocator(bool* value) : fail(value) {}
  template <typename U>
  FailingAllocator(const FailingAllocator<U>& other) : fail(other.fail) {}

  T* allocate(std::size_t count) {
    if (fail && *fail) throw std::bad_alloc();
    return std::allocator<T>{}.allocate(count);
  }
  void deallocate(T* value, std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(value, count);
  }
  template <typename U>
  bool operator==(const FailingAllocator<U>& other) const noexcept {
    return fail == other.fail;
  }
};

void testAllocationFailureSelectsLinearTruthSource() {
  bool fail = true;
  dxmt9::d3d9::PresenceTable<
      int, std::hash<int>, std::equal_to<int>, FailingAllocator<int>>
      presence{FailingAllocator<int>(&fail)};
  presence.reset(8u);
  check(presence.overflowed(),
        "actual vector allocator failure disables the accelerator");

  std::vector<int> truthSource{4, 9, 12};
  const auto contains = [&](int value) {
    return presence.overflowed()
               ? std::find(truthSource.begin(), truthSource.end(), value) !=
                     truthSource.end()
               : presence.contains(value);
  };
  check(contains(9) && !contains(7),
        "disabled accelerator falls back to complete linear truth source");

  fail = false;
  presence.reset(8u);
  check(!presence.overflowed() && presence.insert(9) && presence.contains(9),
        "later successful reset restores bounded acceleration");
}
}  // namespace

int main() {
  try {
    testAllocationFailureSelectsLinearTruthSource();
  } catch (const TestFailure& failure) {
    std::cerr << "presence_table_spec failed: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "presence_table_spec passed\n";
  return EXIT_SUCCESS;
}
