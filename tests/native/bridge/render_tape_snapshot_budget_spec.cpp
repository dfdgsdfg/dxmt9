#include "device_c_render_tape_snapshot_budget.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace snapshot_budget_allocation_probe {

thread_local bool enabled = false;
thread_local bool fail = false;
thread_local std::size_t count = 0u;
void* watchedPayload = nullptr;
dxmt9::d3d9::RenderTapeSnapshotBudget* watchedBudget = nullptr;
std::uint64_t retainedAtPayloadDelete = 0u;
bool payloadDeleted = false;

} // namespace snapshot_budget_allocation_probe

void* operator new(std::size_t size) {
  if (snapshot_budget_allocation_probe::enabled) {
    ++snapshot_budget_allocation_probe::count;
    if (snapshot_budget_allocation_probe::fail) throw std::bad_alloc();
  }
  if (void* value = std::malloc(size == 0u ? 1u : size)) return value;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* value) noexcept {
  if (value && value == snapshot_budget_allocation_probe::watchedPayload) {
    snapshot_budget_allocation_probe::payloadDeleted = true;
    snapshot_budget_allocation_probe::retainedAtPayloadDelete =
        snapshot_budget_allocation_probe::watchedBudget->gauges().retainedCurrentBytes;
    snapshot_budget_allocation_probe::watchedPayload = nullptr;
  }
  std::free(value);
}

void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

