#pragma once

// Value-only contracts for post-encode payload retirement. Receipt values do
// not resolve Tape storage; the queue-owned ledger below is the only mutable
// authority that can activate, submit, and consume them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace dxmt9::core::metalqueue {
class QueueCompletionSpanAuthority;
}

namespace dxmt9::encoders {

// Stable attribution after synchronous encoding has consumed the source
// payload. Conversion from a locator-bearing PublishedCommandRef is restricted
// to the encode seam; this value cannot be used to recover Tape storage.
struct EncodedCommandId {
  std::uint64_t seqId = 0;
  std::uint32_t commandIndex = std::numeric_limits<std::uint32_t>::max();

  constexpr bool valid() const noexcept {
    return seqId != 0u &&
           commandIndex != std::numeric_limits<std::uint32_t>::max();
  }

  friend constexpr bool operator==(EncodedCommandId,
                                   EncodedCommandId) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<EncodedCommandId>);
static_assert(std::is_standard_layout_v<EncodedCommandId>);
static_assert(sizeof(EncodedCommandId) == 16u);

class SessionCompletionAccumulator;
class EncodedCompletionSpan;

// Unverified value summary of completion-source order. `sourceCount` is stored
// explicitly and checked against the dense endpoint width. This is not a queue
// seal or a completion capability. Only a future queue-owned authority may
// promote one into production completion state. SessionCompletionAccumulator
// is intentionally a pure candidate builder.
class UnverifiedEncodedCompletionSpan {
 public:
  constexpr bool valid() const noexcept {
    return firstSeqId_ != 0u && lastSeqId_ >= firstSeqId_ &&
           sourceCount_ != 0u &&
           lastSeqId_ - firstSeqId_ !=
               std::numeric_limits<std::uint64_t>::max() &&
           sourceCount_ == lastSeqId_ - firstSeqId_ + 1u;
  }

  constexpr std::uint64_t firstSeqId() const noexcept { return firstSeqId_; }
  constexpr std::uint64_t lastSeqId() const noexcept { return lastSeqId_; }
  constexpr std::uint64_t sourceCount() const noexcept { return sourceCount_; }
  constexpr bool tailHasPresent() const noexcept { return tailHasPresent_; }

  friend constexpr bool operator==(const UnverifiedEncodedCompletionSpan&,
                                   const UnverifiedEncodedCompletionSpan&) =
      default;

 private:
  friend class SessionCompletionAccumulator;

  constexpr UnverifiedEncodedCompletionSpan(std::uint64_t firstSeqId,
                                            std::uint64_t lastSeqId,
                                            std::uint64_t sourceCount,
                                            bool tailHasPresent) noexcept
      : firstSeqId_(firstSeqId),
        lastSeqId_(lastSeqId),
        sourceCount_(sourceCount),
        tailHasPresent_(tailHasPresent) {}

  std::uint64_t firstSeqId_ = 0;
  std::uint64_t lastSeqId_ = 0;
  std::uint64_t sourceCount_ = 0;
  bool tailHasPresent_ = false;
};

static_assert(std::is_trivially_copyable_v<UnverifiedEncodedCompletionSpan>);
static_assert(std::is_standard_layout_v<UnverifiedEncodedCompletionSpan>);

// Queue-sealed completion shadow. Construction is intentionally unavailable
// outside the queue-internal authority; copying the value does not grant Tape
// lookup or reclaim authority.
class EncodedCompletionSpan {
 public:
  constexpr std::uint64_t firstSeqId() const noexcept { return firstSeqId_; }
  constexpr std::uint64_t lastSeqId() const noexcept { return lastSeqId_; }
  constexpr std::uint64_t sourceCount() const noexcept { return sourceCount_; }
  constexpr bool tailHasPresent() const noexcept { return tailHasPresent_; }

  friend constexpr bool operator==(const EncodedCompletionSpan&,
                                   const EncodedCompletionSpan&) = default;

 private:
  friend class core::metalqueue::QueueCompletionSpanAuthority;

  constexpr EncodedCompletionSpan(std::uint64_t firstSeqId,
                                  std::uint64_t lastSeqId,
                                  std::uint64_t sourceCount,
                                  bool tailHasPresent) noexcept
      : firstSeqId_(firstSeqId),
        lastSeqId_(lastSeqId),
        sourceCount_(sourceCount),
        tailHasPresent_(tailHasPresent) {}

