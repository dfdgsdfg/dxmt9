#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxmt9::core {

// Bump whenever the physical/provision/owner meaning of the schema changes.
// Admission witnesses carry this value so a cached proof cannot survive a
// dimension-inventory change without being rejected.
inline constexpr std::uint32_t kDirectChunkSlotSchemaRevision = 1;

enum class DirectChunkSlotPhysicalRole : std::uint8_t {
  Vector,
  SemanticOnly,
};

enum class DirectChunkSlotProvisionRole : std::uint8_t {
  Staged,
  CoverageOnly,
  SemanticOnly,
};

enum class DirectChunkSlotLookupRole : std::uint8_t {
  Ordinary,
  Head,
  Tail,
  Next,
};

enum class DirectChunkSlotOwnerRole : std::uint8_t {
  Inline,
  Detached,
};

struct DirectChunkSlotDimensionDescriptor {
  const char* name = nullptr;
  DirectChunkSlotPhysicalRole physical =
      DirectChunkSlotPhysicalRole::SemanticOnly;
  DirectChunkSlotProvisionRole provision =
      DirectChunkSlotProvisionRole::SemanticOnly;
  DirectChunkSlotLookupRole lookup = DirectChunkSlotLookupRole::Ordinary;
  DirectChunkSlotOwnerRole owner = DirectChunkSlotOwnerRole::Inline;
};

}  // namespace dxmt9::core

