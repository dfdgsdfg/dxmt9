#include "device_c_chunk_v2_schema.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace {

template <typename T>
constexpr bool isWirePod =
    std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

static_assert(isWirePod<D9CCommandChunkWireHeaderV2>);
static_assert(isWirePod<D9CCommandChunkWireRecordHeaderV2>);
static_assert(isWirePod<D9CCommandChunkWireHandleEntryV2>);
static_assert(isWirePod<D9CCommandChunkWireSectionDescV2>);
static_assert(isWirePod<D9CCommandChunkWireDrawHeaderV2>);
static_assert(isWirePod<D9CWireObjectIdentity>);
static_assert(isWirePod<D9CCommandChunkNegotiation>);

static_assert(sizeof(D9CCommandChunkWireHeaderV2) ==
              D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE);
static_assert(sizeof(D9CCommandChunkWireRecordHeaderV2) ==
              D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_V2_SIZE);
static_assert(sizeof(D9CCommandChunkWireHandleEntryV2) ==
              D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_V2_SIZE);
static_assert(sizeof(D9CCommandChunkWireSectionDescV2) ==
              D9C_COMMAND_CHUNK_WIRE_SECTION_DESC_V2_SIZE);
static_assert(sizeof(D9CCommandChunkWireDrawHeaderV2) ==
              D9C_COMMAND_CHUNK_WIRE_DRAW_HEADER_V2_SIZE);
static_assert(sizeof(D9CWireObjectIdentity) == 16u);
static_assert(alignof(D9CWireObjectIdentity) == 8u);
static_assert(sizeof(D9CCommandChunkNegotiation) == 32u);
static_assert(alignof(D9CCommandChunkNegotiation) == 4u);

static_assert(offsetof(D9CCommandChunkWireHeaderV2, recordTableOffset) == 16u);
static_assert(offsetof(D9CCommandChunkWireHeaderV2, handleTableOffset) == 24u);
static_assert(offsetof(D9CCommandChunkWireHeaderV2, payloadArenaOffset) == 32u);
static_assert(offsetof(D9CCommandChunkWireRecordHeaderV2, payloadOffset) == 8u);
static_assert(offsetof(D9CCommandChunkWireRecordHeaderV2, firstHandle) == 16u);
static_assert(offsetof(D9CCommandChunkWireHandleEntryV2, objectId) == 8u);
static_assert(offsetof(D9CCommandChunkWireSectionDescV2, count) == 4u);
static_assert(offsetof(D9CCommandChunkWireSectionDescV2, payloadOffset) == 8u);
static_assert(offsetof(D9CCommandChunkWireDrawHeaderV2, sectionCount) == 40u);
static_assert(offsetof(D9CCommandChunkWireDrawHeaderV2, sectionTableOffset) ==
              44u);
static_assert(offsetof(D9CCommandChunkWireDrawHeaderV2, sectionPayloadOffset) ==
              48u);

static_assert(sizeof(D9CCommandChunkWireTextureBindingV2) == 16u);
static_assert(sizeof(D9CCommandChunkWireStreamBindingV2) == 28u);
static_assert(sizeof(D9CCommandChunkWireShaderBindingV2) == 16u);
static_assert(sizeof(D9CCommandChunkWireVertexInputV2) == 16u);
static_assert(sizeof(D9CCommandChunkWireIndexBindingV2) == 8u);
static_assert(sizeof(D9CCommandChunkWireRenderTargetBindingV2) == 16u);
static_assert(sizeof(D9CCommandChunkWireDepthStencilBindingV2) == 8u);
static_assert(sizeof(D9CCommandChunkWireClipPlaneV2) == 24u);
static_assert(sizeof(D9CCommandChunkWireConstantRangeV2) == 8u);

static_assert(sizeof(D9CCommandChunkWireSetConstV2) == 8u);
static_assert(sizeof(D9CCommandChunkWireClearV2) == 24u);
static_assert(sizeof(D9CCommandChunkWirePresentV2) == 56u);
static_assert(sizeof(D9CCommandChunkWireStretchRectV2) == 56u);
static_assert(sizeof(D9CCommandChunkWireColorFillV2) == 32u);
static_assert(sizeof(D9CCommandChunkWireUpdateTextureV2) == 8u);
static_assert(sizeof(D9CCommandChunkWireUpdateSurfaceV2) == 48u);
static_assert(sizeof(D9CCommandChunkWireQueryIssueV2) == 8u);
static_assert(sizeof(D9CCommandChunkWireReadbackV2) == 8u);
static_assert(sizeof(D9CCommandChunkWireReszDepthResolveV2) == 8u);

}  // namespace

int main() {
  if (!dxmt9::d3d9::v2RecordSchemaComplete() ||
      !dxmt9::d3d9::v2SectionSchemaComplete()) {
    std::cerr << "chunk_record_v2_layout_spec: incomplete schema\n";
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_v2_layout_spec passed\n";
  return EXIT_SUCCESS;
}
