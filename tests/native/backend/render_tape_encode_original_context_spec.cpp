#include "render_tape_encode_original_context.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

// Executable-local cold allocation fault injection. No production/provider
// hook, physical Pool byte read, command buffer or GPU execution is involved.
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
using Owner = RenderTapeEncodeOriginalBufferContexts;
using Status = Owner::Status;
using Context = Owner::Context;
using Budget = std::shared_ptr<CpuTapeExpectedBufferBudget>;
using Facts = RenderTapeEncodeOriginalBufferFacts;
using Mutation = d3d9::RenderTapeResourceMutationHeader;
constexpr D9CWireObjectIdentity buffer{D9C_CHUNK_HANDLE_KIND_BUFFER, 7u, 701u};
constexpr core::Handle native{101u};
std::size_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

struct AllocationFailure {
  explicit AllocationFailure(std::ptrdiff_t count) {
    allocationFailureAfter.store(count, std::memory_order_relaxed);
  }
  ~AllocationFailure() { allocationFailureAfter.store(-1, std::memory_order_relaxed); }
};

Budget budget(std::size_t bytes = 64u) {
  Budget out;
  check(createCpuTapeExpectedBufferBudget({bytes}, out) == Status::Accepted,
      "original context fixture uses an explicit shared retained-image budget");
  return out;
}

d3d9::RenderTapeObjectDefineHeader definition(D9CWireObjectIdentity identity = buffer,
    std::uint32_t byteSize = 4u, bool closure = true) {
  return {.identity = identity,
      .descriptorKind = static_cast<std::uint32_t>(d3d9::RenderTapeDescriptorKind::Buffer),
      .descriptorBytes = sizeof(D9CBufferDesc),
      .expectedContentBytes = closure ? byteSize : 0u,
      .expectedContentCount = closure ? 1u : 0u};
}

Status define(Owner& owner, D9CWireObjectIdentity identity = buffer,
    core::Handle handle = native, bool hidden = false, bool closure = true) {
  const D9CBufferDesc descriptor{.size = 4u, .pool = 1u}; // Managed shape only.
  return owner.define(definition(identity, descriptor.size, closure),
      std::as_bytes(std::span{&descriptor, 1u}), handle, hidden);
}

Mutation mutation(std::span<const std::byte> bytes, std::uint64_t offset = 0u,
    D9CWireObjectIdentity identity = buffer,
    d3d9::RenderTapeBufferMutationDisposition disposition = d3d9::RenderTapeBufferMutationDisposition::Plain) {
  return {.identity = identity,
      .kind = static_cast<std::uint32_t>(d3d9::RenderTapeMutationKind::Upload),
      .bufferDisposition = static_cast<std::uint32_t>(disposition),
      .byteOffset = offset, .byteSize = bytes.size(),
      .digest = d3d9::RenderTapeCaptureSession::sha256(bytes)};
}

// Deliberately value-only metadata fixtures: these do not authenticate a real
// Pool observation, drain, native association, source lifetime or bytes. There
// is no physical byte input for the owner to use as its canonical oracle.
Facts facts(D9CWireObjectIdentity identity = buffer, core::Handle handle = native,
    std::uint64_t revision = 9001u) {
  return {.identity = identity, .nativeHandle = handle,
      .observation = {.status = resources::BufferVersionObservationStatus::Valid,
          .buffer = handle, .byteSize = 4u, .contentRevision = revision,
          .hasVersionedBacking = false,
          .currentBacking = {.metalHandle = 8001u, .byteSize = 4u}}};
}

bool sameBytes(const Owner::Version& version, std::span<const std::uint8_t> bytes) {
  return version && std::ranges::equal(version->expectedBufferBytes(), bytes);
}

