#pragma once

#include "query_scope_range_policy.hpp"
#include "dxmt9/query_occlusion.hpp"
#include <array>
#include <memory>
#include <span>
#include <utility>

namespace dxmt9::render {

// Candidate-only storage owner. No production queue or Metal consumer yet.
enum class QueryRangeStatus : std::uint8_t {
  Accepted, Capacity, Invalid, AllocationFailed
};

namespace query_range_detail {
struct EndState {
  query::EndMarker marker;
  std::atomic<bool> cancelled{false};
  explicit EndState(query::WriterIdentity owner) noexcept : marker(owner) {}
};
struct MakeEndState {
  auto operator()(query::WriterIdentity owner) const {
    return std::make_shared<EndState>(owner);
  }
};
} // namespace query_range_detail

class QueryRangeEndReceipt {
 public:
  QueryRangeEndReceipt() = default;
  // Fence/cancellation only. GetData must additionally check the retained
  // OcclusionResult's failure/pending state before exposing sample bytes.
  query::Poll poll(query::Progress progress) const noexcept {
    if (!state_ || state_->marker.owner().queue != progress.queue)
      return query::Poll::Invalid;
    if (state_->cancelled.load(std::memory_order_acquire))
      return query::Poll::Failed;
    return query::classifyEnd(
        {progress.queue, state_->marker.sequence(), initializer_},
        progress, true, true);
  }
  std::uint32_t commandEnd() const noexcept { return commandEnd_; }
  std::uint64_t sequence() const noexcept {
    return state_ ? state_->marker.sequence() : 0;
  }
 private:
  template<std::size_t, std::size_t, std::size_t> friend class QueryScopeRangeOwner;
  std::shared_ptr<const query_range_detail::EndState> state_;
  std::uint32_t commandEnd_ = 0;
  std::uint64_t initializer_ = 0;
};

template<std::size_t ScopeCapacity, std::size_t RangeCapacity,
         std::size_t EndCapacity>
class QueryScopeRangeOwner {
 public:
  static_assert(ScopeCapacity <= UINT32_MAX && RangeCapacity <= UINT32_MAX);
  struct Range {
    std::uint32_t begin, end;
    QueryScopeIdentity scope;
  };
  struct End {
    std::uint32_t commandEnd;
    std::uint64_t initializer;
  };
  enum class Phase : std::uint8_t { Writing, Sealed, Published, Cancelled };

  explicit QueryScopeRangeOwner(query::WriterIdentity owner,
                               std::uint32_t commandCapacity) noexcept
      : cursor_{owner}, commandCapacity_(commandCapacity) {}
  QueryScopeRangeOwner(const QueryScopeRangeOwner&) = delete;
  QueryScopeRangeOwner& operator=(const QueryScopeRangeOwner&) = delete;
  QueryScopeRangeOwner& operator=(QueryScopeRangeOwner&&) = delete;
  QueryScopeRangeOwner(QueryScopeRangeOwner&& other) noexcept
      : cursor_(other.cursor_), commandCapacity_(other.commandCapacity_),
        scopes_(std::move(other.scopes_)), scopeCount_(other.scopeCount_),
        ranges_(other.ranges_), ends_(other.ends_), endCount_(other.endCount_),
        endState_(std::move(other.endState_)), phase_(other.phase_) {
    other.phase_ = Phase::Cancelled;
    other.cursor_ = {};
    other.scopeCount_ = other.endCount_ = 0;
  }
  ~QueryScopeRangeOwner() { cancelUnpublished(); }

  QueryRangeStatus append(query::WriterIdentity expected,
                          const query::OcclusionScope& input,
                          std::uint32_t commands) noexcept {
    if (phase_ != Phase::Writing || expected != cursor_.owner ||
        !expected.valid() || input.count > input.capacity)
      return QueryRangeStatus::Invalid;
    for (std::size_t i = 0; i < input.count; ++i) {
      const auto& result = input.results[i];
      if (!result || !result->object || !result->issue || !result->epoch)
        return QueryRangeStatus::Invalid;
      for (std::size_t j = 0; j < i; ++j)
        if (sameIdentity(result, input.results[j])) return QueryRangeStatus::Invalid;
      for (std::size_t s = 0; s < scopeCount_; ++s)
        for (std::size_t j = 0; j < scopes_[s].count; ++j)
          if (sameIdentity(result, scopes_[s].results[j]) &&
              result != scopes_[s].results[j]) return QueryRangeStatus::Invalid;
    }
    if (!commands) return QueryRangeStatus::Accepted;
    std::size_t slot = scopeCount_;
    if (input.count) {
      for (std::size_t s = 0; s < scopeCount_; ++s) {
        bool same = input.count == scopes_[s].count;
        for (std::size_t i = 0; same && i < input.count; ++i)
          same = scopes_[s].contains(input.results[i]);
        if (same) { slot = s; break; }
      }
    }
    const bool newScope = input.count && slot == scopeCount_;
    if (newScope && scopeCount_ == ScopeCapacity) return QueryRangeStatus::Capacity;
    const QueryScopeIdentity identity = input.count
        ? QueryScopeIdentity{slot + 1, expected.storageGeneration} : QueryScopeIdentity{};
    const auto plan = planQueryRangeAppend(cursor_, expected, identity, commands,
        {commandCapacity_, static_cast<std::uint32_t>(RangeCapacity)});
    if (plan.action == QueryRangeAppend::SealRequired) return QueryRangeStatus::Capacity;
    if (plan.action == QueryRangeAppend::Invalid) return QueryRangeStatus::Invalid;
    // Everything below is noexcept. Failed admission above has retained no refs.
    if (newScope) {
      scopes_[slot].count = input.count;
      for (std::size_t i = 0; i < input.count; ++i)
        scopes_[slot].results[i] = input.results[i];
      ++scopeCount_;
    }
    if (plan.action == QueryRangeAppend::NewRange)
      ranges_[cursor_.ranges] = {cursor_.commands, plan.next.commands, identity};
    else
      ranges_[cursor_.ranges - 1].end = plan.next.commands;
    cursor_ = plan.next;
    return QueryRangeStatus::Accepted;
  }

