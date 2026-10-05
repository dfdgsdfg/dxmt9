#include "render_tape_cpu_producer_resource_ledger_v4.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace dxmt9;
using namespace dxmt9::test;
using Ledger = CpuTapeProducerResourceLedgerV4;
using Status = CpuTapeResourceContextStatus;
using Version = Ledger::Version;
using Context = Ledger::Context;
using Budget = std::shared_ptr<CpuTapeExpectedBufferBudget>;
using Facts = resources::BufferVersionObservation;
using Purpose = CpuTapeResourceContextPurpose;
constexpr D9CWireObjectIdentity buffer{D9C_CHUNK_HANDLE_KIND_BUFFER, 7u, 701u};
constexpr D9CWireObjectIdentity texture{D9C_CHUNK_HANDLE_KIND_TEXTURE, 8u, 702u};
constexpr D9CWireObjectIdentity surface{D9C_CHUNK_HANDLE_KIND_SURFACE, 9u, 703u};
std::size_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

Budget budget(std::size_t bytes = 64u) {
  Budget out;
  check(createCpuTapeExpectedBufferBudget({bytes}, out) == Status::Accepted, "explicit ledger session budget is admitted");
  return out;
}

CpuTapeResourceObservation observation(D9CWireObjectIdentity identity, std::uint64_t receiptRevision,
    std::optional<std::uint32_t> alias = std::nullopt) {
  const auto content = alias ? texture : identity;
  const std::array receipts{CpuTapeResourceReceipt{.identity = content,
      .subresource = alias.value_or(0u), .revision = receiptRevision}};
  CpuTapeResourceObservation out;
  check(buildCpuTapeResourceObservation({.identity = identity, .contentIdentity = content,
      .aliasSubresource = alias, .receipts = receipts}, out) == CpuTapeResourceObservationStatus::Accepted,
      "producer normalization owns full qualified resource/alias receipts");
  return out;
}

Version version(const Budget& owner, D9CWireObjectIdentity identity, core::Handle handle,
    std::uint64_t epoch, std::span<const std::uint8_t> bytes = {}, std::optional<std::uint32_t> alias = std::nullopt,
    std::optional<Facts> local = std::nullopt) {
  const auto normalized = observation(identity, epoch, alias);
  Version out;
  check(buildBudgetedCpuTapeResourceVersion({.nativeKind = identity.kind, .nativeHandle = handle,
      .contentEpoch = epoch, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes,
      .localBufferFacts = local},
      normalized, owner, out) == Status::Accepted, "producer version uses exact session budget and full copied image");
  return out;
}

// Pure copied fixture facts only: these tests check inventory qualification,
// not Pool observation authenticity, backing retention or physical contents.
Facts facts(core::Handle handle, std::size_t bytes, std::uint64_t revision) {
  return {.status = resources::BufferVersionObservationStatus::Valid, .buffer = handle,
      .byteSize = bytes, .contentRevision = revision, .hasVersionedBacking = true,
      .currentBacking = {.metalHandle = 901u, .byteSize = bytes}};
}

