#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

template <typename T>
void checkTriviallyCopyable(std::string_view name) {
  if constexpr (!std::is_trivially_copyable_v<T>) {
    fail(std::string(name) + " must remain trivially copyable");
  }
}

template <typename T>
void checkStandardLayout(std::string_view name) {
  if constexpr (!std::is_standard_layout_v<T>) {
    fail(std::string(name) + " must remain standard layout");
  }
}

template <typename T>
void checkPodWireShape(std::string_view name) {
  checkTriviallyCopyable<T>(name);
  checkStandardLayout<T>(name);
}

template <typename T>
void checkSizeAlign(std::string_view name,
                    std::size_t expectedSize,
                    std::size_t expectedAlign) {
  checkEq(sizeof(T), expectedSize, std::string(name) + " byte size");
  checkEq(alignof(T), expectedAlign, std::string(name) + " alignment");
}

std::size_t expectedSetConstBytes(std::size_t elemSize, std::uint32_t count) {
  return sizeof(D9CCommandRecordSetConst) + elemSize * count;
}

std::size_t expectedClearBytes(std::uint32_t rectCount) {
  return sizeof(D9CCommandRecordClear) + sizeof(D9CRect) * rectCount;
}

void testWireRecordsStayPod() {
  checkPodWireShape<D9CWireHandle>("D9CWireHandle");
  checkPodWireShape<D9CDrawPacketRenderState>("D9CDrawPacketRenderState");
  checkPodWireShape<D9CDrawPacketTextureStageState>("D9CDrawPacketTextureStageState");
  checkPodWireShape<D9CDrawPacketSamplerState>("D9CDrawPacketSamplerState");
  checkPodWireShape<D9CDrawPacketTransform>("D9CDrawPacketTransform");
  checkPodWireShape<D9CDrawPacketStreamSource>("D9CDrawPacketStreamSource");
  checkPodWireShape<D9CDrawPrimitivePacket>("D9CDrawPrimitivePacket");
  checkPodWireShape<D9CDrawIndexedPrimitivePacket>("D9CDrawIndexedPrimitivePacket");
  checkPodWireShape<D9CDrawPrimitiveUPPacket>("D9CDrawPrimitiveUPPacket");
  checkPodWireShape<D9CDrawIndexedPrimitiveUPPacket>("D9CDrawIndexedPrimitiveUPPacket");
  checkPodWireShape<D9CCommandChunkWireHeader>("D9CCommandChunkWireHeader");
  checkPodWireShape<D9CCommandChunkWireRecordHeader>("D9CCommandChunkWireRecordHeader");
  checkPodWireShape<D9CCommandChunkWireHandleEntry>("D9CCommandChunkWireHandleEntry");
  checkPodWireShape<D9CCommandChunkWirePayloadSlice>("D9CCommandChunkWirePayloadSlice");
  checkPodWireShape<D9CCommandChunkWireHandleRange>("D9CCommandChunkWireHandleRange");
  checkPodWireShape<D9CCommandChunkWireRecordRanges>("D9CCommandChunkWireRecordRanges");
  checkPodWireShape<D9CCommandRecordHeader>("D9CCommandRecordHeader");
  checkPodWireShape<D9CCommandRecordDrawPrimitive>("D9CCommandRecordDrawPrimitive");
  checkPodWireShape<D9CCommandRecordDrawIndexedPrimitive>("D9CCommandRecordDrawIndexedPrimitive");
  checkPodWireShape<D9CCommandRecordDrawPrimitiveUP>("D9CCommandRecordDrawPrimitiveUP");
  checkPodWireShape<D9CCommandRecordDrawIndexedPrimitiveUP>("D9CCommandRecordDrawIndexedPrimitiveUP");
  checkPodWireShape<D9CCommandRecordSetConst>("D9CCommandRecordSetConst");
  checkPodWireShape<D9CCommandRecordClear>("D9CCommandRecordClear");
  checkPodWireShape<D9CCommandRecordPresent>("D9CCommandRecordPresent");
  checkPodWireShape<D9CCommandRecordStretchRect>("D9CCommandRecordStretchRect");
  checkPodWireShape<D9CCommandRecordColorFill>("D9CCommandRecordColorFill");
  checkPodWireShape<D9CCommandRecordUpdateTexture>("D9CCommandRecordUpdateTexture");
  checkPodWireShape<D9CCommandRecordUpdateSurface>("D9CCommandRecordUpdateSurface");
  checkPodWireShape<D9CCommandRecordQueryIssue>("D9CCommandRecordQueryIssue");
  checkPodWireShape<D9CCommandRecordReadback>("D9CCommandRecordReadback");
  checkPodWireShape<D9CCommandRecordReszDepthResolve>("D9CCommandRecordReszDepthResolve");
  checkPodWireShape<D9CCommandRecordApplyState>("D9CCommandRecordApplyState");
  checkPodWireShape<D9CCommandChunk>("D9CCommandChunk");
  checkPodWireShape<D9CChunkHandleEntry>("D9CChunkHandleEntry");
}