  std::uint64_t firstSeqId_ = 0;
  std::uint64_t lastSeqId_ = 0;
  std::uint64_t sourceCount_ = 0;
  bool tailHasPresent_ = false;
};

static_assert(std::is_trivially_copyable_v<EncodedCompletionSpan>);
static_assert(std::is_standard_layout_v<EncodedCompletionSpan>);

enum class CompletionSpanAppendResult : std::uint8_t {
  Appended,
  InvalidSequence,
  NotStrictlyIncreasing,
  NonContiguous,
  TailHasPresent,
  CountOverflow,
};

// Pure candidate builder for UnverifiedEncodedCompletionSpan. The input is one
// checked source value at a time, so arbitrary numeric ranges cannot be
// synthesized from endpoints. It does not prove that a source was sealed,
// encoded, submitted, or queue-owned. Current production source seqIds are
// dense even when StateOnly or Legacy raw work interposes: those paths affect
// rawOrdinal, not the published-source seqId allocator.
class SessionCompletionAccumulator {
 public:
  constexpr explicit SessionCompletionAccumulator(
      std::uint64_t maxSourceCount =
          std::numeric_limits<std::uint64_t>::max()) noexcept
      : maxSourceCount_(maxSourceCount) {}

  constexpr CompletionSpanAppendResult append(
      std::uint64_t seqId,
      bool hasPresent = false) noexcept {
    if (seqId == 0u) {
      return CompletionSpanAppendResult::InvalidSequence;
    }
    if (span_.valid()) {
      if (span_.tailHasPresent_) {
        return CompletionSpanAppendResult::TailHasPresent;
      }
      if (seqId <= span_.lastSeqId_) {
        return CompletionSpanAppendResult::NotStrictlyIncreasing;
      }
      if (span_.lastSeqId_ == std::numeric_limits<std::uint64_t>::max() ||
          seqId != span_.lastSeqId_ + 1u) {
        return CompletionSpanAppendResult::NonContiguous;
      }
      if (span_.sourceCount_ == maxSourceCount_ ||
          span_.sourceCount_ == std::numeric_limits<std::uint64_t>::max()) {
        return CompletionSpanAppendResult::CountOverflow;
      }
      span_.lastSeqId_ = seqId;
      ++span_.sourceCount_;
      span_.tailHasPresent_ = hasPresent;
      return CompletionSpanAppendResult::Appended;
    }
    if (maxSourceCount_ == 0u) {
      return CompletionSpanAppendResult::CountOverflow;
    }
    span_ = UnverifiedEncodedCompletionSpan(seqId, seqId, 1u, hasPresent);
    return CompletionSpanAppendResult::Appended;
  }

  constexpr CompletionSpanAppendResult merge(
      const SessionCompletionAccumulator& tail) noexcept {
    if (!tail.span_.valid()) {
      return CompletionSpanAppendResult::InvalidSequence;
    }
    if (!span_.valid()) {
      if (tail.span_.sourceCount_ > maxSourceCount_) {
        return CompletionSpanAppendResult::CountOverflow;
      }
      span_ = tail.span_;
      return CompletionSpanAppendResult::Appended;
    }
    if (span_.tailHasPresent_) {
      return CompletionSpanAppendResult::TailHasPresent;
    }
    if (tail.span_.firstSeqId_ <= span_.lastSeqId_) {
      return CompletionSpanAppendResult::NotStrictlyIncreasing;
    }
    if (span_.lastSeqId_ == std::numeric_limits<std::uint64_t>::max() ||
        tail.span_.firstSeqId_ != span_.lastSeqId_ + 1u) {
      return CompletionSpanAppendResult::NonContiguous;
    }
    if (tail.span_.sourceCount_ > maxSourceCount_ - span_.sourceCount_ ||
        tail.span_.sourceCount_ >
            std::numeric_limits<std::uint64_t>::max() -
                span_.sourceCount_) {
      return CompletionSpanAppendResult::CountOverflow;
    }
    span_.lastSeqId_ = tail.span_.lastSeqId_;
    span_.sourceCount_ += tail.span_.sourceCount_;
    span_.tailHasPresent_ = tail.span_.tailHasPresent_;
    return CompletionSpanAppendResult::Appended;
  }