void immutableInventoriesAndOriginalRecordRetries() {
  auto owner = budget();
  Context consumer;
  {
    Ledger ledger(owner, {.maxCurrentEntries = 3u, .maxRecordContexts = 3u});
    const std::array<std::uint8_t, 4> initialBytes{1u, 2u, 3u, 4u};
    auto first = version(owner, buffer, core::Handle{101u}, 1u, initialBytes, std::nullopt,
        facts(core::Handle{101u}, initialBytes.size(), 51u));
    auto unchanged = version(owner, texture, core::Handle{102u}, 1u);
    auto alias = version(owner, surface, core::Handle{103u}, 1u, {}, 7u);
    check(ledger.define(first) == Status::Accepted &&
        ledger.define(unchanged) == Status::Accepted && ledger.define(alias) == Status::Accepted,
        "successful definitions install one current qualified entry each");
    check(ledger.find(buffer).version == first && ledger.find(buffer).localBufferFacts->contentRevision == 51u &&
        first->contentEpoch() == 1u, "logical epoch and local physical revision remain distinct copied facts");
    Context inventory, sameInventory, original;
    check(ledger.freezeCurrent(inventory) == Status::Accepted && ledger.freezeCurrent(sameInventory) == Status::Accepted &&
        inventory == sameInventory && inventory->entries().size() == 3u &&
        owner->snapshot().chargedExpectedBufferBytes == 4u,
        "unchanged full inventories share immutable pointer and versions without recopying expected bytes");
    check(ledger.bindRecord({11u, 0u}, inventory, original) == Status::Accepted && original == inventory &&
        ledger.recordContexts().lookup({11u, 0u}) == original,
        "producer binds immutable inventory before forwarding original record callback");
    const std::array<std::uint8_t, 4> changedBytes{9u, 2u, 3u, 4u};
    auto next = version(owner, buffer, core::Handle{101u}, 2u, changedBytes, std::nullopt,
        facts(core::Handle{101u}, changedBytes.size(), 77u));
    check(ledger.replace(buffer, 1u, next) == Status::Accepted &&
        ledger.find(buffer).version == next && ledger.find(buffer).localBufferFacts->contentRevision == 77u,
        "successful mutation replaces current epoch without equating it with local backing revision");
    Context changed, nextRecord;
    check(ledger.freezeCurrent(changed) == Status::Accepted && changed != original &&
        changed->find(texture.kind, core::Handle{102u}) == unchanged &&
        ledger.bindRecord({11u, 1u}, nextRecord) == Status::Accepted && nextRecord == changed,
        "changed inventory shares unchanged versions and binds its own original record");
    Context retry;
    check(ledger.bindRecord({11u, 0u}, changed, retry) == Status::Accepted && retry == original &&
        ledger.bindRecord({11u, 0u}, retry) == Status::Accepted && retry == original &&
        retry->find(buffer.kind, core::Handle{101u})->expectedBufferBytes()[0] == 1u &&
        nextRecord->find(buffer.kind, core::Handle{101u})->expectedBufferBytes()[0] == 9u &&
        retry->findIdentity(buffer)->localBufferFacts()->contentRevision == 51u &&
        nextRecord->findIdentity(buffer)->localBufferFacts()->contentRevision == 77u,
        "same-locator retry retains original bytes and instantaneous facts after producer mutation");
    check(ledger.destroy(buffer, 2u) == Status::Accepted && !ledger.find(buffer).version &&
        ledger.currentSize() == 2u && ledger.bindRecord({11u, 0u}, retry) == Status::Accepted && retry == original,
        "producer destruction drops only current entry, never original record ownership");
    check(retry->findIdentity(buffer)->localBufferFacts()->contentRevision == 51u &&
        nextRecord->findIdentity(buffer)->localBufferFacts()->contentRevision == 77u,
        "record-bound immutable versions retain their independently captured facts after producer destruction");
    check(ledger.destroy(texture, 1u) == Status::Accepted && ledger.find(surface).version == alias &&
        original->find(surface.kind, core::Handle{103u})->observation().contentIdentity().objectId == texture.objectId &&
        alias->observation().aliasSubresource() == 7u,
        "alias retains original parent's qualified receipts without ledger wrapper dereferences");
    consumer = original;
    first.reset();
    next.reset();
    check(owner->snapshot().chargedExpectedBufferBytes == 8u,
        "record-bound old/current images retain their exact byte charges after producer entry removal");
  }
  check(owner->snapshot().chargedExpectedBufferBytes == 4u && consumer->entries().size() == 3u &&
      consumer->findIdentity(buffer)->localBufferFacts()->contentRevision == 51u,
      "consumer context survives ledger/store destruction with original instantaneous facts and only its own image charged");
  consumer.reset();
  check(!owner->snapshot().chargedExpectedBufferBytes, "last immutable context owner releases final expected image credit");
}