void testCommandRecordIds() {
  checkEq(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, 1, "draw primitive command ID");
  checkEq(D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, 2,
          "draw indexed primitive command ID");
  checkEq(D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP, 3,
          "draw primitive UP command ID");
  checkEq(D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP, 4,
          "draw indexed primitive UP command ID");
  checkEq(D9C_COMMAND_RECORD_SET_VS_CONST_F, 14, "VS const F command ID");
  checkEq(D9C_COMMAND_RECORD_SET_VS_CONST_I, 15, "VS const I command ID");
  checkEq(D9C_COMMAND_RECORD_SET_VS_CONST_B, 16, "VS const B command ID");
  checkEq(D9C_COMMAND_RECORD_SET_PS_CONST_F, 17, "PS const F command ID");
  checkEq(D9C_COMMAND_RECORD_SET_PS_CONST_I, 18, "PS const I command ID");
  checkEq(D9C_COMMAND_RECORD_SET_PS_CONST_B, 19, "PS const B command ID");
  checkEq(D9C_COMMAND_RECORD_CLEAR, 20, "clear command ID");
  checkEq(D9C_COMMAND_RECORD_PRESENT, 21, "present command ID");
  checkEq(D9C_COMMAND_RECORD_STRETCH_RECT, 22, "stretch rect command ID");
  checkEq(D9C_COMMAND_RECORD_COLOR_FILL, 23, "color fill command ID");
  checkEq(D9C_COMMAND_RECORD_UPDATE_TEXTURE, 24, "update texture command ID");
  checkEq(D9C_COMMAND_RECORD_UPDATE_SURFACE, 25, "update surface command ID");
  checkEq(D9C_COMMAND_RECORD_QUERY_ISSUE, 26, "query issue command ID");
  checkEq(D9C_COMMAND_RECORD_READBACK, 27, "readback command ID");
  checkEq(D9C_COMMAND_RECORD_APPLY_STATE, 28, "apply state command ID");
  checkEq(D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE, 29,
          "RESZ depth-resolve command ID");
}

void testRecordHeaderLayout() {
  checkEq(D9C_COMMAND_CHUNK_VERSION, 1u, "command chunk ABI version");
  checkEq(D9C_COMMAND_CHUNK_WIRE_VERSION, 1u, "DOD command chunk ABI version");
  checkSizeAlign<D9CCommandRecordHeader>("D9CCommandRecordHeader", 8u, 4u);
  checkEq(offsetof(D9CCommandRecordHeader, type), std::size_t{0},
          "record header type offset");
  checkEq(offsetof(D9CCommandRecordHeader, size), std::size_t{4},
          "record header size offset");

  checkSizeAlign<D9CWireHandle>("D9CWireHandle", 8u, 4u);
  checkEq(offsetof(D9CWireHandle, lo), std::size_t{0}, "wire handle low offset");
  checkEq(offsetof(D9CWireHandle, hi), std::size_t{4}, "wire handle high offset");

  checkEq(offsetof(D9CCommandRecordDrawPrimitive, header), std::size_t{0}, "draw primitive header offset");
  checkEq(offsetof(D9CCommandRecordDrawPrimitive, packet), std::size_t{8},
          "draw primitive packet offset");
  checkEq(offsetof(D9CCommandRecordDrawIndexedPrimitive, header), std::size_t{0}, "draw indexed header offset");
  checkEq(offsetof(D9CCommandRecordDrawIndexedPrimitive, packet), std::size_t{8},
          "draw indexed packet offset");
  checkEq(offsetof(D9CCommandRecordDrawPrimitiveUP, header), std::size_t{0},
          "draw primitive UP header offset");
  checkEq(offsetof(D9CCommandRecordDrawPrimitiveUP, packet), std::size_t{8},
          "draw primitive UP packet offset");
  checkEq(offsetof(D9CCommandRecordDrawIndexedPrimitiveUP, header), std::size_t{0},
          "draw indexed primitive UP header offset");
  checkEq(offsetof(D9CCommandRecordDrawIndexedPrimitiveUP, packet), std::size_t{8},
          "draw indexed primitive UP packet offset");
  checkEq(offsetof(D9CCommandRecordClear, header), std::size_t{0}, "clear header offset");
  checkEq(offsetof(D9CCommandRecordPresent, header), std::size_t{0}, "present header offset");
  checkEq(offsetof(D9CCommandRecordStretchRect, header), std::size_t{0},
          "stretch rect header offset");
  checkEq(offsetof(D9CCommandRecordColorFill, header), std::size_t{0},
          "color fill header offset");
  checkEq(offsetof(D9CCommandRecordUpdateTexture, header), std::size_t{0},
          "update texture header offset");
  checkEq(offsetof(D9CCommandRecordUpdateSurface, header), std::size_t{0},
          "update surface header offset");
  checkEq(offsetof(D9CCommandRecordQueryIssue, header), std::size_t{0},
          "query issue header offset");
  checkEq(offsetof(D9CCommandRecordReadback, header), std::size_t{0},
          "readback header offset");
  checkEq(offsetof(D9CCommandRecordReszDepthResolve, header), std::size_t{0},
          "RESZ depth-resolve header offset");
  checkEq(offsetof(D9CCommandRecordApplyState, header), std::size_t{0}, "apply state header offset");
  check(sizeof(D9CCommandRecordHeader) <= sizeof(D9CCommandRecordDrawPrimitive),
        "fixed records contain the common header");
}

