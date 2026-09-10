#pragma once

// Cold authenticated-input analysis shared by the inspector and CPU harness.
// This proves command shape only; live queue/runtime admission is separate.
#include "device_c_cpu_ready_plan.hpp"

namespace dxmt9::d3d9 {

inline constexpr std::size_t kRenderTapeCpuWindowPageSize = 4096u;

struct RenderTapeCpuWindowSourcePlan {
  std::size_t recordCount = 0u;
  std::size_t allowedRecordCount = 0u;
  ReplayEmissionPlan emission;

  bool eligible() const noexcept {
    return recordCount > 0u && recordCount <= 64u && allowedRecordCount == recordCount &&
        emission.leaseEligible() && emission.coordinatorCount == 0u &&
        emission.orderedControlCount == 0u && emission.compatibilityRecordCount == 0u &&
        emission.leaseSpans.size() == 1u && emission.leaseSpans.front().ownsLease &&
        emission.leaseSpans.front().firstRecordIndex == 0u &&
        emission.leaseSpans.front().recordCount == recordCount &&
        emission.leaseSpans.front().finalLeaseSpan;
  }
};

inline RenderTapeCpuWindowSourcePlan planRenderTapeCpuWindowSource(
    const ImportedChunkView& imported, RawOrdinal ordinal, std::size_t pageSize) {
  RenderTapeCpuWindowSourcePlan result;
  result.recordCount = imported.records.size();
  for (const auto& record : imported.records) {
    switch (record.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
    case D9C_COMMAND_RECORD_APPLY_STATE:
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
      ++result.allowedRecordCount;
      break;
    default: break;
    }
  }
  result.emission = planReplayEmission(imported, ordinal, pageSize);
  return result;
}

}  // namespace dxmt9::d3d9
