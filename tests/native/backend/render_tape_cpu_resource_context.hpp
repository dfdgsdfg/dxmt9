#pragma once

#include "render_tape_cpu_resource_observation.hpp"
#include "dxmt9/dxmt9_buffer_read_proof.hpp"
#include "dxmt9/dxmt9_render_tape_emission_observer.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::test {

enum class CpuTapeResourceContextStatus : std::uint8_t {
  Accepted, InvalidInput, CapacityExceeded, Conflict, AllocationFailed,
};

// Immutable construction domain, not proof of full CPU semantics or physical
// bytes. Inventory purpose never grants unique-native semantic authority, even
// when this particular inventory contains no native aliases.
enum class CpuTapeResourceContextPurpose : std::uint8_t {
  SemanticUniqueNative, V4SurfaceAliasInventory,
};

struct CpuTapeResourceVersionInput {
  std::uint32_t nativeKind = 0u;
  core::Handle nativeHandle{};
  std::uint64_t contentEpoch = 0u;
  // A BUFFER caller supplies its complete logical extent, not an Upload patch.
  // Equality checks completeness of the supplied span, not physical backing.
  std::uint64_t expectedBufferByteSize = 0u;
  std::span<const std::uint8_t> expectedBufferBytes{};
  // Optional instantaneous copied Pool observation AFTER the caller's actual
  // successful definition/mutation and BEFORE record publication. Validation
  // does not authenticate ordering, equate epochs/revisions, retain a backing,
  // describe an old ring entry, or prove future physical bytes.
  std::optional<resources::BufferVersionObservation> localBufferFacts{};
};

struct CpuTapeResourceVersionLimits {
  std::size_t maxExpectedBufferBytes = 256u * 1024u * 1024u;
};

struct CpuTapeExpectedBufferBudgetLimits {
  // Explicit session limit: a default/zero budget cannot authorize copies.
  std::size_t maxRetainedExpectedBufferBytes = 0u;
};

struct CpuTapeExpectedBufferBudgetSnapshot {
  std::size_t maxRetainedExpectedBufferBytes = 0u;
  std::size_t chargedExpectedBufferBytes = 0u;
  std::size_t highWaterExpectedBufferBytes = 0u;
};

struct CpuTapeResourceContextLimits {
  std::size_t maxEntries = 1024u;
};

struct CpuTapeRecordContextLimits {
  std::size_t maxRecords = 65536u;
};

class CpuTapeResourceVersion;
class CpuTapeResourceContext;
class CpuTapeExpectedBufferBudget;
CpuTapeResourceContextStatus createCpuTapeExpectedBufferBudget(
    CpuTapeExpectedBufferBudgetLimits, std::shared_ptr<CpuTapeExpectedBufferBudget>&) noexcept;
CpuTapeResourceContextStatus buildCpuTapeResourceVersion(
    const CpuTapeResourceVersionInput&, const CpuTapeResourceObservation&,
    std::shared_ptr<const CpuTapeResourceVersion>&,
    CpuTapeResourceVersionLimits = {}) noexcept;
CpuTapeResourceContextStatus buildBudgetedCpuTapeResourceVersion(
    const CpuTapeResourceVersionInput&, const CpuTapeResourceObservation&,
    const std::shared_ptr<CpuTapeExpectedBufferBudget>&,
    std::shared_ptr<const CpuTapeResourceVersion>&,
    CpuTapeResourceVersionLimits = {}) noexcept;
CpuTapeResourceContextStatus buildCpuTapeResourceContext(
    std::span<const std::shared_ptr<const CpuTapeResourceVersion>>,
    std::shared_ptr<const CpuTapeResourceContext>&,
    CpuTapeResourceContextLimits = {}) noexcept;
CpuTapeResourceContextStatus buildCpuTapeV4SurfaceAliasInventory(
    std::span<const std::shared_ptr<const CpuTapeResourceVersion>>,
    const std::shared_ptr<CpuTapeExpectedBufferBudget>&,
    std::shared_ptr<const CpuTapeResourceContext>&,
    CpuTapeResourceContextLimits = {}) noexcept;

