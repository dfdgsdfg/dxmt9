#pragma once

#include <memory>
#include <utility>

namespace dxmt9::detail {

// TLA+: DrawableToken!Complete / Fail. Fulfilment is single-use; the first
// complete/fail transition wins and no later caller may replace its result.
constexpr bool drawableTokenMayFulfill(bool ready) noexcept {
  return !ready;
}

// TLA+: DrawableToken!StashToken / Take. Under the caller's queue-registry
// lock, the slot owns at most one token, refuses overwrite, and becomes empty
// on the first take.
template <typename Token>
class SingleUseTokenSlot {
 public:
  bool stash(std::shared_ptr<Token> token) noexcept {
    if (!token || pending_) {
      return false;
    }
    pending_ = std::move(token);
    return true;
  }

  std::shared_ptr<Token> take() noexcept {
    return std::exchange(pending_, {});
  }

  void reset() noexcept { pending_.reset(); }
  bool occupied() const noexcept { return static_cast<bool>(pending_); }

 private:
  std::shared_ptr<Token> pending_{};
};

}  // namespace dxmt9::detail
