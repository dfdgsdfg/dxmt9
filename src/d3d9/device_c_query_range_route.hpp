#pragma once

#include "device_c_chunk_validate.hpp"

namespace dxmt9::d3d9 {
// A routing shortcut only for an already validated immutable draw/state raw.
// Capture, controls and every unlisted family retain the full planner.
inline bool queryRangeCompatibilityShortcut(
    const ImportedChunkView& imported, bool ranges, bool capture) noexcept {
  if (!ranges || capture) return false;
  bool hasDraw = false;
  for (const auto& record : imported.records) {
    switch (record.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
      hasDraw = true;
      break;
    case D9C_COMMAND_RECORD_APPLY_STATE:
      break;
    default:
      return false;
    }
  }
  return hasDraw;
}
} // namespace dxmt9::d3d9
