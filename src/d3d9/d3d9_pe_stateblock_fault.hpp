#pragma once

// Cold, opt-in fault injection for the PE StateBlock transaction boundary.
// The selector is intentionally one process-global, one-shot seam: production
// runs pay only the cached `enabled` branch on these cold COM calls, while
// bounded tests can force a specific pre-effect or effect-unknown boundary.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

enum class PeStateBlockFaultPoint : std::uint8_t {
  CapturePre,
  ApplyPre,
  EndPre,
  AllocPre,
  BridgePre,
  CaptureEntered,
  ApplyEntered,
  EndEntered,
  BridgeEntered,
  Count,
};

struct PeStateBlockFaultConfig {
  std::uint32_t mask = 0u;
  std::int32_t hresult[static_cast<std::size_t>(PeStateBlockFaultPoint::Count)]{};
};

constexpr std::uint32_t peStateBlockFaultBit(PeStateBlockFaultPoint point) {
  return 1u << static_cast<unsigned>(point);
}

inline bool peStateBlockFaultName(const char* begin, const char* end,
                                  PeStateBlockFaultPoint& point) noexcept {
  const auto matches = [begin, end](const char* text) noexcept {
    const char* cursor = begin;
    while (cursor != end && *text != '\0' && *cursor == *text) {
      ++cursor;
      ++text;
    }
    return cursor == end && *text == '\0';
  };
  constexpr PeStateBlockFaultPoint points[] = {
      PeStateBlockFaultPoint::CapturePre,
      PeStateBlockFaultPoint::ApplyPre,
      PeStateBlockFaultPoint::EndPre,
      PeStateBlockFaultPoint::AllocPre,
      PeStateBlockFaultPoint::BridgePre,
      PeStateBlockFaultPoint::CaptureEntered,
      PeStateBlockFaultPoint::ApplyEntered,
      PeStateBlockFaultPoint::EndEntered,
      PeStateBlockFaultPoint::BridgeEntered,
  };
  constexpr const char* names[] = {
      "capture_pre", "apply_pre", "end_pre", "alloc_pre", "bridge_pre",
      "capture_entered", "apply_entered", "end_entered", "bridge_entered",
  };
  for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
    if (matches(names[i])) {
      point = points[i];
      return true;
    }
  }
  return false;
}

inline std::int32_t peStateBlockFaultDefaultHresult(
    PeStateBlockFaultPoint point) noexcept {
  // E_OUTOFMEMORY is the natural allocation seam result; the other injected
  // boundaries use E_FAIL unless a caller supplies an explicit HRESULT.
  return point == PeStateBlockFaultPoint::AllocPre
             ? static_cast<std::int32_t>(0x8007000eu)
             : static_cast<std::int32_t>(0x80004005u);
}

inline bool peStateBlockFaultParseHresult(const char* begin, const char* end,
                                          std::int32_t& result) noexcept {
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

inline PeStateBlockFaultConfig peStateBlockFaultConfigFromString(
    const char* value) noexcept {
  PeStateBlockFaultConfig config{};
  if (!value || *value == '\0') return config;
  const char* cursor = value;
  while (*cursor != '\0') {
    const char* token = cursor;
    while (*cursor != '\0' && *cursor != ',') ++cursor;
    const char* equals = token;
    while (equals != cursor && *equals != '=') ++equals;
    PeStateBlockFaultPoint point{};
    if (peStateBlockFaultName(token, equals, point)) {
      std::int32_t hr = peStateBlockFaultDefaultHresult(point);
      if (equals != cursor &&
          !peStateBlockFaultParseHresult(equals + 1, cursor, hr)) {
        hr = peStateBlockFaultDefaultHresult(point);
      }
      const auto index = static_cast<std::size_t>(point);
      config.hresult[index] = hr;
      config.mask |= peStateBlockFaultBit(point);
    }
    if (*cursor == ',') ++cursor;
  }
  return config;
}

struct PeStateBlockFaultState {
  PeStateBlockFaultConfig config{};
  std::atomic<std::uint32_t> remainingMask{0u};
};

inline PeStateBlockFaultState& peStateBlockFaultState() noexcept {
  static PeStateBlockFaultState state{};
  static const bool initialized = [&] {
    state.config = peStateBlockFaultConfigFromString(
        std::getenv("DXMT9_PE_STATEBLOCK_FAULT"));
    state.remainingMask.store(state.config.mask, std::memory_order_relaxed);
    return true;
  }();
  (void)initialized;
  return state;
}

inline bool dxmt9PeConsumeStateBlockFault(PeStateBlockFaultPoint point,
                                           std::int32_t& result) noexcept {
  auto& state = peStateBlockFaultState();
  const auto bit = peStateBlockFaultBit(point);
  auto mask = state.remainingMask.load(std::memory_order_relaxed);
  while ((mask & bit) != 0u) {
    if (state.remainingMask.compare_exchange_weak(
            mask, mask & ~bit, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      result = state.config.hresult[static_cast<std::size_t>(point)];
      return true;
    }
  }
  return false;
}

inline bool dxmt9PeConsumeStateBlockEnteredFault(
    PeStateBlockFaultPoint operation, std::int32_t& result) noexcept {
  return dxmt9PeConsumeStateBlockFault(operation, result) ||
         dxmt9PeConsumeStateBlockFault(PeStateBlockFaultPoint::BridgeEntered,
                                       result);
}