void testDrawPacketLayouts() {
  checkSizeAlign<D9CDrawPacketRenderState>("D9CDrawPacketRenderState", 8u, 4u);
  checkSizeAlign<D9CDrawPacketTextureStageState>(
      "D9CDrawPacketTextureStageState", 12u, 4u);
  checkSizeAlign<D9CDrawPacketSamplerState>("D9CDrawPacketSamplerState", 12u, 4u);
  checkSizeAlign<D9CDrawPacketTransform>("D9CDrawPacketTransform", 72u, 4u);
  checkSizeAlign<D9CDrawPacketStreamSource>("D9CDrawPacketStreamSource", 16u, 4u);

  checkSizeAlign<D9CDrawPrimitivePacket>("D9CDrawPrimitivePacket", 4808u, 4u);
  checkEq(offsetof(D9CDrawPrimitivePacket, renderStateCount), std::size_t{0},
          "draw packet render-state count offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, textureMask), std::size_t{516},
          "draw packet texture mask offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, streamSourceMask), std::size_t{680},
          "draw packet stream-source mask offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, fvfValid), std::size_t{940},
          "draw packet FVF valid offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, vsValid), std::size_t{948},
          "draw packet VS valid offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, psValid), std::size_t{960},
          "draw packet PS valid offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, vdeclValid), std::size_t{972},
          "draw packet vertex declaration valid offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, rtMask), std::size_t{984},
          "draw packet render-target mask offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, dsValid), std::size_t{1020},
          "draw packet depth-stencil valid offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, viewportValid), std::size_t{1032},
          "draw packet viewport valid offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, scissorValid), std::size_t{1060},
          "draw packet scissor valid offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, tssCount), std::size_t{1080},
          "draw packet texture-stage-state count offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, samplerStateCount), std::size_t{1852},
          "draw packet sampler-state count offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, materialValid), std::size_t{2624},
          "draw packet material valid offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, clipPlaneMask), std::size_t{2696},
          "draw packet clip-plane mask offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, transformCount), std::size_t{2796},
          "draw packet transform count offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, lightSlotMask), std::size_t{3952},
          "draw packet light slot mask offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, primitiveType), std::size_t{4796},
          "draw packet primitive type offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, startVertex), std::size_t{4800},
          "draw packet start vertex offset");
  checkEq(offsetof(D9CDrawPrimitivePacket, primitiveCount), std::size_t{4804},
          "draw packet primitive count offset");

  checkSizeAlign<D9CDrawIndexedPrimitivePacket>(
      "D9CDrawIndexedPrimitivePacket", 4840u, 4u);
  checkEq(offsetof(D9CDrawIndexedPrimitivePacket, state), std::size_t{0},
          "indexed draw state offset");
  checkEq(offsetof(D9CDrawIndexedPrimitivePacket, baseVertex), std::size_t{4808},
          "indexed draw base vertex offset");
  checkEq(offsetof(D9CDrawIndexedPrimitivePacket, primitiveCount), std::size_t{4824},
          "indexed draw primitive count offset");
  checkEq(offsetof(D9CDrawIndexedPrimitivePacket, ibValid), std::size_t{4828},
          "indexed draw IB valid offset");
  checkEq(offsetof(D9CDrawIndexedPrimitivePacket, ibHandle), std::size_t{4832},
          "indexed draw IB handle offset");

  checkSizeAlign<D9CDrawPrimitiveUPPacket>("D9CDrawPrimitiveUPPacket", 4824u, 4u);
  checkEq(offsetof(D9CDrawPrimitiveUPPacket, primitiveCount), std::size_t{4808},
          "draw primitive UP count offset");
  checkEq(offsetof(D9CDrawPrimitiveUPPacket, stride), std::size_t{4812},
          "draw primitive UP stride offset");
  checkEq(offsetof(D9CDrawPrimitiveUPPacket, vertexDataOffset), std::size_t{4816},
          "draw primitive UP vertex data offset field");
  checkEq(offsetof(D9CDrawPrimitiveUPPacket, vertexDataSize), std::size_t{4820},
          "draw primitive UP vertex data size field");

  checkSizeAlign<D9CDrawIndexedPrimitiveUPPacket>(
      "D9CDrawIndexedPrimitiveUPPacket", 4844u, 4u);
  checkEq(offsetof(D9CDrawIndexedPrimitiveUPPacket, minVertex), std::size_t{4808},
          "indexed UP min vertex offset");
  checkEq(offsetof(D9CDrawIndexedPrimitiveUPPacket, numVertices), std::size_t{4812},
          "indexed UP vertex count offset");
  checkEq(offsetof(D9CDrawIndexedPrimitiveUPPacket, primitiveCount), std::size_t{4816},
          "indexed UP primitive count offset");
  checkEq(offsetof(D9CDrawIndexedPrimitiveUPPacket, indexFormat), std::size_t{4820},
          "indexed UP index format offset");
  checkEq(offsetof(D9CDrawIndexedPrimitiveUPPacket, indexDataOffset), std::size_t{4828},
          "indexed UP index data offset field");
  checkEq(offsetof(D9CDrawIndexedPrimitiveUPPacket, vertexDataOffset), std::size_t{4836},
          "indexed UP vertex data offset field");
}

