#pragma once

#include "dxmt9/core.hpp"

#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
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
  std::uint32_t stateIndex = 0;
  std::uint32_t paramIndex = 0;
};

struct DrawRunCommandRecord {
  std::uint32_t stateIndex = 0;
  std::uint32_t firstParam = 0;
  std::uint32_t paramCount = 0;
};

struct MetalCommandHeader {
  MetalCommandKind kind = MetalCommandKind::Draw;
  std::uint32_t payloadIndex = 0;
};

struct MetalCommandView {
  MetalCommandKind kind = MetalCommandKind::Draw;
  const DrawCommandRecord* drawRecord = nullptr;
  const DrawRunCommandRecord* drawRunRecord = nullptr;
  const CanonicalDrawState* drawState = nullptr;
  const DrawParam* drawParam = nullptr;
  std::span<const DrawParam> drawParams{};
  const DrawDesc* draw = nullptr;
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
  std::vector<CanonicalDrawState> drawStates;
  std::vector<DrawParam> drawParams;
  std::vector<u8> drawPayloadArena;
  std::vector<DrawCommandRecord> drawRecords;
  std::vector<DrawRunCommandRecord> drawRunRecords;
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
    drawStates.clear();
    drawParams.clear();
    drawPayloadArena.clear();
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
    const auto stateIndex = static_cast<std::uint32_t>(drawStates.size());
    const auto paramIndex = static_cast<std::uint32_t>(drawParams.size());
    drawStates.push_back(makeCanonicalDrawState(draw));
    DrawParam param = makeDrawParamFromDesc(draw);
    if (!packDrawParamPayload(param, drawPayloadArena)) {
      param = makeDrawParamFromDesc(draw);
    }
    drawParams.push_back(std::move(param));
    drawRecords.push_back(DrawCommandRecord{
        .stateIndex = stateIndex,
        .paramIndex = paramIndex,
    });
  }

  void appendDrawRun(DrawRunDesc drawRun) {
    drawRun.state.hot = makeFlatDrawStateRecord(drawRun.state.coldDesc);
    drawRun.state.shaderLayout = makeDrawShaderLayoutContext(drawRun.state.coldDesc);
    drawRun.state.debug = makeDrawDebugSnapshot(drawRun.state.coldDesc, drawRun.state.hot);
    const auto stateIndex = static_cast<std::uint32_t>(drawStates.size());
    const auto firstParam = static_cast<std::uint32_t>(drawParams.size());
    drawStates.push_back(std::move(drawRun.state));

    const bool canUseSlotArena =
        drawPayloadArena.size() <= std::numeric_limits<std::uint32_t>::max() &&
        drawRun.payloadArena.size() <=
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) -
                drawPayloadArena.size();
    if (canUseSlotArena && !drawRun.payloadArena.empty()) {
      const auto baseOffset = static_cast<std::uint32_t>(drawPayloadArena.size());
      drawPayloadArena.insert(drawPayloadArena.end(), drawRun.payloadArena.begin(),
                              drawRun.payloadArena.end());
      for (auto& param : drawRun.draws) {
        if (!param.userVertexRange.empty()) param.userVertexRange.offset += baseOffset;
        if (!param.userIndexRange.empty()) param.userIndexRange.offset += baseOffset;
      }
    } else if (!drawRun.payloadArena.empty()) {
      for (auto& param : drawRun.draws) {
        const auto materializePayload = [&drawRun](DrawPayloadRange range) {
          std::vector<u8> bytes;
          const std::size_t offset = range.offset;
          const std::size_t size = range.size;
          if (size == 0 || offset > drawRun.payloadArena.size() ||
              size > drawRun.payloadArena.size() - offset) {
            return bytes;
          }
          bytes.insert(bytes.end(), drawRun.payloadArena.begin() + offset,
                       drawRun.payloadArena.begin() + offset + size);
          return bytes;
        };
        if (!param.userVertexRange.empty()) {
          param.userVertexData = materializePayload(param.userVertexRange);
          param.userVertexRange = {};
        }
        if (!param.userIndexRange.empty()) {
          param.userIndexData = materializePayload(param.userIndexRange);
          param.userIndexRange = {};
        }
        if (!packDrawParamPayload(param, drawPayloadArena)) {
          param.userVertexRange = {};
          param.userIndexRange = {};
        }
      }
    }

    commandHeaders.push_back({MetalCommandKind::DrawRun, static_cast<std::uint32_t>(drawRunRecords.size())});
    drawParams.insert(drawParams.end(),
                      std::make_move_iterator(drawRun.draws.begin()),
                      std::make_move_iterator(drawRun.draws.end()));
    drawRunRecords.push_back(DrawRunCommandRecord{
        .stateIndex = stateIndex,
        .firstParam = firstParam,
        .paramCount = static_cast<std::uint32_t>(drawParams.size() - firstParam),
    });
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
        const auto& record = drawRecords[payloadIndex];
        view.drawRecord = &record;
        if (record.stateIndex < drawStates.size()) {
          view.drawState = &drawStates[record.stateIndex];
          view.draw = &view.drawState->coldDesc;
        }
        if (record.paramIndex < drawParams.size()) {
          view.drawParam = &drawParams[record.paramIndex];
        }
      }
      break;
    case MetalCommandKind::DrawRun:
      if (payloadIndex < drawRunRecords.size()) {
        const auto& record = drawRunRecords[payloadIndex];
        view.drawRunRecord = &record;
        if (record.stateIndex < drawStates.size()) {
          view.drawState = &drawStates[record.stateIndex];
          view.draw = &view.drawState->coldDesc;
        }
        if (record.firstParam <= drawParams.size() &&
            record.paramCount <= drawParams.size() - record.firstParam) {
          view.drawParams = std::span<const DrawParam>(
              drawParams.data() + record.firstParam, record.paramCount);
        }
      }
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