void fullIdentityEpochAndLocalFactFailures() {
  auto owner = budget();
  Ledger ledger(owner, {.maxCurrentEntries = 1u, .maxRecordContexts = 2u});
  const std::array<std::uint8_t, 4> bytes{1u, 2u, 3u, 4u};
  const auto first = version(owner, buffer, core::Handle{201u}, 1u, bytes, std::nullopt,
      facts(core::Handle{201u}, bytes.size(), 11u));
  check(ledger.define(first) == Status::Accepted,
      "negative transition fixture installs one current version");
  Context original;
  check(ledger.freezeCurrent(original) == Status::Accepted, "negative transition fixture has stable cached inventory");
  const auto assertPreserved = [&] {
    Context current;
    check(ledger.find(buffer).version == first && ledger.find(buffer).localBufferFacts->contentRevision == 11u &&
        ledger.currentSize() == 1u && ledger.freezeCurrent(current) == Status::Accepted && current == original,
        "failed producer transition preserves current entry/local facts and cached immutable pointer");
  };
  const auto next = version(owner, buffer, core::Handle{201u}, 2u, bytes, std::nullopt,
      facts(core::Handle{201u}, bytes.size(), 12u));
  const auto movedHandle = version(owner, buffer, core::Handle{202u}, 2u, bytes);
  auto staleIdentity = buffer;
  ++staleIdentity.generation;
  const auto stale = version(owner, staleIdentity, core::Handle{201u}, 2u, bytes);
  const auto usage = owner->snapshot().chargedExpectedBufferBytes;
  check(ledger.define(first) == Status::Conflict && ledger.define(stale) == Status::Conflict,
      "duplicate full identity or occupied native key cannot be defined twice");
  check(ledger.replace(buffer, 0u, next) == Status::InvalidInput &&
      ledger.replace(buffer, 2u, next) == Status::Conflict && ledger.replace(buffer, 1u, first) == Status::Conflict &&
      ledger.replace(buffer, 1u, movedHandle) == Status::Conflict &&
      ledger.replace(staleIdentity, 1u, stale) == Status::InvalidInput &&
      ledger.replace(staleIdentity, 1u, next) == Status::InvalidInput,
      "replacement validates exact captured/native identity, prior epoch and strict logical progression");
  check(ledger.destroy(buffer, 0u) == Status::InvalidInput && ledger.destroy(buffer, 2u) == Status::Conflict &&
      ledger.destroy(staleIdentity, 1u) == Status::InvalidInput,
      "destroy validates the exact currently live identity and epoch");
  assertPreserved();
  for (unsigned invalid = 0u; invalid != 6u; ++invalid) {
    auto local = facts(next->nativeHandle(), bytes.size(), 12u);
    if (invalid == 0u) local.status = resources::BufferVersionObservationStatus::MissingBuffer;
    if (invalid == 1u) local.buffer = core::Handle{999u};
    if (invalid == 2u) ++local.byteSize;
    if (invalid == 3u) local.contentRevision = 0u;
    if (invalid == 4u) local.currentBacking.metalHandle = 0u;
    if (invalid == 5u) ++local.currentBacking.byteSize;
    const auto normalized = observation(buffer, 2u);
    auto rejected = next;
    check(buildBudgetedCpuTapeResourceVersion({.nativeKind = buffer.kind, .nativeHandle = next->nativeHandle(),
        .contentEpoch = 2u, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes,
        .localBufferFacts = local}, normalized, owner, rejected) == Status::InvalidInput && rejected == next,
        "factory rejects malformed local observations before they can enter the immutable ledger");
    assertPreserved();
  }
  auto projected = ledger.find(buffer);
  projected.localBufferFacts->contentRevision = 999u;
  check(first->localBufferFacts()->contentRevision == 11u &&
      ledger.find(buffer).localBufferFacts->contentRevision == 11u &&
      original->findIdentity(buffer)->localBufferFacts()->contentRevision == 11u,
      "producer compatibility result is a value projection, never separately mutable ledger/context facts");
  const auto other = version(owner, texture, core::Handle{203u}, 1u);
  check(ledger.define(other) == Status::CapacityExceeded && ledger.currentSize() == 1u &&
      owner->snapshot().chargedExpectedBufferBytes == usage,
      "metadata capacity rejection neither retains candidate nor changes byte ownership");
  const auto absent = version(owner, buffer, core::Handle{201u}, 2u, bytes);
  check(ledger.replace(buffer, 1u, absent) == Status::Accepted && !ledger.find(buffer).localBufferFacts &&
      !ledger.find(buffer).version->localBufferFacts(),
      "absent immutable observations remain absent without copying prior current-table history");
  check(ledger.destroy(buffer, 2u) == Status::Accepted && ledger.define(stale) == Status::Accepted &&
      ledger.find(staleIdentity).version == stale, "real generation reuse can occupy a retired native key independently");
}