void testCommandRecordLayouts() {
  checkSizeAlign<D9CCommandRecordDrawPrimitive>(
      "D9CCommandRecordDrawPrimitive", 4816u, 4u);
  checkSizeAlign<D9CCommandRecordDrawIndexedPrimitive>(
      "D9CCommandRecordDrawIndexedPrimitive", 4848u, 4u);
  checkSizeAlign<D9CCommandRecordDrawPrimitiveUP>(
      "D9CCommandRecordDrawPrimitiveUP", 4832u, 4u);
  checkSizeAlign<D9CCommandRecordDrawIndexedPrimitiveUP>(
      "D9CCommandRecordDrawIndexedPrimitiveUP", 4852u, 4u);
  checkSizeAlign<D9CCommandRecordSetConst>("D9CCommandRecordSetConst", 16u, 4u);
  checkSizeAlign<D9CCommandRecordClear>("D9CCommandRecordClear", 32u, 4u);
  checkSizeAlign<D9CCommandRecordPresent>(
      "D9CCommandRecordPresent", 64u, alignof(std::uint64_t));
  checkSizeAlign<D9CCommandRecordStretchRect>(
      "D9CCommandRecordStretchRect", 72u, alignof(std::uint64_t));
  checkSizeAlign<D9CCommandRecordColorFill>(
      "D9CCommandRecordColorFill", 40u, alignof(std::uint64_t));
  checkSizeAlign<D9CCommandRecordUpdateTexture>(
      "D9CCommandRecordUpdateTexture", 24u, alignof(std::uint64_t));
  checkSizeAlign<D9CCommandRecordUpdateSurface>(
      "D9CCommandRecordUpdateSurface", 64u, alignof(std::uint64_t));
  checkSizeAlign<D9CCommandRecordQueryIssue>(
      "D9CCommandRecordQueryIssue", 24u, alignof(std::uint64_t));
  checkSizeAlign<D9CCommandRecordReadback>(
      "D9CCommandRecordReadback", 24u, alignof(std::uint64_t));
  // RESZ depth-resolve mirrors Readback's two-handle shape exactly.
  checkSizeAlign<D9CCommandRecordReszDepthResolve>(
      "D9CCommandRecordReszDepthResolve", 24u, alignof(std::uint64_t));
  checkSizeAlign<D9CCommandRecordApplyState>("D9CCommandRecordApplyState", 4816u, 4u);
  checkSizeAlign<D9CCommandChunk>("D9CCommandChunk", 32u, 4u);
  checkSizeAlign<D9CChunkHandleEntry>(
      "D9CChunkHandleEntry", 16u, alignof(std::uint64_t));

  checkEq(offsetof(D9CCommandRecordSetConst, start), std::size_t{8},
          "set const start offset");
  checkEq(offsetof(D9CCommandRecordSetConst, count), std::size_t{12},
          "set const count offset");
  checkEq(offsetof(D9CCommandRecordClear, flags), std::size_t{8},
          "clear flags offset");
  checkEq(offsetof(D9CCommandRecordClear, rectCount), std::size_t{24},
          "clear rect count offset");
  checkEq(offsetof(D9CCommandRecordClear, rectOffset), std::size_t{28},
          "clear rect offset field");
  checkEq(offsetof(D9CCommandRecordPresent, hwnd), std::size_t{8},
          "present hwnd offset");
  checkEq(offsetof(D9CCommandRecordPresent, src), std::size_t{32},
          "present src rect offset");
  checkEq(offsetof(D9CCommandRecordPresent, dst), std::size_t{48},
          "present dst rect offset");
  checkEq(offsetof(D9CCommandRecordStretchRect, srcWire), std::size_t{8},
          "stretch rect source wire offset");
  checkEq(offsetof(D9CCommandRecordStretchRect, dstWire), std::size_t{16},
          "stretch rect destination wire offset");
  checkEq(offsetof(D9CCommandRecordStretchRect, hasSrcRect), std::size_t{24},
          "stretch rect source flag offset");
  checkEq(offsetof(D9CCommandRecordStretchRect, filter), std::size_t{32},
          "stretch rect filter offset");
  checkEq(offsetof(D9CCommandRecordStretchRect, srcRect), std::size_t{40},
          "stretch rect source rect offset");
  checkEq(offsetof(D9CCommandRecordStretchRect, dstRect), std::size_t{56},
          "stretch rect destination rect offset");
  checkEq(offsetof(D9CCommandRecordColorFill, surfaceWire), std::size_t{8},
          "color fill surface wire offset");
  checkEq(offsetof(D9CCommandRecordColorFill, rect), std::size_t{24},
          "color fill rect offset");
  checkEq(offsetof(D9CCommandRecordUpdateTexture, srcWire), std::size_t{8},
          "update texture source wire offset");
  checkEq(offsetof(D9CCommandRecordUpdateTexture, dstWire), std::size_t{16},
          "update texture destination wire offset");
  checkEq(offsetof(D9CCommandRecordUpdateSurface, srcWire), std::size_t{8},
          "update surface source wire offset");
  checkEq(offsetof(D9CCommandRecordUpdateSurface, hasSrcRect), std::size_t{24},
          "update surface source rect flag offset");
  checkEq(offsetof(D9CCommandRecordUpdateSurface, srcRect), std::size_t{32},
          "update surface source rect offset");
  checkEq(offsetof(D9CCommandRecordUpdateSurface, dstPoint), std::size_t{48},
          "update surface destination point offset");
  checkEq(offsetof(D9CCommandRecordQueryIssue, queryWire), std::size_t{8},
          "query issue query wire offset");
  checkEq(offsetof(D9CCommandRecordQueryIssue, flags), std::size_t{16},
          "query issue flags offset");
  checkEq(offsetof(D9CCommandRecordReadback, srcWire), std::size_t{8},
          "readback source wire offset");
  checkEq(offsetof(D9CCommandRecordReszDepthResolve, msaaDepthHandle),
          std::size_t{8}, "RESZ depth-resolve MSAA depth handle offset");
  checkEq(offsetof(D9CCommandRecordReszDepthResolve, intzDestHandle),
          std::size_t{16}, "RESZ depth-resolve INTZ dest handle offset");
  checkEq(offsetof(D9CCommandRecordApplyState, packet), std::size_t{8},
          "apply state packet offset");
  checkEq(offsetof(D9CCommandChunk, records), std::size_t{12},
          "legacy chunk record handle offset");
  checkEq(offsetof(D9CCommandChunk, handles), std::size_t{24},
          "legacy chunk handle-list handle offset");
}

