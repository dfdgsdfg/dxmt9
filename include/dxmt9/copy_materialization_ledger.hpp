#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace dxmt9::core {

// Stable copy/materialization classes. Keep names and ordering append-only so
// experiment output remains comparable across builds.
enum class CopyMaterializationClass : std::uint8_t {
  PeStateShadow,
  PeWireFinal,
  PeBuilderTemporary,
  PeSealRecords,
  PeSealHandles,
  PeSealPayload,
  BridgeRawOwnership,
  ReplaySubmissionCarrierMaterialization,
  ReplaySubmissionCarrierCopy,
  QueueFinalSlotAppend,
  GpuUploadCopy,
  GpuSharedMaterialization,
  ArenaByteCopy,
  // Semantic events describe an ownership/materialization boundary without
  // claiming that bytes moved.  Keep these rows separate from `record()` so
  // reports do not accidentally count a reservation as a memcpy.
  MutationStaging,
  UpScratch,
  PeSectionAppend,
  // A segmented command chunk owns already-final role regions. This row
  // tracks their logical lifetime without misreporting a byte copy.
  PeWireView,
  // Typed producer values become rollback-capable, chunk-owned semantic
  // storage before either contiguous or segmented final-wire emission.
  PeSemanticOwnerAdmission,
  Count,
};

// Registries are binary-local by construction: each image gets its own
// function-local instance, while the owner axis keeps PE and Unix rows
// separate even when a native test links both sides into one image.
enum class CopyMaterializationOwner : std::uint8_t {
  Pe,
  Unix,
};

[[nodiscard]] constexpr const char* copyMaterializationOwnerName(
    CopyMaterializationOwner owner) noexcept {
  return owner == CopyMaterializationOwner::Pe ? "pe" : "unix";
}

[[nodiscard]] constexpr const char* copyMaterializationClassName(
    CopyMaterializationClass materializationClass) noexcept {
  switch (materializationClass) {
  case CopyMaterializationClass::PeStateShadow:
    return "materialize.pe.state-shadow";
  case CopyMaterializationClass::PeWireFinal:
    return "materialize.pe.wire-final";
  case CopyMaterializationClass::PeBuilderTemporary:
    return "materialize.pe.builder-temporary";
  case CopyMaterializationClass::PeSealRecords:
    return "copy.pe.seal-records";
  case CopyMaterializationClass::PeSealHandles:
    return "copy.pe.seal-handles";
  case CopyMaterializationClass::PeSealPayload:
    return "copy.pe.seal-payload";
  case CopyMaterializationClass::BridgeRawOwnership:
    return "copy.bridge.raw-owned";
  case CopyMaterializationClass::ReplaySubmissionCarrierMaterialization:
    return "materialize.replay-submission-carrier";
  case CopyMaterializationClass::ReplaySubmissionCarrierCopy:
    return "copy.replay.submission-carrier";
  case CopyMaterializationClass::QueueFinalSlotAppend:
    return "materialize.queue-final";
  case CopyMaterializationClass::GpuUploadCopy:
    return "copy.gpu.upload";
  case CopyMaterializationClass::GpuSharedMaterialization:
    return "materialize.gpu.shared";
  case CopyMaterializationClass::ArenaByteCopy:
    return "copy.arena.bytes";
  case CopyMaterializationClass::MutationStaging:
    return "materialize.mutation.staging";
  case CopyMaterializationClass::UpScratch:
    return "materialize.up.scratch";
  case CopyMaterializationClass::PeSectionAppend:
    return "materialize.pe.section-append";
  case CopyMaterializationClass::PeWireView:
    return "view.pe.wire-final";
  case CopyMaterializationClass::PeSemanticOwnerAdmission:
    return "materialize.pe.semantic-owner-admission";
  case CopyMaterializationClass::Count:
    break;
  }
  return "unknown";
}

enum class CopyMaterializationClassification : std::uint8_t {
  Necessary,
  Removable,
};

[[nodiscard]] constexpr const char* copyMaterializationClassificationName(
    CopyMaterializationClassification classification) noexcept {
  return classification == CopyMaterializationClassification::Necessary
             ? "necessary"
             : "removable";
}

