#pragma once

// Calibrated, opt-in timing for the PE semantic owner.  This observer is
// intentionally separate from CopyMaterializationLedger: a phase is CPU
// attribution, not a copy/materialization class.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace dxmt9::d3d9::pe {

enum class PeSemanticOwnerPhase : std::uint8_t {
  TryAppendOwnedRecord,
  PrepareAdmission,
  FixedDirectPinCopy,
  SparseVariableCopy,
  CanonicalMaterializationMetrics,
  PendingDeltaSettlement,
  Rollback,
  EmitExactFixedBuildPlan,
  EmitExactFixedRoleCopy,
  EmitExactFixed,
  EmitSegmentedExternalCopy,
  EmitSegmentedExternalRoleCopy,
  EmitSegmentedAliasView,
  SettleClear,
  SettleRetainerEpoch,
  SettleOwnerReset,
  Count,
};

[[nodiscard]] constexpr const char* peSemanticOwnerPhaseName(
    PeSemanticOwnerPhase phase) noexcept {
  switch (phase) {
  case PeSemanticOwnerPhase::TryAppendOwnedRecord: return "tryAppendOwnedRecord";
  case PeSemanticOwnerPhase::PrepareAdmission: return "prepareAdmission";
  case PeSemanticOwnerPhase::FixedDirectPinCopy: return "fixedDirectPinCopy";
  case PeSemanticOwnerPhase::SparseVariableCopy: return "sparseVariableCopy";
  case PeSemanticOwnerPhase::CanonicalMaterializationMetrics:
    return "canonicalMaterializationMetrics";
  case PeSemanticOwnerPhase::PendingDeltaSettlement:
    return "pendingDeltaSettlement";
  case PeSemanticOwnerPhase::Rollback: return "rollback";
  case PeSemanticOwnerPhase::EmitExactFixedBuildPlan:
    return "emitExactFixedBuildPlan";
  case PeSemanticOwnerPhase::EmitExactFixedRoleCopy:
    return "emitExactFixedRoleCopy";
  case PeSemanticOwnerPhase::EmitExactFixed: return "emitExactFixed";
  case PeSemanticOwnerPhase::EmitSegmentedExternalCopy:
    return "emitSegmentedExternalCopy";
  case PeSemanticOwnerPhase::EmitSegmentedExternalRoleCopy:
    return "emitSegmentedExternalRoleCopy";
  case PeSemanticOwnerPhase::EmitSegmentedAliasView:
    return "emitSegmentedAliasView";
  case PeSemanticOwnerPhase::SettleClear: return "settleClear";
  case PeSemanticOwnerPhase::SettleRetainerEpoch: return "settleRetainerEpoch";
  case PeSemanticOwnerPhase::SettleOwnerReset: return "settleOwnerReset";
  case PeSemanticOwnerPhase::Count: break;
  }
  return "unknown";
}

struct PeSemanticOwnerPhaseMetric {
  std::uint64_t events = 0u;
  std::uint64_t samples = 0u;
  std::uint64_t totalNs = 0u;
  std::uint64_t maxNs = 0u;
};

enum class PeSemanticOwnerOutcome : std::uint8_t {
  Accepted,
  Unavailable,
  Capacity,
  Malformed,
  Header,
  Fixed,
  DirectPins,
  Sparse,
  VariablePayload,
  EmissionMetrics,
  Settlement,
  Other,
  Count,
};

enum class PeSemanticOwnerSettlementFamily : std::uint8_t {
  SparseDraw,
  SparseIndexedDraw,
  SparseApply,
  Constant,
  OtherGeneric,
  Count,
};

enum class PeSemanticOwnerAdmissionFamily : std::uint8_t {
  Compact,
  Generic,
  Constant,
  Count,
};

