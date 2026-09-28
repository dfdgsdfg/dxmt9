#include "../../../src/dxmt9/render/query_scope_range_owner.hpp"
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
void check(bool value) {
  if (!value) throw std::runtime_error("query range owner contract");
}
}

int main() {
  using namespace dxmt9;
  using namespace render;
  using Owner = QueryScopeRangeOwner<2, 4, 2>;
  using Status = QueryRangeStatus;
  static_assert(!std::is_copy_constructible_v<Owner>);
  static_assert(std::is_nothrow_move_constructible_v<Owner>);
  const query::WriterIdentity id{7, 2, 3, 4};
  auto a = std::make_shared<query::OcclusionResult>(1, 1, 1);
  auto b = std::make_shared<query::OcclusionResult>(2, 1, 1);
  query::OcclusionScope scope;
  scope.count = 1;
  scope.results[0] = a;
  try {
    QueryRangeEndReceipt survived;
    {
      Owner owner(id, 8);
      check(owner.append(id, scope, 2) == Status::Accepted);
      check(a.use_count() == 3);
      // Input alias mutation cannot mutate the owned frozen membership.
      scope.results[0] = b;
      check(owner.append(id, scope, 1) == Status::Accepted);
      check(owner.end(id, 2, survived) == Status::Accepted);
      check(survived.commandEnd() == 3);
      check(owner.append(id, {}, 1) == Status::Accepted);
      QueryRangeEndReceipt second;
      check(owner.end(id, 5, second) == Status::Accepted);
      check(second.commandEnd() == 4);
      check(owner.end(id, 6, second) == Status::Capacity);
      check(second.commandEnd() == 4);
      check(owner.ranges().empty()); // No mutable draft escape to encode.
      check(!owner.bindPublished(id, 9));
      Owner moved(std::move(owner));
      check(owner.append(id, scope, 1) == Status::Invalid);
      check(moved.seal(id));
      check(moved.ranges().size() == 3 && moved.ends().size() == 2);
      check(moved.scope(id, moved.ranges()[0].scope)->results[0] == a);
      check(moved.scope(id, moved.ranges()[1].scope)->results[0] == b);
      auto stale = id;
      ++stale.storageGeneration;
      check(!moved.scope(stale, moved.ranges()[0].scope));
      check(!moved.bindPublished(stale, 9));
      check(moved.append(id, scope, 1) == Status::Invalid);
      check(moved.bindPublished(id, 9));
      check(!moved.bindPublished(id, 10));
      moved.cancelUnpublished(); // Published owners cannot be rolled back.
      query::Progress progress{7, 9, 0, false, 5, 2};
      check(survived.poll(progress) == query::Poll::Pending);
      progress.completed = 9;
      check(survived.poll(progress) == query::Poll::Complete);
      check(second.poll(progress) == query::Poll::Pending);
      progress.initializerCompleted = 5;
      check(second.poll(progress) == query::Poll::Complete);
      progress.failed = true;
      check(second.poll(progress) == query::Poll::Failed);
    }
    check(a.use_count() == 1 && b.use_count() == 2);
    check(survived.sequence() == 9);

    // Explicit cancellation and RAII abandonment cannot leave a receipt
    // looking like a successful empty prefix or perpetually unbound Pending.
    for (bool explicitCancel : {false, true}) {
      QueryRangeEndReceipt cancelled;
      {
        Owner owner(id, 8);
        check(owner.append(id, scope, 1) == Status::Accepted);
        check(owner.end(id, 0, cancelled) == Status::Accepted);
        if (explicitCancel) owner.cancelUnpublished();
      }
      check(cancelled.poll({7, 0, 0}) == query::Poll::Failed);
      check(query::classifyEnd(cancelled.fence(), {7, 0, 0}) == query::Poll::Failed);
      check(cancelled.poll({8, 0, 0}) == query::Poll::Invalid);
    }

    // The only dynamic allocation is first END state. Both failure modes
    // preserve draft state and an already supplied output receipt.
    Owner faults(id, 8);
    check(faults.append(id, scope, 1) == Status::Accepted);
    auto saved = survived;
    check(faults.end(id, 0, saved, [](auto) -> std::shared_ptr<query_range_detail::EndState> {
      throw std::bad_alloc();
    }) == Status::AllocationFailed);
    check(saved.sequence() == 9 && faults.endCount() == 0);
    check(faults.end(id, 0, saved, [](auto) {
      return std::shared_ptr<query_range_detail::EndState>{};
    }) == Status::AllocationFailed);
    check(saved.sequence() == 9 && faults.endCount() == 0);
    check(faults.end(id, 0, saved) == Status::Accepted);
    check(faults.end(id, 0, saved, [](auto) -> std::shared_ptr<query_range_detail::EndState> {
      throw std::runtime_error("must reuse END allocation");
    }) == Status::Accepted);

    // Each independent capacity fails before retaining an input scope.
    {
      QueryScopeRangeOwner<0, 4, 2> noScopes(id, 8);
      QueryScopeRangeOwner<2, 0, 2> noRanges(id, 8);
      QueryScopeRangeOwner<2, 4, 0> noEnds(id, 8);
      Owner noCommands(id, 0);
      const auto refs = b.use_count();
      check(noScopes.append(id, scope, 1) == Status::Capacity);
      check(noRanges.append(id, scope, 1) == Status::Capacity);
      check(noCommands.append(id, scope, 1) == Status::Capacity);
      check(b.use_count() == refs);
      check(noScopes.scopeCount() == 0 && noRanges.scopeCount() == 0);
      check(noEnds.append(id, {}, 1) == Status::Accepted);
      check(noEnds.end(id, 0, saved) == Status::Capacity);
    }
    {
      Owner owner(id, 8);
      query::OcclusionScope pair;
      pair.count = 2;
      pair.results[0] = a;
      pair.results[1] = b;
      check(owner.append(id, pair, 1) == Status::Accepted);
      std::swap(pair.results[0], pair.results[1]);
      check(owner.append(id, pair, 1) == Status::Accepted);
      check(owner.scopeCount() == 1 && owner.rangeCount() == 1);
      // Duplicate identity with different storage must not split result sums.
      pair.count = 1;
      pair.results[0] = std::make_shared<query::OcclusionResult>(1, 1, 1);
      check(owner.append(id, pair, 1) == Status::Invalid);
      check(owner.commands() == 2 && owner.scopeCount() == 1);
      pair.results[0] = std::make_shared<query::OcclusionResult>(1, 2, 1);
      check(owner.append(id, pair, 1) == Status::Accepted);
      check(owner.scopeCount() == 2 && owner.rangeCount() == 2);
      pair.count = 65;
      check(owner.append(id, pair, 1) == Status::Invalid);
    }
    // Independent bounded phase model against the real owner, including
    // invalid/repeated operations and cancellation at every transition.
    unsigned schedules = 0;
    for (unsigned word = 0; word < 15625; ++word) {
      QueryScopeRangeOwner<1, 1, 2> owner(id, 2);
      QueryRangeEndReceipt receipt;
      unsigned phase = 0, commands = 0, ends = 0, code = word;
      bool hasReceipt = false;
      for (unsigned step = 0; step < 6; ++step) {
        switch (code % 5) {
          case 0: {
            const auto expected = phase != 0 ? Status::Invalid :
                commands == 2 ? Status::Capacity : Status::Accepted;
            check(owner.append(id, scope, 1) == expected);
            if (expected == Status::Accepted) ++commands;
            break;
          }
          case 1: {
            const auto expected = phase != 0 || !commands ? Status::Invalid :
                ends == 2 ? Status::Capacity : Status::Accepted;
            check(owner.end(id, 0, receipt) == expected);
            if (expected == Status::Accepted) { ++ends; hasReceipt = true; }
            break;
          }
          case 2: {
            const bool accepted = phase == 0 && commands;
            check(owner.seal(id) == accepted);
            if (accepted) phase = 1;
            break;
          }
          case 3: {
            const bool accepted = phase == 1;
            check(owner.bindPublished(id, 9) == accepted);
            if (accepted) phase = 2;
            break;
          }
          case 4:
            owner.cancelUnpublished();
            if (phase < 2) phase = 3;
            break;
        }
        code /= 5;
        check(owner.commands() == commands && owner.endCount() == ends);
        const auto expectedPoll = !hasReceipt ? query::Poll::Invalid :
            phase == 3 ? query::Poll::Failed :
            phase == 2 ? query::Poll::Complete : query::Poll::Pending;
        check(receipt.poll({7, 9, 9}) == expectedPoll);
      }
      ++schedules;
    }
    std::cout << schedules << " bounded lifecycle schedules plus retention, refusal and allocation faults passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
