#pragma once

#include "dxmt9/query_end_marker.hpp"
#include <cstdint>

namespace dxmt9::render {

// Candidate value policy only; not yet wired into queue/encoder admission.
// IDs index a frozen, source-owned scope table, never a mutable Query object.
struct QueryScopeIdentity {
  std::uint64_t slot = 0;
  std::uint64_t generation = 0;
  constexpr bool valid() const noexcept { return bool(slot) == bool(generation); }
  friend constexpr bool operator==(QueryScopeIdentity, QueryScopeIdentity) = default;
};

struct QueryRangeCursor {
  query::WriterIdentity owner{};
  std::uint32_t commands = 0;
  std::uint32_t ranges = 0;
  QueryScopeIdentity last{};
  friend constexpr bool operator==(QueryRangeCursor, QueryRangeCursor) = default;
};

struct QueryRangeLimits {
  std::uint32_t commands;
  std::uint32_t ranges;
};

enum class QueryRangeAppend : std::uint8_t {
  Invalid, SealRequired, Empty, Extend, NewRange
};

struct QueryRangeDecision {
  QueryRangeAppend action;
  QueryRangeCursor next;
};

// Refusal leaves the cursor unchanged. Storage/table/pin admission must also
// succeed before committing next; this arithmetic witness is not ownership.
constexpr QueryRangeDecision planQueryRangeAppend(
    QueryRangeCursor current, query::WriterIdentity owner,
    QueryScopeIdentity scope, std::uint32_t count,
    QueryRangeLimits limits) noexcept {
  if (!owner.valid() || current.owner != owner || !scope.valid() ||
      !current.last.valid() || current.commands > limits.commands ||
      current.ranges > limits.ranges || current.ranges > current.commands ||
      ((current.commands == 0) != (current.ranges == 0)) ||
      (current.ranges == 0 && current.last != QueryScopeIdentity{}))
    return {QueryRangeAppend::Invalid, current};
  if (!count) return {QueryRangeAppend::Empty, current};
  const bool newRange = !current.ranges || current.last != scope;
  if (count > limits.commands - current.commands ||
      (newRange && current.ranges == limits.ranges))
    return {QueryRangeAppend::SealRequired, current};
  auto next = current;
  next.commands += count;
  next.ranges += newRange;
  next.last = scope;
  return {newRange ? QueryRangeAppend::NewRange : QueryRangeAppend::Extend, next};
}

enum class QueryRangeBoundary : std::uint8_t {
  Membership, End, Flush, Present, OrderedControl, Capacity
};

// END must retain an unbound marker when this predicate defers publication.
// FLUSH seals the owner and posts a sticky submission request even with no
// successor input. Actual GPU completion remains a separate condition.
constexpr bool queryRangeMustSeal(QueryRangeBoundary boundary,
                                  bool hasCommands,
                                  bool hasUnboundEnd) noexcept {
  switch (boundary) {
    case QueryRangeBoundary::Membership:
    case QueryRangeBoundary::End: return false;
    case QueryRangeBoundary::Flush: return hasCommands || hasUnboundEnd;
    case QueryRangeBoundary::Present:
    case QueryRangeBoundary::OrderedControl:
    case QueryRangeBoundary::Capacity: return hasCommands || hasUnboundEnd;
  }
  return true;
}

} // namespace dxmt9::render
