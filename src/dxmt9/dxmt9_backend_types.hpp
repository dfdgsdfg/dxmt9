#pragma once

#include "dxmt9/core.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace dxmt9::core {

enum class MetalCommandKind : std::uint8_t {
  Draw,
  DrawRun,        // BaseDrawState + N DrawParam — see core::DrawRunDesc
  Clear,
  SurfaceCopy,
  StretchRect,
  Readback,
  ColorFill,
  Present,
};

struct PresentCommandRecord {
  SwapDesc present{};
  Handle presentSource{};
};

struct DrawCommandRecord {
  CanonicalDrawState state{};
  DrawParam param{};
  std::vector<u8> payloadArena;
};

struct MetalCommandHeader {
  MetalCommandKind kind = MetalCommandKind::Draw;
  std::uint32_t payloadIndex = 0;
};

struct MetalCommandView {
  MetalCommandKind kind = MetalCommandKind::Draw;
  const DrawCommandRecord* drawRecord = nullptr;
  const DrawDesc* draw = nullptr;
  const DrawRunDesc* drawRun = nullptr;
  const ClearDesc* clear = nullptr;
  const SurfaceCopyDesc* surfaceCopy = nullptr;
  const StretchRectDesc* stretchRect = nullptr;
  const ReadbackDesc* readback = nullptr;
  const ColorFillDesc* colorFill = nullptr;
  const PresentCommandRecord* present = nullptr;
};

struct ChunkSlot {
  enum class State { Free, Writing, Pending, Encoding, GPU };

  State state = State::Free;
  u64 seqId = 0;

  // Data-oriented execution storage. The replay loop walks commandHeaders
  // linearly and indexes into type-specific payload arrays. This avoids the
  // old fat record shape where every command carried every possible payload.
  std::vector<MetalCommandHeader> commandHeaders;
  std::vector<DrawCommandRecord> drawRecords;
  std::vector<DrawRunDesc> drawRunRecords;
  std::vector<ClearDesc> clearRecords;
  std::vector<SurfaceCopyDesc> surfaceCopyRecords;
  std::vector<StretchRectDesc> stretchRectRecords;
  std::vector<ReadbackDesc> readbackRecords;
  std::vector<ColorFillDesc> colorFillRecords;
  std::vector<PresentCommandRecord> presentRecords;

  bool commandsEmpty() const noexcept {
    return commandHeaders.empty();
  }

  std::size_t commandCount() const noexcept {
    return commandHeaders.size();
  }

  void clearCommands() {
    commandHeaders.clear();
    drawRecords.clear();
    drawRunRecords.clear();
    clearRecords.clear();
    surfaceCopyRecords.clear();
    stretchRectRecords.clear();
    readbackRecords.clear();
    colorFillRecords.clear();
    presentRecords.clear();
  }

  void appendDraw(const DrawDesc& draw) {
    commandHeaders.push_back({MetalCommandKind::Draw, static_cast<std::uint32_t>(drawRecords.size())});
    DrawCommandRecord record{};
    record.state = makeCanonicalDrawState(draw);
    record.param = makeDrawParamFromDesc(draw);
    if (!packDrawParamPayload(record.param, record.payloadArena)) {
      record.payloadArena.clear();
      record.param = makeDrawParamFromDesc(draw);
    }
    drawRecords.push_back(std::move(record));
  }

  void appendDrawRun(DrawRunDesc drawRun) {
    drawRun.state.key = makeFlatDrawStateKey(drawRun.state.desc);
    commandHeaders.push_back({MetalCommandKind::DrawRun, static_cast<std::uint32_t>(drawRunRecords.size())});
    drawRunRecords.push_back(std::move(drawRun));
  }

  void appendClear(const ClearDesc& clear) {
    commandHeaders.push_back({MetalCommandKind::Clear, static_cast<std::uint32_t>(clearRecords.size())});
    clearRecords.push_back(clear);
  }

  void appendSurfaceCopy(const SurfaceCopyDesc& surfaceCopy) {
    commandHeaders.push_back({MetalCommandKind::SurfaceCopy,
                              static_cast<std::uint32_t>(surfaceCopyRecords.size())});
    surfaceCopyRecords.push_back(surfaceCopy);
  }

  void appendStretchRect(const StretchRectDesc& stretchRect) {
    commandHeaders.push_back({MetalCommandKind::StretchRect,
                              static_cast<std::uint32_t>(stretchRectRecords.size())});
    stretchRectRecords.push_back(stretchRect);
  }

  void appendReadback(const ReadbackDesc& readback) {
    commandHeaders.push_back({MetalCommandKind::Readback, static_cast<std::uint32_t>(readbackRecords.size())});
    readbackRecords.push_back(readback);
  }

  void appendColorFill(const ColorFillDesc& colorFill) {
    commandHeaders.push_back({MetalCommandKind::ColorFill,
                              static_cast<std::uint32_t>(colorFillRecords.size())});
    colorFillRecords.push_back(colorFill);
  }

  void appendPresent(const SwapDesc& present, Handle presentSource) {
    commandHeaders.push_back({MetalCommandKind::Present, static_cast<std::uint32_t>(presentRecords.size())});
    presentRecords.push_back(PresentCommandRecord{
        .present = present,
        .presentSource = presentSource,
    });
  }

  MetalCommandView commandAt(std::size_t index) const {
    if (index >= commandHeaders.size()) {
      return {};
    }
    const auto& header = commandHeaders[index];
    const std::size_t payloadIndex = header.payloadIndex;
    MetalCommandView view{.kind = header.kind};
    switch (header.kind) {
    case MetalCommandKind::Draw:
      if (payloadIndex < drawRecords.size()) {
        view.drawRecord = &drawRecords[payloadIndex];
        view.draw = &drawRecords[payloadIndex].state.desc;
      }
      break;
    case MetalCommandKind::DrawRun:
      if (payloadIndex < drawRunRecords.size()) view.drawRun = &drawRunRecords[payloadIndex];
      break;
    case MetalCommandKind::Clear:
      if (payloadIndex < clearRecords.size()) view.clear = &clearRecords[payloadIndex];
      break;
    case MetalCommandKind::SurfaceCopy:
      if (payloadIndex < surfaceCopyRecords.size()) view.surfaceCopy = &surfaceCopyRecords[payloadIndex];
      break;
    case MetalCommandKind::StretchRect:
      if (payloadIndex < stretchRectRecords.size()) view.stretchRect = &stretchRectRecords[payloadIndex];
      break;
    case MetalCommandKind::Readback:
      if (payloadIndex < readbackRecords.size()) view.readback = &readbackRecords[payloadIndex];
      break;
    case MetalCommandKind::ColorFill:
      if (payloadIndex < colorFillRecords.size()) view.colorFill = &colorFillRecords[payloadIndex];
      break;
    case MetalCommandKind::Present:
      if (payloadIndex < presentRecords.size()) view.present = &presentRecords[payloadIndex];
      break;
    }
    return view;
  }
};

}  // namespace dxmt9::core