  // Factory is a compile-time allocation seam, not a runtime diagnostic knob.
  template<class Factory = query_range_detail::MakeEndState>
  QueryRangeStatus end(query::WriterIdentity expected, std::uint64_t initializer,
                       QueryRangeEndReceipt& out, Factory factory = {}) noexcept {
    if (phase_ != Phase::Writing || expected != cursor_.owner ||
        !expected.valid() || !cursor_.commands) return QueryRangeStatus::Invalid;
    if (endCount_ == EndCapacity) return QueryRangeStatus::Capacity;
    auto staged = endState_;
    try {
      if (!staged) staged = factory(expected);
    } catch (...) { return QueryRangeStatus::AllocationFailed; }
    if (!staged) return QueryRangeStatus::AllocationFailed;
    if (staged->marker.owner() != expected || staged->marker.sequence() ||
        staged->cancelled.load(std::memory_order_acquire)) return QueryRangeStatus::Invalid;
    endState_ = std::move(staged);
    ends_[endCount_++] = {cursor_.commands, initializer};
    out.state_ = endState_;
    out.commandEnd_ = cursor_.commands;
    out.initializer_ = initializer;
    return QueryRangeStatus::Accepted;
  }

  bool seal(query::WriterIdentity expected) noexcept {
    if (phase_ != Phase::Writing || !expected.valid() || expected != cursor_.owner ||
        !cursor_.commands) return false;
    phase_ = Phase::Sealed;
    return true;
  }
  // Integration must invoke only after actual queue publication under its lock.
  // This is not a publication capability and never reserves/predicts a seqId.
  bool bindPublished(query::WriterIdentity expected, std::uint64_t sequence) noexcept {
    if (phase_ != Phase::Sealed || expected != cursor_.owner || !sequence) return false;
    if (endState_ && !endState_->marker.bind(expected, sequence)) return false;
    phase_ = Phase::Published;
    return true;
  }
  void cancelUnpublished() noexcept {
    if (phase_ == Phase::Published || phase_ == Phase::Cancelled) return;
    if (endState_) endState_->cancelled.store(true, std::memory_order_release);
    phase_ = Phase::Cancelled;
  }
  Phase phase() const noexcept { return phase_; }
  std::uint32_t commands() const noexcept { return cursor_.commands; }
  std::size_t scopeCount() const noexcept { return scopeCount_; }
  std::size_t rangeCount() const noexcept { return cursor_.ranges; }
  std::size_t endCount() const noexcept { return endCount_; }
  std::span<const Range> ranges() const noexcept {
    return readable() ? std::span<const Range>{ranges_.data(), cursor_.ranges}
                      : std::span<const Range>{};
  }
  std::span<const End> ends() const noexcept {
    return readable() ? std::span<const End>{ends_.data(), endCount_}
                      : std::span<const End>{};
  }
  const query::OcclusionScope* scope(query::WriterIdentity expected,
                                     QueryScopeIdentity id) const noexcept {
    if (!readable() || expected != cursor_.owner || !expected.valid() || !id.slot ||
        id.slot > scopeCount_ || id.generation != expected.storageGeneration)
      return nullptr;
    return &scopes_[id.slot - 1];
  }
 private:
  bool readable() const noexcept {
    return phase_ == Phase::Sealed || phase_ == Phase::Published;
  }
  static bool sameIdentity(const std::shared_ptr<query::OcclusionResult>& a,
                           const std::shared_ptr<query::OcclusionResult>& b) noexcept {
    return a->object == b->object && a->issue == b->issue && a->epoch == b->epoch;
  }
  QueryRangeCursor cursor_;
  std::uint32_t commandCapacity_;
  std::array<query::OcclusionScope, ScopeCapacity> scopes_{};
  std::size_t scopeCount_ = 0;
  std::array<Range, RangeCapacity> ranges_{};
  std::array<End, EndCapacity> ends_{};
  std::size_t endCount_ = 0;
  std::shared_ptr<query_range_detail::EndState> endState_;
  Phase phase_ = Phase::Writing;
};

} // namespace dxmt9::render