[[nodiscard]] constexpr const char* peSemanticOwnerAdmissionFamilyName(
    PeSemanticOwnerAdmissionFamily family) noexcept {
  switch (family) {
  case PeSemanticOwnerAdmissionFamily::Compact: return "compact";
  case PeSemanticOwnerAdmissionFamily::Generic: return "generic";
  case PeSemanticOwnerAdmissionFamily::Constant: return "constant";
  case PeSemanticOwnerAdmissionFamily::Count: break;
  }
  return "unknown";
}

// Cold-adapter output reconstructed after successful preparation. It is not a
// production witness and never crosses the owner transaction boundary.
struct PeSemanticOwnerAdmissionDistribution {
  std::uint64_t bindingOccurrences = 0u;
  std::uint64_t refOccurrences = 0u;
  std::uint64_t uniqueIdentities = 0u;
  std::uint64_t linearComparisons = 0u;
};

struct PeSemanticOwnerAdmissionMetric {
  std::uint64_t attempts = 0u;
  std::uint64_t sampledAttempts = 0u;
  std::uint64_t accepted = 0u;
  std::uint64_t rejected = 0u;
  std::uint64_t sampledAccepted = 0u;
  std::uint64_t sampledRejected = 0u;
  std::uint64_t sampledPrepared = 0u;
  std::uint64_t sampledUnmeasuredFailures = 0u;
  std::uint64_t sampledZeroWork = 0u;
  std::uint64_t sampledBindingOccurrences = 0u;
  std::uint64_t sampledRefOccurrences = 0u;
  std::uint64_t sampledUniqueIdentities = 0u;
  std::uint64_t sampledLinearComparisons = 0u;
  std::uint64_t maxBindingOccurrences = 0u;
  std::uint64_t maxRefOccurrences = 0u;
  std::uint64_t maxUniqueIdentities = 0u;
  std::uint64_t maxLinearComparisons = 0u;
};

[[nodiscard]] constexpr const char* peSemanticOwnerSettlementFamilyName(
    PeSemanticOwnerSettlementFamily family) noexcept {
  switch (family) {
  case PeSemanticOwnerSettlementFamily::SparseDraw:
    return "sparse_draw";
  case PeSemanticOwnerSettlementFamily::SparseIndexedDraw:
    return "sparse_indexed_draw";
  case PeSemanticOwnerSettlementFamily::SparseApply:
    return "sparse_apply";
  case PeSemanticOwnerSettlementFamily::Constant:
    return "constant";
  case PeSemanticOwnerSettlementFamily::OtherGeneric:
    return "other_generic";
  case PeSemanticOwnerSettlementFamily::Count:
    break;
  }
  return "unknown";
}

struct PeSemanticOwnerSettlementMetric {
  std::uint64_t events = 0u;
  std::uint64_t samples = 0u;
  std::uint64_t totalNs = 0u;
  std::uint64_t maxNs = 0u;
};

struct PeSemanticOwnerPhaseStats {
  static constexpr std::size_t kPhaseCount =
      static_cast<std::size_t>(PeSemanticOwnerPhase::Count);
  static constexpr std::size_t kOutcomeCount =
      static_cast<std::size_t>(PeSemanticOwnerOutcome::Count);

  std::uint32_t decimationN = 0u;
  std::uint64_t appendEvents = 0u;
  std::uint64_t appendSampled = 0u;
  std::uint64_t operationEvents = 0u;
  std::uint64_t operationSampled = 0u;
  std::uint64_t nullCalibrationSamples = 0u;
  std::uint64_t nullCalibrationNs = 0u;
  std::array<std::uint64_t, kOutcomeCount> outcomes{};
  std::array<PeSemanticOwnerPhaseMetric, kPhaseCount> phases{};
  static constexpr std::size_t kSettlementFamilyCount =
      static_cast<std::size_t>(PeSemanticOwnerSettlementFamily::Count);
  std::array<PeSemanticOwnerSettlementMetric, kSettlementFamilyCount>
      settlements{};
  static constexpr std::size_t kAdmissionFamilyCount =
      static_cast<std::size_t>(PeSemanticOwnerAdmissionFamily::Count);
  std::array<PeSemanticOwnerAdmissionMetric, kAdmissionFamilyCount>
      admissions{};
};

