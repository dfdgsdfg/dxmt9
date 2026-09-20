#include "../../../src/dxmt9/dxmt9_prepared_encode_plan.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

using namespace dxmt9::encoders;
namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
PreparedSourceKey key(std::uint32_t commands) {
  return {.source = {.id = {2, 7}, .storage = {3, 2, 11}},
          .sequence = 19, .firstCommand = 4, .commandCount = commands};
}
dxmt9::core::ExactResourceSet resources(std::initializer_list<std::uint64_t> ids) {
  dxmt9::core::ExactResourceSet set{};
  set.flags |= dxmt9::core::ExactResourceSetCanonicalized;
  for (auto id : ids) check(set.add(id), "resource fixture capacity");
  return set;
}
auto attachments(std::uint32_t samples = 1) {
  dxmt9::core::RenderAttachmentKey value{};
  value.sampleCount = samples;
  return value;
}
using Kind = dxmt9::core::MetalCommandKind;
constexpr auto Stable = PreparedAccessResolution::CanonicalStable;

void exhaustiveConstruction() {
  // Independently reconstruct expected command access, set membership and
  // maximal ranges from 4^5 input traces. Index assignment is not the oracle.
  for (std::uint32_t trace = 0; trace < 1024; ++trace) {
    PreparedEncodePlanBuilder builder(key(5));
    auto encoded = trace;
    std::array<unsigned, 5> choices{};
    std::set<std::uint64_t> reference;
    std::vector<std::pair<unsigned, unsigned>> ranges;
    for (unsigned i = 0; i < 5; ++i) {
      const auto choice = choices[i] = encoded % 4;
      encoded /= 4;
      const auto kind = choice < 2 ? Kind::DrawRun : choice == 2 ? Kind::Clear : Kind::Present;
      const auto reads = choice < 2 ? resources({choice + 1u}) : resources({});
      const auto writes = choice == 2 ? resources({3}) : resources({});
      check(builder.append(4 + i, kind, reads, writes, attachments(choice + 1), Stable),
            "bounded append");
      if (choice < 3) reference.insert(choice + 1);
      if (i && choice < 2 && choices[i - 1] == choice) ++ranges.back().second;
      else ranges.emplace_back(i, 1);
    }
    auto owner = builder.seal();
    const auto* plan = owner.view(key(5));
    check(plan && plan->commandCount == 5 && plan->rangeCount == ranges.size(),
          "sealed command/range coverage");
    check(std::set<std::uint64_t>(plan->identities.begin(),
              plan->identities.begin() + plan->identityCount) == reference &&
              plan->identityCount == reference.size(), "exact dictionary");
    for (unsigned i = 0; i < 5; ++i) {
      const auto& row = plan->commands[i];
      check(row.readCount == (choices[i] < 2 ? 1u : 0u) &&
            row.writeCount == (choices[i] == 2 ? 1u : 0u), "access role retained");
      for (auto offset : {row.readOffset, row.writeOffset})
        check(offset <= plan->accessCount, "bounded offset");
      if (row.readCount)
        check(plan->identities[plan->accesses[row.readOffset]] == choices[i] + 1,
              "read index resolves to independent identity");
      if (row.writeCount)
        check(plan->identities[plan->accesses[row.writeOffset]] == 3,
              "write index resolves to independent identity");
    }
    for (unsigned i = 0; i < ranges.size(); ++i)
      check(plan->ranges[i].first == ranges[i].first &&
            plan->ranges[i].count == ranges[i].second &&
            plan->ranges[i].attachments == attachments(choices[ranges[i].first] + 1),
            "maximal ordered ranges retain attachments");
    auto stale = key(5);
    ++stale.source.storage.generation;
    check(!owner.view(stale), "stale storage key rejects lookup");
    auto moved = std::move(owner);
    check(!owner && moved.view(key(5)) && !builder.seal(), "single sealed ownership");
  }
}

