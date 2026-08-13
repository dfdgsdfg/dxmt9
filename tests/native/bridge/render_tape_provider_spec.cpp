#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_provider.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_presenter.hpp"
#include "dxmt9/dxmt9_resource_pool.hpp"

// R-HARN-REPLAY-7.6/7.7/7.8: bounded provider identity grammar,
// pre-effect failure, independent evidence, and native Metal output readback.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

std::vector<std::byte> bootstrapChunkWithRenderTargets(
    std::span<const D9CCommandChunkWireRenderTargetBinding> renderTargets) {
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
  constexpr std::uint32_t sectionCount = 3u;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamOffset = sectionPayloadOffset + sizeof(textures);
  const auto renderTargetOffset = streamOffset + sizeof(streams);
  const auto renderTargetBytes = renderTargets.size() *
                                 sizeof(D9CCommandChunkWireRenderTargetBinding);
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
          .elementSize = sizeof(D9CCommandChunkWireRenderTargetBinding),
          .count = static_cast<std::uint32_t>(renderTargets.size()),
          .payloadOffset = static_cast<std::uint32_t>(renderTargetOffset),
          .byteSize = static_cast<std::uint32_t>(renderTargetBytes),
      },
  };
  std::vector<std::byte> payload(renderTargetOffset + renderTargetBytes);
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(), sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(), sizeof(textures));
  std::memcpy(payload.data() + streamOffset, streams.data(), sizeof(streams));
  if (!renderTargets.empty()) {
    std::memcpy(payload.data() + renderTargetOffset, renderTargets.data(),
                renderTargetBytes);
  }
  std::vector<D9CCommandChunkWireHandleEntry> handles(renderTargets.size(),
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = 7u,
          .objectId = 41u,
      });
  const std::array records{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::move(payload),
      .handles = std::move(handles),
  }};
  return makeChunk(records);
}

std::vector<std::byte> bootstrapChunk() {
  const std::array renderTargets{D9CCommandChunkWireRenderTargetBinding{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = 0u,
  }};
  return bootstrapChunkWithRenderTargets(renderTargets);
}

std::vector<std::byte> implicitBootstrapChunk() {
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
  constexpr std::uint32_t sectionCount = 2u;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamOffset = sectionPayloadOffset + sizeof(textures);
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
  };
  std::vector<std::byte> payload(streamOffset + sizeof(streams));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(),
              sizeof(textures));
  std::memcpy(payload.data() + streamOffset, streams.data(), sizeof(streams));
  return makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::move(payload),
  }});
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

struct ProductionFixture {
  std::vector<std::byte> tape{};

  explicit ProductionFixture(bool wrongExpectedDigest = false) {
    const D9CCommandChunkWireClear clear{
        .flags = 1u,
        .colorARGB = 0xff204060u,
        .z = 1.0f,
        .rectCount = 0u,
        .rectOffset = sizeof(D9CCommandChunkWireClear),
    };
    const std::array frameRecords{
        Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)},
        Record{.type = D9C_COMMAND_RECORD_PRESENT,
               .payload = bytesOf(D9CCommandChunkWirePresent{})},
    };
    const auto frame = makeChunk(frameRecords);
    const D9CSurfaceDesc outputDesc{
        .format = 21u,
        .resourceType = 1u,
        .usage = 1u,
        .pool = 0u,
        .multiSampleType = 0u,
        .multiSampleQuality = 0u,
        .width = 16u,
        .height = 16u,
        .depth = 1u,
    };
    const RenderTapeOracleAttachment oracle{
        .identity = kOutput,
        .descriptorKind = static_cast<std::uint32_t>(
            RenderTapeDescriptorKind::Surface),
    };
    const RenderTapeDigest expectedDigest{
        std::byte{0x5f}, std::byte{0x73}, std::byte{0x22}, std::byte{0xd0},
        std::byte{0x5f}, std::byte{0x8b}, std::byte{0xa9}, std::byte{0x74},
        std::byte{0x08}, std::byte{0x93}, std::byte{0xa4}, std::byte{0x70},
        std::byte{0x42}, std::byte{0x3e}, std::byte{0x69}, std::byte{0x2f},
        std::byte{0x05}, std::byte{0x7c}, std::byte{0x05}, std::byte{0x4f},
        std::byte{0xd0}, std::byte{0xbd}, std::byte{0x92}, std::byte{0xaa},
        std::byte{0x60}, std::byte{0x6c}, std::byte{0x44}, std::byte{0x40},
        std::byte{0x10}, std::byte{0x9d}, std::byte{0xca}, std::byte{0x61},
    };
    auto expected = expectedDigest;
    if (wrongExpectedDigest) expected[0] ^= std::byte{1u};
    RenderTapeBuilder builder;
    builder.appendBootstrapState(implicitBootstrapChunk());
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&outputDesc, 1u)), 0u, {});
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 2u, .handleCount = 0u}, frame);
    builder.appendPresentComplete(
        3u, 1u, RenderTapeDigestValidity::Sha256, expected,
        std::as_bytes(std::span(&oracle, 1u)));
    tape = builder.seal();
  }
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

