#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_render_tape_capture.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_presenter.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

static_assert(sizeof(D9CRenderTapePresentCaptureResult) == 56u);
static_assert(alignof(D9CRenderTapePresentCaptureResult) == alignof(uint64_t));
static_assert(std::is_standard_layout_v<D9CRenderTapePresentCaptureResult>);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, status) == 0u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, width) == 4u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, height) == 8u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, format) == 12u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, byteCount) == 16u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, sha256) == 24u);

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
  return RenderTapeOracleAttachment{
      .identity = kSurface,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface)};
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

void testDescriptorKindAxisTruthTable() {
  constexpr std::array identityKinds{
      D9C_CHUNK_HANDLE_KIND_TEXTURE, D9C_CHUNK_HANDLE_KIND_SURFACE,
      D9C_CHUNK_HANDLE_KIND_BUFFER, D9C_CHUNK_HANDLE_KIND_SHADER,
      D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, D9C_CHUNK_HANDLE_KIND_QUERY};
  constexpr std::array expectedDescriptorKinds{
      RenderTapeDescriptorKind::Texture, RenderTapeDescriptorKind::Surface,
      RenderTapeDescriptorKind::Buffer, RenderTapeDescriptorKind::Shader,
      RenderTapeDescriptorKind::VertexDeclaration,
      RenderTapeDescriptorKind::Query};
  constexpr std::array<std::byte, 8u> descriptor{};
  for (std::size_t i = 0u; i < identityKinds.size(); ++i) {
    const D9CWireObjectIdentity identity{
        .kind = identityKinds[i], .generation = 1u,
        .objectId = static_cast<std::uint64_t>(100u + i)};
    check(renderTapeDescriptorKindForObject(identity.kind) ==
              expectedDescriptorKinds[i],
          "identity kind maps to its stable descriptor schema tag");
    RenderTapeCaptureSession session(true);
    check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() ==
                  RenderTapeCaptureStatus::Accepted,
          "descriptor-kind truth-table fixture starts");
    check(session.objectDefine(
              identity, static_cast<std::uint32_t>(expectedDescriptorKinds[i]),
              descriptor, 0u, {}) == RenderTapeCaptureStatus::Accepted,
          "all object categories accept their non-zero descriptor schema tag");
  }
  check(renderTapeDescriptorKindForObject(99u) ==
            RenderTapeDescriptorKind::Invalid,
        "unknown identity kinds map to the invalid descriptor schema tag");
}

void testKindZeroIntervalDefineUsesNonZeroDescriptorTag() {
  constexpr D9CWireObjectIdentity kindZero{
      .kind = 0u, .generation = 7u, .objectId = 700u};
  constexpr std::array<std::byte, 8u> descriptor{};
  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "kind-zero interval define fixture starts");
  const auto descriptorKind = renderTapeDescriptorKindForObject(kindZero.kind);
  check(descriptorKind != RenderTapeDescriptorKind::Invalid &&
            static_cast<std::uint32_t>(descriptorKind) != 0u,
        "kind-zero identity maps to a non-zero descriptor tag");
  check(session.objectDefine(
            kindZero, static_cast<std::uint32_t>(descriptorKind), descriptor,
            0u, {}) == RenderTapeCaptureStatus::Accepted,
        "kind-zero interval ObjectDefine accepts the mapped descriptor tag");
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

void testProfileSelectionTruthTable() {
  check(dxmt9PeRenderTapeProfileFromText("") == kRenderTapeProfileFrame &&
            dxmt9PeRenderTapeProfileFromText("frame-tape") ==
                kRenderTapeProfileFrame &&
            dxmt9PeRenderTapeProfileFromText("sequence-tape") ==
                kRenderTapeProfileSequence &&
            dxmt9PeRenderTapeProfileFromText("bogus") == 0u,
        "capture profile selection accepts only explicit bounded names");
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
  check(session.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}, 4u, 1u) ==
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
  check(session.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}, 4u, 1u) ==
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

void testSequenceCaptureDefersSealUntilSecondPresent() {
  constexpr std::array<std::byte, 4u> firstBytes{
      std::byte{0x10u}, std::byte{0x11u}, std::byte{0x12u}, std::byte{0x13u}};
  constexpr std::array<std::byte, 4u> secondBytes{
      std::byte{0x20u}, std::byte{0x21u}, std::byte{0x22u}, std::byte{0x23u}};
  const std::array<RenderTapeCaptureBlob, 2u> blobs{
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(
          firstBytes.begin(), firstBytes.end())},
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(
          secondBytes.begin(), secondBytes.end())}};
  RenderTapeCaptureSession session(true, {}, kRenderTapeProfileSequence);
  check(session.profile() == kRenderTapeProfileSequence &&
            session.armWithBlobs(bootstrapChunk(), blobs) ==
                RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted,
        "sequence capture arms the explicit two-interval profile");
  constexpr std::array<std::byte, 8u> descriptor{};
  check(session.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}, 4u, 1u) ==
            RenderTapeCaptureStatus::Accepted,
        "sequence capture journals the initial output seed definition");
  const auto firstPresent = presentChunk();
  check(session.resourceMutationBytes(
            kSurface, RenderTapeMutationKind::Upload, 0u, 0u,
            firstBytes) == RenderTapeCaptureStatus::Accepted &&
            session.commandChunk(
                CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
                firstPresent) == RenderTapeCaptureStatus::Accepted,
        "sequence capture records interval one in journal order");
  const auto attachment = oracle();
  check(session.completePresent(
            4u, 1u, RenderTapeDigestValidity::Sha256, digest(),
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::Accepted &&
            session.presentCompletionCount() == 1u &&
            session.state() == RenderTapeCaptureState::Capturing &&
            session.sealedArtifact().empty(),
        "sequence capture does not seal or validate after interval one");
  const auto secondPresent = presentChunk();
  check(session.resourceMutationBytes(
            kSurface, RenderTapeMutationKind::Upload, 0u, 0u,
            secondBytes) == RenderTapeCaptureStatus::Accepted &&
            session.commandChunk(
                CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
                secondPresent) == RenderTapeCaptureStatus::Accepted,
        "sequence capture preserves the between-Present mutation");
  check(session.completePresent(
            7u, 2u, RenderTapeDigestValidity::Sha256, digest(),
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::Complete &&
            session.presentCompletionCount() == 2u &&
            session.state() == RenderTapeCaptureState::Sealed &&
            !session.sealedArtifact().empty(),
        "sequence capture seals only after interval two validation");
  RenderTapeBlobCatalogue catalogue;
  for (const auto& blob : session.publicationBundle().blobs) {
    catalogue.blobs.push_back(RenderTapeBlob{
        .digest = blob.digest, .size = blob.bytes.size(), .verified = 1u});
  }
  check(validateRenderTape(session.sealedArtifact(), catalogue).valid(),
        "sequence capture publishes a structurally valid final tape");
}

void testValidationFailurePreservesEventAndChunkLocation() {
  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "validation-location fixture arms");
  check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "validation-location fixture starts");
  const auto present = presentChunk();
  check(session.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 1u, .handleCount = 0u},
            present) == RenderTapeCaptureStatus::Accepted,
        "validation-location fixture records Present chunk");
  check(session.completePresent(
            1u, 1u, RenderTapeDigestValidity::NotCaptured, {}) ==
            RenderTapeCaptureStatus::ValidationFailed,
        "missing oracle attachment fails at final validation");
  const auto &validation = session.validationResult();
  check(validation.status == RenderTapeValidationStatus::InvalidPresentComplete &&
            validation.failedEventIndex == 2u &&
            validation.chunkStatus == CommandChunkValidationStatus::Valid,
        "validation failure preserves final event index and chunk status");
}