void hiddenPreparationIsNotLogicalReapplication() {
  auto sharedBudget = budget();
  Owner owner(sharedBudget);
  std::array<std::byte, 4> canonical{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto seed = mutation(canonical);
  check(define(owner, buffer, native, true) == Status::Accepted &&
      owner.find(buffer).hidden && !owner.find(buffer).imageComplete &&
      owner.find(buffer).contentEpoch == 0u && !sharedBudget->snapshot().chargedExpectedBufferBytes,
      "hidden definition owns actual association/extent but never guesses allocator contents");
  check(owner.hiddenSeedApplied(12u, seed, canonical) == Status::Accepted,
      "successful hidden apply derives one owned authenticated original image");
  auto prepared = owner.find(buffer).version;
  check(prepared && prepared->contentEpoch() == 1u && !prepared->localBufferFacts() &&
      prepared->observation().receipts().size() == 1u &&
      prepared->observation().receipts()[0].digest == seed.digest,
      "hidden apply counts original mutation once and leaves physical facts pending");
  canonical.fill(std::byte{0xee});
  const std::array<std::uint8_t, 4> expected{1u, 2u, 3u, 4u};
  check(sameBytes(prepared, expected), "later input-blob edits cannot rewrite authenticated expected image");
  Context hidden;
  check(owner.beforeRecord({20u, 0u}, {}, hidden) == Status::Accepted && hidden->entries().empty(),
      "hidden physical preparation has no logical original-record inventory entry");
  check(owner.activate(buffer, core::Handle{999u}) == Status::Conflict && owner.find(buffer).hidden,
      "activation rejects a native key from a different execution");
  check(owner.activate(buffer, native) == Status::Accepted &&
      owner.find(buffer).version == prepared && owner.find(buffer).contentEpoch == 1u,
      "activation changes visibility, not bytes or canonical epoch");
  Context notConsumed;
  check(owner.beforeRecord({20u, 1u}, {}, notConsumed) == Status::Accepted && notConsumed->entries().empty(),
      "activated-but-unconsumed original seed cannot qualify a record early");
  auto wrongSeed = seed;
  ++wrongSeed.byteOffset;
  check(owner.seedConsumed(12u, wrongSeed) == Status::Conflict &&
      owner.seedConsumed(13u, seed) == Status::Conflict && owner.find(buffer).pendingSeed,
      "consume must match the exact original mutation header and ordinal");
  check(owner.seedConsumed(12u, seed) == Status::Accepted &&
      owner.find(buffer).version == prepared && owner.find(buffer).contentEpoch == 1u &&
      !owner.find(buffer).pendingSeed && owner.seedConsumed(12u, seed) == Status::Conflict,
      "logical consume neither patches bytes twice nor increments epoch or receipt count");
  const std::array local{facts()};
  Context original;
  check(owner.beforeRecord({21u, 0u}, local, original) == Status::Accepted,
      "post-existing-drain copied facts finalize a genuine original record context");
  const auto finalized = original->findIdentity(buffer);
  check(finalized && finalized != prepared && sameBytes(finalized, expected) &&
      finalized->contentEpoch() == 1u && finalized->localBufferFacts()->contentRevision == 9001u &&
      !original->hasSemanticUniqueNativeAuthority() &&
      original->purpose() == CpuTapeResourceContextPurpose::V4SurfaceAliasInventory,
      "physical facts replace only same-epoch metadata and never grant whole semantic authority");
  Context unchanged;
  check(owner.beforeRecord({21u, 1u}, local, unchanged) == Status::Accepted && unchanged == original,
      "unchanged copied facts reuse immutable inventory/version ownership without recopying image");
  const std::array patch{std::byte{9}, std::byte{8}};
  const auto update = mutation(patch, 1u);
  check(owner.mutationApplied(30u, update, patch) == Status::Accepted &&
      owner.find(buffer).contentEpoch == 2u && !owner.find(buffer).version->localBufferFacts(),
      "real ordinary patch advances canonical image and clears pre-drain physical facts");
  const std::array<std::uint8_t, 4> patched{1u, 9u, 8u, 4u};
  const std::array nextFacts{facts(buffer, native, 12001u)};
  Context next;
  check(owner.beforeRecord({31u, 0u}, nextFacts, next) == Status::Accepted &&
      sameBytes(next->findIdentity(buffer), patched) &&
      next->findIdentity(buffer)->contentEpoch() == 2u &&
      next->findIdentity(buffer)->observation().receipts().size() == 2u,
      "patch uses authenticated blob plus prior immutable original image, not physical bytes");
  auto wrongIdentity = buffer;
  ++wrongIdentity.generation;
  const std::array invalidFacts{facts(wrongIdentity, core::Handle{555u})};
  Context retry;
  check(owner.beforeRecord({21u, 0u}, invalidFacts, retry) == Status::Accepted && retry == original &&
      sameBytes(retry->findIdentity(buffer), expected) &&
      retry->findIdentity(buffer)->localBufferFacts()->contentRevision == 9001u,
      "write-once retry selects original pointer before consulting changed producer state or invalid facts");
  check(owner.retire(buffer, native) == Status::Accepted && !owner.find(buffer).version &&
      owner.beforeRecord({21u, 0u}, {}, retry) == Status::Accepted && retry == original,
      "retirement drops current ownership only; consumers retain historical original context");
  Context afterRetire;
  check(owner.beforeRecord({32u, 0u}, {}, afterRetire) == Status::Accepted && afterRetire->entries().empty(),
      "new records after retirement cannot find historical native buffer entries");
}

void unknownSeedsAndDiscardFailClosed() {
  Owner owner(budget());
  check(define(owner, buffer, native, false, false) == Status::Accepted,
      "non-closure definition remains an unknown canonical image");
  Context missing;
  const std::array local{facts()};
  check(owner.beforeRecord({40u, 0u}, local, missing) == Status::Accepted &&
      missing->entries().empty() && !owner.find(buffer).imageComplete,
      "even valid-shaped physical metadata cannot manufacture a missing initial seed");
  const std::array half{std::byte{1}, std::byte{2}};
  const auto firstHalf = mutation(half);
  const auto secondHalf = mutation(half, 2u);
  check(owner.mutationApplied(41u, firstHalf, half) == Status::Accepted &&
      owner.mutationApplied(42u, secondHalf, half) == Status::Accepted &&
      !owner.find(buffer).version && owner.find(buffer).contentEpoch == 2u,
      "partial initial patches never treat undefined remainder as zero or infer full closure");
  Context partial;
  check(owner.beforeRecord({43u, 0u}, local, partial) == Status::Accepted && partial->entries().empty(),
      "partial initial seed is absent from consumer inventory and fails later resource lookup closed");
  const std::array full{std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  const auto complete = mutation(full);
  check(owner.mutationApplied(44u, complete, full) == Status::Accepted &&
      owner.find(buffer).imageComplete && owner.find(buffer).contentEpoch == 3u,
      "later full authenticated mutation establishes a complete canonical image without native reads");
  Context qualified;
  check(owner.beforeRecord({45u, 0u}, local, qualified) == Status::Accepted &&
      qualified->findIdentity(buffer)->observation().receipts().size() == 3u,
      "complete image preserves original normalized mutation history");
  const std::array discardBytes{std::byte{99}};
  const auto discard = mutation(discardBytes, 1u, buffer, d3d9::RenderTapeBufferMutationDisposition::Discard);
  check(owner.mutationApplied(46u, discard, discardBytes) == Status::Accepted &&
      !owner.find(buffer).imageComplete && owner.find(buffer).contentEpoch == 4u,
      "partial DISCARD invalidates unspecified remainder rather than retaining old canonical bytes");
  Context discarded;
  check(owner.beforeRecord({47u, 0u}, local, discarded) == Status::Accepted && discarded->entries().empty() &&
      qualified->findIdentity(buffer)->expectedBufferBytes()[0] == 5u,
      "discarded new inventory fails closed while old immutable original context remains intact");
  const auto noOverwrite = mutation(discardBytes, 1u, buffer,
      d3d9::RenderTapeBufferMutationDisposition::NoOverwrite);
  check(owner.mutationApplied(48u, noOverwrite, discardBytes) == Status::Accepted &&
      !owner.find(buffer).imageComplete,
      "NOOVERWRITE cannot recover unknown bytes after a partial discard");
  const auto fullDiscard = mutation(full, 0u, buffer, d3d9::RenderTapeBufferMutationDisposition::Discard);
  check(owner.mutationApplied(49u, fullDiscard, full) == Status::Accepted &&
      owner.find(buffer).contentEpoch == 6u && owner.find(buffer).imageComplete,
      "complete DISCARD can establish a fresh fully authenticated image");
}

void associationGenerationAndInputRejections() {
  auto sharedBudget = budget();
  Owner owner(sharedBudget, {.maxBuffers = 3u, .maxRecordContexts = 8u});
  check(define(owner) == Status::Accepted, "association rejection fixture defines one actual buffer");
  const std::array blob{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto seed = mutation(blob);
  check(owner.mutationApplied(50u, seed, blob) == Status::Accepted, "association fixture has one canonical seed");
  Context prior;
  const std::array local{facts()};
  check(owner.beforeRecord({51u, 0u}, local, prior) == Status::Accepted,
      "association fixture retains one original physical-metadata context");
  const auto version = owner.find(buffer).version;
  const auto retained = sharedBudget->snapshot().chargedExpectedBufferBytes;
  const auto preserved = [&] {
    check(owner.find(buffer).version == version && owner.find(buffer).contentEpoch == 1u &&
        sharedBudget->snapshot().chargedExpectedBufferBytes == retained && owner.recordCount() == 1u,
        "failed input/association update preserves canonical image, epoch, byte credit and original bindings");
  };
  for (unsigned invalid = 0u; invalid != 8u; ++invalid) {
    auto bad = seed;
    if (invalid == 0u) ++bad.identity.generation;
    if (invalid == 1u) bad.kind = static_cast<std::uint32_t>(d3d9::RenderTapeMutationKind::Palette);
    if (invalid == 2u) bad.subresource = 1u;
    if (invalid == 3u) bad.bufferDisposition = 99u;
    if (invalid == 4u) bad.byteOffset = std::numeric_limits<std::uint64_t>::max();
    if (invalid == 5u) --bad.byteSize;
    if (invalid == 6u) bad.digest[0] ^= std::byte{1};
    if (invalid == 7u) bad.byteSize = 0u;
    check(owner.mutationApplied(52u, bad, blob) == Status::InvalidInput,
        "mutations recheck full identity, disposition, range, extent and full blob digest");
    preserved();
  }
  check(owner.mutationApplied(50u, seed, blob) == Status::Conflict &&
      owner.mutationApplied(0u, seed, blob) == Status::InvalidInput,
      "duplicate/stale original mutation ordinal cannot double-apply or increment epoch");
  for (unsigned invalid = 0u; invalid != 8u; ++invalid) {
    auto bad = facts();
    if (invalid == 0u) ++bad.identity.generation;
    if (invalid == 1u) bad.nativeHandle = core::Handle{999u};
    if (invalid == 2u) bad.observation.buffer = core::Handle{999u};
    if (invalid == 3u) ++bad.observation.byteSize;
    if (invalid == 4u) bad.observation.contentRevision = 0u;
    if (invalid == 5u) bad.observation.currentBacking.metalHandle = 0u;
    if (invalid == 6u) ++bad.observation.currentBacking.byteSize;
    if (invalid == 7u) bad.observation.status = resources::BufferVersionObservationStatus::MissingBuffer;
    const std::array inputs{bad};
    Context out = prior;
    check(owner.beforeRecord({53u, invalid}, inputs, out) == Status::InvalidInput && out == prior,
        "same-epoch fact finalization rejects stale native/wire association or unavailable physical metadata");
    preserved();
  }
  const std::array duplicates{facts(), facts()};
  Context out = prior;
  check(owner.beforeRecord({54u, 0u}, duplicates, out) == Status::Conflict && out == prior,
      "duplicate physical fact association has no partial context bind");
  preserved();
  auto nextIdentity = buffer;
  ++nextIdentity.generation;
  auto otherIdentity = buffer;
  ++otherIdentity.objectId;
  check(define(owner, nextIdentity, core::Handle{102u}) == Status::Conflict &&
      define(owner, otherIdentity, native) == Status::Conflict &&
      owner.retire(buffer, core::Handle{999u}) == Status::Conflict,
      "active wire slot and actual native logical key cannot gain conflicting owners");
  preserved();
  check(owner.retire(buffer, native) == Status::Accepted &&
      define(owner, buffer, native) == Status::Conflict &&
      define(owner, nextIdentity, native) == Status::Accepted,
      "retired identity cannot resurrect, but a later captured generation can use a released native key");
  check(!owner.lookupRecord({51u, 0u})->findIdentity(nextIdentity) &&
      owner.lookupRecord({51u, 0u})->findIdentity(buffer) == version,
      "old consumer history keeps full wire generation and is not rebound by native key reuse");
  const D9CBufferDesc descriptor{.size = 4u};
  auto badDefinition = definition(otherIdentity);
  badDefinition.expectedContentBytes = 3u;
  check(owner.define(badDefinition, std::as_bytes(std::span{&descriptor, 1u}), core::Handle{103u}) ==
      Status::InvalidInput, "canonical definition extent must agree with complete descriptor extent");
  badDefinition = definition(otherIdentity);
  badDefinition.descriptorKind = 0u;
  check(owner.define(badDefinition, std::as_bytes(std::span{&descriptor, 1u}), core::Handle{103u}) ==
      Status::InvalidInput, "buffer descriptor tag is independent of wire identity kind");
  check(define(owner, otherIdentity, core::Handle{}) == Status::InvalidInput,
      "missing actual native association cannot enter producer table");
  Owner invalidOwner(sharedBudget, {.maxBuffers = 0u});
  check(define(invalidOwner) == Status::InvalidInput,
      "zero metadata capacity does not silently expand or authorize unbounded producer state");
}

void boundedScratchBudgetAndTwoPhaseRollback() {
  const std::array blob{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto seed = mutation(blob);
  auto tightBudget = budget(4u);
  Owner tight(tightBudget);
  check(define(tight) == Status::Accepted && tight.mutationApplied(60u, seed, blob) == Status::Accepted,
      "tight session admits exactly one canonical retained image");
  const auto first = tight.find(buffer).version;
  const std::array local{facts()};
  Context none;
  check(tight.beforeRecord({61u, 0u}, local, none) == Status::CapacityExceeded && !none &&
      tight.find(buffer).version == first && !first->localBufferFacts() && !tight.recordCount() &&
      tightBudget->snapshot().chargedExpectedBufferBytes == 4u,
      "same-epoch physical clone must fit alongside old immutable version, never autoexpand budget");
  check(tight.mutationApplied(62u, seed, blob) == Status::CapacityExceeded &&
      tight.find(buffer).version == first && tight.find(buffer).contentEpoch == 1u,
      "mutation budget rejection preserves current authenticated image and original epoch");
  Context canonicalOnly;
  check(tight.beforeRecord({61u, 0u}, {}, canonicalOnly) == Status::Accepted &&
      canonicalOnly->findIdentity(buffer) == first && !first->localBufferFacts(),
      "logical canonical inventory without physical facts grants no physical proof");

  Owner scratch(budget(), {.maxScratchBufferBytes = 1u});
  check(define(scratch) == Status::Accepted && scratch.mutationApplied(63u, seed, blob) == Status::Accepted,
      "complete seed uses no temporary complete patch scratch");
  const auto scratchFirst = scratch.find(buffer).version;
  const std::array patch{std::byte{9}};
  const auto partial = mutation(patch, 1u);
  check(scratch.mutationApplied(64u, partial, patch) == Status::CapacityExceeded &&
      scratch.find(buffer).version == scratchFirst && scratch.find(buffer).contentEpoch == 1u,
      "bounded scratch rejects a large complete-image patch copy transactionally");
  const auto discard = mutation(patch, 1u, buffer, d3d9::RenderTapeBufferMutationDisposition::Discard);
  check(scratch.mutationApplied(65u, discard, patch) == Status::Accepted && !scratch.find(buffer).version,
      "partial discard invalidates unknown bytes without allocating complete scratch");

  auto doubleBudget = budget(12u);
  Owner two(doubleBudget);
  auto secondIdentity = buffer;
  ++secondIdentity.objectId;
  constexpr core::Handle secondNative{102u};
  const auto secondSeed = mutation(blob, 0u, secondIdentity);
  check(define(two) == Status::Accepted && define(two, secondIdentity, secondNative) == Status::Accepted &&
      two.mutationApplied(66u, seed, blob) == Status::Accepted &&
      two.mutationApplied(67u, secondSeed, blob) == Status::Accepted,
      "two-buffer rollback fixture owns two distinct canonical/native identities");
  const auto firstBefore = two.find(buffer).version;
  const auto secondBefore = two.find(secondIdentity).version;
  const std::array bothFacts{facts(), facts(secondIdentity, secondNative)};
  Context output;
  check(two.beforeRecord({68u, 0u}, bothFacts, output) == Status::CapacityExceeded && !output &&
      two.find(buffer).version == firstBefore && two.find(secondIdentity).version == secondBefore &&
      !firstBefore->localBufferFacts() && !secondBefore->localBufferFacts() && !two.recordCount() &&
      doubleBudget->snapshot().chargedExpectedBufferBytes == 8u &&
      doubleBudget->snapshot().highWaterExpectedBufferBytes == 12u,
      "later candidate failure rolls back earlier tentative physical clone without partial producer/context commit");

  Owner capped(budget(), {.maxBuffers = 1u, .maxRecordContexts = 1u, .maxReceiptsPerBuffer = 1u});
  check(define(capped) == Status::Accepted && capped.mutationApplied(69u, seed, blob) == Status::Accepted,
      "bounded metadata fixture admits only its explicit capacities");
  Context record;
  check(capped.beforeRecord({70u, 0u}, {}, record) == Status::Accepted &&
      capped.beforeRecord({71u, 0u}, {}, output) == Status::CapacityExceeded &&
      capped.mutationApplied(72u, seed, blob) == Status::CapacityExceeded &&
      capped.retire(buffer, native) == Status::Accepted &&
      define(capped, secondIdentity, secondNative) == Status::CapacityExceeded,
      "record, receipt and lifetime-tombstone metadata capacities remain explicit across retirement");
}

void allocationFailuresPreserveOriginalStateAndCredit() {
  const std::array blob{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto seed = mutation(blob);
  const std::array local{facts()};
  std::size_t factFailures = 0u, factSuccesses = 0u;
  bool sawTentativeFactCredit = false;
  for (std::ptrdiff_t failAfter = 0; failAfter != 20; ++failAfter) {
    auto sharedBudget = budget();
    Owner owner(sharedBudget);
    check(define(owner) == Status::Accepted && owner.mutationApplied(80u, seed, blob) == Status::Accepted,
        "fact allocation fixture owns one authenticated canonical image");
    Context original;
    check(owner.beforeRecord({81u, 0u}, {}, original) == Status::Accepted,
        "fact allocation fixture retains an original write-once context");
    const auto priorVersion = owner.find(buffer).version;
    Context out = original;
    Status status;
    {
      AllocationFailure fail(failAfter);
      status = owner.beforeRecord({82u, 0u}, local, out);
    }
    if (status == Status::AllocationFailed) {
      ++factFailures;
      const auto snapshot = sharedBudget->snapshot();
      sawTentativeFactCredit |= snapshot.highWaterExpectedBufferBytes > 4u;
      check(out == original && owner.find(buffer).version == priorVersion &&
          !priorVersion->localBufferFacts() && owner.find(buffer).contentEpoch == 1u &&
          owner.recordCount() == 1u && snapshot.chargedExpectedBufferBytes == 4u,
          "every allocation failure before bind unwinds tentative facts/image credit and preserves producer/output/history");
    } else {
      ++factSuccesses;
      check(status == Status::Accepted && out != original && owner.recordCount() == 2u &&
          owner.find(buffer).contentEpoch == 1u && owner.find(buffer).version->localBufferFacts() &&
          owner.lookupRecord({81u, 0u}) == original && !priorVersion->localBufferFacts() &&
          sharedBudget->snapshot().chargedExpectedBufferBytes == 8u,
          "successful finalization commits same-epoch facts while keeping old original version independent");
    }
  }
  check(factFailures >= 6u && factSuccesses && sawTentativeFactCredit,
      "fault sweep covers both pre/post-reservation rollback and eventual complete same-epoch commit");

  const std::array patch{std::byte{9}};
  const auto partial = mutation(patch, 1u);
  std::size_t mutationFailures = 0u, mutationSuccesses = 0u;
  for (std::ptrdiff_t failAfter = 0; failAfter != 20; ++failAfter) {
    auto sharedBudget = budget();
    Owner owner(sharedBudget);
    check(define(owner) == Status::Accepted && owner.mutationApplied(83u, seed, blob) == Status::Accepted,
        "patch allocation fixture has one complete original image");
    const auto prior = owner.find(buffer).version;
    Status status;
    {
      AllocationFailure fail(failAfter);
      status = owner.mutationApplied(84u, partial, patch);
    }
    if (status == Status::AllocationFailed) {
      ++mutationFailures;
      check(owner.find(buffer).version == prior && owner.find(buffer).contentEpoch == 1u &&
          sharedBudget->snapshot().chargedExpectedBufferBytes == 4u,
          "patch scratch/receipt/version allocation rollback preserves canonical image and byte credit");
    } else {
      ++mutationSuccesses;
      const std::array<std::uint8_t, 4> expected{1u, 9u, 3u, 4u};
      check(status == Status::Accepted && owner.find(buffer).contentEpoch == 2u &&
          sameBytes(owner.find(buffer).version, expected) &&
          sharedBudget->snapshot().chargedExpectedBufferBytes == 8u,
          "patch eventually commits only its copied canonical image, never rewrites external old version");
    }
  }
  check(mutationFailures >= 6u && mutationSuccesses,
      "fault sweep exercises bounded patch scratch and budgeted immutable allocation stages");

  auto sharedBudget = budget();
  Owner owner(sharedBudget);
  Status status;
  {
    AllocationFailure fail(0);
    status = define(owner);
  }
  check(status == Status::AllocationFailed && !owner.find(buffer).nativeHandle &&
      define(owner) == Status::Accepted && !sharedBudget->snapshot().chargedExpectedBufferBytes,
      "failed definition allocation does not leave a tombstone, native association or image charge");
}

void historyCanOutliveProducerFacade() {
  auto sharedBudget = budget();
  Context retained;
  {
    Owner owner(sharedBudget);
    const std::array blob{std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}};
    const auto seed = mutation(blob);
    const std::array local{facts()};
    check(define(owner) == Status::Accepted && owner.mutationApplied(90u, seed, blob) == Status::Accepted &&
        owner.beforeRecord({91u, 0u}, local, retained) == Status::Accepted &&
        owner.retire(buffer, native) == Status::Accepted,
        "consumer holds original immutable context after logical resource retirement");
  }
  check(retained->findIdentity(buffer)->expectedBufferBytes()[0] == 3u &&
      retained->findIdentity(buffer)->localBufferFacts()->contentRevision == 9001u &&
      sharedBudget->snapshot().chargedExpectedBufferBytes == 4u,
      "owned immutable expected image/facts survive producer/store destruction without any wrapper pointer");
  retained.reset();
  check(!sharedBudget->snapshot().chargedExpectedBufferBytes,
      "last consumer image reference returns exact retained expected-byte budget credit");
}

} // namespace

int main() {
  try {
    hiddenPreparationIsNotLogicalReapplication();
    unknownSeedsAndDiscardFailClosed();
    associationGenerationAndInputRejections();
    boundedScratchBudgetAndTwoPhaseRollback();
    allocationFailuresPreserveOriginalStateAndCredit();
    historyCanOutliveProducerFacade();
    std::cout << "original BUFFER contexts: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "original BUFFER contexts: " << error.what() << '\n';
    return 1;
  }
}
