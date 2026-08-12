#include "device_c_render_tape_capture.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dxmt9::d3d9 {

namespace {

bool validIdentity(const D9CWireObjectIdentity& identity) noexcept {
  return identity.kind <= D9C_CHUNK_HANDLE_KIND_QUERY &&
         identity.generation != 0u && identity.objectId != 0u;
}

bool sameIdentity(const D9CWireObjectIdentity& a,
                  const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

bool sameSlot(const D9CWireObjectIdentity& a,
              const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.objectId == b.objectId;
}

} // namespace

RenderTapeCaptureSession::RenderTapeCaptureSession(
    bool enabled, RenderTapeCaptureLimits limits)
    : enabled_(enabled), limits_(limits),
      state_(RenderTapeCaptureState::Disabled) {}

RenderTapeCaptureStatus RenderTapeCaptureSession::arm(
    std::span<const std::byte> bootstrapOverlay,
    std::span<const RenderTapeBlob> blobs) {
  if (!enabled_) {
    return RenderTapeCaptureStatus::Disabled;
  }
  if (state_ != RenderTapeCaptureState::Disabled &&
      state_ != RenderTapeCaptureState::Aborted) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  if (bootstrapOverlay.empty() || blobs.size() > limits_.maxBlobEntries) {
    return RenderTapeCaptureStatus::InvalidInput;
  }

  try {
    catalogue_.blobs.assign(blobs.begin(), blobs.end());
    objects_.clear();
    sealedArtifact_.clear();
    eventCount_ = 0u;
    eventBytes_ = 0u;
    presentChunkSeen_ = false;
    validationStatus_ = RenderTapeValidationStatus::Valid;
    builder_ = RenderTapeBuilder{};
    const auto status = reserveEvent(
        sizeof(RenderTapeBootstrapHeader) + bootstrapOverlay.size());
    if (status != RenderTapeCaptureStatus::Accepted) {
      return status;
    }
    builder_.appendBootstrapState(bootstrapOverlay);
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeBootstrapHeader) + bootstrapOverlay.size();
    state_ = RenderTapeCaptureState::Armed;
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::beginPresentInterval() {
  if (!enabled_) {
    return RenderTapeCaptureStatus::Disabled;
  }
  if (state_ != RenderTapeCaptureState::Armed) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  state_ = RenderTapeCaptureState::Capturing;
  return RenderTapeCaptureStatus::Accepted;
}

RenderTapeCaptureStatus RenderTapeCaptureSession::registerVerifiedBlob(
    std::span<const std::byte, kRenderTapeDigestSize> digest,
    std::uint64_t size) {
  if (!enabled_) {
    return RenderTapeCaptureStatus::Disabled;
  }
  if (state_ != RenderTapeCaptureState::Armed &&
      state_ != RenderTapeCaptureState::Capturing) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  if (catalogue_.blobs.size() >= limits_.maxBlobEntries) {
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
  try {
    catalogue_.blobs.push_back(RenderTapeBlob{
        .digest = [&] {
          RenderTapeDigest copy{};
          std::memcpy(copy.data(), digest.data(), kRenderTapeDigestSize);
          return copy;
        }(),
        .size = size,
        .verified = 1u,
    });
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::reserveEvent(
    std::size_t payloadBytes) noexcept {
  if (eventCount_ >= limits_.maxEvents ||
      payloadBytes > std::numeric_limits<std::uint64_t>::max() - eventBytes_ ||
      eventBytes_ + payloadBytes > limits_.maxEventBytes) {
    if (state_ == RenderTapeCaptureState::Capturing) {
      // Capacity pressure is terminal for this interval. Keeping a partially
      // admitted owner alive would let a caller accidentally publish a tape
      // that no longer represents the complete Present interval.
      abortInternal();
    }
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
  return RenderTapeCaptureStatus::Accepted;
}

RenderTapeCaptureStatus RenderTapeCaptureSession::requireCapturing() const noexcept {
  if (!enabled_) {
    return RenderTapeCaptureStatus::Disabled;
  }
  return state_ == RenderTapeCaptureState::Capturing
             ? RenderTapeCaptureStatus::Accepted
             : RenderTapeCaptureStatus::InvalidState;
}

bool RenderTapeCaptureSession::hasVerifiedBlob(
    const RenderTapeDigest& digest, std::uint64_t size) const noexcept {
  return catalogue_.lookup(digest, size) == RenderTapeBlobLookup::Exact;
}

bool RenderTapeCaptureSession::hasObject(
    const D9CWireObjectIdentity& identity, bool liveOnly) const noexcept {
  return std::any_of(objects_.begin(), objects_.end(), [&](const auto& slot) {
    return sameIdentity(slot.identity, identity) && (!liveOnly || slot.live);
  });
}

bool RenderTapeCaptureSession::chunkHasPresent(
    std::span<const std::byte> chunk,
    const CommandChunkEnvelope& envelope) const noexcept {
  ImportedChunkView imported{};
  CommandChunkValidationScratch scratch{};
  if (!validateCommandChunk(chunk, envelope, &imported, scratch).valid()) {
    return false;
  }
  return std::any_of(imported.records.begin(), imported.records.end(),
                    [](const auto& record) {
                      return record.type == D9C_COMMAND_RECORD_PRESENT;
                    });
}

RenderTapeCaptureStatus RenderTapeCaptureSession::objectDefine(
    const D9CWireObjectIdentity& identity, std::uint32_t descriptorKind,
    std::span<const std::byte> descriptor, std::uint64_t immutableBytes,
    RenderTapeDigest immutableDigest) {
  if (const auto status = requireCapturing();
      status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  if (!validIdentity(identity) || descriptorKind == 0u || descriptor.empty() ||
      hasObject(identity, false) ||
      std::any_of(objects_.begin(), objects_.end(), [&](const auto& slot) {
        return sameSlot(slot.identity, identity);
      }) ||
      (immutableBytes != 0u && !hasVerifiedBlob(immutableDigest,
                                                  immutableBytes))) {
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const auto status = reserveEvent(sizeof(RenderTapeObjectDefineHeader) +
                                   descriptor.size());
  if (status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  try {
    builder_.appendObjectDefine(identity, descriptorKind, descriptor,
                                immutableBytes, immutableDigest);
    objects_.push_back(ObjectSlot{.identity = identity, .live = true});
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeObjectDefineHeader) + descriptor.size();
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::objectDestroy(
    const D9CWireObjectIdentity& identity) {
  if (const auto status = requireCapturing();
      status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  if (!validIdentity(identity) || !hasObject(identity, true)) {
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const auto status = reserveEvent(sizeof(RenderTapeObjectDestroyHeader));
  if (status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  try {
    builder_.appendObjectDestroy(identity);
    for (auto& slot : objects_) {
      if (sameIdentity(slot.identity, identity)) {
        slot.live = false;
      }
    }
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeObjectDestroyHeader);
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::resourceMutation(
    const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
    std::uint32_t subresource, std::uint64_t byteOffset,
    std::uint64_t byteSize,
    std::span<const std::byte, kRenderTapeDigestSize> digest) {
  if (const auto status = requireCapturing();
      status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  RenderTapeDigest copy{};
  std::memcpy(copy.data(), digest.data(), kRenderTapeDigestSize);
  if (!validIdentity(identity) || !hasObject(identity, true) || byteSize == 0u ||
      byteOffset > std::numeric_limits<std::uint64_t>::max() - byteSize ||
      !hasVerifiedBlob(copy, byteSize)) {
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const auto status = reserveEvent(sizeof(RenderTapeResourceMutationHeader));
  if (status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  try {
    builder_.appendResourceMutation(identity, kind, subresource, byteOffset,
                                    byteSize, digest);
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeResourceMutationHeader);
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::commandChunk(
    const CommandChunkEnvelope& envelope, std::span<const std::byte> chunk) {
  if (const auto status = requireCapturing();
      status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  if (chunk.empty() || !validateCommandChunk(chunk, envelope).valid() ||
      (chunkHasPresent(chunk, envelope) && presentChunkSeen_)) {
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const auto status = reserveEvent(sizeof(RenderTapeCommandChunkHeader) +
                                   chunk.size());
  if (status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  try {
    builder_.appendCommandChunk(envelope, chunk);
    presentChunkSeen_ = presentChunkSeen_ || chunkHasPresent(chunk, envelope);
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeCommandChunkHeader) + chunk.size();
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::orderedControl(
    const RenderTapeOrderedControlHeader& fixed,
    std::span<const std::byte> controlPayload) {
  if (const auto status = requireCapturing();
      status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  const auto kind = static_cast<RenderTapeControlKind>(fixed.kind);
  if (fixed.kind <
          static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData) ||
      fixed.kind > static_cast<std::uint32_t>(RenderTapeControlKind::DeviceLost)) {
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const bool terminal =
      (kind == RenderTapeControlKind::Reset ||
       kind == RenderTapeControlKind::DeviceLost) &&
      fixed.disposition ==
          static_cast<std::uint32_t>(RenderTapeControlDisposition::Terminal);
  if (fixed.controlBytes != controlPayload.size()) {
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const auto status = reserveEvent(sizeof(RenderTapeOrderedControlHeader) +
                                   controlPayload.size());
  if (status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  try {
    builder_.appendOrderedControl(fixed, controlPayload);
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeOrderedControlHeader) +
                   controlPayload.size();
    if (terminal) {
      abortInternal();
      return RenderTapeCaptureStatus::Terminal;
    }
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::completePresent(
    std::uint64_t presentOrdinal, std::uint64_t completionOrdinal,
    RenderTapeDigestValidity digestValidity, RenderTapeDigest expectedDigest,
    std::span<const std::byte> oracleAttachments) {
  if (const auto status = requireCapturing();
      status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  if (!presentChunkSeen_ || oracleAttachments.size() %
                                sizeof(RenderTapeOracleAttachment) != 0u) {
    abortInternal();
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const auto status = reserveEvent(sizeof(RenderTapePresentCompleteHeader) +
                                   oracleAttachments.size());
  if (status != RenderTapeCaptureStatus::Accepted) {
    abortInternal();
    return status;
  }
  try {
    builder_.appendPresentComplete(presentOrdinal, completionOrdinal,
                                   digestValidity, expectedDigest,
                                   oracleAttachments);
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapePresentCompleteHeader) +
                   oracleAttachments.size();
    const auto candidate = builder_.seal();
    RenderTapeValidationScratch scratch{};
    ImportedRenderTapeView imported{};
    const auto validation = validateRenderTape(candidate, catalogue_, &imported,
                                               scratch);
    validationStatus_ = validation.status;
    if (!validation.valid()) {
      abortInternal();
      return RenderTapeCaptureStatus::ValidationFailed;
    }
    sealedArtifact_ = candidate;
    state_ = RenderTapeCaptureState::Sealed;
    return RenderTapeCaptureStatus::Complete;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

void RenderTapeCaptureSession::abortInternal() noexcept {
  state_ = RenderTapeCaptureState::Aborted;
  sealedArtifact_.clear();
}

void RenderTapeCaptureSession::abort() noexcept {
  abortInternal();
}

} // namespace dxmt9::d3d9
