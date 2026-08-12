#include "device_c_render_tape_capture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

std::vector<std::byte> singleRecordChunk(
    std::uint32_t type, std::span<const std::byte> payload) {
  const auto recordsOffset = sizeof(D9CCommandChunkWireHeader);
  const auto payloadOffset = alignUp(
      recordsOffset + sizeof(D9CCommandChunkWireRecordHeader),
      alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordsOffset),
      .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(payloadOffset),
      .handleCount = 0u,
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = type,
      .payloadSize = static_cast<std::uint32_t>(payload.size()),
      .handleCount = 0u,
  };
  std::vector<std::byte> result(payloadOffset + payload.size());
  std::memcpy(result.data(), &header, sizeof(header));
  std::memcpy(result.data() + recordsOffset, &record, sizeof(record));
  std::memcpy(result.data() + payloadOffset, payload.data(), payload.size());
  return result;
}

std::vector<std::byte> bootstrapChunk() {
  std::array<D9CCommandChunkWireTextureBinding,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t i = 0u; i < textures.size(); ++i) {
    textures[i] = D9CCommandChunkWireTextureBinding{
        .slot = i,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  for (std::uint32_t i = 0u; i < streams.size(); ++i) {
    streams[i] = D9CCommandChunkWireStreamBinding{
        .slot = i,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  constexpr std::uint32_t sectionCount = 2u;
  constexpr std::size_t textureBytes =
      sizeof(D9CCommandChunkWireTextureBinding) * D9C_DRAW_PACKET_MAX_TEXTURES;
  constexpr std::size_t streamBytes =
      sizeof(D9CCommandChunkWireStreamBinding) * D9C_DRAW_PACKET_MAX_STREAMS;
  const auto tableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto payloadOffset = alignUp(
      tableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamsOffset = payloadOffset + textureBytes;
  const D9CCommandChunkWireDrawHeader draw{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT,
      .sectionCount = sectionCount,
      .sectionTableOffset = static_cast<std::uint32_t>(tableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(payloadOffset),
  };
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(D9CCommandChunkWireTextureBinding),
          .count = D9C_DRAW_PACKET_MAX_TEXTURES,
          .payloadOffset = static_cast<std::uint32_t>(payloadOffset),
          .byteSize = static_cast<std::uint32_t>(textureBytes),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
          .count = D9C_DRAW_PACKET_MAX_STREAMS,
          .payloadOffset = static_cast<std::uint32_t>(streamsOffset),
          .byteSize = static_cast<std::uint32_t>(streamBytes),
      },
  };
  std::vector<std::byte> payload(streamsOffset + streamBytes);
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + tableOffset, sections.data(), sizeof(sections));
  std::memcpy(payload.data() + payloadOffset, textures.data(), textureBytes);
  std::memcpy(payload.data() + streamsOffset, streams.data(), streamBytes);
  return singleRecordChunk(D9C_COMMAND_RECORD_APPLY_STATE, payload);
}

std::vector<std::byte> presentChunk() {
  const D9CCommandChunkWirePresent present{};
  return singleRecordChunk(D9C_COMMAND_RECORD_PRESENT,
                           std::as_bytes(std::span(&present, 1u)));
}

constexpr D9CWireObjectIdentity kSurface{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 2u,
    .objectId = 17u,
};

RenderTapeDigest digest() {
  RenderTapeDigest value{};
  for (std::size_t i = 0u; i < value.size(); ++i) {
    value[i] = static_cast<std::byte>(i + 1u);
  }
  return value;
}

RenderTapeBlob blob() {
  return RenderTapeBlob{.digest = digest(), .size = 4u, .verified = 1u};
}

RenderTapeOrderedControlHeader flushControl() {
  return RenderTapeOrderedControlHeader{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::FlushWait),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = sizeof(RenderTapeFlushWaitControl),
      .completionOrdinal = 10u,
  };
}

RenderTapeOracleAttachment oracle() {
  return RenderTapeOracleAttachment{.identity = kSurface, .descriptorKind = 1u};
}

void testCaptureOffPreservesBytes() {
  auto chunk = presentChunk();
  const auto original = chunk;
  RenderTapeCaptureSession session(false);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Disabled,
        "capture-off arm is inert");
  check(session.state() == RenderTapeCaptureState::Disabled,
        "capture-off state remains disabled");
  check(chunk == original, "capture-off leaves canonical bytes unchanged");
}