// One schema owns the Direct ChunkSlot dimension inventory. Keep consumers as
// macro expansions: reserve, coverage and swap paths are deliberately
// compile-time-unrolled and must not acquire a per-draw descriptor loop.
//
// X(region, plan member, ChunkSlot vector, element type, physical role,
//   provision role, allocation-fault name, lookup role, owner role)
#define DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(X)                              \
  X(CommandHeaders, commandHeaders, commandHeaders, MetalCommandHeader,    \
    Vector, Staged, CommandHeaders, Ordinary, Inline)                      \
  X(DrawHotStates, drawHotStates, drawHotStates, FlatDrawStateRecord,      \
    Vector, Staged, DrawHotStates, Ordinary, Inline)                       \
  X(DrawShaderLayouts, drawShaderLayouts, drawShaderLayouts,               \
    DrawShaderLayoutContext, Vector, Staged, DrawShaderLayouts, Ordinary,  \
    Detached)                                                              \
  X(DrawDebugSnapshots, drawDebugSnapshots, drawDebugSnapshots,            \
    DrawDebugSnapshot, Vector, Staged, DrawDebugSnapshots, Ordinary,       \
    Inline)                                                                \
  X(DrawPsoSubviews, drawPsoSubviews, drawPsoSubviews, DrawPsoSubview,      \
    Vector, Staged, DrawPsoSubviews, Ordinary, Inline)                     \
  X(DrawUniformFixedPayloads, drawUniformFixedPayloads,                    \
    drawUniformFixedPayloads, DrawUniformFixedPayloadRecord, Vector,       \
    Staged, DrawUniformFixedPayloads, Ordinary, Inline)                    \
  X(DrawUniformVertexConstants, drawUniformVertexConstants,                \
    drawUniformVertexConstants, DrawUniformVertexConstantsRecord, Vector,  \
    Staged, DrawUniformVertexConstants, Ordinary, Inline)                  \
  X(DrawUniformVertexConstantBytes, drawUniformVertexConstantBytes,        \
    drawUniformVertexConstantBytes, u8, Vector, Staged,                    \
    DrawUniformVertexConstantBytes, Ordinary, Inline)                      \
  X(DrawUniformPixelConstants, drawUniformPixelConstants,                  \
    drawUniformPixelConstants, DrawUniformPixelConstantsRecord, Vector,    \
    Staged, DrawUniformPixelConstants, Ordinary, Inline)                   \
  X(DrawUniformPixelConstantBytes, drawUniformPixelConstantBytes,          \
    drawUniformPixelConstantBytes, u8, Vector, Staged,                     \
    DrawUniformPixelConstantBytes, Ordinary, Inline)                       \
  X(DrawUniformPayloads, drawUniformPayloads, drawUniformPayloads,         \
    DrawUniformPayloadRecord, Vector, Staged, DrawUniformPayloads,         \
    Ordinary, Inline)                                                      \
  X(DrawUniformPayloadLookupHeads, drawUniformPayloadLookupHeads,          \
    drawUniformPayloadLookupHeads, std::uint32_t, Vector, Staged,          \
    PayloadLookupHeads, Head, Inline)                                      \
  X(DrawUniformPayloadLookupTails, drawUniformPayloadLookupTails,          \
    drawUniformPayloadLookupTails, std::uint32_t, Vector, Staged,          \
    PayloadLookupTails, Tail, Inline)                                      \
  X(DrawUniformPayloadLookupNext, drawUniformPayloadLookupNext,            \
    drawUniformPayloadLookupNext, std::uint32_t, Vector, Staged,           \
    PayloadLookupNext, Next, Inline)                                       \
  X(DrawUniformVertexConstantsLookupHeads,                                 \
    drawUniformVertexConstantsLookupHeads,                                 \
    drawUniformVertexConstantsLookupHeads, std::uint32_t, Vector, Staged,  \
    VertexLookupHeads, Head, Inline)                                       \
  X(DrawUniformVertexConstantsLookupTails,                                 \
    drawUniformVertexConstantsLookupTails,                                 \
    drawUniformVertexConstantsLookupTails, std::uint32_t, Vector, Staged,  \
    VertexLookupTails, Tail, Inline)                                       \
  X(DrawUniformVertexConstantsLookupNext,                                  \
    drawUniformVertexConstantsLookupNext,                                  \
    drawUniformVertexConstantsLookupNext, std::uint32_t, Vector, Staged,   \
    VertexLookupNext, Next, Inline)                                        \
  X(DrawUniformPixelConstantsLookupHeads,                                  \
    drawUniformPixelConstantsLookupHeads,                                  \
    drawUniformPixelConstantsLookupHeads, std::uint32_t, Vector, Staged,   \
    PixelLookupHeads, Head, Inline)                                        \
  X(DrawUniformPixelConstantsLookupTails,                                  \
    drawUniformPixelConstantsLookupTails,                                  \
    drawUniformPixelConstantsLookupTails, std::uint32_t, Vector, Staged,   \
    PixelLookupTails, Tail, Inline)                                        \
  X(DrawUniformPixelConstantsLookupNext,                                   \
    drawUniformPixelConstantsLookupNext,                                   \
    drawUniformPixelConstantsLookupNext, std::uint32_t, Vector, Staged,    \
    PixelLookupNext, Next, Inline)                                         \
  X(DrawParams, drawParams, drawParams, DrawParam, Vector, Staged,          \
    DrawParams, Ordinary, Inline)                                          \
  X(DrawPayloadBytes, drawPayloadBytes, drawPayloadArena, u8, Vector,       \
    Staged, DrawPayloadArena, Ordinary, Inline)                            \
  X(DrawRunRecords, drawRunRecords, drawRunRecords, DrawRunCommandRecord,   \
    Vector, Staged, DrawRunRecords, Ordinary, Inline)                      \
  X(ClearRecords, clearRecords, clearRecords, ClearDesc, Vector, Staged,    \
    ClearRecords, Ordinary, Inline)                                        \
  X(ClearRects, clearRects, clearRects, D3DRECT, SemanticOnly,              \
    SemanticOnly, None, Ordinary, Inline)                                  \
  X(SurfaceCopyRecords, surfaceCopyRecords, surfaceCopyRecords,             \
    SurfaceCopyDesc, Vector, Staged, SurfaceCopyRecords, Ordinary, Inline)  \
  X(StretchRectRecords, stretchRectRecords, stretchRectRecords,             \
    StretchRectDesc, Vector, Staged, StretchRectRecords, Ordinary, Inline)  \
  X(ReadbackRecords, readbackRecords, readbackRecords, ReadbackDesc,        \
    Vector, CoverageOnly, None, Ordinary, Inline)                          \
  X(ColorFillRecords, colorFillRecords, colorFillRecords, ColorFillDesc,    \
    Vector, Staged, ColorFillRecords, Ordinary, Inline)                    \
  X(DepthResolveRecords, depthResolveRecords, depthResolveRecords,          \
    DepthResolveDesc, Vector, Staged, DepthResolveRecords, Ordinary, Inline)\
  X(GenerateMipmapsRecords, generateMipmapsRecords,                        \
    generateMipmapsRecords, GenerateMipmapsDesc, Vector, Staged,           \
    GenerateMipmapsRecords, Ordinary, Inline)                              \
  X(PresentRecords, presentRecords, presentRecords, PresentCommandRecord,   \
    Vector, Staged, PresentRecords, Ordinary, Inline)

#define DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_Vector(...) __VA_ARGS__
#define DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_SemanticOnly(...)
#define DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PROVISION_Staged(...) __VA_ARGS__
#define DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PROVISION_CoverageOnly(...)
#define DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PROVISION_SemanticOnly(...)
#define DXMT9_DIRECT_CHUNK_SLOT_EXPAND_ORDINARY_Ordinary(...) __VA_ARGS__
#define DXMT9_DIRECT_CHUNK_SLOT_EXPAND_ORDINARY_Head(...)
#define DXMT9_DIRECT_CHUNK_SLOT_EXPAND_ORDINARY_Tail(...)
#define DXMT9_DIRECT_CHUNK_SLOT_EXPAND_ORDINARY_Next(...)
