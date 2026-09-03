#include "../../../src/dxmt9/dxmt9_direct_source_lifecycle.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_lifecycle.hpp"

#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <span>

using namespace dxmt9::queue;

static_assert(std::is_trivially_copyable_v<DirectSourceLifecycleState>);

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::array<std::byte, sizeof(DirectSourceLifecycleState)> stateBytes(
    const DirectSourceLifecycleState& state) {
  std::array<std::byte, sizeof(DirectSourceLifecycleState)> bytes{};
  std::memcpy(bytes.data(), &state, sizeof(state));
  return bytes;
}

DirectSourceIdentity identity(std::uint64_t raw, std::uint32_t span = 0,
                              std::uint32_t slot = 0,
                              std::uint64_t seq = 1) {
  return {
      .rawOrdinal = raw,
      .spanOrdinal = span,
      .sourceOrdinal = seq,
      .seqId = seq,
      .sourceGeneration = raw + 10,
      .storageGeneration = raw + 20,
      .destinationSlot = slot,
      .sourceIndex = slot,
      .firstPage = 100 + slot,
      .pageCount = 1,
  };
}

DirectSourceLifecycleEvent event(
    DirectSourceIdentity id, DirectSourceAction action,
    DirectSourceControlMode mode = DirectSourceControlMode::Ordinary,
    std::uint64_t retained = 64, std::uint64_t detached = 0,
    bool present = false, bool physicalOwner = true) {
  return {
      .identity = id,
      .action = action,
      .controlMode = mode,
      .creditKind = DirectSourceCreditKind::RetainedBytes,
      .schemaRevision = 1,
      .witnessGeneration = id.rawOrdinal + id.spanOrdinal + 100,
      .retainedCredit = physicalOwner ? retained : 0,
      .detachedCredit = physicalOwner ? detached : 0,
      .physicalCreditOwner = physicalOwner,
      .hasPresent = present,
  };
}

void apply(DirectSourceLifecycleState& state,
           const DirectSourceLifecycleEvent& value,
           const char* message) {
  check(reduceDirectSourceLifecycle(state, value) ==
            DirectSourceLifecycleError::None,
        message);
}

void completeSource(DirectSourceLifecycleState& state,
                    DirectSourceIdentity id,
                    DirectSourceControlMode mode,
                    bool present,
                    bool physicalOwner = true) {
  apply(state, event(id, DirectSourceAction::AdmitWitness, mode, 64, 0,
                     false, physicalOwner),
        "real admission attaches the production projection");
  for (const auto action : {
           DirectSourceAction::EffectCut,
           DirectSourceAction::DestinationReceipt,
           DirectSourceAction::Publish,
           DirectSourceAction::Encode,
           DirectSourceAction::Complete,
       }) {
    apply(state, event(id, action, mode, 64, 0,
                       action == DirectSourceAction::Publish && present,
                       physicalOwner),
          "complete Direct prefix reduces through the production table");
  }
  apply(state, event(id, DirectSourceAction::Detach, mode, 32, 32, present,
                     physicalOwner),
        "completion moves exact retained credit into detached credit");
  apply(state, event(id, DirectSourceAction::Restore, mode, 64, 0, present,
                     physicalOwner),
        "restore returns detached credit to retained credit");
  apply(state, event(id, DirectSourceAction::Reclaim, mode, 64, 0, present,
                     physicalOwner),
        "reclaim follows completion and restore");
}

void settleAdmittedSource(DirectSourceLifecycleState& state,
                          DirectSourceIdentity id,
                          DirectSourceControlMode mode, bool present,
                          bool physicalOwner = true) {
  for (const auto action : {
           DirectSourceAction::EffectCut,
           DirectSourceAction::DestinationReceipt,
           DirectSourceAction::Publish,
           DirectSourceAction::Encode,
           DirectSourceAction::Complete,
       }) {
    apply(state, event(id, action, mode, 64, 0,
                       action == DirectSourceAction::Publish && present,
                       physicalOwner),
          "settle admitted Direct source");
  }
  apply(state, event(id, DirectSourceAction::Detach, mode, 32, 32, present,
                     physicalOwner),
        "settle detaches exact retained credit");
  apply(state, event(id, DirectSourceAction::Restore, mode, 64, 0, present,
                     physicalOwner),
        "settle restores exact retained credit");
  apply(state, event(id, DirectSourceAction::Reclaim, mode, 64, 0, present,
                     physicalOwner),
        "settle reclaims after restore");
}