void failuresAndBounds() {
  const auto empty = resources({});
  const auto one = resources({(1ull << 32) | 7});
  const auto two = resources({(2ull << 32) | 7});
  const auto append = [&](PreparedEncodePlanBuilder& b, unsigned i, auto reads,
                          auto writes, Kind kind = Kind::DrawRun) {
    return b.append(i, kind, reads, writes, attachments(), Stable);
  };
  PreparedEncodePlanBuilder invalid({});
  check(invalid.failure() == PreparedPlanFailure::InvalidSource, "invalid key");
  PreparedEncodePlanBuilder noMemory(key(1), {}, nullptr);
  check(noMemory.failure() == PreparedPlanFailure::Allocation && !noMemory.seal(),
        "single allocation fault exposes no plan");
  PreparedEncodePlanBuilder order(key(2));
  check(append(order, 4, one, empty) && !append(order, 4, one, empty) &&
            !order.seal(), "duplicate ordinal discards prefix");
  PreparedEncodePlanBuilder truncated(key(2));
  check(append(truncated, 4, one, empty) && !truncated.seal() &&
            truncated.failure() == PreparedPlanFailure::Incomplete, "truncated seal");
  PreparedEncodePlanBuilder unknown(key(1));
  check(!append(unknown, 4, empty, empty, static_cast<Kind>(255)) &&
            unknown.failure() == PreparedPlanFailure::UnsupportedCommand,
        "unknown command fails closed");
  auto malformed = one;
  malformed.flags &= ~dxmt9::core::ExactResourceSetCanonicalized;
  PreparedEncodePlanBuilder badAccess(key(1));
  check(!append(badAccess, 4, malformed, empty), "unqualified canonical access rejected");
  PreparedEncodePlanBuilder alias(key(1));
  check(append(alias, 4, one, one), "hazard preserved for final admission");
  auto aliasOwner = alias.seal();
  const auto* aliasPlan = aliasOwner.view(key(1));
  check(aliasPlan && aliasPlan->identityCount == 1 && aliasPlan->accessCount == 2 &&
            aliasPlan->accesses[0] == aliasPlan->accesses[1],
        "read/write alias remains explicit; builder does not certify disjointness");

  for (unsigned dimension = 0; dimension < 4; ++dimension) {
    PreparedPlanLimits limits{};
    if (dimension == 0) limits.identities = 1;
    if (dimension == 1) limits.commands = 1;
    if (dimension == 2) limits.accesses = 1;
    if (dimension == 3) limits.ranges = 1;
    PreparedEncodePlanBuilder bounded(key(2), limits);
    if (dimension != 1) {
      if (dimension == 0 || dimension == 2)
        check(!append(bounded, 4, one, two), "partial access append fails atomically");
      else check(append(bounded, 4, one, empty) &&
                 !append(bounded, 5, empty, two, Kind::Clear), "range capacity");
    }
    check(bounded.failure() == PreparedPlanFailure::Capacity && !bounded.seal() &&
              !append(bounded, 4, empty, empty), "all dimensions poison whole plan");
  }
  PreparedEncodePlanBuilder exact(key(256));
  for (unsigned i = 0; i < 256; ++i)
    check(append(exact, 4 + i, resources({(std::uint64_t(i + 1) << 32) | 7}), empty),
          "full-generation exact identity capacity");
  auto exactOwner = exact.seal();
  check(exactOwner.view(key(256))->identityCount == 256, "no low-bit dedup");

  PreparedEncodePlanBuilder unresolved(key(1));
  check(unresolved.append(4, Kind::DrawRun, malformed, two, attachments(),
      PreparedAccessResolution::RequiresEncodeResolution), "unresolved access preserved");
  auto unresolvedOwner = unresolved.seal();
  const auto* plan = unresolvedOwner.view(key(1));
  check(plan && plan->needsEncodeResolution && !plan->identityCount && !plan->accessCount,
        "unresolved alias never becomes guessed canonical proof");

  // Every fixed storage dimension is reached at its exact upper bound.
  PreparedEncodePlanBuilder allRows(key(1024));
  for (unsigned i = 0; i < 1024; ++i)
    check(append(allRows, 4 + i, resources({1,2,3,4,5,6,7,8}), empty, Kind::Clear),
          "exact row/range/access capacity");
  auto allOwner = allRows.seal();
  const auto* all = allOwner.view(key(1024));
  check(all && all->rangeCount == 1024 && all->accessCount == 8192,
        "all exact capacities seal without truncation");

  auto corrupt = std::make_unique<PreparedEncodePlanStorage>(*all);
  const auto reject = [&](auto mutate) {
    *corrupt = *all;
    mutate(*corrupt);
    check(!validatePreparedEncodePlan(*corrupt, key(1024)), "malformed closure rejected");
  };
  reject([](auto& s) { s.commands[0].readOffset = UINT32_MAX; });
  reject([](auto& s) { s.commands[0].writeOffset++; });
  reject([](auto& s) { s.commands[0].readCount = UINT32_MAX; });
  reject([](auto& s) { s.accesses[0] = UINT16_MAX; });
  reject([](auto& s) { s.accesses[1] = s.accesses[0]; });
  reject([](auto& s) { s.ranges[0].first++; });
  reject([](auto& s) { s.ranges[0].count = UINT32_MAX; });
  reject([](auto& s) { s.rangeCount--; });
  reject([](auto& s) { s.identities[1] = s.identities[0]; });
  reject([](auto& s) { s.key.source.id.generation++; });
  reject([](auto& s) { s.commands[0].resolution =
      PreparedAccessResolution::RequiresEncodeResolution; });
  reject([](auto& s) { s.needsEncodeResolution = true; });
}

