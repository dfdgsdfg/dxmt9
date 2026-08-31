#pragma once

// Cold, opt-in fault injection for the common PE recorder envelope.  This is
// deliberately separate from PeStateBlockFaultPoint: the StateBlock selector
// is an existing nine-point COM contract and its point numbering is stable.
// The recorder selector is used by bounded fresh-process tests only.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

enum class PeRecorderFaultPoint : std::uint8_t {
  CapacityPreReserve,
  RetainAcquire,
  BridgePre,
  BridgeEntered,
  CaptureDisposition,
  CaptureThrow,
  Reset,
  Teardown,
  Count,
};

struct PeRecorderFaultConfig {
  std::uint32_t mask = 0u;
  std::int32_t hresult[static_cast<std::size_t>(PeRecorderFaultPoint::Count)]{};
  // `retain_acquire=<decimal>` is a bounded test budget: fail at the typed
  // retainer boundary after this many successful unique acquisitions.  A
  // hexadecimal value remains the explicit HRESULT spelling for this point.
  std::uint32_t retainSuccessesBeforeFailure = 0u;
};

constexpr std::uint32_t peRecorderFaultBit(PeRecorderFaultPoint point) noexcept {
  return 1u << static_cast<unsigned>(point);
}

// This is intentionally a line-oriented test receipt rather than a public
// ABI.  It is emitted only from the cold, fault-enabled consume path below;
// the runner uses the exact line to reject stale binaries and wrong seams.
inline bool peRecorderFaultReceiptLineMatches(const char *line,
                                              const char *selector) noexcept {
  constexpr const char prefix[] = "DXMT9_PE_RECORDER_FAULT_CONSUMED=";
  if (!line || !selector) return false;
  const char *expected = prefix;
  while (*expected != '\0' && *line == *expected) {
    ++line;
    ++expected;
  }
  if (*expected != '\0') return false;
  while (*selector != '\0' && *line == *selector) {
    ++line;
    ++selector;
  }
  return *selector == '\0' && *line == '\0';
}

