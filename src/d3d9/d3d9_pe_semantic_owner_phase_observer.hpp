#pragma once

// Calibrated, opt-in timing for the PE semantic owner.  This observer is
// intentionally separate from CopyMaterializationLedger: a phase is CPU
// attribution, not a copy/materialization class.

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
          bool appendParent = true) noexcept
        : observer_(observer), phase_(phase), parent_(parent),
          appendParent_(appendParent) {
      if (!observer_) return;
      if (parent_) {
        priorActiveParentSampled_ = observer_->activeParentSampled_;
        observer_->activeParentSampled_ = false;
      }
      auto& metric = observer_->stats_.phases[static_cast<std::size_t>(phase_)];
      ++metric.events;
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
          priorActiveParentSampled_(other.priorActiveParentSampled_),
          sampled_(other.sampled_),
          started_(other.started_) {
      other.observer_ = nullptr;
      other.parent_ = false;
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
    bool priorActiveParentSampled_ = false;
    bool sampled_ = false;
    Clock::time_point started_{};
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

  void recordOutcome(PeSemanticOwnerOutcome outcome) noexcept {
    const auto index = static_cast<std::size_t>(outcome);
    if (index < stats_.outcomes.size()) ++stats_.outcomes[index];
  }

  [[nodiscard]] const PeSemanticOwnerPhaseStats& stats() const noexcept {
    return stats_;
  }

 private:
  PeSemanticOwnerPhaseStats stats_{};
  bool activeParentSampled_ = false;
};

}  // namespace dxmt9::d3d9::pe
