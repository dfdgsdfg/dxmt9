#pragma once

#include "d3d9_pe_state_shadow.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dxmt9::d3d9::pe {

// Persistent, value-only owner of one producer chunk transaction. Payload,
// object references, retainer entries, and capture buffers remain in their
// natural storage owners; this object owns their common phase identity and
// checkpoints. It is bounded and allocation-free.
enum class RecorderChunkTransactionPhase : std::uint8_t {
    Idle, Collecting, Sealed, Emitted, BridgeAccepted, CaptureSettled,
    CapacityPost, Completed, RolledBack, Retry, Poisoned,
};

enum class RecorderChunkCaptureDisposition : std::uint8_t {
    Materialized, Rejected, Skipped,
};

class PeRecorderChunkTransaction final {
public:
    constexpr PeRecorderChunkTransaction() noexcept = default;

    constexpr bool beginChunk() noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Idle &&
            phase_ != RecorderChunkTransactionPhase::Completed) return false;
        ++transactionEpoch_;
        if (transactionEpoch_ == 0u) transactionEpoch_ = 1u;
        phase_ = RecorderChunkTransactionPhase::Collecting;
        type_ = 0u; sizeHint_ = 0u; recordCount_ = 0u;
        recordCheckpoint_ = handleCheckpoint_ = payloadCheckpoint_ = 0u;
        retainerCheckpoint_ = 0u; pendingTicket_ = {};
        activeRecordCheckpoint_ = activeHandleCheckpoint_ = 0u;
        activePayloadCheckpoint_ = activeRetainerCheckpoint_ = 0u;
        activeRecord_ = false;
        capacityPreSeen_ = capacityPreSucceeded_ = false;
        capacityPostSeen_ = capacityPostSucceeded_ = false;
        captureReserved_ = false; captureToken_ = captureOrdinal_ = 0u;
        captureDisposition_ = RecorderChunkCaptureDisposition::Skipped;
        retryAtSeal_ = false;
        sealedEvidenceValid_ = false;
        sealedRecordCount_ = sealedHandleCount_ = sealedPayloadBytes_ = 0u;
        sealedRetainerCount_ = 0u;
        return true;
    }

    // Compatibility helper for a standalone one-record transaction.
    constexpr bool beginRecord(std::uint32_t type,
                               std::size_t sizeHint) noexcept {
        return beginChunk() && noteRecord(type, sizeHint, {}, 0u, 0u, 0u, 0u);
    }

    constexpr bool noteRecord(std::uint32_t type, std::size_t sizeHint,
                              PendingDeltaTicket pendingTicket,
                              std::size_t recordCheckpoint,
                              std::size_t handleCheckpoint,
                              std::size_t payloadCheckpoint,
                              std::size_t retainerCheckpoint) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Collecting &&
            phase_ != RecorderChunkTransactionPhase::Emitted) return false;
        // The producer must settle the previous intent before opening the
        // next one.  Keeping this guard here prevents a second note from
        // overwriting the rollback witness that belongs to the active record.
        if (activeRecord_) return false;
        if (type == 0u || sizeHint == 0u) return false;
        activePreviousPendingTicket_ = pendingTicket_;
        pendingTicket_ = pendingTicket;
        activeRecord_ = true;
        activeRecordCheckpoint_ = recordCheckpoint;
        activeHandleCheckpoint_ = handleCheckpoint;
        activePayloadCheckpoint_ = payloadCheckpoint;
        activeRetainerCheckpoint_ = retainerCheckpoint;
        if (recordCount_ == 0u) {
            type_ = type; sizeHint_ = sizeHint;
            recordCheckpoint_ = recordCheckpoint;
            handleCheckpoint_ = handleCheckpoint;
            payloadCheckpoint_ = payloadCheckpoint;
            retainerCheckpoint_ = retainerCheckpoint;
        }
        ++recordCount_;
        return true;
    }

    constexpr bool capacityPre(bool succeeded) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Collecting &&
            phase_ != RecorderChunkTransactionPhase::Emitted) return false;
        return recordCapacityPreResult(succeeded);
    }

    // Records a failed pre-flush even when the prior chunk has already moved
    // the owner to Retry. This keeps the attempted append's CapacityPre
    // evidence in the same persistent owner without reopening its bytes.
    constexpr bool recordCapacityPreResult(bool succeeded) noexcept {
        return recordCapacityPreEvidence(succeeded) && succeeded;
    }

    // Returns whether the evidence was accepted, independent of whether the
    // operation itself succeeded.  This lets release builds distinguish an
    // expected failed preflush from a malformed owner transition.
    constexpr bool recordCapacityPreEvidence(bool succeeded) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Collecting &&
            phase_ != RecorderChunkTransactionPhase::Emitted &&
            phase_ != RecorderChunkTransactionPhase::Retry &&
            phase_ != RecorderChunkTransactionPhase::Poisoned) {
            return false;
        }
        capacityPreSeen_ = true; capacityPreSucceeded_ = succeeded;
        // A failed bridge is effect-unknown.  A later capacity-pre result
        // belongs to the caller's attempted retry and must never reopen or
        // relabel that sealed owner as Retry.
        if (!succeeded && phase_ != RecorderChunkTransactionPhase::Poisoned) {
            phase_ = RecorderChunkTransactionPhase::Retry;
        }
        return true;
    }

    constexpr bool seal(bool succeeded) noexcept {
        return recordSealResult(succeeded) && succeeded;
    }

    constexpr bool recordSealResult(bool succeeded) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Collecting &&
            phase_ != RecorderChunkTransactionPhase::Emitted &&
            !(phase_ == RecorderChunkTransactionPhase::Retry && retryAtSeal_)) return false;
        retryAtSeal_ = !succeeded;
        phase_ = succeeded ? RecorderChunkTransactionPhase::Sealed
                           : RecorderChunkTransactionPhase::Retry;
        return true;
    }

    constexpr bool recordSealedEvidence(std::size_t records,
                                        std::size_t handles,
                                        std::size_t payloadBytes,
                                        std::size_t retainedObjects) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Sealed) return false;
        sealedRecordCount_ = records;
        sealedHandleCount_ = handles;
        sealedPayloadBytes_ = payloadBytes;
        sealedRetainerCount_ = retainedObjects;
        sealedEvidenceValid_ = true;
        return true;
    }

    constexpr bool sealedEvidenceValid() const noexcept {
        return sealedEvidenceValid_;
    }
    constexpr bool sealedEvidenceMatches(std::size_t records,
                                         std::size_t handles,
                                         std::size_t payloadBytes,
                                         std::size_t retainedObjects) const noexcept {
        return sealedEvidenceValid_ && sealedRecordCount_ == records &&
               sealedHandleCount_ == handles &&
               sealedPayloadBytes_ == payloadBytes &&
               sealedRetainerCount_ == retainedObjects;
    }

    constexpr bool emit(bool recordAccepted) noexcept {
        return recordEmitResult(recordAccepted) && recordAccepted;
    }

    // Settles the active record and reports transition validity separately
    // from the emitter result.  A failed emitter is a valid rollback outcome,
    // not an invalid transaction transition.
    constexpr bool recordEmitResult(bool recordAccepted) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Collecting &&
            phase_ != RecorderChunkTransactionPhase::Emitted) return false;
        if (!recordAccepted) {
            if (activeRecord_) {
                activeRecord_ = false;
                if (recordCount_ != 0u) --recordCount_;
                pendingTicket_ = activePreviousPendingTicket_;
                activeRecordCheckpoint_ = activeHandleCheckpoint_ = 0u;
                activePayloadCheckpoint_ = activeRetainerCheckpoint_ = 0u;
                if (recordCount_ == 0u) {
                    type_ = 0u; sizeHint_ = 0u;
                    recordCheckpoint_ = handleCheckpoint_ = 0u;
                    payloadCheckpoint_ = retainerCheckpoint_ = 0u;
                    pendingTicket_ = {};
                    activePreviousPendingTicket_ = {};
                }
            }
            phase_ = recordCount_ == 0u
                ? RecorderChunkTransactionPhase::Collecting
                : RecorderChunkTransactionPhase::Emitted;
            return true;
        }
        activeRecord_ = false;
        phase_ = RecorderChunkTransactionPhase::Emitted;
        return true;
    }

    constexpr bool bridge(bool commandAccepted) noexcept {
        return recordBridgeResult(commandAccepted) && commandAccepted;
    }

    // A fault injected before entering the PE/unix call keeps the sealed
    // bytes and all retained objects retryable. `retryAtSeal_` is reused as
    // the compact retry witness; no recorder footprint or ABI changes.
    constexpr bool recordBridgePreEffectFailure() noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Sealed) return false;
        retryAtSeal_ = true;
        phase_ = RecorderChunkTransactionPhase::Retry;
        return true;
    }

    constexpr bool reopenBridgePreEffectRetry() noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Retry || !retryAtSeal_) {
            return false;
        }
        // The builder remains sealed, but flushPendingCommandChunk runs its
        // normal seal bookkeeping again. Emitted is the pre-seal phase that
        // admits that second bookkeeping pass.
        phase_ = RecorderChunkTransactionPhase::Emitted;
        return true;
    }

    // Returns transition validity independently from the bridge result.  A
    // false bridge result is an accepted effect-unknown transition to
    // Poisoned, while a false return here means the owner was malformed.
    constexpr bool recordBridgeResult(bool commandAccepted) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Sealed) return false;
        phase_ = commandAccepted ? RecorderChunkTransactionPhase::BridgeAccepted
                                 : RecorderChunkTransactionPhase::Poisoned;
        return true;
    }

    // Fail-closed terminal transition for a post-bridge invariant failure
    // (for example malformed capture settlement).  It is deliberately
    // separate from recordBridgeResult(false), which models the bridge call
    // itself returning effect-unknown.
    constexpr bool poison() noexcept {
        if (phase_ == RecorderChunkTransactionPhase::Idle ||
            phase_ == RecorderChunkTransactionPhase::Completed) return false;
        phase_ = RecorderChunkTransactionPhase::Poisoned;
        return true;
    }

    constexpr bool capture(RecorderChunkCaptureDisposition disposition) noexcept {
        return recordCaptureResult(disposition);
    }

    constexpr bool recordCaptureResult(
        RecorderChunkCaptureDisposition disposition) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::BridgeAccepted) return false;
        if (disposition == RecorderChunkCaptureDisposition::Materialized &&
            !captureReserved_) return false;
        if (disposition == RecorderChunkCaptureDisposition::Skipped &&
            captureReserved_) return false;
        captureDisposition_ = disposition;
        phase_ = RecorderChunkTransactionPhase::CaptureSettled;
        return true;
    }

    constexpr bool capacityPost(bool succeeded) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Emitted &&
            phase_ != RecorderChunkTransactionPhase::CaptureSettled &&
            phase_ != RecorderChunkTransactionPhase::Retry &&
            phase_ != RecorderChunkTransactionPhase::Poisoned) return false;
        return recordCapacityPostResult(succeeded) && succeeded;
    }

    // CapacityPost is settled by flushPendingCommandChunk, immediately
    // before the successful commit owner is completed/discarded.  Failed
    // sealing/bridge paths may already be Retry/Poisoned; record evidence
    // without reopening those terminal meanings.
    constexpr bool recordCapacityPostResult(bool succeeded) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Emitted &&
            phase_ != RecorderChunkTransactionPhase::CaptureSettled &&
            phase_ != RecorderChunkTransactionPhase::Retry &&
            phase_ != RecorderChunkTransactionPhase::Poisoned) {
            return false;
        }
        capacityPostSeen_ = true; capacityPostSucceeded_ = succeeded;
        if (phase_ == RecorderChunkTransactionPhase::Emitted ||
            phase_ == RecorderChunkTransactionPhase::CaptureSettled) {
            phase_ = succeeded ? RecorderChunkTransactionPhase::CapacityPost
                               : RecorderChunkTransactionPhase::Retry;
        }
        return true;
    }

    constexpr bool complete() noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Emitted &&
            phase_ != RecorderChunkTransactionPhase::CapacityPost &&
            phase_ != RecorderChunkTransactionPhase::CaptureSettled) return false;
        phase_ = RecorderChunkTransactionPhase::Completed;
        return true;
    }

    constexpr bool recordCaptureReservation(std::uint64_t token,
                                             std::uint64_t ordinal,
                                             bool reserved) noexcept {
        if (phase_ != RecorderChunkTransactionPhase::Sealed) return false;
        captureToken_ = token; captureOrdinal_ = ordinal;
        captureReserved_ = reserved;
        return true;
    }

    constexpr bool captureReservationMatches(std::uint64_t token,
                                              std::uint64_t ordinal,
                                              bool reserved) const noexcept {
        return captureToken_ == token && captureOrdinal_ == ordinal &&
               captureReserved_ == reserved;
    }

    constexpr RecorderChunkTransactionPhase phase() const noexcept { return phase_; }
    constexpr std::uint32_t type() const noexcept { return type_; }
    constexpr std::size_t sizeHint() const noexcept { return sizeHint_; }
    constexpr std::size_t recordCount() const noexcept { return recordCount_; }
    constexpr std::uint64_t transactionEpoch() const noexcept { return transactionEpoch_; }
    constexpr std::size_t recordCheckpoint() const noexcept { return recordCheckpoint_; }
    constexpr std::size_t handleCheckpoint() const noexcept { return handleCheckpoint_; }
    constexpr std::size_t payloadCheckpoint() const noexcept { return payloadCheckpoint_; }
    constexpr std::size_t retainerCheckpoint() const noexcept { return retainerCheckpoint_; }
    constexpr std::size_t activeRecordCheckpoint() const noexcept { return activeRecordCheckpoint_; }
    constexpr std::size_t activeHandleCheckpoint() const noexcept { return activeHandleCheckpoint_; }
    constexpr std::size_t activePayloadCheckpoint() const noexcept { return activePayloadCheckpoint_; }
    constexpr std::size_t activeRetainerCheckpoint() const noexcept { return activeRetainerCheckpoint_; }
    constexpr bool activeRecord() const noexcept { return activeRecord_; }
    constexpr PendingDeltaTicket pendingTicket() const noexcept { return pendingTicket_; }
    constexpr bool captureReserved() const noexcept { return captureReserved_; }
    constexpr std::uint64_t captureToken() const noexcept { return captureToken_; }
    constexpr std::uint64_t captureOrdinal() const noexcept { return captureOrdinal_; }
    constexpr RecorderChunkCaptureDisposition captureDisposition() const noexcept {
        return captureDisposition_;
    }
    constexpr bool capacityPreSeen() const noexcept { return capacityPreSeen_; }
    constexpr bool capacityPreSucceeded() const noexcept { return capacityPreSucceeded_; }
    constexpr bool capacityPostSeen() const noexcept { return capacityPostSeen_; }
    constexpr bool capacityPostSucceeded() const noexcept { return capacityPostSucceeded_; }
    constexpr bool retryable() const noexcept { return phase_ == RecorderChunkTransactionPhase::Retry; }
    constexpr bool retryAtSeal() const noexcept { return retryAtSeal_; }
    constexpr bool poisoned() const noexcept { return phase_ == RecorderChunkTransactionPhase::Poisoned; }
    constexpr bool completed() const noexcept { return phase_ == RecorderChunkTransactionPhase::Completed; }

    // Only a successful reset or explicit device discard may clear this owner.
    // An effect-unknown bridge failure must leave it Poisoned.
    constexpr void discard() noexcept {
        phase_ = RecorderChunkTransactionPhase::Idle;
        type_ = 0u; sizeHint_ = 0u; recordCount_ = 0u;
        recordCheckpoint_ = handleCheckpoint_ = payloadCheckpoint_ = 0u;
        retainerCheckpoint_ = 0u; pendingTicket_ = {};
        activeRecordCheckpoint_ = activeHandleCheckpoint_ = 0u;
        activePayloadCheckpoint_ = activeRetainerCheckpoint_ = 0u;
        capacityPreSeen_ = capacityPreSucceeded_ = false;
        capacityPostSeen_ = capacityPostSucceeded_ = false;
        captureReserved_ = false; captureToken_ = captureOrdinal_ = 0u;
        captureDisposition_ = RecorderChunkCaptureDisposition::Skipped;
        retryAtSeal_ = false;
        activeRecord_ = false;
        sealedEvidenceValid_ = false;
        sealedRecordCount_ = sealedHandleCount_ = sealedPayloadBytes_ = 0u;
        sealedRetainerCount_ = 0u;
    }