  constexpr bool empty() const noexcept { return !span_.valid(); }

  constexpr std::optional<UnverifiedEncodedCompletionSpan> summary()
      const noexcept {
    return span_.valid()
        ? std::optional<UnverifiedEncodedCompletionSpan>(span_)
        : std::nullopt;
  }

 private:
  UnverifiedEncodedCompletionSpan span_{0u, 0u, 0u, false};
  std::uint64_t maxSourceCount_ =
      std::numeric_limits<std::uint64_t>::max();
};

}  // namespace dxmt9::encoders

namespace dxmt9::core::metalqueue {

inline constexpr std::size_t kMaxPostEncodeCompletionReceipts = 1024u;

struct PostEncodeCompletionReceipt {
  std::uint64_t seqId = 0;
  std::uint64_t generation = 0;
  std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();

  constexpr bool valid() const noexcept {
    return seqId != 0u && generation != 0u &&
           slot != std::numeric_limits<std::uint32_t>::max();
  }

  friend constexpr bool operator==(PostEncodeCompletionReceipt,
                                   PostEncodeCompletionReceipt) noexcept =
      default;
};

static_assert(std::is_trivially_copyable_v<PostEncodeCompletionReceipt>);
static_assert(std::is_standard_layout_v<PostEncodeCompletionReceipt>);

enum class PostEncodeReceiptState : std::uint8_t {
  Free,
  Active,
  Submitted,
  Completed,
};

enum class PostEncodeReceiptResult : std::uint8_t {
  Succeeded,
  Invalid,
  Duplicate,
  Capacity,
  Stale,
  WrongState,
};

enum class PostEncodeRetirementIneligibility : std::uint8_t {
  None,
  PendingClear,
  Present,
  Readback,
  UpdateOrSurfaceOperation,
  OrderedControl,
  RemainingPayloadBorrow,
  NotOldestResident,
  ReceiptCapacity,
  Invalid,
};

// Fixed queue-owned receipt ledger. Dense seqIds choose a deterministic ring
// slot; the per-slot generation rejects stale completion, duplicate submit,
// and ABA after a retired Tape source has already reused its old storage.
class PostEncodeCompletionLedger {
 public:
  struct Activation {
    PostEncodeReceiptResult result = PostEncodeReceiptResult::Invalid;
    PostEncodeCompletionReceipt receipt{};
  };

  Activation activate(std::uint64_t seqId, bool hasPresent) noexcept {
    if (seqId == 0u) {
      return {};
    }
    const std::size_t slot = seqId % entries_.size();
    auto& entry = entries_[slot];
    if (entry.state != PostEncodeReceiptState::Free) {
      return Activation{
          .result = entry.seqId == seqId
              ? PostEncodeReceiptResult::Duplicate
              : PostEncodeReceiptResult::Capacity,
      };
    }
    if (entry.generation == 0u) {
      entry.generation = 1u;
    }
    entry.seqId = seqId;
    entry.hasPresent = hasPresent;
    entry.state = PostEncodeReceiptState::Active;
    ++depth_;
    peak_ = std::max(peak_, depth_);
    return Activation{
        .result = PostEncodeReceiptResult::Succeeded,
        .receipt = PostEncodeCompletionReceipt{
            .seqId = seqId,
            .generation = entry.generation,
            .slot = static_cast<std::uint32_t>(slot),
        },
    };
  }

  PostEncodeReceiptResult cancelBeforeActivationEffects(
      PostEncodeCompletionReceipt receipt) noexcept {
    auto* entry = resolve(receipt);
    if (!entry) {
      return PostEncodeReceiptResult::Stale;
    }
    if (entry->state != PostEncodeReceiptState::Active) {
      return PostEncodeReceiptResult::WrongState;
    }
    release(*entry);
    return PostEncodeReceiptResult::Succeeded;
  }

  PostEncodeReceiptResult markSubmitted(
      PostEncodeCompletionReceipt receipt,
      bool hasPresent) noexcept {
    auto* entry = resolve(receipt);
    if (!entry) {
      return PostEncodeReceiptResult::Stale;
    }
    if (entry->state != PostEncodeReceiptState::Active ||
        entry->hasPresent != hasPresent) {
      return PostEncodeReceiptResult::WrongState;
    }
    entry->state = PostEncodeReceiptState::Submitted;
    return PostEncodeReceiptResult::Succeeded;
  }