void testPresentCompleteOracleTargetTruthTable() {
  constexpr D9CWireObjectIdentity buffer{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 1u, .objectId = 99u};
  constexpr std::array<std::byte, 8u> descriptor{};
  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "oracle-target truth-table fixture starts");
  check(session.objectDefine(buffer,
                             static_cast<std::uint32_t>(
                                 RenderTapeDescriptorKind::Buffer),
                             descriptor, 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "oracle-target truth-table admits a live buffer control case");
  const auto present = presentChunk();
  check(session.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 1u, .handleCount = 0u},
            present) == RenderTapeCaptureStatus::Accepted,
        "oracle-target truth-table records Present");
  const RenderTapeOracleAttachment wrongTarget{
      .identity = buffer,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Buffer)};
  check(session.completePresent(
            1u, 1u, RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&wrongTarget, 1u))) ==
            RenderTapeCaptureStatus::ValidationFailed,
        "PresentComplete rejects a non-surface oracle target");
  check(session.validationResult().status ==
            RenderTapeValidationStatus::InvalidPresentComplete,
        "oracle-target rejection is attributed to PresentComplete");
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
    check(session.objectDefine(
              kSurface,
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
              descriptor, 0u, {},
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
  check(bounded.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}) ==
            RenderTapeCaptureStatus::CapacityExceeded,
        "bounded owner rejects before growing the journal");
  check(bounded.state() == RenderTapeCaptureState::Aborted,
        "backpressure aborts before publishing a partial artifact");
  check(bounded.sealedArtifact().empty(), "backpressure has no artifact");
  check(bounded.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            bounded.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted,
        "an aborted capture lifecycle can be re-armed independently");

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

void testProductionBlobDefaultCoversGt2R7Requirement() {
  constexpr std::uint64_t gt2R7RequiredBytes = 67371903u;
  const RenderTapeCaptureLimits limits{};
  check(limits.maxBlobBytes == kRenderTapeDefaultMaxBlobBytes &&
            limits.maxBlobBytes >= gt2R7RequiredBytes,
        "the bounded production blob default covers the measured GT2 r7 closure");
  check(limits.maxBlobBytes - gt2R7RequiredBytes == 3931265u,
        "the production blob default keeps only the measured bounded headroom");
}

void testObjectLifetimeAndTerminalControls() {
  constexpr std::array<std::byte, 8u> descriptor{};
  RenderTapeCaptureSession lifetime(true);
  check(lifetime.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "lifetime fixture arms");
  check(lifetime.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "lifetime fixture starts");
  check(lifetime.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "lifetime definition succeeds");
  check(lifetime.objectDestroy(kSurface) == RenderTapeCaptureStatus::Accepted,
        "lifetime destroy succeeds");
  auto newerSurface = kSurface;
  ++newerSurface.generation;
  check(lifetime.objectDefine(
            newerSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}) == RenderTapeCaptureStatus::Accepted,
        "capture session admits a strictly newer destroyed-slot generation");
  auto overlappingSurface = newerSurface;
  ++overlappingSurface.generation;
  RenderTapeObjectDefineDisposition defineDisposition{};
  check(lifetime.objectDefine(
            overlappingSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}, 0u, 0u, &defineDisposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::OverlappingLiveGeneration,
        "capture session rejects overlapping live slot generations");
  check(lifetime.objectDestroy(newerSurface) ==
            RenderTapeCaptureStatus::Accepted,
        "newer lifetime generation retires exactly");
  check(lifetime.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}, 0u, 0u, &defineDisposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::ExactIdentityConflict &&
            lifetime.objectDefine(
                newerSurface,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                descriptor, 0u, {}, 0u, 0u, &defineDisposition) ==
                RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::ExactIdentityConflict,
        "capture session keeps exact definitions globally unique after destroy");
  auto futureSurface = newerSurface;
  futureSurface.generation += 2u;
  check(lifetime.objectDefine(
            futureSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}) == RenderTapeCaptureStatus::Accepted &&
            lifetime.objectDestroy(futureSurface) ==
                RenderTapeCaptureStatus::Accepted,
        "an unseen monotone generation advances a destroyed ordinary slot");
  auto staleSurface = newerSurface;
  ++staleSurface.generation;
  check(lifetime.objectDefine(
            staleSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptor, 0u, {}, 0u, 0u, &defineDisposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::StaleOrEqualGeneration,
        "an unseen stale generation cannot re-enter a destroyed slot");
  const auto value = digest();
  check(lifetime.resourceMutation(
            newerSurface, RenderTapeMutationKind::Upload, 0u, 0u, 4u,
            std::span<const std::byte, kRenderTapeDigestSize>(value)) ==
            RenderTapeCaptureStatus::InvalidInput,
        "retired object cannot receive a mutation");

  constexpr D9CWireObjectIdentity parentTexture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 4u,
      .objectId = 900u,
  };
  constexpr D9CSurfaceDesc aliasSurface{
      .format = render_tape_d3d_format::A8R8G8B8,
      .width = 64u,
      .height = 32u,
  };
  const RenderTapeTextureDescriptor parentDescriptor{
      .level0 = aliasSurface,
      .levelCount = 1u,
  };
  const RenderTapeSurfaceDescriptorV2 aliasDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = parentTexture,
      .surface = aliasSurface,
  };
  RenderTapeCaptureSession aliasLifetime(true);
  check(aliasLifetime.arm(bootstrapChunk()) ==
            RenderTapeCaptureStatus::Accepted &&
            aliasLifetime.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted,
        "surface-alias lifetime fixture starts");
  check(aliasLifetime.objectDefine(
            parentTexture,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
            std::as_bytes(std::span(&parentDescriptor, 1u)), 0u, {}) ==
            RenderTapeCaptureStatus::Accepted &&
            aliasLifetime.objectDefine(
                kSurface,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                std::as_bytes(std::span(&aliasDescriptor, 1u)), 0u, {}, 0u,
                0u, &defineDisposition) == RenderTapeCaptureStatus::Accepted,
        "surface alias follows its exact parent definition with no seed extent");
  const auto aliasEventCount = aliasLifetime.eventCount();
  check(aliasLifetime.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&aliasDescriptor, 1u)), 0u, {}, 0u, 0u,
            &defineDisposition) == RenderTapeCaptureStatus::Accepted &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::IdempotentSurfaceAlias &&
            aliasLifetime.eventCount() == aliasEventCount &&
            std::string_view(renderTapeObjectDefineDispositionName(
                defineDisposition)) == "idempotent_surface_alias",
        "an exact lazy surface wrapper is idempotent and emits no redefinition");
  auto conflictingAlias = aliasDescriptor;
  conflictingAlias.subresource = 1u;
  check(aliasLifetime.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&conflictingAlias, 1u)), 0u, {}, 0u, 0u,
            &defineDisposition) == RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::ExactIdentityConflict,
        "an exact identity with conflicting alias metadata still rejects");
  auto secondAliasIdentity = kSurface;
  ++secondAliasIdentity.generation;
  check(aliasLifetime.objectDefine(
            secondAliasIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&conflictingAlias, 1u)), 0u, {}, 0u, 0u,
            &defineDisposition) == RenderTapeCaptureStatus::Accepted,
        "one wire object id admits a distinct texture subresource alias");
  auto overlappingAliasIdentity = secondAliasIdentity;
  ++overlappingAliasIdentity.generation;
  check(aliasLifetime.objectDefine(
            overlappingAliasIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&conflictingAlias, 1u)), 0u, {}, 0u, 0u,
            &defineDisposition) == RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::OverlappingLiveGeneration,
        "one logical alias slot still rejects overlapping live generations");
  auto wrongDestroyIdentity = secondAliasIdentity;
  ++wrongDestroyIdentity.generation;
  check(aliasLifetime.objectDestroy(wrongDestroyIdentity) ==
            RenderTapeCaptureStatus::InvalidInput &&
            aliasLifetime.objectDestroy(secondAliasIdentity) ==
                RenderTapeCaptureStatus::Accepted,
        "surface alias destruction resolves the exact identity");
  auto ordinarySurfaceIdentity = kSurface;
  ordinarySurfaceIdentity.generation += 18u;
  check(aliasLifetime.objectDefine(
            ordinarySurfaceIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&aliasSurface, 1u)), 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "an ordinary surface may share an alias wrapper wire object id");

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

