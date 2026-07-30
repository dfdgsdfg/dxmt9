#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"

namespace dxmt9::d3d9 {
struct ResolvedRecordV2View;
}

namespace dxmt9::d3d9::devicec {

enum class ImportedRecordReplayCategory : std::uint8_t {
  Unknown,
  Draw,
  ConstantUpload,
  StateApply,
  Clear,
  Present,
  SurfaceOp,
  QueryIssue,
  Readback,
};

struct ImportedRecordReplayInfo {
  ImportedRecordReplayCategory category = ImportedRecordReplayCategory::Unknown;
  bool ordered = false;
  bool mutatesDeviceState = false;
  bool readsDeviceState = false;
  bool referencesResources = false;
  bool draw = false;
  bool barrier = false;
  bool synchronousReadBoundary = false;
};

ImportedRecordReplayInfo replayInfoForCommandRecordType(std::uint32_t type) noexcept;

}  // namespace dxmt9::d3d9::devicec
