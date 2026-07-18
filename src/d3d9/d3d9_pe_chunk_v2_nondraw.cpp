#include "d3d9_pe_chunk_v2_builder.hpp"

#include <limits>

namespace dxmt9::d3d9::pe {

namespace {

std::uint32_t constantElementSize(std::uint32_t type) {
  switch (type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
      return 16u;
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      return 4u;
    default:
      return 0u;
  }
}

std::uint32_t constantRegisterLimit(std::uint32_t type) {
  switch (type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
      return D9C_DRAW_PACKET_MAX_CONST_VS_F;
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
      return D9C_DRAW_PACKET_MAX_CONST_VS_I;
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
      return D9C_DRAW_PACKET_MAX_CONST_VS_B;
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
      return D9C_DRAW_PACKET_MAX_CONST_PS_F;
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
      return D9C_DRAW_PACKET_MAX_CONST_PS_I;
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      return D9C_DRAW_PACKET_MAX_CONST_PS_B;
    default:
      return 0u;
  }
}

template <typename T>
bool appendFixed(CommandChunkV2Builder& builder, std::uint32_t type,
                 const T& fixed) {
  if (!builder.beginRecord(type)) {
    return false;
  }
  if (!builder.appendPayloadValue(fixed) || !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

template <typename T>
bool appendTwoHandleFixed(CommandChunkV2Builder& builder, std::uint32_t type,
                          T fixed, const PeWireObjectRef& first,
                          std::uint32_t firstKind,
                          std::uint32_t T::* firstField,
                          const PeWireObjectRef& second,
                          std::uint32_t secondKind,
                          std::uint32_t T::* secondField) {
  if (!builder.beginRecord(type)) {
    return false;
  }
  if (!builder.appendHandle(first, firstKind, fixed.*firstField) ||
      !builder.appendHandle(second, secondKind, fixed.*secondField) ||
      !builder.appendPayloadValue(fixed) || !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

}  // namespace

bool appendSetConstantsV2(
    CommandChunkV2Builder& builder, std::uint32_t type,
    std::uint32_t startRegister, std::uint32_t registerCount,
    std::span<const std::byte> registerBytes) noexcept {
  const auto elementSize = constantElementSize(type);
  const auto limit = constantRegisterLimit(type);
  const auto expectedBytes = static_cast<std::uint64_t>(registerCount) *
                             elementSize;
  if (elementSize == 0u ||
      static_cast<std::uint64_t>(startRegister) + registerCount > limit ||
      expectedBytes != registerBytes.size() || !builder.beginRecord(type)) {
    return false;
  }
  const D9CCommandChunkWireSetConstV2 fixed{
      .startRegister = startRegister,
      .registerCount = registerCount,
  };
  if (!builder.appendPayloadValue(fixed) ||
      !builder.appendPayload(registerBytes, alignof(std::uint32_t)) ||
      !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendClearV2(CommandChunkV2Builder& builder,
                   D9CCommandChunkWireClearV2 fixed,
                   std::span<const D9CRect> rects) noexcept {
  if (rects.size() > std::numeric_limits<std::uint32_t>::max() ||
      !builder.beginRecord(D9C_COMMAND_RECORD_CLEAR)) {
    return false;
  }
  fixed.rectCount = static_cast<std::uint32_t>(rects.size());
  fixed.rectOffset = sizeof(fixed);
  if (!builder.appendPayloadValue(fixed) ||
      !builder.appendPayload(std::as_bytes(rects), alignof(D9CRect)) ||
      !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendPresentV2(CommandChunkV2Builder& builder,
                     const D9CCommandChunkWirePresentV2& input) noexcept {
  auto fixed = input;
  fixed.reserved0 = 0u;
  return appendFixed(builder, D9C_COMMAND_RECORD_PRESENT, fixed);
}

bool appendStretchRectV2(CommandChunkV2Builder& builder,
                         D9CCommandChunkWireStretchRectV2 fixed,
                         const PeWireObjectRef& src,
                         const PeWireObjectRef& dst) noexcept {
  fixed.reserved0 = 0u;
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_STRETCH_RECT, fixed, src,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireStretchRectV2::srcHandleIndex, dst,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireStretchRectV2::dstHandleIndex);
}

bool appendColorFillV2(CommandChunkV2Builder& builder,
                       D9CCommandChunkWireColorFillV2 fixed,
                       const PeWireObjectRef& surface) noexcept {
  if (!builder.beginRecord(D9C_COMMAND_RECORD_COLOR_FILL)) {
    return false;
  }
  if (!builder.appendHandle(surface, D9C_CHUNK_HANDLE_KIND_SURFACE,
                            fixed.surfaceHandleIndex)) {
    builder.rollbackRecord();
    return false;
  }
  fixed.reserved0 = 0u;
  if (!builder.appendPayloadValue(fixed) || !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendUpdateTextureV2(CommandChunkV2Builder& builder,
                           const PeWireObjectRef& src,
                           const PeWireObjectRef& dst) noexcept {
  D9CCommandChunkWireUpdateTextureV2 fixed{};
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_UPDATE_TEXTURE, fixed, src,
      D9C_CHUNK_HANDLE_KIND_TEXTURE,
      &D9CCommandChunkWireUpdateTextureV2::srcHandleIndex, dst,
      D9C_CHUNK_HANDLE_KIND_TEXTURE,
      &D9CCommandChunkWireUpdateTextureV2::dstHandleIndex);
}

bool appendUpdateSurfaceV2(CommandChunkV2Builder& builder,
                           D9CCommandChunkWireUpdateSurfaceV2 fixed,
                           const PeWireObjectRef& src,
                           const PeWireObjectRef& dst) noexcept {
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_UPDATE_SURFACE, fixed, src,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireUpdateSurfaceV2::srcHandleIndex, dst,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireUpdateSurfaceV2::dstHandleIndex);
}

bool appendQueryIssueV2(CommandChunkV2Builder& builder,
                        std::uint32_t flags,
                        const PeWireObjectRef& query) noexcept {
  D9CCommandChunkWireQueryIssueV2 fixed{
      .queryHandleIndex = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX,
      .flags = flags,
  };
  if (!builder.beginRecord(D9C_COMMAND_RECORD_QUERY_ISSUE)) {
    return false;
  }
  if (!builder.appendHandle(query, D9C_CHUNK_HANDLE_KIND_QUERY,
                            fixed.queryHandleIndex) ||
      !builder.appendPayloadValue(fixed) || !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendReadbackV2(CommandChunkV2Builder& builder,
                      const PeWireObjectRef& src,
                      const PeWireObjectRef& dst) noexcept {
  D9CCommandChunkWireReadbackV2 fixed{};
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_READBACK, fixed, src,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireReadbackV2::srcHandleIndex, dst,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireReadbackV2::dstHandleIndex);
}

bool appendReszDepthResolveV2(CommandChunkV2Builder& builder,
                              const PeWireObjectRef& msaaDepth,
                              const PeWireObjectRef& intzDest) noexcept {
  D9CCommandChunkWireReszDepthResolveV2 fixed{};
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE, fixed, msaaDepth,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireReszDepthResolveV2::msaaDepthHandleIndex, intzDest,
      D9C_CHUNK_HANDLE_KIND_TEXTURE,
      &D9CCommandChunkWireReszDepthResolveV2::intzDestHandleIndex);
}

}  // namespace dxmt9::d3d9::pe
