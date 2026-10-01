#pragma once

#include "render_tape_cpu_destroy_fixture.hpp"
#include "device_c_render_tape_capture_layout.hpp"

namespace dxmt9::d3d9::test {

enum class CpuVolumeFixtureFault {
  None,
  WrongMipDepth,
  WrongResourceType,
  MissingSliceBytes,
};

struct GeneralCpuVolumeFixture : GeneralCpuDestroyFixture {
  D9CWireObjectIdentity texture{};
  std::array<D9CSurfaceDesc, 3u> levels{};
  std::array<std::vector<std::byte>, 3u> seeds{};
  std::vector<std::byte> changedLevel0{};
};

namespace general_cpu_volume_fixture_detail {

inline constexpr D9CWireObjectIdentity kTexture{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 8u, .objectId = 73u};

inline std::vector<std::byte> texturedDrawChunk() {
  using namespace general_cpu_destroy_fixture_detail;
  const auto source = indexedDrawChunk();
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(source,
          CommandChunkEnvelope{.recordCount = 1u, .handleCount = 5u}, imported))
    return {};
  const auto view = imported.record(0u);
  Record record{.type = view.header.type,
      .payload = {view.payload.begin(), view.payload.end()},
      .handles = {view.handles.begin(), view.handles.end()}};
  for (const auto& section : view.sections) {
    if (section.kind != D9C_COMMAND_CHUNK_SECTION_TEXTURE || section.count == 0u)
      continue;
    D9CCommandChunkWireTextureBinding binding{};
    std::memcpy(&binding, record.payload.data() + section.payloadOffset,
                sizeof(binding));
    binding.handleIndex = static_cast<std::uint32_t>(record.handles.size());
    std::memcpy(record.payload.data() + section.payloadOffset, &binding,
                sizeof(binding));
  }
  record.handles.push_back({.kind = kTexture.kind,
      .generation = kTexture.generation, .objectId = kTexture.objectId});
  return makeChunk(std::span(&record, 1u));
}

}  // namespace general_cpu_volume_fixture_detail