void testDodWireChunkLayout() {
  checkEq(sizeof(D9CCommandChunkWireHeader),
          static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE),
          "DOD chunk header byte size");
  checkEq(alignof(D9CCommandChunkWireHeader), std::size_t{4},
          "DOD chunk header alignment");
  checkEq(sizeof(D9CCommandChunkWireRecordHeader),
          static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE),
          "DOD record header byte size");
  checkEq(alignof(D9CCommandChunkWireRecordHeader), std::size_t{4},
          "DOD record header alignment");
  checkEq(sizeof(D9CCommandChunkWireHandleEntry),
          static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE),
          "DOD handle entry byte size");
  checkEq(alignof(D9CCommandChunkWireHandleEntry), alignof(std::uint64_t),
          "DOD handle entry alignment");
  checkEq(sizeof(D9CCommandChunkWirePayloadSlice),
          static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_PAYLOAD_SLICE_SIZE),
          "DOD payload slice byte size");
  checkEq(alignof(D9CCommandChunkWirePayloadSlice), std::size_t{4},
          "DOD payload slice alignment");
  checkEq(sizeof(D9CCommandChunkWireHandleRange),
          static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_HANDLE_RANGE_SIZE),
          "DOD handle range byte size");
  checkEq(alignof(D9CCommandChunkWireHandleRange), std::size_t{4},
          "DOD handle range alignment");
  checkEq(sizeof(D9CCommandChunkWireRecordRanges),
          static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_RECORD_RANGES_SIZE),
          "DOD record ranges byte size");
  checkEq(alignof(D9CCommandChunkWireRecordRanges), std::size_t{4},
          "DOD record ranges alignment");

  checkEq(offsetof(D9CCommandChunkWireHeader, version), std::size_t{0},
          "DOD chunk header version offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, headerSize), std::size_t{4},
          "DOD chunk header size offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, recordHeaderSize), std::size_t{8},
          "DOD chunk record header size offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, handleEntrySize), std::size_t{12},
          "DOD chunk handle entry size offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, recordTableOffset), std::size_t{16},
          "DOD chunk header record table offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, recordCount), std::size_t{20},
          "DOD chunk record count offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, handleTableOffset), std::size_t{24},
          "DOD chunk header handle table offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, handleCount), std::size_t{28},
          "DOD chunk handle count offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, payloadArenaOffset), std::size_t{32},
          "DOD chunk header payload arena offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, payloadArenaSize), std::size_t{36},
          "DOD chunk payload arena size offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, reserved0), std::size_t{40},
          "DOD chunk reserved0 offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, reserved1), std::size_t{44},
          "DOD chunk reserved1 offset");

  checkEq(offsetof(D9CCommandChunkWireRecordHeader, type), std::size_t{0},
          "DOD record type field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, flags), std::size_t{4},
          "DOD record flags field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, payloadOffset), std::size_t{8},
          "DOD record payload offset field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, payloadSize), std::size_t{12},
          "DOD record payload size field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, firstHandle), std::size_t{16},
          "DOD record first handle field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, handleCount), std::size_t{20},
          "DOD record handle count field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, reserved0), std::size_t{24},
          "DOD record reserved0 offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, reserved1), std::size_t{28},
          "DOD record reserved1 offset");

  checkEq(offsetof(D9CCommandChunkWireHandleEntry, kind), std::size_t{0},
          "DOD handle kind field offset");
  checkEq(offsetof(D9CCommandChunkWireHandleEntry, generation), std::size_t{4},
          "DOD handle generation field offset");
  checkEq(offsetof(D9CCommandChunkWireHandleEntry, opaqueHandle), std::size_t{8},
          "DOD opaque handle field offset");
  checkEq(offsetof(D9CCommandChunkWireHandleEntry, reserved0), std::size_t{16},
          "DOD handle reserved field offset");

  checkEq(offsetof(D9CCommandChunkWirePayloadSlice, payloadOffset), std::size_t{0},
          "DOD payload slice offset field offset");
  checkEq(offsetof(D9CCommandChunkWirePayloadSlice, payloadSize), std::size_t{4},
          "DOD payload slice size field offset");
  checkEq(offsetof(D9CCommandChunkWireHandleRange, firstHandle), std::size_t{0},
          "DOD handle range first field offset");
  checkEq(offsetof(D9CCommandChunkWireHandleRange, handleCount), std::size_t{4},
          "DOD handle range count field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordRanges, payload), std::size_t{0},
          "DOD record ranges payload descriptor offset");
  checkEq(offsetof(D9CCommandChunkWireRecordRanges, handles), std::size_t{8},
          "DOD record ranges handle descriptor offset");
}

