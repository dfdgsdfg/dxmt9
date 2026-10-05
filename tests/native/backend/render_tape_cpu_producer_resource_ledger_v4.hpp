#pragma once

#include "render_tape_cpu_resource_context.hpp"
#include "dxmt9/dxmt9_buffer_read_proof.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace dxmt9::test {

struct CpuTapeProducerResourceLedgerV4Limits {
  std::size_t maxCurrentEntries = 1024u;
  std::size_t maxRecordContexts = 65536u;
};

// Producer-returned inventory values only. localBufferFacts is a convenience
// COPY projected from version, never a separate mutable ledger-owned value.
// The logical version epoch is distinct from the optional instantaneous Pool
// contentRevision/current allocation.
// Neither this value nor equality of those scalars proves backing retention,
// physical bytes, mutation ordering or an exact per-draw resource closure.
struct CpuTapeProducerResourceEntryV4 {
  std::shared_ptr<const CpuTapeResourceVersion> version{};
  std::optional<resources::BufferVersionObservation> localBufferFacts{};
};

// Cold, serialized producer owner. The executor independently authenticates
// successful definitions/mutations/destruction and BEFORE-original-BeforeRecord
// timing. This helper never dereferences wrappers, calls Pool, chooses a source
// cut, or grants semantic/GPU execution authority.
//
// Consumers receive only the const record store or its shared immutable
// contexts, never the mutable current inventory. The ledger
// owner must outlive every borrowed record-store reference and join consumers
// before destruction. Metadata is separately bounded by maxCurrentEntries and
// maxRecordContexts (at most their product in record-bound version references;
// current/cache/temporary inventories each also obey maxCurrentEntries).
// These are metadata counts, not RSS. Full expected images retain the exact
// shared session byte budget.
class CpuTapeProducerResourceLedgerV4 {
public:
  using Status = CpuTapeResourceContextStatus;
  using Version = std::shared_ptr<const CpuTapeResourceVersion>;
  using Context = std::shared_ptr<const CpuTapeResourceContext>;
  using Locator = render::TapeEmissionRecordLocator;
  using Purpose = CpuTapeResourceContextPurpose;

  explicit CpuTapeProducerResourceLedgerV4(std::shared_ptr<CpuTapeExpectedBufferBudget> budget,
      CpuTapeProducerResourceLedgerV4Limits limits = {},
      Purpose purpose = Purpose::SemanticUniqueNative) noexcept
      : budget_(std::move(budget)), limits_(limits), purpose_(purpose), records_({limits.maxRecordContexts}) {}
  CpuTapeProducerResourceLedgerV4(const CpuTapeProducerResourceLedgerV4&) = delete;
  CpuTapeProducerResourceLedgerV4& operator=(const CpuTapeProducerResourceLedgerV4&) = delete;
  CpuTapeProducerResourceLedgerV4(CpuTapeProducerResourceLedgerV4&&) = delete;
  CpuTapeProducerResourceLedgerV4& operator=(CpuTapeProducerResourceLedgerV4&&) = delete;

  // Producer-only methods. Input versions must already have passed the named
  // budgeted factory, including complete BUFFER-image copying. Unsupported
  // resource kinds (queries/shaders/declarations) never enter this inventory.
  // Default native uniqueness is unchanged. Explicit V4SurfaceAliasInventory
  // purpose permits distinct SURFACE wire owners only, never merges them, and
  // does not infer physical aliasing or cross-owner mutation propagation.
  // Local buffer observations belong to the immutable version itself. No
  // second facts input/table can manufacture, replace or discard its history.
  Status define(const Version& version) noexcept {
    if (!usable() || !validVersion(version)) return Status::InvalidInput;
    for (const auto& entry : current_)
      if (sameIdentity(entry->identity(), version->identity()) ||
          (sameNativeKey(*entry, *version) &&
              (purpose_ != Purpose::V4SurfaceAliasInventory ||
                  version->nativeKind() != D9C_CHUNK_HANDLE_KIND_SURFACE))) return Status::Conflict;
    if (current_.size() >= limits_.maxCurrentEntries || current_.size() == current_.max_size())
      return Status::CapacityExceeded;
    try {
      current_.push_back(version);
      cachedInventory_.reset();
      return Status::Accepted;
    } catch (...) { return Status::AllocationFailed; }
  }

  Status replace(D9CWireObjectIdentity identity, std::uint64_t expectedPriorEpoch,
      const Version& version) noexcept {
    if (!usable() || !expectedPriorEpoch || !validVersion(version) ||
        !sameIdentity(identity, version->identity())) return Status::InvalidInput;
    for (auto& entry : current_) {
      if (!sameIdentity(entry->identity(), identity)) continue;
      if (entry->contentEpoch() != expectedPriorEpoch ||
          version->contentEpoch() <= expectedPriorEpoch || !sameNativeKey(*entry, *version))
        return Status::Conflict;
      entry = version;
      cachedInventory_.reset();
      return Status::Accepted;
    }
    return Status::InvalidInput;
  }

  Status destroy(D9CWireObjectIdentity identity, std::uint64_t expectedEpoch) noexcept {
    if (!usable() || !expectedEpoch) return Status::InvalidInput;
    for (auto entry = current_.begin(); entry != current_.end(); ++entry) {
      if (!sameIdentity((*entry)->identity(), identity)) continue;
      if ((*entry)->contentEpoch() != expectedEpoch) return Status::Conflict;
      current_.erase(entry);
      cachedInventory_.reset();
      return Status::Accepted;
    }
    return Status::InvalidInput;
  }

