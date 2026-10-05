#pragma once

#include "render_tape_cpu_resource_context.hpp"
#include "device_c_render_tape_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::test {

struct RenderTapeEncodeOriginalBufferLimits {
  // Includes retired identity tombstones, not just currently active buffers.
  std::size_t maxBuffers = 1024u;
  std::size_t maxRecordContexts = 65536u;
  std::size_t maxReceiptsPerBuffer = 65536u;
  std::size_t maxExpectedBufferBytes = 256u * 1024u * 1024u;
  // At most one temporary complete patch image exists, on the serialized
  // producer stack. It is separately bounded, never retained by a consumer.
  // Peak expected-byte ownership is the shared retained budget plus this cap;
  // the shared budget's counters intentionally do not claim scratch/RSS.
  std::size_t maxScratchBufferBytes = 64u * 1024u * 1024u;
};

struct RenderTapeEncodeOriginalBufferFacts {
  D9CWireObjectIdentity identity{};
  core::Handle nativeHandle{};
  resources::BufferVersionObservation observation{};
};

struct RenderTapeEncodeOriginalBufferEntry {
  D9CWireObjectIdentity identity{};
  core::Handle nativeHandle{};
  std::uint64_t byteSize = 0u, contentEpoch = 0u;
  bool hidden = false, pendingSeed = false, imageComplete = false;
  std::shared_ptr<const CpuTapeResourceVersion> version{};
};

// Test-side cold original BUFFER owner. The caller authenticates original
// event/header/descriptor provenance and associates the actual runtime logical
// handle, never a handle from a second execution. This helper rechecks blob
// size and SHA-256 before deriving canonical bytes. It has no wrapper/device/
// Pool pointer and no native/mapped/wrapper byte-reading interface.
//
// Definition creates UNKNOWN contents, not allocator-zero contents. Only a
// full authenticated mutation establishes a complete image; partial initial
// mutations and partial DISCARD leave it unknown. Such buffers are omitted
// from immutable inventories, so later draw lookup fails closed. Inventories
// are BUFFER subsets with V4SurfaceAliasInventory purpose, never whole-tape
// semantic, physical-fetch-complete, GPU, ordering or lifetime authority.
//
// One serialized producer owns transitions. Consumers use ONLY recordContexts
// or lookupRecord, whose write-once immutable contexts outlive all later
// mutations/retirement. The facade must outlive borrowed record-store refs.
class RenderTapeEncodeOriginalBufferContexts {
public:
  using Status = CpuTapeResourceContextStatus;
  using Version = std::shared_ptr<const CpuTapeResourceVersion>;
  using Context = std::shared_ptr<const CpuTapeResourceContext>;
  using Locator = render::TapeEmissionRecordLocator;

  explicit RenderTapeEncodeOriginalBufferContexts(
      std::shared_ptr<CpuTapeExpectedBufferBudget> budget,
      RenderTapeEncodeOriginalBufferLimits limits = {}) noexcept
      : budget_(std::move(budget)), limits_(limits), records_({limits.maxRecordContexts}) {}
  RenderTapeEncodeOriginalBufferContexts(const RenderTapeEncodeOriginalBufferContexts&) = delete;
  RenderTapeEncodeOriginalBufferContexts& operator=(const RenderTapeEncodeOriginalBufferContexts&) = delete;