void twoSourceTwoSlotLifecycleTruthTable() {
  DirectSourceLifecycleState state{};
  completeSource(state, identity(1, 0, 0, 1),
                 DirectSourceControlMode::Separator, false);
  completeSource(state, identity(2, 0, 1, 2),
                 DirectSourceControlMode::OrderedControl, true);
  check(state.recordCount == 2 &&
            state.records[0].phase == DirectSourcePhase::Reclaimed &&
            state.records[1].phase == DirectSourcePhase::Reclaimed &&
            state.records[0].transitionCount == 9 &&
            state.records[1].transitionCount == 9 &&
            state.records[0].publicationCount == 1 &&
            state.records[1].publicationCount == 1 &&
            state.records[1].hasPresent &&
            state.lastReclaimedRawOrdinal == 2 &&
            state.lastReclaimedSpanOrdinal == 0 &&
            state.lastReclaimedSourceOrdinal == 2,
        "two slots rotate/reuse in exact FIFO and Present is preserved");
}

void sharedSlotCreditOwnerTruthTable() {
  DirectSourceLifecycleState state{};
  const auto owner = identity(1, 0, 0, 1);
  auto sibling = identity(1, 1, 0, 1);
  sibling.storageGeneration = owner.storageGeneration;
  auto orphan = sibling;
  orphan.destinationSlot = 1;
  DirectSourceLifecycleState missingOwnerState{};
  apply(missingOwnerState, event(owner, DirectSourceAction::ImportRaw),
        "import physical-owner baseline");
  check(reduceDirectSourceLifecycle(
            missingOwnerState,
            event(orphan, DirectSourceAction::ImportRaw, {}, 64, 0,
                         false, false)) ==
            DirectSourceLifecycleError::InvalidIdentity,
        "a shared semantic span cannot mint credit without its physical owner");
  completeSource(state, owner, DirectSourceControlMode::Separator, false);
  // Both semantic siblings must be admitted while the physical owner is
  // resident; reclaim removes the owner's ledger row by design.
  DirectSourceLifecycleState siblings{};
  apply(siblings, event(owner, DirectSourceAction::AdmitWitness,
                        DirectSourceControlMode::Separator),
        "admit shared physical owner");
  apply(siblings, event(sibling, DirectSourceAction::AdmitWitness,
                        DirectSourceControlMode::Separator, 64, 0, false,
                        false),
        "admit shared semantic sibling without new physical credit");
  settleAdmittedSource(siblings, owner, DirectSourceControlMode::Separator,
                       false);
  settleAdmittedSource(siblings, sibling,
                       DirectSourceControlMode::Separator, false, false);
  state = siblings;
  check(state.records[0].physicalCreditOwner &&
            !state.records[1].physicalCreditOwner &&
            state.records[0].aggregateCredit == 64 &&
            state.records[1].aggregateCredit == 0 &&
            state.recordCount == 2,
        "shared-slot spans keep one physical credit owner");
}

void rotatedAndCrossRawOwnerTruthTable() {
  {
    DirectSourceLifecycleState state{};
    auto owner = identity(1, 0, 0, 1);
    auto rotated = identity(1, 1, 1, 1);
    apply(state, event(owner, DirectSourceAction::AdmitWitness,
                       DirectSourceControlMode::Separator),
          "admit owner before rotated source");
    apply(state, event(rotated, DirectSourceAction::AdmitWitness,
                       DirectSourceControlMode::Separator, 64, 0, false,
                       true),
          "admit rotated owner");
    settleAdmittedSource(state, owner, DirectSourceControlMode::Separator,
                         false);
    settleAdmittedSource(state, rotated,
                   DirectSourceControlMode::Separator, false, true);
    check(state.records[1].physicalCreditOwner &&
              state.records[1].aggregateCredit == 64,
          "rotated span owns the new physical credit even when span ordinal is nonzero");
  }
  {
    DirectSourceLifecycleState state{};
    auto owner = identity(1, 0, 0, 1);
    auto crossRawAppend = identity(2, 0, 0, 1);
    crossRawAppend.sourceGeneration = owner.sourceGeneration;
    crossRawAppend.storageGeneration = owner.storageGeneration;
    crossRawAppend.sourceIndex = owner.sourceIndex;
    crossRawAppend.firstPage = owner.firstPage;
    apply(state, event(owner, DirectSourceAction::AdmitWitness,
                       DirectSourceControlMode::Separator),
          "admit owner before cross-raw append");
    apply(state, event(crossRawAppend, DirectSourceAction::AdmitWitness,
                       DirectSourceControlMode::Separator, 64, 0, false,
                       false),
          "admit cross-raw append without new physical credit");
    settleAdmittedSource(state, owner, DirectSourceControlMode::Separator,
                         false);
    settleAdmittedSource(state, crossRawAppend,
                         DirectSourceControlMode::Separator, false, false);
    check(!state.records[1].physicalCreditOwner &&
              state.records[1].aggregateCredit == 0,
          "cross-raw append keeps the existing physical credit owner");
  }
}

