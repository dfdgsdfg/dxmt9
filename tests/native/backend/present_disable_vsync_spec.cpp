// Pure-data spec for resolveDisableVsync() in dxmt9_presenter.hpp.
// Locks in the "0" / empty-string / unset treatment of the
// DXMT9_DISABLE_VSYNC env string.

#include "../../../src/dxmt9/dxmt9_presenter.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

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

void testUnsetIsFalse() {
  check(::dxmt9::resolveDisableVsync(nullptr) == false,
        "unset env (nullptr) keeps vsync enabled");
}

void testEmptyIsFalse() {
  check(::dxmt9::resolveDisableVsync("") == false,
        "empty-string env keeps vsync enabled");
}

void testLiteralZeroIsFalse() {
  check(::dxmt9::resolveDisableVsync("0") == false,
        "DXMT9_DISABLE_VSYNC=0 keeps vsync enabled");
}

void testZeroPrefixedStringIsFalse() {
  // Treat any value whose first character is '0' as "off". Matches the
  // sibling layerDisplaySyncEnabled() / envFlagSet() conventions.
  check(::dxmt9::resolveDisableVsync("0xff") == false,
        "string beginning with '0' is treated as off");
}

void testLiteralOneIsTrue() {
  check(::dxmt9::resolveDisableVsync("1") == true,
        "DXMT9_DISABLE_VSYNC=1 disables vsync");
}

void testYesIsTrue() {
  check(::dxmt9::resolveDisableVsync("yes") == true,
        "DXMT9_DISABLE_VSYNC=yes disables vsync");
}

void testNonZeroDigitIsTrue() {
  check(::dxmt9::resolveDisableVsync("2") == true,
        "DXMT9_DISABLE_VSYNC=2 disables vsync");
}

void testArbitraryStringIsTrue() {
  check(::dxmt9::resolveDisableVsync("anything") == true,
        "arbitrary non-zero env value disables vsync");
}

}  // namespace

int main() {
  try {
    testUnsetIsFalse();
    testEmptyIsFalse();
    testLiteralZeroIsFalse();
    testZeroPrefixedStringIsFalse();
    testLiteralOneIsTrue();
    testYesIsTrue();
    testNonZeroDigitIsTrue();
    testArbitraryStringIsTrue();
  } catch (const TestFailure& e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "FAIL (unexpected): " << e.what() << "\n";
    return 2;
  }
  std::cout << "ok\n";
  return 0;
}
