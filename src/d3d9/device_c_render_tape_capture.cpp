#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_identity.hpp"

#include "device_c_render_tape_descriptors.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace dxmt9::d3d9 {

namespace {

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

bool validIdentity(const D9CWireObjectIdentity& identity) noexcept {
  return identity.kind <= D9C_CHUNK_HANDLE_KIND_QUERY &&
         identity.generation != 0u && identity.objectId != 0u;
}

bool sameIdentity(const D9CWireObjectIdentity& a,
                  const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

bool sameObjectDefinitionMetadata(
    const auto& slot, std::uint32_t descriptorKind,
    std::uint64_t immutableBytes, const RenderTapeDigest& immutableDigest,
    std::uint64_t expectedContentBytes,
    std::uint32_t expectedContentCount) noexcept {
  return slot.descriptorKind == descriptorKind &&
         slot.immutableBytes == immutableBytes &&
         slot.immutableDigest == immutableDigest &&
         slot.expectedContentBytes == expectedContentBytes &&
         slot.expectedContentCount == expectedContentCount;
}

bool mutationCapableIdentity(
    const D9CWireObjectIdentity& identity) noexcept {
  return identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE ||
         identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE ||
         identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER;
}

} // namespace

const char* renderTapeObjectDefineDispositionName(
    RenderTapeObjectDefineDisposition disposition) noexcept {
  switch (disposition) {
  case RenderTapeObjectDefineDisposition::Appended:
    return "appended";
  case RenderTapeObjectDefineDisposition::IdempotentSurfaceAlias:
    return "idempotent_surface_alias";
  case RenderTapeObjectDefineDisposition::InvalidState:
    return "invalid_state";
  case RenderTapeObjectDefineDisposition::InvalidIdentity:
    return "invalid_identity";
  case RenderTapeObjectDefineDisposition::InvalidDescriptor:
    return "invalid_descriptor";
  case RenderTapeObjectDefineDisposition::InvalidExpectedContent:
    return "invalid_expected_content";
  case RenderTapeObjectDefineDisposition::MissingImmutableBlob:
    return "missing_immutable_blob";
  case RenderTapeObjectDefineDisposition::ExactIdentityConflict:
    return "exact_identity_conflict";
  case RenderTapeObjectDefineDisposition::OverlappingLiveGeneration:
    return "overlapping_live_generation";
  case RenderTapeObjectDefineDisposition::StaleOrEqualGeneration:
    return "stale_or_equal_generation";
  case RenderTapeObjectDefineDisposition::CapacityExceeded:
    return "capacity_exceeded";
  case RenderTapeObjectDefineDisposition::AllocationFailed:
    return "allocation_failed";
  }
  return "unknown";
}

RenderTapeCaptureSession::RenderTapeCaptureSession(
    bool enabled, RenderTapeCaptureLimits limits, std::uint32_t profile)
    : enabled_(enabled), limits_(limits),
      state_(RenderTapeCaptureState::Disabled), builder_(profile) {
  limits_.maxBlobBytes =
      std::min(limits_.maxBlobBytes, kRenderTapeHardMaxBlobBytes);
}

RenderTapeDigest RenderTapeCaptureSession::sha256(
    std::span<const std::byte> bytes) {
  std::array<std::uint32_t, 8> state = {0x6a09e667u, 0xbb67ae85u,
                                        0x3c6ef372u, 0xa54ff53au,
                                        0x510e527fu, 0x9b05688cu,
                                        0x1f83d9abu, 0x5be0cd19u};
  const std::uint64_t bitCount = static_cast<std::uint64_t>(bytes.size()) * 8u;
  const std::size_t padded = ((bytes.size() + 9u + 63u) / 64u) * 64u;
  std::vector<std::byte> message(padded);
  std::memcpy(message.data(), bytes.data(), bytes.size());
  message[bytes.size()] = std::byte{0x80};
  for (unsigned i = 0u; i < 8u; ++i) {
    message[padded - 1u - i] =
        static_cast<std::byte>((bitCount >> (i * 8u)) & 0xffu);
  }
  for (std::size_t offset = 0u; offset < message.size(); offset += 64u) {
    std::array<std::uint32_t, 64> schedule{};
    for (unsigned i = 0u; i < 16u; ++i) {
      const auto at = offset + i * 4u;
      schedule[i] = (static_cast<std::uint32_t>(message[at]) << 24u) |
                    (static_cast<std::uint32_t>(message[at + 1u]) << 16u) |
                    (static_cast<std::uint32_t>(message[at + 2u]) << 8u) |
                    static_cast<std::uint32_t>(message[at + 3u]);
    }
    for (unsigned i = 16u; i < 64u; ++i) {
      const auto s0 = rotr(schedule[i - 15u], 7u) ^
                      rotr(schedule[i - 15u], 18u) ^
                      (schedule[i - 15u] >> 3u);
      const auto s1 = rotr(schedule[i - 2u], 17u) ^
                      rotr(schedule[i - 2u], 19u) ^
                      (schedule[i - 2u] >> 10u);
      schedule[i] = schedule[i - 16u] + s0 + schedule[i - 7u] + s1;
    }
    auto working = state;
    for (unsigned i = 0u; i < 64u; ++i) {
      const auto s1 = rotr(working[4], 6u) ^ rotr(working[4], 11u) ^
                      rotr(working[4], 25u);
      const auto choice = (working[4] & working[5]) ^
                          ((~working[4]) & working[6]);
      const auto temp1 = working[7] + s1 + choice +
                         kSha256RoundConstants[i] + schedule[i];
      const auto s0 = rotr(working[0], 2u) ^ rotr(working[0], 13u) ^
                      rotr(working[0], 22u);
      const auto majority = (working[0] & working[1]) ^
                            (working[0] & working[2]) ^
                            (working[1] & working[2]);
      const auto temp2 = s0 + majority;
      working = {temp1 + temp2, working[0], working[1], working[2],
                 working[3] + temp1, working[4], working[5], working[6]};
    }
    for (unsigned i = 0u; i < 8u; ++i) {
      state[i] += working[i];
    }
  }
  RenderTapeDigest result{};
  for (unsigned i = 0u; i < 8u; ++i) {
    result[i * 4u] = static_cast<std::byte>(state[i] >> 24u);
    result[i * 4u + 1u] = static_cast<std::byte>(state[i] >> 16u);
    result[i * 4u + 2u] = static_cast<std::byte>(state[i] >> 8u);
    result[i * 4u + 3u] = static_cast<std::byte>(state[i]);
  }
  return result;
}

RenderTapeCaptureStatus RenderTapeCaptureSession::arm(
    std::span<const std::byte> bootstrapOverlay,
    std::span<const RenderTapeBlob> blobs,
    std::span<const std::byte> gammaRamp) {
  if (!enabled_) {
    return RenderTapeCaptureStatus::Disabled;
  }
  if (state_ != RenderTapeCaptureState::Disabled &&
      state_ != RenderTapeCaptureState::Aborted) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  if (bootstrapOverlay.empty() || blobs.size() > limits_.maxBlobEntries ||
      (!gammaRamp.empty() && gammaRamp.size() != kRenderTapeGammaRampBytes)) {
    return RenderTapeCaptureStatus::InvalidInput;
  }
  if (builder_.profile() != kRenderTapeProfileFrame &&
      builder_.profile() != kRenderTapeProfileSequence) {
    return RenderTapeCaptureStatus::InvalidInput;
  }

  try {
    catalogue_.blobs.assign(blobs.begin(), blobs.end());
    publishedBlobs_.clear();
    publicationBundle_ = {};
    objects_.clear();
    sealedArtifact_.clear();
    eventCount_ = 0u;
    eventBytes_ = 0u;
    blobBytes_ = 0u;
    presentChunkSeen_ = false;
    presentCompletionCount_ = 0u;
    validationStatus_ = RenderTapeValidationStatus::Valid;
    validationResult_ = RenderTapeValidationResult{
        .status = RenderTapeValidationStatus::Valid};
    builder_ = RenderTapeBuilder(builder_.profile());
    const auto status = reserveEvent(sizeof(RenderTapeBootstrapHeader) +
                                     bootstrapOverlay.size() + gammaRamp.size());
    if (status != RenderTapeCaptureStatus::Accepted) {
      return status;
    }
    builder_.appendBootstrapState(bootstrapOverlay, gammaRamp);
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeBootstrapHeader) + bootstrapOverlay.size() +
                   gammaRamp.size();
    state_ = RenderTapeCaptureState::Armed;
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::armWithBlobs(
    std::span<const std::byte> bootstrapOverlay,
    std::span<const RenderTapeCaptureBlob> blobs,
    std::span<const std::byte> gammaRamp) {
  std::uint64_t totalBlobBytes = 0u;
  std::vector<RenderTapeBlob> catalogue;
  std::vector<RenderTapePublishedBlob> owned;
  try {
    const auto reserveCount = std::min<std::size_t>(
        blobs.size(), limits_.maxBlobEntries);
    catalogue.reserve(reserveCount);
    owned.reserve(reserveCount);
    for (const auto& blob : blobs) {
      const bool duplicate = std::any_of(
          owned.begin(), owned.end(), [&](const auto& published) {
            return published.bytes == blob.bytes;
          });
      if (duplicate) {
        continue;
      }
      if (owned.size() >= limits_.maxBlobEntries) {
        return RenderTapeCaptureStatus::CapacityExceeded;
      }
      const auto blobBytes = static_cast<std::uint64_t>(blob.bytes.size());
      if (blobBytes > limits_.maxBlobBytes ||
          totalBlobBytes > limits_.maxBlobBytes - blobBytes) {
        return RenderTapeCaptureStatus::CapacityExceeded;
      }
      const auto digest = sha256(blob.bytes);
      catalogue.push_back(RenderTapeBlob{.digest = digest,
                                         .size = blob.bytes.size(),
                                         .verified = 1u});
      owned.push_back(RenderTapePublishedBlob{.digest = digest,
                                              .bytes = blob.bytes});
      totalBlobBytes += blobBytes;
    }
  } catch (...) {
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
  const auto status = arm(bootstrapOverlay, catalogue, gammaRamp);
  if (status == RenderTapeCaptureStatus::Accepted) {
    publishedBlobs_ = std::move(owned);
    blobBytes_ = totalBlobBytes;
  }
  return status;
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
  RenderTapeDigest digestCopy{};
  std::memcpy(digestCopy.data(), digest.data(), kRenderTapeDigestSize);
  if (hasVerifiedBlob(digestCopy, size)) {
    return RenderTapeCaptureStatus::Accepted;
  }
  if (catalogue_.blobs.size() >= limits_.maxBlobEntries) {
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
  try {
    catalogue_.blobs.push_back(RenderTapeBlob{
        .digest = digestCopy,
        .size = size,
        .verified = 1u,
    });
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::registerBlobBytes(
    std::span<const std::byte> bytes, RenderTapeDigest* digestOut) {
  if (!enabled_) {
    return RenderTapeCaptureStatus::Disabled;
  }
  if (state_ != RenderTapeCaptureState::Armed &&
      state_ != RenderTapeCaptureState::Capturing) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  const auto alreadyPublished = std::find_if(
      publishedBlobs_.begin(), publishedBlobs_.end(), [&](const auto& blob) {
        return blob.bytes.size() == bytes.size() &&
               std::equal(blob.bytes.begin(), blob.bytes.end(), bytes.begin());
      });
  if (alreadyPublished != publishedBlobs_.end()) {
    if (digestOut) {
      *digestOut = alreadyPublished->digest;
    }
    return RenderTapeCaptureStatus::Accepted;
  }
  const auto incomingBytes = static_cast<std::uint64_t>(bytes.size());
  if (incomingBytes > limits_.maxBlobBytes ||
      blobBytes_ > limits_.maxBlobBytes - incomingBytes) {
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
  const auto digest = sha256(bytes);
  if (digestOut) {
    *digestOut = digest;
  }
  const bool alreadyCatalogued = hasVerifiedBlob(digest, bytes.size());
  if (!alreadyCatalogued) {
    const auto status = registerVerifiedBlob(
        std::span<const std::byte, kRenderTapeDigestSize>(digest),
        bytes.size());
    if (status != RenderTapeCaptureStatus::Accepted) {
      return status;
    }
  }
  try {
    publishedBlobs_.push_back(RenderTapePublishedBlob{
        .digest = digest, .bytes = std::vector<std::byte>(bytes.begin(), bytes.end())});
    blobBytes_ += incomingBytes;
  } catch (...) {
    if (!alreadyCatalogued && !catalogue_.blobs.empty()) {
      catalogue_.blobs.pop_back();
    }
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
  return RenderTapeCaptureStatus::Accepted;
}

RenderTapeCaptureStatus RenderTapeCaptureSession::resourceMutationBytes(
    const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
    std::uint32_t subresource, std::uint64_t byteOffset,
    std::span<const std::byte> bytes,
    RenderTapeBufferMutationDisposition bufferDisposition) {
  RenderTapeDigest digest{};
  const auto status = registerBlobBytes(bytes, &digest);
  if (status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  return resourceMutation(
      identity, kind, subresource, byteOffset, bytes.size(),
      std::span<const std::byte, kRenderTapeDigestSize>(digest),
      bufferDisposition);
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
    RenderTapeDigest immutableDigest, std::uint64_t expectedContentBytes,
    std::uint32_t expectedContentCount,
    RenderTapeObjectDefineDisposition* disposition) {
  const auto decide = [&](RenderTapeObjectDefineDisposition value) {
    if (disposition) *disposition = value;
  };
  if (const auto status = requireCapturing();
      status != RenderTapeCaptureStatus::Accepted) {
    decide(RenderTapeObjectDefineDisposition::InvalidState);
    return status;
  }
  if (!validIdentity(identity)) {
    decide(RenderTapeObjectDefineDisposition::InvalidIdentity);
    return RenderTapeCaptureStatus::InvalidInput;
  }
  if (descriptorKind == 0u || descriptor.empty()) {
    decide(RenderTapeObjectDefineDisposition::InvalidDescriptor);
    return RenderTapeCaptureStatus::InvalidInput;
  }
  if ((expectedContentBytes == 0u) != (expectedContentCount == 0u) ||
      (expectedContentBytes != 0u && !mutationCapableIdentity(identity))) {
    decide(RenderTapeObjectDefineDisposition::InvalidExpectedContent);
    return RenderTapeCaptureStatus::InvalidInput;
  }
  if (immutableBytes != 0u &&
      !hasVerifiedBlob(immutableDigest, immutableBytes)) {
    decide(RenderTapeObjectDefineDisposition::MissingImmutableBlob);
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const auto exact = std::find_if(
      objects_.begin(), objects_.end(), [&](const auto& candidate) {
        return sameIdentity(candidate.identity, identity);
      });
  if (exact != objects_.end()) {
    if (exact->live &&
        identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
        descriptorKind == static_cast<std::uint32_t>(
                              RenderTapeDescriptorKind::Surface) &&
        renderTapeSurfaceAliasDescriptorsEqual(exact->descriptor, descriptor) &&
        sameObjectDefinitionMetadata(*exact, descriptorKind, immutableBytes,
                                     immutableDigest, expectedContentBytes,
                                     expectedContentCount)) {
      // Texture level wrappers may be materialized lazily after the arm seed.
      // The parent retains this alias identity, so an exact repeat is a
      // lifecycle observation, not a second ObjectDefine event.
      decide(RenderTapeObjectDefineDisposition::IdempotentSurfaceAlias);
      return RenderTapeCaptureStatus::Accepted;
    }
    decide(RenderTapeObjectDefineDisposition::ExactIdentityConflict);
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const auto logicalSlot = renderTapeLogicalObjectSlot(identity, descriptor);
  if (logicalSlot.malformedSurfaceDescriptor) {
    decide(RenderTapeObjectDefineDisposition::InvalidDescriptor);
    return RenderTapeCaptureStatus::InvalidInput;
  }
  bool sameWireSlotSeen = false;
  std::uint32_t latestWireGeneration = 0u;
  for (auto candidate = objects_.begin(); candidate != objects_.end();
       ++candidate) {
    if (renderTapeSameWireObject(candidate->identity, identity)) {
      sameWireSlotSeen = true;
      latestWireGeneration =
          std::max(latestWireGeneration, candidate->identity.generation);
    }
    const auto relation =
        renderTapeLogicalSlotRelation(candidate->logicalSlot, logicalSlot);
    if (relation == RenderTapeLogicalSlotRelation::Different)
      continue;
    if (relation == RenderTapeLogicalSlotRelation::AliasDescriptorMismatch) {
      decide(RenderTapeObjectDefineDisposition::InvalidDescriptor);
      return RenderTapeCaptureStatus::InvalidInput;
    }
    if (candidate->live) {
      decide(RenderTapeObjectDefineDisposition::OverlappingLiveGeneration);
      return RenderTapeCaptureStatus::InvalidInput;
    }
  }
  if (sameWireSlotSeen && identity.generation <= latestWireGeneration) {
    decide(RenderTapeObjectDefineDisposition::StaleOrEqualGeneration);
    return RenderTapeCaptureStatus::InvalidInput;
  }
  const auto status = reserveEvent(sizeof(RenderTapeObjectDefineHeader) +
                                   descriptor.size());
  if (status != RenderTapeCaptureStatus::Accepted) {
    decide(RenderTapeObjectDefineDisposition::CapacityExceeded);
    return status;
  }
  try {
    builder_.appendObjectDefine(identity, descriptorKind, descriptor,
                                immutableBytes, immutableDigest,
                                expectedContentBytes, expectedContentCount);
    ObjectSlot next{
        .identity = identity,
        .logicalSlot = logicalSlot,
        .descriptorKind = descriptorKind,
        .descriptor = std::vector<std::byte>(descriptor.begin(),
                                             descriptor.end()),
        .immutableBytes = immutableBytes,
        .immutableDigest = immutableDigest,
        .expectedContentBytes = expectedContentBytes,
        .expectedContentCount = expectedContentCount,
        .live = true,
    };
    objects_.push_back(std::move(next));
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeObjectDefineHeader) + descriptor.size();
    decide(RenderTapeObjectDefineDisposition::Appended);
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    abortInternal();
    decide(RenderTapeObjectDefineDisposition::AllocationFailed);
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
    std::span<const std::byte, kRenderTapeDigestSize> digest,
    RenderTapeBufferMutationDisposition bufferDisposition) {
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
                                    byteSize, digest, bufferDisposition);
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
      fixed.kind > static_cast<std::uint32_t>(RenderTapeControlKind::GammaRampSet)) {
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
    std::span<const std::byte> oracleAttachments,
    std::span<const std::byte> outputOracle,
    std::span<const std::byte> sourceOracle) {
  if (const auto status = requireCapturing();
      status != RenderTapeCaptureStatus::Accepted) {
    return status;
  }
  if (!presentChunkSeen_ || oracleAttachments.size() %
                                sizeof(RenderTapeOracleAttachment) != 0u ||
      (!outputOracle.empty() &&
       (digestValidity != RenderTapeDigestValidity::Sha256 ||
       sha256(outputOracle) != expectedDigest))) {
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

    ++presentCompletionCount_;
    const auto expectedCompletions =
        profile() == kRenderTapeProfileSequence ? 2u : 1u;
    if (presentCompletionCount_ < expectedCompletions) {
      // A sequence interval is a journal boundary, not a publication
      // boundary. Keep the owner capturing so the next digest-backed
      // mutation and Present are appended in order; only interval 2 seals.
      presentChunkSeen_ = false;
      return RenderTapeCaptureStatus::Accepted;
    }
    if (presentCompletionCount_ > expectedCompletions) {
      abortInternal();
      return RenderTapeCaptureStatus::InvalidState;
    }
    const auto candidate = builder_.seal();
    RenderTapeValidationScratch scratch{};
    ImportedRenderTapeView imported{};
    const auto validation = validateRenderTape(candidate, catalogue_, &imported,
                                               scratch);
    validationResult_ = validation;
    validationStatus_ = validation.status;
    if (!validation.valid()) {
      abortInternal();
      return RenderTapeCaptureStatus::ValidationFailed;
    }
    sealedArtifact_ = candidate;
    publicationBundle_.events = sealedArtifact_;
    publicationBundle_.blobs = publishedBlobs_;
    publicationBundle_.outputOracle.assign(outputOracle.begin(),
                                            outputOracle.end());
    publicationBundle_.sourceOracle.assign(sourceOracle.begin(),
                                           sourceOracle.end());
    state_ = RenderTapeCaptureState::Sealed;
    return RenderTapeCaptureStatus::Complete;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::attachCaptureIdentity(
    std::uint64_t captureToken, std::uint64_t presentOrdinal,
    std::span<const RenderTapeIdentitySource> sources,
    std::span<const RenderTapeIdentityRange> ranges,
    RenderTapeIdentityEventSettlement settlement,
    std::span<const D9CRenderTapeIdentitySettlementEntry> settlements) {
  identityValidationResult_ = {};
  if (state_ != RenderTapeCaptureState::Sealed || captureToken == 0u ||
      presentOrdinal == 0u || sources.empty() || ranges.empty() ||
      !publicationBundle_.identity.empty()) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  try {
    if (settlements.empty()) {
      abortInternal();
      return RenderTapeCaptureStatus::ValidationFailed;
    }
    std::vector<RenderTapeIdentitySettlement> sidecarSettlements;
    sidecarSettlements.reserve(settlements.size());
    for (const auto& item : settlements) {
      sidecarSettlements.push_back(RenderTapeIdentitySettlement{
          .eventOrdinal = item.eventOrdinal,
          .rawOrdinal = item.rawOrdinal,
          .buildGeneration = item.buildGeneration,
          .firstSourceOrdinal = item.firstSourceOrdinal,
          .tailSeqId = item.tailSeqId,
          .sourceCount = item.sourceCount,
          .reserved0 = item.reserved0,
      });
    }
    if (!validateRenderTapeIdentitySettlements(sources, sidecarSettlements)) {
      abortInternal();
      return RenderTapeCaptureStatus::ValidationFailed;
    }
    auto identity = buildRenderTapeIdentity(
        sealedArtifact_, catalogue_, captureToken, presentOrdinal,
        captureToken, RenderTapeIdentityAuthority::Capture, sources, ranges,
        &identityValidationResult_, sidecarSettlements);
    if (identity.empty()) {
      abortInternal();
      return RenderTapeCaptureStatus::ValidationFailed;
    }
    const auto& finalIdentitySettlement = sidecarSettlements.back();
    if (settlement.count != 1u || settlement.eventOrdinal == 0u ||
        settlement.sourceOrdinal == 0u || settlement.seqId == 0u ||
        finalIdentitySettlement.eventOrdinal != settlement.eventOrdinal ||
        finalIdentitySettlement.tailSeqId != settlement.seqId ||
        finalIdentitySettlement.sourceCount == 0u ||
        finalIdentitySettlement.firstSourceOrdinal == 0u ||
        sources.back().eventOrdinal != settlement.eventOrdinal ||
        sources.back().sourceOrdinal != settlement.sourceOrdinal ||
        sources.back().seqId != settlement.seqId) {
      abortInternal();
      return RenderTapeCaptureStatus::ValidationFailed;
    }
    publicationBundle_.identity = std::move(identity);
    publicationBundle_.identitySettlement = settlement;
    publicationBundle_.identitySettlements.assign(settlements.begin(),
                                                  settlements.end());
    return RenderTapeCaptureStatus::Complete;
  } catch (...) {
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

void RenderTapeCaptureSession::abortInternal() noexcept {
  state_ = RenderTapeCaptureState::Aborted;
  sealedArtifact_.clear();
  publicationBundle_ = {};
  blobBytes_ = 0u;
}

void RenderTapeCaptureSession::abort() noexcept {
  abortInternal();
}

} // namespace dxmt9::d3d9