  CpuTapeProducerResourceEntryV4 find(D9CWireObjectIdentity identity) const noexcept {
    for (const auto& entry : current_)
      if (sameIdentity(entry->identity(), identity)) return {entry, entry->localBufferFacts()};
    return {};
  }
  std::size_t currentSize() const noexcept { return current_.size(); }

  // A whole immutable inventory, not the exact resources fetched by a draw.
  // No expected bytes are recopied; unchanged inventories reuse one pointer.
  Status freezeCurrent(Context& out) noexcept {
    if (!usable()) return Status::InvalidInput;
    if (cachedInventory_) {
      out = cachedInventory_;
      return Status::Accepted;
    }
    try {
      std::vector<Version> versions;
      versions.reserve(current_.size());
      for (const auto& entry : current_) versions.push_back(entry);
      Context candidate;
      const auto status = purpose_ == Purpose::V4SurfaceAliasInventory
          ? buildCpuTapeV4SurfaceAliasInventory(versions, budget_, candidate, {limits_.maxCurrentEntries})
          : buildCpuTapeResourceContext(versions, candidate, {limits_.maxCurrentEntries});
      if (status != Status::Accepted) return status;
      cachedInventory_ = candidate;
      out = std::move(candidate);
      return Status::Accepted;
    } catch (...) { return Status::AllocationFailed; }
  }

  // Call before forwarding the genuine original BeforeRecord callback. A retry
  // selects its ORIGINAL pointer even after later producer changes; neither an
  // unrelated prepared context nor current inventory can replace that binding.
  Status bindRecord(Locator locator, Context& out) noexcept {
    if (!usable() || !validLocator(locator)) return Status::InvalidInput;
    if (const auto original = records_.lookup(locator)) return reuseOriginal(original, out);
    if (records_.size() >= limits_.maxRecordContexts) return Status::CapacityExceeded;
    Context candidate;
    const auto status = freezeCurrent(candidate);
    if (status != Status::Accepted) return status;
    return bindRecord(locator, candidate, out);
  }

  Status bindRecord(Locator locator, const Context& prepared, Context& out) noexcept {
    if (!usable() || !validLocator(locator)) return Status::InvalidInput;
    if (const auto original = records_.lookup(locator)) return reuseOriginal(original, out);
    if (!validContext(prepared)) return Status::InvalidInput;
    const auto status = records_.bind(locator, prepared);
    if (status == Status::Accepted) out = prepared;
    return status;
  }

  // These access only the existing thread-safe immutable record store. No
  // producer/current-table read is part of a consumer lookup.
  Context lookupRecord(Locator locator) const noexcept { return records_.lookup(locator); }
  const CpuTapeRecordContextStore& recordContexts() const noexcept { return records_; }
  std::size_t recordCount() const noexcept { return records_.size(); }

private:
  static bool sameIdentity(D9CWireObjectIdentity left, D9CWireObjectIdentity right) noexcept {
    return left.kind == right.kind && left.generation == right.generation && left.objectId == right.objectId;
  }
  static bool sameNativeKey(const CpuTapeResourceVersion& left, const CpuTapeResourceVersion& right) noexcept {
    return left.nativeKind() == right.nativeKind() && left.nativeHandle() == right.nativeHandle();
  }
  static bool supportedKind(std::uint32_t kind) noexcept {
    return kind == D9C_CHUNK_HANDLE_KIND_BUFFER || kind == D9C_CHUNK_HANDLE_KIND_TEXTURE ||
        kind == D9C_CHUNK_HANDLE_KIND_SURFACE;
  }
  static bool validLocator(Locator locator) noexcept {
    return locator.eventOrdinal && locator.recordIndex != std::numeric_limits<std::uint32_t>::max();
  }
  bool usable() const noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max() - 1u;
    return budget_ && limits_.maxCurrentEntries && limits_.maxCurrentEntries <= maximum &&
        limits_.maxRecordContexts && limits_.maxRecordContexts <= maximum &&
        (purpose_ == Purpose::SemanticUniqueNative || purpose_ == Purpose::V4SurfaceAliasInventory);
  }
  bool validVersion(const Version& version) const noexcept {
    return version && supportedKind(version->nativeKind()) && version->usesExpectedBufferBudget(*budget_);
  }
  bool validContext(const Context& context) const noexcept {
    if (!context || context->purpose() != purpose_ ||
        context->entries().size() > limits_.maxCurrentEntries) return false;
    for (const auto& version : context->entries())
      if (!validVersion(version)) return false;
    return true;
  }
  Status reuseOriginal(const Context& original, Context& out) const noexcept {
    if (!validContext(original)) return Status::InvalidInput;
    out = original;
    return Status::Accepted;
  }

  std::shared_ptr<CpuTapeExpectedBufferBudget> budget_{};
  CpuTapeProducerResourceLedgerV4Limits limits_{};
  Purpose purpose_ = Purpose::SemanticUniqueNative;
  std::vector<Version> current_{};
  Context cachedInventory_{};
  CpuTapeRecordContextStore records_;
};

} // namespace dxmt9::test
