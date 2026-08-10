#include "device_c_chunk_replay.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt9::d3d9 {

namespace {

template <typename T>
bool loadFixed(const ImportedRecordView& record, T& value) {
  if (record.payload.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&value, record.payload.data(), sizeof(T));
  return true;
}

template <typename T>
void* resolvedObject(const ResolvedRecordView& record,
                     std::uint32_t T::* field, const T& fixed) {
  return record.objectForAbsoluteIndex(fixed.*field);
}

}  // namespace

void* ResolvedRecordView::objectForAbsoluteIndex(
    std::uint32_t index) const noexcept {
  if (index == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX ||
      index < wire.header.firstHandle) {
    return nullptr;
  }
  const auto relative = static_cast<std::size_t>(
      index - wire.header.firstHandle);
  return relative < objects.size() ? objects[relative] : nullptr;
}

ResolvedRecordView ResolvedChunkView::record(
    std::size_t index) const noexcept {
  const auto imported = wire.record(index);
  if (index >= wire.records.size() ||
      imported.header.firstHandle > objects.size() ||
      imported.header.handleCount >
          objects.size() - imported.header.firstHandle) {
    return {};
  }
  return ResolvedRecordView{
      .wire = imported,
      .objects = objects.subspan(imported.header.firstHandle,
                                 imported.header.handleCount),
  };
}

bool isNonDrawRecord(std::uint32_t type) noexcept {
  const auto* rule = recordRule(type);
  return rule && (rule->ruleFlags & RecordRuleDraw) == 0u;
}

std::int32_t replayNonDrawRecord(
    const ResolvedRecordView& record,
    NonDrawReplaySink& sink) noexcept {
  if (!isNonDrawRecord(record.wire.header.type) ||
      record.objects.size() != record.wire.header.handleCount ||
      std::any_of(record.objects.begin(), record.objects.end(),
                  [](const void* object) { return object == nullptr; })) {
    return kCommandChunkDecodeFailure;
  }

  switch (record.wire.header.type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B: {
      D9CCommandChunkWireSetConst fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkDecodeFailure;
      }
      return sink.setConstants(
          record.wire.header.type, fixed,
          record.wire.payload.subspan(sizeof(fixed)));
    }
    case D9C_COMMAND_RECORD_CLEAR: {
      D9CCommandChunkWireClear fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkDecodeFailure;
      }
      const auto* rects = reinterpret_cast<const D9CRect*>(
          record.wire.payload.data() + fixed.rectOffset);
      return sink.clear(fixed,
                        std::span<const D9CRect>(rects, fixed.rectCount));
    }
    case D9C_COMMAND_RECORD_PRESENT: {
      D9CCommandChunkWirePresent fixed{};
      return loadFixed(record.wire, fixed)
                 ? sink.present(fixed)
                 : kCommandChunkDecodeFailure;
    }
    case D9C_COMMAND_RECORD_STRETCH_RECT: {
      D9CCommandChunkWireStretchRect fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkDecodeFailure;
      }
      return sink.stretchRect(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireStretchRect::
                                    srcHandleIndex,
                                fixed),
          resolvedObject(record,
                         &D9CCommandChunkWireStretchRect::dstHandleIndex,
                         fixed));
    }
    case D9C_COMMAND_RECORD_COLOR_FILL: {
      D9CCommandChunkWireColorFill fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkDecodeFailure;
      }
      return sink.colorFill(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireColorFill::
                                    surfaceHandleIndex,
                                fixed));
    }
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
      D9CCommandChunkWireUpdateTexture fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkDecodeFailure;
      }
      return sink.updateTexture(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireUpdateTexture::
                                    srcHandleIndex,
                                fixed),
          resolvedObject(record,
                         &D9CCommandChunkWireUpdateTexture::dstHandleIndex,
                         fixed));
    }
    case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
      D9CCommandChunkWireUpdateSurface fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkDecodeFailure;
      }
      return sink.updateSurface(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireUpdateSurface::
                                    srcHandleIndex,
                                fixed),
          resolvedObject(record,
                         &D9CCommandChunkWireUpdateSurface::dstHandleIndex,
                         fixed));
    }
    case D9C_COMMAND_RECORD_QUERY_ISSUE: {
      D9CCommandChunkWireQueryIssue fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkDecodeFailure;
      }
      return sink.queryIssue(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireQueryIssue::
                                    queryHandleIndex,
                                fixed));
    }
    case D9C_COMMAND_RECORD_READBACK: {
      D9CCommandChunkWireReadback fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkDecodeFailure;
      }
      return sink.readback(
          fixed, resolvedObject(record,
                                &D9CCommandChunkWireReadback::srcHandleIndex,
                                fixed),
          resolvedObject(record,
                         &D9CCommandChunkWireReadback::dstHandleIndex,
                         fixed));
    }
    case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE: {
      D9CCommandChunkWireReszDepthResolve fixed{};
      if (!loadFixed(record.wire, fixed)) {
        return kCommandChunkDecodeFailure;
      }
      return sink.reszDepthResolve(
          fixed,
          resolvedObject(
              record,
              &D9CCommandChunkWireReszDepthResolve::msaaDepthHandleIndex,
              fixed),
          resolvedObject(
              record,
              &D9CCommandChunkWireReszDepthResolve::intzDestHandleIndex,
              fixed));
    }
    case D9C_COMMAND_RECORD_APPLY_STATE:
      return sink.applyState(record);
    default:
      return kCommandChunkDecodeFailure;
  }
}

}  // namespace dxmt9::d3d9
