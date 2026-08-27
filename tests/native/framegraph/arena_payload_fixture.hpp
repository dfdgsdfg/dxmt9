#pragma once

#include "../../../src/dxmt9/dxmt9_source_payload.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::tests::framegraph {

class ArenaPayloadFixture {
 public:
  explicit ArenaPayloadFixture(const core::ChunkSlot& slot) {
    core::SourcePayloadCapacity capacity{};
    capacity.commandHeaders = slot.commandHeaders.size();
    capacity.drawHotStates = slot.drawHotStates.size();
    capacity.drawShaderLayouts = slot.drawShaderLayouts.size();
    capacity.drawDebugSnapshots = slot.drawDebugSnapshots.size();
    capacity.drawPsoSubviews = slot.drawRunRecords.size();
    capacity.drawUniformFixedPayloads = slot.drawUniformFixedPayloads.size();
    capacity.drawUniformVertexConstants =
        slot.drawUniformVertexConstants.size();
    capacity.drawUniformVertexConstantBytes =
        slot.drawUniformVertexConstantBytes.size();
    capacity.drawUniformPixelConstants =
        slot.drawUniformPixelConstants.size();
    capacity.drawUniformPixelConstantBytes =
        slot.drawUniformPixelConstantBytes.size();
    capacity.drawUniformPayloads = slot.drawUniformPayloads.size();
    capacity.drawParams = slot.drawParams.size();
    capacity.drawPayloadBytes = slot.drawPayloadArena.size();
    capacity.drawRunRecords = slot.drawRunRecords.size();
    capacity.clearRecords = slot.clearRecords.size();
    for (const core::ClearDesc& clear : slot.clearRecords) {
      capacity.clearRects += clear.rects.size();
    }
    capacity.surfaceCopyRecords = slot.surfaceCopyRecords.size();
    capacity.stretchRectRecords = slot.stretchRectRecords.size();
    capacity.readbackRecords = slot.readbackRecords.size();
    capacity.colorFillRecords = slot.colorFillRecords.size();
    capacity.depthResolveRecords = slot.depthResolveRecords.size();
    capacity.presentRecords = slot.presentRecords.size();

    const auto layout =
        core::makeSourcePayloadLayout(capacity, 4096, 64);
    if (!layout.has_value()) {
      return;
    }

    const std::size_t words =
        (layout->usedBytes + sizeof(std::max_align_t) - 1u) /
        sizeof(std::max_align_t);
    backing_.resize(words);
    auto memory = std::span<std::byte>(
        reinterpret_cast<std::byte*>(backing_.data()), layout->usedBytes);
    core::ArenaSourcePayloadBuilder builder(block_, *layout, memory);

    for (const auto& hot : slot.drawHotStates) {
      if (!builder.tryAppendDrawHotState(hot)) return;
    }
    for (const auto& shader : slot.drawShaderLayouts) {
      auto copy = shader;
      if (!builder.tryAppendDrawShaderLayout(std::move(copy))) return;
    }
    for (const auto& debug : slot.drawDebugSnapshots) {
      if (!builder.tryAppendDrawDebugSnapshot(debug)) return;
    }
    for (std::size_t i = 0; i < slot.drawRunRecords.size(); ++i) {
      const core::DrawPsoSubview pso =
          i < slot.drawPsoSubviews.size() ? slot.drawPsoSubviews[i]
                                          : core::DrawPsoSubview{};
      if (!builder.tryAppendDrawPsoSubview(pso)) return;
    }
    for (const auto& value : slot.drawUniformFixedPayloads) {
      if (!builder.tryAppendDrawUniformFixedPayload(value)) return;
    }
    if (!slot.drawUniformVertexConstantBytes.empty()) {
      std::size_t offset = 1;
      if (!builder.tryAppendVertexConstantBytes(
              slot.drawUniformVertexConstantBytes, 1, offset) ||
          offset != 0) {
        return;
      }
    }
    for (const auto& value : slot.drawUniformVertexConstants) {
      if (!builder.tryAppendDrawUniformVertexConstants(value)) return;
    }
    if (!slot.drawUniformPixelConstantBytes.empty()) {
      std::size_t offset = 1;
      if (!builder.tryAppendPixelConstantBytes(
              slot.drawUniformPixelConstantBytes, 1, offset) ||
          offset != 0) {
        return;
      }
    }
    for (const auto& value : slot.drawUniformPixelConstants) {
      if (!builder.tryAppendDrawUniformPixelConstants(value)) return;
    }
    for (const auto& value : slot.drawUniformPayloads) {
      if (!builder.tryAppendDrawUniformPayload(value)) return;
    }
    for (const auto& param : slot.drawParams) {
      if (!builder.tryAppendDrawParam(param)) return;
    }
    if (!slot.drawPayloadArena.empty()) {
      std::size_t offset = 1;
      if (!builder.tryAppendDrawPayloadBytes(slot.drawPayloadArena, 1, offset) ||
          offset != 0) {
        return;
      }
    }

    const core::SourcePayloadView legacy(slot);
    for (std::size_t i = 0; i < legacy.commandCount(); ++i) {
      const core::SourceCommandView source = legacy.commandAt(i);
      const auto& command = source.command;
      switch (source.kind()) {
      case core::MetalCommandKind::DrawRun:
        if (!command.drawRunRecord ||
            !builder.tryAppendDrawRun(*command.drawRunRecord) ||
            !builder.tryAppendCommand(core::MetalCommandKind::DrawRun,
                                      static_cast<std::uint32_t>(
                                          blockDrawRunCount_++))) {
          return;
        }
        break;
      case core::MetalCommandKind::Clear:
        if (!command.clear || !builder.tryAppendClearCommand(*command.clear))
          return;
        break;
      case core::MetalCommandKind::SurfaceCopy:
        if (!command.surfaceCopy ||
            !builder.tryAppendSurfaceCopyCommand(*command.surfaceCopy))
          return;
        break;
      case core::MetalCommandKind::StretchRect:
        if (!command.stretchRect ||
            !builder.tryAppendStretchRectCommand(*command.stretchRect))
          return;
        break;
      case core::MetalCommandKind::Readback:
        if (!command.readback ||
            !builder.tryAppendReadbackCommand(*command.readback))
          return;
        break;
      case core::MetalCommandKind::ColorFill:
        if (!command.colorFill ||
            !builder.tryAppendColorFillCommand(*command.colorFill))
          return;
        break;
      case core::MetalCommandKind::DepthResolve:
        if (!command.depthResolve ||
            !builder.tryAppendDepthResolveCommand(*command.depthResolve))
          return;
        break;
      case core::MetalCommandKind::GenerateMipmaps:
        if (!command.generateMipmaps ||
            !builder.tryAppendGenerateMipmapsCommand(
                *command.generateMipmaps))
          return;
        break;
      case core::MetalCommandKind::Present: {
        if (!command.present) return;
        auto present = *command.present;
        if (!builder.tryAppendPresentCommand(std::move(present))) return;
        break;
      }
      }
    }
    valid_ = builder.publish();
  }