std::vector<std::byte> readbackTight(dxmt9::Device& device,
                                     dxmt9::core::SurfaceHandle surface,
                                     std::uint32_t width,
                                     std::uint32_t height) {
  dxmt9::core::ReadbackPixels pixels;
  check(device.readbackSurface(
            dxmt9::core::ReadbackDesc{.source = surface}, pixels),
        "production Presenter oracle readback succeeds");
  constexpr std::uint32_t bytesPerPixel = 4u;
  const auto tightPitch = width * bytesPerPixel;
  check(pixels.pitch >= tightPitch &&
            pixels.bytes.size() >= static_cast<std::size_t>(pixels.pitch) * height,
        "production Presenter oracle readback has a bounded source span");
  std::vector<std::byte> tight(static_cast<std::size_t>(tightPitch) * height);
  for (std::uint32_t row = 0u; row < height; ++row) {
    std::memcpy(tight.data() + static_cast<std::size_t>(row) * tightPitch,
                pixels.bytes.data() + static_cast<std::size_t>(row) * pixels.pitch,
                tightPitch);
  }
  return tight;
}

void productionPresenterMirrorGpuOracle() {
  constexpr std::uint32_t width = 16u;
  constexpr std::uint32_t height = 16u;
  auto devices = WMT::CopyAllDevices();
  if (!devices || devices.count() == 0u) {
    return;
  }
  WMT::Device metalDevice = devices.object(0u);
  auto upper = dxmt9::CreateDXMT9Device(
      dxmt9::DEVICE_DESC{.device = metalDevice});
  check(upper != nullptr, "production Presenter oracle device constructs");
  const dxmt9::core::SurfaceDesc surfaceDesc{
      .width = width,
      .height = height,
      .format = dxmt9::core::Format::A8R8G8B8,
      .pool = dxmt9::core::Pool::Scratch,
      .usage = dxmt9::core::UsageRenderTarget,
      .renderTarget = true,
      .depthStencil = false,
      .multiSampleType = dxmt9::core::MultiSampleType::None,
  };
  const auto source = upper->createSurface(surfaceDesc);
  const auto primary = upper->createSurface(surfaceDesc);
  const auto mirror = upper->createSurface(surfaceDesc);
  check(source && primary && mirror,
        "production Presenter oracle creates source and output targets");
  auto* sourceRecord = upper->pool()->findSurface(source.value);
  auto* primaryRecord = upper->pool()->findSurface(primary.value);
  auto* mirrorRecord = upper->pool()->findSurface(mirror.value);
  check(sourceRecord && primaryRecord && mirrorRecord && sourceRecord->texture &&
            primaryRecord->texture && mirrorRecord->texture,
        "production Presenter oracle resolves Metal textures from the pool");

  std::vector<std::uint8_t> sourceBytes(width * height * 4u);
  for (std::uint32_t row = 0u; row < height; ++row) {
    for (std::uint32_t column = 0u; column < width; ++column) {
      const auto offset = (row * width + column) * 4u;
      sourceBytes[offset + 0u] = static_cast<std::uint8_t>(column * 13u + row);
      sourceBytes[offset + 1u] = static_cast<std::uint8_t>(row * 17u + column);
      sourceBytes[offset + 2u] = static_cast<std::uint8_t>(column ^ (row * 3u));
      sourceBytes[offset + 3u] = static_cast<std::uint8_t>(0x40u + row + column);
    }
  }
  sourceRecord->texture.replaceRegion(
      WMTOrigin{.x = 0u, .y = 0u, .z = 0u},
      WMTSize{.width = width, .height = height, .depth = 1u}, 0u, 0u,
      sourceBytes.data(), width * 4u, sourceBytes.size());

  auto output = std::make_shared<dxmt9::OffscreenPresentOutput>(
      WMT::Texture{primaryRecord->texture.handle}, width, height);
  dxmt9::Presenter presenter(metalDevice, std::move(output), nullptr, nullptr);
  check(presenter.valid(), "production Presenter oracle is valid");
  auto ticket = std::make_shared<dxmt9::PresentMirrorTicket>();
  const dxmt9::PresentOutputTarget mirrorTarget{
      .texture = WMT::Texture{mirrorRecord->texture.handle},
      .width = width,
      .height = height,
  };
  check(presenter.reservePresentMirror(mirrorTarget, ticket),
        "production Presenter reserves one mirror");
  auto competingTicket = std::make_shared<dxmt9::PresentMirrorTicket>();
  check(!presenter.reservePresentMirror(mirrorTarget, competingTicket),
        "production Presenter rejects a second outstanding mirror");

  auto commandBuffer = upper->queue().newCommandBuffer();
  check(commandBuffer, "production Presenter oracle creates a command buffer");
  const dxmt9::Presenter::EncodeParams params{
      .source = WMT::Texture{sourceRecord->texture.handle},
      .width = width,
      .height = height,
      .displaySyncEnabled = false,
      .contentsScale = 1.0,
      .minimumPresentDuration = 0.0,
      .maxDrawableCount = dxmt9::kDefaultMetalDrawableCount,
      .opaqueAlpha = false,
      .seqId = 1u,
  };
  const auto first = presenter.encodeCommands(commandBuffer, params);
  check(first.acquired && first.encoded && ticket->encoded(),
        "production Presenter encodes the primary and one-shot mirror passes");
  commandBuffer.commit();
  commandBuffer.waitUntilCompleted();
  const auto primaryBytes = readbackTight(*upper, primary, width, height);
  const auto mirrorBytes = readbackTight(*upper, mirror, width, height);
  check(primaryBytes == mirrorBytes,
        "production Presenter primary and mirror readback bytes match");
  check(RenderTapeCaptureSession::sha256(primaryBytes) ==
            RenderTapeCaptureSession::sha256(mirrorBytes),
        "production Presenter primary and mirror digests match");

  auto secondTicket = std::make_shared<dxmt9::PresentMirrorTicket>();
  check(presenter.reservePresentMirror(mirrorTarget, secondTicket),
        "production Presenter permits a new mirror after one-shot consume");
  presenter.cancelPresentMirror(secondTicket);
  for (std::size_t i = 0u; i < sourceBytes.size(); ++i) {
    sourceBytes[i] ^= static_cast<std::uint8_t>((i % 29u) + 1u);
  }
  sourceRecord->texture.replaceRegion(
      WMTOrigin{.x = 0u, .y = 0u, .z = 0u},
      WMTSize{.width = width, .height = height, .depth = 1u}, 0u, 0u,
      sourceBytes.data(), width * 4u, sourceBytes.size());
  auto secondCommandBuffer = upper->queue().newCommandBuffer();
  check(secondCommandBuffer, "production Presenter oracle creates its second command buffer");
  const auto second = presenter.encodeCommands(secondCommandBuffer, params);
  check(second.acquired && second.encoded && !secondTicket->encoded(),
        "cancelled mirror does not leak into a later Present");
  secondCommandBuffer.commit();
  secondCommandBuffer.waitUntilCompleted();
  const auto primaryBytesAfterCancel = readbackTight(*upper, primary, width, height);
  const auto mirrorBytesAfterCancel = readbackTight(*upper, mirror, width, height);
  check(primaryBytesAfterCancel != primaryBytes &&
            mirrorBytesAfterCancel == mirrorBytes,
        "cancelled mirror remains untouched while the later primary changes");
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

void productionShapeUsesImplicitDefaultOutputAndExactDigest() {
  ProductionFixture fixture;
  check(fixture.tape.size() != 0u, "production fixture must seal");
  const auto validation = preflightFrameTapeIdentity(fixture.tape, {});
  check(validation.complete(), frameTapeReplayStatusName(validation.status));
  check(validation.coverage.eventCount == 4u &&
            validation.coverage.objectDefinitions == 1u &&
            validation.coverage.seedMutations == 0u &&
            validation.coverage.commandChunks == 1u &&
            validation.coverage.commandRecords == 2u,
        "production fixture must match the four-event implicit-output capture");
  check(validation.requirements.outputWidth == 16u &&
            validation.requirements.outputHeight == 16u &&
            validation.requirements.outputFormat == 21u,
        "preflight must expose the admitted output requirements");
  check(validation.validity.expectedDigestCaptured,
        "production fixture must carry an output digest oracle");
  check(classifyFrameTapeBootstrapOutput(
            implicitBootstrapChunk(), CommandChunkEnvelope{.recordCount = 1u},
            kOutput) == FrameTapeBootstrapOutputDisposition::ImplicitDefault,
        "production bootstrap must classify as implicit default RT0");
  const auto explicitBootstrap = bootstrapChunk();
  check(classifyFrameTapeBootstrapOutput(
            explicitBootstrap, CommandChunkEnvelope{.recordCount = 1u,
                                                     .handleCount = 1u},
            kOutput) == FrameTapeBootstrapOutputDisposition::ExplicitExact,
        "explicit exact RT0 must remain accepted");
  check(classifyFrameTapeBootstrapOutput(
            explicitBootstrap, CommandChunkEnvelope{.recordCount = 1u,
                                                     .handleCount = 1u},
            D9CWireObjectIdentity{.kind = kOutput.kind,
                                  .generation = kOutput.generation + 1u,
                                  .objectId = kOutput.objectId}) ==
            FrameTapeBootstrapOutputDisposition::WrongIdentity,
        "wrong-generation RT0 must fail closed");
  auto explicitNull = explicitBootstrap;
  ImportedChunkView explicitView;
  check(importPrevalidatedCommandChunk(
            explicitNull, CommandChunkEnvelope{.recordCount = 1u,
                                                .handleCount = 1u},
            explicitView),
        "explicit bootstrap must import for negative classification");
  const auto renderTarget = explicitView.record(0u).section(2u);
  D9CCommandChunkWireRenderTargetBinding nullBinding{};
  std::memcpy(&nullBinding, renderTarget.payload.data(), sizeof(nullBinding));
  nullBinding.valid = 0u;
  nullBinding.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
  std::memcpy(const_cast<std::byte*>(renderTarget.payload.data()), &nullBinding,
              sizeof(nullBinding));
  check(classifyFrameTapeBootstrapOutput(
            explicitNull, CommandChunkEnvelope{.recordCount = 1u,
                                                .handleCount = 1u},
            kOutput) == FrameTapeBootstrapOutputDisposition::ExplicitNull,
        "explicit null RT0 must fail closed");
  const std::array ambiguousTargets{
      D9CCommandChunkWireRenderTargetBinding{
          .slot = 0u,
          .valid = 1u,
          .handleIndex = 0u,
      },
      D9CCommandChunkWireRenderTargetBinding{
          .slot = 0u,
          .valid = 1u,
          .handleIndex = 1u,
      },
  };
  check(classifyFrameTapeBootstrapOutput(
            bootstrapChunkWithRenderTargets(ambiguousTargets),
            CommandChunkEnvelope{.recordCount = 1u, .handleCount = 2u},
            kOutput) == FrameTapeBootstrapOutputDisposition::Ambiguous,
        "duplicate RT0 bindings must fail closed as ambiguous");
  auto outOfRange = explicitBootstrap;
  ImportedChunkView outOfRangeView;
  check(importPrevalidatedCommandChunk(
            outOfRange, CommandChunkEnvelope{.recordCount = 1u,
                                               .handleCount = 1u},
            outOfRangeView),
        "explicit bootstrap must import for slot-range classification");
  const auto outOfRangeRenderTarget = outOfRangeView.record(0u).section(2u);
  D9CCommandChunkWireRenderTargetBinding outOfRangeBinding{};
  std::memcpy(&outOfRangeBinding, outOfRangeRenderTarget.payload.data(),
              sizeof(outOfRangeBinding));
  outOfRangeBinding.slot = 1u;
  std::memcpy(const_cast<std::byte*>(outOfRangeRenderTarget.payload.data()),
              &outOfRangeBinding, sizeof(outOfRangeBinding));
  check(classifyFrameTapeBootstrapOutput(
            outOfRange, CommandChunkEnvelope{.recordCount = 1u,
                                               .handleCount = 1u},
            kOutput) == FrameTapeBootstrapOutputDisposition::SlotOutOfRange,
        "non-zero RT slot must fail closed");
  check(renderTapeTextureSeedExtentMatches(64u, 16, 4u) &&
            !renderTapeTextureSeedExtentMatches(63u, 16, 4u) &&
            !renderTapeTextureSeedExtentMatches(64u, 0, 4u),
        "texture seed extent predicate must be exact and checked");

  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "production fixture factory must be available");
  const D9CPresentParams params{
      .backBufferWidth = validation.requirements.outputWidth,
      .backBufferHeight = validation.requirements.outputHeight,
      .backBufferFormat = validation.requirements.outputFormat,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "production fixture device must construct");
  const auto result = replayFrameTapeIdentity(device, fixture.tape, {});
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.validity.outputReadback && result.validity.expectedDigestCaptured &&
            result.validity.expectedDigestMatched,
        "production fixture must read back and compare its digest");
  check(result.complete(), frameTapeReplayStatusName(result.status));
}

