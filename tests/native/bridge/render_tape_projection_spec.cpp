#include "device_c_render_tape_projection.hpp"

// R-HARN-REPLAY-7.16/7.17: pure frame-tape draw-range projection, canonical
// locator conservation, exact object/blob closure, and pre-effect rejection.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
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
  if (!condition) throw TestFailure(std::string(message));
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

struct Record {
  std::uint32_t type = 0u;
  std::vector<std::byte> payload{};
  std::vector<D9CCommandChunkWireHandleEntry> handles{};
};

std::vector<std::byte> makeChunk(std::span<const Record> specs) {
  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  std::size_t handleCount = 0u;
  for (const auto& spec : specs) handleCount += spec.handles.size();
  const auto handleTableOffset = alignUp(
      recordTableOffset + specs.size() * sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset = alignUp(
      handleTableOffset + handleCount * sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));

  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  std::vector<std::byte> payload;
  for (const auto& spec : specs) {
    const auto* rule = recordRule(spec.type);
    check(rule != nullptr, "fixture record type must be canonical");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back(D9CCommandChunkWireRecordHeader{
        .type = spec.type,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
    });
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
  }
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  std::vector<std::byte> bytes(payloadArenaOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  if (!records.empty()) {
    std::memcpy(bytes.data() + recordTableOffset, records.data(),
                records.size() * sizeof(records[0]));
  }
  if (!handles.empty()) {
    std::memcpy(bytes.data() + handleTableOffset, handles.data(),
                handles.size() * sizeof(handles[0]));
  }
  if (!payload.empty()) {
    std::memcpy(bytes.data() + payloadArenaOffset, payload.data(),
                payload.size());
  }
  return bytes;
}

constexpr D9CWireObjectIdentity kOutput{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 1u,
    .objectId = 11u,
};
constexpr D9CWireObjectIdentity kTexture{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 3u,
    .objectId = 29u,
};
constexpr D9CWireObjectIdentity kShader{
    .kind = D9C_CHUNK_HANDLE_KIND_SHADER,
    .generation = 7u,
    .objectId = 43u,
};

RenderTapeDigest digest(std::byte seed) {
  RenderTapeDigest value{};
  for (std::size_t i = 0u; i < value.size(); ++i) {
    value[i] = static_cast<std::byte>(static_cast<unsigned>(seed) + i);
  }
  return value;
}

const RenderTapeDigest kTextureDigest = digest(std::byte{0x10});
const RenderTapeDigest kShaderDigest = digest(std::byte{0x40});

std::vector<std::byte> drawPayload(bool fullSnapshot,
                                   std::uint32_t firstHandle = 0u) {
  std::array<D9CCommandChunkWireTextureBinding,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = D9CCommandChunkWireTextureBinding{
        .slot = slot,
        .valid = 1u,
        .handleIndex = slot == 0u ? firstHandle
                                  : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = D9CCommandChunkWireStreamBinding{
        .slot = slot,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  const D9CCommandChunkWireShaderBinding shader{
      .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
      .valid = 1u,
      .handleIndex = firstHandle + 1u,
  };

  constexpr std::uint32_t sectionCount = 3u;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamOffset = sectionPayloadOffset + sizeof(textures);
  const auto shaderOffset = streamOffset + sizeof(streams);
  const D9CCommandChunkWireDrawHeader draw{
      .flags = fullSnapshot ? D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT : 0u,
      .primitiveType = 4u,
      .primitiveCount = 1u,
      .sectionCount = sectionCount,
      .sectionTableOffset = static_cast<std::uint32_t>(sectionTableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
  };
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(textures[0]),
          .count = static_cast<std::uint32_t>(textures.size()),
          .payloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
          .byteSize = sizeof(textures),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(streams[0]),
          .count = static_cast<std::uint32_t>(streams.size()),
          .payloadOffset = static_cast<std::uint32_t>(streamOffset),
          .byteSize = sizeof(streams),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_SHADER,
          .elementSize = sizeof(shader),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(shaderOffset),
          .byteSize = sizeof(shader),
      },
  };
  std::vector<std::byte> payload(shaderOffset + sizeof(shader));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(),
              sizeof(textures));
  std::memcpy(payload.data() + streamOffset, streams.data(), sizeof(streams));
  std::memcpy(payload.data() + shaderOffset, &shader, sizeof(shader));
  return payload;
}

std::vector<std::byte> bootstrapChunk() {
  auto payload = drawPayload(true);
  auto* shaderSection = reinterpret_cast<D9CCommandChunkWireSectionDesc*>(
      payload.data() + sizeof(D9CCommandChunkWireDrawHeader) +
      2u * sizeof(D9CCommandChunkWireSectionDesc));
  auto* draw =
      reinterpret_cast<D9CCommandChunkWireDrawHeader*>(payload.data());
  draw->primitiveType = 0u;
  draw->primitiveCount = 0u;
  auto* shader = reinterpret_cast<D9CCommandChunkWireShaderBinding*>(
      payload.data() + shaderSection->payloadOffset);
  shader->valid = 0u;
  shader->handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
  auto* texture = reinterpret_cast<D9CCommandChunkWireTextureBinding*>(
      payload.data() + reinterpret_cast<D9CCommandChunkWireDrawHeader*>(
                           payload.data())
                           ->sectionPayloadOffset);
  texture->handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
  return makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::move(payload),
  }});
}

