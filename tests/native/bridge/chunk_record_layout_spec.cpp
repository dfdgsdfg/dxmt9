#include "device_c_chunk_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace {

template <typename T>
constexpr bool isWirePod =
    std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

static_assert(isWirePod<D9CCommandChunkWireHeader>);
static_assert(isWirePod<D9CCommandChunkWireRecordHeader>);
static_assert(isWirePod<D9CCommandChunkWireHandleEntry>);
static_assert(isWirePod<D9CCommandChunkWireSectionDesc>);
static_assert(isWirePod<D9CCommandChunkWireDrawHeader>);
static_assert(isWirePod<D9CCommandChunk>);
static_assert(isWirePod<D9CWireObjectIdentity>);
static_assert(isWirePod<D9CCommandChunkNegotiation>);
static_assert(D9C_COMMAND_CHUNK_VERSION == 2u);
static_assert(D9C_COMMAND_CHUNK_WIRE_VERSION == 2u);
static_assert(D9C_COMMAND_CHUNK_CAP_CURRENT == 0x00000002u);

static_assert(sizeof(D9CCommandChunkWireHeader) ==
              D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE);
static_assert(sizeof(D9CCommandChunkWireRecordHeader) ==
              D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE);
static_assert(sizeof(D9CCommandChunkWireHandleEntry) ==
              D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE);
static_assert(sizeof(D9CCommandChunkWireSectionDesc) ==
              D9C_COMMAND_CHUNK_WIRE_SECTION_DESC_SIZE);
static_assert(sizeof(D9CCommandChunkWireDrawHeader) ==
              D9C_COMMAND_CHUNK_WIRE_DRAW_HEADER_SIZE);
static_assert(sizeof(D9CWireObjectIdentity) == 16u);
static_assert(alignof(D9CWireObjectIdentity) == 8u);
static_assert(sizeof(D9CCommandChunkNegotiation) == 32u);
static_assert(alignof(D9CCommandChunkNegotiation) == 4u);
static_assert(sizeof(D9CCommandChunk) == 48u);
static_assert(alignof(D9CCommandChunk) == 8u);
static_assert(offsetof(D9CCommandChunk, records) == 12u);
static_assert(offsetof(D9CCommandChunk, handles) == 24u);
static_assert(offsetof(D9CCommandChunk, renderTapeCaptureToken) == 32u);
static_assert(offsetof(D9CCommandChunk, renderTapeEventOrdinal) == 40u);

static_assert(offsetof(D9CCommandChunkWireHeader, recordTableOffset) == 16u);
static_assert(offsetof(D9CCommandChunkWireHeader, handleTableOffset) == 24u);
static_assert(offsetof(D9CCommandChunkWireHeader, payloadArenaOffset) == 32u);
static_assert(offsetof(D9CCommandChunkWireRecordHeader, payloadOffset) == 8u);
static_assert(offsetof(D9CCommandChunkWireRecordHeader, firstHandle) == 16u);
static_assert(offsetof(D9CCommandChunkWireHandleEntry, objectId) == 8u);
static_assert(offsetof(D9CCommandChunkWireSectionDesc, count) == 4u);
static_assert(offsetof(D9CCommandChunkWireSectionDesc, payloadOffset) == 8u);
static_assert(offsetof(D9CCommandChunkWireDrawHeader, sectionCount) == 40u);
static_assert(offsetof(D9CCommandChunkWireDrawHeader, sectionTableOffset) ==
              44u);
static_assert(offsetof(D9CCommandChunkWireDrawHeader, sectionPayloadOffset) ==
              48u);

static_assert(sizeof(D9CCommandChunkWireTextureBinding) == 16u);
static_assert(sizeof(D9CCommandChunkWireStreamBinding) == 28u);
static_assert(sizeof(D9CCommandChunkWireShaderBinding) == 16u);
static_assert(sizeof(D9CCommandChunkWireVertexInput) == 16u);
static_assert(sizeof(D9CCommandChunkWireIndexBinding) == 8u);
static_assert(sizeof(D9CCommandChunkWireRenderTargetBinding) == 16u);
static_assert(sizeof(D9CCommandChunkWireDepthStencilBinding) == 8u);
static_assert(sizeof(D9CCommandChunkWireClipPlane) == 24u);
static_assert(sizeof(D9CCommandChunkWireConstantRange) == 8u);

static_assert(sizeof(D9CCommandChunkWireSetConst) == 8u);
static_assert(sizeof(D9CCommandChunkWireClear) == 24u);
static_assert(sizeof(D9CCommandChunkWirePresent) == 56u);
static_assert(sizeof(D9CCommandChunkWireStretchRect) == 56u);
static_assert(sizeof(D9CCommandChunkWireColorFill) == 32u);
static_assert(sizeof(D9CCommandChunkWireUpdateTexture) == 8u);
static_assert(sizeof(D9CCommandChunkWireUpdateSurface) == 48u);
static_assert(sizeof(D9CCommandChunkWireQueryIssue) == 8u);
static_assert(sizeof(D9CCommandChunkWireReadback) == 8u);
static_assert(sizeof(D9CCommandChunkWireReszDepthResolve) == 8u);

}  // namespace

int main() {
  if (!dxmt9::d3d9::recordSchemaComplete() ||
      !dxmt9::d3d9::sectionSchemaComplete()) {
    std::cerr << "chunk_record_layout_spec: incomplete schema\n";
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_layout_spec passed\n";
  return EXIT_SUCCESS;
}