// Cold shared session authority for expected-byte ownership only. Metadata,
// normalized receipts and context vectors are not charged; these counters are
// not process RSS. High-water includes tentative reservations which unwind on
// allocation failure. A version's charge keeps this owner alive independently
// of the session facade and releases only after its byte storage is destroyed.
class CpuTapeExpectedBufferBudget {
public:
  CpuTapeExpectedBufferBudget(const CpuTapeExpectedBufferBudget&) = delete;
  CpuTapeExpectedBufferBudget& operator=(const CpuTapeExpectedBufferBudget&) = delete;
  CpuTapeExpectedBufferBudgetSnapshot snapshot() const noexcept {
    try {
      std::lock_guard lock(mutex_);
      return {limit_, charged_, highWater_};
    } catch (...) { return {}; }
  }

private:
  friend class CpuTapeResourceVersion;
  friend CpuTapeResourceContextStatus createCpuTapeExpectedBufferBudget(
      CpuTapeExpectedBufferBudgetLimits, std::shared_ptr<CpuTapeExpectedBufferBudget>&) noexcept;
  friend CpuTapeResourceContextStatus buildBudgetedCpuTapeResourceVersion(
      const CpuTapeResourceVersionInput&, const CpuTapeResourceObservation&,
      const std::shared_ptr<CpuTapeExpectedBufferBudget>&,
      std::shared_ptr<const CpuTapeResourceVersion>&, CpuTapeResourceVersionLimits) noexcept;

  class Charge {
  public:
    Charge() = default;
    Charge(std::shared_ptr<CpuTapeExpectedBufferBudget> budget, std::size_t bytes) noexcept
        : budget_(std::move(budget)), bytes_(bytes) {}
    ~Charge() { reset(); }
    Charge(const Charge&) = delete;
    Charge& operator=(const Charge&) = delete;
    Charge(Charge&& other) noexcept
        : budget_(std::move(other.budget_)), bytes_(std::exchange(other.bytes_, 0u)) {}
    Charge& operator=(Charge&& other) noexcept {
      if (this != &other) {
        reset();
        budget_ = std::move(other.budget_);
        bytes_ = std::exchange(other.bytes_, 0u);
      }
      return *this;
    }
    bool belongsTo(const CpuTapeExpectedBufferBudget& budget) const noexcept {
      return budget_.get() == &budget;
    }
    bool budgeted() const noexcept { return bool(budget_); }
  private:
    void reset() noexcept {
      auto budget = std::move(budget_);
      const auto bytes = std::exchange(bytes_, 0u);
      if (budget && bytes) budget->release(bytes);
    }
    std::shared_ptr<CpuTapeExpectedBufferBudget> budget_{};
    std::size_t bytes_ = 0u;
  };

  explicit CpuTapeExpectedBufferBudget(std::size_t limit) noexcept : limit_(limit) {}
  CpuTapeResourceContextStatus reserve(std::size_t bytes) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (charged_ > limit_ || bytes > limit_ - charged_)
        return CpuTapeResourceContextStatus::CapacityExceeded;
      charged_ += bytes;
      if (charged_ > highWater_) highWater_ = charged_;
      return CpuTapeResourceContextStatus::Accepted;
    } catch (...) { return CpuTapeResourceContextStatus::AllocationFailed; }
  }
  void release(std::size_t bytes) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (bytes <= charged_) charged_ -= bytes;
    } catch (...) {} // A lock failure conservatively retains credit.
  }
  const std::size_t limit_;
  mutable std::mutex mutex_{};
  std::size_t charged_ = 0u, highWater_ = 0u;
};

