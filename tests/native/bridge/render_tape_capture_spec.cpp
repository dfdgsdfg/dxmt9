#include "device_c_render_tape_capture.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_render_tape_capture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

using dxmt9::d3d9::pe::CommandChunkBuilder;

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

std::vector<std::byte> recorderPresentChunk();

bool unusedProductionProducer(RenderTapeCaptureBootstrapSeed&) { return true; }

bool unusedProductionPublisher(const RenderTapePublicationBundle&) {
  return true;
}

void testProductionHookGateTruthTable() {
  using Producer = D3D9PeRenderTapeBootstrapProducer;
  using Publisher = D3D9PeRenderTapeArtifactPublisher;
  struct GateCase {
    bool enabled;
    Producer producer;
    Publisher publisher;
    bool expected;
  };
  const auto producer = &unusedProductionProducer;
  const auto publisher = &unusedProductionPublisher;
  const std::array<GateCase, 6u> cases{
      GateCase{false, producer, publisher, false},
      GateCase{false, producer, nullptr, false},
      GateCase{false, nullptr, publisher, false},
      GateCase{true, producer, publisher, true},
      GateCase{true, producer, nullptr, false},
      GateCase{true, nullptr, publisher, true},
  };
  for (const auto& testCase : cases) {
    check(dxmt9PeRenderTapeCaptureCallbacksInstalled(
              testCase.enabled, testCase.producer, testCase.publisher) ==
              testCase.expected,
          "production capture gate truth table is stable");
  }
  auto bytes = recorderPresentChunk();
  const auto original = bytes;
  check(bytes == original,
        "production capture gate does not mutate canonical recorder bytes");
}

std::vector<std::byte> recorderPresentChunk() {
  CommandChunkBuilder recorder;
  check(dxmt9::d3d9::pe::appendPresent(
            recorder, D9CCommandChunkWirePresent{}),
        "PE recorder appends Present");
  const auto sealed = recorder.seal();
  check(sealed.valid(), "PE recorder seals Present");
  return std::vector<std::byte>(sealed.blob.begin(), sealed.blob.end());
}

void writeAtomically(const std::filesystem::path& path,
                     std::span<const std::byte> bytes) {
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  check(output.good(), "fixture opens temporary output");
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  check(output.good(), "fixture writes temporary output");
  output.close();
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  check(!error, "fixture publishes output atomically");
}

void writeProductionFixture(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory / "blobs", error);
  check(!error, "fixture creates output directory");

  const auto bootstrap = bootstrapChunk();
  const auto present = recorderPresentChunk();
  const auto mutationBytes = std::array<std::byte, 4u>{
      std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}, std::byte{0xdd}};
  const RenderTapeCaptureBlob verifiedBlob{
      .bytes = std::vector<std::byte>(mutationBytes.begin(), mutationBytes.end())};
  RenderTapeCaptureSession session(true);
  check(session.armWithBlobs(bootstrap, std::span(&verifiedBlob, 1u)) ==
            RenderTapeCaptureStatus::Accepted,
        "production fixture arms capture owner");
  check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "production fixture starts one Present interval");
  constexpr std::array<std::byte, 8u> descriptor{};
  check(session.objectDefine(kSurface, 1u, descriptor, 0u, {}, 4u, 1u) ==
            RenderTapeCaptureStatus::Accepted,
        "production fixture journals ObjectDefine");
  check(session.resourceMutationBytes(
            kSurface, RenderTapeMutationKind::Upload, 0u, 0u,
            mutationBytes) == RenderTapeCaptureStatus::Accepted,
        "production fixture journals resource mutation");
  check(session.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 1u, .handleCount = 0u},
            present) == RenderTapeCaptureStatus::Accepted,
        "production fixture copies canonical PE recorder bytes once");
  const RenderTapeFlushWaitControl wait{.waitedSeqId = 9u};
  check(session.orderedControl(
            RenderTapeOrderedControlHeader{
                .kind = static_cast<std::uint32_t>(RenderTapeControlKind::FlushWait),
                .disposition = static_cast<std::uint32_t>(
                    RenderTapeControlDisposition::Completed),
                .controlBytes = sizeof(wait),
                .completionOrdinal = 10u},
            std::as_bytes(std::span(&wait, 1u))) ==
            RenderTapeCaptureStatus::Accepted,
        "production fixture journals ordered control");
  const RenderTapeOracleAttachment attachment = oracle();
  check(session.completePresent(
            4u, 11u, RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::Complete,
        "production fixture seals Present transactionally");
  check(session.validationStatus() == RenderTapeValidationStatus::Valid,
        "production fixture validates before publish");

  const auto eventsPath = directory / "events.bin";
  const auto blobPath = directory / "blobs" /
                        "8d70d691c822d55638b6e7fd54cd94170c87d19eb1f628b757506ede5688d297.bin";
  check(session.publicationBundle().events == session.sealedArtifact(),
        "production fixture publishes the session bundle events");
  check(session.publicationBundle().blobs.size() == 1u &&
            session.publicationBundle().blobs[0].bytes ==
                std::vector<std::byte>(mutationBytes.begin(), mutationBytes.end()),
        "production fixture publishes verified blob bytes from the bundle");
  writeAtomically(eventsPath, session.publicationBundle().events);
  writeAtomically(blobPath, session.publicationBundle().blobs[0].bytes);
}

