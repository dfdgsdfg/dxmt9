#pragma once

#include "render_tape_cpu_query_fixture.hpp"

namespace dxmt9::d3d9::test {

enum class CpuD24S8FixtureFault {
  None,
  ShortSeed,
  ExtraSeed,
  PackedFourByteSeed,
  WrongFormat,
  Multisampled,
  MissingSeed,
};

enum class CpuD24S8FixtureVariant { Original, DepthOnly, StencilOnly };

namespace cpu_d24s8_fixture_detail {

inline constexpr D9CWireObjectIdentity kDepth{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 9u, .objectId = 79u};

inline std::vector<std::byte> bindDepth(std::span<const std::byte> bytes,
                                      CommandChunkEnvelope envelope) {
  using namespace general_cpu_destroy_fixture_detail;
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(bytes, envelope, imported)) return {};
  std::vector<Record> records;
  for (std::size_t index = 0u; index < imported.records.size(); ++index) {
    const auto view = imported.record(index);
    Record record{.type = view.header.type,
        .payload = {view.payload.begin(), view.payload.end()},
        .handles = {view.handles.begin(), view.handles.end()}};
    if (view.sparseState()) {
      auto draw = view.drawHeader;
      const auto count = draw.sectionCount + 1u;
      const auto payloadOffset = general_cpu_destroy_fixture_detail::alignUp(
          draw.sectionTableOffset + count * sizeof(D9CCommandChunkWireSectionDesc),
          alignof(std::uint32_t));
      const auto delta = payloadOffset - draw.sectionPayloadOffset;
      record.payload.insert(record.payload.begin() + draw.sectionPayloadOffset,
                            delta, std::byte{});
      std::vector<D9CCommandChunkWireSectionDesc> sections(view.sections.begin(),
                                                         view.sections.end());
      for (auto& section : sections) section.payloadOffset += delta;
      const D9CCommandChunkWireDepthStencilBinding binding{
          .valid = 1u, .handleIndex = static_cast<std::uint32_t>(record.handles.size())};
      const auto depthOffset = general_cpu_destroy_fixture_detail::alignUp(
          record.payload.size(), alignof(std::uint32_t));
      record.payload.resize(depthOffset + sizeof(binding));
      std::memcpy(record.payload.data() + depthOffset, &binding, sizeof(binding));
      sections.push_back({.kind = D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
          .elementSize = sizeof(binding), .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(depthOffset),
          .byteSize = sizeof(binding)});
      draw.sectionCount = count;
      draw.sectionPayloadOffset = static_cast<std::uint32_t>(payloadOffset);
      std::memcpy(record.payload.data(), &draw, sizeof(draw));
      std::memcpy(record.payload.data() + draw.sectionTableOffset, sections.data(),
                  sections.size() * sizeof(sections[0]));
      record.handles.push_back({.kind = kDepth.kind,
          .generation = kDepth.generation, .objectId = kDepth.objectId});
    }
    records.push_back(std::move(record));
  }
  return makeChunk(records);
}

}  // namespace cpu_d24s8_fixture_detail

