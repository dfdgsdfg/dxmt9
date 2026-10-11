#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_identity_transfer_v4.hpp"
#include "device_c_render_tape_identity.hpp"

#include "device_c_render_tape_descriptors.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

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

// Called only with an active capture-path exception. Classification itself
// must not allocate, especially when the original failure was bad_alloc.
const char* captureMutationExceptionKind() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc&) {
    return "bad_alloc";
  } catch (const std::length_error&) {
    return "length_error";
  } catch (const std::invalid_argument&) {
    return "invalid_argument";
  } catch (const RenderTapePayloadReadError&) {
    return "scratch_io";
  } catch (const std::exception&) {
    return "std_exception";
  } catch (...) {
    return "unknown";
  }
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
  limits_.maxEvents = std::min(limits_.maxEvents, kRenderTapeHardMaxEvents);
  limits_.maxBlobEntries = std::min(limits_.maxBlobEntries, kRenderTapeHardMaxBlobEntries);
  limits_.maxBlobBytes =
      std::min(limits_.maxBlobBytes, kRenderTapeHardMaxBlobBytes);
  if (enabled_) payloadArena_ = std::make_shared<RenderTapeScratchArena>();
}

RenderTapeDigest RenderTapeCaptureSession::sha256(
    std::span<const std::byte> bytes) {
  return sha256(RenderTapeByteView(bytes));
}