std::vector<std::byte> frameChunk(bool firstFullSnapshot = true) {
  const D9CCommandChunkWireClear clear{
      .flags = 1u,
      .colorARGB = 0xff204060u,
      .z = 1.0f,
      .rectCount = 0u,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  const std::vector handles{
      D9CCommandChunkWireHandleEntry{
          .kind = kTexture.kind,
          .generation = kTexture.generation,
          .objectId = kTexture.objectId,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = kShader.kind,
          .generation = kShader.generation,
          .objectId = kShader.objectId,
      },
  };
  const std::array records{
      Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)},
      Record{.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
             .payload = drawPayload(firstFullSnapshot),
             .handles = handles},
      Record{.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
             .payload = drawPayload(false, 2u),
             .handles = handles},
      Record{.type = D9C_COMMAND_RECORD_PRESENT,
             .payload = bytesOf(D9CCommandChunkWirePresent{})},
  };
  return makeChunk(records);
}

RenderTapeBlobCatalogue catalogue() {
  return RenderTapeBlobCatalogue{.blobs = {
      RenderTapeBlob{.digest = kShaderDigest, .size = 4u, .verified = 1u},
      RenderTapeBlob{.digest = kTextureDigest, .size = 4u, .verified = 1u},
  }};
}

std::vector<std::byte> makeFrameTape(bool firstFullSnapshot = true,
                                     bool includeTextureDefinition = true,
                                     std::uint64_t expectedTextureBytes = 4u,
                                     bool lateTextureSeed = false) {
  constexpr std::array<std::byte, 8u> descriptor{};
  constexpr std::array<std::byte, 4u> shaderDescriptor{};
  const auto frame = frameChunk(firstFullSnapshot);
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind =
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };
  const RenderTapeFlushWaitControl wait{.waitedSeqId = 8u};
  const RenderTapeOrderedControlHeader control{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::FlushWait),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = sizeof(wait),
      .completionOrdinal = 8u,
  };

  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrapChunk());
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      descriptor, 0u, {});
  if (includeTextureDefinition && !lateTextureSeed) {
    builder.appendObjectDefine(
        kTexture,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        descriptor, 0u, {}, expectedTextureBytes, 1u);
  }
  builder.appendObjectDefine(
      kShader, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
      shaderDescriptor, 4u, kShaderDigest);
  if (lateTextureSeed) {
    // An unrelated command must not close another identity's seed
    // expectation. The texture is defined and seeded immediately before its
    // first selected draw reference.
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
        bootstrapChunk());
    builder.appendObjectDefine(
        kTexture,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        descriptor, 0u, {}, expectedTextureBytes, 1u);
  }
  builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, kTextureDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 4u, .handleCount = 4u}, frame);
  builder.appendObjectDestroy(kTexture);
  builder.appendOrderedControl(control, std::as_bytes(std::span(&wait, 1u)));
  builder.appendPresentComplete(
      builder.eventCount() - 2u, 9u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  return builder.seal();
}