void aliasRefreshRemainsProducerOwned() {
  auto owner = budget();
  Ledger ledger(owner, {.maxCurrentEntries = 2u, .maxRecordContexts = 2u});
  const auto parent = version(owner, texture, core::Handle{251u}, 1u);
  const auto firstAlias = version(owner, surface, core::Handle{252u}, 1u, {}, 7u);
  check(ledger.define(parent) == Status::Accepted && ledger.define(firstAlias) == Status::Accepted,
      "producer installs qualified parent and explicit alias normalization");
  Context before;
  check(ledger.bindRecord({16u, 0u}, before) == Status::Accepted, "original context owns parent's first alias receipt");
  const auto changedParent = version(owner, texture, core::Handle{251u}, 2u);
  check(ledger.replace(texture, 1u, changedParent) == Status::Accepted &&
      ledger.find(surface).version == firstAlias && firstAlias->observation().receipts()[0].revision == 1u,
      "ledger never dereferences alias parents or invents dependent receipt refresh");
  const auto refreshedAlias = version(owner, surface, core::Handle{252u}, 2u, {}, 7u);
  check(ledger.replace(surface, 1u, refreshedAlias) == Status::Accepted,
      "executor explicitly refreshes dependent alias after authentic parent mutation");
  Context after;
  check(ledger.bindRecord({16u, 1u}, after) == Status::Accepted &&
      before->find(surface.kind, core::Handle{252u})->observation().receipts()[0].revision == 1u &&
      after->find(surface.kind, core::Handle{252u})->observation().receipts()[0].revision == 2u &&
      after->find(surface.kind, core::Handle{252u})->observation().contentIdentity().generation == texture.generation,
      "old and refreshed alias receipts remain original-context owned and fully qualified");
  check(ledger.destroy(texture, 2u) == Status::Accepted && ledger.find(surface).version == refreshedAlias &&
      after->find(surface.kind, core::Handle{252u})->observation().aliasSubresource() == 7u,
      "retiring parent wrapper cannot discard surviving alias's immutable content attribution");
}

void explicitSurfaceAliasInventoryMode() {
  auto owner = budget();
  auto peerIdentity = surface;
  peerIdentity.generation += 3u;
  peerIdentity.objectId += 9u;
  const auto first = version(owner, surface, core::Handle{261u}, 1u);
  const auto peer = version(owner, peerIdentity, first->nativeHandle(), 1u);
  Ledger strict(owner, {.maxCurrentEntries = 2u, .maxRecordContexts = 2u});
  check(strict.define(first) == Status::Accepted && strict.define(peer) == Status::Conflict &&
      strict.currentSize() == 1u, "default ledger still rejects distinct surface wire owners sharing native key");
  Context semantic;
  check(strict.bindRecord({17u, 0u}, semantic) == Status::Accepted &&
      semantic->hasSemanticUniqueNativeAuthority(), "default ledger freezes only strict semantic-unique-native purpose");

  Ledger inventory(owner, {.maxCurrentEntries = 2u, .maxRecordContexts = 3u}, Purpose::V4SurfaceAliasInventory);
  check(inventory.define(first) == Status::Accepted && inventory.define(peer) == Status::Accepted &&
      inventory.currentSize() == 2u && inventory.find(surface).version == first &&
      inventory.find(peerIdentity).version == peer,
      "explicit metadata ledger retains distinct complete surface wire identities without native-key merging");
  Context original, sameInventory;
  check(inventory.bindRecord({18u, 0u}, semantic, original) == Status::InvalidInput && !original &&
      !inventory.recordCount(), "metadata ledger cannot first-publish prepared semantic-purpose context");
  check(inventory.bindRecord({18u, 0u}, original) == Status::Accepted &&
      inventory.freezeCurrent(sameInventory) == Status::Accepted && sameInventory == original &&
      original->purpose() == Purpose::V4SurfaceAliasInventory && !original->hasSemanticUniqueNativeAuthority() &&
      original->entries().size() == 2u && original->findIdentity(surface) == first &&
      original->findIdentity(peerIdentity) == peer && !original->find(surface.kind, first->nativeHandle()),
      "original metadata record owns immutable alias inventory and rejects ambiguous native lookup");
  Context output = semantic;
  check(strict.bindRecord({17u, 1u}, original, output) == Status::InvalidInput && output == semantic &&
      strict.recordCount() == 1u && strict.bindRecord({17u, 0u}, original, output) == Status::Accepted &&
      output == semantic, "strict ledger rejects new inventory-purpose binding but retries retain their original purpose");
  const auto changedPeer = version(owner, peerIdentity, first->nativeHandle(), 2u);
  check(inventory.replace(peerIdentity, 1u, changedPeer) == Status::Accepted &&
      inventory.find(surface).version == first && inventory.find(peerIdentity).version == changedPeer,
      "explicit producer refresh changes one alias epoch only, never propagates or merges another wire owner");
  Context changed;
  check(inventory.bindRecord({18u, 1u}, changed) == Status::Accepted && changed != original &&
      changed->findIdentity(surface) == first && changed->findIdentity(peerIdentity) == changedPeer &&
      original->findIdentity(peerIdentity) == peer && peer->observation().receipts()[0].revision == 1u,
      "shared native surface does not rewrite original record's independent alias epoch and receipts");
  const auto duplicateWire = version(owner, surface, core::Handle{262u}, 2u);
  check(inventory.define(duplicateWire) == Status::Conflict &&
      inventory.replace(surface, 1u, duplicateWire) == Status::Conflict &&
      inventory.find(surface).version == first,
      "metadata mode still rejects duplicate complete wire identity and changes to a live owner's native key");
  auto thirdIdentity = surface;
  ++thirdIdentity.objectId;
  const auto third = version(owner, thirdIdentity, first->nativeHandle(), 1u);
  check(inventory.define(third) == Status::CapacityExceeded && inventory.currentSize() == 2u,
      "metadata current-entry limit counts every captured wire alias separately");
  check(inventory.destroy(peerIdentity, 2u) == Status::Accepted && inventory.find(surface).version == first &&
      !inventory.find(peerIdentity).version, "alias destruction removes exact wire owner, not all native-key peers");
  Context single, retry;
  check(inventory.bindRecord({18u, 2u}, single) == Status::Accepted &&
      single->find(surface.kind, first->nativeHandle()) == first && !single->hasSemanticUniqueNativeAuthority() &&
      inventory.bindRecord({18u, 0u}, semantic, retry) == Status::Accepted && retry == original &&
      retry->findIdentity(peerIdentity) == peer && !retry->find(surface.kind, first->nativeHandle()),
      "alias-free later inventory stays metadata-only while same-locator retry retains original ambiguous inventory");
  check(inventory.destroy(surface, 1u) == Status::Accepted &&
      inventory.bindRecord({18u, 0u}, retry) == Status::Accepted && retry == original,
      "all current alias owners may retire without discarding original record-bound inventory");
}

