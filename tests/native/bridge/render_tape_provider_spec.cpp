#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_provider.hpp"

// R-HARN-REPLAY-7.6/7.7/7.8: bounded provider identity grammar,
// pre-effect failure, independent evidence, and native Metal output readback.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

struct Record {
  std::uint32_t type = 0u;
  std::vector<std::byte> payload{};
  std::vector<D9CCommandChunkWireHandleEntry> handles{};
};

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

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
    check(rule != nullptr, "fixture record must be canonical");
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
  std::memcpy(bytes.data() + recordTableOffset, records.data(),
              records.size() * sizeof(records[0]));
  if (!handles.empty()) {
    std::memcpy(bytes.data() + handleTableOffset, handles.data(),
                handles.size() * sizeof(handles[0]));
  }
  std::memcpy(bytes.data() + payloadArenaOffset, payload.data(), payload.size());
  return bytes;
}

std::vector<std::byte> bootstrapChunk() {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = {.slot = slot, .valid = 1u,
                      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = {.slot = slot, .valid = 1u,
                     .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  const D9CCommandChunkWireRenderTargetBinding renderTarget{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = 0u,
  };
  constexpr std::uint32_t sectionCount = 3u;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamOffset = sectionPayloadOffset + sizeof(textures);
  const auto renderTargetOffset = streamOffset + sizeof(streams);
  const D9CCommandChunkWireDrawHeader draw{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT,
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
          .kind = D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
          .elementSize = sizeof(renderTarget),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(renderTargetOffset),
          .byteSize = sizeof(renderTarget),
      },
  };
  std::vector<std::byte> payload(renderTargetOffset + sizeof(renderTarget));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(), sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(), sizeof(textures));
  std::memcpy(payload.data() + streamOffset, streams.data(), sizeof(streams));
  std::memcpy(payload.data() + renderTargetOffset, &renderTarget,
              sizeof(renderTarget));
  const std::array records{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::move(payload),
      .handles = {{
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = 7u,
          .objectId = 41u,
      }},
  }};
  return makeChunk(records);
}

constexpr D9CWireObjectIdentity kOutput{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 7u,
    .objectId = 41u,
};
constexpr D9CWireObjectIdentity kSeedBuffer{
    .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
    .generation = 3u,
    .objectId = 42u,
};

struct Fixture {
  std::array<std::byte, 16u> seed{};
  RenderTapeDigest seedDigest{};
  std::vector<std::byte> tape{};

  explicit Fixture(std::uint32_t clearRectCount = 0u) {
    for (std::size_t i = 0u; i < seed.size(); ++i)
      seed[i] = static_cast<std::byte>(i + 1u);
    seedDigest = RenderTapeCaptureSession::sha256(seed);

    D9CCommandChunkWireClear clear{
        .flags = 1u,
        .colorARGB = 0xff204060u,
        .z = 1.0f,
        .rectCount = clearRectCount,
        .rectOffset = sizeof(D9CCommandChunkWireClear),
    };
    auto clearBytes = bytesOf(clear);
    if (clearRectCount) {
      const D9CRect rect{0, 0, 16, 16};
      const auto rectBytes = std::as_bytes(std::span(&rect, 1u));
      clearBytes.insert(clearBytes.end(), rectBytes.begin(), rectBytes.end());
    }
    const std::array frameRecords{
        Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = std::move(clearBytes)},
        Record{.type = D9C_COMMAND_RECORD_PRESENT,
               .payload = bytesOf(D9CCommandChunkWirePresent{})},
    };
    const auto frame = makeChunk(frameRecords);
    const D9CSurfaceDesc outputDesc{
        .format = 21u, // D3DFMT_A8R8G8B8
        .resourceType = 1u,
        .usage = 1u,
        .pool = 0u,
        .multiSampleType = 0u,
        .width = 16u,
        .height = 16u,
        .depth = 1u,
    };
    const D9CBufferDesc bufferDesc{
        .size = static_cast<std::uint32_t>(seed.size()),
        .pool = 0u,
    };
    const RenderTapeOracleAttachment oracle{
        .identity = kOutput,
        .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
    };
    RenderTapeBuilder builder;
    builder.appendBootstrapState(bootstrapChunk());
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&outputDesc, 1u)), 0u, {});
    builder.appendObjectDefine(
        kSeedBuffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
        std::as_bytes(std::span(&bufferDesc, 1u)), 0u, {}, seed.size(), 1u);
    builder.appendResourceMutation(kSeedBuffer, RenderTapeMutationKind::Upload,
                                   0u, 0u, seed.size(), seedDigest);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 2u}, frame);
    builder.appendPresentComplete(
        5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
    tape = builder.seal();
  }

  RenderTapeProviderBlob blob() const {
    return {.digest = seedDigest, .bytes = seed};
  }
};

