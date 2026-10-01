#pragma once

#include "render_tape_cpu_destroy_fixture.hpp"
#include "d3d9_pe_tape_bootstrap.hpp"

namespace dxmt9::d3d9::test {

// The production cold PE emitter produces the checkpoint bytes. This fixture
// replaces only the bootstrap and clears the existing draw's debug-snapshot
// flag so later scalar pages remain live through an ordinary delta draw.
inline GeneralCpuDestroyFixture makeGeneralCpuBootstrapPagesFixture(
    bool alternateTail = false) {
  const auto base = makeGeneralCpuDestroyFixture(
      true, true, GeneralCpuMutationFixture::None, GeneralCpuResetFixture::A, 1u);
  GeneralCpuDestroyFixture fixture;
  fixture.blobs = base.blobs;

  PeHotStateShadow shadow{};
  auto transition = shadow.transition();
  transition.setViewport(D9CViewport{0u, 0u, 16u, 16u, 0.0f, 1.0f});
  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
    D9CLight light{};
    light.type = 3u;
    light.direction[2] = 1.0f;
    transition.setLight(slot, light);
  }
  for (std::uint32_t ordinal = 0u; ordinal < 83u; ++ordinal) {
    const auto type = 1u + ordinal % 13u;
    std::uint32_t value = 1u;
    if (type == 4u) value = 0xff120000u + ordinal;
    if (type == 8u) value = 0u;
    if (alternateTail && ordinal == 82u) value = 2u;
    transition.setSamplerState(SamplerIndex::fromRaw(ordinal / 13u),
                               SamplerStateType::fromRaw(type), value);
  }
  PeConstShadowBlock constants{};
  pe::CommandChunkBuilder checkpoint({
      .records = pe::kRenderTapeBootstrapMaxRecords, .handles = 256u,
      .payloadBytes = 1024u * 1024u, .sealedBytes = 1024u * 1024u});
  if (pe::appendRenderTapeBootstrapState(checkpoint, shadow, constants,
                                        pe::PeBindingView{}) !=
      pe::RenderTapeBootstrapBuildStatus::Valid) return {};
  const auto sealed = checkpoint.seal();
  if (!sealed.valid() || sealed.recordCount != 3u) return {};

  ImportedRenderTapeView source;
  if (!importPrevalidatedRenderTape(base.tape, source)) return {};
  RenderTapeBuilder builder;
  for (std::uint32_t index = 0u; index < source.header.eventCount; ++index) {
    const auto event = source.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::BootstrapState) {
      builder.appendBootstrapState(sealed.blob);
    } else if (type == RenderTapeEventType::CommandChunk) {
      RenderTapeCommandChunkHeader fixed{};
      std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
      auto bytes = std::vector<std::byte>(event.payload.begin() + sizeof(fixed),
                                           event.payload.end());
      ImportedChunkView chunk;
      if (!importPrevalidatedCommandChunk(bytes,
              CommandChunkEnvelope{.version = fixed.wireVersion,
                  .recordCount = fixed.recordCount, .handleCount = fixed.handleCount},
              chunk)) return {};
      for (std::size_t record = 0u; record < chunk.records.size(); ++record) {
        if (!chunk.record(record).sparseState()) continue;
        D9CCommandChunkWireDrawHeader draw{};
        auto* payload = bytes.data() + chunk.header.payloadArenaOffset +
                        chunk.records[record].payloadOffset;
        std::memcpy(&draw, payload, sizeof(draw));
        draw.flags = 0u;
        std::memcpy(payload, &draw, sizeof(draw));
      }
      builder.appendCommandChunk(
          CommandChunkEnvelope{.version = fixed.wireVersion,
              .recordCount = fixed.recordCount, .handleCount = fixed.handleCount},
          bytes);
    } else {
      builder.appendRawEvent(type, event.payload);
    }
  }
  fixture.tape = builder.seal();
  return fixture;
}

}  // namespace dxmt9::d3d9::test