void productionAdmissionOrderCounterexample() {
  DirectSourceLifecycleState state{};
  apply(state, event(identity(2, 0, 1, 2),
                     DirectSourceAction::AdmitWitness),
        "production admission establishes the FIFO baseline");
  const auto before = state;
  const auto beforeBytes = stateBytes(state);
  check(reduceDirectSourceLifecycle(
            state,
            event(identity(1, 0, 0, 1),
                  DirectSourceAction::AdmitWitness)) ==
            DirectSourceLifecycleError::SourceReordered,
        "production admission rejects a reordered raw/source identity");
  check(state == before &&
            std::memcmp(&state, beforeBytes.data(), sizeof(state)) == 0,
        "rejected production admission leaves lifecycle state unchanged");
}

void preEffectRollbackErasesLifecycleTransaction() {
  DirectSourceLifecycleState state{};
  const auto rolledBack = identity(7, 0, 0, 7);
  apply(state, event(rolledBack, DirectSourceAction::AdmitWitness),
        "production admission creates the rollback transaction");
  apply(state, event(rolledBack, DirectSourceAction::RollbackPreEffect),
        "pre-effect fallback cancels the lifecycle transaction");
  check(state.recordCount == 0u &&
            state.lastImportedRawOrdinal == 0u &&
            state.lastImportedSpanOrdinal == 0u &&
            state.lastImportedSourceOrdinal == 0u,
        "rollback removes the row and restores its FIFO frontier");

  const auto compatibilitySuccessor = identity(7, 0, 0, 7);
  apply(state, event(compatibilitySuccessor,
                     DirectSourceAction::AdmitWitness),
        "the compatibility successor identity may be observed afresh");
  check(state.recordCount == 1u &&
            state.records[0].phase == DirectSourcePhase::Admitted,
        "no terminal RolledBack row leaks into the successor transaction");
}

void preEffectRollbackRequiresLedgerTail() {
  DirectSourceLifecycleState state{};
  const auto owner = identity(1, 0, 0, 1);
  auto sibling = identity(1, 1, 0, 1);
  sibling.storageGeneration = owner.storageGeneration;
  apply(state, event(owner, DirectSourceAction::AdmitWitness),
        "admit the physical-credit owner before its sibling");
  apply(state, event(sibling, DirectSourceAction::AdmitWitness, {}, 64, 0,
                     false, false),
        "admit a later semantic sibling without duplicate credit");

  const auto before = state;
  const auto beforeBytes = stateBytes(state);
  check(reduceDirectSourceLifecycle(
            state, event(owner, DirectSourceAction::RollbackPreEffect)) ==
            DirectSourceLifecycleError::SourceReordered,
        "non-tail rollback cannot orphan a later sibling or credit owner");
  check(state == before &&
            std::memcmp(&state, beforeBytes.data(), sizeof(state)) == 0,
        "rejected non-tail rollback leaves the ledger byte-identical");

  const std::array rollbackGroup{
      event(sibling, DirectSourceAction::RollbackPreEffect, {}, 64, 0, false,
            false),
      event(owner, DirectSourceAction::RollbackPreEffect),
  };
  check(reduceDirectSourceLifecycleBatch(
            state, rollbackGroup,
            /*requireSinglePhysicalCreditOwner=*/true) ==
            DirectSourceLifecycleError::None,
        "a complete sibling group rolls back atomically in tail order");
  check(state.recordCount == 0u && state.lastImportedRawOrdinal == 0u &&
            state.lastImportedSpanOrdinal == 0u &&
            state.lastImportedSourceOrdinal == 0u,
        "group rollback removes every sibling and restores the empty frontier");
}