  Status define(const d3d9::RenderTapeObjectDefineHeader& definition,
      std::span<const std::byte> descriptor, core::Handle actualNative,
      bool hidden = false) noexcept {
    D9CBufferDesc desc{};
    if (!usable() || !validIdentity(definition.identity) || !actualNative ||
        definition.descriptorKind != static_cast<std::uint32_t>(d3d9::RenderTapeDescriptorKind::Buffer) ||
        definition.descriptorBytes != sizeof(desc) || descriptor.size() != sizeof(desc) ||
        definition.reserved0 || definition.reserved1 || definition.payloadValidity ||
        definition.immutablePayloadBytes || definition.immutablePayloadDigest != d3d9::RenderTapeDigest{})
      return Status::InvalidInput;
    std::memcpy(&desc, descriptor.data(), sizeof(desc));
    if (!desc.size ||
        !((definition.expectedContentCount == 0u && definition.expectedContentBytes == 0u) ||
          (definition.expectedContentCount == 1u && definition.expectedContentBytes == desc.size)))
      return Status::InvalidInput;
    if (desc.size > limits_.maxExpectedBufferBytes) return Status::CapacityExceeded;
    for (const auto& entry : entries_) {
      if (entry.identity.objectId == definition.identity.objectId &&
          (!entry.retired || definition.identity.generation <= entry.identity.generation))
        return Status::Conflict;
      if (!entry.retired && entry.nativeHandle == actualNative) return Status::Conflict;
    }
    if (entries_.size() >= limits_.maxBuffers || entries_.size() == entries_.max_size())
      return Status::CapacityExceeded;
    try {
      Entry candidate;
      candidate.identity = definition.identity;
      candidate.nativeHandle = actualNative;
      candidate.byteSize = desc.size;
      candidate.hidden = hidden;
      entries_.push_back(std::move(candidate));
      cachedInventory_.reset();
      return Status::Accepted;
    } catch (...) { return Status::AllocationFailed; }
  }

  // Successful physical preparation of an original seed, before the normal
  // drain. Remember its exact original event/header so later logical consume
  // cannot upload/patch or increment this epoch a second time.
  Status hiddenSeedApplied(std::uint64_t originalOrdinal,
      const d3d9::RenderTapeResourceMutationHeader& mutation,
      std::span<const std::byte> blob) noexcept {
    auto* entry = findMutable(mutation.identity);
    if (!usable() || !entry) return Status::InvalidInput;
    if (!entry->hidden || entry->pendingSeed) return Status::Conflict;
    const auto status = apply(*entry, originalOrdinal, mutation, blob);
    if (status != Status::Accepted) return status;
    entry->pendingSeed = Seed{originalOrdinal, mutation};
    return Status::Accepted;
  }

  Status activate(D9CWireObjectIdentity identity, core::Handle actualNative) noexcept {
    auto* entry = findMutable(identity);
    if (!usable() || !entry || !actualNative) return Status::InvalidInput;
    if (!entry->hidden || entry->nativeHandle != actualNative) return Status::Conflict;
    entry->hidden = false;
    cachedInventory_.reset();
    return Status::Accepted;
  }

  Status seedConsumed(std::uint64_t originalOrdinal,
      const d3d9::RenderTapeResourceMutationHeader& mutation) noexcept {
    auto* entry = findMutable(mutation.identity);
    if (!usable() || !entry || !originalOrdinal) return Status::InvalidInput;
    if (entry->hidden || !entry->pendingSeed ||
        entry->pendingSeed->ordinal != originalOrdinal ||
        !sameMutation(entry->pendingSeed->mutation, mutation)) return Status::Conflict;
    entry->pendingSeed.reset();
    cachedInventory_.reset();
    return Status::Accepted;
  }

  Status mutationApplied(std::uint64_t originalOrdinal,
      const d3d9::RenderTapeResourceMutationHeader& mutation,
      std::span<const std::byte> blob) noexcept {
    auto* entry = findMutable(mutation.identity);
    if (!usable() || !entry) return Status::InvalidInput;
    if (entry->hidden || entry->pendingSeed) return Status::Conflict;
    return apply(*entry, originalOrdinal, mutation, blob);
  }

  Status retire(D9CWireObjectIdentity identity, core::Handle actualNative) noexcept {
    auto* entry = findMutable(identity);
    if (!usable() || !entry || !actualNative) return Status::InvalidInput;
    if (entry->nativeHandle != actualNative) return Status::Conflict;
    // Tombstones preserve generation qualification, not wrapper/backing life.
    entry->retired = true;
    entry->version.reset();
    entry->receipts.clear();
    entry->pendingSeed.reset();
    entry->nativeHandle = {};
    cachedInventory_.reset();
    return Status::Accepted;
  }

