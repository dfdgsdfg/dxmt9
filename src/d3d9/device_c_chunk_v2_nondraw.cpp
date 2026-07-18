#include "device_c_chunk_v2_replay.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt9::d3d9 {

namespace {

template <typename T>
bool loadFixed(const ImportedRecordV2View& record, T& value) {
  if (record.payload.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&value, record.payload.data(), sizeof(T));
  return true;
}

template <typename T>
void* resolvedObject(const ResolvedRecordV2View& record,
                     std::uint32_t T::* field, const T& fixed) {
  return record.objectForAbsoluteIndex(fixed.*field);
}

}  // namespace

void* ResolvedRecordV2View::objectForAbsoluteIndex(
    std::uint32_t index) const noexcept {
  if (index == D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX ||
      index < wire.header.firstHandle) {
    return nullptr;
  }
  const auto relative = static_cast<std::size_t>(
      index - wire.header.firstHandle);
  return relative < objects.size() ? objects[relative] : nullptr;
}

ResolvedRecordV2View ResolvedChunkV2View::record(
    std::size_t index) const noexcept {
  const auto imported = wire.record(index);
  if (index >= wire.records.size() ||
      imported.header.firstHandle > objects.size() ||
      imported.header.handleCount >
          objects.size() - imported.header.firstHandle) {
    return {};
  }
  return ResolvedRecordV2View{
      .wire = imported,
      .objects = objects.subspan(imported.header.firstHandle,
                                 imported.header.handleCount),
  };
}

bool isNonDrawRecordV2(std::uint32_t type) noexcept {
  const auto* rule = v2RecordRule(type);
  return rule && (rule->ruleFlags & V2RecordRuleDraw) == 0u;
}

std::int32_t replayNonDrawRecordV2(
    const ResolvedRecordV2View& record,
    NonDrawReplaySinkV2& sink) noexcept {
  if (!isNonDrawRecordV2(record.wire.header.type) ||
      record.objects.size() != record.wire.header.handleCount ||
      std::any_of(record.objects.begin(), record.objects.end(),
                  [](const void* object) { return object == nullptr; })) {
    return kCommandChunkV2DecodeFailure;
  }

  switch (record.wire.header.type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B: {
      D9CCommandChunkWireSetConstV2 fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkV2DecodeFailure;
      }
      return sink.setConstants(
          record.wire.header.type, fixed,
          record.wire.payload.subspan(sizeof(fixed)));
    }
    case D9C_COMMAND_RECORD_CLEAR: {
      D9CCommandChunkWireClearV2 fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkV2DecodeFailure;
      }
      const auto* rects = reinterpret_cast<const D9CRect*>(
          record.wire.payload.data() + fixed.rectOffset);
      return sink.clear(fixed,
                        std::span<const D9CRect>(rects, fixed.rectCount));
    }
    case D9C_COMMAND_RECORD_PRESENT: {
      D9CCommandChunkWirePresentV2 fixed{};
      return loadFixed(record.wire, fixed)
                 ? sink.present(fixed)
                 : kCommandChunkV2DecodeFailure;
    }
    case D9C_COMMAND_RECORD_STRETCH_RECT: {
      D9CCommandChunkWireStretchRectV2 fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkV2DecodeFailure;
      }
      return sink.stretchRect(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireStretchRectV2::
                                    srcHandleIndex,
                                fixed),
          resolvedObject(record,
                         &D9CCommandChunkWireStretchRectV2::dstHandleIndex,
                         fixed));
    }
    case D9C_COMMAND_RECORD_COLOR_FILL: {
      D9CCommandChunkWireColorFillV2 fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkV2DecodeFailure;
      }
      return sink.colorFill(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireColorFillV2::
                                    surfaceHandleIndex,
                                fixed));
    }
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
      D9CCommandChunkWireUpdateTextureV2 fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkV2DecodeFailure;
      }
      return sink.updateTexture(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireUpdateTextureV2::
                                    srcHandleIndex,
                                fixed),
          resolvedObject(record,
                         &D9CCommandChunkWireUpdateTextureV2::dstHandleIndex,
                         fixed));
    }
    case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
      D9CCommandChunkWireUpdateSurfaceV2 fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkV2DecodeFailure;
      }
      return sink.updateSurface(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireUpdateSurfaceV2::
                                    srcHandleIndex,
                                fixed),
          resolvedObject(record,
                         &D9CCommandChunkWireUpdateSurfaceV2::dstHandleIndex,
                         fixed));
    }
    case D9C_COMMAND_RECORD_QUERY_ISSUE: {
      D9CCommandChunkWireQueryIssueV2 fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkV2DecodeFailure;
      }
      return sink.queryIssue(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireQueryIssueV2::
                                    queryHandleIndex,
                                fixed));
    }
    case D9C_COMMAND_RECORD_READBACK: {
      D9CCommandChunkWireReadbackV2 fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkV2DecodeFailure;
      }
      return sink.readback(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireReadbackV2::srcHandleIndex,
                                fixed),
          resolvedObject(record,
                         &D9CCommandChunkWireReadbackV2::dstHandleIndex,
                         fixed));
    }
    case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE: {
      D9CCommandChunkWireReszDepthResolveV2 fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkV2DecodeFailure;
      }
      return sink.reszDepthResolve(
          fixed,
          resolvedObject(
              record,
              &D9CCommandChunkWireReszDepthResolveV2::msaaDepthHandleIndex,
              fixed),
          resolvedObject(
              record,
              &D9CCommandChunkWireReszDepthResolveV2::intzDestHandleIndex,
              fixed));
    }
    case D9C_COMMAND_RECORD_APPLY_STATE:
      return sink.applyState(record);
    default:
      return kCommandChunkV2DecodeFailure;
  }
}

}  // namespace dxmt9::d3d9