std::vector<std::byte> makeSequenceTape() {
  constexpr std::array<std::byte, 8u> descriptor{};
  constexpr std::array<std::byte, 4u> shaderDescriptor{};
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind =
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };
  const auto frame = frameChunk();
  RenderTapeBuilder builder(kRenderTapeProfileSequence);
  builder.appendBootstrapState(bootstrapChunk());
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      descriptor, 0u, {});
  builder.appendObjectDefine(
      kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      descriptor, 0u, {}, 4u, 1u);
  builder.appendObjectDefine(
      kShader, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
      shaderDescriptor, 4u, kShaderDigest);
  builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, kTextureDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 4u, .handleCount = 4u}, frame);
  builder.appendPresentComplete(
      6u, 7u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, kTextureDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 4u, .handleCount = 4u}, frame);
  builder.appendPresentComplete(
      9u, 10u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  return builder.seal();
}

RenderTapeProjectionSelector selector() {
  return RenderTapeProjectionSelector{
      .commandEventOrdinal = 6u,
      .firstRecordIndex = 1u,
      .recordCount = 2u,
  };
}

void acceptedRangeConservesCanonicalIdentity() {
  const auto tape = makeFrameTape();
  const auto blobs = catalogue();
  check(validateRenderTape(tape, blobs).valid(),
        "projection fixture source must validate");
  const auto projected = projectRenderTapeDrawSlice(tape, blobs, selector());
  check(projected.valid(), renderTapeProjectionStatusName(projected.status));
  check(projected.selectedLocators.size() == 2u &&
            projected.selectedLocators[0].eventOrdinal == 6u &&
            projected.selectedLocators[0].recordIndex == 1u &&
            projected.selectedLocators[1].eventOrdinal == 6u &&
            projected.selectedLocators[1].recordIndex == 2u,
        "draw locators must preserve canonical event ordinal and record order");
  check(projected.clearLocator.eventOrdinal == 6u &&
            projected.clearLocator.recordIndex == 0u &&
            projected.presentLocator.eventOrdinal == 6u &&
            projected.presentLocator.recordIndex == 3u,
        "same-command Clear and immediately-following Present stay outside range");
  check(projected.objects.size() == 2u &&
            projected.objects[0].identity.objectId == kTexture.objectId &&
            projected.objects[0].identity.generation == kTexture.generation &&
            projected.objects[1].identity.objectId == kShader.objectId &&
            projected.objects[1].identity.generation == kShader.generation,
        "projection must conserve exact generation-qualified selected objects");
  check(projected.blobReferences.size() == 2u &&
            projected.blobReferences[0].digest == kShaderDigest &&
            projected.blobReferences[1].digest == kTextureDigest &&
            projected.blobReferences[1].initialContent == 1u,
        "projection must conserve source-ordered immutable and seed digests");
  check(projected.objects[0].initialContentBytes == 4u &&
            projected.objects[0].initialContentCount == 1u &&
            projected.sourceRecordCount == 4u &&
            projected.selectedDrawCount == 2u &&
            projected.excludedRecordCount == 2u,
        "projection must conserve initial content and record counts");
  check(projected.excludedEvents.size() == 3u &&
            projected.excludedEvents[0].kind ==
                RenderTapeProjectionExcludedKind::ObjectDestroy &&
            projected.excludedEvents[1].kind ==
                RenderTapeProjectionExcludedKind::OrderedControl &&
            projected.excludedEvents[2].kind ==
                RenderTapeProjectionExcludedKind::PresentComplete,
        "destroy, control, and completion stay coordinator-owned and excluded");
}

void projectionAccountsLatePerIdentitySeed() {
  const auto tape = makeFrameTape(true, true, 4u, true);
  const auto blobs = catalogue();
  check(validateRenderTape(tape, blobs).valid(),
        "late-seed projection fixture source must validate");
  auto lateSelector = selector();
  lateSelector.commandEventOrdinal = 7u;
  const auto projected =
      projectRenderTapeDrawSlice(tape, blobs, lateSelector);
  check(projected.valid(), renderTapeProjectionStatusName(projected.status));
  const auto texture = std::find_if(
      projected.objects.begin(), projected.objects.end(), [](const auto& object) {
        return object.identity.objectId == kTexture.objectId;
      });
  check(projected.blobReferences.size() == 2u &&
            projected.blobReferences[1].identity.objectId == kTexture.objectId &&
            projected.blobReferences[1].initialContent == 1u &&
            texture != projected.objects.end() &&
            texture->initialContentBytes == 4u &&
            texture->initialContentCount == 1u,
        "projection must classify a late matching mutation as initial content");
}