class PeSemanticOwnerPhaseObserver final {
  using Clock = std::chrono::steady_clock;

 public:
  explicit PeSemanticOwnerPhaseObserver(std::uint32_t decimationN = 0u)
      : stats_{.decimationN = decimationN} {}

  PeSemanticOwnerPhaseObserver(const PeSemanticOwnerPhaseObserver&) = delete;
  PeSemanticOwnerPhaseObserver& operator=(
      const PeSemanticOwnerPhaseObserver&) = delete;

  class Scope final {
   public:
    Scope() noexcept = default;
    Scope(PeSemanticOwnerPhaseObserver* observer,
          PeSemanticOwnerPhase phase, bool parent = false,
          bool appendParent = true,
          PeSemanticOwnerSettlementFamily settlementFamily =
              PeSemanticOwnerSettlementFamily::Count) noexcept
        : observer_(observer), phase_(phase), parent_(parent),
          appendParent_(appendParent), settlementFamily_(settlementFamily) {
      if (!observer_) return;
      if (parent_) {
        priorActiveParentSampled_ = observer_->activeParentSampled_;
        observer_->activeParentSampled_ = false;
      }
      auto& metric = observer_->stats_.phases[static_cast<std::size_t>(phase_)];
      ++metric.events;
      const auto familyIndex = static_cast<std::size_t>(settlementFamily_);
      if (familyIndex < PeSemanticOwnerPhaseStats::kSettlementFamilyCount) {
        settlementMetric_ = &observer_->stats_.settlements[familyIndex];
        ++settlementMetric_->events;
      }
      if (parent_) {
        if (appendParent) {
          ++observer_->stats_.appendEvents;
        } else {
          ++observer_->stats_.operationEvents;
        }
        const auto n = observer_->stats_.decimationN;
        const auto ordinal = appendParent
                                 ? observer_->stats_.appendEvents
                                 : metric.events;
        sampled_ = n != 0u && (ordinal % n) == 0u;
        observer_->activeParentSampled_ = sampled_;
        if (!sampled_) return;
        const auto n0 = Clock::now();
        const auto n1 = Clock::now();
        ++observer_->stats_.nullCalibrationSamples;
        observer_->stats_.nullCalibrationNs += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(n1 - n0)
                .count());
        if (appendParent) {
          ++observer_->stats_.appendSampled;
        } else {
          ++observer_->stats_.operationSampled;
        }
        started_ = Clock::now();
      } else if (parentScopeSampled()) {
        sampled_ = true;
        started_ = Clock::now();
      }
    }

    ~Scope() { stop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&& other) noexcept
        : observer_(other.observer_), phase_(other.phase_),
          parent_(other.parent_), appendParent_(other.appendParent_),
          settlementFamily_(other.settlementFamily_),
          priorActiveParentSampled_(other.priorActiveParentSampled_),
          sampled_(other.sampled_),
          started_(other.started_), settlementMetric_(other.settlementMetric_) {
      other.observer_ = nullptr;
      other.parent_ = false;
      other.settlementMetric_ = nullptr;
    }