void testDodWireDefaultsAndPayloadRanges() {
  D9CCommandChunkWireHeader chunk{};
  checkEq(chunk.reserved0, 0u, "DOD chunk reserved0 defaults to zero");
  checkEq(chunk.reserved1, 0u, "DOD chunk reserved1 defaults to zero");
  check(d9c_command_chunk_wire_header_reserved_valid(&chunk),
        "DOD chunk reserved helper accepts defaults");

  D9CCommandChunkWireRecordHeader record{};
  checkEq(record.flags, D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          "DOD record flags default to none");
  checkEq(record.reserved0, 0u, "DOD record reserved0 defaults to zero");
  checkEq(record.reserved1, 0u, "DOD record reserved1 defaults to zero");
  check(d9c_command_chunk_wire_record_reserved_valid(&record),
        "DOD record reserved helper accepts defaults");

  D9CCommandChunkWireHandleEntry handle{};
  checkEq(handle.generation, D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          "DOD handle generation defaults to none");
  checkEq(handle.reserved0, 0u, "DOD handle reserved0 defaults to zero");
  checkEq(handle.reserved1, 0u, "DOD handle reserved1 defaults to zero");
  check(d9c_command_chunk_wire_handle_entry_reserved_valid(&handle),
        "DOD handle reserved helper accepts defaults");

  D9CCommandChunkWirePayloadSlice slice{};
  checkEq(slice.payloadOffset, 0u, "DOD payload slice offset defaults to zero");
  checkEq(slice.payloadSize, 0u, "DOD payload slice size defaults to zero");

  D9CCommandChunkWireHandleRange range{};
  checkEq(range.firstHandle, 0u, "DOD handle range first defaults to zero");
  checkEq(range.handleCount, 0u, "DOD handle range count defaults to zero");

  D9CCommandChunkWireRecordRanges ranges{};
  checkEq(ranges.payload.payloadOffset, 0u, "DOD record ranges payload offset defaults to zero");
  checkEq(ranges.payload.payloadSize, 0u, "DOD record ranges payload size defaults to zero");
  checkEq(ranges.handles.firstHandle, 0u, "DOD record ranges first handle defaults to zero");
  checkEq(ranges.handles.handleCount, 0u, "DOD record ranges handle count defaults to zero");

  check(d9c_command_chunk_wire_payload_range_valid(16u, 0u, 16u),
        "DOD payload helper accepts full arena");
  check(d9c_command_chunk_wire_payload_range_valid(16u, 8u, 8u),
        "DOD payload helper accepts tail range");
  check(!d9c_command_chunk_wire_payload_range_valid(16u, 17u, 0u),
        "DOD payload helper rejects offset past arena");
  check(!d9c_command_chunk_wire_payload_range_valid(16u, 12u, 5u),
        "DOD payload helper rejects range past arena");
  check(!d9c_command_chunk_wire_payload_range_valid(0xffffffffu, 0xfffffff0u, 0x20u),
        "DOD payload helper rejects overflowing range");

  chunk.reserved1 = 1u;
  check(!d9c_command_chunk_wire_header_reserved_valid(&chunk),
        "DOD chunk reserved helper rejects nonzero reserved field");
  record.reserved0 = 1u;
  check(!d9c_command_chunk_wire_record_reserved_valid(&record),
        "DOD record reserved helper rejects nonzero reserved field");
  handle.reserved1 = 1u;
  check(!d9c_command_chunk_wire_handle_entry_reserved_valid(&handle),
        "DOD handle reserved helper rejects nonzero reserved field");
}

