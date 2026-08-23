#include "d3d9_pe_chunk_builder.hpp"

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
bool appendFixed(CommandChunkBuilder& builder, std::uint32_t type,
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
bool appendTwoHandleFixed(CommandChunkBuilder& builder, std::uint32_t type,
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

bool appendSetConstants(
    CommandChunkBuilder& builder, std::uint32_t type,
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
  const D9CCommandChunkWireSetConst fixed{
      .startRegister = startRegister,
      .registerCount = registerCount,
  };
  if (!builder.appendPayloadValue(fixed) ||
      !builder.appendConstantRecordTail(registerCount, registerBytes) ||
      !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendClear(CommandChunkBuilder& builder,
                   D9CCommandChunkWireClear fixed,
                   std::span<const D9CRect> rects) noexcept {
  if (rects.size() > std::numeric_limits<std::uint32_t>::max() ||
      !builder.beginRecord(D9C_COMMAND_RECORD_CLEAR)) {
    return false;
  }
  fixed.rectCount = static_cast<std::uint32_t>(rects.size());
  fixed.rectOffset = sizeof(fixed);
  if (!builder.appendPayloadValue(fixed) ||
      !builder.appendClearRectTail(rects) ||
      !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendPresent(CommandChunkBuilder& builder,
                     D9CCommandChunkWirePresent fixed,
                     const SurfaceRef& source) noexcept {
  if (!builder.beginRecord(D9C_COMMAND_RECORD_PRESENT)) {
    return false;
  }
  if (!builder.appendHandle(source, D9C_CHUNK_HANDLE_KIND_SURFACE,
                            fixed.sourceHandleIndex) ||
      !builder.appendPayloadValue(fixed) || !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendStretchRect(CommandChunkBuilder& builder,
                         D9CCommandChunkWireStretchRect fixed,
                         const SurfaceRef& src,
                         const SurfaceRef& dst) noexcept {
  fixed.reserved0 = 0u;
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_STRETCH_RECT, fixed, src,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireStretchRect::srcHandleIndex, dst,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireStretchRect::dstHandleIndex);
}

bool appendColorFill(CommandChunkBuilder& builder,
                       D9CCommandChunkWireColorFill fixed,
                       const SurfaceRef& surface) noexcept {
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

bool appendUpdateTexture(CommandChunkBuilder& builder,
                           const TextureRef& src,
                           const TextureRef& dst) noexcept {
  D9CCommandChunkWireUpdateTexture fixed{};
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_UPDATE_TEXTURE, fixed, src,
      D9C_CHUNK_HANDLE_KIND_TEXTURE,
      &D9CCommandChunkWireUpdateTexture::srcHandleIndex, dst,
      D9C_CHUNK_HANDLE_KIND_TEXTURE,
      &D9CCommandChunkWireUpdateTexture::dstHandleIndex);
}

bool appendUpdateSurface(CommandChunkBuilder& builder,
                           D9CCommandChunkWireUpdateSurface fixed,
                           const SurfaceRef& src,
                           const SurfaceRef& dst) noexcept {
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_UPDATE_SURFACE, fixed, src,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireUpdateSurface::srcHandleIndex, dst,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireUpdateSurface::dstHandleIndex);
}

bool appendQueryIssue(CommandChunkBuilder& builder,
                        std::uint32_t flags,
                        const QueryRef& query) noexcept {
  D9CCommandChunkWireQueryIssue fixed{
      .queryHandleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
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

bool appendReadback(CommandChunkBuilder& builder,
                      const SurfaceRef& src,
                      const SurfaceRef& dst) noexcept {
  D9CCommandChunkWireReadback fixed{};
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_READBACK, fixed, src,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireReadback::srcHandleIndex, dst,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireReadback::dstHandleIndex);
}

bool appendReszDepthResolve(CommandChunkBuilder& builder,
                              const SurfaceRef& msaaDepth,
                              const TextureRef& intzDest) noexcept {
  D9CCommandChunkWireReszDepthResolve fixed{};
  return appendTwoHandleFixed(
      builder, D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE, fixed, msaaDepth,
      D9C_CHUNK_HANDLE_KIND_SURFACE,
      &D9CCommandChunkWireReszDepthResolve::msaaDepthHandleIndex, intzDest,
      D9C_CHUNK_HANDLE_KIND_TEXTURE,
      &D9CCommandChunkWireReszDepthResolve::intzDestHandleIndex);
}

}  // namespace dxmt9::d3d9::pe