void surfaceAliasModeDoesNotBroadenOtherKeys() {
  auto owner = budget();
  Ledger inventory(owner, {.maxCurrentEntries = 4u, .maxRecordContexts = 2u}, Purpose::V4SurfaceAliasInventory);
  const std::array<std::uint8_t, 1u> bytes{8u};
  const auto firstBuffer = version(owner, buffer, core::Handle{271u}, 1u, bytes);
  auto peerBufferIdentity = buffer;
  ++peerBufferIdentity.objectId;
  const auto peerBuffer = version(owner, peerBufferIdentity, firstBuffer->nativeHandle(), 2u, bytes);
  const auto firstTexture = version(owner, texture, core::Handle{272u}, 1u);
  auto peerTextureIdentity = texture;
  ++peerTextureIdentity.objectId;
  const auto peerTexture = version(owner, peerTextureIdentity, firstTexture->nativeHandle(), 2u);
  check(inventory.define(firstBuffer) == Status::Accepted && inventory.define(firstTexture) == Status::Accepted &&
      inventory.define(peerBuffer) == Status::Conflict && inventory.define(peerTexture) == Status::Conflict &&
      inventory.currentSize() == 2u, "surface inventory mode does not admit BUFFER/TEXTURE native aliases");
  Context original;
  check(inventory.bindRecord({19u, 0u}, original) == Status::Accepted &&
      original->find(buffer.kind, firstBuffer->nativeHandle()) == firstBuffer &&
      original->find(texture.kind, firstTexture->nativeHandle()) == firstTexture &&
      !original->hasSemanticUniqueNativeAuthority(), "inventory mode retains metadata-only purpose even without surface aliases");
  auto foreignOwner = budget();
  const auto foreign = version(foreignOwner, surface, core::Handle{273u}, 1u);
  const std::array foreignEntries{foreign};
  Context foreignInventory, out = original;
  check(buildCpuTapeV4SurfaceAliasInventory(foreignEntries, foreignOwner, foreignInventory) == Status::Accepted &&
      inventory.define(foreign) == Status::InvalidInput &&
      inventory.bindRecord({19u, 1u}, foreignInventory, out) == Status::InvalidInput && out == original &&
      inventory.recordCount() == 1u, "explicit inventory purpose cannot bypass exact ledger session association");
  Ledger invalid(owner, {}, static_cast<Purpose>(255u));
  check(invalid.define(firstBuffer) == Status::InvalidInput &&
      invalid.freezeCurrent(out) == Status::InvalidInput &&
      invalid.bindRecord({19u, 0u}, out) == Status::InvalidInput && out == original &&
      !invalid.currentSize() && !invalid.recordCount(), "malformed explicit inventory purpose fails closed before publication");
}