void productionShapeReportsWrongExpectedDigest() {
  ProductionFixture fixture(true);
  const auto validation = preflightFrameTapeIdentity(fixture.tape, {});
  check(validation.complete(), frameTapeReplayStatusName(validation.status));
  const D9CPresentParams params{
      .backBufferWidth = validation.requirements.outputWidth,
      .backBufferHeight = validation.requirements.outputHeight,
      .backBufferFormat = validation.requirements.outputFormat,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "wrong-digest fixture factory must be available");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "wrong-digest fixture device must construct");
  const auto result = replayFrameTapeIdentity(device, fixture.tape, {});
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.status == FrameTapeReplayStatus::OutputMismatch,
        "wrong expected digest must report output mismatch");
  check(result.validity.outputReadback && result.validity.expectedDigestCaptured &&
            !result.validity.expectedDigestMatched,
        "output mismatch must retain readback and digest comparison evidence");
  check(result.conservation.objectsCreated == 1u &&
            result.conservation.objectsReleased == 1u,
        "output mismatch must still clean up replay-owned objects");
}

void writeProductionFixture(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  check(!error, "provider fixture directory must be created");
  const ProductionFixture fixture;
  std::ofstream output(directory / "events.bin", std::ios::binary);
  check(output.good(), "provider fixture must open events.bin");
  output.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  check(output.good(), "provider fixture must write events.bin");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--write-production-fixture") {
      writeProductionFixture(argv[2]);
      return 0;
    }
    check(argc == 1,
          "usage: render_tape_provider_spec [--write-production-fixture dir]");
    acceptsBoundedIdentityGrammarAndReportsEvidence();
    failsClosedBeforeEffectsOnUnsupportedAndCorruptInputs();
    productionPresenterMirrorGpuOracle();
    nativeMetalOffscreenIdentityReplay();
    productionShapeUsesImplicitDefaultOutputAndExactDigest();
    productionShapeReportsWrongExpectedDigest();
  } catch (const TestFailure& error) {
    std::cerr << "render_tape_provider_spec failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render_tape_provider_spec passed\n";
  return 0;
}