void exhaustivePublication() {
  using P = PreparedPublicationPhase;
  using A = PreparedPublicationAction;
  std::vector<PreparedPublicationState> states{{.key = key(2)}};
  std::vector<std::pair<std::size_t, std::size_t>> graph;
  std::size_t edges = 0;
  for (std::size_t cursor = 0; cursor < states.size(); ++cursor) {
    const auto before = states[cursor];
    for (unsigned a = 0; a < static_cast<unsigned>(A::Count); ++a) {
      const auto action = static_cast<A>(a);
      const auto result = reducePreparedPublication(before, action, before.key);
      const auto& after = result.state;
      if (!result.accepted) check(after == before, "rejection cannot mutate ownership");
      else {
        ++edges;
        if (after.phase == P::Published)
          check(after.slotReady && (after.planReady || after.planFailed),
                "no partial publication");
        if (action == A::Publish)
          check(before.phase == P::Preparing && after.prepared == before.planReady,
                "one atomic serial/prepared disposition");
        if (before.phase == P::Encoding) {
          check(after.phase == P::Encoding || after.phase == P::Joined,
                "no fallback/retry after effects");
          check(!before.prepared || !after.planReleased, "no early plan retirement");
        }
        if (action == A::Join)
          check(before.childrenDone == (before.prepared ? 3u : 1u), "join every worker");
        if (action == A::RetireSource)
          check(before.phase == P::Completed && before.planReleased,
                "source retirement requires completion and no plan consumers");
        check(!before.encodeFailed || after.encodeFailed, "failure is never success");
        if (std::find(states.begin(), states.end(), after) == states.end())
          states.push_back(after);
        graph.emplace_back(cursor, static_cast<std::size_t>(
            std::find(states.begin(), states.end(), after) - states.begin()));
      }
      // Reject every component of a stale identity on every reachable state/action.
      for (unsigned field = 0; field < 8; ++field) {
        auto stale = before.key;
        switch (field) {
        case 0: ++stale.source.id.index; break;
        case 1: ++stale.source.id.generation; break;
        case 2: ++stale.source.storage.firstPage; break;
        case 3: ++stale.source.storage.pageCount; break;
        case 4: ++stale.source.storage.generation; break;
        case 5: ++stale.sequence; break;
        case 6: ++stale.firstCommand; break;
        case 7: ++stale.commandCount; break;
        }
        const auto rejected = reducePreparedPublication(before, action, stale);
        check(!rejected.accepted && rejected.state == before, "identity-qualified transition");
      }
    }
  }
  check(std::any_of(states.begin(), states.end(), [](const auto& s) {
    return s.phase == P::Retired && s.prepared && !s.encodeFailed;
  }), "prepared successful lifecycle reachable");
  check(std::any_of(states.begin(), states.end(), [](const auto& s) {
    return s.phase == P::Retired && !s.prepared;
  }), "serial fallback lifecycle reachable");
  check(std::any_of(states.begin(), states.end(), [](const auto& s) {
    return s.phase == P::Retired && s.encodeFailed;
  }), "post-effect failure drains without serial retry");
  std::vector<bool> canFinish(states.size());
  for (std::size_t i = 0; i < states.size(); ++i)
    canFinish[i] = states[i].phase == P::Retired || states[i].phase == P::Aborted;
  for (std::size_t round = 0; round < states.size(); ++round)
    for (const auto& [from, to] : graph) canFinish[from] = canFinish[from] || canFinish[to];
  check(std::all_of(canFinish.begin(), canFinish.end(), [](bool v) { return v; }),
        "every reachable state has a terminal path when environment makes progress");
  // Explicit smallest forbidden traces, in addition to the whole graph.
  PreparedPublicationState partial{.key = key(2)};
  partial = reducePreparedPublication(partial, A::PlanReady, partial.key).state;
  check(!reducePreparedPublication(partial, A::Publish, partial.key).accepted,
        "plan alone cannot publish an incomplete Slot");
  partial = reducePreparedPublication(partial, A::SlotReady, partial.key).state;
  partial = reducePreparedPublication(partial, A::Publish, partial.key).state;
  check(!reducePreparedPublication(partial, A::Publish, partial.key).accepted,
        "exactly one publication");
  partial = reducePreparedPublication(partial, A::BeginEncode, partial.key).state;
  check(!reducePreparedPublication(partial, A::ReleasePlan, partial.key).accepted &&
        !reducePreparedPublication(partial, A::SelectSerial, partial.key).accepted,
        "no early release or post-effect serial retry");
  std::cout << "publication bounded states=" << states.size() << " accepted_edges=" << edges << '\n';
}
} // namespace

int main() {
  try {
    std::cout << "prepared plan fixed storage bytes=" << sizeof(PreparedEncodePlanStorage) << '\n';
    exhaustiveConstruction();
    failuresAndBounds();
    exhaustivePublication();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
