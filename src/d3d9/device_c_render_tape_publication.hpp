#pragma once

// Cold CPU experiment publication policy. Call only after authenticated Tape
// validation and bounded CPU grammar admission. This plan never changes the
// production replay planner, capacity admission or semantic fallback policy.

#include "device_c_render_tape.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace dxmt9::d3d9 {

enum class RenderTapeCpuPublicationMode : std::uint8_t {
  PerCommand,
  DrawIsland,
};

struct RenderTapeCpuPublicationRange {
  std::uint32_t firstEvent = 0u;
  std::uint32_t lastEvent = 0u;
  std::uint32_t commandEvents = 0u;
  std::uint32_t drawRecords = 0u;
};

struct RenderTapeCpuPublicationPlan {
  bool valid = false;
  std::uint32_t count = 0u;
  std::array<RenderTapeCpuPublicationRange, 32u> ranges{};

  const RenderTapeCpuPublicationRange* rangeForEvent(
      std::uint32_t event) const noexcept {
    if (!valid) return nullptr;
    for (std::uint32_t i = 0u; i < count; ++i) {
      if (event >= ranges[i].firstEvent && event <= ranges[i].lastEvent)
        return &ranges[i];
    }
    return nullptr;
  }
};

inline RenderTapeCpuPublicationPlan planRenderTapeCpuPublication(
    const ImportedRenderTapeView& tape, RenderTapeCpuPublicationMode mode) {
  RenderTapeCpuPublicationPlan result;
  if (tape.events.size() > result.ranges.size() ||
      (mode != RenderTapeCpuPublicationMode::PerCommand &&
       mode != RenderTapeCpuPublicationMode::DrawIsland)) return result;
  bool priorDrawOnly = false;
  for (std::uint32_t i = 0u; i < tape.events.size(); ++i) {
    const auto event = tape.event(i);
    if (event.header.ordinal != i + 1u) return {};
    if (event.header.type !=
        static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk)) {
      priorDrawOnly = false;
      continue;
    }
    RenderTapeCommandChunkHeader header{};
    if (event.payload.size() < sizeof(header)) return {};
    std::memcpy(&header, event.payload.data(), sizeof(header));
    ImportedChunkView chunk;
    if (!importPrevalidatedCommandChunk(
            event.payload.subspan(sizeof(header)),
            CommandChunkEnvelope{.version = header.wireVersion,
                                 .recordCount = header.recordCount,
                                 .handleCount = header.handleCount}, chunk))
      return {};
    const auto draws = static_cast<std::uint32_t>(std::count_if(
        chunk.records.begin(), chunk.records.end(), [](const auto& record) {
          return record.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
        }));
    const bool drawOnly = draws != 0u && draws == chunk.records.size();
    if (mode == RenderTapeCpuPublicationMode::DrawIsland && priorDrawOnly &&
        drawOnly && result.count != 0u &&
        result.ranges[result.count - 1u].lastEvent == i) {
      auto& range = result.ranges[result.count - 1u];
      range.lastEvent = i + 1u;
      ++range.commandEvents;
      range.drawRecords += draws;
    } else {
      if (result.count == result.ranges.size()) return {};
      result.ranges[result.count++] = {i + 1u, i + 1u, 1u, draws};
    }
    priorDrawOnly = drawOnly;
  }
  result.valid = true;
  return result;
}

// Model/code binding for a multi-event source. The actual semantic draw count
// comes from the submitted payload projection, never from requested-path
// counters. Source identity is independently supplied by the real queue and
// checked by RenderTapeCompletionPlanController.
inline bool renderTapeGroupedPublicationMatches(
    const RenderTapeCpuPublicationRange& expected,
    std::uint32_t executedFirstEvent, std::uint32_t executedLastEvent,
    std::uint32_t executedCommandEvents, std::uint32_t publishEvent,
    std::uint32_t sourceCount, std::uint32_t observedDraws) noexcept {
  return expected.firstEvent != 0u &&
         expected.firstEvent < expected.lastEvent &&
         expected.commandEvents == expected.lastEvent - expected.firstEvent + 1u &&
         expected.drawRecords != 0u &&
         executedFirstEvent == expected.firstEvent &&
         executedLastEvent == expected.lastEvent &&
         executedCommandEvents == expected.commandEvents &&
         publishEvent == expected.lastEvent && sourceCount == 1u &&
         observedDraws == expected.drawRecords;
}

}  // namespace dxmt9::d3d9