// Immutable cold logical resource facts. This owns a copy of the complete
// expected buffer bytes, never a mutable shared vector, wrapper or Pool view.
// Native handles are value lookup keys only; this owner retains no backing and
// neither the content epoch nor these bytes proves physical buffer contents.
class CpuTapeResourceVersion {
public:
  ~CpuTapeResourceVersion() = default;
  CpuTapeResourceVersion(const CpuTapeResourceVersion&) = delete;
  CpuTapeResourceVersion& operator=(const CpuTapeResourceVersion&) = delete;
  std::uint32_t nativeKind() const noexcept { return nativeKind_; }
  core::Handle nativeHandle() const noexcept { return nativeHandle_; }
  const D9CWireObjectIdentity& identity() const noexcept { return identity_; }
  std::uint64_t contentEpoch() const noexcept { return contentEpoch_; }
  const CpuTapeResourceObservation& observation() const noexcept { return observation_; }
  std::span<const std::uint8_t> expectedBufferBytes() const noexcept { return expectedBufferBytes_; }
  const std::optional<resources::BufferVersionObservation>& localBufferFacts() const noexcept {
    return localBufferFacts_;
  }
  bool aggregateBudgeted() const noexcept { return expectedBufferCharge_.budgeted(); }
  bool usesExpectedBufferBudget(const CpuTapeExpectedBufferBudget& budget) const noexcept {
    return expectedBufferCharge_.belongsTo(budget);
  }

private:
  friend CpuTapeResourceContextStatus buildCpuTapeResourceVersion(
      const CpuTapeResourceVersionInput&, const CpuTapeResourceObservation&,
      std::shared_ptr<const CpuTapeResourceVersion>&, CpuTapeResourceVersionLimits) noexcept;
  friend CpuTapeResourceContextStatus buildBudgetedCpuTapeResourceVersion(
      const CpuTapeResourceVersionInput&, const CpuTapeResourceObservation&,
      const std::shared_ptr<CpuTapeExpectedBufferBudget>&,
      std::shared_ptr<const CpuTapeResourceVersion>&, CpuTapeResourceVersionLimits) noexcept;
  static CpuTapeResourceContextStatus allocate(const CpuTapeResourceVersionInput&,
      const CpuTapeResourceObservation&, std::shared_ptr<const CpuTapeResourceVersion>&,
      CpuTapeExpectedBufferBudget::Charge) noexcept;
  CpuTapeResourceVersion() = default;
  std::uint32_t nativeKind_ = 0u;
  core::Handle nativeHandle_{};
  D9CWireObjectIdentity identity_{};
  std::uint64_t contentEpoch_ = 0u;
  // Reverse member destruction frees byte storage before credit can be reused.
  CpuTapeExpectedBufferBudget::Charge expectedBufferCharge_{};
  CpuTapeResourceObservation observation_{};
  // Historical value-only metadata. Context sharing preserves this original
  // observation, not backing lifetime or a reusable physical proof.
  std::optional<resources::BufferVersionObservation> localBufferFacts_{};
  std::vector<std::uint8_t> expectedBufferBytes_{};
};

// A context can cheaply share unchanged versions across records. Its bounds
// cover this entry vector only, not aggregate version bytes across contexts.
// Controlled V4 admission must separately require that every version belongs
// to its exact shared session budget; legacy versions remain per-version only.
class CpuTapeResourceContext {
public:
  ~CpuTapeResourceContext() = default;
  CpuTapeResourceContext(const CpuTapeResourceContext&) = delete;
  CpuTapeResourceContext& operator=(const CpuTapeResourceContext&) = delete;
  std::span<const std::shared_ptr<const CpuTapeResourceVersion>> entries() const noexcept {
    return entries_;
  }
  CpuTapeResourceContextPurpose purpose() const noexcept { return purpose_; }
  bool hasSemanticUniqueNativeAuthority() const noexcept {
    return purpose_ == CpuTapeResourceContextPurpose::SemanticUniqueNative;
  }
  // Native keys do not identify a captured wire alias. Missing OR ambiguous
  // lookups fail closed; equal seeds/epochs/receipts do not resolve ambiguity.
  std::shared_ptr<const CpuTapeResourceVersion> find(
      std::uint32_t kind, core::Handle handle) const noexcept {
    std::shared_ptr<const CpuTapeResourceVersion> found;
    for (const auto& version : entries_)
      if (version->nativeKind() == kind && version->nativeHandle() == handle) {
        if (found) return {};
        found = version;
      }
    return found;
  }
  std::shared_ptr<const CpuTapeResourceVersion> findIdentity(
      D9CWireObjectIdentity identity) const noexcept {
    std::shared_ptr<const CpuTapeResourceVersion> found;
    for (const auto& version : entries_) {
      const auto& candidate = version->identity();
      if (candidate.kind != identity.kind || candidate.generation != identity.generation ||
          candidate.objectId != identity.objectId) continue;
      if (found) return {};
      found = version;
    }
    return found;
  }

private:
  friend CpuTapeResourceContextStatus buildCpuTapeResourceContext(
      std::span<const std::shared_ptr<const CpuTapeResourceVersion>>,
      std::shared_ptr<const CpuTapeResourceContext>&, CpuTapeResourceContextLimits) noexcept;
  friend CpuTapeResourceContextStatus buildCpuTapeV4SurfaceAliasInventory(
      std::span<const std::shared_ptr<const CpuTapeResourceVersion>>,
      const std::shared_ptr<CpuTapeExpectedBufferBudget>&,
      std::shared_ptr<const CpuTapeResourceContext>&, CpuTapeResourceContextLimits) noexcept;
  CpuTapeResourceContext() = default;
  CpuTapeResourceContextPurpose purpose_ = CpuTapeResourceContextPurpose::SemanticUniqueNative;
  std::vector<std::shared_ptr<const CpuTapeResourceVersion>> entries_{};
};