inline void peRecorderFaultEmitReceipt(const char *selector) noexcept {
  if (!selector || *selector == '\0') return;
  std::fputs("DXMT9_PE_RECORDER_FAULT_CONSUMED=", stdout);
  std::fputs(selector, stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

inline bool peRecorderFaultName(const char *begin, const char *end,
                                PeRecorderFaultPoint &point) noexcept {
  constexpr PeRecorderFaultPoint points[] = {
      PeRecorderFaultPoint::CapacityPreReserve,
      PeRecorderFaultPoint::RetainAcquire,
      PeRecorderFaultPoint::BridgePre,
      PeRecorderFaultPoint::BridgeEntered,
      PeRecorderFaultPoint::CaptureDisposition,
      PeRecorderFaultPoint::CaptureThrow,
      PeRecorderFaultPoint::Reset,
      PeRecorderFaultPoint::Teardown,
  };
  constexpr const char *names[] = {
      "capacity_pre_reserve", "retain_acquire", "bridge_pre",
      "bridge_entered", "capture_disposition", "capture_throw", "reset",
      "teardown",
  };
  for (std::size_t i = 0u; i < sizeof(points) / sizeof(points[0]); ++i) {
    const char *cursor = begin;
    const char *text = names[i];
    while (cursor != end && *text != '\0' && *cursor == *text) {
      ++cursor;
      ++text;
    }
    if (cursor == end && *text == '\0') {
      point = points[i];
      return true;
    }
  }
  return false;
}

inline std::int32_t peRecorderFaultDefaultHresult(
    PeRecorderFaultPoint point) noexcept {
  return point == PeRecorderFaultPoint::CapacityPreReserve ||
                 point == PeRecorderFaultPoint::RetainAcquire
             ? static_cast<std::int32_t>(0x8007000eu) // E_OUTOFMEMORY
             : static_cast<std::int32_t>(0x80004005u); // E_FAIL
}

inline bool peRecorderFaultParseHresult(const char *begin, const char *end,
                                        std::int32_t &result) noexcept {
  if (begin == end) return false;
  unsigned base = 10u;
  if (end - begin > 2 && begin[0] == '0' &&
      (begin[1] == 'x' || begin[1] == 'X')) {
    base = 16u;
    begin += 2;
  }
  if (begin == end) return false;
  std::uint32_t value = 0u;
  for (; begin != end; ++begin) {
    unsigned digit = 0u;
    if (*begin >= '0' && *begin <= '9') digit = *begin - '0';
    else if (base == 16u && *begin >= 'a' && *begin <= 'f') digit = *begin - 'a' + 10u;
    else if (base == 16u && *begin >= 'A' && *begin <= 'F') digit = *begin - 'A' + 10u;
    else return false;
    if (digit >= base || value > (0xffffffffu - digit) / base) return false;
    value = value * base + digit;
  }
  result = static_cast<std::int32_t>(value);
  return true;
}

inline bool peRecorderFaultParseUnsigned(const char *begin, const char *end,
                                          std::uint32_t &result) noexcept {
  if (begin == end) return false;
  std::uint32_t value = 0u;
  for (; begin != end; ++begin) {
    if (*begin < '0' || *begin > '9') return false;
    const auto digit = static_cast<std::uint32_t>(*begin - '0');
    if (value > (0xffffffffu - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  result = value;
  return true;
}

inline PeRecorderFaultConfig peRecorderFaultConfigFromString(
    const char *value) noexcept {
  PeRecorderFaultConfig config{};
  if (!value || *value == '\0') return config;
  const char *cursor = value;
  while (*cursor != '\0') {
    const char *token = cursor;
    while (*cursor != '\0' && *cursor != ',') ++cursor;
    const char *equals = token;
    while (equals != cursor && *equals != '=') ++equals;
    PeRecorderFaultPoint point{};
    if (peRecorderFaultName(token, equals, point)) {
      std::int32_t hr = peRecorderFaultDefaultHresult(point);
      if (equals != cursor) {
        const char *parameter = equals + 1;
        std::uint32_t retainBudget = 0u;
        const bool parsedBudget =
            point == PeRecorderFaultPoint::RetainAcquire &&
            peRecorderFaultParseUnsigned(parameter, cursor, retainBudget);
        if (parsedBudget) {
          config.retainSuccessesBeforeFailure = retainBudget;
        } else if (!peRecorderFaultParseHresult(parameter, cursor, hr)) {
          hr = peRecorderFaultDefaultHresult(point);
        }
      }
      const auto index = static_cast<std::size_t>(point);
      config.hresult[index] = hr;
      config.mask |= peRecorderFaultBit(point);
    }
    if (*cursor == ',') ++cursor;
  }
  return config;
}

struct PeRecorderFaultState {
  static constexpr std::size_t maxSelectorBytes = 96u;

  PeRecorderFaultConfig config{};
  std::atomic<std::uint32_t> remainingMask{0u};
  // Own the process's selector snapshot. getenv() storage may be invalidated
  // by a later environment mutation, which would make the evidence receipt
  // itself nondeterministic if only the borrowed pointer were retained.
  char selector[maxSelectorBytes]{};
  // Retains are producer-thread confined by the recorder guard. Keep the
  // countdown out of the normal path's atomic state; it is only read after
  // the cached enabled branch at the actual unique-retain boundary.
  bool retainFaultArmed = false;
  std::uint32_t remainingRetainSuccesses = 0u;
  bool retainFaultResultPending = false;
  std::int32_t retainFaultResult = 0;
};

inline PeRecorderFaultState &peRecorderFaultState() noexcept {
  static PeRecorderFaultState state{};
  static const bool initialized = [&] {
    const char *selector = std::getenv("DXMT9_PE_RECORDER_FAULT");
    std::size_t selectorBytes = 0u;
    if (selector) {
      while (selector[selectorBytes] != '\0' &&
             selectorBytes + 1u < PeRecorderFaultState::maxSelectorBytes) {
        state.selector[selectorBytes] = selector[selectorBytes];
        ++selectorBytes;
      }
      if (selector[selectorBytes] != '\0') {
        // An unbounded or truncated selector cannot produce an exact receipt.
        state.selector[0] = '\0';
      }
    }
    state.config = peRecorderFaultConfigFromString(state.selector);
    state.remainingMask.store(state.config.mask, std::memory_order_relaxed);
    state.retainFaultArmed =
        (state.config.mask & peRecorderFaultBit(
            PeRecorderFaultPoint::RetainAcquire)) != 0u;
    state.remainingRetainSuccesses =
        state.config.retainSuccessesBeforeFailure;
    return true;
  }();
  (void)initialized;
  return state;
}

inline bool dxmt9PeConsumeRecorderRetainFault(
    std::int32_t &result) noexcept {
  auto &state = peRecorderFaultState();
  if (!state.retainFaultArmed) return false;
  if (state.remainingRetainSuccesses != 0u) {
    --state.remainingRetainSuccesses;
    return false;
  }
  state.retainFaultArmed = false;
  state.retainFaultResultPending = true;
  state.retainFaultResult = state.config.hresult[static_cast<std::size_t>(
      PeRecorderFaultPoint::RetainAcquire)];
  result = state.retainFaultResult;
  return true;
}

inline bool dxmt9PeTakeRecorderRetainFaultResult(
    std::int32_t &result) noexcept {
  auto &state = peRecorderFaultState();
  if (!state.retainFaultResultPending) return false;
  state.retainFaultResultPending = false;
  result = state.retainFaultResult;
  return true;
}

inline bool dxmt9PeRecorderFaultsEnabled() noexcept {
  static const bool enabled = peRecorderFaultState().config.mask != 0u;
  return enabled;
}

inline bool dxmt9PeConsumeRecorderFault(PeRecorderFaultPoint point,
                                        std::int32_t &result) noexcept {
  if (point == PeRecorderFaultPoint::RetainAcquire) {
    auto &state = peRecorderFaultState();
    const bool consumed = dxmt9PeConsumeRecorderRetainFault(result);
    if (consumed) peRecorderFaultEmitReceipt(state.selector);
    return consumed;
  }
  auto &state = peRecorderFaultState();
  const auto bit = peRecorderFaultBit(point);
  auto mask = state.remainingMask.load(std::memory_order_relaxed);
  while ((mask & bit) != 0u) {
    if (state.remainingMask.compare_exchange_weak(
            mask, mask & ~bit, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      result = state.config.hresult[static_cast<std::size_t>(point)];
      peRecorderFaultEmitReceipt(state.selector);
      return true;
    }
  }
  return false;
}