    void stop() noexcept {
      if (!observer_) return;
      if (sampled_) {
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - started_)
                .count());
        auto& metric = observer_->stats_.phases[static_cast<std::size_t>(phase_)];
        ++metric.samples;
        metric.totalNs += elapsed;
        if (elapsed > metric.maxNs) metric.maxNs = elapsed;
        if (settlementMetric_) {
          ++settlementMetric_->samples;
          settlementMetric_->totalNs += elapsed;
          if (elapsed > settlementMetric_->maxNs)
            settlementMetric_->maxNs = elapsed;
        }
      }
      if (parent_) {
        observer_->activeParentSampled_ = priorActiveParentSampled_;
      }
      observer_ = nullptr;
    }

    [[nodiscard]] bool sampled() const noexcept { return sampled_; }

   private:
    bool parentScopeSampled() const noexcept {
      return observer_->activeParentSampled_;
    }

    PeSemanticOwnerPhaseObserver* observer_ = nullptr;
    PeSemanticOwnerPhase phase_ = PeSemanticOwnerPhase::TryAppendOwnedRecord;
    bool parent_ = false;
    bool appendParent_ = true;
    PeSemanticOwnerSettlementFamily settlementFamily_ =
        PeSemanticOwnerSettlementFamily::Count;
    bool priorActiveParentSampled_ = false;
    bool sampled_ = false;
    Clock::time_point started_{};
    PeSemanticOwnerSettlementMetric* settlementMetric_ = nullptr;
  };

  [[nodiscard]] Scope beginAppend() noexcept {
    return Scope(this, PeSemanticOwnerPhase::TryAppendOwnedRecord, true);
  }

  [[nodiscard]] Scope beginOperation(PeSemanticOwnerPhase phase) noexcept {
    return Scope(this, phase, true, false);
  }

  [[nodiscard]] Scope child(PeSemanticOwnerPhase phase) noexcept {
    return Scope(this, phase);
  }

  [[nodiscard]] Scope beginSettlement(
      PeSemanticOwnerSettlementFamily family) noexcept {
    return Scope(this, PeSemanticOwnerPhase::PendingDeltaSettlement, false,
                 true, family);
  }

  void recordOutcome(PeSemanticOwnerOutcome outcome) noexcept {
    const auto index = static_cast<std::size_t>(outcome);
    if (index < stats_.outcomes.size()) ++stats_.outcomes[index];
  }

  void recordAdmissionAttempt(PeSemanticOwnerAdmissionFamily family,
                              bool sampled, bool prepared, bool accepted,
                              bool measured) noexcept {
    const auto index = static_cast<std::size_t>(family);
    if (index >= PeSemanticOwnerPhaseStats::kAdmissionFamilyCount) return;
    auto& metric = stats_.admissions[index];
    ++metric.attempts;
    if (accepted) {
      ++metric.accepted;
    } else {
      ++metric.rejected;
    }
    if (!sampled) return;
    ++metric.sampledAttempts;
    if (accepted) {
      ++metric.sampledAccepted;
    } else {
      ++metric.sampledRejected;
    }
    if (prepared && measured) {
      ++metric.sampledPrepared;
    } else {
      ++metric.sampledUnmeasuredFailures;
    }
  }

  void recordAdmissionDistribution(
      PeSemanticOwnerAdmissionFamily family,
      const PeSemanticOwnerAdmissionDistribution& distribution) noexcept {
    const auto index = static_cast<std::size_t>(family);
    if (index >= PeSemanticOwnerPhaseStats::kAdmissionFamilyCount) return;
    auto& metric = stats_.admissions[index];
    metric.sampledBindingOccurrences += distribution.bindingOccurrences;
    metric.sampledRefOccurrences += distribution.refOccurrences;
    metric.sampledUniqueIdentities += distribution.uniqueIdentities;
    metric.sampledLinearComparisons += distribution.linearComparisons;
    metric.maxBindingOccurrences =
        std::max(metric.maxBindingOccurrences, distribution.bindingOccurrences);
    metric.maxRefOccurrences =
        std::max(metric.maxRefOccurrences, distribution.refOccurrences);
    metric.maxUniqueIdentities =
        std::max(metric.maxUniqueIdentities, distribution.uniqueIdentities);
    metric.maxLinearComparisons = std::max(metric.maxLinearComparisons,
                                           distribution.linearComparisons);
    if (distribution.bindingOccurrences == 0u &&
        distribution.refOccurrences == 0u &&
        distribution.uniqueIdentities == 0u &&
        distribution.linearComparisons == 0u) {
      ++metric.sampledZeroWork;
    }
  }

  [[nodiscard]] const PeSemanticOwnerPhaseStats& stats() const noexcept {
    return stats_;
  }

 private:
  PeSemanticOwnerPhaseStats stats_{};
  bool activeParentSampled_ = false;
};

}  // namespace dxmt9::d3d9::pe