void exactBudgetAndMetadataBounds() {
  auto owner = budget();
  auto foreignBudget = budget();
  Ledger ledger(owner, {.maxCurrentEntries = 1u, .maxRecordContexts = 1u});
  const auto own = version(owner, texture, core::Handle{301u}, 1u);
  const auto foreign = version(foreignBudget, texture, core::Handle{302u}, 1u);
  const auto normalized = observation(texture, 1u);
  Version legacy;
  check(buildCpuTapeResourceVersion({.nativeKind = texture.kind, .nativeHandle = core::Handle{303u},
      .contentEpoch = 1u}, normalized, legacy) == Status::Accepted,
      "legacy fixture version remains per-version only");
  check(ledger.define(foreign) == Status::InvalidInput && ledger.define(legacy) == Status::InvalidInput &&
      !ledger.currentSize() && ledger.define(own) == Status::Accepted,
      "producer ledger rejects unbudgeted/mixed-session versions including zero-byte resources");
  Context foreignContext, legacyContext, out;
  const std::array foreignEntries{foreign};
  const std::array legacyEntries{legacy};
  check(buildCpuTapeResourceContext(foreignEntries, foreignContext) == Status::Accepted &&
      buildCpuTapeResourceContext(legacyEntries, legacyContext) == Status::Accepted,
      "generic context factory intentionally does not establish session admission");
  check(ledger.bindRecord({21u, 0u}, foreignContext, out) == Status::InvalidInput &&
      ledger.bindRecord({21u, 0u}, legacyContext, out) == Status::InvalidInput && !out && !ledger.recordCount(),
      "new record publication validates every immutable version's exact session budget");
  const auto extra = version(owner, texture, core::Handle{304u}, 1u);
  const std::array oversizedEntries{own, extra};
  Context oversized;
  check(buildCpuTapeResourceContext(oversizedEntries, oversized) == Status::Accepted &&
      ledger.bindRecord({21u, 0u}, oversized, out) == Status::InvalidInput && !out && !ledger.recordCount(),
      "prepared inventory metadata must satisfy ledger entry count as well as byte/session limits");
  check(ledger.bindRecord({21u, 0u}, out) == Status::Accepted && out->entries().size() == 1u,
      "own-session inventory binds at exact record metadata limit");
  const auto prior = out;
  check(ledger.bindRecord({21u, 1u}, out) == Status::CapacityExceeded && out == prior && ledger.recordCount() == 1u &&
      ledger.bindRecord({0u, 0u}, out) == Status::InvalidInput &&
      ledger.bindRecord({21u, UINT32_MAX}, out) == Status::InvalidInput && out == prior,
      "invalid/over-cap record publication preserves previous output and store");
  check(ledger.bindRecord({21u, 0u}, foreignContext, out) == Status::Accepted && out == prior &&
      out->entries()[0]->usesExpectedBufferBudget(*owner),
      "a retry ignores replacement input and revalidates/returns its original exact-budget context");

  for (const auto limits : std::array<CpuTapeProducerResourceLedgerV4Limits, 4>{{
           {0u, 1u}, {1u, 0u}, {std::numeric_limits<std::uint32_t>::max(), 1u},
           {1u, std::numeric_limits<std::uint32_t>::max()}}}) {
    Ledger invalid(owner, limits);
    auto preserved = prior;
    check(invalid.define(own) == Status::InvalidInput && invalid.freezeCurrent(preserved) == Status::InvalidInput &&
        invalid.bindRecord({22u, 0u}, preserved) == Status::InvalidInput && preserved == prior &&
        !invalid.currentSize() && !invalid.recordCount(), "zero/malformed metadata budgets fail closed transactionally");
  }
  Ledger missingBudget(std::shared_ptr<CpuTapeExpectedBufferBudget>{});
  auto preserved = prior;
  check(missingBudget.define(own) == Status::InvalidInput && missingBudget.freezeCurrent(preserved) == Status::InvalidInput &&
      preserved == prior, "absent session budget cannot admit producer inventory");
}

void exhaustedImageBudgetPreservesProducerLedger() {
  auto owner = budget(4u);
  Ledger ledger(owner);
  const std::array<std::uint8_t, 4> bytes{1u, 2u, 3u, 4u};
  const auto first = version(owner, buffer, core::Handle{401u}, 1u, bytes);
  check(ledger.define(first) == Status::Accepted, "image-limit fixture installs admitted complete image");
  Context original;
  check(ledger.bindRecord({31u, 0u}, original) == Status::Accepted, "original record pins image before next mutation");
  auto out = first;
  const auto normalized = observation(buffer, 2u);
  check(buildBudgetedCpuTapeResourceVersion({.nativeKind = buffer.kind, .nativeHandle = first->nativeHandle(),
      .contentEpoch = 2u, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes}, normalized, owner, out) ==
      Status::CapacityExceeded && out == first && ledger.find(buffer).version == first &&
      ledger.lookupRecord({31u, 0u}) == original && owner->snapshot().chargedExpectedBufferBytes == 4u,
      "failed external version construction preserves prior output/current entry/record and cumulative credit");
}

