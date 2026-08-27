#include "device_c_record_utils.hpp"
#include "device_c_common.hpp"

#include <cstdint>
#include <cstring>

namespace dxmt9::d3d9::devicec {
ImportedRecordReplayInfo replayInfoForCommandRecordType(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::Draw,
        .ordered = true,
        .mutatesDeviceState = true,
        .readsDeviceState = true,
        .referencesResources = true,
        .draw = true,
    };
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::ConstantUpload,
        .ordered = true,
        .mutatesDeviceState = true,
    };
  case D9C_COMMAND_RECORD_APPLY_STATE:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::StateApply,
        .ordered = true,
        .mutatesDeviceState = true,
        .referencesResources = true,
    };
  case D9C_COMMAND_RECORD_CLEAR:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::Clear,
        .ordered = true,
        .readsDeviceState = true,
        .barrier = true,
    };
  case D9C_COMMAND_RECORD_PRESENT:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::Present,
        .ordered = true,
        .readsDeviceState = true,
        .barrier = true,
    };
  case D9C_COMMAND_RECORD_STRETCH_RECT:
  case D9C_COMMAND_RECORD_COLOR_FILL:
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
  case D9C_COMMAND_RECORD_UPDATE_SURFACE:
  // R-FORMAT-11: RESZ depth-resolve is a fire-and-forget surface op (MSAA
  // depth source → INTZ destination) — the same SurfaceOp ordering-barrier
  // class as StretchRect/ColorFill, NOT the synchronousReadBoundary Readback
  // class, so PE never blocks on a result. Classifying it here (rather than
  // letting it fall through to the empty Unknown info) keeps chunk replay
  // from misreading it as a draw.
  case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
  case D9C_COMMAND_RECORD_GENERATE_MIPMAPS:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::SurfaceOp,
        .ordered = true,
        .readsDeviceState = true,
        .referencesResources = true,
        .barrier = true,
    };
  case D9C_COMMAND_RECORD_QUERY_ISSUE:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::QueryIssue,
        .ordered = true,
        .barrier = true,
    };
  case D9C_COMMAND_RECORD_READBACK:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::Readback,
        .ordered = true,
        .readsDeviceState = true,
        .referencesResources = true,
        .barrier = true,
        .synchronousReadBoundary = true,
    };
  default:
    return ImportedRecordReplayInfo{};
  }
}

}  // namespace dxmt9::d3d9::devicec
