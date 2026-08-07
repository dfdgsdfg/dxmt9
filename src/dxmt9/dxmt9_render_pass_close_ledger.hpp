#pragma once

#include "dxmt9_encode_attribution.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_session_finalize_cause.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace dxmt9::encoders {

struct RenderPassCloseKey {
  std::uint64_t color0 = 0;
  std::uint64_t depth = 0;
  std::uint32_t sampleCount = 1;

  constexpr bool valid() const noexcept {
    return color0 != 0u || depth != 0u;
  }

  friend constexpr bool operator==(const RenderPassCloseKey&,
                                   const RenderPassCloseKey&) = default;
};

struct RenderPassCloseRecord {
  RenderPassInstanceToken token{};
  RenderPassCloseKey key{};
  perf::EncoderSplitReason splitReason = perf::EncoderSplitReason::Final;
  SessionFinalizeCause finalizeCause = SessionFinalizeCause::FailOrOther;
  std::uint64_t storeBytes = 0;
};

enum class RenderPassCloseTerminalRelation : std::uint8_t {
  None,
  AdjacentSameKey,
  AdjacentDifferentKey,
};

struct RenderPassCloseStartObservation {
  RenderPassCloseTerminalRelation terminal =
      RenderPassCloseTerminalRelation::None;
  SessionFinalizeCause terminalCause = SessionFinalizeCause::FailOrOther;
  std::optional<perf::EncoderSplitReason> terminalSplitReason{};
  bool shortCrossLookupAttempted = false;
  std::optional<perf::EncoderSplitReason> shortCrossPriorSplitReason{};
  std::uint64_t shortCrossPriorStoreBytes = 0;
};

struct RenderPassCloseFrameTerminal {
  std::size_t notReopenedBeforePresent = 0;
  std::size_t finalNotReopenedBeforePresent = 0;
};

// Encode-thread-only, allocation-free ledger. A close is terminalized by the
// exact immediately prior token at the next physical pass start. The record
// remains queryable until Present so a later short same-key re-entry can find
// the exact prior instance's close reason.
template <std::size_t Capacity = 128>
class RenderPassCloseLedger {
 public:
  bool noteClose(RenderPassCloseRecord record) noexcept {
    if (!record.token.valid() || !record.key.valid() || count_ == Capacity) {
      return false;
    }
    entries_[count_++] = Entry{.record = record};
    return true;
  }

  RenderPassCloseStartObservation noteStart(
      RenderPassInstanceToken immediatePriorToken,
      RenderPassCloseKey currentKey,
      std::optional<RenderPassInstanceToken> shortCrossPriorToken =
          std::nullopt) noexcept {
    RenderPassCloseStartObservation result{};
    if (immediatePriorToken.valid()) {
      if (Entry* prior = find(immediatePriorToken)) {
        if (!prior->terminalized) {
          prior->terminalized = true;
          result.terminalCause = prior->record.finalizeCause;
          result.terminalSplitReason = prior->record.splitReason;
          result.terminal = prior->record.key == currentKey
              ? RenderPassCloseTerminalRelation::AdjacentSameKey
              : RenderPassCloseTerminalRelation::AdjacentDifferentKey;
        }
      }
    }
    if (shortCrossPriorToken && shortCrossPriorToken->valid()) {
      result.shortCrossLookupAttempted = true;
      if (const Entry* prior = find(*shortCrossPriorToken);
          prior && prior->record.key == currentKey) {
        result.shortCrossPriorSplitReason = prior->record.splitReason;
        result.shortCrossPriorStoreBytes = prior->record.storeBytes;
      }
    }
    return result;
  }

  RenderPassCloseFrameTerminal finishFrame() noexcept {
    RenderPassCloseFrameTerminal result{};
    for (std::size_t i = 0; i < count_; ++i) {
      if (!entries_[i].terminalized) {
        ++result.notReopenedBeforePresent;
        if (entries_[i].record.splitReason ==
            perf::EncoderSplitReason::Final) {
          ++result.finalNotReopenedBeforePresent;
        }
      }
    }
    entries_ = {};
    count_ = 0;
    return result;
  }

  std::size_t size() const noexcept { return count_; }

 private:
  struct Entry {
    RenderPassCloseRecord record{};
    bool terminalized = false;
  };

  Entry* find(RenderPassInstanceToken token) noexcept {
    for (std::size_t i = count_; i > 0; --i) {
      if (entries_[i - 1u].record.token == token) {
        return &entries_[i - 1u];
      }
    }
    return nullptr;
  }

  const Entry* find(RenderPassInstanceToken token) const noexcept {
    for (std::size_t i = count_; i > 0; --i) {
      if (entries_[i - 1u].record.token == token) {
        return &entries_[i - 1u];
      }
    }
    return nullptr;
  }

  std::array<Entry, Capacity> entries_{};
  std::size_t count_ = 0;
};

static_assert(std::is_trivially_copyable_v<RenderPassCloseKey>);
static_assert(std::is_standard_layout_v<RenderPassCloseKey>);
static_assert(std::is_trivially_copyable_v<RenderPassCloseRecord>);
static_assert(std::is_standard_layout_v<RenderPassCloseRecord>);

}  // namespace dxmt9::encoders