inline GeneralCpuVolumeFixture makeGeneralCpuVolumeFixture(
    std::uint32_t format = render_tape_d3d_format::A8R8G8B8,
    CpuVolumeFixtureFault fault = CpuVolumeFixtureFault::None,
    bool alternateMutation = false) {
  using namespace general_cpu_volume_fixture_detail;
  const auto base = makeGeneralCpuDestroyFixture();
  GeneralCpuVolumeFixture fixture;
  fixture.blobs = base.blobs;
  fixture.texture = kTexture;
  std::uint64_t expectedBytes = 0u;
  for (std::uint32_t mip = 0u; mip < fixture.levels.size(); ++mip) {
    auto& desc = fixture.levels[mip];
    desc = {.format = format, .resourceType = 4u, .pool = 0u,
            .width = std::max(1u, 7u >> mip),
            .height = std::max(1u, 5u >> mip),
            .depth = std::max(1u, 3u >> mip)};
    RenderTapeVolumeLockLayout layout{};
    if (renderTapeVolumeContentLayout(desc, layout) !=
        RenderTapeVolumeLayoutStatus::Accepted) return {};
    fixture.seeds[mip].resize(static_cast<std::size_t>(layout.fullBytes));
    for (std::size_t index = 0u; index < fixture.seeds[mip].size(); ++index)
      fixture.seeds[mip][index] = static_cast<std::byte>(
          (0x31u + 29u * mip + index) & 0xffu);
    expectedBytes += layout.fullBytes;
  }
  fixture.changedLevel0 = fixture.seeds[0];
  RenderTapeVolumeLockLayout full{};
  if (renderTapeVolumeContentLayout(fixture.levels[0], full) !=
      RenderTapeVolumeLayoutStatus::Accepted) return {};
  const RenderTapeLockBox box{.left = 4, .top = 0, .right = 7, .bottom = 5,
                             .front = 1, .back = 3};
  RenderTapeVolumeLockLayout partial{};
  if (renderTapeVolumeLockLayout(fixture.levels[0],
          static_cast<std::int32_t>(full.fullRowBytes),
          static_cast<std::int64_t>(full.fullSliceBytes), &box, partial) !=
      RenderTapeVolumeLayoutStatus::Accepted) return {};
  std::vector<std::byte> patch(static_cast<std::size_t>(partial.tightBytes),
                               alternateMutation ? std::byte{0xe7} : std::byte{0x96});
  if (applyRenderTapeVolumeMutation(partial, patch, fixture.changedLevel0) !=
      RenderTapeBlockMutationStatus::Accepted) return {};

  auto descriptorLevels = fixture.levels;
  if (fault == CpuVolumeFixtureFault::WrongMipDepth)
    descriptorLevels[1].depth = 2u;
  if (fault == CpuVolumeFixtureFault::WrongResourceType)
    descriptorLevels[0].resourceType = 3u;
  const RenderTapeTextureDescriptorV2 metadata{
      .dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Volume),
      .mipLevelCount = 3u, .subresourceCount = 3u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed)};
  std::vector<std::byte> descriptor(sizeof(metadata) + sizeof(descriptorLevels));
  std::memcpy(descriptor.data(), &metadata, sizeof(metadata));
  std::memcpy(descriptor.data() + sizeof(metadata), descriptorLevels.data(),
              sizeof(descriptorLevels));

  ImportedRenderTapeView source;
  if (!importPrevalidatedRenderTape(base.tape, source)) return {};
  RenderTapeBuilder builder;
  bool sawDraw = false;
  for (std::uint32_t index = 0u; index < source.header.eventCount; ++index) {
    const auto event = source.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::BootstrapState) {
      builder.appendRawEvent(type, event.payload);
      builder.appendObjectDefine(kTexture,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
          descriptor, 0u, {}, expectedBytes, 3u);
      for (std::uint32_t mip = 0u; mip < fixture.seeds.size(); ++mip) {
        auto bytes = fixture.seeds[mip];
        if (fault == CpuVolumeFixtureFault::MissingSliceBytes && mip == 0u)
          bytes.resize(bytes.size() - static_cast<std::size_t>(full.fullSliceBytes));
        const auto digest = RenderTapeCaptureSession::sha256(bytes);
        builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
            mip, 0u, bytes.size(), digest);
        fixture.blobs.push_back(std::move(bytes));
      }
      continue;
    }
    bool draw = false;
    if (type == RenderTapeEventType::CommandChunk) {
      RenderTapeCommandChunkHeader fixed{};
      if (event.payload.size() >= sizeof(fixed)) {
        std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
        ImportedChunkView chunk;
        if (importPrevalidatedCommandChunk(event.payload.subspan(sizeof(fixed)),
                CommandChunkEnvelope{.version = fixed.wireVersion,
                    .recordCount = fixed.recordCount, .handleCount = fixed.handleCount}, chunk))
          for (const auto& record : chunk.records)
            draw |= record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
      }
    }
    if (draw && !sawDraw) {
      sawDraw = true;
      const auto chunk = texturedDrawChunk();
      builder.appendCommandChunk({.recordCount = 1u, .handleCount = 6u}, chunk);
      const auto digest = RenderTapeCaptureSession::sha256(fixture.changedLevel0);
      builder.appendResourceMutation(kTexture, RenderTapeMutationKind::CpuUnlock,
          0u, 0u, fixture.changedLevel0.size(), digest);
      fixture.blobs.push_back(fixture.changedLevel0);
      builder.appendCommandChunk({.recordCount = 1u, .handleCount = 6u}, chunk);
      builder.appendObjectDestroy(kTexture);
    } else {
      builder.appendRawEvent(type, event.payload);
    }
  }
  const auto candidate = builder.seal();
  ImportedRenderTapeView rebuilt;
  if (!importPrevalidatedRenderTape(candidate, rebuilt)) return {};
  RenderTapeBuilder finalized;
  std::uint64_t presentOrdinal = 0u;
  for (std::uint32_t index = 0u; index < rebuilt.header.eventCount; ++index) {
    const auto event = rebuilt.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::CommandChunk) presentOrdinal = event.header.ordinal;
    if (type == RenderTapeEventType::PresentComplete) {
      auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader fixed{};
      std::memcpy(&fixed, payload.data(), sizeof(fixed));
      fixed.presentOrdinal = presentOrdinal;
      std::memcpy(payload.data(), &fixed, sizeof(fixed));
      finalized.appendRawEvent(type, payload);
    } else {
      finalized.appendRawEvent(type, event.payload);
    }
  }
  fixture.tape = finalized.seal();
  return fixture;
}

}  // namespace dxmt9::d3d9::test