  // Call ONLY after the actual provider's existing preRecord replay drain and
  // before its original record callback/publication. Facts are value copies
  // from actual associations; this helper cannot authenticate that ordering,
  // wait, retain a backing, or prove physical bytes. Late fact attachment uses
  // the SAME canonical epoch. No epoch/revision equality is assumed.
  //
  // Two-phase transaction: tentative versions/context are fully allocated and
  // bound first, then producer entries/cache are changed with no-fail pointer
  // moves. Any rejection preserves current state, output, prior record binds
  // and retained-byte charges. An existing locator wins before facts/current
  // are consulted, including after retirement or later producer mutations.
  Status beforeRecord(Locator locator,
      std::span<const RenderTapeEncodeOriginalBufferFacts> facts, Context& out) noexcept {
    if (!usable() || !validLocator(locator)) return Status::InvalidInput;
    if (const auto original = records_.lookup(locator)) {
      out = original;
      return Status::Accepted;
    }
    if (records_.size() >= limits_.maxRecordContexts) return Status::CapacityExceeded;
    if (facts.size() > limits_.maxBuffers) return Status::CapacityExceeded;
    for (std::size_t index = 0u; index < facts.size(); ++index) {
      const auto* entry = findConst(facts[index].identity);
      if (!entry || entry->hidden || entry->pendingSeed ||
          !validFacts(*entry, facts[index])) return Status::InvalidInput;
      for (std::size_t prior = 0u; prior < index; ++prior)
        if (sameIdentity(facts[prior].identity, facts[index].identity) ||
            facts[prior].nativeHandle == facts[index].nativeHandle) return Status::Conflict;
    }
    try {
      std::vector<Replacement> replacements;
      std::vector<Version> versions;
      replacements.reserve(facts.size());
      versions.reserve(entries_.size());
      for (std::size_t index = 0u; index < entries_.size(); ++index) {
        const auto& entry = entries_[index];
        if (entry.retired || entry.hidden || entry.pendingSeed || !entry.version) continue;
        Version version = entry.version;
        for (const auto& local : facts) {
          if (!sameIdentity(local.identity, entry.identity)) continue;
          if (!entry.version->localBufferFacts() ||
              !sameFacts(*entry.version->localBufferFacts(), local.observation)) {
            const auto status = buildVersion(entry, entry.contentEpoch,
                entry.version->expectedBufferBytes(), entry.version->observation(), local.observation, version);
            if (status != Status::Accepted) return status;
            replacements.push_back({index, version});
          }
          break;
        }
        versions.push_back(std::move(version));
      }
      Context candidate;
      if (cachedInventory_ && replacements.empty()) candidate = cachedInventory_;
      else {
        const auto status = buildCpuTapeV4SurfaceAliasInventory(
            versions, budget_, candidate, {limits_.maxBuffers});
        if (status != Status::Accepted) return status;
      }
      const auto bound = records_.bind(locator, candidate);
      if (bound != Status::Accepted) return bound;
      for (auto& replacement : replacements)
        entries_[replacement.index].version = std::move(replacement.version);
      cachedInventory_ = candidate;
      out = std::move(candidate);
      return Status::Accepted;
    } catch (...) { return Status::AllocationFailed; }
  }

  // Producer-only inspection, never a consumer API.
  RenderTapeEncodeOriginalBufferEntry find(D9CWireObjectIdentity identity) const noexcept {
    const auto* entry = findConst(identity);
    if (!entry) return {};
    return {entry->identity, entry->nativeHandle, entry->byteSize, entry->contentEpoch,
        entry->hidden, bool(entry->pendingSeed), bool(entry->version), entry->version};
  }