void acceptsBoundedIdentityGrammarAndReportsEvidence() {
  Fixture fixture;
  const auto blob = fixture.blob();
  const RenderTapeBlobCatalogue catalogue{.blobs = {{
      .digest = blob.digest,
      .size = blob.bytes.size(),
      .verified = 1u,
  }}};
  const auto validation = validateRenderTape(fixture.tape, catalogue);
  check(validation.valid(), renderTapeValidationStatusName(validation.status));
  const auto result = preflightFrameTapeIdentity(fixture.tape,
                                                  std::span(&blob, 1u));
  check(result.complete(), frameTapeReplayStatusName(result.status));
  check(result.validity.structurallyValid && result.validity.digestsValid,
        "validity evidence must be independent and affirmative");
  check(result.coverage.objectDefinitions == 2u &&
            result.coverage.seedMutations == 1u &&
            result.coverage.bootstrapChunks == 1u &&
            result.coverage.commandChunks == 1u &&
            result.coverage.commandRecords == 2u &&
            result.coverage.clearRecords == 1u &&
            result.coverage.presentRecords == 1u &&
            result.coverage.presentOutputs == 1u,
        "coverage counters must describe the admitted grammar exactly");
  check(result.conservation.inputBlobs == 1u &&
            result.conservation.referencedBlobs == 1u &&
            result.conservation.presentOrdinal == 5u &&
            result.conservation.completionOrdinal == 1u,
        "conservation evidence must close blob and ordinal identities");
}

void failsClosedBeforeEffectsOnUnsupportedAndCorruptInputs() {
  Fixture partialClear(1u);
  const auto partialBlob = partialClear.blob();
  check(preflightFrameTapeIdentity(partialClear.tape,
                                   std::span(&partialBlob, 1u)).status ==
            FrameTapeReplayStatus::UnsupportedGrammar,
        "partial Clear must fail the narrow full-surface grammar");

  Fixture wrongDigest;
  auto blob = wrongDigest.blob();
  blob.digest[0] ^= std::byte{1u};
  check(preflightFrameTapeIdentity(wrongDigest.tape, std::span(&blob, 1u)).status ==
            FrameTapeReplayStatus::InvalidBlobCatalogue,
        "blob digest mismatch must fail before tape callbacks or provider effects");

  Fixture stale;
  auto* header = reinterpret_cast<RenderTapeHeader*>(stale.tape.data());
  auto* events = reinterpret_cast<RenderTapeEventHeader*>(
      stale.tape.data() + header->eventTableOffset);
  auto* complete = reinterpret_cast<RenderTapePresentCompleteHeader*>(
      stale.tape.data() + header->payloadArenaOffset + events[5].payloadOffset);
  auto* oracle = reinterpret_cast<RenderTapeOracleAttachment*>(complete + 1u);
  ++oracle->identity.generation;
  const auto staleBlob = stale.blob();
  check(preflightFrameTapeIdentity(stale.tape, std::span(&staleBlob, 1u)).status ==
            FrameTapeReplayStatus::InvalidTape,
        "stale generation must fail structural validation before effects");
}

void nativeMetalOffscreenIdentityReplay() {
  Fixture fixture;
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "native Metal factory must be available");
  const D9CPresentParams params{
      .backBufferWidth = 16u,
      .backBufferHeight = 16u,
      .backBufferFormat = 21u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "native Metal replay device must construct");
  const auto blob = fixture.blob();
  const auto result = replayFrameTapeIdentity(device, fixture.tape,
                                               std::span(&blob, 1u));
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.complete(), frameTapeReplayStatusName(result.status));
  check(result.validity.outputReadback && result.validity.outputBytes != 0u,
        "offscreen PresentOutput must use production readback after completion");
  check(result.conservation.objectsCreated == 2u &&
            result.conservation.objectsReleased == 2u,
        "replay-owned wrappers must be conserved through completion cleanup");
}

} // namespace

int main() {
  try {
    acceptsBoundedIdentityGrammarAndReportsEvidence();
    failsClosedBeforeEffectsOnUnsupportedAndCorruptInputs();
    nativeMetalOffscreenIdentityReplay();
  } catch (const TestFailure& error) {
    std::cerr << "render_tape_provider_spec failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render_tape_provider_spec passed\n";
  return 0;
}
