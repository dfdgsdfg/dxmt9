#pragma once

// Unix-side ownership seam between the imported replay chunk and the
// CPU-ready Tape arena.  This header deliberately lives outside the bridge
// ABI: the PE wire remains unchanged and the transfer owns only Unix values.

#include "device_c_replay_offload.hpp"
#include "../dxmt9/dxmt9_command_queue.hpp"

#include <cstdint>
#include <utility>

namespace dxmt9::d3d9 {

enum class CpuReadySemanticTransferStage : std::uint8_t {
  Reserved,
  Adopted,
  Emitted,
  Published,
  Aborted,
  FailStopped,
};

enum class CpuReadySemanticTransferFailure : std::uint8_t {
  None,
  InvalidSourceIdentity,
  InvalidTransition,
  PublishRecoverable,
  PublishFailStopped,
  AbortFailed,
};

struct CpuReadySemanticTransferIdentity {
  std::uint64_t rawOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint64_t buildGeneration = 0;
  dxmt9::core::CpuReadyProducerIdentity producerIdentity{};

  bool valid() const noexcept {
    return rawOrdinal != 0 && sourceOrdinal != 0 && seqId != 0 &&
           buildGeneration != 0 && producerIdentity.importable();
  }
};

// Move-only owner for one direct-Arena replay transaction.  The source Raw
// object is moved into this owner for the lifetime of Reserve/Adopt/Emit and
// moved back to its caller at destruction.  That preserves the existing
// worker's wrapper-release authority while making the transfer's ownership
// window explicit and testable; this is not a zero-copy ABI claim.
class CpuReadySemanticTransfer final {
 public:
  CpuReadySemanticTransfer(
      RawCommandChunk& source,
      CommandQueue::CpuReadyArenaBuildLease&& lease) noexcept;
  ~CpuReadySemanticTransfer() noexcept;

  CpuReadySemanticTransfer(const CpuReadySemanticTransfer&) = delete;
  CpuReadySemanticTransfer& operator=(const CpuReadySemanticTransfer&) = delete;
  CpuReadySemanticTransfer(CpuReadySemanticTransfer&& other) noexcept;
  CpuReadySemanticTransfer& operator=(CpuReadySemanticTransfer&& other) noexcept;

  RawCommandChunk& raw() noexcept { return raw_; }
  const RawCommandChunk& raw() const noexcept { return raw_; }
  CommandQueue::CpuReadyArenaBuildLease& lease() noexcept { return lease_; }
  const CommandQueue::CpuReadyArenaBuildLease& lease() const noexcept {
    return lease_;
  }

  CpuReadySemanticTransferIdentity identity() const noexcept { return identity_; }
  CpuReadySemanticTransferStage stage() const noexcept { return stage_; }
  CpuReadySemanticTransferFailure failure() const noexcept { return failure_; }

  bool adopt() noexcept;
  bool markEmitted() noexcept;
  CommandQueue::CpuReadyArenaPublishStatus publish(
      CommandQueue::CpuReadyCaptureIdentity* captureIdentity = nullptr) noexcept;
  CommandQueue::CpuReadyArenaPublishStatus publishBatch(
      CommandQueue::CpuReadyCaptureIdentityBatch* captureIdentity = nullptr) noexcept;
  bool abortForFallback() noexcept;
  // Ends the temporary ownership window after a successful pre-effect abort.
  // The replay caller uses this before retrying the same Raw object.
  void restoreToSource() noexcept { restoreSource(); }

 private:
  void restoreSource() noexcept;
  void fail(CpuReadySemanticTransferFailure failure,
            CpuReadySemanticTransferStage stage) noexcept;

  RawCommandChunk* source_ = nullptr;
  RawCommandChunk raw_{};
  CommandQueue::CpuReadyArenaBuildLease lease_{};
  CpuReadySemanticTransferIdentity identity_{};
  CpuReadySemanticTransferStage stage_ = CpuReadySemanticTransferStage::Reserved;
  CpuReadySemanticTransferFailure failure_ =
      CpuReadySemanticTransferFailure::None;
};

}  // namespace dxmt9::d3d9