void exhaustiveLiveEpochTransitions() {
  auto owner = budget();
  std::size_t cases = 0u;
  for (std::uint64_t nextEpoch = 1u; nextEpoch != 4u; ++nextEpoch)
    for (std::uint64_t expectedPrior = 0u; expectedPrior != 4u; ++expectedPrior)
      for (bool foreignIdentity : {false, true})
        for (bool changedNative : {false, true}) {
          Ledger ledger(owner, {.maxCurrentEntries = 1u, .maxRecordContexts = 1u});
          const auto first = version(owner, texture, core::Handle{501u}, 1u);
          check(ledger.define(first) == Status::Accepted, "bounded epoch trace begins at one actual live version");
          Context original;
          check(ledger.bindRecord({41u, 0u}, original) == Status::Accepted, "bounded epoch trace binds original record first");
          auto candidateIdentity = texture;
          if (foreignIdentity) ++candidateIdentity.generation;
          const auto next = version(owner, candidateIdentity, core::Handle{changedNative ? 502u : 501u}, nextEpoch);
          const auto expected = !expectedPrior || foreignIdentity ? Status::InvalidInput :
              expectedPrior != 1u || nextEpoch <= expectedPrior || changedNative ? Status::Conflict : Status::Accepted;
          check(ledger.replace(candidateIdentity, expectedPrior, next) == expected &&
              ledger.find(texture).version == (expected == Status::Accepted ? next : first),
              "actual replacement exhausts exact identity/native/prior-epoch/progression predicates");
          Context retry;
          check(ledger.bindRecord({41u, 0u}, retry) == Status::Accepted && retry == original &&
              retry->entries()[0] == first && ledger.recordCount() == 1u,
              "every successful/rejected epoch transition preserves original write-once record binding");
          ++cases;
        }
  check(cases == 48u && !owner->snapshot().chargedExpectedBufferBytes,
      "bounded logical epoch domain is exhausted without adding fake expected-byte charges");
}

void consumerReadsOnlyImmutableRecordStore() {
  constexpr std::size_t records = 64u;
  auto owner = budget(records * 8u);
  {
    Ledger ledger(owner, {.maxCurrentEntries = 1u, .maxRecordContexts = records});
    const auto& immutableRecords = ledger.recordContexts();
    const auto normalized = observation(buffer, 1u);
    std::atomic<bool> done{false}, okay{true};
    std::atomic<std::size_t> observed{0u};
    const auto read = [&] {
      do {
        for (std::size_t index = 0u; index != records; ++index) {
          const auto context = immutableRecords.lookup({51u, static_cast<std::uint32_t>(index)});
          if (!context) continue;
          const auto entry = context->find(buffer.kind, core::Handle{601u});
          if (context->entries().size() != 1u || !entry || entry->contentEpoch() != index + 1u ||
              !entry->localBufferFacts() || entry->localBufferFacts()->contentRevision != index + 51u ||
              entry->localBufferFacts()->buffer != core::Handle{601u} ||
              entry->expectedBufferBytes().size() != 8u || !std::all_of(entry->expectedBufferBytes().begin(),
                  entry->expectedBufferBytes().end(), [&](std::uint8_t byte) { return byte == index; }))
            okay.store(false, std::memory_order_relaxed);
          observed.fetch_add(1u, std::memory_order_relaxed);
        }
      } while (!done.load(std::memory_order_acquire));
    };
    std::jthread firstReader(read), secondReader(read);
    for (std::size_t index = 0u; index != records; ++index) {
      const auto epoch = index + 1u;
      std::array<std::uint8_t, 8> bytes{};
      bytes.fill(static_cast<std::uint8_t>(index));
      Version next;
      Context bound;
      if (buildBudgetedCpuTapeResourceVersion({.nativeKind = buffer.kind, .nativeHandle = core::Handle{601u},
              .contentEpoch = epoch, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes,
              .localBufferFacts = facts(core::Handle{601u}, bytes.size(), index + 51u)},
              normalized, owner, next) != Status::Accepted ||
          (index ? ledger.replace(buffer, epoch - 1u, next) : ledger.define(next)) != Status::Accepted ||
          ledger.bindRecord({51u, static_cast<std::uint32_t>(index)}, bound) != Status::Accepted) {
        okay.store(false, std::memory_order_relaxed);
        break;
      }
      bytes.fill(255u);
    }
    done.store(true, std::memory_order_release);
    firstReader.join();
    secondReader.join();
    check(okay.load() && observed.load() && ledger.currentSize() == 1u && ledger.recordCount() == records &&
        owner->snapshot().chargedExpectedBufferBytes == records * 8u,
        "concurrent consumers observe original immutable images and historical facts, never mutable producer tables or latest snapshots");
  }
  check(!owner->snapshot().chargedExpectedBufferBytes,
      "joined consumer/store teardown releases every retained version image");
}

