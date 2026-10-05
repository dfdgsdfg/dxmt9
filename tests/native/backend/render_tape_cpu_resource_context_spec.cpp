#include "render_tape_cpu_resource_context.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// This executable alone replaces allocation to exercise cold factory unwind.
// The shared harness and production paths contain no allocation-fault hooks.
namespace {
std::atomic<std::ptrdiff_t> allocationFailureAfter{-1};
}

void* operator new(std::size_t bytes) {
  auto remaining = allocationFailureAfter.load(std::memory_order_relaxed);
  while (remaining >= 0) {
    if (allocationFailureAfter.compare_exchange_weak(
            remaining, remaining - 1, std::memory_order_relaxed)) {
      if (!remaining) throw std::bad_alloc();
      break;
    }
  }
  if (auto* memory = std::malloc(bytes ? bytes : 1u)) return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using namespace dxmt9;
using namespace dxmt9::test;
using Status = CpuTapeResourceContextStatus;
using Version = std::shared_ptr<const CpuTapeResourceVersion>;
using Context = std::shared_ptr<const CpuTapeResourceContext>;
using Budget = std::shared_ptr<CpuTapeExpectedBufferBudget>;
using Locator = render::TapeEmissionRecordLocator;
using Purpose = CpuTapeResourceContextPurpose;
using Facts = resources::BufferVersionObservation;
constexpr D9CWireObjectIdentity buffer{D9C_CHUNK_HANDLE_KIND_BUFFER, 7u, 701u};
constexpr D9CWireObjectIdentity texture{D9C_CHUNK_HANDLE_KIND_TEXTURE, 8u, 702u};
constexpr D9CWireObjectIdentity surface{D9C_CHUNK_HANDLE_KIND_SURFACE, 9u, 703u};
std::size_t checks = 0u;

struct AllocationFailure {
  explicit AllocationFailure(std::ptrdiff_t successfulAllocations) {
    allocationFailureAfter.store(successfulAllocations, std::memory_order_relaxed);
  }
  ~AllocationFailure() { allocationFailureAfter.store(-1, std::memory_order_relaxed); }
};

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

bool sameIdentity(D9CWireObjectIdentity a, D9CWireObjectIdentity b) {
  return a.kind == b.kind && a.generation == b.generation && a.objectId == b.objectId;
}

CpuTapeResourceObservation observation(D9CWireObjectIdentity identity,
    std::uint64_t revision, std::optional<std::uint32_t> alias = std::nullopt) {
  const auto content = alias ? texture : identity;
  std::array receipts{CpuTapeResourceReceipt{.identity = content,
      .subresource = alias.value_or(0u), .revision = revision}};
  for (std::size_t index = 0u; index < receipts[0].digest.size(); ++index)
    receipts[0].digest[index] = static_cast<std::byte>((revision + index) & 0xffu);
  CpuTapeResourceObservation out;
  check(buildCpuTapeResourceObservation({.identity = identity, .contentIdentity = content,
      .aliasSubresource = alias, .receipts = receipts}, out) == CpuTapeResourceObservationStatus::Accepted,
      "resource observation owns its logical receipt values");
  return out;
}

Version version(D9CWireObjectIdentity identity, core::Handle handle, std::uint64_t epoch,
    std::span<const std::uint8_t> bytes = {}, std::optional<std::uint32_t> alias = std::nullopt) {
  const auto normalized = observation(identity, epoch, alias);
  Version out;
  check(buildCpuTapeResourceVersion({.nativeKind = identity.kind, .nativeHandle = handle,
      .contentEpoch = epoch, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes},
      normalized, out) == Status::Accepted, "immutable version copies normalized observation and expected bytes");
  return out;
}

Context context(std::span<const Version> entries) {
  Context out;
  check(buildCpuTapeResourceContext(entries, out) == Status::Accepted,
      "immutable context owns a bounded vector of shared const versions");
  return out;
}

Budget budget(std::size_t bytes) {
  Budget out;
  check(createCpuTapeExpectedBufferBudget({bytes}, out) == Status::Accepted,
      "explicit positive expected-byte session budget is admitted");
  const auto snapshot = out->snapshot();
  check(snapshot.maxRetainedExpectedBufferBytes == bytes && !snapshot.chargedExpectedBufferBytes &&
      !snapshot.highWaterExpectedBufferBytes, "new budget has zero charged ownership and reservation high-water");
  return out;
}

// Pure copied shape fixtures only, not actual Pool observation, retention or
// proof of old-ring/current/future physical buffer bytes.
Facts facts(core::Handle handle, std::size_t bytes, std::uint64_t revision) {
  return {.status = resources::BufferVersionObservationStatus::Valid, .buffer = handle,
      .byteSize = bytes, .contentRevision = revision, .hasVersionedBacking = true,
      .currentBacking = {.metalHandle = 901u, .byteSize = bytes}};
}

Version budgetedVersion(const Budget& owner, D9CWireObjectIdentity identity, core::Handle handle,
    std::uint64_t epoch, std::span<const std::uint8_t> bytes = {}, std::optional<Facts> local = std::nullopt) {
  const auto normalized = observation(identity, epoch);
  Version out;
  check(buildBudgetedCpuTapeResourceVersion({.nativeKind = identity.kind, .nativeHandle = handle,
      .contentEpoch = epoch, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes,
      .localBufferFacts = local},
      normalized, owner, out) == Status::Accepted && out->aggregateBudgeted() &&
      out->usesExpectedBufferBudget(*owner), "budgeted immutable version carries opaque exact session association");
  return out;
}

void immutableHistoricalBufferFacts() {
  auto owner = budget(12u);
  const core::Handle handle{481u};
  const std::array<std::uint8_t, 4u> firstBytes{1u, 2u, 3u, 4u};
  const std::array<std::uint8_t, 4u> nextBytes{9u, 2u, 3u, 4u};
  const auto firstObservation = observation(buffer, 7u);
  CpuTapeResourceVersionInput input{.nativeKind = buffer.kind, .nativeHandle = handle,
      .contentEpoch = 7u, .expectedBufferByteSize = firstBytes.size(), .expectedBufferBytes = firstBytes,
      .localBufferFacts = facts(handle, firstBytes.size(), 51u)};
  Version first;
  check(buildBudgetedCpuTapeResourceVersion(input, firstObservation, owner, first) == Status::Accepted &&
      first->contentEpoch() == 7u && first->localBufferFacts()->contentRevision == 51u &&
      first->usesExpectedBufferBudget(*owner),
      "immutable version copies instantaneous facts while keeping logical epoch and local revision separate");
  input.localBufferFacts->contentRevision = 99u;
  input.localBufferFacts->currentBacking.metalHandle = 999u;
  input.localBufferFacts.reset();
  check(first->localBufferFacts()->contentRevision == 51u && first->localBufferFacts()->currentBacking.metalHandle == 901u,
      "mutating or discarding caller input cannot rewrite immutable version facts");
  auto nextFacts = facts(handle, nextBytes.size(), 77u);
  nextFacts.currentBacking.metalHandle = 902u;
  nextFacts.hasVersionedBacking = false;
  const auto next = budgetedVersion(owner, buffer, handle, 8u, nextBytes, nextFacts);
  const std::array firstEntries{first};
  const std::array nextEntries{next};
  auto oldContext = context(firstEntries);
  auto nextContext = context(nextEntries);
  Context oldInventory, nextInventory;
  check(buildCpuTapeV4SurfaceAliasInventory(firstEntries, owner, oldInventory) == Status::Accepted &&
      buildCpuTapeV4SurfaceAliasInventory(nextEntries, owner, nextInventory) == Status::Accepted &&
      oldContext->findIdentity(buffer)->localBufferFacts()->contentRevision == 51u &&
      nextContext->findIdentity(buffer)->localBufferFacts()->contentRevision == 77u &&
      oldInventory->findIdentity(buffer) == first && nextInventory->findIdentity(buffer) == next &&
      !next->localBufferFacts()->hasVersionedBacking && next->localBufferFacts()->currentBacking.metalHandle == 902u,
      "both construction purposes share original instantaneous facts including legal nonversioned backing metadata");
  CpuTapeRecordContextStore store;
  check(store.bind({18u, 0u}, oldInventory) == Status::Accepted &&
      store.bind({18u, 1u}, nextInventory) == Status::Accepted &&
      store.bind({18u, 0u}, nextInventory) == Status::Conflict &&
      store.lookup({18u, 0u})->findIdentity(buffer)->localBufferFacts()->contentRevision == 51u &&
      store.lookup({18u, 1u})->findIdentity(buffer)->localBufferFacts()->contentRevision == 77u &&
      owner->snapshot().chargedExpectedBufferBytes == 8u,
      "write-once record contexts preserve old facts alongside complete expected images without duplicate image charging");
  const auto absent = budgetedVersion(owner, buffer, handle, 9u, nextBytes);
  check(!absent->localBufferFacts() && absent->contentEpoch() == 9u &&
      owner->snapshot().chargedExpectedBufferBytes == 12u,
      "missing observation stays absent rather than borrowing a preceding physical revision or backing");
}

void localBufferFactsFactoryValidation() {
  auto owner = budget(4u);
  const std::array<std::uint8_t, 4u> bytes{1u, 2u, 3u, 4u};
  const core::Handle handle{491u};
  const auto normalized = observation(buffer, 1u);
  const CpuTapeResourceVersionInput valid{.nativeKind = buffer.kind, .nativeHandle = handle,
      .contentEpoch = 1u, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes,
      .localBufferFacts = facts(handle, bytes.size(), 51u)};
  Version out;
  check(buildBudgetedCpuTapeResourceVersion(valid, normalized, owner, out) == Status::Accepted,
      "valid instantaneous BUFFER facts are admitted with exact complete image budget");
  const auto prior = out;
  for (unsigned invalid = 0u; invalid != 6u; ++invalid) {
    auto input = valid;
    auto& local = *input.localBufferFacts;
    if (invalid == 0u) local.status = resources::BufferVersionObservationStatus::MissingBuffer;
    if (invalid == 1u) local.buffer = core::Handle{999u};
    if (invalid == 2u) ++local.byteSize;
    if (invalid == 3u) local.contentRevision = 0u;
    if (invalid == 4u) local.currentBacking.metalHandle = 0u;
    if (invalid == 5u) ++local.currentBacking.byteSize;
    check(buildBudgetedCpuTapeResourceVersion(input, normalized, owner, out) == Status::InvalidInput && out == prior &&
        buildCpuTapeResourceVersion(input, normalized, out) == Status::InvalidInput && out == prior &&
        owner->snapshot().chargedExpectedBufferBytes == 4u && owner->snapshot().highWaterExpectedBufferBytes == 4u &&
        out->localBufferFacts()->contentRevision == 51u,
        "wrong status/handle/extent/revision/backing facts reject before reservation and preserve previous output history");
  }
  auto nonbuffer = valid;
  nonbuffer.nativeKind = texture.kind;
  nonbuffer.expectedBufferByteSize = 0u;
  nonbuffer.expectedBufferBytes = {};
  const auto textureObservation = observation(texture, 1u);
  check(buildBudgetedCpuTapeResourceVersion(nonbuffer, textureObservation, owner, out) == Status::InvalidInput &&
      out == prior && buildCpuTapeResourceVersion(nonbuffer, textureObservation, out) == Status::InvalidInput && out == prior,
      "non-BUFFER resources cannot carry arbitrary physical buffer observation metadata");
  auto replacement = valid;
  replacement.contentEpoch = 2u;
  replacement.localBufferFacts->contentRevision = 77u;
  check(buildBudgetedCpuTapeResourceVersion(replacement, normalized, owner, out) == Status::CapacityExceeded &&
      out == prior && out->localBufferFacts()->contentRevision == 51u,
      "expected image budget exhaustion cannot replace or attach new immutable facts to prior version");
  Version legacy;
  check(buildCpuTapeResourceVersion(valid, normalized, legacy) == Status::Accepted &&
      legacy->localBufferFacts()->contentRevision == 51u && !legacy->aggregateBudgeted() &&
      !legacy->usesExpectedBufferBudget(*owner),
      "copied local facts alone cannot turn a legacy version into session-budget authority");
}

void sharedSessionBudgetOwnership() {
  auto owner = budget(8u);
  const std::array<std::uint8_t, 4u> bytes{1u, 2u, 3u, 4u};
  auto first = budgetedVersion(owner, buffer, core::Handle{501u}, 1u, bytes);
  auto unchanged = budgetedVersion(owner, texture, core::Handle{502u}, 1u);
  std::array entries{first, unchanged};
  auto firstContext = context(entries);
  auto sharedContext = context(entries);
  check(owner->snapshot().chargedExpectedBufferBytes == bytes.size() &&
      owner->snapshot().highWaterExpectedBufferBytes == bytes.size(),
      "multiple contexts and zero-byte resources do not charge a shared version twice");
  auto consumerVersion = firstContext->find(buffer.kind, core::Handle{501u});
  auto second = budgetedVersion(owner, buffer, core::Handle{501u}, 2u, bytes);
  entries[0] = second;
  auto secondContext = context(entries);
  check(owner->snapshot().chargedExpectedBufferBytes == 8u &&
      owner->snapshot().highWaterExpectedBufferBytes == 8u,
      "distinct immutable content versions sum even when their native handles are equal");
  entries = {};
  first.reset();
  second.reset();
  firstContext.reset();
  sharedContext.reset();
  check(owner->snapshot().chargedExpectedBufferBytes == 8u && consumerVersion->expectedBufferBytes()[0] == 1u,
      "destroying producer contexts cannot release a consumer-held version's byte credit");
  consumerVersion.reset();
  check(owner->snapshot().chargedExpectedBufferBytes == 4u,
      "last shared version reference, not last context reference, releases the exact charge");
  secondContext.reset();
  check(!owner->snapshot().chargedExpectedBufferBytes && owner->snapshot().highWaterExpectedBufferBytes == 8u,
      "all expected-byte owners release current usage while reservation high-water persists");
  const auto legacy = version(buffer, core::Handle{503u}, 1u, bytes);
  check(!legacy->aggregateBudgeted() && !legacy->usesExpectedBufferBudget(*owner) &&
      !owner->snapshot().chargedExpectedBufferBytes,
      "legacy per-version factory does not falsely claim aggregate session admission");
  auto differentBudget = budget(8u);
  check(unchanged->aggregateBudgeted() && unchanged->usesExpectedBufferBudget(*owner) &&
      !unchanged->usesExpectedBufferBudget(*differentBudget),
      "zero-byte nonbuffer carries exact budget identity so future context gates can reject mixed sessions");
  std::weak_ptr<CpuTapeExpectedBufferBudget> retained = owner;
  owner.reset();
  check(!retained.expired(), "zero-byte budget association also retains session authority lifetime");
  unchanged.reset();
  check(retained.expired(), "session owner disappears after its final associated immutable version");
}

void budgetedBoundsAndReplacement() {
  auto owner = budget(4u);
  auto existingBudget = owner;
  check(createCpuTapeExpectedBufferBudget({}, existingBudget) == Status::InvalidInput && existingBudget == owner,
      "zero aggregate limits reject transactionally");
  check(createCpuTapeExpectedBufferBudget({std::numeric_limits<std::size_t>::max()}, existingBudget) ==
      Status::InvalidInput && existingBudget == owner, "unsupported aggregate arithmetic/storage limit rejects");
  const std::array<std::uint8_t, 4u> bytes{1u, 2u, 3u, 4u};
  const auto normalized = observation(buffer, 1u);
  const auto normalizedTexture = observation(texture, 1u);
  const CpuTapeResourceVersionInput valid{.nativeKind = buffer.kind, .nativeHandle = core::Handle{601u},
      .contentEpoch = 1u, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes};
  auto out = budgetedVersion(owner, buffer, valid.nativeHandle, 1u, bytes);
  const auto prior = out;
  check(buildBudgetedCpuTapeResourceVersion(valid, normalized, owner, out) == Status::CapacityExceeded && out == prior &&
      owner->snapshot().chargedExpectedBufferBytes == 4u && owner->snapshot().highWaterExpectedBufferBytes == 4u,
      "limit-full replacement must reserve coexistence credit without silently destroying prior output");
  for (std::uint32_t invalid = 0u; invalid != 9u; ++invalid) {
    auto input = valid;
    auto suppliedBudget = owner;
    CpuTapeResourceVersionLimits limits;
    const CpuTapeResourceObservation* suppliedObservation = &normalized;
    CpuTapeResourceObservation empty;
    if (invalid == 0u) suppliedBudget.reset();
    if (invalid == 1u) limits.maxExpectedBufferBytes = 0u;
    if (invalid == 2u) limits.maxExpectedBufferBytes = std::numeric_limits<std::size_t>::max();
    if (invalid == 3u) input.nativeHandle = {};
    if (invalid == 4u) input.contentEpoch = 0u;
    if (invalid == 5u) ++input.expectedBufferByteSize;
    if (invalid == 6u) input.nativeKind = texture.kind;
    if (invalid == 7u) suppliedObservation = &empty;
    if (invalid == 8u) {
      input.nativeKind = texture.kind;
      suppliedObservation = &normalizedTexture;
    }
    check(buildBudgetedCpuTapeResourceVersion(input, *suppliedObservation, suppliedBudget, out, limits) ==
        Status::InvalidInput && out == prior && owner->snapshot().chargedExpectedBufferBytes == 4u &&
        owner->snapshot().highWaterExpectedBufferBytes == 4u,
        "invalid budgeted input rejects before reservation and preserves previous output");
  }
  check(buildBudgetedCpuTapeResourceVersion(valid, normalized, owner, out, {.maxExpectedBufferBytes = 3u}) ==
      Status::CapacityExceeded && out == prior && owner->snapshot().highWaterExpectedBufferBytes == 4u,
      "per-version limit remains independent and rejects before aggregate reservation");
  Version zero;
  check(buildBudgetedCpuTapeResourceVersion({.nativeKind = texture.kind, .nativeHandle = core::Handle{602u},
      .contentEpoch = 1u}, normalizedTexture, owner, zero) == Status::Accepted &&
      zero->usesExpectedBufferBudget(*owner) && owner->snapshot().chargedExpectedBufferBytes == 4u,
      "full expected-byte budget still admits zero-byte nonbuffer association");

  auto replacingBudget = budget(8u);
  auto replacing = budgetedVersion(replacingBudget, buffer, core::Handle{603u}, 1u, bytes);
  std::weak_ptr<const CpuTapeResourceVersion> replaced = replacing;
  auto replacementInput = valid;
  replacementInput.nativeHandle = core::Handle{603u};
  replacementInput.contentEpoch = 2u;
  check(buildBudgetedCpuTapeResourceVersion(replacementInput, normalized, replacingBudget, replacing) == Status::Accepted &&
      replaced.expired() && replacing->contentEpoch() == 2u &&
      replacingBudget->snapshot().chargedExpectedBufferBytes == 4u &&
      replacingBudget->snapshot().highWaterExpectedBufferBytes == 8u,
      "successful replacement charges both owners during copy then releases old version after publication");
  replacing.reset();
  check(!replacingBudget->snapshot().chargedExpectedBufferBytes,
      "replacement's final destruction releases remaining expected-byte credit");
}

void allocationFailureReturnsTentativeCredit() {
  const std::array<std::uint8_t, 4u> bytes{1u, 2u, 3u, 4u};
  const auto normalized = observation(buffer, 1u);
  const CpuTapeResourceVersionInput input{.nativeKind = buffer.kind, .nativeHandle = core::Handle{701u},
      .contentEpoch = 2u, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes,
      .localBufferFacts = facts(core::Handle{701u}, bytes.size(), 51u)};
  // Version object, shared control block, receipt copy, expected-byte copy.
  for (std::ptrdiff_t failAfter = 0; failAfter != 4; ++failAfter) {
    auto owner = budget(8u);
    auto out = budgetedVersion(owner, buffer, input.nativeHandle, 1u, bytes,
        facts(input.nativeHandle, bytes.size(), 11u));
    const auto prior = out;
    Status status;
    {
      AllocationFailure fault(failAfter);
      status = buildBudgetedCpuTapeResourceVersion(input, normalized, owner, out);
    }
    const auto usage = owner->snapshot();
    check(status == Status::AllocationFailed && out == prior && usage.chargedExpectedBufferBytes == 4u &&
        usage.highWaterExpectedBufferBytes == 8u && out->localBufferFacts()->contentRevision == 11u,
        "allocation failure releases tentative credit, preserves prior facts/output and retains reservation high-water");
  }
  auto priorBudget = budget(4u);
  for (std::ptrdiff_t failAfter = 0; failAfter != 2; ++failAfter) {
    auto out = priorBudget;
    Status status;
    {
      AllocationFailure fault(failAfter);
      status = createCpuTapeExpectedBufferBudget({8u}, out);
    }
    check(status == Status::AllocationFailed && out == priorBudget &&
        !priorBudget->snapshot().chargedExpectedBufferBytes, "budget-owner allocation rollback preserves prior session owner");
  }
}

void exhaustiveBudgetFactoryTransitions() {
  const auto normalized = observation(buffer, 1u);
  std::size_t cases = 0u;
  for (std::size_t limit = 1u; limit != 9u; ++limit)
    for (std::size_t sizeA = 1u; sizeA != 9u; ++sizeA)
      for (std::size_t sizeB = 1u; sizeB != 9u; ++sizeB)
        for (unsigned createOrder = 0u; createOrder != 2u; ++createOrder)
          for (unsigned releaseOrder = 0u; releaseOrder != 2u; ++releaseOrder) {
            auto owner = budget(limit);
            std::array<Version, 2> versions;
            const std::array sizes{sizeA, sizeB};
            const std::array<std::vector<std::uint8_t>, 2> bytes{
                std::vector<std::uint8_t>(sizeA, 1u), std::vector<std::uint8_t>(sizeB, 2u)};
            std::size_t charged = 0u, highWater = 0u;
            const auto attempt = [&](unsigned index) {
              const auto expected = sizes[index] <= limit - charged ? Status::Accepted : Status::CapacityExceeded;
              const auto status = buildBudgetedCpuTapeResourceVersion({.nativeKind = buffer.kind,
                  .nativeHandle = core::Handle{801u + index}, .contentEpoch = index + 1u,
                  .expectedBufferByteSize = sizes[index], .expectedBufferBytes = bytes[index]},
                  normalized, owner, versions[index]);
              if (expected == Status::Accepted) charged += sizes[index];
              highWater = std::max(highWater, charged);
              const auto actual = owner->snapshot();
              check(status == expected && actual.chargedExpectedBufferBytes == charged &&
                  actual.highWaterExpectedBufferBytes == highWater &&
                  bool(versions[index]) == (expected == Status::Accepted),
                  "exhaustive actual factory reserve conserves exact cumulative credit at every bound/order");
            };
            attempt(createOrder);
            attempt(1u - createOrder);
            if (versions[releaseOrder]) charged -= sizes[releaseOrder];
            versions[releaseOrder].reset();
            check(owner->snapshot().chargedExpectedBufferBytes == charged,
                "exhaustive release cannot charge twice or release another version's credit");
            attempt(releaseOrder);
            versions[releaseOrder].reset();
            versions[1u - releaseOrder].reset();
            check(!owner->snapshot().chargedExpectedBufferBytes &&
                owner->snapshot().highWaterExpectedBufferBytes == highWater,
                "exhaustive create/release/retry sequence ends with zero owned expected bytes");
            ++cases;
          }
  check(cases == 2048u, "small cumulative limits/sizes and both creation/release orders are exhausted");
}

void concurrentBudgetReservationsAndReleases() {
  constexpr std::size_t workers = 16u;
  auto owner = budget(8u);
  const auto normalized = observation(buffer, 1u);
  const std::array<std::uint8_t, 2u> bytes{1u, 2u};
  std::array<Version, workers> versions;
  std::array<Status, workers> statuses{};
  std::array<std::jthread, workers> threads;
  for (std::size_t index = 0u; index != workers; ++index)
    threads[index] = std::jthread([&, index] {
      statuses[index] = buildBudgetedCpuTapeResourceVersion({.nativeKind = buffer.kind,
          .nativeHandle = core::Handle{901u + static_cast<std::uint32_t>(index)}, .contentEpoch = index + 1u,
          .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes}, normalized, owner, versions[index]);
    });
  for (auto& thread : threads) thread.join();
  std::size_t admitted = 0u;
  for (std::size_t index = 0u; index != workers; ++index) {
    if (statuses[index] == Status::Accepted) ++admitted;
    check(statuses[index] == (versions[index] ? Status::Accepted : Status::CapacityExceeded),
        "concurrent reserve either owns one whole version or leaves no output");
  }
  check(admitted == 4u && owner->snapshot().chargedExpectedBufferBytes == 8u &&
      owner->snapshot().highWaterExpectedBufferBytes == 8u,
      "mutex-linearized concurrent factories never oversubscribe the aggregate cap");
  for (std::size_t index = 0u; index != workers; ++index)
    threads[index] = std::jthread([&, index] { versions[index].reset(); });
  for (auto& thread : threads) thread.join();
  check(!owner->snapshot().chargedExpectedBufferBytes && owner->snapshot().highWaterExpectedBufferBytes == 8u,
      "concurrent final owner releases conserve credit without resetting high-water");
}

void recordEpochsAndSharedVersions() {
  std::vector<std::uint8_t> original{1u, 2u, 3u, 4u};
  const auto first = version(buffer, core::Handle{101u}, 1u, original);
  const auto unchanged = version(texture, core::Handle{102u}, 1u);
  std::array entries{first, unchanged};
  const auto firstContext = context(entries);
  original[0] = 91u;
  const auto second = version(buffer, core::Handle{101u}, 2u, original);
  entries[0] = second;
  const auto secondContext = context(entries);
  entries = {};
  original.assign(100u, 77u);
  check(first->expectedBufferBytes().size() == 4u && first->expectedBufferBytes()[0] == 1u &&
      second->expectedBufferBytes().size() == 4u && second->expectedBufferBytes()[0] == 91u,
      "mutation and reallocation of source bytes cannot rewrite either complete expected version");
  check(firstContext->find(buffer.kind, core::Handle{101u}) == first &&
      secondContext->find(buffer.kind, core::Handle{101u}) == second &&
      firstContext->find(texture.kind, core::Handle{102u}) == unchanged &&
      secondContext->find(texture.kind, core::Handle{102u}) == unchanged,
      "changed versions separate while unchanged versions share the same immutable owner");
  check(!firstContext->find(buffer.kind, core::Handle{102u}) &&
      !firstContext->find(buffer.kind, core::Handle{999u}), "resource lookup qualifies both native kind and handle");

  CpuTapeRecordContextStore store({.maxRecords = 2u});
  const Locator drawA{11u, 0u}, drawB{11u, 1u};
  check(store.bind(drawA, firstContext) == Status::Accepted &&
      store.bind(drawB, secondContext) == Status::Accepted && store.size() == 2u,
      "two canonical records retain different epochs even when later grouped into one source");
  check(store.lookup(drawA)->find(buffer.kind, core::Handle{101u})->contentEpoch() == 1u &&
      store.lookup(drawB)->find(buffer.kind, core::Handle{101u})->contentEpoch() == 2u,
      "consumer lookup cannot substitute a latest-at-publication context");
  check(store.bind(drawA, firstContext) == Status::Accepted && store.size() == 2u,
      "same locator and exact context pointer retries even at capacity");
  const std::array identicalEntries{first, unchanged};
  const auto distinctPointer = context(identicalEntries);
  check(store.bind(drawA, distinctPointer) == Status::Conflict &&
      store.bind(drawA, secondContext) == Status::Conflict && store.lookup(drawA) == firstContext,
      "equal values in a different context pointer cannot replace a write-once record binding");
  check(store.bind({11u, 2u}, firstContext) == Status::CapacityExceeded &&
      !store.lookup({11u, 2u}) && store.size() == 2u, "one-over record limit leaves prior bindings intact");
}

void aliasesAndHandleAba() {
  auto alias = version(surface, core::Handle{211u}, 4u, {}, 7u);
  std::array aliasEntries{alias};
  auto aliasContext = context(aliasEntries);
  CpuTapeRecordContextStore store;
  check(store.bind({12u, 3u}, aliasContext) == Status::Accepted, "surviving alias binds before future dispatch");
  aliasEntries = {};
  alias.reset();
  aliasContext.reset();
  const auto retained = store.lookup({12u, 3u})->find(surface.kind, core::Handle{211u});
  check(retained && sameIdentity(retained->identity(), surface) &&
      sameIdentity(retained->observation().identity(), surface) &&
      sameIdentity(retained->observation().contentIdentity(), texture) &&
      retained->observation().aliasSubresource() == 7u &&
      retained->observation().receipts().size() == 1u &&
      retained->observation().receipts()[0].subresource == 7u,
      "context preserves alias and original parent full identities after producer owners retire");

  const std::array<std::uint8_t, 2u> bytes{5u, 6u};
  const auto old = version(buffer, core::Handle{212u}, 1u, bytes);
  auto reusedIdentity = buffer;
  ++reusedIdentity.generation;
  const auto reused = version(reusedIdentity, core::Handle{212u}, 1u, bytes);
  const std::array oldEntries{old};
  const std::array reusedEntries{reused};
  const auto oldContext = context(oldEntries);
  const auto reusedContext = context(reusedEntries);
  check(store.bind({13u, 0u}, oldContext) == Status::Accepted &&
      store.bind({14u, 0u}, reusedContext) == Status::Accepted &&
      store.lookup({13u, 0u})->find(buffer.kind, core::Handle{212u})->identity().generation == buffer.generation &&
      store.lookup({14u, 0u})->find(buffer.kind, core::Handle{212u})->identity().generation == reusedIdentity.generation,
      "ABA native handle reuse remains qualified by each record's full wire generation");
  const std::array duplicateHandle{old, reused};
  auto out = oldContext;
  check(buildCpuTapeResourceContext(duplicateHandle, out) == Status::Conflict && out == oldContext,
      "one context cannot contain two generations for the same native kind/handle");
}

void explicitSurfaceAliasInventoryPurpose() {
  auto owner = budget(16u);
  auto peerIdentity = surface;
  peerIdentity.generation += 3u;
  peerIdentity.objectId += 9u;
  const core::Handle sharedNative{221u};
  const auto first = budgetedVersion(owner, surface, sharedNative, 1u);
  Version peer;
  const auto equalSeed = observation(peerIdentity, 1u);
  check(buildBudgetedCpuTapeResourceVersion({.nativeKind = surface.kind,
      .nativeHandle = sharedNative, .contentEpoch = 9u}, equalSeed, owner, peer) == Status::Accepted,
      "two independently qualified surface wire owners can carry equal seed but different logical epochs");
  const std::array single{first};
  auto strict = context(single);
  check(strict->purpose() == Purpose::SemanticUniqueNative && strict->hasSemanticUniqueNativeAuthority(),
      "unchanged strict factory marks its unique-native construction domain only");
  const std::array aliases{first, peer};
  auto out = strict;
  check(buildCpuTapeResourceContext(aliases, out) == Status::Conflict && out == strict,
      "legacy semantic factory still rejects captured surface aliases sharing native key");
  check(buildCpuTapeV4SurfaceAliasInventory(aliases, owner, out) == Status::Accepted &&
      out->purpose() == Purpose::V4SurfaceAliasInventory && !out->hasSemanticUniqueNativeAuthority() &&
      out->entries().size() == 2u && out->entries()[0] == first && out->entries()[1] == peer &&
      out->findIdentity(surface) == first && out->findIdentity(peerIdentity) == peer &&
      first->contentEpoch() == 1u && peer->contentEpoch() == 9u &&
      first->observation().receipts()[0].digest == peer->observation().receipts()[0].digest,
      "explicit metadata inventory preserves all full wire identities, epochs and equal seed observations without merging");
  check(!out->find(surface.kind, sharedNative), "ambiguous native lookup cannot select first wire owner");
  const std::array reversed{peer, first};
  Context reverse;
  check(buildCpuTapeV4SurfaceAliasInventory(reversed, owner, reverse) == Status::Accepted &&
      !reverse->find(surface.kind, sharedNative) && reverse->findIdentity(surface) == first &&
      reverse->findIdentity(peerIdentity) == peer, "native ambiguity rejection is independent of input order");
  for (unsigned field = 0u; field != 3u; ++field) {
    auto missing = surface;
    if (field == 0u) missing.kind = texture.kind;
    if (field == 1u) ++missing.generation;
    if (field == 2u) ++missing.objectId;
    check(!out->findIdentity(missing), "exact inventory lookup qualifies kind, generation and object ID");
  }
  const auto changedPeer = budgetedVersion(owner, peerIdentity, sharedNative, 10u);
  const std::array changedAliases{first, changedPeer};
  Context changed;
  check(buildCpuTapeV4SurfaceAliasInventory(changedAliases, owner, changed) == Status::Accepted &&
      changed->findIdentity(peerIdentity) == changedPeer && out->findIdentity(peerIdentity) == peer &&
      changedPeer->observation().receipts()[0].revision == 10u && peer->observation().receipts()[0].revision == 1u,
      "independent alias refresh cannot rewrite prior inventory's captured receipt or logical epoch");
  Context unambiguous, empty;
  check(buildCpuTapeV4SurfaceAliasInventory(single, owner, unambiguous) == Status::Accepted &&
      unambiguous->find(surface.kind, sharedNative) == first && !unambiguous->hasSemanticUniqueNativeAuthority() &&
      buildCpuTapeV4SurfaceAliasInventory({}, owner, empty, {.maxEntries = 0u}) == Status::Accepted &&
      empty->entries().empty() && empty->purpose() == Purpose::V4SurfaceAliasInventory &&
      !empty->hasSemanticUniqueNativeAuthority(),
      "alias-free and empty inventory contexts remain metadata-only, never semantic-unique-native authority");
  check(!owner->snapshot().chargedExpectedBufferBytes, "surface alias metadata introduces no expected buffer byte charge");
}

void surfaceAliasInventoryBoundsAndFailures() {
  auto owner = budget(16u);
  auto foreignOwner = budget(16u);
  const auto first = budgetedVersion(owner, surface, core::Handle{231u}, 1u);
  auto peerIdentity = surface;
  ++peerIdentity.objectId;
  const auto peer = budgetedVersion(owner, peerIdentity, first->nativeHandle(), 2u);
  const std::array aliases{first, peer};
  Context out;
  check(buildCpuTapeV4SurfaceAliasInventory(aliases, owner, out, {.maxEntries = 2u}) == Status::Accepted,
      "surface alias inventory admits exact wire-entry metadata bound");
  const auto prior = out;
  const auto reject = [&](auto entries, const Budget& supplied, Status expected) {
    check(buildCpuTapeV4SurfaceAliasInventory(entries, supplied, out) == expected && out == prior,
        "rejected alias inventory factory preserves previous immutable output transactionally");
  };
  reject(aliases, Budget{}, Status::InvalidInput);
  reject(aliases, foreignOwner, Status::InvalidInput);
  const auto foreign = budgetedVersion(foreignOwner, peerIdentity, core::Handle{232u}, 1u);
  reject(std::array{first, foreign}, owner, Status::InvalidInput);
  const auto legacy = version(peerIdentity, core::Handle{232u}, 1u);
  reject(std::array{first, legacy}, owner, Status::InvalidInput);
  reject(std::array<Version, 1u>{}, owner, Status::InvalidInput);
  reject(std::array{first, first}, owner, Status::Conflict);
  const auto duplicateWire = budgetedVersion(owner, surface, core::Handle{233u}, 2u);
  reject(std::array{first, duplicateWire}, owner, Status::Conflict);
  const std::array<std::uint8_t, 1u> bytes{5u};
  auto bufferPeerIdentity = buffer;
  ++bufferPeerIdentity.objectId;
  const auto bufferFirst = budgetedVersion(owner, buffer, core::Handle{234u}, 1u, bytes);
  const auto bufferPeer = budgetedVersion(owner, bufferPeerIdentity, bufferFirst->nativeHandle(), 2u, bytes);
  reject(std::array{bufferFirst, bufferPeer}, owner, Status::Conflict);
  auto texturePeerIdentity = texture;
  ++texturePeerIdentity.objectId;
  const auto textureFirst = budgetedVersion(owner, texture, core::Handle{235u}, 1u);
  const auto texturePeer = budgetedVersion(owner, texturePeerIdentity, textureFirst->nativeHandle(), 2u);
  reject(std::array{textureFirst, texturePeer}, owner, Status::Conflict);
  check(buildCpuTapeV4SurfaceAliasInventory(aliases, owner, out, {.maxEntries = 1u}) == Status::CapacityExceeded &&
      out == prior && buildCpuTapeV4SurfaceAliasInventory(aliases, owner, out,
          {.maxEntries = std::numeric_limits<std::uint32_t>::max()}) == Status::CapacityExceeded && out == prior,
      "surface native sharing neither discounts wire-entry counts nor bypasses metadata limits");
  // Context object, shared control block, immutable entry vector.
  for (std::ptrdiff_t failAfter = 0; failAfter != 3; ++failAfter) {
    Status status;
    {
      AllocationFailure fault(failAfter);
      status = buildCpuTapeV4SurfaceAliasInventory(aliases, owner, out);
    }
    check(status == Status::AllocationFailed && out == prior &&
        owner->snapshot().chargedExpectedBufferBytes == 2u,
        "alias inventory allocation rollback preserves output and existing expected-byte ownership");
  }
  const std::array mixed{first, peer, bufferFirst, textureFirst};
  Context mixedInventory;
  check(buildCpuTapeV4SurfaceAliasInventory(mixed, owner, mixedInventory) == Status::Accepted &&
      !mixedInventory->find(surface.kind, first->nativeHandle()) &&
      mixedInventory->find(buffer.kind, bufferFirst->nativeHandle()) == bufferFirst &&
      mixedInventory->find(texture.kind, textureFirst->nativeHandle()) == textureFirst &&
      !mixedInventory->hasSemanticUniqueNativeAuthority(),
      "unambiguous buffer/texture keys remain available without promoting mixed metadata inventory to semantic authority");
  // The legacy factory's acceptance contract is deliberately unchanged. Its
  // native keys can be unique while a new exact-wire lookup is ambiguous.
  const std::array repeatedWire{first, duplicateWire};
  auto strict = context(repeatedWire);
  check(strict->hasSemanticUniqueNativeAuthority() && !strict->findIdentity(surface) &&
      strict->find(surface.kind, first->nativeHandle()) == first,
      "exact identity lookup also rejects ambiguity in a legacy native-unique context");
}

void factoryBoundsAndInvalidInputs() {
  const std::array<std::uint8_t, 4u> bytes{1u, 2u, 3u, 4u};
  const auto normalized = observation(buffer, 1u);
  CpuTapeResourceVersionInput input{.nativeKind = buffer.kind, .nativeHandle = core::Handle{301u},
      .contentEpoch = 1u, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes};
  Version out;
  check(buildCpuTapeResourceVersion(input, normalized, out, {.maxExpectedBufferBytes = 4u}) == Status::Accepted,
      "exact per-version expected byte budget accepts");
  const auto prior = out;
  check(buildCpuTapeResourceVersion(input, normalized, out, {.maxExpectedBufferBytes = 3u}) == Status::CapacityExceeded &&
      out == prior, "one-over byte budget preserves previous version pointer");
  ++input.expectedBufferByteSize;
  check(buildCpuTapeResourceVersion(input, normalized, out) == Status::InvalidInput && out == prior,
      "an Upload patch cannot masquerade as the declared complete buffer extent");
  --input.expectedBufferByteSize;
  input.contentEpoch = 0u;
  check(buildCpuTapeResourceVersion(input, normalized, out) == Status::InvalidInput && out == prior,
      "zero logical content epoch rejects transactionally");
  input.contentEpoch = 1u;
  input.nativeHandle = {};
  check(buildCpuTapeResourceVersion(input, normalized, out) == Status::InvalidInput && out == prior,
      "null native value key rejects before copying bytes");
  input.nativeHandle = core::Handle{301u};
  input.nativeKind = texture.kind;
  check(buildCpuTapeResourceVersion(input, normalized, out) == Status::InvalidInput && out == prior,
      "native kind cannot differ from the full normalized identity");
  const auto textureObservation = observation(texture, 1u);
  check(buildCpuTapeResourceVersion(input, textureObservation, out) == Status::InvalidInput && out == prior,
      "non-buffer versions cannot retain arbitrary expected byte vectors");
  input.nativeKind = buffer.kind;
  CpuTapeResourceObservation invalid;
  check(buildCpuTapeResourceVersion(input, invalid, out) == Status::InvalidInput && out == prior,
      "unadmitted normalization cannot become an immutable version");

  const auto other = version(texture, core::Handle{301u}, 1u);
  const std::array entries{prior, other};
  Context contextOut;
  check(buildCpuTapeResourceContext(entries, contextOut, {.maxEntries = 2u}) == Status::Accepted,
      "exact entry budget permits equal handles in different kind domains");
  const auto priorContext = contextOut;
  check(buildCpuTapeResourceContext(entries, contextOut, {.maxEntries = 1u}) == Status::CapacityExceeded &&
      contextOut == priorContext, "entry capacity is independent of expected-byte and record limits");
  const std::array<Version, 1u> nullEntry{};
  check(buildCpuTapeResourceContext(nullEntry, contextOut) == Status::InvalidInput && contextOut == priorContext,
      "null version cannot leave a partially filled context");
  check(buildCpuTapeResourceContext(entries, contextOut,
      {.maxEntries = std::numeric_limits<std::uint32_t>::max()}) == Status::CapacityExceeded &&
      contextOut == priorContext, "invalid context index budget preserves prior owner");
  Context empty;
  check(buildCpuTapeResourceContext({}, empty, {.maxEntries = 0u}) == Status::Accepted &&
      empty->entries().empty(), "zero entry budget admits an immutable resource-free context");
  CpuTapeRecordContextStore store({.maxRecords = 0u});
  check(store.bind({15u, 0u}, empty) == Status::CapacityExceeded && !store.size(),
      "zero record budget performs no insertion");
  check(store.bind({0u, 0u}, empty) == Status::InvalidInput &&
      store.bind({15u, std::numeric_limits<std::uint32_t>::max()}, empty) == Status::InvalidInput &&
      store.bind({15u, 0u}, {}) == Status::InvalidInput && !store.lookup({0u, 0u}),
      "invalid locators and absent contexts cannot create a record entry");
  CpuTapeRecordContextStore invalidStore({.maxRecords = std::numeric_limits<std::uint32_t>::max()});
  check(invalidStore.bind({15u, 0u}, empty) == Status::InvalidInput && !invalidStore.size(),
      "unsupported record index budget rejects without allocation");
}

void concurrentCompletePublication() {
  constexpr std::size_t records = 128u;
  CpuTapeRecordContextStore store({.maxRecords = records});
  const auto unchanged = version(texture, core::Handle{402u}, 1u);
  std::atomic<bool> done{false}, okay{true};
  std::atomic<unsigned> readersReady{0u};
  std::atomic<std::size_t> observed{0u};
  const auto complete = [&](const Context& value, std::size_t index) {
    if (!value || value->entries().size() != 2u ||
        value->find(texture.kind, core::Handle{402u}) != unchanged) return false;
    const auto current = value->find(buffer.kind, core::Handle{401u});
    return current && current->contentEpoch() == index + 1u &&
        sameIdentity(current->identity(), buffer) && current->observation().receipts().size() == 1u &&
        current->observation().receipts()[0].revision == index + 1u &&
        current->expectedBufferBytes().size() == 8u &&
        std::all_of(current->expectedBufferBytes().begin(), current->expectedBufferBytes().end(),
            [&](std::uint8_t byte) { return byte == static_cast<std::uint8_t>(index); });
  };
  const auto read = [&] {
    readersReady.fetch_add(1u, std::memory_order_release);
    do {
      for (std::size_t index = 0u; index < records; ++index) {
        const auto value = store.lookup({16u, static_cast<std::uint32_t>(index)});
        if (!value) continue;
        if (!complete(value, index)) okay.store(false, std::memory_order_relaxed);
        observed.fetch_add(1u, std::memory_order_relaxed);
      }
    } while (!done.load(std::memory_order_acquire));
  };
  std::thread firstReader(read), secondReader(read);
  std::thread producer([&] {
    try {
      while (readersReady.load(std::memory_order_acquire) != 2u) std::this_thread::yield();
      for (std::size_t index = 0u; index < records; ++index) {
        const auto epoch = index + 1u;
        std::array receipts{CpuTapeResourceReceipt{.identity = buffer, .revision = epoch}};
        CpuTapeResourceObservation normalized;
        Version current;
        Context value;
        std::vector<std::uint8_t> source(8u, static_cast<std::uint8_t>(index));
        if (buildCpuTapeResourceObservation({.identity = buffer, .contentIdentity = buffer,
                .receipts = receipts}, normalized) != CpuTapeResourceObservationStatus::Accepted ||
            buildCpuTapeResourceVersion({.nativeKind = buffer.kind, .nativeHandle = core::Handle{401u},
                .contentEpoch = epoch, .expectedBufferByteSize = source.size(), .expectedBufferBytes = source},
                normalized, current) != Status::Accepted) {
          okay.store(false, std::memory_order_relaxed);
          break;
        }
        const std::array entries{current, unchanged};
        if (buildCpuTapeResourceContext(entries, value) != Status::Accepted ||
            store.bind({16u, static_cast<std::uint32_t>(index)}, value) != Status::Accepted) {
          okay.store(false, std::memory_order_relaxed);
          break;
        }
        source.assign(17u, 255u);
        receipts[0].revision = 999u;
      }
    } catch (...) {
      okay.store(false, std::memory_order_relaxed);
    }
    done.store(true, std::memory_order_release);
  });
  producer.join();
  firstReader.join();
  secondReader.join();
  check(okay.load() && observed.load() && store.size() == records,
      "concurrent lookups observe absent or fully constructed immutable contexts only");
  for (std::size_t index = 0u; index < records; ++index)
    check(complete(store.lookup({16u, static_cast<std::uint32_t>(index)}), index),
        "all complete record contexts survive producer-local receipt and byte mutation");
}

} // namespace

int main() {
  try {
    recordEpochsAndSharedVersions();
    aliasesAndHandleAba();
    explicitSurfaceAliasInventoryPurpose();
    surfaceAliasInventoryBoundsAndFailures();
    factoryBoundsAndInvalidInputs();
    immutableHistoricalBufferFacts();
    localBufferFactsFactoryValidation();
    sharedSessionBudgetOwnership();
    budgetedBoundsAndReplacement();
    allocationFailureReturnsTentativeCredit();
    exhaustiveBudgetFactoryTransitions();
    concurrentBudgetReservationsAndReleases();
    concurrentCompletePublication();
    std::cout << "cpu resource context checks: " << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cpu resource context failure: " << error.what() << '\n';
    return 1;
  }
}