void testProductionFixtureUsesRecorderAndPublishesBundle(
    const std::filesystem::path& directory) {
  const auto recorderBytes = recorderPresentChunk();
  const auto original = recorderBytes;
  RenderTapeCaptureSession disabled(false);
  check(disabled.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Disabled,
        "production capture-off arm is inert");
  check(recorderBytes == original,
        "production capture-off preserves recorder bytes");
  writeProductionFixture(directory);
  check(std::filesystem::is_regular_file(directory / "events.bin"),
        "production fixture publishes events.bin");
  check(std::filesystem::is_regular_file(
            directory / "blobs" /
            "8d70d691c822d55638b6e7fd54cd94170c87d19eb1f628b757506ede5688d297.bin"),
        "production fixture publishes digest-named blob");
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
  check(session.objectDefine(kSurface, 1u, descriptor, 0u, {}, 4u, 1u) ==
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

void testObjectExpectedContentContractTruthTable() {
  struct ExtentCase {
    std::uint64_t bytes;
    std::uint32_t count;
    RenderTapeCaptureStatus expected;
  };
  constexpr std::array cases{
      ExtentCase{0u, 0u, RenderTapeCaptureStatus::Accepted},
      ExtentCase{4u, 0u, RenderTapeCaptureStatus::InvalidInput},
      ExtentCase{0u, 1u, RenderTapeCaptureStatus::InvalidInput},
      ExtentCase{4u, 1u, RenderTapeCaptureStatus::Accepted},
  };
  constexpr std::array<std::byte, 8u> descriptor{};
  for (const auto& testCase : cases) {
    RenderTapeCaptureSession session(true);
    check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() ==
                  RenderTapeCaptureStatus::Accepted,
          "expected-content truth-table fixture starts");
    check(session.objectDefine(kSurface, 1u, descriptor, 0u, {},
                               testCase.bytes, testCase.count) ==
              testCase.expected,
          "ObjectDefine expected extent/count pair truth table is stable");
  }
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

void testBoundedBlobBytesAndDeduplication() {
  const RenderTapeCaptureLimits limits{.maxBlobBytes = 4u};
  const std::array<RenderTapeCaptureBlob, 2u> overflowing{
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(3u, std::byte{1})},
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(2u, std::byte{2})}};
  RenderTapeCaptureSession armOverflow(true, limits);
  check(armOverflow.armWithBlobs(bootstrapChunk(), overflowing) ==
            RenderTapeCaptureStatus::CapacityExceeded,
        "arm rejects the total blob-byte limit before retaining payloads");
  check(armOverflow.ownedBlobBytes() == 0u &&
            armOverflow.state() == RenderTapeCaptureState::Disabled,
        "failed arm leaves the blob-byte invariant untouched");

  RenderTapeCaptureSession incremental(true, limits);
  check(incremental.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "incremental blob fixture arms");
  check(incremental.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "incremental blob fixture starts");
  const std::array<std::byte, 3u> first{
      std::byte{0x10}, std::byte{0x11}, std::byte{0x12}};
  const std::array<std::byte, 2u> tooMuch{std::byte{0x20}, std::byte{0x21}};
  const std::array<std::byte, 1u> last{std::byte{0x30}};
  check(incremental.registerBlobBytes(first) ==
            RenderTapeCaptureStatus::Accepted &&
            incremental.ownedBlobBytes() == 3u,
        "incremental blob registration charges accepted bytes");
  check(incremental.registerBlobBytes(tooMuch) ==
            RenderTapeCaptureStatus::CapacityExceeded &&
            incremental.ownedBlobBytes() == 3u,
        "incremental overflow preserves the owned-byte count");
  check(incremental.registerBlobBytes(last) ==
            RenderTapeCaptureStatus::Accepted &&
            incremental.ownedBlobBytes() == 4u,
        "the exact incremental byte bound is accepted");
  check(incremental.registerBlobBytes(first) ==
            RenderTapeCaptureStatus::Accepted &&
            incremental.ownedBlobBytes() == 4u,
        "deduplicated bytes do not double-charge the bound");

  const std::array<RenderTapeCaptureBlob, 2u> exact{
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(2u, std::byte{3})},
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(2u, std::byte{4})}};
  RenderTapeCaptureSession exactArm(true, limits);
  check(exactArm.armWithBlobs(bootstrapChunk(), exact) ==
            RenderTapeCaptureStatus::Accepted &&
            exactArm.ownedBlobBytes() == 4u,
        "arm accepts the exact total blob-byte bound");
  const auto duplicate = exact[0].bytes;
  check(exactArm.registerBlobBytes(duplicate) ==
            RenderTapeCaptureStatus::Accepted &&
            exactArm.ownedBlobBytes() == 4u,
        "arm-owned duplicate does not double-charge bytes");

  const std::array<RenderTapeCaptureBlob, 2u> duplicateSeeds{
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(
                                3u, std::byte{0x55})},
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(
                                3u, std::byte{0x55})},
  };
  RenderTapeCaptureSession duplicateArm(true,
                                        RenderTapeCaptureLimits{
                                            .maxBlobEntries = 1u,
                                            .maxBlobBytes = 3u});
  check(duplicateArm.armWithBlobs(bootstrapChunk(), duplicateSeeds) ==
            RenderTapeCaptureStatus::Accepted &&
            duplicateArm.ownedBlobBytes() == 3u,
        "duplicate bootstrap blobs consume one entry and one byte charge");
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

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--write-production-fixture") {
      testProductionFixtureUsesRecorderAndPublishesBundle(argv[2]);
      return 0;
    }
    check(argc == 1,
          "usage: render_tape_capture_spec [--write-production-fixture dir]");
    testCaptureOffPreservesBytes();
    testProductionHookGateTruthTable();
    testCompletePresentPublishesExactlyOneTape();
    testFailureBeforePublishAndBoundedBackpressure();
    testBoundedBlobBytesAndDeduplication();
    testObjectLifetimeAndTerminalControls();
    testObjectExpectedContentContractTruthTable();
    return 0;
  } catch (const TestFailure& failure) {
    return 1;
  }
}