// Keep these reasons aligned with the normative identity table in
// specs/archicture/spec.md §2.3.  They are part of the stable report contract,
// not free-form diagnostic prose.
[[nodiscard]] constexpr const char* copyMaterializationOwnershipAbiReason(
  CopyMaterializationClass materializationClass) noexcept {
  switch (materializationClass) {
  case CopyMaterializationClass::PeStateShadow:
    return "producer-visible-d3d9-state-semantics";
  case CopyMaterializationClass::PeWireFinal:
    return "pointer-free-pe-ownership";
  case CopyMaterializationClass::PeBuilderTemporary:
    return "final-wire-layout-target-owner";
  case CopyMaterializationClass::PeSealRecords:
    return "final-offsets-known-before-construction";
  case CopyMaterializationClass::PeSealHandles:
    return "final-handle-capacity-reservable-transactionally";
  case CopyMaterializationClass::PeSealPayload:
    return "typed-producers-final-payload-range";
  case CopyMaterializationClass::BridgeRawOwnership:
    return "process-abi-ownership-no-shared-ownership-abi";
  case CopyMaterializationClass::ReplaySubmissionCarrierMaterialization:
    return "bounded-planning-final-queue-storage";
  case CopyMaterializationClass::ReplaySubmissionCarrierCopy:
    return "final-queue-destination-known-after-bounded-planning";
  case CopyMaterializationClass::QueueFinalSlotAppend:
    return "immutable-queue-ownership";
  case CopyMaterializationClass::GpuUploadCopy:
    return "cpu-allocation-not-gpu-readable";
  case CopyMaterializationClass::GpuSharedMaterialization:
    return "first-gpu-readable-owned-representation";
  case CopyMaterializationClass::ArenaByteCopy:
    return "queue-visible-arena-representation";
  case CopyMaterializationClass::MutationStaging:
    return "asynchronous-offload-ownership";
  case CopyMaterializationClass::UpScratch:
    return "pointer-free-wire-construction-transactional";
  case CopyMaterializationClass::PeSectionAppend:
    return "pe-section-ownership-before-sealing";
  case CopyMaterializationClass::PeWireView:
    return "direct-final-region-view-ownership";
  case CopyMaterializationClass::PeSemanticOwnerAdmission:
    return "rollback-capable-typed-semantic-ownership";
  case CopyMaterializationClass::Count:
    break;
  }
  return "unknown-copy-materialization-class";
}

[[nodiscard]] constexpr CopyMaterializationClassification
copyMaterializationClassification(
    CopyMaterializationClass materializationClass) noexcept {
  switch (materializationClass) {
  case CopyMaterializationClass::PeBuilderTemporary:
  case CopyMaterializationClass::PeSealRecords:
  case CopyMaterializationClass::PeSealHandles:
  case CopyMaterializationClass::PeSealPayload:
  case CopyMaterializationClass::ReplaySubmissionCarrierMaterialization:
  case CopyMaterializationClass::ReplaySubmissionCarrierCopy:
    return CopyMaterializationClassification::Removable;
  case CopyMaterializationClass::PeStateShadow:
  case CopyMaterializationClass::PeWireFinal:
  case CopyMaterializationClass::BridgeRawOwnership:
  case CopyMaterializationClass::QueueFinalSlotAppend:
  case CopyMaterializationClass::GpuUploadCopy:
  case CopyMaterializationClass::GpuSharedMaterialization:
  case CopyMaterializationClass::ArenaByteCopy:
  case CopyMaterializationClass::MutationStaging:
  case CopyMaterializationClass::UpScratch:
  case CopyMaterializationClass::PeSectionAppend:
  case CopyMaterializationClass::PeWireView:
  case CopyMaterializationClass::PeSemanticOwnerAdmission:
  case CopyMaterializationClass::Count:
    return CopyMaterializationClassification::Necessary;
  }
  return CopyMaterializationClassification::Necessary;
}

struct CopyMaterializationDescriptor {
  const char* identity = "unknown";
  const char* classificationName = "necessary";
  CopyMaterializationClassification classification =
      CopyMaterializationClassification::Necessary;
  const char* ownershipAbiReason = "unknown-copy-materialization-class";
};

[[nodiscard]] constexpr CopyMaterializationDescriptor
copyMaterializationDescriptor(
    CopyMaterializationClass materializationClass) noexcept {
  return {
      .identity = copyMaterializationClassName(materializationClass),
      .classificationName = copyMaterializationClassificationName(
          copyMaterializationClassification(materializationClass)),
      .classification =
          copyMaterializationClassification(materializationClass),
      .ownershipAbiReason =
          copyMaterializationOwnershipAbiReason(materializationClass),
  };
}

struct CopyMaterializationSnapshot {
  std::uint64_t calls = 0u;
  std::uint64_t bytes = 0u;
  std::uint64_t inclusiveNanoseconds = 0u;
  std::uint64_t retainedBytes = 0u;
  std::uint64_t retainedBytesPeak = 0u;
  std::uint64_t semanticCalls = 0u;
  std::uint64_t semanticBytes = 0u;
};

