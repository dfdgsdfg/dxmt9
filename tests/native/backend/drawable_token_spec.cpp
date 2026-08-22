#include "../../../src/dxmt9/dxmt9_drawable_token_state.hpp"
#include "../../../src/dxmt9/dxmt9_presenter.hpp"

#include <exception>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

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

void drawableTokenTransitionTruthTable() {
  check(dxmt9::detail::drawableTokenMayFulfill(false),
        "DrawableToken pending state admits complete/fail");
  check(!dxmt9::detail::drawableTokenMayFulfill(true),
        "DrawableToken fulfilled state rejects double complete/fail");
}

void drawableTokenSlotStashTakeIsSingleUse() {
  dxmt9::detail::SingleUseTokenSlot<int> slot;
  auto first = std::make_shared<int>(11);
  auto duplicate = std::make_shared<int>(22);

  check(!slot.stash({}) && !slot.occupied(),
        "DrawableToken null acquisition fallback is not a Stash transition");
  check(slot.stash(first) && slot.occupied(),
        "DrawableToken Stash stores one token");
  check(!slot.stash(duplicate),
        "DrawableToken Stash refuses to overwrite an untaken token");
  auto taken = slot.take();
  check(taken == first && !slot.occupied(),
        "DrawableToken Take returns the stashed identity and empties the slot");
  check(!slot.take(), "DrawableToken second Take is empty");
}

void drawableTokenCompleteBeforeWaitReturns() {
  dxmt9::PresentDrawableToken token;
  token.complete({});
  check(!token.waitDrawable(),
        "DrawableToken complete-before-wait returns the fulfilled value");
}

void drawableTokenWaitThenFailReturnsWithoutTiming() {
  dxmt9::PresentDrawableToken token;
  std::latch waiterStarted{1};
  WMT::MetalDrawable result{};
  std::thread waiter([&] {
    waiterStarted.count_down();
    result = token.waitDrawable();
  });
  waiterStarted.wait();
  token.fail();
  waiter.join();
  check(!result, "DrawableToken fail releases a waiter with null drawable");
}

}  // namespace

int main() {
  try {
    drawableTokenTransitionTruthTable();
    drawableTokenSlotStashTakeIsSingleUse();
    drawableTokenCompleteBeforeWaitReturns();
    drawableTokenWaitThenFailReturnsWithoutTiming();
  } catch (const TestFailure& failure) {
    std::cerr << "drawable_token_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "drawable_token_spec unexpected exception: " << ex.what()
              << '\n';
    return 1;
  }
  std::cout << "drawable_token_spec passed\n";
  return 0;
}
