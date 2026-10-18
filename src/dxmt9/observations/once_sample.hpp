#pragma once

#include <mutex>

namespace dxmt9::observations {

// Diagnostic source only: callers establish physical completion before read().
// Invalid results are cached too. A new owner lifetime gets a new cell.
template <typename T> class OnceSample {
public:
  template <typename Acquire> T read(Acquire&& acquire) noexcept {
    std::call_once(once_, [&] { value_ = acquire(); });
    return value_;
  }
private:
  std::once_flag once_;
  T value_{};
};

inline constexpr unsigned acquisitionVersion = 1;
}  // namespace dxmt9::observations