  ~ArenaPayloadFixture() { block_.destroyConstructed(); }

  ArenaPayloadFixture(const ArenaPayloadFixture&) = delete;
  ArenaPayloadFixture& operator=(const ArenaPayloadFixture&) = delete;
  ArenaPayloadFixture(ArenaPayloadFixture&&) = delete;
  ArenaPayloadFixture& operator=(ArenaPayloadFixture&&) = delete;

  bool valid() const noexcept { return valid_; }
  core::SourcePayloadView view() const noexcept {
    return core::SourcePayloadView(block_);
  }

 private:
  std::vector<std::max_align_t> backing_{};
  core::ArenaSourcePayloadBlock block_{};
  std::size_t blockDrawRunCount_ = 0;
  bool valid_ = false;
};

class SegmentedArenaPayloadFixture {
 public:
  SegmentedArenaPayloadFixture(const core::ChunkSlot& first,
                               const core::ChunkSlot& second)
      : first_(first), second_(second) {
    if (!first_.valid() || !second_.valid()) {
      return;
    }
    const std::array<const core::ArenaSourcePayloadBlock*, 2> segments{
        first_.view().arenaPayload(), second_.view().arenaPayload()};
    valid_ = chain_.initialize(segments);
  }

  ~SegmentedArenaPayloadFixture() { chain_.clear(); }

  SegmentedArenaPayloadFixture(const SegmentedArenaPayloadFixture&) = delete;
  SegmentedArenaPayloadFixture& operator=(
      const SegmentedArenaPayloadFixture&) = delete;
  SegmentedArenaPayloadFixture(SegmentedArenaPayloadFixture&&) = delete;
  SegmentedArenaPayloadFixture& operator=(
      SegmentedArenaPayloadFixture&&) = delete;

  bool valid() const noexcept { return valid_; }
  core::SourcePayloadView view() const noexcept {
    return core::SourcePayloadView(chain_);
  }

 private:
  ArenaPayloadFixture first_;
  ArenaPayloadFixture second_;
  core::ArenaSourcePayloadChain chain_{};
  bool valid_ = false;
};

}  // namespace dxmt9::tests::framegraph
