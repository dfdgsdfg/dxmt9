#pragma once

#include "dxmt9/core.hpp"
#include "dxmt9/assert.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::core {

enum class MetalCommandKind : std::uint8_t {
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

struct DrawRunCommandRecord {
  std::uint32_t stateIndex = 0;
  std::uint32_t firstParam = 0;
  std::uint32_t paramCount = 0;
  std::uint32_t payloadOffset = 0;
  std::uint32_t payloadSize = 0;
  DrawUniformHandle uniformHandle{};
};

struct DrawUniformPayloadRecord {
  DrawUniformHandle handle{};
  DrawUniformPayload payload{};
};

struct MetalCommandHeader {
  MetalCommandKind kind = MetalCommandKind::DrawRun;
  std::uint32_t payloadIndex = 0;
};

struct MetalCommandView {
  MetalCommandKind kind = MetalCommandKind::DrawRun;
  const DrawRunCommandRecord* drawRunRecord = nullptr;
  const CanonicalDrawState* drawState = nullptr;
  const DrawUniformPayload* drawUniformPayload = nullptr;
  std::span<const DrawParam> drawParams{};
  std::span<const u8> drawPayloadBytes{};
  const ClearDesc* clear = nullptr;
  const SurfaceCopyDesc* surfaceCopy = nullptr;
  const StretchRectDesc* stretchRect = nullptr;
  const ReadbackDesc* readback = nullptr;
  const ColorFillDesc* colorFill = nullptr;
  const PresentCommandRecord* present = nullptr;
};

inline std::span<const u8> drawRunPayloadBytes(const MetalCommandView& command) noexcept {
  return command.drawPayloadBytes;
}

inline std::size_t drawRunPayloadSize(const MetalCommandView& command) noexcept {
  return command.drawPayloadBytes.size();
}

inline std::size_t drawRunDrawCount(const MetalCommandView& command) noexcept {
  return command.drawParams.size();
}

struct ChunkSlot {
  enum class State { Free, Writing, Pending, Encoding, GPU };

  State state = State::Free;
  u64 seqId = 0;

  // Data-oriented execution storage. The replay loop walks commandHeaders
  // linearly and indexes into type-specific payload arrays. This avoids the
  // old fat record shape where every command carried every possible payload.
  std::vector<MetalCommandHeader> commandHeaders;
  std::vector<CanonicalDrawState> drawStates;
  std::vector<DrawUniformPayloadRecord> drawUniformPayloads;
  std::vector<DrawParam> drawParams;
  std::vector<u8> drawPayloadArena;
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
    drawUniformPayloads.clear();
    drawParams.clear();
    drawPayloadArena.clear();
    drawRunRecords.clear();
    clearRecords.clear();
    surfaceCopyRecords.clear();
    stretchRectRecords.clear();
    readbackRecords.clear();
    colorFillRecords.clear();
    presentRecords.clear();
  }

  void appendDrawRun(DrawRunDesc drawRun) {
    const auto view = drawRunView(drawRun);
    const auto stateIndex = static_cast<std::uint32_t>(drawStates.size());
    const auto uniformIndex = static_cast<std::uint32_t>(drawUniformPayloads.size());
    const auto firstParam = static_cast<std::uint32_t>(drawParams.size());
    const bool canUseSlotArena =
        drawPayloadArena.size() <= std::numeric_limits<std::uint32_t>::max() &&
        drawRunPayloadSize(drawRun) <=
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) -
                drawPayloadArena.size();
    DXMT_ASSERT(canUseSlotArena && "draw payload arena exceeded 32-bit range storage");
    if (!canUseSlotArena) {
      return;
    }
    const auto payloadOffset = static_cast<std::uint32_t>(drawPayloadArena.size());
    const auto& uniformPayload = drawRunUniformPayload(drawRun);
    const DrawUniformHandle uniformHandle{
        .index = uniformIndex,
        .generation = uniformIndex + 1u,
        .hash = uniformPayload.hash,
    };

    drawStates.push_back(std::move(drawRun.state));
    drawUniformPayloads.push_back(DrawUniformPayloadRecord{
        .handle = uniformHandle,
        .payload = uniformPayload,
    });

    const auto payloadBytes = view.payloadArena;
    if (!payloadBytes.empty()) {
      drawPayloadArena.insert(drawPayloadArena.end(), payloadBytes.begin(), payloadBytes.end());
    }

    commandHeaders.push_back({MetalCommandKind::DrawRun, static_cast<std::uint32_t>(drawRunRecords.size())});
    drawParams.insert(drawParams.end(), view.draws.begin(), view.draws.end());
    drawRunRecords.push_back(DrawRunCommandRecord{
        .stateIndex = stateIndex,
        .firstParam = firstParam,
        .paramCount = static_cast<std::uint32_t>(drawParams.size() - firstParam),
        .payloadOffset = payloadOffset,
        .payloadSize = static_cast<std::uint32_t>(payloadBytes.size()),
        .uniformHandle = uniformHandle,
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
    case MetalCommandKind::DrawRun:
      if (payloadIndex < drawRunRecords.size()) {
        const auto& record = drawRunRecords[payloadIndex];
        view.drawRunRecord = &record;
        if (record.payloadSize > 0 &&
            record.payloadOffset <= drawPayloadArena.size() &&
            record.payloadSize <= drawPayloadArena.size() - record.payloadOffset) {
          view.drawPayloadBytes = std::span<const u8>(
              drawPayloadArena.data() + record.payloadOffset, record.payloadSize);
        }
        if (record.stateIndex < drawStates.size()) {
          view.drawState = &drawStates[record.stateIndex];
        }
        if (record.uniformHandle.index < drawUniformPayloads.size()) {
          const auto& uniformRecord = drawUniformPayloads[record.uniformHandle.index];
          if (uniformRecord.handle == record.uniformHandle) {
            view.drawUniformPayload = &uniformRecord.payload;
          }
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