namespace cpu_resource_context_detail {

inline CpuTapeResourceContextStatus validateVersionInput(
    const CpuTapeResourceVersionInput& input, const CpuTapeResourceObservation& observation,
    CpuTapeResourceVersionLimits limits) noexcept {
  using Status = CpuTapeResourceContextStatus;
  if (!observation.valid() || !input.nativeHandle || !input.contentEpoch ||
      input.nativeKind != observation.identity().kind) return Status::InvalidInput;
  const bool buffer = input.nativeKind == D9C_CHUNK_HANDLE_KIND_BUFFER;
  if (buffer
          ? !input.expectedBufferByteSize || input.expectedBufferByteSize != input.expectedBufferBytes.size()
          : input.expectedBufferByteSize != 0u || !input.expectedBufferBytes.empty())
    return Status::InvalidInput;
  if (input.localBufferFacts) {
    const auto& facts = *input.localBufferFacts;
    if (!buffer || !facts.observed() || facts.buffer != input.nativeHandle ||
        facts.byteSize != input.expectedBufferByteSize || !facts.contentRevision ||
        !facts.currentBacking.metalHandle || facts.currentBacking.byteSize != facts.byteSize)
      return Status::InvalidInput;
  }
  if (input.expectedBufferBytes.size() > limits.maxExpectedBufferBytes)
    return Status::CapacityExceeded;
  return Status::Accepted;
}

inline bool expectedByteLimitAdmitted(std::size_t limit) noexcept {
  return limit && limit <= static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) &&
      limit <= std::vector<std::uint8_t>{}.max_size();
}

} // namespace cpu_resource_context_detail

inline CpuTapeResourceContextStatus createCpuTapeExpectedBufferBudget(
    CpuTapeExpectedBufferBudgetLimits limits, std::shared_ptr<CpuTapeExpectedBufferBudget>& out) noexcept {
  using Status = CpuTapeResourceContextStatus;
  if (!cpu_resource_context_detail::expectedByteLimitAdmitted(limits.maxRetainedExpectedBufferBytes))
    return Status::InvalidInput;
  try {
    auto candidate = std::shared_ptr<CpuTapeExpectedBufferBudget>(
        new CpuTapeExpectedBufferBudget(limits.maxRetainedExpectedBufferBytes));
    out = std::move(candidate);
    return Status::Accepted;
  } catch (...) { return Status::AllocationFailed; }
}