void failClosedSelectionAndClosureCases() {
  const auto blobs = catalogue();
  check(projectRenderTapeDrawSlice(makeSequenceTape(), blobs, selector()).status ==
            RenderTapeProjectionStatus::UnsupportedProfile,
        "sequence tape must not enter the frame projection lane");

  auto nonDraw = selector();
  nonDraw.firstRecordIndex = 0u;
  check(projectRenderTapeDrawSlice(makeFrameTape(), blobs, nonDraw).status ==
            RenderTapeProjectionStatus::NonDrawRecord,
        "Clear in selected record range must fail closed");
  auto coordinator = selector();
  coordinator.commandEventOrdinal = 8u;
  coordinator.firstRecordIndex = 0u;
  coordinator.recordCount = 1u;
  check(projectRenderTapeDrawSlice(makeFrameTape(), blobs, coordinator).status ==
            RenderTapeProjectionStatus::InvalidSelection,
        "coordinator event cannot masquerade as a command record range");
  check(projectRenderTapeDrawSlice(makeFrameTape(false), blobs, selector()).status ==
            RenderTapeProjectionStatus::MissingFullSnapshot,
        "first draw without FULL_SNAPSHOT must fail closed");
  auto outOfRange = selector();
  outOfRange.firstRecordIndex = 3u;
  outOfRange.recordCount = 2u;
  check(projectRenderTapeDrawSlice(makeFrameTape(), blobs, outOfRange).status ==
            RenderTapeProjectionStatus::InvalidSelection,
        "out-of-range record selector must fail closed");

  check(projectRenderTapeDrawSlice(makeFrameTape(true, false), blobs,
                                   selector())
                .status == RenderTapeProjectionStatus::InvalidSource,
        "missing exact definition must fail during source validation");
  auto wrongGeneration = makeFrameTape();
  auto* header = reinterpret_cast<RenderTapeHeader*>(wrongGeneration.data());
  auto* events = reinterpret_cast<RenderTapeEventHeader*>(
      wrongGeneration.data() + header->eventTableOffset);
  auto* command = reinterpret_cast<RenderTapeCommandChunkHeader*>(
      wrongGeneration.data() + header->payloadArenaOffset +
      events[5u].payloadOffset);
  auto* chunk = reinterpret_cast<D9CCommandChunkWireHeader*>(command + 1u);
  auto* handles = reinterpret_cast<D9CCommandChunkWireHandleEntry*>(
      reinterpret_cast<std::byte*>(chunk) + chunk->handleTableOffset);
  handles[0].generation += 1u;
  check(projectRenderTapeDrawSlice(wrongGeneration, blobs, selector()).status ==
            RenderTapeProjectionStatus::InvalidSource,
        "wrong-generation selected identity must fail before projection");
  check(projectRenderTapeDrawSlice(makeFrameTape(true, true, 8u), blobs,
                                   selector())
                .status == RenderTapeProjectionStatus::InvalidSource,
        "incomplete initial seed must fail before projection");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
      const auto tape = makeFrameTape();
      std::ofstream output(argv[2], std::ios::binary);
      output.write(reinterpret_cast<const char*>(tape.data()),
                   static_cast<std::streamsize>(tape.size()));
      check(output.good(), "failed to write projection CLI fixture");
      return 0;
    }
    check(argc == 1,
          "usage: render_tape_projection_spec [--write-fixture path]");
    acceptedRangeConservesCanonicalIdentity();
    projectionAccountsLatePerIdentitySeed();
    failClosedSelectionAndClosureCases();
  } catch (const std::exception& error) {
    std::cerr << "render_tape_projection_spec failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render_tape_projection_spec passed\n";
  return 0;
}