void consumerReadsImmutableSurfaceAliasInventories() {
  constexpr std::size_t records = 64u;
  auto owner = budget();
  auto peerIdentity = surface;
  ++peerIdentity.objectId;
  Ledger ledger(owner, {.maxCurrentEntries = 2u, .maxRecordContexts = records}, Purpose::V4SurfaceAliasInventory);
  const auto peer = version(owner, peerIdentity, core::Handle{611u}, 7u);
  check(ledger.define(peer) == Status::Accepted, "concurrent alias fixture installs one unchanged captured wire owner");
  const auto& immutableRecords = ledger.recordContexts();
  std::atomic<bool> done{false}, okay{true};
  std::atomic<std::size_t> observed{0u};
  const auto read = [&] {
    do {
      for (std::size_t index = 0u; index != records; ++index) {
        const auto context = immutableRecords.lookup({52u, static_cast<std::uint32_t>(index)});
        if (!context) continue;
        const auto entry = context->findIdentity(surface);
        if (context->purpose() != Purpose::V4SurfaceAliasInventory || context->hasSemanticUniqueNativeAuthority() ||
            context->entries().size() != 2u || context->find(surface.kind, peer->nativeHandle()) ||
            context->findIdentity(peerIdentity) != peer || !entry || entry->contentEpoch() != index + 1u ||
            entry->nativeHandle() != peer->nativeHandle() || entry->observation().receipts()[0].revision != index + 1u)
          okay.store(false, std::memory_order_relaxed);
        observed.fetch_add(1u, std::memory_order_relaxed);
      }
    } while (!done.load(std::memory_order_acquire));
  };
  std::jthread firstReader(read), secondReader(read);
  for (std::size_t index = 0u; index != records; ++index) {
    const auto epoch = index + 1u;
    const std::array receipts{CpuTapeResourceReceipt{.identity = surface, .revision = epoch}};
    CpuTapeResourceObservation normalized;
    Version next;
    Context bound;
    if (buildCpuTapeResourceObservation({.identity = surface, .contentIdentity = surface, .receipts = receipts},
            normalized) != CpuTapeResourceObservationStatus::Accepted ||
        buildBudgetedCpuTapeResourceVersion({.nativeKind = surface.kind, .nativeHandle = peer->nativeHandle(),
            .contentEpoch = epoch}, normalized, owner, next) != Status::Accepted ||
        (index ? ledger.replace(surface, epoch - 1u, next) : ledger.define(next)) != Status::Accepted ||
        ledger.bindRecord({52u, static_cast<std::uint32_t>(index)}, bound) != Status::Accepted) {
      okay.store(false, std::memory_order_relaxed);
      break;
    }
  }
  done.store(true, std::memory_order_release);
  firstReader.join();
  secondReader.join();
  check(okay.load() && observed.load() && ledger.currentSize() == 2u && ledger.recordCount() == records &&
      !owner->snapshot().chargedExpectedBufferBytes,
      "concurrent consumers retain every original alias identity/epoch without arbitrary native selection or mutable producer reads");
}

} // namespace

int main() {
  try {
    immutableInventoriesAndOriginalRecordRetries();
    fullIdentityEpochAndLocalFactFailures();
    aliasRefreshRemainsProducerOwned();
    explicitSurfaceAliasInventoryMode();
    surfaceAliasModeDoesNotBroadenOtherKeys();
    exactBudgetAndMetadataBounds();
    exhaustedImageBudgetPreservesProducerLedger();
    exhaustiveLiveEpochTransitions();
    consumerReadsOnlyImmutableRecordStore();
    consumerReadsImmutableSurfaceAliasInventories();
    std::cout << "cpu producer resource ledger v4 checks: " << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cpu producer resource ledger v4 failure: " << error.what() << '\n';
    return 1;
  }
}