void testDodWireHandleRangesAndRecordRanges() {
  check(d9c_command_chunk_wire_handle_range_valid(4u, 0u, 4u),
        "DOD handle helper accepts full handle table");
  check(d9c_command_chunk_wire_handle_range_valid(4u, 4u, 0u),
        "DOD handle helper accepts empty range at table end");
  check(d9c_command_chunk_wire_handle_range_valid(4u, 2u, 2u),
        "DOD handle helper accepts tail handle range");
  check(!d9c_command_chunk_wire_handle_range_valid(4u, 5u, 0u),
        "DOD handle helper rejects first handle past table");
  check(!d9c_command_chunk_wire_handle_range_valid(4u, 3u, 2u),
        "DOD handle helper rejects handle range past table");
  check(!d9c_command_chunk_wire_handle_range_valid(0xffffffffu, 0xfffffff0u, 0x20u),
        "DOD handle helper rejects overflowing range");

  D9CCommandChunkWireHeader chunk{};
  chunk.payloadArenaSize = 64u;
  chunk.handleCount = 8u;

  D9CCommandChunkWireRecordHeader record{};
  record.payloadOffset = 16u;
  record.payloadSize = 48u;
  record.firstHandle = 2u;
  record.handleCount = 6u;

  check(d9c_command_chunk_wire_record_ranges_valid(
            chunk.payloadArenaSize, chunk.handleCount, &record),
        "DOD record range helper accepts payload and handle boundaries");
  check(d9c_command_chunk_wire_record_ranges_valid_for_chunk(&chunk, &record),
        "DOD record range helper accepts chunk descriptor");

  const auto payload = d9c_command_chunk_wire_record_payload_slice(&record);
  checkEq(payload.payloadOffset, 16u, "DOD payload slice exposes record offset");
  checkEq(payload.payloadSize, 48u, "DOD payload slice exposes record size");

  const auto handles = d9c_command_chunk_wire_record_handle_range(&record);
  checkEq(handles.firstHandle, 2u, "DOD handle range exposes record first handle");
  checkEq(handles.handleCount, 6u, "DOD handle range exposes record handle count");

  const auto ranges = d9c_command_chunk_wire_record_ranges(&record);
  checkEq(ranges.payload.payloadOffset, 16u, "DOD record ranges exposes payload offset");
  checkEq(ranges.payload.payloadSize, 48u, "DOD record ranges exposes payload size");
  checkEq(ranges.handles.firstHandle, 2u, "DOD record ranges exposes first handle");
  checkEq(ranges.handles.handleCount, 6u, "DOD record ranges exposes handle count");

  record.payloadOffset = 64u;
  record.payloadSize = 0u;
  record.firstHandle = 8u;
  record.handleCount = 0u;
  check(d9c_command_chunk_wire_record_ranges_valid_for_chunk(&chunk, &record),
        "DOD record range helper accepts empty payload and handles at table ends");

  record.payloadOffset = 65u;
  check(!d9c_command_chunk_wire_record_ranges_valid_for_chunk(&chunk, &record),
        "DOD record range helper rejects payload offset past arena");

  record.payloadOffset = 0u;
  record.payloadSize = 65u;
  check(!d9c_command_chunk_wire_record_ranges_valid_for_chunk(&chunk, &record),
        "DOD record range helper rejects payload size past arena");

  record.payloadSize = 0u;
  record.firstHandle = 9u;
  check(!d9c_command_chunk_wire_record_ranges_valid_for_chunk(&chunk, &record),
        "DOD record range helper rejects first handle past table");

  record.firstHandle = 7u;
  record.handleCount = 2u;
  check(!d9c_command_chunk_wire_record_ranges_valid_for_chunk(&chunk, &record),
        "DOD record range helper rejects handle range past table");

  record.firstHandle = 0xfffffff0u;
  record.handleCount = 0x20u;
  chunk.handleCount = 0xffffffffu;
  check(!d9c_command_chunk_wire_record_ranges_valid_for_chunk(&chunk, &record),
        "DOD record range helper rejects overflowing handle range");

  check(!d9c_command_chunk_wire_record_ranges_valid_for_chunk(&chunk, nullptr),
        "DOD record range helper rejects null record");
  check(!d9c_command_chunk_wire_record_ranges_valid_for_chunk(nullptr, &record),
        "DOD record range helper rejects null chunk");
}