inline GeneralCpuDestroyFixture makeGeneralCpuD24S8Fixture(
    CpuD24S8FixtureFault fault = CpuD24S8FixtureFault::None,
    CpuD24S8FixtureVariant variant = CpuD24S8FixtureVariant::Original) {
  using namespace cpu_d24s8_fixture_detail;
  const auto base = makeGeneralCpuQueryFixture(CpuQueryFixtureFault::None, true);
  GeneralCpuDestroyFixture fixture;
  fixture.blobs = base.blobs;
  RenderTapeSurfaceDescriptorV2 descriptor{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::Standalone),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteDepthStencilFloat32UInt8V1),
      .surface = {.format = 75u, .resourceType = 1u, .usage = 2u, .pool = 0u,
                  .width = 16u, .height = 16u, .depth = 1u}};
  RenderTapeD24S8SnapshotLayout layout{};
  if (!renderTapeDeriveD24S8SnapshotLayout(descriptor.surface, layout)) return {};
  std::vector<std::byte> seed(static_cast<std::size_t>(layout.byteCount));
  for (std::uint32_t pixel = 0u; pixel < 256u; ++pixel) {
    const std::uint32_t depthBits = pixel == 0u && variant == CpuD24S8FixtureVariant::DepthOnly
        ? 0x3f400000u : 0x3f000000u;
    for (std::uint32_t byte = 0u; byte < 4u; ++byte)
      seed[pixel * 4u + byte] = static_cast<std::byte>((depthBits >> (byte * 8u)) & 0xffu);
    seed[layout.stencilOffset + pixel] = static_cast<std::byte>(pixel & 0xffu);
  }
  if (variant == CpuD24S8FixtureVariant::StencilOnly)
    seed[layout.stencilOffset] ^= std::byte{0x5a};
  if (fault == CpuD24S8FixtureFault::ShortSeed) seed.pop_back();
  if (fault == CpuD24S8FixtureFault::ExtraSeed) seed.push_back(std::byte{0x7f});
  if (fault == CpuD24S8FixtureFault::PackedFourByteSeed) seed.resize(256u * 4u);
  if (fault == CpuD24S8FixtureFault::WrongFormat) descriptor.surface.format = 77u;
  if (fault == CpuD24S8FixtureFault::Multisampled) descriptor.surface.multiSampleType = 4u;
  const auto digest = RenderTapeCaptureSession::sha256(seed);
  fixture.blobs.push_back(seed);
  ImportedRenderTapeView source;
  if (!importPrevalidatedRenderTape(base.tape, source)) return {};
  RenderTapeBuilder builder;
  std::uint64_t presentOrdinal = 0u;
  for (std::uint32_t index = 0u; index < source.header.eventCount; ++index) {
    const auto event = source.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::BootstrapState) {
      RenderTapeBootstrapHeader fixed{};
      D9CCommandChunkWireHeader chunk{};
      std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
      std::memcpy(&chunk, event.payload.data() + sizeof(fixed), sizeof(chunk));
      const auto bytes = event.payload.subspan(sizeof(fixed),
          chunk.payloadArenaOffset + chunk.payloadArenaSize);
      builder.appendBootstrapState(bindDepth(bytes, {
          .recordCount = chunk.recordCount, .handleCount = chunk.handleCount}));
      builder.appendObjectDefine(kDepth,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
          std::as_bytes(std::span(&descriptor, 1u)), 0u, {}, layout.byteCount, 1u);
      if (fault != CpuD24S8FixtureFault::MissingSeed)
        builder.appendResourceMutation(kDepth,
            RenderTapeMutationKind::Upload,
            0u, 0u, seed.size(), digest);
    } else if (type == RenderTapeEventType::CommandChunk) {
      RenderTapeCommandChunkHeader fixed{};
      std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
      const auto bytes = bindDepth(event.payload.subspan(sizeof(fixed)), {
          .version = fixed.wireVersion, .recordCount = fixed.recordCount,
          .handleCount = fixed.handleCount});
      D9CCommandChunkWireHeader header{};
      std::memcpy(&header, bytes.data(), sizeof(header));
      builder.appendCommandChunk({.recordCount = header.recordCount,
                                  .handleCount = header.handleCount}, bytes);
      presentOrdinal = builder.eventCount();
    } else if (type == RenderTapeEventType::PresentComplete) {
      builder.appendObjectDestroy(kDepth);
      auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader fixed{};
      std::memcpy(&fixed, payload.data(), sizeof(fixed));
      fixed.presentOrdinal = presentOrdinal;
      std::memcpy(payload.data(), &fixed, sizeof(fixed));
      builder.appendRawEvent(type, payload);
    } else {
      builder.appendRawEvent(type, event.payload);
    }
  }
  fixture.tape = builder.seal();
  return fixture;
}

}  // namespace dxmt9::d3d9::test