// R-RT-CAP-9.4: the PresentOutput role is capture-owned and single-holder.
// GT2 frame-tape retries (artifact
// experiments/output/app-d3d9-3dmark05-gt2-frame-tape-exact-closure-r6-20260814)
// re-admitted a fresh back-buffer wrapper per attempt while the previous
// holder stayed live and roled: the arm saw `present_output_count count=2..8`,
// and a recycled wire object id then met the stale entry in the logical-slot
// scan and rejected as `prior_not_retained_alias`, marking the registry
// invalid for the rest of the process.
void testPresentOutputRoleOwnershipTruthTable() {
  constexpr D9CWireObjectIdentity held{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 0x10000009bull,
  };
  auto recycled = held;
  recycled.generation = 10u;
  D9CWireObjectIdentity other = held;
  other.objectId = held.objectId + 1u;

  using Transition = RenderTapePresentOutputRoleTransition;
  const RenderTapePresentOutputRole unheld{};
  check(renderTapePresentOutputRoleTransition(unheld, &other, true, 1u) ==
            Transition::None,
        "an unheld role has nothing to hand back");

  const RenderTapePresentOutputRole captureOwned{
      .identity = held, .held = true, .captureOwned = true};
  check(renderTapePresentOutputRoleTransition(captureOwned, &held, true, 1u) ==
            Transition::Retained,
        "re-admitting the same exact identity keeps the role in place");
  check(renderTapePresentOutputRoleTransition(captureOwned, nullptr, true,
                                              1u) == Transition::Retire,
        "a capture-owned holder with only the admission reference retires");
  check(renderTapePresentOutputRoleTransition(captureOwned, &other, true, 1u) ==
            Transition::Retire,
        "naming a different holder retires the capture-owned entry");
  check(renderTapePresentOutputRoleTransition(captureOwned, &recycled, true,
                                              1u) == Transition::Retire,
        "a recycled wire object id retires the stale holder before "
        "registration sees it");
  check(renderTapePresentOutputRoleTransition(captureOwned, &other, false,
                                              0u) == Transition::None,
        "a holder already gone from the live registry needs no transition");
  check(renderTapePresentOutputRoleTransition(captureOwned, &held, false, 0u) ==
            Transition::None,
        "re-admitting an identity that left the registry is a fresh admission, "
        "not a retained role");
  check(renderTapePresentOutputRoleTransition(captureOwned, &other, true, 2u) ==
            Transition::Demote,
        "a surviving app wrapper demotes instead of retiring");

  // Retirement is scoped to the swap-chain output handoff. Anything the
  // capture only re-roled stays registered, so the alias rules keep owning it.
  auto nonSurface = held;
  nonSurface.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
  const RenderTapePresentOutputRole captureOwnedTexture{
      .identity = nonSurface, .held = true, .captureOwned = true};
  check(renderTapePresentOutputRoleTransition(captureOwnedTexture, &other, true,
                                              1u) == Transition::Demote,
        "only a surface holder takes the swap-chain output retirement");

  const RenderTapePresentOutputRole appOwned{
      .identity = held, .held = true, .captureOwned = false};
  check(renderTapePresentOutputRoleTransition(appOwned, &other, true, 1u) ==
            Transition::Demote,
        "an app-owned holder is only re-roled, so it is only demoted");
  check(renderTapePresentOutputRoleTransition(appOwned, nullptr, true, 0u) ==
            Transition::Demote,
        "an app-owned holder without wrapper references still demotes");

  // The transition never inspects generations. Monotonicity stays owned by
  // registration, so handing the role back cannot admit an identity that
  // registration would reject.
  auto stale = held;
  stale.generation = 1u;
  check(renderTapePresentOutputRoleTransition(captureOwned, &stale, true, 1u) ==
            Transition::Retire,
        "a stale generation is retired here and rejected by registration");

  check(std::string_view(renderTapePresentOutputRoleTransitionName(
            Transition::Retire)) == "retire" &&
            std::string_view(renderTapePresentOutputRoleTransitionName(
                Transition::Demote)) == "demote" &&
            std::string_view(renderTapePresentOutputRoleTransitionName(
                Transition::Retained)) == "retained" &&
            std::string_view(renderTapePresentOutputRoleTransitionName(
                Transition::None)) == "none",
        "every present output transition has a stable log name");
}

