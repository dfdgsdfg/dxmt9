#include "device_c_cpu_ready_transfer.hpp"

#include <limits>

namespace dxmt9::d3d9 {

CpuReadySemanticTransfer::CpuReadySemanticTransfer(
    RawCommandChunk& source,
    CommandQueue::CpuReadyArenaBuildLease&& lease) noexcept
    : source_(&source), raw_(std::move(source)), lease_(std::move(lease)) {
  const auto ticket = lease_.ticket();
  identity_ = {
      .rawOrdinal = raw_.replaySeq,
      .sourceOrdinal = ticket.sourceOrdinal,
      .seqId = ticket.seqId,
      .buildGeneration = ticket.buildGeneration,
      .producerIdentity = ticket.producerIdentity,
  };
  const dxmt9::core::CpuReadyProducerIdentity rawProducerIdentity{
      .firstEventOrdinal = raw_.producerIdentity.firstEventOrdinal,
      .lastEventOrdinal = raw_.producerIdentity.lastEventOrdinal,
      .firstSourceOrdinal = raw_.producerIdentity.firstSourceOrdinal,
      .lastSourceOrdinal = raw_.producerIdentity.lastSourceOrdinal,
  };
  if (!identity_.valid() || ticket.rawOrdinal != raw_.replaySeq ||
      ticket.producerIdentity != rawProducerIdentity) {
    fail(CpuReadySemanticTransferFailure::InvalidSourceIdentity,
         CpuReadySemanticTransferStage::FailStopped);
  }
}

CpuReadySemanticTransfer::~CpuReadySemanticTransfer() noexcept {
  restoreSource();
}

CpuReadySemanticTransfer::CpuReadySemanticTransfer(
    CpuReadySemanticTransfer&& other) noexcept
    : source_(std::exchange(other.source_, nullptr)),
      raw_(std::move(other.raw_)),
      lease_(std::move(other.lease_)),
      identity_(other.identity_), stage_(other.stage_), failure_(other.failure_) {
  other.identity_ = {};
  other.stage_ = CpuReadySemanticTransferStage::Aborted;
  other.failure_ = CpuReadySemanticTransferFailure::None;
}

CpuReadySemanticTransfer& CpuReadySemanticTransfer::operator=(
    CpuReadySemanticTransfer&& other) noexcept {
  if (this == &other) return *this;
  restoreSource();
  source_ = std::exchange(other.source_, nullptr);
  raw_ = std::move(other.raw_);
  lease_ = std::move(other.lease_);
  identity_ = other.identity_;
  stage_ = other.stage_;
  failure_ = other.failure_;
  other.identity_ = {};
  other.stage_ = CpuReadySemanticTransferStage::Aborted;
  other.failure_ = CpuReadySemanticTransferFailure::None;
  return *this;
}

void CpuReadySemanticTransfer::restoreSource() noexcept {
  if (!source_) return;
  *source_ = std::move(raw_);
  source_ = nullptr;
}

void CpuReadySemanticTransfer::fail(
    CpuReadySemanticTransferFailure failure,
    CpuReadySemanticTransferStage stage) noexcept {
  failure_ = failure;
  stage_ = stage;
}

bool CpuReadySemanticTransfer::adopt() noexcept {
  if (stage_ != CpuReadySemanticTransferStage::Reserved ||
      failure_ != CpuReadySemanticTransferFailure::None) {
    fail(CpuReadySemanticTransferFailure::InvalidTransition,
         CpuReadySemanticTransferStage::FailStopped);
    return false;
  }
  stage_ = CpuReadySemanticTransferStage::Adopted;
  return true;
}

bool CpuReadySemanticTransfer::markEmitted() noexcept {
  if (stage_ != CpuReadySemanticTransferStage::Adopted) {
    fail(CpuReadySemanticTransferFailure::InvalidTransition,
         CpuReadySemanticTransferStage::FailStopped);
    return false;
  }
  stage_ = CpuReadySemanticTransferStage::Emitted;
  return true;
}

CommandQueue::CpuReadyArenaPublishStatus CpuReadySemanticTransfer::publish(
    CommandQueue::CpuReadyCaptureIdentity* captureIdentity) noexcept {
  if (stage_ != CpuReadySemanticTransferStage::Emitted) {
    fail(CpuReadySemanticTransferFailure::InvalidTransition,
         CpuReadySemanticTransferStage::FailStopped);
    return CommandQueue::CpuReadyArenaPublishStatus::FailStopped;
  }
  if (lease_.publish(raw_.resourceEntries, captureIdentity)) {
    stage_ = CpuReadySemanticTransferStage::Published;
    return CommandQueue::CpuReadyArenaPublishStatus::Published;
  }
  fail(CpuReadySemanticTransferFailure::PublishFailStopped,
       CpuReadySemanticTransferStage::FailStopped);
  return CommandQueue::CpuReadyArenaPublishStatus::FailStopped;
}

CommandQueue::CpuReadyArenaPublishStatus
CpuReadySemanticTransfer::publishBatch(
    CommandQueue::CpuReadyCaptureIdentityBatch* captureIdentity) noexcept {
  if (stage_ != CpuReadySemanticTransferStage::Emitted) {
    fail(CpuReadySemanticTransferFailure::InvalidTransition,
         CpuReadySemanticTransferStage::FailStopped);
    return CommandQueue::CpuReadyArenaPublishStatus::FailStopped;
  }
  const auto status = lease_.publishBatchWithStatus(
      raw_.resourceEntries, captureIdentity);
  if (status == CommandQueue::CpuReadyArenaPublishStatus::Published) {
    stage_ = CpuReadySemanticTransferStage::Published;
    return status;
  }
  fail(status == CommandQueue::CpuReadyArenaPublishStatus::RecoverableFailure
           ? CpuReadySemanticTransferFailure::PublishRecoverable
           : CpuReadySemanticTransferFailure::PublishFailStopped,
       CpuReadySemanticTransferStage::FailStopped);
  return status;
}

bool CpuReadySemanticTransfer::abortForFallback() noexcept {
  if (stage_ != CpuReadySemanticTransferStage::Reserved &&
      stage_ != CpuReadySemanticTransferStage::Adopted) {
    fail(CpuReadySemanticTransferFailure::InvalidTransition,
         CpuReadySemanticTransferStage::FailStopped);
    return false;
  }
  if (!lease_.abortForFallback()) {
    fail(CpuReadySemanticTransferFailure::AbortFailed,
         CpuReadySemanticTransferStage::FailStopped);
    return false;
  }
  stage_ = CpuReadySemanticTransferStage::Aborted;
  return true;
}

}  // namespace dxmt9::d3d9
