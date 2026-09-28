// R-CORE-8.2(c): candidate range metadata, not a production GPU oracle.
#include "../../../src/dxmt9/render/query_scope_range_policy.hpp"
#include "dxmt9/query_occlusion.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value) {
  if (!value) throw std::runtime_error("query range refinement");
}
}

int main() {
  using namespace dxmt9;
  using namespace render;
  const query::WriterIdentity owner{7, 0, 11, 13};
  try {
    unsigned traces = 0;
    // Four ordered commands; {}, A, B, AB scopes. Every command may be on
    // a distinct failing origin. Scope ranges must not erase origin identity.
    for (unsigned scopes = 0; scopes < 256; ++scopes) {
      for (unsigned failures = 0; failures < 16; ++failures) {
        struct Range { unsigned begin, end, mask; };
        std::array<Range, 4> ranges{};
        QueryRangeCursor cursor{owner};
        std::array<unsigned, 2> expected{};
        std::array<bool, 2> failed{};
        for (unsigned i = 0; i < 4; ++i) {
          const unsigned mask = (scopes >> (i * 2)) & 3;
          const QueryScopeIdentity key{mask, mask ? 1u : 0u};
          const auto plan = planQueryRangeAppend(cursor, owner, key, 1, {4, 4});
          if (plan.action == QueryRangeAppend::NewRange)
            ranges[cursor.ranges] = {i, i + 1, mask};
          else {
            check(plan.action == QueryRangeAppend::Extend);
            ranges[cursor.ranges - 1].end = i + 1;
          }
          cursor = plan.next;
          for (unsigned q = 0; q < 2; ++q) if (mask & (1u << q)) {
            failed[q] = failed[q] || bool(failures & (1u << i));
            if (!(failures & (1u << i))) expected[q] += (i + 1) * 17;
          }
        }
        std::array<query::OcclusionResult, 2> results{
          query::OcclusionResult(1, 1, 1), query::OcclusionResult(2, 1, 1)};
        unsigned visited = 0;
        for (unsigned r = 0; r < cursor.ranges; ++r) {
          check(ranges[r].begin == visited);
          for (unsigned i = ranges[r].begin; i < ranges[r].end; ++i) {
            ++visited;
            for (unsigned q = 0; q < 2; ++q) if (ranges[r].mask & (1u << q)) {
              results[q].pending.fetch_add(1);
              if (failures & (1u << i)) results[q].failed.store(true);
              else results[q].add((i + 1) * 17);
              check(results[q].pending.fetch_sub(1) == 1);
            }
          }
        }
        check(visited == 4);
        for (unsigned q = 0; q < 2; ++q) {
          check(results[q].samples.load() == expected[q]);
          check(results[q].failed.load() == failed[q]);
          check(results[q].readableAfterFence() == !failed[q]);
        }
        // Every absent command/range dimension refuses without mutation.
        for (unsigned commands = 0; commands <= 4; ++commands)
          for (unsigned capacity = 0; capacity <= 4; ++capacity) {
            QueryRangeCursor limited{owner};
            for (unsigned i = 0; i < 4; ++i) {
              const unsigned mask = (scopes >> (i * 2)) & 3;
              const QueryScopeIdentity key{mask, mask ? 1u : 0u};
              const auto d = planQueryRangeAppend(limited, owner, key, 1,
                                                  {commands, capacity});
              const bool needsRange = i == 0 || ((scopes >> ((i - 1) * 2)) & 3) != mask;
              const bool fits = limited.commands < commands &&
                  (!needsRange || limited.ranges < capacity);
              if (!fits) {
                check(d.action == QueryRangeAppend::SealRequired && d.next == limited);
                break;
              }
              limited = d.next;
            }
          }
        ++traces;
      }
    }
    QueryRangeCursor cursor{owner};
    auto first = planQueryRangeAppend(cursor, owner, {1, 1}, 1, {4, 4});
    check(first.action == QueryRangeAppend::NewRange);
    auto reissue = planQueryRangeAppend(first.next, owner, {1, 2}, 1, {4, 4});
    check(reissue.action == QueryRangeAppend::NewRange);
    check(planQueryRangeAppend(cursor, owner, {0, 1}, 1, {4, 4}).action == QueryRangeAppend::Invalid);
    check(planQueryRangeAppend(cursor, owner, {}, 0, {0, 0}).action == QueryRangeAppend::Empty);
    auto stale = owner;
    ++stale.storageGeneration;
    check(planQueryRangeAppend(cursor, stale, {}, 1, {4, 4}).action == QueryRangeAppend::Invalid);
    auto huge = cursor;
    huge.commands = UINT32_MAX;
    huge.ranges = 1;
    check(planQueryRangeAppend(huge, owner, {}, 1, {UINT32_MAX, 1}).action == QueryRangeAppend::SealRequired);

    for (auto boundary : {QueryRangeBoundary::Present,
                          QueryRangeBoundary::OrderedControl,
                          QueryRangeBoundary::Capacity,
                          QueryRangeBoundary::Flush}) {
      check(!queryRangeMustSeal(boundary, false, false));
      check(queryRangeMustSeal(boundary, true, false));
      check(queryRangeMustSeal(boundary, false, true));
    }
    // Structural upper bound only, not a queue CPU benchmark. Eight queries
    // with an unqueried draw before each query require 16 reference scope
    // publications. Candidate publication is still forced by each FLUSH.
    for (unsigned burst : {1u, 2u, 8u}) {
      unsigned publications = 0;
      QueryRangeCursor draft{owner};
      for (unsigned q = 0; q < 8; ++q) {
        for (auto scope : {QueryScopeIdentity{}, QueryScopeIdentity{q + 1, 1}}) {
          const auto d = planQueryRangeAppend(draft, draft.owner, scope, 1, {16, 16});
          check(d.action == QueryRangeAppend::NewRange);
          draft = d.next;
        }
        check(!queryRangeMustSeal(QueryRangeBoundary::End, true, true));
        if ((q + 1) % burst == 0) {
          check(queryRangeMustSeal(QueryRangeBoundary::Flush, true, true));
          ++publications;
          auto nextOwner = draft.owner;
          ++nextOwner.generation;
          draft = QueryRangeCursor{nextOwner};
        }
      }
      check(publications == 8 / burst);
    }

    // No successor work: END remains Pending until FLUSH seals, binds and
    // requests the prefix; publication alone cannot authorize completion.
    auto marker = std::make_shared<query::EndMarker>(owner);
    query::EndReceipt receipt{owner.queue, 0, 2};
    receipt.marker = marker;
    query::Progress progress{owner.queue, 0, 0, false, 2, 2};
    check(query::classifyEnd(receipt, progress) == query::Poll::Pending);
    check(!queryRangeMustSeal(QueryRangeBoundary::Membership, true, true));
    check(!queryRangeMustSeal(QueryRangeBoundary::End, true, true));
    check(queryRangeMustSeal(QueryRangeBoundary::Flush, true, true));
    check(!marker->bind(stale, 9));
    check(!marker->bind(owner, 0));
    check(marker->bind(owner, 9));
    check(!marker->bind(owner, 10));
    progress.published = 9;
    check(query::classifyEnd(receipt, progress) == query::Poll::Pending);
    check(query::submissionWorkTarget(marker->sequence(), 0, 0) == 9);
    progress.completed = 9;
    progress.initializerCompleted = 1;
    check(query::classifyEnd(receipt, progress) == query::Poll::Pending);
    progress.initializerCompleted = 2;
    check(query::classifyEnd(receipt, progress) == query::Poll::Complete);
    progress.failed = true;
    check(query::classifyEnd(receipt, progress) == query::Poll::Failed);
    ++progress.queue;
    check(query::classifyEnd(receipt, progress) == query::Poll::Invalid);
    std::cout << traces << " scope/failure traces, capacity refusals and END binding passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