RenderTapeDigest RenderTapeCaptureSession::sha256(RenderTapeByteView bytes) {
  std::array<std::uint32_t, 8> state = {0x6a09e667u, 0xbb67ae85u,
                                        0x3c6ef372u, 0xa54ff53au,
                                        0x510e527fu, 0x9b05688cu,
                                        0x1f83d9abu, 0x5be0cd19u};
  const std::uint64_t bitCount = static_cast<std::uint64_t>(bytes.size()) * 8u;
  const auto compress = [&](std::span<const std::byte, 64u> block) {
    std::array<std::uint32_t, 64> schedule{};
    for (unsigned i = 0u; i < 16u; ++i) {
      const auto at = i * 4u;
      schedule[i] = (static_cast<std::uint32_t>(block[at]) << 24u) |
                    (static_cast<std::uint32_t>(block[at + 1u]) << 16u) |
                    (static_cast<std::uint32_t>(block[at + 2u]) << 8u) |
                    static_cast<std::uint32_t>(block[at + 3u]);
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
  };
  // Borrow vector inputs directly; scratch inputs use one bounded transient
  // window. Neither path materializes another complete captured version.
  const std::size_t fullBytes = bytes.size() - bytes.size() % 64u;
  if (bytes.data()) {
    for (std::size_t offset = 0u; offset < fullBytes; offset += 64u)
      compress(std::span<const std::byte, 64u>(bytes.data() + offset, 64u));
  } else {
    std::array<std::byte, kRenderTapePayloadWindowBytes> window{};
    for (std::size_t offset = 0u; offset < fullBytes;) {
      const auto count = std::min(window.size(), fullBytes - offset);
      if (!bytes.readAt(offset, std::span(window).first(count))) throw RenderTapePayloadReadError();
      for (std::size_t at = 0u; at < count; at += 64u)
        compress(std::span<const std::byte, 64u>(window.data() + at, 64u));
      offset += count;
    }
  }
  std::array<std::byte, 128u> tail{};
  const std::size_t remaining = bytes.size() - fullBytes;
  if (remaining != 0u && !bytes.readAt(fullBytes, std::span(tail).first(remaining)))
    throw RenderTapePayloadReadError();
  tail[remaining] = std::byte{0x80};
  const std::size_t tailBytes = remaining < 56u ? 64u : 128u;
  for (unsigned i = 0u; i < 8u; ++i) {
    tail[tailBytes - 1u - i] =
        static_cast<std::byte>((bitCount >> (i * 8u)) & 0xffu);
  }
  compress(std::span<const std::byte, 64u>(tail.data(), 64u));
  if (tailBytes == 128u) {
    compress(std::span<const std::byte, 64u>(tail.data() + 64u, 64u));
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
  armFailure_ = {};
  RenderTapeCaptureArmFailure context{
      .layer = "arm", .phase = "input_validation", .inputBlobCount = blobs.size(),
      .uniqueBlobCount = blobs.size(), .eventCount = eventCount_,
      .eventBytes = eventBytes_};
  for (const auto& blob : blobs) {
    context.cumulativeBlobBytes = blob.size >
            std::numeric_limits<std::uint64_t>::max() - context.cumulativeBlobBytes
        ? std::numeric_limits<std::uint64_t>::max()
        : context.cumulativeBlobBytes + blob.size;
  }
  const auto fail = [&](const char* reason, const char* exception = "none") {
    context.reason = reason;
    context.exceptionKind = exception;
    armFailure_ = context;
  };
  if (!enabled_) {
    fail("disabled");
    return RenderTapeCaptureStatus::Disabled;
  }
  if (state_ != RenderTapeCaptureState::Disabled &&
      state_ != RenderTapeCaptureState::Aborted) {
    fail("invalid_state");
    return RenderTapeCaptureStatus::InvalidState;
  }
  if (bootstrapOverlay.empty() || blobs.size() > limits_.maxBlobEntries ||
      (!gammaRamp.empty() && gammaRamp.size() != kRenderTapeGammaRampBytes)) {
    fail(blobs.size() > limits_.maxBlobEntries ? "blob_entry_limit" : "invalid_input");
    return RenderTapeCaptureStatus::InvalidInput;
  }
  if (builder_.profile() != kRenderTapeProfileFrame &&
      builder_.profile() != kRenderTapeProfileSequence) {
    fail("invalid_profile");
    return RenderTapeCaptureStatus::InvalidInput;
  }

  try {
    context.phase = "catalogue_copy";
    catalogue_.blobs.assign(blobs.begin(), blobs.end());
    context.phase = "session_reset";
    lastAbortAccounting_ = {};
    mutationFailure_ = {};
    publishedBlobs_.clear();
    publicationBundle_ = {};
    identityV4ValidationStatus_ = RenderTapeIdentityV4Status::InvalidHeader;
    topologyV1ValidationStatus_ = RenderTapeExecutionTopologyV1Status::InvalidHeader;
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
    context.phase = "builder_reset";
    builder_ = RenderTapeBuilder(builder_.profile());
    context.phase = "bootstrap_event_reserve";
    context.eventPayloadBytes = sizeof(RenderTapeBootstrapHeader) +
                                bootstrapOverlay.size() + gammaRamp.size();
    context.eventCount = eventCount_;
    context.eventBytes = eventBytes_;
    const auto status = reserveEvent(sizeof(RenderTapeBootstrapHeader) +
                                     bootstrapOverlay.size() + gammaRamp.size());
    if (status != RenderTapeCaptureStatus::Accepted) {
      fail(eventCount_ >= limits_.maxEvents ? "event_count_limit"
          : context.eventPayloadBytes > std::numeric_limits<std::uint64_t>::max() - eventBytes_
              ? "event_bytes_overflow" : "event_byte_limit");
      return status;
    }
    context.phase = "bootstrap_append";
    builder_.appendBootstrapState(bootstrapOverlay, gammaRamp);
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeBootstrapHeader) + bootstrapOverlay.size() +
                   gammaRamp.size();
    state_ = RenderTapeCaptureState::Armed;
    return RenderTapeCaptureStatus::Accepted;
  } catch (const std::bad_alloc&) {
    fail("exception", "bad_alloc");
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  } catch (const std::length_error&) {
    fail("exception", "length_error");
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  } catch (const std::invalid_argument&) {
    fail("exception", "invalid_argument");
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  } catch (const std::exception&) {
    fail("exception", "std_exception");
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  } catch (...) {
    fail("exception", "unknown");
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::armWithBlobs(
    std::span<const std::byte> bootstrapOverlay,
    std::span<const RenderTapeCaptureBlob> blobs,
    std::span<const std::byte> gammaRamp) {
  return armWithBlobStorage(bootstrapOverlay, blobs, gammaRamp, nullptr);
}

RenderTapeCaptureStatus RenderTapeCaptureSession::armWithOwnedBlobs(
    std::span<const std::byte> bootstrapOverlay,
    std::vector<RenderTapeCaptureBlob>&& blobs,
    std::span<const std::byte> gammaRamp) {
  std::vector<RenderTapeCaptureBlob> transferred;
  transferred.swap(blobs);
  return armWithBlobStorage(bootstrapOverlay, transferred, gammaRamp, &transferred);
}

RenderTapeCaptureStatus RenderTapeCaptureSession::armWithBlobStorage(
    std::span<const std::byte> bootstrapOverlay,
    std::span<const RenderTapeCaptureBlob> blobs,
    std::span<const std::byte> gammaRamp,
    std::vector<RenderTapeCaptureBlob>* transferredBlobs) {
  armFailure_ = {};
  RenderTapeCaptureArmFailure context{
      .layer = transferredBlobs ? "arm_with_owned_blobs" : "arm_with_blobs",
      .phase = "catalogue_reserve",
      .inputBlobCount = blobs.size()};
  const auto fail = [&](const char* reason, const char* exception = "none") {
    context.reason = reason;
    context.exceptionKind = exception;
    armFailure_ = context;
  };
  std::uint64_t totalBlobBytes = 0u;
  std::vector<RenderTapeBlob> catalogue;
  std::vector<RenderTapePublishedBlob> owned;
  try {
    const auto reserveCount = std::min<std::size_t>(
        blobs.size(), limits_.maxBlobEntries);
    catalogue.reserve(reserveCount);
    context.phase = "owned_reserve";
    owned.reserve(reserveCount);
    for (std::size_t index = 0u; index < blobs.size(); ++index) {
      const auto& blob = blobs[index];
      const auto bytes = blob.readBytes();
      context.blobIndex = index;
      context.blobBytes = bytes.size();
      context.uniqueBlobCount = owned.size();
      context.cumulativeBlobBytes = totalBlobBytes;
      context.phase = "blob_deduplicate";
      if (blob.immutable.hasBacking() && blob.bytes.capacity() != 0u) {
        fail("ambiguous_blob_storage");
        return RenderTapeCaptureStatus::InvalidInput;
      }
      if (blob.retainedLease.valid()) {
        context.phase = "blob_lease_validation";
        if (blob.immutable.hasBacking() || !transferredBlobs ||
            blob.retainedLease.kind() != RenderTapeSnapshotBudgetKind::Retained ||
            blob.retainedLease.resourceBytes() != bytes.size() ||
            blob.retainedLease.chargedBytes() != bytes.size() ||
            !blob.retainedLease.boundPayloadMatches(blob.bytes.data(), bytes.size())) {
          fail(!transferredBlobs && !blob.immutable.hasBacking()
              ? "borrowed_retained_blob" : "invalid_retained_lease");
          return RenderTapeCaptureStatus::InvalidInput;
        }
      }
      context.phase = "blob_deduplicate";
      const bool duplicate = std::any_of(
          owned.begin(), owned.end(), [&](const auto& published) {
            return published.bytes.readBytes().equals(bytes);
          });
      if (duplicate) {
        continue;
      }
      if (owned.size() >= limits_.maxBlobEntries) {
        context.phase = "blob_entry_admission";
        fail("blob_entry_limit");
        return RenderTapeCaptureStatus::CapacityExceeded;
      }
      const auto blobBytes = static_cast<std::uint64_t>(bytes.size());
      if (blobBytes > limits_.maxBlobBytes ||
          totalBlobBytes > limits_.maxBlobBytes - blobBytes) {
        context.phase = "blob_byte_admission";
        fail("blob_byte_limit");
        return RenderTapeCaptureStatus::CapacityExceeded;
      }
      context.phase = "blob_digest";
      const auto digest = sha256(bytes);
      context.phase = "catalogue_append";
      catalogue.push_back(RenderTapeBlob{.digest = digest,
                                         .size = bytes.size(),
                                         .verified = 1u});
      if (blob.immutable.hasBacking()) {
        context.phase = "owned_blob_share";
        owned.push_back(RenderTapePublishedBlob{.digest = digest, .bytes = blob.immutable});
      } else if (transferredBlobs) {
        static_assert(std::is_nothrow_move_constructible_v<RenderTapePublishedBlob>);
        context.phase = "owned_blob_move";
        owned.push_back(RenderTapePublishedBlob{
            .digest = digest, .bytes = RenderTapeImmutablePayload(
                std::move((*transferredBlobs)[index].bytes), blob.retainedLease)});
      } else {
        context.phase = "owned_blob_copy";
        owned.push_back(RenderTapePublishedBlob{.digest = digest,
                                                .bytes = blob.bytes});
      }
      totalBlobBytes += blobBytes;
    }
  } catch (const std::bad_alloc&) {
    fail("exception", "bad_alloc");
    return RenderTapeCaptureStatus::CapacityExceeded;
  } catch (const std::length_error&) {
    fail("exception", "length_error");
    return RenderTapeCaptureStatus::CapacityExceeded;
  } catch (const RenderTapePayloadReadError&) {
    fail("exception", "scratch_io");
    return RenderTapeCaptureStatus::CapacityExceeded;
  } catch (const std::invalid_argument&) {
    fail("exception", "invalid_argument");
    return RenderTapeCaptureStatus::CapacityExceeded;
  } catch (const std::exception&) {
    fail("exception", "std_exception");
    return RenderTapeCaptureStatus::CapacityExceeded;
  } catch (...) {
    fail("exception", "unknown");
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
  const auto status = arm(bootstrapOverlay, catalogue, gammaRamp);
  if (status == RenderTapeCaptureStatus::Accepted) {
    publishedBlobs_ = std::move(owned);
    blobBytes_ = totalBlobBytes;
  } else {
    // Preserve the downstream layer/phase/reason, while retaining the
    // caller's raw input count and the wrapper's successfully staged totals.
    armFailure_.inputBlobCount = blobs.size();
    armFailure_.uniqueBlobCount = owned.size();
    armFailure_.cumulativeBlobBytes = totalBlobBytes;
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
  mutationFailure_ = {};
  auto context = mutationFailureContext("register_verified_blob");
  context.byteSize = size;
  const auto fail = [&](RenderTapeCaptureStatus status, const char* reason,
                        const char* exception = "none") {
    context.valid = true;
    context.status = status;
    context.reason = reason;
    context.exceptionKind = exception;
    mutationFailure_ = context;
    return status;
  };
  if (state_ != RenderTapeCaptureState::Armed &&
      state_ != RenderTapeCaptureState::Capturing) {
    return fail(RenderTapeCaptureStatus::InvalidState, "invalid_state");
  }
  context.phase = "blob_catalogue_lookup";
  RenderTapeDigest digestCopy{};
  std::memcpy(digestCopy.data(), digest.data(), kRenderTapeDigestSize);
  if (hasVerifiedBlob(digestCopy, size)) {
    return RenderTapeCaptureStatus::Accepted;
  }
  context.phase = "blob_entry_admission";
  if (catalogue_.blobs.size() >= limits_.maxBlobEntries) {
    return fail(RenderTapeCaptureStatus::CapacityExceeded, "blob_entry_limit");
  }
  try {
    context.phase = "blob_catalogue_append";
    catalogue_.blobs.push_back(RenderTapeBlob{
        .digest = digestCopy,
        .size = size,
        .verified = 1u,
    });
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    return fail(RenderTapeCaptureStatus::CapacityExceeded, "exception",
                captureMutationExceptionKind());
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::registerBlobBytes(
    std::span<const std::byte> bytes, RenderTapeDigest* digestOut) {
  return registerBlobStorage(RenderTapeByteView(bytes), digestOut, nullptr);
}

RenderTapeCaptureStatus RenderTapeCaptureSession::registerBlobBytes(
    RenderTapeByteView bytes, RenderTapeDigest* digestOut) {
  return registerBlobStorage(bytes, digestOut, nullptr);
}

RenderTapeCaptureStatus RenderTapeCaptureSession::registerOwnedBlobBytes(
    std::vector<std::byte>&& bytes, RenderTapeDigest* digestOut,
    const RenderTapeSnapshotBudgetLease& retainedLease) {
  // This local receipt also protects failure/dedup paths. Its declaration
  // precedes owned so a redundant or rejected vector is always freed first.
  const auto lease = retainedLease;
  std::vector<std::byte> owned;
  owned.swap(bytes);
  const auto status = registerBlobStorage(
      RenderTapeByteView(std::span<const std::byte>(owned)), digestOut, &owned,
      nullptr, lease);
  if (status != RenderTapeCaptureStatus::Accepted && enabled_)
    abortInternal();
  return status;
}

RenderTapeCaptureStatus RenderTapeCaptureSession::registerSharedBlobBytes(
    const RenderTapeImmutablePayload& bytes, RenderTapeDigest* digestOut) {
  return registerBlobStorage(bytes.readBytes(), digestOut, nullptr, &bytes);
}

RenderTapeCaptureStatus RenderTapeCaptureSession::registerShadowBlobBytes(
    RenderTapeCpuShadowContent& bytes, RenderTapeDigest* digestOut) {
  if (!enabled_) return RenderTapeCaptureStatus::Disabled;
  auto context = mutationFailureContext("register_shadow_blob_bytes");
  context.phase = "shadow_freeze";
  context.byteSize = bytes.size();
  if (state_ != RenderTapeCaptureState::Armed && state_ != RenderTapeCaptureState::Capturing) {
    context.valid = true;
    context.phase = "state_validation";
    context.reason = "invalid_state";
    context.status = RenderTapeCaptureStatus::InvalidState;
    mutationFailure_ = context;
    return context.status;
  }
  try {
    const auto immutable = bytes.freeze();
    return registerSharedBlobBytes(immutable, digestOut);
  } catch (...) {
    context.valid = true;
    context.reason = "exception";
    context.exceptionKind = captureMutationExceptionKind();
    context.status = RenderTapeCaptureStatus::CapacityExceeded;
    mutationFailure_ = context;
    return context.status;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::registerBlobStorage(
    RenderTapeByteView bytes, RenderTapeDigest* digestOut,
    std::vector<std::byte>* transferredBytes,
    const RenderTapeImmutablePayload* sharedBytes,
    const RenderTapeSnapshotBudgetLease& retainedLease) {
  if (!enabled_) {
    return RenderTapeCaptureStatus::Disabled;
  }
  mutationFailure_ = {};
  auto context = mutationFailureContext(
      sharedBytes ? "register_shared_blob_bytes"
          : transferredBytes ? "register_owned_blob_bytes" : "register_blob_bytes");
  context.byteSize = bytes.size();
  const auto fail = [&](RenderTapeCaptureStatus status, const char* reason,
                        const char* exception = "none") {
    context.valid = true;
    context.status = status;
    context.reason = reason;
    context.exceptionKind = exception;
    mutationFailure_ = context;
    return status;
  };
  if (state_ != RenderTapeCaptureState::Armed &&
      state_ != RenderTapeCaptureState::Capturing) {
    return fail(RenderTapeCaptureStatus::InvalidState, "invalid_state");
  }
  if (retainedLease.valid()) {
    context.phase = "blob_lease_validation";
    if (!transferredBytes || sharedBytes ||
        retainedLease.kind() != RenderTapeSnapshotBudgetKind::Retained ||
        retainedLease.resourceBytes() != bytes.size() ||
        retainedLease.chargedBytes() != bytes.size() ||
        !retainedLease.boundPayloadMatches(transferredBytes->data(), bytes.size()))
      return fail(RenderTapeCaptureStatus::InvalidInput, "invalid_retained_lease");
  }
  try {
    context.phase = "blob_deduplicate";
    const auto alreadyPublished = std::find_if(
        publishedBlobs_.begin(), publishedBlobs_.end(), [&](const auto& blob) {
          return blob.bytes.readBytes().equals(bytes);
        });
    if (alreadyPublished != publishedBlobs_.end()) {
      if (digestOut) {
        *digestOut = alreadyPublished->digest;
      }
      return RenderTapeCaptureStatus::Accepted;
    }
    const auto incomingBytes = static_cast<std::uint64_t>(bytes.size());
    context.phase = "blob_byte_admission";
    if (incomingBytes > limits_.maxBlobBytes ||
        blobBytes_ > limits_.maxBlobBytes - incomingBytes) {
      return fail(RenderTapeCaptureStatus::CapacityExceeded, "blob_byte_limit");
    }
    context.phase = "blob_digest";
    const auto digest = sha256(bytes);
    if (digestOut) {
      *digestOut = digest;
    }
    context.phase = "blob_catalogue_lookup";
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
      if (sharedBytes) {
        context.phase = "blob_publication_append";
        publishedBlobs_.push_back(RenderTapePublishedBlob{.digest = digest, .bytes = *sharedBytes});
      } else if (transferredBytes) {
        // The immutable owner's metadata allocation precedes its vector move;
        // later publication failure is fail-stop under the consuming API.
        // The original bytes are hashed above, never a caller digest.
        context.phase = "blob_payload_owner";
        RenderTapeImmutablePayload owned(std::move(*transferredBytes), retainedLease);
        context.phase = "blob_publication_append";
        publishedBlobs_.push_back(RenderTapePublishedBlob{.digest = digest, .bytes = std::move(owned)});
      } else {
        context.phase = "blob_payload_copy";
        RenderTapePublishedBlob owned{
            .digest = digest, .bytes = RenderTapeImmutablePayload::copyToScratch(bytes, payloadArena_)};
        context.phase = "blob_publication_append";
        publishedBlobs_.push_back(std::move(owned));
      }
      blobBytes_ += incomingBytes;
    } catch (...) {
      if (!alreadyCatalogued && !catalogue_.blobs.empty()) {
        catalogue_.blobs.pop_back();
      }
      return fail(RenderTapeCaptureStatus::CapacityExceeded, "exception",
                  captureMutationExceptionKind());
    }
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    return fail(RenderTapeCaptureStatus::CapacityExceeded, "exception",
                captureMutationExceptionKind());
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::resourceMutationBytes(
    const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
    std::uint32_t subresource, std::uint64_t byteOffset,
    std::span<const std::byte> bytes,
    RenderTapeBufferMutationDisposition bufferDisposition) {
  RenderTapeDigest digest{};
  const auto status = registerBlobBytes(bytes, &digest);
  if (status != RenderTapeCaptureStatus::Accepted) {
    // The inner layer owns the failure reason/phase; the wrapper supplies the
    // exact generation-qualified mutation that attempted blob admission.
    if (mutationFailure_.valid) {
      mutationFailure_.hasIdentity = true;
      mutationFailure_.identity = identity;
      mutationFailure_.kind = kind;
      mutationFailure_.bufferDisposition = bufferDisposition;
      mutationFailure_.subresource = subresource;
      mutationFailure_.byteOffset = byteOffset;
      mutationFailure_.byteSize = bytes.size();
      mutationFailure_.eventPayloadBytes = sizeof(RenderTapeResourceMutationHeader);
    }
    return status;
  }
  return resourceMutation(
      identity, kind, subresource, byteOffset, bytes.size(),
      std::span<const std::byte, kRenderTapeDigestSize>(digest),
      bufferDisposition);
}

RenderTapeCaptureStatus RenderTapeCaptureSession::resourceMutationOwnedBytes(
    const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
    std::uint32_t subresource, std::uint64_t byteOffset,
    std::vector<std::byte>&& bytes,
    RenderTapeBufferMutationDisposition bufferDisposition,
    const RenderTapeSnapshotBudgetLease& retainedLease) {
  const auto incomingBytes = static_cast<std::uint64_t>(bytes.size());
  RenderTapeDigest digest{};
  const auto blobStatus = registerOwnedBlobBytes(std::move(bytes), &digest, retainedLease);
  if (blobStatus != RenderTapeCaptureStatus::Accepted) {
    if (mutationFailure_.valid) {
      mutationFailure_.hasIdentity = true;
      mutationFailure_.identity = identity;
      mutationFailure_.kind = kind;
      mutationFailure_.bufferDisposition = bufferDisposition;
      mutationFailure_.subresource = subresource;
      mutationFailure_.byteOffset = byteOffset;
      mutationFailure_.byteSize = incomingBytes;
      mutationFailure_.eventPayloadBytes = sizeof(RenderTapeResourceMutationHeader);
    }
    return blobStatus;
  }
  const auto status = resourceMutation(
      identity, kind, subresource, byteOffset, incomingBytes, digest,
      bufferDisposition);
  if (status != RenderTapeCaptureStatus::Accepted && enabled_)
    abortInternal();
  return status;
}

RenderTapeCaptureStatus RenderTapeCaptureSession::resourceMutationShadowBytes(
    const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
    std::uint32_t subresource, std::uint64_t byteOffset,
    RenderTapeCpuShadowContent& bytes,
    RenderTapeBufferMutationDisposition bufferDisposition) {
  const auto incomingBytes = static_cast<std::uint64_t>(bytes.size());
  RenderTapeDigest digest{};
  const auto status = registerShadowBlobBytes(bytes, &digest);
  if (status != RenderTapeCaptureStatus::Accepted) {
    if (mutationFailure_.valid) {
      mutationFailure_.hasIdentity = true;
      mutationFailure_.identity = identity;
      mutationFailure_.kind = kind;
      mutationFailure_.bufferDisposition = bufferDisposition;
      mutationFailure_.subresource = subresource;
      mutationFailure_.byteOffset = byteOffset;
      mutationFailure_.byteSize = incomingBytes;
      mutationFailure_.eventPayloadBytes = sizeof(RenderTapeResourceMutationHeader);
    }
    return status;
  }
  return resourceMutation(identity, kind, subresource, byteOffset, incomingBytes,
                          digest, bufferDisposition);
}

RenderTapeCaptureMutationFailure RenderTapeCaptureSession::mutationFailureContext(
    const char* layer) const noexcept {
  return {
      .layer = layer, .phase = "state_validation", .captureState = state_,
      .publishedBlobCount = publishedBlobs_.size(),
      .accounting = {
          .valid = true, .eventCount = eventCount_, .eventBytes = eventBytes_,
          .blobCount = catalogue_.blobs.size(), .blobBytes = blobBytes_,
          .objectCount = objects_.size()}};
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
  if (!enabled_) {
    return RenderTapeCaptureStatus::Disabled;
  }
  mutationFailure_ = {};
  auto context = mutationFailureContext("resource_mutation");
  context.hasIdentity = true;
  context.identity = identity;
  context.kind = kind;
  context.bufferDisposition = bufferDisposition;
  context.subresource = subresource;
  context.byteOffset = byteOffset;
  context.byteSize = byteSize;
  context.eventPayloadBytes = sizeof(RenderTapeResourceMutationHeader);
  const auto fail = [&](RenderTapeCaptureStatus status, const char* reason,
                        const char* exception = "none") {
    context.valid = true;
    context.status = status;
    context.reason = reason;
    context.exceptionKind = exception;
    mutationFailure_ = context;
    return status;
  };
  if (const auto status = requireCapturing();
      status != RenderTapeCaptureStatus::Accepted) {
    return fail(status, "invalid_state");
  }
  context.phase = "event_validation";
  RenderTapeDigest copy{};
  std::memcpy(copy.data(), digest.data(), kRenderTapeDigestSize);
  if (!validIdentity(identity)) {
    return fail(RenderTapeCaptureStatus::InvalidInput, "invalid_identity");
  }
  if (!hasObject(identity, true)) {
    return fail(RenderTapeCaptureStatus::InvalidInput, "missing_live_object");
  }
  if (byteSize == 0u) {
    return fail(RenderTapeCaptureStatus::InvalidInput, "empty_mutation");
  }
  if (byteOffset > std::numeric_limits<std::uint64_t>::max() - byteSize) {
    return fail(RenderTapeCaptureStatus::InvalidInput, "mutation_extent_overflow");
  }
  if (!hasVerifiedBlob(copy, byteSize)) {
    return fail(RenderTapeCaptureStatus::InvalidInput, "missing_verified_blob");
  }
  context.phase = "event_reserve";
  // reserveEvent may release the owner. Classify using the pre-reserve values
  // already retained in context, not the now-zero live counters.
  const auto status = reserveEvent(sizeof(RenderTapeResourceMutationHeader));
  if (status != RenderTapeCaptureStatus::Accepted) {
    return fail(status,
        context.accounting.eventCount >= limits_.maxEvents ? "event_count_limit"
        : context.eventPayloadBytes >
                  std::numeric_limits<std::uint64_t>::max() - context.accounting.eventBytes
            ? "event_bytes_overflow" : "event_byte_limit");
  }
  try {
    context.phase = "event_append";
    builder_.appendResourceMutation(identity, kind, subresource, byteOffset,
                                    byteSize, digest, bufferDisposition);
    ++eventCount_;
    eventBytes_ += sizeof(RenderTapeResourceMutationHeader);
    return RenderTapeCaptureStatus::Accepted;
  } catch (...) {
    fail(RenderTapeCaptureStatus::CapacityExceeded, "exception",
         captureMutationExceptionKind());
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
      !publicationBundle_.identity.empty() || !publicationBundle_.executionTopology.empty()) {
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

RenderTapeCaptureStatus RenderTapeCaptureSession::attachCaptureIdentityV3(
    RenderTapeIdentityV3CaptureKey expectedCapture,
    std::uint64_t presentOrdinal,
    std::span<const std::byte> completeSidecar) {
  if (state_ != RenderTapeCaptureState::Sealed ||
      !publicationBundle_.identity.empty() || !publicationBundle_.executionTopology.empty()) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  identityV3ValidationStatus_ = RenderTapeIdentityV3Status::InvalidHeader;
  try {
    identityV3ValidationStatus_ = validateRenderTapeIdentityV3(
        sealedArtifact_, catalogue_, completeSidecar, nullptr, true);
    if (identityV3ValidationStatus_ != RenderTapeIdentityV3Status::Valid) {
      abortInternal();
      const bool capacityFailure =
          identityV3ValidationStatus_ == RenderTapeIdentityV3Status::AllocationFailed ||
          identityV3ValidationStatus_ == RenderTapeIdentityV3Status::CapacityExceeded;
      return capacityFailure
          ? RenderTapeCaptureStatus::CapacityExceeded
          : RenderTapeCaptureStatus::ValidationFailed;
    }
    RenderTapeIdentityV3Header header{};
    std::memcpy(&header, completeSidecar.data(), sizeof(header));
    if (!expectedCapture.token || !expectedCapture.epoch || !presentOrdinal ||
        header.captureToken != expectedCapture.token ||
        header.captureEpoch != expectedCapture.epoch ||
        header.presentOrdinal != presentOrdinal) {
      identityV3ValidationStatus_ = RenderTapeIdentityV3Status::InvalidHeader;
      abortInternal();
      return RenderTapeCaptureStatus::ValidationFailed;
    }
    // All fallible work precedes publication. Borrowed Unix bytes never
    // survive the call, and no v3 rows are coerced into v2 source tables.
    std::vector<std::byte> identity(completeSidecar.begin(), completeSidecar.end());
    publicationBundle_.identity = std::move(identity);
    return RenderTapeCaptureStatus::Complete;
  } catch (...) {
    identityV3ValidationStatus_ = RenderTapeIdentityV3Status::AllocationFailed;
    abortInternal();
    return RenderTapeCaptureStatus::CapacityExceeded;
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::attachCaptureIdentityV3(
    RenderTapeIdentityV3CaptureKey expectedCapture,
    std::uint64_t presentOrdinal,
    const RenderTapeIdentityV3View& snapshot) {
  if (state_ != RenderTapeCaptureState::Sealed ||
      !publicationBundle_.identity.empty() || !publicationBundle_.executionTopology.empty()) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  auto identity = buildRenderTapeIdentityV3(
      sealedArtifact_, catalogue_, snapshot, &identityV3ValidationStatus_);
  if (identity.empty()) {
    abortInternal();
    const bool capacityFailure =
        identityV3ValidationStatus_ == RenderTapeIdentityV3Status::AllocationFailed ||
        identityV3ValidationStatus_ == RenderTapeIdentityV3Status::CapacityExceeded;
    return capacityFailure
        ? RenderTapeCaptureStatus::CapacityExceeded
        : RenderTapeCaptureStatus::ValidationFailed;
  }
  return attachCaptureIdentityV3(expectedCapture, presentOrdinal,
                               std::span<const std::byte>(identity));
}

RenderTapeCaptureStatus RenderTapeCaptureSession::attachCaptureIdentityV4(
    RenderTapeIdentityV4CaptureKey expectedCapture,
    std::uint64_t expectedQueue, std::uint64_t presentOrdinal,
    std::span<const std::byte> completeIdentity,
    std::span<const std::byte> completeTopology,
    const RenderTapeActionWitnessV1& actionWitness,
    std::span<const std::byte> attachmentHistory) {
  if (state_ != RenderTapeCaptureState::Sealed ||
      !publicationBundle_.identity.empty() || !publicationBundle_.executionTopology.empty()) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  identityV4ValidationStatus_ = RenderTapeIdentityV4Status::InvalidHeader;
  topologyV1ValidationStatus_ = RenderTapeExecutionTopologyV1Status::InvalidHeader;
  const auto reject = [&]() {
    abortInternal();
    const bool capacity = identityV4ValidationStatus_ == RenderTapeIdentityV4Status::AllocationFailed ||
        identityV4ValidationStatus_ == RenderTapeIdentityV4Status::CapacityExceeded ||
        topologyV1ValidationStatus_ == RenderTapeExecutionTopologyV1Status::AllocationFailed ||
        topologyV1ValidationStatus_ == RenderTapeExecutionTopologyV1Status::CapacityExceeded;
    return capacity ? RenderTapeCaptureStatus::CapacityExceeded : RenderTapeCaptureStatus::ValidationFailed;
  };
  if (!renderTapeActionWitnessV1Valid(actionWitness) ||
      (actionWitness.evidence != static_cast<std::uint32_t>(
           RenderTapeActionWitnessEvidenceV1::LegacyUnknown) &&
       !renderTapeActionWitnessV1MatchesBinding(actionWitness,
           expectedCapture.token, expectedCapture.epoch, expectedQueue))) {
    return reject();
  }
  try {
    RenderTapeIdentityV4View identityView;
    identityV4ValidationStatus_ = validateRenderTapeIdentityV4(
        sealedArtifact_, catalogue_, completeIdentity, &identityView, true);
    if (identityV4ValidationStatus_ != RenderTapeIdentityV4Status::Valid) return reject();
    const auto& h = identityView.header;
    if (!attachmentHistory.empty() &&
        !renderTapeAttachmentHistoryMatchesCapture(attachmentHistory, h, actionWitness))
      return reject();
    if (!expectedCapture.token || !expectedCapture.epoch || !expectedQueue || !presentOrdinal ||
        h.captureToken != expectedCapture.token || h.captureEpoch != expectedCapture.epoch ||
        h.queue != expectedQueue || h.presentOrdinal != presentOrdinal) {
      identityV4ValidationStatus_ = RenderTapeIdentityV4Status::InvalidHeader;
      topologyV1ValidationStatus_ = RenderTapeExecutionTopologyV1Status::BindingMismatch;
      return reject();
    }
    topologyV1ValidationStatus_ = validateRenderTapeExecutionTopologyV1(
        sealedArtifact_, catalogue_, completeIdentity, completeTopology);
    if (topologyV1ValidationStatus_ != RenderTapeExecutionTopologyV1Status::Valid) return reject();
    // All allocations/validation precede this pair-and-witness commit. Both borrowed
    // inputs become independent owned bytes; no v2 mirrors or v3 Pass rows
    // are synthesized, and GPU/query output qualification remains separate.
    std::vector<std::byte> identity(completeIdentity.begin(), completeIdentity.end());
    std::vector<std::byte> topology(completeTopology.begin(), completeTopology.end());
    std::vector<std::byte> history(attachmentHistory.begin(), attachmentHistory.end());
    publicationBundle_.identity = std::move(identity);
    publicationBundle_.executionTopology = std::move(topology);
    publicationBundle_.actionWitness = actionWitness;
    publicationBundle_.attachmentHistory = std::move(history);
    return RenderTapeCaptureStatus::Complete;
  } catch (...) {
    identityV4ValidationStatus_ = RenderTapeIdentityV4Status::AllocationFailed;
    topologyV1ValidationStatus_ = RenderTapeExecutionTopologyV1Status::AllocationFailed;
    return reject();
  }
}

RenderTapeCaptureStatus RenderTapeCaptureSession::attachCaptureIdentityV4(
    RenderTapeIdentityV4CaptureKey expectedCapture,
    std::uint64_t expectedQueue, std::uint64_t presentOrdinal,
    const RenderTapeIdentityV4View& identitySnapshot,
    const RenderTapeExecutionTopologyV1View& topologySnapshot,
    const RenderTapeActionWitnessV1& actionWitness,
    std::span<const std::byte> attachmentHistory) {
  if (state_ != RenderTapeCaptureState::Sealed ||
      !publicationBundle_.identity.empty() || !publicationBundle_.executionTopology.empty()) {
    return RenderTapeCaptureStatus::InvalidState;
  }
  identityV4ValidationStatus_ = RenderTapeIdentityV4Status::InvalidHeader;
  topologyV1ValidationStatus_ = RenderTapeExecutionTopologyV1Status::InvalidHeader;
  const auto& i = identitySnapshot.header;
  const auto& t = topologySnapshot.header;
  if (!renderTapeActionWitnessV1Valid(actionWitness) ||
      (actionWitness.evidence != static_cast<std::uint32_t>(
           RenderTapeActionWitnessEvidenceV1::LegacyUnknown) &&
       !renderTapeActionWitnessV1MatchesBinding(actionWitness,
           expectedCapture.token, expectedCapture.epoch, expectedQueue)) ||
      !expectedCapture.token || !expectedCapture.epoch || !expectedQueue || !presentOrdinal ||
      i.captureToken != expectedCapture.token || i.captureEpoch != expectedCapture.epoch ||
      i.queue != expectedQueue || i.presentOrdinal != presentOrdinal ||
      t.captureToken != expectedCapture.token || t.captureEpoch != expectedCapture.epoch ||
      t.queue != expectedQueue || i.eventsBytes || i.eventsDigest != RenderTapeDigest{} ||
      t.identityBytes || t.eventsBytes || t.identityDigest != RenderTapeDigest{} ||
      t.eventsDigest != RenderTapeDigest{}) {
    topologyV1ValidationStatus_ = RenderTapeExecutionTopologyV1Status::BindingMismatch;
    abortInternal();
    return RenderTapeCaptureStatus::ValidationFailed;
  }
  auto identity = buildRenderTapeIdentityV4(
      sealedArtifact_, catalogue_, identitySnapshot, &identityV4ValidationStatus_);
  if (identity.empty()) {
    abortInternal();
    const bool capacity = identityV4ValidationStatus_ == RenderTapeIdentityV4Status::AllocationFailed ||
        identityV4ValidationStatus_ == RenderTapeIdentityV4Status::CapacityExceeded;
    return capacity ? RenderTapeCaptureStatus::CapacityExceeded : RenderTapeCaptureStatus::ValidationFailed;
  }
  auto topology = buildRenderTapeExecutionTopologyV1(
      sealedArtifact_, catalogue_, identity, topologySnapshot, &topologyV1ValidationStatus_);
  if (topology.empty()) {
    abortInternal();
    const bool capacity = topologyV1ValidationStatus_ == RenderTapeExecutionTopologyV1Status::AllocationFailed ||
        topologyV1ValidationStatus_ == RenderTapeExecutionTopologyV1Status::CapacityExceeded;
    return capacity ? RenderTapeCaptureStatus::CapacityExceeded : RenderTapeCaptureStatus::ValidationFailed;
  }
  return attachCaptureIdentityV4(expectedCapture, expectedQueue, presentOrdinal,
      identity, topology, actionWitness, attachmentHistory);
}

void RenderTapeCaptureSession::abortInternal() noexcept {
  static_assert(std::is_nothrow_constructible_v<RenderTapeBuilder, std::uint32_t>);
  static_assert(std::is_nothrow_move_assignable_v<RenderTapeBuilder>);
  static_assert(std::is_nothrow_move_assignable_v<RenderTapePublicationBundle>);
  if (state_ != RenderTapeCaptureState::Aborted) {
    lastAbortAccounting_ = {
        .valid = true, .eventCount = eventCount_, .eventBytes = eventBytes_,
        .blobCount = catalogue_.blobs.size(), .blobBytes = blobBytes_,
        .objectCount = objects_.size()};
  }
  state_ = RenderTapeCaptureState::Aborted;
  // These are capture-owned CPU values, never queue/Metal backing. Release
  // them at the terminal boundary, before a retry builds another snapshot.
  std::vector<RenderTapePublishedBlob>{}.swap(publishedBlobs_);
  std::vector<RenderTapeBlob>{}.swap(catalogue_.blobs);
  std::vector<ObjectSlot>{}.swap(objects_);
  std::vector<std::byte>{}.swap(sealedArtifact_);
  builder_ = RenderTapeBuilder(builder_.profile());
  publicationBundle_ = {};
  eventCount_ = 0u;
  eventBytes_ = 0u;
  blobBytes_ = 0u;
}

void RenderTapeCaptureSession::abort() noexcept {
  abortInternal();
}

} // namespace dxmt9::d3d9
