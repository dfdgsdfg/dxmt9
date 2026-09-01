#pragma once

// The recorder state is deliberately a flat owner. D3D9DeviceImpl reaches
// these members through recorderState_ directly: it carries no reference
// aliases that could obscure ownership or outlive a future owner move. The
// owner adds no allocation or virtual dispatch to setter and draw paths.

#include <mutex>
#include <type_traits>

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_stateblock_transaction.hpp"
#include "d3d9_pe_recorder_transaction.hpp"
#include "dxmt9/thread_ownership.hpp"

namespace dxmt9::d3d9::pe {

struct PeRecorderState {
  explicit PeRecorderState(bool lockRequired = false) noexcept
      : recorderLockRequired(lockRequired) {}

  // R-BACK-43.4 `producer-owned` (PE game thread). The ordinary D3D9 device
  // contract is single-threaded.  The recursive
  // mutex is only entered when D3DCREATE_MULTITHREADED (or the explicit
  // rollback knob) requires it; the ownership token catches contract
  // violations in debug builds on the unlocked path.
  bool recorderLockRequired = false;
  std::recursive_mutex recorderMutex{};
  dxmt9::core::ThreadOwnershipToken recorderOwnership{};

  PeHotStateShadow peState{};
  PeConstShadowBlock peConsts{};
  ::PeStateBlockTransactionState stateBlockTransaction{};
  // Binding refs are the borrowed source witness for a call-local compact
  // plan. The section-sized owners below are never written by ordinary
  // APPLY_STATE/draw production; they remain for oversized batches, Render
  // Tape checkpointing, and SWVP override projections.
  mutable PeBindingView peBindingView{};
  mutable PeSparseScratch peSparseScratch{};
  mutable SparseStateInput peSparseState{};
  mutable D9CCommandChunkWireDrawHeader peSparseHeader{};
  mutable PeDrawPayloads peSparsePayloads{};

  // One persistent identity bridges append intent, seal/bridge, capture, and
  // owner reset. Payload/retainer/capture storage remains in their owners.
  PeRecorderChunkTransaction chunkTransaction{};
  bool commandChunkNegotiated = false;
  // Transport is negotiated independently from the canonical V2 grammar.
  // Keep contiguous as the fail-closed default for tests and old providers;
  // a provider that accepts SegmentedTransportV1 selects it explicitly.
  std::uint8_t commandChunkTransport =
      D9C_COMMAND_CHUNK_TRANSPORT_CONTIGUOUS;

  // These are recorder protocol state rather than diagnostics: a failed
  // append must leave them available for retry, and a successful commit must
  // advance them exactly once.
  std::uint64_t commandChunkCommits = 0;
  std::uint64_t commandChunkRecords = 0;
  std::uint64_t commandChunkBytes = 0;

  // These helpers keep the persistent transaction choreography out of the
  // large device header. They only carry value/checkpoint evidence; the
  // semantic owner and shadows remain the storage owners.
  bool recordCapacityPreEvidence(bool succeeded) noexcept {
    return chunkTransaction.recordCapacityPreEvidence(succeeded);
  }
  bool prepareChunkRecord(std::uint32_t type, std::size_t sizeHint,
                          bool capacityPreFlushed,
                          std::size_t recordCount,
                          std::size_t handleCount,
                          std::size_t payloadBytes,
                          std::size_t retainedObjects) noexcept {
    if (chunkTransaction.phase() == RecorderChunkTransactionPhase::RolledBack) {
      chunkTransaction.discard();
    }
    if (chunkTransaction.phase() == RecorderChunkTransactionPhase::Idle ||
        chunkTransaction.phase() == RecorderChunkTransactionPhase::Completed) {
      if (!chunkTransaction.beginChunk()) return false;
    }
    if (capacityPreFlushed && !chunkTransaction.recordCapacityPreEvidence(true)) {
      return false;
    }
    if (!chunkTransaction.noteRecord(
            type, sizeHint, peState.pendingTicket(), recordCount,
            handleCount, payloadBytes, retainedObjects)) {
      return false;
    }
    return true;
  }
  bool settleSemanticEmitter(bool accepted,
                             std::size_t recordCount,
                             std::size_t handleCount,
                             std::size_t payloadBytes,
                             std::size_t retainedObjects) noexcept {
    if (!chunkTransaction.activeRecord()) return false;
    const auto recordCheckpoint = chunkTransaction.activeRecordCheckpoint();
    const auto handleCheckpoint = chunkTransaction.activeHandleCheckpoint();
    const auto payloadCheckpoint = chunkTransaction.activePayloadCheckpoint();
    const auto retainerCheckpoint = chunkTransaction.activeRetainerCheckpoint();
    const bool checkpointsMatch = accepted
        ? recordCount > recordCheckpoint && handleCount >= handleCheckpoint &&
          payloadBytes >= payloadCheckpoint && retainedObjects >= retainerCheckpoint
        : recordCount == recordCheckpoint && handleCount == handleCheckpoint &&
          payloadBytes == payloadCheckpoint && retainedObjects == retainerCheckpoint;
    return checkpointsMatch && chunkTransaction.recordEmitResult(accepted);
  }
  bool recordChunkSealedEvidence(std::size_t recordCount,
                                 std::size_t handleCount,
                                 std::size_t payloadBytes,
                                 std::size_t retainedObjects) noexcept {
    return chunkTransaction.recordSealedEvidence(
        recordCount, handleCount, payloadBytes, retainedObjects);
  }
  bool sealedEvidenceMatchesChunk(std::size_t recordCount,
                                  std::size_t handleCount,
                                  std::size_t payloadBytes,
                                  std::size_t retainedObjects) const noexcept {
    return chunkTransaction.sealedEvidenceMatches(
        recordCount, handleCount, payloadBytes, retainedObjects);
  }
};

// The transaction owner is intentionally part of the recorder footprint; its
// phase/checkpoint state replaces the former split local observers.
#if defined(_WIN64)
static_assert(sizeof(PeRecorderState) == 104120u + sizeof(PeRecorderChunkTransaction));
static_assert(sizeof(PeRecorderState) == 104296u);
#elif defined(_WIN32)
static_assert(sizeof(PeRecorderState) == 103376u + sizeof(PeRecorderChunkTransaction));
static_assert(sizeof(PeRecorderState) == 103496u);
#else
static_assert(sizeof(PeRecorderState) == 104144u + sizeof(PeRecorderChunkTransaction));
static_assert(sizeof(PeRecorderState) == 104320u);
#endif

}  // namespace dxmt9::d3d9::pe