// Report consumers share this pure activity predicate so PE and Unix retain
// identical row filtering without coupling their logging implementations.
[[nodiscard]] constexpr bool copyMaterializationSnapshotHasActivity(
    const CopyMaterializationSnapshot& snapshot) noexcept {
  return snapshot.calls != 0u || snapshot.semanticCalls != 0u ||
         snapshot.retainedBytes != 0u;
}

class CopyMaterializationLedger final {
public:
  static constexpr std::size_t kClassCount =
      static_cast<std::size_t>(CopyMaterializationClass::Count);

  void record(CopyMaterializationClass materializationClass,
              std::size_t bytes, std::uint64_t nanoseconds = 0u) noexcept {
    auto& row = rows_[index(materializationClass)];
    row.calls.fetch_add(1u, std::memory_order_relaxed);
    row.bytes.fetch_add(bytes, std::memory_order_relaxed);
    row.inclusiveNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
  }

  void recordMaterialization(CopyMaterializationClass materializationClass,
                              std::size_t bytes = 0u) noexcept {
    auto& row = rows_[index(materializationClass)];
    row.semanticCalls.fetch_add(1u, std::memory_order_relaxed);
    row.semanticBytes.fetch_add(bytes, std::memory_order_relaxed);
  }

  void retain(CopyMaterializationClass materializationClass,
              std::size_t bytes) noexcept {
    if (bytes == 0u) return;
    auto& row = rows_[index(materializationClass)];
    const auto current =
        row.retainedBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    auto peak = row.retainedBytesPeak.load(std::memory_order_relaxed);
    while (peak < current &&
           !row.retainedBytesPeak.compare_exchange_weak(
               peak, current, std::memory_order_relaxed,
               std::memory_order_relaxed)) {}
  }

  void release(CopyMaterializationClass materializationClass,
               std::size_t bytes) noexcept {
    if (bytes == 0u) return;
    auto& retained = rows_[index(materializationClass)].retainedBytes;
    auto current = retained.load(std::memory_order_relaxed);
    while (current != 0u) {
      const auto next = current > bytes ? current - bytes : 0u;
      if (retained.compare_exchange_weak(current, next,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
        break;
      }
    }
  }

  [[nodiscard]] CopyMaterializationSnapshot snapshot(
      CopyMaterializationClass materializationClass) const noexcept {
    const auto& row = rows_[index(materializationClass)];
    return {
        .calls = row.calls.load(std::memory_order_relaxed),
        .bytes = row.bytes.load(std::memory_order_relaxed),
        .inclusiveNanoseconds =
            row.inclusiveNanoseconds.load(std::memory_order_relaxed),
        .retainedBytes = row.retainedBytes.load(std::memory_order_relaxed),
        .retainedBytesPeak =
            row.retainedBytesPeak.load(std::memory_order_relaxed),
        .semanticCalls = row.semanticCalls.load(std::memory_order_relaxed),
        .semanticBytes = row.semanticBytes.load(std::memory_order_relaxed),
    };
  }

private:
  struct Row {
    std::atomic<std::uint64_t> calls{0u};
    std::atomic<std::uint64_t> bytes{0u};
    std::atomic<std::uint64_t> inclusiveNanoseconds{0u};
    std::atomic<std::uint64_t> retainedBytes{0u};
    std::atomic<std::uint64_t> retainedBytesPeak{0u};
    std::atomic<std::uint64_t> semanticCalls{0u};
    std::atomic<std::uint64_t> semanticBytes{0u};
  };

  static constexpr std::size_t index(
      CopyMaterializationClass materializationClass) noexcept {
    return static_cast<std::size_t>(materializationClass);
  }

  std::array<Row, kClassCount> rows_{};
};

class CopyMaterializationLedgerRegistry final {
 public:
  CopyMaterializationLedger& ledger(CopyMaterializationOwner owner) noexcept {
    return owner == CopyMaterializationOwner::Pe ? pe_ : unix_;
  }