namespace {

using namespace dxmt9::d3d9;
using Status = RenderTapeSnapshotBudgetStatus;
using Kind = RenderTapeSnapshotBudgetKind;
using State = RenderTapeSnapshotBudgetState;

constexpr RenderTapeSnapshotBudgetLimits kSmallLimits{8u, 4u, 12u};

static_assert(std::is_nothrow_copy_constructible_v<RenderTapeSnapshotBudgetLease>);
static_assert(std::is_nothrow_copy_assignable_v<RenderTapeSnapshotBudgetLease>);
static_assert(std::is_nothrow_move_constructible_v<RenderTapeSnapshotBudgetLease>);
static_assert(std::is_nothrow_move_assignable_v<RenderTapeSnapshotBudgetLease>);
static_assert(std::is_nothrow_destructible_v<RenderTapeSnapshotBudgetLease>);

void check(bool value, std::string_view message) {
  if (!value) throw std::runtime_error(std::string(message));
}

void beginAllocationFault() noexcept {
  snapshot_budget_allocation_probe::count = 0u;
  snapshot_budget_allocation_probe::fail = true;
  snapshot_budget_allocation_probe::enabled = true;
}

std::size_t endAllocationFault() noexcept {
  snapshot_budget_allocation_probe::enabled = false;
  snapshot_budget_allocation_probe::fail = false;
  return snapshot_budget_allocation_probe::count;
}

void testDerivedLimitsAndInitialization() {
  constexpr auto mib = 1024ull * 1024ull;
  check(renderTapeDeriveSnapshotBudgetLimits(256u * mib) ==
            RenderTapeSnapshotBudgetLimits{256u * mib, 64u * mib, 128u * mib},
        "default session budget derives independent resource/staging caps");
  check(renderTapeDeriveSnapshotBudgetLimits(16u * mib) ==
            RenderTapeSnapshotBudgetLimits{16u * mib, 16u * mib, 32u * mib},
        "small session budget tightens all three policies");
  check(renderTapeDeriveSnapshotBudgetLimits(1024u * mib) ==
            RenderTapeSnapshotBudgetLimits{1024u * mib, 64u * mib, 128u * mib},
        "hard normalized session ceiling preserves staging/resource caps");
  check(renderTapeDeriveSnapshotBudgetLimits(0u) == RenderTapeSnapshotBudgetLimits{} &&
            renderTapeDeriveSnapshotBudgetLimits(1024u * mib + 1u) ==
                RenderTapeSnapshotBudgetLimits{},
        "zero and unnormalized limits are invalid, never silently clamped");

  RenderTapeSnapshotBudget budget;
  check(!budget.initialized() && budget.reserveRetained(1u).status ==
            Status::NotInitialized && budget.limits() == RenderTapeSnapshotBudgetLimits{},
        "uninitialized owner cannot admit bytes");
  check(budget.initialize(0u) == Status::InvalidLimits && !budget.initialized(),
        "invalid initialization remains retryable without state");
  check(budget.initialize(kSmallLimits) == Status::Complete &&
            budget.initialized() && budget.limits() == kSmallLimits,
        "owner initializes exactly once");
  auto retained = budget.reserveRetained(4u);
  check(retained.status == Status::Complete && retained.lease.valid(),
        "initialized owner admits a retained payload");
  const auto before = budget.gauges();
  check(budget.initialize(1u) == Status::AlreadyInitialized &&
            budget.gauges() == before && budget.limits() == kSmallLimits,
        "reinitialization cannot erase a live reservation or change limits");
  retained.lease.reset();
  check(budget.initialize(1u) == Status::AlreadyInitialized,
        "empty owner still cannot establish a second capture policy");
}

void testReducerInvalidInputsAndOverflow() {
  State state{kSmallLimits, {}};
  auto before = state;
  check(renderTapeSnapshotBudgetAdmit(state, Kind::Retained, 0u, 0u) ==
            Status::ZeroBytes && state == before,
        "zero admission does not mutate gauges");
  check(renderTapeSnapshotBudgetAdmit(state, Kind::Retained, 2u, 3u) ==
            Status::InvalidRequest && state == before,
        "retained resource and allocation charge must match");
  check(renderTapeSnapshotBudgetAdmit(state, static_cast<Kind>(99u), 1u, 1u) ==
            Status::InvalidRequest && state == before,
        "unknown budget kind is invalid");
  check(renderTapeSnapshotBudgetRelease(state, Kind::Retained, 1u) ==
            Status::InvalidRelease && state == before,
        "release cannot underflow an empty pool");
  check(renderTapeSnapshotBudgetRelease(state, Kind::Transient, 0u) ==
            Status::ZeroBytes && state == before,
        "zero release is invalid without mutating peaks");
  check(renderTapeSnapshotBudgetRelease(state, static_cast<Kind>(99u), 1u) ==
            Status::InvalidRequest && state == before,
        "unknown release kind is invalid");

  state.limits.retainedBytes = std::numeric_limits<std::uint64_t>::max();
  before = state;
  check(renderTapeSnapshotBudgetAdmit(state, Kind::Retained, 1u, 1u) ==
            Status::InvalidLimits && state == before,
        "invalid limits fail before arithmetic");
  state = {kSmallLimits, {2u, 1u, 0u, 0u}};
  before = state;
  check(renderTapeSnapshotBudgetAdmit(state, Kind::Transient, 1u, 1u) ==
            Status::InvalidState && state == before &&
            renderTapeSnapshotBudgetRelease(state, Kind::Retained, 1u) ==
                Status::InvalidState && state == before,
        "corrupt current/peak accounting fails closed");
  state = {kSmallLimits, {1u, 1u, 1u, 1u}};
  before = state;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  check(renderTapeSnapshotBudgetAdmit(state, Kind::Retained, maximum, maximum) ==
            Status::Overflow && state == before,
        "retained addition overflow is distinguished without mutation");
  check(renderTapeSnapshotBudgetAdmit(state, Kind::Transient, 1u, maximum) ==
            Status::Overflow && state == before,
        "transient addition overflow is distinguished without mutation");

  std::uint64_t product = 77u;
  check(renderTapeSnapshotBudgetCheckedMultiply(maximum, 2u, product) ==
            Status::Overflow && product == 0u &&
            renderTapeSnapshotBudgetCheckedMultiply(1u, 0u, product) ==
                Status::ZeroBytes && product == 0u,
        "failed staging arithmetic clears any stale computed output");
  check(renderTapeSnapshotBudgetCheckedMultiply(4u, 2u, product) ==
            Status::Complete && product == 8u,
        "staging multiplier derives an exact independent charge");
  for (unsigned value = 0u; value <= static_cast<unsigned>(Status::InternalFailure);
       ++value) {
    check(std::string_view(renderTapeSnapshotBudgetStatusName(
              static_cast<Status>(value))) != "unknown",
          "every defined denial status has a stable diagnostic name");
  }
}

struct Action {
  Kind kind;
  std::uint64_t resourceBytes;
  std::uint64_t chargedBytes;
  bool release;
};

constexpr std::array kActions{
    Action{Kind::Retained, 2u, 2u, false},
    Action{Kind::Retained, 4u, 4u, false},
    Action{Kind::Retained, 5u, 5u, false},
    Action{Kind::Retained, 0u, 0u, false},
    Action{Kind::Transient, 2u, 4u, false},
    Action{Kind::Transient, 4u, 8u, false},
    Action{Kind::Transient, 5u, 2u, false},
    Action{Kind::Transient, 1u, 13u, false},
    Action{Kind::Retained, 0u, 2u, true},
    Action{Kind::Retained, 0u, 4u, true},
    Action{Kind::Transient, 0u, 4u, true},
    Action{Kind::Transient, 0u, 8u, true},
};

// Small-number oracle: use direct arithmetic and independent state updates,
// rather than invoking production admission/release to derive expected state.
Status referenceAction(State& state, const Action& action) {
  auto& current = action.kind == Kind::Retained
      ? state.gauges.retainedCurrentBytes : state.gauges.transientCurrentBytes;
  auto& peak = action.kind == Kind::Retained
      ? state.gauges.retainedPeakBytes : state.gauges.transientPeakBytes;
  if (action.release) {
    if (action.chargedBytes == 0u) return Status::ZeroBytes;
    if (action.chargedBytes > current) return Status::InvalidRelease;
    current -= action.chargedBytes;
    return Status::Complete;
  }
  if (action.resourceBytes == 0u || action.chargedBytes == 0u)
    return Status::ZeroBytes;
  if (action.resourceBytes > state.limits.perResourceBytes)
    return Status::PerResourceLimit;
  const auto next = current + action.chargedBytes;
  const auto limit = action.kind == Kind::Retained
      ? state.limits.retainedBytes : state.limits.transientBytes;
  if (next > limit)
    return action.kind == Kind::Retained ? Status::RetainedLimit : Status::TransientLimit;
  current = next;
  if (peak < next) peak = next;
  return Status::Complete;
}

void testExhaustiveReducerValuations() {
  std::size_t valuations = 0u;
  for (std::uint64_t retained = 0u; retained <= kSmallLimits.retainedBytes; ++retained) {
    for (std::uint64_t transient = 0u; transient <= kSmallLimits.transientBytes; ++transient) {
      for (unsigned retainedPeak = 0u; retainedPeak < 2u; ++retainedPeak) {
        for (unsigned transientPeak = 0u; transientPeak < 2u; ++transientPeak) {
          const State initial{kSmallLimits,
              {retained, retainedPeak ? kSmallLimits.retainedBytes : retained,
               transient, transientPeak ? kSmallLimits.transientBytes : transient}};
          const auto evaluate = [&](const Action& action) {
            auto production = initial;
            auto reference = initial;
            const auto expected = referenceAction(reference, action);
            const auto actual = action.release
                ? renderTapeSnapshotBudgetRelease(production, action.kind, action.chargedBytes)
                : renderTapeSnapshotBudgetAdmit(production, action.kind,
                                               action.resourceBytes, action.chargedBytes);
            check(actual == expected && production == reference &&
                      renderTapeSnapshotBudgetStateValid(production),
                  "all bounded current/peak/request valuations refine the serial ledger");
            if (actual != Status::Complete)
              check(production == initial, "rejected valuation preserves its complete state");
            ++valuations;
          };
          for (std::uint64_t bytes = 0u; bytes <= 14u; ++bytes) {
            evaluate({Kind::Retained, bytes, bytes, false});
            evaluate({Kind::Retained, 0u, bytes, true});
            evaluate({Kind::Transient, 0u, bytes, true});
            for (std::uint64_t resource = 0u; resource <= 5u; ++resource)
              evaluate({Kind::Transient, resource, bytes, false});
          }
        }
      }
    }
  }
  check(valuations == 63180u,
        "both pools, all bounded currents, endpoint peaks, requests and releases are covered");
}

void visitReducerTraces(State production, State reference, unsigned depth,
                        std::size_t& histories) {
  if (depth == 0u) {
    ++histories;
    return;
  }
  for (const auto& action : kActions) {
    auto nextProduction = production;
    auto nextReference = reference;
    const auto expected = referenceAction(nextReference, action);
    const auto actual = action.release
        ? renderTapeSnapshotBudgetRelease(nextProduction, action.kind, action.chargedBytes)
        : renderTapeSnapshotBudgetAdmit(nextProduction, action.kind,
                                       action.resourceBytes, action.chargedBytes);
    check(actual == expected && nextProduction == nextReference &&
              renderTapeSnapshotBudgetStateValid(nextProduction),
          "bounded admission/release trace refines the serial byte ledger");
    if (actual != Status::Complete)
      check(nextProduction == production, "denied trace step preserves current and peak");
    visitReducerTraces(nextProduction, nextReference, depth - 1u, histories);
  }
}

void testExhaustiveReducerTraces() {
  std::size_t histories = 0u;
  const State initial{kSmallLimits, {}};
  visitReducerTraces(initial, initial, 5u, histories);
  check(histories == 248832u, "all twelve-action length-five histories are enumerated");
}

void testLeaseSharingTransferAndTransientScope() {
  RenderTapeSnapshotBudget budget;
  check(budget.initialize(kSmallLimits) == Status::Complete, "initialize lease fixture");
  auto retained = budget.reserveRetained(4u);
  check(retained && retained.lease.resourceBytes() == 4u &&
            retained.lease.chargedBytes() == 4u && retained.lease.kind() == Kind::Retained,
        "retained receipt exposes its exact allocation admission");
  auto alias = retained.lease;
  auto moved = std::move(retained.lease);
  check(!retained.lease.valid() && budget.gauges().retainedCurrentBytes == 4u,
        "copy and move keep one charge without inventing a backing identity");
  {
    auto transient = budget.reserveTransient(4u, 8u);
    check(transient && transient.lease.kind() == Kind::Transient &&
              transient.lease.resourceBytes() == 4u &&
              transient.lease.chargedBytes() == 8u &&
              budget.gauges() == RenderTapeSnapshotBudgetGauges{4u, 4u, 8u, 8u},
          "transient charge is additional staging only, not retained plus staging");
  }
  check(budget.gauges() == RenderTapeSnapshotBudgetGauges{4u, 4u, 0u, 8u},
        "provider scope returns its transient reservation without touching retained bytes");
  moved.reset();
  check(budget.gauges().retainedCurrentBytes == 4u,
        "one retained alias survives another alias's release");
  alias.reset();
  alias.reset();
  check(budget.gauges() == RenderTapeSnapshotBudgetGauges{0u, 4u, 0u, 8u},
        "final retained release occurs once and leaves lifetime peaks");
}

void testAllocationBindingAndMove() {
  RenderTapeSnapshotBudget budget;
  check(budget.initialize(kSmallLimits) == Status::Complete, "initialize binding fixture");
  auto retained = budget.reserveRetained(4u);
  std::vector<std::byte> original(4u);
  std::vector<std::byte> independent(4u);
  check(!retained.lease.boundPayloadMatches(original.data(), original.size()) &&
            !retained.lease.bindRetainedPayload(nullptr, 4u) &&
            !retained.lease.bindRetainedPayload(original.data(), 3u),
        "adoption cannot precede a valid exact-size first binding");
  check(retained.lease.bindRetainedPayload(original.data(), original.size()) &&
            retained.lease.boundPayloadMatches(original.data(), original.size()),
        "PE allocation binds its retained receipt exactly once");
  auto alias = retained.lease;
  check(alias.bindRetainedPayload(original.data(), original.size()) &&
            !alias.bindRetainedPayload(independent.data(), independent.size()) &&
            !alias.boundPayloadMatches(independent.data(), independent.size()),
        "shared receipt rejects independent same-size/same-content vector");
  auto independentReceipt = budget.reserveRetained(independent.size());
  check(independentReceipt &&
            independentReceipt.lease.bindRetainedPayload(independent.data(), independent.size()) &&
            !independentReceipt.lease.boundPayloadMatches(original.data(), original.size()) &&
            budget.gauges().retainedCurrentBytes == 8u,
        "independent allocation needs its own distinct charge and exact receipt");
  std::vector<std::byte> transferred(std::move(original));
  check(alias.boundPayloadMatches(transferred.data(), transferred.size()) &&
            budget.gauges().retainedCurrentBytes == 8u,
        "ownership transfer keeps the same allocation and charge");
  auto transient = budget.reserveTransient(4u, 8u);
  check(!transient.lease.bindRetainedPayload(transferred.data(), 4u) &&
            !transient.lease.boundPayloadMatches(transferred.data(), 4u),
        "transient receipt cannot masquerade as retained backing admission");
  RenderTapeSnapshotBudgetLease empty;
  check(!empty.bindRetainedPayload(transferred.data(), 4u) &&
            !empty.boundPayloadMatches(transferred.data(), 4u),
        "empty receipt cannot bind payload bytes");
}

void testMetadataAndPayloadAllocationFaults() {
  RenderTapeSnapshotBudget budget;
  beginAllocationFault();
  const auto initializeStatus = budget.initialize(kSmallLimits);
  const auto initializeAllocations = endAllocationFault();
  check(initializeStatus == Status::AllocationFailed && initializeAllocations == 1u &&
            !budget.initialized() && budget.gauges() == RenderTapeSnapshotBudgetGauges{},
        "shared-state allocation fault leaves the owner uninitialized");
  check(budget.initialize(kSmallLimits) == Status::Complete, "retry owner initialization");
  auto baseline = budget.reserveRetained(2u);
  const auto before = budget.gauges();
  beginAllocationFault();
  auto failed = budget.reserveRetained(4u);
  const auto receiptAllocations = endAllocationFault();
  check(failed.status == Status::AllocationFailed && !failed.lease.valid() &&
            receiptAllocations == 1u && budget.gauges() == before,
        "receipt allocation precedes both current and peak commit");
  beginAllocationFault();
  const auto zero = budget.reserveRetained(0u);
  const auto oversized = budget.reserveRetained(5u);
  const auto repeat = budget.initialize(kSmallLimits);
  const auto rejectedAllocations = endAllocationFault();
  check(zero.status == Status::ZeroBytes && oversized.status == Status::PerResourceLimit &&
            repeat == Status::AlreadyInitialized && rejectedAllocations == 0u &&
            budget.gauges() == before,
        "invalid admission and repeated initialization allocate no metadata");
  baseline.lease.reset();

  bool payloadFailureCaught = false;
  try {
    auto staged = budget.reserveRetained(4u);
    check(staged.status == Status::Complete, "payload allocation first reserves bytes");
    beginAllocationFault();
    std::vector<std::byte> payload(4u);
    (void)payload;
  } catch (const std::bad_alloc&) {
    payloadFailureCaught = true;
  }
  const auto payloadAllocations = endAllocationFault();
  check(payloadFailureCaught && payloadAllocations == 1u &&
            budget.gauges().retainedCurrentBytes == 0u &&
            budget.gauges().retainedPeakBytes == 4u,
        "failed payload allocation unwinds its reservation without release allocation");

  auto retained = budget.reserveRetained(4u);
  auto transient = budget.reserveTransient(4u, 8u);
  beginAllocationFault();
  retained.lease.reset();
  transient.lease.reset();
  const auto releaseAllocations = endAllocationFault();
  check(releaseAllocations == 0u && budget.gauges().retainedCurrentBytes == 0u &&
            budget.gauges().transientCurrentBytes == 0u,
        "final receipt destruction is allocation-free for both pools");
}

void testBudgetOwnerOutlivedAndPayloadDestructionOrder() {
  RenderTapeSnapshotBudgetLease orphan;
  RenderTapeSnapshotBudgetLease alias;
  {
    RenderTapeSnapshotBudget owner;
    check(owner.initialize(kSmallLimits) == Status::Complete, "initialize orphan fixture");
    orphan = owner.reserveRetained(4u).lease;
    alias = orphan;
  }
  check(orphan.valid() && alias.valid() && orphan.chargedBytes() == 4u,
        "receipt owns its shared state after the budget facade is destroyed");
  beginAllocationFault();
  orphan.reset();
  alias.reset();
  const auto orphanReleaseAllocations = endAllocationFault();
  check(orphanReleaseAllocations == 0u,
        "last orphan alias releases safely without a dangling owner or allocation");

  struct RetainedPayload {
    RenderTapeSnapshotBudgetLease lease{};
    std::vector<std::byte> bytes{};
  };
  RenderTapeSnapshotBudget owner;
  check(owner.initialize(kSmallLimits) == Status::Complete, "initialize destruction fixture");
  auto payload = std::make_unique<RetainedPayload>();
  payload->lease = owner.reserveRetained(4u).lease;
  payload->bytes.resize(4u);
  check(payload->lease.bindRetainedPayload(payload->bytes.data(), payload->bytes.size()),
        "destruction fixture authenticates the exact allocation");
  snapshot_budget_allocation_probe::watchedPayload = payload->bytes.data();
  snapshot_budget_allocation_probe::watchedBudget = &owner;
  snapshot_budget_allocation_probe::payloadDeleted = false;
  payload.reset();
  check(snapshot_budget_allocation_probe::payloadDeleted &&
            snapshot_budget_allocation_probe::retainedAtPayloadDelete == 4u &&
            owner.gauges().retainedCurrentBytes == 0u,
        "receipt field before bytes releases only after actual payload destruction");
  snapshot_budget_allocation_probe::watchedBudget = nullptr;

  auto oldPayload = std::make_shared<RetainedPayload>();
  oldPayload->lease = owner.reserveRetained(4u).lease;
  oldPayload->bytes.resize(4u);
  check(oldPayload->lease.bindRetainedPayload(
            oldPayload->bytes.data(), oldPayload->bytes.size()),
        "old shared storage binds its first receipt");
  auto oldAlias = oldPayload;
  auto replacement = std::make_shared<RetainedPayload>();
  replacement->lease = owner.reserveRetained(4u).lease;
  replacement->bytes.resize(4u);
  check(replacement->lease.bindRetainedPayload(
            replacement->bytes.data(), replacement->bytes.size()),
        "replacement storage has independent retained admission");
  snapshot_budget_allocation_probe::watchedPayload = oldPayload->bytes.data();
  snapshot_budget_allocation_probe::watchedBudget = &owner;
  snapshot_budget_allocation_probe::payloadDeleted = false;
  oldPayload = std::move(replacement);
  check(!replacement && !snapshot_budget_allocation_probe::payloadDeleted &&
            owner.gauges().retainedCurrentBytes == 8u,
        "shared storage move assignment preserves the displaced backing's surviving alias");
  oldAlias.reset();
  check(snapshot_budget_allocation_probe::payloadDeleted &&
            snapshot_budget_allocation_probe::retainedAtPayloadDelete == 8u &&
            owner.gauges().retainedCurrentBytes == 4u,
        "displaced backing's last reference destroys its bytes before returning its charge");
  snapshot_budget_allocation_probe::watchedPayload = oldPayload->bytes.data();
  snapshot_budget_allocation_probe::payloadDeleted = false;
  oldPayload.reset();
  check(snapshot_budget_allocation_probe::payloadDeleted &&
            snapshot_budget_allocation_probe::retainedAtPayloadDelete == 4u &&
            owner.gauges().retainedCurrentBytes == 0u,
        "move-assigned replacement also releases only after byte destruction");
  snapshot_budget_allocation_probe::watchedBudget = nullptr;
}

struct TraceReceipt {
  Kind kind = Kind::Retained;
  std::uint64_t chargedBytes = 0u;
  unsigned aliases = 0u;
};

void testExhaustiveSharedLeaseTraces() {
  constexpr std::size_t histories = 10000u;
  for (std::size_t history = 0u; history < histories; ++history) {
    RenderTapeSnapshotBudget budget;
    check(budget.initialize(kSmallLimits) == Status::Complete, "initialize trace owner");
    State model{kSmallLimits, {}};
    std::array<RenderTapeSnapshotBudgetLease, 4u> slots{};
    std::array<int, 4u> slotReceipt{-1, -1, -1, -1};
    std::array<TraceReceipt, 4u> receipts{};
    unsigned nextReceipt = 0u;
    const auto dropSlot = [&](unsigned slot) {
      const auto id = slotReceipt[slot];
      if (id >= 0) {
        auto& receipt = receipts[static_cast<std::size_t>(id)];
        if (--receipt.aliases == 0u) {
          const auto status = referenceAction(
              model, {receipt.kind, 0u, receipt.chargedBytes, true});
          check(status == Status::Complete, "reference final alias releases exactly once");
        }
      }
      slotReceipt[slot] = -1;
    };
    auto encoded = history;
    for (unsigned step = 0u; step < 4u; ++step) {
      const auto action = encoded % 10u;
      encoded /= 10u;
      if (action < 2u || action == 8u) {
        const bool transient = action == 1u;
        const unsigned slot = transient ? 1u : 0u;
        const Action request{transient ? Kind::Transient : Kind::Retained,
                             2u, transient ? 4u : 2u, false};
        auto expectedNext = model;
        auto expectedStatus = referenceAction(expectedNext, request);
        if (action == 8u) beginAllocationFault();
        auto admitted = transient ? budget.reserveTransient(2u, 4u)
                                  : budget.reserveRetained(2u);
        if (action == 8u) {
          (void)endAllocationFault();
          if (expectedStatus == Status::Complete) expectedStatus = Status::AllocationFailed;
        }
        check(admitted.status == expectedStatus, "shared-lease trace admission matches oracle");
        if (admitted.status == Status::Complete) {
          model = expectedNext;
          dropSlot(slot);
          receipts[nextReceipt] = {request.kind, request.chargedBytes, 1u};
          slotReceipt[slot] = static_cast<int>(nextReceipt++);
          slots[slot] = std::move(admitted.lease);
        } else {
          check(!admitted.lease.valid(), "denied trace admission creates no ownership");
        }
      } else if (action < 4u) {
        const unsigned source = action == 2u ? 0u : 1u;
        const unsigned destination = source + 2u;
        dropSlot(destination);
        slotReceipt[destination] = slotReceipt[source];
        if (slotReceipt[source] >= 0)
          ++receipts[static_cast<std::size_t>(slotReceipt[source])].aliases;
        slots[destination] = slots[source];
      } else if (action < 8u) {
        const auto slot = static_cast<unsigned>(action - 4u);
        dropSlot(slot);
        slots[slot].reset();
      } else {
        for (unsigned slot = 0u; slot < slots.size(); ++slot) {
          dropSlot(slot);
          slots[slot].reset();
        }
      }
      check(budget.gauges() == model.gauges,
            "actual shared receipts refine independent alias-count byte accounting");
    }
    for (unsigned slot = 0u; slot < slots.size(); ++slot) {
      dropSlot(slot);
      slots[slot].reset();
    }
    check(budget.gauges() == model.gauges && model.gauges.retainedCurrentBytes == 0u &&
              model.gauges.transientCurrentBytes == 0u,
          "trace cancellation releases all receipts but preserves peaks");
  }
}

void testConcurrentAdmissionAndBinding() {
  RenderTapeSnapshotBudget budget;
  check(budget.initialize(kSmallLimits) == Status::Complete, "initialize concurrent fixture");
  std::atomic<bool> failed{false};
  std::array<std::thread, 4u> threads;
  for (auto& thread : threads) {
    thread = std::thread([&] {
      for (unsigned iteration = 0u; iteration < 1000u; ++iteration) {
        auto retained = budget.reserveRetained(1u);
        auto transient = budget.reserveTransient(1u, 2u);
        if (!retained || !transient) failed.store(true, std::memory_order_relaxed);
        auto alias = retained.lease;
        retained.lease.reset();
      }
    });
  }
  for (auto& thread : threads) thread.join();
  const auto gauges = budget.gauges();
  check(!failed.load(std::memory_order_relaxed) && gauges.retainedCurrentBytes == 0u &&
            gauges.transientCurrentBytes == 0u && gauges.retainedPeakBytes <= 4u &&
            gauges.transientPeakBytes <= 8u,
        "concurrent cold admission/release obeys cumulative bounds");

  std::array<std::byte, 4u> first{};
  std::array<std::byte, 4u> second{};
  auto reservation = budget.reserveRetained(4u);
  auto alias = reservation.lease;
  std::atomic<unsigned> ready{0u};
  std::atomic<bool> start{false};
  std::array<bool, 2u> bound{};
  std::thread firstBinder([&] {
    ready.fetch_add(1u, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    bound[0] = reservation.lease.bindRetainedPayload(first.data(), first.size());
  });
  std::thread secondBinder([&] {
    ready.fetch_add(1u, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    bound[1] = alias.bindRetainedPayload(second.data(), second.size());
  });
  while (ready.load(std::memory_order_acquire) != 2u) std::this_thread::yield();
  start.store(true, std::memory_order_release);
  firstBinder.join();
  secondBinder.join();
  check(bound[0] != bound[1] &&
            reservation.lease.boundPayloadMatches(first.data(), first.size()) == bound[0] &&
            alias.boundPayloadMatches(second.data(), second.size()) == bound[1],
        "racing distinct allocations cannot bind one shared retained receipt twice");
}

} // namespace

int main() {
  try {
    testDerivedLimitsAndInitialization();
    testReducerInvalidInputsAndOverflow();
    testExhaustiveReducerValuations();
    testExhaustiveReducerTraces();
    testLeaseSharingTransferAndTransientScope();
    testAllocationBindingAndMove();
    testMetadataAndPayloadAllocationFaults();
    testBudgetOwnerOutlivedAndPayloadDestructionOrder();
    testExhaustiveSharedLeaseTraces();
    testConcurrentAdmissionAndBinding();
    std::cout << "render-tape snapshot budget spec passed\n";
    return 0;
  } catch (const std::exception& error) {
    snapshot_budget_allocation_probe::enabled = false;
    std::cerr << "render-tape snapshot budget spec failed: " << error.what() << '\n';
    return 1;
  }
}