void siblingBatchReducerIsAtomic() {
  DirectSourceLifecycleState state{};
  const auto owner = identity(1, 0, 0, 1);
  auto sibling = identity(1, 1, 0, 1);
  sibling.storageGeneration = owner.storageGeneration;
  apply(state, event(owner, DirectSourceAction::AdmitWitness),
        "admit physical owner for sibling batch");
  apply(state, event(sibling, DirectSourceAction::AdmitWitness, {}, 64, 0,
                     false, false),
        "admit semantic sibling for sibling batch");
  for (const auto action : {DirectSourceAction::EffectCut,
                            DirectSourceAction::DestinationReceipt}) {
    apply(state, event(owner, action), "advance physical owner to receipt");
    apply(state, event(sibling, action, {}, 64, 0, false, false),
          "advance semantic sibling to receipt");
  }
  apply(state, event(owner, DirectSourceAction::Publish),
        "make the first sibling encode-valid");

  const std::array batch{
      event(owner, DirectSourceAction::Encode),
      event(sibling, DirectSourceAction::Encode, {}, 64, 0, false, false),
  };
  const auto before = state;
  const auto beforeBytes = stateBytes(state);
  check(reduceDirectSourceLifecycleBatch(
            state, batch,
            /*requireSinglePhysicalCreditOwner=*/true) ==
            DirectSourceLifecycleError::InvalidTransition,
        "a later invalid sibling rejects the complete terminal batch");
  check(state == before &&
            std::memcmp(&state, beforeBytes.data(), sizeof(state)) == 0,
        "a rejected sibling batch leaves state byte- and value-identical");

  auto missingOwnerBatch = batch;
  missingOwnerBatch[0].physicalCreditOwner = false;
  missingOwnerBatch[0].retainedCredit = 0;
  check(reduceDirectSourceLifecycleBatch(
            state, missingOwnerBatch,
            /*requireSinglePhysicalCreditOwner=*/true) ==
            DirectSourceLifecycleError::CreditMismatch &&
            state == before &&
            std::memcmp(&state, beforeBytes.data(), sizeof(state)) == 0,
        "a sibling batch without a physical owner is rejected atomically");

  auto duplicateOwnerBatch = batch;
  duplicateOwnerBatch[1].physicalCreditOwner = true;
  duplicateOwnerBatch[1].retainedCredit = 64;
  check(reduceDirectSourceLifecycleBatch(
            state, duplicateOwnerBatch,
            /*requireSinglePhysicalCreditOwner=*/true) ==
            DirectSourceLifecycleError::CreditMismatch &&
            state == before &&
            std::memcmp(&state, beforeBytes.data(), sizeof(state)) == 0,
        "a sibling batch with duplicate physical owners is rejected atomically");
}