void testCompletePresentPublishesExactlyOneTape() {
  const auto bootstrap = bootstrapChunk();
  auto present = presentChunk();
  const auto resource = blob();
  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrap, std::span(&resource, 1u)) ==
            RenderTapeCaptureStatus::Accepted,
        "complete capture arms");
  check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "complete capture starts one interval");
  constexpr std::array<std::byte, 8u> descriptor{};
  check(session.objectDefine(kSurface, 1u, descriptor, 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "object definition is journaled");
  const auto mutationDigestValue = digest();
  check(session.resourceMutation(
            kSurface, RenderTapeMutationKind::Upload, 0u, 0u, 4u,
            std::span<const std::byte, kRenderTapeDigestSize>(
                mutationDigestValue)) == RenderTapeCaptureStatus::Accepted,
        "verified initial mutation is journaled");
  check(session.commandChunk(
            CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
            present) == RenderTapeCaptureStatus::Accepted,
        "canonical Present chunk is copied once");
  present[0] = std::byte{0xff};
  const RenderTapeFlushWaitControl wait{.waitedSeqId = 9u};
  check(session.orderedControl(flushControl(),
                               std::as_bytes(std::span(&wait, 1u))) ==
            RenderTapeCaptureStatus::Accepted,
        "true bypass control is journaled");
  const auto attachment = oracle();
  check(session.completePresent(
            4u, 11u, RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::Complete,
        "PresentComplete validates and publishes");
  check(session.state() == RenderTapeCaptureState::Sealed,
        "successful Present seals the owner");
  check(!session.sealedArtifact().empty(), "sealed artifact is published");
  check(session.completePresent(
            4u, 12u, RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::InvalidState,
        "second Present cannot publish another interval");
}

void testFailureBeforePublishAndBoundedBackpressure() {
  RenderTapeCaptureSession bounded(
      true, RenderTapeCaptureLimits{.maxEvents = 1u});
  check(bounded.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "bounded capture arms at its event limit");
  check(bounded.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "bounded capture starts");
  constexpr std::array<std::byte, 8u> descriptor{};
  check(bounded.objectDefine(kSurface, 1u, descriptor, 0u, {}) ==
            RenderTapeCaptureStatus::CapacityExceeded,
        "bounded owner rejects before growing the journal");
  check(bounded.state() == RenderTapeCaptureState::Aborted,
        "backpressure aborts before publishing a partial artifact");
  check(bounded.sealedArtifact().empty(), "backpressure has no artifact");

  RenderTapeCaptureSession failed(true);
  check(failed.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "failure fixture arms");
  check(failed.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "failure fixture starts");
  check(failed.completePresent(
            1u, 1u, RenderTapeDigestValidity::NotCaptured, {}) ==
            RenderTapeCaptureStatus::InvalidInput,
        "missing Present chunk fails before publish");
  check(failed.state() == RenderTapeCaptureState::Aborted,
        "failed sealing aborts the owner");
  check(failed.sealedArtifact().empty(), "failed sealing has no partial tape");
}

void testObjectLifetimeAndTerminalControls() {
  constexpr std::array<std::byte, 8u> descriptor{};
  RenderTapeCaptureSession lifetime(true);
  check(lifetime.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "lifetime fixture arms");
  check(lifetime.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "lifetime fixture starts");
  check(lifetime.objectDefine(kSurface, 1u, descriptor, 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "lifetime definition succeeds");
  check(lifetime.objectDestroy(kSurface) == RenderTapeCaptureStatus::Accepted,
        "lifetime destroy succeeds");
  const auto value = digest();
  check(lifetime.resourceMutation(
            kSurface, RenderTapeMutationKind::Upload, 0u, 0u, 4u,
            std::span<const std::byte, kRenderTapeDigestSize>(value)) ==
            RenderTapeCaptureStatus::InvalidInput,
        "retired object cannot receive a mutation");

  RenderTapeCaptureSession reset(true);
  check(reset.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "reset fixture arms");
  check(reset.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "reset fixture starts");
  const RenderTapeResetControl resetPayload{};
  const RenderTapeOrderedControlHeader resetHeader{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::Reset),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Terminal),
      .controlBytes = sizeof(resetPayload),
      .completionOrdinal = 3u,
  };
  check(reset.orderedControl(
            resetHeader, std::as_bytes(std::span(&resetPayload, 1u))) ==
            RenderTapeCaptureStatus::Terminal,
        "successful Reset terminates the interval");
  check(reset.state() == RenderTapeCaptureState::Aborted &&
            reset.sealedArtifact().empty(),
        "Reset aborts without a partial artifact");

  RenderTapeCaptureSession lost(true);
  check(lost.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "device-lost fixture arms");
  check(lost.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "device-lost fixture starts");
  const RenderTapeDeviceLostControl lostPayload{.hrCode = 0x88760868u};
  const RenderTapeOrderedControlHeader lostHeader{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::DeviceLost),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Terminal),
      .resultCode = static_cast<std::int32_t>(lostPayload.hrCode),
      .controlBytes = sizeof(lostPayload),
      .completionOrdinal = 4u,
  };
  check(lost.orderedControl(
            lostHeader, std::as_bytes(std::span(&lostPayload, 1u))) ==
            RenderTapeCaptureStatus::Terminal,
        "device lost terminates the interval");
  check(lost.state() == RenderTapeCaptureState::Aborted &&
            lost.sealedArtifact().empty(),
        "device lost has no partial artifact");
}

} // namespace

int main() {
  try {
    testCaptureOffPreservesBytes();
    testCompletePresentPublishesExactlyOneTape();
    testFailureBeforePublishAndBoundedBackpressure();
    testObjectLifetimeAndTerminalControls();
    return 0;
  } catch (const TestFailure& failure) {
    return 1;
  }
}