  PostEncodeReceiptResult markCompleted(
      PostEncodeCompletionReceipt receipt,
      bool hasPresent) noexcept {
    auto* entry = resolve(receipt);
    if (!entry) {
      return PostEncodeReceiptResult::Stale;
    }
    if (entry->state != PostEncodeReceiptState::Submitted ||
        entry->hasPresent != hasPresent) {
      return PostEncodeReceiptResult::WrongState;
    }
    entry->state = PostEncodeReceiptState::Completed;
    return PostEncodeReceiptResult::Succeeded;
  }

  PostEncodeReceiptResult finishAndRelease(
      std::uint64_t seqId) noexcept {
    if (seqId == 0u) {
      return PostEncodeReceiptResult::Invalid;
    }
    auto& entry = entries_[seqId % entries_.size()];
    if (entry.seqId != seqId) {
      return PostEncodeReceiptResult::Stale;
    }
    if (entry.state != PostEncodeReceiptState::Completed) {
      return PostEncodeReceiptResult::WrongState;
    }
    release(entry);
    return PostEncodeReceiptResult::Succeeded;
  }

  bool completed(std::uint64_t seqId) const noexcept {
    if (seqId == 0u) {
      return false;
    }
    const auto& entry = entries_[seqId % entries_.size()];
    return entry.seqId == seqId &&
           entry.state == PostEncodeReceiptState::Completed;
  }

  bool matches(PostEncodeCompletionReceipt receipt,
               PostEncodeReceiptState state,
               bool hasPresent) const noexcept {
    const auto* entry = resolve(receipt);
    return entry && entry->state == state &&
           entry->hasPresent == hasPresent;
  }

  std::optional<PostEncodeCompletionReceipt> receiptFor(
      std::uint64_t seqId,
      PostEncodeReceiptState state) const noexcept {
    if (seqId == 0u) {
      return std::nullopt;
    }
    const std::size_t slot = seqId % entries_.size();
    const auto& entry = entries_[slot];
    if (entry.seqId != seqId || entry.state != state) {
      return std::nullopt;
    }
    return PostEncodeCompletionReceipt{
        .seqId = seqId,
        .generation = entry.generation,
        .slot = static_cast<std::uint32_t>(slot),
    };
  }

  std::size_t depth() const noexcept { return depth_; }
  std::size_t peak() const noexcept { return peak_; }

 private:
  struct Entry {
    std::uint64_t seqId = 0;
    std::uint64_t generation = 0;
    PostEncodeReceiptState state = PostEncodeReceiptState::Free;
    bool hasPresent = false;
  };

  static constexpr std::uint64_t nextGeneration(
      std::uint64_t generation) noexcept {
    return generation == std::numeric_limits<std::uint64_t>::max()
        ? 1u
        : generation + 1u;
  }

  Entry* resolve(PostEncodeCompletionReceipt receipt) noexcept {
    if (!receipt.valid() || receipt.slot >= entries_.size()) {
      return nullptr;
    }
    auto& entry = entries_[receipt.slot];
    return entry.seqId == receipt.seqId &&
            entry.generation == receipt.generation
        ? &entry
        : nullptr;
  }

  const Entry* resolve(PostEncodeCompletionReceipt receipt) const noexcept {
    if (!receipt.valid() || receipt.slot >= entries_.size()) {
      return nullptr;
    }
    const auto& entry = entries_[receipt.slot];
    return entry.seqId == receipt.seqId &&
            entry.generation == receipt.generation
        ? &entry
        : nullptr;
  }

  void release(Entry& entry) noexcept {
    entry.seqId = 0u;
    entry.hasPresent = false;
    entry.state = PostEncodeReceiptState::Free;
    entry.generation = nextGeneration(entry.generation);
    --depth_;
  }

  std::array<Entry, kMaxPostEncodeCompletionReceipts> entries_{};
  std::size_t depth_ = 0;
  std::size_t peak_ = 0;
};

}  // namespace dxmt9::core::metalqueue