 private:
  CopyMaterializationLedger pe_{};
  CopyMaterializationLedger unix_{};
};

// Function-local storage remains stable for the lifetime of the binary and
// cannot be replaced by a second queue/device.  The environment is sampled
// once, so the disabled path does not touch ledger atomics or clocks.
[[nodiscard]] inline CopyMaterializationLedgerRegistry&
copyMaterializationLedgerRegistry() noexcept {
  static CopyMaterializationLedgerRegistry registry;
  return registry;
}

[[nodiscard]] inline bool copyMaterializationLedgerEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_PERF_COPY_MATERIALIZATION_LEDGER");
    return value && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

[[nodiscard]] constexpr std::size_t copyMaterializationOwnerIndex(
    CopyMaterializationOwner owner) noexcept {
  return static_cast<std::size_t>(owner);
}

// The disabled production path resolves one owner-qualified cached pointer.
// Keep registry construction behind the cached gate: a process with the
// observer off must not construct either atomic ledger or read the environment
// at every copy site.
[[nodiscard]] inline CopyMaterializationLedger* const*
productionCopyMaterializationLedgers() noexcept {
  static const auto ledgers = [] {
    std::array<CopyMaterializationLedger*, 2> result{};
    if (!copyMaterializationLedgerEnabled()) {
      return result;
    }
    auto& registry = copyMaterializationLedgerRegistry();
    result[copyMaterializationOwnerIndex(CopyMaterializationOwner::Pe)] =
        &registry.ledger(CopyMaterializationOwner::Pe);
    result[copyMaterializationOwnerIndex(CopyMaterializationOwner::Unix)] =
        &registry.ledger(CopyMaterializationOwner::Unix);
    return result;
  }();
  return ledgers.data();
}

// Native tests may install a deterministic sink for one synchronous scope;
// production never uses this override. The effective owner slots are seeded
// from the cached production pair, so the disabled path performs one pointer
// read and leaves the existing call-site null branch to elide diagnostics.
inline thread_local std::array<CopyMaterializationLedger*, 2>
    gCopyMaterializationEffectiveOwnerLedgers = [] {
      const auto* production = productionCopyMaterializationLedgers();
      return std::array<CopyMaterializationLedger*, 2>{
          production[copyMaterializationOwnerIndex(CopyMaterializationOwner::Pe)],
          production[copyMaterializationOwnerIndex(
              CopyMaterializationOwner::Unix)]};
    }();

[[nodiscard]] inline CopyMaterializationLedger*
activeCopyMaterializationLedger(
    CopyMaterializationOwner owner) noexcept {
  return gCopyMaterializationEffectiveOwnerLedgers[
      copyMaterializationOwnerIndex(owner)];
}

class ScopedCopyMaterializationLedger final {
public:
  ScopedCopyMaterializationLedger(CopyMaterializationOwner owner,
                                  CopyMaterializationLedger& ledger) noexcept
      : owner_(owner),
        previous_(gCopyMaterializationEffectiveOwnerLedgers[
            copyMaterializationOwnerIndex(owner)]) {
    gCopyMaterializationEffectiveOwnerLedgers[
        copyMaterializationOwnerIndex(owner)] = &ledger;
  }

  ~ScopedCopyMaterializationLedger() {
    gCopyMaterializationEffectiveOwnerLedgers[
        copyMaterializationOwnerIndex(owner_)] = previous_;
  }

  ScopedCopyMaterializationLedger(const ScopedCopyMaterializationLedger&) =
      delete;
  ScopedCopyMaterializationLedger& operator=(
      const ScopedCopyMaterializationLedger&) = delete;

private:
  CopyMaterializationOwner owner_ = CopyMaterializationOwner::Pe;
  CopyMaterializationLedger* previous_ = nullptr;
};

class CopyMaterializationEvent final {
public:
  CopyMaterializationEvent(CopyMaterializationLedger* ledger,
                           CopyMaterializationClass materializationClass,
                           std::size_t bytes) noexcept
      : ledger_(ledger), materializationClass_(materializationClass),
        bytes_(bytes) {
    if (ledger_) started_ = Clock::now();
  }

  ~CopyMaterializationEvent() {
    commit();
  }

  void setBytes(std::size_t bytes) noexcept { bytes_ = bytes; }
  void cancel() noexcept { ledger_ = nullptr; }
  void commit() noexcept {
    if (!ledger_) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - started_);
    ledger_->record(materializationClass_, bytes_,
                    static_cast<std::uint64_t>(elapsed.count()));
    ledger_ = nullptr;
  }

  CopyMaterializationEvent(const CopyMaterializationEvent&) = delete;
  CopyMaterializationEvent& operator=(const CopyMaterializationEvent&) = delete;

private:
  using Clock = std::chrono::steady_clock;
  CopyMaterializationLedger* ledger_ = nullptr;
  CopyMaterializationClass materializationClass_{};
  std::size_t bytes_ = 0u;
  Clock::time_point started_{};
};

} // namespace dxmt9::core