inline CpuTapeResourceContextStatus CpuTapeResourceVersion::allocate(
    const CpuTapeResourceVersionInput& input, const CpuTapeResourceObservation& observation,
    std::shared_ptr<const CpuTapeResourceVersion>& out, CpuTapeExpectedBufferBudget::Charge charge) noexcept {
  using Status = CpuTapeResourceContextStatus;
  try {
    auto candidate = std::shared_ptr<CpuTapeResourceVersion>(new CpuTapeResourceVersion);
    candidate->expectedBufferCharge_ = std::move(charge);
    if (input.expectedBufferBytes.size() > candidate->expectedBufferBytes_.max_size())
      return Status::CapacityExceeded;
    candidate->nativeKind_ = input.nativeKind;
    candidate->nativeHandle_ = input.nativeHandle;
    candidate->identity_ = observation.identity();
    candidate->contentEpoch_ = input.contentEpoch;
    candidate->observation_ = observation;
    candidate->localBufferFacts_ = input.localBufferFacts;
    if (input.nativeKind == D9C_CHUNK_HANDLE_KIND_BUFFER)
      candidate->expectedBufferBytes_.assign(
          input.expectedBufferBytes.begin(), input.expectedBufferBytes.end());
    out = std::move(candidate);
    return Status::Accepted;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

// Legacy fixtures retain their original per-version-only behavior. This
// factory cannot establish a shared-session aggregate budget association.
inline CpuTapeResourceContextStatus buildCpuTapeResourceVersion(
    const CpuTapeResourceVersionInput& input, const CpuTapeResourceObservation& observation,
    std::shared_ptr<const CpuTapeResourceVersion>& out, CpuTapeResourceVersionLimits limits) noexcept {
  const auto status = cpu_resource_context_detail::validateVersionInput(input, observation, limits);
  if (status != CpuTapeResourceContextStatus::Accepted) return status;
  return CpuTapeResourceVersion::allocate(input, observation, out, {});
}

inline CpuTapeResourceContextStatus buildBudgetedCpuTapeResourceVersion(
    const CpuTapeResourceVersionInput& input, const CpuTapeResourceObservation& observation,
    const std::shared_ptr<CpuTapeExpectedBufferBudget>& budget,
    std::shared_ptr<const CpuTapeResourceVersion>& out, CpuTapeResourceVersionLimits limits) noexcept {
  using Status = CpuTapeResourceContextStatus;
  if (!budget || !cpu_resource_context_detail::expectedByteLimitAdmitted(limits.maxExpectedBufferBytes))
    return Status::InvalidInput;
  const auto status = cpu_resource_context_detail::validateVersionInput(input, observation, limits);
  if (status != Status::Accepted) return status;
  // Prior out remains alive during reserve/copy: a replacement must fit with
  // all existing versions, including any consumer-held reference to the old one.
  const auto reserved = budget->reserve(input.expectedBufferBytes.size());
  if (reserved != Status::Accepted) return reserved;
  return CpuTapeResourceVersion::allocate(input, observation, out,
      CpuTapeExpectedBufferBudget::Charge(budget, input.expectedBufferBytes.size()));
}

inline CpuTapeResourceContextStatus buildCpuTapeResourceContext(
    std::span<const std::shared_ptr<const CpuTapeResourceVersion>> entries,
    std::shared_ptr<const CpuTapeResourceContext>& out,
    CpuTapeResourceContextLimits limits) noexcept {
  using Status = CpuTapeResourceContextStatus;
  if (limits.maxEntries > std::numeric_limits<std::uint32_t>::max() - 1u ||
      entries.size() > limits.maxEntries) return Status::CapacityExceeded;
  for (std::size_t index = 0u; index < entries.size(); ++index) {
    if (!entries[index]) return Status::InvalidInput;
    for (std::size_t prior = 0u; prior < index; ++prior)
      if (entries[prior]->nativeKind() == entries[index]->nativeKind() &&
          entries[prior]->nativeHandle() == entries[index]->nativeHandle())
        return Status::Conflict;
  }
  try {
    auto candidate = std::shared_ptr<CpuTapeResourceContext>(new CpuTapeResourceContext);
    if (entries.size() > candidate->entries_.max_size()) return Status::CapacityExceeded;
    if (!entries.empty()) candidate->entries_.assign(entries.begin(), entries.end());
    out = std::move(candidate);
    return Status::Accepted;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

// Explicit V4 METADATA inventory only. Distinct captured SURFACE wrappers can
// intentionally own the same native surface. Preserve every complete wire
// identity, logical epoch and normalized observation independently; do not
// canonicalize aliases or infer equality from shared native keys/seed bytes.
// BUFFER/TEXTURE native uniqueness remains required. The caller authenticates
// actual wrappers and aliasing; this bounded owner grants no semantic/GPU proof.
inline CpuTapeResourceContextStatus buildCpuTapeV4SurfaceAliasInventory(
    std::span<const std::shared_ptr<const CpuTapeResourceVersion>> entries,
    const std::shared_ptr<CpuTapeExpectedBufferBudget>& budget,
    std::shared_ptr<const CpuTapeResourceContext>& out,
    CpuTapeResourceContextLimits limits) noexcept {
  using Status = CpuTapeResourceContextStatus;
  if (!budget) return Status::InvalidInput;
  if (limits.maxEntries > std::numeric_limits<std::uint32_t>::max() - 1u ||
      entries.size() > limits.maxEntries) return Status::CapacityExceeded;
  for (std::size_t index = 0u; index < entries.size(); ++index) {
    const auto& version = entries[index];
    if (!version || !version->usesExpectedBufferBudget(*budget)) return Status::InvalidInput;
    const auto kind = version->nativeKind();
    if (kind != D9C_CHUNK_HANDLE_KIND_BUFFER && kind != D9C_CHUNK_HANDLE_KIND_TEXTURE &&
        kind != D9C_CHUNK_HANDLE_KIND_SURFACE) return Status::InvalidInput;
    for (std::size_t prior = 0u; prior < index; ++prior) {
      const auto& old = entries[prior];
      const auto& identity = version->identity();
      const auto& oldIdentity = old->identity();
      if (identity.kind == oldIdentity.kind && identity.generation == oldIdentity.generation &&
          identity.objectId == oldIdentity.objectId) return Status::Conflict;
      if (old->nativeKind() == kind && old->nativeHandle() == version->nativeHandle() &&
          kind != D9C_CHUNK_HANDLE_KIND_SURFACE) return Status::Conflict;
    }
  }
  try {
    auto candidate = std::shared_ptr<CpuTapeResourceContext>(new CpuTapeResourceContext);
    if (entries.size() > candidate->entries_.max_size()) return Status::CapacityExceeded;
    candidate->purpose_ = CpuTapeResourceContextPurpose::V4SurfaceAliasInventory;
    if (!entries.empty()) candidate->entries_.assign(entries.begin(), entries.end());
    out = std::move(candidate);
    return Status::Accepted;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

// Cold per-run record ledger. The producer must bind the genuine original
// locator BEFORE dispatch; this class cannot authenticate a locator or prove
// that timing. It never follows a handle to a wrapper, Pool record or pointer.
// Write-once contexts survive producer mutations and later source publication.
class CpuTapeRecordContextStore {
public:
  explicit CpuTapeRecordContextStore(CpuTapeRecordContextLimits limits = {}) noexcept
      : limits_(limits) {}
  CpuTapeRecordContextStore(const CpuTapeRecordContextStore&) = delete;
  CpuTapeRecordContextStore& operator=(const CpuTapeRecordContextStore&) = delete;

  CpuTapeResourceContextStatus bind(render::TapeEmissionRecordLocator locator,
      const std::shared_ptr<const CpuTapeResourceContext>& context) noexcept {
    using Status = CpuTapeResourceContextStatus;
    if (!validLocator(locator) || !context ||
        limits_.maxRecords > std::numeric_limits<std::uint32_t>::max() - 1u)
      return Status::InvalidInput;
    try {
      std::lock_guard lock(mutex_);
      for (const auto& record : records_) {
        if (record.locator == locator)
          return record.context == context ? Status::Accepted : Status::Conflict;
      }
      if (records_.size() >= limits_.maxRecords || records_.size() == records_.max_size())
        return Status::CapacityExceeded;
      records_.push_back({locator, context});
      return Status::Accepted;
    } catch (...) {
      return Status::AllocationFailed;
    }
  }

  std::shared_ptr<const CpuTapeResourceContext> lookup(
      render::TapeEmissionRecordLocator locator) const noexcept {
    if (!validLocator(locator)) return {};
    try {
      std::lock_guard lock(mutex_);
      for (const auto& record : records_)
        if (record.locator == locator) return record.context;
    } catch (...) {}
    return {};
  }

  std::size_t size() const noexcept {
    try {
      std::lock_guard lock(mutex_);
      return records_.size();
    } catch (...) {
      return 0u;
    }
  }

private:
  static bool validLocator(render::TapeEmissionRecordLocator locator) noexcept {
    return locator.eventOrdinal != 0u && locator.recordIndex != std::numeric_limits<std::uint32_t>::max();
  }
  struct Record {
    render::TapeEmissionRecordLocator locator{};
    std::shared_ptr<const CpuTapeResourceContext> context{};
  };
  CpuTapeRecordContextLimits limits_{};
  mutable std::mutex mutex_{};
  std::vector<Record> records_{};
};

} // namespace dxmt9::test