void expectedFailuresAreIndependent() {
  {
    DirectSourceLifecycleState state{};
    const auto id = identity(1);
    apply(state, event(id, DirectSourceAction::ImportRaw), "import");
    apply(state, event(id, DirectSourceAction::Plan), "plan");
    apply(state, event(id, DirectSourceAction::AdmitWitness), "admit");
    check(reduceDirectSourceLifecycle(
              state, event(id, DirectSourceAction::AdmitWitness)) ==
              DirectSourceLifecycleError::StaleOrDuplicateWitness,
          "duplicate witness fails independently");
  }
  {
    DirectSourceLifecycleState state{};
    apply(state, event(identity(2), DirectSourceAction::ImportRaw), "import 2");
    check(reduceDirectSourceLifecycle(
              state, event(identity(1), DirectSourceAction::ImportRaw)) ==
              DirectSourceLifecycleError::SourceReordered,
          "source reorder fails independently");
    check(reduceDirectSourceLifecycle(
              state, event(identity(2), DirectSourceAction::ImportRaw)) ==
              DirectSourceLifecycleError::DuplicateEmission,
          "double emission fails independently");
  }
  {
    DirectSourceLifecycleState state{};
    apply(state, event(identity(1, 0), DirectSourceAction::ImportRaw),
          "import first span");
    check(reduceDirectSourceLifecycle(
              state, event(identity(1, 2), DirectSourceAction::ImportRaw)) ==
              DirectSourceLifecycleError::SourceReordered,
          "a skipped span cannot enter the exact source interval");
    DirectSourceLifecycleState sourceOrderState{};
    apply(sourceOrderState, event(identity(1, 0, 0, 2),
                                  DirectSourceAction::ImportRaw),
          "import source-order baseline");
    check(reduceDirectSourceLifecycle(
              sourceOrderState,
              event(identity(2, 0, 0, 1), DirectSourceAction::ImportRaw)) ==
              DirectSourceLifecycleError::SourceReordered,
          "a new raw cannot reverse source ordinal order");
  }
  {
    DirectSourceLifecycleState state{};
    const auto id = identity(1);
    for (const auto action : {DirectSourceAction::ImportRaw,
                              DirectSourceAction::Plan,
                              DirectSourceAction::AdmitWitness,
                              DirectSourceAction::EffectCut}) {
      apply(state, event(id, action), "effect prefix");
    }
    check(reduceDirectSourceLifecycle(
              state, event(id, DirectSourceAction::RollbackPreEffect)) ==
              DirectSourceLifecycleError::FallbackAfterEffect,
          "post-effect retry fails independently");
    check(reduceDirectSourceLifecycle(
              state, event(id, DirectSourceAction::Reclaim)) ==
              DirectSourceLifecycleError::CompletionRequired,
          "early reclaim fails independently");
  }
  {
    DirectSourceLifecycleState state{};
    const auto id = identity(1);
    for (const auto action : {DirectSourceAction::ImportRaw,
                              DirectSourceAction::Plan,
                              DirectSourceAction::AdmitWitness,
                              DirectSourceAction::EffectCut,
                              DirectSourceAction::DestinationReceipt,
                              DirectSourceAction::Publish,
                              DirectSourceAction::Encode,
                              DirectSourceAction::Complete}) {
      apply(state, event(id, action), "completed prefix");
    }
    check(reduceDirectSourceLifecycle(
              state, event(id, DirectSourceAction::Detach, {}, 64, 1)) ==
              DirectSourceLifecycleError::CreditMismatch,
          "phantom detached credit fails independently");
    apply(state, event(id, DirectSourceAction::Detach, {}, 32, 32),
          "valid detach");
    check(reduceDirectSourceLifecycle(
              state, event(id, DirectSourceAction::Reclaim, {}, 32, 32)) ==
              DirectSourceLifecycleError::MissingRestore,
          "missing restore fails independently");
    auto partial = event(id, DirectSourceAction::Restore, {}, 64, 0);
    partial.partialAdoption = true;
    check(reduceDirectSourceLifecycle(state, partial) ==
              DirectSourceLifecycleError::PartialAdoption,
          "partial adoption fails independently");
  }
  {
    DirectSourceLifecycleState state{};
    const auto id = identity(1);
    for (const auto action : {DirectSourceAction::ImportRaw,
                              DirectSourceAction::Plan,
                              DirectSourceAction::AdmitWitness,
                              DirectSourceAction::EffectCut,
                              DirectSourceAction::DestinationReceipt,
                              DirectSourceAction::Publish,
                              DirectSourceAction::Encode,
                              DirectSourceAction::Complete}) {
      apply(state, event(id, action), "poison reclaim prefix");
    }
    apply(state, event(id, DirectSourceAction::Detach, {}, 32, 32),
          "poison reclaim detach");
    apply(state, event(id, DirectSourceAction::PoisonAbandon, {}, 64, 0),
          "poison abandon is an explicit terminal disposition");
    check(reduceDirectSourceLifecycle(
              state, event(id, DirectSourceAction::Reclaim, {}, 64, 0)) ==
              DirectSourceLifecycleError::MissingRestore,
          "poison-abandoned storage cannot be reclaimed as restored storage");
  }
}

void terminalAdapterFaultFailsObserverClosed() {
  PipelineLifecycleObserver observer(/*productionEnabled=*/true,
                                     /*requireEndToEndIdentity=*/false);
  const auto sink = observer.productionSink();
  failDirectSourceLifecycleObserver(sink);
  check(observer.directError() == DirectSourceLifecycleError::InvalidTransition &&
            observer.error() == PipelineObservationError::DirectLifecycleMismatch,
        "a missing terminal adapter match fails the diagnostic observer closed");
}

}  // namespace

int main() {
  twoSourceTwoSlotLifecycleTruthTable();
  sharedSlotCreditOwnerTruthTable();
  rotatedAndCrossRawOwnerTruthTable();
  productionAdmissionOrderCounterexample();
  preEffectRollbackErasesLifecycleTransaction();
  preEffectRollbackRequiresLedgerTail();
  siblingBatchReducerIsAtomic();
  expectedFailuresAreIndependent();
  terminalAdapterFaultFailsObserverClosed();
  return 0;
}