private:
    RecorderChunkTransactionPhase phase_ = RecorderChunkTransactionPhase::Idle;
    RecorderChunkCaptureDisposition captureDisposition_ = RecorderChunkCaptureDisposition::Skipped;
    bool capacityPreSeen_ = false, capacityPreSucceeded_ = false;
    bool capacityPostSeen_ = false, capacityPostSucceeded_ = false;
    bool captureReserved_ = false;
    bool activeRecord_ = false;
    bool retryAtSeal_ = false;
    std::uint32_t type_ = 0u;
    std::size_t sizeHint_ = 0u, recordCount_ = 0u;
    std::size_t recordCheckpoint_ = 0u, handleCheckpoint_ = 0u;
    std::size_t payloadCheckpoint_ = 0u, retainerCheckpoint_ = 0u;
    std::size_t activeRecordCheckpoint_ = 0u, activeHandleCheckpoint_ = 0u;
    std::size_t activePayloadCheckpoint_ = 0u, activeRetainerCheckpoint_ = 0u;
    std::uint64_t captureToken_ = 0u, captureOrdinal_ = 0u;
    PendingDeltaTicket pendingTicket_{};
    PendingDeltaTicket activePreviousPendingTicket_{};
    std::uint64_t transactionEpoch_ = 0u;
    bool sealedEvidenceValid_ = false;
    std::size_t sealedRecordCount_ = 0u, sealedHandleCount_ = 0u;
    std::size_t sealedPayloadBytes_ = 0u, sealedRetainerCount_ = 0u;
};

static_assert(std::is_trivially_destructible_v<PeRecorderChunkTransaction>);
static_assert(std::is_trivially_copyable_v<PeRecorderChunkTransaction>);

}  // namespace dxmt9::d3d9::pe