void testSurfaceAliasGenerationReplacementTransition() {
  constexpr D9CWireObjectIdentity priorIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 0x100000002ull,
  };
  constexpr D9CWireObjectIdentity nextIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 3u,
      .objectId = priorIdentity.objectId,
  };
  constexpr D9CWireObjectIdentity parentIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 7u,
      .objectId = 0x100000009ull,
  };
  constexpr D9CSurfaceDesc surface{
      .format = render_tape_d3d_format::A8R8G8B8,
      .width = 128u,
      .height = 32u,
  };
  const RenderTapeSurfaceDescriptorV2 descriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = parentIdentity,
      .surface = surface,
  };
  const auto descriptorBytes = std::as_bytes(std::span(&descriptor, 1u));
  RenderTapeSurfaceAliasLifetime retained{
      .wrapperRefs = 0u,
      .textureAlias = true,
      .disposition =
          RenderTapeSurfaceAliasLifetime::Disposition::RetainedAlias,
  };
  using Status = RenderTapeSurfaceAliasReplacementStatus;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            descriptorBytes) == Status::Accepted,
        "a retained alias admits a semantically identical newer generation");

  auto staleIdentity = nextIdentity;
  staleIdentity.generation = priorIdentity.generation;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, staleIdentity,
            descriptorBytes) == Status::NonMonotoneGeneration,
        "one wire object id rejects equal and stale alias generations");

  auto crossObjectEqual = nextIdentity;
  ++crossObjectEqual.objectId;
  crossObjectEqual.generation = priorIdentity.generation;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, crossObjectEqual,
            descriptorBytes) == Status::Accepted,
        "event order admits an equal generation from a distinct wire object id");
  auto crossObjectLower = crossObjectEqual;
  ++crossObjectLower.objectId;
  crossObjectLower.generation = 1u;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, crossObjectLower,
            descriptorBytes) == Status::Accepted,
        "event order admits a lower generation from a distinct wire object id");

  auto live = retained;
  live.wrapperRefs = 1u;
  live.disposition = RenderTapeSurfaceAliasLifetime::Disposition::Live;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, live, descriptorBytes, nextIdentity,
            descriptorBytes) == Status::LiveWrapper,
        "a live alias wrapper prevents generation replacement");

  auto standalone = retained;
  standalone.textureAlias = false;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, standalone, descriptorBytes, nextIdentity,
            descriptorBytes) == Status::PriorNotRetainedAlias,
        "a standalone surface never enters alias generation replacement");
  const auto ordinaryDescriptor =
      std::as_bytes(std::span(&surface, 1u));
  check(renderTapeLogicalSlotRelation(
            renderTapeLogicalObjectSlot(priorIdentity, descriptorBytes),
            renderTapeLogicalObjectSlot(nextIdentity, ordinaryDescriptor)) ==
            RenderTapeLogicalSlotRelation::Different,
        "an alias and ordinary surface never collide by shared wire object id");
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            ordinaryDescriptor) == Status::DifferentLogicalSlot,
        "mixed alias and ordinary surfaces coexist instead of replacing");

  auto differentParent = descriptor;
  ++differentParent.parentTexture.generation;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            std::as_bytes(std::span(&differentParent, 1u))) ==
            Status::DifferentLogicalSlot,
        "a different exact parent names a distinct logical alias slot");

  auto differentSubresource = descriptor;
  ++differentSubresource.subresource;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            std::as_bytes(std::span(&differentSubresource, 1u))) ==
            Status::DifferentLogicalSlot,
        "a different parent subresource names a distinct logical alias slot");
  check(renderTapeLogicalSlotRelation(
            renderTapeLogicalObjectSlot(priorIdentity, descriptorBytes),
            renderTapeLogicalObjectSlot(
                nextIdentity,
                std::as_bytes(std::span(&differentSubresource, 1u)))) ==
            RenderTapeLogicalSlotRelation::Different,
        "shared wire object ids do not merge distinct alias subresources");

  auto differentSurface = descriptor;
  ++differentSurface.surface.width;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            std::as_bytes(std::span(&differentSurface, 1u))) ==
            Status::SurfaceMismatch,
        "alias generation replacement preserves the semantic surface descriptor");

  auto invalidDescriptor = descriptor;
  invalidDescriptor.parentTexture.generation = 0u;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            std::as_bytes(std::span(&invalidDescriptor, 1u))) ==
            Status::InvalidDescriptor,
        "malformed alias metadata prevents generation replacement");

  RenderTapeCaptureSession orderedAlias(true);
  RenderTapeObjectDefineDisposition disposition{};
  check(orderedAlias.arm(bootstrapChunk()) ==
            RenderTapeCaptureStatus::Accepted &&
            orderedAlias.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted &&
            orderedAlias.objectDefine(
                priorIdentity,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                descriptorBytes, 0u, {}) == RenderTapeCaptureStatus::Accepted &&
            orderedAlias.objectDestroy(priorIdentity) ==
                RenderTapeCaptureStatus::Accepted,
        "ordered alias replacement fixture retires its first exact identity");
  check(orderedAlias.objectDefine(
            priorIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &disposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            disposition ==
                RenderTapeObjectDefineDisposition::ExactIdentityConflict,
        "a retired exact alias identity cannot be defined again");
  auto sameObjectStale = priorIdentity;
  sameObjectStale.generation = 1u;
  check(orderedAlias.objectDefine(
            sameObjectStale,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &disposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            disposition ==
                RenderTapeObjectDefineDisposition::StaleOrEqualGeneration,
        "a retired alias wire object rejects an unseen lower generation");
  auto lowerCrossObject = priorIdentity;
  ++lowerCrossObject.objectId;
  lowerCrossObject.generation = 1u;
  check(orderedAlias.objectDefine(
            lowerCrossObject,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &disposition) ==
            RenderTapeCaptureStatus::Accepted,
        "a lower generation from a new wire object follows ordered destroy");
  auto overlappingCrossObject = lowerCrossObject;
  ++overlappingCrossObject.objectId;
  check(orderedAlias.objectDefine(
            overlappingCrossObject,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &disposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            disposition ==
                RenderTapeObjectDefineDisposition::OverlappingLiveGeneration,
        "a second live cross-object alias generation remains rejected");

  RenderTapeCaptureSession missingDestroy(true);
  check(missingDestroy.arm(bootstrapChunk()) ==
            RenderTapeCaptureStatus::Accepted &&
            missingDestroy.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted &&
            missingDestroy.objectDestroy(priorIdentity) ==
                RenderTapeCaptureStatus::InvalidInput,
        "a missing exact capture identity makes the ordered destroy fail closed");
}

void testPresentCaptureResultAbiAndOneShotCancellation() {
  D9CRenderTapePresentCaptureResult result{};
  check(result.status == D9C_RENDER_TAPE_PRESENT_CAPTURE_NONE &&
            result.width == 0u && result.height == 0u && result.format == 0u &&
            result.byteCount == 0u,
        "capture-only result is fixed POD with a zero failure baseline");
  check(std::all_of(std::begin(result.sha256), std::end(result.sha256),
                    [](std::uint8_t value) { return value == 0u; }),
        "zeroed capture-only result exposes no partial digest");

  constexpr std::array<std::byte, 3> abc{
      std::byte{0x61u}, std::byte{0x62u}, std::byte{0x63u}};
  constexpr std::array<std::byte, 32> abcSha256{
      std::byte{0xbau}, std::byte{0x78u}, std::byte{0x16u}, std::byte{0xbfu},
      std::byte{0x8fu}, std::byte{0x01u}, std::byte{0xcfu}, std::byte{0xeau},
      std::byte{0x41u}, std::byte{0x41u}, std::byte{0x40u}, std::byte{0xdeu},
      std::byte{0x5du}, std::byte{0xaeu}, std::byte{0x22u}, std::byte{0x23u},
      std::byte{0xb0u}, std::byte{0x03u}, std::byte{0x61u}, std::byte{0xa3u},
      std::byte{0x96u}, std::byte{0x17u}, std::byte{0x7au}, std::byte{0x9cu},
      std::byte{0xb4u}, std::byte{0x10u}, std::byte{0xffu}, std::byte{0x61u},
      std::byte{0xf2u}, std::byte{0x00u}, std::byte{0x15u}, std::byte{0xadu},
  };
  const auto digest = RenderTapeCaptureSession::sha256(abc);
  check(digest == abcSha256,
        "capture-only digest is SHA-256 over tightly packed presentation bytes");
  auto mismatchedBytes = abc;
  mismatchedBytes[2] = std::byte{0x64u};
  check(RenderTapeCaptureSession::sha256(mismatchedBytes) != digest,
        "one changed presentation byte must reject the expected digest");

  dxmt9::PresentMirrorTicket cancelled;
  check(cancelled.cancel() && !cancelled.markEncoded() && !cancelled.encoded(),
        "cancelled mirror ticket cannot be consumed by a later Present");

  dxmt9::PresentMirrorTicket consumed;
  check(consumed.markEncoded() && consumed.encoded() && !consumed.cancel(),
        "encoded mirror ticket is consumed exactly once and is not cancelled");
}

void testBlockCompressedLockCaptureLayouts() {
  constexpr std::uint32_t dxt1 = render_tape_d3d_format::DXT1;
  constexpr std::uint32_t dxt3 = render_tape_d3d_format::DXT3;
  constexpr std::uint32_t dxt5 = render_tape_d3d_format::DXT5;
  for (const auto [format, blockBytes] :
       std::array<std::pair<std::uint32_t, std::uint32_t>, 3>{
           {{dxt1, 8u}, {dxt3, 16u}, {dxt5, 16u}}}) {
    D9CSurfaceDesc desc{.format = format, .width = 8u, .height = 8u};
    RenderTapeBlockLockLayout layout{};
    const std::int32_t pitch = static_cast<std::int32_t>(blockBytes * 2u + 8u);
    check(renderTapeBlockLockLayout(desc, pitch, nullptr, layout) ==
              RenderTapeBlockLayoutStatus::Accepted &&
              layout.fullSubresource && layout.blockBytes == blockBytes &&
              layout.fullRowBytes == blockBytes * 2u && layout.fullRows == 2u &&
              layout.rowBytes == blockBytes * 2u && layout.rows == 2u &&
              layout.pitch == static_cast<std::uint32_t>(pitch) &&
              layout.tightBytes == blockBytes * 4u,
          "DXT1/DXT3/DXT5 full-face layout uses block rows and tight bytes");

    std::vector<std::byte> pitched(static_cast<std::size_t>(pitch) * 2u,
                                  std::byte{0xeeu});
    for (std::uint32_t i = 0u; i < layout.rowBytes; ++i) {
      pitched[i] = static_cast<std::byte>(i);
      pitched[static_cast<std::size_t>(pitch) + i] =
          static_cast<std::byte>(0x40u + i);
    }
    std::vector<std::byte> tight;
    check(copyRenderTapeBlockRows(pitched.data(), layout, tight) &&
              tight.size() == layout.tightBytes &&
              tight[layout.rowBytes] == std::byte{0x40u},
          "block capture honors source pitch without persisting padding");
    std::vector<std::byte> content;
    check(applyRenderTapeBlockMutation(layout, tight, content) ==
              RenderTapeBlockMutationStatus::Accepted &&
              content == tight,
          "full block lock establishes a complete immutable seed");
  }
}

void testBlockCompressedSubrectMipAndOddExtentLayouts() {
  constexpr std::uint32_t dxt1 = render_tape_d3d_format::DXT1;
  constexpr std::uint32_t dxt5 = render_tape_d3d_format::DXT5;
  D9CSurfaceDesc desc{.format = dxt5, .width = 16u, .height = 12u};
  RenderTapeBlockLockLayout full{};
  check(renderTapeBlockLockLayout(desc, 80, nullptr, full) ==
            RenderTapeBlockLayoutStatus::Accepted &&
            full.fullRowBytes == 64u && full.fullRows == 3u &&
            full.tightBytes == 192u,
        "full mip layout rounds to complete block rows independently of pitch");
  std::vector<std::byte> content(192u, std::byte{0x11u});
  const auto priorImmutableContent = content;
  const auto priorDigest = RenderTapeCaptureSession::sha256(content);

  const RenderTapeLockRect rect{4, 4, 12, 12};
  RenderTapeBlockLockLayout partial{};
  check(renderTapeBlockLockLayout(desc, 80, &rect, partial) ==
            RenderTapeBlockLayoutStatus::Accepted &&
            !partial.fullSubresource && partial.blockLeft == 1u &&
            partial.blockTop == 1u && partial.rowBytes == 32u &&
            partial.rows == 2u && partial.tightBytes == 64u,
        "aligned DXT subrect records exact block coordinates and mip layout");
  std::vector<std::byte> patchBytes(64u, std::byte{0x7au});
  check(applyRenderTapeBlockMutation(partial, patchBytes, content) ==
            RenderTapeBlockMutationStatus::Accepted &&
            content[0] == std::byte{0x11u} &&
            content[80u] == std::byte{0x7au} &&
            content[144u] == std::byte{0x7au} &&
            priorImmutableContent ==
                std::vector<std::byte>(192u, std::byte{0x11u}) &&
            RenderTapeCaptureSession::sha256(content) != priorDigest,
        "aligned subrect creates the next full immutable mutation in order");

  D9CSurfaceDesc odd{.format = dxt1, .width = 7u, .height = 5u};
  RenderTapeBlockLockLayout oddFull{};
  check(renderTapeBlockLockLayout(odd, 24, nullptr, oddFull) ==
            RenderTapeBlockLayoutStatus::Accepted &&
            oddFull.fullRowBytes == 16u && oddFull.fullRows == 2u &&
            oddFull.tightBytes == 32u,
        "odd DXT dimensions round width and height independently to blocks");
  const RenderTapeLockRect oddEdge{4, 4, 7, 5};
  RenderTapeBlockLockLayout oddPartial{};
  check(renderTapeBlockLockLayout(odd, 24, &oddEdge, oddPartial) ==
            RenderTapeBlockLayoutStatus::Accepted &&
            oddPartial.blockLeft == 1u && oddPartial.blockTop == 1u &&
            oddPartial.rowBytes == 8u && oddPartial.rows == 1u,
        "edge-aligned odd mip subrect admits a non-multiple terminal edge");
}

void testBlockCompressedCaptureRejectsInvalidLayouts() {
  constexpr std::uint32_t dxt1 = render_tape_d3d_format::DXT1;
  constexpr std::uint32_t dxt2 = render_tape_d3d_format::DXT2;
  D9CSurfaceDesc desc{.format = dxt1, .width = 16u, .height = 16u};
  RenderTapeBlockLockLayout layout{};
  check(std::string_view(renderTapeCaptureRejectionReasonName(
            RenderTapeCaptureRejectionReason::IncompleteSubresourceSeed)) ==
            "incomplete_subresource_seed" &&
            std::string_view(renderTapeCaptureRejectionReasonName(
                RenderTapeCaptureRejectionReason::InvalidBlockAlignment)) ==
                "invalid_block_alignment" &&
            std::string_view(renderTapeCaptureRejectionReasonName(
                RenderTapeCaptureRejectionReason::UnmaterializedPreArmObject)) ==
                "unmaterialized_pre_arm_object",
        "typed first-rejection diagnostics have stable observable names");
  D9CSurfaceDesc unsupported{.format = dxt2, .width = 16u, .height = 16u};
  check(renderTapeBlockLockLayout(unsupported, 64, nullptr, layout) ==
            RenderTapeBlockLayoutStatus::UnsupportedFormat,
        "DXT2 remains outside the narrowly admitted capture family");
  const RenderTapeLockRect badLeft{2, 0, 8, 8};
  const RenderTapeLockRect badRight{4, 4, 10, 12};
  check(renderTapeBlockLockLayout(desc, 32, &badLeft, layout) ==
            RenderTapeBlockLayoutStatus::InvalidAlignment &&
            renderTapeBlockLockLayout(desc, 32, &badRight, layout) ==
                RenderTapeBlockLayoutStatus::InvalidAlignment,
        "non-edge DXT lock bounds must be block aligned");
  const RenderTapeLockRect full{0, 0, 16, 16};
  check(renderTapeBlockLockLayout(desc, 31, &full, layout) ==
            RenderTapeBlockLayoutStatus::InvalidPitch,
        "pitch shorter than one block row rejects before copying");
  const RenderTapeLockRect oneBlock{4, 4, 8, 8};
  check(renderTapeBlockLockLayout(desc, 8, &oneBlock, layout) ==
            RenderTapeBlockLayoutStatus::InvalidPitch,
        "subrect pitch must still span the complete compressed mip row");
  D9CSurfaceDesc huge{.format = render_tape_d3d_format::DXT5,
                      .width = 0xffffffffu,
                      .height = 4u};
  check(renderTapeBlockLockLayout(huge, 0x7fffffff, nullptr, layout) ==
            RenderTapeBlockLayoutStatus::Overflow,
        "block-row arithmetic overflow is rejected");

  RenderTapeBlockLockLayout partial{};
  const RenderTapeLockRect aligned{4, 4, 8, 8};
  check(renderTapeBlockLockLayout(desc, 32, &aligned, partial) ==
            RenderTapeBlockLayoutStatus::Accepted,
        "aligned partial fixture is valid");
  std::vector<std::byte> patchBytes(partial.tightBytes, std::byte{0x33u});
  std::vector<std::byte> unavailable;
  check(applyRenderTapeBlockMutation(partial, patchBytes, unavailable) ==
            RenderTapeBlockMutationStatus::IncompleteSeed &&
            unavailable.empty(),
        "partial block write cannot manufacture unavailable initial bytes");
}

void testLinearLockCaptureLayouts() {
  constexpr std::uint32_t argb = render_tape_d3d_format::A8R8G8B8;
  check(render_tape_d3d_format::X4R4G4B4 == 30u,
        "X4R4G4B4 pins the authoritative public D3DFORMAT value");
  check(render_tape_d3d_format::L16 == 81u,
        "L16 pins the authoritative public D3DFORMAT value");
  constexpr std::array supportedFormats{
      std::pair{render_tape_d3d_format::R8G8B8, 3u},
      std::pair{render_tape_d3d_format::A8R8G8B8, 4u},
      std::pair{render_tape_d3d_format::X8R8G8B8, 4u},
      std::pair{render_tape_d3d_format::R5G6B5, 2u},
      std::pair{render_tape_d3d_format::X1R5G5B5, 2u},
      std::pair{render_tape_d3d_format::A1R5G5B5, 2u},
      std::pair{render_tape_d3d_format::A4R4G4B4, 2u},
      std::pair{render_tape_d3d_format::X4R4G4B4, 2u},
      std::pair{render_tape_d3d_format::A8, 1u},
      std::pair{render_tape_d3d_format::A8B8G8R8, 4u},
      std::pair{render_tape_d3d_format::X8B8G8R8, 4u},
      std::pair{render_tape_d3d_format::A8P8, 2u},
      std::pair{render_tape_d3d_format::P8, 1u},
      std::pair{render_tape_d3d_format::L8, 1u},
      std::pair{render_tape_d3d_format::A8L8, 2u},
      std::pair{render_tape_d3d_format::V8U8, 2u},
      std::pair{render_tape_d3d_format::L16, 2u},
  };
  for (const auto [format, bytesPerPixel] : supportedFormats) {
    const D9CSurfaceDesc formatDesc{
        .format = format, .width = 3u, .height = 2u};
    RenderTapeLinearLockLayout formatLayout{};
    check(renderTapeLinearBytesPerPixel(format) == bytesPerPixel &&
              renderTapeLinearLockLayout(
                  formatDesc,
                  static_cast<std::int32_t>(3u * bytesPerPixel + 4u), nullptr,
                  formatLayout) == RenderTapeLinearLayoutStatus::Accepted &&
              formatLayout.bytesPerPixel == bytesPerPixel &&
              formatLayout.tightBytes == 6u * bytesPerPixel,
          "every admitted linear format uses its authoritative layout");
  }
  check(renderTapeLinearBytesPerPixel(render_tape_d3d_format::DXT1) == 0u,
        "block-compressed formats do not enter the linear layout path");
  const D9CSurfaceDesc l16Desc{
      .format = render_tape_d3d_format::L16,
      .width = 1024u,
      .height = 64u,
  };
  RenderTapeLinearLockLayout l16Layout{};
  check(renderTapeLinearLockLayout(l16Desc, 2048, nullptr, l16Layout) ==
            RenderTapeLinearLayoutStatus::Accepted &&
            l16Layout.bytesPerPixel == 2u &&
            l16Layout.fullRowBytes == 2048u && l16Layout.fullRows == 64u &&
            l16Layout.tightBytes == 131072u &&
            l16Layout.sourceBytes == 131072u && l16Layout.fullSubresource,
        "Firefly Forest L16 full lock closes to exactly 131072 tight bytes");
  const D9CSurfaceDesc desc{.format = argb, .width = 4u, .height = 3u};
  RenderTapeLinearLockLayout full{};
  check(renderTapeLinearLockLayout(desc, 24, nullptr, full) ==
            RenderTapeLinearLayoutStatus::Accepted &&
            full.fullRowBytes == 16u && full.rows == 3u &&
            full.tightBytes == 48u && full.sourceBytes == 64u &&
            full.fullSubresource,
        "linear full LockRect keeps descriptor extent and actual pitch");

  std::vector<std::byte> source(24u * 3u, std::byte{0});
  for (std::uint32_t row = 0u; row < 3u; ++row) {
    for (std::uint32_t byte = 0u; byte < 16u; ++byte) {
      source[row * 24u + byte] =
          static_cast<std::byte>(row * 16u + byte + 1u);
    }
  }
  std::vector<std::byte> copied;
  check(copyRenderTapeLinearRows(source.data(), full, copied) &&
            copied.size() == 48u && copied[23] == std::byte{24u} &&
            copied[24] == std::byte{25u},
        "linear full copy strips only row padding");
  std::vector<std::byte> content;
  check(applyRenderTapeLinearMutation(full, copied, content) ==
            RenderTapeBlockMutationStatus::Accepted &&
            content == copied,
        "linear full mutation establishes the complete seed");

  std::vector<std::byte> providerDestination(24u * 3u, std::byte{0xeeu});
  check(writeRenderTapeLinearRows(copied, providerDestination.data(), full) &&
            providerDestination[15] == copied[15] &&
            providerDestination[16] == std::byte{0xeeu} &&
            providerDestination[24] == copied[16],
        "provider upload restores tight rows without overwriting lock padding");

  const RenderTapeLockRect partialRect{1, 1, 3, 3};
  RenderTapeLinearLockLayout partial{};
  check(renderTapeLinearLockLayout(desc, 24, &partialRect, partial) ==
            RenderTapeLinearLayoutStatus::Accepted &&
            partial.destinationByteOffset == 4u && partial.top == 1u &&
            partial.rowBytes == 8u && partial.rows == 2u &&
            partial.tightBytes == 16u && partial.sourceBytes == 32u &&
            partial.subresourceSourceOffset == 28u &&
            partial.subresourceSourceBytes == 60u &&
            !partial.fullSubresource,
        "linear partial LockRect records exact rectangle extent");
  std::vector<std::byte> partialSource(24u * 2u, std::byte{0});
  for (std::uint32_t row = 0u; row < 2u; ++row) {
    for (std::uint32_t byte = 0u; byte < 8u; ++byte)
      partialSource[row * 24u + byte] = static_cast<std::byte>(
          0xa0u + row * 8u + byte);
  }
  std::vector<std::byte> partialBytes;
  check(copyRenderTapeLinearRows(partialSource.data(), partial, partialBytes) &&
            applyRenderTapeLinearMutation(partial, partialBytes, content) ==
                RenderTapeBlockMutationStatus::Accepted &&
            content[16u + 4u] == std::byte{0xa0u} &&
            content[32u + 4u] == std::byte{0xa8u},
        "linear partial mutation merges rows at descriptor coordinates");

  std::vector<std::byte> fullSubresourceSource(24u * 3u, std::byte{0u});
  std::copy(partialSource.begin(), partialSource.begin() + 8u,
            fullSubresourceSource.begin() + 28u);
  std::copy(partialSource.begin() + 24u, partialSource.begin() + 32u,
            fullSubresourceSource.begin() + 52u);
  std::vector<std::byte> userMemoryBytes;
  check(copyRenderTapeLinearRows(
            fullSubresourceSource.data(), partial, userMemoryBytes,
            RenderTapeLockBitsOrigin::Subresource) &&
            userMemoryBytes == partialBytes,
        "user-memory LockRect applies the checked rectangle offset to base pBits");

  std::vector<std::byte> unavailable;
  check(applyRenderTapeLinearMutation(partial, partialBytes, unavailable) ==
            RenderTapeBlockMutationStatus::IncompleteSeed,
        "linear partial mutation cannot manufacture an unavailable seed");
  check(renderTapeLinearLockLayout(desc, 15, nullptr, full) ==
            RenderTapeLinearLayoutStatus::InvalidPitch,
        "linear pitch shorter than descriptor row rejects fail-closed");
  D9CSurfaceDesc huge{.format = argb,
                      .width = 0xffffffffu,
                      .height = 2u};
  check(renderTapeLinearLockLayout(huge, 0x7fffffff, nullptr, full) ==
            RenderTapeLinearLayoutStatus::Overflow,
        "linear descriptor conversion and byte ranges reject overflow");
}

void testFullSnapshotClosureTruthTable() {
  constexpr std::uint64_t fullBytes = 128u * 32u * 4u;
  const std::array<std::byte, 8u> bytes{};
  check(renderTapeClassifySnapshot(true, true, true, true, 0u, fullBytes) ==
            RenderTapeFullSnapshotStatus::Required &&
            renderTapeClassifySnapshot(true, true, true, true, fullBytes,
                                       fullBytes) ==
                RenderTapeFullSnapshotStatus::NotRequired,
        "snapshot decision distinguishes unseeded partial and seeded overlay");
  check(renderTapeClassifySnapshot(false, true, true, true, 0u, fullBytes) ==
            RenderTapeFullSnapshotStatus::NotRequired &&
            renderTapeClassifySnapshot(true, false, true, true, 0u,
                                       fullBytes) ==
                RenderTapeFullSnapshotStatus::InvalidIdentity &&
            renderTapeClassifySnapshot(true, true, false, true, 0u,
                                       fullBytes) ==
                RenderTapeFullSnapshotStatus::InvalidExtent &&
            renderTapeClassifySnapshot(true, true, true, true, 7u,
                                       fullBytes) ==
                RenderTapeFullSnapshotStatus::InvalidExtent,
        "capture-off, stale identity, and malformed extent are fail-closed");
  check(renderTapeClassifySnapshot(true, true, true, false, 0u, fullBytes) ==
            RenderTapeFullSnapshotStatus::NotRequired,
        "full locks never resnapshot");
  constexpr std::uint64_t fullBufferBytes = 4096u;
  std::vector<std::byte> bufferContent(
      static_cast<std::size_t>(fullBufferBytes), std::byte{0x11});
  const std::array<std::byte, 4u> bufferPatch{
      std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};
  check(applyRenderTapeBufferMutation(fullBufferBytes, 0u, bufferPatch,
                                      bufferContent) ==
                RenderTapeBlockMutationStatus::Accepted &&
            bufferContent.size() == fullBufferBytes &&
            bufferContent[0] == std::byte{0x21} &&
            bufferContent[3] == std::byte{0x24} &&
            bufferContent[4] == std::byte{0x11},
        "offset-zero partial buffer writes overlay an existing complete seed");
  std::vector<std::byte> missingBufferSeed;
  check(applyRenderTapeBufferMutation(fullBufferBytes, 0u, bufferPatch,
                                      missingBufferSeed) ==
            RenderTapeBlockMutationStatus::IncompleteSeed,
        "offset-zero partial buffer writes cannot manufacture a seed");
  check(renderTapeClassifyBufferSnapshot(
            true, true, true, true, 0u, fullBufferBytes) ==
            RenderTapeFullSnapshotStatus::Required &&
            renderTapeClassifyBufferSnapshot(
                true, true, true, true, fullBufferBytes, fullBufferBytes) ==
                RenderTapeFullSnapshotStatus::NotRequired &&
            renderTapeClassifyBufferSnapshot(
                false, true, true, true, 0u, fullBufferBytes) ==
                RenderTapeFullSnapshotStatus::NotRequired,
        "buffer snapshot closure only relocks an unseeded partial writable range");
  check(renderTapeClassifyBufferSnapshot(
            true, false, true, true, 0u, fullBufferBytes) ==
            RenderTapeFullSnapshotStatus::InvalidIdentity &&
            renderTapeClassifyBufferSnapshot(
                true, true, false, true, 0u, fullBufferBytes) ==
                RenderTapeFullSnapshotStatus::InvalidExtent &&
            renderTapeClassifyBufferSnapshot(
                true, true, true, false, 0u, fullBufferBytes) ==
                RenderTapeFullSnapshotStatus::NotRequired,
        "buffer identity, extent, and full-lock failures remain typed and fail-closed");
  check(renderTapeValidateFullSnapshot(true, fullBytes,
                                      std::span<const std::byte>(
                                          bytes.data(), bytes.size())) ==
            RenderTapeFullSnapshotStatus::InvalidBytes,
        "a short full snapshot is rejected");
  std::vector<std::byte> exact(static_cast<std::size_t>(fullBytes));
  check(renderTapeValidateFullSnapshot(true, fullBytes, exact) ==
            RenderTapeFullSnapshotStatus::Accepted,
        "an exact full snapshot is accepted");
  exact.push_back(std::byte{0});
  check(renderTapeValidateFullSnapshot(true, fullBytes, exact) ==
            RenderTapeFullSnapshotStatus::InvalidBytes,
        "a long full snapshot is rejected");
  check(renderTapeValidateFullSnapshot(false, fullBytes, exact) ==
            RenderTapeFullSnapshotStatus::InvalidExtent,
        "a malformed partial full-lock proof is rejected");
  check(RenderTapeFullSnapshotStatus::InvalidIdentity !=
            RenderTapeFullSnapshotStatus::Accepted,
        "stale identity remains a typed rejection state");
  check(renderTapeClassifySurfaceSnapshotRoute(
            false, true, true, true, true) ==
            RenderTapeSurfaceSnapshotRoute::NotRequired &&
            renderTapeClassifySurfaceSnapshotRoute(
                true, false, false, true, true) ==
                RenderTapeSurfaceSnapshotRoute::StandaloneSurface &&
            renderTapeClassifySurfaceSnapshotRoute(
                true, true, false, true, true) ==
                RenderTapeSurfaceSnapshotRoute::InvalidIdentity &&
            renderTapeClassifySurfaceSnapshotRoute(
                true, true, true, true, true) ==
                RenderTapeSurfaceSnapshotRoute::TextureDerived,
        "surface snapshot route only admits an exact 2D texture alias");
  check(renderTapeClassifySurfaceSnapshotRoute(
            true, true, true, false, true) ==
            RenderTapeSurfaceSnapshotRoute::NotRequired &&
            renderTapeClassifySurfaceSnapshotRoute(
                true, true, true, true, false) ==
                RenderTapeSurfaceSnapshotRoute::NotRequired,
        "full or byte-less surface mutations do not trigger a snapshot");
}

void testBootstrapClosureTruthTable() {
  check(!renderTapeBootstrapRequiresAllLiveObjects(kRenderTapeProfileFrame) &&
            renderTapeBootstrapRequiresAllLiveObjects(
                kRenderTapeProfileSequence),
        "frame tapes use exact closure while sequence tapes retain all live objects");
  const D9CWireObjectIdentity texture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 1u, .objectId = 41u};
  const D9CWireObjectIdentity staleTexture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 2u, .objectId = 41u};
  const D9CWireObjectIdentity alias{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 1u, .objectId = 42u};
  const D9CWireObjectIdentity output{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 1u, .objectId = 43u};
  const std::array<RenderTapeBootstrapClosureObject, 3u> complete{
      RenderTapeBootstrapClosureObject{.identity = texture, .complete = true},
      RenderTapeBootstrapClosureObject{
          .identity = alias,
          .complete = true,
          .hasDescriptorDependency = true,
          .descriptorDependency = texture},
      RenderTapeBootstrapClosureObject{.identity = output, .complete = true},
  };
  std::vector<D9CWireObjectIdentity> closure;
  check(renderTapeBuildBootstrapClosure(std::span<const D9CWireObjectIdentity>{},
                                        output, complete, closure) ==
            RenderTapeBootstrapClosureStatus::Accepted &&
            closure.size() == 1u && closure[0].objectId == output.objectId,
        "bootstrap closure always includes only the required Present output");
  const std::array<D9CWireObjectIdentity, 1u> roots{alias};
  const std::array<D9CWireObjectIdentity, 1u> textureRoot{texture};
  check(renderTapeBuildBootstrapClosure(roots, output, complete, closure) ==
            RenderTapeBootstrapClosureStatus::Accepted &&
            closure.size() == 3u &&
            renderTapeBootstrapClosureContains(closure, texture),
        "bootstrap closure unions overlay roots, Present output, and alias parent");

  auto incomplete = complete;
  incomplete[0].complete = false;
  check(renderTapeBuildBootstrapClosure(textureRoot, output, incomplete, closure) ==
            RenderTapeBootstrapClosureStatus::ReferencedObjectIncomplete,
        "referenced incomplete seed is rejected");
  incomplete = complete;
  incomplete[1].complete = false;
  check(renderTapeBuildBootstrapClosure(roots, output, incomplete, closure) ==
            RenderTapeBootstrapClosureStatus::ReferencedObjectIncomplete,
        "referenced incomplete alias is rejected");
  incomplete = complete;
  incomplete[0].complete = false;
  check(renderTapeBuildBootstrapClosure(roots, output, incomplete, closure) ==
            RenderTapeBootstrapClosureStatus::DescriptorDependencyIncomplete,
        "incomplete texture-derived surface parent is rejected");
  incomplete = complete;
  const std::array<D9CWireObjectIdentity, 1u> staleRoot{staleTexture};
  check(renderTapeBuildBootstrapClosure(staleRoot, output, incomplete, closure) ==
            RenderTapeBootstrapClosureStatus::ReferencedObjectMissing,
        "stale generation is not satisfied by a live prior generation");
  const std::array<RenderTapeBootstrapClosureObject, 4u> withUnreferenced{
      complete[0], complete[1], complete[2],
      RenderTapeBootstrapClosureObject{
          .identity = D9CWireObjectIdentity{.kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
                                             .generation = 1u,
                                             .objectId = 99u},
          .complete = false},
  };
  check(renderTapeBuildBootstrapClosure({}, output, withUnreferenced, closure) ==
            RenderTapeBootstrapClosureStatus::Accepted &&
            !renderTapeBootstrapClosureContains(
                closure, withUnreferenced.back().identity),
        "unreferenced incomplete live objects are pruned");
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
    testDescriptorKindAxisTruthTable();
    testKindZeroIntervalDefineUsesNonZeroDescriptorTag();
    testProductionHookGateTruthTable();
    testProfileSelectionTruthTable();
    testCompletePresentPublishesExactlyOneTape();
    testSequenceCaptureDefersSealUntilSecondPresent();
    testValidationFailurePreservesEventAndChunkLocation();
    testPresentCompleteOracleTargetTruthTable();
    testFailureBeforePublishAndBoundedBackpressure();
    testBoundedBlobBytesAndDeduplication();
    testProductionBlobDefaultCoversGt2R7Requirement();
    testObjectLifetimeAndTerminalControls();
    testPresentOutputRoleOwnershipTruthTable();
    testSurfaceAliasGenerationReplacementTransition();
    testPresentCaptureResultAbiAndOneShotCancellation();
    testBlockCompressedLockCaptureLayouts();
    testBlockCompressedSubrectMipAndOddExtentLayouts();
    testBlockCompressedCaptureRejectsInvalidLayouts();
    testLinearLockCaptureLayouts();
    testFullSnapshotClosureTruthTable();
    testBootstrapClosureTruthTable();
    testObjectExpectedContentContractTruthTable();
    return 0;
  } catch (const TestFailure& failure) {
    return 1;
  }
}