  Context lookupRecord(Locator locator) const noexcept { return records_.lookup(locator); }
  const CpuTapeRecordContextStore& recordContexts() const noexcept { return records_; }
  std::size_t recordCount() const noexcept { return records_.size(); }

private:
  struct Seed {
    std::uint64_t ordinal = 0u;
    d3d9::RenderTapeResourceMutationHeader mutation{};
  };
  struct Entry {
    D9CWireObjectIdentity identity{};
    core::Handle nativeHandle{};
    std::uint64_t byteSize = 0u, contentEpoch = 0u, lastMutationOrdinal = 0u;
    bool hidden = false, retired = false;
    std::optional<Seed> pendingSeed{};
    std::vector<CpuTapeResourceReceipt> receipts{};
    Version version{};
  };
  struct Replacement { std::size_t index = 0u; Version version{}; };

  static bool sameIdentity(D9CWireObjectIdentity a, D9CWireObjectIdentity b) noexcept {
    return a.kind == b.kind && a.generation == b.generation && a.objectId == b.objectId;
  }
  static bool validIdentity(D9CWireObjectIdentity identity) noexcept {
    return identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER && identity.generation && identity.objectId;
  }
  static bool validLocator(Locator locator) noexcept {
    return locator.eventOrdinal && locator.recordIndex != std::numeric_limits<std::uint32_t>::max();
  }
  static bool sameMutation(const d3d9::RenderTapeResourceMutationHeader& a,
      const d3d9::RenderTapeResourceMutationHeader& b) noexcept {
    return sameIdentity(a.identity, b.identity) && a.kind == b.kind && a.subresource == b.subresource &&
        a.bufferDisposition == b.bufferDisposition && a.byteOffset == b.byteOffset &&
        a.byteSize == b.byteSize && a.digest == b.digest;
  }
  static bool sameFacts(const resources::BufferVersionObservation& a,
      const resources::BufferVersionObservation& b) noexcept {
    return a.status == b.status && a.buffer == b.buffer && a.byteSize == b.byteSize &&
        a.contentRevision == b.contentRevision && a.hasVersionedBacking == b.hasVersionedBacking &&
        a.currentBacking.metalHandle == b.currentBacking.metalHandle &&
        a.currentBacking.byteSize == b.currentBacking.byteSize;
  }
  static bool validFacts(const Entry& entry, const RenderTapeEncodeOriginalBufferFacts& input) noexcept {
    const auto& facts = input.observation;
    return input.nativeHandle == entry.nativeHandle && facts.observed() &&
        facts.buffer == entry.nativeHandle && facts.byteSize == entry.byteSize &&
        facts.contentRevision && facts.currentBacking.metalHandle &&
        facts.currentBacking.byteSize == entry.byteSize;
  }
  bool usable() const noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max() - 1u;
    return budget_ && limits_.maxBuffers && limits_.maxBuffers <= maximum &&
        limits_.maxRecordContexts && limits_.maxRecordContexts <= maximum &&
        limits_.maxReceiptsPerBuffer && limits_.maxReceiptsPerBuffer <= maximum &&
        cpu_resource_context_detail::expectedByteLimitAdmitted(limits_.maxExpectedBufferBytes) &&
        limits_.maxScratchBufferBytes <= limits_.maxExpectedBufferBytes;
  }
  Entry* findMutable(D9CWireObjectIdentity identity) noexcept {
    for (auto& entry : entries_)
      if (!entry.retired && sameIdentity(entry.identity, identity)) return &entry;
    return nullptr;
  }
  const Entry* findConst(D9CWireObjectIdentity identity) const noexcept {
    for (const auto& entry : entries_)
      if (!entry.retired && sameIdentity(entry.identity, identity)) return &entry;
    return nullptr;
  }
  Status buildVersion(const Entry& entry, std::uint64_t epoch,
      std::span<const std::uint8_t> bytes, const CpuTapeResourceObservation& observation,
      std::optional<resources::BufferVersionObservation> facts, Version& out) const noexcept {
    return buildBudgetedCpuTapeResourceVersion({.nativeKind = D9C_CHUNK_HANDLE_KIND_BUFFER,
        .nativeHandle = entry.nativeHandle, .contentEpoch = epoch,
        .expectedBufferByteSize = entry.byteSize, .expectedBufferBytes = bytes,
        .localBufferFacts = facts}, observation, budget_, out, {limits_.maxExpectedBufferBytes});
  }
  Status apply(Entry& entry, std::uint64_t ordinal,
      const d3d9::RenderTapeResourceMutationHeader& mutation,
      std::span<const std::byte> blob) noexcept {
    if (!ordinal || !sameIdentity(entry.identity, mutation.identity) ||
        mutation.subresource != 0u || !mutation.byteSize || mutation.byteSize != blob.size() ||
        (mutation.kind != static_cast<std::uint32_t>(d3d9::RenderTapeMutationKind::Upload) &&
         mutation.kind != static_cast<std::uint32_t>(d3d9::RenderTapeMutationKind::CpuUnlock)) ||
        mutation.bufferDisposition > static_cast<std::uint32_t>(d3d9::RenderTapeBufferMutationDisposition::Discard) ||
        mutation.byteOffset > entry.byteSize || mutation.byteSize > entry.byteSize - mutation.byteOffset)
      return Status::InvalidInput;
    if (ordinal <= entry.lastMutationOrdinal) return Status::Conflict;
    if (entry.contentEpoch == std::numeric_limits<std::uint64_t>::max() ||
        entry.receipts.size() >= limits_.maxReceiptsPerBuffer) return Status::CapacityExceeded;
    const bool full = mutation.byteOffset == 0u && mutation.byteSize == entry.byteSize;
    const bool preserves = mutation.bufferDisposition !=
        static_cast<std::uint32_t>(d3d9::RenderTapeBufferMutationDisposition::Discard);
    if (!full && preserves && entry.version && entry.byteSize > limits_.maxScratchBufferBytes)
      return Status::CapacityExceeded;
    try {
      if (d3d9::RenderTapeCaptureSession::sha256(blob) != mutation.digest) return Status::InvalidInput;
      const auto epoch = entry.contentEpoch + 1u;
      auto receipts = entry.receipts;
      receipts.push_back({.identity = entry.identity, .subresource = 0u,
          .digest = mutation.digest, .revision = epoch});
      Version version;
      if (full || (preserves && entry.version)) {
        CpuTapeResourceObservation observation;
        const auto normalized = buildCpuTapeResourceObservation({.identity = entry.identity,
            .contentIdentity = entry.identity, .receipts = receipts}, observation,
            {limits_.maxReceiptsPerBuffer});
        if (normalized != CpuTapeResourceObservationStatus::Accepted)
          return normalized == CpuTapeResourceObservationStatus::AllocationFailed
              ? Status::AllocationFailed : Status::InvalidInput;
        std::vector<std::uint8_t> scratch;
        std::span<const std::uint8_t> bytes;
        if (full) bytes = {reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()};
        else {
          const auto prior = entry.version->expectedBufferBytes();
          scratch.assign(prior.begin(), prior.end());
          std::memcpy(scratch.data() + static_cast<std::size_t>(mutation.byteOffset), blob.data(), blob.size());
          bytes = scratch;
        }
        // Physical facts are intentionally cleared until the existing normal
        // drain permits a genuine beforeRecord observation of this mutation.
        const auto status = buildVersion(entry, epoch, bytes, observation, std::nullopt, version);
        if (status != Status::Accepted) return status;
      }
      entry.receipts = std::move(receipts);
      entry.version = std::move(version);
      entry.contentEpoch = epoch;
      entry.lastMutationOrdinal = ordinal;
      cachedInventory_.reset();
      return Status::Accepted;
    } catch (...) { return Status::AllocationFailed; }
  }

  std::shared_ptr<CpuTapeExpectedBufferBudget> budget_{};
  RenderTapeEncodeOriginalBufferLimits limits_{};
  std::vector<Entry> entries_{};
  Context cachedInventory_{};
  CpuTapeRecordContextStore records_;
};

} // namespace dxmt9::test