void testVariableRecordSizes() {
  checkEq(expectedSetConstBytes(sizeof(float) * 4u, 3u),
          sizeof(D9CCommandRecordSetConst) + sizeof(float) * 12u,
          "float constant record byte size");
  checkEq(expectedSetConstBytes(sizeof(std::int32_t) * 4u, 2u),
          sizeof(D9CCommandRecordSetConst) + sizeof(std::int32_t) * 8u,
          "int constant record byte size");
  checkEq(expectedSetConstBytes(sizeof(std::uint32_t), 5u),
          sizeof(D9CCommandRecordSetConst) + sizeof(std::uint32_t) * 5u,
          "bool constant record byte size");
  checkEq(expectedClearBytes(0u), sizeof(D9CCommandRecordClear), "full-target clear size");
  checkEq(expectedClearBytes(2u), sizeof(D9CCommandRecordClear) + sizeof(D9CRect) * 2u,
          "rect clear size");
}

void testHandleKindCompatibility() {
  using dxmt9::core::ChunkHandleKind;
  checkEq(D9C_CHUNK_HANDLE_KIND_TEXTURE, static_cast<std::uint32_t>(ChunkHandleKind::Texture),
          "texture handle kind wire value");
  checkEq(D9C_CHUNK_HANDLE_KIND_SURFACE, static_cast<std::uint32_t>(ChunkHandleKind::Surface),
          "surface handle kind wire value");
  checkEq(D9C_CHUNK_HANDLE_KIND_BUFFER, static_cast<std::uint32_t>(ChunkHandleKind::Buffer),
          "buffer handle kind wire value");
  checkEq(D9C_CHUNK_HANDLE_KIND_SHADER, static_cast<std::uint32_t>(ChunkHandleKind::Shader),
          "shader handle kind wire value");
  checkEq(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, static_cast<std::uint32_t>(ChunkHandleKind::VertexDecl),
          "vertex declaration handle kind wire value");

  D9CCommandChunkWireHandleEntry entry{};
  entry.kind = D9C_CHUNK_HANDLE_KIND_SHADER;
  checkEq(entry.kind, static_cast<std::uint32_t>(ChunkHandleKind::Shader),
          "DOD handle entry uses legacy chunk handle kind mapping");
}

void testDrawPacketDeltaDefaults() {
  D9CDrawPrimitivePacket packet{};
  checkEq(packet.renderStateCount, 0u, "default packet has no render-state delta");
  checkEq(packet.textureMask, 0u, "default packet has no texture delta");
  checkEq(packet.streamSourceMask, 0u, "default packet has no stream delta");
  checkEq(packet.fvfValid, 0u, "default packet has no FVF delta");
  checkEq(packet.vsValid, 0u, "default packet has no VS delta");
  checkEq(packet.psValid, 0u, "default packet has no PS delta");
  checkEq(packet.rtMask, 0u, "default packet has no RT delta");
  checkEq(packet.dsValid, 0u, "default packet has no DS delta");
  checkEq(packet.tssCount, 0u, "default packet has no TSS delta");
  checkEq(packet.samplerStateCount, 0u, "default packet has no sampler delta");
  checkEq(packet.transformCount, 0u, "default packet has no transform delta");
}

}  // namespace

int main() {
  try {
    testWireRecordsStayPod();
    testCommandRecordIds();
    testRecordHeaderLayout();
    testDrawPacketLayouts();
    testCommandRecordLayouts();
    testDodWireChunkLayout();
    testDodWireDefaultsAndPayloadRanges();
    testDodWireHandleRangesAndRecordRanges();
    testVariableRecordSizes();
    testHandleKindCompatibility();
    testDrawPacketDeltaDefaults();
  } catch (const TestFailure& e) {
    std::cerr << "chunk_record_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
